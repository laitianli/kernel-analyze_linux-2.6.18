/*
 *  fs/partitions/check.c
 *
 *  Code extracted from drivers/block/genhd.c
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 *
 *  We now have independent partition support from the
 *  block drivers, which allows all the partition code to
 *  be grouped in one location, and it to be mostly self
 *  contained.
 *
 *  Added needed MAJORS for new pairs, {hdi,hdj}, {hdk,hdl}
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kmod.h>
#include <linux/ctype.h>

#include "check.h"

#include "acorn.h"
#include "amiga.h"
#include "atari.h"
#include "ldm.h"
#include "mac.h"
#include "msdos.h"
#include "osf.h"
#include "sgi.h"
#include "sun.h"
#include "ibm.h"
#include "ultrix.h"
#include "efi.h"
#include "karma.h"

#ifdef CONFIG_BLK_DEV_MD
extern void md_autodetect_dev(dev_t dev);
#endif

int warn_no_part = 1; /*This is ugly: should make genhd removable media aware*/

static int (*check_part[])(struct parsed_partitions *, struct block_device *) = {
	/*
	 * Probe partition formats with tables at disk address 0
	 * that also have an ADFS boot block at 0xdc0.
	 */
#ifdef CONFIG_ACORN_PARTITION_ICS
	adfspart_check_ICS,
#endif
#ifdef CONFIG_ACORN_PARTITION_POWERTEC
	adfspart_check_POWERTEC,
#endif
#ifdef CONFIG_ACORN_PARTITION_EESOX
	adfspart_check_EESOX,
#endif

	/*
	 * Now move on to formats that only have partition info at
	 * disk address 0xdc0.  Since these may also have stale
	 * PC/BIOS partition tables, they need to come before
	 * the msdos entry.
	 */
#ifdef CONFIG_ACORN_PARTITION_CUMANA
	adfspart_check_CUMANA,
#endif
#ifdef CONFIG_ACORN_PARTITION_ADFS
	adfspart_check_ADFS,
#endif

#ifdef CONFIG_EFI_PARTITION
	efi_partition,		/* this must come before msdos */
#endif
#ifdef CONFIG_SGI_PARTITION
	sgi_partition,
#endif
#ifdef CONFIG_LDM_PARTITION
	ldm_partition,		/* this must come before msdos */
#endif
#ifdef CONFIG_MSDOS_PARTITION
	msdos_partition,
#endif
#ifdef CONFIG_OSF_PARTITION
	osf_partition,
#endif
#ifdef CONFIG_SUN_PARTITION
	sun_partition,
#endif
#ifdef CONFIG_AMIGA_PARTITION
	amiga_partition,
#endif
#ifdef CONFIG_ATARI_PARTITION
	atari_partition,
#endif
#ifdef CONFIG_MAC_PARTITION
	mac_partition,
#endif
#ifdef CONFIG_ULTRIX_PARTITION
	ultrix_partition,
#endif
#ifdef CONFIG_IBM_PARTITION
	ibm_partition,
#endif
#ifdef CONFIG_KARMA_PARTITION
	karma_partition,
#endif
	NULL
};
 
/*
 * disk_name() is used by partition check code and the genhd driver.
 * It formats the devicename of the indicated disk into
 * the supplied buffer (of size at least 32), and returns
 * a pointer to that same buffer (for convenience).
 */

char *disk_name(struct gendisk *hd, int part, char *buf)
{
	if (!part)
		snprintf(buf, BDEVNAME_SIZE, "%s", hd->disk_name);
	else if (isdigit(hd->disk_name[strlen(hd->disk_name)-1]))
		snprintf(buf, BDEVNAME_SIZE, "%sp%d", hd->disk_name, part);
	else
		snprintf(buf, BDEVNAME_SIZE, "%s%d", hd->disk_name, part);

	return buf;
}

const char *bdevname(struct block_device *bdev, char *buf)
{
	int part = MINOR(bdev->bd_dev) - bdev->bd_disk->first_minor;
	return disk_name(bdev->bd_disk, part, buf);
}

