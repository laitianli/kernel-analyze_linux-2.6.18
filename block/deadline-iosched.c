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
/* Hash���� */
#define DL_HASH_FN(sec)		(hash_long(DL_HASH_BLOCK((sec)), deadline_hash_shift))
#define DL_HASH_ENTRIES		(1 << deadline_hash_shift)
/* Hash��KeyֵΪ��������һ������ */
#define rq_hash_key(rq)		((rq)->sector + (rq)->nr_sectors) 
#define ON_HASH(drq)		(!hlist_unhashed(&(drq)->hash))

/*ltl
����:Deadline���ݽṹ�������������:2��������У��ú����ʵ�������ܣ������ж�bio�Ƿ������request֮ǰ����(ELEVATOR_FRONT_MERGE)
						  2��FIFO���У����б�ʵ�֣�������¼��ʱ������
						  1��Hash���У���Hashʵ�֣������ж�bio�Ƿ������request֮�����(ELEVATOR_BACK_MERGE)
	�⼸�����е����ÿ��Բο�deadline_merge.						 
						
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
	/* ����һ��request��IO���ȶ����ƶ����ɷ�����ʱ����������¼��һ��request�������(deadline_move_request) */
	struct deadline_rq *next_drq[2];

	/* Ϊ�˸��������ж�bio�ܷ���request֮��ϲ�<deadline_merge>(ע:hash keyΪrequest�����һ��������) */
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
	/* ������������request�ϲ��Ļ������ֵ��¼���Ǻϲ�֮ǰ��rb key */
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
 * ����:��Hash������ɾ���ڵ�
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

	BUG_ON(ON_HASH(drq));/* ˵��drq�Ѿ���hash���� */
	/* ������request�����һ������Ϊhash key,�ҵ�Hash ����ͷ����������뵽Hash���� */
	hlist_add_head(&drq->hash, &dd->hash[DL_HASH_FN(rq_hash_key(rq))]);
}

/*
 * move hot entry to front of chain
 */
/**ltl
 * ����:request��˽�������ƶ�hash���б��ͷ��
 */
static inline void
deadline_hot_drq_hash(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct request *rq = drq->request;
	struct hlist_head *head = &dd->hash[DL_HASH_FN(rq_hash_key(rq))];

	if (ON_HASH(drq) && &drq->hash != head->first) {
		hlist_del(&drq->hash);/* ��һ��hash������ɾ�� */
		hlist_add_head(&drq->hash, head);/* ���뵽��һ��hash������ */
	}
}

/**ltl
 * ����: ��������ֵ����hash���л�ȡrequest����
 * ����: dd	->˽������
 *	    offset->����������
 * ����ֵ:����request����
 */
