// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/rtnetlink.h>
#include <linux/unaligned.h>
#include <net/page_pool/helpers.h>

#define GMAC_ADDR		0x00
#define GMAC_HASH		0x08
#define GMAC_TX_RING		0x20
#define GMAC_CMD			0x37
#define GMAC_POLL		0x38
#define GMAC_IMR			0x3c
#define GMAC_ISR			0x3e
#define GMAC_TCR			0x40
#define GMAC_RCR			0x44
#define GMAC_CFG			0x50
#define GMAC_MULTI_IRQ		0x5c
#define GMAC_MDIO		0x60
#define GMAC_OCP			0xb0
#define GMAC_MCU			0xd3
#define GMAC_RX_MAX		0xda
#define GMAC_CPLUS		0xe0
#define GMAC_RX_RING		0xe4
#define GMAC_TX_EARLY		0xec
#define GMAC_MISC		0xf0

#define CMD_RESET		BIT(4)
#define CMD_RX			BIT(3)
#define CMD_TX			BIT(2)
#define TCR_EMPTY		BIT(11)
#define MCU_EMPTY		(BIT(4) | BIT(5))
#define MISC_RX_GATE		BIT(19)
#define MDIO_BUSY		BIT(31)
#define IRQ_RX			(BIT(0) | BIT(1))
#define IRQ_TX			(BIT(2) | BIT(3))
#define IRQ_RX_OVERFLOW		(BIT(4) | BIT(6))
#define IRQ_MASK		(IRQ_RX | IRQ_TX | IRQ_RX_OVERFLOW)
#define RX_PROMISC		BIT(0)
#define RX_UNICAST		BIT(1)
#define RX_MULTICAST		BIT(2)
#define RX_BROADCAST		BIT(3)
#define RX_ACCEPT_MASK		GENMASK(5, 0)
#define RX_KEEP_MASK		0xff7e1880
#define CPLUS_RX_CSUM		BIT(5)
#define CPLUS_RX_VLAN		BIT(6)

#define DESC_OWN		BIT(31)
#define DESC_END		BIT(30)
#define DESC_FIRST		BIT(29)
#define DESC_LAST		BIT(28)
#define DESC_RX_ERROR		BIT(21)
#define DESC_SIZE		GENMASK(13, 0)
#define RING_COUNT		128
#define RX_SIZE			2048
#define RX_OFFSET		256
#define RING_BYTES		(RING_COUNT * sizeof(struct rtd1296_desc))

struct rtd1296_desc {
	__le32 flags;
	__le32 vlan;
	__le64 addr;
};

struct rtd1296_tx_buffer {
	struct sk_buff *skb;
	dma_addr_t dma;
};

struct rtd1296_gmac {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *base;
	struct regmap *iso;
	struct clk_bulk_data clocks[2];
	struct reset_control *mac_reset;
	struct reset_control *phy_reset;
	struct mii_bus *mdio;
	struct napi_struct napi;
	struct work_struct reset_work;
	/* Serialize interrupt masking, packet engines and the running state. */
	spinlock_t irq_lock;
	/* Serialize transmit ring producers and completion. */
	spinlock_t tx_lock;
	struct page_pool *pool;
	struct rtd1296_desc *rx_ring;
	struct rtd1296_desc *tx_ring;
	dma_addr_t rx_dma;
	dma_addr_t tx_dma;
	struct page *rx_pages[RING_COUNT];
	struct rtd1296_tx_buffer tx_buffers[RING_COUNT];
	u32 rx_head;
	u32 tx_head;
	u32 tx_tail;
	int irq;
	bool running;
	bool powered;
	bool suspended;
};

static void rtd1296_power_off(struct rtd1296_gmac *gmac)
{
	if (!gmac->powered)
		return;
	WRITE_ONCE(gmac->powered, false);
	reset_control_assert(gmac->mac_reset);
	reset_control_assert(gmac->phy_reset);
	clk_bulk_disable_unprepare(ARRAY_SIZE(gmac->clocks), gmac->clocks);
}

