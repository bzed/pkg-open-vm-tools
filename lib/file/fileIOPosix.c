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
 * fileIOPosix.c --
 *
 *      Implementation of the file library host specific functions for linux.
 */


#if !defined(VMX86_TOOLS) && !defined(__APPLE__)
#define FILEIO_SUPPORT_ODIRECT
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#if !defined(N_PLAT_NLM) && defined(linux)
#include <linux/unistd.h>
#endif
#include <sys/stat.h>
#include "su.h"

#if defined(__APPLE__)
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/param.h>
#include <sys/mount.h>
#endif

/* Check for non-matching prototypes */
#include "vmware.h"
#include "str.h"
#include "file.h"
#include "fileInt.h"
#include "config.h"
#include "util.h"
#include "iovector.h"
#include "stats_file.h"

static const unsigned int FileIO_SeekOrigins[] = {
   SEEK_SET,
   SEEK_CUR,
   SEEK_END,
};

static const int FileIO_OpenActions[] = {
   0,
   O_TRUNC,
   O_CREAT,
   O_CREAT | O_EXCL,
   O_CREAT | O_TRUNC,
};

/*
 * Options for FileCoalescing performance optimization
 */
typedef struct FilePosixOptions {
   Bool initialized;
   Bool enabled;
   int countThreshold;
   int sizeThreshold;
} FilePosixOptions;

static FilePosixOptions filePosixOptions;


/*
 *-----------------------------------------------------------------------------
 *
 * FileIOErrno2Result --
 *
 *      Convert a POSIX errno to a FileIOResult code.
 *
 * Results:
 *      The FileIOResult corresponding to the errno, FILEIO_ERROR by default.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static FileIOResult
FileIOErrno2Result(int error) // IN: errno to convert
{
   switch (error) {
   case EEXIST:
      return FILEIO_OPEN_ERROR_EXIST;
   case ENOENT:
      return FILEIO_FILE_NOT_FOUND;
   case EACCES:
      return FILEIO_NO_PERMISSION;
   case ENAMETOOLONG:
      return FILEIO_FILE_NAME_TOO_LONG;
   default:
      return FILEIO_ERROR;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_OptionalSafeInitialize --
 *
 *      Initialize global state. If this module is called from a
 *      thread other than the VMX or VCPU threads, like an aioGeneric worker
 *      thread, then we cannot do things like call config. Do that sort
 *      of initialization here, which is called from a safe thread.
 *
 *      This routine is OPTIONAL if you do not call this module from a
 *      worker thread. The same initialization can be done lazily when
 *      a read/write routine is called.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

INLINE void
FileIO_OptionalSafeInitialize()
{
   if (!filePosixOptions.initialized) {
      filePosixOptions.enabled =
         Config_GetBool(TRUE, "filePosix.coalesce.enable");
      filePosixOptions.countThreshold =
         Config_GetLong(5, "filePosix.coalesce.count");
      filePosixOptions.sizeThreshold =
         Config_GetLong(16*1024, "filePosix.coalesce.size");
      filePosixOptions.initialized = TRUE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Invalidate --
 *
 *      Initialize a FileIODescriptor with an invalid value
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
FileIO_Invalidate(FileIODescriptor *fd) // OUT
{
   (memset)(fd, 0, sizeof *fd);
   fd->posix = -1;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_IsValid --
 *
 *      Check whether a FileIODescriptor is valid.
 *
 * Results:
 *      True if valid.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
FileIO_IsValid(const FileIODescriptor *fd)      // IN
{
   ASSERT(fd);
   return fd->posix != -1;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_CreateFDPosix --
 *
 *      This function is for specific needs: for example, when you need
 *      to create a FileIODescriptor from an already open fd. Use only
 *      FileIO_* library functions on the FileIODescriptor from that point on.
 *
 *      Because FileIODescriptor struct is different on two platforms,
 *      this function is the only one in the file library that's
 *      platform-specific.
 *
 * Results:
 *      FileIODescriptor
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIODescriptor
FileIO_CreateFDPosix(int posix,  // IN: UNIX file descriptor
                     int flags)  // IN: UNIX access flags
{
   FileIODescriptor fd;
   FileIO_Invalidate(&fd);

#if defined(VMX86_STATS)
   STATS_USER_INIT_MODULE_ONCE();
   fd.stats = STATS_USER_INIT_INST("Created");
#endif

   if (flags & O_RDWR) {
      fd.flags |= (FILEIO_OPEN_ACCESS_READ | FILEIO_OPEN_ACCESS_WRITE);
   } else if (flags & O_WRONLY) {
      fd.flags |= FILEIO_OPEN_ACCESS_WRITE;
   } else if (flags & O_RDONLY) {
      fd.flags |= FILEIO_OPEN_ACCESS_READ;
   }

#if defined(O_SYNC) // Not available in FreeBSD tools build
   if (flags & O_SYNC) {
      fd.flags |= FILEIO_OPEN_SYNC;
   }
#endif

   fd.posix = posix;
   return fd;
}


#if !defined(N_PLAT_NLM)
/*
 *----------------------------------------------------------------------
 *
 * FileIO_GetVolumeSectorSize --
 *
 *      Get sector size of underlying volume.
 *
 * Results:
 *      Always FALSE, there does not seem to be a way to query sectorSize
 *      from filename. XXX.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
FileIO_GetVolumeSectorSize(const char *name,    // IN
                           uint32 *sectorSize)  // OUT
{
   *sectorSize = 0;

   return FALSE;
}
#endif /* !defined(N_PLAT_NLM) */

