/*
 *  Deadline i/o scheduler.
 *
 *  Copyright (C) 2002 Jens Axboe <axboe@suse.de>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/hash.h>
#include <linux/rbtree.h>

/*
 * See Documentation/block/deadline-iosched.txt
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

static const int deadline_hash_shift = 5;
#define DL_HASH_BLOCK(sec)	((sec) >> 3)
/* Hash函数 */
#define DL_HASH_FN(sec)		(hash_long(DL_HASH_BLOCK((sec)), deadline_hash_shift))
#define DL_HASH_ENTRIES		(1 << deadline_hash_shift)
/* Hash的Key值为请求的最后一扇区号 */
#define rq_hash_key(rq)		((rq)->sector + (rq)->nr_sectors) 
#define ON_HASH(drq)		(!hlist_unhashed(&(drq)->hash))

/*ltl
功能:Deadline数据结构，包含五个队列:2个排序队列，用红黑树实现排序功能，用来判定bio是否可以在request之前插入(ELEVATOR_FRONT_MERGE)
						  2个FIFO队列，用列表实现，用来记录超时的请求
						  1个Hash队列，用Hash实现，用来判定bio是否可以在request之后插入(ELEVATOR_BACK_MERGE)
	这几个队列的作用可以参看deadline_merge.						 
						
*/
struct deadline_data {
	/*
	 * run time data
	 */

	/*
	 * requests (deadline_rq s) are present on both sort_list and fifo_list
	 */
	struct rb_root sort_list[2];	
	struct list_head fifo_list[2];
	
	/*
	 * next in sort order. read, write or both are NULL
	 */
	/* 当把一个request从IO调度队列移动到派发队列时，用这个域记录下一个request请求对象(deadline_move_request) */
	struct deadline_rq *next_drq[2];

	/* 为了更加容易判定bio能否在request之后合并<deadline_merge>(注:hash key为request的最后一个扇区号) */
	struct hlist_head *hash;	/* request hash */

	/**/
	unsigned int batching;		/* number of sequential requests made */
	/**/
	sector_t last_sector;		/* head position */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[2];
	int fifo_batch;
	int writes_starved;
	int front_merges;

	mempool_t *drq_pool;
};

/*
 * pre-request data.
 */
struct deadline_rq {
	/*
	 * rbtree index, key is the starting offset
	 */
	struct rb_node rb_node;
	/* 当发生了两个request合并的话，这个值记录的是合并之前的rb key */
	sector_t rb_key; 
	
	struct request *request;

	/*
	 * request hash, key is the ending offset (for back merge lookup)
	 */
	struct hlist_node hash;

	/*
	 * expire fifo
	 */
	struct list_head fifo;
	unsigned long expires;
};

static void deadline_move_request(struct deadline_data *dd, struct deadline_rq *drq);

static kmem_cache_t *drq_pool;

#define RQ_DATA(rq)	((struct deadline_rq *) (rq)->elevator_private)

/*
 * the back merge hash support functions
 */
static inline void __deadline_del_drq_hash(struct deadline_rq *drq)
{
	hlist_del_init(&drq->hash);
}
/**ltl
 * 功能:从Hash链表中删除节点
 */
static inline void deadline_del_drq_hash(struct deadline_rq *drq)
{
	if (ON_HASH(drq))
		__deadline_del_drq_hash(drq);
}

static inline void
deadline_add_drq_hash(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct request *rq = drq->request;

	BUG_ON(ON_HASH(drq));/* 说明drq已经在hash表中 */
	/* 以请求request的最后一个扇区为hash key,找到Hash 队列头，将请求插入到Hash表中 */
	hlist_add_head(&drq->hash, &dd->hash[DL_HASH_FN(rq_hash_key(rq))]);
}

/*
 * move hot entry to front of chain
 */
/**ltl
 * 功能:request的私有数据移动hash的列表的头部
 */
