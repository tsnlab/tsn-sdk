#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <net/flow_offload.h>
#include <linux/skbuff.h>
#include <linux/ptp_classify.h>

#include <linux/delay.h>
#include "xdma_netdev.h"
#include "xdma_mod.h"
#include "cdev_sgdma.h"
#include "libxdma.h"
#include "tsn.h"
#include "alinx_arch.h"
#include "frer.h"

/* S2(2026-08-14): H2C 디스크립터 인접 프리페치. 2-1 규명(소프레임 TX는 per-desc
 * PCIe 왕복지연 바운드, x86 루트콤플렉스 지연이 x1 HAT보다 큼)의 실행편. 엔진이
 * 인접 디스크립터를 한 요청으로 블록 fetch하면 여러 프레임의 payload read를 다중
 * outstanding으로 파이프라인해 왕복지연을 은폐한다. tx_prefetch=0이면 종전 동작
 * (first_desc_adjacent=0, 프레임당 개별 fetch). 회귀 안전: EOP/프레임 순서/STOPPED
 * 종료 시맨틱 불변 — 엔진의 fetch 효율만 바뀐다(데이터/와이어 거동 동일). */
unsigned int tx_prefetch = 1;
module_param(tx_prefetch, uint, 0644);
MODULE_PARM_DESC(tx_prefetch,
	"H2C descriptor adjacent prefetch (1=on default, 0=legacy per-frame fetch)");

/* tx_bounce(2026-08-29 근본fix 실험): 1이면 TX를 streaming dma_map_single 대신
 * coherent bounce buffer로 보낸다. skb->data를 고정 coherent 슬롯(tx_bufs)에 memcpy
 * 후 device가 그 고정 주소에서 DMA read. streaming map/unmap이 사라져 lazy IOMMU
 * unmap 창(재활용 유저페이지 손상 벡터)이 원천 소멸한다. 프레임당 memcpy = CPU 비용.
 * 0=production streaming(기본). 이 param으로 손상 벡터가 우리 TX 매핑인지 A/B 판정. */
unsigned int tx_bounce;
module_param(tx_bounce, uint, 0644);
MODULE_PARM_DESC(tx_bounce,
	"1=coherent bounce buffer TX (no streaming map), 0=streaming (default)");

/* se_tx_dest_port(2026-09-10): se_mode TX 프레임의 CPU 포트 헤더 DEST_PORT.
 *
 * DEST_PORT[7:0]:
 *   0x00 = MAC Lookup 으로 포워딩 / 0x01 = port 1 / 0x02 = port 2 / 0x80,0xFF = 전 포트
 * 이 값은 ptp_mux 가 프레임별로 metadata2[87:80](= tx_metadata.reserved0[0])에서
 * 읽는다. 0이면 BD 파라미터 DST_PORT_DFLT(현 형상 = 1)로 대체된다
 *
 * ★그래서 지금까지 호스트 TX 프레임은 전부 port 1 단일 포트로만 나갔고 FRER
 * 복제 경로를 타지 못했다(2026-09-10 실측: 코어생성 dst=0x80 -> listener
 * discarded=20(복제 O) / dst=0x01 -> discarded=0 / 호스트 프레임 -> discarded=0
 * = dst=0x01 과 동일). FRER 이중경로로 호스트 payload 를 보내려면 0x80.
 *
 * 기본 0 = 종전 동작 그대로(RTL 기본 포트). 런타임 변경 가능
 * (`/sys/module/xdma/parameters/se_tx_dest_port`)이라 시연 중 전환에 재적재가
 * 필요 없다. se_mode 가 아닐 때는 사용하지 않는다. */
unsigned int se_tx_dest_port;
module_param(se_tx_dest_port, uint, 0644);
MODULE_PARM_DESC(se_tx_dest_port,
	"CPU port TX header DEST_PORT for se_mode frames "
	"(0=leave RTL default, 0x01=port1, 0x02=port2, 0x80=all ports for FRER dual path)");

/* 인접 개수(Nxt_adj) 산정 — 표준 libxdma xdma_get_next_adj 복제 + 원형 링 상한.
 *   remaining : 이 디스크립터 뒤로 체인에 남은 개수(desc_num - k - 1). <=1이면 0.
 *   next_lo   : 인접 블록 정렬 기준 주소(표준: per-desc=다음 desc 주소, register=첫
 *               desc 자기 주소). (addr in page)/32 % 64 로 64블록 내 위치 산출.
 *   ring_idx  : 이 디스크립터의 링 인덱스. 원형 링은 wrap(255->0)에서 메모리 비연속
 *               이라 링 끝까지(=TX_RING_SIZE-1-ring_idx)로 상한해 cross-wrap 연속
 *               fetch를 막는다(표준은 fresh 연속배열이라 이 상한이 불필요했음).
 * 회귀 안전: 잘못된 상한/off-by-one이 STOPPED 너머·wrap 너머로 프리페치하면 엔진이
 * stale desc를 물어 TX가 깨진다(2026-08-14 1차 구현 실측 — 코어 wedge). */
static inline u32 tx_next_adj(u32 remaining, u32 next_lo, u32 ring_idx)
{
	u32 next_index, adj, cap;

	if (remaining <= 1)
		return 0;
	next_index = ((next_lo & (XDMA_PAGE_SIZE - 1)) >> 5) &
		     (XDMA_MAX_ADJ_BLOCK_SIZE - 1);
	adj = (XDMA_MAX_ADJ_BLOCK_SIZE - 1) - next_index;   /* 64블록 끝까지 */
	if (adj > remaining - 1)
		adj = remaining - 1;                        /* 체인 잔여로 상한 */
	cap = (TX_RING_SIZE - 1) - ring_idx;                /* 링 끝까지(wrap 방지) */
	if (adj > cap)
		adj = cap;
	return adj;
}

/* STOP -> START 순서 방어(2026-08-10 NIC x86 브링업 중 추가): XDMA 규약상 STOP
 * write 후 엔진 내부 정지 완료(BUSY 해제) 전에 START가 도달하면 미정의 동작
 * 위험이 있어, 체인 소진 재킥 전 BUSY 해제를 유한 대기한다(us 오더).
 * 주의(정직 기록): 당시 조사하던 x86 TCP 스톨의 실제 원인은 이 레이스가 아니라
 * iperf3 3.16(호스트)↔3.18(피어) 컨트롤 프로토콜 비호환으로 판명됨(datapath
 * 무죄, 소형/대형 프레임 라인레이트 전달 실측). 이 함수는 스펙 준수 방어로 유지. */
static inline void xdma_engine_stop_sync(struct xdma_engine *engine)
{
	int i;

	iowrite32(DMA_ENGINE_STOP, &engine->regs->control);
	for (i = 0; i < 200; i++) {
		if (!(ioread32(&engine->regs->status) & XDMA_STAT_BUSY))
			return;
		udelay(1);
	}
	pr_warn_ratelimited("xdma: engine busy after stop (200us)\n");
}

#define LOWER_29_BITS ((1ULL << 29) - 1)
#define TX_WORK_OVERFLOW_MARGIN 100

#define WORKAROUND_PAD 5

static size_t workaround_packet_size(size_t size) {
#ifdef __LIBXDMA_RPI__
        if (size >= 95 && size <= 99) {
                return size + WORKAROUND_PAD;
        }
#endif
        return size;
}

void tx_desc_set(struct xdma_desc *desc, dma_addr_t addr, u32 len)
{
        u32 control_field;
        u32 control;

        desc->control = cpu_to_le32(DESC_MAGIC);
        control_field = XDMA_DESC_STOPPED;
        control_field |= XDMA_DESC_EOP;
        control_field |= XDMA_DESC_COMPLETED;
        control = le32_to_cpu(desc->control & ~(LS_BYTE_MASK));
        control |= control_field;
        desc->control = cpu_to_le32(control);

        desc->src_addr_lo = cpu_to_le32(PCI_DMA_L(addr));
        desc->src_addr_hi = cpu_to_le32(PCI_DMA_H(addr));
        desc->bytes = cpu_to_le32(len);
}

/* ------------------------------------------------------------------ */
/* A1: TX 디스크립터 링 + 체인 제출 + NAPI 완료처리                     */
/* ------------------------------------------------------------------ */

