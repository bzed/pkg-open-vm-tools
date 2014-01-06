/* **********************************************************
 * Copyright 2006 VMware, Inc.  All rights reserved. - 
 * *********************************************************
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
 * fileUTF8.h --
 *
 * Interface to host file system and related utility functions.
 * These are the same as the File_*** procedures except these accept
 * and return all strings in UTF8 form. This means they will convert
 * between UTF8 and the local character set, which is needed by the tools
 * code.
 */

#ifndef _FILE_UTF8_H_
#define _FILE_UTF8_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "fileIO.h"


EXTERN Bool FileUTF8_Copy(const char *utf8SrcFile, const char *utf8DstFile);

EXTERN Bool FileUTF8_Rename(const char *utf8OldFile, const char *utf8NewFile);

EXTERN Bool FileUTF8_CreateDirectory(char const *utf8Name);

EXTERN Bool FileUTF8_CreateDirectoryHierarchy(char const *utf8Name);

EXTERN Bool FileUTF8_DeleteEmptyDirectory(char const *utf8Name);

EXTERN int FileUTF8_ListDirectory(char const *utf8Name, char ***ids);

EXTERN int FileUTF8_UnlinkIfExists(const char *name);

EXTERN Bool FileUTF8_IsDirectory(const char *utf8Name);

EXTERN Bool FileUTF8_IsFile(const char *utf8Name);

EXTERN Bool FileUTF8_IsSymLink(char const *utf8Name);

EXTERN Bool FileUTF8_Exists(const char *name);

EXTERN Bool FileUTF8_SetTimes(const char *fileName,
                              VmTimeType createTime,      // IN: Windows NT time format
                              VmTimeType accessTime,      // IN: Windows NT time format
                              VmTimeType writeTime,       // IN: Windows NT time format
                              VmTimeType attrChangeTime); // IN: ignored

EXTERN Bool FileUTF8_DeleteDirectoryTree(char const *utf8Name);

EXTERN int FileUTF8_MakeTemp(const char *tag, char **presult);

EXTERN int FileUTF8_MakeTempEx(const char *utf8Dir,
                               const char *utf8FileName,
                               char **presult);

#endif // ifndef _FILE_UTF8_H_



