#include "libxlog_priv.h"
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
