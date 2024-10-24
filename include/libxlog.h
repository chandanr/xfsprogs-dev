// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.All Rights Reserved.
 */
#ifndef LIBXLOG_H
#define LIBXLOG_H

#include "libxlog_api_defs.h"
#include "platform_defs.h"
#include "ksemaphore.h"
#include "libfrog/pseudo_percpu.h"
#include "xfs.h"

#define memalloc_nofs_save() (0);
#define memalloc_nofs_restore(a)

#define lockdep_assert_held(a)

/*
 * TODO: chandan: Can we get cpp to eliminate references to current
 * entirely?
 */
struct task_struct {
	void *journal_info;
};

static struct task_struct ts;
static struct task_struct *current = &ts;

#include "xfs_trans.h"
#include "xfs_trans_quota.h"

#include "xfs_inode_item.h"
#include "xfs_buf_item.h"

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


#endif	/* LIBXLOG_H */
