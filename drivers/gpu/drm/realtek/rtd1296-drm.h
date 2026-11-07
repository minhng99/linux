/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __RTD1296_DRM_H__
#define __RTD1296_DRM_H__

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/workqueue.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

enum rtd1296_port {
	RTD1296_HDMI,
	RTD1296_DP,
	RTD1296_NUM_PORTS,
};

struct rtd1296_display;

struct rtd1296_output {
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct rtd1296_display *display;
	struct gpio_desc *hpd;
	enum rtd1296_port port;
	int irq;
	int hpd_irq;
	bool link_failed;
};

struct rtd1296_display {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct rtd1296_output outputs[RTD1296_NUM_PORTS];
	void __iomem *vo;
	void __iomem *hdmi;
	void __iomem *lvds;
	void __iomem *dp;
	struct regmap *crt;
	struct clk_bulk_data clocks[4];
	struct reset_control_bulk_data resets[4];
	struct reset_control_bulk_data shared_resets[2];
	struct i2c_adapter *ddc;
	struct drm_dp_aux aux;
	/* Serializes AUX transfers and analogue power changes. */
	struct mutex aux_lock;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	void *descriptors;
	dma_addr_t descriptor_dma;
	unsigned int descriptor_index;
	struct work_struct hotplug_work;
	int active_port;
	bool powered;
	bool scanout_ready;
	bool vblank_enabled;
	bool registered;
	bool aux_powered;
	bool hdmi_powered;
};

static inline struct rtd1296_display *to_rtd1296_display(struct drm_device *drm)
{
	return container_of(drm, struct rtd1296_display, drm);
}

static inline struct rtd1296_output *to_rtd1296_output(struct drm_connector *connector)
{
	return container_of(connector, struct rtd1296_output, connector);
}

int rtd1296_hdmi_enable(struct rtd1296_display *display,
			const struct drm_display_mode *mode);
void rtd1296_hdmi_disable(struct rtd1296_display *display);
bool rtd1296_hdmi_vblank(struct rtd1296_display *display);
int rtd1296_dp_init(struct rtd1296_display *display);
void rtd1296_dp_fini(struct rtd1296_display *display);
int rtd1296_dp_enable(struct rtd1296_display *display,
		      const struct drm_display_mode *mode);
void rtd1296_dp_disable(struct rtd1296_display *display);
bool rtd1296_dp_vblank(struct rtd1296_display *display);

#endif
