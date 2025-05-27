#include <linux/if_ether.h>
#include <linux/string.h>

#include "alinx_ptp.h"
#include "alinx_arch.h"
#include "xdma_netdev.h"
#include "libxdma.h"
#include "tsn.h"

#define NS_IN_1S 1000000000

#define TSN_ALWAYS_OPEN(from) (from - 1) /* For both timestamp and sysclock */

struct timestamps {
	timestamp_t from;
	timestamp_t to;
	timestamp_t delay_from;
	timestamp_t delay_to;
};

static bool is_gptp_packet(const uint8_t* payload);
static void bake_qos_config(struct tsn_config* config);
static uint64_t bytes_to_ns(uint64_t bytes);
static void spend_qav_credit(struct tsn_config* tsn_config, timestamp_t at, uint8_t tc_id, uint64_t bytes);
static bool get_timestamps(struct timestamps* timestamps, const struct tsn_config* tsn_config, timestamp_t from, uint8_t tc_id, uint64_t bytes, bool consider_delay);

static void update_buffer(struct xdma_dev* xdev);
static void decrease_buffer_space(struct xdma_dev* xdev);
static bool is_buffer_available(struct xdma_dev* xdev, uint32_t min_space);

static inline uint8_t tsn_get_mqprio_tc(struct net_device* ndev, uint8_t prio) {
	if (netdev_get_num_tc(ndev) == 0) {
		return prio;
	}

	return netdev_get_prio_tc_map(ndev, prio);
}

static uint8_t tsn_get_vlan_prio(struct tsn_config* tsn_config, struct sk_buff* skb) {
	struct tx_buffer* tx_buf = (struct tx_buffer*)skb->data;
	struct ethhdr* eth = (struct ethhdr*)(tx_buf->data);
	uint16_t eth_type = ntohs(eth->h_proto);
	if (eth_type == ETH_P_8021Q) {
		struct tsn_vlan_hdr* vlan = (struct tsn_vlan_hdr*)(tx_buf->data + ETH_HLEN - ETH_TLEN);  // eth->h_proto == vlan->pid
		return vlan->pcp;
	}
	//XXX: Or you can use skb->priority;
	return 0;
}

static bool is_gptp_packet(const uint8_t* payload) {
	struct ethhdr* eth = (struct ethhdr*)payload;
	uint16_t eth_type = ntohs(eth->h_proto);
	if (eth_type == ETH_P_8021Q) {
		struct tsn_vlan_hdr* vlan = (struct tsn_vlan_hdr*)(eth + 1);
		eth_type = vlan->pid;
	}

	return eth_type == ETH_P_1588;
}

static inline sysclock_t tsn_timestamp_to_sysclock(struct pci_dev* pdev, timestamp_t timestamp) {
	return alinx_timestamp_to_sysclock(pdev, timestamp - TX_ADJUST_NS) - PHY_DELAY_CLOCKS;
}

/**
 * Fill in the time related metadata of a frame
 * @param tsn_config: TSN configuration
 * @param now: Current time
 * @param tx_buf: The frame to be sent
 * @return: true if the frame reserves timestamps, false is for drop
 */
