/*********************************************************
 * Copyright (C) 2006 VMware, Inc. All rights reserved.
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
 *
 *********************************************************/

/*
 * filePosix.c --
 *
 *      Interface to Posix-specific file functions.
 */

#include <sys/types.h> /* Needed before sys/vfs.h with glibc 2.0 --hpreg */

#if !__FreeBSD__
# if !__APPLE__
#  include <sys/vfs.h>
# endif
# include <limits.h>
# include <stdio.h>      /* Needed before sys/mnttab.h in Solaris */
# ifdef sun
#  include <sys/mnttab.h>
# elif __APPLE__
#  include <sys/mount.h>
# else
#  include <mntent.h>
# endif
#include <signal.h>
#endif
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#ifdef __linux__
#   include <pwd.h>
#endif

#include "vmware.h"
#include "file.h"
#include "fileInt.h"
#include "msg.h"
#include "util.h"
#include "str.h"
#include "timeutil.h"
#include "dynbuf.h"
#include "localconfig.h"

#include "unicodeBase.h"
#include "unicodeOperations.h"

#if !defined(__FreeBSD__) && !defined(sun)
#if !defined(__APPLE__)
static char *FilePosixLookupMountPoint(char const *canPath, Bool *bind);
#endif
static char *FilePosixNearestExistingAncestor(char const *path);

# ifdef VMX86_SERVER
#define VMFS2CONST 456
#define VMFS3CONST 256
#include "hostType.h"
/* Needed for VMFS implementation of File_GetFreeSpace() */
#  include <sys/ioctl.h>
# endif
#endif

#ifdef VMX86_SERVER
#include "fs_user.h"
#endif


/*
 * Local functions
 */

static Bool FileIsGroupsMember(gid_t gid);


/*
 *-----------------------------------------------------------------------------
 *
 * FileRemoveDirectory --
 *
 *	Delete a directory.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side effects:
 *      May change the host file system.
 *
 *-----------------------------------------------------------------------------
 */

