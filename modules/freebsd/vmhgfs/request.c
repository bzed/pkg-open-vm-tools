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
 * request.c --
 *
 *      Implementation of routines used to initialize, allocate, and move
 *      requests between lists.
 */

/*
 * Includes
 */

#include "hgfs_kernel.h"
#include "requestInt.h"


/*
 * Macros
 */

/*
 * Since the DblLnkLst_Links in the requests container is just an anchor, we want to
 * skip it (e.g., get the container for the next element)
 */
#define HGFS_SIP_LIST_HEAD(sip)         \
            (DblLnkLst_Container((sip)->reqs->list.next, HgfsKReqObject, fsNode))
#define HGFS_SIP_LIST_HEAD_NODE(sip)    (sip->reqs->list.next)


/*
 * Local data
 */

/*
 * See requestInt.h for details.
 */

uma_zone_t      hgfsKReqZone;
DblLnkLst_Links hgfsKReqWorkItemList;
struct mtx      hgfsKReqWorkItemLock;
struct cv       hgfsKReqWorkItemCv;


/*
 * Local functions (prototypes)
 */

static int      HgfsKReqZCtor(void *mem, int size, void *arg, int flags);
static void     HgfsKReqZDtor(void *mem, int size, void *arg);
static int      HgfsKReqZInit(void *mem, int size, int flags);
static void     HgfsKReqZFini(void *mem, int size);


/*
 * Global functions (definitions)
 */


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReq_SysInit --
 *
 *      This function simply initializes the hgfsKReqZone.  This is done
 *      separately from the VFS initialization routine, our caller, in order
 *      to abstract away the request allocation & support code.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      hgfsKReqZone is initialized.  This routine may sleep.
 *
 *-----------------------------------------------------------------------------
 */