bool tsn_fill_metadata(struct pci_dev* pdev, timestamp_t now, struct sk_buff* skb) {
	uint8_t vlan_prio, tc_id;
	uint64_t duration_ns;
	bool is_gptp, consider_delay;
	timestamp_t from, free_at;
	enum tsn_prio queue_prio;
	struct timestamps timestamps;
	struct tx_buffer* tx_buf = (struct tx_buffer*)skb->data;
	struct tx_metadata* metadata = (struct tx_metadata*)&tx_buf->metadata;
	struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	struct tsn_config* tsn_config = &xdev->tsn_config;
	struct xdma_private* priv = netdev_priv(xdev->ndev);

	vlan_prio = tsn_get_vlan_prio(tsn_config, skb);
	tc_id = tsn_get_mqprio_tc(xdev->ndev, vlan_prio);
	is_gptp = is_gptp_packet(tx_buf->data);

	if (is_gptp) {
		queue_prio = TSN_PRIO_GPTP;
	} else if (vlan_prio > 0) {
		queue_prio = TSN_PRIO_VLAN;
	} else {
		queue_prio = TSN_PRIO_BE;
	}
	consider_delay = (queue_prio != TSN_PRIO_BE);

	from = now + H2C_LATENCY_NS;

	duration_ns = bytes_to_ns(metadata->frame_length);

	if (tsn_config->qbv.enabled == false && tsn_config->qav[tc_id].enabled == false) {
		// Don't care. Just fill in the metadata
		timestamps.from = tsn_config->total_available_at;
		timestamps.to = timestamps.from + _DEFAULT_TO_MARGIN_;
		metadata->fail_policy = TSN_FAIL_POLICY_DROP;
	} else {
		if (tsn_config->qav[tc_id].enabled == true && tsn_config->qav[tc_id].available_at > from) {
			from = tsn_config->qav[tc_id].available_at;
		}

		if (consider_delay) {
			if (!is_buffer_available(xdev, TSN_QUEUE_SIZE_PAD)) {
				return false;
			}
		} else {
			// Best effort
			if (!is_buffer_available(xdev, BE_QUEUE_SIZE_PAD)) {
				return false;
			}
			from = max(from, tsn_config->total_available_at);
		}

		get_timestamps(&timestamps, tsn_config, from, tc_id, metadata->frame_length, consider_delay);
		metadata->fail_policy = consider_delay ? TSN_FAIL_POLICY_RETRY : TSN_FAIL_POLICY_DROP;
	}

	metadata->from.tick = tsn_timestamp_to_sysclock(pdev, timestamps.from);
	metadata->from.priority = queue_prio;
	if (timestamps.to == TSN_ALWAYS_OPEN(timestamps.from)) {
		metadata->to.tick = TSN_ALWAYS_OPEN(metadata->from.tick);
	} else {
		metadata->to.tick = tsn_timestamp_to_sysclock(pdev, timestamps.to);
	}
	metadata->to.priority = queue_prio;
	metadata->delay_from.tick = tsn_timestamp_to_sysclock(pdev, timestamps.delay_from);
	metadata->delay_from.priority = queue_prio;
	metadata->delay_to.tick = tsn_timestamp_to_sysclock(pdev, timestamps.delay_to);
	metadata->delay_to.priority = queue_prio;

	if (priv->tstamp_config.tx_type != HWTSTAMP_TX_ON) {
		metadata->timestamp_id = TSN_TIMESTAMP_ID_NONE;
	} else if (is_gptp) {
		metadata->timestamp_id = TSN_TIMESTAMP_ID_GPTP;
	} else {
		metadata->timestamp_id = TSN_TIMESTAMP_ID_NORMAL;
	}

	decrease_buffer_space(xdev);
	// Update available_ats
	spend_qav_credit(tsn_config, from, tc_id, metadata->frame_length);
	tsn_config->queue_available_at[queue_prio] += duration_ns;
	tsn_config->total_available_at += duration_ns;

	free_at = max(timestamps.to + duration_ns, tsn_config->total_available_at);

	return true;
}

void tsn_init_configs(struct pci_dev* pdev) {
	struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	struct tsn_config* config = &xdev->tsn_config;
	memset(config, 0, sizeof(struct tsn_config));

	// Example Qbv configuration
	if (false) {
		config->qbv.enabled = true;
		config->qbv.start = 0;
		config->qbv.slot_count = 2;
		config->qbv.slots[0].duration_ns = 500000000; // 500ms
		config->qbv.slots[0].opened_prios[0] = true;
		config->qbv.slots[1].duration_ns = 500000000; // 500ms
		config->qbv.slots[1].opened_prios[0] = true;
	}

	// Example Qav configuration
	if (false) {
		// 100Mbps on 1Gbps link
		config->qav[0].enabled = true;
		config->qav[0].hi_credit = +1000000;
		config->qav[0].lo_credit = -1000000;
		config->qav[0].idle_slope = 10;
		config->qav[0].send_slope = -90;
	}

	bake_qos_config(config);
}

