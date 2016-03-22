/*
 *  CFQ, or complete fairness queueing, disk scheduler.
 *
 *  Based on ideas from a previously unfinished io
 *  scheduler (round robin per-process disk scheduling) and Andrea Arcangeli.
 *
 *  Copyright (C) 2003 Jens Axboe <axboe@suse.de>
 */


#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/hash.h>
#include <linux/rbtree.h>
#include <linux/ioprio.h>

/*
 * tunables
 */
static const int cfq_quantum = 4;		/* max queue in one round of service */
static const int cfq_queued = 8;		/* minimum rq allocate limit per-queue*/
static const int cfq_fifo_expire[2] = { HZ / 4, HZ / 8 };
static const int cfq_back_max = 16 * 1024;	/* maximum backwards seek, in KiB */
static const int cfq_back_penalty = 2;		/* penalty of a backwards seek */

static const int cfq_slice_sync = HZ / 10;
static int cfq_slice_async = HZ / 25;
static const int cfq_slice_async_rq = 2;
static int cfq_slice_idle = HZ / 125;

#define CFQ_IDLE_GRACE		(HZ / 10)
#define CFQ_SLICE_SCALE		(5)

#define CFQ_KEY_ASYNC		(0)

static DEFINE_SPINLOCK(cfq_exit_lock);

/*
 * for the hash of cfqq inside the cfqd
 */
#define CFQ_QHASH_SHIFT		6
#define CFQ_QHASH_ENTRIES	(1 << CFQ_QHASH_SHIFT)
#define list_entry_qhash(entry)	hlist_entry((entry), struct cfq_queue, cfq_hash)

/*
 * for the hash of crq inside the cfqq
 */
#define CFQ_MHASH_SHIFT		6
#define CFQ_MHASH_BLOCK(sec)	((sec) >> 3)
#define CFQ_MHASH_ENTRIES	(1 << CFQ_MHASH_SHIFT) /*64*/
#define CFQ_MHASH_FN(sec)	hash_long(CFQ_MHASH_BLOCK(sec), CFQ_MHASH_SHIFT)
#define rq_hash_key(rq)		((rq)->sector + (rq)->nr_sectors)
#define list_entry_hash(ptr)	hlist_entry((ptr), struct cfq_rq, hash)

#define list_entry_cfqq(ptr)	list_entry((ptr), struct cfq_queue, cfq_list)
#define list_entry_fifo(ptr)	list_entry((ptr), struct request, queuelist)

#define RQ_DATA(rq)		(rq)->elevator_private

/*
 * rb-tree defines
 */
#define rb_entry_crq(node)	rb_entry((node), struct cfq_rq, rb_node)
#define rq_rb_key(rq)		(rq)->sector

static kmem_cache_t *crq_pool;
static kmem_cache_t *cfq_pool;
static kmem_cache_t *cfq_ioc_pool;

static atomic_t ioc_count = ATOMIC_INIT(0);
static struct completion *ioc_gone;

#define CFQ_PRIO_LISTS		IOPRIO_BE_NR
#define cfq_class_idle(cfqq)	((cfqq)->ioprio_class == IOPRIO_CLASS_IDLE)
#define cfq_class_be(cfqq)	((cfqq)->ioprio_class == IOPRIO_CLASS_BE)
#define cfq_class_rt(cfqq)	((cfqq)->ioprio_class == IOPRIO_CLASS_RT)

#define ASYNC			(0)
#define SYNC			(1)

/* cfqq分发个数 */
#define cfq_cfqq_dispatched(cfqq)	\
	((cfqq)->on_dispatch[ASYNC] + (cfqq)->on_dispatch[SYNC])

#define cfq_cfqq_class_sync(cfqq)	((cfqq)->key != CFQ_KEY_ASYNC)

#define cfq_cfqq_sync(cfqq)		\
	(cfq_cfqq_class_sync(cfqq) || (cfqq)->on_dispatch[SYNC])

#define sample_valid(samples)	((samples) > 80)

/*
 * Per block device queue structure
 */
/*
 * 注:连接cfq_queue有列表:rr_list和Hash表:cfq_hash，这两者关系?
 */
/*
 * CFQ基本思想: 力图为竞争块设备使用权的所有进程分配一个等同的时间片，在调度器分配给进程的时间片内，
 *		   进程可以将其读写请求发送给底层块设备，当进程的时间片消耗完，进程的请求队列将被挂起，等待调度
 */
struct cfq_data {
	request_queue_t *queue; /* 请求队列对象 */

	/*
	 * rr list of queues with requests and the count of them
	 */
	/*
	 * cfq_queue对象的链表头，连接件:cfq_queue:cfq_list
	 * 这些链表以cfq_queue:service_last从小到大进行排序。
	 */
	struct list_head rr_list[CFQ_PRIO_LISTS]; /* 八个等级的优先级队列 */ 
	struct list_head busy_rr;	/* 忙时的CFQ queue对象队列 */
	struct list_head cur_rr;	/* 当前的运行CFQ queue对象队列 */
	struct list_head idle_rr;  /* 空闲CFQ qeueue对象队列 */
	unsigned int busy_queues;  /* 在cfq_data队列(rr_list/busy_rr/cur_rr/idle_rr)中的cfq_queue对象个数  */

	/*
	 * non-ordered list of empty cfqq's
	 */
	struct list_head empty_list; /* 将没有request请求的cfq_queue对象移入此对列 */

	/*
	 * cfqq lookup hash
	 */
	struct hlist_head *cfq_hash;/* 以进程PID为key的hash链表(cfq_queue) */

	/*
	 * global crq hash for all queues
	 */
	/* 1.存在cfq_rq节点的hash表示头,此域主要为了方便找到后插的request，此Hash的key值为request请求的最后一个扇区 */
	struct hlist_head *crq_hash;

	mempool_t *crq_pool;

	int rq_in_driver;
	int hw_tag;

	/*
	 * schedule slice state info
	 */
	/*
	 * idle window management
	 */
	struct timer_list idle_slice_timer;
	struct work_struct unplug_work;

	/* 从这个cfg_queue分发请求到派发队列 */
	struct cfq_queue *active_queue;
	struct cfq_io_context *active_cic;
	
	/* cur_prio:表示处理此级别的cfq_queue对象可以被执行，cur_end_prio:表示当前最高的级别 */
	int cur_prio, cur_end_prio; 
	unsigned int dispatch_slice;

	struct timer_list idle_class_timer;

	sector_t last_sector; 			/* 执行完成的扇区号(目的:计算next_rq) */
	unsigned long last_end_request; /* 请求完成的最后时刻。目的:计算空闲 */

	unsigned int rq_starved;

	/*
	 * tunables, see top of file
	 */
	unsigned int cfq_quantum;
	unsigned int cfq_queued;
	unsigned int cfq_fifo_expire[2];/* [0]表示异步操作的超时时间(HZ/4) --WRITE
									   [1]表示同步操作的超时时间(HZ/8)--READ/WRITE_SYNC */
	unsigned int cfq_back_penalty;
	unsigned int cfq_back_max;
	unsigned int cfq_slice[2];
	unsigned int cfq_slice_async_rq;
	unsigned int cfq_slice_idle;

	struct list_head cic_list; /* 链表头，用于连接cfq_io_context对象。列表中对象的个数表示正在访问此磁盘的进程数 */
};

/*
 * Per process-grouping structure
 */
/*
 * 1.一个cfq_queue对象存放着来自相同进程、相同IO优先级的request请求。
 * 2.主要包括三个队列:1.hash表(方便找到后插节点)；2.一棵红黑树(方便找到前插节点)；3.fifo队列(避免请求饿死)
 * 3.对一个磁盘的请求先组织成cfq_queue，然后再将所有的cfq_queue对象以Hash表示的形式组织起来，其中以进程PID为key,表头cfq_data:cfq_hash.
 */
struct cfq_queue {
	/* reference count */
	atomic_t ref;
	/* parent cfq_data */
	struct cfq_data *cfqd;
	/* cfqq lookup hash */
	struct hlist_node cfq_hash;/* 1.Hash表的连接件,把cfq_queue对象链接到cfq_data:cfq_hash中 */
	/* hash key */
	unsigned int key;/* 存放进程的PID */
	/* on either rr or empty list of cfqd */
	struct list_head cfq_list;/*连接件,链表头cfq_data:rr_list/busy_rr/cur_rr/idle_rr*/
	/* sorted list of pending requests */
	struct rb_root sort_list;/* 2.排序队列， 节点cfq_rq */
	/* if fifo isn't expired, next request to serve */
	struct cfq_rq *next_crq;/* 下一个请求(不一定是红黑树的下一个节点，有可能是前一个节点) */
	/* requests queued in sort_list */
	int queued[2];
	/* currently allocated requests */
	int allocated[2];
	/* fifo list of requests in sort_list */
	struct list_head fifo;/* 3.FIFO队列，节点为request，连接件为request:queuelist */

	unsigned long slice_start;
	unsigned long slice_end;	/* 一个cfq_queue可以执行的时间片，超时后此进程就要被换出不能再继续访问IO */
	unsigned long slice_left;	/* 剩余时间片 */
	unsigned long service_last; /* 最迟服务时刻 */

	/* number of requests that are on the dispatch list */
	int on_dispatch[2];	/* 已经分发到派发队列的request请求个数 */

	/* io prio of this group */
	unsigned short ioprio, org_ioprio;/* 进程的优先级,值:0~7 */
	unsigned short ioprio_class, org_ioprio_class;/* 进程的优先级别，值分别:Run-time,best-effect,idle */

	/* various state flags, see below */
	unsigned int flags;
};
/* 相当于在CFQ算法中，对request做了一层封装 
 * 注:此对象没有FIFO队列的连接件，FIFO的连接件用request:queuelist
 */
struct cfq_rq {
	struct rb_node rb_node; /* 连接件，做为插入到cfq_queue:sort_list红黑树的连接件，此红黑树的key为rb_key，即请求的起始扇区号 */
	sector_t rb_key;		/* 请求磁盘起始扇区 */
	struct request *request;	/* 请求对象 */
	struct hlist_node hash;    /* 连接件，做为插入到cfq_data:crq_hash的连接件*/

	struct cfq_queue *cfq_queue; /* 请求所属的cfq_queue对象 */
	struct cfq_io_context *io_context; 

	unsigned int crq_flags;
};

enum cfqq_state_flags {
	CFQ_CFQQ_FLAG_on_rr = 0,
	CFQ_CFQQ_FLAG_wait_request,
	CFQ_CFQQ_FLAG_must_alloc,
	CFQ_CFQQ_FLAG_must_alloc_slice,
	CFQ_CFQQ_FLAG_must_dispatch,
	CFQ_CFQQ_FLAG_fifo_expire,
	CFQ_CFQQ_FLAG_idle_window,
	CFQ_CFQQ_FLAG_prio_changed,
};

