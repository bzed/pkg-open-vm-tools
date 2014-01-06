/* ************************************************************************
 * Copyright 2006 VMware, Inc.  All rights reserved. 
 * ***********************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

/*
 * kernelStubs.h
 *
 * This header externs a lot of userspace functions that should be
 * implemented in terms of kernel functions in order to use
 * userspace code in a kernel.
 */

#ifndef __KERNELSTUBS_H__
#define __KERNELSTUBS_H__

#ifdef linux
#include "vm_basic_types.h"
#include "driver-config.h"
#include <linux/kernel.h>
#include <linux/string.h>
#elif defined(_WIN32)
#include "vm_basic_types.h"
#include <ntddk.h>   /* kernel memory APIs */
#include <stdio.h>   /* for _vsnprintf, vsprintf */
#include <stdarg.h>  /* for va_start stuff */
#include <stdlib.h>  /* for min macro. */
#include "vm_assert.h"  /* Our assert macros */
#endif /* _WIN32 */


#ifdef linux
char *strdup(const char *source);

void *malloc(size_t size);
void free(void *mem);
void *calloc(size_t num, size_t len);
void *realloc(void *ptr, size_t newSize);

#elif defined(_WIN32)

#if (_WIN32_WINNT == 0x0400)
/* The following declarations are missing on NT4. */
typedef unsigned int UINT_PTR;
typedef unsigned int SIZE_T;

/* No free with tag availaible on NT4 kernel! */
#define KRNL_STUBS_FREE(P,T)     ExFreePool((P))

#else /* _WIN32_WINNT */
#define KRNL_STUBS_FREE(P,T)     ExFreePoolWithTag((P),(T))
/* Win 2K and later useful kernel function, documented but not declared! */
NTKERNELAPI VOID ExFreePoolWithTag(IN PVOID  P, IN ULONG  Tag);
#endif /* _WIN32_WINNT */

#endif /* _WIN32 */

/*
 * Stub functions we provide.
 */
void Panic(const char *fmt, ...);
uint64 System_Uptime(void);

char *Str_Strcpy(char *buf, const char *src, size_t maxSize);
int Str_Vsnprintf(char *str, size_t size, const char *format,
                  va_list arguments);
char *Str_Vasprintf(size_t *length, const char *format, 
                    va_list arguments);
char *Str_Asprintf(size_t *length, const char *Format, ...);
char *StrUtil_GetNextToken(unsigned int *index,
                           const char *str,
                           const char *delimiters);

/*
 * Functions the driver must implement for the stubs.
 */
EXTERN void Debug(const char *fmt, ...);


#endif /* __KERNELSTUBS_H__ */