EXPORT_SYMBOL(bdevname);

/*
 * There's very little reason to use this, you should really
 * have a struct block_device just about everywhere and use
 * bdevname() instead.
 */
const char *__bdevname(dev_t dev, char *buffer)
{
	scnprintf(buffer, BDEVNAME_SIZE, "unknown-block(%u,%u)",
				MAJOR(dev), MINOR(dev));
	return buffer;
}

EXPORT_SYMBOL(__bdevname);

static struct parsed_partitions *
check_partition(struct gendisk *hd, struct block_device *bdev)
{
	struct parsed_partitions *state;
	int i, res;

	state = kmalloc(sizeof(struct parsed_partitions), GFP_KERNEL);
	if (!state)
		return NULL;

	disk_name(hd, 0, state->name);
	printk(KERN_INFO " %s:", state->name);
	if (isdigit(state->name[strlen(state->name)-1]))
		sprintf(state->name, "p");

	state->limit = hd->minors;
	i = res = 0;
	while (!res && check_part[i]) {
		memset(&state->parts, 0, sizeof(state->parts));
		res = check_part[i++](state, bdev);
	}
	if (res > 0)
		return state;
	if (!res)
		printk(" unknown partition table\n");
	else if (warn_no_part)
		printk(" unable to read partition table\n");
	kfree(state);
	return NULL;
}

/*
 * sysfs bindings for partitions
 */

struct part_attribute {
	struct attribute attr;
	ssize_t (*show)(struct hd_struct *,char *);
	ssize_t (*store)(struct hd_struct *,const char *, size_t);
};

static ssize_t 
part_attr_show(struct kobject * kobj, struct attribute * attr, char * page)
{
	struct hd_struct * p = container_of(kobj,struct hd_struct,kobj);
	struct part_attribute * part_attr = container_of(attr,struct part_attribute,attr);
	ssize_t ret = 0;
	if (part_attr->show)
		ret = part_attr->show(p, page);
	return ret;
}
static ssize_t
part_attr_store(struct kobject * kobj, struct attribute * attr,
		const char *page, size_t count)
{
	struct hd_struct * p = container_of(kobj,struct hd_struct,kobj);
	struct part_attribute * part_attr = container_of(attr,struct part_attribute,attr);
	ssize_t ret = 0;

	if (part_attr->store)
		ret = part_attr->store(p, page, count);
	return ret;
}

static struct sysfs_ops part_sysfs_ops = {
	.show	=	part_attr_show,
	.store	=	part_attr_store,
};

static ssize_t part_uevent_store(struct hd_struct * p,
				 const char *page, size_t count)
{
	kobject_uevent(&p->kobj, KOBJ_ADD);
	return count;
}
static ssize_t part_dev_read(struct hd_struct * p, char *page)
{
	struct gendisk *disk = container_of(p->kobj.parent,struct gendisk,kobj);
	dev_t dev = MKDEV(disk->major, disk->first_minor + p->partno); 
	return print_dev_t(page, dev);
}
static ssize_t part_start_read(struct hd_struct * p, char *page)
{
	return sprintf(page, "%llu\n",(unsigned long long)p->start_sect);
}
static ssize_t part_size_read(struct hd_struct * p, char *page)
{
	return sprintf(page, "%llu\n",(unsigned long long)p->nr_sects);
}
static ssize_t part_stat_read(struct hd_struct * p, char *page)
{
	return sprintf(page, "%8u %8llu %8u %8llu\n",
		       p->ios[0], (unsigned long long)p->sectors[0],
		       p->ios[1], (unsigned long long)p->sectors[1]);
}
static struct part_attribute part_attr_uevent = {
	.attr = {.name = "uevent", .mode = S_IWUSR },
	.store	= part_uevent_store
};
static struct part_attribute part_attr_dev = {
	.attr = {.name = "dev", .mode = S_IRUGO },
	.show	= part_dev_read
};
static struct part_attribute part_attr_start = {
	.attr = {.name = "start", .mode = S_IRUGO },
	.show	= part_start_read
};
static struct part_attribute part_attr_size = {
	.attr = {.name = "size", .mode = S_IRUGO },
	.show	= part_size_read
};
static struct part_attribute part_attr_stat = {
	.attr = {.name = "stat", .mode = S_IRUGO },
	.show	= part_stat_read
};

