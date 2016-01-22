#include <scsi/scsi.h>
#include <linux/highmem.h>
#include <asm/kmap_types.h>
#include <linux/delay.h>
#include "scsi_cmnd_handle.h"
#include <linux/scatterlist.h>
#define MEM_DEVICE_SIZE (16 << 20)


int do_cmnd_test_unit_ready_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_inquiry_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_request_sense_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_read_capacity_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_read_buffer_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_report_luns_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_mode_sense_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_read_10_handle(struct scsi_cmnd* pcmnd);
int do_cmnd_write_10_handle(struct scsi_cmnd* pcmnd);

struct cmnd_handle g_cmnd[] = {
    {TEST_UNIT_READY,               do_cmnd_test_unit_ready_handle},
    {INQUIRY,                       do_cmnd_inquiry_handle},
    {REQUEST_SENSE,                 do_cmnd_request_sense_handle},
    {READ_CAPACITY,                 do_cmnd_read_capacity_handle},
    {REPORT_LUNS,                   do_cmnd_report_luns_handle},
    {READ_BUFFER,                   do_cmnd_read_buffer_handle},
    {MODE_SENSE,                    do_cmnd_mode_sense_handle},
    {READ_10,                       do_cmnd_read_10_handle},
    {WRITE_10,                      do_cmnd_write_10_handle},   
};

char g_mem_device[MEM_DEVICE_SIZE] = {0};

char* get_device_buf(void)
{
	return g_mem_device;
}

unsigned get_cmnd_array_size(void)
{
    return sizeof(g_cmnd) / sizeof(struct cmnd_handle);
}
#if 0
static unsigned int mem_scsi_rbuf_get(struct scsi_cmnd *cmd, u8 **buf_out)
{
    u8 *buf;
    unsigned int buflen;

    if (cmd->use_sg) 
    {
        struct scatterlist *sg;
    
        sg = (struct scatterlist *) cmd->request_buffer;
        buf = kmap_atomic(sg->page, KM_USER0) + sg->offset;
        buflen = sg->length;
     } 
    else 
    {
        buf = cmd->request_buffer;
        buflen = cmd->request_bufflen;
    }

    *buf_out = buf;
    return buflen;
}
#else
#define ATA_SCSI_RBUF_SIZE	4096
static u8 ata_scsi_rbuf[ATA_SCSI_RBUF_SIZE];
static void *ata_scsi_rbuf_get(struct scsi_cmnd *cmd, bool copy_in,
			       unsigned long *flags)
{
//	spin_lock_irqsave(&ata_scsi_rbuf_lock, *flags);

	memset(ata_scsi_rbuf, 0, ATA_SCSI_RBUF_SIZE);
	if (copy_in)
		sg_copy_to_buffer(scsi_sglist(cmd), scsi_sg_count(cmd),
				  ata_scsi_rbuf, ATA_SCSI_RBUF_SIZE);
	return ata_scsi_rbuf;
}

static inline void ata_scsi_rbuf_put(struct scsi_cmnd *cmd, bool copy_out,
				     unsigned long *flags)
{
	if (copy_out)
		sg_copy_from_buffer(scsi_sglist(cmd), scsi_sg_count(cmd),
				    ata_scsi_rbuf, ATA_SCSI_RBUF_SIZE);
	//spin_unlock_irqrestore(&ata_scsi_rbuf_lock, *flags);
}
#endif
int do_cmnd_test_unit_ready_handle(struct scsi_cmnd* pcmnd)
{
	LogPath();
	pcmnd->result = 0;
	if(pcmnd->sense_buffer)
	{
		u8 hdr[] = {0x0,0x6,0x0};
		memcpy(pcmnd->sense_buffer,hdr,sizeof(hdr));
	}
    return 0;
}

