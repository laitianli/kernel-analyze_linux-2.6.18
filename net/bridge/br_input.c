/*
 *	Handle incoming frames
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 *
 *	$Id: br_input.c,v 1.10 2001/12/24 04:50:20 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/netfilter_bridge.h>
#include "br_private.h"

/* Bridge group multicast address 802.1d (pg 51). */
const u8 br_group_address[ETH_ALEN] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 };
/**ltl
 * 功能: 将报文传递给网络层
 * 参数:
 * 返回值:
 * 说明:
 */
static void br_pass_frame_up(struct net_bridge *br, struct sk_buff *skb)
{
	struct net_device *indev;
	/* 网桥的统计信息 */
	br->statistics.rx_packets++;
	br->statistics.rx_bytes += skb->len;

	indev = skb->dev;
	skb->dev = br->dev; /* 将skb中的dev设置成网桥 */

	NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_IN, skb, indev, NULL,
		netif_receive_skb);
}
/**ltl
 * 功能: 网桥报文经过NF_BR_PRE_ROUTING过滤后的处理函数
 * 参数:
 * 返回值:
 * 说明: 此函数要完成MAC的学习和转发功能。
 * 		学习功能原理: 当接收一个报文后，利用报文的源MAC更新TAC表(MAC-端口映射表)，若TAC表中已经存在记录，则更新此表项的端口对象；
 *					若TAC表中没有记录，则创建一条新表项。这里的MAC是发送报文的主机MAC，端口是接收此报文的物理接口。
 *		转发功能原理: 根据报文的目的MAC去查找TAC表，如果找到表项，而此表项的端口是本机的某一接口口，则将此报文传给网络层；
 *					若此报文不是本机的接口， 则将此报文通过对应的端口转发出去。
 *					如果没有找到表项，则报文要通过其它的所有端口转发出去。
 */
/* note: already called with rcu_read_lock (preempt_disabled) */
int br_handle_frame_finish(struct sk_buff *skb)
{
	/* 目的MAC */
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	/* 获取物理接口的端口 */
	struct net_bridge_port *p = rcu_dereference(skb->dev->br_port);
	struct net_bridge *br;
	struct net_bridge_fdb_entry *dst;
	int passedup = 0;

	if (!p || p->state == BR_STATE_DISABLED)
		goto drop;

	/* insert into forwarding database after filtering to avoid spoofing */
	br = p->br;
	/* 1.利用源MAC更新TAC表 */
	br_fdb_update(br, p, eth_hdr(skb)->h_source);

	if (p->state == BR_STATE_LEARNING)
		goto drop;

	if (br->dev->flags & IFF_PROMISC) {
		struct sk_buff *skb2;

		skb2 = skb_clone(skb, GFP_ATOMIC);
		if (skb2 != NULL) {
			passedup = 1;
			br_pass_frame_up(br, skb2);
		}
	}
	/* 目的地址是一个广播地址，则广播此报文 */
	if (is_multicast_ether_addr(dest)) {
		br->statistics.multicast++;
		br_flood_forward(br, skb, !passedup);
		if (!passedup)
			br_pass_frame_up(br, skb);
		goto out;
	}
	/* 2.根据目的MAC获取对应的端口对象 */
	dst = __br_fdb_get(br, dest);
	/* 3.此MAC的端口是本地的，则将此报文上传给网络层 */
	if (dst != NULL && dst->is_local) { 
		if (!passedup)
			br_pass_frame_up(br, skb); /* 传给网络层处理 */
		else
			kfree_skb(skb);
		goto out;
	}
	/* 4.要将报文通过对应的端口转发出去 */
	if (dst != NULL) {
		br_forward(dst->dst, skb);
		goto out;
	}
	/* 5.如果此报文不是要传给本机，并且在TAC表中没有相应的表项，则将此报文转发给其它所有的端口 */
	br_flood_forward(br, skb, 0);

out:
	return 0;
drop:
	kfree_skb(skb);
	goto out;
}

/* note: already called with rcu_read_lock (preempt_disabled) */
static int br_handle_local_finish(struct sk_buff *skb)
{
	struct net_bridge_port *p = rcu_dereference(skb->dev->br_port);

	if (p && p->state != BR_STATE_DISABLED)
		br_fdb_update(p->br, p, eth_hdr(skb)->h_source);

	return 0;	 /* process further */
}

/* Does address match the link local multicast address.
 * 01:80:c2:00:00:0X
 */
static inline int is_link_local(const unsigned char *dest)
{
	return memcmp(dest, br_group_address, 5) == 0 && (dest[5] & 0xf0) == 0;
}

/*
 * Called via br_handle_frame_hook.
 * Return 0 if *pskb should be processed furthur
 *	  1 if *pskb is handled
 * note: already called with rcu_read_lock (preempt_disabled) 
 */
/**ltl
 * 功能: 用于网桥从网卡中接收数据
 * 参数:
 * 返回值:
 * 说明:
 */
int br_handle_frame(struct net_bridge_port *p, struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	/* 判定源地址是否有效 */
	if (!is_valid_ether_addr(eth_hdr(skb)->h_source))
		goto err;

	if (unlikely(is_link_local(dest))) {
		skb->pkt_type = PACKET_HOST;
		return NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_IN, skb, skb->dev,
			       NULL, br_handle_local_finish) != 0;
	}
	/* 端口状态 */
	if (p->state == BR_STATE_FORWARDING || p->state == BR_STATE_LEARNING) {
		if (br_should_route_hook) { /* 决定网桥数据是否要通过L3层转发 */
			if (br_should_route_hook(pskb)) 
				return 0;
			skb = *pskb;
			dest = eth_hdr(skb)->h_dest;
		}
		/* 如果报文的目的MAC就是网桥的MAC，则设置数据包的类型为PACKET_HOST */
		if (!compare_ether_addr(p->br->dev->dev_addr, dest))
			skb->pkt_type = PACKET_HOST;
		/* 网桥的NF_BR_PRE_ROUTING的netfilter点 */
		NF_HOOK(PF_BRIDGE, NF_BR_PRE_ROUTING, skb, skb->dev, NULL,
			br_handle_frame_finish);
		return 1;
	}

err:
	kfree_skb(skb);
	return 1;
}
