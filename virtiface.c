// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/inet.h>

#define VIRTIFACE_NAME "virtiface0"
#define VIRTIFACE_PROC_DIR "virtiface"
#define VIRTIFACE_PROC_IP "ipv4"
#define VIRTIFACE_RX_HEADROOM 32

struct virtiface_priv {
	spinlock_t lock;
	struct rtnl_link_stats64 stats;
	__be32 configured_ipv4;
	bool ipv4_is_set;
};

static struct net_device *virtiface_dev;
static struct proc_dir_entry *virtiface_proc_dir;
static struct proc_dir_entry *virtiface_proc_ip;

static void virtiface_record_tx(struct virtiface_priv *priv, unsigned int len)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->stats.tx_packets++;
	priv->stats.tx_bytes += len;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static void virtiface_record_tx_drop(struct virtiface_priv *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->stats.tx_dropped++;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static void virtiface_record_rx(struct virtiface_priv *priv, unsigned int len)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->stats.rx_packets++;
	priv->stats.rx_bytes += len;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static bool virtiface_target_is_local(struct virtiface_priv *priv, __be32 daddr)
{
	bool matched;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	matched = priv->ipv4_is_set && priv->configured_ipv4 == daddr;
	spin_unlock_irqrestore(&priv->lock, flags);

	return matched;
}

static int virtiface_build_icmp_reply(struct net_device *dev, struct sk_buff *request)
{
	struct virtiface_priv *priv = netdev_priv(dev);
	struct sk_buff *reply;
	struct iphdr *iph;
	struct icmphdr *icmph;
	unsigned int ip_len;
	unsigned int icmp_len;
	__be32 src;

	if (!pskb_may_pull(request, sizeof(struct iphdr)))
		return -EINVAL;

	iph = ip_hdr(request);
	if (!iph || iph->version != 4 || iph->ihl < 5)
		return -EINVAL;

	ip_len = iph->ihl * 4;
	if (!pskb_may_pull(request, ip_len + sizeof(struct icmphdr)))
		return -EINVAL;

	iph = ip_hdr(request);
	if (iph->protocol != IPPROTO_ICMP)
		return -EPROTO;

	if (ntohs(iph->tot_len) < ip_len + sizeof(struct icmphdr) ||
	    request->len < ntohs(iph->tot_len))
		return -EINVAL;

	icmph = (struct icmphdr *)((u8 *)iph + ip_len);
	if (icmph->type != ICMP_ECHO)
		return -EOPNOTSUPP;

	if (!virtiface_target_is_local(priv, iph->daddr))
		return -ENOMSG;

	reply = alloc_skb(request->len + VIRTIFACE_RX_HEADROOM, GFP_ATOMIC);
	if (!reply)
		return -ENOMEM;

	skb_reserve(reply, VIRTIFACE_RX_HEADROOM);
	skb_put_data(reply, request->data, request->len);
	reply->dev = dev;
	reply->protocol = htons(ETH_P_IP);
	reply->pkt_type = PACKET_HOST;
	reply->ip_summed = CHECKSUM_NONE;
	skb_reset_network_header(reply);

	iph = ip_hdr(reply);
	ip_len = iph->ihl * 4;
	icmph = (struct icmphdr *)((u8 *)iph + ip_len);
	icmp_len = ntohs(iph->tot_len) - ip_len;

	src = iph->saddr;
	iph->saddr = iph->daddr;
	iph->daddr = src;
	iph->ttl = 64;
	iph->check = 0;
	iph->check = ip_fast_csum((u8 *)iph, iph->ihl);

	icmph->type = ICMP_ECHOREPLY;
	icmph->checksum = 0;
	icmph->checksum = ip_compute_csum((void *)icmph, icmp_len);

	virtiface_record_rx(priv, reply->len);
	netif_rx(reply);
	return 0;
}

static netdev_tx_t virtiface_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct virtiface_priv *priv = netdev_priv(dev);
	int ret;

	virtiface_record_tx(priv, skb->len);

	if (skb->protocol != htons(ETH_P_IP))
		goto drop;

	ret = virtiface_build_icmp_reply(dev, skb);
	if (!ret) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

drop:
	virtiface_record_tx_drop(priv);
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int virtiface_open(struct net_device *dev)
{
	netif_start_queue(dev);
	netif_carrier_on(dev);
	return 0;
}

static int virtiface_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	netif_carrier_off(dev);
	return 0;
}

static void virtiface_get_stats64(struct net_device *dev,
				  struct rtnl_link_stats64 *stats)
{
	struct virtiface_priv *priv = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*stats = priv->stats;
	spin_unlock_irqrestore(&priv->lock, flags);
}

static const struct net_device_ops virtiface_netdev_ops = {
	.ndo_open = virtiface_open,
	.ndo_stop = virtiface_stop,
	.ndo_start_xmit = virtiface_start_xmit,
	.ndo_get_stats64 = virtiface_get_stats64,
};

