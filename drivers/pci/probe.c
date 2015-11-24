/*
 * probe.c - PCI detection and setup code
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/cpumask.h>
#include "pci.h"

#define CARDBUS_LATENCY_TIMER	176	/* secondary latency timer */
#define CARDBUS_RESERVE_BUSNR	3
#define PCI_CFG_SPACE_SIZE	256
#define PCI_CFG_SPACE_EXP_SIZE	4096

/* Ugh.  Need to stop exporting this to modules. */
LIST_HEAD(pci_root_buses);
EXPORT_SYMBOL(pci_root_buses);

LIST_HEAD(pci_devices);

#ifdef HAVE_PCI_LEGACY
/**
 * pci_create_legacy_files - create legacy I/O port and memory files
 * @b: bus to create files under
 *
 * Some platforms allow access to legacy I/O port and ISA memory space on
 * a per-bus basis.  This routine creates the files and ties them into
 * their associated read, write and mmap files from pci-sysfs.c
 */
static void pci_create_legacy_files(struct pci_bus *b)
{
	b->legacy_io = kzalloc(sizeof(struct bin_attribute) * 2,
			       GFP_ATOMIC);
	if (b->legacy_io) {
		b->legacy_io->attr.name = "legacy_io";
		b->legacy_io->size = 0xffff;
		b->legacy_io->attr.mode = S_IRUSR | S_IWUSR;
		b->legacy_io->attr.owner = THIS_MODULE;
		b->legacy_io->read = pci_read_legacy_io;
		b->legacy_io->write = pci_write_legacy_io;
		class_device_create_bin_file(&b->class_dev, b->legacy_io);

		/* Allocated above after the legacy_io struct */
		b->legacy_mem = b->legacy_io + 1;
		b->legacy_mem->attr.name = "legacy_mem";
		b->legacy_mem->size = 1024*1024;
		b->legacy_mem->attr.mode = S_IRUSR | S_IWUSR;
		b->legacy_mem->attr.owner = THIS_MODULE;
		b->legacy_mem->mmap = pci_mmap_legacy_mem;
		class_device_create_bin_file(&b->class_dev, b->legacy_mem);
	}
}

void pci_remove_legacy_files(struct pci_bus *b)
{
	if (b->legacy_io) {
		class_device_remove_bin_file(&b->class_dev, b->legacy_io);
		class_device_remove_bin_file(&b->class_dev, b->legacy_mem);
		kfree(b->legacy_io); /* both are allocated here */
	}
}
#else /* !HAVE_PCI_LEGACY */
static inline void pci_create_legacy_files(struct pci_bus *bus) { return; }
void pci_remove_legacy_files(struct pci_bus *bus) { return; }
#endif /* HAVE_PCI_LEGACY */

/*
 * PCI Bus Class Devices
 */
static ssize_t pci_bus_show_cpuaffinity(struct class_device *class_dev,
					char *buf)
{
	int ret;
	cpumask_t cpumask;

	cpumask = pcibus_to_cpumask(to_pci_bus(class_dev));
	ret = cpumask_scnprintf(buf, PAGE_SIZE, cpumask);
	if (ret < PAGE_SIZE)
		buf[ret++] = '\n';
	return ret;
}
CLASS_DEVICE_ATTR(cpuaffinity, S_IRUGO, pci_bus_show_cpuaffinity, NULL);

/*
 * PCI Bus Class
 */
static void release_pcibus_dev(struct class_device *class_dev)
{
	struct pci_bus *pci_bus = to_pci_bus(class_dev);

	if (pci_bus->bridge)
		put_device(pci_bus->bridge);
	kfree(pci_bus);
}

static struct class pcibus_class = {
	.name		= "pci_bus",
	.release	= &release_pcibus_dev,
};

static int __init pcibus_class_init(void)
{
	return class_register(&pcibus_class);
}
postcore_initcall(pcibus_class_init);

/*
 * Translate the low bits of the PCI base
 * to the resource type
 */
static inline unsigned int pci_calc_resource_flags(unsigned int flags)
{
	if (flags & PCI_BASE_ADDRESS_SPACE_IO)
		return IORESOURCE_IO;

	if (flags & PCI_BASE_ADDRESS_MEM_PREFETCH)
		return IORESOURCE_MEM | IORESOURCE_PREFETCH;

	return IORESOURCE_MEM;
}

/*
 * Find the extent of a PCI decode..
 */
static u32 pci_size(u32 base, u32 maxbase, u32 mask)
{
	u32 size = mask & maxbase;	/* Find the significant bits */
	if (!size)
		return 0;

	/* Get the lowest of them to find the decode size, and
	   from that the extent.  */
	size = (size & ~(size-1)) - 1;

	/* base == maxbase can be valid only if the BAR has
	   already been programmed with all 1s.  */
	if (base == maxbase && ((base | size) & mask) != mask)
		return 0;

	return size;
}

