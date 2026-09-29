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
stats_t tx_stats;

char tx_devname[MAX_DEVICE_NAME];
int tx_fd;

extern int tx_thread_run;
extern CircularParsedQueue_t g_parsed_queue;

void packet_dump(BUF_POINTER buffer, int length);
BUF_POINTER get_reserved_tx_buffer();

static int enqueue(struct tsn_tx_buffer* tx);
static struct tsn_tx_buffer* dequeue();

static int enqueue(struct tsn_tx_buffer* tx) {
#ifndef DISABLE_TSN_QUEUE
    uint16_t prio = tx->metadata.vlan_prio;
    int res = tsn_queue_enqueue(tx, prio);
    if (res == 0) {
        buffer_pool_free((BUF_POINTER)tx);
    }
    return res;
#else
    transmit_tsn_packet(tx);
    return 1;
#endif
}

static struct tsn_tx_buffer* dequeue() {
    timestamp_t now = gptp_get_timestamp(get_sys_count());
    int queue_index = tsn_select_queue(now);
    if (queue_index < 0) {
        return NULL;
    }

    return tsn_queue_dequeue(queue_index);
}

int transmit_tsn_packet(struct tsn_tx_buffer* packet) {

    uint64_t mem_len = sizeof(packet->metadata) + packet->metadata.frame_length;
    uint64_t bytes_tr;
    int status = 0;

    if (mem_len >= MAX_BUFFER_LENGTH) {
        printf("%s - %p length(%ld) is out of range(%d)\r\n", __func__, packet, mem_len, MAX_BUFFER_LENGTH);
        buffer_pool_free((BUF_POINTER)packet);
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

    buffer_pool_free((BUF_POINTER)packet);

    return status;
}

static void periodic_process_ptp()
{
#ifdef DISABLE_GPTP
    return;
#endif

    uint32_t size;
    struct tsn_tx_buffer* buffer;

    if (1) {
        struct gptp_statistics_result stats[3];
        gptp_get_statistics(stats);

        struct gptp_statistics_result* offset = &stats[0];
        struct gptp_statistics_result* freq = &stats[1];
        struct gptp_statistics_result* delay = &stats[2];

        timestamp_t now = gptp_get_timestamp(get_sys_count());

        if (offset->num + freq->num + delay->num > 0) {
            printf_debug(
                "[%d.%03d]: "
                "rms %4d max %4d "
                "freq %+6d +/- %3d "
                "delay %5d +/- %3d\n",
                (int)(now / 1e9), (int)(now / 1e6) % 1000,
                offset->rms, offset->max_abs,
                freq->mean, freq->stdev,
                delay->mean, delay->stdev);
        }

        gptp_reset_statistics();
    }

    buffer = (struct tsn_tx_buffer*)get_reserved_tx_buffer();
    if (buffer == NULL) {
        printf_debug("Cannot get buffer for pdelay_req\n");
        return;
    }
    size = gptp_make_pdelay_req_packet(buffer);
    if (size == 0) {
        buffer_pool_free((BUF_POINTER)buffer);
        return;
    }
    enqueue(buffer);

    // gPTP Master part

    // Send announce packet
    buffer = (struct tsn_tx_buffer*)get_reserved_tx_buffer();
    if (buffer == NULL) {
        printf_debug("Cannot get buffer for announce\n");
        return;
    }
    size = gptp_make_announce_packet(buffer);
    if (size == 0) {
        buffer_pool_free((BUF_POINTER)buffer);
        return; // I am not a master
    }
    enqueue(buffer);

    // Send Sync packet
    buffer = (struct tsn_tx_buffer*)get_reserved_tx_buffer();
    if (buffer == NULL) {
        printf_debug("Cannot get buffer for sync\n");
        return;
    }
    size = gptp_make_sync_packet(buffer);
    transmit_tsn_packet(buffer);

    // Send Followup packet
    buffer = (struct tsn_tx_buffer*)get_reserved_tx_buffer();
    if (buffer == NULL) {
        printf_debug("Cannot get buffer for fup\n");
        return;
    }
    size = gptp_make_followup_packet(buffer);
    enqueue(buffer);
}

static void send_burst_packet(char* devname, int fd, int bd_num, unsigned long curr_done, struct xdma_multi_read_write_ioctl *io) {

	int bytes_tr;
	int id;

	io->bd_num = bd_num;
	io->done = curr_done;
	if(xdma_api_write_to_multi_buffers_with_fd(devname, fd, io,
								   &bytes_tr)) {
		tx_stats.txErrors+=bd_num;
		multi_buffer_pool_free(io);
		return;
	}
	if(curr_done != bytes_tr) {
		printf("Transmit counter wrong, curr_done: %ld, bytes_tr: %d\n", curr_done, bytes_tr);
	}
	for(id = 0; id < io->bd_num; id++) {
		if(io->bd[id].len) {
			tx_stats.txPackets++;
			tx_stats.txBytes += io->bd[id].len;
		} else {
			tx_stats.txErrors++;
		}
	}
	multi_buffer_pool_free(io);
}

int pbuffer_dequeue(CircularParsedQueue_t *q, struct xdma_buffer_descriptor *element);
static void sender_in_tsn_mode(char* devname, int fd, uint64_t size) {

    int received_packet_count;
    int index;
    uint64_t last_timer = 0;
    struct xdma_buffer_descriptor bd;
	struct xdma_multi_read_write_ioctl io;
	int bd_num;
	unsigned long curr_done;

    while (tx_thread_run) {
        uint64_t now = get_sys_count();
        // Might need to be changed into get_timestamp from gPTP module
        if ((now - last_timer) > (1000000000 / 8)) {
            periodic_process_ptp();
            last_timer = now;
        }

        received_packet_count = getParsedQueueCount(&g_parsed_queue);

        if (received_packet_count > 0) {
			if(received_packet_count > 16) {
				received_packet_count = 16;
			}
            for(index=0; index<received_packet_count; index++) {
                pbuffer_dequeue(&g_parsed_queue, &bd);
                if(bd.buffer == NULL) {
                    continue;
                }
                if (enqueue((struct tsn_tx_buffer*)bd.buffer) > 0) {
                    continue;
                } else {
                    tx_stats.txFiltered++;
                    buffer_pool_free((BUF_POINTER)bd.buffer);
                }
            }
        }
#ifndef DISABLE_TSN_QUEUE
        // Process TX
		bd_num = 0;
		curr_done = 0;
        for (int i = 0; i < 20; i += 1) {
            struct tsn_tx_buffer* tx_buffer = dequeue();
            if (tx_buffer == NULL) {
                break;
            }
			io.bd[bd_num].buffer = (char *)tx_buffer;
            io.bd[bd_num].len = (unsigned long)(tx_buffer->metadata.frame_length + sizeof(struct tx_metadata));
			curr_done += io.bd[bd_num].len;
            bd_num++;
//			if(bd_num >= MAX_BD_NUMBER) 
			if(bd_num >= 1) 
			{
                send_burst_packet(devname, fd, bd_num, curr_done, &io);
				bd_num = 0;
				curr_done = 0;
			}
        }

		if(bd_num) {
            send_burst_packet(devname, fd, bd_num, curr_done, &io);
		}
#endif
    }
}


static void sender_in_normal_mode(char* devname, int fd, uint64_t size) {

    struct xdma_multi_read_write_ioctl bd;
    int bytes_tr;
    int max_bd_num = 0;
    int bd_num;
    int id;
    unsigned long curr_done;
    unsigned long max_done = 0;

    while (tx_thread_run) {
        bd_num = 0;
        curr_done = 0;
        if((bd_num = pbuffer_multi_dequeue(&g_parsed_queue, &bd)) ==0) {
            continue;
        }
        for(id=0; id<bd.bd_num; id++) {
            curr_done += bd.bd[id].len;
        }

        for(id = bd.bd_num; id < MAX_BD_NUMBER; id++) {
            bd.bd[id].buffer = NULL;
            bd.bd[id].len = 0;
        }
        bd.done = curr_done;

        if(xdma_api_write_to_multi_buffers_with_fd(devname, fd, &bd,
                                           &bytes_tr)) {
            tx_stats.txErrors+=bd_num;
            multi_buffer_pool_free(&bd);
            continue;
        }
        if(curr_done != bytes_tr) {
            printf("Transmit counter wrong, curr_done: %ld, bytes_tr: %d\n", curr_done, bytes_tr);
        }
        if(bd_num > max_bd_num) {
            printf("%s max_pkt_cnt from %d to %d\n", __func__, max_bd_num, bd_num);
            max_bd_num = bd_num;
        }
        if(curr_done > max_done) {
            printf("%s max_done from %ld to %ld\n", __func__, max_done, curr_done);
            max_done = curr_done;
        }

        for(id = 0; id < bd.bd_num; id++) {
            if(bd.bd[id].len) {
                tx_stats.txPackets++;
                tx_stats.txBytes += bd.bd[id].len;
            } else {
                tx_stats.txErrors++;
            }
        }
        multi_buffer_pool_free(&bd);
    }
}

void sender_in_loopback_mode(char* devname, int fd, char *fn, uint64_t size) {

    QueueElement buffer = NULL;
    uint64_t bytes_tr;
    int infile_fd = -1;
    ssize_t rc;

    infile_fd = open(fn, O_RDONLY);
    if (infile_fd < 0) {
        printf("Unable to open input file %s, %d.\n", fn, infile_fd);
        return;
    }

    while (tx_thread_run) {
        buffer = NULL;
        buffer = xbuffer_dequeue();

        if(buffer == NULL) {
            continue;
        }

        lseek(infile_fd, 0, SEEK_SET);
        rc = read_to_buffer(fn, infile_fd, buffer, size, 0);
        if (rc < 0 || rc < size) {
            printf("%s - Error, read_to_buffer: size - %ld, rc - %ld.\n", __func__, size, rc);
            close(infile_fd);
            return;
        }

        if(xdma_api_write_from_buffer_with_fd(devname, fd, buffer,
                                       size, &bytes_tr)) {
            printf("%s - Error, xdma_api_write_from_buffer_with_fd.\n", __func__);
            continue;
        }

        tx_stats.txPackets++;
        tx_stats.txBytes += bytes_tr;

        buffer_pool_free((BUF_POINTER)buffer);
    }
    close(infile_fd);
}

void sender_in_performance_mode(char* devname, int fd, char *fn, uint64_t size) {

    QueueElement buffer = NULL;
    uint64_t bytes_tr;
    int infile_fd = -1;
    ssize_t rc;
    struct timeval previousTime, currentTime;
    double elapsedTime;

    infile_fd = open(fn, O_RDONLY);
    if (infile_fd < 0) {
        printf("Unable to open input file %s, %d.\n", fn, infile_fd);
        return;
    }

    buffer = (QueueElement)xdma_api_get_buffer(size);
    if(buffer == NULL) {
        close(infile_fd);
        return;
    }

    rc = read_to_buffer(fn, infile_fd, buffer, size, 0);
    if (rc < 0 || rc < size) {
        close(infile_fd);
        free(buffer);
        return;
    }

    gettimeofday(&previousTime, NULL);
    while (tx_thread_run) {
        gettimeofday(&currentTime, NULL);
        elapsedTime = (currentTime.tv_sec - previousTime.tv_sec) + (currentTime.tv_usec - previousTime.tv_usec) / 1000000.0;
        if (elapsedTime >= 0.00001) {
            if(xdma_api_write_from_buffer_with_fd(devname, fd, buffer,
                                           size, &bytes_tr)) {
                continue;
            }

            tx_stats.txPackets++;
            tx_stats.txBytes += bytes_tr;
            gettimeofday(&previousTime, NULL);
        }
    }

    close(infile_fd);
    free(buffer);
}

static void sender_in_debug_mode(char* devname, int fd, char *fn, uint64_t size) {

    struct xdma_multi_read_write_ioctl bd;
    int bytes_tr;
    int id;
    unsigned long curr_done;

    QueueElement buffer = NULL;
    int infile_fd = -1;
    ssize_t rc;
    FILE *fp = NULL;

    printf(">>> %s\n", __func__);

    fp = fopen(fn, "rb");
    if(fp == NULL) {
        printf("Unable to open input file %s, %d.\n", fn, infile_fd);
        return;
    }

    if(posix_memalign((void **)&buffer, BUFFER_ALIGNMENT /*alignment */, MAX_BUFFER_LENGTH * MAX_BD_NUMBER)) {
        fprintf(stderr, "OOM %u.\n", MAX_BUFFER_LENGTH * MAX_BD_NUMBER);
        fclose(fp);
        return;
    }

    for(int i=0; i<MAX_BD_NUMBER; i++) {
        fseek( fp, 0, SEEK_SET );
        rc = fread((QueueElement)&buffer[i*MAX_BUFFER_LENGTH], sizeof(char), size, fp);
        if (rc < 0 || rc < size) {
            free(buffer);
            fclose(fp);
            return;
        }
    }
    fclose(fp);
   
    set_register(REG_TSN_CONTROL, 1);
    while (tx_thread_run) {
        curr_done = 0;
        bd.bd_num = MAX_BD_NUMBER;
//        bd.bd_num = 2;
        for(id=0; id<bd.bd_num; id++) {
            bd.bd[id].buffer = (char *)&buffer[id*MAX_BUFFER_LENGTH];
            bd.bd[id].len = size;
            curr_done += bd.bd[id].len;
        }

        for(id = bd.bd_num; id < MAX_BD_NUMBER; id++) {
            bd.bd[id].buffer = NULL;
            bd.bd[id].len = 0;
        }
        bd.done = curr_done;

        if(xdma_api_write_to_multi_buffers_with_fd(devname, fd, &bd,
                                           &bytes_tr)) {
            tx_stats.txErrors+=bd.bd_num;
            continue;
        }

        if(curr_done != bytes_tr) {
            printf("Transmit counter wrong, curr_done: %ld, bytes_tr: %d\n", curr_done, bytes_tr);
        }

        for(id = 0; id < bd.bd_num; id++) {
            if(bd.bd[id].len) {
                tx_stats.txPackets++;
                tx_stats.txBytes += bd.bd[id].len;
            } else {
                tx_stats.txErrors++;
            }
        }
        sleep(1);
    }
    set_register(REG_TSN_CONTROL, 0);

    free(buffer);
}

void* sender_thread(void* arg) {

    int cpu;
    tx_thread_arg_t* p_arg = (tx_thread_arg_t*)arg;

    cpu = sched_getcpu();
    printf(">>> %s(cpu: %d, devname: %s, fn: %s, mode: %d, size: %d)\n", 
               __func__, cpu, p_arg->devname, p_arg->fn, 
               p_arg->mode, p_arg->size);

    memset(tx_devname, 0, MAX_DEVICE_NAME);
    memcpy(tx_devname, p_arg->devname, MAX_DEVICE_NAME);

    if(xdma_api_dev_open(p_arg->devname, 0 /* eop_flush */, &tx_fd)) {
        printf("FAILURE: Could not open %s. Make sure xdma device driver is loaded and you have access rights (maybe use sudo?).\n", p_arg->devname);
        printf("<<< %s\n", __func__);
        return NULL;
    }

    initialize_statistics(&tx_stats);

    switch(p_arg->mode) {
    case RUN_MODE_TSN:
        sender_in_tsn_mode(p_arg->devname, tx_fd, p_arg->size);
    break;
    case RUN_MODE_NORMAL:
        sender_in_normal_mode(p_arg->devname, tx_fd, p_arg->size);
    break;
    case RUN_MODE_LOOPBACK:
        sender_in_performance_mode(p_arg->devname, tx_fd, p_arg->fn, p_arg->size);
    break;
    case RUN_MODE_PERFORMANCE:
        sender_in_performance_mode(p_arg->devname, tx_fd, p_arg->fn, p_arg->size);
    break;
    default:
        printf("%s - Unknown mode(%d)\n", __func__, p_arg->mode);
    break;
    }

    close(tx_fd);
    printf("<<< %s()\n", __func__);

    return NULL;
}

void* tx_thread(void* arg) {

    int cpu;
    tx_thread_arg_t* p_arg = (tx_thread_arg_t*)arg;

    cpu = sched_getcpu();
    printf(">>> %s(cpu: %d, devname: %s, fn: %s, mode: %d, size: %d)\n",
               __func__, cpu, p_arg->devname, p_arg->fn,
               p_arg->mode, p_arg->size);

    memset(tx_devname, 0, MAX_DEVICE_NAME);
    memcpy(tx_devname, p_arg->devname, MAX_DEVICE_NAME);

    if(xdma_api_dev_open(p_arg->devname, 0 /* eop_flush */, &tx_fd)) {
        printf("FAILURE: Could not open %s. Make sure xdma device driver is loaded and you have access rights (maybe use sudo?).\n", p_arg->devname);
        printf("<<< %s\n", __func__);
        return NULL;
    }

    initialize_statistics(&tx_stats);

    switch(p_arg->mode) {
    case RUN_MODE_DEBUG:
        sender_in_debug_mode(p_arg->devname, tx_fd, p_arg->fn, p_arg->size);
    break;
    default:
        printf("%s - Unknown mode(%d)\n", __func__, p_arg->mode);
    break;
    }

    close(tx_fd);
    printf("<<< %s()\n", __func__);

    return NULL;
}

