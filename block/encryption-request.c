/******
 * ʵ��bio����ļӽ��ܹ���
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
 * ����: ����дbio�������ɻص�������
 * ����: bio	-> bio�������
 *	    err	-> ������
 * ����ֵ: ��
 * ˵��: �����������������ɺ����
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
	/* �ͷż���bio�����ÿ��page */
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
	/* �������ǰ��bio����ɴ����� */
	bio_endio(bio_orig, err);
#endif
	bio_put(bio); /* �ͷ�bio���� */

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
	return 0;
#endif
}
 
/**ltl
 * ����: ����bio����
 * ����: q	-> ������ж���
 *	    org_bio->bio����
 * ����ֵ: �µ�bio����
 * ˵��: ��дbio����Ŀ����ӿ�
 */
static struct bio* copy_bio(struct request_queue *q, struct bio* org_bio,
		bio_end_io_t* end_bio_fun)
{
	struct bio_vec *to, *from;
	int i, rw = bio_data_dir(org_bio); 
	char *vto, *vfrom;	
	unsigned int cnt = org_bio->bi_vcnt;
	/* ����bio���� */
	struct bio* bio = bio_alloc(GFP_NOIO, cnt);
	if (!bio)
		return org_bio;
	memset(bio->bi_io_vec, 0, cnt * sizeof(struct bio_vec));
	/* ����bio,���������ݺ����� */
	bio_for_each_segment(from, org_bio, i) {		
		to = bio->bi_io_vec + i;
		to->bv_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		to->bv_len = from->bv_len;
		to->bv_offset = from->bv_offset;
		
		flush_dcache_page(from->bv_page);
		vto = page_address(to->bv_page) + to->bv_offset;
		if(rw == WRITE) {/* ֻ�ж������ſ������� */			
			/* page�ڸ߶��ڴ��� */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
			if (page_to_pfn(from->bv_page) > q-> bounce_pfn)
#else				
			if (page_to_pfn(from->bv_page) > queue_bounce_pfn(q)) 
#endif				
				vfrom = kmap(from->bv_page) + from->bv_offset;
			else /* page�ڵ׶��ڴ��� */
				vfrom = page_address(from->bv_page) + from->bv_offset;
			memcpy(vto, vfrom, to->bv_len); /* �������� */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
			if (page_to_pfn(from->bv_page) > q-> bounce_pfn)
#else			
			if (page_to_pfn(from->bv_page) > queue_bounce_pfn(q))
#endif				
				kunmap(from->bv_page);
		}
	}
	/* �������� */
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
 * ����:д������ܽӿ�
 * ����: q	-> ������ж���
 *		bio	->[in] bioд������� ; [out] �������ɵ��Ѿ������ܹ�������
 * ����ֵ: ��
 * ˵��: 1. copy bio�����С�
 */
void encryption_request(struct request_queue *q, struct bio **bio)
{
	struct bio_vec *from;
	struct page *page;
	struct bio *new_bio = NULL; /* ������Ҫ���´���һ��bio�����ײ㴦�� */
	unsigned char* buf = NULL;
	int i = 0;
	char b[BDEVNAME_SIZE]={0}; 
	
 	if ((*bio)->bi_private1) /* ���Ѿ����ܹ� */
 		return ;

	/* �Ƿ�����Ҫ���ܵĴ��� */
	if(!(*bio)->bi_bdev || 
		!(bdevname((*bio)->bi_bdev, b) && strlen(b))|| 
		!be_encryption_disk(b))
		return ;
	
	/* ��������̵ķ����� */
	(*bio)->bi_private1 = kzalloc(BDEVNAME_SIZE, GFP_KERNEL);
	if(!(*bio)->bi_private1)
		return;
	strncpy((char*)((*bio)->bi_private1), b, BDEVNAME_SIZE-1);
	
	if(bio_data_dir(*bio) != WRITE)
	{/* �����̵Ķ����� */ 
		return ;
	}
	
	/*д���� */
	/* ����bio���� */
	new_bio = copy_bio(q, *bio, encryption_end_io_write);
	if(new_bio == *bio)
		return ;
	NLog(30,"begin encryption disk: %s", b);
	
	bio_for_each_segment(from, new_bio, i) { 
		page = from->bv_page;
 		/* page�ڸ߶��ڴ��� */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
		if (page_to_pfn(page) > q-> bounce_pfn)
#else		
		if (page_to_pfn(page) > queue_bounce_pfn(q))
#endif			
			buf = kmap(page) + from->bv_offset;
		else /* page�ڵ׶��ڴ��� */
			buf = page_address(page) + from->bv_offset;
		/* ��buf���� */
		encryption(buf, from->bv_len);			
	}
	/* �����ܹ���bio���󷵻� */
	*bio = new_bio; 
}

/**ltl
 * ����: ��������ܽӿ�
 * ����: q	->������ж���
 *		bio	->bio���������
 * ����ֵ:��
 * ˵��: ����bio�����е�ÿһpage����page�е����ݽ���
 */
void decryption_reuqest(struct request_queue *q, struct bio *bio)
{
	struct bio_vec *from;
	struct page *page;
	unsigned char* buf = NULL;
	int i = 0;
	 /* �Ƿ��Ƕ����� */
	if(bio_data_dir(bio) != READ || 
		!bio->bi_private1 || 
		!be_encryption_disk(bio->bi_private1)) /* �Ƿ�����Ҫ���ܵĴ��� */
		return ; 
	
	NLog(30,"decryption disk: %s", (const char*)(bio->bi_private1));
	kfree(bio->bi_private1);
	bio->bi_private1 = NULL;
	/* ��bio�е��������ݽ��ܴ��� */
	bio_for_each_segment(from, bio, i) {
		page = from->bv_page;
		flush_dcache_page(page);
		/* page�ڸ߶��ڴ��� */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)			
		if (page_to_pfn(page) > q-> bounce_pfn)
#else		
		if (page_to_pfn(page) > queue_bounce_pfn(q))
#endif			
			buf = kmap(page) + from->bv_offset;
		else /* page�ڵ׶��ڴ��� */
			buf = page_address(page) + from->bv_offset;
		/* buf���� */
		decryption(buf, from->bv_len);	
	}
	
}
EXPORT_SYMBOL(encryption_request);
EXPORT_SYMBOL(decryption_reuqest);

/**ltl
 * ����:�����㷨�ӿ�
 * ����:
 * ����ֵ:
 * ˵��: ����������+1
 */
static char* encryption(unsigned char *buf, int len)
{
	int i = 0;
	for (i = 0; i < len; i++)
 		buf[i] += 1;

	return buf;
}

/**ltl
 * ����:�����㷨�ӿ�
 * ����:
 * ����ֵ:
 * ˵��: ����������-1
 */
static char* decryption(unsigned char *buf, int len)
{
	int i = 0;
	for (i = 0; i < len; i++)
		buf[i] -= 1;
	return buf;
}
