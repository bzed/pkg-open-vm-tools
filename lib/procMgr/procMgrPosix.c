/*
 * Copyright 1998 VMware, Inc.  All rights reserved. 
 *
 *
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
 * procMgrPosix.c --
 *
 *    Posix implementation of the process management lib
 *
 */

#ifndef VMX86_DEVEL

#endif

// pull in setresuid()/setresgid() if possible
#define  _GNU_SOURCE
#include <unistd.h>
#if !defined(__FreeBSD__) && !defined(sun)
#include <asm/param.h>
#include <locale.h>
#include <sys/stat.h>
#endif
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <time.h>
#include <grp.h>
#include <sys/syscall.h>

#include "vmware.h"
#include "procMgr.h"
#include "vm_assert.h"
#include "debug.h"
#include "util.h"
#include "msg.h"
#include "vmsignal.h"
#undef offsetof
#include "file.h"
#include "dynbuf.h"
#include "su.h"
#include "str.h"


/*
 * The IPC messages sent from the child process to the parent. These
 * are 1 byte so that we are guaranteed they are written over the pipe
 * in one go.
 */
#define ASYNCEXEC_SUCCESS_IPC "1"
#define ASYNCEXEC_FAILURE_IPC "0"


/*
 * All signals that:
 * . Can terminate the process
 * . May occur even if the program has no bugs
 */
static int const cSignals[] = {
   SIGHUP,
   SIGINT,
   SIGQUIT,
   SIGTERM,
   SIGUSR1,
   SIGUSR2,
};


/*
 * Keeps track of the posix async proc info.
 */
struct ProcMgr_AsyncProc {
   pid_t waiterPid;  // pid of the waiter process
   int   fd;           // fd to write to when the child is done
   Bool validExitCode;
   int exitCode;
};

static Bool ProcMgrExecSync(char const *cmd,
                            Bool *validExitCode,
			    int *exitCode);


#if defined(linux) && !defined(GLIBC_VERSION_23)
/*
 * Implements the system calls (they are not wrapped by glibc til 2.3.2)
 */
static
_syscall3(int, setresuid,
          uid_t, ruid,
          uid_t, euid,
          uid_t, suid);

static
_syscall3(int, setresgid,
          gid_t, rgid,
          gid_t, egid,
          gid_t, sgid);
#endif

/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_ListProcesses --
 *
 *      List all the processes that the calling client has privilege to
 *      enumerate.
 *
 * Results:
 *      
 *      A ProcMgr_ProcList.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

