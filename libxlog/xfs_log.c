#include "libxfs.h"
#include "libxlog.h"
#include "xfs_log_priv.h"

/*
 * Free a used ticket when its refcount falls to zero.
 */
void
xfs_log_ticket_put(
	xlog_ticket_t	*ticket)
{
	ASSERT(atomic_read(&ticket->t_ref) > 0);
	if (atomic_dec_and_test(&ticket->t_ref))
		kmem_cache_free(xfs_log_ticket_cache, ticket);
}

void
xlog_cil_destroy(
	struct xlog	*log)
{
	struct xfs_cil	*cil = log->l_cilp;

	if (cil->xc_ctx) {
		if (cil->xc_ctx->ticket)
			xfs_log_ticket_put(cil->xc_ctx->ticket);
		kmem_free(cil->xc_ctx);
	}

	ASSERT(test_bit(XLOG_CIL_EMPTY, &cil->xc_flags));
	free_percpu(cil->xc_pcp);
	destroy_workqueue(cil->xc_push_wq);
	kmem_free(cil);
}

/*
 * Deallocate a log structure
 */
STATIC void
xlog_dealloc_log(
	struct xlog	*log)
{
	xlog_in_core_t	*iclog, *next_iclog;
	int		i;

	/*
	 * Destroy the CIL after waiting for iclog IO completion because an
	 * iclog EIO error will try to shut down the log, which accesses the
	 * CIL to wake up the waiters.
	 */
	xlog_cil_destroy(log);

	iclog = log->l_iclog;
	for (i = 0; i < log->l_iclog_bufs; i++) {
		next_iclog = iclog->ic_next;
		kmem_free(iclog->ic_data);
		kmem_free(iclog);
		iclog = next_iclog;
	}

	log->l_mp->m_log = NULL;
	destroy_workqueue(log->l_ioend_workqueue);
	kmem_free(log);
}

/*
 * Mount a log filesystem
 *
 * mp		- ubiquitous xfs mount point structure
 * log_target	- buftarg of on-disk log device
 * blk_offset	- Start block # where block size is 512 bytes (BBSIZE)
 * num_bblocks	- Number of BBSIZE blocks in on-disk log
 *
 * Return error or zero.
 */
int
xfs_log_mount(
	struct xfs_mount	*mp,
	struct xfs_buftarg	*log_target,
	xfs_daddr_t		blk_offset,
	int			num_bblks)
{
	struct xlog	*log;
	int		error = 0;
	int		min_logfsbs;

	if (!xfs_has_norecovery(mp)) {
		xfs_notice(mp, "Mounting V%d Filesystem %pU",
			   XFS_SB_VERSION_NUM(&mp->m_sb),
			   &mp->m_sb.sb_uuid);
	} else {
		xfs_notice(mp,
"Mounting V%d filesystem %pU in no-recovery mode. Filesystem will be inconsistent.",
			   XFS_SB_VERSION_NUM(&mp->m_sb),
			   &mp->m_sb.sb_uuid);
		ASSERT(xfs_is_readonly(mp));
	}

	log = xlog_alloc_log(mp, log_target, blk_offset, num_bblks);
	if (IS_ERR(log)) {
		error = PTR_ERR(log);
		goto out;
	}
	mp->m_log = log;

	/*
	 * Now that we have set up the log and it's internal geometry
	 * parameters, we can validate the given log space and drop a critical
	 * message via syslog if the log size is too small. A log that is too
	 * small can lead to unexpected situations in transaction log space
	 * reservation stage. The superblock verifier has already validated all
	 * the other log geometry constraints, so we don't have to check those
	 * here.
	 *
	 * Note: For v4 filesystems, we can't just reject the mount if the
	 * validation fails.  This would mean that people would have to
	 * downgrade their kernel just to remedy the situation as there is no
	 * way to grow the log (short of black magic surgery with xfs_db).
	 *
	 * We can, however, reject mounts for V5 format filesystems, as the
	 * mkfs binary being used to make the filesystem should never create a
	 * filesystem with a log that is too small.
	 */
	min_logfsbs = libxfs_log_calc_minimum_size(mp);
	if (mp->m_sb.sb_logblocks < min_logfsbs) {
		xfs_warn(mp,
		"Log size %d blocks too small, minimum size is %d blocks",
			 mp->m_sb.sb_logblocks, min_logfsbs);

		/*
		 * Log check errors are always fatal on v5; or whenever bad
		 * metadata leads to a crash.
		 */
		if (xfs_has_crc(mp)) {
			xfs_crit(mp, "AAIEEE! Log failed size checks. Abort!");
			ASSERT(0);
			error = -EINVAL;
			goto out_free_log;
		}
		xfs_crit(mp, "Log size out of supported range.");
		xfs_crit(mp,
"Continuing onwards, but if log hangs are experienced then please report this message in the bug report.");
	}

	/*
	 * Initialize the AIL now we have a log.
	 */
	error = xfs_trans_ail_init(mp);
	if (error) {
		xfs_warn(mp, "AIL initialisation failed: error %d", error);
		goto out_free_log;
	}
	log->l_ailp = mp->m_ail;

	/*
	 * skip log recovery on a norecovery mount.  pretend it all
	 * just worked.
	 */
	if (!xfs_has_norecovery(mp)) {
		error = xlog_recover(log);
		if (error) {
			xfs_warn(mp, "log mount/recovery failed: error %d",
				error);
			xlog_recover_cancel(log);
			goto out_destroy_ail;
		}
	}

	error = xfs_sysfs_init(&log->l_kobj, &xfs_log_ktype, &mp->m_kobj,
			       "log"); /* chandan: check this later */
	if (error)
		goto out_destroy_ail;

	/* Normal transactions can now occur */
	clear_bit(XLOG_ACTIVE_RECOVERY, &log->l_opstate);

	/*
	 * Now the log has been fully initialised and we know were our
	 * space grant counters are, we can initialise the permanent ticket
	 * needed for delayed logging to work.
	 */
	xlog_cil_init_post_recovery(log);

	return 0;

out_destroy_ail:
	xfs_trans_ail_destroy(mp);
out_free_log:
	xlog_dealloc_log(log);
out:
	return error;
}


STATIC void
xlog_grant_head_init(
	struct xlog_grant_head	*head)
{
	xlog_assign_grant_head(&head->grant, 1, 0);
	INIT_LIST_HEAD(&head->waiters);
	spin_lock_init(&head->lock);
}

/*
 * Return size of each in-core log record buffer.
 *
 * All machines get 8 x 32kB buffers by default, unless tuned otherwise.
 *
 * If the filesystem blocksize is too large, we may need to choose a
 * larger size since the directory code currently logs entire blocks.
 */
STATIC void
xlog_get_iclog_buffer_size(
	struct xfs_mount	*mp,
	struct xlog		*log)
{
	if (mp->m_logbufs <= 0)
		mp->m_logbufs = XLOG_MAX_ICLOGS;
	if (mp->m_logbsize <= 0)
		mp->m_logbsize = XLOG_BIG_RECORD_BSIZE;

	log->l_iclog_bufs = mp->m_logbufs;
	log->l_iclog_size = mp->m_logbsize;

	/*
	 * # headers = size / 32k - one header holds cycles from 32k of data.
	 */
	log->l_iclog_heads =
		DIV_ROUND_UP(mp->m_logbsize, XLOG_HEADER_CYCLE_SIZE);
	log->l_iclog_hsize = log->l_iclog_heads << BBSHIFT;
}

/*
 * Every sync period we need to unpin all items in the AIL and push them to
 * disk. If there is nothing dirty, then we might need to cover the log to
 * indicate that the filesystem is idle.
 */
static void
xfs_log_worker(
	struct work_struct	*work)
{
	struct xlog		*log = container_of(to_delayed_work(work),
						struct xlog, l_work);
	struct xfs_mount	*mp = log->l_mp;

	/* dgc: errors ignored - not fatal and nowhere to report them */
	if (xfs_fs_writable(mp, SB_FREEZE_WRITE) && xfs_log_need_covered(mp)) {
		/*
		 * Dump a transaction into the log that contains no real change.
		 * This is needed to stamp the current tail LSN into the log
		 * during the covering operation.
		 *
		 * We cannot use an inode here for this - that will push dirty
		 * state back up into the VFS and then periodic inode flushing
		 * will prevent log covering from making progress. Hence we
		 * synchronously log the superblock instead to ensure the
		 * superblock is immediately unpinned and can be written back.
		 */
		xlog_clear_incompat(log);
		xfs_sync_sb(mp, true);
	} else
		xfs_log_force(mp, 0);

