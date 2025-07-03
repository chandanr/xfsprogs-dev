/*
 * Look up the inode number specified and if it is not already marked XFS_ISTALE
 * mark it stale. We should only find clean inodes in this lookup that aren't
 * already stale.
 */
static void
xfs_ifree_mark_inode_stale(
	struct xfs_perag	*pag,
	struct xfs_inode	*free_ip,
	xfs_ino_t		inum)
{
	struct xfs_mount	*mp = pag->pag_mount;
	struct xfs_inode_log_item *iip;
	struct xfs_inode	*ip;

retry:
	rcu_read_lock();
	ip = radix_tree_lookup(&pag->pag_ici_root, XFS_INO_TO_AGINO(mp, inum));

	/* Inode not in memory, nothing to do */
	if (!ip) {
		rcu_read_unlock();
		return;
	}

	/*
	 * because this is an RCU protected lookup, we could find a recently
	 * freed or even reallocated inode during the lookup. We need to check
	 * under the i_flags_lock for a valid inode here. Skip it if it is not
	 * valid, the wrong inode or stale.
	 */
	spin_lock(&ip->i_flags_lock);
	if (ip->i_ino != inum || __xfs_iflags_test(ip, XFS_ISTALE))
		goto out_iflags_unlock;

	/*
	 * Don't try to lock/unlock the current inode, but we _cannot_ skip the
	 * other inodes that we did not find in the list attached to the buffer
	 * and are not already marked stale. If we can't lock it, back off and
	 * retry.
	 */
	if (ip != free_ip) {
		if (!xfs_ilock_nowait(ip, XFS_ILOCK_EXCL)) {
			spin_unlock(&ip->i_flags_lock);
			rcu_read_unlock();
			delay(1);
			goto retry;
		}
	}
	ip->i_flags |= XFS_ISTALE;

	/*
	 * If the inode is flushing, it is already attached to the buffer.  All
	 * we needed to do here is mark the inode stale so buffer IO completion
	 * will remove it from the AIL.
	 */
	iip = ip->i_itemp;
	if (__xfs_iflags_test(ip, XFS_IFLUSHING)) {
		ASSERT(!list_empty(&iip->ili_item.li_bio_list));
		ASSERT(iip->ili_last_fields);
		goto out_iunlock;
	}

	/*
	 * Inodes not attached to the buffer can be released immediately.
	 * Everything else has to go through xfs_iflush_abort() on journal
	 * commit as the flock synchronises removal of the inode from the
	 * cluster buffer against inode reclaim.
	 */
	if (!iip || list_empty(&iip->ili_item.li_bio_list))
		goto out_iunlock;

	__xfs_iflags_set(ip, XFS_IFLUSHING);
	spin_unlock(&ip->i_flags_lock);
	rcu_read_unlock();

	/* we have a dirty inode in memory that has not yet been flushed. */
	spin_lock(&iip->ili_lock);
	iip->ili_last_fields = iip->ili_fields;
	iip->ili_fields = 0;
	iip->ili_fsync_fields = 0;
	spin_unlock(&iip->ili_lock);
	ASSERT(iip->ili_last_fields);

	if (ip != free_ip)
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return;

out_iunlock:
	if (ip != free_ip)
		xfs_iunlock(ip, XFS_ILOCK_EXCL);
out_iflags_unlock:
	spin_unlock(&ip->i_flags_lock);
	rcu_read_unlock();
}

/*
 * A big issue when freeing the inode cluster is that we _cannot_ skip any
 * inodes that are in memory - they all must be marked stale and attached to
 * the cluster buffer.
 */
static int
xfs_ifree_cluster(
	struct xfs_trans	*tp,
	struct xfs_perag	*pag,
	struct xfs_inode	*free_ip,
	struct xfs_icluster	*xic)
{
	struct xfs_mount	*mp = free_ip->i_mount;
	struct xfs_ino_geometry	*igeo = M_IGEO(mp);
	struct xfs_buf		*bp;
	xfs_daddr_t		blkno;
	xfs_ino_t		inum = xic->first_ino;
	int			nbufs;
	int			i, j;
	int			ioffset;
	int			error;

