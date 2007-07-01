/*

  Broadcom BCM43xx wireless driver

  DMA ringbuffer and descriptor allocation/management

  Copyright (c) 2005, 2006 Michael Buesch <mb@bu3sch.de>

  Some code in this file is derived from the b44.c driver
  Copyright (C) 2002 David S. Miller
  Copyright (C) Pekka Pietikainen

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to
  the Free Software Foundation, Inc., 51 Franklin Steet, Fifth Floor,
  Boston, MA 02110-1301, USA.

*/

#include "bcm43xx.h"
#include "bcm43xx_dma.h"
#include "bcm43xx_main.h"
#include "bcm43xx_debugfs.h"
#include "bcm43xx_power.h"
#include "bcm43xx_xmit.h"

#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/skbuff.h>


/* 32bit DMA ops. */
static
struct bcm43xx_dmadesc_generic * op32_idx2desc(struct bcm43xx_dmaring *ring,
					       int slot,
					       struct bcm43xx_dmadesc_meta **meta)
{
	struct bcm43xx_dmadesc32 *desc;

	*meta = &(ring->meta[slot]);
	desc = ring->descbase;
	desc = &(desc[slot]);

	return (struct bcm43xx_dmadesc_generic *)desc;
}

static void op32_fill_descriptor(struct bcm43xx_dmaring *ring,
				 struct bcm43xx_dmadesc_generic *desc,
				 dma_addr_t dmaaddr, u16 bufsize,
				 int start, int end, int irq)
{
	struct bcm43xx_dmadesc32 *descbase = ring->descbase;
	int slot;
	u32 ctl;
	u32 addr;
	u32 addrext;

	slot = (int)(&(desc->dma32) - descbase);
	assert(slot >= 0 && slot < ring->nr_slots);

	addr = (u32)(dmaaddr & ~SSB_DMA_TRANSLATION_MASK);
	addrext = (u32)(dmaaddr & SSB_DMA_TRANSLATION_MASK)
		   >> SSB_DMA_TRANSLATION_SHIFT;
	addr |= ssb_dma_translation(ring->dev->dev);
	ctl = (bufsize - ring->frameoffset)
	      & BCM43xx_DMA32_DCTL_BYTECNT;
	if (slot == ring->nr_slots - 1)
		ctl |= BCM43xx_DMA32_DCTL_DTABLEEND;
	if (start)
		ctl |= BCM43xx_DMA32_DCTL_FRAMESTART;
	if (end)
		ctl |= BCM43xx_DMA32_DCTL_FRAMEEND;
	if (irq)
		ctl |= BCM43xx_DMA32_DCTL_IRQ;
	ctl |= (addrext << BCM43xx_DMA32_DCTL_ADDREXT_SHIFT)
	       & BCM43xx_DMA32_DCTL_ADDREXT_MASK;

	desc->dma32.control = cpu_to_le32(ctl);
	desc->dma32.address = cpu_to_le32(addr);
}

static void op32_poke_tx(struct bcm43xx_dmaring *ring, int slot)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA32_TXINDEX,
			  (u32)(slot * sizeof(struct bcm43xx_dmadesc32)));
}

static void op32_tx_suspend(struct bcm43xx_dmaring *ring)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA32_TXCTL,
			  bcm43xx_dma_read(ring, BCM43xx_DMA32_TXCTL)
			  | BCM43xx_DMA32_TXSUSPEND);
}

static void op32_tx_resume(struct bcm43xx_dmaring *ring)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA32_TXCTL,
			  bcm43xx_dma_read(ring, BCM43xx_DMA32_TXCTL)
			  & ~BCM43xx_DMA32_TXSUSPEND);
}

static int op32_get_current_rxslot(struct bcm43xx_dmaring *ring)
{
	u32 val;

	val = bcm43xx_dma_read(ring, BCM43xx_DMA32_RXSTATUS);
	val &= BCM43xx_DMA32_RXDPTR;

	return (val / sizeof(struct bcm43xx_dmadesc32));
}

static void op32_set_current_rxslot(struct bcm43xx_dmaring *ring,
				    int slot)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA32_RXINDEX,
			  (u32)(slot * sizeof(struct bcm43xx_dmadesc32)));
}

static const struct bcm43xx_dma_ops dma32_ops = {
	.idx2desc		= op32_idx2desc,
	.fill_descriptor	= op32_fill_descriptor,
	.poke_tx		= op32_poke_tx,
	.tx_suspend		= op32_tx_suspend,
	.tx_resume		= op32_tx_resume,
	.get_current_rxslot	= op32_get_current_rxslot,
	.set_current_rxslot	= op32_set_current_rxslot,
};

/* 64bit DMA ops. */
static
struct bcm43xx_dmadesc_generic * op64_idx2desc(struct bcm43xx_dmaring *ring,
					       int slot,
					       struct bcm43xx_dmadesc_meta **meta)
{
	struct bcm43xx_dmadesc64 *desc;

	*meta = &(ring->meta[slot]);
	desc = ring->descbase;
	desc = &(desc[slot]);

	return (struct bcm43xx_dmadesc_generic *)desc;
}

static void op64_fill_descriptor(struct bcm43xx_dmaring *ring,
				 struct bcm43xx_dmadesc_generic *desc,
				 dma_addr_t dmaaddr, u16 bufsize,
				 int start, int end, int irq)
{
	struct bcm43xx_dmadesc64 *descbase = ring->descbase;
	int slot;
	u32 ctl0 = 0, ctl1 = 0;
	u32 addrlo, addrhi;
	u32 addrext;

	slot = (int)(&(desc->dma64) - descbase);
	assert(slot >= 0 && slot < ring->nr_slots);

	addrlo = (u32)(dmaaddr & 0xFFFFFFFF);
	addrhi = (((u64)dmaaddr >> 32) & ~SSB_DMA_TRANSLATION_MASK);
	addrext = (((u64)dmaaddr >> 32) & SSB_DMA_TRANSLATION_MASK)
		  >> SSB_DMA_TRANSLATION_SHIFT;
	addrhi |= ssb_dma_translation(ring->dev->dev);
	if (slot == ring->nr_slots - 1)
		ctl0 |= BCM43xx_DMA64_DCTL0_DTABLEEND;
	if (start)
		ctl0 |= BCM43xx_DMA64_DCTL0_FRAMESTART;
	if (end)
		ctl0 |= BCM43xx_DMA64_DCTL0_FRAMEEND;
	if (irq)
		ctl0 |= BCM43xx_DMA64_DCTL0_IRQ;
	ctl1 |= (bufsize - ring->frameoffset)
		& BCM43xx_DMA64_DCTL1_BYTECNT;
	ctl1 |= (addrext << BCM43xx_DMA64_DCTL1_ADDREXT_SHIFT)
		& BCM43xx_DMA64_DCTL1_ADDREXT_MASK;

	desc->dma64.control0 = cpu_to_le32(ctl0);
	desc->dma64.control1 = cpu_to_le32(ctl1);
	desc->dma64.address_low = cpu_to_le32(addrlo);
	desc->dma64.address_high = cpu_to_le32(addrhi);
}

