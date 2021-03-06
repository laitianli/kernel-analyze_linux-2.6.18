/*
 *	Intel Multiprocessor Specification 1.1 and 1.4
 *	compliant MP-table parsing routines.
 *
 *	(c) 1995 Alan Cox, Building #3 <alan@redhat.com>
 *	(c) 1998, 1999, 2000 Ingo Molnar <mingo@redhat.com>
 *
 *	Fixes
 *		Erich Boleyn	:	MP v1.4 and additional changes.
 *		Alan Cox	:	Added EBDA scanning
 *		Ingo Molnar	:	various cleanups and rewrites
 *		Maciej W. Rozycki:	Bits for default MP configurations
 *		Paul Diefenbaugh:	Added full ACPI support
 */

#include <linux/mm.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/bootmem.h>
#include <linux/smp_lock.h>
#include <linux/kernel_stat.h>
#include <linux/mc146818rtc.h>
#include <linux/acpi.h>
#include <linux/module.h>

#include <asm/smp.h>
#include <asm/mtrr.h>
#include <asm/mpspec.h>
#include <asm/pgalloc.h>
#include <asm/io_apic.h>
#include <asm/proto.h>
#include <asm/acpi.h>

/* Have we found an MP table */
int smp_found_config;	/* 在特定的内存区域内是否找到SMP的config */
unsigned int __initdata maxcpus = NR_CPUS;

int acpi_found_madt;

/*
 * Various Linux-internal data structures created from the
 * MP-table.
 */
unsigned char apic_version [MAX_APICS];/**/
/* 记录系统<总线编号:总线类型> */
unsigned char mp_bus_id_to_type [MAX_MP_BUSSES] = { [0 ... MAX_MP_BUSSES-1] = -1 }; /* 值:mp_bustype */
/* 记录系统<总线编号:PCI总线编号> */
int mp_bus_id_to_pci_bus [MAX_MP_BUSSES] = { [0 ... MAX_MP_BUSSES-1] = -1 };
/* 系统总线数 */
static int mp_current_pci_id = 0;
/* I/O APIC entries */
struct mpc_config_ioapic mp_ioapics[MAX_IO_APICS]; /* ioapic表 */
/* 中断信息，包括(0~15)及ioapic */
/* # of MP IRQ source entries */
struct mpc_config_intsrc mp_irqs[MAX_IRQ_SOURCES];

/* MP IRQ source entries */
int mp_irq_entries; /* 系统中中断源个数 */

int nr_ioapics;	/* ioapic芯片总数 */
int pic_mode;
/* 每个cpu中对应的local apic的物理地址 */
unsigned long mp_lapic_addr = 0; /* local */



/* Processor that is doing the boot up */
unsigned int boot_cpu_id = -1U; /* 在SMP系统中，用于启动的CPU ID */
/* Internal processor count */
unsigned int num_processors __initdata = 0;

unsigned disabled_cpus __initdata;

/* Bitmask of physically existing CPUs */
physid_mask_t phys_cpu_present_map = PHYSID_MASK_NONE;

/* ACPI MADT entry parsing functions */
#ifdef CONFIG_ACPI
extern struct acpi_boot_flags acpi_boot;
#ifdef CONFIG_X86_LOCAL_APIC
extern int acpi_parse_lapic (acpi_table_entry_header *header);
extern int acpi_parse_lapic_addr_ovr (acpi_table_entry_header *header);
extern int acpi_parse_lapic_nmi (acpi_table_entry_header *header);
#endif /*CONFIG_X86_LOCAL_APIC*/
#ifdef CONFIG_X86_IO_APIC
extern int acpi_parse_ioapic (acpi_table_entry_header *header);
#endif /*CONFIG_X86_IO_APIC*/
#endif /*CONFIG_ACPI*/

u8 bios_cpu_apicid[NR_CPUS] = { [0 ... NR_CPUS-1] = BAD_APICID };


/*
 * Intel MP BIOS table parsing routines:
 */

/*
 * Checksum an MP configuration block.
 */

static int __init mpf_checksum(unsigned char *mp, int len)
{
	int sum = 0;

	while (len--)
		sum += *mp++;

	return sum & 0xFF;
}
/**ltl
 * 功能: 保存cpu config
 * 参数:
 * 返回值:
 * 说明:
 */
