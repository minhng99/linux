// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/crc-itu-t.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define CR_DMA_ADDR		0x004
#define CR_DMA_BLOCKS		0x008
#define CR_DMA_CTL		0x00c
#define CR_CTL			0x020
#define CR_ISR			0x024
#define CR_IER			0x028
#define CR_PAD			0x074
#define CR_CLOCK		0x078
#define CR_CARD_STOP		0x103
#define CR_CARD_OUTPUT		0x104
#define CR_CARD_SELECT		0x10e
#define CR_CARD_PRESENT		0x11f
#define CR_CARD_CLOCK		0x129
#define SD_CFG1			0x180
#define SD_CFG2			0x181
#define SD_CFG3			0x182
#define SD_STAT1			0x183
#define SD_STAT2			0x184
#define SD_BUS_STAT		0x185
#define SD_CMD			0x189
#define SD_BYTE_COUNT		0x18f
#define SD_BLOCK_COUNT		0x191
#define SD_TRANSFER		0x193
#define SD_BUS_STATE		0x197

#define IRQ_CARD_END		BIT(1)
#define IRQ_ERROR		BIT(2)
#define IRQ_DMA_END		BIT(4)
#define IRQ_ALL			(IRQ_CARD_END | IRQ_ERROR | IRQ_DMA_END)
/* Bit zero selects whether the written interrupt-enable bits are set or cleared. */
#define IRQ_SET		BIT(0)
#define DMA_ENABLE		BIT(0)
#define DMA_READ			BIT(1)
#define DMA_RESPONSE		BIT(4)
#define DMA_SHORT		BIT(5)
#define XFER_START		BIT(7)
#define XFER_END			(BIT(6) | BIT(5))
#define XFER_ERROR		BIT(4)
#define SD_CRC7_ERROR		BIT(7)
#define SD_CRC16_ERROR		BIT(6)
#define SD_WRITE_CRC_ERROR	BIT(5)
#define SD_WRITE_CRC_TIMEOUT	BIT(1)
#define SD_RESPONSE_TIMEOUT	BIT(0)
#define SD_DIV256		GENMASK(7, 6)
#define SD_WIDTH		GENMASK(1, 0)
#define SD_FIFO_RESET_N		BIT(4)
#define SD_DAT0			BIT(1)
#define SD_CLOCK_TOGGLE		BIT(7)
#define SD_CLOCK_STOP		BIT(6)
#define BUFFER_SIZE		SZ_64K

struct rtd1296_sd {
	struct mmc_host *mmc;
	struct device *dev;
	void __iomem *base;
	struct regmap *sb2;
	struct clk_bulk_data clocks[2];
	struct reset_control *reset;
	/* Serialize commands, power transitions and clock configuration. */
	struct mutex io_lock;
	/* Protect request handoff and interrupt completion state. */
	spinlock_t irq_lock;
	struct completion completion;
	struct work_struct work;
	struct workqueue_struct *wq;
	struct mmc_request *mrq;
	void *buffer;
	dma_addr_t dma;
	unsigned long rate;
	u32 irq_status;
	u8 transfer_status;
	u8 status2;
	int irq;
	int ios_error;
	bool waiting;
	bool powered;
	bool irq_registered;
	bool irq_live;
	bool broken;
};

static int rtd1296_sd_sync(struct rtd1296_sd *host)
{
	return regmap_write(host->sb2, 0x20, 0);
}

static int rtd1296_sd_cancel(struct rtd1296_sd *host)
{
	u32 control = readl(host->base + CR_CTL);
	u32 status;
	int ret;

	writel(0, host->base + CR_DMA_CTL);
	writeb(0, host->base + SD_TRANSFER);
	writeb(0xff, host->base + CR_CARD_STOP);
	writeb(0, host->base + CR_CARD_STOP);
	writel(control & ~BIT(0), host->base + CR_CTL);
	readl(host->base + CR_CTL);
	udelay(1);
	writel(control | BIT(0), host->base + CR_CTL);
	readl(host->base + CR_CTL);
	ret = readl_poll_timeout(host->base + CR_DMA_CTL, status,
				 !(status & DMA_ENABLE), 10, 100000);
	return ret ?: rtd1296_sd_sync(host);
}

