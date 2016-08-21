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
/* ������������ĺ���ָ����������ڷ���SYN+ACK�Ρ�ACK�Ρ�RST�εĺ��� */
struct request_sock_ops {
	int		family; /* ����Э���� */
	kmem_cache_t	*slab;
	int		obj_size;
	/* ����syn+ack�εĺ���ָ�룬ֵ: tcp_v4_send_synack() */
	int		(*rtx_syn_ack)(struct sock *sk,
				       struct request_sock *req,
				       struct dst_entry *dst);
	/* ����ACK�εĺ���ָ�룬ֵ: tcp_v4_reqsk_send_ack() */
	void		(*send_ack)(struct sk_buff *skb,
				    struct request_sock *req);
	/* ����RST�εĺ���ָ�룬ֵ: tcp_v4_send_reset() */
	void		(*send_reset)(struct sk_buff *skb);
	/* ����������tcp_v4_reqsk_destructor() */
	void		(*destructor)(struct request_sock *req);
};

/* struct request_sock - mini sock to represent a connection request
 */
struct request_sock {
	/* ���Ӽ� */
	struct request_sock		*dl_next; /* Must be first member! */
	u16				mss; /* �ͻ��������������ͨ���MSS,����ͨ�棬���ʼֵ536 */
	u8				retrans; /* �ش�SYN+ACK�εĴ������ޣ����ﵽ���ޣ�����ȡ�� */
	u8				__pad; /* <δʹ��> */
	/* The following two fields can be easily recomputed I think -AK */
	/* ��ʶ���˵����ͨ�д��ڣ�������SYN+ACK��ʱ�����ֵ */
	u32				window_clamp; /* window clamp at creation time */
	/* ��ʶ�����ӽ���ʱ���˽��մ��ڴ�С����ʼ��Ϊ0��������SYN+ACK��ʱ�����ֵ */
	u32				rcv_wnd;	  /* rcv_wnd offered first time */
	/* ��һ����Ҫ���͵�ACK�е�ʱ���ֵ�� */
	u32				ts_recent;
	/* ������SYN+ACK�κ󣬵ȴ��ͻ���Ӧ�ĳ�ʱʱ�䡣�����ʱ�ͻ��ط�SYN+ACK����ֱ���ش������ﵽ���� */
	unsigned long			expires;
	/* ����������ĺ���ָ��� */
	struct request_sock_ops		*rsk_ops;
	/* ������ƿ� */
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
/* �洢��������飬��listenϵͳ����֮�󴴽� */
struct listen_sock {
	u8			max_qlen_log; /* nr_table_entries����2Ϊ�׵Ķ���ֵ */
	/* 3 bytes hole, try to use */
	int			qlen;/* ��ǰ�����������Ŀ */
	int			qlen_young;/* ��ǰδ�ش���SYN+ACK�ε��������Ŀ�� */
	int			clock_hand;
	u32			hash_rnd;
	/* ʵ�ʷ�����������SYN�������ӵ�request_sock�ṹ����ĳ��� */
	u32			nr_table_entries;
	struct request_sock	*syn_table[0]; /* ��listenϵͳ���������� */
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
/* ���ڴ����������������(����SYN_RECV״̬�Լ������ӵ�δ��accept�Ĵ�����ƿ�) */
struct request_sock_queue {
	/* ������Ӻ����������飬��ִ��accept()��listen_opt���Ƶ������� */
	struct request_sock	*rskq_accept_head;
	struct request_sock	*rskq_accept_tail;
	rwlock_t		syn_wait_lock;
	u8			rskq_defer_accept;
	/* 3 bytes hole, try to pack */
	struct listen_sock	*listen_opt; /* ��ʵ��������ʱ���� */
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
 * ����: ��ȡ�Ѿ����ӵĴ�����ƿ�
 * ����:
 * ����ֵ:
 * ˵��:
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