static void __cpuinit MP_processor_info (struct mpc_config_processor *m)
{
	int cpu;
	unsigned char ver;
	cpumask_t tmp_map;

	if (!(m->mpc_cpuflag & CPU_ENABLED)) { /* CPU无效 */
		disabled_cpus++;
		return;
	}
	/* 
	  Processor #0 7:14 APIC version 21
	  Processor #2 7:14 APIC version 21
	  Processor #4 7:14 APIC version 21
	  Processor #6 7:14 APIC version 21
	 */
	printk(KERN_INFO "Processor #%d %d:%d APIC version %d\n",
		m->mpc_apicid,
	       (m->mpc_cpufeature & CPU_FAMILY_MASK)>>8,
	       (m->mpc_cpufeature & CPU_MODEL_MASK)>>4,
		m->mpc_apicver);

	if (m->mpc_cpuflag & CPU_BOOTPROCESSOR) { /* 此CPU用于bst (bootstap process) */
		Dprintk("    Bootup CPU\n");
		boot_cpu_id = m->mpc_apicid; /* 保存用于启动的cpu ID */
	}
	if (num_processors >= NR_CPUS) {
		printk(KERN_WARNING "WARNING: NR_CPUS limit of %i reached."
			" Processor ignored.\n", NR_CPUS);
		return;
	}
	/* 递增CPU数 */
	num_processors++;
	cpus_complement(tmp_map, cpu_present_map);
	cpu = first_cpu(tmp_map);

#if MAX_APICS < 255	
	if ((int)m->mpc_apicid > MAX_APICS) {
		printk(KERN_ERR "Processor #%d INVALID. (Max ID: %d).\n",
			m->mpc_apicid, MAX_APICS);
		return;
	}
#endif
	ver = m->mpc_apicver;

	physid_set(m->mpc_apicid, phys_cpu_present_map);
	/*
	 * Validate version
	 */
	if (ver == 0x0) {
		printk(KERN_ERR "BIOS bug, APIC version is 0 for CPU#%d! fixing up to 0x10. (tell your hw vendor)\n", m->mpc_apicid);
		ver = 0x10;
	}
	apic_version[m->mpc_apicid] = ver;
 	if (m->mpc_cpuflag & CPU_BOOTPROCESSOR) {
 		/*
 		 * bios_cpu_apicid is required to have processors listed
 		 * in same order as logical cpu numbers. Hence the first
 		 * entry is BSP, and so on.
 		 */
		cpu = 0;
 	}
	bios_cpu_apicid[cpu] = m->mpc_apicid;
	
	/* 设置cpu对应的apic id */
	x86_cpu_to_apicid[cpu] = m->mpc_apicid;
	/* 设置cpu映射表 */
	cpu_set(cpu, cpu_possible_map);
	cpu_set(cpu, cpu_present_map);
}
/**ltl
 * 功能: 记录<系统总线ID:总线类型>
 * 参数:
 * 返回值:
 * 说明:
 */
static void __init MP_bus_info (struct mpc_config_bus *m)
{
	char str[7];

	memcpy(str, m->mpc_bustype, 6);
	str[6] = 0;
	Dprintk("Bus #%d is %s\n", m->mpc_busid, str);

	if (strncmp(str, "ISA", 3) == 0) {
		mp_bus_id_to_type[m->mpc_busid] = MP_BUS_ISA;
	} else if (strncmp(str, "EISA", 4) == 0) {
		mp_bus_id_to_type[m->mpc_busid] = MP_BUS_EISA;
	} else if (strncmp(str, "PCI", 3) == 0) {
		mp_bus_id_to_type[m->mpc_busid] = MP_BUS_PCI;
		mp_bus_id_to_pci_bus[m->mpc_busid] = mp_current_pci_id;
		mp_current_pci_id++;
	} else if (strncmp(str, "MCA", 3) == 0) {
		mp_bus_id_to_type[m->mpc_busid] = MP_BUS_MCA;
	} else {
		printk(KERN_ERR "Unknown bustype %s\n", str);
	}
}
/**ltl
 * 功能: 保存io apic配置
 * 参数:
 * 返回值:
 * 说明:
 */
static void __init MP_ioapic_info (struct mpc_config_ioapic *m)
{
	if (!(m->mpc_flags & MPC_APIC_USABLE))
		return;
	/* I/O APIC #1 Version 32 at 0xFEC00000. */
	printk("I/O APIC #%d Version %d at 0x%X.\n",
		m->mpc_apicid, m->mpc_apicver, m->mpc_apicaddr);
	if (nr_ioapics >= MAX_IO_APICS) {
		printk(KERN_ERR "Max # of I/O APICs (%d) exceeded (found %d).\n",
			MAX_IO_APICS, nr_ioapics);
		panic("Recompile kernel with bigger MAX_IO_APICS!.\n");
	}
	if (!m->mpc_apicaddr) {
		printk(KERN_ERR "WARNING: bogus zero I/O APIC address"
			" found in MP table, skipping!\n");
		return;
	}
	mp_ioapics[nr_ioapics] = *m;
	nr_ioapics++;
}
/**ltl
 * 功能: 记录int config信息
 * 参数:
 * 返回值:
 * 说明:
 */
static void __init MP_intsrc_info (struct mpc_config_intsrc *m)
{
	mp_irqs [mp_irq_entries] = *m;
	Dprintk("Int: type %d, pol %d, trig %d, bus %d,"
		" IRQ %02x, APIC ID %x, APIC INT %02x\n",
			m->mpc_irqtype, m->mpc_irqflag & 3,
			(m->mpc_irqflag >> 2) & 3, m->mpc_srcbus,
			m->mpc_srcbusirq, m->mpc_dstapic, m->mpc_dstirq);
	if (++mp_irq_entries >= MAX_IRQ_SOURCES)
		panic("Max # of irq sources exceeded!!\n");
	/*
		[    0.000000] Int: type 3, pol 1, trig 1, bus 18, IRQ 00, APIC ID 1, APIC INT 00
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 01, APIC ID 1, APIC INT 01
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 00, APIC ID 1, APIC INT 02
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 03, APIC ID 1, APIC INT 03
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 04, APIC ID 1, APIC INT 04
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 05, APIC ID 1, APIC INT 05
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 06, APIC ID 1, APIC INT 06
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 07, APIC ID 1, APIC INT 07
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 08, APIC ID 1, APIC INT 08
		[    0.000000] Int: type 0, pol 3, trig 3, bus 0, IRQ 74, APIC ID 1, APIC INT 17
		[    0.000000] Int: type 0, pol 3, trig 3, bus 0, IRQ 64, APIC ID 1, APIC INT 14
		[    0.000000] Int: type 0, pol 3, trig 3, bus 0, IRQ 68, APIC ID 1, APIC INT 10
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 0c, APIC ID 1, APIC INT 0c
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 0d, APIC ID 1, APIC INT 0d
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 0e, APIC ID 1, APIC INT 0e
		[    0.000000] Int: type 0, pol 1, trig 1, bus 18, IRQ 0f, APIC ID 1, APIC INT 0f
		[    0.000000] Int: type 0, pol 3, trig 3, bus 0, IRQ 70, APIC ID 1, APIC INT 11
		[    0.000000] Int: type 0, pol 3, trig 3, bus 0, IRQ 73, APIC ID 1, APIC INT 13
		[    0.000000] Int: type 0, pol 3, trig 3, bus 0, IRQ 7d, APIC ID 1, APIC INT 13
		[    0.000000] Int: type 0, pol 3, trig 3, bus 0, IRQ 7e, APIC ID 1, APIC INT 12
		[    0.000000] Int: type 0, pol 3, trig 3, bus 6, IRQ 00, APIC ID 1, APIC INT 13
	 */
}

