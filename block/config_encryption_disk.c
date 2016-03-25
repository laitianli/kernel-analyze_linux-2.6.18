/*
 * Scatterlist Cryptographic API.
 *
 * Procfs information.
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * Copyright (c) 2005 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 */
#include <linux/init.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include "config_encryption_disk.h"

#define ENCRYTION_MAX_NAME 64

LIST_HEAD(encryption_disk_list);
DECLARE_RWSEM(encryption_disk_sem);

struct encryption_disk {
	struct list_head list;
	char name[ENCRYTION_MAX_NAME];
	int 	be_free;
};
#define MAX_ENCRYPTION_DISK_COUNT	30	/* 加密磁盘的最大个数 */
static struct encryption_disk disk_name[MAX_ENCRYPTION_DISK_COUNT];

static int be_parse_cmdline = 1;

#define ELock()  do{	\
	if(be_parse_cmdline != 1)\
		down_write(&encryption_disk_sem);\
}while(0)

#define EUnlock() do{ \
	if(be_parse_cmdline != 1)\
		up_write(&encryption_disk_sem);\
}while(0)

static int find_free_disk_entry(void)
{
	int i = 0;
	for (i = 0; i < MAX_ENCRYPTION_DISK_COUNT; i++)
	{
		if(disk_name[i].be_free == 0) {			
			disk_name[i].be_free = 1;
			return i;
		}
	}
	return -1;
}

static void insert_disk_name_list(const char* partition_name)
{
	struct encryption_disk * pos = NULL;
	int i = 0;
	if(!partition_name)
		return ;
	
	i = find_free_disk_entry();	
	strcpy(disk_name[i].name, partition_name);
	printk(KERN_DEBUG"[%s] insert partition name [%s].\n",__func__ , partition_name);
	INIT_LIST_HEAD(&disk_name[i].list);
	
	//ELock();
	list_for_each_entry(pos, &encryption_disk_list, list) {
		if(!strcmp(pos->name, partition_name)) {
			printk(KERN_DEBUG" [%s] disk name has insert into list.\n", partition_name);
			return ;
		}
	}
	list_add_tail(&disk_name[i].list, &encryption_disk_list);
	//EUnlock();
}

static int  encryption_disk_setup(char *str)
{
	static int first_init = 1;
	char* p = str, *q = NULL; 
	if(first_init==1){
		INIT_LIST_HEAD(&encryption_disk_list);
		be_parse_cmdline = 0;
		first_init = 0;
	}	
	
	while(p && (q = strstr(p,","))) {
		*q = '\0';
		insert_disk_name_list(p);
		p = q + 1;
	}
	insert_disk_name_list(p) ;
	
	return 1;
}

__setup("encryption_disk_name=", encryption_disk_setup);

static void delete_disk_name_list(const char* partition_name)
{
	struct encryption_disk * pos = NULL;
	if(!partition_name)
		return ;

	list_for_each_entry(pos, &encryption_disk_list, list) {
		if(!strcmp(pos->name, partition_name)) {
			list_del(&pos->list);
			pos->be_free = 0;
			memset(pos->name, 0 ,sizeof(pos->name));
			INIT_LIST_HEAD(&pos->list);
			return ;
		}
	}
}

static void delete_encryption_disk(char* str)
{
	char* p = str, *q = NULL; 
	while(p && (q = strstr(p,","))) {
		*q = '\0';
		delete_disk_name_list(p);
		p = q + 1;
	}
	delete_disk_name_list(p) ;
}
static void *c_start(struct seq_file *m, loff_t *pos)
{
	struct list_head *v;
	loff_t n = *pos;

	//down_read(&encryption_disk_sem);
	list_for_each(v, &encryption_disk_list)
		if (!n--)
			return list_entry(v, struct encryption_disk, list);
	return NULL;
}