static inline dma_addr_t tx_ring_desc_bus(struct xdma_private_common *common, u32 idx)
{
        return common->tx_ring_bus + (dma_addr_t)(idx & TX_RING_MASK) * sizeof(struct xdma_desc);
}

static inline u32 tx_ring_used(struct xdma_private_common *common)
{
        return common->tx_head - common->tx_tail;
}

static inline u32 tx_ring_free(struct xdma_private_common *common)
{
        return TX_RING_SIZE - tx_ring_used(common);
}

/*
 * 대기 프레임(tx_tail+tx_run_len .. tx_head)에서 포트 호환 구간을 체인으로 묶어
 * 엔진에 1회 START 제출. tx_lock 보유 상태에서 호출. 엔진 유휴(tx_run_len==0)
 * 전제. 마지막 desc만 STOPPED|COMPLETED -> 체인당 완료 IRQ 1회.
 */
static void xdma_tx_ring_kick(struct xdma_private_common *common)
{
        struct xdma_engine *engine = common->tx_engine;
        struct xdma_dev *xdev = common->xdev;
        u32 first, i, n, k, pending;
        s8 chain_port = -1;
        u32 ctrl;
        dma_addr_t bus;

        if (common->tx_run_len)
                return;         /* 체인 실행 중 — 완료 시 NAPI가 재kick */

        pending = tx_ring_used(common);
        if (!pending)
                return;

        first = common->tx_tail;
        for (n = 0; n < pending; n++) {
                s8 p = common->tx_slots[(first + n) & TX_RING_MASK].port_id;
                if (p >= 0) {
                        if (chain_port < 0)
                                chain_port = p;
                        else if (p != chain_port)
                                break;  /* 포트가 바뀌면 체인 절단 */
                }
        }

        /* 체인 내 desc 마무리: 마지막에만 STOPPED|COMPLETED */
        i = (first + n - 1) & TX_RING_MASK;
        ctrl = le32_to_cpu(common->tx_ring[i].control);
        ctrl |= XDMA_DESC_STOPPED | XDMA_DESC_COMPLETED;
        common->tx_ring[i].control = cpu_to_le32(ctrl);

        /* S2: 체인 각 desc의 인접 개수(Nxt_adj [13:8]) 설정 — 표준 libxdma 방식.
         * per-desc는 다음 desc 주소(next_lo) 기준, 체인 잔여(n-k-1)·링끝으로 상한.
         * 제어 하위바이트(EOP/STOPPED/COMPLETED)와 [15:12] 보존(& 0x0000f0ff). */
        if (tx_prefetch) {
                for (k = 0; k < n; k++) {
                        u32 di = (first + k) & TX_RING_MASK;
                        u32 nadj = tx_next_adj(n - k - 1,
                                le32_to_cpu(common->tx_ring[di].next_lo), di);
                        u32 c = le32_to_cpu(common->tx_ring[di].control) &
                                0x0000f0ffUL;
                        c |= DESC_MAGIC | (nadj << 8);
                        common->tx_ring[di].control = cpu_to_le32(c);
                }
        }

        if (chain_port >= 0 && chain_port != common->tx_port)
                xdma_swap_ports(xdev, chain_port, common->rx_port);

        dma_wmb();

        ioread32(&engine->regs->status_rc);     /* 이전 run 상태 클리어 */
        bus = tx_ring_desc_bus(common, first);
        iowrite32(cpu_to_le32(PCI_DMA_L(bus)), &engine->sgdma_regs->first_desc_lo);
        iowrite32(cpu_to_le32(PCI_DMA_H(bus)), &engine->sgdma_regs->first_desc_hi);
        /* S2: 첫 디스크립터 인접 프리페치 개수(표준: 첫 desc 자기 주소 기준, n·링끝
         * 상한). off=0=종전 프레임당 개별 fetch. */
        iowrite32(tx_prefetch ?
                  tx_next_adj(n, cpu_to_le32(PCI_DMA_L(bus)), first & TX_RING_MASK) : 0,
                  &engine->sgdma_regs->first_desc_adjacent);
        iowrite32(DMA_ENGINE_START, &engine->regs->control);

        common->tx_run_len = n;
}

/*
 * start_xmit 경로에서 프레임 1개를 링에 적재. tx_lock 보유 상태에서 호출.
 * 링이 가득 차 있으면 false(호출측에서 drop 처리 — 사전 고수위 stop으로 희귀).
 */
static bool xdma_tx_ring_enqueue(struct xdma_private_common *common,
                                 struct sk_buff *skb, dma_addr_t dma, s8 port_id)
{
        struct tx_ring_slot *slot;
        struct xdma_desc *desc;
        u32 idx;

        if (tx_ring_free(common) == 0)
                return false;

        idx = common->tx_head & TX_RING_MASK;
        slot = &common->tx_slots[idx];
        if (tx_bounce) {
                /* skb->data를 슬롯별 고정 coherent 버퍼에 복사하고 그 주소를 desc로.
                 * streaming map/unmap 없음 → lazy-unmap 창 노출 소멸. (len<=BUF는
                 * start_xmit에서 보장) */
                memcpy(common->tx_bufs + (size_t)idx * TX_BOUNCE_BUF_SIZE,
                       skb->data, skb->len);
                dma = common->tx_bufs_bus + (dma_addr_t)idx * TX_BOUNCE_BUF_SIZE;
        }
        slot->skb = skb;
        slot->dma = dma;
        slot->len = skb->len;
        slot->port_id = port_id;

        desc = &common->tx_ring[idx];
        /* 인접 개수(Nxt_adj)는 체인 확정 후 kick에서 설정(체인 잔여·wrap 상한 필요) */
        desc->control = cpu_to_le32(DESC_MAGIC | XDMA_DESC_EOP);
        desc->src_addr_lo = cpu_to_le32(PCI_DMA_L(dma));
        desc->src_addr_hi = cpu_to_le32(PCI_DMA_H(dma));
        desc->bytes = cpu_to_le32(skb->len);
        /* next 포인터는 링 생성 시 원형으로 프리링크됨 */

        common->tx_head++;

        if (common->tx_run_len == 0)
                xdma_tx_ring_kick(common);
        return true;
}

/* ISR H2C 완료 훅: NAPI로 위임 (H2C 채널 IRQ는 ISR 진입부에서 이미 disable됨) */
void xdma_tx_completion_irq(struct xdma_private_common *common)
{
        napi_schedule(&common->tx_napi);
}

static int xdma_tx_napi_poll(struct napi_struct *napi, int budget)
{
        struct xdma_private_common *common =
                container_of(napi, struct xdma_private_common, tx_napi);
        struct xdma_engine *engine = common->tx_engine;
        struct xdma_dev *xdev = common->xdev;
        struct sk_buff_head done;
        struct sk_buff *skb;
        unsigned long flags;
        u32 status, cmpl, n, i;
        bool ring_has_room;

        __skb_queue_head_init(&done);

        spin_lock_irqsave(&common->tx_lock, flags);
        status = ioread32(&engine->regs->status_rc);    /* read-clear */
        cmpl = ioread32(&engine->regs->completed_desc_count);

        if (common->tx_run_len &&
            ((status & XDMA_STAT_DESC_STOPPED) || cmpl >= common->tx_run_len)) {
                if (unlikely(status & XDMA_STAT_COMMON_ERR_MASK))
                        pr_err_ratelimited("tx engine error status 0x%08x\n", status);

                n = common->tx_run_len;
                for (i = 0; i < n; i++) {
                        struct tx_ring_slot *slot =
                                &common->tx_slots[(common->tx_tail + i) & TX_RING_MASK];
                        /* dma/len을 skb->cb에 실어 언맵을 락 밖에서 수행 */
                        *(dma_addr_t *)slot->skb->cb = slot->dma;
                        *(u32 *)(slot->skb->cb + sizeof(dma_addr_t)) = slot->len;
                        __skb_queue_tail(&done, slot->skb);
                        slot->skb = NULL;
                }
                common->tx_tail += n;
                common->tx_run_len = 0;

                xdma_engine_stop_sync(engine);
                xdma_tx_ring_kick(common);      /* 다음 체인 즉시 제출 */
        }
        ring_has_room = tx_ring_free(common) >= TX_RING_WAKE_THRESH;
        spin_unlock_irqrestore(&common->tx_lock, flags);

        n = 0;
        while ((skb = __skb_dequeue(&done)) != NULL) {
                dma_addr_t dma = *(dma_addr_t *)skb->cb;
                u32 len = *(u32 *)(skb->cb + sizeof(dma_addr_t));

                if (!tx_bounce)
                        dma_unmap_single(&xdev->pdev->dev, dma, len, DMA_TO_DEVICE);
                if (xdev->ndev[0]) {
                        xdev->ndev[0]->stats.tx_packets++;
                        xdev->ndev[0]->stats.tx_bytes += len;
                }
                dev_kfree_skb_any(skb);
                n++;
        }

        if (ring_has_room)
                xdma_start_all_queues(xdev);

        napi_complete_done(napi, 0);
        channel_interrupts_enable(xdev, xdev->mask_irq_h2c);

        /* 재확인: IRQ 재활성화 직전에 완료된 체인을 놓치지 않는다 */
        if (common->tx_run_len &&
            ioread32(&engine->regs->completed_desc_count) >= common->tx_run_len)
                napi_schedule(&common->tx_napi);

        return 0;
}