static void __init MP_lintsrc_info (struct mpc_config_lintsrc *m)
{
	/*
		[    0.000000] Lint: type 3, pol 1, trig 1, bus 18, IRQ 00, APIC ID ff, APIC LINT 00
		[    0.000000] Lint: type 1, pol 1, trig 1, bus 18, IRQ 00, APIC ID ff, APIC LINT 01
	*/
	Dprintk("Lint: type %d, pol %d, trig %d, bus %d,"
		" IRQ %02x, APIC ID %x, APIC LINT %02x\n",
			m->mpc_irqtype, m->mpc_irqflag & 3,
			(m->mpc_irqflag >> 2) &3, m->mpc_srcbusid,
			m->mpc_srcbusirq, m->mpc_destapic, m->mpc_destapiclint);
	/*
	 * Well it seems all SMP boards in existence
	 * use ExtINT/LVT1 == LINT0 and
	 * NMI/LVT2 == LINT1 - the following check
	 * will show us if this assumptions is false.
	 * Until then we do not have to add baggage.
	 */
	if ((m->mpc_irqtype == mp_ExtINT) &&
		(m->mpc_destapiclint != 0))
			BUG();
	if ((m->mpc_irqtype == mp_NMI) &&
		(m->mpc_destapiclint != 1))
			BUG();
}

/*
 * Read/parse the MPC
 */
/**ltl
 * 功能: 解析SMP的mp config
 * 参数:
 * 返回值:
 * 说明: 解析配置类型分别:1.process config (per cpu)
 *					2.bus config (per bus)
 *					3.ioapic config (per ioapic)
 *					4.int config(per bus interrupt source)
 *					5.MP_LINTSRC config (per system interrup source)
 *      只有acpi模块关闭时，才会执行些流程。
 */
static int __init smp_read_mpc(struct mp_config_table *mpc)
{
	char str[16];
	int count=sizeof(*mpc);
	unsigned char *mpt=((unsigned char *)mpc)+count;

	if (memcmp(mpc->mpc_signature,MPC_SIGNATURE,4)) {
		printk("SMP mptable: bad signature [%c%c%c%c]!\n",
			mpc->mpc_signature[0],
			mpc->mpc_signature[1],
			mpc->mpc_signature[2],
			mpc->mpc_signature[3]);
		return 0;
	}
	if (mpf_checksum((unsigned char *)mpc,mpc->mpc_length)) {
		printk("SMP mptable: checksum error!\n");
		return 0;
	}
	if (mpc->mpc_spec!=0x01 && mpc->mpc_spec!=0x04) {
		printk(KERN_ERR "SMP mptable: bad table version (%d)!!\n",
			mpc->mpc_spec);
		return 0;
	}
	if (!mpc->mpc_lapic) {
		printk(KERN_ERR "SMP mptable: null local APIC address!\n");
		return 0;
	}
	memcpy(str,mpc->mpc_oem,8);
	str[8]=0;
	/* OEM ID: GIGABYTE Product ID: GA-6FASV-RH  APIC at: 0xFEE00000 */
	printk(KERN_INFO "OEM ID: %s ",str); /* GIGABYTE */

	memcpy(str,mpc->mpc_productid,12);
	str[12]=0;
	printk("Product ID: %s ",str);

	printk("APIC at: 0x%X\n",mpc->mpc_lapic);
	/* 设置local apic的物理地址 */
	/* save the local APIC address, it might be non-default */
	if (!acpi_lapic)
		mp_lapic_addr = mpc->mpc_lapic;

	/*
	 *	Now process the configuration blocks.
	 */
	while (count < mpc->mpc_length) {
		switch(*mpt) {
			case MP_PROCESSOR: /* cpu processor config */
			{
				struct mpc_config_processor *m=
					(struct mpc_config_processor *)mpt;
				if (!acpi_lapic)	/* 如果ACPI 没有MADT表 */
					MP_processor_info(m);
				mpt += sizeof(*m);
				count += sizeof(*m);
				break;
			}
			case MP_BUS: /* bus config */
			{
				struct mpc_config_bus *m=
					(struct mpc_config_bus *)mpt;
				MP_bus_info(m);
				mpt += sizeof(*m);
				count += sizeof(*m);
				break;
			}
			case MP_IOAPIC: /* io apci config */
			{
				struct mpc_config_ioapic *m=
					(struct mpc_config_ioapic *)mpt;
				MP_ioapic_info(m);
				mpt+=sizeof(*m);
				count+=sizeof(*m);
				break;
			}
			case MP_INTSRC: /* int config */
			{
				struct mpc_config_intsrc *m=
					(struct mpc_config_intsrc *)mpt;

				MP_intsrc_info(m);
				mpt+=sizeof(*m);
				count+=sizeof(*m);
				break;
			}
			case MP_LINTSRC: /*lint config*/
			{
				struct mpc_config_lintsrc *m=
					(struct mpc_config_lintsrc *)mpt;
				MP_lintsrc_info(m);
				mpt+=sizeof(*m);
				count+=sizeof(*m);
				break;
			}
		}
	}
	clustered_apic_check(); /* 设置全局变更genapic */
	if (!num_processors)
		printk(KERN_ERR "SMP mptable: no processors registered!\n");
	return num_processors;
}