	/* start pushing all the metadata that is currently dirty */
	xfs_ail_push_all(mp->m_ail);

	/* queue us up again */
	xfs_log_work_queue(mp);
}

/*
 * This routine initializes some of the log structure for a given mount point.
 * Its primary purpose is to fill in enough, so recovery can occur.  However,
 * some other stuff may be filled in too.
 */
STATIC struct xlog *
xlog_alloc_log(
	struct xfs_mount	*mp,
	struct xfs_buftarg	*log_target,
	xfs_daddr_t		blk_offset,
	int			num_bblks)
{
	struct xlog		*log;
	xlog_rec_header_t	*head;
	xlog_in_core_t		**iclogp;
	xlog_in_core_t		*iclog, *prev_iclog=NULL;
	int			i;
	int			error = -ENOMEM;
	uint			log2_size = 0;

	log = kmem_zalloc(sizeof(struct xlog), KM_MAYFAIL);
	if (!log) {
		xfs_warn(mp, "Log allocation failed: No memory!");
		goto out;
	}

	log->l_mp	   = mp;
	log->l_targ	   = log_target;
	log->l_logsize     = BBTOB(num_bblks);
	log->l_logBBstart  = blk_offset;
	log->l_logBBsize   = num_bblks;
	log->l_covered_state = XLOG_STATE_COVER_IDLE;
	set_bit(XLOG_ACTIVE_RECOVERY, &log->l_opstate);
	INIT_DELAYED_WORK(&log->l_work, xfs_log_worker);

	log->l_prev_block  = -1;
	/* log->l_tail_lsn = 0x100000000LL; cycle = 1; current block = 0 */
	xlog_assign_atomic_lsn(&log->l_tail_lsn, 1, 0);
	xlog_assign_atomic_lsn(&log->l_last_sync_lsn, 1, 0);
	log->l_curr_cycle  = 1;	    /* 0 is bad since this is initial value */

	if (xfs_has_logv2(mp) && mp->m_sb.sb_logsunit > 1)
		log->l_iclog_roundoff = mp->m_sb.sb_logsunit;
	else
		log->l_iclog_roundoff = BBSIZE;

	xlog_grant_head_init(&log->l_reserve_head);
	xlog_grant_head_init(&log->l_write_head);

	error = -EFSCORRUPTED;
	if (xfs_has_sector(mp)) {
		log2_size = mp->m_sb.sb_logsectlog;
		if (log2_size < BBSHIFT) {
			xfs_warn(mp, "Log sector size too small (0x%x < 0x%x)",
				log2_size, BBSHIFT);
			goto out_free_log;
		}

		log2_size -= BBSHIFT;
		if (log2_size > mp->m_sectbb_log) {
			xfs_warn(mp, "Log sector size too large (0x%x > 0x%x)",
				log2_size, mp->m_sectbb_log);
			goto out_free_log;
		}

		/* for larger sector sizes, must have v2 or external log */
		if (log2_size && log->l_logBBstart > 0 &&
			    !xfs_has_logv2(mp)) {
			xfs_warn(mp,
		"log sector size (0x%x) invalid for configuration.",
				log2_size);
			goto out_free_log;
		}
	}
	log->l_sectBBsize = 1 << log2_size;

	init_rwsem(&log->l_incompat_users);

	xlog_get_iclog_buffer_size(mp, log);

	spin_lock_init(&log->l_icloglock);
	init_waitqueue_head(&log->l_flush_wait);

	iclogp = &log->l_iclog;
	/*
	 * The amount of memory to allocate for the iclog structure is
	 * rather funky due to the way the structure is defined.  It is
	 * done this way so that we can use different sizes for machines
	 * with different amounts of memory.  See the definition of
	 * xlog_in_core_t in xfs_log_priv.h for details.
	 */
	ASSERT(log->l_iclog_size >= 4096);
	for (i = 0; i < log->l_iclog_bufs; i++) {
		size_t bvec_size = howmany(log->l_iclog_size, PAGE_SIZE) *
				sizeof(struct bio_vec);
		iclog = kmem_zalloc(sizeof(*iclog) + bvec_size, KM_MAYFAIL);
		if (!iclog)
			goto out_free_iclog;

		*iclogp = iclog;
		iclog->ic_prev = prev_iclog;
		prev_iclog = iclog;

		iclog->ic_data = kvzalloc(log->l_iclog_size,
				GFP_KERNEL | __GFP_RETRY_MAYFAIL);
		if (!iclog->ic_data)
			goto out_free_iclog;
		head = &iclog->ic_header;
		memset(head, 0, sizeof(xlog_rec_header_t));
		head->h_magicno = cpu_to_be32(XLOG_HEADER_MAGIC_NUM);
		head->h_version = cpu_to_be32(
			xfs_has_logv2(log->l_mp) ? 2 : 1);
		head->h_size = cpu_to_be32(log->l_iclog_size);
		/* new fields */
		head->h_fmt = cpu_to_be32(XLOG_FMT);
		memcpy(&head->h_fs_uuid, &mp->m_sb.sb_uuid, sizeof(uuid_t));

		iclog->ic_size = log->l_iclog_size - log->l_iclog_hsize;
		iclog->ic_state = XLOG_STATE_ACTIVE;
		iclog->ic_log = log;
		atomic_set(&iclog->ic_refcnt, 0);
		INIT_LIST_HEAD(&iclog->ic_callbacks);
		iclog->ic_datap = (void *)iclog->ic_data + log->l_iclog_hsize;

		init_waitqueue_head(&iclog->ic_force_wait);
		init_waitqueue_head(&iclog->ic_write_wait);
		INIT_WORK(&iclog->ic_end_io_work, xlog_ioend_work);
		sema_init(&iclog->ic_sema, 1);

		iclogp = &iclog->ic_next;
	}
	*iclogp = log->l_iclog;			/* complete ring */
	log->l_iclog->ic_prev = prev_iclog;	/* re-write 1st prev ptr */

	log->l_ioend_workqueue = alloc_workqueue("xfs-log/%s",
			XFS_WQFLAGS(WQ_FREEZABLE | WQ_MEM_RECLAIM |
				    WQ_HIGHPRI),
			0, mp->m_super->s_id);
	if (!log->l_ioend_workqueue)
		goto out_free_iclog;

	error = xlog_cil_init(log);
	if (error)
		goto out_destroy_workqueue;
	return log;

out_destroy_workqueue:
	destroy_workqueue(log->l_ioend_workqueue);
out_free_iclog:
	for (iclog = log->l_iclog; iclog; iclog = prev_iclog) {
		prev_iclog = iclog->ic_next;
		kmem_free(iclog->ic_data);
		kmem_free(iclog);
		if (prev_iclog == log->l_iclog)
			break;
	}
out_free_log:
	kmem_free(log);
out:
	return ERR_PTR(error);
}	/* xlog_alloc_log */

/*
 * Calculate the checksum for a log buffer.
 *
 * This is a little more complicated than it should be because the various
 * headers and the actual data are non-contiguous.
 */
__le32
xlog_cksum(
	struct xlog		*log,
	struct xlog_rec_header	*rhead,
	char			*dp,
	int			size)
{
	uint32_t		crc;

	/* first generate the crc for the record header ... */
	crc = xfs_start_cksum_update((char *)rhead,
			      sizeof(struct xlog_rec_header),
			      offsetof(struct xlog_rec_header, h_crc));

	/* ... then for additional cycle data for v2 logs ... */
	if (xfs_has_logv2(log->l_mp)) {
		union xlog_in_core2 *xhdr = (union xlog_in_core2 *)rhead;
		int		i;
		int		xheads;

		xheads = DIV_ROUND_UP(size, XLOG_HEADER_CYCLE_SIZE);

		for (i = 1; i < xheads; i++) {
			crc = crc32c(crc, &xhdr[i].hic_xheader,
				     sizeof(struct xlog_rec_ext_header));
		}
	}

	/* ... and finally for the payload */
	crc = crc32c(crc, dp, size);

	return xfs_end_cksum(crc);
}

/*
 * Verify that an LSN stamped into a piece of metadata is valid. This is
 * intended for use in read verifiers on v5 superblocks.
 */
