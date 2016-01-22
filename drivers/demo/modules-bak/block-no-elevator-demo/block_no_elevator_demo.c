#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/genhd.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/version.h>
#define DISK_NAME "block-no-elv-demo"
#define DISK_SIZE (1024*1024*16)
#define BLK_DEV_MAJOR   228
static char g_mem_buf[DISK_SIZE]  = {0};
static struct gendisk* gp_blk_dev_disk = NULL;

static int blk_dev_open(struct inode* inode,struct file* fp);
static int blk_dev_release(struct inode* inode,struct file* fp);

static struct block_device_operations g_mem_fops = {
    .owner          = THIS_MODULE,
    .open           = blk_dev_open,
    .release        = blk_dev_release,
};

static int blk_dev_open(struct inode* inode,struct file* fp)
{
//    LogPath();
    return 0;
}
static int blk_dev_release(struct inode* inode,struct file* fp)
{
 //   LogPath();
    return 0;
}
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
static int mem_block_no_elevator_request_fn(request_queue_t* q,struct bio* bio)
{
    int status = 0,i = 0;
    struct  bio_vec* bvec = NULL;
    bio_for_each_segment(bvec,bio,i)
    {
        char* buffer = __bio_kmap_atomic(bio,i,KM_USER0);
        switch(bio_data_dir(bio))
        {
            case WRITE:
            {
                memcpy(g_mem_buf + (bio->bi_sector << 9),buffer,bio_cur_sectors(bio) << 9);
                status = 0;
                break;
            }
            case READ:
            {
                memcpy(buffer,g_mem_buf + (bio->bi_sector << 9),bio_cur_sectors(bio) << 9);
                status = 0;
                break;
            }
            default:
            {
                Log("[Error] Unknown opetator.");
                status = -EIO;
                break;                
            }
        }
        bio_endio(bio,bio->bi_size,status);
        __bio_kunmap_atomic(bio,KM_USER0);
    }
    return 0;
}
#else
typedef struct request_queue request_queue_t;
static int mem_block_no_elevator_request_fn(request_queue_t* q,struct bio* bio)
{
    int status = 0,i = 0;
    struct bio_vec* bvec = NULL;
    bio_for_each_segment(bvec,bio,i)
    {
        char * buffer = __bio_kmap_atomic(bio,i,KM_USER0);
        loff_t pos = (bio->bi_sector << 9);
        switch(bio_data_dir(bio))
        {
            case WRITE:
            {
             //   Log("pos:%lld,bi_sector:%d,offset:%lld,bv_len:%d",pos,bio->bi_sector << 9,bvec->bv_offset,bvec->bv_len);
                memcpy(g_mem_buf + pos,buffer + bvec->bv_offset,bvec->bv_len);
                status = 0;
                break;
            }
            case READ:
            {
                //Log("pos:%lld,bi_sector:%d,offset:%lld,bv_len:%d",pos,bio->bi_sector << 9,bvec->bv_offset,bvec->bv_len);
                memcpy(buffer + bvec->bv_offset,g_mem_buf + pos,bvec->bv_len);
                status = 0;
                break;
            }
            default:
            {
                status = -EIO;
                break;
            }
        }
        bio_endio(bio,status);
        __bio_kunmap_atomic(bio,KM_USER0);
    }
    return 0;
}
#endif
static int __init block_demo_init(void)
{
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
    gp_blk_dev_disk->fops = &g_mem_fops;
    sprintf(gp_blk_dev_disk->disk_name,DISK_NAME);
    set_capacity(gp_blk_dev_disk, DISK_SIZE >> 9);
    gp_blk_dev_disk->queue = blk_alloc_queue(GFP_KERNEL);
    blk_queue_make_request(gp_blk_dev_disk->queue, mem_block_no_elevator_request_fn);

    add_disk(gp_blk_dev_disk);

    return 0;
FAIL_ALLOC_DISK:
    unregister_blkdev(BLK_DEV_MAJOR,"blk-dev-demo");
    return err;
}

static void __exit block_demo_exit(void)
{
    unregister_blkdev(BLK_DEV_MAJOR,"blk-dev-demo");
    del_gendisk(gp_blk_dev_disk);
    return ;
}

module_init(block_demo_init);
module_exit(block_demo_exit);

MODULE_LICENSE("GPL");

