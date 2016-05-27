/*
 *  pci_irq.c - ACPI PCI Interrupt Routing ($Revision: 11 $)
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *  Copyright (C) 2002       Dominik Brodowski <devel@brodo.de>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>

#define _COMPONENT		ACPI_PCI_COMPONENT
ACPI_MODULE_NAME("pci_irq");

static struct acpi_prt_list acpi_prt;
static DEFINE_SPINLOCK(acpi_prt_lock);

/* --------------------------------------------------------------------------
                         PCI IRQ Routing Table (PRT) Support
   -------------------------------------------------------------------------- */

static struct acpi_prt_entry *acpi_pci_irq_find_prt_entry(int segment,
							  int bus,
							  int device, int pin)
{
	struct list_head *node = NULL;
	struct acpi_prt_entry *entry = NULL;


	if (!acpi_prt.count)
		return NULL;

	/*
	 * Parse through all PRT entries looking for a match on the specified
	 * PCI device's segment, bus, device, and pin (don't care about func).
	 *
	 */
	spin_lock(&acpi_prt_lock);
	list_for_each(node, &acpi_prt.entries) {
		entry = list_entry(node, struct acpi_prt_entry, node);
		if ((segment == entry->id.segment)
		    && (bus == entry->id.bus)
		    && (device == entry->id.device)
		    && (pin == entry->pin)) {
			spin_unlock(&acpi_prt_lock);
			return entry;
		}
	}

	spin_unlock(&acpi_prt_lock);
	return NULL;
}
/**ltl
 * 功能:将中断路由表插入到全局列表acpi_prt中
 * 参数:
 * 返回值:
 * 说明:
 */
static int
acpi_pci_irq_add_entry(acpi_handle handle,
		       int segment, int bus, struct acpi_pci_routing_table *prt)
{
	struct acpi_prt_entry *entry = NULL;


	if (!prt)
		return -EINVAL;
	/* 分配中断路由表的空间 */
	entry = kmalloc(sizeof(struct acpi_prt_entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	memset(entry, 0, sizeof(struct acpi_prt_entry));
	/* 域 */
	entry->id.segment = segment;
	entry->id.bus = bus; /* 总线号 */
	entry->id.device = (prt->address >> 16) & 0xFFFF; /* 设备号 */
	entry->id.function = prt->address & 0xFFFF;			/* 功能号 */
	entry->pin = prt->pin; 								/* 中断引脚 */

	/*
	 * Type 1: Dynamic
	 * ---------------
	 * The 'source' field specifies the PCI interrupt link device used to
	 * configure the IRQ assigned to this slot|dev|pin.  The 'source_index'
	 * indicates which resource descriptor in the resource template (of
	 * the link device) this interrupt is allocated from.
	 * 
	 * NOTE: Don't query the Link Device for IRQ information at this time
	 *       because Link Device enumeration may not have occurred yet
	 *       (e.g. exists somewhere 'below' this _PRT entry in the ACPI
	 *       namespace).
	 */
	if (prt->source[0]) {
		acpi_get_handle(handle, prt->source, &entry->link.handle);
		entry->link.index = prt->source_index;
	}
	/*
	 * Type 2: Static
	 * --------------
	 * The 'source' field is NULL, and the 'source_index' field specifies
	 * the IRQ value, which is hardwired to specific interrupt inputs on
	 * the interrupt controller.
	 */
	else
		entry->link.index = prt->source_index;

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_INFO,
			      "      %02X:%02X:%02X[%c] -> %s[%d]\n",
			      entry->id.segment, entry->id.bus,
			      entry->id.device, ('A' + entry->pin), prt->source,
			      entry->link.index));
	/* 将对象插入到全局列表中  */
	spin_lock(&acpi_prt_lock);
	list_add_tail(&entry->node, &acpi_prt.entries);
	acpi_prt.count++;
	spin_unlock(&acpi_prt_lock);

	return 0;
}

static void
acpi_pci_irq_del_entry(int segment, int bus, struct acpi_prt_entry *entry)
{
	if (segment == entry->id.segment && bus == entry->id.bus) {
		acpi_prt.count--;
		list_del(&entry->node);
		kfree(entry);
	}
}
/**ltl
 * 功能: 获取PCI设备的中断路由表，并将此路由表添加到全局列表acpi_prt中
 * 参数:
 * 返回值:
 * 说明:
 */