#define CFQ_CFQQ_FNS(name)						\
static inline void cfq_mark_cfqq_##name(struct cfq_queue *cfqq)		\
{									\
	cfqq->flags |= (1 << CFQ_CFQQ_FLAG_##name);			\
}									\
static inline void cfq_clear_cfqq_##name(struct cfq_queue *cfqq)	\
{									\
	cfqq->flags &= ~(1 << CFQ_CFQQ_FLAG_##name);			\
}									\
static inline int cfq_cfqq_##name(const struct cfq_queue *cfqq)		\
{									\
	return (cfqq->flags & (1 << CFQ_CFQQ_FLAG_##name)) != 0;	\
}

CFQ_CFQQ_FNS(on_rr);
CFQ_CFQQ_FNS(wait_request);
CFQ_CFQQ_FNS(must_alloc);
CFQ_CFQQ_FNS(must_alloc_slice);
CFQ_CFQQ_FNS(must_dispatch);
CFQ_CFQQ_FNS(fifo_expire);
CFQ_CFQQ_FNS(idle_window);
CFQ_CFQQ_FNS(prio_changed);
#undef CFQ_CFQQ_FNS

enum cfq_rq_state_flags {
	CFQ_CRQ_FLAG_is_sync = 0,
};

#define CFQ_CRQ_FNS(name)						\
static inline void cfq_mark_crq_##name(struct cfq_rq *crq)		\
{									\
	crq->crq_flags |= (1 << CFQ_CRQ_FLAG_##name);			\
}									\
static inline void cfq_clear_crq_##name(struct cfq_rq *crq)		\
{									\
	crq->crq_flags &= ~(1 << CFQ_CRQ_FLAG_##name);			\
}									\
static inline int cfq_crq_##name(const struct cfq_rq *crq)		\
{									\
	return (crq->crq_flags & (1 << CFQ_CRQ_FLAG_##name)) != 0;	\
}

CFQ_CRQ_FNS(is_sync);
#undef CFQ_CRQ_FNS

static struct cfq_queue *cfq_find_cfq_hash(struct cfq_data *, unsigned int, unsigned short);
static void cfq_dispatch_insert(request_queue_t *, struct cfq_rq *);
static struct cfq_queue *cfq_get_queue(struct cfq_data *cfqd, unsigned int key, struct task_struct *tsk, gfp_t gfp_mask);

/*
 * lots of deadline iosched dupes, can be abstracted later...
 */
/**ltl
 * 功能:将请求从Hash表中删除
 * 参数:
 * 返回值:
 * 说明:
 */
static inline void cfq_del_crq_hash(struct cfq_rq *crq)
{
	hlist_del_init(&crq->hash);
}

/**ltl
 * 功能:将请求插入到Hash表中
 * 参数:
 * 返回值:
 * 说明:
 */
static inline void cfq_add_crq_hash(struct cfq_data *cfqd, struct cfq_rq *crq)
{
	const int hash_idx = CFQ_MHASH_FN(rq_hash_key(crq->request));

	hlist_add_head(&crq->hash, &cfqd->crq_hash[hash_idx]);
}
/**ltl
 * 功能:在Hash表中查找请求的最后一磁盘扇区为offset的请求
 * 参数:
 * 返回值:
 * 说明:offset是请求的最后一扇区，主要是为了方便后插。
 */
static struct request *cfq_find_rq_hash(struct cfq_data *cfqd, sector_t offset)
{
	struct hlist_head *hash_list = &cfqd->crq_hash[CFQ_MHASH_FN(offset)];
	struct hlist_node *entry, *next;

	hlist_for_each_safe(entry, next, hash_list) {
		struct cfq_rq *crq = list_entry_hash(entry);
		struct request *__rq = crq->request;

		if (!rq_mergeable(__rq)) { /* 不能合并的请求从Hash表中删除 */
			cfq_del_crq_hash(crq);
			continue;
		}
		/* 比较Hash key值 */
		if (rq_hash_key(__rq) == offset)
			return __rq;
	}

	return NULL;
}

/*
 * scheduler run of queue, if there are requests pending and no one in the
 * driver that will restart queueing
 */
static inline void cfq_schedule_dispatch(struct cfq_data *cfqd)
{
	if (cfqd->busy_queues)
		kblockd_schedule_work(&cfqd->unplug_work);
}
/**ltl
功能:判定IO调度器是否为空
*/
static int cfq_queue_empty(request_queue_t *q)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;

	return !cfqd->busy_queues;
}
/**ltl
功能:如果是读或者同步写，则返回的时进程PID，如果是写，返回0
注:从这个函数能不能说明读为同步，写操作为异步
*/
static inline pid_t cfq_queue_pid(struct task_struct *task, int rw)
{
	if (rw == READ || rw == WRITE_SYNC) /* 同步操作(读/写、同步写读) */
		return task->pid;

	return CFQ_KEY_ASYNC;
}

/*
 * Lifted from AS - choose which of crq1 and crq2 that is best served now.
 * We choose the request that is closest to the head right now. Distance
 * behind the head is penalized and only allowed to a certain extent.
 */
/**ltl
 * 功能:从crq1和cfq2中选择最靠近磁头的请求。(这个函数的实现细节没有搞明白)
 */
static struct cfq_rq *
cfq_choose_req(struct cfq_data *cfqd, struct cfq_rq *crq1, struct cfq_rq *crq2)
{
	sector_t last, s1, s2, d1 = 0, d2 = 0;
	unsigned long back_max;
#define CFQ_RQ1_WRAP	0x01 /* request 1 wraps */
#define CFQ_RQ2_WRAP	0x02 /* request 2 wraps */
	unsigned wrap = 0; /* bit mask: requests behind the disk head? */

	if (crq1 == NULL || crq1 == crq2)
		return crq2;
	if (crq2 == NULL)
		return crq1;

	if (cfq_crq_is_sync(crq1) && !cfq_crq_is_sync(crq2))
		return crq1;
	else if (cfq_crq_is_sync(crq2) && !cfq_crq_is_sync(crq1))
		return crq2;

	s1 = crq1->request->sector;
	s2 = crq2->request->sector;

	last = cfqd->last_sector;

	/*
	 * by definition, 1KiB is 2 sectors
	 */
	back_max = cfqd->cfq_back_max * 2;

	/*
	 * Strict one way elevator _except_ in the case where we allow
	 * short backward seeks which are biased as twice the cost of a
	 * similar forward seek.
	 */
	if (s1 >= last)
		d1 = s1 - last;
	else if (s1 + back_max >= last)
		d1 = (last - s1) * cfqd->cfq_back_penalty;
	else
		wrap |= CFQ_RQ1_WRAP;

	if (s2 >= last)
		d2 = s2 - last;
	else if (s2 + back_max >= last)
		d2 = (last - s2) * cfqd->cfq_back_penalty;
	else
		wrap |= CFQ_RQ2_WRAP;

	/* Found required data */

	/*
	 * By doing switch() on the bit mask "wrap" we avoid having to
	 * check two variables for all permutations: --> faster!
	 */
	switch (wrap) {
	case 0: /* common case for CFQ: crq1 and crq2 not wrapped */
		if (d1 < d2)
			return crq1;
		else if (d2 < d1)
			return crq2;
		else {
			if (s1 >= s2)
				return crq1;
			else
				return crq2;
		}

	case CFQ_RQ2_WRAP:
		return crq1;
	case CFQ_RQ1_WRAP:
		return crq2;
	case (CFQ_RQ1_WRAP|CFQ_RQ2_WRAP): /* both crqs wrapped */
	default:
		/*
		 * Since both rqs are wrapped,
		 * start with the one that's further behind head
		 * (--> only *one* back seek required),
		 * since back seek takes more time than forward.
		 */
		if (s1 <= s2)
			return crq1;
		else
			return crq2;
	}
}

/*
 * would be nice to take fifo expire time into account as well
 */
static struct cfq_rq *
cfq_find_next_crq(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		  struct cfq_rq *last)
{
	struct cfq_rq *crq_next = NULL, *crq_prev = NULL;
	struct rb_node *rbnext, *rbprev;
	//求下一个节点
	if (!(rbnext = rb_next(&last->rb_node))) {
		rbnext = rb_first(&cfqq->sort_list);
		if (rbnext == &last->rb_node)//说明last就是第一个节点，即sort_list是只有一个节点的树
			rbnext = NULL;
	}
	//求前一个节点
	rbprev = rb_prev(&last->rb_node);

	if (rbprev)
		crq_prev = rb_entry_crq(rbprev);
	if (rbnext)
		crq_next = rb_entry_crq(rbnext);
	//从prev和next中选一个离当前磁头最近的那个请求
	return cfq_choose_req(cfqd, crq_next, crq_prev);
}

//更新next_crq的指针
static void cfq_update_next_crq(struct cfq_rq *crq)
{
	struct cfq_queue *cfqq = crq->cfq_queue;

	if (cfqq->next_crq == crq)
		cfqq->next_crq = cfq_find_next_crq(cfqq->cfqd, cfqq, crq);
}
/**ltl
 * 功能:把cfqq对象以升序形式插入到cfq_data中的队列中。
 * 参数:	
 *	cfqq	->需要插入的cfq_queue对象
 *   preempted	->是否以抢占的方式插入
 * 返回值:
 * 说明: 如果以抢占方式插入到队列中，不用排序，只插入到队列头
 */
static void cfq_resort_rr_list(struct cfq_queue *cfqq, int preempted)
{
	struct cfq_data *cfqd = cfqq->cfqd;
	struct list_head *list, *entry;

	BUG_ON(!cfq_cfqq_on_rr(cfqq));

	list_del(&cfqq->cfq_list); /* 使cfq_queue对象处理于游离状态 */
	/* RT类型CFQ队列优先级最高，直接把请求插入到当前队列cur_rr */
	if (cfq_class_rt(cfqq))
		list = &cfqd->cur_rr;
	else if (cfq_class_idle(cfqq)) /* idle类型的请求，插入到idle队列中 */
		list = &cfqd->idle_rr; /* idle类型的CFQ QUEUE */
	else {
		/*
		 * if cfqq has requests in flight, don't allow it to be
		 * found in cfq_set_active_queue before it has finished them.
		 * this is done to increase fairness between a process that
		 * has lots of io pending vs one that only generates one
		 * sporadically or synchronously
		 */
		if (cfq_cfqq_dispatched(cfqq)) /* 队列正在分发请求 */
			list = &cfqd->busy_rr;	/* 对"繁忙"队列操作 */
		else
			list = &cfqd->rr_list[cfqq->ioprio];/*ioprio:0~7*/ /* 根据不同的优先级选择要插入的队列 */
	}

	/*
	 * if queue was preempted, just add to front to be fair. busy_rr
	 * isn't sorted, but insert at the back for fairness.
	 */
	if (preempted || list == &cfqd->busy_rr) { /* 允许抢占，或者刚好是忙队列 */
		if (preempted) /* 允许抢占，则把cfq_queue插入到队列头，而如果是忙队列，则插入到队尾 */
			list = list->prev;

		list_add_tail(&cfqq->cfq_list, list);
		return;
	}

	/*
	 * sort by when queue was last serviced
	 */
	/* 按service_last值从小到大排序 */
	entry = list;
	while ((entry = entry->prev) != list) {
		struct cfq_queue *__cfqq = list_entry_cfqq(entry);

		if (!__cfqq->service_last)
			break;
		if (time_before(__cfqq->service_last, cfqq->service_last)) 
			break;
	}

	list_add(&cfqq->cfq_list, entry);	/* 把cfqq插入到entry元素之后 */
}

/*
 * add to busy list of queues for service, trying to be fair in ordering
 * the pending list according to last request service
 */
/**ltl
 * 功能:把cfqq对象插入到cfq_data的队列中。
 * 参数:
 * 返回值:
 * 说明:
 */
static inline void
cfq_add_cfqq_rr(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	BUG_ON(cfq_cfqq_on_rr(cfqq));
	cfq_mark_cfqq_on_rr(cfqq); /* 标记cfqq正在排序 */
	cfqd->busy_queues++; 	/* 正在排序的cfq_queue个数，即在cfq_data队列中的cfq_queue对象个数 */
	/*  以非抢占方式插入到cfq_data队列中 */
	cfq_resort_rr_list(cfqq, 0);
}
/**ltl
 * 功能: 清除on_rr标志，同时把cfqq对象移至empty_list队列中
 * 参数:
 * 返回值:
 * 说明:这个函数只有在请求被合并或者是请求转移到派发队列中才调用。
 */
static inline void
cfq_del_cfqq_rr(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	BUG_ON(!cfq_cfqq_on_rr(cfqq));
	cfq_clear_cfqq_on_rr(cfqq);
	list_move(&cfqq->cfq_list, &cfqd->empty_list);

	BUG_ON(!cfqd->busy_queues);
	cfqd->busy_queues--;
}

/*
 * rb tree support functions
 */
/* 从排序树中删除一个节点 */
static inline void cfq_del_crq_rb(struct cfq_rq *crq)
{
	struct cfq_queue *cfqq = crq->cfq_queue;
	struct cfq_data *cfqd = cfqq->cfqd;
	const int sync = cfq_crq_is_sync(crq);

	BUG_ON(!cfqq->queued[sync]);
	cfqq->queued[sync]--;
	/* 更新next_crq指针 */
	cfq_update_next_crq(crq);

	rb_erase(&crq->rb_node, &cfqq->sort_list);
	/* 1.有标志on_rr，并且cfq队列没有请求 */
	if (cfq_cfqq_on_rr(cfqq) && RB_EMPTY_ROOT(&cfqq->sort_list))
		cfq_del_cfqq_rr(cfqd, cfqq); /* 1.清除标志；2.把cfqq对象移入到empty_list列表。 */
}
/**ltl
 * 功能:在排序队列(红黑树)中查找crq如果找到，直接返回，否则设置其父节点
 */
static struct cfq_rq *
__cfq_add_crq_rb(struct cfq_rq *crq)
{
	struct rb_node **p = &crq->cfq_queue->sort_list.rb_node;
	struct rb_node *parent = NULL;
	struct cfq_rq *__crq;

	while (*p) {
		parent = *p;
		__crq = rb_entry_crq(parent);

		if (crq->rb_key < __crq->rb_key)
			p = &(*p)->rb_left;
		else if (crq->rb_key > __crq->rb_key)
			p = &(*p)->rb_right;
		else
			return __crq;
	}

	rb_link_node(&crq->rb_node, parent, p);/* 设置父节点 */
	return NULL;
}
/**ltl
 * 功能:把request插入到排序树中。同时要根据一定算法插入到busy_rr,idle_rr,cur_rr三个队列中
 * 参数:
 *说明:主要完成两个步骤:
 *	1.把请求插入到cfq_queue的红黑树中。
 *	2.把cfq_queue对象添加到cfq_data的优先级队列中cfq_data:list_rr
 */
static void cfq_add_crq_rb(struct cfq_rq *crq)
{
	struct cfq_queue *cfqq = crq->cfq_queue;
	struct cfq_data *cfqd = cfqq->cfqd;
	struct request *rq = crq->request;
	struct cfq_rq *__alias;

	crq->rb_key = rq_rb_key(rq);/* 设置cfq_rq的rb key值为磁盘的起始地址。*/
	cfqq->queued[cfq_crq_is_sync(crq)]++;/* cfq_queue中的request个数 */

	/*
	 * looks a little odd, but the first insert might return an alias.
	 * if that happens, put the alias on the dispatch list
	 */
	while ((__alias = __cfq_add_crq_rb(crq)) != NULL)/* 如果在红黑树中已经存在key值相同的节点，则把当前请求派发到派发队列中 */
		cfq_dispatch_insert(cfqd->queue, __alias);
	/* 1.新插入的节点存放到红黑树中 */
	rb_insert_color(&crq->rb_node, &cfqq->sort_list);

	/* 2.将cfqq对象以升序插入cfq_data中的队列中。 */
	if (!cfq_cfqq_on_rr(cfqq))
		cfq_add_cfqq_rr(cfqd, cfqq);

	/*
	 * check if this request is a better next-serve candidate
	 */
	 /* 记录下个节点的地址 */
	cfqq->next_crq = cfq_choose_req(cfqd, cfqq->next_crq, crq);
}

//调整crq在红黑树中的位置
static inline void
cfq_reposition_crq_rb(struct cfq_queue *cfqq, struct cfq_rq *crq)
{
	rb_erase(&crq->rb_node, &cfqq->sort_list);//先从树中删除
	cfqq->queued[cfq_crq_is_sync(crq)]--;//因为在cfq_add_crq_rb函数实现里面有++，所以在这里要--
	//添加到排序树中
	cfq_add_crq_rb(crq);
}
/**ltl
 * 功能:在排序树中查找可以与bio合并的request
 * 参数:
 * 返回值:
 */
static struct request *
cfq_find_rq_fmerge(struct cfq_data *cfqd, struct bio *bio)
{
	struct task_struct *tsk = current;
	pid_t key = cfq_queue_pid(tsk, bio_data_dir(bio));
	struct cfq_queue *cfqq;
	struct rb_node *n;
	sector_t sector;
	/* 根据进程PID值从Hash表中获取cfq_queue对象 */
	cfqq = cfq_find_cfq_hash(cfqd, key, tsk->ioprio);
	if (!cfqq)
		goto out;

	sector = bio->bi_sector + bio_sectors(bio);
	n = cfqq->sort_list.rb_node;
	while (n) {
		struct cfq_rq *crq = rb_entry_crq(n);

		if (sector < crq->rb_key)
			n = n->rb_left;
		else if (sector > crq->rb_key)
			n = n->rb_right;
		else
			return crq->request;
	}

out:
	return NULL;
}

static void cfq_activate_request(request_queue_t *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;

	cfqd->rq_in_driver++;

	/*
	 * If the depth is larger 1, it really could be queueing. But lets
	 * make the mark a little higher - idling could still be good for
	 * low queueing, and a low queueing number could also just indicate
	 * a SCSI mid layer like behaviour where limit+1 is often seen.
	 */
	if (!cfqd->hw_tag && cfqd->rq_in_driver > 4)
		cfqd->hw_tag = 1;
}

static void cfq_deactivate_request(request_queue_t *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;

	WARN_ON(!cfqd->rq_in_driver);
	cfqd->rq_in_driver--;
}
/**ltl
 * 功能:把rq对象从三个列表中脱落，成为孤立节点。
 * 参数:
 * 返回值:
 */
static void cfq_remove_request(struct request *rq)
{
	struct cfq_rq *crq = RQ_DATA(rq);
	/* 1.从列表中删除 */
	list_del_init(&rq->queuelist);
	/* 2.从排序树中删除 */
	cfq_del_crq_rb(crq);
	/* 3.从hash表中删除 */
	cfq_del_crq_hash(crq);
}
/**ltl
 * 功能:获取bio添加到req的位置
 * 参数:
 * 返回值:
 */
static int
cfq_merge(request_queue_t *q, struct request **req, struct bio *bio)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct request *__rq;
	int ret;
	/* 从Hash中获取可以与bio合并的request (Q:后插为什么用Hash表?) */
	__rq = cfq_find_rq_hash(cfqd, bio->bi_sector);
	if (__rq && elv_rq_merge_ok(__rq, bio)) {/* 判定是否能够合并 */
		ret = ELEVATOR_BACK_MERGE;
		goto out;
	}
	/* 从排序树中获取可以与bio合并的request (Q:前插入为什么要用红黑树呢? 用Hash表示可以吗? 是为了方便求出下一个请求(next_rq)吗?) */
	__rq = cfq_find_rq_fmerge(cfqd, bio);
	if (__rq && elv_rq_merge_ok(__rq, bio)) { /* 判定是否能够合并 */
		ret = ELEVATOR_FRONT_MERGE;
		goto out;
	}

	return ELEVATOR_NO_MERGE;
out:
	*req = __rq;
	return ret;
}
/**ltl
 * 功能:这个函数调整request中与CFQ调度算法相关的数据，在此之前，已经有一个bio与req进行合并
 * 参数:
 * 返回值:
 */
static void cfq_merged_request(request_queue_t *q, struct request *req)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_rq *crq = RQ_DATA(req);
	
	cfq_del_crq_hash(crq);/* 先从hash表中脱落 */
	cfq_add_crq_hash(cfqd, crq);/* 重新插入到hash表中(req的section和sectio_size已经发生了变化) */

	if (rq_rb_key(req) != crq->rb_key) {/* rb_key:记录的是在bio合到request之前的数值 */
		struct cfq_queue *cfqq = crq->cfq_queue;

		cfq_update_next_crq(crq);/* 重新记录crq_next地址 */
		cfq_reposition_crq_rb(cfqq, crq);/* 调整request在排序树中的位置 */
	}
}
/**ltl
 * 功能:调整rq在调度器中的位置，并删除next对象。在调用这个函数之前，已经把next的数据合入到rq中。
 * 参数:
 * 返回值:
 */
static void
cfq_merged_requests(request_queue_t *q, struct request *rq,
		    struct request *next)
{
	/* 调整rq在IO调度器的位置 */
	cfq_merged_request(q, rq);

	/*
	 * reposition in fifo if next is older than rq
	 */
	 /*Q:为什么要这步呢?*/
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist) &&
	    time_before(next->start_time, rq->start_time))
		list_move(&rq->queuelist, &next->queuelist);
	/* 把rq对象从三个列表中脱落，成为孤立节点。*/
	cfq_remove_request(next);
}
/**ltl
 * 功能:设置活动的CFQ队列
 * 参数:
 * 返回值:
 */
static inline void
__cfq_set_active_queue(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	if (cfqq) {
		/*
		 * stop potential idle class queues waiting service
		 */
		del_timer(&cfqd->idle_class_timer);

		cfqq->slice_start = jiffies;
		cfqq->slice_end = 0;
		cfqq->slice_left = 0;
		cfq_clear_cfqq_must_alloc_slice(cfqq);
		cfq_clear_cfqq_fifo_expire(cfqq);
	}

	cfqd->active_queue = cfqq;
}

/*
 * current cfqq expired its slice (or was too idle), select new one
 */
/**ltl
功能:将时间片耗完的cfqq重新插入到cfq_data的队列中
参数:
返回值:
*/
static void
__cfq_slice_expired(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		    int preempted)
{
	unsigned long now = jiffies;

	if (cfq_cfqq_wait_request(cfqq))
		del_timer(&cfqd->idle_slice_timer);

	if (!preempted && !cfq_cfqq_dispatched(cfqq)) {
		cfqq->service_last = now;
		cfq_schedule_dispatch(cfqd);
	}

	cfq_clear_cfqq_must_dispatch(cfqq);
	cfq_clear_cfqq_wait_request(cfqq);

	/*
	 * store what was left of this slice, if the queue idled out
	 * or was preempted
	 */
	if (time_after(cfqq->slice_end, now))/* 更新cfq_queue剩余时间片 */
		cfqq->slice_left = cfqq->slice_end - now;
	else
		cfqq->slice_left = 0;

	if (cfq_cfqq_on_rr(cfqq))
		cfq_resort_rr_list(cfqq, preempted);

	if (cfqq == cfqd->active_queue)
		cfqd->active_queue = NULL;

	if (cfqd->active_cic) {
		put_io_context(cfqd->active_cic->ioc);
		cfqd->active_cic = NULL;
	}

	cfqd->dispatch_slice = 0;
}
/**ltl
 * 功能:将活动的cfqq重新插入到cfq_data的队列(service_tree)中。
 * 参数:
 * 返回值:
 * 说明:
 */
static inline void cfq_slice_expired(struct cfq_data *cfqd, int preempted)
{
	/* 活动的CFQ queue队列 */
	struct cfq_queue *cfqq = cfqd->active_queue;

	if (cfqq)
		__cfq_slice_expired(cfqd, cfqq, preempted);
}

/*
 * 0
 * 0,1
 * 0,1,2
 * 0,1,2,3
 * 0,1,2,3,4
 * 0,1,2,3,4,5
 * 0,1,2,3,4,5,6
 * 0,1,2,3,4,5,6,7
 */
/**ltl
 * 功能:从当前级别到最后一个级别这个区间找出一个中找出一个含有request的列表，然后把这个列表添加到cur_rr列表之后。
 * 参数:
 * 返回值:
 * 说明:完全公平算法的公平性就体现在这里。
 */
static int cfq_get_next_prio_level(struct cfq_data *cfqd)
{
	int prio, wrap;

	prio = -1;
	wrap = 0;
	do {
		int p;
		//从当前级别的列表到最后一个级别列表中找出一个含有request的列表
		for (p = cfqd->cur_prio; p <= cfqd->cur_end_prio; p++) {
			if (!list_empty(&cfqd->rr_list[p])) {
				prio = p;
				break;
			}
		}

		if (prio != -1)
			break;
		cfqd->cur_prio = 0;
		/* 更新当前最后的优先级别 */
		if (++cfqd->cur_end_prio == CFQ_PRIO_LISTS) {
			cfqd->cur_end_prio = 0;
			if (wrap)
				break;
			wrap = 1;
		}
	} while (1);

	if (unlikely(prio == -1))
		return -1;

	BUG_ON(prio >= CFQ_PRIO_LISTS);
	//把rr_list列表插入到cur_rr列表之后
	list_splice_init(&cfqd->rr_list[prio], &cfqd->cur_rr);
	/* 下次就要从此处的优先级开始取 */
	cfqd->cur_prio = prio + 1;
	if (cfqd->cur_prio > cfqd->cur_end_prio) {
		cfqd->cur_end_prio = cfqd->cur_prio;
		cfqd->cur_prio = 0;
	}
	if (cfqd->cur_end_prio == CFQ_PRIO_LISTS) {
		cfqd->cur_prio = 0;
		cfqd->cur_end_prio = 0;
	}

	return prio;
}
/**ltl
 * 功能:获取active cfq_queue，从这个cfq_queue中的队列的元素分发到派发队列
 * 参数:
 */
static struct cfq_queue *cfq_set_active_queue(struct cfq_data *cfqd)
{
	struct cfq_queue *cfqq = NULL;

	/*
	 * if current list is non-empty, grab first entry. if it is empty,
	 * get next prio level and grab first entry then if any are spliced
	 */
	/* 如果cur_rr列表不为空，或者下一级别的列表也不为空，则从cur_rr列表中取出第一个对象标记为active_queue,并返回这个cfg_queue */
	if (!list_empty(&cfqd->cur_rr) || cfq_get_next_prio_level(cfqd) != -1)
		cfqq = list_entry_cfqq(cfqd->cur_rr.next);

	/*
	 * If no new queues are available, check if the busy list has some
	 * before falling back to idle io.
	 */
	/* 如果busy_rr队列不空，取出第一个作为active_queue */
	if (!cfqq && !list_empty(&cfqd->busy_rr))
		cfqq = list_entry_cfqq(cfqd->busy_rr.next);

	/*
	 * if we have idle queues and no rt or be queues had pending
	 * requests, either allow immediate service if the grace period
	 * has passed or arm the idle grace timer
	 */
	/* 如果idle队列不为空，并且离上一次操作完成已经过了HZ/10，则从这个队列取出cfg_queue,并置为active_queue */
	if (!cfqq && !list_empty(&cfqd->idle_rr)) {
		unsigned long end = cfqd->last_end_request + CFQ_IDLE_GRACE;

		if (time_after_eq(jiffies, end))
			cfqq = list_entry_cfqq(cfqd->idle_rr.next);
		else
			mod_timer(&cfqd->idle_class_timer, end);
	}
	/* 设置活动队列 */
	__cfq_set_active_queue(cfqd, cfqq);
	return cfqq;
}

#define CIC_SEEKY(cic) ((cic)->seek_mean > (128 * 1024))

static int cfq_arm_slice_timer(struct cfq_data *cfqd, struct cfq_queue *cfqq)

{
	struct cfq_io_context *cic;
	unsigned long sl;

	WARN_ON(!RB_EMPTY_ROOT(&cfqq->sort_list));
	WARN_ON(cfqq != cfqd->active_queue);

	/*
	 * idle is disabled, either manually or by past process history
	 */
	if (!cfqd->cfq_slice_idle)
		return 0;
	if (!cfq_cfqq_idle_window(cfqq))
		return 0;
	/*
	 * task has exited, don't wait
	 */
	cic = cfqd->active_cic;
	if (!cic || !cic->ioc->task)
		return 0;

	cfq_mark_cfqq_must_dispatch(cfqq);
	cfq_mark_cfqq_wait_request(cfqq);

	sl = min(cfqq->slice_end - 1, (unsigned long) cfqd->cfq_slice_idle);

	/*
	 * we don't want to idle for seeks, but we do want to allow
	 * fair distribution of slice time for a process doing back-to-back
	 * seeks. so allow a little bit of time for him to submit a new rq
	 */
	if (sample_valid(cic->seek_samples) && CIC_SEEKY(cic))
		sl = min(sl, msecs_to_jiffies(2));

	mod_timer(&cfqd->idle_slice_timer, jiffies + sl);
	return 1;
}
/**ltl
 * 功能:把request移到派发队列中。
 * 参数:
 * 返回值:
 */
static void cfq_dispatch_insert(request_queue_t *q, struct cfq_rq *crq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_queue *cfqq = crq->cfq_queue;
	struct request *rq;

	cfqq->next_crq = cfq_find_next_crq(cfqd, cfqq, crq);/* 记录下一个cfq_rq对象地址(离当前磁头最近的请求) */
	cfq_remove_request(crq->request);/* 从三个队列(list,rb_tree,hash)删除节点 */
	cfqq->on_dispatch[cfq_crq_is_sync(crq)]++;/* 到派发队列的个数 */
	elv_dispatch_sort(q, crq->request);/* 把request从IO调度队列中转到派发队列中 */

	rq = list_entry(q->queue_head.prev, struct request, queuelist);
	cfqd->last_sector = rq->sector + rq->nr_sectors;/* 记录最后一个扇区号，在cfq_find_next_crq中会用到。*/
}

/*
 * return expired entry, or NULL to just start from scratch in rbtree
 */
/**ltl
 * 功能:判定FIFO队列中的请求是否已经过期
 */
static inline struct cfq_rq *cfq_check_fifo(struct cfq_queue *cfqq)
{
	struct cfq_data *cfqd = cfqq->cfqd;
	struct request *rq;
	struct cfq_rq *crq;

	if (cfq_cfqq_fifo_expire(cfqq))
		return NULL;

	if (!list_empty(&cfqq->fifo)) {
		int fifo = cfq_cfqq_class_sync(cfqq);/* 获取同步标志 */
		/* 取FIFO队列头元素 */
		crq = RQ_DATA(list_entry_fifo(cfqq->fifo.next));
		rq = crq->request;
		/* 已经超时 */
		if (time_after(jiffies, rq->start_time + cfqd->cfq_fifo_expire[fifo])) {
			cfq_mark_cfqq_fifo_expire(cfqq);
			return crq;
		}
	}

	return NULL;
}

/*
 * Scale schedule slice based on io priority. Use the sync time slice only
 * if a queue is marked sync and has sync io queued. A sync queue with async
 * io only, should not get full sync slice length.
 */
static inline int
cfq_prio_to_slice(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	const int base_slice = cfqd->cfq_slice[cfq_cfqq_sync(cfqq)];

	WARN_ON(cfqq->ioprio >= IOPRIO_BE_NR);

	return base_slice + (base_slice/CFQ_SLICE_SCALE * (4 - cfqq->ioprio));
}

static inline void
cfq_set_prio_slice(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	cfqq->slice_end = cfq_prio_to_slice(cfqd, cfqq) + jiffies;
}

static inline int
cfq_prio_to_maxrq(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	const int base_rq = cfqd->cfq_slice_async_rq;

	WARN_ON(cfqq->ioprio >= IOPRIO_BE_NR);

	return 2 * (base_rq + base_rq * (CFQ_PRIO_LISTS - 1 - cfqq->ioprio));
}

/*
 * get next queue for service
 */
static struct cfq_queue *cfq_select_queue(struct cfq_data *cfqd)
{
	unsigned long now = jiffies;
	struct cfq_queue *cfqq;
	/* 选取当前活动的cfq队列 */
	cfqq = cfqd->active_queue;
	if (!cfqq)
		goto new_queue;

	/*
	 * slice has expired
	 */
	/*  没有设置dispatch标志，但时间片已经过，则分发请求到派发队列 */
	if (!cfq_cfqq_must_dispatch(cfqq) && time_after(now, cfqq->slice_end))
		goto expire;

	/*
	 * if queue has requests, dispatch one. if not, check if
	 * enough slice is left to wait for one
	 */
	if (!RB_EMPTY_ROOT(&cfqq->sort_list)) /* 表示活动的队列中有请求，直接选取此队列 */
		goto keep_queue;
	else if (cfq_cfqq_dispatched(cfqq)) { /* 活动队列正在分发请求，则不能再分发队列了。 */
		cfqq = NULL;
		goto keep_queue;
	} else if (cfq_cfqq_class_sync(cfqq)) { /* 活动队列是一个同步请求 (读/写) */
		if (cfq_arm_slice_timer(cfqd, cfqq))
			return NULL;
	}

expire:
	cfq_slice_expired(cfqd, 0); /* 以非抢占方式更新cfq队列的时间片 */
new_queue:
	cfqq = cfq_set_active_queue(cfqd);/* 设置active_queue，*/
keep_queue:
	return cfqq;
}
/**ltl
 * 功能:
 * 参数:
 * 返回值:
 */
static int
__cfq_dispatch_requests(struct cfq_data *cfqd, struct cfq_queue *cfqq,
			int max_dispatch/*4*/)
{
	int dispatched = 0;

	BUG_ON(RB_EMPTY_ROOT(&cfqq->sort_list));

	do {
		struct cfq_rq *crq;

		/*
		 * follow expired path, else get first next available
		 */
		/*如果FIFO队列中存在超时的request，则从FIFO队列中获取请求分发到派发队列中，如果没有，把next_crq(从红黑树中取出)分发到派发队列*/
		if ((crq = cfq_check_fifo(cfqq)) == NULL)
			crq = cfqq->next_crq; /* next_crq从红黑树中获取 */

		/*
		 * finally, insert request into driver dispatch list
		 */
		/* 把request分发到派发队列中 */
		cfq_dispatch_insert(cfqd->queue, crq);

		cfqd->dispatch_slice++;
		dispatched++;

		if (!cfqd->active_cic) {
			atomic_inc(&crq->io_context->ioc->refcount);
			cfqd->active_cic = crq->io_context;
		}
		/* 排序队列已经为空 */
		if (RB_EMPTY_ROOT(&cfqq->sort_list))
			break;

	} while (dispatched < max_dispatch);

	/*
	 * if slice end isn't set yet, set it.
	 */
	if (!cfqq->slice_end)
		cfq_set_prio_slice(cfqd, cfqq);

	/*
	 * expire an async queue immediately if it has used up its slice. idle
	 * queue always expire after 1 dispatch round.
	 */
	if ((!cfq_cfqq_sync(cfqq) &&
	    cfqd->dispatch_slice >= cfq_prio_to_maxrq(cfqd, cfqq)) ||
	    cfq_class_idle(cfqq) ||
	    !cfq_cfqq_idle_window(cfqq))
		cfq_slice_expired(cfqd, 0);

	return dispatched;
}
/**ltl
 * 功能:强制分发list队列中的请求
 * 参数:
 * 返回值:
 */
static int
cfq_forced_dispatch_cfqqs(struct list_head *list)
{
	struct cfq_queue *cfqq, *next;
	struct cfq_rq *crq;
	int dispatched;

	dispatched = 0;
	list_for_each_entry_safe(cfqq, next, list, cfq_list) {
		while ((crq = cfqq->next_crq)) {
			cfq_dispatch_insert(cfqq->cfqd->queue, crq);
			dispatched++;
		}
		BUG_ON(!list_empty(&cfqq->fifo));
	}

	return dispatched;
}
/**ltl
 * 功能:
 * 参数:
 * 返回值:
 */
static int
cfq_forced_dispatch(struct cfq_data *cfqd)
{
	int i, dispatched = 0;

	for (i = 0; i < CFQ_PRIO_LISTS; i++)
		dispatched += cfq_forced_dispatch_cfqqs(&cfqd->rr_list[i]);
	/* 强制分配busy队列中的请求 */
	dispatched += cfq_forced_dispatch_cfqqs(&cfqd->busy_rr);
	/* 强制分发cur队列中的请求 */
	dispatched += cfq_forced_dispatch_cfqqs(&cfqd->cur_rr);
	/* 强制分发idle队列中的请求 */
	dispatched += cfq_forced_dispatch_cfqqs(&cfqd->idle_rr);

	cfq_slice_expired(cfqd, 0);

	BUG_ON(cfqd->busy_queues);

	return dispatched;
}
/**ltl
功能:
参数:
返回值:
*/
static int
cfq_dispatch_requests(request_queue_t *q, int force)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_queue *cfqq, *prev_cfqq;
	int dispatched;

	if (!cfqd->busy_queues)
		return 0;

	if (unlikely(force))/* 强制分发到派发队列 */
		return cfq_forced_dispatch(cfqd);

	dispatched = 0;
	prev_cfqq = NULL;
	
	/* 选取cfqq队列，把此队列中的请求转移到派发队列中。注:每次要把cfq_data中的所有的cfq队列都分发到派发队列中 */
	while ((cfqq = cfq_select_queue(cfqd)) != NULL) {
		int max_dispatch;

		/*
		 * Don't repeat dispatch from the previous queue.
		 */
		if (prev_cfqq == cfqq)
			break;

		cfq_clear_cfqq_must_dispatch(cfqq);
		cfq_clear_cfqq_wait_request(cfqq);
		del_timer(&cfqd->idle_slice_timer);
		
		/* 一次最多只能把cfq_queue中的4个请求转移到派发队列 */
		max_dispatch = cfqd->cfq_quantum;/*4*/
		if (cfq_class_idle(cfqq))/* 如果active_queue是idle_rr,则此次最多分发1个 */
			max_dispatch = 1;
		
		dispatched += __cfq_dispatch_requests(cfqd, cfqq, max_dispatch);

		/*
		 * If the dispatch cfqq has idling enabled and is still
		 * the active queue, break out.
		 */
		if (cfq_cfqq_idle_window(cfqq) && cfqd->active_queue)
			break;
		/* 记录上一次的cfq_queue对象 */
		prev_cfqq = cfqq;
	}

	return dispatched;
}

/*
 * task holds one reference to the queue, dropped when task exits. each crq
 * in-flight on this queue also holds a reference, dropped when crq is freed.
 *
 * queue lock must be held here.
 */
static void cfq_put_queue(struct cfq_queue *cfqq)
{
	struct cfq_data *cfqd = cfqq->cfqd;

	BUG_ON(atomic_read(&cfqq->ref) <= 0);

	if (!atomic_dec_and_test(&cfqq->ref))
		return;

	BUG_ON(rb_first(&cfqq->sort_list));
	BUG_ON(cfqq->allocated[READ] + cfqq->allocated[WRITE]);
	BUG_ON(cfq_cfqq_on_rr(cfqq));

	if (unlikely(cfqd->active_queue == cfqq))
		__cfq_slice_expired(cfqd, cfqq, 0);

	/*
	 * it's on the empty list and still hashed
	 */
	list_del(&cfqq->cfq_list);
	hlist_del(&cfqq->cfq_hash);
	kmem_cache_free(cfq_pool, cfqq);
}
/**ltl
 * 功能: 根据进程pid(key) 和 优先级查找对应的cfq_queue对象
 * 参数:
 * 返回值:
 * 说明:
 */
static inline struct cfq_queue *
__cfq_find_cfq_hash(struct cfq_data *cfqd, unsigned int key, unsigned int prio,
		    const int hashval)
{
	struct hlist_head *hash_list = &cfqd->cfq_hash[hashval];
	struct hlist_node *entry;
	struct cfq_queue *__cfqq;

	hlist_for_each_entry(__cfqq, entry, hash_list, cfq_hash) {
		const unsigned short __p = IOPRIO_PRIO_VALUE(__cfqq->org_ioprio_class, __cfqq->org_ioprio);
		/* 如果优先级为0或者优先级相同，相同进程，此cfq就是所查找的cfq_queue */
		if (__cfqq->key == key && (__p == prio || !prio))
			return __cfqq;
	}

	return NULL;
}
/**ltl
 * 功能: 根据进程pid(key) 和 优先级查找对应的cfq_queue对象
 * 参数:
 * 返回值:
 * 说明:
 */
static struct cfq_queue *
cfq_find_cfq_hash(struct cfq_data *cfqd, unsigned int key, unsigned short prio)
{
	return __cfq_find_cfq_hash(cfqd, key, prio, hash_long(key, CFQ_QHASH_SHIFT));
}

static void cfq_free_io_context(struct io_context *ioc)
{
	struct cfq_io_context *__cic;
	struct rb_node *n;
	int freed = 0;

	while ((n = rb_first(&ioc->cic_root)) != NULL) {
		__cic = rb_entry(n, struct cfq_io_context, rb_node);
		rb_erase(&__cic->rb_node, &ioc->cic_root);
		kmem_cache_free(cfq_ioc_pool, __cic);
		freed++;
	}

	if (atomic_sub_and_test(freed, &ioc_count) && ioc_gone)
		complete(ioc_gone);
}

static void cfq_trim(struct io_context *ioc)
{
	ioc->set_ioprio = NULL;
	cfq_free_io_context(ioc);
}

/*
 * Called with interrupts disabled
 */
static void cfq_exit_single_io_context(struct cfq_io_context *cic)
{
	struct cfq_data *cfqd = cic->key;
	request_queue_t *q;

	if (!cfqd)
		return;

	q = cfqd->queue;

	WARN_ON(!irqs_disabled());

	spin_lock(q->queue_lock);

	if (cic->cfqq[ASYNC]) {
		if (unlikely(cic->cfqq[ASYNC] == cfqd->active_queue))
			__cfq_slice_expired(cfqd, cic->cfqq[ASYNC], 0);
		cfq_put_queue(cic->cfqq[ASYNC]);
		cic->cfqq[ASYNC] = NULL;
	}

	if (cic->cfqq[SYNC]) {
		if (unlikely(cic->cfqq[SYNC] == cfqd->active_queue))
			__cfq_slice_expired(cfqd, cic->cfqq[SYNC], 0);
		cfq_put_queue(cic->cfqq[SYNC]);
		cic->cfqq[SYNC] = NULL;
	}

	cic->key = NULL;
	list_del_init(&cic->queue_list);
	spin_unlock(q->queue_lock);
}

static void cfq_exit_io_context(struct io_context *ioc)
{
	struct cfq_io_context *__cic;
	unsigned long flags;
	struct rb_node *n;

	/*
	 * put the reference this task is holding to the various queues
	 */
	spin_lock_irqsave(&cfq_exit_lock, flags);

	n = rb_first(&ioc->cic_root);
	while (n != NULL) {
		__cic = rb_entry(n, struct cfq_io_context, rb_node);

		cfq_exit_single_io_context(__cic);
		n = rb_next(n);
	}

	spin_unlock_irqrestore(&cfq_exit_lock, flags);
}

static struct cfq_io_context *
cfq_alloc_io_context(struct cfq_data *cfqd, gfp_t gfp_mask)
{
	struct cfq_io_context *cic = kmem_cache_alloc(cfq_ioc_pool, gfp_mask);

	if (cic) {
		memset(cic, 0, sizeof(*cic));
		cic->last_end_request = jiffies;
		INIT_LIST_HEAD(&cic->queue_list);
		cic->dtor = cfq_free_io_context;
		cic->exit = cfq_exit_io_context;
		atomic_inc(&ioc_count);
	}

	return cic;
}
/**ltl
 * 功能:设置cfq_queue的优先级别
 * 参数:
 * 返回值:
 * 说明:
 */
static void cfq_init_prio_data(struct cfq_queue *cfqq)
{
	struct task_struct *tsk = current;
	int ioprio_class;

	if (!cfq_cfqq_prio_changed(cfqq))
		return;

	ioprio_class = IOPRIO_PRIO_CLASS(tsk->ioprio);
	switch (ioprio_class) {
		default:
			printk(KERN_ERR "cfq: bad prio %x\n", ioprio_class);
		case IOPRIO_CLASS_NONE:
			/*
			 * no prio set, place us in the middle of the BE classes
			 */
			cfqq->ioprio = task_nice_ioprio(tsk);
			cfqq->ioprio_class = IOPRIO_CLASS_BE;
			break;
		case IOPRIO_CLASS_RT:/*实时*/
			cfqq->ioprio = task_ioprio(tsk);
			cfqq->ioprio_class = IOPRIO_CLASS_RT;
			break;
		case IOPRIO_CLASS_BE:/* best-effect */
			cfqq->ioprio = task_ioprio(tsk);
			cfqq->ioprio_class = IOPRIO_CLASS_BE;
			break;
		case IOPRIO_CLASS_IDLE:/* 空闲 */
			cfqq->ioprio_class = IOPRIO_CLASS_IDLE;
			cfqq->ioprio = 7;
			cfq_clear_cfqq_idle_window(cfqq);
			break;
	}

	/*
	 * keep track of original prio settings in case we have to temporarily
	 * elevate the priority of this queue
	 */
	cfqq->org_ioprio = cfqq->ioprio;
	cfqq->org_ioprio_class = cfqq->ioprio_class;

	if (cfq_cfqq_on_rr(cfqq))
		cfq_resort_rr_list(cfqq, 0); /* 对cfqq重新排序 */

	cfq_clear_cfqq_prio_changed(cfqq);
}

static inline void changed_ioprio(struct cfq_io_context *cic)
{
	struct cfq_data *cfqd = cic->key;
	struct cfq_queue *cfqq;

	if (unlikely(!cfqd))
		return;

	spin_lock(cfqd->queue->queue_lock);

	cfqq = cic->cfqq[ASYNC];
	if (cfqq) {
		struct cfq_queue *new_cfqq;
		new_cfqq = cfq_get_queue(cfqd, CFQ_KEY_ASYNC, cic->ioc->task,
					 GFP_ATOMIC);
		if (new_cfqq) {
			cic->cfqq[ASYNC] = new_cfqq;
			cfq_put_queue(cfqq);
		}
	}

	cfqq = cic->cfqq[SYNC];
	if (cfqq)
		cfq_mark_cfqq_prio_changed(cfqq);

	spin_unlock(cfqd->queue->queue_lock);
}

/*
 * callback from sys_ioprio_set, irqs are disabled
 */
static int cfq_ioc_set_ioprio(struct io_context *ioc, unsigned int ioprio)
{
	struct cfq_io_context *cic;
	struct rb_node *n;

	spin_lock(&cfq_exit_lock);

	n = rb_first(&ioc->cic_root);
	while (n != NULL) {
		cic = rb_entry(n, struct cfq_io_context, rb_node);

		changed_ioprio(cic);
		n = rb_next(n);
	}

	spin_unlock(&cfq_exit_lock);

	return 0;
}
/**ltl
 * 功能:分配cfg_queue空间
 * 参数: cfqd	->cfq_data对象
 *		key	->进程PID
 *		task	->当前状态
 *		gfp_mask->
 * 返回值:
 */
static struct cfq_queue *
cfq_get_queue(struct cfq_data *cfqd, unsigned int key, struct task_struct *tsk,
	      gfp_t gfp_mask)
{
	const int hashval = hash_long(key, CFQ_QHASH_SHIFT);
	struct cfq_queue *cfqq, *new_cfqq = NULL;
	unsigned short ioprio;

retry:
	ioprio = tsk->ioprio;/* 进程的IO优先级别 */
	cfqq = __cfq_find_cfq_hash(cfqd, key, ioprio, hashval); /* 在cfq_data:cfq_hash[pid] Hash列表中查找 */

	if (!cfqq) {
		if (new_cfqq) {
			cfqq = new_cfqq;
			new_cfqq = NULL;
		} else if (gfp_mask & __GFP_WAIT) {
			spin_unlock_irq(cfqd->queue->queue_lock);
			/* 分配CFQ对象 */
			new_cfqq = kmem_cache_alloc(cfq_pool, gfp_mask);
			spin_lock_irq(cfqd->queue->queue_lock);
			goto retry;
		} else {
			
			cfqq = kmem_cache_alloc(cfq_pool, gfp_mask);
			if (!cfqq)
				goto out;
		}

		memset(cfqq, 0, sizeof(*cfqq));

		INIT_HLIST_NODE(&cfqq->cfq_hash);
		INIT_LIST_HEAD(&cfqq->cfq_list);
		INIT_LIST_HEAD(&cfqq->fifo);

		cfqq->key = key;/* 进程PID */
		hlist_add_head(&cfqq->cfq_hash, &cfqd->cfq_hash[hashval]);/* 将cfq_queue链接到cfq_data */
		atomic_set(&cfqq->ref, 0);
		cfqq->cfqd = cfqd;
		cfqq->service_last = 0;
		/*
		 * set ->slice_left to allow preemption for a new process
		 */
		cfqq->slice_left = 2 * cfqd->cfq_slice_idle; /* cfq队列时间片 */
		cfq_mark_cfqq_idle_window(cfqq);
		cfq_mark_cfqq_prio_changed(cfqq);
		cfq_init_prio_data(cfqq);	/* 设置cfq队列IO优先级别 */
	}

	if (new_cfqq)
		kmem_cache_free(cfq_pool, new_cfqq);

	atomic_inc(&cfqq->ref);
out:
	WARN_ON((gfp_mask & __GFP_WAIT) && !cfqq);
	return cfqq;
}

static void
cfq_drop_dead_cic(struct io_context *ioc, struct cfq_io_context *cic)
{
	spin_lock(&cfq_exit_lock);
	rb_erase(&cic->rb_node, &ioc->cic_root);
	list_del_init(&cic->queue_list);
	spin_unlock(&cfq_exit_lock);
	kmem_cache_free(cfq_ioc_pool, cic);
	atomic_dec(&ioc_count);
}
/**ltl
 * 功能: 查找与cfqqd相对应的cfq_io_context对象
 * 参数:
 * 返回值:
 * 说明:
 */
static struct cfq_io_context *
cfq_cic_rb_lookup(struct cfq_data *cfqd, struct io_context *ioc)
{
	struct rb_node *n;
	struct cfq_io_context *cic;
	void *k, *key = cfqd;

restart:
	n = ioc->cic_root.rb_node;
	while (n) {
		cic = rb_entry(n, struct cfq_io_context, rb_node);
		/* ->key must be copied to avoid race with cfq_exit_queue() */
		k = cic->key;
		if (unlikely(!k)) {
			cfq_drop_dead_cic(ioc, cic);
			goto restart;
		}

		if (key < k)
			n = n->rb_left;
		else if (key > k)
			n = n->rb_right;
		else
			return cic;
	}

	return NULL;
}

static inline void
cfq_cic_link(struct cfq_data *cfqd, struct io_context *ioc,
	     struct cfq_io_context *cic)
{
	struct rb_node **p;
	struct rb_node *parent;
	struct cfq_io_context *__cic;
	void *k;

	cic->ioc = ioc;
	cic->key = cfqd;/* cfg_io_context的key值---cfg_data对象 */

	ioc->set_ioprio = cfq_ioc_set_ioprio;
restart:
	parent = NULL;
	p = &ioc->cic_root.rb_node;
	while (*p) {/* 寻找cic的父节点parent */
		parent = *p;
		__cic = rb_entry(parent, struct cfq_io_context, rb_node);
		/* ->key must be copied to avoid race with cfq_exit_queue() */
		k = __cic->key;
		if (unlikely(!k)) {
			cfq_drop_dead_cic(ioc, __cic);
			goto restart;
		}

		if (cic->key < k)
			p = &(*p)->rb_left;
		else if (cic->key > k)
			p = &(*p)->rb_right;
		else
			BUG();
	}

	spin_lock(&cfq_exit_lock);
	rb_link_node(&cic->rb_node, parent, p);/* 设置cic的父节点 */
	rb_insert_color(&cic->rb_node, &ioc->cic_root);/*把cfg_io_context对象插入到进程上下文中,红黑树节点个数表示进程访问的磁盘个数 */
	list_add(&cic->queue_list, &cfqd->cic_list); /* 将cfq_io_context对象插入到cfq_data对象中，列表中对象的个数表示正在访问此磁盘的进程数 */
	spin_unlock(&cfq_exit_lock);
}

/*
 * Setup general io context and cfq io context. There can be several cfq
 * io contexts per general io context, if this process is doing io to more
 * than one device managed by cfq.
 */
static struct cfq_io_context *
cfq_get_io_context(struct cfq_data *cfqd, gfp_t gfp_mask)
{
	struct io_context *ioc = NULL;
	struct cfq_io_context *cic;

	might_sleep_if(gfp_mask & __GFP_WAIT);
	/* 获取与当前进程相关联的io_context对象 */
	ioc = get_io_context(gfp_mask);
	if (!ioc)
		return NULL;
	/* 根据io_context对象，从红黑树中查找cfg_io_context对象 */
	cic = cfq_cic_rb_lookup(cfqd, ioc);
	if (cic)
		goto out;
	/* 分配cfg_io_context对象 */
	cic = cfq_alloc_io_context(cfqd, gfp_mask);
	if (cic == NULL)
		goto err;
	/* 把cic插入到进程上下文(io_context)中 */
	cfq_cic_link(cfqd, ioc, cic);
out:
	return cic;
err:
	put_io_context(ioc);
	return NULL;
}

static void
cfq_update_io_thinktime(struct cfq_data *cfqd, struct cfq_io_context *cic)
{
	unsigned long elapsed, ttime;

	/*
	 * if this context already has stuff queued, thinktime is from
	 * last queue not last end
	 */
#if 0
	if (time_after(cic->last_end_request, cic->last_queue))
		elapsed = jiffies - cic->last_end_request;
	else
		elapsed = jiffies - cic->last_queue;
#else
		elapsed = jiffies - cic->last_end_request;
#endif

	ttime = min(elapsed, 2UL * cfqd->cfq_slice_idle);

	cic->ttime_samples = (7*cic->ttime_samples + 256) / 8;
	cic->ttime_total = (7*cic->ttime_total + 256*ttime) / 8;
	cic->ttime_mean = (cic->ttime_total + 128) / cic->ttime_samples;
}

static void
cfq_update_io_seektime(struct cfq_data *cfqd, struct cfq_io_context *cic,
		       struct cfq_rq *crq)
{
	sector_t sdist;
	u64 total;

	if (cic->last_request_pos < crq->request->sector)
		sdist = crq->request->sector - cic->last_request_pos;
	else
		sdist = cic->last_request_pos - crq->request->sector;

	/*
	 * Don't allow the seek distance to get too large from the
	 * odd fragment, pagein, etc
	 */
	if (cic->seek_samples <= 60) /* second&third seek */
		sdist = min(sdist, (cic->seek_mean * 4) + 2*1024*1024);
	else
		sdist = min(sdist, (cic->seek_mean * 4)	+ 2*1024*64);

	cic->seek_samples = (7*cic->seek_samples + 256) / 8;
	cic->seek_total = (7*cic->seek_total + (u64)256*sdist) / 8;
	total = cic->seek_total + (cic->seek_samples/2);
	do_div(total, cic->seek_samples);
	cic->seek_mean = (sector_t)total;
}

/*
 * Disable idle window if the process thinks too long or seeks so much that
 * it doesn't matter
 */
static void
cfq_update_idle_window(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		       struct cfq_io_context *cic)
{
	int enable_idle = cfq_cfqq_idle_window(cfqq);

	if (!cic->ioc->task || !cfqd->cfq_slice_idle ||
	    (cfqd->hw_tag && CIC_SEEKY(cic)))
		enable_idle = 0;
	else if (sample_valid(cic->ttime_samples)) {
		if (cic->ttime_mean > cfqd->cfq_slice_idle)
			enable_idle = 0;
		else
			enable_idle = 1;
	}

	if (enable_idle)
		cfq_mark_cfqq_idle_window(cfqq);
	else
		cfq_clear_cfqq_idle_window(cfqq);
}


/*
 * Check if new_cfqq should preempt the currently active queue. Return 0 for
 * no or if we aren't sure, a 1 will cause a preempt.
 */
/**ltl
 * 功能:判定new_cfqq是否可以抢占
 * 参数:
 * 返回值:
 * 说明:
 */
static int
cfq_should_preempt(struct cfq_data *cfqd, struct cfq_queue *new_cfqq,
		   struct cfq_rq *crq)
{
	struct cfq_queue *cfqq = cfqd->active_queue;
	/* 新的cfq_queue是Idle级别，则不能抢占其它cfq_queue */
	if (cfq_class_idle(new_cfqq))
		return 0;

	if (!cfqq)
		return 0;
	/* 原先的cfq_queue就idle级别，可以允许新的去抢占 */
	if (cfq_class_idle(cfqq))
		return 1;
	if (!cfq_cfqq_wait_request(new_cfqq))
		return 0;
	/*
	 * if it doesn't have slice left, forget it
	 */
	if (new_cfqq->slice_left < cfqd->cfq_slice_idle)
		return 0;
	if (cfq_crq_is_sync(crq) && !cfq_cfqq_sync(cfqq)) /* crq是一个同步操作，而active_queue是一个异步列表，因此可以抢占 */
		return 1;

	return 0;
}

/*
 * cfqq preempts the active queue. if we allowed preempt with no slice left,
 * let it have half of its nominal slice.
 */
/**ltl
 * 功能:执行抢占
 * 参数:
 * 返回值:
 * 说明:cfqq抢占当前活动的cfq_queue(即:cfqd->active_queue)
 */
static void cfq_preempt_queue(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	struct cfq_queue *__cfqq, *next;
	/* 把cur_rr队列中的cfq_queue对象移回对应优先级的队列(rr_list)中 */
	list_for_each_entry_safe(__cfqq, next, &cfqd->cur_rr, cfq_list)
		cfq_resort_rr_list(__cfqq, 1);
	/* 初始设置一个时间片(Q:为什么要这语句? 在__cfq_set_active_queue函数里对这此成员置0) */
	if (!cfqq->slice_left)
		cfqq->slice_left = cfq_prio_to_slice(cfqd, cfqq) / 2;
	cfqq->slice_end = cfqq->slice_left + jiffies;
	
	cfq_slice_expired(cfqd, 1); /* 被抢占的cfqq要以抢占方式插入到相应等级的cfq_data的队列中(service_tree) */
	/* 把cfqq设置成当前活动的cfq队列 */
	__cfq_set_active_queue(cfqd, cfqq);
}

/*
 * should really be a ll_rw_blk.c helper
 */
/**ltl
功能:开始"泄流"
*/
static void cfq_start_queueing(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	request_queue_t *q = cfqd->queue;

	if (!blk_queue_plugged(q))
		q->request_fn(q);
	else
		__generic_unplug_device(q);
}

/*
 * Called when a new fs request (crq) is added (to cfqq). Check if there's
 * something we should do about it
 */
static void
cfq_crq_enqueued(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		 struct cfq_rq *crq)
{
	struct cfq_io_context *cic = crq->io_context;

	/*
	 * we never wait for an async request and we don't allow preemption
	 * of an async request. so just return early
	 */
	/* 非同步请求 */
	if (!cfq_crq_is_sync(crq)) {
		/*
		 * sync process issued an async request, if it's waiting
		 * then expire it and kick rq handling.
		 */
		if (cic == cfqd->active_cic &&
		    del_timer(&cfqd->idle_slice_timer)) {
			cfq_slice_expired(cfqd, 0);
			cfq_start_queueing(cfqd, cfqq);
		}
		return;
	}

	cfq_update_io_thinktime(cfqd, cic);
	cfq_update_io_seektime(cfqd, cic, crq);
	cfq_update_idle_window(cfqd, cfqq, cic);
	/* 最后的请求时间 */
	cic->last_queue = jiffies;
	/* 最后的请求磁盘扇区 */
	cic->last_request_pos = crq->request->sector + crq->request->nr_sectors;
	/* 当前的队列就是一个活动CFQ队列 */
	if (cfqq == cfqd->active_queue) {
		/*
		 * if we are waiting for a request for this queue, let it rip
		 * immediately and flag that we must not expire this queue
		 * just now
		 */
		if (cfq_cfqq_wait_request(cfqq)) {
			cfq_mark_cfqq_must_dispatch(cfqq);
			del_timer(&cfqd->idle_slice_timer);
			cfq_start_queueing(cfqd, cfqq);
		}
	} else if (cfq_should_preempt(cfqd, cfqq, crq)) { /* 是否允许cfqq抢占当前活动的CFQ队列 */
		/*
		 * not the active queue - expire current slice if it is
		 * idle and has expired it's mean thinktime or this new queue
		 * has some old slice time left and is of higher priority
		 */
		cfq_preempt_queue(cfqd, cfqq); /* 执行抢占 */
		cfq_mark_cfqq_must_dispatch(cfqq);
		cfq_start_queueing(cfqd, cfqq); /* 执行策略处理 */
	}
}

/**ltl
 * 功能
 * 参数
 * 返回值
 * 说明
 */
static void cfq_insert_request(request_queue_t *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_rq *crq = RQ_DATA(rq);
	struct cfq_queue *cfqq = crq->cfq_queue;
	/* 初始化cfq_queue优先级别信息 */
	cfq_init_prio_data(cfqq);
	/* 1.插入到排序树中，同时重排cfqq在cfq_queue:sort_list红黑树中 */
	cfq_add_crq_rb(crq);
	/* 2.插入到cfq_queue:FIFO中 */
	list_add_tail(&rq->queuelist, &cfqq->fifo);
	/* 3.插入到cfq_data的Hash表示中(此Hash只为了找到后插的位置用到) */
	if (rq_mergeable(rq))
		cfq_add_crq_hash(cfqd, crq);
	/* 4.在插入新的请求后，要去检查是否有请求已经超时而要马上去处理 */
	cfq_crq_enqueued(cfqd, cfqq, crq);
}
/**ltl
 * 功能:请求执行完成后的处理函数
 * 参数:
 * 返回值:
 * 说明:
 */
static void cfq_completed_request(request_queue_t *q, struct request *rq)
{
	struct cfq_rq *crq = RQ_DATA(rq);
	struct cfq_queue *cfqq = crq->cfq_queue;
	struct cfq_data *cfqd = cfqq->cfqd;
	const int sync = cfq_crq_is_sync(crq);
	unsigned long now;

	now = jiffies;

	WARN_ON(!cfqd->rq_in_driver);
	WARN_ON(!cfqq->on_dispatch[sync]);
	cfqd->rq_in_driver--;
	cfqq->on_dispatch[sync]--;

	if (!cfq_class_idle(cfqq))
		cfqd->last_end_request = now; /* 记录一个请求处理的最后时刻 */

	if (!cfq_cfqq_dispatched(cfqq)) {
		if (cfq_cfqq_on_rr(cfqq)) {
			cfqq->service_last = now;
			cfq_resort_rr_list(cfqq, 0);
		}
	}

	if (sync)
		crq->io_context->last_end_request = now;

	/*
	 * If this is the active queue, check if it needs to be expired,
	 * or if we want to idle in case it has no pending requests.
	 */
	if (cfqd->active_queue == cfqq) {
		if (time_after(now, cfqq->slice_end))
			cfq_slice_expired(cfqd, 0);
		else if (sync && RB_EMPTY_ROOT(&cfqq->sort_list)) {
			if (!cfq_arm_slice_timer(cfqd, cfqq))
				cfq_schedule_dispatch(cfqd);
		}
	}
}

static struct request *
cfq_former_request(request_queue_t *q, struct request *rq)
{
	struct cfq_rq *crq = RQ_DATA(rq);
	struct rb_node *rbprev = rb_prev(&crq->rb_node);

	if (rbprev)
		return rb_entry_crq(rbprev)->request;

	return NULL;
}

static struct request *
cfq_latter_request(request_queue_t *q, struct request *rq)
{
	struct cfq_rq *crq = RQ_DATA(rq);
	struct rb_node *rbnext = rb_next(&crq->rb_node);

	if (rbnext)
		return rb_entry_crq(rbnext)->request;

	return NULL;
}

/*
 * we temporarily boost lower priority queues if they are holding fs exclusive
 * resources. they are boosted to normal prio (CLASS_BE/4)
 */
static void cfq_prio_boost(struct cfq_queue *cfqq)
{
	const int ioprio_class = cfqq->ioprio_class;
	const int ioprio = cfqq->ioprio;

	if (has_fs_excl()) {
		/*
		 * boost idle prio on transactions that would lock out other
		 * users of the filesystem
		 */
		if (cfq_class_idle(cfqq))
			cfqq->ioprio_class = IOPRIO_CLASS_BE;
		if (cfqq->ioprio > IOPRIO_NORM)
			cfqq->ioprio = IOPRIO_NORM;
	} else {
		/*
		 * check if we need to unboost the queue
		 */
		if (cfqq->ioprio_class != cfqq->org_ioprio_class)
			cfqq->ioprio_class = cfqq->org_ioprio_class;
		if (cfqq->ioprio != cfqq->org_ioprio)
			cfqq->ioprio = cfqq->org_ioprio;
	}

	/*
	 * refile between round-robin lists if we moved the priority class
	 */
	if ((ioprio_class != cfqq->ioprio_class || ioprio != cfqq->ioprio) &&
	    cfq_cfqq_on_rr(cfqq))
		cfq_resort_rr_list(cfqq, 0);
}

static inline int
__cfq_may_queue(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		struct task_struct *task, int rw)
{
	if ((cfq_cfqq_wait_request(cfqq) || cfq_cfqq_must_alloc(cfqq)) &&
	    !cfq_cfqq_must_alloc_slice(cfqq)) {
		cfq_mark_cfqq_must_alloc_slice(cfqq);
		return ELV_MQUEUE_MUST;
	}

	return ELV_MQUEUE_MAY;
}

static int cfq_may_queue(request_queue_t *q, int rw, struct bio *bio)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct task_struct *tsk = current;
	struct cfq_queue *cfqq;

	/*
	 * don't force setup of a queue from here, as a call to may_queue
	 * does not necessarily imply that a request actually will be queued.
	 * so just lookup a possibly existing queue, or return 'may queue'
	 * if that fails
	 */
	cfqq = cfq_find_cfq_hash(cfqd, cfq_queue_pid(tsk, rw), tsk->ioprio);
	if (cfqq) {
		cfq_init_prio_data(cfqq);
		cfq_prio_boost(cfqq);

		return __cfq_may_queue(cfqd, cfqq, tsk, rw);
	}

	return ELV_MQUEUE_MAY;
}

static void cfq_check_waiters(request_queue_t *q, struct cfq_queue *cfqq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;

	if (unlikely(cfqd->rq_starved)) {
		struct request_list *rl = &q->rq;

		smp_mb();
		if (waitqueue_active(&rl->wait[READ]))
			wake_up(&rl->wait[READ]);
		if (waitqueue_active(&rl->wait[WRITE]))
			wake_up(&rl->wait[WRITE]);
	}
}

/*
 * queue lock held here
 */
static void cfq_put_request(request_queue_t *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_rq *crq = RQ_DATA(rq);

	if (crq) {
		struct cfq_queue *cfqq = crq->cfq_queue;
		const int rw = rq_data_dir(rq);

		BUG_ON(!cfqq->allocated[rw]);
		cfqq->allocated[rw]--;

		put_io_context(crq->io_context->ioc);

		mempool_free(crq, cfqd->crq_pool);
		rq->elevator_private = NULL;

		cfq_check_waiters(q, cfqq);
		cfq_put_queue(cfqq);
	}
}

/*
 * Allocate cfq data structures associated with this request.
 */
/**ltl
 * 功能: 为请求分配与CFQ算法相关的私有数据成员
 * 参数:
 * 返回值:
 * 说明:
 */
static int
cfq_set_request(request_queue_t *q, struct request *rq, struct bio *bio,
		gfp_t gfp_mask)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct task_struct *tsk = current;
	struct cfq_io_context *cic;
	const int rw = rq_data_dir(rq);
	pid_t key = cfq_queue_pid(tsk, rw);/* 获取进程的PID */
	struct cfq_queue *cfqq;
	struct cfq_rq *crq;
	unsigned long flags;
	int is_sync = key != CFQ_KEY_ASYNC;/* 是否是同步 */

	might_sleep_if(gfp_mask & __GFP_WAIT);
	/* 申请cfg_io_context对象，并插入到进程上下文中io_context */
	cic = cfq_get_io_context(cfqd, gfp_mask);

	spin_lock_irqsave(q->queue_lock, flags);

	if (!cic)
		goto queue_fail;

	if (!cic->cfqq[is_sync]) {/* 分配cfg_queue */
		cfqq = cfq_get_queue(cfqd, key, tsk, gfp_mask);
		if (!cfqq)
			goto queue_fail;

		cic->cfqq[is_sync] = cfqq;
	} else
		cfqq = cic->cfqq[is_sync];
	/* 递增分配计数器 */
	cfqq->allocated[rw]++;
	cfq_clear_cfqq_must_alloc(cfqq);
	cfqd->rq_starved = 0;
	atomic_inc(&cfqq->ref);
	spin_unlock_irqrestore(q->queue_lock, flags);
	/* 分配cfg_rq,并与request关联,并对cfg_rq中的部分成员变量初始化(注:此时cfq_rq对象并没有插入到cfq_queue对象列表中) */
	crq = mempool_alloc(cfqd->crq_pool, gfp_mask);
	if (crq) {
		RB_CLEAR_NODE(&crq->rb_node);
		crq->rb_key = 0;
		crq->request = rq;
		INIT_HLIST_NODE(&crq->hash);
		crq->cfq_queue = cfqq;
		crq->io_context = cic;

		if (is_sync)
			cfq_mark_crq_is_sync(crq);
		else
			cfq_clear_crq_is_sync(crq);
		/* 设置请求的私有数据 */
		rq->elevator_private = crq;
		return 0;
	}

	spin_lock_irqsave(q->queue_lock, flags);
	cfqq->allocated[rw]--;
	if (!(cfqq->allocated[0] + cfqq->allocated[1]))
		cfq_mark_cfqq_must_alloc(cfqq);
	cfq_put_queue(cfqq);
queue_fail:
	if (cic)
		put_io_context(cic->ioc);
	/*
	 * mark us rq allocation starved. we need to kickstart the process
	 * ourselves if there are no pending requests that can do it for us.
	 * that would be an extremely rare OOM situation
	 */
	cfqd->rq_starved = 1;
	cfq_schedule_dispatch(cfqd);
	spin_unlock_irqrestore(q->queue_lock, flags);
	return 1;
}