static int rtd1296_power_on(struct rtd1296_gmac *gmac)
{
	int ret;

	ret = reset_control_assert(gmac->mac_reset);
	if (ret)
		return ret;
	ret = reset_control_assert(gmac->phy_reset);
	if (ret)
		return ret;
	ret = reset_control_deassert(gmac->phy_reset);
	if (ret)
		return ret;
	usleep_range(200, 300);
	ret = regmap_update_bits(gmac->iso, 0x60, BIT(1), 0);
	if (ret)
		goto assert_phy;
	ret = regmap_update_bits(gmac->iso, 0xfc0, BIT(0), 0);
	if (ret)
		goto assert_phy;
	usleep_range(200, 300);
	/* Clock the PHY before releasing the MAC reset. */
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(gmac->clocks), gmac->clocks);
	if (ret)
		goto assert_phy;
	clk_bulk_disable_unprepare(ARRAY_SIZE(gmac->clocks), gmac->clocks);
	ret = reset_control_deassert(gmac->mac_reset);
	if (ret)
		goto assert_phy;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(gmac->clocks), gmac->clocks);
	if (ret)
		goto assert_mac;
	msleep(100);
	WRITE_ONCE(gmac->powered, true);
	return 0;

assert_mac:
	reset_control_assert(gmac->mac_reset);
assert_phy:
	reset_control_assert(gmac->phy_reset);
	return ret;
}

static int rtd1296_reset(struct rtd1296_gmac *gmac)
{
	u32 addr = (0xea34 / 2) << 16;
	u32 value;
	u8 command;
	int ret;

	writew(0, gmac->base + GMAC_IMR);
	writeb(CMD_RESET, gmac->base + GMAC_CMD);
	ret = readb_poll_timeout(gmac->base + GMAC_CMD, command,
				 !(command & CMD_RESET), 10, 100000);
	if (ret)
		return ret;
	writel(addr, gmac->base + GMAC_OCP);
	value = readl(gmac->base + GMAC_OCP) & 0xffff;
	value = (value & ~GENMASK(1, 0)) | BIT(1);
	writel(BIT(31) | addr | value, gmac->base + GMAC_OCP);
	return 0;
}

static int rtd1296_quiesce(struct rtd1296_gmac *gmac)
{
	u32 value;
	u8 status;
	int ret;

	value = readl(gmac->base + GMAC_RCR);
	writel(value & ~RX_ACCEPT_MASK, gmac->base + GMAC_RCR);
	value = readl(gmac->base + GMAC_MISC);
	writel(value | MISC_RX_GATE, gmac->base + GMAC_MISC);
	usleep_range(2000, 3000);
	ret = readl_poll_timeout(gmac->base + GMAC_TCR, value,
				 value & TCR_EMPTY, 10, 10000);
	if (ret)
		return ret;
	ret = readb_poll_timeout(gmac->base + GMAC_MCU, status,
				 (status & MCU_EMPTY) == MCU_EMPTY, 10, 10000);
	return ret ? ret : rtd1296_reset(gmac);
}

static int rtd1296_mdio_read(struct mii_bus *bus, int phy, int reg)
{
	struct rtd1296_gmac *gmac = bus->priv;
	u32 value;
	int ret;

	if (!READ_ONCE(gmac->powered))
		return -ENODEV;
	if (phy || reg > 31)
		return -EOPNOTSUPP;
	writel(reg << 16, gmac->base + GMAC_MDIO);
	ret = readl_poll_timeout(gmac->base + GMAC_MDIO, value,
				 value & MDIO_BUSY, 10, 50000);
	return ret ? ret : value & 0xffff;
}

static int rtd1296_mdio_write(struct mii_bus *bus, int phy, int reg, u16 data)
{
	struct rtd1296_gmac *gmac = bus->priv;
	u32 value;

	if (!READ_ONCE(gmac->powered))
		return -ENODEV;
	if (phy || reg > 31)
		return -EOPNOTSUPP;
	writel(MDIO_BUSY | (reg << 16) | data, gmac->base + GMAC_MDIO);
	return readl_poll_timeout(gmac->base + GMAC_MDIO, value,
				 !(value & MDIO_BUSY), 10, 50000);
}

