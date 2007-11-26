/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
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
 *
 *********************************************************/

/*
 * idLinux.c --
 *
 *   uid/gid helpers.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/socket.h>
#include <Security/Authorization.h>
#endif

#include "vmware.h"
#include "su.h"

#if defined(__linux__)
#ifndef GLIBC_VERSION_21
/*
 * SYS_ constants for glibc 2.0, some of which may already be defined on some of those
 * older systems.
 */
#ifndef SYS_setresuid
#define SYS_setresuid          164
#endif
#ifndef SYS_setresgid
#define SYS_setresgid          170
#endif
#define SYS_setreuid32         203
#define SYS_setregid32         204
#define SYS_setresuid32        208
#define SYS_setresgid32        210
#define SYS_setuid32           213
#define SYS_setgid32           214
#endif // ifndef GLIBC_VERSION_21

/*
 * 64bit linux has no 16 bit versions and
 * the 32bit versions have the un-suffixed names.
 * And obviously, we're not using glibc 2.0
 * for our 64bit builds!
 */
#ifdef VM_X86_64
#define SYS_setreuid32         (abort(), 0)
#define SYS_setregid32         (abort(), 0)
#define SYS_setresuid32        (abort(), 0)
#define SYS_setresgid32        (abort(), 0)
#define SYS_setuid32           (abort(), 0)
#define SYS_setgid32           (abort(), 0)
#endif

/*
 * On 32bit systems:
 * Set to 1 when system supports 32bit uids, cleared to 0 when system
 * supports 16bit calls only.  By default we assume that 32bit uids
 * are supported and clear this flag on first failure.
 *
 * On 64bit systems:
 * Only the 32bit uid syscalls exist, but they do not have the names
 * with the '32' suffix, so we get the behaviour we want by forcing
 * the code to use the unsuffixed syscalls.
 */
#ifdef VM_X86_64
static int uid32 = 0;
#else
static int uid32 = 1;
#endif
#endif // __linux__

#if defined(__APPLE__)
#include <sys/kauth.h>
#endif


#if !defined(__APPLE__) && !defined(sun) && !defined(__FreeBSD__)
/*
 *----------------------------------------------------------------------------
 *
 * Id_SetUid --
 *
 *	If calling thread has euid = 0, it sets real, effective and saved uid
 *	to the specified value.
 *	If calling thread has euid != 0, then only effective uid is set.
 *
 * Results:
 *      0 on success, -1 on failure, errno set
 *
 * Side effects:
 *      errno may be modified on success
 *
 *----------------------------------------------------------------------------
 */

int
Id_SetUid(uid_t euid)		// IN: new euid
{
   if (uid32) {
      int r = syscall(SYS_setuid32, euid);
      if (r != -1 || errno != ENOSYS) {
         return r;
      }
      uid32 = 0;
   }
   return syscall(SYS_setuid, euid);
}
#endif


/*
 *----------------------------------------------------------------------------
 *
 * Id_SetGid --
 *
 *      If calling thread has euid = 0, it sets real, effective and saved gid
 *	to the specified value.
 *	If calling thread has euid != 0, then only effective gid is set.
 *
 * Results:
 *      0 on success, -1 on failure, errno set
 *
 * Side effects:
 *      errno may be modified on success
 *
 *----------------------------------------------------------------------------
 */

int
Id_SetGid(gid_t egid)		// IN: new egid
{
#if defined(__APPLE__)
   Warning("XXXMACOS: implement %s\n", __func__);
   return -1;
#elif defined(sun)
   Warning("XXXSolaris: implement %s\n", __FUNCTION__);
   return -1;
#elif defined(__FreeBSD__)
   Warning("XXXFreeBSD: implement %s\n", __FUNCTION__);
   return -1;
#else
   if (uid32) {
      int r = syscall(SYS_setgid32, egid);
      if (r != -1 || errno != ENOSYS) {
         return r;
      }
      uid32 = 0;
   }
   return syscall(SYS_setgid, egid);
#endif
}


/*
 *----------------------------------------------------------------------------
 *
 * Id_SetRESUid --
 *
 *      Sets uid, euid and saved uid to the specified values.  You can use -1
 *	for values which should not change.
 *
 * Results:
 *      0 on success, -1 on failure, errno set
 *
 * Side effects:
 *      errno may be modified on success
 *
 *----------------------------------------------------------------------------
 */

