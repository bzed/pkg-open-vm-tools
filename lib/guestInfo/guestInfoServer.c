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
 * guestInfoServer.c --
 *
 *      This is the implementation of the common code in the guest tools
 *      to send out guest information to the host. The guest info server
 *      is currently a thread spawned by the tools daemon which periodically
 *      gathers all guest information and sends updates to the host if required.
 *      This file implements the platform independent framework for this.
 *      A separate thread is only spawned for Windows guests, currently.
 */


#ifndef VMX86_DEVEL

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#include <winbase.h>       /* For Sleep() */
#include <memory.h>
#define SleepFunction(x) Sleep((uint32)ceil((double)(x) / 1000))
#else
/* SleepFunction() for Posix defined below; these includes are needed. */
#include <sys/poll.h>
#endif

#include "vmware.h"
#include "eventManager.h"
#include "debug.h"
#include "str.h"
#include "rpcout.h"
#include "rpcvmx.h"
#include "guestInfo.h"
#include "guestApp.h"
#include "guestInfoServer.h"
#include "guestInfoInt.h"
#include "buildNumber.h"
#include "system.h"
#include "wiper.h" // for WiperPartition functions

#define GUESTINFO_DEFAULT_DELIMITER ' '

/*
 * Stores information about all guest information sent to the vmx.
 */

typedef struct _GuestInfoCache{
   char value[INFO_MAX][MAX_VALUE_LEN]; /* Stores values of all key-value pairs. */
   NicInfo nicInfo;
   DiskInfo diskInfo;
} GuestInfoCache;


/*
 * Local variables.
 */
static uint32 gDisableQueryDiskInfo = FALSE;

static DblLnkLst_Links *gGuestInfoEventQueue;
static uint32 gTimerInterval;
/* Local cache of the guest information that was last sent to vmx. */
static GuestInfoCache gInfoCache;

/* 
 * A boolean flag that specifies whether the state of the VM was
 * changed since the last time guest info was sent to the VMX.
 * Tools daemon sets it to TRUE after the VM was resumed.
 */

static Bool vmResumed;

/* 
 * The Windows Guest Info Server runs in a separate thread,
 * so we have to synchronize access to 'vmResumed' variable.
 * Non-windows guest info server does not run in a separate
 * thread, so no locking is needed.
 */

#if defined(_WIN32)
typedef CRITICAL_SECTION vmResumedLockType;
#define GUESTINFO_DELETE_LOCK(lockPtr) DeleteCriticalSection(lockPtr)
#define GUESTINFO_ENTER_LOCK(lockPtr) EnterCriticalSection(lockPtr)
#define GUESTINFO_LEAVE_LOCK(lockPtr) LeaveCriticalSection(lockPtr)
#define GUESTINFO_INIT_LOCK(lockPtr) InitializeCriticalSection(lockPtr)

vmResumedLockType vmResumedLock;

#else // #if LINUX

typedef int vmResumedLockType;
#define GUESTINFO_DELETE_LOCK(lockPtr) 
#define GUESTINFO_ENTER_LOCK(lockPtr)
#define GUESTINFO_LEAVE_LOCK(lockPtr)
#define GUESTINFO_INIT_LOCK(lockPtr)

#endif // #if LINUX

static Bool GuestInfoGather(void * clientData);
static Bool GuestInfoUpdateVmdb(GuestInfoType infoType, void* info);
static Bool SetGuestInfo(GuestInfoType key, const char* value, char delimiter);
static Bool NicInfoChanged(PNicInfo nicInfo);
static Bool DiskInfoChanged(PDiskInfo diskInfo);
static void GuestInfoClearCache(void);
static int PrintNicInfo(PNicInfo nicInfo, int (*PrintFunc)(const char *, ...));

#ifdef _WIN32

/*
 *-----------------------------------------------------------------------------
 *
 * GuestInfoServer_Main --
 *
 *    The main event loop for the guest info server.
 *    GuestInfoServer_Init() much be called prior to calling this function.
 *
 * Result
 *    None
 *
 * Side-effects
 *    Events are processed, information gathered and updates sent.
 *
 *-----------------------------------------------------------------------------
 */

