// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>

#include "rtd1296-drm.h"

#define CRT_DISPLAY_CLOCK	0x024
#define CRT_TMDS_POWER		0x190
#define CRT_HDMI_BIAS		0x230
#define CRT_DP_PLL		0x248
#define CRT_DP_POWER		0x24c
#define CRT_PIXEL_PLL		0x250
#define CRT_PIXEL_POWER		0x254
#define CRT_DP_SSC_CONTROL	0x5e0
#define CRT_DP_SSC_TARGET		0x5e4
#define CRT_DP_SSC_STEP		0x5e8
#define CRT_DP_SSC_LIMIT		0x5ec
#define CRT_DP_SSC_PERIOD		0x5f0
#define CRT_DP_SSC_STATUS		0x5fc
#define CRT_PIXEL_SSC_CONTROL	0x600
#define CRT_PIXEL_SSC_TARGET	0x604
#define CRT_PIXEL_SSC_STEP		0x608
#define CRT_PIXEL_SSC_STATUS	0x61c

#define AIF_MISC			0x000
#define AIF_LANES			0x040
#define AIF_EMPHASIS		0x044
#define AIF_DRIVE			0x048
#define AIF_ISOLATE		BIT(25)
#define AIF_BIAS			BIT(0)

#define DP_PHY			0x000
#define DP_PATTERN		0x004
#define DP_PHY_CONTROL		0x034
#define DP_MAC			0x280
#define DP_RESET		0x284
#define DP_IRQ_CONTROL		0x28c
#define DP_PIXEL_FIFO		0x290
#define DP_MN_CONTROL		0x2a0
#define DP_M_VALUE		0x2a4
#define DP_N_VALUE		0x2b0
#define DP_MSA_CONTROL		0x2d0
#define DP_MSA_MISC		0x2d4
#define DP_HTOTAL		0x2dc
#define DP_HSTART		0x2e4
#define DP_HWIDTH		0x2ec
#define DP_HSYNC		0x2f4
#define DP_VTOTAL		0x2fc
#define DP_VSTART		0x304
#define DP_VHEIGHT		0x30c
#define DP_VSYNC		0x314
#define DP_VBID			0x31c
#define DP_ARBITER		0x324
#define DP_LINE_BYTES		0x328
#define DP_TU_SIZE		0x330
#define DP_TU_VALID		0x334
#define DP_FIFO_WATERMARK		0x34c
#define DP_SECONDARY_END		0x350
#define DP_SERIAL_FIFO		0x69c
#define DP_AUX_ANALOG1		0x984
#define DP_AUX_ANALOG2		0x988
#define DP_AUX_ANALOG6		0x998
#define DP_AUX_DRIVE		0x99c
#define DP_AUX_POWER		0xa80
#define DP_AUX_TIMEOUT		0xa84
#define DP_AUX_START		0xa8c
#define DP_AUX_COMMAND		0xa90
#define DP_AUX_ADDRESS_MID	0xa94
#define DP_AUX_ADDRESS_LOW	0xa98
#define DP_AUX_LENGTH		0xa9c
#define DP_AUX_WRITE_DATA		0xaa0
#define DP_AUX_REPLY		0xaa4
#define DP_AUX_READ_DATA		0xaa8
#define DP_AUX_FIFO		0xaac
#define DP_AUX_READ_POINTER	0xab4
#define DP_AUX_WRITE_POINTER	0xab8
#define DP_AUX_EVENT		0xac4
#define DP_AUX_INTERRUPT		0xac8
#define DP_AUX_SYNC		0xbd8

#define DP_AUX_DONE		BIT(5)
#define DP_AUX_RX_ERROR		BIT(4)
#define DP_AUX_READ_ERROR		BIT(3)
#define DP_AUX_NACK_EVENT		BIT(2)
#define DP_AUX_DEFER_EVENT		BIT(1)
#define DP_AUX_TIMEOUT_EVENT	BIT(0)
#define DP_AUX_ADDRESS_ONLY	BIT(7)
#define DP_AUX_FIRMWARE_HANDLE	BIT(5)
#define DP_AUX_START_REQUEST	BIT(0)