static void cfq_kick_queue(void *data)
{
	request_queue_t *q = data;
	struct cfq_data *cfqd = q->elevator->elevator_data;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);

	if (cfqd->rq_starved) {
		struct request_list *rl = &q->rq;

		/*
		 * we aren't guaranteed to get a request after this, but we
		 * have to be opportunistic
		 */
		smp_mb();
		if (waitqueue_active(&rl->wait[READ]))
			wake_up(&rl->wait[READ]);
		if (waitqueue_active(&rl->wait[WRITE]))
			wake_up(&rl->wait[WRITE]);
	}

	blk_remove_plug(q);
	q->request_fn(q);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

/*
 * Timer running if the active_queue is currently idling inside its time slice
 */
static void cfq_idle_slice_timer(unsigned long data)
{
	struct cfq_data *cfqd = (struct cfq_data *) data;
	struct cfq_queue *cfqq;
	unsigned long flags;

	spin_lock_irqsave(cfqd->queue->queue_lock, flags);

	if ((cfqq = cfqd->active_queue) != NULL) {
		unsigned long now = jiffies;

		/*
		 * expired
		 */
		if (time_after(now, cfqq->slice_end))
			goto expire;

		/*
		 * only expire and reinvoke request handler, if there are
		 * other queues with pending requests
		 */
		if (!cfqd->busy_queues) /* 调度队列中没有请求 */
			goto out_cont;

		/*
		 * not expired and it has a request pending, let it dispatch
		 */
		if (!RB_EMPTY_ROOT(&cfqq->sort_list)) {
			cfq_mark_cfqq_must_dispatch(cfqq);
			goto out_kick;
		}
	}
expire:
	cfq_slice_expired(cfqd, 0);
out_kick:
	cfq_schedule_dispatch(cfqd);
out_cont:
	spin_unlock_irqrestore(cfqd->queue->queue_lock, flags);
}