int xdma_tx_ring_alloc(struct xdma_private_common *common)
{
        struct pci_dev *pdev = common->pdev;
        u32 i;

        common->tx_ring = dma_alloc_coherent(&pdev->dev,
                        TX_RING_SIZE * sizeof(struct xdma_desc),
                        &common->tx_ring_bus, GFP_KERNEL);
        if (!common->tx_ring)
                return -ENOMEM;

        memset(common->tx_ring, 0, TX_RING_SIZE * sizeof(struct xdma_desc));
        for (i = 0; i < TX_RING_SIZE; i++) {
                dma_addr_t next = tx_ring_desc_bus(common, i + 1);

                common->tx_ring[i].control = cpu_to_le32(DESC_MAGIC | XDMA_DESC_STOPPED);
                common->tx_ring[i].next_lo = cpu_to_le32(PCI_DMA_L(next));
                common->tx_ring[i].next_hi = cpu_to_le32(PCI_DMA_H(next));
        }
        if (tx_bounce) {
                common->tx_bufs = dma_alloc_coherent(&pdev->dev,
                                TX_RING_SIZE * (size_t)TX_BOUNCE_BUF_SIZE,
                                &common->tx_bufs_bus, GFP_KERNEL);
                if (!common->tx_bufs) {
                        dma_free_coherent(&pdev->dev,
                                          TX_RING_SIZE * sizeof(struct xdma_desc),
                                          common->tx_ring, common->tx_ring_bus);
                        common->tx_ring = NULL;
                        return -ENOMEM;
                }
        }
        common->tx_head = 0;
        common->tx_tail = 0;
        common->tx_run_len = 0;
        return 0;
}

void xdma_tx_ring_free(struct xdma_private_common *common)
{
        struct pci_dev *pdev = common->pdev;
        u32 i;

        if (!common->tx_ring)
                return;

        for (i = common->tx_tail; i != common->tx_head; i++) {
                struct tx_ring_slot *slot = &common->tx_slots[i & TX_RING_MASK];

                if (slot->skb) {
                        if (!tx_bounce)
                                dma_unmap_single(&pdev->dev, slot->dma, slot->len,
                                                 DMA_TO_DEVICE);
                        dev_kfree_skb_any(slot->skb);
                        slot->skb = NULL;
                }
        }
        if (common->tx_bufs) {
                dma_free_coherent(&pdev->dev, TX_RING_SIZE * (size_t)TX_BOUNCE_BUF_SIZE,
                                  common->tx_bufs, common->tx_bufs_bus);
                common->tx_bufs = NULL;
        }
        dma_free_coherent(&pdev->dev, TX_RING_SIZE * sizeof(struct xdma_desc),
                          common->tx_ring, common->tx_ring_bus);
        common->tx_ring = NULL;
}

/* probe에서 NAPI 등록용 (ndev[0]에 부착) */
void xdma_tx_napi_setup(struct xdma_private_common *common, struct net_device *ndev)
{
        netif_napi_add(ndev, &common->tx_napi, xdma_tx_napi_poll);
        napi_enable(&common->tx_napi);
}

void xdma_tx_napi_teardown(struct xdma_private_common *common)
{
        napi_disable(&common->tx_napi);
        netif_napi_del(&common->tx_napi);
}

/* ------------------------------------------------------------------ */
/* A2: RX 디스크립터 링 + 체인 제출 + NAPI 수확                         */
/* ------------------------------------------------------------------ */

static inline dma_addr_t rx_ring_desc_bus(struct xdma_private_common *common, u32 idx)
{
        return common->rx_ring_bus + (dma_addr_t)(idx & RX_RING_MASK) * sizeof(struct xdma_desc);
}

/* libxdma.c ISR에서 이관 (A2) — RX 수확 경로 전용 */
static bool filter_rx_timestamp(struct xdma_private_common *common, struct sk_buff *skb)
{
        u8 msg_type;
        u16 eth_type;
        uint8_t *payload;
        struct ptp_header *ptp;
        struct ethhdr *eth;
        struct tsn_vlan_hdr *vlan;
        int rx_filter = common->tstamp_config.rx_filter;

        if (rx_filter == HWTSTAMP_FILTER_NONE) {
                return false;
        } else if (rx_filter == HWTSTAMP_FILTER_ALL) {
                return true;
        }

        payload = skb->data;
        eth = (struct ethhdr *)payload;
        payload += sizeof(*eth);
        eth_type = ntohs(eth->h_proto);
        if (eth_type == ETH_P_8021Q) {
                vlan = (struct tsn_vlan_hdr *)payload;
                eth_type = vlan->pid;
                payload += sizeof(*vlan);
        }

        if (eth_type != ETH_P_1588) {
                return false;
        }

        ptp = (struct ptp_header *)payload;
        msg_type = ptp->tsmt & 0xF;
        switch (rx_filter) {
        case HWTSTAMP_FILTER_PTP_V2_EVENT:
        case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
                return true;
        case HWTSTAMP_FILTER_PTP_V2_SYNC:
        case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
                return msg_type == PTP_MSGTYPE_SYNC;
        case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
        case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
                return msg_type == PTP_MSGTYPE_DELAY_REQ;
        default:
                return false;
        }
}

/*
 * 빈 슬롯([rx_head, rx_tail+RING) 구간)을 체인으로 묶어 엔진에 1회 START 제출.
 * rx_lock 보유 + 엔진 유휴(rx_run_len==0) 전제. 각 desc는 COMPLETED(프레임당
 * IRQ), 마지막만 STOPPED(링 추월 방지 — SW가 밀리면 엔진이 멈추고 FPGA FIFO가
 * 흡수). 실행 중 체인의 desc는 절대 수정하지 않는다(레이스 원천 차단).
 */
static void xdma_rx_ring_kick(struct xdma_private_common *common)
{
        struct xdma_engine *engine = common->rx_engine;
        u32 first, i, n;
        u32 ctrl;
        dma_addr_t bus;

        if (common->rx_run_len)
                return;         /* 체인 실행 중 */
        if (!common->open_cnt)
                return;         /* 전 인터페이스 down — 재시동 금지 */

        n = RX_RING_SIZE - (common->rx_head - common->rx_tail);
        if (!n)
                return;         /* 미수확 만석 — 수확 후 NAPI가 재kick */

        first = common->rx_head;
        for (i = 0; i < n; i++) {
                u32 idx = (first + i) & RX_RING_MASK;

                common->rx_res[idx].status = 0;
                common->rx_res[idx].length = 0;
                ctrl = DESC_MAGIC | XDMA_DESC_COMPLETED;
                if (i == n - 1)
                        ctrl |= XDMA_DESC_STOPPED;
                common->rx_ring[idx].control = cpu_to_le32(ctrl);
        }
        dma_wmb();

        ioread32(&engine->regs->status_rc);     /* 이전 run 상태 클리어 */
        bus = rx_ring_desc_bus(common, first);
        iowrite32(cpu_to_le32(PCI_DMA_L(bus)), &engine->sgdma_regs->first_desc_lo);
        iowrite32(cpu_to_le32(PCI_DMA_H(bus)), &engine->sgdma_regs->first_desc_hi);
        iowrite32(0, &engine->sgdma_regs->first_desc_adjacent);
        iowrite32(DMA_ENGINE_START, &engine->regs->control);

        common->rx_head += n;
        common->rx_run_len = n;
}

