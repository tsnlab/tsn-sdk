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

#define LOWER_29_BITS ((1ULL << 29) - 1)
#define TX_WORK_OVERFLOW_MARGIN 100

#define WORKAROUND_PAD 5

/* Static function declarations */
static int xdma_netdev_rx_poll(struct xdma_private* priv, int budget);
static int xdma_process_rx_packet(struct xdma_private* priv);
static void xdma_restart_rx_engine(struct xdma_private* priv);
static void xdma_netdev_tx_complete(struct xdma_private* priv);

static size_t workaround_packet_size(size_t size) {
#ifdef __LIBXDMA_RPI__
        if (size >= 95 && size <= 99) {
                return size + WORKAROUND_PAD;
        }
#endif
        return size;
}

static void tx_desc_set(struct xdma_desc *desc, dma_addr_t addr, u32 len)
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

/* -------------------------------
 * TX Complete
 * ------------------------------- */
static void xdma_netdev_tx_complete(struct xdma_private* priv)
{
        struct xdma_tx_ring* ring = &priv->tx_ring;
        struct xdma_dev* xdev = priv->xdev;
        struct xdma_engine* engine = &xdev->engine_h2c[0];
        u32 tail = ring->tail;
        struct xdma_tx_slot* slot = &ring->slot[tail];

        /* Stop DMA */
        iowrite32(DMA_ENGINE_STOP, &engine->regs->control);
        //  pr_info("TX-COMPLETE: stop DMA, freeing slot[%u]\n", tail);

        /* free the completed slot */
        if (slot->skb)
        {
            napi_consume_skb(slot->skb, 1);
            slot->skb = NULL;
            slot->len = 0;
        }
        else
        {
            pr_err("TX-COMPLETE: slot[%u] skb NULL\n", tail);
        }

        /* advance tail */
        ring->tail = (tail + 1) % XDMA_TX_RING_SIZE;
        // pr_info("TX-COMPLETE: new tail=%u\n", ring->tail);
}

/* -------------------------------
 * TX Polling Handler
 * ------------------------------- */
int xdma_netdev_tx_poll(struct xdma_private* priv, int budget)
{
        u32 status_tx;
        int work_done = 0;

        // pr_info("[TX_POLL] enter (budget=%d)\n", budget);

        /* 1. Read DMA engine status (cached from ISR) */
        status_tx = READ_ONCE(priv->last_tx_status);
        // pr_info("[TX_POLL] DMA status=0x%x\n", status_tx);

        udelay(10);

        /* 2. Check descriptor completion */
        if (status_tx & XDMA_STAT_DESC_COMPLETED)
        {
            /* 3. TX Complete */
            xdma_netdev_tx_complete(priv);

            WRITE_ONCE(priv->last_tx_status, 0);

            // pr_info("[TX_POLL] TX complete (total=%llu, done=%u)\n", priv->total_tx_count, priv->tx_packets);

            work_done = 1;
            // pr_info("[TX_POLL] work_done = 1\n");
        }

        smp_wmb();

        /* 4. Wake TX queue */
        if (netif_queue_stopped(priv->ndev))
        {
            netif_wake_queue(priv->ndev);
            // pr_warn("[TX_POLL] netif_wake_queue(priv->ndev)\n");
        }

        // pr_info("[TX_POLL] exit (work_done=%d)\n", work_done);
        return work_done;
}

/* ------------------------------------------------------------------
 * NAPI RX Poll Function
 *  - Called when ISR schedules NAPI after RX descriptor complete
 *  - Handles packet processing and safely restarts RX DMA engine
 * ------------------------------------------------------------------ */
