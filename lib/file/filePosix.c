/* ************************************************************************
 * Copyright 2006 VMware, Inc.  All rights reserved. 
 * ************************************************************************
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
 * filePosix.c --
 *
 *      Interface to host-specific file functions for Posix hosts.
 */

#include <sys/types.h> /* Needed before sys/vfs.h with glibc 2.0 --hpreg */

#if !__FreeBSD__
# if !__APPLE__
#  include <sys/vfs.h>
# endif
# include <limits.h>
# include <stdio.h>      /* Needed before sys/mnttab.h in Solaris */
# ifdef sun
#  include <sys/mnttab.h>
# elif __APPLE__
#  include <Carbon/Carbon.h> 
#  include <DiskArbitration/DiskArbitration.h>
#  include <sys/param.h>
#  include <sys/mount.h>
#  include <paths.h>
# else
#  include <mntent.h>
# endif
#include <signal.h>
#endif
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#ifdef __linux__
#   include <pwd.h>
#endif

#include "vmware.h"
#include "file.h"
#include "fileInt.h"
#include "msg.h"
#include "util.h"
#include "str.h"
#include "timeutil.h"
#include "dynbuf.h"
#include "localconfig.h"

#if !defined(__FreeBSD__) && !defined(sun)
static char *FilePosixLookupFSDeviceName(const char *filename);

# ifdef VMX86_SERVER
#define VMFS2CONST 456
#define VMFS3CONST 256
#include "hostType.h"
/* Needed for VMFS implementation of File_GetFreeSpace() */
#  include <sys/ioctl.h>
# endif
#endif

#ifdef VMX86_SERVER
#include "fs_user.h"
#endif


/*
 * Local functions
 */

static Bool FileIsGroupsMember(gid_t gid);


#if __APPLE__

struct FileVolumeItem {
   char                  *name;
   char                  *device; /* Typically "disk1s0". */
   FSVolumeRefNum         ref;
   struct FileVolumeItem *next;
};

struct FileMacOsUnmountState {
   Bool finished;
   Bool unmounted;
   Bool eject;
};


/*
 *----------------------------------------------------------------------
 *
 * FileMacosDADiskUnmountCb --
 *
 *      Callback called when a disk is unmounted.
 *
 * Results:
 *      Context is a FileMacOsUnmountState. We set 'unmounted' to
 *      TRUE if the unmount was successful. Logs errors on failure.
 *
 * Side effects:
 *      May terminate the current CFRunLoop.
 *
 *----------------------------------------------------------------------
 */

static void
FileMacosDADiskUnmountCb(DADiskRef disk,            // IN
                         DADissenterRef dissenter,  // IN
                         void *context)             // IN/OUT
{
   struct FileMacOsUnmountState *s = (struct FileMacOsUnmountState *)context;

   ASSERT(s);

   if (dissenter) {
      Log(LGPFX" DA reported failure to unmount %s: %d.\n",
          DADiskGetBSDName(disk), DADissenterGetStatus(dissenter));
   }
   s->unmounted = dissenter == NULL;

   if (!s->eject) {
      /*
       * If we are not still waiting on a pending Eject operation, we are
       * done.
       */
      s->finished = TRUE;
      CFRunLoopStop(CFRunLoopGetCurrent());
   }
}


/*
 *----------------------------------------------------------------------
 *
 * FileMacosDADiskEjectCb --
 *
 *      Callback called when a disk is ejected.
 *
 * Results:
 *      Logs errors on failure.
 *
 * Side effects:
 *      Terminates the current CFRunLoop.
 *
 *----------------------------------------------------------------------
 */

static void
FileMacosDADiskEjectCb(DADiskRef disk,            // IN
                       DADissenterRef dissenter,  // IN
                       void *context)             // IN/OUT
{
   struct FileMacOsUnmountState *s = (struct FileMacOsUnmountState*)context;

   ASSERT(s);

   if (dissenter) {
      Log(LGPFX" DA reported failure to eject %s: %d.\n",
          DADiskGetBSDName(disk), DADissenterGetStatus(dissenter));
   }

   s->finished = TRUE;
   CFRunLoopStop(CFRunLoopGetCurrent());
}


/*
 *----------------------------------------------------------------------
 *
 * File_UnmountDev --
 *
 *      Given a BSD device name (disk1 for instance) or a full path
 *      (/dev/disk0s3), unmount the partitions mounted on it.
 *
 *      This function *must* either be called:
 *
 *         - With the BULL held
 *
 *      or:
 *
 *         - Early enough in initialization such that we can
 *           guarantee no lib/machPoll callbacks are registered.
 *
 * Results:
 *      TRUE if the disk was unmounted. (Even if there were errors
 *      ejecting the disk)
 *
 * Side effects:
 *      Runs a CFRunLoop, therefore this may invoke callbacks
 *      registered by Carbon APIs or by lib/machPoll.
 *
 *      Invokes DiskArbitration callbacks on all other processes
 *      that have registered applicable callbacks.  May block for
 *      several seconds while the disk is unmounted.
 *
 *----------------------------------------------------------------------
 */

