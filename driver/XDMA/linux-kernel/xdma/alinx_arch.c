#include "alinx_arch.h"
#include "libxdma.h"
#include "xdma_netdev.h"

extern unsigned int se_mode;	/* HAT 모드: BAR0=코어 APB — TSNv3 시스템 레지스터 write 우회 */

#ifdef __linux__

#include <linux/io.h>
#include <linux/ktime.h>

/*
 * Track B (2026-07-07) 레지스터 read 전역 직렬화.
 *
 * AXI4L_Slave_IF(_ext)의 captured-lo 래치는 주소 구분 없이 slv_reg_rden
 * (아무 레지스터 read)마다 모든 64비트 쌍의 lo를 일괄 재래치한다.
 * 따라서 "hi read가 lo를 래치한다"는 쌍 보장은 읽는 주체가 하나일 때만
 * 성립하고, PHC gettime·ISR·TX tstamp 리더가 동시에 돌면 남의 read가
 * 내 래치를 클로버해 wrap 경계에서 +-2^32 오염값이 조합된다(1차 FAIL-01
 * 의 드라이버측 증상 및 구 +2^32 휴리스틱의 존재 이유).
 * -> 모든 레지스터 read를 한 락으로 직렬화하고, 64비트 쌍은 락 안에서
 * hi->lo 연속 read로 HW 캡처 시맨틱을 의도대로 쓴다.
 * (FPGA측 근본수정 = 캡처 주소한정. 차기 비트스트림 스핀에서.)
 * 주의: /dev/xdma0_user 유저스페이스 read도 rden을 발생시키므로 정밀
 * 타이밍 시험 중 디버그 툴(regdiff/ctrlsample 등) 병행 금지.
 */
static DEFINE_SPINLOCK(alinx_reg_lock);

u32 read32(void * addr) {
        unsigned long flags;
        u32 val;

        spin_lock_irqsave(&alinx_reg_lock, flags);
        val = ioread32(addr);
        spin_unlock_irqrestore(&alinx_reg_lock, flags);
        return val;
}

void write32(u32 val, void * addr) {
        iowrite32(val, addr);
}

u64 read64(void *addr_high, void *addr_low) {
        unsigned long flags;
        u32 high, low;

        /*
         * 실측(2026-07-07 semprobe A, doc\07 §3.5): 이 AXI 창구의 read는
         * "직전 rden 시점에 캡처된 값"을 반환한다(one-read-lag). 드라이버처럼
         * 희소하게(수십 ms 간격) 읽으면 첫 read가 수십 ms stale 값을 얻고,
         * 그 사이 lo-wrap이 끼면 {hi,lo}가 +-2^32 어긋난다.
         * -> 더미 HI read로 캡처 파이프라인을 프라이밍한 뒤 HI/LO를 읽는다.
         *    (HI = 더미 read 시점(~us 전) 값, LO = HI read 시점 캡처 = 쌍 보장)
         */
        spin_lock_irqsave(&alinx_reg_lock, flags);
        (void)ioread32(addr_high);    /* prime: 캡처 파이프라인 갱신 */
        high = ioread32(addr_high);
        low = ioread32(addr_low);
        spin_unlock_irqrestore(&alinx_reg_lock, flags);
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

/*
 * Track B (2026-07-07): +2^32 보정 휴리스틱 폐기.
 *
 * 기존 로직은 "lo wrap 시 hi로의 캐리가 늦게 반영되는 HW 버그"를 전제로,
 * raw가 reference보다 0x8000_0000~0x1_8000_0000 뒤지면 +2^32를 더했다.
 * 실측(2026-07-07, RPi5+trackB bit, 삼중읽기 13.4M 샘플/2 wrap) 결과
 * 카운터 레지스터쌍은 wrap 캐리 포함 완전 coherent — 전제가 된 HW 버그는
 * 현 비트스트림에 존재하지 않는다. 반면 이 휴리스틱은 한 번 오발동하면
 * reference(last_sysclock)가 +2^32 오염돼 이후 모든 정상 read가 보정창에
 * 들어가는 자가유지 오염 상태(dmesg "Sysclock corrected" 연발, PHC +34.4s
 * 시프트, gPTP 서보 붕괴)를 만든다 — 1h 장시간 시험에서 실제 발생.
 * 조합 read의 wrap-straddle 위험은 read64_counter(hi-lo-hi 재시도)가
 * 원천 차단하므로 보정 없이 raw를 그대로 신뢰한다.
 */
sysclock_t _alinx_adjust_sysclock(sysclock_t current_sysclock, sysclock_t reference, const char *caller) {
        (void)reference;
        (void)caller;
        return current_sysclock;
}

void alinx_set_pulse_at_by_xdev(struct xdma_dev *xdev, sysclock_t time) {
        /* se_mode: BAR0=코어 APB. 0x18/0x1C는 코어 lookup 영역 — TSNv3 PPS 레지스터 아님 */
        if (se_mode)
                return;
        write32((u32)(time >> 32), xdev->bar[0] + REG_NEXT_PULSE_AT_HI);
        write32((u32)time, xdev->bar[0] + REG_NEXT_PULSE_AT_LO);
}

void alinx_set_pulse_at(struct pci_dev *pdev, sysclock_t time) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	alinx_set_pulse_at_by_xdev(xdev, time);
}

