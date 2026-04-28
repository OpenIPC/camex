// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * camex_kmod - optional kernel-mode UDP tunnel for embedded targets
 *
 * Creates a point-to-point netdevice (camex0) that encapsulates raw IP
 * packets in UDP datagrams, mirroring the camexd userspace daemon but
 * running entirely in kernel space for lower overhead.
 *
 * Module parameters:
 *   remote=<dotted-quad>   Remote peer IPv4 address (required)
 *   port=<1-65535>         UDP port for both endpoints (default: 7777)
 *   mtu=<bytes>            Interface MTU (default: 1400)
 */

#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <net/sock.h>

#define CAMEX_DEFAULT_PORT  7777
#define CAMEX_DEFAULT_MTU   1400
#define CAMEX_MAX_MTU       65515
#define CAMEX_MIN_MTU       576

static char *remote = "";
static int   port   = CAMEX_DEFAULT_PORT;
static int   mtu    = CAMEX_DEFAULT_MTU;

module_param(remote, charp, 0444);
MODULE_PARM_DESC(remote, "Remote peer IPv4 address (required)");

module_param(port, int, 0444);
MODULE_PARM_DESC(port, "UDP port (default: 7777)");

module_param(mtu, int, 0444);
MODULE_PARM_DESC(mtu, "Interface MTU in bytes (default: 1400)");

struct camex_priv {
    struct net_device       *dev;
    struct socket           *sock;
    struct sockaddr_in       peer;
    struct task_struct      *rx_thread;
    int                      mtu;
};

static struct net_device *camex_dev;

/* ------------------------------------------------------------------ */
/* netdevice operations                                                 */
/* ------------------------------------------------------------------ */

static int camex_open(struct net_device *dev)
{
    netif_start_queue(dev);
    return 0;
}

static int camex_stop(struct net_device *dev)
{
    netif_stop_queue(dev);
    return 0;
}

static netdev_tx_t camex_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct camex_priv *priv = netdev_priv(dev);
    struct kvec iov = { skb->data, skb->len };
    struct msghdr msg = {
        .msg_name    = &priv->peer,
        .msg_namelen = sizeof(priv->peer),
        .msg_flags   = MSG_DONTWAIT | MSG_NOSIGNAL,
    };
    int len = skb->len;
    int ret;

    ret = kernel_sendmsg(priv->sock, &msg, &iov, 1, len);
    if (ret > 0) {
        dev->stats.tx_packets++;
        dev->stats.tx_bytes += len;
    } else {
        dev->stats.tx_errors++;
    }

    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

static const struct net_device_ops camex_netdev_ops = {
    .ndo_open       = camex_open,
    .ndo_stop       = camex_stop,
    .ndo_start_xmit = camex_xmit,
};

static void camex_setup(struct net_device *dev)
{
    dev->netdev_ops      = &camex_netdev_ops;
    dev->type            = ARPHRD_NONE;
    dev->hard_header_len = 0;
    dev->addr_len        = 0;
    dev->flags           = IFF_POINTOPOINT | IFF_NOARP;
    dev->tx_queue_len    = 500;
}

/* ------------------------------------------------------------------ */
/* Receive kthread: blocking kernel_recvmsg loop                       */
/* ------------------------------------------------------------------ */