int acpi_pci_irq_add_prt(acpi_handle handle, int segment, int bus)
{
	acpi_status status = AE_OK;
	char *pathname = NULL;
	struct acpi_buffer buffer = { 0, NULL };
	struct acpi_pci_routing_table *prt = NULL;
	struct acpi_pci_routing_table *entry = NULL;
	static int first_time = 1;


	pathname = (char *)kmalloc(ACPI_PATHNAME_MAX, GFP_KERNEL);
	if (!pathname)
		return -ENOMEM;
	memset(pathname, 0, ACPI_PATHNAME_MAX);

	if (first_time) {	/* 第一次调用时，先初始化全局列表头 */
		acpi_prt.count = 0;
		INIT_LIST_HEAD(&acpi_prt.entries);
		first_time = 0;
	}

	/* 
	 * NOTE: We're given a 'handle' to the _PRT object's parent device
	 *       (either a PCI root bridge or PCI-PCI bridge).
	 */

	buffer.length = ACPI_PATHNAME_MAX;
	buffer.pointer = pathname;
	acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer); /* 获取_PRT的路径 */
	/*
      *ACPI: PCI Interrupt Routing Table [\_SB_.PCI0._PRT]
	 *ACPI: PCI Interrupt Routing Table [\_SB_.PCI0.P0P3._PRT]
	 *ACPI: PCI Interrupt Routing Table [\_SB_.PCI0.PEX1._PRT]
	 *ACPI: PCI Interrupt Routing Table [\_SB_.PCI0.PEX8._PRT]
	 *ACPI: PCI Interrupt Routing Table [\_SB_.PCI0.PCIB._PRT]
	 */
	printk(KERN_DEBUG "ACPI: PCI Interrupt Routing Table [%s._PRT]\n",
	       pathname);

	/* 
	 * Evaluate this _PRT and add its entries to our global list (acpi_prt).
	 */

	buffer.length = 0;
	buffer.pointer = NULL;
	kfree(pathname);
	/* 获取中断路由表的长度 */
	status = acpi_get_irq_routing_table(handle, &buffer);
	if (status != AE_BUFFER_OVERFLOW) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating _PRT [%s]",
				acpi_format_exception(status)));
		return -ENODEV;
	}
	/* 分配中断路由表对象 */
	prt = kmalloc(buffer.length, GFP_KERNEL);
	if (!prt) {
		return -ENOMEM;
	}
	memset(prt, 0, buffer.length);
	buffer.pointer = prt;
	/* 获取中断路由表 */
	status = acpi_get_irq_routing_table(handle, &buffer);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating _PRT [%s]",
				acpi_format_exception(status)));
		kfree(buffer.pointer);
		return -ENODEV;
	}

	entry = prt;
	/* 将中断路由表插入到全局列表中 */
	while (entry && (entry->length > 0)) { /* entry->length=24 */
		acpi_pci_irq_add_entry(handle, segment, bus, entry);
		entry = (struct acpi_pci_routing_table *)
		    ((unsigned long)entry + entry->length);
	}

	kfree(prt);

	return 0;
}

void acpi_pci_irq_del_prt(int segment, int bus)
{
	struct list_head *node = NULL, *n = NULL;
	struct acpi_prt_entry *entry = NULL;

	if (!acpi_prt.count) {
		return;
	}

	printk(KERN_DEBUG
	       "ACPI: Delete PCI Interrupt Routing Table for %x:%x\n", segment,
	       bus);
	spin_lock(&acpi_prt_lock);
	list_for_each_safe(node, n, &acpi_prt.entries) {
		entry = list_entry(node, struct acpi_prt_entry, node);

		acpi_pci_irq_del_entry(segment, bus, entry);
	}
	spin_unlock(&acpi_prt_lock);
}

/* --------------------------------------------------------------------------
                          PCI Interrupt Routing Support
   -------------------------------------------------------------------------- */
typedef int (*irq_lookup_func) (struct acpi_prt_entry *, int *, int *, char **);
/**ltl
 * 功能: 从entry中获取中断号 
 * 参数:
 * 返回值:
 * 说明:
 */
