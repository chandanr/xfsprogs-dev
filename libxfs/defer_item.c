// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_alloc.h"
#include "xfs_rmap.h"
#include "xfs_refcount.h"
#include "xfs_bmap.h"
#include "xfs_inode.h"
#include "xfs_swapext.h"

/* Dummy defer item ops, since we don't do logging. */

/* Extent Freeing */

/* Sort bmap items by AG. */
static int
xfs_extent_free_diff_items(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	struct xfs_mount		*mp = priv;
	struct xfs_extent_free_item	*ra;
	struct xfs_extent_free_item	*rb;

	ra = container_of(a, struct xfs_extent_free_item, xefi_list);
	rb = container_of(b, struct xfs_extent_free_item, xefi_list);
	return  XFS_FSB_TO_AGNO(mp, ra->xefi_startblock) -
		XFS_FSB_TO_AGNO(mp, rb->xefi_startblock);
}

/* Get an EFI. */
static struct xfs_log_item *
xfs_extent_free_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_extent_free_diff_items);
	return NULL;
}

/* Get an EFD so we can process all the free extents. */
static struct xfs_log_item *
xfs_extent_free_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Process a free extent. */
STATIC int
xfs_extent_free_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_extent_free_item	*free;
	int				error;

	free = container_of(item, struct xfs_extent_free_item, xefi_list);
	error = xfs_free_extent(tp, free->xefi_startblock,
			free->xefi_blockcount, &free->xefi_oinfo,
			XFS_AG_RESV_NONE);
	kmem_free(free);
	return error;
}

/* Abort all pending EFIs. */
STATIC void
xfs_extent_free_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a free extent. */
STATIC void
xfs_extent_free_cancel_item(
	struct list_head		*item)
{
	struct xfs_extent_free_item	*free;

	free = container_of(item, struct xfs_extent_free_item, xefi_list);
	kmem_free(free);
}

const struct xfs_defer_op_type xfs_extent_free_defer_type = {
	.create_intent	= xfs_extent_free_create_intent,
	.abort_intent	= xfs_extent_free_abort_intent,
	.create_done	= xfs_extent_free_create_done,
	.finish_item	= xfs_extent_free_finish_item,
	.cancel_item	= xfs_extent_free_cancel_item,
};

/*
 * AGFL blocks are accounted differently in the reserve pools and are not
 * inserted into the busy extent list.
 */
STATIC int
xfs_agfl_free_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_extent_free_item	*free;
	struct xfs_buf			*agbp;
	int				error;
	xfs_agnumber_t			agno;
	xfs_agblock_t			agbno;

	free = container_of(item, struct xfs_extent_free_item, xefi_list);
	ASSERT(free->xefi_blockcount == 1);
	agno = XFS_FSB_TO_AGNO(mp, free->xefi_startblock);
	agbno = XFS_FSB_TO_AGBNO(mp, free->xefi_startblock);

	error = xfs_alloc_read_agf(mp, tp, agno, 0, &agbp);
	if (!error)
		error = xfs_free_agfl_block(tp, agno, agbno, agbp,
					    &free->xefi_oinfo);
	kmem_free(free);
	return error;
}

/* sub-type with special handling for AGFL deferred frees */
const struct xfs_defer_op_type xfs_agfl_free_defer_type = {
	.create_intent	= xfs_extent_free_create_intent,
	.abort_intent	= xfs_extent_free_abort_intent,
	.create_done	= xfs_extent_free_create_done,
	.finish_item	= xfs_agfl_free_finish_item,
	.cancel_item	= xfs_extent_free_cancel_item,
};

/* Reverse Mapping */

/* Sort rmap intents by AG. */
static int
xfs_rmap_update_diff_items(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	struct xfs_mount		*mp = priv;
	struct xfs_rmap_intent		*ra;
	struct xfs_rmap_intent		*rb;

	ra = container_of(a, struct xfs_rmap_intent, ri_list);
	rb = container_of(b, struct xfs_rmap_intent, ri_list);
	return  XFS_FSB_TO_AGNO(mp, ra->ri_bmap.br_startblock) -
		XFS_FSB_TO_AGNO(mp, rb->ri_bmap.br_startblock);
}

