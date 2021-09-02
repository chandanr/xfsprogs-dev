// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Red Hat, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"
#include "libfrog/fiexchange.h"
#include "libfrog/file_exchange.h"

static cmdinfo_t swapext_cmd;

static void
swapext_help(void)
{
	printf(_(
"\n"
" Swaps extents between the open file descriptor and the supplied filename.\n"
"\n"
" -a   -- use atomic extent swapping\n"
" -d N -- start swapping extents at this offset in the open file\n"
" -e   -- Swap extents to the ends of both files, including the file sizes\n"
" -f   -- Flush changed file data and metadata to disk\n"
" -h   -- Do not swap ranges that correspond to holes in the supplied file\n"
" -l N -- swap this many bytes between the two files\n"
" -n   -- Dry run; do all the parameter validation but do not change anything.\n"
" -s N -- start swapping extents at this offset in the supplied file\n"
" -u   -- do not compare the open file's timestamps\n"
" -v   -- 'xfs' for XFS_IOC_SWAPEXT, or 'vfs' for FIEXCHANGE_RANGE\n"));
}

static void
set_xfd_flags(
	struct xfs_fd	*xfd,
	int		api_ver)
{
	switch (api_ver) {
	case 0:
		xfd->flags |= XFROG_FLAG_FORCE_SWAPEXT;
		break;
	case 1:
		xfd->flags |= XFROG_FLAG_FORCE_FIEXCHANGE;
		break;
	default:
		break;
	}
}

static int
swapext_f(
	int			argc,
	char			**argv)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct file_xchg_range	fxr;
	struct stat		stat;
	uint64_t		flags = FILE_XCHG_RANGE_NONATOMIC |
					FILE_XCHG_RANGE_FILE2_FRESH |
					FILE_XCHG_RANGE_FULL_FILES;
	int64_t			src_offset = 0;
	int64_t			dest_offset = 0;
	int64_t			length = -1;
	size_t			fsblocksize, fssectsize;
	int			api_ver = -1;
	int			c;
	int			fd;
	int			ret;

	init_cvtnum(&fsblocksize, &fssectsize);
	while ((c = getopt(argc, argv, "ad:efhl:ns:uv:")) != -1) {
		switch (c) {
		case 'a':
			flags &= ~FILE_XCHG_RANGE_NONATOMIC;
			break;
		case 'd':
			dest_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (dest_offset < 0) {
				printf(
			_("non-numeric open file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~FILE_XCHG_RANGE_FULL_FILES;
			break;
		case 'e':
			flags |= FILE_XCHG_RANGE_TO_EOF;
			flags &= ~FILE_XCHG_RANGE_FULL_FILES;
			break;
		case 'f':
			flags |= FILE_XCHG_RANGE_FSYNC;
			break;
		case 'h':
			flags |= FILE_XCHG_RANGE_SKIP_FILE1_HOLES;
			break;
		case 'l':
			length = cvtnum(fsblocksize, fssectsize, optarg);
			if (length < 0) {
				printf(
			_("non-numeric length argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~FILE_XCHG_RANGE_FULL_FILES;
			break;
		case 'n':
			flags |= FILE_XCHG_RANGE_DRY_RUN;
			break;
		case 's':
			src_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (src_offset < 0) {
				printf(
			_("non-numeric supplied file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~FILE_XCHG_RANGE_FULL_FILES;
			break;
		case 'u':
			flags &= ~FILE_XCHG_RANGE_FILE2_FRESH;
			break;
		case 'v':
			if (!strcmp(optarg, "xfs"))
				api_ver = 0;
			else if (!strcmp(optarg, "vfs"))
				api_ver = 1;
			else {
				fprintf(stderr,
			_("version must be 'xfs' or 'vfs'.\n"));
				return 1;
			}
			break;
		default:
			swapext_help();
			return 0;
		}
	}
	if (optind != argc - 1) {
		swapext_help();
		return 0;
	}

	/* open the donor file */
	fd = openfile(argv[optind], NULL, 0, 0, NULL);
	if (fd < 0)
		return 0;

	ret = -xfd_prepare_geometry(&xfd);
	if (ret) {
		xfrog_perror(ret, "xfd_prepare_geometry");
		exitcode = 1;
		goto out;
	}

	if (length < 0) {
		ret = fstat(file->fd, &stat);
		if (ret) {
			perror("fstat");
			exitcode = 1;
			goto out;
		}

		length = stat.st_size;
	}

	ret = xfrog_file_exchange_prep(&xfd, flags, dest_offset, fd, src_offset,
			length, &fxr);
	if (ret) {
		xfrog_perror(ret, "xfrog_file_exchange_prep");
		exitcode = 1;
		goto out;
	}

	set_xfd_flags(&xfd, api_ver);

	ret = xfrog_file_exchange(&xfd, &fxr);
	if (ret) {
		xfrog_perror(ret, "swapext");
		exitcode = 1;
		goto out;
	}
out:
	close(fd);
	return 0;
}

void
swapext_init(void)
{
	swapext_cmd.name = "swapext";
	swapext_cmd.cfunc = swapext_f;
	swapext_cmd.argmin = 1;
	swapext_cmd.argmax = -1;
	swapext_cmd.flags = CMD_NOMAP_OK;
	swapext_cmd.args = _("[-a] [-e] [-f] [-u] [-d dest_offset] [-s src_offset] [-l length] [-v xfs|vfs] <donorfile>");
	swapext_cmd.oneline = _("Swap extents between files.");
	swapext_cmd.help = swapext_help;

	add_command(&swapext_cmd);
}
