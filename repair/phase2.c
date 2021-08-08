// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "libxlog.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "protos.h"
#include "err_protos.h"
#include "incore.h"
#include "progress.h"
#include "scan.h"

/* workaround craziness in the xlog routines */
int xlog_recover_do_trans(struct xlog *log, struct xlog_recover *t, int p)
{
	return 0;
}

static void
zero_log(
	struct xfs_mount	*mp)
{
	int			error;
	xfs_daddr_t		head_blk;
	xfs_daddr_t		tail_blk;
	struct xlog		*log = mp->m_log;

	memset(log, 0, sizeof(struct xlog));
	x.logBBsize = XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks);
	x.logBBstart = XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart);
	x.lbsize = BBSIZE;
	if (xfs_sb_version_hassector(&mp->m_sb))
		x.lbsize <<= (mp->m_sb.sb_logsectlog - BBSHIFT);

	log->l_dev = mp->m_logdev_targp;
	log->l_logBBsize = x.logBBsize;
	log->l_logBBstart = x.logBBstart;
	log->l_sectBBsize  = BTOBB(x.lbsize);
	log->l_mp = mp;
	if (xfs_sb_version_hassector(&mp->m_sb)) {
		log->l_sectbb_log = mp->m_sb.sb_logsectlog - BBSHIFT;
		ASSERT(log->l_sectbb_log <= mp->m_sectbb_log);
		/* for larger sector sizes, must have v2 or external log */
		ASSERT(log->l_sectbb_log == 0 ||
			log->l_logBBstart == 0 ||
			xfs_sb_version_haslogv2(&mp->m_sb));
		ASSERT(mp->m_sb.sb_logsectlog >= BBSHIFT);
	}
	log->l_sectbb_mask = (1 << log->l_sectbb_log) - 1;

	/*
	 * Find the log head and tail and alert the user to the situation if the
	 * log appears corrupted or contains data. In either case, we do not
	 * proceed past this point unless the user explicitly requests to zap
	 * the log.
	 */
	error = xlog_find_tail(log, &head_blk, &tail_blk);
	if (error) {
		do_warn(
		_("zero_log: cannot find log head/tail (xlog_find_tail=%d)\n"),
			error);
		if (!no_modify && !zap_log) {
			do_warn(_(
"ERROR: The log head and/or tail cannot be discovered. Attempt to mount the\n"
"filesystem to replay the log or use the -L option to destroy the log and\n"
"attempt a repair.\n"));
			exit(2);
		}
	} else {
		if (verbose) {
			do_log(
	_("zero_log: head block %" PRId64 " tail block %" PRId64 "\n"),
				head_blk, tail_blk);
		}
		if (head_blk != tail_blk) {
			if (!no_modify && zap_log) {
				do_warn(_(
"ALERT: The filesystem has valuable metadata changes in a log which is being\n"
"destroyed because the -L option was used.\n"));
			} else if (no_modify) {
				do_warn(_(
"ALERT: The filesystem has valuable metadata changes in a log which is being\n"
"ignored because the -n option was used.  Expect spurious inconsistencies\n"
"which may be resolved by first mounting the filesystem to replay the log.\n"));
			} else {
				do_warn(_(
"ERROR: The filesystem has valuable metadata changes in a log which needs to\n"
"be replayed.  Mount the filesystem to replay the log, and unmount it before\n"
"re-running xfs_repair.  If you are unable to mount the filesystem, then use\n"
"the -L option to destroy the log and attempt a repair.\n"
"Note that destroying the log may cause corruption -- please attempt a mount\n"
"of the filesystem before doing this.\n"));
				exit(2);
			}
		}
	}

	/*
	 * Only clear the log when explicitly requested. Doing so is unnecessary
	 * unless something is wrong. Further, this resets the current LSN of
	 * the filesystem and creates more work for repair of v5 superblock
	 * filesystems.
	 */
	if (!no_modify && zap_log) {
		libxfs_log_clear(log->l_dev, NULL,
			XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart),
			(xfs_extlen_t)XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks),
			&mp->m_sb.sb_uuid,
			xfs_sb_version_haslogv2(&mp->m_sb) ? 2 : 1,
			mp->m_sb.sb_logsunit, XLOG_FMT, XLOG_INIT_CYCLE, true);

		/* update the log data structure with new state */
		error = xlog_find_tail(log, &head_blk, &tail_blk);
		if (error || head_blk != tail_blk)
			do_error(_("failed to clear log"));
	}

	/* And we are now magically complete! */
	PROG_RPT_INC(prog_rpt_done[0], mp->m_sb.sb_logblocks);

	/*
	 * Finally, seed the max LSN from the current state of the log if this
	 * is a v5 filesystem.
	 */
	if (xfs_sb_version_hascrc(&mp->m_sb))
		libxfs_max_lsn = log->l_last_sync_lsn;
}