static void rtd1296_sd_clocks_off(struct rtd1296_sd *host)
{
	if (host->irq_live) {
		disable_irq(host->irq);
		host->irq_live = false;
	}
	if (host->powered) {
		WRITE_ONCE(host->powered, false);
		clk_bulk_disable_unprepare(ARRAY_SIZE(host->clocks), host->clocks);
	}
}

static int rtd1296_sd_hw_on(struct rtd1296_sd *host)
{
	u32 control;
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(host->clocks), host->clocks);
	if (ret)
		return ret;
	WRITE_ONCE(host->powered, true);
	writel(IRQ_ALL, host->base + CR_IER);
	control = readl(host->base + CR_CTL);
	writel(control | BIT(1), host->base + CR_CTL);
	ret = rtd1296_sd_cancel(host);
	if (ret) {
		rtd1296_sd_clocks_off(host);
		return ret;
	}
	writeb(3, host->base + SD_BUS_STATE);
	writel(0, host->base + CR_PAD);
	writel(0x2100, host->base + CR_CLOCK);
	writeb(0xd0, host->base + SD_CFG1);
	writeb(2, host->base + CR_CARD_SELECT);
	writeb(0, host->base + CR_CARD_OUTPUT);
	writeb(0, host->base + CR_CARD_CLOCK);
	writel(IRQ_ALL, host->base + CR_ISR);
	host->broken = false;
	host->ios_error = 0;
	if (host->irq_registered && !host->irq_live) {
		enable_irq(host->irq);
		host->irq_live = true;
	}
	return 0;
}

static void rtd1296_sd_hw_off(struct rtd1296_sd *host)
{
	if (host->powered) {
		writel(IRQ_ALL, host->base + CR_IER);
		rtd1296_sd_cancel(host);
		writeb(0, host->base + CR_CARD_CLOCK);
		writeb(0, host->base + CR_CARD_OUTPUT);
		writel(readl(host->base + CR_CTL) & ~BIT(0), host->base + CR_CTL);
		readl(host->base + CR_CTL);
	}
	rtd1296_sd_clocks_off(host);
}

static int rtd1296_sd_present(struct rtd1296_sd *host)
{
	return host->powered && !host->broken &&
	       (readb(host->base + CR_CARD_PRESENT) & BIT(2));
}

