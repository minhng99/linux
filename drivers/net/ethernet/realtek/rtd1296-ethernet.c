// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/rtnetlink.h>

#define RTD_RING_SIZE		64
#define RTD_BUFFER_SIZE		2048
#define RTD_PORT			5
#define RTD_RX_WATCHDOG_NS	100000

#define CPU_CONTROL		0x160000
#define CPU_RX_BASE(n)		(0x160004 + 4 * (n))
#define CPU_TX_BASE		0x160020
#define CPU_IRQ_MASK		0x160028
#define CPU_IRQ_STATUS		0x16002c
#define CPU_QUEUE_MAP(n)		(0x160030 + 2 * (n))
#define CPU_DMA_CONTROL		0x16003c
#define CPU_TX_LIMIT		0x160040
#define CPU_TX_RING_ENABLE	0x160078
#define CPU_TX_TAIL_ENABLE	0x1600a0
#define CPU_FORMAT		0x1600a4
#define CPU_TX_ENABLE		BIT(31)
#define CPU_RX_ENABLE		BIT(30)
#define CPU_BURST_128WORDS	(2 << 28)
#define CPU_BUFFER_2K		(4 << 24)
#define CPU_TX_FETCH		BIT(23)
#define CPU_IRQ_RX		BIT(3)
#define CPU_IRQ_TX		BIT(9)
#define CPU_IRQ_RUNOUT		BIT(17)
#define CPU_IRQ_EVENTS		(CPU_IRQ_RX | CPU_IRQ_TX | CPU_IRQ_RUNOUT)
#define CPU_FORMAT_SHORT		BIT(8)
#define CPU_FORMAT_LITTLE	BIT(1)

#define MDIO_COMMAND		0x164004
#define MDIO_STATUS		0x164008
#define MDIO_BUSY		BIT(31)
#define MDIO_ERROR		BIT(30)
#define PORT_INTERFACE		0x164100
#define PORT_CONTROL(n)		(0x164104 + 4 * (n))
#define PORT_PHY_ID		GENMASK(30, 26)
#define PORT_FORCE_MODE		BIT(25)
#define PORT_POLL_LINK		BIT(24)
#define PORT_LINK		BIT(23)
#define PORT_SPEED		GENMASK(20, 19)
#define PORT_DUPLEX		BIT(18)
#define PORT_RX_PAUSE		BIT(17)
#define PORT_TX_PAUSE		BIT(16)
#define PORT_FORWARD		GENMASK(5, 4)
#define PORT_RESET_N		BIT(3)
#define PORT_ENABLE		BIT(0)
#define PORT5_GMII		0x164150
#define PORT_CONFIG_DONE		BIT(6)
#define SWITCH_CONTROL		0x164204
#define SWITCH_FULL_RESET	BIT(2)
#define SWITCH_ENABLE		BIT(0)
#define SWITCH_MODE		0x164410
#define SWITCH_FORWARD		0x164428
#define WRAPPER_GATE		0x169118

#define DESC_OWN			BIT(0)
#define DESC_WRAP		BIT(1)
#define DESC_LAST		BIT(2)
#define DESC_FIRST		BIT(3)
#define DESC_RX_ERROR		GENMASK(5, 4)
#define DESC_RX_CAPACITY		GENMASK(31, 16)
#define DESC_RX_LENGTH		GENMASK(13, 0)
#define DESC_RX_PORT		GENMASK(15, 13)
#define DESC_OWN2		BIT(15)
#define DESC_TX_LENGTH		GENMASK(22, 6)
#define DESC_TX_BUFFER_LENGTH	GENMASK(31, 15)
#define DESC_TX_PORTS		GENMASK(30, 24)

struct rtd_desc {
	__le32 word[6];
};

struct rtd_buffer {
	struct sk_buff *skb;
	dma_addr_t dma;
	unsigned int length;
};

struct rtd_ethernet {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *base;
	struct clk_bulk_data clocks[2];
	struct reset_control *reset;
	struct mii_bus *mdio;
	struct phylink *phylink;
	struct phylink_config link_config;
	struct napi_struct napi;
	struct work_struct reset_work;
	struct hrtimer rx_watchdog;
	u64 rx_watchdog_polls;
	/* Serializes descriptor indices, queue stops and completion accounting. */
	spinlock_t tx_lock;
	/* Serializes interrupt rearming and DMA shutdown. */
	spinlock_t irq_lock;
	struct rtd_desc *rx, *tx;
	dma_addr_t rx_dma, tx_dma;
	struct rtd_buffer rx_buffers[RTD_RING_SIZE];
	struct rtd_buffer tx_buffers[RTD_RING_SIZE];
	u32 rx_next, tx_producer, tx_consumer;
	int irq;
	u32 phy_addr;
	bool suspended;
	bool running;
	bool clocked;
	bool faulted;
};

static u32 rtd_read(struct rtd_ethernet *eth, u32 reg)
{
	return readl(eth->base + reg);
}

static void rtd_write(struct rtd_ethernet *eth, u32 reg, u32 value)
{
	writel(value, eth->base + reg);
}

