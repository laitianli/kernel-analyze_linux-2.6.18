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
/* 是IPv4协议专用的传输控制块，是对sock结构的扩展，在传输控制块的基本属性已具备的基础上，进一步提供了IPv4协议专有的一些属性。如:TTL,组播列表
 * IP地址、端口。
 */
struct inet_sock {
	/* sk and pinet6 has to be the first two members of inet_sock */
	/* 通用的网络层描述块 */
	struct sock		sk;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct ipv6_pinfo	*pinet6;
#endif
	/* Socket demultiplex comparisons on incoming packets. */
	__u32			daddr; /* 目的IP地址 */  
	__u32			rcv_saddr; /* 已经绑定的本地IP地址。接收数据时，作为条件一部分查找数据所属的传输控制块。 */
	__u16			dport; /* 目的端口 */
	__u16			num; /* 主机字节序存储的本地端口 */
	__u32			saddr; /* 跟rcv_saddr一样也标识本地IP地址，但在发送时使用。两者用途不一样。 */
	__s16			uc_ttl; /* 单播报文的TTL */
	/* 存放一些IPPROTO_IP级别的选项值 */
	__u16			cmsg_flags;
	struct ip_options	*opt;/* IP数据报选项本身 */
	__u16			sport; /* 由num转换成的网络字节序的源端口 */
	__u16			id; /* 一个单调递增的值，用来赋给IP首部中的id域 */
	__u8			tos;/* 用于设置IP数据报首部的TOS域 */
	__u8			mc_ttl; /* 用于设置多播数据报的TTL */
	/* 标志套接口是否启用跌幅MTU发现功能，值: IP_PMTUDISC_DO，与IP_MTU_DISCOVER套接口选项有关 
     * 在输出IP数据报时，会用ip_dont_fragment()来检测待输出的IP数据报能否分片。如果不能分片，则会在IP数据报首部添加不允许分片的标志。
	 */
	__u8			pmtudisc;
	/* 标识是否允许接收扩展的可靠错误信息，与IP_RECVERR套接口选项相关 */
	__u8			recverr:1,
	/* 是否为基于连接的传输控制块。即是否为基于inet_connection_sock结构的传输控制块，如TCP传输控制块 */
				is_icsk:1,
	/* 标识是否允许绑定非主机地址。与IP_FREEBIND 选项相关 */				
				freebind:1,
	/* 标识IP首部是否由用户数据构建。该标志只用于RAW套接口。一旦设置后，IP选项中的IP_TTL和IP_TOS都被忽略。 */				
				hdrincl:1,
	/* 标识组播是否发几回路 */				
				mc_loop:1;
	/* 发送组播报文的网络设备索引号。如果为0，则表示可以从任何网络设备发送。 */
	int			mc_index;
	/* 发送组播报文的源地址。 */
	__u32			mc_addr;
	/* 所在套接口加入的组播地址列表。 */
	struct ip_mc_socklist	*mc_list;
	/* UDP或原始IP在每次发送时缓存的一些临时信息。如，UDP数据报或原始IP数据报分片的大小。 */
	struct {
		unsigned int		flags; 
		unsigned int		fragsize;/* 分片大小 */
		struct ip_options	*opt; /* IP选项 */
		struct rtable		*rt; /* 发送数据报使用的输出跌幅缓存项。 */
		/* 当前发送的数据报的数据长度 */
		int			length; /* Total length of all frames */
		/* 输出IP数据报的目的地址 */
		u32			addr;
		 /* 用flowi结构来缓存目的地址、目的端口、源地址和源端口，构造UDP报文时有关信息就在这里取。 */
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