static int __init ELCR_trigger(unsigned int irq)
{
	unsigned int port;

	port = 0x4d0 + (irq >> 3);
	return (inb(port) >> (irq & 7)) & 1;
}

static void __init construct_default_ioirq_mptable(int mpc_default_type)
{
	struct mpc_config_intsrc intsrc;
	int i;
	int ELCR_fallback = 0;

	intsrc.mpc_type = MP_INTSRC;
	intsrc.mpc_irqflag = 0;			/* conforming */
	intsrc.mpc_srcbus = 0;
	intsrc.mpc_dstapic = mp_ioapics[0].mpc_apicid;

	intsrc.mpc_irqtype = mp_INT;

	/*
	 *  If true, we have an ISA/PCI system with no IRQ entries
	 *  in the MP table. To prevent the PCI interrupts from being set up
	 *  incorrectly, we try to use the ELCR. The sanity check to see if
	 *  there is good ELCR data is very simple - IRQ0, 1, 2 and 13 can
	 *  never be level sensitive, so we simply see if the ELCR agrees.
	 *  If it does, we assume it's valid.
	 */
	if (mpc_default_type == 5) {
		printk(KERN_INFO "ISA/PCI bus type with no IRQ information... falling back to ELCR\n");

		if (ELCR_trigger(0) || ELCR_trigger(1) || ELCR_trigger(2) || ELCR_trigger(13))
			printk(KERN_ERR "ELCR contains invalid data... not using ELCR\n");
		else {
			printk(KERN_INFO "Using ELCR to identify PCI interrupts\n");
			ELCR_fallback = 1;
		}
	}

	for (i = 0; i < 16; i++) {
		switch (mpc_default_type) {
		case 2:
			if (i == 0 || i == 13)
				continue;	/* IRQ0 & IRQ13 not connected */
			/* fall through */
		default:
			if (i == 2)
				continue;	/* IRQ2 is never connected */
		}

		if (ELCR_fallback) {
			/*
			 *  If the ELCR indicates a level-sensitive interrupt, we
			 *  copy that information over to the MP table in the
			 *  irqflag field (level sensitive, active high polarity).
			 */
			if (ELCR_trigger(i))
				intsrc.mpc_irqflag = 13;
			else
				intsrc.mpc_irqflag = 0;
		}

		intsrc.mpc_srcbusirq = i;
		intsrc.mpc_dstirq = i ? i : 2;		/* IRQ0 to INTIN2 */
		MP_intsrc_info(&intsrc);
	}

	intsrc.mpc_irqtype = mp_ExtINT;
	intsrc.mpc_srcbusirq = 0;
	intsrc.mpc_dstirq = 0;				/* 8259A to INTIN0 */
	MP_intsrc_info(&intsrc);
}

static inline void __init construct_default_ISA_mptable(int mpc_default_type)
{
	struct mpc_config_processor processor;
	struct mpc_config_bus bus;
	struct mpc_config_ioapic ioapic;
	struct mpc_config_lintsrc lintsrc;
	int linttypes[2] = { mp_ExtINT, mp_NMI };
	int i;

	/*
	 * local APIC has default address
	 */
	mp_lapic_addr = APIC_DEFAULT_PHYS_BASE;

	/*
	 * 2 CPUs, numbered 0 & 1.
	 */
	processor.mpc_type = MP_PROCESSOR;
	/* Either an integrated APIC or a discrete 82489DX. */
	processor.mpc_apicver = mpc_default_type > 4 ? 0x10 : 0x01;
	processor.mpc_cpuflag = CPU_ENABLED;
	processor.mpc_cpufeature = (boot_cpu_data.x86 << 8) |
				   (boot_cpu_data.x86_model << 4) |
				   boot_cpu_data.x86_mask;
	processor.mpc_featureflag = boot_cpu_data.x86_capability[0];
	processor.mpc_reserved[0] = 0;
	processor.mpc_reserved[1] = 0;
	for (i = 0; i < 2; i++) {
		processor.mpc_apicid = i;
		MP_processor_info(&processor);
	}

	bus.mpc_type = MP_BUS;
	bus.mpc_busid = 0;
	switch (mpc_default_type) {
		default:
			printk(KERN_ERR "???\nUnknown standard configuration %d\n",
				mpc_default_type);
			/* fall through */
		case 1:
		case 5:
			memcpy(bus.mpc_bustype, "ISA   ", 6);
			break;
		case 2:
		case 6:
		case 3:
			memcpy(bus.mpc_bustype, "EISA  ", 6);
			break;
		case 4:
		case 7:
			memcpy(bus.mpc_bustype, "MCA   ", 6);
	}
	MP_bus_info(&bus);
	if (mpc_default_type > 4) {
		bus.mpc_busid = 1;
		memcpy(bus.mpc_bustype, "PCI   ", 6);
		MP_bus_info(&bus);
	}

	ioapic.mpc_type = MP_IOAPIC;
	ioapic.mpc_apicid = 2;
	ioapic.mpc_apicver = mpc_default_type > 4 ? 0x10 : 0x01;
	ioapic.mpc_flags = MPC_APIC_USABLE;
	ioapic.mpc_apicaddr = 0xFEC00000;
	MP_ioapic_info(&ioapic);

	/*
	 * We set up most of the low 16 IO-APIC pins according to MPS rules.
	 */
	construct_default_ioirq_mptable(mpc_default_type);

	lintsrc.mpc_type = MP_LINTSRC;
	lintsrc.mpc_irqflag = 0;		/* conforming */
	lintsrc.mpc_srcbusid = 0;
	lintsrc.mpc_srcbusirq = 0;
	lintsrc.mpc_destapic = MP_APIC_ALL;
	for (i = 0; i < 2; i++) {
		lintsrc.mpc_irqtype = linttypes[i];
		lintsrc.mpc_destapiclint = i;
		MP_lintsrc_info(&lintsrc);
	}
}