#define PIF_CONTROL		0x400
#define PIF_HSYNC		0x404
#define PIF_HTOTAL		0x408
#define PIF_HACTIVE		0x40c
#define PIF_VACTIVE		0x410
#define PIF_VTOTAL		0x418
#define PIF_VSYNC		0x41c
#define PIF_VSYNC_ADJUST		0x420
#define PIF_INTERRUPT		0x42c
#define PIF_STATUS		0x430

#define DP_LINK_CLOCK_KHZ	270000
#define DP_TRANSFER_UNIT		64
#define DP_MN_DENOMINATOR		32768

static void rtd1296_dp_update(struct rtd1296_display *display, u32 reg,
			      u32 mask, u32 value)
{
	u32 old = readl(display->dp + reg);

	writel((old & ~mask) | value, display->dp + reg);
}

static int rtd1296_dp_aux_power_on(struct rtd1296_display *display)
{
	struct regmap *crt = display->crt;
	u32 status;
	int ret;

	lockdep_assert_held(&display->aux_lock);
	if (display->hdmi_powered)
		return -EBUSY;
	if (display->aux_powered)
		return 0;
	regmap_write(crt, CRT_DISPLAY_CLOCK, BIT(22) | 2 << 17);
	usleep_range(200, 300);
	writel(BIT(24) | 3 << 12 | 3 << 8 | AIF_BIAS, display->lvds + AIF_MISC);
	regmap_update_bits(crt, CRT_TMDS_POWER, BIT(3), BIT(3));
	regmap_update_bits(crt, CRT_HDMI_BIAS, BIT(6), 0);
	regmap_write(crt, CRT_PIXEL_PLL, 0x934701a2);
	usleep_range(200, 300);
	regmap_update_bits(crt, CRT_PIXEL_PLL, BIT(14), BIT(14));
	regmap_write(crt, CRT_PIXEL_POWER, 0x1e01);
	regmap_write(crt, CRT_PIXEL_SSC_TARGET, 30 << 11);
	regmap_write(crt, CRT_PIXEL_SSC_STEP, 0x501008);
	regmap_write(crt, CRT_PIXEL_SSC_CONTROL, 0xc);
	usleep_range(200, 300);
	regmap_write(crt, CRT_PIXEL_SSC_CONTROL, 0xd);
	regmap_write(crt, CRT_DP_PLL, 0x9a356007);
	regmap_write(crt, CRT_DP_POWER, 3);
	regmap_write(crt, CRT_DP_SSC_CONTROL, 0xc);
	regmap_write(crt, CRT_DP_SSC_TARGET, 97 << 11);
	regmap_write(crt, CRT_DP_SSC_LIMIT, 0x30480);
	regmap_write(crt, CRT_DP_SSC_PERIOD, 0x1100);
	regmap_write(crt, CRT_DP_SSC_STEP, 0x501068);
	regmap_write(crt, CRT_DP_SSC_CONTROL, 0xd);
	ret = regmap_read_poll_timeout(crt, CRT_DP_SSC_STATUS, status,
				       status & BIT(20), 10, 20000);
	if (ret)
		goto power;
	/* Start both PLLs before selecting the pixel clock. */
	ret = regmap_read_poll_timeout(crt, CRT_PIXEL_SSC_STATUS, status,
				       status & BIT(20), 10, 20000);
	if (ret)
		goto power;
	regmap_write(crt, CRT_DISPLAY_CLOCK, BIT(16) | BIT(15));
	regmap_write(crt, CRT_DISPLAY_CLOCK, BIT(22) | BIT(17));
	usleep_range(200, 300);
	rtd1296_dp_update(display, DP_AUX_ANALOG1, GENMASK(7, 5) | GENMASK(3, 0), 0x8f);
	rtd1296_dp_update(display, DP_AUX_ANALOG2, BIT(5) | BIT(0), BIT(5) | BIT(0));
	rtd1296_dp_update(display, DP_AUX_ANALOG6, GENMASK(4, 0), GENMASK(4, 0));
	writel(0xff, display->dp + DP_AUX_DRIVE);
	writel(0x95, display->dp + DP_AUX_TIMEOUT);
	rtd1296_dp_update(display, DP_AUX_POWER, BIT(0), BIT(0));
	rtd1296_dp_update(display, DP_AUX_SYNC, BIT(1), BIT(1));
	writel(0, display->dp + DP_AUX_INTERRUPT);
	writel(0, display->dp + DP_IRQ_CONTROL);
	WRITE_ONCE(display->aux_powered, true);
	return 0;
power:
	writel(AIF_ISOLATE | BIT(24) | 3 << 12 | 3 << 8, display->lvds + AIF_MISC);
	regmap_write(crt, CRT_DP_POWER, 0);
	regmap_update_bits(crt, CRT_DP_PLL, BIT(31), 0);
	regmap_write(crt, CRT_PIXEL_SSC_CONTROL, 8);
	regmap_update_bits(crt, CRT_PIXEL_POWER, BIT(0), 0);
	regmap_update_bits(crt, CRT_PIXEL_PLL, BIT(31), 0);
	return ret;
}