int
FileRemoveDirectory(ConstUnicode pathName)  // IN:
{
   int err;
   char *path;

   if (pathName == NULL) {
      return EFAULT;
   }

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      err = ENOMEM;
   } else {
      err = (rmdir(path) == -1) ? errno : 0;

      free(path);
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileRename --
 *
 *	Rename a file.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side effects:
 *      May change the host file system.
 *
 *-----------------------------------------------------------------------------
 */

int
FileRename(ConstUnicode oldName,  // IN:
           ConstUnicode newName)  // IN:
{
   int err;
   char *newPath;
   char *oldPath;

   if ((oldName == NULL) || (newName == NULL)) {
      return EFAULT;
   }

   newPath = Unicode_GetAllocBytes(newName, STRING_ENCODING_DEFAULT);
   oldPath = Unicode_GetAllocBytes(oldName, STRING_ENCODING_DEFAULT);

   if ((newPath == NULL) || (oldPath == NULL)) {
      err = ENOMEM;
   } else {
      err = (rename(oldPath, newPath) == -1) ? errno : 0;
   }

   free(newPath);
   free(oldPath);

   return err;
}


/*
 *----------------------------------------------------------------------
 *
 *  FileDeletion --
 *	Delete the specified file
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side effects:
 *      May change the host file system.
 *
 *----------------------------------------------------------------------
 */

int
FileDeletion(ConstUnicode pathName,   // IN:
             const Bool handleLink)   // IN:
{
   char *primaryPath;

   int err = 0;
   char *linkPath = NULL;

   if (pathName == NULL) {
      return EFAULT;
   }

   primaryPath = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (primaryPath == NULL) {
      return ENOMEM;
   }

   if (handleLink) {
      struct stat statbuf;

      if (lstat(primaryPath, &statbuf) == -1) {
         err = errno;
         goto bail;
      }

      if (S_ISLNK(statbuf.st_mode)) {
         linkPath = malloc(statbuf.st_size + 1);

         if (linkPath == NULL) {
            err = ENOMEM;
            goto bail;
         }

         if (readlink(primaryPath, linkPath,
                      statbuf.st_size) != statbuf.st_size) {
            err = errno;
            goto bail;
         }

         linkPath[statbuf.st_size] = '\0';

         if (unlink(linkPath) == -1) {
            if (errno != ENOENT) {
               err = errno;
               goto bail;
            }
         }
      }
   }

   err = (unlink(primaryPath) == -1) ? errno : 0;

bail:

   free(primaryPath);
   free(linkPath);

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * File_UnlinkDelayed --
 *
 *    Same as File_Unlink for POSIX systems since we can unlink anytime.
 *
 * Results:
 *    Return 0 if the unlink is successful.   Otherwise, returns -1.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

int
File_UnlinkDelayed(ConstUnicode pathName)  // IN:
{
   return (FileDeletion(pathName, TRUE) == 0) ? 0 : -1;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileAttributes --
 *
 *	Return the attributes of a file.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

int
FileAttributes(ConstUnicode pathName,  // IN:
               FileData *fileData)     // OUT:
{
   char *path;
   struct stat statbuf;

   int err = 0;

   if (pathName == NULL) {
      return EFAULT;
   }

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      return ENOMEM;
   }

   if (stat(path, &statbuf) == -1) {
      err = errno;
   } else {
      if (fileData != NULL) {
         fileData->fileCreationTime = statbuf.st_ctime;
         fileData->fileModificationTime = statbuf.st_mtime;
         fileData->fileAccessTime = statbuf.st_atime;
         fileData->fileSize = statbuf.st_size;

         switch (statbuf.st_mode & S_IFMT) {
         case S_IFREG:
            fileData->fileType = FILE_TYPE_REGULAR;
            break;

         case S_IFDIR:
            fileData->fileType = FILE_TYPE_DIRECTORY;
            break;

         case S_IFBLK:
            fileData->fileType = FILE_TYPE_BLOCKDEVICE;
            break;

         case S_IFCHR:
            fileData->fileType = FILE_TYPE_CHARDEVICE;
            break;

         case S_IFLNK:
            fileData->fileType = FILE_TYPE_SYMLINK;
            break;

         default:
            fileData->fileType = FILE_TYPE_UNCERTAIN;
            break;
         }

         fileData->fileMode = statbuf.st_mode;
         fileData->fileOwner = statbuf.st_uid;
         fileData->fileGroup = statbuf.st_gid;
      }

      err = 0;
   }

   free(path);

   return err;
}


/*
 *----------------------------------------------------------------------
 *
 * File_IsRemote --
 *
 *      Determine whether a file is on a remote filesystem.
 *      In case of an error be conservative and assume that 
 *      the file is a remote file.
 *
 * Results:
 *      The answer.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

#if !defined(__FreeBSD__) && !defined(sun)
Bool
File_IsRemote(const char *fileName) // IN: File name
{
   struct statfs sfbuf;

#ifdef VMX86_SERVER
   /*
    * On ESX, statfs() will always return VMFS_MAGIC for files on VMFS so this
    * function is only correct for files on COS, otherwise it always returns
    * FALSE.
    * On VMvisor, statfs() could return VMFS_NFS_MAGIC but it is very slow.
    * Since there is no COS for VMvisor, just be on par with ESX and return
    * FALSE directly.
    * XXX See PR 158284. It is not clear what the side-effects are of this
    * function being incorrect for VMFS files.
    */
   if (HostType_OSIsPureVMK()) {
      return FALSE;
   }
#endif

   if (statfs(fileName, &sfbuf) == -1) {
      Log("%s: statfs(%s) failed: %s\n", __func__, fileName, Msg_ErrString());
      return TRUE;
   }
#if defined(__APPLE__)
   return sfbuf.f_flags & MNT_LOCAL ? FALSE : TRUE;
#else
   if (NFS_SUPER_MAGIC == sfbuf.f_type) {
      return TRUE;
   }
   if (SMB_SUPER_MAGIC == sfbuf.f_type) {
      return TRUE;
   }
   return FALSE;
#endif
}
#endif /* !FreeBSD && !sun */


/*
 *----------------------------------------------------------------------
 *
 * File_IsSymLink --
 *
 *      Check if the specified file is a symbolic link or not
 *
 * Results:
 *      Bool - TRUE -> is a symlink, FALSE -> not a symlink or error
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsSymLink(ConstUnicode pathName)  // IN:
{
   char *path;
   struct stat statbuf;

   int err = 0;

   if (pathName == NULL) {
      return FALSE;
   }

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      return FALSE;
   }

   err = (lstat(path, &statbuf) == -1) ? errno : 0;

   free(path);

   return (err == 0) && S_ISLNK(statbuf.st_mode);
}


/*
 *----------------------------------------------------------------------
 *
 * File_Cwd --
 *
 *      Find the current directory on drive DRIVE.
 *      DRIVE is either NULL (current drive) or a string
 *      starting with [A-Za-z].
 *
 * Results:
 *      NULL if error.
 *
 * Side effects:
 *      The result is allocated
 *
 *----------------------------------------------------------------------
 */

char *
File_Cwd(const char *drive)     // IN
{
   char buffer[FILE_MAXPATH];

   if ((drive != NULL) && (drive[0] != '\0')) {
      Warning("Drive letter %s on Linux?\n", drive);
   }

   if (getcwd(buffer, FILE_MAXPATH) == NULL) {
      Msg_Append(MSGID(filePosix.getcwd)
                 "Unable to retrieve the current working directory: %s. "
                 "Please check if the directory has been deleted or "
                 "unmounted.\n",
                 Msg_ErrString());
      Warning("%s:%d getcwd() failed: %s\n",
              __FILE__, __LINE__, Msg_ErrString());
      return NULL;
   };

   return Util_SafeStrdup(buffer);
}


/*
 *----------------------------------------------------------------------
 *
 * FileStripFwdSlashes --
 *
 *      Strips off extraneous forward slashes ("/") from the pathnames.
 *
 * Results:
 *      Stripped off path over-written in the supplied argument.
 *
 * Side effects:
 *      Argument over-written.
 *
 *----------------------------------------------------------------------
 */

static void
FileStripFwdSlashes(char *path)		// IN/OUT
{
   char *cptr = path;
   char *ptr = path;
   char *prev = path;

   if (!path) {
      return;
   }

   /*
    * Copy over if not DIRSEPC. If yes, copy over only if 
    * previous character was not DIRSEPC.
    */
   while (*ptr != '\0') {
      if (*ptr == DIRSEPC) {
         if (prev != ptr - 1) {
	    *cptr++ = *ptr;
	 }
         prev = ptr;
      } else {
         *cptr++ = *ptr;
      }
      ptr++;
   }

   *cptr = '\0';
}


/*
 *----------------------------------------------------------------------
 *
 * File_FullPath --
 *
 *      Compute the full path of a file. If the file if NULL or "", the
 *      current directory is returned
 *
 * Results:
 *      NULL if error (reported to the user)
 *
 * Side effects:
 *      The result is allocated
 *
 *----------------------------------------------------------------------
 */

char *
File_FullPath(const char *fileName)     // IN
{
   char *cwd;
   char *ret;
   char buffer[FILE_MAXPATH];
   char rpath[FILE_MAXPATH];

   if ((fileName != NULL) && (fileName[0] == '/')) {
      cwd = NULL;
   } else {
      cwd = File_Cwd(NULL);
      if (cwd == NULL) {
         ret = NULL;
         goto end;
      }
   }

   if ((fileName == NULL) || (fileName[0] == '\0')) {
      ret = cwd;
   } else if (fileName[0] == '/') {
      ret = (char *) fileName;
   } else {
      char *p;
      int n;

      n = Str_Snprintf(buffer, FILE_MAXPATH, "%s/%s", cwd, fileName);
      if (n < 0) {
         Warning("%s: Couldn't snprintf\n", __func__);
         ret = NULL;
         goto end;
      }

      p = realpath(buffer, rpath);

      ret = (p == NULL) ? buffer : rpath;
   }

   ret = Util_SafeStrdup(ret);

end:
   FileStripFwdSlashes(ret);
   if (cwd != NULL) {
      free(cwd);
   }
   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * File_IsFullPath --
 *
 *      Is this a full path?
 *
 * Results:
 *      TRUE if full path.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsFullPath(ConstUnicode pathName)  // IN:
{
   if (pathName == NULL) {
      return FALSE;
   }

   /* start with a slash? */
   return Unicode_StartsWith(pathName, U(DIRSEPS));
}


/*
 *----------------------------------------------------------------------
 *
 * File_GetTimes --
 *
 *      Get the date and time that a file was created, last accessed,
 *      last modified and last attribute changed.
 *
 * Results:
 *      TRUE if succeed or FALSE if error.
 *
 * Side effects:
 *      If a particular time is not available, -1 will be returned for
 *      that time.
 *
 *----------------------------------------------------------------------
 */

Bool
File_GetTimes(ConstUnicode pathName,      // IN:
              VmTimeType *createTime,     // OUT: Windows NT time format
              VmTimeType *accessTime,     // OUT: Windows NT time format
              VmTimeType *writeTime,      // OUT: Windows NT time format
              VmTimeType *attrChangeTime) // OUT: Windows NT time format
{
   int err;
   char *path;
   struct stat statBuf;

   if (pathName == NULL) {
      return FALSE;
   }

   ASSERT(createTime && accessTime && writeTime && attrChangeTime);

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      return FALSE;
   }

   *createTime     = -1;
   *accessTime     = -1;
   *writeTime      = -1;
   *attrChangeTime = -1;

   err = (lstat(path, &statBuf) == -1) ? errno : 0;

   free(path);

   if (err != 0) {
// XXX unicode "string" in message
      Log(LGPFX" error stating file \"%s\": %s\n", pathName, strerror(err));
      return FALSE;
   }

   /*
    * XXX We should probably use the MIN of all Unix times for the creation
    *     time, so that at least times are never inconsistent in the
    *     cross-platform format. Maybe atime is always that MIN. We should
    *     check and change the code if it is not.
    *
    * XXX atime is almost always MAX.
    */

#if defined(__FreeBSD__)
   /*
    * FreeBSD: All supported versions have timestamps with nanosecond resolution.
    *          FreeBSD 5+ has also file creation time.
    */
#   if BSD_VERSION >= 50
   *createTime     = TimeUtil_UnixTimeToNtTime(statBuf.st_birthtimespec);
#   endif
   *accessTime     = TimeUtil_UnixTimeToNtTime(statBuf.st_atimespec);
   *writeTime      = TimeUtil_UnixTimeToNtTime(statBuf.st_mtimespec);
   *attrChangeTime = TimeUtil_UnixTimeToNtTime(statBuf.st_ctimespec);
#elif defined(linux)
   /*
    * Linux: Glibc 2.3+ has st_Xtim.  Glibc 2.1/2.2 has st_Xtime/__unusedX on
    *        same place (see below).  We do not support Glibc 2.0 or older.
    */
#   if (__GLIBC__ == 2) && (__GLIBC_MINOR__ < 3)
   {
      /*
       * stat structure is same between glibc 2.3 and older glibcs, just
       * these __unused fields are always zero. If we'll use __unused*
       * instead of zeroes, we get automatically nanosecond timestamps
       * when running on host which provides them.
       */
      struct timespec timeBuf;

      timeBuf.tv_sec  = statBuf.st_atime;
      timeBuf.tv_nsec = statBuf.__unused1;
      *accessTime     = TimeUtil_UnixTimeToNtTime(timeBuf);


      timeBuf.tv_sec  = statBuf.st_mtime;
      timeBuf.tv_nsec = statBuf.__unused2;
      *writeTime      = TimeUtil_UnixTimeToNtTime(timeBuf);

      timeBuf.tv_sec  = statBuf.st_ctime;
      timeBuf.tv_nsec = statBuf.__unused3;
      *attrChangeTime = TimeUtil_UnixTimeToNtTime(timeBuf);
   }
#   else
   *accessTime     = TimeUtil_UnixTimeToNtTime(statBuf.st_atim);
   *writeTime      = TimeUtil_UnixTimeToNtTime(statBuf.st_mtim);
   *attrChangeTime = TimeUtil_UnixTimeToNtTime(statBuf.st_ctim);
#   endif
#elif defined(__APPLE__)
   /* Mac: No file create timestamp. */
   *accessTime     = TimeUtil_UnixTimeToNtTime(statBuf.st_atimespec);
   *writeTime      = TimeUtil_UnixTimeToNtTime(statBuf.st_mtimespec);
   *attrChangeTime = TimeUtil_UnixTimeToNtTime(statBuf.st_ctimespec);
#else
   {
      /* Solaris: No nanosecond timestamps, no file create timestamp. */
      struct timespec timeBuf;

      timeBuf.tv_nsec = 0;

      timeBuf.tv_sec  = statBuf.st_atime;
      *accessTime     = TimeUtil_UnixTimeToNtTime(timeBuf);

      timeBuf.tv_sec  = statBuf.st_mtime;
      *writeTime      = TimeUtil_UnixTimeToNtTime(timeBuf);

      timeBuf.tv_sec  = statBuf.st_ctime;
      *attrChangeTime = TimeUtil_UnixTimeToNtTime(timeBuf);
   }
#endif

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * File_SetTimes --
 *
 *      Set the date and time that a file was created, last accessed, or
 *      last modified.
 *
 * Results:
 *      TRUE if succeed or FALSE if error.
 *
 * Side effects:
 *      If fileName is a symlink, target's timestamps will be updated.
 *      Symlink itself's timestamps will not be changed.
 *
 *----------------------------------------------------------------------
 */

Bool
File_SetTimes(ConstUnicode pathName,      // IN:
              VmTimeType createTime,      // IN: ignored
              VmTimeType accessTime,      // IN: Windows NT time format
              VmTimeType writeTime,       // IN: Windows NT time format
              VmTimeType attrChangeTime)  // IN: ignored
{
   struct timeval times[2];
   struct timeval *aTime, *wTime;
   struct stat statBuf;
   char *path;
   int err;

   if (pathName == NULL) {
      return FALSE;
   }

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      return FALSE;
   }

   err = (lstat(path, &statBuf) == -1) ? errno : 0;

   if (err != 0) {
// XXX unicode "string" in message
      Log(LGPFX" error stating file \"%s\": %s\n", pathName, strerror(err));
      free(path);
      return FALSE;
   }

   aTime = &times[0];
   wTime = &times[1];

   /*
    * Preserve old times if new time <= 0.
    * XXX Need a better implementation to preserve tv_usec.
    */
   aTime->tv_sec = statBuf.st_atime;
   aTime->tv_usec = 0;
   wTime->tv_sec = statBuf.st_mtime;
   wTime->tv_usec = 0;

   if (accessTime > 0) {
      struct timespec ts;
      TimeUtil_NtTimeToUnixTime(&ts, accessTime);
      aTime->tv_sec = ts.tv_sec;
      aTime->tv_usec = ts.tv_nsec / 1000;
   }

   if (writeTime > 0) {
      struct timespec ts;
      TimeUtil_NtTimeToUnixTime(&ts, writeTime);
      wTime->tv_sec = ts.tv_sec;
      wTime->tv_usec = ts.tv_nsec / 1000;
   }

   err = (utimes(path, times) == -1) ? errno : 0;

   free(path);

   if (err != 0) {
// XXX unicode "string" in message
      Log(LGPFX" utimes error on file \"%s\": %s\n", pathName, strerror(err));
      return FALSE;
   }

   return TRUE;
}


#if !defined(__FreeBSD__) && !defined(sun)
/*
 *-----------------------------------------------------------------------------
 *
 * FilePosixGetParent --
 *
 *      The input buffer is a canonical file path. Change it in place to the
 *      canonical file path of its parent directory.
 *
 *      Although this code is quite simple, we encapsulate it in a function
 *      because it is easy to get it wrong.
 *
 * Results:
 *      TRUE if the input buffer was (and remains) the root directory.
 *      FALSE if the input buffer was not the root directory and was changed in
 *            place to its parent directory.
 *
 *      Example: "/foo/bar" -> "/foo" FALSE
 *               "/foo"     -> "/"    FALSE
 *               "/"        -> "/"    TRUE
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
FilePosixGetParent(char const *canPath) // IN/OUT: Canonical file path
{
   char *ptr;

   ASSERT(canPath[0] == DIRSEPC);
   ptr = strrchr(canPath, DIRSEPC);
   ASSERT(ptr);
   if (ptr != canPath) {
      // "/foo/bar" -> "/foo"
   } else {
      // "/foo" -> "/"
      ptr++;

      if (*ptr == '\0') {
         // "/" -> "/"
         return TRUE;
      }
   }
   *ptr = '\0';

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * FileGetStats --
 *
 *      Calls statfs on a full path (eg. something returned from File_FullPath)
 *
 * Results:
 *      -1 if error
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Bool
FileGetStats(const char *fullPath,      // IN 
             struct statfs *pstatfsbuf) // OUT
{
   Bool retval = TRUE;
   char *dupPath = NULL;

   while (statfs(dupPath? dupPath : fullPath, pstatfsbuf) == -1) {
      if (errno != ENOENT) {
         retval = FALSE;
         goto out;
      }

      if (!dupPath) {
         /* Dup fullPath, so as not to modify input parameters */
         dupPath = Util_SafeStrdup(fullPath);
      }

      FilePosixGetParent(dupPath);
   }
   
