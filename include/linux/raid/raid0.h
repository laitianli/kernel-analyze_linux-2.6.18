#ifndef _RAID0_H
#define _RAID0_H

#include <linux/raid/md.h>
/* 条带区域 */
struct strip_zone
{	
	/* 第i条带区域的偏移量 */
	sector_t zone_offset;	/* Zone offset in md_dev */
	sector_t dev_offset;	/* Zone offset in real dev */
	/* 条带区域大小 */
	sector_t size;		/* Zone size */
	/* 条带区域包含设备数 */
	int nb_dev;		/* # of devices attached to the zone */
	mdk_rdev_t **dev;	/* Devices attached to the zone */
};

struct raid0_private_data
{
	struct strip_zone **hash_table; /* Table of indexes into strip_zone */
	/* 条带区域数组 */
	struct strip_zone *strip_zone;
	mdk_rdev_t **devlist; /* lists of rdevs, pointed to by strip_zone->dev */
	int nr_strip_zones;	/* raid0条带数 */

	sector_t hash_spacing;
	int preshift;			/* shift this before divide by hash_spacing */
};

typedef struct raid0_private_data raid0_conf_t;

#define mddev_to_conf(mddev) ((raid0_conf_t *) mddev->private)

#endif