void
GuestInfoServer_Main(void *data) // IN
{
   int retVal;
   uint64 sleepUsecs;
   HANDLE *events = (HANDLE *) data;
   HANDLE quitEvent;
   HANDLE finishedEvent;

   ASSERT(data);
   ASSERT(events[0]);
   ASSERT(events[1]);

   quitEvent = events[0];
   finishedEvent = events[1];

   Debug("Starting GuestInfoServer for Windows.\n");
   for(;;) {
      DWORD dwError;

      retVal = EventManager_ProcessNext(gGuestInfoEventQueue, &sleepUsecs);
      if (retVal != 1) {
         Debug("Unexpected end of the guest info loop.\n");
         break;
      }

      /*
       * The number of micro seconds to sleep should not overflow a long. This
       * corresponds to a maximum sleep time of around 4295 seconds (~ 71 minutes)
       * which should be more than enough.
       */

      Debug("Sleeping for %"FMT64"u msecs...\n", sleepUsecs / 1000);
      dwError = WaitForSingleObject(quitEvent, sleepUsecs / 1000);
      if (dwError == WAIT_OBJECT_0) {
         GuestApp_Log("GuestInfoServer received quit event.\n");
         Debug("GuestInfoServer received quit event.\n");
         break;
      } else if (dwError == WAIT_TIMEOUT) {
         Debug("GuestInfoServer woke up.\n");
      } else if (dwError == WAIT_FAILED) {
         Debug("GuestInfoServer error waiting on exit event: %d %d\n",
               dwError, GetLastError());
         break;
      }
   }
   SetEvent(finishedEvent);
   GuestApp_Log("GuestInfoServer exiting.\n");
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * GuestInfoServer_Init --
 *
 *    This function must be called before the guest info thread is running.
 *    Initialize the event queue. If an event queue has been supplied, just
 *    add the first event to it. If not, create an event queue and then add
 *    an event to this queue. Initialize vmResumedLock. Even if the function
 *    fails, the lock is initialized anyway since the main thread calls
 *    GuestInfoServer_VMResumedNotify() regardless of whether the guest info
 *    thread was started successfully.
 *    
 *    Call GuestInfoServer_Cleanup() to do the necessary cleanup after the
 *    guest info thread has finished running.
 *
 * Result
 *    TRUE on success, FALSE on failure.
 *
 * Side-effects
 *    The timer event queue is initialized and populated with the first event.
 *    Lock is created for synchronized access to vmResumed variable.
 *
 *-----------------------------------------------------------------------------
 */

Bool
GuestInfoServer_Init(DblLnkLst_Links *eventQueue) // IN: queue for event loop
{
   Debug("Entered guest info init.\n");

   memset(&gInfoCache, 0, sizeof gInfoCache);

   GUESTINFO_INIT_LOCK(&vmResumedLock);
   GUESTINFO_ENTER_LOCK(&vmResumedLock);
   vmResumed = FALSE;
   GUESTINFO_LEAVE_LOCK(&vmResumedLock);

   gGuestInfoEventQueue = eventQueue ? eventQueue: EventManager_Init();
   if(!gGuestInfoEventQueue) {
      Debug("Unable to create the event queue.\n");
      return FALSE;
   }

   /*
    * Get the timer interval.
    * XXX: A default value of 30 seconds is acceptable to the VPX team
    *      This value should however be made configurable.
    */

   gTimerInterval = 3000;     /* 30 seconds. */

   /* Add the first timer event. */
   if (!EventManager_Add(gGuestInfoEventQueue, gTimerInterval, GuestInfoGather, NULL)) {
      Debug("Unable to add initial event.\n");
      return FALSE;
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * GuestInfoServer_DisableDiskInfoQuery --
 *
 *    Set whether to disable/enable querying disk information.
 *    This function is required to provide a work around for bug number 94434.
 *    On Win 9x/ME querying for the disk information prevents the machine from 
 *    entering standby. So we added a configuration option diable-query-diskinfo 
 *    for the tools.conf file. We use this function to let the guestd & tools service
 *    control the disabling/enabling of disk information querying.
 *    
 * Result
 *    None.
 *
 * Side-effects
 *    GuestInfoGather ignores disk information querying if this is called with TRUE.
 *
 *-----------------------------------------------------------------------------
 */

void
GuestInfoServer_DisableDiskInfoQuery(Bool disable)
{
   gDisableQueryDiskInfo = disable; 
}


/*
 *-----------------------------------------------------------------------------
 *
 * GuestInfoServer_Cleanup --
 *
 *    Cleanup initialized values.
 *
 * Result
 *    None.
 *
 * Side-effects
 *    Timer event queue is destroyed.
 *    Deallocate any memory allocated in gInfoCache.
 *    vmResumedLock is deleted.
 *
 *-----------------------------------------------------------------------------
 */

void
GuestInfoServer_Cleanup(void)
{
   GuestInfoClearCache();
   if (gGuestInfoEventQueue) {
      EventManager_Destroy(gGuestInfoEventQueue);
   }
   GUESTINFO_DELETE_LOCK(&vmResumedLock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * GuestInfoServer_VMResumedNotify --
 *
 *    Called by the tools daemon to notify of the VM's state change.
 *    Right now this function is called after the VM was resumed.
 *
 * Result
 *    None.
 *
 * Side-effects
 *    vmResumed is set to TRUE.
 *
 *-----------------------------------------------------------------------------
 */

void
GuestInfoServer_VMResumedNotify(void)
{
   GUESTINFO_ENTER_LOCK(&vmResumedLock);
   vmResumed = TRUE;
   GUESTINFO_LEAVE_LOCK(&vmResumedLock);
}


#ifndef _WIN32
/*
 *----------------------------------------------------------------------------
 *
 * SleepFunction --
 *
 *    Sleeps in milliseconds. (Posix)
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static inline void
SleepFunction(uint64 tmout)
{
   if (tmout >= INT_MAX * CONST64U(1000)) {
      tmout = INT_MAX;
   } else {
      tmout = (tmout + 999) / 1000;
   }

   poll(NULL, 0, tmout);
}
#endif




/*
 *-----------------------------------------------------------------------------
 *
 * GuestInfoGather --
 *
 *    Periodically collects all the desired guest information and updates VMDB.
 *
 * Result
 *    TRUE always. Even if some of the values were not updated, continue running.
 *
 * Side-effects
 *    VMDB is updated if the given value has changed.
 *
 *-----------------------------------------------------------------------------
 */

Bool
GuestInfoGather(void *clientData)   // IN: unused
{
   char name[255];
   char osNameFull[MAX_VALUE_LEN];
   char osName[MAX_VALUE_LEN];
   NicInfo nicInfo;
   DiskInfo diskInfo;
   
   Debug("Entered guest info gather.\n");
   
   /* Send tools version. */
   if (!GuestInfoUpdateVmdb(INFO_TOOLS_VERSION, BUILD_NUMBER)) {
      /*
       * An older vmx talking to new tools wont be able to handle
       * this message. Continue, if thats the case.
       */

      Debug("Failed to update VMDB with tools version.\n");
   }

   /* Gather all the relevant guest information. */
   if (!GuestInfoGetOSName(sizeof osNameFull, sizeof osName, osNameFull, osName)) {
      Debug("Failed to get OS info.\n");
   } else {
      if (!GuestInfoUpdateVmdb(INFO_OS_NAME_FULL, osNameFull)) {
         Debug("Failed to update VMDB\n");
      }
      if (!GuestInfoUpdateVmdb(INFO_OS_NAME, osName)) {
         Debug("Failed to update VMDB\n");
      }
   }
   
   if (!gDisableQueryDiskInfo) {
      if (!GuestInfoGetDiskInfo(&diskInfo)) {
         Debug("Failed to get disk info.\n");
      } else {
         if (!GuestInfoUpdateVmdb(INFO_DISK_FREE_SPACE, &diskInfo)) {
            Debug("Failed to update VMDB\n.");
         }
      }
      /* Free memory allocated in GuestInfoGetDiskInfo. */
      if (diskInfo.partitionList != NULL) {
         free(diskInfo.partitionList);
         diskInfo.partitionList = NULL;
      }
   }   
   
   if(!GuestInfoGetFqdn(sizeof name, name)) {
         Debug("Failed to get netbios name.\n");
   } else {
      if (!GuestInfoUpdateVmdb(INFO_DNS_NAME, name)) {
         Debug("Failed to update VMDB.\n");
      }
   }

   /* Get NIC information. */
   if (!GuestInfoGetNicInfo(&nicInfo)) {
      Debug("Failed to get nic info.\n");
   } else {
      if (!GuestInfoUpdateVmdb(INFO_IPADDRESS, &nicInfo)) {
         Debug("Failed to update VMDB.\n");
      }
   }

   /* Send the uptime to VMX so that it can detect soft resets. */
   if (!GuestInfoServer_SendUptime()) {
      Debug("Failed to update VMDB with uptime.\n");
   }

   /* 
    * Even if one of the updates was unsuccessfull, 
    * we still add the next timer event. This way
    * if one of the pieces failed, other information will
    * still be passed to the host.
    *
    */
   if (!EventManager_Add(gGuestInfoEventQueue, gTimerInterval, GuestInfoGather, NULL)) {
      Debug("GuestInfoGather: Unable to add next event.\n");
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * GuestInfoUpdateVmdb --
 *
 *    Update VMDB with new guest information.
 *    This is the only function that should need to change when the VMDB pipe
 *    is implemented. Since we dont currently have a VMDB instance in the guest
 *    the function updates the VMDB instance on the host. Updates are sent only
 *    if the values have changed.
 *
 * Result
 *    TRUE on success, FALSE on failure.
 *
 * Side-effects
 *    VMDB is updated if the given value has changed.
 *
 *-----------------------------------------------------------------------------
 */


Bool
GuestInfoUpdateVmdb(GuestInfoType infoType, // IN: guest information type
                    void *info)             // IN: type specific information
{
   Bool resumed = FALSE;
   
   ASSERT(info);
   Debug("Entered update vmdb.\n");

   GUESTINFO_ENTER_LOCK(&vmResumedLock);
   if (vmResumed) {
      resumed = vmResumed;
      vmResumed = FALSE;
   }
   GUESTINFO_LEAVE_LOCK(&vmResumedLock);

   if (resumed) {
      GuestInfoClearCache();
   }

   switch (infoType) {
   case INFO_DNS_NAME:
   case INFO_TOOLS_VERSION:
   case INFO_OS_NAME:
   case INFO_OS_NAME_FULL:
   case INFO_UPTIME:
      /*
       * This is one of our key value pairs. Update it if it has changed.
       * Above fall-through is intentional.
       */

      if (strcmp(gInfoCache.value[infoType], (char *)info) == 0) {
         /* The value has not changed */
         Debug("Value unchanged for infotype %d.\n", infoType);
         break;
      }

      if(!SetGuestInfo(infoType, (char *)info, 0)) {
         Debug("Failed to update key/value pair for type %d.\n", infoType);
         return FALSE;
      }

      /* Update the value in the cache as well. */
      Str_Strcpy(gInfoCache.value[infoType], (char *)info, MAX_VALUE_LEN);
      break;

   case INFO_IPADDRESS:
      {
         char request[sizeof (NicInfo) + sizeof GUEST_INFO_COMMAND + 2 +
                      3 * sizeof ' ']; /* 2 bytes are for digits of infotype. */
         char *reply;
         size_t replyLen;
         Bool status;

         if (!NicInfoChanged((PNicInfo)info)) {
            Debug("GuestInfo: Nic info not changed.\n");
            break;
         }
         Debug("Creating nic info message.\n");
         Str_Sprintf(request, sizeof request, "%s  %d ", GUEST_INFO_COMMAND,
                     INFO_IPADDRESS);
         memcpy(request + strlen(request), info, sizeof(NicInfo));

         Debug("GuestInfo: Sending nic info message.\n");
         /* Send all the information in the message. */
         status = RpcOut_SendOneRaw(request, sizeof request, &reply, &replyLen);

         Debug("GuestInfo: Just sent nic info message.\n");
         if (!status || (strncmp(reply, "", 1) != 0)) {
            Debug("Failed to update nic information\n");
            free(reply);
            return FALSE;
         }

         if (RpcVMX_ConfigGetBool(FALSE, "printNicInfo")) {
            PrintNicInfo((PNicInfo) info, (int (*)(const char *fmt, ...)) RpcVMX_Log);
         }

         Debug("GuestInfo: Updated NIC information\n");
         free(reply);

         /* Update the cache. */
         gInfoCache.nicInfo = *(PNicInfo)info;
         break;
      }
   case INFO_DISK_FREE_SPACE:
      {
         /*
          * As above, 2 accounts for the digits of infotype and 3 for the three
          * spaces.
          */
         unsigned int requestSize = sizeof GUEST_INFO_COMMAND + 2 + 3 * sizeof (char);
         uint8 partitionCount;
         size_t offset;
         char *request;
         char *reply;
         size_t replyLen;
         Bool status;
         PDiskInfo pdi = (PDiskInfo)info;
         int j = 0;

         if (!DiskInfoChanged(pdi)) {
            Debug("GuestInfo: Disk info not changed.\n");
            break;
         }
         
         requestSize += sizeof pdi->numEntries +
                        sizeof *pdi->partitionList * pdi->numEntries;
         request = (char *)calloc(requestSize, sizeof (char));
         if (request == NULL) {
            Debug("GuestInfo: Could not allocate memory for request.\n");
            break;
         }

         Str_Sprintf(request, requestSize, "%s  %d ", GUEST_INFO_COMMAND, 
                     INFO_DISK_FREE_SPACE);
            
         /* partitionCount is a uint8 and cannot be larger than UCHAR_MAX. */
         if (pdi->numEntries > UCHAR_MAX) {
            Debug("GuestInfo: Too many partitions.\n");
            free(request);
            return FALSE;
         }
         partitionCount = pdi->numEntries;

         offset = strlen(request);

         /*
          * Construct the disk information message to send to the host.  This
          * contains a single byte indicating the number partitions followed by
          * the PartitionEntry structure for each one.
          *
          * Note that the use of a uint8 to specify the partitionCount is the
          * result of a bug (see bug 117224) but should not cause a problem
          * since UCHAR_MAX is 255.  Also note that PartitionEntry is packed so
          * it's safe to send it from 64-bit Tools to a 32-bit VMX, etc.
          */
         memcpy(request + offset, &partitionCount, sizeof partitionCount);
         memcpy(request + offset + sizeof partitionCount, pdi->partitionList,
                sizeof *pdi->partitionList * pdi->numEntries);
   
         Debug("sizeof request is %d\n", requestSize);
         status = RpcOut_SendOneRaw(request, requestSize, &reply, &replyLen);

         if (!status || (strncmp(reply, "", 1) != 0)) {
            Debug("Failed to update disk information.\n");
            free(request);
            free(reply);
            return FALSE;
         }

         Debug("GuestInfo: Updated disk info information\n");
         free(reply);
         free(request);

         /* Free any memory previously allocated in the cache. */
         if (gInfoCache.diskInfo.partitionList != NULL) {
            free(gInfoCache.diskInfo.partitionList);
            gInfoCache.diskInfo.partitionList = NULL;
         }
         gInfoCache.diskInfo.numEntries = pdi->numEntries;
         gInfoCache.diskInfo.partitionList = calloc(pdi->numEntries, 
                                                         sizeof(PartitionEntry));
         if (gInfoCache.diskInfo.partitionList == NULL) {
            Debug("GuestInfo: could not allocate memory for the disk info cache.\n");
            return FALSE; 
         }

         for (j = 0; j < pdi->numEntries; j++) {
            gInfoCache.diskInfo.partitionList[j] = pdi->partitionList[j];
         }
         break;
      }
   default:
      Debug("GuestInfo: Invalid info type.\n");
      NOT_REACHED();
      break;
   }

   Debug("GuestInfo: Returning after updating guest information\n");
   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SetGuestInfo --
 *
 *      Ask Vmx to write some information about the guest into VMDB.
 *
 * Results:
 *
 *      TRUE/FALSE depending on whether the RPCI succeeded or failed.
 *
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

Bool
SetGuestInfo(GuestInfoType key,  // IN: the VMDB key to set
             const char *value,  // IN:
             char delimiter)     // IN: delimiting character for the rpc
                                 //     message. 0 indicates default.
{
   Bool status;
   char *reply;
   size_t replyLen;

   ASSERT(key);
   ASSERT(value);

   if (!delimiter) {
      delimiter = GUESTINFO_DEFAULT_DELIMITER;
   }

   status = RpcOut_sendOne(&reply, &replyLen, "%s %c%d%c%s", GUEST_INFO_COMMAND,
                           delimiter, key, delimiter, value);

   if (!status) {
      Debug("SetGuestInfo: Error sending rpc message: %s\n", 
            reply ? reply : "NULL");
      free(reply);
      return FALSE;
   }

   /* The reply indicates whether the key,value pair was updated in VMDB. */
   status = (strncmp(reply, "", 1) == 0);
   free(reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NicInfoChanged --
 *
 *      Checks whether Nic information just obtained is different from
 *      the information last sent to VMDB.
 *
 * Results:
 *
 *      TRUE if the NIC info has changed, FALSE otherwise
 *
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

Bool
NicInfoChanged(PNicInfo nicInfo)     // IN:
{
   int nicIndex;
   int ipIndex;
   char *currentMac;
   int i;
   int matchedNic;
   PNicInfo cachedNicInfo;

   cachedNicInfo = &gInfoCache.nicInfo;

   if (cachedNicInfo->numNicEntries != nicInfo->numNicEntries) {
      Debug("GuestInfo: number of nics has changed\n");
      return TRUE;
   }

   /* Have any MAC or IP addresses been modified? */
   for (nicIndex = 0; nicIndex < cachedNicInfo->numNicEntries; nicIndex++) {
      currentMac = cachedNicInfo->nicList[nicIndex].macAddress;


      /* Find the corresponding nic in the new nic info. */
      for (i = 0; i < nicInfo->numNicEntries; i++) {
         if (strncmp(nicInfo->nicList[i].macAddress, currentMac, MAC_ADDR_SIZE)
              == 0) {
            break;
         }
      }

      matchedNic = i;
      if (matchedNic == nicInfo->numNicEntries) {
         /* This mac address has been deleted. */
         Debug("GuestInfo: mac address %s deleted\n", currentMac);
         return TRUE;
      }

      if (nicInfo->nicList[matchedNic].numIPs !=
          cachedNicInfo->nicList[nicIndex].numIPs) {
         Debug("GuestInfo: count of ip addresses for mac %d\n", matchedNic);
         return TRUE;
      }

      /* Which IP addresses have been modified for this NIC? */
      for (ipIndex = 0; ipIndex < cachedNicInfo->nicList[nicIndex].numIPs;
           ipIndex++) {
         char *currentIp;
         currentIp = cachedNicInfo->nicList[nicIndex].ipAddress[ipIndex];

         for (i = 0; i < nicInfo->nicList[matchedNic].numIPs; i++) {
            if (strncmp(nicInfo->nicList[matchedNic].ipAddress[i],
               currentIp, IP_ADDR_SIZE) == 0) {
               break;
            }
         }

         if (i == nicInfo->nicList[matchedNic].numIPs) {
            /* This ip address couldnt be found and has been modified. */
            Debug("GuestInfo: mac address %s, ipaddress %s deleted\n", currentMac,
                  currentIp);
            return TRUE;
         }
      }
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 * PrintNicInfo --
 *
 *      Print NIC info struct using the specified print function.
 *
 * Results:
 *      Sum of return values of print function (for printf, this will be the
 *      number of characters printed).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

int
PrintNicInfo(PNicInfo nicInfo,              // IN
             int (*PrintFunc)(const char *, ...)) // IN
{
   int ret = 0;
   int i;

   ASSERT(nicInfo);

   ret += PrintFunc("NicInfo: count: %d\n", nicInfo->numNicEntries);
   for (i = 0; i < nicInfo->numNicEntries; i++) {
      int j;
      NicEntry *ne = &nicInfo->nicList[i];

      ret += PrintFunc("NicInfo: nic [%d/%d] mac:      %s",
                       i+1, nicInfo->numNicEntries, ne->macAddress);
      for (j = 0; j < ne->numIPs; j++) {
         ret += PrintFunc("NicInfo: nic [%d/%d] IP [%d/%d]: %s",
                          i+1, nicInfo->numNicEntries, j+1, ne->numIPs,
                          ne->ipAddress[j]);
      }
   }

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * DiskInfoChanged --
 *
 *      Checks whether disk info information just obtained is different from
 *      the information last sent to VMDB.
 *
 * Results:
 *
 *      TRUE if the disk info has changed, FALSE otherwise.
 *
 * Side effects:
 *
 *	None.
 *
 *----------------------------------------------------------------------
 */

Bool
DiskInfoChanged(PDiskInfo diskInfo)     // IN:
{
   int index;
   char *name;
   int i;
   int matchedPartition;
   PDiskInfo cachedDiskInfo;

   cachedDiskInfo = &gInfoCache.diskInfo;

   if (cachedDiskInfo->numEntries != diskInfo->numEntries) {
      Debug("GuestInfo: number of disks has changed\n");
      return TRUE;
   }

   /* Have any disks been modified? */
   for (index = 0; index < cachedDiskInfo->numEntries; index++) {
      name = cachedDiskInfo->partitionList[index].name;

      /* Find the corresponding partition in the new partition info. */
      for (i = 0; i < diskInfo->numEntries; i++) {
         if (!strncmp(diskInfo->partitionList[i].name, name, PARTITION_NAME_SIZE)) {
            break;
         }
      }

      matchedPartition = i;
      if (matchedPartition == diskInfo->numEntries) {
         /* This partition has been deleted. */
         Debug("GuestInfo: partition %s deleted\n", name);
         return TRUE;
      } else {
         /* Compare the free space. */
         if (diskInfo->partitionList[matchedPartition].freeBytes != 
             cachedDiskInfo->partitionList[index].freeBytes) {
            Debug("GuestInfo: free space changed\n");
            return TRUE;
         }
         if (diskInfo->partitionList[matchedPartition].totalBytes != 
            cachedDiskInfo->partitionList[index].totalBytes) {
            Debug("GuestInfo: total space changed\n");
            return TRUE;
         }
      }
   }

   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * GuestInfoGetDiskInfo --
 *
 *      Get disk information.
 *
 * Results:
 *
 *      TRUE if successful, FALSE otherwise.
 *
 * Side effects:
 *
 *	     Allocates memory for di->partitionList.
 *
 *----------------------------------------------------------------------
 */

Bool 
GuestInfoGetDiskInfo(PDiskInfo di) // IN/OUT
{
   int i = 0; 
   WiperPartition_List *pl = NULL;
   unsigned int partCount = 0; 
   uint64 freeBytes = 0;
   uint64 totalBytes = 0;
   WiperPartition nextPartition;
   unsigned int partNameSize = 0;
   Bool success = FALSE;
   
   ASSERT(di);
   partNameSize = sizeof (di->partitionList)[0].name;
   di->numEntries = 0; 
   di->partitionList = NULL;
   
    /* Get partition list. */
   if (!Wiper_Init(NULL)) {
      Debug("GetDiskInfo: ERROR: could not initialize wiper library\n");
      return FALSE;
   }

   pl = WiperPartition_Open();
   if (pl == NULL) {
      Debug("GetDiskInfo: ERROR: could not get partition list\n");
      return FALSE;
   }
      
   for (i = 0; i < pl->size; i++) {
      nextPartition = pl->partitions[i];
      if (!strlen(nextPartition.comment)) {
         PPartitionEntry newPartitionList;
         unsigned char *error; 
         error = WiperSinglePartition_GetSpace(&nextPartition, &freeBytes, &totalBytes);
         if (strlen(error)) {
            Debug("GetDiskInfo: ERROR: could not get space for partition %s: %s\n", 
                  nextPartition.mountPoint, error);
            goto out;
         }
       
         if (strlen(nextPartition.mountPoint) + 1 > partNameSize) {
            Debug("GetDiskInfo: ERROR: Partition name buffer too small\n");
            goto out;
         }
         
         newPartitionList = realloc(di->partitionList, 
                                    (partCount + 1) * sizeof *di->partitionList);
         if (newPartitionList == NULL) {
            Debug("GetDiskInfo: ERROR: could not allocate partition list.\n");
            goto out;
         }
         di->partitionList = newPartitionList;

         Str_Strcpy((di->partitionList)[partCount].name, nextPartition.mountPoint, 
                        partNameSize); 
         (di->partitionList)[partCount].freeBytes = freeBytes;
         (di->partitionList)[partCount].totalBytes = totalBytes;
         partCount++;
      }
   }

   di->numEntries = partCount;
   success = TRUE;
  out:
   WiperPartition_Close(pl);
   return success;
}


/*
 *----------------------------------------------------------------------
 *
 * GuestInfoClearCache --
 *
 *    Clears the cached guest info data.
 *
 * Results:
 *    None.  
 *
 * Side effects:
 *    gInfoCache is cleared.
 *
 *----------------------------------------------------------------------
 */

static void
GuestInfoClearCache(void) 
{
   int i; 

   for (i = 0; i < INFO_MAX; i++) {
      gInfoCache.value[i][0] = 0;
   }
   
   gInfoCache.nicInfo.numNicEntries = 0;
   gInfoCache.diskInfo.numEntries = 0;

   if (gInfoCache.diskInfo.partitionList != NULL) {
      free(gInfoCache.diskInfo.partitionList);
      gInfoCache.diskInfo.partitionList = NULL;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * GetAvailableDiskSpace --
 *
 *    Get the amount of disk space available on the volume the FCP (file copy/
 *    paste) staging area is in. DnD and FCP use same staging area in guest.
 *    But it is only called in host->guest FCP case. DnD checks guest available
 *    disk space in host side (UI).
 *
 * Results:
 *    Available disk space size if succeed, otherwise 0.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

uint64
GetAvailableDiskSpace(char *pathName)
{
   WiperPartition p; 
   uint64 freeBytes  = 0;
   uint64 totalBytes = 0; 
   char *wiperError;
      
   Wiper_Init(NULL);

   if (strlen(pathName) > sizeof p.mountPoint) {
      Debug("GetAvailableDiskSpace: gFileRoot path too long\n");
      return 0;
   }
   Str_Strcpy((char *)p.mountPoint, pathName, sizeof p.mountPoint);
   wiperError = (char *)WiperSinglePartition_GetSpace(&p, &freeBytes, &totalBytes);      
   if (strlen(wiperError) > 0) {
      Debug("GetAvailableDiskSpace: error using wiper lib: %s\n", wiperError);
      return 0; 
   }
   Debug("GetAvailableDiskSpace: free bytes is %"FMT64"u\n", freeBytes);
   return freeBytes;
}


/*
 *----------------------------------------------------------------------
 *
 * GuestInfoServer_SendUptime --
 *
 *      Set the guest uptime through the backdoor.
 *
 * Results:
 *      TRUE if the rpci send succeeded
 *      FALSE if it failed
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

Bool
GuestInfoServer_SendUptime(void)
{
   char *uptime = Str_Asprintf(NULL, "%"FMT64"u", System_Uptime());
   Bool ret;

   ASSERT_MEM_ALLOC(uptime);
   Debug("Setting guest uptime to '%s'\n", uptime);
   ret = GuestInfoUpdateVmdb(INFO_UPTIME, uptime);
   free(uptime);
   return ret;
}