out:
   free(dupPath);
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * File_GetFreeSpace --
 *
 *      Return the free space (in bytes) available to the user on a disk where
 *      a file is or would be
 *
 * Results:
 *      -1 if error (reported to the user)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

uint64
File_GetFreeSpace(const char *fileName) // IN: File name
{
   char *fullPath;
   uint64 ret;
   struct statfs statfsbuf;

   fullPath = File_FullPath(fileName);
   if (fullPath == NULL) {
      ret = -1;
      goto end;
   }
   
  if (!FileGetStats(fullPath, &statfsbuf)) {
      Warning("%s: Couldn't statfs\n", __func__);
      ret = -1;
      goto end;
   }
  ret = (uint64)statfsbuf.f_bavail * statfsbuf.f_bsize;
#ifdef VMX86_SERVER
   // The following test is never true on VMvisor but we do not care as
   // this is only intended for callers going through vmkfs. Direct callers
   // as we are always get the right answer from statfs above.
   if (statfsbuf.f_type == VMFS_MAGIC_NUMBER) {
      int fd;
      FS_FreeSpaceArgs args = { 0 };
      char *directory = NULL;

      File_SplitName(fullPath, NULL, &directory, NULL);
      /* Must use an ioctl() to get free space for a VMFS file. */
      ret = -1;
      fd = open(directory, O_RDONLY);
      if (fd == -1) {
         Warning("%s: open of %s failed with: %s\n", __func__, directory,
                 Msg_ErrString());
      } else {
	 if (ioctl(fd, IOCTLCMD_VMFS_GET_FREE_SPACE, &args) == -1) {
            Warning("%s: ioctl on %s failed with: %s\n", __func__,
                    fullPath, Msg_ErrString());
	 } else {
	    ret = args.bytesFree;
         }
	 close(fd);
      }
      free(directory);
   }
#endif

end:
   free(fullPath);
   return ret;
}

