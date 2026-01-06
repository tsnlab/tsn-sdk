/* xdma_rx_ring.c */

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "libxdma.h"
#include "xdma_mod.h"    /* must provide struct xdma_desc / xdma_result */
#include "xdma_netdev.h" /* must provide rx_desc_set() or include where it lives */
#include "xdma_rx_ring.h"

static inline void canary_fill(u8* p, size_t n, u8 pat) {
    memset(p, pat, n);
}

static inline bool canary_check(const u8* p, size_t n, u8 pat) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != pat)
            return false;
    }
    return true;
}

void xdma_rx_canary_init_slot(struct xdma_rx_ring* rxr, u32 idx) {
    u8* base;
    if (!rxr || idx >= rxr->count)
        return;

    /* Payload guards */
    if (rxr->payload_base[idx]) {
        base = (u8*)rxr->payload_base[idx];
        canary_fill(base, rxr->guard_sz, XDMA_CANARY_PRE);
        canary_fill(base + rxr->guard_sz + rxr->payload_sz, rxr->guard_sz, XDMA_CANARY_POST);
    }

    /* Result guards */
    if (rxr->res_base[idx]) {
        base = (u8*)rxr->res_base[idx];
        canary_fill(base, rxr->guard_sz, XDMA_CANARY_PRE);
        canary_fill(base + rxr->guard_sz + rxr->res_sz, rxr->guard_sz, XDMA_CANARY_POST);
    }
}

bool xdma_rx_canary_check_payload(struct xdma_rx_ring* rxr, u32 idx, const char* tag) {
    u8* base;
    bool ok_pre, ok_post;

    if (!rxr || idx >= rxr->count || !rxr->payload_base[idx])
        return true;

    base = (u8*)rxr->payload_base[idx];

    /* Ensure DMA writes are visible before checking (coherent, but keep ordering explicit) */
    dma_rmb();

    ok_pre = canary_check(base, rxr->guard_sz, XDMA_CANARY_PRE);
    ok_post = canary_check(base + rxr->guard_sz + rxr->payload_sz, rxr->guard_sz, XDMA_CANARY_POST);

    if (unlikely(!ok_pre || !ok_post)) {
        rxr->canary_fail_payload++;
        pr_err("[RX_CANARY][PAYLOAD] %s: slot=%u pre=%d post=%d base=%p dma=%pad\n", tag ? tag : "?", idx, ok_pre,
               ok_post, rxr->payload_base[idx], &rxr->payload_base_dma[idx]);
        return false;
    }
    return true;
}

bool xdma_rx_canary_check_res(struct xdma_rx_ring* rxr, u32 idx, const char* tag) {
    u8* base;
    bool ok_pre, ok_post;

    if (!rxr || idx >= rxr->count || !rxr->res_base[idx])
        return true;

    base = (u8*)rxr->res_base[idx];

    dma_rmb();

    ok_pre = canary_check(base, rxr->guard_sz, XDMA_CANARY_PRE);
    ok_post = canary_check(base + rxr->guard_sz + rxr->res_sz, rxr->guard_sz, XDMA_CANARY_POST);

    if (unlikely(!ok_pre || !ok_post)) {
        rxr->canary_fail_res++;
        pr_err("[RX_CANARY][RES] %s: slot=%u pre=%d post=%d base=%p dma=%pad\n", tag ? tag : "?", idx, ok_pre, ok_post,
               rxr->res_base[idx], &rxr->res_base_dma[idx]);
        return false;
    }
    return true;
}

