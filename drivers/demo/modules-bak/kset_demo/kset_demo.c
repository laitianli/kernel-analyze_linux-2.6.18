#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>

static struct kset g_kset_root  = {0};

static struct kset g_kset_child = {0};

void demo_kobject_release(struct kobject* obj)
{
	printk("[Note]=[%s:%s:%d]==\n",__FILE__,__func__,__LINE__);
}

ssize_t demo_kobject_show(struct kobject* obj,struct attribute* attr,char* buf)
{
	int len = 0;
	printk("[Note]=[%s:%s:%d]==file name:%s\n",__FILE__,__func__,__LINE__,attr->name);
	len = sprintf(buf,"call fun:%s\n",__func__);
	return len;
}

ssize_t demo_kobject_store(struct kobject* obj,struct attribute* attr,const char* buf,size_t count)
{
	int len = 0;
	
	printk("[Note]=[%s:%s:%d]==file name:%s\n",__FILE__,__func__,__LINE__,attr->name);
	if(buf && count > 0)
	{
		printk("len:%d,buf:%s\n",count,buf);
	}
	return count;
}
static struct sysfs_ops g_ops = {
	.show = demo_kobject_show,
	.store = demo_kobject_store,
};
static struct attribute g_attr[] = {
	[0] = {"kobject-demo-1_val_1",THIS_MODULE,0755},
	[1] = {"kobject-demo-1_val_2",THIS_MODULE,0755},
};
static struct kobj_type g_type = {
	.release = demo_kobject_release,
	.sysfs_ops = &g_ops,
};


static struct attribute g_attr_2[] = {
	[0] = {"kobject-demo-2_val_1",THIS_MODULE,0777},
	[1] = {"kobject-demo-2_val_2",THIS_MODULE,0744},
	[2] = {"KOBJECT-demo-2_val_3",THIS_MODULE,0744},
};

static struct kobj_type g_type_2 = {
	.release = demo_kobject_release,
	.sysfs_ops = &g_ops,
//	.default_attrs = &g_attr_2,
};

static struct kobject g_kobj = {0};


static struct kobject g_kobj_2 = {0};

static int __init demo_kset_init(void)
{
	memset(&g_kset_root ,0,sizeof(struct kset));
	memset(&g_kset_child,0,sizeof(struct kset));
	kobject_set_name(&g_kset_child.kobj,"%s","kset_child");
	kobject_set_name(&g_kset_root.kobj,"%s","kset_demo_root");
	int ret = kset_register(&g_kset_root );
	if(ret)
	{
		printk("[Error]=[%s:%s:%d]kset_register function error!\n",__FILE__,__func__,__LINE__);
		return -1;
	}
	g_kset_child.kobj.parent = &g_kset_root.kobj;

	ret = kset_register(&g_kset_child);
	if(ret)
	{
		printk("[Error]=[%s:%s:%d]kset_register function error!\n",__FILE__,__func__,__LINE__);
		return -1;
	}

	memset(&g_kobj,0,sizeof(struct kobject));
	kobject_init(&g_kobj);
	kobject_set_name(&g_kobj,"%s","kobject-demo-1");
	g_kobj.parent = &g_kset_child.kobj;
	g_kobj.ktype = &g_type;
	kobject_add(&g_kobj);

	int i = 0; 
	for(i = 0; i < sizeof(g_attr)/sizeof(struct attribute); i++)
		sysfs_create_file(&g_kobj,&g_attr[i]);

#if 1	

	memset(&g_kobj_2,0,sizeof(struct kobject));
	kobject_init(&g_kobj_2);
	kobject_set_name(&g_kobj_2,"%s","kobject-demo-2");
	g_kobj_2.parent = &g_kset_child.kobj;
	g_kobj_2.ktype = &g_type_2;
	kobject_add(&g_kobj_2);
	
	for(i = 0; i < sizeof(g_attr_2)/sizeof(struct attribute); i++)
		sysfs_create_file(&g_kobj_2,&g_attr_2[i]);
#endif
	//for()


	return 0;
}

static void __exit demo_kset_exit(void)
{
	int i = 0; 
	for(i = 0; i < sizeof(g_attr) / sizeof(struct attribute); i++)
		sysfs_remove_file(&g_kobj,&g_attr[i]);
	kobject_unregister(&g_kobj_2);
	kobject_unregister(&g_kobj);
	kset_unregister(&g_kset_child);
	kset_unregister(&g_kset_root );
}

module_init(demo_kset_init);
module_exit(demo_kset_exit);

MODULE_LICENSE("GPL");
