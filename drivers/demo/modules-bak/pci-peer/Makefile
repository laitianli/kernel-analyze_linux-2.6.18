MODULE_NAME=pci-peer

ifneq ($(KERNELRELEASE),)
	obj-m = $(MODULE_NAME).o
	$(MODULE_NAME)-objs=seq_pci.o
else
	PWD=$(shell pwd)
	KER-DIR=/home/laitianli/kernel/linux-2.6.32.9
default:
	$(MAKE) -C $(KER-DIR) M=$(PWD) ARCH=x86_64 CROSS_COMPILE= modules
endif


clean:
	rm -rf *.o *.ko .*.cmd *.mod.*  Module.symvers .tmp_versions modules.order
