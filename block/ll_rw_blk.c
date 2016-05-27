/*
 * Copyright (C) 1991, 1992 Linus Torvalds
 * Copyright (C) 1994,      Karl Keyte: Added support for disk statistics
 * Elevator latency, (C) 2000  Andrea Arcangeli <andrea@suse.de> SuSE
 * Queue request tables / lock, selectable elevator, Jens Axboe <axboe@suse.de>
 * kernel-doc documentation started by NeilBrown <neilb@cse.unsw.edu.au> -  July2000
 * bio rewrite, highmem i/o, etc, Jens Axboe <axboe@suse.de> - may 2001
 */

/*
 * This handles all read/write requests to block devices
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/bootmem.h>	/* for max_pfn/max_low_pfn */
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/interrupt.h>
#include <linux/cpu.h>
#include <linux/blktrace_api.h>

/*
 * for max sense size
 */
#include <scsi/scsi_cmnd.h>

#include "encryption-request.h"

static void blk_unplug_work(void *data);
static void blk_unplug_timeout(unsigned long data);
static void drive_stat_acct(struct request *rq, int nr_sectors, int new_io);
static void init_request_from_bio(struct request *req, struct bio *bio);
static int __make_request(request_queue_t *q, struct bio *bio);

/*
 * For the allocated request tables
 */
/* 用于分配request请求对象的高速缓存对象 */
static kmem_cache_t *request_cachep;

/*
 * For queue allocation
 */
/* 用于分配请求队列的高速缓存对象 */
static kmem_cache_t *requestq_cachep;

/*
 * For io context allocations
 */
/* 用于分配io上下文对象的高速缓存对象 */
static kmem_cache_t *iocontext_cachep;

static wait_queue_head_t congestion_wqh[2] = {
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[0]),
		__WAIT_QUEUE_HEAD_INITIALIZER(congestion_wqh[1])
	};

/*
 * Controlling structure to kblockd
 */
static struct workqueue_struct *kblockd_workqueue;

unsigned long blk_max_low_pfn, blk_max_pfn;

EXPORT_SYMBOL(blk_max_low_pfn);
EXPORT_SYMBOL(blk_max_pfn);

//.data.percpu段中声明per_cpu_blk_cpu_done全局列表
static DEFINE_PER_CPU(struct list_head, blk_cpu_done);

/* Amount of time in which a process may batch requests */
#define BLK_BATCH_TIME	(HZ/50UL)

/* Number of requests a "batching" process may submit */
#define BLK_BATCH_REQ	32

/*
 * Return the threshold (number of used requests) at which the queue is
 * considered to be congested.  It include a little hysteresis to keep the
 * context switch rate down.
 */
static inline int queue_congestion_on_threshold(struct request_queue *q)
{
	return q->nr_congestion_on;//5
}

/*
 * The threshold at which a queue is considered to be uncongested
 */
static inline int queue_congestion_off_threshold(struct request_queue *q)
{
	return q->nr_congestion_off;///3
}

static void blk_queue_congestion_threshold(struct request_queue *q)
{
	int nr;

	nr = q->nr_requests/*4*/ - (q->nr_requests / 8) + 1;//5
	if (nr > q->nr_requests)
		nr = q->nr_requests;
	q->nr_congestion_on = nr;//5

	nr = q->nr_requests - (q->nr_requests / 8) - (q->nr_requests / 16) - 1;
	if (nr < 1)
		nr = 1;
	q->nr_congestion_off = nr;////3
}

/*
 * A queue has just exitted congestion.  Note this in the global counter of
 * congested queues, and wake up anyone who was waiting for requests to be
 * put back.
 */
static void clear_queue_congested(request_queue_t *q, int rw)
{
	enum bdi_state bit;
	wait_queue_head_t *wqh = &congestion_wqh[rw];

	bit = (rw == WRITE) ? BDI_write_congested : BDI_read_congested;
	clear_bit(bit, &q->backing_dev_info.state);
	smp_mb__after_clear_bit();
	if (waitqueue_active(wqh))
		wake_up(wqh);
}

/*
 * A queue has just entered congestion.  Flag that in the queue's VM-visible
 * state flags and increment the global gounter of congested queues.
 */
static void set_queue_congested(request_queue_t *q, int rw)
{
	enum bdi_state bit;

	bit = (rw == WRITE) ? BDI_write_congested : BDI_read_congested;
	set_bit(bit, &q->backing_dev_info.state);
}

/**
 * blk_get_backing_dev_info - get the address of a queue's backing_dev_info
 * @bdev:	device
 *
 * Locates the passed device's request queue and returns the address of its
 * backing_dev_info
 *
 * Will return NULL if the request queue cannot be located.
 */
struct backing_dev_info *blk_get_backing_dev_info(struct block_device *bdev)
{
	struct backing_dev_info *ret = NULL;
	request_queue_t *q = bdev_get_queue(bdev);

	if (q)
		ret = &q->backing_dev_info;
	return ret;
}

EXPORT_SYMBOL(blk_get_backing_dev_info);

void blk_queue_activity_fn(request_queue_t *q, activity_fn *fn, void *data)
{
	q->activity_fn = fn;
	q->activity_data = data;
}

EXPORT_SYMBOL(blk_queue_activity_fn);

/**
 * blk_queue_prep_rq - set a prepare_request function for queue
 * @q:		queue
 * @pfn:	prepare_request function
 *
 * It's possible for a queue to register a prepare_request callback which
 * is invoked before the request is handed to the request_fn. The goal of
 * the function is to prepare a request for I/O, it can be used to build a
 * cdb from the request data for instance.
 *
 */
/**ltl
 * 功能:设置请求队列的prep_rq_fn接口，这个接口主要作用:在处理某一个request进，先根据请求去生成一个scsi_cmnd命令对象，并把这个scsi_cmnd对象存放到spicel域中
 * 参数:
*/
void blk_queue_prep_rq(request_queue_t *q, prep_rq_fn *pfn)
{
	q->prep_rq_fn = pfn;
}

EXPORT_SYMBOL(blk_queue_prep_rq);

/**
 * blk_queue_merge_bvec - set a merge_bvec function for queue
 * @q:		queue
 * @mbfn:	merge_bvec_fn
 *
 * Usually queues have static limitations on the max sectors or segments that
 * we can put in a request. Stacking drivers may have some settings that
 * are dynamic, and thus we have to query the queue whether it is ok to
 * add a new bio_vec to a bio at a given offset or not. If the block device
 * has such limitations, it needs to register a merge_bvec_fn to control
 * the size of bio's sent to it. Note that a block device *must* allow a
 * single page to be added to an empty bio. The block device driver may want
 * to use the bio_split() function to deal with these bio's. By default
 * no merge_bvec_fn is defined for a queue, and only the fixed limits are
 * honored.
 */
void blk_queue_merge_bvec(request_queue_t *q, merge_bvec_fn *mbfn)
{
	q->merge_bvec_fn = mbfn;
}

EXPORT_SYMBOL(blk_queue_merge_bvec);

/**ltl
 * 功能:设置请求队列的软中断处理函数(是软中断BLOCK_SOFTIRQ的处理函数)
 * 参数:q	->请求列表
 *	fn	->中断处理函数
 */
void blk_queue_softirq_done(request_queue_t *q, softirq_done_fn *fn)
{
	q->softirq_done_fn = fn;
}

EXPORT_SYMBOL(blk_queue_softirq_done);

/**
 * blk_queue_make_request - define an alternate make_request function for a device
 * @q:  the request queue for the device to be affected
 * @mfn: the alternate make_request function
 *
 * Description:
 *    The normal way for &struct bios to be passed to a device
 *    driver is for them to be collected into requests on a request
 *    queue, and then to allow the device driver to select requests
 *    off that queue when it is ready.  This works well for many block
 *    devices. However some block devices (typically virtual devices
 *    such as md or lvm) do not benefit from the processing on the
 *    request queue, and are served best by having the requests passed
 *    directly to them.  This can be achieved by providing a function
 *    to blk_queue_make_request().
 *
 * Caveat:
 *    The driver that does this *must* be able to deal appropriately
 *    with buffers in "highmemory". This can be accomplished by either calling
 *    __bio_kmap_atomic() to get a temporary kernel mapping, or by calling
 *    blk_queue_bounce() to create a buffer in normal memory.
 **/
/**ltl
 * 功能:对请求队列部分域的初始化
 * 参数:q	->请求队列
 *	mfn	->构造请求的处理函数
 */
void blk_queue_make_request(request_queue_t * q, make_request_fn * mfn)
{
	/*
	 * set defaults
	 */
	q->nr_requests = BLKDEV_MAX_RQ; /* 一个请求队列，请求request最大个数 */
	blk_queue_max_phys_segments(q, MAX_PHYS_SEGMENTS);	/* 设置一个request最大物理段数据 */
	blk_queue_max_hw_segments(q, MAX_HW_SEGMENTS);
	q->make_request_fn = mfn;	/* 设置构造request请求的接口，在这个接口是把request放入IO调度器 */
	q->backing_dev_info.ra_pages = (VM_MAX_READAHEAD * 1024) / PAGE_CACHE_SIZE;
	q->backing_dev_info.state = 0;
	q->backing_dev_info.capabilities = BDI_CAP_MAP_COPY;
	blk_queue_max_sectors(q, SAFE_MAX_SECTORS);/* 一个request的最大扇区数 */
	blk_queue_hardsect_size(q, 512);	/* 一个物理扇区大小 */
	blk_queue_dma_alignment(q, 511);
	blk_queue_congestion_threshold(q);/* 设置拥塞参数 */
	q->nr_batching = BLK_BATCH_REQ;

	q->unplug_thresh = 4;/*在IO调度器中请求超过这个数，就要"泄流"*/		/* hmm */
	q->unplug_delay = (3 * HZ) / 1000;	/*第一个提交到IO调度器超过3ms，就要开始"泄流"*//* 3 milliseconds */
	if (q->unplug_delay == 0)
		q->unplug_delay = 1;

	INIT_WORK(&q->unplug_work, blk_unplug_work, q);/* 初始化"泄流"工作队列 */

	q->unplug_timer.function = blk_unplug_timeout;/* 初始化"泄流"定时器 */
	q->unplug_timer.data = (unsigned long)q;

	/*
	 * by default assume old behaviour and bounce for any highmem page
	 */
	blk_queue_bounce_limit(q, BLK_BOUNCE_HIGH);/* 设置反弹缓冲区的地址 */

	blk_queue_activity_fn(q, NULL, NULL);
}

EXPORT_SYMBOL(blk_queue_make_request);

/**ltl
 * 功能:初始化request对象
 * 参数:
 */
static inline void rq_init(request_queue_t *q, struct request *rq)
{
	INIT_LIST_HEAD(&rq->queuelist);
	INIT_LIST_HEAD(&rq->donelist);

	rq->errors = 0;
	rq->rq_status = RQ_ACTIVE;
	rq->bio = rq->biotail = NULL;
	rq->ioprio = 0;
	rq->buffer = NULL;
	rq->ref_count = 1;
	rq->q = q;
	rq->waiting = NULL;
	rq->special = NULL;
	rq->data_len = 0;
	rq->data = NULL;
	rq->nr_phys_segments = 0;
	rq->sense = NULL;
	rq->end_io = NULL;
	rq->end_io_data = NULL;
	rq->completion_data = NULL;
}

/**
 * blk_queue_ordered - does this queue support ordered writes
 * @q:        the request queue
 * @ordered:  one of QUEUE_ORDERED_*
 * @prepare_flush_fn: rq setup helper for cache flush ordered writes
 *
 * Description:
 *   For journalled file systems, doing ordered writes on a commit
 *   block instead of explicitly doing wait_on_buffer (which is bad
 *   for performance) can be a big win. Block drivers supporting this
 *   feature should call this function and indicate so.
 *
 **/
/**ltl
 * 功能:设置请求队列的屏障属性，这是提供给块设备驱动程序的一接口
 * 参数:	q	->请求队列
 *	oreder	->屏障标志
 *	prepare_flush_fn ->刷新之前的处理函数
 */
int blk_queue_ordered(request_queue_t *q, unsigned ordered,
		      prepare_flush_fn *prepare_flush_fn)
{
	if (ordered & (QUEUE_ORDERED_PREFLUSH | QUEUE_ORDERED_POSTFLUSH) &&
	    prepare_flush_fn == NULL) {
		printk(KERN_ERR "blk_queue_ordered: prepare_flush_fn required\n");
		return -EINVAL;
	}

	if (ordered != QUEUE_ORDERED_NONE &&
	    ordered != QUEUE_ORDERED_DRAIN &&
	    ordered != QUEUE_ORDERED_DRAIN_FLUSH &&
	    ordered != QUEUE_ORDERED_DRAIN_FUA &&
	    ordered != QUEUE_ORDERED_TAG &&
	    ordered != QUEUE_ORDERED_TAG_FLUSH &&
	    ordered != QUEUE_ORDERED_TAG_FUA) {
		printk(KERN_ERR "blk_queue_ordered: bad value %d\n", ordered);
		return -EINVAL;
	}

	q->ordered = ordered;
	q->next_ordered = ordered;
	q->prepare_flush_fn = prepare_flush_fn;/* 值:sd_prepare_flush */

	return 0;
}

EXPORT_SYMBOL(blk_queue_ordered);

/**
 * blk_queue_issue_flush_fn - set function for issuing a flush
 * @q:     the request queue
 * @iff:   the function to be called issuing the flush
 *
 * Description:
 *   If a driver supports issuing a flush command, the support is notified
 *   to the block layer by defining it through this call.
 *
 **/
void blk_queue_issue_flush_fn(request_queue_t *q, issue_flush_fn *iff)
{
	q->issue_flush_fn = iff;
}

EXPORT_SYMBOL(blk_queue_issue_flush_fn);

/*
 * Cache flushing for ordered writes handling
 */
/**ltl
 * 功能:获取当前最低位的值
 * 参数:
 */
inline unsigned blk_ordered_cur_seq(request_queue_t *q)
{
	if (!q->ordseq)
		return 0;
	return 1 << ffz(q->ordseq);/* 在字中查找第一个0 */
}
/**ltl
 * 功能:判定请求处理的阶段
 * 参数:
 */
unsigned blk_ordered_req_seq(struct request *rq)
{
	request_queue_t *q = rq->q;

	BUG_ON(q->ordseq == 0);

	if (rq == &q->pre_flush_rq)
		return QUEUE_ORDSEQ_PREFLUSH;
	if (rq == &q->bar_rq)
		return QUEUE_ORDSEQ_BAR;
	if (rq == &q->post_flush_rq)
		return QUEUE_ORDSEQ_POSTFLUSH;
	/* 其他非文件系统请求应该在语法队列处理QUEUE_ORDSEQ_DRAIN阶段时执行(即要抽干IO调度器) */
	if ((rq->flags & REQ_ORDERED_COLOR) ==
	    (q->orig_bar_rq->flags & REQ_ORDERED_COLOR))
		return QUEUE_ORDSEQ_DRAIN;
	else
		return QUEUE_ORDSEQ_DONE;
}
/**ltl
 * 功能:设置屏障IO的状态，当状态为QUEUE_ORDSEQ_DONE时，去处理原始的request。
 * 参数:
 */
void blk_ordered_complete_seq(request_queue_t *q, unsigned seq, int error)
{
	struct request *rq;
	int uptodate;

	if (error && !q->orderr)
		q->orderr = error;

	BUG_ON(q->ordseq & seq);/* 已经设置的seq状态，则挂起系统 */
	q->ordseq |= seq;/* 设置当前状态 */

	if (blk_ordered_cur_seq(q) != QUEUE_ORDSEQ_DONE)/* 处理没有结束时，没有把状态位QUEUE_ORDSEQ_DONE置位 */
		return;

	/*所有的屏障序列都执行完毕*/
	/*
	 * Okay, sequence complete.
	 */
	rq = q->orig_bar_rq;/* 处理最原始的数据 */
	uptodate = q->orderr ? q->orderr : 1;/* 是否出错，错误码 */

	q->ordseq = 0;
	/* 调用bio->bi_end_io() */
	end_that_request_first(rq, uptodate, rq->hard_nr_sectors);
	/* 释放request与调度算法的私有对象 */
	end_that_request_last(rq, uptodate);
}

static void pre_flush_end_io(struct request *rq, int error)
{
	elv_completed_request(rq->q, rq);
	blk_ordered_complete_seq(rq->q, QUEUE_ORDSEQ_PREFLUSH, error);
}
/**ltl
 * 功能:一个屏障IO处理完成后，调用的函数
 * 参数:rq	->屏障IO请求对象
 *	error	->处理是否成功，失败的错误码
 */
static void bar_end_io(struct request *rq, int error)
{
	elv_completed_request(rq->q, rq);/* 请求处理完成 */
	blk_ordered_complete_seq(rq->q, QUEUE_ORDSEQ_BAR, error);
}

static void post_flush_end_io(struct request *rq, int error)
{
	elv_completed_request(rq->q, rq);
	blk_ordered_complete_seq(rq->q, QUEUE_ORDSEQ_POSTFLUSH, error);
}
/**ltl
 * 功能: 构造调度队列的冲刷请求
 * 参数: q	->请求队列对象
 * 		which->冲刷位置
 * 返回值:
 * 说明:
 */
static void queue_flush(request_queue_t *q, unsigned which)
{
	struct request *rq;
	rq_end_io_fn *end_io;

	if (which == QUEUE_ORDERED_PREFLUSH) {
		rq = &q->pre_flush_rq;
		end_io = pre_flush_end_io;
	} else {
		rq = &q->post_flush_rq;
		end_io = post_flush_end_io;
	}

	rq_init(q, rq);	/* 初始化请求 */
	rq->flags = REQ_HARDBARRIER;	/* 设置屏障标志 */
	rq->elevator_private = NULL;
	rq->rq_disk = q->bar_rq.rq_disk;	/* 通用磁盘块 */
	rq->rl = NULL;
	rq->end_io = end_io;				/* 屏障结束的回调接口 */
	q->prepare_flush_fn(q, rq);			/* 调用构造冲刷请求 */
	/* 将冲刷请求插入到派发队列中 */
	elv_insert(q, rq, ELEVATOR_INSERT_FRONT);
}
/**ltl
 * 功能:为处理屏障请求准备整个请求序列，该系列最多包含三个请求:屏障前冲刷请求(pre_flush_rq)、代理屏障请求(bar_rq)和屏障后冲刷请求(post_flush_rq)
 *	注:之所以叫代理屏障请求，是因为可能没有办法直接用原始的屏障请求，比如在QUEUE_ORDERED_DRAIN_FUA的方法下，需要将屏障请求替换为带FUA(force unit access)的形式
 * 参数:q	->请求队列
 *	rq	->请求对象
 * 返回值:
 */
static inline struct request *start_ordered(request_queue_t *q,
					    struct request *rq)
{
	q->bi_size = 0;
	q->orderr = 0;
	q->ordered = q->next_ordered;
	q->ordseq |= QUEUE_ORDSEQ_STARTED;/* 把当前状态设置成"开始" */

