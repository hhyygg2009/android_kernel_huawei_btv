/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/libnvdimm.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/ndctl.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <nfit.h>
#include <nd.h>
#include "nfit_test.h"

/*
 * Generate an NFIT table to describe the following topology:
 *
 * BUS0: Interleaved PMEM regions, and aliasing with BLK regions
 *
 *                     (a)                       (b)            DIMM   BLK-REGION
 *           +----------+--------------+----------+---------+
 * +------+  |  blk2.0  |     pm0.0    |  blk2.1  |  pm1.0  |    0      region2
 * | imc0 +--+- - - - - region0 - - - -+----------+         +
 * +--+---+  |  blk3.0  |     pm0.0    |  blk3.1  |  pm1.0  |    1      region3
 *    |      +----------+--------------v----------v         v
 * +--+---+                            |                    |
 * | cpu0 |                                    region1
 * +--+---+                            |                    |
 *    |      +-------------------------^----------^         ^
 * +--+---+  |                 blk4.0             |  pm1.0  |    2      region4
 * | imc1 +--+-------------------------+----------+         +
 * +------+  |                 blk5.0             |  pm1.0  |    3      region5
 *           +-------------------------+----------+-+-------+
 *
 * *) In this layout we have four dimms and two memory controllers in one
 *    socket.  Each unique interface (BLK or PMEM) to DPA space
 *    is identified by a region device with a dynamically assigned id.
 *
 * *) The first portion of dimm0 and dimm1 are interleaved as REGION0.
 *    A single PMEM namespace "pm0.0" is created using half of the
 *    REGION0 SPA-range.  REGION0 spans dimm0 and dimm1.  PMEM namespace
 *    allocate from from the bottom of a region.  The unallocated
 *    portion of REGION0 aliases with REGION2 and REGION3.  That
 *    unallacted capacity is reclaimed as BLK namespaces ("blk2.0" and
 *    "blk3.0") starting at the base of each DIMM to offset (a) in those
 *    DIMMs.  "pm0.0", "blk2.0" and "blk3.0" are free-form readable
 *    names that can be assigned to a namespace.
 *
 * *) In the last portion of dimm0 and dimm1 we have an interleaved
 *    SPA range, REGION1, that spans those two dimms as well as dimm2
 *    and dimm3.  Some of REGION1 allocated to a PMEM namespace named
 *    "pm1.0" the rest is reclaimed in 4 BLK namespaces (for each
 *    dimm in the interleave set), "blk2.1", "blk3.1", "blk4.0", and
 *    "blk5.0".
 *
 * *) The portion of dimm2 and dimm3 that do not participate in the
 *    REGION1 interleaved SPA range (i.e. the DPA address below offset
 *    (b) are also included in the "blk4.0" and "blk5.0" namespaces.
 *    Note, that BLK namespaces need not be contiguous in DPA-space, and
 *    can consume aliased capacity from multiple interleave sets.
 *
 * BUS1: Legacy NVDIMM (single contiguous range)
 *
 *  region2
 * +---------------------+
 * |---------------------|
 * ||       pm2.0       ||
 * |---------------------|
 * +---------------------+
 *
 * *) A NFIT-table may describe a simple system-physical-address range
 *    with no BLK aliasing.  This type of region may optionally
 *    reference an NVDIMM.
 */
enum {
	NUM_PM  = 2,
	NUM_DCR = 4,
	NUM_BDW = NUM_DCR,
	NUM_SPA = NUM_PM + NUM_DCR + NUM_BDW,
	NUM_MEM = NUM_DCR + NUM_BDW + 2 /* spa0 iset */ + 4 /* spa1 iset */,
	DIMM_SIZE = SZ_32M,
	LABEL_SIZE = SZ_128K,
	SPA0_SIZE = DIMM_SIZE,
	SPA1_SIZE = DIMM_SIZE*2,
	SPA2_SIZE = DIMM_SIZE,
	BDW_SIZE = 64 << 8,
	DCR_SIZE = 12,
	NUM_NFITS = 2, /* permit testing multiple NFITs per system */
};

struct nfit_test_dcr {
	__le64 bdw_addr;
	__le32 bdw_status;
	__u8 aperature[BDW_SIZE];
};

#define NFIT_DIMM_HANDLE(node, socket, imc, chan, dimm) \
	(((node & 0xfff) << 16) | ((socket & 0xf) << 12) \
	 | ((imc & 0xf) << 8) | ((chan & 0xf) << 4) | (dimm & 0xf))

static u32 handle[NUM_DCR] = {
	[0] = NFIT_DIMM_HANDLE(0, 0, 0, 0, 0),
	[1] = NFIT_DIMM_HANDLE(0, 0, 0, 0, 1),
	[2] = NFIT_DIMM_HANDLE(0, 0, 1, 0, 0),
	[3] = NFIT_DIMM_HANDLE(0, 0, 1, 0, 1),
};