#ifdef VMX86_SERVER

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSAttributes --
 *
 *      Acquire the attributes for a given file on a VMFS volume.
 *
 * Results:
 *      Integer return value and populated FS_PartitionListResult
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSAttributes(const char *fileName,             // IN: File to test
                       FS_PartitionListResult **fsAttrs) // IN/OUT: VMFS Info
{
   int ret = -1;
   int fd;
   char *pathname = File_FullPath(fileName);
   char *parentPath;

   File_SplitName(pathname, NULL, &parentPath, NULL);

   if (parentPath == NULL) {
      Log(LGPFX "%s: Error acquiring parent path name\n", __func__);
      free(pathname);
      return -1;
   }

   if (!File_OnVMFS(fileName)) {
      Log(LGPFX "%s: File %s not on VMFS volume\n", __func__, fileName);
      free(pathname);
      free(parentPath);
      return -1;
   }

   *fsAttrs = Util_SafeMalloc(FS_PARTITION_ARR_SIZE(FS_PLIST_DEF_MAX_PARTITIONS));

   if (*fsAttrs == NULL) {
      Log(LGPFX "%s: failed to allocate memory\n", __func__);
      free(pathname);
      free(parentPath);
      return -1;
   }

   memset(*fsAttrs, 0, FS_PARTITION_ARR_SIZE(FS_PLIST_DEF_MAX_PARTITIONS));

   (*fsAttrs)->ioctlAttr.maxPartitions = FS_PLIST_DEF_MAX_PARTITIONS;
   (*fsAttrs)->ioctlAttr.getAttrSpec = FS_ATTR_SPEC_BASIC;

   fd = open(parentPath, O_RDONLY);
   if (fd < 0) {
      Log(LGPFX "%s: could not open %s.\n", __func__, fileName);
      goto done;
   }

   ret = ioctl(fd, IOCTLCMD_VMFS_FS_GET_ATTR, (char *) *fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: Could not get volume attributes (ret = %d)\n", __func__,
              ret);
   }

