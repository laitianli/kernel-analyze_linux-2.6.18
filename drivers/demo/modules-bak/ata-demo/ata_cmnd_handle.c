#include <linux/ata.h>
#include <linux/libata.h>
#include <linux/kernel.h>
#include <linux/scatterlist.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi.h>

#include "ata_cmnd_handle.h"

#define ATA_SCSI_RBUF_SIZE	4096

struct ata_cmnd_handle g_handle[] = {
	{ATA_CMD_ID_ATA,ata_cmnd_id_ata_handle},
	{ATA_CMD_INIT_DEV_PARAMS,ata_cmnd_init_dev_params_handle},
	{ATA_CMD_PIO_READ,ata_cmnd_pio_read_handle},
	{ATA_CMD_PIO_WRITE,ata_cmnd_pio_write_handle},
};


static u8 ata_scsi_rbuf[ATA_SCSI_RBUF_SIZE];
static void *ata_scsi_rbuf_get(struct scsi_cmnd *cmd, int copy_in,unsigned long *flags)
{
	memset(ata_scsi_rbuf, 0, ATA_SCSI_RBUF_SIZE);
	if (copy_in)
		sg_copy_to_buffer(scsi_sglist(cmd), scsi_sg_count(cmd),
				  ata_scsi_rbuf, ATA_SCSI_RBUF_SIZE);
	return ata_scsi_rbuf;
}

static inline void ata_scsi_rbuf_put(struct scsi_cmnd *cmd, int copy_out,unsigned long *flags)
{
	if (copy_out)
		sg_copy_from_buffer(scsi_sglist(cmd), scsi_sg_count(cmd),
				    ata_scsi_rbuf, ATA_SCSI_RBUF_SIZE);
}

static void* ata_internal_buf_get(struct scatterlist* sgl,int count,int copy_in)
{
#if 1
	memset(ata_scsi_rbuf, 0, ATA_SCSI_RBUF_SIZE);
	if(copy_in)
		sg_copy_to_buffer(sgl,count,ata_scsi_rbuf,ATA_SCSI_RBUF_SIZE);
	return ata_scsi_rbuf;
#else
	return page_to_virt(sgl->page_link);
#endif
}
static inline void ata_internal_buf_put(struct scatterlist* sgl,int count,int copy_out)
{	
	int ret = 0;
	if(copy_out)
		ret = sg_copy_from_buffer(sgl,count,ata_scsi_rbuf,ATA_SCSI_RBUF_SIZE);

	ATA_PLog("ret=%d",ret);
	
}
unsigned int get_cmnd_array_size(void)
{
    return sizeof(g_handle) / sizeof(struct ata_cmnd_handle);
}

unsigned int parse_hd_address(struct ata_queued_cmd* qc,u64* block,u32* n_block)
{
	struct ata_device* dev = qc->dev;
	struct ata_taskfile* tf = &qc->tf;
	if(!dev || !tf)
		return -1;
	if(ata_ncq_enabled(dev))
	{
		ATA_PLog("NCQ address...");
	}
	else if(dev->flags & ATA_DFLAG_LBA)
	{
		//ATA_PLog("LBA address...");
		*n_block = tf->nsect;
		*block = (tf->lbah << 16 | tf->lbam << 8 | tf->lbal);
	}
	else //CHS
	{
		//ATA_PLog("CHS address...");
		u32 sect, head, cyl, track;
		cyl = (tf->lbam | (tf->lbah << 8));
		sect = tf->lbal;
		head = tf->device & 0xF;
		*n_block = tf->nsect;
		*block = (cyl * dev->heads + head) * dev->sectors + sect - 1;

		//
	}
	//ATA_PLog("block:%llu,n_block:%d",*block,*n_block);
	return 0;
}

