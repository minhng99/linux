// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

#define RTD1296_EMMC_ISR		0x424
#define RTD1296_EMMC_ISR_SET	BIT(0)
#define RTD1296_EMMC_DMA_DONE	BIT(1)
#define RTD1296_EMMC_DMA_MASK	BIT(2)
#define RTD1296_EMMC_DESC_MASK	BIT(3)
#define RTD1296_EMMC_IP_MASK	BIT(4)
#define RTD1296_EMMC_SYNC		0x42c
#define RTD1296_EMMC_CLK_SYNC	BIT(30)
#define RTD1296_EMMC_PAD		0x474
#define RTD1296_EMMC_CLOCK	0x478
#define RTD1296_EMMC_SAMPLE	GENMASK(13, 12)
#define RTD1296_EMMC_PUSH	GENMASK(9, 8)

static void rtd1296_emmc_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	u32 sync;

	writel(0, host->regs + RTD1296_EMMC_PAD);
	writel(FIELD_PREP(RTD1296_EMMC_SAMPLE, 2) |
	       FIELD_PREP(RTD1296_EMMC_PUSH, 1),
	       host->regs + RTD1296_EMMC_CLOCK);
	/* Synchronize the wrapper after programming the phase clocks. */
	sync = readl(host->regs + RTD1296_EMMC_SYNC);
	writel(sync ^ RTD1296_EMMC_CLK_SYNC, host->regs + RTD1296_EMMC_SYNC);
}

static int rtd1296_emmc_init(struct dw_mci *host)
{
	struct mmc_clk_phase *phase;
	struct clk *drive, *sample;
	int ret;

	host->quirks |= DW_MMC_QUIRK_FIFO64_32;
	/* FIFO transfers use the native interrupt and data paths. */
	host->dma_threshold = U32_MAX;

	drive = devm_clk_get_enabled(host->dev, "ciu-drive");
	if (IS_ERR(drive))
		return dev_err_probe(host->dev, PTR_ERR(drive),
				     "cannot enable drive clock\n");
	sample = devm_clk_get_enabled(host->dev, "ciu-sample");
	if (IS_ERR(sample))
		return dev_err_probe(host->dev, PTR_ERR(sample),
				     "cannot enable sample clock\n");

	mmc_of_parse_clk_phase(host->dev, &host->phase_map);
	phase = &host->phase_map.phase[MMC_TIMING_LEGACY];
	if (!phase->valid)
		return dev_err_probe(host->dev, -EINVAL, "missing legacy clock phase\n");
	ret = clk_set_phase(drive, phase->out_deg);
	if (ret)
		return ret;
	ret = clk_set_phase(sample, phase->in_deg);
	if (ret)
		return ret;

	/* Route native controller interrupts; mask the wrapper DMA sources. */
	writel(RTD1296_EMMC_ISR_SET | RTD1296_EMMC_DMA_MASK |
	       RTD1296_EMMC_DESC_MASK, host->regs + RTD1296_EMMC_ISR);
	writel(RTD1296_EMMC_IP_MASK, host->regs + RTD1296_EMMC_ISR);
	writel(RTD1296_EMMC_DMA_DONE, host->regs + RTD1296_EMMC_ISR);
	mci_writel(host, DDR_REG, 0);
	rtd1296_emmc_set_ios(host, &host->mmc->ios);
	return 0;
}

static void rtd1296_emmc_assert_cr(void *data)
{
	reset_control_assert(data);
}

static u32 rtd1296_emmc_get_drto_clks(struct dw_mci *host)
{
	const struct mmc_data *data = host->data;
	u64 clocks = mci_readl(host, TMOUT) >> 8;

	/* The software guard also covers data serialization. */
	if (data) {
		u32 width = 1 << host->mmc->ios.bus_width;

		clocks += (DIV_ROUND_UP_ULL((u64)data->blksz * 8, width) + 18) *
			  data->blocks;
	}
	return min_t(u64, clocks, U32_MAX);
}

static const struct dw_mci_drv_data rtd1296_emmc_data = {
	.common_caps = MMC_CAP_CMD23,
	.init = rtd1296_emmc_init,
	.set_ios = rtd1296_emmc_set_ios,
	.get_drto_clks = rtd1296_emmc_get_drto_clks,
};

static int rtd1296_emmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *cr;
	struct resource *res;
	struct clk *clk;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < RTD1296_EMMC_CLOCK + sizeof(u32))
		return -EINVAL;

	clk = devm_clk_get_enabled(dev, "card");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot enable card clock\n");

	/* The card reader and eMMC share the CR reset. */
	cr = devm_reset_control_get_shared(dev, "cr");
	if (IS_ERR(cr))
		return dev_err_probe(dev, PTR_ERR(cr), "cannot get CR reset\n");
	ret = reset_control_deassert(cr);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, rtd1296_emmc_assert_cr, cr);
	if (ret)
		return ret;

	pm_runtime_get_noresume(dev);
	ret = pm_runtime_set_active(dev);
	if (ret) {
		pm_runtime_put_noidle(dev);
		return ret;
	}
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 50);
	pm_runtime_use_autosuspend(dev);
	ret = dw_mci_pltfm_register(pdev, &rtd1296_emmc_data);
	if (ret) {
		pm_runtime_disable(dev);
		pm_runtime_set_suspended(dev);
		pm_runtime_put_noidle(dev);
		return ret;
	}
	pm_runtime_put_autosuspend(dev);
	return 0;
}

static void rtd1296_emmc_remove(struct platform_device *pdev)
{
	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	dw_mci_pltfm_remove(pdev);
}

static const struct of_device_id rtd1296_emmc_of_match[] = {
	{ .compatible = "realtek,rtd1296-dw-mshc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_emmc_of_match);

static struct platform_driver rtd1296_emmc_driver = {
	.probe = rtd1296_emmc_probe,
	.remove = rtd1296_emmc_remove,
	.driver = {
		.name = "dwmmc-rtd1296",
		.of_match_table = rtd1296_emmc_of_match,
		.pm = pm_ptr(&dw_mci_pmops),
	},
};
module_platform_driver(rtd1296_emmc_driver);

MODULE_DESCRIPTION("Realtek RTD1296 DW-MSHC integration");
MODULE_LICENSE("GPL");
