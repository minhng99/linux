// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>

#include "rtd1296-drm.h"

#define CRT_DISPLAY_CLOCK	0x024
#define CRT_HDMI_POWER		0x190
#define CRT_HDMI_DIVIDER		0x194
#define CRT_HDMI_FREQUENCY	0x204
#define CRT_HDMI_UPDATE		0x208
#define CRT_HDMI_STATUS		0x210
#define CRT_HDMI_SSC		0x21c
#define CRT_HDMI_RESET		0x220
#define CRT_HDMI_BIAS		0x230
#define CRT_HDMI_LANES		0x234
#define CRT_HDMI_DRIVE		0x238
#define CRT_HDMI_CURRENT		0x23c
#define CRT_HDMI_DISPLAY_BIAS	0x240

#define HDMI_CHANNEL		0x020
#define HDMI_SYNC_DELAY		0x030
#define HDMI_CONTROL		0x034
#define HDMI_LINK_FORMAT		0x040
#define HDMI_SCRAMBLE_PERIOD	0x044
#define HDMI_GCP			0x078
#define HDMI_PACKET_ENABLE	0x0a4
#define HDMI_SCHEDULER		0x0b8
#define HDMI_AUDIO_CONTROL	0x0bc
#define HDMI_DEEP_COLOR		0x154
#define HDMI_H_TIMING1		0x234
#define HDMI_H_TIMING2		0x238
#define HDMI_H_TIMING3		0x23c
#define HDMI_V_TIMING1		0x240
#define HDMI_V_TIMING4		0x24c
#define HDMI_V_TIMING5		0x250
#define HDMI_VBLANK_ENABLE	0x274
#define HDMI_VBLANK_STATUS	0x278

#define HDMI_CONTROL_ENABLE	BIT(0)
#define HDMI_CONTROL_WRITE	(BIT(5) | BIT(1))
#define HDMI_CONTROL_ENCODER	BIT(4)
#define HDMI_FORMAT_WRITE	(BIT(7) | BIT(5) | BIT(3) | BIT(1))
#define HDMI_FRAME_IRQ		BIT(2)
#define HDMI_FRAME_DONE		BIT(1)

static int rtd1296_hdmi_phy_enable(struct rtd1296_display *display,
				   const struct drm_display_mode *mode)
{
	static const struct reg_sequence calibration[] = {
		{ CRT_HDMI_BIAS, 0x0100000f },
		{ CRT_HDMI_LANES, 0x01540000 },
		{ CRT_HDMI_DRIVE, 0x00a49000 },
		{ CRT_HDMI_CURRENT, 0x0000c092 },
		{ CRT_HDMI_DISPLAY_BIAS, 2 },
	};
	struct regmap *crt = display->crt;
	u32 status, divider;
	int ret;

	switch (mode->clock) {
	case 74250:
		divider = 0x60232;
		break;
	case 148500:
		divider = 0x40221;
		break;
	case 297000:
		divider = 0x20210;
		break;
	default:
		return -EINVAL;
	}
	ret = regmap_update_bits(crt, CRT_HDMI_POWER, GENMASK(8, 0), 0xdb);
	if (ret)
		return ret;
	ret = regmap_write(crt, CRT_DISPLAY_CLOCK, BIT(16) | BIT(15) | BIT(8));
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(crt, calibration, ARRAY_SIZE(calibration));
	if (ret)
		return ret;
	regmap_update_bits(crt, CRT_HDMI_FREQUENCY, GENMASK(18, 0), 19);
	regmap_update_bits(crt, CRT_HDMI_DIVIDER, GENMASK(22, 0), divider);
	regmap_update_bits(crt, CRT_HDMI_SSC, BIT(19), 0);
	regmap_update_bits(crt, CRT_HDMI_RESET, BIT(23), BIT(23));
	regmap_write(crt, CRT_HDMI_UPDATE, 0x90200ffe);
	regmap_update_bits(crt, CRT_HDMI_UPDATE, BIT(0), BIT(0));
	usleep_range(200, 300);
	regmap_update_bits(crt, CRT_HDMI_POWER, GENMASK(8, 0), 0xff);
	ret = regmap_read_poll_timeout(crt, CRT_HDMI_STATUS, status,
				       status & BIT(1), 10, 20000);
	if (ret)
		return ret;
	return regmap_update_bits(crt, CRT_HDMI_LANES, GENMASK(7, 4), GENMASK(7, 4));
}

