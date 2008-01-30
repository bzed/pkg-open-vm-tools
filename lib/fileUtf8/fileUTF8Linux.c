/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
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
 * fileUTF8Posix.c --
 *
 *      Interface to host-specific file functions for Posix hosts
 *
 */

#include <sys/types.h> /* Needed before sys/vfs.h with glibc 2.0 --hpreg */
#ifndef __FreeBSD__
#include <limits.h>
#include <stdio.h>      /* Needed before sys/mnttab.h in Solaris */
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
#include "util.h"
#include "str.h"
#include "dynbuf.h"
#include "codeset.h"
#include "fileUTF8.h"


/*
 * There has been uncertainity about whether we should convert from
 * the input/output string character set, which is assumed to be UTF-8, and the
 * strings passed to the linux libs/systemcalls. Currently, we think this is
 * unnecessary, since linux should accept utf-8. 
 */
#define CONVERT_STRINGS_FROM_UTF8_TO_LOCAL  0


/*
 *-----------------------------------------------------------------------------
 *
 * FileUTF8_Copy --
 *
 *      Copy a file from one place to another. An existing file is never
 *      overwritten.
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
FileUTF8_Copy(const char *utf8SrcFile,   // IN: old file
              const char *utf8DstFile)   // IN: new file
{
   Bool result = FALSE;
   char *localSrcName = NULL;
   char *localDstName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8SrcFile,
                              strlen(utf8SrcFile),
                              &localSrcName,
                              NULL)) {
      result = FALSE;
      goto abort;
   }
   if (!CodeSet_Utf8ToCurrent(utf8DstFile,
                              strlen(utf8DstFile),
                              &localDstName,
                              NULL)) {
      result = FALSE;
      goto abort;
   }
#else
   localSrcName = (char *) utf8SrcFile;
   localDstName = (char *) utf8DstFile;
#endif

   // The "2" on the next line tells the func to return an err if dst exists
   result = File_Copy(localSrcName, localDstName, FALSE);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
abort:
   free(localSrcName);
   free(localDstName);
#endif

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileUTF8_Rename --
 *
 *      Rename old file to new file.
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
FileUTF8_Rename(const char *utf8OldFile,   // IN: old file
                 const char *utf8NewFile)   // IN: new file
{
   Bool result = FALSE;
   char *localOldName = NULL;
   char *localNewName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8OldFile,
                              strlen(utf8OldFile),
                              &localOldName,
                              NULL)) {
      result = FALSE;
      goto abort;
   }
   if (!CodeSet_Utf8ToCurrent(utf8NewFile,
                              strlen(utf8NewFile),
                              &localNewName,
                              NULL)) {
      result = FALSE;
      goto abort;
   }
#else
   localOldName = (char *) utf8OldFile;
   localNewName = (char *) utf8NewFile;
#endif

   result = File_Rename(localOldName, localNewName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
abort:
   free(localOldName);
   free(localNewName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------------
 *
 * FileUTF8_GetSize --
 *
 *      Get size of file.
 *
 * Results:
 *      Size of file on success, -1 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

int64
FileUTF8_GetSize(const char *utf8Name)    // IN
{
   int64 result = -1;
#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   char *localName = NULL;
   
   if (CodeSet_Utf8ToCurrent(utf8Name,
                             strlen(utf8Name),
                             &localName,
                             NULL)) {
      result = File_GetSize(localName);
      free(localName);
   }
#else
   result = File_GetSize(utf8Name);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_CreateDirectory --
 *
 *      Creates the specified directory.
 *
 * Results:
 *      True if the directory is successfully created, false otherwise.
 *
 * Side effects:
 *      Creates the directory on disk.
 *
 *----------------------------------------------------------------------
 */

