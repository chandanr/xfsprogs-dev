// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.All Rights Reserved.
 */
#ifndef LIBXLOG_H
#define LIBXLOG_H

#include "libxlog_api_defs.h"
#include "platform_defs.h"
#include "libfrog/pseudo_percpu.h"
#include "libfrog/refcount.h"
#include "xfs.h"

#include "sem.h"
#define memalloc_nofs_save() (0);
#define memalloc_nofs_restore(a)

#define memalloc_noreclaim_save() (0)
#define memalloc_noreclaim_restore(a) \
	((a) = (a));

#define lockdep_assert_held(a)

#include "xfs_trans.h"
#include "xfs_trans_quota.h"

#include "xfs_inode_item.h"
#include "xfs_buf_item.h"

enum libxlog_init_phase {
	LIBXLOG_INIT_PHASE_1,
	LIBXLOG_INIT_PHASE_2,
};

struct xfs_kobj {
	;
};

#define smp_rmb()

/* TODO: chandan: implement waitqueues */
#define wait_var_event(var, cond)
#define DECLARE_WAITQUEUE(wait, current)
#define add_wait_queue_exclusive(wq, waitp)
#define remove_wait_queue(wq, waitp)
#define __set_current_state(state)
#define schedule()

/*
 * #include "xfs_attr_item.h"
 * #include "xfs_bmap_item.h"
 * #include "xfs_dquot_item.h"
 * #include "xfs_extfree_item.h"
 * #include "xfs_icreate_item.h"
 * #include "xfs_iunlink_item.h"
 * #include "xfs_refcount_item.h"
 * #include "xfs_rmap_item.h"
 */

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