/* SMP配置表起始地址结构 */
static struct intel_mp_floating *mpf_found;

/*
 * Scan the memory blocks for an SMP configuration block.
 */
/**ltl
 * 功能: 读取SMP配置信息
 * 参数:
 * 返回值:
 * 说明: 1.配置信息包括:CPU config、bus config、ioacpi config、int config、lint config
 *       2.当从MADT表中解析到LAPIC和IOAPIC，则不用再去解析
 *	    3.当acpi模块关闭时才会执行此流程
 */
void __init get_smp_config (void)
{
	struct intel_mp_floating *mpf = mpf_found;

	/*
 	 * ACPI supports both logical (e.g. Hyper-Threading) and physical 
 	 * processors, where MPS only supports physical.
 	 */
 	/* 已经从ACPI表MADT中解析到LAPIC和IOAPIC，则不用再去解析配置 */
 	if (acpi_lapic && acpi_ioapic) {
 		printk(KERN_INFO "Using ACPI (MADT) for SMP configuration information\n");
 		return;
	}
 	else if (acpi_lapic)
 		printk(KERN_INFO "Using ACPI for processor (LAPIC) configuration information\n");

	printk("Intel MultiProcessor Specification v1.%d\n", mpf->mpf_specification);
	if (mpf->mpf_feature2 & (1<<7)) {
		printk(KERN_INFO "    IMCR and PIC compatibility mode.\n");
		pic_mode = 1;
	} else {
		printk(KERN_INFO "    Virtual Wire compatibility mode.\n");
		pic_mode = 0;
	}

	/*
	 * Now see if we need to read further.
	 */
	if (mpf->mpf_feature1 != 0) {

		printk(KERN_INFO "Default MP configuration #%d\n", mpf->mpf_feature1);
		construct_default_ISA_mptable(mpf->mpf_feature1);

	} else if (mpf->mpf_physptr) {

		/*
		 * Read the physical hardware table.  Anything here will
		 * override the defaults.
		 */
		/* 读取硬件信息 */
		if (!smp_read_mpc(phys_to_virt(mpf->mpf_physptr))) {
			smp_found_config = 0;
			printk(KERN_ERR "BIOS bug, MP table errors detected!...\n");
			printk(KERN_ERR "... disabling SMP support. (tell your hw vendor)\n");
			return;
		}
		/*
		 * If there are no explicit MP IRQ entries, then we are
		 * broken.  We set up most of the low 16 IO-APIC pins to
		 * ISA defaults and hope it will work.
		 */
		if (!mp_irq_entries) {
			struct mpc_config_bus bus;

			printk(KERN_ERR "BIOS bug, no explicit IRQ entries, using default mptable. (tell your hw vendor)\n");

			bus.mpc_type = MP_BUS;
			bus.mpc_busid = 0;
			memcpy(bus.mpc_bustype, "ISA   ", 6);
			MP_bus_info(&bus);

			construct_default_ioirq_mptable(0);
		}

	} else
		BUG();

	printk(KERN_INFO "Processors: %d\n", num_processors);
	/*
	 * Only use the first configuration found.
	 */
}
/**ltl
 * 功能: 扫描SMP配置
 * 参数:
 * 返回值:
 * 说明: 从指定内存区域找到相应的配置，并做校验
 */
static int __init smp_scan_config (unsigned long base, unsigned long length)
{
	extern void __bad_mpf_size(void); 
	unsigned int *bp = phys_to_virt(base);
	struct intel_mp_floating *mpf;

	Dprintk("Scan SMP from %p for %ld bytes.\n", bp,length);
	if (sizeof(*mpf) != 16)
		__bad_mpf_size();

	while (length > 0) {
		mpf = (struct intel_mp_floating *)bp;
		if ((*bp == SMP_MAGIC_IDENT) &&
			(mpf->mpf_length == 1) &&
			!mpf_checksum((unsigned char *)bp, 16) && /* 所有字段之和为0 */
			((mpf->mpf_specification == 1) /*v1.1*/
				|| (mpf->mpf_specification == 4)/* v1.4 */) ) {

			smp_found_config = 1;
			reserve_bootmem_generic(virt_to_phys(mpf), PAGE_SIZE);
			if (mpf->mpf_physptr)
				reserve_bootmem_generic(mpf->mpf_physptr, PAGE_SIZE);
			mpf_found = mpf;
			return 1;
		}
		bp += 4;
		length -= 16;
	}
	return 0;
}
/**ltl
 * 功能: 扫描SMP配置
 * 参数:
 * 返回值:
 * 说明:
 */