	/*
	 * Prep proxy barrier request.
	 */
	blkdev_dequeue_request(rq);/* 把request从派发队列中删除 */
	q->orig_bar_rq = rq;/* 记录原始的request，以防在后面被替换掉，在处理完新的request找不回原来的request. */
	rq = &q->bar_rq;/* 代理屏障请求 */
	rq_init(q, rq);/* 初始化 */
	rq->flags = bio_data_dir(q->orig_bar_rq->bio);/* 数据请求的方向 */
	rq->flags |= q->ordered & QUEUE_ORDERED_FUA ? REQ_FUA : 0; /* 屏障请求刷新方式 */
	rq->elevator_private = NULL;
	rq->rl = NULL;
	init_request_from_bio(rq, q->orig_bar_rq->bio);/* 对请求属性域进行赋值。*/
	rq->end_io = bar_end_io;/* 设置请求处理完成函数(针对屏障IO的请求) */

	/*
	 * Queue ordered sequence.  As we stack them at the head, we
	 * need to queue in reverse order.  Note that we rely on that
	 * no fs request uses ELEVATOR_INSERT_FRONT and thus no fs
	 * request gets inbetween ordered sequence.
	 */
	/*
	 * 注:执行下面三个步骤后，由于三个请求都是往派发队列头插入，所以在队列的顺序与插入顺序相反，
	 *  即派发队列的顺序:屏障前冲刷请求|屏障请求|屏障后冲刷请求
	 */
	 /* 1.把屏障后冲刷请求插入到派发队列中 */
	if (q->ordered & QUEUE_ORDERED_POSTFLUSH)
		queue_flush(q, QUEUE_ORDERED_POSTFLUSH);
	else
		q->ordseq |= QUEUE_ORDSEQ_POSTFLUSH;

	/* 2.把代理屏蔽IO请求插入到派发队列中 */
	elv_insert(q, rq, ELEVATOR_INSERT_FRONT);

	/* 3.把屏障前冲刷请求插入到派发队列 */
	if (q->ordered & QUEUE_ORDERED_PREFLUSH) {
		queue_flush(q, QUEUE_ORDERED_PREFLUSH);
		rq = &q->pre_flush_rq;
	} else
		q->ordseq |= QUEUE_ORDSEQ_PREFLUSH;

	if ((q->ordered & QUEUE_ORDERED_TAG) || q->in_flight == 0)
		q->ordseq |= QUEUE_ORDSEQ_DRAIN;
	else
		rq = NULL;

	return rq;
}
/**ltl
 * 功能:处理屏障请求
 * 参数:q	->请求队列
 *	rgp	->[in/out]当前要处理的请求
 * 返回值:0				->尝试下一个请求
 *	 1 && rqp==NULL		->稍后尝试
 *	 1 && rpq != NULL 	->表示要执行下一个请求，这个请求可能是原来的或者已经被替换的请求
 */
int blk_do_ordered(request_queue_t *q, struct request **rqp)
{
	struct request *rq = *rqp;
	int is_barrier = blk_fs_request(rq) && blk_barrier_rq(rq);/* 屏障请求标志位 */

	if (!q->ordseq) {/* 即此时还没有进入排序队列。*/
		if (!is_barrier)/* 如果进入的请求不是屏障请求，则返回1,即表示直接处理请求 */
			return 1;

		if (q->next_ordered != QUEUE_ORDERED_NONE) {/* request支持屏障IO */
			*rqp = start_ordered(q, rq);
			return 1;
		} else {
			/*
			 * This can happen when the queue switches to
			 * ORDERED_NONE while this request is on it.
			 */
			/*如果传入的是屏障请求，需要看请求队列支持的排序方式，即 next_ordered域。如果为QUEUE_ORDERED_NONE,即不支持屏障IO,
			则只能将屏障请求从队列取出，并以-EOPNOTSUPP错误码结束它。*/
			blkdev_dequeue_request(rq);
			end_that_request_first(rq, -EOPNOTSUPP,
					       rq->hard_nr_sectors);
			end_that_request_last(rq, -EOPNOTSUPP);
			*rqp = NULL;
			return 0;
		}
	}

	/*
	 * Ordered sequence in progress
	 */

	/* Special requests are not subject to ordering rules. */
	/* 不是文件系统的请求、不是屏障前冲刷请求、不是屏障后冲刷请求 */
	if (!blk_fs_request(rq) &&
	    rq != &q->pre_flush_rq && rq != &q->post_flush_rq)
		return 1;

	if (q->ordered & QUEUE_ORDERED_TAG) {
		/* Ordered by tag.  Blocking the next barrier is enough. */
		if (is_barrier && rq != &q->bar_rq)
			*rqp = NULL;
	} else {
		/* Ordered by draining.  Wait for turn. */
		WARN_ON(blk_ordered_req_seq(rq) < blk_ordered_cur_seq(q));
		if (blk_ordered_req_seq(rq) > blk_ordered_cur_seq(q))
			*rqp = NULL;
	}

	return 1;
}

/**ltl
功能:屏障IO的处理函数，注:要搞清楚request_queue、request、bio、bio_vec中与数据长度有关的各个域含义
参数:bio	->bio对象
	bytes	->处理完成的字节数
	error	->错误码
*/
static int flush_dry_bio_endio(struct bio *bio, unsigned int bytes, int error)
{
	request_queue_t *q = bio->bi_private;
	struct bio_vec *bvec;
	int i;

	/*
	 * This is dry run, restore bio_sector and size.  We'll finish
	 * this request again with the original bi_end_io after an
	 * error occurs or post flush is complete.
	 */
	q->bi_size += bytes;

	if (bio->bi_size)
		return 1;

	/* Rewind bvec's */
	bio->bi_idx = 0;
	bio_for_each_segment(bvec, bio, i) {
		bvec->bv_len += bvec->bv_offset;
		bvec->bv_offset = 0;
	}

	/* Reset bio */
	set_bit(BIO_UPTODATE, &bio->bi_flags);
	bio->bi_size = q->bi_size;
	bio->bi_sector -= (q->bi_size >> 9);
	q->bi_size = 0;

	return 0;
}

/**ltl
功能:屏障IO处理完成后，调用此接口
参数:rq	->请求对象
	bio	->处理完成的bio对象
	nbytes->完成的字节数
	error	->错误码
*/
static inline int ordered_bio_endio(struct request *rq, struct bio *bio,
				    unsigned int nbytes, int error)
{
	request_queue_t *q = rq->q;
	bio_end_io_t *endio;
	void *private;

	if (&q->bar_rq != rq)/* 如果不是屏障IO请求，直接退出。*/
		return 0;

	/*
	 * Okay, this is the barrier request in progress, dry finish it.
	 */
	if (error && !q->orderr)
		q->orderr = error;

	endio = bio->bi_end_io;
	private = bio->bi_private;
	bio->bi_end_io = flush_dry_bio_endio;/* 设置屏障IO完成处理函数 */
	bio->bi_private = q;

	bio_endio(bio, nbytes, error);/* 调用屏障IO完成处理函数 */

	bio->bi_end_io = endio;
	bio->bi_private = private;

	return 1;
}

/**
 * blk_queue_bounce_limit - set bounce buffer limit for queue
 * @q:  the request queue for the device
 * @dma_addr:   bus address limit
 *
 * Description:
 *    Different hardware can have different requirements as to what pages
 *    it can do I/O directly to. A low level driver can call
 *    blk_queue_bounce_limit to have lower memory pages allocated as bounce
 *    buffers for doing I/O to pages residing above @page.
 **/
void blk_queue_bounce_limit(request_queue_t *q, u64 dma_addr)
{
	unsigned long bounce_pfn = dma_addr >> PAGE_SHIFT;
	int dma = 0;

	q->bounce_gfp = GFP_NOIO;
#if BITS_PER_LONG == 64
	/* Assume anything <= 4GB can be handled by IOMMU.
	   Actually some IOMMUs can handle everything, but I don't
	   know of a way to test this here. */
	if (bounce_pfn < (min_t(u64,0xffffffff,BLK_BOUNCE_HIGH) >> PAGE_SHIFT))
		dma = 1;
	q->bounce_pfn = max_low_pfn;/* 低端内存的最大页号 */
#else
	if (bounce_pfn < blk_max_low_pfn)
		dma = 1;
	q->bounce_pfn = bounce_pfn;
#endif
	if (dma) {
		init_emergency_isa_pool();
		q->bounce_gfp = GFP_NOIO | GFP_DMA;
		q->bounce_pfn = bounce_pfn;
	}
}

EXPORT_SYMBOL(blk_queue_bounce_limit);

/**
 * blk_queue_max_sectors - set max sectors for a request for this queue
 * @q:  the request queue for the device
 * @max_sectors:  max sectors in the usual 512b unit
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the size of
 *    received requests.
 **/
/**ltl
 * 功能:根据具体设备配置最大的扇区数和最大的硬件扇区数。
 * 参数:q	->请求队列
 *	max_sectors->硬件的最大扇区数(由硬件确定),eg:ata_scsi_dev_config
*/
void blk_queue_max_sectors(request_queue_t *q, unsigned int max_sectors)
{
	if ((max_sectors << 9) < PAGE_CACHE_SIZE) {
		max_sectors = 1 << (PAGE_CACHE_SHIFT - 9);
		printk("%s: set to minimum %d\n", __FUNCTION__, max_sectors);
	}
	/*
	 * 如果在低层驱动里设置的最大扇区数小于1024，则max_hw_sectors和max_sectors都为硬件设置的值。
	 * 否则:max_sectors为1024，而max_hw_sectors为硬件驱动设置的值。
	 */
	if (BLK_DEF_MAX_SECTORS > max_sectors)
		q->max_hw_sectors = q->max_sectors = max_sectors;
 	else {
		q->max_sectors = BLK_DEF_MAX_SECTORS;
		q->max_hw_sectors = max_sectors;
	}
}

EXPORT_SYMBOL(blk_queue_max_sectors);

/**
 * blk_queue_max_phys_segments - set max phys segments for a request for this queue
 * @q:  the request queue for the device
 * @max_segments:  max number of segments
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the number of
 *    physical data segments in a request.  This would be the largest sized
 *    scatter list the driver could handle.
 **/
void blk_queue_max_phys_segments(request_queue_t *q, unsigned short max_segments)
{
	if (!max_segments) {
		max_segments = 1;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_segments);
	}

	q->max_phys_segments = max_segments;
}

EXPORT_SYMBOL(blk_queue_max_phys_segments);

/**
 * blk_queue_max_hw_segments - set max hw segments for a request for this queue
 * @q:  the request queue for the device
 * @max_segments:  max number of segments
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the number of
 *    hw data segments in a request.  This would be the largest number of
 *    address/length pairs the host adapter can actually give as once
 *    to the device.
 **/
void blk_queue_max_hw_segments(request_queue_t *q, unsigned short max_segments)
{
	if (!max_segments) {
		max_segments = 1;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_segments);
	}

	q->max_hw_segments = max_segments;
}

EXPORT_SYMBOL(blk_queue_max_hw_segments);

/**
 * blk_queue_max_segment_size - set max segment size for blk_rq_map_sg
 * @q:  the request queue for the device
 * @max_size:  max size of segment in bytes
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the size of a
 *    coalesced segment
 **/
void blk_queue_max_segment_size(request_queue_t *q, unsigned int max_size)
{
	if (max_size < PAGE_CACHE_SIZE) {
		max_size = PAGE_CACHE_SIZE;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_size);
	}

	q->max_segment_size = max_size;
}

EXPORT_SYMBOL(blk_queue_max_segment_size);

/**
 * blk_queue_hardsect_size - set hardware sector size for the queue
 * @q:  the request queue for the device
 * @size:  the hardware sector size, in bytes
 *
 * Description:
 *   This should typically be set to the lowest possible sector size
 *   that the hardware can operate on (possible without reverting to
 *   even internal read-modify-write operations). Usually the default
 *   of 512 covers most hardware.
 **/
void blk_queue_hardsect_size(request_queue_t *q, unsigned short size)
{
	q->hardsect_size = size;
}

EXPORT_SYMBOL(blk_queue_hardsect_size);

/*
 * Returns the minimum that is _not_ zero, unless both are zero.
 */
#define min_not_zero(l, r) (l == 0) ? r : ((r == 0) ? l : min(l, r))

/**
 * blk_queue_stack_limits - inherit underlying queue limits for stacked drivers
 * @t:	the stacking driver (top)
 * @b:  the underlying device (bottom)
 **/
void blk_queue_stack_limits(request_queue_t *t, request_queue_t *b)
{
	/* zero is "infinity" */
	t->max_sectors = min_not_zero(t->max_sectors,b->max_sectors);
	t->max_hw_sectors = min_not_zero(t->max_hw_sectors,b->max_hw_sectors);

	t->max_phys_segments = min(t->max_phys_segments,b->max_phys_segments);
	t->max_hw_segments = min(t->max_hw_segments,b->max_hw_segments);
	t->max_segment_size = min(t->max_segment_size,b->max_segment_size);
	t->hardsect_size = max(t->hardsect_size,b->hardsect_size);
	if (!test_bit(QUEUE_FLAG_CLUSTER, &b->queue_flags))
		clear_bit(QUEUE_FLAG_CLUSTER, &t->queue_flags);
}

EXPORT_SYMBOL(blk_queue_stack_limits);

/**
 * blk_queue_segment_boundary - set boundary rules for segment merging
 * @q:  the request queue for the device
 * @mask:  the memory boundary mask
 **/
void blk_queue_segment_boundary(request_queue_t *q, unsigned long mask)
{
	if (mask < PAGE_CACHE_SIZE - 1) {
		mask = PAGE_CACHE_SIZE - 1;
		printk("%s: set to minimum %lx\n", __FUNCTION__, mask);
	}

	q->seg_boundary_mask = mask;
}

EXPORT_SYMBOL(blk_queue_segment_boundary);

/**
 * blk_queue_dma_alignment - set dma length and memory alignment
 * @q:     the request queue for the device
 * @mask:  alignment mask
 *
 * description:
 *    set required memory and length aligment for direct dma transactions.
 *    this is used when buiding direct io requests for the queue.
 *
 **/
void blk_queue_dma_alignment(request_queue_t *q, int mask)
{
	q->dma_alignment = mask;
}

EXPORT_SYMBOL(blk_queue_dma_alignment);

/**
 * blk_queue_find_tag - find a request by its tag and queue
 * @q:	 The request queue for the device
 * @tag: The tag of the request
 *
 * Notes:
 *    Should be used when a device returns a tag and you want to match
 *    it with a request.
 *
 *    no locks need be held.
 **/
struct request *blk_queue_find_tag(request_queue_t *q, int tag)
{
	struct blk_queue_tag *bqt = q->queue_tags;

	if (unlikely(bqt == NULL || tag >= bqt->real_max_depth))
		return NULL;

	return bqt->tag_index[tag];
}

EXPORT_SYMBOL(blk_queue_find_tag);

/**
 * __blk_queue_free_tags - release tag maintenance info
 * @q:  the request queue for the device
 *
 *  Notes:
 *    blk_cleanup_queue() will take care of calling this function, if tagging
 *    has been used. So there's no need to call this directly.
 **/
static void __blk_queue_free_tags(request_queue_t *q)
{
	struct blk_queue_tag *bqt = q->queue_tags;

	if (!bqt)
		return;

	if (atomic_dec_and_test(&bqt->refcnt)) {
		BUG_ON(bqt->busy);
		BUG_ON(!list_empty(&bqt->busy_list));

		kfree(bqt->tag_index);
		bqt->tag_index = NULL;

		kfree(bqt->tag_map);
		bqt->tag_map = NULL;

		kfree(bqt);
	}

	q->queue_tags = NULL;
	q->queue_flags &= ~(1 << QUEUE_FLAG_QUEUED);
}

/**
 * blk_queue_free_tags - release tag maintenance info
 * @q:  the request queue for the device
 *
 *  Notes:
 *	This is used to disabled tagged queuing to a device, yet leave
 *	queue in function.
 **/
void blk_queue_free_tags(request_queue_t *q)
{
	clear_bit(QUEUE_FLAG_QUEUED, &q->queue_flags);
}

EXPORT_SYMBOL(blk_queue_free_tags);

static int
init_tag_map(request_queue_t *q, struct blk_queue_tag *tags, int depth)
{
	struct request **tag_index;
	unsigned long *tag_map;
	int nr_ulongs;

	if (depth > q->nr_requests * 2) {
		depth = q->nr_requests * 2;
		printk(KERN_ERR "%s: adjusted depth to %d\n",
				__FUNCTION__, depth);
	}

	tag_index = kzalloc(depth * sizeof(struct request *), GFP_ATOMIC);
	if (!tag_index)
		goto fail;

	nr_ulongs = ALIGN(depth, BITS_PER_LONG) / BITS_PER_LONG;
	tag_map = kzalloc(nr_ulongs * sizeof(unsigned long), GFP_ATOMIC);
	if (!tag_map)
		goto fail;

	tags->real_max_depth = depth;
	tags->max_depth = depth;
	tags->tag_index = tag_index;
	tags->tag_map = tag_map;

	return 0;
fail:
	kfree(tag_index);
	return -ENOMEM;
}

/**
 * blk_queue_init_tags - initialize the queue tag info
 * @q:  the request queue for the device
 * @depth:  the maximum queue depth supported
 * @tags: the tag to use
 **/
int blk_queue_init_tags(request_queue_t *q, int depth,
			struct blk_queue_tag *tags)
{
	int rc;

	BUG_ON(tags && q->queue_tags && tags != q->queue_tags);

	if (!tags && !q->queue_tags) {
		tags = kmalloc(sizeof(struct blk_queue_tag), GFP_ATOMIC);
		if (!tags)
			goto fail;

		if (init_tag_map(q, tags, depth))
			goto fail;

		INIT_LIST_HEAD(&tags->busy_list);
		tags->busy = 0;
		atomic_set(&tags->refcnt, 1);
	} else if (q->queue_tags) {
		if ((rc = blk_queue_resize_tags(q, depth)))
			return rc;
		set_bit(QUEUE_FLAG_QUEUED, &q->queue_flags);
		return 0;
	} else
		atomic_inc(&tags->refcnt);

	/*
	 * assign it, all done
	 */
	q->queue_tags = tags;
	q->queue_flags |= (1 << QUEUE_FLAG_QUEUED);
	return 0;
fail:
	kfree(tags);
	return -ENOMEM;
}

EXPORT_SYMBOL(blk_queue_init_tags);

/**
 * blk_queue_resize_tags - change the queueing depth
 * @q:  the request queue for the device
 * @new_depth: the new max command queueing depth
 *
 *  Notes:
 *    Must be called with the queue lock held.
 **/
