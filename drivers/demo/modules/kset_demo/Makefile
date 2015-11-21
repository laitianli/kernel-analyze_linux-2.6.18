MODULE_NAME=kset
ifneq ($(KERNELRELEASE),)
	obj-m = $(MODULE_NAME).o
	$(MODULE_NAME)-objs = kset_demo.o
else
	KER-DIR=/home/laitianli/kernel/linux-2.6.18
	PWD=$(shell pwd)
default:
	$(MAKE) -C $(KER-DIR) M=$(PWD) ARCH=x86_64 CROSS_COMPILE= modules 

endif

clean:
	rm -rf *.o *.ko .*.cmd *.mod.*  Module.symvers .tmp_versions