static struct request *
deadline_find_drq_hash(struct deadline_data *dd, sector_t offset)
{
	/* ��hash���л�ȡoffset��request���ڵ��б�ͷ */
	struct hlist_head *hash_list = &dd->hash[DL_HASH_FN(offset)];
	struct hlist_node *entry, *next;
	struct deadline_rq *drq;

	hlist_for_each_entry_safe(drq, entry, next, hash_list, hash) {
		struct request *__rq = drq->request;

		BUG_ON(!ON_HASH(drq));/* drq����hash���� */

		if (!rq_mergeable(__rq)) {/* ��ǰ������reqeust�����˲��ɺϲ���־ */
			__deadline_del_drq_hash(drq);/* ��hash��ɾ�� */
			continue;
		}

		if (rq_hash_key(__rq) == offset)/* request����������һ��������offset��ȣ��򷵻����reqeust���� */
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
 * ����:����������������(����������Ԫ��)�����Ҫ��ӵ�Ԫ���Ѿ��ں�����У���ֻ�Ƿ���ԭ�����Ǹ������˽������ָ�롣
 * ����:dd	->��������˽�����ݵ�ַ
 *	drq	->�����˽�����ݵ�ַ
 * ����ֵ:
 *	NULL	->���µ�drq�ɹ����뵽�������
 *	!NULL ->��ʾ�ں�������Ѿ����Ԫ�ء�
 */
static struct deadline_rq *
__deadline_add_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	/* drqҪ���뵽�������ͷָ�� */
	struct rb_node **p = &DRQ_RB_ROOT(dd, drq)->rb_node;
	struct rb_node *parent = NULL;
	struct deadline_rq *__drq;

	/* rb_key:����ÿ��request����ʼ�����š�*/
	while (*p) {
		parent = *p;
		__drq = rb_entry_drq(parent);/* ��ȡparent�Ľṹ���׵�ַ */

		if (drq->rb_key < __drq->rb_key)/* keyֵС������������� */
			p = &(*p)->rb_left;
		else if (drq->rb_key > __drq->rb_key)/* keyֵ������뵽��������*/
			p = &(*p)->rb_right;
		else
			return __drq;/* keyֵ��� */
	}
	/* ���뵽������� */
	rb_link_node(&drq->rb_node, parent, p);
	return NULL;
}

/**ltl
 * ����:��������в�������(�����������request)
 * ����:dd	->��������˽�����ݵ�ַ
 *	drq	->�����˽�����ݵ�ַ
 */
static void
deadline_add_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct deadline_rq *__alias;

	drq->rb_key = rq_rb_key(drq->request);/* ����request��IO��������˽�����ݵĺ����keyֵ */

retry:
	__alias = __deadline_add_drq_rb(dd, drq);/* ��drq����ʾ��request���뵽dd���������������� */
	if (!__alias) {/* �����������ӵģ����Ǳ���ʹ��ڣ������øղŲ���ڵ��"���"���ԣ�����������л�Ժ����������ת */
		rb_insert_color(&drq->rb_node, DRQ_RB_ROOT(dd, drq));
		return;
	}
	/* ���drq�Ѿ��ں�����У��������·����ɷ������� */
	deadline_move_request(dd, __alias);
	goto retry;
}
/**ltl
����:���������ɾ������(�Ӻ����ɾ��request)
����:dd	->��������˽�����ݵ�ַ
	drq	->�����˽�����ݵ�ַ
*/
static inline void
deadline_del_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	const int data_dir = rq_data_dir(drq->request);

	//�����һ������ĵ�ַ���ǵ�ǰҪɾ����request����Ҫ�����ҵ�drq��next��ַ����next_drq��ֵ��
	if (dd->next_drq[data_dir] == drq) {
		struct rb_node *rbnext = rb_next(&drq->rb_node);//��¼drq�ĺ�����

		dd->next_drq[data_dir] = NULL;
		if (rbnext)
			dd->next_drq[data_dir] = rb_entry_drq(rbnext);
	}

	BUG_ON(!RB_EMPTY_NODE(&drq->rb_node));//�����ǰ��request���ں������(�����ڵ�)�������ϵͳ
	rb_erase(&drq->rb_node, DRQ_RB_ROOT(dd, drq));//�Ӻ������ɾ���ڵ�
	RB_CLEAR_NODE(&drq->rb_node);//����node�ĸ�ָ����ָ������
}
/**ltl
����:������ʼ��������������в���request
����:dd	->��������˽�����ݵ�ַ
	sector->�����˽�����ݵ�ַ
	data_dir->���ݷ���
����ֵ:��ʼ����Ϊsector��request
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
 * ����:���ҵ�һ��������request(�������)
 * ����:dd		->��������˽�����ݵ�ַ
 *	data_dir	->���ݷ���
 * ����ֵ:������еĵ�һ��������reqeust
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
 * ����:���request�����ȶ�����,ÿ��requestҪ��ӵ�����������:1.������У�2.FIFO���У�3.Hash��ʾ��
 * ����:q	->�������ָ��
 *	rq	->Ҫ������������
 * ����ֵ:
 */
static void
deadline_add_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct deadline_rq *drq = RQ_DATA(rq);

	const int data_dir = rq_data_dir(drq->request);

	deadline_add_drq_rb(dd, drq);/* ���request����������� */
	/*
	 * set expire time (only used for reads) and add to fifo list
	 */
	drq->expires = jiffies + dd->fifo_expire[data_dir];/* ����drq��FIFO���г�ʱ��ʱ�� */
	list_add_tail(&drq->fifo, &dd->fifo_list[data_dir]);/* ���request��FIFO������(�ڶ�β����) */

	if (rq_mergeable(rq))/* reqeust���Ժϲ�,����뵽hash���� */
		deadline_add_drq_hash(dd, drq);
}