done:
   if (fd) {
      close(fd);
   }

   free(pathname);
   free(parentPath);   
   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSVersion --
 *
 *      Acquire the version number for a given file on a VMFS file system.
 *
 * Results:
 *      Integer return value and version number
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSVersion(const char *fileName, // IN: Filename to test
                    uint32 *version)      // IN/OUT: version number of VMFS
{
   int ret = -1;
   FS_PartitionListResult *fsAttrs = NULL;

   ret = File_GetVMFSAttributes(fileName, &fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: File_GetVMFSAttributes failed\n", __func__);
      goto done;
   }

   *version = fsAttrs->versionNumber;

done:
   if (fsAttrs) {
      free(fsAttrs);
   }
   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSBlockSize --
 *
 *      Acquire the blocksize for a given file on a VMFS file system.
 *
 * Results:
 *      Integer return value and block size
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSBlockSize(const char *fileName, // IN: File name to test
                      uint32 *blockSize)    // IN/OUT: VMFS block size
{
   int ret = -1;
   FS_PartitionListResult *fsAttrs = NULL;

   ret = File_GetVMFSAttributes(fileName, &fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: File_GetVMFSAttributes failed\n", __func__);
      goto done;
   }

   *blockSize = fsAttrs->fileBlockSize;

done:
   if (fsAttrs) {
      free(fsAttrs);
   }
   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSfsType --
 *
 *      Acquire the fsType for a given file on a VMFS.
 *
 * Results:
 *      Integer return value and fs type
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSfsType(const char *fileName, // IN: File name to test
                   char **fsType)        // IN/OUT: VMFS fsType
{
   int ret = -1;
   FS_PartitionListResult *fsAttrs = NULL;

   ret = File_GetVMFSAttributes(fileName, &fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: File_GetVMFSAttributes failed\n", __func__);
      goto done;
   }

   *fsType = Util_SafeMalloc(sizeof(char) * FS_PLIST_DEF_MAX_FSTYPE_LEN);
   memcpy(*fsType, fsAttrs->fsType, FS_PLIST_DEF_MAX_FSTYPE_LEN);

done:
   if (fsAttrs) {
      free(fsAttrs);
   }
   return ret;
}


#endif

/*
 *----------------------------------------------------------------------
 *
 * File_OnVMFS --
 *
 *      Return TRUE if file is on a VMFS file system.
 *
 * Results:
 *      Boolean
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_OnVMFS(const char *fileName)
{
#ifdef VMX86_SERVER
   char *fullPath;
   Bool ret;
   struct statfs statfsbuf;

   // XXX See Vmfs_IsVMFSDir. Same caveat about fs exclusion.
   if (HostType_OSIsPureVMK()) {
      return TRUE;
   }

   /*
    * Do a quick statfs() for best performance in the case that the file
    * exists.  If file doesn't exist, then get the full path and do a
    * FileGetStats() to check each of the parent directories.
    */
   if (statfs(fileName, &statfsbuf) == -1) {
      fullPath = File_FullPath(fileName);
      if (fullPath == NULL) {
	 ret = FALSE;
	 goto end;
      }
   
      if (!FileGetStats(fullPath, &statfsbuf)) {
	 Warning("%s: Couldn't statfs\n", __FUNCTION__);
	 ret = FALSE;
	 free(fullPath);
	 goto end;
      }
      free(fullPath);
   }
   ret = (statfsbuf.f_type == VMFS_MAGIC_NUMBER);

end:
   return ret;
#else
   return FALSE;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetCapacity --
 *
 *      Return the total capacity (in bytes) available to the user on a disk
 *      where a file is or would be
 *
 * Results:
 *      -1 if error (reported to the user)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

uint64
File_GetCapacity(const char *fileName) // IN: File name
{
   char *fullPath;
   uint64 ret;
   struct statfs statfsbuf;

   fullPath = File_FullPath(fileName);
   if (fullPath == NULL) {
      ret = -1;
      goto end;
   }
   
   if (!FileGetStats(fullPath, &statfsbuf)) {
      Warning("%s: Couldn't statfs\n", __func__);
      ret = -1;
      goto end;
   }

   ret = (uint64)statfsbuf.f_blocks * statfsbuf.f_bsize;

end:
   free(fullPath);
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * File_GetUniqueFileSystemID --
 *
 *      Returns a string which uniquely identifies the underlying filesystem
 *      for a given path.
 *
 *      'path' can be relative (including empty) or absolute, and any number of
 *      non-existing components at the end of 'path' are simply ignored.
 *
 *      XXX: On Posix systems, we choose the underlying device's name as the
 *           unique ID. I make no claim that this is 100% unique so if you need
 *           this functionality to be 100% perfect, I suggest you think about
 *           it more deeply than I did. -meccleston
 *
 * Results:
 *      On success: Allocated and NUL-terminated filesystem ID.
 *      On failure: NULL.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

char * 
File_GetUniqueFileSystemID(char const *path) // IN: File path
{
#ifdef VMX86_SERVER
   char canPath[FILE_MAXPATH];

   realpath(path, canPath);

   /*
    * VCFS doesn't have real mount points, so the mount point lookup below
    * returns "/vmfs", instead of the VCFS mount point.
    *
    * See bug 61646 for why we care.
    */
   if (strncmp(canPath, VCFS_MOUNT_POINT, strlen(VCFS_MOUNT_POINT)) == 0) {
      char vmfsVolumeName[FILE_MAXPATH];

      if (sscanf(canPath, VCFS_MOUNT_PATH "%[^/]%*s", vmfsVolumeName) == 1) {
         return Str_Asprintf(NULL, "%s/%s", VCFS_MOUNT_POINT, vmfsVolumeName);
      }
   }
#endif

   return FilePosixGetBlockDevice(path);
}


#if !defined(__APPLE__)
/*
 *-----------------------------------------------------------------------------
 *
 * FilePosixLookupMountPoint --
 *
 *      Looks up passed in canonical file path in list of mount points.
 *      If there is a match, it returns the underlying device name of the
 *      mount point along with a flag indicating whether the mount point is
 *      mounted with the "--[r]bind" option.
 *
 * Results:
 *      On success: The allocated, NUL-terminated mounted "device".
 *      On failure: NULL.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static char *
FilePosixLookupMountPoint(char const *canPath, // IN: Canonical file path
                          Bool *bind)          // OUT: Mounted with --[r]bind?
{
   FILE *f;
   struct mntent *mnt;

   ASSERT(canPath);
   ASSERT(bind);

   f = setmntent(MOUNTED, "r");
   if (f == NULL) {
      return NULL;
   }

   // XXX getmntent() is not thread-safe. Use getmntent_r() instead.
   while ((mnt = getmntent(f)) != NULL) {
      /*
       * NB: A call to realpath is not needed as getmntent() already
       *     returns it in canonical form.  Additionally, it is bad
       *     to call realpath() as often a mount point is down, and
       *     realpath calls stat which can block trying to stat
       *     a filesystem that the caller of the function is not at
       *     all expecting.
       */
      if (strcmp(mnt->mnt_dir, canPath) == 0) {
         endmntent(f);

         /*
          * The --bind and --rbind options behave differently. See 
          * FilePosixGetBlockDevice() for details.
          *
          * Sadly (I blame a bug in 'mount'), there is no way to tell them
          * apart in /etc/mtab: the option recorded there is, in both cases,
          * always "bind".
          */
         *bind = strstr(mnt->mnt_opts, "bind") != NULL;

         return Util_SafeStrdup(mnt->mnt_fsname);
      }
   }

   // 'canPath' is not a mount point.
   endmntent(f);
   return NULL;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * FilePosixGetBlockDevice --
 *
 *      Retrieve the block device that backs file path 'path'.
 *
 *      'path' can be relative (including empty) or absolute, and any number of
 *      non-existing components at the end of 'path' are simply ignored.
 *
 * Results:
 *      On success: The allocated, NUL-terminated block device absolute path.
 *      On failure: NULL.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

char *
FilePosixGetBlockDevice(char const *path) // IN: File path
{
   char *existPath;
   Bool failed;
#if defined(__APPLE__)
   struct statfs buf;
#else
   char canPath[FILE_MAXPATH];
   char canPath2[FILE_MAXPATH];
   unsigned int retries = 0;
#endif

   existPath = FilePosixNearestExistingAncestor(path);
   if (!existPath) {
      return NULL;
   }

#if defined(__APPLE__)
   failed = statfs(existPath, &buf) == -1;
   free(existPath);
   if (failed) {
      return NULL;
   }

   return Util_SafeStrdup(buf.f_mntfromname);
#else
   failed = !realpath(existPath, canPath);
   free(existPath);
   if (failed) {
      return NULL;
   }

retry:
   Str_Strcpy(canPath2, canPath, sizeof canPath2);

   // Find the nearest ancestor of 'canPath' that is a mount point.
   for (;;) {
      Bool bind;
      char *ptr;

      ptr = FilePosixLookupMountPoint(canPath, &bind);
      if (ptr) {
         if (bind) {
            /*
             * 'canPath' is a mount point mounted with --[r]bind. This is the
             * mount equivalent of a hard link. Follow the rabbit...
             *
             * --bind and --rbind behave differently. Consider this mount
             * table:
             *
             *    /dev/sda1              /             ext3
             *    exit14:/vol/vol0/home  /exit14/home  nfs
             *    /                      /bind         (mounted with --bind)
             *    /                      /rbind        (mounted with --rbind)
             *
             * then what we _should_ return for these paths is:
             *
             *    /bind/exit14/home -> /dev/sda1
             *    /rbind/exit14/home -> exit14:/vol/vol0/home
             *
             * XXX but currently because we cannot easily tell the difference,
             *     we always assume --rbind and we return:
             *
             *    /bind/exit14/home -> exit14:/vol/vol0/home
             *    /rbind/exit14/home -> exit14:/vol/vol0/home
             */
            Bool rbind = TRUE;

            if (rbind) {
               /*
                * Compute 'canPath = ptr + (canPath2 - canPath)' using and
                * preserving the structural properties of all canonical
                * paths involved in the expression.
                */

               size_t canPathLen = strlen(canPath);
               char const *diff = canPath2 + (canPathLen > 1 ? canPathLen : 0);

               if (*diff != '\0') {
                  Str_Sprintf(canPath, sizeof canPath, "%s%s",
                     strlen(ptr) > 1 ? ptr : "",
                     diff);
               } else {
                  Str_Strcpy(canPath, ptr, sizeof canPath);
               }
            } else {
               Str_Strcpy(canPath, ptr, sizeof canPath);
            }

            free(ptr);

            /*
             * There could be a series of these chained together.  It is
             * possible for the mounts to get into a loop, so limit the total
             * number of retries to something reasonable like 10.
             */
            retries++;
            if (retries > 10) {
               Warning("%s: The --[r]bind mount count exceeds %u. Giving "
                       "up.\n", __func__, 10);
               return NULL;
            }

            goto retry;
         }

         return ptr;
      }

      failed = FilePosixGetParent(canPath);
      /*
       * Prevent an infinite loop in case FilePosixLookupMountPoint() even
       * fails on "/".
       */
      if (failed) {
         return NULL;
      }
   }
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * FilePosixNearestExistingAncestor --
 *
 *      Find the nearest existing ancestor of 'path'.
 *
 *      'path' can be relative (including empty) or absolute, and 'path' can
 *      have any number of non-existing components at its end.
 *
 * Results:
 *      On success: The allocated, NUL-terminated, non-empty path of the
 *                  nearest existing ancestor.
 *      On failure: NULL.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static char *
FilePosixNearestExistingAncestor(char const *path) // IN: File path
{
   size_t resultSize;
   char *result;

   resultSize = MAX(strlen(path), 1) + 1;
   result = malloc(resultSize);
   if (!result) {
      return NULL;
   }

   Str_Strcpy(result, path, resultSize);
   for (;;) {
      char *ptr;

      if (*result == '\0') {
         Str_Strcpy(result, *path == DIRSEPC ? "/" : ".", resultSize);
         break;
      }

      if (File_Exists(result)) {
         break;
      }

      ptr = strrchr(result, DIRSEPC);
      if (!ptr) {
         ptr = result;
      }
      *ptr = '\0';
   }

   return result;
}


/*
 *----------------------------------------------------------------------------
 *
 * File_IsSameFile --
 *
 *      Determine whether both paths point to the same file.
 *
 *      Caveats - While local files are matched based on inode and device 
 *      ID, some older versions of NFS return buggy device IDs, so the
 *      determination cannot be done with 100% confidence across NFS.
 *      Paths that traverse NFS mounts are matched based on device, inode
 *      and all of the fields of the stat structure except for times.
 *      This introduces a race condition in that if the target files are not
 *      locked, they can change out from underneath this function yielding
 *      false negative results.  Cloned files sytems mounted across an old
 *      version of NFS may yield a false positive.  
 *
 * Results:
 *      TRUE if both paths point to the same file, FALSE otherwise.
 *
 * Side effects:
 *      Changes errno, maybe.
 *
 *----------------------------------------------------------------------------
 */

Bool
File_IsSameFile(const char *path1, // IN
                const char *path2) // IN
{
   struct stat st1;
   struct stat st2;
   struct statfs stfs1;
   struct statfs stfs2;

   ASSERT(path1);
   ASSERT(path2);

#ifdef VMX86_SERVER
   {
      char *fs1;
      char *fs2;
      char realpath1[FILE_MAXPATH];
      char realpath2[FILE_MAXPATH];

      fs1 = realpath(path1, realpath1);
      fs2 = realpath(path2, realpath2);

      /*
       * ESX doesn't have real inodes for VMFS disks in User Worlds. So only way
       * to check if a file is the same is using real path.  So said Satyam.
       */

      if (fs1 &&
          strncmp(fs1, VCFS_MOUNT_POINT, strlen(VCFS_MOUNT_POINT)) == 0) {
         if (!fs2 || strcmp(realpath1, realpath2) != 0) {
            return FALSE;
         } else {
            return TRUE;
         }
      }
   }
#endif

   /*
    * First take care of the easy checks.  If the paths are identical, or if
    * the inode numbers don't match, we're done.
    */
   if (strcmp(path1, path2) == 0) {
      return TRUE;
   }

   if (stat(path1, &st1) == -1) {
      return FALSE;
   }

   if (stat(path2, &st2) == -1) {
      return FALSE;
   }

   if (st1.st_ino != st2.st_ino) {
      return FALSE;
   }

   if (statfs(path1, &stfs1) != 0) {
      return FALSE;
   }

   if (statfs(path2, &stfs2) != 0) {
      return FALSE;
   }

#if defined(__APPLE__)
   if ((stfs1.f_flags & MNT_LOCAL) && (stfs2.f_flags & MNT_LOCAL)) {
      return st1.st_dev == st2.st_dev;
   }
#else
   if ((stfs1.f_type != NFS_SUPER_MAGIC) && (stfs2.f_type != NFS_SUPER_MAGIC)) {
      return st1.st_dev == st2.st_dev;
   }
#endif

   /*
    * At least one of the paths traverses NFS and some older NFS
    * implementations can set st_dev incorrectly. Do some extra checks of the
    * stat structure to increase our confidence. Since the st_ino numbers had
    * to match to get this far, the overwhelming odds are the two files are
    * the same.  
    *
    * If another process was actively writing or otherwise modifying the file
    * while we stat'd it, then the following test could fail and we could
    * return a false negative.  On the other hand, if NFS lies about st_dev
    * and the paths point to a cloned file system, then the we will return a
    * false positive.
    */
   if ((st1.st_dev == st2.st_dev) &&
       (st1.st_mode == st2.st_mode) &&
       (st1.st_nlink == st2.st_nlink) &&
       (st1.st_uid == st2.st_uid) &&
       (st1.st_gid == st2.st_gid) &&
       (st1.st_rdev == st2.st_rdev) &&
       (st1.st_size == st2.st_size) &&
       (st1.st_blksize == st2.st_blksize) &&
       (st1.st_blocks == st2.st_blocks)) {
      return TRUE;
   }
   return FALSE;
}


#endif /* !FreeBSD && !sun */


/*
 *-----------------------------------------------------------------------------
 *
 * File_Replace --
 *
 *      Replace old file with new file, and attempt to reproduce
 *      file permissions.
 *
 * Results:
 *      TRUE on success.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
File_Replace(ConstUnicode oldName,  // IN: old file
             ConstUnicode newName)  // IN: new file
{
   Bool status;
   char *newPath;
   char *oldPath;
   struct stat st;

   if ((oldName == NULL) || (newName == NULL)) {
      return FALSE;
   }

   newPath = Unicode_GetAllocBytes(newName, STRING_ENCODING_DEFAULT);
   oldPath = Unicode_GetAllocBytes(oldName, STRING_ENCODING_DEFAULT);

   if ((oldPath == NULL) || (newPath == NULL)) {
      status = FALSE;
      goto bail;
   }

   /* UNICODE: path names added to Msg_Append... */

   if ((stat(oldPath, &st) == 0) && (chmod(newPath, st.st_mode) == -1)) {
      Msg_Append(MSGID(filePosix.replaceChmodFailed)
                 "Failed to duplicate file permissions from "
                 "\"%s\" to \"%s\": %s\n",
                 oldPath, newPath, Msg_ErrString());

      status = FALSE;
      goto bail;
   }

   if (rename(newPath, oldPath) == -1) {
      Msg_Append(MSGID(filePosix.replaceRenameFailed)
                 "Failed to rename \"%s\" to \"%s\": %s\n",
                 newPath, oldPath, Msg_ErrString());

      status = FALSE;
      goto bail;
   }

   status = TRUE;

bail:
   free(newPath);
   free(oldPath);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIsVMFS --
 *
 *      Determine whether specified file lives on VMFS filesystem.
 *      Only Linux host can have VMFS, so skip it on Solaris
 *      and FreeBSD.
 *
 * Results:
 *      TRUE if specified file lives on VMFS
 *      FALSE if file is not on VMFS or does not exist
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
FileIsVMFS(ConstUnicode pathName)  // IN: file name to test
{
#if defined(linux)
   char *path;
   struct statfs statbuf;

   int err = 0;

#if defined(VMX86_SERVER)
   // XXX See Vmfs_IsVMFSFile. Same caveat about fs exclusion.
   if (HostType_OSIsPureVMK()) {
      return TRUE;
   }
#endif

   if (pathName == NULL) {
      return FALSE;
   }

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      return FALSE;
   }

   err = (statfs(path, &statbuf) == -1) ? errno : 0;

   free(path);

   if (err == 0) {
      return statbuf.f_type == VMFS_SUPER_MAGIC;
   }

#endif

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * FilePosixCreateTestFileSize --
 *
 *      See if the given directory is on a file system that supports
 *      large files.  We just create an empty file and pass it to the
 *      FileIO_SupportsFileSize which does actual job of determining
 *      file size support.
 *
 * Results:
 *      TRUE if FS supports files of specified size
 *      FALSE otherwise (no support, invalid path, ...)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
FilePosixCreateTestFileSize(const char *dirName,  // IN: directory to create large file
                            uint64      fileSize) // IN: test file size
{
   FileIODescriptor fd;
   char pathname[FILE_MAXPATH];
   Bool retVal;
   char *tmpFileName;
   int posixFD;

   Str_Sprintf(pathname, sizeof pathname, "%s/.vmBigFileTest", dirName);
   posixFD = File_MakeTemp(pathname, &tmpFileName);
   if (posixFD == -1) {
      return FALSE;
   }
   
   fd = FileIO_CreateFDPosix(posixFD, O_RDWR);
   retVal = FileIO_SupportsFileSize(&fd, fileSize);
   /* Eventually perform destructive tests here... */
   FileIO_Close(&fd);
   File_Unlink(tmpFileName);
   free(tmpFileName);
   return retVal;
}

/*
 *----------------------------------------------------------------------
 *
 * File_VMFSSupportsFileSize --
 *
 *      Check if the given file is on a VMFS supports such a file size
 *
 *      In the case of VMFS2, the largest supported file size is
 *         456 * 1024 * B bytes
 *
 *      In the case of VMFS3/4, the largest supported file size is
 *         256 * 1024 * B bytes
 *
 *      where B represents the blocksize in bytes
 *
 *
 * Results:
 *      TRUE if VMFS supports such file size
 *      FALSE otherwise (file size not supported)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Bool
File_VMFSSupportsFileSize(const char *fileName, // IN
                          uint64      fileSize) // IN
{
#ifdef VMX86_SERVER
   uint32 version = -1;
   uint32 blockSize = -1;
   uint64 maxFileSize = -1;
   Bool supported;
   char *pathName;
   char *parentPath;
   char *fsType = NULL;

   if (File_GetVMFSVersion(fileName, &version) < 0) {
      Log(LGPFX "%s: File_GetVMFSVersion Failed\n", __func__);
      return FALSE;
   }
   if (File_GetVMFSBlockSize(fileName, &blockSize) < 0) {
      Log(LGPFX "%s: File_GetVMFSBlockSize Failed\n", __func__);
      return FALSE;
   }
   if (File_GetVMFSfsType(fileName, &fsType) < 0) {
      Log(LGPFX "%s: File_GetVMFSfsType Failed\n", __func__);
      return FALSE;
   }

   if (strcmp(fsType, "VMFS") == 0) {
      if (version == 2) {
         maxFileSize = (VMFS2CONST * (uint64) blockSize * 1024);
      } else if (version >= 3) {
         /* Get ready for VMFS4 and perform sanity check on version */
         ASSERT(version == 3 || version == 4);

         maxFileSize = (VMFS3CONST * (uint64) blockSize * 1024);
      } 

      if (fileSize <= maxFileSize && maxFileSize != -1) {
         free(fsType);
         return TRUE;
      } else {
         Log(LGPFX "Requested file size (%"FMT64"d) larger than maximum "
             "supported filesystem file size (%"FMT64"d)\n",
             fileSize, maxFileSize);
         free(fsType);
         return FALSE;
      }

   } else {
      pathName = File_FullPath(fileName);
      if (pathName == NULL) {
         Log(LGPFX "%s: Error acquiring full path\n", __func__);
         free(fsType);
         return FALSE;
      }

      File_SplitName(pathName, NULL, &parentPath, NULL);
      if (parentPath == NULL) {
         Log(LGPFX "%s: Error acquiring parent path name\n", __func__);
         free(fsType);
         free(pathName);
         return FALSE;
      }

      supported = FilePosixCreateTestFileSize(parentPath, fileSize);
      
      free(fsType);
      free(pathName);
      free(parentPath);
      return supported;
   }
   
#endif
   Log(LGPFX "%s did not execute properly\n", __func__);
   return FALSE; /* happy compiler */
}

