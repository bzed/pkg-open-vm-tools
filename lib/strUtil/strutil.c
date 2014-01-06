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
 * strutil.c --
 *
 *    String utility functions.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmware.h"
#include "strutil.h"
#include "str.h"



/*
 *-----------------------------------------------------------------------------
 *
 * StrUtil_GetNextToken --
 *
 *      Get the next token from a string after a given index w/o modifying the
 *      original string.
 *
 * Results:
 *      An allocated, NUL-terminated string containing the token. 'index' is
 *         updated to point after the returned token
 *      NULL if no tokens are left
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

char *
StrUtil_GetNextToken(unsigned int *index,    // IN/OUT: Index to start at
                     const char *str,        // IN    : String to parse
                     const char *delimiters) // IN    : Chars separating tokens
{
   unsigned int startIndex;
   unsigned int length;
   char *token;

   ASSERT(index);
   ASSERT(str);
   ASSERT(delimiters);
   ASSERT(*index <= strlen(str));

#define NOT_DELIMITER (Str_Strchr(delimiters, str[*index]) == NULL)

   /* Skip leading delimiters. */
   for (; ; (*index)++) {
      if (str[*index] == '\0') {
         return NULL;
      }

      if (NOT_DELIMITER) {
         break;
      }
   }
   startIndex = *index;

   /*
    * Walk the string until we reach the end of it, or we find a
    * delimiter.
    */
   for ((*index)++; str[*index] != '\0' && NOT_DELIMITER; (*index)++) {
   }

#undef NOT_DELIMITER

   length = *index - startIndex;
   ASSERT(length);
   token = (char *)malloc(length + 1 /* NUL */);
   ASSERT_MEM_ALLOC(token);
   memcpy(token, str + startIndex, length);
   token[length] = '\0';

   return token;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StrUtil_GetNextIntToken --
 *
 *      Acts like StrUtil_GetNextToken except it returns an int32.
 *
 * Results:
 *      TRUE if a valid int was parsed and 'out' contains the parsed int.
 *      FALSE otherwise. Contents of 'out' are undefined.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
StrUtil_GetNextIntToken(int32 *out,             // OUT   : parsed int
                        unsigned int *index,    // IN/OUT: Index to start at
                        const char *str,        // IN    : String to parse
                        const char *delimiters) // IN    : Chars separating tokens
{
   char *resultStr;
   Bool valid = FALSE;

   ASSERT(out);
   ASSERT(index);
   ASSERT(str);
   ASSERT(delimiters);

   resultStr = StrUtil_GetNextToken(index, str, delimiters);
   if (resultStr == NULL) {
      return FALSE;
   }

   valid = StrUtil_StrToInt(out, resultStr);
   free(resultStr);

   return valid;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StrUtil_GetNextUintToken --
 *
 *      Acts like StrUtil_GetNextIntToken except it returns an uint32.
 *
 * Results:
 *      TRUE if a valid int was parsed and 'out' contains the parsed int.
 *      FALSE otherwise. Contents of 'out' are undefined.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
StrUtil_GetNextUintToken(uint32 *out,            // OUT   : parsed int
                         unsigned int *index,    // IN/OUT: Index to start at
                         const char *str,        // IN    : String to parse
                         const char *delimiters) // IN    : Chars separating tokens
{
   char *resultStr;
   Bool valid = FALSE;

   ASSERT(out);
   ASSERT(index);
   ASSERT(str);
   ASSERT(delimiters);

   resultStr = StrUtil_GetNextToken(index, str, delimiters);
   if (resultStr == NULL) {
      return FALSE;
   }

   valid = StrUtil_StrToUint(out, resultStr);
   free(resultStr);

   return valid;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StrUtil_GetNextInt64Token --
 *
 *      Acts like StrUtil_GetNextToken except it returns an int64.
 *
 * Results:
 *      TRUE on a successful retrieval. FALSE otherwise.
 *      Token is stored in 'out', which is left undefined in the FALSE case.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
StrUtil_GetNextInt64Token(int64 *out,          // OUT: The output value
                          unsigned int *index,    // IN/OUT: Index to start at
                          const char *str,        // IN    : String to parse
                          const char *delimiters) // IN    : Chars separating tokens
{
   char *resultStr;
   Bool result;

   ASSERT(out);
   ASSERT(index);
   ASSERT(str);
   ASSERT(delimiters);

   resultStr = StrUtil_GetNextToken(index, str, delimiters);
   result = resultStr ? StrUtil_StrToInt64(out, resultStr) : FALSE;
   free(resultStr);

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StrUtil_StrToInt --
 *
 *      Convert a string into an integer.
 *
 * Results:
 *      TRUE if the conversion was successful and 'out' contains the converted
 *      result.
 *      FALSE otherwise. 'out' is undefined.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
StrUtil_StrToInt(int32 *out,      // OUT
                 const char *str) // IN : String to parse
{
   char *ptr;

   ASSERT(out);
   ASSERT(str);

   errno = 0;

   *out = (int32)strtol(str, &ptr, 0);
   
   return ptr[0] == '\0' && errno != ERANGE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StrUtil_StrToUint --
 *
 *      Convert a string into unsigned integer.
 *
 * Results:
 *      TRUE if the conversion succeeded and 'out' contains the result.
 *      FALSE otherwise. 'out' is undefined.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
StrUtil_StrToUint(uint32 *out,     // OUT
                  const char *str) // IN : String to parse
{
   char *ptr;

   ASSERT(out);
   ASSERT(str);

   errno = 0;

   *out = (uint32)strtoul(str, &ptr, 0);

   return *ptr == '\0' && errno != ERANGE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * StrUtil_StrToInt64 --
 *
 *      Convert a string into a 64bit integer.
 *
 * Results:
 *      TRUE if conversion was successful, FALSE otherwise.
 *      Value is stored in 'out', which is left undefined in the FALSE case.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
StrUtil_StrToInt64(int64 *out,   // OUT: The output value
                   const char *str) // IN : String to parse
{
   char *ptr;

   ASSERT(out);
   ASSERT(str);

   errno = 0;

#if defined(_WIN32)
   *out= _strtoi64(str, &ptr, 0);
#elif defined(__FreeBSD__)
   *out= strtoq(str, &ptr, 0);
#elif defined(N_PLAT_NLM)
   /* Works for small values of str... */
   *out= (int64)strtol(str, &ptr, 0);
#else
   *out= strtoll(str, &ptr, 0);
#endif

   return ptr[0] == '\0' && errno != ERANGE;
}

/*
 *----------------------------------------------------------------------
 *
 * StrUtil_GetLongestLineLength --
 *
 *      Given a buffer with one or more lines
 *      this function computes the length of the
 *      longest line in a buffer.
 *
 * Results:
 *      Returns the length of the longest line in the 'buf'.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

size_t
StrUtil_GetLongestLineLength(const char *buf,   //IN
                             size_t bufLength)  //IN
{
    size_t longest = 0;

    while (bufLength) {
       const char* next;
       size_t len;

       next = memchr(buf, '\n', bufLength);
       if (next) {
          next++;
          len = next - buf + 1;
       } else {
          len = bufLength;
       }
       if (len > longest) {
          longest = len;
       }
       bufLength -= len;
       buf = next;
    }
    return longest;
}