/*
 * remove rq from rbtree, fifo, and hash
 */
/**ltl
 * ����:��IO������ɾ��request
 * ����:q	->�������
 *	rq	->Ҫɾ�����������
 * ����ֵ:
 */
static void deadline_remove_request(request_queue_t *q, struct request *rq)
{
	struct deadline_rq *drq = RQ_DATA(rq);
	struct deadline_data *dd = q->elevator->elevator_data;

	list_del_init(&drq->fifo);	/* ��FIFO������ɾ�� */
	deadline_del_drq_rb(dd, drq);	/* �����������ɾ�� */
	deadline_del_drq_hash(drq);		/* ��hash����ɾ�� */
}

/**ltl
 * ����:�ж�bio�Ƿ��ܺϲ����ܹ��ϲ����ĸ�request���ϲ���request�����е�ͷ������β����
 * ����:q	->�������
 *	req	->[out]Ҫ�ϲ�bio��request(������bio����������)
 * ����ֵ:
 *	ELEVATOR_BACK_MERGE		->bio�ϲ���request���еĺ�
 *	ELEVATOR_FRONT_MERGE	->bio�ϲ���request���е�ͷ��
 *	ELEVATOR_NO_MERGE		->bio������request�ϲ�
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
	 /*��hash���в�������bio�ϲ���request,��Ϊ��bio����ʼ�������в��ң��������ҵ�request�Ļ�����ʾrequest����������bio������֮ǰ*/
	__rq = deadline_find_drq_hash(dd, bio->bi_sector);
	if (__rq) {
		BUG_ON(__rq->sector + __rq->nr_sectors != bio->bi_sector);/* ���������Ƿ�������ϵͳ */

		if (elv_rq_merge_ok(__rq, bio)) {/* �ж�bio��request��������ԣ����ܲ��ܺϲ� */
			ret = ELEVATOR_BACK_MERGE;/* ��request���б��β������bio */
			goto out;
		}
	}

	/*
	 * check for front merge
	 */
	if (dd->front_merges) {/* ������ǰ�ϲ�(deadline_init_queue�Ѿ���ʼ��) */
		sector_t rb_key = bio->bi_sector + bio_sectors(bio);/* ��ȡ�������key */

		/* ����������в���request */
		__rq = deadline_find_drq_rb(dd, rb_key, bio_data_dir(bio));
		if (__rq) {
			BUG_ON(rb_key != rq_rb_key(__rq));

			if (elv_rq_merge_ok(__rq, bio)) {/* �ж�bio��request��������ԣ����ܲ��ܺϲ� */
				ret = ELEVATOR_FRONT_MERGE;/* ��request���б��ͷ������bio */
				goto out;
			}
		}
	}
	/* û���ҵ��ܺϲ�bio��request */
	return ELEVATOR_NO_MERGE;
out:
	if (ret)/* �ƶ�req��hash��λ��!����Ϊʲô��??????? */
		deadline_hot_drq_hash(dd, RQ_DATA(__rq));
	*req = __rq;
	return ret;
}

/**ltl
 * ����:��һ��bio����ϵ�req�����reqҪ���ºϵ�hash����������С�
 * ����:	q	->�������
 *	req	->�������
 */
