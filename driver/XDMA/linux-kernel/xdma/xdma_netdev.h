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

/*
 * HP 트랙 A1: TX 디스크립터 링 (프레임당 엔진-start 모델 대체, docs/11 §6-1)
 * - 링 슬롯 1개 = 프레임 1개 = xdma_desc 1개(EOP). 체인(연속 슬롯 구간)을
 *   next 포인터로 묶어 엔진에 1회 START로 제출, 마지막 desc만 STOPPED|COMPLETED
 *   -> 체인당 IRQ 1회. 완료 처리는 NAPI(xdma_tx_napi_poll)가 배치 회수.
 * - TX_RING_SIZE는 2의 거듭제곱. 인덱스는 free-running u32, 접근 시 & MASK.
 */
#define TX_RING_SIZE 256
#define TX_RING_MASK (TX_RING_SIZE - 1)
/* tx_bounce 실험(2026-08-29): coherent bounce buffer 슬롯 크기(max 프레임 + 여유) */
#define TX_BOUNCE_BUF_SIZE 2048
/* 링 여유가 이 값 이상으로 회복되면 netif queue를 다시 깨운다 */
#define TX_RING_WAKE_THRESH 32
#define TX_NAPI_WEIGHT 64

struct tx_ring_slot {
        struct sk_buff *skb;
        dma_addr_t dma;
        u16 len;
        s8 port_id;
};

/*
 * HP 트랙 A2: RX 디스크립터 링 (단일버퍼 + 프레임당 엔진 STOP/START 모델 대체)
 * - 슬롯 1개 = 프레임 1개 = xdma_desc 1개 + 고정 DMA 버퍼 + C2H writeback 결과 1개.
 * - 엔진은 체인([rx_head, rx_head+n)) 단위로 START 1회. 각 desc에 COMPLETED
 *   (프레임 도착마다 IRQ — NAPI가 채널 IRQ masking으로 자연 코얼레싱), 마지막
 *   desc만 STOPPED(링 추월 방지 백스톱). 체인 소진 시 NAPI가 수확분만큼 재제출.
 * - 프레임 완료 판정은 슬롯별 C2H ST writeback(result.status != 0) 기준.
 */
#define RX_RING_SIZE 256
#define RX_RING_MASK (RX_RING_SIZE - 1)
#define RX_NAPI_WEIGHT 64

#define CRC_LEN 4

#define TX_TSTAMP_POLL_US 10
#define TX_TSTAMP_MAX_RETRY 5000
#define TX_TSTAMP_TIMEOUT_MARGIN (RESERVED_CYCLE / 10)

enum xdma_state_t {
        XDMA_TX1_IN_PROGRESS = 1,
        XDMA_TX2_IN_PROGRESS = 2,
        XDMA_TX3_IN_PROGRESS = 3,
        XDMA_TX4_IN_PROGRESS = 4,
};

struct xdma_private_common {
        struct pci_dev *pdev;
        struct xdma_dev *xdev;

        struct xdma_engine *tx_engine;
        struct xdma_engine *rx_engine;

        /* A1 TX 디스크립터 링 (tx_lock 보호) */
        struct xdma_desc *tx_ring;              /* TX_RING_SIZE 연속 desc */
        dma_addr_t tx_ring_bus;
        u8 *tx_bufs;                            /* tx_bounce: TX_RING_SIZE * TX_BOUNCE_BUF_SIZE coherent */
        dma_addr_t tx_bufs_bus;
        struct tx_ring_slot tx_slots[TX_RING_SIZE];
        u32 tx_head;                            /* 다음 채울 슬롯 (producer) */
        u32 tx_tail;                            /* 다음 회수할 슬롯 (consumer) */
        u32 tx_run_len;                         /* 실행 중 체인 길이. 0=엔진 유휴 */
        struct napi_struct tx_napi;

