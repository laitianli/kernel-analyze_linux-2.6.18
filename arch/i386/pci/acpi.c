#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <asm/numa.h>
#include "pci.h"
/**ltl
 *功能:扫描PCI主控制器下的PCI设备
 *参数:device		->
 *	domain	->域
 *	busnum	->总线号(0-255)
 */
struct pci_bus * __devinit pci_acpi_scan_root(struct acpi_device *device, int domain, int busnum)
{
	struct pci_bus *bus;

	if (domain != 0) {
		printk(KERN_WARNING "PCI: Multiple domains not supported\n");
		return NULL;
	}
	/* 扫描总线号为busnum下的子总线 */
	bus = pcibios_scan_root(busnum);
#ifdef CONFIG_ACPI_NUMA
	if (bus != NULL) {
		int pxm = acpi_get_pxm(device->handle);
		if (pxm >= 0) {
			bus->sysdata = (void *)(unsigned long)pxm_to_node(pxm);
			printk("bus %d -> pxm %d -> node %ld\n",
				busnum, pxm, (long)(bus->sysdata));
		}
	}
#endif
	
	return bus;
}

extern int pci_routeirq;
/**ltl
 * 功能:设备PCI设备中断的使能接口
 * 参数:
 * 返回值:
 * 说明:
 */
static int __init pci_acpi_init(void)
{
	struct pci_dev *dev = NULL;

	if (pcibios_scanned)
		return 0;

	if (acpi_noirq)
		return 0;

	printk(KERN_INFO "PCI: Using ACPI for IRQ routing\n");
	acpi_irq_penalty_init();
	pcibios_scanned++;
	/* 设置中断使能接口  */
	pcibios_enable_irq = acpi_pci_irq_enable;
	pcibios_disable_irq = acpi_pci_irq_disable;

	if (pci_routeirq) {
		/*
		 * PCI IRQ routing is set up by pci_enable_device(), but we
		 * also do it here in case there are still broken drivers that
		 * don't use pci_enable_device().
		 */
		printk(KERN_INFO "PCI: Routing PCI interrupts for all devices because \"pci=routeirq\" specified\n");
		for_each_pci_dev(dev)
			acpi_pci_irq_enable(dev);
	} else
		printk(KERN_INFO "PCI: If a device doesn't work, try \"pci=routeirq\".  If it helps, post a report\n");

#ifdef CONFIG_X86_IO_APIC
	if (acpi_ioapic)
		print_IO_APIC();
#endif

	return 0;
}
subsys_initcall(pci_acpi_init);