int blk_queue_resize_tags(request_queue_t *q, int new_depth)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	struct request **tag_index;
	unsigned long *tag_map;
	int max_depth, nr_ulongs;

	if (!bqt)
		return -ENXIO;

	/*
	 * if we already have large enough real_max_depth.  just
	 * adjust max_depth.  *NOTE* as requests with tag value
	 * between new_depth and real_max_depth can be in-flight, tag
	 * map can not be shrunk blindly here.
	 */
	if (new_depth <= bqt->real_max_depth) {
		bqt->max_depth = new_depth;
		return 0;
	}

	/*
	 * save the old state info, so we can copy it back
	 */
	tag_index = bqt->tag_index;
	tag_map = bqt->tag_map;
	max_depth = bqt->real_max_depth;

	if (init_tag_map(q, bqt, new_depth))
		return -ENOMEM;

	memcpy(bqt->tag_index, tag_index, max_depth * sizeof(struct request *));
	nr_ulongs = ALIGN(max_depth, BITS_PER_LONG) / BITS_PER_LONG;
	memcpy(bqt->tag_map, tag_map, nr_ulongs * sizeof(unsigned long));

	kfree(tag_index);
	kfree(tag_map);
	return 0;
}

EXPORT_SYMBOL(blk_queue_resize_tags);

/**
 * blk_queue_end_tag - end tag operations for a request
 * @q:  the request queue for the device
 * @rq: the request that has completed
 *
 *  Description:
 *    Typically called when end_that_request_first() returns 0, meaning
 *    all transfers have been done for a request. It's important to call
 *    this function before end_that_request_last(), as that will put the
 *    request back on the free list thus corrupting the internal tag list.
 *
 *  Notes:
 *   queue lock must be held.
 **/
void blk_queue_end_tag(request_queue_t *q, struct request *rq)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	int tag = rq->tag;

	BUG_ON(tag == -1);

	if (unlikely(tag >= bqt->real_max_depth))
		/*
		 * This can happen after tag depth has been reduced.
		 * FIXME: how about a warning or info message here?
		 */
		return;

	if (unlikely(!__test_and_clear_bit(tag, bqt->tag_map))) {
		printk(KERN_ERR "%s: attempt to clear non-busy tag (%d)\n",
		       __FUNCTION__, tag);
		return;
	}

	list_del_init(&rq->queuelist);
	rq->flags &= ~REQ_QUEUED;
	rq->tag = -1;

	if (unlikely(bqt->tag_index[tag] == NULL))
		printk(KERN_ERR "%s: tag %d is missing\n",
		       __FUNCTION__, tag);

	bqt->tag_index[tag] = NULL;
	bqt->busy--;
}

EXPORT_SYMBOL(blk_queue_end_tag);

/**
 * blk_queue_start_tag - find a free tag and assign it
 * @q:  the request queue for the device
 * @rq:  the block request that needs tagging
 *
 *  Description:
 *    This can either be used as a stand-alone helper, or possibly be
 *    assigned as the queue &prep_rq_fn (in which case &struct request
 *    automagically gets a tag assigned). Note that this function
 *    assumes that any type of request can be queued! if this is not
 *    true for your device, you must check the request type before
 *    calling this function.  The request will also be removed from
 *    the request queue, so it's the drivers responsibility to readd
 *    it if it should need to be restarted for some reason.
 *
 *  Notes:
 *   queue lock must be held.
 **/
int blk_queue_start_tag(request_queue_t *q, struct request *rq)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	int tag;

	if (unlikely((rq->flags & REQ_QUEUED))) {
		printk(KERN_ERR 
		       "%s: request %p for device [%s] already tagged %d",
		       __FUNCTION__, rq,
		       rq->rq_disk ? rq->rq_disk->disk_name : "?", rq->tag);
		BUG();
	}

	tag = find_first_zero_bit(bqt->tag_map, bqt->max_depth);
	if (tag >= bqt->max_depth)
		return 1;

	__set_bit(tag, bqt->tag_map);

	rq->flags |= REQ_QUEUED;
	rq->tag = tag;
	bqt->tag_index[tag] = rq;
	blkdev_dequeue_request(rq);
	list_add(&rq->queuelist, &bqt->busy_list);
	bqt->busy++;
	return 0;
}

EXPORT_SYMBOL(blk_queue_start_tag);

/**
 * blk_queue_invalidate_tags - invalidate all pending tags
 * @q:  the request queue for the device
 *
 *  Description:
 *   Hardware conditions may dictate a need to stop all pending requests.
 *   In this case, we will safely clear the block side of the tag queue and
 *   readd all requests to the request queue in the right order.
 *
 *  Notes:
 *   queue lock must be held.
 **/
void blk_queue_invalidate_tags(request_queue_t *q)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	struct list_head *tmp, *n;
	struct request *rq;

	list_for_each_safe(tmp, n, &bqt->busy_list) {
		rq = list_entry_rq(tmp);

		if (rq->tag == -1) {
			printk(KERN_ERR
			       "%s: bad tag found on list\n", __FUNCTION__);
			list_del_init(&rq->queuelist);
			rq->flags &= ~REQ_QUEUED;
		} else
			blk_queue_end_tag(q, rq);

		rq->flags &= ~REQ_STARTED;
		__elv_add_request(q, rq, ELEVATOR_INSERT_BACK, 0);
	}
}

EXPORT_SYMBOL(blk_queue_invalidate_tags);

static const char * const rq_flags[] = {
	"REQ_RW",
	"REQ_FAILFAST",
	"REQ_SORTED",
	"REQ_SOFTBARRIER",
	"REQ_HARDBARRIER",
	"REQ_FUA",
	"REQ_CMD",
	"REQ_NOMERGE",
	"REQ_STARTED",
	"REQ_DONTPREP",
	"REQ_QUEUED",
	"REQ_ELVPRIV",
	"REQ_PC",
	"REQ_BLOCK_PC",
	"REQ_SENSE",
	"REQ_FAILED",
	"REQ_QUIET",
	"REQ_SPECIAL",
	"REQ_DRIVE_CMD",
	"REQ_DRIVE_TASK",
	"REQ_DRIVE_TASKFILE",
	"REQ_PREEMPT",
	"REQ_PM_SUSPEND",
	"REQ_PM_RESUME",
	"REQ_PM_SHUTDOWN",
	"REQ_ORDERED_COLOR",
};

void blk_dump_rq_flags(struct request *rq, char *msg)
{
	int bit;

	printk("%s: dev %s: flags = ", msg,
		rq->rq_disk ? rq->rq_disk->disk_name : "?");
	bit = 0;
	do {
		if (rq->flags & (1 << bit))
			printk("%s ", rq_flags[bit]);
		bit++;
	} while (bit < __REQ_NR_BITS);

	printk("\nsector %llu, nr/cnr %lu/%u\n", (unsigned long long)rq->sector,
						       rq->nr_sectors,
						       rq->current_nr_sectors);
	printk("bio %p, biotail %p, buffer %p, data %p, len %u\n", rq->bio, rq->biotail, rq->buffer, rq->data, rq->data_len);

	if (rq->flags & (REQ_BLOCK_PC | REQ_PC)) {
		printk("cdb: ");
		for (bit = 0; bit < sizeof(rq->cmd); bit++)
			printk("%02x ", rq->cmd[bit]);
		printk("\n");
	}
}

EXPORT_SYMBOL(blk_dump_rq_flags);

/**ltl
 * 功能:重新计算bio的段长度
 * 参数:q	->请求队列
 *	bio	->要计算的bio对象
 */
void blk_recount_segments(request_queue_t *q, struct bio *bio)
{
	struct bio_vec *bv, *bvprv = NULL;
	int i, nr_phys_segs, nr_hw_segs, seg_size, hw_seg_size, cluster;
	int high, highprv = 1;

	if (unlikely(!bio->bi_io_vec))
		return;

	cluster = q->queue_flags & (1 << QUEUE_FLAG_CLUSTER);
	hw_seg_size = seg_size = nr_phys_segs = nr_hw_segs = 0;
	/* 遍历当前bio */
	bio_for_each_segment(bv, bio, i) {
		/*
		 * the trick here is making sure that a high page is never
		 * considered part of another segment, since that might
		 * change with the bounce page.
		 */
		high = page_to_pfn(bv->bv_page) >= q->bounce_pfn;/* 当前的page地址是否在高端内存中 */
		if (high || highprv)
			goto new_hw_segment;
		if (cluster) {
			if (seg_size + bv->bv_len > q->max_segment_size/*65536*/)
				goto new_segment;
			if (!BIOVEC_PHYS_MERGEABLE(bvprv, bv))/* 物理内存不相邻 */
				goto new_segment;
			if (!BIOVEC_SEG_BOUNDARY(q, bvprv, bv))
				goto new_segment;
			if (BIOVEC_VIRT_OVERSIZE(hw_seg_size + bv->bv_len))
				goto new_hw_segment;

			seg_size += bv->bv_len;
			hw_seg_size += bv->bv_len;
			bvprv = bv;
			continue;
		}
new_segment:/* bvprv与bv物理内存首尾相邻 */
		if (BIOVEC_VIRT_MERGEABLE(bvprv, bv) &&
		    !BIOVEC_VIRT_OVERSIZE(hw_seg_size + bv->bv_len)) {
			hw_seg_size += bv->bv_len;
		} else {/* bvpr与bv物理内存不相邻 */
new_hw_segment:
			if (hw_seg_size > bio->bi_hw_front_size)
				bio->bi_hw_front_size = hw_seg_size;
			hw_seg_size = BIOVEC_VIRT_START_SIZE(bv) + bv->bv_len;
			nr_hw_segs++;
		}

		nr_phys_segs++;
		bvprv = bv;
		seg_size = bv->bv_len;
		highprv = high;
	}
	if (hw_seg_size > bio->bi_hw_back_size)
		bio->bi_hw_back_size = hw_seg_size;
	if (nr_hw_segs == 1 && hw_seg_size > bio->bi_hw_front_size)
		bio->bi_hw_front_size = hw_seg_size;
	bio->bi_phys_segments = nr_phys_segs;/*当前bio对象所承载的数据在内存(虚拟)中的段数)*/
	bio->bi_hw_segments = nr_hw_segs;/*这个字段在高版本已经废弃,这里不关注其作用*/
	bio->bi_flags |= (1 << BIO_SEG_VALID);
}


static int blk_phys_contig_segment(request_queue_t *q, struct bio *bio,
				   struct bio *nxt)
{
	if (!(q->queue_flags & (1 << QUEUE_FLAG_CLUSTER)))
		return 0;

	if (!BIOVEC_PHYS_MERGEABLE(__BVEC_END(bio), __BVEC_START(nxt)))
		return 0;
	if (bio->bi_size + nxt->bi_size > q->max_segment_size)
		return 0;

	/*
	 * bio and nxt are contigous in memory, check if the queue allows
	 * these two to be merged into one
	 */
	if (BIO_SEG_BOUNDARY(q, bio, nxt))
		return 1;

	return 0;
}

static int blk_hw_contig_segment(request_queue_t *q, struct bio *bio,
				 struct bio *nxt)
{
	if (unlikely(!bio_flagged(bio, BIO_SEG_VALID)))
		blk_recount_segments(q, bio);
	if (unlikely(!bio_flagged(nxt, BIO_SEG_VALID)))
		blk_recount_segments(q, nxt);
	if (!BIOVEC_VIRT_MERGEABLE(__BVEC_END(bio), __BVEC_START(nxt)) ||
	    BIOVEC_VIRT_OVERSIZE(bio->bi_hw_front_size + bio->bi_hw_back_size))
		return 0;
	if (bio->bi_size + nxt->bi_size > q->max_segment_size)
		return 0;

	return 1;
}

/*
 * map a request to scatterlist, return number of sg entries setup. Caller
 * must make sure sg can hold rq->nr_phys_segments entries
 */
/**ltl
 * 功能:映射request与聚散列表
 * 参数:q	->请求队列
 *	rq	->请求对象
 *	sg	->指向一块聚散列表空间的地址。
 * 返回值:聚散列表的段数
 */
int blk_rq_map_sg(request_queue_t *q, struct request *rq, struct scatterlist *sg)
{
	struct bio_vec *bvec, *bvprv;
	struct bio *bio;
	int nsegs, i, cluster;

	nsegs = 0;
	cluster = q->queue_flags & (1 << QUEUE_FLAG_CLUSTER);

	/*
	 * for each bio in rq
	 */
	bvprv = NULL;
	rq_for_each_bio(bio, rq) {
		/*
		 * for each segment in bio
		 */
		bio_for_each_segment(bvec, bio, i) {
			int nbytes = bvec->bv_len;

			if (bvprv && cluster) {
				if (sg[nsegs - 1].length + nbytes > q->max_segment_size)
					goto new_segment;

				if (!BIOVEC_PHYS_MERGEABLE(bvprv, bvec))
					goto new_segment;
				if (!BIOVEC_SEG_BOUNDARY(q, bvprv, bvec))
					goto new_segment;

				sg[nsegs - 1].length += nbytes;
			} else {
new_segment:
				memset(&sg[nsegs],0,sizeof(struct scatterlist));
				sg[nsegs].page = bvec->bv_page;
				sg[nsegs].length = nbytes;
				sg[nsegs].offset = bvec->bv_offset;

				nsegs++;
			}
			bvprv = bvec;
		} /* segments in bio */
	} /* bios in rq */

	return nsegs;
}

EXPORT_SYMBOL(blk_rq_map_sg);

/*
 * the standard queue merge functions, can be overridden with device
 * specific ones if so desired
 */

static inline int ll_new_mergeable(request_queue_t *q,
				   struct request *req,
				   struct bio *bio)
{
	int nr_phys_segs = bio_phys_segments(q, bio);

	if (req->nr_phys_segments + nr_phys_segs > q->max_phys_segments) {
		req->flags |= REQ_NOMERGE;
		if (req == q->last_merge)
			q->last_merge = NULL;
		return 0;
	}

	/*
	 * A hw segment is just getting larger, bump just the phys
	 * counter.
	 */
	req->nr_phys_segments += nr_phys_segs;
	return 1;
}

static inline int ll_new_hw_segment(request_queue_t *q,
				    struct request *req,
				    struct bio *bio)
{
	int nr_hw_segs = bio_hw_segments(q, bio);
	int nr_phys_segs = bio_phys_segments(q, bio);

	if (req->nr_hw_segments + nr_hw_segs > q->max_hw_segments
	    || req->nr_phys_segments + nr_phys_segs > q->max_phys_segments) {
		req->flags |= REQ_NOMERGE;
		if (req == q->last_merge)
			q->last_merge = NULL;
		return 0;
	}

	/*
	 * This will form the start of a new hw segment.  Bump both
	 * counters.
	 */
	req->nr_hw_segments += nr_hw_segs;
	req->nr_phys_segments += nr_phys_segs;
	return 1;
}

/**ltl
功能: 判定bio是否能合入到req中的列表后部
参数:q	->请求队列
	req	->请求对象
	bio	->要合入的bio
说明:依两方面判断:1.请求的扇区总数是否超过请求队列的总扇区阀值。
			   2.请求内存段总数是否超过请求队列的内存段阀值。
*/
static int ll_back_merge_fn(request_queue_t *q, struct request *req, 
			    struct bio *bio)
{
	unsigned short max_sectors;
	int len;
	/*如果bio请求是scsi命令*/
	if (unlikely(blk_pc_request(req)))
		/* 硬件设备限制一次request所能处理的扇区数量。在SCSI设备，这个值为512
		   参见scsi_add_lun函数，对q->max_hw_sectors的设置		*/
		max_sectors = q->max_hw_sectors;
	else
		max_sectors = q->max_sectors;

	/* 如果req的扇区数与bio的扇区数和大于硬件所能处理的扇区数，则不能合入到request中。*/
	if (req->nr_sectors + bio_sectors(bio) > max_sectors) {
		req->flags |= REQ_NOMERGE;/* 标识当前的request为不可以合并 */
		if (req == q->last_merge)/* 最后一次合并的地址也要置空，否则在elv_merge就会判定出错 */
			q->last_merge = NULL;
		return 0;
	}
	/* 如果当前的request的列表尾bio没有置为有效，则要重置bio段值 */
	if (unlikely(!bio_flagged(req->biotail, BIO_SEG_VALID)))
		blk_recount_segments(q, req->biotail);
	/* 如果要合入的bio没有置为有效，则要重置bio段值 */
	if (unlikely(!bio_flagged(bio, BIO_SEG_VALID)))
		blk_recount_segments(q, bio);
	
	len = req->biotail->bi_hw_back_size + bio->bi_hw_front_size;
	if (BIOVEC_VIRT_MERGEABLE(__BVEC_END(req->biotail), __BVEC_START(bio)) &&
	    !BIOVEC_VIRT_OVERSIZE(len)) {
		int mergeable =  ll_new_mergeable(q, req, bio);

		if (mergeable) {
			if (req->nr_hw_segments == 1)
				req->bio->bi_hw_front_size = len;
			if (bio->bi_hw_segments == 1)
				bio->bi_hw_back_size = len;
		}
		return mergeable;
	}

	return ll_new_hw_segment(q, req, bio);
}

/**ltl
 * 功能: 判定bio是否能合入到req中的列表队头
 * 参数:q	->请求队列
 *	req	->请求对象
 *	bio	->要合入的bio
 */
static int ll_front_merge_fn(request_queue_t *q, struct request *req, 
			     struct bio *bio)
{
	unsigned short max_sectors;
	int len;
	/* 如果request是来自于内部命令，则最大的扇区数限定为硬件的限定扇区个数，否则就限定为1024或者更小 */
	if (unlikely(blk_pc_request(req)))
		max_sectors = q->max_hw_sectors;
	else
		max_sectors = q->max_sectors;

	/* 合并后的扇区数会超过限定的最大扇区数 */
	if (req->nr_sectors + bio_sectors(bio) > max_sectors) {
		req->flags |= REQ_NOMERGE;
		if (req == q->last_merge)
			q->last_merge = NULL;
		return 0;
	}
	len = bio->bi_hw_back_size + req->bio->bi_hw_front_size;
	if (unlikely(!bio_flagged(bio, BIO_SEG_VALID)))
		blk_recount_segments(q, bio);
	if (unlikely(!bio_flagged(req->bio, BIO_SEG_VALID)))
		blk_recount_segments(q, req->bio);
	if (BIOVEC_VIRT_MERGEABLE(__BVEC_END(bio), __BVEC_START(req->bio)) &&
	    !BIOVEC_VIRT_OVERSIZE(len)) {
		int mergeable =  ll_new_mergeable(q, req, bio);

		if (mergeable) {
			if (bio->bi_hw_segments == 1)
				bio->bi_hw_front_size = len;
			if (req->nr_hw_segments == 1)
				req->biotail->bi_hw_back_size = len;
		}
		return mergeable;
	}

	return ll_new_hw_segment(q, req, bio);
}

/**ltl
 * 功能:判定能否合并两个request
 * 参数:q	->请求队列
 *	req	->
 *	next	->
 * 返回值:0->不能合并
 *	 1->可以合并
 * 说明:
 *	两个request可以合并的两条件:
 *	1.请求的扇区总数是否超过请求队列的总扇区阀值。
 *	2.请求内存段总数是否超过请求队列的内存段阀值。
*/
static int ll_merge_requests_fn(request_queue_t *q, struct request *req,
				struct request *next)
{
	int total_phys_segments;
	int total_hw_segments;

