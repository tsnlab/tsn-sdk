#ifndef XDMA_NETDEV_H
#define XDMA_NETDEV_H

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/net_tstamp.h>

#include "xdma_mod.h"

#define DESC_REG_LO (SGDMA_OFFSET_FROM_CHANNEL + 0x80)
#define DESC_REG_HI (SGDMA_OFFSET_FROM_CHANNEL + 0x84)

#define DESC_REG_LO_RX (SGDMA_OFFSET_FROM_CHANNEL_RX + 0x80)
#define DESC_REG_HI_RX (SGDMA_OFFSET_FROM_CHANNEL_RX + 0x84)

#define DMA_ENGINE_START 16268831
#define DMA_ENGINE_STOP 16268830

#define DESC_EMPTY 0
#define DESC_READY 1
#define DESC_BUSY 2

#define CRC_LEN 4

#define TX_TSTAMP_MAX_RETRY 5

enum xdma_state_t {
        XDMA_TX1_IN_PROGRESS = 1,
        XDMA_TX2_IN_PROGRESS = 2,
        XDMA_TX3_IN_PROGRESS = 3,
        XDMA_TX4_IN_PROGRESS = 4,
};

struct xdma_private {
        struct pci_dev *pdev;
        struct net_device *ndev;
        struct xdma_dev *xdev;

        struct xdma_engine *tx_engine;
        struct xdma_engine *rx_engine;
        struct xdma_desc *rx_desc;
        struct xdma_desc *tx_desc;

        struct xdma_result *res;

        dma_addr_t tx_bus_addr;
        dma_addr_t tx_dma_addr;
        dma_addr_t rx_bus_addr;
        dma_addr_t rx_dma_addr;
        dma_addr_t res_bus_addr;
        dma_addr_t res_dma_addr;

        struct sk_buff *rx_skb;
        struct sk_buff *tx_skb;
        u8 *rx_buffer;
        spinlock_t tx_lock;
        spinlock_t rx_lock;
        int irq;
        int rx_count;

        struct work_struct tx_work[TSN_TIMESTAMP_ID_MAX];
        struct sk_buff *tx_work_skb[TSN_TIMESTAMP_ID_MAX];
        sysclock_t tx_work_start_after[TSN_TIMESTAMP_ID_MAX];
        sysclock_t tx_work_wait_until[TSN_TIMESTAMP_ID_MAX];
        struct hwtstamp_config tstamp_config;
        sysclock_t last_tx_tstamp[TSN_TIMESTAMP_ID_MAX];
        int tstamp_retry[TSN_TIMESTAMP_ID_MAX];

        uint64_t total_tx_count;
        uint64_t total_tx_drop_count;
        uint64_t last_normal_timeout;
        uint64_t last_to_overflow_popped;
        uint64_t last_to_overflow_timeout;

        unsigned long state;
};

#define _DEFAULT_FROM_MARGIN_ (500)
#define _DEFAULT_TO_MARGIN_ (50000)
struct tick_count {
        uint32_t tick:29;
        uint32_t priority:3;
} __attribute__((packed, scalar_storage_order("big-endian")));

struct tx_metadata {
        struct tick_count from;
        struct tick_count to;
        struct tick_count delay_from;
        struct tick_count delay_to;
        uint16_t frame_length;
        uint16_t timestamp_id;
        uint8_t fail_policy;
        uint8_t reserved0[3];
        uint32_t reserved1;
        uint32_t reserved2;
} __attribute__((packed, scalar_storage_order("big-endian")));

struct tx_buffer {
        struct tx_metadata metadata;
        uint8_t data[0];
} __attribute__((packed, scalar_storage_order("big-endian")));

struct rx_metadata {
    uint64_t timestamp;
    uint16_t frame_length;
} __attribute__((packed, scalar_storage_order("big-endian")));

struct rx_buffer {
    struct rx_metadata metadata;
    uint8_t data[0];
} __attribute__((packed, scalar_storage_order("big-endian")));

#define RX_METADATA_SIZE (sizeof(struct rx_metadata))
#define TX_METADATA_SIZE (sizeof(struct tx_metadata))

void rx_desc_set(struct xdma_desc *desc, dma_addr_t addr, u32 len);

/*
 * xdma_tx_handler - Transmit packet
 * @ndev: Pointer to the network device
 */
int xdma_tx_handler(struct net_device *ndev);

/*
 * xdma_rx_handler - Receive packet
 * @ndev: Pointer to the network device
 */
int xdma_rx_handler(struct net_device *ndev);

/*
 * xdma_netdev_open - Open the network device
 * @netdev: Pointer to the network device
 */
int xdma_netdev_open(struct net_device *netdev);

/*
 * xdma_netdev_close - Close the network device
 * @netdev: Pointer to the network device
 */
int xdma_netdev_close(struct net_device *netdev);

/*
 * xdma_netdev_start_xmit - Tx handler
 * If user transmits a packet, this function is called
 * @skb: Pointer to the socket buffer
 * @netdev: Pointer to the network device
 */
netdev_tx_t xdma_netdev_start_xmit(struct sk_buff *skb,
                                   struct net_device *netdev);

/*
 * xdma_netdev_setup_tc - TC config handler
 * @dev: Pointer to the network device
 * @type: Tc setup type
 * @type_data: parameters passed to the tc command
 */
int xdma_netdev_setup_tc(struct net_device *ndev, enum tc_setup_type type, void *type_data);

int xdma_netdev_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd);
u16 xdma_select_queue(struct net_device *ndev, struct sk_buff *skb, struct net_device *sb_dev);

void xdma_tx_work1(struct work_struct *work);
void xdma_tx_work2(struct work_struct *work);
void xdma_tx_work3(struct work_struct *work);
void xdma_tx_work4(struct work_struct *work);

#endif
