#include <linux/module.h>
#include <linux/init.h>

#include "bus_demo.h"
#include "device_demo.h"
#include "driver_demo.h"

#define DEVICE_NAME "my-demo"
#define DRIVER_NAME "my-demo"

void my_device_demo_release(struct device* pdev)
{
	LogPath();
}

static struct device_demo g_my_dev_demo = {
	.name = 	DEVICE_NAME,
	.dev = {
			.release = my_device_demo_release,
		},
};


static int my_driver_demo_probe(struct device_demo* pdev)
{
	if(!pdev)
		return -1;

	LogPath();
	return 0;
}

static int my_driver_demo_remove(struct device_demo* pdev)
{
	if(!pdev)
		return -1;
	LogPath();
	return 0;
}

static struct driver_demo g_my_drv_demo = {
	.drv = {
				.name = DRIVER_NAME,
				.owner = THIS_MODULE,
			},
	.probe = my_driver_demo_probe,
	.remove = my_driver_demo_remove,
};

static int __init driver_module_demo_init(void)
{
	int ret = 0;
	ret = bus_demo_register();
	if(ret)
	{
		Log("call fun bus_demo_register error!");	
		return -1;
	}
	ret = device_demo_register(&g_my_dev_demo);
	if(ret)
	{
		Log("call fun device_demo_register error!");
		goto ERROR_DEV;
	}

	ret = driver_demo_register(&g_my_drv_demo);
	if(ret)
	{
		Log("call fun driver_demo_register error!");
		goto ERROR_DRV;
	}
	return 0;
ERROR_DRV:
	device_demo_unregister(&g_my_dev_demo);
ERROR_DEV:
	bus_demo_unregister();
	return -1;
}

static void __exit driver_module_demo_exit(void)
{
	device_demo_unregister(&g_my_dev_demo);
	driver_demo_unregister(&g_my_drv_demo);
	bus_demo_unregister();
}

module_init(driver_module_demo_init);
module_exit(driver_module_demo_exit);

MODULE_LICENSE("GPL");