static irqreturn_t rtd1296_sd_irq(int irq, void *data)
{
	struct rtd1296_sd *host = data;
	u32 status;

	if (!READ_ONCE(host->powered))
		return IRQ_NONE;
	spin_lock(&host->irq_lock);
	status = readl(host->base + CR_ISR) & IRQ_ALL;
	if (status) {
		if (host->waiting) {
			host->irq_status |= status;
			host->transfer_status |= readb(host->base + SD_TRANSFER) & XFER_ERROR;
			host->status2 |= readb(host->base + SD_STAT2);
			complete(&host->completion);
		}
		writel(status, host->base + CR_ISR);
		readl(host->base + CR_ISR);
	}
	spin_unlock(&host->irq_lock);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

static void rtd1296_sd_count(struct rtd1296_sd *host, u32 reg, u16 count)
{
	writeb(count, host->base + reg);
	writeb(count >> 8, host->base + reg + 1);
}

static void rtd1296_sd_set_clock(struct rtd1296_sd *host, unsigned int clock)
{
	struct mmc_host *mmc = host->mmc;
	unsigned int target = clock ? clamp(clock, mmc->f_min, mmc->f_max) : 0;
	u8 config = readb(host->base + SD_CFG1) & ~SD_DIV256;
	u32 divider;

	if (target >= host->rate / 4) {
		divider = 2;
		mmc->actual_clock = host->rate / 4;
	} else if (target >= host->rate / 8) {
		divider = 3;
		mmc->actual_clock = host->rate / 8;
	} else {
		divider = target >= host->rate / 256 ? 0 : 1;
		config |= SD_DIV256;
		mmc->actual_clock = host->rate / (256 << divider);
	}
	writel(0x2100 | divider, host->base + CR_CLOCK);
	writeb(config, host->base + SD_CFG1);
	writeb(clock ? BIT(2) : 0, host->base + CR_CARD_CLOCK);
	if (!clock)
		mmc->actual_clock = 0;
}

static u16 rtd1296_sd_lane_bits(const u8 *data, unsigned int len, unsigned int lane)
{
	u16 bits = 0;
	unsigned int i;

	for (i = 0; i < len; i++) {
		bits = (bits << 1) | ((data[i] >> (lane + 4)) & 1);
		bits = (bits << 1) | ((data[i] >> lane) & 1);
	}
	return bits;
}

/* Padded packets retain wire CRC after the payload. */
static int rtd1296_sd_short_crc(struct rtd1296_sd *host, unsigned int len)
{
	const u8 *data = host->buffer;
	unsigned int lane, i;
	u16 crc;

	if (host->mmc->ios.bus_width == MMC_BUS_WIDTH_1) {
		if (len > 62)
			return -EOPNOTSUPP;
		return crc_itu_t(0, data, len) == get_unaligned_be16(data + len) ?
			0 : -EILSEQ;
	}
	if (len > 56 || len % 4)
		return -EOPNOTSUPP;
	for (lane = 0; lane < 4; lane++) {
		crc = 0;
		for (i = 0; i < len; i += 4)
			crc = crc_itu_t_byte(crc, rtd1296_sd_lane_bits(data + i, 4, lane));
		if (crc != rtd1296_sd_lane_bits(data + len, 8, lane))
			return -EILSEQ;
	}
	return 0;
}

static int rtd1296_sd_check_status(struct rtd1296_sd *host,
				   struct mmc_command *cmd, struct mmc_data *data)
{
	u8 status = readb(host->base + SD_STAT1);
	u8 errors = 0;

	if (readb(host->base + SD_STAT2) & SD_RESPONSE_TIMEOUT)
		return -ETIMEDOUT;
	if (cmd->flags & MMC_RSP_CRC)
		errors |= SD_CRC7_ERROR;
	if (data && (data->flags & MMC_DATA_READ) && data->blksz >= 64)
		errors |= SD_CRC16_ERROR;
	if (data && (data->flags & MMC_DATA_WRITE)) {
		if (status & SD_WRITE_CRC_TIMEOUT)
			return -ETIMEDOUT;
		errors |= SD_WRITE_CRC_ERROR;
	}
	return status & errors ? -EILSEQ : 0;
}

static int rtd1296_sd_command(struct rtd1296_sd *host, struct mmc_command *cmd,
			      struct mmc_data *data)
{
	bool long_response = cmd->flags & MMC_RSP_136;
	bool use_dma = data || long_response;
	bool short_crc = data && (data->flags & MMC_DATA_READ) && data->blksz < 64;
	u32 expected = IRQ_CARD_END | (use_dma ? IRQ_DMA_END : 0);
	u32 dma_control = DMA_ENABLE;
	u32 bytes = data ? data->blksz * data->blocks : 0;
	u16 block_size = 512, blocks = 1;
	u8 response = BIT(6), transfer = 8;
	unsigned long flags, deadline, timeout;
	u32 status;
	u8 config;
	int i, ret;

	if (!rtd1296_sd_present(host))
		return -ENOMEDIUM;
	if (cmd->opcode > 63 || bytes > BUFFER_SIZE)
		return -EINVAL;
	if (cmd->flags & MMC_RSP_PRESENT)
		response |= long_response ? 2 : 1;
	if (!(cmd->flags & MMC_RSP_CRC))
		response |= BIT(2);
	if (cmd->flags & MMC_RSP_BUSY)
		response |= BIT(3);
	if (long_response)
		dma_control |= DMA_READ | DMA_RESPONSE;
	if (data) {
		if (long_response || !data->blocks || !data->blksz ||
		    data->blocks > BUFFER_SIZE / 512 ||
		    data->blksz > 512 ||
		    (data->blksz != 512 && (data->blocks != 1 || data->blksz > 64)))
			return -EINVAL;
		blocks = data->blocks;
		block_size = data->blksz == 512 ? 512 : 64;
		if (data->blksz >= 64)
			response &= ~BIT(6);
		if (block_size <= 64)
			dma_control |= DMA_SHORT;
		if (data->flags & MMC_DATA_READ) {
			dma_control |= DMA_READ;
			transfer = block_size <= 64 ? 0xc : 0xe;
		} else {
			if (data->blksz != 512)
				return -EINVAL;
			if (sg_copy_to_buffer(data->sg, data->sg_len, host->buffer,
					      bytes) != bytes)
				return -EINVAL;
			transfer = 0xa;
		}
	}
	writel(IRQ_ALL, host->base + CR_IER);
	config = readb(host->base + SD_CFG1);
	writeb(config & ~SD_FIFO_RESET_N, host->base + SD_CFG1);
	readb(host->base + SD_CFG1);
	writeb(config | SD_FIFO_RESET_N, host->base + SD_CFG1);
	readb(host->base + SD_CFG1);
	writeb(0x40 | cmd->opcode, host->base + SD_CMD);
	for (i = 0; i < 4; i++)
		writeb(cmd->arg >> (24 - 8 * i), host->base + SD_CMD + 1 + i);
	writeb(0, host->base + SD_CMD + 5);
	writeb(response, host->base + SD_CFG2);
	writeb(5, host->base + SD_CFG3);
	/* Write DMA can finish before the card transfer starts. */
	spin_lock_irqsave(&host->irq_lock, flags);
	writel(IRQ_ALL, host->base + CR_ISR);
	reinit_completion(&host->completion);
	host->irq_status = 0;
	host->transfer_status = 0;
	host->status2 = 0;
	host->waiting = true;
	spin_unlock_irqrestore(&host->irq_lock, flags);
	if (use_dma) {
		rtd1296_sd_count(host, SD_BYTE_COUNT, block_size);
		rtd1296_sd_count(host, SD_BLOCK_COUNT, blocks);
		writel(lower_32_bits(host->dma >> 3), host->base + CR_DMA_ADDR);
		writel(blocks, host->base + CR_DMA_BLOCKS);
		dma_wmb();
		writel(dma_control, host->base + CR_DMA_CTL);
	}
	ret = rtd1296_sd_sync(host);
	if (ret)
		goto cancel;
	timeout = msecs_to_jiffies(max(cmd->busy_timeout, 3000U));
	deadline = jiffies + timeout;
	spin_lock_irqsave(&host->irq_lock, flags);
	writel(IRQ_SET | IRQ_ALL, host->base + CR_IER);
	readl(host->base + CR_IER);
	writeb(XFER_START | transfer, host->base + SD_TRANSFER);
	spin_unlock_irqrestore(&host->irq_lock, flags);
	ret = rtd1296_sd_sync(host);
	if (ret)
		goto cancel;
	for (;;) {
		u8 state = readb(host->base + SD_TRANSFER);
		u8 error = readb(host->base + SD_STAT2);
		u32 pending;

		spin_lock_irqsave(&host->irq_lock, flags);
		pending = host->irq_status;
		state |= host->transfer_status;
		error |= host->status2;
		spin_unlock_irqrestore(&host->irq_lock, flags);

		if (!rtd1296_sd_present(host)) {
			ret = -ENOMEDIUM;
			break;
		}
		if (error & SD_RESPONSE_TIMEOUT) {
			ret = -ETIMEDOUT;
			break;
		}
		if ((pending & IRQ_ERROR) || (state & XFER_ERROR)) {
			ret = -EILSEQ;
			break;
		}
		if ((pending & expected) == expected && (state & XFER_END) == XFER_END) {
			ret = 0;
			break;
		}
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		if ((pending & expected) == expected)
			usleep_range(10, 20);
		else
			wait_for_completion_timeout(&host->completion,
						    min(deadline - jiffies, msecs_to_jiffies(100)));
	}
	spin_lock_irqsave(&host->irq_lock, flags);
	host->waiting = false;
	writel(IRQ_ALL, host->base + CR_IER);
	spin_unlock_irqrestore(&host->irq_lock, flags);
	if (ret)
		goto cancel;
	if (use_dma) {
		ret = readl_poll_timeout(host->base + CR_DMA_CTL, status,
					 !(status & DMA_ENABLE), 10, 100000);
		if (ret)
			goto cancel;
		dma_rmb();
	}
	ret = rtd1296_sd_sync(host);
	if (ret)
		goto cancel;
	ret = rtd1296_sd_check_status(host, cmd, data);
	if (ret)
		goto cancel;
	if (long_response) {
		u8 *buffer = host->buffer;

		buffer[16] = readb(host->base + SD_CMD + 5);
		for (i = 0; i < 4; i++)
			cmd->resp[i] = get_unaligned_be32(buffer + 1 + 4 * i);
	} else if (cmd->flags & MMC_RSP_PRESENT) {
		cmd->resp[0] = 0;
		for (i = 0; i < 4; i++)
			cmd->resp[0] = (cmd->resp[0] << 8) |
				readb(host->base + SD_CMD + 1 + i);
	}
	if (short_crc) {
		ret = rtd1296_sd_short_crc(host, bytes);
		if (ret)
			goto cancel;
	}
	if (data && (data->flags & MMC_DATA_READ) &&
	    sg_copy_from_buffer(data->sg, data->sg_len, host->buffer, bytes) != bytes)
		return -EINVAL;
	if (cmd->opcode == MMC_SELECT_CARD) {
		/* The initialization divider cannot clock the receive DMA path. */
		host->mmc->f_min = cmd->arg ? host->rate / 8 : host->rate / 512;
		rtd1296_sd_set_clock(host, host->mmc->ios.clock);
	}
	return 0;

cancel:
	spin_lock_irqsave(&host->irq_lock, flags);
	host->waiting = false;
	writel(IRQ_ALL, host->base + CR_IER);
	spin_unlock_irqrestore(&host->irq_lock, flags);
	dev_dbg(host->dev, "CMD%u failed: %d, transfer=%02x status=%02x/%02x irq=%x\n",
		cmd->opcode, ret, readb(host->base + SD_TRANSFER),
		readb(host->base + SD_STAT1), readb(host->base + SD_STAT2),
		host->irq_status);
	if (rtd1296_sd_cancel(host)) {
		host->broken = true;
		rtd1296_sd_clocks_off(host);
	}
	return ret;
}

static void rtd1296_sd_work(struct work_struct *work)
{
	struct rtd1296_sd *host = container_of(work, struct rtd1296_sd, work);
	struct mmc_request *mrq = host->mrq;
	unsigned long flags;
	int ret;

	mutex_lock(&host->io_lock);
	ret = host->ios_error;
	if (!host->powered || host->broken)
		ret = -EIO;
	if (!ret && mrq->sbc) {
		ret = rtd1296_sd_command(host, mrq->sbc, NULL);
		mrq->sbc->error = ret;
	}
	if (!ret)
		ret = rtd1296_sd_command(host, mrq->cmd, mrq->data);
	mrq->cmd->error = ret;
	if (mrq->data) {
		mrq->data->error = ret;
		mrq->data->bytes_xfered = ret ? 0 : mrq->data->blocks * mrq->data->blksz;
	}
	if (mrq->stop && host->powered && !host->broken &&
	    (!mrq->sbc || ret))
		mrq->stop->error = rtd1296_sd_command(host, mrq->stop, NULL);
	mutex_unlock(&host->io_lock);
	spin_lock_irqsave(&host->irq_lock, flags);
	host->mrq = NULL;
	spin_unlock_irqrestore(&host->irq_lock, flags);
	mmc_request_done(host->mmc, mrq);
}

static void rtd1296_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct rtd1296_sd *host = mmc_priv(mmc);
	unsigned long flags;

	spin_lock_irqsave(&host->irq_lock, flags);
	if (host->mrq) {
		spin_unlock_irqrestore(&host->irq_lock, flags);
		mrq->cmd->error = -EBUSY;
		mmc_request_done(mmc, mrq);
		return;
	}
	host->mrq = mrq;
	spin_unlock_irqrestore(&host->irq_lock, flags);
	queue_work(host->wq, &host->work);
}

