#include "kthread.h"

struct task_struct *
kthread_run(
	threadfn_t threadfn,
	void *data,
	...)
{
	struct task_struct *ts;
	int error;

	ts = calloc(1, sizeof(*ts));
	if (ts == NULL)
		return NULL;

	error = pthread_create(&ts->thread, NULL, threadfn, data);
	if (error) {
		free(ts);
		return NULL;
	}

	return ts;
}