	/*
	 * First check if the either of the requests are re-queued
	 * requests.  Can't merge them if they are.
	 */
	 /*如果有一个请求在派发队列中，则不能合并。即请求已经执行了，不能并之*/
	if (req->special || next->special)
		return 0;

	/*
	 * Will it become too large?
	 */
	 /* 两个请求的扇区数超过请求队列限定的最大扇区数 */
	if ((req->nr_sectors + next->nr_sectors) > q->max_sectors)
		return 0;

	/* nr_phys_segments字段作用???? */
	total_phys_segments = req->nr_phys_segments + next->nr_phys_segments;
	if (blk_phys_contig_segment(q, req->biotail, next->bio))
		total_phys_segments--;

	if (total_phys_segments > q->max_phys_segments)
		return 0;
	/* nr_hw_segments字段的作用???? */
	total_hw_segments = req->nr_hw_segments + next->nr_hw_segments;
	if (blk_hw_contig_segment(q, req->biotail, next->bio)) {
		int len = req->biotail->bi_hw_back_size + next->bio->bi_hw_front_size;
		/*
		 * propagate the combined length to the end of the requests
		 */
		if (req->nr_hw_segments == 1)
			req->bio->bi_hw_front_size = len;
		if (next->nr_hw_segments == 1)
			next->biotail->bi_hw_back_size = len;
		total_hw_segments--;
	}

	if (total_hw_segments > q->max_hw_segments)
		return 0;

	/* Merge is OK... */
	req->nr_phys_segments = total_phys_segments;
	req->nr_hw_segments = total_hw_segments;
	return 1;
}

/*
 * "plug" the device if there are no outstanding requests: this will
 * force the transfer to start only after we have put all the requests
 * on the list.
 *
 * This is called with interrupts off and no requests on the queue and
 * with the queue lock held.
 */
/**ltl
 * 功能:实现队列"畜流"，设置"畜流"标志，并开启" 泄流"的时间处理函数
 * 参数:q->请求队列
 */
void blk_plug_device(request_queue_t *q)
{
	/* 如果不处于中断禁止状态，则发出告警 */
	WARN_ON(!irqs_disabled());

	/*
	 * don't plug a stopped queue, it must be paired with blk_start_queue()
	 * which will restart the queueing
	 */
	 
	if (blk_queue_stopped(q))/* 如果请求队列已经停止，则直接退出 */
		return;

	/* 设置"畜流"标志，并返回旧的标志，如果旧标志没有"畜流"标志，则开启"泄流"定时器 */
	if (!test_and_set_bit(QUEUE_FLAG_PLUGGED, &q->queue_flags)) {
		mod_timer(&q->unplug_timer, jiffies + q->unplug_delay/*3ms*/);
		blk_add_trace_generic(q, NULL, 0, BLK_TA_PLUG);
	}
}

EXPORT_SYMBOL(blk_plug_device);

/*
 * remove the queue from the plugged list, if present. called with
 * queue lock held and interrupts disabled.
 */
int blk_remove_plug(request_queue_t *q)
{
	WARN_ON(!irqs_disabled());

	if (!test_and_clear_bit(QUEUE_FLAG_PLUGGED, &q->queue_flags))
		return 0;

	del_timer(&q->unplug_timer);
	return 1;
}

EXPORT_SYMBOL(blk_remove_plug);

/*
 * remove the plug and let it rip..
 */
/**ltl
功能:执行"泄流"功能
参数:q->请求队列
*/
void __generic_unplug_device(request_queue_t *q)
{
	/*如果请求队列已经停止，直接返回*/
	if (unlikely(blk_queue_stopped(q)))
		return;
	/*清除"畜流"标志位，并返回旧的标志值。如果旧值没有"畜流"标志，则直接返回*/
	if (!blk_remove_plug(q))
		return;
	/*调用底层驱动程序的策略处理函数*/
	q->request_fn(q);
}
EXPORT_SYMBOL(__generic_unplug_device);

/**
 * generic_unplug_device - fire a request queue
 * @q:    The &request_queue_t in question
 *
 * Description:
 *   Linux uses plugging to build bigger requests queues before letting
 *   the device have at them. If a queue is plugged, the I/O scheduler
 *   is still adding and merging requests on the queue. Once the queue
 *   gets unplugged, the request_fn defined for the queue is invoked and
 *   transfers started.
 **/
/**ltl
功能:请求队列"泄流"的处理函数，unplug_fn的值就是generic_unplug_device
参数:q->请求队列
*/
void generic_unplug_device(request_queue_t *q)
{
	spin_lock_irq(q->queue_lock);
	__generic_unplug_device(q);
	spin_unlock_irq(q->queue_lock);
}
EXPORT_SYMBOL(generic_unplug_device);

/**ltl
 * 功能: 请求队列的后备设备对象的接口
 * 参数:
 * 返回值:
 * 说明:
 */
static void blk_backing_dev_unplug(struct backing_dev_info *bdi,
				   struct page *page)
{
	request_queue_t *q = bdi->unplug_io_data;

	/*
	 * devices don't necessarily have an ->unplug_fn defined
	 */
	if (q->unplug_fn) {
		blk_add_trace_pdu_int(q, BLK_TA_UNPLUG_IO, NULL,
					q->rq.count[READ] + q->rq.count[WRITE]);

		q->unplug_fn(q); /* 请求队列的"泄流"(generic_unplug_device) */
	}
}
/**ltl
 * 功能:工作队列的处理函数
 * 参数:data	->请求队列地址
 */
static void blk_unplug_work(void *data)
{
	request_queue_t *q = data;

	blk_add_trace_pdu_int(q, BLK_TA_UNPLUG_IO, NULL,
				q->rq.count[READ] + q->rq.count[WRITE]);
	/* "泄流"处理函数，调用generic_unplug_device */
	q->unplug_fn(q);
}

/**ltl
 * 功能:"泄流"定时器的处理函数
 * 参数:data	->请求队列地址
 */
static void blk_unplug_timeout(unsigned long data)
{
	request_queue_t *q = (request_queue_t *)data;

	blk_add_trace_pdu_int(q, BLK_TA_UNPLUG_TIMER, NULL,
				q->rq.count[READ] + q->rq.count[WRITE]);

	kblockd_schedule_work(&q->unplug_work);/* 执行"泄流"工作队列 */
}

/**
 * blk_start_queue - restart a previously stopped queue
 * @q:    The &request_queue_t in question
 *
 * Description:
 *   blk_start_queue() will clear the stop flag on the queue, and call
 *   the request_fn for the queue if it was in a stopped state when
 *   entered. Also see blk_stop_queue(). Queue lock must be held.
 **/
void blk_start_queue(request_queue_t *q)
{
	WARN_ON(!irqs_disabled());

	clear_bit(QUEUE_FLAG_STOPPED, &q->queue_flags);

	/*
	 * one level of recursion is ok and is much faster than kicking
	 * the unplug handling
	 */
	if (!test_and_set_bit(QUEUE_FLAG_REENTER, &q->queue_flags)) {
		q->request_fn(q);
		clear_bit(QUEUE_FLAG_REENTER, &q->queue_flags);
	} else {
		blk_plug_device(q);
		kblockd_schedule_work(&q->unplug_work);
	}
}

EXPORT_SYMBOL(blk_start_queue);

/**
 * blk_stop_queue - stop a queue
 * @q:    The &request_queue_t in question
 *
 * Description:
 *   The Linux block layer assumes that a block driver will consume all
 *   entries on the request queue when the request_fn strategy is called.
 *   Often this will not happen, because of hardware limitations (queue
 *   depth settings). If a device driver gets a 'queue full' response,
 *   or if it simply chooses not to queue more I/O at one point, it can
 *   call this function to prevent the request_fn from being called until
 *   the driver has signalled it's ready to go again. This happens by calling
 *   blk_start_queue() to restart queue operations. Queue lock must be held.
 **/
void blk_stop_queue(request_queue_t *q)
{
	blk_remove_plug(q);
	set_bit(QUEUE_FLAG_STOPPED, &q->queue_flags);
}
EXPORT_SYMBOL(blk_stop_queue);

/**
 * blk_sync_queue - cancel any pending callbacks on a queue
 * @q: the queue
 *
 * Description:
 *     The block layer may perform asynchronous callback activity
 *     on a queue, such as calling the unplug function after a timeout.
 *     A block device may call blk_sync_queue to ensure that any
 *     such activity is cancelled, thus allowing it to release resources
 *     the the callbacks might use. The caller must already have made sure
 *     that its ->make_request_fn will not re-add plugging prior to calling
 *     this function.
 *
 */
void blk_sync_queue(struct request_queue *q)
{
	del_timer_sync(&q->unplug_timer);
	kblockd_flush();
}
EXPORT_SYMBOL(blk_sync_queue);

/**
 * blk_run_queue - run a single device queue
 * @q:	The queue to run
 */
void blk_run_queue(struct request_queue *q)
{
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	blk_remove_plug(q);

	/*
	 * Only recurse once to avoid overrunning the stack, let the unplug
	 * handling reinvoke the handler shortly if we already got there.
	 */
	if (!elv_queue_empty(q)) {
		if (!test_and_set_bit(QUEUE_FLAG_REENTER, &q->queue_flags)) {
			q->request_fn(q);
			clear_bit(QUEUE_FLAG_REENTER, &q->queue_flags);
		} else {
			blk_plug_device(q);
			kblockd_schedule_work(&q->unplug_work);
		}
	}

	spin_unlock_irqrestore(q->queue_lock, flags);
}
EXPORT_SYMBOL(blk_run_queue);

/**
 * blk_cleanup_queue: - release a &request_queue_t when it is no longer needed
 * @kobj:    the kobj belonging of the request queue to be released
 *
 * Description:
 *     blk_cleanup_queue is the pair to blk_init_queue() or
 *     blk_queue_make_request().  It should be called when a request queue is
 *     being released; typically when a block device is being de-registered.
 *     Currently, its primary task it to free all the &struct request
 *     structures that were allocated to the queue and the queue itself.
 *
 * Caveat:
 *     Hopefully the low level driver will have finished any
 *     outstanding requests first...
 **/
static void blk_release_queue(struct kobject *kobj)
{
	request_queue_t *q = container_of(kobj, struct request_queue, kobj);
	struct request_list *rl = &q->rq;

	blk_sync_queue(q);

	if (rl->rq_pool)
		mempool_destroy(rl->rq_pool);

	if (q->queue_tags)
		__blk_queue_free_tags(q);

	if (q->blk_trace)
		blk_trace_shutdown(q);

	kmem_cache_free(requestq_cachep, q);
}

void blk_put_queue(request_queue_t *q)
{
	kobject_put(&q->kobj);
}
EXPORT_SYMBOL(blk_put_queue);

void blk_cleanup_queue(request_queue_t * q)
{
	mutex_lock(&q->sysfs_lock);
	set_bit(QUEUE_FLAG_DEAD, &q->queue_flags);
	mutex_unlock(&q->sysfs_lock);

	if (q->elevator)
		elevator_exit(q->elevator);

	blk_put_queue(q);
}

EXPORT_SYMBOL(blk_cleanup_queue);

static int blk_init_free_list(request_queue_t *q)
{
	struct request_list *rl = &q->rq;

	rl->count[READ] = rl->count[WRITE] = 0;
	rl->starved[READ] = rl->starved[WRITE] = 0;
	rl->elvpriv = 0;
	init_waitqueue_head(&rl->wait[READ]);
	init_waitqueue_head(&rl->wait[WRITE]);

	rl->rq_pool = mempool_create_node(BLKDEV_MIN_RQ, mempool_alloc_slab,
				mempool_free_slab, request_cachep, q->node);

	if (!rl->rq_pool)
		return -ENOMEM;

	return 0;
}

request_queue_t *blk_alloc_queue(gfp_t gfp_mask)
{
	return blk_alloc_queue_node(gfp_mask, -1);
}
EXPORT_SYMBOL(blk_alloc_queue);

static struct kobj_type queue_ktype;

/**ltl
 * 功能:在指定的内存节点分配请求队列
 * 参数:gfp_mask	->与kmalloc的分配标志一样。
 *	node_id	->内存节点编号
 * 返回值:申请的请求队列的对象。
 */
request_queue_t *blk_alloc_queue_node(gfp_t gfp_mask, int node_id)
{
	request_queue_t *q;
	/* 分配请求队列对象 */
	q = kmem_cache_alloc_node(requestq_cachep, gfp_mask, node_id);
	if (!q)
		return NULL;

	memset(q, 0, sizeof(*q));
	init_timer(&q->unplug_timer);/* 初始化"泄流"定时器 */

	/* 设置内嵌的kobject对象的名字，并初始化kobject对象 */
	snprintf(q->kobj.name, KOBJ_NAME_LEN, "%s", "queue");
	q->kobj.ktype = &queue_ktype;
	kobject_init(&q->kobj);
	/* 设置请求队列的后备设备信息操作接口 */
	q->backing_dev_info.unplug_io_fn = blk_backing_dev_unplug;
	q->backing_dev_info.unplug_io_data = q;

	mutex_init(&q->sysfs_lock);

	return q;
}
EXPORT_SYMBOL(blk_alloc_queue_node);

/**
 * blk_init_queue  - prepare a request queue for use with a block device
 * @rfn:  The function to be called to process requests that have been
 *        placed on the queue.
 * @lock: Request queue spin lock
 *
 * Description:
 *    If a block device wishes to use the standard request handling procedures,
 *    which sorts requests and coalesces adjacent requests, then it must
 *    call blk_init_queue().  The function @rfn will be called when there
 *    are requests on the queue that need to be processed.  If the device
 *    supports plugging, then @rfn may not be called immediately when requests
 *    are available on the queue, but may be called at some time later instead.
 *    Plugged queues are generally unplugged when a buffer belonging to one
 *    of the requests on the queue is needed, or due to memory pressure.
 *
 *    @rfn is not required, or even expected, to remove all requests off the
 *    queue, but only as many as it can handle at a time.  If it does leave
 *    requests on the queue, it is responsible for arranging that the requests
 *    get dealt with eventually.
 *
 *    The queue spin lock must be held while manipulating the requests on the
 *    request queue; this lock will be taken also from interrupt context, so irq
 *    disabling is needed for it.
 *
 *    Function returns a pointer to the initialized request queue, or NULL if
 *    it didn't succeed.
 *
 * Note:
 *    blk_init_queue() must be paired with a blk_cleanup_queue() call
 *    when the block device is deactivated (such as at module unload).
 **/
/**ltl
 * 功能: 分配请求队列
 * 参数: rfn->与设备相关的策略处理函数，这个函数由编写设备驱动程序完成，主要用来对物理设备读写。
 *	 lock->请求队列的锁，这个值可以为空，因为在request_queue会默认一个锁
 * 返回值:申请的请求队列的对象。
 * 说明: 在编写驱动程序时，主要调用这个函数来分配请求队列
 */
request_queue_t *blk_init_queue(request_fn_proc *rfn, spinlock_t *lock)
{
	return blk_init_queue_node(rfn, lock, -1);
}
EXPORT_SYMBOL(blk_init_queue);

/**ltl
 * 功能:在指定的内存节点分配请求队列,并对部分成员做初始化
 * 参数:rfn	->与设备相关的策略处理函数，这个函数由编写设备驱动程序完成，主要用来对物理设备读写。
 *	lock		->请求队列的锁，这个值可以为空，因为在request_queue会默认一个锁
 *	node_id	->内存节点编号
 * 返回值:申请的请求队列的对象。
 */
request_queue_t *
blk_init_queue_node(request_fn_proc *rfn, spinlock_t *lock, int node_id)
{
	/* 从高速缓冲区中分配request_queue对象 */
	request_queue_t *q = blk_alloc_queue_node(GFP_KERNEL, node_id);

	if (!q)
		return NULL;

	q->node = node_id;
	/* 初始化request_list */
	if (blk_init_free_list(q)) {
		kmem_cache_free(requestq_cachep, q);
		return NULL;
	}

	/*
	 * if caller didn't supply a lock, they get per-queue locking with
	 * our embedded lock
	 */
	if (!lock) {
		spin_lock_init(&q->__queue_lock);
		lock = &q->__queue_lock;
	}
	
	q->request_fn		= rfn;/* 设备驱动程序的策略处理函数 */
	q->back_merge_fn       	= ll_back_merge_fn;	/* 判定bio能否合入到request中列表的尾部*/
	q->front_merge_fn      	= ll_front_merge_fn;/* 判定bio能否合入到request中列表的头部*/
	q->merge_requests_fn	= ll_merge_requests_fn;/* 合并两个request */
	/* 构造SCSI命令的方法。例如:针对磁盘驱动，如果在没有被高层驱动绑定，这个函数一般是scsi_prep_fn,如果被绑定的话，就设置成sd_prep_fn */
	q->prep_rq_fn		= NULL;		
	q->unplug_fn		= generic_unplug_device;	/* 请求队列的"泄流"接口 */
	q->queue_flags		= (1 << QUEUE_FLAG_CLUSTER);
	q->queue_lock		= lock;

	blk_queue_segment_boundary(q, 0xffffffff);
	
	/* 设置块设备驱动程序的request请求构造处理函数，并设置request_queue的限制属性 */
	blk_queue_make_request(q, __make_request);
	blk_queue_max_segment_size(q, MAX_SEGMENT_SIZE);/* 一个段的最大字节数 */

	blk_queue_max_hw_segments(q, MAX_HW_SEGMENTS);
	blk_queue_max_phys_segments(q, MAX_PHYS_SEGMENTS);

	/*
	 * all done
	 */
	 /* 把请求队列与IO调度算法相关联 */
	if (!elevator_init(q, NULL)) {
		blk_queue_congestion_threshold(q);
		return q;
	}

	blk_put_queue(q);
	return NULL;
}
EXPORT_SYMBOL(blk_init_queue_node);

int blk_get_queue(request_queue_t *q)
{
	if (likely(!test_bit(QUEUE_FLAG_DEAD, &q->queue_flags))) {
		kobject_get(&q->kobj);
		return 0;
	}

	return 1;
}

EXPORT_SYMBOL(blk_get_queue);
/**ltl
功能:如果request关联的与IO调度算法相关数据结构，刚先释放它，再释放reqeust本身
参数:q	->请求队列
	rq	->释放请求对象
*/
static inline void blk_free_request(request_queue_t *q, struct request *rq)
{
	if (rq->flags & REQ_ELVPRIV)
		elv_put_request(q, rq);/* 如果存在与IO调度器相关的数据，释放 */
	mempool_free(rq, q->rq.rq_pool);/* 释放request的内存空间 */
}
/**ltl
 * 功能:分配request对象，同时去分配与调试器有关的私有数据成员
 */
static inline struct request *
blk_alloc_request(request_queue_t *q, int rw, struct bio *bio,
		  int priv, gfp_t gfp_mask)
{
	/* 在缓冲区池中分配request对象 */
	struct request *rq = mempool_alloc(q->rq.rq_pool, gfp_mask);

	if (!rq)
		return NULL;

	/*
	 * first three bits are identical in rq->flags and bio->bi_rw,
	 * see bio.h and blkdev.h
	 */
	rq->flags = rw;

	if (priv) {/* 分配IO调度器的私有数据成员elevator_private，同时设置REQ_ELVPRIV标志，表示这个request有私有数据成员  */
		if (unlikely(elv_set_request(q, rq, bio, gfp_mask))) {
			mempool_free(rq, q->rq.rq_pool);
			return NULL;
		}
		/* 设置调度算法的私有数据标志(很重要，会影响到request插入的位置,见:__elv_add_request()) */
		rq->flags |= REQ_ELVPRIV;
	}

	return rq;
}

