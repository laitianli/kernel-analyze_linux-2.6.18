/*
 * NET		Generic infrastructure for Network protocols.
 *
 *		Definitions for request_sock 
 *
 * Authors:	Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *
 * 		From code originally in include/net/tcp.h
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _REQUEST_SOCK_H
#define _REQUEST_SOCK_H

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <net/sock.h>

struct request_sock;
struct sk_buff;
struct dst_entry;
struct proto;
/* 处理连接请求的函数指针表，包括用于发送SYN+ACK段、ACK段、RST段的函数 */
struct request_sock_ops {
	int		family; /* 所属协议族 */
	kmem_cache_t	*slab;
	int		obj_size;
	/* 发送syn+ack段的函数指针，值: tcp_v4_send_synack() */
	int		(*rtx_syn_ack)(struct sock *sk,
				       struct request_sock *req,
				       struct dst_entry *dst);
	/* 发送ACK段的函数指针，值: tcp_v4_reqsk_send_ack() */
	void		(*send_ack)(struct sk_buff *skb,
				    struct request_sock *req);
	/* 发送RST段的函数指针，值: tcp_v4_send_reset() */
	void		(*send_reset)(struct sk_buff *skb);
	/* 析构函数，tcp_v4_reqsk_destructor() */
	void		(*destructor)(struct request_sock *req);
};

/* struct request_sock - mini sock to represent a connection request
 */
struct request_sock {
	/* 连接件 */
	struct request_sock		*dl_next; /* Must be first member! */
	u16				mss; /* 客户端连接请求段中通告的MSS,若无通告，则初始值536 */
	u8				retrans; /* 重传SYN+ACK段的次数上限，若达到上限，连接取消 */
	u8				__pad; /* <未使用> */
	/* The following two fields can be easily recomputed I think -AK */
	/* 标识本端的最大通行窗口，在生成SYN+ACK段时计算该值 */
	u32				window_clamp; /* window clamp at creation time */
	/* 标识在连接建立时本端接收窗口大小，初始化为0，在生成SYN+ACK段时计算该值 */
	u32				rcv_wnd;	  /* rcv_wnd offered first time */
	/* 下一个将要发送的ACK中的时间戳值。 */
	u32				ts_recent;
	/* 发送了SYN+ACK段后，等待客户回应的超时时间。如果超时就会重发SYN+ACK包，直到重传次数达到上限 */
	unsigned long			expires;
	/* 与连接请求的函数指针表 */
	struct request_sock_ops		*rsk_ops;
	/* 传输控制块 */
	struct sock			*sk;
};

static inline struct request_sock *reqsk_alloc(struct request_sock_ops *ops)
{
	struct request_sock *req = kmem_cache_alloc(ops->slab, SLAB_ATOMIC);

	if (req != NULL)
		req->rsk_ops = ops;

	return req;
}

static inline void __reqsk_free(struct request_sock *req)
{
	kmem_cache_free(req->rsk_ops->slab, req);
}

static inline void reqsk_free(struct request_sock *req)
{
	req->rsk_ops->destructor(req);
	__reqsk_free(req);
}

extern int sysctl_max_syn_backlog;

/** struct listen_sock - listen state
 *
 * @max_qlen_log - log_2 of maximal queued SYNs/REQUESTs
 */
/* 存储连接请求块，在listen系统调用之后创建 */
struct listen_sock {
	u8			max_qlen_log; /* nr_table_entries的以2为底的对数值 */
	/* 3 bytes hole, try to use */
	int			qlen;/* 当前连接请求块数目 */
	int			qlen_young;/* 当前未重传过SYN+ACK段的请求块数目。 */
	int			clock_hand;
	u32			hash_rnd;
	/* 实际分配用来保存SYN请求连接的request_sock结构数组的长度 */
	u32			nr_table_entries;
	struct request_sock	*syn_table[0]; /* 在listen系统调用中生成 */
};

/** struct request_sock_queue - queue of request_socks
 *
 * @rskq_accept_head - FIFO head of established children
 * @rskq_accept_tail - FIFO tail of established children
 * @rskq_defer_accept - User waits for some data after accept()
 * @syn_wait_lock - serializer
 *
 * %syn_wait_lock is necessary only to avoid proc interface having to grab the main
 * lock sock while browsing the listening hash (otherwise it's deadlock prone).
 *
 * This lock is acquired in read mode only from listening_get_next() seq_file
 * op and it's acquired in write mode _only_ from code that is actively
 * changing rskq_accept_head. All readers that are holding the master sock lock
 * don't need to grab this lock in read mode too as rskq_accept_head. writes
 * are always protected from the main sock lock.
 */
