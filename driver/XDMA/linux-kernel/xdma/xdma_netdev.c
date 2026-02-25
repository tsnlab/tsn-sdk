#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <net/flow_offload.h>
#include <linux/skbuff.h>

#include "xdma_netdev.h"
#include "xdma_mod.h"
#include "cdev_sgdma.h"
#include "libxdma.h"
#include "tsn.h"
#include "alinx_arch.h"
#include "frer.h"

#define LOWER_29_BITS ((1ULL << 29) - 1)
#define TX_WORK_OVERFLOW_MARGIN 100

#define WORKAROUND_PAD 5

static size_t workaround_packet_size(size_t size) {
#ifdef __LIBXDMA_RPI__
        if (size >= 95 && size <= 99) {
                return size + WORKAROUND_PAD;
        }
#endif
        return size;
}

void tx_desc_set(struct xdma_desc *desc, dma_addr_t addr, u32 len)
{
        u32 control_field;
        u32 control;

        desc->control = cpu_to_le32(DESC_MAGIC);
        control_field = XDMA_DESC_STOPPED;
        control_field |= XDMA_DESC_EOP;
        control_field |= XDMA_DESC_COMPLETED;
        control = le32_to_cpu(desc->control & ~(LS_BYTE_MASK));
        control |= control_field;
        desc->control = cpu_to_le32(control);

        desc->src_addr_lo = cpu_to_le32(PCI_DMA_L(addr));
        desc->src_addr_hi = cpu_to_le32(PCI_DMA_H(addr));
        desc->bytes = cpu_to_le32(len);
}

void rx_desc_set(struct xdma_desc *desc, dma_addr_t addr, u32 len)
{
        u32 control_field;
        u32 control;

        desc->control = cpu_to_le32(DESC_MAGIC);
        control_field = XDMA_DESC_STOPPED;
        control_field |= XDMA_DESC_EOP;
        control_field |= XDMA_DESC_COMPLETED;
        control = le32_to_cpu(desc->control & ~(LS_BYTE_MASK));
        control |= control_field;
        desc->control = cpu_to_le32(control);

        desc->dst_addr_lo = cpu_to_le32(PCI_DMA_L(addr));
        desc->dst_addr_hi = cpu_to_le32(PCI_DMA_H(addr));
        desc->bytes = cpu_to_le32(len);
}

int xdma_netdev_open(struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        u32 lo, hi;
        unsigned long flag;
        int i;

        netif_carrier_on(ndev);
        netif_start_queue(ndev);
        for (i = 0; i < TX_QUEUE_COUNT; i++) {
                netif_start_subqueue(ndev, i);
        }

        if (common->open_cnt++ > 0) {
                return 0;
        }

        /* Set the RX descriptor */
        common->rx_desc->src_addr_lo = cpu_to_le32(PCI_DMA_L(common->res_dma_addr));
        common->rx_desc->src_addr_hi = cpu_to_le32(PCI_DMA_H(common->res_dma_addr));
        rx_desc_set(common->rx_desc, common->rx_dma_addr, XDMA_BUFFER_SIZE);
        spin_lock_irqsave(&common->rx_lock, flag);
        ioread32(&common->rx_engine->regs->status_rc);

        /* RX start */
        lo = cpu_to_le32(PCI_DMA_L(common->rx_bus_addr));
        iowrite32(lo, &common->rx_engine->sgdma_regs->first_desc_lo);

        hi = cpu_to_le32(PCI_DMA_H(common->rx_bus_addr));
        iowrite32(hi, &common->rx_engine->sgdma_regs->first_desc_hi);

        iowrite32(DMA_ENGINE_START, &common->rx_engine->regs->control);
        spin_unlock_irqrestore(&common->rx_lock, flag);

        return 0;
}

