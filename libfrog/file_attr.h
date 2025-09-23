// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Red Hat, Inc.
 * All Rights Reserved.
 */
#ifndef __LIBFROG_FILE_ATTR_H__
#define __LIBFROG_FILE_ATTR_H__

#include "linux.h"
#include <sys/stat.h>

#define SPECIAL_FILE(x) \
	   (S_ISCHR((x)) \
	|| S_ISBLK((x)) \
	|| S_ISFIFO((x)) \
	|| S_ISLNK((x)) \
	|| S_ISSOCK((x)))

int
xfrog_file_getattr(
	const int		dfd,
	const char		*path,
	const struct stat	*stat,
	struct file_attr	*fa,
	const unsigned int	at_flags);

int xfrog_file_setattr(const int dfd, const char *path, const mode_t mode,
		struct file_attr *fa, const unsigned int at_flags);

#endif /* __LIBFROG_FILE_ATTR_H__ */
