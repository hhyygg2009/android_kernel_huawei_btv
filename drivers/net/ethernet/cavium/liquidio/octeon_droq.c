/**********************************************************************
* Author: Cavium, Inc.
*
* Contact: support@cavium.com
*          Please include "LiquidIO" in the subject.
*
* Copyright (c) 2003-2015 Cavium, Inc.
*
* This file is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, Version 2, as
* published by the Free Software Foundation.
*
* This file is distributed in the hope that it will be useful, but
* AS-IS and WITHOUT ANY WARRANTY; without even the implied warranty
* of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE, TITLE, or
* NONINFRINGEMENT.  See the GNU General Public License for more
* details.
*
* This file may also be available under a different license from Cavium.
* Contact Cavium, Inc. for more information
**********************************************************************/
#include <linux/version.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include "octeon_config.h"
#include "liquidio_common.h"
#include "octeon_droq.h"
#include "octeon_iq.h"
#include "response_manager.h"
#include "octeon_device.h"
#include "octeon_nic.h"
#include "octeon_main.h"
#include "octeon_network.h"
#include "cn66xx_regs.h"
#include "cn66xx_device.h"
#include "cn68xx_regs.h"
#include "cn68xx_device.h"
#include "liquidio_image.h"
#include "octeon_mem_ops.h"

/* #define CAVIUM_ONLY_PERF_MODE */

#define     CVM_MIN(d1, d2)           (((d1) < (d2)) ? (d1) : (d2))
#define     CVM_MAX(d1, d2)           (((d1) > (d2)) ? (d1) : (d2))

struct niclist {
	struct list_head list;
	void *ptr;
};

struct __dispatch {
	struct list_head list;
	struct octeon_recv_info *rinfo;
	octeon_dispatch_fn_t disp_fn;
};

/** Get the argument that the user set when registering dispatch
 *  function for a given opcode/subcode.
 *  @param  octeon_dev - the octeon device pointer.
 *  @param  opcode     - the opcode for which the dispatch argument
 *                       is to be checked.
 *  @param  subcode    - the subcode for which the dispatch argument
 *                       is to be checked.
 *  @return  Success: void * (argument to the dispatch function)
 *  @return  Failure: NULL
 *
 */
static inline void *octeon_get_dispatch_arg(struct octeon_device *octeon_dev,
					    u16 opcode, u16 subcode)
{
	int idx;
	struct list_head *dispatch;
	void *fn_arg = NULL;
	u16 combined_opcode = OPCODE_SUBCODE(opcode, subcode);

	idx = combined_opcode & OCTEON_OPCODE_MASK;

	spin_lock_bh(&octeon_dev->dispatch.lock);

	if (octeon_dev->dispatch.count == 0) {
		spin_unlock_bh(&octeon_dev->dispatch.lock);
		return NULL;
	}

	if (octeon_dev->dispatch.dlist[idx].opcode == combined_opcode) {
		fn_arg = octeon_dev->dispatch.dlist[idx].arg;
	} else {
		list_for_each(dispatch,
			      &octeon_dev->dispatch.dlist[idx].list) {
			if (((struct octeon_dispatch *)dispatch)->opcode ==
			    combined_opcode) {
				fn_arg = ((struct octeon_dispatch *)
					  dispatch)->arg;
				break;
			}
		}
	}

	spin_unlock_bh(&octeon_dev->dispatch.lock);
	return fn_arg;
}

u32 octeon_droq_check_hw_for_pkts(struct octeon_device *oct,
				  struct octeon_droq *droq)
{
	u32 pkt_count = 0;

	pkt_count = readl(droq->pkts_sent_reg);
	if (pkt_count) {
		atomic_add(pkt_count, &droq->pkts_pending);
		writel(pkt_count, droq->pkts_sent_reg);
	}

	return pkt_count;
}

