#ifndef _SCHEDULE_H
#define _SCHEDULE_H

struct task_struct {
	pthread_t	thread;
	bool		wakeup;
};

static inline void
schedule(
	struct task_struct	*ts,
	pthread_mutex_t		*mutex,
	pthread_cond_t		*cond)
{
	int			error;

	while (!ts->wakeup) {
		error = pthread_mutex_lock(mutex);
		ASSERT(error == 0);

		error = pthread_cond_wait(cond, mutex);
		ASSERT(error == 0);

		error = pthread_mutex_unlock(mutex);
		ASSERT(error == 0);
	}
}

#endif	/* _SCHEDULE_H */
