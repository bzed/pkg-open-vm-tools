/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

#ifndef __COMPAT_WORKQUEUE_H__
# define __COMPAT_WORKQUEUE_H__

#include <linux/kernel.h>
#include <linux/workqueue.h>

/*
 * Linux 2.6.20 differentiates normal work structs ("work_struct") from
 * delayed work ("delayed_work"). So define a few of the names that don't
 * exist in older kernels to use the old work_struct.
 *
 * Also, in 2.6.20 and beyond, the work_struct itself is passed as an
 * argument to the function, and the data is retrieved similar to how
 * linked lists work. To properly pass an argument to the callback function,
 * the work struct should be declared as part of a struct, and the struct
 * retrieved by using the COMPAT_DELAYEDWORK_GETDATA macro (for instances
 * of delayed_work).
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20) /* { */

typedef void compat_workqueue_arg;

# define delayed_work work_struct
# define INIT_DELAYED_WORK(work, func) INIT_WORK((work), (func), (work))
# define COMPAT_DELAYEDWORK_GETDATA(data, type, field) container_of(data, type, field)

#else /* } Linux >= 2.6.20 { */

typedef struct work_struct compat_workqueue_arg;

# define COMPAT_DELAYEDWORK_GETDATA(data, type, field)                           \
({                                                                               \
   struct delayed_work *__dwork = container_of(data, struct delayed_work, work); \
   container_of(__dwork, type, field);                                           \
})

#endif /* } */

#endif /* __COMPAT_WORKQUEUE_H__ */

