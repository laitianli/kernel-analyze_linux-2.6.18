MODULE_NAME=sm-mod
_ARCH=$(ARCH)
_CROSS_COMPILE=$(CROSS_COMPILE)
ifeq ($(_ARCH),)
	_ARCH=x86_64
endif
ifneq ($(KERNELRELEASE),)
	obj-m=$(MODULE_NAME).o
	$(MODULE_NAME)-objs=sm.o
else
	PWD=$(shell pwd)
	#KER-DIR=/home/laitianli/kernel/linux-2.6.18
	KERD ?=/home/laitianli/kernel/linux-2.6.32.9
default:
	$(MAKE) -C $(KERD) M=$(PWD) ARCH=$(_ARCH) CROSS_COMPILE=$(_CROSS_COMPILE) modules
endif

clean:
	rm -rf *.o *.ko .*.cmd *.mod.*  Module.symvers .tmp_versions modules.order
