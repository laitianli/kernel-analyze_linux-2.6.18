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
 * ����: �����Ĵ��ݸ������
 * ����:
 * ����ֵ:
 * ˵��:
 */
static void br_pass_frame_up(struct net_bridge *br, struct sk_buff *skb)
{
	struct net_device *indev;
	/* ���ŵ�ͳ����Ϣ */
	br->statistics.rx_packets++;
	br->statistics.rx_bytes += skb->len;

	indev = skb->dev;
	skb->dev = br->dev; /* ��skb�е�dev���ó����� */

	NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_IN, skb, indev, NULL,
		netif_receive_skb);
}
/**ltl
 * ����: ���ű��ľ���NF_BR_PRE_ROUTING���˺�Ĵ�����
 * ����:
 * ����ֵ:
 * ˵��: �˺���Ҫ���MAC��ѧϰ��ת�����ܡ�
 * 		ѧϰ����ԭ��: ������һ�����ĺ����ñ��ĵ�ԴMAC����TAC��(MAC-�˿�ӳ���)����TAC�����Ѿ����ڼ�¼������´˱���Ķ˿ڶ���
 *					��TAC����û�м�¼���򴴽�һ���±�������MAC�Ƿ��ͱ��ĵ�����MAC���˿��ǽ��մ˱��ĵ�����ӿڡ�
 *		ת������ԭ��: ���ݱ��ĵ�Ŀ��MACȥ����TAC������ҵ�������˱���Ķ˿��Ǳ�����ĳһ�ӿڿڣ��򽫴˱��Ĵ�������㣻
 *					���˱��Ĳ��Ǳ����Ľӿڣ� �򽫴˱���ͨ����Ӧ�Ķ˿�ת����ȥ��
 *					���û���ҵ��������Ҫͨ�����������ж˿�ת����ȥ��
 */
/* note: already called with rcu_read_lock (preempt_disabled) */
int br_handle_frame_finish(struct sk_buff *skb)
{
	/* Ŀ��MAC */
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	/* ��ȡ����ӿڵĶ˿� */
	struct net_bridge_port *p = rcu_dereference(skb->dev->br_port);
	struct net_bridge *br;
	struct net_bridge_fdb_entry *dst;
	int passedup = 0;

	if (!p || p->state == BR_STATE_DISABLED)
		goto drop;

	/* insert into forwarding database after filtering to avoid spoofing */
	br = p->br;
	/* 1.����ԴMAC����TAC�� */
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
	/* Ŀ�ĵ�ַ��һ���㲥��ַ����㲥�˱��� */
	if (is_multicast_ether_addr(dest)) {
		br->statistics.multicast++;
		br_flood_forward(br, skb, !passedup);
		if (!passedup)
			br_pass_frame_up(br, skb);
		goto out;
	}
	/* 2.����Ŀ��MAC��ȡ��Ӧ�Ķ˿ڶ��� */
	dst = __br_fdb_get(br, dest);
	/* 3.��MAC�Ķ˿��Ǳ��صģ��򽫴˱����ϴ�������� */
	if (dst != NULL && dst->is_local) { 
		if (!passedup)
			br_pass_frame_up(br, skb); /* ��������㴦�� */
		else
			kfree_skb(skb);
		goto out;
	}
	/* 4.Ҫ������ͨ����Ӧ�Ķ˿�ת����ȥ */
	if (dst != NULL) {
		br_forward(dst->dst, skb);
		goto out;
	}
	/* 5.����˱��Ĳ���Ҫ����������������TAC����û����Ӧ�ı���򽫴˱���ת�����������еĶ˿� */
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
 * ����: �������Ŵ������н�������
 * ����:
 * ����ֵ:
 * ˵��:
 */
int br_handle_frame(struct net_bridge_port *p, struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	/* �ж�Դ��ַ�Ƿ���Ч */
	if (!is_valid_ether_addr(eth_hdr(skb)->h_source))
		goto err;

	if (unlikely(is_link_local(dest))) {
		skb->pkt_type = PACKET_HOST;
		return NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_IN, skb, skb->dev,
			       NULL, br_handle_local_finish) != 0;
	}
	/* �˿�״̬ */
	if (p->state == BR_STATE_FORWARDING || p->state == BR_STATE_LEARNING) {
		if (br_should_route_hook) { /* �������������Ƿ�Ҫͨ��L3��ת�� */
			if (br_should_route_hook(pskb)) 
				return 0;
			skb = *pskb;
			dest = eth_hdr(skb)->h_dest;
		}
		/* ������ĵ�Ŀ��MAC�������ŵ�MAC�����������ݰ�������ΪPACKET_HOST */
		if (!compare_ether_addr(p->br->dev->dev_addr, dest))
			skb->pkt_type = PACKET_HOST;
		/* ���ŵ�NF_BR_PRE_ROUTING��netfilter�� */
		NF_HOOK(PF_BRIDGE, NF_BR_PRE_ROUTING, skb, skb->dev, NULL,
			br_handle_frame_finish);
		return 1;
	}

err:
	kfree_skb(skb);
	return 1;
}
