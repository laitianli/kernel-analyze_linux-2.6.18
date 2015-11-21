#include "device_demo.h"
#include "bus_demo.h"


int device_demo_register(struct device_demo* pdev)
{
	if(!pdev)
		return -1;

	device_initialize(&pdev->dev);

	if(!pdev->dev.parent)
		pdev->dev.parent = get_bus_demo_dev();

	if(strlen(pdev->dev.bus_id) <= 0)
		strncpy(pdev->dev.bus_id,pdev->name,BUS_ID_SIZE);
	pdev->dev.bus = get_bus_demo();

	int ret = 0;

	ret = device_add(&pdev->dev);
	if(ret)
	{
		printk("[Error]=[%s:%s:%d]=call fun device_add failed.\n",__FILE__,__func__,__LINE__);
		return -1;
	}
	return 0;
}

void device_demo_unregister(struct device_demo* pdev)
{
	if(!pdev)
		return -1;

	Log("driver:%p",pdev->dev.driver);
	device_del(&pdev->dev);

	put_device(&pdev->dev);
}