static int rtd_control_init(struct rtd_ethernet *eth)
{
	u32 value;
	int i, ret;

	value = rtd_read(eth, WRAPPER_GATE);
	rtd_write(eth, WRAPPER_GATE, value & ~BIT(0));
	rtd_read(eth, WRAPPER_GATE);
	rtd_write(eth, CPU_IRQ_MASK, 0);
	rtd_write(eth, CPU_CONTROL, 0);
	rtd_write(eth, SWITCH_CONTROL, SWITCH_FULL_RESET);
	ret = readl_poll_timeout(eth->base + SWITCH_CONTROL, value,
				 !(value & SWITCH_FULL_RESET), 10, 100000);
	if (ret)
		return ret;
	rtd_write(eth, CPU_IRQ_STATUS, ~0U);
	for (i = 0; i < 6; i++) {
		value = rtd_read(eth, PORT_CONTROL(i));
		value |= PORT_FORCE_MODE;
		value &= ~(PORT_POLL_LINK | PORT_LINK | PORT_ENABLE);
		rtd_write(eth, PORT_CONTROL(i), value);
	}
	value = rtd_read(eth, PORT_CONTROL(RTD_PORT));
	value &= ~PORT_PHY_ID;
	value |= FIELD_PREP(PORT_PHY_ID, eth->phy_addr);
	rtd_write(eth, PORT_CONTROL(RTD_PORT), value);
	value = rtd_read(eth, PORT_INTERFACE);
	rtd_write(eth, PORT_INTERFACE, value & ~GENMASK(11, 10));
	value = rtd_read(eth, PORT5_GMII);
	rtd_write(eth, PORT5_GMII, value & ~GENMASK(24, 23));
	return 0;
}

static int rtd_runtime_resume(struct device *dev)
{
	struct rtd_ethernet *eth = netdev_priv(dev_get_drvdata(dev));
	int ret;

	ret = reset_control_assert(eth->reset);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(eth->clocks), eth->clocks);
	if (ret)
		return ret;
	ret = reset_control_deassert(eth->reset);
	if (ret) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(eth->clocks), eth->clocks);
		return ret;
	}
	usleep_range(1000, 1500);
	ret = rtd_control_init(eth);
	if (ret) {
		reset_control_assert(eth->reset);
		clk_bulk_disable_unprepare(ARRAY_SIZE(eth->clocks), eth->clocks);
		return ret;
	}
	WRITE_ONCE(eth->clocked, true);
	eth->faulted = false;
	return 0;
}

static int rtd_runtime_suspend(struct device *dev)
{
	struct rtd_ethernet *eth = netdev_priv(dev_get_drvdata(dev));
	int ret;

	rtd_write(eth, CPU_IRQ_MASK, 0);
	rtd_write(eth, CPU_CONTROL, 0);
	rtd_read(eth, CPU_CONTROL);
	WRITE_ONCE(eth->clocked, false);
	synchronize_irq(eth->irq);
	ret = reset_control_assert(eth->reset);
	if (ret) {
		WRITE_ONCE(eth->clocked, true);
		return ret;
	}
	clk_bulk_disable_unprepare(ARRAY_SIZE(eth->clocks), eth->clocks);
	return 0;
}

static int rtd_mdio_wait(struct rtd_ethernet *eth, u32 *status)
{
	return readl_poll_timeout(eth->base + MDIO_STATUS, *status,
				 !(*status & MDIO_BUSY), 1, 10000);
}

static int rtd_mdio_read(struct mii_bus *bus, int phy, int reg)
{
	struct rtd_ethernet *eth = bus->priv;
	u32 status;
	int ret;

	ret = rtd_mdio_wait(eth, &status);
	if (ret)
		return ret;
	rtd_write(eth, MDIO_COMMAND, phy << 24 | reg << 16);
	udelay(1);
	ret = rtd_mdio_wait(eth, &status);
	if (ret)
		return ret;
	return status & MDIO_ERROR ? -EIO : status & 0xffff;
}

static int rtd_mdio_write(struct mii_bus *bus, int phy, int reg, u16 data)
{
	struct rtd_ethernet *eth = bus->priv;
	u32 status;
	int ret;

	ret = rtd_mdio_wait(eth, &status);
	if (ret)
		return ret;
	rtd_write(eth, MDIO_COMMAND, BIT(31) | phy << 24 | reg << 16 | data);
	udelay(1);
	return rtd_mdio_wait(eth, &status);
}

static struct rtd_ethernet *rtd_from_link(struct phylink_config *config)
{
	return container_of(config, struct rtd_ethernet, link_config);
}

static void rtd_mac_config(struct phylink_config *config, unsigned int mode,
			   const struct phylink_link_state *state)
{
	struct rtd_ethernet *eth = rtd_from_link(config);
	u32 value = rtd_read(eth, PORT5_GMII);

	value &= ~GENMASK(24, 23);
	rtd_write(eth, PORT5_GMII, value | PORT_CONFIG_DONE);
}

static void rtd_mac_link_down(struct phylink_config *config, unsigned int mode,
			      phy_interface_t interface)
{
	struct rtd_ethernet *eth = rtd_from_link(config);
	u32 value = rtd_read(eth, PORT_CONTROL(RTD_PORT));

	rtd_write(eth, PORT_CONTROL(RTD_PORT), value & ~PORT_LINK);
}