static void deadline_merged_request(request_queue_t *q, struct request *req)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct deadline_rq *drq = RQ_DATA(req);

	/*
	 * hash always needs to be repositioned, key is end sector
	 */
	deadline_del_drq_hash(drq);	/*��ԭ����hash����ɾ����*/
	deadline_add_drq_hash(dd, drq);/*���뵽�µ�hash�б��� */

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	 /* ���bio��request��ͷ������(ELEVATOR_FRONT_MERGE)ʱ��Ҫ�ı�req�ں������λ��
	  * ע:drq->rb_key��¼���Ǻϲ�ǰ��keyֵ(��ʼ����)��rq_rb_key(req)��¼���Ǻϲ��µ�bio���µ�������
	  */
	if (rq_rb_key(req) != drq->rb_key) {
		deadline_del_drq_rb(dd, drq);/* �Ӻ�������� */
		deadline_add_drq_rb(dd, drq);/* �������� */
	}
}
/**ltl
 * ����:��һ��bio�ϲ���req��Ҫ��ȥ�ϲ�����request��˽�����ݳ�Աelevator_private
 * ����:q	->�ȴ�����
 *	req	->Ҫ�ϲ���request�����Ѿ��ϲ���bio����
 *	next	->Ҫ�ϲ���reqeust����
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
	deadline_del_drq_hash(drq);/* �ȴ�hash�б������� */
	deadline_add_drq_hash(dd, drq);/* ���²��뵽hash�б��� */

	if (rq_rb_key(req) != drq->rb_key) {/* ��bio���뵽req�б��ͷ���������²��������� */
		deadline_del_drq_rb(dd, drq);/*��ԭ�ȵĺ�������� */
		deadline_add_drq_rb(dd, drq);/*���²��뵽������С�*/
	}

	/*
	 * if dnext expires before drq, assign its expire time to drq
	 * and move into dnext position (dnext will be deleted) in fifo
	 */
	/*
	���req��next�����ǹ����ڵ㣬����next��ʱʱ���req���磬���ǰ�req���óɸ���ģ�����ԭ����FIFO�б������룬���뵽������б��С�
	*/
	if (!list_empty(&drq->fifo) && !list_empty(&dnext->fifo)) {
		if (time_before(dnext->expires, drq->expires)) {
			/* �о�����Ƕ���ġ�ԭ��:drq,dnext�����Ѿ������ڣ�����û�б�Ҫ���ƶ�����deadline_remove_request��ȥɾ�� */
			list_move(&drq->fifo, &dnext->fifo);
			drq->expires = dnext->expires;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	 /* ��next�Ķ����hash��FIFO�б����������ɾ�� */
	deadline_remove_request(q, next);
}

/*
 * move request from sort list to dispatch queue.
 */
/**ltl
 * ����:��request��IO�������ĵ��ȶ��������룬����ӵ��ȴ�������
 * ����:dd	->IO����������
 *	drq	->�����˽������
 */
static inline void
deadline_move_to_dispatch(struct deadline_data *dd, struct deadline_rq *drq)
{
	request_queue_t *q = drq->request->q;
	/* ��next�Ķ����hash��FIFO�б����������ɾ����ע��:drq�������request�ͷ�ʱ�����ͷš��������ڲ��뵽�ɷ�����ʱ����ȥ�ͷ� */
	deadline_remove_request(q, drq->request);

	/* ��request��ӵ��ȴ������У����requestҪ�ȴ�IO���ȶ��������� */
	elv_dispatch_add_tail(q, drq->request);
}

/*
 * move an entry to dispatch queue
 */
/**ltl
 * ����:�������IO���ȶ����зַ����ɷ�������
 * ����:dd	->�ɷ�����
 *	drq	->����
 */
static void
deadline_move_request(struct deadline_data *dd, struct deadline_rq *drq)
{
	const int data_dir = rq_data_dir(drq->request);
	struct rb_node *rbnext = rb_next(&drq->rb_node);

	dd->next_drq[READ] = NULL;
	dd->next_drq[WRITE] = NULL;

	if (rbnext)
		dd->next_drq[data_dir] = rb_entry_drq(rbnext);/* ��¼��һ��request�ĵ�ַ */

	/* ��¼���һ�������������ţ�����?????? */
	dd->last_sector = drq->request->sector + drq->request->nr_sectors;

	/*
	 * take it off the sort and fifo list, move
	 * to dispatch queue
	 */
	/*��request��IO�������ĵ��ȶ�����ӵ��ɷ�������*/
	deadline_move_to_dispatch(dd, drq);
}

#define list_entry_fifo(ptr)	list_entry((ptr), struct deadline_rq, fifo)

/*
 * deadline_check_fifo returns 0 if there are no expired reads on the fifo,
 * 1 otherwise. Requires !list_empty(&dd->fifo_list[data_dir])
 */
/**ltl
����:�ж�FIFO���е������Ƿ�ʱ
����:dd	->IO��������˽������
	ddir	->��д����
*/
static inline int deadline_check_fifo(struct deadline_data *dd, int ddir)
{
	struct deadline_rq *drq = list_entry_fifo(dd->fifo_list[ddir].next);

	/*
	 * drq is expired!
	 */
	if (time_after(jiffies, drq->expires))//ʱ��Ƚ�
		return 1;

	return 0;
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc
 */
/**ltl
 * ����:��IO���������ҵ�һ����ѵ�request
 * ����:q	->�ȴ�����
 *	force	->1:"���"IO���ȶ���
 *		  0:
 * ����ֵ:1	->��һ��request���ɷ�������ת�Ƶ��ɷ�������
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

	if (drq) {/* Q:�����ʲô������??????????? */
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

	if (reads) {/* IO���������ж����� */
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[READ]));/* ����������������Ϊ�գ�����ϵͳ */
		/* ���IO���ȶ�����Ҳ��д���󣬲���д��"����"�����ݴ�������"����"����������ȥ�ַ�д�����ɷ������С�*/
		if (writes && (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;
		/* ��ʾΪ������ */
		data_dir = READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	/* ���IO����������д���󣬶�û�ж����� */
	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[WRITE]));
		/* ����д�ļ������� */
		dd->starved = 0;
		/* ��ʾд���� */
		data_dir = WRITE;

		goto dispatch_find_request;
	}

	return 0;
