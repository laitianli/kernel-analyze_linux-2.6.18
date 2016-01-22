#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>

#include "scsi_mem_host.h"

#define PLATFORM_NAME "scsi-mem"

void sm_platform_release_dev(struct device* dev);
int sm_platform_probe(struct platform_device* dev);
int sm_platform_remove(struct platform_device* dev);

static struct platform_device sm_platform_dev = {
    .name = PLATFORM_NAME,
    .dev = {
        .release = sm_platform_release_dev,
    },
};


static struct platform_driver sm_platform_drv = {
    .driver = {
        .name = PLATFORM_NAME,
        },    
    .probe = sm_platform_probe,
    .remove = sm_platform_remove,
};

void sm_platform_release_dev(struct device* dev)
{
    LogPath();
}
int sm_platform_probe(struct platform_device* dev)
{
    LogPath();
    struct Scsi_Host* pmHost = scsi_host_alloc(&g_scsi_mem_drv_template,sizeof(g_scsi_mem_drv_template));
    if(!pmHost)
    {
        Log("[Error] scsi_host_alloc failed.");
        return -1;
    }
    pmHost->io_port = 0;
    pmHost->n_io_port = 0;
    pmHost->irq = 0;
    pmHost->max_cmd_len = 16;
    pmHost->max_id = 1;
    pmHost->max_lun = 1;
    pmHost->max_channel = 1;
	pmHost->unique_id = 1;
    int err = scsi_add_host(pmHost,&dev->dev);
    if(err < 0)
    {
        Log("[Error] scsi_add_host failed.");
        goto ERROR;
    }
  //  scsi_scan_host(pmHost);
  	struct scsi_device* pdev = __scsi_add_device(pmHost,0,0,0,NULL);
  	if(!pdev)
  	{
  		Log("[Error] rescan error;");
		goto ERROR;
  	}
    return 0;
ERROR:
    scsi_host_put(pmHost);
    return err;
}
int sm_platform_remove(struct platform_device* dev)
{
    LogPath();
    return 0;
}
static int __init scsi_mem_init(void)
{
    int ret = platform_device_register(&sm_platform_dev);
    if(ret < 0)
    {
        Log("[Error] platform_device_register failed.");
        return ret;
    }

    ret = platform_driver_register(&sm_platform_drv);
    if(ret < 0)
    {
        Log("[Error] platform_driver_register failed.");
        goto ERROR;
    }
    return 0;
ERROR:
    platform_device_unregister(&sm_platform_dev);
    return ret;
}

static void __exit scsi_mem_exit(void)
{
    platform_device_unregister(&sm_platform_dev);
    platform_driver_unregister(&sm_platform_drv);
}

module_init(scsi_mem_init);
module_exit(scsi_mem_exit);

MODULE_LICENSE("GPL");