static void octeon_droq_compute_max_packet_bufs(struct octeon_droq *droq)
{
	u32 count = 0;

	/* max_empty_descs is the max. no. of descs that can have no buffers.
	 * If the empty desc count goes beyond this value, we cannot safely
	 * read in a 64K packet sent by Octeon
	 * (64K is max pkt size from Octeon)
	 */
	droq->max_empty_descs = 0;

	do {
		droq->max_empty_descs++;
		count += droq->buffer_size;
	} while (count < (64 * 1024));

	droq->max_empty_descs = droq->max_count - droq->max_empty_descs;
}

static void octeon_droq_reset_indices(struct octeon_droq *droq)
{
	droq->read_idx = 0;
	droq->write_idx = 0;
	droq->refill_idx = 0;
	droq->refill_count = 0;
	atomic_set(&droq->pkts_pending, 0);
}

static void
octeon_droq_destroy_ring_buffers(struct octeon_device *oct,
				 struct octeon_droq *droq)
{
	u32 i;

	for (i = 0; i < droq->max_count; i++) {
		if (droq->recv_buf_list[i].buffer) {
			if (droq->desc_ring) {
				lio_unmap_ring_info(oct->pci_dev,
						    (u64)droq->
						    desc_ring[i].info_ptr,
						    OCT_DROQ_INFO_SIZE);
				lio_unmap_ring(oct->pci_dev,
					       (u64)droq->desc_ring[i].
					       buffer_ptr,
					       droq->buffer_size);
			}
			recv_buffer_free(droq->recv_buf_list[i].buffer);
			droq->recv_buf_list[i].buffer = NULL;
		}
	}

	octeon_droq_reset_indices(droq);
}

static int
octeon_droq_setup_ring_buffers(struct octeon_device *oct,
			       struct octeon_droq *droq)
{
	u32 i;
	void *buf;
	struct octeon_droq_desc *desc_ring = droq->desc_ring;

	for (i = 0; i < droq->max_count; i++) {
		buf = recv_buffer_alloc(oct, droq->q_no, droq->buffer_size);

		if (!buf) {
			dev_err(&oct->pci_dev->dev, "%s buffer alloc failed\n",
				__func__);
			return -ENOMEM;
		}

		droq->recv_buf_list[i].buffer = buf;
		droq->recv_buf_list[i].data = get_rbd(buf);

		droq->info_list[i].length = 0;

		/* map ring buffers into memory */
		desc_ring[i].info_ptr = lio_map_ring_info(droq, i);
		desc_ring[i].buffer_ptr =
			lio_map_ring(oct->pci_dev,
				     droq->recv_buf_list[i].buffer,
				     droq->buffer_size);
	}

	octeon_droq_reset_indices(droq);

	octeon_droq_compute_max_packet_bufs(droq);

	return 0;
}

int octeon_delete_droq(struct octeon_device *oct, u32 q_no)
{
	struct octeon_droq *droq = oct->droq[q_no];

	dev_dbg(&oct->pci_dev->dev, "%s[%d]\n", __func__, q_no);

	octeon_droq_destroy_ring_buffers(oct, droq);
	vfree(droq->recv_buf_list);

	if (droq->info_base_addr)
		cnnic_free_aligned_dma(oct->pci_dev, droq->info_list,
				       droq->info_alloc_size,
				       droq->info_base_addr,
				       droq->info_list_dma);

	if (droq->desc_ring)
		lio_dma_free(oct, (droq->max_count * OCT_DROQ_DESC_SIZE),
			     droq->desc_ring, droq->desc_ring_dma);

	memset(droq, 0, OCT_DROQ_SIZE);

	return 0;
}

