// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "ohci.h"
#include "rtd1296-usb.h"

static struct hc_driver rtd1296_ohci_hc_driver __read_mostly;
static int (*rtd1296_ohci_bus_resume)(struct usb_hcd *hcd);

static int rtd1296_ohci_wake(struct usb_hcd *hcd)
{
	u32 control;
	int ret;

	ret = rtd1296_usb2_wake(hcd->self.controller->parent);
	if (ret)
		return ret;
	return readl_poll_timeout(hcd->regs + 0x04, control,
				  !(control & ~GENMASK(10, 0)), 10, 10000);
}

static int rtd1296_ohci_resume_bus(struct usb_hcd *hcd)
{
	int ret;

	ret = rtd1296_ohci_wake(hcd);
	if (ret)
		return ret;
	return rtd1296_ohci_bus_resume(hcd);
}

static int rtd1296_ohci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct usb_hcd *hcd;
	struct resource *res;
	int ret, irq;

	if (usb_disabled())
		return -ENODEV;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	hcd = usb_create_hcd(&rtd1296_ohci_hc_driver, dev, dev_name(dev));
	if (!hcd)
		return -ENOMEM;
	hcd->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hcd->regs)) {
		ret = PTR_ERR(hcd->regs);
		goto put;
	}
	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);
	ret = rtd1296_ohci_wake(hcd);
	if (ret) {
		dev_err_probe(dev, ret, "Cannot wake USB2 host clocks\n");
		goto put;
	}
	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret)
		goto put;
	platform_set_drvdata(pdev, hcd);
	device_wakeup_enable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	return 0;

put:
	usb_put_hcd(hcd);
	return ret;
}

static void rtd1296_ohci_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);
	int ret;

	pm_runtime_disable(&pdev->dev);
	ret = rtd1296_usb2_wake(pdev->dev.parent);
	if (ret)
		dev_warn(&pdev->dev, "Cannot wake USB2 host clocks: %d\n", ret);
	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);
}

static void rtd1296_ohci_shutdown(struct platform_device *pdev)
{
	int ret;

	ret = rtd1296_usb2_wake(pdev->dev.parent);
	if (ret)
		dev_warn(&pdev->dev, "Cannot wake USB2 host clocks: %d\n", ret);
	usb_hcd_platform_shutdown(pdev);
}

static int __maybe_unused rtd1296_ohci_suspend(struct device *dev)
{
	return ohci_suspend(dev_get_drvdata(dev), device_may_wakeup(dev));
}

static int rtd1296_ohci_resume_common(struct device *dev, bool hibernated)
{
	int ret;

	ret = rtd1296_ohci_wake(dev_get_drvdata(dev));
	if (ret)
		return ret;
	ret = ohci_resume(dev_get_drvdata(dev), hibernated);
	if (ret)
		return ret;
	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	return 0;
}

static int __maybe_unused rtd1296_ohci_resume(struct device *dev)
{
	return rtd1296_ohci_resume_common(dev, false);
}

static int __maybe_unused rtd1296_ohci_restore(struct device *dev)
{
	return rtd1296_ohci_resume_common(dev, true);
}

static const struct dev_pm_ops rtd1296_ohci_pm_ops = {
	.suspend = pm_sleep_ptr(rtd1296_ohci_suspend),
	.resume = pm_sleep_ptr(rtd1296_ohci_resume),
	.freeze = pm_sleep_ptr(rtd1296_ohci_suspend),
	.thaw = pm_sleep_ptr(rtd1296_ohci_resume),
	.poweroff = pm_sleep_ptr(rtd1296_ohci_suspend),
	.restore = pm_sleep_ptr(rtd1296_ohci_restore),
};

static const struct of_device_id rtd1296_ohci_match[] = {
	{ .compatible = "realtek,rtd1296-ohci" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_ohci_match);

static struct platform_driver rtd1296_ohci_driver = {
	.probe = rtd1296_ohci_probe,
	.remove = rtd1296_ohci_remove,
	.shutdown = rtd1296_ohci_shutdown,
	.driver = {
		.name = "rtd1296-ohci",
		.of_match_table = rtd1296_ohci_match,
		.pm = pm_sleep_ptr(&rtd1296_ohci_pm_ops),
	},
};

static int __init rtd1296_ohci_init(void)
{
	ohci_init_driver(&rtd1296_ohci_hc_driver, NULL);
	rtd1296_ohci_bus_resume = rtd1296_ohci_hc_driver.bus_resume;
	if (rtd1296_ohci_bus_resume)
		rtd1296_ohci_hc_driver.bus_resume = rtd1296_ohci_resume_bus;
	return platform_driver_register(&rtd1296_ohci_driver);
}
module_init(rtd1296_ohci_init);

static void __exit rtd1296_ohci_exit(void)
{
	platform_driver_unregister(&rtd1296_ohci_driver);
}
module_exit(rtd1296_ohci_exit);

MODULE_DESCRIPTION("Realtek RTD1296 OHCI host controller");
MODULE_LICENSE("GPL");
