// SPDX-License-Identifier: GPL-2.0

#include "libxfs.h"
#include <pthread.h>
#include <signal.h>
#include "threads.h"
#include "err_protos.h"
#include "protos.h"
#include "globals.h"

void
thread_init(void)
{
	sigset_t	blocked;

	/*
	 *  block delivery of progress report signal to all threads
         */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGHUP);
	sigaddset(&blocked, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &blocked, NULL);
}


void
create_work_queue(
	struct workqueue	*wq,
	struct xfs_mount	*mp,
	unsigned int		nworkers)
{
	int			err;

	err = -workqueue_create(wq, mp, nworkers);
	if (err)
		do_error(_("cannot create worker threads, error = [%d] %s\n"),
				err, strerror(err));
}
