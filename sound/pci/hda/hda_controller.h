/*
 *  Common functionality for the alsa driver code base for HD Audio.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 */

#ifndef __SOUND_HDA_CONTROLLER_H
#define __SOUND_HDA_CONTROLLER_H

#include <linux/timecounter.h>
#include <linux/interrupt.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include "hda_codec.h"
#include <sound/hda_register.h>

#define AZX_MAX_CODECS		HDA_MAX_CODECS
#define AZX_DEFAULT_CODECS	4

/* driver quirks (capabilities) */
/* bits 0-7 are used for indicating driver type */
#define AZX_DCAPS_NO_TCSEL	(1 << 8)	/* No Intel TCSEL bit */
#define AZX_DCAPS_NO_MSI	(1 << 9)	/* No MSI support */
#define AZX_DCAPS_SNOOP_MASK	(3 << 10)	/* snoop type mask */
#define AZX_DCAPS_SNOOP_OFF	(1 << 12)	/* snoop default off */
/* 13 unused */
/* 14 unused */
#define AZX_DCAPS_CTX_WORKAROUND (1 << 15)	/* X-Fi workaround */
#define AZX_DCAPS_POSFIX_LPIB	(1 << 16)	/* Use LPIB as default */
#define AZX_DCAPS_POSFIX_VIA	(1 << 17)	/* Use VIACOMBO as default */
#define AZX_DCAPS_NO_64BIT	(1 << 18)	/* No 64bit address */
#define AZX_DCAPS_SYNC_WRITE	(1 << 19)	/* sync each cmd write */
#define AZX_DCAPS_OLD_SSYNC	(1 << 20)	/* Old SSYNC reg for ICH */
#define AZX_DCAPS_NO_ALIGN_BUFSIZE (1 << 21)	/* no buffer size alignment */
/* 22 unused */
#define AZX_DCAPS_4K_BDLE_BOUNDARY (1 << 23)	/* BDLE in 4k boundary */
#define AZX_DCAPS_REVERSE_ASSIGN (1 << 24)	/* Assign devices in reverse order */
#define AZX_DCAPS_COUNT_LPIB_DELAY  (1 << 25)	/* Take LPIB as delay */
#define AZX_DCAPS_PM_RUNTIME	(1 << 26)	/* runtime PM support */
#define AZX_DCAPS_I915_POWERWELL (1 << 27)	/* HSW i915 powerwell support */
#define AZX_DCAPS_CORBRP_SELF_CLEAR (1 << 28)	/* CORBRP clears itself after reset */
#define AZX_DCAPS_NO_MSI64      (1 << 29)	/* Stick to 32-bit MSIs */
#define AZX_DCAPS_SEPARATE_STREAM_TAG	(1 << 30) /* capture and playback use separate stream tag */

enum {
	AZX_SNOOP_TYPE_NONE,
	AZX_SNOOP_TYPE_SCH,
	AZX_SNOOP_TYPE_ATI,
	AZX_SNOOP_TYPE_NVIDIA,
};

struct azx_dev {
	struct snd_dma_buffer bdl; /* BDL buffer */
	u32 *posbuf;		/* position buffer pointer */

	unsigned int bufsize;	/* size of the play buffer in bytes */
	unsigned int period_bytes; /* size of the period in bytes */
	unsigned int frags;	/* number for period in the play buffer */
	unsigned int fifo_size;	/* FIFO size */
	unsigned long start_wallclk;	/* start + minimum wallclk */
	unsigned long period_wallclk;	/* wallclk for period */

	void __iomem *sd_addr;	/* stream descriptor pointer */

	u32 sd_int_sta_mask;	/* stream int status mask */

	/* pcm support */
	struct snd_pcm_substream *substream;	/* assigned substream,
						 * set in PCM open
						 */
	unsigned int format_val;	/* format value to be set in the
					 * controller and the codec
					 */
	unsigned char stream_tag;	/* assigned stream */
	unsigned char index;		/* stream index */
	int assigned_key;		/* last device# key assigned to */