/* Get an RUI. */
static struct xfs_log_item *
xfs_rmap_update_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	 struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_rmap_update_diff_items);
	return NULL;
}

/* Get an RUD so we can process all the deferred rmap updates. */
static struct xfs_log_item *
xfs_rmap_update_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Process a deferred rmap update. */
STATIC int
xfs_rmap_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_rmap_intent		*rmap;
	int				error;

	rmap = container_of(item, struct xfs_rmap_intent, ri_list);
	error = xfs_rmap_finish_one(tp,
			rmap->ri_type,
			rmap->ri_owner, rmap->ri_whichfork,
			rmap->ri_bmap.br_startoff,
			rmap->ri_bmap.br_startblock,
			rmap->ri_bmap.br_blockcount,
			rmap->ri_bmap.br_state,
			state);
	kmem_free(rmap);
	return error;
}

/* Abort all pending RUIs. */
STATIC void
xfs_rmap_update_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a deferred rmap update. */
STATIC void
xfs_rmap_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_rmap_intent		*rmap;

	rmap = container_of(item, struct xfs_rmap_intent, ri_list);
	kmem_free(rmap);
}

const struct xfs_defer_op_type xfs_rmap_update_defer_type = {
	.create_intent	= xfs_rmap_update_create_intent,
	.abort_intent	= xfs_rmap_update_abort_intent,
	.create_done	= xfs_rmap_update_create_done,
	.finish_item	= xfs_rmap_update_finish_item,
	.finish_cleanup = xfs_rmap_finish_one_cleanup,
	.cancel_item	= xfs_rmap_update_cancel_item,
};

/* Reference Counting */

/* Sort refcount intents by AG. */
static int
xfs_refcount_update_diff_items(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	struct xfs_mount		*mp = priv;
	struct xfs_refcount_intent	*ra;
	struct xfs_refcount_intent	*rb;

	ra = container_of(a, struct xfs_refcount_intent, ri_list);
	rb = container_of(b, struct xfs_refcount_intent, ri_list);
	return  XFS_FSB_TO_AGNO(mp, ra->ri_startblock) -
		XFS_FSB_TO_AGNO(mp, rb->ri_startblock);
}

/* Get an CUI. */
static struct xfs_log_item *
xfs_refcount_update_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_refcount_update_diff_items);
	return NULL;
}

/* Get an CUD so we can process all the deferred refcount updates. */
static struct xfs_log_item *
xfs_refcount_update_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Process a deferred refcount update. */
STATIC int
xfs_refcount_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_refcount_intent	*refc;
	xfs_fsblock_t			new_fsb;
	xfs_extlen_t			new_aglen;
	int				error;

	refc = container_of(item, struct xfs_refcount_intent, ri_list);
	error = xfs_refcount_finish_one(tp,
			refc->ri_type,
			refc->ri_startblock,
			refc->ri_blockcount,
			&new_fsb, &new_aglen,
			state);
	/* Did we run out of reservation?  Requeue what we didn't finish. */
	if (!error && new_aglen > 0) {
		ASSERT(refc->ri_type == XFS_REFCOUNT_INCREASE ||
		       refc->ri_type == XFS_REFCOUNT_DECREASE);
		refc->ri_startblock = new_fsb;
		refc->ri_blockcount = new_aglen;
		return -EAGAIN;
	}
	kmem_free(refc);
	return error;
}

/* Abort all pending CUIs. */
STATIC void
xfs_refcount_update_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a deferred refcount update. */
STATIC void
xfs_refcount_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_refcount_intent	*refc;

	refc = container_of(item, struct xfs_refcount_intent, ri_list);
	kmem_free(refc);
}

const struct xfs_defer_op_type xfs_refcount_update_defer_type = {
	.create_intent	= xfs_refcount_update_create_intent,
	.abort_intent	= xfs_refcount_update_abort_intent,
	.create_done	= xfs_refcount_update_create_done,
	.finish_item	= xfs_refcount_update_finish_item,
	.finish_cleanup = xfs_refcount_finish_one_cleanup,
	.cancel_item	= xfs_refcount_update_cancel_item,
};

/* Inode Block Mapping */

