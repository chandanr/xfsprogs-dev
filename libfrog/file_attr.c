// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "file_attr.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <asm/types.h>
#include <fcntl.h>

static void
file_attr_to_fsxattr(
	const struct file_attr	*fa,
	struct fsxattr		*fsxa)
{
	memset(fsxa, 0, sizeof(struct fsxattr));

	fsxa->fsx_xflags = fa->fa_xflags;
	fsxa->fsx_extsize = fa->fa_extsize;
	fsxa->fsx_nextents = fa->fa_nextents;
	fsxa->fsx_projid = fa->fa_projid;
	fsxa->fsx_cowextsize = fa->fa_cowextsize;
}

static void
fsxattr_to_file_attr(
	const struct fsxattr	*fsxa,
	struct file_attr	*fa)
{
	memset(fa, 0, sizeof(struct file_attr));

	fa->fa_xflags = fsxa->fsx_xflags;
	fa->fa_extsize = fsxa->fsx_extsize;
	fa->fa_nextents = fsxa->fsx_nextents;
	fa->fa_projid = fsxa->fsx_projid;
	fa->fa_cowextsize = fsxa->fsx_cowextsize;
}

int
xfrog_file_getattr(
	const int		dfd,
	const char		*path,
	const struct stat	*stat,
	struct file_attr	*fa,
	const unsigned int	at_flags)
{
	int			error;
	int			fd;
	struct fsxattr		fsxa;

#ifdef HAVE_FILE_GETATTR
	error = syscall(__NR_file_getattr, dfd, path, fa,
			sizeof(struct file_attr), at_flags);
	if (error && errno != ENOSYS)
		return error;

	if (!error)
		return error;
#endif

	if (SPECIAL_FILE(stat->st_mode)) {
		errno = EOPNOTSUPP;
		return -1;
	}

	fd = open(path, O_RDONLY|O_NOCTTY);
	if (fd == -1)
		return fd;

	error = ioctl(fd, FS_IOC_FSGETXATTR, &fsxa);
	close(fd);
	if (error)
		return error;

	fsxattr_to_file_attr(&fsxa, fa);

	return error;
}

int
xfrog_file_setattr(
	const int		dfd,
	const char		*path,
	const mode_t		mode,
	struct file_attr	*fa,
	const unsigned int	at_flags)
{
	int			error;
	int			fd;
	struct fsxattr		fsxa;

#ifdef HAVE_FILE_GETATTR /* file_get/setattr goes together */
	error = syscall(__NR_file_setattr, dfd, path, fa,
			sizeof(struct file_attr), at_flags);
	if (error && errno != ENOSYS)
		return error;

	if (!error)
		return error;
#endif

	if (SPECIAL_FILE(mode)) {
		errno = EOPNOTSUPP;
		return -1;
	}

	fd = open(path, O_RDONLY|O_NOCTTY);
	if (fd == -1)
		return fd;

	file_attr_to_fsxattr(fa, &fsxa);

	error = ioctl(fd, FS_IOC_FSSETXATTR, fa);
	close(fd);

	return error;
}