static void bake_qos_config(struct tsn_config* config) {
	int slot_id, tc_id; // Iterators
	bool qav_disabled = true;
	struct qbv_baked_config* baked;
	if (config->qbv.enabled == false) {
		// TODO: remove this when throughput issue without QoS gets resolved
		for (tc_id = 0; tc_id < TC_COUNT; tc_id++) {
			if (config->qav[tc_id].enabled) {
				qav_disabled = false;
				break;
			}
		}

		if (qav_disabled) {
			config->qbv.enabled = true;
			config->qbv.start = 0;
			config->qbv.slot_count = 1;
			config->qbv.slots[0].duration_ns = 1000000000; // 1s
			for (tc_id = 0; tc_id < TC_COUNT; tc_id++) {
				config->qbv.slots[0].opened_prios[tc_id] = true;
			}
		}
	}

	baked = &config->qbv_baked;
	memset(baked, 0, sizeof(struct qbv_baked_config));

	baked->cycle_ns = 0;

	// First slot
	for (tc_id = 0; tc_id < TC_COUNT; tc_id += 1) {
		baked->prios[tc_id].slot_count = 1;
		baked->prios[tc_id].slots[0].opened = config->qbv.slots[0].opened_prios[tc_id];
	}

	for (slot_id = 0; slot_id < config->qbv.slot_count; slot_id += 1) {
		uint64_t slot_duration = config->qbv.slots[slot_id].duration_ns;
		baked->cycle_ns += slot_duration;
		for (tc_id = 0; tc_id < TC_COUNT; tc_id += 1) {
			struct qbv_baked_prio* prio = &baked->prios[tc_id];
			if (prio->slots[prio->slot_count - 1].opened == config->qbv.slots[slot_id].opened_prios[tc_id]) {
				// Same as the last slot. Just increase the duration
				prio->slots[prio->slot_count - 1].duration_ns += slot_duration;
			} else {
				// Different from the last slot. Add a new slot
				prio->slots[prio->slot_count].opened = config->qbv.slots[slot_id].opened_prios[tc_id];
				prio->slots[prio->slot_count].duration_ns = slot_duration;
				prio->slot_count += 1;
			}
		}
	}

	// Adjust slot counts to be even number. Because we need to have open-close pairs
	for (tc_id = 0; tc_id < TC_COUNT; tc_id += 1) {
		struct qbv_baked_prio* prio = &baked->prios[tc_id];
		if (prio->slot_count % 2 == 1) {
			prio->slots[prio->slot_count].opened = !prio->slots[prio->slot_count - 1].opened;
			prio->slots[prio->slot_count].duration_ns = 0;
			prio->slot_count += 1;
		}
	}
}

static uint64_t bytes_to_ns(uint64_t bytes) {
	// TODO: Get link speed
	uint64_t link_speed = 1000000000; // Assume 1Gbps
	return max(bytes, (uint64_t)ETH_ZLEN) * 8 * NS_IN_1S / link_speed;
}

static void spend_qav_credit(struct tsn_config* tsn_config, timestamp_t at, uint8_t tc_id, uint64_t bytes) {
	uint64_t elapsed_from_last_update, sending_duration;
	double earned_credit, spending_credit;
	timestamp_t send_end;
	struct qav_state* qav = &tsn_config->qav[tc_id];

	if (qav->enabled == false) {
		return;
	}

	if (at < qav->last_update || at < qav->available_at) {
		// Invalid
		pr_err("Invalid timestamp Qav spending");
		return;
	}

	elapsed_from_last_update = at - qav->last_update;
	earned_credit = (double)elapsed_from_last_update * qav->idle_slope;
	qav->credit += earned_credit;
	if (qav->credit > qav->hi_credit) {
		qav->credit = qav->hi_credit;
	}

	sending_duration = bytes_to_ns(bytes);
	spending_credit = (double)sending_duration * qav->send_slope;
	qav->credit += spending_credit;
	if (qav->credit < qav->lo_credit) {
		qav->credit = qav->lo_credit;
	}

	// Calulate next available time
	send_end = at + sending_duration;
	qav->last_update = send_end;
	if (qav->credit < 0) {
		qav->available_at = send_end + -(qav->credit / qav->idle_slope);
	} else {
		qav->available_at = send_end;
	}
}

/**
 * Get timestamps for a frame based on Qbv configuration
 * @param timestamps: Output timestamps
 * @param tsn_config: TSN configuration
 * @param from: The time when the frame is ready to be sent
 * @param tc_id: ID of mqprio tc or VLAN priority of the frame
 * @param bytes: Size of the frame
 * @param consider_delay: If true, calculate delay_from and delay_to
 * @return: true if the frame reserves timestamps, false is for drop
 */
