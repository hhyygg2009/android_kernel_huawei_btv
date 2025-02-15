/*
 * Copyright (C) 2015, Samsung Electronics Co., Ltd.
 *
 * Author: Marek Szyprowski <m.szyprowski@samsung.com>
 *
 * License terms: GNU General Public License (GPL) version 2
 *
 * Simple eMMC hardware reset provider
 */
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/err.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0))
#include <linux/gpio/consumer.h>
#endif
#include <linux/reboot.h>

#include <linux/mmc/host.h>

#include "pwrseq.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0))
/**
 * Opaque descriptor for a GPIO. These are obtained using gpiod_get() and are
 * preferable to the old integer-based handles.
 *
 * Contrary to integers, a pointer to a gpio_desc is guaranteed to be valid
 * until the GPIO is released.
 */
struct gpio_desc;

#define GPIOD_FLAGS_BIT_DIR_SET		BIT(0)
#define GPIOD_FLAGS_BIT_DIR_OUT		BIT(1)
#define GPIOD_FLAGS_BIT_DIR_VAL		BIT(2)

/**
 * Optional flags that can be passed to one of gpiod_* to configure direction
 * and output value. These values cannot be OR'd.
 */
enum gpiod_flags {
	GPIOD_ASIS	= 0,
	GPIOD_IN	= GPIOD_FLAGS_BIT_DIR_SET,
	GPIOD_OUT_LOW	= GPIOD_FLAGS_BIT_DIR_SET | GPIOD_FLAGS_BIT_DIR_OUT,
	GPIOD_OUT_HIGH	= GPIOD_FLAGS_BIT_DIR_SET | GPIOD_FLAGS_BIT_DIR_OUT |
			  GPIOD_FLAGS_BIT_DIR_VAL,
};

/* Value get/set from non-sleeping context */
static void gpiod_set_value(struct gpio_desc *desc, int value)
{
	/* GPIO can never have been requested */
	WARN_ON(1);
}

/* Acquire and dispose GPIOs */
static struct gpio_desc *__must_check gpiod_get_index(struct device *dev,
					       const char *con_id,
					       unsigned int idx,
					       enum gpiod_flags flags)
{
	return ERR_PTR(-ENOSYS);
}

static void gpiod_put(struct gpio_desc *desc)
{
	might_sleep();

	/* GPIO can never have been requested */
	WARN_ON(1);
}

static int unregister_restart_handler(struct notifier_block *nb)
{
	return 0;
}

#endif

struct mmc_pwrseq_emmc {
	struct mmc_pwrseq pwrseq;
	struct notifier_block reset_nb;
	struct gpio_desc *reset_gpio;
};

static void __mmc_pwrseq_emmc_reset(struct mmc_pwrseq_emmc *pwrseq)
{
	gpiod_set_value(pwrseq->reset_gpio, 1);
	udelay(1);
	gpiod_set_value(pwrseq->reset_gpio, 0);
	udelay(200);
}

static void mmc_pwrseq_emmc_reset(struct mmc_host *host)
{
	struct mmc_pwrseq_emmc *pwrseq = container_of(host->pwrseq,
					struct mmc_pwrseq_emmc, pwrseq);

	__mmc_pwrseq_emmc_reset(pwrseq);
}

static void mmc_pwrseq_emmc_free(struct mmc_host *host)
{
	struct mmc_pwrseq_emmc *pwrseq = container_of(host->pwrseq,
					struct mmc_pwrseq_emmc, pwrseq);

	unregister_restart_handler(&pwrseq->reset_nb);
	gpiod_put(pwrseq->reset_gpio);
	kfree(pwrseq);
}

static struct mmc_pwrseq_ops mmc_pwrseq_emmc_ops = {
	.post_power_on = mmc_pwrseq_emmc_reset,
	.free = mmc_pwrseq_emmc_free,
};

static int mmc_pwrseq_emmc_reset_nb(struct notifier_block *this,
				    unsigned long mode, void *cmd)
{
	struct mmc_pwrseq_emmc *pwrseq = container_of(this,
					struct mmc_pwrseq_emmc, reset_nb);

	__mmc_pwrseq_emmc_reset(pwrseq);
	return NOTIFY_DONE;
}

struct mmc_pwrseq *mmc_pwrseq_emmc_alloc(struct mmc_host *host,
					 struct device *dev)
{
	struct mmc_pwrseq_emmc *pwrseq;
	int ret = 0;

	pwrseq = kzalloc(sizeof(struct mmc_pwrseq_emmc), GFP_KERNEL);
	if (!pwrseq)
		return ERR_PTR(-ENOMEM);

	pwrseq->reset_gpio = gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pwrseq->reset_gpio)) {
		ret = PTR_ERR(pwrseq->reset_gpio);
		goto free;
	}

	/*
	 * register reset handler to ensure emmc reset also from
	 * emergency_reboot(), priority 255 is the highest priority
	 * so it will be executed before any system reboot handler.
	 */
	pwrseq->reset_nb.notifier_call = mmc_pwrseq_emmc_reset_nb;
	pwrseq->reset_nb.priority = 255;
	register_restart_handler(&pwrseq->reset_nb);

	pwrseq->pwrseq.ops = &mmc_pwrseq_emmc_ops;

	return &pwrseq->pwrseq;
free:
	kfree(pwrseq);
	return ERR_PTR(ret);
}
