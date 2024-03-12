#include "libxfs.h"
#include "libxlog.h"
#include "xfs_log_priv.h"
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
			       "log");
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
