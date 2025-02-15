/*
 *  Driver for AMBA serial ports
 *
 *  Based on drivers/char/serial.c, by Linus Torvalds, Theodore Ts'o.
 *
 *  Copyright 1999 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd.
 *  Copyright (C) 2010 ST-Ericsson SA
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * This is a generic driver for ARM AMBA-type serial ports.  They
 * have a lot of 16550-like features, but are not register compatible.
 * Note that although they do have CTS, DCD and DSR inputs, they do
 * not have an RI input, nor do they have DTR or RTS outputs.  If
 * required, these have to be supplied via some other means (eg, GPIO)
 * and hooked into this driver.
 */


#if defined(CONFIG_SERIAL_AMBA_PL011_CONSOLE) && defined(CONFIG_MAGIC_SYSRQ)
#define SUPPORT_SYSRQ
#endif

#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/sysrq.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/amba/bus.h>
#include <linux/amba/serial.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/sizes.h>
#include <linux/io.h>
#ifdef CONFIG_HISI_AMBA_PL011
#include "hisi-amba-pl011.h"
#endif
#include <linux/acpi.h>

#ifdef CONFIG_HISI_AMBA_PL011
#else
#define UART_NR			14
#endif
#define SERIAL_AMBA_MAJOR	204
#define SERIAL_AMBA_MINOR	64
#define SERIAL_AMBA_NR		UART_NR

#define AMBA_ISR_PASS_LIMIT	256

#define UART_DR_ERROR		(UART011_DR_OE|UART011_DR_BE|UART011_DR_PE|UART011_DR_FE)
#define UART_DUMMY_DR_RX	(1 << 16)

#ifdef CONFIG_HISI_AMBA_PL011
#else
/* There is by now at least one vendor with differing details, so handle it */
struct vendor_data {
	unsigned int		ifls;
	unsigned int		fr_busy;
	unsigned int		fr_dsr;
	unsigned int		fr_cts;
	unsigned int		fr_ri;
	unsigned int		lcrh_tx;
	unsigned int		lcrh_rx;
	u16			*reg_lut;
	bool			oversampling;
	bool			dma_threshold;
	bool			cts_event_workaround;
	bool			always_enabled;
	bool			fixed_options;

	unsigned int (*get_fifosize)(struct amba_device *dev);
};
#endif

/* Max address offset of register in use is 0x48 */
#define REG_NR		(0x48 >> 2)
#define IDX(x)		(x >> 2)
enum reg_idx {
	REG_DR		= IDX(UART01x_DR),
	REG_RSR		= IDX(UART01x_RSR),
	REG_ST_DMAWM	= IDX(ST_UART011_DMAWM),
	REG_FR		= IDX(UART01x_FR),
	REG_ST_LCRH_RX  = IDX(ST_UART011_LCRH_RX),
	REG_ILPR	= IDX(UART01x_ILPR),
	REG_IBRD	= IDX(UART011_IBRD),
	REG_FBRD	= IDX(UART011_FBRD),
	REG_LCRH	= IDX(UART011_LCRH),
	REG_CR		= IDX(UART011_CR),
	REG_IFLS	= IDX(UART011_IFLS),
	REG_IMSC	= IDX(UART011_IMSC),
	REG_RIS		= IDX(UART011_RIS),
	REG_MIS		= IDX(UART011_MIS),
	REG_ICR		= IDX(UART011_ICR),
	REG_DMACR	= IDX(UART011_DMACR),
};

static u16 arm_reg[] = {
	[REG_DR]		= UART01x_DR,
	[REG_RSR]		= UART01x_RSR,
	[REG_ST_DMAWM]		= ~0,
	[REG_FR]		= UART01x_FR,
	[REG_ST_LCRH_RX]	= ~0,
	[REG_ILPR]		= UART01x_ILPR,
	[REG_IBRD]		= UART011_IBRD,
	[REG_FBRD]		= UART011_FBRD,
	[REG_LCRH]		= UART011_LCRH,
	[REG_CR]		= UART011_CR,
	[REG_IFLS]		= UART011_IFLS,
	[REG_IMSC]		= UART011_IMSC,
	[REG_RIS]		= UART011_RIS,
	[REG_MIS]		= UART011_MIS,
	[REG_ICR]		= UART011_ICR,
	[REG_DMACR]		= UART011_DMACR,
};

#ifdef CONFIG_ARM_AMBA
static unsigned int get_fifosize_arm(struct amba_device *dev)
{
#ifdef CONFIG_HISI_AMBA_PL011
	return 64;
#else
	return amba_rev(dev) < 3 ? 16 : 32;
#endif
}

static struct vendor_data vendor_arm = {
#ifdef CONFIG_HISI_AMBA_PL011
	.ifls			= UART011_IFLS_RX2_8|UART011_IFLS_TX4_8 | (4<<6),
#else
	.ifls			= UART011_IFLS_RX4_8|UART011_IFLS_TX4_8,
#endif
	.fr_busy		= UART01x_FR_BUSY,
	.fr_dsr			= UART01x_FR_DSR,
	.fr_cts			= UART01x_FR_CTS,
	.fr_ri			= UART011_FR_RI,
	.lcrh_tx		= REG_LCRH,
	.lcrh_rx		= REG_LCRH,
	.reg_lut		= arm_reg,
	.oversampling		= false,
	.dma_threshold		= false,
	.cts_event_workaround	= false,
	.always_enabled		= false,
	.fixed_options		= false,
	.get_fifosize		= get_fifosize_arm,
};
#endif

static struct vendor_data vendor_sbsa = {
	.fr_busy		= UART01x_FR_BUSY,
	.fr_dsr			= UART01x_FR_DSR,
	.fr_cts			= UART01x_FR_CTS,
	.fr_ri			= UART011_FR_RI,
	.reg_lut		= arm_reg,
	.oversampling		= false,
	.dma_threshold		= false,
	.cts_event_workaround	= false,
	.always_enabled		= true,
	.fixed_options		= true,
};

#ifdef CONFIG_ARM_AMBA
static u16 st_reg[] = {
	[REG_DR]		= UART01x_DR,
	[REG_RSR]		= UART01x_RSR,
	[REG_ST_DMAWM]		= ST_UART011_DMAWM,
	[REG_FR]		= UART01x_FR,
	[REG_ST_LCRH_RX]	= ST_UART011_LCRH_RX,
	[REG_ILPR]		= UART01x_ILPR,
	[REG_IBRD]		= UART011_IBRD,
	[REG_FBRD]		= UART011_FBRD,
	[REG_LCRH]		= UART011_LCRH,
	[REG_CR]		= UART011_CR,
	[REG_IFLS]		= UART011_IFLS,
	[REG_IMSC]		= UART011_IMSC,
	[REG_RIS]		= UART011_RIS,
	[REG_MIS]		= UART011_MIS,
	[REG_ICR]		= UART011_ICR,
	[REG_DMACR]		= UART011_DMACR,
};

static unsigned int get_fifosize_st(struct amba_device *dev)
{
	return 64;
}

static struct vendor_data vendor_st = {
	.ifls			= UART011_IFLS_RX_HALF|UART011_IFLS_TX_HALF,
	.fr_busy		= UART01x_FR_BUSY,
	.fr_dsr			= UART01x_FR_DSR,
	.fr_cts			= UART01x_FR_CTS,
	.fr_ri			= UART011_FR_RI,
	.lcrh_tx		= REG_LCRH,
	.lcrh_rx		= REG_ST_LCRH_RX,
	.reg_lut		= st_reg,
	.oversampling		= true,
	.dma_threshold		= true,
	.cts_event_workaround	= true,
	.always_enabled		= false,
	.fixed_options		= false,
	.get_fifosize		= get_fifosize_st,
};
#endif

#ifdef CONFIG_SOC_ZX296702
static u16 zte_reg[] = {
	[REG_DR]		= ZX_UART01x_DR,
	[REG_RSR]		= UART01x_RSR,
	[REG_ST_DMAWM]		= ST_UART011_DMAWM,
	[REG_FR]		= ZX_UART01x_FR,
	[REG_ST_LCRH_RX]	= ST_UART011_LCRH_RX,
	[REG_ILPR]		= UART01x_ILPR,
	[REG_IBRD]		= UART011_IBRD,
	[REG_FBRD]		= UART011_FBRD,
	[REG_LCRH]		= ZX_UART011_LCRH_TX,
	[REG_CR]		= ZX_UART011_CR,
	[REG_IFLS]		= ZX_UART011_IFLS,
	[REG_IMSC]		= ZX_UART011_IMSC,
	[REG_RIS]		= ZX_UART011_RIS,
	[REG_MIS]		= ZX_UART011_MIS,
	[REG_ICR]		= ZX_UART011_ICR,
	[REG_DMACR]		= ZX_UART011_DMACR,
};

static struct vendor_data vendor_zte = {
	.ifls			= UART011_IFLS_RX4_8|UART011_IFLS_TX4_8,
	.fr_busy		= ZX_UART01x_FR_BUSY,
	.fr_dsr			= ZX_UART01x_FR_DSR,
	.fr_cts			= ZX_UART01x_FR_CTS,
	.fr_ri			= ZX_UART011_FR_RI,
	.lcrh_tx		= REG_LCRH,
	.lcrh_rx		= REG_ST_LCRH_RX,
	.reg_lut		= zte_reg,
	.oversampling		= false,
	.dma_threshold		= false,
	.cts_event_workaround	= false,
	.fixed_options		= false,
};
#endif

/* Deals with DMA transactions */
#ifdef CONFIG_HISI_AMBA_PL011
#else

struct pl011_sgbuf {
	struct scatterlist sg;
	char *buf;
};

struct pl011_dmarx_data {
	struct dma_chan		*chan;
	struct completion	complete;
	bool			use_buf_b;
	struct pl011_sgbuf	sgbuf_a;
	struct pl011_sgbuf	sgbuf_b;
	dma_cookie_t		cookie;
	bool			running;
	struct timer_list	timer;
	unsigned int last_residue;
	unsigned long last_jiffies;
	bool auto_poll_rate;
	unsigned int poll_rate;
	unsigned int poll_timeout;
};

struct pl011_dmatx_data {
	struct dma_chan		*chan;
	struct scatterlist	sg;
	char			*buf;
	bool			queued;
};

/*
 * We wrap our port structure around the generic uart_port.
 */
struct uart_amba_port {
	struct uart_port	port;
	struct clk		*clk;
	const struct vendor_data *vendor;
	u16			*reg_lut;
	unsigned int		dmacr;		/* dma control reg */
	unsigned int		im;		/* interrupt mask */
	unsigned int		old_status;
	unsigned int		fifosize;	/* vendor-specific */
	unsigned int		fr_busy;        /* vendor-specific */
	unsigned int		fr_dsr;		/* vendor-specific */
	unsigned int		fr_cts;         /* vendor-specific */
	unsigned int		fr_ri;		/* vendor-specific */
	unsigned int		lcrh_tx;	/* vendor-specific */
	unsigned int		lcrh_rx;	/* vendor-specific */
	unsigned int		old_cr;		/* state during shutdown */
	bool			autorts;
	unsigned int		fixed_baud;	/* vendor-set fixed baud rate */
	char			type[12];
#ifdef CONFIG_DMA_ENGINE
	/* DMA stuff */
	bool			using_tx_dma;
	bool			using_rx_dma;
	struct pl011_dmarx_data dmarx;
	struct pl011_dmatx_data	dmatx;
	bool			dma_probed;
#endif
};
#endif

static bool is_implemented(struct uart_amba_port *uap, unsigned int reg)
{
	return uap->reg_lut[reg] != (u16)~0;
}

static unsigned int pl011_readw(struct uart_amba_port *uap, int index)
{
	WARN_ON(index > REG_NR);
	return readw_relaxed(uap->port.membase + uap->reg_lut[index]);
}

static void pl011_writew(struct uart_amba_port *uap, int val, int index)
{
	WARN_ON(index > REG_NR);
	writew_relaxed(val, uap->port.membase + uap->reg_lut[index]);
}

static void pl011_writeb(struct uart_amba_port *uap, u8 val, int index)
{
	WARN_ON(index > REG_NR);
	writeb_relaxed(val, uap->port.membase + uap->reg_lut[index]);
}