static void op64_poke_tx(struct bcm43xx_dmaring *ring, int slot)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA64_TXINDEX,
			  (u32)(slot * sizeof(struct bcm43xx_dmadesc64)));
}

static void op64_tx_suspend(struct bcm43xx_dmaring *ring)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA64_TXCTL,
			  bcm43xx_dma_read(ring, BCM43xx_DMA64_TXCTL)
			  | BCM43xx_DMA64_TXSUSPEND);
}

static void op64_tx_resume(struct bcm43xx_dmaring *ring)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA64_TXCTL,
			  bcm43xx_dma_read(ring, BCM43xx_DMA64_TXCTL)
			  & ~BCM43xx_DMA64_TXSUSPEND);
}

static int op64_get_current_rxslot(struct bcm43xx_dmaring *ring)
{
	u32 val;

	val = bcm43xx_dma_read(ring, BCM43xx_DMA64_RXSTATUS);
	val &= BCM43xx_DMA64_RXSTATDPTR;

	return (val / sizeof(struct bcm43xx_dmadesc64));
}

static void op64_set_current_rxslot(struct bcm43xx_dmaring *ring,
				    int slot)
{
	bcm43xx_dma_write(ring, BCM43xx_DMA64_RXINDEX,
			  (u32)(slot * sizeof(struct bcm43xx_dmadesc64)));
}

static const struct bcm43xx_dma_ops dma64_ops = {
	.idx2desc		= op64_idx2desc,
	.fill_descriptor	= op64_fill_descriptor,
	.poke_tx		= op64_poke_tx,
	.tx_suspend		= op64_tx_suspend,
	.tx_resume		= op64_tx_resume,
	.get_current_rxslot	= op64_get_current_rxslot,
	.set_current_rxslot	= op64_set_current_rxslot,
};


static inline int free_slots(struct bcm43xx_dmaring *ring)
{
	return (ring->nr_slots - ring->used_slots);
}

static inline int next_slot(struct bcm43xx_dmaring *ring, int slot)
{
	assert(slot >= -1 && slot <= ring->nr_slots - 1);
	if (slot == ring->nr_slots - 1)
		return 0;
	return slot + 1;
}

static inline int prev_slot(struct bcm43xx_dmaring *ring, int slot)
{
	assert(slot >= 0 && slot <= ring->nr_slots - 1);
	if (slot == 0)
		return ring->nr_slots - 1;
	return slot - 1;
}

#ifdef CONFIG_BCM43XX_MAC80211_DEBUG
static void update_max_used_slots(struct bcm43xx_dmaring *ring,
				  int current_used_slots)
{
	if (current_used_slots <= ring->max_used_slots)
		return;
	ring->max_used_slots = current_used_slots;
	if (bcm43xx_debug(ring->dev, BCM43xx_DBG_DMAVERBOSE)) {
		dprintk(KERN_DEBUG PFX
			"max_used_slots increased to %d on %s ring %d\n",
			ring->max_used_slots,
			ring->tx ? "TX" : "RX",
			ring->index);
	}
}
#else
static inline
void update_max_used_slots(struct bcm43xx_dmaring *ring,
			   int current_used_slots)
{ }
#endif /* DEBUG */

/* Request a slot for usage. */
static inline
int request_slot(struct bcm43xx_dmaring *ring)
{
	int slot;

	assert(ring->tx);
	assert(!ring->stopped);
	assert(free_slots(ring) != 0);

	slot = next_slot(ring, ring->current_slot);
	ring->current_slot = slot;
	ring->used_slots++;

	update_max_used_slots(ring, ring->used_slots);

	return slot;
}

/* Mac80211-queue to bcm43xx-ring mapping */
static struct bcm43xx_dmaring * priority_to_txring(struct bcm43xx_wldev *dev,
						   int queue_priority)
{
	struct bcm43xx_dmaring *ring;

/*FIXME: For now we always run on TX-ring-1 */
return dev->dma.tx_ring1;

	/* 0 = highest priority */
	switch (queue_priority) {
	default:
		assert(0);
		/* fallthrough */
	case 0:
		ring = dev->dma.tx_ring3;
		break;
	case 1:
		ring = dev->dma.tx_ring2;
		break;
	case 2:
		ring = dev->dma.tx_ring1;
		break;
	case 3:
		ring = dev->dma.tx_ring0;
		break;
	case 4:
		ring = dev->dma.tx_ring4;
		break;
	case 5:
		ring = dev->dma.tx_ring5;
		break;
	}

	return ring;
}

/* Bcm43xx-ring to mac80211-queue mapping */
static inline int txring_to_priority(struct bcm43xx_dmaring *ring)
{
	static const u8 idx_to_prio[] =
		{ 3, 2, 1, 0, 4, 5, };

/*FIXME: have only one queue, for now */
return 0;

	return idx_to_prio[ring->index];
}


u16 bcm43xx_dmacontroller_base(int dma64bit, int controller_idx)
{
	static const u16 map64[] = {
		BCM43xx_MMIO_DMA64_BASE0,
		BCM43xx_MMIO_DMA64_BASE1,
		BCM43xx_MMIO_DMA64_BASE2,
		BCM43xx_MMIO_DMA64_BASE3,
		BCM43xx_MMIO_DMA64_BASE4,
		BCM43xx_MMIO_DMA64_BASE5,
	};
	static const u16 map32[] = {
		BCM43xx_MMIO_DMA32_BASE0,
		BCM43xx_MMIO_DMA32_BASE1,
		BCM43xx_MMIO_DMA32_BASE2,
		BCM43xx_MMIO_DMA32_BASE3,
		BCM43xx_MMIO_DMA32_BASE4,
		BCM43xx_MMIO_DMA32_BASE5,
	};

	if (dma64bit) {
		assert(controller_idx >= 0 &&
		       controller_idx < ARRAY_SIZE(map64));
		return map64[controller_idx];
	}
	assert(controller_idx >= 0 &&
	       controller_idx < ARRAY_SIZE(map32));
	return map32[controller_idx];
}

static inline
dma_addr_t map_descbuffer(struct bcm43xx_dmaring *ring,
			  unsigned char *buf,
			  size_t len,
			  int tx)
{
	dma_addr_t dmaaddr;

	if (tx) {
		dmaaddr = dma_map_single(ring->dev->dev->dev,
					 buf, len,
					 DMA_TO_DEVICE);
	} else {
		dmaaddr = dma_map_single(ring->dev->dev->dev,
					 buf, len,
					 DMA_FROM_DEVICE);
	}

	return dmaaddr;
}

