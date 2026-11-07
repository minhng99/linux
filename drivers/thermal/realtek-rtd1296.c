// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/thermal.h>

#define RTD_TM_CONTROL		0x08
#define RTD_TM_TEMPERATURE	0x18
#define RTD_TM_STATUS		0x1c
#define RTD_TM_RESET_N		BIT(17)
#define RTD_TM_CONFIG		0x01904001
#define RTD_TM_STATUS_MASK	GENMASK(21, 0)
#define RTD_TM_VALUE_MASK		GENMASK(18, 0)

struct rtd_thermal {
	void __iomem *base;
	/* Serializes sampling with sensor reset and resume. */
	struct mutex lock;
};

static bool rtd_thermal_valid(u32 status)
{
	status &= RTD_TM_STATUS_MASK;
	return status && status != RTD_TM_STATUS_MASK;
}

static int rtd_thermal_start(struct rtd_thermal *sensor)
{
	u32 status;

	writel(RTD_TM_CONFIG, sensor->base + RTD_TM_CONTROL);
	writel(RTD_TM_CONFIG | RTD_TM_RESET_N,
	       sensor->base + RTD_TM_CONTROL);
	usleep_range(25000, 26000);
	return readl_poll_timeout(sensor->base + RTD_TM_STATUS, status,
				  rtd_thermal_valid(status), 100, 25000);
}

static int rtd_thermal_sample(struct rtd_thermal *sensor, int *temperature)
{
	u32 raw;
	int value;

	if (!rtd_thermal_valid(readl(sensor->base + RTD_TM_STATUS)))
		return -EAGAIN;
	raw = readl(sensor->base + RTD_TM_TEMPERATURE) & RTD_TM_VALUE_MASK;
	value = sign_extend32(raw, 18) * 1000 / 1024;
	if (value < -40000 || value > 150000)
		return -ERANGE;
	*temperature = value;
	return 0;
}

static int rtd_thermal_get_temp(struct thermal_zone_device *zone, int *temperature)
{
	struct rtd_thermal *sensor = thermal_zone_device_priv(zone);
	int ret;

	mutex_lock(&sensor->lock);
	ret = rtd_thermal_sample(sensor, temperature);
	if (ret == -EAGAIN) {
		ret = rtd_thermal_start(sensor);
		if (!ret)
			ret = rtd_thermal_sample(sensor, temperature);
	}
	mutex_unlock(&sensor->lock);
	return ret;
}

static const struct thermal_zone_device_ops rtd_thermal_ops = {
	.get_temp = rtd_thermal_get_temp,
};

static int rtd_thermal_resume(struct device *dev)
{
	struct rtd_thermal *sensor = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&sensor->lock);
	ret = rtd_thermal_start(sensor);
	mutex_unlock(&sensor->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rtd_thermal_pm, NULL, rtd_thermal_resume);

static int rtd_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *zone;
	struct rtd_thermal *sensor;
	struct resource *resource;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->base = devm_platform_get_and_ioremap_resource(pdev, 0, &resource);
	if (IS_ERR(sensor->base))
		return PTR_ERR(sensor->base);
	if (resource_size(resource) < RTD_TM_STATUS + sizeof(u32))
		return -EINVAL;
	ret = devm_mutex_init(dev, &sensor->lock);
	if (ret)
		return ret;
	ret = rtd_thermal_start(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "sensor did not become ready\n");
	zone = devm_thermal_of_zone_register(dev, 0, sensor, &rtd_thermal_ops);
	if (IS_ERR(zone))
		return dev_err_probe(dev, PTR_ERR(zone), "cannot register thermal zone\n");
	platform_set_drvdata(pdev, sensor);
	return 0;
}

static const struct of_device_id rtd_thermal_match[] = {
	{ .compatible = "realtek,rtd1296-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd_thermal_match);

static struct platform_driver rtd_thermal_driver = {
	.probe = rtd_thermal_probe,
	.driver = {
		.name = "rtd1296-thermal",
		.of_match_table = rtd_thermal_match,
		.pm = pm_sleep_ptr(&rtd_thermal_pm),
	},
};
module_platform_driver(rtd_thermal_driver);

MODULE_DESCRIPTION("Realtek RTD1296 thermal sensor");
MODULE_AUTHOR("Nguyen Quang Minh <oss@minhng99.eu>");
MODULE_LICENSE("GPL");