static void rtd1296_filter(struct rtd1296_gmac *gmac)
{
	struct net_device *ndev = gmac->ndev;
	struct netdev_hw_addr *ha;
	u32 accept = RX_UNICAST | RX_BROADCAST;
	u64 hash = 0;
	u32 config;

	if (ndev->flags & IFF_PROMISC) {
		accept |= RX_PROMISC | RX_MULTICAST;
		hash = U64_MAX;
	} else if (ndev->flags & IFF_ALLMULTI) {
		accept |= RX_MULTICAST;
		hash = U64_MAX;
	} else {
		netdev_for_each_mc_addr(ha, ndev)
			hash |= BIT_ULL(ether_crc(ETH_ALEN, ha->addr) >> 26);
		if (hash)
			accept |= RX_MULTICAST;
	}
	hash = swab64(hash);
	writel(lower_32_bits(hash), gmac->base + GMAC_HASH);
	writel(upper_32_bits(hash), gmac->base + GMAC_HASH + 4);
	config = readl(gmac->base + GMAC_RCR) & ~RX_ACCEPT_MASK;
	writel(config | accept, gmac->base + GMAC_RCR);
}

static void rtd1296_set_rx_mode(struct net_device *ndev)
{
	struct rtd1296_gmac *gmac = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&gmac->irq_lock, flags);
	if (gmac->running)
		rtd1296_filter(gmac);
	spin_unlock_irqrestore(&gmac->irq_lock, flags);
}

static void rtd1296_rx_publish(struct rtd1296_gmac *gmac, u32 index)
{
	struct rtd1296_desc *desc = &gmac->rx_ring[index];
	dma_addr_t addr = page_pool_get_dma_addr(gmac->rx_pages[index]);
	u32 flags = DESC_OWN | RX_SIZE;

	if (index == RING_COUNT - 1)
		flags |= DESC_END;
	desc->addr = cpu_to_le64(addr + RX_OFFSET);
	desc->vlan = 0;
	dma_wmb();
	WRITE_ONCE(desc->flags, cpu_to_le32(flags));
}

