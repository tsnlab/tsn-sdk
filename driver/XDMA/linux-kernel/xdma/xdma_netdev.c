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

// #define TX_SAFE_GUARD

/* Static function declarations */
static int xdma_netdev_rx_poll(struct xdma_private* priv, int budget);
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
        control = le32_to_cpu(desc->control) & ~LS_BYTE_MASK;
        control |= control_field;
        desc->control = cpu_to_le32(control);

        desc->src_addr_lo = cpu_to_le32(PCI_DMA_L(addr));
        desc->src_addr_hi = cpu_to_le32(PCI_DMA_H(addr));
        desc->bytes = cpu_to_le32(len);

    /*
     * Ensure descriptor writes are visible before MMIO starts DMA.
     * Caller should still do dma_wmb() just before programming DESC_REG_LO/HI
     * and writing DMA_ENGINE_START, but placing a compiler barrier here
     * helps prevent reordering within the function.
     */
    barrier();
}

void rx_desc_set(struct xdma_desc *desc, dma_addr_t addr, u32 len)
{
        u32 control_field;
        u32 control;

        desc->control = cpu_to_le32(DESC_MAGIC);
        control_field = XDMA_DESC_STOPPED;
        control_field |= XDMA_DESC_EOP;
        control_field |= XDMA_DESC_COMPLETED;
        control = le32_to_cpu(desc->control) & ~LS_BYTE_MASK;
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

#if XDMA_CANARY_TEST
        /*
        * Guard check MUST happen before slot->skb is cleared.
        * If corrupted: this is hard evidence of DMA overwrite / length/address mismatch.
        */
        if (likely(slot->raw_vaddr))
        {
            int g = xdma_tx_guard_check(slot->raw_vaddr, XDMA_TX_BUF_SIZE);
            if (unlikely(g))
            {
                netdev_err(priv->ndev, "[TX-GUARD] CORRUPTED g=%d slot=%u slot_len=%u slot_dma=%pad pending=%d\n", g, tail,
                        slot->len, &slot->dma, READ_ONCE(priv->tx_desc_has_pending));

                netdev_err(priv->ndev, "[TX-GUARD] tx_ctl=0x%08x tx_st=0x%08x first_lo=0x%08x first_hi=0x%08x\n",
                        ioread32(&engine->regs->control), ioread32(&engine->regs->status),
                        ioread32(&engine->sgdma_regs->first_desc_lo), ioread32(&engine->sgdma_regs->first_desc_hi));
            }
        }
#endif

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

        WRITE_ONCE(priv->tx_desc_has_pending, false);

    /* ---- ping-pong prep index update ---- */
#if (XDMA_TX_DESC_RING_SIZE == 2)
        priv->tx_desc_prep = READ_ONCE(priv->tx_desc_pending) ^ 1U;
#else
        priv->tx_desc_prep = (READ_ONCE(priv->tx_desc_pending) + 1U) % XDMA_TX_DESC_RING_SIZE;
#endif

        /* single queue: wake queue now that pending is cleared */
        if (netif_queue_stopped(priv->ndev))
            netif_wake_queue(priv->ndev);
}

/* -------------------------------
 * TX Polling Handler
 * ------------------------------- */
int xdma_netdev_tx_poll(struct xdma_private* priv, int budget)
{
    struct xdma_engine* engine = &priv->xdev->engine_h2c[0];
    u32 status_tx;
    int work_done = 0;
    int tx_done = 0;

    spin_lock_bh(&priv->tx_lock);

    // pr_info("[TX_POLL] enter (budget=%d)\n", budget);
    if (budget <= 0) {
        spin_unlock_bh(&priv->tx_lock);
        return 0;
    }

    /* only act if we actually have an outstanding TX */
    if (unlikely(!READ_ONCE(priv->tx_desc_has_pending)))
    {
        pr_debug("TX-POLL: no pending\n");
        spin_unlock_bh(&priv->tx_lock);
        return 0;
    }

    /* 1. Read DMA engine status (cached from ISR) */
    status_tx = READ_ONCE(priv->last_tx_status);
    // status_tx = ioread32(&engine->regs->status);
    // pr_info("[TX_POLL] DMA status=0x%x\n", status_tx);

    /* 2. Check descriptor completion */
    tx_done = atomic_read(&priv->tx_done_cnt);
    // pr_info("[TX_POLL] tx_done = %d\n", tx_done);
    // if (status_tx & XDMA_STAT_DESC_COMPLETED) {
    if (tx_done > 0)
    {
        /* Clear completion + busy bits */
        // iowrite32(XDMA_STAT_DESC_COMPLETED | XDMA_STAT_BUSY, &engine->regs->status_rc);
        atomic_dec(&priv->tx_done_cnt);
        tx_done = atomic_read(&priv->tx_done_cnt);

        /* 3. TX Complete */
        xdma_netdev_tx_complete(priv);

        WRITE_ONCE(priv->last_tx_status, 0);

        // pr_info("[TX_POLL] TX complete (total=%llu, done=%u)\n", priv->total_tx_count, priv->tx_packets);

        work_done = 1;
        // pr_info("[TX_POLL] work_done = 1\n");
    }

    if (tx_done > 0)
    {
        pr_info("[TX_POLL] tx_done_cnt = %d\n", tx_done);
    }

    spin_unlock_bh(&priv->tx_lock);
    return work_done;
}