static inline enum pci_bar_type decode_bar(struct resource *res, u32 bar)
{
	if ((bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
		res->flags = bar & ~PCI_BASE_ADDRESS_IO_MASK;
		return pci_bar_io;
	}

	res->flags = bar & ~PCI_BASE_ADDRESS_MEM_MASK;

	if (res->flags & PCI_BASE_ADDRESS_MEM_TYPE_64)
		return pci_bar_mem64;
	return pci_bar_mem32;
}

/** add by ltl
 * pci_read_base - read a PCI BAR
 * @dev: the PCI device
 * @type: type of the BAR
 * @res: resource buffer to be filled in
 * @pos: BAR position in the config space
 *
 * Returns 1 if the BAR is 64-bit, or 0 if 32-bit.
 */
int __pci_read_base(struct pci_dev *dev, enum pci_bar_type type,
			struct resource *res, unsigned int pos)
{
	u32 l, sz, mask;

	mask = type ? ~PCI_ROM_ADDRESS_ENABLE : ~0;

	res->name = pci_name(dev);

	pci_read_config_dword(dev, pos, &l);
	pci_write_config_dword(dev, pos, mask);
	pci_read_config_dword(dev, pos, &sz);
	pci_write_config_dword(dev, pos, l);

	/*
	 * All bits set in sz means the device isn't working properly.
	 * If the BAR isn't implemented, all bits must be 0.  If it's a
	 * memory BAR or a ROM, bit 0 must be clear; if it's an io BAR, bit
	 * 1 must be clear.
	 */
	if (!sz || sz == 0xffffffff)
		goto fail;

	/*
	 * I don't know how l can have all bits set.  Copied from old code.
	 * Maybe it fixes a bug on some ancient platform.
	 */
	if (l == 0xffffffff)
		l = 0;

	if (type == pci_bar_unknown) {
		type = decode_bar(res, l);
		res->flags |= pci_calc_resource_flags(l);
		if (type == pci_bar_io) {
			l &= PCI_BASE_ADDRESS_IO_MASK;
			mask = PCI_BASE_ADDRESS_IO_MASK & 0xffff;
		} else {
			l &= PCI_BASE_ADDRESS_MEM_MASK;
			mask = (u32)PCI_BASE_ADDRESS_MEM_MASK;
		}
	} else {
		res->flags |= (l & IORESOURCE_ROM_ENABLE);
		l &= PCI_ROM_ADDRESS_MASK;
		mask = (u32)PCI_ROM_ADDRESS_MASK;
	}

	if (type == pci_bar_mem64) {
		u64 l64 = l;
		u64 sz64 = sz;
		u64 mask64 = mask | (u64)~0 << 32;

		pci_read_config_dword(dev, pos + 4, &l);
		pci_write_config_dword(dev, pos + 4, ~0);
		pci_read_config_dword(dev, pos + 4, &sz);
		pci_write_config_dword(dev, pos + 4, l);

		l64 |= ((u64)l << 32);
		sz64 |= ((u64)sz << 32);

		sz64 = pci_size(l64, sz64, mask64);

		if (!sz64)
			goto fail;

		if ((sizeof(resource_size_t) < 8) && (sz64 > 0x100000000ULL)) {
			dev_err(&dev->dev, "can't handle 64-bit BAR\n");
			goto fail;
		} else if ((sizeof(resource_size_t) < 8) && l) {
			/* Address above 32-bit boundary; disable the BAR */
			pci_write_config_dword(dev, pos, 0);
			pci_write_config_dword(dev, pos + 4, 0);
			res->start = 0;
			res->end = sz64;
		} else {
			res->start = l64;
			res->end = l64 + sz64;
		}
	} else {
		sz = pci_size(l, sz, mask);

		if (!sz)
			goto fail;

		res->start = l;
		res->end = l + sz;
	}

 out:
	return (type == pci_bar_mem64) ? 1 : 0;
 fail:
	res->flags = 0;
	goto out;
}

/**ltl
 * 功能: 读取设备的BAR空间
 * 参数: dev		-> 设备对象
 *		howmany	-> BAR空间总数2或6
 *		rom		-> PCI配置的ROM空间地址(0x38或0x30)
 * 返回值:
 * 说明:
 */
static void pci_read_bases(struct pci_dev *dev, unsigned int howmany, int rom)
{
	unsigned int pos, reg, next;
	u32 l, sz;
	struct resource *res;
	/* 1.读bar */
	for(pos=0; pos<howmany; pos = next) {
		next = pos+1;
		res = &dev->resource[pos];
		res->name = pci_name(dev);/* 资源名 */
		reg = PCI_BASE_ADDRESS_0 + (pos << 2);
		/* 以下四句作用:读取bar空间的大小sz,这个大小必须是8x(2^n):8,16,32,64,128,256,512,1024,2048,4096,8196 */
		pci_read_config_dword(dev, reg, &l);	/* bar空间的值 */
		pci_write_config_dword(dev, reg, ~0);
		pci_read_config_dword(dev, reg, &sz);	/* 地址空间长度 */
		pci_write_config_dword(dev, reg, l);
		if (!sz || sz == 0xffffffff)
			continue;
		if (l == 0xffffffff)
			l = 0;
		/* 内存空间 */
		if ((l & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_MEMORY) {
			sz = pci_size(l, sz, (u32)PCI_BASE_ADDRESS_MEM_MASK);/* 修正空间大小，如果sz=128，则结果为127 */
			if (!sz)
				continue;
			res->start = l & PCI_BASE_ADDRESS_MEM_MASK;/* 设置起始地址 */
			res->flags |= l & ~PCI_BASE_ADDRESS_MEM_MASK;/* 类型 */
		} else {
			sz = pci_size(l, sz, PCI_BASE_ADDRESS_IO_MASK & 0xffff);
			if (!sz)
				continue;
			res->start = l & PCI_BASE_ADDRESS_IO_MASK;/* 设置起始地址 */
			res->flags |= l & ~PCI_BASE_ADDRESS_IO_MASK;/* 类型 */
		}
		/* 设置结束地址 */
		res->end = res->start + (unsigned long) sz;
		/* 设置资源的类型:IORESORCE_IO/IORESOURCE_MEM */
		res->flags |= pci_calc_resource_flags(l);
		/* 表示l为64位地址，此时对PCI设备，就只能变成3个bar空间 */
		if ((l & (PCI_BASE_ADDRESS_SPACE | PCI_BASE_ADDRESS_MEM_TYPE_MASK))
		    == (PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64)) {
			u32 szhi, lhi;
			/* 读取高位地址值 */
			pci_read_config_dword(dev, reg+4, &lhi);
			pci_write_config_dword(dev, reg+4, ~0);
			pci_read_config_dword(dev, reg+4, &szhi);/* 求大小 */
			pci_write_config_dword(dev, reg+4, lhi);
			szhi = pci_size(lhi, szhi, 0xffffffff);
			next++;
#if BITS_PER_LONG == 64/* 设置64位地址 */
			res->start |= ((unsigned long) lhi) << 32;
			res->end = res->start + sz;
			if (szhi) {
				/* This BAR needs > 4GB?  Wow. */
				res->end |= (unsigned long)szhi<<32;
			}
#else
			if (szhi) {
				printk(KERN_ERR "PCI: Unable to handle 64-bit BAR for device %s\n", pci_name(dev));
				res->start = 0;
				res->flags = 0;
			} else if (lhi) {
				/* 64-bit wide address, treat as disabled */
				pci_write_config_dword(dev, reg, l & ~(u32)PCI_BASE_ADDRESS_MEM_MASK);
				pci_write_config_dword(dev, reg+4, 0);
				res->start = 0;
				res->end = sz;
			}
#endif
		}
	}
	//读取rom空间的信息
	if (rom) {
		dev->rom_base_reg = rom;
		res = &dev->resource[PCI_ROM_RESOURCE];
		res->name = pci_name(dev);
		pci_read_config_dword(dev, rom, &l);
		pci_write_config_dword(dev, rom, ~PCI_ROM_ADDRESS_ENABLE);
		pci_read_config_dword(dev, rom, &sz);
		pci_write_config_dword(dev, rom, l);
		if (l == 0xffffffff)
			l = 0;
		if (sz && sz != 0xffffffff) {
			sz = pci_size(l, sz, (u32)PCI_ROM_ADDRESS_MASK);
			if (sz) {
				res->flags = (l & IORESOURCE_ROM_ENABLE) |
				  IORESOURCE_MEM | IORESOURCE_PREFETCH |
				  IORESOURCE_READONLY | IORESOURCE_CACHEABLE;
				res->start = l & PCI_ROM_ADDRESS_MASK;
				res->end = res->start + (unsigned long) sz;
			}
		}
	}
}
/**ltl
 * 功能: 读取PCI桥的资源窗口
 * 参数:
 * 返回值:
 * 说明:
 */
void __devinit pci_read_bridge_bases(struct pci_bus *child)
{
	struct pci_dev *dev = child->self;/* child总线所属的桥设备对象(child为子总线，而非根总线) */
	u8 io_base_lo, io_limit_lo;
	u16 mem_base_lo, mem_limit_lo;
	unsigned long base, limit;
	struct resource *res;
	int i;
	/*dev为NULL，说明child为根总线对象，不存在桥设备(host主控制器不算)*/
	if (!dev)		/* It's a host bus, nothing to read */
		return;
	/*如果是透明桥*/
	if (dev->transparent) {
		printk(KERN_INFO "PCI: Transparent bridge - %s\n", pci_name(dev));
		for(i = 3; i < PCI_BUS_NUM_RESOURCES; i++)
			child->resource[i] = child->parent->resource[i - 3];
	}
	/*子总线资源[0~2]与所属桥设备资源的[7~9]相同。[0]:io windows,[1]:mem windows,[2]:prefetch mem windows*/
	for(i=0; i<3; i++)
		child->resource[i] = &dev->resource[PCI_BRIDGE_RESOURCES+i];

	res = child->resource[0];
	pci_read_config_byte(dev, PCI_IO_BASE, &io_base_lo);/*io base*/
	pci_read_config_byte(dev, PCI_IO_LIMIT, &io_limit_lo);/*io limit */
	/* Q:为什么要空出8位  */
	base = (io_base_lo & PCI_IO_RANGE_MASK) << 8;
	limit = (io_limit_lo & PCI_IO_RANGE_MASK) << 8;

	/*如果io窗口支持32位，则读取高16位*/
	if ((io_base_lo & PCI_IO_RANGE_TYPE_MASK) == PCI_IO_RANGE_TYPE_32) {
		u16 io_base_hi, io_limit_hi;
		pci_read_config_word(dev, PCI_IO_BASE_UPPER16, &io_base_hi);/*io base upper 16*/
		pci_read_config_word(dev, PCI_IO_LIMIT_UPPER16, &io_limit_hi);/*io limit upper 16*/
		base |= (io_base_hi << 16);
		limit |= (io_limit_hi << 16);
	}

	if (base <= limit) {
		res->flags = (io_base_lo & PCI_IO_RANGE_TYPE_MASK) | IORESOURCE_IO;
		if (!res->start)
			res->start = base;
		if (!res->end)
			res->end = limit + 0xfff;/*4k*/
	}

	res = child->resource[1];
	pci_read_config_word(dev, PCI_MEMORY_BASE, &mem_base_lo);
	pci_read_config_word(dev, PCI_MEMORY_LIMIT, &mem_limit_lo);
	/* Q:为什么要空16位 */
	base = (mem_base_lo & PCI_MEMORY_RANGE_MASK) << 16;
	limit = (mem_limit_lo & PCI_MEMORY_RANGE_MASK) << 16;
	if (base <= limit) {
		res->flags = (mem_base_lo & PCI_MEMORY_RANGE_TYPE_MASK) | IORESOURCE_MEM;
		res->start = base;
		res->end = limit + 0xfffff;/*1M*/
	}

	res = child->resource[2];
	pci_read_config_word(dev, PCI_PREF_MEMORY_BASE, &mem_base_lo);
	pci_read_config_word(dev, PCI_PREF_MEMORY_LIMIT, &mem_limit_lo);
	base = (mem_base_lo & PCI_PREF_RANGE_MASK) << 16;
	limit = (mem_limit_lo & PCI_PREF_RANGE_MASK) << 16;

	if ((mem_base_lo & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) {
		u32 mem_base_hi, mem_limit_hi;
		pci_read_config_dword(dev, PCI_PREF_BASE_UPPER32, &mem_base_hi);
		pci_read_config_dword(dev, PCI_PREF_LIMIT_UPPER32, &mem_limit_hi);

		/*
		 * Some bridges set the base > limit by default, and some
		 * (broken) BIOSes do not initialize them.  If we find
		 * this, just assume they are not being used.
		 */
		if (mem_base_hi <= mem_limit_hi) {
#if BITS_PER_LONG == 64
			base |= ((long) mem_base_hi) << 32;
			limit |= ((long) mem_limit_hi) << 32;
#else
			if (mem_base_hi || mem_limit_hi) {
				printk(KERN_ERR "PCI: Unable to handle 64-bit address space for bridge %s\n", pci_name(dev));
				return;
			}
#endif
		}
	}
	if (base <= limit) {
		res->flags = (mem_base_lo & PCI_MEMORY_RANGE_TYPE_MASK) | IORESOURCE_MEM | IORESOURCE_PREFETCH;
		res->start = base;
		res->end = limit + 0xfffff;/*为什么要多留出1M*/
	}
}
/**ltl
功能:分配空间并初始化三个成员列表
参数:无
返回值:pci_bus对象
*/
static struct pci_bus * __devinit pci_alloc_bus(void)
{
	struct pci_bus *b;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (b) {
		INIT_LIST_HEAD(&b->node);
		INIT_LIST_HEAD(&b->children);
		INIT_LIST_HEAD(&b->devices);
	}
	return b;
}
/**ltl
 * 功能:分配一个以busnr为总线编号的总线对象。
 * 参数:parent	->父总线对象
 *	bridge->桥设备对象
 *	busnr	->总线编号
 * 返回值:
 *	以busnr为总线编号的总线对象。 
 */
static struct pci_bus * __devinit
pci_alloc_child_bus(struct pci_bus *parent, struct pci_dev *bridge, int busnr)
{
	struct pci_bus *child;
	int i;

	/*
	 * Allocate a new bus, and inherit stuff from the parent..
	 */
	/* 分配总线对象 */
	child = pci_alloc_bus();
	if (!child)
		return NULL;
	child->parent = parent;/* 总线的父总线对象 */
	child->ops = parent->ops;/* 总线的操作对象 */
	child->sysdata = parent->sysdata;/* 私有数据 */
	child->bus_flags = parent->bus_flags;/* 总线的属性标志 */

	child->class_dev.class = &pcibus_class;/* 总线类 */
	sprintf(child->class_dev.class_id, "%04x:%02x", pci_domain_nr(child), busnr);/* 总线类名 */
	class_device_register(&child->class_dev);/* 注册总线类设备 */
	class_device_create_file(&child->class_dev, &class_device_attr_cpuaffinity);/* 创建总线类设备下的属性文件。*/

	/*
	 * Set up the primary, secondary and subordinate
	 * bus numbers.
	 */
	child->number = child->secondary = busnr;/* 设置总线的编号，次总线编号 */
	child->primary = parent->secondary;/* 设置总线的主总线编号。*/
	child->subordinate = 0xff;/* 设置此棵总线树的中最大总线编号。*/

	if (!bridge)
		return child;

	child->self = bridge; /* 总线所属的桥设备对象 */
	child->bridge = get_device(&bridge->dev); /* 设备对象 */

	/* Set up default resource pointers and names.. */
	for (i = 0; i < 4; i++) {/* 设置总线的资源，与所属桥设备中的resource[7,8,9,10]一样 */
		child->resource[i] = &bridge->resource[PCI_BRIDGE_RESOURCES+i];
		child->resource[i]->name = child->name;/* 资源名字 */
	}
	bridge->subordinate = child;/*桥设备的子总线对象 */

	return child;
}
/**ltl
 * 功能:分配一总线编号为busnr的总线对象。并添加到其父总线中的子总线链表中。
 * 参数:parent	->父总线对象
 *	dev	->新分配的总线所属的pci桥对象
 *	busnr	->总线编号。
 * 返回值:
 */
struct pci_bus * __devinit pci_add_new_bus(struct pci_bus *parent, struct pci_dev *dev, int busnr)
{
	struct pci_bus *child;
	/* 分配一以busnr为总线编号的总线对象 */
	child = pci_alloc_child_bus(parent, dev, busnr);
	if (child) {
		down_write(&pci_bus_sem);
		/* 把子总线对象添加到父总线的子总线链表中。 */
		list_add_tail(&child->node, &parent->children);
		up_write(&pci_bus_sem);
	}
	return child;
}
/**ltl
 *功能:<这个与PCIe中的CRS功能相关，后面再去详述>
 *参数:
 */
static void pci_enable_crs(struct pci_dev *dev)
{
	u16 cap, rpctl;
	int rpcap = pci_find_capability(dev, PCI_CAP_ID_EXP);
	if (!rpcap)
		return;

	pci_read_config_word(dev, rpcap + PCI_CAP_FLAGS, &cap);
	if (((cap & PCI_EXP_FLAGS_TYPE) >> 4) != PCI_EXP_TYPE_ROOT_PORT)
		return;

	pci_read_config_word(dev, rpcap + PCI_EXP_RTCTL, &rpctl);
	rpctl |= PCI_EXP_RTCTL_CRSSVE;
	pci_write_config_word(dev, rpcap + PCI_EXP_RTCTL, rpctl);
}

static void __devinit pci_fixup_parent_subordinate_busnr(struct pci_bus *child, int max)
{
	struct pci_bus *parent = child->parent;

	/* Attempts to fix that up are really dangerous unless
	   we're going to re-assign all bus numbers. */
	if (!pcibios_assign_all_busses())
		return;
	//更新child父总线的subordinate值，同时要更新到所属的PCI桥设备中。
	while (parent->parent && parent->subordinate < max) {
		parent->subordinate = max;
		pci_write_config_byte(parent->self, PCI_SUBORDINATE_BUS, max);
		parent = parent->parent;
	}
}

unsigned int __devinit pci_scan_child_bus(struct pci_bus *bus);

/*
 * If it's a bridge, configure it and scan the bus behind it.
 * For CardBus bridges, we don't scan behind as the devices will
 * be handled by the bridge driver itself.
 *
 * We need to process bridges in two passes -- first we scan those
 * already configured by the BIOS and after we are done with all of
 * them, we proceed to assigning numbers to the remaining buses in
 * order to avoid overlaps between old and new bus numbers.
 */
/**ltl
 * 功能:扫描桥设备下的总线，其中bus是其父总线。
 * 参数:bus	->父总线
 *	dev	->桥设备对象
 *	max	->已经扫描过的最大总线编号。
 *	pass	->0:表示当前函数为检查bios已经分配的总线编号是否合法。
 *		  1:表示分配一新的总线编号。
 * 返回值:当前子树下最大的总线编号。
 */
int __devinit pci_scan_bridge(struct pci_bus *bus, struct pci_dev * dev, int max, int pass)
{
	struct pci_bus *child;
	int is_cardbus = (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS);
	u32 buses, i, j = 0;
	u16 bctl;
	/* 读取主总线编号。*/
	pci_read_config_dword(dev, PCI_PRIMARY_BUS, &buses);

	pr_debug("PCI: Scanning behind PCI bridge %s, config %06x, pass %d\n",
		 pci_name(dev), buses & 0xffffff, pass);

	/* Disable MasterAbortMode during probing to avoid reporting
	   of bus errors (in some architectures) */ 
	/* 读取桥设备的控制寄存器 */
	pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &bctl);
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL,
			      bctl & ~PCI_BRIDGE_CTL_MASTER_ABORT);/* 禁止主控制器抛出异常。*/
	/* 启用PCIe的CRS特性。*/
	pci_enable_crs(dev);
	
	/* 如果在bios已经为PCI桥分配了主总线编号 */
	if ((buses & 0xffff00) && !pcibios_assign_all_busses() && !is_cardbus) {
		unsigned int cmax, busnr;
		/*
		 * Bus already configured by firmware, process it in the first
		 * pass and just note the configuration.
		 */
		/*
		 *只有第一次扫描才能做以下工作:1.申请一个与次总线编号的总线对象，并添加到系统中。
		 *					 2.扫描次总线下的设备。
		 */
		if (pass)
			goto out;
		/* 获取bios已经为这个PCI桥设备分配好的次总线编号。*/
		busnr = (buses >> 8) & 0xFF;

		/*
		 * If we already got to this bus through a different bridge,
		 * ignore it.  This can happen with the i450NX chipset.
		 */
		 /* 如果总线已经存在，则退出 */
		if (pci_find_bus(pci_domain_nr(bus), busnr)) {
			printk(KERN_INFO "PCI: Bus %04x:%02x already known\n",
					pci_domain_nr(bus), busnr);
			goto out;
		}
		/* 分配一个以busnr(PCI桥的次总线编号)为总线编号的总线对象 */
		child = pci_add_new_bus(bus, dev, busnr);
		if (!child)
			goto out;
		/*虽然在pci_add_net_bus中已经设备的总线编号，但这里要重新设置。*/
		child->primary = buses & 0xFF;/*设置主总线编号*/
		child->subordinate = (buses >> 16) & 0xFF;/*设置总线总编号。*/
		child->bridge_ctl = bctl;/*此总线所性PCI桥设备的桥控制寄存器(0x3e)的值*/

		/*扫描child总线下的PCI设备(PCI桥和PCI设备)*/
		cmax = pci_scan_child_bus(child);
		if (cmax > max)/*如果返回的总线编号大于现有的总线编号，说明在child总线下，又扫描到了总线。因此要去更新编号值。*/
			max = cmax;
		if (child->subordinate > max)
			max = child->subordinate;
	} 
	else 
	{/*如果bios没有为pci桥设置总线编号。*/
	 /*注:以下流程的总线编号都是max+1(或者max++)的原因是:max其父总线的次总线编号。*/
		/*
		 * We need to assign a number to this bus which we always
		 * do in the second pass.
		 */
		if (!pass) {/*第一次扫描，则把pci桥的总线编号置为无效。*/
			if (pcibios_assign_all_busses())
				/* Temporarily disable forwarding of the
				   configuration cycles on all bridges in
				   this bus segment to avoid possible
				   conflicts in the second pass between two
				   bridges programmed with overlapping
				   bus ranges. */
				pci_write_config_dword(dev, PCI_PRIMARY_BUS,
						       buses & ~0xffffff);
			goto out;
		}
		/*清空PCI桥的状态寄存器。*/
		/* Clear errors */
		pci_write_config_word(dev, PCI_STATUS, 0xffff);

		/* Prevent assigning a bus number that already exists.
		 * This can happen when a bridge is hot-plugged */
		/*总线已经存在，退出*/
		if (pci_find_bus(pci_domain_nr(bus), max+1))
			goto out;
		/*分配总线编号为++max的总线对象。*/
		child = pci_add_new_bus(bus, dev, ++max);
		buses = (buses & 0xff000000)
		      | ((unsigned int)(child->primary)     <<  0)
		      | ((unsigned int)(child->secondary)   <<  8)
		      | ((unsigned int)(child->subordinate) << 16);

		/*
		 * yenta.c forces a secondary latency timer of 176.
		 * Copy that behaviour here.
		 */
		if (is_cardbus) {
			buses &= ~0xff000000;
			buses |= CARDBUS_LATENCY_TIMER << 24;
		}
			
		/*
		 * We need to blast all three values with a single write.
		 */
		/*把分配好的总线编号写放所属的桥设备的寄存器中。*/
		pci_write_config_dword(dev, PCI_PRIMARY_BUS, buses);
		/*dev为PCI桥*/
		if (!is_cardbus) {
			/*禁用ISA端口*/
			child->bridge_ctl = bctl | PCI_BRIDGE_CTL_NO_ISA;
			/*
			 * Adjust subordinate busnr in parent buses.
			 * We do this before scanning for children because
			 * some devices may not be detected if the bios
			 * was lazy.
			 */
			/*先更新child所有父总线对象的subordinate域，因为在总线扫描时，要总线编号大于这个数，就不能往其子树扫描。*/
			pci_fixup_parent_subordinate_busnr(child, max);
			/* Now we can scan all subordinate buses... */
			/*扫描child下的PCI桥和PCI设备，以及总线。*/
			max = pci_scan_child_bus(child);
			/*
			 * now fix it up again since we have found
			 * the real value of max.
			 */
			/*重新更新child的父总线对象的subordinate域。*/
			pci_fixup_parent_subordinate_busnr(child, max);
		} else {
			/*
			 * For CardBus bridges, we leave 4 bus numbers
			 * as cards with a PCI-to-PCI bridge can be
			 * inserted later.
			 */
			 /*<这个是CardBus流程，这里不考虑>*/
			for (i=0; i<CARDBUS_RESERVE_BUSNR; i++) {
				struct pci_bus *parent = bus;
				if (pci_find_bus(pci_domain_nr(bus),
							max+i+1))
					break;
				while (parent->parent) {
					if ((!pcibios_assign_all_busses()) &&
					    (parent->subordinate > max) &&
					    (parent->subordinate <= max+i)) {
						j = 1;
					}
					parent = parent->parent;
				}
				if (j) {
					/*
					 * Often, there are two cardbus bridges
					 * -- try to leave one valid bus number
					 * for each one.
					 */
					i /= 2;
					break;
				}
			}
			max += i;
			pci_fixup_parent_subordinate_busnr(child, max);
		}
		/*
		 * Set the subordinate bus number to its real value.
		 */
		child->subordinate = max;/*更新新创建的总线对象的subordinate域值*/
		pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max);/*把subordinate值更新到所属桥设备的寄存器中。*/
	}

	sprintf(child->name, (is_cardbus ? "PCI CardBus #%02x" : "PCI Bus #%02x"), child->number);

	while (bus->parent) {
		if ((child->subordinate > bus->subordinate) ||
		    (child->number > bus->subordinate) ||
		    (child->number < bus->number) ||
		    (child->subordinate < bus->number)) {
			printk(KERN_WARNING "PCI: Bus #%02x (-#%02x) is "
			       "hidden behind%s bridge #%02x (-#%02x)%s\n",
			       child->number, child->subordinate,
			       bus->self->transparent ? " transparent" : " ",
			       bus->number, bus->subordinate,
			       pcibios_assign_all_busses() ? " " :
			       " (try 'pci=assign-busses')");
			printk(KERN_WARNING "Please report the result to "
			       "linux-kernel to fix this permanently\n");
		}
		bus = bus->parent;
	}

out:
	/*更新桥设备的控制寄存器*/
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, bctl);

	return max;
}

