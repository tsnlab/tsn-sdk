/*
 * This file is part of the Xilinx DMA IP Core driver for Linux
 * Copyright (c) 2016-present,  Xilinx, Inc. All rights reserved.
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#define pr_fmt(fmt)     KBUILD_MODNAME ":%s: " fmt, __func__

#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/aer.h>
#include <linux/ethtool.h>
#include <uapi/linux/net_tstamp.h>
/* include early, to verify it depends only on the headers above */
#include "libxdma_api.h"
#include "libxdma.h"
#include "xdma_mod.h"
#include "xdma_cdev.h"
#include "version.h"
#include "xdma_netdev.h"
#include "alinx_ptp.h"
#include "alinx_arch.h"
#include "frer.h"

#define DRV_MODULE_NAME		"xdma"
#define DRV_MODULE_DESC		"Xilinx XDMA Reference Driver"

/* build43+: read-only AXI GPIO in the existing 1 MiB user BAR.
 * channel 1 DATA = adapter drop, channel 2 DATA = adapter emitted data. */
#define SE_RXSTATS_GPIO_OFFSET	0x00080000
#define SE_RXSTATS_DROP_OFFSET	(SE_RXSTATS_GPIO_OFFSET + 0x0)
#define SE_RXSTATS_DATA_OFFSET	(SE_RXSTATS_GPIO_OFFSET + 0x8)

#define SE_LLEMAC_MACADR_H_OFF	0x08
#define SE_LLEMAC_MAC_CTRL_OFF	0x0C
#define SE_LLEMAC_PMAC_OFF	0x200
#define SE_LLEMAC_PROMISC	BIT(16)
#define SE_LLEMAC_RX_EN		BIT(1)
#define SE_LLEMAC_TX_EN		BIT(2)

/* The 2048-byte C2H slot fixed the host overrun seen with full pMAC traffic.
 * Keep full FPE (TX fragmentation + RX reassembly) as the production default;
 * pmac_rx_enable=0 remains available for controlled A/B isolation. */
static unsigned int pmac_rx_enable = 1;
module_param(pmac_rx_enable, uint, 0644);
MODULE_PARM_DESC(pmac_rx_enable,
	"Enable pMAC RX/reassembly path in HAT mode (default 1=full FPE; 0=TX-only A/B)");

static char version[] =
	DRV_MODULE_DESC " " DRV_MODULE_NAME " v" DRV_MODULE_VERSION "\n";

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION(DRV_MODULE_DESC);
MODULE_VERSION(DRV_MODULE_VERSION);
MODULE_LICENSE("Dual BSD/GPL");

/* SECTION: Module global variables */
static int xpdev_cnt;

static bool get_host_id(uint64_t* hostid) {
	void* buf = NULL;
	size_t size;
	int ret;
	bool result;
	ret = kernel_read_file_from_path("/etc/machine-id", 0, &buf, INT_MAX, NULL, READING_UNKNOWN);
	if (ret < 0) {
		pr_err("Failed to read machine-id\n");
		return false;
	}

	size = ret;

	if (size != 33) {
		pr_err("Invalid machine-id\n");
		result = false;
		goto end;
	}

	// Cut it to 64-bit
	((char*)buf)[16] = '\0';

	if ((ret = kstrtoull(buf, 16, hostid))) {
		pr_err("Failed to convert machine-id to uint64_t: %d\n", ret);
		result = false;
		goto end;
	}
	result = true;

end:
	vfree(buf);
	return result;
}

static uint64_t hash(unsigned long hostid, unsigned long num) {
	// Note that these magic numbers are well known constants for hashing
	// See splitmix64
	uint64_t hash = hostid;
	hash = ((hash >> 30) ^ (hash ^ num)) * U64_C(0xbf58476d1ce4e5b9);
	hash = ((hash >> 27) ^ hash) * U64_C(0x94d049bb133111eb);
	hash = (hash >> 31) ^ hash;

	return hash;
}