	nbufs = igeo->ialloc_blks / igeo->blocks_per_cluster;

	for (j = 0; j < nbufs; j++, inum += igeo->inodes_per_cluster) {
		/*
		 * The allocation bitmap tells us which inodes of the chunk were
		 * physically allocated. Skip the cluster if an inode falls into
		 * a sparse region.
		 */
		ioffset = inum - xic->first_ino;
		if ((xic->alloc & XFS_INOBT_MASK(ioffset)) == 0) {
			ASSERT(ioffset % igeo->inodes_per_cluster == 0);
			continue;
		}

		blkno = XFS_AGB_TO_DADDR(mp, XFS_INO_TO_AGNO(mp, inum),
					 XFS_INO_TO_AGBNO(mp, inum));

		/*
		 * We obtain and lock the backing buffer first in the process
		 * here to ensure dirty inodes attached to the buffer remain in
		 * the flushing state while we mark them stale.
		 *
		 * If we scan the in-memory inodes first, then buffer IO can
		 * complete before we get a lock on it, and hence we may fail
		 * to mark all the active inodes on the buffer stale.
		 */
		error = xfs_trans_get_buf(tp, mp->m_ddev_targp, blkno,
				mp->m_bsize * igeo->blocks_per_cluster,
				XBF_UNMAPPED, &bp);
		if (error)
			return error;

		/*
		 * This buffer may not have been correctly initialised as we
		 * didn't read it from disk. That's not important because we are
		 * only using to mark the buffer as stale in the log, and to
		 * attach stale cached inodes on it. That means it will never be
		 * dispatched for IO. If it is, we want to know about it, and we
		 * want it to fail. We can acheive this by adding a write
		 * verifier to the buffer.
		 */
		bp->b_ops = &xfs_inode_buf_ops;

		/*
		 * Now we need to set all the cached clean inodes as XFS_ISTALE,
		 * too. This requires lookups, and will skip inodes that we've
		 * already marked XFS_ISTALE.
		 */
		for (i = 0; i < igeo->inodes_per_cluster; i++)
			xfs_ifree_mark_inode_stale(pag, free_ip, inum + i);

		xfs_trans_stale_inode_buf(tp, bp);
		xfs_trans_binval(tp, bp);
	}
	return 0;
}

/*
 * Point the AGI unlinked bucket at an inode and log the results.  The caller
 * is responsible for validating the old value.
 */
STATIC int
xfs_iunlink_update_bucket(
	struct xfs_trans	*tp,
	struct xfs_perag	*pag,
	struct xfs_buf		*agibp,
	unsigned int		bucket_index,
	xfs_agino_t		new_agino)
{
	struct xfs_agi		*agi = agibp->b_addr;
	xfs_agino_t		old_value;
	int			offset;

	ASSERT(xfs_verify_agino_or_null(pag, new_agino));

	old_value = be32_to_cpu(agi->agi_unlinked[bucket_index]);
	trace_xfs_iunlink_update_bucket(tp->t_mountp, pag->pag_agno, bucket_index,
			old_value, new_agino);

	/*
	 * We should never find the head of the list already set to the value
	 * passed in because either we're adding or removing ourselves from the
	 * head of the list.
	 */
	if (old_value == new_agino) {
		xfs_buf_mark_corrupt(agibp);
		return -EFSCORRUPTED;
	}

	agi->agi_unlinked[bucket_index] = cpu_to_be32(new_agino);
	offset = offsetof(struct xfs_agi, agi_unlinked) +
			(sizeof(xfs_agino_t) * bucket_index);
	xfs_trans_log_buf(tp, agibp, offset, offset + sizeof(xfs_agino_t) - 1);
	return 0;
}

/*
 * Load the inode @next_agino into the cache and set its prev_unlinked pointer
 * to @prev_agino.  Caller must hold the AGI to synchronize with other changes
 * to the unlinked list.
 */