struct nfit_test {
	struct acpi_nfit_desc acpi_desc;
	struct platform_device pdev;
	struct list_head resources;
	void *nfit_buf;
	dma_addr_t nfit_dma;
	size_t nfit_size;
	int num_dcr;
	int num_pm;
	void **dimm;
	dma_addr_t *dimm_dma;
	void **flush;
	dma_addr_t *flush_dma;
	void **label;
	dma_addr_t *label_dma;
	void **spa_set;
	dma_addr_t *spa_set_dma;
	struct nfit_test_dcr **dcr;
	dma_addr_t *dcr_dma;
	int (*alloc)(struct nfit_test *t);
	void (*setup)(struct nfit_test *t);
};

static struct nfit_test *to_nfit_test(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

	return container_of(pdev, struct nfit_test, pdev);
}

static int nfit_test_ctl(struct nvdimm_bus_descriptor *nd_desc,
		struct nvdimm *nvdimm, unsigned int cmd, void *buf,
		unsigned int buf_len)
{
	struct acpi_nfit_desc *acpi_desc = to_acpi_desc(nd_desc);
	struct nfit_test *t = container_of(acpi_desc, typeof(*t), acpi_desc);
	struct nfit_mem *nfit_mem = nvdimm_provider_data(nvdimm);
	int i, rc;

	if (!nfit_mem || !test_bit(cmd, &nfit_mem->dsm_mask))
		return -ENOTTY;

	/* lookup label space for the given dimm */
	for (i = 0; i < ARRAY_SIZE(handle); i++)
		if (__to_nfit_memdev(nfit_mem)->device_handle == handle[i])
			break;
	if (i >= ARRAY_SIZE(handle))
		return -ENXIO;

	switch (cmd) {
	case ND_CMD_GET_CONFIG_SIZE: {
		struct nd_cmd_get_config_size *nd_cmd = buf;

		if (buf_len < sizeof(*nd_cmd))
			return -EINVAL;
		nd_cmd->status = 0;
		nd_cmd->config_size = LABEL_SIZE;
		nd_cmd->max_xfer = SZ_4K;
		rc = 0;
		break;
	}
	case ND_CMD_GET_CONFIG_DATA: {
		struct nd_cmd_get_config_data_hdr *nd_cmd = buf;
		unsigned int len, offset = nd_cmd->in_offset;

		if (buf_len < sizeof(*nd_cmd))
			return -EINVAL;
		if (offset >= LABEL_SIZE)
			return -EINVAL;
		if (nd_cmd->in_length + sizeof(*nd_cmd) > buf_len)
			return -EINVAL;

		nd_cmd->status = 0;
		len = min(nd_cmd->in_length, LABEL_SIZE - offset);
		memcpy(nd_cmd->out_buf, t->label[i] + offset, len);
		rc = buf_len - sizeof(*nd_cmd) - len;
		break;
	}
	case ND_CMD_SET_CONFIG_DATA: {
		struct nd_cmd_set_config_hdr *nd_cmd = buf;
		unsigned int len, offset = nd_cmd->in_offset;
		u32 *status;

		if (buf_len < sizeof(*nd_cmd))
			return -EINVAL;
		if (offset >= LABEL_SIZE)
			return -EINVAL;
		if (nd_cmd->in_length + sizeof(*nd_cmd) + 4 > buf_len)
			return -EINVAL;

		status = buf + nd_cmd->in_length + sizeof(*nd_cmd);
		*status = 0;
		len = min(nd_cmd->in_length, LABEL_SIZE - offset);
		memcpy(t->label[i] + offset, nd_cmd->in_buf, len);
		rc = buf_len - sizeof(*nd_cmd) - (len + 4);
		break;
	}
	default:
		return -ENOTTY;
	}

	return rc;
}

static DEFINE_SPINLOCK(nfit_test_lock);
static struct nfit_test *instances[NUM_NFITS];

static void release_nfit_res(void *data)
{
	struct nfit_test_resource *nfit_res = data;
	struct resource *res = nfit_res->res;

	spin_lock(&nfit_test_lock);
	list_del(&nfit_res->list);
	spin_unlock(&nfit_test_lock);

	if (is_vmalloc_addr(nfit_res->buf))
		vfree(nfit_res->buf);
	else
		dma_free_coherent(nfit_res->dev, resource_size(res),
				nfit_res->buf, res->start);
	kfree(res);
	kfree(nfit_res);
}

static void *__test_alloc(struct nfit_test *t, size_t size, dma_addr_t *dma,
		void *buf)
{
	struct device *dev = &t->pdev.dev;
	struct resource *res = kzalloc(sizeof(*res) * 2, GFP_KERNEL);
	struct nfit_test_resource *nfit_res = kzalloc(sizeof(*nfit_res),
			GFP_KERNEL);
	int rc;

	if (!res || !buf || !nfit_res)
		goto err;
	rc = devm_add_action(dev, release_nfit_res, nfit_res);
	if (rc)
		goto err;
	INIT_LIST_HEAD(&nfit_res->list);
	memset(buf, 0, size);
	nfit_res->dev = dev;
	nfit_res->buf = buf;
	nfit_res->res = res;
	res->start = *dma;
	res->end = *dma + size - 1;
	res->name = "NFIT";
	spin_lock(&nfit_test_lock);
	list_add(&nfit_res->list, &t->resources);
	spin_unlock(&nfit_test_lock);

	return nfit_res->buf;
 err:
	if (buf && !is_vmalloc_addr(buf))
		dma_free_coherent(dev, size, buf, *dma);
	else if (buf)
		vfree(buf);
	kfree(res);
	kfree(nfit_res);
	return NULL;
}