/*
 *----------------------------------------------------------------------
 *
 * File_SupportsFileSize --
 *
 *      Check if the given file is on an FS that supports such file size
 *
 * Results:
 *      TRUE if FS supports such file size
 *      FALSE otherwise (file size not supported, invalid path, read-only, ...)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_SupportsFileSize(const char *filePath, // IN
                      uint64      fileSize) // IN
{
   Bool supported = FALSE;
   char *p;
   char *pathname;
   char *parentPath = NULL;

   /* All supported filesystems can hold at least 2GB - 1 files. */
   if (fileSize <= 0x7FFFFFFF) {
      return TRUE;
   }

   /* 
    * We acquire the full path name for testing in 
    * FilePosixCreateTestFileSize().  This is also done in the event that
    * a user tries to create a virtual disk in the directory that they want
    * a vmdk created in (setting filePath only to the disk name, not the entire
    * path.
    */
   pathname = File_FullPath(filePath);
   if (pathname == NULL) {
      Log(LGPFX "%s: Error acquiring full path\n", __func__);
      goto out;
   }

   /* 
    * We then truncate the name to point to the parent directory of the file
    * created so we can get accurate results from FileIsVMFS.
    */
   File_SplitName(pathname, NULL, &parentPath, NULL);
   if (parentPath == NULL) {
      Log(LGPFX "%s: Error acquiring parent path name\n", __func__);
      goto out;
   }

   /* 
    * We know that VMFS supports large files - But they have limitations
    * See function File_VMFSSupportsFileSize() - PR 146965
    */
   if (FileIsVMFS(parentPath)) {
      supported = File_VMFSSupportsFileSize(filePath, fileSize);
      goto out;
   }

   if (File_IsFile(filePath)) {
      FileIODescriptor fd;
      FileIOResult res;

      FileIO_Invalidate(&fd);
      res = FileIO_Open(&fd, filePath, FILEIO_OPEN_ACCESS_READ, FILEIO_OPEN);
      if (res == FILEIO_SUCCESS) {
         supported = FileIO_SupportsFileSize(&fd, fileSize);
         FileIO_Close(&fd);
         goto out;
      }
   }

   p = strrchr(pathname, '/');
   if (p == NULL) {
      free(pathname);
      pathname = File_Cwd(NULL);

      if (pathname == NULL) {
         goto out;
      }
   } else {
      *p = '\0';
   }

   /*
    * On unknown filesystems create temporary file and use it to test.
    */
   supported = FilePosixCreateTestFileSize(parentPath, fileSize);

