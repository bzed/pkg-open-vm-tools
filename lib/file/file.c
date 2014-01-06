/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. 
 * **********************************************************
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 */

/*
 * file.c --
 *
 *        Interface to host file system.  See also filePosix.c,
 *        fileWin32.c, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include "safetime.h"
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>

#include "vmware.h"
#include "util.h"
#include "str.h"
#include "msg.h"
#include "uuid.h"
#include "config.h"
#include "file.h"
#include "fileInt.h"
#include "stats_file.h"
#include "dynbuf.h"
#include "base64.h"
#include "timeutil.h"
#include "hostinfo.h"
#if !defined(N_PLAT_NLM)
#include "vm_atomic.h"
#endif

#define SETUP_DEFINE_VARS
#include "stats_user_setup.h"

#if !defined(O_BINARY)
#define O_BINARY 0
#endif

/*
 *----------------------------------------------------------------------
 *
 * File_Exists --
 *
 *      Check if a file exists.
 *
 * Results:
 *      TRUE if it exists
 *      FALSE if it doesn't exist
 *      
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_Exists(const char *name)   // IN
{
   return FileIO_Access(name, FILEIO_ACCESS_EXISTS) == FILEIO_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * File_UnlinkIfExists --
 *
 *      If the given file exists, unlink it. 
 *
 * Results:
 *      Return 0 if the unlink is successful or if the file did not exist.   
 *      Otherwise return -1.
 *
 * Side effects:
 *      May unlink the file.
 *      
 *----------------------------------------------------------------------
 */

