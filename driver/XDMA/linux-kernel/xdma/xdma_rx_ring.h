/* xdma_rx_ring.h */

#pragma once

#ifndef XDMA_CANARY_TEST
#define XDMA_CANARY_TEST 0
#endif

#include <linux/dma-mapping.h>
#include <linux/types.h>

#ifndef XDMA_RX_DESC_RING_SIZE
#define XDMA_RX_DESC_RING_SIZE 2
#endif

/* Canary/guard configuration */
#ifndef XDMA_RX_GUARD_SZ
#define XDMA_RX_GUARD_SZ 64
#endif

#define XDMA_CANARY_PRE 0xA5
#define XDMA_CANARY_POST 0x5A

struct xdma_desc;
struct xdma_result;

enum {
    DESC_EMPTY = 0,
    DESC_READY,
    DESC_BUSY,
};

/*
 * RX ring container.
 * - payload_base/res_base are the actual allocated buffers (include guards)
 * - payload/res point to the usable region (after head guard)
 */
struct xdma_rx_ring {
    struct device* dev;

    u32 count;   /* XDMA_RX_DESC_RING_SIZE */
    u32 pending; /* last STARTed slot */
    u32 prep;    /* next slot to arm */
    bool has_pending;

    size_t payload_sz;       /* usable payload size (e.g., XDMA_BUFFER_SIZE) */
    size_t guard_sz;         /* XDMA_RX_GUARD_SZ */
    size_t payload_alloc_sz; /* payload_sz + 2*guard_sz */

    size_t res_sz;       /* sizeof(struct xdma_result) */
    size_t res_alloc_sz; /* res_sz + 2*guard_sz */

    /* Descriptor */
    struct xdma_desc* desc[XDMA_RX_DESC_RING_SIZE];
    dma_addr_t desc_bus[XDMA_RX_DESC_RING_SIZE];

    /* Payload (dst_addr) */
    void* payload_base[XDMA_RX_DESC_RING_SIZE];
    dma_addr_t payload_base_dma[XDMA_RX_DESC_RING_SIZE];
    u8* payload[XDMA_RX_DESC_RING_SIZE];
    dma_addr_t payload_dma[XDMA_RX_DESC_RING_SIZE];

    /* Result/writeback (src_addr) */
    void* res_base[XDMA_RX_DESC_RING_SIZE];
    dma_addr_t res_base_dma[XDMA_RX_DESC_RING_SIZE];
    struct xdma_result* res[XDMA_RX_DESC_RING_SIZE];
    dma_addr_t res_dma[XDMA_RX_DESC_RING_SIZE];

    u8 state[XDMA_RX_DESC_RING_SIZE];

    /* Debug counters */
    u64 canary_fail_payload;
    u64 canary_fail_payload_after;
    u64 canary_fail_res;

    u64 canary_fail_cnt;
    u64 canary_fail_after_cnt;
};

int xdma_rx_ring_alloc_init(struct xdma_rx_ring* rxr, struct device* dev);
void xdma_rx_ring_free(struct xdma_rx_ring* rxr);

/* Canary helpers */
void xdma_rx_canary_init_slot(struct xdma_rx_ring* rxr, u32 idx);
bool xdma_rx_canary_check_payload(struct xdma_rx_ring* rxr, u32 idx, const char* tag);
bool xdma_rx_canary_check_res(struct xdma_rx_ring* rxr, u32 idx, const char* tag);

/* Index helper */
static inline u32 xdma_rx_next_idx(u32 idx) {
#if (XDMA_RX_DESC_RING_SIZE == 2)
    return idx ^ 1U;
#else
    return (idx + 1U) % XDMA_RX_DESC_RING_SIZE;
#endif
}
