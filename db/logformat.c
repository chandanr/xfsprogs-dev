// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "init.h"
#include "output.h"
#include "libxlog.h"
#include "logformat.h"

#define MAX_LSUNIT	256 * 1024	/* max log buf. size */

static int
logformat_f(int argc, char **argv)
{
	xfs_daddr_t	head_blk;
	xfs_daddr_t	tail_blk;
	int		logversion;
	int		lsunit = -1;
	int		cycle = -1;
	int		error;
	int		c;

	logversion = xfs_has_logv2(mp) ? 2 : 1;

	while ((c = getopt(argc, argv, "c:s:")) != EOF) {
		switch (c) {
		case 'c':
			cycle = strtol(optarg, NULL, 0);
			if (cycle == 0) {
				dbprintf("invalid cycle\n");
				return -1;
			}
			break;
		case 's':
			lsunit = strtol(optarg, NULL, 0);
			/*
			 * The log stripe unit must be block aligned and no
			 * larger than 256k.
			 */
			if (lsunit > 1 &&
			    (lsunit % mp->m_sb.sb_blocksize ||
			    (logversion == 2 && lsunit > MAX_LSUNIT))) {
				dbprintf("invalid log stripe unit\n");
				return -1;
			}
			break;
		default:
			dbprintf("invalid option\n");
			return -1;
		}
	}

	/*
	 * Check whether the log is dirty. This also determines the current log
	 * cycle if we have to use it by default below.
	 */
	memset(mp->m_log, 0, sizeof(struct xlog));
	mp->m_log->l_mp = mp;
	mp->m_log->l_dev = mp->m_logdev_targp;
	mp->m_log->l_logBBsize = XFS_FSB_TO_BB(mp, mp->m_sb.sb_logblocks);
	mp->m_log->l_logBBstart = XFS_FSB_TO_DADDR(mp, mp->m_sb.sb_logstart);
	mp->m_log->l_sectBBsize = BBSIZE;
	if (xfs_has_sector(mp))
		mp->m_log->l_sectBBsize <<= (mp->m_sb.sb_logsectlog - BBSHIFT);
	mp->m_log->l_sectBBsize = BTOBB(mp->m_log->l_sectBBsize);

	error = xlog_find_tail(mp->m_log, &head_blk, &tail_blk);
	if (error) {
		dbprintf("could not find log head/tail\n");
		return -1;
	}
	if (head_blk != tail_blk) {
		dbprintf(_(
			"The log is dirty. Please mount to replay the log.\n"));
		return -1;
	}

	/*
	 * Use the current cycle and/or log stripe unit if either is not
	 * provided by the user.
	 */
	if (cycle < 0)
		cycle = mp->m_log->l_curr_cycle;
	if (lsunit < 0)
		lsunit = mp->m_sb.sb_logsunit;

	dbprintf("Formatting the log to cycle %d, stripe unit %d bytes.\n",
		 cycle, lsunit);
	error = -libxfs_log_clear(mp->m_logdev_targp, NULL,
				 mp->m_log->l_logBBstart,
				 mp->m_log->l_logBBsize,
				 &mp->m_sb.sb_uuid, logversion, lsunit,
				 XLOG_FMT, cycle, false);
	if (error) {
		dbprintf("error formatting log - %d\n", error);
		return error;
	}

	return 0;
}

static void
logformat_help(void)
{
	dbprintf(_(
"\n"
" The 'logformat' command reformats (clears) the log to the specified log\n"
" cycle and log stripe unit. If the log cycle is not specified, the log is\n"
" reformatted to the current cycle. If the log stripe unit is not specified,\n"
" the stripe unit from the filesystem superblock is used.\n"
"\n"
	));
}

static const struct cmdinfo logformat_cmd = {
	.name =		"logformat",
	.altname =	NULL,
	.cfunc =	logformat_f,
	.argmin =	0,
	.argmax =	4,
	.canpush =	0,
	.args =		N_("[-c cycle] [-s sunit]"),
	.oneline =	N_("reformat the log"),
	.help =		logformat_help,
};

void
logformat_init(void)
{
	if (!expert_mode)
		return;

	add_command(&logformat_cmd);
}

static void
print_logres(
	int			i,
	struct xfs_trans_res	*res)
{
	dbprintf(_("type %d logres %u logcount %d flags 0x%x\n"),
		i, res->tr_logres, res->tr_logcount, res->tr_logflags);
}

static int
logres_f(
	int			argc,
	char			**argv)
{
	struct xfs_trans_res	resv;
	struct xfs_trans_res	*res;
	struct xfs_trans_res	*end_res;
	int			i;

	res = (struct xfs_trans_res *)M_RES(mp);
	end_res = (struct xfs_trans_res *)(M_RES(mp) + 1);
	for (i = 0; res < end_res; i++, res++)
		print_logres(i, res);

	libxfs_log_get_max_trans_res(mp, &resv);
	dbprintf(_("minlogsize logres %u logcount %d\n"),
			resv.tr_logres, resv.tr_logcount);

	return 0;
}

