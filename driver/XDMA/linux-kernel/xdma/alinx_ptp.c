#include "xdma_mod.h"
#include "xdma_cdev.h"
#include "version.h"
#include "xdma_netdev.h"
#include "alinx_ptp.h"
#include "alinx_arch.h"

#define NS_IN_1S 1000000000

static timestamp_t alinx_get_timestamp(u64 sys_count, double ticks_scale, u64 offset) {
        timestamp_t timestamp = ticks_scale * sys_count;

        return timestamp + offset;
}

static void set_pulse_at(struct ptp_device_data *ptp_data, sysclock_t sys_count) {
        timestamp_t current_ns, next_pulse_ns;
        sysclock_t next_pulse_sysclock;
        struct xdma_dev *xdev = ptp_data->xdev;

        current_ns = alinx_get_timestamp(sys_count, ptp_data->ticks_scale, ptp_data->offset);
        next_pulse_ns = current_ns - (current_ns % NS_IN_1S) + NS_IN_1S;
        next_pulse_sysclock = ((double)(next_pulse_ns - ptp_data->offset) / ptp_data->ticks_scale);
        xdma_debug("ptp%u: %s sys_count=%llu, current_ns=%llu, next_pulse_ns=%llu, next_pulse_sysclock=%llu",
                   ptp_data->ptp_id, __func__, sys_count, current_ns, next_pulse_ns, next_pulse_sysclock);

        alinx_set_pulse_at_by_xdev(xdev, next_pulse_sysclock);
}

static void set_cycle_1s(struct ptp_device_data *ptp_data, u32 cycle_1s) {
        xdma_debug("ptp%u: %s cycle_1s=%u", ptp_data->ptp_id, __func__, cycle_1s);
        alinx_set_cycle_1s_by_xdev(ptp_data->xdev, cycle_1s);
}

static void set_ticks_scale(struct ptp_device_data *ptp_data, double ticks_scale) {
        alinx_set_ticks_scale(ptp_data->xdev->pdev, ticks_scale);
}

sysclock_t alinx_timestamp_to_sysclock(struct pci_dev* pdev, timestamp_t timestamp) {
        struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);
        struct ptp_device_data* ptp_data = xpdev->ptp;

        return (timestamp - ptp_data->offset) / ptp_data->ticks_scale;
}

timestamp_t alinx_sysclock_to_timestamp(struct pci_dev* pdev, sysclock_t sysclock) {
        struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);
        struct ptp_device_data* ptp_data = xpdev->ptp;

        u64 offset = ptp_data->offset;

        return alinx_get_timestamp(sysclock, ptp_data->ticks_scale, offset);
}

timestamp_t alinx_get_rx_timestamp(struct pci_dev* pdev, sysclock_t sysclock) {
        return alinx_sysclock_to_timestamp(pdev, sysclock) - RX_ADJUST_NS;
}

timestamp_t alinx_get_tx_timestamp(struct pci_dev* pdev, int tx_id) {
        sysclock_t sysclock = alinx_read_tx_timestamp(pdev, tx_id);

        return alinx_sysclock_to_timestamp(pdev, sysclock) + TX_ADJUST_NS;
}

timestamp_t alinx_sysclock_to_txtstamp(struct pci_dev* pdev, sysclock_t sysclock) {
        return alinx_sysclock_to_timestamp(pdev, sysclock) + TX_ADJUST_NS;
}

double alinx_get_ticks_scale(struct pci_dev* pdev) {
        struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);
        struct ptp_device_data* ptp_data = xpdev->ptp;

        return ptp_data->ticks_scale;
}

void alinx_set_ticks_scale(struct pci_dev* pdev, double ticks_scale) {
        struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);
        struct ptp_device_data* ptp_data = xpdev->ptp;

        ptp_data->ticks_scale = ticks_scale;
        alinx_set_cycle_1s(pdev, (double)NS_IN_1S / ptp_data->ticks_scale);
}

static int alinx_ptp_gettimex(struct ptp_clock_info *ptp, struct timespec64 *ts,
			  struct ptp_system_timestamp *sts)
{
        u64 clock, timestamp;
        unsigned long flags;

        struct ptp_device_data *ptp_data = container_of(
                                ptp,
                                struct ptp_device_data,
                                ptp_info);

        spin_lock_irqsave(&ptp_data->lock, flags);

        ptp_read_system_prets(sts);
        clock = alinx_get_sys_clock_by_xdev(ptp_data->xdev);
        ptp_read_system_postts(sts);

        timestamp = alinx_get_timestamp(clock, ptp_data->ticks_scale, ptp_data->offset);

        ts->tv_sec = timestamp / NS_IN_1S;
        ts->tv_nsec = timestamp % NS_IN_1S;

        spin_unlock_irqrestore(&ptp_data->lock, flags);

        xdma_debug("ptp%u: %s clock=%llu, timestamp=%llu", ptp_data->ptp_id, __func__, clock, timestamp);

        return 0;
}

