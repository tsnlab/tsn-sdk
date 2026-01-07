#ifndef ALINX_ARCH_H
#define ALINX_ARCH_H

#ifdef __linux__
#include <linux/types.h>
#elif defined __ZEPHYR__
typedef uint32_t u32
#else
#error unsupported os
#endif

#include <linux/pci.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/hashtable.h>
#include <net/pkt_sched.h>

#define REG_NEXT_PULSE_AT_HI 0x0018
#define REG_NEXT_PULSE_AT_LO 0x001c
#define REG_CYCLE_1S_HI 0x0020
#define REG_CYCLE_1S_LO 0x0024
#define REG_SYS_CLOCK_HI 0x0058
#define REG_SYS_CLOCK_LO 0x005c

#define REG_TX_TIMESTAMP1_HIGH 0x02d0
#define REG_TX_TIMESTAMP1_LOW 0x02d4
#define REG_TX_TIMESTAMP2_HIGH 0x02d8
#define REG_TX_TIMESTAMP2_LOW 0x02dc
#define REG_TX_TIMESTAMP3_HIGH 0x02e0
#define REG_TX_TIMESTAMP3_LOW 0x02e4
#define REG_TX_TIMESTAMP4_HIGH 0x02e8
#define REG_TX_TIMESTAMP4_LOW 0x02ec

#define REG_BUFFER_WRITE_STATUS1_HIGH 0x0258
#define REG_BUFFER_WRITE_STATUS1_LOW 0x025c

#define REG_TOTAL_NEW_ENTRY_CNT_HIGH 0x0228
#define REG_TOTAL_NEW_ENTRY_CNT_LOW 0x022c

#define REG_TOTAL_VALID_ENTRY_CNT_HIGH 0x0230
#define REG_TOTAL_VALID_ENTRY_CNT_LOW 0x0234

#define REG_TOTAL_DROP_ENTRY_CNT_HIGH 0x0238
#define REG_TOTAL_DROP_ENTRY_CNT_LOW 0x023c

#define REG_TSN_SYSTEM_CONTROL_HIGH 0x0000
#define REG_TSN_SYSTEM_CONTROL_LOW 0x0004

#define REG_FBW_ADDR_FIFO_CNT_HIGH 0x0270
#define REG_FBW_ADDR_FIFO_CNT_LOW 0x0274

#define REG_ETH0_RX_FIFO_STATUS_HIGH 0x00f0
#define REG_ETH0_RX_FIFO_STATUS_LOW 0x00f4

#define REG_ETH1_RX_FIFO_STATUS_HIGH 0x0188
#define REG_ETH1_RX_FIFO_STATUS_LOW 0x018c

#define FIFO_DATA_CNT_MASK 0x00ff

#define TSN_ENABLE 0x1
/* Bit 1: RX port select (0=port A, 1=port B) */
/* Bit 2: TX port select (0=port A, 1=port B) */
#define TSN_TX_PORT0 0b000
#define TSN_TX_PORT1 0b100
#define TSN_RX_PORT0 0b00
#define TSN_RX_PORT1 0b10

#define TSN_TX_PORT(n) (TSN_TX_PORT##n)
#define TSN_RX_PORT(n) (TSN_RX_PORT##n)

#define RX_POLL_WORK_INTERVAL_US (100)

#define TX_QUEUE_COUNT 8
#define RX_QUEUE_COUNT 8

/* 125 MHz */
#define TICKS_SCALE 8.0
#define RESERVED_CYCLE 125000000

#define HW_QUEUE_SIZE_PAD 20
#define HW_QUEUE_SIZE 128

#define BE_QUEUE_SIZE_PAD 20
#define BE_QUEUE_SIZE (HW_QUEUE_SIZE - BE_QUEUE_SIZE_PAD)

#define TSN_QUEUE_SIZE_PAD 2
#define TSN_QUEUE_SIZE (HW_QUEUE_SIZE - TSN_QUEUE_SIZE_PAD)

#define TC_COUNT 8
#define TSN_PRIO_COUNT 8
#define MAX_QBV_SLOTS 20

/* FRER (802.1CB) configuration limits */
#define MAX_FRER_STREAMS 64
#define FRER_HASH_BITS 6