	unsigned int opened:1;
	unsigned int running:1;
	unsigned int irq_pending:1;
	unsigned int prepared:1;
	unsigned int locked:1;
	/*
	 * For VIA:
	 *  A flag to ensure DMA position is 0
	 *  when link position is not greater than FIFO size
	 */
	unsigned int insufficient:1;
	unsigned int wc_marked:1;
	unsigned int no_period_wakeup:1;

	struct timecounter  azx_tc;
	struct cyclecounter azx_cc;

	int delay_negative_threshold;

#ifdef CONFIG_SND_HDA_DSP_LOADER
	/* Allows dsp load to have sole access to the playback stream. */
	struct mutex dsp_mutex;
#endif
};

/* CORB/RIRB */
struct azx_rb {
	u32 *buf;		/* CORB/RIRB buffer
				 * Each CORB entry is 4byte, RIRB is 8byte
				 */
	dma_addr_t addr;	/* physical address of CORB/RIRB buffer */
	/* for RIRB */
	unsigned short rp, wp;	/* read/write pointers */
	int cmds[AZX_MAX_CODECS];	/* number of pending requests */
	u32 res[AZX_MAX_CODECS];	/* last read value */
};

struct azx;

/* Functions to read/write to hda registers. */
struct hda_controller_ops {
	/* Register Access */
	void (*reg_writel)(u32 value, u32 __iomem *addr);
	u32 (*reg_readl)(u32 __iomem *addr);
	void (*reg_writew)(u16 value, u16 __iomem *addr);
	u16 (*reg_readw)(u16 __iomem *addr);
	void (*reg_writeb)(u8 value, u8 __iomem *addr);
	u8 (*reg_readb)(u8 __iomem *addr);
	/* Disable msi if supported, PCI only */
	int (*disable_msi_reset_irq)(struct azx *);
	/* Allocation ops */
	int (*dma_alloc_pages)(struct azx *chip,
			       int type,
			       size_t size,
			       struct snd_dma_buffer *buf);
	void (*dma_free_pages)(struct azx *chip, struct snd_dma_buffer *buf);
	int (*substream_alloc_pages)(struct azx *chip,
				     struct snd_pcm_substream *substream,
				     size_t size);
	int (*substream_free_pages)(struct azx *chip,
				    struct snd_pcm_substream *substream);
	void (*pcm_mmap_prepare)(struct snd_pcm_substream *substream,
				 struct vm_area_struct *area);
	/* Check if current position is acceptable */
	int (*position_check)(struct azx *chip, struct azx_dev *azx_dev);
};

struct azx_pcm {
	struct azx *chip;
	struct snd_pcm *pcm;
	struct hda_codec *codec;
	struct hda_pcm *info;
	struct list_head list;
};

typedef unsigned int (*azx_get_pos_callback_t)(struct azx *, struct azx_dev *);
typedef int (*azx_get_delay_callback_t)(struct azx *, struct azx_dev *, unsigned int pos);

struct azx {
	struct snd_card *card;
	struct pci_dev *pci;
	int dev_index;

	/* chip type specific */
	int driver_type;
	unsigned int driver_caps;
	int playback_streams;
	int playback_index_offset;
	int capture_streams;
	int capture_index_offset;
	int num_streams;
	const int *jackpoll_ms; /* per-card jack poll interval */

	/* Register interaction. */
	const struct hda_controller_ops *ops;

	/* position adjustment callbacks */
	azx_get_pos_callback_t get_position[2];
	azx_get_delay_callback_t get_delay[2];

	/* pci resources */
	unsigned long addr;
	void __iomem *remap_addr;
	int irq;

	/* locks */
	spinlock_t reg_lock;
	struct mutex open_mutex; /* Prevents concurrent open/close operations */

	/* streams (x num_streams) */
	struct azx_dev *azx_dev;

	/* PCM */
	struct list_head pcm_list; /* azx_pcm list */

	/* HD codec */
	unsigned short codec_mask;
	int  codec_probe_mask; /* copied from probe_mask option */
	struct hda_bus *bus;
	unsigned int beep_mode;

	/* CORB/RIRB */
	struct azx_rb corb;
	struct azx_rb rirb;

	/* CORB/RIRB and position buffers */
	struct snd_dma_buffer rb;
	struct snd_dma_buffer posbuf;

#ifdef CONFIG_SND_HDA_PATCH_LOADER
	const struct firmware *fw;
#endif