/*
 * 7층 SW 우회 (2026-07-08, doc\07 §5): 라이브 sysclock 읽기.
 * FPGA hi 워드(도메인별 System_Counter FSM 경유)는 wrap 트리거를 놓치면
 * 34.36초간 통째로 오염된다(실측). lo(하위 32b)는 gray+skew 제약으로
 * 검증된 신뢰값 -> hi는 SW가 lo wrap을 감지해 유지한다.
 * 요구조건: 읽기 간격 < wrap 주기/2 (rx_poll_work가 100ms 주기 read로 보장).
 * 시드 hi가 오염돼 있어도 상수 시프트일 뿐 — PHC offset(settime)이 흡수.
 */
static sysclock_t sysclock_read_locked(struct xdma_dev *xdev) {
        unsigned long flags;
        u32 hi_fpga, lo;

        spin_lock_irqsave(&alinx_reg_lock, flags);
        (void)ioread32(xdev->bar[0] + REG_SYS_CLOCK_HI);   /* prime (one-read-lag) */
        hi_fpga = ioread32(xdev->bar[0] + REG_SYS_CLOCK_HI);
        lo = ioread32(xdev->bar[0] + REG_SYS_CLOCK_LO);

        if (!xdev->sw_hi_valid) {
                xdev->sw_hi = hi_fpga;
                xdev->sw_hi_valid = 1;
        } else if (lo < xdev->sw_last_lo) {
                xdev->sw_hi++;                             /* lo wrap 감지 */
        }
        xdev->sw_last_lo = lo;

        {
                /*
                 * last_sysclock 비교/갱신도 같은 락 안에서 — 락 밖 READ/WRITE_ONCE
                 * 조합은 동시 리더가 엇갈릴 때 us 스케일의 가짜 backward 경고를
                 * 만들었다(2026-07-08 새벽 v12에서 3건 실측, 전부 ~6.3us).
                 */
                sysclock_t v = ((sysclock_t)xdev->sw_hi << 32) | (sysclock_t)lo;

                if (xdev->last_sysclock && v < xdev->last_sysclock)
                        pr_warn_ratelimited("Sysclock went backward: raw=0x%010llx, last=0x%010llx\n",
                                            v, xdev->last_sysclock);
                xdev->last_sysclock = v;

                /* A3: 외삽 기준점 스탬프 (같은 락 안 — 쌍의 원자성) */
                xdev->cache_sysclock = v;
                xdev->cache_kt_ns = ktime_get_raw_ns();
                xdev->cache_valid = 1;

                spin_unlock_irqrestore(&alinx_reg_lock, flags);
                return v;
        }
}

/*
 * A3 (2026-07-26): TX 핫패스용 sysclock — BAR read 없이 캐시에서 외삽.
 * tick=8ns(125MHz 고정). 캐시가 SYSCLK_CACHE_MAX_NS보다 묵으면 실측으로 폴백
 * (기준점 재스탬프). 외삽 오차 = 발진기 ppm차 x 캐시 나이 (50ppm x 1ms = 50ns
 * = 6tick) — from/to 게이팅 창(수백us~ms)과 H2C_LATENCY 마진 대비 무시 가능.
 * 정밀 경로(PTP gettime/타임스탬프 재구성)는 계속 실측(sysclock_read_locked)을
 * 쓴다. 외삽값은 last_sysclock(역행 감시)을 갱신하지 않는다.
 */
#define SYSCLK_CACHE_MAX_NS 1000000ULL   /* 1ms */

sysclock_t alinx_get_sys_clock_fast_by_xdev(struct xdma_dev *xdev) {
        unsigned long flags;
        sysclock_t v;
        u64 now_ns;

        spin_lock_irqsave(&alinx_reg_lock, flags);
        now_ns = ktime_get_raw_ns();
        if (!xdev->cache_valid ||
            now_ns - xdev->cache_kt_ns > SYSCLK_CACHE_MAX_NS) {
                spin_unlock_irqrestore(&alinx_reg_lock, flags);
                return sysclock_read_locked(xdev);
        }
        v = xdev->cache_sysclock + (sysclock_t)((now_ns - xdev->cache_kt_ns) >> 3);
        spin_unlock_irqrestore(&alinx_reg_lock, flags);
        return v;
}

