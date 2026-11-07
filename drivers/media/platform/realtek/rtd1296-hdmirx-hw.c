// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/v4l2-dv-timings.h>

#include "rtd1296-hdmirx.h"

static const unsigned int rx_lane_regs[] = { 0xa18, 0xa3c, 0xa4c };

const struct rtd1296_rx_mode rtd1296_rx_modes[RTD1296_RX_MODES] = {
	{
		.timings = V4L2_DV_BT_CEA_1280X720P60,
		.scale_divisor = 1,
		.cmu_divider = 0,
		.phase_divider = 3,
		.demux = 3,
		.cdr_rate = 2,
		.cdr_gain = 16,
		.cdr_integral = 0,
		.pll_m = 8,
		.pll_n = 2,
		.pll_o = 1,
		.pll_divider = 1,
	}, {
		.timings = V4L2_DV_BT_CEA_1920X1080P60,
		.scale_divisor = 1,
		.cmu_divider = 1,
		.phase_divider = 2,
		.demux = 3,
		.cdr_rate = 2,
		.cdr_gain = 32,
		.cdr_integral = 0,
		.pll_m = 4,
		.pll_n = 2,
		.pll_o = 1,
		.pll_divider = 0,
	}, {
		.timings = V4L2_DV_BT_CEA_3840X2160P30,
		.scale_divisor = 2,
		.cmu_divider = 2,
		.phase_divider = 1,
		.demux = 2,
		.cdr_rate = 1,
		.cdr_gain = 32,
		.cdr_integral = 1,
		.pll_m = 4,
		.pll_n = 4,
		.pll_o = 0,
		.pll_divider = 0,
	},
};

void rtd1296_rx_hw_init(struct rtd1296_rx *rx)
{
	unsigned int i;

	writel(0, rx->dma + RX_DMA_CONTROL);
	writel(0, rx->dma + RX_DMA_FORMAT_CONTROL);
	writel(RX_DMA_IRQS | RX_DMA_FORMAT_ERRORS, rx->dma + RX_DMA_STATUS);
	writel(0, rx->wrapper + RX_WRAP_IRQ_ENABLE);
	writel(BIT(11) | RX_WRAP_ENABLE, rx->wrapper + RX_WRAP_CONTROL);
	writel(120, rx->wrapper + RX_WRAP_THRESHOLD);
	writel(0, rx->mac + RX_INTERRUPT);
	writel(0, rx->mac + RX_WATCHDOG);
	writel(0, rx->mac + RX_AUDIO_OUTPUT);
	rx_update(rx->mac, RX_HDCP, BIT(0), 0);
	rx_update(rx->mac, RX_PHY_BANDGAP, BIT(1), BIT(1));
	rx_update(rx->mac, RX_PHY_TOP, GENMASK(21, 20) | GENMASK(7, 0),
		  BIT(20) | BIT(0));
	rx_update(rx->mac, RX_PHY_IMPEDANCE,
		  GENMASK(28, 27) | GENMASK(18, 4),
		  FIELD_PREP(GENMASK(8, 4), 14) |
		  FIELD_PREP(GENMASK(13, 9), 14) |
		  FIELD_PREP(GENMASK(18, 14), 14));
	rx_update(rx->mac, RX_PHY_CLOCK1, GENMASK(15, 12) | GENMASK(7, 0) |
		  BIT(9), GENMASK(15, 13) | BIT(9) | BIT(2) | BIT(1));
	rx_update(rx->mac, RX_PHY_CLOCK2, BIT(25), BIT(25));
	rx_update(rx->mac, RX_PHY_LDO, BIT(0), 0);
	rx_update(rx->mac, RX_PHY_BIAS, BIT(0), BIT(0));
	rx_update(rx->mac, RX_PHY_TERMINATION, 0x249, 0x249);
	rx_update(rx->mac, RX_PHY_CLOCK_DETECT, GENMASK(2, 0), BIT(2));
	rx_update(rx->mac, RX_PHY_MHL, GENMASK(3, 1), BIT(3) | BIT(2));
	for (i = 0; i < ARRAY_SIZE(rx_lane_regs); i++) {
		rx_update(rx->mac, rx_lane_regs[i], GENMASK(20, 16), BIT(20));
		rx_update(rx->mac, rx_lane_regs[i] + 4, BIT(25) | BIT(0),
			  BIT(25) | BIT(0));
	}
	rx_update(rx->mac, RX_PHY_FIFO, GENMASK(24, 20), GENMASK(24, 22));
	rx_update(rx->mac, RX_TMDS_OUT, RX_TMDS_CLOCKS, RX_TMDS_CLOCKS);
	rx_update(rx->mac, RX_TMDS_INPUT, RX_TMDS_INPUT_ENABLE, 0);
	rx_update(rx->mac, RX_TMDS_DECODE, BIT(0), 0);
	rx_update(rx->mac, RX_TMDS_CLOCK, GENMASK(7, 6) | BIT(9), GENMASK(7, 6));
	writel(0x123, rx->mac + RX_MODE);
	writel(BIT(3), rx->mac + RX_AV_CONTROL);
	rx_update(rx->mac, RX_BCH, BIT(4) | BIT(3), BIT(4) | BIT(3));
	rx_update(rx->mac, RX_VIDEO, BIT(16) | BIT(12), BIT(16) | BIT(12));
	rx_update(rx->mac, RX_PHY_MEASURE, RX_PHY_MEASURE_START, 0);
	rx_update(rx->mac, RX_PHY_MEASURE,
		  RX_PHY_MEASURE_START | RX_PHY_MEASURE_AUTO | GENMASK(17, 16),
		  RX_PHY_MEASURE_START | RX_PHY_MEASURE_AUTO);
	writel(RX_PHY_CLOCK_VALID, rx->mac + RX_PHY_CLOCK_RESULT);
}

