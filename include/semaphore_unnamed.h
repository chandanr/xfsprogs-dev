#ifndef __LIBXFS_UNNAMED_SEMAPHORE_H__
#define __LIBXFS_UNNAMED_SEMAPHORE_H__

/* Wrapper around POSIX unnamed semaphores  */

#include <semaphore.h>

struct semaphore {
	sem_t sem;
};

#define sem_init(semaphore, val) \
{				 \
	int error;		 \
	error = sem_init(&(semaphore)->sem, 0, (val));	\
	ASSERT(error == 0); \
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

#endif	/* __LIBXFS_UNNAMED_SEMAPHORE_H__ */