out:
   
   free(pathname);
   free(parentPath);
   return supported;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileCreateDirectory --
 *
 *	Create a directory. The umask is honored.
 *
 * Results:
 *	0	success
 *	> 0	failure (errno)
 *
 * Side effects:
 *      May change the host file system.
 *
 *-----------------------------------------------------------------------------
 */

int
FileCreateDirectory(ConstUnicode pathName)  // IN:
{
   int err;
   char *path;

   if (pathName == NULL) {
      return EFAULT;
   }

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      err = ENOMEM;
   } else {
      err = (mkdir(path, S_IRWXU | S_IRWXG | S_IRWXO) == -1) ? errno : 0;

      free(path);
   }

   return err;
}


/*
 *----------------------------------------------------------------------
 *
 * File_ListDirectory --
 *
 *      Gets the list of files (and directories) in a directory.
 *
 * Results:
 *      Returns the number of files returned or -1 on failure.
 *
 * Side effects:
 *      If ids is provided and the function succeeds, memory is allocated
 *      and must be freed.  Array of strings and array itself must be
 *      freed.
 *
 *----------------------------------------------------------------------
 */

int
File_ListDirectory(char const *pathName,     // IN
                   char ***ids)              // OUT: relative paths
{
   int err;
   DIR *dir;
   DynBuf b;
   int count = 0;

   if (pathName == NULL) {
      errno = EFAULT;
      return -1;
   }

   errno = 0;
   dir = opendir(pathName);

   if (dir == (DIR *) NULL) {
      // errno is accessible, in the future, for more detail
      return -1;
   }

   DynBuf_Init(&b);

   while (TRUE) {
      struct dirent *entry;

      errno = 0;
      entry = readdir(dir);

      if (entry == (struct dirent *) NULL) {
         err = errno;
         break;
      }

      /* Strip out undesirable paths.  No one ever cares about these. */
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
         continue;
      }

      /* Don't create the file list if we aren't providing it to the caller. */
      if (ids) {
         char *id = Util_SafeStrdup(entry->d_name);
         DynBuf_Append(&b, &id, sizeof(&id));
      }

      count++;
   }

   closedir(dir);

   if (ids && (err == 0)) {
      *ids = DynBuf_AllocGet(&b);
      ASSERT_MEM_ALLOC(*ids);
   }

   DynBuf_Destroy(&b);

   return (err == 0) ? count : -1;
}