/*
 * Timer running if an idle class queue is waiting for service
 */
static void cfq_idle_class_timer(unsigned long data)
{
	struct cfq_data *cfqd = (struct cfq_data *) data;
	unsigned long flags, end;

	spin_lock_irqsave(cfqd->queue->queue_lock, flags);

	/*
	 * race with a non-idle queue, reset timer
	 */
	end = cfqd->last_end_request + CFQ_IDLE_GRACE;
	if (!time_after_eq(jiffies, end))
		mod_timer(&cfqd->idle_class_timer, end);
	else
		cfq_schedule_dispatch(cfqd);

	spin_unlock_irqrestore(cfqd->queue->queue_lock, flags);
}

static void cfq_shutdown_timer_wq(struct cfq_data *cfqd)
{
	del_timer_sync(&cfqd->idle_slice_timer);
	del_timer_sync(&cfqd->idle_class_timer);
	blk_sync_queue(cfqd->queue);
}

static void cfq_exit_queue(elevator_t *e)
{
	struct cfq_data *cfqd = e->elevator_data;
	request_queue_t *q = cfqd->queue;

	cfq_shutdown_timer_wq(cfqd);

	spin_lock(&cfq_exit_lock);
	spin_lock_irq(q->queue_lock);

	if (cfqd->active_queue)
		__cfq_slice_expired(cfqd, cfqd->active_queue, 0);

	while (!list_empty(&cfqd->cic_list)) {
		struct cfq_io_context *cic = list_entry(cfqd->cic_list.next,
							struct cfq_io_context,
							queue_list);
		if (cic->cfqq[ASYNC]) {
			cfq_put_queue(cic->cfqq[ASYNC]);
			cic->cfqq[ASYNC] = NULL;
		}
		if (cic->cfqq[SYNC]) {
			cfq_put_queue(cic->cfqq[SYNC]);
			cic->cfqq[SYNC] = NULL;
		}
		cic->key = NULL;
		list_del_init(&cic->queue_list);
	}

	spin_unlock_irq(q->queue_lock);
	spin_unlock(&cfq_exit_lock);

	cfq_shutdown_timer_wq(cfqd);

	mempool_destroy(cfqd->crq_pool);
	kfree(cfqd->crq_hash);
	kfree(cfqd->cfq_hash);
	kfree(cfqd);
}
/**ltl
 * 功能:初始化CFQ调度算法数据结构
 * 参数:
 * 返回值:
 * 说明:
 */
