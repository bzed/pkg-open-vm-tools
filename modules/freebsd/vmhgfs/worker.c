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
 * worker.c --
 *
 *	Worker thread to process issue Guest -> Host Hgfs requests.
 */

#include "hgfs_kernel.h"
#include "request.h"
#include "requestInt.h"

#include "hgfsBd.h"

#include <sys/kthread.h>
#include <sys/libkern.h>


/*
 * Local data
 */

/*
 * Process structure filled in when the worker thread is created.
 */
struct proc *hgfsKReqWorkerProc;

/*
 * See requestInt.h.
 */
HgfsKReqWState hgfsKReqWorkerState;


/*
 * Global (module) functions
 */


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReqWorker --
 *
 *      Main routine for Hgfs client worker thread.  This thread is responsible
 *      for all Hgfs communication with the host via the backdoor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
HgfsKReqWorker(void *arg)
{
   DblLnkLst_Links *currNode, *nextNode;
   HgfsKReqWState *ws = (HgfsKReqWState *)arg;
   HgfsKReqObject *req;
   RpcOut *hgfsRpcOut = NULL;
   char const *replyPacket;
   int ret = 0;

   ws->running = TRUE;

   for (;;) {
      /*
       * This loop spends most of its time sleeping until signalled by another
       * thread.  We expect to be signalled only if either there is work to do
       * or if the module is being unloaded.
       */

      mtx_lock(&hgfsKReqWorkItemLock);

      while (!ws->exit && !DblLnkLst_IsLinked(&hgfsKReqWorkItemList)) {
         cv_wait(&hgfsKReqWorkItemCv, &hgfsKReqWorkItemLock);
      }

      if (ws->exit) {
         /* Note that the list lock is still held. */
         break;
      }

      /*
       * We have work to do!  Hooray!  Start by locking the request and pulling
       * it from the work item list.  (The list's reference is transferred to
       * us, so we'll decrement the request's reference count when we're
       * finished with it.)
       *
       * With the request locked, make a decision based on the request's state.
       * Typically a request will be in the SUBMITTED state, but if its owner
       * aborted an operation or the file system cancelled it, it may be listed
       * as ABANDONED.  If either of the latter are true, then we don't bother
       * with any further processing.
       *
       * Because we're not sure how long the backdoor operation will take, we
       * yield the request's state lock before calling HgfsBd_Dispatch.  Upon
       * return, we must test the state again (see above re: cancellation),
       * and then we finally update the state & signal any waiters.
       */

      currNode = hgfsKReqWorkItemList.next;
      DblLnkLst_Unlink1(currNode);
      req = DblLnkLst_Container(currNode, HgfsKReqObject, pendingNode);

      mtx_lock(&req->stateLock);
      switch (req->state) {
      case HGFS_REQ_SUBMITTED:
         if (!HgfsBd_OpenBackdoor(&hgfsRpcOut)) {
            req->state = HGFS_REQ_ERROR;
            cv_signal(&req->stateCv);
            mtx_unlock(&req->stateLock);
            goto done;
         }
         break;
      case HGFS_REQ_ABANDONED:
      case HGFS_REQ_ERROR:
         goto done;
         break;
      default:
         panic("Request object (%p) in unknown state: %u", req, req->state);
      }
      mtx_unlock(&req->stateLock);

      /*
       * We're done with the work item list for now.  Unlock it and let the file
       * system add requests while we're busy.
       */
      mtx_unlock(&hgfsKReqWorkItemLock);

      ret = HgfsBd_Dispatch(hgfsRpcOut, req->payload, &req->payloadSize,
                            &replyPacket);

      /*
       * We have a response.  (Maybe.)  Re-lock the request, update its state,
       * etc.
       */

      mtx_lock(&req->stateLock);

      if ((ret == 0) && (req->state == HGFS_REQ_SUBMITTED)) {
         bcopy(replyPacket, req->payload, req->payloadSize);
         req->state = HGFS_REQ_COMPLETED;
      } else {
         req->state = HGFS_REQ_ERROR;
      }

      cv_signal(&req->stateCv);
      mtx_unlock(&req->stateLock);

      if (ret != 0) {
         /*
          * If the channel was previously open, make sure it's dead and gone
          * now. We do this because subsequent requests deserve a chance to
          * reopen it.
          */
         HgfsBd_CloseBackdoor(&hgfsRpcOut);
      }

done:
      if (atomic_fetchadd_int(&req->refcnt, -1) == 1) {
         uma_zfree(hgfsKReqZone, req);
      }
   }

   /*
    * NB:  The work item lock is still held.
    */

   /*
    * We're signaled to exit.  Remove any items from the pending request list
    * before exiting.
    */
   DblLnkLst_ForEachSafe(currNode, nextNode, &hgfsKReqWorkItemList) {
      req = DblLnkLst_Container(currNode, HgfsKReqObject, pendingNode);
      DblLnkLst_Unlink1(currNode);
      mtx_lock(&req->stateLock);
      req->state = HGFS_REQ_ERROR;
      cv_signal(&req->stateCv);
      mtx_unlock(&req->stateLock);

      /*
       * If we held the final reference to a request, free it.
       */
      if (atomic_fetchadd_int(&req->refcnt, -1) == 1) {
         uma_zfree(hgfsKReqZone, req);
      }
   }

   mtx_unlock(&hgfsKReqWorkItemLock);

   ws->running = FALSE;

   if (hgfsRpcOut != NULL ) {
      HgfsBd_CloseBackdoor(&hgfsRpcOut);
   }

   kthread_exit(0);
}
