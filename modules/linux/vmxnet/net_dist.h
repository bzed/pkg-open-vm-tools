/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * 
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
 * net_dist.h --
 *
 *      Networking headers.
 */

#ifndef _NET_DIST_H_
#define _NET_DIST_H_

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMNIXMOD
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"
#include "vmkapi_types.h"

typedef struct PktHandle   vmk_PktHandle;
typedef vmk_uint32         vmk_NetPortID;
typedef vmk_uint16         vmk_VlanID;

typedef vmk_NetPortID Net_PortID;
typedef vmk_PktHandle PktHandle;
typedef struct PktList PktList;

/*
 * Set this to anything, and that value will never be assigned as a 
 * PortID for a valid port, but will be assigned in most error cases.
 */
#define NET_INVALID_PORT_ID      0

/*
 * Set this to the largest size of private implementation data expected
 * to be embedded in a pkt by device driver wrapper modules
 * (e.g. sizeof(esskaybee_or_whatever))
 */
#ifdef VM_X86_64
#define NET_MAX_IMPL_PKT_OVHD   (616)
#else
#define NET_MAX_IMPL_PKT_OVHD   (432)
#endif

/*
 * Set this to the largest alignment requirements made by various driver
 * wrappers. Unfortunately linux guarantees space for 16 byte alignment
 * from the drivers, although most drivers don't use it (none?).
 */
#define NET_MAX_IMPL_ALIGN_OVHD (16)

/*
 * Set this to the size of the largest object that a driver will embed in
 * the buffer aside from the frame data, currently the e100 has a 32 byte
 * struct that it puts there.
 */ 
#define NET_MAX_DRV_PKT_OVHD    (32) 

/*
 * Set this to the largest alignment overhead connsumed by the various
 * drivers for alignment purposes.  Many of the drivers want an extra 
 * 2 bytes for aligning iphdr, and the 3c90x wants 64 byte aligned dma.
 */
#define NET_MAX_DRV_ALIGN_OVHD  (64+2)

/*
 * Portset event API.  Callers who register for these events will
 * recieve asynchronus notification whenever an event occurs.
 */
typedef enum {
   PORTSET_EVENT_PORT_CONNECT     = 0x00000001,
   PORTSET_EVENT_PORT_DISCONNECT  = 0x00000002,
   PORTSET_EVENT_PORT_BLOCK       = 0x00000004,
   PORTSET_EVENT_PORT_UNBLOCK     = 0x00000008,
   PORTSET_EVENT_PORT_L2ADDR      = 0x00000010,
   PORTSET_EVENT_PORT_ENABLE      = 0x00000020,
   PORTSET_EVENT_PORT_DISABLE     = 0x00000040,
   PORTSET_EVENT_MASK_ALL         = 0x0000007f
} PortsetEventID;

typedef void (*PortsetEventCB)  (Net_PortID, PortsetEventID, void *);

#endif