static inline
void unmap_descbuffer(struct bcm43xx_dmaring *ring,
		      dma_addr_t addr,
		      size_t len,
		      int tx)
{
	if (tx) {
		dma_unmap_single(ring->dev->dev->dev,
				 addr, len,
				 DMA_TO_DEVICE);
	} else {
		dma_unmap_single(ring->dev->dev->dev,
				 addr, len,
				 DMA_FROM_DEVICE);
	}
}

static inline
void sync_descbuffer_for_cpu(struct bcm43xx_dmaring *ring,
			     dma_addr_t addr,
			     size_t len)
{
	assert(!ring->tx);

	dma_sync_single_for_cpu(ring->dev->dev->dev,
				addr, len, DMA_FROM_DEVICE);
}

static inline
void sync_descbuffer_for_device(struct bcm43xx_dmaring *ring,
				dma_addr_t addr,
				size_t len)
{
	assert(!ring->tx);

	dma_sync_single_for_device(ring->dev->dev->dev,
				   addr, len, DMA_FROM_DEVICE);
}

static inline
void free_descriptor_buffer(struct bcm43xx_dmaring *ring,
			    struct bcm43xx_dmadesc_meta *meta,
			    int irq_context)
{
	if (meta->skb) {
		if (irq_context)
			dev_kfree_skb_irq(meta->skb);
		else
			dev_kfree_skb(meta->skb);
		meta->skb = NULL;
	}
}

static int alloc_ringmemory(struct bcm43xx_dmaring *ring)
{
	struct device *dev = ring->dev->dev->dev;

	ring->descbase = dma_alloc_coherent(dev, BCM43xx_DMA_RINGMEMSIZE,
					    &(ring->dmabase), GFP_KERNEL);
	if (!ring->descbase) {
		printk(KERN_ERR PFX "DMA ringmemory allocation failed\n");
		return -ENOMEM;
	}
	memset(ring->descbase, 0, BCM43xx_DMA_RINGMEMSIZE);

	return 0;
}

static void free_ringmemory(struct bcm43xx_dmaring *ring)
{
	struct device *dev = ring->dev->dev->dev;

	dma_free_coherent(dev, BCM43xx_DMA_RINGMEMSIZE,
			  ring->descbase, ring->dmabase);
}

/* Reset the RX DMA channel */
int bcm43xx_dmacontroller_rx_reset(struct bcm43xx_wldev *dev,
				   u16 mmio_base, int dma64)
{
	int i;
	u32 value;
	u16 offset;

	might_sleep();

	offset = dma64 ? BCM43xx_DMA64_RXCTL : BCM43xx_DMA32_RXCTL;
	bcm43xx_write32(dev, mmio_base + offset, 0);
	for (i = 0; i < 10; i++) {
		offset = dma64 ? BCM43xx_DMA64_RXSTATUS : BCM43xx_DMA32_RXSTATUS;
		value = bcm43xx_read32(dev, mmio_base + offset);
		if (dma64) {
			value &= BCM43xx_DMA64_RXSTAT;
			if (value == BCM43xx_DMA64_RXSTAT_DISABLED) {
				i = -1;
				break;
			}
		} else {
			value &= BCM43xx_DMA32_RXSTATE;
			if (value == BCM43xx_DMA32_RXSTAT_DISABLED) {
				i = -1;
				break;
			}
		}
		msleep(1);
	}
	if (i != -1) {
		printk(KERN_ERR PFX "ERROR: DMA RX reset timed out\n");
		return -ENODEV;
	}

	return 0;
}

/* Reset the RX DMA channel */
int bcm43xx_dmacontroller_tx_reset(struct bcm43xx_wldev *dev,
				   u16 mmio_base, int dma64)
{
	int i;
	u32 value;
	u16 offset;

	might_sleep();

	for (i = 0; i < 10; i++) {
		offset = dma64 ? BCM43xx_DMA64_TXSTATUS : BCM43xx_DMA32_TXSTATUS;
		value = bcm43xx_read32(dev, mmio_base + offset);
		if (dma64) {
			value &= BCM43xx_DMA64_TXSTAT;
			if (value == BCM43xx_DMA64_TXSTAT_DISABLED ||
			    value == BCM43xx_DMA64_TXSTAT_IDLEWAIT ||
			    value == BCM43xx_DMA64_TXSTAT_STOPPED)
				break;
		} else {
			value &= BCM43xx_DMA32_TXSTATE;
			if (value == BCM43xx_DMA32_TXSTAT_DISABLED ||
			    value == BCM43xx_DMA32_TXSTAT_IDLEWAIT ||
			    value == BCM43xx_DMA32_TXSTAT_STOPPED)
				break;
		}
		msleep(1);
	}
	offset = dma64 ? BCM43xx_DMA64_TXCTL : BCM43xx_DMA32_TXCTL;
	bcm43xx_write32(dev, mmio_base + offset, 0);
	for (i = 0; i < 10; i++) {
		offset = dma64 ? BCM43xx_DMA64_TXSTATUS : BCM43xx_DMA32_TXSTATUS;
		value = bcm43xx_read32(dev, mmio_base + offset);
		if (dma64) {
			value &= BCM43xx_DMA64_TXSTAT;
			if (value == BCM43xx_DMA64_TXSTAT_DISABLED) {
				i = -1;
				break;
			}
		} else {
			value &= BCM43xx_DMA32_TXSTATE;
			if (value == BCM43xx_DMA32_TXSTAT_DISABLED) {
				i = -1;
				break;
			}
		}
		msleep(1);
	}
	if (i != -1) {
		printk(KERN_ERR PFX "ERROR: DMA TX reset timed out\n");
		return -ENODEV;
	}
	/* ensure the reset is completed. */
	msleep(1);

	return 0;
}

static int setup_rx_descbuffer(struct bcm43xx_dmaring *ring,
			       struct bcm43xx_dmadesc_generic *desc,
			       struct bcm43xx_dmadesc_meta *meta,
			       gfp_t gfp_flags)
{
	struct bcm43xx_rxhdr_fw4 *rxhdr;
	struct bcm43xx_hwtxstatus *txstat;
	dma_addr_t dmaaddr;
	struct sk_buff *skb;

	assert(!ring->tx);

	skb = __dev_alloc_skb(ring->rx_buffersize, gfp_flags);
	if (unlikely(!skb))
		return -ENOMEM;
	dmaaddr = map_descbuffer(ring, skb->data,
				 ring->rx_buffersize, 0);
	if (dma_mapping_error(dmaaddr)) {
		/* ugh. try to realloc in zone_dma */
		gfp_flags |= GFP_DMA;

		dev_kfree_skb_any(skb);

		skb = __dev_alloc_skb(ring->rx_buffersize, gfp_flags);
		if (unlikely(!skb))
			return -ENOMEM;
		dmaaddr = map_descbuffer(ring, skb->data,
					 ring->rx_buffersize, 0);
	}

	if (dma_mapping_error(dmaaddr)) {
		dev_kfree_skb_any(skb);
		return -EIO;
	}

	meta->skb = skb;
	meta->dmaaddr = dmaaddr;
	ring->ops->fill_descriptor(ring, desc, dmaaddr,
				   ring->rx_buffersize, 0, 0, 0);

	rxhdr = (struct bcm43xx_rxhdr_fw4 *)(skb->data);
	rxhdr->frame_len = 0;
	txstat = (struct bcm43xx_hwtxstatus *)(skb->data);
	txstat->cookie = 0;

	return 0;
}

