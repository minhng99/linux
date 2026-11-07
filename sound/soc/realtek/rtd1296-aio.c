// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define AIO_INPUT_CONTROL	0x000
#define AIO_INPUT_WIDTH		GENMASK(14, 12)
#define AIO_INPUT_SLAVE		BIT(15)
#define AIO_INPUT_HDMI		BIT(18)
#define AIO_INPUT_INTERLEAVE	BIT(19)
#define AIO_INPUT_ENABLE		BIT(0)
#define AIO_OUTPUT_CONTROL	0x004
#define AIO_INPUT_SELECT		GENMASK(29, 28)
#define AIO_INPUT_ANALOG		BIT(30)
#define AIO_INPUT_FIFO		0x080
#define AIO_FIFO_ENABLE		BIT(0)
#define AIO_RING_START(n)	(0x100 + 4 * (n))
#define AIO_RING_END(n)		(0x120 + 4 * (n))
#define AIO_RING_READ(n)		(0x140 + 4 * (n))
#define AIO_RING_WRITE(n)	(0x160 + 4 * (n))
#define AIO_INPUT_GO		0x300
#define AIO_GO_ENABLE		BIT(1)
#define AIO_SAMPLE_COUNT		0x318
#define AIO_SAMPLE_LIMIT		0x320
#define AIO_IRQ_ENABLE		0x330
#define AIO_IRQ_STATUS		0x334
#define AIO_IRQ_TIMER		BIT(25)
#define AIO_IRQ_OVERRUN		(BIT(9) | BIT(1))
#define AIO_IRQ_CAPTURE		(AIO_IRQ_TIMER | AIO_IRQ_OVERRUN)
#define AIO_IRQ_ALL		(BIT(25) | BIT(21) | GENMASK(10, 9) | GENMASK(2, 1))
#define AIO_IRQ_SET		BIT(0)
#define AIO_GLOBAL		0x508
#define AIO_REFERENCE		0x604
#define AIO_REFERENCE_POWER	(BIT(18) | BIT(16))
#define AIO_REFERENCE_UPDATE	(BIT(19) | BIT(17))
#define AIO_ADC_ANALOG1		0x60c
#define AIO_ADC_ANALOG2		0x610
#define AIO_ADC_FILTER		0x6ec
#define AIO_ADC_CONTROL		0x6f0
#define AIO_ADC_TIMING		0x6fc
#define AIO_ADC_CLOCK		0x710
#define AIO_DMA_BURST_FRAMES	32
#define AIO_BUFFER_MAX		SZ_256K
#define AIO_CAPTURE_SETTLE_MS	2500

struct rtd1296_aio {
	void __iomem *base;
	struct clk_bulk_data *clocks;
	struct clk *sample_clock;
	struct reset_control *reset;
	struct snd_pcm_substream *substream;
	dma_addr_t spare_dma;
	/* Protects DMA state and interrupt programming. */
	spinlock_t lock;
	u32 next_period;
	u32 period_frames;
	int num_clocks;
	int irq;
	bool powered;
	bool reset_held;
	bool prepared;
	bool running;
};

static void aio_update(struct rtd1296_aio *aio, unsigned int reg,
		       u32 mask, u32 value)
{
	u32 old = readl(aio->base + reg);

	writel((old & ~mask) | value, aio->base + reg);
}

static void aio_stop(struct rtd1296_aio *aio)
{
	writel(AIO_IRQ_ALL, aio->base + AIO_IRQ_ENABLE);
	writel(0, aio->base + AIO_INPUT_GO);
	aio_update(aio, AIO_INPUT_FIFO, AIO_FIFO_ENABLE, 0);
	aio_update(aio, AIO_INPUT_CONTROL, AIO_INPUT_ENABLE, 0);
	writel(AIO_REFERENCE_UPDATE, aio->base + AIO_REFERENCE);
	readl(aio->base + AIO_INPUT_GO);
	aio->running = false;
}

