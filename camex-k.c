// SPDX-License-Identifier: GPL-2.0
/*
 * camex.c — Minimal virtual TUN driver (no tun.ko dependency)
 *
 * Creates /dev/camex + net interface "camex".
 * Compatible with Linux 3.x – 6.x kernels.
 * IPv4 only.
 *
 * Usage (userspace):
 *   fd = open("/dev/camex", O_RDWR);
 *   // read() → raw IPv4 packets from kernel to userspace
 *   // write() → inject raw IPv4 packets into kernel stack
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/ip.h>
#include <linux/version.h>
#include "version.h"

/* ================================================================
 * Kernel version compatibility shims
 *
 * All API differences between 3.x and 6.x are isolated here.
 * The driver body below does not contain any #if VERSION guards.
 * ================================================================ */

/*
 * NET_NAME_PREDICTABLE: controls sysfs naming hint.
 * Introduced in 3.17. On older kernels alloc_netdev takes 3 args.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)
# define CAMEX_ALLOC_NETDEV(sz, name, setup) \
    alloc_netdev((sz), (name), NET_NAME_PREDICTABLE, (setup))
#else
# define CAMEX_ALLOC_NETDEV(sz, name, setup) \
    alloc_netdev((sz), (name), (setup))
#endif

/*
 * min_mtu / max_mtu fields in net_device appeared in 4.10.
 * Guard their assignment with this flag.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
# define CAMEX_NO_MTU_FIELDS 1
#endif

/*
 * __poll_t type and EPOLL* constants replaced the old POLL* names
 * and unsigned-int return from ->poll() starting in 4.16.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
# define CAMEX_POLL_T       __poll_t
# define CAMEX_EPOLLIN      EPOLLIN
# define CAMEX_EPOLLOUT     EPOLLOUT
# define CAMEX_EPOLLRDNORM  EPOLLRDNORM
# define CAMEX_EPOLLWRNORM  EPOLLWRNORM
#else
# define CAMEX_POLL_T       unsigned int
# define CAMEX_EPOLLIN      POLLIN
# define CAMEX_EPOLLOUT     POLLOUT
# define CAMEX_EPOLLRDNORM  POLLRDNORM
# define CAMEX_EPOLLWRNORM  POLLWRNORM
#endif

/*
 * skb_put() return type changed from (unsigned char *) to (void *)
 * in 4.20. Cast explicitly so both old and new compilers are happy.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
# define CAMEX_SKB_PUT(skb, len) ((unsigned char *)skb_put((skb), (len)))
#else
# define CAMEX_SKB_PUT(skb, len) (skb_put((skb), (len)))
#endif

/*
 * NET_IP_ALIGN: preferred alignment offset inside an skb to avoid
 * unaligned IP-header accesses on some architectures. Usually 2.
 * Define a fallback in case the kernel header omits it.
 */
#ifndef NET_IP_ALIGN
# define NET_IP_ALIGN 2
#endif

/* ================================================================
 * Driver constants
 * ================================================================ */

#define DRV_NAME     "camex"
#define DEV_NAME     "camex"
#define QUEUE_LIMIT   64    /* max packets queued for userspace read() */
#define CAMEX_MTU    1500

/* ================================================================
 * Data structures
 * ================================================================ */

/**
 * struct pkt_node - one node in the per-device RX queue.
 * @list: linked list linkage
 * @skb:  the queued socket buffer (owned by this node until freed)
 */
struct pkt_node {
    struct list_head  list;
    struct sk_buff   *skb;
};

/**
 * struct camex_priv - private data embedded inside net_device.
 * @dev:      back-pointer to our net_device
 * @rx_queue: FIFO of pkt_node; filled by xmit, drained by read()
 * @rx_count: current depth of rx_queue
 * @rx_lock:  spinlock protecting rx_queue and rx_count
 * @rx_wait:  wait queue; userspace blocks here when queue is empty
 * @attached: true while /dev/camex is held open by a process
 */
struct camex_priv {
    struct net_device *dev;

    struct list_head   rx_queue;
    int                rx_count;
    spinlock_t         rx_lock;
    wait_queue_head_t  rx_wait;

    bool               attached;
};

/* Single global instance — one device, one user at a time. */
static struct camex_priv *g_priv;

/* ================================================================
 * net_device operations
 * ================================================================ */

static int camex_net_open(struct net_device *dev)
{
    netif_start_queue(dev);
    return 0;
}

static int camex_net_stop(struct net_device *dev)
{
    netif_stop_queue(dev);
    return 0;
}

/**
 * camex_net_xmit - called by the kernel IP stack to transmit a packet.
 *
 * Instead of sending it over a wire we enqueue it for userspace to
 * pick up via read().  If userspace is not attached or the queue is
 * full the packet is silently dropped (counted in tx_dropped).
 */
static netdev_tx_t camex_net_xmit(struct sk_buff *skb,
                                   struct net_device *dev)
{
    struct camex_priv *priv = netdev_priv(dev);
    struct pkt_node   *node;
    unsigned long      flags;