/*
 * Read interrupt line and base address registers.
 * The architecture-dependent code can tweak these, of course.
 */
static void pci_read_irq(struct pci_dev *dev)
{
	unsigned char irq;

	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &irq);
	dev->pin = irq;
	if (irq)
		pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &irq);
	dev->irq = irq;
}

/**ltl
 * 功能:设置PCIe端口类型
 * 参数:
 * 返回值:
 * 说明:
 */
static void set_pcie_port_type(struct pci_dev *pdev)
{
	int pos;
	u16 reg16;

	pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	if (!pos)
		return;
	pdev->is_pcie = 1; /* 1表示此设备为PCIe设备 */
	pci_read_config_word(pdev, pos + PCI_EXP_FLAGS, &reg16);
	/* pcie设备(EP)、端口(Root point、switch upstream point、switch downstream point) */
	pdev->pcie_type = (reg16 & PCI_EXP_FLAGS_TYPE) >> 4; 
}

/**
 * pci_setup_device - fill in class and map information of a device
 * @dev: the device structure to fill
 *
 * Initialize the device structure with information about the device's 
 * vendor,class,memory and IO-space addresses,IRQ lines etc.
 * Called at initialisation of the PCI subsystem and by CardBus services.
 * Returns 0 on success and -1 if unknown type of device (not normal, bridge
 * or CardBus).
 */
