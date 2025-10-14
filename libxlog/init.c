#include "generic_headers.h"

#include "platform_defs.h"

#include "kernel_types.h"
#include "kernel_misc_stage1.h"

#include "xfsprogs_helpers.h"

/* Header files from libfrog/ */
#include "libfrog/refcount.h"
#include "libfrog/radix-tree.h"
#include "libfrog/rbtree.h"
#include "libfrog/crc32c.h"
#include "libfrog/bio.h"
#include "libfrog/pseudo_percpu.h"
#include "libfrog/schedule.h"
#include "libfrog/waitqueue.h"
#include "libfrog/workqueue.h"
#include "libfrog/delayed-work.h"

/* chandan: xfs/xfs_types.h declares xfs_verify_*() */
#include "libxfs_api_defs.h"
#include "libxlog_api_defs.h"

/* XFS header files from xfsprogs/include/ */
#include "xfs.h"
#include "xfs_arch.h"

#include "kernel_misc_stage2.h"

/* Header files from libxlog/ */
#include "libxlog_priv.h"

#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_shared.h"
#include "xfs_mount.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_log.h"

#include "libxlog.h"

int
libxlog_mount(
	struct xfs_mount	*mp,
	struct xfs_sb		*sbp,
	struct xfs_buftarg	*log_target,
	enum libxlog_init_phase	phase)
{
	int			error;

	switch (phase) {
	case LIBXLOG_INIT_PHASE_1:
		error = xfs_log_mount(mp, log_target,
				XFS_FSB_TO_DADDR(mp, sbp->sb_logstart),
				XFS_FSB_TO_BB(mp, sbp->sb_logblocks));
		if (error) {
			fprintf(stderr, _("%s: Log initialization failed\n"),
					progname);
			return error;
		}

		break;

	case LIBXLOG_INIT_PHASE_2:
		error = xfs_log_mount_finish(mp);
		if (error) {
			xfs_warn(mp, "log mount finish failed");
			return error;
		}

		break;

	default:
		ASSERT(0);
	}

	return 0;
}