int
Id_SetRESUid(uid_t uid,		// IN: new uid
	     uid_t euid,	// IN: new effective uid
	     uid_t suid)	// IN: new saved uid
{
#if defined(__APPLE__)
   Warning("XXXMACOS: implement %s\n", __func__);
   return -1;
#elif defined(sun)
   Warning("XXXSolaris: implement %s\n", __FUNCTION__);
   return -1;
#elif defined(__FreeBSD__)
   Warning("XXXFreeBSD: implement %s\n", __FUNCTION__);
   return -1;
#else
   if (uid32) {
      int r = syscall(SYS_setresuid32, uid, euid, suid);
      if (r != -1 || errno != ENOSYS) {
         return r;
      }
      uid32 = 0;
   }
   return syscall(SYS_setresuid, uid, euid, suid);
#endif
}


#if !defined(__APPLE__)
/*
 *----------------------------------------------------------------------------
 *
 * Id_SetRESGid --
 *
 *      Sets gid, egid and saved gid to the specified values.  You can use -1
 *      for values which should not change.
 *
 * Results:
 *      0 on success, -1 on failure, errno set
 *
 * Side effects:
 *      errno may be modified on success
 *
 *----------------------------------------------------------------------------
 */

int
Id_SetRESGid(gid_t gid,		// IN: new gid
	     gid_t egid,	// IN: new effective gid
	     gid_t sgid)	// IN: new saved gid
{
#ifdef sun
   Warning("XXXSolaris: implement %s\n", __FUNCTION__);
   return -1;
#elif defined(__FreeBSD__)
   Warning("XXXFreeBSD: implement %s\n", __FUNCTION__);
   return -1;
#else
   if (uid32) {
      int r = syscall(SYS_setresgid32, gid, egid, sgid);
      if (r != -1 || errno != ENOSYS) {
         return r;
      }
      uid32 = 0;
   }
   return syscall(SYS_setresgid, gid, egid, sgid);
#endif
}
#endif


/*
 *----------------------------------------------------------------------------
 *
 * Id_SetREUid --
 *
 *      Sets uid and euid to the specified values.  You can use -1
 *      for values which should not change.  If you are changing uid,
 *      or if you are changing euid to value which differs from old uid,
 *      then saved uid is updated to new euid value.
 *
 * Results:
 *      0 on success, -1 on failure, errno set
 *
 * Side effects:
 *      errno may be modified on success
 *
 *----------------------------------------------------------------------------
 */

int
Id_SetREUid(uid_t uid,		// IN: new uid
	    uid_t euid)		// IN: new effective uid
{
#if defined(__APPLE__)
   Warning("XXXMACOS: implement %s\n", __func__);
   return -1;
#elif defined(sun)
   Warning("XXXSolaris: implement %s\n", __FUNCTION__);
   return -1;
#elif defined(__FreeBSD__)
   Warning("XXXFreeBSD: implement %s\n", __FUNCTION__);
   return -1;
#else
   if (uid32) {
      int r = syscall(SYS_setreuid32, uid, euid);
      if (r != -1 || errno != ENOSYS) {
         return r;
      }
      uid32 = 0;
   }
   return syscall(SYS_setreuid, uid, euid);
#endif
}


#if !defined(__APPLE__) && !defined(sun) && !defined(__FreeBSD__)
/*
 *----------------------------------------------------------------------------
 *
 * Id_SetREGid --
 *
 *      Sets gid and egid to the specified values.  You can use -1
 *      for values which should not change.  If you are changing gid,
 *      or if you are changing egid to value which differs from old gid,
 *      then saved gid is updated to new egid value.
 *
 * Results:
 *      0 on success, -1 on failure, errno set
 *
 * Side effects:
 *      errno may be modified on success
 *
 *----------------------------------------------------------------------------
 */

int
Id_SetREGid(gid_t gid,		// IN: new gid
	    gid_t egid)		// IN: new effective gid
{
   if (uid32) {
      int r = syscall(SYS_setregid32, gid, egid);
      if (r != -1 || errno != ENOSYS) {
         return r;
      }
      uid32 = 0;
   }
   return syscall(SYS_setregid, gid, egid);
}
#endif