static int xdma_netdev_rx_poll(struct xdma_private* priv, int budget)
{
        // struct xdma_engine* engine = priv->rx_engine;
        struct xdma_engine* engine = &priv->xdev->engine_c2h[0];
        struct xdma_result* result = priv->res;
        int work_done = 0;
        u32 status;

        // pr_info("[RX_POLL] enter (budget=%d)\n", budget);

        while (work_done < budget)
        {
            /* Stop flag check (e.g., device close) */
            if (READ_ONCE(priv->b_stop_rx_polling))
            {
                // pr_info("[RX_POLL] stop flag detected, exiting loop\n");
                break;
            }

            /* Read current DMA status */
            status = ioread32(&engine->regs->status);
            // pr_info("[RX_POLL] DMA status=0x%x\n", status);

            /* Check software RX pending flag */
            if (!READ_ONCE(priv->rx_pending))
            {
                pr_debug("[RX_POLL] no RX pending flag set\n");
                break;
            }

            /* Clear RX pending before processing (avoid race) */
            priv->rx_pending = false;

            /* Process received packet */
            // pr_info("[RX_POLL] descriptor completed ? process_rx_packet()\n");
            xdma_process_rx_packet(priv);

            /* Clear DMA completion status */
            iowrite32(XDMA_STAT_DESC_COMPLETED | XDMA_STAT_BUSY, &engine->regs->status_rc);

            /* Fully restart RX engine using helper */
            xdma_restart_rx_engine(priv);
            // pr_info("[RX_POLL] RX engine restarted\n");

            work_done++;
        }

        // pr_info("[RX_POLL] exit (work_done=%d)\n", work_done);
        return work_done;
}

static int xdma_process_rx_packet(struct xdma_private* priv)
{
        struct xdma_dev* xdev = priv->xdev;
        struct net_device* ndev = priv->ndev;
        struct sk_buff* skb;
        struct xdma_result* result = priv->res;
        int skb_len;

        if (!xdev || !ndev)
        {
            pr_err("[RX_PROC] invalid device context\n");
            return -EINVAL;
        }

        if (result->length <= 0)
        {
            pr_err("[RX_PROC] invalid skb_len=%d\n", result->length);
            return -EINVAL;
        }

        if (result->length > XDMA_BUFFER_SIZE)
        {
            pr_err("[RX_PROC] result->length too large: %d (max %d)\n", result->length, XDMA_BUFFER_SIZE);
            return -EINVAL;
        }

        skb_len = result->length - RX_METADATA_SIZE - CRC_LEN;
        if (skb_len <= 0)
        {
            pr_err("[RX_PROC] invalid skb_len=%d (result->length=%d)\n", skb_len, result->length);
            return -EINVAL;
        }

        if (skb_len > ETH_FRAME_LEN)
        {
            pr_err("[RX_PROC] skb_len too large: %d (result->length=%d)\n", skb_len, result->length);
            return -EINVAL;
        }

        skb = dev_alloc_skb(skb_len);
        if (!skb)
        {
            pr_err("[RX_PROC] skb allocation failed\n");
            return -ENOMEM;
        }

        memcpy(skb_put(skb, skb_len), priv->rx_buffer + RX_METADATA_SIZE, skb_len);

        skb->dev = ndev;
        skb->protocol = eth_type_trans(skb, ndev);

        netif_receive_skb(skb);

        // pr_info("[RX_PROC] packet handed to network stack (len=%d)\n", skb_len);
        return 0;
}

/* RX engine restart after completion */
static void xdma_restart_rx_engine(struct xdma_private* priv)
{
        struct xdma_dev* xdev = priv->xdev;
        struct xdma_engine* engine = &xdev->engine_c2h[0];
        unsigned long flags;
        u32 lo, hi;

        spin_lock_irqsave(&priv->rx_lock, flags);

        /* Reset rx buffer */
        memset(priv->rx_buffer, 0, XDMA_BUFFER_SIZE);

        /* RX desc set to receive next packet */
        rx_desc_set(priv->rx_desc, priv->rx_dma_addr, XDMA_BUFFER_SIZE);

        lo = cpu_to_le32(PCI_DMA_L(priv->rx_bus_addr));
        iowrite32(lo, &engine->sgdma_regs->first_desc_lo);

        hi = cpu_to_le32(PCI_DMA_H(priv->rx_bus_addr));
        iowrite32(hi, &engine->sgdma_regs->first_desc_hi);

        iowrite32(DMA_ENGINE_START, &engine->regs->control);

        spin_unlock_irqrestore(&priv->rx_lock, flags);
        // pr_info("[RX_RESTART] engine restarted (INT re-enabled)\n");
}

