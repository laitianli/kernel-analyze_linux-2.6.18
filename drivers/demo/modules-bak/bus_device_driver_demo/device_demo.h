#ifndef _DEVICE_DEMO_H_
#define _DEVICE_DEMO_H_
#include <linux/device.h>
struct device_demo
{
	char* name;
	struct device dev;
};

int device_demo_register(struct device_demo* pdev);

void device_demo_unregister(struct device_demo* pdev);

#endif

