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

sysclock_t _alinx_adjust_sysclock(sysclock_t current_sysclock, sysclock_t reference, const char *caller) {
        if (reference == 0)
                return current_sysclock;

        /*
         * HW bug: when low 32 bits overflow, the carry to high bits
         * may be delayed. If current is behind reference by roughly
         * one high-bit unit (0x1_0000_0000), add the missed carry.
         *
         * Case 1: same high, low wrapped (ref was pre-overflow)
         *   raw=0x0900819a36, ref=0x09ffa226b1 → diff ~0xFF20...
         * Case 2: ref already corrected to high+1
         *   raw=0x09008fef67, ref=0x0a00819a36 → diff ~0xFF...
         */
        if (current_sysclock < reference &&
            (reference - current_sysclock) > 0x80000000ULL &&
            (reference - current_sysclock) < 0x180000000ULL) {
                pr_warn_ratelimited("Sysclock corrected [%s]: raw=0x%010llx, ref=0x%010llx\n", caller, current_sysclock, reference);
                return current_sysclock + (1ULL << 32);
        }
        return current_sysclock;
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
        sysclock_t raw = read64(xdev->bar[0] + REG_SYS_CLOCK_HI,
                                xdev->bar[0] + REG_SYS_CLOCK_LO);
        sysclock_t last = xdev->last_sysclock;
        sysclock_t adjusted = alinx_adjust_sysclock(raw, last);

        if (last && adjusted < last)
                pr_warn_ratelimited("Sysclock went backward: raw=0x%010llx, adjusted=0x%010llx, last=0x%010llx\n",
                                    raw, adjusted, last);
        else if (last && (adjusted - last) > (u64)RESERVED_CYCLE * 2)
                pr_warn_ratelimited("Sysclock jumped forward: raw=0x%010llx, adjusted=0x%010llx, last=0x%010llx, diff=%llu\n",
                                    raw, adjusted, last, adjusted - last);

        xdev->last_sysclock = adjusted;
        return adjusted;
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

static timestamp_t read_tx_timestamp(struct xdma_dev *xdev, void *hi, void *lo) {
        sysclock_t raw = read64(hi, lo);
        sysclock_t adjusted = alinx_adjust_sysclock(raw, xdev->last_sysclock);
        sysclock_t last = xdev->last_sysclock;

        if (last && adjusted > last)
                pr_warn("TX timestamp in the future: raw=0x%010llx, adjusted=0x%010llx, last_sysclock=0x%010llx, diff=%lld\n",
                        raw, adjusted, last, (s64)(adjusted - last));
        else if (last && (last - adjusted) > (u64)RESERVED_CYCLE * 2)
                pr_warn("TX timestamp too old: raw=0x%010llx, adjusted=0x%010llx, last_sysclock=0x%010llx, diff=%lld\n",
                        raw, adjusted, last, (s64)(last - adjusted));

        return adjusted;
}

timestamp_t alinx_read_tx_timestamp_by_xdev(struct xdma_dev* xdev, int tx_id) {
        switch (tx_id) {
        case 1:
                return read_tx_timestamp(xdev, xdev->bar[0] + REG_TX_TIMESTAMP1_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP1_LOW);
        case 2:
                return read_tx_timestamp(xdev, xdev->bar[0] + REG_TX_TIMESTAMP2_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP2_LOW);
        case 3:
                return read_tx_timestamp(xdev, xdev->bar[0] + REG_TX_TIMESTAMP3_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP3_LOW);
        case 4:
                return read_tx_timestamp(xdev, xdev->bar[0] + REG_TX_TIMESTAMP4_HIGH, xdev->bar[0] + REG_TX_TIMESTAMP4_LOW);
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
