/*
 * IEEE 802.1CB Frame Replication and Elimination for Reliability (FRER)
 *
 * Implementation of FRER for TSN networks:
 * - Talker: R-TAG insertion with sequence number generation
 * - Listener: Duplicate elimination using Vector Recovery Algorithm
 * - Stream Identification: SMAC + DMAC based
 */

#include <linux/slab.h>
#include <linux/if_vlan.h>
#include <linux/etherdevice.h>

#include "frer.h"

/**
 * frer_init - Initialize FRER configuration
 * @frer: FRER configuration structure
 */
void frer_init(struct frer_config *frer)
{
	hash_init(frer->streams);
	spin_lock_init(&frer->lock);
	frer->enabled = false;
	frer->stream_count = 0;
}

/**
 * frer_cleanup - Cleanup FRER configuration and free all streams
 * @frer: FRER configuration structure
 */
void frer_cleanup(struct frer_config *frer)
{
	struct frer_stream *stream;
	struct hlist_node *tmp;
	int bkt;
	unsigned long flags;

	spin_lock_irqsave(&frer->lock, flags);

	hash_for_each_safe(frer->streams, bkt, tmp, stream, hash_node) {
		hash_del(&stream->hash_node);
		kfree(stream);
	}

	frer->stream_count = 0;
	frer->enabled = false;

	spin_unlock_irqrestore(&frer->lock, flags);
}

/**
 * frer_stream_lookup - Look up a stream by SMAC and DMAC
 * @frer: FRER configuration
 * @smac: Source MAC address
 * @dmac: Destination MAC address
 *
 * Returns the stream if found, NULL otherwise.
 * Caller must hold frer->lock or be in a context where the stream table is stable.
 */
struct frer_stream *frer_stream_lookup(struct frer_config *frer,
				       const uint8_t *smac, const uint8_t *dmac)
{
	struct frer_stream *stream;
	u32 hash = frer_stream_hash(smac, dmac);

	hash_for_each_possible(frer->streams, stream, hash_node, hash) {
		if (ether_addr_equal(stream->id.smac, smac) &&
		    ether_addr_equal(stream->id.dmac, dmac)) {
			return stream;
		}
	}

	return NULL;
}

/**
 * frer_add_stream - Add a new FRER stream
 * @frer: FRER configuration
 * @config: Stream configuration
 *
 * Returns 0 on success, negative error code on failure.
 */
int frer_add_stream(struct frer_config *frer, const struct frer_stream_config *config)
{
	struct frer_stream *stream;
	u32 hash;
	unsigned long flags;

	if (frer->stream_count >= MAX_FRER_STREAMS) {
		return -ENOSPC;
	}

	spin_lock_irqsave(&frer->lock, flags);

	/* Check if stream already exists */
	stream = frer_stream_lookup(frer, config->id.smac, config->id.dmac);
	if (stream) {
		/* Update existing stream */
		stream->seq_gen.active = config->seq_gen_active;
		stream->seq_recv.active = config->seq_recv_active;
		spin_unlock_irqrestore(&frer->lock, flags);
		return 0;
	}

	spin_unlock_irqrestore(&frer->lock, flags);

	/* Allocate new stream */
	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		return -ENOMEM;
	}

	/* Initialize stream */
	memcpy(stream->id.smac, config->id.smac, ETH_ALEN);
	memcpy(stream->id.dmac, config->id.dmac, ETH_ALEN);
	stream->seq_gen.next_seq = 0;
	stream->seq_gen.active = config->seq_gen_active;
	stream->seq_recv.recv_seq = 0;
	stream->seq_recv.history = 0;
	stream->seq_recv.initialized = false;
	stream->seq_recv.active = config->seq_recv_active;
	stream->seq_recv.received_count = 0;
	stream->seq_recv.eliminated_count = 0;
	stream->seq_recv.out_of_order_count = 0;
	stream->seq_recv.out_of_window_count = 0;

	hash = frer_stream_hash(config->id.smac, config->id.dmac);

	spin_lock_irqsave(&frer->lock, flags);

	/* Double-check in case another thread added the same stream */
	if (frer_stream_lookup(frer, config->id.smac, config->id.dmac)) {
		spin_unlock_irqrestore(&frer->lock, flags);
		kfree(stream);
		return -EEXIST;
	}

	hash_add(frer->streams, &stream->hash_node, hash);
	frer->stream_count++;

	spin_unlock_irqrestore(&frer->lock, flags);

	pr_info("FRER: Added stream SMAC=%pM DMAC=%pM (gen=%d, recv=%d)\n",
		config->id.smac, config->id.dmac,
		config->seq_gen_active, config->seq_recv_active);

	return 0;
}

