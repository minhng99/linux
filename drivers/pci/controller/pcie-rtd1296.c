// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqdomain.h>
#include <linux/logic_iomem.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

#define PCIE_CONTROL		0xc00
#define PCIE_INTERRUPT_ENABLE	0xc04
#define PCIE_REQUEST		0xc14
#define PCIE_MDIO		0xc1c
#define PCIE_CFG_START		0xc38
#define PCIE_CFG_ENABLE		0xc3c
#define PCIE_CFG_STATUS		0xc40
#define PCIE_CFG_ADDRESS		0xc44
#define PCIE_CFG_WRITE		0xc48
#define PCIE_CFG_READ		0xc4c
#define PCIE_POWER_CONTROL	0xc6c
#define PCIE_IO_STATUS		0xc74
#define PCIE_IO_TIMEOUT		0xc78
#define PCIE_DLLP_STATUS		0xc7c
#define PCIE_LINK_STATUS		0xcb4
#define PCIE_MSI_ADDRESS		0xcd0
#define PCIE_MSI_DATA		0xcd4
#define PCIE_WINDOW_BASE		0xcfc
#define PCIE_WINDOW_MASK		0xd00
#define PCIE_WINDOW_TARGET	0xd04
#define PCIE_TRANSLATE		BIT(20)
#define PCIE_MEMORY		BIT(19)
#define PCIE_MDIO_OE		BIT(18)
#define PCIE_MDIO_RESET_N		BIT(17)
#define PCIE_INDIRECT_CFG		BIT(5)
#define PCIE_DIRECT_CFG		BIT(4)
#define PCIE_LTSSM		BIT(1)
#define PCIE_LINK_UP		BIT(11)
#define PCIE_MDIO_BUSY		BIT(7)
#define PCIE_CFG_DONE		BIT(0)
#define PCIE_CFG_ERROR		BIT(1)
#define PCIE_EXIT_L1		BIT(1)
#define PCIE_LTSSM_STATE		GENMASK(9, 4)
#define PCIE_LTSSM_L0		0x11
#define PCIE_MSI_ENABLE		BIT(12)
#define PCIE_MSI_PENDING		BIT(16)
#define PCIE_APERTURE_SIZE	SZ_2K
#define PCIE_LOGICAL_SIZE		SZ_16M
#define PCIE_MEMORY_CONTROL	(PCIE_TRANSLATE | PCIE_MEMORY | PCIE_MDIO_OE | \
				 PCIE_MDIO_RESET_N | PCIE_LTSSM)

struct rtd1296_pcie_setting {
	u8 reg;
	u16 value;
};

static const struct rtd1296_pcie_setting rtd1296_gen1_settings[] = {
	{ 0x08, 0x3181 }, { 0x11, 0xb200 }, { 0x00, 0x0010 },
};

static const struct rtd1296_pcie_setting rtd1296_gen2_settings[] = {
	{ 0x03, 0x27e9 }, { 0x04, 0x52f5 }, { 0x05, 0xead7 },
	{ 0x06, 0x000c }, { 0x0a, 0xa653 }, { 0x01, 0xa84a },
	{ 0x20, 0xc466 }, { 0x2b, 0xb803 },
	{ 0x43, 0x27e9 }, { 0x44, 0x52f5 }, { 0x45, 0xead7 },
	{ 0x46, 0x000c }, { 0x4a, 0xa653 }, { 0x41, 0xa84a },
	{ 0x60, 0xd477 }, { 0x6b, 0xa803 },
	{ 0x5a, 0x0122 }, { 0x09, 0x521c }, { 0x09, 0x501c },
	{ 0x09, 0x521c }, { 0x24, 0x0828 }, { 0x64, 0x0828 },
};

struct rtd1296_pcie_variant {
	const struct rtd1296_pcie_setting *settings;
	unsigned int count;
	u32 lanes;
};

static const struct rtd1296_pcie_variant rtd1296_gen1 = {
	.settings = rtd1296_gen1_settings,
	.count = ARRAY_SIZE(rtd1296_gen1_settings),
	.lanes = GENMASK(3, 0),
};