void rtd1296_rx_hw_stop(struct rtd1296_rx *rx)
{
	writel(0, rx->dma + RX_DMA_CONTROL);
	writel(0, rx->wrapper + RX_WRAP_CONTROL);
	writel(0, rx->wrapper + RX_WRAP_IRQ_ENABLE);
	rx_update(rx->ddc, RX_DDC_CONTROL, RX_DDC_ENABLE, 0);
	writel(0, rx->mac + RX_VIDEO_PLL_POWER);
	rx_update(rx->mac, RX_TMDS_OUT, RX_TMDS_CLOCKS, 0);
	rx_update(rx->mac, RX_PHY_TERMINATION, 0x249, 0);
	rx_update(rx->mac, RX_PHY_ENABLE, BIT(3), 0);
	rx_update(rx->mac, RX_PHY_CLOCK1, BIT(2) | BIT(1), 0);
	rx_update(rx->mac, RX_PHY_CLOCK2, BIT(25), 0);
	rx_update(rx->mac, RX_PHY_LDO, BIT(0), BIT(0));
	rx_update(rx->mac, RX_PHY_BIAS, BIT(0), 0);
	rx_update(rx->mac, RX_PHY_BANDGAP, BIT(1), 0);
}

int rtd1296_rx_edid_write(struct rtd1296_rx *rx)
{
	unsigned int i;

	rx_update(rx->ddc, RX_DDC_CONTROL, RX_DDC_ENABLE, 0);
	for (i = 0; i < rx->edid_blocks * 128; i++) {
		writel(i, rx->ddc + RX_DDC_ADDRESS);
		writel(rx->edid[i], rx->ddc + RX_DDC_DATA);
	}
	for (i = 0; i < rx->edid_blocks * 128; i++) {
		writel(i, rx->ddc + RX_DDC_ADDRESS);
		if ((readl(rx->ddc + RX_DDC_DATA) & 0xff) != rx->edid[i])
			return -EIO;
	}
	writel(0, rx->ddc + RX_DDC_FILTER);
	rx_update(rx->ddc, RX_DDC_CONTROL, GENMASK(4, 2) | RX_DDC_ENABLE,
		  rx->edid_blocks ? RX_DDC_ENABLE | BIT(2) : 0);
	return 0;
}

