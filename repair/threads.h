// SPDX-License-Identifier: GPL-2.0

#ifndef	_XFS_REPAIR_THREADS_H_
#define	_XFS_REPAIR_THREADS_H_

#include "libfrog/workqueue.h"

void	thread_init(void);

void
create_work_queue(
	struct workqueue	*wq,
	struct xfs_mount	*mp,
	unsigned int		nworkers);

#endif	/* _XFS_REPAIR_THREADS_H_ */