/**ltl
 * 功能:读取标准的配置头
 * 参数:dev->pci设备对象
 * 返回值:
*/
int pci_setup_device(struct pci_dev * dev)
{
	u32 class;
	u8 hdr_type = 0;
	/* 读取header_type寄存器，判定单功能/多功能，设备类型:PCI device、PCI bridge、cardbus */
	if (pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type))
	{
		return -EIO;
	}
	/* PCI名字: dev->dev.bus_id */
	sprintf(pci_name(dev), "%04x:%02x:%02x.%d", pci_domain_nr(dev->bus),
		dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
	/* 读取class:revision */
	pci_read_config_dword(dev, PCI_CLASS_REVISION, &class);
	class >>= 8;				    /* upper 3 bytes */
	dev->class = class;/* classcode->baseclass:subclass:interface */
	class >>= 8;
	/* 设备私有数据 */
	dev->sysdata = dev->bus->sysdata;
	dev->dev.parent = dev->bus->bridge;
	dev->dev.bus = &pci_bus_type;
	/* 配置头的大小:256B或者4096B */
	dev->cfg_size = pci_cfg_space_size(dev);
	/* 0:PCI普通设备;1:PCI桥设备;2:CardBus设备 */
	dev->hdr_type = hdr_type & 0x7f;
	/* 1:表示为多功能设备,0:表示设备是单功能设备 */
	dev->multifunction = !!(hdr_type & 0x80); 
	dev->error_state = pci_channel_io_normal;
	
	pr_debug("PCI: Found %s [%04x/%04x] %06x %02x\n", pci_name(dev),
		 dev->vendor, dev->device, class, dev->hdr_type);

	/* "Unknown power state" */
	dev->current_state = PCI_UNKNOWN;
	/* 设置PCIe标志 */
	set_pcie_port_type(dev);
	/* Early fixups, before probing the BARs */
	pci_fixup_device(pci_fixup_early, dev);/* 这里主要禁用中断(MSI/MSIX,PIN) */
	class = dev->class >> 8;

	switch (dev->hdr_type) {		    /* header type */
	case PCI_HEADER_TYPE_NORMAL:		    /* standard header */
		if (class == PCI_CLASS_BRIDGE_PCI)
			goto bad;
		/* pci设备 */
		pci_read_irq(dev);/* 读取interrupt pin and interrupt line register */
		pci_read_bases(dev, 6, PCI_ROM_ADDRESS);/* 读取bar */
		pci_read_config_word(dev, PCI_SUBSYSTEM_VENDOR_ID, &dev->subsystem_vendor);/* read sub system vendor */
		pci_read_config_word(dev, PCI_SUBSYSTEM_ID, &dev->subsystem_device);/* read subsystem device */
		break;

	case PCI_HEADER_TYPE_BRIDGE:		    /* bridge header */
		if (class != PCI_CLASS_BRIDGE_PCI)
			goto bad;
		/* The PCI-to-PCI bridge spec requires that subtractive
		   decoding (i.e. transparent) bridge must have programming
		   interface code of 0x01. */ 
		pci_read_irq(dev);
		dev->transparent = ((dev->class & 0xff) == 1);/* 是否是透明桥 */
		pci_read_bases(dev, 2, PCI_ROM_ADDRESS1);
		break;

	case PCI_HEADER_TYPE_CARDBUS:		    /* CardBus bridge header */
		if (class != PCI_CLASS_BRIDGE_CARDBUS)
			goto bad;
		pci_read_irq(dev);
		pci_read_bases(dev, 1, 0);
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_VENDOR_ID, &dev->subsystem_vendor);
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_ID, &dev->subsystem_device);
		break;

	default:				    /* unknown header */
		printk(KERN_ERR "PCI: device %s has unknown header type %02x, ignoring.\n",
			pci_name(dev), dev->hdr_type);
		return -1;

	bad:
		printk(KERN_ERR "PCI: %s: class %x doesn't match header type %02x. Ignoring class.\n",
		       pci_name(dev), class, dev->hdr_type);
		dev->class = PCI_CLASS_NOT_DEFINED;
	}

	/* We found a fine healthy device, go go go... */
	return 0;
}