int xdma_napi_poll(struct napi_struct* napi, int budget)
{
        struct xdma_private* priv = container_of(napi, struct xdma_private, napi);
        int work_done = 0;

        struct xdma_dev* xdev = priv->xdev;

        /* check network device validity */
        if (unlikely(!priv || !xdev))
        {
            if (napi_complete_done(napi, 0)) {
                // complete the abnormal calls during initialization
            }
            return 0;
        }

        u32 irq_mask = xdev->engine_c2h[0].irq_bitmask | xdev->engine_h2c[0].irq_bitmask;

        /* RX first */
        work_done = xdma_netdev_rx_poll(priv, budget);

        /* TX only if remaining budget exists */
        if (work_done < budget)
        {
            work_done += xdma_netdev_tx_poll(priv, budget - work_done);
        }

        /* Stop request */
        if (priv->b_stop_rx_polling)
        {
            pr_info("xdma_napi_poll b_stop_rx_polling\n");
            napi_complete_done(napi, work_done);
            priv->b_stopped_rx_polling = true;

            return work_done;
        }

        /* Normal completion */
        if (work_done < budget)
        {
            // pr_info("xdma_napi_poll work_done: %d, budget: %d -> NAPI complete and IRQ re-enable\n", work_done, budget);

            if (napi_complete_done(napi, work_done))
            {
                channel_interrupts_enable(xdev, irq_mask);
            }
        }

        return work_done;
}

/* NAPI enable */
void xdma_enable_napi(struct xdma_private* priv)
{
        if (!priv->napi_enabled)
        {
            napi_enable(&priv->napi);
            priv->napi_enabled = true;
            pr_info("NAPI enabled for xdma_netdev\n");
        }
}

/* NAPI disable */
void xdma_disable_napi(struct xdma_private* priv)
{
        if (priv->napi_enabled)
        {
            napi_disable(&priv->napi);
            priv->napi_enabled = false;
            pr_info("NAPI disabled for xdma_netdev\n");
        }
}

int xdma_netdev_open(struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        u32 lo, hi;
        unsigned long flag;
        int i;

        if (!priv->ndev)
        {
            priv->ndev = ndev;
            pr_info("xdma_netdev_open(): linked xdev->ndev to %s\n", ndev->name);
        }

        /* Device ready + clear OFFLINE flag */
        xdma_device_flag_clear(priv->xdev, XDEV_FLAG_OFFLINE);
        netif_device_attach(ndev);

        /* Enable IRQ (disabled in libxdma) */
        enable_irq(priv->xdev->pdev->irq);
        pr_info("xdma_netdev_open(): IRQ enabled\n");

        /* Reset RX/TX software state */
        priv->b_stop_rx_polling = false;
        priv->b_stopped_rx_polling = false;
        priv->last_tx_status = 0;

        /* Enable NAPI */
        xdma_enable_napi(priv);
        pr_info("xdma_netdev_open(): NAPI initialized\n");

        /* --------------------------------------------------------
        * Initialize TX DMA Ring (NEW)
        * --------------------------------------------------------
        * All TX packets will be copied into these pre-allocated
        * DMA-coherent buffers to eliminate dma_map_single() and
        * prevent memory corruption.
        */
        if (xdma_tx_ring_init(&priv->tx_ring, &priv->pdev->dev))
        {
            pr_err("xdma_netdev_open(): TX ring init failed\n");
            return -ENOMEM;
        }
        pr_info("xdma_netdev_open(): TX ring initialized\n");

        /* Set the RX descriptor */
        priv->rx_desc->src_addr_lo = cpu_to_le32(PCI_DMA_L(priv->res_dma_addr));
        priv->rx_desc->src_addr_hi = cpu_to_le32(PCI_DMA_H(priv->res_dma_addr));
        rx_desc_set(priv->rx_desc, priv->rx_dma_addr, XDMA_BUFFER_SIZE);
        spin_lock_irqsave(&priv->rx_lock, flag);
        ioread32(&priv->rx_engine->regs->status_rc);

        /* RX start */
        lo = cpu_to_le32(PCI_DMA_L(priv->rx_bus_addr));
        iowrite32(lo, &priv->rx_engine->sgdma_regs->first_desc_lo);

        hi = cpu_to_le32(PCI_DMA_H(priv->rx_bus_addr));
        iowrite32(hi, &priv->rx_engine->sgdma_regs->first_desc_hi);

        iowrite32(DMA_ENGINE_START, &priv->rx_engine->regs->control);
        spin_unlock_irqrestore(&priv->rx_lock, flag);

        netif_carrier_on(ndev);
        netif_start_queue(ndev);
        for (i = 0; i < TX_QUEUE_COUNT; i++)
        {
                netif_start_subqueue(ndev, i);
        }

        pr_info("xdma_netdev_open(): carrier ON + TX queues started\n");

        return 0;
}