int xdma_netdev_close(struct net_device *ndev)
{
        int i;
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        iowrite32(DMA_ENGINE_STOP, &common->rx_engine->regs->control);
        netif_stop_queue(ndev);
        for (i = 0; i < TX_QUEUE_COUNT; i++) {
                netif_stop_subqueue(ndev, i);
        }
        if (--common->open_cnt == 0 && common->tx_skb) {
                dev_kfree_skb_any(common->tx_skb);
                common->tx_skb = NULL;
        }
        pr_info("xdma_netdev_close\n");
        netif_carrier_off(ndev);
        pr_info("netif_carrier_off\n");
        return 0;
}

netdev_tx_t xdma_netdev_start_xmit(struct sk_buff *skb,
                struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        struct xdma_dev *xdev = common->xdev;
        sysclock_t sys_count, sys_count_upper, sys_count_lower;
        timestamp_t now;
        u16 frame_length;
        dma_addr_t dma_addr;
        struct tx_buffer* tx_buffer;
        struct tx_metadata* tx_metadata;
        struct xdma_tx_queue *queue;
        u32 to_value;
        unsigned long flags;


        /* Check desc count */
        xdma_debug("xdma_netdev_start_xmit(skb->len : %d)\n", skb->len);

        frame_length = max((unsigned int)ETH_ZLEN, skb->len);
        frame_length = workaround_packet_size(frame_length);

        if (skb_put_padto(skb, frame_length)) {
                return NETDEV_TX_OK;
        }

        /* Jumbo frames not supported */
        if (skb->len > XDMA_BUFFER_SIZE) {
#ifdef __LIBXDMA_DEBUG__
                pr_err("Jumbo frames not supported\n");
#endif
                dev_kfree_skb(skb);
                return NETDEV_TX_OK;
        }

        /* Add metadata to the skb */
        if (pskb_expand_head(skb, TX_METADATA_SIZE,
                             (priv->port_flag & XDMA_PORT_FLAG_FRER) ? FRER_RTAG_SIZE : 0,
                             GFP_ATOMIC) != 0) {
#ifdef __LIBXDMA_DEBUG__
                pr_err("pskb_expand_head failed\n");
#endif
                dev_kfree_skb(skb);
                return NETDEV_TX_OK;
        }
        skb_push(skb, TX_METADATA_SIZE);
        memset(skb->data, 0, TX_METADATA_SIZE);

        xdma_debug("skb->len : %d\n", skb->len);
        tx_buffer = (struct tx_buffer*)skb->data;
        /* Fill in the metadata */
        tx_metadata = (struct tx_metadata*)&tx_buffer->metadata;
        tx_metadata->frame_length = frame_length;

        sys_count = alinx_get_sys_clock_by_xdev(xdev);
        // FIXME: Remove this after resolving the sysclock adjustment issue
        sys_count -= ((sysclock_t)xdev->sysclock_adjustment << 32);
        now = alinx_sysclock_to_timestamp(common->pdev, sys_count);
        sys_count_lower = sys_count & LOWER_29_BITS;
        sys_count_upper = sys_count & ~LOWER_29_BITS;


	/* Set the fromtick & to_tick values based on the lower 29 bits of the system count */
	if (tsn_fill_metadata(xdev->pdev, now, skb) == false) {
#ifdef __LIBXDMA_DEBUG__
		pr_warn("tsn_fill_metadata failed\n");
#endif
                /* HW Queue is almost full, but there is still some room left */
                /* Next frames have to wait for the queue to be available */
                xdma_stop_all_queues(xdev);
	}

	/* FRER (802.1CB): Insert R-TAG with auto stream registration */
	if (xdev->tsn_config.frer && xdev->tsn_config.frer->enabled && (priv->port_flag & XDMA_PORT_FLAG_FRER)) {
		struct ethhdr *eth = (struct ethhdr *)(tx_buffer->data);
		struct frer_stream *stream;
		unsigned long frer_flags;
		
		spin_lock_irqsave(&xdev->tsn_config.frer->lock, frer_flags);
		stream = frer_stream_lookup(xdev->tsn_config.frer, 
					    eth->h_source, eth->h_dest);
		
		/* Auto-register stream if not found */
		if (!stream)
			stream = frer_auto_register_stream(xdev->tsn_config.frer,
							   eth->h_source, eth->h_dest);
		
		if (!stream) {
			pr_err_ratelimited("FRER: Failed to get/create stream - dropping packet\\n");
			spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		} else if (stream->seq_gen.active) {
			/* Use TX-aware R-TAG insertion */
			if (frer_insert_rtag_tx(skb, stream, TX_METADATA_SIZE, frame_length) < 0) {
				pr_err("FRER: Failed to insert R-TAG (tailroom=%d, need=%lu)\\n",
				       skb_tailroom(skb), FRER_RTAG_SIZE);
				spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
				dev_kfree_skb(skb);
				return NETDEV_TX_OK;
			}
			/* Update frame length after R-TAG insertion */
			tx_metadata->frame_length += FRER_RTAG_SIZE;
			frame_length += FRER_RTAG_SIZE;
		} else {
			pr_err_ratelimited("FRER: Stream exists but seq_gen not active - dropping packet\\n");
			spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		}
		spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
	}

        xdma_debug("0x%08x  0x%08x  0x%08x  %4d  %1d",
                sys_count_lower, tx_metadata->from.tick, tx_metadata->to.tick,
                tx_metadata->frame_length, tx_metadata->fail_policy);
        dump_buffer((unsigned char*)tx_metadata, (int)(sizeof(struct tx_metadata) + skb->len));

        spin_lock_irqsave(&common->tx_queue_lock, flags);

        switch (tx_metadata->from.priority) {
                case TSN_PRIO_GPTP:
                        queue = &common->gptp_tx_queue;
                        break;
                case TSN_PRIO_VLAN:
                        queue = &common->vlan_tx_queue;
                        break;
                default:
                        queue = &common->be_tx_queue;
                        break;
        }

        if (xdma_tx_queue_is_full(queue)) {
#ifdef __LIBXDMA_DEBUG__
                pr_err("TX queue is full\n");
#endif
                spin_unlock_irqrestore(&common->tx_queue_lock, flags);
                return NETDEV_TX_BUSY;
        }

        dma_addr = dma_map_single(&xdev->pdev->dev, skb->data, skb->len, DMA_TO_DEVICE);
        if (unlikely(dma_mapping_error(&xdev->pdev->dev, dma_addr))) {
                pr_err("dma_map_single failed\n");
                spin_unlock_irqrestore(&common->tx_queue_lock, flags);
                return NETDEV_TX_BUSY;
        }

        if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) {
                if (common->tstamp_config.tx_type == HWTSTAMP_TX_ON &&
                        !test_and_set_bit_lock(tx_metadata->timestamp_id, &common->state)) {
                        skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
                        common->tx_work_skb[tx_metadata->timestamp_id] = skb_get(skb);
                        /*
                         * Even if the driver's intention was sys_count == from,
                         * there might be a slight error caused during conversion (sysclock <-> timestamp)
                         * So if the diffence is small, don't consider it as overflow.
                         * Adding/Subtracting directly from values can cause another overflow,
                         * so just calculate the difference.
                         */
                        common->tx_work_start_after[tx_metadata->timestamp_id] = sys_count_upper | tx_metadata->from.tick;
                        if (sys_count_lower > tx_metadata->from.tick && sys_count_lower - tx_metadata->from.tick > TX_WORK_OVERFLOW_MARGIN) {
                                // Overflow
                                common->tx_work_start_after[tx_metadata->timestamp_id] += (1 << 29);
                        }
                        to_value = (tx_metadata->fail_policy == TSN_FAIL_POLICY_RETRY ? tx_metadata->delay_to.tick : tx_metadata->to.tick);
                        common->tx_work_wait_until[tx_metadata->timestamp_id] = sys_count_upper | to_value;
                        if (sys_count_lower > to_value && sys_count_lower - to_value > TX_WORK_OVERFLOW_MARGIN) {
                                // Overflow
                                common->tx_work_wait_until[tx_metadata->timestamp_id] += (1 << 29);
                        }
                        schedule_work(&common->tx_work[tx_metadata->timestamp_id]);
                } else if (common->tstamp_config.tx_type != HWTSTAMP_TX_ON) {
                        pr_warn("Timestamp skipped: timestamp config is off\n");
                } else {
                        pr_warn("Timestamp skipped: driver is waiting for previous packet's timestamp\n");
                }
                // TODO: track the number of skipped packets for ethtool stats
        }

        xdma_tx_queue_enqueue(queue, skb, dma_addr, priv->port_id);
        spin_unlock_irqrestore(&common->tx_queue_lock, flags);

        return NETDEV_TX_OK;
}

