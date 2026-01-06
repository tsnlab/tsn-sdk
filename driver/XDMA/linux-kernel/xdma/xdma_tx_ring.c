#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "xdma_tx_ring.h"

/* ---- TX canary/guard test ---- */
#define XDMA_TX_GUARD_SZ 64 /* 32~128 ??, ?? 64 */
#define XDMA_TX_GUARD_WORD 0xA5A5A5A5u

inline void xdma_tx_guard_fill(void* raw, size_t payload_sz) {
    u32* head = (u32*)raw;
    u32* tail = (u32*)((u8*)raw + XDMA_TX_GUARD_SZ + payload_sz);
    size_t i;

    for (i = 0; i < XDMA_TX_GUARD_SZ / sizeof(u32); i++) {
        head[i] = XDMA_TX_GUARD_WORD;
        tail[i] = XDMA_TX_GUARD_WORD;
    }
}

/* 0=ok, 1=head corrupt, 2=tail corrupt */
inline int xdma_tx_guard_check(void* raw, size_t payload_sz) {
    u32* head = (u32*)raw;
    u32* tail = (u32*)((u8*)raw + XDMA_TX_GUARD_SZ + payload_sz);
    size_t i;

    for (i = 0; i < XDMA_TX_GUARD_SZ / sizeof(u32); i++) {
        if (head[i] != XDMA_TX_GUARD_WORD)
            return 1;
        if (tail[i] != XDMA_TX_GUARD_WORD)
            return 2;
    }
    return 0;
}

/*
 * Initialize the TX ring:
 *  - Zero all fields
 *  - Allocate DMA-coherent buffers for each slot
 *  - Initialize spinlock
 *
 * This function must be called at device open().
 */
int xdma_tx_ring_init(struct xdma_tx_ring* ring, struct device* dev) {
    int i;
    size_t alloc_sz;

    if (!ring || !dev)
        return -EINVAL;

    memset(ring, 0, sizeof(*ring));
    ring->dev = dev;
    spin_lock_init(&ring->lock);

    /* payload(XDMA_TX_BUF_SIZE) + head/tail guard */
#if XDMA_TX_CANARY_TEST
    alloc_sz = XDMA_TX_BUF_SIZE + (2 * XDMA_TX_GUARD_SZ);
#else
    alloc_sz = XDMA_TX_BUF_SIZE;
#endif

    for (i = 0; i < XDMA_TX_RING_SIZE; i++) {

        /*
         * Each slot receives a dedicated DMA-coherent buffer.
         * Layout:
         *   [ GUARD | PAYLOAD(XDMA_TX_BUF_SIZE) | GUARD ]
         *
         * slot->vaddr/slot->dma must point to PAYLOAD start so existing TX path
         * (memcpy(slot->vaddr, ...), tx_desc_set(desc, slot->dma, ...)) stays unchanged.
         */
        ring->slot[i].raw_vaddr = dma_alloc_coherent(dev, alloc_sz, &ring->slot[i].raw_dma, GFP_KERNEL);
        if (!ring->slot[i].raw_vaddr) {
            pr_err("xdma_tx_ring: dma_alloc_coherent failed for slot %d (alloc_sz=%zu)\n", i, alloc_sz);
            goto fail;
        }

        ring->slot[i].vaddr = (u8*)ring->slot[i].raw_vaddr + XDMA_TX_GUARD_SZ;
        ring->slot[i].dma = ring->slot[i].raw_dma + XDMA_TX_GUARD_SZ;

#if XDMA_TX_CANARY_TEST
        /* Initialize guards once (per-slot). We will typically re-fill per packet in xmit. */
        xdma_tx_guard_fill(ring->slot[i].raw_vaddr, XDMA_TX_BUF_SIZE);
#endif

        ring->slot[i].len = 0;
        ring->slot[i].skb = NULL;
    }

    ring->head = 0;
    ring->tail = 0;

    pr_info("xdma_tx_ring: initialized (%d slots), payload=%u, guard=%u, alloc_sz=%zu\n", XDMA_TX_RING_SIZE,
            (unsigned)XDMA_TX_BUF_SIZE, (unsigned)XDMA_TX_GUARD_SZ, alloc_sz);
    return 0;

fail:
    /*
     * Free all slots that were successfully allocated before failure.
     */
    while (--i >= 0) {
        if (ring->slot[i].raw_vaddr) {
            dma_free_coherent(dev, alloc_sz, ring->slot[i].raw_vaddr, ring->slot[i].raw_dma);
            ring->slot[i].raw_vaddr = NULL;
            ring->slot[i].raw_dma = (dma_addr_t)0;
        }
        ring->slot[i].vaddr = NULL;
        ring->slot[i].dma = (dma_addr_t)0;
        ring->slot[i].len = 0;
        ring->slot[i].skb = NULL;
    }
    return -ENOMEM;
}

/*
 * Cleanup the TX ring:
 *  - Release all DMA-coherent buffers
 *  - Reset internal pointers
 *
 * This function must be called at device close().
 */
void xdma_tx_ring_cleanup(struct xdma_tx_ring* ring) {
    int i;
    size_t alloc_sz;

    if (!ring || !ring->dev)
        return;

    /* Must match allocation size in xdma_tx_ring_init() */
#if XDMA_TX_CANARY_TEST
    alloc_sz = XDMA_TX_BUF_SIZE + (2 * XDMA_TX_GUARD_SZ);
#else
    alloc_sz = XDMA_TX_BUF_SIZE;
#endif

    for (i = 0; i < XDMA_TX_RING_SIZE; i++) {

        /*
         * Free the ORIGINAL base returned by dma_alloc_coherent().
         * Do NOT free slot[i].vaddr/slot[i].dma (they are payload offsets).
         */
        if (ring->slot[i].raw_vaddr) {
            dma_free_coherent(ring->dev, alloc_sz, ring->slot[i].raw_vaddr, ring->slot[i].raw_dma);

            ring->slot[i].raw_vaddr = NULL;
            ring->slot[i].raw_dma = (dma_addr_t)0;
        }

            ring->slot[i].vaddr = NULL;
        ring->slot[i].dma = (dma_addr_t)0;
            ring->slot[i].len = 0;

        /* Defensive: if any skb is still referenced, drop it safely */
        if (ring->slot[i].skb) {
            /* In close(), NAPI/IRQ should already be drained, so this is a safety net */
            dev_kfree_skb_any(ring->slot[i].skb);
            ring->slot[i].skb = NULL;
        }
    }

    ring->head = 0;
    ring->tail = 0;
    ring->dev = NULL;

    pr_info("xdma_tx_ring: cleaned up\n");
}