int octeon_init_droq(struct octeon_device *oct,
		     u32 q_no,
		     u32 num_descs,
		     u32 desc_size,
		     void *app_ctx)
{
	struct octeon_droq *droq;
	u32 desc_ring_size = 0, c_num_descs = 0, c_buf_size = 0;
	u32 c_pkts_per_intr = 0, c_refill_threshold = 0;

	dev_dbg(&oct->pci_dev->dev, "%s[%d]\n", __func__, q_no);

	droq = oct->droq[q_no];
	memset(droq, 0, OCT_DROQ_SIZE);

	droq->oct_dev = oct;
	droq->q_no = q_no;
	if (app_ctx)
		droq->app_ctx = app_ctx;
	else
		droq->app_ctx = (void *)(size_t)q_no;

	c_num_descs = num_descs;
	c_buf_size = desc_size;
	if (OCTEON_CN6XXX(oct)) {
		struct octeon_config *conf6x = CHIP_FIELD(oct, cn6xxx, conf);

		c_pkts_per_intr = (u32)CFG_GET_OQ_PKTS_PER_INTR(conf6x);
		c_refill_threshold = (u32)CFG_GET_OQ_REFILL_THRESHOLD(conf6x);
	}

	droq->max_count = c_num_descs;
	droq->buffer_size = c_buf_size;

	desc_ring_size = droq->max_count * OCT_DROQ_DESC_SIZE;
	droq->desc_ring = lio_dma_alloc(oct, desc_ring_size,
					(dma_addr_t *)&droq->desc_ring_dma);

	if (!droq->desc_ring) {
		dev_err(&oct->pci_dev->dev,
			"Output queue %d ring alloc failed\n", q_no);
		return 1;
	}

	dev_dbg(&oct->pci_dev->dev, "droq[%d]: desc_ring: virt: 0x%p, dma: %lx\n",
		q_no, droq->desc_ring, droq->desc_ring_dma);
	dev_dbg(&oct->pci_dev->dev, "droq[%d]: num_desc: %d\n", q_no,
		droq->max_count);

	droq->info_list =
		cnnic_alloc_aligned_dma(oct->pci_dev,
					(droq->max_count * OCT_DROQ_INFO_SIZE),
					&droq->info_alloc_size,
					&droq->info_base_addr,
					&droq->info_list_dma);

	if (!droq->info_list) {
		dev_err(&oct->pci_dev->dev, "Cannot allocate memory for info list.\n");
		lio_dma_free(oct, (droq->max_count * OCT_DROQ_DESC_SIZE),
			     droq->desc_ring, droq->desc_ring_dma);
		return 1;
	}

	droq->recv_buf_list = (struct octeon_recv_buffer *)
			      vmalloc(droq->max_count *
						OCT_DROQ_RECVBUF_SIZE);
	if (!droq->recv_buf_list) {
		dev_err(&oct->pci_dev->dev, "Output queue recv buf list alloc failed\n");
		goto init_droq_fail;
	}

	if (octeon_droq_setup_ring_buffers(oct, droq))
		goto init_droq_fail;

	droq->pkts_per_intr = c_pkts_per_intr;
	droq->refill_threshold = c_refill_threshold;

	dev_dbg(&oct->pci_dev->dev, "DROQ INIT: max_empty_descs: %d\n",
		droq->max_empty_descs);

	spin_lock_init(&droq->lock);

	INIT_LIST_HEAD(&droq->dispatch_list);

	/* For 56xx Pass1, this function won't be called, so no checks. */
	oct->fn_list.setup_oq_regs(oct, q_no);

	oct->io_qmask.oq |= (1 << q_no);

	return 0;

init_droq_fail:
	octeon_delete_droq(oct, q_no);
	return 1;
}

/* octeon_create_recv_info
 * Parameters:
 *  octeon_dev - pointer to the octeon device structure
 *  droq       - droq in which the packet arrived.
 *  buf_cnt    - no. of buffers used by the packet.
 *  idx        - index in the descriptor for the first buffer in the packet.
 * Description:
 *  Allocates a recv_info_t and copies the buffer addresses for packet data
 *  into the recv_pkt space which starts at an 8B offset from recv_info_t.
 *  Flags the descriptors for refill later. If available descriptors go
 *  below the threshold to receive a 64K pkt, new buffers are first allocated
 *  before the recv_pkt_t is created.
 *  This routine will be called in interrupt context.
 * Returns:
 *  Success: Pointer to recv_info_t
 *  Failure: NULL.
 * Locks:
 *  The droq->lock is held when this routine is called.
 */