Bool
FileUTF8_CreateDirectory(char const *utf8Name)     // IN
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif
                              
   result = File_CreateDirectory(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_DeleteEmptyDirectory --
 *
 *      Deletes the specified directory if it is empty.
 *
 * Results:
 *      True if the directory is successfully deleted, false otherwise.
 *
 * Side effects:
 *      Deletes the directory from disk.
 *
 *----------------------------------------------------------------------
 */

Bool
FileUTF8_DeleteEmptyDirectory(char const *utf8Name)     // IN
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_DeleteEmptyDirectory(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_ListDirectory --
 *
 *      Gets the list of files (and directories) in a directory.
 *
 * Results:
 *      Returns the number of files returned or -1 on failure.
 *
 * Side effects:
 *      Memory is allocated and must be freed.  Array of strings and
 *      array itself must be freed.
 *
 *----------------------------------------------------------------------
 */

int
FileUTF8_ListDirectory(char const *utf8Name,     // IN
                       char ***ids)              // OUT: relative paths
{
   int result = 0;
   char *localName = NULL;
   int index;
   char **strList;
   char *currentLocalStr = NULL;
#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   int resultLength;
#endif

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return -1;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_ListDirectory(localName, ids);

   if ((result > 0) && (NULL != ids) && (NULL != *ids)) {
      strList = *ids;
      for (index = 0; index < result; index++) {
         currentLocalStr = strList[index];
         if (NULL == currentLocalStr) {
            continue;
         }

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
         if (!CodeSet_CurrentToUtf8(currentLocalStr,
                                    strlen(currentLocalStr),
                                    &(strList[index]),
                                    &resultLength)) {
            result = -1;
            goto abort;
         }
         free(currentLocalStr);
#else
         strList[index] = currentLocalStr;
#endif
         currentLocalStr = NULL;
      } // for (index = 0; index < result; index++)
   } // if ((result > 0) && (NULL != ids) && (NULL != *ids))

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
abort:
   free(currentLocalStr);
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_UnlinkIfExists --
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
FileUTF8_UnlinkIfExists(const char *utf8Name)   // IN
{
   int result = 0;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return 0;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = unlink(localName);
   if (result < 0 && ENOENT == errno) {
      result = 0;
   }

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_IsFile --
 *
 *      Check if specified file is a file or not
 *
 * Results:
 *      Bool - TRUE -> is a file, FALSE -> not a file or error
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
FileUTF8_IsFile(const char *utf8Name)      // IN
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_IsFile(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif
   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_IsDirectory --
 *
 *      Check if specified file is a directory or not
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
FileUTF8_IsDirectory(const char *utf8Name)      // IN
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_IsDirectory(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif
   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_IsSymLink --
 *
 *      Check if the specified file is a symbolic link or not.
 *
 * Results:
 *      Returns TRUE if 'name' is a symlink, or FALSE if
 *      'name' is not a symlink or there is an error.
 *      
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
FileUTF8_IsSymLink(char const *utf8Name)   // IN: Path to test
{
   Bool result;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_IsSymLink(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_Exists --
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
FileUTF8_Exists(const char *utf8Name)   // IN
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_Exists(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_GetTimes --
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
FileUTF8_GetTimes(const char *utf8Name,       // IN
                  VmTimeType *createTime,     // OUT: Windows NT time format
                  VmTimeType *accessTime,     // OUT: Windows NT time format
                  VmTimeType *writeTime,      // OUT: Windows NT time format
                  VmTimeType *attrChangeTime) // OUT: Windows NT time format
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *)utf8Name;
#endif

   result = File_GetTimes(localName,
                          createTime,
                          accessTime,
                          writeTime,
                          attrChangeTime);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_SetTimes --
 *
 *      Set the date and time that a file was created, last accessed, or
 *      last modified.
 *
 * Results:
 *      TRUE if succeed or FALSE if error.
 *      
 * Side effects:
 *      If utf8Name is a symlink, target's timestamps will be updated.
 *      Symlink itself's timestamps will not be changed.
 *
 *----------------------------------------------------------------------
 */

Bool
FileUTF8_SetTimes(const char *utf8Name,       // IN
                  VmTimeType createTime,      // IN: ignored
                  VmTimeType accessTime,      // IN: Windows NT time format
                  VmTimeType writeTime,       // IN: Windows NT time format
                  VmTimeType attrChangeTime)  // IN: ignored
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_SetTimes(localName, createTime, accessTime, writeTime, attrChangeTime);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FileUTF8_DeleteDirectoryTree --
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
FileUTF8_DeleteDirectoryTree(char const *utf8Name) // IN: directory to delete
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              &localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_DeleteDirectoryTree(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileUTF8_CreateDirectoryHierarchy --
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
FileUTF8_CreateDirectoryHierarchy(char const *utf8Name)  // IN
{
   Bool result = FALSE;
   char *localName = NULL;

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   if (!CodeSet_Utf8ToCurrent(utf8Name,
                              strlen(utf8Name),
                              (char **)&localName,
                              NULL)) {
      return FALSE;
   }
#else
   localName = (char *) utf8Name;
#endif

   result = File_CreateDirectoryHierarchy(localName);

#if CONVERT_STRINGS_FROM_UTF8_TO_LOCAL
   free(localName);
#endif

   return result;
}
