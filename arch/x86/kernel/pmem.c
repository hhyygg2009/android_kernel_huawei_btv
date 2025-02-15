/*
 * Copyright (c) 2015, Christoph Hellwig.
 */
#include <linux/memblock.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <asm/e820.h>
#include <asm/page_types.h>
#include <asm/setup.h>

static __init void register_pmem_device(struct resource *res)
{
	struct platform_device *pdev;
	int error;

	pdev = platform_device_alloc("pmem", PLATFORM_DEVID_AUTO);
	if (!pdev)
		return;

	error = platform_device_add_resources(pdev, res, 1);
	if (error)
		goto out_put_pdev;

	error = platform_device_add(pdev);
	if (error)
		goto out_put_pdev;
	return;

out_put_pdev:
	dev_warn(&pdev->dev, "failed to add 'pmem' (persistent memory) device!\n");
	platform_device_put(pdev);
}

static __init int register_pmem_devices(void)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];

		memset(&ndr_desc, 0, sizeof(ndr_desc));
		ndr_desc.res = &res;
		ndr_desc.attr_groups = e820_pmem_region_attribute_groups;
		ndr_desc.numa_node = NUMA_NO_NODE;
		if (!nvdimm_pmem_region_create(nvdimm_bus, &ndr_desc))
			goto err;
	}

	return 0;
}
device_initcall(register_pmem_devices);