static bool
set_inobtcount(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Inode btree count feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (!xfs_sb_version_hasfinobt(&mp->m_sb)) {
		printf(
	_("Inode btree count feature requires free inode btree.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasinobtcounts(&mp->m_sb)) {
		printf(_("Filesystem already has inode btree counts.\n"));
		exit(0);
	}

	printf(_("Adding inode btree counts to filesystem.\n"));
	new_sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_INOBTCNT;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

static bool
set_bigtime(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Large timestamp feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasbigtime(&mp->m_sb)) {
		printf(_("Filesystem already supports large timestamps.\n"));
		exit(0);
	}

	printf(_("Adding large timestamp support to filesystem.\n"));
	new_sb->sb_features_incompat |= (XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR |
					 XFS_SB_FEAT_INCOMPAT_BIGTIME);
	return true;
}

/* Make sure we can actually upgrade this (v5) filesystem. */
static void
check_new_v5_geometry(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	struct xfs_sb		old_sb;
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno;
	xfs_ino_t		rootino;
	int			min_logblocks;
	int			error;

	/*
	 * Save the current superblock, then copy in the new one to do log size
	 * and root inode checks.
	 */
	memcpy(&old_sb, &mp->m_sb, sizeof(struct xfs_sb));
	memcpy(&mp->m_sb, new_sb, sizeof(struct xfs_sb));

	/* Do we have a big enough log? */
	min_logblocks = libxfs_log_calc_minimum_size(mp);
	if (old_sb.sb_logblocks < min_logblocks) {
		printf(
	_("Filesystem log too small to upgrade filesystem; need %u blocks, have %u.\n"),
				min_logblocks, old_sb.sb_logblocks);
		exit(0);
	}

	rootino = libxfs_ialloc_calc_rootino(mp, new_sb->sb_unit);
	if (old_sb.sb_rootino != rootino) {
		printf(
	_("Cannot upgrade filesystem, root inode (%llu) cannot be moved to %llu.\n"),
				(unsigned long long)old_sb.sb_rootino,
				(unsigned long long)rootino);
		exit(0);
	}

	/* Make sure we have enough space for per-AG reservations. */
	for_each_perag(mp, agno, pag) {
		struct xfs_trans	*tp;
		struct xfs_agf		*agf;
		struct xfs_buf		*agi_bp, *agf_bp;
		unsigned int		avail, agblocks;

		/*
		 * Create a dummy transaction so that we can load the AGI and
		 * AGF buffers in memory with the old fs geometry and pin them
		 * there while we try to make a per-AG reservation with the new
		 * geometry.
		 */
		error = -libxfs_trans_alloc_empty(mp, &tp);
		if (error)
			do_error(
	_("Cannot reserve resources for upgrade check, err=%d.\n"),
					error);

		error = -libxfs_ialloc_read_agi(mp, tp, agno, &agi_bp);
		if (error)
			do_error(
	_("Cannot read AGI %u for upgrade check, err=%d.\n"),
					agno, error);

		error = -libxfs_alloc_read_agf(mp, tp, agno, 0, &agf_bp);
		if (error)
			do_error(
	_("Cannot read AGF %u for upgrade check, err=%d.\n"),
					agno, error);
		agf = agf_bp->b_addr;
		agblocks = be32_to_cpu(agf->agf_length);

		error = -libxfs_ag_resv_init(pag, tp);
		if (error == ENOSPC) {
			printf(
	_("Not enough free space would remain in AG %u for metadata.\n"),
					agno);
			exit(0);
		}
		if (error)
			do_error(
	_("Error %d while checking AG %u space reservation.\n"),
					error, agno);

		/*
		 * Would we have at least 10% free space in this AG after
		 * making per-AG reservations?
		 */
		avail = pag->pagf_freeblks + pag->pagf_flcount;
		avail -= pag->pag_meta_resv.ar_reserved;
		avail -= pag->pag_rmapbt_resv.ar_asked;
		if (avail < agblocks / 10)
			printf(
	_("AG %u will be low on space after upgrade.\n"),
					agno);

		libxfs_ag_resv_free(pag);

		/*
		 * Mark the per-AG structure as uninitialized so that we don't
		 * trip over stale cached counters after the upgrade, and
		 * release all the resources.
		 */
		libxfs_trans_cancel(tp);
		pag->pagf_init = 0;
		pag->pagi_init = 0;
	}

	/*
	 * Put back the old superblock.
	 */
	memcpy(&mp->m_sb, &old_sb, sizeof(struct xfs_sb));
}

static bool
set_finobt(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Free inode btree feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasfinobt(&mp->m_sb)) {
		printf(_("Filesystem already supports free inode btrees.\n"));
		exit(0);
	}

	printf(_("Adding free inode btrees to filesystem.\n"));
	new_sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_FINOBT;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

static bool
set_reflink(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Reflink feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb)) {
		printf(_("Filesystem already supports reflink.\n"));
		exit(0);
	}

	printf(_("Adding reflink support to filesystem.\n"));
	new_sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_REFLINK;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

static bool
set_rmapbt(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Reverse mapping btree feature only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasreflink(&mp->m_sb)) {
		printf(
	_("Reverse mapping btrees cannot be added when reflink is enabled.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
		printf(_("Filesystem already supports reverse mapping btrees.\n"));
		exit(0);
	}

	printf(_("Adding reverse mapping btrees to filesystem.\n"));
	new_sb->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_RMAPBT;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;
	return true;
}

static xfs_ino_t doomed_rbmino = NULLFSINO;
static xfs_ino_t doomed_rsumino = NULLFSINO;
static xfs_ino_t doomed_uquotino = NULLFSINO;
static xfs_ino_t doomed_gquotino = NULLFSINO;
static xfs_ino_t doomed_pquotino = NULLFSINO;

bool
wipe_pre_metadir_file(
	xfs_ino_t	ino)
{
	if (ino == doomed_rbmino ||
	    ino == doomed_rsumino ||
	    ino == doomed_uquotino ||
	    ino == doomed_gquotino ||
	    ino == doomed_pquotino)
		return true;
	return false;
}

static bool
set_metadir(
	struct xfs_mount	*mp,
	struct xfs_sb		*new_sb)
{
	if (!xfs_sb_version_hascrc(&mp->m_sb)) {
		printf(
	_("Metadata directory trees only supported on V5 filesystems.\n"));
		exit(0);
	}

	if (xfs_sb_version_hasmetadir(&mp->m_sb)) {
		printf(_("Filesystem already supports metadata directory trees.\n"));
		exit(0);
	}

	printf(_("Adding metadata directory trees to filesystem.\n"));
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_METADIR;
	new_sb->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR;

	/* Blow out all the old metadata inodes; we'll rebuild in phase6. */
	new_sb->sb_metadirino = new_sb->sb_rootino + 1;
	doomed_rbmino = mp->m_sb.sb_rbmino;
	doomed_rsumino = mp->m_sb.sb_rsumino;
	doomed_uquotino = mp->m_sb.sb_uquotino;
	doomed_gquotino = mp->m_sb.sb_gquotino;
	doomed_pquotino = mp->m_sb.sb_pquotino;

	new_sb->sb_rbmino = NULLFSINO;
	new_sb->sb_rsumino = NULLFSINO;
	new_sb->sb_uquotino = NULLFSINO;
	new_sb->sb_gquotino = NULLFSINO;
	new_sb->sb_pquotino = NULLFSINO;

	/* Indicate that we need a rebuild. */
	need_metadir_inode = 1;
	need_rbmino = 1;
	need_rsumino = 1;
	have_uquotino = 0;
	have_gquotino = 0;
	have_pquotino = 0;
	return true;
}

/* Perform the user's requested upgrades on filesystem. */
static void
upgrade_filesystem(
	struct xfs_mount	*mp)
{
	struct xfs_sb		new_sb;
	struct xfs_buf		*bp;
	bool			dirty = false;
	int			error;

	memcpy(&new_sb, &mp->m_sb, sizeof(struct xfs_sb));

	if (add_inobtcount)
		dirty |= set_inobtcount(mp, &new_sb);
	if (add_bigtime)
		dirty |= set_bigtime(mp, &new_sb);
	if (add_finobt)
		dirty |= set_finobt(mp, &new_sb);
	if (add_reflink)
		dirty |= set_reflink(mp, &new_sb);
	if (add_rmapbt)
		dirty |= set_rmapbt(mp, &new_sb);
	if (add_metadir)
		dirty |= set_metadir(mp, &new_sb);
	if (!dirty)
		return;

	check_new_v5_geometry(mp, &new_sb);
	memcpy(&mp->m_sb, &new_sb, sizeof(struct xfs_sb));
	if (no_modify)
                return;

        bp = libxfs_getsb(mp);
        if (!bp || bp->b_error) {
                do_error(
	_("couldn't get superblock for feature upgrade, err=%d\n"),
                                bp ? bp->b_error : ENOMEM);
        } else {
                libxfs_sb_to_disk(bp->b_addr, &mp->m_sb);

                /*
		 * Write the primary super to disk immediately so that
		 * needsrepair will be set if repair doesn't complete.
		 */
                error = -libxfs_bwrite(bp);
                if (error)
                        do_error(
	_("filesystem feature upgrade failed, err=%d\n"),
                                        error);
        }
        if (bp)
                libxfs_buf_relse(bp);
}

/*
 * ok, at this point, the fs is mounted but the root inode may be
 * trashed and the ag headers haven't been checked.  So we have
 * a valid xfs_mount_t and superblock but that's about it.  That
 * means we can use macros that use mount/sb fields in calculations
 * but I/O or btree routines that depend on space maps or inode maps
 * being correct are verboten.
 */

void
phase2(
	struct xfs_mount	*mp,
	int			scan_threads)
{
	int			j;
	ino_tree_node_t		*ino_rec;

	/* now we can start using the buffer cache routines */
	set_mp(mp);

	/* Check whether this fs has internal or external log */
	if (mp->m_sb.sb_logstart == 0) {
		if (!x.logname)
			do_error(_("This filesystem has an external log.  "
				   "Specify log device with the -l option.\n"));

		do_log(_("Phase 2 - using external log on %s\n"), x.logname);
	} else
		do_log(_("Phase 2 - using internal log\n"));

	/* Zero log if applicable */
	do_log(_("        - zero log...\n"));

	set_progress_msg(PROG_FMT_ZERO_LOG, (uint64_t)mp->m_sb.sb_logblocks);
	zero_log(mp);
	print_final_rpt();

	do_log(_("        - scan filesystem freespace and inode maps...\n"));

	bad_ino_btree = 0;

	set_progress_msg(PROG_FMT_SCAN_AG, (uint64_t) glob_agcount);

	scan_ags(mp, scan_threads);

	print_final_rpt();

	/*
	 * make sure we know about the root inode chunk
	 */
	if ((ino_rec = find_inode_rec(mp, 0, mp->m_sb.sb_rootino)) == NULL)  {
		ASSERT(!xfs_sb_version_hasmetadir(&mp->m_sb) ||
		       mp->m_sb.sb_metadirino == mp->m_sb.sb_rootino + 1);
		ASSERT(xfs_sb_version_hasmetadir(&mp->m_sb) ||
		       (mp->m_sb.sb_rbmino == mp->m_sb.sb_rootino + 1 &&
			mp->m_sb.sb_rsumino == mp->m_sb.sb_rootino + 2));
		do_warn(_("root inode chunk not found\n"));

		/*
		 * mark the first 3 used, the rest are free
		 */
		ino_rec = set_inode_used_alloc(mp, 0,
				(xfs_agino_t) mp->m_sb.sb_rootino);
		set_inode_used(ino_rec, 1);
		set_inode_used(ino_rec, 2);

		for (j = 3; j < XFS_INODES_PER_CHUNK; j++)
			set_inode_free(ino_rec, j);

		/*
		 * also mark blocks
		 */
		set_bmap_ext(0, XFS_INO_TO_AGBNO(mp, mp->m_sb.sb_rootino),
			     M_IGEO(mp)->ialloc_blks, XR_E_INO);
	} else  {
		do_log(_("        - found root inode chunk\n"));

		/*
		 * blocks are marked, just make sure they're in use
		 */
		if (is_inode_free(ino_rec, 0))  {
			do_warn(_("root inode marked free, "));
			set_inode_used(ino_rec, 0);
			if (!no_modify)
				do_warn(_("correcting\n"));
			else
				do_warn(_("would correct\n"));
		}

		if (is_inode_free(ino_rec, 1))  {
			do_warn(_("realtime bitmap inode marked free, "));
			set_inode_used(ino_rec, 1);
			if (!no_modify)
				do_warn(_("correcting\n"));
			else
				do_warn(_("would correct\n"));
		}

		if (is_inode_free(ino_rec, 2))  {
			do_warn(_("realtime summary inode marked free, "));
			set_inode_used(ino_rec, 2);
			if (!no_modify)
				do_warn(_("correcting\n"));
			else
				do_warn(_("would correct\n"));
		}
	}

	/*
	 * Upgrade the filesystem now that we've done a preliminary check of
	 * the superblocks, the AGs, the log, and the metadata inodes.
	 */
	upgrade_filesystem(mp);
}