/* Allocate the initial descbuffers.
 * This is used for an RX ring only.
 */
static int alloc_initial_descbuffers(struct bcm43xx_dmaring *ring)
{
	int i, err = -ENOMEM;
	struct bcm43xx_dmadesc_generic *desc;
	struct bcm43xx_dmadesc_meta *meta;

	for (i = 0; i < ring->nr_slots; i++) {
		desc = ring->ops->idx2desc(ring, i, &meta);

		err = setup_rx_descbuffer(ring, desc, meta, GFP_KERNEL);
		if (err) {
			printk(KERN_ERR PFX "Failed to allocate initial descbuffers\n");
			goto err_unwind;
		}
	}
	mb();
	ring->used_slots = ring->nr_slots;
	err = 0;
out:
	return err;

err_unwind:
	for (i--; i >= 0; i--) {
		desc = ring->ops->idx2desc(ring, i, &meta);

		unmap_descbuffer(ring, meta->dmaaddr, ring->rx_buffersize, 0);
		dev_kfree_skb(meta->skb);
	}
	goto out;
}

/* Do initial setup of the DMA controller.
 * Reset the controller, write the ring busaddress
 * and switch the "enable" bit on.
 */
static int dmacontroller_setup(struct bcm43xx_dmaring *ring)
{
	int err = 0;
	u32 value;
	u32 addrext;
	u32 trans = ssb_dma_translation(ring->dev->dev);

	if (ring->tx) {
		if (ring->dma64) {
			u64 ringbase = (u64)(ring->dmabase);

			addrext = ((ringbase >> 32) & SSB_DMA_TRANSLATION_MASK)
				  >> SSB_DMA_TRANSLATION_SHIFT;
			value = BCM43xx_DMA64_TXENABLE;
			value |= (addrext << BCM43xx_DMA64_TXADDREXT_SHIFT)
				& BCM43xx_DMA64_TXADDREXT_MASK;
			bcm43xx_dma_write(ring, BCM43xx_DMA64_TXCTL, value);
			bcm43xx_dma_write(ring, BCM43xx_DMA64_TXRINGLO,
					(ringbase & 0xFFFFFFFF));
			bcm43xx_dma_write(ring, BCM43xx_DMA64_TXRINGHI,
					((ringbase >> 32) & ~SSB_DMA_TRANSLATION_MASK)
					| trans);
		} else {
			u32 ringbase = (u32)(ring->dmabase);

			addrext = (ringbase & SSB_DMA_TRANSLATION_MASK)
				  >> SSB_DMA_TRANSLATION_SHIFT;
			value = BCM43xx_DMA32_TXENABLE;
			value |= (addrext << BCM43xx_DMA32_TXADDREXT_SHIFT)
				& BCM43xx_DMA32_TXADDREXT_MASK;
			bcm43xx_dma_write(ring, BCM43xx_DMA32_TXCTL, value);
			bcm43xx_dma_write(ring, BCM43xx_DMA32_TXRING,
					(ringbase & ~SSB_DMA_TRANSLATION_MASK)
					| trans);
		}
	} else {
		err = alloc_initial_descbuffers(ring);
		if (err)
			goto out;
		if (ring->dma64) {
			u64 ringbase = (u64)(ring->dmabase);

			addrext = ((ringbase >> 32) & SSB_DMA_TRANSLATION_MASK)
				  >> SSB_DMA_TRANSLATION_SHIFT;
			value = (ring->frameoffset << BCM43xx_DMA64_RXFROFF_SHIFT);
			value |= BCM43xx_DMA64_RXENABLE;
			value |= (addrext << BCM43xx_DMA64_RXADDREXT_SHIFT)
				& BCM43xx_DMA64_RXADDREXT_MASK;
			bcm43xx_dma_write(ring, BCM43xx_DMA64_RXCTL, value);
			bcm43xx_dma_write(ring, BCM43xx_DMA64_RXRINGLO,
					(ringbase & 0xFFFFFFFF));
			bcm43xx_dma_write(ring, BCM43xx_DMA64_RXRINGHI,
					((ringbase >> 32) & ~SSB_DMA_TRANSLATION_MASK)
					| trans);
			bcm43xx_dma_write(ring, BCM43xx_DMA64_RXINDEX, 200);
		} else {
			u32 ringbase = (u32)(ring->dmabase);

			addrext = (ringbase & SSB_DMA_TRANSLATION_MASK)
				  >> SSB_DMA_TRANSLATION_SHIFT;
			value = (ring->frameoffset << BCM43xx_DMA32_RXFROFF_SHIFT);
			value |= BCM43xx_DMA32_RXENABLE;
			value |= (addrext << BCM43xx_DMA32_RXADDREXT_SHIFT)
				& BCM43xx_DMA32_RXADDREXT_MASK;
			bcm43xx_dma_write(ring, BCM43xx_DMA32_RXCTL, value);
			bcm43xx_dma_write(ring, BCM43xx_DMA32_RXRING,
					(ringbase & ~SSB_DMA_TRANSLATION_MASK)
					| trans);
			bcm43xx_dma_write(ring, BCM43xx_DMA32_RXINDEX, 200);
		}
	}

out:
	return err;
}

/* Shutdown the DMA controller. */
static void dmacontroller_cleanup(struct bcm43xx_dmaring *ring)
{
	if (ring->tx) {
		bcm43xx_dmacontroller_tx_reset(ring->dev, ring->mmio_base, ring->dma64);
		if (ring->dma64) {
			bcm43xx_dma_write(ring, BCM43xx_DMA64_TXRINGLO, 0);
			bcm43xx_dma_write(ring, BCM43xx_DMA64_TXRINGHI, 0);
		} else
			bcm43xx_dma_write(ring, BCM43xx_DMA32_TXRING, 0);
	} else {
		bcm43xx_dmacontroller_rx_reset(ring->dev, ring->mmio_base, ring->dma64);
		if (ring->dma64) {
			bcm43xx_dma_write(ring, BCM43xx_DMA64_RXRINGLO, 0);
			bcm43xx_dma_write(ring, BCM43xx_DMA64_RXRINGHI, 0);
		} else
			bcm43xx_dma_write(ring, BCM43xx_DMA32_RXRING, 0);
	}
}

