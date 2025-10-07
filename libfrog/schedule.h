#ifndef _SCHEDULE_H
#define _SCHEDULE_H

#define TASK_RUNNING		0
#define TASK_KILLABLE		1
#define TASK_FREEZABLE		2
#define TASK_INTERRUPTIBLE	3
#define TASK_UNINTERRUPTIBLE	4

struct task_struct {
	pthread_t	thread;
	bool		wakeup;
	void		*journal_info;
};

#define set_freezable()
#define set_current_state(...)
#define try_to_freeze()

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

static inline void
io_schedule(
	struct task_struct	*ts,
	pthread_mutex_t		*mutex,
	pthread_cond_t		*cond)
{
	schedule(ts, mutex, cond);
}

#define msecs_to_jiffies(msecs) (msecs)

static inline void
schedule_timeout(
	signed long	msecs)
{
	struct timespec ts;
	int		error;

	ts.tv_sec = msecs / 1000;
	ts.tv_nsec = (msecs % 1000) * 1000 * 1000;

	error = nanosleep(&ts, NULL);
	if (error == -1 && errno == EINTR)
		error = 0;
}

#endif	/* _SCHEDULE_H */
