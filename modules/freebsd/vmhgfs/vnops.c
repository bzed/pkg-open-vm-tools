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
 * vnops.c --
 *
 *	Vnode operations for FreeBSD HGFS client.
 */

#include <sys/param.h>          // for everything
#include <sys/vnode.h>          // for struct vnode
#include <sys/mount.h>          // for struct mount
#include <sys/namei.h>          // for name lookup goodness
#include <sys/libkern.h>        // for string & other functions
#include <sys/fcntl.h>          // for in-kernel file access flags (FREAD, etc)
#include <sys/stat.h>           // for file flag bitmasks (S_IRWXU, etc)
#include <sys/uio.h>            // for uiomove
#include <sys/dirent.h>         // for struct dirent

#include "cpName.h"
#include "staticEscape.h"

#include "hgfsUtil.h"

#include "hgfs_kernel.h"
#include "request.h"
#include "state.h"
#include "debug.h"


/*
 * Macros
 */

/*
 * Hgfs permissions are similar to Unix permissions in that they both include
 * bits for read vs. write vs. execute permissions.  However, Hgfs is only
 * concerned with file owners, meaning no "group" or "other" bits, so we need to
 * translate between Hgfs and Unix permissions with a simple bitshift.  The
 * shift value corresponds to omitting the "group" and "other" bits.
 */
#define HGFS_ATTR_MODE_SHIFT    6

/* Sets the values of request headers properly */
#define HGFS_INIT_REQUEST_HDR(request, req, _op)                \
         do {                                                   \
            request->header.id = HgfsKReq_GetId(req);           \
            request->header.op = _op;                           \
         } while(0)

/* FreeBSD times support nsecs, so only use these functions directly */
#define HGFS_SET_TIME(unixtm, nttime)                   \
         HgfsConvertFromNtTimeNsec(&unixtm, nttime)
#define HGFS_GET_TIME(unixtm)                           \
         HgfsConvertTimeSpecToNtTime(&unixtm)

/* Determine if this is the root vnode. */
#define HGFS_IS_ROOT_VNODE(sip, vp)                     \
                (sip->rootVnode == vp)


/*
 * Local functions (prototypes)
 */

static vop_lookup_t     HgfsVopLookup;
static vop_create_t	HgfsVopCreate;
static vop_open_t	HgfsVopOpen;
static vop_close_t	HgfsVopClose;
static vop_access_t	HgfsVopAccess;
static vop_getattr_t	HgfsVopGetattr;
static vop_setattr_t	HgfsVopSetattr;
static vop_read_t	HgfsVopRead;
static vop_write_t	HgfsVopWrite;
static vop_remove_t	HgfsVopRemove;
static vop_rename_t	HgfsVopRename;
static vop_mkdir_t	HgfsVopMkdir;
static vop_rmdir_t	HgfsVopRmdir;
static vop_readdir_t	HgfsVopReaddir;
static vop_inactive_t   HgfsVopInactive;
static vop_reclaim_t	HgfsVopReclaim;
static vop_print_t	HgfsVopPrint;

/* Local vnode functions */
static int HgfsDirOpen(HgfsSuperInfo *sip, struct vnode *vp);
static int HgfsFileOpen(HgfsSuperInfo *sip, struct vnode *vp,
                        int flag, int permissions);
static int HgfsDirClose(HgfsSuperInfo *sip, struct vnode *vp);
static int HgfsFileClose(HgfsSuperInfo *sip, struct vnode *vp);
static int HgfsGetNextDirEntry(HgfsSuperInfo *sip, HgfsHandle handle,
                               uint32_t offset, char *nameOut, size_t nameSize,
                               HgfsFileType *type, Bool *done);
static int HgfsDoRead(HgfsSuperInfo *sip, HgfsHandle handle, uint64_t offset,
                      uint32_t size, struct uio *uiop);
static int HgfsDoWrite(HgfsSuperInfo *sip, HgfsHandle handle, int ioflag,
                       uint64_t offset, uint32_t size, struct uio *uiop);
static int HgfsDelete(HgfsSuperInfo *sip, const char *filename, HgfsOp op);

/* Local utility functions */
static int HgfsSubmitRequest(HgfsSuperInfo *sip, HgfsKReqHandle req);
static int HgfsValidateReply(HgfsKReqHandle req, uint32_t minSize);
static int HgfsEscapeBuffer(char const *bufIn, uint32 sizeIn,
                            uint32 sizeBufOut, char *bufOut);
static int HgfsUnescapeBuffer(char *bufIn, uint32 sizeIn);
static void HgfsAttrToBSD(struct vnode *vp, const HgfsAttr *hgfsAttr,
                          struct vattr *BSDAttr);
static Bool HgfsSetattrCopy(struct vattr *vap, HgfsAttr *hgfsAttr,
                            HgfsAttrChanges *update);
static int HgfsMakeFullName(const char *path, uint32_t pathLen, const char *file,
                            size_t fileLen, char *outBuf, ssize_t bufSize);
static int HgfsGetOpenMode(uint32 flags);
static int HgfsGetOpenFlags(uint32 flags);


/*
 * Global data
 */

/*
 * HGFS vnode operations vector
 */
struct vop_vector HgfsVnodeOps = {
   .vop_lookup          = HgfsVopLookup,
   .vop_create          = HgfsVopCreate,
   .vop_open            = HgfsVopOpen,
   .vop_close           = HgfsVopClose,
   .vop_access          = HgfsVopAccess,
   .vop_getattr         = HgfsVopGetattr,
   .vop_setattr         = HgfsVopSetattr,
   .vop_read            = HgfsVopRead,
   .vop_write           = HgfsVopWrite,
   .vop_remove          = HgfsVopRemove,
   .vop_rename          = HgfsVopRename,
   .vop_mkdir           = HgfsVopMkdir,
   .vop_rmdir           = HgfsVopRmdir,
   .vop_readdir         = HgfsVopReaddir,
   .vop_inactive        = HgfsVopInactive,
   .vop_reclaim         = HgfsVopReclaim,
   .vop_print           = HgfsVopPrint,

   /*
    * The following operations are not supported directly by the Hgfs module,
    * so we fall back to the kernel's default support routines.  (Most cases
    * return EOPNOTSUPP or EINVAL.
    */
   .vop_advlock         = VOP_EINVAL,
   .vop_bmap            = vop_stdbmap,
   .vop_bypass          = VOP_EOPNOTSUPP,
   .vop_fsync           = VOP_NULL,
   .vop_getpages        = vop_stdgetpages,
   .vop_getwritemount   = vop_stdgetwritemount,
   .vop_ioctl           = VOP_ENOTTY,
   .vop_islocked        = vop_stdislocked,
   .vop_kqfilter        = vop_stdkqfilter,
   .vop_lease           = VOP_NULL,
   .vop_lock            = vop_stdlock,
   .vop_pathconf        = VOP_EINVAL,
   .vop_poll            = vop_nopoll,
   .vop_putpages        = vop_stdputpages,
   .vop_readlink        = VOP_EINVAL,
   .vop_revoke          = VOP_PANIC,
   .vop_unlock          = vop_stdunlock,
};


/*
 * Local functions (definitions)
 */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopLookup --
 *
 *    Looks in the provided directory for the specified filename.  If we cannot
 *    determine the vnode locally (i.e, the vnode is not the root vnode of the
 *    filesystem or the provided dvp), we send a getattr request to the server
 *    and allocate a vnode and internal filesystem state for this file.
 *
 * Results:
 *    Returns zero on success and ENOENT if the file cannot be found
 *    If file is found, a vnode representing the file is returned in vpp.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopLookup(struct vop_lookup_args *ap)
/*
struct vop_lookup_args {
   struct vnode *dvp;           // IN    : locked vnode of search directory
   struct vnode **vpp;          // IN/OUT: addr to store located (locked) vnode
   struct componentname *cnp;   // IN    : pathname component to search for
};
 */
{
   struct vnode *dvp = ap->a_dvp;
   struct vnode **vpp = ap->a_vpp;
   struct componentname *cnp = ap->a_cnp;

   HgfsRequestGetattr *request;
   HgfsReplyGetattr *reply;
   HgfsSuperInfo *sip;
   HgfsKReqHandle req;
   char *path = NULL;           // allocated from M_TEMP; free when done
   int ret = 0, len = 0;

   DEBUG(VM_DEBUG_ENTRY, "HgfsVopLookup(%.*s, %.*s).\n",
         HGFS_VP_TO_FILENAME_LENGTH(dvp), HGFS_VP_TO_FILENAME(dvp),
         (int)cnp->cn_namelen, cnp->cn_nameptr);

   /*
    * Get pointer to the superinfo.  If the device is not attached,
    * hgfsInstance will not be valid and we immediately return an error.
    */
   sip = HGFS_VP_TO_SIP(dvp);
   if (!sip) {
      DEBUG(VM_DEBUG_FAIL, "couldn't acquire superinfo.\n");
      return EDOOFUS;
   }

   /* Snag a pathname buffer */
   path = malloc(MAXPATHLEN, M_TEMP, M_WAITOK);

   /* Construct the full path for this lookup. */
   len = HgfsMakeFullName(HGFS_VP_TO_FILENAME(dvp),             // Path to this file
                          HGFS_VP_TO_FILENAME_LENGTH(dvp),      // Length of path
                          cnp->cn_nameptr,                      // File's name
                          cnp->cn_namelen,                      // Filename length
                          path,                                 // Destination buffer
                          MAXPATHLEN);                          // Size of dest buffer
   if (len < 0) {
      ret = EINVAL;
      goto out;
   }

   DEBUG(VM_DEBUG_LOAD, "full path is \"%s\"\n", path);

   /* See if the lookup is really for the root vnode. */
   if (strcmp(path, "/") == 0) {
      DEBUG(VM_DEBUG_INFO, "returning the root vnode.\n");
      *vpp = sip->rootVnode;
      /*
       * Note that this is the only vnode we maintain a reference count on; all
       * others are per-open-file and should only be given to the Kernel once.
       */
      vref(*vpp);
      goto out;
   }

   /*
    * Now that we know the full filename, we can check our hash table for this
    * file to prevent having to send a request to the Hgfs Server.  If we do
    * find this file in the hash table, this function will correctly create
    * a vnode and other per-open state for us.
    *
    * On an 'ls -l', this saves sending two requests for each file in the
    * directory.
    *
    * XXX
    * Note that this optimization leaves open the possibility that a file that
    * has been removed on the host will not be noticed as promptly by the
    * filesystem.  This shouldn't cause any problems, though, because as far
    * as we can tell this function is invoked internally by the kernel before
    * other operations.  That is, this function is called implicitly for path
    * traversal when user applications issue other system calls.  The operation
    * next performed on the vnode we create here should happen prior to
    * returning to the user application, so if that next operation fails
    * because the file has been deleted, the user won't see different behavior
    * than if this optimization was not included.
    */
   ret = HgfsFileNameToVnode(path, vpp, sip, sip->vfsp, &sip->fileHashTable);
   if (ret == 0) {
      /*
       * The filename was in our hash table and we successfully created new
       * per-open state for it.
       */
      DEBUG(VM_DEBUG_DONE, "created per-open state from filename.\n");
      goto out;
   }

   /*
    * We don't have any reference to this vnode, so we must send a get
    * attribute request to see if the file exists and create one.
    */
   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return EIO;
   }

   /* Fill in the header of this request. */
   request = (HgfsRequestGetattr *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_GETATTR);

   /* Fill in the filename portion of the request. */
   ret = CPName_ConvertTo(path, MAXPATHLEN, request->fileName.name);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "CPName_ConvertTo failed.\n");
      ret = ENAMETOOLONG;
      goto destroy_out;
   }
   ret = HgfsUnescapeBuffer(request->fileName.name, ret);
   request->fileName.length = ret;

   /* Packet size includes the request and its payload. */
   HgfsKReq_SetPayloadSize(req, request->fileName.length + sizeof *request);

   DEBUG(VM_DEBUG_COMM, "sending getattr request for ID %d\n",
         request->header.id);
   DEBUG(VM_DEBUG_COMM, " fileName.length: %d\n", request->fileName.length);
   DEBUG(VM_DEBUG_COMM, " fileName.name: \"%s\"\n", request->fileName.name);

   /*
    * Submit the request and wait for the reply.  HgfsSubmitRequest handles
    * destroying the request on both error and interrupt cases.
    */
   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      goto out;
   }

   /* The reply is in the request's packet */
   reply = (HgfsReplyGetattr *)HgfsKReq_GetPayload(req);

   /* Validate the reply was COMPLETED and at least contains a header */
   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
      DEBUG(VM_DEBUG_FAIL, "invalid reply received for ID %d "
            "with status %d.\n", reply->header.id, reply->header.status);
      ret = EPROTO;
      goto destroy_out;
   }

   DEBUG(VM_DEBUG_COMM, "received reply for ID %d\n", reply->header.id);
   DEBUG(VM_DEBUG_COMM, " status: %d (see hgfsProto.h)\n", reply->header.status);
   DEBUG(VM_DEBUG_COMM, " file type: %d\n", reply->attr.type);
   DEBUG(VM_DEBUG_COMM, " file size: %"FMT64"u\n", reply->attr.size);
   DEBUG(VM_DEBUG_COMM, " permissions: %o\n", reply->attr.permissions);

   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      /* Ensure packet contains correct amount of data */
      if (HgfsKReq_GetPayloadSize(req) != sizeof *reply) {
         DEBUG(VM_DEBUG_COMM,
               "HgfsLookup: invalid packet size received for \"%s\".\n",
               cnp->cn_nameptr);
         ret = EFAULT;
         goto destroy_out;
      }
      /* Success */
      break;

   case HGFS_STATUS_OPERATION_NOT_PERMITTED:
      DEBUG(VM_DEBUG_LOG, "operation not permitted on \"%s\".\n",
            cnp->cn_nameptr);
      ret = EACCES;
      goto destroy_out;

   case HGFS_STATUS_NO_SUCH_FILE_OR_DIR:
   case HGFS_STATUS_INVALID_NAME:
      DEBUG(VM_DEBUG_LOG, "\"%s\" does not exist.\n",
            cnp->cn_nameptr);

      /*
       * If this is the final pathname component & the user is attempt a CREATE
       * or RENAME, just return without a leaf vnode.  (This differs from
       * FreeBSD where ENOENT would be returned in all cases.)
       */
      if ((cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME) &&
          cnp->cn_flags & ISLASTCN) {
         ret = EJUSTRETURN;
      } else {
         ret = ENOENT;
      }
      goto destroy_out;

   default:
      DEBUG(VM_DEBUG_LOG, "error on \"%s\".\n", cnp->cn_nameptr);
      ret = EFAULT;
      goto destroy_out;
   }

   /*
    * We need to create a vnode for this found file to give back to the Kernel.
    * Note that v_mount of the filesystem's root vnode was set properly in
    * HgfsMount(), so that value (dvp->v_mount) propagates down to each vnode.
    */
   ret = HgfsVnodeGet(vpp,                      // Location to write vnode's address
                      sip,                      // Superinfo
                      dvp->v_mount,             // VFS for our filesystem
                      dvp->v_op,                // Vnode op vector
                      path,                     // Full name of the file
                      reply->attr.type,         // Type of file
                      &sip->fileHashTable);     // File hash table

   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "couldn't create vnode for \"%s\".\n", path);
      ret = EFAULT;
      goto destroy_out;
   }
   /* HgfsVnodeGet guarantees this. */
   ASSERT(*vpp);

   DEBUG(VM_DEBUG_LOAD, "assigned vnode %p to %s\n", *vpp, path);

   ret = 0;     /* Return success */

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);