static inline struct octeon_recv_info *octeon_create_recv_info(
		struct octeon_device *octeon_dev,
		struct octeon_droq *droq,
		u32 buf_cnt,
		u32 idx)
{
	struct octeon_droq_info *info;
	struct octeon_recv_pkt *recv_pkt;
	struct octeon_recv_info *recv_info;
	u32 i, bytes_left;

	info = &droq->info_list[idx];

	recv_info = octeon_alloc_recv_info(sizeof(struct __dispatch));
	if (!recv_info)
		return NULL;

	recv_pkt = recv_info->recv_pkt;
	recv_pkt->rh = info->rh;
	recv_pkt->length = (u32)info->length;
	recv_pkt->buffer_count = (u16)buf_cnt;
	recv_pkt->octeon_id = (u16)octeon_dev->octeon_id;

	i = 0;
	bytes_left = (u32)info->length;

	while (buf_cnt) {
		lio_unmap_ring(octeon_dev->pci_dev,
			       (u64)droq->desc_ring[idx].buffer_ptr,
			       droq->buffer_size);

		recv_pkt->buffer_size[i] =
			(bytes_left >=
			 droq->buffer_size) ? droq->buffer_size : bytes_left;

		recv_pkt->buffer_ptr[i] = droq->recv_buf_list[idx].buffer;
		droq->recv_buf_list[idx].buffer = NULL;

		INCR_INDEX_BY1(idx, droq->max_count);
		bytes_left -= droq->buffer_size;
		i++;
		buf_cnt--;
	}

	return recv_info;
}

/* If we were not able to refill all buffers, try to move around
 * the buffers that were not dispatched.
 */
static inline u32
octeon_droq_refill_pullup_descs(struct octeon_droq *droq,
				struct octeon_droq_desc *desc_ring)
{
	u32 desc_refilled = 0;

	u32 refill_index = droq->refill_idx;

	while (refill_index != droq->read_idx) {
		if (droq->recv_buf_list[refill_index].buffer) {
			droq->recv_buf_list[droq->refill_idx].buffer =
				droq->recv_buf_list[refill_index].buffer;
			droq->recv_buf_list[droq->refill_idx].data =
				droq->recv_buf_list[refill_index].data;
			desc_ring[droq->refill_idx].buffer_ptr =
				desc_ring[refill_index].buffer_ptr;
			droq->recv_buf_list[refill_index].buffer = NULL;
			desc_ring[refill_index].buffer_ptr = 0;
			do {
				INCR_INDEX_BY1(droq->refill_idx,
					       droq->max_count);
				desc_refilled++;
				droq->refill_count--;
			} while (droq->recv_buf_list[droq->refill_idx].
				 buffer);
		}
		INCR_INDEX_BY1(refill_index, droq->max_count);
	}                       /* while */
	return desc_refilled;
}

/* octeon_droq_refill
 * Parameters:
 *  droq       - droq in which descriptors require new buffers.
 * Description:
 *  Called during normal DROQ processing in interrupt mode or by the poll
 *  thread to refill the descriptors from which buffers were dispatched
 *  to upper layers. Attempts to allocate new buffers. If that fails, moves
 *  up buffers (that were not dispatched) to form a contiguous ring.
 * Returns:
 *  No of descriptors refilled.
 * Locks:
 *  This routine is called with droq->lock held.
 */