/*
 * Reads up to 256 characters from the FIFO or until it's empty and
 * inserts them into the TTY layer. Returns the number of characters
 * read from the FIFO.
 */
static int pl011_fifo_to_tty(struct uart_amba_port *uap)
{
	u16 status;
	unsigned int ch, flag, max_count = 256;
	int fifotaken = 0;

	while (max_count--) {
		status = pl011_readw(uap, REG_FR);
		if (status & UART01x_FR_RXFE)
			break;

		/* Take chars from the FIFO and update status */
		ch = pl011_readw(uap, REG_DR) |
			UART_DUMMY_DR_RX;
		flag = TTY_NORMAL;
		uap->port.icount.rx++;
		fifotaken++;

		if (unlikely(ch & UART_DR_ERROR)) {
			if (ch & UART011_DR_BE) {
				ch &= ~(UART011_DR_FE | UART011_DR_PE);
				uap->port.icount.brk++;
				if (uart_handle_break(&uap->port))
					continue;
			} else if (ch & UART011_DR_PE)
				uap->port.icount.parity++;
			else if (ch & UART011_DR_FE)
				uap->port.icount.frame++;
			if (ch & UART011_DR_OE)
				uap->port.icount.overrun++;

			ch &= uap->port.read_status_mask;

			if (ch & UART011_DR_BE)
				flag = TTY_BREAK;
			else if (ch & UART011_DR_PE)
				flag = TTY_PARITY;
			else if (ch & UART011_DR_FE)
				flag = TTY_FRAME;
		}

		if (uart_handle_sysrq_char(&uap->port, ch & 255))
			continue;

		uart_insert_char(&uap->port, ch, UART011_DR_OE, ch, flag);
	}

	return fifotaken;
}


/*
 * All the DMA operation mode stuff goes inside this ifdef.
 * This assumes that you have a generic DMA device interface,
 * no custom DMA interfaces are supported.
 */
#ifdef CONFIG_DMA_ENGINE

#define PL011_DMA_BUFFER_SIZE PAGE_SIZE

static int pl011_sgbuf_init(struct dma_chan *chan, struct pl011_sgbuf *sg,
	enum dma_data_direction dir)
{
	dma_addr_t dma_addr;

	sg->buf = dma_alloc_coherent(chan->device->dev,
		PL011_DMA_BUFFER_SIZE, &dma_addr, GFP_KERNEL);
	if (!sg->buf)
		return -ENOMEM;

	sg_init_table(&sg->sg, 1);
	sg_set_page(&sg->sg, phys_to_page(dma_addr),
		PL011_DMA_BUFFER_SIZE, offset_in_page(dma_addr));
	sg_dma_address(&sg->sg) = dma_addr;
	sg_dma_len(&sg->sg) = PL011_DMA_BUFFER_SIZE;

	return 0;
}

static void pl011_sgbuf_free(struct dma_chan *chan, struct pl011_sgbuf *sg,
	enum dma_data_direction dir)
{
	if (sg->buf) {
		dma_free_coherent(chan->device->dev,
			PL011_DMA_BUFFER_SIZE, sg->buf,
			sg_dma_address(&sg->sg));
	}
}

static void pl011_dma_probe(struct uart_amba_port *uap)
{
	/* DMA is the sole user of the platform data right now */
	struct amba_pl011_data *plat = dev_get_platdata(uap->port.dev);
	struct device *dev = uap->port.dev;
	struct dma_slave_config tx_conf = {
		.dst_addr = uap->port.mapbase + uap->reg_lut[REG_DR],
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
		.direction = DMA_MEM_TO_DEV,
#ifdef CONFIG_HISI_AMBA_PL011
		.dst_maxburst = ((uap->fifosize >> 2) - 1),
#else
		.dst_maxburst = uap->fifosize >> 1,
#endif
		.device_fc = false,
	};
	struct dma_chan *chan;
	dma_cap_mask_t mask;

	uap->dma_probed = true;
	chan = dma_request_slave_channel_reason(dev, "tx");
	if (IS_ERR(chan)) {
		if (PTR_ERR(chan) == -EPROBE_DEFER) {
			uap->dma_probed = false;
			return;
		}

		/* We need platform data */
		if (!plat || !plat->dma_filter) {
			dev_info(uap->port.dev, "no DMA platform data\n");
			return;
		}

		/* Try to acquire a generic DMA engine slave TX channel */
		dma_cap_zero(mask);
		dma_cap_set(DMA_SLAVE, mask);

		chan = dma_request_channel(mask, plat->dma_filter,
						plat->dma_tx_param);
		if (!chan) {
			dev_err(uap->port.dev, "no TX DMA channel!\n");
			return;
		}
	}

	dmaengine_slave_config(chan, &tx_conf);
	uap->dmatx.chan = chan;

	dev_info(uap->port.dev, "DMA channel TX %s\n",
		 dma_chan_name(uap->dmatx.chan));

	/* Optionally make use of an RX channel as well */
	chan = dma_request_slave_channel(dev, "rx");

	if (!chan && plat->dma_rx_param) {
		chan = dma_request_channel(mask, plat->dma_filter, plat->dma_rx_param);

		if (!chan) {
			dev_err(uap->port.dev, "no RX DMA channel!\n");
			return;
		}
	}

	if (chan) {
		struct dma_slave_config rx_conf = {
			.src_addr = uap->port.mapbase + uap->reg_lut[REG_DR],
			.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
			.direction = DMA_DEV_TO_MEM,
#ifdef CONFIG_HISI_AMBA_PL011
			.src_maxburst = ((uap->fifosize >> 2) - 1),
#else
			.src_maxburst = uap->fifosize >> 2,
#endif
			.device_fc = false,
		};
		struct dma_slave_caps caps;

		/*
		 * Some DMA controllers provide information on their capabilities.
		 * If the controller does, check for suitable residue processing
		 * otherwise assime all is well.
		 */
		if (0 == dma_get_slave_caps(chan, &caps)) {
			if (caps.residue_granularity ==
					DMA_RESIDUE_GRANULARITY_DESCRIPTOR) {
				dma_release_channel(chan);
				dev_info(uap->port.dev,
					"RX DMA disabled - no residue processing\n");
				return;
			}
		}
		dmaengine_slave_config(chan, &rx_conf);
		uap->dmarx.chan = chan;

		uap->dmarx.auto_poll_rate = false;
		if (plat && plat->dma_rx_poll_enable) {
			/* Set poll rate if specified. */
			if (plat->dma_rx_poll_rate) {
				uap->dmarx.auto_poll_rate = false;
				uap->dmarx.poll_rate = plat->dma_rx_poll_rate;
			} else {
				/*
				 * 100 ms defaults to poll rate if not
				 * specified. This will be adjusted with
				 * the baud rate at set_termios.
				 */
				uap->dmarx.auto_poll_rate = true;
				uap->dmarx.poll_rate =  100;
			}
			/* 3 secs defaults poll_timeout if not specified. */
			if (plat->dma_rx_poll_timeout)
				uap->dmarx.poll_timeout =
					plat->dma_rx_poll_timeout;
			else
				uap->dmarx.poll_timeout = 3000;
		} else if (!plat && dev->of_node) {
			uap->dmarx.auto_poll_rate = of_property_read_bool(
						dev->of_node, "auto-poll");
			if (uap->dmarx.auto_poll_rate) {
				u32 x;

				if (0 == of_property_read_u32(dev->of_node,
						"poll-rate-ms", &x))
					uap->dmarx.poll_rate = x;
				else
					uap->dmarx.poll_rate = 100;
				if (0 == of_property_read_u32(dev->of_node,
						"poll-timeout-ms", &x))
					uap->dmarx.poll_timeout = x;
				else
					uap->dmarx.poll_timeout = 3000;
			}
		}
		dev_info(uap->port.dev, "DMA channel RX %s\n",
			 dma_chan_name(uap->dmarx.chan));
	}
}

static void pl011_dma_remove(struct uart_amba_port *uap)
{
	if (uap->dmatx.chan)
		dma_release_channel(uap->dmatx.chan);
	if (uap->dmarx.chan)
		dma_release_channel(uap->dmarx.chan);
}

/* Forward declare these for the refill routine */
static int pl011_dma_tx_refill(struct uart_amba_port *uap);
static void pl011_start_tx_pio(struct uart_amba_port *uap);

/*
 * The current DMA TX buffer has been sent.
 * Try to queue up another DMA buffer.
 */
static void pl011_dma_tx_callback(void *data)
{
	struct uart_amba_port *uap = data;
	struct pl011_dmatx_data *dmatx = &uap->dmatx;
	unsigned long flags;
	u16 dmacr;

	spin_lock_irqsave(&uap->port.lock, flags);
	if (uap->dmatx.queued)
		dma_unmap_sg(dmatx->chan->device->dev, &dmatx->sg, 1,
			     DMA_TO_DEVICE);

	dmacr = uap->dmacr;
	uap->dmacr = dmacr & ~UART011_TXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);

	/*
	 * If TX DMA was disabled, it means that we've stopped the DMA for
	 * some reason (eg, XOFF received, or we want to send an X-char.)
	 *
	 * Note: we need to be careful here of a potential race between DMA
	 * and the rest of the driver - if the driver disables TX DMA while
	 * a TX buffer completing, we must update the tx queued status to
	 * get further refills (hence we check dmacr).
	 */
	if (!(dmacr & UART011_TXDMAE) || uart_tx_stopped(&uap->port) ||
	    uart_circ_empty(&uap->port.state->xmit)) {
		uap->dmatx.queued = false;
		spin_unlock_irqrestore(&uap->port.lock, flags);
		return;
	}

	if (pl011_dma_tx_refill(uap) <= 0)
		/*
		 * We didn't queue a DMA buffer for some reason, but we
		 * have data pending to be sent.  Re-enable the TX IRQ.
		 */
		pl011_start_tx_pio(uap);

	spin_unlock_irqrestore(&uap->port.lock, flags);
}

/*
 * Try to refill the TX DMA buffer.
 * Locking: called with port lock held and IRQs disabled.
 * Returns:
 *   1 if we queued up a TX DMA buffer.
 *   0 if we didn't want to handle this by DMA
 *  <0 on error
 */
static int pl011_dma_tx_refill(struct uart_amba_port *uap)
{
	struct pl011_dmatx_data *dmatx = &uap->dmatx;
	struct dma_chan *chan = dmatx->chan;
	struct dma_device *dma_dev = chan->device;
	struct dma_async_tx_descriptor *desc;
	struct circ_buf *xmit = &uap->port.state->xmit;
	unsigned int count;

	/*
	 * Try to avoid the overhead involved in using DMA if the
	 * transaction fits in the first half of the FIFO, by using
	 * the standard interrupt handling.  This ensures that we
	 * issue a uart_write_wakeup() at the appropriate time.
	 */
	count = uart_circ_chars_pending(xmit);
	if (count < (uap->fifosize >> 1)) {
		uap->dmatx.queued = false;
		return 0;
	}

#if defined CONFIG_HISI_UART_OVERSAMPLING

#else

	/*
	 * Bodge: don't send the last character by DMA, as this
	 * will prevent XON from notifying us to restart DMA.
	 */
	count -= 1;
#endif

	/* Else proceed to copy the TX chars to the DMA buffer and fire DMA */
	if (count > PL011_DMA_BUFFER_SIZE)
		count = PL011_DMA_BUFFER_SIZE;

	if (xmit->tail < xmit->head)
		memcpy(&dmatx->buf[0], &xmit->buf[xmit->tail], count);
	else {
		size_t first = UART_XMIT_SIZE - xmit->tail;
		size_t second;

		if (first > count)
			first = count;
		second = count - first;

		memcpy(&dmatx->buf[0], &xmit->buf[xmit->tail], first);
		if (second)
			memcpy(&dmatx->buf[first], &xmit->buf[0], second);
	}

	dmatx->sg.length = count;

	if (dma_map_sg(dma_dev->dev, &dmatx->sg, 1, DMA_TO_DEVICE) != 1) {
		uap->dmatx.queued = false;
		dev_dbg(uap->port.dev, "unable to map TX DMA\n");
		return -EBUSY;
	}

	desc = dmaengine_prep_slave_sg(chan, &dmatx->sg, 1, DMA_MEM_TO_DEV,
					     DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		dma_unmap_sg(dma_dev->dev, &dmatx->sg, 1, DMA_TO_DEVICE);
		uap->dmatx.queued = false;
		/*
		 * If DMA cannot be used right now, we complete this
		 * transaction via IRQ and let the TTY layer retry.
		 */
		dev_dbg(uap->port.dev, "TX DMA busy\n");
		return -EBUSY;
	}

	/* Some data to go along to the callback */
	desc->callback = pl011_dma_tx_callback;
	desc->callback_param = uap;

	/* All errors should happen at prepare time */
	dmaengine_submit(desc);

	/* Fire the DMA transaction */
	dma_dev->device_issue_pending(chan);

	uap->dmacr |= UART011_TXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);
	uap->dmatx.queued = true;

	/*
	 * Now we know that DMA will fire, so advance the ring buffer
	 * with the stuff we just dispatched.
	 */
	xmit->tail = (xmit->tail + count) & (UART_XMIT_SIZE - 1);
	uap->port.icount.tx += count;

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(&uap->port);

	return 1;
}