/**
 * pci_release_dev - free a pci device structure when all users of it are finished.
 * @dev: device that's been disconnected
 *
 * Will be called only by the device core when all users of this pci device are
 * done.
 */
static void pci_release_dev(struct device *dev)
{
	struct pci_dev *pci_dev;

	pci_dev = to_pci_dev(dev);
	pci_iov_release(pci_dev);
	kfree(pci_dev);
}

/**
 * pci_cfg_space_size - get the configuration space size of the PCI device.
 * @dev: PCI device
 *
 * Regular PCI devices have 256 bytes, but PCI-X 2 and PCI Express devices
 * have 4096 bytes.  Even if the device is capable, that doesn't mean we can
 * access it.  Maybe we don't have a way to generate extended config space
 * accesses, or the device is behind a reverse Express bridge.  So we try
 * reading the dword at 0x100 which must either be 0 or a valid extended
 * capability header.
 */
int pci_cfg_space_size(struct pci_dev *dev)
{
	int pos;
	u32 status;

	pos = pci_find_capability(dev, PCI_CAP_ID_EXP);
	if (!pos) {
		pos = pci_find_capability(dev, PCI_CAP_ID_PCIX);
		if (!pos)
			goto fail;

		pci_read_config_dword(dev, pos + PCI_X_STATUS, &status);
		if (!(status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ)))
			goto fail;
	}

	if (pci_read_config_dword(dev, 256, &status) != PCIBIOS_SUCCESSFUL)
		goto fail;
	if (status == 0xffffffff)
		goto fail;

	return PCI_CFG_SPACE_EXP_SIZE;

 fail:
	return PCI_CFG_SPACE_SIZE;
}