static int
acpi_pci_allocate_irq(struct acpi_prt_entry *entry,
		      int *triggering, int *polarity, char **link)
{
	int irq;

	/* 中断链接句柄 */
	if (entry->link.handle) {
		irq = acpi_pci_link_allocate_irq(entry->link.handle,
						 entry->link.index, triggering,
						 polarity, link);
		if (irq < 0) {
			printk(KERN_WARNING PREFIX
				      "Invalid IRQ link routing entry\n");
			return -1;
		}
	} else {
		irq = entry->link.index;
		*triggering = ACPI_LEVEL_SENSITIVE;
		*polarity = ACPI_ACTIVE_LOW;
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Found IRQ %d\n", irq));
	return irq;
}

static int
acpi_pci_free_irq(struct acpi_prt_entry *entry,
		  int *triggering, int *polarity, char **link)
{
	int irq;

	if (entry->link.handle) {
		irq = acpi_pci_link_free_irq(entry->link.handle);
	} else {
		irq = entry->link.index;
	}
	return irq;
}

/*
 * acpi_pci_irq_lookup
 * success: return IRQ >= 0
 * failure: return -1
 */
/**ltl
 * 功能:根据<域:总线号:设备号:pin>获取中断号
 * 参数:
 * 返回值:
 * 说明:
 */
static int
acpi_pci_irq_lookup(struct pci_bus *bus,
		    int device,
		    int pin,
		    int *triggering,
		    int *polarity, char **link, irq_lookup_func func)
{
	struct acpi_prt_entry *entry = NULL;
	int segment = pci_domain_nr(bus);
	int bus_nr = bus->number;
	int ret;


	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "Searching for PRT entry for %02x:%02x:%02x[%c]\n",
			  segment, bus_nr, device, ('A' + pin)));
	/* 根据<域:总线号:设备号>获取entry */
	entry = acpi_pci_irq_find_prt_entry(segment, bus_nr, device, pin);
	if (!entry) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO, "PRT entry not found\n"));
		return -1;
	}
	
	ret = func(entry, triggering, polarity, link);
	return ret;
}

/*
 * acpi_pci_irq_derive
 * success: return IRQ >= 0
 * failure: return < 0
 */
static int
acpi_pci_irq_derive(struct pci_dev *dev,
		    int pin,
		    int *triggering,
		    int *polarity, char **link, irq_lookup_func func)
{
	struct pci_dev *bridge = dev;
	int irq = -1;
	u8 bridge_pin = 0;


	if (!dev)
		return -EINVAL;

	/* 
	 * Attempt to derive an IRQ for this device from a parent bridge's
	 * PCI interrupt routing entry (eg. yenta bridge and add-in card bridge).
	 */
	while (irq < 0 && bridge->bus->self) {
		pin = (pin + PCI_SLOT(bridge->devfn)) % 4;
		bridge = bridge->bus->self;

		if ((bridge->class >> 8) == PCI_CLASS_BRIDGE_CARDBUS) {
			/* PC card has the same IRQ as its cardbridge */
			bridge_pin = bridge->pin;
			if (!bridge_pin) {
				ACPI_DEBUG_PRINT((ACPI_DB_INFO,
						  "No interrupt pin configured for device %s\n",
						  pci_name(bridge)));
				return -1;
			}
			/* Pin is from 0 to 3 */
			bridge_pin--;
			pin = bridge_pin;
		}

		irq = acpi_pci_irq_lookup(bridge->bus, PCI_SLOT(bridge->devfn),
					  pin, triggering, polarity,
					  link, func);
	}

	if (irq < 0) {
		printk(KERN_WARNING PREFIX "Unable to derive IRQ for device %s\n",
			      pci_name(dev));
		return -1;
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Derive IRQ %d for device %s from %s\n",
			  irq, pci_name(dev), pci_name(bridge)));

	return irq;
}

/*
 * acpi_pci_irq_enable
 * success: return 0
 * failure: return < 0
 */
/**ltl
 * 功能:使能pci设备中断
 * 参数:
 * 返回值:
 * 说明: 在acpi功能开启的情况下，使能PCI设备中断的函数
 */