static irqreturn_t aio_interrupt(int irq, void *data)
{
	struct rtd1296_aio *aio = data;
	struct snd_pcm_substream *substream = NULL;
	u32 status, now;
	bool overrun;

	spin_lock(&aio->lock);
	if (!aio->powered || aio->reset_held) {
		spin_unlock(&aio->lock);
		return IRQ_NONE;
	}
	status = readl(aio->base + AIO_IRQ_STATUS);
	status &= AIO_IRQ_CAPTURE;
	if (!status) {
		spin_unlock(&aio->lock);
		return IRQ_NONE;
	}
	overrun = status & AIO_IRQ_OVERRUN;
	if (aio->running && overrun) {
		substream = aio->substream;
		aio_stop(aio);
	} else if (aio->running) {
		now = readl(aio->base + AIO_SAMPLE_COUNT);
		aio->next_period += aio->period_frames;
		if ((s32)(aio->next_period - now) <= AIO_DMA_BURST_FRAMES)
			aio->next_period = now + aio->period_frames;
		writel(aio->next_period, aio->base + AIO_SAMPLE_LIMIT);
		substream = aio->substream;
	}
	writel(status, aio->base + AIO_IRQ_STATUS);
	spin_unlock(&aio->lock);
	if (substream) {
		if (overrun)
			snd_pcm_stop_xrun(substream);
		else
			snd_pcm_period_elapsed(substream);
	}
	return IRQ_HANDLED;
}

static const struct snd_pcm_hardware aio_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_BATCH | SNDRV_PCM_INFO_SYNC_APPLPTR |
		SNDRV_PCM_INFO_NO_REWINDS,
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = AIO_BUFFER_MAX,
	.period_bytes_min = 1024,
	.period_bytes_max = SZ_64K,
	.periods_min = 2,
	.periods_max = 128,
	.fifo_size = AIO_DMA_BURST_FRAMES * 4,
};

static int aio_open(struct snd_soc_component *component,
		    struct snd_pcm_substream *substream)
{
	struct rtd1296_aio *aio = snd_soc_component_get_drvdata(component);
	int ret;

	snd_soc_set_runtime_hwparams(substream, &aio_hardware);
	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 32);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 32);
	if (ret < 0)
		return ret;
	aio->substream = substream;
	return 0;
}

static int aio_sync_stop(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct rtd1296_aio *aio = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int ret;

	if (!aio->powered)
		return 0;
	spin_lock_irqsave(&aio->lock, flags);
	if (!aio->reset_held)
		aio_stop(aio);
	aio->reset_held = true;
	spin_unlock_irqrestore(&aio->lock, flags);
	synchronize_irq(aio->irq);
	aio->prepared = false;
	ret = reset_control_assert(aio->reset);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	return 0;
}

static int aio_close(struct snd_soc_component *component,
		     struct snd_pcm_substream *substream)
{
	struct rtd1296_aio *aio = snd_soc_component_get_drvdata(component);
	int ret;

	ret = aio_sync_stop(component, substream);
	aio->substream = NULL;
	return ret;
}

static void aio_initialize(struct rtd1296_aio *aio)
{
	writel(AIO_IRQ_ALL, aio->base + AIO_IRQ_ENABLE);
	writel(AIO_IRQ_ALL, aio->base + AIO_IRQ_STATUS);
	writel(0, aio->base + AIO_INPUT_GO);
	aio_update(aio, AIO_INPUT_FIFO, AIO_FIFO_ENABLE, 0);
	aio_update(aio, AIO_INPUT_CONTROL, AIO_INPUT_ENABLE, 0);
	writel(U32_MAX, aio->base + AIO_SAMPLE_LIMIT);
	writel(0x1a, aio->base + AIO_GLOBAL);
	aio_update(aio, AIO_OUTPUT_CONTROL, AIO_INPUT_SELECT, AIO_INPUT_ANALOG);
	writel(AIO_REFERENCE_POWER | AIO_REFERENCE_UPDATE,
	       aio->base + AIO_REFERENCE);
	writel(0x09556b54, aio->base + AIO_ADC_ANALOG1);
	writel(0x9c6bba00, aio->base + AIO_ADC_ANALOG2);
	writel(0x301c0000, aio->base + AIO_ADC_CONTROL);
	writel(0x7ff, aio->base + AIO_ADC_CLOCK);
	writel((2 << 20) | (2 << 23) | BIT(16),
	       aio->base + AIO_ADC_FILTER);
	aio_update(aio, AIO_ADC_TIMING, ~0xf01c0000U, 0x3ff00);
	aio_update(aio, AIO_INPUT_CONTROL,
		   AIO_INPUT_WIDTH | AIO_INPUT_SLAVE | AIO_INPUT_HDMI,
		   AIO_INPUT_INTERLEAVE | AIO_INPUT_ENABLE);
}