static ssize_t rtd1296_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	struct rtd1296_display *display = container_of(aux, struct rtd1296_display, aux);
	u8 *buffer = msg->buffer;
	u8 request = msg->request & ~DP_AUX_I2C_MOT;
	u32 status, reply;
	unsigned int i, count;
	int ret, hpd;

	if (msg->size > DP_AUX_MAX_PAYLOAD_BYTES)
		return -E2BIG;
	if (request != DP_AUX_NATIVE_WRITE && request != DP_AUX_NATIVE_READ &&
	    request != DP_AUX_I2C_WRITE && request != DP_AUX_I2C_READ &&
	    request != DP_AUX_I2C_WRITE_STATUS_UPDATE)
		return -EINVAL;
	hpd = gpiod_get_value_cansleep(display->outputs[RTD1296_DP].hpd);
	if (hpd <= 0)
		return hpd < 0 ? hpd : -ENXIO;
	ret = pm_runtime_resume_and_get(display->drm.dev);
	if (ret < 0)
		return ret;
	mutex_lock(&display->aux_lock);
	ret = rtd1296_dp_aux_power_on(display);
	if (ret)
		goto unlock;
	writel(3, display->dp + DP_AUX_FIFO);
	writel(GENMASK(5, 0), display->dp + DP_AUX_EVENT);
	writel(msg->request << 4 | (msg->address >> 16 & 0xf), display->dp + DP_AUX_COMMAND);
	writel(msg->address >> 8 & 0xff, display->dp + DP_AUX_ADDRESS_MID);
	writel(msg->address & 0xff, display->dp + DP_AUX_ADDRESS_LOW);
	writel(msg->size ? msg->size - 1 : 0, display->dp + DP_AUX_LENGTH);
	if (!(request & DP_AUX_I2C_READ)) {
		for (i = 0; i < msg->size; i++)
			writel(buffer[i], display->dp + DP_AUX_WRITE_DATA);
	}
	writel(DP_AUX_FIRMWARE_HANDLE | DP_AUX_START_REQUEST |
	       (msg->size ? 0 : DP_AUX_ADDRESS_ONLY),
	       display->dp + DP_AUX_START);
	ret = readl_poll_timeout(display->dp + DP_AUX_EVENT, status,
				 status & GENMASK(5, 0), 10, 2000);
	if (ret || (status & DP_AUX_TIMEOUT_EVENT)) {
		ret = -ETIMEDOUT;
		goto clear;
	}
	if (status & (DP_AUX_RX_ERROR | DP_AUX_READ_ERROR)) {
		ret = -EIO;
		goto clear;
	}
	reply = readl(display->dp + DP_AUX_REPLY);
	msg->reply = FIELD_GET(GENMASK(7, 4), reply);
	if (!msg->reply && (status & DP_AUX_NACK_EVENT))
		msg->reply = DP_AUX_NATIVE_REPLY_NACK;
	if (!msg->reply && (status & DP_AUX_DEFER_EVENT))
		msg->reply = DP_AUX_NATIVE_REPLY_DEFER;
	if (msg->reply) {
		ret = msg->size;
		goto clear;
	}
	if (!(status & DP_AUX_DONE)) {
		ret = -EIO;
		goto clear;
	}
	count = msg->size;
	if ((request & DP_AUX_I2C_READ) && count) {
		u32 read_pointer = readl(display->dp + DP_AUX_READ_POINTER);
		u32 write_pointer = readl(display->dp + DP_AUX_WRITE_POINTER);

		count = min_t(unsigned int, count, (write_pointer - read_pointer) & GENMASK(4, 0));
		for (i = 0; i < count; i++)
			buffer[i] = readl(display->dp + DP_AUX_READ_DATA);
	}
	ret = count;
