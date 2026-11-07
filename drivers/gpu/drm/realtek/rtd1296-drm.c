// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "rtd1296-drm.h"

#define VO_MODE			0x000
#define VO_OUT			0x004
#define VO_MIX			0x008
#define VO_FRAME_CONTROL	0x020
#define VO_OSD			0xa24
#define VO_OSD_HEADER_ENDIAN	0xa2c
#define VO_OSD_HEADER		0xa30
#define VO_OSD_HEADER_3D		0xa34
#define VO_OSD_SIZE		0xa38
#define VO_OSD_CSC		0xa40
#define VO_MIXER_SIZE		0xd78
#define VO_PORT_L_CSC		0xdfc
#define VO_PORT_H_CSC		0xe44
#define VO_SYNC			0xe78
#define VO_INTERRUPT_ENABLE	0xe80
#define VO_INTERRUPT_STATUS	0xe84

#define VO_WRITE_SET		BIT(0)
#define VO_MIXER1_ENABLE		BIT(1)
#define VO_OSD_ENABLE		BIT(16)
#define VO_OSD_RGB		BIT(25)
#define VO_MIXER1_DONE		BIT(1)
#define VO_OSD_HEADER_BYTES	64
#define VO_OSD_HEADER_LAST	BIT(31)
#define VO_OSD_HEADER_XRGB_ORDER	(5U << 28)
#define VO_PORT_H_CSC_UNITY	BIT(15)

static const u32 rtd1296_formats[] = { DRM_FORMAT_XRGB8888 };

DEFINE_DRM_GEM_DMA_FOPS(rtd1296_drm_fops);

static void rtd1296_vo_update(struct rtd1296_display *display, u32 reg,
			      u32 mask, bool set)
{
	writel(mask | (set ? VO_WRITE_SET : 0), display->vo + reg);
}

static void rtd1296_csc_identity(struct rtd1296_display *display, u32 reg)
{
	static const u32 identity[] = { 0x800, 0, 0x800, 0, 0x800, 0, 0 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(identity); i++)
		writel(identity[i], display->vo + reg + i * sizeof(u32));
}

static enum drm_connector_status rtd1296_detect(struct drm_connector *connector,
						bool force)
{
	struct rtd1296_output *output = to_rtd1296_output(connector);
	int value = gpiod_get_value_cansleep(output->hpd);

	if (value < 0)
		return connector_status_unknown;
	return value ? connector_status_connected : connector_status_disconnected;
}

static int rtd1296_get_modes(struct drm_connector *connector)
{
	struct rtd1296_output *output = to_rtd1296_output(connector);
	struct rtd1296_display *display = output->display;
	const struct drm_edid *edid;
	struct i2c_adapter *ddc;
	int count;

	if (rtd1296_detect(connector, false) != connector_status_connected)
		return 0;
	ddc = output->port == RTD1296_HDMI ? display->ddc : &display->aux.ddc;
	edid = drm_edid_read_ddc(connector, ddc);
	drm_edid_connector_update(connector, edid);
	count = drm_edid_connector_add_modes(connector);
	drm_edid_free(edid);
	return count;
}

static enum drm_mode_status rtd1296_mode_valid(struct drm_connector *connector,
					       const struct drm_display_mode *mode)
{
	struct rtd1296_output *output = to_rtd1296_output(connector);
	u8 vic = drm_match_cea_mode(mode);

	if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN |
			   DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_CLKDIV2 |
			   DRM_MODE_FLAG_3D_MASK))
		return MODE_NO_INTERLACE;
	if (output->port == RTD1296_DP && vic != 4)
		return MODE_BAD;
	switch (vic) {
	case 4:
		return mode->clock == 74250 ? MODE_OK : MODE_CLOCK_RANGE;
	case 16:
		return mode->clock == 148500 ? MODE_OK : MODE_CLOCK_RANGE;
	case 95:
		return mode->clock == 297000 ? MODE_OK : MODE_CLOCK_RANGE;
	default:
		return MODE_BAD;
	}
}

static const struct drm_connector_helper_funcs rtd1296_connector_helpers = {
	.get_modes = rtd1296_get_modes,
	.mode_valid = rtd1296_mode_valid,
};

