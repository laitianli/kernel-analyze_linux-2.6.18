/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for inet_sock
 *
 * Authors:	Many, reorganised here by
 * 		Arnaldo Carvalho de Melo <acme@mandriva.com>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _INET_SOCK_H
#define _INET_SOCK_H


#include <linux/string.h>
#include <linux/types.h>

#include <net/flow.h>
#include <net/sock.h>
#include <net/request_sock.h>

/** struct ip_options - IP Options
 *
 * @faddr - Saved first hop address
 * @is_setbyuser - Set by setsockopt?
 * @is_data - Options in __data, rather than skb
 * @is_strictroute - Strict source route
 * @srr_is_hit - Packet destination addr was our one
 * @is_changed - IP checksum more not valid
 * @rr_needaddr - Need to record addr of outgoing dev
 * @ts_needtime - Need to record timestamp
 * @ts_needaddr - Need to record addr of outgoing dev
 */
struct ip_options {
	__u32		faddr;
	unsigned char	optlen;
	unsigned char	srr;
	unsigned char	rr;
	unsigned char	ts;
	unsigned char	is_setbyuser:1,
			is_data:1,
			is_strictroute:1,
			srr_is_hit:1,
			is_changed:1,
			rr_needaddr:1,
			ts_needtime:1,
			ts_needaddr:1;
	unsigned char	router_alert;
	unsigned char	__pad1;
	unsigned char	__pad2;
	unsigned char	__data[0];
};

#define optlength(opt) (sizeof(struct ip_options) + opt->optlen)

struct inet_request_sock {
	struct request_sock	req;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	u16			inet6_rsk_offset;
	/* 2 bytes hole, try to pack */
#endif
	u32			loc_addr;
	u32			rmt_addr;
	u16			rmt_port;
	u16			snd_wscale : 4, 
				rcv_wscale : 4, 
				tstamp_ok  : 1,
				sack_ok	   : 1,
				wscale_ok  : 1,
				ecn_ok	   : 1,
				acked	   : 1;
	struct ip_options	*opt;
};

static inline struct inet_request_sock *inet_rsk(const struct request_sock *sk)
{
	return (struct inet_request_sock *)sk;
}

struct ip_mc_socklist;
struct ipv6_pinfo;
struct rtable;

/** struct inet_sock - representation of INET sockets
 *
 * @sk - ancestor class
 * @pinet6 - pointer to IPv6 control block
 * @daddr - Foreign IPv4 addr
 * @rcv_saddr - Bound local IPv4 addr
 * @dport - Destination port
 * @num - Local port
 * @saddr - Sending source
 * @uc_ttl - Unicast TTL
 * @sport - Source port
 * @id - ID counter for DF pkts
 * @tos - TOS
 * @mc_ttl - Multicasting TTL
 * @is_icsk - is this an inet_connection_sock?
 * @mc_index - Multicast device index
 * @mc_list - Group array
 * @cork - info to build ip hdr on each ip frag while socket is corked
 */
/* ��IPv4Э��ר�õĴ�����ƿ飬�Ƕ�sock�ṹ����չ���ڴ�����ƿ�Ļ��������Ѿ߱��Ļ����ϣ���һ���ṩ��IPv4Э��ר�е�һЩ���ԡ���:TTL,�鲥�б�
 * IP��ַ���˿ڡ�
 */
