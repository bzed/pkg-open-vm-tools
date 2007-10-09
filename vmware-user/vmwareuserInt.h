
/* **************************************************************************
 * Copyright (C) 2005 VMware, Inc. All Rights Reserved 
 * **************************************************************************
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
 * vmwareuserInt.h --
 *
 *     Common defines used by vmwareuser
 */
#ifndef _VMWAREUSER_INT_H_
# define _VMWAREUSER_INT_H_

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#ifndef NO_MULTIMON
#include <X11/extensions/Xinerama.h>
#endif
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#undef Bool
#include "vm_basic_types.h"
#include "rpcout.h"
#include "rpcin.h"

/*
 * These must be the same as the minimum values used by the lots_of_modlines()
 * function in config.pl
 */
#define RESOLUTION_MIN_WIDTH  100
#define RESOLUTION_MIN_HEIGHT 100

#define RPCIN_POLL_TIME        10  /* in 1/1000ths of a second */
#define POINTER_POLL_TIME      15  /* in 1/1000ths of a second */
#define UNGRABBED_POS -100
#define DEBUG_PREFIX           "vmusr"

#define FCP_FILE_TRANSFER_NOT_YET            0
#define FCP_FILE_TRANSFERRING                1
#define FCP_FILE_TRANSFERRED                 2

Bool Resolution_Register(void);
Bool Resolution_RegisterCapability(void);
Bool Resolution_Unregister(void);

Bool DnD_Register(GtkWidget *mainWnd);
Bool DnD_RegisterCapability(void);
void DnD_Unregister(GtkWidget *mainWnd);
uint32 DnD_GetVmxDnDVersion(void);
int DnD_GetNewFileRoot(char *fileRoot, int bufSize);
void DnD_OnReset(GtkWidget *mainWnd);
Bool DnD_InProgress(void);

Bool CopyPaste_Register(GtkWidget* mainWnd);
Bool CopyPaste_RegisterCapability(void);
int32 CopyPaste_GetVmxCopyPasteVersion(void);
void CopyPaste_RequestSelection(void);
Bool CopyPaste_GetBackdoorSelections(void);
void CopyPaste_Unregister(GtkWidget* mainWnd);
Bool CopyPaste_GHFileListGetNext(char **fileName, size_t *fileNameSize);
Bool CopyPaste_InProgress(void);

Bool Pointer_Register(GtkWidget* mainWnd);

extern RpcIn *gRpcIn;
extern Display *gXDisplay;
extern Window gXRoot;
extern DblLnkLst_Links *gEventQueue;
extern GtkWidget *gUserMainWidget;
extern Bool optionCopyPaste;
extern Bool gCanUseVMwareCtrl;
extern Bool gCanUseVMwareCtrlTopologySet;
extern int gBlockFd;

#endif // _VMWAREUSER_INT_H_