Bool
File_UnmountDev(const char *devName,       // IN
                Bool  isFullPath,          // IN
                Bool  wholeDev,            // IN
                Bool  eject)               // IN
{
   /*
    * We use our own timeout so we can recover should 'diskarbitrationd' die.
    *
    * Our timeout should be longer than any timeout used by 'diskarbitrationd'
    * internally in order to avoid situations in which we timeout (thus
    * returning FALSE), but the disk in fact does get unmounted after we return
    * (so we should have returned TRUE). The maximum 'diskarbitrationd' timeout
    * we have experienced so far was 18 s.
    */
   static const CFTimeInterval timeout = 30.0;

   const CFStringRef runLoopMode = kCFRunLoopDefaultMode;
   struct FileMacOsUnmountState state;
   char dev[FILE_MAXPATH];
   const char *name;
   DASessionRef session;
   DADiskRef disk;

   state.unmounted = FALSE;
   state.finished  = FALSE;
   state.eject = eject;

   session = DASessionCreate(kCFAllocatorDefault);
   if (!session) {
      Log(LGPFX" Failed to create a DA session.\n");
      return FALSE;
   }

   if (isFullPath) {
      ASSERT(strncmp(devName, _PATH_DEV, strlen(_PATH_DEV)) == 0);
      name = devName;
   } else {
      Str_Snprintf(dev, sizeof dev, _PATH_DEV"%s", devName);
      name = dev;
   }
   disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, name);
   if (!disk) {
      Log(LGPFX" Failed to create a DA disk.\n");
      CFRelease(session);
      return FALSE;
   }

   DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(), runLoopMode);

   DADiskUnmount(disk,
      wholeDev ? kDADiskUnmountOptionWhole : kDADiskUnmountOptionDefault,
      FileMacosDADiskUnmountCb, &state);

   if (eject) {
      DADiskEject(disk, kDADiskEjectOptionDefault, FileMacosDADiskEjectCb,
                  &state);
   }

   if (!state.finished &&
       CFRunLoopRunInMode(runLoopMode,
                          timeout, FALSE) == kCFRunLoopRunTimedOut) {
      Log(LGPFX" Timeout while waiting for the DA callback.\n");
      state.finished = TRUE;
   }

   ASSERT(state.finished);

   DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(), runLoopMode);

   CFRelease(disk);
   CFRelease(session);
   return state.unmounted;
}

#endif /* __APPLE__ */


/*
 *----------------------------------------------------------------------
 *
 * File_Unlink --
 *
 *      Unlink the file.  If name is a symbolic link, then unlink the
 *      the file the link refers to as well as the link itself.  Only one
 *      level of links are followed.
 *
 * Results:
 *
 *      Return 0 if the unlink is successful.   Otherwise, returns -1.
 *
 * Side effects:
 *      The file is removed.
 *
 *----------------------------------------------------------------------
 */

