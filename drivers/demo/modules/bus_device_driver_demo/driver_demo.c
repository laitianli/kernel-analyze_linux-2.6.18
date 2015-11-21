#include "driver_demo.h"
#include "bus_demo.h"
#include "device_demo.h"

static int driver_demo_probe(struct device* dev)
{
	struct device_demo* pdev = container_of(dev,struct device_demo,dev);
	struct driver_demo* pdrv = container_of(dev->driver,struct driver_demo,drv);

	return pdrv->probe(pdev);
}

static int driver_demo_remove(struct device* dev)
{
	struct device_demo* pdev = container_of(dev,struct device_demo,dev);
	struct driver_demo* pdrv = container_of(dev->driver,struct driver_demo,drv);

	return pdrv->remove(pdev);
}

int driver_demo_register(struct driver_demo* pdrv)
{
	if(!pdrv)
		return -1;
	pdrv->drv.bus = get_bus_demo();

	if(pdrv->probe)
		pdrv->drv.probe = driver_demo_probe;

	if(pdrv->remove)
		pdrv->drv.remove = driver_demo_remove;

	return driver_register(&pdrv->drv);
}

void driver_demo_unregister(struct driver_demo* pdrv)
{
	if(!pdrv)
		return ;

	driver_unregister(&pdrv->drv);
}

