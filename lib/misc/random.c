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
 * random.c --
 *
 *    Random bits generation. --hpreg
 */


#ifdef _WIN32
#   include <windows.h>
#   include <wincrypt.h>
#else
#   include <errno.h>
#   include <fcntl.h>
#   include <unistd.h>
#endif

#include "vmware.h"
#include "random.h"


/*
 *-----------------------------------------------------------------------------
 *
 * Random_Crypto --
 *
 *      Generate 'size' bytes of cryptographically strong random bits in
 *      'buffer'. Use this function when you need non-predictable random
 *      bits, typically in security applications. Otherwise use
 *      rand(3). --hpreg
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Random_Crypto(unsigned int size, // IN
              void *buffer)      // OUT
{
#if defined(_WIN32)
   HCRYPTPROV csp;
   DWORD error;

   if (CryptAcquireContext(&csp, NULL, NULL, PROV_RSA_FULL,
                           CRYPT_VERIFYCONTEXT) == FALSE) {
      error = GetLastError();
      Log("Random_Crypto: CryptAcquireContext failed %d\n", error);
      return FALSE;
   }

   if (CryptGenRandom(csp, size, buffer) == FALSE) {
      CryptReleaseContext(csp, 0);
      error = GetLastError();
      Log("Random_Crypto: CryptGenRandom failed %d\n", error);
      return FALSE;
   }

   if (CryptReleaseContext(csp, 0) == FALSE) {
      error = GetLastError();
      Log("Random_Crypto: CryptReleaseContext failed %d\n", error);
      return FALSE;
   }
#else
   int fd;
   int error;

   /*
    * We use /dev/urandom and not /dev/random because it is good enough and
    * because it cannot block. --hpreg
    */
   fd = open("/dev/urandom", O_RDONLY);
   if (fd < 0) {
      error = errno;
      Log("Random_Crypto: Failed to open: %d\n", error);
      return FALSE;
   }

   /* Although /dev/urandom does not block, it can return short reads. */
   while (size > 0) {
      ssize_t bytesRead = read(fd, buffer, size);
      if (bytesRead == 0 || (bytesRead == -1 && errno != EINTR)) {
         error = errno;
         close(fd);
         Log("Random_Crypto: Short read: %d\n", error);
         return FALSE;
      }
      if (bytesRead > 0) {
         size -= bytesRead;
         buffer = ((uint8 *) buffer) + bytesRead; 
      }
   }

   if (close(fd) < 0) {
      error = errno;
      Log("Random_Crypto: Failed to close: %d\n", error);
      return FALSE;
   }
#endif

   return TRUE;
}