extern unsigned int rtag_duplicate_mac;
static void get_mac_address(char* mac_addr, struct xdma_dev *xdev, int port_id) {
	int i;
	uint64_t hashed_num;
	unsigned long long machine_id;
	unsigned char pcie_num = xdev->idx; // FIXME: Use proper PCIe number

	if (get_host_id(&machine_id) == false) {
		machine_id = 0; // Fallback value
	}

	// Hashing
	hashed_num = hash(machine_id, pcie_num);

	// Convert to MAC address
	for (i = 0; i < ETH_ALEN; i++) {
		mac_addr[i] = (hashed_num >> (i * 8)) & 0xFF;
	}

	if (port_id >= XDMA_NUM_PORTS && rtag_duplicate_mac) {
		port_id = 0;
	}

	mac_addr[5] += port_id;

	// Adjust U/L, I/G bits
	mac_addr[0] &= ~0x1; // Unicast
	mac_addr[0] &= ~0x2; // Global
}

static const struct pci_device_id pci_ids[] = {
	{ PCI_DEVICE(0x10ee, 0x9048), },
	{ PCI_DEVICE(0x10ee, 0x9044), },
	{ PCI_DEVICE(0x10ee, 0x9042), },
	{ PCI_DEVICE(0x10ee, 0x9041), },
	{ PCI_DEVICE(0x10ee, 0x903f), },
	{ PCI_DEVICE(0x10ee, 0x9038), },
	{ PCI_DEVICE(0x10ee, 0x9028), },
	{ PCI_DEVICE(0x10ee, 0x9018), },
	{ PCI_DEVICE(0x10ee, 0x9034), },
	{ PCI_DEVICE(0x10ee, 0x9024), },
	{ PCI_DEVICE(0x10ee, 0x9014), },
	{ PCI_DEVICE(0x10ee, 0x9032), },
	{ PCI_DEVICE(0x10ee, 0x9022), },
	{ PCI_DEVICE(0x10ee, 0x9012), },
	{ PCI_DEVICE(0x10ee, 0x9031), },
	{ PCI_DEVICE(0x10ee, 0x9021), },
	{ PCI_DEVICE(0x10ee, 0x9011), },

	{ PCI_DEVICE(0x10ee, 0x8011), },
	{ PCI_DEVICE(0x10ee, 0x8012), },
	{ PCI_DEVICE(0x10ee, 0x8014), },
	{ PCI_DEVICE(0x10ee, 0x8018), },
	{ PCI_DEVICE(0x10ee, 0x8021), },
	{ PCI_DEVICE(0x10ee, 0x8022), },
	{ PCI_DEVICE(0x10ee, 0x8024), },
	{ PCI_DEVICE(0x10ee, 0x8028), },
	{ PCI_DEVICE(0x10ee, 0x8031), },
	{ PCI_DEVICE(0x10ee, 0x8032), },
	{ PCI_DEVICE(0x10ee, 0x8034), },
	{ PCI_DEVICE(0x10ee, 0x8038), },

	{ PCI_DEVICE(0x10ee, 0x7011), },
	{ PCI_DEVICE(0x10ee, 0x7012), },
	{ PCI_DEVICE(0x10ee, 0x7014), },
	{ PCI_DEVICE(0x10ee, 0x7018), },
	{ PCI_DEVICE(0x10ee, 0x7021), },
	{ PCI_DEVICE(0x10ee, 0x7022), },
	{ PCI_DEVICE(0x10ee, 0x7024), },
	{ PCI_DEVICE(0x10ee, 0x7028), },
	{ PCI_DEVICE(0x10ee, 0x7031), },
	{ PCI_DEVICE(0x10ee, 0x7032), },
	{ PCI_DEVICE(0x10ee, 0x7034), },
	{ PCI_DEVICE(0x10ee, 0x7038), },

	{ PCI_DEVICE(0x10ee, 0x6828), },
	{ PCI_DEVICE(0x10ee, 0x6830), },
	{ PCI_DEVICE(0x10ee, 0x6928), },
	{ PCI_DEVICE(0x10ee, 0x6930), },
	{ PCI_DEVICE(0x10ee, 0x6A28), },
	{ PCI_DEVICE(0x10ee, 0x6A30), },
	{ PCI_DEVICE(0x10ee, 0x6D30), },

	{ PCI_DEVICE(0x10ee, 0x4808), },
	{ PCI_DEVICE(0x10ee, 0x4828), },
	{ PCI_DEVICE(0x10ee, 0x4908), },
	{ PCI_DEVICE(0x10ee, 0x4A28), },
	{ PCI_DEVICE(0x10ee, 0x4B28), },

	{ PCI_DEVICE(0x10ee, 0x2808), },

#ifdef INTERNAL_TESTING
	{ PCI_DEVICE(0x1d0f, 0x1042), 0},
#endif
	/* aws */
	{ PCI_DEVICE(0x1d0f, 0xf000), },
	{ PCI_DEVICE(0x1d0f, 0xf001), },

	{0,}
};
MODULE_DEVICE_TABLE(pci, pci_ids);