/*
 * We received a transmit interrupt without a pending X-char but with
 * pending characters.
 * Locking: called with port lock held and IRQs disabled.
 * Returns:
 *   false if we want to use PIO to transmit
 *   true if we queued a DMA buffer
 */
static bool pl011_dma_tx_irq(struct uart_amba_port *uap)
{
	if (!uap->using_tx_dma)
		return false;

	/*
	 * If we already have a TX buffer queued, but received a
	 * TX interrupt, it will be because we've just sent an X-char.
	 * Ensure the TX DMA is enabled and the TX IRQ is disabled.
	 */
	if (uap->dmatx.queued) {
		uap->dmacr |= UART011_TXDMAE;
		pl011_writew(uap, uap->dmacr, REG_DMACR);
		uap->im &= ~UART011_TXIM;
		pl011_writew(uap, uap->im, REG_IMSC);
		return true;
	}

	/*
	 * We don't have a TX buffer queued, so try to queue one.
	 * If we successfully queued a buffer, mask the TX IRQ.
	 */
	if (pl011_dma_tx_refill(uap) > 0) {
		uap->im &= ~UART011_TXIM;
		pl011_writew(uap, uap->im, REG_IMSC);
		return true;
	}
	return false;
}

/*
 * Stop the DMA transmit (eg, due to received XOFF).
 * Locking: called with port lock held and IRQs disabled.
 */
static inline void pl011_dma_tx_stop(struct uart_amba_port *uap)
{
	if (uap->dmatx.queued) {
		uap->dmacr &= ~UART011_TXDMAE;
		pl011_writew(uap, uap->dmacr, REG_DMACR);
	}
}

/*
 * Try to start a DMA transmit, or in the case of an XON/OFF
 * character queued for send, try to get that character out ASAP.
 * Locking: called with port lock held and IRQs disabled.
 * Returns:
 *   false if we want the TX IRQ to be enabled
 *   true if we have a buffer queued
 */
static inline bool pl011_dma_tx_start(struct uart_amba_port *uap)
{
	u16 dmacr;

	if (!uap->using_tx_dma)
		return false;

	if (!uap->port.x_char) {
		/* no X-char, try to push chars out in DMA mode */
		bool ret = true;

		if (!uap->dmatx.queued) {
			if (pl011_dma_tx_refill(uap) > 0) {
				uap->im &= ~UART011_TXIM;
				pl011_writew(uap, uap->im, REG_IMSC);
			} else
				ret = false;
		} else if (!(uap->dmacr & UART011_TXDMAE)) {
			uap->dmacr |= UART011_TXDMAE;
			pl011_writew(uap, uap->dmacr, REG_DMACR);
		}
		return ret;
	}

	/*
	 * We have an X-char to send.  Disable DMA to prevent it loading
	 * the TX fifo, and then see if we can stuff it into the FIFO.
	 */
	dmacr = uap->dmacr;
	uap->dmacr &= ~UART011_TXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);

	if (pl011_readw(uap, REG_FR) & UART01x_FR_TXFF) {
		/*
		 * No space in the FIFO, so enable the transmit interrupt
		 * so we know when there is space.  Note that once we've
		 * loaded the character, we should just re-enable DMA.
		 */
		return false;
	}

	pl011_writew(uap, uap->port.x_char, REG_DR);
	uap->port.icount.tx++;
	uap->port.x_char = 0;

	/* Success - restore the DMA state */
	uap->dmacr = dmacr;
	pl011_writew(uap, dmacr, REG_DMACR);

	return true;
}

/*
 * Flush the transmit buffer.
 * Locking: called with port lock held and IRQs disabled.
 */
static void pl011_dma_flush_buffer(struct uart_port *port)
__releases(&uap->port.lock)
__acquires(&uap->port.lock)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	if (!uap->using_tx_dma)
		return;

	/* Avoid deadlock with the DMA engine callback */
	spin_unlock(&uap->port.lock);
	dmaengine_terminate_all(uap->dmatx.chan);
	spin_lock(&uap->port.lock);
	if (uap->dmatx.queued) {
		dma_unmap_sg(uap->dmatx.chan->device->dev, &uap->dmatx.sg, 1,
			     DMA_TO_DEVICE);
		uap->dmatx.queued = false;
		uap->dmacr &= ~UART011_TXDMAE;
		pl011_writew(uap, uap->dmacr, REG_DMACR);
	}
}

static void pl011_dma_rx_callback(void *data);

static int pl011_dma_rx_trigger_dma(struct uart_amba_port *uap)
{
	struct dma_chan *rxchan = uap->dmarx.chan;
	struct pl011_dmarx_data *dmarx = &uap->dmarx;
	struct dma_async_tx_descriptor *desc;
	struct pl011_sgbuf *sgbuf;

	if (!rxchan)
		return -EIO;

	/* Start the RX DMA job */
	sgbuf = uap->dmarx.use_buf_b ?
		&uap->dmarx.sgbuf_b : &uap->dmarx.sgbuf_a;
	desc = dmaengine_prep_slave_sg(rxchan, &sgbuf->sg, 1,
					DMA_DEV_TO_MEM,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	/*
	 * If the DMA engine is busy and cannot prepare a
	 * channel, no big deal, the driver will fall back
	 * to interrupt mode as a result of this error code.
	 */
	if (!desc) {
		uap->dmarx.running = false;
		dmaengine_terminate_all(rxchan);
		return -EBUSY;
	}

	/* Some data to go along to the callback */
	desc->callback = pl011_dma_rx_callback;
	desc->callback_param = uap;
	dmarx->cookie = dmaengine_submit(desc);
	dma_async_issue_pending(rxchan);

	uap->dmacr |= UART011_RXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);
	uap->dmarx.running = true;

	uap->im &= ~UART011_RXIM;
	pl011_writew(uap, uap->im, REG_IMSC);

	return 0;
}

/*
 * This is called when either the DMA job is complete, or
 * the FIFO timeout interrupt occurred. This must be called
 * with the port spinlock uap->port.lock held.
 */
static void pl011_dma_rx_chars(struct uart_amba_port *uap,
			       u32 pending, bool use_buf_b,
			       bool readfifo)
{
	struct tty_port *port = &uap->port.state->port;
	struct pl011_sgbuf *sgbuf = use_buf_b ?
		&uap->dmarx.sgbuf_b : &uap->dmarx.sgbuf_a;
	int dma_count = 0;
	u32 fifotaken = 0; /* only used for vdbg() */

	struct pl011_dmarx_data *dmarx = &uap->dmarx;
	int dmataken = 0;

	if (uap->dmarx.poll_rate) {
		/* The data can be taken by polling */
		dmataken = sgbuf->sg.length - dmarx->last_residue;
		/* Recalculate the pending size */
		if (pending >= dmataken)
			pending -= dmataken;
	}

	/* Pick the remain data from the DMA */
	if (pending) {

		/*
		 * First take all chars in the DMA pipe, then look in the FIFO.
		 * Note that tty_insert_flip_buf() tries to take as many chars
		 * as it can.
		 */
		dma_count = tty_insert_flip_string(port, sgbuf->buf + dmataken,
				pending);

		uap->port.icount.rx += dma_count;
		if (dma_count < pending)
			dev_warn(uap->port.dev,
				 "couldn't insert all characters (TTY is full?)\n");
	}

	/* Reset the last_residue for Rx DMA poll */
	if (uap->dmarx.poll_rate)
		dmarx->last_residue = sgbuf->sg.length;

	/*
	 * Only continue with trying to read the FIFO if all DMA chars have
	 * been taken first.
	 */
	if (dma_count == pending && readfifo) {
		/* Clear any error flags */
		pl011_writew(uap,
			     UART011_OEIS | UART011_BEIS | UART011_PEIS
			     | UART011_FEIS, REG_ICR);

		/*
		 * If we read all the DMA'd characters, and we had an
		 * incomplete buffer, that could be due to an rx error, or
		 * maybe we just timed out. Read any pending chars and check
		 * the error status.
		 *
		 * Error conditions will only occur in the FIFO, these will
		 * trigger an immediate interrupt and stop the DMA job, so we
		 * will always find the error in the FIFO, never in the DMA
		 * buffer.
		 */
		fifotaken = pl011_fifo_to_tty(uap);
	}

	spin_unlock(&uap->port.lock);
	dev_vdbg(uap->port.dev,
		 "Took %d chars from DMA buffer and %d chars from the FIFO\n",
		 dma_count, fifotaken);
	tty_flip_buffer_push(port);
	spin_lock(&uap->port.lock);
}

static void pl011_dma_rx_irq(struct uart_amba_port *uap)
{
	struct pl011_dmarx_data *dmarx = &uap->dmarx;
	struct dma_chan *rxchan = dmarx->chan;
	struct pl011_sgbuf *sgbuf = dmarx->use_buf_b ?
		&dmarx->sgbuf_b : &dmarx->sgbuf_a;
	size_t pending;
	struct dma_tx_state state;
	enum dma_status dmastat;

#ifdef CONFIG_HISI_AMBA_PL011
	/* Disable RX DMA - incoming data will wait in the FIFO */
	uap->dmacr &= ~UART011_RXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);
#endif

	/*
	 * Pause the transfer so we can trust the current counter,
	 * do this before we pause the PL011 block, else we may
	 * overflow the FIFO.
	 */
	if (dmaengine_pause(rxchan))
		dev_err(uap->port.dev, "unable to pause DMA transfer\n");
	dmastat = rxchan->device->device_tx_status(rxchan,
						   dmarx->cookie, &state);
	if (dmastat != DMA_PAUSED)
		dev_err(uap->port.dev, "unable to pause DMA transfer\n");

#ifdef CONFIG_HISI_AMBA_PL011
	if (dmastat == DMA_COMPLETE || state.residue == 0) {
		uap->port.icount.buf_overrun++;
		uap->im &= ~UART011_RTIM;
		pl011_writew(uap, uap->im, REG_IMSC);
		return;
	}
#else
	/* Disable RX DMA - incoming data will wait in the FIFO */
	uap->dmacr &= ~UART011_RXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);
#endif

	uap->dmarx.running = false;

	pending = sgbuf->sg.length - state.residue;
	BUG_ON(pending > PL011_DMA_BUFFER_SIZE);
	/* Then we terminate the transfer - we now know our residue */
	dmaengine_terminate_all(rxchan);

	/*
	 * This will take the chars we have so far and insert
	 * into the framework.
	 */
	pl011_dma_rx_chars(uap, pending, dmarx->use_buf_b, true);

	/* Switch buffer & re-trigger DMA job */
	dmarx->use_buf_b = !dmarx->use_buf_b;
	if (pl011_dma_rx_trigger_dma(uap)) {
		dev_dbg(uap->port.dev, "could not retrigger RX DMA job "
			"fall back to interrupt mode\n");
		uap->im |= UART011_RXIM;
		pl011_writew(uap, uap->im, REG_IMSC);
	}
}