static void rtd_mac_link_up(struct phylink_config *config, struct phy_device *phy,
			    unsigned int mode, phy_interface_t interface,
			    int speed, int duplex, bool tx_pause, bool rx_pause)
{
	struct rtd_ethernet *eth = rtd_from_link(config);
	u32 value = rtd_read(eth, PORT_CONTROL(RTD_PORT));
	u32 encoding = speed == SPEED_1000 ? 2 : speed == SPEED_100 ? 1 : 0;

	value &= ~(PORT_SPEED | PORT_DUPLEX | PORT_RX_PAUSE | PORT_TX_PAUSE);
	value |= FIELD_PREP(PORT_SPEED, encoding) | PORT_LINK | PORT_ENABLE;
	value |= PORT_FORCE_MODE | PORT_FORWARD | PORT_RESET_N;
	if (duplex == DUPLEX_FULL)
		value |= PORT_DUPLEX;
	if (tx_pause)
		value |= PORT_TX_PAUSE;
	if (rx_pause)
		value |= PORT_RX_PAUSE;
	rtd_write(eth, PORT_CONTROL(RTD_PORT), value);
}

static const struct phylink_mac_ops rtd_mac_ops = {
	.mac_config = rtd_mac_config,
	.mac_link_down = rtd_mac_link_down,
	.mac_link_up = rtd_mac_link_up,
};

static int rtd_alloc_rx(struct rtd_ethernet *eth, struct rtd_buffer *buffer)
{
	struct sk_buff *skb;
	dma_addr_t dma;

	skb = netdev_alloc_skb_ip_align(eth->ndev, RTD_BUFFER_SIZE);
	if (!skb)
		return -ENOMEM;
	dma = dma_map_single(eth->dev, skb->data, RTD_BUFFER_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(eth->dev, dma)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}
	buffer->skb = skb;
	buffer->dma = dma;
	buffer->length = RTD_BUFFER_SIZE;
	return 0;
}

static void rtd_arm_rx(struct rtd_ethernet *eth, unsigned int index)
{
	struct rtd_desc *desc = &eth->rx[index];
	u32 flags = FIELD_PREP(DESC_RX_CAPACITY, RTD_BUFFER_SIZE);

	if (index == RTD_RING_SIZE - 1)
		flags |= DESC_WRAP;
	desc->word[1] = cpu_to_le32(eth->rx_buffers[index].dma);
	desc->word[2] = 0;
	desc->word[3] = 0;
	desc->word[4] = 0;
	WRITE_ONCE(desc->word[5], cpu_to_le32(DESC_OWN2));
	dma_wmb();
	WRITE_ONCE(desc->word[0], cpu_to_le32(flags | DESC_OWN));
}