static void virtiface_setup(struct net_device *dev)
{
	struct virtiface_priv *priv = netdev_priv(dev);

	dev->netdev_ops = &virtiface_netdev_ops;
	dev->type = ARPHRD_NONE;
	dev->flags = IFF_NOARP | IFF_POINTOPOINT;
	dev->needs_free_netdev = true;
	dev->mtu = ETH_DATA_LEN;
	dev->min_mtu = 68;
	dev->max_mtu = ETH_DATA_LEN;
	dev->tx_queue_len = 0;
	dev->hard_header_len = 0;
	dev->addr_len = 0;

	spin_lock_init(&priv->lock);
}

static ssize_t virtiface_proc_read(struct file *file, char __user *buffer,
				   size_t count, loff_t *ppos)
{
	struct virtiface_priv *priv = netdev_priv(virtiface_dev);
	char out[64];
	unsigned long flags;
	__be32 addr;
	bool is_set;
	int len;

	spin_lock_irqsave(&priv->lock, flags);
	addr = priv->configured_ipv4;
	is_set = priv->ipv4_is_set;
	spin_unlock_irqrestore(&priv->lock, flags);

	if (!is_set)
		len = scnprintf(out, sizeof(out), "unset\n");
	else
		len = scnprintf(out, sizeof(out), "%pI4\n", &addr);

	return simple_read_from_buffer(buffer, count, ppos, out, len);
}

static ssize_t virtiface_proc_write(struct file *file, const char __user *buffer,
				    size_t count, loff_t *ppos)
{
	struct virtiface_priv *priv = netdev_priv(virtiface_dev);
	char in[32];
	size_t len;
	__be32 addr;
	unsigned long flags;

	if (!count)
		return -EINVAL;

	len = min(count, sizeof(in) - 1);
	if (copy_from_user(in, buffer, len))
		return -EFAULT;
	in[len] = '\0';
	strim(in);

	if (!strcmp(in, "unset") || !strcmp(in, "none") || !strcmp(in, "0")) {
		spin_lock_irqsave(&priv->lock, flags);
		priv->configured_ipv4 = 0;
		priv->ipv4_is_set = false;
		spin_unlock_irqrestore(&priv->lock, flags);
		return count;
	}

	if (!in4_pton(in, -1, (u8 *)&addr, '\0', NULL))
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	priv->configured_ipv4 = addr;
	priv->ipv4_is_set = true;
	spin_unlock_irqrestore(&priv->lock, flags);

	return count;
}

static const struct proc_ops virtiface_proc_ops = {
	.proc_read = virtiface_proc_read,
	.proc_write = virtiface_proc_write,
};

static int __init virtiface_init(void)
{
	struct net_device *dev;
	int ret;

	dev = alloc_netdev(sizeof(struct virtiface_priv), VIRTIFACE_NAME,
			   NET_NAME_UNKNOWN, virtiface_setup);
	if (!dev)
		return -ENOMEM;

	virtiface_dev = dev;
	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		virtiface_dev = NULL;
		return ret;
	}

	virtiface_proc_dir = proc_mkdir(VIRTIFACE_PROC_DIR, NULL);
	if (!virtiface_proc_dir) {
		ret = -ENOMEM;
		goto err_unregister_netdev;
	}

	virtiface_proc_ip = proc_create(VIRTIFACE_PROC_IP, 0666,
					virtiface_proc_dir, &virtiface_proc_ops);
	if (!virtiface_proc_ip) {
		ret = -ENOMEM;
		goto err_remove_proc_dir;
	}

	pr_info("virtiface: registered %s and /proc/%s/%s\n",
		dev->name, VIRTIFACE_PROC_DIR, VIRTIFACE_PROC_IP);
	return 0;

err_remove_proc_dir:
	remove_proc_entry(VIRTIFACE_PROC_DIR, NULL);
	virtiface_proc_dir = NULL;
err_unregister_netdev:
	unregister_netdev(dev);
	virtiface_dev = NULL;
	return ret;
}

static void __exit virtiface_exit(void)
{
	if (virtiface_proc_ip)
		remove_proc_entry(VIRTIFACE_PROC_IP, virtiface_proc_dir);
	if (virtiface_proc_dir)
		remove_proc_entry(VIRTIFACE_PROC_DIR, NULL);
	if (virtiface_dev)
		unregister_netdev(virtiface_dev);

	virtiface_proc_ip = NULL;
	virtiface_proc_dir = NULL;
	virtiface_dev = NULL;
	pr_info("virtiface: unloaded\n");
}

module_init(virtiface_init);
module_exit(virtiface_exit);

MODULE_AUTHOR("rgurin");
MODULE_DESCRIPTION("Virtual IPv4 Linux network interface");
MODULE_LICENSE("GPL");