bool
xfs_log_check_lsn(
	struct xfs_mount	*mp,
	xfs_lsn_t		lsn)
{
	struct xlog		*log = mp->m_log;
	bool			valid;

	/*
	 * norecovery mode skips mount-time log processing and unconditionally
	 * resets the in-core LSN. We can't validate in this mode, but
	 * modifications are not allowed anyways so just return true.
	 */
	if (xfs_has_norecovery(mp))
		return true;

	/*
	 * Some metadata LSNs are initialized to NULL (e.g., the agfl). This is
	 * handled by recovery and thus safe to ignore here.
	 */
	if (lsn == NULLCOMMITLSN)
		return true;

	valid = xlog_valid_lsn(mp->m_log, lsn);

	/* warn the user about what's gone wrong before verifier failure */
	if (!valid) {
		spin_lock(&log->l_icloglock);
		xfs_warn(mp,
"Corruption warning: Metadata has LSN (%d:%d) ahead of current LSN (%d:%d). "
"Please unmount and run xfs_repair (>= v4.3) to resolve.",
			 CYCLE_LSN(lsn), BLOCK_LSN(lsn),
			 log->l_curr_cycle, log->l_curr_block);
		spin_unlock(&log->l_icloglock);
	}

	return valid;
}

xfs_lsn_t
xlog_assign_tail_lsn_locked(
	struct xfs_mount	*mp)
{
	struct xlog		*log = mp->m_log;
	struct xfs_log_item	*lip;
	xfs_lsn_t		tail_lsn;

	assert_spin_locked(&mp->m_ail->ail_lock);

	/*
	 * To make sure we always have a valid LSN for the log tail we keep
	 * track of the last LSN which was committed in log->l_last_sync_lsn,
	 * and use that when the AIL was empty.
	 */
	lip = xfs_ail_min(mp->m_ail);
	if (lip)
		tail_lsn = lip->li_lsn;
	else
		tail_lsn = atomic64_read(&log->l_last_sync_lsn);
	trace_xfs_log_assign_tail_lsn(log, tail_lsn);
	atomic64_set(&log->l_tail_lsn, tail_lsn);
	return tail_lsn;
}

/*
 * Return the space in the log between the tail and the head.  The head
 * is passed in the cycle/bytes formal parms.  In the special case where
 * the reserve head has wrapped passed the tail, this calculation is no
 * longer valid.  In this case, just return 0 which means there is no space
 * in the log.  This works for all places where this function is called
 * with the reserve head.  Of course, if the write head were to ever
 * wrap the tail, we should blow up.  Rather than catch this case here,
 * we depend on other ASSERTions in other parts of the code.   XXXmiken
 *
 * If reservation head is behind the tail, we have a problem. Warn about it,
 * but then treat it as if the log is empty.
 *
 * If the log is shut down, the head and tail may be invalid or out of whack, so
 * shortcut invalidity asserts in this case so that we don't trigger them
 * falsely.
 */
STATIC int
xlog_space_left(
	struct xlog	*log,
	atomic64_t	*head)
{
	int		tail_bytes;
	int		tail_cycle;
	int		head_cycle;
	int		head_bytes;

	xlog_crack_grant_head(head, &head_cycle, &head_bytes);
	xlog_crack_atomic_lsn(&log->l_tail_lsn, &tail_cycle, &tail_bytes);
	tail_bytes = BBTOB(tail_bytes);
	if (tail_cycle == head_cycle && head_bytes >= tail_bytes)
		return log->l_logsize - (head_bytes - tail_bytes);
	if (tail_cycle + 1 < head_cycle)
		return 0;

	/* Ignore potential inconsistency when shutdown. */
	if (xlog_is_shutdown(log))
		return log->l_logsize;

	if (tail_cycle < head_cycle) {
		ASSERT(tail_cycle == (head_cycle - 1));
		return tail_bytes - head_bytes;
	}

	/*
	 * The reservation head is behind the tail. In this case we just want to
	 * return the size of the log as the amount of space left.
	 */
	xfs_alert(log->l_mp, "xlog_space_left: head behind tail");
	xfs_alert(log->l_mp, "  tail_cycle = %d, tail_bytes = %d",
		  tail_cycle, tail_bytes);
	xfs_alert(log->l_mp, "  GH   cycle = %d, GH   bytes = %d",
		  head_cycle, head_bytes);
	ASSERT(0);
	return log->l_logsize;
}

static inline int
xlog_ticket_reservation(
	struct xlog		*log,
	struct xlog_grant_head	*head,
	struct xlog_ticket	*tic)
{
	if (head == &log->l_write_head) {
		ASSERT(tic->t_flags & XLOG_TIC_PERM_RESERV);
		return tic->t_unit_res;
	}

	if (tic->t_flags & XLOG_TIC_PERM_RESERV)
		return tic->t_unit_res * tic->t_cnt;

	return tic->t_unit_res;
}

/*
 * Compute the LSN that we'd need to push the log tail towards in order to have
 * (a) enough on-disk log space to log the number of bytes specified, (b) at
 * least 25% of the log space free, and (c) at least 256 blocks free.  If the
 * log free space already meets all three thresholds, this function returns
 * NULLCOMMITLSN.
 */
xfs_lsn_t
xlog_grant_push_threshold(
	struct xlog	*log,
	int		need_bytes)
{
	xfs_lsn_t	threshold_lsn = 0;
	xfs_lsn_t	last_sync_lsn;
	int		free_blocks;
	int		free_bytes;
	int		threshold_block;
	int		threshold_cycle;
	int		free_threshold;

	ASSERT(BTOBB(need_bytes) < log->l_logBBsize);

	free_bytes = xlog_space_left(log, &log->l_reserve_head.grant);
	free_blocks = BTOBBT(free_bytes);

	/*
	 * Set the threshold for the minimum number of free blocks in the
	 * log to the maximum of what the caller needs, one quarter of the
	 * log, and 256 blocks.
	 */
	free_threshold = BTOBB(need_bytes);
	free_threshold = max(free_threshold, (log->l_logBBsize >> 2));
	free_threshold = max(free_threshold, 256);
	if (free_blocks >= free_threshold)
		return NULLCOMMITLSN;

	xlog_crack_atomic_lsn(&log->l_tail_lsn, &threshold_cycle,
						&threshold_block);
	threshold_block += free_threshold;
	if (threshold_block >= log->l_logBBsize) {
		threshold_block -= log->l_logBBsize;
		threshold_cycle += 1;
	}
	threshold_lsn = xlog_assign_lsn(threshold_cycle,
					threshold_block);
	/*
	 * Don't pass in an lsn greater than the lsn of the last
	 * log record known to be on disk. Use a snapshot of the last sync lsn
	 * so that it doesn't change between the compare and the set.
	 */
	last_sync_lsn = atomic64_read(&log->l_last_sync_lsn);
	if (XFS_LSN_CMP(threshold_lsn, last_sync_lsn) > 0)
		threshold_lsn = last_sync_lsn;

	return threshold_lsn;
}


/*
 * Push the tail of the log if we need to do so to maintain the free log space
 * thresholds set out by xlog_grant_push_threshold.  We may need to adopt a
 * policy which pushes on an lsn which is further along in the log once we
 * reach the high water mark.  In this manner, we would be creating a low water
 * mark.
 */
STATIC void
xlog_grant_push_ail(
	struct xlog	*log,
	int		need_bytes)
{
	xfs_lsn_t	threshold_lsn;

	threshold_lsn = xlog_grant_push_threshold(log, need_bytes);
	if (threshold_lsn == NULLCOMMITLSN || xlog_is_shutdown(log))
		return;

	/*
	 * Get the transaction layer to kick the dirty buffers out to
	 * disk asynchronously. No point in trying to do this if
	 * the filesystem is shutting down.
	 */
	xfs_ail_push(log->l_ailp, threshold_lsn);
}