out:
   if (path != NULL) {
      free(path, M_TEMP);
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopCreate --
 *
 *    This entry point is invoked when a user calls open(2) with the O_CREAT
 *    flag specified.  The kernel calls our open entry point (HgfsOpen()) after
 *    calling this function, so here all we do is consruct the vnode and
 *    save the filename and permission bits for the file to be created within
 *    our filesystem internal state.
 *
 * Results:
 *    Returns zero on success and an appropriate error code on error.
 *
 * Side effects:
 *    If the file exists, the vnode is duplicated since they are kepy per-open.
 *    If the file doesn't exist, a vnode will be created.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopCreate(struct vop_create_args *ap)
/*
struct vop_create {
   struct vnode *dvp;           // IN : locked directory vnode
   struct vnode **vp;           // OUT: location to place resultant locked vnode
   struct componentname *cnp;   // IN : pathname component created
   struct vattr *vap;           // IN : attributes to create new object with
 */
{
   struct vnode *dvp = ap->a_dvp;
   struct vnode **vpp = ap->a_vpp;
   struct componentname *cnp = ap->a_cnp;
   struct vattr *vap = ap->a_vap;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(dvp);
   char *fullname = NULL;       // allocated from M_TEMP; free when done.
   int ret = 0;

   if (*vpp != NULL) {
      DEBUG(VM_DEBUG_ALWAYS, "vpp (%p) not null\n", vpp);
   }

   /*
    * There are two cases: either the file already exists or it doesn't.  If
    * the file exists already then *vpp points to its vnode that was allocated
    * in HgfsLookup().  In both cases we need to create a new vnode (since our
    * vnodes are per-open-file, not per-file), but we don't need to construct
    * the full name again if we already have it in the existing vnode.
    */
   fullname = malloc(MAXPATHLEN, M_TEMP, M_WAITOK);

   ret = HgfsMakeFullName(HGFS_VP_TO_FILENAME(dvp),  // Name of directory to create in
                          HGFS_VP_TO_FILENAME_LENGTH(dvp), // Length of name
                          cnp->cn_nameptr,           // Name of file to create
                          cnp->cn_namelen,           // Length of new filename
                          fullname,                  // Buffer to write full name
                          MAXPATHLEN);               // Size of this buffer

   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "couldn't create full path name.\n");
      ret = ENAMETOOLONG;
      goto out;
   }

   /* Create the vnode for this file. */
   ret = HgfsVnodeGet(vpp, sip, dvp->v_mount, dvp->v_op, fullname,
                      HGFS_FILE_TYPE_REGULAR, &sip->fileHashTable);
   if (ret) {
      goto out;
   }

   /* HgfsVnodeGet() guarantees this. */
   ASSERT(*vpp);

   /* Save the mode so when open is called we can reference it. */
   HgfsSetOpenFileMode(*vpp, vap->va_mode);

out:
   if (fullname != NULL) {
      free(fullname, M_TEMP);
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopOpen --
 *
 *    Invoked when open(2) is called on a file in our filesystem.  Sends an
 *    OPEN request to the Hgfs server with the filename of this vnode.
 *
 *    "Opens a file referenced by the supplied vnode.  The open() system call
 *    has already done a vop_lookup() on the path name, which returned a vnode
 *    pointer and then calls to vop_open().  This function typically does very
 *    little since most of the real work was performed by vop_lookup()."
 *    (Solaris Internals, p537)
 *
 * Results:
 *    Returns 0 on success and an error code on error.
 *
 * Side effects:
 *    The HgfsOpenFile for this file is given a handle that can be used on
 *    future read and write requests.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopOpen(struct vop_open_args *ap)
/*
struct vop_open_args {
   struct vnode *vp;    // IN: vnode of file to open
   int mode;            // IN: access mode requested by calling process
   struct ucred *cred;  // IN: calling process's user's credentials
   struct thread *td;   // IN: thread accessing file
   int fdidx;           // IN: file descriptor number
};
*/
{
   struct vnode *vp = ap->a_vp;
   int mode = ap->a_mode;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);

   /*
    * Each lookup should return its own vnode, so if we've been given a vnode
    * already in possession of a handle, something is awry.
    */
   ASSERT(HgfsHandleIsSet(vp) == FALSE);

   switch(vp->v_type) {
   case VDIR:
      DEBUG(VM_DEBUG_COMM, "opening a directory\n");
      return HgfsDirOpen(sip, vp);

   case VREG:
      {
         HgfsMode hmode = 0;

         /*
          * If HgfsCreate() was called prior to this, this fills in the mode we
          * saved there.  It's okay if this fails since often HgfsCreate()
          * won't have been called.
          */
         HgfsGetOpenFileMode(vp, &hmode);

         DEBUG(VM_DEBUG_COMM, "opening a file with flag %x\n", mode);
         return HgfsFileOpen(sip, vp, mode, hmode);
      }

   default:
      DEBUG(VM_DEBUG_FAIL,
            "HgfsOpen: unrecognized file of type %d.\n", vp->v_type);
      return EINVAL;
   }

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopClose --
 *
 *    Invoked when a user calls close(2) on a file in our filesystem.  Sends
 *    a CLOSE request to the Hgfs server with the filename of this vnode.
 *
 *    "Closes the file given by the supplied vnode.  When this is the last
 *    close, some filesystems use vop_close() to initiate a writeback of
 *    outstanding dirty pages by checking the reference cound in the vnode."
 *    (Solaris Internals, p536)
 *
 * Results:
 *    Returns 0 on success and an error code on error.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopClose(struct vop_close_args *ap)
/*
struct vop_close_args {
   struct vnode *vp;    // IN: vnode of object to close [exclusive lock held]
   int fflag;           // IN: F* flags (FWRITE, etc) on object
   struct ucred *cred;  // IN: calling process's user's credentials
   struct thread *td;   // IN: thread accessing file
};
*/
{
   struct vnode *vp = ap->a_vp;
   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);

   /*
    * If we are closing a directory we need to send a SEARCH_CLOSE request,
    * but if we are closing a regular file we need to send a CLOSE request.
    * Other file types are not supported by the Hgfs protocol.
    */

   switch (vp->v_type) {
   case VDIR:
      return HgfsDirClose(sip, vp);

   case VREG:
      return HgfsFileClose(sip, vp);

   default:
      DEBUG(VM_DEBUG_FAIL, "unsupported filetype %d.\n", vp->v_type);
      return EINVAL;
   }

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopAccess --
 *
 *    This function is invoked when the user calls access(2) on a file in our
 *    filesystem.  It checks to ensure the user has the specified type of
 *    access to the file.
 *
 *    We send a GET_ATTRIBUTE request by calling HgfsGetattr() to get the mode
 *    (permissions) for the provided vnode.
 *
 * Results:
 *    Returns 0 if access is allowed and a non-zero error code otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopAccess(struct vop_access_args *ap)
/*
struct vop_access_args {
   struct vnode *vp;    // IN: vnode of file to check
   int mode;            // IN: type of access required (mask of VREAD|VWRITE|VEXEC)
   struct ucred *cred;  // IN: calling process's user's credentials
   struct thread *td;   // IN: thread accessing file
};
*/
{
   struct vop_getattr_args ga;
   struct vattr va;
   int mode = ap->a_mode;
   int ret = 0;

   /* Build VOP_GETATTR argument container */
   ga.a_vp = ap->a_vp;
   ga.a_vap = &va;
   ga.a_cred = ap->a_cred;
   ga.a_td = ap->a_td;

   /* Get the attributes for this file from the Hgfs server. */
   ret = HgfsVopGetattr(&ga);
   if (ret) {
      return ret;
   }

   DEBUG(VM_DEBUG_INFO, "vp's mode: %o\n", va.va_mode);

   /*
    * mode is the desired access from the caller, and is composed of S_IREAD,
    * S_IWRITE, and S_IEXEC from <sys/stat.h>.  Since the mode of the file is
    * guaranteed to only contain owner permissions (by the Hgfs server), we
    * don't need to shift any bits.
    */
   if ((mode & S_IREAD) && !(va.va_mode & S_IREAD)) {
      DEBUG(VM_DEBUG_FAIL, "read access not allowed (%s).\n",
            HGFS_VP_TO_FILENAME(ap->a_vp));
      return EPERM;
   }

   if ((mode & S_IWRITE) && !(va.va_mode & S_IWRITE)) {
      DEBUG(VM_DEBUG_FAIL, "write access not allowed (%s).\n",
            HGFS_VP_TO_FILENAME(ap->a_vp));
      return EPERM;
   }

   if ((mode & S_IEXEC) && !(va.va_mode & S_IEXEC)) {
      DEBUG(VM_DEBUG_FAIL, "execute access not allowed (%s).\n",
            HGFS_VP_TO_FILENAME(ap->a_vp));
      return EPERM;
   }

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopGetattr --
 *
 *    "Gets the attributes for the supplied vnode." (Solaris Internals, p536)
 *
 * Results:
 *    Zero if successful, an errno-type value otherwise.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopGetattr(struct vop_getattr_args *ap)
/*
struct vop_getattr_args {
   struct vnode *vp;    // IN : vnode of file
   struct vattr *vap;   // OUT: attribute container
   struct ucred *cred;  // IN : calling process's user's credentials
   struct thread *td;   // IN : thread accessing file
};
*/
{
   struct vnode *vp = ap->a_vp;
   struct vattr *vap = ap->a_vap;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);
   HgfsKReqHandle req;
   HgfsRequestGetattr *request;
   HgfsReplyGetattr *reply;
   int ret;

   DEBUG(VM_DEBUG_ENTRY, "HgfsGetattr().\n");

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return EIO;
   }

   request = (HgfsRequestGetattr *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_GETATTR);

   /*
    * Now we need to convert the filename to cross-platform and unescaped
    * format.
    */
   ret = CPName_ConvertTo(HGFS_VP_TO_FILENAME(vp), MAXPATHLEN, request->fileName.name);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "CPName_ConvertTo failed.\n");
      ret = ENAMETOOLONG;
      goto destroy_out;
   }

   ret = HgfsUnescapeBuffer(request->fileName.name, ret);       /* cannot fail */
   request->fileName.length = ret;

   HgfsKReq_SetPayloadSize(req, sizeof *request + request->fileName.length);

   /*
    * Now submit request and wait for reply.  The request's state will be
    * properly set to COMPLETED, ERROR, or ABANDONED after calling
    * HgfsSubmitRequest()
    */
   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest destroys the request if necessary */
      goto out;
   }

   reply = (HgfsReplyGetattr *)HgfsKReq_GetPayload(req);

   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
      DEBUG(VM_DEBUG_FAIL, "reply not valid.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      /* Make sure we got all of the attributes */
      if (HgfsKReq_GetPayloadSize(req) != sizeof *reply) {
         DEBUG(VM_DEBUG_FAIL, "packet too small.\n");
         ret = EFAULT;
         break;
      }

      DEBUG(VM_DEBUG_COMM, "received reply for ID %d\n",
            reply->header.id);
      DEBUG(VM_DEBUG_COMM, " status: %d (see hgfsProto.h)\n",
            reply->header.status);
      DEBUG(VM_DEBUG_COMM, " file type: %d\n", reply->attr.type);
      DEBUG(VM_DEBUG_COMM, " file size: %"FMT64"u\n", reply->attr.size);
      DEBUG(VM_DEBUG_COMM, " permissions: %o\n", reply->attr.permissions);
      DEBUG(VM_DEBUG_COMM, "filename %s\n", HGFS_VP_TO_FILENAME(vp));

      /* Map the Hgfs attributes into the FreeBSD attributes */
      HgfsAttrToBSD(vp, &reply->attr, vap);

      DEBUG(VM_DEBUG_DONE, "done.\n");
      break;

   /* We'll add other error codes based on status here */
   default:
      ret = EFAULT;
      break;
   }

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopSetattr --
 *
 *    Maps the FreeBSD attributes to Hgfs attributes (by calling
 *    HgfsSetattrCopy()) and sends a set attribute request to the Hgfs server.
 *
 *    "Sets the attributes for the supplied vnode." (Solaris Internals, p537)
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on error.
 *
 * Side effects:
 *    The file on the host will have new attributes.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopSetattr(struct vop_setattr_args *ap)
