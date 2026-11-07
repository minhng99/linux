// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec.h>
#include <linux/workqueue.h>

#define CC1_CONTROL	0x00
#define CC1_REFERENCE	0x04
#define CC2_CONTROL	0x08
#define CC2_REFERENCE	0x0c
#define CC_STATUS	0x10
#define CC_INTERRUPT	0x14
#define CC_ROUTE		GENMASK(29, 27)
#define CC_ROUTE_ENABLE	BIT(29)
#define CC_ROUTE_REVERSE	GENMASK(28, 27)
#define CC_ENABLE	GENMASK(6, 0)
#define CC_SOURCE_USB	(BIT(5) | BIT(3) | BIT(0))
#define CC1_STATE	GENMASK(2, 0)
#define CC2_STATE	GENMASK(5, 3)
#define CC_RD		3
#define CC_IRQ_ENABLE	GENMASK(11, 10)
#define CC_IRQ_STATUS	GENMASK(9, 8)
#define CC_SAMPLE_MS	20

struct rtd1296_typec {
	struct device *dev;
	void __iomem *base;
	struct regulator *vbus;
	struct typec_port *port;
	struct typec_partner *partner;
	/* Serialize connector transitions and hardware shutdown. */
	struct mutex lock;
	struct delayed_work work;
	unsigned long changed;
	enum typec_orientation attached;
	enum typec_orientation candidate;
	int irq;
	bool active;
	bool vbus_enabled;
	bool irq_wake;
	bool suspended;
	bool port_disabled;
};

static void rtd1296_typec_route(struct rtd1296_typec *cc,
				enum typec_orientation orientation)
{
	u32 value = readl(cc->base + CC1_CONTROL) & ~CC_ROUTE;

	if (orientation != TYPEC_ORIENTATION_NONE) {
		value |= CC_ROUTE_ENABLE;
		if (orientation == TYPEC_ORIENTATION_REVERSE)
			value |= CC_ROUTE_REVERSE;
	}
	writel(value, cc->base + CC1_CONTROL);
}

static int rtd1296_typec_detach(struct rtd1296_typec *cc)
{
	int ret;

	if (cc->vbus_enabled) {
		ret = regulator_disable(cc->vbus);
		if (ret)
			return ret;
		cc->vbus_enabled = false;
	}
	rtd1296_typec_route(cc, TYPEC_ORIENTATION_NONE);
	if (cc->partner) {
		typec_unregister_partner(cc->partner);
		cc->partner = NULL;
	}
	cc->attached = TYPEC_ORIENTATION_NONE;
	return typec_set_orientation(cc->port, TYPEC_ORIENTATION_NONE);
}

static int rtd1296_typec_attach(struct rtd1296_typec *cc,
				enum typec_orientation orientation)
{
	struct typec_partner_desc partner = { };
	struct typec_partner *device;
	int ret;

	ret = typec_set_orientation(cc->port, orientation);
	if (ret)
		return ret;
	rtd1296_typec_route(cc, orientation);
	ret = regulator_enable(cc->vbus);
	if (ret)
		goto detach;
	cc->vbus_enabled = true;
	device = typec_register_partner(cc->port, &partner);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto detach;
	}
	cc->partner = device;
	cc->attached = orientation;
	return 0;

detach:
	rtd1296_typec_detach(cc);
	return ret;
}

static enum typec_orientation rtd1296_typec_orientation(struct rtd1296_typec *cc)
{
	u32 status = readl(cc->base + CC_STATUS);
	bool cc1 = FIELD_GET(CC1_STATE, status) == CC_RD;
	bool cc2 = FIELD_GET(CC2_STATE, status) == CC_RD;

	if (cc1 == cc2)
		return TYPEC_ORIENTATION_NONE;
	return cc1 ? TYPEC_ORIENTATION_NORMAL : TYPEC_ORIENTATION_REVERSE;
}