static const struct rtd1296_pcie_variant rtd1296_gen2 = {
	.settings = rtd1296_gen2_settings,
	.count = ARRAY_SIZE(rtd1296_gen2_settings),
	.lanes = GENMASK(19, 16),
};

struct rtd1296_pcie {
	struct device *dev;
	struct pci_host_bridge *bridge;
	const struct rtd1296_pcie_variant *variant;
	void __iomem *regs;
	void __iomem *aperture;
	struct regmap *sds;
	struct clk *clock;
	struct gpio_desc *perst;
	struct reset_control *serdes;
	struct reset_control_bulk_data resets[7];
	struct resource logical;
	struct resource buses;
	struct irq_domain *msi_domain;
	dma_addr_t msi_address;
	int msi_irq;
	bool msi_used;
	raw_spinlock_t lock;
	u32 pci_base;
	u32 aperture_base;
	bool clocked;
	bool serdes_enabled;
	bool accessible;
};

static bool rtd1296_pcie_link(struct rtd1296_pcie *pcie)
{
	return pcie->accessible && (readl(pcie->regs + PCIE_LINK_STATUS) & PCIE_LINK_UP);
}

static void rtd1296_pcie_memory_mode(struct rtd1296_pcie *pcie)
{
	writel(pcie->pci_base, pcie->regs + PCIE_WINDOW_TARGET);
	writel(PCIE_MEMORY_CONTROL, pcie->regs + PCIE_CONTROL);
	readl(pcie->regs + PCIE_CONTROL);
}

static int rtd1296_pcie_access_begin(struct rtd1296_pcie *pcie, u32 *power)
{
	u32 status;
	int ret;

	if (!rtd1296_pcie_link(pcie))
		return -ENODEV;
	*power = readl(pcie->regs + PCIE_POWER_CONTROL);
	/* Configuration requests cannot wake the link from L1. */
	writel(*power | PCIE_EXIT_L1, pcie->regs + PCIE_POWER_CONTROL);
	ret = readl_poll_timeout_atomic(pcie->regs + PCIE_LINK_STATUS, status,
					FIELD_GET(PCIE_LTSSM_STATE, status) == PCIE_LTSSM_L0,
					1, 10000);
	if (ret) {
		writel(*power, pcie->regs + PCIE_POWER_CONTROL);
		readl(pcie->regs + PCIE_POWER_CONTROL);
	}
	return ret;
}

static void rtd1296_pcie_access_end(struct rtd1296_pcie *pcie, u32 power)
{
	rtd1296_pcie_memory_mode(pcie);
	writel(power, pcie->regs + PCIE_POWER_CONTROL);
	readl(pcie->regs + PCIE_POWER_CONTROL);
}

static int rtd1296_pcie_config(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *value, bool write)
{
	struct rtd1296_pcie *pcie = bus->sysdata;
	unsigned long flags;
	u32 enables, status, data, power;
	unsigned int shift;
	int ret;

	if (where < 0 || where >= PCI_CFG_SPACE_EXP_SIZE ||
	    (size != 1 && size != 2 && size != 4) || (where & (size - 1)))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (!write)
		*value = ~0U;
	if (devfn || bus->number != pcie->buses.start)
		return PCIBIOS_DEVICE_NOT_FOUND;
	raw_spin_lock_irqsave(&pcie->lock, flags);
	ret = rtd1296_pcie_access_begin(pcie, &power);
	if (ret) {
		ret = ret == -ENODEV ? PCIBIOS_DEVICE_NOT_FOUND : PCIBIOS_SET_FAILED;
		goto unlock;
	}
	shift = (where & 3) * 8;
	enables = GENMASK(size - 1, 0) << (where & 3);
	writel(PCIE_MDIO_OE | PCIE_LTSSM | PCIE_INDIRECT_CFG, pcie->regs + PCIE_CONTROL);
	writel(write ? 0x12 : 0x10, pcie->regs + PCIE_REQUEST);
	writel(PCIE_CFG_DONE | PCIE_CFG_ERROR, pcie->regs + PCIE_CFG_STATUS);
	writel(where & ~3, pcie->regs + PCIE_CFG_ADDRESS);
	if (write)
		writel(*value << shift, pcie->regs + PCIE_CFG_WRITE);
	writel((enables << 4) | BIT(1) | write, pcie->regs + PCIE_CFG_ENABLE);
	writel(1, pcie->regs + PCIE_CFG_START);
	ret = readl_poll_timeout_atomic(pcie->regs + PCIE_CFG_STATUS, status,
					status & (PCIE_CFG_DONE | PCIE_CFG_ERROR), 1, 10000);
	if (ret || (status & PCIE_CFG_ERROR)) {
		ret = PCIBIOS_SET_FAILED;
	} else {
		if (!write) {
			data = readl(pcie->regs + PCIE_CFG_READ);
			*value = (data >> shift) & (~0U >> (32 - size * 8));
		}
		ret = PCIBIOS_SUCCESSFUL;
	}
	rtd1296_pcie_access_end(pcie, power);
unlock:
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
	return ret;
}