static int camex_rx_thread(void *data)
{
    struct net_device *dev  = data;
    struct camex_priv *priv = netdev_priv(dev);
    unsigned char     *buf;
    /*
     * bufsz = mtu: in a point-to-point tunnel both peers use the same
     * MTU, so no UDP payload will exceed this limit.  Using the MTU
     * directly keeps heap usage minimal on constrained embedded targets.
     */
    int bufsz = priv->mtu;

    buf = kmalloc(bufsz, GFP_KERNEL);
    if (!buf) {
        pr_err("camex: failed to allocate rx buffer\n");
        return -ENOMEM;
    }

    while (!kthread_should_stop()) {
        struct kvec     iov = { buf, bufsz };
        struct msghdr   msg = {};
        struct sk_buff *skb;
        __be16          proto;
        int             len;

        len = kernel_recvmsg(priv->sock, &msg, &iov, 1, bufsz, 0);
        if (len <= 0) {
            /* socket was shut down or a transient error; check stop flag */
            if (kthread_should_stop())
                break;
            msleep(1);
            continue;
        }

        if (!netif_running(dev)) {
            dev->stats.rx_dropped++;
            continue;
        }

        /* Detect IP version from the first nibble */
        if ((buf[0] >> 4) == 6)
            proto = htons(ETH_P_IPV6);
        else
            proto = htons(ETH_P_IP);

        /* +2 so IP header lands on a 4-byte boundary after skb_reserve */
        skb = netdev_alloc_skb(dev, len + 2);
        if (!skb) {
            dev->stats.rx_dropped++;
            continue;
        }

        /* Reserve 2 bytes so IP header lands on a 4-byte boundary */
        skb_reserve(skb, 2);
        skb_put_data(skb, buf, len);

        skb->dev      = dev;
        skb->protocol = proto;
        skb_reset_mac_header(skb);
        skb_reset_network_header(skb);

        dev->stats.rx_packets++;
        dev->stats.rx_bytes += len;

        netif_rx(skb);
    }

    kfree(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init camex_init(void)
{
    struct camex_priv *priv;
    struct sockaddr_in local_addr;
    int ret;

    if (!remote || !remote[0]) {
        pr_err("camex: module parameter 'remote' is required\n");
        return -EINVAL;
    }

    if (port < 1 || port > 65535) {
        pr_err("camex: invalid port %d\n", port);
        return -EINVAL;
    }

    if (mtu < CAMEX_MIN_MTU || mtu > CAMEX_MAX_MTU) {
        pr_err("camex: mtu %d out of range [%d, %d]\n",
               mtu, CAMEX_MIN_MTU, CAMEX_MAX_MTU);
        return -EINVAL;
    }

    camex_dev = alloc_netdev(sizeof(*priv), "camex%d",
                             NET_NAME_UNKNOWN, camex_setup);
    if (!camex_dev)
        return -ENOMEM;

    priv       = netdev_priv(camex_dev);
    priv->dev  = camex_dev;
    priv->mtu  = mtu;
    camex_dev->mtu = mtu;

    /* Resolve remote peer address */
    memset(&priv->peer, 0, sizeof(priv->peer));
    priv->peer.sin_family      = AF_INET;
    priv->peer.sin_port        = htons((u16)port);
    priv->peer.sin_addr.s_addr = in_aton(remote);
    if (!priv->peer.sin_addr.s_addr) {
        pr_err("camex: invalid remote address '%s'\n", remote);
        ret = -EINVAL;
        goto err_free_dev;
    }

    /* Create UDP socket */
    ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                           &priv->sock);
    if (ret < 0) {
        pr_err("camex: sock_create_kern failed: %d\n", ret);
        goto err_free_dev;
    }

    /* Bind to 0.0.0.0:<port> */
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family      = AF_INET;
    local_addr.sin_port        = htons((u16)port);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    ret = kernel_bind(priv->sock, (struct sockaddr *)&local_addr,
                      sizeof(local_addr));
    if (ret < 0) {
        pr_err("camex: kernel_bind port %d failed: %d\n", port, ret);
        goto err_release_sock;
    }

    ret = register_netdev(camex_dev);
    if (ret < 0) {
        pr_err("camex: register_netdev failed: %d\n", ret);
        goto err_release_sock;
    }

    /* Start the receive kthread */
    priv->rx_thread = kthread_run(camex_rx_thread, camex_dev, "camex_rx");
    if (IS_ERR(priv->rx_thread)) {
        ret = PTR_ERR(priv->rx_thread);
        pr_err("camex: kthread_run failed: %d\n", ret);
        goto err_unreg_dev;
    }

    pr_info("camex: tunnel %s  local=:%d  remote=%s:%d  mtu=%d\n",
            camex_dev->name, port, remote, port, mtu);
    return 0;

err_unreg_dev:
    unregister_netdev(camex_dev);
err_release_sock:
    sock_release(priv->sock);
err_free_dev:
    free_netdev(camex_dev);
    return ret;
}

static void __exit camex_exit(void)
{
    struct camex_priv *priv = netdev_priv(camex_dev);

    /* Shut down the socket to unblock kernel_recvmsg, then stop thread */
    kernel_sock_shutdown(priv->sock, SHUT_RDWR);
    kthread_stop(priv->rx_thread);

    unregister_netdev(camex_dev);
    sock_release(priv->sock);
    free_netdev(camex_dev);
    pr_info("camex: unloaded\n");
}

module_init(camex_init);
module_exit(camex_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenIPC");
MODULE_DESCRIPTION("Minimal UDP/TUN tunnel for embedded targets");
MODULE_VERSION("1.0.0");
