/*********************************************************
 * Copyright (C) 2008 VMware, Inc. All rights reserved.
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

/*!
 * @file guestcust-events.h --
 */

#ifndef _GUESTCUST_EVENTS_H
#define _GUESTCUST_EVENTS_H

/*
 * Customization-specific events generated in the guest and handled by
 * hostd. They are sent via the Tools/deployPkgState/error VIGOR path.
 * The error(int) field is overloaded for both the deploy pkg errors
 * and the deploy pkg events.
 * Therefore, We start these at 100 to avoid conflict with the deployPkg error
 * codes listed in bora/guestABI/include/vmware/guestrpc/deploypkg.h
 */
typedef enum {
   GUESTCUST_EVENT_CUSTOMIZE_FAILED = 100,
   GUESTCUST_EVENT_NETWORK_SETUP_FAILED,
   GUESTCUST_EVENT_SYSPREP_FAILED,
   GUESTCUST_EVENT_ENABLE_NICS,
   GUESTCUST_EVENT_QUERY_NICS
} GuestCustEvent;

#endif // _GUESTCUST_EVENTS_H