static void xpdev_free(struct xdma_pci_dev *xpdev)
{
	struct xdma_dev *xdev = xpdev->xdev;

	pr_info("xpdev 0x%p, destroy_interfaces, xdev 0x%p.\n", xpdev, xdev);
	xpdev_destroy_interfaces(xpdev);
	xpdev->xdev = NULL;
	pr_info("xpdev 0x%p, xdev 0x%p xdma_device_close.\n", xpdev, xdev);
	xdma_device_close(xpdev->pdev, xdev);
	xpdev_cnt--;

	kfree(xpdev);
}

static struct xdma_pci_dev *xpdev_alloc(struct pci_dev *pdev)
{
	struct xdma_pci_dev *xpdev = kmalloc(sizeof(*xpdev), GFP_KERNEL);

	if (!xpdev)
		return NULL;
	memset(xpdev, 0, sizeof(*xpdev));

	xpdev->magic = MAGIC_DEVICE;
	xpdev->pdev = pdev;
	xpdev->user_max = MAX_USER_IRQ;
	xpdev->h2c_channel_max = XDMA_CHANNEL_NUM_MAX;
	xpdev->c2h_channel_max = XDMA_CHANNEL_NUM_MAX;

	xpdev_cnt++;
	return xpdev;
}

/* TODO: Add tc, ethtool function */
static const struct net_device_ops xdma_netdev_ops = {
	.ndo_open = xdma_netdev_open,
	.ndo_stop = xdma_netdev_close,
	.ndo_start_xmit = xdma_netdev_start_xmit,
	.ndo_setup_tc = xdma_netdev_setup_tc,
	.ndo_eth_ioctl = xdma_netdev_ioctl,
	.ndo_siocdevprivate = xdma_netdev_siocdevprivate,
	.ndo_select_queue = xdma_select_queue,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static int xdma_ethtool_get_ts_info(struct net_device * ndev, struct kernel_ethtool_ts_info * info) {
#else
static int xdma_ethtool_get_ts_info(struct net_device * ndev, struct ethtool_ts_info * info) {
#endif
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&priv->common->pdev->dev);

	/* HAT 모드: PHC 미등록(-1). RX HW TS는 코어 헤더 유래로 계속 제공 */
	info->phc_index = xpdev->ptp ? ptp_clock_index(xpdev->ptp->ptp_clock) : -1;

	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
							SOF_TIMESTAMPING_RX_SOFTWARE |
							SOF_TIMESTAMPING_SOFTWARE |
							SOF_TIMESTAMPING_TX_HARDWARE |
							SOF_TIMESTAMPING_RX_HARDWARE |
							SOF_TIMESTAMPING_RAW_HARDWARE;

	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);

	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE)
	                 | BIT(HWTSTAMP_FILTER_ALL)
	                 | BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT)
	                 | BIT(HWTSTAMP_FILTER_PTP_V2_L2_SYNC)
	                 | BIT(HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ);

	return 0;
}

