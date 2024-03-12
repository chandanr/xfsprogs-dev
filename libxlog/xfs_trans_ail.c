#include "xfs_trans.h"
#include "kmem.h"
#include "spinlock.h"

int
xfs_trans_ail_init(
	xfs_mount_t	*mp)
{
	struct xfs_ail	*ailp;

	ailp = kmem_zalloc(sizeof(struct xfs_ail), KM_MAYFAIL);
	if (!ailp)
		return -ENOMEM;

	ailp->ail_log = mp->m_log;
	INIT_LIST_HEAD(&ailp->ail_head);
	INIT_LIST_HEAD(&ailp->ail_cursors);
	spin_lock_init(&ailp->ail_lock);
	INIT_LIST_HEAD(&ailp->ail_buf_list);
	init_waitqueue_head(&ailp->ail_empty);

	ailp->ail_task = kthread_run(xfsaild, ailp, "xfsaild/%s",
				mp->m_super->s_id);
	if (IS_ERR(ailp->ail_task))
		goto out_free_ailp;

	mp->m_ail = ailp;
	return 0;

out_free_ailp:
	kmem_free(ailp);
	return -ENOMEM;
}
