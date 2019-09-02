/*********************************************************
 * Copyright (C) 2007-2016 VMware, Inc. All rights reserved.
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

/*
 * checkvm_version.h --
 *
 * Version definitions for the VM checking utility.
 */

#ifndef _CHECKVM_VERSION_H_
#define _CHECKVM_VERSION_H_

/*
 * This component's version is coupled with Tools versioning. The effect
 * is that the version increments with each build, and with each Tools
 * version bump. If and when it becomes necessary to version the component
 * manually, make sure that the version is bumped any time the component or
 * its dependencies are changed.
 */
#include "vm_tools_version.h"
#define CHECKVM_VERSION_COMMAS   TOOLS_VERSION_EXT_CURRENT_CSV
#define CHECKVM_VERSION_STRING   TOOLS_VERSION_EXT_CURRENT_STR

#endif /* _CHECKVM_VERSION_H_ */