static void *test_alloc(struct nfit_test *t, size_t size, dma_addr_t *dma)
{
	void *buf = vmalloc(size);

	*dma = (unsigned long) buf;
	return __test_alloc(t, size, dma, buf);
}

static void *test_alloc_coherent(struct nfit_test *t, size_t size,
		dma_addr_t *dma)
{
	struct device *dev = &t->pdev.dev;
	void *buf = dma_alloc_coherent(dev, size, dma, GFP_KERNEL);

	return __test_alloc(t, size, dma, buf);
}

static struct nfit_test_resource *nfit_test_lookup(resource_size_t addr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(instances); i++) {
		struct nfit_test_resource *n, *nfit_res = NULL;
		struct nfit_test *t = instances[i];

		if (!t)
			continue;
		spin_lock(&nfit_test_lock);
		list_for_each_entry(n, &t->resources, list) {
			if (addr >= n->res->start && (addr < n->res->start
						+ resource_size(n->res))) {
				nfit_res = n;
				break;
			} else if (addr >= (unsigned long) n->buf
					&& (addr < (unsigned long) n->buf
						+ resource_size(n->res))) {
				nfit_res = n;
				break;
			}
		}
		spin_unlock(&nfit_test_lock);
		if (nfit_res)
			return nfit_res;
	}

	return NULL;
}

static int nfit_test0_alloc(struct nfit_test *t)
{
	size_t nfit_size = sizeof(struct acpi_table_nfit)
			+ sizeof(struct acpi_nfit_system_address) * NUM_SPA
			+ sizeof(struct acpi_nfit_memory_map) * NUM_MEM
			+ sizeof(struct acpi_nfit_control_region) * NUM_DCR
			+ sizeof(struct acpi_nfit_data_region) * NUM_BDW
			+ sizeof(struct acpi_nfit_flush_address) * NUM_DCR;
	int i;

	t->nfit_buf = test_alloc(t, nfit_size, &t->nfit_dma);
	if (!t->nfit_buf)
		return -ENOMEM;
	t->nfit_size = nfit_size;

	t->spa_set[0] = test_alloc_coherent(t, SPA0_SIZE, &t->spa_set_dma[0]);
	if (!t->spa_set[0])
		return -ENOMEM;

	t->spa_set[1] = test_alloc_coherent(t, SPA1_SIZE, &t->spa_set_dma[1]);
	if (!t->spa_set[1])
		return -ENOMEM;

	for (i = 0; i < NUM_DCR; i++) {
		t->dimm[i] = test_alloc(t, DIMM_SIZE, &t->dimm_dma[i]);
		if (!t->dimm[i])
			return -ENOMEM;

		t->label[i] = test_alloc(t, LABEL_SIZE, &t->label_dma[i]);
		if (!t->label[i])
			return -ENOMEM;
		sprintf(t->label[i], "label%d", i);

		t->flush[i] = test_alloc(t, 8, &t->flush_dma[i]);
		if (!t->flush[i])
			return -ENOMEM;
	}

	for (i = 0; i < NUM_DCR; i++) {
		t->dcr[i] = test_alloc(t, LABEL_SIZE, &t->dcr_dma[i]);
		if (!t->dcr[i])
			return -ENOMEM;
	}

	return 0;
}

static int nfit_test1_alloc(struct nfit_test *t)
{
	size_t nfit_size = sizeof(struct acpi_table_nfit)
		+ sizeof(struct acpi_nfit_system_address)
		+ sizeof(struct acpi_nfit_memory_map)
		+ sizeof(struct acpi_nfit_control_region);

	t->nfit_buf = test_alloc(t, nfit_size, &t->nfit_dma);
	if (!t->nfit_buf)
		return -ENOMEM;
	t->nfit_size = nfit_size;

	t->spa_set[0] = test_alloc_coherent(t, SPA2_SIZE, &t->spa_set_dma[0]);
	if (!t->spa_set[0])
		return -ENOMEM;

	return 0;
}

static void nfit_test_init_header(struct acpi_table_nfit *nfit, size_t size)
{
	memcpy(nfit->header.signature, ACPI_SIG_NFIT, 4);
	nfit->header.length = size;
	nfit->header.revision = 1;
	memcpy(nfit->header.oem_id, "LIBND", 6);
	memcpy(nfit->header.oem_table_id, "TEST", 5);
	nfit->header.oem_revision = 1;
	memcpy(nfit->header.asl_compiler_id, "TST", 4);
	nfit->header.asl_compiler_revision = 1;
}

