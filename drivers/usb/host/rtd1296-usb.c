// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/usb.h>

#include "rtd1296-usb.h"

#define USB2_PHY_WAKE	BIT(6)
#define USB2_WRAPPER_2PORT	0x20

struct rtd1296_usb {
	struct device *dev;
	struct clk *clock;
	void __iomem *usb2_wrapper;
	struct regulator *usb2_vbus;
	struct reset_control *serdes;
	struct reset_control_bulk_data phys[6];
	struct reset_control_bulk_data interfaces[3];
	bool clock_enabled;
	bool serdes_enabled;
	bool usb2_powered;
};

int rtd1296_usb2_wake(struct device *dev)
{
	struct rtd1296_usb *usb;
	void __iomem *second;

	if (!dev || !of_device_is_compatible(dev->of_node, "realtek,rtd1296-usb-bus"))
		return -EINVAL;
	usb = dev_get_drvdata(dev);
	if (!usb)
		return -EPROBE_DEFER;
	if (!usb->clock_enabled)
		return -EIO;
	second = usb->usb2_wrapper + USB2_WRAPPER_2PORT;
	writel(readl(usb->usb2_wrapper) | USB2_PHY_WAKE, usb->usb2_wrapper);
	writel(readl(second) | USB2_PHY_WAKE, second);
	readl(second);
	fsleep(1000);
	return 0;
}
EXPORT_SYMBOL_GPL(rtd1296_usb2_wake);

static int rtd1296_usb_clock_enable(struct rtd1296_usb *usb)
{
	int ret;

	if (usb->clock_enabled)
		return 0;
	ret = clk_prepare_enable(usb->clock);
	if (!ret)
		usb->clock_enabled = true;
	return ret;
}

static void rtd1296_usb_clock_disable(struct rtd1296_usb *usb)
{
	if (usb->clock_enabled) {
		clk_disable_unprepare(usb->clock);
		usb->clock_enabled = false;
	}
}

static void rtd1296_usb_stop(struct rtd1296_usb *usb)
{
	int ret;

	if (usb->usb2_powered) {
		ret = regulator_disable(usb->usb2_vbus);
		if (ret)
			dev_err(usb->dev, "Cannot disable USB2 VBUS: %d\n", ret);
		else
			usb->usb2_powered = false;
	}
	ret = reset_control_bulk_assert(ARRAY_SIZE(usb->interfaces), usb->interfaces);
	if (ret)
		dev_err(usb->dev, "Cannot assert interface resets: %d\n", ret);
	ret = reset_control_bulk_assert(ARRAY_SIZE(usb->phys), usb->phys);
	if (ret)
		dev_err(usb->dev, "Cannot assert PHY resets: %d\n", ret);
	rtd1296_usb_clock_disable(usb);
	if (usb->serdes_enabled) {
		ret = reset_control_assert(usb->serdes);
		if (ret)
			dev_err(usb->dev, "Cannot release SerDes reset reference: %d\n", ret);
		else
			usb->serdes_enabled = false;
	}
}

static int rtd1296_usb_start(struct rtd1296_usb *usb)
{
	int ret;

	ret = reset_control_deassert(usb->serdes);
	if (ret)
		return ret;
	usb->serdes_enabled = true;
	ret = rtd1296_usb_clock_enable(usb);
	if (ret)
		goto stop;
	ret = reset_control_bulk_assert(ARRAY_SIZE(usb->interfaces), usb->interfaces);
	if (ret)
		goto stop;
	ret = reset_control_bulk_assert(ARRAY_SIZE(usb->phys), usb->phys);
	if (ret)
		goto stop;
	rtd1296_usb_clock_disable(usb);
	ret = reset_control_bulk_deassert(ARRAY_SIZE(usb->phys), usb->phys);
	if (ret)
		goto stop;
	msleep(200);
	ret = rtd1296_usb_clock_enable(usb);
	if (ret)
		goto stop;
	rtd1296_usb_clock_disable(usb);
	fsleep(10000);
	ret = reset_control_bulk_deassert(ARRAY_SIZE(usb->interfaces), usb->interfaces);
	if (ret)
		goto stop;
	fsleep(10000);
	ret = rtd1296_usb_clock_enable(usb);
	if (ret)
		goto stop;
	fsleep(10000);
	writel(readl(usb->usb2_wrapper + 0x2c) | BIT(0), usb->usb2_wrapper + 0x2c);
	writel(readl(usb->usb2_wrapper + 0x40) & ~BIT(0), usb->usb2_wrapper + 0x40);
	writel(readl(usb->usb2_wrapper + 0x44) & ~GENMASK(8, 7),
	       usb->usb2_wrapper + 0x44);
	fsleep(1000);
	if (usb->usb2_vbus) {
		ret = regulator_enable(usb->usb2_vbus);
		if (ret)
			goto stop;
		usb->usb2_powered = true;
	}
	return 0;

stop:
	rtd1296_usb_stop(usb);
	return ret;
}

