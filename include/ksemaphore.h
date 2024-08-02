#ifndef __KSEMAPHORE_H__
#define __KSEMAPHORE_H__

/* Wrapper around POSIX unnamed semaphores  */

#include <semaphore.h>

struct semaphore {
	sem_t sem;
};

static inline void
sema_init(struct semaphore *semaphore, int val)
{
	int error;

	error = sem_init(&semaphore->sem, 0, val);

	if (error)
		ASSERT(0);
}

static inline void
down(struct semaphore *semaphore)
{
	sem_wait(&semaphore->sem);
}

static inline void
up(struct semaphore *semaphore)
{
	sem_post(&semaphore->sem);
}

/* TODO: Should we implement a wrapper for sem_destroy() */


struct rw_semaphore {
	sem_t sem;
};

static inline void
init_rwsem(struct rw_semaphore *rw_sem)
{
	int error;

	error = sem_init(&rw_sem->sem, 0, 1);
	if (error)
		ASSERT(0);
}

static inline void
down_read(struct rw_semaphore *rw_sem)
{
	sem_wait(&rw_sem->sem);
}

static inline void
down_write(struct rw_semaphore *rw_sem)
{
	sem_wait(&rw_sem->sem);
}

static inline int
down_write_trylock(struct rw_semaphore *rw_sem)
{
	int error;

	error = sem_trywait(&rw_sem->sem);
	if (error == -1 && errno == -EAGAIN)
		return 0;
	ASSERT(error == 0);

	return 1;
}

static inline void
up_write(struct rw_semaphore *rw_sem)
{
	sem_post(&rw_sem->sem);
}

static inline void
up_read(struct rw_semaphore *rw_sem)
{
	sem_post(&rw_sem->sem);
}

#endif	/* __KSEMAPHORE_H__ */
