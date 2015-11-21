/*
 * MSI hooks for standard x86 apic
 */

#include <linux/pci.h>
#include <linux/irq.h>
#include <asm/smp.h>

#include "msi.h"

/*
 * Shifts for APIC-based data
 */

#define MSI_DATA_VECTOR_SHIFT		0
#define	    MSI_DATA_VECTOR(v)		(((u8)v) << MSI_DATA_VECTOR_SHIFT)

#define MSI_DATA_DELIVERY_SHIFT		8
#define     MSI_DATA_DELIVERY_FIXED	(0 << MSI_DATA_DELIVERY_SHIFT) /* ��ʾ�ж�����ֱ�ӱ�ָ����CPU���� */
#define     MSI_DATA_DELIVERY_LOWPRI	(1 << MSI_DATA_DELIVERY_SHIFT) /* ��ʾ���жϽ�������Ȩ��͵�CPU���� */

#define MSI_DATA_LEVEL_SHIFT		14
#define     MSI_DATA_LEVEL_DEASSERT	(0 << MSI_DATA_LEVEL_SHIFT) /* ����͵�ƽ������ʽ */
#define     MSI_DATA_LEVEL_ASSERT	(1 << MSI_DATA_LEVEL_SHIFT) /* �͵�ƽ������ʽ */

#define MSI_DATA_TRIGGER_SHIFT		15
#define     MSI_DATA_TRIGGER_EDGE	(0 << MSI_DATA_TRIGGER_SHIFT) /* �����Ե������ʽ */
#define     MSI_DATA_TRIGGER_LEVEL	(1 << MSI_DATA_TRIGGER_SHIFT) /* ��Ե������ʽ */

/*
 * Shift/mask fields for APIC-based bus address
 */

#define MSI_ADDR_HEADER			0xfee00000

#define MSI_ADDR_DESTID_MASK		0xfff0000f
#define     MSI_ADDR_DESTID_CPU(cpu)	((cpu) << MSI_TARGET_CPU_SHIFT)

#define MSI_ADDR_DESTMODE_SHIFT		2
#define     MSI_ADDR_DESTMODE_PHYS	(0 << MSI_ADDR_DESTMODE_SHIFT)
#define	    MSI_ADDR_DESTMODE_LOGIC	(1 << MSI_ADDR_DESTMODE_SHIFT)

#define MSI_ADDR_REDIRECTION_SHIFT	3
#define     MSI_ADDR_REDIRECTION_CPU	(0 << MSI_ADDR_REDIRECTION_SHIFT)
#define     MSI_ADDR_REDIRECTION_LOWPRI	(1 << MSI_ADDR_REDIRECTION_SHIFT)


static void
msi_target_apic(unsigned int vector,
		unsigned int dest_cpu,
		u32 *address_hi,	/* in/out */
		u32 *address_lo)	/* in/out */
{
	u32 addr = *address_lo;

	addr &= MSI_ADDR_DESTID_MASK;
	addr |= MSI_ADDR_DESTID_CPU(cpu_physical_id(dest_cpu));

	*address_lo = addr;
}

static int
msi_setup_apic(struct pci_dev *pdev,	/* unused in generic */
		unsigned int vector,
		u32 *address_hi,
		u32 *address_lo,
		u32 *data)
{
	unsigned long	dest_phys_id;

	dest_phys_id = cpu_physical_id(first_cpu(cpu_online_map));
	/* ��ַ�ֶ�->
	 * [31:20]=FSB Interrupt�洢���ռ�Ļ���ַ����ֵΪ0xFEE
	 * [19:12]=Destination ID(CPU ID)
	 * [11:04]=reserved 
	 * [3]=RH(Redirection Hint Indication) 0:��ʾinterrupt messageֱ�ӷ��͵�Destination ID CPU���д���1:��ʾ��ʹ���ж�ת�����ܡ�
	 * [2]=DM(Destination Mode) ��ʾ�ڴ�������Ȩ��͵��ж�����ʱ��Destination ID�ֶ��Ƿ񱻷���ΪLogical����Physical APIC ID.(APIC ID������ģʽ:Physical Logical ClusterID) 
	 * [1:0]=xx
	 */
	*address_hi = 0;
	*address_lo =	MSI_ADDR_HEADER |	
			MSI_ADDR_DESTMODE_PHYS |
			MSI_ADDR_REDIRECTION_CPU |
			MSI_ADDR_DESTID_CPU(dest_phys_id);
	/* �����ֶ�->
	 * [31:16]->reserved
	 * [14:15]->trigger Mode(�͵�ƽ������ʽ����Ե������ʽ)
	 * [13:11]->reserved
	 * [10:08]->delivery mode (��ʾ��δ�������PCIe�豸���ж����� Fixed��ʾ�ж�����ֱ�ӱ�ָ����CPU����)
	 * [07:00]->vector (�ж�����)
	 */
	*data = MSI_DATA_TRIGGER_EDGE | /* ��Ե������ʽ */
		MSI_DATA_LEVEL_ASSERT |     /* ��ʹ�õ͵�ƽ������ʽ */
		MSI_DATA_DELIVERY_FIXED |	/* FIXED��ʽֱ�Ӵ����ж� */
		MSI_DATA_VECTOR(vector);    /* �ж����� */

	return 0;
}

static void
msi_teardown_apic(unsigned int vector)
{
	return;		/* no-op */
}

/*
 * Generic ops used on most IA archs/platforms.  Set with msi_register()
 */

struct msi_ops msi_apic_ops = {
	.setup = msi_setup_apic,
	.teardown = msi_teardown_apic,
	.target = msi_target_apic,
};