static void pl011_dma_rx_callback(void *data)
{
	struct uart_amba_port *uap = data;
	struct pl011_dmarx_data *dmarx = &uap->dmarx;
	struct dma_chan *rxchan = dmarx->chan;
	bool lastbuf = dmarx->use_buf_b;
	struct pl011_sgbuf *sgbuf = dmarx->use_buf_b ?
		&dmarx->sgbuf_b : &dmarx->sgbuf_a;
	size_t pending;
	struct dma_tx_state state;
	int ret;

	/*
	 * This completion interrupt occurs typically when the
	 * RX buffer is totally stuffed but no timeout has yet
	 * occurred. When that happens, we just want the RX
	 * routine to flush out the secondary DMA buffer while
	 * we immediately trigger the next DMA job.
	 */
	spin_lock_irq(&uap->port.lock);
#ifdef CONFIG_HISI_AMBA_PL011
	uap->dmacr &= ~UART011_RXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);
#endif

	/*
	 * Rx data can be taken by the UART interrupts during
	 * the DMA irq handler. So we check the residue here.
	 */
	rxchan->device->device_tx_status(rxchan, dmarx->cookie, &state);
	pending = sgbuf->sg.length - state.residue;
	BUG_ON(pending > PL011_DMA_BUFFER_SIZE);
	/* Then we terminate the transfer - we now know our residue */
	dmaengine_terminate_all(rxchan);

	uap->dmarx.running = false;
	dmarx->use_buf_b = !lastbuf;
	ret = pl011_dma_rx_trigger_dma(uap);

	pl011_dma_rx_chars(uap, pending, lastbuf, false);
	spin_unlock_irq(&uap->port.lock);
	/*
	 * Do this check after we picked the DMA chars so we don't
	 * get some IRQ immediately from RX.
	 */
#ifdef CONFIG_HISI_AMBA_PL011
	if (ret) {
		dev_dbg(uap->port.dev, "could not retrigger RX DMA job "
			"fall back to interrupt mode\n");
		uap->im |= UART011_RXIM | UART011_RTIM;
		pl011_writew(uap, uap->im, REG_IMSC);
	} else {
		uap->im &= ~UART011_RXIM;
		uap->im |= UART011_RTIM;
		pl011_writew(uap, uap->im, REG_IMSC);
	}
#else
	if (ret) {
		dev_dbg(uap->port.dev, "could not retrigger RX DMA job "
			"fall back to interrupt mode\n");
		uap->im |= UART011_RXIM;
		pl011_writew(uap, uap->im, REG_IMSC);
	}
#endif
}

/*
 * Stop accepting received characters, when we're shutting down or
 * suspending this port.
 * Locking: called with port lock held and IRQs disabled.
 */
static inline void pl011_dma_rx_stop(struct uart_amba_port *uap)
{
	/* FIXME.  Just disable the DMA enable */
	uap->dmacr &= ~UART011_RXDMAE;
	pl011_writew(uap, uap->dmacr, REG_DMACR);
}

/*
 * Timer handler for Rx DMA polling.
 * Every polling, It checks the residue in the dma buffer and transfer
 * data to the tty. Also, last_residue is updated for the next polling.
 */
static void pl011_dma_rx_poll(unsigned long args)
{
	struct uart_amba_port *uap = (struct uart_amba_port *)args;
	struct tty_port *port = &uap->port.state->port;
	struct pl011_dmarx_data *dmarx = &uap->dmarx;
	struct dma_chan *rxchan = uap->dmarx.chan;
	unsigned long flags = 0;
	unsigned int dmataken = 0;
	unsigned int size = 0;
	struct pl011_sgbuf *sgbuf;
	int dma_count;
	struct dma_tx_state state;

	sgbuf = dmarx->use_buf_b ? &uap->dmarx.sgbuf_b : &uap->dmarx.sgbuf_a;
	rxchan->device->device_tx_status(rxchan, dmarx->cookie, &state);
	if (likely(state.residue < dmarx->last_residue)) {
		dmataken = sgbuf->sg.length - dmarx->last_residue;
		size = dmarx->last_residue - state.residue;
		dma_count = tty_insert_flip_string(port, sgbuf->buf + dmataken,
				size);
		if (dma_count == size)
			dmarx->last_residue =  state.residue;
		dmarx->last_jiffies = jiffies;
	}
	tty_flip_buffer_push(port);

	/*
	 * If no data is received in poll_timeout, the driver will fall back
	 * to interrupt mode. We will retrigger DMA at the first interrupt.
	 */
	if (jiffies_to_msecs(jiffies - dmarx->last_jiffies)
			> uap->dmarx.poll_timeout) {

		spin_lock_irqsave(&uap->port.lock, flags);
		pl011_dma_rx_stop(uap);
		uap->im |= UART011_RXIM;
		pl011_writew(uap, uap->im, REG_IMSC);
		spin_unlock_irqrestore(&uap->port.lock, flags);

		uap->dmarx.running = false;
		dmaengine_terminate_all(rxchan);
		del_timer(&uap->dmarx.timer);
	} else {
		mod_timer(&uap->dmarx.timer,
			jiffies + msecs_to_jiffies(uap->dmarx.poll_rate));
	}
}

static void pl011_dma_startup(struct uart_amba_port *uap)
{
	int ret;

	if (!uap->dma_probed)
		pl011_dma_probe(uap);

	if (!uap->dmatx.chan)
		return;

	uap->dmatx.buf = kmalloc(PL011_DMA_BUFFER_SIZE, GFP_KERNEL | __GFP_DMA);
	if (!uap->dmatx.buf) {
		dev_err(uap->port.dev, "no memory for DMA TX buffer\n");
		uap->port.fifosize = uap->fifosize;
		return;
	}

	sg_init_one(&uap->dmatx.sg, uap->dmatx.buf, PL011_DMA_BUFFER_SIZE);

	/* The DMA buffer is now the FIFO the TTY subsystem can use */
	uap->port.fifosize = PL011_DMA_BUFFER_SIZE;
	uap->using_tx_dma = true;

	if (!uap->dmarx.chan)
		goto skip_rx;

	/* Allocate and map DMA RX buffers */
#ifdef CONFIG_HISI_AMBA_PL011
	if(!uap->pm_op_in_progress){
#endif
		ret = pl011_sgbuf_init(uap->dmarx.chan, &uap->dmarx.sgbuf_a,
					   DMA_FROM_DEVICE);
		if (ret) {
			dev_err(uap->port.dev, "failed to init DMA %s: %d\n",
				"RX buffer A", ret);
			goto skip_rx;
		}

		ret = pl011_sgbuf_init(uap->dmarx.chan, &uap->dmarx.sgbuf_b,
					   DMA_FROM_DEVICE);
		if (ret) {
			dev_err(uap->port.dev, "failed to init DMA %s: %d\n",
				"RX buffer B", ret);
			pl011_sgbuf_free(uap->dmarx.chan, &uap->dmarx.sgbuf_a,
					 DMA_FROM_DEVICE);
			goto skip_rx;
		}
#ifdef CONFIG_HISI_AMBA_PL011
	}
#endif
	uap->using_rx_dma = true;

skip_rx:
	/* Turn on DMA error (RX/TX will be enabled on demand) */
	uap->dmacr |= UART011_DMAONERR;
	pl011_writew(uap, uap->dmacr, REG_DMACR);

	/*
	 * ST Micro variants has some specific dma burst threshold
	 * compensation. Set this to 16 bytes, so burst will only
	 * be issued above/below 16 bytes.
	 */
	if (uap->vendor->dma_threshold)
		pl011_writew(uap,
			     ST_UART011_DMAWM_RX_16 | ST_UART011_DMAWM_TX_16,
			     REG_ST_DMAWM);

	if (uap->using_rx_dma) {
		if (pl011_dma_rx_trigger_dma(uap))
			dev_dbg(uap->port.dev, "could not trigger initial "
				"RX DMA job, fall back to interrupt mode\n");
		if (uap->dmarx.poll_rate) {
			init_timer(&(uap->dmarx.timer));
			uap->dmarx.timer.function = pl011_dma_rx_poll;
			uap->dmarx.timer.data = (unsigned long)uap;
			mod_timer(&uap->dmarx.timer,
				jiffies +
				msecs_to_jiffies(uap->dmarx.poll_rate));
			uap->dmarx.last_residue = PL011_DMA_BUFFER_SIZE;
			uap->dmarx.last_jiffies = jiffies;
		}
	}
}

static void pl011_dma_shutdown(struct uart_amba_port *uap)
{
#ifdef CONFIG_HISI_AMBA_PL011
	unsigned int cr;
#endif

	if (!(uap->using_tx_dma || uap->using_rx_dma))
		return;

#ifdef CONFIG_HISI_AMBA_PL011
	cr = pl011_readw(uap, REG_CR);
	cr &= ~UART011_CR_CTSEN;
	pl011_writew(uap, cr, REG_CR);
#endif
	/* Disable RX and TX DMA */
	while (pl011_readw(uap, REG_FR) & uap->fr_busy)
		barrier();

	spin_lock_irq(&uap->port.lock);
	uap->dmacr &= ~(UART011_DMAONERR | UART011_RXDMAE | UART011_TXDMAE);
	pl011_writew(uap, uap->dmacr, REG_DMACR);
	spin_unlock_irq(&uap->port.lock);

	if (uap->using_tx_dma) {
		/* In theory, this should already be done by pl011_dma_flush_buffer */
		dmaengine_terminate_all(uap->dmatx.chan);
		if (uap->dmatx.queued) {
			dma_unmap_sg(uap->dmatx.chan->device->dev, &uap->dmatx.sg, 1,
				     DMA_TO_DEVICE);
			uap->dmatx.queued = false;
		}

		kfree(uap->dmatx.buf);
		uap->using_tx_dma = false;
	}

	if (uap->using_rx_dma) {
		dmaengine_terminate_all(uap->dmarx.chan);
		/* Clean up the RX DMA */
#ifdef CONFIG_HISI_AMBA_PL011
		if(!uap->pm_op_in_progress){
#endif
			pl011_sgbuf_free(uap->dmarx.chan, &uap->dmarx.sgbuf_a, DMA_FROM_DEVICE);
			pl011_sgbuf_free(uap->dmarx.chan, &uap->dmarx.sgbuf_b, DMA_FROM_DEVICE);
#ifdef CONFIG_HISI_AMBA_PL011
		}
#endif
		if (uap->dmarx.poll_rate)
			del_timer_sync(&uap->dmarx.timer);
		uap->using_rx_dma = false;
	}
}

static inline bool pl011_dma_rx_available(struct uart_amba_port *uap)
{
	return uap->using_rx_dma;
}

static inline bool pl011_dma_rx_running(struct uart_amba_port *uap)
{
	return uap->using_rx_dma && uap->dmarx.running;
}