static void pci_release_bus_bridge_dev(struct device *dev)
{
	kfree(dev);
}

/*
 * Read the config data for a PCI device, sanity-check it
 * and fill in the dev structure...
 */
/**ltl
 * 功能:读取功能号为devfn的设备配置信息
 * 参数:bus	->总线对象
 *	devfn	->功能号
 * 返回值:NULL	->不存在devfn的功能号
 *	 !NULL->设备对象
 * 说明: 判定<总线号:设备号:功能号>存在的依据是能从PCI设备配置头从读取厂商ID
 */
static struct pci_dev * __devinit
pci_scan_device(struct pci_bus *bus, int devfn)
{
	struct pci_dev *dev;
	u32 l;
	u8 hdr_type;
	int delay = 1;
	/* 读取vendorID,若读取成功，说明功能号是存在的 */
	if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, &l))
		return NULL;
	/* 功能号无效 */
	/* some broken boards return 0 or ~0 if a slot is empty: */
	if (l == 0xffffffff || l == 0x00000000 ||
	    l == 0x0000ffff || l == 0xffff0000)
		return NULL;

	/* Configuration request Retry Status */
	while (l == 0xffff0001) {
		msleep(delay);
		delay *= 2;
		if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, &l))
			return NULL;
		/* Card hasn't responded in 60 seconds?  Must be stuck. */
		if (delay > 60 * 1000) {
			printk(KERN_WARNING "Device %04x:%02x:%02x.%d not "
					"responding\n", pci_domain_nr(bus),
					bus->number, PCI_SLOT(devfn),
					PCI_FUNC(devfn));
			return NULL;
		}
	}