/* 用于存放连接请求块的容器(处于SYN_RECV状态以及已连接但未被accept的传输控制块) */
struct request_sock_queue {
	/* 完成连接后的连接请求块，当执行accept()从listen_opt中移到此链表 */
	struct request_sock	*rskq_accept_head;
	struct request_sock	*rskq_accept_tail;
	rwlock_t		syn_wait_lock;
	u8			rskq_defer_accept;
	/* 3 bytes hole, try to pack */
	struct listen_sock	*listen_opt; /* 该实例在侦听时建立 */
};

extern int reqsk_queue_alloc(struct request_sock_queue *queue,
			     const int nr_table_entries);

static inline struct listen_sock *reqsk_queue_yank_listen_sk(struct request_sock_queue *queue)
{
	struct listen_sock *lopt;

	write_lock_bh(&queue->syn_wait_lock);
	lopt = queue->listen_opt;
	queue->listen_opt = NULL;
	write_unlock_bh(&queue->syn_wait_lock);

	return lopt;
}

static inline void __reqsk_queue_destroy(struct request_sock_queue *queue)
{
	kfree(reqsk_queue_yank_listen_sk(queue));
}

extern void reqsk_queue_destroy(struct request_sock_queue *queue);

static inline struct request_sock *
	reqsk_queue_yank_acceptq(struct request_sock_queue *queue)
{
	struct request_sock *req = queue->rskq_accept_head;

	queue->rskq_accept_head = NULL;
	return req;
}

static inline int reqsk_queue_empty(struct request_sock_queue *queue)
{
	return queue->rskq_accept_head == NULL;
}

static inline void reqsk_queue_unlink(struct request_sock_queue *queue,
				      struct request_sock *req,
				      struct request_sock **prev_req)
{
	write_lock(&queue->syn_wait_lock);
	*prev_req = req->dl_next;
	write_unlock(&queue->syn_wait_lock);
}

static inline void reqsk_queue_add(struct request_sock_queue *queue,
				   struct request_sock *req,
				   struct sock *parent,
				   struct sock *child)
{
	req->sk = child;
	sk_acceptq_added(parent);

	if (queue->rskq_accept_head == NULL)
		queue->rskq_accept_head = req;
	else
		queue->rskq_accept_tail->dl_next = req;

	queue->rskq_accept_tail = req;
	req->dl_next = NULL;
}

static inline struct request_sock *reqsk_queue_remove(struct request_sock_queue *queue)
{
	struct request_sock *req = queue->rskq_accept_head;

	BUG_TRAP(req != NULL);

	queue->rskq_accept_head = req->dl_next;
	if (queue->rskq_accept_head == NULL)
		queue->rskq_accept_tail = NULL;

	return req;
}
/**ltl
 * 功能: 获取已经连接的传输控制块
 * 参数:
 * 返回值:
 * 说明:
 */
static inline struct sock *reqsk_queue_get_child(struct request_sock_queue *queue,
						 struct sock *parent)
{
	struct request_sock *req = reqsk_queue_remove(queue);
	struct sock *child = req->sk;

	BUG_TRAP(child != NULL);

	sk_acceptq_removed(parent);
	__reqsk_free(req);
	return child;
}

static inline int reqsk_queue_removed(struct request_sock_queue *queue,
				      struct request_sock *req)
{
	struct listen_sock *lopt = queue->listen_opt;

	if (req->retrans == 0)
		--lopt->qlen_young;

	return --lopt->qlen;
}

static inline int reqsk_queue_added(struct request_sock_queue *queue)
{
	struct listen_sock *lopt = queue->listen_opt;
	const int prev_qlen = lopt->qlen;

	lopt->qlen_young++;
	lopt->qlen++;
	return prev_qlen;
}

static inline int reqsk_queue_len(const struct request_sock_queue *queue)
{
	return queue->listen_opt != NULL ? queue->listen_opt->qlen : 0;
}

static inline int reqsk_queue_len_young(const struct request_sock_queue *queue)
{
	return queue->listen_opt->qlen_young;
}

static inline int reqsk_queue_is_full(const struct request_sock_queue *queue)
{
	return queue->listen_opt->qlen >> queue->listen_opt->max_qlen_log;
}

static inline void reqsk_queue_hash_req(struct request_sock_queue *queue,
					u32 hash, struct request_sock *req,
					unsigned long timeout)
{
	struct listen_sock *lopt = queue->listen_opt;

	req->expires = jiffies + timeout;
	req->retrans = 0;
	req->sk = NULL;
	req->dl_next = lopt->syn_table[hash];

	write_lock(&queue->syn_wait_lock);
	lopt->syn_table[hash] = req;
	write_unlock(&queue->syn_wait_lock);
}

#endif /* _REQUEST_SOCK_H */
