/*
 * arch/i386/kernel/acpi/processor.c
 *
 * Copyright (C) 2005 Intel Corporation
 * 	Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 * 	- Added _PDC for platforms with Intel CPUs
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/acpi.h>

#include <acpi/processor.h>
#include <asm/acpi.h>

static void init_intel_pdc(struct acpi_processor *pr, struct cpuinfo_x86 *c)
{
	struct acpi_object_list *obj_list;
	union acpi_object *obj;
	u32 *buf;

	/* allocate and initialize pdc. It will be used later. */
	obj_list = kmalloc(sizeof(struct acpi_object_list), GFP_KERNEL);
	if (!obj_list) {
		printk(KERN_ERR "Memory allocation error\n");
		return;
	}

	obj = kmalloc(sizeof(union acpi_object), GFP_KERNEL);
	if (!obj) {
		printk(KERN_ERR "Memory allocation error\n");
		kfree(obj_list);
		return;
	}

	buf = kmalloc(12, GFP_KERNEL);
	if (!buf) {
		printk(KERN_ERR "Memory allocation error\n");
		kfree(obj);
		kfree(obj_list);
		return;
	}

	buf[0] = ACPI_PDC_REVISION_ID; /*reviosion id*/
	buf[1] = 1;						/* count */
	buf[2] = ACPI_PDC_C_CAPABILITY_SMP; /* the capabilities bit */

	if (cpu_has(c, X86_FEATURE_EST))
		buf[2] |= ACPI_PDC_EST_CAPABILITY_SWSMP;

	obj->type = ACPI_TYPE_BUFFER; /* 对象类型 */
	obj->buffer.length = 12;		/* 长度 */
	obj->buffer.pointer = (u8 *) buf; /* point to the buffer of the capabilities array */
	obj_list->count = 1;
	obj_list->pointer = obj;
	pr->pdc = obj_list;

	return;
}
/**ltl
 * 功能: 初始化_PDC
 * 参数: pr -> acpi processor object.
 * 返回值:
 * 说明: PDC-> processor driver capabilities,根据Intel CPU具有特性初始化此结构，结构组成:
 *		revisionId
 * 		count -> the number of capability values in the capability array.
 * 		capabilities[count]-> capability array. Each DWORD entry in the capabilities array is a bitfield 
 *							that difines capabilities and features supported by OSPM for processor 
 * 							configuration and power management as specified by the cpu manufacturer.
 */
/* Initialize _PDC data based on the CPU vendor */
void arch_acpi_processor_init_pdc(struct acpi_processor *pr)
{
	unsigned int cpu = pr->id;
	struct cpuinfo_x86 *c = cpu_data + cpu;

	pr->pdc = NULL;
	if (c->x86_vendor == X86_VENDOR_INTEL)
		init_intel_pdc(pr, c);

	return;
}

EXPORT_SYMBOL(arch_acpi_processor_init_pdc);