static void
logres_help(void)
{
	dbprintf(_(
"\n"
" The 'logres' command prints information about all log reservation types.\n"
" This includes the reservation space, the intended transaction roll count,\n"
" and the reservation flags, if any.\n"
"\n"
	));
}

static const struct cmdinfo logres_cmd = {
	.name =		"logres",
	.altname =	NULL,
	.cfunc =	logres_f,
	.argmin =	0,
	.argmax =	0,
	.canpush =	0,
	.args =		NULL,
	.oneline =	N_("dump log reservations"),
	.help =		logres_help,
};

STATIC void
untorn_cow_limits(
	struct xfs_mount	*mp,
	unsigned int		logres,
	unsigned int		desired_max)
{
	const unsigned int	efi = xfs_efi_log_space(1);
	const unsigned int	efd = xfs_efd_log_space(1);
	const unsigned int	rui = xfs_rui_log_space(1);
	const unsigned int	rud = xfs_rud_log_space();
	const unsigned int	cui = xfs_cui_log_space(1);
	const unsigned int	cud = xfs_cud_log_space();
	const unsigned int	bui = xfs_bui_log_space(1);
	const unsigned int	bud = xfs_bud_log_space();

	/*
	 * Maximum overhead to complete an untorn write ioend in software:
	 * remove data fork extent + remove cow fork extent + map extent into
	 * data fork.
	 *
	 * tx0: Creates a BUI and a CUI and that's all it needs.
	 *
	 * tx1: Roll to finish the BUI.  Need space for the BUD, an RUI, and
	 * enough space to relog the CUI (== CUI + CUD).
	 *
	 * tx2: Roll again to finish the RUI.  Need space for the RUD and space
	 * to relog the CUI.
	 *
	 * tx3: Roll again, need space for the CUD and possibly a new EFI.
	 *
	 * tx4: Roll again, need space for an EFD.
	 *
	 * If the extent referenced by the pair of BUI/CUI items is not the one
	 * being currently processed, then we need to reserve space to relog
	 * both items.
	 */
	const unsigned int	tx0 = bui + cui;
	const unsigned int	tx1 = bud + rui + cui + cud;
	const unsigned int	tx2 = rud + cui + cud;
	const unsigned int	tx3 = cud + efi;
	const unsigned int	tx4 = efd;
	const unsigned int	relog = bui + bud + cui + cud;

	const unsigned int	per_intent = max(max3(tx0, tx1, tx2),
						 max3(tx3, tx4, relog));

	/* Overhead to finish one step of each intent item type */
	const unsigned int	f1 = libxfs_calc_finish_efi_reservation(mp, 1);
	const unsigned int	f2 = libxfs_calc_finish_rui_reservation(mp, 1);
	const unsigned int	f3 = libxfs_calc_finish_cui_reservation(mp, 1);
	const unsigned int	f4 = libxfs_calc_finish_bui_reservation(mp, 1);

	/* We only finish one item per transaction in a chain */
	const unsigned int	step_size = max(f4, max3(f1, f2, f3));

	if (desired_max) {
		dbprintf(
 "desired_max: %u\nstep_size: %u\nper_intent: %u\nlogres: %u\n",
				desired_max, step_size, per_intent,
				(desired_max * per_intent) + step_size);
	} else if (logres) {
		dbprintf(
 "logres: %u\nstep_size: %u\nper_intent: %u\nmax_awu: %u\n",
				logres, step_size, per_intent,
				logres >= step_size ? (logres - step_size) / per_intent : 0);
	}
}

static void
untorn_write_max_help(void)
{
	dbprintf(_(
"\n"
" The 'untorn_write_max' command computes either the log reservation needed to\n"
" complete an untorn write of a given block count; or the maximum number of\n"
" blocks that can be completed given a specific log reservation.\n"
"\n"
	));
}

static int
untorn_write_max_f(
	int		argc,
	char		**argv)
{
	unsigned int	logres = 0;
	unsigned int	desired_max = 0;
	int		c;

	while ((c = getopt(argc, argv, "l:b:")) != EOF) {
		switch (c) {
		case 'l':
			logres = atoi(optarg);
			break;
		case 'b':
			desired_max = atoi(optarg);
			break;
		default:
			untorn_write_max_help();
			return 0;
		}
	}

	if (!logres && !desired_max) {
		dbprintf("untorn_write_max needs -l or -b option\n");
		return 0;
	}

	if (xfs_has_reflink(mp))
		untorn_cow_limits(mp, logres, desired_max);
	else
		dbprintf("untorn write emulation not supported\n");

	return 0;
}

static const struct cmdinfo untorn_write_max_cmd = {
	.name =		"untorn_write_max",
	.altname =	NULL,
	.cfunc =	untorn_write_max_f,
	.argmin =	0,
	.argmax =	-1,
	.canpush =	0,
	.args =		NULL,
	.oneline =	N_("compute untorn write max"),
	.help =		logres_help,
};

void
logres_init(void)
{
	add_command(&logres_cmd);
	add_command(&untorn_write_max_cmd);
}
