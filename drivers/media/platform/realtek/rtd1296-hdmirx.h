/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTD1296_HDMIRX_H
#define RTD1296_HDMIRX_H

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/reset.h>
#include <linux/workqueue.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#define RX_TMDS_CTRL		0x014
#define RX_TMDS_VSYNC		BIT(3)
#define RX_TMDS_OUT		0x018
#define RX_TMDS_CLOCKS		GENMASK(8, 5)
#define RX_TMDS_INPUT		0x01c
#define RX_TMDS_INPUT_ENABLE	GENMASK(3, 0)
#define RX_TMDS_DECODE		0x020
#define RX_TMDS_CLOCK		0x024
#define RX_DEEP_COLOR		0x040
#define RX_PIXEL_CONTROL	0x048
#define RX_PIXEL_ENABLE		BIT(7)
#define RX_STATUS		0x080
#define RX_AVMUTE		BIT(6)
#define RX_MODE		0x090
#define RX_BCH			0x094
#define RX_AV_CONTROL		0x098
#define RX_WATCHDOG		0x09c
#define RX_VIDEO		0x0bc
#define RX_VIDEO_INTERLACE	BIT(6)
#define RX_VIDEO_COLOR		GENMASK(19, 18)
#define RX_VIDEO_REPEAT		GENMASK(11, 8)
#define RX_INTERRUPT		0x0d4
#define RX_AUDIO_OUTPUT		0x0dc
#define RX_CHANNEL_FIFO		0x0f8
#define RX_CHANNEL_ENABLE	GENMASK(24, 22)
#define RX_PHY_FIFO		0x1a0
#define RX_HDCP			0x204
#define RX_VIDEO_PLL		0x400
#define RX_VIDEO_PLL_POWER	0x404
#define RX_VIDEO_PLL_DIV		0x414
#define RX_VIDEO_PLL_LOAD	0x410

#define RX_PHY_BANDGAP		0xa00
#define RX_PHY_CLOCK_DETECT	0xa04
#define RX_PHY_LDO		0xa08
#define RX_PHY_MHL		0xa0c
#define RX_PHY_CLOCK1		0xa28
#define RX_PHY_CLOCK2		0xa2c
#define RX_PHY_CLOCK3		0xa30
#define RX_PHY_ENABLE		0xa38
#define RX_PHY_TOP		0xaf0
#define RX_PHY_IMPEDANCE		0xaf8
#define RX_PHY_TERMINATION	0xafc
#define RX_PHY_BIAS		0xb00
#define RX_PHY_RESET		0xb0c
#define RX_PHY_CDR		0xb1c
#define RX_PHY_WATCHDOG		0xb58
#define RX_PHY_CMU		0xb5c
#define RX_PHY_ADAPT		0xb70
#define RX_PHY_CP_TIME		0xb7c
#define RX_PHY_TIMER		0xb80
#define RX_PHY_SLOPE		0xb84
#define RX_PHY_MEASURE		0xbc4
#define RX_PHY_MEASURE_START	BIT(31)
#define RX_PHY_MEASURE_AUTO	BIT(15)
#define RX_PHY_CLOCK_RESULT	0xbc8
#define RX_PHY_CLOCK_VALID	BIT(9)
#define RX_PHY_CLOCK_COUNT	GENMASK(31, 19)

#define RX_DFE_LANE(n)		(0xf00 + (n) * 0x10)
#define RX_DFE_INITIAL(n)	(0xf04 + (n) * 0x10)
#define RX_DFE_LOAD(n)		(0xf08 + (n) * 0x10)
#define RX_DFE_MODE		0xf30
#define RX_DFE_GAIN		0xf34
#define RX_DFE_LIMIT0		0xf38
#define RX_DFE_LIMIT1		0xf3c

#define RX_WRAP_WIDTH		0x00
#define RX_WRAP_THRESHOLD	0x08
#define RX_WRAP_SIZE		0x0c
#define RX_WRAP_HEIGHT		0x14
#define RX_WRAP_IRQ_ENABLE	0x18
#define RX_WRAP_IRQ_STATUS	0x1c
#define RX_WRAP_DIMENSION_ERROR	(BIT(2) | BIT(1))
#define RX_WRAP_CONTROL		0x20
#define RX_WRAP_DMA		BIT(10)
#define RX_WRAP_ENABLE		(BIT(12) | BIT(0))

#define RX_DDC_FILTER		0x00
#define RX_DDC_CONTROL		0x0c
#define RX_DDC_ENABLE		BIT(0)
#define RX_DDC_ADDRESS		0x20
#define RX_DDC_DATA		0x24

