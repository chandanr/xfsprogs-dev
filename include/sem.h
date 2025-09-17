#ifndef __SEM_H__
#define __SEM_H__

#include <pthread.h>

struct semaphore {
	pthread_mutex_t mutex;
};

static inline void
sema_init(
	struct semaphore	*sem,
	int			val)
{
	int error;

	error = pthread_mutex_init(&sem->mutex, NULL);
	ASSERT(error == 0);
}

static inline void
down(
	struct semaphore	*sem)
{
	int			error;

	error = pthread_mutex_lock(&sem->sem);
	ASSERT(error == 0);
}

static inline void
up(
	struct semaphore	*sem)
{
	int			error;

	error = pthread_mutex_unlock(&sem->sem);
	ASSERT(error == 0);
}

#endif	/* __SEM_H__ */
