MODULE_NAME = class-demo
_ARCH ?=$(ARCH)
_CROSS_COMPILE ?=$(CROSS_COMPILE)

ifeq ($(_ARCH),)
	_ARCH=x86_64
endif

ifneq ($(KERNELRELEASE),)
	obj-m = $(MODULE_NAME).o
	$(MODULE_NAME)-objs = class_demo.o
else
	PWD=$(shell pwd)
	KERD ?=/home/laitianli/kernel/linux-2.6.18
default:
	$(MAKE) -C $(KERD) M=$(PWD) ARCH=$(_ARCH) CROSS_COMPILE=$(_CROSS_COMPILE) modules

endif

clean:
	rm -rf *.o *.ko .*.cmd *.mod.*  Module.symvers .tmp_versions modules.order	
