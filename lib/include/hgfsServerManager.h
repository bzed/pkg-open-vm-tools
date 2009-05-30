/*********************************************************
 * Copyright (C) 2006 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#ifndef _HGFS_SERVER_MANAGER_H_
# define _HGFS_SERVER_MANAGER_H_

/*
 * hgfsServerManager.h --
 *
 *    Common routines needed to register an HGFS server.
 */


#ifndef VMX86_TOOLS
#include "device_shared.h" // For DeviceLock and functions

Bool Hgfs_PowerOn(void);

void HgfsServerManager_GetDeviceLock(DeviceLock **lock);

#else  /* VMX86_TOOLS */
Bool HgfsServerManager_Register(void *rpcIn,
                                const char *appName);
void HgfsServerManager_Unregister(void *rpcIn,
                                  const char *appName);
Bool HgfsServerManager_CapReg(const char *appName,
                              Bool enable);
#endif

#endif // _HGFS_SERVER_MANAGER_H_
