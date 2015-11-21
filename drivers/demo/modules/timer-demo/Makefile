MODULE_NAME=timer
ifneq ($(KERNELRELEASE),)
	obj-m=$(MODULE_NAME).o
	$(MODULE_NAME)-objs=timer-demo.o
else
	PWD=$(shell pwd)
	KER-DIR=/home/laitianli/kernel/linux-2.6.18
#	KER-DIR=/home/laitianli/kernel/linux-2.6.32.9
default:
	$(MAKE) -C $(KER-DIR) M=$(PWD) ARCH=x86_64 CROSS_COMPILE= modules 
endif
clean:
	rm -rf *.o *.ko .*.cmd *.mod.*  Module.symvers .tmp_versions
