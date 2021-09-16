// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 RedHat, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBXFS_COMPLETION_H__
#define __LIBXFS_COMPLETION_H__

/*
 * This implements kernel compatible completion semantics. This is slightly
 * different to the way pthread conditional variables work in that completions
 * can be signalled before the waiter tries to wait on the variable. In the
 * pthread case, the completion is ignored and the waiter goes to sleep, whilst
 * the kernel will see that the completion has already been completed and so
 * will not block. This is handled through the addition of the the @signalled
 * flag in the struct completion.
 */
struct completion {
	pthread_mutex_t		lock;
	pthread_cond_t		cond;
	bool			signalled; /* for kernel completion behaviour */
	int			waiters;
};

static inline void
init_completion(struct completion *w)
{
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->cond, NULL);
	w->signalled = false;
}

static inline void
complete(struct completion *w)
{
	pthread_mutex_lock(&w->lock);
	w->signalled = true;
	pthread_cond_broadcast(&w->cond);
	pthread_mutex_unlock(&w->lock);
}

/*
 * Support for mulitple waiters requires that we count the number of waiters
 * we have and only clear the signalled variable once all those waiters have
 * been woken.
 */
static inline void
wait_for_completion(struct completion *w)
{
	pthread_mutex_lock(&w->lock);
	if (!w->signalled) {
		w->waiters++;
		pthread_cond_wait(&w->cond, &w->lock);
		w->waiters--;
	}
	if (!w->waiters)
		w->signalled = false;
	pthread_mutex_unlock(&w->lock);
}

#endif /* __LIBXFS_COMPLETION_H__ */
