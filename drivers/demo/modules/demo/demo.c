#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>

static int __init demo_module_init(void)
{
    printk("[:%s%s:%d]\n",__FILE__,__func__,__LINE__);
    return  0;
}

static void __exit demo_module_exit(void)
{
    printk("[:%s%s:%d]\n",__FILE__,__func__,__LINE__);
}

module_init(demo_module_init);
module_exit(demo_module_exit);
MODULE_LICENSE("GPL");