int xdma_netdev_close(struct net_device *ndev)
{
        int i;
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_dev* xdev = priv->xdev;

        /* 1. Stop flag set (prevent ISR from scheduling NAPI) */
        WRITE_ONCE(priv->b_stop_rx_polling, true);

        /* 2. Stop DMA engines */
            iowrite32(DMA_ENGINE_STOP, &priv->rx_engine->regs->control);
        iowrite32(DMA_ENGINE_STOP, &priv->tx_engine->regs->control);

        /* 3. Mask all interrupts */
        channel_interrupts_disable(xdev, 0xFFFFFFFF);
        user_interrupts_disable(xdev, 0xFFFFFFFF);

        /* 4. Synchronize pending IRQs */
        if (priv->irq)
            synchronize_irq(priv->irq);

        /* 5. Disable NAPI */
        if (priv->napi_enabled)
        {
            napi_disable(&priv->napi);
            priv->napi_enabled = false;
            pr_info("NAPI disabled for xdma_netdev\n");
        }

        /* 6. Stop TX/RX queues */
        netif_stop_queue(ndev);
        for (i = 0; i < TX_QUEUE_COUNT; i++)
        {
                netif_stop_subqueue(ndev, i);
        }

        netif_carrier_off(ndev);
        pr_info("netif_carrier_off\n");

        /* Free all DMA-coherent TX buffers allocated at open(). */
        xdma_tx_ring_cleanup(&priv->tx_ring);
        pr_info("xdma_netdev_close(): TX ring cleaned up\n");
        return 0;
}