    if (!priv->attached) {
        dev->stats.tx_dropped++;
        dev_kfree_skb(skb);
        return NETDEV_TX_OK;
    }

    spin_lock_irqsave(&priv->rx_lock, flags);

    if (priv->rx_count >= QUEUE_LIMIT) {
        spin_unlock_irqrestore(&priv->rx_lock, flags);
        dev->stats.tx_dropped++;
        dev_kfree_skb(skb);
        return NETDEV_TX_OK;
    }

    node = kmalloc(sizeof(*node), GFP_ATOMIC);
    if (!node) {
        spin_unlock_irqrestore(&priv->rx_lock, flags);
        dev->stats.tx_dropped++;
        dev_kfree_skb(skb);
        return NETDEV_TX_OK;
    }

    node->skb = skb;
    list_add_tail(&node->list, &priv->rx_queue);
    priv->rx_count++;
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += skb->len;

    spin_unlock_irqrestore(&priv->rx_lock, flags);

    wake_up_interruptible(&priv->rx_wait);
    return NETDEV_TX_OK;
}

static const struct net_device_ops camex_netdev_ops = {
    .ndo_open       = camex_net_open,
    .ndo_stop       = camex_net_stop,
    .ndo_start_xmit = camex_net_xmit,
};

/**
 * camex_net_setup - called by alloc_netdev to initialise net_device fields.
 *
 * We configure a point-to-point, ARP-less Layer-3 interface, exactly
 * like the upstream tun driver does for IFF_TUN mode.
 */
static void camex_net_setup(struct net_device *dev)
{
    dev->netdev_ops      = &camex_netdev_ops;
    dev->flags           = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
    dev->type            = ARPHRD_NONE;
    dev->mtu             = CAMEX_MTU;
    dev->hard_header_len = 0;
    dev->addr_len        = 0;
    dev->tx_queue_len    = 500;

#ifndef CAMEX_NO_MTU_FIELDS
    dev->min_mtu = 68;
    dev->max_mtu = 65535;
#endif
}

/* ================================================================
 * Character device (file_operations)
 * ================================================================ */

static int camex_chr_open(struct inode *inode, struct file *filp)
{
    if (!g_priv)
        return -ENODEV;
    if (g_priv->attached)
        return -EBUSY;  /* only one process may hold the fd */

    g_priv->attached   = true;
    filp->private_data = g_priv;

    netif_carrier_on(g_priv->dev);
    pr_info("camex: userspace attached\n");
    return 0;
}

static int camex_chr_release(struct inode *inode, struct file *filp)
{
    struct camex_priv *priv = filp->private_data;
    struct pkt_node   *node, *tmp;
    unsigned long      flags;

    priv->attached = false;
    netif_carrier_off(priv->dev);

    /* Discard any packets that were never read. */
    spin_lock_irqsave(&priv->rx_lock, flags);
    list_for_each_entry_safe(node, tmp, &priv->rx_queue, list) {
        list_del(&node->list);
        dev_kfree_skb(node->skb);
        kfree(node);
    }
    priv->rx_count = 0;
    spin_unlock_irqrestore(&priv->rx_lock, flags);

    pr_info("camex: userspace detached\n");
    return 0;
}

/**
 * camex_chr_read - deliver the next queued IPv4 packet to userspace.
 *
 * Behaviour mirrors /dev/net/tun in IFF_TUN mode:
 *  - each read() returns exactly one packet;
 *  - if buf is too small, the packet is dropped and EMSGSIZE returned;
 *  - blocks (or returns EAGAIN if O_NONBLOCK) when the queue is empty.
 */
static ssize_t camex_chr_read(struct file *filp, char __user *buf,
                               size_t count, loff_t *pos)
{
    struct camex_priv *priv = filp->private_data;
    struct pkt_node   *node;
    struct sk_buff    *skb;
    unsigned long      flags;
    ssize_t            ret;

    if (filp->f_flags & O_NONBLOCK) {
        spin_lock_irqsave(&priv->rx_lock, flags);
        if (list_empty(&priv->rx_queue)) {
            spin_unlock_irqrestore(&priv->rx_lock, flags);
            return -EAGAIN;
        }
        spin_unlock_irqrestore(&priv->rx_lock, flags);
    } else {
        ret = wait_event_interruptible(priv->rx_wait,
                    !list_empty(&priv->rx_queue));
        if (ret)
            return ret;  /* -ERESTARTSYS */
    }

    spin_lock_irqsave(&priv->rx_lock, flags);
    if (list_empty(&priv->rx_queue)) {
        /* spurious wake-up */
        spin_unlock_irqrestore(&priv->rx_lock, flags);
        return -EAGAIN;
    }
    node = list_first_entry(&priv->rx_queue, struct pkt_node, list);
    list_del(&node->list);
    priv->rx_count--;
    spin_unlock_irqrestore(&priv->rx_lock, flags);

    skb = node->skb;
    kfree(node);

    if (count < (size_t)skb->len) {
        dev_kfree_skb(skb);
        return -EMSGSIZE;
    }

    ret = skb->len;
    if (copy_to_user(buf, skb->data, skb->len))
        ret = -EFAULT;

    dev_kfree_skb(skb);
    return ret;
}

