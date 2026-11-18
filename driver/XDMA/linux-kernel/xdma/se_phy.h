/* SPDX-License-Identifier: GPL-2.0 */
#ifndef XDMA_SE_PHY_H
#define XDMA_SE_PHY_H

#include <linux/types.h>

struct xdma_private_common;
struct ifreq;
struct net_device;

void xdma_se_phy_init(struct xdma_private_common *common);
void xdma_se_phy_open(struct net_device *ndev);
void xdma_se_phy_close(struct net_device *ndev);
int xdma_se_mii_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd);
bool xdma_se_mdio_owned_offset(loff_t offset);

#endif
