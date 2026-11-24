// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mii.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>

#include "libxdma.h"
#include "xdma_netdev.h"
#include "se_phy.h"

#define SE_MDIO_CFG           0x20
#define SE_MDIO_CTRL          0x24
#define SE_MDIO_RD            0x28
#define SE_MDIO_ENABLE        BIT(8)
#define SE_MDIO_READ          (2U << 30)
#define SE_MDIO_START         BIT(8)
#define SE_MDIO_READY         BIT(0)
#define SE_MDIO_DIVIDER       0x18
#define SE_PHY_ID1            0x0022
#define SE_PHY_ID2_MASK       0xfff0
#define SE_PHY_ID2_KSZ9031    0x1620
#define SE_LINK_POLL_MS       250

extern unsigned int se_mode;
static bool se_phy_poll_enabled = true;
module_param_named(tsn_phy_poll, se_phy_poll_enabled, bool, 0444);
MODULE_PARM_DESC(tsn_phy_poll, "Track physical links in TSN mode (default on)");

static const unsigned int mdio_base[] = { 0x20000, 0x40000 };

static bool se_phy_managed(void)
{
        return se_mode && se_phy_poll_enabled;
}

static int se_mdio_read(void __iomem *base, int addr, int reg)
{
        u32 ctrl;
        int ret;

        iowrite32(SE_MDIO_READ | (addr << 24) | (reg << 16) | SE_MDIO_START,
                  base + SE_MDIO_CTRL);
        /* READY can still describe the previous transfer immediately after START. */
        usleep_range(200, 300);
        ret = read_poll_timeout(ioread32, ctrl, ctrl & SE_MDIO_READY, 40, 2500,
                                false, base + SE_MDIO_CTRL);
        if (ret)
                return ret;
        if (ctrl == U32_MAX)
                return -EIO;
        return ioread32(base + SE_MDIO_RD) & 0xffff;
}

static int se_phy_address(struct xdma_private_common *common, int port,
                          void __iomem *base)
{
        int addr, id, found = -ENODEV;

        if (common->phy_addr[port] >= 0)
                return common->phy_addr[port];
        for (addr = 0; addr < 32; addr++) {
                id = se_mdio_read(base, addr, MII_PHYSID1);
                if (id < 0)
                        return id;
                if (id != SE_PHY_ID1)
                        continue;
                id = se_mdio_read(base, addr, MII_PHYSID2);
                if (id < 0)
                        return id;
                if ((id & SE_PHY_ID2_MASK) != SE_PHY_ID2_KSZ9031)
                        continue;
                if (found >= 0)
                        return -ENODEV;
                found = addr;
        }
        common->phy_addr[port] = found;
        return found;
}

/* Both polling and the read-only MII ioctl run with phy_lock held. */
static int se_phy_read(struct xdma_private_common *common, int port, int reg,
                       int requested_addr, int *addr_out)
{
        struct xdma_dev *xdev = common->xdev;
        void __iomem *base;
        u32 cfg;
        int addr, value;
        bool enabled_here;

        if (!xdev->bar[0] || pci_resource_len(common->pdev, 0) < 0x40030 ||
            pci_channel_offline(common->pdev) ||
            xdma_device_flag_check(xdev, XDEV_FLAG_OFFLINE))
                return -ENODEV;
        base = xdev->bar[0] + mdio_base[port];
        cfg = ioread32(base + SE_MDIO_CFG);
        if (cfg == U32_MAX)
                return -EIO;
        enabled_here = !(cfg & SE_MDIO_ENABLE);
        if (enabled_here) {
                iowrite32((cfg & ~0xffU) | SE_MDIO_ENABLE | SE_MDIO_DIVIDER,
                          base + SE_MDIO_CFG);
                usleep_range(200, 300);
        }
        addr = se_phy_address(common, port, base);
        if (addr < 0) {
                value = addr;
        } else if (requested_addr >= 0 && requested_addr != addr) {
                value = -ENODEV;
        } else {
                *addr_out = addr;
                value = se_mdio_read(base, addr, reg);
                if (reg == MII_BMSR && value >= 0)
                        value = se_mdio_read(base, addr, reg);
                if (value == 0xffff || (reg == MII_BMSR && value == 0))
                        value = -EIO;
                if (value < 0)
                        common->phy_addr[port] = -1;
        }
        if (enabled_here)
                iowrite32(cfg, base + SE_MDIO_CFG);
        return value;
}