static u32
octeon_droq_refill(struct octeon_device *octeon_dev, struct octeon_droq *droq)
{
	struct octeon_droq_desc *desc_ring;
	void *buf = NULL;
	u8 *data;
	u32 desc_refilled = 0;

	desc_ring = droq->desc_ring;

	while (droq->refill_count && (desc_refilled < droq->max_count)) {
		/* If a valid buffer exists (happens if there is no dispatch),
		 * reuse
		 * the buffer, else allocate.
		 */
		if (!droq->recv_buf_list[droq->refill_idx].buffer) {
			buf = recv_buffer_alloc(octeon_dev, droq->q_no,
						droq->buffer_size);
			/* If a buffer could not be allocated, no point in
			 * continuing
			 */
			if (!buf)
				break;
			droq->recv_buf_list[droq->refill_idx].buffer =
				buf;
			data = get_rbd(buf);
		} else {
			data = get_rbd(droq->recv_buf_list
				       [droq->refill_idx].buffer);
		}

		droq->recv_buf_list[droq->refill_idx].data = data;

		desc_ring[droq->refill_idx].buffer_ptr =
			lio_map_ring(octeon_dev->pci_dev,
				     droq->recv_buf_list[droq->
				     refill_idx].buffer,
				     droq->buffer_size);

		/* Reset any previous values in the length field. */
		droq->info_list[droq->refill_idx].length = 0;

		INCR_INDEX_BY1(droq->refill_idx, droq->max_count);
		desc_refilled++;
		droq->refill_count--;
	}

	if (droq->refill_count)
		desc_refilled +=
			octeon_droq_refill_pullup_descs(droq, desc_ring);

	/* if droq->refill_count
	 * The refill count would not change in pass two. We only moved buffers
	 * to close the gap in the ring, but we would still have the same no. of
	 * buffers to refill.
	 */
	return desc_refilled;
}

static inline u32
octeon_droq_get_bufcount(u32 buf_size, u32 total_len)
{
	u32 buf_cnt = 0;

	while (total_len > (buf_size * buf_cnt))
		buf_cnt++;
	return buf_cnt;
}

static int
octeon_droq_dispatch_pkt(struct octeon_device *oct,
			 struct octeon_droq *droq,
			 union octeon_rh *rh,
			 struct octeon_droq_info *info)
{
	u32 cnt;
	octeon_dispatch_fn_t disp_fn;
	struct octeon_recv_info *rinfo;

	cnt = octeon_droq_get_bufcount(droq->buffer_size, (u32)info->length);

	disp_fn = octeon_get_dispatch(oct, (u16)rh->r.opcode,
				      (u16)rh->r.subcode);
	if (disp_fn) {
		rinfo = octeon_create_recv_info(oct, droq, cnt, droq->read_idx);
		if (rinfo) {
			struct __dispatch *rdisp = rinfo->rsvd;

			rdisp->rinfo = rinfo;
			rdisp->disp_fn = disp_fn;
			rinfo->recv_pkt->rh = *rh;
			list_add_tail(&rdisp->list,
				      &droq->dispatch_list);
		} else {
			droq->stats.dropped_nomem++;
		}
	} else {
		dev_err(&oct->pci_dev->dev, "DROQ: No dispatch function\n");
		droq->stats.dropped_nodispatch++;
	}                       /* else (dispatch_fn ... */

	return cnt;
}

static inline void octeon_droq_drop_packets(struct octeon_device *oct,
					    struct octeon_droq *droq,
					    u32 cnt)
{
	u32 i = 0, buf_cnt;
	struct octeon_droq_info *info;

	for (i = 0; i < cnt; i++) {
		info = &droq->info_list[droq->read_idx];
		octeon_swap_8B_data((u64 *)info, 2);

		if (info->length) {
			info->length -= OCT_RH_SIZE;
			droq->stats.bytes_received += info->length;
			buf_cnt = octeon_droq_get_bufcount(droq->buffer_size,
							   (u32)info->length);
		} else {
			dev_err(&oct->pci_dev->dev, "DROQ: In drop: pkt with len 0\n");
			buf_cnt = 1;
		}

		INCR_INDEX(droq->read_idx, buf_cnt, droq->max_count);
		droq->refill_count += buf_cnt;
	}
}