netdev_tx_t xdma_netdev_start_xmit(struct sk_buff *skb,
                struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_dev *xdev = priv->xdev;
        u32 w;
        sysclock_t sys_count, sys_count_upper, sys_count_lower;
        timestamp_t now;
        u16 frame_length;
        struct tx_buffer* tx_buffer;
        struct tx_metadata* tx_metadata;
        u32 to_value;
        u16 q;
        u32 head, tail;
        u32 next_head;
        struct xdma_tx_ring* ring = &priv->tx_ring;
        struct xdma_tx_slot* slot;

        /* Check desc count */
        q = skb_get_queue_mapping(skb);
        netif_stop_subqueue(ndev, q);
        xdma_debug("xdma_netdev_start_xmit(skb->len : %d)\n", skb->len);

        head = ring->head;
        tail = ring->tail;
        next_head = (head + 1) % XDMA_TX_RING_SIZE;

        /* Ring full? Should not happen since we TX one packet at a time */
        if (unlikely(next_head == tail))
        {
            pr_err("TX-XMIT: ring full (head=%u tail=%u)\n", head, tail);
            return NETDEV_TX_BUSY;
        }

        /* Use head slot (producer slot) */
        slot = &ring->slot[head];
        if (unlikely(slot->skb != NULL))
        {
            pr_err("TX-ERROR: head slot[%u] is busy (skb=%p)\n", head, slot->skb);
            return NETDEV_TX_BUSY;
        }
        // pr_info("TX-XMIT START: head=%u tail=%u skb_len=%u\n", head, tail, skb->len);

        skb->len = max((unsigned int)ETH_ZLEN, skb->len);

        /* Store packet length */
        frame_length = skb->len;
        skb->len = workaround_packet_size(skb->len);

        if (skb_padto(skb, skb->len))
        {
                pr_err("skb_padto failed\n");
                netif_wake_subqueue(ndev, q);
                dev_kfree_skb(skb);
                return NETDEV_TX_OK;
        }

        /* Jumbo frames not supported */
        if (skb->len > XDMA_BUFFER_SIZE)
        {
#ifdef __LIBXDMA_DEBUG__
                pr_err("Jumbo frames not supported\n");
#endif
                netif_wake_subqueue(ndev, q);
                dev_kfree_skb(skb);
                return NETDEV_TX_OK;
        }

        /* Add metadata to the skb */
        if (pskb_expand_head(skb, TX_METADATA_SIZE, 0, GFP_ATOMIC) != 0) {
#ifdef __LIBXDMA_DEBUG__
                pr_err("pskb_expand_head failed\n");
#endif
                netif_wake_subqueue(ndev, q);
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
        now = alinx_sysclock_to_timestamp(priv->pdev, sys_count);
        sys_count_lower = sys_count & LOWER_29_BITS;
        sys_count_upper = sys_count & ~LOWER_29_BITS;


        /* Set the fromtick & to_tick values based on the lower 29 bits of the system count */
        if (tsn_fill_metadata(xdev->pdev, now, skb) == false) {
                // TODO: Increment SW drop stats
#ifdef __LIBXDMA_DEBUG__
                pr_warn("tsn_fill_metadata failed\n");
#endif
                netif_wake_subqueue(ndev, q);
                return NETDEV_TX_BUSY;
        }

        xdma_debug("0x%08x  0x%08x  0x%08x  %4d  %1d",
                sys_count_lower, tx_metadata->from.tick, tx_metadata->to.tick,
                tx_metadata->frame_length, tx_metadata->fail_policy);
        dump_buffer((unsigned char*)tx_metadata, (int)(sizeof(struct tx_metadata) + skb->len));

        if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) {
                if (priv->tstamp_config.tx_type == HWTSTAMP_TX_ON &&
                        !test_and_set_bit_lock(tx_metadata->timestamp_id, &priv->state)) {
                        skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
                        priv->tx_work_skb[tx_metadata->timestamp_id] = skb_get(skb);
                        /*
                         * Even if the driver's intention was sys_count == from,
                         * there might be a slight error caused during conversion (sysclock <-> timestamp)
                         * So if the diffence is small, don't consider it as overflow.
                         * Adding/Subtracting directly from values can cause another overflow,
                         * so just calculate the difference.
                         */
                        priv->tx_work_start_after[tx_metadata->timestamp_id] = sys_count_upper | tx_metadata->from.tick;
                        if (sys_count_lower > tx_metadata->from.tick && sys_count_lower - tx_metadata->from.tick > TX_WORK_OVERFLOW_MARGIN) {
                                // Overflow
                                priv->tx_work_start_after[tx_metadata->timestamp_id] += (1 << 29);
                        }
                        to_value = (tx_metadata->fail_policy == TSN_FAIL_POLICY_RETRY ? tx_metadata->delay_to.tick : tx_metadata->to.tick);
                        priv->tx_work_wait_until[tx_metadata->timestamp_id] = sys_count_upper | to_value;
                        if (sys_count_lower > to_value && sys_count_lower - to_value > TX_WORK_OVERFLOW_MARGIN) {
                                // Overflow
                                priv->tx_work_wait_until[tx_metadata->timestamp_id] += (1 << 29);
                        }
                        schedule_work(&priv->tx_work[tx_metadata->timestamp_id]);
                } else if (priv->tstamp_config.tx_type != HWTSTAMP_TX_ON) {
                        pr_warn("Timestamp skipped: timestamp config is off\n");
                } else {
                        pr_warn("Timestamp skipped: driver is waiting for previous packet's timestamp\n");
                }
                // TODO: track the number of skipped packets for ethtool stats
        }

        /* Copy into DMA-coherent slot buffer */
        memcpy(slot->vaddr, skb->data, skb->len);
        slot->skb = skb;
        slot->len = skb->len;

        /* netif_wake_queue() will be called in xdma_isr() */

        /* Set descriptor to this slot's dma buffer */
        tx_desc_set(priv->tx_desc, slot->dma, slot->len);

        w = cpu_to_le32(PCI_DMA_L(priv->tx_bus_addr));
        iowrite32(w, xdev->bar[1] + DESC_REG_LO);

        w = cpu_to_le32(PCI_DMA_H(priv->tx_bus_addr));
        iowrite32(w, xdev->bar[1] + DESC_REG_HI);
        iowrite32(0, xdev->bar[1] + DESC_REG_HI + 4);

        iowrite32(DMA_ENGINE_START, &priv->tx_engine->regs->control);

        /* Advance HEAD pointer (producer moves) */
        ring->head = next_head;
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

        switch (type) {
        case TC_SETUP_QDISC_MQPRIO:
                return tsn_set_mqprio(priv->pdev, (struct tc_mqprio_qopt_offload*)type_data);
        case TC_SETUP_QDISC_CBS:
                return tsn_set_qav(priv->pdev, (struct tc_cbs_qopt_offload*)type_data);
        case TC_SETUP_QDISC_TAPRIO:
                return tsn_set_qbv(priv->pdev, (struct tc_taprio_qopt_offload*)type_data);
        case TC_SETUP_BLOCK:
                return flow_block_cb_setup_simple(type_data, &xdma_block_cb_list, xdma_setup_tc_block_cb, priv, priv, true);
        default:
                return -ENOTSUPP;
        }

        return 0;
}