static int rtd1296_pcie_read_config(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 *value)
{
	return rtd1296_pcie_config(bus, devfn, where, size, value, false);
}

static int rtd1296_pcie_write_config(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 value)
{
	return rtd1296_pcie_config(bus, devfn, where, size, &value, true);
}

static struct pci_ops rtd1296_pcie_ops = {
	.read = rtd1296_pcie_read_config,
	.write = rtd1296_pcie_write_config,
};

static void rtd1296_pcie_fixup(struct pci_dev *pdev)
{
	struct rtd1296_pcie *pcie;

	if (pdev->bus->ops != &rtd1296_pcie_ops)
		return;
	pcie = pdev->bus->sysdata;
	pdev->non_mappable_bars = true;
	pdev->no_msi = !pcie->msi_domain;
}
DECLARE_PCI_FIXUP_EARLY(PCI_ANY_ID, PCI_ANY_ID, rtd1296_pcie_fixup);

static bool rtd1296_pcie_cycle(struct rtd1296_pcie *pcie, unsigned int offset,
			       unsigned int size, u32 *data, bool write)
{
	void __iomem *address = pcie->aperture + (offset & (PCIE_APERTURE_SIZE - 1));
	u32 status, dllp;

	writel(BIT(0), pcie->regs + PCIE_IO_STATUS);
	writel(0x1f, pcie->regs + PCIE_DLLP_STATUS);
	writel((pcie->pci_base + offset) & ~(PCIE_APERTURE_SIZE - 1),
	       pcie->regs + PCIE_WINDOW_TARGET);
	readl(pcie->regs + PCIE_WINDOW_TARGET);
	if (write) {
		switch (size) {
		case 1:
			__raw_writeb(*data, address);
			break;
		case 2:
			__raw_writew(*data, address);
			break;
		case 4:
			__raw_writel(*data, address);
			break;
		}
	} else {
		switch (size) {
		case 1:
			*data = __raw_readb(address);
			break;
		case 2:
			*data = __raw_readw(address);
			break;
		case 4:
			*data = __raw_readl(address);
			break;
		}
	}
	status = readl(pcie->regs + PCIE_IO_STATUS);
	dllp = readl(pcie->regs + PCIE_DLLP_STATUS);
	return !(status & BIT(0)) && !(dllp & 0x1f);
}

static unsigned long rtd1296_pcie_read(void *priv, unsigned int offset, int size)
{
	struct rtd1296_pcie *pcie = priv;
	unsigned long flags, value = ~0UL;
	u32 words[2], power;
	bool good;

	if ((size != 1 && size != 2 && size != 4 && size != 8) ||
	    offset > PCIE_LOGICAL_SIZE - size || (offset & (size - 1)))
		return value;
	raw_spin_lock_irqsave(&pcie->lock, flags);
	if (rtd1296_pcie_access_begin(pcie, &power))
		goto unlock;
	good = rtd1296_pcie_cycle(pcie, offset, min(size, 4), &words[0], false);
	if (good && size == 8)
		good = rtd1296_pcie_cycle(pcie, offset + 4, 4, &words[1], false);
	if (good) {
		if (size == 8)
			memcpy(&value, words, sizeof(words));
		else
			value = words[0];
	} else {
		dev_err_ratelimited(pcie->dev, "PCIe memory read failed\n");
	}
	rtd1296_pcie_access_end(pcie, power);
unlock:
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
	return value;
}