static inline void
deadline_hot_drq_hash(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct request *rq = drq->request;
	struct hlist_head *head = &dd->hash[DL_HASH_FN(rq_hash_key(rq))];

	if (ON_HASH(drq) && &drq->hash != head->first) {
		hlist_del(&drq->hash);/* 从一个hash队列中删除 */
		hlist_add_head(&drq->hash, head);/* 插入到新一个hash队列中 */
	}
}

/**ltl
 * 功能: 根据扇区值，从hash表中获取request对象
 * 参数: dd	->私有数据
 *	    offset->磁盘扇区号
 * 返回值:请求request对象
 */
static struct request *
deadline_find_drq_hash(struct deadline_data *dd, sector_t offset)
{
	/* 从hash表中获取offset的request所在的列表头 */
	struct hlist_head *hash_list = &dd->hash[DL_HASH_FN(offset)];
	struct hlist_node *entry, *next;
	struct deadline_rq *drq;

	hlist_for_each_entry_safe(drq, entry, next, hash_list, hash) {
		struct request *__rq = drq->request;

		BUG_ON(!ON_HASH(drq));/* drq不在hash表中 */

		if (!rq_mergeable(__rq)) {/* 当前的请求reqeust设置了不可合并标志 */
			__deadline_del_drq_hash(drq);/* 从hash表删除 */
			continue;
		}

		if (rq_hash_key(__rq) == offset)/* request的请求的最后一个扇区与offset相等，则返回这个reqeust对象 */
			return __rq;
	}

	return NULL;
}

/*
 * rb tree support functions
 */
#define rb_entry_drq(node)	rb_entry((node), struct deadline_rq, rb_node)
#define DRQ_RB_ROOT(dd, drq)	(&(dd)->sort_list[rq_data_dir((drq)->request)])
#define rq_rb_key(rq)		(rq)->sector

/**ltl
 * 功能:向排序队列添加请求(往红黑树添加元素)，如果要添加的元素已经在红黑树中，则只是返回原来的那个请求的私有数据指针。
 * 参数:dd	->调度器的私有数据地址
 *	drq	->请求的私有数据地址
 * 返回值:
 *	NULL	->将新的drq成功插入到红黑树中
 *	!NULL ->表示在红黑树中已经存此元素。
 */
static struct deadline_rq *
__deadline_add_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	/* drq要插入到红黑树的头指针 */
	struct rb_node **p = &DRQ_RB_ROOT(dd, drq)->rb_node;
	struct rb_node *parent = NULL;
	struct deadline_rq *__drq;

	/* rb_key:就是每个request的起始扇区号。*/
	while (*p) {
		parent = *p;
		__drq = rb_entry_drq(parent);/* 获取parent的结构的首地址 */

		if (drq->rb_key < __drq->rb_key)/* key值小，则进入左子树 */
			p = &(*p)->rb_left;
		else if (drq->rb_key > __drq->rb_key)/* key值大，则进入到右子树。*/
			p = &(*p)->rb_right;
		else
			return __drq;/* key值相等 */
	}
	/* 插入到红黑树中 */
	rb_link_node(&drq->rb_node, parent, p);
	return NULL;
}

/**ltl
 * 功能:向排序队列插入请求(往红黑树插入request)
 * 参数:dd	->调度器的私有数据地址
 *	drq	->请求的私有数据地址
 */
static void
deadline_add_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct deadline_rq *__alias;

	drq->rb_key = rq_rb_key(drq->request);/* 设置request的IO调度器的私有数据的红黑树key值 */