#if 0	
	if (pci_bus_read_config_byte(bus, devfn, PCI_HEADER_TYPE, &hdr_type))
		return NULL;
#endif	
	/* 分配设备对象，并置空 */
	dev = kzalloc(sizeof(struct pci_dev), GFP_KERNEL);
	if (!dev)
		return NULL;
	/* PCI设备所属的PCI总线 */
	dev->bus = bus;
#if 0	
	dev->sysdata = bus->sysdata;
	dev->dev.parent = bus->bridge;
	dev->dev.bus = &pci_bus_type;
	dev->hdr_type = hdr_type & 0x7f;//0:PCI普通设备;1:PCI桥设备;2:CardBus设备
	dev->multifunction = !!(hdr_type & 0x80);//表示为多功能设备
	dev->cfg_size = pci_cfg_space_size(dev);//配置头的大小:256B或者4096B
	dev->error_state = pci_channel_io_normal;
#endif	
	dev->devfn = devfn;/* 设备号:功能号，此字段必须设置，否则以下将无法读取配置	*/
	dev->vendor = l & 0xffff;/* vendor ID */
	dev->device = (l >> 16) & 0xffff;/* device id */
	
	

	/* Assume 32-bit PCI; let 64-bit PCI cards (which are far rarer)
	   set this higher, assuming the system even supports it.  */
	dev->dma_mask = 0xffffffff;
	/* 读取标准头 */
	if (pci_setup_device(dev) < 0) {
		kfree(dev);
		return NULL;
	}

	return dev;
}
/**ltl
 * 功能: 添加PCI设置到总线的设备链表中。 如果此设置是IOV设备，则使能之
 * 参数: dev	->
 *		bus	->
 * 返回值:
 * 说明:
 */
void __devinit pci_device_add(struct pci_dev *dev, struct pci_bus *bus)
{
	/*初始化pci设备上device*/
	device_initialize(&dev->dev);
	dev->dev.release = pci_release_dev;
	pci_dev_get(dev);
	/*设备DMA mask*/
	dev->dev.dma_mask = &dev->dma_mask;
	dev->dev.coherent_dma_mask = 0xffffffffull;
	/*对一此特殊的设备，还要读取PCI配置的其它信息，并做处理。比如:多余的bar*/
	/* Fix up broken headers */
	pci_fixup_device(pci_fixup_header, dev);

	/* Alternative Routing-ID Forwarding */
	pci_enable_ari(dev);

	/* Single Root I/O Virtualization */
	pci_iov_init(dev);

	/* Enable ACS P2P upstream forwarding */
	pci_enable_acs(dev);

	/*
	 * Add the device to our list of discovered devices
	 * and the bus list for fixup functions, etc.
	 */
	INIT_LIST_HEAD(&dev->global_list);
	down_write(&pci_bus_sem);
	/*把扫描到的设备添加到总线里面(设备和桥)*/
	list_add_tail(&dev->bus_list, &bus->devices);
	up_write(&pci_bus_sem);
}
/**ltl
 * 功能:扫描功能号为devfn的设备
 * 参数:bus	->总线对象
 *	devfn	->功能号
 * 返回值:PCI设备对象
 */
struct pci_dev * __devinit
pci_scan_single_device(struct pci_bus *bus, int devfn)
{
	struct pci_dev *dev;
	/* 扫描总线号为bus->number、功能号为devfn的设备 */
	dev = pci_scan_device(bus, devfn);
	if (!dev)
		return NULL;
	/* 初始化pci_dev中与总线设备驱动模型中的变量 */
	pci_device_add(dev, bus);
	
	/* 配置空间否存在MSI/MSIX中断方式，如果是，则增加全局引用计数器:nr_msix_devices/nr_reserved_vectors。这两个在pci_enable_msix接口中用到。*/
	pci_scan_msi_device(dev);

	return dev;
}

/**
 * pci_scan_slot - scan a PCI slot on a bus for devices.
 * @bus: PCI bus to scan
 * @devfn: slot number to scan (must have zero function.)
 *
 * Scan a PCI slot on the specified PCI bus for devices, adding
 * discovered devices to the @bus->devices list.  New devices
 * will have an empty dev->global_list head.
 */
/**ltl
 * 功能:扫描bus总线下，设备号devfn下的8个功能
 * 参数:bus	->总线对象
 *	devfn	->设备号(槽编号)
 * 返回值:devfn下的功能数
 */
int __devinit pci_scan_slot(struct pci_bus *bus, int devfn)
{
	int func, nr = 0;
	int scan_all_fns;
	/* 在x86与i386平台都置成0 */
	scan_all_fns = pcibios_scan_all_fns(bus, devfn);
	/* 由于每个设备有8个功能，所有循环体要循环8次 */
	for (func = 0; func < 8; func++, devfn++) {
		struct pci_dev *dev;
		/* 扫描<总线号:设备号:功能号>的PCI设备 */
		dev = pci_scan_single_device(bus, devfn);
		if (dev) {
			nr++;

			/*
		 	 * If this is a single function device,
		 	 * don't scan past the first function.
		 	 */
		 	/* 如果从PCI配置头中解析的head_type是单一设备，而循环体又进行了多次，则标志此设备为多功能设备 */
			if (!dev->multifunction) {
				if (func > 0) {
					dev->multifunction = 1;
				} else {
 					break;
				}
			}
		} else {
			if (func == 0 && !scan_all_fns)
				break;
		}
	}
	return nr;
}
/**ltl
 * 功能:扫描bus下的所有子总线和设备(PCI设备和PCI桥)
 * 参数:bus->总线对象
 * 返回值:以bus总线为根的树的最大总线编号
 * 说明: 此函数完成三个功能: 1. 扫描总线bus下的所有PCI设备(包括PCI桥)
 *					    2. 配置桥设备的资源配置窗口
 *					    3. 扫描PCI桥下的PCI总线。
 */
