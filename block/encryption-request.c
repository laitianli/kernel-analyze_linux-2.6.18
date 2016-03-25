/******
 * 实现bio请求的加解密功能
 */
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include "encryption-request.h"
#include "config_encryption_disk.h"
 
#undef NLog
#undef ELog
#define ELog(fmt,arg...) printk(KERN_WARNING"[Encryption]=[%s:%d]="fmt"\n",__func__,__LINE__,##arg);
#define NLog(n,fmt,arg...)	do{	static int i = 0;if(i++ < n){printk(KERN_WARNING"[Encryption]=[%s:%d]="fmt"\n",__func__,__LINE__,##arg);}}while(0)
static char* encryption(unsigned char *buf, int len);
static char* decryption(unsigned char *buf, int len);
static int be_encryption_disk(const char* partition_name);

static int be_encryption_disk(const char* partition_name)
{
#if 0
	if( !strcmp(partition_name,"sdb")  || 
		!strcmp(partition_name,"sdb1") ||
		!strcmp(partition_name,"sdb2") ||
		!strcmp(partition_name,"sdb3") ||
		!strcmp(partition_name,"sdb4") ||
		!strcmp(partition_name,"sdc")  ||
		!strcmp(partition_name,"sdc1") ||
		!strcmp(partition_name,"sdc2") ||
		!strcmp(partition_name,"sdc3"))
		return 1;
	
	return 0;
#else
	return is_encrytion_disk(partition_name);
#endif
} 
/**ltl
 * 功能: 加密写bio请求的完成回调函数。
 * 参数: bio	-> bio请求对象
 *	    err	-> 错误码
 * 返回值: 无
 * 说明: 这个函数由请求处理完成后调用
 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
static int encryption_end_io_write(struct bio *bio, unsigned int bytes_done, int err)
#else
static void encryption_end_io_write(struct bio *bio, int err)
#endif
{
	struct bio *bio_orig = bio->bi_private;
	struct bio_vec *bvec, *org_vec;
	int i;
	/* 释放加密bio请求的每个page */
 	__bio_for_each_segment(bvec, bio, i, 0) {
		org_vec = bio_orig->bi_io_vec + i;
		__free_page(bvec->bv_page);
	} 
	kfree(bio->bi_private1);
	bio->bi_private1 = NULL;
	bio_orig->bi_private1 = NULL;
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
	bio_endio(bio_orig, bytes_done, err);
#else
	/* 请求加密前的bio的完成处理函数 */
	bio_endio(bio_orig, err);
#endif
	bio_put(bio); /* 释放bio请求 */

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
	return 0;
#endif
}
 
/**ltl
 * 功能: 拷贝bio对象
 * 参数: q	-> 请求队列对象
 *	    org_bio->bio对象
 * 返回值: 新的bio对象
 * 说明: 读写bio请求的拷贝接口
 */