static void rtd1296_rx_equalizer(struct rtd1296_rx *rx,
				 const struct rtd1296_rx_mode *mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rx_lane_regs); i++) {
		u32 lane = rx_lane_regs[i];

		rx_update(rx->mac, lane, BIT(26) | GENMASK(9, 8), BIT(26));
		rx_update(rx->mac, lane + 4,
			  GENMASK(31, 26) | BIT(24) | GENMASK(23, 11) |
			  GENMASK(4, 3),
			  FIELD_PREP(GENMASK(31, 29), mode->demux) |
			  FIELD_PREP(GENMASK(28, 26), mode->phase_divider) |
			  GENMASK(23, 22) | BIT(16) | BIT(3));
		rx_update(rx->mac, lane + 8,
			  BIT(16) | BIT(15) | GENMASK(13, 3),
			  BIT(16) | BIT(13) | BIT(10) | BIT(7) | BIT(4) | BIT(3));
	}

	rx_update(rx->mac, RX_DFE_MODE, GENMASK(31, 0),
		  FIELD_PREP(GENMASK(31, 30), 2) | BIT(28) | BIT(24) |
		  BIT(21) | BIT(18) | BIT(16) | GENMASK(15, 8) | BIT(2) | BIT(1));
	for (i = 0; i < 3; i++) {
		writel(BIT(31) | BIT(30) | BIT(29) | BIT(24) |
		       FIELD_PREP(GENMASK(15, 11), 24) |
		       FIELD_PREP(GENMASK(10, 6), 3), rx->mac + RX_DFE_LANE(i));
		rx_update(rx->mac, RX_DFE_INITIAL(i), GENMASK(19, 0),
			  FIELD_PREP(GENMASK(13, 9), 12) |
			  FIELD_PREP(GENMASK(8, 4), 15) | 3);
		rx_update(rx->mac, RX_DFE_LOAD(i), GENMASK(31, 18),
			  GENMASK(31, 24) | BIT(23) | BIT(21) | BIT(19));
		rx_update(rx->mac, RX_DFE_LOAD(i), GENMASK(31, 24), 0);
	}
	rx_update(rx->mac, RX_DFE_MODE, GENMASK(7, 4), GENMASK(7, 4));
	rx_update(rx->mac, RX_DFE_GAIN, GENMASK(2, 0), 2);
	rx_update(rx->mac, RX_DFE_LIMIT0, GENMASK(31, 11),
		  FIELD_PREP(GENMASK(31, 27), 24) |
		  FIELD_PREP(GENMASK(26, 21), 53) |
		  FIELD_PREP(GENMASK(20, 16), 10) |
		  FIELD_PREP(GENMASK(15, 11), 25));
	rx_update(rx->mac, RX_DFE_LIMIT1, GENMASK(21, 5),
		  FIELD_PREP(GENMASK(18, 15), 7) | BIT(7) | BIT(5));
	for (i = 0; i < 3; i++)
		rx_update(rx->mac, RX_DFE_LANE(i), GENMASK(23, 16),
			  BIT(22) | BIT(17) | BIT(16));
	msleep(150);
}

