#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/pci.h>
#include <linux/list.h>
#include <linux/pci_regs.h>
#include "seq_pci.h"

//capabilities ID
#define POWER_MANAGEMENT_CAPABILITY		0x01
#define MSI_CAPABILITY					0x05
#define MSIX_CAPABILITY					0x11
#define PCI_EXPRESS_CAPABILITY			0x10
//0x03
//0x09
//0x0d
//0x0a
//0x12
//0x13


#define PCILog(fmt,arg...) printk(KERN_WARNING"	"fmt"\n",##arg)
struct pci_dev_config g_pci_dev ;
struct pci_bridge_config g_pci_bdg ;

static void read_pci_common_register(struct pci_dev* pdev,struct pci_config_common* pcom);
static void read_pci_register(struct pci_dev* pdev);
static void read_pci_bridge_register(struct pci_dev* pdev);
static void read_pci_dev_register(struct pci_dev* pdev);
static void read_pci_capability_register(struct pci_dev* pdev,const u8* cab);
static void print_pci_dev(struct pci_dev_config* pdev);
static void print_pci_bridge(struct pci_bridge_config* pdev);

static void text_fun(struct pci_dev* pdev);

static void read_pci_common_register(struct pci_dev* pdev,struct pci_config_common* pcom)
{
	u32 reg_ver_class = 0;
	if(!pcom)
		return;
	
    pci_read_config_word(pdev,PCI_VENDOR_ID,&pcom->vendorID);
    pci_read_config_word(pdev,PCI_DEVICE_ID,&pcom->deviceID);


    pci_read_config_word(pdev,PCI_COMMAND,&pcom->command);
    pci_read_config_word(pdev,PCI_STATUS,&pcom->status);

    
    pci_read_config_dword(pdev,PCI_CLASS_REVISION,&reg_ver_class);
    pcom->revision = reg_ver_class & 0xF;
    pcom->classcode = reg_ver_class >> 8;


    pci_read_config_byte(pdev,PCI_CACHE_LINE_SIZE,&pcom->cache_line_size);
    pci_read_config_byte(pdev,PCI_LATENCY_TIMER,&pcom->latency_time);
    pci_read_config_byte(pdev,PCI_HEADER_TYPE,&pcom->head_type);
    pci_read_config_byte(pdev,PCI_BIST,&pcom->bist);


	pci_read_config_dword(pdev,PCI_BASE_ADDRESS_0,&pcom->bar0);
    pci_read_config_dword(pdev,PCI_BASE_ADDRESS_1,&pcom->bar1);
	
	pci_read_config_byte(pdev,PCI_CAPABILITY_LIST,&pcom->cap_base);
	
	pci_read_config_byte(pdev,PCI_INTERRUPT_LINE,&pcom->inter_line);
    pci_read_config_byte(pdev,PCI_INTERRUPT_PIN,&pcom->inter_pin);
	
}

static void read_pci_bridge_register(struct pci_dev* pdev)
{
    read_pci_common_register(pdev,&g_pci_bdg.common);
	
    pci_read_config_byte(pdev,PCI_PRIMARY_BUS,&g_pci_bdg.primary);
    pci_read_config_byte(pdev,PCI_SECONDARY_BUS,&g_pci_bdg.secondary);
    pci_read_config_byte(pdev,PCI_SUBORDINATE_BUS,&g_pci_bdg.subordinate);
    pci_read_config_byte(pdev,PCI_SEC_LATENCY_TIMER,&g_pci_bdg.secondary_latency_timer);

    pci_read_config_byte(pdev,PCI_IO_BASE,&g_pci_bdg.iobase);
    pci_read_config_byte(pdev,PCI_IO_LIMIT,&g_pci_bdg.iolimit);
    pci_read_config_word(pdev,PCI_SEC_STATUS,&g_pci_bdg.secondary_status);


    pci_read_config_word(pdev,PCI_MEMORY_BASE,&g_pci_bdg.membase);
    pci_read_config_word(pdev,PCI_MEMORY_LIMIT,&g_pci_bdg.memlimit);
	
    pci_read_config_word(pdev,PCI_PREF_MEMORY_BASE,&g_pci_bdg.prefmembase);
    pci_read_config_word(pdev,PCI_PREF_MEMORY_LIMIT,&g_pci_bdg.prefmemlimit);

    pci_read_config_dword(pdev,PCI_PREF_BASE_UPPER32,&g_pci_bdg.prefmembaseupper32);
    pci_read_config_dword(pdev,PCI_PREF_LIMIT_UPPER32,&g_pci_bdg.prefmemlimitupper32);


    pci_read_config_word(pdev,PCI_IO_BASE_UPPER16,&g_pci_bdg.iobaseupper16);
    pci_read_config_word(pdev,PCI_IO_LIMIT_UPPER16,&g_pci_bdg.iolimitupper16);
	
    pci_read_config_dword(pdev,PCI_ROM_ADDRESS1,&g_pci_bdg.rom_base_addr);
	
    pci_read_config_word(pdev,PCI_BRIDGE_CONTROL,&g_pci_bdg.bridge_ctl);
	
}
static void read_pci_dev_register(struct pci_dev* pdev)
{
    read_pci_common_register(pdev,&g_pci_dev.common);


    pci_read_config_dword(pdev,PCI_BASE_ADDRESS_2,&g_pci_dev.bar2);
    pci_read_config_dword(pdev,PCI_BASE_ADDRESS_3,&g_pci_dev.bar3);
    pci_read_config_dword(pdev,PCI_BASE_ADDRESS_4,&g_pci_dev.bar4);
    pci_read_config_dword(pdev,PCI_BASE_ADDRESS_5,&g_pci_dev.bar5);   

    pci_read_config_dword(pdev,PCI_CARDBUS_CIS,&g_pci_dev.cardbusCIS);
	
    pci_read_config_word(pdev,PCI_SUBSYSTEM_VENDOR_ID,&g_pci_dev.sub_vendorID);
    pci_read_config_word(pdev,PCI_SUBSYSTEM_ID,&g_pci_dev.sub_deviceID);

    pci_read_config_dword(pdev,PCI_ROM_ADDRESS,&g_pci_dev.rom_base_addr);

    pci_read_config_byte(pdev,PCI_MIN_GNT,&g_pci_dev.min_gnt);
    pci_read_config_byte(pdev,PCI_MAX_LAT,&g_pci_dev.max_lat);
	
	if(pdev->bus)//this device belong to which bridge
	{
		g_pci_dev.belong_primary = pdev->bus->primary;
		g_pci_dev.belong_secondary = pdev->bus->secondary;
	}
}