/**
 * camex_chr_write - inject a raw IPv4 packet into the kernel network stack.
 *
 * The caller writes a complete IPv4 datagram (no extra header).
 * Non-IPv4 datagrams (first nibble != 4) are rejected with EINVAL.
 */
static ssize_t camex_chr_write(struct file *filp, const char __user *buf,
                                size_t count, loff_t *pos)
{
    struct camex_priv *priv = filp->private_data;
    struct sk_buff    *skb;
    unsigned char     *data;
    unsigned char      first_byte;

    if (count < 20 || count > 65535)
        return -EINVAL;

    /* Peek at the version nibble without consuming the data. */
    if (get_user(first_byte, buf))
        return -EFAULT;
    if ((first_byte >> 4) != 4)
        return -EINVAL;  /* IPv4 only */

    skb = netdev_alloc_skb(priv->dev, count + NET_IP_ALIGN);
    if (!skb)
        return -ENOMEM;

    skb_reserve(skb, NET_IP_ALIGN);
    data = CAMEX_SKB_PUT(skb, count);

    if (copy_from_user(data, buf, count)) {
        kfree_skb(skb);
        return -EFAULT;
    }

    skb->protocol = htons(ETH_P_IP);
    skb->dev      = priv->dev;
    skb_reset_network_header(skb);

    priv->dev->stats.rx_packets++;
    priv->dev->stats.rx_bytes += count;

    netif_rx(skb);
    return count;
}

/**
 * camex_chr_poll - support select/poll/epoll.
 *
 * The fd is always writable.  It becomes readable as soon as at least
 * one packet is waiting in the queue.
 */
static CAMEX_POLL_T camex_chr_poll(struct file *filp,
                                    struct poll_table_struct *wait)
{
    struct camex_priv *priv = filp->private_data;
    CAMEX_POLL_T       mask = CAMEX_EPOLLOUT | CAMEX_EPOLLWRNORM;

    poll_wait(filp, &priv->rx_wait, wait);

    if (!list_empty(&priv->rx_queue))
        mask |= CAMEX_EPOLLIN | CAMEX_EPOLLRDNORM;

    return mask;
}

static const struct file_operations camex_fops = {
    .owner   = THIS_MODULE,
    .open    = camex_chr_open,
    .release = camex_chr_release,
    .read    = camex_chr_read,
    .write   = camex_chr_write,
    .poll    = camex_chr_poll,
    .llseek  = no_llseek,
};

static struct miscdevice camex_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DRV_NAME,   /* → /dev/camex */
    .fops  = &camex_fops,
    .mode  = 0666,
};

/* ================================================================
 * Module init / exit
 * ================================================================ */

static int __init camex_init(void)
{
    struct net_device  *dev;
    struct camex_priv  *priv;
    int ret;

    dev = CAMEX_ALLOC_NETDEV(sizeof(struct camex_priv),
                              DEV_NAME, camex_net_setup);
    if (!dev)
        return -ENOMEM;

    priv = netdev_priv(dev);
    priv->dev = dev;
    INIT_LIST_HEAD(&priv->rx_queue);
    priv->rx_count = 0;
    spin_lock_init(&priv->rx_lock);
    init_waitqueue_head(&priv->rx_wait);
    priv->attached = false;
    g_priv = priv;

    ret = register_netdev(dev);
    if (ret) {
        pr_err("camex: register_netdev failed: %d\n", ret);
        free_netdev(dev);
        g_priv = NULL;
        return ret;
    }

    ret = misc_register(&camex_miscdev);
    if (ret) {
        pr_err("camex: misc_register failed: %d\n", ret);
        unregister_netdev(dev);
        free_netdev(dev);
        g_priv = NULL;
        return ret;
    }

    pr_info("camex: loaded — /dev/%s <-> net:%s  (kernel %d.%d)\n",
            camex_miscdev.name, DEV_NAME,
            (LINUX_VERSION_CODE >> 16) & 0xff,
            (LINUX_VERSION_CODE >>  8) & 0xff);
    return 0;
}

static void __exit camex_exit(void)
{
    struct net_device *dev;

    misc_deregister(&camex_miscdev);

    if (g_priv) {
        dev    = g_priv->dev;
        g_priv = NULL;
        unregister_netdev(dev);
        free_netdev(dev);
    }
    pr_info("camex: unloaded\n");
}

module_init(camex_init);
module_exit(camex_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("camex");
MODULE_DESCRIPTION("camex — standalone IPv4 TUN driver, no tun.ko dependency");
MODULE_VERSION(CAMEX_VERSION);