retry:
	__alias = __deadline_add_drq_rb(dd, drq);/* 将drq所表示的request插入到dd所代码的请求队列中 */
	if (!__alias) {/* 如果是最新添加的，而非本身就存在，则设置刚才插入节点的"红黑"属性，在这个操作中会对红黑树进行旋转 */
		rb_insert_color(&drq->rb_node, DRQ_RB_ROOT(dd, drq));
		return;
	}
	/* 如果drq已经在红黑树中，则将请求下发到派发队列中 */
	deadline_move_request(dd, __alias);
	goto retry;
}
/**ltl
功能:从排序队列删除请求(从红黑树删除request)
参数:dd	->调度器的私有数据地址
	drq	->请求的私有数据地址
*/
static inline void
deadline_del_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	const int data_dir = rq_data_dir(drq->request);

	//如果下一次请求的地址就是当前要删除的request，则要重新找到drq的next地址，对next_drq赋值。
	if (dd->next_drq[data_dir] == drq) {
		struct rb_node *rbnext = rb_next(&drq->rb_node);//记录drq的后序结点

		dd->next_drq[data_dir] = NULL;
		if (rbnext)
			dd->next_drq[data_dir] = rb_entry_drq(rbnext);
	}

	BUG_ON(!RB_EMPTY_NODE(&drq->rb_node));//如果当前的request不在红黑树中(孤立节点)，则挂起系统
	rb_erase(&drq->rb_node, DRQ_RB_ROOT(dd, drq));//从红黑树中删除节点
	RB_CLEAR_NODE(&drq->rb_node);//设置node的父指针域指向自身
}
/**ltl
功能:根据起始扇区在排序队列中查找request
参数:dd	->调度器的私有数据地址
	sector->请求的私有数据地址
	data_dir->数据方向
返回值:起始扇区为sector的request
*/
static struct request *
deadline_find_drq_rb(struct deadline_data *dd, sector_t sector, int data_dir)
{
	struct rb_node *n = dd->sort_list[data_dir].rb_node;
	struct deadline_rq *drq;

	while (n) {
		drq = rb_entry_drq(n);

		if (sector < drq->rb_key)
			n = n->rb_left;
		else if (sector > drq->rb_key)
			n = n->rb_right;
		else
			return drq->request;
	}

	return NULL;
}

/*
 * deadline_find_first_drq finds the first (lowest sector numbered) request
 * for the specified data_dir. Used to sweep back to the start of the disk
 * (1-way elevator) after we process the last (highest sector) request.
 */
/**ltl
 * 功能:查找第一个扇区的request(先序遍历)
 * 参数:dd		->调度器的私有数据地址
 *	data_dir	->数据方向
 * 返回值:排序队列的第一个扇区的reqeust
 */
static struct deadline_rq *
deadline_find_first_drq(struct deadline_data *dd, int data_dir)
{
	struct rb_node *n = dd->sort_list[data_dir].rb_node;

	for (;;) {
		if (n->rb_left == NULL)
			return rb_entry_drq(n);
		
		n = n->rb_left;
	}
}

/*
 * add drq to rbtree and fifo
 */
/**ltl
 * 功能:添加request到调度队列中,每个request要添加到三个队列中:1.排序队列；2.FIFO队列；3.Hash表示中
 * 参数:q	->请求队列指针
 *	rq	->要插入的请求对象
 * 返回值:
 */
static void
deadline_add_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct deadline_rq *drq = RQ_DATA(rq);

	const int data_dir = rq_data_dir(drq->request);

	deadline_add_drq_rb(dd, drq);/* 添加request到排序队列中 */
	/*
	 * set expire time (only used for reads) and add to fifo list
	 */
	drq->expires = jiffies + dd->fifo_expire[data_dir];/* 设置drq在FIFO队列超时的时间 */
	list_add_tail(&drq->fifo, &dd->fifo_list[data_dir]);/* 添加request到FIFO队列中(在队尾插入) */

	if (rq_mergeable(rq))/* reqeust可以合并,则插入到hash表中 */
		deadline_add_drq_hash(dd, drq);
}

/*
 * remove rq from rbtree, fifo, and hash
 */
/**ltl
 * 功能:从IO调度器删除request
 * 参数:q	->请求队列
 *	rq	->要删除的请求对象
 * 返回值:
 */
static void deadline_remove_request(request_queue_t *q, struct request *rq)
{
	struct deadline_rq *drq = RQ_DATA(rq);
	struct deadline_data *dd = q->elevator->elevator_data;

	list_del_init(&drq->fifo);	/* 从FIFO队列中删除 */
	deadline_del_drq_rb(dd, drq);	/* 从排序队列中删除 */
	deadline_del_drq_hash(drq);		/* 从hash表中删除 */
}