#else
/* Blank functions if the DMA engine is not available */
static inline void pl011_dma_probe(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_remove(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_startup(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_shutdown(struct uart_amba_port *uap)
{
}

static inline bool pl011_dma_tx_irq(struct uart_amba_port *uap)
{
	return false;
}

static inline void pl011_dma_tx_stop(struct uart_amba_port *uap)
{
}

static inline bool pl011_dma_tx_start(struct uart_amba_port *uap)
{
	return false;
}

static inline void pl011_dma_rx_irq(struct uart_amba_port *uap)
{
}

static inline void pl011_dma_rx_stop(struct uart_amba_port *uap)
{
}

static inline int pl011_dma_rx_trigger_dma(struct uart_amba_port *uap)
{
	return -EIO;
}

static inline bool pl011_dma_rx_available(struct uart_amba_port *uap)
{
	return false;
}

static inline bool pl011_dma_rx_running(struct uart_amba_port *uap)
{
	return false;
}

#define pl011_dma_flush_buffer	NULL
#endif

static void pl011_stop_tx(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im &= ~UART011_TXIM;
	pl011_writew(uap, uap->im, REG_IMSC);
	pl011_dma_tx_stop(uap);
}

static void pl011_tx_chars(struct uart_amba_port *uap, bool from_irq);

/* Start TX with programmed I/O only (no DMA) */
static void pl011_start_tx_pio(struct uart_amba_port *uap)
{
	uap->im |= UART011_TXIM;
	pl011_writew(uap, uap->im, REG_IMSC);
	pl011_tx_chars(uap, false);
}

static void pl011_start_tx(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	if (!pl011_dma_tx_start(uap))
		pl011_start_tx_pio(uap);
}

static void pl011_stop_rx(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im &= ~(UART011_RXIM|UART011_RTIM|UART011_FEIM|
		     UART011_PEIM|UART011_BEIM|UART011_OEIM);
	pl011_writew(uap, uap->im, REG_IMSC);

	pl011_dma_rx_stop(uap);
}

static void pl011_enable_ms(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im |= UART011_RIMIM|UART011_CTSMIM|UART011_DCDMIM|UART011_DSRMIM;
	pl011_writew(uap, uap->im, REG_IMSC);
}

static void pl011_rx_chars(struct uart_amba_port *uap)
__releases(&uap->port.lock)
__acquires(&uap->port.lock)
{
	pl011_fifo_to_tty(uap);

	spin_unlock(&uap->port.lock);
	tty_flip_buffer_push(&uap->port.state->port);
	/*
	 * If we were temporarily out of DMA mode for a while,
	 * attempt to switch back to DMA mode again.
	 */
	if (pl011_dma_rx_available(uap)) {
		if (pl011_dma_rx_trigger_dma(uap)) {
			dev_dbg(uap->port.dev, "could not trigger RX DMA job "
				"fall back to interrupt mode again\n");
#ifdef CONFIG_HISI_AMBA_PL011
			uap->im |= UART011_RXIM | UART011_RTIM;
#else
			uap->im |= UART011_RXIM;
#endif
			pl011_writew(uap, uap->im, REG_IMSC);
		} else {
#ifdef CONFIG_HISI_AMBA_PL011
			uap->im &= ~UART011_RXIM;
			uap->im |= UART011_RTIM;
			pl011_writew(uap, uap->im, REG_IMSC);
#endif
#ifdef CONFIG_DMA_ENGINE
			/* Start Rx DMA poll */
			if (uap->dmarx.poll_rate) {
				uap->dmarx.last_jiffies = jiffies;
				uap->dmarx.last_residue	= PL011_DMA_BUFFER_SIZE;
				mod_timer(&uap->dmarx.timer,
					jiffies +
					msecs_to_jiffies(uap->dmarx.poll_rate));
			}
#endif
		}
	}
	spin_lock(&uap->port.lock);
}

static bool pl011_tx_char(struct uart_amba_port *uap, unsigned char c,
			  bool from_irq)
{
	if (unlikely(!from_irq) &&
	    pl011_readw(uap, REG_FR) & UART01x_FR_TXFF)
		return false; /* unable to transmit character */

	pl011_writew(uap, c, REG_DR);
	uap->port.icount.tx++;

	return true;
}

static void pl011_tx_chars(struct uart_amba_port *uap, bool from_irq)
{
	struct circ_buf *xmit = &uap->port.state->xmit;
	int count = uap->fifosize >> 1;

	if (uap->port.x_char) {
		if (!pl011_tx_char(uap, uap->port.x_char, from_irq))
			return;
		uap->port.x_char = 0;
		--count;
	}
	if (uart_circ_empty(xmit) || uart_tx_stopped(&uap->port)) {
		pl011_stop_tx(&uap->port);
		return;
	}

	/* If we are using DMA mode, try to send some characters. */
	if (pl011_dma_tx_irq(uap))
		return;

	do {
		if (likely(from_irq) && count-- == 0)
			break;

		if (!pl011_tx_char(uap, xmit->buf[xmit->tail], from_irq))
			break;

		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
	} while (!uart_circ_empty(xmit));

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(&uap->port);

	if (uart_circ_empty(xmit))
		pl011_stop_tx(&uap->port);
}

static void pl011_modem_status(struct uart_amba_port *uap)
{
	unsigned int status, delta;

	status = pl011_readw(uap, REG_FR) & UART01x_FR_MODEM_ANY;

	delta = status ^ uap->old_status;
	uap->old_status = status;

	if (!delta)
		return;

	if (delta & UART01x_FR_DCD)
		uart_handle_dcd_change(&uap->port, status & UART01x_FR_DCD);

	if (delta & uap->fr_dsr)
		uap->port.icount.dsr++;

	if (delta & uap->fr_cts)
		uart_handle_cts_change(&uap->port, status & uap->fr_cts);

	wake_up_interruptible(&uap->port.state->port.delta_msr_wait);
}

static void check_apply_cts_event_workaround(struct uart_amba_port *uap)
{
	unsigned int dummy_read;

	if (!uap->vendor->cts_event_workaround)
		return;

	/* workaround to make sure that all bits are unlocked.. */
	pl011_writew(uap, 0x00, REG_ICR);

	/*
	 * WA: introduce 26ns(1 uart clk) delay before W1C;
	 * single apb access will incur 2 pclk(133.12Mhz) delay,
	 * so add 2 dummy reads
	 */
	dummy_read = pl011_readw(uap, REG_ICR);
	dummy_read = pl011_readw(uap, REG_ICR);
}

static irqreturn_t pl011_int(int irq, void *dev_id)
{
	struct uart_amba_port *uap = dev_id;
	unsigned long flags;
	unsigned int status, pass_counter = AMBA_ISR_PASS_LIMIT;
	u16 imsc;
	int handled = 0;

	spin_lock_irqsave(&uap->port.lock, flags);
	imsc = pl011_readw(uap, REG_IMSC);
	status = pl011_readw(uap, REG_RIS) & imsc;
	if (status) {
		do {
			check_apply_cts_event_workaround(uap);
			pl011_writew(uap, status & ~(UART011_TXIS|UART011_RTIS|
				     UART011_RXIS), REG_ICR);

			if (status & (UART011_RTIS|UART011_RXIS)) {
				if (pl011_dma_rx_running(uap))
					pl011_dma_rx_irq(uap);
				else
					pl011_rx_chars(uap);
			}
			if (status & (UART011_DSRMIS|UART011_DCDMIS|
				      UART011_CTSMIS|UART011_RIMIS))
				pl011_modem_status(uap);
			if (status & UART011_TXIS)
				pl011_tx_chars(uap, true);

			if (pass_counter-- == 0)
				break;

			status = pl011_readw(uap, REG_RIS) & imsc;
		} while (status != 0);
		handled = 1;
	}

	spin_unlock_irqrestore(&uap->port.lock, flags);

	return IRQ_RETVAL(handled);
}

static unsigned int pl011_tx_empty(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int status = pl011_readw(uap, REG_FR);
	return status & (uap->fr_busy|UART01x_FR_TXFF) ? 0 : TIOCSER_TEMT;
}

static unsigned int pl011_get_mctrl(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int result = 0;
	unsigned int status = pl011_readw(uap, REG_FR);

#define TIOCMBIT(uartbit, tiocmbit)	\
	if (status & uartbit)		\
		result |= tiocmbit

	TIOCMBIT(UART01x_FR_DCD, TIOCM_CAR);
	TIOCMBIT(uap->fr_dsr, TIOCM_DSR);
	TIOCMBIT(uap->fr_cts, TIOCM_CTS);
	TIOCMBIT(uap->fr_ri, TIOCM_RNG);
#undef TIOCMBIT
	return result;
}

static void pl011_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int cr;

	cr = pl011_readw(uap, REG_CR);

#define	TIOCMBIT(tiocmbit, uartbit)		\
	if (mctrl & tiocmbit)		\
		cr |= uartbit;		\
	else				\
		cr &= ~uartbit

	TIOCMBIT(TIOCM_RTS, UART011_CR_RTS);
	TIOCMBIT(TIOCM_DTR, UART011_CR_DTR);
	TIOCMBIT(TIOCM_OUT1, UART011_CR_OUT1);
	TIOCMBIT(TIOCM_OUT2, UART011_CR_OUT2);
	TIOCMBIT(TIOCM_LOOP, UART011_CR_LBE);

	if (uap->autorts) {
		/* We need to disable auto-RTS if we want to turn RTS off */
		TIOCMBIT(TIOCM_RTS, UART011_CR_RTSEN);
	}
#undef TIOCMBIT

	pl011_writew(uap, cr, REG_CR);
}

static void pl011_break_ctl(struct uart_port *port, int break_state)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned long flags;
	unsigned int lcr_h;

	spin_lock_irqsave(&uap->port.lock, flags);
	lcr_h = pl011_readw(uap, uap->lcrh_tx);
	if (break_state == -1)
		lcr_h |= UART01x_LCRH_BRK;
	else
		lcr_h &= ~UART01x_LCRH_BRK;
	pl011_writew(uap, lcr_h, uap->lcrh_tx);
	spin_unlock_irqrestore(&uap->port.lock, flags);
}

#ifdef CONFIG_CONSOLE_POLL

static void pl011_quiesce_irqs(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	pl011_writew(uap, pl011_readw(uap, REG_MIS), REG_ICR);
	/*
	 * There is no way to clear TXIM as this is "ready to transmit IRQ", so
	 * we simply mask it. start_tx() will unmask it.
	 *
	 * Note we can race with start_tx(), and if the race happens, the
	 * polling user might get another interrupt just after we clear it.
	 * But it should be OK and can happen even w/o the race, e.g.
	 * controller immediately got some new data and raised the IRQ.
	 *
	 * And whoever uses polling routines assumes that it manages the device
	 * (including tx queue), so we're also fine with start_tx()'s caller
	 * side.
	 */
	pl011_writew(uap, pl011_readw(uap, REG_IMSC) & ~UART011_TXIM, REG_IMSC);
}

static int pl011_get_poll_char(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int status;

	/*
	 * The caller might need IRQs lowered, e.g. if used with KDB NMI
	 * debugger.
	 */
	pl011_quiesce_irqs(port);

	status = pl011_readw(uap, REG_FR);
	if (status & UART01x_FR_RXFE)
		return NO_POLL_CHAR;

	return pl011_readw(uap, REG_DR);
}

static void pl011_put_poll_char(struct uart_port *port,
			 unsigned char ch)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	while (pl011_readw(uap, REG_FR) & UART01x_FR_TXFF)
		barrier();

	pl011_writew(uap, ch, REG_DR);
}

#endif /* CONFIG_CONSOLE_POLL */

static int pl011_hwinit(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	int retval;

#ifndef CONFIG_HISI_AMBA_PL011
	/* Optionaly enable pins to be muxed in and configured */
	pinctrl_pm_select_default_state(port->dev);
#endif

	/*
	 * Try to enable the clock producer.
	 */
	retval = clk_prepare_enable(uap->clk);
	if (retval)
		return retval;

	uap->port.uartclk = clk_get_rate(uap->clk);

	/* Clear pending error and receive interrupts */
	pl011_writew(uap, UART011_OEIS | UART011_BEIS | UART011_PEIS |
		     UART011_FEIS | UART011_RTIS | UART011_RXIS, REG_ICR);

	/*
	 * Save interrupts enable mask, and enable RX interrupts in case if
	 * the interrupt is used for NMI entry.
	 */
	uap->im = pl011_readw(uap, REG_IMSC);
	pl011_writew(uap, UART011_RTIM | UART011_RXIM, REG_IMSC);

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->init)
			plat->init();
	}

#ifdef CONFIG_HISI_AMBA_PL011
	/* Optionaly enable pins to be muxed in and configured */
	if (!IS_ERR(uap->pins_default)) {
		retval = pinctrl_select_state(uap->pinctrl, uap->pins_default);
		if (retval) {
			dev_err(port->dev, "could not set default pins\n");
			return retval;
		}
	}
#endif
	return 0;
}

static void pl011_write_lcr_h(struct uart_amba_port *uap, unsigned int lcr_h)
{
	pl011_writew(uap, lcr_h, uap->lcrh_rx);
	if (is_implemented(uap, REG_ST_LCRH_RX)) {
		int i;
		/*
		 * Wait 10 PCLKs before writing LCRH_TX register,
		 * to get this delay write read only register 10 times
		 */
		for (i = 0; i < 10; ++i)
			pl011_writew(uap, 0xff, REG_MIS);
		pl011_writew(uap, lcr_h, uap->lcrh_tx);
	}
}