	/* flags */
	const int *bdl_pos_adj;
	int poll_count;
	unsigned int running:1;
	unsigned int initialized:1;
	unsigned int single_cmd:1;
	unsigned int polling_mode:1;
	unsigned int msi:1;
	unsigned int probing:1; /* codec probing phase */
	unsigned int snoop:1;
	unsigned int align_buffer_size:1;
	unsigned int region_requested:1;
	unsigned int disabled:1; /* disabled by vga_switcheroo */

	/* for debugging */
	unsigned int last_cmd[AZX_MAX_CODECS];

#ifdef CONFIG_SND_HDA_DSP_LOADER
	struct azx_dev saved_azx_dev;
#endif
};

#ifdef CONFIG_X86
#define azx_snoop(chip)		((chip)->snoop)
#else
#define azx_snoop(chip)		true
#endif

/*
 * macros for easy use
 */

#define azx_writel(chip, reg, value) \
	((chip)->ops->reg_writel(value, (chip)->remap_addr + AZX_REG_##reg))
#define azx_readl(chip, reg) \
	((chip)->ops->reg_readl((chip)->remap_addr + AZX_REG_##reg))
#define azx_writew(chip, reg, value) \
	((chip)->ops->reg_writew(value, (chip)->remap_addr + AZX_REG_##reg))
#define azx_readw(chip, reg) \
	((chip)->ops->reg_readw((chip)->remap_addr + AZX_REG_##reg))
#define azx_writeb(chip, reg, value) \
	((chip)->ops->reg_writeb(value, (chip)->remap_addr + AZX_REG_##reg))
#define azx_readb(chip, reg) \
	((chip)->ops->reg_readb((chip)->remap_addr + AZX_REG_##reg))

#define azx_sd_writel(chip, dev, reg, value) \
	((chip)->ops->reg_writel(value, (dev)->sd_addr + AZX_REG_##reg))
#define azx_sd_readl(chip, dev, reg) \
	((chip)->ops->reg_readl((dev)->sd_addr + AZX_REG_##reg))
#define azx_sd_writew(chip, dev, reg, value) \
	((chip)->ops->reg_writew(value, (dev)->sd_addr + AZX_REG_##reg))
#define azx_sd_readw(chip, dev, reg) \
	((chip)->ops->reg_readw((dev)->sd_addr + AZX_REG_##reg))
#define azx_sd_writeb(chip, dev, reg, value) \
	((chip)->ops->reg_writeb(value, (dev)->sd_addr + AZX_REG_##reg))
#define azx_sd_readb(chip, dev, reg) \
	((chip)->ops->reg_readb((dev)->sd_addr + AZX_REG_##reg))

#define azx_has_pm_runtime(chip) \
	((chip)->driver_caps & AZX_DCAPS_PM_RUNTIME)

/* PCM setup */
static inline struct azx_dev *get_azx_dev(struct snd_pcm_substream *substream)
{
	return substream->runtime->private_data;
}
unsigned int azx_get_position(struct azx *chip, struct azx_dev *azx_dev);
unsigned int azx_get_pos_lpib(struct azx *chip, struct azx_dev *azx_dev);
unsigned int azx_get_pos_posbuf(struct azx *chip, struct azx_dev *azx_dev);

/* Stream control. */
void azx_stream_stop(struct azx *chip, struct azx_dev *azx_dev);

/* Allocation functions. */
int azx_alloc_stream_pages(struct azx *chip);
void azx_free_stream_pages(struct azx *chip);

/* Low level azx interface */
void azx_init_chip(struct azx *chip, bool full_reset);
void azx_stop_chip(struct azx *chip);
void azx_enter_link_reset(struct azx *chip);
irqreturn_t azx_interrupt(int irq, void *dev_id);

/* Codec interface */
int azx_bus_create(struct azx *chip, const char *model);
int azx_probe_codecs(struct azx *chip, unsigned int max_slots);
int azx_codec_configure(struct azx *chip);
int azx_init_stream(struct azx *chip);

#endif /* __SOUND_HDA_CONTROLLER_H */
