// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.All Rights Reserved.
 */
#ifndef LIBXLOG_H
#define LIBXLOG_H

#include "libxlog_api_defs.h"
#include "platform_defs.h"
#include "xfs.h"

#include "sem.h"
#include "libfrog/pseudo_percpu.h"
#include "libfrog/refcount.h"

enum libxlog_init_phase {
	LIBXLOG_INIT_PHASE_1,
	LIBXLOG_INIT_PHASE_2,
};

struct xfs_kobj {
	;
};

/*
 * Allocate a transaction that can be rolled.  Since userspace doesn't have
 * a need for log reservations, we really only tr_itruncate to get the
 * permanent log reservation flag to avoid blowing asserts.
 */
static inline int
xfs_trans_alloc_rollable(
	struct xfs_mount	*mp,
	unsigned int		blocks,
	struct xfs_trans	**tpp)
{
	return libxlog_trans_alloc(mp, &M_RES(mp)->tr_itruncate, blocks,
			0, 0, tpp);
}

int libxlog_mount(struct xfs_mount *mp, struct xfs_sb *sbp,
		struct xfs_buftarg *log_target, enum libxlog_init_phase phase);

#endif	/* LIBXLOG_H */
