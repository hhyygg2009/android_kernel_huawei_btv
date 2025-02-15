/*
 * HD-audio controller helpers
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <sound/core.h>
#include <sound/hdaudio.h>
#include <sound/hda_register.h>

/* clear CORB read pointer properly */
static void azx_clear_corbrp(struct hdac_bus *bus)
{
	int timeout;

	for (timeout = 1000; timeout > 0; timeout--) {
		if (snd_hdac_chip_readw(bus, CORBRP) & AZX_CORBRP_RST)
			break;
		udelay(1);
	}
	if (timeout <= 0)
		dev_err(bus->dev, "CORB reset timeout#1, CORBRP = %d\n",
			snd_hdac_chip_readw(bus, CORBRP));

	snd_hdac_chip_writew(bus, CORBRP, 0);
	for (timeout = 1000; timeout > 0; timeout--) {
		if (snd_hdac_chip_readw(bus, CORBRP) == 0)
			break;
		udelay(1);
	}
	if (timeout <= 0)
		dev_err(bus->dev, "CORB reset timeout#2, CORBRP = %d\n",
			snd_hdac_chip_readw(bus, CORBRP));
}

/**
 * snd_hdac_bus_init_cmd_io - set up CORB/RIRB buffers
 * @bus: HD-audio core bus
 */
void snd_hdac_bus_init_cmd_io(struct hdac_bus *bus)
{
	spin_lock_irq(&bus->reg_lock);
	/* CORB set up */
	bus->corb.addr = bus->rb.addr;
	bus->corb.buf = (__le32 *)bus->rb.area;
	snd_hdac_chip_writel(bus, CORBLBASE, (u32)bus->corb.addr);
	snd_hdac_chip_writel(bus, CORBUBASE, upper_32_bits(bus->corb.addr));

	/* set the corb size to 256 entries (ULI requires explicitly) */
	snd_hdac_chip_writeb(bus, CORBSIZE, 0x02);
	/* set the corb write pointer to 0 */
	snd_hdac_chip_writew(bus, CORBWP, 0);

	/* reset the corb hw read pointer */
	snd_hdac_chip_writew(bus, CORBRP, AZX_CORBRP_RST);
	if (!bus->corbrp_self_clear)
		azx_clear_corbrp(bus);

	/* enable corb dma */
	snd_hdac_chip_writeb(bus, CORBCTL, AZX_CORBCTL_RUN);

	/* RIRB set up */
	bus->rirb.addr = bus->rb.addr + 2048;
	bus->rirb.buf = (__le32 *)(bus->rb.area + 2048);
	bus->rirb.wp = bus->rirb.rp = 0;
	memset(bus->rirb.cmds, 0, sizeof(bus->rirb.cmds));
	snd_hdac_chip_writel(bus, RIRBLBASE, (u32)bus->rirb.addr);
	snd_hdac_chip_writel(bus, RIRBUBASE, upper_32_bits(bus->rirb.addr));

	/* set the rirb size to 256 entries (ULI requires explicitly) */
	snd_hdac_chip_writeb(bus, RIRBSIZE, 0x02);
	/* reset the rirb hw write pointer */
	snd_hdac_chip_writew(bus, RIRBWP, AZX_RIRBWP_RST);
	/* set N=1, get RIRB response interrupt for new entry */
	snd_hdac_chip_writew(bus, RINTCNT, 1);
	/* enable rirb dma and response irq */
	snd_hdac_chip_writeb(bus, RIRBCTL, AZX_RBCTL_DMA_EN | AZX_RBCTL_IRQ_EN);
	spin_unlock_irq(&bus->reg_lock);
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_init_cmd_io);

/**
 * snd_hdac_bus_stop_cmd_io - clean up CORB/RIRB buffers
 * @bus: HD-audio core bus
 */