static void rtd1296_free_rings(struct rtd1296_gmac *gmac)
{
	int i;

	for (i = 0; i < RING_COUNT; i++) {
		struct rtd1296_tx_buffer *buffer = &gmac->tx_buffers[i];

		if (buffer->skb) {
			dma_unmap_single(gmac->dev, buffer->dma, buffer->skb->len,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(buffer->skb);
			buffer->skb = NULL;
			gmac->ndev->stats.tx_dropped++;
		}
		if (gmac->rx_pages[i]) {
			page_pool_put_full_page(gmac->pool, gmac->rx_pages[i], false);
			gmac->rx_pages[i] = NULL;
		}
	}
	if (gmac->pool)
		page_pool_destroy(gmac->pool);
	gmac->pool = NULL;
	if (gmac->rx_ring)
		dma_free_coherent(gmac->dev, RING_BYTES, gmac->rx_ring, gmac->rx_dma);
	if (gmac->tx_ring)
		dma_free_coherent(gmac->dev, RING_BYTES, gmac->tx_ring, gmac->tx_dma);
	gmac->rx_ring = NULL;
	gmac->tx_ring = NULL;
}

static int rtd1296_alloc_rings(struct rtd1296_gmac *gmac)
{
	struct page_pool_params params = {
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = RING_COUNT,
		.nid = dev_to_node(gmac->dev),
		.dev = gmac->dev,
		.napi = &gmac->napi,
		.dma_dir = DMA_FROM_DEVICE,
		.max_len = RX_SIZE,
		.offset = RX_OFFSET,
		.netdev = gmac->ndev,
	};
	int i;

	gmac->pool = page_pool_create(&params);
	if (IS_ERR(gmac->pool)) {
		int ret = PTR_ERR(gmac->pool);

		gmac->pool = NULL;
		return ret;
	}
	gmac->rx_ring = dma_alloc_coherent(gmac->dev, RING_BYTES, &gmac->rx_dma,
					   GFP_KERNEL);
	gmac->tx_ring = dma_alloc_coherent(gmac->dev, RING_BYTES, &gmac->tx_dma,
					   GFP_KERNEL);
	if (!gmac->rx_ring || !gmac->tx_ring)
		goto free_rings;
	for (i = 0; i < RING_COUNT; i++) {
		gmac->rx_pages[i] = page_pool_alloc_pages(gmac->pool, GFP_KERNEL);
		if (!gmac->rx_pages[i])
			goto free_rings;
		rtd1296_rx_publish(gmac, i);
	}
	gmac->rx_head = 0;
	gmac->tx_head = 0;
	gmac->tx_tail = 0;
	return 0;

free_rings:
	rtd1296_free_rings(gmac);
	return -ENOMEM;
}

static void rtd1296_tx_clean(struct rtd1296_gmac *gmac, int budget)
{
	struct net_device *ndev = gmac->ndev;
	u32 packets = 0, bytes = 0;

	spin_lock_bh(&gmac->tx_lock);
	while (gmac->tx_tail != gmac->tx_head) {
		u32 index = gmac->tx_tail % RING_COUNT;
		struct rtd1296_tx_buffer *buffer = &gmac->tx_buffers[index];
		u32 flags = le32_to_cpu(READ_ONCE(gmac->tx_ring[index].flags));

		if (flags & DESC_OWN)
			break;
		dma_rmb();
		dma_unmap_single(gmac->dev, buffer->dma, buffer->skb->len,
				 DMA_TO_DEVICE);
		bytes += buffer->skb->len;
		packets++;
		napi_consume_skb(buffer->skb, budget);
		buffer->skb = NULL;
		gmac->tx_tail++;
	}
	if (packets) {
		netdev_completed_queue(ndev, packets, bytes);
		dev_sw_netstats_tx_add(ndev, packets, bytes);
		if (READ_ONCE(gmac->running) && netif_queue_stopped(ndev))
			netif_wake_queue(ndev);
	}
	spin_unlock_bh(&gmac->tx_lock);
}

static int rtd1296_rx(struct rtd1296_gmac *gmac, int budget)
{
	struct net_device *ndev = gmac->ndev;
	int work = 0;

	while (work < budget) {
		u32 index = gmac->rx_head % RING_COUNT;
		struct page *page = gmac->rx_pages[index];
		u32 flags = le32_to_cpu(READ_ONCE(gmac->rx_ring[index].flags));
		struct page *replacement;
		struct sk_buff *skb;
		u32 length;

		if (flags & DESC_OWN)
			break;
		dma_rmb();
		length = flags & DESC_SIZE;
		if ((flags & DESC_RX_ERROR) || length < ETH_HLEN + ETH_FCS_LEN ||
		    length > VLAN_ETH_FRAME_LEN + ETH_FCS_LEN ||
		    (flags & (DESC_FIRST | DESC_LAST)) != (DESC_FIRST | DESC_LAST)) {
			ndev->stats.rx_errors++;
			goto recycle;
		}
		replacement = page_pool_dev_alloc_pages(gmac->pool);
		if (!replacement) {
			ndev->stats.rx_dropped++;
			goto recycle;
		}
		gmac->rx_pages[index] = replacement;
		page_pool_dma_sync_for_cpu(gmac->pool, page, RX_OFFSET, length);
		skb = napi_build_skb(page_address(page), PAGE_SIZE);
		if (!skb) {
			page_pool_put_full_page(gmac->pool, page, true);
			ndev->stats.rx_dropped++;
			goto recycle;
		}
		skb_reserve(skb, RX_OFFSET);
		skb_put(skb, length - ETH_FCS_LEN);
		skb_mark_for_recycle(skb);
		skb->protocol = eth_type_trans(skb, ndev);
		if (skb->pkt_type == PACKET_MULTICAST)
			ndev->stats.multicast++;
		dev_sw_netstats_rx_add(ndev, length - ETH_FCS_LEN);
		napi_gro_receive(&gmac->napi, skb);
recycle:
		rtd1296_rx_publish(gmac, index);
		gmac->rx_head++;
		work++;
	}
	return work;
}

static int rtd1296_poll(struct napi_struct *napi, int budget)
{
	struct rtd1296_gmac *gmac = container_of(napi, struct rtd1296_gmac, napi);
	unsigned long flags;
	int work;

	rtd1296_tx_clean(gmac, budget);
	if (!budget)
		return 0;
	work = rtd1296_rx(gmac, budget);
	if (work < budget && napi_complete_done(napi, work)) {
		spin_lock_irqsave(&gmac->irq_lock, flags);
		if (gmac->running)
			writew(IRQ_MASK, gmac->base + GMAC_IMR);
		spin_unlock_irqrestore(&gmac->irq_lock, flags);
	}
	return work;
}

static irqreturn_t rtd1296_irq(int irq, void *data)
{
	struct rtd1296_gmac *gmac = netdev_priv(data);
	u16 status = readw(gmac->base + GMAC_ISR);

	if (!(status & IRQ_MASK))
		return IRQ_NONE;
	spin_lock(&gmac->irq_lock);
	writew(0, gmac->base + GMAC_IMR);
	writew(status, gmac->base + GMAC_ISR);
	if (status & IRQ_RX_OVERFLOW)
		gmac->ndev->stats.rx_over_errors++;
	if (gmac->running)
		napi_schedule_irqoff(&gmac->napi);
	spin_unlock(&gmac->irq_lock);
	return IRQ_HANDLED;
}

static netdev_tx_t rtd1296_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rtd1296_gmac *gmac = netdev_priv(ndev);
	struct rtd1296_desc *desc;
	dma_addr_t dma;
	u32 index, flags;

	if (skb_put_padto(skb, ETH_ZLEN))
		goto dropped;
	dma = dma_map_single(gmac->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(gmac->dev, dma)) {
		dev_kfree_skb_any(skb);
		goto dropped;
	}
	spin_lock_bh(&gmac->tx_lock);
	index = gmac->tx_head % RING_COUNT;
	desc = &gmac->tx_ring[index];
	gmac->tx_buffers[index].skb = skb;
	gmac->tx_buffers[index].dma = dma;
	desc->addr = cpu_to_le64(dma);
	desc->vlan = 0;
	flags = DESC_OWN | DESC_FIRST | DESC_LAST | skb->len;
	if (index == RING_COUNT - 1)
		flags |= DESC_END;
	netdev_sent_queue(ndev, skb->len);
	skb_tx_timestamp(skb);
	dma_wmb();
	WRITE_ONCE(desc->flags, cpu_to_le32(flags));
	gmac->tx_head++;
	if (gmac->tx_head - gmac->tx_tail == RING_COUNT)
		netif_stop_queue(ndev);
	writeb(BIT(6), gmac->base + GMAC_POLL);
	spin_unlock_bh(&gmac->tx_lock);
	return NETDEV_TX_OK;

dropped:
	ndev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static int rtd1296_open(struct net_device *ndev)
{
	struct rtd1296_gmac *gmac = netdev_priv(ndev);
	unsigned long flags;
	bool init_phy = !gmac->powered;
	u32 config;
	int ret;

	if (!gmac->powered) {
		ret = rtd1296_power_on(gmac);
		if (ret)
			return ret;
	}
	ret = rtd1296_reset(gmac);
	if (ret)
		return ret;
	if (init_phy) {
		ret = phy_init_hw(ndev->phydev);
		if (ret)
			return ret;
	}
	ret = phy_resume(ndev->phydev);
	if (ret)
		return ret;
	ret = rtd1296_alloc_rings(gmac);
	if (ret)
		return ret;
	napi_enable(&gmac->napi);
	ret = request_irq(gmac->irq, rtd1296_irq, 0, ndev->name, ndev);
	if (ret) {
		napi_disable(&gmac->napi);
		rtd1296_free_rings(gmac);
		return ret;
	}
	netdev_reset_queue(ndev);
	writeb(0xc0, gmac->base + GMAC_CFG);
	writel(get_unaligned_le32(ndev->dev_addr), gmac->base + GMAC_ADDR);
	writew(get_unaligned_le16(ndev->dev_addr + 4), gmac->base + GMAC_ADDR + 4);
	writeb(0x3f, gmac->base + GMAC_TX_EARLY);
	writew(RX_SIZE, gmac->base + GMAC_RX_MAX);
	writew(readw(gmac->base + GMAC_CPLUS) & ~(CPLUS_RX_CSUM | CPLUS_RX_VLAN),
	       gmac->base + GMAC_CPLUS);
	config = readl(gmac->base + GMAC_RCR) & RX_KEEP_MASK;
	writel(config | (7 << 13) | (6 << 8), gmac->base + GMAC_RCR);
	writel((3 << 24) | (3 << 8), gmac->base + GMAC_TCR);
	writel(lower_32_bits(gmac->tx_dma), gmac->base + GMAC_TX_RING);
	writel(upper_32_bits(gmac->tx_dma), gmac->base + GMAC_TX_RING + 4);
	writel(lower_32_bits(gmac->rx_dma), gmac->base + GMAC_RX_RING);
	writel(upper_32_bits(gmac->rx_dma), gmac->base + GMAC_RX_RING + 4);
	writew(readw(gmac->base + GMAC_MULTI_IRQ) & ~0xfff,
	       gmac->base + GMAC_MULTI_IRQ);
	writew(0xffff, gmac->base + GMAC_ISR);
	spin_lock_irqsave(&gmac->irq_lock, flags);
	rtd1296_filter(gmac);
	writel(readl(gmac->base + GMAC_MISC) & ~MISC_RX_GATE,
	       gmac->base + GMAC_MISC);
	dma_wmb();
	writeb(0, gmac->base + GMAC_CFG);
	gmac->running = true;
	writew(IRQ_MASK, gmac->base + GMAC_IMR);
	spin_unlock_irqrestore(&gmac->irq_lock, flags);
	phy_start(ndev->phydev);
	netif_start_queue(ndev);
	if (!gmac->suspended)
		netif_device_attach(ndev);
	return 0;
}

static int rtd1296_stop(struct net_device *ndev)
{
	struct rtd1296_gmac *gmac = netdev_priv(ndev);
	unsigned long flags;
	int ret;

	if (!gmac->running)
		return 0;
	netif_tx_disable(ndev);
	spin_lock_irqsave(&gmac->irq_lock, flags);
	gmac->running = false;
	writew(0, gmac->base + GMAC_IMR);
	readw(gmac->base + GMAC_IMR);
	spin_unlock_irqrestore(&gmac->irq_lock, flags);
	free_irq(gmac->irq, ndev);
	napi_disable(&gmac->napi);
	ret = rtd1296_quiesce(gmac);
	phy_stop(ndev->phydev);
	if (ret) {
		netdev_err(ndev, "DMA stop timed out; disabling the MAC clocks\n");
		rtd1296_power_off(gmac);
	}
	rtd1296_tx_clean(gmac, 0);
	rtd1296_free_rings(gmac);
	return 0;
}

static void rtd1296_reset_work(struct work_struct *work)
{
	struct rtd1296_gmac *gmac = container_of(work, struct rtd1296_gmac, reset_work);
	struct net_device *ndev = gmac->ndev;

	rtnl_lock();
	if (netif_running(ndev) && !gmac->suspended) {
		netif_device_detach(ndev);
		rtd1296_stop(ndev);
		if (rtd1296_open(ndev)) {
			netif_device_detach(ndev);
			netdev_err(ndev, "Transmit recovery failed\n");
		} else {
			netif_device_attach(ndev);
		}
	}
	rtnl_unlock();
}

static void rtd1296_tx_timeout(struct net_device *ndev, unsigned int queue)
{
	struct rtd1296_gmac *gmac = netdev_priv(ndev);

	ndev->stats.tx_errors++;
	schedule_work(&gmac->reset_work);
}

static void rtd1296_link_change(struct net_device *ndev)
{
	struct rtd1296_gmac *gmac = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&gmac->irq_lock, flags);
	if (gmac->running && ndev->phydev->link) {
		writeb(CMD_RX | CMD_TX, gmac->base + GMAC_CMD);
		writeb(BIT(6), gmac->base + GMAC_POLL);
	}
	spin_unlock_irqrestore(&gmac->irq_lock, flags);
	phy_print_status(ndev->phydev);
}