static void rtd1296_rx_phy_enable(struct rtd1296_rx *rx,
				  const struct rtd1296_rx_mode *mode)
{
	rx_update(rx->mac, RX_PIXEL_CONTROL, RX_PIXEL_ENABLE, 0);
	rx_update(rx->mac, RX_CHANNEL_FIFO, RX_CHANNEL_ENABLE, 0);
	rx_update(rx->mac, RX_TMDS_DECODE, BIT(0), 0);
	rx_update(rx->mac, RX_PHY_CDR, GENMASK(31, 26), 0);
	rx_update(rx->mac, RX_PHY_RESET, BIT(18) | BIT(12) | BIT(9) |
		  GENMASK(2, 0), 0);
	rx_update(rx->mac, RX_PHY_ENABLE, BIT(3), 0);

	rx_update(rx->mac, RX_PHY_CMU, GENMASK(8, 0), 36);
	rx_update(rx->mac, RX_PHY_CLOCK1, GENMASK(23, 18),
		  FIELD_PREP(GENMASK(23, 18), mode->cmu_divider));
	rx_update(rx->mac, RX_PHY_CLOCK2,
		  GENMASK(31, 26) | BIT(25) | BIT(19) | BIT(13) |
		  GENMASK(10, 8) | GENMASK(7, 4) | GENMASK(2, 0),
		  GENMASK(31, 29) | BIT(25) | BIT(19) | BIT(13) | BIT(10) |
		  FIELD_PREP(GENMASK(7, 4), 7) | 2);
	rx_update(rx->mac, RX_PHY_CDR, GENMASK(19, 17) | GENMASK(15, 4),
		  FIELD_PREP(GENMASK(19, 17), mode->cdr_rate) |
		  FIELD_PREP(GENMASK(15, 8), mode->cdr_gain) |
		  FIELD_PREP(GENMASK(7, 5), mode->cdr_integral) | BIT(4));
	rx_update(rx->mac, RX_PHY_CLOCK3, BIT(5) | BIT(2), BIT(5) | BIT(2));
	rx_update(rx->mac, RX_PHY_ENABLE, BIT(3), BIT(3));
	rx_update(rx->mac, RX_PHY_RESET, BIT(18) | BIT(12) | BIT(9) |
		  GENMASK(2, 0), BIT(18) | BIT(12) | BIT(9) | GENMASK(2, 0));
	rx_update(rx->mac, RX_PHY_CDR, GENMASK(31, 26), GENMASK(31, 26));
	writel(0xe4e84505, rx->mac + RX_PHY_WATCHDOG);
	rx_update(rx->mac, RX_PHY_CMU, BIT(31), BIT(31));
	rx_update(rx->mac, RX_PHY_ADAPT,
		  GENMASK(31, 30) | GENMASK(27, 20) | GENMASK(18, 1),
		  BIT(30) | BIT(25) | BIT(20) | BIT(18) |
		  BIT(16) | BIT(15) | BIT(9) | BIT(7) | BIT(1));
	rx_update(rx->mac, RX_PHY_CP_TIME, GENMASK(9, 0), BIT(7) | BIT(1));
	rx_update(rx->mac, RX_PHY_TIMER, GENMASK(9, 4), 7 << 4);
	rx_update(rx->mac, RX_PHY_SLOPE, BIT(23) | GENMASK(21, 16) | BIT(15),
		  BIT(23) | BIT(20) | BIT(15));
	rx_update(rx->mac, RX_PHY_FIFO, GENMASK(5, 0), GENMASK(5, 0));
	rtd1296_rx_equalizer(rx, mode);

	rx_update(rx->mac, RX_TMDS_DECODE, BIT(0), BIT(0));
	usleep_range(1000, 1500);
	rx_update(rx->mac, RX_TMDS_INPUT, RX_TMDS_INPUT_ENABLE,
		  RX_TMDS_INPUT_ENABLE);
	rx_update(rx->mac, RX_CHANNEL_FIFO, RX_CHANNEL_ENABLE, RX_CHANNEL_ENABLE);
	rx_update(rx->mac, RX_PIXEL_CONTROL, RX_PIXEL_ENABLE, RX_PIXEL_ENABLE);

	/* Keep the video PLL VCO at 297 MHz. */
	writel(BIT(2), rx->mac + RX_VIDEO_PLL_POWER);
	writel(FIELD_PREP(GENMASK(29, 22), mode->pll_m - 2) |
	       FIELD_PREP(BIT(14), mode->pll_o) |
	       FIELD_PREP(GENMASK(8, 6), mode->pll_n - 2) |
	       FIELD_PREP(GENMASK(5, 3), 3) | 3,
	       rx->mac + RX_VIDEO_PLL);
	writel(mode->pll_divider, rx->mac + RX_VIDEO_PLL_DIV);
	rx_update(rx->mac, RX_VIDEO_PLL_LOAD, BIT(4), BIT(4));
	rx_update(rx->mac, RX_VIDEO_PLL_POWER, BIT(12), BIT(12));
	usleep_range(5, 10);
	rx_update(rx->mac, RX_VIDEO_PLL_POWER, BIT(3) | BIT(0), BIT(3) | BIT(0));
	usleep_range(5, 10);
	rx_update(rx->mac, RX_VIDEO_PLL_POWER, BIT(1), BIT(1));
	usleep_range(10, 20);
	rx_update(rx->mac, RX_VIDEO_PLL_POWER, BIT(2), 0);
}

