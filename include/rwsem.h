#ifndef __RWSEM_H__
#define __RWSEM_H__

#include <pthread.h>

struct rw_semaphore {
	pthread_rwlock_t rwlock;
};

static inline void
init_rwsem(
	struct rw_semaphore	*sem)
{
	int			error;

	error = pthread_rwlock_init(&sem->rwlock, NULL);
	ASSERT(error == 0);
}

static inline void
down_read(
	struct rw_semaphore	*sem)
{
	int			error;

	error = pthread_rwlock_rdlock(&sem->rwlock);
	ASSERT(error == 0);
}

static inline void
down_write(
	struct rw_semaphore	*sem)
{
	int			error;

	error = pthread_rwlock_wrlock(&sem->rwlock);
	ASSERT(error == 0);
}

static inline int
down_write_trylock(
	struct rw_semaphore	*sem)
{
	int			error;

	error = pthread_rwlock_trywrlock(&sem->rwlock);
	if (error) {
		if (error == EBUSY)
			return 0;
		ASSERT(0);
	}

	return 1;
}

static inline void
up_read(
	struct rw_semaphore	*sem)
{
	int			error;

	error = pthread_rwlock_unlock(&sem->rwlock);
	ASSERT(error == 0);
}

static inline void
up_write(
	struct rw_semaphore	*sem)
{
	int			error;

	error = pthread_rwlock_unlock(&sem->rwlock);
	ASSERT(error == 0);
}

#endif	/* __RWSEM_H__ */