void snd_hdac_bus_stop_cmd_io(struct hdac_bus *bus)
{
	spin_lock_irq(&bus->reg_lock);
	/* disable ringbuffer DMAs */
	snd_hdac_chip_writeb(bus, RIRBCTL, 0);
	snd_hdac_chip_writeb(bus, CORBCTL, 0);
	/* disable unsolicited responses */
	snd_hdac_chip_updatel(bus, GCTL, AZX_GCTL_UNSOL, 0);
	spin_unlock_irq(&bus->reg_lock);
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_stop_cmd_io);

static unsigned int azx_command_addr(u32 cmd)
{
	unsigned int addr = cmd >> 28;

	if (snd_BUG_ON(addr >= HDA_MAX_CODECS))
		addr = 0;
	return addr;
}

/**
 * snd_hdac_bus_send_cmd - send a command verb via CORB
 * @bus: HD-audio core bus
 * @val: encoded verb value to send
 *
 * Returns zero for success or a negative error code.
 */
int snd_hdac_bus_send_cmd(struct hdac_bus *bus, unsigned int val)
{
	unsigned int addr = azx_command_addr(val);
	unsigned int wp, rp;

	spin_lock_irq(&bus->reg_lock);

	bus->last_cmd[azx_command_addr(val)] = val;

	/* add command to corb */
	wp = snd_hdac_chip_readw(bus, CORBWP);
	if (wp == 0xffff) {
		/* something wrong, controller likely turned to D3 */
		spin_unlock_irq(&bus->reg_lock);
		return -EIO;
	}
	wp++;
	wp %= AZX_MAX_CORB_ENTRIES;

	rp = snd_hdac_chip_readw(bus, CORBRP);
	if (wp == rp) {
		/* oops, it's full */
		spin_unlock_irq(&bus->reg_lock);
		return -EAGAIN;
	}

	bus->rirb.cmds[addr]++;
	bus->corb.buf[wp] = cpu_to_le32(val);
	snd_hdac_chip_writew(bus, CORBWP, wp);

	spin_unlock_irq(&bus->reg_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_send_cmd);

#define AZX_RIRB_EX_UNSOL_EV	(1<<4)

/**
 * snd_hdac_bus_update_rirb - retrieve RIRB entries
 * @bus: HD-audio core bus
 *
 * Usually called from interrupt handler.
 */
void snd_hdac_bus_update_rirb(struct hdac_bus *bus)
{
	unsigned int rp, wp;
	unsigned int addr;
	u32 res, res_ex;

	wp = snd_hdac_chip_readw(bus, RIRBWP);
	if (wp == 0xffff) {
		/* something wrong, controller likely turned to D3 */
		return;
	}

	if (wp == bus->rirb.wp)
		return;
	bus->rirb.wp = wp;

	while (bus->rirb.rp != wp) {
		bus->rirb.rp++;
		bus->rirb.rp %= AZX_MAX_RIRB_ENTRIES;

		rp = bus->rirb.rp << 1; /* an RIRB entry is 8-bytes */
		res_ex = le32_to_cpu(bus->rirb.buf[rp + 1]);
		res = le32_to_cpu(bus->rirb.buf[rp]);
		addr = res_ex & 0xf;
		if (addr >= HDA_MAX_CODECS) {
			dev_err(bus->dev,
				"spurious response %#x:%#x, rp = %d, wp = %d",
				res, res_ex, bus->rirb.rp, wp);
			snd_BUG();
		} else if (res_ex & AZX_RIRB_EX_UNSOL_EV)
			snd_hdac_bus_queue_event(bus, res, res_ex);
		else if (bus->rirb.cmds[addr]) {
			bus->rirb.res[addr] = res;
			bus->rirb.cmds[addr]--;
		} else {
			dev_err_ratelimited(bus->dev,
				"spurious response %#x:%#x, last cmd=%#08x\n",
				res, res_ex, bus->last_cmd[addr]);
		}
	}
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_update_rirb);

/**
 * snd_hdac_bus_get_response - receive a response via RIRB
 * @bus: HD-audio core bus
 * @addr: codec address
 * @res: pointer to store the value, NULL when not needed
 *
 * Returns zero if a value is read, or a negative error code.
 */
int snd_hdac_bus_get_response(struct hdac_bus *bus, unsigned int addr,
			      unsigned int *res)
{
	unsigned long timeout;
	unsigned long loopcounter;

	timeout = jiffies + msecs_to_jiffies(1000);

	for (loopcounter = 0;; loopcounter++) {
		spin_lock_irq(&bus->reg_lock);
		if (!bus->rirb.cmds[addr]) {
			if (res)
				*res = bus->rirb.res[addr]; /* the last value */
			spin_unlock_irq(&bus->reg_lock);
			return 0;
		}
		spin_unlock_irq(&bus->reg_lock);
		if (time_after(jiffies, timeout))
			break;
		if (loopcounter > 3000)
			msleep(2); /* temporary workaround */
		else {
			udelay(10);
			cond_resched();
		}
	}

	return -EIO;
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_get_response);

/*
 * Lowlevel interface
 */

/**
 * snd_hdac_bus_enter_link_reset - enter link reset
 * @bus: HD-audio core bus
 *
 * Enter to the link reset state.
 */
void snd_hdac_bus_enter_link_reset(struct hdac_bus *bus)
{
	unsigned long timeout;

	/* reset controller */
	snd_hdac_chip_updatel(bus, GCTL, AZX_GCTL_RESET, 0);

	timeout = jiffies + msecs_to_jiffies(100);
	while ((snd_hdac_chip_readb(bus, GCTL) & AZX_GCTL_RESET) &&
	       time_before(jiffies, timeout))
		usleep_range(500, 1000);
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_enter_link_reset);

/**
 * snd_hdac_bus_exit_link_reset - exit link reset
 * @bus: HD-audio core bus
 *
 * Exit from the link reset state.
 */
void snd_hdac_bus_exit_link_reset(struct hdac_bus *bus)
{
	unsigned long timeout;

	snd_hdac_chip_updateb(bus, GCTL, 0, AZX_GCTL_RESET);

	timeout = jiffies + msecs_to_jiffies(100);
	while (!snd_hdac_chip_readb(bus, GCTL) && time_before(jiffies, timeout))
		usleep_range(500, 1000);
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_exit_link_reset);

/* reset codec link */
static int azx_reset(struct hdac_bus *bus, bool full_reset)
{
	if (!full_reset)
		goto skip_reset;

	/* clear STATESTS */
	snd_hdac_chip_writew(bus, STATESTS, STATESTS_INT_MASK);

	/* reset controller */
	snd_hdac_bus_enter_link_reset(bus);

	/* delay for >= 100us for codec PLL to settle per spec
	 * Rev 0.9 section 5.5.1
	 */
	usleep_range(500, 1000);

	/* Bring controller out of reset */
	snd_hdac_bus_exit_link_reset(bus);

	/* Brent Chartrand said to wait >= 540us for codecs to initialize */
	usleep_range(1000, 1200);

 skip_reset:
	/* check to see if controller is ready */
	if (!snd_hdac_chip_readb(bus, GCTL)) {
		dev_dbg(bus->dev, "azx_reset: controller not ready!\n");
		return -EBUSY;
	}

	/* Accept unsolicited responses */
	snd_hdac_chip_updatel(bus, GCTL, 0, AZX_GCTL_UNSOL);

	/* detect codecs */
	if (!bus->codec_mask) {
		bus->codec_mask = snd_hdac_chip_readw(bus, STATESTS);
		dev_dbg(bus->dev, "codec_mask = 0x%lx\n", bus->codec_mask);
	}

	return 0;
}

/* enable interrupts */
static void azx_int_enable(struct hdac_bus *bus)
{
	/* enable controller CIE and GIE */
	snd_hdac_chip_updatel(bus, INTCTL, 0, AZX_INT_CTRL_EN | AZX_INT_GLOBAL_EN);
}

/* disable interrupts */
static void azx_int_disable(struct hdac_bus *bus)
{
	struct hdac_stream *azx_dev;

	/* disable interrupts in stream descriptor */
	list_for_each_entry(azx_dev, &bus->stream_list, list)
		snd_hdac_stream_updateb(azx_dev, SD_CTL, SD_INT_MASK, 0);

	/* disable SIE for all streams */
	snd_hdac_chip_writeb(bus, INTCTL, 0);

	/* disable controller CIE and GIE */
	snd_hdac_chip_updatel(bus, INTCTL, AZX_INT_CTRL_EN | AZX_INT_GLOBAL_EN, 0);
}

/* clear interrupts */
static void azx_int_clear(struct hdac_bus *bus)
{
	struct hdac_stream *azx_dev;

	/* clear stream status */
	list_for_each_entry(azx_dev, &bus->stream_list, list)
		snd_hdac_stream_writeb(azx_dev, SD_STS, SD_INT_MASK);

	/* clear STATESTS */
	snd_hdac_chip_writew(bus, STATESTS, STATESTS_INT_MASK);

	/* clear rirb status */
	snd_hdac_chip_writeb(bus, RIRBSTS, RIRB_INT_MASK);

	/* clear int status */
	snd_hdac_chip_writel(bus, INTSTS, AZX_INT_CTRL_EN | AZX_INT_ALL_STREAM);
}

/**
 * snd_hdac_bus_init_chip - reset and start the controller registers
 * @bus: HD-audio core bus
 * @full_reset: Do full reset
 */
bool snd_hdac_bus_init_chip(struct hdac_bus *bus, bool full_reset)
{
	if (bus->chip_init)
		return false;

	/* reset controller */
	azx_reset(bus, full_reset);

	/* initialize interrupts */
	azx_int_clear(bus);
	azx_int_enable(bus);

	/* initialize the codec command I/O */
	snd_hdac_bus_init_cmd_io(bus);

	/* program the position buffer */
	if (bus->use_posbuf && bus->posbuf.addr) {
		snd_hdac_chip_writel(bus, DPLBASE, (u32)bus->posbuf.addr);
		snd_hdac_chip_writel(bus, DPUBASE, upper_32_bits(bus->posbuf.addr));
	}

	bus->chip_init = true;
	return true;
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_init_chip);

/**
 * snd_hdac_bus_stop_chip - disable the whole IRQ and I/Os
 * @bus: HD-audio core bus
 */
void snd_hdac_bus_stop_chip(struct hdac_bus *bus)
{
	if (!bus->chip_init)
		return;

	/* disable interrupts */
	azx_int_disable(bus);
	azx_int_clear(bus);

	/* disable CORB/RIRB */
	snd_hdac_bus_stop_cmd_io(bus);

	/* disable position buffer */
	if (bus->posbuf.addr) {
		snd_hdac_chip_writel(bus, DPLBASE, 0);
		snd_hdac_chip_writel(bus, DPUBASE, 0);
	}

	bus->chip_init = false;
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_stop_chip);

/**
 * snd_hdac_bus_handle_stream_irq - interrupt handler for streams
 * @bus: HD-audio core bus
 * @status: INTSTS register value
 * @ask: callback to be called for woken streams
 */
void snd_hdac_bus_handle_stream_irq(struct hdac_bus *bus, unsigned int status,
				    void (*ack)(struct hdac_bus *,
						struct hdac_stream *))
{
	struct hdac_stream *azx_dev;
	u8 sd_status;

	list_for_each_entry(azx_dev, &bus->stream_list, list) {
		if (status & azx_dev->sd_int_sta_mask) {
			sd_status = snd_hdac_stream_readb(azx_dev, SD_STS);
			snd_hdac_stream_writeb(azx_dev, SD_STS, SD_INT_MASK);
			if (!azx_dev->substream || !azx_dev->running ||
			    !(sd_status & SD_INT_COMPLETE))
				continue;
			if (ack)
				ack(bus, azx_dev);
		}
	}
}
EXPORT_SYMBOL_GPL(snd_hdac_bus_handle_stream_irq);