static int pl011_allocate_irq(struct uart_amba_port *uap)
{
	pl011_writew(uap, uap->im, REG_IMSC);

	return request_irq(uap->port.irq, pl011_int, 0, "uart-pl011", uap);
}

/*
 * Enable interrupts, only timeouts when using DMA
 * if initial RX DMA job failed, start in interrupt mode
 * as well.
 */
static void pl011_enable_interrupts(struct uart_amba_port *uap)
{
	spin_lock_irq(&uap->port.lock);

	/* Clear out any spuriously appearing RX interrupts */
	pl011_writew(uap, UART011_RTIS | UART011_RXIS, REG_ICR);
	uap->im = UART011_RTIM;
	if (!pl011_dma_rx_running(uap))
		uap->im |= UART011_RXIM;
	pl011_writew(uap, uap->im, REG_IMSC);
	spin_unlock_irq(&uap->port.lock);
}

static int pl011_startup(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int cr;
	int retval;
#ifdef CONFIG_HISI_AMBA_PL011
	dev_info(port->dev, "%s: ttyAMA%d\n", __func__, port->line);
	uart_chip_reset_endisable(uap, 1);
	uart_chip_reset_endisable(uap, 0);
#endif
	retval = pl011_hwinit(port);
	if (retval)
		goto clk_dis;

	retval = pl011_allocate_irq(uap);
	if (retval)
		goto clk_dis;

	pl011_writew(uap, uap->vendor->ifls, REG_IFLS);

	spin_lock_irq(&uap->port.lock);

	/* restore RTS and DTR */
	cr = uap->old_cr & (UART011_CR_RTS | UART011_CR_DTR);
	cr |= UART01x_CR_UARTEN | UART011_CR_RXE | UART011_CR_TXE;
	pl011_writew(uap, cr, REG_CR);

#ifdef CONFIG_HISI_AMBA_PL011
	pl011_modem_status(uap);

	spin_unlock_irq(&uap->port.lock);
#else
	spin_unlock_irq(&uap->port.lock);

	/*
	 * initialise the old status of the modem signals
	 */
	uap->old_status = pl011_readw(uap, REG_FR) & UART01x_FR_MODEM_ANY;
#endif

	/* Startup DMA */
	pl011_dma_startup(uap);

	pl011_enable_interrupts(uap);

	return 0;

 clk_dis:
	clk_disable_unprepare(uap->clk);
	return retval;
}

static int sbsa_uart_startup(struct uart_port *port)
{
	struct uart_amba_port *uap =
		container_of(port, struct uart_amba_port, port);
	int retval;

	retval = pl011_hwinit(port);
	if (retval)
		return retval;

	retval = pl011_allocate_irq(uap);
	if (retval)
		return retval;

	/* The SBSA UART does not support any modem status lines. */
	uap->old_status = 0;

	pl011_enable_interrupts(uap);

	return 0;
}

static void pl011_shutdown_channel(struct uart_amba_port *uap,
					unsigned int lcrh)
{
	unsigned long val;

	val = pl011_readw(uap, lcrh);
	val &= ~(UART01x_LCRH_BRK | UART01x_LCRH_FEN);
	pl011_writew(uap, val, lcrh);
}

/*
 * disable the port. It should not disable RTS and DTR.
 * Also RTS and DTR state should be preserved to restore
 * it during startup().
 */
static void pl011_disable_uart(struct uart_amba_port *uap)
{
	unsigned int cr;

	uap->autorts = false;
	spin_lock_irq(&uap->port.lock);
	cr = pl011_readw(uap, REG_CR);
	uap->old_cr = cr;
	cr &= UART011_CR_RTS | UART011_CR_DTR;
	cr |= UART01x_CR_UARTEN | UART011_CR_TXE;
	pl011_writew(uap, cr, REG_CR);
	spin_unlock_irq(&uap->port.lock);

	/*
	 * disable break condition and fifos
	 */
	pl011_shutdown_channel(uap, uap->lcrh_rx);
	if (is_implemented(uap, REG_ST_LCRH_RX))
		pl011_shutdown_channel(uap, uap->lcrh_tx);
}

static void pl011_disable_interrupts(struct uart_amba_port *uap)
{
	spin_lock_irq(&uap->port.lock);

	/* mask all interrupts and clear all pending ones */
	uap->im = 0;
	pl011_writew(uap, uap->im, REG_IMSC);
	pl011_writew(uap, 0xffff, REG_ICR);

	spin_unlock_irq(&uap->port.lock);
}

static void pl011_shutdown(struct uart_port *port)
{
	struct uart_amba_port *uap =
		container_of(port, struct uart_amba_port, port);
#ifdef CONFIG_HISI_AMBA_PL011
	int retval;

	if (port)
	    dev_info(port->dev, "%s: ttyAMA%d\n", __func__, port->line);
#endif

	pl011_disable_interrupts(uap);

	pl011_dma_shutdown(uap);

	free_irq(uap->port.irq, uap);

	pl011_disable_uart(uap);

	/*
	 * Shut down the clock producer
	 */
	clk_disable_unprepare(uap->clk);
	/* Optionally let pins go into sleep states */
#ifdef CONFIG_HISI_AMBA_PL011
	if (!IS_ERR(uap->pins_idle) && ((0==console_uart_name_is_ttyAMA) || (get_console_index() != (int)uap->port.line))) {
		retval = pinctrl_select_state(uap->pinctrl, uap->pins_idle);
		if (retval)
			dev_err(port->dev,
				"could not set pins to sleep state\n");/*lint !e613 */
	}
#else
	pinctrl_pm_select_sleep_state(port->dev);
#endif

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->exit)
			plat->exit();
	}

	if (uap->port.ops->flush_buffer)
		uap->port.ops->flush_buffer(port);
}

static void sbsa_uart_shutdown(struct uart_port *port)
{
	struct uart_amba_port *uap =
		container_of(port, struct uart_amba_port, port);

	pl011_disable_interrupts(uap);

	free_irq(uap->port.irq, uap);

	if (uap->port.ops->flush_buffer)
		uap->port.ops->flush_buffer(port);
}

static void
pl011_setup_status_masks(struct uart_port *port, struct ktermios *termios)
{
	port->read_status_mask = UART011_DR_OE | 255;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UART011_DR_FE | UART011_DR_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= UART011_DR_BE;

	/*
	 * Characters to ignore
	 */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UART011_DR_FE | UART011_DR_PE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UART011_DR_BE;
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UART011_DR_OE;
	}

	/*
	 * Ignore all characters if CREAD is not set.
	 */
	if ((termios->c_cflag & CREAD) == 0)
		port->ignore_status_mask |= UART_DUMMY_DR_RX;
}

static void
pl011_set_termios(struct uart_port *port, struct ktermios *termios,
		     struct ktermios *old)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int lcr_h, old_cr;
	unsigned long flags;
	unsigned int baud, quot, clkdiv;
#ifdef CONFIG_HISI_AMBA_PL011
	unsigned int count = 0;
#endif

	if (uap->vendor->oversampling)
		clkdiv = 8;
	else
		clkdiv = 16;

	/*
	 * Ask the core to calculate the divisor for us.
	 */
	baud = uart_get_baud_rate(port, termios, old, 0,
				  port->uartclk / clkdiv);
#ifdef CONFIG_DMA_ENGINE
	/*
	 * Adjust RX DMA polling rate with baud rate if not specified.
	 */
	if (uap->dmarx.auto_poll_rate)
		uap->dmarx.poll_rate = DIV_ROUND_UP(10000000, baud);
#endif

	if (baud > port->uartclk/16)
		quot = DIV_ROUND_CLOSEST(port->uartclk * 8, baud);
	else
		quot = DIV_ROUND_CLOSEST(port->uartclk * 4, baud);

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lcr_h = UART01x_LCRH_WLEN_5;
		break;
	case CS6:
		lcr_h = UART01x_LCRH_WLEN_6;
		break;
	case CS7:
		lcr_h = UART01x_LCRH_WLEN_7;
		break;
	default: // CS8
		lcr_h = UART01x_LCRH_WLEN_8;
		break;
	}
	if (termios->c_cflag & CSTOPB)
		lcr_h |= UART01x_LCRH_STP2;
	if (termios->c_cflag & PARENB) {
		lcr_h |= UART01x_LCRH_PEN;
		if (!(termios->c_cflag & PARODD))
			lcr_h |= UART01x_LCRH_EPS;
	}
	if (uap->fifosize > 1)
		lcr_h |= UART01x_LCRH_FEN;

	spin_lock_irqsave(&port->lock, flags);

	/*
	 * Update the per-port timeout.
	 */
	uart_update_timeout(port, termios->c_cflag, baud);

	pl011_setup_status_masks(port, termios);

	if (UART_ENABLE_MS(port, termios->c_cflag))
		pl011_enable_ms(port);
#ifdef CONFIG_HISI_AMBA_PL011
	else
		hisi_pl011_disable_ms(port);
#endif
	/* first, disable everything */
	old_cr = pl011_readw(uap, REG_CR);
	pl011_writew(uap, 0, REG_CR);

	if (termios->c_cflag & CRTSCTS) {
#ifdef CONFIG_HISI_AMBA_PL011
		old_cr |= UART011_CR_RTSEN | UART011_CR_CTSEN;
#else
		if (old_cr & UART011_CR_RTS)
			old_cr |= UART011_CR_RTSEN;

		old_cr |= UART011_CR_CTSEN;
#endif
		uap->autorts = true;
	} else {
		old_cr &= ~(UART011_CR_CTSEN | UART011_CR_RTSEN);
		uap->autorts = false;
	}

#ifdef CONFIG_HISI_AMBA_PL011
	uap->port.membase = port->membase;
	while ((pl011_readw(uap, REG_FR) & UART01x_FR_BUSY) && (count < 100000)) {
		udelay(1);
		count++;
		barrier();
	}
#endif
#if defined CONFIG_HISI_UART_OVERSAMPLING

#else
	if (uap->vendor->oversampling) {
		if (baud > port->uartclk / 16)
			old_cr |= ST_UART011_CR_OVSFACT;
		else
			old_cr &= ~ST_UART011_CR_OVSFACT;
	}
#endif

	/*
	 * Workaround for the ST Micro oversampling variants to
	 * increase the bitrate slightly, by lowering the divisor,
	 * to avoid delayed sampling of start bit at high speeds,
	 * else we see data corruption.
	 */
	if (uap->vendor->oversampling) {
		if ((baud >= 3000000) && (baud < 3250000) && (quot > 1))
			quot -= 1;
		else if ((baud > 3250000) && (quot > 2))
			quot -= 2;
	}
	/* Set baud rate */
	pl011_writew(uap, quot & 0x3f, REG_FBRD);
	pl011_writew(uap, quot >> 6, REG_IBRD);

	/*
	 * ----------v----------v----------v----------v-----
	 * NOTE: lcrh_tx and lcrh_rx MUST BE WRITTEN AFTER
	 * REG_FBRD & REG_IBRD.
	 * ----------^----------^----------^----------^-----
	 */
	pl011_write_lcr_h(uap, lcr_h);
	pl011_writew(uap, old_cr, REG_CR);

	spin_unlock_irqrestore(&port->lock, flags);
}

static void
sbsa_uart_set_termios(struct uart_port *port, struct ktermios *termios,
		      struct ktermios *old)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned long flags;

	tty_termios_encode_baud_rate(termios, uap->fixed_baud, uap->fixed_baud);

	/* The SBSA UART only supports 8n1 without hardware flow control. */
	termios->c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
	termios->c_cflag &= ~(CMSPAR | CRTSCTS);
	termios->c_cflag |= CS8 | CLOCAL;

	spin_lock_irqsave(&port->lock, flags);
	uart_update_timeout(port, CS8, uap->fixed_baud);
	pl011_setup_status_masks(port, termios);
	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *pl011_type(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	return uap->port.type == PORT_AMBA ? uap->type : NULL;
}

/*
 * Release the memory region(s) being used by 'port'
 */
static void pl011_release_port(struct uart_port *port)
{
	release_mem_region(port->mapbase, SZ_4K);
}

/*
 * Request the memory region(s) being used by 'port'
 */