static void rtd_free_rings(struct rtd_ethernet *eth)
{
	struct rtd_buffer *buffer;
	int i;

	for (i = 0; i < RTD_RING_SIZE; i++) {
		buffer = &eth->rx_buffers[i];
		if (buffer->skb) {
			dma_unmap_single(eth->dev, buffer->dma, buffer->length,
					 DMA_FROM_DEVICE);
			dev_kfree_skb_any(buffer->skb);
			buffer->skb = NULL;
		}
		buffer = &eth->tx_buffers[i];
		if (buffer->skb) {
			dma_unmap_single(eth->dev, buffer->dma, buffer->length,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(buffer->skb);
			buffer->skb = NULL;
		}
	}
	if (eth->rx)
		dma_free_coherent(eth->dev, sizeof(*eth->rx) * RTD_RING_SIZE,
				  eth->rx, eth->rx_dma);
	if (eth->tx)
		dma_free_coherent(eth->dev, sizeof(*eth->tx) * RTD_RING_SIZE,
				  eth->tx, eth->tx_dma);
	eth->rx = NULL;
	eth->tx = NULL;
	netdev_reset_queue(eth->ndev);
}

static int rtd_alloc_rings(struct rtd_ethernet *eth)
{
	int i;

	eth->rx_next = 0;
	eth->tx_producer = 0;
	eth->tx_consumer = 0;
	eth->rx = dma_alloc_coherent(eth->dev, sizeof(*eth->rx) * RTD_RING_SIZE,
				     &eth->rx_dma, GFP_KERNEL);
	eth->tx = dma_alloc_coherent(eth->dev, sizeof(*eth->tx) * RTD_RING_SIZE,
				     &eth->tx_dma, GFP_KERNEL);
	if (!eth->rx || !eth->tx)
		goto fail;
	for (i = 0; i < RTD_RING_SIZE; i++) {
		if (rtd_alloc_rx(eth, &eth->rx_buffers[i]))
			goto fail;
		rtd_arm_rx(eth, i);
	}
	return 0;
fail:
	rtd_free_rings(eth);
	return -ENOMEM;
}

static void rtd_clean_tx(struct rtd_ethernet *eth, int budget)
{
	unsigned int packets = 0, bytes = 0;
	struct rtd_buffer *buffer;
	unsigned long flags;
	u32 index;

	spin_lock_irqsave(&eth->tx_lock, flags);
	while (eth->tx_consumer != eth->tx_producer) {
		index = eth->tx_consumer & (RTD_RING_SIZE - 1);
		if (le32_to_cpu(READ_ONCE(eth->tx[index].word[0])) & DESC_OWN)
			break;
		dma_rmb();
		buffer = &eth->tx_buffers[index];
		dma_unmap_single(eth->dev, buffer->dma, buffer->length, DMA_TO_DEVICE);
		bytes += buffer->skb->len;
		packets++;
		napi_consume_skb(buffer->skb, budget);
		buffer->skb = NULL;
		eth->tx_consumer++;
	}
	eth->ndev->stats.tx_packets += packets;
	eth->ndev->stats.tx_bytes += bytes;
	netdev_completed_queue(eth->ndev, packets, bytes);
	if (READ_ONCE(eth->running) && netif_queue_stopped(eth->ndev) &&
	    eth->tx_producer - eth->tx_consumer < RTD_RING_SIZE - 1)
		netif_wake_queue(eth->ndev);
	spin_unlock_irqrestore(&eth->tx_lock, flags);
}

static bool rtd_accept(struct net_device *ndev, const u8 *destination)
{
	struct netdev_hw_addr *address;
	bool accept = false;

	if (ndev->flags & IFF_PROMISC || is_broadcast_ether_addr(destination) ||
	    ether_addr_equal(destination, ndev->dev_addr))
		return true;
	if (is_multicast_ether_addr(destination) && ndev->flags & IFF_ALLMULTI)
		return true;
	netif_addr_lock_bh(ndev);
	if (is_multicast_ether_addr(destination)) {
		netdev_for_each_mc_addr(address, ndev)
			if (ether_addr_equal(destination, address->addr)) {
				accept = true;
				break;
			}
	} else {
		netdev_for_each_uc_addr(address, ndev)
			if (ether_addr_equal(destination, address->addr)) {
				accept = true;
				break;
			}
	}
	netif_addr_unlock_bh(ndev);
	return accept;
}

static bool rtd_schedule_poll(struct rtd_ethernet *eth, u32 status)
{
	lockdep_assert_held(&eth->irq_lock);
	if (!READ_ONCE(eth->running))
		return false;
	rtd_write(eth, CPU_IRQ_MASK, 0);
	rtd_write(eth, CPU_IRQ_STATUS, status);
	if (!napi_schedule_prep(&eth->napi))
		return false;
	__napi_schedule(&eth->napi);
	return true;
}

/* RX completion can latch status without asserting the interrupt. */
static enum hrtimer_restart rtd_rx_watchdog(struct hrtimer *timer)
{
	struct rtd_ethernet *eth = container_of(timer, struct rtd_ethernet, rx_watchdog);
	enum hrtimer_restart restart = HRTIMER_NORESTART;
	struct rtd_desc *desc;
	unsigned long flags;
	unsigned int index;

	spin_lock_irqsave(&eth->irq_lock, flags);
	if (!eth->running)
		goto unlock;
	index = READ_ONCE(eth->rx_next) & (RTD_RING_SIZE - 1);
	desc = &eth->rx[index];
	if (!(le32_to_cpu(READ_ONCE(desc->word[0])) & DESC_OWN) &&
	    rtd_schedule_poll(eth, CPU_IRQ_EVENTS))
		eth->rx_watchdog_polls++;
	hrtimer_forward_now(timer, ns_to_ktime(RTD_RX_WATCHDOG_NS));
	restart = HRTIMER_RESTART;
unlock:
	spin_unlock_irqrestore(&eth->irq_lock, flags);
	return restart;
}

static int rtd_poll(struct napi_struct *napi, int budget)
{
	struct rtd_ethernet *eth = container_of(napi, struct rtd_ethernet, napi);
	struct net_device *ndev = eth->ndev;
	struct rtd_buffer replacement, old;
	struct rtd_desc *desc;
	u32 word0, word4, length, status;
	unsigned int index;
	unsigned long flags;
	int work = 0;

	rtd_clean_tx(eth, budget);
	while (work < budget) {
		index = eth->rx_next & (RTD_RING_SIZE - 1);
		desc = &eth->rx[index];
		word0 = le32_to_cpu(READ_ONCE(desc->word[0]));
		if (word0 & DESC_OWN)
			break;
		dma_rmb();
		if (le32_to_cpu(READ_ONCE(desc->word[5])) & DESC_OWN2)
			return budget;
		dma_rmb();
		length = FIELD_GET(DESC_RX_LENGTH, le32_to_cpu(desc->word[2]));
		word4 = le32_to_cpu(desc->word[4]);
		if (word0 & DESC_RX_ERROR || length < ETH_HLEN + ETH_FCS_LEN ||
		    length > RTD_BUFFER_SIZE || FIELD_GET(DESC_RX_PORT, word4) != RTD_PORT ||
		    (word0 & (DESC_FIRST | DESC_LAST)) != (DESC_FIRST | DESC_LAST)) {
			ndev->stats.rx_errors++;
			goto rearm;
		}
		if (rtd_alloc_rx(eth, &replacement)) {
			ndev->stats.rx_dropped++;
			goto rearm;
		}
		old = eth->rx_buffers[index];
		eth->rx_buffers[index] = replacement;
		dma_unmap_single(eth->dev, old.dma, old.length, DMA_FROM_DEVICE);
		length -= ETH_FCS_LEN;
		skb_put(old.skb, length);
		if (rtd_accept(ndev, old.skb->data)) {
			ndev->stats.rx_packets++;
			ndev->stats.rx_bytes += length;
			old.skb->protocol = eth_type_trans(old.skb, ndev);
			napi_gro_receive(napi, old.skb);
		} else {
			dev_kfree_skb_any(old.skb);
		}
rearm:
		rtd_arm_rx(eth, index);
		eth->rx_next++;
		work++;
	}
	if (work < budget && napi_complete_done(napi, work)) {
		spin_lock_irqsave(&eth->irq_lock, flags);
		if (eth->running) {
			rtd_write(eth, CPU_IRQ_MASK, CPU_IRQ_EVENTS);
			status = rtd_read(eth, CPU_IRQ_STATUS) & CPU_IRQ_EVENTS;
			if (status)
				rtd_schedule_poll(eth, status);
		}
		spin_unlock_irqrestore(&eth->irq_lock, flags);
	}
	return work;
}

static irqreturn_t rtd_irq(int irq, void *data)
{
	struct rtd_ethernet *eth = data;
	unsigned long flags;
	u32 status;

	if (!READ_ONCE(eth->clocked))
		return IRQ_NONE;
	spin_lock_irqsave(&eth->irq_lock, flags);
	status = rtd_read(eth, CPU_IRQ_STATUS) & rtd_read(eth, CPU_IRQ_MASK);
	if (status)
		rtd_schedule_poll(eth, status);
	spin_unlock_irqrestore(&eth->irq_lock, flags);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

static netdev_tx_t rtd_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);
	struct rtd_desc *desc;
	struct rtd_buffer *buffer;
	unsigned long flags;
	dma_addr_t dma;
	u32 index, length;

	spin_lock_irqsave(&eth->tx_lock, flags);
	if (eth->tx_producer - eth->tx_consumer >= RTD_RING_SIZE - 1) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&eth->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	spin_unlock_irqrestore(&eth->tx_lock, flags);
	if (skb_linearize(skb) || skb_cow_head(skb, 0))
		goto drop;
	if (skb_put_padto(skb, ETH_ZLEN)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	if (skb_tailroom(skb) < ETH_FCS_LEN &&
	    pskb_expand_head(skb, 0, ETH_FCS_LEN, GFP_ATOMIC))
		goto drop;
	memset(skb_tail_pointer(skb), 0, ETH_FCS_LEN);
	length = skb->len + ETH_FCS_LEN;
	dma = dma_map_single(eth->dev, skb->data, length, DMA_TO_DEVICE);
	if (dma_mapping_error(eth->dev, dma))
		goto drop;
	spin_lock_irqsave(&eth->tx_lock, flags);
	index = eth->tx_producer & (RTD_RING_SIZE - 1);
	desc = &eth->tx[index];
	buffer = &eth->tx_buffers[index];
	buffer->skb = skb;
	buffer->dma = dma;
	buffer->length = length;
	desc->word[1] = cpu_to_le32(dma);
	desc->word[2] = cpu_to_le32(FIELD_PREP(DESC_TX_BUFFER_LENGTH, length));
	desc->word[3] = 0;
	desc->word[4] = cpu_to_le32(FIELD_PREP(DESC_TX_PORTS, BIT(RTD_PORT)));
	desc->word[5] = 0;
	netdev_sent_queue(ndev, skb->len);
	dma_wmb();
	WRITE_ONCE(desc->word[0], cpu_to_le32(DESC_OWN | DESC_FIRST | DESC_LAST |
				 FIELD_PREP(DESC_TX_LENGTH, length)));
	eth->tx_producer++;
	if (eth->tx_producer - eth->tx_consumer >= RTD_RING_SIZE - 1)
		netif_stop_queue(ndev);
	rtd_write(eth, CPU_CONTROL, CPU_TX_ENABLE | CPU_RX_ENABLE |
		  CPU_BURST_128WORDS | CPU_BUFFER_2K | CPU_TX_FETCH);
	spin_unlock_irqrestore(&eth->tx_lock, flags);
	return NETDEV_TX_OK;
drop:
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int rtd_dma_start(struct rtd_ethernet *eth)
{
	u32 value;
	int i, ret;

	if (eth->faulted || eth->rx || eth->tx)
		return -EIO;
	ret = rtd_alloc_rings(eth);
	if (ret)
		return ret;
	value = rtd_read(eth, CPU_FORMAT);
	value &= ~GENMASK(9, 8);
	rtd_write(eth, CPU_FORMAT, value | CPU_FORMAT_SHORT | CPU_FORMAT_LITTLE);
	for (i = 0; i < 6; i++) {
		rtd_write(eth, CPU_RX_BASE(i), i ? 0 : eth->rx_dma);
		writew(0, eth->base + CPU_QUEUE_MAP(i));
	}
	rtd_write(eth, CPU_TX_BASE, eth->tx_dma);
	rtd_write(eth, CPU_TX_BASE + 4, 0);
	rtd_write(eth, 0x160060, 0);
	rtd_write(eth, 0x160064, 0);
	rtd_write(eth, CPU_TX_LIMIT, (RTD_RING_SIZE - 1) * sizeof(*eth->tx));
	rtd_write(eth, CPU_TX_RING_ENABLE, BIT(0));
	rtd_write(eth, CPU_TX_TAIL_ENABLE, BIT(0));
	rtd_write(eth, SWITCH_MODE, 0);
	value = rtd_read(eth, SWITCH_FORWARD);
	rtd_write(eth, SWITCH_FORWARD, value | BIT(0) | BIT(1));
	rtd_write(eth, SWITCH_CONTROL, SWITCH_ENABLE);
	value = rtd_read(eth, PORT_CONTROL(RTD_PORT));
	rtd_write(eth, PORT_CONTROL(RTD_PORT), value | PORT_ENABLE | PORT_RESET_N);
	WRITE_ONCE(eth->running, true);
	napi_enable(&eth->napi);
	rtd_write(eth, CPU_IRQ_STATUS, ~0U);
	dma_wmb();
	rtd_write(eth, CPU_CONTROL, CPU_TX_ENABLE | CPU_RX_ENABLE |
		  CPU_BURST_128WORDS | CPU_BUFFER_2K);
	rtd_write(eth, CPU_IRQ_MASK, CPU_IRQ_EVENTS);
	hrtimer_start(&eth->rx_watchdog, ns_to_ktime(RTD_RX_WATCHDOG_NS),
		      HRTIMER_MODE_REL_SOFT);
	return 0;
}

static int rtd_dma_stop(struct rtd_ethernet *eth)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&eth->irq_lock, flags);
	WRITE_ONCE(eth->running, false);
	rtd_write(eth, CPU_IRQ_MASK, 0);
	rtd_read(eth, CPU_IRQ_MASK);
	spin_unlock_irqrestore(&eth->irq_lock, flags);
	hrtimer_cancel(&eth->rx_watchdog);
	synchronize_irq(eth->irq);
	napi_disable(&eth->napi);
	rtd_write(eth, CPU_IRQ_MASK, 0);
	rtd_write(eth, CPU_CONTROL, 0);
	rtd_read(eth, CPU_CONTROL);
	ret = reset_control_assert(eth->reset);
	if (ret) {
		eth->faulted = true;
		netdev_err(eth->ndev, "DMA reset failed: %d\n", ret);
		return ret;
	}
	usleep_range(1000, 1500);
	rtd_free_rings(eth);
	ret = reset_control_deassert(eth->reset);
	if (ret) {
		eth->faulted = true;
		return ret;
	}
	usleep_range(1000, 1500);
	ret = rtd_control_init(eth);
	if (ret)
		eth->faulted = true;
	return ret;
}