/*
 * ioc_batching returns true if the ioc is a valid batching request and
 * should be given priority access to a request.
 */
static inline int ioc_batching(request_queue_t *q, struct io_context *ioc)
{
	if (!ioc)
		return 0;

	/*
	 * Make sure the process is able to allocate at least 1 request
	 * even if the batch times out, otherwise we could theoretically
	 * lose wakeups.
	 */
	return ioc->nr_batch_requests == q->nr_batching ||
		(ioc->nr_batch_requests > 0
		&& time_before(jiffies, ioc->last_waited + BLK_BATCH_TIME));
}

/*
 * ioc_set_batching sets ioc to be a new "batcher" if it is not one. This
 * will cause the process to be a "batcher" on all queues in the system. This
 * is the behaviour we want though - once it gets a wakeup it should be given
 * a nice run.
 */
static void ioc_set_batching(request_queue_t *q, struct io_context *ioc)
{
	if (!ioc || ioc_batching(q, ioc))
		return;

	ioc->nr_batch_requests = q->nr_batching;
	ioc->last_waited = jiffies;
}
/* 主要功能是唤醒等待分配request内存的进程 */
static void __freed_request(request_queue_t *q, int rw)
{
	struct request_list *rl = &q->rq;

	if (rl->count[rw] < queue_congestion_off_threshold(q))
		clear_queue_congested(q, rw);

	if (rl->count[rw] + 1 <= q->nr_requests) {
		if (waitqueue_active(&rl->wait[rw]))
			wake_up(&rl->wait[rw]);/* 唤醒等待的进程 */

		blk_clear_queue_full(q, rw);
	}
}

/*
 * A request has just been released.  Account for it, update the full and
 * congestion status, wake up any waiters.   Called under q->queue_lock.
 */
/**ltl
 * 功能:由于request的对象已经释放，则可以去改变引用引用记数器和唤醒等待申请request的进程。
 * 参数:q	->请求队列
 *	rw	->READ/WRITE
 *	priv	->是否有与IO调度算法相关的私有数据
 */
static void freed_request(request_queue_t *q, int rw, int priv)
{
	struct request_list *rl = &q->rq;

	rl->count[rw]--;/* 释放request对象时，就把计数器-1 */
	if (priv)
		rl->elvpriv--;
	/* 唤醒等待进程 */
	__freed_request(q, rw);

	if (unlikely(rl->starved[rw ^ 1])) /* 只对CFQ算法才有用。(get_request) */
		__freed_request(q, rw ^ 1);
}

#define blkdev_free_rq(list) list_entry((list)->next, struct request, queuelist)
/*
 * Get a free request, queue_lock must be held.
 * Returns NULL on failure, with queue_lock held.
 * Returns !NULL on success, with queue_lock *not held*.
 */
/**ltl
 * 功能:调用mempool_alloc分配request对象，分配request对象有可能失败
 * 参数:q	->请求队列
 *	rw	->READ/WRITE
 *	bio	->
 * 返回值:
 */
static struct request *get_request(request_queue_t *q, int rw, struct bio *bio,
				   gfp_t gfp_mask)
{
	struct request *rq = NULL;
	struct request_list *rl = &q->rq;
	struct io_context *ioc = NULL;
	int may_queue, priv;

	may_queue = elv_may_queue(q, rw, bio);/*只对CFQ IO调度算法有用，先不考虑*/
	if (may_queue == ELV_MQUEUE_NO)
		goto rq_starved;
	/* 拥塞控制 */
	if (rl->count[rw]+1 >= queue_congestion_on_threshold(q)/*113*/) {
		if (rl->count[rw]+1 >= q->nr_requests/*128*/) {
			ioc = current_io_context(GFP_ATOMIC);
			/*
			 * The queue will fill after this allocation, so set
			 * it as full, and mark this process as "batching".
			 * This process will be allowed to complete a batch of
			 * requests, others will be blocked.
			 */
			if (!blk_queue_full(q, rw)) {
				ioc_set_batching(q, ioc);
				blk_set_queue_full(q, rw);
			} else {
				if (may_queue != ELV_MQUEUE_MUST
						&& !ioc_batching(q, ioc)) {
					/*
					 * The queue is full and the allocating
					 * process is not a "batcher", and not
					 * exempted by the IO scheduler
					 */
					goto out;
				}
			}
		}
		set_queue_congested(q, rw);
	}

	/*
	 * Only allow batching queuers to allocate up to 50% over the defined
	 * limit of requests, otherwise we could have thousands of requests
	 * allocated with any setting of ->nr_requests
	 */
	if (rl->count[rw] >= (3 * q->nr_requests / 2))
		goto out;

	rl->count[rw]++;/* 在申请request对象时，记数器+1 */
	rl->starved[rw] = 0;
	
	/* 没有设置调度算法切换开关，在elevator_switch()中设置 */
	priv = !test_bit(QUEUE_FLAG_ELVSWITCH, &q->queue_flags);
	if (priv)
		rl->elvpriv++;

	spin_unlock_irq(q->queue_lock);

	/* 分配request对象 */
	rq = blk_alloc_request(q, rw, bio, priv, gfp_mask);
	if (unlikely(!rq)) {
		/*
		 * Allocation failed presumably due to memory. Undo anything
		 * we might have messed up.
		 *
		 * Allocating task should really be put onto the front of the
		 * wait queue, but this is pretty rare.
		 */
		spin_lock_irq(q->queue_lock);
		freed_request(q, rw, priv);

		/*
		 * in the very unlikely event that allocation failed and no
		 * requests for this direction was pending, mark us starved
		 * so that freeing of a request in the other direction will
		 * notice us. another possible fix would be to split the
		 * rq mempool into READ and WRITE
		 */
rq_starved:/* 只对CFQ有用 */
		if (unlikely(rl->count[rw] == 0))
			rl->starved[rw] = 1;

		goto out;
	}

	/*
	 * ioc may be NULL here, and ioc_batching will be false. That's
	 * OK, if the queue is under the request limit then requests need
	 * not count toward the nr_batch_requests limit. There will always
	 * be some limit enforced by BLK_BATCH_TIME.
	 */
	if (ioc_batching(q, ioc))
		ioc->nr_batch_requests--;
	/* 初始化request对象 */
	rq_init(q, rq);
	rq->rl = rl;

	blk_add_trace_generic(q, bio, rw, BLK_TA_GETRQ);
out:
	return rq;
}

/*
 * No available requests for this queue, unplug the device and wait for some
 * requests to become available.
 *
 * Called with q->queue_lock held, and returns with it unlocked.
 */
/**ltl
 * 功能:申请一个新的request对象
 * 参数:q	->请求队列
 *	rw	->读写方向
 *	bio	->要提交给底层驱动的bio对象
 * 返回值:request对象
 */
static struct request *get_request_wait(request_queue_t *q, int rw,
					struct bio *bio)
{
	struct request *rq;

	rq = get_request(q, rw, bio, GFP_NOIO);
	while (!rq) {/* 如果分配request失败，说明系统中没有多余内存，要先泄流，去释放现有的request空间，并睡眠当前进程等待 */
		DEFINE_WAIT(wait);
		struct request_list *rl = &q->rq;

		/* 把当前进程加入到等待队列(还没有进入睡眠) */
		prepare_to_wait_exclusive(&rl->wait[rw], &wait,
				TASK_UNINTERRUPTIBLE);
		/* 现次去获取request */
		rq = get_request(q, rw, bio, GFP_NOIO);

		if (!rq) {
			struct io_context *ioc;
			/* 把bio加入到跟踪列表中 */
			blk_add_trace_generic(q, bio, rw, BLK_TA_SLEEPRQ);
			/* "泄流"等待列表 */
			__generic_unplug_device(q);
			spin_unlock_irq(q->queue_lock);
			io_schedule();/* 进程调度,与schedule()区别是让CPU调度算法知道是由于IO引起的休眠 */

			/*
			 * After sleeping, we become a "batching" process and
			 * will be able to allocate at least one request, and
			 * up to a big batch of them for a small period time.
			 * See ioc_batching, ioc_set_batching
			 */
			ioc = current_io_context(GFP_NOIO);
			ioc_set_batching(q, ioc);

			spin_lock_irq(q->queue_lock);
		}
		finish_wait(&rl->wait[rw], &wait);
	}

	return rq;
}
/**ltl
 * 功能:申请新的request对象；这个函数可以实现两个功能:1.根据分配标志__GFP_WAIT分配request一定要成功，如果在分配时没有成功，则会引起睡眠；
 *									2.分配可能失败
 * 参数: q		->请求队列
 *	rw		->READ/WRITE
 *	gfp_mask	->分配内存的标志
 * 返回值:NULL	分配失败
 *	!NULL 分配成功
 */
struct request *blk_get_request(request_queue_t *q, int rw, gfp_t gfp_mask)
{
	struct request *rq;

	BUG_ON(rw != READ && rw != WRITE);

	spin_lock_irq(q->queue_lock);
	if (gfp_mask & __GFP_WAIT) {/* 用等待标志分配request对象，分配不会失败 */
		rq = get_request_wait(q, rw, NULL);
	} else {/* 用一般的分配标志去分配内存，有可能失败 */
		rq = get_request(q, rw, NULL, gfp_mask);
		if (!rq)
			spin_unlock_irq(q->queue_lock);
	}
	/* q->queue_lock is unlocked at this point */

	return rq;
}
EXPORT_SYMBOL(blk_get_request);

/**
 * blk_requeue_request - put a request back on queue
 * @q:		request queue where request should be inserted
 * @rq:		request to be inserted
 *
 * Description:
 *    Drivers often keep queueing requests until the hardware cannot accept
 *    more, when that condition happens we need to put the request back
 *    on the queue. Must be called with queue lock held.
 */
/**ltl
 * 功能:由于对rq处理失败(主要原因是物理设备还没有准备好处理命令)，把rq重新插入到请求队列中
 * 参数:q	->请求队列
 *	rq	->要插入到请求队列的请求
 */
void blk_requeue_request(request_queue_t *q, struct request *rq)
{
	blk_add_trace_rq(q, rq, BLK_TA_REQUEUE);

	if (blk_rq_tagged(rq))
		blk_queue_end_tag(q, rq);
	/* 把请求重新插入到派发队列中，不经过IO调度器。 */
	elv_requeue_request(q, rq);
}

EXPORT_SYMBOL(blk_requeue_request);

/**
 * blk_insert_request - insert a special request in to a request queue
 * @q:		request queue where request should be inserted
 * @rq:		request to be inserted
 * @at_head:	insert request at head or tail of queue
 * @data:	private data
 *
 * Description:
 *    Many block devices need to execute commands asynchronously, so they don't
 *    block the whole kernel from preemption during request execution.  This is
 *    accomplished normally by inserting aritficial requests tagged as
 *    REQ_SPECIAL in to the corresponding request queue, and letting them be
 *    scheduled for actual execution by the request queue.
 *
 *    We have the option of inserting the head or the tail of the queue.
 *    Typically we use the tail for new ioctls and so forth.  We use the head
 *    of the queue for things like a QUEUE_FULL message from a device, or a
 *    host that is unable to accept a particular command.
 */
void blk_insert_request(request_queue_t *q, struct request *rq,
			int at_head, void *data)
{
	int where = at_head ? ELEVATOR_INSERT_FRONT : ELEVATOR_INSERT_BACK;
	unsigned long flags;

	/*
	 * tell I/O scheduler that this isn't a regular read/write (ie it
	 * must not attempt merges on this) and that it acts as a soft
	 * barrier
	 */
	rq->flags |= REQ_SPECIAL | REQ_SOFTBARRIER;

	rq->special = data;

	spin_lock_irqsave(q->queue_lock, flags);

	/*
	 * If command is tagged, release the tag
	 */
	if (blk_rq_tagged(rq))
		blk_queue_end_tag(q, rq);

	drive_stat_acct(rq, rq->nr_sectors, 1);
	__elv_add_request(q, rq, where, 0);

	if (blk_queue_plugged(q))
		__generic_unplug_device(q);
	else
		q->request_fn(q);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

EXPORT_SYMBOL(blk_insert_request);

/**
 * blk_rq_map_user - map user data to a request, for REQ_BLOCK_PC usage
 * @q:		request queue where request should be inserted
 * @rq:		request structure to fill
 * @ubuf:	the user buffer
 * @len:	length of user data
 *
 * Description:
 *    Data will be mapped directly for zero copy io, if possible. Otherwise
 *    a kernel bounce buffer is used.
 *
 *    A matching blk_rq_unmap_user() must be issued at the end of io, while
 *    still in process context.
 *
 *    Note: The mapped bio may need to be bounced through blk_queue_bounce()
 *    before being submitted to the device, as pages mapped may be out of
 *    reach. It's the callers responsibility to make sure this happens. The
 *    original bio must be passed back in to blk_rq_unmap_user() for proper
 *    unmapping.
 */
int blk_rq_map_user(request_queue_t *q, struct request *rq, void __user *ubuf,
		    unsigned int len)
{
	unsigned long uaddr;
	struct bio *bio;
	int reading;

	if (len > (q->max_hw_sectors << 9))
		return -EINVAL;
	if (!len || !ubuf)
		return -EINVAL;

	reading = rq_data_dir(rq) == READ;

	/*
	 * if alignment requirement is satisfied, map in user pages for
	 * direct dma. else, set up kernel bounce buffers
	 */
	uaddr = (unsigned long) ubuf;
	if (!(uaddr & queue_dma_alignment(q)) && !(len & queue_dma_alignment(q)))
		bio = bio_map_user(q, NULL, uaddr, len, reading);
	else
		bio = bio_copy_user(q, uaddr, len, reading);

	if (!IS_ERR(bio)) {
		rq->bio = rq->biotail = bio;
		blk_rq_bio_prep(q, rq, bio);

		rq->buffer = rq->data = NULL;
		rq->data_len = len;
		return 0;
	}

	/*
	 * bio is the err-ptr
	 */
	return PTR_ERR(bio);
}

EXPORT_SYMBOL(blk_rq_map_user);

/**
 * blk_rq_map_user_iov - map user data to a request, for REQ_BLOCK_PC usage
 * @q:		request queue where request should be inserted
 * @rq:		request to map data to
 * @iov:	pointer to the iovec
 * @iov_count:	number of elements in the iovec
 *
 * Description:
 *    Data will be mapped directly for zero copy io, if possible. Otherwise
 *    a kernel bounce buffer is used.
 *
 *    A matching blk_rq_unmap_user() must be issued at the end of io, while
 *    still in process context.
 *
 *    Note: The mapped bio may need to be bounced through blk_queue_bounce()
 *    before being submitted to the device, as pages mapped may be out of
 *    reach. It's the callers responsibility to make sure this happens. The
 *    original bio must be passed back in to blk_rq_unmap_user() for proper
 *    unmapping.
 */
int blk_rq_map_user_iov(request_queue_t *q, struct request *rq,
			struct sg_iovec *iov, int iov_count)
{
	struct bio *bio;

	if (!iov || iov_count <= 0)
		return -EINVAL;

	/* we don't allow misaligned data like bio_map_user() does.  If the
	 * user is using sg, they're expected to know the alignment constraints
	 * and respect them accordingly */
	bio = bio_map_user_iov(q, NULL, iov, iov_count, rq_data_dir(rq)== READ);
	if (IS_ERR(bio))
		return PTR_ERR(bio);

	rq->bio = rq->biotail = bio;
	blk_rq_bio_prep(q, rq, bio);
	rq->buffer = rq->data = NULL;
	rq->data_len = bio->bi_size;
	return 0;
}

EXPORT_SYMBOL(blk_rq_map_user_iov);

/**
 * blk_rq_unmap_user - unmap a request with user data
 * @bio:	bio to be unmapped
 * @ulen:	length of user buffer
 *
 * Description:
 *    Unmap a bio previously mapped by blk_rq_map_user().
 */
int blk_rq_unmap_user(struct bio *bio, unsigned int ulen)
{
	int ret = 0;

	if (bio) {
		if (bio_flagged(bio, BIO_USER_MAPPED))
			bio_unmap_user(bio);
		else
			ret = bio_uncopy_user(bio);
	}

	return 0;
}

EXPORT_SYMBOL(blk_rq_unmap_user);

/**
 * blk_rq_map_kern - map kernel data to a request, for REQ_BLOCK_PC usage
 * @q:		request queue where request should be inserted
 * @rq:		request to fill
 * @kbuf:	the kernel buffer
 * @len:	length of user data
 * @gfp_mask:	memory allocation flags
 */
/**ltl
 * 功能:映射一数据buffer与request对象
 * 参数:
 * 返回值:
 */
int blk_rq_map_kern(request_queue_t *q, struct request *rq, void *kbuf,
		    unsigned int len, gfp_t gfp_mask)
{
	struct bio *bio;

	if (len > (q->max_hw_sectors << 9))
		return -EINVAL;
	if (!len || !kbuf)
		return -EINVAL;

	bio = bio_map_kern(q, kbuf, len, gfp_mask);
	if (IS_ERR(bio))
		return PTR_ERR(bio);

	if (rq_data_dir(rq) == WRITE)
		bio->bi_rw |= (1 << BIO_RW);

	rq->bio = rq->biotail = bio;
	blk_rq_bio_prep(q, rq, bio);

	rq->buffer = rq->data = NULL;
	rq->data_len = len;
	return 0;
}

EXPORT_SYMBOL(blk_rq_map_kern);

/**
 * blk_execute_rq_nowait - insert a request into queue for execution
 * @q:		queue to insert the request in
 * @bd_disk:	matching gendisk
 * @rq:		request to insert
 * @at_head:    insert request at head or tail of queue
 * @done:	I/O completion handler
 *
 * Description:
 *    Insert a fully prepared request at the back of the io scheduler queue
 *    for execution.  Don't wait for completion.
 */
/**ltl
 * 功能:把scsi内部分配直接插入到派发队列中，并发给底层驱动
 * 说明:只执行scsi命令，request->flags:设置REQ_BLOCK_PC和REQ_NOMERGE
 */
void blk_execute_rq_nowait(request_queue_t *q, struct gendisk *bd_disk,
			   struct request *rq, int at_head,
			   rq_end_io_fn *done)
{
	int where = at_head ? ELEVATOR_INSERT_FRONT : ELEVATOR_INSERT_BACK;

	rq->rq_disk = bd_disk;
	rq->flags |= REQ_NOMERGE;
	rq->end_io = done;
	WARN_ON(irqs_disabled());
	spin_lock_irq(q->queue_lock);
	/* 把请求插入到派发队列(队头)中 */
	__elv_add_request(q, rq, where, 1);
	__generic_unplug_device(q);/* "泄流" */
	spin_unlock_irq(q->queue_lock);
}
EXPORT_SYMBOL_GPL(blk_execute_rq_nowait);

/**
 * blk_execute_rq - insert a request into queue for execution
 * @q:		queue to insert the request in
 * @bd_disk:	matching gendisk
 * @rq:		request to insert
 * @at_head:    insert request at head or tail of queue
 *
 * Description:
 *    Insert a fully prepared request at the back of the io scheduler queue
 *    for execution and wait for completion.
 */
/**ltl
 * 功能:由SCSI子系统层调用，执行SCSI命令
 * 参数:q	->请求队列
 *	bd_disk->关联的gendisk对象
 *	rq	->请求对象
 *	at_head->请求插入到派发队列的位置:头部或者尾部
 * 返回值:
*/
int blk_execute_rq(request_queue_t *q, struct gendisk *bd_disk,
		   struct request *rq, int at_head)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	char sense[SCSI_SENSE_BUFFERSIZE];
	int err = 0;

	/*
	 * we need an extra reference to the request, so we can look at
	 * it after io completion
	 */
	rq->ref_count++;

	if (!rq->sense) {
		memset(sense, 0, sizeof(sense));
		rq->sense = sense;
		rq->sense_len = 0;
	}
	/* 当request是来自于scsi层，则此等到完全执行结束后，程序才可以返回，kernel运用completion机制来实现此功能。*/
	rq->waiting = &wait;
	blk_execute_rq_nowait(q, bd_disk, rq, at_head, blk_end_sync_rq);/* 执行命令 */
	wait_for_completion(&wait);/* 等待命令执行结束 */
	rq->waiting = NULL;

	if (rq->errors)
		err = -EIO;

	return err;
}

