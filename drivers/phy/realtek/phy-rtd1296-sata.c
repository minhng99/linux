// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define SATA_MDIO	0x00
#define SATA_SELECT	0x04
#define SATA_RATE	0x08
#define SATA_BUSY	BIT(7)
#define SATA_REGISTER	GENMASK(13, 8)
#define SATA_GENERATION	GENMASK(15, 14)
#define SATA_VALUE	GENMASK(31, 16)
#define SATA_OOB		0x980
#define SATA_PORTS	2

struct rtd1296_sata;

struct rtd1296_sata_port {
	struct rtd1296_sata *controller;
	struct phy *phy;
	struct reset_control *power;
	u8 index;
};

struct rtd1296_sata {
	struct device *dev;
	void __iomem *base;
	struct regmap *sb2;
	struct clk_bulk_data clocks[2];
	struct reset_control *serdes;
	struct rtd1296_sata_port ports[SATA_PORTS];
	/* Protect the shared MDIO selector and clock references. */
	struct mutex lock;
	u8 initialized;
	bool clocked;
};

struct rtd1296_sata_setting {
	u8 reg;
	u16 generation[3];
};

static const struct rtd1296_sata_setting rtd1296_sata_settings[] = {
	{ 0x11, { 0x0000, 0x0000, 0x0000 } },
	{ 0x05, { 0x336a, 0x336a, 0x336a } },
	{ 0x01, { 0xe070, 0xe05c, 0xe04a } },
	{ 0x06, { 0x0015, 0x0015, 0x0015 } },
	{ 0x0a, { 0xc600, 0xc600, 0xc600 } },
	{ 0x02, { 0x7000, 0x7000, 0x7000 } },
	{ 0x0a, { 0xc660, 0xc660, 0xc660 } },
	{ 0x19, { 0x2004, 0x2004, 0x2004 } },
	{ 0x20, { 0x94aa, 0x94aa, 0x94aa } },
	{ 0x15, { 0x1717, 0x1717, 0x1717 } },
	{ 0x16, { 0x0770, 0x0770, 0x0770 } },
	{ 0x2a, { 0x4000, 0x4000, 0x4000 } },
	{ 0x10, { 0x2900, 0x2900, 0x2900 } },
	{ 0x0c, { 0x4000, 0x4000, 0x4000 } },
	{ 0x17, { 0x0027, 0x0027, 0x0027 } },
	{ 0x20, { 0x94a7, 0x94a7, 0x94a7 } },
	{ 0x21, { 0x587a, 0x587a, 0x587a } },
	{ 0x04, { 0x538e, 0x538e, 0x538e } },
	{ 0x09, { 0x4210, 0x4210, 0x4210 } },
	{ 0x03, { 0x276f, 0x276d, 0x276d } },
	{ 0x2a, { 0x7c00, 0x7c00, 0x7c00 } },
};

static int rtd1296_sata_wait_idle(struct rtd1296_sata *controller)
{
	u32 status;

	return readl_poll_timeout(controller->base + SATA_MDIO, status,
				  !(status & SATA_BUSY), 10, 10000);
}

static int rtd1296_sata_write(struct rtd1296_sata *controller,
			      u8 reg, unsigned int generation, u16 value)
{
	int ret;

	ret = rtd1296_sata_wait_idle(controller);
	if (ret)
		return ret;
	writel(BIT(0) | FIELD_PREP(SATA_REGISTER, reg) |
	       FIELD_PREP(SATA_GENERATION, generation) | FIELD_PREP(SATA_VALUE, value),
	       controller->base + SATA_MDIO);
	fsleep(1000);
	return rtd1296_sata_wait_idle(controller);
}

static void rtd1296_sata_clocks_off(struct rtd1296_sata *controller)
{
	if (controller->clocked) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(controller->clocks), controller->clocks);
		controller->clocked = false;
	}
}

static int rtd1296_sata_init(struct phy *phy)
{
	struct rtd1296_sata_port *port = phy_get_drvdata(phy);
	struct rtd1296_sata *controller = port->controller;
	unsigned int i, generation;
	u32 mask, value;
	int ret = 0;

	mutex_lock(&controller->lock);
	if (controller->initialized & BIT(port->index))
		goto unlock;
	if (!controller->clocked) {
		ret = clk_bulk_prepare_enable(ARRAY_SIZE(controller->clocks), controller->clocks);
		if (ret)
			goto unlock;
		controller->clocked = true;
	}
	ret = reset_control_assert(port->power);
	if (ret)
		goto disable;
	mask = port->index ? BIT(1) | BIT(3) | BIT(5) :
			    BIT(0) | BIT(2) | BIT(4) | BIT(8);
	ret = regmap_update_bits(controller->sb2, SATA_OOB, mask, mask);
	if (ret)
		goto disable;
	ret = rtd1296_sata_wait_idle(controller);
	if (ret)
		goto disable;
	writel(port->index, controller->base + SATA_SELECT);
	for (i = 0; i < ARRAY_SIZE(rtd1296_sata_settings); i++) {
		const struct rtd1296_sata_setting *setting = &rtd1296_sata_settings[i];

		for (generation = 0; generation < ARRAY_SIZE(setting->generation); generation++) {
			ret = rtd1296_sata_write(controller, setting->reg, generation,
						 setting->generation[generation]);
			if (ret)
				goto disable;
		}
	}
	value = readl(controller->base + SATA_RATE);
	writel((value & ~GENMASK(3, 0)) | 0xa, controller->base + SATA_RATE);
	readl(controller->base + SATA_RATE);
	controller->initialized |= BIT(port->index);
	goto unlock;

disable:
	if (!controller->initialized)
		rtd1296_sata_clocks_off(controller);
unlock:
	mutex_unlock(&controller->lock);
	return ret;
}