static int pl011_request_port(struct uart_port *port)
{
	return request_mem_region(port->mapbase, SZ_4K, "uart-pl011")
			!= NULL ? 0 : -EBUSY;
}

/*
 * Configure/autoconfigure the port.
 */
static void pl011_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE) {
		port->type = PORT_AMBA;
		pl011_request_port(port);
	}
}

/*
 * verify the new serial_struct (for TIOCSSERIAL).
 */
static int pl011_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	int ret = 0;
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_AMBA)
		ret = -EINVAL;
	if (ser->irq < 0 || ser->irq >= nr_irqs)
		ret = -EINVAL;
	if (ser->baud_base < 9600)
		ret = -EINVAL;
	return ret;
}

static struct uart_ops amba_pl011_pops = {
	.tx_empty	= pl011_tx_empty,
	.set_mctrl	= pl011_set_mctrl,
	.get_mctrl	= pl011_get_mctrl,
	.stop_tx	= pl011_stop_tx,
	.start_tx	= pl011_start_tx,
	.stop_rx	= pl011_stop_rx,
	.enable_ms	= pl011_enable_ms,
	.break_ctl	= pl011_break_ctl,
	.startup	= pl011_startup,
	.shutdown	= pl011_shutdown,
	.flush_buffer	= pl011_dma_flush_buffer,
	.set_termios	= pl011_set_termios,
	.type		= pl011_type,
	.release_port	= pl011_release_port,
	.request_port	= pl011_request_port,
	.config_port	= pl011_config_port,
	.verify_port	= pl011_verify_port,
#ifdef CONFIG_HISI_AMBA_PL011
	.pm		= pl011_pm,
#endif
#ifdef CONFIG_CONSOLE_POLL
	.poll_init     = pl011_hwinit,
	.poll_get_char = pl011_get_poll_char,
	.poll_put_char = pl011_put_poll_char,
#endif
};

static void sbsa_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int sbsa_uart_get_mctrl(struct uart_port *port)
{
	return 0;
}

static const struct uart_ops sbsa_uart_pops = {
	.tx_empty	= pl011_tx_empty,
	.set_mctrl	= sbsa_uart_set_mctrl,
	.get_mctrl	= sbsa_uart_get_mctrl,
	.stop_tx	= pl011_stop_tx,
	.start_tx	= pl011_start_tx,
	.stop_rx	= pl011_stop_rx,
	.startup	= sbsa_uart_startup,
	.shutdown	= sbsa_uart_shutdown,
	.set_termios	= sbsa_uart_set_termios,
	.type		= pl011_type,
	.release_port	= pl011_release_port,
	.request_port	= pl011_request_port,
	.config_port	= pl011_config_port,
	.verify_port	= pl011_verify_port,
#ifdef CONFIG_CONSOLE_POLL
	.poll_init     = pl011_hwinit,
	.poll_get_char = pl011_get_poll_char,
	.poll_put_char = pl011_put_poll_char,
#endif
};

#ifdef CONFIG_HISI_AMBA_PL011
#else
static struct uart_amba_port *amba_ports[UART_NR];
#endif

#ifdef CONFIG_SERIAL_AMBA_PL011_CONSOLE

#ifdef CONFIG_HISI_AMBA_PL011
void pl011_console_putchar(struct uart_port *port, int ch)
#else
static void pl011_console_putchar(struct uart_port *port, int ch)
#endif
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

#ifdef CONFIG_HISI_AMBA_PL011
	unsigned int count = 0;
	while ((pl011_readw(uap, REG_FR) & UART01x_FR_TXFF) && (count < 100000)) {
		udelay(1);
		barrier();
		count++;
	}
#else
	while (pl011_readw(uap, REG_FR) & UART01x_FR_TXFF)
		barrier();
#endif
	pl011_writew(uap, ch, REG_DR);
}

static void
pl011_console_write(struct console *co, const char *s, unsigned int count)
{
	struct uart_amba_port *uap = amba_ports[co->index];
	unsigned int status, old_cr = 0, new_cr;
	unsigned long flags;
	int locked = 1;

	clk_enable(uap->clk);

	local_irq_save(flags);
	if (uap->port.sysrq)
		locked = 0;
	else if (oops_in_progress)
		locked = spin_trylock(&uap->port.lock);
	else
		spin_lock(&uap->port.lock);

	/*
	 *	First save the CR then disable the interrupts
	 */
	if (!uap->vendor->always_enabled) {
		old_cr = pl011_readw(uap, REG_CR);
		new_cr = old_cr & ~UART011_CR_CTSEN;
		new_cr |= UART01x_CR_UARTEN | UART011_CR_TXE;
		pl011_writew(uap, new_cr, REG_CR);
	}

	uart_console_write(&uap->port, s, count, pl011_console_putchar);

	/*
	 *	Finally, wait for transmitter to become empty
	 *	and restore the TCR
	 */
	do {
		status = pl011_readw(uap, REG_FR);
	} while (status & uap->fr_busy);
	if (!uap->vendor->always_enabled)
		pl011_writew(uap, old_cr, REG_CR);

	if (locked)
		spin_unlock(&uap->port.lock);
	local_irq_restore(flags);

	clk_disable(uap->clk);
}

static void __init
pl011_console_get_options(struct uart_amba_port *uap, int *baud,
			     int *parity, int *bits)
{
	if (pl011_readw(uap, REG_CR) & UART01x_CR_UARTEN) {
		unsigned int lcr_h, ibrd, fbrd;

		lcr_h = pl011_readw(uap, uap->lcrh_tx);

		*parity = 'n';
		if (lcr_h & UART01x_LCRH_PEN) {
			if (lcr_h & UART01x_LCRH_EPS)
				*parity = 'e';
			else
				*parity = 'o';
		}

		if ((lcr_h & 0x60) == UART01x_LCRH_WLEN_7)
			*bits = 7;
		else
			*bits = 8;

		ibrd = pl011_readw(uap, REG_IBRD);
		fbrd = pl011_readw(uap, REG_FBRD);

		*baud = uap->port.uartclk * 4 / (64 * ibrd + fbrd);
#if defined CONFIG_HISI_UART_OVERSAMPLING

#else
		if (uap->vendor->oversampling) {
			if (pl011_readw(uap, REG_CR)
				  & ST_UART011_CR_OVSFACT)
				*baud *= 2;
		}
#endif
	}
}

static int pl011_console_setup(struct console *co, char *options)
{
	struct uart_amba_port *uap;
	int baud = 38400;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	int ret;

	/*
	 * Check whether an invalid uart number has been specified, and
	 * if so, search for the first available port that does have
	 * console support.
	 */
	if (co->index >= UART_NR)
		co->index = 0;
	uap = amba_ports[co->index];
	if (!uap)
		return -ENODEV;

	/* Allow pins to be muxed in and configured */
#ifdef CONFIG_HISI_AMBA_PL011
	if (!IS_ERR(uap->pins_default)) {
		ret = pinctrl_select_state(uap->pinctrl, uap->pins_idle);    /* make sure state changed, do not care return value */
		ret = pinctrl_select_state(uap->pinctrl, uap->pins_default);
		if (ret) {
			dev_err(uap->port.dev, "could not set default pins\n");
			return ret;
		}
	}
#else
	pinctrl_pm_select_default_state(uap->port.dev);
#endif

	ret = clk_prepare(uap->clk);
	if (ret)
		return ret;

#ifdef CONFIG_HISI_AMBA_PL011
	uart_chip_reset_endisable(uap, 1);
	uart_chip_reset_endisable(uap, 0);
#endif

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->init)
			plat->init();
	}

	uap->port.uartclk = clk_get_rate(uap->clk);

	if (uap->vendor->fixed_options) {
		baud = uap->fixed_baud;
	} else {
		if (options)
			uart_parse_options(options,
					   &baud, &parity, &bits, &flow);
		else
			pl011_console_get_options(uap, &baud, &parity, &bits);
    }
#ifdef CONFIG_HISI_AMBA_PL011
	ret = uart_set_options(&uap->port, co, baud, parity, bits, flow);

	if (0 == ret) {
		uap->console_fbrd = pl011_readw(uap, REG_FBRD);
		uap->console_ibrd = pl011_readw(uap, REG_IBRD);
		uap->console_lcrh_rx = pl011_readw(uap, uap->lcrh_rx);
		if (uap->lcrh_tx != uap->lcrh_rx)
			uap->console_lcrh_tx = pl011_readw(uap, uap->lcrh_tx);
	}

	return ret;
#else
	return uart_set_options(&uap->port, co, baud, parity, bits, flow);
#endif
}

static struct uart_driver amba_reg;
static struct console amba_console = {
	.name		= "ttyAMA",
#ifdef CONFIG_HISI_AMBA_PL011
	.write		= hisi_pl011_console_write,
#else
	.write		= pl011_console_write,
#endif
	.device		= uart_console_device,
	.setup		= pl011_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &amba_reg,
};

#define AMBA_CONSOLE	(&amba_console)

static void pl011_putc(struct uart_port *port, int c)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	while (pl011_readw(uap, REG_FR) & UART01x_FR_TXFF)
		;
	pl011_writeb(uap, c, REG_DR);
	while (pl011_readw(uap, REG_FR) & uap->fr_busy)
		;
}

static void pl011_early_write(struct console *con, const char *s, unsigned n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, pl011_putc);
}

static int __init pl011_early_console_setup(struct earlycon_device *device,
					    const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = pl011_early_write;
	return 0;
}
OF_EARLYCON_DECLARE(pl011, "arm,pl011", pl011_early_console_setup);

#else
#define AMBA_CONSOLE	NULL
#endif

static struct uart_driver amba_reg = {
	.owner			= THIS_MODULE,
	.driver_name		= "ttyAMA",
	.dev_name		= "ttyAMA",
	.major			= SERIAL_AMBA_MAJOR,
	.minor			= SERIAL_AMBA_MINOR,
	.nr			= UART_NR,
	.cons			= AMBA_CONSOLE,
};

static int pl011_probe_dt_alias(int index, struct device *dev)
{
	struct device_node *np;
	static bool seen_dev_with_alias = false;
	static bool seen_dev_without_alias = false;
	int ret = index;

	if (!IS_ENABLED(CONFIG_OF))
		return ret;

	np = dev->of_node;
	if (!np)
		return ret;

	ret = of_alias_get_id(np, "serial");
	if (IS_ERR_VALUE(ret)) {
		seen_dev_without_alias = true;
		ret = index;
	} else {
		seen_dev_with_alias = true;
		if (ret >= ARRAY_SIZE(amba_ports) || amba_ports[ret] != NULL) {
			dev_warn(dev, "requested serial port %d  not available.\n", ret);
			ret = index;
		}
	}

	if (seen_dev_with_alias && seen_dev_without_alias)
		dev_warn(dev, "aliased and non-aliased serial devices found in device tree. Serial port enumeration may be unpredictable.\n");

	return ret;
}

/* unregisters the driver also if no more ports are left */
static void pl011_unregister_port(struct uart_amba_port *uap)
{
	int i;
	bool busy = false;

	for (i = 0; i < ARRAY_SIZE(amba_ports); i++) {
		if (amba_ports[i] == uap)
			amba_ports[i] = NULL;
		else if (amba_ports[i])
			busy = true;
	}
	pl011_dma_remove(uap);
	if (!busy)
		uart_unregister_driver(&amba_reg);
}

static int pl011_find_free_port(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(amba_ports); i++)
		if (amba_ports[i] == NULL)
			return i;

	return -EBUSY;
}

static int pl011_setup_port(struct device *dev, struct uart_amba_port *uap,
			    struct resource *mmiobase, int index)
{
	void __iomem *base;
	int ret;

#ifdef CONFIG_HISI_AMBA_PL011
struct amba_device *pdev = container_of(dev,struct amba_device,dev);
#endif
	base = devm_ioremap_resource(dev, mmiobase);
	if (IS_ERR(base))
		return PTR_ERR(base);

