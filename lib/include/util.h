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
 * util.h --
 *
 *    misc util functions
 */

#ifndef UTIL_H
#define UTIL_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
   #ifdef USERLEVEL
      #include <tchar.h>   /* Needed for MBCS string functions */
      #include <windows.h> /* for definition of HANDLE */
   #endif
#else
   #include <sys/types.h>
#endif

#ifdef __APPLE__
   #include <IOKit/IOTypes.h>
   #include <CoreFoundation/CFNumber.h>
   #include <CoreFoundation/CFDictionary.h>
#endif

#include "vm_basic_types.h"
#include "vm_assert.h"


#ifdef __APPLE__
/* Types used for expressing a dictionary <key, value> tuple. */
typedef enum ValueType {
   VALUETYPE_ASCIISTRING,
   VALUETYPE_NUMBER,
   VALUETYPE_BOOLEAN
} ValueType;

typedef struct NumberType {
   CFNumberType type;
   const void *numptr;
} NumberType;

typedef struct DictItem {
   const char *key;
   ValueType type;
   union {
      const char *str;
      NumberType num;
      Bool boolVal;
   } u;
} DictItem;

EXTERN char * Util_IORegGetStringProperty(io_object_t entry, CFStringRef property);
EXTERN Bool Util_IORegGetNumberProperty(io_object_t entry, CFStringRef property,
                                        CFNumberType type, void *val);
EXTERN Bool Util_IORegGetBooleanProperty(io_object_t entry, CFStringRef property,
                                         Bool *boolVal);
EXTERN CFMutableDictionaryRef Util_CreateDictFromList(const DictItem *list, int cnt);
EXTERN io_iterator_t Util_IORegGetIter(const char *key, const char *val);
EXTERN io_object_t Util_IORegGetDeviceObjectByName(const char *deviceName);
EXTERN char * Util_GetBSDName(const char *deviceName);
EXTERN char * Util_IORegGetDriveType(const char *deviceName);
#endif // __APPLE__


EXTERN uint32 CRC_Compute(uint8 *buf, int len);
EXTERN uint32 Util_Checksum32(uint32 *buf, int len);
EXTERN uint32 Util_Checksum(uint8 *buf, int len);
EXTERN uint32 Util_Checksumv(void *iov, int numEntries);
EXTERN void Util_ShortenPath(char *dst, const char *src, int maxLen);
EXTERN char *Util_ExpandString(const char *fileName);
EXTERN void Util_ExitThread(int);
EXTERN NORETURN void Util_ExitProcessAbruptly(int);
EXTERN int Util_HasAdminPriv(void);
#if defined _WIN32 && defined USERLEVEL
EXTERN int Util_TokenHasAdminPriv(HANDLE token);
EXTERN int Util_TokenHasInteractPriv(HANDLE token);
#endif
EXTERN Bool Util_Data2Buffer(char *buf, size_t bufSize, const void *data0,
                             size_t dataSize);
EXTERN char *Util_GetCanonicalPath(const char *path);
#ifdef _WIN32
EXTERN char *Util_GetLowerCaseCanonicalPath(const char *path);
#endif
EXTERN Bool Util_CanonicalPathsIdentical(const char *path1, const char *path2);
EXTERN Bool Util_IsAbsolutePath(const char *path);
EXTERN unsigned Util_GetPrime(unsigned n0);
EXTERN uint32 Util_GetCurrentThreadId(void);

EXTERN char *Util_DeriveFileName(const char *source,
                                 const char *name,
                                 const char *ext);

EXTERN char *Util_CombineStrings(char **sources, int count);
EXTERN char **Util_SeparateStrings(char *source, int *count);

EXTERN char *Util_GetSafeTmpDir(Bool useConf);
EXTERN int Util_MakeSafeTemp(const char *tag, char **presult);

#if defined(__linux__) || defined(__FreeBSD__) || defined(sun)
EXTERN Bool Util_GetProcessName(pid_t pid, char *bufOut, size_t bufOutSize);
#endif

// backtrace functions and utilities

#define UTIL_BACKTRACE_LINE_LEN (255)
typedef void (*Util_OutputFunc)(void *data, const char *fmt, ...);

void Util_Backtrace(int bugNr);
void Util_BacktraceFromPointer(uintptr_t *basePtr);
void Util_BacktraceFromPointerWithFunc(uintptr_t *basePtr,
                                       Util_OutputFunc outFunc,
                                       void *outFuncData);
void Util_BacktraceWithFunc(int bugNr,
                            Util_OutputFunc outFunc,
                            void *outFuncData);
void Util_LogWrapper(void *ignored, const char *fmt, ...);

int Util_CompareDotted(const char *s1, const char *s2);