STATIC int
xfs_iunlink_reload_next(
	struct xfs_trans	*tp,
	struct xfs_buf		*agibp,
	xfs_agino_t		prev_agino,
	xfs_agino_t		next_agino)
{
	struct xfs_perag	*pag = agibp->b_pag;
	struct xfs_mount	*mp = pag->pag_mount;
	struct xfs_inode	*next_ip = NULL;
	xfs_ino_t		ino;
	int			error;

	ASSERT(next_agino != NULLAGINO);

#ifdef DEBUG
	rcu_read_lock();
	next_ip = radix_tree_lookup(&pag->pag_ici_root, next_agino);
	ASSERT(next_ip == NULL);
	rcu_read_unlock();
#endif

	xfs_info_ratelimited(mp,
 "Found unrecovered unlinked inode 0x%x in AG 0x%x.  Initiating recovery.",
			next_agino, pag->pag_agno);

	/*
	 * Use an untrusted lookup just to be cautious in case the AGI has been
	 * corrupted and now points at a free inode.  That shouldn't happen,
	 * but we'd rather shut down now since we're already running in a weird
	 * situation.
	 */
	ino = XFS_AGINO_TO_INO(mp, pag->pag_agno, next_agino);
	error = xfs_iget(mp, tp, ino, XFS_IGET_UNTRUSTED, 0, &next_ip);
	if (error)
		return error;

	/* If this is not an unlinked inode, something is very wrong. */
	if (VFS_I(next_ip)->i_nlink != 0) {
		error = -EFSCORRUPTED;
		goto rele;
	}

	next_ip->i_prev_unlinked = prev_agino;
	trace_xfs_iunlink_reload_next(next_ip);
rele:
	ASSERT(!(VFS_I(next_ip)->i_state & I_DONTCACHE));
	if (xfs_is_quotacheck_running(mp) && next_ip)
		xfs_iflags_set(next_ip, XFS_IQUOTAUNCHECKED);
	xfs_irele(next_ip);
	return error;
}

/*
 * Find an inode on the unlinked list. This does not take references to the
 * inode as we have existence guarantees by holding the AGI buffer lock and that
 * only unlinked, referenced inodes can be on the unlinked inode list.  If we
 * don't find the inode in cache, then let the caller handle the situation.
 */
static struct xfs_inode *
xfs_iunlink_lookup(
	struct xfs_perag	*pag,
	xfs_agino_t		agino)
{
	struct xfs_inode	*ip;

	rcu_read_lock();
	ip = radix_tree_lookup(&pag->pag_ici_root, agino);
	if (!ip) {
		/* Caller can handle inode not being in memory. */
		rcu_read_unlock();
		return NULL;
	}

	/*
	 * Inode in RCU freeing limbo should not happen.  Warn about this and
	 * let the caller handle the failure.
	 */
	if (WARN_ON_ONCE(!ip->i_ino)) {
		rcu_read_unlock();
		return NULL;
	}
	ASSERT(!xfs_iflags_test(ip, XFS_IRECLAIMABLE | XFS_IRECLAIM));
	rcu_read_unlock();
	return ip;
}

/*
 * Update the prev pointer of the next agino.  Returns -ENOLINK if the inode
 * is not in cache.
 */
static int
xfs_iunlink_update_backref(
	struct xfs_perag	*pag,
	xfs_agino_t		prev_agino,
	xfs_agino_t		next_agino)
{
	struct xfs_inode	*ip;

	/* No update necessary if we are at the end of the list. */
	if (next_agino == NULLAGINO)
		return 0;

	ip = xfs_iunlink_lookup(pag, next_agino);
	if (!ip)
		return -ENOLINK;

	ip->i_prev_unlinked = prev_agino;
	return 0;
}