static struct bio* copy_bio(struct request_queue *q, struct bio* org_bio,
		bio_end_io_t* end_bio_fun)
{
	struct bio_vec *to, *from;
	int i, rw = bio_data_dir(org_bio); 
	char *vto, *vfrom;	
	unsigned int cnt = org_bio->bi_vcnt;
	/* 分配bio对象 */
	struct bio* bio = bio_alloc(GFP_NOIO, cnt);
	if (!bio)
		return org_bio;
	memset(bio->bi_io_vec, 0, cnt * sizeof(struct bio_vec));
	/* 遍历bio,拷贝其数据和属性 */
	bio_for_each_segment(from, org_bio, i) {		
		to = bio->bi_io_vec + i;
		to->bv_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		to->bv_len = from->bv_len;
		to->bv_offset = from->bv_offset;
		
		flush_dcache_page(from->bv_page);
		vto = page_address(to->bv_page) + to->bv_offset;
		if(rw == WRITE) {/* 只有读操作才拷贝数据 */			
			/* page在高端内存中 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
			if (page_to_pfn(from->bv_page) > q-> bounce_pfn)
#else				
			if (page_to_pfn(from->bv_page) > queue_bounce_pfn(q)) 
#endif				
				vfrom = kmap(from->bv_page) + from->bv_offset;
			else /* page在底端内存中 */
				vfrom = page_address(from->bv_page) + from->bv_offset;
			memcpy(vto, vfrom, to->bv_len); /* 拷贝数据 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
			if (page_to_pfn(from->bv_page) > q-> bounce_pfn)
#else			
			if (page_to_pfn(from->bv_page) > queue_bounce_pfn(q))
#endif				
				kunmap(from->bv_page);
		}
	}
	/* 拷贝属性 */
	bio->bi_bdev = org_bio->bi_bdev;
	bio->bi_flags = org_bio->bi_flags;
	bio->bi_sector = org_bio->bi_sector;
	bio->bi_rw = org_bio->bi_rw;

	bio->bi_vcnt = org_bio->bi_vcnt;
	bio->bi_idx = org_bio->bi_idx;
	bio->bi_size = org_bio->bi_size;
	bio->bi_end_io = end_bio_fun;
	bio->bi_private = org_bio;
	bio->bi_private1 = org_bio->bi_private1;
	return bio;
	
}
/**ltl
 * 功能:写请求加密接口
 * 参数: q	-> 请求队列对象
 *		bio	->[in] bio写请求对象 ; [out] 重新生成的已经被加密过的请求。
 * 返回值: 无
 * 说明: 1. copy bio对象中。
 */
void encryption_request(struct request_queue *q, struct bio **bio)
{
	struct bio_vec *from;
	struct page *page;
	struct bio *new_bio = NULL; /* 在这里要重新创建一个bio交给底层处理 */
	unsigned char* buf = NULL;
	int i = 0;
	char b[BDEVNAME_SIZE]={0}; 
	
 	if ((*bio)->bi_private1) /* 此已经加密过 */
 		return ;

	/* 是否是需要加密的磁盘 */
	if(!(*bio)->bi_bdev || 
		!(bdevname((*bio)->bi_bdev, b) && strlen(b))|| 
		!be_encryption_disk(b))
		return ;
	
	/* 保存加密盘的分区名 */
	(*bio)->bi_private1 = kzalloc(BDEVNAME_SIZE, GFP_KERNEL);
	if(!(*bio)->bi_private1)
		return;
	strncpy((char*)((*bio)->bi_private1), b, BDEVNAME_SIZE-1);
	
	if(bio_data_dir(*bio) != WRITE)
	{/* 加密盘的读操作 */ 
		return ;
	}
	
	/*写操作 */
	/* 拷贝bio对象 */
	new_bio = copy_bio(q, *bio, encryption_end_io_write);
	if(new_bio == *bio)
		return ;
	NLog(30,"begin encryption disk: %s", b);
	
	bio_for_each_segment(from, new_bio, i) { 
		page = from->bv_page;
 		/* page在高端内存中 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
		if (page_to_pfn(page) > q-> bounce_pfn)
#else		
		if (page_to_pfn(page) > queue_bounce_pfn(q))
#endif			
			buf = kmap(page) + from->bv_offset;
		else /* page在底端内存中 */
			buf = page_address(page) + from->bv_offset;
		/* 对buf加密 */
		encryption(buf, from->bv_len);			
	}
	/* 将加密过的bio请求返回 */
	*bio = new_bio; 
}

/**ltl
 * 功能: 读请求加密接口
 * 参数: q	->请求队列对象
 *		bio	->bio读请求对象
 * 返回值:无
 * 说明: 遍历bio对象中的每一page，对page中的数据解密
 */
void decryption_reuqest(struct request_queue *q, struct bio *bio)
{
	struct bio_vec *from;
	struct page *page;
	unsigned char* buf = NULL;
	int i = 0;
	 /* 是否是读操作 */
	if(bio_data_dir(bio) != READ || 
		!bio->bi_private1 || 
		!be_encryption_disk(bio->bi_private1)) /* 是否是需要加密的磁盘 */
		return ; 
	
	NLog(30,"decryption disk: %s", (const char*)(bio->bi_private1));
	kfree(bio->bi_private1);
	bio->bi_private1 = NULL;
	/* 对bio中的所有数据解密处理 */
	bio_for_each_segment(from, bio, i) {
		page = from->bv_page;
		flush_dcache_page(page);
		/* page在高端内存中 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
		if (page_to_pfn(page) > q-> bounce_pfn)
#else		
		if (page_to_pfn(page) > queue_bounce_pfn(q))
#endif			
			buf = kmap(page) + from->bv_offset;
		else /* page在底端内存中 */
			buf = page_address(page) + from->bv_offset;
		/* buf解密 */
		decryption(buf, from->bv_len);	
	}
	
}
EXPORT_SYMBOL(encryption_request);
EXPORT_SYMBOL(decryption_reuqest);

/**ltl
 * 功能:加密算法接口
 * 参数:
 * 返回值:
 * 说明: 对所有数据+1
 */
static char* encryption(unsigned char *buf, int len)
{
	int i = 0;
	for (i = 0; i < len; i++)
 		buf[i] += 1;

	return buf;
}

/**ltl
 * 功能:解密算法接口
 * 参数:
 * 返回值:
 * 说明: 对所有数据-1
 */
static char* decryption(unsigned char *buf, int len)
{
	int i = 0;
	for (i = 0; i < len; i++)
		buf[i] -= 1;
	return buf;
}