static u32
octeon_droq_fast_process_packets(struct octeon_device *oct,
				 struct octeon_droq *droq,
				 u32 pkts_to_process)
{
	struct octeon_droq_info *info;
	union octeon_rh *rh;
	u32 pkt, total_len = 0, pkt_count;

	pkt_count = pkts_to_process;

	for (pkt = 0; pkt < pkt_count; pkt++) {
		u32 pkt_len = 0;
		struct sk_buff *nicbuf = NULL;

		info = &droq->info_list[droq->read_idx];
		octeon_swap_8B_data((u64 *)info, 2);

		if (!info->length) {
			dev_err(&oct->pci_dev->dev,
				"DROQ[%d] idx: %d len:0, pkt_cnt: %d\n",
				droq->q_no, droq->read_idx, pkt_count);
			print_hex_dump_bytes("", DUMP_PREFIX_ADDRESS,
					     (u8 *)info,
					     OCT_DROQ_INFO_SIZE);
			break;
		}

		/* Len of resp hdr in included in the received data len. */
		info->length -= OCT_RH_SIZE;
		rh = &info->rh;

		total_len += (u32)info->length;

		if (OPCODE_SLOW_PATH(rh)) {
			u32 buf_cnt;

			buf_cnt = octeon_droq_dispatch_pkt(oct, droq, rh, info);
			INCR_INDEX(droq->read_idx, buf_cnt, droq->max_count);
			droq->refill_count += buf_cnt;
		} else {
			if (info->length <= droq->buffer_size) {
				lio_unmap_ring(oct->pci_dev,
					       (u64)droq->desc_ring[
					       droq->read_idx].buffer_ptr,
					       droq->buffer_size);
				pkt_len = (u32)info->length;
				nicbuf = droq->recv_buf_list[
					droq->read_idx].buffer;
				droq->recv_buf_list[droq->read_idx].buffer =
					NULL;
				INCR_INDEX_BY1(droq->read_idx, droq->max_count);
				skb_put(nicbuf, pkt_len);
				droq->refill_count++;
			} else {
				nicbuf = octeon_fast_packet_alloc(oct, droq,
								  droq->q_no,
								  (u32)
								  info->length);
				pkt_len = 0;
				/* nicbuf allocation can fail. We'll handle it
				 * inside the loop.
				 */
				while (pkt_len < info->length) {
					int cpy_len;

					cpy_len = ((pkt_len +
						droq->buffer_size) >
						info->length) ?
						((u32)info->length - pkt_len) :
						droq->buffer_size;

					if (nicbuf) {
						lio_unmap_ring(oct->pci_dev,
							       (u64)
							       droq->desc_ring
							       [droq->read_idx].
							       buffer_ptr,
							       droq->
							       buffer_size);
						octeon_fast_packet_next(droq,
									nicbuf,
									cpy_len,
									droq->
									read_idx
									);
					}

					pkt_len += cpy_len;
					INCR_INDEX_BY1(droq->read_idx,
						       droq->max_count);
					droq->refill_count++;
				}
			}

			if (nicbuf) {
				if (droq->ops.fptr)
					droq->ops.fptr(oct->octeon_id,
					nicbuf, pkt_len,
					rh, &droq->napi);
				else
					recv_buffer_free(nicbuf);
			}
		}

		if (droq->refill_count >= droq->refill_threshold) {
			int desc_refilled = octeon_droq_refill(oct, droq);

			/* Flush the droq descriptor data to memory to be sure
			* that when we update the credits the data in memory
			* is accurate.
			*/
			wmb();
			writel((desc_refilled), droq->pkts_credit_reg);
			/* make sure mmio write completes */
			mmiowb();
		}

	}                       /* for ( each packet )... */

	/* Increment refill_count by the number of buffers processed. */
	droq->stats.pkts_received += pkt;
	droq->stats.bytes_received += total_len;

	if ((droq->ops.drop_on_max) && (pkts_to_process - pkt)) {
		octeon_droq_drop_packets(oct, droq, (pkts_to_process - pkt));

		droq->stats.dropped_toomany += (pkts_to_process - pkt);
		return pkts_to_process;
	}

	return pkt;
}

