// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <urcu.h>
#include "workqueue.h"

/* Main processing thread */
static void *
workqueue_thread(
	void			*arg)
{
	struct workqueue	*wq = arg;
	struct work_struct	*work;

	/*
	 * Loop pulling work from the passed in work queue.
	 * Check for notification to exit after every chunk of work.
	 */
	rcu_register_thread();
	pthread_mutex_lock(&wq->lock);
	while (1) {

		/*
		 * Wait for work.
		 */
		while (wq->next_item == NULL && !wq->terminate) {
			assert(wq->item_count == 0);
			pthread_cond_wait(&wq->wakeup, &wq->lock);
		}
		if (wq->next_item == NULL && wq->terminate)
			break;

		/*
		 *  Dequeue work from the head of the list. If the queue was
		 *  full then send a wakeup if we're configured to do so.
		 */
		assert(wq->item_count > 0);
		if (wq->max_queued)
			pthread_cond_broadcast(&wq->queue_full);

		work = wq->next_item;
		wq->next_item = work->next;
		wq->item_count--;

		if (wq->max_queued && wq->next_item) {
			/* more work, wake up another worker */
			pthread_cond_signal(&wq->wakeup);
		}

		if (wq->item_count == 0)
			pthread_cond_signal(&wq->queue_empty);

		wq->active_threads++;
		pthread_mutex_unlock(&wq->lock);

		(work->function)(work);

		pthread_mutex_lock(&wq->lock);
		wq->active_threads--;
	}
	pthread_mutex_unlock(&wq->lock);
	rcu_unregister_thread();

	return NULL;
}

/* Allocate a work queue and threads.  Returns zero or negative error code. */
int
workqueue_create_bound(
	struct workqueue	*wq,
	void			*wq_ctx,
	unsigned int		nr_workers,
	unsigned int		max_queue)
{
	unsigned int		i;
	int			err = 0;

	memset(wq, 0, sizeof(*wq));
	err = -pthread_cond_init(&wq->wakeup, NULL);
	if (err)
		return err;
	err = -pthread_cond_init(&wq->queue_full, NULL);
	if (err)
		goto out_wake;
	err = -pthread_cond_init(&wq->queue_empty, NULL);
	if (err)
		goto out_queue_full;
	err = -pthread_mutex_init(&wq->lock, NULL);
	if (err)
		goto out_queue_empty;

	wq->wq_ctx = wq_ctx;
	wq->thread_count = nr_workers;
	wq->max_queued = max_queue;
	wq->threads = malloc(nr_workers * sizeof(pthread_t));
	if (!wq->threads) {
		err = -errno;
		goto out_mutex;
	}
	wq->terminate = false;
	wq->terminated = false;

	for (i = 0; i < nr_workers; i++) {
		err = -pthread_create(&wq->threads[i], NULL, workqueue_thread,
				wq);
		if (err)
			break;
	}

	/*
	 * If we encounter errors here, we have to signal and then wait for all
	 * the threads that may have been started running before we can destroy
	 * the workqueue.
	 */
	if (err)
		workqueue_destroy(wq);
	return err;
out_mutex:
	pthread_mutex_destroy(&wq->lock);
out_queue_empty:
	pthread_cond_destroy(&wq->queue_empty);
out_queue_full:
	pthread_cond_destroy(&wq->queue_full);
out_wake:
	pthread_cond_destroy(&wq->wakeup);
	return err;
}

int
workqueue_create(
	struct workqueue	*wq,
	void			*wq_ctx,
	unsigned int		nr_workers)
{
	return workqueue_create_bound(wq, wq_ctx, nr_workers, 0);
}

struct workqueue *
alloc_workqueue(
	const char		*fmt,
	unsigned int		flags,
	int			max_active,
	...)
{
	struct workqueue	*wq;
	int			error;

	wq = calloc(1, sizeof(*wq));
	if (!wq)
		return NULL;

	error = workqueue_create(wq, NULL, 1);
	if (error) {
		free(wq);
		return NULL;
	}

	return wq;
}

/*
 * Create a work item consisting of a function and some arguments and schedule
 * the work item to be run via the thread pool.  Returns zero or a negative
 * error code.
 */
void
queue_work(
	struct workqueue	*wq,
	struct work_struct	*work)
{
	int			ret;

	assert(!wq->terminated);

	if (wq->thread_count == 0) {
		work->function(work);
		return;
	}

	work->next = NULL;

	/* Now queue the new work structure to the work queue. */
	pthread_mutex_lock(&wq->lock);
restart:
	if (wq->next_item == NULL) {
		assert(wq->item_count == 0);
		wq->next_item = work;
	} else {
		/* throttle on a full queue if configured */
		if (wq->max_queued && wq->item_count == wq->max_queued) {
			pthread_cond_wait(&wq->queue_full, &wq->lock);
			/*
			 * Queue might be empty or even still full by the time
			 * we get the lock back, so restart the lookup so we do
			 * the right thing with the current state of the queue.
			 */
			goto restart;
		}
		wq->last_item->next = work;
	}
	wq->last_item = work;
	wq->item_count++;

	if (wq->active_threads == wq->thread_count - 1) {
		/* One thread is idle, wake it */
		ret = -pthread_cond_signal(&wq->wakeup);
		if (ret) {
			pthread_mutex_unlock(&wq->lock);
			assert(0);
			return;
		}
	} else if (wq->active_threads < wq->thread_count) {
		/* Multiple threads are idle, wake everyone */
		ret = -pthread_cond_broadcast(&wq->wakeup);
		if (ret) {
			pthread_mutex_unlock(&wq->lock);
			assert(0);
			return;
		}
	}

	pthread_mutex_unlock(&wq->lock);
}

void
flush_workqueue(
	struct workqueue	*wq)
{
	pthread_mutex_lock(&wq->lock);

	if (wq->item_count == 0)
		goto out;

	pthread_cond_wait(&wq->queue_empty, &wq->lock);

out:
	pthread_mutex_unlock(&wq->lock);
}

/*
 * Wait for all pending work items to be processed and tear down the
 * workqueue thread pool.  Returns zero or a negative error code.
 */
int
workqueue_terminate(
	struct workqueue	*wq)
{
	unsigned int		i;
	int			ret;

	pthread_mutex_lock(&wq->lock);
	wq->terminate = true;
	pthread_mutex_unlock(&wq->lock);

	ret = -pthread_cond_broadcast(&wq->wakeup);
	if (ret)
		return ret;

	for (i = 0; i < wq->thread_count; i++) {
		ret = -pthread_join(wq->threads[i], NULL);
		if (ret)
			return ret;
	}

	pthread_mutex_lock(&wq->lock);
	wq->terminated = true;
	pthread_mutex_unlock(&wq->lock);

	return 0;
}

/* Tear down the workqueue. */
void
workqueue_destroy(
	struct workqueue	*wq)
{
	assert(wq->terminated);

	free(wq->threads);
	pthread_mutex_destroy(&wq->lock);
	pthread_cond_destroy(&wq->wakeup);
	pthread_cond_destroy(&wq->queue_full);
	memset(wq, 0, sizeof(*wq));
}

void
destroy_workqueue(
	struct workqueue	*wq)
{
	int			err;

	err = -workqueue_terminate(wq);
	if (err)
		assert(0);

	workqueue_destroy(wq);
}