/* 슬롯 1개 수확: skb 생성 -> 타임스탬프/FRER -> 스택 전달. NAPI(softirq) 컨텍스트. */
static void xdma_rx_process_slot(struct xdma_private_common *common, u32 idx, u32 length)
{
        struct xdma_dev *xdev = common->xdev;
        u8 *slot_buf = common->rx_bufs + (size_t)idx * XDMA_RX_BUFFER_SIZE;
        struct rx_buffer *rx_buf = (struct rx_buffer *)slot_buf;
        struct net_device *ndev = xdev->ndev[common->rx_port];
        struct xdma_pci_dev *xpdev;
        struct ptp_device_data *ptp_data;
        struct sk_buff *skb;
        unsigned long ptp_flag;
        /* se_mode: 코어→CPU 경로는 FCS를 이미 제거하고 준다(2026-08-03 ILA 실측:
         * 242B 프레임이 FCS 없이 완전체로 도달). TSNv3 관례의 CRC_LEN 차감을 그대로
         * 두면 진짜 페이로드 4B가 잘려 100B+ 프레임 전멸(IP truncated). */
        int skb_len = (int)length - RX_METADATA_SIZE - (se_mode ? 0 : CRC_LEN);

        if (unlikely(skb_len <= 0 || skb_len > XDMA_RX_BUFFER_SIZE - RX_METADATA_SIZE)) {
                pr_warn_ratelimited("rx: invalid writeback length %u\n", length);
                ndev->stats.rx_errors++;
                return;
        }

        skb = napi_alloc_skb(&common->rx_napi, skb_len);
        if (unlikely(!skb)) {
                ndev->stats.rx_dropped++;
                return;
        }
        memcpy(skb_put(skb, skb_len), slot_buf + RX_METADATA_SIZE, skb_len);

        if (se_mode && filter_rx_timestamp(common, skb)) {
                /* HAT 모드: rx_metadata.timestamp = 코어→CPU 헤더 유래
                 * {ts_sec[63:32], ts_ns[31:0]} (se_rx_host_adapter가 팩) */
                u64 se_ts = rx_buf->metadata.timestamp;
                skb_hwtstamps(skb)->hwtstamp =
                        ktime_set((s64)(se_ts >> 32), (u32)(se_ts & 0xFFFFFFFF));
        } else if (filter_rx_timestamp(common, skb)) {
                xpdev = dev_get_drvdata(&xdev->pdev->dev);
                ptp_data = xpdev ? xpdev->ptp : NULL;
                if (ptp_data) {
                        alinx_get_sys_clock_by_xdev(xdev);
                        spin_lock_irqsave(&ptp_data->lock, ptp_flag);
                        /* RX 타임스탬프: 캡처 hi도 FSM 땜질 카운터 경유라 오염
                         * 가능 -> lo만 취해 SW hi 재구성(7층 우회) */
                        skb_hwtstamps(skb)->hwtstamp = alinx_get_rx_timestamp(xdev->pdev,
                                alinx_rebuild_sysclock_by_xdev(xdev,
                                        (u32)rx_buf->metadata.timestamp));
                        spin_unlock_irqrestore(&ptp_data->lock, ptp_flag);
                }
        }

        /* FRER (802.1CB): R-TAG 처리 + 중복 제거 */
        if (xdev->tsn_config.frer && xdev->tsn_config.frer->enabled) {
                int frer_result = frer_process_rtag(skb, xdev->tsn_config.frer,
                                                    common->rx_port);
                if (frer_result == FRER_DROP_DUPLICATE ||
                    frer_result == FRER_DROP_OUT_OF_WINDOW ||
                    frer_result == FRER_DROP_NOT_FOR_US) {
                        dev_kfree_skb(skb);
                        return;
                }
                if (frer_result != FRER_NO_RTAG)
                        ndev = xdev->ndev[XDMA_FRER_PORT_ID];
        }

        skb->dev = ndev;
        skb->protocol = eth_type_trans(skb, ndev);
        ndev->stats.rx_packets++;
        ndev->stats.rx_bytes += skb_len;
        netif_receive_skb(skb);
}

/* ISR C2H 완료 훅: NAPI로 위임 (C2H 채널 IRQ는 ISR 진입부에서 이미 disable됨) */
void xdma_rx_completion_irq(struct xdma_private_common *common)
{
        napi_schedule(&common->rx_napi);
}

static int xdma_rx_napi_poll(struct napi_struct *napi, int budget)
{
        struct xdma_private_common *common =
                container_of(napi, struct xdma_private_common, rx_napi);
        struct xdma_engine *engine = common->rx_engine;
        struct xdma_dev *xdev = common->xdev;
        unsigned long flags;
        u32 status, cmpl;
        int work = 0;
        bool more;

        /* 수확: writeback(status != 0)이 도착한 슬롯을 순서대로 처리.
         * tail/슬롯 내용의 소비자는 NAPI 단독이라 락 불필요. */
        while (work < budget && common->rx_tail != common->rx_head) {
                u32 idx = common->rx_tail & RX_RING_MASK;
                u32 st = le32_to_cpu(READ_ONCE(common->rx_res[idx].status));

                if (!st)
                        break;  /* 아직 미완료 */
                if (unlikely((st >> 16) != C2H_WB))
                        pr_warn_ratelimited("rx: unexpected wb status 0x%08x\n", st);
                dma_rmb();      /* status 관측 후에 데이터/길이를 읽는다 */
                xdma_rx_process_slot(common, idx,
                                     le32_to_cpu(common->rx_res[idx].length));
                common->rx_res[idx].status = 0;
                common->rx_tail++;
                work++;
        }

        spin_lock_irqsave(&common->rx_lock, flags);
        if (common->rx_run_len) {
                status = ioread32(&engine->regs->status_rc);    /* read-clear */
                cmpl = ioread32(&engine->regs->completed_desc_count);
                if ((status & XDMA_STAT_DESC_STOPPED) || cmpl >= common->rx_run_len) {
                        if (unlikely(status & XDMA_STAT_COMMON_ERR_MASK))
                                pr_err_ratelimited("rx engine error status 0x%08x\n",
                                                   status);
                        /* 체인 소진 — run 비트 내리고 즉시 재제출 */
                        common->rx_run_len = 0;
                        xdma_engine_stop_sync(engine);
                        xdma_rx_ring_kick(common);
                }
        } else {
                xdma_rx_ring_kick(common);      /* 유휴 + 빈 슬롯 회복 시 재시동 */
        }
        more = common->rx_tail != common->rx_head &&
               READ_ONCE(common->rx_res[common->rx_tail & RX_RING_MASK].status) != 0;
        spin_unlock_irqrestore(&common->rx_lock, flags);

        if (work == budget || more)
                return budget;  /* 계속 폴링 (채널 IRQ는 꺼진 상태 유지) */

        napi_complete_done(napi, work);
        channel_interrupts_enable(xdev, xdev->mask_irq_c2h);

        /* 재확인: IRQ 재활성화 직전에 도착한 완료를 놓치지 않는다 */
        if (READ_ONCE(common->rx_res[common->rx_tail & RX_RING_MASK].status) != 0)
                napi_schedule(napi);

        return work;
}

