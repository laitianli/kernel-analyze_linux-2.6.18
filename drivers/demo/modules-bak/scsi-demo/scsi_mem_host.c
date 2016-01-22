#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi.h>
#include "scsi_mem_host.h"
#include "scsi_cmnd_handle.h"

#define SCSI_MEM_HOST_PROC_NAME "scsi-name"

int scsi_mem_host_proc_info(struct Scsi_Host* host,char* buffer,char** start,off_t offset,int len,int fun);
int scsi_mem_host_qcmd(struct scsi_cmnd* cmnd,void (*done)(struct scsi_cmnd*));
struct scsi_host_template g_scsi_mem_drv_template = {
    .module     = THIS_MODULE,
    .proc_name  = SCSI_MEM_HOST_PROC_NAME,
    .proc_info  = scsi_mem_host_proc_info,
    .name       = "scsi mem name",
    .queuecommand = scsi_mem_host_qcmd,
    .can_queue    = 127,
    .this_id      = -1,
    .sg_tablesize = 128,
    .max_sectors  = 8192,
    .cmd_per_lun  = 7,
};



int scsi_mem_host_proc_info(struct Scsi_Host* host,char* buffer,char** start,off_t offset,int len,int fun)
{
    LogPath();
    return 0;
}

int scsi_mem_host_qcmd(struct scsi_cmnd* cmnd,void (*done)(struct scsi_cmnd*))
{
    if(cmnd->cmnd[0] != 0x28 && cmnd->cmnd[0] != 0x2A)
        Log("cmd id:0x%x",cmnd->cmnd[0]);
    int ret = 0;
    unsigned int i = 0;
    int bHandle = 0;
    for(i = 0; i < get_cmnd_array_size(); i++)
    {
        if(cmnd->cmnd[0] == g_cmnd[i].cmd)
        {
            g_cmnd[i].cmnd_function(cmnd);
            bHandle = 1;
            break;
        }
    }
    if(bHandle != 1)
    {
        Log("cmd id:0x%x do not handle..",cmnd->cmnd[0]);
    }
    if(done)
        done(cmnd);
    return 0;
}



