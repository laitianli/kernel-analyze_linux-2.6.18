/*
 * legacy.c - traditional, old school PCI bus probing
 */
#include <linux/init.h>
#include <linux/pci.h>
#include "pci.h"

/*
 * Discover remaining PCI buses in case there are peer host bridges.
 * We use the number of last PCI bus provided by the PCI BIOS.
 */
/**ltl
 * 功能: 扫描系统下的所有总线下的PCI设备
 * 参数:
 * 返回值:
 * 说明: 必须在cmdline中添加字段pci=lastbus=0xff
 */
static void __devinit pcibios_fixup_peer_bridges(void)
{
	int n, devfn;
	/* 系统存在总线号为0xff，因此要改成pcibios_last_bus > 0xff */
	if (pcibios_last_bus <= 0 || pcibios_last_bus >= 0xff)
		return;
	DBG("PCI: Peer bridge fixup\n");

	for (n=0; n <= pcibios_last_bus; n++) {
		u32 l;
		if (pci_find_bus(0, n))
			continue;
		for (devfn = 0; devfn < 256; devfn += 8) {
			if (!raw_pci_ops->read(0, n, devfn, PCI_VENDOR_ID, 2, &l) &&
			    l != 0x0000 && l != 0xffff) {
				DBG("Found device at %02x:%02x [%04x]\n", n, devfn, l);
				printk(KERN_INFO "PCI: Discovered peer bus %02x\n", n);
				pci_scan_bus(n, &pci_root_ops, NULL);
				break;
			}
		}
	}
}
/**ltl
 * 功能: PCI设备扫描的入口
 * 参数:
 * 返回值:
 * 说明: 当系统没有开启ACPI模块的情况下，使用此种方式扫描
 */
static int __init pci_legacy_init(void)
{
	if (!raw_pci_ops) {
		printk("PCI: System does not support PCI\n");
		return 0;
	}

	if (pcibios_scanned++)
		return 0;

	printk("PCI: Probing PCI hardware\n");
	pci_root_bus = pcibios_scan_root(0);
	if (pci_root_bus)
		pci_bus_add_devices(pci_root_bus);

	pcibios_fixup_peer_bridges();

	return 0;
}

subsys_initcall(pci_legacy_init);
