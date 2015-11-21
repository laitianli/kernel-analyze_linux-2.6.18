#include <linux/device.h>
#include "bus_demo.h"
#include "device_demo.h"
#include "driver_demo.h"

#define BUS_NAME 	"bus_demo"

static int bus_demo_match(struct device* dev,struct device_driver* drv)
{
	struct device_demo* ddev = container_of(dev,struct device_demo,dev);

	if(!strncmp(ddev->name,drv->name,BUS_ID_SIZE))
		return 1;
	return 0;
}

static struct bus_type g_bus_demo = {
	.name = BUS_NAME,
	.match = bus_demo_match,
};

void dev_demo_bus_release(struct device* dev)
{
	LogPath();
}

static struct device g_dev_demo_bus = {
	.bus_id = "dev-demo",
	.release = dev_demo_bus_release,
};
int bus_demo_register()
{
	device_register(&g_dev_demo_bus);
	return bus_register(&g_bus_demo);
}

void bus_demo_unregister()
{
	device_unregister(&g_dev_demo_bus);
	bus_unregister(&g_bus_demo);
}

struct bus_type* get_bus_demo(void)
{
	return &g_bus_demo;
}

struct device* get_bus_demo_dev(void)
{
	return &g_dev_demo_bus;
}
