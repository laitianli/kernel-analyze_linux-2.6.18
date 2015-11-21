#! /bin/bash

insmod ata-demo/ata-mem.ko
sleep 1
insmod scsi-demo/scsi-mem.ko
insmod sm-demo/sm-mod.ko