static int alinx_ptp_settime(struct ptp_clock_info *ptp, const struct timespec64 *ts)
{
        u64 hw_timestamp, host_timestamp, sys_clock;
        unsigned long flags;

        struct ptp_device_data *ptp_data = container_of(
                                ptp,
                                struct ptp_device_data,
                                ptp_info);

        struct xdma_dev *xdev = ptp_data->xdev;

        /* Get host timestamp */
        host_timestamp = (u64)ts->tv_sec * NS_IN_1S + ts->tv_nsec;

        spin_lock_irqsave(&ptp_data->lock, flags);

        ptp_data->ticks_scale = TICKS_SCALE;

        sys_clock = alinx_get_sys_clock_by_xdev(xdev);
        hw_timestamp = alinx_get_timestamp(sys_clock, ptp_data->ticks_scale, ptp_data->offset);

        ptp_data->offset = host_timestamp - hw_timestamp;

        set_cycle_1s(ptp_data, RESERVED_CYCLE);
        set_pulse_at(ptp_data, sys_clock);

        spin_unlock_irqrestore(&ptp_data->lock, flags);

        xdma_debug("ptp%u: %s host_timestamp=%llu, hw_timestamp=%llu, offset=%llu",
                   ptp_data->ptp_id,__func__, host_timestamp, hw_timestamp, ptp_data->offset);

        return 0;
}

static int alinx_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
        u64 sys_clock;
        unsigned long flags;

        struct ptp_device_data *ptp_data = container_of(
                                ptp,
                                struct ptp_device_data,
                                ptp_info);

        spin_lock_irqsave(&ptp_data->lock, flags);

        /* Adjust offset */
        ptp_data->offset += delta;

        /* Set pulse_at */
        sys_clock = alinx_get_sys_clock_by_xdev(ptp_data->xdev);
        set_pulse_at(ptp_data, sys_clock);

        spin_unlock_irqrestore(&ptp_data->lock, flags);

        xdma_debug("ptp%u: %s delta=%lld, offset=%llu", ptp_data->ptp_id, __func__, delta, ptp_data->offset);

        return 0;
}

static int alinx_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
        u64 cur_timestamp, new_timestamp;
        u64 sys_clock;
        double diff;
        unsigned long flags;
        int is_negative = 0;

        struct ptp_device_data *ptp_data = container_of(
                                ptp,
                                struct ptp_device_data,
                                ptp_info);
        struct xdma_dev *xdev = ptp_data->xdev;

        spin_lock_irqsave(&ptp_data->lock, flags);

        sys_clock = alinx_get_sys_clock_by_xdev(xdev);

        if (scaled_ppm == 0) {
                goto exit;
        }

        cur_timestamp = alinx_get_timestamp(sys_clock, ptp_data->ticks_scale, ptp_data->offset);

        if (scaled_ppm < 0) {
                is_negative = 1;
                scaled_ppm = -scaled_ppm;
        }

        /* Adjust ticks_scale */
        diff = TICKS_SCALE * (double)scaled_ppm / (double)(1000000ULL << 16);
        ptp_data->ticks_scale = TICKS_SCALE + (is_negative ? - diff : diff);

        /* Adjust offset */
        new_timestamp = alinx_get_timestamp(sys_clock, ptp_data->ticks_scale, ptp_data->offset);
        ptp_data->offset += (cur_timestamp - new_timestamp);

        /* Adjust cycle_1s */
        set_ticks_scale(ptp_data, ptp_data->ticks_scale);

        /* Set pulse_at */
        sys_clock = alinx_get_sys_clock_by_xdev(xdev);
        set_pulse_at(ptp_data, sys_clock);

        xdma_debug("ptp%u: %s scaled_ppm=%ld, offset=%llu, ticks_scale:%lf",
                   ptp_data->ptp_id, __func__, scaled_ppm, ptp_data->offset, ptp_data->ticks_scale);

exit:
        spin_unlock_irqrestore(&ptp_data->lock, flags);

        return 0;
}

static struct ptp_clock_info ptp_clock_info_init(void) {
        struct ptp_clock_info info = {
                .owner = THIS_MODULE,
                .name = "ptp",
                .max_adj = RESERVED_CYCLE,
                .n_ext_ts = 0,
                .pps = 0,
                .adjfine = alinx_ptp_adjfine,
                .adjtime = alinx_ptp_adjtime,
                .gettimex64 = alinx_ptp_gettimex,
                .settime64 = alinx_ptp_settime,
        };

        return info;
}

struct ptp_device_data *ptp_device_init(struct device *dev, struct xdma_dev *xdev) {

        struct ptp_device_data *ptp;
        struct timespec64 ts;
#ifdef __LIBXDMA_DEBUG__
        static u32 ptp_cnt = 0;
#endif

        ptp = kzalloc(sizeof(struct ptp_device_data), GFP_KERNEL);
        if (!ptp) {
                pr_err("Failed to allocate memory for ptp device\n");
                return NULL;
        }
        memset(ptp, 0, sizeof(struct ptp_device_data));

        ptp->ptp_info = ptp_clock_info_init();
        ptp->ticks_scale = TICKS_SCALE;

        spin_lock_init(&ptp->lock);

        ptp->xdev = xdev;

        ptp->ptp_clock = ptp_clock_register(&ptp->ptp_info, dev);
        if (IS_ERR(ptp->ptp_clock)) {
                pr_err("Failed to register ptp clock\n");
                kfree(ptp);
                return NULL;
        }

#ifdef __LIBXDMA_DEBUG__
        ptp->ptp_id = ptp_cnt++;
#endif

        /* Set offset, cycle_1s */
        ts = ktime_to_timespec64(ktime_get_real());
        alinx_ptp_settime(&ptp->ptp_info, &ts);

        return ptp;
}

void ptp_device_destroy(struct ptp_device_data *ptp_data) {
        ptp_clock_unregister(ptp_data->ptp_clock);
        kfree(ptp_data);
}
