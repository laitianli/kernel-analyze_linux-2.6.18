#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi.h>
#include <linux/libata.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ata_cmnd_handle.h"

#define ATA_MEM_PROC_NAME 	"ata-mem-proc"
#define PLATFORM_NAME		"ata-mem"
#define ATA_MEM_PORT_NUM	1
#define ATA_MEM_VIRTUAL_IRQ_NUM	 25



static void ata_mem_platform_release(struct device* dev);
static int ata_mem_platform_probe(struct platform_device* pdev);
static int ata_mem_platform_remove(struct platform_device* pdev);


static void ata_mem_qc_prep(struct ata_queued_cmd *qc);
static unsigned int ata_mem_qc_issue(struct ata_queued_cmd *qc);
static bool ata_mem_qc_fill_rtf(struct ata_queued_cmd* qc);
static void ata_mem_freeze(struct ata_port *ap);

static int ata_mem_softreset(struct ata_link *link, unsigned int *class, unsigned long deadline);
static int ata_mem_hardreset(struct ata_link *link, unsigned int *class,unsigned long deadline);
static void ata_mem_postreset(struct ata_link *link, unsigned int *class);
static void ata_mem_error_handler(struct ata_port *ap);
static void ata_mem_post_internal_cmd(struct ata_queued_cmd *qc);
static void ata_mem_dev_config(struct ata_device *dev);
static int ata_mem_scr_read(struct ata_link *link, unsigned int sc_reg, u32 *val);
static int ata_mem_scr_write(struct ata_link *link, unsigned int sc_reg, u32 val);
static int ata_mem_port_start(struct ata_port *ap);
static void ata_mem_port_stop(struct ata_port *ap);

static struct scsi_host_template g_ata_mem_sht = {
	ATA_NCQ_SHT(ATA_MEM_PROC_NAME),						
	.can_queue		= 32 - 1,			
	.sg_tablesize		= 168,				
	.dma_boundary		= 0xffffffff,			
};

struct ata_port_operations ata_mem_ops = {
	.inherits		= &sata_pmp_port_ops,
	.qc_prep		= ata_mem_qc_prep,
	.qc_issue		= ata_mem_qc_issue,
	.qc_fill_rtf	= ata_mem_qc_fill_rtf,
	.freeze			= ata_mem_freeze,
	.softreset		= ata_mem_softreset,
	.hardreset		= ata_mem_hardreset,
	.postreset		= ata_mem_postreset,
	.error_handler	= ata_mem_error_handler,
	.post_internal_cmd = ata_mem_post_internal_cmd,
	.dev_config			= ata_mem_dev_config,
	.scr_read			= ata_mem_scr_read,
	.scr_write			= ata_mem_scr_write,
	.port_start			= ata_mem_port_start,
	.port_stop			= ata_mem_port_stop,
};

static const struct ata_port_info ata_mem_port_info = {
		.flags		= ATA_FLAG_SATA | ATA_FLAG_NO_LEGACY 
					 /*| ATA_FLAG_MMIO  | ATA_FLAG_PIO_DMA*/,
		.pio_mask	= ATA_PIO0,
		.mwdma_mask = ATA_MWDMA0,
		.udma_mask	= ATA_UDMA0,
		.port_ops	= &ata_mem_ops,
};

static void ata_mem_qc_prep(struct ata_queued_cmd *qc)
{
	//ATA_PLog("command:0x%x",qc->tf.command);
	/*unsigned int size = get_cmnd_array_size();
	unsigned int i = 0;
	for(i = 0; i < size; i++)
	{
		if(qc->tf.command == g_handle[i].cmd)
			g_handle[i].handle(qc,NULL);
	}*/
}

static unsigned int ata_mem_qc_issue(struct ata_queued_cmd *qc)
{
	if(qc->tf.command != 0x20 && qc->tf.command != 0x30)
		ATA_PLog("command:0x%x",qc->tf.command);
	unsigned int size = get_cmnd_array_size();
	unsigned int i = 0;
	for(i = 0; i < size; i++)
	{
		if(qc->tf.command == g_handle[i].cmd)
		{
			g_handle[i].handle(qc,NULL);
			ata_qc_complete(qc);
			break;
		}
	}

	return 0;
}

static bool ata_mem_qc_fill_rtf(struct ata_queued_cmd* qc)
{
	ATA_PLog("enter...");

	return 1;
}
static void ata_mem_freeze(struct ata_port *ap)
{
	ATA_PLog("enter...");
}

