#pragma once

#include <linux/skbuff.h>
#include <linux/pci.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>

typedef uint64_t timestamp_t;
typedef uint64_t sysclock_t;

enum tsn_timestamp_id {
	TSN_TIMESTAMP_ID_NONE = 0,
	TSN_TIMESTAMP_ID_GPTP = 1,
	TSN_TIMESTAMP_ID_NORMAL = 2,
	/*
	 * Track B 결함② : Tx timestamp 슬롯 4→32 확장.
	 * 유효 id = 1..32 (1=gPTP, 2..32=normal flow, 총 31 normal 슬롯).
	 * HW(AXI4L_Slave_IF_ext)가 슬롯 32개 제공 → 1차 시나리오#4(30 flow)
	 * 를 조건 축소 없이 측정 가능. id 배정은 tsn.c에서 round-robin.
	 */
	TSN_TIMESTAMP_ID_MAX = 33,
};

enum tsn_prio {
	TSN_PRIO_GPTP = 3,
	TSN_PRIO_VLAN = 5,
	TSN_PRIO_BE = 7,
};

enum tsn_fail_policy {
	TSN_FAIL_POLICY_DROP = 0,
	TSN_FAIL_POLICY_RETRY = 1,
};

struct tsn_vlan_hdr {
	uint16_t pid;
	uint8_t pcp:3;
	uint8_t dei:1;
	uint16_t vid:12;
} __attribute__((packed, scalar_storage_order("big-endian")));

bool tsn_fill_metadata(struct xdma_dev* xdev, timestamp_t now, struct sk_buff* skb);
void tsn_init_configs(struct pci_dev* pdev);
void tsn_cleanup_configs(struct pci_dev* pdev);

int tsn_set_mqprio(struct pci_dev* pdev, struct tc_mqprio_qopt_offload* offload);
int tsn_set_qav(struct pci_dev* pdev, struct tc_cbs_qopt_offload* offload);
int tsn_set_qbv(struct pci_dev* pdev, struct tc_taprio_qopt_offload* offload);