static int
xfs_iunlink_remove_inode(
	struct xfs_trans	*tp,
	struct xfs_perag	*pag,
	struct xfs_buf		*agibp,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_agi		*agi = agibp->b_addr;
	xfs_agino_t		agino = XFS_INO_TO_AGINO(mp, ip->i_ino);
	xfs_agino_t		head_agino;
	short			bucket_index = agino % XFS_AGI_UNLINKED_BUCKETS;
	int			error;

	trace_xfs_iunlink_remove(ip);

	/*
	 * Get the index into the agi hash table for the list this inode will
	 * go on.  Make sure the head pointer isn't garbage.
	 */
	head_agino = be32_to_cpu(agi->agi_unlinked[bucket_index]);
	if (!xfs_verify_agino(pag, head_agino)) {
		XFS_CORRUPTION_ERROR(__func__, XFS_ERRLEVEL_LOW, mp,
				agi, sizeof(*agi));
		return -EFSCORRUPTED;
	}

	/*
	 * Set our inode's next_unlinked pointer to NULL and then return
	 * the old pointer value so that we can update whatever was previous
	 * to us in the list to point to whatever was next in the list.
	 */
	error = xfs_iunlink_log_inode(tp, ip, pag, NULLAGINO);
	if (error)
		return error;

	/*
	 * Update the prev pointer in the next inode to point back to previous
	 * inode in the chain.
	 */
	error = xfs_iunlink_update_backref(pag, ip->i_prev_unlinked,
			ip->i_next_unlinked);
	if (error == -ENOLINK)
		error = xfs_iunlink_reload_next(tp, agibp, ip->i_prev_unlinked,
				ip->i_next_unlinked);
	if (error)
		return error;

	if (head_agino != agino) {
		struct xfs_inode	*prev_ip;

		prev_ip = xfs_iunlink_lookup(pag, ip->i_prev_unlinked);
		if (!prev_ip)
			return -EFSCORRUPTED;

		error = xfs_iunlink_log_inode(tp, prev_ip, pag,
				ip->i_next_unlinked);
		prev_ip->i_next_unlinked = ip->i_next_unlinked;
	} else {
		/* Point the head of the list to the next unlinked inode. */
		error = xfs_iunlink_update_bucket(tp, pag, agibp, bucket_index,
				ip->i_next_unlinked);
	}

	ip->i_next_unlinked = NULLAGINO;
	ip->i_prev_unlinked = 0;
	return error;
}

/*
 * Pull the on-disk inode from the AGI unlinked list.
 */
STATIC int
xfs_iunlink_remove(
	struct xfs_trans	*tp,
	struct xfs_perag	*pag,
	struct xfs_inode	*ip)
{
	struct xfs_buf		*agibp;
	int			error;

	trace_xfs_iunlink_remove(ip);

	/* Get the agi buffer first.  It ensures lock ordering on the list. */
	error = xfs_read_agi(pag, tp, &agibp);
	if (error)
		return error;

	return xfs_iunlink_remove_inode(tp, pag, agibp, ip);
}

/*
 * This is called to return an inode to the inode free list.  The inode should
 * already be truncated to 0 length and have no pages associated with it.  This
 * routine also assumes that the inode is already a part of the transaction.
 *
 * The on-disk copy of the inode will have been added to the list of unlinked
 * inodes in the AGI. We need to remove the inode from that list atomically with
 * respect to freeing it here.
 */