/* Sort bmap intents by inode. */
static int
xfs_bmap_update_diff_items(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	struct xfs_bmap_intent		*ba;
	struct xfs_bmap_intent		*bb;

	ba = container_of(a, struct xfs_bmap_intent, bi_list);
	bb = container_of(b, struct xfs_bmap_intent, bi_list);
	return ba->bi_owner->i_ino - bb->bi_owner->i_ino;
}

/* Get an BUI. */
static struct xfs_log_item *
xfs_bmap_update_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	struct xfs_mount		*mp = tp->t_mountp;

	if (sort)
		list_sort(mp, items, xfs_bmap_update_diff_items);
	return NULL;
}

/* Get an BUD so we can process all the deferred rmap updates. */
static struct xfs_log_item *
xfs_bmap_update_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Process a deferred rmap update. */
STATIC int
xfs_bmap_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_bmap_intent		*bi;
	int				error;

	bi = container_of(item, struct xfs_bmap_intent, bi_list);
	error = xfs_bmap_finish_one(tp, bi);
	if (!error && bi->bi_bmap.br_blockcount > 0) {
		ASSERT(bi->bi_type == XFS_BMAP_UNMAP);
		return -EAGAIN;
	}
	kmem_free(bi);
	return error;
}

/* Abort all pending BUIs. */
STATIC void
xfs_bmap_update_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a deferred rmap update. */
STATIC void
xfs_bmap_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_bmap_intent		*bi;

	bi = container_of(item, struct xfs_bmap_intent, bi_list);
	kmem_free(bi);
}

const struct xfs_defer_op_type xfs_bmap_update_defer_type = {
	.create_intent	= xfs_bmap_update_create_intent,
	.abort_intent	= xfs_bmap_update_abort_intent,
	.create_done	= xfs_bmap_update_create_done,
	.finish_item	= xfs_bmap_update_finish_item,
	.cancel_item	= xfs_bmap_update_cancel_item,
};

/* Atomic Swapping of File Ranges */

STATIC struct xfs_log_item *
xfs_swapext_create_intent(
	struct xfs_trans		*tp,
	struct list_head		*items,
	unsigned int			count,
	bool				sort)
{
	return NULL;
}

STATIC struct xfs_log_item *
xfs_swapext_create_done(
	struct xfs_trans		*tp,
	struct xfs_log_item		*intent,
	unsigned int			count)
{
	return NULL;
}

/* Process a deferred swapext update. */
STATIC int
xfs_swapext_finish_item(
	struct xfs_trans		*tp,
	struct xfs_log_item		*done,
	struct list_head		*item,
	struct xfs_btree_cur		**state)
{
	struct xfs_swapext_intent	*sxi;
	int				error;

	sxi = container_of(item, struct xfs_swapext_intent, sxi_list);

	/*
	 * Swap one more extent between the two files.  If there's still more
	 * work to do, we want to requeue ourselves after all other pending
	 * deferred operations have finished.  This includes all of the dfops
	 * that we queued directly as well as any new ones created in the
	 * process of finishing the others.  Doing so prevents us from queuing
	 * a large number of SXI log items in kernel memory, which in turn
	 * prevents us from pinning the tail of the log (while logging those
	 * new SXI items) until the first SXI items can be processed.
	 */
	error = xfs_swapext_finish_one(tp, sxi);
	if (error == -EAGAIN)
		return error;

	kmem_free(sxi);
	return error;
}

/* Abort all pending SXIs. */
STATIC void
xfs_swapext_abort_intent(
	struct xfs_log_item		*intent)
{
}

/* Cancel a deferred swapext update. */
STATIC void
xfs_swapext_cancel_item(
	struct list_head		*item)
{
	struct xfs_swapext_intent	*sxi;

	sxi = container_of(item, struct xfs_swapext_intent, sxi_list);
	kmem_free(sxi);
}

const struct xfs_defer_op_type xfs_swapext_defer_type = {
	.create_intent	= xfs_swapext_create_intent,
	.abort_intent	= xfs_swapext_abort_intent,
	.create_done	= xfs_swapext_create_done,
	.finish_item	= xfs_swapext_finish_item,
	.cancel_item	= xfs_swapext_cancel_item,
};
