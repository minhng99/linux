// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define TVE_DAC_CONTROL	0x3a0
#define TVE_DAC_ENABLE	BIT(31)

struct rtd1296_tve_power {
	struct generic_pm_domain domain;
	struct device *dev;
	struct regmap *regmap;
	struct clk *bus_clock;
	struct reset_control *reset;
	struct reset_control *domain_reset;
	bool failed;
};

static int tve_read_control(struct rtd1296_tve_power *power, u32 *value)
{
	int ret;

	ret = regmap_read(power->regmap, TVE_DAC_CONTROL, value);
	if (ret)
		return ret;
	return *value == 0xdeadbeef ? -EIO : 0;
}

static int tve_set_enable(struct rtd1296_tve_power *power, bool enable)
{
	u32 value;
	int ret;

	ret = tve_read_control(power, &value);
	if (ret)
		return ret;
	ret = regmap_update_bits(power->regmap, TVE_DAC_CONTROL, TVE_DAC_ENABLE,
				 enable ? TVE_DAC_ENABLE : 0);
	if (ret)
		return ret;
	ret = tve_read_control(power, &value);
	if (ret)
		return ret;
	return !!(value & TVE_DAC_ENABLE) == enable ? 0 : -EIO;
}

static int tve_prepare_access(struct rtd1296_tve_power *power)
{
	int ret;

	ret = clk_prepare_enable(power->bus_clock);
	if (ret)
		return ret;
	ret = reset_control_deassert(power->domain_reset);
	if (ret)
		goto clock;
	ret = reset_control_deassert(power->reset);
	if (ret)
		goto domain;
	usleep_range(1000, 2000);
	return 0;

domain:
	reset_control_assert(power->domain_reset);
clock:
	clk_disable_unprepare(power->bus_clock);
	return ret;
}

static int tve_release_resets(struct rtd1296_tve_power *power)
{
	int ret;

	ret = reset_control_assert(power->reset);
	if (ret)
		return ret;
	return reset_control_assert(power->domain_reset);
}

static int tve_power_on(struct generic_pm_domain *domain)
{
	struct rtd1296_tve_power *power = container_of(domain,
					struct rtd1296_tve_power, domain);
	int ret, restore, reset;

	if (READ_ONCE(power->failed))
		return -EIO;
	ret = tve_prepare_access(power);
	if (ret)
		return ret;
	ret = tve_set_enable(power, true);
	if (ret) {
		restore = tve_set_enable(power, false);
		reset = tve_release_resets(power);
		if (restore || reset)
			WRITE_ONCE(power->failed, true);
	}
	clk_disable_unprepare(power->bus_clock);
	return ret;
}

static int tve_power_off(struct generic_pm_domain *domain)
{
	struct rtd1296_tve_power *power = container_of(domain,
					struct rtd1296_tve_power, domain);
	int ret;

	ret = clk_prepare_enable(power->bus_clock);
	if (ret)
		return ret;
	ret = tve_set_enable(power, false);
	if (!ret)
		ret = tve_release_resets(power);
	if (ret) {
		WRITE_ONCE(power->failed, true);
		dev_err(power->dev, "analogue power-off failed: %d\n", ret);
	}
	clk_disable_unprepare(power->bus_clock);
	return ret;
}

static int tve_consumer_start(struct device *dev)
{
	struct generic_pm_domain *domain = pd_to_genpd(dev->pm_domain);
	struct rtd1296_tve_power *power = container_of(domain,
					struct rtd1296_tve_power, domain);

	return READ_ONCE(power->failed) ? -EIO : 0;
}

static int tve_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd1296_tve_power *power;
	u32 value;
	bool off;
	int ret, release;

	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = dev;
	power->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(power->regmap))
		return dev_err_probe(dev, PTR_ERR(power->regmap),
				     "cannot get TVE registers\n");
	power->bus_clock = devm_clk_get(dev, NULL);
	if (IS_ERR(power->bus_clock))
		return dev_err_probe(dev, PTR_ERR(power->bus_clock),
				     "cannot get register clock\n");
	power->reset = devm_reset_control_get_shared(dev, "tve");
	if (IS_ERR(power->reset))
		return dev_err_probe(dev, PTR_ERR(power->reset), "cannot get TVE reset\n");
	power->domain_reset = devm_reset_control_get_shared(dev, "domain");
	if (IS_ERR(power->domain_reset))
		return dev_err_probe(dev, PTR_ERR(power->domain_reset),
				     "cannot get domain reset\n");
	ret = tve_prepare_access(power);
	if (ret)
		return ret;
	ret = tve_read_control(power, &value);
	off = ret || !(value & TVE_DAC_ENABLE);
	if (off) {
		release = tve_release_resets(power);
		if (!ret)
			ret = release;
	}
	clk_disable_unprepare(power->bus_clock);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read analogue control\n");
	power->domain.name = "tve-analog";
	power->domain.power_on = tve_power_on;
	power->domain.power_off = tve_power_off;
	power->domain.dev_ops.start = tve_consumer_start;
	ret = pm_genpd_init(&power->domain, NULL, off);
	if (ret)
		goto release;
	ret = of_genpd_add_provider_simple(dev->of_node, &power->domain);
	if (ret)
		pm_genpd_remove(&power->domain);
	if (!ret)
		return 0;
release:
	if (!off)
		tve_power_off(&power->domain);
	return ret;
}

static const struct of_device_id tve_power_match[] = {
	{ .compatible = "realtek,rtd1296-tve-analog-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, tve_power_match);

static struct platform_driver tve_power_driver = {
	.probe = tve_power_probe,
	.driver = {
		.name = "rtd1296-tve-analog-power",
		.of_match_table = tve_power_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(tve_power_driver);

MODULE_DESCRIPTION("Realtek RTD1296 TVE analogue power domain");
MODULE_LICENSE("GPL");