/*
 *----------------------------------------------------------------------
 *
 * File_IsWritableDir --
 *
 *	Determine in a non-intrusive way if the user can create a file in a
 *	directory
 *
 * Results:
 *	FALSE if error (reported to the user)
 *
 * Side effects:
 *	None
 *
 * Bug:
 *	It would be cleaner to use the POSIX access(2), which deals well
 *	with read-only filesystems. Unfortunately, access(2) doesn't deal with
 *	the effective [u|g]ids.
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsWritableDir(ConstUnicode dirName)  // IN:
{
   int err;
   uid_t euid;
   FileData fileData;

   if (dirName == NULL) {
      errno = EFAULT;
      return FALSE;
   }

   err = FileAttributes(dirName, &fileData);

   if ((err != 0) || (fileData.fileType != FILE_TYPE_DIRECTORY)) {
      errno = err;
      return FALSE;
   }

   euid = geteuid();
   if (euid == 0) {
      /* Root can read or write any file. Well... This is not completely true
         because of read-only filesystems and NFS root squashing... What a
         nightmare --hpreg */
      return TRUE;
   }

   if (fileData.fileOwner == euid) {
      fileData.fileMode >>= 6;
   } else if (FileIsGroupsMember(fileData.fileGroup)) {
      fileData.fileMode >>= 3;
   }

   /* Check for Read and Execute permissions */
   return (fileData.fileMode & 3) == 3;
}


/*
 *----------------------------------------------------------------------
 *
 * FileTryDir --
 *
 *	Check to see if the given directory is actually a directory
 *      and is writable by us.
 *
 * Results:
 *	The expanded directory name on success, NULL on failure.
 *
 * Side effects:
 *	The result is allocated.
 *
 *----------------------------------------------------------------------
 */

static char *
FileTryDir(const char *dirName) // IN: Is this a writable directory?
{
   char *edirName;

   if (dirName == NULL) {
      return NULL;
   }

   edirName = Util_ExpandString(dirName);
   if (edirName != NULL && File_IsWritableDir(edirName)) {
      return edirName;
   }
   free(edirName);

   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * File_GetTmpDir --
 *
 *	Determine the best temporary directory. Unsafe since the
 *	returned directory is generally going to be 0777, thus all sorts
 *	of denial of service or symlink attacks are possible.  Please
 *	use Util_GetSafeTmpDir if your dependencies permit it.
 *
 * Results:
 *	NULL if error (reported to the user).
 *
 * Side effects:
 *	The result is allocated.
 *
 *----------------------------------------------------------------------
 */

char *
File_GetTmpDir(Bool useConf) // IN: Use the config file?
{
   char *dirName;
   char *edirName;

   /* Make several attempts to find a good temporary directory candidate */

   if (useConf) {
      dirName = (char *)LocalConfig_GetString(NULL, "tmpDirectory");
      edirName = FileTryDir(dirName);
      free(dirName);
      if (edirName != NULL) {
         return edirName;
      }
   }

   /* getenv string must _not_ be freed */
   edirName = FileTryDir(getenv("TMPDIR"));
   if (edirName != NULL) {
      return edirName;
   }

   /* P_tmpdir is usually defined in <stdio.h> */
   edirName = FileTryDir(P_tmpdir);
   if (edirName != NULL) {
      return edirName;
   }

   edirName = FileTryDir("/tmp");
   if (edirName != NULL) {
      return edirName;
   }

   edirName = FileTryDir("~");
   if (edirName != NULL) {
      return edirName;
   }

   dirName = File_Cwd(NULL);

   if (dirName != NULL) {
      edirName = FileTryDir(dirName);
      free(dirName);
      if (edirName != NULL) {
         return edirName;
      }
   }

   edirName = FileTryDir("/");
   if (edirName != NULL) {
      return edirName;
   }

   Warning("%s: Couldn't get a temporary directory\n", __FUNCTION__);
   return NULL;
}

#undef HOSTINFO_TRYDIR


/*
 *----------------------------------------------------------------------
 *
 * FileIsGroupsMember --
 *
 *	Determine if a gid is in the gid list of the current process
 *
 * Results:
 *	FALSE if error (reported to the user)
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static Bool
FileIsGroupsMember(gid_t gid)
{
   int nr_members;
   gid_t *members;
   int res;
   int ret;

   members = NULL;
   nr_members = 0;
   for (;;) {
      gid_t *new;

      res = getgroups(nr_members, members);
      if (res == -1) {
	 Warning("%s: Couldn't getgroups\n", __FUNCTION__);
	 ret = FALSE;
	 goto end;
      }

      if (res == nr_members) {
	 break;
      }

      /* Was bug 17760 --hpreg */
      new = realloc(members, res * sizeof *members);
      if (new == NULL) {
	 Warning("%s: Couldn't realloc\n", __FUNCTION__);
	 ret = FALSE;
	 goto end;
      }

      members = new;
      nr_members = res;
   }

   for (res = 0; res < nr_members; res++) {
      if (members[res] == gid) {
	 ret = TRUE;
	 goto end;
      }
   }
   ret = FALSE;

end:
   free(members);

   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_MakeCfgFileExecutable --
 *
 *	Make a .vmx file executable. This is sometimes necessary 
 *      to enable MKS access to the VM.
 *
 * Results:
 *	FALSE if error
 *
 * Side effects:
 *	errno is set on error
 *
 *----------------------------------------------------------------------
 */

Bool
File_MakeCfgFileExecutable(ConstUnicode pathName)
{
   char *path;
   Bool result;

   if (pathName == NULL) {
      errno = EFAULT;
      return FALSE;
   }

   path = Unicode_GetAllocBytes(pathName, STRING_ENCODING_DEFAULT);

   if (path == NULL) {
      errno = ENOMEM;
      result = FALSE;
   } else {
      int err;

      result = chmod(path,
                     S_IRUSR | S_IWUSR | S_IXUSR |  // rwx by user
                     S_IRGRP | S_IXGRP |            // rx by group
                     S_IROTH | S_IXOTH              // rx by others
                    ) == 0;

      err = errno;
      free(path);
      errno = err;
   }

   return result;
}


/*
 *----------------------------------------------------------------------------
 *
 * File_GetSizeAlternate --
 *
 *      An alternate way to determine the filesize. Useful for finding problems
 *      with files on remote fileservers, such as described in bug 19036.
 *      However, in Linux we do not have an alternate way, yet, to determine the
 *      problem, so we call back into the regular getSize function.
 *
 * Results:
 *      Size of file or -1.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

int64
File_GetSizeAlternate(ConstUnicode pathName)  // IN:
{
   return File_GetSize(pathName);
}
