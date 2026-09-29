/*
* TSNv1 XDMA :
* -------------------------------------------------------------------------------
# Copyrights (c) 2023 TSN Lab. All rights reserved.
# Programmed by hounjoung@tsnlab.com
#
# Revision history
# 2023-xx-xx    hounjoung   create this file.
# $Id$
*/

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sched.h>

#include "error_define.h"
#include "receiver_thread.h"
#include "platform_config.h"

#include "ethernet.h"
#include "ip.h"
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "arp.h"
#include "gptp.h"

#include "util.h"
#include "tsn.h"

#include "../libxdma/api_xdma.h"
#include "parser_thread.h"

/******************** Constant Definitions **********************************/

extern int parse_thread_run;
extern stats_t tx_stats;

void packet_dump(BUF_POINTER buffer, int length);

CircularParsedQueue_t g_parsed_queue;

void initialize_p_queue(CircularParsedQueue_t * q) {

    q->front = 0;
    q->rear = -1;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
}

int isParsedQueueEmpty(CircularParsedQueue_t * q) {
    return (q->count == 0);
}

int isParsedQueueFull(CircularParsedQueue_t * q) {
    return (q->count == NUMBER_OF_QUEUE);
}

int getParsedQueueCount(CircularParsedQueue_t * q) {
    return q->count;
}

