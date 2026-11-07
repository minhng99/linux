// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mtd/spi-nor.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/unaligned.h>

#define SFC_OPCODE	0x00
#define SFC_FORMAT	0x04
#define SFC_CLOCK	0x08
#define SFC_DIVIDER	GENMASK(7, 0)
#define SFC_AUTO_WAIT	0x18
#define SFC_AUTO_WREN	0x1c
#define SFC_ADDR_MODE	0x28
#define SFC_HAS_DATA	BIT(4)
#define SFC_HAS_ADDR	BIT(3)
#define SFC_DUMMY_MASK	GENMASK(2, 0)
#define SB2_SYNC	0x20

struct rtd1296_sfc {
	void __iomem *regs;
	void __iomem *window;
	struct regmap *sb2;
	resource_size_t size;
	unsigned long rate;
	u32 boot_clock;
};

static int rtd1296_sfc_sync(struct rtd1296_sfc *sfc)
{
	int ret;

	/* Publish controller writes before flushing SB2. */
	mb();
	ret = regmap_write(sfc->sb2, SB2_SYNC, 0);
	/* Complete the flush before accessing the aperture. */
	mb();
	return ret;
}

static int rtd1296_sfc_command(struct rtd1296_sfc *sfc, u8 opcode, u32 format)
{
	writel(opcode, sfc->regs + SFC_OPCODE);
	writel(format, sfc->regs + SFC_FORMAT);
	return rtd1296_sfc_sync(sfc);
}

static bool rtd1296_sfc_supports_op(struct spi_mem *mem,
				    const struct spi_mem_op *op)
{
	if (!spi_mem_default_supports_op(mem, op) || op->cmd.nbytes != 1 ||
	    op->cmd.buswidth != 1 || op->cmd.dtr ||
	    (op->addr.nbytes && (op->addr.nbytes != 3 ||
				op->addr.buswidth != 1 || op->addr.dtr)) ||
	    (op->dummy.nbytes && (op->dummy.buswidth != 1 || op->dummy.dtr)) ||
	    (op->data.nbytes && (op->data.buswidth != 1 || op->data.dtr)) ||
	    op->dummy.nbytes > SFC_DUMMY_MASK)
		return false;

	if (op->addr.nbytes || !op->data.nbytes)
		return true;
	if (op->data.dir == SPI_MEM_DATA_IN)
		return op->data.nbytes <= 8 - op->dummy.nbytes;

	return op->data.nbytes == 1 || op->data.nbytes == 2 ||
	       op->data.nbytes == 4;
}

static int rtd1296_sfc_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	if (op->addr.nbytes && op->data.nbytes &&
	    op->data.dir == SPI_MEM_DATA_OUT)
		op->data.nbytes = IS_ALIGNED(op->addr.val, 4) &&
				 op->data.nbytes >= 4 ? 4 : 1;
	else
		op->data.nbytes = min_t(unsigned int, op->data.nbytes, SZ_4K);
	return 0;
}

static int rtd1296_sfc_read_register(struct rtd1296_sfc *sfc,
				     const struct spi_mem_op *op, u32 format)
{
	u8 *data = op->data.buf.in;
	unsigned int offset, i;
	u32 value;
	int ret;

	/* Reissue register reads with additional leading dummy bytes. */
	for (offset = 0; offset < op->data.nbytes; offset += sizeof(value)) {
		ret = rtd1296_sfc_command(sfc, op->cmd.opcode, format + offset);
		if (ret)
			return ret;
		value = readl(sfc->window);
		for (i = 0; i < min_t(unsigned int, 4, op->data.nbytes - offset); i++)
			data[offset + i] = value >> (i * 8);
	}
	return 0;
}

static void rtd1296_sfc_read_data(struct rtd1296_sfc *sfc,
				  const struct spi_mem_op *op)
{
	u32 offset = op->addr.val;
	u8 *data = op->data.buf.in;
	unsigned int count = op->data.nbytes;

	while (count) {
		if (IS_ALIGNED(offset, 4) && count >= 4) {
			put_unaligned_le32(readl(sfc->window + offset), data);
			offset += 4;
			data += 4;
			count -= 4;
		} else {
			*data++ = readb(sfc->window + offset++);
			count--;
		}
	}
}

static int rtd1296_sfc_write_data(struct rtd1296_sfc *sfc,
				  const struct spi_mem_op *op)
{
	void __iomem *dst = sfc->window + op->addr.val;
	const void *data = op->data.buf.out;

	if (!IS_ALIGNED(op->addr.val, op->data.nbytes))
		return -EINVAL;

	switch (op->data.nbytes) {
	case 1:
		writeb(*(const u8 *)data, dst);
		break;
	case 2:
		writew(get_unaligned_le16(data), dst);
		break;
	case 4:
		writel(get_unaligned_le32(data), dst);
		break;
	default:
		return -EOPNOTSUPP;
	}
	return rtd1296_sfc_sync(sfc);
}