static int rtd_open(struct net_device *ndev)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);
	int ret;

	ret = rtd_dma_start(eth);
	if (ret)
		return ret;
	phylink_start(eth->phylink);
	netif_start_queue(ndev);
	return 0;
}

static int rtd_stop(struct net_device *ndev)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);

	if (!eth->running)
		return eth->faulted ? -EIO : 0;
	netif_tx_disable(ndev);
	phylink_stop(eth->phylink);
	return rtd_dma_stop(eth);
}

static void rtd_reset_work(struct work_struct *work)
{
	struct rtd_ethernet *eth = container_of(work, struct rtd_ethernet, reset_work);

	rtnl_lock();
	if (netif_running(eth->ndev) && !eth->suspended) {
		netif_device_detach(eth->ndev);
		rtd_stop(eth->ndev);
		if (!rtd_open(eth->ndev))
			netif_device_attach(eth->ndev);
		else
			netdev_err(eth->ndev, "Transmit recovery failed\n");
	}
	rtnl_unlock();
}

static void rtd_tx_timeout(struct net_device *ndev, unsigned int queue)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);

	ndev->stats.tx_errors++;
	if (!READ_ONCE(eth->suspended))
		schedule_work(&eth->reset_work);
}

static int rtd_ioctl(struct net_device *ndev, struct ifreq *request, int command)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);

	return phylink_mii_ioctl(eth->phylink, request, command);
}