static void free_all_descbuffers(struct bcm43xx_dmaring *ring)
{
	struct bcm43xx_dmadesc_generic *desc;
	struct bcm43xx_dmadesc_meta *meta;
	int i;

	if (!ring->used_slots)
		return;
	for (i = 0; i < ring->nr_slots; i++) {
		desc = ring->ops->idx2desc(ring, i, &meta);

		if (!meta->skb) {
			assert(ring->tx);
			continue;
		}
		if (ring->tx) {
			unmap_descbuffer(ring, meta->dmaaddr,
					meta->skb->len, 1);
		} else {
			unmap_descbuffer(ring, meta->dmaaddr,
					ring->rx_buffersize, 0);
		}
		free_descriptor_buffer(ring, meta, 0);
	}
}

static u64 supported_dma_mask(struct bcm43xx_wldev *dev)
{
	u32 tmp;
	u16 mmio_base;

	tmp = bcm43xx_read32(dev, SSB_TMSHIGH);
	if (tmp & SSB_TMSHIGH_DMA64)
		return DMA_64BIT_MASK;
	mmio_base = bcm43xx_dmacontroller_base(0, 0);
	bcm43xx_write32(dev,
			mmio_base + BCM43xx_DMA32_TXCTL,
			BCM43xx_DMA32_TXADDREXT_MASK);
	tmp = bcm43xx_read32(dev,
			     mmio_base + BCM43xx_DMA32_TXCTL);
	if (tmp & BCM43xx_DMA32_TXADDREXT_MASK)
		return DMA_32BIT_MASK;

	return DMA_30BIT_MASK;
}

/* Main initialization function. */
static
struct bcm43xx_dmaring * bcm43xx_setup_dmaring(struct bcm43xx_wldev *dev,
					       int controller_index,
					       int for_tx,
					       int dma64)
{
	struct bcm43xx_dmaring *ring;
	int err;
	int nr_slots;
	dma_addr_t dma_test;

	ring = kzalloc(sizeof(*ring), GFP_KERNEL);
	if (!ring)
		goto out;

	nr_slots = BCM43xx_RXRING_SLOTS;
	if (for_tx)
		nr_slots = BCM43xx_TXRING_SLOTS;

	ring->meta = kcalloc(nr_slots, sizeof(struct bcm43xx_dmadesc_meta),
			     GFP_KERNEL);
	if (!ring->meta)
		goto err_kfree_ring;
	if (for_tx) {
		ring->txhdr_cache = kcalloc(nr_slots,
					    sizeof(struct bcm43xx_txhdr_fw4),
					    GFP_KERNEL);
		if (!ring->txhdr_cache)
			goto err_kfree_meta;

		/* test for ability to dma to txhdr_cache */
		dma_test = dma_map_single(dev->dev->dev,
				ring->txhdr_cache, sizeof(struct bcm43xx_txhdr_fw4),
				DMA_TO_DEVICE);

		if (dma_mapping_error(dma_test)) {
			/* ugh realloc */
			kfree(ring->txhdr_cache);
			ring->txhdr_cache = kcalloc(nr_slots,
							sizeof(struct bcm43xx_txhdr_fw4),
							GFP_KERNEL | GFP_DMA);
			if (!ring->txhdr_cache)
				goto err_kfree_meta;

			dma_test = dma_map_single(dev->dev->dev,
					ring->txhdr_cache, sizeof(struct bcm43xx_txhdr_fw4),
					DMA_TO_DEVICE);

			if (dma_mapping_error(dma_test))
				goto err_kfree_txhdr_cache;
		}

		dma_unmap_single(dev->dev->dev,
				dma_test, sizeof(struct bcm43xx_txhdr_fw4),
				DMA_TO_DEVICE);
	}

	ring->dev = dev;
	ring->nr_slots = nr_slots;
	ring->mmio_base = bcm43xx_dmacontroller_base(dma64, controller_index);
	ring->index = controller_index;
	ring->dma64 = !!dma64;
	if (dma64)
		ring->ops = &dma64_ops;
	else
		ring->ops = &dma32_ops;
	if (for_tx) {
		ring->tx = 1;
		ring->current_slot = -1;
	} else {
		if (ring->index == 0) {
			ring->rx_buffersize = BCM43xx_DMA0_RX_BUFFERSIZE;
			ring->frameoffset = BCM43xx_DMA0_RX_FRAMEOFFSET;
		} else if (ring->index == 3) {
			ring->rx_buffersize = BCM43xx_DMA3_RX_BUFFERSIZE;
			ring->frameoffset = BCM43xx_DMA3_RX_FRAMEOFFSET;
		} else
			assert(0);
	}
	spin_lock_init(&ring->lock);
#ifdef CONFIG_BCM43XX_MAC80211_DEBUG
	ring->last_injected_overflow = jiffies;
#endif

	err = alloc_ringmemory(ring);
	if (err)
		goto err_kfree_txhdr_cache;
	err = dmacontroller_setup(ring);
	if (err)
		goto err_free_ringmemory;

out:
	return ring;

err_free_ringmemory:
	free_ringmemory(ring);
err_kfree_txhdr_cache:
	kfree(ring->txhdr_cache);
err_kfree_meta:
	kfree(ring->meta);
err_kfree_ring:
	kfree(ring);
	ring = NULL;
	goto out;
}

/* Main cleanup function. */
static void bcm43xx_destroy_dmaring(struct bcm43xx_dmaring *ring)
{
	if (!ring)
		return;

	dprintk(KERN_INFO PFX "DMA-%s 0x%04X (%s) max used slots: %d/%d\n",
		(ring->dma64) ? "64" : "32",
		ring->mmio_base,
		(ring->tx) ? "TX" : "RX",
		ring->max_used_slots, ring->nr_slots);
	/* Device IRQs are disabled prior entering this function,
	 * so no need to take care of concurrency with rx handler stuff.
	 */
	dmacontroller_cleanup(ring);
	free_all_descbuffers(ring);
	free_ringmemory(ring);

	kfree(ring->txhdr_cache);
	kfree(ring->meta);
	kfree(ring);
}

void bcm43xx_dma_free(struct bcm43xx_wldev *dev)
{
	struct bcm43xx_dma *dma;

	if (bcm43xx_using_pio(dev))
		return;
	dma = &dev->dma;

	bcm43xx_destroy_dmaring(dma->rx_ring3);
	dma->rx_ring3 = NULL;
	bcm43xx_destroy_dmaring(dma->rx_ring0);
	dma->rx_ring0 = NULL;

	bcm43xx_destroy_dmaring(dma->tx_ring5);
	dma->tx_ring5 = NULL;
	bcm43xx_destroy_dmaring(dma->tx_ring4);
	dma->tx_ring4 = NULL;
	bcm43xx_destroy_dmaring(dma->tx_ring3);
	dma->tx_ring3 = NULL;
	bcm43xx_destroy_dmaring(dma->tx_ring2);
	dma->tx_ring2 = NULL;
	bcm43xx_destroy_dmaring(dma->tx_ring1);
	dma->tx_ring1 = NULL;
	bcm43xx_destroy_dmaring(dma->tx_ring0);
	dma->tx_ring0 = NULL;
}