u16 xdma_select_queue(struct net_device *ndev, struct sk_buff *skb, struct net_device *sb_dev) {
        /* Always use the first subqueue */
        return 0;
}

static LIST_HEAD(xdma_block_cb_list);

static int xdma_setup_tc_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv) {
        // If mqprio is only used for queue mapping this should not be called
        return -EOPNOTSUPP;
}

int xdma_netdev_setup_tc(struct net_device *ndev, enum tc_setup_type type, void *type_data) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        // TODO: TSN config per port

        switch (type) {
        case TC_SETUP_QDISC_MQPRIO:
                return tsn_set_mqprio(common->pdev, (struct tc_mqprio_qopt_offload*)type_data);
        case TC_SETUP_QDISC_CBS:
                return tsn_set_qav(common->pdev, (struct tc_cbs_qopt_offload*)type_data);
        case TC_SETUP_QDISC_TAPRIO:
                return tsn_set_qbv(common->pdev, (struct tc_taprio_qopt_offload*)type_data);
        case TC_SETUP_BLOCK:
                return flow_block_cb_setup_simple(type_data, &xdma_block_cb_list, xdma_setup_tc_block_cb, priv, priv, true);
        default:
                return -ENOTSUPP;
        }

        return 0;
}

static int xdma_get_ts_config(struct net_device *ndev, struct ifreq *ifr) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        struct hwtstamp_config *config = &common->tstamp_config;

        return copy_to_user(ifr->ifr_data, config, sizeof(*config)) ? -EFAULT : 0;
}

