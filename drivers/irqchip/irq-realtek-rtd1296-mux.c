// SPDX-License-Identifier: GPL-2.0-or-later
/* Realtek RTD1296 MISC and ISO interrupt muxes. */

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define RTD1296_MUX_NR_IRQS	32
#define RTD1296_MUX_NO_EN	0xff

struct rtd1296_mux_info {
	/* Enable bit of each status bit, RTD1296_MUX_NO_EN if not a source */
	u8 en_bit[RTD1296_MUX_NR_IRQS];
	u32 dedicated_sources;
};

struct rtd1296_mux;

struct rtd1296_mux_parent {
	struct rtd1296_mux *mux;
	u32 sources;
	int irq;
};

struct rtd1296_mux {
	void __iomem *status;
	void __iomem *enable;
	const struct rtd1296_mux_info *info;
	struct irq_domain *domain;
	struct rtd1296_mux_parent parents[2];
	raw_spinlock_t lock;
	/* Unmasked sources, by status bit */
	u32 unmasked;
};

static void rtd1296_mux_handle_irq(struct irq_desc *desc)
{
	struct rtd1296_mux_parent *parent = irq_desc_get_handler_data(desc);
	struct rtd1296_mux *mux = parent->mux;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long pending;
	unsigned int hwirq;

	chained_irq_enter(chip, desc);

	/* Masked sources can leave an interrupt pending at the parent. */
	pending = readl(mux->status) & READ_ONCE(mux->unmasked) & parent->sources;

	for_each_set_bit(hwirq, &pending, RTD1296_MUX_NR_IRQS)
		generic_handle_domain_irq(mux->domain, hwirq);

	chained_irq_exit(chip, desc);
}

static void rtd1296_mux_ack_irq(struct irq_data *d)
{
	struct rtd1296_mux *mux = irq_data_get_irq_chip_data(d);

	/* Keep wake events pending until normal interrupt delivery resumes. */
	if (irqd_is_wakeup_armed(d))
		return;
	writel(BIT(d->hwirq), mux->status);
}

static void rtd1296_mux_set_enable(struct irq_data *d, bool enable)
{
	struct rtd1296_mux *mux = irq_data_get_irq_chip_data(d);
	u32 mask = BIT(mux->info->en_bit[d->hwirq]);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&mux->lock, flags);

	val = readl(mux->enable);
	if (enable) {
		val |= mask;
		mux->unmasked |= BIT(d->hwirq);
	} else {
		val &= ~mask;
		mux->unmasked &= ~BIT(d->hwirq);
	}
	writel(val, mux->enable);

	raw_spin_unlock_irqrestore(&mux->lock, flags);
}

static void rtd1296_mux_mask_irq(struct irq_data *d)
{
	rtd1296_mux_set_enable(d, false);
}

static void rtd1296_mux_unmask_irq(struct irq_data *d)
{
	rtd1296_mux_set_enable(d, true);
}

static const struct irq_chip rtd1296_mux_irq_chip = {
	.name		= "rtd1296-mux",
	.irq_ack	= rtd1296_mux_ack_irq,
	.irq_mask	= rtd1296_mux_mask_irq,
	.irq_unmask	= rtd1296_mux_unmask_irq,
	.flags		= IRQCHIP_SKIP_SET_WAKE,
};

static int rtd1296_mux_irq_domain_map(struct irq_domain *d, unsigned int irq,
				      irq_hw_number_t hwirq)
{
	struct rtd1296_mux *mux = d->host_data;

	if (hwirq >= RTD1296_MUX_NR_IRQS ||
	    mux->info->en_bit[hwirq] == RTD1296_MUX_NO_EN)
		return -EINVAL;

	irq_set_chip_and_handler(irq, &rtd1296_mux_irq_chip, handle_level_irq);
	irq_set_status_flags(irq, IRQ_LEVEL);
	irq_set_chip_data(irq, mux);
	irq_set_probe(irq);

	return 0;
}

static const struct irq_domain_ops rtd1296_mux_irq_domain_ops = {
	.xlate	= irq_domain_xlate_onecell,
	.map	= rtd1296_mux_irq_domain_map,
};

static void __iomem *rtd1296_mux_map(struct platform_device *pdev,
				     unsigned int index)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);

	/* GPIO and reset controllers share these register windows. */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?:
	       IOMEM_ERR_PTR(-ENOMEM);
}