int bcm43xx_dma_init(struct bcm43xx_wldev *dev)
{
	struct bcm43xx_dma *dma = &dev->dma;
	struct bcm43xx_dmaring *ring;
	int err;
	u64 dmamask;
	int dma64 = 0;

	dmamask = supported_dma_mask(dev);
	if (dmamask == DMA_64BIT_MASK)
		dma64 = 1;

	err = ssb_dma_set_mask(dev->dev, dmamask);
	if (err) {
#ifdef BCM43XX_MAC80211_PIO
		printk(KERN_WARNING PFX "DMA for this device not supported. "
					"Falling back to PIO\n");
		dev->__using_pio = 1;
		return -EAGAIN;
#else
		printk(KERN_ERR PFX "DMA for this device not supported and "
				    "no PIO support compiled in\n");
		return -EOPNOTSUPP;
#endif
	}

	err = -ENOMEM;
	/* setup TX DMA channels. */
	ring = bcm43xx_setup_dmaring(dev, 0, 1, dma64);
	if (!ring)
		goto out;
	dma->tx_ring0 = ring;

	ring = bcm43xx_setup_dmaring(dev, 1, 1, dma64);
	if (!ring)
		goto err_destroy_tx0;
	dma->tx_ring1 = ring;

	ring = bcm43xx_setup_dmaring(dev, 2, 1, dma64);
	if (!ring)
		goto err_destroy_tx1;
	dma->tx_ring2 = ring;

	ring = bcm43xx_setup_dmaring(dev, 3, 1, dma64);
	if (!ring)
		goto err_destroy_tx2;
	dma->tx_ring3 = ring;

	ring = bcm43xx_setup_dmaring(dev, 4, 1, dma64);
	if (!ring)
		goto err_destroy_tx3;
	dma->tx_ring4 = ring;

	ring = bcm43xx_setup_dmaring(dev, 5, 1, dma64);
	if (!ring)
		goto err_destroy_tx4;
	dma->tx_ring5 = ring;

	/* setup RX DMA channels. */
	ring = bcm43xx_setup_dmaring(dev, 0, 0, dma64);
	if (!ring)
		goto err_destroy_tx5;
	dma->rx_ring0 = ring;

	if (dev->dev->id.revision < 5) {
		ring = bcm43xx_setup_dmaring(dev, 3, 0, dma64);
		if (!ring)
			goto err_destroy_rx0;
		dma->rx_ring3 = ring;
	}

	dprintk(KERN_INFO PFX "%d-bit DMA initialized\n",
		(dmamask == DMA_64BIT_MASK) ? 64 :
		(dmamask == DMA_32BIT_MASK) ? 32 : 30);
	err = 0;
out:
	return err;

err_destroy_rx0:
	bcm43xx_destroy_dmaring(dma->rx_ring0);
	dma->rx_ring0 = NULL;
err_destroy_tx5:
	bcm43xx_destroy_dmaring(dma->tx_ring5);
	dma->tx_ring5 = NULL;
err_destroy_tx4:
	bcm43xx_destroy_dmaring(dma->tx_ring4);
	dma->tx_ring4 = NULL;
err_destroy_tx3:
	bcm43xx_destroy_dmaring(dma->tx_ring3);
	dma->tx_ring3 = NULL;
err_destroy_tx2:
	bcm43xx_destroy_dmaring(dma->tx_ring2);
	dma->tx_ring2 = NULL;
err_destroy_tx1:
	bcm43xx_destroy_dmaring(dma->tx_ring1);
	dma->tx_ring1 = NULL;
err_destroy_tx0:
	bcm43xx_destroy_dmaring(dma->tx_ring0);
	dma->tx_ring0 = NULL;
	goto out;
}

/* Generate a cookie for the TX header. */
static u16 generate_cookie(struct bcm43xx_dmaring *ring,
			   int slot)
{
	u16 cookie = 0x1000;

	/* Use the upper 4 bits of the cookie as
	 * DMA controller ID and store the slot number
	 * in the lower 12 bits.
	 * Note that the cookie must never be 0, as this
	 * is a special value used in RX path.
	 */
	switch (ring->index) {
	case 0:
		cookie = 0xA000;
		break;
	case 1:
		cookie = 0xB000;
		break;
	case 2:
		cookie = 0xC000;
		break;
	case 3:
		cookie = 0xD000;
		break;
	case 4:
		cookie = 0xE000;
		break;
	case 5:
		cookie = 0xF000;
		break;
	}
	assert(((u16)slot & 0xF000) == 0x0000);
	cookie |= (u16)slot;

	return cookie;
}

/* Inspect a cookie and find out to which controller/slot it belongs. */
static
struct bcm43xx_dmaring * parse_cookie(struct bcm43xx_wldev *dev,
				      u16 cookie, int *slot)
{
	struct bcm43xx_dma *dma = &dev->dma;
	struct bcm43xx_dmaring *ring = NULL;

	switch (cookie & 0xF000) {
	case 0xA000:
		ring = dma->tx_ring0;
		break;
	case 0xB000:
		ring = dma->tx_ring1;
		break;
	case 0xC000:
		ring = dma->tx_ring2;
		break;
	case 0xD000:
		ring = dma->tx_ring3;
		break;
	case 0xE000:
		ring = dma->tx_ring4;
		break;
	case 0xF000:
		ring = dma->tx_ring5;
		break;
	default:
		assert(0);
	}
	*slot = (cookie & 0x0FFF);
	assert(ring && *slot >= 0 && *slot < ring->nr_slots);

	return ring;
}

static int dma_tx_fragment(struct bcm43xx_dmaring *ring,
			    struct sk_buff *skb,
			    struct ieee80211_tx_control *ctl)
{
	const struct bcm43xx_dma_ops *ops = ring->ops;
	u8 *header;
	int slot;
	int err;
	struct bcm43xx_dmadesc_generic *desc;
	struct bcm43xx_dmadesc_meta *meta;
	struct bcm43xx_dmadesc_meta *meta_hdr;
	struct sk_buff *bounce_skb;

#define SLOTS_PER_PACKET  2
	assert(skb_shinfo(skb)->nr_frags == 0);

	/* Get a slot for the header. */
	slot = request_slot(ring);
	desc = ops->idx2desc(ring, slot, &meta_hdr);
	memset(meta_hdr, 0, sizeof(*meta_hdr));

	header = &(ring->txhdr_cache[slot * sizeof(struct bcm43xx_txhdr_fw4)]);
	bcm43xx_generate_txhdr(ring->dev, header,
			       skb->data, skb->len, ctl,
			       generate_cookie(ring, slot));

