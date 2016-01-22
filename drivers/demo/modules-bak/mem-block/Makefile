PWD=$(shell pwd)
subdir=$(PWD)/ata-demo $(PWD)/scsi-demo $(PWD)/sm-demo


all:default
default clean:
	$(foreach i, $(subdir),$(MAKE) -C $(i) $@;)
#	 	$(MAKE) -C $(i) default
	

#$clean:
#	$(MAKE) -C $(subdir) $@
