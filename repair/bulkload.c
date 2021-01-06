// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <libxfs.h>
#include "bulkload.h"

int bload_leaf_slack = -1;
int bload_node_slack = -1;

/* Initialize accounting resources for staging a new AG btree. */
void
bulkload_init_ag(
	struct bulkload			*bkl,
	struct repair_ctx		*sc,
	const struct xfs_owner_info	*oinfo,
	xfs_fsblock_t			alloc_hint)
{
	memset(bkl, 0, sizeof(struct bulkload));
	bkl->sc = sc;
	bkl->oinfo = *oinfo; /* structure copy */
	bkl->alloc_hint = alloc_hint;
	INIT_LIST_HEAD(&bkl->resv_list);
}

/* Initialize accounting resources for staging a new inode fork btree. */
void
bulkload_init_inode(
	struct bulkload			*bkl,
	struct repair_ctx		*sc,
	int				whichfork,
	const struct xfs_owner_info	*oinfo)
{
	bulkload_init_ag(bkl, sc, oinfo, XFS_INO_TO_FSB(sc->mp, sc->ip->i_ino));
	bkl->ifake.if_fork = kmem_cache_zalloc(xfs_ifork_zone, 0);
	bkl->ifake.if_fork_size = XFS_IFORK_SIZE(sc->ip, whichfork);
}

/* Designate specific blocks to be used to build our new btree. */
int
bulkload_add_blocks(
	struct bulkload		*bkl,
	xfs_fsblock_t		fsbno,
	xfs_extlen_t		len)
{
	struct bulkload_resv	*resv;

	resv = kmem_alloc(sizeof(struct bulkload_resv), KM_MAYFAIL);
	if (!resv)
		return ENOMEM;

	INIT_LIST_HEAD(&resv->list);
	resv->fsbno = fsbno;
	resv->len = len;
	resv->used = 0;
	list_add_tail(&resv->list, &bkl->resv_list);
	bkl->nr_reserved += len;

	return 0;
}

/* Reserve disk space for our new btree. */
int
bulkload_alloc_blocks(
	struct bulkload		*bkl,
	uint64_t		nr_blocks)
{
	struct repair_ctx	*sc = bkl->sc;
	xfs_alloctype_t		type;
	int			error = 0;

	type = sc->ip ? XFS_ALLOCTYPE_START_BNO : XFS_ALLOCTYPE_NEAR_BNO;

	while (nr_blocks > 0) {
		struct xfs_alloc_arg	args = {
			.tp		= sc->tp,
			.mp		= sc->mp,
			.type		= type,
			.fsbno		= bkl->alloc_hint,
			.oinfo		= bkl->oinfo,
			.minlen		= 1,
			.maxlen		= nr_blocks,
			.prod		= 1,
			.resv		= XFS_AG_RESV_NONE,
		};

		error = -libxfs_alloc_vextent(&args);
		if (error)
			return error;
		if (args.fsbno == NULLFSBLOCK)
			return ENOSPC;

		error = bulkload_add_blocks(bkl, args.fsbno, args.len);
		if (error)
			return error;

		nr_blocks -= args.len;

		error = -libxfs_trans_roll_inode(&sc->tp, sc->ip);
		if (error)
			return error;
	}

	return 0;
}

/*
 * Release blocks that were reserved for a btree repair.  If the repair
 * succeeded then we log deferred frees for unused blocks.  Otherwise, we try
 * to free the extents immediately to roll the filesystem back to where it was
 * before we started.
 */
static inline int
bulkload_destroy_reservation(
	struct bulkload		*bkl,
	struct bulkload_resv	*resv,
	bool			cancel_repair)
{
	struct repair_ctx	*sc = bkl->sc;

	if (cancel_repair) {
		int		error;

		/* Free the extent then roll the transaction. */
		error = -libxfs_free_extent(sc->tp, resv->fsbno, resv->len,
				&bkl->oinfo, XFS_AG_RESV_NONE);
		if (error)
			return error;

		return -libxfs_trans_roll_inode(&sc->tp, sc->ip);
	}

	/*
	 * Use the deferred freeing mechanism to schedule for deletion any
	 * blocks we didn't use to rebuild the tree.  This enables us to log
	 * them all in the same transaction as the root change.
	 */
	resv->fsbno += resv->used;
	resv->len -= resv->used;
	resv->used = 0;

	if (resv->len == 0)
		return 0;

	__xfs_bmap_add_free(sc->tp, resv->fsbno, resv->len, &bkl->oinfo, true);

	return 0;
}