int
octeon_droq_process_packets(struct octeon_device *oct,
			    struct octeon_droq *droq,
			    u32 budget)
{
	u32 pkt_count = 0, pkts_processed = 0;
	struct list_head *tmp, *tmp2;

	pkt_count = atomic_read(&droq->pkts_pending);
	if (!pkt_count)
		return 0;

	if (pkt_count > budget)
		pkt_count = budget;

	/* Grab the lock */
	spin_lock(&droq->lock);

	pkts_processed = octeon_droq_fast_process_packets(oct, droq, pkt_count);

	atomic_sub(pkts_processed, &droq->pkts_pending);

	/* Release the spin lock */
	spin_unlock(&droq->lock);

	list_for_each_safe(tmp, tmp2, &droq->dispatch_list) {
		struct __dispatch *rdisp = (struct __dispatch *)tmp;

		list_del(tmp);
		rdisp->disp_fn(rdisp->rinfo,
			       octeon_get_dispatch_arg
			       (oct,
				(u16)rdisp->rinfo->recv_pkt->rh.r.opcode,
				(u16)rdisp->rinfo->recv_pkt->rh.r.subcode));
	}

	/* If there are packets pending. schedule tasklet again */
	if (atomic_read(&droq->pkts_pending))
		return 1;

	return 0;
}

/**
 * Utility function to poll for packets. check_hw_for_packets must be
 * called before calling this routine.
 */

static int
octeon_droq_process_poll_pkts(struct octeon_device *oct,
			      struct octeon_droq *droq, u32 budget)
{
	struct list_head *tmp, *tmp2;
	u32 pkts_available = 0, pkts_processed = 0;
	u32 total_pkts_processed = 0;

	if (budget > droq->max_count)
		budget = droq->max_count;

	spin_lock(&droq->lock);

	while (total_pkts_processed < budget) {
		pkts_available =
			CVM_MIN((budget - total_pkts_processed),
				(u32)(atomic_read(&droq->pkts_pending)));

		if (pkts_available == 0)
			break;

		pkts_processed =
			octeon_droq_fast_process_packets(oct, droq,
							 pkts_available);

		atomic_sub(pkts_processed, &droq->pkts_pending);

		total_pkts_processed += pkts_processed;

		octeon_droq_check_hw_for_pkts(oct, droq);
	}

	spin_unlock(&droq->lock);

	list_for_each_safe(tmp, tmp2, &droq->dispatch_list) {
		struct __dispatch *rdisp = (struct __dispatch *)tmp;

		list_del(tmp);
		rdisp->disp_fn(rdisp->rinfo,
			       octeon_get_dispatch_arg
			       (oct,
				(u16)rdisp->rinfo->recv_pkt->rh.r.opcode,
				(u16)rdisp->rinfo->recv_pkt->rh.r.subcode));
	}

	return total_pkts_processed;
}

int
octeon_process_droq_poll_cmd(struct octeon_device *oct, u32 q_no, int cmd,
			     u32 arg)
{
	struct octeon_droq *droq;
	struct octeon_config *oct_cfg = NULL;

	oct_cfg = octeon_get_conf(oct);

	if (!oct_cfg)
		return -EINVAL;

	if (q_no >= CFG_GET_OQ_MAX_Q(oct_cfg)) {
		dev_err(&oct->pci_dev->dev, "%s: droq id (%d) exceeds MAX (%d)\n",
			__func__, q_no, (oct->num_oqs - 1));
		return -EINVAL;
	}

	droq = oct->droq[q_no];

	if (cmd == POLL_EVENT_PROCESS_PKTS)
		return octeon_droq_process_poll_pkts(oct, droq, arg);

	if (cmd == POLL_EVENT_PENDING_PKTS) {
		u32 pkt_cnt = atomic_read(&droq->pkts_pending);

		return  octeon_droq_process_packets(oct, droq, pkt_cnt);
	}

	if (cmd == POLL_EVENT_ENABLE_INTR) {
		u32 value;
		unsigned long flags;

		/* Enable Pkt Interrupt */
		switch (oct->chip_id) {
		case OCTEON_CN66XX:
		case OCTEON_CN68XX: {
			struct octeon_cn6xxx *cn6xxx =
				(struct octeon_cn6xxx *)oct->chip;
			spin_lock_irqsave
				(&cn6xxx->lock_for_droq_int_enb_reg, flags);
			value =
				octeon_read_csr(oct,
						CN6XXX_SLI_PKT_TIME_INT_ENB);
			value |= (1 << q_no);
			octeon_write_csr(oct,
					 CN6XXX_SLI_PKT_TIME_INT_ENB,
					 value);
			value =
				octeon_read_csr(oct,
						CN6XXX_SLI_PKT_CNT_INT_ENB);
			value |= (1 << q_no);
			octeon_write_csr(oct,
					 CN6XXX_SLI_PKT_CNT_INT_ENB,
					 value);

			/* don't bother flushing the enables */

			spin_unlock_irqrestore
				(&cn6xxx->lock_for_droq_int_enb_reg, flags);
			return 0;
		}
		break;
		}

		return 0;
	}

	dev_err(&oct->pci_dev->dev, "%s Unknown command: %d\n", __func__, cmd);
	return -EINVAL;
}