static void nfit_test0_setup(struct nfit_test *t)
{
	struct nvdimm_bus_descriptor *nd_desc;
	struct acpi_nfit_desc *acpi_desc;
	struct acpi_nfit_memory_map *memdev;
	void *nfit_buf = t->nfit_buf;
	size_t size = t->nfit_size;
	struct acpi_nfit_system_address *spa;
	struct acpi_nfit_control_region *dcr;
	struct acpi_nfit_data_region *bdw;
	struct acpi_nfit_flush_address *flush;
	unsigned int offset;

	nfit_test_init_header(nfit_buf, size);

	/*
	 * spa0 (interleave first half of dimm0 and dimm1, note storage
	 * does not actually alias the related block-data-window
	 * regions)
	 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit);
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_PM), 16);
	spa->range_index = 0+1;
	spa->address = t->spa_set_dma[0];
	spa->length = SPA0_SIZE;

	/*
	 * spa1 (interleave last half of the 4 DIMMS, note storage
	 * does not actually alias the related block-data-window
	 * regions)
	 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa);
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_PM), 16);
	spa->range_index = 1+1;
	spa->address = t->spa_set_dma[1];
	spa->length = SPA1_SIZE;

	/* spa2 (dcr0) dimm0 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 2;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_DCR), 16);
	spa->range_index = 2+1;
	spa->address = t->dcr_dma[0];
	spa->length = DCR_SIZE;

	/* spa3 (dcr1) dimm1 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 3;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_DCR), 16);
	spa->range_index = 3+1;
	spa->address = t->dcr_dma[1];
	spa->length = DCR_SIZE;

	/* spa4 (dcr2) dimm2 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 4;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_DCR), 16);
	spa->range_index = 4+1;
	spa->address = t->dcr_dma[2];
	spa->length = DCR_SIZE;

	/* spa5 (dcr3) dimm3 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 5;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_DCR), 16);
	spa->range_index = 5+1;
	spa->address = t->dcr_dma[3];
	spa->length = DCR_SIZE;

	/* spa6 (bdw for dcr0) dimm0 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 6;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_BDW), 16);
	spa->range_index = 6+1;
	spa->address = t->dimm_dma[0];
	spa->length = DIMM_SIZE;

	/* spa7 (bdw for dcr1) dimm1 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 7;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_BDW), 16);
	spa->range_index = 7+1;
	spa->address = t->dimm_dma[1];
	spa->length = DIMM_SIZE;

	/* spa8 (bdw for dcr2) dimm2 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 8;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_BDW), 16);
	spa->range_index = 8+1;
	spa->address = t->dimm_dma[2];
	spa->length = DIMM_SIZE;

	/* spa9 (bdw for dcr3) dimm3 */
	spa = nfit_buf + sizeof(struct acpi_table_nfit) + sizeof(*spa) * 9;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_BDW), 16);
	spa->range_index = 9+1;
	spa->address = t->dimm_dma[3];
	spa->length = DIMM_SIZE;

	offset = sizeof(struct acpi_table_nfit) + sizeof(*spa) * 10;
	/* mem-region0 (spa0, dimm0) */
	memdev = nfit_buf + offset;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[0];
	memdev->physical_id = 0;
	memdev->region_id = 0;
	memdev->range_index = 0+1;
	memdev->region_index = 0+1;
	memdev->region_size = SPA0_SIZE/2;
	memdev->region_offset = t->spa_set_dma[0];
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 2;

	/* mem-region1 (spa0, dimm1) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map);
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[1];
	memdev->physical_id = 1;
	memdev->region_id = 0;
	memdev->range_index = 0+1;
	memdev->region_index = 1+1;
	memdev->region_size = SPA0_SIZE/2;
	memdev->region_offset = t->spa_set_dma[0] + SPA0_SIZE/2;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 2;

	/* mem-region2 (spa1, dimm0) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 2;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[0];
	memdev->physical_id = 0;
	memdev->region_id = 1;
	memdev->range_index = 1+1;
	memdev->region_index = 0+1;
	memdev->region_size = SPA1_SIZE/4;
	memdev->region_offset = t->spa_set_dma[1];
	memdev->address = SPA0_SIZE/2;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 4;

	/* mem-region3 (spa1, dimm1) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 3;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[1];
	memdev->physical_id = 1;
	memdev->region_id = 1;
	memdev->range_index = 1+1;
	memdev->region_index = 1+1;
	memdev->region_size = SPA1_SIZE/4;
	memdev->region_offset = t->spa_set_dma[1] + SPA1_SIZE/4;
	memdev->address = SPA0_SIZE/2;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 4;

	/* mem-region4 (spa1, dimm2) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 4;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[2];
	memdev->physical_id = 2;
	memdev->region_id = 0;
	memdev->range_index = 1+1;
	memdev->region_index = 2+1;
	memdev->region_size = SPA1_SIZE/4;
	memdev->region_offset = t->spa_set_dma[1] + 2*SPA1_SIZE/4;
	memdev->address = SPA0_SIZE/2;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 4;

	/* mem-region5 (spa1, dimm3) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 5;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[3];
	memdev->physical_id = 3;
	memdev->region_id = 0;
	memdev->range_index = 1+1;
	memdev->region_index = 3+1;
	memdev->region_size = SPA1_SIZE/4;
	memdev->region_offset = t->spa_set_dma[1] + 3*SPA1_SIZE/4;
	memdev->address = SPA0_SIZE/2;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 4;

	/* mem-region6 (spa/dcr0, dimm0) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 6;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[0];
	memdev->physical_id = 0;
	memdev->region_id = 0;
	memdev->range_index = 2+1;
	memdev->region_index = 0+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	/* mem-region7 (spa/dcr1, dimm1) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 7;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[1];
	memdev->physical_id = 1;
	memdev->region_id = 0;
	memdev->range_index = 3+1;
	memdev->region_index = 1+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	/* mem-region8 (spa/dcr2, dimm2) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 8;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[2];
	memdev->physical_id = 2;
	memdev->region_id = 0;
	memdev->range_index = 4+1;
	memdev->region_index = 2+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	/* mem-region9 (spa/dcr3, dimm3) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 9;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[3];
	memdev->physical_id = 3;
	memdev->region_id = 0;
	memdev->range_index = 5+1;
	memdev->region_index = 3+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	/* mem-region10 (spa/bdw0, dimm0) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 10;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[0];
	memdev->physical_id = 0;
	memdev->region_id = 0;
	memdev->range_index = 6+1;
	memdev->region_index = 0+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	/* mem-region11 (spa/bdw1, dimm1) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 11;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[1];
	memdev->physical_id = 1;
	memdev->region_id = 0;
	memdev->range_index = 7+1;
	memdev->region_index = 1+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	/* mem-region12 (spa/bdw2, dimm2) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 12;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[2];
	memdev->physical_id = 2;
	memdev->region_id = 0;
	memdev->range_index = 8+1;
	memdev->region_index = 2+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	/* mem-region13 (spa/dcr3, dimm3) */
	memdev = nfit_buf + offset + sizeof(struct acpi_nfit_memory_map) * 13;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = handle[3];
	memdev->physical_id = 3;
	memdev->region_id = 0;
	memdev->range_index = 9+1;
	memdev->region_index = 3+1;
	memdev->region_size = 0;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;

	offset = offset + sizeof(struct acpi_nfit_memory_map) * 14;
	/* dcr-descriptor0 */
	dcr = nfit_buf + offset;
	dcr->header.type = ACPI_NFIT_TYPE_CONTROL_REGION;
	dcr->header.length = sizeof(struct acpi_nfit_control_region);
	dcr->region_index = 0+1;
	dcr->vendor_id = 0xabcd;
	dcr->device_id = 0;
	dcr->revision_id = 1;
	dcr->serial_number = ~handle[0];
	dcr->windows = 1;
	dcr->window_size = DCR_SIZE;
	dcr->command_offset = 0;
	dcr->command_size = 8;
	dcr->status_offset = 8;
	dcr->status_size = 4;

	/* dcr-descriptor1 */
	dcr = nfit_buf + offset + sizeof(struct acpi_nfit_control_region);
	dcr->header.type = ACPI_NFIT_TYPE_CONTROL_REGION;
	dcr->header.length = sizeof(struct acpi_nfit_control_region);
	dcr->region_index = 1+1;
	dcr->vendor_id = 0xabcd;
	dcr->device_id = 0;
	dcr->revision_id = 1;
	dcr->serial_number = ~handle[1];
	dcr->windows = 1;
	dcr->window_size = DCR_SIZE;
	dcr->command_offset = 0;
	dcr->command_size = 8;
	dcr->status_offset = 8;
	dcr->status_size = 4;

	/* dcr-descriptor2 */
	dcr = nfit_buf + offset + sizeof(struct acpi_nfit_control_region) * 2;
	dcr->header.type = ACPI_NFIT_TYPE_CONTROL_REGION;
	dcr->header.length = sizeof(struct acpi_nfit_control_region);
	dcr->region_index = 2+1;
	dcr->vendor_id = 0xabcd;
	dcr->device_id = 0;
	dcr->revision_id = 1;
	dcr->serial_number = ~handle[2];
	dcr->windows = 1;
	dcr->window_size = DCR_SIZE;
	dcr->command_offset = 0;
	dcr->command_size = 8;
	dcr->status_offset = 8;
	dcr->status_size = 4;

	/* dcr-descriptor3 */
	dcr = nfit_buf + offset + sizeof(struct acpi_nfit_control_region) * 3;
	dcr->header.type = ACPI_NFIT_TYPE_CONTROL_REGION;
	dcr->header.length = sizeof(struct acpi_nfit_control_region);
	dcr->region_index = 3+1;
	dcr->vendor_id = 0xabcd;
	dcr->device_id = 0;
	dcr->revision_id = 1;
	dcr->serial_number = ~handle[3];
	dcr->windows = 1;
	dcr->window_size = DCR_SIZE;
	dcr->command_offset = 0;
	dcr->command_size = 8;
	dcr->status_offset = 8;
	dcr->status_size = 4;

	offset = offset + sizeof(struct acpi_nfit_control_region) * 4;
	/* bdw0 (spa/dcr0, dimm0) */
	bdw = nfit_buf + offset;
	bdw->header.type = ACPI_NFIT_TYPE_DATA_REGION;
	bdw->header.length = sizeof(struct acpi_nfit_data_region);
	bdw->region_index = 0+1;
	bdw->windows = 1;
	bdw->offset = 0;
	bdw->size = BDW_SIZE;
	bdw->capacity = DIMM_SIZE;
	bdw->start_address = 0;

	/* bdw1 (spa/dcr1, dimm1) */
	bdw = nfit_buf + offset + sizeof(struct acpi_nfit_data_region);
	bdw->header.type = ACPI_NFIT_TYPE_DATA_REGION;
	bdw->header.length = sizeof(struct acpi_nfit_data_region);
	bdw->region_index = 1+1;
	bdw->windows = 1;
	bdw->offset = 0;
	bdw->size = BDW_SIZE;
	bdw->capacity = DIMM_SIZE;
	bdw->start_address = 0;

	/* bdw2 (spa/dcr2, dimm2) */
	bdw = nfit_buf + offset + sizeof(struct acpi_nfit_data_region) * 2;
	bdw->header.type = ACPI_NFIT_TYPE_DATA_REGION;
	bdw->header.length = sizeof(struct acpi_nfit_data_region);
	bdw->region_index = 2+1;
	bdw->windows = 1;
	bdw->offset = 0;
	bdw->size = BDW_SIZE;
	bdw->capacity = DIMM_SIZE;
	bdw->start_address = 0;

	/* bdw3 (spa/dcr3, dimm3) */
	bdw = nfit_buf + offset + sizeof(struct acpi_nfit_data_region) * 3;
	bdw->header.type = ACPI_NFIT_TYPE_DATA_REGION;
	bdw->header.length = sizeof(struct acpi_nfit_data_region);
	bdw->region_index = 3+1;
	bdw->windows = 1;
	bdw->offset = 0;
	bdw->size = BDW_SIZE;
	bdw->capacity = DIMM_SIZE;
	bdw->start_address = 0;

	offset = offset + sizeof(struct acpi_nfit_data_region) * 4;
	/* flush0 (dimm0) */
	flush = nfit_buf + offset;
	flush->header.type = ACPI_NFIT_TYPE_FLUSH_ADDRESS;
	flush->header.length = sizeof(struct acpi_nfit_flush_address);
	flush->device_handle = handle[0];
	flush->hint_count = 1;
	flush->hint_address[0] = t->flush_dma[0];

	/* flush1 (dimm1) */
	flush = nfit_buf + offset + sizeof(struct acpi_nfit_flush_address) * 1;
	flush->header.type = ACPI_NFIT_TYPE_FLUSH_ADDRESS;
	flush->header.length = sizeof(struct acpi_nfit_flush_address);
	flush->device_handle = handle[1];
	flush->hint_count = 1;
	flush->hint_address[0] = t->flush_dma[1];

	/* flush2 (dimm2) */
	flush = nfit_buf + offset + sizeof(struct acpi_nfit_flush_address) * 2;
	flush->header.type = ACPI_NFIT_TYPE_FLUSH_ADDRESS;
	flush->header.length = sizeof(struct acpi_nfit_flush_address);
	flush->device_handle = handle[2];
	flush->hint_count = 1;
	flush->hint_address[0] = t->flush_dma[2];

	/* flush3 (dimm3) */
	flush = nfit_buf + offset + sizeof(struct acpi_nfit_flush_address) * 3;
	flush->header.type = ACPI_NFIT_TYPE_FLUSH_ADDRESS;
	flush->header.length = sizeof(struct acpi_nfit_flush_address);
	flush->device_handle = handle[3];
	flush->hint_count = 1;
	flush->hint_address[0] = t->flush_dma[3];

	acpi_desc = &t->acpi_desc;
	set_bit(ND_CMD_GET_CONFIG_SIZE, &acpi_desc->dimm_dsm_force_en);
	set_bit(ND_CMD_GET_CONFIG_DATA, &acpi_desc->dimm_dsm_force_en);
	set_bit(ND_CMD_SET_CONFIG_DATA, &acpi_desc->dimm_dsm_force_en);
	nd_desc = &acpi_desc->nd_desc;
	nd_desc->ndctl = nfit_test_ctl;
}