static struct attribute * default_attrs[] = {
	&part_attr_uevent.attr,
	&part_attr_dev.attr,
	&part_attr_start.attr,
	&part_attr_size.attr,
	&part_attr_stat.attr,
	NULL,
};

extern struct subsystem block_subsys;

static void part_release(struct kobject *kobj)
{
	struct hd_struct * p = container_of(kobj,struct hd_struct,kobj);
	kfree(p);
}

struct kobj_type ktype_part = {
	.release	= part_release,
	.default_attrs	= default_attrs,
	.sysfs_ops	= &part_sysfs_ops,
};

static inline void partition_sysfs_add_subdir(struct hd_struct *p)
{
	struct kobject *k;

	k = kobject_get(&p->kobj);
	p->holder_dir = kobject_add_dir(k, "holders");
	kobject_put(k);
}

static inline void disk_sysfs_add_subdirs(struct gendisk *disk)
{
	struct kobject *k;

	k = kobject_get(&disk->kobj);
	disk->holder_dir = kobject_add_dir(k, "holders");
	disk->slave_dir = kobject_add_dir(k, "slaves");
	kobject_put(k);
}
/**ltl
功能:删除分区
参数:disk:通用磁盘设备描述符
	part:分区号
*/
void delete_partition(struct gendisk *disk, int part)
{
	struct hd_struct *p = disk->part[part-1];
	if (!p)
		return;
	if (!p->nr_sects)
		return;
	disk->part[part-1] = NULL;
	p->start_sect = 0;//起始扇区重置0
	p->nr_sects = 0;//扇区数置0
	p->ios[0] = p->ios[1] = 0;
	p->sectors[0] = p->sectors[1] = 0;
	//删除软链接
	sysfs_remove_link(&p->kobj, "subsystem");
	if (p->holder_dir)
		kobject_unregister(p->holder_dir);
	//向用户空间发送设备删除消息
	kobject_uevent(&p->kobj, KOBJ_REMOVE);
	kobject_del(&p->kobj);
	kobject_put(&p->kobj);
}
/**ltl
功能:把分区添加到gendisk和sys系统中
参数:disk->通用磁盘描述符
	part->分区号
	start->起始扇区
	len->当前分区的扇区总数
*/
void add_partition(struct gendisk *disk, int part, sector_t start, sector_t len)
{
	struct hd_struct *p;

	//分配分区结构
	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return;
	
	memset(p, 0, sizeof(*p));
	p->start_sect = start;//起始扇区
	p->nr_sects = len;		//扇区总数
	p->partno = part;		//分区号
	p->policy = disk->policy;

	/*设备名字*/
	if (isdigit(disk->kobj.name[strlen(disk->kobj.name)-1]))
		snprintf(p->kobj.name,KOBJ_NAME_LEN,"%sp%d",disk->kobj.name,part);
	else
		snprintf(p->kobj.name,KOBJ_NAME_LEN,"%s%d",disk->kobj.name,part);
	/*把当前分区添加到sys系统中*/
	p->kobj.parent = &disk->kobj;
	p->kobj.ktype = &ktype_part;
	kobject_init(&p->kobj);
	kobject_add(&p->kobj);
	if (!disk->part_uevent_suppress)
		kobject_uevent(&p->kobj, KOBJ_ADD);
	sysfs_create_link(&p->kobj, &block_subsys.kset.kobj, "subsystem");
	partition_sysfs_add_subdir(p);
	disk->part[part-1] = p;
}