#if defined(__APPLE__)
/*
 *----------------------------------------------------------------------
 *
 * ProxySendResults --
 *
 *      Send the results of a open from the proxy.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
ProxySendResults(int sock_fd,    // IN:
                 int send_fd,    // IN:
                 int send_errno) // IN:
{
   struct iovec iov;
   struct msghdr msg;

   iov.iov_base = &send_errno;
   iov.iov_len = sizeof send_errno;

   if (send_fd == -1) {
      msg.msg_control = NULL;
      msg.msg_controllen = 0;
   } else {
      struct cmsghdr *cmsg;
      char cmsgBuf[CMSG_SPACE(sizeof send_fd)];

      msg.msg_control = cmsgBuf;
      msg.msg_controllen = sizeof cmsgBuf;

      cmsg = CMSG_FIRSTHDR(&msg);

      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_len = CMSG_LEN(sizeof send_fd);
      cmsg->cmsg_type = SCM_RIGHTS;

      (*(int *) CMSG_DATA(cmsg)) = send_fd;

      msg.msg_controllen = cmsg->cmsg_len;
   }

   msg.msg_name = NULL;
   msg.msg_namelen = 0;
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   msg.msg_flags = 0;

   sendmsg(sock_fd, &msg, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * ProxyReceiveResults --
 *
 *      Receive the results of an open from the proxy.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static void
ProxyReceiveResults(int sock_fd,     // IN:
                    int *recv_fd,    // OUT:
                    int *recv_errno) // OUT:
{
   int err;
   struct iovec iov;
   struct msghdr msg;
   struct cmsghdr *cmsg;
   uint8_t cmsgBuf[CMSG_SPACE(sizeof(int))];

   iov.iov_base = recv_errno;
   iov.iov_len = sizeof *recv_errno;

   msg.msg_control = cmsgBuf;
   msg.msg_controllen = sizeof cmsgBuf;
   msg.msg_name = NULL;
   msg.msg_namelen = 0;
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;

   err = recvmsg(sock_fd, &msg, 0);

   if (err <= 0) {
      *recv_fd = -1;
      *recv_errno = (err == 0) ? EIO : errno;

      return;
   }

   if (msg.msg_controllen == 0) {
      *recv_fd = -1;
   } else {
      cmsg = CMSG_FIRSTHDR(&msg); 

      if ((cmsg->cmsg_level == SOL_SOCKET) &&
          (cmsg->cmsg_type == SCM_RIGHTS)) {
         *recv_fd = *((int *) CMSG_DATA(cmsg));
      } else {
         *recv_fd = -1;
         *recv_errno = EIO;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * ProxyOpen --
 *
 *      Open a file via a proxy.
 *
 * Results:
 *      -1 on error
 *      >= 0 on success
 *
 * Side effects:
 *      errno is set
 *
 *----------------------------------------------------------------------
 */

static int
ProxyOpen(const char *path, // IN:
          int flags,        // IN:
          mode_t mode)      // IN:
{
   int err;
   pid_t pid;
   int fds[2];
   int proxyFD;

   int saveErrno = 0;

   err = socketpair(AF_UNIX, SOCK_DGRAM, 0, fds);
   if (err == -1) {
      errno = ENOMEM; // Out of resources...
      return err;
   }

   pid = fork();
   if (pid == -1) {
      proxyFD = -1;
      saveErrno = ENOMEM; // Out of resources...
      goto bail;
   }

   if (pid == 0) { /* child:  use fd[0] */
      proxyFD = open(path, flags, mode);

      ProxySendResults(fds[0], proxyFD, errno);

      _exit(0);
   } else {        /* parent: use fd[1] */
      ProxyReceiveResults(fds[1], &proxyFD, &saveErrno);

      waitpid(pid, &err, 0);
   }

bail:

   close(fds[0]);
   close(fds[1]);

   errno = saveErrno;

   return proxyFD;
}


/*
 *----------------------------------------------------------------------
 *
 * ProxyUse --
 *
 *      Determine is the open proxy is to be used.
 *
 * Results:
 *	0	Success, useProxy is set
 *	> 0	Failure, errno value and useProxy is not set
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

static int
ProxyUse(const char *filePath, Bool *useProxy)
{
   char *p = NULL;
   char *testPath = NULL;
   struct statfs sfbuf;
   struct stat statbuf;

   /*
    * If the file to be opened exists and is a symbolic link use the
    * proxy... this is a rare case and the work to fully resolve this
    * doesn't seem to be worth it.
    */

   if ((lstat(filePath, &statbuf) == 0) && S_ISLNK(statbuf.st_mode)) {
      *useProxy = TRUE;
      return 0;
   }

   /*
    * Construct the testPath - the path to the directory that contains
    * the filePath.
    */

   testPath = malloc(strlen(filePath) + 2);
   if (testPath == NULL) {
      return ENOMEM;  // Really out of memory...
   }

   strcpy(testPath, filePath);

   p = strrchr(testPath, '/');
   if (p == NULL) {
      p = testPath;
   } else {
      p++;
   }

   strcpy(p, ".");

   /*
    * Attempt to obtain information abour the testPath (directory
    * containing filePath).
    */

   if (statfs(testPath, &sfbuf) == 0) {
      /*
       * The testPath exists; determine proxy usage explicitely.
       */

      *useProxy = strcmp(sfbuf.f_fstypename, "nfs") == 0 ?  TRUE : FALSE;
   } else {
      /*
       * A statfs error of some sort; Err on the side of caution.
       */

      *useProxy = TRUE;
   }

   free(testPath);

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * PosixFileOpener --
 *
 *      Open a file. Use a proxy when creating a file or on NFS.
 *
 *	Why a proxy? The MacOS X 10.4.* NFS client interacts with our
 *	use of settid() and doesn't send the proper credentials on opens.
 *	This leads to files being written without error but containing no
 *	data. The proxy avoids all of this unhappiness.
 *
 * Results:
 *      -1 on error
 *      >= 0 on success
 *
 * Side effects:
 *      errno is set
 *
 *----------------------------------------------------------------------
 */