static void rtd1296_pcie_write(void *priv, unsigned int offset, int size,
			       unsigned long value)
{
	struct rtd1296_pcie *pcie = priv;
	unsigned long flags;
	u32 words[2], power;
	bool good;

	if ((size != 1 && size != 2 && size != 4 && size != 8) ||
	    offset > PCIE_LOGICAL_SIZE - size || (offset & (size - 1)))
		return;
	if (size == 8)
		memcpy(words, &value, sizeof(words));
	else
		words[0] = value;
	raw_spin_lock_irqsave(&pcie->lock, flags);
	if (rtd1296_pcie_access_begin(pcie, &power))
		goto unlock;
	good = rtd1296_pcie_cycle(pcie, offset, min(size, 4), &words[0], true);
	if (good && size == 8)
		good = rtd1296_pcie_cycle(pcie, offset + 4, 4, &words[1], true);
	if (!good)
		dev_err_ratelimited(pcie->dev, "PCIe memory write failed\n");
	rtd1296_pcie_access_end(pcie, power);
unlock:
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
}

static const struct logic_iomem_ops rtd1296_pcie_memory_ops = {
	.read = rtd1296_pcie_read,
	.write = rtd1296_pcie_write,
};

static long rtd1296_pcie_map(struct resource *resource, unsigned long offset,
			     size_t size, const struct logic_iomem_ops **ops, void **priv)
{
	if (!size || size > PCIE_LOGICAL_SIZE || offset > PCIE_LOGICAL_SIZE - size)
		return -EINVAL;
	*priv = container_of(resource, struct rtd1296_pcie, logical);
	*ops = &rtd1296_pcie_memory_ops;
	return offset;
}

static const struct logic_iomem_region_ops rtd1296_pcie_region_ops = {
	.map = rtd1296_pcie_map,
};

static int rtd1296_pcie_phy_init(struct rtd1296_pcie *pcie)
{
	u32 status;
	unsigned int i;
	int ret;

	writel(3, pcie->regs + PCIE_MDIO);
	fsleep(1000);
	writel(0, pcie->regs + PCIE_MDIO);
	fsleep(1000);
	for (i = 0; i < pcie->variant->count; i++) {
		const struct rtd1296_pcie_setting *setting = &pcie->variant->settings[i];

		ret = readl_poll_timeout(pcie->regs + PCIE_MDIO, status,
					 !(status & PCIE_MDIO_BUSY), 10, 50000);
		if (ret)
			return ret;
		writel((u32)setting->value << 16 | (u32)setting->reg << 8 | BIT(0),
		       pcie->regs + PCIE_MDIO);
		fsleep(1000);
		ret = readl_poll_timeout(pcie->regs + PCIE_MDIO, status,
					 !(status & PCIE_MDIO_BUSY), 10, 50000);
		if (ret)
			return ret;
	}
	return 0;
}

static void rtd1296_pcie_stop(void *data)
{
	struct rtd1296_pcie *pcie = data;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&pcie->lock, flags);
	if (pcie->accessible)
		writel(0, pcie->regs + PCIE_INTERRUPT_ENABLE);
	pcie->accessible = false;
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
	gpiod_set_value_cansleep(pcie->perst, 1);
	ret = reset_control_bulk_assert(ARRAY_SIZE(pcie->resets), pcie->resets);
	if (ret)
		dev_err(pcie->dev, "Cannot assert controller resets: %d\n", ret);
	if (pcie->clocked) {
		clk_disable_unprepare(pcie->clock);
		pcie->clocked = false;
	}
	if (pcie->serdes_enabled) {
		ret = reset_control_assert(pcie->serdes);
		if (ret)
			dev_err(pcie->dev, "Cannot release SerDes reset reference: %d\n", ret);
		else
			pcie->serdes_enabled = false;
	}
}