static int rtd_get_settings(struct net_device *ndev,
			    struct ethtool_link_ksettings *settings)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);

	return phylink_ethtool_ksettings_get(eth->phylink, settings);
}

static int rtd_set_settings(struct net_device *ndev,
			    const struct ethtool_link_ksettings *settings)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);

	return phylink_ethtool_ksettings_set(eth->phylink, settings);
}

static const u32 rtd_registers[] = {
	CPU_CONTROL, CPU_RX_BASE(0), CPU_TX_BASE, CPU_IRQ_MASK, CPU_IRQ_STATUS,
	CPU_DMA_CONTROL, CPU_TX_LIMIT, CPU_TX_RING_ENABLE,
	CPU_TX_TAIL_ENABLE, CPU_FORMAT,
	PORT_CONTROL(RTD_PORT), PORT5_GMII, SWITCH_CONTROL, SWITCH_MODE,
	SWITCH_FORWARD, WRAPPER_GATE,
};

static int rtd_get_regs_len(struct net_device *ndev)
{
	return (ARRAY_SIZE(rtd_registers) + 3 + 12) * sizeof(u32);
}

static void rtd_get_regs(struct net_device *ndev, struct ethtool_regs *regs,
			 void *data)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);
	unsigned long flags;
	u32 *values = data;
	unsigned int i;

	regs->version = 1;
	memset(data, 0, rtd_get_regs_len(ndev));
	if (!READ_ONCE(eth->clocked))
		return;
	spin_lock_irqsave(&eth->tx_lock, flags);
	for (i = 0; i < ARRAY_SIZE(rtd_registers); i++)
		*values++ = rtd_read(eth, rtd_registers[i]);
	*values++ = eth->rx_next;
	*values++ = eth->tx_producer;
	*values++ = eth->tx_consumer;
	for (i = 0; i < 6; i++)
		*values++ = eth->rx ? le32_to_cpu(READ_ONCE(eth->rx[0].word[i])) : 0;
	for (i = 0; i < 6; i++)
		*values++ = eth->tx ? le32_to_cpu(READ_ONCE(eth->tx[0].word[i])) : 0;
	spin_unlock_irqrestore(&eth->tx_lock, flags);
}