/**ltl
 * 功能:判定bio是否能合并；能够合并到哪个request，合并到request队列中的头部或者尾部。
 * 参数:q	->请求队列
 *	req	->[out]要合并bio的request(扇区与bio的扇区相邻)
 * 返回值:
 *	ELEVATOR_BACK_MERGE		->bio合并到request队列的后部
 *	ELEVATOR_FRONT_MERGE	->bio合并到request队列的头部
 *	ELEVATOR_NO_MERGE		->bio不能与request合并
 */
static int
deadline_merge(request_queue_t *q, struct request **req, struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct request *__rq;
	int ret;

	/*
	 * see if the merge hash can satisfy a back merge
	 */
	 /*在hash表中查找能与bio合并的request,因为是bio的起始扇区进行查找，因此如果找到request的话，表示request的扇区是在bio的扇区之前*/
	__rq = deadline_find_drq_hash(dd, bio->bi_sector);
	if (__rq) {
		BUG_ON(__rq->sector + __rq->nr_sectors != bio->bi_sector);/* 请求扇区非法，挂起系统 */

		if (elv_rq_merge_ok(__rq, bio)) {/* 判定bio和request本身的属性，看能不能合并 */
			ret = ELEVATOR_BACK_MERGE;/* 在request的列表的尾部合上bio */
			goto out;
		}
	}

	/*
	 * check for front merge
	 */
	if (dd->front_merges) {/* 允许向前合并(deadline_init_queue已经初始化) */
		sector_t rb_key = bio->bi_sector + bio_sectors(bio);/* 获取红黑树的key */

		/* 从排序队列中查找request */
		__rq = deadline_find_drq_rb(dd, rb_key, bio_data_dir(bio));
		if (__rq) {
			BUG_ON(rb_key != rq_rb_key(__rq));

			if (elv_rq_merge_ok(__rq, bio)) {/* 判定bio和request本身的属性，看能不能合并 */
				ret = ELEVATOR_FRONT_MERGE;/* 在request的列表的头部合上bio */
				goto out;
			}
		}
	}
	/* 没有找到能合并bio的request */
	return ELEVATOR_NO_MERGE;
out:
	if (ret)/* 移动req在hash的位置!这是为什么呢??????? */
		deadline_hot_drq_hash(dd, RQ_DATA(__rq));
	*req = __rq;
	return ret;
}

/**ltl
 * 功能:在一个bio对象合到req对象后，req要得新合到hash和排序队列中。
 * 参数:	q	->请求队列
 *	req	->请求对象
 */
static void deadline_merged_request(request_queue_t *q, struct request *req)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct deadline_rq *drq = RQ_DATA(req);

	/*
	 * hash always needs to be repositioned, key is end sector
	 */
	deadline_del_drq_hash(drq);	/*从原来的hash表中删除，*/
	deadline_add_drq_hash(dd, drq);/*插入到新的hash列表中 */

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	 /* 如果bio在request的头部合入(ELEVATOR_FRONT_MERGE)时，要改变req在红黑树的位置
	  * 注:drq->rb_key记录的是合并前的key值(起始扇区)，rq_rb_key(req)记录的是合并新的bio后，新的扇区。
	  */
	if (rq_rb_key(req) != drq->rb_key) {
		deadline_del_drq_rb(dd, drq);/* 从红黑树脱落 */
		deadline_add_drq_rb(dd, drq);/* 插入红黑树 */
	}
}
/**ltl
 * 功能:当一个bio合并到req后，要再去合并两个request的私有数据成员elevator_private
 * 参数:q	->等待队列
 *	req	->要合并的request对象，已经合并了bio对象
 *	next	->要合并的reqeust对象
 */
