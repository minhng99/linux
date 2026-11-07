// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>

#define CRT_POWER_CONTROL	0x400
#define NAT_ISOLATION		BIT(18)
#define NAT_IRQ_MASK		BIT(19)
#define NAT_SRAM_MUX		0x42c
#define NAT_SRAM_MANUAL		BIT(0)
#define NAT_SRAM_CONTROL		0x430
#define NAT_SRAM_REQUEST		0xf00
#define NAT_SRAM_OFF		BIT(0)
#define NAT_SRAM_STATUS		0x434
#define NAT_SRAM_DONE		BIT(2)

struct rtd1296_nat_power {
	struct generic_pm_domain domain;
	struct device *dev;
	struct regmap *crt;
	bool failed;
};

static int rtd1296_nat_command(struct rtd1296_nat_power *power, bool on)
{
	u32 status;
	int ret;

	ret = regmap_write(power->crt, NAT_SRAM_STATUS, NAT_SRAM_DONE);
	if (ret)
		return ret;
	ret = regmap_read(power->crt, NAT_SRAM_STATUS, &status);
	if (ret)
		return ret;
	if (status & NAT_SRAM_DONE)
		return -EIO;
	ret = regmap_write(power->crt, NAT_SRAM_CONTROL,
			   NAT_SRAM_REQUEST | (on ? 0 : NAT_SRAM_OFF));
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(power->crt, NAT_SRAM_STATUS, status,
				       status & NAT_SRAM_DONE, 1, 10000);
	if (ret)
		return ret;
	return regmap_write(power->crt, NAT_SRAM_STATUS, NAT_SRAM_DONE);
}

static int rtd1296_nat_isolate(struct rtd1296_nat_power *power, bool isolate)
{
	u32 value;
	int ret;

	ret = regmap_update_bits(power->crt, CRT_POWER_CONTROL, NAT_ISOLATION,
				 isolate ? NAT_ISOLATION : 0);
	if (ret)
		return ret;
	ret = regmap_read(power->crt, CRT_POWER_CONTROL, &value);
	if (ret)
		return ret;
	return !!(value & NAT_ISOLATION) == isolate ? 0 : -EIO;
}

static int rtd1296_nat_set_power(struct generic_pm_domain *domain, bool on)
{
	struct rtd1296_nat_power *power = container_of(domain, struct rtd1296_nat_power,
						    domain);
	u32 isolation, control, mux;
	int ret, restore;

	ret = regmap_read(power->crt, CRT_POWER_CONTROL, &isolation);
	if (ret)
		return ret;
	ret = regmap_read(power->crt, NAT_SRAM_CONTROL, &control);
	if (ret)
		return ret;
	ret = regmap_read(power->crt, NAT_SRAM_MUX, &mux);
	if (ret)
		return ret;
	if (!on) {
		ret = rtd1296_nat_isolate(power, true);
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(power->crt, NAT_SRAM_MUX, NAT_SRAM_MANUAL, 0);
	if (ret)
		goto rollback;
	ret = rtd1296_nat_command(power, on);
	if (ret)
		goto rollback;
	if (on) {
		ret = rtd1296_nat_isolate(power, false);
		if (ret)
			goto rollback;
	}
	WRITE_ONCE(power->failed, false);
	return 0;

rollback:
	restore = rtd1296_nat_command(power, !(control & NAT_SRAM_OFF));
	if (!restore)
		restore = regmap_write(power->crt, NAT_SRAM_CONTROL, control);
	if (!restore)
		restore = regmap_write(power->crt, NAT_SRAM_MUX, mux);
	if (!restore)
		restore = rtd1296_nat_isolate(power, isolation & NAT_ISOLATION);
	if (restore) {
		rtd1296_nat_isolate(power, true);
		WRITE_ONCE(power->failed, true);
		dev_err(power->dev, "SRAM power recovery failed: %d\n", restore);
	}
	return ret;
}

static int rtd1296_nat_power_on(struct generic_pm_domain *domain)
{
	return rtd1296_nat_set_power(domain, true);
}

static int rtd1296_nat_power_off(struct generic_pm_domain *domain)
{
	return rtd1296_nat_set_power(domain, false);
}

static int rtd1296_nat_start(struct device *dev)
{
	struct generic_pm_domain *domain = pd_to_genpd(dev->pm_domain);
	struct rtd1296_nat_power *power = container_of(domain, struct rtd1296_nat_power,
						    domain);

	return READ_ONCE(power->failed) ? -EIO : 0;
}

static int rtd1296_nat_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd1296_nat_power *power;
	u32 isolation, control;
	int ret;

	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = dev;
	power->crt = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(power->crt))
		return dev_err_probe(dev, PTR_ERR(power->crt), "Cannot get CRT registers\n");
	ret = regmap_update_bits(power->crt, CRT_POWER_CONTROL, NAT_IRQ_MASK, NAT_IRQ_MASK);
	if (ret)
		return ret;
	ret = regmap_read(power->crt, CRT_POWER_CONTROL, &isolation);
	if (ret)
		return ret;
	ret = regmap_read(power->crt, NAT_SRAM_CONTROL, &control);
	if (ret)
		return ret;
	power->domain.name = "nat-sram";
	power->domain.power_on = rtd1296_nat_power_on;
	power->domain.power_off = rtd1296_nat_power_off;
	power->domain.dev_ops.start = rtd1296_nat_start;
	ret = pm_genpd_init(&power->domain, NULL,
			    (isolation & NAT_ISOLATION) || (control & NAT_SRAM_OFF));
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(dev->of_node, &power->domain);
	if (ret)
		pm_genpd_remove(&power->domain);
	return ret;
}

static const struct of_device_id rtd1296_nat_power_match[] = {
	{ .compatible = "realtek,rtd1296-nat-sram-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_nat_power_match);

static struct platform_driver rtd1296_nat_power_driver = {
	.probe = rtd1296_nat_power_probe,
	.driver = {
		.name = "rtd1296-nat-sram-power",
		.of_match_table = rtd1296_nat_power_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rtd1296_nat_power_driver);

MODULE_DESCRIPTION("Realtek RTD1296 Ethernet switch SRAM power domain");
MODULE_LICENSE("GPL");