/*
 * Prepare one RX slot descriptor (SW-only).
 * - Do NOT touch HW registers here.
 * - Safe to call from process/softirq context.
 */
static void xdma_rx_prepare_slot(struct xdma_private* priv, u32 idx)
{
    struct xdma_rx_ring* rxr;

    if (unlikely(!priv))
        return;

    rxr = &priv->rx_ring;

    if (unlikely(idx >= XDMA_RX_DESC_RING_SIZE))
        return;

    if (unlikely(!rxr->desc[idx] || !rxr->payload[idx] || !rxr->res[idx])) {
        pr_err("[RX_PREP] slot %u resources NULL (desc=%p payload=%p res=%p)\n", idx, rxr->desc[idx], rxr->payload[idx],
               rxr->res[idx]);
        return;
    }

    /* Clear writeback/result so we never reuse stale length */
    memset(rxr->res[idx], 0, sizeof(struct xdma_result));

    /* Optional: payload clear for debug only (disable later) */
    // memset(rxr->payload[idx], 0, XDMA_BUFFER_SIZE);

    /* Program writeback/result address into descriptor */
    rxr->desc[idx]->src_addr_lo = cpu_to_le32(PCI_DMA_L(rxr->res_dma[idx]));
    rxr->desc[idx]->src_addr_hi = cpu_to_le32(PCI_DMA_H(rxr->res_dma[idx]));

    /* Program payload destination into descriptor */
    rx_desc_set(rxr->desc[idx], rxr->payload_dma[idx], XDMA_BUFFER_SIZE);

    rxr->state[idx] = DESC_READY;
}

/*
 * Arm/activate a prepared RX slot (HW access).
 * Must be called with rx_lock held.
 */
static void xdma_rx_arm_slot_locked(struct xdma_private* priv, u32 idx)
{
    struct xdma_engine* engine;
    struct xdma_rx_ring* rxr;
    u32 lo, hi;

    if (unlikely(!priv || !priv->xdev))
    {
        pr_err("[RX_ARM] invalid priv/xdev\n");
        return;
    }

    rxr = &priv->rx_ring;

    engine = priv->rx_engine ? priv->rx_engine : &priv->xdev->engine_c2h[0];

    if (unlikely(idx >= XDMA_RX_DESC_RING_SIZE))
    {
        pr_err("[RX_ARM] invalid idx=%u\n", idx);
        return;
    }

    if (unlikely(!rxr->desc[idx] || !rxr->desc_bus[idx]))
    {
        pr_err("[RX_ARM] slot %u desc not ready (desc=%p bus=%pad)\n", idx, rxr->desc[idx], &rxr->desc_bus[idx]);
        return;
    }

    /*
     * HW requirement: STOP -> START per transfer (as per current design).
     */
    iowrite32(DMA_ENGINE_STOP, &engine->regs->control);

    /* Clear completion bits before arming next transfer */
    iowrite32(XDMA_STAT_DESC_COMPLETED | XDMA_STAT_BUSY, &engine->regs->status_rc);

    /* Program first descriptor pointer (descriptor bus addr) */
    lo = cpu_to_le32(PCI_DMA_L(rxr->desc_bus[idx]));
    hi = cpu_to_le32(PCI_DMA_H(rxr->desc_bus[idx]));
    iowrite32(lo, &engine->sgdma_regs->first_desc_lo);
    iowrite32(hi, &engine->sgdma_regs->first_desc_hi);

    /*
     * Publish pending index BEFORE START.
     * This matches the outstanding=1 model (poll/ISR will consume pending).
     */
    WRITE_ONCE(rxr->pending, idx);
    WRITE_ONCE(rxr->has_pending, true);
    rxr->state[idx] = DESC_BUSY;

    /* Advance prep index for next arm */
    WRITE_ONCE(rxr->prep, xdma_rx_next_idx(idx));

    /*
     * Ensure descriptor/result writes are visible before START.
     * Coherent memory typically covers this, but keep it as a safety barrier.
     */
    dma_wmb();

    iowrite32(DMA_ENGINE_START, &engine->regs->control);
}

/*
 * RX restart after one completion:
 * - prepare next SW-owned slot (prep)
 * - STOP/first_desc/START with that slot
 * - advance prep index (handled inside xdma_rx_arm_slot_locked)
 */
static void xdma_restart_rx_engine(struct xdma_private* priv)
{
    struct xdma_rx_ring* rxr;
    u32 next_idx;
    unsigned long flags;

    if (unlikely(!priv))
        return;

    rxr = &priv->rx_ring;

    next_idx = READ_ONCE(rxr->prep);

    /*
     * Prepare outside lock to keep lock hold-time minimal.
     * (Even if called from NAPI, this helps at high throughput.)
     */
    xdma_rx_prepare_slot(priv, next_idx);

    spin_lock_irqsave(&priv->rx_lock, flags);

    /*
     * Arm the prepared slot.
     * NOTE: xdma_rx_arm_slot_locked() updates:
     *   - rxr->pending / rxr->has_pending
     *   - rxr->state[idx]
     *   - rxr->prep (advance)
     */
    xdma_rx_arm_slot_locked(priv, next_idx);

    spin_unlock_irqrestore(&priv->rx_lock, flags);
}

/* --------------------------------------------
 * RX packet processing (uses per-slot writeback)
 * -------------------------------------------- */
