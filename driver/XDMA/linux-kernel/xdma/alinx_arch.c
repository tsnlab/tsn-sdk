#include "alinx_arch.h"
#include "libxdma.h"
#include "xdma_netdev.h"

#ifdef __linux__

#include <linux/io.h>

u32 read32(void * addr) {
        return ioread32(addr);
}

void write32(u32 val, void * addr) {
        iowrite32(val, addr);
}

u64 read64(void *addr_high, void *addr_low) {
        u32 high = ioread32(addr_high);
        u32 low = ioread32(addr_low);
        return ((u64)high << 32) | (u64)low;
}

#elif defined __ZEPHYR__

#include <zephyr/sys/sys_io.h>

u32 read32(void * addr) {
        return sys_read32((mem_addr_t)addr);
}

void write32(u32 val, void * addr) {
        sys_write32(val, (mem_addr_t)addr);
}

u64 read64(void *addr_high, void *addr_low) {
        u32 high = sys_read32((mem_addr_t)addr_high);
        u32 low = sys_read32((mem_addr_t)addr_low);
        return ((u64)high << 32) | (u64)low;
}

#else

#error unsupported os

#endif

sysclock_t alinx_adjust_sysclock(struct xdma_dev *xdev, sysclock_t current_sysclock, sysclock_t last_sysclock, uint8_t* adjustment) {
        unsigned long flags;
        if (last_sysclock == 0) {
                return current_sysclock;
        }
        spin_lock_irqsave(&xdev->sysclock_lock, flags);
        if (current_sysclock < last_sysclock) {
                pr_err("Sysclock error detected: current_sysclock=0x%08llx, last_sysclock=0x%08llx\n", current_sysclock, last_sysclock);
                (*adjustment) += 1;
        } else if ((*adjustment > 0) && ((current_sysclock >> 32) > (last_sysclock >> 32))) {
                pr_err("Updating adjustment: %u %llu %llu\n", *adjustment, (current_sysclock >> 32), (last_sysclock >> 32));
                (*adjustment) -= ((current_sysclock >> 32) - (last_sysclock >> 32)) - 1;
        }
        spin_unlock_irqrestore(&xdev->sysclock_lock, flags);
        return current_sysclock + ((sysclock_t)(*adjustment) << 32);
}

sysclock_t alinx_get_adjusted_sysclock(struct xdma_dev *xdev, void* hi_addr, void* lo_addr, sysclock_t* last_sysclock, uint8_t* adjustment) {
        sysclock_t current_sysclock = read64(hi_addr, lo_addr);
        sysclock_t adjusted_sysclock = alinx_adjust_sysclock(xdev, current_sysclock, *last_sysclock, adjustment);
        *last_sysclock = current_sysclock;
        return adjusted_sysclock;
}

void alinx_set_pulse_at_by_xdev(struct xdma_dev *xdev, sysclock_t time) {
        write32((u32)(time >> 32), xdev->bar[0] + REG_NEXT_PULSE_AT_HI);
        write32((u32)time, xdev->bar[0] + REG_NEXT_PULSE_AT_LO);
}

void alinx_set_pulse_at(struct pci_dev *pdev, sysclock_t time) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	alinx_set_pulse_at_by_xdev(xdev, time);
}

sysclock_t alinx_get_sys_clock_by_xdev(struct xdma_dev *xdev) {
        return alinx_get_adjusted_sysclock(xdev, xdev->bar[0] + REG_SYS_CLOCK_HI, xdev->bar[0] + REG_SYS_CLOCK_LO, &xdev->last_sysclock, &xdev->sysclock_adjustment);
        // return read64(xdev->bar[0] + REG_SYS_CLOCK_HI,
        //               xdev->bar[0] + REG_SYS_CLOCK_LO);
}

sysclock_t alinx_get_sys_clock(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
        return alinx_get_sys_clock_by_xdev(xdev);
}

void alinx_set_cycle_1s_by_xdev(struct xdma_dev *xdev, u32 cycle_1s) {
        write32(0, xdev->bar[0] + REG_CYCLE_1S_HI);
        write32(cycle_1s, xdev->bar[0] + REG_CYCLE_1S_LO);
}

void alinx_set_cycle_1s(struct pci_dev *pdev, u32 cycle_1s) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	alinx_set_cycle_1s_by_xdev(xdev, cycle_1s);
}

u32 alinx_get_cycle_1s_by_xdev(struct xdma_dev *xdev) {
        (void)read32(xdev->bar[0] + REG_CYCLE_1S_HI);
        u32 ret = read32(xdev->bar[0] + REG_CYCLE_1S_LO);
        return ret ? ret : RESERVED_CYCLE;
}

u32 alinx_get_cycle_1s(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	return alinx_get_cycle_1s_by_xdev(xdev);
}

