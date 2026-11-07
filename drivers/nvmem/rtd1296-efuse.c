// SPDX-License-Identifier: GPL-2.0-only
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>

struct rtd1296_efuse {
	void __iomem *base;
};

static int rtd1296_efuse_read(void *context, unsigned int offset,
			      void *buffer, size_t bytes)
{
	struct rtd1296_efuse *efuse = context;
	u8 *data = buffer;

	while (bytes--)
		*data++ = readb(efuse->base + offset++);

	return 0;
}

static int rtd1296_efuse_probe(struct platform_device *pdev)
{
	struct nvmem_config config = {
		.dev = &pdev->dev,
		.owner = THIS_MODULE,
		.type = NVMEM_TYPE_OTP,
		.read_only = true,
		.root_only = true,
		.word_size = 1,
		.stride = 1,
		.reg_read = rtd1296_efuse_read,
	};
	struct rtd1296_efuse *efuse;
	struct resource *res;

	efuse = devm_kzalloc(&pdev->dev, sizeof(*efuse), GFP_KERNEL);
	if (!efuse)
		return -ENOMEM;
	efuse->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(efuse->base))
		return PTR_ERR(efuse->base);
	if (resource_size(res) != SZ_1K)
		return -EINVAL;
	config.size = resource_size(res);
	config.priv = efuse;

	return PTR_ERR_OR_ZERO(devm_nvmem_register(&pdev->dev, &config));
}

static const struct of_device_id rtd1296_efuse_of_match[] = {
	{ .compatible = "realtek,rtd1296-efuse" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_efuse_of_match);

static struct platform_driver rtd1296_efuse_driver = {
	.probe = rtd1296_efuse_probe,
	.driver = {
		.name = "rtd1296-efuse",
		.of_match_table = rtd1296_efuse_of_match,
	},
};
module_platform_driver(rtd1296_efuse_driver);

MODULE_DESCRIPTION("Realtek RTD1296 eFuse readout");
MODULE_LICENSE("GPL");
