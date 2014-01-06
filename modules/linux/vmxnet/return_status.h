/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. 
 * **********************************************************
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
 * return_status.h --
 *
 *      VMkernel return status codes.
 *
 */

#ifndef _RETURN_STATUS_H_
#define _RETURN_STATUS_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMNIXMOD
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMKDRIVERS
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vmkapi_status.h"

/*
 * vmkernel error codes and translation to Unix error codes
 *
 * The table defined in vmkapi_status.h gives the name, description,
 * and corresponding Unix error code for each VMK error code.  The
 * Unix error code is used when a VMK error propagates up to a user
 * world through the Linux-compatible system call interface and we
 * need to translate it.
 *
 * There is also a mechanism to wrap a Linux error code opaquely
 * inside a VMK error code.  When the COS proxy generates an error, it
 * starts out as a Linux error code in a COS process, propagates into
 * the vmkernel where it needs to be translated to a VMK error code,
 * and then goes out to a user world where it needs to be a Unix error
 * code again.  The vmkernel does not have to understand these errors
 * other than to know that a nonzero value is an error, so we make
 * them opaque for simplicity.  The COS proxy calls
 * VMK_WrapLinuxError, which adds the absolute value of (nonzero)
 * Linux error codes to VMK_GENERIC_LINUX_ERROR.  User_TranslateStatus
 * undoes this transformation on the way out.
 *
 * XXX Currently there is no need to translate VMK error codes to BSD
 * error codes, but the macros used with this table could be easily
 * extended to do so.  We do translate BSD error codes to VMK error
 * codes in vmkernel/networking/lib/support.c, using a case statement.
 * See PR 35564 for comments on how this could be improved.
 *
 * VMK_FAILURE and VMK_GENERIC_LINUX_ERROR must be at the start and
 * end, and must be defined with specific values using
 * DEFINE_VMK_ERR_AT; see return_status.c.
 *
 * All the values should be positive because we return these directly as
 * _vmnix call return values (at least for sysinfo).  A negative value
 * there could get interpretted as a linux error code.
 *
 */

#define LINUX_OK   0
#define FREEBSD_OK 0

/*
 * operations
 */

extern const char *VMK_ReturnStatusToString(VMK_ReturnStatus status);

/*
 *----------------------------------------------------------------------
 *
 * VMK_WrapLinuxError --
 *
 *      Wrap a Linux errno value inside a VMK_ReturnStatus value.  The
 *      status value is opaque to the vmkernel, except that 0 (no
 *      error) is guaranteed to translate to VMK_OK.  This routine is
 *      for use by the COS proxy to pass errors back through the
 *      vmkernel to a user world.
 *
 *      This is a macro instead of a static inline because
 *      return_status.h gets #included both from places where "INLINE"
 *      is not defined and from places where "inline" is wrong.  Ugh.
 *
 * Results:
 *      Opaque VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#define VMK_WrapLinuxError(error) \
   ((error) == 0 ? VMK_OK : \
    (error) <  0 ? VMK_GENERIC_LINUX_ERROR - (error) : \
                   VMK_GENERIC_LINUX_ERROR + (error))

#endif
