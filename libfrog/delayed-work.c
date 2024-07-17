#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include "list.h"
#include "delayed-work.h"

struct delayed_work_list {
	struct list_head list;
	pthread_mutex_t mutex;
	timer_t timer_id;
} delayed_work_list;

static void
add_dwork_to_wq(
	union sigval sigval)
{
	struct delayed_work *dwork, *next_dwork;
	struct itimerspec its;
	int error;

	error = pthread_mutex_lock(&delayed_work_list.mutex);
	if (error) {
		fprintf(stderr, "pthread_mutex_lock: %s\n", strerror(error));
		goto out1;
	}

	dwork = list_first_entry(&delayed_work_list.list,
			struct delayed_work, list);
	assert(dwork != NULL);

	list_del_init(&dwork->list);

	if (!list_empty(&delayed_work_list.list)) {
		next_dwork = list_first_entry(&delayed_work_list.list,
				struct delayed_work, list);

		memset(&its, 0, sizeof(its));
		its.it_value = next_dwork->ts;
		error = timer_settime(delayed_work_list.timer_id, TIMER_ABSTIME,
				&its, NULL);
		if (error == -1) {
			perror("timer_settime");
			goto out3;
		}
	}

	error = pthread_mutex_unlock(&delayed_work_list.mutex);
	if (error) {
		fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(error));
		goto out2;
	}

	error = workqueue_add(dwork->wq, dwork->func, dwork->index, dwork->arg);
	if (error)
		goto out2;

	free(dwork);

	return;
out3:
	error = pthread_mutex_unlock(&delayed_work_list.mutex);
out2:
	free(dwork);
out1:
	return;
}


int
queue_delayed_work(
	struct workqueue *wq,
	struct delayed_work *dwork,
	unsigned long delay)
{
	struct itimerspec its;
	struct delayed_work *dwork_cur;
	struct list_head *insert_after;
	bool start_timer;
	int error;

	dwork->wq = wq;
	list_head_init(&dwork->list);

	error = clock_gettime(CLOCK_REALTIME, &dwork->ts);
	if (error == -1) {
		perror("clock_gettime");
		goto out2;
	}
	dwork->ts.tv_sec += delay;

	error = pthread_mutex_lock(&delayed_work_list.mutex);
	if (error) {
		fprintf(stderr, "pthread_mutex_lock: %s\n", strerror(error));
		goto out1;
	}

	insert_after = &delayed_work_list.list;
	list_for_each_entry(dwork_cur, &delayed_work_list.list, list) {
		insert_after = &dwork_cur->list;
		if (dwork_cur->ts.tv_sec > dwork->ts.tv_sec ||
		    (dwork_cur->ts.tv_sec == dwork->ts.tv_sec &&
				dwork_cur->ts.tv_nsec > dwork->ts.tv_nsec)) {
			insert_after = &(list_prev_entry(dwork_cur, list)->list);
			break;
		}
	}

	memset(&its, 0, sizeof(its));

	start_timer = false;

	if (insert_after == &delayed_work_list.list) {
		if (!list_empty(&delayed_work_list.list)) {
			/* Disarm timer */
			error = timer_settime(delayed_work_list.timer_id,
					TIMER_ABSTIME, &its, NULL);
			if (error == -1) {
				perror("timer_settime");
				goto out2;
			}
		}

		start_timer = true;
	}

	list_add(&dwork->list, insert_after);

	if (start_timer) {
		its.it_value = dwork->ts;
		error = timer_settime(delayed_work_list.timer_id, TIMER_ABSTIME,
				&its, NULL);
		if (error == -1) {
			perror("timer_settime");
			goto out2;
		}
	}

	error = pthread_mutex_unlock(&delayed_work_list.mutex);
	if (error) {
		fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(error));
		goto out1;
	}

	return 0;
out2:
	error = pthread_mutex_unlock(&delayed_work_list.mutex);

out1:
	return error;
}

void
init_delayed_work(
	struct delayed_work *dwork,
	workqueue_func_t *func,
	uint32_t index,
	void *arg)
{
	memset(dwork, 0, sizeof(*dwork));
	dwork->func = func;
	dwork->index = index;
	dwork->arg = arg;
}

int
delayed_work_module_init(void)
{
	struct sigevent sev;
	int error;

	list_head_init(&delayed_work_list.list);

	error = pthread_mutex_init(&delayed_work_list.mutex, NULL);
	assert(error == 0);

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = add_dwork_to_wq;
	error = timer_create(CLOCK_REALTIME, &sev, &delayed_work_list.timer_id);
	if (error == -1) {
		perror("timer_create");
		goto out1;
	}

	return 0;
out1:
	return error;
}