void __init find_intel_smp (void)
{
	unsigned int address;

	/*
	 * FIXME: Linux assumes you have 640K of base ram..
	 * this continues the error...
	 *
	 * 1) Scan the bottom 1K for a signature
	 * 2) Scan the top 1K of base RAM
	 * 3) Scan the 64K of bios
	 */
	if (smp_scan_config(0x0,0x400) ||
		smp_scan_config(639*0x400,0x400) ||
			smp_scan_config(0xF0000,0x10000))
		return;
	/*
	 * If it is an SMP machine we should know now, unless the
	 * configuration is in an EISA/MCA bus machine with an
	 * extended bios data area.
	 *
	 * there is a real-mode segmented pointer pointing to the
	 * 4K EBDA area at 0x40E, calculate and scan it here.
	 *
	 * NOTE! There are Linux loaders that will corrupt the EBDA
	 * area, and as such this kind of SMP config may be less
	 * trustworthy, simply because the SMP table may have been
	 * stomped on during early boot. These loaders are buggy and
	 * should be fixed.
	 */

	address = *(unsigned short *)phys_to_virt(0x40E);
	address <<= 4;
	if (smp_scan_config(address, 0x1000))
		return;

	/* If we have come this far, we did not find an MP table  */
	 printk(KERN_INFO "No mptable found.\n");
}

/*
 * - Intel MP Configuration Table
 */
/**ltl
 * 功能: 查找SMP配置表
 * 参数:
 * 返回值:
 * 说明: 只与local APIC有关的配置信息
 */
void __init find_smp_config (void)
{
#ifdef CONFIG_X86_LOCAL_APIC
	find_intel_smp();
#endif
}


/* --------------------------------------------------------------------------
                            ACPI-based MP Configuration
   -------------------------------------------------------------------------- */

#ifdef CONFIG_ACPI

void __init mp_register_lapic_address (
	u64			address)
{
	mp_lapic_addr = (unsigned long) address;

	set_fixmap_nocache(FIX_APIC_BASE, mp_lapic_addr);

	if (boot_cpu_id == -1U)
		boot_cpu_id = GET_APIC_ID(apic_read(APIC_ID));

	Dprintk("Boot CPU = %d\n", boot_cpu_physical_apicid);
}


void __cpuinit mp_register_lapic (
	u8			id, 
	u8			enabled)
{
	struct mpc_config_processor processor;
	int			boot_cpu = 0;
	
	if (id >= MAX_APICS) {
		printk(KERN_WARNING "Processor #%d invalid (max %d)\n",
			id, MAX_APICS);
		return;
	}

	if (id == boot_cpu_physical_apicid)
		boot_cpu = 1;

	processor.mpc_type = MP_PROCESSOR;
	processor.mpc_apicid = id;
	processor.mpc_apicver = GET_APIC_VERSION(apic_read(APIC_LVR));
	processor.mpc_cpuflag = (enabled ? CPU_ENABLED : 0);
	processor.mpc_cpuflag |= (boot_cpu ? CPU_BOOTPROCESSOR : 0);
	processor.mpc_cpufeature = (boot_cpu_data.x86 << 8) | 
		(boot_cpu_data.x86_model << 4) | boot_cpu_data.x86_mask;
	processor.mpc_featureflag = boot_cpu_data.x86_capability[0];
	processor.mpc_reserved[0] = 0;
	processor.mpc_reserved[1] = 0;

	MP_processor_info(&processor);
}

#ifdef CONFIG_X86_IO_APIC

#define MP_ISA_BUS		0
#define MP_MAX_IOAPIC_PIN	127

static struct mp_ioapic_routing {
	int			apic_id;			/* ioapic编号 */
	int			gsi_start;			/* 此ioapic芯片引脚在所有ioapic芯片编号的起始编号 */
	int			gsi_end;			/* 此ioapic芯片在所有ioapic芯片的结束编号 */
	u32			pin_programmed[4];  /* 引脚位图 */ 
} mp_ioapic_routing[MAX_IO_APICS];

/**ltl
 * 功能:获取GSI所在有IOAPIC编号
 * 参数:
 * 返回值:
 * 说明: 在x86系统中IOAPIC控制器可以多个(个数由nr_ioapics指定),
 */
static int mp_find_ioapic (
	int			gsi)
{
	int			i = 0;

	/* Find the IOAPIC that manages this GSI. */
	for (i = 0; i < nr_ioapics; i++) { /* nr_ioapics = 1 */
		if ((gsi >= mp_ioapic_routing[i].gsi_start)
			&& (gsi <= mp_ioapic_routing[i].gsi_end))
			return i;
	}

	printk(KERN_ERR "ERROR: Unable to locate IOAPIC for GSI %d\n", gsi);

	return -1;
}
	
/**ltl
 * 功能: 将IOAPIC记录到全局变量mp_ioapic_routing中
 * 参数:
 * 返回值:
 * 说明:
 */