/* ��ʼ����data_dir��־���͵����� */
/*�㷨:1.�ȴ�FIFO��ȡ����һ��Ԫ�أ����Ƿ��Ѿ���ʱ�������ʱ������������ת�Ƶ��ɷ������С�
	 2.�����ǰ�Ƿǵ�һ��ִ�в��ң�Ҫ�ж�next_drq����һ���Ƿ��Ѿ����ã�������ã����������ת�Ƶ��ɷ������С�
	 3.�����������ȡ����������С��request
*/
dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	 /* �Ƚ�fifo���еĵ�һ��Ԫ�ص�ʱ���Ƿ�ʱ��*/
	if (deadline_check_fifo(dd, data_dir)) {
		/* An expired request exists - satisfy it */
		dd->batching = 0;
		drq = list_entry_fifo(dd->fifo_list[data_dir].next);/* ȡ������ͷ���е�Ԫ�� */
		
	} else if (dd->next_drq[data_dir]) {/* ��һ��������Ϊ�գ�ֱ��ȡ�����������deadline_move_request���¼���ֵ */
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		drq = dd->next_drq[data_dir];
	} else {/* ������������ҵ���������С������*/
		/*
		 * The last req was the other direction or we have run out of
		 * higher-sectored requests. Go back to the lowest sectored
		 * request (1 way elevator) and start a new batch.
		 */
		dd->batching = 0;
		drq = deadline_find_first_drq(dd, data_dir);/* �Ӻ������ȡ����������С������ */
	}

dispatch_request:
	/*
	 * drq is the selected appropriate request.
	 */
	dd->batching++;/* IO���������ɷ������ύ�ĸ��� */
	deadline_move_request(dd, drq);/* ��req��IO������ת�Ƶ��ɷ������� */

	return 1;
}

/**ltl
����:�ж�FIFO�����Ƿ�Ϊ��
����:q	->�������
*/
static int deadline_queue_empty(request_queue_t *q)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	return list_empty(&dd->fifo_list[WRITE])
		&& list_empty(&dd->fifo_list[READ]);
}

/**ltl
����:��ȡrq��ǰһ������
����:
����ֵ:
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
����:��ȡrq�ĺ�һ������
����:
����ֵ:
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
����:ж��IO������
����:
����ֵ:
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
 * ����:�ڹ���������к�IO�������󣬵��ô˽ӿ�ʵ�ֵ�����˽������(deadline_data)�ķ���
 * ����:q	->�������
 *	e	->����������
 */