EXPORT_SYMBOL(blk_execute_rq);

/**
 * blkdev_issue_flush - queue a flush
 * @bdev:	blockdev to issue flush for
 * @error_sector:	error sector
 *
 * Description:
 *    Issue a flush for the block device in question. Caller can supply
 *    room for storing the error offset in case of a flush error, if they
 *    wish to.  Caller must run wait_for_completion() on its own.
 */
int blkdev_issue_flush(struct block_device *bdev, sector_t *error_sector)
{
	request_queue_t *q;

	if (bdev->bd_disk == NULL)
		return -ENXIO;

	q = bdev_get_queue(bdev);
	if (!q)
		return -ENXIO;
	if (!q->issue_flush_fn)
		return -EOPNOTSUPP;

	return q->issue_flush_fn(q, bdev->bd_disk, error_sector);
}

EXPORT_SYMBOL(blkdev_issue_flush);

static void drive_stat_acct(struct request *rq, int nr_sectors, int new_io)
{
	int rw = rq_data_dir(rq);

	if (!blk_fs_request(rq) || !rq->rq_disk)
		return;

	if (!new_io) {
		__disk_stat_inc(rq->rq_disk, merges[rw]);
	} else {
		disk_round_stats(rq->rq_disk);
		rq->rq_disk->in_flight++;
	}
}

/*
 * add-request adds a request to the linked list.
 * queue lock is held and interrupts disabled, as we muck with the
 * request queue list.
 */
/**ltl
 * 功能:添加request到IO调度器或者派发队列中
 * 参数:q	->请求队列
 *	req	->当req是屏障IO或者是SCSI内部的命令请求时，就把requet添加到派发队列中；
 *		  如果req是来自应用进程的请求时，把request添加到IO调度器中。
 */
static inline void add_request(request_queue_t * q, struct request * req)
{
	/* 处理gendisk中的disk_stats成员，具体功能还不是很清楚!!!! */
	drive_stat_acct(req, req->nr_sectors, 1);
	/* 对于scsi磁盘，activity_fn值为空, 详细看:blk_queue_make_request */
	if (q->activity_fn)
		q->activity_fn(q->activity_data, rq_data_dir(req));

	/*
	 * elevator indicated where it wants this request to be
	 * inserted at elevator_merge time
	 */
	 /* 插入到请求队列 */
	__elv_add_request(q, req, ELEVATOR_INSERT_SORT, 0);
}
 
/*
 * disk_round_stats()	- Round off the performance stats on a struct
 * disk_stats.
 *
 * The average IO queue length and utilisation statistics are maintained
 * by observing the current state of the queue length and the amount of
 * time it has been in this state for.
 *
 * Normally, that accounting is done on IO completion, but that can result
 * in more than a second's worth of IO being accounted for within any one
 * second, leading to >100% utilisation.  To deal with that, we call this
 * function to do a round-off before returning the results when reading
 * /proc/diskstats.  This accounts immediately for all queue usage up to
 * the current jiffies and restarts the counters again.
 */
void disk_round_stats(struct gendisk *disk)
{
	unsigned long now = jiffies;

	if (now == disk->stamp)
		return;

	if (disk->in_flight) {
		__disk_stat_add(disk, time_in_queue,
				disk->in_flight * (now - disk->stamp));
		__disk_stat_add(disk, io_ticks, (now - disk->stamp));
	}
	disk->stamp = now;
}

EXPORT_SYMBOL_GPL(disk_round_stats);

/*
 * queue lock must be held
 */
/**ltl
 * 功能:释放request对象
 * 说明: 这个函数对REQ_CMD和REQ_PC两种请求执行结束后都会调用此函数来释放request对象
 */
void __blk_put_request(request_queue_t *q, struct request *req)
{
	struct request_list *rl = req->rl;

	if (unlikely(!q))
		return;
	if (unlikely(--req->ref_count))/* request还在被引用 */
		return;
	/* request完成处理，对正常的request没有做什么处理 */
	elv_completed_request(q, req);

	req->rq_status = RQ_INACTIVE;
	req->rl = NULL;

	/*
	 * Request may not have originated from ll_rw_blk. if not,
	 * it didn't come out of our reserved rq pools
	 */
	if (rl) {
		int rw = rq_data_dir(req);
		int priv = req->flags & REQ_ELVPRIV;
		/* request还在派发队列中，按正常流程已经在blkdev_dequeue_request函数中，把request移出派发队列中。*/
		BUG_ON(!list_empty(&req->queuelist));
		/* 释放IO调度器的私有数据和request本身 */
		blk_free_request(q, req);
		/* 更改引用计数器和唤醒进程 */
		freed_request(q, rw, priv);
	}
}

EXPORT_SYMBOL_GPL(__blk_put_request);
/**ltl
 * 功能:释放request内存空间，供其它子系统使用
 */