int
File_UnlinkIfExists(const char *name)   // IN
{
   int ret = File_Unlink(name);
   if (ret < 0 && errno == ENOENT) {
      ret = 0;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * FileGetType --
 *
 *      Get the file type as returned by stat() in st_mode field.
 *
 * Bugs:
 *      Fails for the root directory of any drive on Windows, because
 *      stat() fails.
 *
 * Results:
 *      Bit mask representing the file type (S_IFDIR, S_IFREG, etc.)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static unsigned int
FileGetType(const char *name)      // IN
{
   int err;
   struct stat st;

   ASSERT(name);

   err = stat(name, &st);
   if (err < 0) {
      return FALSE;
   }
   return (st.st_mode & S_IFMT);
}


/*
 *----------------------------------------------------------------------
 *
 * File_IsDirectory --
 *
 *      Check if specified file is a directory or not.
 *
 * Bugs:
 *      Fails for the root directory of any drive on Windows.
 *
 * Results:
 *      Bool - TRUE -> is a directory, FALSE -> not a directory or error
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsDirectory(const char *name)      // IN
{
   return FileGetType(name) == S_IFDIR;
}


#if !defined(N_PLAT_NLM)
/*
 *----------------------------------------------------------------------
 *
 * GetOldMachineID --
 *
 *      Return the old machineID, the one based on Hostinfo_MachineID.
 *
 * Results:
 *      The machineID is returned.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static const char *
GetOldMachineID(void)
{
   static Atomic_Ptr atomic; /* Implicitly initialized to NULL. --mbellon */
   const char        *machineID;

   machineID = Atomic_ReadPtr(&atomic);

   if (machineID == NULL) {
      char *p;
      uint32 hashValue;
      uint64 hardwareID;
      char encodedMachineID[16 + 1];
      char rawMachineID[sizeof hashValue + sizeof hardwareID];

      Hostinfo_MachineID(&hashValue, &hardwareID);

      // Build the raw machineID
      memcpy(rawMachineID, &hashValue, sizeof hashValue);
      memcpy(&rawMachineID[sizeof hashValue], &hardwareID,
             sizeof hardwareID);

      // Base 64 encode the binary data to obtain printable characters
      Base64_Encode(rawMachineID, sizeof rawMachineID, encodedMachineID,
                    sizeof encodedMachineID, NULL);

      // remove any '/' from the encoding; no problem using it for a file name
      for (p = encodedMachineID; *p; p++) {
         if (*p == '/') {
            *p = '-';
         }
      }

      p = Util_SafeStrdup(encodedMachineID);

      if (Atomic_ReadIfEqualWritePtr(&atomic, NULL, p)) {
         free(p);
      }

      machineID = Atomic_ReadPtr(&atomic);
      ASSERT(machineID);
   }

   return machineID;
}


/*
 *----------------------------------------------------------------------
 *
 * FileLockGetMachineID --
 *
 *      Return the machineID, a "universally unique" identification of
 *      of the system that calls this routine.
 *
 *      An attempt is first made to use the host machine's UUID. If that
 *      fails drop back to the older machineID method.
 *
 * Results:
 *      The machineID is returned.
 *
 * Side effects:
 *      Memory allocated for the machineID is never freed, however the
 *      memory is cached - there is no memory leak.
 *
 *----------------------------------------------------------------------
 */

const char *
FileLockGetMachineID(void)
{
   static Atomic_Ptr atomic; /* Implicitly initialized to NULL. --mbellon */
   const char        *machineID;

   machineID = Atomic_ReadPtr(&atomic);

   if (machineID == NULL) {
      char *p;
      char *q;

      /*
       * UUID_GetHostRealUUID is fine on Windows.
       *
       * UUID_GetHostUUID is fine on Macs because the UUID can't be found
       * in /dev/mem even if it can be accessed. Macs always use the MAC
       * address from en0 to provide a UUID.
       *
       * UUID_GetHostUUID is problematic on Linux so it is not acceptable for
       * locking purposes - it accesses /dev/mem to obtain the SMBIOS UUID
       * and that can fail when the calling process is not priviledged.
       *
       */

#if defined(_WIN32)
      q = UUID_GetRealHostUUID();
#elif defined(__APPLE__) || defined(VMX86_SERVER)
      q = UUID_GetHostUUID();
#else
      q = NULL;
#endif

      if (q == NULL) {
         p = (char *) GetOldMachineID();
      } else {
         p = Str_Asprintf(NULL, "uuid=%s", q);
         free(q);

         /* Surpress any whitespace. */
         for (q = p; *q; q++) {
            if (isspace((int) *q)) {
               *q = '-';
            }
         }
      }

      if (Atomic_ReadIfEqualWritePtr(&atomic, NULL, p)) {
         free(p);
      }

      machineID = Atomic_ReadPtr(&atomic);
      ASSERT(machineID);
   }

   return machineID;
}


/*
 *-----------------------------------------------------------------------------
 *
 * OldMachineIDMatch --
 *
 *	Do the old-style MachineIDs match?
 *
 * Results:
 *      TRUE     Yes
 *      FALSE    No
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
OldMachineIDMatch(const char *first,  // IN:
                  const char *second) // IN:
{
#if defined(__APPLE__) || defined(linux)
   /* Ignore the host name hash */
   char *p;
   char *q;
   size_t len;
   Bool result;
   uint8 rawMachineID_1[12];
   uint8 rawMachineID_2[12];

   for (p = Util_SafeStrdup(first), q = p; *p; p++) {
      if (*p == '-') {
         *p = '/';
      }
   }
   result = Base64_Decode(q, rawMachineID_1, sizeof rawMachineID_1, &len);
   free(q);

   if ((result == FALSE) || (len != 12)) {
      Warning("%s: unexpected decode problem #1 (%s)\n", __FUNCTION__,
              first);
      return FALSE;
   }

   for (p = Util_SafeStrdup(second), q = p; *p; p++) {
      if (*p == '-') {
         *p = '/';
      }
   }
   result = Base64_Decode(q, rawMachineID_2, sizeof rawMachineID_2, &len);
   free(q);

   if ((result == FALSE) || (len != 12)) {
      Warning("%s: unexpected decode problem #2 (%s)\n", __FUNCTION__,
              second);
      return FALSE;
   }

   return memcmp(&rawMachineID_1[4],
                 &rawMachineID_2[4], 8) == 0 ? TRUE : FALSE;
#else
   return strcmp(first, second) == 0 ? TRUE : FALSE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileLockMachineIDMatch --
 *
 *	Do the MachineIDs match?
 *
 * Results:
 *      TRUE     Yes
 *      FALSE    No
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
FileLockMachineIDMatch(char *hostMachineID,  // IN:
                       char *otherMachineID) // IN:
{
   if (strncmp(hostMachineID, "uuid=", 5) == 0) {
      if (strncmp(otherMachineID, "uuid=", 5) == 0) {
         return strcmp(hostMachineID + 5,
                       otherMachineID + 5) == 0 ? TRUE : FALSE;
      } else {
         return OldMachineIDMatch(GetOldMachineID(), otherMachineID);
      }
   } else {
      if (strncmp(otherMachineID, "uuid=", 5) == 0) {
         return FALSE;
      } else {
         return strcmp(hostMachineID, otherMachineID) == 0 ? TRUE : FALSE;
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * File_IsEmptyDirectory --
 *
 *      Check if specified file is a directory and contains no files.
 *
 * Results:
 *      Bool - TRUE -> is an empty directory, FALSE -> not an empty directory
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

Bool
File_IsEmptyDirectory(const char *name)  // IN
{
   int numFiles;

   if (!File_IsDirectory(name)) {
      return FALSE;
   }

   numFiles = File_ListDirectory(name, NULL);
   if (numFiles < 0) {
      return FALSE;
   }

   return numFiles == 0;
}
#endif /* N_PLAT_NLM */


/*
 *----------------------------------------------------------------------
 *
 * File_IsFile --
 *
 *      Check if specified file is a regular file.
 *
 * Results:
 *      Bool - TRUE -> is a regular file, FALSE -> not a regular file or error.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsFile(const char *name)      // IN
{
   return FileGetType(name) == S_IFREG;
}


/*
 *----------------------------------------------------------------------
 *
 * FileFindFirstSlash --
 *
 *      Finds the first pathname slash in a path (both slashes count for
 *      Win32, only forward slash for Unix).
 *
 *----------------------------------------------------------------------
 */

static char *
FileFindFirstSlash(const char *path) // IN
{
   char *firstFS = Str_Strchr(path, '/');
#if defined(_WIN32)
   char *firstBS = Str_Strchr(path, '\\');

   if (firstFS && firstBS) {
      return MIN(firstFS, firstBS);
   } else if (firstFS) {
      return firstFS;
   } else {
      return firstBS;
   }
#else
   return firstFS;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * File_FindLastSlash --
 *
 *      Finds the last pathname slash in a path (both slashes count for
 *      Win32, only forward slash for Unix).
 *
 *----------------------------------------------------------------------
 */

char *
File_FindLastSlash(const char *path) // IN
{
   char *lastFS = Str_Strrchr(path, '/');
#if defined(_WIN32)
   char *lastBS = Str_Strrchr(path, '\\');

   if (lastFS && lastBS) {
      return MAX(lastFS, lastBS);
   } else if (lastFS) {
      return lastFS;
   } else {
      return lastBS;
   }
#else
   return lastFS;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * File_SplitName --
 *
 *      Split a file name into three components: VOLUME, DIRECTORY,
 *      BASE.  The return values must be freed.
 *
 *      VOLUME is empty for an empty string or a UNIX-style path, the
 *      drive letter and colon for a Win32 drive-letter path, or the
 *      construction "\\server\share" for a Win32 UNC path.
 *
 *      BASE is the longest string at the end that begins after the
 *      volume string and after the last directory separator.
 *
 *      DIRECTORY is everything in-between VOLUME and BASE.
 *
 *      The concatenation of VOLUME, DIRECTORY, and BASE produces the
 *      original string, so any of those strings may be empty.
 *
 *      A NULL pointer may be passed for one or more OUT parameters, in
 *      which case that parameter is not returned.
 *
 *      Able to handle both UNC and drive-letter paths on Windows.
 *
 * Results:
 *      As described.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
File_SplitName(const char *pathname,     // IN
               char **volume,            // OUT (OPT)
               char **directory,         // OUT (OPT)
               char **base)              // OUT (OPT)
{
   char *vol;
   char *dir;
   char *bas;
   char *basebegin;
   char *volend;
   int vollen, dirlen;
   size_t len;

   ASSERT(pathname);
   len = strlen(pathname);

   /*
    * Get volume.
    */

   volend = (char *) pathname;

#if defined(_WIN32)
   if ((len > 2) &&
       (!Str_Strncmp("\\\\", pathname, 2) ||
        !Str_Strncmp("//", pathname, 2))) {
      /* UNC path */
      volend = FileFindFirstSlash(volend + 2);

      if (volend) {
         volend = FileFindFirstSlash(volend + 1);

         if (!volend) {
            /* we have \\foo\bar, which is legal */
            volend = (char *) pathname + len;
         }

      } else {
         /* we have \\foo, which is just bogus */
         volend = (char *) pathname;
      }
   } else if ((len >= 2) && (':' == pathname[1])) {
      // drive-letter path
      volend = (char *) pathname + 2;
   }
#endif /* _WIN32 */

   vollen = volend - pathname;
   vol = Util_SafeMalloc(vollen + 1);
   memcpy(vol, pathname, vollen);
   vol[vollen] = 0;

   /*
    * Get base.
    */

   basebegin = File_FindLastSlash(pathname);
   basebegin = (basebegin ? basebegin + 1 : (char *) pathname);

   if (basebegin < volend) {
      basebegin = (char *) pathname + len;
   }

   bas = Util_SafeStrdup(basebegin);

   /*
    * Get dir.
    */

   dirlen = basebegin - volend;
   dir = Util_SafeMalloc(dirlen + 1);
   memcpy(dir, volend, dirlen);
   dir[dirlen] = 0;
 
   /*
    * Return what needs to be returned.
    */

   if (volume) {
      *volume = vol;
   } else {
      free(vol);
   }

   if (directory) {
      *directory = dir;
   } else {
      free(dir);
   }

   if (base) {
      *base = bas;
   } else {
      free(bas);
   }
}


/*
 *---------------------------------------------------------------------------
 *
 * File_GetPathName --
 *
 *      Behaves like File_SplitName by splitting the fullpath into
 *      pathname & filename components.
 *
 *      The trailing directory separator [\|/] is stripped off the
 *      pathname component. This in turn means that on Linux the root
 *      directory will be returned as the empty string "". On Windows
 *      it will be returned as X: where X is the drive letter. It is
 *      important that callers of this functions are aware that the ""
 *      on Linux means root "/".
 *
 *      A NULL pointer may be passed for one or more OUT parameters,
 *      in which case that parameter is not returned.
 *
 * Results: 
 *      As described.
 *
 * Side effects: 
 *      The return values must be freed.
 *
 *---------------------------------------------------------------------------
 */

void 
File_GetPathName(const char *fullpath,    // IN
                 char **pathname,         // OUT (OPT)
                 char **base)             // OUT (OPT)
{
   char *volume = NULL;
   char *p;

   File_SplitName(fullpath, &volume, pathname, base);
   if (!pathname) {
      free(volume);
      return;
   }

   /*
    * volume component may be empty.
    */
   if (volume) {
      if (volume[0] != '\0') {
         char *volPathname = Str_Asprintf(NULL, "%s%s", volume, *pathname);
         free(*pathname);
         *pathname = volPathname;
      }
      free(volume);
   }


   /*
    * Check for a trailing directory separator, remove it.
    */
   p = Str_Strrchr(*pathname, DIRSEPC);
   if (p != NULL) {
      if (p == (*pathname + strlen(*pathname) - 1)) {
         *p = '\0';
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 *  File_MakeTempEx --
 *
 *      Create a temporary file and, if successful, return an open file
 *      descriptor to that file.
 *
 *      'dir' specifies the directory in which to create the file. It
 *      must not end in a slash.
 *
 *      'fileName' specifies the base filename of the created file.
 *
 * Results:
 *      Open file descriptor or -1; if successful then filename points
 *      to a dynamically allocated string with the pathname of the temp
 *      file.
 *
 * Side effects:
 *      Creates a file if successful.
 *
 *----------------------------------------------------------------------
 */

int
File_MakeTempEx(const char *dir,       // IN
                const char *fileName,  // IN
                char **presult)        // OUT
{
   char *basePath = NULL, *path = NULL;
   unsigned int var;
   int fd = -1;

   *presult = NULL;

   /* construct base full pathname to use */
   basePath = Str_Asprintf(NULL, "%s"DIRSEPS"%s", dir, fileName);

   for (var = 0; var <= 0xFFFFFFFF; var++) {
      /* construct suffixed pathname to use */
      free(path);
      path = Str_Asprintf(NULL, "%s%d", basePath, var);

      fd = PosixFileOpener(path,
                           O_CREAT | O_EXCL | O_BINARY | O_RDWR,
                           0600);

      if (fd >= 0) {
         *presult = path;
         path = NULL;
         break;
      }

      if (errno != EEXIST) {
         Msg_Append(MSGID(file.maketemp.openFailed)
                 "Failed to create temporary file \"%s\": %s.\n",
                 path, Msg_ErrString());
         goto exit;
      }
   }

   if (-1 == fd) {
      Msg_Append(MSGID(file.maketemp.fullNamespace)
                 "Failed to create temporary file \"%s\": The name space is "
                 "full.\n", path);
   }

  exit:
   free(basePath);
   free(path);

   return fd;
}


/*
 *----------------------------------------------------------------------
 *
 *  File_MakeTemp --
 *
 *      Create a temporary file and, if successful, return an open file
 *      descriptor to the file.
 *
 *      'tag' can either be a full pathname, a string, or NULL.
 *
 *      If 'tag' is a full pathname, that path will be used as the root
 *      path for the file.
 *
 *      If 'tag' is a string, the created file's filename will begin
 *      with 'tag' and will be created in the default temp directory.
 *
 *      If 'tag' is NULL, then 'tag' is assumed to be "vmware" and the
 *      above case applies.
 *
 *      This API is technically unsafe if you allow this function to use
 *      the default temp directory since it's not guaranteed on Windows
 *      that when the file is closed it is not readable by other users
 *      (no matter what we specify as the mode to open, the new file
 *      will inherit DACLs from the parent, and certain temp directories
 *      on Windows give all Power Users read&write access). Please use
 *      Util_MakeSafeTemp if your dependencies permit it.
 *
 * Results:
 *      Open file descriptor or -1; if successful then filename points
 *      to a dynamically allocated string with the pathname of the temp
 *      file.
 *
 * Side effects:
 *      Creates a file if successful.
 *
 *----------------------------------------------------------------------
 */

int
File_MakeTemp(const char *tag,  // IN (OPT)
              char **presult)   // OUT
{
   char *dir = NULL, *fileName = NULL;
   int fd = -1;

   *presult = NULL;

   if (tag && File_IsFullPath(tag)) {
      char *lastSlash;

      dir = Util_SafeStrdup(tag);
      lastSlash = Str_Strrchr(dir, DIRSEPC);
      ASSERT(lastSlash);

      fileName = Util_SafeStrdup(lastSlash + 1);
      *lastSlash = 0;
   } else {
      dir = File_GetTmpDir(TRUE);

      if (!dir) {
         goto exit;
      }

      fileName = Util_SafeStrdup(tag ? tag : "vmware");
   }

   fd = File_MakeTempEx(dir, fileName, presult);

  exit:
   free(dir);
   free(fileName);

   return fd;
}


/*
 *----------------------------------------------------------------------
 *
 * File_CopyFromFdToFd --
 *
 *      Write all data between the current position in the 'src' file and the
 *      end of the 'src' file to the current position in the 'dst' file
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure
 *
 * Side effects:
 *      The current position in the 'src' file and the 'dst' file are modified
 *
 *----------------------------------------------------------------------
 */

Bool
File_CopyFromFdToFd(FileIODescriptor src,       // IN
                    FileIODescriptor dst)       // IN
{
   FileIOResult fretR;

   do {
      unsigned char buf[1024];
      size_t actual;
      FileIOResult fretW;

      fretR = FileIO_Read(&src, buf, sizeof(buf), &actual);
      if (fretR != FILEIO_SUCCESS &&
          fretR != FILEIO_READ_ERROR_EOF) {
         Msg_Append(MSGID(File.CopyFromFdToFd.read.failure) "Read error: %s.\n\n",
                    FileIO_MsgError(fretR));
         return FALSE;
      }

      fretW = FileIO_Write(&dst, buf, actual, NULL);
      if (fretW != FILEIO_SUCCESS) {
         Msg_Append(MSGID(File.CopyFromFdToFd.write.failure) "Write error: %s.\n\n",
                    FileIO_MsgError(fretW));
         return FALSE;
      }
   } while (fretR != FILEIO_READ_ERROR_EOF);

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * File_CopyFromFdToName --
 *
 *      Copy the 'src' file to 'dstName'.
 *      If the 'dstName' file already exists,
 *      If 'dstDispose' is -1, the user is prompted for proper action.
 *      If 'dstDispose' is 0, retry until success (dangerous).
 *      If 'dstDispose' is 1, overwrite the file.
 *      If 'dstDispose' is 2, return the error.
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure: if the user cancelled the operation, no message is
 *                        appended. Otherwise messages are appended
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_CopyFromFdToName(FileIODescriptor src,     // IN
                      const char *dstName,      // IN
                      int dstDispose)           // IN
{
   FileIODescriptor dst;
   FileIOResult fret;
   Bool result;
   int ret;
   FileIO_Invalidate(&dst);

   fret = File_CreatePrompt(&dst, dstName, 0, dstDispose);
   if (fret != FILEIO_SUCCESS) {
      if (fret != FILEIO_CANCELLED) {
         Msg_Append(MSGID(File.CopyFromFdToName.create.failure)
                    "Unable to create a new '%s' file: %s.\n\n", dstName,
                    FileIO_MsgError(fret));
      }

      return FALSE;
   }

   result = File_CopyFromFdToFd(src, dst);

   ret = FileIO_Close(&dst);
   if (ret) {
      Msg_Append(MSGID(File.CopyFromFdToName.close.failure)
                 "Unable to close the '%s' file: %s.\n"
                 "\n",
                 dstName,
                 Msg_ErrString());
      result = FALSE;
   }

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * File_CreatePrompt --
 *
 *      Create the 'name' file for write access or 'access' access.
 *      If the 'name' file already exists,
 *      If 'prompt' is not -1, it is the automatic answer to the question that
 *      would be asked to the user if it was -1.
 *
 * Results:
 *      FILEIO_CANCELLED if the operation was cancelled by the user, otherwise
 *      as FileIO_Open()
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
File_CreatePrompt(FileIODescriptor *file,       // OUT
                  const char *name,             // IN
                  int access,                   // IN
                  int prompt)                   // IN
{
   FileIOOpenAction action;
   FileIOResult fret;
   
   action = FILEIO_OPEN_CREATE_SAFE;
   while ((fret = FileIO_Open(file, name, FILEIO_OPEN_ACCESS_WRITE | access,
                             action)) == FILEIO_OPEN_ERROR_EXIST) {
      static Msg_String const buttons[] = {
         {BUTTONID(file.create.retry) "Retry"},
         {BUTTONID(file.create.overwrite) "Overwrite"},
         {BUTTONID(cancel) "Cancel"},
         {NULL}
      };
      int answer;

      answer = (prompt != -1) ? prompt : Msg_Question(buttons, 2,
         MSGID(File.CreatePrompt.question)
         "The file '%s' already exists.\n"
         "To overwrite the content of the file, select Overwrite.\n"
         "To retry the operation after you have moved the file "
         "to another location, select Retry.\n"
         "To cancel the operation, select Cancel.\n",
         name);
      if (answer == 2) {
         fret = FILEIO_CANCELLED;
         break;
      }
      if (answer == 1) {
         action = FILEIO_OPEN_CREATE_EMPTY;
      }
   }

   return fret;
}


/*
 *----------------------------------------------------------------------
 *
 * File_CopyFromNameToName --
 *
 *      Copy the 'srcName' file to 'dstName'.
 *      If 'srcName' doesn't exist, an error is reported
 *      If the 'dstName' file already exists,
 *      If 'dstDispose' is -1, the user is prompted for proper action.
 *      If 'dstDispose' is 0, retry until success (dangerous).
 *      If 'dstDispose' is 1, overwrite the file.
 *      If 'dstDispose' is 2, return the error.
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure: if the user cancelled the operation, no message is
 *                        appended. Otherwise messages are appended
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_CopyFromNameToName(const char *srcName, // IN
                        const char *dstName, // IN
                        int dstDispose)      // IN
{
   FileIODescriptor src;
   FileIOResult fret;
   Bool result;
   int ret;
   FileIO_Invalidate(&src);

   fret = FileIO_Open(&src, srcName, FILEIO_OPEN_ACCESS_READ, FILEIO_OPEN);
   if (fret != FILEIO_SUCCESS) {
      Msg_Append(MSGID(File.CopyFromNameToName.open.failure)
                 "Unable to open the '%s' file for read access: %s.\n\n", srcName,
                 FileIO_MsgError(fret));
      return FALSE;
   }

   result = File_CopyFromFdToName(src, dstName, dstDispose);
   
   ret = FileIO_Close(&src);
   if (ret) {
      Msg_Append(MSGID(File.CopyFromNameToName.close.failure)
                 "Unable to close the '%s' file: %s.\n"
                 "\n",
                 srcName,
                 Msg_ErrString());
      result = FALSE;
   }

   return result;
}

/*
 *----------------------------------------------------------------------
 *
 * File_CopyFromFd --
 *
 *      Copy the 'src' file to 'dstName'.
 *      If the 'dstName' file already exists, 'overwriteExisting'
 *      decides whether to overwrite the existing file or not.
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure: Messages are appended
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_CopyFromFd(FileIODescriptor src,     // IN
                const char *dstName,      // IN
                Bool overwriteExisting)   // IN
{
   FileIODescriptor dst;
   FileIOOpenAction action;
   FileIOResult fret;
   Bool result;
   int ret;
   FileIO_Invalidate(&dst);

   action = overwriteExisting ? FILEIO_OPEN_CREATE_EMPTY :
                                FILEIO_OPEN_CREATE_SAFE;

   fret = FileIO_Open(&dst, dstName, FILEIO_OPEN_ACCESS_WRITE, action);
   if (fret != FILEIO_SUCCESS) {
      Msg_Append(MSGID(File.CopyFromFdToName.create.failure)
                 "Unable to create a new '%s' file: %s.\n\n", dstName,
                 FileIO_MsgError(fret));
      return FALSE;
   }

   result = File_CopyFromFdToFd(src, dst);

   ret = FileIO_Close(&dst);
   if (ret) {
      Msg_Append(MSGID(File.CopyFromFdToName.close.failure)
                 "Unable to close the '%s' file: %s.\n"
                 "\n",
                 dstName,
                 Msg_ErrString());
      result = FALSE;
   }

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * File_Copy --
 *
 *      Copy the 'srcName' file to 'dstName'.
 *      If 'srcName' doesn't exist, an error is reported
 *      If the 'dstName' file already exists, 'overwriteExisting'
 *      decides whether to overwrite the existing file or not.
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure: Messages are appended
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_Copy(const char *srcName,    // IN
          const char *dstName,    // IN
          Bool overwriteExisting) // IN
{
   FileIODescriptor src;
   FileIOResult fret;
   Bool result;
   int ret;
   FileIO_Invalidate(&src);

   fret = FileIO_Open(&src, srcName, FILEIO_OPEN_ACCESS_READ, FILEIO_OPEN);
   if (fret != FILEIO_SUCCESS) {
      Msg_Append(MSGID(File.Copy.open.failure)
                 "Unable to open the '%s' file for read access: %s.\n\n", srcName,
                 FileIO_MsgError(fret));
      return FALSE;
   }

   result = File_CopyFromFd(src, dstName, overwriteExisting);
   
   ret = FileIO_Close(&src);
   if (ret) {
      Msg_Append(MSGID(File.Copy.close.failure)
                 "Unable to close the '%s' file: %s.\n"
                 "\n",
                 srcName,
                 Msg_ErrString());
      result = FALSE;
   }

   return result;
}

/*
 *----------------------------------------------------------------------
 *
 * File_Rename --
 *
 *      Renames a source to a destination file.
 *      Will copy the file if necessary
 *
 * Results:
 *      TRUE if succeeded FALSE otherwise
 *      
 * Side effects:
 *      src file is no more, but dst file exists
 *
 *----------------------------------------------------------------------
 */

Bool 
File_Rename(const char *src, const char *dst)
{
   Bool ret = TRUE;

   if (rename(src, dst) < 0) {
      /* overwrite the file if it exists */
      if (File_Copy(src, dst, TRUE)) {
         File_Unlink(src);
      } else {
         ret = FALSE;
      }
   }

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * File_GetModTimeString --
 *
 *      Returns a human-readable string denoting the last modification 
 *      time of a file.
 *      ctime() returns string terminated with newline, which we replace
 *      with a '\0'.
 *
 * Results:
 *      Last modification time string on success, NULL on error.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
File_GetModTimeString(const char *fileName)   // IN
{
   int64 modTime = File_GetModTime(fileName);
   if (modTime == -1) {
      return NULL;
   } else {
      return TimeUtil_GetTimeFormat(modTime, TRUE, TRUE);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * File_GetSize --
 *
 *      Get size of file.
 *
 * Results:
 *      Size of file or -1.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

int64
File_GetSize(const char *name)
{
   int64 ret = -1;

   if (name) {
      FileIODescriptor fd;
      FileIOResult res;

      FileIO_Invalidate(&fd);
      res = FileIO_Open(&fd, name, FILEIO_OPEN_ACCESS_READ, FILEIO_OPEN);

      if (res == FILEIO_SUCCESS) {
         ret = FileIO_GetSize(&fd);
         FileIO_Close(&fd);
      }
   }
   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * File_SupportsLargeFiles --
 *
 *      Check if the given file is on an FS that supports 4GB files.
 *      Require 4GB support so we rule out FAT filesystems, which
 *      support 4GB-1 on both Linux and Windows.
 *
 * Results:
 *      TRUE if FS supports files over 4GB.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_SupportsLargeFiles(const char *filePath) // IN
{
   return File_SupportsFileSize(filePath, CONST64U(0x100000000));
}


#if !defined(N_PLAT_NLM)
/*
 *----------------------------------------------------------------------------
 *
 * File_GetSizeByPath --
 *
 *      Get size of a file without opening it.
 *
 * Results:
 *      Size of file or -1.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

int64
File_GetSizeByPath(const char *name)
{
   int64 ret = -1;
   
   if (name) {
      ret = FileIO_GetSizeByPath(name);
   }
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * File_CreateDirectoryHierarchy --
 *
 *      Create a directory including any parents that don't already exist.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *      Only the obvious.
 *
 *-----------------------------------------------------------------------------
 */

Bool
File_CreateDirectoryHierarchy(char const *pathName)
{
   char *volume;
   char *parent;
   char *sep;
   char sepContents;
   Bool success = FALSE;

   if (!pathName || !pathName[0]) {
      return TRUE;
   }

   /*
    * Skip past any volume/share.
    */
   parent = Util_SafeStrdup(pathName);
   File_SplitName(pathName, &volume, NULL, NULL);
   sep = parent + strlen(volume);
   free(volume);
   if (sep >= parent + strlen(parent)) {
      return FALSE;
   }

   /*
    * Iterate parent directories, splitting on appropriate dir separators.
    */

   while ((sep = FileFindFirstSlash(sep + 1)) != NULL) {
      /*
       * Temporarily terminate string here so we can check if this parent
       * exists, and create it if not.
       */

      sepContents = *sep;
      *sep = 0;
      if (!File_IsDirectory(parent) && !File_CreateDirectory(parent)) {
         goto fail;
      }

      /*
       * Restore string and continue walking parents.
       */

      *sep = sepContents;
   }

   success = File_IsDirectory(pathName) || File_CreateDirectory(pathName);

 fail:
   free(parent);
   return success;
}


/*
 *----------------------------------------------------------------------
 *
 * File_DeleteDirectoryTree --
 *
 *      Deletes the specified directory tree. If filesystem errors are
 *      encountered along the way, the function will continue to delete what it
 *      can but will return FALSE.
 *
 * Results:
 *      TRUE if the entire tree was deleted or didn't exist, FALSE otherwise.
 *      
 * Side effects:
 *      Deletes the directory tree from disk.
 *
 *----------------------------------------------------------------------
 */

Bool
File_DeleteDirectoryTree(char const *pathName) // IN: directory to delete
{
   Bool succeeded = FALSE, sawFileError = FALSE;
   char **fileList = NULL;
   DynBuf b;
   int i, numFiles = 0;

   ASSERT(pathName);

   DynBuf_Init(&b);

   if (!File_Exists(pathName)) {
      succeeded = TRUE;
      goto exit;
   }

   /* get list of files in current directory */
   numFiles = File_ListDirectory(pathName, &fileList);

   if (-1 == numFiles) {
      goto exit;
   }

   /* delete everything in the directory */
   for (i = 0; i < numFiles; i++) {
      const char *curPath;

      /* construct path to this file */
      DynBuf_SetSize(&b, 0);

      if (!DynBuf_Append(&b, pathName, strlen(pathName)) ||
          !DynBuf_Append(&b, DIRSEPS, 1) ||
          !DynBuf_Append(&b, fileList[i], strlen(fileList[i])) ||
          !DynBuf_Append(&b, "\0", 1)) {
         goto exit;
      }

      curPath = (const char *) DynBuf_Get(&b);

      if (File_IsDirectory(curPath)) {
         /* is dir, recurse */
         if (!File_DeleteDirectoryTree(curPath)) {
            sawFileError = TRUE;
         }
      } else {
         /* is file, delete */
         if (-1 == File_Unlink(curPath)) {
            sawFileError = TRUE;
         }
      }
   }

   /* delete the now-empty directory */
   if (!File_DeleteEmptyDirectory(pathName)) {
      sawFileError = TRUE;
   }

   if (!sawFileError) {
      succeeded = TRUE;
   }

  exit:
   DynBuf_Destroy(&b);

   if (fileList) {
      for (i = 0; i < numFiles; i++) {
         free(fileList[i]);
      }

      free(fileList);
   }

   return succeeded;
}


/*
 *-----------------------------------------------------------------------------
 *
 * File_FindFileInSearchPath --
 *
 *      Search all the directories in searchPath for a filename.
 *      If searchPath has a relative path take it with respect to cwd.
 *      searchPath must be ';' delimited.
 *
 * Results:
 *      TRUE if a file is found. FALSE otherwise.
 *
 * Side effects:
 *      If result is non Null allocate a string for the filename found.
 *
 *-----------------------------------------------------------------------------
 */

Bool
File_FindFileInSearchPath(const char *fileIn,       // IN
                          const char *searchPath,   // IN
                          const char *cwd,          // IN
                          char **result)            // OUT
{
   Bool found = FALSE;
   char *cur = NULL;
   char *sp = NULL;
   char *file = NULL;
   char *tok;

   ASSERT(fileIn);
   ASSERT(cwd);
   ASSERT(searchPath);

   /*
    * First check the usual places, the fullpath, and the cwd.
    */

   if (File_IsFullPath(fileIn)) {
      cur = Util_SafeStrdup(fileIn);
   } else {
      cur = Str_Asprintf(NULL, "%s"DIRSEPS"%s", cwd, fileIn);
   }

   if (File_Exists(cur)) {
      goto found;
   }
   free(cur);

   /*
    * Didn't find it in the usual places so strip it to its bare minimum and
    * start searching.
    */
   File_GetPathName(fileIn, NULL, &file);

   sp = Util_SafeStrdup(searchPath);
   tok = strtok(sp, FILE_SEARCHPATHTOKEN);

   while (tok) {
      if (File_IsFullPath(tok)) {
         /* Fully Qualified Path. Use it. */
         cur = Str_Asprintf(NULL, "%s%s%s", tok, DIRSEPS, file);
      } else {
         /* Relative Path.  Prepend the cwd. */
         if (Str_Strcasecmp(tok, ".") == 0) {
            /* Don't append "." */
            cur = Str_Asprintf(NULL, "%s"DIRSEPS"%s", cwd, file);
         } else {
            cur = Str_Asprintf(NULL, "%s"DIRSEPS"%s"DIRSEPS"%s", cwd,
                               tok, file);
         }
      }

      if (File_Exists(cur)) {
         goto found;
      }
      free(cur);
      tok = strtok(NULL, FILE_SEARCHPATHTOKEN);
   }

  exit:
   free(sp);
   free(file);
   return found;

  found:
   ASSERT(cur);
   found = TRUE;
   if (result) {
      *result = File_FullPath(cur);

      if (*result == NULL) {
         found = FALSE;
      }
   }
   free(cur);
   goto exit;
}


/*
 *-----------------------------------------------------------------------------
 *
 * File_ReplaceExtension --
 *
 *      Replaces the extension in input with newExtension as long as it is
 *      listed in ...
 *
 *      NB: newExtension and the extension list must have .'s.
 *          If the extension is not found the newExtension is just appended.
 *
 * Results:
 *      The name with newExtension added to it.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
File_ReplaceExtension(const char *input,                // IN
                      const char *newExtension,         // IN
                      int numExtensions,                // IN
                      ...)                              // IN
{
   char *p;
   char *temp;
   char *out;
   int i;
   va_list arguments;

   va_start(arguments, numExtensions);

   ASSERT(input);
   ASSERT(newExtension);
   ASSERT(newExtension[0] == '.');

   temp = Util_SafeStrdup(input);
   p = strrchr(temp, '.');
   if (p != NULL) {
      for (i = 0; i < numExtensions ; i++) {
         char *oldExtension = va_arg(arguments, char *);
         ASSERT(oldExtension[0] == '.');
         if (strcmp(p, oldExtension) == 0) {
            *p = '\0';
            break;
         }
      }
   }

   out = Str_Asprintf(NULL, "%s%s", temp, newExtension);
   free(temp);
   ASSERT_MEM_ALLOC(out);
   va_end(arguments);
   return out;
}


/*
 *----------------------------------------------------------------------
 *
 * File_ExpandAndCheckDir --
 *
 *	Expand any environment variables in the given path and check that
 *	the named directory is writeable.
 *
 * Results:
 *	NULL if error, the expanded path otherwise.
 *
 * Side effects:
 *	The result is allocated.
 *
 *----------------------------------------------------------------------
 */

char *
File_ExpandAndCheckDir(const char *dirName)
{
   char *edirName;

   if (dirName != NULL) {
      edirName = Util_ExpandString(dirName);
      if (edirName != NULL) {
	 if (File_IsWritableDir(edirName) == TRUE) {
            if (edirName[strlen(edirName) - 1] == DIRSEPC) {
               edirName[strlen(edirName) - 1] = '\0';
            }
	    return edirName;
	 }
	 free(edirName);
      }
   }
   return NULL;
}

#endif // N_PLAT_NLM
