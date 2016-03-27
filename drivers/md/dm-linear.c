/*
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dm.h"

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>

#define DM_MSG_PREFIX "linear"

/*
 * Linear: maps a linear range of a device.
 */
struct linear_c {
	struct dm_dev *dev; /* 低层设备对象 */
	sector_t start; /* 此目标设备相所属的物理设备的起始位置(相当于分区的起始地址) */
};

/*
 * Construct a linear mapping: <dev_path> <offset>
 */
/**ltl
 * 功能: linear目标类型的构造函数
 * 参数: ti	->
 *		argc->参数数组大小
 *		argv->[0]:目标设备"主设备号:次设备号"
 *			[1]:此目标设备的偏移量
 * 返回值:
 * 说明:
 */
static int linear_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct linear_c *lc;
	unsigned long long tmp;

	if (argc != 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}
	/* 分配linear内存空间 */
	lc = kmalloc(sizeof(*lc), GFP_KERNEL);
	if (lc == NULL) {
		ti->error = "dm-linear: Cannot allocate linear context";
		return -ENOMEM;
	}
	/* 此目标设备的偏移量 */
	if (sscanf(argv[1], "%llu", &tmp) != 1) {
		ti->error = "dm-linear: Invalid device sector";
		goto bad;
	}
	/* 此目标设备相所属的物理设备的起始位置(相当于分区的起始地址) */
	lc->start = tmp;
	/* 获取目标设备 */
	if (dm_get_device(ti, argv[0], lc->start, ti->len,
			  dm_table_get_mode(ti->table), &lc->dev)) {
		ti->error = "dm-linear: Device lookup failed";
		goto bad;
	}
	/* lc->start: 此dm使用到目标设备的首地址，ti->len:此dm在使用到此目标设备的长度 */
	ti->private = lc;
	return 0;

      bad:
	kfree(lc);
	return -EINVAL;
}

static void linear_dtr(struct dm_target *ti)
{
	struct linear_c *lc = (struct linear_c *) ti->private;

	dm_put_device(ti, lc->dev);
	kfree(lc);
}
/**ltl
 * 功能: 将bio请求映射到目标设备
 * 参数: ti	-> 目标设备对象
 *		bio	-> bio请求
 *		map_context->
 * 返回值:
 * 说明:
 */
static int linear_map(struct dm_target *ti, struct bio *bio,
		      union map_info *map_context)
{
	/* 映射目标类型对象 */
	struct linear_c *lc = (struct linear_c *) ti->private;
	/* 目标设备的块设备对象 */
	bio->bi_bdev = lc->dev->bdev;
	/*
	 * bio->bi_sector->ti->begin: 表示此请求相对于此目标设备的偏移量
	 */
	bio->bi_sector = lc->start + (bio->bi_sector/*相对于dm设备的起始扇区 */ - ti->begin); /* 相对于块设备的地址 */

	return 1;
}

static int linear_status(struct dm_target *ti, status_type_t type,
			 char *result, unsigned int maxlen)
{
	struct linear_c *lc = (struct linear_c *) ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		snprintf(result, maxlen, "%s %llu", lc->dev->name,
				(unsigned long long)lc->start);
		break;
	}
	return 0;
}

static struct target_type linear_target = {
	.name   = "linear",
	.version= {1, 0, 1},
	.module = THIS_MODULE,
	.ctr    = linear_ctr,	/* 目标设备类型的构造函数 */
	.dtr    = linear_dtr,   /*  */
	.map    = linear_map,	/* 映射函数 */
	.status = linear_status,
};

int __init dm_linear_init(void)
{
	int r = dm_register_target(&linear_target);

	if (r < 0)
		DMERR("register failed %d", r);

	return r;
}

void dm_linear_exit(void)
{
	int r = dm_unregister_target(&linear_target);

	if (r < 0)
		DMERR("unregister failed %d", r);
}
