/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. 
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

#ifndef __VMXNETINT_H__
#define __VMXNETINT_H__

#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"

#include "return_status.h"
#include "net_dist.h"

#define VMXNET_CHIP_NAME "vmxnet ether"

#define CRC_POLYNOMIAL_LE 0xedb88320UL  /* Ethernet CRC, little endian */

#define PKT_BUF_SZ			1536
#define VMXNET_MIN_MTU                  (ETH_MIN_FRAME_LEN - 14)
#define VMXNET_MAX_MTU                  (16 * 1024 - 18)

/* Largest address able to be shared between the driver and the device */
#define SHARED_MEM_MAX 0xFFFFFFFF

typedef enum Vmxnet_TxStatus {
   VMXNET_CALL_TRANSMIT,
   VMXNET_DEFER_TRANSMIT,
   VMXNET_STOP_TRANSMIT
} Vmxnet_TxStatus;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,1,0))
#   define MODULE_PARM(var, type)
#   define net_device_stats enet_statistics
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,14))
#   define net_device device
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,43))
static inline void
netif_start_queue(struct device *dev)
{
   clear_bit(0, &dev->tbusy);
}

static inline void
netif_stop_queue(struct device *dev)
{
   set_bit(0, &dev->tbusy);
}

static inline int
netif_queue_stopped(struct device *dev)
{
   return test_bit(0, &dev->tbusy);
}

static inline void
netif_wake_queue(struct device *dev)
{
   clear_bit(0, &dev->tbusy);
   mark_bh(NET_BH);
}

static inline int
netif_running(struct device *dev)
{
   return dev->start == 0;
}
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,2,0))
#   define le16_to_cpu(x) ((__u16)(x))
#   define le32_to_cpu(x) ((__u32)(x))
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,1,0))
#   define compat_kfree_skb(skb, type) kfree_skb(skb, type)
#   define compat_dev_kfree_skb(skb, type) dev_kfree_skb(skb, type)
#   define compat_dev_kfree_skb_any(skb, type) dev_kfree_skb(skb, type)
#   define compat_dev_kfree_skb_irq(skb, type) dev_kfree_skb(skb, type)
#else
#   define compat_kfree_skb(skb, type) kfree_skb(skb)
#   define compat_dev_kfree_skb(skb, type) dev_kfree_skb(skb)
#   if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,43))
#      define compat_dev_kfree_skb_any(skb, type) dev_kfree_skb(skb)
#      define compat_dev_kfree_skb_irq(skb, type) dev_kfree_skb(skb)
#   else
#      define compat_dev_kfree_skb_any(skb, type) dev_kfree_skb_any(skb)
#      define compat_dev_kfree_skb_irq(skb, type) dev_kfree_skb_irq(skb)
#   endif
#endif

#if defined(BUG_ON)
#define VMXNET_ASSERT(cond) BUG_ON(!(cond))
#else
#define VMXNET_ASSERT(cond)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19) && defined(CHECKSUM_HW)
#   define VM_CHECKSUM_PARTIAL     CHECKSUM_HW
#   define VM_CHECKSUM_UNNECESSARY CHECKSUM_UNNECESSARY
#else
#   define VM_CHECKSUM_PARTIAL     CHECKSUM_PARTIAL
#   define VM_CHECKSUM_UNNECESSARY CHECKSUM_UNNECESSARY
#endif

struct Vmxnet2_TxBuf {
   struct sk_buff *skb;
   char    sgForLinear; /* the sg entry mapping the linear part 
                         * of the skb, -1 means this tx entry only
                         * mapps the frags of the skb
                         */ 
   char    firstSgForFrag;   /* the first sg entry mapping the frags */
   Bool    eop;
};

/*
 * Private data area, pointed to by priv field of our struct net_device.
 * dd field is shared with the lower layer.
 */
typedef struct Vmxnet_Private {
   Vmxnet2_DriverData	       *dd;
   const char 		       *name;
   struct net_device_stats	stats;
   struct sk_buff	       *rxSkbuff[VMXNET2_MAX_NUM_RX_BUFFERS];
   struct page                 *rxPages[VMXNET2_MAX_NUM_RX_BUFFERS2];
   struct Vmxnet2_TxBuf         txBufInfo[VMXNET2_MAX_NUM_TX_BUFFERS_TSO];
   spinlock_t                   txLock;
   int				numTxPending;
   unsigned int			numRxBuffers;
   unsigned int			numRxBuffers2;
   unsigned int			numTxBuffers;
   Vmxnet2_RxRingEntry         *rxRing;
   Vmxnet2_RxRingEntry         *rxRing2;
   Vmxnet2_TxRingEntry         *txRing;

   Bool				devOpen;
   Net_PortID			portID;

   uint32                       capabilities;
   uint32                       features;

   Bool                         zeroCopyTx;
   Bool                         partialHeaderCopyEnabled;
   Bool                         tso;
   Bool                         chainTx;
   Bool                         chainRx;
   Bool                         jumboFrame;
   Bool                         lpd;
   
   Bool                         morphed;           // Indicates whether adapter is morphed
   void                        *ddAllocated;
   char                        *txBufferStartRaw;
   char                        *txBufferStart;
   struct pci_dev              *pdev;
} Vmxnet_Private;

#endif /* __VMXNETINT_H__ */