static void
 deadline_merged_requests(request_queue_t *q, struct request *req,
			 struct request *next)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct deadline_rq *drq = RQ_DATA(req);
	struct deadline_rq *dnext = RQ_DATA(next);

	BUG_ON(!drq);
	BUG_ON(!dnext);

	/*
	 * reposition drq (this is the merged request) in hash, and in rbtree
	 * in case of a front merge
	 */
	deadline_del_drq_hash(drq);/* 先从hash列表中脱落 */
	deadline_add_drq_hash(dd, drq);/* 重新插入到hash列表中 */

	if (rq_rb_key(req) != drq->rb_key) {/* 如bio插入到req列表的头部，则重新插入红黑树中 */
		deadline_del_drq_rb(dd, drq);/*从原先的红黑树脱落 */
		deadline_add_drq_rb(dd, drq);/*重新插入到红黑树中。*/
	}

	/*
	 * if dnext expires before drq, assign its expire time to drq
	 * and move into dnext position (dnext will be deleted) in fifo
	 */
	/*
	如果req和next都不是孤立节点，并且next超时时间比req的早，就是把req设置成更早的，并从原来的FIFO列表中脱离，插入到更早的列表中。
	*/
	if (!list_empty(&drq->fifo) && !list_empty(&dnext->fifo)) {
		if (time_before(dnext->expires, drq->expires)) {
			/* 感觉这个是多余的。原因:drq,dnext两个已经是相邻，这里没有必要先移动，在deadline_remove_request会去删除 */
			list_move(&drq->fifo, &dnext->fifo);
			drq->expires = dnext->expires;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	 /* 把next的对象从hash表、FIFO列表、排序队列中删除 */
	deadline_remove_request(q, next);
}

/*
 * move request from sort list to dispatch queue.
 */
/**ltl
 * 功能:把request从IO调度器的调度队列中脱离，并添加到等待队列中
 * 参数:dd	->IO调度器对象
 *	drq	->请求的私有数据
 */
static inline void
deadline_move_to_dispatch(struct deadline_data *dd, struct deadline_rq *drq)
{
	request_queue_t *q = drq->request->q;
	/* 把next的对象从hash表、FIFO列表、排序队列中删除。注意:drq对象的在request释放时，才释放。而不是在插入到派发队列时，就去释放 */
	deadline_remove_request(q, drq->request);

	/* 把request添加到等待队列中，这个request要先从IO调度队列中脱离 */
	elv_dispatch_add_tail(q, drq->request);
}

/*
 * move an entry to dispatch queue
 */
/**ltl
 * 功能:把请求从IO调度队列中分发到派发队列中
 * 参数:dd	->派发队列
 *	drq	->请求
 */
static void
deadline_move_request(struct deadline_data *dd, struct deadline_rq *drq)
{
	const int data_dir = rq_data_dir(drq->request);
	struct rb_node *rbnext = rb_next(&drq->rb_node);

	dd->next_drq[READ] = NULL;
	dd->next_drq[WRITE] = NULL;

	if (rbnext)
		dd->next_drq[data_dir] = rb_entry_drq(rbnext);/* 记录下一个request的地址 */

	/* 记录最后一个扇区的扇区号，作用?????? */
	dd->last_sector = drq->request->sector + drq->request->nr_sectors;

	/*
	 * take it off the sort and fifo list, move
	 * to dispatch queue
	 */
	/*把request从IO调度器的调度队列添加到派发队列中*/
	deadline_move_to_dispatch(dd, drq);
}

#define list_entry_fifo(ptr)	list_entry((ptr), struct deadline_rq, fifo)

/*
 * deadline_check_fifo returns 0 if there are no expired reads on the fifo,
 * 1 otherwise. Requires !list_empty(&dd->fifo_list[data_dir])
 */
/**ltl
功能:判定FIFO队列的请求是否超时
参数:dd	->IO调度器的私有数据
	ddir	->读写方向
*/
static inline int deadline_check_fifo(struct deadline_data *dd, int ddir)
{
	struct deadline_rq *drq = list_entry_fifo(dd->fifo_list[ddir].next);

	/*
	 * drq is expired!
	 */
	if (time_after(jiffies, drq->expires))//时间比较
		return 1;

	return 0;
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc
 */
/**ltl
 * 功能:从IO调度器中找到一个最佳的request
 * 参数:q	->等待队列
 *	force	->1:"抽干"IO调度队列
 *		  0:
 * 返回值:1	->有一个request从派发队列中转移到派发队列中
 */
static int deadline_dispatch_requests(request_queue_t *q, int force)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const int reads = !list_empty(&dd->fifo_list[READ]);
	const int writes = !list_empty(&dd->fifo_list[WRITE]);
	struct deadline_rq *drq;
	int data_dir;

	/*
	 * batches are currently reads XOR writes
	 */
	if (dd->next_drq[WRITE])
		drq = dd->next_drq[WRITE];
	else
		drq = dd->next_drq[READ];

	if (drq) {/* Q:这个是什么作用呢??????????? */
		/* we have a "next request" */
		
		if (dd->last_sector != drq->request->sector)
			/* end the batch on a non sequential request */
			dd->batching += dd->fifo_batch;
		
		if (dd->batching < dd->fifo_batch)
			/* we are still entitled to batch */
			goto dispatch_request;
	}

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (reads) {/* IO调度器中有读请求 */
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[READ]));/* 如果读请求排序队列为空，挂起系统 */
		/* 如果IO调度队列中也有写请求，并且写的"饥饿"次数据大于最大的"饥饿"次数，则先去分发写请求到派发队列中。*/
		if (writes && (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;
		/* 表示为读请求 */
		data_dir = READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	/* 如果IO调度器中有写请求，而没有读请求 */
	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[WRITE]));
		/* 重置写的饥饿次数 */
		dd->starved = 0;
		/* 表示写请求 */
		data_dir = WRITE;

		goto dispatch_find_request;
	}

	return 0;