static char *make_block_name(struct gendisk *disk)
{
	char *name;
	static char *block_str = "block:";
	int size;
	char *s;

	size = strlen(block_str) + strlen(disk->disk_name) + 1;
	name = kmalloc(size, GFP_KERNEL);
	if (!name)
		return NULL;
	strcpy(name, block_str);
	strcat(name, disk->disk_name);
	/* ewww... some of these buggers have / in name... */
	s = strchr(name, '/');
	if (s)
		*s = '!';
	return name;
}

static void disk_sysfs_symlinks(struct gendisk *disk)
{
	struct device *target = get_device(disk->driverfs_dev);
	if (target) {
		char *disk_name = make_block_name(disk);
		sysfs_create_link(&disk->kobj,&target->kobj,"device");
		if (disk_name) {
			sysfs_create_link(&target->kobj,&disk->kobj,disk_name);
			kfree(disk_name);
		}
	}
	sysfs_create_link(&disk->kobj, &block_subsys.kset.kobj, "subsystem");
}

/* Not exported, helper to add_disk(). */
/**ltl
 * 功能：将磁盘添加到sys中，并扫描磁盘分区。
 * 参数：disk:通用磁盘描述符
 */
void register_disk(struct gendisk *disk)
{
	struct block_device *bdev;
	char *s;
	int i;
	struct hd_struct *p;
	int err;

	/* 用磁盘名字设备sys中的kobj的名字 */
	strlcpy(disk->kobj.name,disk->disk_name,KOBJ_NAME_LEN);
	/* ewww... some of these buggers have / in name... */
	s = strchr(disk->kobj.name, '/');
	if (s)
		*s = '!';
	/* 把disk添加到sys系统中 */
	if ((err = kobject_add(&disk->kobj)))
		return;
	/*在/sys/block/sda创建软链接文件device，这个文件是链接到文件
		devices/pci0000:00/0000:00:10.0/host0/target0:0:0/0:0:0:0
	*/
	disk_sysfs_symlinks(disk);
	/* 在/sys/block/sda创建holders和slaves两个目录 */
 	disk_sysfs_add_subdirs(disk);

	/* No minors to use for partitions */
	/* 如果只存在子分区，则退出 */
	if (disk->minors == 1)
		goto exit;

	/* No such device (e.g., media were just removed) */
	/* 如果设备的容量等于0，则退出 */
	if (!get_capacity(disk))
		goto exit;

	/* 通过通用磁盘描述符获取块设备描述符 */
	bdev = bdget_disk(disk, 0);
	if (!bdev)
		goto exit;

	/* scan partition table, but suppress uevents */
	/* bd_invalidated:1表示在blkdev_get时可以去扫描子分区，0:表示不能扫描子分区 */
	bdev->bd_invalidated = 1;
	/* part_uevent_suppress:1表示添加分区时可以向用户空间发送添加设备消息 */
	disk->part_uevent_suppress = 1;
	/* 扫描子分区 */
	err = blkdev_get(bdev, FMODE_READ, 0);
	/* 扫描完分区后，重置此标志，使内核可以向用户空间发送消息 */
	disk->part_uevent_suppress = 0;
	if (err < 0)
		goto exit;
	
	blkdev_put(bdev);/*TODO:这个函数的作用？*/

exit:
	/* announce disk after possible partitions are already created */
	/*分区已经扫描完成，向用户空间发送添加设备消息*/
	kobject_uevent(&disk->kobj, KOBJ_ADD);

	/* announce possible partitions */
	/*分区已经扫描完成，向用户空间发送添加设备消息，有几个分区，就发送几个*/
	for (i = 1; i < disk->minors; i++) {
		p = disk->part[i-1];
		if (!p || !p->nr_sects)
			continue;
		kobject_uevent(&p->kobj, KOBJ_ADD);
	}
}
/**ltl
功能:扫描主设备bdev下的子分区
参数: disk:通用磁盘描述符
	bdev:块设备描述符
返回值:0:扫描分区成功
	<0:扫描失败
*/
int rescan_partitions(struct gendisk *disk, struct block_device *bdev)
{
	struct parsed_partitions *state;
	int p, res;

	//如果bdev所代表的块设备描述符已经打开过，则退出分区扫描
	if (bdev->bd_part_count)
		return -EBUSY;
	/*设置分区无效*/
	res = invalidate_partition(disk, 0);
	if (res)
		return res;
	//重置bd_invalidated,因为在调用rescan_partitions之前把其置1
	bdev->bd_invalidated = 0;
	
	/*使各个分区的信息置0*/
	for (p = 1; p < disk->minors; p++)
		delete_partition(disk, p);
	/*调用gendisk中的重新生效接口*/
	if (disk->fops->revalidate_disk)
		disk->fops->revalidate_disk(disk);
	/*
	如果磁盘的容量为0，或者在扫描分区失败，直接退出。
	*/
	if (!get_capacity(disk) || !(state = check_partition(disk, bdev)))
		return 0;
	/*经过check_partition函数后，所以的分区信息都存在state中。现在只要根据这里面的信息添加到系统中，并保存到gendisk结构里*/
	for (p = 1; p < state->limit; p++) {
		sector_t size = state->parts[p].size;//子分区的大小
		sector_t from = state->parts[p].from;//子分区的起始位置
		if (!size)
			continue;
		if (from + size > get_capacity(disk)) {//如果子分区的结束扇区大于磁盘容量^^^^
			printk(" %s: p%d exceeds device capacity\n",
				disk->disk_name, p);
		}
		/*把分区添加到系统中，主要完成两个功能:1.添加到gendisk；2.添加到sys系统中*/
		add_partition(disk, p, from, size);
#ifdef CONFIG_BLK_DEV_MD
		if (state->parts[p].flags)
			md_autodetect_dev(bdev->bd_dev+p);
#endif
	}
	kfree(state);
	return 0;
}