STATIC bool
xlog_grant_head_wake(
	struct xlog		*log,
	struct xlog_grant_head	*head,
	int			*free_bytes)
{
	struct xlog_ticket	*tic;
	int			need_bytes;
	bool			woken_task = false;

	list_for_each_entry(tic, &head->waiters, t_queue) {

		/*
		 * There is a chance that the size of the CIL checkpoints in
		 * progress at the last AIL push target calculation resulted in
		 * limiting the target to the log head (l_last_sync_lsn) at the
		 * time. This may not reflect where the log head is now as the
		 * CIL checkpoints may have completed.
		 *
		 * Hence when we are woken here, it may be that the head of the
		 * log that has moved rather than the tail. As the tail didn't
		 * move, there still won't be space available for the
		 * reservation we require.  However, if the AIL has already
		 * pushed to the target defined by the old log head location, we
		 * will hang here waiting for something else to update the AIL
		 * push target.
		 *
		 * Therefore, if there isn't space to wake the first waiter on
		 * the grant head, we need to push the AIL again to ensure the
		 * target reflects both the current log tail and log head
		 * position before we wait for the tail to move again.
		 */

		need_bytes = xlog_ticket_reservation(log, head, tic);
		if (*free_bytes < need_bytes) {
			if (!woken_task)
				xlog_grant_push_ail(log, need_bytes);
			return false;
		}

		*free_bytes -= need_bytes;
		trace_xfs_log_grant_wake_up(log, tic);
		wake_up_process(tic->t_task);
		woken_task = true;
	}

	return true;
}

/*
 * Wake up processes waiting for log space after we have moved the log tail.
 */
void
xfs_log_space_wake(
	struct xfs_mount	*mp)
{
	struct xlog		*log = mp->m_log;
	int			free_bytes;

	if (xlog_is_shutdown(log))
		return;

	if (!list_empty_careful(&log->l_write_head.waiters)) {
		ASSERT(!xlog_in_recovery(log));

		spin_lock(&log->l_write_head.lock);
		free_bytes = xlog_space_left(log, &log->l_write_head.grant);
		xlog_grant_head_wake(log, &log->l_write_head, &free_bytes);
		spin_unlock(&log->l_write_head.lock);
	}

	if (!list_empty_careful(&log->l_reserve_head.waiters)) {
		ASSERT(!xlog_in_recovery(log));

		spin_lock(&log->l_reserve_head.lock);
		free_bytes = xlog_space_left(log, &log->l_reserve_head.grant);
		xlog_grant_head_wake(log, &log->l_reserve_head, &free_bytes);
		spin_unlock(&log->l_reserve_head.lock);
	}
}

xfs_lsn_t
xlog_assign_tail_lsn(
	struct xfs_mount	*mp)
{
	xfs_lsn_t		tail_lsn;

	spin_lock(&mp->m_ail->ail_lock);
	tail_lsn = xlog_assign_tail_lsn_locked(mp);
	spin_unlock(&mp->m_ail->ail_lock);

	return tail_lsn;
}

/*
 * Figure out the total log space unit (in bytes) that would be
 * required for a log ticket.
 */
static int
xlog_calc_unit_res(
	struct xlog		*log,
	int			unit_bytes,
	int			*niclogs)
{
	int			iclog_space;
	uint			num_headers;

	/*
	 * Permanent reservations have up to 'cnt'-1 active log operations
	 * in the log.  A unit in this case is the amount of space for one
	 * of these log operations.  Normal reservations have a cnt of 1
	 * and their unit amount is the total amount of space required.
	 *
	 * The following lines of code account for non-transaction data
	 * which occupy space in the on-disk log.
	 *
	 * Normal form of a transaction is:
	 * <oph><trans-hdr><start-oph><reg1-oph><reg1><reg2-oph>...<commit-oph>
	 * and then there are LR hdrs, split-recs and roundoff at end of syncs.
	 *
	 * We need to account for all the leadup data and trailer data
	 * around the transaction data.
	 * And then we need to account for the worst case in terms of using
	 * more space.
	 * The worst case will happen if:
	 * - the placement of the transaction happens to be such that the
	 *   roundoff is at its maximum
	 * - the transaction data is synced before the commit record is synced
	 *   i.e. <transaction-data><roundoff> | <commit-rec><roundoff>
	 *   Therefore the commit record is in its own Log Record.
	 *   This can happen as the commit record is called with its
	 *   own region to xlog_write().
	 *   This then means that in the worst case, roundoff can happen for
	 *   the commit-rec as well.
	 *   The commit-rec is smaller than padding in this scenario and so it is
	 *   not added separately.
	 */

	/* for trans header */
	unit_bytes += sizeof(xlog_op_header_t);
	unit_bytes += sizeof(xfs_trans_header_t);

	/* for start-rec */
	unit_bytes += sizeof(xlog_op_header_t);

	/*
	 * for LR headers - the space for data in an iclog is the size minus
	 * the space used for the headers. If we use the iclog size, then we
	 * undercalculate the number of headers required.
	 *
	 * Furthermore - the addition of op headers for split-recs might
	 * increase the space required enough to require more log and op
	 * headers, so take that into account too.
	 *
	 * IMPORTANT: This reservation makes the assumption that if this
	 * transaction is the first in an iclog and hence has the LR headers
	 * accounted to it, then the remaining space in the iclog is
	 * exclusively for this transaction.  i.e. if the transaction is larger
	 * than the iclog, it will be the only thing in that iclog.
	 * Fundamentally, this means we must pass the entire log vector to
	 * xlog_write to guarantee this.
	 */
	iclog_space = log->l_iclog_size - log->l_iclog_hsize;
	num_headers = howmany(unit_bytes, iclog_space);

	/* for split-recs - ophdrs added when data split over LRs */
	unit_bytes += sizeof(xlog_op_header_t) * num_headers;

	/* add extra header reservations if we overrun */
	while (!num_headers ||
	       howmany(unit_bytes, iclog_space) > num_headers) {
		unit_bytes += sizeof(xlog_op_header_t);
		num_headers++;
	}
	unit_bytes += log->l_iclog_hsize * num_headers;

	/* for commit-rec LR header - note: padding will subsume the ophdr */
	unit_bytes += log->l_iclog_hsize;

	/* roundoff padding for transaction data and one for commit record */
	unit_bytes += 2 * log->l_iclog_roundoff;

	if (niclogs)
		*niclogs = num_headers;
	return unit_bytes;
}

/*
 * Allocate and initialise a new log ticket.
 */
struct xlog_ticket *
xlog_ticket_alloc(
	struct xlog		*log,
	int			unit_bytes,
	int			cnt,
	bool			permanent)
{
	struct xlog_ticket	*tic;
	int			unit_res;

	tic = kmem_cache_zalloc(xfs_log_ticket_cache, GFP_NOFS | __GFP_NOFAIL);

	unit_res = xlog_calc_unit_res(log, unit_bytes, &tic->t_iclog_hdrs);

	atomic_set(&tic->t_ref, 1);
	tic->t_task		= current;
	INIT_LIST_HEAD(&tic->t_queue);
	tic->t_unit_res		= unit_res;
	tic->t_curr_res		= unit_res;
	tic->t_cnt		= cnt;
	tic->t_ocnt		= cnt;
	tic->t_tid		= get_random_u32();
	if (permanent)
		tic->t_flags |= XLOG_TIC_PERM_RESERV;

	return tic;
}

void
xfs_log_work_queue(
	struct xfs_mount        *mp)
{
	queue_delayed_work(mp->m_sync_workqueue, &mp->m_log->l_work,
				msecs_to_jiffies(xfs_syncd_centisecs * 10));
}

/*
 * Finish the recovery of the file system.  This is separate from the
 * xfs_log_mount() call, because it depends on the code in xfs_mountfs() to read
 * in the root and real-time bitmap inodes between calling xfs_log_mount() and
 * here.
 *
 * If we finish recovery successfully, start the background log work. If we are
 * not doing recovery, then we have a RO filesystem and we don't need to start
 * it.
 */
int
xfs_log_mount_finish(
	struct xfs_mount	*mp)
{
	struct xlog		*log = mp->m_log;
	int			error = 0;

	if (xfs_has_norecovery(mp)) {
		ASSERT(xfs_is_readonly(mp));
		return 0;
	}