int xdma_rx_ring_alloc(struct xdma_private_common *common)
{
        struct pci_dev *pdev = common->pdev;
        u32 i;

        /* Keep the host C2H descriptor allocation larger than every stream
         * the in-repo RTL is allowed to emit. This is a pre-DMA safety gate;
         * xdma_rx_process_slot() can only validate after the write happened. */
        BUILD_BUG_ON(XDMA_RX_BUFFER_SIZE <
                     XDMA_SE_RX_PAYLOAD_MAX + RX_METADATA_SIZE);

        common->rx_ring = dma_alloc_coherent(&pdev->dev,
                        RX_RING_SIZE * sizeof(struct xdma_desc),
                        &common->rx_ring_bus, GFP_KERNEL);
        if (!common->rx_ring)
                return -ENOMEM;

        common->rx_res = dma_alloc_coherent(&pdev->dev,
                        RX_RING_SIZE * sizeof(struct xdma_result),
                        &common->rx_res_bus, GFP_KERNEL);
        if (!common->rx_res)
                goto err_res;

        common->rx_bufs = dma_alloc_coherent(&pdev->dev,
                        RX_RING_SIZE * (size_t)XDMA_RX_BUFFER_SIZE,
                        &common->rx_bufs_bus, GFP_KERNEL);
        if (!common->rx_bufs)
                goto err_bufs;

        memset(common->rx_ring, 0, RX_RING_SIZE * sizeof(struct xdma_desc));
        memset(common->rx_res, 0, RX_RING_SIZE * sizeof(struct xdma_result));
        for (i = 0; i < RX_RING_SIZE; i++) {
                struct xdma_desc *desc = &common->rx_ring[i];
                dma_addr_t next = rx_ring_desc_bus(common, i + 1);
                dma_addr_t res = common->rx_res_bus +
                                 (dma_addr_t)i * sizeof(struct xdma_result);
                dma_addr_t buf = common->rx_bufs_bus +
                                 (dma_addr_t)i * XDMA_RX_BUFFER_SIZE;

                desc->control = cpu_to_le32(DESC_MAGIC | XDMA_DESC_STOPPED);
                desc->bytes = cpu_to_le32(XDMA_RX_BUFFER_SIZE);
                /* C2H ST: src_addr = 슬롯별 writeback 결과 주소 */
                desc->src_addr_lo = cpu_to_le32(PCI_DMA_L(res));
                desc->src_addr_hi = cpu_to_le32(PCI_DMA_H(res));
                desc->dst_addr_lo = cpu_to_le32(PCI_DMA_L(buf));
                desc->dst_addr_hi = cpu_to_le32(PCI_DMA_H(buf));
                desc->next_lo = cpu_to_le32(PCI_DMA_L(next));
                desc->next_hi = cpu_to_le32(PCI_DMA_H(next));
        }
        common->rx_head = 0;
        common->rx_tail = 0;
        common->rx_run_len = 0;
        return 0;

err_bufs:
        dma_free_coherent(&pdev->dev, RX_RING_SIZE * sizeof(struct xdma_result),
                          common->rx_res, common->rx_res_bus);
        common->rx_res = NULL;
err_res:
        dma_free_coherent(&pdev->dev, RX_RING_SIZE * sizeof(struct xdma_desc),
                          common->rx_ring, common->rx_ring_bus);
        common->rx_ring = NULL;
        return -ENOMEM;
}

void xdma_rx_ring_free(struct xdma_private_common *common)
{
        struct pci_dev *pdev = common->pdev;

        if (common->rx_bufs) {
                dma_free_coherent(&pdev->dev, RX_RING_SIZE * (size_t)XDMA_RX_BUFFER_SIZE,
                                  common->rx_bufs, common->rx_bufs_bus);
                common->rx_bufs = NULL;
        }
        if (common->rx_res) {
                dma_free_coherent(&pdev->dev, RX_RING_SIZE * sizeof(struct xdma_result),
                                  common->rx_res, common->rx_res_bus);
                common->rx_res = NULL;
        }
        if (common->rx_ring) {
                dma_free_coherent(&pdev->dev, RX_RING_SIZE * sizeof(struct xdma_desc),
                                  common->rx_ring, common->rx_ring_bus);
                common->rx_ring = NULL;
        }
}

/* 첫 open: 링 체인 제출로 RX 시동 */
void xdma_rx_ring_start(struct xdma_private_common *common)
{
        unsigned long flags;

        spin_lock_irqsave(&common->rx_lock, flags);
        xdma_rx_ring_kick(common);
        spin_unlock_irqrestore(&common->rx_lock, flags);
}

/* 마지막 close: 엔진 정지 + 미수확분 폐기(인터페이스 down) */
void xdma_rx_ring_stop(struct xdma_private_common *common)
{
        struct xdma_engine *engine = common->rx_engine;
        unsigned long flags;

        napi_disable(&common->rx_napi);         /* 진행 중 수확 완료 대기 */
        spin_lock_irqsave(&common->rx_lock, flags);
        iowrite32(DMA_ENGINE_STOP, &engine->regs->control);
        ioread32(&engine->regs->status_rc);
        common->rx_run_len = 0;
        common->rx_tail = common->rx_head;
        spin_unlock_irqrestore(&common->rx_lock, flags);
        napi_enable(&common->rx_napi);
}

/* probe에서 NAPI 등록용 (ndev[0]에 부착) */
void xdma_rx_napi_setup(struct xdma_private_common *common, struct net_device *ndev)
{
        netif_napi_add(ndev, &common->rx_napi, xdma_rx_napi_poll);
        napi_enable(&common->rx_napi);
}

void xdma_rx_napi_teardown(struct xdma_private_common *common)
{
        napi_disable(&common->rx_napi);
        netif_napi_del(&common->rx_napi);
}

int xdma_netdev_open(struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        int i;

        netif_carrier_on(ndev);
        netif_start_queue(ndev);
        for (i = 0; i < TX_QUEUE_COUNT; i++) {
                netif_start_subqueue(ndev, i);
        }

        if (common->open_cnt++ > 0) {
                return 0;
        }

        /* A2: RX 링 체인 제출로 시동 */
        xdma_rx_ring_start(common);

        return 0;
}

int xdma_netdev_close(struct net_device *ndev)
{
        int i;
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;

        netif_stop_queue(ndev);
        for (i = 0; i < TX_QUEUE_COUNT; i++) {
                netif_stop_subqueue(ndev, i);
        }
        /* A2: 마지막 close에서만 RX 엔진 정지 (구 코드는 매 close마다 정지 —
         * 다른 포트가 아직 up인데 RX가 죽는 결함이었다) */
        if (--common->open_cnt == 0)
                xdma_rx_ring_stop(common);
        pr_info("xdma_netdev_close\n");
        netif_carrier_off(ndev);
        return 0;
}

