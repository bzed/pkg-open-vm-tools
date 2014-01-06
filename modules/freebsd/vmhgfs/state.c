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
 * state.c --
 *
 *	Vnode, HgfsOpenFile, and HgfsFile state manipulation routines.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include "hgfs_kernel.h"
#include "state.h"
#include "debug.h"
#include "sha1.h"


/*
 * Macros
 */

#define HGFS_FILE_HT_HEAD(ht, index)    (ht->hashTable[index]).next
#define HGFS_FILE_HT_BUCKET(ht, index)  (&ht->hashTable[index])

#define HGFS_IS_ROOT_FILE(sip, file)    (HGFS_VP_TO_FP(sip->rootVnode) == file)


/*
 * Local functions (prototypes)
 */

/* Internal version of a public function, to allow bypassing htp locking as needed */
static int HgfsVnodeGetInt(struct vnode **vpp,
                           struct HgfsSuperInfo *sip,
                           struct mount *vfsp,
                           struct vop_vector *vopp,
                           const char *fileName,
                           HgfsFileType fileType,
                           HgfsFileHashTable *htp,
                           Bool lockHtp);

/* Allocation/initialization/free of open file state */
static HgfsOpenFile *HgfsAllocOpenFile(const char *fileName, HgfsFileType fileType,
                                       HgfsFileHashTable *htp,
                                       Bool lockHtp);
static void HgfsFreeOpenFile(HgfsOpenFile *ofp, HgfsFileHashTable *htp);

/* Acquiring/releasing file state */
static HgfsFile *HgfsGetFile(const char *fileName, HgfsFileType fileType,
                             HgfsFileHashTable *htp,
                             Bool lockHtp);
static void HgfsReleaseFile(HgfsFile *fp, HgfsFileHashTable *htp);
static int HgfsInitFile(HgfsFile *fp, const char *fileName, HgfsFileType fileType);

/* Adding/finding/removing file state from hash table */
static void HgfsAddFile(HgfsFile *fp, HgfsFileHashTable *htp);
static void HgfsRemoveFile(HgfsFile *fp, HgfsFileHashTable *htp);
static HgfsFile *HgfsFindFile(const char *fileName, HgfsFileHashTable *htp);

/* Other utility functions */
static unsigned int HgfsFileNameHash(const char *fileName);
static void HgfsNodeIdHash(const char *fileName, uint32_t fileNameLength,
                           ino_t *outHash);


/*
 * Global functions
 */

/*
 *----------------------------------------------------------------------------
 *
 * HgfsVnodeGet --
 *
 *    Creates a vnode for the provided filename.
 *
 *    This will always allocate a vnode and HgfsOpenFile.  If a HgfsFile
 *    already exists for this filename then that is used, if a HgfsFile doesn't
 *    exist, one is created.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.  The new
 *    vnode is returned locked.
 *
 * Side effects:
 *    If the HgfsFile already exists, its reference count is incremented;
 *    otherwise a HgfsFile is created.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsVnodeGet(struct vnode **vpp,        // OUT: Filled with address of created vnode
             HgfsSuperInfo *sip,        // IN:  Superinfo
             struct mount *vfsp,        // IN:  Filesystem structure
             struct vop_vector *vopp,   // IN:  Vnode operations vector
             const char *fileName,      // IN:  Name of this file
             HgfsFileType fileType,     // IN:  Type of file
             HgfsFileHashTable *htp)    // IN:  File hash table
{
   return HgfsVnodeGetInt(vpp, sip, vfsp, vopp, fileName, fileType, htp, TRUE);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVnodePut --
 *
 *    Releases the provided vnode.
 *
 *    This will always free both the vnode and its associated HgfsOpenFile.
 *    The HgfsFile's reference count is decremented and, if 0, freed.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------------
 */

