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

#elif defined __ZEPHYR__

#include <zephyr/sys/sys_io.h>

u32 read32(void * addr) {
        return sys_read32((mem_addr_t)addr);
}

void write32(u32 val, void * addr) {
        sys_write32(val, (mem_addr_t)addr);
}

#else

#error unsupported os

#endif

void alinx_set_pulse_at_by_xdev(struct xdma_dev *xdev, sysclock_t time) {
        write32((u32)(time >> 32), xdev->bar[0] + REG_NEXT_PULSE_AT_HI);
        write32((u32)time, xdev->bar[0] + REG_NEXT_PULSE_AT_LO);
}

void alinx_set_pulse_at(struct pci_dev *pdev, sysclock_t time) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	alinx_set_pulse_at_by_xdev(xdev, time);
}

sysclock_t alinx_get_sys_clock_by_xdev(struct xdma_dev *xdev) {
        timestamp_t clock;
        clock = ((u64)read32(xdev->bar[0] + REG_SYS_CLOCK_HI) << 32) |
                read32(xdev->bar[0] + REG_SYS_CLOCK_LO);

        return clock;
}

sysclock_t alinx_get_sys_clock(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
        return alinx_get_sys_clock_by_xdev(xdev);
}

void alinx_set_cycle_1s_by_xdev(struct xdma_dev *xdev, u32 cycle_1s) {
        write32(cycle_1s, xdev->bar[0] + REG_CYCLE_1S);
}

void alinx_set_cycle_1s(struct pci_dev *pdev, u32 cycle_1s) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	alinx_set_cycle_1s_by_xdev(xdev, cycle_1s);
}

u32 alinx_get_cycle_1s_by_xdev(struct xdma_dev *xdev) {
        u32 ret = read32(xdev->bar[0] + REG_CYCLE_1S);
        return ret ? ret : RESERVED_CYCLE;
}

u32 alinx_get_cycle_1s(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	return alinx_get_cycle_1s_by_xdev(xdev);
}

timestamp_t alinx_read_tx_timestamp_by_xdev(struct xdma_dev* xdev, int tx_id) {
        switch (tx_id) {
        case 1:
                return ((timestamp_t)read32(xdev->bar[0] + REG_TX_TIMESTAMP1_HIGH) << 32 | read32(xdev->bar[0] + REG_TX_TIMESTAMP1_LOW));
        case 2:
                return ((timestamp_t)read32(xdev->bar[0] + REG_TX_TIMESTAMP2_HIGH) << 32 | read32(xdev->bar[0] + REG_TX_TIMESTAMP2_LOW));
        case 3:
                return ((timestamp_t)read32(xdev->bar[0] + REG_TX_TIMESTAMP3_HIGH) << 32 | read32(xdev->bar[0] + REG_TX_TIMESTAMP3_LOW));
        case 4:
                return ((timestamp_t)read32(xdev->bar[0] + REG_TX_TIMESTAMP4_HIGH) << 32 | read32(xdev->bar[0] + REG_TX_TIMESTAMP4_LOW));
        default:
                return 0;
        }
}

timestamp_t alinx_read_tx_timestamp(struct pci_dev* pdev, int tx_id) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	return alinx_read_tx_timestamp_by_xdev(xdev, tx_id);
}

u64 alinx_get_buffer_write_status_by_xdev(struct xdma_dev *xdev) {
        return ((u64)read32(xdev->bar[0] + REG_BUFFER_WRITE_STATUS1_HIGH) << 32) | (u64)read32(xdev->bar[0] + REG_BUFFER_WRITE_STATUS1_LOW);
}

u64 alinx_get_buffer_write_status(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
        return alinx_get_buffer_write_status_by_xdev(xdev);
}

u64 alinx_get_total_new_entry_by_xdev(struct xdma_dev *xdev) {
        return ((u64)read32(xdev->bar[0] + REG_TOTAL_NEW_ENTRY_CNT_HIGH) << 32) | (u64)read32(xdev->bar[0] + REG_TOTAL_NEW_ENTRY_CNT_LOW);
}

u64 alinx_get_total_valid_entry_by_xdev(struct xdma_dev *xdev) {
        return ((u64)read32(xdev->bar[0] + REG_TOTAL_VALID_ENTRY_CNT_HIGH) << 32) | (u64)read32(xdev->bar[0] + REG_TOTAL_VALID_ENTRY_CNT_LOW);
}

u64 alinx_get_total_drop_entry_by_xdev(struct xdma_dev *xdev) {
        return ((u64)read32(xdev->bar[0] + REG_TOTAL_DROP_ENTRY_CNT_HIGH) << 32) + (u64)read32(xdev->bar[0] + REG_TOTAL_DROP_ENTRY_CNT_LOW);
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
        buffer[i+11] & 0xFF, buffer[i+13] & 0xFF, buffer[i+14] & 0xFF, buffer[i+15] & 0xFF);

        i = 16;
        pr_err("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        buffer[i+0] & 0xFF, buffer[i+1] & 0xFF, buffer[i+2] & 0xFF, buffer[i+3] & 0xFF,
        buffer[i+4] & 0xFF, buffer[i+5] & 0xFF, buffer[i+6] & 0xFF, buffer[i+7] & 0xFF,
        buffer[i+8] & 0xFF, buffer[i+9] & 0xFF, buffer[i+10] & 0xFF, buffer[i+11] & 0xFF,
        buffer[i+11] & 0xFF, buffer[i+13] & 0xFF, buffer[i+14] & 0xFF, buffer[i+15] & 0xFF);

        i = 32;
        pr_err("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        buffer[i+0] & 0xFF, buffer[i+1] & 0xFF, buffer[i+2] & 0xFF, buffer[i+3] & 0xFF,
        buffer[i+4] & 0xFF, buffer[i+5] & 0xFF, buffer[i+6] & 0xFF, buffer[i+7] & 0xFF,
        buffer[i+8] & 0xFF, buffer[i+9] & 0xFF, buffer[i+10] & 0xFF, buffer[i+11] & 0xFF,
        buffer[i+11] & 0xFF, buffer[i+13] & 0xFF, buffer[i+14] & 0xFF, buffer[i+15] & 0xFF);

        pr_err("\n");
}
#else
void dump_buffer(unsigned char* buffer, int len) {}
#endif