/* 开始查找data_dir标志类型的请求 */
/*算法:1.先从FIFO中取出第一个元素，看是否已经超时，如果超时，则把这个请求转移到派发队列中。
	 2.如果当前是非第一次执行查找，要判定next_drq在上一次是否已经设置，如果设置，则到这个请求转移到派发队列中。
	 3.从排序队列中取出扇区号最小的request
*/
dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	 /* 比较fifo队列的第一个元素的时间是否超时。*/
	if (deadline_check_fifo(dd, data_dir)) {
		/* An expired request exists - satisfy it */
		dd->batching = 0;
		drq = list_entry_fifo(dd->fifo_list[data_dir].next);/* 取出队列头部中的元素 */
		
	} else if (dd->next_drq[data_dir]) {/* 下一个请求域不为空，直接取出这个请求，在deadline_move_request会记录这个值 */
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		drq = dd->next_drq[data_dir];
	} else {/* 从排序队列中找到扇区号最小的请求。*/
		/*
		 * The last req was the other direction or we have run out of
		 * higher-sectored requests. Go back to the lowest sectored
		 * request (1 way elevator) and start a new batch.
		 */
		dd->batching = 0;
		drq = deadline_find_first_drq(dd, data_dir);/* 从红黑树中取到扇区号最小的请求 */
	}

dispatch_request:
	/*
	 * drq is the selected appropriate request.
	 */
	dd->batching++;/* IO调度器向派发队列提交的个数 */
	deadline_move_request(dd, drq);/* 把req从IO调度器转移到派发队列中 */

	return 1;
}

/**ltl
功能:判定FIFO队列是否为空
参数:q	->请求队列
*/
static int deadline_queue_empty(request_queue_t *q)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	return list_empty(&dd->fifo_list[WRITE])
		&& list_empty(&dd->fifo_list[READ]);
}