int ata_cmnd_id_ata_handle(void* arg1,void* arg2)
{
	ATA_PLog("do id...");
	struct ata_queued_cmd* qc = (struct ata_queued_cmd*)arg1;
	unsigned long flags = 0;
	char serial[] = "ATA";
	char module[] = "ATA-MEM";
	char fwrev[] = "ATA-FW";
	ATA_PLog("m_elem:%d,qc->cursg:%p",qc->n_elem,qc->cursg);
	u16 *buf = (u16*)ata_internal_buf_get(qc->cursg,qc->n_elem,0);
	//131072 = 
	buf[0] = 0;
	buf[1] = 256;//the cyls
	buf[3] = 4;//the sum of heads
	buf[6] = 128;//the sum of sectors
	//这里存在时序的问题
	memcpy(&buf[10],serial,sizeof(serial));
	memcpy(&buf[23],fwrev,sizeof(fwrev));
	memcpy(&buf[27],module,sizeof(module));

	buf[47] = 0x8000;
	buf[48] = 0x0;
#if 0
	buf[49] = 0x0;// 1 << 9;//ata_id_has_lba
#else
	buf[49] = 1 << 9;
#endif
	buf[50] = 0x0;
	memset(&buf[51],0,2);
#if 0
	buf[53] = 0x0;	//ata_id_current_chs_valid()
	memset(&buf[54],0,5);
#else
	buf[53] = 1 << 0;	//is valid
	buf[54] = 256;		//cyls
	buf[55] = 4;		//heads
	buf[56] = 128;		//sectors
#endif
	buf[59] = 0x0;
	buf[60] = (ATA_MEM_CAPABILITY >> 9) & 0xFFFF;			//
	buf[61] = ((ATA_MEM_CAPABILITY >> 9) >> 16) & 0xFFFF;
	buf[62] = 0x0;
	buf[63] = 0x0;
	buf[64] = 0x0;
	buf[65] = 0x0;
	buf[66] = 0x0;
	buf[67] = 0x0;
	buf[68] = 0x0;
	buf[69] = (~(1 << 6)) | (1 << 10) | (1 << 11);
	buf[70] = 0x0;
	memset(&buf[71],0,4);

	buf[75] = 32;
	buf[76] = (1 << 2) | (1 << 8);
	buf[77] = (~(1 << 0)) | (1 << 4) | (1 << 5) | (1 << 6);
	buf[78] = (1 << 3) | (1 << 4);
	buf[79] = 0x0;
	buf[80] = (1 << 6);
	buf[81] = 0x1;
	buf[82] = (1 << 12) | (1 << 13);
	buf[83] = (~(1<<10)) | (1 << 13);
	buf[84] = (1 << 4) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 14);
	buf[85] = (1 << 4) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 14);
	buf[86] = (~(1 << 10)) | (1 << 12) | (~(1 << 15));
	buf[87] = (1 << 8) | (1 << 14) | (1 << 15);
	buf[88] = (1 << 1);
	buf[89] = (1 << 15) | (1 << 7);
	buf[90] = (1 << 15) | (1 << 7);
	buf[91] = 0x0;
	buf[92] = 0x0;
	buf[93] = 0x0;
	buf[94] = 0x0;
	buf[95] = 0x0;
	buf[96] = 0x0;
	buf[97] = 0x0;
	memset(&buf[98],0,2);
	/*Number of User Addressable Logical Sectors*/
	buf[100] = (ATA_MEM_CAPABILITY >>9) & 0xFFFF;
	buf[101] = ((ATA_MEM_CAPABILITY >> 9)>> 16) & 0xFFFF;
	buf[102] = ((ATA_MEM_CAPABILITY >> 9) >> 32) & 0xFFFF;
	buf[103] = ((ATA_MEM_CAPABILITY >> 9) >> 48) & 0xFFFF;
	buf[104] = 0x0;
	buf[105] = 0x0;
	buf[106] = (1 << 14) | (~(1 << 15));
	buf[107] = 0x0;
	memset(&buf[108],0,4);
	memset(&buf[112],0,4);
	buf[116] = 0x0;
	buf[117] = ATA_MEM_SECTOR_SIZE & 0xFFFF;
	buf[118] = (ATA_MEM_SECTOR_SIZE >> 16) & 0xFFFF;
	buf[119] = (1 << 14);
	buf[120] = (1 << 14);
	memset(&buf[121],0,7);
	buf[127] = 0x0;
	buf[128] = 0x0;
	memset(&buf[129],0,20);
	memset(&buf[160],0,8);
	buf[168] = 0x0;
	buf[169] = 0x0;
	memset(&buf[170],0,4);
	memset(&buf[174],0,2);
	memset(&buf[176],0,30);
	buf[206] = 0x0;
	memset(&buf[207],0,2);
	buf[209] = (1 << 14);
	memset(&buf[210],0,2);
	memset(&buf[212],0,2);
	memset(&buf[214],0,2);
	buf[217] = 0x0;
	buf[218] = 0x0;
	buf[219] = 0x0;
	buf[220] = 0x0;
	buf[221] = 0x0;
	buf[222] = 0x0;
	buf[223] = 0x0;
	memset(&buf[224],0,6);
	memset(&buf[230],0,4);
	buf[234] = 0x0;
	buf[235] = 0x0;
	memset(&buf[236],0,19);
	buf[255] = 0x0;

	ata_internal_buf_put(qc->cursg,qc->n_elem,1);

	return 0;
}

int ata_cmnd_init_dev_params_handle(void* arg1,void* arg2)
{
	ATA_PLog("enter...");
	struct ata_queued_cmd* qc = (struct ata_queued_cmd*)arg1;
	struct ata_taskfile* tf = &qc->tf;
	ATA_PLog("commnad:0x%x",tf->command);
	ATA_PLog("protocol:0x%x",tf->protocol);
	ATA_PLog("heads:0x%x,sectors:0x%x",tf->device,tf->nsect);
	return 0;
}

int ata_cmnd_pio_read_handle(void* arg1,void* arg2)
{
	//ATA_PLog("enter...");
	u64 block;
	u32 n_block;
	struct ata_queued_cmd* qc = (struct ata_queued_cmd*)arg1;
	
	parse_hd_address(qc,&block,&n_block);
	sg_copy_from_buffer(qc->cursg,qc->n_elem,
		get_device_buf()+(block << 9), n_block  << 9);
	return 0;
}
int ata_cmnd_pio_write_handle(void* arg1,void* arg2)
{
	//ATA_PLog("enter...");
	u64 block;
	u32 n_block;
	struct ata_queued_cmd* qc = (struct ata_queued_cmd*)arg1;
	
	parse_hd_address(qc,&block,&n_block);
	sg_copy_to_buffer(qc->cursg, qc->n_elem,
				    get_device_buf()+(block << 9), n_block << 9);
	return 0;
}