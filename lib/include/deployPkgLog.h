/* **********************************************************
 * Copyright 2006 VMware, Inc.  All rights reserved. 
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
 * deployPkgLog.h --
 *
 *    logger for both windows and posix versions of depployPkg
 */

/* Functions to manage the log */
void DeployPkgLog_Open(void);
void DeployPkgLog_Close(void);
void DeployPkgLog_Log(int level, const char *fmtstr, ...);
