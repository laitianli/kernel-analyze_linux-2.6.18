#ifndef _ENCRYPTION_REQUEST_H_
#define _ENCRYPTION_REQUEST_H_

#include <linux/blkdev.h>
#include <linux/bio.h>
void encryption_request(struct request_queue *q, struct bio **bio);
void decryption_reuqest(struct request_queue *q, struct bio *bio);

#endif