	/*
	 * During the second phase of log recovery, we need iget and
	 * iput to behave like they do for an active filesystem.
	 * xfs_fs_drop_inode needs to be able to prevent the deletion
	 * of inodes before we're done replaying log items on those
	 * inodes.  Turn it off immediately after recovery finishes
	 * so that we don't leak the quota inodes if subsequent mount
	 * activities fail.
	 *
	 * We let all inodes involved in redo item processing end up on
	 * the LRU instead of being evicted immediately so that if we do
	 * something to an unlinked inode, the irele won't cause
	 * premature truncation and freeing of the inode, which results
	 * in log recovery failure.  We have to evict the unreferenced
	 * lru inodes after clearing SB_ACTIVE because we don't
	 * otherwise clean up the lru if there's a subsequent failure in
	 * xfs_mountfs, which leads to us leaking the inodes if nothing
	 * else (e.g. quotacheck) references the inodes before the
	 * mount failure occurs.
	 */
	mp->m_super->s_flags |= SB_ACTIVE;
	xfs_log_work_queue(mp);
	if (xlog_recovery_needed(log))
		error = xlog_recover_finish(log);
	mp->m_super->s_flags &= ~SB_ACTIVE;
	evict_inodes(mp->m_super);

	/*
	 * Drain the buffer LRU after log recovery. This is required for v4
	 * filesystems to avoid leaving around buffers with NULL verifier ops,
	 * but we do it unconditionally to make sure we're always in a clean
	 * cache state after mount.
	 *
	 * Don't push in the error case because the AIL may have pending intents
	 * that aren't removed until recovery is cancelled.
	 */
	if (xlog_recovery_needed(log)) {
		if (!error) {
			xfs_log_force(mp, XFS_LOG_SYNC);
			xfs_ail_push_all_sync(mp->m_ail);
		}
		xfs_notice(mp, "Ending recovery (logdev: %s)",
				mp->m_logname ? mp->m_logname : "internal");
	} else {
		xfs_info(mp, "Ending clean mount");
	}
	xfs_buftarg_drain(mp->m_ddev_targp);

	clear_bit(XLOG_RECOVERY_NEEDED, &log->l_opstate);

	/* Make sure the log is dead if we're returning failure. */
	ASSERT(!error || xlog_is_shutdown(log));

	return error;
}

/*
 * Write out all data in the in-core log as of this exact moment in time.
 *
 * Data may be written to the in-core log during this call.  However,
 * we don't guarantee this data will be written out.  A change from past
 * implementation means this routine will *not* write out zero length LRs.
 *
 * Basically, we try and perform an intelligent scan of the in-core logs.
 * If we determine there is no flushable data, we just return.  There is no
 * flushable data if:
 *
 *	1. the current iclog is active and has no data; the previous iclog
 *		is in the active or dirty state.
 *	2. the current iclog is drity, and the previous iclog is in the
 *		active or dirty state.
 *
 * We may sleep if:
 *
 *	1. the current iclog is not in the active nor dirty state.
 *	2. the current iclog dirty, and the previous iclog is not in the
 *		active nor dirty state.
 *	3. the current iclog is active, and there is another thread writing
 *		to this particular iclog.
 *	4. a) the current iclog is active and has no other writers
 *	   b) when we return from flushing out this iclog, it is still
 *		not in the active nor dirty state.
 */
int
xfs_log_force(
	struct xfs_mount	*mp,
	uint			flags)
{
	struct xlog		*log = mp->m_log;
	struct xlog_in_core	*iclog;

	XFS_STATS_INC(mp, xs_log_force);
	trace_xfs_log_force(mp, 0, _RET_IP_);

	xlog_cil_force(log);

	spin_lock(&log->l_icloglock);
	if (xlog_is_shutdown(log))
		goto out_error;

	iclog = log->l_iclog;
	trace_xlog_iclog_force(iclog, _RET_IP_);

	if (iclog->ic_state == XLOG_STATE_DIRTY ||
	    (iclog->ic_state == XLOG_STATE_ACTIVE &&
	     atomic_read(&iclog->ic_refcnt) == 0 && iclog->ic_offset == 0)) {
		/*
		 * If the head is dirty or (active and empty), then we need to
		 * look at the previous iclog.
		 *
		 * If the previous iclog is active or dirty we are done.  There
		 * is nothing to sync out. Otherwise, we attach ourselves to the
		 * previous iclog and go to sleep.
		 */
		iclog = iclog->ic_prev;
	} else if (iclog->ic_state == XLOG_STATE_ACTIVE) {
		if (atomic_read(&iclog->ic_refcnt) == 0) {
			/* We have exclusive access to this iclog. */
			bool	completed;

			if (xlog_force_and_check_iclog(iclog, &completed))
				goto out_error;

			if (completed)
				goto out_unlock;
		} else {
			/*
			 * Someone else is still writing to this iclog, so we
			 * need to ensure that when they release the iclog it
			 * gets synced immediately as we may be waiting on it.
			 */
			xlog_state_switch_iclogs(log, iclog, 0);
		}
	}

	/*
	 * The iclog we are about to wait on may contain the checkpoint pushed
	 * by the above xlog_cil_force() call, but it may not have been pushed
	 * to disk yet. Like the ACTIVE case above, we need to make sure caches
	 * are flushed when this iclog is written.
	 */
	if (iclog->ic_state == XLOG_STATE_WANT_SYNC)
		iclog->ic_flags |= XLOG_ICL_NEED_FLUSH | XLOG_ICL_NEED_FUA;

	if (flags & XFS_LOG_SYNC)
		return xlog_wait_on_iclog(iclog);
out_unlock:
	spin_unlock(&log->l_icloglock);
	return 0;
out_error:
	spin_unlock(&log->l_icloglock);
	return -EIO;
}

/* check if it will fit */
STATIC void
xlog_verify_tail_lsn(
	struct xlog		*log,
	struct xlog_in_core	*iclog)
{
	xfs_lsn_t	tail_lsn = be64_to_cpu(iclog->ic_header.h_tail_lsn);
	int		blocks;

    if (CYCLE_LSN(tail_lsn) == log->l_prev_cycle) {
	blocks =
	    log->l_logBBsize - (log->l_prev_block - BLOCK_LSN(tail_lsn));
	if (blocks < BTOBB(iclog->ic_offset)+BTOBB(log->l_iclog_hsize))
		xfs_emerg(log->l_mp, "%s: ran out of log space", __func__);
    } else {
	ASSERT(CYCLE_LSN(tail_lsn)+1 == log->l_prev_cycle);

	if (BLOCK_LSN(tail_lsn) == log->l_prev_block)
		xfs_emerg(log->l_mp, "%s: tail wrapped", __func__);

	blocks = BLOCK_LSN(tail_lsn) - log->l_prev_block;
	if (blocks < BTOBB(iclog->ic_offset) + 1)
		xfs_emerg(log->l_mp, "%s: ran out of log space", __func__);
    }
}

static int
xlog_calc_iclog_size(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	uint32_t		*roundoff)
{
	uint32_t		count_init, count;

	/* Add for LR header */
	count_init = log->l_iclog_hsize + iclog->ic_offset;
	count = roundup(count_init, log->l_iclog_roundoff);

	*roundoff = count - count_init;

	ASSERT(count >= count_init);
	ASSERT(*roundoff < log->l_iclog_roundoff);
	return count;
}

static void
xlog_grant_add_space(
	struct xlog		*log,
	atomic64_t		*head,
	int			bytes)
{
	int64_t	head_val = atomic64_read(head);
	int64_t new, old;

	do {
		int		tmp;
		int		cycle, space;

		xlog_crack_grant_head_val(head_val, &cycle, &space);

		tmp = log->l_logsize - space;
		if (tmp > bytes)
			space += bytes;
		else {
			space = bytes - tmp;
			cycle++;
		}

		old = head_val;
		new = xlog_assign_grant_head_val(cycle, space);
		head_val = atomic64_cmpxchg(head, old, new);
	} while (head_val != old);
}