/*
 * 과거 캡처값(TX/RX 타임스탬프)의 lo를 SW hi 기준으로 64비트 재구성.
 * 캡처는 회수 시점 기준 항상 "최근 과거"(수 ms) -> 캡처 lo가 현재 lo보다
 * 크면 wrap 직전 캡처 = hi-1. (34.36초 이상 묵은 캡처는 이 경로에 없음)
 */
sysclock_t alinx_rebuild_sysclock_by_xdev(struct xdma_dev *xdev, u32 captured_lo) {
        sysclock_t now = sysclock_read_locked(xdev);
        u32 now_lo = (u32)now;
        u32 hi = (u32)(now >> 32);

        if (captured_lo > now_lo)
                hi--;
        return ((sysclock_t)hi << 32) | (sysclock_t)captured_lo;
}

sysclock_t alinx_read_sys_clock_raw(struct xdma_dev *xdev) {
        return sysclock_read_locked(xdev);
}

sysclock_t alinx_get_sys_clock_by_xdev(struct xdma_dev *xdev) {
        /* 이상탐지·last 갱신은 sysclock_read_locked 내부(락 안)에서 수행 */
        return sysclock_read_locked(xdev);
}

sysclock_t alinx_get_sys_clock(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
        return alinx_get_sys_clock_by_xdev(xdev);
}

void alinx_set_cycle_1s_by_xdev(struct xdma_dev *xdev, u32 cycle_1s) {
        /* se_mode: 0x20/0x24 = 코어 lookup 동작 레지스터 — 여기에 125e6을 쓰면
         * gPTP 트랩 액션이 0x0773xxxx로 오염된다(2026-08-03 실측). write 금지. */
        if (se_mode)
                return;
        write32(0, xdev->bar[0] + REG_CYCLE_1S_HI);
        write32(cycle_1s, xdev->bar[0] + REG_CYCLE_1S_LO);
}

void alinx_set_cycle_1s(struct pci_dev *pdev, u32 cycle_1s) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	alinx_set_cycle_1s_by_xdev(xdev, cycle_1s);
}

u32 alinx_get_cycle_1s_by_xdev(struct xdma_dev *xdev) {
        /* hi 더미 read가 lo를 래치 — 클로버 방지 위해 락 안에서 쌍 read */
        u32 ret = (u32)read64(xdev->bar[0] + REG_CYCLE_1S_HI,
                              xdev->bar[0] + REG_CYCLE_1S_LO);
        return ret ? ret : RESERVED_CYCLE;
}

u32 alinx_get_cycle_1s(struct pci_dev *pdev) {
        struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	return alinx_get_cycle_1s_by_xdev(xdev);
}

/*
 * TX timestamps can legitimately be slightly ahead of last_sysclock
 * because the packet leaves the MAC after the last sysclock read.
 * Only warn when the difference is large enough to indicate real
 * tearing or a stale register.
 */
#define TX_TSTAMP_FUTURE_THRESHOLD 500000

static timestamp_t read_tx_timestamp(struct xdma_dev *xdev, void *hi, void *lo) {
        /*
         * 캡처값의 hi도 (Tx_Tstamper의 i_syscount가 FSM 땜질 카운터 경유라)
         * 오염될 수 있다 -> lo만 취해 SW hi 기준으로 재구성(7층 우회).
         */
        sysclock_t raw = read64(hi, lo);
        sysclock_t ts = alinx_rebuild_sysclock_by_xdev(xdev, (u32)raw);
        sysclock_t last = READ_ONCE(xdev->last_sysclock);

        if (last && ts > last && (ts - last) > TX_TSTAMP_FUTURE_THRESHOLD)
                pr_warn_ratelimited("TX timestamp in the future: ts=0x%010llx, last_sysclock=0x%010llx, diff=%lld\n",
                        ts, last, (s64)(ts - last));

        return ts;
}

timestamp_t alinx_read_tx_timestamp_by_xdev(struct xdma_dev* xdev, int tx_id) {
        /*
         * Track B 결함② : Tx timestamp 슬롯 4→32 확장.
         * 슬롯 1~4 = 0x2d0~0x2ec (기존, 불변), 슬롯 5~32 = 0x350~0x428.
         * 두 블록 모두 8바이트(hi/lo) 간격이라 base+오프셋 계산으로 처리.
         */
        u32 hi_off;

        if (tx_id >= 1 && tx_id <= 4)
                hi_off = REG_TX_TIMESTAMP1_HIGH + (u32)(tx_id - 1) * 8;
        else if (tx_id >= 5 && tx_id <= 32)
                hi_off = REG_TX_TIMESTAMP5_HIGH + (u32)(tx_id - 5) * 8;
        else
                return 0;

        return read_tx_timestamp(xdev, xdev->bar[0] + hi_off,
                                 xdev->bar[0] + hi_off + 4);
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
