/* **********************************************************
 * Copyright (C) 2002 VMware, Inc.  All Rights Reserved. 
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

#ifndef __COMPAT_NETDEVICE_H__
#   define __COMPAT_NETDEVICE_H__


#include <linux/netdevice.h>
#include <linux/etherdevice.h>

/*
 * The enet_statistics structure moved from linux/if_ether.h to
 * linux/netdevice.h and is renamed net_device_stats in 2.1.25 --hpreg
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 1, 25)
#   include <linux/if_ether.h>

#   define net_device_stats enet_statistics
#endif


/* The netif_rx_ni() API appeared in 2.4.8 --hpreg */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 8)
#   define netif_rx_ni netif_rx
#endif


/* The device struct was renamed net_device in 2.3.14 --hpreg */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 3, 14)
#   define net_device device
#endif


/*
 * SET_MODULE_OWNER appeared sometime during 2.3.x. It was setting
 * dev->owner = THIS_MODULE until 2.5.70, where netdevice refcounting
 * was completely changed.
 *
 * MOD_xxx_USE_COUNT wrappers are here, as they must be mutually
 * exclusive with SET_MODULE_OWNER call.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 0)
#   define COMPAT_SET_MODULE_OWNER(dev) do {} while (0)
#   define COMPAT_NETDEV_MOD_INC_USE_COUNT MOD_INC_USE_COUNT
#   define COMPAT_NETDEV_MOD_DEC_USE_COUNT MOD_DEC_USE_COUNT
#else
#   define COMPAT_SET_MODULE_OWNER(dev) SET_MODULE_OWNER(dev)
#   define COMPAT_NETDEV_MOD_INC_USE_COUNT do {} while (0)
#   define COMPAT_NETDEV_MOD_DEC_USE_COUNT do {} while (0)
#endif


/*
 * Build alloc_etherdev API on the top of init_etherdev.  For 2.0.x kernels
 * we must provide dummy init method, otherwise register_netdev does
 * nothing.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 3)

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 0)
int
vmware_dummy_init(struct net_device *dev)
{
   return 0;
}
#endif


static inline struct net_device*
compat_alloc_etherdev(int priv_size)
{
   struct net_device* dev;
   int size = sizeof *dev + priv_size;

   /*
    * The name is dynamically allocated before 2.4.0, but 
    * is an embedded array in later kernels.
    */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 0)
   size += sizeof("ethXXXXXXX");
#endif
   dev = kmalloc(size, GFP_KERNEL);
   if (dev) {
      memset(dev, 0, size);
      if (priv_size) {
         dev->priv = dev + 1;
      }
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 0)
      dev->name = (char *)(dev + 1) + priv_size;
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 0)
      dev->init = vmware_dummy_init;
#endif
      if (init_etherdev(dev, 0) != dev) {
         kfree(dev);
         dev = NULL;
      }
   }
   return dev;
}
#else
#define compat_alloc_etherdev(sz)   alloc_etherdev(sz)
#endif


/* free_netdev() is available since 2.4.23.  Use kfree() on older kernels. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 23)
#define compat_free_netdev(dev)     kfree(dev)
#else
#define compat_free_netdev(dev)     free_netdev(dev)
#endif


#endif /* __COMPAT_NETDEVICE_H__ */