netdev_tx_t xdma_netdev_start_xmit(struct sk_buff *skb,
                struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        struct xdma_dev *xdev = common->xdev;
        sysclock_t sys_count, sys_count_upper, sys_count_lower;
        timestamp_t now;
        u16 frame_length;
        dma_addr_t dma_addr;
        struct tx_buffer* tx_buffer;
        struct tx_metadata* tx_metadata;
        u32 to_value;
        unsigned long flags;
        bool tstamp_acquired = false;
        bool skb_was_shared = false;


        /* Check desc count */
        xdma_debug("xdma_netdev_start_xmit(skb->len : %d)\n", skb->len);

        /* 공유 skb 방어 (pktgen/AF_PACKET): 이 드라이버는 skb를 변형(패딩 +
         * 메타데이터 push)하므로 users>1인 skb를 그대로 다루면 pskb_expand_head
         * 의 BUG()로 xmit 스레드가 TX 큐 락을 쥔 채 죽는다(2026-07-25 pktgen
         * 실측 — 즉시 웨지). 공유 상태면 사본으로 교체해 진행한다. */
        if (unlikely(skb_shared(skb))) {
                skb = skb_share_check(skb, GFP_ATOMIC);
                if (unlikely(!skb))
                        return NETDEV_TX_OK;
                skb_was_shared = true;
        }

        /* A1: 링 고수위 사전 정지 — skb 변형(메타데이터 push/R-TAG) 전에
         * BUSY를 돌려줘야 재큐 시 이중 변형이 없다. */
        spin_lock_irqsave(&common->tx_lock, flags);
        if (unlikely(tx_ring_free(common) < 8)) {
                xdma_stop_all_queues(xdev);
                if (tx_ring_free(common) == 0) {
                        spin_unlock_irqrestore(&common->tx_lock, flags);
                        /* 사본으로 교체된 skb는 재큐 주체가 원본을 들고 있어
                         * BUSY를 돌려주면 사본이 샌다 -> drop으로 처리 */
                        if (skb_was_shared) {
                                dev_kfree_skb_any(skb);
                                ndev->stats.tx_dropped++;
                                return NETDEV_TX_OK;
                        }
                        return NETDEV_TX_BUSY;
                }
        }
        spin_unlock_irqrestore(&common->tx_lock, flags);

        frame_length = max((unsigned int)ETH_ZLEN, skb->len);
        frame_length = workaround_packet_size(frame_length);

        if (skb_put_padto(skb, frame_length)) {
                return NETDEV_TX_OK;
        }

        /* Jumbo frames not supported */
        if (skb->len > XDMA_BUFFER_SIZE) {
#ifdef __LIBXDMA_DEBUG__
                pr_err("Jumbo frames not supported\n");
#endif
                dev_kfree_skb(skb);
                return NETDEV_TX_OK;
        }

        /* Add metadata to the skb.
         * A3 (2026-07-26): 무조건 pskb_expand_head(전 프레임 재할당+512~1500B
         * 복사)가 프레임당 ~1us 고정비였다. 헤드룸이 충분하고 데이터가 공유
         * (clone)되지 않았으면 재할당 없이 그대로 push한다(표준 skb_cow_head
         * 패턴). ndev->needed_headroom 설정으로 스택 발신 skb는 대부분 무복사. */
        {
                unsigned int need_tail =
                        (priv->port_flag & XDMA_PORT_FLAG_FRER) ? FRER_RTAG_SIZE : 0;
                if (skb_header_cloned(skb) ||
                    skb_headroom(skb) < TX_METADATA_SIZE ||
                    skb_tailroom(skb) < need_tail) {
                        if (pskb_expand_head(skb, TX_METADATA_SIZE, need_tail,
                                             GFP_ATOMIC) != 0) {
#ifdef __LIBXDMA_DEBUG__
                                pr_err("pskb_expand_head failed\n");
#endif
                                dev_kfree_skb(skb);
                                return NETDEV_TX_OK;
                        }
                }
        }
        skb_push(skb, TX_METADATA_SIZE);
        memset(skb->data, 0, TX_METADATA_SIZE);

        xdma_debug("skb->len : %d\n", skb->len);
        tx_buffer = (struct tx_buffer*)skb->data;
        /* Fill in the metadata */
        tx_metadata = (struct tx_metadata*)&tx_buffer->metadata;
        tx_metadata->frame_length = frame_length;

        /* HAT 모드: 셰이핑/타임스탬프/FRER는 TSN 코어 담당 —
         * sysclock 읽기·tsn_fill_metadata·TX-TS 슬롯·R-TAG 삽입 전부 우회.
         * 메타는 frame_length + DEST_PORT만 유효(meta1=0은 ptp_mux가 폐기).
         * reserved0[0] = CPU-Port 헤더 DEST_PORT 슬롯(metadata2[87:80]) — 0이면
         * ptp_mux 가 DST_PORT_DFLT(=1)로 대체하므로 단일 포트로만 나간다.
         * se_tx_dest_port 로 그 슬롯을 채우면 전 포트(0x80) 송신이 되어 코어 FRER
         * 이중경로에 호스트 프레임이 실린다(파라미터 주석의 실측 근거 참조).
         * BAR는 코어 APB라 구 레지스터 접근은 전부 무효 주소이므로 반드시 우회. */
        if (se_mode) {
                if (se_tx_dest_port)
                        tx_metadata->reserved0[0] = (uint8_t)se_tx_dest_port;
                goto se_enqueue;
        }

        /* A3 (2026-07-26): 핫패스는 캐시 외삽 — 프레임당 BAR read 3회(~5us)가
         * 84kpps 병목의 주범이었다(pktgen 실측 11.8us/프레임). 정확도는 1ms
         * 리싱크로 충분(오차 <=50ns, 게이팅 창 대비 무시 가능). */
        sys_count = alinx_get_sys_clock_fast_by_xdev(xdev);
        now = alinx_sysclock_to_timestamp(common->pdev, sys_count);
        sys_count_lower = sys_count & LOWER_29_BITS;
        sys_count_upper = sys_count & ~LOWER_29_BITS;


	/* Set the fromtick & to_tick values based on the lower 29 bits of the system count */
	if (tsn_fill_metadata(xdev, now, skb) == false) {
#ifdef __LIBXDMA_DEBUG__
		pr_warn("tsn_fill_metadata failed\n");
#endif
                /* HW Queue is almost full, but there is still some room left */
                /* Next frames have to wait for the queue to be available */
                /*
                 * Track B (2026-07-08) 규명 메모: 이 stop 은 BE(비-HWtstamp)
                 * 경로에서 함수 끝 !tstamp_acquired 의 xdma_start_all_queues 로
                 * 곧바로 무효화된다(자기무효 백프레셔). 그러나 실측 결과 admission
                 * drop 으로 바꿔도 오염이 재현됐다: is_buffer_available 가 읽는
                 * FBW_ADDR_FIFO_CNT(fifocnt)가 지속 과부하 중 실시간 점유를
                 * 반영하지 못해(blast 15s 동안 tx_dropped=0) 드라이버가 "풀"을
                 * 감지조차 못 한다. 즉 현 비트스트림에선 드라이버 단독 방지가
                 * 불가하며, 근본 해소는 RTL(Frame Buffer Writer 의 AXI-Stream
                 * 백프레셔=tready) 몫이다(HP 트랙). 현 비트 완화책 = SW 라인레이트
                 * 페이싱. 상세: doc\11.
                 */
                xdma_stop_all_queues(xdev);
	}

	/* Acquire TX timestamp slot early, before FRER / DMA / enqueue.
	 * If the slot is busy, undo metadata and return NETDEV_TX_BUSY so
	 * the qdisc retries later — no FRER or DMA state to roll back. */
	if ((skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) &&
	    common->tstamp_config.tx_type == HWTSTAMP_TX_ON) {
		if (test_and_set_bit_lock(tx_metadata->timestamp_id,
					  &common->state)) {
			pr_debug_ratelimited("Timestamp busy (id=%u): retry\n",
					     tx_metadata->timestamp_id);
			if (skb_was_shared) {
				dev_kfree_skb_any(skb);
				ndev->stats.tx_dropped++;
				return NETDEV_TX_OK;
			}
			skb_pull(skb, TX_METADATA_SIZE);
			return NETDEV_TX_BUSY;
		}
		tstamp_acquired = true;
	}

	/* FRER (802.1CB): Insert R-TAG with auto stream registration */
	if (xdev->tsn_config.frer && xdev->tsn_config.frer->enabled && (priv->port_flag & XDMA_PORT_FLAG_FRER)) {
		struct ethhdr *eth = (struct ethhdr *)(tx_buffer->data);
		struct frer_stream *stream;
		unsigned long frer_flags;
		
		spin_lock_irqsave(&xdev->tsn_config.frer->lock, frer_flags);
		stream = frer_stream_lookup(xdev->tsn_config.frer, 
					    eth->h_source, eth->h_dest);
		
		/* Auto-register stream if not found */
		if (!stream)
			stream = frer_auto_register_stream(xdev->tsn_config.frer,
							   eth->h_source, eth->h_dest);
		
		if (!stream) {
			pr_err_ratelimited("FRER: Failed to get/create stream - dropping packet\\n");
			spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
			if (tstamp_acquired)
				clear_bit_unlock(tx_metadata->timestamp_id,
						 &common->state);
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		} else if (stream->seq_gen.active) {
			/* Use TX-aware R-TAG insertion */
			if (frer_insert_rtag_tx(skb, stream, TX_METADATA_SIZE, frame_length) < 0) {
				pr_err("FRER: Failed to insert R-TAG (tailroom=%d, need=%lu)\\n",
				       skb_tailroom(skb), FRER_RTAG_SIZE);
				spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
				if (tstamp_acquired)
					clear_bit_unlock(tx_metadata->timestamp_id,
							 &common->state);
				dev_kfree_skb(skb);
				return NETDEV_TX_OK;
			}
			/* Update frame length after R-TAG insertion */
			tx_metadata->frame_length += FRER_RTAG_SIZE;
			frame_length += FRER_RTAG_SIZE;
		} else {
			pr_err_ratelimited("FRER: Stream exists but seq_gen not active - dropping packet\\n");
			spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
			if (tstamp_acquired)
				clear_bit_unlock(tx_metadata->timestamp_id,
						 &common->state);
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		}
		spin_unlock_irqrestore(&xdev->tsn_config.frer->lock, frer_flags);
	}

        xdma_debug("0x%08x  0x%08x  0x%08x  %4d  %1d",
                sys_count_lower, tx_metadata->from.tick, tx_metadata->to.tick,
                tx_metadata->frame_length, tx_metadata->fail_policy);
        dump_buffer((unsigned char*)tx_metadata, (int)(sizeof(struct tx_metadata) + skb->len));

se_enqueue:

        if (tx_bounce) {
                /* streaming map 없음 — 실제 복사·주소는 enqueue에서(슬롯 idx 확정 후).
                 * 여기선 프레임이 슬롯 버퍼에 들어가는지만 보장한다. */
                if (unlikely(skb->len > TX_BOUNCE_BUF_SIZE)) {
                        if (tstamp_acquired)
                                clear_bit_unlock(tx_metadata->timestamp_id,
                                                 &common->state);
                        dev_kfree_skb_any(skb);
                        ndev->stats.tx_dropped++;
                        return NETDEV_TX_OK;
                }
                dma_addr = 0;
        } else {
                dma_addr = dma_map_single(&xdev->pdev->dev, skb->data, skb->len,
                                          DMA_TO_DEVICE);
                if (unlikely(dma_mapping_error(&xdev->pdev->dev, dma_addr))) {
                        pr_err("dma_map_single failed\n");
                        if (tstamp_acquired)
                                clear_bit_unlock(tx_metadata->timestamp_id,
                                                 &common->state);
                        dev_kfree_skb_any(skb);
                        ndev->stats.tx_dropped++;
                        return NETDEV_TX_OK;
                }
        }

        if (tstamp_acquired) {
                skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
                common->tx_work_skb[tx_metadata->timestamp_id] = skb_get(skb);
                common->tx_work_start_after[tx_metadata->timestamp_id] = sys_count_upper | tx_metadata->from.tick;
                if (sys_count_lower > tx_metadata->from.tick && sys_count_lower - tx_metadata->from.tick > TX_WORK_OVERFLOW_MARGIN) {
                        common->tx_work_start_after[tx_metadata->timestamp_id] += (1 << 29);
                }
                to_value = (tx_metadata->fail_policy == TSN_FAIL_POLICY_RETRY ? tx_metadata->delay_to.tick : tx_metadata->to.tick);
                common->tx_work_wait_until[tx_metadata->timestamp_id] = sys_count_upper | to_value;
                if (sys_count_lower > to_value && sys_count_lower - to_value > TX_WORK_OVERFLOW_MARGIN) {
                        common->tx_work_wait_until[tx_metadata->timestamp_id] += (1 << 29);
                }
                schedule_work(&common->tx_work[tx_metadata->timestamp_id]);
        }

        /* A1: 링 적재 (+엔진 유휴 시 체인 kick). 고수위 사전정지 때문에
         * 여기서의 만석은 희귀 레이스 — BUSY 재큐는 이중 변형 위험이 있어
         * drop으로 처리한다. */
        spin_lock_irqsave(&common->tx_lock, flags);
        /* FRER 특수 포트는 포트 먹스를 건드리지 않는다(와일드카드 -1) */
        if (unlikely(!xdma_tx_ring_enqueue(common, skb, dma_addr,
                        priv->port_id == XDMA_FRER_PORT_ID ? -1 : (s8)priv->port_id))) {
                spin_unlock_irqrestore(&common->tx_lock, flags);
                if (!tx_bounce)
                        dma_unmap_single(&xdev->pdev->dev, dma_addr, skb->len,
                                         DMA_TO_DEVICE);
                if (tstamp_acquired) {
                        struct sk_buff *ts_skb = common->tx_work_skb[tx_metadata->timestamp_id];
                        common->tx_work_skb[tx_metadata->timestamp_id] = NULL;
                        if (ts_skb)
                                dev_kfree_skb_any(ts_skb);
                        clear_bit_unlock(tx_metadata->timestamp_id, &common->state);
                }
                dev_kfree_skb_any(skb);
                ndev->stats.tx_dropped++;
                return NETDEV_TX_OK;
        }
        to_value = tx_ring_free(common);
        spin_unlock_irqrestore(&common->tx_lock, flags);

        /* 구 모델의 BE 경로 자기무효 백프레셔 의미 보존: tsn_fill_metadata의
         * stop을 즉시 되돌리되, 링 고수위 stop은 유지한다(NAPI가 회복 시 wake). */
        if (!tstamp_acquired && to_value >= TX_RING_WAKE_THRESH)
                xdma_start_all_queues(xdev);

        return NETDEV_TX_OK;
}