#define RX_DMA_CONTROL		0x00
#define RX_DMA_ADDRESS(n)	(0x04 + (n) * 4)
#define RX_DMA_SIZE		0x10
#define RX_DMA_SOURCE_SIZE	0x18
#define RX_DMA_STATUS		0x28
#define RX_DMA_TYPE		0x2c
#define RX_DMA_H_INITIAL		0x60
#define RX_DMA_H_STEP		0x64
#define RX_DMA_V_INITIAL		0x68
#define RX_DMA_V_STEP		0x6c
#define RX_DMA_H_LUMA(n)		(0x70 + (n) * 4)
#define RX_DMA_H_CHROMA(n)	(0x90 + (n) * 4)
#define RX_DMA_SOURCE_HEIGHT	0xfc
#define RX_DMA_FORMAT_CONTROL	0xec
#define RX_DMA_FORMAT_CHECK	BIT(2)
#define RX_DMA_FORMAT_IRQ	BIT(1)
#define RX_DMA_FORMAT_ERRORS	GENMASK(7, 6)
#define RX_DMA_UV_ADDRESS(n)	(0xe0 + (n) * 4)
#define RX_DMA_ENABLE		BIT(0)
#define RX_DMA_DONE(n)		BIT((n) + 1)
#define RX_DMA_ERRORS		GENMASK(5, 4)
#define RX_DMA_IRQS		GENMASK(5, 1)
#define RX_DMA_RGB		(FIELD_PREP(GENMASK(31, 27), 0x10) | BIT(24))
#define RX_DMA_SCALER_OUTPUT	GENMASK(30, 20)
#define RX_DMA_SCALER_UNITY	BIT(14)
#define RX_DMA_H_CENTER		BIT(28)
#define RX_DMA_HSCALE		BIT(18)
#define RX_DMA_VSCALE		BIT(22)
#define RX_DMA_VNEAREST		BIT(21)

#define RX_CBUS_POWER		0x08
#define RX_CBUS_STATUS		0x14
#define RX_CBUS_5V_PRESENT	BIT(21)
#define RX_CBUS_PLL_POWER	(BIT(20) | BIT(0))

#define RTD1296_RX_EDID_BLOCKS	2
#define RTD1296_RX_CLOCKS	7
#define RTD1296_RX_VIDEO_CLOCKS	3
#define RTD1296_RX_DETECT_CLOCKS	4
#define RTD1296_RX_RESETS	6
#define RTD1296_RX_VIDEO_RESETS	3
#define RTD1296_RX_DETECT_RESETS	3
#define RTD1296_RX_MODES		3

struct rtd1296_rx_mode {
	struct v4l2_dv_timings timings;
	u8 scale_divisor;
	u8 cmu_divider;
	u8 phase_divider;
	u8 demux;
	u8 cdr_rate;
	u8 cdr_gain;
	u8 cdr_integral;
	u8 pll_m;
	u8 pll_n;
	u8 pll_o;
	u8 pll_divider;
};

extern const struct rtd1296_rx_mode rtd1296_rx_modes[RTD1296_RX_MODES];

struct rtd1296_rx_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct rtd1296_rx {
	struct device *dev;
	void __iomem *mac;
	void __iomem *wrapper;
	void __iomem *ddc;
	void __iomem *dma;
	void __iomem *cbus;
	struct clk_bulk_data *clocks;
	struct reset_control_bulk_data *resets;
	struct reset_control *domain_reset;
	struct gpio_desc *hpd;
	struct mutex lock; /* Serializes controls, format, power and queue operations. */
	spinlock_t queue_lock; /* Protects DMA slots, running state and pending buffers. */
	struct delayed_work detect_work;
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *power_control;
	struct vb2_queue queue;
	struct list_head pending;
	struct rtd1296_rx_buffer *slot[2];
	void *scratch;
	dma_addr_t scratch_dma;
	unsigned int sequence;
	int irq;
	u8 edid[128 * RTD1296_RX_EDID_BLOCKS];
	unsigned int edid_blocks;
	const struct rtd1296_rx_mode *mode;
	const struct rtd1296_rx_mode *signal_mode;
	const struct rtd1296_rx_mode *phy_mode;
	u32 width;
	u32 height;
	u32 stride;
	u32 sizeimage;
	unsigned long last_frame;
	unsigned int stable_count;
	bool detector_on;
	bool powered;
	bool cable;
	bool locked;
	bool streaming;
	bool dma_running;
	bool suspended;
	bool removing;
};

static inline void rx_update(void __iomem *base, unsigned int reg,
			     u32 mask, u32 value)
{
	writel((readl(base + reg) & ~mask) | value, base + reg);
}

void rtd1296_rx_hw_init(struct rtd1296_rx *rx);
void rtd1296_rx_hw_stop(struct rtd1296_rx *rx);
int rtd1296_rx_edid_write(struct rtd1296_rx *rx);
int rtd1296_rx_detect(struct rtd1296_rx *rx,
		      const struct rtd1296_rx_mode **mode, bool monitor);

#endif