static int xdma_set_ts_config(struct net_device *ndev, struct ifreq *ifr) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        struct hwtstamp_config *config = &common->tstamp_config;

        return copy_from_user(config, ifr->ifr_data, sizeof(*config)) ? -EFAULT : 0;
}

static int frer_ioctl_add_stream(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	struct frer_stream_config config;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&config, data, sizeof(config))) {
		return -EFAULT;
	}

	return frer_add_stream(xdev->tsn_config.frer, &config);
}

static int frer_ioctl_del_stream(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	struct frer_stream_id id;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&id, data, sizeof(id))) {
		return -EFAULT;
	}

	return frer_del_stream(xdev->tsn_config.frer, &id);
}

static int frer_ioctl_get_stats(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	struct frer_stream_stats stats;
	int ret;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&stats, data, sizeof(stats))) {
		return -EFAULT;
	}

	ret = frer_get_stream_stats(xdev->tsn_config.frer, &stats);
	if (ret) {
		return ret;
	}

	if (copy_to_user(data, &stats, sizeof(stats))) {
		return -EFAULT;
	}

	return 0;
}

static int frer_ioctl_enable(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	int enable;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&enable, data, sizeof(enable))) {
		return -EFAULT;
	}

	xdev->tsn_config.frer->enabled = (enable != 0);
	pr_info("FRER: %s\n", xdev->tsn_config.frer->enabled ? "enabled" : "disabled");

	return 0;
}

int xdma_netdev_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd) {
	switch (cmd) {
	case SIOCGHWTSTAMP:
		return xdma_get_ts_config(ndev, ifr);
	case SIOCSHWTSTAMP:
		return xdma_set_ts_config(ndev, ifr);
	default:
		return -EOPNOTSUPP;
	}
}

