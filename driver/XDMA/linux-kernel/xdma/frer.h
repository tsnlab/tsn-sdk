/*
 * IEEE 802.1CB Frame Replication and Elimination for Reliability (FRER)
 *
 * This file provides FRER support for TSN networks.
 * - Talker: R-TAG insertion with sequence number generation
 * - Listener: Duplicate elimination based on sequence numbers
 * - Stream Identification: SMAC + DMAC based
 */

#ifndef FRER_H
#define FRER_H

#include <linux/types.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/netdevice.h>

/* R-TAG EtherType (IEEE 802.1CB) */
#define ETH_P_RTAG		0xF1C1

/* FRER configuration limits */
#define MAX_FRER_STREAMS	64
#define FRER_HASH_BITS		6
#define MAX_FRER_PORTS		2

/* Vector Recovery Algorithm history window size (in bits) */
#define FRER_HISTORY_LEN	64

/* FRER processing results */
#define FRER_PASS		0
#define FRER_DROP_DUPLICATE	1
#define FRER_DROP_OUT_OF_WINDOW	2
#define FRER_ERROR		-1

/* R-TAG structure (IEEE 802.1CB)
 * Inserted after VLAN tag (if present) or after source MAC
 *
 * Format:
 * [DMAC][SMAC][VLAN?][R-TAG][Original EtherType][Payload]
 *
 * R-TAG (6 bytes):
 * - EtherType: 2 bytes (0xF1C1)
 * - Reserved:  2 bytes
 * - SeqNum:    2 bytes (16-bit sequence number)
 */
struct frer_rtag {
	__be16 ethertype;	/* 0xF1C1 */
	__be16 reserved;	/* Reserved, must be 0 */
	__be16 seq_num;		/* 16-bit sequence number */
} __packed;

#define FRER_RTAG_SIZE		sizeof(struct frer_rtag)

/* Stream identification based on SMAC + DMAC */
struct frer_stream_id {
	uint8_t smac[ETH_ALEN];
	uint8_t dmac[ETH_ALEN];
};

/* Per-stream state for sequence generation (Talker side) */
struct frer_seq_gen {
	uint16_t next_seq;	/* Next sequence number to use */
	bool active;		/* Whether sequence generation is enabled */
};

/* Per-port statistics for sequence recovery */
struct frer_port_stats {
	uint64_t received_count;	/* Frames received on this port */
	uint64_t passed_count;		/* Frames passed (first arrival) */
	uint64_t eliminated_count;	/* Duplicates eliminated on this port */
};

/* Per-stream state for sequence recovery (Listener side)
 * Uses Vector Recovery Algorithm (supports reordering within history window)
 *
 * The algorithm maintains:
 * - recv_seq: highest sequence number accepted (recovery point)
 * - history: bitmap of received seq nums in [recv_seq - HISTORY_LEN, recv_seq)
 *
 * For incoming seq_num:
 * - seq_num > recv_seq: new frame, shift history and accept
 * - seq_num == recv_seq: duplicate of recovery point, drop
 * - seq_num in history window: check/set bit in history bitmap
 * - seq_num < recv_seq - HISTORY_LEN: out of window (policy: drop)
 *
 * Multi-port support:
 * - Same stream can receive from multiple ports
 * - First frame with a sequence number passes, duplicates from other port dropped
 * - Per-port statistics tracked separately
 */
struct frer_seq_recv {
	uint16_t recv_seq;		/* Recovery point (highest seq accepted) */
	uint64_t history;		/* Bitmap: bit i = recv_seq - 1 - i received */
	bool initialized;		/* Whether recv_seq is valid */
	bool active;			/* Whether recovery is enabled */
	uint64_t received_count;	/* Total frames received (all ports) */
	uint64_t eliminated_count;	/* Total duplicates eliminated */
	uint64_t out_of_order_count;	/* Out-of-order frames accepted */
	uint64_t out_of_window_count;	/* Frames dropped (too old) */
	struct frer_port_stats port_stats[MAX_FRER_PORTS]; /* Per-port stats */
};