/**
 * frer_del_stream - Delete a FRER stream
 * @frer: FRER configuration
 * @id: Stream identifier
 *
 * Returns 0 on success, -ENOENT if stream not found.
 */
int frer_del_stream(struct frer_config *frer, const struct frer_stream_id *id)
{
	struct frer_stream *stream;
	unsigned long flags;

	spin_lock_irqsave(&frer->lock, flags);

	stream = frer_stream_lookup(frer, id->smac, id->dmac);
	if (!stream) {
		spin_unlock_irqrestore(&frer->lock, flags);
		return -ENOENT;
	}

	hash_del(&stream->hash_node);
	frer->stream_count--;

	spin_unlock_irqrestore(&frer->lock, flags);

	pr_info("FRER: Deleted stream SMAC=%pM DMAC=%pM\n", id->smac, id->dmac);

	kfree(stream);
	return 0;
}

/**
 * frer_get_stream_stats - Get statistics for a stream
 * @frer: FRER configuration
 * @stats: Statistics structure (id field must be set)
 *
 * Returns 0 on success, -ENOENT if stream not found.
 */
int frer_get_stream_stats(struct frer_config *frer, struct frer_stream_stats *stats)
{
	struct frer_stream *stream;
	unsigned long flags;

	spin_lock_irqsave(&frer->lock, flags);

	stream = frer_stream_lookup(frer, stats->id.smac, stats->id.dmac);
	if (!stream) {
		spin_unlock_irqrestore(&frer->lock, flags);
		return -ENOENT;
	}

	stats->received_count = stream->seq_recv.received_count;
	stats->eliminated_count = stream->seq_recv.eliminated_count;
	stats->out_of_order_count = stream->seq_recv.out_of_order_count;
	stats->out_of_window_count = stream->seq_recv.out_of_window_count;
	stats->recv_seq = stream->seq_recv.recv_seq;
	stats->next_seq = stream->seq_gen.next_seq;

	spin_unlock_irqrestore(&frer->lock, flags);

	return 0;
}

/**
 * frer_insert_rtag - Insert R-TAG into a frame (Talker side)
 * @skb: Socket buffer containing the frame
 * @stream: Stream entry with sequence generation state
 *
 * Inserts R-TAG after VLAN tag (if present) or after source MAC.
 * The frame format becomes:
 *   [DMAC][SMAC][VLAN?][R-TAG][Original EtherType][Payload]
 *
 * Returns 0 on success, negative error code on failure.
 */
int frer_insert_rtag(struct sk_buff *skb, struct frer_stream *stream)
{
	struct ethhdr *eth;
	struct frer_rtag *rtag;
	__be16 orig_proto;
	int insert_offset;

	if (!stream->seq_gen.active) {
		return 0;
	}

	eth = (struct ethhdr *)skb->data;
	orig_proto = eth->h_proto;

	/* Determine insertion point: after VLAN tag if present, else after ETH header */
	if (ntohs(orig_proto) == ETH_P_8021Q) {
		/* VLAN tagged: insert after VLAN header */
		insert_offset = ETH_HLEN + VLAN_HLEN;
	} else {
		/* No VLAN: insert after Ethernet header (replacing h_proto temporarily) */
		insert_offset = ETH_HLEN;
	}

	/* Make room for R-TAG */
	if (skb_headroom(skb) < FRER_RTAG_SIZE) {
		if (pskb_expand_head(skb, FRER_RTAG_SIZE, 0, GFP_ATOMIC) != 0) {
			return -ENOMEM;
		}
		eth = (struct ethhdr *)skb->data;
	}

	/* Shift data to make room for R-TAG */
	memmove(skb->data - FRER_RTAG_SIZE, skb->data, insert_offset);
	skb_push(skb, FRER_RTAG_SIZE);

	/* Update eth pointer after push */
	eth = (struct ethhdr *)skb->data;

	/* Insert R-TAG at the insertion point */
	rtag = (struct frer_rtag *)(skb->data + insert_offset);

	if (ntohs(orig_proto) == ETH_P_8021Q) {
		/* For VLAN tagged frames, R-TAG ethertype replaces inner protocol */
		struct vlan_hdr *vhdr = (struct vlan_hdr *)(skb->data + ETH_HLEN);
		__be16 inner_proto = vhdr->h_vlan_encapsulated_proto;

		rtag->ethertype = htons(ETH_P_RTAG);
		rtag->reserved = 0;
		rtag->seq_num = htons(stream->seq_gen.next_seq);

		/* The original inner protocol follows R-TAG */
		*(__be16 *)(rtag + 1) = inner_proto;
		vhdr->h_vlan_encapsulated_proto = htons(ETH_P_RTAG);
	} else {
		/* For non-VLAN frames */
		rtag->ethertype = htons(ETH_P_RTAG);
		rtag->reserved = 0;
		rtag->seq_num = htons(stream->seq_gen.next_seq);

		/* Original protocol follows R-TAG - need to shift it */
		memmove((uint8_t *)rtag + FRER_RTAG_SIZE, &orig_proto, sizeof(orig_proto));
		eth->h_proto = htons(ETH_P_RTAG);
	}

	/* Increment sequence number (wraps at 16 bits) */
	stream->seq_gen.next_seq++;

	return 0;
}