static int xdma_process_rx_packet(struct xdma_private* priv, u32 done_idx)
{
    struct net_device* ndev = priv->ndev;
    struct xdma_rx_ring* rxr = &priv->rx_ring;

    struct xdma_result* result = rxr->res[done_idx];
    u8* rx_buffer = rxr->payload[done_idx];

    u32 raw_len;
    int skb_len;
    struct sk_buff* skb;

    /* Basic pointer sanity */
    if (unlikely(!ndev || !result || !rx_buffer))
        return -EINVAL;

#if XDMA_CANARY_TEST
    /*
     * Canary check BEFORE using result/payload.
     * If this fails, treat it as DMA corruption evidence.
     */
    {
        bool ok_res = xdma_rx_canary_check_res(rxr, done_idx, "before_rx_copy");
        bool ok_pay = xdma_rx_canary_check_payload(rxr, done_idx, "before_rx_copy");

        if (unlikely(!ok_res || !ok_pay)) {
            /* Keep separate counters so we can tell res vs payload corruption */
            if (!ok_res)
                rxr->canary_fail_res++;
            if (!ok_pay)
                rxr->canary_fail_payload++;

            if (net_ratelimit()) {
                raw_len = le32_to_cpu(result->length);
                pr_err("[RX_PROC] Canary broken BEFORE copy: idx=%u ok_res=%d ok_pay=%d raw_len=%u (drop -EIO)\n",
                       done_idx, ok_res, ok_pay, raw_len);
            }

            return -EIO;
        }
    }
#endif

    /* Always interpret writeback length as little-endian */
    raw_len = le32_to_cpu(result->length);

    /* Validate reported DMA length */
    if (raw_len == 0 || raw_len > XDMA_BUFFER_SIZE) {
        pr_warn("[RX_PROC] Invalid result length: idx=%u raw_len=%u\n", done_idx, raw_len);
        return -EINVAL;
    }

    /* Compute skb_len from the reported DMA length */
    skb_len = (int)raw_len - RX_METADATA_SIZE - CRC_LEN;
    if (skb_len <= 0 || skb_len > ETH_FRAME_LEN) {
        pr_warn("[RX_PROC] Invalid skb_len: idx=%u skb_len=%d raw_len=%u\n", done_idx, skb_len, raw_len);
        return -EINVAL;
    }

    /* Allocate skb and copy payload (skip metadata header) */
    skb = dev_alloc_skb(skb_len);
    if (!skb)
        return -ENOMEM;

    memcpy(skb_put(skb, skb_len), rx_buffer + RX_METADATA_SIZE, skb_len);

    skb->dev = ndev;
    skb->protocol = eth_type_trans(skb, ndev);
    netif_receive_skb(skb);

#if XDMA_CANARY_TEST
    /*
     * Optional: Canary check AFTER copy.
     * Useful to detect concurrent overwrite while CPU was copying.
     * (We already delivered skb; this is diagnostics only.)
     */
    if (unlikely(!xdma_rx_canary_check_payload(rxr, done_idx, "after_rx_copy")))
    {
        rxr->canary_fail_payload_after++;

        if (net_ratelimit()) {
            pr_err("[RX_PROC] Canary broken AFTER copy: idx=%u (skb already delivered)\n", done_idx);
        }
    }
#endif

    return 0;
}

/* --------------------------------------------
 * NAPI RX poll
 * -------------------------------------------- */

static int xdma_netdev_rx_poll(struct xdma_private* priv, int budget) {
    int work_done = 0;
    struct xdma_rx_ring* rx_ring = &priv->rx_ring;

    while (work_done < budget) {
        u32 done_idx;
        int ret;

        if (READ_ONCE(priv->b_stop_rx_polling))
            break;

        if (!READ_ONCE(priv->rx_pending))
            break;

        /* Consume rx_pending (edge-trigger style) */
        WRITE_ONCE(priv->rx_pending, false);

        /* Outstanding=1 model: completed = last STARTed */
        if (unlikely(!READ_ONCE(rx_ring->has_pending))) {
            pr_warn("[RX_POLL] rx_pending but no pending descriptor\n");
            done_idx = 0;
        } else {
            done_idx = READ_ONCE(rx_ring->pending);
        }

        /* Process the completed slot */
        ret = xdma_process_rx_packet(priv, done_idx);
        if (ret)
            pr_debug("[RX_POLL] process failed: %d (idx=%u)\n", ret, done_idx);

        /* Mark done slot as no longer pending */
        WRITE_ONCE(rx_ring->has_pending, false);
        rx_ring->state[done_idx] = DESC_EMPTY;

        /* Arm next slot (should use rx_ring fields internally) */
        xdma_restart_rx_engine(priv);

        work_done++;
    }

    return work_done;
}

void waste_time(void) {
    u64 tt = ktime_get_ns();
    u64 nop_count = 0;
    while ((ktime_get_ns() - tt) < 10000) {
        nop_count++;
    }
    pr_info_ratelimited("[NAPI_POLL] waste_time: %u\n", nop_count);
}

#ifdef TX_TEST_WITHOUT_ISR
/*
 * This is to test TX ONLY without ISR
 * It reads and puts the napi schedule instead of isr_tx
 */
enum hrtimer_restart xdma_hrtimer_callback(struct hrtimer* timer) {
    struct xdma_private* priv = container_of(timer, struct xdma_private, tx_hrtimer);

    if (!READ_ONCE(priv->tx_desc_has_pending))
        return HRTIMER_NORESTART;

    pr_info_ratelimited("[HRTIMER_CB] xdma_hrtimer_callback\n");

