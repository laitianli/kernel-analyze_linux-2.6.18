#ifndef _KOBJECT_DEMO_H_
#define _KOBJECT_DEMO_H_

#include <linux/kobject.h>

struct demo_obj
{
	struct kobject obj;
	int 	count;
};

struct demo_attribute
{
	struct attribute attr;
	ssize_t (*show)(struct demo_obj* obj,struct demo_attribute* attr,char* buf);
	ssize_t (*store)(struct demo_obj* obj,struct demo_attribute* attr,const char* buf,size_t count);
};


#define to_demo_attr(ap) container_of(ap,struct demo_attribute,attr)


#endif
