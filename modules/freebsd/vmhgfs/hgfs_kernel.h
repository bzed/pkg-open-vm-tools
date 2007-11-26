/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
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
 *
 *********************************************************/

/*
 * hgfs_kernel.h --
 *
 *	Declarations for the FreeBSD Hgfs client kernel module.  All
 *	FreeBSD-specifc source files will include this.
 */

#ifndef _HGFSKERNEL_H_
#define _HGFSKERNEL_H_

/*
 * Intended for the Hgfs client kernel module only.
 */
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"


/*
 * System includes
 */

#include <sys/param.h>          // for <everything>
#include <sys/vnode.h>          // for struct vnode
#include <sys/lock.h>           // for struct mtx
#include <sys/mutex.h>          // for struct mtx
#include <sys/proc.h>           // for struct cv
#include <sys/condvar.h>        // for struct cv
#include <sys/malloc.h>         // for MALLOC_*
#include <sys/queue.h>          // for uma_zone_t
#include <vm/uma.h>             // for uma_zone_t


/*
 * VMware includes
 */

#include "dbllnklst.h"
#include "hgfs.h"
#include "hgfsProto.h"

#include "request.h"
#include "state.h"

#include "vm_basic_types.h"
#include "vm_assert.h"


/*
 * Macros
 */

#define HGFS_PAYLOAD_MAX(reply)         (HGFS_PACKET_MAX - sizeof *reply)
#define HGFS_FS_NAME                    "vmhgfs"
#define HGFS_FS_NAME_LONG               "VMware Hgfs client"
/*
 * NB: Used only to provide a value for struct vattr::va_blocksize, "blocksize
 * preferred for i/o".
 */
#define HGFS_BLOCKSIZE                  1024

/* Maximum amount of data that can be transferred in any one request */
#define HGFS_IO_MAX                     4096

/* Internal error code(s) */
#define HGFS_ERR                        (-1)
#define HGFS_ERR_NULL_INPUT             (-50)
#define HGFS_ERR_NODEV                  (-51)
#define HGFS_ERR_INVAL                  (-52)

/* Getting to sip via any vnode */
#define HGFS_VP_TO_SIP(vp)              ((HgfsSuperInfo*)(vp)->v_mount->mnt_data)


/*
 * Types
 */

/* We call them *Header in the kernel code for clarity. */
typedef HgfsReply       HgfsReplyHeader;
typedef HgfsRequest     HgfsRequestHeader;

/*
 * The global state structure for a single filesystem mount.  This is allocated
 * in HgfsVfsMount() and destroyed in HgfsVfsUnmount().
 */
typedef struct HgfsSuperInfo {
   /* Request container */
   HgfsKReqContainerHandle reqs;        /* See request.h. */
   /* For filesystem */
   struct mount *vfsp;                  /* Our filesystem structure */
   struct vnode *rootVnode;             /* Root vnode of the filesystem */
   HgfsFileHashTable fileHashTable;     /* File hash table */
} HgfsSuperInfo;


/*
 * Global variables
 */

/* Defined in vfsops.c. */
MALLOC_DECLARE(M_HGFS);

/* Defined in vnops.c. */
extern struct vop_vector HgfsVnodeOps;

#endif // ifndef _HGFSKERNEL_H_
