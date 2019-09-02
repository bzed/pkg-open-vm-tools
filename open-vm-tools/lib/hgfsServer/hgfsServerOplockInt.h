/*********************************************************
 * Copyright (C) 2013-2016 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * hgfsServerOplock.h --
 *
 *	Header file for private common data types used in the HGFS
 *	opportunistic lock routines.
 */

#ifndef _HGFS_SERVER_OPLOCKINT_H_
#define _HGFS_SERVER_OPLOCKINT_H_

#include "hgfsProto.h"     // for protocol types
#include "hgfsServerInt.h" // for common server types e.g. HgfsSessionInfo

/*
 * Does this platform have oplock support? We define it here to avoid long
 * ifdefs all over the code. For now, Linux and Windows hosts only.
 *
 * XXX: Just kidding, no oplock support yet.
 */
#if 0
#define HGFS_OPLOCKS
#endif

/*
 * XXX describe the data structure
 */

/* Server lock related structure */
typedef struct {
   fileDesc fileDesc;
   int32 event;
   HgfsLockType serverLock;
} ServerLockData;


/*
 * Global variables
 */



/*
 * Global functions
 */

#ifdef HGFS_OPLOCKS
void
HgfsServerOplockBreak(ServerLockData *data);

void
HgfsAckOplockBreak(ServerLockData *lockData,
                   HgfsLockType replyLock);

#endif

#endif // ifndef _HGFS_SERVER_OPLOCKINT_H_
