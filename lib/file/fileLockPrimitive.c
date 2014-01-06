/* ************************************************************************
 * Copyright 2007 VMware, Inc.  All rights reserved. 
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
 * fileLockPrimitive.c --
 *
 *      Portable file locking via Lamport's Bakery algorithm.
 *
 * Makes use of the following POSIX routines:
 *
 *	stat
 *	open (O_CREAT)
 *	close
 *	unlink
 *	mkdir
 *	rmdir
 *	rename - this can be removed, if desired
 *
 * by not making use of any file system specific attribute this method
 * should be portable to all file systems, local and remote, without
 * modification.
 *
 * This implementation does rely upon the rmdir to fail if the directory
 * contains any files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <direct.h>
typedef int mode_t;
#define LOCKFILE_CREATION_PERMISSIONS _S_IREAD | _S_IWRITE
#define LOCKFILE_CREATION_FLAGS _O_CREAT | _O_TRUNC | _O_RDWR
#define open _open
#define close _close
#define unlink _unlink
#define rmdir _rmdir
#define strtok_r strtok_s
#else
#define LOCKFILE_CREATION_PERMISSIONS 0644
#define LOCKFILE_CREATION_FLAGS O_CREAT | O_TRUNC | O_RDWR
#include <unistd.h>
#include <sys/param.h>
#endif
#include "vmware.h"
#include "hostinfo.h"
#include "util.h"
#include "log.h"
#include "str.h"
#include "file.h"
#include "fileLock.h"
#include "fileInt.h"

#define LOGLEVEL_MODULE main
#include "loglevel_user.h"

#define	LOCK_SHARED	"S"
#define	LOCK_EXCLUSIVE	"X"
#define FILELOCK_PROGRESS_DEARTH 8000 // Dearth of progress time in msec
#define FILELOCK_PROGRESS_SAMPLE 200  // Progress sampling time in msec

static char implicitReadToken;

#if defined(_WIN32) && (_MSC_VER < 1400 || defined(_WIN64))
// hack until SDK is updated; must be removed after that
static char *
strtok_s(char *str,
         const char *delim,
         char **context)
{
   char *p;

   if (str == NULL) {
      str = *context;
   }

   while (*str) {
      if (strchr(delim, *str) == NULL) {
         break;
      }
      str++;
   }

   if (*str == '\0') {
      *context = str;
      return NULL;
   }

   p = str + 1;

   while (*p) {
      if (strchr(delim, *p) != NULL) {
         break;
      }
      p++;
   }

   if (*p == '\0') {
      *context = p;
   } else {
      *p = '\0';
      *context = p + 1;
   }

   return str;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Sleeper --
 *
 *	Have the calling thread sleep "for a while". The duration of the
 *	sleep is determined by the count that is passed in. Checks are
 *	also done for exceeding the maximum wait time.
 *
 * Results:
 *	0	slept
 *	EAGAIN	maximum sleep time exceeded
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
Sleeper(LockValues *myValues, // IN/OUT:
        uint32 *loopCount)    // IN/OUT:
{
   uint32 msecSleepTime;

   if ((myValues->msecMaxWaitTime == FILELOCK_TRYLOCK_WAIT) ||
       ((myValues->msecMaxWaitTime != FILELOCK_INFINITE_WAIT) &&
        (myValues->waitTime > myValues->msecMaxWaitTime))) {
      return EAGAIN;
   }

   if (*loopCount <= 20) {
      /* most locks are "short" */
      msecSleepTime = 100;
      *loopCount += 1;
   } else if (*loopCount < 40) {
      /* lock has been around a while, linear back-off */
      msecSleepTime = 100 * (*loopCount - 19);
      *loopCount += 1;
   } else {
      /* WOW! long time... Set a maximum */
      msecSleepTime = 2000;
   }

   myValues->waitTime += msecSleepTime;

   while (msecSleepTime) {
      uint32 sleepTime = (msecSleepTime > 900) ? 900 : msecSleepTime;

      usleep(1000 * sleepTime);

      msecSleepTime -= sleepTime;
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * RemoveLockingFile --
 *
 *	Remove the specified file.
 *
 * Results:
 *	0	success
 *	not 0	failure (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
RemoveLockingFile(const char *lockDir,  // IN:
                  const char *fileName) // IN;
{
   int  err;
   char path[FILE_MAXPATH];

   Str_Sprintf(path, sizeof path, "%s%s%s", lockDir, DIRSEPS, fileName);

   err = (unlink(path) == 0) ? 0 : errno;

   if (err != 0) {
      if (err == ENOENT) {
         // Not there anymore; locker unlocked or timed out
         err = 0;
      } else {
         Warning(LGPFX" %s of '%s' failed: %s\n", __FUNCTION__,
                 path, strerror(err));
      }
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileLockMemberValues --
 *
 *	Returns the values associated with lock directory file.
 *
 * Results:
 *	0	Valid lock file; values have been returned
 *	!0	Lock file problem (errno); values have not been returned
 *
 * Side effects:
 *      The lock file may be deleted if it is invalid
 *
 *-----------------------------------------------------------------------------
 */

int
FileLockMemberValues(const char *lockDir,      // IN:
                     const char *fileName,     // IN:
                     char *buffer,             // OUT:
                     size_t requiredSize,      // IN:
                     LockValues *memberValues) // OUT:
{
   uint32 i;
   int fd;
   ssize_t len;
   char *lasts;
   char *argv[4];
   int saveErrno;
   char path[FILE_MAXPATH];
   struct stat statbuf;
   int64 fileSize;

   Str_Strcpy(memberValues->memberName, fileName,
              sizeof memberValues->memberName);

   Str_Sprintf(path, sizeof path, "%s%s%s", lockDir, DIRSEPS, fileName);

   fd = PosixFileOpener(path, O_RDONLY, 0);
   if (fd == -1) {
      saveErrno = errno;

      /*
       * A member file may "disappear" if is unlinked due to an unlock
       * immediately after a directory scan but before the scan is processed.
       * Since this is a "normal" thing ENOENT will be surpressed.
       */

      if (errno != ENOENT) {
         Warning(LGPFX" %s open failure on '%s': %s\n", __FUNCTION__,
                 path, strerror(errno));
      }

      return saveErrno;
   }

   /* Attempt to obtain the lock file attributes now that is opened */
   if (fstat(fd, &statbuf) == -1) {
      saveErrno = errno;

      Warning(LGPFX" %s fstat failure on '%s': %s\n", __FUNCTION__, path,
              strerror(errno));

      close(fd);

      return saveErrno;
   }

   /* Complain if the lock file is not the proper size */
   fileSize = statbuf.st_size;
   if (fileSize != requiredSize) {
      Warning(LGPFX" %s file '%s': size %"FMT64"d, required size %"FMTSZ"d\n",
              __FUNCTION__, path, fileSize,  requiredSize);

      close(fd);

      goto corrupt;
   }

   /* Attempt to read the lock file data and validate how much was read. */
   len = read(fd, buffer, requiredSize);
   saveErrno = errno;

   close(fd);

   if (len == -1) {
      Warning(LGPFX" %s read failure on '%s': %s\n",
              __FUNCTION__, path, strerror(saveErrno));

      return saveErrno;
   }

   if (len != requiredSize) {
      Warning(LGPFX" %s read length issue on '%s': %"FMTSZ"d and %"FMTSZ"d\n",
              __FUNCTION__, path, len, requiredSize);

      return EIO;
   }

   /* Extract and validate the lock file data. */
   for (i = 0; i < 4; i++) {
      argv[i] = strtok_r((i == 0) ? buffer : NULL, " ", &lasts);

      if (argv[i] == NULL) {
         Warning(LGPFX" %s mandatory argument %u is missing!\n",
                 __FUNCTION__, i);

         goto corrupt;
      }
   }

   memberValues->payload = strtok_r(NULL, " ", &lasts);

   if (sscanf(argv[2], "%u", &memberValues->lamportNumber) != 1) {
      Warning(LGPFX" %s Lamport number conversion error\n",
              __FUNCTION__);

      goto corrupt;
   }

   if ((strcmp(argv[3], LOCK_SHARED) != 0) &&
       (strcmp(argv[3], LOCK_EXCLUSIVE) != 0)) {
      Warning(LGPFX" %s unknown lock type '%s'\n", __FUNCTION__, argv[3]);

      goto corrupt;
   }

   memberValues->machineID = argv[0];
   memberValues->executionID = argv[1];
   memberValues->lockType = argv[3];

   return 0;

corrupt:
   Warning(LGPFX" %s removing problematic lock file '%s'\n", __FUNCTION__,
           path);

   /* Remove the lock file and behave like it has disappeared */
   return unlink(path) == 0 ? ENOENT : errno;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileLockValidName --
 *
 *	Validate the format of the file name.
 *
 * Results:
 *	TRUE	yes
 *	FALSE	No
 *
 * Side effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

Bool
FileLockValidName(const char *fileName) // IN:
{
   uint32 i;
   char   *p = (char *) fileName;

   if ((*p != 'M') && (*p != 'D') && (*p != 'E')) {
      return FALSE;
   }

   p++;

   for (i = 0; i < 5; i++) {
      if ((*p < '0') || (*p > '9')) {
         return FALSE;
      }

      p++;
   }

   return (strcmp(p, FILELOCK_SUFFIX) == 0) ? TRUE : FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ActivateLockList
 *
 *	Insure a lock list entry exists for the lock directory.
 *
 * Results:
 *	0	success
 *	> 0	error (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
ActivateLockList(const char *dirName,   // IN:
                 LockValues *myValues)  // IN:
{
   ActiveLock   *ptr;

   ASSERT(dirName[0] == 'D');

   // Search the list for a matching entry
   for (ptr = myValues->lockList; ptr != NULL; ptr = ptr->next) {
      if (strcmp(ptr->dirName, dirName) == 0) {
         break;
      }
   }

   // No entry? Attempt to add one.
   if (ptr == NULL) {
      ptr = malloc(sizeof *ptr);

      if (ptr == NULL) {
         return ENOMEM;
      }

      ptr->next = myValues->lockList;
      myValues->lockList = ptr;

      ptr->age = 0;
      Str_Strcpy(ptr->dirName, dirName, sizeof ptr->dirName);
   }

   // Mark the entry (exists)
   ptr->marked = TRUE;

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ScanDirectory --
 *
 *	Call the specified function for each member file found in the
 *	specified directory.
 *
 * Results:
 *	0	success
 *	not 0	failure
 *
 * Side effects:
 *	Anything that this not a valid locking file is deleted.
 *
 *-----------------------------------------------------------------------------
 */

static int
ScanDirectory(const char *lockDir,      // IN:
              int (*func)(              // IN:
                     const char *lockDir,
                     const char *fileName,
                     LockValues *memberValues,
                     LockValues *myValues
                   ),
              LockValues *myValues,    // IN:
              Bool cleanUp)            // IN:
{
   uint32 i;
   int    err;
   int    numEntries;

   char   **fileList = NULL;

   numEntries = File_ListDirectory(lockDir, &fileList);

   if (numEntries == -1) {
      Log(LGPFX" %s: Could not read the directory '%s'.\n",
          __FUNCTION__, lockDir);

      return EDOM;	/* out of my domain */
   }

   // Pass 1: Validate entries and handle any 'D' entries
   for (i = 0, err = 0; i < numEntries; i++) {
      // Remove any non-locking files
      if (!FileLockValidName(fileList[i])) {
         Log(LGPFX" %s discarding file '%s%s%s'; invalid file name.\n",
             __FUNCTION__, lockDir, DIRSEPS, fileList[i]);

         err = RemoveLockingFile(lockDir, fileList[i]);
         if (err != 0) {
            goto bail;
         }

        free(fileList[i]);
        fileList[i] = NULL;

        continue;
      }

      /*
       * Any lockers appear to be entering?
       *
       * This should be rather rare. If a locker dies while entering
       * this will cleaned-up.
       */

      if (*fileList[i] == 'D') {
         if (cleanUp) {
            err = ActivateLockList(fileList[i], myValues);
            if (err != 0) {
               goto bail;
            }
        }

        free(fileList[i]);
        fileList[i] = NULL;
      }
   }

   if (myValues->lockList != NULL) {
      goto bail;
   }

   // Pass 2: Handle the 'M' entries
   for (i = 0, err = 0; i < numEntries; i++) {
      LockValues *ptr;
      Bool       myLockFile;
      LockValues memberValues;
      char       buffer[FILELOCK_DATA_SIZE];

      if ((fileList[i] == NULL) || (*fileList[i] == 'E')) {
         continue;
      }

      myLockFile = strcmp(fileList[i],
                          myValues->memberName) == 0 ? TRUE : FALSE;

      if (myLockFile) {
         // It's me! No need to read or validate anything.
         ptr = myValues;
      } else {
         // It's not me! Attempt to extract the member values.
         err = FileLockMemberValues(lockDir, fileList[i], buffer,
                                    FILELOCK_DATA_SIZE, &memberValues);

         if (err != 0) {
            if (err == ENOENT) {
               // Not there anymore; locker unlocked or timed out
               continue;
            }

            break;
         }

         // Remove any stale locking files
         if (FileLockMachineIDMatch(myValues->machineID,
                                    memberValues.machineID) &&
             !FileLockValidOwner(memberValues.executionID,
                                 memberValues.payload)) {
            Log(LGPFX" %s discarding file '%s%s%s'; invalid executionID.\n",
                __FUNCTION__, lockDir, DIRSEPS, fileList[i]);

            err = RemoveLockingFile(lockDir, fileList[i]);
            if (err != 0) {
               break;
            }

            continue;
         }

         ptr = &memberValues;
      }

      // Locking file looks good; see what happens
      err = (*func)(lockDir, fileList[i], ptr, myValues);
      if (err != 0) {
         break;
      }
   }

bail:

   for (i = 0; i < numEntries; i++) {
      free(fileList[i]);
   }

   free(fileList);

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Scanner --
 *
 *	Call the specified function for each member file found in the
 *	specified directory. If a rescan is necessary check the list
 *	of outstanding locks and handle removing stale locks.
 *
 * Results:
 *	0	success
 *	not 0	failure
 *
 * Side effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

static int
Scanner(const char *lockDir,     // IN:
        int (*func)(             // IN:
               const char *lockDir,
               const char *fileName,
               LockValues *memberValues,
               LockValues *myValues
            ),
        LockValues *myValues,    // IN:
        Bool cleanUp)            // IN:
{
   int        err;
   ActiveLock *ptr;

   myValues->lockList = NULL;

   while (TRUE) {
      ActiveLock *prev;

      err = ScanDirectory(lockDir, func, myValues, cleanUp);
      if ((err > 0) || ((err == 0) && (myValues->lockList == NULL))) {
         break;
      }

      prev = NULL;
      ptr = myValues->lockList;

      /*
       * Some 'D' entries have persisted. Age them and remove those that
       * have not progressed. Remove those that have disappeared.
       */

      while (ptr != NULL) {
         Bool remove;

         if (ptr->marked) {
            if (ptr->age > FILELOCK_PROGRESS_DEARTH) {
               char *p;
               char path[FILE_MAXPATH];

               ASSERT(ptr->dirName[0] == 'D');

               Log(LGPFX" %s discarding %s data from '%s'.\n",
                   __FUNCTION__, ptr->dirName, lockDir);

               Str_Sprintf(path, sizeof path, "%s%s%s", lockDir,
                           DIRSEPS, ptr->dirName);

               p = strrchr(path, 'D');
               ASSERT(p);

               *p = 'M';
               unlink(path);

               *p = 'E';
               unlink(path);

               *p = 'D';
               rmdir(path);

               remove = TRUE;
            } else {
               ptr->marked = FALSE;
               ptr->age += FILELOCK_PROGRESS_SAMPLE;

               remove = FALSE;
            }
         } else {
            remove = TRUE;
         }

         if (remove) {
            if (prev == NULL) {
               myValues->lockList = ptr->next;
            } else {
               prev->next = ptr->next;
            }
         }

         prev = ptr;
         ptr = ptr->next;
      }

      usleep(FILELOCK_PROGRESS_SAMPLE * 1000); // relax
   }

   // Clean up anything still on the list; they are no longer important
   while (myValues->lockList != NULL) {
      ptr = myValues->lockList;
      myValues->lockList = ptr->next;

      free(ptr);
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FileUnlockIntrinsic --
 *
 *	Release a lock on a file.
 *
 *	The locker is required to identify themselves in a "universally
 *	unique" manner. This is done via two parameters:
 *
 *	machineID --
 *		This a machine/hardware identifier string.
 *
 *		The MAC address of a hardware Ethernet, a WWN of a
 *		hardware FibreChannel HBA, the UUID of an Infiniband HBA
 *		and a machine serial number (e.g. Macs) are all good
 *		candidates for a machine identifier.
 *
 *	executionID --
 *		This is an string which differentiates one thread of
 *		execution from another within the host OS. In a
 *		non-threaded environment this can simply be some form
 *		of process identifier (e.g. getpid() on UNIXen or
 *		_getpid() on Windows). When a process makes use of
 *		threads AND more than one thread may perform locking
 *		this identifier must discriminate between all threads
 *		within the process.
 *
 *	All of the ID strings must encode their respective information
 *	such that any OS may utilize the strings as part of a file name.
 *	Keep them short and, at a minimum, do not use ':', '/', '\', '.'
 *	and white space characters.
 *
 * Results:
 *	0	unlocked
 *	>0	errno
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

int
FileUnlockIntrinsic(const char *machineID,    // IN:
                    const char *executionID,  // IN:
                    const char *filePathName, // IN:
                    const void *lockToken)    // IN:
{
   int err;

   ASSERT(machineID);
   ASSERT(executionID);
   ASSERT(filePathName);
   ASSERT(lockToken);

   LOG(1, ("Releasing lock on %s (%s, %s).\n", filePathName,
       machineID, executionID));

   /*
    * An implicit read lock is granted with a special, fixed-address token.
    * General locks have a lock token that is actually the pathname of the
    * lock file representing the lock.
    *
    * TODO: under vmx86_debug validate the contents of the lock file as
    *       matching the machineID and executionID.
    */

   if (lockToken == &implicitReadToken) {
      err = 0;
   } else {
      char *dirPath;

      // The lock directory path
      dirPath = Str_Asprintf(NULL, "%s%s", filePathName, FILELOCK_SUFFIX);
      if (dirPath == NULL) {
         return ENOMEM;
      }

      if (strncmp((char *) lockToken, dirPath, strlen(dirPath)) == 0) {
         /* The token looks good. Do the unlock */
         err = unlink((char *) lockToken) == -1 ? errno : 0;

         if (err && vmx86_debug) {
            Log(LGPFX" %s unlock failed for %s: %s\n",
                __FUNCTION__, (char *) lockToken, strerror(err));
         }
      } else {
         if (vmx86_debug) {
            Log(LGPFX" %s unlock of %s with invalid token (%s)\n",
                __FUNCTION__, filePathName, (char *) lockToken);
         }

         err = EINVAL; // The lock token is not valid for this file.
      }

      free((void *) lockToken); // It was allocated in FileLockIntrinsic.

      rmdir(dirPath); // just in case we can clean up

      free(dirPath);
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * WaitForPossession --
 *
 *	Wait until the caller has a higher priority towards taking
 *	possession of a lock than the specified file.
 *
 * Results:
 *	0	success
 *	> 0	error (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
WaitForPossession(const char *lockDir,      // IN:
                  const char *fileName,     // IN:
                  LockValues *memberValues, // IN:
                  LockValues *myValues)     // IN:
{
   int err = 0;

   // "Win" or wait?
   if (((memberValues->lamportNumber < myValues->lamportNumber) ||
       ((memberValues->lamportNumber == myValues->lamportNumber) &&
          (strcmp(memberValues->memberName, myValues->memberName) < 0))) &&
        ((strcmp(memberValues->lockType, LOCK_EXCLUSIVE) == 0) ||
         (strcmp(myValues->lockType, LOCK_EXCLUSIVE) == 0))) {
      uint32 loopCount;
      Bool   thisMachine; 
      char   path[FILE_MAXPATH];

      thisMachine = FileLockMachineIDMatch(myValues->machineID,
                                           memberValues->machineID);

      loopCount = 0;
      Str_Sprintf(path, sizeof path, "%s%s%s", lockDir, DIRSEPS,
                  fileName);

      while ((err = Sleeper(myValues, &loopCount)) == 0) {
         struct stat statbuf;

         // still there?
         if (stat(path, &statbuf) != 0) {
            if (errno == ENOENT) {
               // Not there anymore; locker unlocked or timed out
               errno = 0;
            }
            err = errno;
            break;
         }

         // still valid?
         if (thisMachine && !FileLockValidOwner(memberValues->executionID,
                                                memberValues->payload)) {
            // Invalid Execution ID; remove the member file
            Warning(LGPFX" %s discarding file '%s'; invalid executionID.\n",
                    __FUNCTION__, path);

            err = RemoveLockingFile(lockDir, fileName);
            break;
         }
      }
   }

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * NumberScan --
 *
 *	Determine the maxmimum number value within the current locking set.
 *
 * Results:
 *	0	success
 *	not 0	failure (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
NumberScan(const char *lockDir,       // IN:
           const char *fileName,      // IN:
           LockValues *memberValues,  // IN:
           LockValues *myValues)      // IN/OUT:
{
   if (memberValues->lamportNumber > myValues->lamportNumber) {
      myValues->lamportNumber = memberValues->lamportNumber;
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CreateLockingDirectory --
 *
 *	Create a locking directory.
 *
 * Results:
 *	0	success
 *	not 0	failure (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
CreateLockingDirectory(const char *path, // IN:
                       mode_t mode)      // IN:
{
   int    err;
   mode_t save;

#if defined(_WIN32)	// Windows
   save = _umask(0);
   err = (_mkdir(path) == 0) ? 0 : errno;
   _umask(save);
#else			// Not Windows
   save = umask(0);
   err = (mkdir(path, mode) == 0) ? 0 : errno;
   umask(save);
#endif			// Not Windows

   return err;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SimpleRandomNumber --
 *
 *	Return a random number in the range of 0 and 2^16-1. A linear
 *	congruential generator is used. The values are taken from
 *	"Numerical Recipies in C".
 *
 * Results:
 *	Random number is returned.
 *
 * Side Effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static uint32
SimpleRandomNumber(const char *machineID,   // IN:
                   const char *executionID) // IN:
{
   static uint32 value = 0;

   if (value == 0) {
      /*
       * Use the machineID and executionID to hopefully start each machine
       * and process/thread at a different place in the answer stream.
       */

      while (*machineID) {
         value += *machineID++;
      }

      while (*executionID) {
         value += *executionID++;
      }
   }

   value = (1664525 * value) + 1013904223;

   return (value >> 8) & 0xFFFF;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CreateEntryDirectory --
 *
 *	Create an entry directory in the specified locking directory.
 *
 *	Due to FileLock_UnlockFile() attempting to remove the locking
 *	directory on an unlock operation (to "clean up" and remove the
 *	locking directory when it is no longer needed), this routine
 *	must carefully handle a number of race conditions to insure the
 *	the locking directory exists and the entry directory is created
 *	within.
 *
 * Results:
 *	0	success
 *	not 0	failure (errno)
 *
 * Side Effects:
 *	On success returns the number identifying the entry directory and
 *	the entry directory path name.
 *
 *-----------------------------------------------------------------------------
 */

static int
CreateEntryDirectory(const char *machineID,   // IN:
                     const char *executionID, // IN:
                     const char *lockDir,     // IN:
                     char **entryDirectory,   // OUT:
                     char **entryFilePath,    // OUT:
                     char **memberFilePath,   // OUT:
                     char *memberName)        // OUT:
{
   int err = 0;
   uint32 randomNumber = 0;

   *entryDirectory = NULL;
   *entryFilePath = NULL;
   *memberFilePath = NULL;

   /* Fun at the races */

   while (TRUE) {
      struct stat statbuf;

      if (stat(lockDir, &statbuf) == 0) {
        // The name exists. Deal with it...

        if ((statbuf.st_mode & S_IFMT) == S_IFREG) {
           /*
            * It's a file. Assume this is an (active?) old style lock
            * and err on the safe side - don't remove it (and
            * automatically upgrade to a new style lock).
            */

            Log(LGPFX" %s: '%s' exists; an old style lock file?\n",
                      __FUNCTION__, lockDir);

            err = EAGAIN;
            break;
        }

        if ((statbuf.st_mode & S_IFMT) != S_IFDIR) {
           // Not a directory; attempt to remove the debris
           if (unlink(lockDir) != 0) {
              Warning(LGPFX" %s: '%s' exists and is not a directory.\n",
                      __FUNCTION__, lockDir);

              err = ENOTDIR;
              break;
           }

           continue;
        }
      } else {
         if (errno == ENOENT) {
            // Not there anymore; locker unlocked or timed out
            err = CreateLockingDirectory(lockDir, 0777);

            if ((err != 0) && (err != EEXIST)) {
               Warning(LGPFX" %s lock directory '%s' creation failure: %s\n",
                       __FUNCTION__, lockDir, strerror(err));

               break;
            }
         } else {
            err = errno;

            Warning(LGPFX" %s lock directory '%s' stat failure: %s\n",
                    __FUNCTION__, lockDir, strerror(err));

            break;
         }
      }

      // There is a small chance of collision/failure; grab stings now
      randomNumber = SimpleRandomNumber(machineID, executionID);

      Str_Sprintf(memberName, FILELOCK_OVERHEAD, "M%05u%s", randomNumber,
                  FILELOCK_SUFFIX);

      *entryDirectory = Str_Asprintf(NULL, "%s%sD%05u%s", lockDir, DIRSEPS,
                                     randomNumber, FILELOCK_SUFFIX);

      *entryFilePath = Str_Asprintf(NULL, "%s%sE%05u%s", lockDir, DIRSEPS,
                                    randomNumber, FILELOCK_SUFFIX);

      *memberFilePath = Str_Asprintf(NULL, "%s%s%s", lockDir, DIRSEPS,
                                     memberName);

      if ((*entryDirectory == NULL) || (*entryFilePath == NULL) ||
          (*memberFilePath == NULL)) {
         err = ENOMEM;
         break;
      }

      err = CreateLockingDirectory(*entryDirectory, 0777);

      if (err == 0) {
         /*
          * The entry directory was safely created. See if a member file
          * is in use (the entry directory is removed once the member file
          * is created). If a member file is in use, choose another number,
          * otherwise the use of the this number is OK.
          *
          * Err on the side of caution... don't want to trash perfectly
          * good member files.
          */

         if (stat(*memberFilePath, &statbuf) != 0) {
            if (errno == ENOENT) {
               break;
            }

            if (vmx86_debug) {
               Log(LGPFX" %s stat() of '%s': %s\n",
                   __FUNCTION__, *memberFilePath, strerror(errno));
             }
         }

         rmdir(*entryDirectory);
      } else {
          if (err != EEXIST) {
             Warning(LGPFX" %s lock directory '%s' creation failure: %s\n",
                     __FUNCTION__, *entryDirectory, strerror(err));

             break;
          }
      }

      free(*entryDirectory);
      free(*entryFilePath);
      free(*memberFilePath);

      *entryDirectory = NULL;
      *entryFilePath = NULL;
      *memberFilePath = NULL;
   }

   if (err != 0) {
      free(*entryDirectory);
      free(*entryFilePath);
      free(*memberFilePath);

      *entryDirectory = NULL;
      *entryFilePath = NULL;
      *memberFilePath = NULL;
      *memberName = '\0';
   }

   return err;
}

/*
 *-----------------------------------------------------------------------------
 *
 * CreateMemberFile --
 *
 *	Create the member file.
 *
 * Results:
 *	0	success
 *	not 0	failure (errno)
 *
 * Side Effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

static int
CreateMemberFile(int entryFd,                // IN:
                 const LockValues *myValues, // IN:
                 const char *entryFilePath,  // IN:
                 const char *memberFilePath) // IN:
{
   int err;
   ssize_t len;
   char buffer[FILELOCK_DATA_SIZE] = { 0 };

   // Populate the buffer with appropriate data
   Str_Sprintf(buffer, sizeof buffer, "%s %s %u %s %s", myValues->machineID,
               myValues->executionID, myValues->lamportNumber,
               myValues->lockType,
               myValues->payload == NULL ? "" : myValues->payload);

   // Attempt to write the data
   len = write(entryFd, buffer, sizeof buffer);

   if (len == -1) {
      err = errno;

      Warning(LGPFX" %s write of '%s' failed: %s\n", __FUNCTION__,
              entryFilePath, strerror(err));

      close(entryFd);

      return err;
   }

   if (close(entryFd) == -1) {
      err = errno;

      Warning(LGPFX" %s close of '%s' failed: %s\n", __FUNCTION__,
              entryFilePath, strerror(err));

      return err;
   }

   if (len != sizeof buffer) {
      Warning(LGPFX" %s write length issue on '%s': %"FMTSZ"d and %"FMTSZ"d\n",
              __FUNCTION__, entryFilePath, len, sizeof buffer);

      return EIO;
   }

   if (rename(entryFilePath, memberFilePath) != 0) {
      err = errno;

      Warning(LGPFX" %s rename of '%s' to '%s' failed: %s\n",
              __FUNCTION__, entryFilePath, memberFilePath,
              strerror(err));

      if (vmx86_debug) {
         struct stat statbuf;

         Log(LGPFX" %s stat() of '%s': %s\n",
             __FUNCTION__, entryFilePath,
            strerror(stat(entryFilePath, &statbuf) == 0 ? 0 : errno));

         Log(LGPFX" %s stat() of '%s': %s\n",
             __FUNCTION__, memberFilePath,
            strerror(stat(memberFilePath, &statbuf) == 0 ? 0 : errno));
      }

      return err;
   }

   return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FileLockIntrinsic --
 *
 *	Obtain a lock on a file; shared or exclusive access.
 *
 *	Each locker is required to identify themselves in a "universally
 *	unique" manner. This is done via two parameters:
 *
 *	machineID --
 *		This a machine/hardware identifier string.
 *
 *		The MAC address of a hardware Ethernet, a WWN of a
 *		hardware FibreChannel HBA, the UUID of an Infiniband HBA
 *		and a machine serial number (e.g. Macs) are all good
 *		candidates for a machine identifier.
 *
 *		The machineID is "univerally unique", discriminating
 *		between all computational platforms.
 *
 *	executionID --
 *		This is an string which differentiates one thread of
 *		execution from another within the host OS. In a
 *		non-threaded environment this can simply be some form
 *		of process identifier (e.g. getpid() on UNIXen or
 *		_getpid() on Windows). When a process makes use of
 *		threads AND more than one thread may perform locking
 *		this identifier must discriminate between all threads
 *		within the process.
 *
 *	All of the ID strings must encode their respective information
 *	such that any OS may utilize the strings as part of a file name.
 *	Keep them short and, at a minimum, do not use ':', '/', '\', '.'
 *	and white space characters.
 *
 *	msecMaxWaitTime specifies the maximum amount of time, in
 *	milliseconds, to wait for the lock before returning the "not
 *	acquired" status. A value of FILELOCK_TRYLOCK_WAIT is the
 *	equivalent of a "try lock" - the lock will be acquired only if
 *	there is no contention. A value of FILELOCK_INFINITE_WAIT
 *	specifies "waiting forever" to acquire the lock.
 *
 * Results:
 *	NULL	Lock not acquired. Check err.
 *		err	0	Lock Timed Out
 *		err	!0	errno
 *	!NULL	Lock Acquired. This is the "lockToken" for an unlock.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

void *
FileLockIntrinsic(const char *machineID,    // IN:
                  const char *executionID,  // IN:
                  const char *payload,      // IN:
                  const char *filePathName, // IN:
                  Bool exclusivity,         // IN:
                  uint32 msecMaxWaitTime,   // IN:
                  int *err)                 // OUT:
{
   int        fd;
   LockValues myValues;

   char       *dirPath = NULL;
   char       *entryFilePath = NULL;
   char       *memberFilePath = NULL;
   char       *entryDirectory = NULL;

   ASSERT(machineID);
   ASSERT(executionID);
   ASSERT(filePathName);
   ASSERT(err);

   // establish our values
   myValues.machineID = (char *) machineID;
   myValues.executionID = (char *) executionID;
   myValues.payload = (char *) payload;
   myValues.lockType = exclusivity ? LOCK_EXCLUSIVE : LOCK_SHARED;
   myValues.lamportNumber = 0;
   myValues.waitTime = 0;
   myValues.msecMaxWaitTime = msecMaxWaitTime;

   LOG(1, ("Requesting %s lock on %s (%s, %s, %u).\n",
       myValues.lockType, filePathName, myValues.machineID,
       myValues.executionID, myValues.msecMaxWaitTime));

   /*
    * Enforce the maximum path length restriction explicitely. Apparently
    * the Windows POSIX routine mappings cannot be trusted to return
    * ENAMETOOLONG when it is appropriate.
    */

   if ((strlen(filePathName) + FILELOCK_OVERHEAD) >= FILE_MAXPATH) {
      *err = ENAMETOOLONG;
      goto bail;
   }

   // Construct the locking directory path
   dirPath = Str_Asprintf(NULL, "%s%s", filePathName, FILELOCK_SUFFIX);

   if (dirPath == NULL) {
      *err = ENOMEM;
      goto bail;
   }

   /*
    * Attempt to create the locking and entry directories; obtain the
    * entry and member path names.
    */

   *err = CreateEntryDirectory(machineID, executionID, dirPath, &entryDirectory,
                               &entryFilePath, &memberFilePath,
                               myValues.memberName);

   switch (*err) {
   case 0:
      break;

   case EROFS:
      /* FALL THROUGH */
   case EACCES:
      if (!exclusivity) {
         /*
          * Lock is for read/shared access however the lock directory could
          * not be created. Grant an implicit read lock whenever possible.
          * The address of a private variable will be used for the lock token.
          */

         Warning(LGPFX" %s implicit %s lock succeeded on '%s'.\n",
                 __FUNCTION__, LOCK_SHARED, filePathName);

         *err = 0;
         memberFilePath = &implicitReadToken;
      }

      /* FALL THROUGH */
   default:
      goto bail;
   }

   ASSERT(strlen(memberFilePath) - strlen(filePathName) <=
          FILELOCK_OVERHEAD);

   // Attempt to create the entry file
   fd = PosixFileOpener(entryFilePath, LOCKFILE_CREATION_FLAGS,
                        LOCKFILE_CREATION_PERMISSIONS);

   if (fd == -1) {
      *err = errno;

      rmdir(entryDirectory); // Remove entry directory; it has done its job
      rmdir(dirPath); // just in case we can clean up

      goto bail;
   }

   // what is max(Number[1]... Number[all lockers])?
   *err = Scanner(dirPath, NumberScan, &myValues, FALSE);

   if (*err != 0) {
      close(fd); // clean-up; be nice
      unlink(entryFilePath); // clean-up; be nice
      rmdir(entryDirectory); // Remove entry directory; it has done its job
      rmdir(dirPath); // just in case we can clean up

      goto bail;
   }

   // Number[i] = 1 + max([Number[1]... Number[all lockers])
   myValues.lamportNumber++;

   // Attempt to create the member file
   *err = CreateMemberFile(fd, &myValues, entryFilePath, memberFilePath);

   rmdir(entryDirectory); // Remove entry directory; it has done its job

   if (*err != 0) {
      unlink(entryFilePath); // clean-up; be nice
      unlink(memberFilePath); // clean-up; be nice
      rmdir(dirPath); // just in case we can clean up

      goto bail;
   }

   // Attempt to acquire the lock
   *err = Scanner(dirPath, WaitForPossession, &myValues, TRUE);

   switch (*err) {
   case 0:
      break;

   case EAGAIN:
      unlink(memberFilePath); // clean-up; be nice
      rmdir(dirPath); // just in case we can clean up

      /* FALL THROUGH */
   default:
      break;
   }

bail:

   free(dirPath);
   free(entryDirectory);
   free(entryFilePath);

   if (*err != 0) {
      free(memberFilePath);
      memberFilePath = NULL;

      if (*err == EAGAIN) {
         *err = 0; // lock not acquired
      }
   }

   return (void *) memberFilePath;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ScannerVMX --
 *
 *	VMX hack scanner
 *
 * Results:
 *	0	success
 *	not 0	error (errno)
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static int
ScannerVMX(const char *lockDir,      // IN:
           const char *fileName,     // IN:
           LockValues *memberValues, // IN:
           LockValues *myValues)     // IN/OUT:
{
   myValues->lamportNumber++;

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * FileLockHackVMX --
 *
 *	The VMX file delete primitive.
 *
 * Results:
 *	0	unlocked
 *	>0	errno
 *
 * Side effects:
 *      Changes the host file system.
 *
 * Note:
 *	THIS IS A HORRIBLE HACK AND NEEDS TO BE REMOVED ASAP!!!
 *
 *----------------------------------------------------------------------
 */

int
FileLockHackVMX(const char *machineID,    // IN:
                const char *executionID,  // IN:
                const char *filePathName) // IN:
{
   int        err;
   LockValues myValues;

   char       *dirPath = NULL;
   char       *entryFilePath = NULL;
   char       *memberFilePath = NULL;
   char       *entryDirectory = NULL;

   LOG(1, ("%s on %s (%s, %s).\n", __FUNCTION__, filePathName,
       machineID, executionID));

   // first the locking directory path name
   dirPath = Str_Asprintf(NULL, "%s%s", filePathName, FILELOCK_SUFFIX);

   if (dirPath == NULL) {
      err = ENOMEM;
      goto bail;
   }

   err = CreateEntryDirectory(machineID, executionID, dirPath, &entryDirectory,
                              &entryFilePath, &memberFilePath,
                              myValues.memberName);

   if (err != 0) {
      goto bail;
   }

   // establish our values
   myValues.machineID = (char *) machineID;
   myValues.executionID = (char *) executionID;
   myValues.lamportNumber = 0;

   // Scan the lock directory
   err = Scanner(dirPath, ScannerVMX, &myValues, FALSE);

   if (err == 0) {
      // if no members are valid, clean up
      if (myValues.lamportNumber == 1) {
         unlink(filePathName);
      }
   } else {
      if (vmx86_debug) {
         Warning(LGPFX" %s clean-up failure for '%s': %s\n",
                 __FUNCTION__, filePathName, strerror(err));
      }
   }

   rmdir(entryDirectory); // clean-up; be nice
   rmdir(dirPath); // just in case we can clean up

bail:

   free(dirPath);
   free(entryDirectory);
   free(entryFilePath);
   free(memberFilePath);

   return err;
}