static void rtd1296_sd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct rtd1296_sd *host = mmc_priv(mmc);
	u8 config;
	int ret;

	mutex_lock(&host->io_lock);
	if (ios->power_mode == MMC_POWER_OFF) {
		if (host->powered) {
			writeb(0, host->base + CR_CARD_CLOCK);
			writeb(0, host->base + CR_CARD_OUTPUT);
		}
		mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);
		mmc->actual_clock = 0;
		mmc->f_min = host->rate / 512;
		goto out;
	}
	if (!host->powered) {
		host->ios_error = -EIO;
		goto out;
	}
	ret = mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, ios->vdd);
	if (ret) {
		host->ios_error = ret;
		goto out;
	}
	config = readb(host->base + SD_CFG1) & ~SD_WIDTH;
	if (ios->bus_width == MMC_BUS_WIDTH_4)
		config |= 1;
	writeb(config, host->base + SD_CFG1);
	writeb(BIT(2), host->base + CR_CARD_OUTPUT);
	rtd1296_sd_set_clock(host, ios->clock);
	host->ios_error = 0;
out:
	mutex_unlock(&host->io_lock);
}

static int rtd1296_sd_get_cd(struct mmc_host *mmc)
{
	struct rtd1296_sd *host = mmc_priv(mmc);
	int ret = mmc_gpio_get_cd(mmc);

	if (ret >= 0)
		return ret;
	mutex_lock(&host->io_lock);
	ret = !!rtd1296_sd_present(host);
	mutex_unlock(&host->io_lock);
	return ret;
}