/*
struct vop_setattr_args {
   struct vnode *vp;    // IN: vnode of file
   struct vattr *vap;   // IN: attribute container
   struct ucred *cred;  // IN: calling process's user's credentials
   struct thread *td;   // IN: thread accessing file
};
*/
{
   struct vnode *vp = ap->a_vp;
   struct vattr *vap = ap->a_vap;

   HgfsSuperInfo *sip;
   HgfsKReqHandle req;
   HgfsRequestSetattr *request;
   HgfsReplySetattr *reply;
   int ret;

   sip = HGFS_VP_TO_SIP(vp);

   DEBUG(VM_DEBUG_ENTRY, "Called for %.*s\n",
         HGFS_VP_TO_FILENAME_LENGTH(vp), HGFS_VP_TO_FILENAME(vp));

   /*
    * As per HgfsSetattrCopy, Hgfs supports changing these attributes: permissions/mode
    * bits (va_mode), size (va_size), and access/write times (va_atime/va_mtime). All
    * other attributes are either ones that Hgfs does not know how to set, or ones that
    * it doesn't make sense to set at all.
    *
    * Because FreeBSD can pass in va_filerev = 0 and va_vaflags = 0 when doing setattr
    * after creating a file, we ignore those values as well.
    *
    * (VNON is used for va_type instead of VNOVAL, as it denotes a file type of
    * "None").
    */
   if ((vap->va_type != VNON) || (vap->va_nlink != VNOVAL) ||
       (vap->va_fsid != VNOVAL) || (vap->va_fileid != VNOVAL) ||
       (vap->va_blocksize != VNOVAL) || (vap->va_rdev != VNOVAL) ||
       ((int)vap->va_bytes != VNOVAL) || (vap->va_gen != VNOVAL) ||
       ((vap->va_filerev != VNOVAL) && (vap->va_filerev != 0)) ||
       (vap->va_flags != VNOVAL) ||
       ((vap->va_vaflags != VNOVAL) && (vap->va_vaflags != 0))) {
      DEBUG(VM_DEBUG_FAIL,
            "HgfsSetattr: You are not allowed to set one of those attributes.\n"
            "va_type = %d, va_nlink = %d, va_fsid = %d, va_fileid = %d\n"
            "va_blocksize = %d, va_rdev = %d, va_bytes = %d, va_gen = %d\n"
            "va_filerev = %d, va_flags = %d, va_vaflags = %d\n",
            vap->va_type, vap->va_nlink, vap->va_fsid, (int)vap->va_fileid,
            (int)vap->va_blocksize, vap->va_rdev, (int)vap->va_bytes,
            (int)vap->va_gen, (int)vap->va_filerev, (int)vap->va_flags, vap->va_vaflags);
      return EINVAL;
   }

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return EIO;
   }

   request = (HgfsRequestSetattr *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_SETATTR);

   /*
    * Fill the attributes and update fields of the request.  If no updates are
    * needed then we will just return success without sending the request.
    */
   if (HgfsSetattrCopy(vap, &request->attr, &request->update) == FALSE) {
      DEBUG(VM_DEBUG_DONE, "don't need to update attributes.\n");
      ret = 0;
      goto destroy_out;
   }

   /* Convert the filename to cross platform and escape its buffer. */
   ret = CPName_ConvertTo(HGFS_VP_TO_FILENAME(vp), MAXPATHLEN, request->fileName.name);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "CPName_ConvertTo failed.\n");
      ret = ENAMETOOLONG;
      goto destroy_out;
   }

   ret = HgfsUnescapeBuffer(request->fileName.name, ret);
   request->fileName.length = ret;

   /* The request's size includes the request and filename. */
   HgfsKReq_SetPayloadSize(req, sizeof *request + request->fileName.length);

   if (request->update) {
      ret = HgfsSubmitRequest(sip, req);
      if (ret) {
         /* HgfsSubmitRequest() destroys the request if necessary. */
         goto out;
      }

      reply = (HgfsReplySetattr *)HgfsKReq_GetPayload(req);

      if (HgfsValidateReply(req, sizeof *reply) != 0) {
         DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
         ret = EPROTO;
         goto destroy_out;
      }

      switch(reply->header.status) {
      case HGFS_STATUS_SUCCESS:
         /* Success handled after switch. */
         break;

      case HGFS_STATUS_NO_SUCH_FILE_OR_DIR:
      case HGFS_STATUS_INVALID_NAME:
         DEBUG(VM_DEBUG_FAIL, "no such file or directory.\n");
         ret = ENOENT;
         goto destroy_out;

      case HGFS_STATUS_OPERATION_NOT_PERMITTED:
         DEBUG(VM_DEBUG_FAIL, "operation not permitted.\n");
         ret = EPERM;
         goto destroy_out;

      default:
         DEBUG(VM_DEBUG_FAIL, "default error.\n");
         ret = EPROTO;
         goto destroy_out;
      }
   } /* else { they were trying to set filerev or vaflags, which we ignore } */

   /* Success */
   ret = 0;
   DEBUG(VM_DEBUG_DONE, "done.\n");

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopRead --
 *
 *    Invoked when a user calls read(2) on a file in our filesystem.
 *
 *    We call HgfsDoRead() to fill the user's buffer until the request is met
 *    or the file has no more data.  This is done since we can only transfer
 *    HGFS_IO_MAX bytes in any one request.
 *
 *    "Reads the range supplied for the given vnode.  vop_read() typically
 *    maps the requested range of a file into kernel memory and then uses
 *    vop_getpage() to do the real work." (Solaris Internals, p537)
 *
 * Results:
 *    Returns zero on success and an error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopRead(struct vop_read_args *ap)
/*
struct vop_read_args {
   struct vnode *vp;    // IN   : the vnode of the file
   struct uio *uio;     // INOUT: location of data to be read
   int ioflag;          // IN   : hints & other directives
   struct ucread *cred; // IN   : caller's credentials
};
*/
{
   struct vnode *vp = ap->a_vp;
   struct uio *uiop = ap->a_uio;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);
   HgfsHandle handle;
   uint64_t offset;
   int ret;

   DEBUG(VM_DEBUG_ENTRY, "entry.\n");

   /* We can't read from directories, that's what readdir() is for. */
   if (vp->v_type == VDIR) {
      DEBUG(VM_DEBUG_FAIL, "cannot read directories.\n");
      return EISDIR;
   }

   /* off_t is a signed quantity */
   if (uiop->uio_offset < 0) {
      DEBUG(VM_DEBUG_FAIL, "given negative offset.\n");
      return EINVAL;
   }

   /* This is where the user wants to start reading from in the file. */
   offset = uiop->uio_offset;

   /*
    * We need to get the handle for the requests sent to the Hgfs server.  Note
    * that this is guaranteed to not change until a close(2) is called on this
    * vnode, so it's safe and correct to acquire it outside the loop below.
    */
   ret = HgfsGetOpenFileHandle(vp, &handle);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "could not get handle.\n");
      return EINVAL;
   }

   /*
    * Here we loop around HgfsDoRead with requests less than or equal to
    * HGFS_IO_MAX until one of the following conditions is met:
    *  (1) All the requested data has been read
    *  (2) The file has no more data
    *  (3) An error occurred
    *
    * Since HgfsDoRead() calls uiomove(9), we know condition (1) is met when
    * the uio structure's uio_resid is decremented to zero.  If HgfsDoRead()
    * returns 0 we know condition (2) was met, and if it returns less than 0 we
    * know condtion (3) was met.
    */
   do {
      uint32_t size;

      DEBUG(VM_DEBUG_INFO, "offset=%"FMT64"d, uio_offset=%jd\n",
            offset, uiop->uio_offset);
      DEBUG(VM_DEBUG_HANDLE, "** handle=%d, file=%s\n",
            handle, HGFS_VP_TO_FILENAME(vp));

      /* Request at most HGFS_IO_MAX bytes */
      size = (uiop->uio_resid > HGFS_IO_MAX) ? HGFS_IO_MAX : uiop->uio_resid;

      /* Send one read request. */
      ret = HgfsDoRead(sip, handle, offset, size, uiop);
      if (ret == 0) {
         /* On end of file we return success */
         DEBUG(VM_DEBUG_DONE, "end of file reached.\n");
         return 0;
      } else  if (ret < 0) {
         /*
          * HgfsDoRead() returns the negative of an appropriate error code to
          * differentiate between success and error cases.  We flip the sign
          * and return the appropriate error code.  See the HgfsDoRead()
          * function header for a fuller explanation.
          */
         DEBUG(VM_DEBUG_FAIL, "HgfsDoRead() failed.\n");
         return -ret;
      }

      /* Bump the offset past where we have already read. */
      offset += ret;
   } while (uiop->uio_resid);

   /* We fulfilled the user's read request, so return success. */
   DEBUG(VM_DEBUG_DONE, "done.\n");
   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopWrite --
 *
 *    This is invoked when a user calls write(2) on a file in our filesystem.
 *
 *    We call HgfsDoWrite() once with requests less than or equal to
 *    HGFS_IO_MAX bytes until the user's write request has completed.
 *
 *    "Writes the range supplied for the given vnode.  The write system call
 *    typically maps the requested range of a file into kernel memory and then
 *    uses vop_putpage() to do the real work." (Solaris Internals, p538)
 *
 * Results:
 *    Returns 0 on success and error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopWrite(struct vop_write_args *ap)
/*
struct vop_write_args {
   struct vnode *vp;    // IN   :
   struct uio *uio;     // INOUT:
   int ioflag;          // IN   :
   struct ucred *cred;  // IN   :
};
*/
{
   struct vnode *vp = ap->a_vp;
   struct uio *uiop = ap->a_uio;
   int ioflag = ap->a_ioflag;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);
   HgfsHandle handle;
   uint64_t offset;
   int ret;

   DEBUG(VM_DEBUG_ENTRY, "entry. (vp=%p)\n", vp);
   DEBUG(VM_DEBUG_INFO, "***ioflag=%x, uio_resid=%d\n",
         ioflag, uiop->uio_resid);

   /* Skip write requests for 0 bytes. */
   if (uiop->uio_resid == 0) {
      DEBUG(VM_DEBUG_INFO, "write of 0 bytes requested.\n");
      return 0;
   }

   DEBUG(VM_DEBUG_INFO, "file is %s\n", HGFS_VP_TO_FILENAME(vp));

   /* Off_t is a signed type. */
   if (uiop->uio_offset < 0) {
      DEBUG(VM_DEBUG_FAIL, "given negative offset.\n");
      return EINVAL;
   }

   /* This is where the user will begin writing into the file. */
   offset = uiop->uio_offset;

   /* Get the handle we need to supply the Hgfs server. */
   ret = HgfsGetOpenFileHandle(vp, &handle);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "could not get handle.\n");
      return EINVAL;
   }

   /*
    * We loop around calls to HgfsDoWrite() until either (1) we have written all
    * of our data or (2) an error has occurred.  uiop->uio_resid is decremented
    * by uiomove(9F) inside HgfsDoWrite(), so condition (1) is met when it
    * reaches zero.  Condition (2) occurs when HgfsDoWrite() returns less than
    * zero.
    */
   do {
      uint32_t size;

      DEBUG(VM_DEBUG_INFO, "** offset=%"FMT64"d, uio_offset=%jd\n",
            offset, uiop->uio_offset);
      DEBUG(VM_DEBUG_HANDLE, "** handle=%d, file=%s\n",
            handle, HGFS_VP_TO_FILENAME(vp));

      /* Write at most HGFS_IO_MAX bytes. */
      size = (uiop->uio_resid > HGFS_IO_MAX) ? HGFS_IO_MAX : uiop->uio_resid;

      /* Send one write request. */
      ret = HgfsDoWrite(sip, handle, ioflag, offset, size, uiop);
      if (ret < 0) {
         /*
          * As in HgfsRead(), we need to flip the sign.  See the comment in the
          * function header of HgfsDoWrite() for a more complete explanation.
          */
         DEBUG(VM_DEBUG_INFO, "HgfsDoWrite failed, returning %d\n", -ret);
         return -ret;
      }

      /* Increment the offest by the amount already written. */
      offset += ret;

   } while (uiop->uio_resid);

   /* We have completed the user's write request, so return success. */
   DEBUG(VM_DEBUG_DONE, "done.\n");
   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopRemove --
 *
 *    Composes the full pathname of this file and sends a DELETE_FILE request
 *    by calling HgfsDelete().
 *
 *    "Removes the file for the supplied vnode." (Solaris Internals, p537)
 *
 * Results:
 *    Returns 0 on success or a non-zero error code on error.
 *
 * Side effects:
 *    If successful, the file specified will be deleted from the host's
 *    filesystem.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopRemove(struct vop_remove_args *ap)