static void *c_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct list_head *v = p;
	
	(*pos)++;
	v = v->next;
	return (v == &encryption_disk_list) ?
		NULL : list_entry(v, struct encryption_disk, list);
}

static void c_stop(struct seq_file *m, void *p)
{
	//up_read(&encryption_disk_sem);
}

static int c_show(struct seq_file *m, void *p)
{
	struct encryption_disk *disk = (struct encryption_disk *)p;
	
	seq_printf(m, "name         : %s\n", disk->name);
	//seq_printf(m, "is_encrytion_dist         : %d\n", is_encrytion_dist(disk->name));
	//seq_printf(m, "is_encrytion_dist         : %d\n", is_encrytion_dist("aaaa"));
	return 0;
}

static struct seq_operations encrytion_disk_seq_ops = {
	.start		= c_start,
	.next		= c_next,
	.stop		= c_stop,
	.show		= c_show
};

static int encryption_disk_info_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &encrytion_disk_seq_ops);
}

static ssize_t encryption_disk_info_write(struct file *file, const char __user *buf,
				    size_t size, loff_t *_pos)
{
	char *name;
	long ret = 0;

	/* start by dragging the command into memory */
	if (size <= 1 || size >= PAGE_SIZE || size > ENCRYTION_MAX_NAME-1)
		return -EINVAL;

	name = kmalloc(size, GFP_KERNEL);
	if (!name)
		return -ENOMEM; 

	if ( copy_from_user((void *)name, buf, size) != 0)
		goto done;
	name[size-1] = '\0';
	if (name[0]=='-' && name[1]=='d') { /* 删除操作 */
		delete_encryption_disk(&name[3]); /* name[2]为空格 */
	}
	else { /* 添加操作 */
		encryption_disk_setup(name); 
	}
	ret = size;
done:
	kfree(name);
	return (ssize_t)ret;
}

int is_encrytion_disk(const char *name)
{
	struct list_head *v;
	struct encryption_disk *disk;

//	down_read(&encryption_disk_sem);
	list_for_each(v, &encryption_disk_list)
	{
		disk = list_entry(v, struct encryption_disk, list);
		if(!strcmp(disk->name, name))
		{
			//up_read(&encryption_disk_sem);
			return 1;
		}
	}
//	up_read(&encryption_disk_sem);
	return 0;
}
        
static struct file_operations proc_encryption_disk_ops = {
	.open		= encryption_disk_info_open,
	.read		= seq_read,
	.write	= encryption_disk_info_write,
	.llseek		= seq_lseek,
	.release	= seq_release
};

static int __init encryption_disk_init_proc(void)
{
	struct proc_dir_entry *proc;
	int ret;
	ret = 0;
	proc = create_proc_entry("encryption_disk_name", 0, NULL);
	if (proc)
	{
		proc->proc_fops = &proc_encryption_disk_ops;
		ret =1;
	}
	printk(KERN_INFO"create /proc/encryption_disk_name file success.\n");
	return ret;
}

static void __exit encrytion_disk_destroy_proc(void)
{
	//struct list_head *v;
	struct encryption_disk *disk, *tmp_disk;

	//down_write(&encryption_disk_sem);
	list_for_each_entry_safe(disk, tmp_disk, &encryption_disk_list, list)
	{
		//disk = list_entry(v, struct encryption_disk, list);
		//if(disk != NULL)
		//{
			list_del(&disk->list);
			kfree(disk);
		//}
	}
	//up_write(&encryption_disk_sem);
	remove_proc_entry("encryption_disk_name", NULL);
	
}

#if 0
module_init(encryption_disk_init_proc);
module_exit(encryption_disk_destroy_proc);
#else
subsys_initcall(encryption_disk_init_proc);
#endif


MODULE_AUTHOR("xiaosan.li <lixiaoshan_18899@163.com>");
MODULE_DESCRIPTION("encryption disk for kernel 2.6.32");
MODULE_LICENSE("GPL");