static const struct drm_connector_funcs rtd1296_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = rtd1296_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void rtd1296_hotplug_work(struct work_struct *work)
{
	struct rtd1296_display *display = container_of(work,
				struct rtd1296_display, hotplug_work);
	unsigned int i;

	if (!READ_ONCE(display->registered))
		return;
	for (i = 0; i < RTD1296_NUM_PORTS; i++) {
		struct rtd1296_output *output = &display->outputs[i];
		u64 status = READ_ONCE(output->link_failed) ? DRM_MODE_LINK_STATUS_BAD :
			     DRM_MODE_LINK_STATUS_GOOD;

		drm_connector_set_link_status_property(&output->connector, status);
	}
	drm_kms_helper_hotplug_event(&display->drm);
}

static irqreturn_t rtd1296_hpd_irq(int irq, void *data)
{
	struct rtd1296_display *display = data;

	if (READ_ONCE(display->registered))
		schedule_work(&display->hotplug_work);
	return IRQ_HANDLED;
}

static irqreturn_t rtd1296_timing_irq(int irq, void *data)
{
	struct rtd1296_output *output = data;
	struct rtd1296_display *display = output->display;
	bool pending;

	if (!READ_ONCE(display->powered))
		return IRQ_NONE;
	pending = output->port == RTD1296_HDMI ? rtd1296_hdmi_vblank(display) :
					       rtd1296_dp_vblank(display);
	if (!pending)
		return IRQ_NONE;
	if (READ_ONCE(display->active_port) != output->port ||
	    !READ_ONCE(display->scanout_ready))
		return IRQ_HANDLED;
	rtd1296_vo_update(display, VO_INTERRUPT_STATUS, VO_MIXER1_DONE, false);
	rtd1296_vo_update(display, VO_FRAME_CONTROL, VO_MIXER1_ENABLE, true);
	if (READ_ONCE(display->vblank_enabled))
		drm_crtc_handle_vblank(&display->crtc);
	return IRQ_HANDLED;
}

static int rtd1296_plane_check(struct drm_plane *plane,
			       struct drm_atomic_commit *commit)
{
	struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!state->crtc)
		return 0;
	crtc_state = drm_atomic_get_crtc_state(commit, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !state->visible)
		return ret;
	if (state->fb->pitches[0] & 15)
		return -EINVAL;
	if (state->src_x || state->src_y || state->crtc_x || state->crtc_y)
		return -EINVAL;
	return 0;
}

static void rtd1296_plane_update(struct drm_plane *plane,
				 struct drm_atomic_commit *commit)
{
	struct rtd1296_display *display = to_rtd1296_display(plane->dev);
	struct drm_plane_state *state = drm_atomic_get_new_plane_state(commit, plane);
	dma_addr_t address;
	__le32 *header;
	u32 width, height;

	if (!state->fb || !state->crtc || !READ_ONCE(display->powered) ||
	    READ_ONCE(display->active_port) < 0)
		return;
	width = state->src_w >> 16;
	height = state->src_h >> 16;
	address = drm_fb_dma_get_gem_addr(state->fb, state, 0);
	display->descriptor_index ^= 1;
	header = display->descriptors + display->descriptor_index * VO_OSD_HEADER_BYTES;
	memset(header, 0, VO_OSD_HEADER_BYTES);
	header[0] = cpu_to_le32(VO_OSD_HEADER_LAST);
	header[2] = cpu_to_le32(width | height << 16);
	header[3] = cpu_to_le32(VO_OSD_HEADER_XRGB_ORDER | 7 | BIT(9) |
			      255 << 16 | BIT(25));
	header[6] = cpu_to_le32(lower_32_bits(address));
	header[7] = header[6];
	header[8] = cpu_to_le32(state->fb->pitches[0]);
	dma_wmb();
	address = display->descriptor_dma + display->descriptor_index * VO_OSD_HEADER_BYTES;
	writel(lower_32_bits(address), display->vo + VO_OSD_HEADER);
	writel(lower_32_bits(address), display->vo + VO_OSD_HEADER_3D);
	writel(width << 12 | height, display->vo + VO_OSD_SIZE);
	if (!READ_ONCE(display->scanout_ready)) {
		WRITE_ONCE(display->scanout_ready, true);
		rtd1296_vo_update(display, VO_FRAME_CONTROL, VO_MIXER1_ENABLE, true);
	}
}

static const struct drm_plane_helper_funcs rtd1296_plane_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = rtd1296_plane_check,
	.atomic_update = rtd1296_plane_update,
};

static const struct drm_plane_funcs rtd1296_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.atomic_create_state = drm_atomic_helper_plane_create_state,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int rtd1296_crtc_check(struct drm_crtc *crtc,
			      struct drm_atomic_commit *commit)
{
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(commit, crtc);
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	int ret;

