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
 * syncMutex.c --
 *
 *      Implements a non-recursive mutex in a platform independent way.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/poll.h>
#include <errno.h>
#endif // #ifdef _WIN32

#include "vm_assert.h"
#include "syncMutex.h"
#include "util.h"

/*
 *----------------------------------------------------------------------
 *
 * SyncMutex_Init --
 *
 *      Initializes a mutex structure. The 'path' parameter names the
 *      mutex. If 'path' is NULL, then an anonymous mutex is
 *      created. (see SyncWaitQ_Init)
 *
 * Results:
 *      TRUE on success and FALSE otherwise
 *
 *---------------------------------------------------------------------- 
 */

Bool
SyncMutex_Init(SyncMutex *that,      // OUT
	       char const *path) // IN
{
   ASSERT(that);

   if (!SyncWaitQ_Init(&that->wq, path)) {
      return FALSE;
   }

   Atomic_Write(&that->unlocked, TRUE);

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SyncMutex_Destroy --
 *
 *      Frees members of the specified mutex structure
 *      (see SyncWaitQ_Destroy for side effects)
 *
 *----------------------------------------------------------------------
 */

void
SyncMutex_Destroy(SyncMutex *that) // IN
{
   SyncWaitQ_Destroy(&that->wq);
}


/*
 *----------------------------------------------------------------------
 *
 * SyncMutex_Lock --
 *
 *      Obtains the mutex
 *
 * Results:
 *      TRUE on success and FALSE otherwise
 *
 *----------------------------------------------------------------------
 */

Bool
SyncMutex_Lock(SyncMutex *that) // IN
{
#ifndef VMX86_DEVEL
#define RETRY_TIMEOUT_MS 5000 // Workaround for bug #23716
#else
#define RETRY_TIMEOUT_MS -1 // Infinite time out in devel builds to catch bug #23716
#endif
   
   int handle;

   ASSERT(that);

   if (Atomic_ReadIfEqualWrite(&that->unlocked, TRUE, FALSE)) {
      return TRUE;
   }

   for (;;) {
      int status;

      handle = SyncWaitQ_Add(&that->wq);
      if (handle < 0) {
         return FALSE;
      }

      if (Atomic_ReadIfEqualWrite(&that->unlocked, TRUE, FALSE)) {
         if (!SyncWaitQ_Remove(&that->wq, handle)) {
            return FALSE;
         }

         break;
      }

#ifdef _WIN32
      status = WaitForSingleObject((HANDLE) handle, RETRY_TIMEOUT_MS);
      ASSERT(status != WAIT_FAILED);
#else // #ifdef _WIN32
      {
	 struct pollfd p;
	 p.events = POLLIN;
	 p.fd     = handle;

	 for (;;) {
	    status = poll(&p, 1, RETRY_TIMEOUT_MS);
	    if (status == 1 || status == 0) {
	       break;
	    }

	    ASSERT(status < 0);
	    if (errno != EINTR) {
	       SyncWaitQ_Remove(&that->wq, handle);
	       return FALSE;
	    }

	    /* We were interrupted by a signal, retry --hpreg */
	 }
      }
#endif // #ifdef _WIN32

      if (!SyncWaitQ_Remove(&that->wq, handle)) {
         return FALSE;
      }
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SyncMutex_Unlock --
 *
 *      Releases the mutex
 *
 * Results:
 *      TRUE on success and FALSE otherwise
 *
 *----------------------------------------------------------------------
 */

Bool
SyncMutex_Unlock(SyncMutex *that) // IN
{
   ASSERT(that);

   Atomic_Write(&that->unlocked, TRUE);

   return SyncWaitQ_WakeUp(&that->wq);
}


/*
 *-----------------------------------------------------------------------------
 *
 * SyncMutex_CreateSingleton --
 *
 *      Creates and returns a mutex backed by the specified storage in a
 *      thread-safe manner. This is useful for modules that need to
 *      protect something with a lock but don't have an existing Init()
 *      entry point where a lock can be created.
 *
 * Results:
 *      A pointer to the mutex. Don't destroy it.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

SyncMutex *
SyncMutex_CreateSingleton(Atomic_Ptr *lckStorage) // IN
{
   SyncMutex *before, *lck;

   /* Check if we've created the lock already. */
   lck = (SyncMutex *) Atomic_ReadPtr(lckStorage);

   if (UNLIKELY(NULL == lck)) {
      /* We haven't, create it. */
      lck = (SyncMutex *) Util_SafeMalloc(sizeof *lck);
      SyncMutex_Init(lck, NULL);

      /*
       * We have successfully created the lock, save it.
       */

      before = (SyncMutex *) Atomic_ReadIfEqualWritePtr(lckStorage, NULL, lck);

      if (before) {
         /* We raced and lost, but it's all cool. */
         SyncMutex_Destroy(lck);
         free(lck);
         lck = before;
      }
   }

   return lck;
}