/*
struct vop_remove_args {
   struct vnode *dvp;           // IN: parent directory
   struct vnode *vp;            // IN: vnode to remove
   struct componentname *cnp;   // IN: file's pathname information
*/
{
   struct vnode *vp = ap->a_vp;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);

   DEBUG(VM_DEBUG_ENTRY, "HgfsRemove().\n");

   /* Removing directories is a no-no; save that for VOP_RMDIR. */
   if (vp->v_type == VDIR) {
      return EPERM;
   }

   /* We can now send the delete request. */
   return HgfsDelete(sip, HGFS_VP_TO_FILENAME(vp), HGFS_OP_DELETE_FILE);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopRename --
 *
 *    Renames the provided source name in the source directory with the
 *    destination name in the destination directory.  A RENAME request is sent
 *    to the Hgfs server.
 *
 * Results:
 *    Returns 0 on success and an error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopRename(struct vop_rename_args *ap)
/*
struct vop_rename_args {
   struct vnode *fdvp;          // IN: "from" parent directory
   struct vnode *fvp;           // IN: "from" file
   struct componentname *fcnp:  // IN: "from" pathname info
   struct vnode *tdvp;          // IN: "to" parent directory
   struct vnode *tvp;           // IN: "to" file (if it exists)
   struct componentname *tcnp:  // IN: "to" pathname info
};
*/
{
   struct vnode *fdvp = ap->a_fdvp;
   struct vnode *fvp = ap->a_fvp;
   struct vnode *tdvp = ap->a_tdvp;
   struct vnode *tvp = ap->a_tvp;
   struct componentname *tcnp = ap->a_tcnp;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(fdvp);
   HgfsKReqHandle req;
   HgfsRequestRename *request;
   HgfsReplyRename *reply;
   HgfsFileName *newNameP;
   char *srcFullPath = NULL;    // will point to fvp's filename; don't free
   char *dstFullPath = NULL;    // allocated from M_TEMP; free when done.
   int ret = 0;

   /* No cross-device renaming. */
   if (fvp->v_mount != tdvp->v_mount) {
      return EXDEV;
   }

   /* Make the full path of the source. */
   srcFullPath = HGFS_VP_TO_FILENAME(fvp);

   /* Make the full path of the destination. */
   dstFullPath = malloc(MAXPATHLEN, M_TEMP, M_WAITOK);
   ret = HgfsMakeFullName(HGFS_VP_TO_FILENAME(tdvp), HGFS_VP_TO_FILENAME_LENGTH(tdvp),
                          tcnp->cn_nameptr, tcnp->cn_namelen, dstFullPath, MAXPATHLEN);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "could not construct full path of dest.\n");
      ret = ENAMETOOLONG;
      goto out;
   }

   /* Ensure both names will fit in one request. */
   if ((sizeof *request + strlen(srcFullPath) + strlen(dstFullPath))
       > HGFS_PACKET_MAX) {
      DEBUG(VM_DEBUG_FAIL, "names too big for one request.\n");
      ret = EPROTO;
      goto out;
   }

   /*
    * Now we can prepare and send the request.
    */
   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      ret = EIO;
      goto out;
   }

   request = (HgfsRequestRename *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_RENAME);

   /* Convert the source to cross platform and unescape its buffer. */
   ret = CPName_ConvertTo(srcFullPath, MAXPATHLEN, request->oldName.name);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL,
            "HgfsRename: couldn't convert source to cross platform name.\n");
      ret = ENAMETOOLONG;
      goto destroy_out;
   }

   ret = HgfsUnescapeBuffer(request->oldName.name, ret);
   request->oldName.length = ret;

   /*
    * The new name is placed directly after the old name in the packet and we
    * access it through this pointer.
    */
   newNameP = (HgfsFileName *)((char *)&request->oldName +
                               sizeof request->oldName +
                               request->oldName.length);

   /* Convert the destination to cross platform and unescape its buffer. */
   ret = CPName_ConvertTo(dstFullPath, MAXPATHLEN, newNameP->name);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL,
            "HgfsRename: couldn't convert destination to cross platform name.\n");
      ret = ENAMETOOLONG;
      goto destroy_out;
   }

   ret = HgfsUnescapeBuffer(newNameP->name, ret);
   newNameP->length = ret;

   /* The request's size includes the request and both filenames. */
   HgfsKReq_SetPayloadSize(req, sizeof *request + request->oldName.length + newNameP->length);

   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest() destroys the request if necessary. */
      goto out;
   }

   reply = (HgfsReplyRename *)HgfsKReq_GetPayload(req);

   /* Validate the reply's state and size. */
   if (HgfsValidateReply(req, sizeof *reply) != 0) {
      DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   /* Return appropriate value. */
   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      /* Handled after switch. */
      break;

   case HGFS_STATUS_OPERATION_NOT_PERMITTED:
      DEBUG(VM_DEBUG_FAIL, "operation not permitted.\n");
      ret = EACCES;
      goto destroy_out;

   case HGFS_STATUS_NOT_DIRECTORY:
      DEBUG(VM_DEBUG_FAIL, "not a directory.\n");
      ret = ENOTDIR;
      goto destroy_out;

   case HGFS_STATUS_DIR_NOT_EMPTY:
      DEBUG(VM_DEBUG_FAIL, "directory not empty.\n");
      ret = EEXIST;
      goto destroy_out;

   case HGFS_STATUS_NO_SUCH_FILE_OR_DIR:
   case HGFS_STATUS_INVALID_NAME:
      DEBUG(VM_DEBUG_FAIL, "no such file or directory.\n");
      ret = ENOENT;
      goto destroy_out;

   default:
      DEBUG(VM_DEBUG_FAIL, "default error.\n");
      ret = EPROTO;
      goto destroy_out;
   }


   /* Successfully renamed file. */
   ret = 0;
   DEBUG(VM_DEBUG_DONE, "done.\n");

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   if (dstFullPath != NULL) {
      free(dstFullPath, M_TEMP);
   }

   vrele(fdvp);
   vrele(fvp);

   vput(tdvp);
   if (tvp) {
      vput(tvp);
   }

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsMkdir --
 *
 *    Makes a directory named dirname in the directory specified by the dvp
 *    vnode by sending a CREATE_DIR request, then allocates a vnode for this
 *    new directory and writes its address into vpp.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    If successful, a directory is created on the host's filesystem.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopMkdir(struct vop_mkdir_args *ap)
/*
struct vop_mkdir_args {
   struct vnode *dvp;           // IN : directory vnode
   struct vnode **vpp;          // OUT: pointer to new directory vnode
   struct componentname *cnp;   // IN : pathname component created
   struct vattr *vap;           // IN : attributes to create directory with
};
*/
{
   struct vnode *dvp = ap->a_dvp;
   struct vnode **vpp = ap->a_vpp;
   struct componentname *cnp = ap->a_cnp;
   struct vattr *vap = ap->a_vap;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(dvp);
   HgfsKReqHandle req;
   HgfsRequestCreateDir *request;
   HgfsReplyCreateDir *reply;
   char *fullname = NULL;       // allocated from M_TEMP; free when done.
   int ret;

   DEBUG(VM_DEBUG_ENTRY, "dvp=%p (%s), dirname=%s, vap=%p, vpp=%p\n",
                         dvp, HGFS_VP_TO_FILENAME(dvp), cnp->cn_nameptr, vap,
                         *vpp);

   /*
    * We need to construct the full path of the directory to create then send
    * a CREATE_DIR request.  If successful we will create a vnode and fill in
    * vpp with a pointer to it.
    *
    * Note that unlike in HgfsCreate(), *vpp is always NULL.
    */

   /* Construct the complete path of the directory to create. */
   fullname = malloc(MAXPATHLEN, M_TEMP, M_WAITOK);
   ret = HgfsMakeFullName(HGFS_VP_TO_FILENAME(dvp),// Name of directory to create in
                          HGFS_VP_TO_FILENAME_LENGTH(dvp), // Length of name
                          cnp->cn_nameptr,         // Name of file to create
                          cnp->cn_namelen,         // Length of filename
                          fullname,                // Buffer to write full name
                          MAXPATHLEN);             // Size of this buffer

   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "couldn't create full path name.\n");
      ret = ENAMETOOLONG;
      goto out;
   }

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      ret = EIO;
      goto out;
   }

   /* Initialize the request's contents. */
   request = (HgfsRequestCreateDir *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_CREATE_DIR);

   request->permissions = (vap->va_mode & S_IRWXU) >> HGFS_ATTR_MODE_SHIFT;

   ret = CPName_ConvertTo(fullname, MAXPATHLEN, request->fileName.name);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "cross-platform name is too long.\n");
      ret = ENAMETOOLONG;
      goto destroy_out;
   }

   ret = HgfsUnescapeBuffer(request->fileName.name, ret);
   request->fileName.length = ret;

   /* Set the size of this request. */
   HgfsKReq_SetPayloadSize(req, sizeof *request + request->fileName.length);

   /* Send the request to guestd. */
   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* Request is destroyed in HgfsSubmitRequest() if necessary. */
      goto out;
   }

   reply = (HgfsReplyCreateDir *)HgfsKReq_GetPayload(req);

   if (HgfsValidateReply(req, sizeof *reply) != 0) {
      DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      /* Handled below switch. */
      break;

   case HGFS_STATUS_OPERATION_NOT_PERMITTED:
      DEBUG(VM_DEBUG_FAIL, "operation not permitted.\n");
      ret = EACCES;
      goto destroy_out;

   case HGFS_STATUS_FILE_EXISTS:
      DEBUG(VM_DEBUG_FAIL, "directory already exists.\n");
      ret = EEXIST;
      goto destroy_out;

   default:
      ret = EPROTO;
      goto destroy_out;
   }

   /* We now create the vnode for the new directory. */
   ret = HgfsVnodeGet(vpp, sip, dvp->v_mount, dvp->v_op, fullname,
                      HGFS_FILE_TYPE_DIRECTORY, &sip->fileHashTable);
   if (ret) {
      ret = EIO;
      goto destroy_out;
   }

   ASSERT(*vpp);        /* HgfsIget guarantees this. */
   ret = 0;

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   if (fullname != NULL) {
      free(fullname, M_TEMP);
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopRmdir --
 *
 *    Removes the specified name from the provided vnode.  Sends a DELETE
 *    request by calling HgfsDelete() with the filename and correct opcode to
 *    indicate deletion of a directory.
 *
 *    "Removes the directory pointed to by the supplied vnode." (Solaris
 *    Internals, p537)
 *
 * Results:
 *    Returns 0 on success and an error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopRmdir(struct vop_rmdir_args *ap)
/*
struct vop_rmdir_args {
   struct vnode *dvp;           // IN: parent directory vnode
   struct vnode *vp;            // IN: directory to remove
   struct componentname *cnp;   // IN: pathname information
};
*/
{
   struct vnode *dvp = ap->a_dvp;
   struct vnode *vp = ap->a_vp;
#ifdef VM_DEBUG_LEV
   struct componentname *cnp = ap->a_cnp;
#endif

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(dvp);

   DEBUG(VM_DEBUG_ENTRY, "HgfsRmdir().\n");

   DEBUG(VM_DEBUG_ENTRY, "dvp=%p (%s), nm=%s, vp=%p (%s)\n",
         dvp, (HGFS_VP_TO_FP(dvp)) ? HGFS_VP_TO_FILENAME(dvp) : "dvp->v_data null",
         cnp->cn_nameptr, vp,
         (HGFS_VP_TO_FP(vp)) ? HGFS_VP_TO_FILENAME(vp) : "vp->v_data null");

   return HgfsDelete(sip, HGFS_VP_TO_FILENAME(vp), HGFS_OP_DELETE_DIR);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopReaddir --
 *
 *    Reads as many entries from the directory as will fit in to the provided
 *    buffer.  Each directory entry is read by calling HgfsGetNextDirEntry().
 *
 *    "The vop_readdir() method reads chunks of the directory into a uio
 *    structure.  Each chunk can contain as many entries as will fit within
 *    the size supplied by the uio structure.  The uio_resid structure member
 *    shows the size of the getdents request in bytes, which is divided by the
 *    size of the directory entry made by the vop_readdir() method to
 *    calculate how many directory entries to return." (Solaris Internals,
 *    p555)
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopReaddir(struct vop_readdir_args *ap)
/*
struct vop_readdir_args {
   struct vnode *vp;    // IN   : directory to read from
   struct uio *uio;     // INOUT: where to read contents
   struct ucred *cred;  // IN   : caller's credentials
   int *eofflag;        // INOUT: end of file status
   int *ncookies;       // OUT  : used by NFS server only; ignored
   u_long **cookies;    // INOUT: used by NFS server only; ignored
};
*/
{
   struct vnode *vp = ap->a_vp;
   struct uio *uiop = ap->a_uio;
   int *eofp = ap->a_eofflag;

   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);
   HgfsHandle handle;
   uint64_t offset;
   Bool done;
   char *fullName = NULL;       /* Hashed to generate inode number */
   int ret;

   DEBUG(VM_DEBUG_ENTRY, "uiop->uio_resid=%d, "
         "uiop->uio_offset=%jd\n",
         uiop->uio_resid, uiop->uio_offset);

   /* uio_offset is a signed quantity. */
   if (uiop->uio_offset < 0) {
      DEBUG(VM_DEBUG_FAIL, "fed negative offset.\n");
      return EINVAL;
   }

   /*
    * In order to fill the user's buffer with directory entries, we must
    * iterate on HGFS_OP_SEARCH_READ requests until either the user's buffer is
    * full or there are no more entries.  Each call to HgfsGetNextDirEntry()
    * fills in the name and attribute structure for the next entry.  We then
    * escape that name and place it in a kernel buffer that's the same size as
    * the user's buffer.  Once there are no more entries or no more room in the
    * buffer, we copy it to user space.
    */

   /*
    * We need to get the handle for this open directory to send to the Hgfs
    * server in our requests.
    */
   ret = HgfsGetOpenFileHandle(vp, &handle);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "could not get handle.\n");
      return EINVAL;
   }

   /*
    * Allocate 1K (MAXPATHLEN) buffer for inode number generation.
    */
   fullName = malloc(MAXPATHLEN, M_HGFS, M_WAITOK);

   /*
    * Loop until one of the following conditions is met:
    *  o An error occurs while reading a directory entry
    *  o There are no more directory entries to read
    *  o The buffer is full and cannot hold the next entry
    *
    * We request dentries from the Hgfs server based on their index in the
    * directory.  The offset value is initialized to the value specified in
    * the user's io request and is incremented each time through the loop.
    *
    * dirp is incremented by the record length each time through the loop and
    * is used to determine where in the kernel buffer we write to.
    */
   for (offset = uiop->uio_offset, done = 0; /* Nothing */ ; offset++) {
      struct dirent dirent, *dirp = &dirent;
      char nameBuf[sizeof dirp->d_name];
      HgfsFileType fileType = HGFS_FILE_TYPE_REGULAR;

      DEBUG(VM_DEBUG_COMM,
            "HgfsReaddir: getting directory entry at offset %"FMT64"u.\n", offset);

      DEBUG(VM_DEBUG_HANDLE, "** handle=%d, file=%s\n",
            handle, HGFS_VP_TO_FILENAME(vp));

      bzero(dirp, sizeof *dirp);

      ret = HgfsGetNextDirEntry(sip, handle, offset, nameBuf, sizeof nameBuf,
                                &fileType, &done);
      /* If the filename was too long, we skip to the next entry ... */
      if (ret == EOVERFLOW) {
         continue;
      /* ... but if another error occurred, we return that error code ... */
      } else if (ret) {
         DEBUG(VM_DEBUG_FAIL, "failure occurred in HgfsGetNextDirEntry\n");
         goto out;
      /*
       * ... and if there are no more entries, we set the end of file pointer
       * and break out of the loop.
       */
      } else if (done == TRUE) {
         DEBUG(VM_DEBUG_COMM, "Done reading directory entries.\n");
         if (eofp != NULL) {
            *eofp = TRUE;
         }
         break;
      }

      /*
       * We now have the directory entry, so we sanitize the name and try to
       * put it in our buffer.
       */
      DEBUG(VM_DEBUG_COMM, "received filename \"%s\"\n", nameBuf);

      ret = HgfsEscapeBuffer(nameBuf, strlen(nameBuf), sizeof dirp->d_name, dirp->d_name);
      /* If the escaped name didn't fit in the buffer, skip to the next entry. */
      if (ret < 0) {
         DEBUG(VM_DEBUG_FAIL, "HgfsEscapeBuffer failed.\n");
         continue;
      }

      /* Fill in the directory entry. */
      dirp->d_namlen = ret;
      dirp->d_reclen = GENERIC_DIRSIZ(dirp);    // NB: d_namlen must be set first!
      dirp->d_type =
         (fileType == HGFS_FILE_TYPE_REGULAR) ? DT_REG :
         (fileType == HGFS_FILE_TYPE_DIRECTORY) ? DT_DIR :
         DT_UNKNOWN;

      /*
       * Make sure there is enough room in the buffer for the entire directory
       * entry.  If not, we just break out of the loop and copy what we have.
       */
      if (dirp->d_reclen > uiop->uio_resid) {
         DEBUG(VM_DEBUG_INFO, "ran out of room in the buffer.\n");
         break;
      }


      ret = HgfsMakeFullName(HGFS_VP_TO_FILENAME(vp),           // Directorie's name
                             HGFS_VP_TO_FILENAME_LENGTH(vp),    // Length
                             dirp->d_name,                      // Name of file
                             dirp->d_namlen,                    // Length of filename
                             fullName,                          // Destination buffer
                             MAXPATHLEN);                       // Size of this buffer

      /* Skip this entry if the full path was too long. */
      if (ret < 0) {
         continue;
      }

      /*
       * Place the node id, which serves the purpose of inode number, for this
       * filename directory entry.  As long as we are using a dirent64, this is
       * okay since ino_t is also a u_longlong_t.
       */
      HgfsNodeIdGet(&sip->fileHashTable, fullName, (uint32_t)ret,
                    &dirp->d_fileno);

      /* Copy out this directory entry. */
      ret = uiomove(dirp, dirp->d_reclen, uiop);
      if (ret) {
         DEBUG(VM_DEBUG_FAIL, "uiomove failed.\n");
         goto out;
      }
   }

   /*
    * uiomove(9) will have incremented the uio offset by the number of bytes
    * written.  We reset it here to the fs-specific offset in our directory so
    * the next time we are called it is correct.  (Note, this does not break
    * anything and /is/ how this field is intended to be used.)
    */
   uiop->uio_offset = offset;

   DEBUG(VM_DEBUG_DONE, "done (ret=%d, *eofp=%d).\n", ret, *eofp);
