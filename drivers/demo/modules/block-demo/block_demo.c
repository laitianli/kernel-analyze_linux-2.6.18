#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/genhd.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/spinlock.h>
#define DISK_NAME "block-demo"
#define DISK_SIZE (5 << 20)
#define BLK_DEV_MAJOR   229
static char g_mem_buf[DISK_SIZE]  = {0};
static struct gendisk* gp_blk_dev_disk = NULL;
spinlock_t g_lock;

static int blk_dev_open(struct inode* inode,struct file* fp);
static int blk_dev_release(struct inode* inode,struct file* fp);

static struct block_device_operations g_mem_fops = {
    .owner          = THIS_MODULE,
    .open           = blk_dev_open,
    .release        = blk_dev_release,
};

static int blk_dev_open(struct inode* inode,struct file* fp)
{
 //   LogPath();
    return 0;
}
static int blk_dev_release(struct inode* inode,struct file* fp)
{
//    LogPath();
    return 0;
}
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
static void mem_block_requeut_fn(struct request_queue* q)
{
    struct request* req = NULL;
   while(NULL != (req = elv_next_request(q))) 
   {
       if(req -> sector + req -> current_nr_sectors > get_capacity(req->rq_disk))
       {
           end_request(req,0);
          // return 0;
          LogPath();
          continue;
       }
  //  Log("sector:%d,current_nr_sectors:%d",req->sector,req->current_nr_sectors);
       switch(rq_data_dir(req))
       {
           case READ:
           {
               memcpy(req->buffer,g_mem_buf + (req->sector << 9),req->current_nr_sectors << 9);
               end_request(req,1);
               break;
           }
           case WRITE:
           {
               memcpy(g_mem_buf + (req->sector << 9), req->buffer,req->current_nr_sectors << 9);
               end_request(req,1);
                break;
           }
           default:
                Log("[Error] Unknown request...");
				break;
             //   return 0;
       }
   }
}
#else //is not ok!!
static void mem_block_requeut_fn(struct request_queue* q)
{
	struct request* req = NULL;
	while(NULL != (req = blk_fetch_request(q)))
	{
		if(blk_rq_pos(req) + blk_rq_cur_sectors(req) > get_capacity(req->rq_disk))
		{
			__blk_end_request_cur(req,-EIO);
			continue;
		}
//        BLK_PLog("dir:%d,pos:%d,bytes:%d",rq_data_dir(req),blk_rq_pos(req) << 9,blk_rq_cur_bytes(req));
		switch(rq_data_dir(req))
		{
			case READ:
                {
				    memcpy(req->buffer,g_mem_buf + (blk_rq_pos(req) << 9),blk_rq_cur_bytes(req));
				    __blk_end_request_cur(req,0);
				    break;
                }
			case WRITE:
                {
				    memcpy(g_mem_buf + (blk_rq_pos(req) << 9),req->buffer,blk_rq_cur_bytes(req));
				    __blk_end_request_cur(req,0);
				    break;
                }
			default:
				__blk_end_request_cur(req,-EIO);
				break;
		}
	}
    BLK_PLog("req:0x%p",req);
}
#endif

static int __init block_demo_init(void)
{
    spin_lock_init(&g_lock);
    int err = register_blkdev(BLK_DEV_MAJOR,"blk-dev-demo");
    if(err != 0)
    {
        Log("[Error] register_blkdev failed.");
        return -1;
    }
    gp_blk_dev_disk = alloc_disk(1);
    if(!gp_blk_dev_disk)
    {
        Log("[Error] alloc_disk failed.");
        err = -1;
        goto FAIL_ALLOC_DISK;
    }

    gp_blk_dev_disk->major = BLK_DEV_MAJOR;
    gp_blk_dev_disk->first_minor = 0;
    
    sprintf(gp_blk_dev_disk->disk_name,DISK_NAME);
    set_capacity(gp_blk_dev_disk, DISK_SIZE >> 9);
	
	gp_blk_dev_disk->fops = &g_mem_fops;
    gp_blk_dev_disk->queue = blk_init_queue(mem_block_requeut_fn,&g_lock);
	
    add_disk(gp_blk_dev_disk);

    return 0;
FAIL_ALLOC_DISK:
    unregister_blkdev(BLK_DEV_MAJOR,"blk-dev-demo");
    return err;
}

static void __exit block_demo_exit(void)
{
	del_gendisk(gp_blk_dev_disk);
    unregister_blkdev(BLK_DEV_MAJOR,"blk-dev-demo");
    return ;
}

module_init(block_demo_init);
module_exit(block_demo_exit);

MODULE_LICENSE("GPL");

