// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "threads.h"
#include "prefetch.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "protos.h"
#include "err_protos.h"
#include "dinode.h"
#include "progress.h"
#include "bmap.h"
#include "threads.h"
#include "xfs/xfs_types.h"

static void
process_agi_unlinked(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno)
{
	struct xfs_buf		*bp;
	struct xfs_agi		*agip;
	xfs_agnumber_t		i;
	int			agi_dirty = 0;
	int			error;

	error = -libxfs_buf_read(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, LIBXFS_B_SALVAGE,
			&bp, &xfs_agi_buf_ops);
	if (error)
		do_error(_("cannot read agi block %" PRId64 " for ag %u\n"),
			XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)), agno);

	agip = bp->b_addr;

	ASSERT(be32_to_cpu(agip->agi_seqno) == agno);

	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++)  {
		if (agip->agi_unlinked[i] != cpu_to_be32(NULLAGINO)) {
			agip->agi_unlinked[i] = cpu_to_be32(NULLAGINO);
			agi_dirty = 1;
		}
	}

	if (agi_dirty) {
		libxfs_buf_mark_dirty(bp);
		libxfs_buf_relse(bp);
	}
	else
		libxfs_buf_relse(bp);
}

static void
process_ag_func(
	struct work		*work)
{
	struct prefetch_args_t	*pf_args;
	struct xfs_mount	*mp;
	xfs_agnumber_t		agno;

	pf_args = container_of(work, struct prefetch_args, work);
	mp = pf_args->mp;
	agno = pf_args->agno;

	/*
	 * turn on directory processing (inode discovery) and
	 * attribute processing (extra_attr_check)
	 */
	wait_for_inode_prefetch(pf_args);
	do_log(_("        - agno = %d\n"), agno);
	process_aginodes(mp, pf_args, agno, 1, 0, 1);
	blkmap_free_final();
	cleanup_inode_prefetch(pf_args);
}

static void
process_ags(
	xfs_mount_t		*mp)
{
	do_inode_prefetch(mp, ag_stride, process_ag_func, false, false);
}

struct uncertain_aginodes_arg {
	struct work		work;
	struct xfs_mount	*mp;
	xfs_agnumber_t		agno;
	int			count;
};

static void
do_uncertain_aginodes(
	struct work			*work)
{
	struct uncertain_aginodes_arg	*arg;

	arg = container_of(work, struct uncertain_aginodes_arg, work);

	arg->count = process_uncertain_aginodes(arg->mp, arg->agno);

#ifdef XR_INODE_TRACE
	fprintf(stderr,
		"\t\t phase 3 - ag %d process_uncertain_inodes returns %d\n",
		*count, j);
#endif

	PROG_RPT_INC(prog_rpt_done[agno], 1);
}

void
phase3(
	struct xfs_mount		*mp,
	int				scan_threads)
{
	struct uncertain_aginodes_arg	*args;
	int				i, j;
	struct workqueue		wq;

	do_log(_("Phase 3 - for each AG...\n"));
	if (!no_modify)
		do_log(_("        - scan and clear agi unlinked lists...\n"));
	else
		do_log(_("        - scan (but don't clear) agi unlinked lists...\n"));

	set_progress_msg(PROG_FMT_AGI_UNLINKED, (uint64_t) glob_agcount);

	/* first clear the agi unlinked AGI list */
	if (!no_modify) {
		for (i = 0; i < mp->m_sb.sb_agcount; i++)
			process_agi_unlinked(mp, i);
	}

	/* now look at possibly bogus inodes */
	for (i = 0; i < mp->m_sb.sb_agcount; i++)  {
		check_uncertain_aginodes(mp, i);
		PROG_RPT_INC(prog_rpt_done[i], 1);
	}
	print_final_rpt();

	/* ok, now that the tree's ok, let's take a good look */

	do_log(_(
	    "        - process known inodes and perform inode discovery...\n"));

	set_progress_msg(PROG_FMT_PROCESS_INO, (uint64_t) mp->m_sb.sb_icount);

	process_ags(mp);

	print_final_rpt();

	/*
	 * process newly discovered inode chunks
	 */
	do_log(_("        - process newly discovered inodes...\n"));
	set_progress_msg(PROG_FMT_NEW_INODES, (uint64_t) glob_agcount);

	args = malloc(mp->m_sb.sb_agcount * sizeof(*args));
	if (!args) {
		do_abort(_("no memory for uncertain inode counts\n"));
		return;
	}

	do  {
		/*
		 * have to loop until no ag has any uncertain
		 * inodes
		 */
		j = 0;
		memset(args, 0, mp->m_sb.sb_agcount * sizeof(*args));

		create_work_queue(&wq, mp, scan_threads);

		for (i = 0; i < mp->m_sb.sb_agcount; i++) {
			INIT_WORK(&args[i].work, do_uncertain_aginodes);
			args[i].mp = mp;
			args[i].agno = i;
			queue_work(&wq, &args[i].work);
		}

		destroy_workqueue(&wq);

		/* tally up the counts */
		for (i = 0; i < mp->m_sb.sb_agcount; i++)
			j += args[i].count;

	} while (j != 0);

	free(args);

	print_final_rpt();
}