out:
   if (fullName != NULL) {
      free(fullName, M_HGFS);
   }
   DEBUG(VM_DEBUG_ENTRY, "exiting.\n");
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopInactive --
 *
 *    Called when vnode's use count reaches zero.
 *
 * Results:
 *    Unconditionally zero.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopInactive(struct vop_inactive_args *ap)
/*
struct vop_inactive_args {
   struct vnode *vp;    // IN: vnode to inactive
   struct thread *td;   // IN: caller's thread context
};
*/
{
   /*
    * Since we allocate vnodes once per file descriptor, a closed vnode
    * should be completely dissociated from its HgfsOpenFile.  We'll call
    * vgone(), which in turn will call our reclaim routine and be done with the
    * vnode completely.
    *
    * (Other file systems would simply put a vnode on an "unused" list such
    * that a subsequent re-open would require less work.)
    */
   vgone(ap->a_vp);
   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopReclaim --
 *
 *    Dissociates vnode from the underlying filesystem.
 *
 * Results:
 *    Zero on success, or an appropriate system error otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopReclaim(struct vop_reclaim_args *ap)
/*
struct vop_reclaim_args {
   struct vnode *vp;    // IN: vnode to reclaim
   struct thread *td;   // IN: caller's thread context
};
*/
{
   struct vnode *vp = ap->a_vp;
   HgfsSuperInfo *sip = HGFS_VP_TO_SIP(vp);

   HgfsVnodePut(vp, &sip->fileHashTable);
   vp->v_data = NULL;

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVopPrint --
 *
 *    This function is needed to fill in the HgfsVnodeOps structure.
 *    Right now it does nothing.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsVopPrint(struct vop_print_args *ap)
{
   return 0;
}


/*
 * Local vnode functions.
 *
 * (The rest of the functions in this file are only invoked by our code so they
 *  ASSERT() their pointer arguments.)
 */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsDirOpen --
 *
 *    Invoked when HgfsOpen() is called with a vnode of type VDIR.
 *
 *    Sends a SEARCH_OPEN request to the Hgfs server.
 *
 * Results:
 *    Returns zero on success and an error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsDirOpen(HgfsSuperInfo *sip, // IN: Superinfo pointer
            struct vnode *vp)   // IN: Vnode of directory to open
{
   int ret;
   HgfsKReqHandle req;
   HgfsRequestSearchOpen *request;
   HgfsReplySearchOpen *reply;

   ASSERT(sip);
   ASSERT(vp);

   DEBUG(VM_DEBUG_ENTRY, "opening \"%s\"\n", HGFS_VP_TO_FILENAME(vp));

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return EIO;
   }

   /* Set the correct header values */
   request = (HgfsRequestSearchOpen *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_SEARCH_OPEN);

   /*
    * Convert name to cross-platform and unescape.  If the vnode is the root of
    * our filesystem the Hgfs server expects an empty string.
    */
   ret = CPName_ConvertTo((HGFS_IS_ROOT_VNODE(sip, vp)) ? "" : HGFS_VP_TO_FILENAME(vp),
                          MAXPATHLEN, request->dirName.name);
   if (ret < 0) {
      ret = ENAMETOOLONG;
      goto destroy_out;
   }

   ret = HgfsUnescapeBuffer(request->dirName.name, ret);        /* cannot fail */
   request->dirName.length = ret;

   HgfsKReq_SetPayloadSize(req, request->dirName.length + sizeof *request);

   /* Submit the request to the Hgfs server */
   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest destroys the request if necessary */
      goto out;
   }

   /* Our reply is in the request packet */
   reply = (HgfsReplySearchOpen *)HgfsKReq_GetPayload(req);

   /* Perform basic validation of packet transfer */
   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
         DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
         ret = EPROTO;
         goto destroy_out;
   }

   DEBUG(VM_DEBUG_COMM, "received reply for ID %d\n", reply->header.id);
   DEBUG(VM_DEBUG_COMM, " status: %d (see hgfsProto.h)\n", reply->header.status);
   DEBUG(VM_DEBUG_COMM, " handle: %d\n", reply->search);

   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      if (HgfsKReq_GetPayloadSize(req) != sizeof *reply) {
         DEBUG(VM_DEBUG_FAIL, "incorrect packet size.\n");
         ret = EFAULT;
         goto destroy_out;
      }
      /* Success handled after switch. */
      break;

   case HGFS_STATUS_OPERATION_NOT_PERMITTED:
      DEBUG(VM_DEBUG_FAIL, "operation not permitted (%s).\n",
            HGFS_VP_TO_FILENAME(vp));
      ret = EACCES;
      goto destroy_out;

   default:
      DEBUG(VM_DEBUG_FAIL, "default error (%s).\n",
            HGFS_VP_TO_FILENAME(vp));
      ret = EPROTO;
      goto destroy_out;
   }

   /* Set the search open handle for use in HgfsReaddir() */
   ret = HgfsSetOpenFileHandle(vp, reply->search);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "couldn't assign handle=%d to %s\n",
         reply->search, HGFS_VP_TO_FILENAME(vp));
      ret = EINVAL;
      goto destroy_out;
   }

   ret = 0;     /* Return success */

destroy_out:
   /* Make sure we put the request back on the list */
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   DEBUG(VM_DEBUG_DONE, "done\n");
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsFileOpen --
 *
 *    Invoked when HgfsOpen() is called with a vnode of type VREG.  Sends
 *    a OPEN request to the Hgfs server.
 *
 *    Note that this function doesn't need to handle creations since the
 *    HgfsCreate() entry point is called by the kernel for that.
 *
 * Results:
 *    Returns zero on success and an error code on error.
 *
 * Side effects:
 *    None.
 *
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsFileOpen(HgfsSuperInfo *sip,        // IN: Superinfo pointer
             struct vnode *vp,          // IN: Vnode of file to open
             int flag,                  // IN: Flags of open
             int permissions)           // IN: Permissions of open (only when creating)
{
   HgfsKReqHandle req;
   HgfsRequestOpen *request;
   HgfsReplyOpen *reply;
   int ret;

   ASSERT(sip);
   ASSERT(vp);

   DEBUG(VM_DEBUG_ENTRY, "opening \"%s\"\n", HGFS_VP_TO_FILENAME(vp));

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      DEBUG(VM_DEBUG_FAIL, "HgfsKReq_AllocateRequest failed.\n");
      return EIO;
   }

   request = (HgfsRequestOpen *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_OPEN);

   /* Convert FreeBSD modes to Hgfs modes */
   ret = HgfsGetOpenMode((uint32_t)flag);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "HgfsGetOpenMode failed.\n");
      ret = EINVAL;
      goto destroy_out;
   }

   request->mode = ret;
   DEBUG(VM_DEBUG_COMM, "open mode is %x\n", request->mode);

   /* Convert FreeBSD flags to Hgfs flags */
   ret = HgfsGetOpenFlags((uint32_t)flag);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "HgfsGetOpenFlags failed.\n");
      ret = EINVAL;
      goto destroy_out;
   }

   request->flags = ret;
   DEBUG(VM_DEBUG_COMM, "open flags are %x\n", request->flags);

   request->permissions = (permissions & S_IRWXU) >> HGFS_ATTR_MODE_SHIFT;
   DEBUG(VM_DEBUG_COMM, "permissions are %o\n", request->permissions);

   /* Convert the file name to cross platform format. */
   ret = CPName_ConvertTo(HGFS_VP_TO_FILENAME(vp), MAXPATHLEN, request->fileName.name);
   if (ret < 0) {
      DEBUG(VM_DEBUG_FAIL, "CPName_ConvertTo failed.\n");
      ret = ENAMETOOLONG;
      goto destroy_out;
   }
   ret = HgfsUnescapeBuffer(request->fileName.name, ret);
   request->fileName.length = ret;

   /* Packet size includes the request and its payload. */
   HgfsKReq_SetPayloadSize(req, request->fileName.length + sizeof *request);

   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest will destroy the request if necessary. */
      DEBUG(VM_DEBUG_FAIL, "could not submit request.\n");
      goto out;
   }

   reply = (HgfsReplyOpen *)HgfsKReq_GetPayload(req);

   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
      DEBUG(VM_DEBUG_FAIL, "request not valid.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      if (HgfsKReq_GetPayloadSize(req) != sizeof *reply) {
         DEBUG(VM_DEBUG_FAIL, "size of reply is incorrect.\n");
         ret = EFAULT;
         goto destroy_out;
      }

      /* Success case is handled after switch. */
      break;

   case HGFS_STATUS_NO_SUCH_FILE_OR_DIR:
   case HGFS_STATUS_INVALID_NAME:
      DEBUG(VM_DEBUG_FAIL, "no such file \"%s\".\n",
            HGFS_VP_TO_FILENAME(vp));
      ret = ENOENT;
      goto destroy_out;

   case HGFS_STATUS_FILE_EXISTS:
      DEBUG(VM_DEBUG_FAIL, "\"%s\" exists.\n",
            HGFS_VP_TO_FILENAME(vp));
      ret = EEXIST;
      goto destroy_out;

   case HGFS_STATUS_OPERATION_NOT_PERMITTED:
      DEBUG(VM_DEBUG_FAIL, "operation not permitted on %s.\n",
            HGFS_VP_TO_FILENAME(vp));
      ret = EACCES;
      goto destroy_out;

   case HGFS_STATUS_ACCESS_DENIED:
      DEBUG(VM_DEBUG_FAIL, "access denied on %s.\n",
            HGFS_VP_TO_FILENAME(vp));
      ret = EACCES;
      goto destroy_out;

   default:
      DEBUG(VM_DEBUG_FAIL, "default/unknown error %d.\n",
            reply->header.status);
      ret = EACCES;
      goto destroy_out;
   }

   /*
    * We successfully received a reply, so we need to save the handle in
    * this file's HgfsOpenFile and return success.
    */
   ret = HgfsSetOpenFileHandle(vp, reply->file);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "couldn't assign handle %d (%s)\n",
            reply->file, HGFS_VP_TO_FILENAME(vp));
      ret = EINVAL;
      goto destroy_out;
   }

   ret = 0;


destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   DEBUG(VM_DEBUG_DONE, "returning %d\n", ret);
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsDirClose --
 *
 *    Invoked when HgfsClose() is called with a vnode of type VDIR.
 *
 *    Sends an SEARCH_CLOSE request to the Hgfs server.
 *
 * Results:
 *    Returns zero on success and an error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsDirClose(HgfsSuperInfo *sip,        // IN: Superinfo pointer
             struct vnode *vp)          // IN: Vnode of directory to close
{
   HgfsKReqHandle req;
   HgfsRequestSearchClose *request;
   HgfsReplySearchClose *reply;
   int ret;

   ASSERT(sip);
   ASSERT(vp);

   DEBUG(VM_DEBUG_ENTRY, "closing \"%s\"\n", HGFS_VP_TO_FILENAME(vp));

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return EIO;
   }

   /*
    * Prepare the request structure.  Of note here is that the request is
    * always the same size so we just set the packetSize to that.
    */
   request = (HgfsRequestSearchClose *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_SEARCH_CLOSE);

   /* Get this open file's handle, since that is what we want to close. */
   ret = HgfsGetOpenFileHandle(vp, &request->search);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "couldn't get handle for %s\n",
            HGFS_VP_TO_FILENAME(vp));
      ret = EINVAL;
      goto destroy_out;
   }
   HgfsKReq_SetPayloadSize(req, sizeof *request);

   /* Submit the request to the Hgfs server */
   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest destroys the request if necessary */
      goto out;
   }

   reply = (HgfsReplySearchClose *)HgfsKReq_GetPayload(req);

   /* Ensure reply was received correctly and is necessary size. */
   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
      DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   DEBUG(VM_DEBUG_COMM, "received reply for ID %d\n", reply->header.id);
   DEBUG(VM_DEBUG_COMM, " status: %d (see hgfsProto.h)\n", reply->header.status);

   /* Ensure server was able to close directory. */
   if (reply->header.status != HGFS_STATUS_SUCCESS) {
      ret = EFAULT;
      goto destroy_out;
   }

   /* Now clear this open file's handle for future use. */
   ret = HgfsClearOpenFileHandle(vp);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "couldn't clear handle.\n");
      ret = EINVAL;
      goto destroy_out;
   }

   /* The directory was closed successfully so we return success. */
   ret = 0;


destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsFileClose --
 *
 *    Invoked when HgfsClose() is called with a vnode of type VREG.
 *
 *    Sends a CLOSE request to the Hgfs server.
 *
 * Results:
 *    Returns zero on success and an error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsFileClose(HgfsSuperInfo *sip,       // IN: Superinfo pointer
              struct vnode *vp)         // IN: Vnode of file to close
{
   HgfsKReqHandle req;
   HgfsRequestClose *request;
   HgfsReplyClose *reply;
   int ret;

   ASSERT(sip);
   ASSERT(vp);

   DEBUG(VM_DEBUG_ENTRY, "closing \"%s\"\n", HGFS_VP_TO_FILENAME(vp));

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      ret = EFAULT;
      goto out;
   }

   request = (HgfsRequestClose *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_CLOSE);

   /* Tell the Hgfs server which handle to close */
   ret = HgfsGetOpenFileHandle(vp, &request->file);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL, "couldn't get handle.\n");
      ret = EINVAL;
      goto destroy_out;
   }

   HgfsKReq_SetPayloadSize(req, sizeof *request);

   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest will destroy the request if necessary. */
      DEBUG(VM_DEBUG_FAIL, "submit request failed.\n");
      goto out;
   }

   if (HgfsValidateReply(req, sizeof *reply) != 0) {
      DEBUG(VM_DEBUG_FAIL, "reply was invalid.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   reply = (HgfsReplyClose *)HgfsKReq_GetPayload(req);

   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      /*
       * We already verified the size of the reply above since this reply type
       * only contains a header, so we just clear the handle and return success.
       */
      ret = HgfsClearOpenFileHandle(vp);
      if (ret) {
         DEBUG(VM_DEBUG_FAIL, "couldn't clear handle.\n");
         ret = EINVAL;
         goto destroy_out;
      }

      ret = 0;
      goto destroy_out;

   case HGFS_STATUS_INVALID_HANDLE:
      DEBUG(VM_DEBUG_FAIL, "invalid handle error.\n");
      ret = EFAULT;     // XXX Is this really EPROTO?
      goto destroy_out;

   case HGFS_STATUS_OPERATION_NOT_PERMITTED:
      DEBUG(VM_DEBUG_FAIL, "operation not permitted error.\n");
      ret = EACCES;
      goto destroy_out;

   default:
      DEBUG(VM_DEBUG_FAIL, "other/unknown error.\n");
      ret = EPROTO;
      goto destroy_out;
   }

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   DEBUG(VM_DEBUG_DONE, "returning %d\n", ret);
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsGetNextDirEntry --
 *
 *    Writes the name of the directory entry matching the handle and offset to
 *    nameOut.  Also records the entry's type (file, directory) in type.  This
 *    requires sending a SEARCH_READ request.
 *
 * Results:
 *    Returns zero on success and an error code on error.  The done value is
 *    set if there are no more directory entries.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsGetNextDirEntry(HgfsSuperInfo *sip,         // IN: Superinfo pointer
                    HgfsHandle handle,          // IN: Handle for request
                    uint32_t offset,            // IN: Offset
                    char *nameOut,              // OUT: Location to write name
                    size_t nameSize,            // IN : Size of nameOut
                    HgfsFileType *type,         // OUT: Entry's type
                    Bool *done)                 // OUT: Whether there are any more
{
   HgfsKReqHandle req;
   HgfsRequestSearchRead *request;
   HgfsReplySearchRead *reply;
   int ret;

   DEBUG(VM_DEBUG_ENTRY,
         "HgfsGetNextDirEntry: handle=%d, offset=%d.\n", handle, offset);

   ASSERT(sip);
   ASSERT(nameOut);
   ASSERT(done);

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      DEBUG(VM_DEBUG_FAIL, "couldn't get req.\n");
      return EIO;
   }

   /*
    * Fill out the search read request that will return a single directory
    * entry for the provided handle at the given offset.
    */
   request = (HgfsRequestSearchRead *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_SEARCH_READ);

   request->search = handle;
   request->offset = offset;

   HgfsKReq_SetPayloadSize(req, sizeof *request);

   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest will destroy the request if necessary. */
      DEBUG(VM_DEBUG_FAIL, "HgfsSubmitRequest failed.\n");
      goto out;
   }

   reply = (HgfsReplySearchRead *)HgfsKReq_GetPayload(req);

   /* Validate the request state and ensure we have at least a header */
   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
      DEBUG(VM_DEBUG_FAIL, "reply not valid.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   DEBUG(VM_DEBUG_COMM, "received reply for ID %d\n",
         reply->header.id);
   DEBUG(VM_DEBUG_COMM, " status: %d (see hgfsProto.h)\n", reply->header.status);

   /* Now ensure the server didn't have an error */
   if (reply->header.status != HGFS_STATUS_SUCCESS) {
      DEBUG(VM_DEBUG_FAIL, "server didn't return success (%d).\n",
            reply->header.status);
      ret = EINVAL;
      goto destroy_out;
   }

   /* Make sure we got an entire reply (excluding filename) */
   if (HgfsKReq_GetPayloadSize(req) < sizeof *reply) {
      DEBUG(VM_DEBUG_FAIL, "server didn't provide entire reply.\n");
      ret = EFAULT;
      goto destroy_out;
   }

   /* See if there are no more filenames to read */
   if (reply->fileName.length <= 0) {
      DEBUG(VM_DEBUG_DONE, "no more directory entries.\n");
      *done = TRUE;
      ret = 0;         /* return success */
      goto destroy_out;
   }

   /* Make sure filename isn't too long */
   if ((reply->fileName.length >= nameSize) ||
       (reply->fileName.length > HGFS_PAYLOAD_MAX(reply)) ) {
      DEBUG(VM_DEBUG_FAIL, "filename is too long.\n");
      ret = EOVERFLOW;
      goto destroy_out;
   }

   /*
    * Everything is all right, copy filename to caller's buffer.  Note that even though
    * the hgfs SearchRead reply holds lots of information about the file's attributes,
    * FreeBSD directory entries do not currently need any of that information except the
    * file type.
    */
   memcpy(nameOut, reply->fileName.name, reply->fileName.length);
   nameOut[reply->fileName.length] = '\0';
   *type = reply->attr.type;
   ret = 0;

   DEBUG(VM_DEBUG_DONE, "done.\n");
destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsDoRead --
 *
 *    Sends a single READ request to the Hgfs server and writes the contents
 *    into the user's buffer if successful.
 *
 *    This function is called repeatedly by HgfsRead() with requests of size
 *    less than or equal to HGFS_IO_MAX.
 *
 *    Note that we return the negative of an appropriate error code in this
 *    function so we can differentiate between success and failure.  On success
 *    we need to return the number of bytes read, but FreeBSD's error codes are
 *    positive so we negate them before returning.  If callers want to return
 *    these error codes to the Kernel, they will need to flip their sign.
 *
 * Results:
 *    Returns number of bytes read on success and a negative value on error.
 *
 * Side effects:
 *    On success, size bytes are written into the user's buffer.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsDoRead(HgfsSuperInfo *sip,  // IN: Superinfo pointer
           HgfsHandle handle,   // IN: Server's handle to read from
           uint64_t offset,     // IN: File offset to read at
           uint32_t size,       // IN: Number of bytes to read
           struct uio *uiop)    // IN: Defines user's read request
{
   HgfsKReqHandle req;
   HgfsRequestRead *request;
   HgfsReplyRead *reply;
   int ret;

   ASSERT(sip);
   ASSERT(uiop);
   ASSERT(size <= HGFS_IO_MAX); // HgfsRead() should guarantee this

   DEBUG(VM_DEBUG_ENTRY, "entry.\n");

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return -EIO;
   }

   request = (HgfsRequestRead *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_READ);

   /* Indicate which file, where in the file, and how much to read. */
   request->file = handle;
   request->offset = offset;
   request->requiredSize = size;

   HgfsKReq_SetPayloadSize(req, sizeof *request);

   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /*
       * We need to flip the sign of the return value to indicate error; see
       * the comment in the function header.  HgfsSubmitRequest() handles
       * destroying the request if necessary, so we don't here.
       */
      ret = -ret;
      goto out;
   }

   reply = (HgfsReplyRead *)HgfsKReq_GetPayload(req);

   /* Ensure we got an entire header. */
   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
      DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
      ret = -EPROTO;
      goto destroy_out;
   }

   if (reply->header.status != HGFS_STATUS_SUCCESS) {
      DEBUG(VM_DEBUG_FAIL, "request not completed successfully.\n");
      ret = -EACCES;
      goto destroy_out;
   }

   /*
    * Now perform checks on the actualSize.  There are three cases:
    *  o actualSize is less than or equal to size, which indicates success
    *  o actualSize is zero, which indicates the end of the file (and success)
    *  o actualSize is greater than size, which indicates a server error
    */
   if (reply->actualSize <= size) {
      /* If we didn't get any data, we don't need to copy to the user. */
      if (reply->actualSize == 0) {
         goto success;
      }

      /* Perform the copy to the user */
      ret = uiomove(reply->payload, reply->actualSize, uiop);
      if (ret) {
         ret = -EIO;
         goto destroy_out;
      }

      /* We successfully copied the payload to the user's buffer */
      goto success;

   } else {
      /* We got too much data: server error. */
      DEBUG(VM_DEBUG_FAIL, "received too much data in payload.\n");
      ret = -EPROTO;
      goto destroy_out;
   }