static void rtd1296_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *info)
{
	struct rtd1296_gmac *gmac = netdev_priv(ndev);

	strscpy(info->driver, "rtd1296-gmac", sizeof(info->driver));
	strscpy(info->bus_info, dev_name(gmac->dev), sizeof(info->bus_info));
}

static const struct ethtool_ops rtd1296_ethtool_ops = {
	.get_drvinfo = rtd1296_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
	.nway_reset = phy_ethtool_nway_reset,
	.get_ts_info = ethtool_op_get_ts_info,
};

static const struct net_device_ops rtd1296_netdev_ops = {
	.ndo_open = rtd1296_open,
	.ndo_stop = rtd1296_stop,
	.ndo_start_xmit = rtd1296_xmit,
	.ndo_set_rx_mode = rtd1296_set_rx_mode,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_get_stats64 = dev_get_tstats64,
	.ndo_eth_ioctl = phy_do_ioctl_running,
	.ndo_tx_timeout = rtd1296_tx_timeout,
};

static int rtd1296_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *mdio_node;
	struct rtd1296_gmac *gmac;
	struct net_device *ndev;
	phy_interface_t mode;
	int ret;

	ret = of_get_phy_mode(dev->of_node, &mode);
	if (ret || mode != PHY_INTERFACE_MODE_INTERNAL)
		return dev_err_probe(dev, -EINVAL, "Internal PHY connection required\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ndev = devm_alloc_etherdev(dev, sizeof(*gmac));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	gmac = netdev_priv(ndev);
	gmac->ndev = ndev;
	gmac->dev = dev;
	gmac->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gmac->base))
		return PTR_ERR(gmac->base);
	gmac->irq = platform_get_irq(pdev, 0);
	if (gmac->irq < 0)
		return gmac->irq;
	gmac->iso = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,iso");
	if (IS_ERR(gmac->iso))
		return dev_err_probe(dev, PTR_ERR(gmac->iso), "Cannot access ISO control\n");
	gmac->clocks[0].id = "mac";
	gmac->clocks[1].id = "bus";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(gmac->clocks), gmac->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get clocks\n");
	gmac->mac_reset = devm_reset_control_get_exclusive(dev, "gmac");
	if (IS_ERR(gmac->mac_reset))
		return dev_err_probe(dev, PTR_ERR(gmac->mac_reset), "Cannot get MAC reset\n");
	gmac->phy_reset = devm_reset_control_get_exclusive(dev, "gphy");
	if (IS_ERR(gmac->phy_reset))
		return dev_err_probe(dev, PTR_ERR(gmac->phy_reset), "Cannot get PHY reset\n");
	ret = rtd1296_power_on(gmac);
	if (ret)
		return ret;
	ret = rtd1296_reset(gmac);
	if (ret)
		goto power_off;
	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret == -EPROBE_DEFER)
		goto power_off;
	if (ret)
		eth_hw_addr_random(ndev);
	spin_lock_init(&gmac->irq_lock);
	spin_lock_init(&gmac->tx_lock);
	INIT_WORK(&gmac->reset_work, rtd1296_reset_work);
	ndev->netdev_ops = &rtd1296_netdev_ops;
	ndev->ethtool_ops = &rtd1296_ethtool_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	netif_carrier_off(ndev);
	netif_napi_add(ndev, &gmac->napi, rtd1296_poll);
	gmac->mdio = devm_mdiobus_alloc(dev);
	if (!gmac->mdio) {
		ret = -ENOMEM;
		goto del_napi;
	}
	gmac->mdio->name = "RTD1296 internal MDIO";
	snprintf(gmac->mdio->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	gmac->mdio->parent = dev;
	gmac->mdio->priv = gmac;
	gmac->mdio->read = rtd1296_mdio_read;
	gmac->mdio->write = rtd1296_mdio_write;
	gmac->mdio->phy_mask = ~1U;
	mdio_node = of_get_child_by_name(dev->of_node, "mdio");
	if (!mdio_node) {
		ret = -EINVAL;
		goto del_napi;
	}
	ret = of_mdiobus_register(gmac->mdio, mdio_node);
	of_node_put(mdio_node);
	if (ret)
		goto del_napi;
	if (!of_phy_get_and_connect(ndev, dev->of_node, rtd1296_link_change)) {
		ret = -ENODEV;
		goto unregister_mdio;
	}
	ndev->phydev->mac_managed_pm = true;
	phy_set_max_speed(ndev->phydev, SPEED_1000);
	phy_remove_link_mode(ndev->phydev, ETHTOOL_LINK_MODE_Pause_BIT);
	phy_remove_link_mode(ndev->phydev, ETHTOOL_LINK_MODE_Asym_Pause_BIT);
	ret = register_netdev(ndev);
	if (ret)
		goto disconnect_phy;
	platform_set_drvdata(pdev, ndev);
	return 0;

disconnect_phy:
	phy_disconnect(ndev->phydev);
unregister_mdio:
	mdiobus_unregister(gmac->mdio);
del_napi:
	netif_napi_del(&gmac->napi);
power_off:
	rtd1296_power_off(gmac);
	return ret;
}

