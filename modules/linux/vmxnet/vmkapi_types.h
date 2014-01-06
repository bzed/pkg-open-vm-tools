/* **********************************************************
 * Copyright 1998-2004 VMware, Inc.  All rights reserved. 
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
 * @VMKAPIMOD_LICENSE@
 */

/*
 * vmkapi_types.h --
 *
 *	Defines the basic types used in the VMKernel API
 */

#ifndef _VMKAPI_TYPES_H_
#define _VMKAPI_TYPES_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMKDRIVERS
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMNIXMOD
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE

#include "includeCheck.h"

/*
 * Branch prediction hints:
 *     VMK_LIKELY(exp)   - Expression exp is likely true.
 *     VMK_UNLIKELY(exp) - Expression exp is likely false.
 */

#if (__GNUC__ >= 3)
/*
 * gcc3 uses __builtin_expect() to inform the compiler of an expected value.
 * We use this to inform the static branch predictor. The '!!' in VMK_LIKELY
 * will convert any !=0 to a 1.
 */
#  define VMK_LIKELY(_exp)     __builtin_expect(!!(_exp), 1)
#  define VMK_UNLIKELY(_exp)   __builtin_expect((_exp), 0)
#else
#  define VMK_LIKELY(_exp)      (_exp)
#  define VMK_UNLIKELY(_exp)    (_exp)
#endif

typedef enum { VMK_FALSE, VMK_TRUE } vmk_Bool;

typedef signed char        vmk_int8;
typedef unsigned char      vmk_uint8;
typedef short              vmk_int16;
typedef unsigned short     vmk_uint16;
typedef int                vmk_int32;
typedef unsigned int       vmk_uint32;

#if defined(__ia64__) || defined(__x86_64__)
typedef long               vmk_int64;
typedef unsigned long      vmk_uint64;
typedef vmk_uint64         vmk_VirtAddr;
#else 
typedef long long          vmk_int64;
typedef unsigned long long vmk_uint64;
typedef vmk_uint32         vmk_VirtAddr;
#endif

typedef vmk_uint32	   vmk_MachPage;
typedef vmk_uint64         vmk_MachAddr;
typedef unsigned long      vmk_size_t;
typedef long               vmk_ssize_t;
typedef long long          vmk_loff_t;

/**
 * \brief Abstract address
 */
typedef union {
   vmk_VirtAddr addr;
   void *ptr;
} vmk_AddrCookie __attribute__ ((__transparent_union__));

#endif //_VMKAPI_TYPES_H_