int
HgfsVnodePut(struct vnode *vp,          // IN: Vnode to release
             HgfsFileHashTable *htp)    // IN: Hash table pointer
{
   HgfsOpenFile *ofp;

   ASSERT(vp);
   ASSERT(htp);

   /* Get our private open-file state. */
   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {          // XXX Maybe ASSERT() this?
      return HGFS_ERR;
   }

   /*
    * We need to free the open file structure.  This takes care of releasing
    * our reference on the underlying file structure (and freeing it if
    * necessary).
    */
   HgfsFreeOpenFile(ofp, htp);

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsVnodeDup --
 *
 *    Duplicates the vnode and HgfsOpenFile (per-open state) of a file and
 *    increments the reference count of the underlying HgfsFile.  This function
 *    just calls HgfsVnodeGet with the right arguments.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.  On success
 *    the address of the duplicated vnode is written to newVpp.
 *
 * Side effects:
 *    The HgfsFile for origVp will have an additional reference.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsVnodeDup(struct vnode **newVpp,             // OUT: Given address of new vnode
             struct vnode *origVp,              // IN:  Vnode to duplicate
             struct HgfsSuperInfo *sip,         // IN:  Superinfo pointer
             HgfsFileHashTable *htp)            // IN:  File hash table
{
   ASSERT(newVpp);
   ASSERT(origVp);
   ASSERT(sip);
   ASSERT(htp);

   DEBUG(VM_DEBUG_ALWAYS, "HgfsVnodeDup: duping %s\n", HGFS_VP_TO_FILENAME(origVp));

   return HgfsVnodeGet(newVpp, sip, origVp->v_mount, origVp->v_op,
                       HGFS_VP_TO_FILENAME(origVp), HGFS_VP_TO_HGFSFILETYPE(origVp),
                       &sip->fileHashTable);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsFileNameToVnode --
 *
 *    Allocates new per-open-file state if a HgfsFile for fileName exists in
 *    the provided file hash table.
 *
 * Results:
 *    Returns 0 on success or a non-zero error code on failure.  On success,
 *    vpp is filled with the address of the new per-open state.
 *
 * Side effects:
 *    The reference count of the HgfsFile for fileName is incremented if it
 *    exists.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsFileNameToVnode(const char *fileName,       // IN: Name of the file to look up
                    struct vnode **vpp,         // OUT: Address of created vnode
                    struct HgfsSuperInfo *sip,  // IN: Superinfo pointer
                    struct mount *vfsp,         // IN: Filesystem structure
                    HgfsFileHashTable *htp)     // IN: File hash table
{
   HgfsFile *fp;
   int retval;

   ASSERT(vpp);
   ASSERT(sip);
   ASSERT(vfsp);
   ASSERT(fileName);
   ASSERT(htp);

   DEBUG(VM_DEBUG_ALWAYS, "HgfsFileNameToVnode: looking for %s\n", fileName);

   mtx_lock(&htp->mutex);

   fp = HgfsFindFile(fileName, htp);
   if (!fp) {
      mtx_unlock(&htp->mutex);
      return HGFS_ERR;
   }

   /* Guaranteed by HgfsFindFile(). */
   ASSERT(strcmp(fileName, fp->fileName) == 0);

   retval = HgfsVnodeGetInt(vpp, sip, vfsp, sip->rootVnode->v_op, fileName, fp->fileType,
                            htp, FALSE);
   mtx_unlock(&htp->mutex);

   DEBUG(VM_DEBUG_ALWAYS, "HgfsFileNameToVnode: found %s\n", fileName);

   return retval;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsNodeIdGet --
 *
 *    Gets the node id for the provided file.  This will only calculate the
 *    node id again if a per-file state structure doesn't yet exist for this
 *    file.  (This situation exists on a readdir since dentries are filled in
 *    rather than creating vnodes.)
 *
 *    In Solaris, node ids are provided in vnodes and inode numbers are
 *    provided in dentries.  For applications to work correctly, we must make
 *    sure that the inode number of a file's dentry and the node id in a file's
 *    vnode match one another.  This poses a problem since vnodes typically do
 *    not exist when dentries need to be created, and once a dentry is created
 *    we have no reference to it since it is copied to the user and freed from
 *    kernel space.  An example of a program that breaks when these values
 *    don't match is /usr/bin/pwd.  This program first acquires the node id of
 *    "." from its vnode, then traverses backwards to ".." and looks for the
 *    dentry in that directory with the inode number matching the node id.
 *    (This is how it obtains the name of the directory it was just in.)
 *    /usr/bin/pwd repeats this until it reaches the root directory, at which
 *    point it concatenates the filenames it acquired along the way and
 *    displays them to the user.  When inode numbers don't match the node id,
 *    /usr/bin/pwd displays an error saying it cannot determine the directory.
 *
 *    The Hgfs protocol does not provide us with unique identifiers for files
 *    since it must support filesystems that do not have the concept of inode
 *    numbers.  Therefore, we must maintain a mapping from filename to node id/
 *    inode numbers.  This is done in a stateless manner by calculating the
 *    SHA-1 hash of the filename.  All points in the Hgfs code that need a node
 *    id/inode number obtain it by either calling this function or directly
 *    referencing the saved node id value in the vnode, if one is available.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
HgfsNodeIdGet(HgfsFileHashTable *htp,   // IN:  File hash table
              const char *fileName,     // IN:  Filename to get node id for
              uint32_t fileNameLength,  // IN:  Length of filename
              ino_t *outNodeId)         // OUT: Destination for nodeid
{
   HgfsFile *fp;

   ASSERT(htp);
   ASSERT(fileName);
   ASSERT(outNodeId);

   mtx_lock(&htp->mutex);

   fp = HgfsFindFile(fileName, htp);
   if (fp) {
      *outNodeId = fp->nodeId;
   } else {
      HgfsNodeIdHash(fileName, fileNameLength, outNodeId);
   }

   mtx_unlock(&htp->mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsInitFileHashTable --
 *
 *    Initializes the hash table used to track per-file state.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsInitFileHashTable(HgfsFileHashTable *htp)   // IN: Hash table to initialize
{
   int i;

   ASSERT(htp);

   mtx_init(&htp->mutex, "HgfsHashChain", NULL, MTX_DEF);

   for (i = 0; i < ARRAYSIZE(htp->hashTable); i++) {
      DblLnkLst_Init(&htp->hashTable[i]);
   }

   return 0;
}


void
HgfsDestroyFileHashTable(HgfsFileHashTable *htp)
{
   ASSERT(htp);
   mtx_destroy(&htp->mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsFileHashTableIsEmpty --
 *
 *    Determines whether the hash table is in an acceptable state to unmount
 *    the file system.
 *
 *    Note that this is not strictly empty: if the only file in the table is
 *    the root of the filesystem and its reference count is 1, this is
 *    considered empty since this is part of the operation of unmounting the
 *    filesystem.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

Bool
HgfsFileHashTableIsEmpty(HgfsSuperInfo *sip,            // IN: Superinfo
                         HgfsFileHashTable *htp)        // IN: File hash table
{
   int i;

   ASSERT(sip);
   ASSERT(htp);

   mtx_lock(&htp->mutex);

   /* Traverse each bucket. */
   for (i = 0; i < ARRAYSIZE(htp->hashTable); i++) {
      DblLnkLst_Links *currNode = HGFS_FILE_HT_HEAD(htp, i);

      /* Visit each file in this bucket */
      while (currNode != HGFS_FILE_HT_BUCKET(htp, i)) {
         HgfsFile *currFile = DblLnkLst_Container(currNode, HgfsFile, listNode);

         /*
          * Here we special case the root of our filesystem.  In a correct
          * unmount, the root vnode of the filesystem will have an entry in the
          * hash table and will have a reference count of 1.  We check if the
          * current entry is the root file, and if so, make sure its vnode's
          * reference count is not > 1.  Note that we are not mapping from file
          * to vnode here (which is not possible), we are using the root vnode
          * stored in the superinfo structure.  This is the only vnode that
          * should have multiple references associated with it because whenever
          * someone calls HgfsRoot(), we return that vnode.
          */
         if (HGFS_IS_ROOT_FILE(sip, currFile)) {
            VI_LOCK(sip->rootVnode);
            if (sip->rootVnode->v_usecount <= 1) {
               VI_UNLOCK(sip->rootVnode);

               /* This file is okay; skip to the next one. */
               currNode = currNode->next;
               continue;
            }

            DEBUG(VM_DEBUG_FAIL, "HgfsFileHashTableIsEmpty: %s has count of %d.\n",
                  currFile->fileName, sip->rootVnode->v_usecount);

            VI_UNLOCK(sip->rootVnode);
            /* Fall through to failure case */
         }

         /* Fail if a file is found. */
         mtx_unlock(&htp->mutex);
         DEBUG(VM_DEBUG_FAIL, "HgfsFileHashTableIsEmpty: %s "
               "still in use (file count=%d).\n",
               currFile->fileName, currFile->refCount);
         return FALSE;
      }
   }

   mtx_unlock(&htp->mutex);

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsHandleIsSet --
 *
 *    Determines whether handle of the vnode's open file is currently set.
 *
 * Results:
 *    Returns TRUE if the handle is set, FALSE if the handle is not set.
 *    HGFS_ERR is returned on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

Bool
HgfsHandleIsSet(struct vnode *vp)       // IN: Vnode to check handle of
{
   HgfsOpenFile *ofp;
   Bool isSet;

   ASSERT(vp);

   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {
      return HGFS_ERR;
   }

   mtx_lock(&ofp->handleMutex);
   isSet = ofp->handleIsSet;
   mtx_unlock(&ofp->handleMutex);

   return isSet;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsSetOpenFileHandle --
 *
 *    Sets the open file handle for the provided vnode.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    The handle may not be set again until it is cleared.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsSetOpenFileHandle(struct vnode *vp,         // IN: Vnode to set handle for
                      HgfsHandle handle)        // IN: Value of handle
{
   HgfsOpenFile *ofp;

   ASSERT(vp);

   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {
      return HGFS_ERR;
   }

   mtx_lock(&ofp->handleMutex);

   if (ofp->handleIsSet) {
      DEBUG(VM_DEBUG_FAIL, "**HgfsSetOpenFileHandle: handle for %s already set to %d; "
            "cannot set to %d\n", HGFS_VP_TO_FILENAME(vp), ofp->handle, handle);
      mtx_unlock(&ofp->handleMutex);
      return HGFS_ERR;
   }

   ofp->handle = handle;
   ofp->handleIsSet = TRUE;

   DEBUG(VM_DEBUG_STATE, "HgfsSetOpenFileHandle: set handle for %s to %d\n",
         HGFS_VP_TO_FILENAME(vp), ofp->handle);

   mtx_unlock(&ofp->handleMutex);

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsGetOpenFileHandle --
 *
 *    Gets the open file handle for the provided vnode.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.  On success,
 *    the value of the vnode's handle is placed in outHandle.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsGetOpenFileHandle(struct vnode *vp,         // IN:  Vnode to get handle for
                      HgfsHandle *outHandle)    // OUT: Filled with value of handle
{
   HgfsOpenFile *ofp;

   ASSERT(vp);
   ASSERT(outHandle);

   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {
      return HGFS_ERR;
   }

   mtx_lock(&ofp->handleMutex);

   if (!ofp->handleIsSet) {
      DEBUG(VM_DEBUG_FAIL, "**HgfsGetOpenFileHandle: handle for %s is not set.\n",
            HGFS_VP_TO_FILENAME(vp));
      mtx_unlock(&ofp->handleMutex);
      return HGFS_ERR;
   }

   *outHandle = ofp->handle;

   mtx_unlock(&ofp->handleMutex);

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsClearOpenFileHandle --
 *
 *    Clears the open file handle for the provided vnode.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    The handle may be set.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsClearOpenFileHandle(struct vnode *vp)       // IN: Vnode to clear handle for
{
   HgfsOpenFile *ofp;

   ASSERT(vp);

   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {
      return HGFS_ERR;
   }

   mtx_lock(&ofp->handleMutex);

   ofp->handle = 0;
   ofp->handleIsSet = FALSE;

   DEBUG(VM_DEBUG_STATE, "HgfsClearOpenFileHandle: cleared %s's handle\n",
         HGFS_VP_TO_FILENAME(vp));

   mtx_unlock(&ofp->handleMutex);

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsSetOpenFileMode --
 *
 *    Sets the mode of the open file for the provided vnode.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    The mode may not be set again until cleared.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsSetOpenFileMode(struct vnode *vp,   // IN: Vnode to set mode for
                    HgfsMode mode)      // IN: Mode to set to
{
   HgfsOpenFile *ofp;

   ASSERT(vp);

   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {
      return HGFS_ERR;
   }

   mtx_lock(&ofp->modeMutex);

   if (ofp->modeIsSet) {
      DEBUG(VM_DEBUG_FAIL, "**HgfsSetOpenFileMode: mode for %s already set to %d; "
            "cannot set to %d\n", HGFS_VP_TO_FILENAME(vp), ofp->mode, mode);
      mtx_unlock(&ofp->modeMutex);
      return HGFS_ERR;
   }

   ofp->mode = mode;
   ofp->modeIsSet = TRUE;

   DEBUG(VM_DEBUG_STATE, "HgfsSetOpenFileMode: set mode for %s to %d\n",
         HGFS_VP_TO_FILENAME(vp), ofp->mode);

   mtx_unlock(&ofp->modeMutex);

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsGetOpenFileMode --
 *
 *    Gets the mode of the open file for the provided vnode.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsGetOpenFileMode(struct vnode *vp,   // IN:  Vnode to get mode for
                    HgfsMode *outMode)  // OUT: Filled with mode
{
   HgfsOpenFile *ofp;

   ASSERT(vp);
   ASSERT(outMode);

   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {
      return HGFS_ERR;
   }

   mtx_lock(&ofp->modeMutex);

   if (!ofp->modeIsSet) {
      mtx_unlock(&ofp->modeMutex);
      return HGFS_ERR;
   }

   *outMode = ofp->mode;

   mtx_unlock(&ofp->modeMutex);

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsClearOpenFileMode --
 *
 *    Clears the mode of the open file for the provided vnode.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    The mode may be set again.
 *
 *----------------------------------------------------------------------------
 */

int
HgfsClearOpenFileMode(struct vnode *vp) // IN: Vnode to clear mode for
{
   HgfsOpenFile *ofp;

   ASSERT(vp);

   ofp = HGFS_VP_TO_OFP(vp);
   if (!ofp) {
      return HGFS_ERR;
   }

   mtx_lock(&ofp->modeMutex);

   ofp->mode = 0;
   ofp->modeIsSet = FALSE;

   DEBUG(VM_DEBUG_STATE, "HgfsClearOpenFileMode: cleared %s's mode\n",
         HGFS_VP_TO_FILENAME(vp));

   mtx_unlock(&ofp->modeMutex);

   return 0;
}


/*
 * Local functions (definitions)
 */

/* Internal versions of public functions to allow bypassing htp locking */

/*
 *----------------------------------------------------------------------------
 *
 * HgfsVnodeGetInt --
 *
 *    Creates a vnode for the provided filename.
 *
 *    This will always allocate a vnode and HgfsOpenFile.  If a HgfsFile
 *    already exists for this filename then that is used, if a HgfsFile doesn't
 *    exist, one is created.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.  The new
 *    vnode is returned locked.
 *
 * Side effects:
 *    If the HgfsFile already exists, its reference count is incremented;
 *    otherwise a HgfsFile is created.
 *
 *----------------------------------------------------------------------------
 */
static int
HgfsVnodeGetInt(struct vnode **vpp,        // OUT: Filled with address of created vnode
                HgfsSuperInfo *sip,        // IN:  Superinfo
                struct mount *vfsp,        // IN:  Filesystem structure
                struct vop_vector *vopp,   // IN:  Vnode operations vector
                const char *fileName,      // IN:  Name of this file
                HgfsFileType fileType,     // IN:  Type of file
                HgfsFileHashTable *htp,    // IN:  File hash table
                Bool lockHtp)              // IN:  Whether to lock the file hash table
{
   struct vnode *vp;
   int ret = 0;

   ASSERT(vpp);
   ASSERT(sip);
   ASSERT(vfsp);
   ASSERT(fileName);
   ASSERT(htp);

   /*
    * Here we need to construct the vnode for the kernel as well as our
    * internal file system state.  Our internal state consists of
    * a HgfsOpenFile and a HgfsFile.  The HgfsOpenFile is state kept per-open
    * file; the HgfsFile state is kept per-file.  We have a one-to-one mapping
    * between vnodes and HgfsOpenFiles, and a many-to-one mapping from each of
    * those to a HgfsFile.
    *
    * Note that it appears the vnode is intended to be used as a per-file
    * structure, but we are using it as a per-open-file. The sole exception
    * for this is the root vnode because it is returned by HgfsRoot().  This
    * also means that reference counts for all vnodes except the root should
    * be one; the reference count in our HgfsFile takes on the role of the
    * vnode reference count.
    */
   if ((ret = getnewvnode(HGFS_FS_NAME, vfsp, vopp, &vp)) != 0) {
      return ret;
   }

   /*
    * Return a locked vnode to the caller.
    */
   lockmgr(vp->v_vnlock, LK_EXCLUSIVE, NULL, curthread);

   /*
    * Now we'll initialize the vnode.  We need to set the file type, vnode
    * operations, flags, filesystem pointer, reference count, and device.
    * After that we'll create our private structures and hang them from the
    * vnode's v_data pointer.
    */
   switch (fileType) {
   case HGFS_FILE_TYPE_REGULAR:
      vp->v_type = VREG;
      break;

   case HGFS_FILE_TYPE_DIRECTORY:
      vp->v_type = VDIR;
      break;

   default:
      /* Hgfs only supports directories and regular files */
      goto vnode_error;
   }

   /*
    * We now allocate our private open file structure.  This will correctly
    * initialize the per-open-file state, as well as locate (or create if
    * necessary) the per-file state.
    */
   vp->v_data = (void *)HgfsAllocOpenFile(fileName, fileType, htp, lockHtp);
   if (vp->v_data == NULL) {
      ret = ENOMEM;
      goto vnode_error;
   }

   /* Fill in the provided address with the new vnode. */
   *vpp = vp;

   /* Return success */
   return 0;

   /* Cleanup points for errors. */
vnode_error:
   vrele(vp);
   return ret;
}

/* Allocation/initialization/free of open file state */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsAllocOpenFile --
 *
 *    Allocates and initializes an open file structure.  Also finds or, if
 *    necessary, creates the underlying HgfsFile per-file state.
 *
 * Results:
 *    Returns a pointer to the open file on success, NULL on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static HgfsOpenFile *
HgfsAllocOpenFile(const char *fileName,         // IN: Name of file
                  HgfsFileType fileType,        // IN: Type of file
                  HgfsFileHashTable *htp,       // IN: Hash table
                  Bool lockHtp)                 // IN: Whether to lock the hash table
{
   HgfsOpenFile *ofp;

   ASSERT(fileName);
   ASSERT(htp);

   /*
    * We allocate and initialize our open-file state.
    */
   ofp = (HgfsOpenFile *)malloc(sizeof *ofp, M_HGFS, M_ZERO|M_WAITOK);

   /* Manually set these since the public functions need the lock. */
   ofp->handle = 0;
   ofp->handleIsSet = FALSE;

   ofp->mode = 0;
   ofp->modeIsSet = FALSE;

   mtx_init(&ofp->handleMutex, "hgfs_ofp", NULL, MTX_DEF);
   mtx_init(&ofp->modeMutex, "hgfs_ofp_mode", NULL, MTX_DEF);

   /*
    * Now we get a reference to the underlying per-file state.
    */
   ofp->hgfsFile = HgfsGetFile(fileName, fileType, htp, lockHtp);
   if (!ofp->hgfsFile) {
      free(ofp, M_HGFS);
      return NULL;
   }

   return ofp;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsFreeOpenFile --
 *
 *    Frees the provided open file.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
HgfsFreeOpenFile(HgfsOpenFile *ofp,             // IN: Open file to free
                 HgfsFileHashTable *htp)        // IN: File hash table
{
   ASSERT(ofp);
   ASSERT(htp);

   /*
    * First we release our reference to the underlying per-file state.
    */
   HgfsReleaseFile(ofp->hgfsFile, htp);

   /*
    * Then we destroy anything initialized and free the open file.
    */
   mtx_destroy(&ofp->handleMutex);
   mtx_destroy(&ofp->modeMutex);

   free(ofp, M_HGFS);
}


/* Acquiring/releasing file state */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsGetFile --
 *
 *    Gets the file for the provided filename.
 *
 *    If no file structure exists for this filename, one is created and added
 *    to the hash table.
 *
 * Results:
 *    Returns a pointer to the file on success, NULL on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static HgfsFile *
HgfsGetFile(const char *fileName,       // IN: Filename to get file for
            HgfsFileType fileType,      // IN: Type of file
            HgfsFileHashTable *htp,     // IN: Hash table to look in
            Bool lockHtp)               // IN: Whether to lock the hash table
{
   HgfsFile *fp;
   int err;

   ASSERT(fileName);
   ASSERT(htp);

   /*
    * We try to find the file in the hash table.  If it exists we increment its
    * reference count and return it.
    */
   if (lockHtp) {
      mtx_lock(&htp->mutex);
   }

   fp = HgfsFindFile(fileName, htp);
   if (fp) {
      /* Signify our reference to this file. */
      mtx_lock(&fp->mutex);
      fp->refCount++;
      mtx_unlock(&fp->mutex);

      if (lockHtp) {
         mtx_unlock(&htp->mutex);
      }

      return fp;
   }

   DEBUG(VM_DEBUG_ALWAYS, "HgfsGetFile: allocated HgfsFile for %s.\n", fileName);

   /*
    * If it doesn't exist we create one, initialize it, and add it to the hash
    * table.  (M_NOWAIT set because sleeping while holding a lock is
    * forbidden.)
    */
   fp = (HgfsFile *)malloc(sizeof *fp, M_HGFS, M_ZERO|M_NOWAIT);
   if (!fp) {
      /* fp is NULL already */
      goto out;
   }

   err = HgfsInitFile(fp, fileName, fileType);
   if (err) {
      free(fp, M_HGFS);
      fp = NULL;
      goto out;
   }

   /*
    * This is guaranteed to not add a duplicate since we checked above and have
    * held the lock until now.
    */
   HgfsAddFile(fp, htp);

out:
   if (lockHtp) {
      mtx_unlock(&htp->mutex);
   }
   DEBUG(VM_DEBUG_DONE, "HgfsGetFile: done\n");
   return fp;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsReleaseFile --
 *
 *    Releases a reference to the provided file.  If the reference count of
 *    this file becomes zero, the file structure is removed from the hash table
 *    and freed.
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
HgfsReleaseFile(HgfsFile *fp,           // IN: File to release
                HgfsFileHashTable *htp) // IN: Hash table to look in/remove from
{
   ASSERT(fp);
   ASSERT(htp);

   /*
    * Decrement this file's reference count.  If it becomes zero, then we
    * remove it from the hash table and free it.
    */
   mtx_lock(&fp->mutex);

   if ( !(--fp->refCount) ) {
      mtx_unlock(&fp->mutex);

      /* Remove file from hash table, then clean up. */
      HgfsRemoveFile(fp, htp);

      DEBUG(VM_DEBUG_ALWAYS, "HgfsReleaseFile: freeing HgfsFile for %s.\n",
            fp->fileName);

      sx_destroy(&fp->rwlock);
      mtx_destroy(&fp->mutex);
      free(fp, M_HGFS);
      return;
   }

   DEBUG(VM_DEBUG_ALWAYS, "HgfsReleaseFile: %s has %d references.\n",
         fp->fileName, fp->refCount);

   mtx_unlock(&fp->mutex);
}


/* Allocation/initialization/free of file state */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsInitFile --
 *
 *    Initializes a file structure.
 *
 *    This sets the filename of the file and initializes other structure
 *    elements.
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
HgfsInitFile(HgfsFile *fp,              // IN: File to initialize
             const char *fileName,      // IN: Name of file
             HgfsFileType fileType)     // IN: Type of file
{
   int len;

   ASSERT(fp);
   ASSERT(fileName);

   /* Make sure the filename will fit. */
   len = strlen(fileName);
   if (len > sizeof fp->fileName - 1) {
      return HGFS_ERR;
   }

   fp->fileNameLength = len;
   memcpy(fp->fileName, fileName, len + 1);
   fp->fileName[fp->fileNameLength] = '\0';

   /*
    * We save the file type so we can recreate a vnode for the HgfsFile without
    * sending a request to the Hgfs Server.
    */
   fp->fileType = fileType;

   /* Initialize the links to place this file in our hash table. */
   DblLnkLst_Init(&fp->listNode);

   /*
    * Fill in the node id.  This serves as the inode number in directory
    * entries and the node id in vnode attributes.
    */
   HgfsNodeIdHash(fp->fileName, fp->fileNameLength, &fp->nodeId);

   /*
    * The reader/write lock is for the rwlock/rwunlock vnode entry points and
    * the mutex is to protect the reference count on this structure.
    */
   sx_init(&fp->rwlock, "hgfs_file_sx");
   mtx_init(&fp->mutex, "hgfs_file_mtx", NULL, MTX_DEF);

   /* The caller is the single reference. */
   fp->refCount = 1;

   return 0;
}


/* Adding/finding/removing file state from hash table */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsAddFile --
 *
 *    Adds the file to the hash table.
 *
 *    This function must be called with the hash table lock held.  This is done
 *    so adding the file in the hash table can be made with any other
 *    operations (such as previously finding out that this file wasn't in the
 *    hash table).
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
HgfsAddFile(HgfsFile *fp,               // IN: File to add
            HgfsFileHashTable *htp)     // IN: Hash table to add to
{
   unsigned int index;

   ASSERT(fp);
   ASSERT(htp);

   index = HgfsFileNameHash(fp->fileName);

   /* Add this file to the end of the bucket's list */
   DblLnkLst_LinkLast(HGFS_FILE_HT_HEAD(htp, index), &fp->listNode);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsRemoveFile --
 *
 *    Removes file from the hash table.
 *
 *    Note that unlike the other two hash functions, this one performs its own
 *    locking since the removal doesn't need to be atomic with other
 *    operations.  (This could change in the future if the functions that use
 *    this one are reorganized.)
 *
 * Results:
 *    Returns 0 on success and a non-zero error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
HgfsRemoveFile(HgfsFile *fp,            // IN: File to remove
               HgfsFileHashTable *htp)  // IN: Hash table to remove from
{
   ASSERT(fp);
   ASSERT(htp);

   mtx_lock(&htp->mutex);

   /* Take this file off its list */
   DblLnkLst_Unlink1(&fp->listNode);

   mtx_unlock(&htp->mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsFindFile --
 *
 *    Looks for a filename in the hash table.
 *
 *    This function must be called with the hash table lock held.  This is done
 *    so finding the file in the hash table and using it (after this function
 *    returns) can be atomic.
 *
 * Results:
 *    Returns a pointer to the file if found, NULL otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static HgfsFile *
HgfsFindFile(const char *fileName,      // IN: Filename to look for
             HgfsFileHashTable *htp)    // IN: Hash table to look in
{
   HgfsFile *found = NULL;
   DblLnkLst_Links *currNode;
   unsigned int index;

   ASSERT(fileName);
   ASSERT(htp);

   /* Determine which bucket. */
   index = HgfsFileNameHash(fileName);

   /* Traverse the bucket's list. */
   for (currNode = HGFS_FILE_HT_HEAD(htp, index);
        currNode != HGFS_FILE_HT_BUCKET(htp, index);
        currNode = currNode->next) {
      HgfsFile *curr;
      curr = DblLnkLst_Container(currNode, HgfsFile, listNode);

      if (strcmp(curr->fileName, fileName) == 0) {
         /* We found the file we want. */
         found = curr;
         break;
      }
   }

   /* Return file if found. */
   return found;
}


/* Other utility functions */


/*
 *----------------------------------------------------------------------------
 *
 * HgfsFileNameHash --
 *
 *    Hashes the filename to get an index into the hash table.  This is known
 *    as the PJW string hash function and it was taken from "Mastering
 *    Algorithms in C".
 *
 * Results:
 *    Returns an index between 0 and HGFS_HT_NR_BUCKETS.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static unsigned int
HgfsFileNameHash(const char *fileName)  // IN: Filename to hash
{
   unsigned int val = 0;

   ASSERT(fileName);

   while (*fileName != '\0') {
      unsigned int tmp;

      val = (val << 4) + (*fileName);
      if ((tmp = (val & 0xf0000000))) {
        val = val ^ (tmp >> 24);
        val = val ^ tmp;
      }

      fileName++;
   }

   return val % HGFS_HT_NR_BUCKETS;
}


/*
 *----------------------------------------------------------------------------
 *
 * HgfsNodeIdHash --
 *
 *    Hashes the provided filename to generate a node id.
 *
 * Results:
 *    None.  The value of the hash is filled into outHash.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
HgfsNodeIdHash(const char *fileName,    // IN:  Filename to hash
               uint32_t fileNameLength, // IN:  Length of the filename
               ino_t *outHash)          // OUT: Location to write hash to
{
   SHA1_CTX hashContext;
   unsigned char digest[SHA1_HASH_LEN];
   int i;

   ASSERT(fileName);
   ASSERT(outHash);

   /* Make sure we start at a consistent state. */
   memset(&hashContext, 0, sizeof hashContext);
   memset(digest, 0, sizeof digest);
   memset(outHash, 0, sizeof *outHash);

   /* Generate a SHA1 hash of the filename */
   SHA1Init(&hashContext);
   SHA1Update(&hashContext, (unsigned const char *)fileName, fileNameLength);
   SHA1Final(digest, &hashContext);

   /*
    * Fold the digest into the allowable size of our hash.
    *
    * For each group of bytes the same size as our output hash, xor the
    * contents of the digest together.  If there are less than that many bytes
    * left in the digest, xor each byte that's left.
    */
   for(i = 0; i < sizeof digest; i += sizeof *outHash) {
      int bytesLeft = sizeof digest - i;

      /* Do a byte-by-byte xor if there aren't enough bytes left in the digest */
      if (bytesLeft < sizeof *outHash) {
         int j;

         for (j = 0; j < bytesLeft; j++) {
            uint8 *outByte = (uint8 *)outHash + j;
            uint8 *inByte = (uint8 *)((uint32_t *)(digest + i)) + j;
            *outByte ^= *inByte;
         }
         break;
      }

      /* Block xor */
      *outHash ^= *((uint32_t *)(digest + i));
   }

   /*
    * Clear the most significant byte so that user space apps depending on
    * a node id/inode number that's only 32 bits won't break.  (For example,
    * gedit's call to stat(2) returns EOVERFLOW if we don't do this.)
    */
#if 0
#  ifndef HGFS_BREAK_32BIT_USER_APPS
   *((uint32_t *)outHash) ^= *((uint32_t *)outHash + 1);
   *((uint32_t *)outHash + 1) = 0;
#  endif
#endif

   DEBUG(VM_DEBUG_INFO, "Hash of: %s (%d) is %u\n", fileName, fileNameLength, *outHash);

   return;
}
