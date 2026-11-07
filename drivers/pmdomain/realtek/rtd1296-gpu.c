// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define GPU_SRAM_MUX		0x3a0
#define GPU_SRAM_MANUAL		GENMASK(3, 0)
#define GPU_SRAM_CONTROL		0x3a4
#define GPU_SRAM_REQUEST		GENMASK(9, 8)
#define GPU_SRAM_OFF		BIT(0)
#define GPU_SRAM_STATUS		0x3ac
#define GPU_SRAM_DONE		BIT(2)
#define GPU_REPAIR_CLOCK		0x00
#define GPU_REPAIR_STATUS	0x20
#define GPU_REPAIR_FAILURE	0x44
#define GPU_REPAIR_CONTROL	0x9c
#define GPU_REPAIR_L2		BIT(5)

struct rtd1296_gpu_power {
	struct generic_pm_domain domain;
	struct device *dev;
	struct regmap *crt;
	void __iomem *repair;
	struct clk *clock;
	struct reset_control *reset;
	bool failed;
};

static int gpu_sram_power(struct rtd1296_gpu_power *power, bool on)
{
	u32 status;
	int ret;

	ret = regmap_write(power->crt, GPU_SRAM_STATUS, GPU_SRAM_DONE);
	if (ret)
		return ret;
	ret = regmap_read(power->crt, GPU_SRAM_STATUS, &status);
	if (ret)
		return ret;
	if (status & GPU_SRAM_DONE)
		return -EIO;
	ret = regmap_write(power->crt, GPU_SRAM_CONTROL,
			   GPU_SRAM_REQUEST | (on ? 0 : GPU_SRAM_OFF));
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(power->crt, GPU_SRAM_STATUS, status,
				       status & GPU_SRAM_DONE, 1, 10000);
	if (ret)
		return ret;
	return regmap_write(power->crt, GPU_SRAM_STATUS, GPU_SRAM_DONE);
}

static int gpu_repair_memory(struct rtd1296_gpu_power *power)
{
	u32 status;
	int ret;

	writel(GPU_REPAIR_L2, power->repair + GPU_REPAIR_CLOCK);
	writel(1, power->repair + GPU_REPAIR_CONTROL);
	writel(3, power->repair + GPU_REPAIR_CONTROL);
	ret = readl_poll_timeout(power->repair + GPU_REPAIR_STATUS, status,
				 status & GPU_REPAIR_L2, 1, 10000);
	if (!ret && readl(power->repair + GPU_REPAIR_FAILURE))
		ret = -EIO;
	if (!ret)
		writel(0xf, power->repair + GPU_REPAIR_CONTROL);
	writel(0, power->repair + GPU_REPAIR_CLOCK);
	readl(power->repair + GPU_REPAIR_CLOCK);
	return ret;
}

static int gpu_restore_power(struct rtd1296_gpu_power *power)
{
	int ret;

	ret = gpu_sram_power(power, true);
	if (ret)
		return ret;
	ret = reset_control_deassert(power->reset);
	if (ret)
		return ret;
	return gpu_repair_memory(power);
}

static int rtd1296_gpu_power_on(struct generic_pm_domain *domain)
{
	struct rtd1296_gpu_power *power = container_of(domain,
					struct rtd1296_gpu_power, domain);
	int ret, reset, restore;

	if (READ_ONCE(power->failed))
		return -EIO;
	ret = clk_prepare_enable(power->clock);
	if (ret)
		return ret;
	ret = gpu_restore_power(power);
	if (!ret)
		return 0;
	reset = reset_control_assert(power->reset);
	restore = gpu_sram_power(power, false);
	if (reset || restore) {
		WRITE_ONCE(power->failed, true);
		dev_err(power->dev, "GPU power-on recovery failed\n");
	}
	clk_disable_unprepare(power->clock);
	return ret;
}

static int rtd1296_gpu_power_off(struct generic_pm_domain *domain)
{
	struct rtd1296_gpu_power *power = container_of(domain,
					struct rtd1296_gpu_power, domain);
	int ret, restore;

	ret = reset_control_assert(power->reset);
	if (ret)
		return ret;
	ret = gpu_sram_power(power, false);
	if (!ret) {
		clk_disable_unprepare(power->clock);
		return 0;
	}
	restore = gpu_restore_power(power);
	if (restore) {
		reset_control_assert(power->reset);
		WRITE_ONCE(power->failed, true);
		dev_err(power->dev, "GPU power-off recovery failed: %d\n", restore);
	}
	return ret;
}

static int rtd1296_gpu_start(struct device *dev)
{
	struct rtd1296_gpu_power *power = container_of(pd_to_genpd(dev->pm_domain),
					struct rtd1296_gpu_power, domain);

	return READ_ONCE(power->failed) ? -EIO : 0;
}

static int rtd1296_gpu_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd1296_gpu_power *power;
	int ret;

	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = dev;
	power->crt = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,crt");
	if (IS_ERR(power->crt))
		return dev_err_probe(dev, PTR_ERR(power->crt), "cannot get CRT registers\n");
	power->repair = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(power->repair))
		return PTR_ERR(power->repair);
	power->clock = devm_clk_get(dev, NULL);
	if (IS_ERR(power->clock))
		return dev_err_probe(dev, PTR_ERR(power->clock), "cannot get GPU clock\n");
	power->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(power->reset))
		return dev_err_probe(dev, PTR_ERR(power->reset), "cannot get GPU reset\n");
	ret = reset_control_assert(power->reset);
	if (ret)
		return ret;
	ret = regmap_clear_bits(power->crt, GPU_SRAM_MUX, GPU_SRAM_MANUAL);
	if (ret)
		return ret;
	ret = gpu_sram_power(power, false);
	if (ret)
		return dev_err_probe(dev, ret, "cannot initialize GPU SRAM power\n");
	power->domain.name = "gpu";
	power->domain.power_on = rtd1296_gpu_power_on;
	power->domain.power_off = rtd1296_gpu_power_off;
	power->domain.dev_ops.start = rtd1296_gpu_start;
	ret = pm_genpd_init(&power->domain, NULL, true);
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(dev->of_node, &power->domain);
	if (ret)
		pm_genpd_remove(&power->domain);
	return ret;
}

static const struct of_device_id rtd1296_gpu_power_match[] = {
	{ .compatible = "realtek,rtd1296-gpu-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_gpu_power_match);

static struct platform_driver rtd1296_gpu_power_driver = {
	.probe = rtd1296_gpu_power_probe,
	.driver = {
		.name = "rtd1296-gpu-power",
		.of_match_table = rtd1296_gpu_power_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rtd1296_gpu_power_driver);

MODULE_DESCRIPTION("Realtek RTD1296 GPU power domain");
MODULE_LICENSE("GPL");
