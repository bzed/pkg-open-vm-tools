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
 * cpNameUtilLinux.c
 *
 *    Linux implementation of CPName utility functions.  These are not included
 *    in the main CPName API since they may require other calls on the
 *    resulting CPName to be from the same OS-type.
 */

/* Some of the headers in cpNameUtil.c cannot be included in driver code */
#ifndef __KERNEL__

#include "cpNameUtil.h"

/*
 *----------------------------------------------------------------------------
 *
 * CPNameUtil_ConvertToRoot --
 *
 *    Pass through function that calls Linux version of _ConvertToRoot().
 *
 *    Performs CPName conversion and such that the result can be converted back
 *    to an absolute path (in the "root" share) by a Linux hgfs server.
 *
 *    Note that nameIn must contain an absolute path.
 *
 * Results:
 *    Size of the output buffer on success, negative value on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

int
CPNameUtil_ConvertToRoot(char const *nameIn, // IN:  buf to convert
                         size_t bufOutSize,  // IN:  size of the output buffer
                         char *bufOut)       // OUT: output buffer
{
   return CPNameUtil_LinuxConvertToRoot(nameIn, bufOutSize, bufOut);
}

#endif /* __KERNEL__ */