static void nfit_test1_setup(struct nfit_test *t)
{
	size_t size = t->nfit_size, offset;
	void *nfit_buf = t->nfit_buf;
	struct acpi_nfit_memory_map *memdev;
	struct acpi_nfit_control_region *dcr;
	struct acpi_nfit_system_address *spa;

	nfit_test_init_header(nfit_buf, size);

	offset = sizeof(struct acpi_table_nfit);
	/* spa0 (flat range with no bdw aliasing) */
	spa = nfit_buf + offset;
	spa->header.type = ACPI_NFIT_TYPE_SYSTEM_ADDRESS;
	spa->header.length = sizeof(*spa);
	memcpy(spa->range_guid, to_nfit_uuid(NFIT_SPA_PM), 16);
	spa->range_index = 0+1;
	spa->address = t->spa_set_dma[0];
	spa->length = SPA2_SIZE;

	offset += sizeof(*spa);
	/* mem-region0 (spa0, dimm0) */
	memdev = nfit_buf + offset;
	memdev->header.type = ACPI_NFIT_TYPE_MEMORY_MAP;
	memdev->header.length = sizeof(*memdev);
	memdev->device_handle = 0;
	memdev->physical_id = 0;
	memdev->region_id = 0;
	memdev->range_index = 0+1;
	memdev->region_index = 0+1;
	memdev->region_size = SPA2_SIZE;
	memdev->region_offset = 0;
	memdev->address = 0;
	memdev->interleave_index = 0;
	memdev->interleave_ways = 1;
	memdev->flags = ACPI_NFIT_MEM_SAVE_FAILED | ACPI_NFIT_MEM_RESTORE_FAILED
		| ACPI_NFIT_MEM_FLUSH_FAILED | ACPI_NFIT_MEM_HEALTH_OBSERVED
		| ACPI_NFIT_MEM_ARMED;

	offset += sizeof(*memdev);
	/* dcr-descriptor0 */
	dcr = nfit_buf + offset;
	dcr->header.type = ACPI_NFIT_TYPE_CONTROL_REGION;
	dcr->header.length = sizeof(struct acpi_nfit_control_region);
	dcr->region_index = 0+1;
	dcr->vendor_id = 0xabcd;
	dcr->device_id = 0;
	dcr->revision_id = 1;
	dcr->serial_number = ~0;
	dcr->code = 0x201;
	dcr->windows = 0;
	dcr->window_size = 0;
	dcr->command_offset = 0;
	dcr->command_size = 0;
	dcr->status_offset = 0;
	dcr->status_size = 0;
}

