/*********************************************************
 * Copyright (C) 2008 VMware, Inc. All rights reserved.
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

/*********************************************************
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *********************************************************/

#ifndef __COMPAT_VOP_H__
#   define __COMPAT_VOP_H__ 1

#if __FreeBSD_version >= 800011
#define COMPAT_THREAD_VAR(varname, varval)
#define COMPAT_VOP_LOCK(vop, flags, threadvar) VOP_LOCK((vop), (flags))
#define COMPAT_VOP_UNLOCK(vop, flags, threadvar) VOP_UNLOCK((vop), (flags))
#define compat_lockstatus(lock, threadvar) lockstatus((lock))
#define compat_lockmgr(lock, flags, randompointerparam, threadval) lockmgr((lock), (flags), (randompointerparam))
#define compat_vn_lock(vp, flags, threadval) vn_lock((vp), (flags))
#define compat_accmode_t accmode_t
#define compat_a_accmode a_accmode
#else
#define COMPAT_THREAD_VAR(varname, varval) struct thread *varname = varval
#define COMPAT_VOP_LOCK(vop, flags, threadvar) VOP_LOCK((vop), (flags), (threadvar))
#define COMPAT_VOP_UNLOCK(vop, flags, threadvar) VOP_UNLOCK((vop), (flags), (threadvar))
#define compat_lockstatus(lock, threadvar) lockstatus((lock), (threadvar))
#define compat_vn_lock(vp, flags, threadval) vn_lock((vp), (flags), (threadval))
#define compat_lockmgr(lock, flags, randompointerparam, threadval) lockmgr((lock), (flags), (randompointerparam), (threadval))
#define compat_accmode_t mode_t
#define compat_a_accmode a_mode
#endif

/*
 * We use defines rather than typedefs here to avoid causing problems for files that
 * don't have a vnode_if.h available.
 */
#if __FreeBSD_version >= 700055
#   define compat_vop_lock_t vop_lock1_t
#   define compat_vop_lock_args struct vop_lock1_args
#   define COMPAT_VOP_LOCK_OP_ELEMENT vop_lock1
#else
#   define compat_vop_lock_t vop_lock_t
#   define compat_vop_lock_args struct vop_lock_args
#   define COMPAT_VOP_LOCK_OP_ELEMENT vop_lock
#endif

#endif
