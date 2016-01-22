MODULE_NAME=driver-module

ifneq ($(KERNELRELEASE),)
	obj-m = $(MODULE_NAME).o
	$(MODULE_NAME)-objs = driver_module.o bus_demo.o device_demo.o driver_demo.o
else
	KER_DIR=/home/laitianli/kernel/linux-2.6.18
	PWD = $(shell pwd)
default:
	$(MAKE) -C $(KER_DIR) M=$(PWD) ARCH=x86_64 CROSS_COMPILE= modules

endif


clean:
	rm -rf *.o *.ko .*.cmd *.mod.*  Module.symvers .tmp_versions
