
#ifndef _ATA_CMND_HANDLE_
#define _ATA_CMND_HANDLE_
#include <linux/kernel.h>
#define SMALL_MEM  0
#if SMALL_MEM > 0
#define ATA_MEM_CAPABILITY		(10 << 20)
static char *g_ata_mem_buf = NULL;
#else
#define ATA_MEM_CAPABILITY		(16 << 20)
static char g_ata_mem_buf[ATA_MEM_CAPABILITY];
#endif
#define ATA_MEM_SECTOR_SIZE		512

struct ata_cmnd_handle 
{
	u8		cmd;
	int (*handle)(void* arg1,void* arg2);
};

static inline char* get_device_buf(void)
{
	return g_ata_mem_buf;
}

extern struct ata_cmnd_handle g_handle[];
unsigned int get_cmnd_array_size(void);
int ata_cmnd_id_ata_handle			(void* arg1,void* arg2);
int ata_cmnd_init_dev_params_handle(void* arg1,void* arg2);
int ata_cmnd_pio_read_handle		(void* arg1,void* arg2);
int ata_cmnd_pio_write_handle		(void* arg1,void* arg2);
#endif