timestamp_t alinx_read_tx_timestamp_by_xdev(struct xdma_dev* xdev, int tx_id) {
        switch (tx_id) {
        case 1:
                return alinx_get_adjusted_sysclock(xdev, xdev->bar[0] + REG_TX_TIMESTAMP1_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP1_LOW, &xdev->last_tx_timestamp[0], &xdev->tx_timestamp_adjustment[0]);
                // return read64(xdev->bar[0] + REG_TX_TIMESTAMP1_HIGH,
                //               xdev->bar[0] + REG_TX_TIMESTAMP1_LOW);
        case 2:
                return alinx_get_adjusted_sysclock(xdev, xdev->bar[0] + REG_TX_TIMESTAMP2_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP2_LOW, &xdev->last_tx_timestamp[1], &xdev->tx_timestamp_adjustment[1]);
                // return read64(xdev->bar[0] + REG_TX_TIMESTAMP2_HIGH,
                //               xdev->bar[0] + REG_TX_TIMESTAMP2_LOW);
        case 3:
                return alinx_get_adjusted_sysclock(xdev, xdev->bar[0] + REG_TX_TIMESTAMP3_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP3_LOW, &xdev->last_tx_timestamp[2], &xdev->tx_timestamp_adjustment[2]);
                // return read64(xdev->bar[0] + REG_TX_TIMESTAMP3_HIGH,
                //               xdev->bar[0] + REG_TX_TIMESTAMP3_LOW);
        case 4:
                return alinx_get_adjusted_sysclock(xdev, xdev->bar[0] + REG_TX_TIMESTAMP4_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP4_LOW, &xdev->last_tx_timestamp[3], &xdev->tx_timestamp_adjustment[3]);
                // return read64(xdev->bar[0] + REG_TX_TIMESTAMP4_HIGH,
                //               xdev->bar[0] + REG_TX_TIMESTAMP4_LOW);
        default:
                return 0;
        }
}

timestamp_t alinx_read_tx_timestamp(struct pci_dev* pdev, int tx_id) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	return alinx_read_tx_timestamp_by_xdev(xdev, tx_id);
}

u64 alinx_get_buffer_write_status_by_xdev(struct xdma_dev *xdev) {
        return read64(xdev->bar[0] + REG_BUFFER_WRITE_STATUS1_HIGH,
                      xdev->bar[0] + REG_BUFFER_WRITE_STATUS1_LOW);
}

u64 alinx_get_buffer_write_status(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
        return alinx_get_buffer_write_status_by_xdev(xdev);
}

u64 alinx_get_total_new_entry_by_xdev(struct xdma_dev *xdev) {
        return read64(xdev->bar[0] + REG_TOTAL_NEW_ENTRY_CNT_HIGH,
                      xdev->bar[0] + REG_TOTAL_NEW_ENTRY_CNT_LOW);
}

u64 alinx_get_total_valid_entry_by_xdev(struct xdma_dev *xdev) {
        return read64(xdev->bar[0] + REG_TOTAL_VALID_ENTRY_CNT_HIGH,
                      xdev->bar[0] + REG_TOTAL_VALID_ENTRY_CNT_LOW);
}

u64 alinx_get_total_drop_entry_by_xdev(struct xdma_dev *xdev) {
        return read64(xdev->bar[0] + REG_TOTAL_DROP_ENTRY_CNT_HIGH,
                      xdev->bar[0] + REG_TOTAL_DROP_ENTRY_CNT_LOW);
}

u64 alinx_get_fifo_cnt_by_xdev(struct xdma_dev *xdev) {
        return read64(xdev->bar[0] + REG_FBW_ADDR_FIFO_CNT_HIGH,
                      xdev->bar[0] + REG_FBW_ADDR_FIFO_CNT_LOW);
}

u64 alinx_get_rx_fifo_status_by_xdev(struct xdma_dev *xdev, int port) {
        switch (port) {
        case 0:
                return read64(xdev->bar[0] + REG_ETH0_RX_FIFO_STATUS_HIGH,
                              xdev->bar[0] + REG_ETH0_RX_FIFO_STATUS_LOW);
        case 1:
                return read64(xdev->bar[0] + REG_ETH1_RX_FIFO_STATUS_HIGH,
                              xdev->bar[0] + REG_ETH1_RX_FIFO_STATUS_LOW);
        default:
                pr_err("Invalid port id: %d\n", port);
                return 0;
        }
}

#ifdef __LIBXDMA_DEBUG__
void dump_buffer(unsigned char* buffer, int len)
{
        int i = 0;
        pr_err("[Buffer]");
        pr_err("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        buffer[i+0] & 0xFF, buffer[i+1] & 0xFF, buffer[i+2] & 0xFF, buffer[i+3] & 0xFF,
        buffer[i+4] & 0xFF, buffer[i+5] & 0xFF, buffer[i+6] & 0xFF, buffer[i+7] & 0xFF,
        buffer[i+8] & 0xFF, buffer[i+9] & 0xFF, buffer[i+10] & 0xFF, buffer[i+11] & 0xFF,
        buffer[i+12] & 0xFF, buffer[i+13] & 0xFF, buffer[i+14] & 0xFF, buffer[i+15] & 0xFF);

        i = 16;
        pr_err("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        buffer[i+0] & 0xFF, buffer[i+1] & 0xFF, buffer[i+2] & 0xFF, buffer[i+3] & 0xFF,
        buffer[i+4] & 0xFF, buffer[i+5] & 0xFF, buffer[i+6] & 0xFF, buffer[i+7] & 0xFF,
        buffer[i+8] & 0xFF, buffer[i+9] & 0xFF, buffer[i+10] & 0xFF, buffer[i+11] & 0xFF,
        buffer[i+12] & 0xFF, buffer[i+13] & 0xFF, buffer[i+14] & 0xFF, buffer[i+15] & 0xFF);

        i = 32;
        pr_err("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        buffer[i+0] & 0xFF, buffer[i+1] & 0xFF, buffer[i+2] & 0xFF, buffer[i+3] & 0xFF,
        buffer[i+4] & 0xFF, buffer[i+5] & 0xFF, buffer[i+6] & 0xFF, buffer[i+7] & 0xFF,
        buffer[i+8] & 0xFF, buffer[i+9] & 0xFF, buffer[i+10] & 0xFF, buffer[i+11] & 0xFF,
        buffer[i+12] & 0xFF, buffer[i+13] & 0xFF, buffer[i+14] & 0xFF, buffer[i+15] & 0xFF);

        pr_err("\n");
}
#else
void dump_buffer(unsigned char* buffer, int len) {}
#endif