static int nfit_test_blk_do_io(struct nd_blk_region *ndbr, resource_size_t dpa,
		void *iobuf, u64 len, int rw)
{
	struct nfit_blk *nfit_blk = ndbr->blk_provider_data;
	struct nfit_blk_mmio *mmio = &nfit_blk->mmio[BDW];
	struct nd_region *nd_region = &ndbr->nd_region;
	unsigned int lane;

	lane = nd_region_acquire_lane(nd_region);
	if (rw)
		memcpy(mmio->base + dpa, iobuf, len);
	else
		memcpy(iobuf, mmio->base + dpa, len);
	nd_region_release_lane(nd_region, lane);

	return 0;
}

static int nfit_test_probe(struct platform_device *pdev)
{
	struct nvdimm_bus_descriptor *nd_desc;
	struct acpi_nfit_desc *acpi_desc;
	struct device *dev = &pdev->dev;
	struct nfit_test *nfit_test;
	int rc;

	nfit_test = to_nfit_test(&pdev->dev);

	/* common alloc */
	if (nfit_test->num_dcr) {
		int num = nfit_test->num_dcr;

		nfit_test->dimm = devm_kcalloc(dev, num, sizeof(void *),
				GFP_KERNEL);
		nfit_test->dimm_dma = devm_kcalloc(dev, num, sizeof(dma_addr_t),
				GFP_KERNEL);
		nfit_test->flush = devm_kcalloc(dev, num, sizeof(void *),
				GFP_KERNEL);
		nfit_test->flush_dma = devm_kcalloc(dev, num, sizeof(dma_addr_t),
				GFP_KERNEL);
		nfit_test->label = devm_kcalloc(dev, num, sizeof(void *),
				GFP_KERNEL);
		nfit_test->label_dma = devm_kcalloc(dev, num,
				sizeof(dma_addr_t), GFP_KERNEL);
		nfit_test->dcr = devm_kcalloc(dev, num,
				sizeof(struct nfit_test_dcr *), GFP_KERNEL);
		nfit_test->dcr_dma = devm_kcalloc(dev, num,
				sizeof(dma_addr_t), GFP_KERNEL);
		if (nfit_test->dimm && nfit_test->dimm_dma && nfit_test->label
				&& nfit_test->label_dma && nfit_test->dcr
				&& nfit_test->dcr_dma && nfit_test->flush
				&& nfit_test->flush_dma)
			/* pass */;
		else
			return -ENOMEM;
	}

	if (nfit_test->num_pm) {
		int num = nfit_test->num_pm;

		nfit_test->spa_set = devm_kcalloc(dev, num, sizeof(void *),
				GFP_KERNEL);
		nfit_test->spa_set_dma = devm_kcalloc(dev, num,
				sizeof(dma_addr_t), GFP_KERNEL);
		if (nfit_test->spa_set && nfit_test->spa_set_dma)
			/* pass */;
		else
			return -ENOMEM;
	}

	/* per-nfit specific alloc */
	if (nfit_test->alloc(nfit_test))
		return -ENOMEM;

	nfit_test->setup(nfit_test);
	acpi_desc = &nfit_test->acpi_desc;
	acpi_desc->dev = &pdev->dev;
	acpi_desc->nfit = nfit_test->nfit_buf;
	acpi_desc->blk_do_io = nfit_test_blk_do_io;
	nd_desc = &acpi_desc->nd_desc;
	nd_desc->attr_groups = acpi_nfit_attribute_groups;
	acpi_desc->nvdimm_bus = nvdimm_bus_register(&pdev->dev, nd_desc);
	if (!acpi_desc->nvdimm_bus)
		return -ENXIO;

	INIT_LIST_HEAD(&acpi_desc->spa_maps);
	INIT_LIST_HEAD(&acpi_desc->spas);
	INIT_LIST_HEAD(&acpi_desc->dcrs);
	INIT_LIST_HEAD(&acpi_desc->bdws);
	INIT_LIST_HEAD(&acpi_desc->idts);
	INIT_LIST_HEAD(&acpi_desc->flushes);
	INIT_LIST_HEAD(&acpi_desc->memdevs);
	INIT_LIST_HEAD(&acpi_desc->dimms);
	mutex_init(&acpi_desc->spa_map_mutex);
	mutex_init(&acpi_desc->init_mutex);

	rc = acpi_nfit_init(acpi_desc, nfit_test->nfit_size);
	if (rc) {
		nvdimm_bus_unregister(acpi_desc->nvdimm_bus);
		return rc;
	}

	if (nfit_test->setup != nfit_test0_setup)
		return 0;

	flush_work(&acpi_desc->work);
	nfit_test->setup_hotplug = 1;
	nfit_test->setup(nfit_test);

	rc = acpi_nfit_init(acpi_desc, nfit_test->nfit_size);
	if (rc) {
		nvdimm_bus_unregister(acpi_desc->nvdimm_bus);
		return rc;
	}

	return 0;
}