unsigned int __devinit pci_scan_child_bus(struct pci_bus *bus)
{
	unsigned int devfn, pass, max = bus->secondary;/* pci bus的次总线编号。*/
	struct pci_dev *dev;

	pr_debug("PCI: Scanning bus %04x:%02x\n", pci_domain_nr(bus), bus->number);

	/* Go find them, Rover! */
	/* 1.一条PCI中线下，可以有32个设备(槽)，每个设备8个功能，所以要循环256(0x100)(32*8)次，因此递增为8 
	 * [02:00] 功能号，[07:03] 设备号
	 */
	for (devfn = 0; devfn < 0x100; devfn += 8)
		pci_scan_slot(bus, devfn);

	/*
	 * After performing arch-dependent fixup of the bus, look behind
	 * all PCI-to-PCI bridges on this bus.
	 */
	pr_debug("PCI: Fixups for bus %04x:%02x\n", pci_domain_nr(bus), bus->number);
	
	/*2.读取桥设备的资源配置窗口*/
	pcibios_fixup_bus(bus);

	/*3.扫描所有桥设备的子总线。循环两次原因:第一次是检测在bios已经为总线分配编号是否合法；第二次是分配新的总线 */
	for (pass=0; pass < 2; pass++)
		list_for_each_entry(dev, &bus->devices, bus_list) {
			if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE ||
			    dev->hdr_type == PCI_HEADER_TYPE_CARDBUS)
				max = pci_scan_bridge(bus, dev, max, pass);
		}

	/*
	 * We've scanned the bus and so we know all about what's on
	 * the other side of any bridges that may be on this bus plus
	 * any devices.
	 *
	 * Return how far we've got finding sub-buses.
	 */
	pr_debug("PCI: Bus scan for %04x:%02x returning with max=%02x\n",
		pci_domain_nr(bus), bus->number, max);
	return max;
}

unsigned int __devinit pci_do_scan_bus(struct pci_bus *bus)
{
	unsigned int max;

	max = pci_scan_child_bus(bus);

	/*
	 * Make the discovered devices available.
	 */
	pci_bus_add_devices(bus);

	return max;
}
/**ltl
 * 功能:创建一个pci_bus对象(针对根总线)
 * 参数:parent		->父设备
 *	bus		->总线编号
 *	ops		->访问配置的方法
 *	sysdata	->私有数据
 * 返回值:pci_bus对象
 * 注:对于根总线，主总线编号primary不设置(初始值为0)，次总线编号secondary为0
 */
struct pci_bus * __devinit pci_create_bus(struct device *parent,
		int bus, struct pci_ops *ops, void *sysdata)
{
	int error;
	struct pci_bus *b;
	struct device *dev;
	/* 申请pci_bus空间 */
	b = pci_alloc_bus();
	if (!b)
		return NULL;
	/* 分配device */
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev){
		kfree(b);
		return NULL;
	}

	b->sysdata = sysdata;
	b->ops = ops;  /* 访问PCI配置的操作函数 */
	/* 如果已经在链表中，则出错返回 */
	if (pci_find_bus(pci_domain_nr(b), bus)) {
		/* If we already got to this bus through a different bridge, ignore it */
		pr_debug("PCI: Bus %04x:%02x already known\n", pci_domain_nr(b), bus);
		goto err_out;
	}

	down_write(&pci_bus_sem);
	list_add_tail(&b->node, &pci_root_buses);/* 做为根总线，把它添加到全局链表pci_root_buses中。*/
	up_write(&pci_bus_sem);

	memset(dev, 0, sizeof(*dev));
	dev->parent = parent;
	dev->release = pci_release_bus_bridge_dev;
	sprintf(dev->bus_id, "pci%04x:%02x", pci_domain_nr(b), bus);
	error = device_register(dev);/* 初始化并添加设备 */
	if (error)
		goto dev_reg_err;
	b->bridge = get_device(dev);

	b->class_dev.class = &pcibus_class;
	sprintf(b->class_dev.class_id, "%04x:%02x", pci_domain_nr(b), bus);
	error = class_device_register(&b->class_dev);/* 添加设备类 */
	if (error)
		goto class_dev_reg_err;
	/* 创建设备类的属性文件 */
	error = class_device_create_file(&b->class_dev, &class_device_attr_cpuaffinity);
	if (error)
		goto class_dev_create_file_err;

	/* Create legacy_io and legacy_mem files for this bus */
	/* 创建两个文件:legacy_io和legacy_mem，并设置两个文件的读写操作接口 */
	pci_create_legacy_files(b);

	error = sysfs_create_link(&b->class_dev.kobj, &b->bridge->kobj, "bridge");
	if (error)
		goto sys_create_link_err;

	b->number = b->secondary = bus;/* 设置总线编号与次总线编号 */
	b->resource[0] = &ioport_resource;
	b->resource[1] = &iomem_resource;

	return b;

sys_create_link_err:
	class_device_remove_file(&b->class_dev, &class_device_attr_cpuaffinity);
class_dev_create_file_err:
	class_device_unregister(&b->class_dev);
class_dev_reg_err:
	device_unregister(dev);
dev_reg_err:
	down_write(&pci_bus_sem);
	list_del(&b->node);
	up_write(&pci_bus_sem);
err_out:
	kfree(dev);
	kfree(b);
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_create_bus);

/**ltl
 *功能:扫描以编号为bus的总线和其子总线
 *参数:parent	->总线驱动模型，pci_bus对象的父设备
 *	bus		->总线编号
 *	ops		->PCI配置访问方式
 *	sysdata	->系统数据(私有数据)
 *返回值:编号bus的pci_bus对象
 */
struct pci_bus * __devinit pci_scan_bus_parented(struct device *parent,
		int bus, struct pci_ops *ops, void *sysdata)
{
	struct pci_bus *b;
	/*创建一个pci_bus对象(根总线) */
	b = pci_create_bus(parent, bus, ops, sysdata);
	if (b)/* 扫描根总线及其下面的子总线 */
		b->subordinate = pci_scan_child_bus(b);
	return b;
}
EXPORT_SYMBOL(pci_scan_bus_parented);

#ifdef CONFIG_HOTPLUG
EXPORT_SYMBOL(pci_add_new_bus);
EXPORT_SYMBOL(pci_do_scan_bus);
EXPORT_SYMBOL(pci_scan_slot);
EXPORT_SYMBOL(pci_scan_bridge);
EXPORT_SYMBOL(pci_scan_single_device);
EXPORT_SYMBOL_GPL(pci_scan_child_bus);
#endif