static void *cfq_init_queue(request_queue_t *q, elevator_t *e)
{
	struct cfq_data *cfqd;
	int i;

	cfqd = kmalloc(sizeof(*cfqd), GFP_KERNEL);
	if (!cfqd)
		return NULL;

	memset(cfqd, 0, sizeof(*cfqd));

	for (i = 0; i < CFQ_PRIO_LISTS; i++)
		INIT_LIST_HEAD(&cfqd->rr_list[i]);

	INIT_LIST_HEAD(&cfqd->busy_rr);
	INIT_LIST_HEAD(&cfqd->cur_rr);
	INIT_LIST_HEAD(&cfqd->idle_rr);
	INIT_LIST_HEAD(&cfqd->empty_list);
	INIT_LIST_HEAD(&cfqd->cic_list);

	cfqd->crq_hash = kmalloc(sizeof(struct hlist_head) * CFQ_MHASH_ENTRIES, GFP_KERNEL);
	if (!cfqd->crq_hash)
		goto out_crqhash;

	cfqd->cfq_hash = kmalloc(sizeof(struct hlist_head) * CFQ_QHASH_ENTRIES, GFP_KERNEL);
	if (!cfqd->cfq_hash)
		goto out_cfqhash;

	cfqd->crq_pool = mempool_create_slab_pool(BLKDEV_MIN_RQ, crq_pool);
	if (!cfqd->crq_pool)
		goto out_crqpool;

	for (i = 0; i < CFQ_MHASH_ENTRIES; i++)
		INIT_HLIST_HEAD(&cfqd->crq_hash[i]);
	for (i = 0; i < CFQ_QHASH_ENTRIES; i++)
		INIT_HLIST_HEAD(&cfqd->cfq_hash[i]);

	cfqd->queue = q;

	init_timer(&cfqd->idle_slice_timer);
	cfqd->idle_slice_timer.function = cfq_idle_slice_timer;
	cfqd->idle_slice_timer.data = (unsigned long) cfqd;

	init_timer(&cfqd->idle_class_timer);
	cfqd->idle_class_timer.function = cfq_idle_class_timer;
	cfqd->idle_class_timer.data = (unsigned long) cfqd;

	INIT_WORK(&cfqd->unplug_work, cfq_kick_queue, q);

	cfqd->cfq_queued = cfq_queued;
	cfqd->cfq_quantum = cfq_quantum;
	cfqd->cfq_fifo_expire[0] = cfq_fifo_expire[0];
	cfqd->cfq_fifo_expire[1] = cfq_fifo_expire[1];
	cfqd->cfq_back_max = cfq_back_max;
	cfqd->cfq_back_penalty = cfq_back_penalty;
	cfqd->cfq_slice[0] = cfq_slice_async;
	cfqd->cfq_slice[1] = cfq_slice_sync;
	cfqd->cfq_slice_async_rq = cfq_slice_async_rq;
	cfqd->cfq_slice_idle = cfq_slice_idle;

	return cfqd;
out_crqpool:
	kfree(cfqd->cfq_hash);
out_cfqhash:
	kfree(cfqd->crq_hash);
out_crqhash:
	kfree(cfqd);
	return NULL;
}