        /* A2 RX 디스크립터 링 (인덱스/kick = rx_lock 보호, 슬롯 수확 = NAPI 단독) */
        struct xdma_desc *rx_ring;              /* RX_RING_SIZE 연속 desc */
        dma_addr_t rx_ring_bus;
        struct xdma_result *rx_res;             /* 슬롯별 C2H writeback 결과 */
        dma_addr_t rx_res_bus;
        u8 *rx_bufs;                            /* RX_RING_SIZE * XDMA_RX_BUFFER_SIZE 연속 */
        dma_addr_t rx_bufs_bus;
        u32 rx_head;                            /* 다음 HW 제출 슬롯 (free-running) */
        u32 rx_tail;                            /* 다음 수확 슬롯 (free-running) */
        u32 rx_run_len;                         /* 실행 중 체인 길이. 0=엔진 유휴 */
        struct napi_struct rx_napi;

        spinlock_t tx_lock;
        spinlock_t rx_lock;
        int irq;

        struct work_struct tx_work[TSN_TIMESTAMP_ID_MAX];
        struct sk_buff *tx_work_skb[TSN_TIMESTAMP_ID_MAX];
        sysclock_t tx_work_start_after[TSN_TIMESTAMP_ID_MAX];
        sysclock_t tx_work_wait_until[TSN_TIMESTAMP_ID_MAX];
        struct hwtstamp_config tstamp_config;
        sysclock_t last_tx_tstamp[TSN_TIMESTAMP_ID_MAX];

        struct delayed_work rx_poll_work;
        unsigned long last_switch_jiffies;

        bool is_running;

        int tx_port;
        int rx_port;

        uint64_t total_tx_count;
        uint64_t total_tx_drop_count;
        uint64_t last_normal_timeout;
        uint64_t last_to_overflow_popped;
        uint64_t last_to_overflow_timeout;

        int open_cnt;
        unsigned long state;

        atomic_t next_normal_tstamp_id;
};

struct xdma_private {
        struct xdma_private_common *common;
        struct net_device *ndev;
        int port_id;
        int physical_port_id; /* 0 or 1 */
        uint32_t port_flag;
        // uint64_t last_rx_timestamp;
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
    uint16_t frame_length;
    uint64_t timestamp;
} __attribute__((packed, scalar_storage_order("big-endian")));

struct rx_buffer {
    struct rx_metadata metadata;
    uint8_t data[0];
} __attribute__((packed, scalar_storage_order("big-endian")));

#define RX_METADATA_SIZE (sizeof(struct rx_metadata))
#define TX_METADATA_SIZE (sizeof(struct tx_metadata))

void tx_desc_set(struct xdma_desc *desc, dma_addr_t addr, u32 len);

/* A1 TX 링: 할당/해제(probe/remove), ISR->NAPI 스케줄 훅 */
int xdma_tx_ring_alloc(struct xdma_private_common *common);
void xdma_tx_ring_free(struct xdma_private_common *common);
void xdma_tx_completion_irq(struct xdma_private_common *common);
void xdma_tx_napi_setup(struct xdma_private_common *common, struct net_device *ndev);
void xdma_tx_napi_teardown(struct xdma_private_common *common);

/* A2 RX 링: 할당/해제(probe/remove), 시동/정지(open/close), ISR->NAPI 스케줄 훅 */
int xdma_rx_ring_alloc(struct xdma_private_common *common);
void xdma_rx_ring_free(struct xdma_private_common *common);
void xdma_rx_ring_start(struct xdma_private_common *common);
void xdma_rx_ring_stop(struct xdma_private_common *common);
void xdma_rx_completion_irq(struct xdma_private_common *common);
void xdma_rx_napi_setup(struct xdma_private_common *common, struct net_device *ndev);
void xdma_rx_napi_teardown(struct xdma_private_common *common);

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
int xdma_netdev_siocdevprivate(struct net_device *ndev, struct ifreq *ifr, void *data, int cmd);
u16 xdma_select_queue(struct net_device *ndev, struct sk_buff *skb, struct net_device *sb_dev);

/*
 * Track B 결함② : Tx timestamp 슬롯 4→32 확장.
 * 트램펄린(xdma_tx_work1..32)은 xdma_netdev.c에 static 정의, 아래 테이블로 노출.
 * INIT_WORK 시 index = tstamp_id (1..32)로 참조. [0]은 미사용.
 */
extern const work_func_t xdma_tx_work_fns[TSN_TIMESTAMP_ID_MAX];

#endif