static int xdma_get_ts_config(struct net_device *ndev, struct ifreq *ifr) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct hwtstamp_config *config = &priv->tstamp_config;

        return copy_to_user(ifr->ifr_data, config, sizeof(*config)) ? -EFAULT : 0;
}

static int xdma_set_ts_config(struct net_device *ndev, struct ifreq *ifr) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct hwtstamp_config *config = &priv->tstamp_config;

        return copy_from_user(config, ifr->ifr_data, sizeof(*config)) ? -EFAULT : 0;
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

static void do_tx_work(struct work_struct *work, u16 tstamp_id) {
        sysclock_t tx_tstamp;
        struct skb_shared_hwtstamps shhwtstamps;
        struct xdma_private* priv = container_of(work - tstamp_id, struct xdma_private, tx_work[0]);
        struct sk_buff* skb = priv->tx_work_skb[tstamp_id];
        sysclock_t now = alinx_get_sys_clock_by_xdev(priv->xdev);

        if (tstamp_id >= TSN_TIMESTAMP_ID_MAX) {
                pr_err("Invalid timestamp ID\n");
                return;
        }

        if (!priv->tx_work_skb[tstamp_id]) {
                goto return_error;
        }

        if (now < priv->tx_work_start_after[tstamp_id]) {
                goto retry;
        }
        /*
         * Read TX timestamp several times because
         * the work thread might try to read TX timestamp
         * before the register gets updated
         */
        tx_tstamp = alinx_read_tx_timestamp_by_xdev(priv->xdev, tstamp_id);
        if (tx_tstamp == priv->last_tx_tstamp[tstamp_id]) {
                if (alinx_get_sys_clock_by_xdev(priv->xdev) < priv->tx_work_wait_until[tstamp_id]) {
                        /* The packet might have not been sent yet */
                        goto retry;
                }
                /*
                 * Tx timestamp is not updated. Try again.
                 * Waiting for it to be updated forever is not desirable,
                 * so limit the number of retries
                 */
                if (++(priv->tstamp_retry[tstamp_id]) >= TX_TSTAMP_MAX_RETRY) {
                        /* TODO: track the number of skipped packets for ethtool stats */
                        pr_warn("Failed to get timestamp: timestamp is not getting updated, " \
                                "the packet might have been dropped\n");
                        goto return_error;
                }
                goto retry;
        }

        priv->tstamp_retry[tstamp_id] = 0;
        shhwtstamps.hwtstamp = ns_to_ktime(alinx_sysclock_to_txtstamp(priv->pdev, tx_tstamp));
        priv->last_tx_tstamp[tstamp_id] = tx_tstamp;

        priv->tx_work_skb[tstamp_id] = NULL;
        clear_bit_unlock(tstamp_id, &priv->state);
        skb_tstamp_tx(skb, &shhwtstamps);
    /* dev_kfree_skb_any -> napi_consume_skb */
    napi_consume_skb(skb, 0);
        return;

return_error:
        priv->tstamp_retry[tstamp_id] = 0;
        clear_bit_unlock(tstamp_id, &priv->state);
        return;

retry:
        schedule_work(&priv->tx_work[tstamp_id]);
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