int pbuffer_enqueue(CircularParsedQueue_t *q, struct xdma_buffer_descriptor element) {
    pthread_mutex_lock(&q->mutex);

    if (isParsedQueueFull(q)) {
        debug_printf("Parsed Queue is full. Cannot pbuffer_enqueue.\n");
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    q->rear = (q->rear + 1) % NUMBER_OF_QUEUE;
    q->elements[q->rear].buffer = element.buffer;
    q->elements[q->rear].len = element.len;
    q->count++;

    pthread_mutex_unlock(&q->mutex);

    return 0;
}

int pbuffer_dequeue(CircularParsedQueue_t *q, struct xdma_buffer_descriptor *element) {
    pthread_mutex_lock(&q->mutex);

    if (isParsedQueueEmpty(q)) {
        debug_printf("Parsed Queue is empty. Cannot pbuffer_dequeue.\n");
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    element->buffer = q->elements[q->front].buffer;
    element->len = q->elements[q->front].len;
    q->front = (q->front + 1) % NUMBER_OF_QUEUE;
    q->count--;

    pthread_mutex_unlock(&q->mutex);

    return 0;
}

int pbuffer_multi_dequeue(CircularParsedQueue_t *q, struct xdma_multi_read_write_ioctl *bd) {
    pthread_mutex_lock(&q->mutex);

    int id;
    for(id=0; id<MAX_BD_NUMBER; id++) {
        if (isParsedQueueEmpty(q)) {
            debug_printf("Parsed Queue is empty. Cannot pbuffer_dequeue.\n");
            bd->bd_num = id;
            pthread_mutex_unlock(&q->mutex);
            return id;
        }
        bd->bd[id].buffer = q->elements[q->front].buffer;
        bd->bd[id].len = q->elements[q->front].len;
        q->front = (q->front + 1) % NUMBER_OF_QUEUE;
        q->count--;
    }

    bd->bd_num = id;
    pthread_mutex_unlock(&q->mutex);

    return id;
}

#include "packet.h"

static const char myMAC[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };

extern char tx_devname[MAX_DEVICE_NAME];
extern int tx_fd;

static int transmit_tsn_packet_no_free(struct tsn_tx_buffer* packet) {

    uint64_t bytes_tr;
    uint64_t mem_len = sizeof(packet->metadata) + packet->metadata.frame_length;
    int status = 0;

    if (mem_len >= MAX_BUFFER_LENGTH) {
        printf("%s - %p length(%ld) is out of range(%d)\r\n", __func__, packet, mem_len, MAX_BUFFER_LENGTH);
        return XST_FAILURE;
    }

    status = xdma_api_write_from_buffer_with_fd(tx_devname, tx_fd, (char *)packet,
                                       mem_len, &bytes_tr);

    if(status != XST_SUCCESS) {
        tx_stats.txErrors++;
    } else {
        tx_stats.txPackets++;
        tx_stats.txBytes += mem_len;
    }

    return status;
}

static int parse_packet_with_bd(struct tsn_rx_buffer* rx, struct xdma_buffer_descriptor *bd) {

    int transmit_flag = 0;
    uint8_t *buffer =(uint8_t *)rx;
    int tx_len;
    // Reuse RX buffer as TX
    struct tsn_tx_buffer* tx = (struct tsn_tx_buffer*)(buffer + sizeof(struct rx_metadata) - sizeof(struct tx_metadata));
    struct tx_metadata* tx_metadata = &tx->metadata;
    uint8_t* rx_frame = (uint8_t*)&rx->data;
    uint8_t* tx_frame = (uint8_t*)&tx->data;
    struct ethernet_header* rx_eth = (struct ethernet_header*)rx_frame;
    struct ethernet_header* tx_eth = (struct ethernet_header*)tx_frame;

    // make tx metadata
    // tx_metadata->vlan_tag = rx->metadata.vlan_tag;
    tx_metadata->timestamp_id = 0;
    tx_metadata->reserved = 0;

    // make ethernet frame
    memcpy(&(tx_eth->dmac), &(rx_eth->smac), 6);
    memcpy(&(tx_eth->smac), myMAC, 6);
    // tx_eth->type = rx_eth->type;

    tx_len = ETH_HLEN;

    if(rx->metadata.frame_length >= MAX_BUFFER_LENGTH) {
        printf("%s - %s - rx->metadata.frame_length: %d, MAX_BUFFER_LENGTH: %d\n",
            __FILE__, __func__, rx->metadata.frame_length, MAX_BUFFER_LENGTH);
        return XST_FAILURE;
    }

    bd->buffer = NULL;
    bd->len =  0;

    // do arp, udp echo, etc.
    switch (rx_eth->type) {
#ifndef DISABLE_GPTP
    case ETH_TYPE_PTPv2:
        ;
        int len = process_gptp_packet(rx);
        if (len <= 0) { return XST_FAILURE; }
        tx_len += len;
        break;
#endif
    case ETH_TYPE_ARP: // arp
        ;
        struct arp_header* rx_arp = (struct arp_header*)ETH_PAYLOAD(rx_frame);
        struct arp_header* tx_arp = (struct arp_header*)ETH_PAYLOAD(tx_frame);
        if (rx_arp->opcode != ARP_OPCODE_ARP_REQUEST) { return XST_FAILURE; }

        // make arp packet
        // tx_arp->hw_type = rx_arp->hw_type;
        // tx_arp->proto_type = rx_arp->proto_type;
        // tx_arp->hw_size = rx_arp->hw_size;
        // tx_arp->proto_size = rx_arp->proto_size;
        tx_arp->opcode = ARP_OPCODE_ARP_REPLY;
        memcpy(tx_arp->target_hw, rx_arp->sender_hw, HW_ADDR_LEN);
        memcpy(tx_arp->sender_hw, myMAC, HW_ADDR_LEN);
        uint8_t sender_proto[4];
        memcpy(sender_proto, rx_arp->sender_proto, IP_ADDR_LEN);
        memcpy(tx_arp->sender_proto, rx_arp->target_proto, IP_ADDR_LEN);
        memcpy(tx_arp->target_proto, sender_proto, IP_ADDR_LEN);

        tx_len += ARP_HLEN;
        transmit_flag = 1;
        break;
    case ETH_TYPE_IPv4: // ip
        ;
        struct ipv4_header* rx_ipv4 = (struct ipv4_header*)ETH_PAYLOAD(rx_frame);
        struct ipv4_header* tx_ipv4 = (struct ipv4_header*)ETH_PAYLOAD(tx_frame);

        uint32_t src;

        // Fill IPv4 header
        // memcpy(tx_ipv4, rx_ipv4, IPv4_HLEN(rx_ipv4));
        src = rx_ipv4->dst;
        tx_ipv4->dst = rx_ipv4->src;
        tx_ipv4->src = src;
        tx_len += IPv4_HLEN(rx_ipv4);

        if (rx_ipv4->proto == IP_PROTO_ICMP) {
            struct icmp_header* rx_icmp = (struct icmp_header*)IPv4_PAYLOAD(rx_ipv4);

            if (rx_icmp->type != ICMP_TYPE_ECHO_REQUEST) { return XST_FAILURE; }

            struct icmp_header* tx_icmp = (struct icmp_header*)IPv4_PAYLOAD(tx_ipv4);
            unsigned long icmp_len = IPv4_BODY_LEN(rx_ipv4);

            // Fill ICMP header and body
            // memcpy(tx_icmp, rx_icmp, icmp_len);
            tx_icmp->type = ICMP_TYPE_ECHO_REPLY;
            icmp_checksum(tx_icmp, icmp_len);
            tx_len += icmp_len;

        } else if (rx_ipv4->proto == IP_PROTO_UDP){
            struct udp_header* rx_udp = (struct udp_header*)IPv4_PAYLOAD(rx_ipv4);
            if (rx_udp->dstport != 7) { return XST_FAILURE; }

            struct udp_header* tx_udp = IPv4_PAYLOAD(tx_ipv4);

            // Fill UDP header
            // memcpy(tx_udp, rx_udp, rx_udp->length);
            uint16_t srcport;
            srcport = rx_udp->dstport;
            tx_udp->dstport = rx_udp->srcport;
            tx_udp->srcport = srcport;
            tx_udp->checksum = 0;
            tx_len += rx_udp->length; // UDP.length contains header length
        } else {
            return XST_FAILURE;
        }
        break;
    default:
        printf_debug("Unknown type: %04x\n", rx_eth->type);
        return XST_FAILURE;
    }
    tx_metadata->frame_length = tx_len;
    if(transmit_flag) {
        transmit_tsn_packet_no_free(tx);
        return XST_FAILURE;
    } else {
        bd->buffer = (char *)tx;
        bd->len = (unsigned long)(sizeof(struct tx_metadata) + tx_len);
        return XST_SUCCESS;
    }
}

void parser_in_normal_mode() {

    char *buffer;
    int status;
    struct xdma_buffer_descriptor bd;

    while (parse_thread_run) {
        buffer = NULL;
        buffer = xbuffer_dequeue();
        if(buffer == NULL) {
            continue;
        }

        status = parse_packet_with_bd((struct tsn_rx_buffer*)buffer, &bd);
        if(status == XST_FAILURE) {
            tx_stats.txFiltered++;
            buffer_pool_free((BUF_POINTER)buffer);
            continue;
        }

        if(bd.len >= MAX_BUFFER_LENGTH) {
            printf("%s - %s - bd.len: %ld, MAX_BUFFER_LENGTH: %d\n",
                __FILE__, __func__, bd.len, MAX_BUFFER_LENGTH);
            tx_stats.txFiltered++;
            buffer_pool_free((BUF_POINTER)buffer);
            continue;
        }
        if(pbuffer_enqueue(&g_parsed_queue, bd)) {
            tx_stats.txFiltered++;
            buffer_pool_free((BUF_POINTER)buffer);
            continue;
        }
    }
}

void* parse_thread(void* arg) {

    int cpu;
    parse_thread_arg_t* p_arg = (parse_thread_arg_t*)arg;

    cpu = sched_getcpu();
    printf(">>> %s(cpu: %d, devname: %s, fn: %s, mode: %d, size: %d)\n",
               __func__, cpu, p_arg->devname, p_arg->fn,
               p_arg->mode, p_arg->size);

    initialize_p_queue(&g_parsed_queue);

    switch(p_arg->mode) {
    case RUN_MODE_TSN:
    case RUN_MODE_NORMAL:
        parser_in_normal_mode();
    break;
    default:
        printf("%s - Unknown mode(%d)\n", __func__, p_arg->mode);
    break;
    }

    pthread_mutex_destroy(&g_parsed_queue.mutex);

    printf("<<< %s()\n", __func__);

    return NULL;
}