static int rtd_get_sset_count(struct net_device *ndev, int sset)
{
	return sset == ETH_SS_STATS ? 1 : -EOPNOTSUPP;
}

static void rtd_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS)
		ethtool_puts(&data, "rx_watchdog_polls");
}

static void rtd_get_stats(struct net_device *ndev, struct ethtool_stats *stats,
			  u64 *data)
{
	struct rtd_ethernet *eth = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&eth->irq_lock, flags);
	data[0] = eth->rx_watchdog_polls;
	spin_unlock_irqrestore(&eth->irq_lock, flags);
}

static const struct ethtool_ops rtd_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = rtd_get_settings,
	.set_link_ksettings = rtd_set_settings,
	.get_regs_len = rtd_get_regs_len,
	.get_regs = rtd_get_regs,
	.get_sset_count = rtd_get_sset_count,
	.get_strings = rtd_get_strings,
	.get_ethtool_stats = rtd_get_stats,
};

static const struct net_device_ops rtd_netdev_ops = {
	.ndo_open = rtd_open,
	.ndo_stop = rtd_stop,
	.ndo_start_xmit = rtd_xmit,
	.ndo_tx_timeout = rtd_tx_timeout,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_eth_ioctl = rtd_ioctl,
};

static int rtd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node;
	struct net_device *ndev;
	struct rtd_ethernet *eth;
	phy_interface_t interface;
	int ret;

	ndev = alloc_etherdev(sizeof(*eth));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	eth = netdev_priv(ndev);
	eth->ndev = ndev;
	eth->dev = dev;
	platform_set_drvdata(pdev, ndev);
	spin_lock_init(&eth->tx_lock);
	spin_lock_init(&eth->irq_lock);
	INIT_WORK(&eth->reset_work, rtd_reset_work);
	hrtimer_setup(&eth->rx_watchdog, rtd_rx_watchdog, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	eth->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eth->base)) {
		ret = PTR_ERR(eth->base);
		goto free;
	}
	eth->irq = platform_get_irq(pdev, 0);
	if (eth->irq < 0) {
		ret = eth->irq;
		goto free;
	}
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto free;
	eth->clocks[0].id = "bus";
	eth->clocks[1].id = "core";
	ret = clk_bulk_get(dev, ARRAY_SIZE(eth->clocks), eth->clocks);
	if (ret)
		goto free;
	eth->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(eth->reset)) {
		ret = PTR_ERR(eth->reset);
		goto put_clocks;
	}
	ret = of_get_phy_mode(dev->of_node, &interface);
	if (ret)
		goto put_clocks;
	node = of_parse_phandle(dev->of_node, "phy-handle", 0);
	if (!node) {
		ret = -EINVAL;
		goto put_clocks;
	}
	ret = of_property_read_u32(node, "reg", &eth->phy_addr);
	of_node_put(node);
	if (ret || eth->phy_addr >= PHY_MAX_ADDR) {
		ret = -EINVAL;
		goto put_clocks;
	}
	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret == -EPROBE_DEFER)
		goto put_clocks;
	if (ret)
		eth_hw_addr_random(ndev);
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto disable;
	eth->mdio = mdiobus_alloc();
	if (!eth->mdio) {
		ret = -ENOMEM;
		goto suspend;
	}
	eth->mdio->name = "RTD1296 switch MDIO";
	eth->mdio->parent = dev;
	eth->mdio->priv = eth;
	eth->mdio->read = rtd_mdio_read;
	eth->mdio->write = rtd_mdio_write;
	snprintf(eth->mdio->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	node = of_get_child_by_name(dev->of_node, "mdio");
	ret = of_mdiobus_register(eth->mdio, node);
	of_node_put(node);
	if (ret)
		goto free_mdio;
	eth->link_config.dev = &ndev->dev;
	eth->link_config.type = PHYLINK_NETDEV;
	eth->link_config.mac_managed_pm = true;
	eth->link_config.mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
					   MAC_10 | MAC_100 | MAC_1000FD;
	phy_interface_set_rgmii(eth->link_config.supported_interfaces);
	eth->phylink = phylink_create(&eth->link_config, dev_fwnode(dev), interface,
				      &rtd_mac_ops);
	if (IS_ERR(eth->phylink)) {
		ret = PTR_ERR(eth->phylink);
		goto unregister_mdio;
	}
	ret = phylink_of_phy_connect(eth->phylink, dev->of_node, 0);
	if (ret)
		goto destroy_link;
	netif_napi_add(ndev, &eth->napi, rtd_poll);
	ret = devm_request_irq(dev, eth->irq, rtd_irq, 0, dev_name(dev), eth);
	if (ret)
		goto delete_napi;
	ndev->netdev_ops = &rtd_netdev_ops;
	ndev->ethtool_ops = &rtd_ethtool_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;
	ndev->priv_flags |= IFF_UNICAST_FLT;
	ndev->watchdog_timeo = 5 * HZ;
	ret = register_netdev(ndev);
	if (ret)
		goto free_irq;
	return 0;
free_irq:
	devm_free_irq(dev, eth->irq, eth);
delete_napi:
	netif_napi_del(&eth->napi);
	rtnl_lock();
	phylink_disconnect_phy(eth->phylink);
	rtnl_unlock();
destroy_link:
	phylink_destroy(eth->phylink);
unregister_mdio:
	mdiobus_unregister(eth->mdio);
free_mdio:
	mdiobus_free(eth->mdio);
suspend:
	pm_runtime_disable(dev);
	rtd_runtime_suspend(dev);
	pm_runtime_put_noidle(dev);
	pm_runtime_set_suspended(dev);
	goto put_clocks;
disable:
	pm_runtime_disable(dev);
put_clocks:
	clk_bulk_put(ARRAY_SIZE(eth->clocks), eth->clocks);
free:
	free_netdev(ndev);
	return dev_err_probe(dev, ret, "Ethernet initialization failed\n");
}