int
File_Unlink(const char *name)   // IN
{
   struct stat statBuf;

   if (lstat(name, &statBuf) == -1) {
      return -1;
   }

   if (S_ISLNK(statBuf.st_mode)) {
       char buf[FILE_MAXPATH];
       ssize_t len = readlink(name, buf, sizeof buf - 1);
       if (len == -1) {
          return -1;
       }
       buf[len] = 0;
       if (unlink(buf) == -1) {
          if (errno != ENOENT) {
             return -1;
          }
       }
   }

   if (unlink(name) == -1) {
      return -1;
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * File_UnlinkDelayed --
 *
 *    Same as File_Unlink for POSIX systems since we can unlink anytime.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

int
File_UnlinkDelayed(const char *fileName)   // IN
{
   return File_Unlink(fileName);
}

/*
 *----------------------------------------------------------------------
 *
 * File_IsRemote --
 *
 *      Determine whether a file is on a remote filesystem.
 *      In case of an error be conservative and assume that 
 *      the file is a remote file.
 *
 * Results:
 *      The answer.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

#if !defined(__FreeBSD__) && !defined(sun)
Bool
File_IsRemote(const char *fileName) // IN: File name
{
#ifdef VMX86_SERVER
   // statfs will always return VMFS_MAGIC on ESX so it cannot be used to
   // find out about remoteness. On VMvisor it could return VMFS_NFS_MAGIC
   // but it is very slow, so let's just be as bad as ESX. XXX PR 158284
   return FALSE;
#else
   struct statfs sfbuf;

   if (statfs(fileName, &sfbuf) != 0) {
      Log("File_IsRemote: statfs(%s) failed: %s\n", 
          fileName, Msg_ErrString());
      return TRUE;
   }
#if __APPLE__
   return sfbuf.f_flags & MNT_LOCAL ? FALSE : TRUE;
#else
   if (NFS_SUPER_MAGIC == sfbuf.f_type) {
      return TRUE;
   }
   if (SMB_SUPER_MAGIC == sfbuf.f_type) {
      return TRUE;
   }
   return FALSE;
#endif
#endif
}
#endif /* !FreeBSD && !sun */


/*
 *----------------------------------------------------------------------
 *
 * File_IsSymLink --
 *
 *      Check if the specified file is a symbolic link or not
 *
 * Results:
 *      Bool - TRUE -> is a symlink, FALSE -> not a symlink or error
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsSymLink(const char *name)      // IN
{
   struct stat statBuf;

   ASSERT(name);

   if (lstat(name, &statBuf) == -1) {
      return FALSE;
   }
   return S_ISLNK(statBuf.st_mode);
}


/*
 *----------------------------------------------------------------------
 *
 * File_Cwd --
 *
 *      Find the current directory on drive DRIVE.
 *      DRIVE is either NULL (current drive) or a string
 *      starting with [A-Za-z].
 *
 * Results:
 *      NULL if error.
 *
 * Side effects:
 *      The result is allocated
 *
 *----------------------------------------------------------------------
 */

char *
File_Cwd(const char *drive)     // IN
{
   char buffer[FILE_MAXPATH];

   if (drive != NULL && drive[0] != '\0') {
      Warning("Drive letter %s on Linux?\n", drive);
   }

   if (getcwd(buffer, FILE_MAXPATH) == NULL) {
      Msg_Append(MSGID(filePosix.getcwd)
                 "Unable to retrieve the current working directory: %s. "
                 "Please check if the directory has been deleted or "
                 "unmounted.\n",
                 Msg_ErrString());
      Warning("%s:%d getcwd() failed: %s\n",
              __FILE__, __LINE__, Msg_ErrString());
      return NULL;
   };

   return Util_SafeStrdup(buffer);
}


/*
 *----------------------------------------------------------------------
 *
 * FileStripFwdSlashes --
 *
 *      Strips off extraneous forward slashes ("/") from the pathnames.
 *
 * Results:
 *      Stripped off path over-written in the supplied argument.
 *
 * Side effects:
 *      Argument over-written.
 *
 *----------------------------------------------------------------------
 */

static void
FileStripFwdSlashes(char *path)		// IN/OUT
{
   char *cptr = path;
   char *ptr = path;
   char *prev = path;

   if (!path) {
      return;
   }

   /*
    * Copy over if not DIRSEPC. If yes, copy over only if 
    * previous character was not DIRSEPC.
    */
   while (*ptr != '\0') {
      if (*ptr == DIRSEPC) {
         if (prev != ptr - 1) {
	    *cptr++ = *ptr;
	 }
         prev = ptr;
      } else {
         *cptr++ = *ptr;
      }
      ptr++;
   }

   *cptr = '\0';
}


/*
 *----------------------------------------------------------------------
 *
 * File_FullPath --
 *
 *      Compute the full path of a file. If the file if NULL or "", the
 *      current directory is returned
 *
 * Results:
 *      NULL if error (reported to the user)
 *
 * Side effects:
 *      The result is allocated
 *
 *----------------------------------------------------------------------
 */

char *
File_FullPath(const char *fileName)     // IN
{
   char *cwd;
   char *ret;
   char buffer[FILE_MAXPATH];
   char rpath[FILE_MAXPATH];

   if ((fileName != NULL) && (fileName[0] == '/')) {
      cwd = NULL;
   } else {
      cwd = File_Cwd(NULL);
      if (cwd == NULL) {
         ret = NULL;
         goto end;
      }
   }

   if ((fileName == NULL) || (fileName[0] == '\0')) {
      ret = cwd;
   } else if (fileName[0] == '/') {
      ret = (char *) fileName;
   } else {
      char *p;
      int n;

      n = Str_Snprintf(buffer, FILE_MAXPATH, "%s/%s", cwd, fileName);
      if (n < 0) {
         Warning("File_FullPath: Couldn't snprintf\n");
         ret = NULL;
         goto end;
      }
      p = realpath(buffer, rpath);
      if (p == 0) {
         ret = buffer;
      } else {
         ret = rpath;
      }
   }

   ret = Util_SafeStrdup(ret);

end:
   FileStripFwdSlashes(ret);
   if (cwd != NULL) {
      free(cwd);
   }
   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * File_IsFullPath --
 *
 *      Is this a full path?
 *
 * Results:
 *      TRUE if full path.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsFullPath(const char *fileName)   // IN
{
   return *fileName == DIRSEPC;
}


/*
 *----------------------------------------------------------------------
 *
 * File_SetTimes --
 *
 *      Set the date and time that a file was created, last accessed, or
 *      last modified.
 *
 * Results:
 *      TRUE if succeed or FALSE if error.
 *
 * Side effects:
 *      If fileName is a symlink, target's timestamps will be updated.
 *      Symlink itself's timestamps will not be changed.
 *
 *----------------------------------------------------------------------
 */

Bool
File_SetTimes(const char *fileName,       // IN
              VmTimeType createTime,      // IN: ignored
              VmTimeType accessTime,      // IN: Windows NT time format
              VmTimeType writeTime,       // IN: Windows NT time format
              VmTimeType attrChangeTime)  // IN: ignored
{
   struct timeval times[2];
   struct timeval *aTime, *wTime;
   struct stat statBuf;
   int error;

   /* We need the old stats so that we can preserve times. */
   if (lstat(fileName, &statBuf) == -1) {
      error = errno;
      Log(LGPFX" error stating file \"%s\": %s\n", fileName, strerror(error));
      return FALSE;
   }

   aTime = &times[0];
   wTime = &times[1];

   /*
    * Preserve old times if new time <= 0.
    * XXX Need a better implementation to preserve tv_usec.
    */
   aTime->tv_sec = statBuf.st_atime;
   aTime->tv_usec = 0;
   wTime->tv_sec = statBuf.st_mtime;
   wTime->tv_usec = 0;

   if (accessTime > 0) {
      struct timespec ts;
      TimeUtil_NtTimeToUnixTime(&ts, accessTime);
      aTime->tv_sec = ts.tv_sec;
      aTime->tv_usec = ts.tv_nsec / 1000;
   }

   if (writeTime > 0) {
      struct timespec ts;
      TimeUtil_NtTimeToUnixTime(&ts, writeTime);
      wTime->tv_sec = ts.tv_sec;
      wTime->tv_usec = ts.tv_nsec / 1000;
   }

   if (utimes(fileName, times) < 0) {
      error = errno;
      Log(LGPFX" utimes error on file \"%s\": %s\n", fileName, strerror(error));
      return FALSE;
   }

   return TRUE;
}


#if !defined(__FreeBSD__) && !defined(sun)
/*
 *----------------------------------------------------------------------
 *
 * FileGetStats --
 *
 *      Calls statfs on a full path (eg. something returned from File_FullPath)
 *
 * Results:
 *      -1 if error
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Bool
FileGetStats(const char *fullPath,      // IN 
             struct statfs *pstatfsbuf) // OUT
{
   Bool retval = TRUE;
   char *dupPath = NULL;

   while (statfs(dupPath? dupPath : fullPath, pstatfsbuf) == -1) {
      char *pos;
      
      if (errno != ENOENT) {
         retval = FALSE;
         goto out;
      }

      if (!dupPath) {
         /* Dup fullPath, so as not to modify input parameters */
         dupPath = Util_SafeStrdup(fullPath);
      }

      /* The file does not exist */
      pos = strrchr(dupPath, '/');
      ASSERT(pos);
      if (pos == dupPath) {
         /* Use the root directory as a last resort */
         *(pos + 1) = '\0';
      } else {
         /* Reduce the path by one item */
         *pos = '\0';
      }
   }
   
out:
   free(dupPath);
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * File_GetFreeSpace --
 *
 *      Return the free space (in bytes) available to the user on a disk where
 *      a file is or would be
 *
 * Results:
 *      -1 if error (reported to the user)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

uint64
File_GetFreeSpace(const char *fileName) // IN: File name
{
   char *fullPath;
   uint64 ret;
   struct statfs statfsbuf;

   fullPath = File_FullPath(fileName);
   if (fullPath == NULL) {
      ret = -1;
      goto end;
   }
   
  if (!FileGetStats(fullPath, &statfsbuf)) {
      Warning("File_GetFreeSpace: Couldn't statfs\n");
      ret = -1;
      goto end;
   }
  ret = (uint64)statfsbuf.f_bavail * statfsbuf.f_bsize;
#ifdef VMX86_SERVER
   // The following test is never true on VMvisor but we do not care as
   // this is only intended for callers going through vmkfs. Direct callers
   // as we are always get the right answer from statfs above.
   if (statfsbuf.f_type == VMFS_MAGIC_NUMBER) {
      int r;
      int fd;
      FS_FreeSpaceArgs args = { 0 };
      char *directory = NULL;

      File_SplitName(fullPath, NULL, &directory, NULL);
      /* Must use an ioctl() to get free space for a VMFS file. */
      ret = -1;
      fd = open(directory, O_RDONLY);
      if (fd != -1) {
	 r = ioctl(fd, IOCTLCMD_VMFS_GET_FREE_SPACE, &args);
	 if (r != -1) {
	    ret = args.bytesFree;
	 } else {
            Warning("GetFreeSpace: ioctl on %s failed with %d %d\n", 
                    fullPath, errno, r);
         }
	 close(fd);
      } else {
         Warning("GetFreeSpace: open of %s failed with: %s\n", directory,
                 Msg_ErrString());
      }
      free(directory);
   }
#endif

end:
   free(fullPath);
   return ret;
}

#ifdef VMX86_SERVER

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSAttributes --
 *
 *      Acquire the attributes for a given file on a VMFS volume.
 *
 * Results:
 *      Integer return value and populated FS_PartitionListResult
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSAttributes(const char *fileName,             // IN: File to test
                       FS_PartitionListResult **fsAttrs) // IN/OUT: VMFS Info
{
   int ret = -1;
   int fd;
   char *pathname = File_FullPath(fileName);
   char *parentPath;

   File_SplitName(pathname, NULL, &parentPath, NULL);

   if(parentPath == NULL) {
      Log(LGPFX "%s: Error acquiring parent path name\n", __func__);
      free(pathname);
      return -1;
   }

   if (!File_OnVMFS(fileName)) {
      Log(LGPFX "%s: File %s not on VMFS volume\n", __func__, fileName);
      free(pathname);
      free(parentPath);
      return -1;
   }

   *fsAttrs = Util_SafeMalloc(FS_PARTITION_ARR_SIZE(FS_PLIST_DEF_MAX_PARTITIONS));

   if (*fsAttrs == NULL) {
      Log(LGPFX "%s: failed to allocate memory\n", __func__);
      free(pathname);
      free(parentPath);
      return -1;
   }

   memset(*fsAttrs, 0, FS_PARTITION_ARR_SIZE(FS_PLIST_DEF_MAX_PARTITIONS));

   (*fsAttrs)->ioctlAttr.maxPartitions = FS_PLIST_DEF_MAX_PARTITIONS;
   (*fsAttrs)->ioctlAttr.getAttrSpec = FS_ATTR_SPEC_BASIC;

   fd = open(parentPath, O_RDONLY);
   if (fd < 0) {
      Log(LGPFX "%s: could not open %s.\n", __func__, fileName);
      goto done;
   }

   ret = ioctl(fd, IOCTLCMD_VMFS_FS_GET_ATTR, (char *) *fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: Could not get volume attributes (ret = %d)\n", __func__,
              ret);
   }

done:
   if (fd) {
      close(fd);
   }

   free(pathname);
   free(parentPath);   
   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSVersion --
 *
 *      Acquire the version number for a given file on a VMFS file system.
 *
 * Results:
 *      Integer return value and version number
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSVersion(const char *fileName, // IN: Filename to test
                    uint32 *version)      // IN/OUT: version number of VMFS
{
   int ret = -1;
   FS_PartitionListResult *fsAttrs = NULL;

   ret = File_GetVMFSAttributes(fileName, &fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: File_GetVMFSAttributes failed\n", __func__);
      goto done;
   }

   *version = fsAttrs->versionNumber;

done:
   if (fsAttrs) {
      free(fsAttrs);
   }
   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSBlockSize --
 *
 *      Acquire the blocksize for a given file on a VMFS file system.
 *
 * Results:
 *      Integer return value and block size
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSBlockSize(const char *fileName, // IN: File name to test
                      uint32 *blockSize)    // IN/OUT: VMFS block size
{
   int ret = -1;
   FS_PartitionListResult *fsAttrs = NULL;

   ret = File_GetVMFSAttributes(fileName, &fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: File_GetVMFSAttributes failed\n", __func__);
      goto done;
   }

   *blockSize = fsAttrs->fileBlockSize;

done:
   if (fsAttrs) {
      free(fsAttrs);
   }
   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetVMFSfsType --
 *
 *      Acquire the fsType for a given file on a VMFS.
 *
 * Results:
 *      Integer return value and fs type
 *
 * Side effects:
 *      Will fail if file is not on VMFS or not enough memory for partition
 *      query results
 *
 *----------------------------------------------------------------------
 */

static int
File_GetVMFSfsType(const char *fileName, // IN: File name to test
                   char **fsType)        // IN/OUT: VMFS fsType
{
   int ret = -1;
   FS_PartitionListResult *fsAttrs = NULL;

   ret = File_GetVMFSAttributes(fileName, &fsAttrs);
   if (ret < 0) {
      Log(LGPFX "%s: File_GetVMFSAttributes failed\n", __func__);
      goto done;
   }

   *fsType = Util_SafeMalloc(sizeof(char) * FS_PLIST_DEF_MAX_FSTYPE_LEN);
   memcpy(*fsType, fsAttrs->fsType, FS_PLIST_DEF_MAX_FSTYPE_LEN);

done:
   if (fsAttrs) {
      free(fsAttrs);
   }
   return ret;
}


#endif

/*
 *----------------------------------------------------------------------
 *
 * File_OnVMFS --
 *
 *      Return TRUE if file is on a VMFS file system.
 *
 * Results:
 *      Boolean
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_OnVMFS(const char *fileName)
{
#ifdef VMX86_SERVER
   char *fullPath;
   Bool ret;
   struct statfs statfsbuf;

   // XXX See Vmfs_IsVMFSDir. Same caveat about fs exclusion.
   if (HostType_OSIsPureVMK()) {
      return TRUE;
   }

   /*
    * Do a quick statfs() for best performance in the case that the file
    * exists.  If file doesn't exist, then get the full path and do a
    * FileGetStats() to check each of the parent directories.
    */
   if (statfs(fileName, &statfsbuf) == -1) {

      fullPath = File_FullPath(fileName);
      if (fullPath == NULL) {
	 ret = FALSE;
	 goto end;
      }
   
      if (!FileGetStats(fullPath, &statfsbuf)) {
	 Warning("File_IsVMFS: Couldn't statfs\n");
	 ret = FALSE;
	 free(fullPath);
	 goto end;
      }
      free(fullPath);
   }
   ret = (statfsbuf.f_type == VMFS_MAGIC_NUMBER);

end:
   return ret;
#else
   return FALSE;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * File_GetCapacity --
 *
 *      Return the total capcity (in bytes) available to the user on a disk where
 *      a file is or would be
 *
 * Results:
 *      -1 if error (reported to the user)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

uint64
File_GetCapacity(const char *fileName) // IN: File name
{
   char *fullPath;
   uint64 ret;
   struct statfs statfsbuf;

   fullPath = File_FullPath(fileName);
   if (fullPath == NULL) {
      ret = -1;
      goto end;
   }
   
   if (!FileGetStats(fullPath, &statfsbuf)) {
      Warning("File_GetCapacity: Couldn't statfs\n");
      ret = -1;
      goto end;
   }

   ret = (uint64)statfsbuf.f_blocks * statfsbuf.f_bsize;

end:
   free(fullPath);
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * File_GetUniqueFileSystemID --
 *
 *      Returns a string which uniquely identifies the underlying filesystem
 *      for a given path
 *
 *      On linux we choose the underlying device's name as the unique ID
 *
 *      NB: The file need not exist
 *
 *      XXX: I make no claim that this is 100% unique so if you need this
 *           functionality to be 100% perfect, I suggest you think about it
 *           more deeply than I did. -meccleston
 *
 * Results:
 *      char* - allocated string containing name of device (must be free()'d)
 *                NULL on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

char * 
File_GetUniqueFileSystemID(const char *fileName)        // IN
{
   char realFileName[FILE_MAXPATH];
   char *end;
   char *mntDev;

   /* Convert to canonical name */
   realpath(fileName, realFileName);

   /* VCFS doesn't have real mount points, confusing FilePosixLookupFSDeviceName
    * below, causing it to return "/vmfs", instead of the actual mount point.  
    *
    * See bug 61646 for why we care.
    */
#ifdef VMX86_SERVER
   if (strncmp(realFileName, VCFS_MOUNT_POINT, strlen(VCFS_MOUNT_POINT)) == 0) {
      char vmfsVolumeName[FILE_MAXPATH];
      if (sscanf(realFileName, VCFS_MOUNT_PATH "%[^/]%*s", vmfsVolumeName) == 1) {
         return Str_Asprintf(NULL, "%s/%s", VCFS_MOUNT_POINT, vmfsVolumeName);
      }
   }
#endif

   /*
    * March backwards from the end of the path.  When we encounter a '/'
    * look it up to see if we find a mount point match.
    */
   end = &realFileName[strlen(realFileName)];
   while (1) {
      /*
       * Truncate string until we find a '/'
       */
      while (*end != '/') {
         if (end < realFileName) {
            return NULL;
         }
         *end-- = 0;
      }

      mntDev = FilePosixLookupFSDeviceName(realFileName);
      if (mntDev != NULL) {
         /* Found it! */
         return mntDev;
      }

      /*
       * No match, chop off the trailing '/' and try again
       */
      *end-- = 0;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * FilePosixLookupFSDeviceName --
 *
 *      Looks up passed in filename in list of mount points.  If there is
 *      a match, it returns the underlying device name of the mount point.
 *
 * Results:
 *      char* - Name of FS device (needs to be free()'d) or NULL if no match
 *              is found.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static char *
FilePosixLookupFSDeviceName(const char *filename)     // IN
{
#if __APPLE__ 
   struct statfs buf;
   int res;

   res = statfs(filename, &buf);
   if (res != 0) {
      NOT_TESTED();
      return NULL;
   }
   return Util_SafeStrdup(buf.f_mntfromname);
#else
   FILE* f;
   struct mntent *mnt;
   char realFileName[FILE_MAXPATH];

   realpath(filename, realFileName);

   f = setmntent(MOUNTED, "r");
   if (f == NULL) {
      return NULL;
   }

   while ((mnt = getmntent(f)) != NULL) {
      /*
       * NB: A call to realpath is not needed as getmntent() already
       *     returns it in canonical form.  Additionally, it is bad
       *     to call realpath() as often a mount point is down, and
       *     realpath calls stat which can block trying to stat
       *     a filesystem that the caller of the function is not at
       *     all expecting
       */
      if (strcmp(mnt->mnt_dir, realFileName) == 0) {
         endmntent(f);
         return Util_SafeStrdup(mnt->mnt_fsname);
      }
   }
   endmntent(f);
   return NULL;
#endif
}


/*
 *----------------------------------------------------------------------------
 *
 * File_IsSameFile --
 *
 *      Determine whether both paths point to the same file.
 *
 * Results:
 *      TRUE if both paths point to the same file, FALSE otherwise.
 *
 * Side effects:
 *      Changes errno, maybe.
 *
 *----------------------------------------------------------------------------
 */

Bool
File_IsSameFile(const char *path1, // IN
                const char *path2) // IN
{
   int err;
   struct stat st1;
   struct stat st2;
   char *fs1;
   char *fs2;

   ASSERT(path1);
   ASSERT(path2);

#ifdef VMX86_SERVER
   {

      char realpath1[FILE_MAXPATH];
      char realpath2[FILE_MAXPATH];

      fs1 = realpath(path1, realpath1);
      fs2 = realpath(path2, realpath2);

      /*
       * ESX doesn't have real inodes for VMFS disks in User Worlds. So only way
       * to check if a file is the same is using real path.  So said Satyam.
       */

      if (fs1 &&
          strncmp(fs1, VCFS_MOUNT_POINT, strlen(VCFS_MOUNT_POINT)) == 0) {
         if (!fs2 || strcmp(realpath1, realpath2) != 0) {
            return FALSE;
         } else {
            return TRUE;
         }
      }
   }
#endif


   if (strcmp(path1, path2) == 0) {
      return TRUE;
   }

   err = stat(path1, &st1);
   if (err == -1) {
      return FALSE;
   }

   err = stat(path2, &st2);
   if (err == -1) {
      return FALSE;
   }

   if (st1.st_ino != st2.st_ino) {
      return FALSE;
   }

   fs1 = File_GetUniqueFileSystemID(path1);
   if (fs1 == NULL) {
      return FALSE;
   }

   fs2 = File_GetUniqueFileSystemID(path2);
   if (fs2 == NULL) {
      free(fs1);
      return FALSE;
   }

   if (strcmp(fs1, fs2) == 0) {
      free(fs1);
      free(fs2);
      return TRUE;
   }

   free(fs1);
   free(fs2);

   return FALSE;
}


#endif /* !FreeBSD && !sun */


/*
 *-----------------------------------------------------------------------------
 *
 * File_Replace --
 *
 *      Replace old file with new file, and attempt to reproduce
 *      file permissions.
 *
 * Results:
 *      TRUE on success.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
File_Replace(const char *oldFile,        // IN: old file
             const char *newFile)        // IN: new file
{
   struct stat st;

   if (stat(oldFile, &st) == 0 &&
       chmod(newFile, st.st_mode) == -1) {
      Msg_Append(MSGID(filePosix.replaceChmodFailed)
                 "Failed to duplicate file permissions from "
                 "\"%s\" to \"%s\": %s\n",
                 oldFile, newFile, Msg_ErrString());
      return FALSE;
   }
   if (rename(newFile, oldFile) == -1) {
      Msg_Append(MSGID(filePosix.replaceRenameFailed)
                 "Failed to rename \"%s\" to \"%s\": %s\n",
                 newFile, oldFile, Msg_ErrString());
      return FALSE;
   }
   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * File_GetModTime -- 
 *
 *      Get the modification time of a file.
 *
 * Results:
 *      Last modification time of file or -1 if error.
 *      
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int64
File_GetModTime(const char *fileName)   // IN
{
   struct stat statBuf;
   if (stat(fileName, &statBuf) == -1) {
      return -1;
   }
   return (int64)statBuf.st_mtime;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIsVMFS --
 *
 *      Determine whether specified file lives on VMFS filesystem.
 *      Only Linux host can have VMFS, so skip it on Solaris
 *      and FreeBSD.
 *
 * Results:
 *      TRUE if specified file lives on VMFS
 *      FALSE if file is not on VMFS or does not exist
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
FileIsVMFS(const char *fileName)  // IN: file name to test
{
#if defined(linux)
   struct statfs sfbuf;

#ifdef VMX86_SERVER
   // XXX See Vmfs_IsVMFSFile. Same caveat about fs exclusion.
   if (HostType_OSIsPureVMK()) {
      return TRUE;
   }
#endif
   if (statfs(fileName, &sfbuf) == 0) {
      return sfbuf.f_type == VMFS_SUPER_MAGIC;
   }
#endif
   return FALSE;

}


/*
 *----------------------------------------------------------------------
 *
 * FilePosixCreateTestFileSize --
 *
 *      See if the given directory is on a file system that supports
 *      large files.  We just create an empty file and pass it to the
 *      FileIO_SupportsFileSize which does actual job of determining
 *      file size support.
 *
 * Results:
 *      TRUE if FS supports files of specified size
 *      FALSE otherwise (no support, invalid path, ...)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
FilePosixCreateTestFileSize(const char *dirName,  // IN: directory to create large file
                            uint64      fileSize) // IN: test file size
{
   FileIODescriptor fd;
   char pathname[FILE_MAXPATH];
   Bool retVal;
   char *tmpFileName;
   int posixFD;

   Str_Sprintf(pathname, sizeof pathname, "%s/.vmBigFileTest", dirName);
   posixFD = File_MakeTemp(pathname, &tmpFileName);
   if (posixFD == -1) {
      return FALSE;
   }
   
   fd = FileIO_CreateFDPosix(posixFD, O_RDWR);
   retVal = FileIO_SupportsFileSize(&fd, fileSize);
   /* Eventually perform destructive tests here... */
   FileIO_Close(&fd);
   File_Unlink(tmpFileName);
   free(tmpFileName);
   return retVal;
}

/*
 *----------------------------------------------------------------------
 *
 * File_VMFSSupportsFileSize --
 *
 *      Check if the given file is on a VMFS supports such a file size
 *
 *      In the case of VMFS2, the largest supported file size is
 *         456 * 1024 * B bytes
 *
 *      In the case of VMFS3/4, the largest supported file size is
 *         256 * 1024 * B bytes
 *
 *      where B represents the blocksize in bytes
 *
 *
 * Results:
 *      TRUE if VMFS supports such file size
 *      FALSE otherwise (file size not supported)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Bool
File_VMFSSupportsFileSize(const char *fileName, // IN
                          uint64      fileSize) // IN
{
#ifdef VMX86_SERVER
   uint32 version;
   uint32 blockSize;
   uint64 maxFileSize = -1;
   Bool supported;
   char *pathName;
   char *parentPath;
   char *fsType;

   if (File_GetVMFSVersion(fileName, &version) < 0) {
      Log(LGPFX "%s: File_GetVMFSVersion Failed\n", __func__);
      return FALSE;
   }
   if (File_GetVMFSBlockSize(fileName, &blockSize) < 0) {
      Log(LGPFX "%s: File_GetVMFSBlockSize Failed\n", __func__);
      return FALSE;
   }
   if (File_GetVMFSfsType(fileName, &fsType) < 0) {
      Log(LGPFX "%s: File_GetVMFSfsType Failed\n", __func__);
      return FALSE;
   }

   if (strcmp(fsType, "VMFS") == 0) {
      if (version == 2) {
         maxFileSize = (VMFS2CONST * (uint64) blockSize * 1024);
      } else if (version >= 3) {
         /* Get ready for VMFS4 and perform sanity check on version */
         ASSERT(version == 3 || version == 4);

         maxFileSize = (VMFS3CONST * (uint64) blockSize * 1024);
      } 

      if (fileSize <= maxFileSize && maxFileSize != -1) {
         free(fsType);
         return TRUE;
      } else {
         Log(LGPFX "Requested file size (%"FMT64"d) larger than maximum "
             "supported filesystem file size (%"FMT64"d)\n",
             fileSize, maxFileSize);
         free(fsType);
         return FALSE;
      }

   } else {
      pathName = File_FullPath(fileName);
      if (pathName == NULL) {
         Log(LGPFX "%s: Error acquiring full path\n", __func__);
         free(fsType);
         return FALSE;
      }

      File_SplitName(pathName, NULL, &parentPath, NULL);
      if (parentPath == NULL) {
         Log(LGPFX "%s: Error acquiring parent path name\n", __func__);
         free(fsType);
         free(pathName);
         return FALSE;
      }

      supported = FilePosixCreateTestFileSize(parentPath, fileSize);
      
      free(fsType);
      free(pathName);
      free(parentPath);
      return supported;
   }
   
#endif
   Log(LGPFX "%s did not execute properly\n", __func__);
   return FALSE; /* happy compiler */
}

/*
 *----------------------------------------------------------------------
 *
 * File_SupportsFileSize --
 *
 *      Check if the given file is on an FS that supports such file size
 *
 * Results:
 *      TRUE if FS supports such file size
 *      FALSE otherwise (file size not supported, invalid path, read-only, ...)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
File_SupportsFileSize(const char *filePath, // IN
                      uint64      fileSize) // IN
{
   Bool supported = FALSE;
   char *p;
   char *pathname;
   char *parentPath = NULL;

   /* All supported filesystems can hold at least 2GB - 1 files. */
   if (fileSize <= 0x7FFFFFFF) {
      return TRUE;
   }

   /* 
    * We acquire the full path name for testing in 
    * FilePosixCreateTestFileSize().  This is also done in the event that
    * a user tries to create a virtual disk in the directory that they want
    * a vmdk created in (setting filePath only to the disk name, not the entire
    * path.
    */
   pathname = File_FullPath(filePath);
   if (pathname == NULL) {
      Log(LGPFX "%s: Error acquiring full path\n", __func__);
      goto out;
   }

   /* 
    * We then truncate the name to point to the parent directory of the file
    * created so we can get accurate results from FileIsVMFS.
    */
   File_SplitName(pathname, NULL, &parentPath, NULL);
   if (parentPath == NULL) {
      Log(LGPFX "%s: Error acquiring parent path name\n", __func__);
      goto out;
   }

   /* 
    * We know that VMFS supports large files - But they have limitations
    * See function File_VMFSSupportsFileSize() - PR 146965
    */
   if (FileIsVMFS(parentPath)) {
      supported = File_VMFSSupportsFileSize(filePath, fileSize);
      goto out;
   }

   if (File_IsFile(filePath)) {
      FileIODescriptor fd;
      FileIOResult res;

      FileIO_Invalidate(&fd);
      res = FileIO_Open(&fd, filePath, FILEIO_OPEN_ACCESS_READ, FILEIO_OPEN);
      if (res == FILEIO_SUCCESS) {
         supported = FileIO_SupportsFileSize(&fd, fileSize);
         FileIO_Close(&fd);
         goto out;
      }
   }

   p = strrchr(pathname, '/');
   if (p == NULL) {
      free(pathname);
      pathname = File_Cwd(NULL);

      if (pathname == NULL) {
         goto out;
      }
   } else {
      *p = '\0';
   }

   /*
    * On unknown filesystems create temporary file and use it to test.
    */
   supported = FilePosixCreateTestFileSize(parentPath, fileSize);

out:
   
   free(pathname);
   free(parentPath);
   return supported;
}


/*
 *----------------------------------------------------------------------
 *
 * File_CreateDirectory --
 *
 *      Creates the specified directory.
 *
 * Results:
 *      True if the directory is successfully created, false otherwise.
 *
 * Side effects:
 *      Creates the directory on disk.
 *
 *----------------------------------------------------------------------
 */

Bool
File_CreateDirectory(char const *pathName)     // IN
{
   ASSERT(pathName);
   return mkdir(pathName, S_IRWXU | S_IRWXG | S_IRWXO) == 0;
}


/*
 *----------------------------------------------------------------------
 *
 * File_DeleteEmptyDirectory --
 *
 *      Deletes the specified directory if it is empty.
 *
 * Results:
 *      True if the directory is successfully deleted, false otherwise.
 *
 * Side effects:
 *      Deletes the directory from disk.
 *
 *----------------------------------------------------------------------
 */

Bool
File_DeleteEmptyDirectory(char const *pathName)     // IN
{
   ASSERT(pathName);
   return rmdir(pathName) == 0;
}


/*
 *----------------------------------------------------------------------
 *
 * File_ListDirectory --
 *
 *      Gets the list of files (and directories) in a directory.
 *
 * Results:
 *      Returns the number of files returned or -1 on failure.
 *
 * Side effects:
 *      If ids is provided and the function succeeds, memory is allocated
 *      and must be freed.  Array of strings and array itself must be
 *      freed.
 *
 *----------------------------------------------------------------------
 */

int
File_ListDirectory(char const *pathName,     // IN
                   char ***ids)              // OUT: relative paths
{
   int err;
   DIR *dir;
   DynBuf b;
   int count = 0;

   ASSERT(pathName);

   errno = 0;
   dir = opendir(pathName);

   if (dir == (DIR *) NULL) {
      // errno is accessible, in the future, for more detail
      return -1;
   }

   DynBuf_Init(&b);

   while (TRUE) {
      struct dirent *entry;

      errno = 0;
      entry = readdir(dir);

      if (entry == (struct dirent *) NULL) {
         err = errno;
         break;
      }

      /* Strip out undesirable paths.  No one ever cares about these. */
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
         continue;
      }

      /* Don't create the file list if we aren't providing it to the caller. */
      if (ids) {
         char *id = Util_SafeStrdup(entry->d_name);
         DynBuf_Append(&b, &id, sizeof(&id));
      }

      count++;
   }

   closedir(dir);

   if (ids && (err == 0)) {
      *ids = DynBuf_AllocGet(&b);
      ASSERT_MEM_ALLOC(*ids);
   }

   DynBuf_Destroy(&b);

   return (err == 0) ? count : -1;
}


/*
 *----------------------------------------------------------------------
 *
 * File_IsWritableDir --
 *
 *	Determine in a non-intrusive way if the user can create a file in a
 *	directory
 *
 * Results:
 *	FALSE if error (reported to the user)
 *
 * Side effects:
 *	None
 *
 * Bug:
 *	It would be cleaner to use the POSIX access(2), which deals well
 *	with read-only filesystems. Unfortunately, access(2) doesn't deal with
 *	the effective [u|g]ids.
 *
 *----------------------------------------------------------------------
 */

Bool
File_IsWritableDir(const char *dirName)
{
   struct stat statbuf;
   uid_t euid;

   if (stat(dirName, &statbuf) == -1) {
      return FALSE;
   }

   if (!S_ISDIR(statbuf.st_mode)) {
      return FALSE;
   }

   euid = geteuid();
   if (euid == 0) {
      /* Root can read or write any file. Well... This is not completely true
         because of read-only filesystems and NFS root squashing... What a
         nightmare --hpreg */
      return TRUE;
   }

   if (statbuf.st_uid == euid) {
      statbuf.st_mode >>= 6;
   } else if (FileIsGroupsMember(statbuf.st_gid)) {
      statbuf.st_mode >>= 3;
   }

   /* Check for Read and Execute permissions */
   return (statbuf.st_mode & 3) == 3;
}


/*
 *----------------------------------------------------------------------
 *
 * FileTryDir --
 *
 *	Check to see if the given directory is actually a directory
 *      and is writable by us.
 *
 * Results:
 *	The expanded directory name on success, NULL on failure.
 *
 * Side effects:
 *	The result is allocated.
 *
 *----------------------------------------------------------------------
 */

static char *
FileTryDir(const char *dirName) // IN: Is this a writable directory?
{
   char *edirName;

   if (dirName == NULL) {
      return NULL;
   }

   edirName = Util_ExpandString(dirName);
   if (edirName != NULL && File_IsWritableDir(edirName)) {
      return edirName;
   }
   free(edirName);

   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * File_GetTmpDir --
 *
 *	Determine the best temporary directory. Unsafe since the
 *	returned directory is generally going to be 0777, thus all sorts
 *	of denial of service or symlink attacks are possible.  Please
 *	use Util_GetSafeTmpDir if your dependencies permit it.
 *
 * Results:
 *	NULL if error (reported to the user).
 *
 * Side effects:
 *	The result is allocated.
 *
 *----------------------------------------------------------------------
 */

char *
File_GetTmpDir(Bool useConf) // IN: Use the config file?
{
   char *dirName;
   char *edirName;

   /* Make several attempts to find a good temporary directory candidate */

   if (useConf) {
      dirName = (char *)LocalConfig_GetString(NULL, "tmpDirectory");
      edirName = FileTryDir(dirName);
      free(dirName);
      if (edirName != NULL) {
         return edirName;
      }
   }

   /* getenv string must _not_ be freed */
   edirName = FileTryDir(getenv("TMPDIR"));
   if (edirName != NULL) {
      return edirName;
   }

   /* P_tmpdir is usually defined in <stdio.h> */
   edirName = FileTryDir(P_tmpdir);
   if (edirName != NULL) {
      return edirName;
   }

   edirName = FileTryDir("/tmp");
   if (edirName != NULL) {
      return edirName;
   }

   edirName = FileTryDir("~");
   if (edirName != NULL) {
      return edirName;
   }

   dirName = File_Cwd(NULL);

   if (dirName != NULL) {
      edirName = FileTryDir(dirName);
      free(dirName);
      if (edirName != NULL) {
         return edirName;
      }
   }

   edirName = FileTryDir("/");
   if (edirName != NULL) {
      return edirName;
   }

   Warning("File_GetTmpDir: Couldn't get a temporary directory\n");
   return NULL;
}

#undef HOSTINFO_TRYDIR


/*
 *----------------------------------------------------------------------
 *
 * FileIsGroupsMember --
 *
 *	Determine if a gid is in the gid list of the current process
 *
 * Results:
 *	FALSE if error (reported to the user)
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static Bool
FileIsGroupsMember(gid_t gid)
{
   int nr_members;
   gid_t *members;
   int res;
   int ret;

   members = NULL;
   nr_members = 0;
   for (;;) {
      gid_t *new;

      res = getgroups(nr_members, members);
      if (res == -1) {
	 Warning("FileIsGroupMember: Couldn't getgroups\n");
	 ret = FALSE;
	 goto end;
      }

      if (res == nr_members) {
	 break;
      }

      /* Was bug 17760 --hpreg */
      new = realloc(members, res * sizeof *members);
      if (new == NULL) {
	 Warning("FileIsGroupMember: Couldn't realloc\n");
	 ret = FALSE;
	 goto end;
      }

      members = new;
      nr_members = res;
   }

   for (res = 0; res < nr_members; res++) {
      if (members[res] == gid) {
	 ret = TRUE;
	 goto end;
      }
   }
   ret = FALSE;

end:
   free(members);

   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * File_MakeCfgFileExecutable --
 *
 *	Make a .vmx file executable. This is sometimes necessary 
 *      to enable MKS access to the VM.
 *
 * Results:
 *	FALSE if error
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

Bool
File_MakeCfgFileExecutable(const char *path)
{   
   return chmod(path,
                S_IRUSR | S_IWUSR | S_IXUSR |  // rwx by user
                S_IRGRP | S_IXGRP |            // rx by group
                S_IROTH | S_IXOTH              // rx by others
                ) == 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * File_GetSizeAlternate --
 *
 *      An alternate way to determine the filesize. Useful for finding problems
 *      with files on remote fileservers, such as described in bug 19036.
 *      However, in Linux we do not have an alternate way, yet, to determine the
 *      problem, so we call back into the regular getSize function.
 *
 * Results:
 *      Size of file or -1.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

int64
File_GetSizeAlternate(const char *fileName)        // IN
{
   return File_GetSize(fileName);
}

/*
 *----------------------------------------------------------------------------
 *
 * File_IsCharDevice --
 *
 *      For files like /dev/ttyS0, /dev/lp0 we need to know whether
 *      they are device files so that we can take appropriate action.
 *      This function checks whether the given file is a char device
 *      and return TRUE in such case.
 *
 * Results:
 *      TRUE if the file is char device FALSE otherwise.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

Bool
File_IsCharDevice(const char *filename) //IN
{
   struct stat st;
   if (stat(filename, &st) >= 0 && S_ISCHR(st.st_mode)) {
      return TRUE;
   }
   return FALSE;
}