static void cfq_slab_kill(void)
{
	if (crq_pool)
		kmem_cache_destroy(crq_pool);
	if (cfq_pool)
		kmem_cache_destroy(cfq_pool);
	if (cfq_ioc_pool)
		kmem_cache_destroy(cfq_ioc_pool);
}

static int __init cfq_slab_setup(void)
{
	crq_pool = kmem_cache_create("crq_pool", sizeof(struct cfq_rq), 0, 0,
					NULL, NULL);
	if (!crq_pool)
		goto fail;

	cfq_pool = kmem_cache_create("cfq_pool", sizeof(struct cfq_queue), 0, 0,
					NULL, NULL);
	if (!cfq_pool)
		goto fail;

	cfq_ioc_pool = kmem_cache_create("cfq_ioc_pool",
			sizeof(struct cfq_io_context), 0, 0, NULL, NULL);
	if (!cfq_ioc_pool)
		goto fail;

	return 0;
fail:
	cfq_slab_kill();
	return -ENOMEM;
}

/*
 * sysfs parts below -->
 */

static ssize_t
cfq_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
cfq_var_store(unsigned int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtoul(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(elevator_t *e, char *page)			\
{									\
	struct cfq_data *cfqd = e->elevator_data;			\
	unsigned int __data = __VAR;					\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return cfq_var_show(__data, (page));				\
}
SHOW_FUNCTION(cfq_quantum_show, cfqd->cfq_quantum, 0);
SHOW_FUNCTION(cfq_queued_show, cfqd->cfq_queued, 0);
SHOW_FUNCTION(cfq_fifo_expire_sync_show, cfqd->cfq_fifo_expire[1], 1);
SHOW_FUNCTION(cfq_fifo_expire_async_show, cfqd->cfq_fifo_expire[0], 1);
SHOW_FUNCTION(cfq_back_seek_max_show, cfqd->cfq_back_max, 0);
SHOW_FUNCTION(cfq_back_seek_penalty_show, cfqd->cfq_back_penalty, 0);
SHOW_FUNCTION(cfq_slice_idle_show, cfqd->cfq_slice_idle, 1);
SHOW_FUNCTION(cfq_slice_sync_show, cfqd->cfq_slice[1], 1);
SHOW_FUNCTION(cfq_slice_async_show, cfqd->cfq_slice[0], 1);
SHOW_FUNCTION(cfq_slice_async_rq_show, cfqd->cfq_slice_async_rq, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(elevator_t *e, const char *page, size_t count)	\
{									\
	struct cfq_data *cfqd = e->elevator_data;			\
	unsigned int __data;						\
	int ret = cfq_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(cfq_quantum_store, &cfqd->cfq_quantum, 1, UINT_MAX, 0);
STORE_FUNCTION(cfq_queued_store, &cfqd->cfq_queued, 1, UINT_MAX, 0);
STORE_FUNCTION(cfq_fifo_expire_sync_store, &cfqd->cfq_fifo_expire[1], 1, UINT_MAX, 1);
STORE_FUNCTION(cfq_fifo_expire_async_store, &cfqd->cfq_fifo_expire[0], 1, UINT_MAX, 1);
STORE_FUNCTION(cfq_back_seek_max_store, &cfqd->cfq_back_max, 0, UINT_MAX, 0);
STORE_FUNCTION(cfq_back_seek_penalty_store, &cfqd->cfq_back_penalty, 1, UINT_MAX, 0);
STORE_FUNCTION(cfq_slice_idle_store, &cfqd->cfq_slice_idle, 0, UINT_MAX, 1);
STORE_FUNCTION(cfq_slice_sync_store, &cfqd->cfq_slice[1], 1, UINT_MAX, 1);
STORE_FUNCTION(cfq_slice_async_store, &cfqd->cfq_slice[0], 1, UINT_MAX, 1);
STORE_FUNCTION(cfq_slice_async_rq_store, &cfqd->cfq_slice_async_rq, 1, UINT_MAX, 0);
#undef STORE_FUNCTION

#define CFQ_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, cfq_##name##_show, cfq_##name##_store)

static struct elv_fs_entry cfq_attrs[] = {
	CFQ_ATTR(quantum),
	CFQ_ATTR(queued),
	CFQ_ATTR(fifo_expire_sync),
	CFQ_ATTR(fifo_expire_async),
	CFQ_ATTR(back_seek_max),
	CFQ_ATTR(back_seek_penalty),
	CFQ_ATTR(slice_sync),
	CFQ_ATTR(slice_async),
	CFQ_ATTR(slice_async_rq),
	CFQ_ATTR(slice_idle),
	__ATTR_NULL
};

static struct elevator_type iosched_cfq = {
	.ops = {
		.elevator_merge_fn = 		cfq_merge,
		.elevator_merged_fn =		cfq_merged_request,
		.elevator_merge_req_fn =	cfq_merged_requests,
		.elevator_dispatch_fn =		cfq_dispatch_requests,
		.elevator_add_req_fn =		cfq_insert_request,
		.elevator_activate_req_fn =	cfq_activate_request,
		.elevator_deactivate_req_fn =	cfq_deactivate_request,
		.elevator_queue_empty_fn =	cfq_queue_empty,
		.elevator_completed_req_fn =	cfq_completed_request,
		.elevator_former_req_fn =	cfq_former_request,
		.elevator_latter_req_fn =	cfq_latter_request,
		.elevator_set_req_fn =		cfq_set_request,	/* 在分配新的request对象时，要调用此接口为request:elevator_private设置成员 */
		.elevator_put_req_fn =		cfq_put_request,
		.elevator_may_queue_fn =	cfq_may_queue,
		.elevator_init_fn =		cfq_init_queue,
		.elevator_exit_fn =		cfq_exit_queue,
		.trim =				cfq_trim,
	},
	.elevator_attrs =	cfq_attrs,
	.elevator_name =	"cfq",
	.elevator_owner =	THIS_MODULE,
};

static int __init cfq_init(void)
{
	int ret;

	/*
	 * could be 0 on HZ < 1000 setups
	 */
	if (!cfq_slice_async)
		cfq_slice_async = 1;
	if (!cfq_slice_idle)
		cfq_slice_idle = 1;

	if (cfq_slab_setup())
		return -ENOMEM;

	ret = elv_register(&iosched_cfq);
	if (ret)
		cfq_slab_kill();

	return ret;
}

static void __exit cfq_exit(void)
{
	DECLARE_COMPLETION(all_gone);
	elv_unregister(&iosched_cfq);
	ioc_gone = &all_gone;
	/* ioc_gone's update must be visible before reading ioc_count */
	smp_wmb();
	if (atomic_read(&ioc_count))
		wait_for_completion(ioc_gone);
	synchronize_rcu();
	cfq_slab_kill();
}

module_init(cfq_init);
module_exit(cfq_exit);

MODULE_AUTHOR("Jens Axboe");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Completely Fair Queueing IO scheduler");