/*
 * In util_shared.h
 */
EXTERN Bool Util_Throttle(uint32 count);

/*
 *----------------------------------------------------------------------
 *
 * Util_BufferIsEmpty --
 *
 *    Determine wether or not the buffer of 'len' bytes starting at 'base' is
 *    empty (i.e. full of zeroes)
 *
 * Results:
 *    TRUE if yes
 *    FALSE if no
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

static INLINE Bool Util_BufferIsEmpty(void const *base, // IN
                                      size_t len)       // IN
{
   uint32 const *p32;
   uint32 const *e32;
   uint16 const *p16;

   ASSERT_ON_COMPILE(sizeof(uint32) == 4);

   p32 = (uint32 const *)base;
   e32 = p32 + len / 4;
   for (; p32 < e32; p32++) {
      if (*p32) {
         return FALSE;
      }
   }

   len &= 0x3;
   p16 = (uint16 const *)p32;

   if (len & 0x2) {
      if (*p16) {
         return FALSE;
      }

      p16++;
   }

   if (   len & 0x1
       && *(uint8 const *)p16) {
      return FALSE;
   }

   return TRUE;
};


EXTERN Bool Util_MakeSureDirExistsAndAccessible(char const *path,
						unsigned int mode);

#ifdef N_PLAT_NLM
#   define DIRSEPS	      "\\"
#   define DIRSEPC	      '\\'
#   define VALID_DIRSEPS      "\\/:"
#elif _WIN32
#   define DIRSEPS	      "\\"
#   define DIRSEPC	      '\\'
#   define VALID_DIRSEPS   "\\/"
#else
#   define DIRSEPS	      "/"
#   define DIRSEPC	      '/'
#endif


/*
 *-----------------------------------------------------------------------
 *
 * Util_Safe[Malloc, Realloc, Calloc, Strdup] and
 * Util_Safe[Malloc, Realloc, Calloc, Strdup]Bug --
 *
 *      These functions work just like the standard C library functions
 *      (except Util_SafeStrdup[,Bug]() accept NULL, see below),
 *      but will not fail. Instead they Panic(), printing the file and
 *      line number of the caller, if the underlying library function
 *      fails.  The Util_SafeFnBug functions print bugNumber in the
 *      Panic() message.
 *
 *      These functions should only be used when there is no way to
 *      gracefully recover from the error condition.
 *
 *      The internal versions of these functions expect a bug number
 *      as the first argument.  If that bug number is something other
 *      than -1, the panic message will include the bug number.
 *
 *      Since Util_SafeStrdup[,Bug]() do not need to return NULL
 *      on error, they have been extended to accept the null pointer
 *      (and return it).  The competing view is that they should
 *      panic on NULL.  This is a convenience vs. strictness argument.
 *      Convenience wins.  -- edward
 *
 * Results:
 *      The freshly allocated memory.
 *
 * Side effects:
 *      Panic() if the library function fails.
 *
 *--------------------------------------------------------------------------
 */

#define Util_SafeMalloc(_size) \
   UtilSafeMallocInternal(-1, (_size), __FILE__, __LINE__)

#define Util_SafeMallocBug(_bugNr, _size) \
   UtilSafeMallocInternal((_bugNr), (_size), __FILE__, __LINE__)

#define Util_SafeRealloc(_ptr, _size) \
   UtilSafeReallocInternal(-1, (_ptr), (_size), __FILE__, __LINE__)

#define Util_SafeReallocBug(_bugNr, _ptr, _size) \
   UtilSafeReallocInternal((_bugNr), (_ptr), (_size), __FILE__, __LINE__)

#define Util_SafeCalloc(_nmemb, _size) \
   UtilSafeCallocInternal(-1, (_nmemb), (_size), __FILE__, __LINE__)

#define Util_SafeCallocBug(_bugNr, _nmemb, _size) \
   UtilSafeCallocInternal((_bugNr), (_nmemb), (_size), __FILE__, __LINE__)

#define Util_SafeStrndup(_str, _size) \
   UtilSafeStrndupInternal(-1, (_str), (_size), __FILE__, __LINE__)

#define Util_SafeStrndupBug(_bugNr, _str, _size) \
   UtilSafeStrndupInternal((_bugNr), (_str), (_size), __FILE__, __LINE__)

#define Util_SafeStrdup(_str) \
   UtilSafeStrdupInternal(-1, (_str), __FILE__, __LINE__)

#define Util_SafeStrdupBug(_bugNr, _str) \
   UtilSafeStrdupInternal((_bugNr), (_str), __FILE__, __LINE__)