static int rtd1296_sata_power_on(struct phy *phy)
{
	struct rtd1296_sata_port *port = phy_get_drvdata(phy);
	struct rtd1296_sata *controller = port->controller;
	int ret = -EINVAL;

	mutex_lock(&controller->lock);
	if (controller->initialized & BIT(port->index))
		ret = reset_control_deassert(port->power);
	mutex_unlock(&controller->lock);
	return ret;
}

static int rtd1296_sata_power_off(struct phy *phy)
{
	struct rtd1296_sata_port *port = phy_get_drvdata(phy);
	struct rtd1296_sata *controller = port->controller;
	int ret;

	mutex_lock(&controller->lock);
	ret = reset_control_assert(port->power);
	mutex_unlock(&controller->lock);
	return ret;
}

static int rtd1296_sata_exit(struct phy *phy)
{
	struct rtd1296_sata_port *port = phy_get_drvdata(phy);
	struct rtd1296_sata *controller = port->controller;
	int ret;

	mutex_lock(&controller->lock);
	ret = reset_control_assert(port->power);
	if (!ret) {
		controller->initialized &= ~BIT(port->index);
		if (!controller->initialized)
			rtd1296_sata_clocks_off(controller);
	}
	mutex_unlock(&controller->lock);
	return ret;
}

static const struct phy_ops rtd1296_sata_ops = {
	.init = rtd1296_sata_init,
	.exit = rtd1296_sata_exit,
	.power_on = rtd1296_sata_power_on,
	.power_off = rtd1296_sata_power_off,
	.owner = THIS_MODULE,
};

static struct phy *rtd1296_sata_xlate(struct device *dev, const struct of_phandle_args *args)
{
	struct rtd1296_sata *controller = dev_get_drvdata(dev);

	if (args->args_count != 1 || args->args[0] >= SATA_PORTS)
		return ERR_PTR(-EINVAL);
	return controller->ports[args->args[0]].phy;
}

static void rtd1296_sata_release(void *data)
{
	struct rtd1296_sata *controller = data;
	unsigned int i;
	int ret;

	mutex_lock(&controller->lock);
	for (i = 0; i < SATA_PORTS; i++) {
		ret = reset_control_assert(controller->ports[i].power);
		if (ret)
			dev_err(controller->dev, "Cannot assert PHY%u reset: %d\n", i, ret);
	}
	rtd1296_sata_clocks_off(controller);
	ret = reset_control_assert(controller->serdes);
	if (ret)
		dev_err(controller->dev, "Cannot release SerDes reset reference: %d\n", ret);
	mutex_unlock(&controller->lock);
}

static int rtd1296_sata_probe(struct platform_device *pdev)
{
	static const char * const resets[] = { "port0", "port1" };
	struct device *dev = &pdev->dev;
	struct rtd1296_sata *controller;
	struct phy_provider *provider;
	unsigned int i;
	int ret;

	controller = devm_kzalloc(dev, sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return -ENOMEM;
	controller->dev = dev;
	mutex_init(&controller->lock);
	controller->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(controller->base))
		return PTR_ERR(controller->base);
	controller->sb2 = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sb2");
	if (IS_ERR(controller->sb2))
		return dev_err_probe(dev, PTR_ERR(controller->sb2), "Cannot get OOB control\n");
	controller->clocks[0].id = "port0";
	controller->clocks[1].id = "port1";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(controller->clocks), controller->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get PHY clocks\n");
	controller->serdes = devm_reset_control_get_shared(dev, "serdes");
	if (IS_ERR(controller->serdes))
		return dev_err_probe(dev, PTR_ERR(controller->serdes), "Cannot get SerDes reset\n");
	for (i = 0; i < SATA_PORTS; i++) {
		controller->ports[i].controller = controller;
		controller->ports[i].index = i;
		controller->ports[i].power = devm_reset_control_get_exclusive(dev, resets[i]);
		if (IS_ERR(controller->ports[i].power))
			return dev_err_probe(dev, PTR_ERR(controller->ports[i].power),
					     "Cannot get PHY%u reset\n", i);
	}
	ret = reset_control_deassert(controller->serdes);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, rtd1296_sata_release, controller);
	if (ret)
		return ret;
	for (i = 0; i < SATA_PORTS; i++) {
		controller->ports[i].phy = devm_phy_create(dev, NULL, &rtd1296_sata_ops);
		if (IS_ERR(controller->ports[i].phy))
			return PTR_ERR(controller->ports[i].phy);
		phy_set_drvdata(controller->ports[i].phy, &controller->ports[i]);
	}
	platform_set_drvdata(pdev, controller);
	provider = devm_of_phy_provider_register(dev, rtd1296_sata_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id rtd1296_sata_match[] = {
	{ .compatible = "realtek,rtd1296-sata-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_sata_match);

static struct platform_driver rtd1296_sata_driver = {
	.probe = rtd1296_sata_probe,
	.driver = {
		.name = "rtd1296-sata-phy",
		.of_match_table = rtd1296_sata_match,
	},
};
module_platform_driver(rtd1296_sata_driver);

MODULE_DESCRIPTION("Realtek RTD1296 SATA PHY");
MODULE_LICENSE("GPL");