/**ltl
功能:获取rq的前一个请求
参数:
返回值:
*/
static struct request *
deadline_former_request(request_queue_t *q, struct request *rq)
{
	struct deadline_rq *drq = RQ_DATA(rq);
	struct rb_node *rbprev = rb_prev(&drq->rb_node);

	if (rbprev)
		return rb_entry_drq(rbprev)->request;

	return NULL;
}
/**ltl
功能:获取rq的后一个请求
参数:
返回值:
*/
static struct request *
deadline_latter_request(request_queue_t *q, struct request *rq)
{
	struct deadline_rq *drq = RQ_DATA(rq);
	struct rb_node *rbnext = rb_next(&drq->rb_node);

	if (rbnext)
		return rb_entry_drq(rbnext)->request;

	return NULL;
}
/**ltl
功能:卸载IO调度器
参数:
返回值:
*/
static void deadline_exit_queue(elevator_t *e)
{
	struct deadline_data *dd = e->elevator_data;

	BUG_ON(!list_empty(&dd->fifo_list[READ]));
	BUG_ON(!list_empty(&dd->fifo_list[WRITE]));

	mempool_destroy(dd->drq_pool);
	kfree(dd->hash);
	kfree(dd);
}

/*
 * initialize elevator private data (deadline_data), and alloc a drq for
 * each request on the free lists
 */
/**ltl
 * 功能:在关联请求队列和IO调度器后，调用此接口实现调度器私有数据(deadline_data)的分配
 * 参数:q	->请求队列
 *	e	->调度器对象
 */
static void *deadline_init_queue(request_queue_t *q, elevator_t *e)
{
	struct deadline_data *dd;
	int i;

	if (!drq_pool)
		return NULL;

	/* 分配deadline_data空间 */
	dd = kmalloc_node(sizeof(*dd), GFP_KERNEL, q->node);
	if (!dd)
		return NULL;
	memset(dd, 0, sizeof(*dd));

	/* 分配hash列表头，其中数组大小为32 */
	dd->hash = kmalloc_node(sizeof(struct hlist_head)*DL_HASH_ENTRIES,
				GFP_KERNEL, q->node);
	if (!dd->hash) {
		kfree(dd);
		return NULL;
	}

	/* 创建deadline request的内存池 */
	dd->drq_pool = mempool_create_node(BLKDEV_MIN_RQ, mempool_alloc_slab,
					mempool_free_slab, drq_pool, q->node);
	if (!dd->drq_pool) {
		kfree(dd->hash);
		kfree(dd);
		return NULL;
	}

	/* 初始化hash列表头 */
	for (i = 0; i < DL_HASH_ENTRIES; i++)
		INIT_HLIST_HEAD(&dd->hash[i]);

	/* 初始化读请求FIFO队列 */
	INIT_LIST_HEAD(&dd->fifo_list[READ]);
	/* 初始化写请求FIFO队列 */
	INIT_LIST_HEAD(&dd->fifo_list[WRITE]);
	/* 初始化读请求排序队列 */
	dd->sort_list[READ] = RB_ROOT;
	/* 初始化写请求排序队列 */
	dd->sort_list[WRITE] = RB_ROOT;
	dd->fifo_expire[READ] = read_expire;/* 设置读请求的超时时间500ms */
	dd->fifo_expire[WRITE] = write_expire;/* 设置写请求的超时时间5s */
	dd->writes_starved = writes_starved;/* 读可以使写"饥饿"的最大次数2 */
	dd->front_merges = 1;/* 允许向前合并 */
	/* 在请求过期时，它必须转移到派发队列立即处理。过期的请求是批量转移，以提高吞吐率。这个域给出了每批应包含的请求数目。*/
	dd->fifo_batch = fifo_batch;	
	return dd;
}

static void deadline_put_request(request_queue_t *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct deadline_rq *drq = RQ_DATA(rq);

	mempool_free(drq, dd->drq_pool);
	rq->elevator_private = NULL;
}

/**ltl
功能:设置请求request的与调度器相关的私有数据
参数:q	->请求队列
	rq	->要关联私有数据的request
	bio	->bio对象
	gfp_mask->分配标志
*/
static int
deadline_set_request(request_queue_t *q, struct request *rq, struct bio *bio,
		     gfp_t gfp_mask)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct deadline_rq *drq;

	drq = mempool_alloc(dd->drq_pool, gfp_mask);
	if (drq) {
		memset(drq, 0, sizeof(*drq));
		RB_CLEAR_NODE(&drq->rb_node);
		drq->request = rq;

		INIT_HLIST_NODE(&drq->hash);

		INIT_LIST_HEAD(&drq->fifo);

		rq->elevator_private = drq;//请求request的私有数据
		return 0;
	}

	return 1;
}