static int rtd1296_sd_get_ro(struct mmc_host *mmc)
{
	struct rtd1296_sd *host = mmc_priv(mmc);
	int ret;

	if (mmc->caps2 & MMC_CAP2_NO_WRITE_PROTECT)
		return 0;
	ret = mmc_gpio_get_ro(mmc);
	if (ret >= 0)
		return ret;
	mutex_lock(&host->io_lock);
	ret = host->powered && (readb(host->base + CR_CARD_PRESENT) & BIT(5));
	mutex_unlock(&host->io_lock);
	return !!ret;
}

static int rtd1296_sd_card_busy(struct mmc_host *mmc)
{
	struct rtd1296_sd *host = mmc_priv(mmc);
	u8 value;
	int busy = 0;

	mutex_lock(&host->io_lock);
	if (host->powered) {
		value = readb(host->base + SD_BUS_STAT) & ~SD_CLOCK_STOP;
		writeb(value | SD_CLOCK_TOGGLE, host->base + SD_BUS_STAT);
		busy = !(readb(host->base + SD_BUS_STAT) & SD_DAT0);
		writeb(value & ~SD_CLOCK_TOGGLE, host->base + SD_BUS_STAT);
	}
	mutex_unlock(&host->io_lock);
	return busy;
}

static const struct mmc_host_ops rtd1296_sd_ops = {
	.request = rtd1296_sd_request,
	.set_ios = rtd1296_sd_set_ios,
	.get_cd = rtd1296_sd_get_cd,
	.get_ro = rtd1296_sd_get_ro,
	.card_busy = rtd1296_sd_card_busy,
};

