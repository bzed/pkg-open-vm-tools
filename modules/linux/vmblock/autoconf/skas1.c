/* **********************************************************
 * Copyright (C) 2004 VMware, Inc.  All Rights Reserved. 
 * **********************************************************
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
 */

/*
 * SKAS patch adds 'struct mm *mm' as first argument to do_mmap_pgoff.
 * This patch never hit mainstream kernel.
 */

#include <linux/mm.h>

unsigned long check_do_mmap_pgoff(struct mm_struct *mm, struct file *file,
				  unsigned long addr, unsigned long len,
				  unsigned long prot, unsigned long flag,
				  unsigned long pgoff) {
   return do_mmap_pgoff(mm, file, addr, len, prot, flag, pgoff);
}