int rtd1296_rx_detect(struct rtd1296_rx *rx,
		      const struct rtd1296_rx_mode **mode, bool monitor)
{
	const struct rtd1296_rx_mode *signal = NULL;
	const struct v4l2_bt_timings *bt;
	u32 result = readl(rx->mac + RX_PHY_CLOCK_RESULT);
	unsigned int count, expected, width, height, i;
	u32 video, value;
	u64 clock, total, frame_us;
	ktime_t start;
	s64 period;
	int ret;

	if (!(result & RX_PHY_CLOCK_VALID))
		return -ENOLINK;
	count = FIELD_GET(RX_PHY_CLOCK_COUNT, result);
	writel(RX_PHY_CLOCK_VALID, rx->mac + RX_PHY_CLOCK_RESULT);
	for (i = 0; i < ARRAY_SIZE(rtd1296_rx_modes); i++) {
		clock = rtd1296_rx_modes[i].timings.bt.pixelclock;
		expected = DIV_ROUND_CLOSEST_ULL(clock * 256, 27000000);
		if (abs((int)count - (int)expected) <= 3) {
			signal = &rtd1296_rx_modes[i];
			break;
		}
	}
	if (!signal)
		return -ERANGE;
	if (rx->phy_mode != signal) {
		if (rx->locked || READ_ONCE(rx->dma_running))
			return -ENOLINK;
		rtd1296_rx_phy_enable(rx, signal);
		rx->phy_mode = signal;
		rx->stable_count = 0;
		return -EAGAIN;
	}
	bt = &signal->timings.bt;
	video = readl(rx->mac + RX_VIDEO);
	width = readl(rx->wrapper + RX_WRAP_WIDTH) & GENMASK(12, 0);
	height = readl(rx->wrapper + RX_WRAP_HEIGHT) & GENMASK(11, 0);
	if (width != bt->width || height != bt->height)
		return -ERANGE;
	if ((video & (RX_VIDEO_COLOR | RX_VIDEO_REPEAT | RX_VIDEO_INTERLACE)) ||
	    (readl(rx->mac + RX_DEEP_COLOR) & 0xf) > 4)
		return -ERANGE;
	if (readl(rx->mac + RX_STATUS) & RX_AVMUTE)
		return -ENOLINK;
	if (rx->locked) {
		/* A new frame must arrive between periodic signal checks. */
		if (monitor) {
			if (!(readl(rx->mac + RX_TMDS_CTRL) & RX_TMDS_VSYNC))
				return -ENOLINK;
			rx_update(rx->mac, RX_TMDS_CTRL, RX_TMDS_VSYNC, RX_TMDS_VSYNC);
		}
		*mode = signal;
		return 0;
	}
	if (++rx->stable_count < 3)
		return -EAGAIN;
	rx_update(rx->mac, RX_TMDS_CTRL, RX_TMDS_VSYNC, RX_TMDS_VSYNC);
	ret = readl_poll_timeout(rx->mac + RX_TMDS_CTRL, value,
				 value & RX_TMDS_VSYNC, 250, 50000);
	if (ret)
		return -ENOLINK;
	start = ktime_get();
	rx_update(rx->mac, RX_TMDS_CTRL, RX_TMDS_VSYNC, RX_TMDS_VSYNC);
	ret = readl_poll_timeout(rx->mac + RX_TMDS_CTRL, value,
				 value & RX_TMDS_VSYNC, 250, 50000);
	if (ret)
		return -ENOLINK;
	period = ktime_us_delta(ktime_get(), start);
	total = (u64)(bt->width + bt->hfrontporch + bt->hsync + bt->hbackporch) *
		(bt->height + bt->vfrontporch + bt->vsync + bt->vbackporch);
	frame_us = div64_u64(total * 1000000, bt->pixelclock);
	if (period < frame_us * 9 / 10 || period > frame_us * 11 / 10)
		return -ERANGE;
	rx_update(rx->mac, RX_TMDS_CTRL, RX_TMDS_VSYNC, RX_TMDS_VSYNC);
	*mode = signal;
	return 0;
}
