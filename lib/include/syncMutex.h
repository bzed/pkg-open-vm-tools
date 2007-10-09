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
 * syncMutex.h --
 *
 *      Implements a platform independent mutex
 */

#ifndef _SYNC_MUTEX_H_
#define _SYNC_MUTEX_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include "syncWaitQ.h"
#include "vm_atomic.h"

/*
 * SyncMutex --
 */

#define MUTEX_MAX_PATH   WAITQ_MAX_PATH

typedef struct SyncMutex {
   SyncWaitQ wq;

   /* Is the mutex unlocked? --hpreg */
   Atomic_uint32 unlocked;
#if !defined(_WIN32)
   pthread_mutex_t _mutex;
#endif
} SyncMutex;

Bool SyncMutex_Init(SyncMutex *that, char const *path);
void SyncMutex_Destroy(SyncMutex *that);
Bool SyncMutex_Lock(SyncMutex *that);
Bool SyncMutex_Unlock(SyncMutex *that);

SyncMutex *SyncMutex_CreateSingleton(Atomic_Ptr *lckStorage);

#endif // #ifndef _SYNC_MUTEX_H_