static void rtd1296_sd_destroy_workqueue(void *data)
{
	destroy_workqueue(data);
}

static int rtd1296_sd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd1296_sd *host;
	struct mmc_host *mmc;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(31));
	if (ret)
		return ret;
	mmc = devm_mmc_alloc_host(dev, sizeof(*host));
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->dev = dev;
	mutex_init(&host->io_lock);
	spin_lock_init(&host->irq_lock);
	init_completion(&host->completion);
	INIT_WORK(&host->work, rtd1296_sd_work);
	host->wq = alloc_ordered_workqueue("rtd1296-sd-%s", WQ_MEM_RECLAIM,
					   dev_name(dev));
	if (!host->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, rtd1296_sd_destroy_workqueue, host->wq);
	if (ret)
		return ret;
	host->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->base))
		return PTR_ERR(host->base);
	host->irq = platform_get_irq(pdev, 0);
	if (host->irq < 0)
		return host->irq;
	host->sb2 = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sb2");
	if (IS_ERR(host->sb2))
		return dev_err_probe(dev, PTR_ERR(host->sb2), "Cannot access SB2\n");
	host->clocks[0].id = "biu";
	host->clocks[1].id = "ciu";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(host->clocks), host->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get clocks\n");
	host->rate = clk_get_rate(host->clocks[1].clk);
	if (!host->rate)
		return dev_err_probe(dev, -EINVAL, "Card clock rate unavailable\n");
	host->reset = devm_reset_control_get_shared(dev, NULL);
	if (IS_ERR(host->reset))
		return dev_err_probe(dev, PTR_ERR(host->reset), "Cannot get CR reset\n");
	ret = mmc_regulator_get_supply(mmc);
	if (ret)
		return ret;
	if (IS_ERR(mmc->supply.vmmc))
		return dev_err_probe(dev, PTR_ERR(mmc->supply.vmmc), "Card supply required\n");
	host->buffer = dmam_alloc_coherent(dev, BUFFER_SIZE, &host->dma, GFP_KERNEL);
	if (!host->buffer)
		return -ENOMEM;
	ret = reset_control_deassert(host->reset);
	if (ret)
		return ret;
	ret = rtd1296_sd_hw_on(host);
	if (ret)
		goto assert_reset;
	mmc->ops = &rtd1296_sd_ops;
	mmc->f_min = host->rate / 512;
	mmc->f_max = min_t(unsigned long, host->rate / 4, 25000000);
	mmc->max_busy_timeout = 3000;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps |= MMC_CAP_NEEDS_POLL | MMC_CAP_WAIT_WHILE_BUSY;
	mmc->caps2 |= MMC_CAP2_NO_MMC | MMC_CAP2_NO_SDIO;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = BUFFER_SIZE / 512;
	mmc->max_req_size = BUFFER_SIZE;
	mmc->max_seg_size = BUFFER_SIZE;
	mmc->max_segs = BUFFER_SIZE / 512;
	ret = mmc_of_parse(mmc);
	if (ret)
		goto clocks_off;
	mmc->f_max = min_t(unsigned long, mmc->f_max, host->rate / 4);
	mmc->f_max = min(mmc->f_max, 25000000U);
	mmc->caps &= ~(MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED | MMC_CAP_UHS |
		       MMC_CAP_1_8V_DDR | MMC_CAP_1_2V_DDR);
	if (mmc->f_max < host->rate / 8 || (mmc->caps & MMC_CAP_8_BIT_DATA)) {
		ret = -EINVAL;
		goto clocks_off;
	}
	ret = devm_request_irq(dev, host->irq, rtd1296_sd_irq, 0, dev_name(dev), host);
	if (ret)
		goto clocks_off;
	host->irq_registered = true;
	host->irq_live = true;
	platform_set_drvdata(pdev, host);
	ret = mmc_add_host(mmc);
	if (!ret)
		return 0;