static int xdma_ethtool_get_link_ksettings(struct net_device *netdev, struct ethtool_link_ksettings *cmd) {
	/* TODO: PHY also supports 100Mbps */
	cmd->base.speed = 1000;
	return 0;
}

static const char xdma_se_stat_names[][ETH_GSTRING_LEN] = {
	"se_rx_adapter_drop",
	"se_rx_adapter_data",
};

static int xdma_ethtool_get_sset_count(struct net_device *ndev, int stringset)
{
	if (stringset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	return ARRAY_SIZE(xdma_se_stat_names);
}

static void xdma_ethtool_get_strings(struct net_device *ndev, u32 stringset,
				     u8 *data)
{
	if (stringset == ETH_SS_STATS)
		memcpy(data, xdma_se_stat_names, sizeof(xdma_se_stat_names));
}

static void xdma_ethtool_get_stats(struct net_device *ndev,
				   struct ethtool_stats *stats, u64 *data)
{
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_dev *xdev = priv->common->xdev;

	/* BAR0 has unrelated TSNv3 semantics outside se_mode. Return zero rather
	 * than touching those addresses. The GPIO is read-only in the FPGA. */
	if (!se_mode || !xdev->bar[0]) {
		data[0] = 0;
		data[1] = 0;
		return;
	}

	data[0] = ioread32(xdev->bar[0] + SE_RXSTATS_DROP_OFFSET);
	data[1] = ioread32(xdev->bar[0] + SE_RXSTATS_DATA_OFFSET);
}

static const struct ethtool_ops xdma_ethtool_ops = {
	.get_ts_info = xdma_ethtool_get_ts_info,
	.get_link_ksettings = xdma_ethtool_get_link_ksettings,
	.get_sset_count = xdma_ethtool_get_sset_count,
	.get_strings = xdma_ethtool_get_strings,
	.get_ethtool_stats = xdma_ethtool_get_stats,
};

static int probe_one(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int rv = 0;
	struct xdma_pci_dev *xpdev = NULL;
	struct xdma_dev *xdev;
	void *hndl;
	struct net_device *ndev[XDMA_NUM_TOTAL_PORTS] = { NULL };
	struct xdma_private *priv[XDMA_NUM_TOTAL_PORTS] = { NULL };
	struct xdma_private_common *common = NULL;
	struct ptp_device_data *ptp_data = NULL;
	unsigned char mac_addr[ETH_ALEN];

	xpdev = xpdev_alloc(pdev);
	if (!xpdev) {
		pr_err("xpdev_alloc failed\n");
		return -ENOMEM;
	}

	hndl = xdma_device_open(DRV_MODULE_NAME, pdev, &xpdev->user_max,
			&xpdev->h2c_channel_max, &xpdev->c2h_channel_max);
	if (!hndl) {
		pr_err("xdma_device_open failed\n");
		rv = -EINVAL;
		goto err_out;
	}

	if (xpdev->user_max > MAX_USER_IRQ) {
		pr_err("Maximum users limit reached\n");
		rv = -EINVAL;
		goto err_out;
	}

	if (xpdev->h2c_channel_max > XDMA_CHANNEL_NUM_MAX) {
		pr_err("Maximun H2C channel limit reached\n");
		rv = -EINVAL;
		goto err_out;
	}

	if (xpdev->c2h_channel_max > XDMA_CHANNEL_NUM_MAX) {
		pr_err("Maximun C2H channel limit reached\n");
		rv = -EINVAL;
		goto err_out;
	}

	if (!xpdev->h2c_channel_max && !xpdev->c2h_channel_max)
		pr_warn("NO engine found!\n");

	if (xpdev->user_max) {
		u32 mask = (1 << (xpdev->user_max + 1)) - 1;

		rv = xdma_user_isr_enable(hndl, mask);
		if (rv) {
			pr_err("xdma_user_isr_enable failed\n");
			goto err_out;
		}
	}
	/* make sure no duplicate */
	xdev = xdev_find_by_pdev(pdev);
	if (!xdev) {
		pr_warn("NO xdev found!\n");
		pr_err("xdev_find_by_pdev failed\n");
		rv =  -EINVAL;
		goto err_out;
	}

	if (hndl != xdev) {
		pr_err("xdev handle mismatch\n");
		rv =  -EINVAL;
		goto err_out;
	}

	pr_info("%s xdma%d, pdev 0x%p, xdev 0x%p, 0x%p, usr %d, ch %d,%d.\n",
		dev_name(&pdev->dev), xdev->idx, pdev, xpdev, xdev,
		xpdev->user_max, xpdev->h2c_channel_max,
		xpdev->c2h_channel_max);

	xpdev->xdev = hndl;

	rv = xpdev_create_interfaces(xpdev);
	if (rv) {
		pr_err("xpdev_create_interfaces failed\n");
		goto err_out;
	}
	dev_set_drvdata(&pdev->dev, xpdev);

	/* Enable TSN and use Port 1.
	 * se_mode: BAR0 = 코어 APB. 0x04 = 코어 lookup 제어(bypass 비트)라
	 * 이 write가 스위칭 전체를 바이패스(블랙홀)시킨다 — HAT 모드에선 금지. */
	if (!se_mode)
		iowrite32(TSN_ENABLE | TSN_TX_PORT0 | TSN_RX_PORT0, xdev->bar[0] + REG_TSN_SYSTEM_CONTROL_LOW);

	if (se_mode) {
		/* LLEMAC promiscuous: 리셋 디폴트 OFF는 DA 불일치 유니캐스트를 전부
		 * 드롭한다(host 종단 통신 두절). LLEMAC MACADR가 netdev MAC과 무관한
		 * 리셋값이라 정합 필터 대신 promisc로 연다. full FPE 시 pMAC
		 * RX/reassembly도 호스트로 받으므로 pmac_rx_enable=1일 때 같이 연다. */
		static const u32 se_port_bases[] = { 0x20000, 0x40000 };
		int p;

		for (p = 0; p < ARRAY_SIZE(se_port_bases); p++) {
			u32 base = se_port_bases[p];
			u32 emac_ctrl, pmac_ctrl;
			u32 val;

			val = ioread32(xdev->bar[0] + base + SE_LLEMAC_MACADR_H_OFF);
			iowrite32(val | SE_LLEMAC_PROMISC,
				  xdev->bar[0] + base + SE_LLEMAC_MACADR_H_OFF);

			if (pmac_rx_enable) {
				val = ioread32(xdev->bar[0] + base + SE_LLEMAC_PMAC_OFF +
					       SE_LLEMAC_MACADR_H_OFF);
				iowrite32(val | SE_LLEMAC_PROMISC,
					  xdev->bar[0] + base + SE_LLEMAC_PMAC_OFF +
					  SE_LLEMAC_MACADR_H_OFF);
			}

			/* pMAC은 eMAC과 별개 LLEMAC이다. eMAC 모드 필드는 승계하되
			 * fragment_tx에 필요한 TX_EN은 항상 켜고, full FPE 기본에서
			 * RX_EN을 열어 reassembly를 유지한다. 0은 A/B 격리용이다. */
			emac_ctrl = ioread32(xdev->bar[0] + base +
						 SE_LLEMAC_MAC_CTRL_OFF);
			pmac_ctrl = emac_ctrl & ~SE_LLEMAC_RX_EN;
			pmac_ctrl |= SE_LLEMAC_TX_EN;
			if (pmac_rx_enable)
				pmac_ctrl |= SE_LLEMAC_RX_EN;
			iowrite32(pmac_ctrl, xdev->bar[0] + base + SE_LLEMAC_PMAC_OFF +
				  SE_LLEMAC_MAC_CTRL_OFF);
		}
	}

	common = kzalloc(sizeof(struct xdma_private_common), GFP_KERNEL);

	common->pdev = pdev;
	common->xdev = xpdev->xdev;
	common->rx_engine = &xdev->engine_c2h[0];
	common->tx_engine = &xdev->engine_h2c[0];

	rv = xdma_tx_ring_alloc(common);
	if (rv) {
		pr_err("xdma_tx_ring_alloc failed\n");
		goto err_out;
	}

	rv = xdma_rx_ring_alloc(common);
	if (rv) {
		pr_err("xdma_rx_ring_alloc failed\n");
		goto err_out;
	}

	spin_lock_init(&common->tx_lock);
	spin_lock_init(&common->rx_lock);

	/* Tx works for each timestamp id (1..32, Track B 결함② 32-slot 확장) */
	{
		int tsid;
		for (tsid = 1; tsid < TSN_TIMESTAMP_ID_MAX; tsid++)
			INIT_WORK(&common->tx_work[tsid], xdma_tx_work_fns[tsid]);
	}
	INIT_DELAYED_WORK(&common->rx_poll_work, xdma_rx_poll_work);
	schedule_delayed_work(&common->rx_poll_work, usecs_to_jiffies(RX_POLL_WORK_INTERVAL_US));

	common->tx_port = 0;
	common->rx_port = 0;

	/* HAT 모드: PHC ops가 구(TSNv3) sysclock BAR 레지스터를 읽으므로 코어 APB
	 * BAR에서는 무효 — PHC 미등록. gPTP는 HAT 코어+펌웨어 폐루프 담당.
	 * (호스트 PHC가 필요해지면 코어 RTC 레지스터 기반 ops로 별도 구현 — 백로그) */
	if (se_mode) {
		ptp_data = NULL;
		xpdev->ptp = NULL;
	} else {
		ptp_data = ptp_device_init(&pdev->dev, xdev);
		if (!ptp_data) {
			pr_err("ptp_device_init failed\n");
			rv = -ENOMEM;
			goto err_out;
		}

		ptp_data->xdev = xpdev->xdev;
		xpdev->ptp = ptp_data;
	}

	for (int i = 0; i < XDMA_NUM_TOTAL_PORTS; i++) {
		/* Allocate the network device */
		/* TC command requires multiple TX queues */
		ndev[i] = alloc_etherdev_mq(sizeof(struct xdma_private), TX_QUEUE_COUNT);
		if (!ndev[i]) {
			pr_err("alloc_etherdev failed\n");
			rv = -ENOMEM;
			goto err_out;
		}

		/*
		* Multiple RX queues drops throughput significantly.
		* TODO: Find out why RX queue count affects throughput
		* and see if it can be resolved in another way
		*/
		rv = netif_set_real_num_rx_queues(ndev[i], RX_QUEUE_COUNT);
		if (rv) {
			pr_err("netif_set_real_num_rx_queues failed\n");
			goto err_out;
		}

		/* Set up the network interface */
		xdev->ndev[i] = ndev[i];
		xpdev->ndev[i] = ndev[i];
		ndev[i]->netdev_ops = &xdma_netdev_ops;
		ndev[i]->ethtool_ops = &xdma_ethtool_ops;
		/* A3: 스택이 skb 할당 시 메타데이터 헤드룸/R-TAG 테일룸을 미리
		 * 확보하게 해 start_xmit의 pskb_expand_head(재할당+복사)를 회피 */
		ndev[i]->needed_headroom = TX_METADATA_SIZE;
		ndev[i]->needed_tailroom = FRER_RTAG_SIZE;
		SET_NETDEV_DEV(ndev[i], &pdev->dev);
		ndev[i]->dev_port = i >= XDMA_NUM_PORTS ? XDMA_SPECIAL_DEV_PORT_START + (i - XDMA_NUM_PORTS) : i + 1;
		priv[i] = netdev_priv(ndev[i]);
		memset(priv[i], 0, sizeof(struct xdma_private));
		priv[i]->ndev = ndev[i];
		priv[i]->port_id = i;
		priv[i]->physical_port_id = i >= XDMA_NUM_PORTS ? 0 : i;
		priv[i]->common = common;
		// priv[i]->last_rx_timestamp = 0;  // for logging

		switch (i) {
			case XDMA_FRER_PORT_ID:
				priv[i]->port_flag |= XDMA_PORT_FLAG_FRER;
				break;
			default:
				break;
		}

		/* Set the MAC address */
		get_mac_address(mac_addr, xdev, i);
		dev_addr_set(ndev[i], mac_addr);
	}

	for (int i = 0; i < XDMA_NUM_TOTAL_PORTS; i++) {
		rv = register_netdev(ndev[i]);
		if (rv < 0) {
			pr_err("register_netdev failed\n");
			goto err_out;
		}
	}
	xdma_tx_napi_setup(common, ndev[0]);
	xdma_rx_napi_setup(common, ndev[0]);
	channel_interrupts_enable(xdev, ~0);
	common->is_running = true;
	//netif_stop_queue(ndev);
	return 0;

err_out:
	pr_err("pdev 0x%p, err %d.\n", pdev, rv);

	if (common != NULL) {
		xdma_tx_ring_free(common);
		xdma_rx_ring_free(common);
		cancel_delayed_work(&common->rx_poll_work);
		kfree(common);
	}

	for (int i = 0; i < XDMA_NUM_TOTAL_PORTS; i++) {
		if (ndev[i] != NULL) {
			unregister_netdev(ndev[i]);
			free_netdev(ndev[i]);
		}
	}

	if (ptp_data != NULL) {
		ptp_device_destroy(ptp_data);
	}
	xpdev_free(xpdev);
	dev_set_drvdata(&pdev->dev, NULL);
	return rv;
}

static void remove_one(struct pci_dev *pdev)
{
	struct xdma_pci_dev *xpdev;
	struct xdma_dev *xdev;
	struct xdma_private *priv;
	struct xdma_private_common *common;
	struct ptp_device_data *ptp_data;

	if (!pdev)
		return;

	xpdev = dev_get_drvdata(&pdev->dev);
	if (!xpdev)
		return;

	pr_info("pdev 0x%p, xdev 0x%p, 0x%p.\n",
		pdev, xpdev, xpdev->xdev);

	for (int i = 0; i < XDMA_NUM_TOTAL_PORTS; i++) {
		if (!xpdev->ndev[i]) {
			pr_err("ndev is NULL\n");
			return;
		}
	}
	priv = netdev_priv(xpdev->ndev[0]);
	common = priv->common;
	xdev = xpdev->xdev;
	ptp_data = xpdev->ptp;
	common->is_running = false;
	cancel_delayed_work(&common->rx_poll_work);
	/* 순서 중요: 먼저 netdev를 내려 close 경로로 엔진을 정지시킨 뒤에
	 * NAPI를 지우고 링을 해제한다. 링을 먼저 풀면 실행 중인 RX 엔진이
	 * 해제된 코히런트 메모리에 DMA를 계속 쓴다(use-after-free). */
	for (int i = 0; i < XDMA_NUM_TOTAL_PORTS; i++) {
		unregister_netdev(xpdev->ndev[i]);
	}
	xdma_tx_napi_teardown(common);
	xdma_rx_napi_teardown(common);
	xdma_tx_ring_free(common);
	xdma_rx_ring_free(common);
	for (int i = 0; i < XDMA_NUM_TOTAL_PORTS; i++) {
		free_netdev(xpdev->ndev[i]);
	}
	kfree(common);
	if (ptp_data)
		ptp_device_destroy(ptp_data);
	xpdev_free(xpdev);
	dev_set_drvdata(&pdev->dev, NULL);
}

static pci_ers_result_t xdma_error_detected(struct pci_dev *pdev,
					pci_channel_state_t state)
{
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);

	switch (state) {
	case pci_channel_io_normal:
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		pr_warn("dev 0x%p,0x%p, frozen state error, reset controller\n",
			pdev, xpdev);
		xdma_device_offline(pdev, xpdev->xdev);
		pci_disable_device(pdev);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		pr_warn("dev 0x%p,0x%p, failure state error, req. disconnect\n",
			pdev, xpdev);
		return PCI_ERS_RESULT_DISCONNECT;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t xdma_slot_reset(struct pci_dev *pdev)
{
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);

	pr_info("0x%p restart after slot reset\n", xpdev);
	if (pci_enable_device_mem(pdev)) {
		pr_info("0x%p failed to renable after slot reset\n", xpdev);
		return PCI_ERS_RESULT_DISCONNECT;
	}

	pci_set_master(pdev);
	pci_restore_state(pdev);
	pci_save_state(pdev);
	xdma_device_online(pdev, xpdev->xdev);

	return PCI_ERS_RESULT_RECOVERED;
}

static void xdma_error_resume(struct pci_dev *pdev)
{
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);

	pr_info("dev 0x%p,0x%p.\n", pdev, xpdev);
#if PCI_AER_NAMECHANGE
	pci_aer_clear_nonfatal_status(pdev);
#else
	pci_cleanup_aer_uncorrect_error_status(pdev);
#endif
}