int
PosixFileOpener(const char *filePath, // IN:
                int flags,            // IN:
                mode_t mode)          // IN:
{
   Bool useProxy;

   if ((flags & O_ACCMODE) || (flags & O_CREAT)) {
      int err;

      /*
       * Open for write and/or O_CREAT. Determine proxy usage.
       */

      err = ProxyUse(filePath, &useProxy);
      if (err != 0) {
         errno = err;
         return -1;
      }
   } else {
      /*
       * No write access, no need for a proxy.
       */

      useProxy = FALSE;
   }

   return useProxy ? ProxyOpen(filePath, flags, mode) :
                     open(filePath, flags, mode);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Open --
 *
 *      Open a file
 *
 * Results:
 *      FILEIO_SUCCESS on success: 'file' is set
 *      FILEIO_OPEN_ERROR_EXIST if the file already exists
 *      FILEIO_FILE_NOT_FOUND if the file is not present
 *      FILEIO_ERROR for other errors
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Open(FileIODescriptor *file,     // OUT
            const char *name,           // IN
            int access,                 // IN
            FileIOOpenAction action)    // IN
{
   Bool su = FALSE;
   int fd = -1;
   int flags = 0;
   int error;
   FileIOResult ret;

#if defined(VMX86_STATS)
   {
      char *tmp;
      File_SplitName(name, NULL, NULL, &tmp);
      STATS_USER_INIT_MODULE_ONCE();
      file->stats = STATS_USER_INIT_INST(tmp);
      free(tmp);
   }
#endif

   ASSERT(!FileIO_IsValid(file));
   ASSERT(file->lockToken == NULL);
   ASSERT(FILEIO_ERROR_LAST < 16); /* See comment in fileIO.h */

#if !defined(__FreeBSD__) && !defined(sun) && !defined(N_PLAT_NLM)
   /*
    * If FILEIO_OPEN_EXCLUSIVE_LOCK or FILEIO_OPEN_MULTIWRITER_LOCK or
    * (FILEIO_OPEN_ACCESS_READ | FILEIO_OPEN_LOCKED) are passed, and we are
    * on VMFS, then pass in special flags to get exclusive, multiwriter, or
    * cross-host read-only mode.  The first if statement is to avoid calling
    * File_OnVMFS() unless really necessary.
    *
    * If the above conditions are met FILEIO_OPEN_LOCKED, is filtered out --
    * vmfs will be handling the locking, so there is no need to create 
    * lockfiles.
    */
   if ((access & (FILEIO_OPEN_EXCLUSIVE_LOCK |
		  FILEIO_OPEN_MULTIWRITER_LOCK)) != 0 ||
       (access & (FILEIO_OPEN_ACCESS_READ | FILEIO_OPEN_ACCESS_WRITE |
		  FILEIO_OPEN_LOCKED)) ==
       (FILEIO_OPEN_ACCESS_READ | FILEIO_OPEN_LOCKED)) {
      if (File_OnVMFS(name)) {
         access &= ~FILEIO_OPEN_LOCKED;
	 if ((access & FILEIO_OPEN_MULTIWRITER_LOCK) != 0) {
	    flags |= O_MULTIWRITER_LOCK;
	 } else {
	    flags |= O_EXCLUSIVE_LOCK;
	 }
      }
   }
#endif

   FileIO_Init(file, name);
   ret = FileIO_Lock(file, access);
   if (ret != FILEIO_SUCCESS) {
      goto error;
   }

   if ((access & (FILEIO_OPEN_ACCESS_READ | FILEIO_OPEN_ACCESS_WRITE)) ==
       (FILEIO_OPEN_ACCESS_READ | FILEIO_OPEN_ACCESS_WRITE)) {
      flags |= O_RDWR;
   } else if (access & FILEIO_OPEN_ACCESS_WRITE) {
      flags |= O_WRONLY;
   } else if (access & FILEIO_OPEN_ACCESS_READ) {
      flags |= O_RDONLY;
   }

   if (access & FILEIO_OPEN_EXCLUSIVE_READ &&
       access & FILEIO_OPEN_EXCLUSIVE_WRITE) {
      flags |= O_EXCL;
   }

   if (access & FILEIO_OPEN_UNBUFFERED) {
#if defined(FILEIO_SUPPORT_ODIRECT)
      flags |= O_DIRECT;
#elif !defined(__APPLE__) // Mac hosts need this access flag after opening.
      access &= ~FILEIO_OPEN_UNBUFFERED;
      LOG_ONCE((LGPFX" %s reverting to buffered IO on %s.\n",
                __FUNCTION__, name));
#endif
   }

   if (access & FILEIO_OPEN_NONBLOCK) {
      flags |= O_NONBLOCK;
   }

   file->flags = access;

   if (access & FILEIO_OPEN_PRIVILEGED) {
      su = IsSuperUser();
      SuperUser(TRUE);
   }

   fd = PosixFileOpener(name, flags
#if defined(linux) && !defined(N_PLAT_NLM)
                        | ((access & FILEIO_OPEN_SYNC) ? O_SYNC : 0)
#endif
                        | FileIO_OpenActions[action],
                        S_IRUSR | S_IWUSR);

   error = errno;

   if (access & FILEIO_OPEN_PRIVILEGED) {
      SuperUser(su);
   }

   errno = error;

   if (fd == -1) {
      ret = FileIOErrno2Result(errno);
      goto error;
   }

#if defined(__APPLE__)
   if (access & (FILEIO_OPEN_UNBUFFERED|FILEIO_OPEN_SYNC)) {
      error = fcntl(fd, F_NOCACHE, 1);
      if (error == -1) {
         ret = FileIOErrno2Result(errno);
         goto error;
      }
   }
#endif

   if (access & FILEIO_OPEN_DELETE_ASAP) {
      /*
       * Remove the name from the name space. The file remains laid out on the
       * disk and accessible through the file descriptor until it is closed.
       */
      if (unlink(name) == -1) {
         ret = FileIOErrno2Result(errno);
         goto error;
      }
   }

   file->posix = fd;

   FileIO_StatsInit(file);

   return FILEIO_SUCCESS;

error:
   error = errno;
   if (fd != -1) {
      close(fd);
   }
   FileIO_Unlock(file);
   FileIO_Cleanup(file);
   FileIO_Invalidate(file);
   errno = error;

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Seek --
 *
 *      Change the current position in a file
 *
 * Results:
 *      On success: the new current position in bytes from the beginning of the
 *                file
 *      On failure: -1
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

#if defined(linux) && !defined(GLIBC_VERSION_21) && !defined(N_PLAT_NLM) 
/*
 * Implements the system call for non-LFS glibc
 */
static INLINE int
_llseek(unsigned int fd,
        unsigned long offset_high,
        unsigned long offset_low,
        loff_t * result,
        unsigned int whence)
{
   return syscall(SYS__llseek, fd, offset_high, offset_low, result, whence);
}
#endif

uint64
FileIO_Seek(const FileIODescriptor *file, // IN
            int64 distance,               // IN
            FileIOSeekOrigin origin)      // IN
{
#if defined(linux) && !defined(GLIBC_VERSION_21) && !defined(N_PLAT_NLM) 

   loff_t res;
   if (_llseek(file->posix, distance >> 32, distance & 0xFFFFFFFF,
               &res, FileIO_SeekOrigins[origin]) == -1) {
      res = -1;
   }
   return res;

#else // either not linux or linux w/ LFS glibc

    return lseek(file->posix, distance, FileIO_SeekOrigins[origin]);

#endif
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Write --
 *
 *      Write to a file
 *
 * Results:
 *      FILEIO_SUCCESS on success: '*actual_count' = 'requested' bytes have
 *       been written
 *      FILEIO_WRITE_ERROR_FBIG for the attempt to write file that exceeds
 *       maximum file size
 *      FILEIO_WRITE_ERROR_NOSPC when the device containing the file has no
 *       room for the data
 *      FILEIO_WRITE_ERROR_DQUOT for attempts to write file that exceeds
 *       user's disk quota 
 *      FILEIO_ERROR for other errors: only '*actual_count' bytes have been
 *       written for sure
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Write(FileIODescriptor *fd,      // IN
             const void *bufIn,         // IN
             size_t requested,          // IN
             size_t *actual)            // OUT
{
   const uint8 *buf = (const uint8 *)bufIn;
   size_t initial_requested;

   STAT_INST_INC(fd->stats, NumWrites);
   STAT_INST_INC_BY(fd->stats, BytesWritten, requested);
   STATS_ONLY({
      fd->writeIn++;
      fd->bytesWritten += requested;
   })

   ASSERT_NOT_IMPLEMENTED(requested < 0x80000000);

   initial_requested = requested;
   while (requested > 0) {
      ssize_t res;

      STATS_ONLY(fd->writeDirect++;)
      res = write(fd->posix, buf, requested);

      if (res == -1) {
         int error = errno;
         FileIOResult fret;

         switch (error) {
         case EINTR:
            NOT_TESTED();
            continue;

         case ENOSPC:
            fret = FILEIO_WRITE_ERROR_NOSPC;
            break;

         case EFBIG:
            fret = FILEIO_WRITE_ERROR_FBIG;
            break;

#ifdef EDQUOT
         case EDQUOT:
            fret = FILEIO_WRITE_ERROR_DQUOT;
            break;
#endif
     
         default:
            fret = FILEIO_ERROR;
         }

         Log(LGPFX" %s failed %d.\n", __FUNCTION__, error);
            
         if (actual) {
            *actual = initial_requested - requested;
         }

         return fret;
      }

      buf += res;
      requested -= res;
   }

   if (actual) {
      *actual = initial_requested;
   }
   return FILEIO_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Read --
 *
 *      Read from a file
 *
 * Results:
 *      FILEIO_SUCCESS on success: '*actual_count' = 'requested' bytes have
 *       been written
 *      FILEIO_READ_ERROR_EOF if the end of the file was reached: only
 *       '*actual_count' bytes have been read for sure
 *      FILEIO_ERROR for other errors: only '*actual_count' bytes have been
 *       read for sure
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Read(FileIODescriptor *fd,       // IN
            void *bufIn,                // OUT
            size_t requested,           // IN
            size_t *actual)             // OUT
{
   uint8 *buf = (uint8 *)bufIn;
   size_t initial_requested;

   STAT_INST_INC(fd->stats, NumReads);
   STAT_INST_INC_BY(fd->stats, BytesRead, requested);
   STATS_ONLY({
      fd->readIn++;
      fd->bytesRead += requested;
   })

   ASSERT_NOT_IMPLEMENTED(requested < 0x80000000);

   initial_requested = requested;
   while (requested > 0) {
      ssize_t res;

      STATS_ONLY(fd->readDirect++;)
      res = read(fd->posix, buf, requested);
      if (res == -1) {
         if (errno == EINTR) {
            NOT_TESTED();
            continue;
         }

         if (actual) {
            *actual = initial_requested - requested;
         }
         return FILEIO_ERROR;
      }

      if (res == 0) {
         if (actual) {
            *actual = initial_requested - requested;
         }
         return FILEIO_READ_ERROR_EOF;
      }

      buf += res;
      requested -= res;
   }

   if (actual) {
      *actual = initial_requested;
   }
   return FILEIO_SUCCESS;
}


#if !defined(N_PLAT_NLM)
/*
 *----------------------------------------------------------------------
 *
 * FileIO_Truncate --
 *
 *      Truncates file to a given length
 *
 * Results:
 *      Bool - TRUE on success, FALSE on failure
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Bool
FileIO_Truncate(FileIODescriptor *file, // IN
                uint64 newLength)       // IN
{
   return ftruncate(file->posix, newLength) == 0;
}
#endif /* !defined(N_PLAT_NLM) */


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Close --
 *
 *      Close a file
 *
 * Results:
 *      On success: 0
 *      On failure: non-zero
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
FileIO_Close(FileIODescriptor *file) // IN
{
   int retval = close(file->posix);

   FileIO_StatsExit(file);

   /* Unlock the file if it was locked */
   FileIO_Unlock(file);
   FileIO_Cleanup(file);
   FileIO_Invalidate(file);
   return retval;
}


#if !defined(N_PLAT_NLM)
/*
 *----------------------------------------------------------------------
 *
 * FileIO_Sync --
 *
 *      Synchronize the disk state of a file with its memory state
 *
 * Results:
 *      On success: 0
 *      On failure: -1
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
FileIO_Sync(const FileIODescriptor *file) // IN
{
   return fsync(file->posix);
}

/*
 * readv & writev are not available in the FreeBSD or Solaris Tools builds
 */
#if !defined(VMX86_TOOLS) || (!defined(__FreeBSD__) && !defined(sun))


/*
 *-----------------------------------------------------------------------------
 *
 * FileIOCoalesce --
 *
 *      Linux 2.2 does a fairly braindead thing with ioVec's.  It simply issues
 *      reads and writes internal to the kernel in serial
 *      (linux/fs/read_write.c:do_readv_writev()).  We optimize here for the
 *      case of many small chunks.  The cost of the extra copy in this case
 *      is made up for by the decreased number of separate I/Os the kernel
 *      issues internally. Note that linux 2.4 seems to be smarter with respect
 *      to this problem.
 *
 * Results:
 *      Bool - Whether or not coalescing was done.  If it was done,
 *             FileIODecoalesce *MUST* be called.
 *
 * Side effects:
 *      FileIOCoalesce will malloc *outVec if coalescing is performed
 *
 *-----------------------------------------------------------------------------
 */

static Bool
FileIOCoalesce(struct iovec *inVec,     // IN:  Vector to coalesce from
               int inCount,             // IN:  count for inVec
               size_t inTotalSize,      // IN:  totalSize (bytes) in inVec
               Bool isWrite,            // IN:  coalesce for writing (or reading)
               Bool forceCoalesce,      // IN:  if TRUE always coalesce
               struct iovec *outVec)    // OUT: Coalesced (1-entry) iovec
{
   uint8 *cBuf;

   ASSERT(inVec);
   ASSERT(outVec);

   FileIO_OptionalSafeInitialize();

   /* simple case: no need to coalesce */
   if (inCount == 1) {
      return FALSE;
   }

   /*
    * Only coalesce when the number of entries is above our count threshold
    * and the average size of an entry is less than our size threshold
    */
   if (!forceCoalesce &&
       (!filePosixOptions.enabled ||
       inCount <= filePosixOptions.countThreshold ||
       inTotalSize / inCount >= filePosixOptions.sizeThreshold)) {
      return FALSE;
   }

   // XXX: Wouldn't it be nice if we could log from here!
   //LOG(5, ("FILE: Coalescing %s of %d elements and %d size\n",
   //        isWrite ? "write" : "read", inCount, inTotalSize));
   cBuf = malloc(sizeof(uint8) * inTotalSize);
   ASSERT_MEM_ALLOC(cBuf);

  if (isWrite) {
      IOV_WriteIovToBuf(inVec, inCount, cBuf, inTotalSize);
   }

   outVec->iov_base = cBuf;
   outVec->iov_len = inTotalSize;
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileIODecoalesce --
 *
 *      Inverse of the coalesce optimization.  For writes, its a NOOP, but
 *      for reads, it copies the data back into the original buffer.
 *      It also frees the memory allocated by FileIOCoalesce.
 *
 * Results:
 *      void
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
FileIODecoalesce(struct iovec *coVec,   // IN: Coalesced (1-entry) vector
                 struct iovec *origVec, // IN: Original vector
                 int origVecCount,      // IN: count for origVec
                 size_t actualSize,     // IN: # bytes to transfer back to origVec
                 Bool isWrite)          // IN: decoalesce for writing (or reading)
{
   ASSERT(coVec);
   ASSERT(origVec);

   ASSERT(actualSize <= coVec->iov_len);
   ASSERT_NOT_TESTED(actualSize == coVec->iov_len);

   if (!isWrite) {
      IOV_WriteBufToIov(coVec->iov_base, actualSize, origVec, origVecCount);
   }

   free(coVec->iov_base);
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Readv --
 *
 *      Wrapper for readv. In linux, we can issue a readv directly.
 *      But the readv is not atomic, i.e, the read can succeed
 *      on the first N vectors, and return a positive value in spite
 *      of the fact that there was an error on the N+1st vector. There
 *      is no way to query the exact error that happened. So, we retry
 *      in a loop (for a max of MAX_RWV_RETRIES).
 *      XXX: If we retried MAX_RWV_RETRIES times and gave up, we will
 *      return FILEIO_ERROR even if errno is undefined.
 *
 * Results:
 *      FILEIO_SUCCESS, FILEIO_ERROR, FILEIO_READ_ERROR_EOF
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Readv(FileIODescriptor *fd,      // IN
             struct iovec *v,           // IN
             int numEntries,            // IN
             size_t totalSize,          // IN
             size_t *actual)            // OUT
{
   size_t bytesRead = 0, sum = 0;
   FileIOResult fret = FILEIO_ERROR;
   int nRetries = 0, maxRetries = numEntries;
   struct iovec coV;
   struct iovec *vPtr;
   Bool didCoalesce;
   int numVec;

   didCoalesce = FileIOCoalesce(v, numEntries, totalSize, FALSE, FALSE, &coV);

   STAT_INST_INC(fd->stats, NumReadvs);
   STAT_INST_INC_BY(fd->stats, BytesReadv, totalSize);
   STATS_ONLY({
      fd->readvIn++;
      fd->bytesRead += totalSize;
      if (didCoalesce) {
         fd->numReadCoalesced++;
      }
   })

   ASSERT_NOT_IMPLEMENTED(totalSize < 0x80000000);

   numVec = didCoalesce ? 1 : numEntries;
   vPtr = didCoalesce ? &coV : v;

   while (nRetries < maxRetries) {
      ssize_t retval;
      ASSERT(numVec > 0);
      STATS_ONLY(fd->readvDirect++;)
      retval = readv(fd->posix, vPtr, numVec);
      if (retval == -1) {
         fret = FILEIO_ERROR;
         break;
      }
      bytesRead += retval;
      if (bytesRead == totalSize) {
         fret =  FILEIO_SUCCESS;
         break;
      }
      /*
       * Ambigous case. Stupid Linux. If the bytesRead matches an
       * exact iovector boundary, we need to retry from the next
       * iovec. 2) If it does not match, EOF is the only error possible.
       * NOTE: If Linux Readv implementation changes, this
       * ambiguity handling may need to change.
       * --Ganesh, 08/15/2001.
       */
      if (retval == 0) {
         // Got 0, meaning EOF
         fret = FILEIO_READ_ERROR_EOF;
         break;
      }
      for (; sum <= bytesRead; vPtr++, numVec--) {
         sum += vPtr->iov_len;
         /*
          * In each syscall, we will process atleast one iovec
          * or get an error back. We will therefore retry atmost
          * count times. If multiple iovecs were processed before
          * an error hit, we will retry a lesser number of times.
          */
         nRetries++;
      }
      if (sum > bytesRead) {
         // A partially filled iovec can ONLY mean EOF
         fret = FILEIO_READ_ERROR_EOF;
         break;
      }
   }

   if (didCoalesce) {
      FileIODecoalesce(&coV, v, numEntries, bytesRead, FALSE);
   }

   if (actual) {
      *actual = bytesRead;
   }
   return fret;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Writev --
 *
 *      Wrapper for writev. In linux, we can issue a writev directly.
 *      But the writev is not atomic, i.e, the write can succeed
 *      on the first N vectors, and return a positive value in spite
 *      of the fact that there was an error on the N+1st vector. There
 *      is no way to query the exact error that happened. So, we retry
 *      in a loop (for a max of MAX_RWV_RETRIES).
 *      XXX: If we retried MAX_RWV_RETRIES times and gave up, we will
 *      return FILEIO_ERROR even if errno is undefined.
 *
 * Results:
 *      FILEIO_SUCCESS, FILEIO_ERROR
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Writev(FileIODescriptor *fd,     // IN
              struct iovec *v,          // IN
              int numEntries,           // IN
              size_t totalSize,         // IN
              size_t *actual)           // OUT
{
   size_t bytesWritten = 0, sum = 0;
   FileIOResult fret = FILEIO_ERROR;
   int nRetries = 0, maxRetries = numEntries;
   struct iovec coV;
   struct iovec *vPtr;
   Bool didCoalesce;
   int numVec;

   didCoalesce = FileIOCoalesce(v, numEntries, totalSize, TRUE, FALSE, &coV);

   STAT_INST_INC(fd->stats, NumWritevs);
   STAT_INST_INC_BY(fd->stats, BytesWritev, totalSize);
   STATS_ONLY({
      fd->writevIn++;
      fd->bytesWritten += totalSize;
      if (didCoalesce) {
         fd->numWriteCoalesced++;
      }
   })

   ASSERT_NOT_IMPLEMENTED(totalSize < 0x80000000);

   numVec = didCoalesce ? 1 : numEntries;
   vPtr = didCoalesce ? &coV : v;

   while (nRetries < maxRetries) {
      ssize_t retval;
      ASSERT(numVec > 0);
      STATS_ONLY(fd->writevDirect++;)
      retval = writev(fd->posix, vPtr, numVec);
      if (retval == -1) {
         fret = FILEIO_ERROR;
         break;
      }

      bytesWritten += retval;
      if (bytesWritten == totalSize) {
         fret =  FILEIO_SUCCESS;
         break;
      }
      NOT_TESTED();
      for (; sum <= bytesWritten; vPtr++, numVec--) {
         sum += vPtr->iov_len;
         nRetries++;
      }
      /*
       * writev only seems to produce a partial iovec when the disk is
       * out of space.  Just call it an error. --probin
       */
      if (sum != bytesWritten) {
         fret = FILEIO_ERROR;
         break;
      }
   }

   if (didCoalesce) {
      FileIODecoalesce(&coV, v, numEntries, bytesWritten, TRUE);
   }

   if (actual) {
      *actual = bytesWritten;
   }
   return fret;
}


#if defined(GLIBC_VERSION_21) || defined(__APPLE__)

/*
 *----------------------------------------------------------------------
 *
 * FileIO_Preadv --
 *
 *      Implementation of vector pread. The incoming vectors are
 *      coalesced to a single buffer to issue only one pread()
 *      system call which reads from a specified offset. The
 *      vectors are then decoalesced before return.
 *
 * Results:
 *      FILEIO_SUCCESS, FILEIO_ERROR
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Preadv(FileIODescriptor *fd,    // IN: File descriptor
              struct iovec *entries,   // IN: Vector to read into
              int numEntries,          // IN: Number of vector entries
              uint64 offset,           // IN: Offset to start reading
              size_t totalSize)        // IN: totalSize (bytes) in entries
{
   size_t sum = 0;
   struct iovec *vPtr;
   struct iovec coV;
   int count;
   uint64 fileOffset;
   FileIOResult fret = FILEIO_ERROR;
   Bool didCoalesce;

   ASSERT(fd);
   ASSERT(entries);
   ASSERT(!(fd->flags & FILEIO_ASYNCHRONOUS));
   ASSERT_NOT_IMPLEMENTED(totalSize < 0x80000000);

   didCoalesce = FileIOCoalesce(entries, numEntries, totalSize, FALSE,
                                TRUE /* force coalescing */,
                                &coV);

   count = didCoalesce ? 1 : numEntries;
   vPtr = didCoalesce ? &coV : entries;

   STAT_INST_INC(fd->stats, NumPreadvs);
   STAT_INST_INC_BY(fd->stats, BytesPreadv, totalSize);
   STATS_ONLY({
      fd->preadvIn++;
      fd->bytesRead += totalSize;
      if (didCoalesce) {
         fd->numReadCoalesced++;
      }
   })

   fileOffset = offset;
   while (count > 0) {
      size_t leftToRead = vPtr->iov_len;
      uint8 *buf = (uint8 *)vPtr->iov_base;
      STATS_ONLY(fd->preadDirect++;)

      while (leftToRead > 0) {
         ssize_t retval = pread(fd->posix, buf, leftToRead, fileOffset);

         if (retval == -1) {
            if (errno == EINTR || errno == EAGAIN) {
               LOG_ONCE((LGPFX" %s got %s.  Retrying\n",
                         __FUNCTION__, errno == EINTR ? "EINTR" : "EAGAIN"));
               NOT_TESTED_ONCE();
               continue;
            }

            goto exit;
         }

         if (retval == 0) {
            fret = FILEIO_READ_ERROR_EOF;
            goto exit;
         }

         buf += retval;
         leftToRead -= retval;
         sum += retval;
         fileOffset += retval;
      }

      count--;
      vPtr++;
   }

exit:
   if (sum == totalSize) {
      fret = FILEIO_SUCCESS;
   }

   if (didCoalesce) {
      FileIODecoalesce(&coV, entries, numEntries, sum, FALSE);
   }

   return fret;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Pwritev --
 *
 *      Implementation of vector pwrite. The incoming vectors are
 *      coalesced to a single buffer to issue only one pwrite()
 *      system call which writes from a specified offset. The
 *      vectors are then decoalesced before return.
 *
 * Results:
 *      FILEIO_SUCCESS, FILEIO_ERROR
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Pwritev(FileIODescriptor *fd,   // IN: File descriptor
               struct iovec *entries,  // IN: Vector to write from
               int numEntries,         // IN: Number of vector entries
               uint64 offset,          // IN: Offset to start writing
               size_t totalSize)       // IN: Total size (bytes) in entries
{
   struct iovec coV;
   Bool didCoalesce;
   struct iovec *vPtr;
   int count;
   size_t sum = 0;
   uint64 fileOffset;
   FileIOResult fret = FILEIO_ERROR;

   ASSERT(fd);
   ASSERT(entries);
   ASSERT(!(fd->flags & FILEIO_ASYNCHRONOUS));
   ASSERT_NOT_IMPLEMENTED(totalSize < 0x80000000);

   didCoalesce = FileIOCoalesce(entries, numEntries, totalSize, TRUE,
                                TRUE /* force coalescing */,
                                &coV);

   count = didCoalesce ? 1 : numEntries;
   vPtr = didCoalesce ? &coV : entries;

   STAT_INST_INC(fd->stats, NumPwritevs);
   STAT_INST_INC_BY(fd->stats, BytesPwritev, totalSize);
   STATS_ONLY({
      fd->pwritevIn++;
      fd->bytesWritten += totalSize;
      if (didCoalesce) {
         fd->numWriteCoalesced++;
      }
   })

   fileOffset = offset;
   while (count > 0) {
      size_t leftToWrite = vPtr->iov_len;
      uint8 *buf = (uint8 *)vPtr->iov_base;
      STATS_ONLY(fd->pwriteDirect++;)

      while (leftToWrite > 0) {
         ssize_t retval = pwrite(fd->posix, buf, leftToWrite, fileOffset);

         if (retval == -1) {
            if (errno == EINTR || errno == EAGAIN) {
               LOG_ONCE((LGPFX" %s got %s.  Retrying\n",
                         __FUNCTION__, errno == EINTR ? "EINTR" : "EAGAIN"));
               NOT_TESTED_ONCE();
               continue;
            }

            goto exit;
         }

         if (retval < leftToWrite) {
            LOG_ONCE((LGPFX" %s wrote %"FMTSZ"d out of %"FMTSZ"u bytes.\n",
                      __FUNCTION__, retval, leftToWrite));
         }

         buf += retval;
         leftToWrite -= retval;
         sum += retval;
         fileOffset += retval;
      }

      count--;
      vPtr++;
   }

exit:
   if (sum == totalSize) {
      fret = FILEIO_SUCCESS;
   }

   if (didCoalesce) {
      FileIODecoalesce(&coV, entries, numEntries, sum, TRUE);
   }

   return fret;
}
#endif /* defined(GLIBC_VERSION_21) || defined(__APPLE__) */
#endif /* !defined(VMX86_TOOLS) || !(defined(FreeBSD) || defined(sun)) */
#endif /* !defined(N_PLAT_NLM) */

/*
 *----------------------------------------------------------------------
 *
 * FileIO_GetSize --
 *
 *      Get size of file.
 *
 * Results:
 *      Size of file or -1.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int64
FileIO_GetSize(const FileIODescriptor *fd)      // IN
{
   struct stat statBuf;
   if (fstat(fd->posix, &statBuf) == -1) {
      return -1;
   }
   return statBuf.st_size;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_GetSizeByPath --
 *
 *      Get size of a file specified by path. 
 *
 * Results:
 *      Size of file or -1.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int64
FileIO_GetSizeByPath(const char *name)      // IN
{
   struct stat statBuf;
   if (stat(name, &statBuf) == -1) {
      return -1;
   } 
   return statBuf.st_size;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_Access --
 *
 *      Wrapper for access syscall. We return FILEIO_SUCCESS if the file
 *      is accessible with the specified mode. If not, we will return
 *      FILEIO_ERROR.
 *
 * Results:
 *      FILEIO_SUCCESS or FILEIO_ERROR.
 *
 * Side effects:
 *      Hah! Changes errno. Maybe.
 *
 *----------------------------------------------------------------------
 */

FileIOResult
FileIO_Access(const char *name,         // IN:
              int accessMode)           // IN:
{
   int mode = 0;

   if (accessMode & FILEIO_ACCESS_READ) {
      mode |= R_OK;
   }
   if (accessMode & FILEIO_ACCESS_WRITE) {
      mode |= W_OK;
   }
   if (accessMode & FILEIO_ACCESS_EXEC) {
      mode |= X_OK;
   }
   if (accessMode & FILEIO_ACCESS_EXISTS) {
      mode |= F_OK;
   }

   return access(name, mode) == -1 ? FILEIO_ERROR : FILEIO_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_GetFlags --
 *
 *      Accessor for fd->flags;
 *
 * Results:
 *      fd->flags
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

uint32
FileIO_GetFlags(FileIODescriptor *fd)   // IN
{
   ASSERT(FileIO_IsValid(fd));
   return fd->flags;
}


/*
 *----------------------------------------------------------------------
 *
 * FileIO_SupportsFileSize --
 *
 *      Test whether underlying filesystem supports specified file size.
 *
 * Results:
 *      Return TRUE if such file size is supported, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
FileIO_SupportsFileSize(const FileIODescriptor *fd,            // IN
                        uint64                  requestedSize) // IN
{
#if defined(N_PLAT_NLM)
   /* API we use on NetWare cannot go over 2GB - 1 although filesystem could. */
   return requestedSize <= 0x7FFFFFFF;
#elif defined(linux)
   /*
    * Linux makes test on seek(), so we can do simple non-intrusive test.
    * Verified to work on 2.2.x, 2.4.x and 2.6.x, with ext2, ext3, smbfs, 
    * cifs, nfs and ncpfs.  Always got some reasonable value.
    */
   Bool supported = FALSE;
   uint64 oldPos;

   ASSERT(FileIO_IsValid(fd));

   oldPos = FileIO_Seek(fd, 0, FILEIO_SEEK_CURRENT);
   if (oldPos != (uint64)-1) {
      uint64 newPos;

      if (FileIO_Seek(fd, requestedSize, FILEIO_SEEK_BEGIN) == requestedSize) {
         supported = TRUE;
      }
      newPos = FileIO_Seek(fd, oldPos, FILEIO_SEEK_BEGIN);
      ASSERT_NOT_IMPLEMENTED(oldPos == newPos);
   }
   return supported;
#elif defined(__APPLE__)
   struct statfs buf;

   if (fstatfs(fd->posix, &buf) == -1) {
      Log(LGPFX" %s fstatfs failure: %s\n", __FUNCTION__, strerror(errno));
      /* Be optimistic despite failure */
      return TRUE;
   }

   /* Check for FAT and UFS file systems */
   if ((Str_Strcasecmp(buf.f_fstypename, "msdos") == 0) ||
       (Str_Strcasecmp(buf.f_fstypename, "ufs") == 0)) {
      /* 4 GB limit */
      return requestedSize > CONST64U(0xFFFFFFFF) ? FALSE : TRUE;
   }

   /* Be optimistic... */
   return TRUE;
#else
   /* Be optimistic on FreeBSD and Solaris... */
   return TRUE;
#endif
}

