#include <stdio.h>
#include <stdlib.h>

#include "platform_defs.h"
#include "bio.h"

void
bio_init(
	struct bio *bio, struct block_device *bdev, struct bio_vec *table,
	unsigned short max_vecs, blk_opf_t opf)
{
	bio->__bi_remaining = 1;
	bio->bi_bdev = bdev;
	bio->bi_opf = opf;
	bio->bi_status = 0;
	bio->bi_iter.bi_sector = 0;
	bio->bi_iter.bi_size = 0;
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_end_io = NULL;
	bio->bi_private = NULL;
	bio->bi_vcnt = 0;
	bio->bi_io_vec = table;
}

static struct page *
kmem_to_page(void *data)
{
	struct page *page;

	page = calloc(1, sizeof(*page));
	ASSERT(page != NULL);

	page->bufp = data;
}

static unsigned int
offset_in_page(void *data)
{
	return 0;
}

int
bio_add_page(struct bio *bio, struct page *page, unsigned int len,
		unsigned int offset)
{
	struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt];

	ASSERT(offset == 0);

	bv->bv_page = page;
	bv->bv_len = len;

	bio->bi_vcnt++;
}

struct bio *
bio_split(struct bio *bio, int sectors,
		gfp_t gfp, struct bio_set *bs)
{
	struct bio *split;
	struct bio_vec *bv;
	unsigned int bytes;
	unsigned int idx;

	split = calloc(1, sizeof(*bio));
	if (!split)
		return NULL;

	bytes = sectors << 9;

	bio_init(split, bio->bi_bdev, NULL, 0, bio->bi_opf);

	split->bi_iter = bio->bi_iter;
	split->bi_iter.bi_size = bytes;

	split->bi_io_vec = bio->bi_io_vec;

	/* Advance source bio */
	bio->bi_iter.bi_sector += bytes;
	bio->bi_iter.bi_size -= bytes;
	bytes += bio->bi_iter.bi_bvec_done;

	idx = bio->bi_iter.bi_idx;
	bv = &bio->bi_io_vec[idx];
	while (bytes && bytes >= bv[idx].bv_len) {
		bytes -= bv[idx].bv_len;
		idx++;
	}
	bio->bi_iter.bi_idx = idx;
	bio->bi_iter.bi_bvec_done = bytes;

	return split;
}

static void
bio_put(struct bio *bio)
{
	free(bio);
}

static struct bio *__bio_chain_endio(struct bio *bio)
{
	struct bio *parent = bio->bi_private;
	if (bio->bi_status && !parent->bi_status)
		parent->bi_status = bio->bi_status;
	bio_put(bio);
	return parent;
}

static inline bool bio_remaining_done(struct bio *bio)
{
	/*
	 * If we're not chaining, then ->__bi_remaining is always 1 and
	 * we always end io on the first invocation.
	 */
	if (!bio_flagged(bio, BIO_CHAIN))
		return true;

	ASSERT(bio->__bi_remaining > 0);
	--bio->__bi_remaining;
	if (bio->__bi_remaining == 0) {
		bio_clear_flag(bio, BIO_CHAIN);
		return true;
	}

	return false;
}

static void
bio_endio(struct bio *bio)
{
	if (!bio_remaining_done(bio))
		return;
	if (bio->bi_end_io)
		bio->bi_end_io(bio);
}

static void
bio_chain_endio(struct bio *bio)
{
	bio_endio(__bio_chain_endio(bio));
}

static inline void bio_inc_remaining(struct bio *bio)
{
	bio_set_flag(bio, BIO_CHAIN);
	++bio->__bi_remaining;
}

void
bio_chain(struct bio *bio, struct bio *parent)
{
	ASSERT(bio->bi_private == NULL && bio->bi_end_io == NULL);

	bio->bi_private = parent;
	bio->bi_end_io	= bio_chain_endio;
	bio_inc_remaining(parent);
}

void submit_bio(struct bio *bio)
{
	struct bio_vec *bv;
	unsigned int skip;
	unsigned int idx;
	off_t offset;
	int error;


	offset = bio->bi_iter.bi_sector << 9;
	idx = bio->bi_iter.bi_idx;
	skip = bio->bi_iter.bi_bvec_done;

	for (; idx < bio->bi_vcnt; idx++) {
		bv = &bio->bi_io_vec[idx];
		/* chandan: Handle short reads/writes */
		if (bio_op(bio) == REQ_OP_WRITE) {
			error = pwrite(bio->bi_bdev->dev_fd,
					bv->bv_page->bufp + skip,
					bv->bv_len - skip, offset);
			if (error == -1) {
				bio->bi_status = -errno;
				break;
			}
		} else if (bio_op(bio) == REQ_OP_READ) {
			error = pread(bio->bi_bdev->dev_fd,
					bv->bv_page->bufp + skip,
					bv->bv_len - skip, offset);
			if (error <= 0) {
				if (error == -1)
					bio->bi_status = -errno;
				break;
			}
		}

		offset += bv->bv_len - skip;
		skip = 0;
	}

	bio_endio(bio);
}