static INLINE void *
UtilSafeMallocInternal(int bugNumber, size_t size, char *file, int lineno)
{
   void *result = malloc(size);

   if (result == NULL) {
      if (bugNumber == -1) {
         Panic("Unrecoverable memory allocation failure at %s:%d\n",
               file, lineno);
      } else {
         Panic("Unrecoverable memory allocation failure at %s:%d.  Bug "
               "number: %d\n", file, lineno, bugNumber);
      }
   }
   return result;
}

static INLINE void *
UtilSafeReallocInternal(int bugNumber, void *ptr, size_t size,
                        char *file, int lineno)
{
   void *result = realloc(ptr, size);

   if (result == NULL && size != 0) {
      if (bugNumber == -1) {
         Panic("Unrecoverable memory allocation failure at %s:%d\n",
               file, lineno);
      } else {
         Panic("Unrecoverable memory allocation failure at %s:%d.  Bug "
               "number: %d\n", file, lineno, bugNumber);
      }
   }
   return result;
}

static INLINE void *
UtilSafeCallocInternal(int bugNumber, size_t nmemb, size_t size,
                       char *file, int lineno)
{
   void *result = calloc(nmemb, size);

   if (result == NULL) {
      if (bugNumber == -1) {
         Panic("Unrecoverable memory allocation failure at %s:%d\n",
               file, lineno);
      } else {
         Panic("Unrecoverable memory allocation failure at %s:%d.  Bug "
               "number: %d\n", file, lineno, bugNumber);
      }
   }
   return result;
}

#ifdef VMX86_SERVER
#if __GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ > 1)
// XXX Prevents an "inlining failed" warning in vmkproxy.c.  Ugh.
static INLINE char *
UtilSafeStrdupInternal(int bugNumber, const char *s, char *file,
                       int lineno) __attribute__((always_inline));
#endif
#endif

static INLINE char *
UtilSafeStrdupInternal(int bugNumber, const char *s, char *file,
                       int lineno)
{
   char *result;

   if (s == NULL) {
      return NULL;
   }
   if ((result = strdup(s)) == NULL) {
      if (bugNumber == -1) {
         Panic("Unrecoverable memory allocation failure at %s:%d\n",
               file, lineno);
      } else {
         Panic("Unrecoverable memory allocation failure at %s:%d.  Bug "
               "number: %d\n", file, lineno, bugNumber);
      }
   }
   return result;
}

/*
 *-----------------------------------------------------------------------------
 *
 * UtilSafeStrndupInternal --
 *
 *      Returns a string consisting of first n characters of 's' if 's' has
 *      length >= 'n', otherwise returns a string duplicate of 's'.
 *
 * Results:
 *      Pointer to the duplicated string.
 *
 * Side effects:
 *      May Panic if ran out of memory.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE char *
UtilSafeStrndupInternal(int bugNumber,    // IN
                        const char *s,    // IN
                        size_t n,         // IN
			char *file,       // IN
                        int lineno)       // IN
{
   size_t size;
   char *copy;
   const char *null;

   if (s == NULL) {
      return NULL;
   }

   null = (char *)memchr(s, '\0', n);
   size = null ? null - s: n;
   copy = (char *)malloc(size + 1);

   if (copy == NULL) {
      if (bugNumber == -1) {
         Panic("Unrecoverable memory allocation failure at %s:%d\n",
               file, lineno);
      } else {
         Panic("Unrecoverable memory allocation failure at %s:%d.  Bug "
               "number: %d\n", file, lineno, bugNumber);
      }
   }

   copy[size] = '\0';
   return (char *)memcpy(copy, s, size);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_Zero --
 *
 *      Zeros out bufSize bytes of buf. NULL is legal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See above.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
Util_Zero(void *buf,       // OUT
          size_t bufSize)  // IN 
{
   if (buf != NULL) {
      memset(buf, 0, bufSize);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_ZeroString --
 *
 *      Zeros out a NULL-terminated string. NULL is legal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See above.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
Util_ZeroString(char *str)  // IN
{
   if (str != NULL) {
      memset(str, 0, strlen(str));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_ZeroFree --
 *
 *      Zeros out bufSize bytes of buf, and then frees it. NULL is
 *      legal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	buf is zeroed, and then free() is called on it.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
Util_ZeroFree(void *buf,       // OUT
              size_t bufSize)  // IN 
{
   if (buf != NULL) {
      memset(buf, 0, bufSize);
      free(buf); 
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_ZeroFreeString --
 *
 *      Zeros out a NULL-terminated string, and then frees it. NULL is
 *      legal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	str is zeroed, and then free() is called on it.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
Util_ZeroFreeString(char *str)  // IN
{
   if (str != NULL) {
      Util_Zero(str, strlen(str));
      free(str); 
   }
}

#endif /* UTIL_H */