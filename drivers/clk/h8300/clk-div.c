/*
 * H8/300 divide clock driver
 *
 * Copyright 2015 Yoshinori Sato <ysato@users.sourceforge.jp>
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>

static DEFINE_SPINLOCK(clklock);

static void __init h8300_div_clk_setup(struct device_node *node)
{
	unsigned int num_parents;
	struct clk *clk;
	const char *clk_name = node->name;
	const char *parent_name;
	void __iomem *divcr = NULL;
	int width;

	num_parents = of_clk_get_parent_count(node);
	if (num_parents < 1) {
		pr_err("%s: no parent found", clk_name);
		return;
	}

	divcr = of_iomap(node, 0);
	if (divcr == NULL) {
		pr_err("%s: failed to map divide register", clk_name);
		goto error;
	}

	parent_name = of_clk_get_parent_name(node, 0);
	of_property_read_u32(node, "renesas,width", &width);
	clk = clk_register_divider(NULL, clk_name, parent_name,
				   CLK_SET_RATE_GATE, divcr, 0, width,
				   CLK_DIVIDER_POWER_OF_TWO, &clklock);
	if (!IS_ERR(clk)) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		return;
	}
	pr_err("%s: failed to register %s div clock (%ld)\n",
	       __func__, clk_name, PTR_ERR(clk));
error:
	if (divcr)
		iounmap(divcr);
}

CLK_OF_DECLARE(h8300_div_clk, "renesas,h8300-div-clock", h8300_div_clk_setup);