/* Stream entry in the hash table */
struct frer_stream {
	struct frer_stream_id id;	/* Stream identifier (SMAC + DMAC) */
	struct frer_seq_gen seq_gen;	/* Talker: sequence generation state */
	struct frer_seq_recv seq_recv;	/* Listener: sequence recovery state */
	struct hlist_node hash_node;	/* Hash table linkage */
};

/* FRER configuration (can be shared across multiple ports) */
struct frer_config {
	DECLARE_HASHTABLE(streams, FRER_HASH_BITS);
	spinlock_t lock;
	bool enabled;
	uint32_t stream_count;
	uint32_t num_ports;		/* Number of registered ports */
	struct net_device *ports[MAX_FRER_PORTS]; /* Registered net devices */
};

/* Global FRER manager for multi-port support */
struct frer_global {
	struct frer_config config;
	bool initialized;
};

/* IOCTL commands for FRER configuration */
#define SIOC_FRER_ADD_STREAM	(SIOCDEVPRIVATE + 1)
#define SIOC_FRER_DEL_STREAM	(SIOCDEVPRIVATE + 2)
#define SIOC_FRER_GET_STATS	(SIOCDEVPRIVATE + 3)
#define SIOC_FRER_ENABLE	(SIOCDEVPRIVATE + 4)

/* IOCTL data structures */
struct frer_stream_config {
	struct frer_stream_id id;
	bool seq_gen_active;	/* Enable sequence generation (Talker) */
	bool seq_recv_active;	/* Enable sequence recovery (Listener) */
};

struct frer_stream_stats {
	struct frer_stream_id id;
	uint64_t received_count;
	uint64_t eliminated_count;
	uint64_t out_of_order_count;
	uint64_t out_of_window_count;
	uint16_t recv_seq;
	uint16_t next_seq;
	struct frer_port_stats port_stats[MAX_FRER_PORTS]; /* Per-port stats */
};

/* Forward declaration */
struct xdma_dev;
struct tsn_config;

/* Global FRER manager */
extern struct frer_global g_frer;

/* Global FRER initialization and cleanup */
void frer_global_init(void);
void frer_global_cleanup(void);

/* Port registration for multi-port support */
int frer_register_port(struct net_device *ndev);
void frer_unregister_port(struct net_device *ndev);
int frer_get_port_id(struct net_device *ndev);

/* FRER initialization and cleanup (per-config) */
void frer_init(struct frer_config *frer);
void frer_cleanup(struct frer_config *frer);

/* Stream management */
int frer_add_stream(struct frer_config *frer, const struct frer_stream_config *config);
int frer_del_stream(struct frer_config *frer, const struct frer_stream_id *id);
struct frer_stream *frer_stream_lookup(struct frer_config *frer,
				       const uint8_t *smac, const uint8_t *dmac);
int frer_get_stream_stats(struct frer_config *frer, struct frer_stream_stats *stats);

/* TX path: R-TAG insertion (Talker) */
int frer_insert_rtag(struct sk_buff *skb, struct frer_stream *stream);

/* TX path: R-TAG insertion with TX metadata offset */
int frer_insert_rtag_tx(struct sk_buff *skb, struct frer_stream *stream,
			int frame_offset, int frame_length);

/* Auto-register stream (caller must hold frer->lock) */
struct frer_stream *frer_auto_register_stream(struct frer_config *frer,
					      const uint8_t *smac,
					      const uint8_t *dmac);

/* RX path: R-TAG processing and duplicate elimination (Listener)
 * @port_id: ID of the receiving port (0 or 1), -1 if unknown
 */
int frer_process_rtag(struct sk_buff *skb, struct frer_config *frer, int port_id);

/* Helper to compute hash for stream lookup */
static inline u32 frer_stream_hash(const uint8_t *smac, const uint8_t *dmac)
{
	u32 hash = 0;
	int i;

	for (i = 0; i < ETH_ALEN; i++) {
		hash ^= smac[i] << ((i % 4) * 8);
		hash ^= dmac[i] << (((i + 2) % 4) * 8);
	}

	return hash;
}

#endif /* FRER_H */