static int rtd1296_mux_probe(struct platform_device *pdev,
			     const struct rtd1296_mux_info *info)
{
	struct device_node *node = pdev->dev.of_node;
	u32 en_mask = 0, st_mask = 0;
	struct rtd1296_mux *mux;
	unsigned int i;
	int parent_count, ret;

	mux = devm_kzalloc(&pdev->dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->info = info;
	raw_spin_lock_init(&mux->lock);

	mux->status = rtd1296_mux_map(pdev, 0);
	if (IS_ERR(mux->status))
		return PTR_ERR(mux->status);

	mux->enable = rtd1296_mux_map(pdev, 1);
	if (IS_ERR(mux->enable))
		return PTR_ERR(mux->enable);

	parent_count = platform_irq_count(pdev);
	if (parent_count < 0)
		return parent_count;
	if (parent_count < 1 || parent_count > ARRAY_SIZE(mux->parents) ||
	    (parent_count == 2 && !info->dedicated_sources))
		return -EINVAL;
	for (i = 0; i < parent_count; i++) {
		ret = platform_get_irq(pdev, i);
		if (ret < 0)
			return ret;
		mux->parents[i].irq = ret;
		mux->parents[i].mux = mux;
	}

	/* Mask and clear all sources the boot firmware may have left on */
	for (i = 0; i < RTD1296_MUX_NR_IRQS; i++) {
		if (info->en_bit[i] == RTD1296_MUX_NO_EN)
			continue;
		en_mask |= BIT(info->en_bit[i]);
		st_mask |= BIT(i);
	}
	writel(readl(mux->enable) & ~en_mask, mux->enable);
	writel(st_mask, mux->status);

	mux->domain = irq_domain_create_linear(of_fwnode_handle(node),
					       RTD1296_MUX_NR_IRQS,
					       &rtd1296_mux_irq_domain_ops, mux);
	if (!mux->domain)
		return -ENOMEM;

	mux->parents[0].sources = st_mask;
	if (parent_count == 2) {
		mux->parents[0].sources &= ~info->dedicated_sources;
		mux->parents[1].sources = info->dedicated_sources;
	}
	for (i = 0; i < parent_count; i++)
		irq_set_chained_handler_and_data(mux->parents[i].irq,
						 rtd1296_mux_handle_irq,
						 &mux->parents[i]);

	return 0;
}

#define NO_EN	RTD1296_MUX_NO_EN

static const struct rtd1296_mux_info rtd1296_misc_mux_info = {
	.en_bit = {
		NO_EN, NO_EN, NO_EN, 3,     NO_EN, 5,     NO_EN, NO_EN,
		7,     NO_EN, 10,    11,    12,    6,     14,    15,
		NO_EN, NO_EN, NO_EN, 19,    20,    21,    22,    28,
		24,    NO_EN, 26,    27,    NO_EN, 29,    NO_EN, NO_EN,
	},
};

static const struct rtd1296_mux_info rtd1296_iso_mux_info = {
	.dedicated_sources = BIT(13),
	.en_bit = {
		NO_EN, NO_EN, 2,     NO_EN, NO_EN, 5,     NO_EN, NO_EN,
		8,     NO_EN, NO_EN, 11,    12,    13,    NO_EN, NO_EN,
		NO_EN, NO_EN, NO_EN, 19,    20,    NO_EN, NO_EN, NO_EN,
		NO_EN, NO_EN, NO_EN, NO_EN, NO_EN, 29,    30,    31,
	},
};

static int rtd1296_misc_mux_probe(struct platform_device *pdev,
				  struct device_node *parent)
{
	return rtd1296_mux_probe(pdev, &rtd1296_misc_mux_info);
}

static int rtd1296_iso_mux_probe(struct platform_device *pdev,
				 struct device_node *parent)
{
	return rtd1296_mux_probe(pdev, &rtd1296_iso_mux_info);
}

IRQCHIP_PLATFORM_DRIVER_BEGIN(rtd1296_mux)
IRQCHIP_MATCH("realtek,rtd1296-misc-irq-mux", rtd1296_misc_mux_probe)
IRQCHIP_MATCH("realtek,rtd1296-iso-irq-mux", rtd1296_iso_mux_probe)
IRQCHIP_PLATFORM_DRIVER_END(rtd1296_mux)
MODULE_DESCRIPTION("Realtek RTD1296 MISC/ISO interrupt mux driver");
MODULE_LICENSE("GPL");