static int aio_prepare(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream)
{
	struct rtd1296_aio *aio = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 bytes = snd_pcm_lib_buffer_bytes(substream);
	int ret;

	if (runtime->rate != 48000)
		return -EINVAL;
	if (aio->reset_held) {
		ret = reset_control_deassert(aio->reset);
		if (ret)
			return ret;
		aio->reset_held = false;
		aio_initialize(aio);
	}
	writel(runtime->dma_addr, aio->base + AIO_RING_START(0));
	writel(runtime->dma_addr + bytes, aio->base + AIO_RING_END(0));
	writel(runtime->dma_addr, aio->base + AIO_RING_READ(0));
	writel(runtime->dma_addr, aio->base + AIO_RING_WRITE(0));
	writel(aio->spare_dma, aio->base + AIO_RING_START(1));
	writel(aio->spare_dma + PAGE_SIZE, aio->base + AIO_RING_END(1));
	writel(aio->spare_dma, aio->base + AIO_RING_READ(1));
	writel(aio->spare_dma, aio->base + AIO_RING_WRITE(1));
	aio->period_frames = runtime->period_size;
	/* Allow microphone capture to settle before enabling DMA. */
	msleep(AIO_CAPTURE_SETTLE_MS);
	aio->prepared = true;
	return 0;
}

static int aio_trigger(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream, int command)
{
	struct rtd1296_aio *aio = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&aio->lock, flags);
	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!aio->prepared) {
			ret = -EINVAL;
			break;
		}
		dma_wmb();
		aio_update(aio, AIO_INPUT_FIFO, AIO_FIFO_ENABLE, AIO_FIFO_ENABLE);
		writel(AIO_IRQ_ALL, aio->base + AIO_IRQ_STATUS);
		writel(AIO_GO_ENABLE, aio->base + AIO_INPUT_GO);
		/* DMA completion trails the sample counter by one burst. */
		aio->next_period = readl(aio->base + AIO_SAMPLE_COUNT) +
				   aio->period_frames + AIO_DMA_BURST_FRAMES;
		writel(aio->next_period, aio->base + AIO_SAMPLE_LIMIT);
		aio->running = true;
		writel(AIO_IRQ_CAPTURE | AIO_IRQ_SET, aio->base + AIO_IRQ_ENABLE);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		aio_stop(aio);
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&aio->lock, flags);
	return ret;
}

static snd_pcm_uframes_t aio_pointer(struct snd_soc_component *component,
				     struct snd_pcm_substream *substream)
{
	struct rtd1296_aio *aio = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 pointer, offset;

	if (!aio->prepared)
		return 0;
	pointer = readl(aio->base + AIO_RING_WRITE(0));
	offset = pointer - (u32)runtime->dma_addr;

	if (offset >= snd_pcm_lib_buffer_bytes(substream))
		return SNDRV_PCM_POS_XRUN;
	dma_rmb();
	return bytes_to_frames(runtime, offset);
}

static int aio_ack(struct snd_soc_component *component,
		   struct snd_pcm_substream *substream)
{
	struct rtd1296_aio *aio = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t frames;

	if (!aio->prepared)
		return 0;
	frames = READ_ONCE(runtime->control->appl_ptr) % runtime->buffer_size;
	dma_mb();
	writel(runtime->dma_addr + frames_to_bytes(runtime, frames),
	       aio->base + AIO_RING_READ(0));
	return 0;
}

static int aio_pcm_new(struct snd_soc_component *component,
		       struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
					      component->dev, SZ_64K, AIO_BUFFER_MAX);
}

static const struct snd_soc_dapm_widget aio_widgets[] = {
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),
	SND_SOC_DAPM_ADC("ADC", "ADC Capture", SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route aio_routes[] = {
	{ "ADC", NULL, "MIC1" },
	{ "ADC", NULL, "MIC2" },
};

static const struct snd_soc_component_driver aio_component = {
	.name = "rtd1296-aio",
	.open = aio_open,
	.close = aio_close,
	.prepare = aio_prepare,
	.trigger = aio_trigger,
	.sync_stop = aio_sync_stop,
	.hw_free = aio_sync_stop,
	.pointer = aio_pointer,
	.ack = aio_ack,
	.pcm_new = aio_pcm_new,
	.dapm_widgets = aio_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aio_widgets),
	.dapm_routes = aio_routes,
	.num_dapm_routes = ARRAY_SIZE(aio_routes),
};

static struct snd_soc_dai_driver aio_dais[] = {
	{
		.name = "rtd1296-aio-dma",
		.capture = {
			.stream_name = "DMA Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_BE,
		},
	},
	{
		.name = "rtd1296-amic",
		.capture = {
			.stream_name = "ADC Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_BE,
		},
	},
};