static void rtd1296_typec_work(struct work_struct *work)
{
	struct rtd1296_typec *cc = container_of(to_delayed_work(work),
					     struct rtd1296_typec, work);
	enum typec_orientation orientation;
	unsigned int debounce;
	int ret = 0;

	mutex_lock(&cc->lock);
	if (!READ_ONCE(cc->active) || READ_ONCE(cc->suspended))
		goto unlock;
	orientation = rtd1296_typec_orientation(cc);
	if (orientation != cc->candidate) {
		cc->candidate = orientation;
		cc->changed = jiffies;
	}
	if (cc->attached == TYPEC_ORIENTATION_NONE && cc->vbus_enabled) {
		ret = rtd1296_typec_detach(cc);
		cc->changed = jiffies;
		goto requeue;
	}
	debounce = cc->attached == TYPEC_ORIENTATION_NONE ?
		   PD_T_CC_DEBOUNCE : PD_T_PD_DEBOUNCE;
	if (orientation != cc->attached &&
	    time_after_eq(jiffies, cc->changed + msecs_to_jiffies(debounce))) {
		if (cc->attached != TYPEC_ORIENTATION_NONE) {
			ret = rtd1296_typec_detach(cc);
			cc->changed = jiffies;
		} else {
			ret = rtd1296_typec_attach(cc, orientation);
		}
	}
requeue:
	if (ret)
		dev_err_ratelimited(cc->dev, "Connector transition failed: %d\n", ret);
	queue_delayed_work(system_freezable_wq, &cc->work, msecs_to_jiffies(CC_SAMPLE_MS));
unlock:
	mutex_unlock(&cc->lock);
}

static irqreturn_t rtd1296_typec_irq(int irq, void *data)
{
	struct rtd1296_typec *cc = data;
	u32 value = readl(cc->base + CC_INTERRUPT);

	if (!(value & CC_IRQ_STATUS))
		return IRQ_NONE;
	writel(value & ~CC_IRQ_STATUS, cc->base + CC_INTERRUPT);
	readl(cc->base + CC_INTERRUPT);
	if (READ_ONCE(cc->active)) {
		if (device_may_wakeup(cc->dev))
			pm_wakeup_event(cc->dev, PD_T_CC_DEBOUNCE + 100);
		if (!READ_ONCE(cc->suspended))
			mod_delayed_work(system_freezable_wq, &cc->work, 0);
	}
	return IRQ_HANDLED;
}

static void rtd1296_typec_stop(void *data)
{
	struct rtd1296_typec *cc = data;
	u32 value;
	int ret;

	if (!READ_ONCE(cc->active))
		return;
	WRITE_ONCE(cc->active, false);
	disable_irq(cc->irq);
	cancel_delayed_work_sync(&cc->work);
	if (cc->irq_wake) {
		disable_irq_wake(cc->irq);
		cc->irq_wake = false;
	}
	mutex_lock(&cc->lock);
	value = readl(cc->base + CC_INTERRUPT);
	writel(value & ~(CC_IRQ_ENABLE | CC_IRQ_STATUS), cc->base + CC_INTERRUPT);
	ret = rtd1296_typec_detach(cc);
	if (ret)
		dev_err(cc->dev, "Cannot turn off connector: %d\n", ret);
	value = readl(cc->base + CC1_CONTROL);
	writel(value & ~(CC_ENABLE | CC_ROUTE), cc->base + CC1_CONTROL);
	value = readl(cc->base + CC2_CONTROL);
	writel(value & ~CC_ENABLE, cc->base + CC2_CONTROL);
	mutex_unlock(&cc->lock);
	device_init_wakeup(cc->dev, false);
}

static void rtd1296_typec_unregister(void *data)
{
	typec_unregister_port(data);
}

static void rtd1296_typec_put_connector(void *data)
{
	fwnode_handle_put(data);
}

