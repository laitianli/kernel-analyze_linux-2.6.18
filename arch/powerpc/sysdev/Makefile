ifeq ($(CONFIG_PPC64),y)
EXTRA_CFLAGS			+= -mno-minimal-toc
endif

obj-$(CONFIG_MPIC)		+= mpic.o
obj-$(CONFIG_PPC_INDIRECT_PCI)	+= indirect_pci.o
obj-$(CONFIG_PPC_MPC106)	+= grackle.o
obj-$(CONFIG_BOOKE)		+= dcr.o
obj-$(CONFIG_40x)		+= dcr.o
obj-$(CONFIG_U3_DART)		+= dart_iommu.o
obj-$(CONFIG_MMIO_NVRAM)	+= mmio_nvram.o
obj-$(CONFIG_FSL_SOC)		+= fsl_soc.o
obj-$(CONFIG_PPC_TODC)		+= todc.o
obj-$(CONFIG_TSI108_BRIDGE)	+= tsi108_pci.o tsi108_dev.o

ifeq ($(CONFIG_PPC_MERGE),y)
obj-$(CONFIG_PPC_I8259)		+= i8259.o
obj-$(CONFIG_PPC_83xx)		+= ipic.o
endif