clocks_off:
	rtd1296_sd_hw_off(host);
assert_reset:
	reset_control_assert(host->reset);
	return ret;
}

static void rtd1296_sd_remove(struct platform_device *pdev)
{
	struct rtd1296_sd *host = platform_get_drvdata(pdev);

	mmc_remove_host(host->mmc);
	cancel_work_sync(&host->work);
	mutex_lock(&host->io_lock);
	rtd1296_sd_hw_off(host);
	mutex_unlock(&host->io_lock);
	reset_control_assert(host->reset);
}

static int rtd1296_sd_suspend(struct device *dev)
{
	struct rtd1296_sd *host = dev_get_drvdata(dev);

	flush_work(&host->work);
	mutex_lock(&host->io_lock);
	rtd1296_sd_hw_off(host);
	mutex_unlock(&host->io_lock);
	return 0;
}

static int rtd1296_sd_resume(struct device *dev)
{
	struct rtd1296_sd *host = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&host->io_lock);
	ret = rtd1296_sd_hw_on(host);
	mutex_unlock(&host->io_lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rtd1296_sd_pm_ops, rtd1296_sd_suspend,
			      rtd1296_sd_resume);

static void rtd1296_sd_shutdown(struct platform_device *pdev)
{
	struct rtd1296_sd *host = platform_get_drvdata(pdev);

	flush_work(&host->work);
	mutex_lock(&host->io_lock);
	rtd1296_sd_hw_off(host);
	mutex_unlock(&host->io_lock);
}

static const struct of_device_id rtd1296_sd_match[] = {
	{ .compatible = "realtek,rtd1296-sdmmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_sd_match);

static struct platform_driver rtd1296_sd_driver = {
	.probe = rtd1296_sd_probe,
	.remove = rtd1296_sd_remove,
	.shutdown = rtd1296_sd_shutdown,
	.driver = {
		.name = "rtd1296-sdmmc",
		.of_match_table = rtd1296_sd_match,
		.pm = pm_sleep_ptr(&rtd1296_sd_pm_ops),
	},
};
module_platform_driver(rtd1296_sd_driver);

MODULE_DESCRIPTION("Realtek RTD1296 SD card controller");
MODULE_LICENSE("GPL");
