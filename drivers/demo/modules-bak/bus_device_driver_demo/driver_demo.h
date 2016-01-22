#ifndef _DRIVER_DEMO_H_
#define _DRIVER_DEMO_H_
#include <linux/device.h>
struct driver_demo
{
	struct device_driver drv;
	int (*probe)(struct driver_demo* );
	int (*remove)(struct driver_demo* );
};

int driver_demo_register(struct driver_demo* pdrv);

void driver_demo_unregister(struct driver_demo* pdrv);

#endif

