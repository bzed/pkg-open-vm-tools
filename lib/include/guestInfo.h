/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved.
 * 
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
 * guestInfo.h --
 *
 *    Common declarations that aid in sending guest information to the host.
 */

#ifndef _GUEST_INFO_H_
#define _GUEST_INFO_H_

#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"

#include "vm_basic_types.h"

#define GUEST_INFO_COMMAND "SetGuestInfo"
#define MAX_VALUE_LEN 100
#define MAX_NICS     16
#define MAX_IPS      8     // Max number of IP addresses for a single NIC
#define MAC_ADDR_SIZE 19
#define IP_ADDR_SIZE 16
#define PARTITION_NAME_SIZE MAX_VALUE_LEN

typedef enum {
   INFO_ERROR,       /* Zero is unused so that errors in atoi can be caught. */
   INFO_DNS_NAME,
   INFO_IPADDRESS,
   INFO_DISK_FREE_SPACE,
   INFO_TOOLS_VERSION,
   INFO_OS_NAME_FULL,
   INFO_OS_NAME,
   INFO_UPTIME,
   INFO_MAX
} GuestInfoType;

typedef struct _NicEntry {
   unsigned int numIPs;
   char macAddress[MAC_ADDR_SIZE]; // In the format "12-23-34-45-56-67"
   char ipAddress[MAX_IPS][IP_ADDR_SIZE];
} NicEntry, *PNicEntry;

typedef struct _NicInfo {
   unsigned int numNicEntries;
   NicEntry nicList[MAX_NICS];
} NicInfo, *PNicInfo;


typedef
#include "vmware_pack_begin.h"
struct _PartitionEntry {
   uint64 freeBytes;
   uint64 totalBytes;
   char name[PARTITION_NAME_SIZE]; 
}
#include "vmware_pack_end.h"
PartitionEntry, *PPartitionEntry;

typedef struct _DiskInfo {
   unsigned int numEntries;
   PPartitionEntry partitionList;
} DiskInfo, *PDiskInfo;

#endif // _GUEST_INFO_H_