#if defined(__APPLE__)
/*
 *----------------------------------------------------------------------------
 *
 * Id_SetSuperUser --
 *
 *      If the calling process does not have euid root, do nothing.
 *      If the calling process has euid root, make the calling thread acquire
 *      or release euid root.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

void
Id_SetSuperUser(Bool yes) // IN: TRUE to acquire super user, FALSE to release
{
   if (!IsSuperUser() == !yes) {
      // settid(2) fails on spurious transitions.
      return;
   }

   if (yes) {
      syscall(SYS_settid, KAUTH_UID_NONE, KAUTH_GID_NONE /* Ignored. */);
   } else {
      if (syscall(SYS_settid, getuid(), getgid()) == -1) {
         Log("Failed to release super user privileges.\n");
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * IdAuthCreate --
 *
 *      Create an Authorization session.
 *
 *      An Authorization session remembers which process name and which
 *      credentials created it, and how much time has elapsed since it last
 *      prompted the user at the console to authenticate to grant the
 *      Authorization session a specific right.
 *
 * Results:
 *      On success: A ref to the Authorization session.
 *      On failure: NULL.
 *
 * Side effects:
 *      The current process is forked.
 *
 *-----------------------------------------------------------------------------
 */

static AuthorizationRef
IdAuthCreate(void)
{
   int fds[2] = { -1, -1, };
   pid_t child;
   AuthorizationRef auth = NULL;
   struct {
      Bool success;
      AuthorizationExternalForm ext;
   } data;
   uint8 buf;

   /*
    * XXX One more Apple bug related to thread credentials:
    *     AuthorizationCreate() incorrectly uses process instead of thread
    *     credentials. So for this code to properly work in the VMX for
    *     example, we must do this elaborate fork/handshake dance. Fortunately
    *     this function is only called once very early when a process starts.
    */

   if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
      Warning("%s: socketpair() failed.\n", __func__);
      goto out;
   }

   child = fork();
   if (child < 0) {
      Warning("%s: fork() failed.\n", __func__);
      goto out;
   }

   if (child) {
      size_t rcvd;
      int status;
      pid_t result;

      // Parent: use fds[0]

      // Wait until the child has created its process ref to the auth session.
      for (rcvd = 0; rcvd < sizeof data; ) {
         ssize_t actual;

         actual = read(fds[0], (void *)&data + rcvd, sizeof data - rcvd);
         ASSERT(actual <= sizeof data - rcvd);
         if (actual < 0) {
            ASSERT(errno == EPIPE);
            Warning("%s: parent read() failed because child died.\n",
                    __func__);
            data.success = FALSE;
            break;
         }

         rcvd += actual;
      }

      if (data.success) {
         if (AuthorizationCreateFromExternalForm(&data.ext, &auth)
             != errAuthorizationSuccess) {
            Warning("%s: parent AuthorizationCreateFromExternalForm() "
                    "failed.\n", __func__);
         }
      }

      // Tell the child it can now destroy its process ref to the auth session.
      write(fds[0], &buf, sizeof buf);

      // Reap the child, looping if we get interrupted by a signal.
      do {
         result = waitpid(child, &status, 0);
      } while (result == -1 && errno == EINTR);

      ASSERT_NOT_IMPLEMENTED(result == child);
   } else {
      // Child: use fds[1]

      data.success = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                         kAuthorizationFlagDefaults, &auth)
                     == errAuthorizationSuccess;
      if (data.success) {
         data.success = AuthorizationMakeExternalForm(auth, &data.ext)
                        == errAuthorizationSuccess;
         if (!data.success) {
            Warning("%s: child AuthorizationMakeExternalForm() failed.\n",
                    __func__);
         }
      } else {
         Warning("%s: child AuthorizationCreate() failed.\n", __func__);
      }

      // Tell the parent it can now create a process ref to the auth session.
      if (write(fds[1], &data, sizeof data) == sizeof data) {
         /*
          * Wait until the child can destroy its process ref to the auth
          * session.
          */
         for (;;) {
            ssize_t actual;

            actual = read(fds[1], &buf, sizeof buf);
            ASSERT(actual <= sizeof buf);
            if (actual) {
               break;
            }
         }
      }

      /*
       * This implicitly:
       * o Destroys the child process ref to the Authorization session.
       * o Closes fds[0] and fds[1]
       */
      exit(0);
   }

out:
   close(fds[0]);
   close(fds[1]);

   return auth;
}


