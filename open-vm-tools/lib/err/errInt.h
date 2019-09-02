/*********************************************************
 * Copyright (C) 2006-2016 VMware, Inc. All rights reserved.
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
 * errInt.h --
 *
 *	Internal definitions for the Err module.
 */

#ifndef _ERRINT_H_
#define _ERRINT_H_

#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"

#include "err.h"

const char *ErrErrno2String(Err_Number errorNumber,
                            char *buf,
                            size_t bufSize);

#endif // ifndef _ERRINT_H_