static void rtd_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct rtd_ethernet *eth = netdev_priv(ndev);

	rtnl_lock();
	WRITE_ONCE(eth->suspended, true);
	netif_device_detach(ndev);
	rtnl_unlock();
	cancel_work_sync(&eth->reset_work);
	unregister_netdev(ndev);
	devm_free_irq(&pdev->dev, eth->irq, eth);
	netif_napi_del(&eth->napi);
	rtnl_lock();
	phylink_disconnect_phy(eth->phylink);
	rtnl_unlock();
	phylink_destroy(eth->phylink);
	mdiobus_unregister(eth->mdio);
	mdiobus_free(eth->mdio);
	pm_runtime_disable(&pdev->dev);
	rtd_runtime_suspend(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	clk_bulk_put(ARRAY_SIZE(eth->clocks), eth->clocks);
	if (eth->rx || eth->tx)
		dev_err(&pdev->dev, "Retaining DMA buffers after failed reset\n");
	else
		free_netdev(ndev);
}

static int rtd_suspend(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct rtd_ethernet *eth = netdev_priv(ndev);
	int ret;

	rtnl_lock();
	WRITE_ONCE(eth->suspended, true);
	netif_device_detach(ndev);
	rtnl_unlock();
	cancel_work_sync(&eth->reset_work);
	rtnl_lock();
	if (netif_running(ndev)) {
		ret = rtd_stop(ndev);
		if (ret)
			goto out;
	}
	ret = phy_suspend(ndev->phydev);
	if (!ret)
		ret = pm_runtime_force_suspend(dev);
	if (ret) {
		phy_resume(ndev->phydev);
		if (!netif_running(ndev) || !rtd_open(ndev)) {
			WRITE_ONCE(eth->suspended, false);
			netif_device_attach(ndev);
		}
	}
out:
	rtnl_unlock();
	return ret;
}

static int rtd_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct rtd_ethernet *eth = netdev_priv(ndev);
	int ret;

	rtnl_lock();
	ret = pm_runtime_force_resume(dev);
	if (ret)
		goto out;
	ret = phy_init_hw(ndev->phydev);
	if (ret)
		goto power_off;
	ret = phy_resume(ndev->phydev);
	if (ret)
		goto power_off;
	if (netif_running(ndev)) {
		ret = rtd_open(ndev);
		if (ret)
			goto power_off;
	}
	WRITE_ONCE(eth->suspended, false);
	netif_device_attach(ndev);
	goto out;
power_off:
	pm_runtime_force_suspend(dev);
out:
	rtnl_unlock();
	return ret;
}

static void rtd_shutdown(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct rtd_ethernet *eth = netdev_priv(ndev);

	rtnl_lock();
	WRITE_ONCE(eth->suspended, true);
	netif_device_detach(ndev);
	rtnl_unlock();
	cancel_work_sync(&eth->reset_work);
	rtnl_lock();
	if (eth->running)
		rtd_stop(ndev);
	if (eth->clocked)
		rtd_runtime_suspend(&pdev->dev);
	rtnl_unlock();
}

static const struct dev_pm_ops rtd_pm_ops = {
	SET_RUNTIME_PM_OPS(rtd_runtime_suspend, rtd_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(rtd_suspend, rtd_resume)
};

static const struct of_device_id rtd_of_match[] = {
	{ .compatible = "realtek,rtd1296-ethernet" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd_of_match);

static struct platform_driver rtd_driver = {
	.probe = rtd_probe,
	.remove = rtd_remove,
	.shutdown = rtd_shutdown,
	.driver = {
		.name = "rtd1296-ethernet",
		.of_match_table = rtd_of_match,
		.pm = &rtd_pm_ops,
	},
};
module_platform_driver(rtd_driver);

MODULE_DESCRIPTION("Realtek RTD1296 RGMII0 Ethernet");
MODULE_LICENSE("GPL");