success:
   ret = reply->actualSize;
   DEBUG(VM_DEBUG_DONE, "successfully read %d bytes to user.\n", ret);
destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsDoWrite --
 *
 *    Sends a single WRITE request to the Hgfs server with the contents of
 *    the user's buffer.
 *
 *    This function is called repeatedly by HgfsWrite() with requests of size
 *    less than or equal to HGFS_IO_MAX.
 *
 *    Note that we return the negative of an appropriate error code in this
 *    function so we can differentiate between success and failure.  On success
 *    we need to return the number of bytes written, but FreeBSD's error codes are
 *    positive so we negate them before returning.  If callers want to return
 *    these error codes to the kernel, they will need to flip their sign.
 *
 * Results:
 *    Returns number of bytes written on success and a negative value on error.
 *
 * Side effects:
 *    On success, size bytes are written to the file specified by the handle.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsDoWrite(HgfsSuperInfo *sip, // IN: Superinfo pointer
            HgfsHandle handle,  // IN: Handle representing file to write to
            int ioflag,         // IN: Flags for write
            uint64_t offset,    // IN: Where in the file to begin writing
            uint32_t size,      // IN: How much data to write
            struct uio *uiop)   // IN: Describes user's write request
{
   HgfsKReqHandle req;
   HgfsRequestWrite *request;
   HgfsReplyWrite *reply;
   int ret;