static void rtd1296_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct rtd1296_gmac *gmac = netdev_priv(ndev);

	unregister_netdev(ndev);
	cancel_work_sync(&gmac->reset_work);
	phy_disconnect(ndev->phydev);
	mdiobus_unregister(gmac->mdio);
	netif_napi_del(&gmac->napi);
	if (gmac->powered)
		rtd1296_power_off(gmac);
}

static int rtd1296_suspend(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct rtd1296_gmac *gmac = netdev_priv(ndev);
	int ret;

	rtnl_lock();
	gmac->suspended = true;
	netif_device_detach(ndev);
	if (netif_running(ndev))
		rtd1296_stop(ndev);
	ret = phy_suspend(ndev->phydev);
	if (ret) {
		if (!netif_running(ndev) || !rtd1296_open(ndev))
			netif_device_attach(ndev);
		gmac->suspended = false;
	} else {
		rtd1296_power_off(gmac);
	}
	rtnl_unlock();
	return ret;
}

static int rtd1296_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct rtd1296_gmac *gmac = netdev_priv(ndev);
	int ret;

	rtnl_lock();
	ret = rtd1296_power_on(gmac);
	if (ret)
		goto out;
	ret = rtd1296_reset(gmac);
	if (ret)
		goto power_off;
	ret = phy_init_hw(ndev->phydev);
	if (ret)
		goto power_off;
	ret = phy_resume(ndev->phydev);
	if (ret)
		goto power_off;
	if (netif_running(ndev)) {
		ret = rtd1296_open(ndev);
		if (ret)
			goto power_off;
	}
	gmac->suspended = false;
	netif_device_attach(ndev);
	goto out;

power_off:
	rtd1296_power_off(gmac);
out:
	rtnl_unlock();
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rtd1296_pm_ops, rtd1296_suspend, rtd1296_resume);

static void rtd1296_shutdown(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct rtd1296_gmac *gmac = netdev_priv(ndev);

	rtnl_lock();
	gmac->suspended = true;
	netif_device_detach(ndev);
	rtd1296_stop(ndev);
	rtd1296_power_off(gmac);
	rtnl_unlock();
	cancel_work_sync(&gmac->reset_work);
}

static const struct of_device_id rtd1296_gmac_of_match[] = {
	{ .compatible = "realtek,rtd1296-gmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1296_gmac_of_match);

static struct platform_driver rtd1296_gmac_driver = {
	.probe = rtd1296_probe,
	.remove = rtd1296_remove,
	.shutdown = rtd1296_shutdown,
	.driver = {
		.name = "rtd1296-gmac",
		.of_match_table = rtd1296_gmac_of_match,
		.pm = pm_sleep_ptr(&rtd1296_pm_ops),
	},
};
module_platform_driver(rtd1296_gmac_driver);

MODULE_DESCRIPTION("Realtek RTD1296 integrated Gigabit Ethernet MAC");
MODULE_LICENSE("GPL");