int octeon_register_droq_ops(struct octeon_device *oct, u32 q_no,
			     struct octeon_droq_ops *ops)
{
	struct octeon_droq *droq;
	unsigned long flags;
	struct octeon_config *oct_cfg = NULL;

	oct_cfg = octeon_get_conf(oct);

	if (!oct_cfg)
		return -EINVAL;

	if (!(ops)) {
		dev_err(&oct->pci_dev->dev, "%s: droq_ops pointer is NULL\n",
			__func__);
		return -EINVAL;
	}

	if (q_no >= CFG_GET_OQ_MAX_Q(oct_cfg)) {
		dev_err(&oct->pci_dev->dev, "%s: droq id (%d) exceeds MAX (%d)\n",
			__func__, q_no, (oct->num_oqs - 1));
		return -EINVAL;
	}

	droq = oct->droq[q_no];

	spin_lock_irqsave(&droq->lock, flags);

	memcpy(&droq->ops, ops, sizeof(struct octeon_droq_ops));

	spin_unlock_irqrestore(&droq->lock, flags);

	return 0;
}

int octeon_unregister_droq_ops(struct octeon_device *oct, u32 q_no)
{
	unsigned long flags;
	struct octeon_droq *droq;
	struct octeon_config *oct_cfg = NULL;

	oct_cfg = octeon_get_conf(oct);

	if (!oct_cfg)
		return -EINVAL;

	if (q_no >= CFG_GET_OQ_MAX_Q(oct_cfg)) {
		dev_err(&oct->pci_dev->dev, "%s: droq id (%d) exceeds MAX (%d)\n",
			__func__, q_no, oct->num_oqs - 1);
		return -EINVAL;
	}

	droq = oct->droq[q_no];

	if (!droq) {
		dev_info(&oct->pci_dev->dev,
			 "Droq id (%d) not available.\n", q_no);
		return 0;
	}

	spin_lock_irqsave(&droq->lock, flags);

	droq->ops.fptr = NULL;
	droq->ops.drop_on_max = 0;

	spin_unlock_irqrestore(&droq->lock, flags);

	return 0;
}

int octeon_create_droq(struct octeon_device *oct,
		       u32 q_no, u32 num_descs,
		       u32 desc_size, void *app_ctx)
{
	struct octeon_droq *droq;

	if (oct->droq[q_no]) {
		dev_dbg(&oct->pci_dev->dev, "Droq already in use. Cannot create droq %d again\n",
			q_no);
		return 1;
	}

	/* Allocate the DS for the new droq. */
	droq = vmalloc(sizeof(*droq));
	if (!droq)
		goto create_droq_fail;
	memset(droq, 0, sizeof(struct octeon_droq));

	/*Disable the pkt o/p for this Q  */
	octeon_set_droq_pkt_op(oct, q_no, 0);
	oct->droq[q_no] = droq;

	/* Initialize the Droq */
	octeon_init_droq(oct, q_no, num_descs, desc_size, app_ctx);

	oct->num_oqs++;

	dev_dbg(&oct->pci_dev->dev, "%s: Total number of OQ: %d\n", __func__,
		oct->num_oqs);

	/* Global Droq register settings */

	/* As of now not required, as setting are done for all 32 Droqs at
	 * the same time.
	 */
	return 0;

create_droq_fail:
	octeon_delete_droq(oct, q_no);
	return -1;
}