int
xfs_ifree(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_perag	*pag;
	struct xfs_icluster	xic = { 0 };
	struct xfs_inode_log_item *iip = ip->i_itemp;
	int			error;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(VFS_I(ip)->i_nlink == 0);
	ASSERT(ip->i_df.if_nextents == 0);
	ASSERT(ip->i_disk_size == 0 || !S_ISREG(VFS_I(ip)->i_mode));
	ASSERT(ip->i_nblocks == 0);

	pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ip->i_ino));

	/*
	 * Free the inode first so that we guarantee that the AGI lock is going
	 * to be taken before we remove the inode from the unlinked list. This
	 * makes the AGI lock -> unlinked list modification order the same as
	 * used in O_TMPFILE creation.
	 */
	error = xfs_difree(tp, pag, ip->i_ino, &xic);
	if (error)
		goto out;

	error = xfs_iunlink_remove(tp, pag, ip);
	if (error)
		goto out;

	/*
	 * Free any local-format data sitting around before we reset the
	 * data fork to extents format.  Note that the attr fork data has
	 * already been freed by xfs_attr_inactive.
	 */
	if (ip->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		kmem_free(ip->i_df.if_u1.if_data);
		ip->i_df.if_u1.if_data = NULL;
		ip->i_df.if_bytes = 0;
	}

	VFS_I(ip)->i_mode = 0;		/* mark incore inode as free */
	ip->i_diflags = 0;
	ip->i_diflags2 = mp->m_ino_geo.new_diflags2;
	ip->i_forkoff = 0;		/* mark the attr fork not in use */
	ip->i_df.if_format = XFS_DINODE_FMT_EXTENTS;
	if (xfs_iflags_test(ip, XFS_IPRESERVE_DM_FIELDS))
		xfs_iflags_clear(ip, XFS_IPRESERVE_DM_FIELDS);

	/* Don't attempt to replay owner changes for a deleted inode */
	spin_lock(&iip->ili_lock);
	iip->ili_fields &= ~(XFS_ILOG_AOWNER | XFS_ILOG_DOWNER);
	spin_unlock(&iip->ili_lock);

	/*
	 * Bump the generation count so no one will be confused
	 * by reincarnations of this inode.
	 */
	VFS_I(ip)->i_generation++;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	if (xic.deleted)
		error = xfs_ifree_cluster(tp, pag, ip, &xic);
out:
	xfs_perag_put(pag);
	return error;
}

/*
 * xfs_inactive_ifree()
 *
 * Perform the inode free when an inode is unlinked.
 */
STATIC int
xfs_inactive_ifree(
	struct xfs_inode *ip)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	int			error;

	/*
	 * We try to use a per-AG reservation for any block needed by the finobt
	 * tree, but as the finobt feature predates the per-AG reservation
	 * support a degraded file system might not have enough space for the
	 * reservation at mount time.  In that case try to dip into the reserved
	 * pool and pray.
	 *
	 * Send a warning if the reservation does happen to fail, as the inode
	 * now remains allocated and sits on the unlinked list until the fs is
	 * repaired.
	 */

	/* chandan: TODO: Check the meaning of m_finobt_nores later */
#if 0
	if (unlikely(mp->m_finobt_nores)) {
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_ifree,
				XFS_IFREE_SPACE_RES(mp), 0, XFS_TRANS_RESERVE,
				&tp);
	} else {
#endif
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_ifree, 0, 0, 0, &tp);
#if 0
	}
#endif
	if (error) {
		if (error == -ENOSPC) {
			xfs_warn_ratelimited(mp,
			"Failed to remove inode(s) from unlinked list. "
			"Please free space, unmount and run xfs_repair.");
		} else {
			ASSERT(xfs_is_shutdown(mp));
		}
		return error;
	}

	/*
	 * We do not hold the inode locked across the entire rolling transaction
	 * here. We only need to hold it for the first transaction that
	 * xfs_ifree() builds, which may mark the inode XFS_ISTALE if the
	 * underlying cluster buffer is freed. Relogging an XFS_ISTALE inode
	 * here breaks the relationship between cluster buffer invalidation and
	 * stale inode invalidation on cluster buffer item journal commit
	 * completion, and can result in leaving dirty stale inodes hanging
	 * around in memory.
	 *
	 * We have no need for serialising this inode operation against other
	 * operations - we freed the inode and hence reallocation is required
	 * and that will serialise on reallocating the space the deferops need
	 * to free. Hence we can unlock the inode on the first commit of
	 * the transaction rather than roll it right through the deferops. This
	 * avoids relogging the XFS_ISTALE inode.
	 *
	 * We check that xfs_ifree() hasn't grown an internal transaction roll
	 * by asserting that the inode is still locked when it returns.
	 */
	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, XFS_ILOCK_EXCL);

	error = xfs_ifree(tp, ip);
	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	if (error) {
		/*
		 * If we fail to free the inode, shut down.  The cancel
		 * might do that, we need to make sure.  Otherwise the
		 * inode might be lost for a long time or forever.
		 */
		if (!xfs_is_shutdown(mp)) {
			xfs_notice(mp, "%s: xfs_ifree returned error %d",
				__func__, error);
			xfs_force_shutdown(mp, SHUTDOWN_META_IO_ERROR);
		}
		xfs_trans_cancel(tp);
		return error;
	}