/* Free all the accounting info and disk space we reserved for a new btree. */
void
bulkload_destroy(
	struct bulkload		*bkl,
	int			error)
{
	struct repair_ctx	*sc = bkl->sc;
	struct bulkload_resv	*resv, *n;
	int			err2;

	list_for_each_entry_safe(resv, n, &bkl->resv_list, list) {
		err2 = bulkload_destroy_reservation(bkl, resv, error != 0);
		if (err2)
			goto junkit;

		list_del(&resv->list);
		kmem_free(resv);
	}

junkit:
	/*
	 * If we still have reservations attached to @newbt, cleanup must have
	 * failed and the filesystem is about to go down.  Clean up the incore
	 * reservations.
	 */
	list_for_each_entry_safe(resv, n, &bkl->resv_list, list) {
		list_del(&resv->list);
		kmem_free(resv);
	}

	if (sc->ip) {
		kmem_cache_free(xfs_ifork_zone, bkl->ifake.if_fork);
		bkl->ifake.if_fork = NULL;
	}
}

/* Feed one of the reserved btree blocks to the bulk loader. */
int
bulkload_claim_block(
	struct xfs_btree_cur	*cur,
	struct bulkload		*bkl,
	union xfs_btree_ptr	*ptr)
{
	struct bulkload_resv	*resv;
	xfs_fsblock_t		fsb;

	/*
	 * The first item in the list should always have a free block unless
	 * we're completely out.
	 */
	resv = list_first_entry(&bkl->resv_list, struct bulkload_resv, list);
	if (resv->used == resv->len)
		return ENOSPC;

	/*
	 * Peel off a block from the start of the reservation.  We allocate
	 * blocks in order to place blocks on disk in increasing record or key
	 * order.  The block reservations tend to end up on the list in
	 * decreasing order, which hopefully results in leaf blocks ending up
	 * together.
	 */
	fsb = resv->fsbno + resv->used;
	resv->used++;

	/* If we used all the blocks in this reservation, move it to the end. */
	if (resv->used == resv->len)
		list_move_tail(&resv->list, &bkl->resv_list);

	if (cur->bc_flags & XFS_BTREE_LONG_PTRS)
		ptr->l = cpu_to_be64(fsb);
	else
		ptr->s = cpu_to_be32(XFS_FSB_TO_AGBNO(cur->bc_mp, fsb));
	return 0;
}

/*
 * Estimate proper slack values for a btree that's being reloaded.
 *
 * Under most circumstances, we'll take whatever default loading value the
 * btree bulk loading code calculates for us.  However, there are some
 * exceptions to this rule:
 *
 * (1) If someone turned one of the debug knobs.
 * (2) The AG has less than ~9% space free.
 *
 * Note that we actually use 3/32 for the comparison to avoid division.
 */
void
bulkload_estimate_ag_slack(
	struct repair_ctx	*sc,
	struct xfs_btree_bload	*bload,
	unsigned int		free)
{
	/*
	 * The global values are set to -1 (i.e. take the bload defaults)
	 * unless someone has set them otherwise, so we just pull the values
	 * here.
	 */
	bload->leaf_slack = bload_leaf_slack;
	bload->node_slack = bload_node_slack;

	/* No further changes if there's more than 3/32ths space left. */
	if (free >= ((sc->mp->m_sb.sb_agblocks * 3) >> 5))
		return;

	/*
	 * We're low on space; load the btrees as tightly as possible.  Leave
	 * a couple of open slots in each btree block so that we don't end up
	 * splitting the btrees like crazy right after mount.
	 */
	if (bload->leaf_slack < 0)
		bload->leaf_slack = 2;
	if (bload->node_slack < 0)
		bload->node_slack = 2;
}

/*
 * Estimate proper slack values for a btree that's being reloaded.
 *
 * Under most circumstances, we'll take whatever default loading value the
 * btree bulk loading code calculates for us.  However, there are some
 * exceptions to this rule:
 *
 * (1) If someone turned one of the debug knobs.
 * (2) The FS has less than ~9% space free.
 *
 * Note that we actually use 3/32 for the comparison to avoid division.
 */
void
bulkload_estimate_inode_slack(
	struct xfs_mount	*mp,
	struct xfs_btree_bload	*bload)
{
	/*
	 * The global values are set to -1 (i.e. take the bload defaults)
	 * unless someone has set them otherwise, so we just pull the values
	 * here.
	 */
	bload->leaf_slack = bload_leaf_slack;
	bload->node_slack = bload_node_slack;

	/* No further changes if there's more than 3/32ths space left. */
	if (mp->m_sb.sb_fdblocks >= ((mp->m_sb.sb_dblocks * 3) >> 5))
		return;

	/*
	 * We're low on space; load the btrees as tightly as possible.  Leave
	 * a couple of open slots in each btree block so that we don't end up
	 * splitting the btrees like crazy right after mount.
	 */
	if (bload->leaf_slack < 0)
		bload->leaf_slack = 2;
	if (bload->node_slack < 0)
		bload->node_slack = 2;
}