static void se_phy_poll(struct work_struct *work)
{
        struct xdma_private_common *common =
                container_of(to_delayed_work(work), struct xdma_private_common, phy_poll);
        bool up[XDMA_NUM_PORTS] = {};
        int port, addr, bmsr;

        mutex_lock(&common->phy_lock);
        for (port = 0; port < XDMA_NUM_PORTS; port++) {
                if (!(common->phy_open_mask & (BIT(port) | BIT(XDMA_FRER_PORT_ID))))
                        continue;
                bmsr = se_phy_read(common, port, MII_BMSR, -1, &addr);
                up[port] = bmsr >= 0 && (bmsr & BMSR_LSTATUS);
        }
        for (port = 0; port < XDMA_NUM_TOTAL_PORTS; port++) {
                struct net_device *ndev = common->xdev->ndev[port];
                bool linked;

                if (!(common->phy_open_mask & BIT(port)))
                        continue;
                linked = port < XDMA_NUM_PORTS ? up[port] : up[0] || up[1];
                if (linked)
                        netif_carrier_on(ndev);
                else
                        netif_carrier_off(ndev);
        }
        if (common->phy_open_mask)
                schedule_delayed_work(&common->phy_poll, msecs_to_jiffies(SE_LINK_POLL_MS));
        mutex_unlock(&common->phy_lock);
}

void xdma_se_phy_init(struct xdma_private_common *common)
{
        int port;

        mutex_init(&common->phy_lock);
        INIT_DELAYED_WORK(&common->phy_poll, se_phy_poll);
        for (port = 0; port < XDMA_NUM_PORTS; port++)
                common->phy_addr[port] = -1;
}

void xdma_se_phy_open(struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;

        if (!se_phy_managed()) {
                netif_carrier_on(ndev);
                return;
        }
        netif_carrier_off(ndev);
        mutex_lock(&common->phy_lock);
        common->phy_open_mask |= BIT(priv->port_id);
        mod_delayed_work(system_wq, &common->phy_poll, 0);
        mutex_unlock(&common->phy_lock);
}

void xdma_se_phy_close(struct net_device *ndev)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        bool last;

        mutex_lock(&common->phy_lock);
        common->phy_open_mask &= ~BIT(priv->port_id);
        last = !common->phy_open_mask;
        netif_carrier_off(ndev);
        mutex_unlock(&common->phy_lock);
        /* ndo_open/stop are serialized by RTNL. Never wait with phy_lock held. */
        if (last)
                cancel_delayed_work_sync(&common->phy_poll);
}

int xdma_se_mii_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
        struct xdma_private *priv = netdev_priv(ndev);
        struct xdma_private_common *common = priv->common;
        struct mii_ioctl_data *data = if_mii(ifr);
        int addr, value;

        if (!se_phy_managed() || priv->port_id >= XDMA_NUM_PORTS)
                return -EOPNOTSUPP;
        if (data->reg_num >= 32 || (cmd == SIOCGMIIREG && data->phy_id >= 32))
                return -EINVAL;
        mutex_lock(&common->phy_lock);
        value = se_phy_read(common, priv->port_id, data->reg_num,
                            cmd == SIOCGMIIPHY ? -1 : data->phy_id, &addr);
        mutex_unlock(&common->phy_lock);
        if (value < 0)
                return value;
        data->phy_id = addr;
        data->val_out = value;
        return 0;
}

bool xdma_se_mdio_owned_offset(loff_t offset)
{
        int p;

        if (!se_phy_managed())
                return false;
        for (p = 0; p < ARRAY_SIZE(mdio_base); p++) {
                if (offset >= mdio_base[p] + SE_MDIO_CFG &&
                    offset <= mdio_base[p] + 0x2c)
                        return true;
        }
        return false;
}