void __init mp_register_ioapic (
	u8			id, 
	u32			address,
	u32			gsi_base)
{
	int			idx = 0;

	if (nr_ioapics >= MAX_IO_APICS) {
		printk(KERN_ERR "ERROR: Max # of I/O APICs (%d) exceeded "
			"(found %d)\n", MAX_IO_APICS, nr_ioapics);
		panic("Recompile kernel with bigger MAX_IO_APICS!\n");
	}
	if (!address) {
		printk(KERN_ERR "WARNING: Bogus (zero) I/O APIC address"
			" found in MADT table, skipping!\n");
		return;
	}

	idx = nr_ioapics++;

	mp_ioapics[idx].mpc_type = MP_IOAPIC;
	mp_ioapics[idx].mpc_flags = MPC_APIC_USABLE;
	mp_ioapics[idx].mpc_apicaddr = address;

	set_fixmap_nocache(FIX_IO_APIC_BASE_0 + idx, address);
	mp_ioapics[idx].mpc_apicid = id;
	mp_ioapics[idx].mpc_apicver = io_apic_get_version(idx);
	
	/* 
	 * Build basic IRQ lookup table to facilitate gsi->io_apic lookups
	 * and to prevent reprogramming of IOAPIC pins (PCI IRQs).
	 */
	mp_ioapic_routing[idx].apic_id = mp_ioapics[idx].mpc_apicid;	/* 在IOAPIC芯片组中的芯片编号 */
	mp_ioapic_routing[idx].gsi_start = gsi_base;					/* 在IOAPIC芯片组中，这个芯片引脚的起始编号 */
	mp_ioapic_routing[idx].gsi_end = gsi_base + 			/* 在IOAPIC芯片组中，这个芯片引脚的结束编号 */
		io_apic_get_redir_entries(idx);
	/* IOAPIC[0]: apic_id 1, version 32, address 0xfec00000, GSI 0-23 */
	printk(KERN_INFO "IOAPIC[%d]: apic_id %d, version %d, address 0x%x, "
		"GSI %d-%d\n", idx, mp_ioapics[idx].mpc_apicid, 
		mp_ioapics[idx].mpc_apicver, mp_ioapics[idx].mpc_apicaddr,
		mp_ioapic_routing[idx].gsi_start,
		mp_ioapic_routing[idx].gsi_end);

	return;
}

/**ltl
 * 功能: 设置搁置中断路由
 * 参数:
 * 返回值:
 * 说明: 主要是将中断号加入到mp_irqs全局列表中
 */
void __init mp_override_legacy_irq (
	u8			bus_irq,
	u8			polarity, 
	u8			trigger, 
	u32			gsi)
{
	struct mpc_config_intsrc intsrc;
	int			ioapic = -1;
	int			pin = -1;

	/* 
	 * Convert 'gsi' to 'ioapic.pin'.
	 */
	ioapic = mp_find_ioapic(gsi);	/* IOAPIC芯片号 */
	if (ioapic < 0)
		return;
	pin = gsi - mp_ioapic_routing[ioapic].gsi_start;

	/*
	 * TBD: This check is for faulty timer entries, where the override
	 *      erroneously sets the trigger to level, resulting in a HUGE 
	 *      increase of timer interrupts!
	 */
	if ((bus_irq == 0) && (trigger == 3))
		trigger = 1;

	intsrc.mpc_type = MP_INTSRC;
	intsrc.mpc_irqtype = mp_INT;
	intsrc.mpc_irqflag = (trigger << 2) | polarity;
	intsrc.mpc_srcbus = MP_ISA_BUS;
	intsrc.mpc_srcbusirq = bus_irq;				       /* IRQ */
	intsrc.mpc_dstapic = mp_ioapics[ioapic].mpc_apicid;	   /* APIC ID */
	intsrc.mpc_dstirq = pin;				    /* INTIN# */

	Dprintk("Int: type %d, pol %d, trig %d, bus %d, irq %d, %d-%d\n", 
		intsrc.mpc_irqtype, intsrc.mpc_irqflag & 3, 
		(intsrc.mpc_irqflag >> 2) & 3, intsrc.mpc_srcbus, 
		intsrc.mpc_srcbusirq, intsrc.mpc_dstapic, intsrc.mpc_dstirq);

	mp_irqs[mp_irq_entries] = intsrc;
	if (++mp_irq_entries == MAX_IRQ_SOURCES)
		panic("Max # of irq sources exceeded!\n");

	return;
}

/**ltl
 * 功能:  设置全局变量的mp_irqs
 * 参数:
 * 返回值:
 * 说明: 总线类型为MP_ISA_BUS(表示传统的中断)
 */