int
HgfsKReq_SysInit(void)
{
   int ret = 0;

   hgfsKReqZone = uma_zcreate(HGFS_FS_NAME "_zone",
                              sizeof (struct HgfsKReqObject),
                              HgfsKReqZCtor, HgfsKReqZDtor, HgfsKReqZInit,
                              HgfsKReqZFini, UMA_ALIGN_PTR, 0);

   /* uma_zcreate() may sleep, so it should never fail. */
   if (hgfsKReqZone == NULL) {
      return ENOMEM;
   }

   /* The following routines cannot fail. */
   mtx_init(&hgfsKReqWorkItemLock, HGFS_FS_NAME "_workmtx", NULL, MTX_DEF);
   cv_init(&hgfsKReqWorkItemCv, HGFS_FS_NAME "_workcv");
   DblLnkLst_Init(&hgfsKReqWorkItemList);

   /* Spawn the worker thread. */
   ret = kthread_create(HgfsKReqWorker, &hgfsKReqWorkerState,
                        &hgfsKReqWorkerProc, 0, 0, "HgfsKReqWorker");

   if (ret != 0) {
      mtx_destroy(&hgfsKReqWorkItemLock);
      cv_destroy(&hgfsKReqWorkItemCv);
   }

   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReq_SysFini --
 *
 *      Hgfs request subsystem cleanup routine.  This should be called when the
 *      Hgfs client module is unloaded from the kernel.
 *
 * Results:
 *      Zero on success or errno on failure.
 *
 * Side effects:
 *      This routine may (will?) sleep.  hgfsKReqZone is destroyed.
 *
 *-----------------------------------------------------------------------------
 */

int
HgfsKReq_SysFini(void)
{
   int ret = 0;

   /* Signal the worker thread to exit. */
   mtx_lock(&hgfsKReqWorkItemLock);
   hgfsKReqWorkerState.exit = TRUE;
   cv_signal(&hgfsKReqWorkItemCv);

   /*
    * Sleep until the worker thread exits.  The PDROP flag indicates that
    * hgfsKReqWorkItemLock should not be reacquired upon wakeup.
    */
   msleep(hgfsKReqWorkerProc, &hgfsKReqWorkItemLock, PDROP, "Hgfs Shutdown", 0);

   /*
    * Destroy resources allocated during _SysInit().
    */
   uma_zdestroy(hgfsKReqZone);
   cv_destroy(&hgfsKReqWorkItemCv);
   mtx_destroy(&hgfsKReqWorkItemLock);

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_AllocateContainer --
 *
 *      Allocate a request container for a single file system mount.
 *
 * Results:
 *      Pointer to a new allocation container.  Always succeeds.
 *
 * Side effects:
 *      This routine may sleep.
 *
 *----------------------------------------------------------------------------
 */

HgfsKReqContainerHandle
HgfsKReq_AllocateContainer(void)
{
   HgfsKReqContainer *container;

   /* Called with M_WAITOK, so this cannot fail. */
   container = malloc(sizeof (struct HgfsKReqContainer), M_HGFS,
                      M_WAITOK|M_ZERO);

   mtx_init(&container->listLock, "hgfs_reql_mtx", NULL, MTX_DEF);
   DblLnkLst_Init(&container->list);

   return container;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_FreeContainer --
 *
 *      Free a request container.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

void
HgfsKReq_FreeContainer(HgfsKReqContainerHandle container) // IN: file system's
                                                          // container handle
{
   ASSERT(container);
   ASSERT(DblLnkLst_IsLinked(&container->list) == FALSE);

   mtx_destroy(&container->listLock);
   free(container, M_HGFS);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_CancelRequests --
 *
 *    Cancels all allocated requests by updating their status (set to
 *    HGFS_REQ_ERROR) and waking up any waiting clients.  Also, if linked,
 *    removes any items from the work item list.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    This file system's entries are removed from the work item list are
 *    removed.
 *
 *----------------------------------------------------------------------------
 */

void
HgfsKReq_CancelRequests(HgfsKReqContainerHandle container) // IN: request container
{
   DblLnkLst_Links *currNode, *nextNode;
   HgfsKReqObject *req;

   DEBUG(VM_DEBUG_REQUEST, "HgfsCancelAllRequests().\n");

   ASSERT(container);

   /*
    * 1. Lock this file system's request list.
    * 2. Lock the global pending request list.
    * 3. For each request in the file system's request list:
    *    a.  Remove from the global pending request list.
    *    b.  Lock the request.
    *    c.  Set the request's state to HGFS_REQ_ERROR.
    *    d.  Signal any waiters.
    *    e.  Drop our reference, destroying the object if ours was the last.
    * 4. Unlock the global pending request list.
    * 5. Unlock the file system's request list.
    */

   mtx_lock(&container->listLock);
   mtx_lock(&hgfsKReqWorkItemLock);

   DEBUG(VM_DEBUG_REQUEST, "HgfsCancelAllRequests(): traversing pending request list.\n");

   DblLnkLst_ForEachSafe(currNode, nextNode, &container->list) {
      Bool deref = FALSE;

      /* Get a pointer to the request represented by currNode. */
      req = DblLnkLst_Container(currNode, HgfsKReqObject, fsNode);

      /*
       * If linked in the pending request list, remove it.  Note that we're
       * transferring that list's reference to ourself.  (I.e., we'll be
       * responsible for decrementing the reference count and freeing if it
       * reaches zero.)
       */
      if (DblLnkLst_IsLinked(&req->pendingNode)) {
         deref = TRUE;
         DblLnkLst_Unlink1(&req->pendingNode);
      }

      /* Force this over to the error state & wake up any waiters. */
      mtx_lock(&req->stateLock);
      req->state = HGFS_REQ_ERROR;
      cv_signal(&req->stateCv);
      mtx_unlock(&req->stateLock);

      if (deref) {
         if (atomic_fetchadd_int(&req->refcnt, -1) == 1) {
            uma_zfree(hgfsKReqZone, req);
         }
      }
   }

   mtx_unlock(&hgfsKReqWorkItemLock);
   mtx_unlock(&container->listLock);

   DEBUG(VM_DEBUG_REQUEST, "HgfsCancelAllRequests() done.\n");
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_ContainerIsEmpty --
 *
 *      Indicates whether a file system, represented by its superinfo, has any
 *      outstanding HgfsKReqObjectuests.
 *
 * Results:
 *    Returns zero if list is not empty, a positive integer if it is empty.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

Bool
HgfsKReq_ContainerIsEmpty(HgfsKReqContainerHandle container)       // IN:
{
   Bool result;

   ASSERT(container);

   mtx_lock(&container->listLock);
   result = DblLnkLst_IsLinked(&container->list) ? FALSE : TRUE;
   mtx_unlock(&container->listLock);

   return result;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_AllocateRequest --
 *
 *    Allocates and initializes a new request structure from the request pool.
 *    This function blocks until a request is available or it has been
 *    interrupted by a signal.
 *
 * Results:
 *    Pointer to fresh HgfsKReqObject.
 *
 * Side effects:
 *    Request inserted into caller's requests container.  This routine may
 *    sleep.
 *
 *----------------------------------------------------------------------------
 */

HgfsKReqObject *
HgfsKReq_AllocateRequest(HgfsKReqContainerHandle container)        // IN:
{
   HgfsKReqObject *req;

   ASSERT(container);

   /* Called with M_WAITOK, so this can not fail. */
   req = uma_zalloc(hgfsKReqZone, M_WAITOK);

   mtx_lock(&container->listLock);
   DblLnkLst_LinkLast(&container->list, &req->fsNode);
   mtx_unlock(&container->listLock);

   return req;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_ReleaseReq --
 *
 *      Routine for file systems to return a request to the pool.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      oldReq->refcnt will be decremented, and oldReq may be freed.
 *
 *----------------------------------------------------------------------------
 */

void
HgfsKReq_ReleaseRequest(HgfsKReqContainerHandle container,      // IN:
                        HgfsKReqHandle oldRequest)              // IN:
{
   DEBUG(VM_DEBUG_ENTRY, "%s\n", __func__);

   ASSERT(container);
   ASSERT(oldRequest);

   /* Dissociate request from this file system. */
   mtx_lock(&container->listLock);
   DblLnkLst_Unlink1(&oldRequest->fsNode);
   mtx_unlock(&container->listLock);

   /* State machine update */
   mtx_lock(&oldRequest->stateLock);

   switch (oldRequest->state) {
   case HGFS_REQ_ALLOCATED:
   case HGFS_REQ_SUBMITTED:
      oldRequest->state = HGFS_REQ_ABANDONED;
      break;
   case HGFS_REQ_ABANDONED:
      panic("%s: Request (%p) already abandoned!\n", __func__, oldRequest);
      break;
   case HGFS_REQ_ERROR:
   case HGFS_REQ_COMPLETED:
      break;
   default:
      NOT_REACHED();
   }

   mtx_unlock(&oldRequest->stateLock);

   /* Dereference file system from request.  If refcnt goes to zero, free. */
   if (atomic_fetchadd_int(&oldRequest->refcnt, -1) == 1) {
      uma_zfree(hgfsKReqZone, oldRequest);
   }

   DEBUG(VM_DEBUG_REQUEST, "%s done.\n", __func__);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_SubmitRequest --
 *
 *      Queues caller's request for Guest <-> Host processing and waits for
 *      it to be processed.
 *
 * Results:
 *      Zero on success, errno if interrupted.
 *
 * Side effects:
 *      Request's state may change.
 *
 * Synchronization notes:
 *      Assumes caller holds newReq->stateLock.  (Implicit from _GetNewReq.)
 *
 *----------------------------------------------------------------------------
 */

int
HgfsKReq_SubmitRequest(HgfsKReqObject *newreq)     // IN: Request to enqueue
{
   int ret = 0;

   DEBUG(VM_DEBUG_REQUEST, "HgfsEnqueueRequest().\n");

   ASSERT(newreq);

   /*
    * Insert request on pending request list, then alert of its arrival the
    * request processor.  Since the list will also reference the request, be
    * sure to bump its count before unlocking the list!
    */

   mtx_lock(&hgfsKReqWorkItemLock);

   /*
    * With the work item list locked, lock our object and operate on its state.
    * Typically we expect it to be in the ALLOCATED state, but if the file
    * system asynchronously cancelled all requests, it may be in ERROR instead.
    */

   mtx_lock(&newreq->stateLock);

   switch (newreq->state) {
   case HGFS_REQ_ALLOCATED:
      /*
       * Update request's state, bump refcnt, and signal worker thread.
       */
      newreq->state = HGFS_REQ_SUBMITTED;
      atomic_add_int(&newreq->refcnt, 1);
      DblLnkLst_LinkLast(&hgfsKReqWorkItemList, &newreq->pendingNode);
      cv_signal(&hgfsKReqWorkItemCv);
      mtx_unlock(&hgfsKReqWorkItemLock);
      /*
       * NB: We're still holding this request's state lock for use with
       * cv_wait_sig.
       */
      break;

   case HGFS_REQ_ERROR:
      /*
       * Bail ASAP.
       */
      mtx_unlock(&newreq->stateLock);
      mtx_unlock(&hgfsKReqWorkItemLock);
      return EIO;
      break;

   case HGFS_REQ_UNUSED:
   case HGFS_REQ_SUBMITTED:
   case HGFS_REQ_ABANDONED:
   case HGFS_REQ_COMPLETED:
      panic("Cannot submit object (%p) in its current state: %u",
            newreq, newreq->state);
      break;
   default:
      panic("Request object (%p) in unknown state: %u", newreq, newreq->state);
   }

   /* Sleep until request is processed or we're interrupted. */
   while (newreq->state == HGFS_REQ_SUBMITTED && ret == 0) {
      ret = cv_wait_sig(&newreq->stateCv, &newreq->stateLock);
   }

   /* Okay, we're finished with the state lock for now. */
   mtx_unlock(&newreq->stateLock);

   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReq_GetId --
 *
 *      Return this object's unique request ID.
 *
 * Results:
 *      Object's unique request ID.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

uint32_t
HgfsKReq_GetId(HgfsKReqHandle request) // IN: Request to get the ID for
{
   ASSERT(request);

   return request->id;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReq_GetPayload --
 *
 *      Return a pointer to the payload area of a request.  Callers may write
 *      Hgfs packet data directly to this area.  It's guaranteed to hold at
 *      most HGFS_PACKET_MAX (6144) bytes.
 *
 * Results:
 *      Pointer to the payload area.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
HgfsKReq_GetPayload(HgfsKReqHandle request)     // IN:
{
   ASSERT(request);

   return request->payload;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReq_GetPayloadSize --
 *
 *      Returns the amount of data current stored in the payload.  (Typically
 *      used when the file system receives an Hgfs reply.)
 *
 * Results:
 *      Size of current payload in bytes.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

size_t
HgfsKReq_GetPayloadSize(HgfsKReqHandle request) // IN: Request to get the size of
{
   ASSERT(request);

   return request->payloadSize;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReq_SetPayloadSize --
 *
 *      Record the amount of data currently stored in the payload.  (Typically
 *      used when the file system finishes composing its request.)
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Request object's payload size is modified.
 *
 *-----------------------------------------------------------------------------
 */

void
HgfsKReq_SetPayloadSize(HgfsKReqHandle request, // IN: Request object
                        size_t payloadSize) // IN: New payload size
{
   ASSERT(request);
   ASSERT(payloadSize <= HGFS_PACKET_MAX);
   request->payloadSize = payloadSize;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsKReq_GetState --
 *
 *      Retrieves state of provided request.
 *
 * Results:
 *      Returns state of request.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

HgfsKReqState
HgfsKReq_GetState(HgfsKReqObject *req)      // IN: Request to retrieve state of
{
   HgfsKReqState state;

   ASSERT(req);

   mtx_lock(&req->stateLock);
   state = req->state;
   mtx_unlock(&req->stateLock);

   return state;
}


/*
 * Local functions (definitions)
 */


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReqZInit --
 *
 *      "The initializer is called when the memory is cached in the uma zone.
 *      this should be the same state that the destructor leaves the object in."
 *        - sys/vm/uma.h
 *
 * Results:
 *      Zero on success, errno on failure.
 *
 * Side effects:
 *      A request's mutex and condvar are initialized, ID recorded, and status
 *      set to HGFS_REQ_UNUSED.
 *
 *-----------------------------------------------------------------------------
 */

static int
HgfsKReqZInit(void *mem,     // IN: Pointer to the allocated object
              int size,      // IN: Size of item being initialized [ignored]
              int flags)     // IN: malloc(9) style flags
{
   HgfsKReqObject *req = (HgfsKReqObject *)mem;
   ASSERT(size == sizeof *req);

   /*
    * Zero out the object.  (Do NOT pass UMA_ZEROINIT to uma_zcreate, as
    * that will override this routine and zero everything -after- us.)
    */
   bzero(req, sizeof *req);

   /*
    * Request IDs are a 32-bit unsigned integer.  Conveniently enough for us,
    * our memory addresses provide (at least) 32 bits.
    *
    * Assumption:  Kernel memory will never straddle a 2**32 byte boundary.
    */
   req->id = (uint32_t)((unsigned long)req & 0xffffffff);
   req->state = HGFS_REQ_UNUSED;
   mtx_init(&req->stateLock, "hgfs_req_mtx", NULL, MTX_DEF);
   cv_init(&req->stateCv, "hgfs_req_cv");

   /* Reset list pointers. */
   DblLnkLst_Init(&req->fsNode);
   DblLnkLst_Init(&req->pendingNode);

   /* Clear packet of request before allocating to clients. */
   bzero(&req->__rpc_packet, sizeof req->__rpc_packet);
   bcopy(HGFS_SYNC_REQREP_CLIENT_CMD, req->__rpc_packet._command,
         HGFS_SYNC_REQREP_CLIENT_CMD_LEN);

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReqZFini --
 *
 *      "This routine is called when memory leaves a zone and is returned
 *       to the system for other uses.  It is the counter part to the
 *       init function." - sys/vm/uma.h
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A request's mutex and condvar are destroyed.
 *
 *-----------------------------------------------------------------------------
 */

static void
HgfsKReqZFini(void *mem,     // IN: Pointer to object leaving the UMA cache
              int size)      // IN: Size of object [Ignored]
{
   HgfsKReqObject *req = (HgfsKReqObject *)mem;
   ASSERT(size == sizeof *req);
   ASSERT(req->state == HGFS_REQ_UNUSED);
   mtx_destroy(&req->stateLock);
   cv_destroy(&req->stateCv);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReqZCtor
 *
 *      "The constructor is called just before the memory is returned
 *       to the user. It may block if necessary." - sys/vm/uma.h
 *
 * Results:
 *      Zero on success, errno on failure.
 *
 * Side effects:
 *      Request's state is set to HGFS_REQ_ALLOCATED, its listNode is
 *      initialized, and its packet is zeroed out.
 *
 *-----------------------------------------------------------------------------
 */

static int
HgfsKReqZCtor(void *mem,     // IN: Pointer to memory allocated to user
              int size,      // IN: Size of allocated object [ignored]
              void *arg,     // IN: Optional argument from uma_zalloc_arg [ignored]
              int flags)     // IN: malloc(9) flags
{
   HgfsKReqObject *req = (HgfsKReqObject *)mem;

   ASSERT(size == sizeof *req);
   ASSERT(req->state == HGFS_REQ_UNUSED);
   ASSERT(DblLnkLst_IsLinked(&req->fsNode) == FALSE);
   ASSERT(DblLnkLst_IsLinked(&req->pendingNode) == FALSE);

   /* Initialize state & reference count. */
   req->state = HGFS_REQ_ALLOCATED;
   req->refcnt = 1;

   ASSERT(!strncmp(req->__rpc_packet._command, HGFS_SYNC_REQREP_CLIENT_CMD, HGFS_SYNC_REQREP_CLIENT_CMD_LEN));

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsKReqZDtor
 *
 *      "The destructor may perform operations that differ from those performed
 *       by the initializer, but it must leave the object in the same state.
 *       This IS type stable storage.  This is called after EVERY zfree call."
 *        - sys/vm/uma.h
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Object's state is set to HGFS_REQ_UNUSED.
 *
 *-----------------------------------------------------------------------------
 */

static void
HgfsKReqZDtor(void *mem,     // IN: Pointer to allocated object
              int size,      // IN: Size of allocated object [ignored]
              void *arg)     // IN: Argument for uma_zfree_arg [ignored]
{
   HgfsKReqObject *req = (HgfsKReqObject *)mem;

   ASSERT(req->refcnt == 0);
   ASSERT(DblLnkLst_IsLinked(&req->fsNode) == FALSE);
   ASSERT(DblLnkLst_IsLinked(&req->pendingNode) == FALSE);

   req->state = HGFS_REQ_UNUSED;
}