u16 xdma_select_queue(struct net_device *ndev, struct sk_buff *skb, struct net_device *sb_dev) {
        /* Always use the first subqueue */
        return 0;
}

static LIST_HEAD(xdma_block_cb_list);

static int xdma_setup_tc_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv) {
        // If mqprio is only used for queue mapping this should not be called
        return -EOPNOTSUPP;
}

int xdma_netdev_setup_tc(struct net_device *ndev, enum tc_setup_type type, void *type_data) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        // TODO: TSN config per port

        switch (type) {
        case TC_SETUP_QDISC_MQPRIO:
                return tsn_set_mqprio(common->pdev, (struct tc_mqprio_qopt_offload*)type_data);
        case TC_SETUP_QDISC_CBS:
                return tsn_set_qav(common->pdev, (struct tc_cbs_qopt_offload*)type_data);
        case TC_SETUP_QDISC_TAPRIO:
                return tsn_set_qbv(common->pdev, (struct tc_taprio_qopt_offload*)type_data);
        case TC_SETUP_BLOCK:
                return flow_block_cb_setup_simple(type_data, &xdma_block_cb_list, xdma_setup_tc_block_cb, priv, priv, true);
        default:
                return -ENOTSUPP;
        }

        return 0;
}

static int xdma_get_ts_config(struct net_device *ndev, struct ifreq *ifr) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        struct hwtstamp_config *config = &common->tstamp_config;

        return copy_to_user(ifr->ifr_data, config, sizeof(*config)) ? -EFAULT : 0;
}

static int xdma_set_ts_config(struct net_device *ndev, struct ifreq *ifr) {
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        struct hwtstamp_config *config = &common->tstamp_config;

        return copy_from_user(config, ifr->ifr_data, sizeof(*config)) ? -EFAULT : 0;
}

static int frer_ioctl_add_stream(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	struct frer_stream_config config;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&config, data, sizeof(config))) {
		return -EFAULT;
	}

	return frer_add_stream(xdev->tsn_config.frer, &config);
}

static int frer_ioctl_del_stream(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	struct frer_stream_id id;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&id, data, sizeof(id))) {
		return -EFAULT;
	}

	return frer_del_stream(xdev->tsn_config.frer, &id);
}

static int frer_ioctl_get_stats(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	struct frer_stream_stats stats;
	int ret;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&stats, data, sizeof(stats))) {
		return -EFAULT;
	}

	ret = frer_get_stream_stats(xdev->tsn_config.frer, &stats);
	if (ret) {
		return ret;
	}

	if (copy_to_user(data, &stats, sizeof(stats))) {
		return -EFAULT;
	}

	return 0;
}

static int frer_ioctl_enable(struct net_device *ndev, struct ifreq *ifr, void *data) {
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_private_common *common = priv->common;
	struct xdma_dev *xdev = common->xdev;
	int enable;

	if (!xdev->tsn_config.frer) {
		return -ENODEV;
	}

	if (copy_from_user(&enable, data, sizeof(enable))) {
		return -EFAULT;
	}

	xdev->tsn_config.frer->enabled = (enable != 0);
	pr_info("FRER: %s\n", xdev->tsn_config.frer->enabled ? "enabled" : "disabled");

	return 0;
}