static int nfit_test_remove(struct platform_device *pdev)
{
	struct nfit_test *nfit_test = to_nfit_test(&pdev->dev);
	struct acpi_nfit_desc *acpi_desc = &nfit_test->acpi_desc;

	nvdimm_bus_unregister(acpi_desc->nvdimm_bus);

	return 0;
}

static void nfit_test_release(struct device *dev)
{
	struct nfit_test *nfit_test = to_nfit_test(dev);

	kfree(nfit_test);
}

static const struct platform_device_id nfit_test_id[] = {
	{ KBUILD_MODNAME },
	{ },
};

static struct platform_driver nfit_test_driver = {
	.probe = nfit_test_probe,
	.remove = nfit_test_remove,
	.driver = {
		.name = KBUILD_MODNAME,
	},
	.id_table = nfit_test_id,
};

#ifdef CONFIG_CMA_SIZE_MBYTES
#define CMA_SIZE_MBYTES CONFIG_CMA_SIZE_MBYTES
#else
#define CMA_SIZE_MBYTES 0
#endif

static __init int nfit_test_init(void)
{
	int rc, i;

	nfit_test_setup(nfit_test_lookup);

	for (i = 0; i < NUM_NFITS; i++) {
		struct nfit_test *nfit_test;
		struct platform_device *pdev;
		static int once;

		nfit_test = kzalloc(sizeof(*nfit_test), GFP_KERNEL);
		if (!nfit_test) {
			rc = -ENOMEM;
			goto err_register;
		}
		INIT_LIST_HEAD(&nfit_test->resources);
		switch (i) {
		case 0:
			nfit_test->num_pm = NUM_PM;
			nfit_test->num_dcr = NUM_DCR;
			nfit_test->alloc = nfit_test0_alloc;
			nfit_test->setup = nfit_test0_setup;
			break;
		case 1:
			nfit_test->num_pm = 1;
			nfit_test->alloc = nfit_test1_alloc;
			nfit_test->setup = nfit_test1_setup;
			break;
		default:
			rc = -EINVAL;
			goto err_register;
		}
		pdev = &nfit_test->pdev;
		pdev->name = KBUILD_MODNAME;
		pdev->id = i;
		pdev->dev.release = nfit_test_release;
		rc = platform_device_register(pdev);
		if (rc) {
			put_device(&pdev->dev);
			goto err_register;
		}

		rc = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
		if (rc)
			goto err_register;

		instances[i] = nfit_test;

		if (!once++) {
			dma_addr_t dma;
			void *buf;

			buf = dma_alloc_coherent(&pdev->dev, SZ_128M, &dma,
					GFP_KERNEL);
			if (!buf) {
				rc = -ENOMEM;
				dev_warn(&pdev->dev, "need 128M of free cma\n");
				goto err_register;
			}
			dma_free_coherent(&pdev->dev, SZ_128M, buf, dma);
		}
	}

	rc = platform_driver_register(&nfit_test_driver);
	if (rc)
		goto err_register;
	return 0;

 err_register:
	for (i = 0; i < NUM_NFITS; i++)
		if (instances[i])
			platform_device_unregister(&instances[i]->pdev);
	nfit_test_teardown();
	return rc;
}

static __exit void nfit_test_exit(void)
{
	int i;

	platform_driver_unregister(&nfit_test_driver);
	for (i = 0; i < NUM_NFITS; i++)
		platform_device_unregister(&instances[i]->pdev);
	nfit_test_teardown();
}

module_init(nfit_test_init);
module_exit(nfit_test_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel Corporation");