STATIC void
xlog_pack_data(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	int			roundoff)
{
	int			i, j, k;
	int			size = iclog->ic_offset + roundoff;
	__be32			cycle_lsn;
	char			*dp;

	cycle_lsn = CYCLE_LSN_DISK(iclog->ic_header.h_lsn);

	dp = iclog->ic_datap;
	for (i = 0; i < BTOBB(size); i++) {
		if (i >= (XLOG_HEADER_CYCLE_SIZE / BBSIZE))
			break;
		iclog->ic_header.h_cycle_data[i] = *(__be32 *)dp;
		*(__be32 *)dp = cycle_lsn;
		dp += BBSIZE;
	}

	if (xfs_has_logv2(log->l_mp)) {
		xlog_in_core_2_t *xhdr = iclog->ic_data;

		for ( ; i < BTOBB(size); i++) {
			j = i / (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
			k = i % (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
			xhdr[j].hic_xheader.xh_cycle_data[k] = *(__be32 *)dp;
			*(__be32 *)dp = cycle_lsn;
			dp += BBSIZE;
		}

		for (i = 1; i < log->l_iclog_heads; i++)
			xhdr[i].hic_xheader.xh_cycle = cycle_lsn;
	}
}

static void
xlog_ioend_work(
	struct work_struct	*work)
{
	struct xlog_in_core     *iclog =
		container_of(work, struct xlog_in_core, ic_end_io_work);
	struct xlog		*log = iclog->ic_log;
	int			error;

	error = blk_status_to_errno(iclog->ic_bio.bi_status);
#ifdef DEBUG
	/* treat writes with injected CRC errors as failed */
	if (iclog->ic_fail_crc)
		error = -EIO;
#endif

	/*
	 * Race to shutdown the filesystem if we see an error.
	 */
	if (XFS_TEST_ERROR(error, log->l_mp, XFS_ERRTAG_IODONE_IOERR)) {
		xfs_alert(log->l_mp, "log I/O error %d", error);
		xlog_force_shutdown(log, SHUTDOWN_LOG_IO_ERROR);
	}

	xlog_state_done_syncing(iclog);
	bio_uninit(&iclog->ic_bio);

	/*
	 * Drop the lock to signal that we are done. Nothing references the
	 * iclog after this, so an unmount waiting on this lock can now tear it
	 * down safely. As such, it is unsafe to reference the iclog after the
	 * unlock as we could race with it being freed.
	 */
	up(&iclog->ic_sema);
}

static void
xlog_bio_end_io(
	struct bio		*bio)
{
	struct xlog_in_core	*iclog = bio->bi_private;

	queue_work(iclog->ic_log->l_ioend_workqueue,
		   &iclog->ic_end_io_work);
}

static int
xlog_map_iclog_data(
	struct bio		*bio,
	void			*data,
	size_t			count)
{
	do {
		struct page	*page = kmem_to_page(data);
		unsigned int	off = offset_in_page(data);
		size_t		len = min_t(size_t, count, PAGE_SIZE - off);

		if (bio_add_page(bio, page, len, off) != len)
			return -EIO;

		data += len;
		count -= len;
	} while (count);

	return 0;
}

STATIC void
xlog_write_iclog(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	uint64_t		bno,
	unsigned int		count)
{
	ASSERT(bno < log->l_logBBsize);
	trace_xlog_iclog_write(iclog, _RET_IP_);

	/*
	 * We lock the iclogbufs here so that we can serialise against I/O
	 * completion during unmount.  We might be processing a shutdown
	 * triggered during unmount, and that can occur asynchronously to the
	 * unmount thread, and hence we need to ensure that completes before
	 * tearing down the iclogbufs.  Hence we need to hold the buffer lock
	 * across the log IO to archieve that.
	 */
	down(&iclog->ic_sema);
	if (xlog_is_shutdown(log)) {
		/*
		 * It would seem logical to return EIO here, but we rely on
		 * the log state machine to propagate I/O errors instead of
		 * doing it here.  We kick of the state machine and unlock
		 * the buffer manually, the code needs to be kept in sync
		 * with the I/O completion path.
		 */
		xlog_state_done_syncing(iclog);
		up(&iclog->ic_sema);
		return;
	}

	/*
	 * We use REQ_SYNC | REQ_IDLE here to tell the block layer the are more
	 * IOs coming immediately after this one. This prevents the block layer
	 * writeback throttle from throttling log writes behind background
	 * metadata writeback and causing priority inversions.
	 */
	bio_init(&iclog->ic_bio, log->l_targ->bt_bdev, iclog->ic_bvec,
		 howmany(count, PAGE_SIZE),
		 REQ_OP_WRITE | REQ_META | REQ_SYNC | REQ_IDLE);
	iclog->ic_bio.bi_iter.bi_sector = log->l_logBBstart + bno;
	iclog->ic_bio.bi_end_io = xlog_bio_end_io;
	iclog->ic_bio.bi_private = iclog;

	if (iclog->ic_flags & XLOG_ICL_NEED_FLUSH) {
		iclog->ic_bio.bi_opf |= REQ_PREFLUSH;
		/*
		 * For external log devices, we also need to flush the data
		 * device cache first to ensure all metadata writeback covered
		 * by the LSN in this iclog is on stable storage. This is slow,
		 * but it *must* complete before we issue the external log IO.
		 *
		 * If the flush fails, we cannot conclude that past metadata
		 * writeback from the log succeeded.  Repeating the flush is
		 * not possible, hence we must shut down with log IO error to
		 * avoid shutdown re-entering this path and erroring out again.
		 */
		if (log->l_targ != log->l_mp->m_ddev_targp &&
		    blkdev_issue_flush(log->l_mp->m_ddev_targp->bt_bdev)) {
			xlog_force_shutdown(log, SHUTDOWN_LOG_IO_ERROR);
			return;
		}
	}
	if (iclog->ic_flags & XLOG_ICL_NEED_FUA)
		iclog->ic_bio.bi_opf |= REQ_FUA;

	iclog->ic_flags &= ~(XLOG_ICL_NEED_FLUSH | XLOG_ICL_NEED_FUA);

	if (xlog_map_iclog_data(&iclog->ic_bio, iclog->ic_data, count)) {
		xlog_force_shutdown(log, SHUTDOWN_LOG_IO_ERROR);
		return;
	}
	if (is_vmalloc_addr(iclog->ic_data))
		flush_kernel_vmap_range(iclog->ic_data, count);

	/*
	 * If this log buffer would straddle the end of the log we will have
	 * to split it up into two bios, so that we can continue at the start.
	 */
	if (bno + BTOBB(count) > log->l_logBBsize) {
		struct bio *split;

		split = bio_split(&iclog->ic_bio, log->l_logBBsize - bno,
				  GFP_NOIO, &fs_bio_set);
		bio_chain(split, &iclog->ic_bio);
		submit_bio(split);

		/* restart at logical offset zero for the remainder */
		iclog->ic_bio.bi_iter.bi_sector = log->l_logBBstart;
	}

	submit_bio(&iclog->ic_bio);
}

static void
xlog_split_iclog(
	struct xlog		*log,
	void			*data,
	uint64_t		bno,
	unsigned int		count)
{
	unsigned int		split_offset = BBTOB(log->l_logBBsize - bno);
	unsigned int		i;

	for (i = split_offset; i < count; i += BBSIZE) {
		uint32_t cycle = get_unaligned_be32(data + i);

		if (++cycle == XLOG_HEADER_MAGIC_NUM)
			cycle++;
		put_unaligned_be32(cycle, data + i);
	}
}

/*
 * Flush out the in-core log (iclog) to the on-disk log in an asynchronous
 * fashion.  Previously, we should have moved the current iclog
 * ptr in the log to point to the next available iclog.  This allows further
 * write to continue while this code syncs out an iclog ready to go.
 * Before an in-core log can be written out, the data section must be scanned
 * to save away the 1st word of each BBSIZE block into the header.  We replace
 * it with the current cycle count.  Each BBSIZE block is tagged with the
 * cycle count because there in an implicit assumption that drives will
 * guarantee that entire 512 byte blocks get written at once.  In other words,
 * we can't have part of a 512 byte block written and part not written.  By
 * tagging each block, we will know which blocks are valid when recovering
 * after an unclean shutdown.
 *
 * This routine is single threaded on the iclog.  No other thread can be in
 * this routine with the same iclog.  Changing contents of iclog can there-
 * fore be done without grabbing the state machine lock.  Updating the global
 * log will require grabbing the lock though.
 *
 * The entire log manager uses a logical block numbering scheme.  Only
 * xlog_write_iclog knows about the fact that the log may not start with
 * block zero on a given device.
 */
STATIC void
xlog_sync(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	struct xlog_ticket	*ticket)
{
	unsigned int		count;		/* byte count of bwrite */
	unsigned int		roundoff;       /* roundoff to BB or stripe */
	uint64_t		bno;
	unsigned int		size;

	ASSERT(atomic_read(&iclog->ic_refcnt) == 0);
	trace_xlog_iclog_sync(iclog, _RET_IP_);

	count = xlog_calc_iclog_size(log, iclog, &roundoff);

	/*
	 * If we have a ticket, account for the roundoff via the ticket
	 * reservation to avoid touching the hot grant heads needlessly.
	 * Otherwise, we have to move grant heads directly.
	 */
	if (ticket) {
		ticket->t_curr_res -= roundoff;
	} else {
		xlog_grant_add_space(log, &log->l_reserve_head.grant, roundoff);
		xlog_grant_add_space(log, &log->l_write_head.grant, roundoff);
	}

	/* put cycle number in every block */
	xlog_pack_data(log, iclog, roundoff);

	/* real byte length */
	size = iclog->ic_offset;
	if (xfs_has_logv2(log->l_mp))
		size += roundoff;
	iclog->ic_header.h_len = cpu_to_be32(size);

	XFS_STATS_INC(log->l_mp, xs_log_writes);
	XFS_STATS_ADD(log->l_mp, xs_log_blocks, BTOBB(count));

	bno = BLOCK_LSN(be64_to_cpu(iclog->ic_header.h_lsn));

	/* Do we need to split this write into 2 parts? */
	if (bno + BTOBB(count) > log->l_logBBsize)
		xlog_split_iclog(log, &iclog->ic_header, bno, count);

	/* calculcate the checksum */
	iclog->ic_header.h_crc = xlog_cksum(log, &iclog->ic_header,
					    iclog->ic_datap, size);
	/*
	 * Intentionally corrupt the log record CRC based on the error injection
	 * frequency, if defined. This facilitates testing log recovery in the
	 * event of torn writes. Hence, set the IOABORT state to abort the log
	 * write on I/O completion and shutdown the fs. The subsequent mount
	 * detects the bad CRC and attempts to recover.
	 */
#ifdef DEBUG
	if (XFS_TEST_ERROR(false, log->l_mp, XFS_ERRTAG_LOG_BAD_CRC)) {
		iclog->ic_header.h_crc &= cpu_to_le32(0xAAAAAAAA);
		iclog->ic_fail_crc = true;
		xfs_warn(log->l_mp,
	"Intentionally corrupted log record at LSN 0x%llx. Shutdown imminent.",
			 be64_to_cpu(iclog->ic_header.h_lsn));
	}
#endif
	/*
	 * xlog_verify_iclog(log, iclog, count);
	 */
	xlog_write_iclog(log, iclog, bno, count);
}

/*
 * Flush iclog to disk if this is the last reference to the given iclog and the
 * it is in the WANT_SYNC state.
 *
 * If XLOG_ICL_NEED_FUA is already set on the iclog, we need to ensure that the
 * log tail is updated correctly. NEED_FUA indicates that the iclog will be
 * written to stable storage, and implies that a commit record is contained
 * within the iclog. We need to ensure that the log tail does not move beyond
 * the tail that the first commit record in the iclog ordered against, otherwise
 * correct recovery of that checkpoint becomes dependent on future operations
 * performed on this iclog.
 *
 * Hence if NEED_FUA is set and the current iclog tail lsn is empty, write the
 * current tail into iclog. Once the iclog tail is set, future operations must
 * not modify it, otherwise they potentially violate ordering constraints for
 * the checkpoint commit that wrote the initial tail lsn value. The tail lsn in
 * the iclog will get zeroed on activation of the iclog after sync, so we
 * always capture the tail lsn on the iclog on the first NEED_FUA release
 * regardless of the number of active reference counts on this iclog.
 */
int
xlog_state_release_iclog(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	struct xlog_ticket	*ticket)
{
	xfs_lsn_t		tail_lsn;
	bool			last_ref;

	lockdep_assert_held(&log->l_icloglock);

	trace_xlog_iclog_release(iclog, _RET_IP_);
	/*
	 * Grabbing the current log tail needs to be atomic w.r.t. the writing
	 * of the tail LSN into the iclog so we guarantee that the log tail does
	 * not move between the first time we know that the iclog needs to be
	 * made stable and when we eventually submit it.
	 */
	if ((iclog->ic_state == XLOG_STATE_WANT_SYNC ||
	     (iclog->ic_flags & XLOG_ICL_NEED_FUA)) &&
	    !iclog->ic_header.h_tail_lsn) {
		tail_lsn = xlog_assign_tail_lsn(log->l_mp);
		iclog->ic_header.h_tail_lsn = cpu_to_be64(tail_lsn);
	}

	last_ref = atomic_dec_and_test(&iclog->ic_refcnt);

	if (xlog_is_shutdown(log)) {
		/*
		 * If there are no more references to this iclog, process the
		 * pending iclog callbacks that were waiting on the release of
		 * this iclog.
		 */
		if (last_ref)
			xlog_state_shutdown_callbacks(log);
		return -EIO;
	}

	if (!last_ref)
		return 0;

	if (iclog->ic_state != XLOG_STATE_WANT_SYNC) {
		ASSERT(iclog->ic_state == XLOG_STATE_ACTIVE);
		return 0;
	}

	iclog->ic_state = XLOG_STATE_SYNCING;
	xlog_verify_tail_lsn(log, iclog);
	trace_xlog_iclog_syncing(iclog, _RET_IP_);

	spin_unlock(&log->l_icloglock);
	xlog_sync(log, iclog, ticket);
	spin_lock(&log->l_icloglock);
	return 0;
}

/*
 * Finish transitioning this iclog to the dirty state.
 *
 * Callbacks could take time, so they are done outside the scope of the
 * global state machine log lock.
 */
STATIC void
xlog_state_done_syncing(
	struct xlog_in_core	*iclog)
{
	struct xlog		*log = iclog->ic_log;

	spin_lock(&log->l_icloglock);
	ASSERT(atomic_read(&iclog->ic_refcnt) == 0);
	trace_xlog_iclog_sync_done(iclog, _RET_IP_);

	/*
	 * If we got an error, either on the first buffer, or in the case of
	 * split log writes, on the second, we shut down the file system and
	 * no iclogs should ever be attempted to be written to disk again.
	 */
	if (!xlog_is_shutdown(log)) {
		ASSERT(iclog->ic_state == XLOG_STATE_SYNCING);
		iclog->ic_state = XLOG_STATE_DONE_SYNC;
	}

	/*
	 * Someone could be sleeping prior to writing out the next
	 * iclog buffer, we wake them all, one will get to do the
	 * I/O, the others get to wait for the result.
	 */
	wake_up_all(&iclog->ic_write_wait);
	spin_unlock(&log->l_icloglock);
	xlog_state_do_callback(log);
}

/*
 * If the head of the in-core log ring is not (ACTIVE or DIRTY), then we must
 * sleep.  We wait on the flush queue on the head iclog as that should be
 * the first iclog to complete flushing. Hence if all iclogs are syncing,
 * we will wait here and all new writes will sleep until a sync completes.
 *
 * The in-core logs are used in a circular fashion. They are not used
 * out-of-order even when an iclog past the head is free.
 *
 * return:
 *	* log_offset where xlog_write() can start writing into the in-core
 *		log's data space.
 *	* in-core log pointer to which xlog_write() should write.
 *	* boolean indicating this is a continued write to an in-core log.
 *		If this is the last write, then the in-core log's offset field
 *		needs to be incremented, depending on the amount of data which
 *		is copied.
 */
STATIC int
xlog_state_get_iclog_space(
	struct xlog		*log,
	int			len,
	struct xlog_in_core	**iclogp,
	struct xlog_ticket	*ticket,
	int			*logoffsetp)
{
	int		  log_offset;
	xlog_rec_header_t *head;
	xlog_in_core_t	  *iclog;

restart:
	spin_lock(&log->l_icloglock);
	if (xlog_is_shutdown(log)) {
		spin_unlock(&log->l_icloglock);
		return -EIO;
	}

	iclog = log->l_iclog;
	if (iclog->ic_state != XLOG_STATE_ACTIVE) {
		XFS_STATS_INC(log->l_mp, xs_log_noiclogs);

		/* Wait for log writes to have flushed */
		xlog_wait(&log->l_flush_wait, &log->l_icloglock);
		goto restart;
	}

	head = &iclog->ic_header;

	atomic_inc(&iclog->ic_refcnt);	/* prevents sync */
	log_offset = iclog->ic_offset;

	trace_xlog_iclog_get_space(iclog, _RET_IP_);

	/* On the 1st write to an iclog, figure out lsn.  This works
	 * if iclogs marked XLOG_STATE_WANT_SYNC always write out what they are
	 * committing to.  If the offset is set, that's how many blocks
	 * must be written.
	 */
	if (log_offset == 0) {
		ticket->t_curr_res -= log->l_iclog_hsize;
		head->h_cycle = cpu_to_be32(log->l_curr_cycle);
		head->h_lsn = cpu_to_be64(
			xlog_assign_lsn(log->l_curr_cycle, log->l_curr_block));
		ASSERT(log->l_curr_block >= 0);
	}

	/* If there is enough room to write everything, then do it.  Otherwise,
	 * claim the rest of the region and make sure the XLOG_STATE_WANT_SYNC
	 * bit is on, so this will get flushed out.  Don't update ic_offset
	 * until you know exactly how many bytes get copied.  Therefore, wait
	 * until later to update ic_offset.
	 *
	 * xlog_write() algorithm assumes that at least 2 xlog_op_header_t's
	 * can fit into remaining data section.
	 */
	if (iclog->ic_size - iclog->ic_offset < 2*sizeof(xlog_op_header_t)) {
		int		error = 0;

		xlog_state_switch_iclogs(log, iclog, iclog->ic_size);

		/*
		 * If we are the only one writing to this iclog, sync it to
		 * disk.  We need to do an atomic compare and decrement here to
		 * avoid racing with concurrent atomic_dec_and_lock() calls in
		 * xlog_state_release_iclog() when there is more than one
		 * reference to the iclog.
		 */
		if (!atomic_add_unless(&iclog->ic_refcnt, -1, 1))
			error = xlog_state_release_iclog(log, iclog, ticket);
		spin_unlock(&log->l_icloglock);
		if (error)
			return error;
		goto restart;
	}

	/* Do we have enough room to write the full amount in the remainder
	 * of this iclog?  Or must we continue a write on the next iclog and
	 * mark this iclog as completely taken?  In the case where we switch
	 * iclogs (to mark it taken), this particular iclog will release/sync
	 * to disk in xlog_write().
	 */
	if (len <= iclog->ic_size - iclog->ic_offset)
		iclog->ic_offset += len;
	else
		xlog_state_switch_iclogs(log, iclog, iclog->ic_size);
	*iclogp = iclog;

	ASSERT(iclog->ic_offset <= iclog->ic_size);
	spin_unlock(&log->l_icloglock);

	*logoffsetp = log_offset;
	return 0;
}

/*
 * This routine will mark the current iclog in the ring as WANT_SYNC and move
 * the current iclog pointer to the next iclog in the ring.
 */
void
xlog_state_switch_iclogs(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	int			eventual_size)
{
	ASSERT(iclog->ic_state == XLOG_STATE_ACTIVE);
	assert_spin_locked(&log->l_icloglock);
	trace_xlog_iclog_switch(iclog, _RET_IP_);

	if (!eventual_size)
		eventual_size = iclog->ic_offset;
	iclog->ic_state = XLOG_STATE_WANT_SYNC;
	iclog->ic_header.h_prev_block = cpu_to_be32(log->l_prev_block);
	log->l_prev_block = log->l_curr_block;
	log->l_prev_cycle = log->l_curr_cycle;

	/* roll log?: ic_offset changed later */
	log->l_curr_block += BTOBB(eventual_size)+BTOBB(log->l_iclog_hsize);

	/* Round up to next log-sunit */
	if (log->l_iclog_roundoff > BBSIZE) {
		uint32_t sunit_bb = BTOBB(log->l_iclog_roundoff);
		log->l_curr_block = roundup(log->l_curr_block, sunit_bb);
	}

	if (log->l_curr_block >= log->l_logBBsize) {
		/*
		 * Rewind the current block before the cycle is bumped to make
		 * sure that the combined LSN never transiently moves forward
		 * when the log wraps to the next cycle. This is to support the
		 * unlocked sample of these fields from xlog_valid_lsn(). Most
		 * other cases should acquire l_icloglock.
		 */
		log->l_curr_block -= log->l_logBBsize;
		ASSERT(log->l_curr_block >= 0);
		smp_wmb();
		log->l_curr_cycle++;
		if (log->l_curr_cycle == XLOG_HEADER_MAGIC_NUM)
			log->l_curr_cycle++;
	}
	ASSERT(iclog == log->l_iclog);
	log->l_iclog = iclog->ic_next;
}

/*
 * Write some region out to in-core log
 *
 * This will be called when writing externally provided regions or when
 * writing out a commit record for a given transaction.
 *
 * General algorithm:
 *	1. Find total length of this write.  This may include adding to the
 *		lengths passed in.
 *	2. Check whether we violate the tickets reservation.
 *	3. While writing to this iclog
 *	    A. Reserve as much space in this iclog as can get
 *	    B. If this is first write, save away start lsn
 *	    C. While writing this region:
 *		1. If first write of transaction, write start record
 *		2. Write log operation header (header per region)
 *		3. Find out if we can fit entire region into this iclog
 *		4. Potentially, verify destination memcpy ptr
 *		5. Memcpy (partial) region
 *		6. If partial copy, release iclog; otherwise, continue
 *			copying more regions into current iclog
 *	4. Mark want sync bit (in simulation mode)
 *	5. Release iclog for potential flush to on-disk log.
 *
 * ERRORS:
 * 1.	Panic if reservation is overrun.  This should never happen since
 *	reservation amounts are generated internal to the filesystem.
 * NOTES:
 * 1. Tickets are single threaded data structures.
 * 2. The XLOG_END_TRANS & XLOG_CONTINUE_TRANS flags are passed down to the
 *	syncing routine.  When a single log_write region needs to span
 *	multiple in-core logs, the XLOG_CONTINUE_TRANS bit should be set
 *	on all log operation writes which don't contain the end of the
 *	region.  The XLOG_END_TRANS bit is used for the in-core log
 *	operation which contains the end of the continued log_write region.
 * 3. When xlog_state_get_iclog_space() grabs the rest of the current iclog,
 *	we don't really know exactly how much space will be used.  As a result,
 *	we don't update ic_offset until the end when we know exactly how many
 *	bytes have been written out.
 */
int
xlog_write(
	struct xlog		*log,
	struct xfs_cil_ctx	*ctx,
	struct list_head	*lv_chain,
	struct xlog_ticket	*ticket,
	uint32_t		len)

{
	struct xlog_in_core	*iclog = NULL;
	struct xfs_log_vec	*lv;
	uint32_t		record_cnt = 0;
	uint32_t		data_cnt = 0;
	int			error = 0;
	int			log_offset;

	if (ticket->t_curr_res < 0) {
		xfs_alert_tag(log->l_mp, XFS_PTAG_LOGRES,
		     "ctx ticket reservation ran out. Need to up reservation");
		xlog_print_tic_res(log->l_mp, ticket);
		xlog_force_shutdown(log, SHUTDOWN_LOG_IO_ERROR);
	}

	error = xlog_state_get_iclog_space(log, len, &iclog, ticket,
					   &log_offset);
	if (error)
		return error;

	ASSERT(log_offset <= iclog->ic_size - 1);

	/*
	 * If we have a context pointer, pass it the first iclog we are
	 * writing to so it can record state needed for iclog write
	 * ordering.
	 */
	if (ctx)
		xlog_cil_set_ctx_write_state(ctx, iclog);

	list_for_each_entry(lv, lv_chain, lv_list) {
		/*
		 * If the entire log vec does not fit in the iclog, punt it to
		 * the partial copy loop which can handle this case.
		 */
		if (lv->lv_niovecs &&
		    lv->lv_bytes > iclog->ic_size - log_offset) {
			error = xlog_write_partial(lv, ticket, &iclog,
					&log_offset, &len, &record_cnt,
					&data_cnt);
			if (error) {
				/*
				 * We have no iclog to release, so just return
				 * the error immediately.
				 */
				return error;
			}
		} else {
			xlog_write_full(lv, ticket, iclog, &log_offset,
					 &len, &record_cnt, &data_cnt);
		}
	}
	ASSERT(len == 0);

	/*
	 * We've already been guaranteed that the last writes will fit inside
	 * the current iclog, and hence it will already have the space used by
	 * those writes accounted to it. Hence we do not need to update the
	 * iclog with the number of bytes written here.
	 */
	spin_lock(&log->l_icloglock);
	xlog_state_finish_copy(log, iclog, record_cnt, 0);
	error = xlog_state_release_iclog(log, iclog, ticket);
	spin_unlock(&log->l_icloglock);

	return error;
}