/**
 * frer_seq_diff - Calculate signed difference between two sequence numbers
 * @a: First sequence number
 * @b: Second sequence number
 *
 * Returns (a - b) handling 16-bit wrap-around correctly.
 * Result is in range [-32768, 32767].
 */
static inline int16_t frer_seq_diff(uint16_t a, uint16_t b)
{
	return (int16_t)(a - b);
}

/**
 * frer_process_rtag - Process R-TAG and perform duplicate elimination (Listener side)
 * @skb: Socket buffer containing the frame
 * @frer: FRER configuration
 *
 * Uses Vector Recovery Algorithm:
 * - Maintains recv_seq (highest sequence number accepted, recovery point)
 * - Maintains history bitmap for sequences in [recv_seq - HISTORY_LEN, recv_seq)
 *
 * For incoming seq_num:
 * - seq_num > recv_seq: New sequence, shift history and accept
 * - seq_num == recv_seq: Duplicate of recovery point, drop
 * - seq_num in [recv_seq - HISTORY_LEN, recv_seq): Check history bitmap
 *   - Bit set: duplicate, drop
 *   - Bit clear: out-of-order, set bit and accept
 * - seq_num < recv_seq - HISTORY_LEN: Out of window, drop
 *
 * R-TAG is stripped from the frame on success.
 *
 * Returns:
 *   FRER_PASS - Frame should be accepted (R-TAG stripped)
 *   FRER_DROP_DUPLICATE - Frame is a duplicate and should be dropped
 *   FRER_DROP_OUT_OF_WINDOW - Frame is too old (outside history window)
 *   FRER_ERROR - Error processing frame
 */