	if (hweight32(state->connector_mask) > 1)
		return -EINVAL;
	if (state->enable) {
		ret = 0;
		drm_connector_list_iter_begin(crtc->dev, &iter);
		drm_for_each_connector_iter(connector, &iter) {
			if (!(state->connector_mask & drm_connector_mask(connector)))
				continue;
			if (rtd1296_mode_valid(connector, &state->mode) != MODE_OK)
				ret = -EINVAL;
		}
		drm_connector_list_iter_end(&iter);
		if (ret)
			return ret;
		ret = drm_atomic_helper_check_crtc_primary_plane(state);
		if (ret)
			return ret;
	}
	return drm_atomic_add_affected_planes(commit, crtc);
}

static void rtd1296_crtc_enable(struct drm_crtc *crtc,
				struct drm_atomic_commit *commit)
{
	struct rtd1296_display *display = to_rtd1296_display(crtc->dev);
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(commit, crtc);
	struct drm_connector *connector;
	struct drm_connector_list_iter iter;
	struct rtd1296_output *output = NULL;
	u32 route = BIT(10) | BIT(2);
	int ret;

	drm_connector_list_iter_begin(crtc->dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (connector->state->crtc == crtc) {
			output = to_rtd1296_output(connector);
			break;
		}
	}
	drm_connector_list_iter_end(&iter);
	if (!output)
		return;
	ret = pm_runtime_resume_and_get(crtc->dev->dev);
	if (ret < 0)
		goto error;
	WRITE_ONCE(display->active_port, output->port);
	rtd1296_vo_update(display, VO_MODE, VO_MIXER1_ENABLE, true);
	writel(BIT(12), display->vo + VO_MIX);
	route |= output->port == RTD1296_HDMI ? BIT(8) : 1;
	writel(route, display->vo + VO_OUT);
	rtd1296_vo_update(display, VO_SYNC, BIT(1), output->port == RTD1296_DP);
	writel(state->adjusted_mode.hdisplay << 16 | state->adjusted_mode.vdisplay,
	       display->vo + VO_MIXER_SIZE);
	writel(0, display->vo + VO_OSD_HEADER_ENDIAN);
	rtd1296_csc_identity(display, VO_OSD_CSC);
	rtd1296_csc_identity(display, VO_PORT_L_CSC);
	for (ret = 0; ret < 11; ret++)
		writel(ret == 0 || ret == 4 || ret == 8 ? VO_PORT_H_CSC_UNITY : 0,
		       display->vo + VO_PORT_H_CSC + ret * sizeof(u32));
	rtd1296_vo_update(display, VO_OSD, VO_OSD_ENABLE | VO_OSD_RGB, true);
	if (output->port == RTD1296_HDMI) {
		mutex_lock(&display->aux_lock);
		ret = rtd1296_hdmi_enable(display, &state->adjusted_mode);
		mutex_unlock(&display->aux_lock);
	} else {
		ret = rtd1296_dp_enable(display, &state->adjusted_mode);
	}
	if (ret)
		goto power;
	if (READ_ONCE(output->link_failed)) {
		WRITE_ONCE(output->link_failed, false);
		schedule_work(&display->hotplug_work);
	}
	drm_crtc_vblank_on(crtc);
	return;
power:
	WRITE_ONCE(display->active_port, -1);
	pm_runtime_put_sync(crtc->dev->dev);
error:
	WRITE_ONCE(output->link_failed, true);
	schedule_work(&display->hotplug_work);
	drm_err(crtc->dev, "display enable failed: %d\n", ret);
}

static void rtd1296_crtc_disable(struct drm_crtc *crtc,
				 struct drm_atomic_commit *commit)
{
	struct rtd1296_display *display = to_rtd1296_display(crtc->dev);
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(commit, crtc);
	unsigned long flags;
	int port = READ_ONCE(display->active_port);

