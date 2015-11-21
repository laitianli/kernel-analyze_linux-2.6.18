#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/genhd.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_device.h>
#include <linux/fs.h>
#include <scsi/scsi.h>
#include <scsi/scsi_eh.h>
#include <linux/dma-mapping.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_ioctl.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#define SM_MAJOR   123
#define SM_CAPICITY	(16*1024*1024)

static int sm_probe(struct device* dev);
static int sm_remove(struct device* dev);
//static int sm_init_command(struct scsi_cmnd* pcmnd);
static int sm_prep_fn(struct request_queue *q, struct request *rq);
static void sm_rescan(struct device* dev);

static unsigned long g_mem_capacity = SM_CAPICITY;

static struct scsi_driver sm_template = {
    .owner          = THIS_MODULE,
    .gendrv         = {
        .name = "sm",
        .probe = sm_probe,
        .remove = sm_remove,        
    },
    .rescan       = sm_rescan,
};

static void sm_spinup_mem_disk(struct scsi_device *sdp)
{
	unsigned char cmd[10] = {0};

	int retries = 0;
	int result = 0;
	struct scsi_sense_hdr sshdr = {0};
	int sense_valid = 0;
	do 
	{
		cmd[0] = TEST_UNIT_READY;
		memset((void *) &cmd[1], 0, 9);
		result = scsi_execute_req(sdp, cmd,
						      DMA_NONE, NULL, 0,
						      &sshdr, 30 * HZ,
						      3,NULL);

		if(result)
			sense_valid = scsi_sense_valid(&sshdr);
		retries++;		
	}while(retries < 3 && result != 0);
	Log("result:%d",result);
}

static void sm_read_capacity(struct scsi_device *sdp)
{
	unsigned char 			cmd[16] = {0};
	int 		  			retries = 0;
	int 		  			result  = 0;
	struct scsi_sense_hdr 	sshdr = {0};
	int 		  			sense_valid = 0;
	
	unsigned char buffer[512] = {0};
	do
	{
		
		cmd[0] = READ_CAPACITY;
		memset((void *) &cmd[1], 0, 9);
		memset(buffer,0,sizeof(buffer));
		result = scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE,
					      buffer, 8, &sshdr,
					      30 * HZ, 3,NULL);
		retries ++;
	}while(result && retries < 3);

	if(!result)
	{
		sdp -> sector_size = buffer[4] << 24 | buffer[5] << 16 | buffer[6] << 8 | buffer[7];
		g_mem_capacity 	   = buffer[0] << 24 | buffer[1] << 16 | buffer[2] << 8 | buffer[3];

		Log("sector_size:%d,capacity:%d",sdp->sector_size,g_mem_capacity);
	}
}

static int sm_open(struct block_device *pdev, fmode_t mode)
//static int sm_open(struct inode *inode, struct file *filp)
{
	LogPath();
	int retval = -ENXIO;
	#if 1
	struct gendisk* disk = pdev->bd_disk;
	Log("disk:0x%x",disk);
	
	struct scsi_device* sdev = disk->private_data;
	
	if(!scsi_block_when_processing_errors(sdev))
	{
		goto error_out;
	}

	if(!scsi_device_online(sdev))
		goto error_out;
	
	#endif
	return 0;
error_out:
	return retval;
}
static int sm_release(struct gendisk *disk, fmode_t mode)
{
	LogPath();
	struct scsi_device* sdev = disk->private_data;
	if(scsi_block_when_processing_errors(sdev))
		scsi_set_medium_removal(sdev, SCSI_REMOVAL_ALLOW);
	else
		return -EBUSY;
	return 0;
}

static struct block_device_operations sm_fops = {
	.owner 	= THIS_MODULE,
	.open	= sm_open,
	.release = sm_release,
};

static int sm_probe(struct device* dev)
{
    struct scsi_device *sdp = to_scsi_device(dev);
    int error = -ENODEV;

    if(sdp->type != TYPE_MEM)
        goto OUT;

    LogPath();
	struct gendisk* gd = NULL;
	gd = alloc_disk(1);
	if(!gd)
	{
		Log("[Error] alloc_disk failed.");
		return -1;
	}
	gd->major = SM_MAJOR;
	gd->first_minor = 0;
	gd->fops = &sm_fops;
	gd->private_data = sdp;
	sprintf(gd->disk_name,"sm-scsi");
	gd->queue = sdp->request_queue;
	gd->driverfs_dev = &sdp->sdev_gendev;
	//gd->flags = GENHD_FL_DRIVERFS;
	if(sdp->removable)
		gd->flags |= GENHD_FL_REMOVABLE;
	
	//dev->p->driver_data = (void*)gd;
	dev_set_drvdata(dev,(void*)gd);
	
	sm_spinup_mem_disk(sdp);
	sm_read_capacity(sdp);
	
	set_capacity(gd,g_mem_capacity >> 9);
	blk_queue_prep_rq(sdp->request_queue,sm_prep_fn);
	add_disk(gd);
	
	return 0;
OUT:
    return error;
}

static int sm_remove(struct device* dev)
{
	struct gendisk *gd = (struct gendisk*)dev_get_drvdata(dev);
	del_gendisk(gd);
    return 0;
}

static void sm_rescan(struct device* dev)
{
}