/*
 * sysfs parts below
 */

static ssize_t
deadline_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
deadline_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtol(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(elevator_t *e, char *page)			\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return deadline_var_show(__data, (page));			\
}
SHOW_FUNCTION(deadline_read_expire_show, dd->fifo_expire[READ], 1);
SHOW_FUNCTION(deadline_write_expire_show, dd->fifo_expire[WRITE], 1);
SHOW_FUNCTION(deadline_writes_starved_show, dd->writes_starved, 0);
SHOW_FUNCTION(deadline_front_merges_show, dd->front_merges, 0);
SHOW_FUNCTION(deadline_fifo_batch_show, dd->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(elevator_t *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data;							\
	int ret = deadline_var_store(&__data, (page), count);		\
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
STORE_FUNCTION(deadline_read_expire_store, &dd->fifo_expire[READ], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_write_expire_store, &dd->fifo_expire[WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX, 0);
STORE_FUNCTION(deadline_front_merges_store, &dd->front_merges, 0, 1, 0);
STORE_FUNCTION(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, deadline_##name##_show, \
				      deadline_##name##_store)

static struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(fifo_batch),
	__ATTR_NULL
};
/* 最后期限高度算法对象 */
static struct elevator_type iosched_deadline = {
	.ops = {
		/* 查找可以和bio进行合并的request. */
		.elevator_merge_fn = 		deadline_merge,
		/*有请求被合并时被调用，即有bio合并到一个request后被调用 */
		.elevator_merged_fn =		deadline_merged_request,
		/* 合并两个request */
		.elevator_merge_req_fn =	deadline_merged_requests,
		/*将准备好的请求request派发到派发队列中。*/
		.elevator_dispatch_fn =		deadline_dispatch_requests,
		/*往调度器中添加一个新请求时被调用*/
		.elevator_add_req_fn =		deadline_add_request,
		/*判定电梯算法队列是否为空*/
		.elevator_queue_empty_fn =	deadline_queue_empty,
		/*返回按照磁盘排序顺序在给定请求前面的那个请求，被块层用来查找合并可能性。*/
		.elevator_former_req_fn =	deadline_former_request,
		/*返回按照磁盘排序顺序在给定请求后面的那个请求，被块层用来查找合并可能性。*/
		.elevator_latter_req_fn =	deadline_latter_request,
		/*用于为请求分配私有存储空间*/
		.elevator_set_req_fn =		deadline_set_request,
		/*释放私有存储空间*/
		.elevator_put_req_fn = 		deadline_put_request,
		/*初始化调度器的私有数据*/
		.elevator_init_fn =		deadline_init_queue,
		/*释放调度器的私有数据*/
		.elevator_exit_fn =		deadline_exit_queue,
	},
	/*调度器的私有属性(在blk_register_queue->elv_register_queue中创建)*/
	.elevator_attrs = deadline_attrs,
	/*调度器的名字*/
	.elevator_name = "deadline",		
	.elevator_owner = THIS_MODULE,
};

/**ltl
 * 功能:初始化deadline调度器
 * 参数:
 * 返回值:
 */
static int __init deadline_init(void)
{
	int ret;

	/* 创建一个内存池，用于分配deadline的请求数据 */
	drq_pool = kmem_cache_create("deadline_drq", sizeof(struct deadline_rq),
				     0, 0, NULL, NULL);

	if (!drq_pool)
		return -ENOMEM;

	/* 注册deadline调度算法对象 */
	ret = elv_register(&iosched_deadline);
	if (ret)
		kmem_cache_destroy(drq_pool);

	return ret;
}

static void __exit deadline_exit(void)
{
	kmem_cache_destroy(drq_pool);
	elv_unregister(&iosched_deadline);
}

module_init(deadline_init);
module_exit(deadline_exit);

MODULE_AUTHOR("Jens Axboe");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("deadline IO scheduler");