static int rtd1296_sfc_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct rtd1296_sfc *sfc = spi_controller_get_devdata(mem->spi->controller);
	u32 format = op->dummy.nbytes;
	unsigned int divider;
	int ret, restore;

	if (!op->max_freq)
		return -EINVAL;
	divider = DIV_ROUND_UP(sfc->rate, op->max_freq);
	if (!divider || divider > SFC_DIVIDER + 1)
		return -EINVAL;
	divider = max(divider, (sfc->boot_clock & SFC_DIVIDER) + 1);
	if (op->addr.val >= sfc->size ||
	    op->data.nbytes > sfc->size - op->addr.val)
		return -ERANGE;
	writel((sfc->boot_clock & ~SFC_DIVIDER) | (divider - 1),
	       sfc->regs + SFC_CLOCK);
	if (op->addr.nbytes)
		format |= SFC_HAS_ADDR;
	if (op->data.nbytes)
		format |= SFC_HAS_DATA;

	if (!op->addr.nbytes && op->data.nbytes &&
	    op->data.dir == SPI_MEM_DATA_IN) {
		ret = rtd1296_sfc_read_register(sfc, op, format);
	} else {
		ret = rtd1296_sfc_command(sfc, op->cmd.opcode, format);
		if (ret)
			goto restore;
		if (!op->data.nbytes) {
			readb(sfc->window + op->addr.val);
			ret = rtd1296_sfc_sync(sfc);
		} else if (op->data.dir == SPI_MEM_DATA_OUT) {
			ret = rtd1296_sfc_write_data(sfc, op);
		} else {
			rtd1296_sfc_read_data(sfc, op);
		}
	}
restore:
	writel(sfc->boot_clock, sfc->regs + SFC_CLOCK);
	restore = rtd1296_sfc_command(sfc, SPINOR_OP_READ, SFC_HAS_ADDR | SFC_HAS_DATA);
	return ret ?: restore;
}

static const struct spi_controller_mem_ops rtd1296_sfc_mem_ops = {
	.supports_op = rtd1296_sfc_supports_op,
	.adjust_op_size = rtd1296_sfc_adjust_op_size,
	.exec_op = rtd1296_sfc_exec_op,
};

static const struct spi_controller_mem_caps rtd1296_sfc_mem_caps = {
	.per_op_freq = true,
};

static int rtd1296_sfc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct rtd1296_sfc *sfc;
	struct resource *res;
	struct clk *clk;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*sfc));
	if (!ctlr)
		return -ENOMEM;
	sfc = spi_controller_get_devdata(ctlr);
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot enable input clock\n");
	sfc->rate = clk_get_rate(clk);
	if (!sfc->rate)
		return -EINVAL;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ctrl");
	if (!res || resource_size(res) < SFC_ADDR_MODE + sizeof(u32))
		return -EINVAL;
	sfc->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(sfc->regs))
		return PTR_ERR(sfc->regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "flash");
	if (!res || resource_size(res) > SZ_16M)
		return -EINVAL;
	sfc->size = resource_size(res);
	sfc->window = devm_ioremap_resource(dev, res);
	if (IS_ERR(sfc->window))
		return PTR_ERR(sfc->window);
	sfc->sb2 = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sb2");
	if (IS_ERR(sfc->sb2))
		return dev_err_probe(dev, PTR_ERR(sfc->sb2), "cannot get SB2 regmap\n");

	/* Keep serial rates within the entry pad timing limit. */
	sfc->boot_clock = readl(sfc->regs + SFC_CLOCK);
	ctlr->max_speed_hz = sfc->rate / ((sfc->boot_clock & SFC_DIVIDER) + 1);
	ctlr->min_speed_hz = DIV_ROUND_UP(sfc->rate, SFC_DIVIDER + 1);
	writel(SPINOR_OP_RDSR, sfc->regs + SFC_AUTO_WAIT);
	writel(SPINOR_OP_WREN, sfc->regs + SFC_AUTO_WREN);
	writel(0, sfc->regs + SFC_ADDR_MODE);
	ret = rtd1296_sfc_command(sfc, SPINOR_OP_READ, SFC_HAS_ADDR | SFC_HAS_DATA);
	if (ret)
		return ret;

	device_set_node(&ctlr->dev, dev_fwnode(dev));
	ctlr->num_chipselect = 1;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->mem_ops = &rtd1296_sfc_mem_ops;
	ctlr->mem_caps = &rtd1296_sfc_mem_caps;

	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id rtd1296_sfc_of_match[] = {
	{ .compatible = "realtek,rtd1296-sfc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_sfc_of_match);

static struct platform_driver rtd1296_sfc_driver = {
	.probe = rtd1296_sfc_probe,
	.driver = {
		.name = "rtd1296-sfc",
		.of_match_table = rtd1296_sfc_of_match,
	},
};
module_platform_driver(rtd1296_sfc_driver);

MODULE_DESCRIPTION("Realtek RTD1296 serial flash controller");
MODULE_LICENSE("GPL");