static int rtd1296_pcie_start(struct rtd1296_pcie *pcie, u32 revision)
{
	u32 status;
	int ret;

	ret = reset_control_deassert(pcie->serdes);
	if (ret)
		return ret;
	pcie->serdes_enabled = true;
	ret = reset_control_bulk_assert(ARRAY_SIZE(pcie->resets), pcie->resets);
	if (ret)
		goto stop;
	ret = clk_prepare_enable(pcie->clock);
	if (ret)
		goto stop;
	pcie->clocked = true;
	if (revision != 0x20000) {
		ret = regmap_update_bits(pcie->sds, 0x14, GENMASK(2, 0), 1);
		if (ret)
			goto stop;
		ret = regmap_update_bits(pcie->sds, 0, pcie->variant->lanes, 0);
		if (ret)
			goto stop;
		ret = regmap_update_bits(pcie->sds, 8, 0xff, 0x51);
		if (ret)
			goto stop;
	}
	ret = reset_control_bulk_deassert(ARRAY_SIZE(pcie->resets), pcie->resets);
	if (ret)
		goto stop;
	pcie->accessible = true;
	writel(PCIE_TRANSLATE | PCIE_MDIO_OE | PCIE_DIRECT_CFG, pcie->regs + PCIE_CONTROL);
	readl(pcie->regs + PCIE_CONTROL);
	ret = rtd1296_pcie_phy_init(pcie);
	if (ret)
		goto stop;
	msleep(100);
	gpiod_set_value_cansleep(pcie->perst, 0);
	writel(PCIE_MDIO_OE | PCIE_DIRECT_CFG | PCIE_LTSSM, pcie->regs + PCIE_CONTROL);
	msleep(50);
	writel(BIT(16) | BIT(8) | BIT(5), pcie->regs + 0x710);
	ret = readl_poll_timeout(pcie->regs + PCIE_LINK_STATUS, status,
				 status & PCIE_LINK_UP, 1000, 1000000);
	if (!ret)
		msleep(100);
	dev_info(pcie->dev, "link %s\n", ret ? "down" : "up");
	writew(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, pcie->regs + PCI_COMMAND);
	writel(0, pcie->regs + PCIE_INTERRUPT_ENABLE);
	writel(pcie->aperture_base, pcie->regs + PCIE_WINDOW_BASE);
	writel(~(PCIE_APERTURE_SIZE - 1), pcie->regs + PCIE_WINDOW_MASK);
	writel(BIT(24) | BIT(0), pcie->regs + PCIE_IO_TIMEOUT);
	rtd1296_pcie_memory_mode(pcie);
	return 0;
stop:
	rtd1296_pcie_stop(pcie);
	return ret;
}

static void rtd1296_pcie_release_memory(void *data)
{
	struct rtd1296_pcie *pcie = data;
	int ret;

	rtd1296_pcie_stop(pcie);
	ret = logic_iomem_remove_region(&pcie->logical);
	if (ret) {
		/* Retain callback state for mappings left by a consumer. */
		get_device(&pcie->bridge->dev);
		dev_err(pcie->dev, "Logical mappings remain active: %d\n", ret);
	}
}

static void rtd1296_pcie_release_bus(void *data)
{
	struct rtd1296_pcie *pcie = data;

	pci_stop_root_bus(pcie->bridge->bus);
	pci_remove_root_bus(pcie->bridge->bus);
}

static void rtd1296_pcie_free_bridge(void *data)
{
	pci_free_host_bridge(data);
}

#ifdef CONFIG_PCI_MSI
static void rtd1296_pcie_msi_message(struct irq_data *data, struct msi_msg *msg)
{
	struct rtd1296_pcie *pcie = irq_data_get_irq_chip_data(data);

	msg->address_lo = lower_32_bits(pcie->msi_address);
	msg->address_hi = 0;
	msg->data = 0;
}