	index = pl011_probe_dt_alias(index, dev);

#ifdef CONFIG_HISI_AMBA_PL011
	ret = hisi_pl011_pinctrl(pdev, uap);
	if (ret) {
		dev_err(&pdev->dev, "%s get pinctrl failed!!\n", __func__);
		return ret;
	}
#endif
	uap->old_cr = 0;
	uap->port.dev = dev;
	uap->port.mapbase = mmiobase->start;
	uap->port.membase = base;
	uap->port.iotype = UPIO_MEM;
	uap->port.fifosize = uap->fifosize;
	uap->port.flags = UPF_BOOT_AUTOCONF;
	uap->port.line = index;

	amba_ports[index] = uap;

#ifdef CONFIG_HISI_AMBA_PL011
	ret = hisi_pl011_probe_reset_func_enable(pdev, uap);
	if (ret) {
		dev_err(&pdev->dev, "%s can not enable reset function!\n", __func__);
	}

	ret = clk_prepare_enable(uap->clk);
	if (ret) {
		dev_err(&pdev->dev, "%s clk_prepare_enable failed!!\n", __func__);
		return ret;
	}

	/* Ensure interrupts from this UART are masked and cleared */
	writew(0, uap->port.membase + UART011_IMSC);
	writew(0xffff, uap->port.membase + UART011_ICR);

	clk_disable_unprepare(uap->clk);

	hisi_pl011_probe_console_enable(pdev, uap, amba_console.name);
#endif

	return 0;
}

static int pl011_register_port(struct uart_amba_port *uap)
{
	int ret;

#ifndef CONFIG_HISI_AMBA_PL011
	/* Ensure interrupts from this UART are masked and cleared */
	pl011_writew(uap, 0, REG_IMSC);
	pl011_writew(uap, 0xffff, REG_ICR);
#endif

	if (!amba_reg.state) {
		ret = uart_register_driver(&amba_reg);
		if (ret < 0) {
			dev_err(uap->port.dev,
				"Failed to register AMBA-PL011 driver\n");
			return ret;
		}
	}

	ret = uart_add_one_port(&amba_reg, &uap->port);
	if (ret) {
#ifdef CONFIG_HISI_AMBA_PL011
	if (console_uart_name_is_ttyAMA && (get_console_index() == (int)uap->port.line)) {
		pl011_tx_work_uninit(uap);
	}
#endif
		pl011_unregister_port(uap);
	}

	return ret;
}

#ifdef CONFIG_ARM_AMBA
static int pl011_probe(struct amba_device *dev, const struct amba_id *id)
{
	struct uart_amba_port *uap;
	struct vendor_data *vendor = id->data;
	int portnr, ret;

	portnr = pl011_find_free_port();
	if (portnr < 0)
		return portnr;

	uap = devm_kzalloc(&dev->dev, sizeof(struct uart_amba_port),
			   GFP_KERNEL);
	if (!uap)
		return -ENOMEM;

	uap->clk = devm_clk_get(&dev->dev, NULL);
	if (IS_ERR(uap->clk))
		return PTR_ERR(uap->clk);
#ifdef CONFIG_HISI_AMBA_PL011
	ret = hisi_pl011_probe_get_clk_freq(dev, uap, portnr);
	if (ret) {
		dev_err(&dev->dev, "%s can not get clk freq!\n", __func__);
	}
	uap->pm_op_in_progress = 0;
#endif
	uap->vendor = vendor;
	uap->reg_lut = vendor->reg_lut;
	uap->lcrh_rx = vendor->lcrh_rx;
	uap->lcrh_tx = vendor->lcrh_tx;
	uap->fr_busy = vendor->fr_busy;
	uap->fr_dsr = vendor->fr_dsr;
	uap->fr_cts = vendor->fr_cts;
	uap->fr_ri = vendor->fr_ri;
	uap->fifosize = vendor->get_fifosize(dev);
	uap->port.irq = dev->irq[0];
	uap->port.ops = &amba_pl011_pops;

	snprintf(uap->type, sizeof(uap->type), "PL011 rev%u", amba_rev(dev));

	ret = pl011_setup_port(&dev->dev, uap, &dev->res, portnr);
	if (ret)
		return ret;

	amba_set_drvdata(dev, uap);

	return pl011_register_port(uap);
}

static int pl011_remove(struct amba_device *dev)
{
	struct uart_amba_port *uap = amba_get_drvdata(dev);

#ifdef CONFIG_HISI_AMBA_PL011
	devm_pinctrl_put(uap->pinctrl);
#endif

	uart_remove_one_port(&amba_reg, &uap->port);
	pl011_unregister_port(uap);
	return 0;
}
#endif

#ifdef CONFIG_SOC_ZX296702
static int zx_uart_probe(struct platform_device *pdev)
{
	struct uart_amba_port *uap;
	struct vendor_data *vendor = &vendor_zte;
	struct resource *res;
	int portnr, ret;

	portnr = pl011_find_free_port();
	if (portnr < 0)
		return portnr;

	uap = devm_kzalloc(&pdev->dev, sizeof(struct uart_amba_port),
			GFP_KERNEL);
	if (!uap) {
		ret = -ENOMEM;
		goto out;
	}

	uap->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(uap->clk)) {
		ret = PTR_ERR(uap->clk);
		goto out;
	}

	uap->vendor	= vendor;
	uap->reg_lut	= vendor->reg_lut;
	uap->lcrh_rx	= vendor->lcrh_rx;
	uap->lcrh_tx	= vendor->lcrh_tx;
	uap->fr_busy	= vendor->fr_busy;
	uap->fr_dsr	= vendor->fr_dsr;
	uap->fr_cts	= vendor->fr_cts;
	uap->fr_ri	= vendor->fr_ri;
	uap->fifosize	= 16;
	uap->port.irq	= platform_get_irq(pdev, 0);
	uap->port.ops	= &amba_pl011_pops;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	ret = pl011_setup_port(&pdev->dev, uap, res, portnr);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, uap);

	return pl011_register_port(uap);
out:
	return ret;
}

static int zx_uart_remove(struct platform_device *pdev)
{
	struct uart_amba_port *uap = platform_get_drvdata(pdev);

	uart_remove_one_port(&amba_reg, &uap->port);
	pl011_unregister_port(uap);
	return 0;
}
#endif

#ifdef CONFIG_HISI_AMBA_PL011
#else
#ifdef CONFIG_PM_SLEEP
static int pl011_suspend(struct device *dev)
{
	struct uart_amba_port *uap = dev_get_drvdata(dev);

	if (!uap)
		return -EINVAL;

	return uart_suspend_port(&amba_reg, &uap->port);
}

static int pl011_resume(struct device *dev)
{
	struct uart_amba_port *uap = dev_get_drvdata(dev);

	if (!uap)
		return -EINVAL;

	return uart_resume_port(&amba_reg, &uap->port);
}
#endif
#endif
#ifdef CONFIG_HISI_AMBA_PL011
static SIMPLE_DEV_PM_OPS(pl011_dev_pm_ops, hisi_pl011_suspend, hisi_pl011_resume);
#else
static SIMPLE_DEV_PM_OPS(pl011_dev_pm_ops, pl011_suspend, pl011_resume);
#endif

static int sbsa_uart_probe(struct platform_device *pdev)
{
	struct uart_amba_port *uap;
	struct resource *r;
	int portnr, ret;
	int baudrate;

	/*
	 * Check the mandatory baud rate parameter in the DT node early
	 * so that we can easily exit with the error.
	 */
	if (pdev->dev.of_node) {
		struct device_node *np = pdev->dev.of_node;

		ret = of_property_read_u32(np, "current-speed", &baudrate);
		if (ret)
			return ret;
	} else {
		baudrate = 115200;
	}

	portnr = pl011_find_free_port();
	if (portnr < 0)
		return portnr;

	uap = devm_kzalloc(&pdev->dev, sizeof(struct uart_amba_port),
			   GFP_KERNEL);
	if (!uap)
		return -ENOMEM;

	uap->vendor	= &vendor_sbsa;
	uap->reg_lut	= vendor_sbsa.reg_lut;
	uap->fr_busy	= vendor_sbsa.fr_busy;
	uap->fr_dsr	= vendor_sbsa.fr_dsr;
	uap->fr_cts	= vendor_sbsa.fr_cts;
	uap->fr_ri	= vendor_sbsa.fr_ri;
	uap->fifosize	= 32;
	uap->port.irq	= platform_get_irq(pdev, 0);
	uap->port.ops	= &sbsa_uart_pops;
	uap->fixed_baud = baudrate;

	snprintf(uap->type, sizeof(uap->type), "SBSA");

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	ret = pl011_setup_port(&pdev->dev, uap, r, portnr);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, uap);

	return pl011_register_port(uap);
}

static int sbsa_uart_remove(struct platform_device *pdev)
{
	struct uart_amba_port *uap = platform_get_drvdata(pdev);

	uart_remove_one_port(&amba_reg, &uap->port);
	pl011_unregister_port(uap);
	return 0;
}

static const struct of_device_id sbsa_uart_of_match[] = {
	{ .compatible = "arm,sbsa-uart", },
	{},
};
MODULE_DEVICE_TABLE(of, sbsa_uart_of_match);

static const struct acpi_device_id sbsa_uart_acpi_match[] = {
	{ "ARMH0011", 0 },
	{},
};
MODULE_DEVICE_TABLE(acpi, sbsa_uart_acpi_match);

static struct platform_driver arm_sbsa_uart_platform_driver = {
	.probe		= sbsa_uart_probe,
	.remove		= sbsa_uart_remove,
	.driver	= {
		.name	= "sbsa-uart",
		.of_match_table = of_match_ptr(sbsa_uart_of_match),
		.acpi_match_table = ACPI_PTR(sbsa_uart_acpi_match),
	},
};

#ifdef CONFIG_ARM_AMBA
static struct amba_id pl011_ids[] = {
	{
		.id	= 0x00041011,
		.mask	= 0x000fffff,
		.data	= &vendor_arm,
	},
	{
		.id	= 0x00380802,
		.mask	= 0x00ffffff,
		.data	= &vendor_st,
	},
	{ 0, 0 },
};

MODULE_DEVICE_TABLE(amba, pl011_ids);

static struct amba_driver pl011_driver = {
	.drv = {
		.name	= "uart-pl011",
		.pm	= &pl011_dev_pm_ops,
	},
	.id_table	= pl011_ids,
	.probe		= pl011_probe,
	.remove		= pl011_remove,
};
#endif

#ifdef CONFIG_SOC_ZX296702
static const struct of_device_id zx_uart_dt_ids[] = {
	{ .compatible = "zte,zx296702-uart", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, zx_uart_dt_ids);

static struct platform_driver zx_uart_driver = {
	.driver = {
		.name	= "zx-uart",
		.owner	= THIS_MODULE,
		.pm	= &pl011_dev_pm_ops,
		.of_match_table = zx_uart_dt_ids,
	},
	.probe		= zx_uart_probe,
	.remove		= zx_uart_remove,
};
#endif


static int __init pl011_init(void)
{
	int ret;
	printk(KERN_INFO "Serial: AMBA PL011 UART driver\n");

	if (platform_driver_register(&arm_sbsa_uart_platform_driver))
		pr_warn("could not register SBSA UART platform driver\n");

#ifdef CONFIG_SOC_ZX296702
	ret = platform_driver_register(&zx_uart_driver);
	if (ret)
		pr_warn("could not register ZX UART platform driver\n");
#endif

#ifdef CONFIG_ARM_AMBA
	ret = amba_driver_register(&pl011_driver);
#endif
	return ret;
}

static void __exit pl011_exit(void)
{
	platform_driver_unregister(&arm_sbsa_uart_platform_driver);
#ifdef CONFIG_SOC_ZX296702
	platform_driver_unregister(&zx_uart_driver);
#endif
#ifdef CONFIG_ARM_AMBA
	amba_driver_unregister(&pl011_driver);
#endif
}
#ifdef CONFIG_HISI_AMBA_PL011
#include "hisi-amba-pl011.c"
#endif
/*
 * While this can be a module, if builtin it's most likely the console
 * So let's leave module_exit but move module_init to an earlier place
 */
arch_initcall(pl011_init);
module_exit(pl011_exit);

MODULE_AUTHOR("ARM Ltd/Deep Blue Solutions Ltd");
MODULE_DESCRIPTION("ARM AMBA serial port driver");
MODULE_LICENSE("GPL");
