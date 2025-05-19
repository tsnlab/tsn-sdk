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

#define DRV_MODULE_NAME		"xdma"
#define DRV_MODULE_DESC		"Xilinx XDMA Reference Driver"

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

static void get_mac_address(char* mac_addr, struct xdma_dev *xdev) {
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
	.ndo_select_queue = xdma_select_queue,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static int xdma_ethtool_get_ts_info(struct net_device * ndev, struct kernel_ethtool_ts_info * info) {
#else
static int xdma_ethtool_get_ts_info(struct net_device * ndev, struct ethtool_ts_info * info) {
#endif
	struct xdma_private *priv = netdev_priv(ndev);
	struct xdma_pci_dev *xpdev = dev_get_drvdata(&priv->pdev->dev);

	info->phc_index = ptp_clock_index(xpdev->ptp->ptp_clock);

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

static const struct ethtool_ops xdma_ethtool_ops = {
	.get_ts_info = xdma_ethtool_get_ts_info,
	.get_link_ksettings = xdma_ethtool_get_link_ksettings,
};

static int probe_one(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int rv = 0;
	struct xdma_pci_dev *xpdev = NULL;
	struct xdma_dev *xdev;
	void *hndl;
	struct net_device *ndev;
	struct xdma_private *priv;
	struct ptp_device_data *ptp_data;
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

	/* Set the TSN register to 0x1 */
	iowrite32(TSN_ENABLE, xdev->bar[0] + REG_TSN_SYSTEM_CONTROL_LOW);

	/* Allocate the network device */
	/* TC command requires multiple TX queues */
	ndev = alloc_etherdev_mq(sizeof(struct xdma_private), TX_QUEUE_COUNT);
	if (!ndev) {
		pr_err("alloc_etherdev failed\n");
		rv = -ENOMEM;
		goto err_out;
	}
	/*
	 * Multiple RX queues drops throughput significantly.
	 * TODO: Find out why RX queue count affects throughput
	 * and see if it can be resolved in another way
	 */
	rv = netif_set_real_num_rx_queues(ndev, RX_QUEUE_COUNT);
	if (rv) {
		pr_err("netif_set_real_num_rx_queues failed\n");
		goto err_out;
	}

	xdev->ndev = ndev;
	xpdev->ndev = ndev;

	/* Set up the network interface */
	ndev->netdev_ops = &xdma_netdev_ops;
	ndev->ethtool_ops = &xdma_ethtool_ops;
	SET_NETDEV_DEV(ndev, &pdev->dev);
	priv = netdev_priv(ndev);
	memset(priv, 0, sizeof(struct xdma_private));
	priv->pdev = pdev;
	priv->ndev = ndev;
	priv->xdev = xpdev->xdev;
	priv->rx_engine = &xdev->engine_c2h[0];
	priv->tx_engine = &xdev->engine_h2c[0];

	priv->tx_desc = dma_alloc_coherent(
				&pdev->dev,
				sizeof(struct xdma_desc),
				&priv->tx_bus_addr,
				GFP_KERNEL);
	if (!priv->tx_desc) {
		pr_err("dma_alloc_coherent failed\n");
		free_netdev(ndev);
		rv = -ENOMEM;
		goto err_out;
	}

	priv->rx_desc = dma_alloc_coherent(
				&pdev->dev,
				sizeof(struct xdma_desc),
				&priv->rx_bus_addr,
				GFP_KERNEL);
	if (!priv->rx_desc) {
		pr_err("dma_alloc_coherent failed\n");
		free_netdev(ndev);
		rv = -ENOMEM;
		goto err_out;
	}

	priv->res = dma_alloc_coherent(&pdev->dev, sizeof(struct xdma_result), &priv->res_dma_addr, GFP_KERNEL);
	if (!priv->res) {
		pr_err("res dma_alloc_coherent failed\n");
		free_netdev(ndev);
		rv = -ENOMEM;
		goto err_out;
	}

	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->rx_lock);

	/* Set the MAC address */
	get_mac_address(mac_addr, xdev);
	dev_addr_set(ndev, mac_addr);

	priv->rx_buffer = dma_alloc_coherent(&pdev->dev, XDMA_BUFFER_SIZE, &priv->rx_dma_addr, GFP_KERNEL);
	if (!priv->rx_buffer) {
		pr_err("buffer dma_alloc_coherent failed\n");
		free_netdev(ndev);
		rv = -ENOMEM;
		goto err_out;
	}

	/* Tx works for each timestamp id */
	INIT_WORK(&priv->tx_work[1], xdma_tx_work1);
	INIT_WORK(&priv->tx_work[2], xdma_tx_work2);
	INIT_WORK(&priv->tx_work[3], xdma_tx_work3);
	INIT_WORK(&priv->tx_work[4], xdma_tx_work4);

	ptp_data = ptp_device_init(&pdev->dev, xdev);
	if (!ptp_data) {
		pr_err("ptp_device_init failed\n");
		free_netdev(ndev);
		rv = -ENOMEM;
		goto err_out;
	}

	ptp_data->xdev = xpdev->xdev;
	xpdev->ptp = ptp_data;

	rv = register_netdev(ndev);
	if (rv < 0) {
		free_netdev(ndev);
		pr_err("register_netdev failed\n");
		goto err_out;
	}
	channel_interrupts_enable(xdev, ~0);
	//netif_stop_queue(ndev);
	return 0;

err_out:
	pr_err("pdev 0x%p, err %d.\n", pdev, rv);
	xpdev_free(xpdev);
	return rv;
}

static void remove_one(struct pci_dev *pdev)
{
	struct xdma_pci_dev *xpdev;
	struct xdma_dev *xdev;
	struct net_device *ndev;
	struct xdma_private *priv;
	struct ptp_device_data *ptp_data;

	if (!pdev)
		return;

	xpdev = dev_get_drvdata(&pdev->dev);
	if (!xpdev)
		return;

	pr_info("pdev 0x%p, xdev 0x%p, 0x%p.\n",
		pdev, xpdev, xpdev->xdev);

	ndev = xpdev->ndev;
	if (!ndev) {
		pr_err("ndev is NULL\n");
		return;
	}
	priv = netdev_priv(ndev);
	xdev = xpdev->xdev;
	ptp_data = xpdev->ptp;
	dma_free_coherent(&pdev->dev, sizeof(struct xdma_desc), priv->tx_desc, priv->tx_bus_addr);
	dma_free_coherent(&pdev->dev, sizeof(struct xdma_desc), priv->rx_desc, priv->rx_bus_addr);
	dma_free_coherent(&pdev->dev, XDMA_BUFFER_SIZE, priv->rx_buffer, priv->rx_dma_addr);
	dma_free_coherent(&pdev->dev, sizeof(struct xdma_result), priv->res, priv->res_dma_addr);
	unregister_netdev(ndev);
	ptp_device_destroy(ptp_data);
	free_netdev(ndev);
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
