/*
 * Runtime PM support code
 *
 *  Copyright (C) 2009-2010 Magnus Damm
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/pm_clock.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/sh_clk.h>
#include <linux/bitmap.h>
#include <linux/slab.h>

#ifdef CONFIG_PM
static int sh_pm_runtime_suspend(struct device *dev)
{
	int ret;

	ret = pm_generic_runtime_suspend(dev);
	if (ret) {
		dev_err(dev, "failed to suspend device\n");
		return ret;
	}

	ret = pm_clk_suspend(dev);
	if (ret) {
		dev_err(dev, "failed to suspend clock\n");
		pm_generic_runtime_resume(dev);
		return ret;
	}

	return 0;
}

static int sh_pm_runtime_resume(struct device *dev)
{
	int ret;

	ret = pm_clk_resume(dev);
	if (ret) {
		dev_err(dev, "failed to resume clock\n");
		return ret;
	}

	return pm_generic_runtime_resume(dev);
}

static struct dev_pm_domain default_pm_domain = {
	.ops = {
		.runtime_suspend = sh_pm_runtime_suspend,
		.runtime_resume = sh_pm_runtime_resume,
		USE_PLATFORM_PM_SLEEP_OPS
	},
};

#define DEFAULT_PM_DOMAIN_PTR	(&default_pm_domain)

#else

#define DEFAULT_PM_DOMAIN_PTR	NULL

#endif /* CONFIG_PM */

static struct pm_clk_notifier_block platform_bus_notifier = {
	.pm_domain = DEFAULT_PM_DOMAIN_PTR,
	.con_ids = { NULL, },
};

static int __init sh_pm_runtime_init(void)
{
	if (IS_ENABLED(CONFIG_OF) && IS_ENABLED(CONFIG_ARCH_SHMOBILE)) {
		if (!of_find_compatible_node(NULL, NULL,
					     "renesas,cpg-mstp-clocks"))
			return 0;
		if (IS_ENABLED(CONFIG_PM_GENERIC_DOMAINS_OF) &&
		    of_find_node_with_property(NULL, "#power-domain-cells"))
			return 0;
	}

	pm_clk_add_notifier(&platform_bus_type, &platform_bus_notifier);
	return 0;
}
core_initcall(sh_pm_runtime_init);