int xdma_rx_ring_alloc_init(struct xdma_rx_ring* rxr, struct device* dev) {
    int i;

    if (!rxr || !dev)
        return -EINVAL;

    memset(rxr, 0, sizeof(*rxr));
    rxr->dev = dev;
    rxr->count = XDMA_RX_DESC_RING_SIZE;

    rxr->payload_sz = XDMA_BUFFER_SIZE;
    rxr->guard_sz = XDMA_RX_GUARD_SZ;
    rxr->payload_alloc_sz = rxr->payload_sz + 2 * rxr->guard_sz;

    rxr->res_sz = sizeof(struct xdma_result);
    rxr->res_alloc_sz = rxr->res_sz + 2 * rxr->guard_sz;

    for (i = 0; i < rxr->count; i++) {
        /* Payload base alloc (with guards) */
        rxr->payload_base[i] = dma_alloc_coherent(dev, rxr->payload_alloc_sz, &rxr->payload_base_dma[i], GFP_KERNEL);
        if (!rxr->payload_base[i])
            goto err;

        rxr->payload[i] = (u8*)rxr->payload_base[i] + rxr->guard_sz;
        rxr->payload_dma[i] = rxr->payload_base_dma[i] + rxr->guard_sz;

        /* Result base alloc (with guards) */
        rxr->res_base[i] = dma_alloc_coherent(dev, rxr->res_alloc_sz, &rxr->res_base_dma[i], GFP_KERNEL);
        if (!rxr->res_base[i])
            goto err;

        rxr->res[i] = (struct xdma_result*)((u8*)rxr->res_base[i] + rxr->guard_sz);
        rxr->res_dma[i] = rxr->res_base_dma[i] + rxr->guard_sz;

        /* Descriptor alloc */
        rxr->desc[i] = dma_alloc_coherent(dev, sizeof(struct xdma_desc), &rxr->desc_bus[i], GFP_KERNEL);
        if (!rxr->desc[i])
            goto err;

        memset(rxr->desc[i], 0, sizeof(struct xdma_desc));
        memset(rxr->res[i], 0, sizeof(struct xdma_result));
        /* Optional: payload clear in debug */
        // memset(rxr->payload[i], 0, rxr->payload_sz);

        /* Init canaries for this slot */
        xdma_rx_canary_init_slot(rxr, i);

        /* Program descriptor fields (src = res, dst = payload) */
        rxr->desc[i]->src_addr_lo = cpu_to_le32(PCI_DMA_L(rxr->res_dma[i]));
        rxr->desc[i]->src_addr_hi = cpu_to_le32(PCI_DMA_H(rxr->res_dma[i]));

        rx_desc_set(rxr->desc[i], rxr->payload_dma[i], rxr->payload_sz);

        rxr->state[i] = DESC_READY;
    }

    rxr->pending = 0;
    rxr->prep = (rxr->count > 1) ? 1 : 0;
    rxr->has_pending = false;

    pr_info("xdma_rx_ring: initialized (%u slots), payload=%zu, guard=%zu, alloc_sz=%zu, res_sz=%zu\n", rxr->count,
            rxr->payload_sz, rxr->guard_sz, rxr->payload_alloc_sz, rxr->res_sz);
    return 0;

err:
    xdma_rx_ring_free(rxr);
    return -ENOMEM;
}

void xdma_rx_ring_free(struct xdma_rx_ring* rxr) {
    int i;

    if (!rxr || !rxr->dev)
        return;

    for (i = 0; i < rxr->count; i++) {
        if (rxr->desc[i]) {
            dma_free_coherent(rxr->dev, sizeof(struct xdma_desc), rxr->desc[i], rxr->desc_bus[i]);
            rxr->desc[i] = NULL;
            rxr->desc_bus[i] = (dma_addr_t)0;
        }

        if (rxr->res_base[i]) {
            dma_free_coherent(rxr->dev, rxr->res_alloc_sz, rxr->res_base[i], rxr->res_base_dma[i]);
            rxr->res_base[i] = NULL;
            rxr->res_base_dma[i] = (dma_addr_t)0;
            rxr->res[i] = NULL;
            rxr->res_dma[i] = (dma_addr_t)0;
        }

        if (rxr->payload_base[i]) {
            dma_free_coherent(rxr->dev, rxr->payload_alloc_sz, rxr->payload_base[i], rxr->payload_base_dma[i]);
            rxr->payload_base[i] = NULL;
            rxr->payload_base_dma[i] = (dma_addr_t)0;
            rxr->payload[i] = NULL;
            rxr->payload_dma[i] = (dma_addr_t)0;
        }

        rxr->state[i] = DESC_EMPTY;
    }

    rxr->pending = 0;
    rxr->prep = 0;
    rxr->has_pending = false;
}