ProcMgr_ProcList *
ProcMgr_ListProcesses(void)
{
   ProcMgr_ProcList *procList = NULL;
#if !defined(__FreeBSD__) && !defined(sun)
   Bool failed = FALSE;
   DynBuf dbProcId;
   DynBuf dbProcCmd;
   DynBuf dbProcStartTime;
   DynBuf dbProcOwner;
   DIR *dir;
   struct dirent *ent;
   static time_t hostStartTime = 0;
   static unsigned long long hertz = 100;
   int numberFound;

   DynBuf_Init(&dbProcId);
   DynBuf_Init(&dbProcCmd);
   DynBuf_Init(&dbProcStartTime);
   DynBuf_Init(&dbProcOwner);

   /*
    * Figure out when the system started.  We need this number to
    * compute process start times, which are relative to this number.
    * We grab the first float in /proc/uptime, convert it to an integer,
    * and then subtract that from the current time.  That leaves us
    * with the seconds since epoch that the system booted up.
    */
   if (0 == hostStartTime) {
      FILE *uptimeFile = NULL;

      uptimeFile = fopen("/proc/uptime", "r");
      if (NULL != uptimeFile) {
         double secondsSinceBoot;
         char *realLocale;

         /*
          * Set the locale such that floats are delimited with ".".
          */
         realLocale = setlocale(LC_NUMERIC, NULL);
         setlocale(LC_NUMERIC, "C");
         numberFound = fscanf(uptimeFile, "%lf", &secondsSinceBoot);
         setlocale(LC_NUMERIC, realLocale);

         /*
          * Figure out system boot time in absolute terms.
          */
         if (numberFound) {
            hostStartTime = time(NULL) - (time_t) secondsSinceBoot;
         }
         fclose(uptimeFile);
      }

      /*
       * Figure out the "hertz" value, which may be radically
       * different than the actual CPU frequency of the machine.
       * The process start time is expressed in terms of this value,
       * so let's compute it now and keep it in a static variable.
       */
#ifdef HZ
      hertz = (unsigned long long) HZ;
#else
      /*
       * Don't do anything.  Use the default value of 100.
       */
#endif
   } // if (0 == hostStartTime)

   /*
    * Scan /proc for any directory that is all numbers.
    * That represents a process id.
    */
   dir = opendir("/proc");
   if (NULL == dir) {
      Warning("ProcMgr_ListProcesses unable to open /proc\n");
      failed = TRUE;
      goto abort;
   }

   while ((ent = readdir(dir))) {
      struct stat fileStat;
      char cmdFilePath[1024];
      int statResult;
      int numRead = 0;   /* number of bytes that read() actually read */
      int cmdFd;
      pid_t pid;
      int replaceLoop;
      struct passwd *pwd;
      char cmdLineTemp[2048];
      char cmdStatTemp[2048];
      char *cmdLine;
      char *userName = NULL;
      size_t strLen = 0;
      unsigned long long dummy;
      unsigned long long relativeStartTime;
      char *stringBegin;
      time_t processStartTime;

      /*
       * We only care about dirs that look like processes.
       */
      if (strspn(ent->d_name, "0123456789") != strlen(ent->d_name)) {
         continue;
      }

      if (snprintf(cmdFilePath,
                   sizeof cmdFilePath,
                   "/proc/%s/cmdline",
                   ent->d_name) == -1) {
         Debug("Giant process id '%s'\n", ent->d_name);
         continue;
      }
      
      cmdFd = open(cmdFilePath, O_RDONLY);
      if (-1 == cmdFd) {
         /*
          * We may not be able to open the file due to the security reason.
          * In that case, just ignore and continue.
          */
         continue;
      }

      /*
       * Read in the command and its arguments.  Arguments are separated
       * by \0, which we convert to ' '.  Then we add a NULL terminator
       * at the end.  Example: "perl -cw try.pl" is read in as
       * "perl\0-cw\0try.pl\0", which we convert to "perl -cw try.pl\0".
       * It would have been nice to preserve the NUL character so it is easy
       * to determine what the command line arguments are without
       * using a quote and space parsing heuristic.  But we do this
       * to have parity with how Windows reports the command line.
       * In the future, we could keep the NUL version around and pass it
       * back to the client for easier parsing when retrieving individual
       * command line parameters is needed.
       *
       * We read at most (sizeof cmdLineTemp) - 1 bytes to leave room
       * for NUL termination at the end.
       */
      numRead = read(cmdFd, cmdLineTemp, sizeof cmdLineTemp - sizeof(char));
      close(cmdFd);

      if (1 > numRead) {
         continue;
      }
      for (replaceLoop = 0 ; replaceLoop < (numRead - 1) ; replaceLoop++) {
         if ('\0' == cmdLineTemp[replaceLoop]) {
            cmdLineTemp[replaceLoop] = ' ';
         }
      }

      /*
       * There is an edge case where /proc/#/cmdline does not NUL terminate
       * the command.  /sbin/init (process 1) is like that on some distros.
       * So let's guarantee that the string is NUL terminated, even if
       * the last character of the string might already be NUL.
       * This is safe to do because we read at most (sizeof cmdLineTemp) - 1
       * bytes from /proc/#/cmdline -- we left just enough space to add
       * NUL termination at the end.
       */
      cmdLineTemp[numRead] = '\0';

      /*
       * Get the inode information for this process.  This gives us
       * the process owner.
       */
      if (snprintf(cmdFilePath,
                   sizeof cmdFilePath,
                   "/proc/%s",
                   ent->d_name) == -1) {
         Debug("Giant process id '%s'\n", ent->d_name);
         continue;
      }

      /*
       * stat() /proc/<pid> to get the owner.  We use fileStat.st_uid
       * later in this code.  If we can't stat(), ignore and continue.
       * Maybe we don't have enough permission.
       */
      statResult = stat(cmdFilePath, &fileStat);
      if (0 != statResult) {
         continue;
      }

      /*
       * Figure out the process start time.  Open /proc/<pid>/stat
       * and read the start time and compute it in absolute time.
       */
      if (snprintf(cmdFilePath,
                   sizeof cmdFilePath,
                   "/proc/%s/stat",
                   ent->d_name) == -1) {
         Debug("Giant process id '%s'\n", ent->d_name);
         continue;
      }
      cmdFd = open(cmdFilePath, O_RDONLY);
      if (-1 == cmdFd) {
         continue;
      }
      numRead = read(cmdFd, cmdStatTemp, sizeof cmdStatTemp);
      close(cmdFd);
      if (0 >= numRead) {
         continue;
      }
      /*
       * Skip over initial process id and process name.  "123 (bash) [...]".
       */
      stringBegin = strchr(cmdStatTemp, ')') + 2;
      
      numberFound = sscanf(stringBegin, "%c %d %d %d %d %d "
                           "%lu %lu %lu %lu %lu %Lu %Lu %Lu %Lu %ld %ld "
                           "%d %ld %Lu",
                           (char *) &dummy, (int *) &dummy, (int *) &dummy,
                           (int *) &dummy, (int *) &dummy,  (int *) &dummy,
                           (unsigned long *) &dummy, (unsigned long *) &dummy,
                           (unsigned long *) &dummy, (unsigned long *) &dummy,
                           (unsigned long *) &dummy,
                           (unsigned long long *) &dummy,
                           (unsigned long long *) &dummy,
                           (unsigned long long *) &dummy,
                           (unsigned long long *) &dummy,
                           (long *) &dummy, (long *) &dummy,
                           (int *) &dummy, (long *) &dummy,
                           &relativeStartTime);
      if (20 != numberFound) {
         continue;
      }
      processStartTime = hostStartTime + (relativeStartTime / hertz);

      /*
       * Store the command line string pointer in dynbuf.
       */
      cmdLine = strdup(cmdLineTemp);
      DynBuf_Append(&dbProcCmd, &cmdLine, sizeof cmdLine);

      /*
       * Store the pid in dynbuf.
       */
      pid = (pid_t) atoi(ent->d_name);
      DynBuf_Append(&dbProcId, &pid, sizeof pid);

      /*
       * Store the owner of the process.
       */
      pwd = getpwuid(fileStat.st_uid);
      userName = (NULL == pwd)
                 ? Str_Asprintf(&strLen, "%d", (int) fileStat.st_uid)
                 : Util_SafeStrdup(pwd->pw_name);
      DynBuf_Append(&dbProcOwner, &userName, sizeof userName);

      /*
       * Store the time that the process started.
       */
      DynBuf_Append(&dbProcStartTime,
                    &processStartTime,
                    sizeof processStartTime);
   } // while readdir

   closedir(dir);

   if (0 == DynBuf_GetSize(&dbProcId)) {
      failed = TRUE;
      goto abort;
   }

   /*
    * We're done adding to DynBuf.  Trim off any unused allocated space.
    * DynBuf_Trim() followed by DynBuf_Detach() avoids a memcpy().
    */
   DynBuf_Trim(&dbProcId);
   DynBuf_Trim(&dbProcCmd);
   DynBuf_Trim(&dbProcStartTime);
   DynBuf_Trim(&dbProcOwner);
   /*
    * Create a ProcMgr_ProcList and populate its fields.
    */
   procList = (ProcMgr_ProcList *) calloc(1, sizeof(ProcMgr_ProcList));
   ASSERT_NOT_IMPLEMENTED(procList);

   procList->procCount = DynBuf_GetSize(&dbProcId) / sizeof(pid_t);

   procList->procIdList = (pid_t *) DynBuf_Detach(&dbProcId);
   ASSERT_NOT_IMPLEMENTED(procList->procIdList);
   procList->procCmdList = (char **) DynBuf_Detach(&dbProcCmd);
   ASSERT_NOT_IMPLEMENTED(procList->procCmdList);
   procList->startTime = (time_t *) DynBuf_Detach(&dbProcStartTime);
   ASSERT_NOT_IMPLEMENTED(procList->startTime);
   procList->procOwnerList = (char **) DynBuf_Detach(&dbProcOwner);
   ASSERT_NOT_IMPLEMENTED(procList->procOwnerList);

abort:
   DynBuf_Destroy(&dbProcId);
   DynBuf_Destroy(&dbProcCmd);
   DynBuf_Destroy(&dbProcStartTime);
   DynBuf_Destroy(&dbProcOwner);

   if (failed) {
      ProcMgr_FreeProcList(procList);
      procList = NULL;
   }
#endif // !defined(__FreeBSD__) && !defined(sun)

   return procList;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_FreeProcList --
 *
 *      Free the memory occupied by ProcMgr_ProcList.
 *
 * Results:
 *      
 *      None.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

void
ProcMgr_FreeProcList(ProcMgr_ProcList *procList)
{
   int i;

   if (NULL == procList) {
      return;
   }

   for (i = 0; i < procList->procCount; i++) {
      free(procList->procCmdList[i]);
      free(procList->procOwnerList[i]);
   }

   free(procList->procIdList);
   free(procList->procCmdList);
   free(procList->startTime);
   free(procList->procOwnerList);
   free(procList);
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgrWaiter --
 *
 *      The waiter process for ProcMgr_ExecAsync which runs in the
 *      child process. Execs the cmd & writes an IPC msg to the given
 *      fd when its done.
 *
 * Results:
 *      
 *      TRUE:  cmd was successful (exit code 0)
 *      FALSE: cmd failed or an error occurred (detail is displayed)
 *
 * Side effects:
 *
 *	Side effects
 *
 *----------------------------------------------------------------------
 */

static void
ProcMgrWaiter(const char *cmd, // IN
              int writeFd,     // IN
	      Bool *validExitCode,
	      int *exitCode)
{
   const char *doneMsg;
   Bool status;
   
   status = ProcMgrExecSync(cmd, validExitCode, exitCode);

   doneMsg = status ? ASYNCEXEC_SUCCESS_IPC : ASYNCEXEC_FAILURE_IPC;
   
   /* send IPC back to caller */
   Debug("Writing '%s' to fd %x\n", doneMsg, writeFd);
   if (write(writeFd, doneMsg, strlen(doneMsg) + 1) == -1) {
      Warning("Waiter unable to write back to parent\n");
      return;
   }
   if (write(writeFd, exitCode, sizeof(*exitCode)) == -1) {
      Warning("Waiter unable to write back to parent\n");
      return;
   }
   
   return;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_ExecSync --
 *
 *      Synchronously execute a cmd
 *
 * Results:
 *      
 *      TRUE on success (the program had an exit code of 0)
 *      FALSE on failure or if an error occurred (detail is displayed)
 *
 * Side effects:
 *
 *	Lots, depending on the program
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgr_ExecSync(char const *cmd,                  // IN: Command line
                 ProcMgr_ProcArgs *userArgs)       // IN: Unused
{
   Debug("Executing sync command: %s\n", cmd);

   return ProcMgrExecSync(cmd, NULL, NULL);
}

static Bool 
ProcMgrExecSync(char const *cmd,
                Bool *validExitCode,
		int *exitCode)
{
   Bool retVal;
   pid_t pid;
   int childStatus;

   if (NULL != validExitCode) {
      *validExitCode = FALSE;
   }

   pid = fork();
   
   if (pid == -1) {
      Warning("Unable to fork: %s.\n\n", strerror(errno));
      return FALSE;
   } else if (pid == 0) {
      /*
       * Child
       */
      
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      
      /* Failure */
      Panic("Unable to execute the \"%s\" shell command: %s.\n\n",
            cmd, strerror(errno));
   }

   /*
    * Parent
    */
   
   for (;;) {
      pid_t status;

      status = waitpid(pid, &childStatus, 0);
      if (status == pid) {
         /* Success */
         break;
      }

      if (   status == (pid_t)-1
          && errno == EINTR) {
         /* System call interrupted by a signal */
         continue;
      }

      Warning("Unable to wait for the \"%s\" shell command to terminate: "
              "%s.\n\n", cmd, strerror(errno));

      return FALSE;
   }

   if ((NULL != validExitCode) && (NULL != exitCode)) {
      *validExitCode = WIFEXITED(childStatus);
      *exitCode = WEXITSTATUS(childStatus);
   }

   retVal = (WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0);

   Debug("Done executing command: %s (%s)\n", cmd,
         retVal ? "success" : "failure");

   return retVal;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_ExecAsync --
 *
 *      Execute a command in the background, returning immediately.
 *
 * Results:
 *
 *      The async proc (must be freed) or
 *      NULL if the cmd failed to be forked.
 *
 * Side effects:
 *
 *	The cmd is run.
 *
 *----------------------------------------------------------------------
 */

ProcMgr_AsyncProc *
ProcMgr_ExecAsync(char const *cmd,                 // IN: Command line
                  ProcMgr_ProcArgs *userArgs)      // IN: Unused
{
   ProcMgr_AsyncProc *asyncProc;
   pid_t pid;
   int fds[2];
   Bool validExitCode;
   int exitCode;

   Debug("Executing async command: %s\n", cmd);

   if (pipe(fds) == -1) {
      ASSERT(FALSE);
   }

   pid = fork();

   if (pid == -1) {
      Warning("Unable to fork: %s.\n\n", strerror(errno));
      return NULL;
   } else if (pid == 0) {
      struct sigaction olds[ARRAYSIZE(cSignals)];
      int i, maxfd;

      /*
       * Child
       */

      /*
       * shut down everything but stdio and the pipe() we just made.
       * leaving all the other fds behind can cause nastiness with the X
       * connection and I/O errors, and make wait() hang.
       *
       * should probably call Hostinfo_ResetProcessState(), but that
       * does some stuff with iopl() we don't need
       */
      maxfd = sysconf(_SC_OPEN_MAX);
      for (i = STDERR_FILENO + 1; i < maxfd; i++) {
         if (i != fds[0] && i != fds[1]) {
            close(i);
         }
      }

      if (Signal_SetGroupHandler(cSignals, olds, ARRAYSIZE(cSignals),
#ifndef sun
                                 SIG_DFL
#else
                                 0
#endif
                                 ) == 0)
      {
         return FALSE;
      }

      close(fds[0]);
      ProcMgrWaiter(cmd, fds[1], &validExitCode, &exitCode);
      close(fds[1]);

      if (Signal_ResetGroupHandler(cSignals, olds, ARRAYSIZE(cSignals)) == 0) {
         return FALSE;
      }

      if (!validExitCode) {
         exitCode = 0;
      }

      exit(exitCode);
   }

   /*
    * Parent
    */

   close(fds[1]);

   asyncProc = malloc(sizeof(ProcMgr_AsyncProc));
   ASSERT_NOT_IMPLEMENTED(asyncProc);
   asyncProc->fd = fds[0];
   asyncProc->waiterPid = pid;
   asyncProc->validExitCode = FALSE;
   asyncProc->exitCode = -1;

   return asyncProc;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_IsProcessRunning --
 *
 *      Check to see if a pid is active
 *
 * Results:
 *
 *      TRUE if the process exists; FALSE otherwise
 *
 * Side effects:
 *
 *	Side effects
 *
 *----------------------------------------------------------------------
 */

static Bool
ProcMgr_IsProcessRunning(pid_t pid)
{
   /* 
    * if its not linux, assume its gone
    */
#if !defined(__FreeBSD__) && !defined(sun)
   char procname[256];
   int ret;
   struct stat st;

   snprintf(procname, sizeof(procname), "/proc/%"FMTPID, pid);

   /*
    * will fail if its gone or we don't have permission; both are
    * FALSE cases
    */
   ret = stat(procname, &st);
   if (0 == ret) {
      return TRUE;
   }
#endif
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgrKill --
 *
 *      Try to kill a pid & check every so often to see if it has died.
 *
 * Results:
 *
 *      TRUE if the process died; FALSE otherwise
 *
 * Side effects:
 *
 *	Side effects
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgrKill(pid_t pid,      // IN
            int sig,        // IN
            int timeout)    // IN: -1 will wait indefinitely
{
   if (kill(pid, sig) == -1) {
      Warning("Error trying to kill process %"FMTPID" with signal %d: %s\n",
              pid, sig, Msg_ErrString());
   } else {
      int i;

      /* Try every 100ms until we've reached the timeout */
      for (i = 0; timeout == -1 || i < timeout * 10; i++) {
         int ret;

         ret = waitpid(pid, NULL, WNOHANG);

         if (ret == -1) {
            /*
             * if we didn't start it, we can only check if its running
             * by looking in the proc table
             */
            if (ECHILD == errno) {
               if (ProcMgr_IsProcessRunning(pid)) {
                  Debug("Process %"FMTPID" is not a child, still running\n",
                        pid);
                  usleep(100000);
                  continue;
               }
               return TRUE;
            }
            Warning("Error trying to wait on process %"FMTPID": %s\n",
                    pid, Msg_ErrString());
         } else if (ret == 0) {
            usleep(100000);
         } else {
            Debug("Process %"FMTPID" died from signal %d on iteration #%d\n",
                  pid, sig, i);
            return TRUE;
         }
      }
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_KillByPid --
 *
 *      Terminate the process of procId.
 *
 * Results:
 *      
 *      None.
 *
 * Side effects:
 *
 *	Lots, depending on the program
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgr_KillByPid(ProcMgr_Pid procId)   // IN
{
   Bool success = TRUE;

   if (!ProcMgrKill(procId, SIGTERM, 5)) {
      success = ProcMgrKill(procId, SIGKILL, -1);
   }

   return success;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_Kill --
 *
 *      Kill a process synchronously by first attempty to do so
 *      nicely & then whipping out the SIGKILL axe.
 *
 * Results:
 *      
 *      None.
 *
 * Side effects:
 *
 *	None
 *
 *----------------------------------------------------------------------
 */

void
ProcMgr_Kill(ProcMgr_AsyncProc *asyncProc) // IN
{
   ASSERT(asyncProc);

   close(asyncProc->fd);

   ProcMgr_KillByPid(asyncProc->waiterPid);
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_GetAsyncStatus --
 *
 *      Get the return status of an async process.
 *
 * Results:
 *      
 *      TRUE if the status was retrieved.
 *      FALSE if it couldn't be retrieved.
 *
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgr_GetAsyncStatus(ProcMgr_AsyncProc *asyncProc, // IN
                       Bool *status)                 // OUT
{
   char buf[8];
   int bytesRead;
   int bytesTotal;
   char *helper;
   Bool retVal = FALSE;
   
   ASSERT(status);
   
   ASSERT(strlen(ASYNCEXEC_SUCCESS_IPC) == strlen(ASYNCEXEC_FAILURE_IPC));
   bytesTotal = strlen(ASYNCEXEC_SUCCESS_IPC) + 1 + sizeof(int);

   // Prevent buffer overflows.
   ASSERT(bytesTotal <= sizeof(buf));

   bytesRead = 0;
   while (bytesRead < bytesTotal) {
      int currentBytesRead;

      currentBytesRead = read(asyncProc->fd, buf + bytesRead,
			      sizeof(buf) - bytesRead);
      if (currentBytesRead <= 0) {
	 Warning("Error reading async process status (bytes read=%d)"
		 "Bytes read: %d\n",
		 currentBytesRead, bytesRead);
	 goto end;
      }
      bytesRead += currentBytesRead;
   }

   /* 
    * Coverity doesn't like it when we assume that buf is NUL-terminated.
    * This is a safe assumption for us because we control both ends of the
    * pipe. But let's humor Coverity and not make such assumptions.
    * This means we can't use strlen() to calculcate helper, and we can't
    * use strcmp() to compare buf to macros.
    */
   if (memcmp(buf, ASYNCEXEC_SUCCESS_IPC, 
              sizeof ASYNCEXEC_SUCCESS_IPC) == 0) {
      *status = TRUE;
      helper = buf + sizeof ASYNCEXEC_SUCCESS_IPC;
   } else if (memcmp(buf, ASYNCEXEC_FAILURE_IPC, 
                     sizeof ASYNCEXEC_FAILURE_IPC) == 0) {
      *status = FALSE;
      helper = buf + sizeof ASYNCEXEC_FAILURE_IPC;
   } else {
      *status = FALSE;
      Warning("Error reading async process status ('%s')\n", buf);
      goto end;
   }

   memcpy(&asyncProc->exitCode, helper, sizeof(asyncProc->exitCode));
   asyncProc->validExitCode = TRUE;

   Debug("Child w/ fd %x exited (msg='%s') with status=%d\n", asyncProc->fd,
         buf, *status);

   retVal = TRUE;

 end:
   close(asyncProc->fd);

   /* Read the pid so the processes don't become zombied */
   Debug("Waiting on pid %"FMTPID" to de-zombify it\n", asyncProc->waiterPid);
   waitpid(asyncProc->waiterPid, NULL, 0);

   return retVal;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_IsAsyncProcRunning --
 *
 *      Checks whether an async process is still running.
 *
 * Results:
 *
 *      TRUE iff the process is still running.
 *
 * Side effects:
 *
 *	     None.
 *
 *----------------------------------------------------------------------
 */

#if 0
Bool
ProcMgr_IsAsyncProcRunning(ProcMgr_AsyncProc *asyncProc) // IN
{
   static char buf[1024];
   int numBytesRead;
   
   ASSERT(asyncProc);
   
   numBytesRead = read(asyncProc->fd, buf, sizeof buf);
   if (numBytesRead <= 0) {
      return TRUE;
   }

   return FALSE;
}
   
#else
   
Bool
ProcMgr_IsAsyncProcRunning(ProcMgr_AsyncProc *asyncProc) // IN
{
   int maxFd;
   fd_set readFds;
   struct timeval tv;
   int status;

   ASSERT(asyncProc);
   
   /*
    * Do a select, not a read. This procedure may be called many times,
    * while polling another program. After it returns true, then the
    * watcher program will try to read the socket to get the IPC error
    * and the exit code.
    */
   FD_ZERO(&readFds);
   FD_SET(asyncProc->fd, &readFds);
   maxFd = asyncProc->fd;

   tv.tv_sec = 0;
   tv.tv_usec = 0;

   status = select(maxFd + 1, &readFds, NULL, NULL, &tv);
   if (status == -1) {
      return(FALSE); // Not running
   } else if (status > 0) {
      return(FALSE); // Not running
   } else {
      return(TRUE); // Still running
   }
}

#endif

/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_GetAsyncProcSelectable --
 *
 *      Get the selectable fd for an async proc struct.
 *
 * Results:
 *      
 *      The fd casted to a void *.
 *
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

Selectable
ProcMgr_GetAsyncProcSelectable(ProcMgr_AsyncProc *asyncProc)
{
   ASSERT(asyncProc);
   
   return asyncProc->fd;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_GetPid --
 *
 *      Get the pid for an async proc struct.
 *
 * Results:
 *      
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

ProcMgr_Pid
ProcMgr_GetPid(ProcMgr_AsyncProc *asyncProc)
{
   ASSERT(asyncProc);
   
   return asyncProc->waiterPid;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_GetExitCode --
 *
 *      Get the exit code status of an async process.
 *
 * Results:
 *      0 if successful, -1 if not.
 *
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
ProcMgr_GetExitCode(ProcMgr_AsyncProc *asyncProc,  // IN
                    int *exitCode)                 // OUT
{
   ASSERT(asyncProc);
   ASSERT(exitCode);

   if (!asyncProc->validExitCode) {
      Bool dummy;

      if (!ProcMgr_GetAsyncStatus(asyncProc, &dummy)) {
         *exitCode = -1;
         return(-1);
      }

      if (!(asyncProc->validExitCode)) {
         *exitCode = -1;
         return(-1);
      }
   }

   *exitCode = asyncProc->exitCode;
   return(0);
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_Free --
 *
 *      Discard the state of an async process.
 *
 * Results:
 *      None
 *
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
ProcMgr_Free(ProcMgr_AsyncProc *asyncProc) // IN
{
   free(asyncProc);
}

#ifdef linux

/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_ImpersonateUserStart --
 *
 *      Impersonate a user.  Much like bora/lib/impersonate, but
 *      changes the real and saved uid as well, to work with syscalls
 *      (access() and kill()) that look at real UID instead of effective.
 *
 *      Assumes it will be called as root.
 *
 * Results:
 *      TRUE on success
 *
 * Side effects:
 *
 *      Uid/gid set to given user, saved uid/gid left as root.
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgr_ImpersonateUserStart(const char *user,                      // IN
                  AuthToken token)                                  // IN
{
   char buffer[BUFSIZ];
   struct passwd pw;
   struct passwd *ppw = &pw;
   gid_t root_gid;
   int error;
   int ret;

   if ((error = getpwuid_r(0, &pw, buffer, sizeof(buffer), &ppw)) != 0 ||
       !ppw) {
      /*
       * getpwuid_r() and getpwnam_r() can return a 0 (success) but not
       * set the return pointer (ppw) if there's no entry for the user,
       * according to POSIX 1003.1-2003, so patch up the errno.
       */
      if (error == 0) {
         error = ENOENT;
      }
      return FALSE;
   }

   root_gid = ppw->pw_gid;

   if ((error = getpwnam_r(user, &pw, buffer, sizeof(buffer), &ppw)) != 0 ||
       !ppw) {
      if (error == 0) {
         error = ENOENT;
      }
      return FALSE;
   }

   // first change group
   ret = setresgid(ppw->pw_gid, ppw->pw_gid, root_gid);
   if (ret < 0) {
      Warning("Failed to setresgid() for user %s\n", user);
      return FALSE;
   }
   ret = initgroups(ppw->pw_name, ppw->pw_gid);
   if (ret < 0) {
      Warning("Failed to initgroups() for user %s\n", user);
      goto failure;
   }
   // now user
   ret = setresuid(ppw->pw_gid, ppw->pw_gid, 0);
   if (ret < 0) {
      Warning("Failed to setresuid() for user %s\n", user);
      goto failure;
   }

   // set env
   setenv("USER", ppw->pw_name, 1);
   setenv("HOME", ppw->pw_dir, 1);
   setenv("SHELL", ppw->pw_shell, 1);

   return TRUE;

failure:
   // try to restore on error
   ProcMgr_ImpersonateUserStop();

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcMgr_ImpersonateUserStop --
 *
 *      Stop impersonating a user and return to root.
 *
 * Results:
 *      TRUE on success
 *
 * Side effects:
 *
 *      Uid/gid restored to root.
 *
 *----------------------------------------------------------------------
 */

Bool
ProcMgr_ImpersonateUserStop(void)
{
   char buffer[BUFSIZ];
   struct passwd pw;
   struct passwd *ppw = &pw;
   int error;
   int ret;

   if ((error = getpwuid_r(0, &pw, buffer, sizeof(buffer), &ppw)) != 0 ||
       !ppw) {
      if (error == 0) {
         error = ENOENT;
      }
      return FALSE;
   }

   // first change back user
   ret = setresuid(ppw->pw_gid, ppw->pw_gid, 0);
   if (ret < 0) {
      Warning("Failed to setresuid() for root\n");
      return FALSE;
   }

   // now group
   ret = setresgid(ppw->pw_gid, ppw->pw_gid, ppw->pw_gid);
   if (ret < 0) {
      Warning("Failed to setresgid() for root\n");
      return FALSE;
   }
   ret = initgroups(ppw->pw_name, ppw->pw_gid);
   if (ret < 0) {
      Warning("Failed to initgroups() for root\n");
      return FALSE;
   }

   // set env
   setenv("USER", ppw->pw_name, 1);
   setenv("HOME", ppw->pw_dir, 1);
   setenv("SHELL", ppw->pw_shell, 1);

   return TRUE;
}

#endif // linux