static void rtd1296_hdmi_timing(struct rtd1296_display *display,
				const struct drm_display_mode *mode)
{
	void __iomem *base = display->hdmi;
	u32 hblank = mode->htotal - mode->hdisplay;
	u32 hfront = mode->hsync_start - mode->hdisplay;
	u32 hsync = mode->hsync_end - mode->hsync_start;
	u32 hback = mode->htotal - mode->hsync_end;
	u32 vblank = mode->vtotal - mode->vdisplay;
	u32 vfront = mode->vsync_start - mode->vdisplay;
	u32 vsync = mode->vsync_end - mode->vsync_start;
	u32 vback = mode->vtotal - mode->vsync_end;
	unsigned int reg;

	writel((hblank - 1) << 16 | (mode->hdisplay - 1), base + HDMI_H_TIMING1);
	writel((hsync - 1) << 16 | (hfront - 1), base + HDMI_H_TIMING2);
	writel(hback - 1, base + HDMI_H_TIMING3);
	writel((mode->vdisplay - 1) << 16 | (mode->vdisplay - 1), base + HDMI_V_TIMING1);
	writel(0, base + 0x244);
	writel(0, base + 0x248);
	writel((vsync * 2) << 16 | (vblank - 1), base + HDMI_V_TIMING4);
	writel((vback - 1) << 16 | (vfront * 2), base + HDMI_V_TIMING5);
	for (reg = 0x254; reg <= 0x270; reg += sizeof(u32))
		writel(0, base + reg);
	writel(BIT(11) | BIT(8) | BIT(5) | BIT(2), base + HDMI_SYNC_DELAY);
	writel(BIT(30) | BIT(28) | BIT(25) | BIT(23) | GENMASK(15, 12) |
	       BIT(8) | BIT(1), base + HDMI_DEEP_COLOR);
	writel(BIT(2), base + HDMI_CHANNEL);
}

int rtd1296_hdmi_enable(struct rtd1296_display *display,
			const struct drm_display_mode *mode)
{
	void __iomem *base = display->hdmi;
	int ret;

	lockdep_assert_held(&display->aux_lock);
	rtd1296_dp_disable(display);
	writel(HDMI_CONTROL_WRITE, base + HDMI_CONTROL);
	writel(BIT(28) | BIT(26), base + HDMI_AUDIO_CONTROL);
	writel(GENMASK(6, 1), base + HDMI_PACKET_ENABLE);
	writel(GENMASK(3, 1), base + HDMI_GCP);
	ret = rtd1296_hdmi_phy_enable(display, mode);
	if (ret) {
		rtd1296_hdmi_disable(display);
		return ret;
	}
	rtd1296_hdmi_timing(display, mode);
	writel(HDMI_FORMAT_WRITE, base + HDMI_LINK_FORMAT);
	writel(BIT(30) | 6 << 16 | BIT(14) | 2, base + HDMI_SCRAMBLE_PERIOD);
	writel(BIT(28) | BIT(26) | BIT(24) | BIT(22) | BIT(20) |
	       BIT(18) | BIT(16) | BIT(14) | BIT(12) | BIT(6) | BIT(3) |
	       BIT(1), base + HDMI_SCHEDULER);
	writel(0, base + HDMI_VBLANK_STATUS);
	writel(HDMI_FRAME_IRQ | BIT(0), base + HDMI_VBLANK_ENABLE);
	writel(HDMI_CONTROL_WRITE | HDMI_CONTROL_ENCODER | HDMI_CONTROL_ENABLE,
	       base + HDMI_CONTROL);
	display->hdmi_powered = true;
	return 0;
}

void rtd1296_hdmi_disable(struct rtd1296_display *display)
{
	lockdep_assert_held(&display->aux_lock);
	display->hdmi_powered = false;
	writel(HDMI_FRAME_IRQ, display->hdmi + HDMI_VBLANK_ENABLE);
	writel(HDMI_CONTROL_WRITE, display->hdmi + HDMI_CONTROL);
	writel(0, display->hdmi + HDMI_CHANNEL);
	regmap_update_bits(display->crt, CRT_HDMI_LANES, GENMASK(7, 4), 0);
	regmap_update_bits(display->crt, CRT_HDMI_POWER, GENMASK(8, 0),
			   BIT(8) | (READ_ONCE(display->aux_powered) ? BIT(3) : 0));
}

bool rtd1296_hdmi_vblank(struct rtd1296_display *display)
{
	u32 status = readl(display->hdmi + HDMI_VBLANK_STATUS);

	if (!(status & HDMI_FRAME_DONE))
		return false;
	writel(0, display->hdmi + HDMI_VBLANK_STATUS);
	return true;
}