	drm_crtc_vblank_off(crtc);
	WRITE_ONCE(display->scanout_ready, false);
	if (port >= 0) {
		mutex_lock(&display->aux_lock);
		if (port == RTD1296_HDMI)
			rtd1296_hdmi_disable(display);
		else
			rtd1296_dp_disable(display);
		mutex_unlock(&display->aux_lock);
		synchronize_irq(display->outputs[port].irq);
		rtd1296_vo_update(display, VO_OSD, VO_OSD_ENABLE, false);
		rtd1296_vo_update(display, VO_MODE, VO_MIXER1_ENABLE, false);
		WRITE_ONCE(display->active_port, -1);
		pm_runtime_put_sync(crtc->dev->dev);
	}
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (state->event) {
		drm_crtc_send_vblank_event(crtc, state->event);
		state->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void rtd1296_crtc_flush(struct drm_crtc *crtc,
			       struct drm_atomic_commit *commit)
{
	struct rtd1296_display *display = to_rtd1296_display(crtc->dev);
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(commit, crtc);
	unsigned long flags;

	if (!state->event)
		return;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (READ_ONCE(display->active_port) >= 0 && !drm_crtc_vblank_get(crtc))
		drm_crtc_arm_vblank_event(crtc, state->event);
	else
		drm_crtc_send_vblank_event(crtc, state->event);
	state->event = NULL;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static const struct drm_crtc_helper_funcs rtd1296_crtc_helpers = {
	.atomic_check = rtd1296_crtc_check,
	.atomic_enable = rtd1296_crtc_enable,
	.atomic_disable = rtd1296_crtc_disable,
	.atomic_flush = rtd1296_crtc_flush,
};

static int rtd1296_enable_vblank(struct drm_crtc *crtc)
{
	WRITE_ONCE(to_rtd1296_display(crtc->dev)->vblank_enabled, true);
	return 0;
}

static void rtd1296_disable_vblank(struct drm_crtc *crtc)
{
	WRITE_ONCE(to_rtd1296_display(crtc->dev)->vblank_enabled, false);
}

static const struct drm_crtc_funcs rtd1296_crtc_funcs = {
	.atomic_create_state = drm_atomic_helper_crtc_create_state,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = rtd1296_enable_vblank,
	.disable_vblank = rtd1296_disable_vblank,
};

static const struct drm_mode_config_funcs rtd1296_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs rtd1296_mode_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static const struct drm_driver rtd1296_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &rtd1296_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = "rtd1296-drm",
	.desc = "Realtek RTD1296 display controller",
	.major = 1,
	.minor = 0,
};

static int rtd1296_runtime_resume(struct device *dev)
{
	struct rtd1296_display *display = to_rtd1296_display(dev_get_drvdata(dev));
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(display->clocks), display->clocks);
	if (ret)
		return ret;
	ret = reset_control_bulk_deassert(ARRAY_SIZE(display->shared_resets),
					  display->shared_resets);
	if (ret)
		goto clocks;
	ret = reset_control_bulk_deassert(ARRAY_SIZE(display->resets), display->resets);
	if (ret)
		goto domain;
	if (readl(display->vo + VO_MODE) == 0xdeadbeef) {
		ret = -EIO;
		goto resets;
	}
	rtd1296_vo_update(display, VO_INTERRUPT_ENABLE, GENMASK(11, 1), false);
	WRITE_ONCE(display->powered, true);
	return 0;
resets:
	reset_control_bulk_assert(ARRAY_SIZE(display->resets), display->resets);
domain:
	reset_control_bulk_assert(ARRAY_SIZE(display->shared_resets), display->shared_resets);
clocks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(display->clocks), display->clocks);
	return ret;
}

static int rtd1296_runtime_suspend(struct device *dev)
{
	struct rtd1296_display *display = to_rtd1296_display(dev_get_drvdata(dev));
	unsigned int i;
	int ret;

	WRITE_ONCE(display->powered, false);
	mutex_lock(&display->aux_lock);
	rtd1296_dp_disable(display);
	rtd1296_hdmi_disable(display);
	mutex_unlock(&display->aux_lock);
	for (i = 0; i < RTD1296_NUM_PORTS; i++)
		synchronize_irq(display->outputs[i].irq);
	ret = reset_control_bulk_assert(ARRAY_SIZE(display->resets), display->resets);
	if (ret)
		goto restore;
	ret = reset_control_bulk_assert(ARRAY_SIZE(display->shared_resets),
					display->shared_resets);
	if (ret) {
		reset_control_bulk_deassert(ARRAY_SIZE(display->resets), display->resets);
		goto restore;
	}
	clk_bulk_disable_unprepare(ARRAY_SIZE(display->clocks), display->clocks);
	return 0;
restore:
	WRITE_ONCE(display->powered, true);
	return ret;
}

static int __maybe_unused rtd1296_display_suspend(struct device *dev)
{
	int ret;

	ret = drm_mode_config_helper_suspend(dev_get_drvdata(dev));
	if (ret)
		return ret;
	ret = pm_runtime_force_suspend(dev);
	if (ret)
		drm_mode_config_helper_resume(dev_get_drvdata(dev));
	return ret;
}