static bool get_timestamps(struct timestamps* timestamps, const struct tsn_config* tsn_config, timestamp_t from, uint8_t tc_id, uint64_t bytes, bool consider_delay) {
	int slot_id, slot_count;
	uint64_t sending_duration, remainder;
	const struct qbv_baked_config* baked;
	const struct qbv_baked_prio* baked_prio;
	const struct qbv_config* qbv = &tsn_config->qbv;
	memset(timestamps, 0, sizeof(struct timestamps));

	if (qbv->enabled == false) {
		// No Qbv. Just return the current time
		timestamps->from = from;
		timestamps->to = TSN_ALWAYS_OPEN(timestamps->from);
		// delay_* is pointless. Just set it to be right next to the frame
		timestamps->delay_from = timestamps->from;
		timestamps->delay_to = TSN_ALWAYS_OPEN(timestamps->delay_from);
		return true;
	}

	baked = &tsn_config->qbv_baked;
	baked_prio = &baked->prios[tc_id];
	sending_duration = bytes_to_ns(bytes);

	// TODO: Need to check if the slot is big enough to fit the frame. But, That is a user fault. Don't mind for now
	// But we still have to check if the first current slot's remaining time is enough to fit the frame

	remainder = (from - qbv->start) % baked->cycle_ns;
	slot_id = 0;
	slot_count = baked_prio->slot_count;

	// Check if tc_id is always open or always closed
	if (slot_count == 2 && baked_prio->slots[1].duration_ns == 0) {
		if (baked_prio->slots[0].opened == false) {
			// The only slot is closed. Drop the frame
			return false;
		}
		timestamps->from = from;
		timestamps->to = TSN_ALWAYS_OPEN(timestamps->from);
		if (consider_delay) {
			timestamps->delay_from = timestamps->from;
			timestamps->delay_to = TSN_ALWAYS_OPEN(timestamps->delay_from);
		}
		return true;
	}

	while (remainder > baked_prio->slots[slot_id].duration_ns) {
		remainder -= baked_prio->slots[slot_id].duration_ns;
		slot_id += 1;
	}

	// 1. "from"
	if (baked_prio->slots[slot_id].opened) {
		// Skip the slot if its remaining time is not enough to fit the frame
		if (baked_prio->slots[slot_id].duration_ns - remainder < sending_duration) {
			// Skip this slot. Because the slots are on/off pairs, we skip 2 slots
			// First
			timestamps->from = from - remainder + baked_prio->slots[slot_id].duration_ns;
			slot_id = (slot_id + 1) % baked_prio->slot_count;
			// Second
			timestamps->from += baked_prio->slots[slot_id].duration_ns;
			slot_id = (slot_id + 1) % baked_prio->slot_count;
		} else {
			// The slot's remaining time is enough to fit the frame
			timestamps->from = from - remainder;
		}
	} else {
		// Select next slot
		timestamps->from = from - remainder + baked_prio->slots[slot_id].duration_ns;
		slot_id = (slot_id + 1) % baked_prio->slot_count; // Opened slot
	}

	// 2. "to"
	timestamps->to = timestamps->from + baked_prio->slots[slot_id].duration_ns;

	if (consider_delay) {
		// 3. "delay_from"
		timestamps->delay_from = timestamps->from + baked_prio->slots[slot_id].duration_ns;
		slot_id = (slot_id + 1) % baked_prio->slot_count; // Closed slot
		timestamps->delay_from += baked_prio->slots[slot_id].duration_ns;
		slot_id = (slot_id + 1) % baked_prio->slot_count; // Opened slot
		// 4. "delay_to"
		timestamps->delay_to = timestamps->delay_from + baked_prio->slots[slot_id].duration_ns;
	}

	// Adjust times
	timestamps->from = max(timestamps->from, from); // If already in the slot
	timestamps->to -= sending_duration;
	if (consider_delay) {
		timestamps->delay_to -= sending_duration;
	}

#ifdef __LIBXDMA_DEBUG__
	// Assert the timestamps
	if (timestamps->from >= timestamps->to) {
		pr_err("Invalid timestamps, from >= to, %llu >= %llu", timestamps->from, timestamps->to);
	}

	if (consider_delay) {
		if (timestamps->delay_from >= timestamps->delay_to) {
			pr_err("Invalid timestamps, delay_from >= delay_to, %llu >= %llu", timestamps->delay_from, timestamps->delay_to);
		}

		if (timestamps->to >= timestamps->delay_from) {
			pr_err("Invalid timestamps, to >= delay_from, %llu >= %llu", timestamps->to, timestamps->delay_from);
		}
	}
#endif // __LIBXDMA_DEBUG__

	return true;
}