//static int sm_init_command(struct scsi_cmnd* pcmnd)
static int sm_prep_fn(struct request_queue *q, struct request *rq)
{
	//LogPath();
	struct scsi_device* sdp = q->queuedata;
//	struct request* rq = pcmnd->request;
	struct gendisk* disk = rq->rq_disk;
	sector_t 	 block = blk_rq_pos(rq);
	unsigned int this_count = blk_rq_sectors(rq);
	int ret = 0;
	//[OS]=[sm_prep_fn:193]=block:32640,this_count:8
//	Log("block:%d,this_count:%d",block,this_count);
	if(rq->cmd_type == REQ_TYPE_BLOCK_PC)
	{
		ret = scsi_setup_blk_pc_cmnd(sdp,rq);	
		goto out;
	}
	else if(rq->cmd_type != REQ_TYPE_FS)
	{
		ret = BLKPREP_KILL;
		goto out;
	}
	ret = scsi_setup_fs_cmnd(sdp,rq);
	if(ret != BLKPREP_OK)
	{
		goto out;
	}
	struct scsi_cmnd* pcmnd = rq->special;
//	unsigned int timeout = sdp->timeout;

	if(!sdp || !scsi_device_online(sdp) || 
		block + blk_rq_sectors(rq) > get_capacity(disk))
	{
		Log("[Error] commnad.");
		return 0;
	}

	if(rq_data_dir(rq) == WRITE)
	{
		//LogPath();
		pcmnd->cmnd[0] = WRITE_6;
		pcmnd->sc_data_direction = DMA_TO_DEVICE;
	}
	else
	{
		//LogPath();
		pcmnd->cmnd[0] = READ_6;
		pcmnd->sc_data_direction = DMA_FROM_DEVICE;
	}
	//LogPath();
	pcmnd->cmnd[1] = 0;

	if(block > 0xFFFFFFFF)
	{
		pcmnd->cmnd[0] += READ_16 - READ_6;
		pcmnd->cmnd[1] |= ((rq->cmd_flags & REQ_FUA) ? 0x8 : 0);
		pcmnd->cmnd[2] = sizeof(block) > 4 ? (unsigned char)(block >> 56) & 0xFF : 0;
		pcmnd->cmnd[3] = sizeof(block) > 4 ? (unsigned char)(block >> 48) & 0xFF : 0;
		pcmnd->cmnd[4] = sizeof(block) > 4 ? (unsigned char)(block >> 40) & 0xFF : 0;
		pcmnd->cmnd[5] = sizeof(block) > 4 ? (unsigned char)(block >> 30) & 0xFF : 0;
		pcmnd->cmnd[6] = (unsigned char)(block >> 24) & 0xFF;
		pcmnd->cmnd[7] = (unsigned char)(block >> 16) & 0xFF;
		pcmnd->cmnd[8] = (unsigned char)(block >> 8) & 0xFF;
		pcmnd->cmnd[9] = (unsigned char)block & 0xFF;

		pcmnd->cmnd[10] = (unsigned char)(this_count >> 24) & 0xFF;
		pcmnd->cmnd[11] = (unsigned char)(this_count >> 16) & 0xFF;
		pcmnd->cmnd[12] = (unsigned char)(this_count >> 8)  & 0xFF;
		pcmnd->cmnd[13] = (unsigned char)(this_count)		&0xFF;

		pcmnd->cmnd[14] = pcmnd->cmnd[15] = 0;
 	}
	else if((this_count > 0xFF) || (block > 0x1FFFFF) ||
		pcmnd->device->use_10_for_rw)
	{
		if(this_count > 0xFFFF)
			this_count = 0xFFFF;

		pcmnd->cmnd[0] += READ_10 - READ_6;
		pcmnd->cmnd[1] |= ((rq->cmd_flags & REQ_FUA) ? 0x8 : 0);
		pcmnd->cmnd[2] = (unsigned char)(block >> 24) & 0xFF;
		pcmnd->cmnd[3] = (unsigned char)(block >> 16) & 0xFF;
		pcmnd->cmnd[4] = (unsigned char)(block >> 8 ) & 0xFF;
		pcmnd->cmnd[5] = (unsigned char)(block )	  & 0xFF;
		pcmnd->cmnd[6] = pcmnd->cmnd[9] = 0;
		pcmnd->cmnd[7] = (unsigned char)(this_count >> 8) & 0xFF;
		pcmnd->cmnd[8] = (unsigned char)(this_count)	  & 0xFF;
	}
	else
	{
		if(unlikely((rq->cmd_flags & REQ_FUA)))
		{
			Log("[Error] sm:FUA write to READ/WRITE(6) drive");
			return 0;
		}
		pcmnd->cmnd[1] |= (unsigned char)((block >> 16) & 0x1F);
		pcmnd->cmnd[2] = (unsigned char)((block >> 8) & 0xFF);
		pcmnd->cmnd[3] = (unsigned char)(block & 0xFF);
		pcmnd->cmnd[4] = (unsigned char)this_count;
		pcmnd->cmnd[5] = 0;
	}
//	LogPath();
	pcmnd->sdb.length = this_count * sdp->sector_size;
	pcmnd->transfersize = sdp->sector_size;
	pcmnd->underflow = this_count << 9;
	pcmnd->allowed = 3;
	//pcmnd->timeout_per_command = timeout;
	
	//pcmnd->done = sm_rw_intr;
	ret = BLKPREP_OK;
 out:
	return scsi_prep_return(q, rq, ret);
}

static int __init sm_demo_init(void)
{
    int i = 0;
    int majors = 0;
  
    if(register_blkdev(SM_MAJOR,"sm")!=0)
		Log("[Error] register blkdev major failed.");


    
    return scsi_register_driver(&sm_template.gendrv);
}

static void __exit sm_demo_exit(void)
{
    scsi_unregister_driver(&sm_template.gendrv);
}

module_init(sm_demo_init);
module_exit(sm_demo_exit);
MODULE_LICENSE("GPL");



