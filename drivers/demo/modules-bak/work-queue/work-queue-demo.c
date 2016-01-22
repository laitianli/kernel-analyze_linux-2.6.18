#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/compile.h>
#include <linux/kthread.h>

#define WORKQUEUE_NAME "workqueue-demo"
#define PROC_NAME WORKQUEUE_NAME
#define START "s"
#define END "e"
static struct workqueue_struct* gp_workqueue = NULL;
static struct work_struct g_work = {0};

static struct proc_dir_entry* gp_proc_entry = NULL;
static struct completion work_completion = {0};
static struct task_struct* gp_work_thread = NULL;

static unsigned int g_work_thread_tag = 1;

static int open_workqueue_demo(struct inode* p_node,struct file* pf)
{
	LogPath();
	return 0;
}

static int close_workqueue_demo(struct inode* p_node,struct file* pf)
{
	LogPath();
	return 0;
}

ssize_t read_workqueue_demo(struct file* pf,char __user* buf,size_t len,loff_t *pos)
{
	LogPath();
	return 0;
}


ssize_t write_workqueue_demo(struct file* pf,const char __user* buf,size_t len,loff_t* pos)
{
	char* tmpbuf = kmalloc(len* sizeof(size_t) + 1,GFP_KERNEL);
	if(!tmpbuf)
		return -1;
	memset(tmpbuf,0,len * sizeof(size_t) + 1);
	copy_from_user(tmpbuf,buf,len);
	if(!strncmp(tmpbuf,START,strlen(START)))
	{
		Log("start...");
		g_work_thread_tag = 1;
		schedule_work(&g_work);//cal function work_demo_handle again...
	//	complete(&work_completion);
	wake_up_process(gp_work_thread);

	}
	else if(!strncmp(tmpbuf,END,strlen(END)))
	{
		Log("end...");
		g_work_thread_tag = 0;
		complete_all(&work_completion);

	}
	kfree(tmpbuf);
	return len;
}

static struct file_operations g_fop = {
	.owner = THIS_MODULE,
	.open = open_workqueue_demo,
	.release = close_workqueue_demo,
	.read = read_workqueue_demo,
	.write = write_workqueue_demo,
};

static void work_demo_handle(void* parg)
{
	Log("do work handle...");

}

static int work_thread_handle(void* data)
{
	/*
	while(1)
	{
		if(g_work_thread_tag <= 0)
			break;
		Log("before wait_for_completion...");

		wait_for_completion(&work_completion);
		Log("run thread handle...");
	}*/
	set_current_state(TASK_INTERRUPTIBLE);
	while(!kthread_should_stop())
	{
		if(g_work_thread_tag == 0)
		{
			schedule();
			set_current_state(TASK_INTERRUPTIBLE);
			continue;
		}

		__set_current_state(TASK_RUNNING);
		Log("run work thread....");
		g_work_thread_tag = 0;
		set_current_state(TASK_INTERRUPTIBLE);
		
	}

	Log("kthread exit...");
}

static int __init queue_work_demo_init(void)
{
	int ret = 0;
	gp_workqueue = create_singlethread_workqueue(WORKQUEUE_NAME);
	if(gp_workqueue == NULL)
	{
		Log("[error] call function create_singlethread_workqueue failed.");
		return -1;
	}

	INIT_WORK(&g_work,work_demo_handle,gp_workqueue);
	queue_work(gp_workqueue,&g_work);//will call work_demo_handle function....
	gp_proc_entry = create_proc_entry(PROC_NAME,0755,NULL);
	if(gp_proc_entry==NULL)
	{
		Log("[Error] call function create_proc_entry failed.");
		ret = -1;
		goto ERROR;
	}
	gp_proc_entry->proc_fops = &g_fop;

	gp_work_thread = kthread_create(work_thread_handle,NULL,"work-thread");
	if(!gp_work_thread)
	{
		Log("[erro] kthread_create failed.");
		goto ERROR;
	}
	wake_up_process(gp_work_thread);

	init_completion(&work_completion);
	return 0;
ERROR:
	if(gp_proc_entry)
		remove_proc_entry(PROC_NAME,NULL);
	destroy_workqueue(gp_workqueue);
	return ret;
}

static void __exit queue_work_demo_exit(void)
{
	LogPath();
	kthread_stop(gp_work_thread);
	remove_proc_entry(PROC_NAME,NULL);
	flush_workqueue(gp_workqueue);
	destroy_workqueue(gp_workqueue);
}

module_init(queue_work_demo_init);
module_exit(queue_work_demo_exit);
MODULE_LICENSE("GPL");