	meta_hdr->dmaaddr = map_descbuffer(ring, (unsigned char *)header,
				       sizeof(struct bcm43xx_txhdr_fw4), 1);
	if (dma_mapping_error(meta_hdr->dmaaddr))
		return -EIO;
	ops->fill_descriptor(ring, desc, meta_hdr->dmaaddr,
			     sizeof(struct bcm43xx_txhdr_fw4), 1, 0, 0);

	/* Get a slot for the payload. */
	slot = request_slot(ring);
	desc = ops->idx2desc(ring, slot, &meta);
	memset(meta, 0, sizeof(*meta));

	memcpy(&meta->txstat.control, ctl, sizeof(*ctl));
	meta->skb = skb;
	meta->is_last_fragment = 1;

	meta->dmaaddr = map_descbuffer(ring, skb->data, skb->len, 1);
	/* create a bounce buffer in zone_dma on mapping failure. */
	if (dma_mapping_error(meta->dmaaddr)) {
		bounce_skb = __dev_alloc_skb(skb->len, GFP_ATOMIC | GFP_DMA);
		if (!bounce_skb) {
			err = -ENOMEM;
			goto out_unmap_hdr;
		}

		memcpy(skb_put(bounce_skb, skb->len), skb->data, skb->len);
		dev_kfree_skb_any(skb);
		skb = bounce_skb;
		meta->skb = skb;
		meta->dmaaddr = map_descbuffer(ring, skb->data, skb->len, 1);
		if (dma_mapping_error(meta->dmaaddr)) {
			err = -EIO;
			goto out_free_bounce;
		}
	}

	ops->fill_descriptor(ring, desc, meta->dmaaddr,
			     skb->len, 0, 1, 1);

	/* Now transfer the whole frame. */
	wmb();
	ops->poke_tx(ring, next_slot(ring, slot));
	return 0;

out_free_bounce:
	dev_kfree_skb_any(skb);
out_unmap_hdr:
	unmap_descbuffer(ring, meta_hdr->dmaaddr,
			sizeof(struct bcm43xx_txhdr_fw4), 1);
	return err;
}

static inline
int should_inject_overflow(struct bcm43xx_dmaring *ring)
{
#ifdef CONFIG_BCM43XX_MAC80211_DEBUG
	if (unlikely(bcm43xx_debug(ring->dev, BCM43xx_DBG_DMAOVERFLOW))) {
		/* Check if we should inject another ringbuffer overflow
		 * to test handling of this situation in the stack. */
		unsigned long next_overflow;

		next_overflow = ring->last_injected_overflow + HZ;
		if (time_after(jiffies, next_overflow)) {
			ring->last_injected_overflow = jiffies;
			dprintk(KERN_DEBUG PFX "Injecting TX ring overflow on "
				"DMA controller %d\n", ring->index);
			return 1;
		}
	}
#endif /* CONFIG_BCM43XX_MAC80211_DEBUG */
	return 0;
}

int bcm43xx_dma_tx(struct bcm43xx_wldev *dev,
		   struct sk_buff *skb,
		   struct ieee80211_tx_control *ctl)
{
	struct bcm43xx_dmaring *ring;
	int err = 0;
	unsigned long flags;

	ring = priority_to_txring(dev, ctl->queue);
	spin_lock_irqsave(&ring->lock, flags);
	assert(ring->tx);
	if (unlikely(free_slots(ring) < SLOTS_PER_PACKET)) {
		printkl(KERN_ERR PFX "DMA queue overflow\n");
		err = -ENOSPC;
		goto out_unlock;
	}
	/* Check if the queue was stopped in mac80211,
	 * but we got called nevertheless.
	 * That would be a mac80211 bug. */
	assert(!ring->stopped);

	err = dma_tx_fragment(ring, skb, ctl);
	if (unlikely(err)) {
		printkl(KERN_ERR PFX "DMA tx mapping failure\n");
		goto out_unlock;
	}
	ring->nr_tx_packets++;
	if ((free_slots(ring) < SLOTS_PER_PACKET) ||
	    should_inject_overflow(ring)) {
		/* This TX ring is full. */
		ieee80211_stop_queue(dev->wl->hw, txring_to_priority(ring));
		ring->stopped = 1;
		if (bcm43xx_debug(dev, BCM43xx_DBG_DMAVERBOSE)) {
			dprintk(KERN_DEBUG PFX "Stopped TX ring %d\n",
				ring->index);
		}
	}
out_unlock:
	spin_unlock_irqrestore(&ring->lock, flags);

	return err;
}

void bcm43xx_dma_handle_txstatus(struct bcm43xx_wldev *dev,
				 const struct bcm43xx_txstatus *status)
{
	const struct bcm43xx_dma_ops *ops;
	struct bcm43xx_dmaring *ring;
	struct bcm43xx_dmadesc_generic *desc;
	struct bcm43xx_dmadesc_meta *meta;
	int slot;

	ring = parse_cookie(dev, status->cookie, &slot);
	if (unlikely(!ring))
		return;
	assert(irqs_disabled());
	spin_lock(&ring->lock);

	assert(ring->tx);
	ops = ring->ops;
	while (1) {
		assert(slot >= 0 && slot < ring->nr_slots);
		desc = ops->idx2desc(ring, slot, &meta);

		if (meta->skb)
			unmap_descbuffer(ring, meta->dmaaddr, meta->skb->len, 1);
		else
			unmap_descbuffer(ring, meta->dmaaddr, sizeof(struct bcm43xx_txhdr_fw4), 1);

		if (meta->is_last_fragment) {
			assert(meta->skb);
			/* Call back to inform the ieee80211 subsystem about the
			 * status of the transmission.
			 * Some fields of txstat are already filled in dma_tx().
			 */
			if (status->acked)
				meta->txstat.flags |= IEEE80211_TX_STATUS_ACK;
			meta->txstat.retry_count = status->frame_count - 1;
			ieee80211_tx_status_irqsafe(dev->wl->hw, meta->skb, &(meta->txstat));
			/* skb is freed by ieee80211_tx_status_irqsafe() */
			meta->skb = NULL;
		} else {
			/* No need to call free_descriptor_buffer here, as
			 * this is only the txhdr, which is not allocated.
			 */
			assert(meta->skb == NULL);
		}

		/* Everything unmapped and free'd. So it's not used anymore. */
		ring->used_slots--;

		if (meta->is_last_fragment)
			break;
		slot = next_slot(ring, slot);
	}
	dev->stats.last_tx = jiffies;
	if (ring->stopped) {
		assert(free_slots(ring) >= SLOTS_PER_PACKET);
		ieee80211_wake_queue(dev->wl->hw, txring_to_priority(ring));
		ring->stopped = 0;
		if (bcm43xx_debug(dev, BCM43xx_DBG_DMAVERBOSE)) {
			dprintk(KERN_DEBUG PFX "Woke up TX ring %d\n",
				ring->index);
		}
	}

	spin_unlock(&ring->lock);
}