static int aio_runtime_resume(struct device *dev)
{
	struct rtd1296_aio *aio = dev_get_drvdata(dev);
	int ret;

	ret = clk_set_rate(aio->sample_clock, 48000);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(aio->num_clocks, aio->clocks);
	if (ret)
		return ret;
	ret = reset_control_deassert(aio->reset);
	if (ret)
		goto clocks;
	aio->reset_held = false;
	if (readl(aio->base + AIO_INPUT_GO) == 0xdeadbeef) {
		ret = -EIO;
		goto reset;
	}
	aio_initialize(aio);
	aio->powered = true;
	return 0;

reset:
	reset_control_assert(aio->reset);
	aio->reset_held = true;
clocks:
	clk_bulk_disable_unprepare(aio->num_clocks, aio->clocks);
	return ret;
}

static int aio_runtime_suspend(struct device *dev)
{
	struct rtd1296_aio *aio = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&aio->lock, flags);
	if (aio->running) {
		spin_unlock_irqrestore(&aio->lock, flags);
		return -EBUSY;
	}
	if (!aio->reset_held)
		aio_stop(aio);
	aio->reset_held = true;
	aio->powered = false;
	spin_unlock_irqrestore(&aio->lock, flags);
	synchronize_irq(aio->irq);
	ret = reset_control_assert(aio->reset);
	if (ret) {
		aio->powered = true;
		return ret;
	}
	usleep_range(1000, 2000);
	aio->prepared = false;
	clk_bulk_disable_unprepare(aio->num_clocks, aio->clocks);
	return 0;
}

static const struct dev_pm_ops aio_pm_ops = {
	RUNTIME_PM_OPS(aio_runtime_suspend, aio_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static int aio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd1296_aio *aio;
	int ret;

	aio = devm_kzalloc(dev, sizeof(*aio), GFP_KERNEL);
	if (!aio)
		return -ENOMEM;
	spin_lock_init(&aio->lock);
	platform_set_drvdata(pdev, aio);
	aio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(aio->base))
		return PTR_ERR(aio->base);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(31));
	if (ret)
		return ret;
	if (!dmam_alloc_coherent(dev, PAGE_SIZE, &aio->spare_dma, GFP_KERNEL))
		return -ENOMEM;
	aio->num_clocks = devm_clk_bulk_get_all(dev, &aio->clocks);
	if (aio->num_clocks < 0)
		return dev_err_probe(dev, aio->num_clocks, "cannot get clocks\n");
	aio->sample_clock = devm_clk_get(dev, "pcm");
	if (IS_ERR(aio->sample_clock))
		return dev_err_probe(dev, PTR_ERR(aio->sample_clock),
				     "cannot get sample clock\n");
	aio->reset = devm_reset_control_get_exclusive(dev, "aio");
	if (IS_ERR(aio->reset))
		return dev_err_probe(dev, PTR_ERR(aio->reset), "cannot get AIO reset\n");
	aio->irq = platform_get_irq(pdev, 0);
	if (aio->irq < 0)
		return aio->irq;
	ret = devm_request_irq(dev, aio->irq, aio_interrupt, 0, dev_name(dev), aio);
	if (ret)
		return ret;
	pm_runtime_set_suspended(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	ret = snd_soc_register_component(dev, &aio_component, aio_dais,
					 ARRAY_SIZE(aio_dais));
	if (ret)
		pm_runtime_disable(dev);
	return ret;
}

static void aio_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	snd_soc_unregister_component(dev);
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		aio_runtime_suspend(dev);
}

static void aio_shutdown(struct platform_device *pdev)
{
	struct rtd1296_aio *aio = platform_get_drvdata(pdev);
	unsigned long flags;

	if (pm_runtime_status_suspended(&pdev->dev))
		return;
	spin_lock_irqsave(&aio->lock, flags);
	if (!aio->reset_held)
		aio_stop(aio);
	spin_unlock_irqrestore(&aio->lock, flags);
	aio_runtime_suspend(&pdev->dev);
}

static const struct of_device_id aio_match[] = {
	{ .compatible = "realtek,rtd1296-aio" },
	{ }
};
MODULE_DEVICE_TABLE(of, aio_match);

static struct platform_driver aio_driver = {
	.probe = aio_probe,
	.remove = aio_remove,
	.shutdown = aio_shutdown,
	.driver = {
		.name = "rtd1296-aio",
		.of_match_table = aio_match,
		.pm = pm_ptr(&aio_pm_ops),
	},
};
module_platform_driver(aio_driver);

MODULE_DESCRIPTION("Realtek RTD1296 analogue microphone capture");
MODULE_LICENSE("GPL");
