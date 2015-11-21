/*
 * elevator noop
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/init.h>

struct noop_data {
	struct list_head queue;
};
/**ltl
功能:合并两个request:把next合到rq之后
参数:
返回值:
说明；rq和next两个请求的sector相邻。
*/
static void noop_merged_requests(request_queue_t *q, struct request *rq,
				 struct request *next)
{
	list_del_init(&next->queuelist);
}
/**ltl
功能:把IO调度器的request分发到派发队列中
参数:
返回值:
*/
static int noop_dispatch(request_queue_t *q, int force)
{
	struct noop_data *nd = q->elevator->elevator_data;

	if (!list_empty(&nd->queue)) {
		struct request *rq;
		rq = list_entry(nd->queue.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}
/**ltl
功能:把请求request添加到IO调度器中。
参数:
返回值:
*/
static void noop_add_request(request_queue_t *q, struct request *rq)
{
	struct noop_data *nd = q->elevator->elevator_data;

	list_add_tail(&rq->queuelist, &nd->queue);
}
/**ltl
功能:判定IO调度器是否为空
参数:
返回值:
*/
static int noop_queue_empty(request_queue_t *q)
{
	struct noop_data *nd = q->elevator->elevator_data;

	return list_empty(&nd->queue);
}
/**ltl
功能:返回在扇区上与rq相邻的前一个request
参数:
返回值:
*/
static struct request *
noop_former_request(request_queue_t *q, struct request *rq)
{
	struct noop_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.prev == &nd->queue)
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}
/**ltl
功能:返回在扇区上与rq相邻的前一个request
参数:
返回值:
*/
static struct request *
noop_latter_request(request_queue_t *q, struct request *rq)
{
	struct noop_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.next == &nd->queue)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}
/**ltl
功能:
参数:
返回值:
*/
static void *noop_init_queue(request_queue_t *q, elevator_t *e)
{
	struct noop_data *nd;

	nd = kmalloc(sizeof(*nd), GFP_KERNEL);
	if (!nd)
		return NULL;
	INIT_LIST_HEAD(&nd->queue);
	return nd;
}
/**ltl
功能:
参数:
返回值:
*/
static void noop_exit_queue(elevator_t *e)
{
	struct noop_data *nd = e->elevator_data;

	BUG_ON(!list_empty(&nd->queue));
	kfree(nd);
}

static struct elevator_type elevator_noop = {
	.ops = {
		.elevator_merge_req_fn		= noop_merged_requests,
		.elevator_dispatch_fn		= noop_dispatch,
		.elevator_add_req_fn		= noop_add_request,
		.elevator_queue_empty_fn	= noop_queue_empty,
		.elevator_former_req_fn		= noop_former_request,
		.elevator_latter_req_fn		= noop_latter_request,
		.elevator_init_fn		= noop_init_queue,
		.elevator_exit_fn		= noop_exit_queue,
	},
	.elevator_name = "noop",
	.elevator_owner = THIS_MODULE,
};

static int __init noop_init(void)
{
	return elv_register(&elevator_noop);
}

static void __exit noop_exit(void)
{
	elv_unregister(&elevator_noop);
}

module_init(noop_init);
module_exit(noop_exit);


MODULE_AUTHOR("Jens Axboe");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("No-op IO scheduler");
