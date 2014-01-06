/* **********************************************************
 * Copyright 2007 VMware, Inc.  All rights reserved.
 * 
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
 * msgfmg.h --
 *
 *	MsgFmt: format messages for the Msg module
 */

#ifndef _MSGFMT_H_
#define _MSGFMT_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "str.h" // for HAS_BSD_PRINTF


/*
 * A format argument
 */

typedef enum MsgFmt_ArgType {
   MSGFMT_ARG_INVALID, // must be 0
   MSGFMT_ARG_INT32,
   MSGFMT_ARG_INT64,
   MSGFMT_ARG_PTR32,
   MSGFMT_ARG_PTR64,
   MSGFMT_ARG_FLOAT64,
   MSGFMT_ARG_STRING8,
   MSGFMT_ARG_STRING16,
   MSGFMT_ARG_STRING32,
} MsgFmt_ArgType;

typedef struct MsgFmt_Arg {
   MsgFmt_ArgType type;
   union {
      int32 signed32;
      int64 signed64;
      uint32 unsigned32;
      uint64 unsigned64;
      double float64;
      int8 *string8;
      int16 *string16;
      int32 *string32;

      // private
      struct {
	 void *ptr;	// must align with string{8,16,32}
	 int precision;
      } s;
   } v;
} MsgFmt_Arg;


/*
 * Global functions
 */

Bool MsgFmt_GetArgs(const char *fmt, va_list va,
                    MsgFmt_Arg **args, int *numArgs);
void MsgFmt_FreeArgs(MsgFmt_Arg *args, int numArgs);

#ifdef HAS_BSD_PRINTF
int MsgFmt_Snprintf(char *buf, size_t size, const char *format,
                    const MsgFmt_Arg *args, int numArgs);
char *MsgFmt_Asprintf(size_t *length, const char *format,
                      const MsgFmt_Arg *args, int numArgs);
#endif


#endif // ifndef _MSGFMT_H_
