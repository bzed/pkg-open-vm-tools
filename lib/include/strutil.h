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
 * strutil.h --
 *
 *    String utility functions.
 */


#ifndef __STRUTIL_H__
#   define __STRUTIL_H__

#include "fileIO.h"

char * StrUtil_GetNextToken(unsigned int *index, const char *str,
                            const char *delimiters);
Bool StrUtil_GetNextIntToken(int32 *out, unsigned int *index, const char *str,
                             const char *delimiters);
Bool StrUtil_GetNextUintToken(uint32 *out, unsigned int *index, const char *str,
                              const char *delimiters);
Bool StrUtil_GetNextInt64Token(int64 *out, unsigned int *index, const char *str,
                              const char *delimiters);
Bool StrUtil_StrToInt(int32 *out, const char *str);
Bool StrUtil_StrToUint(uint32 *out, const char *str);
Bool StrUtil_StrToInt64(int64 *out, const char *str);
size_t StrUtil_GetLongestLineLength(const char *buf, size_t bufLength);

char **StrUtil_Split(const char *filename,
                     const char *delimiter);

char **StrUtil_Grep(const char *filename,
                    const char *search,
                    const char *delimiter);

char **StrUtil_GrepFd(FileIODescriptor *fd,
                      const char *search,
                      const char *delimiter);

void StrUtil_GrepFree(char **retval);

#endif /* __STRUTIL_H__ */
