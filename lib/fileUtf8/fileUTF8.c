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
 * fileUTF8.c --
 *
 *      Interface to host-specific file functions for Posix hosts
 *
 */

#include "vmware.h"
#include "file.h"
#include "util.h"
#include "str.h"
#include "dynbuf.h"
#include "codeset.h"
#include "fileUTF8.h"


/*
 *----------------------------------------------------------------------
 *
 *  FileUTF8_MakeTemp --
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
FileUTF8_MakeTemp(const char *utf8Tag,  // IN (OPT)
                  char **presult)       // OUT
{
   int result = 0;
   char *localTag = NULL;
   char *localResult = NULL;
   size_t resultLength;

   if (NULL != presult) {
       *presult = NULL;
   }

   if ((NULL != utf8Tag) && !CodeSet_Utf8ToCurrent(utf8Tag,
                              strlen(utf8Tag),
                              (char **)&localTag,
                              NULL)) {
      result = -1;
      goto abort;
   }

   result = File_MakeTemp(localTag, &localResult);

   if ((-1 != result) && (NULL != presult)) {
      if (!CodeSet_CurrentToUtf8(localResult,
                                 strlen(localResult),
                                 presult,
                                 &resultLength)) {
         result = -1;
         goto abort;
      }
   }

abort:
   free(localTag);
   free(localResult);

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 *  FileUTF8_MakeTempEx --
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
FileUTF8_MakeTempEx(const char *utf8Dir,       // IN
                    const char *utf8FileName,  // IN
                    char **presult)            // OUT
{
   int result = 0;
   char *localDir = NULL;
   char *localFileName = NULL;
   char *localResult = NULL;
   size_t resultLength;

   if (NULL != presult) {
       *presult = NULL;
   }

   if ((NULL != utf8Dir) && !CodeSet_Utf8ToCurrent(utf8Dir,
                                                   strlen(utf8Dir),
                                                   (char **)&localDir,
                                                   NULL)) {
      result = -1;
      goto abort;
   }

   if ((NULL != utf8FileName) && !CodeSet_Utf8ToCurrent(utf8FileName,
                                                        strlen(utf8FileName),
                                                        (char **)&localFileName,
                                                        NULL)) {
      result = -1;
      goto abort;
   }

   result = File_MakeTempEx(localDir, localFileName, &localResult);

   if ((-1 != result) && (NULL != presult)) {
      if (!CodeSet_CurrentToUtf8(localResult,
                                 strlen(localResult),
                                 presult,
                                 &resultLength)) {
         result = -1;
         goto abort;
      }
   }

abort:
   free(localDir);
   free(localFileName);
   free(localResult);

   return result;
}
