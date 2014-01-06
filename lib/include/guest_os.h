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

#ifndef _GUEST_OS_H_
#define _GUEST_OS_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

/*
 * There's a max of 64 guests that can be defined in this list below.
 * Be conservative and only declare entries in this list if you need to refer
 * to the guest specifically in vmx/main/guest_os.c,
 * vmcore/vmx/main/monitorControl.c, or similar. Don't rely on every supported
 * guest having an entry in this list.
 */
typedef enum GuestOSType {
   GUEST_OS_BASE                = 0x5000,

   GUEST_OS_ANY                 = GUEST_OS_BASE + 0,
   GUEST_OS_DOS                 = GUEST_OS_BASE + 1,
   GUEST_OS_WIN31               = GUEST_OS_BASE + 2,
   GUEST_OS_WIN95               = GUEST_OS_BASE + 3,
   GUEST_OS_WIN98               = GUEST_OS_BASE + 4,
   GUEST_OS_WINME               = GUEST_OS_BASE + 5,
   GUEST_OS_WINNT               = GUEST_OS_BASE + 6,
   GUEST_OS_WIN2000             = GUEST_OS_BASE + 7,
   GUEST_OS_WINXP               = GUEST_OS_BASE + 8,
   GUEST_OS_WINXPPRO_64         = GUEST_OS_BASE + 9,
   GUEST_OS_WINNET              = GUEST_OS_BASE + 10,
   GUEST_OS_WINNET_64           = GUEST_OS_BASE + 11,
   GUEST_OS_LONGHORN            = GUEST_OS_BASE + 12,
   GUEST_OS_LONGHORN_64         = GUEST_OS_BASE + 13,
   GUEST_OS_WINVISTA            = GUEST_OS_BASE + 14,
   GUEST_OS_WINVISTA_64         = GUEST_OS_BASE + 15,
   GUEST_OS_UBUNTU              = GUEST_OS_BASE + 16,
   GUEST_OS_OTHER24XLINUX       = GUEST_OS_BASE + 17,
   GUEST_OS_OTHER24XLINUX_64    = GUEST_OS_BASE + 18,
   GUEST_OS_OTHER26XLINUX       = GUEST_OS_BASE + 19,
   GUEST_OS_OTHER26XLINUX_64    = GUEST_OS_BASE + 20,
   GUEST_OS_OTHERLINUX          = GUEST_OS_BASE + 21,
   GUEST_OS_OTHERLINUX_64       = GUEST_OS_BASE + 22,
   GUEST_OS_OS2                 = GUEST_OS_BASE + 23,
   GUEST_OS_OTHER               = GUEST_OS_BASE + 24,
   GUEST_OS_OTHER_64            = GUEST_OS_BASE + 25,
   GUEST_OS_FREEBSD             = GUEST_OS_BASE + 26,
   GUEST_OS_FREEBSD_64          = GUEST_OS_BASE + 27,
   GUEST_OS_NETWARE4            = GUEST_OS_BASE + 28,
   GUEST_OS_NETWARE5            = GUEST_OS_BASE + 29,
   GUEST_OS_NETWARE6            = GUEST_OS_BASE + 30,
   GUEST_OS_SOLARIS6            = GUEST_OS_BASE + 31,
   GUEST_OS_SOLARIS7            = GUEST_OS_BASE + 32,
   GUEST_OS_SOLARIS8            = GUEST_OS_BASE + 33,
   GUEST_OS_SOLARIS9            = GUEST_OS_BASE + 34,
   GUEST_OS_SOLARIS10           = GUEST_OS_BASE + 35,
   GUEST_OS_SOLARIS10_64        = GUEST_OS_BASE + 36,
   GUEST_OS_VMKERNEL            = GUEST_OS_BASE + 37,
   GUEST_OS_DARWIN              = GUEST_OS_BASE + 38,
   GUEST_OS_DARWIN_64           = GUEST_OS_BASE + 39,
   GUEST_OS_OPENSERVER5         = GUEST_OS_BASE + 40,
   GUEST_OS_OPENSERVER6         = GUEST_OS_BASE + 41,
   GUEST_OS_UNIXWARE7           = GUEST_OS_BASE + 42,
} GuestOSType;


typedef enum GuestOSFamilyType {
   GUEST_OS_FAMILY_ANY         = 0x0000,
   GUEST_OS_FAMILY_LINUX       = 0x0001,
   GUEST_OS_FAMILY_WINDOWS     = 0x0002,
   GUEST_OS_FAMILY_WIN9X       = 0x0004,
   GUEST_OS_FAMILY_WINNT       = 0x0008,
   GUEST_OS_FAMILY_WIN2000     = 0x0010,
   GUEST_OS_FAMILY_WINXP       = 0x0020,
   GUEST_OS_FAMILY_WINNET      = 0x0040,
   GUEST_OS_FAMILY_NETWARE     = 0x0080
} GuestOSFamilyType;

#define B(guest)	((uint64) 1 << ((guest) - GUEST_OS_BASE))
#define BS(suf)		B(GUEST_OS_##suf)
#define ALLWIN9X	(BS(WIN95) | BS(WIN98) | BS(WINME))
#define ALLWIN2000	BS(WIN2000)

#define ALLWINXP32	BS(WINXP)
#define ALLWINXP64	BS(WINXPPRO_64)
#define ALLWINXP        (ALLWINXP32 | ALLWINXP64)

#define ALLFREEBSD      BS(FREEBSD) | BS(FREEBSD_64)

#define ALLWINNET32	BS(WINNET)
#define ALLWINNET64	BS(WINNET_64)
#define ALLWINNET	(ALLWINNET32 | ALLWINNET64)

#define ALLWINVISTA32   (BS(LONGHORN) | BS(WINVISTA))
#define ALLWINVISTA64   (BS(LONGHORN_64) | BS(WINVISTA_64))
#define ALLWINVISTA     (ALLWINVISTA32 | ALLWINVISTA64)

#define ALLWINNT32	(BS(WINNT) | ALLWIN2000 | ALLWINXP32 | ALLWINNET32 | \
                         ALLWINVISTA32)
#define ALLWINNT64	(ALLWINXP64 | ALLWINNET64 | ALLWINVISTA64)
#define ALLWINNT	(ALLWINNT32 | ALLWINNT64)

#define ALLWIN32	(ALLWIN9X | ALLWINNT32)
#define ALLWIN64	 ALLWINNT64
#define ALLWIN          (ALLWIN32 | ALLWIN64)
#define ALLSOLARIS      (BS(SOLARIS6) | BS(SOLARIS7) | BS(SOLARIS8) | \
                         BS(SOLARIS9) | BS(SOLARIS10) | BS(SOLARIS10_64))
#define ALLNETWARE      (BS(NETWARE4) | BS(NETWARE5) | BS(NETWARE6))
#define ALLLINUX32      (BS(UBUNTU) | BS(OTHER24XLINUX) | BS(VMKERNEL) | \
                         BS(OTHER26XLINUX) | BS(OTHERLINUX))
#define ALLLINUX64      (BS(OTHERLINUX_64) | BS(OTHER24XLINUX_64) | \
                         BS(OTHER26XLINUX_64))
#define ALLLINUX        (ALLLINUX32 | ALLLINUX64)
#define ALLDARWIN       (BS(DARWIN) | BS(DARWIN_64))
#define ALL64           (ALLWIN64 | ALLLINUX64 | BS(LONGHORN_64) | \
                         BS(SOLARIS10_64) | BS(FREEBSD_64) | \
                         BS(WINVISTA_64) | BS(DARWIN_64) | BS(OTHER_64))

#endif