#if KERNEL_VERSION(4, 13, 0) <= LINUX_VERSION_CODE
static void xdma_reset_prepare(struct pci_dev *pdev)
{
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);

	pr_info("dev 0x%p,0x%p.\n", pdev, xpdev);
	xdma_device_offline(pdev, xpdev->xdev);
}

static void xdma_reset_done(struct pci_dev *pdev)
{
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);

	pr_info("dev 0x%p,0x%p.\n", pdev, xpdev);
	xdma_device_online(pdev, xpdev->xdev);
}

#elif KERNEL_VERSION(3, 16, 0) <= LINUX_VERSION_CODE
static void xdma_reset_notify(struct pci_dev *pdev, bool prepare)
{
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&pdev->dev);

	pr_info("dev 0x%p,0x%p, prepare %d.\n", pdev, xpdev, prepare);

	if (prepare)
		xdma_device_offline(pdev, xpdev->xdev);
	else
		xdma_device_online(pdev, xpdev->xdev);
}
#endif

static const struct pci_error_handlers xdma_err_handler = {
	.error_detected	= xdma_error_detected,
	.slot_reset	= xdma_slot_reset,
	.resume		= xdma_error_resume,
#if KERNEL_VERSION(4, 13, 0) <= LINUX_VERSION_CODE
	.reset_prepare	= xdma_reset_prepare,
	.reset_done	= xdma_reset_done,
#elif KERNEL_VERSION(3, 16, 0) <= LINUX_VERSION_CODE
	.reset_notify	= xdma_reset_notify,
#endif
};

static struct pci_driver pci_driver = {
	.name = DRV_MODULE_NAME,
	.id_table = pci_ids,
	.probe = probe_one,
	.remove = remove_one,
	.err_handler = &xdma_err_handler,
};

static int xdma_mod_init(void)
{
	int rv;
	pr_info("%s", version);

	if (desc_blen_max > XDMA_DESC_BLEN_MAX)
		desc_blen_max = XDMA_DESC_BLEN_MAX;
	pr_info("desc_blen_max: 0x%x/%u, timeout: h2c %u c2h %u sec.\n",
		desc_blen_max, desc_blen_max, h2c_timeout, c2h_timeout);

	rv = xdma_cdev_init();
	if (rv < 0)
		return rv;

	return pci_register_driver(&pci_driver);
}

static void xdma_mod_exit(void)
{
	/* unregister this driver from the PCI bus driver */
	dbg_init("pci_unregister_driver.\n");
	pci_unregister_driver(&pci_driver);
	xdma_cdev_cleanup();
}

module_init(xdma_mod_init);
module_exit(xdma_mod_exit);
