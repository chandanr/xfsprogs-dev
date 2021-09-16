// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-20 RedHat, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBXFS_SEMA_H__
#define __LIBXFS_SEMA_H__

/*
 * This implements kernel compatible semaphore _exclusion_ semantics. It does
 * not implement counting semaphore behaviour.
 *
 * This makes use of the fact that fast pthread mutexes on Linux don't check
 * that the unlocker is the same thread that locked the mutex, and hence can be
 * unlocked in a different thread safely.
 *
 * If this needs to be portable or we require counting semaphore behaviour in
 * libxfs code, this requires re-implementation based on posix semaphores.
 */
struct semaphore {
	pthread_mutex_t		lock;
};

#define sema_init(l, nolock)		\
do {					\
	pthread_mutex_init(&(l)->lock, NULL);	\
	if (!nolock)			\
		pthread_mutex_lock(&(l)->lock);	\
} while (0)

#define down(l)			pthread_mutex_lock(&(l)->lock)
#define down_trylock(l)		pthread_mutex_trylock(&(l)->lock)
#define up(l)			pthread_mutex_unlock(&(l)->lock)

#endif /* __LIBXFS_SEMA_H__ */