int xdma_netdev_siocdevprivate(struct net_device *ndev, struct ifreq *ifr, void *data, int cmd) {
	switch (cmd) {
	case SIOC_FRER_ADD_STREAM:
		return frer_ioctl_add_stream(ndev, ifr, data);
	case SIOC_FRER_DEL_STREAM:
		return frer_ioctl_del_stream(ndev, ifr, data);
	case SIOC_FRER_GET_STATS:
		return frer_ioctl_get_stats(ndev, ifr, data);
	case SIOC_FRER_ENABLE:
		return frer_ioctl_enable(ndev, ifr, data);
	default:
		return -EOPNOTSUPP;
	}
}

static void do_tx_work(struct work_struct *work, u16 tstamp_id) {
        sysclock_t tx_tstamp;
        struct xdma_pci_dev *xpdev;
        struct ptp_device_data *ptp_data;
        struct skb_shared_hwtstamps shhwtstamps;
        struct xdma_private_common* common = container_of(work - tstamp_id, struct xdma_private_common, tx_work[0]);
        struct sk_buff* skb = common->tx_work_skb[tstamp_id];
        sysclock_t now = alinx_get_sys_clock_by_xdev(common->xdev);
        unsigned long ptp_flag;

        if (tstamp_id >= TSN_TIMESTAMP_ID_MAX) {
                pr_err("Invalid timestamp ID\n");
                return;
        }

        if (!common->tx_work_skb[tstamp_id]) {
                goto return_error;
        }

        if (now < common->tx_work_start_after[tstamp_id]) {
                goto retry;
        }
        /*
         * Read TX timestamp several times because 
         * the work thread might try to read TX timestamp
         * before the register gets updated
         */
        tx_tstamp = alinx_read_tx_timestamp_by_xdev(common->xdev, tstamp_id);
        if (tx_tstamp == common->last_tx_tstamp[tstamp_id]) {
                if (alinx_get_sys_clock_by_xdev(common->xdev) < common->tx_work_wait_until[tstamp_id]) {
                        /* The packet might have not been sent yet */
                        goto retry;
                }
                /*
                 * Tx timestamp is not updated. Try again.
                 * Waiting for it to be updated forever is not desirable,
                 * so limit the number of retries
                 */
                if (++(common->tstamp_retry[tstamp_id]) >= TX_TSTAMP_MAX_RETRY) {
                        /* TODO: track the number of skipped packets for ethtool stats */
                        pr_warn("Failed to get timestamp: timestamp is not getting updated, " \
                                "the packet might have been dropped\n");
                        goto return_error;
                }
                goto retry;
        }

        xpdev = dev_get_drvdata(&common->xdev->pdev->dev);
        ptp_data = xpdev->ptp;
        if (!ptp_data) {
                pr_err("Invalid ptp_data\n");
                goto return_error;
        }

        common->tstamp_retry[tstamp_id] = 0;
        spin_lock_irqsave(&ptp_data->lock, ptp_flag);
        shhwtstamps.hwtstamp = ns_to_ktime(alinx_sysclock_to_txtstamp(common->pdev, tx_tstamp));
        // pr_err("%s 0x%08llx %llu %u\n", skb->dev->name, tx_tstamp, alinx_sysclock_to_txtstamp(common->pdev, tx_tstamp), common->xdev->tx_timestamp_adjustment[tstamp_id]);
        spin_unlock_irqrestore(&ptp_data->lock, ptp_flag);
        common->last_tx_tstamp[tstamp_id] = tx_tstamp;

        common->tx_work_skb[tstamp_id] = NULL;
        clear_bit_unlock(tstamp_id, &common->state);
        skb_tstamp_tx(skb, &shhwtstamps);
        dev_kfree_skb_any(skb);
        return;

return_error:
        common->tstamp_retry[tstamp_id] = 0;
        clear_bit_unlock(tstamp_id, &common->state);
        return;

retry:
        schedule_work(&common->tx_work[tstamp_id]);
        return;
}

#define DEFINE_TX_WORK(n) \
void xdma_tx_work##n(struct work_struct *work) { \
        do_tx_work(work, n); \
}

DEFINE_TX_WORK(1);
DEFINE_TX_WORK(2);
DEFINE_TX_WORK(3);
DEFINE_TX_WORK(4);