static void *deadline_init_queue(request_queue_t *q, elevator_t *e)
{
	struct deadline_data *dd;
	int i;

	if (!drq_pool)
		return NULL;

	/* ����deadline_data�ռ� */
	dd = kmalloc_node(sizeof(*dd), GFP_KERNEL, q->node);
	if (!dd)
		return NULL;
	memset(dd, 0, sizeof(*dd));

	/* ����hash�б�ͷ�����������СΪ32 */
	dd->hash = kmalloc_node(sizeof(struct hlist_head)*DL_HASH_ENTRIES,
				GFP_KERNEL, q->node);
	if (!dd->hash) {
		kfree(dd);
		return NULL;
	}

	/* ����deadline request���ڴ�� */
	dd->drq_pool = mempool_create_node(BLKDEV_MIN_RQ, mempool_alloc_slab,
					mempool_free_slab, drq_pool, q->node);
	if (!dd->drq_pool) {
		kfree(dd->hash);
		kfree(dd);
		return NULL;
	}

	/* ��ʼ��hash�б�ͷ */
	for (i = 0; i < DL_HASH_ENTRIES; i++)
		INIT_HLIST_HEAD(&dd->hash[i]);

	/* ��ʼ��������FIFO���� */
	INIT_LIST_HEAD(&dd->fifo_list[READ]);
	/* ��ʼ��д����FIFO���� */
	INIT_LIST_HEAD(&dd->fifo_list[WRITE]);
	/* ��ʼ��������������� */
	dd->sort_list[READ] = RB_ROOT;
	/* ��ʼ��д����������� */
	dd->sort_list[WRITE] = RB_ROOT;
	dd->fifo_expire[READ] = read_expire;/* ���ö�����ĳ�ʱʱ��500ms */
	dd->fifo_expire[WRITE] = write_expire;/* ����д����ĳ�ʱʱ��5s */
	dd->writes_starved = writes_starved;/* ������ʹд"����"��������2 */
	dd->front_merges = 1;/* ������ǰ�ϲ� */
	/* ���������ʱ��������ת�Ƶ��ɷ����������������ڵ�����������ת�ƣ�����������ʡ�����������ÿ��Ӧ������������Ŀ��*/
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
����:��������request�����������ص�˽������
����:q	->�������
	rq	->Ҫ����˽�����ݵ�request
	bio	->bio����
	gfp_mask->�����־
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

		rq->elevator_private = drq;//����request��˽������
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
/* ������޸߶��㷨���� */
static struct elevator_type iosched_deadline = {
	.ops = {
		/* ���ҿ��Ժ�bio���кϲ���request. */
		.elevator_merge_fn = 		deadline_merge,
		/*�����󱻺ϲ�ʱ�����ã�����bio�ϲ���һ��request�󱻵��� */
		.elevator_merged_fn =		deadline_merged_request,
		/* �ϲ�����request */
		.elevator_merge_req_fn =	deadline_merged_requests,
		/*��׼���õ�����request�ɷ����ɷ������С�*/
		.elevator_dispatch_fn =		deadline_dispatch_requests,
		/*�������������һ��������ʱ������*/
		.elevator_add_req_fn =		deadline_add_request,
		/*�ж������㷨�����Ƿ�Ϊ��*/
		.elevator_queue_empty_fn =	deadline_queue_empty,
		/*���ذ��մ�������˳���ڸ�������ǰ����Ǹ����󣬱�����������Һϲ������ԡ�*/
		.elevator_former_req_fn =	deadline_former_request,
		/*���ذ��մ�������˳���ڸ������������Ǹ����󣬱�����������Һϲ������ԡ�*/
		.elevator_latter_req_fn =	deadline_latter_request,
		/*����Ϊ�������˽�д洢�ռ�*/
		.elevator_set_req_fn =		deadline_set_request,
		/*�ͷ�˽�д洢�ռ�*/
		.elevator_put_req_fn = 		deadline_put_request,
		/*��ʼ����������˽������*/
		.elevator_init_fn =		deadline_init_queue,
		/*�ͷŵ�������˽������*/
		.elevator_exit_fn =		deadline_exit_queue,
	},
	/*��������˽������(��blk_register_queue->elv_register_queue�д���)*/
	.elevator_attrs = deadline_attrs,
	/*������������*/
	.elevator_name = "deadline",		
	.elevator_owner = THIS_MODULE,
};

/**ltl
 * ����:��ʼ��deadline������
 * ����:
 * ����ֵ:
 */
static int __init deadline_init(void)
{
	int ret;

	/* ����һ���ڴ�أ����ڷ���deadline���������� */
	drq_pool = kmem_cache_create("deadline_drq", sizeof(struct deadline_rq),
				     0, 0, NULL, NULL);

	if (!drq_pool)
		return -ENOMEM;

	/* ע��deadline�����㷨���� */
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