#if 0
	/*
	 * Credit the quota account(s). The inode is gone.
	 */
	xfs_trans_mod_dquot_byino(tp, ip, XFS_TRANS_DQ_ICOUNT, -1);
#endif

	return xfs_trans_commit(tp);
}


/*
 * Free up the underlying blocks past new_size.  The new size must be smaller
 * than the current size.  This routine can be used both for the attribute and
 * data fork, and does not modify the inode size, which is left to the caller.
 *
 * The transaction passed to this routine must have made a permanent log
 * reservation of at least XFS_ITRUNCATE_LOG_RES.  This routine may commit the
 * given transaction and start new ones, so make sure everything involved in
 * the transaction is tidy before calling here.  Some transaction will be
 * returned to the caller to be committed.  The incoming transaction must
 * already include the inode, and both inode locks must be held exclusively.
 * The inode must also be "held" within the transaction.  On return the inode
 * will be "held" within the returned transaction.  This routine does NOT
 * require any disk space to be reserved for it within the transaction.
 *
 * If we get an error, we must return with the inode locked and linked into the
 * current transaction. This keeps things simple for the higher level code,
 * because it always knows that the inode is locked and held in the transaction
 * that returns to it whether errors occur or not.  We don't mark the inode
 * dirty on error so that transactions can be easily aborted if possible.
 */
int
xfs_itruncate_extents_flags(
	struct xfs_trans	**tpp,
	struct xfs_inode	*ip,
	int			whichfork,
	xfs_fsize_t		new_size,
	int			flags)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp = *tpp;
	xfs_fileoff_t		first_unmap_block;
	xfs_filblks_t		unmap_len;
	int			error = 0;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(!atomic_read(&VFS_I(ip)->i_count) ||
	       xfs_isilocked(ip, XFS_IOLOCK_EXCL));
	ASSERT(new_size <= XFS_ISIZE(ip));
	ASSERT(tp->t_flags & XFS_TRANS_PERM_LOG_RES);
	ASSERT(ip->i_itemp != NULL);
	ASSERT(ip->i_itemp->ili_lock_flags == 0);
	ASSERT(!XFS_NOT_DQATTACHED(mp, ip));

	trace_xfs_itruncate_extents_start(ip, new_size);

	flags |= xfs_bmapi_aflag(whichfork);

	/*
	 * Since it is possible for space to become allocated beyond
	 * the end of the file (in a crash where the space is allocated
	 * but the inode size is not yet updated), simply remove any
	 * blocks which show up between the new EOF and the maximum
	 * possible file size.
	 *
	 * We have to free all the blocks to the bmbt maximum offset, even if
	 * the page cache can't scale that far.
	 */
	first_unmap_block = XFS_B_TO_FSB(mp, (xfs_ufsize_t)new_size);
	if (!xfs_verify_fileoff(mp, first_unmap_block)) {
		WARN_ON_ONCE(first_unmap_block > XFS_MAX_FILEOFF);
		return 0;
	}

	unmap_len = XFS_MAX_FILEOFF - first_unmap_block + 1;
	while (unmap_len > 0) {
		ASSERT(tp->t_highest_agno == NULLAGNUMBER);
		error = __xfs_bunmapi(tp, ip, first_unmap_block, &unmap_len,
				flags, XFS_ITRUNC_MAX_EXTENTS);
		if (error)
			goto out;

		/* free the just unmapped extents */
		error = xfs_defer_finish(&tp);
		if (error)
			goto out;
	}

	if (whichfork == XFS_DATA_FORK) {
		/* Remove all pending CoW reservations. */
		error = xfs_reflink_cancel_cow_blocks(ip, &tp,
				first_unmap_block, XFS_MAX_FILEOFF, true);
		if (error)
			goto out;

		xfs_itruncate_clear_reflink_flags(ip);
	}

	/*
	 * Always re-log the inode so that our permanent transaction can keep
	 * on rolling it forward in the log.
	 */
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	trace_xfs_itruncate_extents_end(ip, new_size);

