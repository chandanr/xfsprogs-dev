// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#ifndef	__LIBFROG_WORKQUEUE_H__
#define	__LIBFROG_WORKQUEUE_H__

#include <pthread.h>

struct workqueue;
struct work_struct;

typedef void workqueue_func_t(struct work_struct *);

struct work_struct {
	struct work_struct	*next;
	workqueue_func_t	*function;
};

#define INIT_WORK(work, func)			\
	do {					\
		(work)->function = func;	\
	} while(0)

#define destroy_workqueue workqueue_destroy

struct workqueue {
	void			*wq_ctx;
	pthread_t		*threads;
	struct work_struct	*next_item;
	struct work_struct	*last_item;
	pthread_mutex_t		lock;
	pthread_cond_t		wakeup;
	unsigned int		item_count;
	unsigned int		thread_count;
	unsigned int		active_threads;
	bool			terminate;
	bool			terminated;
	int			max_queued;
	pthread_cond_t		queue_full;
	pthread_cond_t		queue_empty;
};

struct workqueue *alloc_workqueue(const char *fmt, unsigned int flags,
		int max_active, ...);
int workqueue_create(struct workqueue *wq, void *wq_ctx,
		unsigned int nr_workers);
int workqueue_create_bound(struct workqueue *wq, void *wq_ctx,
		unsigned int nr_workers, unsigned int max_queue);
void queue_work(struct workqueue *wq, struct work_struct *work);
void flush_workqueue(struct workqueue *wq);
int workqueue_terminate(struct workqueue *wq);
void workqueue_destroy(struct workqueue *wq);

#endif	/* __LIBFROG_WORKQUEUE_H__ */