static int ata_mem_softreset(struct ata_link *link, unsigned int *class, unsigned long deadline)
{
	ATA_PLog("enter...");
	*class = ATA_DEV_ATA;
	return 0;
}
static int ata_mem_hardreset(struct ata_link *link, unsigned int *class,unsigned long deadline)
{
	ATA_PLog("enter...");
	*class = ATA_DEV_ATA;
	return 0;
}
static void ata_mem_postreset(struct ata_link *link, unsigned int *class)
{
	ATA_PLog("enter...");
	ata_std_postreset(link,class);
}
static void ata_mem_error_handler(struct ata_port *ap)
{
	ATA_PLog("enter...");
	sata_pmp_error_handler(ap);
}
static void ata_mem_post_internal_cmd(struct ata_queued_cmd *qc)
{
	ATA_PLog("enter...");
}
static void ata_mem_dev_config(struct ata_device *dev)
{
	ATA_PLog("enter...");
}
static int ata_mem_scr_read(struct ata_link *link, unsigned int sc_reg, u32 *val)
{
	ATA_PLog("enter...sc_reg=%d",sc_reg);
	switch(sc_reg)
	{
	case SCR_STATUS:
		*val = 0x123;
		break;
	case SCR_CONTROL:
		*val = 0x300;
		break;
	case SCR_ACTIVE:
		*val = 0;
		break;
	default:
		break;
	}
	return 0;
}
static int ata_mem_scr_write(struct ata_link *link, unsigned int sc_reg, u32 val)
{
	ATA_PLog("enter...sc_reg:%d",sc_reg);
	
	return 0;
}
static int ata_mem_port_start(struct ata_port *ap)
{
	ATA_PLog("enter...");
#if SMALL_MEM > 0
	g_ata_mem_buf = (char*)kmalloc(ATA_MEM_CAPABILITY,GFP_KERNEL);
	if(!g_ata_mem_buf)
	{
		ATA_PLog("[Error] kmalloc failed.");
		return -ENOMEM;
	}	
#endif
	memset(g_ata_mem_buf,0,sizeof(g_ata_mem_buf));
	return 0;
}
static void ata_mem_port_stop(struct ata_port *ap)
{
	ATA_PLog("enter...");
#if SMALL_MEM > 0	
	if(g_ata_mem_buf)
		kfree((void*)g_ata_mem_buf);
#endif	
}


static void ata_mem_platform_release(struct device* dev)
{
	ATA_PLog("ata_mem release.");
}
static int ata_mem_platform_probe(struct platform_device* pdev)
{
	ATA_PLog("probe.");
	int rc = 0;
	
	const struct ata_port_info *ppi[] = { &ata_mem_port_info, NULL };
	struct ata_host* host = ata_host_alloc_pinfo(&pdev->dev,ppi,ATA_MEM_PORT_NUM);
	if(!host)
	{
		ATA_PLog("[Error] ata_host alloc_pinfo failed.");
		return -1;
	}
	dev_set_drvdata(&pdev->dev,host);
	rc = ata_host_start(host);
	if(rc)
	{
		ATA_PLog("[Error] ata_host_start failed.");
		return rc;
	}
	set_port_name(host,"MEM");
	rc = ata_host_register(host, &g_ata_mem_sht);
	if(rc)
	{
		ATA_PLog("[Error] ata_host_register failed.");
		return rc;
	}
	
	return 0;
}
static int ata_mem_platform_remove(struct platform_device* pdev)
{
	ATA_PLog("remove");
	struct ata_host* host = dev_get_drvdata(&pdev->dev);
	ata_host_detach(host);
	return 0;
}

static struct platform_device ata_mem_dev = {
	.dev = {
		.release = ata_mem_platform_release,
		},
	.name = PLATFORM_NAME,
};


static struct platform_driver ata_mem_drv = {
	.driver = {
		.name = PLATFORM_NAME,
		},
	.probe = ata_mem_platform_probe,
	.remove = ata_mem_platform_remove,
};

static int __init ata_mem_init(void)
{
	int ret = platform_device_register(&ata_mem_dev);
	if(!ret)
	{	
		ret = platform_driver_register(&ata_mem_drv);
		return ret;
	}
	return -1;
}

static void __exit ata_mem_exit(void)
{
	platform_device_unregister(&ata_mem_dev);
	platform_driver_unregister(&ata_mem_drv);
}

module_init(ata_mem_init);
module_exit(ata_mem_exit);
MODULE_LICENSE("GPL");