static int __maybe_unused rtd1296_display_resume(struct device *dev)
{
	int ret = pm_runtime_force_resume(dev);

	return ret ? ret : drm_mode_config_helper_resume(dev_get_drvdata(dev));
}

static const struct dev_pm_ops rtd1296_display_pm_ops = {
	RUNTIME_PM_OPS(rtd1296_runtime_suspend, rtd1296_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(rtd1296_display_suspend, rtd1296_display_resume)
};

static void rtd1296_display_pm_disable(void *data)
{
	struct device *dev = data;
	int ret;

	pm_runtime_disable(dev);
	pm_runtime_dont_use_autosuspend(dev);
	if (pm_runtime_status_suspended(dev))
		return;
	ret = rtd1296_runtime_suspend(dev);
	if (ret) {
		dev_err(dev, "cannot suspend display: %d\n", ret);
		return;
	}
	pm_runtime_set_suspended(dev);
}

static int rtd1296_output_init(struct rtd1296_display *display, unsigned int port)
{
	struct rtd1296_output *output = &display->outputs[port];
	struct device *dev = display->drm.dev;
	const char *name = port == RTD1296_HDMI ? "hdmi-hpd" : "dp-hpd";
	int type = port == RTD1296_HDMI ? DRM_MODE_CONNECTOR_HDMIA : DRM_MODE_CONNECTOR_DisplayPort;
	int irq, ret;

	output->display = display;
	output->port = port;
	output->hpd = devm_gpiod_get(dev, name, GPIOD_IN);
	if (IS_ERR(output->hpd))
		return PTR_ERR(output->hpd);
	ret = drmm_encoder_init(&display->drm, &output->encoder, NULL,
				DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	output->encoder.possible_crtcs = drm_crtc_mask(&display->crtc);
	ret = drm_connector_init(&display->drm, &output->connector,
				 &rtd1296_connector_funcs, type);
	if (ret)
		return ret;
	output->connector.polled = DRM_CONNECTOR_POLL_HPD;
	drm_connector_helper_add(&output->connector, &rtd1296_connector_helpers);
	ret = drm_connector_attach_encoder(&output->connector, &output->encoder);
	if (ret)
		return ret;
	irq = gpiod_to_irq(output->hpd);
	if (irq < 0)
		return irq;
	output->hpd_irq = irq;
	return devm_request_threaded_irq(dev, irq, NULL, rtd1296_hpd_irq,
			IRQF_ONESHOT | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			dev_name(dev), display);
}

static void rtd1296_ddc_put(void *data)
{
	i2c_put_adapter(data);
}

static int rtd1296_display_probe(struct platform_device *pdev)
{
	static const char * const clock_names[] = { "vo", "hdmi", "tve", "lvds" };
	static const char * const reset_names[] = { "vo", "hdmi", "lvds", "dp" };
	struct device *dev = &pdev->dev;
	struct rtd1296_display *display;
	struct device_node *ddc;
	unsigned int i;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	display = devm_drm_dev_alloc(dev, &rtd1296_drm_driver, struct rtd1296_display, drm);
	if (IS_ERR(display))
		return PTR_ERR(display);
	platform_set_drvdata(pdev, &display->drm);
	display->active_port = -1;
	INIT_WORK(&display->hotplug_work, rtd1296_hotplug_work);
	mutex_init(&display->aux_lock);
	display->vo = devm_platform_ioremap_resource_byname(pdev, "vo");
	if (IS_ERR(display->vo))
		return PTR_ERR(display->vo);
	display->hdmi = devm_platform_ioremap_resource_byname(pdev, "hdmi");
	if (IS_ERR(display->hdmi))
		return PTR_ERR(display->hdmi);
	display->lvds = devm_platform_ioremap_resource_byname(pdev, "lvds");
	if (IS_ERR(display->lvds))
		return PTR_ERR(display->lvds);
	display->dp = devm_platform_ioremap_resource_byname(pdev, "dp");
	if (IS_ERR(display->dp))
		return PTR_ERR(display->dp);
	display->crt = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,crt");
	if (IS_ERR(display->crt))
		return PTR_ERR(display->crt);
	for (i = 0; i < ARRAY_SIZE(clock_names); i++)
		display->clocks[i].id = clock_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(display->clocks), display->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get display clocks\n");
	for (i = 0; i < ARRAY_SIZE(reset_names); i++)
		display->resets[i].id = reset_names[i];
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(display->resets),
						    display->resets);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get display resets\n");
	display->shared_resets[0].id = "domain";
	display->shared_resets[1].id = "tve";
	ret = devm_reset_control_bulk_get_shared(dev, ARRAY_SIZE(display->shared_resets),
						 display->shared_resets);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get shared display resets\n");
	ddc = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (!ddc)
		return -EINVAL;
	display->ddc = of_get_i2c_adapter_by_node(ddc);
	of_node_put(ddc);
	if (!display->ddc)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, rtd1296_ddc_put, display->ddc);
	if (ret)
		return ret;
	display->descriptors = dmam_alloc_coherent(dev, 2 * VO_OSD_HEADER_BYTES,
						   &display->descriptor_dma, GFP_KERNEL);
	if (!display->descriptors)
		return -ENOMEM;
	ret = drmm_mode_config_init(&display->drm);
	if (ret)
		return ret;
	display->drm.mode_config.funcs = &rtd1296_mode_funcs;
	display->drm.mode_config.helper_private = &rtd1296_mode_helpers;
	display->drm.mode_config.min_width = 1280;
	display->drm.mode_config.min_height = 720;
	display->drm.mode_config.max_width = 3840;
	display->drm.mode_config.max_height = 2160;
	ret = drm_universal_plane_init(&display->drm, &display->plane, 1,
				       &rtd1296_plane_funcs, rtd1296_formats,
				       ARRAY_SIZE(rtd1296_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&display->plane, &rtd1296_plane_helpers);
	ret = drm_crtc_init_with_planes(&display->drm, &display->crtc,
					&display->plane, NULL, &rtd1296_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&display->crtc, &rtd1296_crtc_helpers);
	ret = drm_vblank_init(&display->drm, 1);
	if (ret)
		return ret;
	for (i = 0; i < RTD1296_NUM_PORTS; i++) {
		ret = rtd1296_output_init(display, i);
		if (ret)
			return dev_err_probe(dev, ret, "cannot initialize connector\n");
		display->outputs[i].irq = platform_get_irq(pdev, i);
		if (display->outputs[i].irq < 0)
			return display->outputs[i].irq;
		ret = devm_request_irq(dev, display->outputs[i].irq,
				       rtd1296_timing_irq, 0, dev_name(dev), &display->outputs[i]);
		if (ret)
			return ret;
	}
	pm_runtime_set_autosuspend_delay(dev, 500);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	ret = devm_add_action_or_reset(dev, rtd1296_display_pm_disable, dev);
	if (ret)
		return ret;
	ret = rtd1296_dp_init(display);
	if (ret)
		return ret;
	drm_mode_config_reset(&display->drm);
	drm_kms_helper_poll_init(&display->drm);
	ret = drm_dev_register(&display->drm, 0);
	if (ret) {
		drm_kms_helper_poll_fini(&display->drm);
		rtd1296_dp_fini(display);
		return ret;
	}
	WRITE_ONCE(display->registered, true);
	drm_client_setup_with_fourcc(&display->drm, DRM_FORMAT_XRGB8888);
	return 0;
}

static void rtd1296_display_remove(struct platform_device *pdev)
{
	struct rtd1296_display *display = to_rtd1296_display(platform_get_drvdata(pdev));
	unsigned int i;

	WRITE_ONCE(display->registered, false);
	for (i = 0; i < RTD1296_NUM_PORTS; i++)
		disable_irq(display->outputs[i].hpd_irq);
	cancel_work_sync(&display->hotplug_work);
	drm_dev_unplug(&display->drm);
	drm_atomic_helper_shutdown(&display->drm);
	drm_kms_helper_poll_fini(&display->drm);
	rtd1296_dp_fini(display);
}

static void rtd1296_display_shutdown(struct platform_device *pdev)
{
	struct rtd1296_display *display = to_rtd1296_display(platform_get_drvdata(pdev));

	drm_atomic_helper_shutdown(&display->drm);
}

static const struct of_device_id rtd1296_display_match[] = {
	{ .compatible = "realtek,rtd1296-display" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_display_match);

static struct platform_driver rtd1296_display_driver = {
	.probe = rtd1296_display_probe,
	.remove = rtd1296_display_remove,
	.shutdown = rtd1296_display_shutdown,
	.driver = {
		.name = "rtd1296-display",
		.of_match_table = rtd1296_display_match,
		.pm = pm_ptr(&rtd1296_display_pm_ops),
	},
};
module_platform_driver(rtd1296_display_driver);

MODULE_DESCRIPTION("Realtek RTD1296 display controller");
MODULE_LICENSE("GPL");
