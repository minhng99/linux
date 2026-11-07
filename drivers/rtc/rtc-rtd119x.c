// SPDX-License-Identifier: GPL-2.0+
/*
 * Realtek RTD129x RTC
 * Copyright (c) 2017 Andreas Färber
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/regmap.h>
#include <linux/rtc.h>

#define RTD_RTCSEC		0x00
#define RTD_RTCMIN		0x04
#define RTD_ALARMMIN		0x14
#define RTD_RTCACR		0x28
#define RTD_RTCEN		0x2c
#define RTD_RTCCR		0x30
#define RTD_RTCPWR		BIT(7)
#define RTD_RTCRST		BIT(6)
#define RTD_ISO_ISR		0x00
#define RTD_ISO_RTC		0x34
#define RTD_ALARM_ENABLE	BIT(0)
#define RTD_ALARM_PENDING	BIT(13)
#define RTD_TIME_DAYS		GENMASK(14, 0)
#define RTD_ALARM_DAYS		GENMASK(13, 0)

struct rtd119x_rtc {
	void __iomem *base;
	struct regmap *iso;
	struct rtc_device *rtcdev;
	time64_t epoch;
	/* Protects calendar updates and alarm state against the interrupt handler. */
	struct mutex lock;
};

static int rtd119x_read_calendar(struct rtd119x_rtc *data, unsigned int offset,
				 unsigned int day_mask, unsigned int seconds,
				 struct rtc_time *tm)
{
	unsigned int minute = readl(data->base + offset) & 0x3f;
	unsigned int hour = readl(data->base + offset + 4) & 0x1f;
	unsigned int day = readl(data->base + offset + 8) & 0xff;

	day |= (readl(data->base + offset + 12) << 8) & day_mask;
	if (seconds > 59 || minute > 59 || hour > 23)
		return -EINVAL;
	rtc_time64_to_tm(data->epoch + (time64_t)day * 86400 +
			 hour * 3600 + minute * 60 + seconds, tm);
	return 0;
}

static void rtd119x_write_calendar(struct rtd119x_rtc *data, unsigned int offset,
				   time64_t elapsed, const struct rtc_time *tm)
{
	unsigned int day = div_s64(elapsed, 86400);

	writel(tm->tm_min, data->base + offset);
	writel(tm->tm_hour, data->base + offset + 4);
	writel(day & 0xff, data->base + offset + 8);
	writel(day >> 8, data->base + offset + 12);
}

static int rtd119x_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct rtd119x_rtc *data = dev_get_drvdata(dev);
	unsigned int seconds;
	int ret = -EAGAIN;

	mutex_lock(&data->lock);
	for (int tries = 0; tries < 3; tries++) {
		seconds = (readl(data->base + RTD_RTCSEC) & 0x7f) >> 1;
		ret = rtd119x_read_calendar(data, RTD_RTCMIN, RTD_TIME_DAYS, seconds, tm);
		if (ret || seconds == ((readl(data->base + RTD_RTCSEC) & 0x7f) >> 1))
			break;
		ret = -EAGAIN;
	}
	mutex_unlock(&data->lock);
	return ret;
}

static int rtd119x_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct rtd119x_rtc *data = dev_get_drvdata(dev);
	time64_t elapsed = rtc_tm_to_time64(tm) - data->epoch;

	if (elapsed < 0 || elapsed / 86400 > RTD_TIME_DAYS)
		return -EINVAL;
	mutex_lock(&data->lock);
	writel(0, data->base + RTD_RTCEN);
	writel(tm->tm_sec << 1, data->base + RTD_RTCSEC);
	rtd119x_write_calendar(data, RTD_RTCMIN, elapsed, tm);
	writel(0x5a, data->base + RTD_RTCEN);
	mutex_unlock(&data->lock);
	return 0;
}

static int rtd119x_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct rtd119x_rtc *data = dev_get_drvdata(dev);
	unsigned int enable, pending;
	int ret;

	mutex_lock(&data->lock);
	ret = rtd119x_read_calendar(data, RTD_ALARMMIN, RTD_ALARM_DAYS, 0, &alarm->time);
	if (!ret)
		ret = regmap_read(data->iso, RTD_ISO_RTC, &enable);
	if (!ret)
		ret = regmap_read(data->iso, RTD_ISO_ISR, &pending);
	if (!ret) {
		alarm->enabled = !!(enable & RTD_ALARM_ENABLE);
		alarm->pending = !!(pending & RTD_ALARM_PENDING);
	}
	mutex_unlock(&data->lock);
	return ret;
}