   ASSERT(sip);
   ASSERT(uiop);
   ASSERT(size <= HGFS_IO_MAX); // HgfsWrite() guarantees this

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return -EIO;
   }

   request = (HgfsRequestWrite *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, HGFS_OP_WRITE);

   request->file = handle;
   request->flags = 0;
   request->offset = offset;
   request->requiredSize = size;

   if (ioflag & IO_APPEND) {
      DEBUG(VM_DEBUG_COMM, "writing in append mode.\n");
      request->flags |= HGFS_WRITE_APPEND;
   }

   DEBUG(VM_DEBUG_COMM, "requesting write of %d bytes.\n", size);

   /* Copy the data the user wants to write into the payload. */
   ret = uiomove(request->payload, request->requiredSize, uiop);
   if (ret) {
      DEBUG(VM_DEBUG_FAIL,
            "HgfsDoWrite: uiomove(9F) failed copying data from user.\n");
      ret = -EIO;
      goto destroy_out;
   }

   /* We subtract one so request's 'char payload[1]' member isn't double counted. */
   HgfsKReq_SetPayloadSize(req, sizeof *request + request->requiredSize - 1);

   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /*
       * As in HgfsDoRead(), we need to flip the sign of the error code
       * returned by HgfsSubmitRequest().
       */
      DEBUG(VM_DEBUG_FAIL, "HgfsSubmitRequest failed.\n");
      ret = -ret;
      goto out;
   }

   reply = (HgfsReplyWrite *)HgfsKReq_GetPayload(req);

   if (HgfsValidateReply(req, sizeof reply->header) != 0) {
      DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
      ret = -EPROTO;
      goto destroy_out;
   }

   if (reply->header.status != HGFS_STATUS_SUCCESS) {
      DEBUG(VM_DEBUG_FAIL, "write failed (status=%d).\n",
            reply->header.status);
      ret = -EACCES;
      goto destroy_out;
   }

   if (HgfsKReq_GetPayloadSize(req) != sizeof *reply) {
      DEBUG(VM_DEBUG_FAIL,
            "HgfsDoWrite: invalid size of reply on successful reply.\n");
      ret = -EPROTO;
      goto destroy_out;
   }

   /* The write was completed successfully, so return the amount written. */
   ret = reply->actualSize;
   DEBUG(VM_DEBUG_DONE, "wrote %d bytes.\n", ret);

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsDelete --
 *
 *    Sends a request to delete a file or directory.
 *
 * Results:
 *    Returns 0 on success or an error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsDelete(HgfsSuperInfo *sip,          // IN: Superinfo
           const char *filename,        // IN: Full name of file to remove
           HgfsOp op)                   // IN: Hgfs operation this delete is for
{
   HgfsKReqHandle req;
   HgfsRequestDelete *request;
   HgfsReplyDelete *reply;
   int ret;

   ASSERT(sip);
   ASSERT(filename);
   ASSERT((op == HGFS_OP_DELETE_FILE) || (op == HGFS_OP_DELETE_DIR));

   DEBUG(VM_DEBUG_ENTRY, "HgfsDelete().\n");

   req = HgfsKReq_AllocateRequest(sip->reqs);
   if (!req) {
      return EIO;
   }

   /* Initialize the request's contents. */
   request = (HgfsRequestDelete *)HgfsKReq_GetPayload(req);
   HGFS_INIT_REQUEST_HDR(request, req, op);

   /* Convert filename to cross platform and unescape. */
   ret = CPName_ConvertTo(filename, MAXPATHLEN, request->fileName.name);
   if (ret < 0) {
      ret = ENAMETOOLONG;
      goto destroy_out;
   }

   ret = HgfsUnescapeBuffer(request->fileName.name, ret);
   request->fileName.length = ret;

   /* Set the size of our request. (XXX should this be - 1 for char[1]?) */
   HgfsKReq_SetPayloadSize(req, sizeof *request + request->fileName.length);

   DEBUG(VM_DEBUG_COMM, "deleting \"%s\"\n", filename);

   /* Submit our request to guestd. */
   ret = HgfsSubmitRequest(sip, req);
   if (ret) {
      /* HgfsSubmitRequest() handles destroying the request if necessary. */
      goto out;
   }

   reply = (HgfsReplyDelete *)HgfsKReq_GetPayload(req);

   /* Check the request status and size of reply. */
   if (HgfsValidateReply(req, sizeof *reply) != 0) {
      DEBUG(VM_DEBUG_FAIL, "invalid reply received.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   /* Return the appropriate value. */
   switch (reply->header.status) {
   case HGFS_STATUS_SUCCESS:
      ret = 0;
      break;

   case HGFS_STATUS_OPERATION_NOT_PERMITTED:
      DEBUG(VM_DEBUG_FAIL, "operation not permitted.\n");
      ret = EACCES;
      goto destroy_out;

   case HGFS_STATUS_NOT_DIRECTORY:
      DEBUG(VM_DEBUG_FAIL, "not a directory.\n");
      ret = ENOTDIR;
      goto destroy_out;

   case HGFS_STATUS_DIR_NOT_EMPTY:
      DEBUG(VM_DEBUG_FAIL, "directory not empty.\n");
      ret = EEXIST;
      goto destroy_out;

   case HGFS_STATUS_NO_SUCH_FILE_OR_DIR:
   case HGFS_STATUS_INVALID_NAME:
      DEBUG(VM_DEBUG_FAIL, "no such file or directory.\n");
      ret = ENOENT;
      goto destroy_out;
   case HGFS_STATUS_ACCESS_DENIED:
      DEBUG(VM_DEBUG_FAIL, "access denied.\n");

      /* XXX: Add retry behavior after removing r/o bit. */
      ret = EACCES;
      goto destroy_out;
   default:
      DEBUG(VM_DEBUG_FAIL, "default error.\n");
      ret = EPROTO;
      goto destroy_out;
   }

   DEBUG(VM_DEBUG_DONE, "done.\n");

destroy_out:
   HgfsKReq_ReleaseRequest(sip->reqs, req);
out:
   return ret;
}


/*
 * Local utility functions.
 */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsSubmitRequest --
 *
 *    Places a request on the queue for submission to guestd, then waits for
 *    the response.
 *
 *    Both submitting request and waiting for reply are in this function
 *    because the signaling of the request list's condition variable and
 *    waiting on the request's condition variable must be atomic.
 *
 * Results:
 *    Returns zero on success, and an appropriate error code on error.
 *    Note: EINTR is returned if cv_wait_sig() is interrupted.
 *
 * Side effects:
 *    The request list's condition variable is signaled.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsSubmitRequest(HgfsSuperInfo *sip,   // IN: Superinfo containing request list,
                                        //     condition variable, and mutex
                  HgfsKReqHandle req)         // IN: Request to submit
{
   int ret = 0;

   ASSERT(sip);
   ASSERT(req);

   /*
    * The process of submitting the request involves putting it on the request
    * list, waking up the backdoor req thread if it is waiting for a request,
    * then atomically waiting for the reply.
    */

   /*
    * Fail the request if a forcible unmount is in progress.
    */
   if (sip->vfsp->mnt_kern_flag & MNTK_UNMOUNTF) {
      HgfsKReq_ReleaseRequest(sip->reqs, req);
      return EIO;
   }

   /* Submit the request & wait for a result. */
   ret = HgfsKReq_SubmitRequest(req);

   if (ret == 0) {
      /* The reply should now be in HgfsKReq_GetPayload(req). */
      DEBUG(VM_DEBUG_SIG, "awoken because reply received.\n");
   } else {
      /* HgfsKReq_SubmitRequest was interrupted, so we'll abandon now. */
      HgfsKReq_ReleaseRequest(sip->reqs, req);
   }

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsValidateReply --
 *
 *    Validates a reply to ensure that its state is set appropriately and the
 *    reply is at least the minimum expected size and not greater than the
 *    maximum allowed packet size.
 *
 * Results:
 *    Returns zero on success, and a non-zero on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsValidateReply(HgfsKReqHandle req,    // IN: Request that contains reply data
                  uint32_t minSize)      // IN: Minimum size expected for the reply
{
   ASSERT(req);
   ASSERT(minSize <= HGFS_PACKET_MAX);  /* we want to know if this fails */

   switch (HgfsKReq_GetState(req)) {
   case HGFS_REQ_ERROR:
      DEBUG(VM_DEBUG_FAIL, "received reply with error.\n");
      return -1;

   case HGFS_REQ_COMPLETED:
      if ((HgfsKReq_GetPayloadSize(req) < minSize) || (HgfsKReq_GetPayloadSize(req) > HGFS_PACKET_MAX)) {
         DEBUG(VM_DEBUG_FAIL, "successfully "
               "completed reply is too small/big: !(%d < %" FMTSZ "d < %d).\n",
               minSize, HgfsKReq_GetPayloadSize(req), HGFS_PACKET_MAX);
         return -1;
      } else {
         return 0;
      }
   /*
    * If we get here then there is a programming error in this module:
    *  HGFS_REQ_UNUSED should be for requests in the free list
    *  HGFS_REQ_SUBMITTED should be for requests only that are awaiting
    *                     a response
    *  HGFS_REQ_ABANDONED should have returned an error to the client
    */
   default:
      NOT_REACHED();
      return -1;        /* avoid compiler warning */
   }
}


/*
 * XXX: These were taken directly from hgfs/solaris/vnode.c.  Should we
 * move them to hgfsUtil.c or similar?  (And Solaris took them from the Linux
 * implementation.)
 */


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsEscapeBuffer --
 *
 *    Escape any characters that are not legal in a linux filename,
 *    which is just the character "/". We also of course have to
 *    escape the escape character, which is "%".
 *
 *    sizeBufOut must account for the NUL terminator.
 *
 *    XXX: See the comments in staticEscape.c and staticEscapeW.c to understand
 *    why this interface sucks.
 *
 * Results:
 *    On success, the size (excluding the NUL terminator) of the
 *    escaped, NUL terminated buffer.
 *    On failure (bufOut not big enough to hold result), negative value.
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static int
HgfsEscapeBuffer(char const *bufIn, // IN:  Buffer with unescaped input
                 uint32 sizeIn,     // IN:  Size of input buffer (chars)
                 uint32 sizeBufOut, // IN:  Size of output buffer (bytes)
                 char *bufOut)      // OUT: Buffer for escaped output
{
   /*
    * This is just a wrapper around the more general escape
    * routine; we pass it the correct bitvector and the
    * buffer to escape. [bac]
    */
   EscBitVector bytesToEsc;

   ASSERT(bufIn);
   ASSERT(bufOut);

   /* Set up the bitvector for "/" and "%" */
   EscBitVector_Init(&bytesToEsc);
   EscBitVector_Set(&bytesToEsc, (unsigned char)'%');
   EscBitVector_Set(&bytesToEsc, (unsigned char)'/');

   return StaticEscape_Do('%',
                          &bytesToEsc,
                          bufIn,
                          sizeIn,
                          sizeBufOut,
                          bufOut);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HgfsUnescapeBuffer --
 *
 *    Unescape a buffer that was escaped using HgfsEscapeBuffer.
 *
 *    The unescaping is done in place in the input buffer, and
 *    can not fail.
 *
 * Results:
 *    The size (excluding the NUL terminator) of the unescaped, NUL
 *    terminated buffer.
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static int
HgfsUnescapeBuffer(char *bufIn,   // IN: Buffer to be unescaped
                   uint32 sizeIn) // IN: Size of input buffer
{
   /*
    * This is just a wrapper around the more general unescape
    * routine; we pass it the correct escape characer and the
    * buffer to unescape. [bac]
    */
   ASSERT(bufIn);
   return StaticEscape_Undo('%', bufIn, sizeIn);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsAttrToBSD --
 *
 *    Maps Hgfs attributes to FreeBSD attributes, filling the provided FreeBSD
 *    attribute structure appropriately.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
HgfsAttrToBSD(struct vnode *vp,             // IN:  The vnode for this file
              const HgfsAttr *hgfsAttr,     // IN:  Hgfs attributes to copy from
              struct vattr *vap)        // OUT: BSD attributes to fill
{
   ASSERT(vp);
   ASSERT(hgfsAttr);
   ASSERT(vap);

   DEBUG(VM_DEBUG_ENTRY, "%p -> %p", hgfsAttr, vap);

   /* Initialize all fields to zero. */
   VATTR_NULL(vap);

   /* Set the file type. */
   switch (hgfsAttr->type) {
   case HGFS_FILE_TYPE_REGULAR:
      vap->va_type = VREG;
      DEBUG(VM_DEBUG_ATTR, " Type: VREG\n");
      break;

   case HGFS_FILE_TYPE_DIRECTORY:
      vap->va_type = VDIR;
      DEBUG(VM_DEBUG_ATTR, " Type: VDIR\n");
      break;

   default:
      /*
       * There are only the above two filetypes.  If there is an error
       * elsewhere that provides another value, we set the FreeBSD type to
       * none and ASSERT in devel builds.
       */
      vap->va_type = VNON;
      DEBUG(VM_DEBUG_FAIL, "invalid HgfsFileType provided.\n");
      ASSERT(0);
   }

   /* We only have permissions for owners. */
   vap->va_mode = (hgfsAttr->permissions << HGFS_ATTR_MODE_SHIFT);
   DEBUG(VM_DEBUG_ATTR, " Owner's permissions: %o\n",
         vap->va_mode >> HGFS_ATTR_MODE_SHIFT);

   DEBUG(VM_DEBUG_ATTR, " Setting nlink\n");
   vap->va_nlink = 1;               /* fake */

   DEBUG(VM_DEBUG_ATTR, " Setting uid\n");
   vap->va_uid = 0;                 /* XXX root? */

   DEBUG(VM_DEBUG_ATTR, " Setting gid\n");
   vap->va_gid = 0;                 /* XXX root? */

   DEBUG(VM_DEBUG_ATTR, " Setting fsid\n");
   vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];

   /* Get the node id calculated for this file in HgfsVnodeGet() */
   vap->va_fileid = HGFS_VP_TO_NODEID(vp);
   DEBUG(VM_DEBUG_ATTR, "*HgfsAttrToBSD: fileName %s\n",
         HGFS_VP_TO_FILENAME(vp));
   DEBUG(VM_DEBUG_ATTR, " Node ID: %ld\n", vap->va_fileid);

   DEBUG(VM_DEBUG_ATTR, " Setting size\n");
   vap->va_size = vap->va_bytes = hgfsAttr->size;

   DEBUG(VM_DEBUG_ATTR, " Setting blksize\n");
   vap->va_blocksize = HGFS_BLOCKSIZE;

   DEBUG(VM_DEBUG_ATTR, " Setting atime\n");
   HGFS_SET_TIME(vap->va_atime, hgfsAttr->accessTime);

   DEBUG(VM_DEBUG_ATTR, " Setting mtime\n");
   HGFS_SET_TIME(vap->va_mtime, hgfsAttr->writeTime);

   DEBUG(VM_DEBUG_ATTR, " Setting ctime\n");
   /* Since Windows doesn't keep ctime, we may need to use mtime instead. */
   if (HGFS_SET_TIME(vap->va_ctime, hgfsAttr->attrChangeTime)) {
      vap->va_ctime = vap->va_mtime;
   }

   DEBUG(VM_DEBUG_ATTR, " Setting birthtime\n");
   HGFS_SET_TIME(vap->va_birthtime, hgfsAttr->creationTime);

   HgfsDebugPrintVattr(vap);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsSetattrCopy --
 *
 *    Sets the Hgfs attributes that need to be modified based on the provided
 *    FreeBSD attribute structure.
 *
 * Results:
 *    Returns TRUE if changes need to be made, FALSE otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static Bool
HgfsSetattrCopy(struct vattr *vap,          // IN:  Attributes to change to
                HgfsAttr *hgfsAttr,             // OUT: Hgfs attributes to fill in
                HgfsAttrChanges *update)        // OUT: Hgfs attribute changes to make
{
   Bool ret = FALSE;

   ASSERT(vap);
   ASSERT(hgfsAttr);
   ASSERT(update);

   memset(hgfsAttr, 0, sizeof *hgfsAttr);
   memset(update, 0, sizeof *update);

   /*
    * Hgfs supports changing these attributes:
    * o mode bits (permissions)
    * o size
    * o access/write times
    */

   if (vap->va_mode != (mode_t)VNOVAL) {
      DEBUG(VM_DEBUG_COMM, "updating permissions.\n");
      *update |= HGFS_ATTR_PERMISSIONS;
      hgfsAttr->permissions = (vap->va_mode & S_IRWXU) >> HGFS_ATTR_MODE_SHIFT;
      ret = TRUE;
   }

   if (vap->va_size != (u_quad_t)VNOVAL) {
      DEBUG(VM_DEBUG_COMM, "updating size.\n");
      *update |= HGFS_ATTR_SIZE;
      hgfsAttr->size = vap->va_size;
      ret = TRUE;
   }

   if (vap->va_atime.tv_sec != VNOVAL) {
      DEBUG(VM_DEBUG_COMM, "updating access time.\n");
      *update |= HGFS_ATTR_ACCESS_TIME;
      hgfsAttr->accessTime = HGFS_GET_TIME(vap->va_atime);
      ret = TRUE;
   }

   if (vap->va_mtime.tv_sec != VNOVAL) {
      DEBUG(VM_DEBUG_COMM, "updating write time.\n");
      *update |= HGFS_ATTR_WRITE_TIME;
      hgfsAttr->writeTime = HGFS_GET_TIME(vap->va_mtime);
      ret = TRUE;
   }

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsMakeFullName --
 *
 *    Concatenates the path and filename to construct the full path.  This
 *    handles the special cases of . and .. filenames so the Hgfs server
 *    doesn't return an error.
 *
 * Results:
 *    Returns the length of the full path on success, and a negative value on
 *    error.  The full pathname is placed in outBuf.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
HgfsMakeFullName(const char *path,      // IN:  Path of directory containing file
                 uint32_t pathLen,      // IN:  Length of path
                 const char *file,      // IN:  Name of file
                 size_t fileLen,          // IN:  Length of filename
                 char *outBuf,          // OUT: Location to write full path
                 ssize_t bufSize)       // IN:  Size of the out buffer
{

   ASSERT(path);
   ASSERT(file);
   ASSERT(outBuf);


   DEBUG(VM_DEBUG_INFO, "HgfsMakeFullName:\n"
         " path: \"%.*s\" (%d)\n"
         " file: \"%s\" (%zu)\n",
         pathLen, path, pathLen, file, fileLen);

   /*
    * Here there are three possibilities:
    *  o file is ".", in which case we just place path in outBuf
    *  o file is "..", in which case we strip the last component from path and
    *    put that in outBuf
    *  o for all other cases, we concatenate path, a path separator, file, and
    *    a NUL terminator and place it in outBuf
    */

   /* Make sure that the path and a NUL terminator will fit. */
   if (bufSize < pathLen + 1) {
      return HGFS_ERR_INVAL;
   }


   /* Copy path for this file into the caller's buffer. */
   memset(outBuf, 0, bufSize);
   memcpy(outBuf, path, pathLen);

   /* Handle three cases. */
   if (fileLen == 1 && strncmp(file, ".", 1) == 0) {
      /* NUL terminate and return provided length. */
      outBuf[pathLen] = '\0';
      return pathLen;

   } else if (fileLen == 2 && strncmp(file, "..", 2) == 0) {
      /*
       * Replace the last path separator with a NUL terminator, then return the
       * size of the buffer.
       */
      char *newEnd = rindex(outBuf, '/');
      if (!newEnd) {
         /*
          * We should never get here since we name the root vnode "/" in
          * HgfsMount().
          */
         return HGFS_ERR_INVAL;
      }

      *newEnd = '\0';
      return ((uintptr_t)newEnd - (uintptr_t)outBuf);

   } else {
      char *outPos;

      if (bufSize < pathLen + 1 + fileLen + 1) {
         return HGFS_ERR_INVAL;
      }

      outPos = outBuf + pathLen;
      /*
       * The CPName_ConvertTo function handles multiple path separators
       * at the beginning of the filename, so we skip the checks to limit
       * them to one.  This also enables clobbering newEnd above to work
       * properly on base shares (named "//sharename") that need to turn into
       * "/".
       */
      if (1) { // outBuf[pathLen - 1] != '/') {
         *(outPos++) = '/';
      }

      /* Now append the filename and NUL terminator. */
      memcpy(outPos, file, fileLen);
      outPos += fileLen;
      *(outPos++) = '\0';

      DEBUG(VM_DEBUG_INFO, "HgfsMakeFullName returning %s\n", outBuf);

      return (outPos - outBuf);
   }
}


/*
 * XXX
 * These were taken and slightly modified from hgfs/driver/solaris/vnode.c.
 * (Which, in turn, took them from hgfs/driver/linux/driver.c.) Should we
 * move them into a hgfs/driver/posix/driver.c?
 */


/*
 *----------------------------------------------------------------------
 *
 * HgfsGetOpenMode --
 *
 *    Based on the flags requested by the process making the open()
 *    syscall, determine which open mode (access type) to request from
 *    the server.
 *
 * Results:
 *    Returns the correct HgfsOpenMode enumeration to send to the
 *    server, or -1 on failure.
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

static int
HgfsGetOpenMode(uint32 flags) // IN: Open flags
{
   /*
    * Preprocessor wrapper kept for when this function is factored out
    * into a common file.
    */
#if defined(__FreeBSD__)
   /*
    * FreeBSD uses different values in the kernel.  These are defined in
    * <sys/fcntl.h>.
    */
   #undef O_RDONLY
   #undef O_WRONLY
   #undef O_RDWR

   #define O_RDONLY     FREAD
   #define O_WRONLY     FWRITE
   #define O_RDWR       (FREAD | FWRITE)
#endif

   uint32 mask = O_RDONLY|O_WRONLY|O_RDWR;
   int result = -1;

   DEBUG(VM_DEBUG_LOG, "entered\n");

   /*
    * Mask the flags to only look at the access type.
    */
   flags &= mask;

   /* Pick the correct HgfsOpenMode. */
   switch (flags) {

   case O_RDONLY:
      DEBUG(VM_DEBUG_COMM, "O_RDONLY\n");
      result = HGFS_OPEN_MODE_READ_ONLY;
      break;

   case O_WRONLY:
      DEBUG(VM_DEBUG_COMM, "O_WRONLY\n");
      result = HGFS_OPEN_MODE_WRITE_ONLY;
      break;

   case O_RDWR:
      DEBUG(VM_DEBUG_COMM, "O_RDWR\n");
      result = HGFS_OPEN_MODE_READ_WRITE;
      break;

   default:
      /* This should never happen. */
      NOT_REACHED();
      DEBUG(VM_DEBUG_LOG, "invalid open flags %o\n", flags);
      result = -1;
      break;
   }

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HgfsGetOpenFlags --
 *
 *    Based on the flags requested by the process making the open()
 *    syscall, determine which flags to send to the server to open the
 *    file.
 *
 * Results:
 *    Returns the correct HgfsOpenFlags enumeration to send to the
 *    server, or -1 on failure.
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

static int
HgfsGetOpenFlags(uint32 flags) // IN: Open flags
{
   uint32 mask = O_CREAT | O_TRUNC | O_EXCL;
   int result = -1;

   DEBUG(VM_DEBUG_INFO, "entered\n");

   /*
    * Mask the flags to only look at O_CREAT, O_EXCL, and O_TRUNC.
    */

   flags &= mask;

   /* O_EXCL has no meaning if O_CREAT is not set. */
   if (!(flags & O_CREAT)) {
      flags &= ~O_EXCL;
   }

   /* Pick the right HgfsOpenFlags. */
   switch (flags) {

   case 0:
      /* Regular open; fails if file nonexistant. */
      DEBUG(VM_DEBUG_COMM, "0\n");
      result = HGFS_OPEN;
      break;

   case O_CREAT:
      /* Create file; if it exists already just open it. */
      DEBUG(VM_DEBUG_COMM, "O_CREAT\n");
      result = HGFS_OPEN_CREATE;
      break;

   case O_TRUNC:
      /* Truncate existing file; fails if nonexistant. */
      DEBUG(VM_DEBUG_COMM, "O_TRUNC\n");
      result = HGFS_OPEN_EMPTY;
      break;

   case (O_CREAT | O_EXCL):
      /* Create file; fail if it exists already. */
      DEBUG(VM_DEBUG_COMM, "O_CREAT | O_EXCL\n");
      result = HGFS_OPEN_CREATE_SAFE;
      break;

   case (O_CREAT | O_TRUNC):
      /* Create file; if it exists already, truncate it. */
      DEBUG(VM_DEBUG_COMM, "O_CREAT | O_TRUNC\n");
      result = HGFS_OPEN_CREATE_EMPTY;
      break;

   default:
      /*
       * This can only happen if all three flags are set, which
       * conceptually makes no sense because O_EXCL and O_TRUNC are
       * mutually exclusive if O_CREAT is set.
       *
       * However, the open(2) man page doesn't say you can't set all
       * three flags, and certain apps (*cough* Nautilus *cough*) do
       * so. To be friendly to those apps, we just silenty drop the
       * O_TRUNC flag on the assumption that it's safer to honor
       * O_EXCL.
       */
      DEBUG(VM_DEBUG_INFO, "invalid open flags %o.  "
            "Ignoring the O_TRUNC flag.\n", flags);
      result = HGFS_OPEN_CREATE_SAFE;
      break;
   }

   return result;
}