    priv->last_tx_status = XDMA_STAT_DESC_COMPLETED;
    atomic_inc(&priv->tx_done_cnt);

    if (napi_schedule_prep(&priv->napi)) {
        __napi_schedule(&priv->napi);
        pr_info_ratelimited("[HRTIMER_CB] __napi_schedule\n");
    }

    return HRTIMER_NORESTART;
}

#endif

int xdma_napi_poll(struct napi_struct* napi, int budget) {
    struct xdma_private* priv = container_of(napi, struct xdma_private, napi);
    int work_done = 0;

    struct xdma_dev* xdev = priv->xdev;

    /* check network device validity */
    if (unlikely(!priv || !xdev)) {
        if (napi_complete_done(napi, 0)) {
            /* abnormal calls during initialization */
        }
        return 0;
    }

    u32 irq_mask = xdev->engine_c2h[0].irq_bitmask | xdev->engine_h2c[0].irq_bitmask;

    /* RX first */
    work_done = xdma_netdev_rx_poll(priv, budget);

    /* TX only if remaining budget exists */
    if (work_done < budget) {
        work_done += xdma_netdev_tx_poll(priv, budget - work_done);
    }

    /* Stop request (e.g., close/unload): complete + re-enable IRQ */
    if (READ_ONCE(priv->b_stop_rx_polling)) {
        pr_info("xdma_napi_poll b_stop_rx_polling\n");
        if (napi_complete_done(napi, work_done)) {
            channel_interrupts_enable(xdev, irq_mask);
        }
        WRITE_ONCE(priv->b_stopped_rx_polling, true);
        return work_done;
    }

    /* Normal completion */
    if (work_done < budget) {
        // pr_info("xdma_napi_poll work_done: %d, budget: %d -> NAPI complete and IRQ re-enable\n", work_done, budget);

        if (napi_complete_done(napi, work_done)) {
            // waste_time();
            pr_info_ratelimited("[NETDEV][NAPI_POLL] napi_poll complete_done\n");
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

/* Free TX descriptor ring (dma_alloc_coherent) and reset SW state. */
static void xdma_tx_desc_ring_cleanup(struct xdma_private* priv) {
    int i;

    for (i = 0; i < XDMA_TX_DESC_RING_SIZE; i++) {
        if (priv->tx_desc_ring[i]) {
            dma_free_coherent(&priv->pdev->dev, sizeof(struct xdma_desc), priv->tx_desc_ring[i], priv->tx_desc_bus[i]);
            priv->tx_desc_ring[i] = NULL;
            priv->tx_desc_bus[i] = (dma_addr_t)0;
        }
        priv->tx_desc_state[i] = 0; /* optional */
    }

    priv->tx_desc_prep = 0;
    priv->tx_desc_pending = 0;
    priv->tx_desc_has_pending = false;
}

/*
 * Allocate TX ping-pong descriptors (DMA-coherent) and init ownership state.
 * Must be called before starting TX queues (start_xmit can run immediately).
 */
static int xdma_tx_desc_ring_init(struct xdma_private* priv) {
    int i;

    /* If re-init happens, clean stale allocations first. */
    for (i = 0; i < XDMA_TX_DESC_RING_SIZE; i++) {
        if (priv->tx_desc_ring[i] || priv->tx_desc_bus[i]) {
            xdma_tx_desc_ring_cleanup(priv);
            break;
        }
    }

    for (i = 0; i < XDMA_TX_DESC_RING_SIZE; i++) {
        priv->tx_desc_ring[i] =
            dma_alloc_coherent(&priv->pdev->dev, sizeof(struct xdma_desc), &priv->tx_desc_bus[i], GFP_KERNEL);
        if (!priv->tx_desc_ring[i] || !priv->tx_desc_bus[i])
            goto err;

        memset(priv->tx_desc_ring[i], 0, sizeof(struct xdma_desc));
        priv->tx_desc_state[i] = 0; /* optional */

        // pr_info("TX-DESC: idx=%d desc=%p bus=%pad\n", i, priv->tx_desc_ring[i], &priv->tx_desc_bus[i]);
    }

    priv->tx_desc_prep = 0;
    priv->tx_desc_pending = 0;
    priv->tx_desc_has_pending = false;

    return 0;

err:
    pr_err("TX-DESC: dma_alloc_coherent failed idx=%d desc=%p bus=%pad\n", i, priv->tx_desc_ring[i],
           &priv->tx_desc_bus[i]);

    while (--i >= 0) {
        if (priv->tx_desc_ring[i]) {
            dma_free_coherent(&priv->pdev->dev, sizeof(struct xdma_desc), priv->tx_desc_ring[i], priv->tx_desc_bus[i]);
            priv->tx_desc_ring[i] = NULL;
            priv->tx_desc_bus[i] = (dma_addr_t)0;
            priv->tx_desc_state[i] = 0;
        }
    }
    priv->tx_desc_prep = 0;
    priv->tx_desc_pending = 0;
    priv->tx_desc_has_pending = false;
    return -ENOMEM;
}

#define DEF_TX_QUEUE_LEN (24)

inline void xdma_set_txqueue_length(struct net_device* ndev, u32 length) {
    ndev->tx_queue_len = length;
}

int xdma_netdev_open(struct net_device* ndev) {
    struct xdma_private* priv = netdev_priv(ndev);
    struct device* dev = &priv->pdev->dev;
    struct xdma_dev* xdev = priv->xdev;
    u32 lo, hi;
    unsigned long flags;
    dma_addr_t rx_bus_addr;

    if (!priv->ndev) {
        priv->ndev = ndev;
        pr_info("xdma_netdev_open(): linked xdev->ndev to %s\n", ndev->name);
    }

    /* set default tx queue length */
    xdma_set_txqueue_length(ndev, DEF_TX_QUEUE_LEN);

    /* Device ready + clear OFFLINE flag */
    xdma_device_flag_clear(priv->xdev, XDEV_FLAG_OFFLINE);
    netif_device_attach(ndev);

    /* Reset RX/TX software state */
    priv->b_stop_rx_polling = false;
    priv->b_stopped_rx_polling = false;

    priv->last_tx_status = 0;
    priv->last_rx_status = 0;

    priv->rx_pending = false;

    /* Reset TX ownership state too */
    priv->rx_ring.pending = 0;
    priv->rx_ring.prep = 0;
    priv->rx_ring.has_pending = false;
    priv->tx_last_time = ktime_get_ns();

    atomic_set(&priv->tx_done_cnt, 0);

    /* Enable NAPI */
    xdma_enable_napi(priv);
    pr_info("xdma_netdev_open(): NAPI initialized\n");

    /* --------------------------------------------------------
     * Initialize TX DMA Buffer Ring
     * -------------------------------------------------------- */
    if (xdma_tx_ring_init(&priv->tx_ring, dev)) {
        pr_err("xdma_netdev_open(): TX ring init failed\n");
        xdma_tx_ring_cleanup(&priv->tx_ring);
        return -ENOMEM;
    }
    pr_info("xdma_netdev_open(): TX DMA buffer ring initialized\n");

    /* --------------------------------------------------------
     * Initialize TX DMA Descriptor Ring
     * -------------------------------------------------------- */
    if (xdma_tx_desc_ring_init(priv)) {
        pr_err("xdma_netdev_open(): TX ring init failed\n");
        return -ENOMEM;
    }
    pr_info("xdma_netdev_open(): TX DMA Descriptor ring initialized\n");

    /* --------------------------------------------------------
     * Initialize RX ring resources (desc/payload/result per slot)
     * -------------------------------------------------------- */
    if (xdma_rx_ring_alloc_init(&priv->rx_ring, dev)) {
        pr_err("xdma_netdev_open(): RX ring init failed\n");
        xdma_tx_desc_ring_cleanup(priv);
        xdma_tx_ring_cleanup(&priv->tx_ring);
        return -ENOMEM;
    }
    pr_info("xdma_netdev_open(): RX ring initialized\n");

    /* --------------------------------------------------------
     * Arm RX with the initial pending descriptor (slot 0)
     * -------------------------------------------------------- */
    priv->rx_ring.pending = 0;
    priv->rx_ring.prep = (XDMA_RX_DESC_RING_SIZE > 1) ? 1 : 0;
    priv->rx_ring.has_pending = true;

    /*
     * IMPORTANT:
     * first_desc_lo/hi must point to the RX descriptor BUS address.
     */
    rx_bus_addr = priv->rx_ring.desc_bus[priv->rx_ring.pending];

    spin_lock_irqsave(&priv->rx_lock, flags);

    /* Clear latched status (if any) */
    ioread32(&priv->rx_engine->regs->status_rc);

    /* Program first descriptor address */
    lo = cpu_to_le32(PCI_DMA_L(rx_bus_addr));
    iowrite32(lo, &priv->rx_engine->sgdma_regs->first_desc_lo);

    hi = cpu_to_le32(PCI_DMA_H(rx_bus_addr));
    iowrite32(hi, &priv->rx_engine->sgdma_regs->first_desc_hi);

#ifdef TX_TEST_WITHOUT_ISR
    hrtimer_init(&priv->tx_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    priv->tx_hrtimer.function = xdma_hrtimer_callback;
#endif

    /* START RX engine */
#ifndef TX_ONLY_TEST
    iowrite32(DMA_ENGINE_START, &priv->rx_engine->regs->control);
#endif

    spin_unlock_irqrestore(&priv->rx_lock, flags);

    /* Bring link up and start queues */
    channel_interrupts_enable(xdev, ~0U);
    atomic_set(&xdev->isr_ready, 1);
    enable_irq(xdev->pdev->irq);

    netif_carrier_on(ndev);
    netif_start_queue(ndev);

    pr_info("xdma_netdev_open(): carrier ON + TX queues started\n");

    ndev->watchdog_timeo = msecs_to_jiffies(1000);
    return 0;
}

int xdma_netdev_close(struct net_device* ndev) {
    int i;
    struct xdma_private* priv = netdev_priv(ndev);
    struct xdma_dev* xdev = priv->xdev;

    /* 1) Stop flag set (prevent RX path from progressing) */
    WRITE_ONCE(priv->b_stop_rx_polling, true);

#ifdef TX_TEST_WITHOUT_ISR
    hrtimer_cancel(&priv->tx_hrtimer);
#endif

    /*
     * 2) Mask interrupts FIRST to prevent new ISR scheduling NAPI
     *    (This closes the window where ISR could schedule after napi_synchronize)
     */
    channel_interrupts_disable(xdev, 0xFFFFFFFF);
    user_interrupts_disable(xdev, 0xFFFFFFFF);

    /* 3) Drain in-flight ISR */
    if (priv->irq)
        synchronize_irq(priv->irq);

    /* 4) Drain in-flight NAPI (if scheduled already) */
    if (priv->napi_enabled)
        napi_synchronize(&priv->napi);

    /* 5) Stop DMA engines */
    iowrite32(DMA_ENGINE_STOP, &priv->rx_engine->regs->control);
    iowrite32(DMA_ENGINE_STOP, &priv->tx_engine->regs->control);

    /* 6) Disable NAPI */
    if (priv->napi_enabled) {
        napi_disable(&priv->napi);
        priv->napi_enabled = false;
        pr_info("NAPI disabled for xdma_netdev\n");
    }

    /* 7) Stop TX/RX queues */
    netif_stop_queue(ndev);
    for (i = 0; i < TX_QUEUE_COUNT; i++) {
        netif_stop_subqueue(ndev, i);
    }

    netif_carrier_off(ndev);
    pr_info("netif_carrier_off\n");

    /* 8) Cleanup rings */
    xdma_tx_ring_cleanup(&priv->tx_ring);
    pr_info("xdma_netdev_close(): TX DMA buffer ring cleaned up\n");

    xdma_tx_desc_ring_cleanup(priv);
    pr_info("xdma_netdev_close(): TX DMA descriptor ring cleaned up\n");

    xdma_rx_ring_free(&priv->rx_ring);
    pr_info("xdma_netdev_close(): RX ring cleaned up\n");

    return 0;
}

netdev_tx_t xdma_netdev_start_xmit(struct sk_buff* skb, struct net_device* ndev) {
    struct xdma_private* priv = netdev_priv(ndev);
    struct xdma_dev* xdev = priv->xdev;
    struct device* dma_dev = ndev->dev.parent;
    u32 w;
    sysclock_t sys_count, sys_count_upper, sys_count_lower;
    timestamp_t now;
    u16 frame_length;
    struct tx_buffer* tx_buffer;
    struct tx_metadata* tx_metadata;
    u32 to_value;
    u32 head, tail;
    u32 next_head;
    struct xdma_tx_ring* ring = &priv->tx_ring;
    struct xdma_tx_slot* slot;

    /* descriptor ping-pong */
    u32 desc_idx;
    struct xdma_desc* desc;
    dma_addr_t desc_bus;

    // pr_info("xdma_netdev_start_xmit\n");

    spin_lock_bh(&priv->tx_lock);
    if (unlikely(!priv || !xdev)) {
        pr_err("[XMIT] invalid priv/xdev (priv=%p xdev=%p)\n", priv, xdev);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_OK;
    }

    if (unlikely(!priv->tx_engine || !priv->tx_engine->regs || !priv->tx_engine->sgdma_regs)) {
        pr_err("[XMIT] tx_engine not ready (engine=%p regs=%p sgdma=%p)\n", priv->tx_engine,
               priv->tx_engine ? priv->tx_engine->regs : NULL, priv->tx_engine ? priv->tx_engine->sgdma_regs : NULL);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_OK;
    }

    /*
     * descriptor ping-pong selection
     */
    desc_idx = priv->tx_desc_prep;
    desc = priv->tx_desc_ring[desc_idx];
    desc_bus = priv->tx_desc_bus[desc_idx];

    if (unlikely(!desc || !desc_bus)) {
        pr_err("[XMIT] TX desc not ready idx=%u desc=%p bus=%pad\n", desc_idx, desc, &desc_bus);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_OK;
    }

    /* Check desc count */
    if (unlikely(READ_ONCE(priv->tx_desc_has_pending))) {
        pr_info("xdma_netdev_start_xmit tx_desc_has_pending\n");
        netif_stop_queue(ndev);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_BUSY;
    }

    // pr_info("xdma_netdev_start_xmit(skb->len : %d)\n", skb->len);

    head = ring->head;
    tail = ring->tail;
    next_head = (head + 1) % XDMA_TX_RING_SIZE;

    /* Ring full? Should not happen since we TX one packet at a time */
    if (unlikely(next_head == tail)) {
        pr_err("TX-XMIT: ring full (head=%u tail=%u)\n", head, tail);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_BUSY;
    }

    /* Use head slot (producer slot) */
    slot = &ring->slot[head];
    if (unlikely(slot->skb != NULL)) {
        pr_err("TX-ERROR: head slot[%u] is busy (skb=%p)\n", head, slot->skb);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_BUSY;
    }
    // pr_info("TX-XMIT START: head=%u tail=%u skb_len=%u desc_idx=%u desc_bus=%pad\n", head, tail, skb->len, desc_idx,
    //         &desc_bus);

    skb->len = max((unsigned int)ETH_ZLEN, skb->len);

    /* Store packet length */
    // frame_length = skb->len;
    // skb->len = workaround_packet_size(skb->len);

#ifdef TX_SAFE_GUARD
    /* Fix Alignment: if skb data is not aligned by 8-byte, create a copy */
    if (unlikely(((unsigned long)skb->data & 0x7) != 0)) {
        struct sk_buff* nskb = skb_copy(skb, GFP_ATOMIC);
        if (!nskb) {
            // drop old skb
            if (skb)
                dev_kfree_skb_any(skb);
            spin_unlock_bh(&priv->tx_lock);
            pr_err("TX-ERROR: failed to copy a new skb from a not-aligned skb\n");
            return NETDEV_TX_OK;
        }
        consume_skb(skb); // free original skb
        skb = nskb;       // use new aligned skb
    }
#endif

    u32 orig_len = skb->len;
    u32 target_len = max_t(u32, ETH_ZLEN, orig_len);
    u32 padded_len = workaround_packet_size(target_len);

    struct sk_buff* new_skb;

    if (skb_headroom(skb) < TX_METADATA_SIZE) {
        new_skb = skb_realloc_headroom(skb, TX_METADATA_SIZE);
        if (!new_skb) {
            dev_kfree_skb_any(skb);
            return NETDEV_TX_OK;
        }
        consume_skb(skb); // release prev skb
        skb = new_skb;    // replace new skb
    }

    if (skb->len < padded_len) {
        if (skb_padto(skb, padded_len)) {
            pr_err("skb_padto failed\n");
            netif_wake_queue(ndev);
            spin_unlock_bh(&priv->tx_lock);
            return NETDEV_TX_OK;
        }
    }
    frame_length = orig_len;

    /* Jumbo frames not supported */
    if ((skb->len + TX_METADATA_SIZE) > XDMA_BUFFER_SIZE) {
#ifdef __LIBXDMA_DEBUG__
        pr_err("Jumbo frames not supported\n");
#endif
        pr_err("Jumbo frames not supported\n");
        netif_wake_queue(ndev);
        dev_kfree_skb(skb);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_OK;
    }

    /* Add metadata to the skb */
    if (pskb_expand_head(skb, TX_METADATA_SIZE, 0, GFP_ATOMIC) != 0) {
#ifdef __LIBXDMA_DEBUG__
        pr_err("pskb_expand_head failed\n");
#endif
        pr_err("pskb_expand_head failed\n");
        netif_wake_queue(ndev);
        dev_kfree_skb(skb);
        spin_unlock_bh(&priv->tx_lock);
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
        pr_warn("tsn_fill_metadata failed\n");
        netif_wake_queue(ndev);
        spin_unlock_bh(&priv->tx_lock);
        return NETDEV_TX_BUSY;
    }

    xdma_debug("0x%08x  0x%08x  0x%08x  %4d  %1d", sys_count_lower, tx_metadata->from.tick, tx_metadata->to.tick,
               tx_metadata->frame_length, tx_metadata->fail_policy);
    dump_buffer((unsigned char*)tx_metadata, (int)(sizeof(struct tx_metadata) + skb->len));

    if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) {
        if (priv->tstamp_config.tx_type == HWTSTAMP_TX_ON &&
            !test_and_set_bit_lock(tx_metadata->timestamp_id, &priv->state)) {
            skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
            priv->tx_work_skb[tx_metadata->timestamp_id] = skb_get(skb);

            priv->tx_work_start_after[tx_metadata->timestamp_id] = sys_count_upper | tx_metadata->from.tick;
            if (sys_count_lower > tx_metadata->from.tick &&
                sys_count_lower - tx_metadata->from.tick > TX_WORK_OVERFLOW_MARGIN) {
                priv->tx_work_start_after[tx_metadata->timestamp_id] += (1 << 29);
            }

            to_value =
                (tx_metadata->fail_policy == TSN_FAIL_POLICY_RETRY ? tx_metadata->delay_to.tick : tx_metadata->to.tick);
            priv->tx_work_wait_until[tx_metadata->timestamp_id] = sys_count_upper | to_value;
            if (sys_count_lower > to_value && sys_count_lower - to_value > TX_WORK_OVERFLOW_MARGIN) {
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
/* ---- TX preflight checks before copying into DMA slot ---- */
#ifdef XDMA_CANARY_TEST
    /* 1) Detect prior corruption (evidence of previous DMA overrun) */
    {
        int g = xdma_tx_guard_check(slot->raw_vaddr, XDMA_TX_BUF_SIZE);
        if (unlikely(g)) {
            if (net_ratelimit()) {
                pr_err("[TX_XMIT] guard already broken BEFORE copy: slot=%u g=%d skb_len=%u\n", head, g, skb->len);
            }
            /* Optional: increment a counter in ring/slot */
            /* ring->guard_fail_pre++; */
            /* Hard fail is acceptable while debugging */
            spin_unlock_bh(&priv->tx_lock);
            dev_kfree_skb_any(skb);
            netif_wake_queue(ndev);
            return NETDEV_TX_OK;
        }

        /* 2) Re-fill guards for this packet (catch SW memcpy overrun immediately) */
        xdma_tx_guard_fill(slot->raw_vaddr, XDMA_TX_BUF_SIZE);
    }
#endif

    /* 3) Hard length guard against TX payload capacity */
    if (unlikely(skb->len > XDMA_TX_BUF_SIZE)) {
        if (net_ratelimit()) {
            pr_err("[TX_XMIT] skb->len exceeds TX payload: skb_len=%u payload=%u (drop)\n", skb->len,
                   (unsigned)XDMA_TX_BUF_SIZE);
        }
        spin_unlock_bh(&priv->tx_lock);
        dev_kfree_skb_any(skb);
        netif_wake_queue(ndev);
        return NETDEV_TX_OK;
    }

#ifdef TX_SAFE_GUARD
    if (skb_linearize(skb)) {
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }
#endif

    dma_sync_single_for_cpu(dma_dev, slot->dma, XDMA_TX_BUF_SIZE, DMA_TO_DEVICE);

#ifdef TX_SAFE_GUARD
    {
        if (unlikely(((unsigned long)skb->data & 0x7) != 0)) {
            struct sk_buff* nskb = skb_copy(skb, GFP_ATOMIC);
            if (!nskb) {
                // drop skb
                pr_warn("[XMIT-ALIGN] Misaligned skb->data\n");
                if (skb)
                    dev_kfree_skb_any(skb);
                spin_unlock_bh(&priv->tx_lock);
                return NETDEV_TX_OK;
            }
            consume_skb(skb);
            skb = nskb;
        }
    }
#endif

    memcpy(slot->vaddr, skb->data, skb->len);
    dma_sync_single_for_device(dma_dev, slot->dma, XDMA_TX_BUF_SIZE, DMA_TO_DEVICE);

#if XDMA_CANARY_TEST
    /* 4) Immediate post-copy guard check (detect SW overwrite past payload) */
    {
        int g = xdma_tx_guard_check(slot->raw_vaddr, XDMA_TX_BUF_SIZE);
        if (unlikely(g)) {
            if (net_ratelimit()) {
                pr_err("[TX_XMIT] guard broken AFTER copy: slot=%u g=%d skb_len=%u payload=%u\n", head, g, skb->len,
                       (unsigned)XDMA_TX_BUF_SIZE);
            }
            /* ring->guard_fail_post++; */
            spin_unlock_bh(&priv->tx_lock);
            dev_kfree_skb_any(skb);
            netif_wake_queue(ndev);
            return NETDEV_TX_OK;
        }
    }
#endif

    slot->skb = skb;
    slot->len = skb->len;

    /* Set descriptor to this slot's dma buffer */
    memset(desc, 0, sizeof(*desc));
    dma_wmb();

    tx_desc_set(desc, slot->dma, slot->len);

    /* Optional: dump descriptor bytes to verify STOP/EOP/len/src/dst/next fields */
    // print_hex_dump(KERN_INFO, "[XMIT] DESC: ", DUMP_PREFIX_OFFSET, 16, 4, desc, min_t(size_t, 64, sizeof(*desc)),
    //                false);

    /*
     * ensure descriptor memory writes are visible before MMIO start
     */
    dma_wmb();

    /* Read status/control before touching engine */
    // ctl_before = ioread32(&priv->tx_engine->regs->control);
    // st_before = ioread32(&priv->tx_engine->regs->status);
    // pr_info("[XMIT] before: tx_ctl=0x%08x tx_st=0x%08x\n", ctl_before, st_before);

    /*
     * If HW requires STOP->START per packet, enforce it here.
     * Also clear any latched status bits.
     */
    iowrite32(DMA_ENGINE_STOP, &priv->tx_engine->regs->control);
    iowrite32(0xFFFFFFFF, &priv->tx_engine->regs->status_rc);

    /*
     * Program first descriptor address via ENGINE SGDMA regs (recommended).
     * This should match the RX path style.
     */
    w = cpu_to_le32(PCI_DMA_L(desc_bus));
    iowrite32(w, &priv->tx_engine->sgdma_regs->first_desc_lo);

    w = cpu_to_le32(PCI_DMA_H(desc_bus));
    iowrite32(w, &priv->tx_engine->sgdma_regs->first_desc_hi);

    /* Flush posted writes (critical on ARM) */
    (void)ioread32(xdev->bar[1] + DESC_REG_HI);

    // pr_info("[XMIT] first_desc_lo=0x%08x hi=0x%08x\n", ioread32(&priv->tx_engine->sgdma_regs->first_desc_lo),
    //         ioread32(&priv->tx_engine->sgdma_regs->first_desc_hi));

    /* Start TX engine */
#ifndef TX_TEST_WITHOUT_ISR
    iowrite32(DMA_ENGINE_START, &priv->tx_engine->regs->control);
#endif

    /* Flush START too */
    (void)ioread32(&priv->tx_engine->regs->control);

    /* Mark pending AFTER start to avoid permanent block on failed start */
    WRITE_ONCE(priv->tx_desc_pending, desc_idx);
    WRITE_ONCE(priv->tx_desc_has_pending, true);

    netif_stop_queue(ndev);
    /* Advance HEAD pointer (producer moves) */
    ring->head = next_head;
    // pr_info("[TX-XMIT] xdma_netdev_start_xmit exit\n");
    spin_unlock_bh(&priv->tx_lock);

    pr_info_ratelimited("[XMIT] interval tx_last_time = %u ns\n", ktime_get_ns() - priv->tx_last_time);
    priv->tx_last_time = ktime_get_ns();
    netif_trans_update(ndev);

#ifdef TX_TEST_WITHOUT_ISR
    ktime_t kt = ktime_set(0, 10000); // 24 microseconds
    hrtimer_start(&priv->tx_hrtimer, kt, HRTIMER_MODE_REL);
#endif
    return NETDEV_TX_OK;
}

/* timeout handler */
void xdma_netdev_tx_timeout(struct net_device* ndev, unsigned int txq) 
{
    struct xdma_private* priv = netdev_priv(ndev);

    netif_tx_stop_all_queues(ndev);

    netdev_err(ndev, "TX timeout detected %u\n");

    // dump xdma all registers
    engine_status_read(priv->tx_engine, 0, 1);

    engine_status_read(priv->rx_engine, 0, 1);

    // dump device driver status
    // netdev_info(ndev, "SW Stats: Last XMIT jiffies: %lu\n", ndev->trans_start);

    // restore net device
    // schedule_work(&priv->reset_work);
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