unsigned char *read_dev_sector(struct block_device *bdev, sector_t n, Sector *p)
{
	struct address_space *mapping = bdev->bd_inode->i_mapping;
	struct page *page;

	page = read_mapping_page(mapping, (pgoff_t)(n >> (PAGE_CACHE_SHIFT-9)),
				 NULL);
	if (!IS_ERR(page)) {
		wait_on_page_locked(page);
		if (!PageUptodate(page))
			goto fail;
		if (PageError(page))
			goto fail;
		p->v = page;
		return (unsigned char *)page_address(page) +  ((n & ((1 << (PAGE_CACHE_SHIFT - 9)) - 1)) << 9);
fail:
		page_cache_release(page);
	}
	p->v = NULL;
	return NULL;
}

EXPORT_SYMBOL(read_dev_sector);

void del_gendisk(struct gendisk *disk)
{
	int p;

	/* invalidate stuff */
	for (p = disk->minors - 1; p > 0; p--) {
		invalidate_partition(disk, p);
		delete_partition(disk, p);
	}
	invalidate_partition(disk, 0);
	disk->capacity = 0;
	disk->flags &= ~GENHD_FL_UP;
	unlink_gendisk(disk);
	disk_stat_set_all(disk, 0);
	disk->stamp = 0;

	kobject_uevent(&disk->kobj, KOBJ_REMOVE);
	if (disk->holder_dir)
		kobject_unregister(disk->holder_dir);
	if (disk->slave_dir)
		kobject_unregister(disk->slave_dir);
	if (disk->driverfs_dev) {
		char *disk_name = make_block_name(disk);
		sysfs_remove_link(&disk->kobj, "device");
		if (disk_name) {
			sysfs_remove_link(&disk->driverfs_dev->kobj, disk_name);
			kfree(disk_name);
		}
		put_device(disk->driverfs_dev);
		disk->driverfs_dev = NULL;
	}
	sysfs_remove_link(&disk->kobj, "subsystem");
	kobject_del(&disk->kobj);
}