static int rtd1296_typec_probe(struct platform_device *pdev)
{
	struct typec_capability capability = { };
	struct device *dev = &pdev->dev;
	struct fwnode_handle *connector;
	struct rtd1296_typec *cc;
	struct gpio_desc *rd;
	const char *mode;
	u32 value;
	int ret;

	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;
	cc->dev = dev;
	mutex_init(&cc->lock);
	INIT_DELAYED_WORK(&cc->work, rtd1296_typec_work);
	cc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cc->base))
		return PTR_ERR(cc->base);
	value = readl(cc->base + CC_INTERRUPT);
	writel(value & ~(CC_IRQ_ENABLE | CC_IRQ_STATUS), cc->base + CC_INTERRUPT);
	value = readl(cc->base + CC1_CONTROL);
	writel(value & ~(CC_ENABLE | CC_ROUTE), cc->base + CC1_CONTROL);
	value = readl(cc->base + CC2_CONTROL);
	writel(value & ~CC_ENABLE, cc->base + CC2_CONTROL);
	cc->irq = platform_get_irq(pdev, 0);
	if (cc->irq < 0)
		return cc->irq;
	cc->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(cc->vbus))
		return dev_err_probe(dev, PTR_ERR(cc->vbus), "Cannot get VBUS supply\n");
	rd = devm_gpiod_get_optional(dev, "realtek,rd-ctrl", GPIOD_OUT_HIGH);
	if (IS_ERR(rd))
		return dev_err_probe(dev, PTR_ERR(rd), "Cannot disable external Rd\n");
	connector = device_get_named_child_node(dev, "connector");
	if (!connector)
		return dev_err_probe(dev, -EINVAL, "Connector description required\n");
	ret = devm_add_action_or_reset(dev, rtd1296_typec_put_connector, connector);
	if (ret)
		return ret;
	ret = typec_get_fw_cap(&capability, connector);
	if (ret)
		return ret;
	if (!fwnode_property_present(connector, "data-role") ||
	    capability.type != TYPEC_PORT_SRC || capability.data != TYPEC_PORT_DFP ||
	    !fwnode_property_read_bool(connector, "pd-disable"))
		return dev_err_probe(dev, -EOPNOTSUPP, "Source-only USB connector required\n");
	ret = fwnode_property_read_string(connector, "typec-power-opmode", &mode);
	if (!ret && strcmp(mode, "default"))
		return dev_err_probe(dev, -EOPNOTSUPP, "Default USB current required\n");
	capability.revision = USB_TYPEC_REV_1_0;
	capability.orientation_aware = true;
	capability.usb_capability = USB_CAPABILITY_USB2 | USB_CAPABILITY_USB3;
	capability.fwnode = connector;
	cc->port = typec_register_port(dev, &capability);
	if (IS_ERR(cc->port))
		return PTR_ERR(cc->port);
	ret = devm_add_action_or_reset(dev, rtd1296_typec_unregister, cc->port);
	if (ret)
		return ret;
	typec_set_pwr_role(cc->port, TYPEC_SOURCE);
	typec_set_data_role(cc->port, TYPEC_HOST);
	typec_set_pwr_opmode(cc->port, TYPEC_PWR_MODE_USB);
	value = readl(cc->base + CC_INTERRUPT);
	writel(value & ~(CC_IRQ_ENABLE | CC_IRQ_STATUS), cc->base + CC_INTERRUPT);
	ret = devm_request_irq(dev, cc->irq, rtd1296_typec_irq, 0, dev_name(dev), cc);
	if (ret)
		return ret;
	ret = device_init_wakeup(dev, true);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, rtd1296_typec_stop, cc);
	if (ret) {
		device_init_wakeup(dev, false);
		return ret;
	}
	platform_set_drvdata(pdev, cc);
	writel((0xb << 22) | (0x17 << 17) | (0x10 << 12) | CC_SOURCE_USB,
	       cc->base + CC1_CONTROL);
	writel((0xc << 22) | (0x17 << 17) | (0x12 << 12) | CC_SOURCE_USB,
	       cc->base + CC2_CONTROL);
	writel((3 << 18) | (4 << 8), cc->base + CC1_REFERENCE);
	writel((2 << 26) | (3 << 18) | (5 << 8), cc->base + CC2_REFERENCE);
	WRITE_ONCE(cc->active, true);
	writel(CC_IRQ_ENABLE | 0x7f, cc->base + CC_INTERRUPT);
	queue_delayed_work(system_freezable_wq, &cc->work, 0);
	return 0;
}

