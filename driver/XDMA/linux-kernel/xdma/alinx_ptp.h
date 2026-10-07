#ifndef ALINX_PTP_H
#define ALINX_PTP_H

#include "libxdma.h"
#include "tsn.h"

#ifdef __LIBXDMA_DEBUG__
#define xdma_debug(...) pr_debug(__VA_ARGS__)
#else
#define xdma_debug(...) {}
#endif  // __LIBXDMA_DEBUG__

struct ptp_device_data *ptp_device_init(struct device *dev, struct xdma_dev *xdev);
void ptp_device_destroy(struct ptp_device_data *ptp);

sysclock_t alinx_timestamp_to_sysclock(struct pci_dev* pdev, timestamp_t timestamp);
timestamp_t alinx_sysclock_to_timestamp(struct pci_dev* pdev, sysclock_t sysclock);
timestamp_t alinx_get_rx_timestamp(struct pci_dev* pdev, sysclock_t sysclock);
timestamp_t alinx_get_tx_timestamp(struct pci_dev* pdev, int tx_id);
timestamp_t alinx_sysclock_to_txtstamp(struct pci_dev* pdev, sysclock_t sysclock);

u64 alinx_get_ticks_scale(struct pci_dev* pdev);
void alinx_set_ticks_scale(struct pci_dev* pdev, u64 ticks_scale_fp);

#endif /* ALINX_PTP_H */