static int rtd1296_usb_probe(struct platform_device *pdev)
{
	static const char * const phy_names[] = {
		"usb2-0", "usb2-1", "usb2-2", "usb3-0", "usb2-3", "usb3-1",
	};
	static const char * const interface_names[] = {
		"controller", "mdio0", "mdio1",
	};
	struct device *dev = &pdev->dev;
	struct rtd1296_usb *usb;
	int i, ret;

	if (usb_disabled())
		return -ENODEV;
	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dev = dev;
	usb->usb2_wrapper = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(usb->usb2_wrapper))
		return PTR_ERR(usb->usb2_wrapper);
	usb->usb2_vbus = devm_regulator_get_optional(dev, "usb2-vbus");
	if (IS_ERR(usb->usb2_vbus)) {
		ret = PTR_ERR(usb->usb2_vbus);
		if (ret != -ENODEV)
			return dev_err_probe(dev, ret, "Cannot get USB2 VBUS supply\n");
		usb->usb2_vbus = NULL;
	}
	usb->clock = devm_clk_get(dev, NULL);
	if (IS_ERR(usb->clock))
		return dev_err_probe(dev, PTR_ERR(usb->clock), "Cannot get clock\n");
	usb->serdes = devm_reset_control_get_shared(dev, "serdes");
	if (IS_ERR(usb->serdes))
		return dev_err_probe(dev, PTR_ERR(usb->serdes), "Cannot get SerDes reset\n");
	for (i = 0; i < ARRAY_SIZE(usb->phys); i++)
		usb->phys[i].id = phy_names[i];
	for (i = 0; i < ARRAY_SIZE(usb->interfaces); i++)
		usb->interfaces[i].id = interface_names[i];
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(usb->phys), usb->phys);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get PHY resets\n");
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(usb->interfaces),
						    usb->interfaces);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get interface resets\n");
	ret = rtd1296_usb_start(usb);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot initialize USB interfaces\n");
	platform_set_drvdata(pdev, usb);
	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret) {
		of_platform_depopulate(dev);
		rtd1296_usb_stop(usb);
	}
	return ret;
}

static void rtd1296_usb_remove(struct platform_device *pdev)
{
	struct rtd1296_usb *usb = platform_get_drvdata(pdev);

	of_platform_depopulate(&pdev->dev);
	rtd1296_usb_stop(usb);
}

static void rtd1296_usb_shutdown(struct platform_device *pdev)
{
	rtd1296_usb_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id rtd1296_usb_match[] = {
	{ .compatible = "realtek,rtd1296-usb-bus" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_usb_match);

static const struct dev_pm_ops rtd1296_usb_pm_ops = {
	.resume = pm_sleep_ptr(rtd1296_usb2_wake),
	.thaw = pm_sleep_ptr(rtd1296_usb2_wake),
	.restore = pm_sleep_ptr(rtd1296_usb2_wake),
};

static struct platform_driver rtd1296_usb_driver = {
	.probe = rtd1296_usb_probe,
	.remove = rtd1296_usb_remove,
	.shutdown = rtd1296_usb_shutdown,
	.driver = {
		.name = "rtd1296-usb",
		.of_match_table = rtd1296_usb_match,
		.pm = pm_sleep_ptr(&rtd1296_usb_pm_ops),
	},
};
module_platform_driver(rtd1296_usb_driver);

MODULE_DESCRIPTION("Realtek RTD1296 USB clock and reset control");
MODULE_LICENSE("GPL");