int frer_process_rtag(struct sk_buff *skb, struct frer_config *frer)
{
	struct ethhdr *eth;
	struct frer_rtag *rtag;
	struct frer_stream *stream;
	uint16_t seq_num;
	__be16 proto;
	int rtag_offset;
	unsigned long flags;
	uint8_t smac[ETH_ALEN], dmac[ETH_ALEN];
	int result = FRER_PASS;
	int16_t delta;
	int bit_pos;

	if (!frer->enabled) {
		return FRER_PASS;
	}

	eth = (struct ethhdr *)skb->data;
	proto = eth->h_proto;

	/* Save MAC addresses before any manipulation */
	memcpy(dmac, eth->h_dest, ETH_ALEN);
	memcpy(smac, eth->h_source, ETH_ALEN);

	/* Find R-TAG location */
	if (ntohs(proto) == ETH_P_8021Q) {
		struct vlan_hdr *vhdr = (struct vlan_hdr *)(skb->data + ETH_HLEN);
		if (ntohs(vhdr->h_vlan_encapsulated_proto) != ETH_P_RTAG) {
			/* No R-TAG present */
			return FRER_PASS;
		}
		rtag_offset = ETH_HLEN + VLAN_HLEN;
	} else if (ntohs(proto) == ETH_P_RTAG) {
		rtag_offset = ETH_HLEN;
	} else {
		/* No R-TAG present */
		return FRER_PASS;
	}

	/* Validate packet length */
	if (skb->len < rtag_offset + FRER_RTAG_SIZE) {
		return FRER_ERROR;
	}

	rtag = (struct frer_rtag *)(skb->data + rtag_offset);
	seq_num = ntohs(rtag->seq_num);

	/* Look up stream - note: for listener, we swap SMAC/DMAC since
	 * the talker's SMAC is our received frame's source */
	spin_lock_irqsave(&frer->lock, flags);

	stream = frer_stream_lookup(frer, smac, dmac);
	if (!stream || !stream->seq_recv.active) {
		spin_unlock_irqrestore(&frer->lock, flags);
		/* Stream not configured for recovery, just strip R-TAG and pass */
		goto strip_rtag;
	}

	/* Vector Recovery Algorithm */
	stream->seq_recv.received_count++;

	if (!stream->seq_recv.initialized) {
		/* First frame for this stream - initialize recovery point */
		stream->seq_recv.recv_seq = seq_num;
		stream->seq_recv.history = 0;
		stream->seq_recv.initialized = true;
		spin_unlock_irqrestore(&frer->lock, flags);
		goto strip_rtag;
	}

	/* Calculate difference: positive = ahead of recv_seq, negative = behind */
	delta = frer_seq_diff(seq_num, stream->seq_recv.recv_seq);

	if (delta > 0) {
		/*
		 * seq_num > recv_seq: New sequence ahead of recovery point
		 * Shift history by delta positions and update recv_seq
		 */
		if (delta >= FRER_HISTORY_LEN) {
			/* Large jump - clear entire history */
			stream->seq_recv.history = 0;
		} else {
			/* Shift history left by delta, marking old recv_seq position */
			stream->seq_recv.history <<= delta;
			/* Set bit for the old recv_seq (now at position delta-1) */
			stream->seq_recv.history |= (1ULL << (delta - 1));
		}
		stream->seq_recv.recv_seq = seq_num;
		/* Accept: new frame */
	} else if (delta == 0) {
		/* seq_num == recv_seq: Duplicate of recovery point */
		stream->seq_recv.eliminated_count++;
		result = FRER_DROP_DUPLICATE;
	} else {
		/* delta < 0: seq_num is behind recv_seq */
		bit_pos = -delta - 1;  /* Position in history bitmap */

		if (bit_pos >= FRER_HISTORY_LEN) {
			/* Too old - outside history window */
			stream->seq_recv.out_of_window_count++;
			result = FRER_DROP_OUT_OF_WINDOW;
		} else if (stream->seq_recv.history & (1ULL << bit_pos)) {
			/* Bit already set - duplicate */
			stream->seq_recv.eliminated_count++;
			result = FRER_DROP_DUPLICATE;
		} else {
			/* Not seen before - out-of-order but valid */
			stream->seq_recv.history |= (1ULL << bit_pos);
			stream->seq_recv.out_of_order_count++;
			/* Accept: out-of-order frame */
		}
	}

	spin_unlock_irqrestore(&frer->lock, flags);

	if (result != FRER_PASS) {
		return result;
	}

strip_rtag:
	/* Strip R-TAG from the frame */
	if (ntohs(eth->h_proto) == ETH_P_8021Q) {
		/* VLAN tagged: restore inner protocol from after R-TAG */
		struct vlan_hdr *vhdr = (struct vlan_hdr *)(skb->data + ETH_HLEN);
		__be16 inner_proto = *(__be16 *)((uint8_t *)rtag + FRER_RTAG_SIZE);

		/* Shift everything after VLAN header to remove R-TAG */
		memmove(skb->data + ETH_HLEN + VLAN_HLEN,
			skb->data + ETH_HLEN + VLAN_HLEN + FRER_RTAG_SIZE,
			skb->len - ETH_HLEN - VLAN_HLEN - FRER_RTAG_SIZE);

		vhdr->h_vlan_encapsulated_proto = inner_proto;
		skb_trim(skb, skb->len - FRER_RTAG_SIZE);
	} else {
		/* Non-VLAN: restore original ethertype */
		__be16 orig_proto = *(__be16 *)((uint8_t *)rtag + sizeof(struct frer_rtag) - sizeof(__be16));

		/* Shift the header to cover R-TAG */
		memmove(skb->data + FRER_RTAG_SIZE, skb->data, ETH_HLEN);
		skb_pull(skb, FRER_RTAG_SIZE);

		eth = (struct ethhdr *)skb->data;
		eth->h_proto = orig_proto;
	}

	return FRER_PASS;
}
