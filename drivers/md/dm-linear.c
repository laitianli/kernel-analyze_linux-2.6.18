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
	struct dm_dev *dev; /* �Ͳ��豸���� */
	sector_t start; /* ��Ŀ���豸�������������豸����ʼλ��(�൱�ڷ�������ʼ��ַ) */
};

/*
 * Construct a linear mapping: <dev_path> <offset>
 */
/**ltl
 * ����: linearĿ�����͵Ĺ��캯��
 * ����: ti	->
 *		argc->���������С
 *		argv->[0]:Ŀ���豸"���豸��:���豸��"
 *			[1]:��Ŀ���豸��ƫ����
 * ����ֵ:
 * ˵��:
 */
static int linear_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct linear_c *lc;
	unsigned long long tmp;

	if (argc != 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}
	/* ����linear�ڴ�ռ� */
	lc = kmalloc(sizeof(*lc), GFP_KERNEL);
	if (lc == NULL) {
		ti->error = "dm-linear: Cannot allocate linear context";
		return -ENOMEM;
	}
	/* ��Ŀ���豸��ƫ���� */
	if (sscanf(argv[1], "%llu", &tmp) != 1) {
		ti->error = "dm-linear: Invalid device sector";
		goto bad;
	}
	/* ��Ŀ���豸�������������豸����ʼλ��(�൱�ڷ�������ʼ��ַ) */
	lc->start = tmp;
	/* ��ȡĿ���豸 */
	if (dm_get_device(ti, argv[0], lc->start, ti->len,
			  dm_table_get_mode(ti->table), &lc->dev)) {
		ti->error = "dm-linear: Device lookup failed";
		goto bad;
	}
	/* lc->start: ��dmʹ�õ�Ŀ���豸���׵�ַ��ti->len:��dm��ʹ�õ���Ŀ���豸�ĳ��� */
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
 * ����: ��bio����ӳ�䵽Ŀ���豸
 * ����: ti	-> Ŀ���豸����
 *		bio	-> bio����
 *		map_context->
 * ����ֵ:
 * ˵��:
 */
static int linear_map(struct dm_target *ti, struct bio *bio,
		      union map_info *map_context)
{
	/* ӳ��Ŀ�����Ͷ��� */
	struct linear_c *lc = (struct linear_c *) ti->private;
	/* Ŀ���豸�Ŀ��豸���� */
	bio->bi_bdev = lc->dev->bdev;
	/*
	 * bio->bi_sector->ti->begin: ��ʾ����������ڴ�Ŀ���豸��ƫ����
	 */
	bio->bi_sector = lc->start + (bio->bi_sector/*�����dm�豸����ʼ���� */ - ti->begin); /* ����ڿ��豸�ĵ�ַ */

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
	.ctr    = linear_ctr,	/* Ŀ���豸���͵Ĺ��캯�� */
	.dtr    = linear_dtr,   /*  */
	.map    = linear_map,	/* ӳ�亯�� */
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