static AuthorizationRef procAuth = NULL;


/*
 *-----------------------------------------------------------------------------
 *
 * IdAuthGet --
 *
 *      Get a ref to the process' Authorization session.
 *
 *      Not thread-safe.
 *
 * Results:
 *      On success: The ref.
 *      On failure: NULL.
 *
 * Side effects:
 *      If the process' Authorization session does not exist yet, it is
 *      created.
 *
 *-----------------------------------------------------------------------------
 */

static AuthorizationRef
IdAuthGet(void)
{
   if (!procAuth) {
      procAuth = IdAuthCreate();
   }

   return procAuth;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Id_AuthGetLocal --
 *
 *      Get a local ref to the process' Authorization session.
 *
 *      Not thread-safe.
 *
 * Results:
 *      On success: The ref.
 *      On failure: NULL.
 *
 * Side effects:
 *      If the process' Authorization session does not exist yet, it is
 *      created.
 *
 *-----------------------------------------------------------------------------
 */

void *
Id_AuthGetLocal(void)
{
   return (void *)IdAuthGet();
}


/*
 *-----------------------------------------------------------------------------
 *
 * Id_AuthGetExternal --
 *
 *      Get a cross-process ref to the process' Authorization session.
 *
 *      Not thread-safe.
 *
 * Results:
 *      On success: An allocated cross-process ref.
 *      On failure: NULL.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void *
Id_AuthGetExternal(size_t *size) // OUT
{
   AuthorizationRef auth;
   AuthorizationExternalForm *ext;

   auth = IdAuthGet();
   if (!auth) {
      return NULL;
   }

   ext = malloc(sizeof *ext);
   if (!ext) {
      Warning("Unable to allocate an AuthorizationExternalForm.\n");
      return NULL;
   }

   if (AuthorizationMakeExternalForm(auth, ext) != errAuthorizationSuccess) {
      Warning("AuthorizationMakeExternalForm() failed.\n");
      free(ext);
      return NULL;
   }

   *size = sizeof *ext;
   return ext;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Id_AuthSet --
 *
 *      Set the process' Authorization session to the Authorization session
 *      referred to by a cross-process ref.
 *
 *      Not thread-safe.
 *
 * Results:
 *      On success: TRUE.
 *      On failure: FALSE.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Id_AuthSet(void const *buf, // IN
           size_t size)     // IN
{
   AuthorizationExternalForm const *ext =
      (AuthorizationExternalForm const *)buf;

   if (!buf || size != sizeof *ext) {
      Warning("%s: Invalid argument.\n", __func__);
      return FALSE;
   }

   ASSERT(!procAuth);
   if (AuthorizationCreateFromExternalForm(ext, &procAuth)
       != errAuthorizationSuccess) {
      Warning("AuthorizationCreateFromExternalForm failed.\n");
      return FALSE;
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Id_AuthCheck --
 *
 *      Check if 'right' is granted to the process' Authorization session.
 *
 *      Not thread-safe.
 *
 * Results:
 *      On success: TRUE is granted.
 *      On failure: FALSE if not granted.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Id_AuthCheck(char const *right) // IN
{
   AuthorizationRef auth;
   AuthorizationItem items[1];
   AuthorizationRights rights;

   auth = IdAuthGet();
   if (!auth) {
      return FALSE;
   }

   items[0].name = right;
   items[0].valueLength = 0;
   items[0].value = NULL;
   items[0].flags = 0;
   rights.items = items;
   rights.count = ARRAYSIZE(items);

   return AuthorizationCopyRights(auth, &rights,
             kAuthorizationEmptyEnvironment,
             kAuthorizationFlagDefaults |
             kAuthorizationFlagInteractionAllowed | 
             kAuthorizationFlagExtendRights,
             NULL) == errAuthorizationSuccess;
}
#endif
