#ifndef __BIO_H_
#define __BIO_H_

#include <stdint.h>
#include <stdbool.h>
#include <linux/types.h>

#include "kmem.h"

#define __force

#define PAGE_SIZE getpagesize()

/* chandan: Will check this later. */
#define blkdev_issue_flush(bdev) (0)

struct bio;
struct bio_set;

typedef __u32 blk_opf_t;
typedef __u64 sector_t;
typedef __u8 blk_status_t;

struct page {
	void *bufp;
};

/**
 * enum req_op - Operations common to the bio and request structures.
 * We use 8 bits for encoding the operation, and the remaining 24 for flags.
 *
 * The least significant bit of the operation number indicates the data
 * transfer direction:
 *
 *   - if the least significant bit is set transfers are TO the device
 *   - if the least significant bit is not set transfers are FROM the device
 *
 * If a operation does not transfer data the least significant bit has no
 * meaning.
 */
enum req_op {
	/* read sectors from the device */
	REQ_OP_READ		= (__force blk_opf_t)0,
	/* write sectors to the device */
	REQ_OP_WRITE		= (__force blk_opf_t)1,
	/* flush the volatile write cache */
	REQ_OP_FLUSH		= (__force blk_opf_t)2,
	/* discard sectors */
	REQ_OP_DISCARD		= (__force blk_opf_t)3,
	/* securely erase sectors */
	REQ_OP_SECURE_ERASE	= (__force blk_opf_t)5,
	/* write the zero filled sector many times */
	REQ_OP_WRITE_ZEROES	= (__force blk_opf_t)9,
	/* Open a zone */
	REQ_OP_ZONE_OPEN	= (__force blk_opf_t)10,
	/* Close a zone */
	REQ_OP_ZONE_CLOSE	= (__force blk_opf_t)11,
	/* Transition a zone to full */
	REQ_OP_ZONE_FINISH	= (__force blk_opf_t)12,
	/* write data at the current zone write pointer */
	REQ_OP_ZONE_APPEND	= (__force blk_opf_t)13,
	/* reset a zone write pointer */
	REQ_OP_ZONE_RESET	= (__force blk_opf_t)15,
	/* reset all the zone present on the device */
	REQ_OP_ZONE_RESET_ALL	= (__force blk_opf_t)17,

	/* Driver private requests */
	REQ_OP_DRV_IN		= (__force blk_opf_t)34,
	REQ_OP_DRV_OUT		= (__force blk_opf_t)35,

	REQ_OP_LAST		= (__force blk_opf_t)36,
};

#define REQ_OP_BITS     8
#define REQ_OP_MASK     (__force blk_opf_t)((1 << REQ_OP_BITS) - 1)
#define REQ_FLAG_BITS   24

/*
 * bio flags
 */
enum {
	BIO_PAGE_PINNED,	/* Unpin pages in bio_release_pages() */
	BIO_CLONED,		/* doesn't own data */
	BIO_BOUNCED,		/* bio is a bounce bio */
	BIO_QUIET,		/* Make BIO Quiet */
	BIO_CHAIN,		/* chained bio, ->bi_remaining in effect */
	BIO_REFFED,		/* bio has elevated ->bi_cnt */
	BIO_BPS_THROTTLED,	/* This bio has already been subjected to
				 * throttling rules. Don't do it again. */
	BIO_TRACE_COMPLETION,	/* bio_endio() should trace the final completion
				 * of this bio. */
	BIO_CGROUP_ACCT,	/* has been accounted to a cgroup */
	BIO_QOS_THROTTLED,	/* bio went through rq_qos throttle path */
	BIO_QOS_MERGED,		/* but went through rq_qos merge path */
	BIO_REMAPPED,
	BIO_ZONE_WRITE_LOCKED,	/* Owns a zoned device zone write lock */
	BIO_FLAG_LAST
};

typedef void (bio_end_io_t) (struct bio *);

struct block_device {
	int dev_fd;
};

struct bvec_iter {
	sector_t bi_sector;
	unsigned int bi_size;
	unsigned int bi_idx;
	unsigned int bi_bvec_done;
};

struct bio_vec {
	struct page *bv_page;
	unsigned int bv_len;
};

struct bio {
	struct block_device *bi_bdev;
	blk_opf_t bi_opf;
	unsigned short bi_flags;
	blk_status_t bi_status;
	int __bi_remaining;
	struct bvec_iter bi_iter;
	bio_end_io_t *bi_end_io;
	void *bi_private;
	unsigned short bi_vcnt;
	struct bio_vec *bi_io_vec;
};

static inline bool bio_flagged(struct bio *bio, unsigned int bit)
{
	return bio->bi_flags & (1U << bit);
}

static inline void bio_set_flag(struct bio *bio, unsigned int bit)
{
	bio->bi_flags |= (1U << bit);
}

static inline void bio_clear_flag(struct bio *bio, unsigned int bit)
{
	bio->bi_flags &= ~(1U << bit);
}

static inline enum req_op bio_op(const struct bio *bio)
{
	return bio->bi_opf & REQ_OP_MASK;
}

void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
		unsigned short max_vecs, blk_opf_t opf);
int bio_add_page(struct bio *bio, struct page *page, unsigned int len,
		 unsigned int offset);
struct bio *bio_split(struct bio *bio, int sectors, gfp_t gfp,
		struct bio_set *bs);
void bio_chain(struct bio *bio, struct bio *parent);
void submit_bio(struct bio *bio);

#endif	/* __BIO_H_ */

