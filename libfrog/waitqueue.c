#include "waitqueue.h"
#include "list.h"
#include <bits/pthreadtypes.h>
#include <pthread.h>
#include <stdint.h>

void
init_waitqueue_head(
	struct wait_queue_head	*wq_head)
{
	int error;

	error = pthread_mutex_init(&wq_head->cond_mutex, NULL);
	ASSERT(error == 0);

	error = pthread_cond_init(&wq_head->cond, NULL);
	ASSERT(error == 0);

	error = pthread_mutex_init(&wq_head->list_mutex, NULL);
	ASSERT(error == 0);

	INIT_LIST_HEAD(&wq_head->head);
}

void
prepare_to_wait(
	struct wait_queue_head	*wq_head,
	struct wait_queue_entry *wq_entry,
	int			state)
{
	int			error;

	error = pthread_mutex_lock(&wq_head->list_mutex);
	ASSERT(error == 0);

	if (list_empty(&wq_entry->entry)) {
		list_add(&wq_entry->entry, &wq_head->head);
		wq_entry->task->wakeup = false;
	}

	error = pthread_mutex_unlock(&wq_head->list_mutex);
	ASSERT(error == 0);
}

void
finish_wait(
	struct wait_queue_head	*wq_head,
	struct wait_queue_entry *wq_entry)
{
	int			error;

	error = pthread_mutex_lock(&wq_head->list_mutex);
	ASSERT(error == 0);

	if (!list_empty_careful(&wq_entry->entry))
		list_del_init(&wq_entry->entry);

	error = pthread_mutex_unlock(&wq_head->list_mutex);
	ASSERT(error == 0);
}

int
wake_up_process(
	struct task_struct	*ts,
	pthread_cond_t		*cond)
{
	ts->wakeup = true;
	return pthread_cond_broadcast(cond);
}

int
wake_up(
	struct wait_queue_head	*wq)
{
	struct wait_queue_entry *cur;
	int			error;

	error = pthread_mutex_lock(&wq_head->list_mutex);
	ASSERT(error == 0);

	list_for_each_entry(cur, &wq->head, entry) {
		cur->task->wakeup = true;
	}

	error = pthread_cond_broadcast(&wq->cond);
	ASSERT(error == 0);

	error = pthread_mutex_unlock(&wq_head->list_mutex);
	ASSERT(error == 0);

	return 0;
}