struct inet_sock {
	/* sk and pinet6 has to be the first two members of inet_sock */
	/* ͨ�õ������������ */
	struct sock		sk;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct ipv6_pinfo	*pinet6;
#endif
	/* Socket demultiplex comparisons on incoming packets. */
	__u32			daddr; /* Ŀ��IP��ַ */  
	__u32			rcv_saddr; /* �Ѿ��󶨵ı���IP��ַ����������ʱ����Ϊ����һ���ֲ������������Ĵ�����ƿ顣 */
	__u16			dport; /* Ŀ�Ķ˿� */
	__u16			num; /* �����ֽ���洢�ı��ض˿� */
	__u32			saddr; /* ��rcv_saddrһ��Ҳ��ʶ����IP��ַ�����ڷ���ʱʹ�á�������;��һ���� */
	__s16			uc_ttl; /* �������ĵ�TTL */
	/* ���һЩIPPROTO_IP�����ѡ��ֵ */
	__u16			cmsg_flags;
	struct ip_options	*opt;/* IP���ݱ�ѡ��� */
	__u16			sport; /* ��numת���ɵ������ֽ����Դ�˿� */
	__u16			id; /* һ������������ֵ����������IP�ײ��е�id�� */
	__u8			tos;/* ��������IP���ݱ��ײ���TOS�� */
	__u8			mc_ttl; /* �������öಥ���ݱ���TTL */
	/* ��־�׽ӿ��Ƿ����õ���MTU���ֹ��ܣ�ֵ: IP_PMTUDISC_DO����IP_MTU_DISCOVER�׽ӿ�ѡ���й� 
     * �����IP���ݱ�ʱ������ip_dont_fragment()�����������IP���ݱ��ܷ��Ƭ��������ܷ�Ƭ�������IP���ݱ��ײ���Ӳ������Ƭ�ı�־��
	 */
	__u8			pmtudisc;
	/* ��ʶ�Ƿ����������չ�Ŀɿ�������Ϣ����IP_RECVERR�׽ӿ�ѡ����� */
	__u8			recverr:1,
	/* �Ƿ�Ϊ�������ӵĴ�����ƿ顣���Ƿ�Ϊ����inet_connection_sock�ṹ�Ĵ�����ƿ飬��TCP������ƿ� */
				is_icsk:1,
	/* ��ʶ�Ƿ�����󶨷�������ַ����IP_FREEBIND ѡ����� */				
				freebind:1,
	/* ��ʶIP�ײ��Ƿ����û����ݹ������ñ�־ֻ����RAW�׽ӿڡ�һ�����ú�IPѡ���е�IP_TTL��IP_TOS�������ԡ� */				
				hdrincl:1,
	/* ��ʶ�鲥�Ƿ񷢼���· */				
				mc_loop:1;
	/* �����鲥���ĵ������豸�����š����Ϊ0�����ʾ���Դ��κ������豸���͡� */
	int			mc_index;
	/* �����鲥���ĵ�Դ��ַ�� */
	__u32			mc_addr;
	/* �����׽ӿڼ�����鲥��ַ�б� */
	struct ip_mc_socklist	*mc_list;
	/* UDP��ԭʼIP��ÿ�η���ʱ�����һЩ��ʱ��Ϣ���磬UDP���ݱ���ԭʼIP���ݱ���Ƭ�Ĵ�С�� */
	struct {
		unsigned int		flags; 
		unsigned int		fragsize;/* ��Ƭ��С */
		struct ip_options	*opt; /* IPѡ�� */
		struct rtable		*rt; /* �������ݱ�ʹ�õ������������� */
		/* ��ǰ���͵����ݱ������ݳ��� */
		int			length; /* Total length of all frames */
		/* ���IP���ݱ���Ŀ�ĵ�ַ */
		u32			addr;
		 /* ��flowi�ṹ������Ŀ�ĵ�ַ��Ŀ�Ķ˿ڡ�Դ��ַ��Դ�˿ڣ�����UDP����ʱ�й���Ϣ��������ȡ�� */
		struct flowi		fl;
	} cork;
};

#define IPCORK_OPT	1	/* ip-options has been held in ipcork.opt */
#define IPCORK_ALLFRAG	2	/* always fragment (for ipv6 for now) */

static inline struct inet_sock *inet_sk(const struct sock *sk)
{
	return (struct inet_sock *)sk;
}

static inline void __inet_sk_copy_descendant(struct sock *sk_to,
					     const struct sock *sk_from,
					     const int ancestor_size)
{
	memcpy(inet_sk(sk_to) + 1, inet_sk(sk_from) + 1,
	       sk_from->sk_prot->obj_size - ancestor_size);
}
#if !(defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE))
static inline void inet_sk_copy_descendant(struct sock *sk_to,
					   const struct sock *sk_from)
{
	__inet_sk_copy_descendant(sk_to, sk_from, sizeof(struct inet_sock));
}
#endif

extern int inet_sk_rebuild_header(struct sock *sk);

static inline unsigned int inet_ehashfn(const __u32 laddr, const __u16 lport,
					const __u32 faddr, const __u16 fport)
{
	unsigned int h = (laddr ^ lport) ^ (faddr ^ fport);
	h ^= h >> 16;
	h ^= h >> 8;
	return h;
}

static inline int inet_sk_ehashfn(const struct sock *sk)
{
	const struct inet_sock *inet = inet_sk(sk);
	const __u32 laddr = inet->rcv_saddr;
	const __u16 lport = inet->num;
	const __u32 faddr = inet->daddr;
	const __u16 fport = inet->dport;

	return inet_ehashfn(laddr, lport, faddr, fport);
}

#endif	/* _INET_SOCK_H */