static void rtd1296_typec_shutdown(struct platform_device *pdev)
{
	rtd1296_typec_stop(platform_get_drvdata(pdev));
}

static int rtd1296_typec_suspend(struct device *dev)
{
	struct rtd1296_typec *cc = dev_get_drvdata(dev);
	u32 value;
	int ret = 0;

	mutex_lock(&cc->lock);
	WRITE_ONCE(cc->suspended, true);
	mutex_unlock(&cc->lock);
	cancel_delayed_work_sync(&cc->work);
	if (device_may_wakeup(dev)) {
		ret = enable_irq_wake(cc->irq);
		if (!ret)
			cc->irq_wake = true;
	} else {
		mutex_lock(&cc->lock);
		ret = rtd1296_typec_detach(cc);
		if (!ret) {
			value = readl(cc->base + CC_INTERRUPT);
			writel(value & ~(CC_IRQ_ENABLE | CC_IRQ_STATUS),
			       cc->base + CC_INTERRUPT);
			value = readl(cc->base + CC1_CONTROL);
			writel(value & ~CC_ENABLE, cc->base + CC1_CONTROL);
			value = readl(cc->base + CC2_CONTROL);
			writel(value & ~CC_ENABLE, cc->base + CC2_CONTROL);
			cc->port_disabled = true;
		}
		mutex_unlock(&cc->lock);
	}
	if (ret) {
		WRITE_ONCE(cc->suspended, false);
		queue_delayed_work(system_freezable_wq, &cc->work, 0);
	}
	return ret;
}

static int rtd1296_typec_resume(struct device *dev)
{
	struct rtd1296_typec *cc = dev_get_drvdata(dev);
	u32 value;

	if (cc->irq_wake) {
		disable_irq_wake(cc->irq);
		cc->irq_wake = false;
	}
	mutex_lock(&cc->lock);
	if (cc->port_disabled) {
		value = readl(cc->base + CC1_CONTROL);
		writel((value & ~CC_ENABLE) | CC_SOURCE_USB, cc->base + CC1_CONTROL);
		value = readl(cc->base + CC2_CONTROL);
		writel((value & ~CC_ENABLE) | CC_SOURCE_USB, cc->base + CC2_CONTROL);
		value = readl(cc->base + CC_INTERRUPT);
		writel((value & ~CC_IRQ_STATUS) | CC_IRQ_ENABLE,
		       cc->base + CC_INTERRUPT);
		cc->candidate = TYPEC_ORIENTATION_NONE;
		cc->changed = jiffies;
		cc->port_disabled = false;
	}
	WRITE_ONCE(cc->suspended, false);
	mutex_unlock(&cc->lock);
	queue_delayed_work(system_freezable_wq, &cc->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rtd1296_typec_pm_ops, rtd1296_typec_suspend,
			      rtd1296_typec_resume);

static const struct of_device_id rtd1296_typec_match[] = {
	{ .compatible = "realtek,rtd1296-type-c" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_typec_match);

static struct platform_driver rtd1296_typec_driver = {
	.probe = rtd1296_typec_probe,
	.shutdown = rtd1296_typec_shutdown,
	.driver = {
		.name = "rtd1296-typec",
		.of_match_table = rtd1296_typec_match,
		.pm = pm_sleep_ptr(&rtd1296_typec_pm_ops),
	},
};
module_platform_driver(rtd1296_typec_driver);

MODULE_DESCRIPTION("Realtek RTD1296 USB Type-C source controller");
MODULE_LICENSE("GPL");