#define ETHERNET_GAP_SIZE (8 + 4 + 12) // 8 bytes preamble, 4 bytes FCS, 12 bytes interpacket gap
#define PHY_DELAY_CLOCKS 13 // 14 clocks from MAC to PHY, but sometimes there is 1 tick error

#define TX_ADJUST_NS (100 + 200)  // MAC + PHY
#define RX_ADJUST_NS (188 + 324)  // MAC + PHY

#define H2C_LATENCY_NS 30000 // TODO: Adjust this value dynamically

typedef u64 sysclock_t;
typedef u64 timestamp_t;

struct ptp_device_data {
        struct device *dev;
        struct ptp_clock *ptp_clock;
        struct ptp_clock_info ptp_info;
        struct xdma_dev *xdev;
        double ticks_scale;
        u64 offset;
        spinlock_t lock;
#ifdef __LIBXDMA_DEBUG__
        u32 ptp_id;
#endif
};

struct qbv_slot {
	uint32_t duration_ns; // We don't support cycle > 1s
	bool opened_prios[TC_COUNT];
};

struct qbv_config {
	bool enabled;
	timestamp_t start;
	struct qbv_slot slots[MAX_QBV_SLOTS];

	uint32_t slot_count;
};

struct qbv_baked_prio_slot {
	uint64_t duration_ns;
	bool opened;
};

struct qbv_baked_prio {
	struct qbv_baked_prio_slot slots[MAX_QBV_SLOTS];
	size_t slot_count;
};

struct qbv_baked_config {
	uint64_t cycle_ns;
	struct qbv_baked_prio prios[TC_COUNT];
};

struct qav_state {
	bool enabled;
	int32_t idle_slope; // credits/ns
	int32_t send_slope; // credits/ns
	int32_t hi_credit;
	int32_t lo_credit;

	int32_t credit;
	timestamp_t last_update;
	timestamp_t available_at;
};

/* FRER (802.1CB) configuration - forward declaration */
struct frer_config;

struct tsn_config {
	struct qbv_config qbv;
	struct qbv_baked_config qbv_baked;
	struct qav_state qav[TC_COUNT];
	uint32_t buffer_space;
	timestamp_t queue_available_at[TSN_PRIO_COUNT];
	timestamp_t total_available_at;

	/* FRER (802.1CB) configuration */
	struct frer_config *frer;
};

u32 read32(void * addr);
void write32(u32 val, void * addr);
u64 read64(void *addr_high, void *addr_low);

void alinx_set_pulse_at_by_xdev(struct xdma_dev *xdev, sysclock_t time);
void alinx_set_pulse_at(struct pci_dev *pdev, sysclock_t time);
sysclock_t alinx_get_sys_clock_by_xdev(struct xdma_dev *pdev);
sysclock_t alinx_get_sys_clock(struct pci_dev *pdev);
void alinx_set_cycle_1s_by_xdev(struct xdma_dev *xdev, u32 cycle_1s);
void alinx_set_cycle_1s(struct pci_dev *pdev, u32 cycle_1s);
u32 alinx_get_cycle_1s_by_xdev(struct xdma_dev *xdev);
u32 alinx_get_cycle_1s(struct pci_dev *pdev);
timestamp_t alinx_read_tx_timestamp_by_xdev(struct xdma_dev *xdev, int tx_id);
timestamp_t alinx_read_tx_timestamp(struct pci_dev *pdev, int tx_id);
u64 alinx_get_buffer_write_status_by_xdev(struct xdma_dev *xdev);
u64 alinx_get_buffer_write_status(struct pci_dev *pdev);
u64 alinx_get_total_new_entry_by_xdev(struct xdma_dev *xdev);
u64 alinx_get_total_valid_entry_by_xdev(struct xdma_dev *xdev);
u64 alinx_get_total_drop_entry_by_xdev(struct xdma_dev *xdev);
u64 alinx_get_fifo_cnt_by_xdev(struct xdma_dev *xdev);
u64 alinx_get_rx_fifo_status_by_xdev(struct xdma_dev *xdev, int port_id);

void dump_buffer(unsigned char* buffer, int len);

#endif  /* ALINX_ARCH_H */