void blk_put_request(struct request *req)
{
	unsigned long flags;
	request_queue_t *q = req->q;

	/*
	 * Gee, IDE calls in w/ NULL q.  Fix IDE and remove the
	 * following if (q) test.
	 */
	if (q) {
		spin_lock_irqsave(q->queue_lock, flags);
		__blk_put_request(q, req);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
}

EXPORT_SYMBOL(blk_put_request);

/**
 * blk_end_sync_rq - executes a completion event on a request
 * @rq: request to complete
 * @error: end io status of the request
 */
void blk_end_sync_rq(struct request *rq, int error)
{
	struct completion *waiting = rq->waiting;

	rq->waiting = NULL;
	__blk_put_request(rq->q, rq);/* 释放request对象 */

	/*
	 * complete last, if this is a stack request the process (and thus
	 * the rq pointer) could be invalid right after this complete()
	 */
	complete(waiting);
}
EXPORT_SYMBOL(blk_end_sync_rq);

/**
 * blk_congestion_wait - wait for a queue to become uncongested
 * @rw: READ or WRITE
 * @timeout: timeout in jiffies
 *
 * Waits for up to @timeout jiffies for a queue (any queue) to exit congestion.
 * If no queues are congested then just wait for the next request to be
 * returned.
 */
long blk_congestion_wait(int rw, long timeout)
{
	long ret;
	DEFINE_WAIT(wait);
	wait_queue_head_t *wqh = &congestion_wqh[rw];

	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	ret = io_schedule_timeout(timeout);
	finish_wait(wqh, &wait);
	return ret;
}

EXPORT_SYMBOL(blk_congestion_wait);

/*
 * Has to be called with the request spinlock acquired
 */
/**ltl
 * 功能:把next表示的request对象合并到req表示的request对象
 * 参数:q	->请求队列
 *	req	->需要合并的request
 *	next	->需要合并的request
 * 返回值:0	->合并失败
 *	 1	->合并成功
 * 说明: 1.先判定两个request是否能够合并
 * 	    2.合并两个request请求
 */
static int attempt_merge(request_queue_t *q, struct request *req,
			  struct request *next)
{
	/* 两个request都设置了不可合并标志，则不能合并 */
	if (!rq_mergeable(req) || !rq_mergeable(next))
		return 0;

	/*
	 * not contiguous
	 */
	 /* 如果两个request所在的扇区号不连续，则不能合并 */
	if (req->sector + req->nr_sectors != next->sector)
		return 0;
	/* 如果两个request的读写方向不一致，或者不是同一块磁盘，或者下一请求为scsi命令，或者下一请求已经提交给底层驱动，则不能合并 */
	if (rq_data_dir(req) != rq_data_dir(next)
	    || req->rq_disk != next->rq_disk
	    || next->waiting || next->special)
		return 0;

	/*
	 * If we are allowed to merge, then append bio list
	 * from next to rq and release next. merge_requests_fn
	 * will have updated segment counts, update sector
	 * counts here.
	 */
	 /* 判定两个request能否合并。ll_merge_requests_fn */
	if (!q->merge_requests_fn(q, req, next))
		return 0;

	/*
	 * At this point we have either done a back merge
	 * or front merge. We need the smaller start_time of
	 * the merged requests to be the current request
	 * for accounting purposes.
	 */
	 /* 更新请求时间，以最靠前的时间为准 */
	if (time_after(req->start_time, next->start_time))
		req->start_time = next->start_time;

	/* 合并两个bio列表 */
	req->biotail->bi_next = next->bio;
	req->biotail = next->biotail;

	/* 扇区数总数 */
	req->nr_sectors = req->hard_nr_sectors += next->hard_nr_sectors;

	/* 合并两个request的私有数据elevator_private */
	elv_merge_requests(q, req, next);

	if (req->rq_disk) {/* <统计信息，略过> */
		disk_round_stats(req->rq_disk);
		req->rq_disk->in_flight--;
	}
	/* 请求的优先级，对CFQ调度算法有用 */
	req->ioprio = ioprio_best(req->ioprio, next->ioprio);
	/* 释放被合并request对象内存空间 */
	__blk_put_request(q, next);
	return 1;
}

/**ltl
功能:合并rq与其之后的request
参数:q 	->请求队列
	rq	->要合并的request对象
返回值:
*/
static inline int attempt_back_merge(request_queue_t *q, struct request *rq)
{
	/*获取rq的下一个可以与之合并的request*/
	struct request *next = elv_latter_request(q, rq);

	/*找到可以与之合并的request，刚进行合并*/
	if (next)
		return attempt_merge(q, rq, next);

	return 0;
}
/**ltl
功能:合并rq与其之前合并request
参数:q 	->请求队列
	rq	->要合并的request对象
返回值:
*/
static inline int attempt_front_merge(request_queue_t *q, struct request *rq)
{	
	/*获取rq的前一个request*/
	struct request *prev = elv_former_request(q, rq);

	if (prev)
		return attempt_merge(q, prev, rq);

	return 0;
}
/**ltl
 * 功能:利用bio属性域对新的request属性域赋值
 * 参数:req	->要赋值的请求
 *	bio	->bio对象
 */
static void init_request_from_bio(struct request *req, struct bio *bio)
{
	/* 设置REQ_CMD标志，表示当前的request包含一个标准的读写IO数据传送的请求(来自应用进程)。*/
	req->flags |= REQ_CMD;

	/*
	 * inherit FAILFAST from bio (for read-ahead, and explicit FAILFAST)
	 */
	if (bio_rw_ahead(bio) || bio_failfast(bio))
		req->flags |= REQ_FAILFAST; /* 设置REQ_FAILFAST标志后，表示当前的request出错时，就停止处理，即在底层驱动不再进行重试。*/

	/*
	 * REQ_BARRIER implies no merging, but lets make it explicit
	 */
	 /* bio是一个屏障请求，则当前的request设置屏障标志和不可合并标志 */
	if (unlikely(bio_barrier(bio)))
		req->flags |= (REQ_HARDBARRIER | REQ_NOMERGE);

	/* bio是一个同步请求，则request就要设置同步标志。*/
	if (bio_sync(bio))
		req->flags |= REQ_RW_SYNC;
	/* 利用bio的属性去设置request的属性 */
	req->errors = 0;
	req->hard_sector = req->sector = bio->bi_sector; /* 请求的起始扇区号 */
	req->hard_nr_sectors = req->nr_sectors = bio_sectors(bio); /* 数据长度 */
	req->current_nr_sectors = req->hard_cur_sectors = bio_cur_sectors(bio); /* 当前段的数据长度 */
	req->nr_phys_segments = bio_phys_segments(req->q, bio);
	req->nr_hw_segments = bio_hw_segments(req->q, bio);
	/* 这个字段可以不用设置(scsi_init_io函数设置) */
	req->buffer = bio_data(bio);	/* see ->buffer comment above */ 
	/*
	 * waiting是一个completion对象，如果等于NULL,表示此request是来自用户进程的请求;
	 * 如果非空，则表示request是SCSI子系统的请求,在函数blk_execute_rq中设置。
	 * 这个字段在在elv_rq_merge_ok会用这个字段。
	 */
	req->waiting = NULL;
	req->bio = req->biotail = bio;
	req->ioprio = bio_prio(bio);
	req->rq_disk = bio->bi_bdev->bd_disk;
	req->start_time = jiffies; /* request开始的时间 */
}

/**ltl
 * 功能:块设备驱动的处理例程，根据当前系统所使用的调度算法，将当前的bio插入到调度队列中。
 * 参数:q->bio要插入的等待队列。
 *	   bio->要插入到等待队列的bio对象
 * 返回值:0:表示块设备驱动已经成功处理了当前的请求。
 */
static int __make_request(request_queue_t *q, struct bio *bio)
{
	struct request *req;
	int el_ret, rw, nr_sectors, cur_nr_sectors, barrier, err, sync;
	unsigned short prio;
	sector_t sector;

	sector = bio->bi_sector;/* bio的起始扇区号 */
	nr_sectors = bio_sectors(bio);/* bio的扇区总数 */
	cur_nr_sectors = bio_cur_sectors(bio);/* 当前处理的段的扇区数 */
	prio = bio_prio(bio);/* bio的优先级别 */

	rw = bio_data_dir(bio);/* 请求的方向:READ/WRITE */
	sync = bio_sync(bio);/* 同步请求标识 */

	/*
	 * low level driver can indicate that it wants pages above a
	 * certain limit bounced to low memory (ie for highmem, or even
	 * ISA dma in theory)
	 */
	 /*创建一个反弹缓冲区。
	  */
	blk_queue_bounce(q, &bio);

	spin_lock_prefetch(q->queue_lock);


	/*屏障IO的处理*/
	barrier = bio_barrier(bio);
	if (unlikely(barrier) && (q->next_ordered == QUEUE_ORDERED_NONE)) {
		err = -EOPNOTSUPP;
		goto end_io;
	}

	spin_lock_irq(q->queue_lock);

	/* 当前的bio是一个屏障IO或者IO调度器或者派发队列是空的，则直接获得request后，再插入到请求队列 */
	if (unlikely(barrier) || elv_queue_empty(q))
		goto get_rq;
	/* 根据IO调度算法，找出bio要合入的request对象和要合入到request的bio列表的位置,
	  即elv_merge是从IO调度算法的角度找到合入的request */
	el_ret = elv_merge(q, &req, bio);
	switch (el_ret) {
		case ELEVATOR_BACK_MERGE:/* bio合入到request的列表后部 */
			BUG_ON(!rq_mergeable(req));/* 如果request设置了不可合入标志RQ_NOMERGE_FLAGS，则挂起系统 */
			/* 判定能否在req列表(各个bio对象组成)的尾部合入新bio。这个是从request本身的属性限制判定是否能够合入bio */
			if (!q->back_merge_fn(q, req, bio))
				break;

			blk_add_trace_bio(q, bio, BLK_TA_BACKMERGE);
			/* 在request列表的尾部合入bio */
			req->biotail->bi_next = bio;
			req->biotail = bio;
			req->nr_sectors = req->hard_nr_sectors += nr_sectors;
			req->ioprio = ioprio_best(req->ioprio, prio);
			drive_stat_acct(req, nr_sectors, 0);/* 记录gendisk中的stat信息 */
			/* 如果这次合并可能正好填补request和起始位置上后一个request之间的空洞，则进一步合并这两个request.
			  合完两个request之后，要去合并与IO调度算法相关的私有数据 */
			if (!attempt_back_merge(q, req))
				elv_merged_request(q, req);/* 合并入的bio不能填补两个request之间的空洞，则单独更新IO调度算法中的私有数据 */
			goto out;

		case ELEVATOR_FRONT_MERGE:/* bio合入到request的列表头部 */
			BUG_ON(!rq_mergeable(req));/* 如果request设置了不可合入标志RQ_NOMERGE_FLAGS，则挂起系统 */
			/* 判定能否在req列表的头部合入bio */
			if (!q->front_merge_fn(q, req, bio))
				break;
			
			blk_add_trace_bio(q, bio, BLK_TA_FRONTMERGE);
			/* 在request列表的头部合入新的bio */
			bio->bi_next = req->bio;
			req->bio = bio;

			/*
			 * may not be valid. if the low level driver said
			 * it didn't need a bounce buffer then it better
			 * not touch req->buffer either...
			 */
			/* 更新request的部分属性值 */
			req->buffer = bio_data(bio);	/* 把buffer指针指向第一段内存地址。*/
			req->current_nr_sectors = cur_nr_sectors;
			req->hard_cur_sectors = cur_nr_sectors;
			req->sector = req->hard_sector = sector;
			req->nr_sectors = req->hard_nr_sectors += nr_sectors;
			req->ioprio = ioprio_best(req->ioprio, prio);/* 更新优先级别 */
			drive_stat_acct(req, nr_sectors, 0);
			/*如果此次合入，正好填补了"合入的reqeust"与"前面的request"之前的空洞，则就去合并这两个request,
			  同时去合并IO调度算法中的私有数据*/
			if (!attempt_front_merge(q, req))
				elv_merged_request(q, req);/* 如果合入的bio不能填补两个request之间的空洞，则单独更新IO调度算法中的私有数据。*/
			goto out;

		/* ELV_NO_MERGE: elevator says don't/can't merge. */
		default: /* 表示在请求列表中找不到request能与bio合并，则重新申请一个request，并插入到请求列表中，标签get_rq就是完成这一工作。*/
			;
	}

get_rq:
	/*
	 * Grab a free request. This is might sleep but can not fail.
	 * Returns with the queue unlocked.
	 */
	 /*申请一个request对象，当申请不到时，则进入休眠*/
	req = get_request_wait(q, rw, bio);

	/*
	 * After dropping the lock and possibly sleeping here, our request
	 * may now be mergeable after it had proven unmergeable (above).
	 * We don't worry about that case for efficiency. It won't happen
	 * often, and the elevators are able to handle it.
	 */
	/* 利用bio的各个域来初始化req的个各域 */
	init_request_from_bio(req, bio);

	spin_lock_irq(q->queue_lock);
	if (elv_queue_empty(q)) /* 如果请求队列是空的，则进入队列"蓄流" */
		blk_plug_device(q); /* 执行"畜流"功能 */
	add_request(q, req);/* 把当前的request插入到请求队列中 */
out:       
	if (sync)/* 如果在bio的标志已经设置的同步标志，调用底层驱动程序的策略处理函数，马上处理当前的request,而不是进入"畜流"状态。 */
		__generic_unplug_device(q);

	spin_unlock_irq(q->queue_lock);
	return 0;

end_io:/* 调用bio_endio结束当前的bio处理。*/
	bio_endio(bio, nr_sectors << 9, err);
	return 0;
}

/*
 * If bio->bi_dev is a partition, remap the location
 */
/**ltl
 * 功能:如果bio是由一个子分区发起请求，就要转化为对当前子分区的所属主分区的读写。
 * 参数:bio->子分区发起的bio
 */
static inline void blk_partition_remap(struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;
	/*子分区发起的bio请求*/
	if (bdev != bdev->bd_contains) {
		struct hd_struct *p = bdev->bd_part;
		const int rw = bio_data_dir(bio);

		p->sectors[rw] += bio_sectors(bio); /*累加对分区请求数量*/
		p->ios[rw]++; /* 分区的请求次数 */

		bio->bi_sector += p->start_sect; /* 加上当前分区的起始地址，就是当就bio所请求的起始地址。*/
		bio->bi_bdev = bdev->bd_contains; /* 设备描述指向主分区的设备描述符。*/
	}
}

static void handle_bad_sector(struct bio *bio)
{
	char b[BDEVNAME_SIZE];

	printk(KERN_INFO "attempt to access beyond end of device\n");
	printk(KERN_INFO "%s: rw=%ld, want=%Lu, limit=%Lu\n",
			bdevname(bio->bi_bdev, b),
			bio->bi_rw,
			(unsigned long long)bio->bi_sector + bio_sectors(bio),
			(long long)(bio->bi_bdev->bd_inode->i_size >> 9));

	/*设置BIO_EOF标志*/
	set_bit(BIO_EOF, &bio->bi_flags);
}

/**
 * generic_make_request: hand a buffer to its device driver for I/O
 * @bio:  The bio describing the location in memory and on the device.
 *
 * generic_make_request() is used to make I/O requests of block
 * devices. It is passed a &struct bio, which describes the I/O that needs
 * to be done.
 *
 * generic_make_request() does not return any status.  The
 * success/failure status of the request, along with notification of
 * completion, is delivered asynchronously through the bio->bi_end_io
 * function described (one day) else where.
 *
 * The caller of generic_make_request must make sure that bi_io_vec
 * are set to describe the memory buffer, and that bi_dev and bi_sector are
 * set to describe the device address, and the
 * bi_end_io and optionally bi_private are set to describe how
 * completion notification should be signaled.
 *
 * generic_make_request and the drivers it calls may use bi_next if this
 * bio happens to be merged with someone else, and may change bi_dev and
 * bi_sector for remaps as it sees fit.  So the values of these fields
 * should NOT be depended on after the call to generic_make_request.
 */
/**ltl
 * 功能: 将bio对象构造一个request请求对象
 * 参数: bio对象
 * 返回值: 
 * 说明: 将bio对象转化成request对象
 */
void generic_make_request(struct bio *bio)
{
	request_queue_t *q;
	sector_t maxsector;
	int ret, nr_sectors = bio_sectors(bio);
	dev_t old_dev;
	/**/
	might_sleep();
	/* Test device or partition size, when known. */
	/* i_size在do_open中设置 */
	maxsector = bio->bi_bdev->bd_inode->i_size >> 9;/* 当前分区的容量 */
	if (maxsector) {
		sector_t sector = bio->bi_sector;
		/* 如果当前请求的数据量超出了分区容量或者所请求的起始扇区位置不合理，终止当前的bio */
		if (maxsector < nr_sectors || maxsector - nr_sectors < sector) {
			/*
			 * This may well happen - the kernel calls bread()
			 * without checking the size of the device, e.g., when
			 * mounting a device.
			 */
			handle_bad_sector(bio);
			goto end_io;
		}
	}

	/*
	 * Resolve the mapping until finished. (drivers are
	 * still free to implement/resolve their own stacking
	 * by explicitly returning 0)
	 *
	 * NOTE: we don't repeat the blk_size check for each new device.
	 * Stacking drivers are expected to know what they are doing.
	 */
	maxsector = -1;
	old_dev = 0;
	/*
	 * 循环的作用:
	 * make_request_fn是返回值有两种情况:
	 *	0->表示bio已经提交执行;
	 *	!0->表示说明函数实现了从一个设备到另一种设备的映射，也就是说，bio已经被修改成新的块设备对象,需要提交给新的块设备执行。
	 */
	do {
		char b[BDEVNAME_SIZE];
		/* 获取块设备描述符所关联的请求队列 */
		q = bdev_get_queue(bio->bi_bdev);
		if (!q) {
			printk(KERN_ERR
			       "generic_make_request: Trying to access "
				"nonexistent block-device %s (%Lu)\n",
				bdevname(bio->bi_bdev, b),
				(long long) bio->bi_sector);
end_io:
			/* 出现错误，结束当前的bio */
			bio_endio(bio, bio->bi_size, -EIO);
			break;
		}
		/* 当前请求的扇区数大于硬件的最大扇区数，结束当前的bio, */
		if (unlikely(bio_sectors(bio) > q->max_hw_sectors)) {
			printk("bio too big device %s (%u > %u)\n", 
				bdevname(bio->bi_bdev, b),
				bio_sectors(bio),
				q->max_hw_sectors);
			goto end_io;
		}
		/* 当前的队列已经销毁，结束当前的bio */
		if (unlikely(test_bit(QUEUE_FLAG_DEAD, &q->queue_flags)))
			goto end_io;

		encryption_request(q, &bio);

		/*
		 * If this device has partitions, remap block n
		 * of partition p to block n+start(p) of the disk.
		 */
		/*如果当前的bio是请求某一个子分区上的数据，则要重新计算bio的起始扇区值*/
		blk_partition_remap(bio);

		if (maxsector != -1)
			blk_add_trace_remap(q, bio, old_dev, bio->bi_sector, 
					    maxsector);

		blk_add_trace_bio(q, bio, BLK_TA_QUEUE);

		maxsector = bio->bi_sector;
		old_dev = bio->bi_bdev->bd_dev;
		/*将请求提交给块设备驱动程序，对磁盘驱动，make_request_fn就是__make_request()*/
		ret = q->make_request_fn(q, bio);
	} while (ret);
}

EXPORT_SYMBOL(generic_make_request);

/**
 * submit_bio: submit a bio to the block device layer for I/O
 * @rw: whether to %READ or %WRITE, or maybe to %READA (read ahead)
 * @bio: The &struct bio which describes the I/O
 *
 * submit_bio() is very similar in purpose to generic_make_request(), and
 * uses that function to do most of the work. Both are fairly rough
 * interfaces, @bio must be presetup and ready for I/O.
 *
 */
/**ltl
 * 功能:向块设备层提交bio
 * 参数: rw->读写方向
 *	 bio->bio数据对象
 */
void submit_bio(int rw, struct bio *bio)
{
	/* bio所包含数据块数 */
	int count = bio_sectors(bio);
	
	BIO_BUG_ON(!bio->bi_size);/*数据长度为0*/
	BIO_BUG_ON(!bio->bi_io_vec);/*数据的首地址*/
	bio->bi_rw |= rw;/*设置数据读写方向READ/WRITE*/
	if (rw & WRITE)
		count_vm_events(PGPGOUT, count);
	else
		count_vm_events(PGPGIN, count);

	if (unlikely(block_dump)) {
		char b[BDEVNAME_SIZE];
		printk(KERN_DEBUG "%s(%d): %s block %Lu on %s\n",
			current->comm, current->pid,
			(rw & WRITE) ? "WRITE" : "READ",
			(unsigned long long)bio->bi_sector,
			bdevname(bio->bi_bdev,b));
	}
	/* 提交bio请求 */
	generic_make_request(bio);
}

EXPORT_SYMBOL(submit_bio);
/**ltl
 * 功能:重新计算nr_phys_segments和nr_hw_segments值
 * 参数:rq	->request对象
 * 说明:这个函数只有在request中的数据没有传输完成的情况下使用。
 */
static void blk_recalc_rq_segments(struct request *rq)
{
	struct bio *bio, *prevbio = NULL;
	int nr_phys_segs, nr_hw_segs;
	unsigned int phys_size, hw_size;
	request_queue_t *q = rq->q;

	if (!rq->bio)
		return;

	phys_size = hw_size = nr_phys_segs = nr_hw_segs = 0;
	rq_for_each_bio(bio, rq) {
		/* Force bio hw/phys segs to be recalculated. */
		bio->bi_flags &= ~(1 << BIO_SEG_VALID);

		nr_phys_segs += bio_phys_segments(q, bio);
		nr_hw_segs += bio_hw_segments(q, bio);
		if (prevbio) {
			int pseg = phys_size + prevbio->bi_size + bio->bi_size;
			int hseg = hw_size + prevbio->bi_size + bio->bi_size;

			if (blk_phys_contig_segment(q, prevbio, bio) &&
			    pseg <= q->max_segment_size) {
				nr_phys_segs--;
				phys_size += prevbio->bi_size + bio->bi_size;
			} else
				phys_size = 0;

			if (blk_hw_contig_segment(q, prevbio, bio) &&
			    hseg <= q->max_segment_size) {
				nr_hw_segs--;
				hw_size += prevbio->bi_size + bio->bi_size;
			} else
				hw_size = 0;
		}
		prevbio = bio;
	}

	rq->nr_phys_segments = nr_phys_segs;
	rq->nr_hw_segments = nr_hw_segs;
}
/**ltl
 * 功能:重新计算reqeust中统计数据的信息
 * 参数:
 * 说明:这个函数只有在request数据一次没有全部传输完成时调用
 */
static void blk_recalc_rq_sectors(struct request *rq, int nsect)
{
	if (blk_fs_request(rq)) {
		rq->hard_sector += nsect;/* 起始扇区偏移 */
		rq->hard_nr_sectors -= nsect;/* 总的扇区数 */

		/*
		 * Move the I/O submission pointers ahead if required.
		 */
		if ((rq->nr_sectors >= rq->hard_nr_sectors) &&
		    (rq->sector <= rq->hard_sector)) {
			rq->sector = rq->hard_sector;
			rq->nr_sectors = rq->hard_nr_sectors;
			rq->hard_cur_sectors = bio_cur_sectors(rq->bio);
			rq->current_nr_sectors = rq->hard_cur_sectors;
			rq->buffer = bio_data(rq->bio);
		}

		/*
		 * if total number of sectors is less than the first segment
		 * size, something has gone terribly wrong
		 */
		if (rq->nr_sectors < rq->current_nr_sectors) {
			printk("blk: request botched\n");
			rq->nr_sectors = rq->current_nr_sectors;
		}
	}
}
/**ltl
 * 功能: request对象结束处理流程
 * 参数: req		-> 请求对象
 * 		uptodate	-> 1.表示处理成功，0.表示失败
 *		nr_bytes	-> 处理的的数据长度
 * 返回值:
 * 说明:
 */
static int __end_that_request_first(struct request *req, int uptodate,
				    int nr_bytes)
{
	int total_bytes, bio_nbytes, error, next_idx = 0;
	struct bio *bio;

	blk_add_trace_rq(req->q, req, BLK_TA_COMPLETE);

	/*
	 * extend uptodate bool to allow < 0 value to be direct io error
	 */
	error = 0;
	if (end_io_error(uptodate))
		error = !uptodate ? -EIO : uptodate;

	/*
	 * for a REQ_BLOCK_PC request, we want to carry any eventual
	 * sense key with us all the way through
	 */
	if (!blk_pc_request(req))/* 不是由SCSI子系统层的命令 */
		req->errors = 0;

	if (!uptodate) { /* request请求处理失败 */
		if (blk_fs_request(req) && !(req->flags & REQ_QUIET))/* 当命令是应用进程的命令时，就打印日志 */
			printk("end_request: I/O error, dev %s, sector %llu\n",
				req->rq_disk ? req->rq_disk->disk_name : "?",
				(unsigned long long)req->sector);
	}

	if (blk_fs_request(req) && req->rq_disk) {/* 加入到统计器(先不考虑)*/
		const int rw = rq_data_dir(req);

		disk_stat_add(req->rq_disk, sectors[rw], nr_bytes >> 9);
	}

	total_bytes = bio_nbytes = 0;
	/*
	 * 若当前的request是来自于用户进程(request->flags | REQ_CMD )，则会执行下面的代码，主要是调用bio中的回调函数:bio->bi_end_io=end_bio_bh_io_sync
	 * 若当前的request是来自SCSI子系统(request->flags | REQ_BLOCK_PC ),则不会执行下面的代码，在块IO系统层中要释放request对象即可。
	 */
	while ((bio = req->bio) != NULL) {
		int nbytes;
		/* 遍历request请求的bio列表 */
		if (nr_bytes >= bio->bi_size) { 
			req->bio = bio->bi_next;
			nbytes = bio->bi_size;
			if (!ordered_bio_endio(req, bio, nbytes, error))/* 是否是屏障IO */
			{
				if(blk_fs_request(req))
					decryption_reuqest(req->q, bio);
				
				bio_endio(bio, nbytes, error);/* 处理普通IO请求的处理完成的处理函数 */
			}
			next_idx = 0;
			bio_nbytes = 0;
		} else {/* request承载的数据没有被处理完成，只是处理了bio中的部分bio_vec中的数据 */
			int idx = bio->bi_idx + next_idx;

			if (unlikely(bio->bi_idx >= bio->bi_vcnt)) {
				blk_dump_rq_flags(req, "__end_that");
				printk("%s: bio idx %d >= vcnt %d\n",
						__FUNCTION__,
						bio->bi_idx, bio->bi_vcnt);
				break;
			}

			nbytes = bio_iovec_idx(bio, idx)->bv_len;
			BIO_BUG_ON(nbytes > bio->bi_size);

			/*
			 * not a complete bvec done
			 */
			if (unlikely(nbytes > nr_bytes)) {/* 当存在没有处理完成的数据，这里退出循环 */
				bio_nbytes += nr_bytes;
				total_bytes += nr_bytes;
				break;
			}

			/*
			 * advance to the next vector
			 */
			next_idx++;
			bio_nbytes += nbytes;
		}

		total_bytes += nbytes;
		nr_bytes -= nbytes;
		/* request中的bio链表不为空，而已经处理的字节数却小于0,说明request有没有处理完成的数据。*/
		if ((bio = req->bio)) {
			/*
			 * end more in this run, or just return 'not-done'
			 */
			if (unlikely(nr_bytes <= 0))
				break;
		}
	}

	/*
	 * completely done
	 */
	/* 一个请求处理到这里就已经完成。即以下代码不会再执行。*/
	if (!req->bio)
		return 0;

	/*
	 * if the request wasn't completed, update state
	 */
	 /* bio_nbytes是已经处理完成的数据长度 */
	if (bio_nbytes) {
		if (!ordered_bio_endio(req, bio, bio_nbytes, error))
		{
			if(blk_fs_request(req))
				decryption_reuqest(req->q, bio);
			
			bio_endio(bio, bio_nbytes, error);/* 通道上层已经传输的长度 */
		}
		/* 更新bio中的成员值，因为后面要把此request对象放入请求队列中 */
		bio->bi_idx += next_idx;
		bio_iovec(bio)->bv_offset += nr_bytes;
		bio_iovec(bio)->bv_len -= nr_bytes;
	}
	/* 得新计算request中的起始扇区和数据总长度 */
	blk_recalc_rq_sectors(req, total_bytes >> 9);
	blk_recalc_rq_segments(req);
	return 1;
}

/**
 * end_that_request_first - end I/O on a request
 * @req:      the request being processed
 * @uptodate: 1 for success, 0 for I/O error, < 0 for specific error
 * @nr_sectors: number of sectors to end I/O on
 *
 * Description:
 *     Ends I/O on a number of sectors attached to @req, and sets it up
 *     for the next range of segments (if any) in the cluster.
 *
 * Return:
 *     0 - we are done with this request, call end_that_request_last()
 *     1 - still buffers pending for this request
 **/
/**ltl
 * 功能: 调用bio->bi_end_io接口去通知FS层。
 * 参数: req	-> 请求对象
 * 		uptodate	->1.表示处理成功；0.处理失败
 *		nr_sectors->处理的数据长度
 * 返回值:
 * 说明:
 */
int end_that_request_first(struct request *req, int uptodate, int nr_sectors)
{
	return __end_that_request_first(req, uptodate, nr_sectors << 9);
}

EXPORT_SYMBOL(end_that_request_first);

/**
 * end_that_request_chunk - end I/O on a request
 * @req:      the request being processed
 * @uptodate: 1 for success, 0 for I/O error, < 0 for specific error
 * @nr_bytes: number of bytes to complete
 *
 * Description:
 *     Ends I/O on a number of bytes attached to @req, and sets it up
 *     for the next range of segments (if any). Like end_that_request_first(),
 *     but deals with bytes instead of sectors.
 *
 * Return:
 *     0 - we are done with this request, call end_that_request_last()
 *     1 - still buffers pending for this request
 **/
/**ltl
功能:调用bio->done通知上层命令已经处理完成，处理是否成功由uptodate标志
参数:req	->请求
	uptodate->0表示处理失败，1表示成功处理
	nr_bytes->成功处理或者错误处理的字节数
返回值:
*/
int end_that_request_chunk(struct request *req, int uptodate, int nr_bytes)
{
	return __end_that_request_first(req, uptodate, nr_bytes);
}

EXPORT_SYMBOL(end_that_request_chunk);

/*
 * splice the completion data to a local structure and hand off to
 * process_completion_queue() to complete the requests
 */
/**ltl
功能:软中断的处理函数，这个软中断的作用:底层驱动处理完一个request后，触发这个软件中断继续处理后续工作
参数:h	->
*/
static void blk_done_softirq(struct softirq_action *h)
{
	struct list_head *cpu_list, local_list;

	local_irq_disable();/* 中断屏蔽 */
	cpu_list = &__get_cpu_var(blk_cpu_done);
	list_replace_init(cpu_list, &local_list);/* 把列表中的数据拷贝到loacal_list中 */
	local_irq_enable();/* 开启中断 */

	while (!list_empty(&local_list)) {/* 如果列表不为空,即底层驱动已经处理完多个请求，则逐一调用softirq_done_fn去处理中断。*/
		struct request *rq = list_entry(local_list.next, struct request, donelist);

		list_del_init(&rq->donelist);/* 从local_list中删除request */
		rq->q->softirq_done_fn(rq);
	}
}

#ifdef CONFIG_HOTPLUG_CPU

static int blk_cpu_notify(struct notifier_block *self, unsigned long action,
			  void *hcpu)
{
	/*
	 * If a CPU goes away, splice its entries to the current CPU
	 * and trigger a run of the softirq
	 */
	if (action == CPU_DEAD) {
		int cpu = (unsigned long) hcpu;

		local_irq_disable();
		list_splice_init(&per_cpu(blk_cpu_done, cpu),
				 &__get_cpu_var(blk_cpu_done));
		raise_softirq_irqoff(BLOCK_SOFTIRQ);
		local_irq_enable();
	}

	return NOTIFY_OK;
}


static struct notifier_block __devinitdata blk_cpu_notifier = {
	.notifier_call	= blk_cpu_notify,
};

#endif /* CONFIG_HOTPLUG_CPU */

/**
 * blk_complete_request - end I/O on a request
 * @req:      the request being processed
 *
 * Description:
 *     Ends all I/O on a request. It does not handle partial completions,
 *     unless the driver actually implements this in its completion callback
 *     through requeueing. Theh actual completion happens out-of-order,
 *     through a softirq handler. The user must have registered a completion
 *     callback through blk_queue_softirq_done().
 **/
/**ltl
 * 功能:当scsi处理完一个scsi_cmnd后，调用__scsi_done处理函数，做后续处理工作，而__scsi_done正是调用这个函数把处理结果往块IO层传递的。
 * 参数:req	->处理完成的request对象
 */
void blk_complete_request(struct request *req)
{
	struct list_head *cpu_list;
	unsigned long flags;

	BUG_ON(!req->q->softirq_done_fn);
		
	local_irq_save(flags);/* 加锁，并把系统中的中断状态的值保存到flags */

	cpu_list = &__get_cpu_var(blk_cpu_done);
	list_add_tail(&req->donelist, cpu_list);/* 把处理完的request存入到当前cpu的列表中。*/
	raise_softirq_irqoff(BLOCK_SOFTIRQ);/* 引发软件中断:BLOCK_SOFTIRQ */

	local_irq_restore(flags);/* 解锁,并恢复保存在flags中的中断状态的值 */
}

EXPORT_SYMBOL(blk_complete_request);
	
/*
 * queue lock must be held
 */
/**ltl
 * 功能:释放request对象和与调度算法相关的私有数据成员elevator_private
 * 参数:
 */
void end_that_request_last(struct request *req, int uptodate)
{
	struct gendisk *disk = req->rq_disk;
	int error;

	/*
	 * extend uptodate bool to allow < 0 value to be direct io error
	 */
	error = 0;
	if (end_io_error(uptodate))
		error = !uptodate ? -EIO : uptodate;

	if (unlikely(laptop_mode) && blk_fs_request(req))
		laptop_io_completion();

	/*
	 * Account IO completion.  bar_rq isn't accounted as a normal
	 * IO on queueing nor completion.  Accounting the containing
	 * request is enough.
	 */
	/* 非屏障IO */
	if (disk && blk_fs_request(req) && req != &req->q->bar_rq) {
		unsigned long duration = jiffies - req->start_time;
		const int rw = rq_data_dir(req);

		__disk_stat_inc(disk, ios[rw]);
		__disk_stat_add(disk, ticks[rw], duration);
		disk_round_stats(disk);
		disk->in_flight--;
	}
	if (req->end_io)/* req屏障IO的请求和SCSI命令，才走这个流程 */
		req->end_io(req, error);
	else/* 正常的request,直接去释放request对象和IO调度算法有关的成员elevator_private */
		__blk_put_request(req->q, req);
}

EXPORT_SYMBOL(end_that_request_last);

/**ltl
 * 功能:结束当前的请求后，调用的接口函数
 * 参数:req	->处理完的request对象
 *	uptodate->1: 表示处理成功，<=0: 表示错误
 */
void end_request(struct request *req, int uptodate)
{
	if (!end_that_request_first(req, uptodate, req->hard_cur_sectors)) {
		add_disk_randomness(req->rq_disk);
		blkdev_dequeue_request(req);/* 从派发队列中删除 */
		end_that_request_last(req, uptodate);/* 释放elevator_private的内存空间和request内存空间 */
	}
}

EXPORT_SYMBOL(end_request);

void blk_rq_bio_prep(request_queue_t *q, struct request *rq, struct bio *bio)
{
	/* first two bits are identical in rq->flags and bio->bi_rw */
	rq->flags |= (bio->bi_rw & 3);

	rq->nr_phys_segments = bio_phys_segments(q, bio);
	rq->nr_hw_segments = bio_hw_segments(q, bio);
	rq->current_nr_sectors = bio_cur_sectors(bio);
	rq->hard_cur_sectors = rq->current_nr_sectors;
	rq->hard_nr_sectors = rq->nr_sectors = bio_sectors(bio);
	rq->buffer = bio_data(bio);

	rq->bio = rq->biotail = bio;
}

EXPORT_SYMBOL(blk_rq_bio_prep);

int kblockd_schedule_work(struct work_struct *work)
{
	return queue_work(kblockd_workqueue, work);
}

EXPORT_SYMBOL(kblockd_schedule_work);

void kblockd_flush(void)
{
	flush_workqueue(kblockd_workqueue);
}
EXPORT_SYMBOL(kblockd_flush);
/**ltl
功能:块设备模块的初始化
*/
int __init blk_dev_init(void)
{
	int i;
	/*创建块设备的工作队列*/
	kblockd_workqueue = create_workqueue("kblockd");
	if (!kblockd_workqueue)
		panic("Failed to create kblockd\n");
	/*创建request对象的内存池*/
	request_cachep = kmem_cache_create("blkdev_requests",
			sizeof(struct request), 0, SLAB_PANIC, NULL, NULL);
	/*创建request_queue_t对象内存池*/
	requestq_cachep = kmem_cache_create("blkdev_queue",
			sizeof(request_queue_t), 0, SLAB_PANIC, NULL, NULL);
	/*创建io上下文对象内存池*/
	iocontext_cachep = kmem_cache_create("blkdev_ioc",
			sizeof(struct io_context), 0, SLAB_PANIC, NULL, NULL);

	/* 为每个CPU的列表初始化，这个列表的作用是:在底层驱动接收到中断后，会触发软中断"BLOCK_SOFTIRQ",从底层接收到的数据保存到这个列表中。*/
	for_each_possible_cpu(i)
		INIT_LIST_HEAD(&per_cpu(blk_cpu_done, i));
	/* 注册块设备专用的软中断 */
	open_softirq(BLOCK_SOFTIRQ, blk_done_softirq, NULL);/* 注册软件中断 */

	/*Q:CPU hotplug?*/
	register_hotcpu_notifier(&blk_cpu_notifier);

	blk_max_low_pfn = max_low_pfn;/* 低端内存的最大页号 */
	blk_max_pfn = max_pfn;	/* 内存的最大页号 */

	return 0;
}

/*
 * IO Context helper functions
 */
void put_io_context(struct io_context *ioc)
{
	if (ioc == NULL)
		return;

	BUG_ON(atomic_read(&ioc->refcount) == 0);

	if (atomic_dec_and_test(&ioc->refcount)) {
		struct cfq_io_context *cic;

		rcu_read_lock();
		if (ioc->aic && ioc->aic->dtor)
			ioc->aic->dtor(ioc->aic);
		if (ioc->cic_root.rb_node != NULL) {
			struct rb_node *n = rb_first(&ioc->cic_root);

			cic = rb_entry(n, struct cfq_io_context, rb_node);
			cic->dtor(ioc);
		}
		rcu_read_unlock();

		kmem_cache_free(iocontext_cachep, ioc);
	}
}
EXPORT_SYMBOL(put_io_context);

/* Called by the exitting task */
void exit_io_context(void)
{
	unsigned long flags;
	struct io_context *ioc;
	struct cfq_io_context *cic;

	local_irq_save(flags);
	task_lock(current);
	ioc = current->io_context;
	current->io_context = NULL;
	ioc->task = NULL;
	task_unlock(current);
	local_irq_restore(flags);

	if (ioc->aic && ioc->aic->exit)
		ioc->aic->exit(ioc->aic);
	if (ioc->cic_root.rb_node != NULL) {
		cic = rb_entry(rb_first(&ioc->cic_root), struct cfq_io_context, rb_node);
		cic->exit(ioc);
	}
 
	put_io_context(ioc);
}

/*
 * If the current task has no IO context then create one and initialise it.
 * Otherwise, return its existing IO context.
 *
 * This returned IO context doesn't have a specifically elevated refcount,
 * but since the current task itself holds a reference, the context can be
 * used in general code, so long as it stays within `current` context.
 */
struct io_context *current_io_context(gfp_t gfp_flags)
{
	struct task_struct *tsk = current;
	struct io_context *ret;

	ret = tsk->io_context;
	if (likely(ret))
		return ret;

	ret = kmem_cache_alloc(iocontext_cachep, gfp_flags);
	if (ret) {
		atomic_set(&ret->refcount, 1);
		ret->task = current;
		ret->set_ioprio = NULL;
		ret->last_waited = jiffies; /* doesn't matter... */
		ret->nr_batch_requests = 0; /* because this is 0 */
		ret->aic = NULL;
		ret->cic_root.rb_node = NULL;
		/* make sure set_task_ioprio() sees the settings above */
		smp_wmb();
		tsk->io_context = ret;
	}

	return ret;
}
EXPORT_SYMBOL(current_io_context);

/*
 * If the current task has no IO context then create one and initialise it.
 * If it does have a context, take a ref on it.
 *
 * This is always called in the context of the task which submitted the I/O.
 */
struct io_context *get_io_context(gfp_t gfp_flags)
{
	struct io_context *ret;
	ret = current_io_context(gfp_flags);
	if (likely(ret))
		atomic_inc(&ret->refcount);
	return ret;
}
EXPORT_SYMBOL(get_io_context);

void copy_io_context(struct io_context **pdst, struct io_context **psrc)
{
	struct io_context *src = *psrc;
	struct io_context *dst = *pdst;

	if (src) {
		BUG_ON(atomic_read(&src->refcount) == 0);
		atomic_inc(&src->refcount);
		put_io_context(dst);
		*pdst = src;
	}
}
EXPORT_SYMBOL(copy_io_context);

void swap_io_context(struct io_context **ioc1, struct io_context **ioc2)
{
	struct io_context *temp;
	temp = *ioc1;
	*ioc1 = *ioc2;
	*ioc2 = temp;
}
EXPORT_SYMBOL(swap_io_context);

/*
 * sysfs parts below
 */
struct queue_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct request_queue *, char *);
	ssize_t (*store)(struct request_queue *, const char *, size_t);
};

