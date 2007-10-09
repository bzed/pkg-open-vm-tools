/* **********************************************************
 * Copyright (C) 2005 VMware, Inc. All Rights Reserved 
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
 * cpNameUtil.c
 *
 *    Common implementations of CPName utility functions.
 */


/* Some of the headers below cannot be included in driver code */
#ifndef __KERNEL__

#include "cpNameUtil.h"
#include "hgfsServerPolicy.h"
#include "hgfsVirtualDir.h"
#include "util.h"
#include "vm_assert.h"
#include "str.h"

#define WIN_DIRSEPC     '\\'
#define WIN_DIRSEPS     "\\"


/*
 *----------------------------------------------------------------------------
 *
 * CPNameUtil_Strrchr --
 *
 *    Performs strrchr(3) on a CPName path.
 *
 * Results:
 *    Pointer to last occurrence of searchChar in cpNameIn if found, NULL if
 *    not found.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

char *
CPNameUtil_Strrchr(char const *cpNameIn,       // IN: CPName path to search
                   size_t cpNameInSize,        // IN: Size of CPName path
                   char searchChar)            // IN: Character to search for
{
   ssize_t index;

   ASSERT(cpNameIn);
   ASSERT(cpNameInSize > 0);

   for (index = cpNameInSize - 1;
        cpNameIn[index] != searchChar && index >= 0;
        index--);

   return (index < 0) ? NULL : (char *)(cpNameIn + index);
}


/*
 *----------------------------------------------------------------------------
 *
 * CPNameUtil_LinuxConvertToRoot --
 *
 *    Performs CPName conversion and such that the result can be converted back
 *    to an absolute path (in the "root" share) by a Linux hgfs server.
 *
 *    Note that nameIn must contain an absolute path.
 *
 * Results:
 *    Size of the output buffer on success, negative value on error
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

int
CPNameUtil_LinuxConvertToRoot(char const *nameIn, // IN:  buf to convert
                              size_t bufOutSize,  // IN:  size of the output buffer
                              char *bufOut)       // OUT: output buffer
{
   const size_t shareNameSize = HGFS_STR_LEN(HGFS_SERVER_POLICY_ROOT_SHARE_NAME);

   int result;

   ASSERT(nameIn);
   ASSERT(bufOut);

   if (bufOutSize <= shareNameSize) {
      return -1;
   }

   /* Prepend the name of the "root" share directly in the output buffer */
   memcpy(bufOut, HGFS_SERVER_POLICY_ROOT_SHARE_NAME, shareNameSize);
   bufOut[shareNameSize] = '\0';

   result = CPName_LinuxConvertTo(nameIn, bufOutSize - shareNameSize - 1,
                                  bufOut + shareNameSize + 1);

   /* Return either the same error code or the correct size */
   return (result < 0) ? result : (int)(result + shareNameSize + 1);
}


/*
 *----------------------------------------------------------------------------
 *
 * CPNameUtil_WindowsConvertToRoot --
 *
 *    Performs CPName conversion and appends necessary strings ("root" and
 *    "drive"|"unc") so that the result can be converted back to an absolute
 *    path (in the "root" share) by a Windows hgfs server.
 *
 *    Note that nameIn must contain an absolute path.
 *
 * Results:
 *    Size of the output buffer on success, negative value on error
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

int
CPNameUtil_WindowsConvertToRoot(char const *nameIn, // IN:  buf to convert
                                size_t bufOutSize,  // IN:  size of the output buffer
                                char *bufOut)       // OUT: output buffer
{
   const char partialName[] = HGFS_SERVER_POLICY_ROOT_SHARE_NAME;
   const size_t partialNameLen = HGFS_STR_LEN(HGFS_SERVER_POLICY_ROOT_SHARE_NAME);
   const char *partialNameSuffix = "";
   size_t partialNameSuffixLen;
   char *fullName;
   size_t fullNameLen;
   size_t nameLen;
   int result;

   ASSERT(nameIn);
   ASSERT(bufOut);

   /*
    * Create the full name. Note that Str_Asprintf should not be
    * used here as it uses FormatMessages which interprets 'data', a UTF-8
    * string, as a string in the current locale giving wrong results.
    */

   /*
    * Is this file path a UNC path?
    */
   if (nameIn[0] == WIN_DIRSEPC && nameIn[1] == WIN_DIRSEPC) {
      partialNameSuffix    = WIN_DIRSEPS HGFS_UNC_DIR_NAME WIN_DIRSEPS;
      partialNameSuffixLen = HGFS_STR_LEN(WIN_DIRSEPS) +
                             HGFS_STR_LEN(HGFS_UNC_DIR_NAME) +
                             HGFS_STR_LEN(WIN_DIRSEPS);
   } else {
      partialNameSuffix    = WIN_DIRSEPS HGFS_DRIVE_DIR_NAME WIN_DIRSEPS;
      partialNameSuffixLen = HGFS_STR_LEN(WIN_DIRSEPS) +
                             HGFS_STR_LEN(HGFS_DRIVE_DIR_NAME) +
                             HGFS_STR_LEN(WIN_DIRSEPS);
   }

   /* Skip any path separators at the beginning of the input string */
   while (*nameIn == WIN_DIRSEPC) {
      nameIn++;
   }

   nameLen = strlen(nameIn);
   fullNameLen = partialNameLen + partialNameSuffixLen + nameLen;
   fullName = (char *)Util_SafeMalloc(fullNameLen + 1);

   memcpy(fullName, partialName, partialNameLen);
   memcpy(fullName + partialNameLen, partialNameSuffix, partialNameSuffixLen);
   memcpy(fullName + partialNameLen + partialNameSuffixLen, nameIn, nameLen);
   fullName[fullNameLen] = '\0';

   /* CPName_ConvertTo strips out the ':' character */
   result = CPName_WindowsConvertTo(fullName, bufOutSize, bufOut);
   free(fullName);

   return result;
}


#endif /* __KERNEL__ */