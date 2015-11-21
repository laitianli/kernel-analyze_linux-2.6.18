#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kdev_t.h>
static struct class* g_class            = NULL;
static struct class_device* g_class_dev = NULL;

static void device_release(struct device* dev)
{
    LogPath();
}

static struct device        g_dev = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18)
    .init_name = "device-demo",
#endif
    .release = device_release,
};


static int __init class_demo_init(void)
{
    Log("enter fun.");
    device_initialize(&g_dev);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
    strcpy(g_dev.bus_id,"device-demo");
#endif    
    device_add(&g_dev);
    g_class = class_create(THIS_MODULE,"class-demo");
    if(!g_class)
        return -1;
    g_class_dev = class_device_create(g_class,NULL,MKDEV(0,0),&g_dev,"class_demo0");
    if(!g_class_dev)
        return -1;
    return 0;
}

static void __exit class_demo_exit(void)
{
    Log("enter exit fun.");
    device_del(&g_dev);
    put_device(&g_dev);
    class_device_unregister(g_class_dev);
    class_destroy(g_class);
}

module_init(class_demo_init);
module_exit(class_demo_exit);

MODULE_LICENSE("GPL");
