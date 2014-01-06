/* **************************************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. 
 * **************************************************************************
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
 * err.c --
 *
 *      General error handling library
 *
 */

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#  include "win32util.h"
#endif

#include "str.h"
#include "err.h"


/*
 *----------------------------------------------------------------------
 *
 * Err_Errno2String --
 *
 *      Returns a string that corresponds to the passed error number.
 *
 * Results:
 *      Error message string.
 *      
 * Side effects:
 *      None.
 *
 * The result should be printed or copied before calling again.
 *
 *----------------------------------------------------------------------
 */

const char *
Err_Errno2String(Err_Number errorNumber)
{
#if !_WIN32
   return strerror(errorNumber);
#else
   static char buf[2048];
   char *p;

   if (FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |     
                     FORMAT_MESSAGE_IGNORE_INSERTS,    
                     NULL,
                     errorNumber,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     buf, sizeof buf,
                     NULL) == 0) {
      Str_Sprintf(buf, sizeof buf, "Unknown error %d (0x%x)\n",
                  errorNumber, errorNumber);
   }

   /*
    * Squash trailing CR-LF and period, if any, for consistency with linux.
    */

   for (p = buf + strlen(buf);
	p > buf && (p[-1] == '\n' || p[-1] == '\r');
	p--) {
   }
   if (p > buf && p[-1] == '.') {
      p--;
   }
   *p = 0;

   return buf;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Err_ErrString --
 *
 *      Returns a string that corresponds to the last error message.
 *
 * Results:
 *      Error message string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Err_ErrString(void)
{
   return Err_Errno2String(Err_Errno());
}