static struct irq_chip rtd1296_pcie_msi_chip = {
	.name = "RTD1296 MSI",
	.irq_compose_msi_msg = rtd1296_pcie_msi_message,
};

static int rtd1296_pcie_msi_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int count, void *args)
{
	struct rtd1296_pcie *pcie = domain->host_data;
	unsigned long flags;

	/* The receiver has one pending latch shared by all message data. */
	if (count != 1)
		return -ENOSPC;
	raw_spin_lock_irqsave(&pcie->lock, flags);
	if (pcie->msi_used) {
		raw_spin_unlock_irqrestore(&pcie->lock, flags);
		return -ENOSPC;
	}
	pcie->msi_used = true;
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
	irq_domain_set_info(domain, virq, 0, &rtd1296_pcie_msi_chip, pcie,
			    handle_simple_irq, NULL, NULL);
	raw_spin_lock_irqsave(&pcie->lock, flags);
	writel(PCIE_MSI_PENDING, pcie->regs + PCIE_MSI_DATA);
	writel(PCIE_MSI_ENABLE, pcie->regs + PCIE_INTERRUPT_ENABLE);
	readl(pcie->regs + PCIE_INTERRUPT_ENABLE);
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
	return 0;
}

static void rtd1296_pcie_msi_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int count)
{
	struct rtd1296_pcie *pcie = domain->host_data;
	unsigned long flags;

	raw_spin_lock_irqsave(&pcie->lock, flags);
	if (pcie->accessible) {
		writel(0, pcie->regs + PCIE_INTERRUPT_ENABLE);
		writel(PCIE_MSI_PENDING, pcie->regs + PCIE_MSI_DATA);
		readl(pcie->regs + PCIE_INTERRUPT_ENABLE);
	}
	pcie->msi_used = false;
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
	synchronize_irq(pcie->msi_irq);
}

static const struct irq_domain_ops rtd1296_pcie_msi_domain_ops = {
	.alloc = rtd1296_pcie_msi_alloc,
	.free = rtd1296_pcie_msi_free,
};

static const struct msi_parent_ops rtd1296_pcie_msi_parent_ops = {
	.required_flags = MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
			  MSI_FLAG_NO_AFFINITY,
	.supported_flags = MSI_GENERIC_FLAGS_MASK | MSI_FLAG_PCI_MSIX,
	.bus_select_token = DOMAIN_BUS_PCI_MSI,
	.prefix = "RTD1296-",
	.init_dev_msi_info = msi_lib_init_dev_msi_info,
};

static irqreturn_t rtd1296_pcie_msi_handler(int irq, void *data)
{
	struct rtd1296_pcie *pcie = data;
	unsigned long flags;
	bool allocated;
	u32 status;

	raw_spin_lock_irqsave(&pcie->lock, flags);
	if (!pcie->accessible) {
		raw_spin_unlock_irqrestore(&pcie->lock, flags);
		return IRQ_NONE;
	}
	status = readl(pcie->regs + PCIE_MSI_DATA);
	if (!(status & PCIE_MSI_PENDING)) {
		raw_spin_unlock_irqrestore(&pcie->lock, flags);
		return IRQ_NONE;
	}
	writel(PCIE_MSI_PENDING, pcie->regs + PCIE_MSI_DATA);
	readl(pcie->regs + PCIE_MSI_DATA);
	allocated = pcie->msi_used;
	raw_spin_unlock_irqrestore(&pcie->lock, flags);
	if (allocated)
		generic_handle_domain_irq(pcie->msi_domain, 0);
	return IRQ_HANDLED;
}

static void rtd1296_pcie_msi_remove(void *data)
{
	struct rtd1296_pcie *pcie = data;

	dev_set_msi_domain(&pcie->bridge->dev, NULL);
	irq_domain_remove(pcie->msi_domain);
	pcie->msi_domain = NULL;
}

static int rtd1296_pcie_msi_init(struct platform_device *pdev,
				 struct rtd1296_pcie *pcie)
{
	struct irq_domain_info info = {
		.fwnode = dev_fwnode(&pdev->dev),
		.ops = &rtd1296_pcie_msi_domain_ops,
		.host_data = pcie,
		.size = 1,
	};
	int ret;