int do_cmnd_inquiry_handle(struct scsi_cmnd* pcmnd)
{
    LogPath();
	pcmnd->result = 0;//处理的返回值
	if(pcmnd->sense_buffer)
	{
	    u8 hdr[] = {0x0,0x0,0xa};
	    memcpy(pcmnd->sense_buffer,hdr,sizeof(hdr));
		pcmnd->sense_buffer[12] = 0x24;
	}
    u8* databuf = NULL;
	#if 0
    int buflen = mem_scsi_rbuf_get(pcmnd,&databuf);
	#else
	databuf = ata_scsi_rbuf_get(pcmnd,0,NULL);
	#endif
  //  Log("bufLen:%d",buflen);//36
    databuf[0]=TYPE_MEM;//device type : mem
    databuf[1] = 0x1;//removeable
	databuf[2]=0x3;//scsi-level
	databuf[4] = 0x10;//???

	databuf[3] = 0x1;//softreset
	databuf[7] = 0x1;//softreset
	//vendor
	u8 vendor[] = {'M','E','M','-','S',' '};
	memcpy(&databuf[8],vendor,sizeof(vendor));

	u8 model[] = {'S','C','S','I',' '};
	memcpy(&databuf[16],model,sizeof(model));
    ata_scsi_rbuf_put(pcmnd,1, NULL);
    return 0;
}

int do_cmnd_request_sense_handle(struct scsi_cmnd* pcmnd)
{
    return 0;
}

int do_cmnd_read_capacity_handle(struct scsi_cmnd* pcmnd)
{
	LogPath();
	pcmnd->result = 0;
	int sector_size = 512;
	int capacity = MEM_DEVICE_SIZE;
	
	u8* databuf = NULL;
	databuf = ata_scsi_rbuf_get(pcmnd,0, NULL);
   // int buflen = mem_scsi_rbuf_get(pcmnd,&databuf);
  //  Log("bufLen:%d",buflen);//3

	databuf[0] = (capacity >> 24) & 0xFF;
	databuf[1] = (capacity >> 16) & 0xFF;
	databuf[2] = (capacity >> 8)  & 0xFF;
	databuf[3] = (capacity)       & 0xFF;
	
	databuf[4] = (sector_size >> 24) & 0xFF;
	databuf[5] = (sector_size >> 16) & 0xFF;
	databuf[6] = (sector_size >> 8)  & 0xFF;
	databuf[7] = (sector_size)		 & 0xFF;
	ata_scsi_rbuf_put(pcmnd, 1, NULL);
    return 0;
}

int do_cmnd_read_buffer_handle(struct scsi_cmnd* pcmnd)
{
	//LogPath();

    return 0;
}

int do_cmnd_mode_sense_handle(struct scsi_cmnd* pcmnd)
{
	LogPath();
    return 0;
}

int do_cmnd_read_10_handle(struct scsi_cmnd* pcmnd)
{
	//LogPath();
	
	u8* databuf = NULL;
	pcmnd->result = 0;
	sector_t block = pcmnd->cmnd[2] << 24 | pcmnd->cmnd[3] << 16 | pcmnd->cmnd[4] << 8 | pcmnd->cmnd[5];
	unsigned int this_count = pcmnd->cmnd[7] << 8 | pcmnd->cmnd[8];
	//Log("block:%d,this_count:%d",block,this_count);
	
	sg_copy_from_buffer(scsi_sglist(pcmnd), scsi_sg_count(pcmnd),
				    get_device_buf()+(block << 9), this_count  << 9);
	while(0)
	{
		mdelay(2*HZ);
		LogPath();
	}
    return 0;
}

int do_cmnd_write_10_handle(struct scsi_cmnd* pcmnd)
{
	u8* databuf = NULL;
	pcmnd->result = 0;
	sector_t block = pcmnd->cmnd[2] << 24 | pcmnd->cmnd[3] << 16 | pcmnd->cmnd[4] << 8 | pcmnd->cmnd[5];
	unsigned int this_count = pcmnd->cmnd[7] << 8 | pcmnd->cmnd[8];
	//Log("block:%d,this_count:%d",block,this_count);

	sg_copy_to_buffer(scsi_sglist(pcmnd), scsi_sg_count(pcmnd),
				    get_device_buf()+(block << 9), this_count  << 9);
	while(0)
	{
		mdelay(2*HZ);
		LogPath();
	}
    return 0;
}

int do_cmnd_report_luns_handle(struct scsi_cmnd* pcmnd)
{
	LogPath();
	pcmnd->result = 1;
	u8* databuf = NULL;
   // int buflen = mem_scsi_rbuf_get(pcmnd,&databuf);
	//Log("buflen:%d",buflen);
	databuf = ata_scsi_rbuf_get(pcmnd,1,0);
//	memset(databuf,0,buflen);//set length and count region
	ata_scsi_rbuf_put(pcmnd,1,NULL);
	return 0;
}