int xdma_netdev_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd) {
	switch (cmd) {
	case SIOCGHWTSTAMP:
		return xdma_get_ts_config(ndev, ifr);
	case SIOCSHWTSTAMP:
		return xdma_set_ts_config(ndev, ifr);
	default:
		return -EOPNOTSUPP;
	}
}

int xdma_netdev_siocdevprivate(struct net_device *ndev, struct ifreq *ifr, void *data, int cmd) {
	switch (cmd) {
	case SIOC_FRER_ADD_STREAM:
		return frer_ioctl_add_stream(ndev, ifr, data);
	case SIOC_FRER_DEL_STREAM:
		return frer_ioctl_del_stream(ndev, ifr, data);
	case SIOC_FRER_GET_STATS:
		return frer_ioctl_get_stats(ndev, ifr, data);
	case SIOC_FRER_ENABLE:
		return frer_ioctl_enable(ndev, ifr, data);
	default:
		return -EOPNOTSUPP;
	}
}

static void do_tx_work(struct work_struct *work, u16 tstamp_id) {
        sysclock_t tx_tstamp;
        struct xdma_pci_dev *xpdev;
        struct ptp_device_data *ptp_data;
        struct skb_shared_hwtstamps shhwtstamps;
        struct xdma_private_common* common = container_of(work - tstamp_id, struct xdma_private_common, tx_work[0]);
        struct sk_buff* skb = common->tx_work_skb[tstamp_id];
        sysclock_t now;
        unsigned long ptp_flag;
        int retry;

        if (tstamp_id >= TSN_TIMESTAMP_ID_MAX) {
                pr_err("Invalid timestamp ID\n");
                return;
        }

        if (!skb) {
                goto return_error;
        }

        for (retry = 0; retry < TX_TSTAMP_MAX_RETRY; retry++) {
                now = alinx_read_sys_clock_raw(common->xdev);

                tx_tstamp = alinx_read_tx_timestamp_by_xdev(common->xdev, tstamp_id);
                if (tx_tstamp >= common->tx_work_start_after[tstamp_id] &&
                    tx_tstamp <= now) {
                        now = alinx_get_sys_clock_by_xdev(common->xdev);
                        goto got_tstamp;
                }

                if (now > common->tx_work_wait_until[tstamp_id] &&
                    (now - common->tx_work_wait_until[tstamp_id]) > TX_TSTAMP_TIMEOUT_MARGIN)
                        break;

                usleep_range(TX_TSTAMP_POLL_US, TX_TSTAMP_POLL_US * 2);
        }

        pr_warn("Failed to get timestamp: id=%u, cur=0x%010llx, " \
                "now=0x%010llx, start=0x%010llx, wait=0x%010llx\n",
                tstamp_id, tx_tstamp,
                now, common->tx_work_start_after[tstamp_id],
                common->tx_work_wait_until[tstamp_id]);
        goto return_error;

got_tstamp:
        xpdev = dev_get_drvdata(&common->xdev->pdev->dev);
        ptp_data = xpdev->ptp;
        if (!ptp_data) {
                pr_err("Invalid ptp_data\n");
                goto return_error;
        }

        spin_lock_irqsave(&ptp_data->lock, ptp_flag);
        shhwtstamps.hwtstamp = ns_to_ktime(alinx_sysclock_to_txtstamp(common->pdev, tx_tstamp));
        spin_unlock_irqrestore(&ptp_data->lock, ptp_flag);
        common->last_tx_tstamp[tstamp_id] = tx_tstamp;

        common->tx_work_skb[tstamp_id] = NULL;
        skb_tstamp_tx(skb, &shhwtstamps);
        dev_kfree_skb_any(skb);
        clear_bit_unlock(tstamp_id, &common->state);
        xdma_start_all_queues(common->xdev);
        return;

return_error:
        if (skb) {
                /*
                 * Deliver a fallback PTP-clock timestamp so that userspace
                 * poll(POLLPRI) on the error queue does not stall for the full
                 * timeout.  Without this, the latency tool falls back to
                 * SystemTime::now() (wall-clock / TAI) which is in a completely
                 * different epoch from HW timestamps.
                 */
                struct skb_shared_hwtstamps fallback;
                memset(&fallback, 0, sizeof(fallback));
                xpdev = dev_get_drvdata(&common->xdev->pdev->dev);
                ptp_data = xpdev->ptp;
                if (ptp_data) {
                        spin_lock_irqsave(&ptp_data->lock, ptp_flag);
                        now = alinx_get_sys_clock_by_xdev(common->xdev);
                        fallback.hwtstamp = ns_to_ktime(
                                alinx_sysclock_to_txtstamp(common->pdev, now));
                        spin_unlock_irqrestore(&ptp_data->lock, ptp_flag);
                }
                skb_tstamp_tx(skb, &fallback);
                common->tx_work_skb[tstamp_id] = NULL;
                dev_kfree_skb_any(skb);
        }
        clear_bit_unlock(tstamp_id, &common->state);
        xdma_start_all_queues(common->xdev);
        return;
}

/*
 * Track B 결함② : Tx timestamp 슬롯 4→32 확장.
 * 각 슬롯(tstamp_id)마다 트램펄린 1개 -> do_tx_work(work, id) 호출.
 * (work_struct 콜백은 인자를 못 받으므로 id를 컴파일타임 상수로 넘김)
 */
#define DEFINE_TX_WORK(n) \
static void xdma_tx_work##n(struct work_struct *work) { \
        do_tx_work(work, n); \
}

DEFINE_TX_WORK(1);
DEFINE_TX_WORK(2);
DEFINE_TX_WORK(3);
DEFINE_TX_WORK(4);
DEFINE_TX_WORK(5);
DEFINE_TX_WORK(6);
DEFINE_TX_WORK(7);
DEFINE_TX_WORK(8);
DEFINE_TX_WORK(9);
DEFINE_TX_WORK(10);
DEFINE_TX_WORK(11);
DEFINE_TX_WORK(12);
DEFINE_TX_WORK(13);
DEFINE_TX_WORK(14);
DEFINE_TX_WORK(15);
DEFINE_TX_WORK(16);
DEFINE_TX_WORK(17);
DEFINE_TX_WORK(18);
DEFINE_TX_WORK(19);
DEFINE_TX_WORK(20);
DEFINE_TX_WORK(21);
DEFINE_TX_WORK(22);
DEFINE_TX_WORK(23);
DEFINE_TX_WORK(24);
DEFINE_TX_WORK(25);
DEFINE_TX_WORK(26);
DEFINE_TX_WORK(27);
DEFINE_TX_WORK(28);
DEFINE_TX_WORK(29);
DEFINE_TX_WORK(30);
DEFINE_TX_WORK(31);
DEFINE_TX_WORK(32);

/* INIT_WORK용 트램펄린 테이블. index = tstamp_id (1..32), [0]=미사용(NULL). */
const work_func_t xdma_tx_work_fns[TSN_TIMESTAMP_ID_MAX] = {
        [1] = xdma_tx_work1, [2] = xdma_tx_work2, [3] = xdma_tx_work3, [4] = xdma_tx_work4,
        [5] = xdma_tx_work5, [6] = xdma_tx_work6, [7] = xdma_tx_work7, [8] = xdma_tx_work8,
        [9] = xdma_tx_work9, [10] = xdma_tx_work10, [11] = xdma_tx_work11, [12] = xdma_tx_work12,
        [13] = xdma_tx_work13, [14] = xdma_tx_work14, [15] = xdma_tx_work15, [16] = xdma_tx_work16,
        [17] = xdma_tx_work17, [18] = xdma_tx_work18, [19] = xdma_tx_work19, [20] = xdma_tx_work20,
        [21] = xdma_tx_work21, [22] = xdma_tx_work22, [23] = xdma_tx_work23, [24] = xdma_tx_work24,
        [25] = xdma_tx_work25, [26] = xdma_tx_work26, [27] = xdma_tx_work27, [28] = xdma_tx_work28,
        [29] = xdma_tx_work29, [30] = xdma_tx_work30, [31] = xdma_tx_work31, [32] = xdma_tx_work32,
};