int acpi_pci_irq_enable(struct pci_dev *dev)
{
	int irq = 0;
	u8 pin = 0;
	int triggering = ACPI_LEVEL_SENSITIVE;
	int polarity = ACPI_ACTIVE_LOW;
	char *link = NULL;
	int rc;


	if (!dev)
		return -EINVAL;
	/* 从pci配置interrupt pin里读出的引脚号 */
	pin = dev->pin;
	if (!pin) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,
				  "No interrupt pin configured for device %s\n",
				  pci_name(dev)));
		return 0;
	}
	pin--; /* 为什么? */

	if (!dev->bus) {
		printk(KERN_ERR PREFIX "Invalid (NULL) 'bus' field\n");
		return -ENODEV;
	}

	/* 
	 * First we check the PCI IRQ routing table (PRT) for an IRQ.  PRT
	 * values override any BIOS-assigned IRQs set during boot.
	 */
	/* 分配中断号(此号为GSI(global system interrupt)) */
	irq = acpi_pci_irq_lookup(dev->bus, PCI_SLOT(dev->devfn), pin,
				  &triggering, &polarity, &link,
				  acpi_pci_allocate_irq);

	/*
	 * If no PRT entry was found, we'll try to derive an IRQ from the
	 * device's parent bridge.
	 */
	if (irq < 0)
		irq = acpi_pci_irq_derive(dev, pin, &triggering,
					  &polarity, &link,
					  acpi_pci_allocate_irq);

	/*
	 * No IRQ known to the ACPI subsystem - maybe the BIOS / 
	 * driver reported one, then use it. Exit in any case.
	 */
	if (irq < 0) {
		printk(KERN_WARNING PREFIX "PCI Interrupt %s[%c]: no GSI",
		       pci_name(dev), ('A' + pin));
		/* Interrupt Line values above 0xF are forbidden */
		if (dev->irq > 0 && (dev->irq <= 0xF)) {
			printk(" - using IRQ %d\n", dev->irq);
			acpi_register_gsi(dev->irq, ACPI_LEVEL_SENSITIVE,
					  ACPI_ACTIVE_LOW);
			return 0;
		} else {
			printk("\n");
			return 0;
		}
	}
	/* 将GSI号转换成系统软件使用的irq号 */
	rc = acpi_register_gsi(irq, triggering, polarity);
	if (rc < 0) {
		printk(KERN_WARNING PREFIX "PCI Interrupt %s[%c]: failed "
		       "to register GSI\n", pci_name(dev), ('A' + pin));
		return rc;
	}
	/* 设备的中断号 */
	dev->irq = rc;

	printk(KERN_INFO PREFIX "PCI Interrupt %s[%c] -> ",
	       pci_name(dev), 'A' + pin);

	if (link)
		printk("Link [%s] -> ", link);

	printk("GSI %u (%s, %s) -> IRQ %d\n", irq,
	       (triggering == ACPI_LEVEL_SENSITIVE) ? "level" : "edge",
	       (polarity == ACPI_ACTIVE_LOW) ? "low" : "high", dev->irq);

	return 0;
}

EXPORT_SYMBOL(acpi_pci_irq_enable);

/* FIXME: implement x86/x86_64 version */
void __attribute__ ((weak)) acpi_unregister_gsi(u32 i)
{
}

void acpi_pci_irq_disable(struct pci_dev *dev)
{
	int gsi = 0;
	u8 pin = 0;
	int triggering = ACPI_LEVEL_SENSITIVE;
	int polarity = ACPI_ACTIVE_LOW;


	if (!dev || !dev->bus)
		return;

	pin = dev->pin;
	if (!pin)
		return;
	pin--;

	/*
	 * First we check the PCI IRQ routing table (PRT) for an IRQ.
	 */
	gsi = acpi_pci_irq_lookup(dev->bus, PCI_SLOT(dev->devfn), pin,
				  &triggering, &polarity, NULL,
				  acpi_pci_free_irq);
	/*
	 * If no PRT entry was found, we'll try to derive an IRQ from the
	 * device's parent bridge.
	 */
	if (gsi < 0)
		gsi = acpi_pci_irq_derive(dev, pin,
					  &triggering, &polarity, NULL,
					  acpi_pci_free_irq);
	if (gsi < 0)
		return;

	/*
	 * TBD: It might be worth clearing dev->irq by magic constant
	 * (e.g. PCI_UNDEFINED_IRQ).
	 */

	printk(KERN_INFO PREFIX "PCI interrupt for device %s disabled\n",
	       pci_name(dev));

	acpi_unregister_gsi(gsi);

	return;
}