static void read_pci_capability_register(struct pci_dev* pdev,const u8* cab)
{
	u8 capID = 0,next_cap_point = 0;
	if(!pdev || !cab || *cab < 0x40)
	{
		PCILog("\n");
		return ;
	}
	u8 where = *cab;
	do
	{
		pci_read_config_byte(pdev,where,&capID);
		pci_read_config_byte(pdev,where+1,&next_cap_point);
		PCILog("capID=0x%02x,next cap point=0x%02x",capID,next_cap_point);
		if(next_cap_point < 0x40)
			break;
			
		where = next_cap_point;
	}while(1);
	PCILog("\n");
}

static void print_pci_dev(struct pci_dev_config* pdev)
{
    printk(KERN_WARNING"-->>PCI Device:\n");
    PCILog("vendor:device=%x:%x",pdev->common.vendorID,pdev->common.deviceID);
	PCILog("command reg=0x%x,status reg=0x%x",pdev->common.command,pdev->common.status);
	PCILog("revision=0x%x",pdev->common.revision);
	PCILog("classcode=0x%x",pdev->common.classcode);
	PCILog("cache_line_size=0x%x,latency_time=0x%x,head_type=0x%x,bist=0x%x",
		pdev->common.cache_line_size,pdev->common.latency_time,pdev->common.head_type,pdev->common.bist);
	PCILog("bar0=0x%x",pdev->common.bar0);
	PCILog("bar1=0x%x",pdev->common.bar1);
	PCILog("bar2=0x%x",pdev->bar2);
	PCILog("bar3=0x%x",pdev->bar3);
	PCILog("bar4=0x%x",pdev->bar4);
	PCILog("bar5=0x%x",pdev->bar5);
	PCILog("CardBus CIS Point=0x%x",pdev->cardbusCIS);
	PCILog("Sub vendorID: Sub deviceID=%x:%x",pdev->sub_vendorID,pdev->sub_deviceID);
	PCILog("Ext Rom Base Addr=0x%x",pdev->rom_base_addr);
	PCILog("capabilities point=0x%x",pdev->common.cap_base);
	PCILog("interr line=0x%x,inter pin=0x%x,min gnt=0x%x,max lat=0x%x",
		pdev->common.inter_line,pdev->common.inter_pin,pdev->min_gnt,pdev->max_lat);
		
	PCILog("belong bus primary=%d,secondary=%d",pdev->belong_primary,pdev->belong_secondary);
		
//	printk(KERN_WARNING"\n");
}
static void print_pci_bridge(struct pci_bridge_config* pdev)
{
     printk(KERN_WARNING"-->>PCI Bridge:\n");
	PCILog("vendor:device=%x:%x",pdev->common.vendorID,pdev->common.deviceID);
	PCILog("command reg=0x%x,status reg=0x%x",pdev->common.command,pdev->common.status);
	PCILog("revision=0x%x",pdev->common.revision);
	PCILog("classcode=0x%x",pdev->common.classcode);
	PCILog("cache_line_size=0x%x,latency_time=0x%x,head_type=0x%x,bist=0x%x",
		pdev->common.cache_line_size,pdev->common.latency_time,pdev->common.head_type,pdev->common.bist);
	PCILog("bar0=0x%x",pdev->common.bar0);
	PCILog("bar1=0x%x",pdev->common.bar1);
	
	PCILog("primary=%d,secondary=%d,subordinate=%d",pdev->primary,pdev->secondary,pdev->subordinate);
	PCILog("secondary_latency_timer=%d",pdev->secondary_latency_timer);
	
	PCILog("IO Base=0x%x,IO limit=0x%x",pdev->iobase,pdev->iolimit);
	PCILog("secondary status=0x%x",pdev->secondary_status);
	PCILog("MEM Base=0x%x,MEM Limit=0x%x",pdev->membase,pdev->memlimit);
	PCILog("PRE MEM Base=0x%x,PRE MEM Limit=0x%x",pdev->prefmembase,pdev->prefmemlimit);
	PCILog("PRE MEM Base Upper_32=0x%x,PRE MEM Limit Upper_32=0x%x",pdev->prefmembaseupper32,pdev->prefmemlimitupper32);
	
	PCILog("IO Base Upper_16=0x%x,IO limit Upper_16=0x%x",pdev->iobaseupper16,pdev->iolimitupper16);
	PCILog("capabilities point=0x%x",pdev->common.cap_base);
	PCILog("Ext Rom Base Addr=0x%x",pdev->rom_base_addr);
	PCILog("interr line=0x%x,inter pin=0x%x",pdev->common.inter_line,pdev->common.inter_pin);
	PCILog("Bridge contrl=0x%x",pdev->bridge_ctl);
	
//	printk(KERN_WARNING"\n");
}

