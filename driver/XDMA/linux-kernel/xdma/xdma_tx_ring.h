#ifndef _XDMA_TX_RING_H_
#define _XDMA_TX_RING_H_

#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/*
 * Software TX ring for XDMA.
 *
 * Even though the hardware uses a single DMA descriptor at a time,
 * a multi-slot software ring allows:
 *   - safe staging of packets before DMA submission
 *   - eliminating dma_map_single()/dma_unmap_single() race conditions
 *   - complete isolation between SKB memory and DMA memory
 *   - fully reusable DMA buffers allocated once at device open
 *
 * Recommended ring size: 4–8.
 */
#define XDMA_TX_RING_SIZE 8
#define XDMA_TX_BUF_SIZE 4096 /* Enough for metadata + max Ethernet payload */

/*
 * A single TX slot in the ring.
 *
 * vaddr : CPU-accessible coherent buffer used as copy target
 * dma   : DMA engine physical address for this slot
 * skb   : original SKB, freed at TX completion
 * len   : actual transmitted length copied into vaddr
 */
struct xdma_tx_slot {
    void* vaddr;
    dma_addr_t dma;
    struct sk_buff* skb;
    u32 len;
};

/*
 * TX ring structure.
 *
 * head : next slot to fill (CPU producer)
 * tail : next slot to complete/free (DMA consumer)
 *
 * head == tail → ring is empty
 * (head + 1) % size == tail → ring is full
 *
 * All slots are persistent DMA-coherent buffers allocated at open()
 * and reused until close().
 */
struct xdma_tx_ring {
    struct device* dev;

    struct xdma_tx_slot slot[XDMA_TX_RING_SIZE];

    u32 head;
    u32 tail;

    spinlock_t lock; /* Protects head/tail updates */
};

/* Allocate all TX slots and DMA-coherent buffers */
int xdma_tx_ring_init(struct xdma_tx_ring* ring, struct device* dev);

/* Release all DMA-coherent buffers */
void xdma_tx_ring_cleanup(struct xdma_tx_ring* ring);

/* Ring full check */
static inline bool xdma_tx_ring_full(struct xdma_tx_ring* ring) {
    return ((ring->head + 1) % XDMA_TX_RING_SIZE) == ring->tail;
}

/* Ring empty check */
static inline bool xdma_tx_ring_empty(struct xdma_tx_ring* ring) {
    return ring->head == ring->tail;
}

/* Slot where CPU writes the next packet */
static inline struct xdma_tx_slot* xdma_tx_ring_head_slot(struct xdma_tx_ring* ring) {
    return &ring->slot[ring->head];
}

/* Slot where DMA just completed a packet */
static inline struct xdma_tx_slot* xdma_tx_ring_tail_slot(struct xdma_tx_ring* ring) {
    return &ring->slot[ring->tail];
}

/* Advance head pointer after filling a slot */
static inline void xdma_tx_ring_push(struct xdma_tx_ring* ring) {
    ring->head = (ring->head + 1) % XDMA_TX_RING_SIZE;
}

/* Advance tail after processing TX completion */
static inline void xdma_tx_ring_pop(struct xdma_tx_ring* ring) {
    ring->tail = (ring->tail + 1) % XDMA_TX_RING_SIZE;
}

#endif /* _XDMA_TX_RING_H_ */
