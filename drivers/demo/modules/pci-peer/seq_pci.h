#ifndef _SEQ_PCI_H_
#define _SEQ_PCI_H_
#include <linux/kernel.h>
#include <linux/init.h>
struct pci_config_common
{
	u16 vendorID,deviceID;
    u16 command,status;
    u8  revision;
    u32 classcode;
    u8 cache_line_size,latency_time,head_type,bist;
	u32 bar0,bar1;
	u8 inter_line,inter_pin;
	 u8  cap_base;
};

struct pci_dev_config
{	struct pci_config_common common;
    u32 bar2,bar3,bar4,bar5;
    u32 cardbusCIS;
    u16 sub_vendorID,sub_deviceID;
    u32 rom_base_addr;    
    u8 min_gnt,max_lat;
	
	u8 belong_primary,belong_secondary
};

struct pci_bridge_config
{
	struct pci_config_common common;
    u8 primary,secondary,subordinate,secondary_latency_timer;
    u8 iobase,iolimit;
    u16 secondary_status;
    u16 membase,memlimit;
    u16 prefmembase,prefmemlimit;
    u32 prefmembaseupper32,prefmemlimitupper32;
    u16 iobaseupper16,iolimitupper16;
	u32 rom_base_addr; 
    u16 bridge_ctl;
};

#endif