static void read_pci_register(struct pci_dev* pdev)
{
    u8 reg_head_type = 0;
    pci_read_config_byte(pdev,PCI_HEADER_TYPE,&reg_head_type);
    switch(reg_head_type & 0x3F)
    {
        case PCI_HEADER_TYPE_NORMAL://0x0:pci device
            read_pci_dev_register(pdev);
			print_pci_dev(&g_pci_dev);
			read_pci_capability_register(pdev,&g_pci_dev.common.cap_base);
			
			text_fun(pdev);
            break;
        case PCI_HEADER_TYPE_BRIDGE://0x1:pci bridge
            read_pci_bridge_register(pdev);
			print_pci_bridge(&g_pci_bdg);
			read_pci_capability_register(pdev,&g_pci_bdg.common.cap_base);

            break;
        case PCI_HEADER_TYPE_CARDBUS://0x2:cardbus
            Log("cardbus device");
            break;
        default:
            BUG_ON(1);
    }	
}

static void text_fun(struct pci_dev* pdev)
{
	#if 0
	u32 addr = 0;
	u32 size = 0;
	int i = 0 ,count = 6;
	for(i = 0; i < count ; i++)
	{
		pci_read_config_dword(pdev,PCI_BASE_ADDRESS_0 + i * 4,&addr);
		pci_write_config_dword(pdev,PCI_BASE_ADDRESS_0 + i * 4,~0);
		pci_read_config_dword(pdev,PCI_BASE_ADDRESS_0 + i * 4,&size);
		pci_write_config_dword(pdev,PCI_BASE_ADDRESS_0 + i * 4,&addr);
		PCILog("bar%d size:0x%x",i,size);
	}
	#endif
}

static void* pci_printer_start(struct seq_file* seqf,loff_t* pos)
{
    loff_t n = *pos;
    struct pci_dev* pdev = NULL;
    for_each_pci_dev(pdev)
    {
        if(!n--)
            break;
    }
    return pdev;
}

static void* pci_printer_next(struct seq_file* seqf,void* v,loff_t* pos)
{    
    struct pci_dev* pdev = (struct pci_dev*)v;
	(*pos)++;
    pdev = pci_get_device(PCI_ANY_ID,PCI_ANY_ID,pdev);
    return pdev;
}

static void pci_printer_stop(struct seq_file* seqf,void* v)
{
    if(v)
    {
        struct pci_dev* pdev = (struct pci_dev*)v;
        pci_dev_put(pdev);
    }
    //LogPath();
}

static int pci_printer_show(struct seq_file* seqf,void* v)
{
    struct pci_dev* pdev = (struct pci_dev*)v;
    if(!pdev)
        return -1;
    read_pci_register(pdev);
    return 0;
}

static const struct seq_operations g_pci_printer_ops = {
    .start = pci_printer_start,
    .next  = pci_printer_next,
    .stop  = pci_printer_stop,
    .show  = pci_printer_show,
};

static int pci_printer_open(struct inode* inode,struct file* file)
{
    LogPath();

    return seq_open(file,&g_pci_printer_ops);
}

static struct file_operations g_fops = {
    .open = pci_printer_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = seq_release,
    .owner = THIS_MODULE, 
};

static int __init pci_printer_init(void)
{
    proc_create("pci_peer",0,NULL,&g_fops);
    return 0;
}

static void __exit pci_printer_exit(void)
{
    remove_proc_entry("pci_peer",NULL);
}

module_init(pci_printer_init);
module_exit(pci_printer_exit);

MODULE_LICENSE("GPL");