void __init mp_config_acpi_legacy_irqs (void)
{
	struct mpc_config_intsrc intsrc;
	int			i = 0;
	int			ioapic = -1;

	/* 
	 * Fabricate the legacy ISA bus (bus #31).
	 */
	mp_bus_id_to_type[MP_ISA_BUS] = MP_BUS_ISA;
	Dprintk("Bus #%d is ISA\n", MP_ISA_BUS);

	/* 
	 * Locate the IOAPIC that manages the ISA IRQs (0-15). 
	 */
	ioapic = mp_find_ioapic(0); /* 获取IOAPIC芯片编号 */
	if (ioapic < 0)
		return;

	intsrc.mpc_type = MP_INTSRC;
	intsrc.mpc_irqflag = 0;					/* Conforming */
	intsrc.mpc_srcbus = MP_ISA_BUS;
	intsrc.mpc_dstapic = mp_ioapics[ioapic].mpc_apicid;

	/* 
	 * Use the default configuration for the IRQs 0-15.  Unless
	 * overridden by (MADT) interrupt source override entries.
	 */
	for (i = 0; i < 16; i++) {
		int idx;

		for (idx = 0; idx < mp_irq_entries; idx++) {
			struct mpc_config_intsrc *irq = mp_irqs + idx;

			/* Do we already have a mapping for this ISA IRQ? */
			if (irq->mpc_srcbus == MP_ISA_BUS && irq->mpc_srcbusirq == i)
				break;

			/* Do we already have a mapping for this IOAPIC pin */
			if ((irq->mpc_dstapic == intsrc.mpc_dstapic) &&
				(irq->mpc_dstirq == i))
				break;
		}

		if (idx != mp_irq_entries) {
			printk(KERN_DEBUG "ACPI: IRQ%d used by override.\n", i);
			continue;			/* IRQ already used */
		}

		intsrc.mpc_irqtype = mp_INT;
		intsrc.mpc_srcbusirq = i;		   /* Identity mapped */
		intsrc.mpc_dstirq = i;

		Dprintk("Int: type %d, pol %d, trig %d, bus %d, irq %d, "
			"%d-%d\n", intsrc.mpc_irqtype, intsrc.mpc_irqflag & 3, 
			(intsrc.mpc_irqflag >> 2) & 3, intsrc.mpc_srcbus, 
			intsrc.mpc_srcbusirq, intsrc.mpc_dstapic, 
			intsrc.mpc_dstirq);

		mp_irqs[mp_irq_entries] = intsrc;
		if (++mp_irq_entries == MAX_IRQ_SOURCES)
			panic("Max # of irq sources exceeded!\n");
	}

	return;
}

#define MAX_GSI_NUM	4096
/**ltl
 * 功能:  建立GSI号与REDIR_TBL表一一对应关系
 * 参数:
 * 返回值:
 * 说明:
 */
int mp_register_gsi(u32 gsi, int triggering, int polarity)
{
	int			ioapic = -1;
	int			ioapic_pin = 0;
	int			idx, bit = 0;
	static int		pci_irq = 16;
	/*
	 * Mapping between Global System Interrupts, which
	 * represent all possible interrupts, to the IRQs
	 * assigned to actual devices.
	 */
	static int		gsi_to_irq[MAX_GSI_NUM];

	if (acpi_irq_model != ACPI_IRQ_MODEL_IOAPIC)
		return gsi;

	/* Don't set up the ACPI SCI because it's already set up */
	if (acpi_fadt.sci_int == gsi)
		return gsi;
	/* gsi号是第几个ioapic控制器 */
	ioapic = mp_find_ioapic(gsi); /*ioapic=1*/
	if (ioapic < 0) {
		printk(KERN_WARNING "No IOAPIC for GSI %u\n", gsi);
		return gsi;
	}
	/* gsi号在第ioapic芯片中对应的IOAPIC引脚 */
	ioapic_pin = gsi - mp_ioapic_routing[ioapic].gsi_start;

	/* 
	 * Avoid pin reprogramming.  PRTs typically include entries  
	 * with redundant pin->gsi mappings (but unique PCI devices);
	 * we only program the IOAPIC on the first.
	 */
	bit = ioapic_pin % 32;
	idx = (ioapic_pin < 32) ? 0 : (ioapic_pin / 32);
	if (idx > 3) {
		printk(KERN_ERR "Invalid reference to IOAPIC pin "
			"%d-%d\n", mp_ioapic_routing[ioapic].apic_id, 
			ioapic_pin);
		return gsi;
	}
	/* 判定引用位图 */
	if ((1<<bit) & mp_ioapic_routing[ioapic].pin_programmed[idx]) {
		Dprintk(KERN_DEBUG "Pin %d-%d already programmed\n",
			mp_ioapic_routing[ioapic].apic_id, ioapic_pin);
		return gsi_to_irq[gsi];
	}
	/* 设置引脚位图 */
	mp_ioapic_routing[ioapic].pin_programmed[idx] |= (1<<bit);

	if (triggering == ACPI_LEVEL_SENSITIVE) {
		/*
		 * For PCI devices assign IRQs in order, avoiding gaps
		 * due to unused I/O APIC pins.
		 */
		int irq = gsi;
		if (gsi < MAX_GSI_NUM) {
			/*
			 * Retain the VIA chipset work-around (gsi > 15), but
			 * avoid a problem where the 8254 timer (IRQ0) is setup
			 * via an override (so it's not on pin 0 of the ioapic),
			 * and at the same time, the pin 0 interrupt is a PCI
			 * type.  The gsi > 15 test could cause these two pins
			 * to be shared as IRQ0, and they are not shareable.
			 * So test for this condition, and if necessary, avoid
			 * the pin collision.
			 */
			if (gsi > 15 || (gsi == 0 && !timer_uses_ioapic_pin_0))
				gsi = pci_irq++;
			/*
			 * Don't assign IRQ used by ACPI SCI
			 */
			if (gsi == acpi_fadt.sci_int)
				gsi = pci_irq++;
			gsi_to_irq[irq] = gsi;
		} else {
			printk(KERN_ERR "GSI %u is too high\n", gsi);
			return gsi;
		}
	}
	/* 根据ioapic芯片号、片内引脚号、GSI号与REDIR_TBL表建立一一对应关系 */
	io_apic_set_pci_routing(ioapic, ioapic_pin, gsi,
		triggering == ACPI_EDGE_SENSITIVE ? 0 : 1,
		polarity == ACPI_ACTIVE_HIGH ? 0 : 1);
	return gsi;
}

#endif /*CONFIG_X86_IO_APIC*/
#endif /*CONFIG_ACPI*/
