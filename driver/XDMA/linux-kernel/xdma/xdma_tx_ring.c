#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "xdma_tx_ring.h"

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

    if (!ring || !dev)
        return -EINVAL;

    memset(ring, 0, sizeof(*ring));
    ring->dev = dev;
    spin_lock_init(&ring->lock);

    for (i = 0; i < XDMA_TX_RING_SIZE; i++) {

        /*
         * Each slot receives a dedicated DMA-coherent buffer.
         * The buffer is reused for every transmitted packet.
         */
        ring->slot[i].vaddr = dma_alloc_coherent(dev, XDMA_TX_BUF_SIZE, &ring->slot[i].dma, GFP_KERNEL);

        if (!ring->slot[i].vaddr) {
            pr_err("xdma_tx_ring: dma_alloc_coherent failed for slot %d\n", i);
            goto fail;
        }

        ring->slot[i].len = 0;
        ring->slot[i].skb = NULL;
    }

    ring->head = 0;
    ring->tail = 0;

    pr_info("xdma_tx_ring: initialized (%d slots)\n", XDMA_TX_RING_SIZE);
    return 0;

fail:
    /*
     * Free all slots that were successfully allocated before failure.
     */
    while (--i >= 0) {
        if (ring->slot[i].vaddr) {
            dma_free_coherent(dev, XDMA_TX_BUF_SIZE, ring->slot[i].vaddr, ring->slot[i].dma);
        }
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

    if (!ring || !ring->dev)
        return;

    for (i = 0; i < XDMA_TX_RING_SIZE; i++) {

        if (ring->slot[i].vaddr) {
            dma_free_coherent(ring->dev, XDMA_TX_BUF_SIZE, ring->slot[i].vaddr, ring->slot[i].dma);

            ring->slot[i].vaddr = NULL;
            ring->slot[i].dma = 0;
            ring->slot[i].len = 0;
            ring->slot[i].skb = NULL;
        }
    }

    ring->head = 0;
    ring->tail = 0;
    ring->dev = NULL;

    pr_info("xdma_tx_ring: cleaned up\n");
}
