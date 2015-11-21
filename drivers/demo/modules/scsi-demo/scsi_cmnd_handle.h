#ifndef _SCSI_CMND_HANDLE_H_
#define _SCSI_CMND_HANDLE_H_
#include <linux/module.h>
#include <linux/init.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

struct cmnd_handle
{
    unsigned char cmd;
    int (*cmnd_function)(struct scsi_cmnd* cmd);
};
extern struct cmnd_handle g_cmnd[];
unsigned get_cmnd_array_size(void);
#endif