void bcm43xx_dma_get_tx_stats(struct bcm43xx_wldev *dev,
			      struct ieee80211_tx_queue_stats *stats)
{
	const int nr_queues = dev->wl->hw->queues;
	struct bcm43xx_dmaring *ring;
	struct ieee80211_tx_queue_stats_data *data;
	unsigned long flags;
	int i;

	for (i = 0; i < nr_queues; i++) {
		data = &(stats->data[i]);
		ring = priority_to_txring(dev, i);

		spin_lock_irqsave(&ring->lock, flags);
		data->len = ring->used_slots / SLOTS_PER_PACKET;
		data->limit = ring->nr_slots / SLOTS_PER_PACKET;
		data->count = ring->nr_tx_packets;
		spin_unlock_irqrestore(&ring->lock, flags);
	}
}

static void dma_rx(struct bcm43xx_dmaring *ring,
		   int *slot)
{
	const struct bcm43xx_dma_ops *ops = ring->ops;
	struct bcm43xx_dmadesc_generic *desc;
	struct bcm43xx_dmadesc_meta *meta;
	struct bcm43xx_rxhdr_fw4 *rxhdr;
	struct sk_buff *skb;
	u16 len;
	int err;
	dma_addr_t dmaaddr;

	desc = ops->idx2desc(ring, *slot, &meta);

	sync_descbuffer_for_cpu(ring, meta->dmaaddr, ring->rx_buffersize);
	skb = meta->skb;

	if (ring->index == 3) {
		/* We received an xmit status. */
		struct bcm43xx_hwtxstatus *hw = (struct bcm43xx_hwtxstatus *)skb->data;
		int i = 0;

		while (hw->cookie == 0) {
			if (i > 100)
				break;
			i++;
			udelay(2);
			barrier();
		}
		bcm43xx_handle_hwtxstatus(ring->dev, hw);
		/* recycle the descriptor buffer. */
		sync_descbuffer_for_device(ring, meta->dmaaddr, ring->rx_buffersize);

		return;
	}
	rxhdr = (struct bcm43xx_rxhdr_fw4 *)skb->data;
	len = le16_to_cpu(rxhdr->frame_len);
	if (len == 0) {
		int i = 0;

		do {
			udelay(2);
			barrier();
			len = le16_to_cpu(rxhdr->frame_len);
		} while (len == 0 && i++ < 5);
		if (unlikely(len == 0)) {
			/* recycle the descriptor buffer. */
			sync_descbuffer_for_device(ring, meta->dmaaddr,
						   ring->rx_buffersize);
			goto drop;
		}
	}
	if (unlikely(len > ring->rx_buffersize)) {
		/* The data did not fit into one descriptor buffer
		 * and is split over multiple buffers.
		 * This should never happen, as we try to allocate buffers
		 * big enough. So simply ignore this packet.
		 */
		int cnt = 0;
		s32 tmp = len;

		while (1) {
			desc = ops->idx2desc(ring, *slot, &meta);
			/* recycle the descriptor buffer. */
			sync_descbuffer_for_device(ring, meta->dmaaddr,
						   ring->rx_buffersize);
			*slot = next_slot(ring, *slot);
			cnt++;
			tmp -= ring->rx_buffersize;
			if (tmp <= 0)
				break;
		}
		printkl(KERN_ERR PFX "DMA RX buffer too small "
			"(len: %u, buffer: %u, nr-dropped: %d)\n",
			len, ring->rx_buffersize, cnt);
		goto drop;
	}

	dmaaddr = meta->dmaaddr;
	err = setup_rx_descbuffer(ring, desc, meta, GFP_ATOMIC);
	if (unlikely(err)) {
		dprintkl(KERN_ERR PFX "DMA RX: setup_rx_descbuffer() failed\n");
		sync_descbuffer_for_device(ring, dmaaddr,
					   ring->rx_buffersize);
		goto drop;
	}

	unmap_descbuffer(ring, dmaaddr, ring->rx_buffersize, 0);
	skb_put(skb, len + ring->frameoffset);
	skb_pull(skb, ring->frameoffset);

	bcm43xx_rx(ring->dev, skb, rxhdr);
drop:
	return;
}

void bcm43xx_dma_rx(struct bcm43xx_dmaring *ring)
{
	const struct bcm43xx_dma_ops *ops = ring->ops;
	int slot, current_slot;
	int used_slots = 0;

	assert(!ring->tx);
	current_slot = ops->get_current_rxslot(ring);
	assert(current_slot >= 0 && current_slot < ring->nr_slots);

	slot = ring->current_slot;
	for ( ; slot != current_slot; slot = next_slot(ring, slot)) {
		dma_rx(ring, &slot);
		update_max_used_slots(ring, ++used_slots);
	}
	ops->set_current_rxslot(ring, slot);
	ring->current_slot = slot;
}

static void bcm43xx_dma_tx_suspend_ring(struct bcm43xx_dmaring *ring)
{
	unsigned long flags;

	spin_lock_irqsave(&ring->lock, flags);
	assert(ring->tx);
	ring->ops->tx_suspend(ring);
	spin_unlock_irqrestore(&ring->lock, flags);
}

static void bcm43xx_dma_tx_resume_ring(struct bcm43xx_dmaring *ring)
{
	unsigned long flags;

	spin_lock_irqsave(&ring->lock, flags);
	assert(ring->tx);
	ring->ops->tx_resume(ring);
	spin_unlock_irqrestore(&ring->lock, flags);
}

void bcm43xx_dma_tx_suspend(struct bcm43xx_wldev *dev)
{
	bcm43xx_power_saving_ctl_bits(dev, -1, 1);
	bcm43xx_dma_tx_suspend_ring(dev->dma.tx_ring0);
	bcm43xx_dma_tx_suspend_ring(dev->dma.tx_ring1);
	bcm43xx_dma_tx_suspend_ring(dev->dma.tx_ring2);
	bcm43xx_dma_tx_suspend_ring(dev->dma.tx_ring3);
	bcm43xx_dma_tx_suspend_ring(dev->dma.tx_ring4);
	bcm43xx_dma_tx_suspend_ring(dev->dma.tx_ring5);
}

void bcm43xx_dma_tx_resume(struct bcm43xx_wldev *dev)
{
	bcm43xx_dma_tx_resume_ring(dev->dma.tx_ring5);
	bcm43xx_dma_tx_resume_ring(dev->dma.tx_ring4);
	bcm43xx_dma_tx_resume_ring(dev->dma.tx_ring3);
	bcm43xx_dma_tx_resume_ring(dev->dma.tx_ring2);
	bcm43xx_dma_tx_resume_ring(dev->dma.tx_ring1);
	bcm43xx_dma_tx_resume_ring(dev->dma.tx_ring0);
	bcm43xx_power_saving_ctl_bits(dev, -1, -1);
}