static ssize_t
queue_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
queue_var_store(unsigned long *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtoul(p, &p, 10);
	return count;
}

static ssize_t queue_requests_show(struct request_queue *q, char *page)
{
	return queue_var_show(q->nr_requests, (page));
}

static ssize_t
queue_requests_store(struct request_queue *q, const char *page, size_t count)
{
	struct request_list *rl = &q->rq;
	unsigned long nr;
	int ret = queue_var_store(&nr, page, count);
	if (nr < BLKDEV_MIN_RQ)
		nr = BLKDEV_MIN_RQ;

	spin_lock_irq(q->queue_lock);
	q->nr_requests = nr;
	blk_queue_congestion_threshold(q);

	if (rl->count[READ] >= queue_congestion_on_threshold(q)/*5*/)
		set_queue_congested(q, READ);/* 设置READ为拥塞 */
	else if (rl->count[READ] < queue_congestion_off_threshold(q)/*3*/)
		clear_queue_congested(q, READ);/* 清除读拥塞标志 */

	if (rl->count[WRITE] >= queue_congestion_on_threshold(q)/*5*/)
		set_queue_congested(q, WRITE);/* 设置写拥塞 */
	else if (rl->count[WRITE] < queue_congestion_off_threshold(q)/*3*/)
		clear_queue_congested(q, WRITE);/* 清除写拥塞标志 */

	if (rl->count[READ] >= q->nr_requests/*4*/) {
		blk_set_queue_full(q, READ);/* 读的请求已经满 */
	} else if (rl->count[READ]+1 <= q->nr_requests) {
		blk_clear_queue_full(q, READ);/* 如果读的请求小于nr_requests时，清除标志 */
		wake_up(&rl->wait[READ]);/* 唤醒读(让读的进程请求读吗???) */
	}

	if (rl->count[WRITE] >= q->nr_requests) {
		blk_set_queue_full(q, WRITE);
	} else if (rl->count[WRITE]+1 <= q->nr_requests) {
		blk_clear_queue_full(q, WRITE);
		wake_up(&rl->wait[WRITE]);
	}
	spin_unlock_irq(q->queue_lock);
	return ret;
}

static ssize_t queue_ra_show(struct request_queue *q, char *page)
{
	int ra_kb = q->backing_dev_info.ra_pages << (PAGE_CACHE_SHIFT - 10);

	return queue_var_show(ra_kb, (page));
}

static ssize_t
queue_ra_store(struct request_queue *q, const char *page, size_t count)
{
	unsigned long ra_kb;
	ssize_t ret = queue_var_store(&ra_kb, page, count);

	spin_lock_irq(q->queue_lock);
	if (ra_kb > (q->max_sectors >> 1))
		ra_kb = (q->max_sectors >> 1);

	q->backing_dev_info.ra_pages = ra_kb >> (PAGE_CACHE_SHIFT - 10);
	spin_unlock_irq(q->queue_lock);

	return ret;
}

static ssize_t queue_max_sectors_show(struct request_queue *q, char *page)
{
	int max_sectors_kb = q->max_sectors >> 1;

	return queue_var_show(max_sectors_kb, (page));
}

static ssize_t
queue_max_sectors_store(struct request_queue *q, const char *page, size_t count)
{
	unsigned long max_sectors_kb,
			max_hw_sectors_kb = q->max_hw_sectors >> 1,
			page_kb = 1 << (PAGE_CACHE_SHIFT - 10);
	ssize_t ret = queue_var_store(&max_sectors_kb, page, count);
	int ra_kb;

	if (max_sectors_kb > max_hw_sectors_kb || max_sectors_kb < page_kb)
		return -EINVAL;
	/*
	 * Take the queue lock to update the readahead and max_sectors
	 * values synchronously:
	 */
	spin_lock_irq(q->queue_lock);
	/*
	 * Trim readahead window as well, if necessary:
	 */
	ra_kb = q->backing_dev_info.ra_pages << (PAGE_CACHE_SHIFT - 10);
	if (ra_kb > max_sectors_kb)
		q->backing_dev_info.ra_pages =
				max_sectors_kb >> (PAGE_CACHE_SHIFT - 10);

	q->max_sectors = max_sectors_kb << 1;
	spin_unlock_irq(q->queue_lock);

	return ret;
}

static ssize_t queue_max_hw_sectors_show(struct request_queue *q, char *page)
{
	int max_hw_sectors_kb = q->max_hw_sectors >> 1;

	return queue_var_show(max_hw_sectors_kb, (page));
}


static struct queue_sysfs_entry queue_requests_entry = {
	.attr = {.name = "nr_requests", .mode = S_IRUGO | S_IWUSR },
	.show = queue_requests_show,
	.store = queue_requests_store,
};

static struct queue_sysfs_entry queue_ra_entry = {
	.attr = {.name = "read_ahead_kb", .mode = S_IRUGO | S_IWUSR },
	.show = queue_ra_show,
	.store = queue_ra_store,
};

static struct queue_sysfs_entry queue_max_sectors_entry = {
	.attr = {.name = "max_sectors_kb", .mode = S_IRUGO | S_IWUSR },
	.show = queue_max_sectors_show,
	.store = queue_max_sectors_store,
};

static struct queue_sysfs_entry queue_max_hw_sectors_entry = {
	.attr = {.name = "max_hw_sectors_kb", .mode = S_IRUGO },
	.show = queue_max_hw_sectors_show,
};

static struct queue_sysfs_entry queue_iosched_entry = {
	.attr = {.name = "scheduler", .mode = S_IRUGO | S_IWUSR },
	.show = elv_iosched_show,
	.store = elv_iosched_store,
};

static struct attribute *default_attrs[] = {
	&queue_requests_entry.attr,
	&queue_ra_entry.attr,
	&queue_max_hw_sectors_entry.attr,
	&queue_max_sectors_entry.attr,
	&queue_iosched_entry.attr,
	NULL,
};

#define to_queue(atr) container_of((atr), struct queue_sysfs_entry, attr)

static ssize_t
queue_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct queue_sysfs_entry *entry = to_queue(attr);
	request_queue_t *q = container_of(kobj, struct request_queue, kobj);
	ssize_t res;

	if (!entry->show)
		return -EIO;
	mutex_lock(&q->sysfs_lock);
	if (test_bit(QUEUE_FLAG_DEAD, &q->queue_flags)) {
		mutex_unlock(&q->sysfs_lock);
		return -ENOENT;
	}
	res = entry->show(q, page);
	mutex_unlock(&q->sysfs_lock);
	return res;
}

static ssize_t
queue_attr_store(struct kobject *kobj, struct attribute *attr,
		    const char *page, size_t length)
{
	struct queue_sysfs_entry *entry = to_queue(attr);
	request_queue_t *q = container_of(kobj, struct request_queue, kobj);

	ssize_t res;

	if (!entry->store)
		return -EIO;
	mutex_lock(&q->sysfs_lock);
	if (test_bit(QUEUE_FLAG_DEAD, &q->queue_flags)) {
		mutex_unlock(&q->sysfs_lock);
		return -ENOENT;
	}
	res = entry->store(q, page, length);
	mutex_unlock(&q->sysfs_lock);
	return res;
}

static struct sysfs_ops queue_sysfs_ops = {
	.show	= queue_attr_show,
	.store	= queue_attr_store,
};

static struct kobj_type queue_ktype = {
	.sysfs_ops	= &queue_sysfs_ops,
	.default_attrs	= default_attrs,
	.release	= blk_release_queue,
};
/**ltl
 * 功能:在/sys/block/sd[a-b]下创建queue目录
 * 参数:disk	->通用磁盘对象
 */
int blk_register_queue(struct gendisk *disk)
{
	int ret;

	request_queue_t *q = disk->queue;

	if (!q || !q->request_fn)
		return -ENXIO;
	/* 设置父亲节点 */
	q->kobj.parent = kobject_get(&disk->kobj);
	/* 添加请求队列对象 */
	ret = kobject_add(&q->kobj);
	if (ret < 0)
		return ret;
	
	kobject_uevent(&q->kobj, KOBJ_ADD);
	/* 添加调度算法的object,并创建相关属性文件 */
	ret = elv_register_queue(q);
	if (ret) {
		kobject_uevent(&q->kobj, KOBJ_REMOVE);
		kobject_del(&q->kobj);
		return ret;
	}

	return 0;
}

void blk_unregister_queue(struct gendisk *disk)
{
	request_queue_t *q = disk->queue;

	if (q && q->request_fn) {
		elv_unregister_queue(q);

		kobject_uevent(&q->kobj, KOBJ_REMOVE);
		kobject_del(&q->kobj);
		kobject_put(&disk->kobj);
	}
}
