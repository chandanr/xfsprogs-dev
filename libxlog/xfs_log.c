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