out:
	*tpp = tp;
	return error;
}

/*
 * xfs_inactive_truncate
 *
 * Called to perform a truncate when an inode becomes unlinked.
 */
STATIC int
xfs_inactive_truncate(
	struct xfs_inode *ip)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	int			error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp);
	if (error) {
		ASSERT(xfs_is_shutdown(mp));
		return error;
	}
	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	/*
	 * Log the inode size first to prevent stale data exposure in the event
	 * of a system crash before the truncate completes. See the related
	 * comment in xfs_vn_setattr_size() for details.
	 */
	ip->i_disk_size = 0;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	error = xfs_itruncate_extents(&tp, ip, XFS_DATA_FORK, 0);
	if (error)
		goto error_trans_cancel;

	ASSERT(ip->i_df.if_nextents == 0);

	error = xfs_trans_commit(tp);
	if (error)
		goto error_unlock;

	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return 0;

error_trans_cancel:
	xfs_trans_cancel(tp);
error_unlock:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

int
xfs_inactive(
	xfs_inode_t	*ip)
{
	struct xfs_mount	*mp;
	int			error = 0;
	int			truncate = 0;

	/*
	 * If the inode is already free, then there can be nothing
	 * to clean up here.
	 */
	if (VFS_I(ip)->i_mode == 0) {
		ASSERT(ip->i_df.if_broot_bytes == 0);
		goto out;
	}

	mp = ip->i_mount;
	ASSERT(!xfs_iflags_test(ip, XFS_IRECOVERY));

	/* Metadata inodes require explicit resource cleanup. */
	if (xfs_is_metadata_inode(ip))
		goto out;

	/* Try to clean out the cow blocks if there are any. */
	if (xfs_inode_has_cow_data(ip))
		ASSERT(0);

	if (VFS_I(ip)->i_nlink != 0)
		ASSERT(0);

	if (S_ISREG(VFS_I(ip)->i_mode) &&
	    (ip->i_disk_size != 0 ||
	     ip->i_df.if_nextents > 0))
		truncate = 1;

	/* chandan: TODO: check quota stuff later */
	if (xfs_iflags_test(ip, XFS_IQUOTAUNCHECKED)) {
		/*
		 * If this inode is being inactivated during a quotacheck and
		 * has not yet been scanned by quotacheck, we /must/ remove
		 * the dquots from the inode before inactivation changes the
		 * block and inode counts.  Most probably this is a result of
		 * reloading the incore iunlinked list to purge unrecovered
		 * unlinked inodes.
		 */
		xfs_qm_dqdetach(ip);
	} else {
		error = xfs_qm_dqattach(ip);
		if (error)
			goto out;
	}

	if (S_ISLNK(VFS_I(ip)->i_mode)) {
		/* chandan: TODO: Get to symlink inactivation later. */
		ASSERT(0);
		error = xfs_inactive_symlink(ip);
	} else if (truncate) {
		error = xfs_inactive_truncate(ip);
	}
	if (error)
		goto out;

	/*
	 * If there are attributes associated with the file then blow them away
	 * now.  The code calls a routine that recursively deconstructs the
	 * attribute fork. If also blows away the in-core attribute fork.
	 */
	if (xfs_inode_has_attr_fork(ip)) {
		/* chandan: TODO: Get to attr fork inactivation later. */
		ASSERT(0);
		error = xfs_attr_inactive(ip);
		if (error)
			goto out;
	}

	ASSERT(ip->i_forkoff == 0);

	/*
	 * Free the inode.
	 */
	error = xfs_inactive_ifree(ip);

out:
	/*
	 * We're done making metadata updates for this inode, so we can release
	 * the attached dquots.
	 */
	xfs_qm_dqdetach(ip);
	return error;
}