static int rtd119x_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct rtd119x_rtc *data = dev_get_drvdata(dev);
	time64_t elapsed = rtc_tm_to_time64(&alarm->time) - data->epoch;
	int ret;

	if (elapsed < 0 || elapsed / 86400 > RTD_ALARM_DAYS || alarm->time.tm_sec)
		return -EINVAL;
	mutex_lock(&data->lock);
	ret = regmap_clear_bits(data->iso, RTD_ISO_RTC, RTD_ALARM_ENABLE);
	if (ret)
		goto out;
	rtd119x_write_calendar(data, RTD_ALARMMIN, elapsed, &alarm->time);
	ret = regmap_write(data->iso, RTD_ISO_ISR, RTD_ALARM_PENDING);
	if (!ret && alarm->enabled)
		ret = regmap_set_bits(data->iso, RTD_ISO_RTC, RTD_ALARM_ENABLE);
out:
	mutex_unlock(&data->lock);
	return ret;
}

static int rtd119x_rtc_alarm_enable(struct device *dev, unsigned int enable)
{
	struct rtd119x_rtc *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	ret = regmap_update_bits(data->iso, RTD_ISO_RTC, RTD_ALARM_ENABLE,
				 enable ? RTD_ALARM_ENABLE : 0);
	mutex_unlock(&data->lock);
	return ret;
}

static irqreturn_t rtd119x_rtc_irq(int irq, void *arg)
{
	struct rtd119x_rtc *data = arg;

	mutex_lock(&data->lock);
	regmap_clear_bits(data->iso, RTD_ISO_RTC, RTD_ALARM_ENABLE);
	mutex_unlock(&data->lock);
	rtc_update_irq(data->rtcdev, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}

static const struct rtc_class_ops rtd119x_time_ops = {
	.read_time = rtd119x_rtc_read_time,
	.set_time = rtd119x_rtc_set_time,
};

static const struct rtc_class_ops rtd119x_alarm_ops = {
	.read_time = rtd119x_rtc_read_time,
	.set_time = rtd119x_rtc_set_time,
	.read_alarm = rtd119x_rtc_read_alarm,
	.set_alarm = rtd119x_rtc_set_alarm,
	.alarm_irq_enable = rtd119x_rtc_alarm_enable,
};

static void rtd119x_rtc_wakeup_disable(void *arg)
{
	device_init_wakeup(arg, false);
}

static int rtd119x_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd119x_rtc *data;
	struct clk *clk;
	struct rtc_time initial;
	unsigned int value;
	int irq, ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot enable RTC clock\n");
	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;
	data->epoch = mktime64(2014, 1, 1, 0, 0, 0);
	platform_set_drvdata(pdev, data);
	value = readl(data->base + RTD_RTCACR);
	if (!(value & RTD_RTCPWR)) {
		writel(value | RTD_RTCPWR, data->base + RTD_RTCACR);
		value = readl(data->base + RTD_RTCCR);
		writel(value | RTD_RTCRST, data->base + RTD_RTCCR);
		writel(value & ~RTD_RTCRST, data->base + RTD_RTCCR);
		rtc_time64_to_tm(data->epoch, &initial);
		rtd119x_rtc_set_time(dev, &initial);
	}
	if ((readl(data->base + RTD_RTCEN) & 0xff) != 0x5a)
		writel(0x5a, data->base + RTD_RTCEN);

	data->rtcdev = devm_rtc_allocate_device(dev);
	if (IS_ERR(data->rtcdev))
		return PTR_ERR(data->rtcdev);
	data->rtcdev->ops = &rtd119x_time_ops;
	data->rtcdev->range_min = data->epoch;
	data->rtcdev->range_max = data->epoch + ((time64_t)RTD_TIME_DAYS + 1) * 86400 - 1;
	irq = platform_get_irq_optional(pdev, 0);
	if (irq == -EPROBE_DEFER)
		return irq;
	if (irq > 0) {
		data->iso = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,iso-syscon");
		if (IS_ERR(data->iso))
			return dev_err_probe(dev, PTR_ERR(data->iso), "cannot get alarm control\n");
		ret = regmap_clear_bits(data->iso, RTD_ISO_RTC, RTD_ALARM_ENABLE);
		if (ret)
			return ret;
		ret = devm_request_threaded_irq(dev, irq, NULL, rtd119x_rtc_irq,
						IRQF_ONESHOT, dev_name(dev), data);
		if (ret)
			return dev_err_probe(dev, ret, "cannot request alarm IRQ\n");
		ret = device_init_wakeup(dev, true);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(dev, rtd119x_rtc_wakeup_disable, dev);
		if (ret)
			return ret;
		ret = devm_pm_set_wake_irq(dev, irq);
		if (ret)
			return ret;
		data->rtcdev->ops = &rtd119x_alarm_ops;
		set_bit(RTC_FEATURE_ALARM_RES_MINUTE, data->rtcdev->features);
	}
	return devm_rtc_register_device(data->rtcdev);
}

static const struct of_device_id rtd119x_rtc_dt_ids[] = {
	{ .compatible = "realtek,rtd1295-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd119x_rtc_dt_ids);

static struct platform_driver rtd119x_rtc_driver = {
	.probe = rtd119x_rtc_probe,
	.driver = {
		.name = "rtd1295-rtc",
		.of_match_table = rtd119x_rtc_dt_ids,
	},
};
builtin_platform_driver(rtd119x_rtc_driver);