clear:
	writel(GENMASK(5, 0), display->dp + DP_AUX_EVENT);
	writel(3, display->dp + DP_AUX_FIFO);
unlock:
	mutex_unlock(&display->aux_lock);
	pm_runtime_mark_last_busy(display->drm.dev);
	pm_runtime_put_autosuspend(display->drm.dev);
	return ret;
}

static void rtd1296_dp_write16(struct rtd1296_display *display, u32 reg, u32 value)
{
	writel(value >> 8 & 0xff, display->dp + reg);
	writel(value & 0xff, display->dp + reg + 4);
}

static void rtd1296_dp_pattern(struct rtd1296_display *display, unsigned int pattern)
{
	writel(pattern << 4, display->dp + DP_PATTERN);
	writel(pattern << 4 | BIT(0), display->dp + DP_PATTERN);
}

static void rtd1296_dp_signal(struct rtd1296_display *display, u8 training)
{
	static const u8 swing_code[] = { 3, 5, 9, 14 };
	static const u8 emphasis_code[] = { 0, 7, 12, 9 };
	u32 swing = training & DP_TRAIN_VOLTAGE_SWING_MASK;
	u32 emphasis = (training & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT;
	u32 level = swing_code[swing];
	u32 value;

	if (!swing && emphasis)
		level = emphasis == 3 ? 6 : 5;
	value = readl(display->lvds + AIF_EMPHASIS);
	value &= ~(GENMASK(23, 20) | BIT(16) | GENMASK(3, 0));
	value |= GENMASK(23, 20) | emphasis_code[emphasis] | (emphasis == 3 ? BIT(16) : 0);
	writel(value, display->lvds + AIF_EMPHASIS);
	value = readl(display->lvds + AIF_DRIVE);
	value &= ~(GENMASK(31, 24) | GENMASK(3, 0));
	writel(value | 0x1f << 24 | level, display->lvds + AIF_DRIVE);
}

static int rtd1296_dp_train_phase(struct rtd1296_display *display,
				  unsigned int pattern, u8 *training)
{
	u8 status[DP_LINK_STATUS_SIZE];
	unsigned int attempt, voltage, emphasis;
	int ret;

	rtd1296_dp_pattern(display, pattern);
	ret = drm_dp_dpcd_writeb(&display->aux, DP_TRAINING_PATTERN_SET,
				 pattern | DP_LINK_SCRAMBLING_DISABLE);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	for (attempt = 0; attempt < 10; attempt++) {
		rtd1296_dp_signal(display, *training);
		ret = drm_dp_dpcd_writeb(&display->aux, DP_TRAINING_LANE0_SET, *training);
		if (ret != 1)
			return ret < 0 ? ret : -EIO;
		if (pattern == DP_TRAINING_PATTERN_1)
			drm_dp_link_train_clock_recovery_delay(&display->aux, display->dpcd);
		else
			drm_dp_link_train_channel_eq_delay(&display->aux, display->dpcd);
		ret = drm_dp_dpcd_read_link_status(&display->aux, status);
		if (ret)
			return ret;
		if (pattern == DP_TRAINING_PATTERN_1 && drm_dp_clock_recovery_ok(status, 1))
			return 0;
		if (pattern == DP_TRAINING_PATTERN_2 && drm_dp_channel_eq_ok(status, 1))
			return 0;
		if (pattern == DP_TRAINING_PATTERN_2 && !drm_dp_clock_recovery_ok(status, 1))
			return -EIO;
		voltage = drm_dp_get_adjust_request_voltage(status, 0);
		emphasis = drm_dp_get_adjust_request_pre_emphasis(status, 0) >>
							DP_TRAIN_PRE_EMPHASIS_SHIFT;
		emphasis = min(emphasis, 3 - voltage);
		*training = voltage | emphasis << DP_TRAIN_PRE_EMPHASIS_SHIFT;
		if (voltage == 3)
			*training |= DP_TRAIN_MAX_SWING_REACHED;
		if (emphasis == 3 - voltage)
			*training |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
	}
	return -ETIMEDOUT;
}

static int rtd1296_dp_train(struct rtd1296_display *display)
{
	u8 training = 0, lane_count = 1;
	int ret, stop;

	ret = drm_dp_read_dpcd_caps(&display->aux, display->dpcd);
	if (ret)
		return ret;
	if (drm_dp_max_link_rate(display->dpcd) < DP_LINK_CLOCK_KHZ ||
	    !drm_dp_max_lane_count(display->dpcd))
		return -EINVAL;
	ret = drm_dp_dpcd_writeb(&display->aux, DP_SET_POWER, DP_SET_POWER_D0);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	usleep_range(1000, 2000);
	ret = drm_dp_dpcd_writeb(&display->aux, DP_DOWNSPREAD_CTRL, 0);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = drm_dp_dpcd_writeb(&display->aux, DP_MAIN_LINK_CHANNEL_CODING_SET,
				 DP_SET_ANSI_8B10B);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = drm_dp_dpcd_writeb(&display->aux, DP_LINK_BW_SET, DP_LINK_BW_2_7);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	if (drm_dp_enhanced_frame_cap(display->dpcd))
		lane_count |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	rtd1296_dp_update(display, DP_MAC, BIT(2),
			  lane_count & DP_LANE_COUNT_ENHANCED_FRAME_EN ? BIT(2) : 0);
	ret = drm_dp_dpcd_writeb(&display->aux, DP_LANE_COUNT_SET, lane_count);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = rtd1296_dp_train_phase(display, DP_TRAINING_PATTERN_1, &training);
	if (!ret)
		ret = rtd1296_dp_train_phase(display, DP_TRAINING_PATTERN_2, &training);
	stop = drm_dp_dpcd_writeb(&display->aux, DP_TRAINING_PATTERN_SET,
				  DP_TRAINING_PATTERN_DISABLE);
	if (!ret && stop != 1)
		ret = stop < 0 ? stop : -EIO;
	if (!ret)
		rtd1296_dp_pattern(display, 4);
	return ret;
}

static void rtd1296_dp_timing(struct rtd1296_display *display,
			      const struct drm_display_mode *mode)
{
	void __iomem *pif = display->lvds;
	u32 hsync = mode->hsync_end - mode->hsync_start;
	u32 hstart = mode->htotal - mode->hsync_start;
	u32 vstart = mode->vtotal - mode->vsync_start;
	u32 vsync = mode->vsync_end - mode->vsync_start;
	u32 numerator, valid;

	writel(0, pif + PIF_CONTROL);
	writel(hsync, pif + PIF_HSYNC);
	writel(mode->htotal << 16 | mode->htotal, pif + PIF_HTOTAL);
	writel((hstart - 9) << 16 | (hstart - 9 + mode->hdisplay), pif + PIF_HACTIVE);
	writel((vstart + 1) << 16 | (vstart + 1 + mode->vdisplay), pif + PIF_VACTIVE);
	writel(mode->vtotal, pif + PIF_VTOTAL);
	writel(BIT(16) | (vsync + 1), pif + PIF_VSYNC);
	writel(0, pif + PIF_VSYNC_ADJUST);
	writel(BIT(0), pif + PIF_STATUS);
	writel(BIT(31) | (mode->vtotal - 2), pif + PIF_INTERRUPT);
	writel(BIT(6) | BIT(7), display->dp + DP_RESET);
	writel(0, display->dp + DP_RESET);
	numerator = DIV_ROUND_CLOSEST_ULL((u64)mode->clock * DP_MN_DENOMINATOR,
					  DP_LINK_CLOCK_KHZ);
	writel(numerator >> 16, display->dp + DP_M_VALUE);
	rtd1296_dp_write16(display, DP_M_VALUE + 4, numerator);
	writel(0, display->dp + DP_N_VALUE);
	rtd1296_dp_write16(display, DP_N_VALUE + 4, DP_MN_DENOMINATOR);
	rtd1296_dp_write16(display, DP_HTOTAL, mode->htotal);
	rtd1296_dp_write16(display, DP_HSTART, hstart + 1);
	rtd1296_dp_write16(display, DP_HWIDTH, mode->hdisplay);
	rtd1296_dp_write16(display, DP_HSYNC, hsync);
	rtd1296_dp_write16(display, DP_VTOTAL, mode->vtotal);
	rtd1296_dp_write16(display, DP_VSTART, vstart + 1);
	rtd1296_dp_write16(display, DP_VHEIGHT, mode->vdisplay);
	rtd1296_dp_write16(display, DP_VSYNC, vsync);
	writel(0x20, display->dp + DP_MSA_MISC);
	writel(BIT(7), display->dp + DP_MSA_CONTROL);
	writel(BIT(6), display->dp + DP_MN_CONTROL);
	valid = DIV_ROUND_UP_ULL((u64)DP_TRANSFER_UNIT * mode->clock * 3 * 8,
				 DP_LINK_CLOCK_KHZ);
	writel(DP_TRANSFER_UNIT, display->dp + DP_TU_SIZE);
	writel(valid >> 3, display->dp + DP_TU_VALID);
	writel(valid & 7, display->dp + DP_TU_VALID + 4);
	rtd1296_dp_write16(display, DP_LINE_BYTES, mode->hdisplay * 3);
	writel(0xc0, display->dp + DP_FIFO_WATERMARK);
	writel(0xff, display->dp + DP_PIXEL_FIFO);
	writel(3, display->dp + DP_ARBITER);
	writel(6, display->dp + DP_VBID);
	writel(0, display->dp + DP_SECONDARY_END);
	writel(0x14, display->dp + DP_PHY);
	writel(5, display->dp + DP_MAC);
	writel(1, display->lvds + AIF_LANES);
	writel(0x80, display->dp + DP_SERIAL_FIFO);
	writel(0x15, display->dp + DP_PHY_CONTROL);
}

int rtd1296_dp_enable(struct rtd1296_display *display,
		      const struct drm_display_mode *mode)
{
	int ret;

	mutex_lock(&display->aux_lock);
	ret = rtd1296_dp_aux_power_on(display);
	if (!ret)
		rtd1296_dp_timing(display, mode);
	mutex_unlock(&display->aux_lock);
	if (!ret)
		ret = rtd1296_dp_train(display);
	if (ret) {
		mutex_lock(&display->aux_lock);
		rtd1296_dp_disable(display);
		mutex_unlock(&display->aux_lock);
	}
	return ret;
}

void rtd1296_dp_disable(struct rtd1296_display *display)
{
	lockdep_assert_held(&display->aux_lock);
	if (!display->aux_powered)
		return;
	writel(0, display->lvds + PIF_INTERRUPT);
	WRITE_ONCE(display->aux_powered, false);
	synchronize_irq(display->outputs[RTD1296_DP].irq);
	writel(1, display->dp + DP_PHY);
	writel(0, display->lvds + AIF_LANES);
	writel(0, display->dp + DP_AUX_POWER);
	regmap_write(display->crt, CRT_DISPLAY_CLOCK, BIT(22) | 2 << 17);
	usleep_range(200, 300);
	writel(AIF_ISOLATE | BIT(24) | 3 << 12 | 3 << 8, display->lvds + AIF_MISC);
	regmap_write(display->crt, CRT_DP_POWER, 0);
	regmap_update_bits(display->crt, CRT_DP_PLL, BIT(31), 0);
	regmap_write(display->crt, CRT_PIXEL_SSC_CONTROL, 8);
	regmap_update_bits(display->crt, CRT_PIXEL_POWER, BIT(0), 0);
	regmap_update_bits(display->crt, CRT_PIXEL_PLL, BIT(31), 0);
}

bool rtd1296_dp_vblank(struct rtd1296_display *display)
{
	u32 status;

	if (!READ_ONCE(display->aux_powered))
		return false;
	status = readl(display->lvds + PIF_STATUS);
	if (!(status & BIT(0)))
		return false;
	writel(BIT(0), display->lvds + PIF_STATUS);
	return true;
}

int rtd1296_dp_init(struct rtd1296_display *display)
{
	display->aux.name = "RTD1296 DisplayPort";
	display->aux.dev = display->drm.dev;
	display->aux.drm_dev = &display->drm;
	display->aux.transfer = rtd1296_aux_transfer;
	display->aux.no_zero_sized = true;
	return drm_dp_aux_register(&display->aux);
}

void rtd1296_dp_fini(struct rtd1296_display *display)
{
	drm_dp_aux_unregister(&display->aux);
}