	if (!device_property_read_bool(&pdev->dev, "msi-controller"))
		return 0;
	pcie->msi_irq = platform_get_irq(pdev, 0);
	if (pcie->msi_irq < 0)
		return pcie->msi_irq;
	if (!dmam_alloc_coherent(&pdev->dev, sizeof(u32), &pcie->msi_address, GFP_KERNEL))
		return -ENOMEM;
	pcie->msi_domain = msi_create_parent_irq_domain(&info, &rtd1296_pcie_msi_parent_ops);
	if (!pcie->msi_domain)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, rtd1296_pcie_msi_remove, pcie);
	if (ret)
		return ret;
	ret = devm_request_irq(&pdev->dev, pcie->msi_irq, rtd1296_pcie_msi_handler,
			       IRQF_SHARED, dev_name(&pdev->dev), pcie);
	if (ret)
		return ret;
	writel(lower_32_bits(pcie->msi_address), pcie->regs + PCIE_MSI_ADDRESS);
	writel(PCIE_MSI_PENDING, pcie->regs + PCIE_MSI_DATA);
	dev_set_msi_domain(&pcie->bridge->dev, pcie->msi_domain);
	return 0;
}
#else
static int rtd1296_pcie_msi_init(struct platform_device *pdev,
				 struct rtd1296_pcie *pcie)
{
	return 0;
}
#endif

static int rtd1296_pcie_ranges(struct rtd1296_pcie *pcie)
{
	struct of_pci_range_parser parser;
	struct of_pci_range range;
	bool found = false;
	u32 buses[2];
	int ret;

	ret = of_pci_range_parser_init(&parser, pcie->dev->of_node);
	if (ret)
		return ret;
	for_each_of_pci_range(&parser, &range) {
		if (found || (range.flags & IORESOURCE_TYPE_BITS) != IORESOURCE_MEM ||
		    range.size != PCIE_APERTURE_SIZE || range.cpu_addr != pcie->aperture_base ||
		    range.pci_addr > U32_MAX - PCIE_LOGICAL_SIZE + 1 ||
		    (range.pci_addr & (PCIE_LOGICAL_SIZE - 1)))
			return -EINVAL;
		pcie->pci_base = range.pci_addr;
		found = true;
	}
	if (!found)
		return -EINVAL;
	ret = of_property_read_u32_array(pcie->dev->of_node, "bus-range", buses, 2);
	if (ret)
		return ret;
	if (buses[0] > 255 || buses[0] != buses[1])
		return -EINVAL;
	pcie->buses = (struct resource) {
		.name = "PCI bus",
		.start = buses[0],
		.end = buses[1],
		.flags = IORESOURCE_BUS,
	};
	return 0;
}

