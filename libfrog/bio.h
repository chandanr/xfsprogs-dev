#ifndef __BIO_H_
#define __BIO_H_

struct bio;

typedef void (bio_end_io_t) (struct bio *);

struct block_device {
	int dev_fd;
};

struct bvec_iter {
	sector_t bi_sector;
	unsigned int bi_size;
	unsigned int bi_idx;	/* chandan: Index into bio_io_vec[] which should be written next. */
	unsigned int bi_bvec_done; /* chandan: Number of bytes from bi_io_vec already written */
};

struct bio {
	struct block_device bi_bdev;
	struct bvec_iter bi_iter; /* chandan: stores disk offset to write to and size of data to be written.  */
	bio_end_io_t *bi_end_io;
	void *bi_private;
	unsigned short bi_vcnt;	/* chandan: maybe this isn't required. */
	struct bio_vec *bi_io_vec;
	int __bi_remaining;	/* chandan: Number of children bios remaining. */
};

/*
 * Implement the following functions.
 * bio_init()
 * bio_add_page(); To determine the value of PAGE_SIZE use getpagesize()
 * bio_split()
 * bio_chain()
 * blkdev_issue_flush()
 * submit_bio()
 */

#endif	/* __BIO_H_ */