static void update_buffer(struct xdma_dev* xdev) {
	if (xdev->tsn_config.buffer_space < HW_QUEUE_SIZE_PAD) {
		u64 total_pkts = alinx_get_total_new_entry_by_xdev(xdev);
		u64 sent_pkts = alinx_get_total_valid_entry_by_xdev(xdev);
		u64 dropped_pkts = alinx_get_total_drop_entry_by_xdev(xdev);
		xdev->tsn_config.buffer_space = HW_QUEUE_SIZE - (u32)(total_pkts - sent_pkts - dropped_pkts);
	}
}

static void decrease_buffer_space(struct xdma_dev* xdev) {
	xdev->tsn_config.buffer_space -= 1;
}

static bool is_buffer_available(struct xdma_dev* xdev, uint32_t min_space) {
	update_buffer(xdev);
	return xdev->tsn_config.buffer_space > min_space;
}

int tsn_set_mqprio(struct pci_dev* pdev, struct tc_mqprio_qopt_offload* offload) {
	u8 i;
	int ret;
	struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	struct tc_mqprio_qopt qopt = offload->qopt;

	if (offload->mode != TC_MQPRIO_MODE_DCB) {
		return -ENOTSUPP;
	}

	if (qopt.num_tc >= TC_QOPT_MAX_QUEUE) {
		pr_err("Invalid number of tc\n");
		return -EINVAL;
	}

	if ((ret = netdev_set_num_tc(xdev->ndev, qopt.num_tc)) < 0) {
		pr_err("Failed to set num_tc\n");
		return ret;
	}

	if (qopt.num_tc == 0) {
		// No need to proceed further
		return 0;
	}

	for (i = 0; i < qopt.num_tc; i++) {
		if (netdev_set_tc_queue(xdev->ndev, i, qopt.count[i], qopt.offset[i]) < 0) {
			pr_warn("Failed to set tc queue: tc [%u], queue [%u@%u]\n", i, qopt.count[i], qopt.offset[i]);
		}
	}

	for (i = 0; i < TC_QOPT_BITMASK; i++) {
		if (netdev_set_prio_tc_map(xdev->ndev, i, qopt.prio_tc_map[i]) < 0) {
			pr_warn("Failed to set tc map: prio [%u], tc [%d]\n", i, qopt.prio_tc_map[i]);
		}
	}

	return 0;
}

int tsn_set_qav(struct pci_dev* pdev, struct tc_cbs_qopt_offload* offload) {
	struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	struct tsn_config* config = &xdev->tsn_config;
	if (offload->queue < 0 || offload->queue >= TC_COUNT) {
		return -EINVAL;
	}

	config->qav[offload->queue].enabled = offload->enable;
	config->qav[offload->queue].hi_credit = offload->hicredit * 1000;
	config->qav[offload->queue].lo_credit = offload->locredit * 1000;
	config->qav[offload->queue].idle_slope = offload->idleslope / 1000;
	config->qav[offload->queue].send_slope = offload->sendslope / 1000;

	bake_qos_config(config);

	return 0;
}

int tsn_set_qbv(struct pci_dev* pdev, struct tc_taprio_qopt_offload* offload) {
	u32 i, j;
	struct xdma_dev* xdev = xdev_find_by_pdev(pdev);
	struct tsn_config* config = &xdev->tsn_config;
	bool enabled = false;

	if (offload->num_entries > MAX_QBV_SLOTS) {
		return -EINVAL;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	switch (offload->cmd) {
	case TAPRIO_CMD_REPLACE:
		config->qbv.enabled = true;
		enabled = true;
		break;
	case TAPRIO_CMD_DESTROY:
		config->qbv.enabled = false;
		break;
	default:
		/* TAPRIO_CMD_STATS */
		/* TAPRIO_CMD_QUEUE_STATS */
		return -EOPNOTSUPP;
	}
#else
	config->qbv.enabled = offload->enable;
	enabled = config->qbv.enabled;
#endif

	if (enabled) {
		config->qbv.start = offload->base_time;
		config->qbv.slot_count = offload->num_entries;

		for (i = 0; i < config->qbv.slot_count; i++) {
			// TODO: handle offload->entries[i].command
			config->qbv.slots[i].duration_ns = offload->entries[i].interval;
			for (j = 0; j < TC_COUNT; j++) {
				config->qbv.slots[i].opened_prios[j] = (offload->entries[i].gate_mask & (1 << j));
			}
		}
	}

	bake_qos_config(config);

	return 0;
}