static int rtd1296_pcie_probe(struct platform_device *pdev)
{
	static const char * const reset_names[] = {
		"stitch", "main", "core", "power", "non-stitch", "phy", "mdio",
	};
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct rtd1296_pcie *pcie;
	struct resource *resource;
	struct regmap *sb2;
	unsigned int i;
	u32 revision;
	int ret;

	/* Register logical BAR resources separately from the hardware aperture. */
	bridge = pci_alloc_host_bridge(sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;
	bridge->dev.parent = dev;
	ret = devm_add_action_or_reset(dev, rtd1296_pcie_free_bridge, bridge);
	if (ret)
		return ret;
	pcie = pci_host_bridge_priv(bridge);
	pcie->dev = dev;
	pcie->bridge = bridge;
	pcie->variant = device_get_match_data(dev);
	raw_spin_lock_init(&pcie->lock);
	pcie->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &resource);
	if (IS_ERR(pcie->regs))
		return PTR_ERR(pcie->regs);
	if (resource_size(resource) < PCIE_WINDOW_TARGET + sizeof(u32))
		return -EINVAL;
	pcie->aperture = devm_platform_get_and_ioremap_resource(pdev, 1, &resource);
	if (IS_ERR(pcie->aperture))
		return PTR_ERR(pcie->aperture);
	if (resource_size(resource) != PCIE_APERTURE_SIZE || resource->start > U32_MAX ||
	    (resource->start & (PCIE_APERTURE_SIZE - 1)))
		return -EINVAL;
	pcie->aperture_base = resource->start;
	ret = rtd1296_pcie_ranges(pcie);
	if (ret)
		return dev_err_probe(dev, ret, "Invalid PCI aperture\n");
	pcie->sds = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sds");
	if (IS_ERR(pcie->sds))
		return dev_err_probe(dev, PTR_ERR(pcie->sds), "Cannot get PHY control\n");
	sb2 = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sb2");
	if (IS_ERR(sb2))
		return dev_err_probe(dev, PTR_ERR(sb2), "Cannot get revision registers\n");
	ret = regmap_read(sb2, 0x204, &revision);
	if (ret)
		return ret;
	pcie->clock = devm_clk_get(dev, NULL);
	if (IS_ERR(pcie->clock))
		return dev_err_probe(dev, PTR_ERR(pcie->clock), "Cannot get controller clock\n");
	pcie->serdes = devm_reset_control_get_shared(dev, "serdes");
	if (IS_ERR(pcie->serdes))
		return dev_err_probe(dev, PTR_ERR(pcie->serdes), "Cannot get SerDes reset\n");
	for (i = 0; i < ARRAY_SIZE(pcie->resets); i++)
		pcie->resets[i].id = reset_names[i];
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(pcie->resets), pcie->resets);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get controller resets\n");
	pcie->perst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pcie->perst))
		return dev_err_probe(dev, PTR_ERR(pcie->perst), "Cannot get endpoint reset\n");
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	platform_set_drvdata(pdev, pcie);
	ret = rtd1296_pcie_start(pcie, revision);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot start controller\n");
	ret = devm_add_action_or_reset(dev, rtd1296_pcie_stop, pcie);
	if (ret)
		return ret;
	pcie->logical.name = "RTD1296 PCIe logical memory";
	pcie->logical.flags = IORESOURCE_MEM | IORESOURCE_WINDOW;
	ret = logic_iomem_alloc_region(&pcie->logical, &rtd1296_pcie_region_ops,
				       PCIE_LOGICAL_SIZE, 1ULL << 56, RESOURCE_SIZE_MAX,
				       PCIE_LOGICAL_SIZE);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot allocate logical PCI memory\n");
	ret = devm_add_action_or_reset(dev, rtd1296_pcie_release_memory, pcie);
	if (ret)
		return ret;
	pci_add_resource_offset(&bridge->windows, &pcie->logical,
				pcie->logical.start - pcie->pci_base);
	pci_add_resource(&bridge->windows, &pcie->buses);
	bridge->sysdata = pcie;
	bridge->ops = &rtd1296_pcie_ops;
	bridge->busnr = pcie->buses.start;
	bridge->map_irq = of_irq_parse_and_map_pci;
	bridge->swizzle_irq = pci_common_swizzle;
	ret = rtd1296_pcie_msi_init(pdev, pcie);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot initialize MSI receiver\n");
	ret = pci_host_probe(bridge);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, rtd1296_pcie_release_bus, pcie);
}

static void rtd1296_pcie_shutdown(struct platform_device *pdev)
{
	rtd1296_pcie_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id rtd1296_pcie_match[] = {
	{ .compatible = "realtek,rtd1296-pcie-gen1", .data = &rtd1296_gen1 },
	{ .compatible = "realtek,rtd1296-pcie-gen2", .data = &rtd1296_gen2 },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_pcie_match);

static struct platform_driver rtd1296_pcie_driver = {
	.probe = rtd1296_pcie_probe,
	.shutdown = rtd1296_pcie_shutdown,
	.driver = {
		.name = "rtd1296-pcie",
		.of_match_table = rtd1296_pcie_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rtd1296_pcie_driver);

MODULE_DESCRIPTION("Realtek RTD1296 PCIe host controller");
MODULE_LICENSE("GPL");
