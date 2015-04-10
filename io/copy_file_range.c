/*
 * Copyright (c) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"

static cmdinfo_t copy_file_range_cmd;

static void
copy_file_range_help(void)
{
	printf(_(
"\n"
" copy a range of bytes between files\n"
"\n"
" Example:\n"
" 'copy_file_range -f 2 512' - copies 512 bytes between the open files\n"
"\n"
" Copies data between one file descriptor and another.  The copy can be\n"
" accelerated if the file descriptors support it.  The kernel may not see\n"
" any data transfer at all.  For example, with NFS, the server may copy\n"
" between files without the client seeing any data.\n"
" The offsets in the files are optional.  If an offset isn't provided then\n"
" the current position in the file descriptor is used and updated.\n"
" -f -- specifies an open input file number from which to source data to write\n"
" -i -- specifies an input file name from which to source data to write.\n"
" -s -- specifies the byte offset to copy from in the source file.\n"
" -t -- specifies the byte offset to copy to in the target file.\n"
" -F -- specifies the raw flags argument for the copy system call.\n" 
" The final argument specifies length in bytes to copy between the files.\n"
"\n"));
}

/* XXX devel hack, remove once libc has copy_file_range wrappers */
#if 1

#ifdef __x86_64__
#define __NR_copy_file_range 326
#elif __i386__
#define __NR_copy_file_range 377
#elif __powerpc64__
#define __NR_copy_file_range 379
#else
#error "sorry, don't know __NR_copy_file_range for arch"
#endif

static inline int copy_file_range(int fd_in, loff_t *pos_in, int fd_out,
				  loff_t *pos_out, size_t len, int flags)
{
	return syscall(__NR_copy_file_range, fd_in, pos_in, fd_out, pos_out,
		       len, flags);
}
#endif

static int
copy_range(
	int		fd_in,
	off64_t		*off_in,
	int		fd_out,
	off64_t		*off_out,
	size_t		len,
	int		flags,
	long long	*total)
{
	ssize_t		bytes;
	int		ops = 0;

	*total = 0;
	while (len > 0) {
		bytes = copy_file_range(fd_in, off_in, fd_out, off_out, len,
					flags);
		if (bytes == 0)
			break;
		if (bytes < 0) {
			perror("copy_file_range");
			return -1;
		}
		ops++;
		*total += bytes;

		if (off_in)
			*off_in += bytes;
		if (off_out)
			*off_out += bytes;

		if (bytes >= len)
			break;
		len -= bytes;
	}
	return ops;
}

static int
copy_file_range_f(
	int		argc,
	char		**argv)
{
	off64_t		in_offset;
	off64_t		out_offset;
	off64_t		*off_in = NULL;
	off64_t		*off_out = NULL;
	long long	count, total;
	size_t		blocksize, sectsize;
	struct timeval	t1, t2;
	char		s1[64], s2[64], ts[64];
	char		*infile = NULL;
	int		Cflag, qflag;
	int		c, fd = -1;
	int		flags = 0;

	Cflag = qflag = 0;
	init_cvtnum(&blocksize, &sectsize);
	while ((c = getopt(argc, argv, "Cf:F:i:s:t:q")) != EOF) {
		switch (c) {
		case 'C':
			Cflag = 1;
			break;
		case 'q':
			qflag = 1;
			break;
		case 'f':
			fd = atoi(argv[1]);
			if (fd < 0 || fd >= filecount) {
				printf(_("value %d is out of range (0-%d)\n"),
					fd, filecount-1);
				return 0;
			}
			break;
		case 'F':
			flags = strtoul(optarg, NULL, 0);
			if (errno) {
				printf(_("invalid flags argument -- %s\n"),
					 argv[1]);
				return 0;
			}
			break;
		case 'i':
			infile = optarg;
			break;
		case 's':
			in_offset = cvtnum(blocksize, sectsize, optarg);
			if (in_offset < 0) {
				printf(_("non-numeric offset argument -- %s\n"),
					optarg);
				goto done;
			}
			off_in = &in_offset;
			break;
		case 't':
			out_offset = cvtnum(blocksize, sectsize, optarg);
			if (out_offset < 0) {
				printf(_("non-numeric offset argument -- %s\n"),
					optarg);
				goto done;
			}
			off_out = &out_offset;
			break;
		default:
			return command_usage(&copy_file_range_cmd);
		}
	}
	if (infile && fd != -1)
		return command_usage(&copy_file_range_cmd);

	if (!infile)
		fd = filetable[fd].fd;
	else if ((fd = openfile(infile, NULL, IO_READONLY, 0)) < 0)
		return 0;

	/* must specify length */
	if (optind != argc - 1)
		return command_usage(&copy_file_range_cmd);

	count = cvtnum(blocksize, sectsize, argv[optind]);
	if (count < 0) {
		printf(_("non-numeric length argument -- %s\n"),
			argv[optind]);
		goto done;
	}

	gettimeofday(&t1, NULL);
	c = copy_range(fd, off_in, file->fd, off_out, count, flags, &total);
	if (c < 0)
		goto done;
	if (qflag)
		goto done;
	gettimeofday(&t2, NULL);
	t2 = tsub(t2, t1);

	/* Finally, report back -- -C gives a parsable format */
	timestr(&t2, ts, sizeof(ts), Cflag ? VERBOSE_FIXED_TIME : 0);
	if (!Cflag) {
		cvtstr((double)total, s1, sizeof(s1));
		cvtstr(tdiv((double)total, t2), s2, sizeof(s2));
		printf(_("copied %lld/%lld bytes\n"),
			total, count);
		printf(_("%s, %d ops; %s (%s/sec and %.4f ops/sec)\n"),
			s1, c, ts, s2, tdiv((double)c, t2));
	} else {/* bytes,ops,time,bytes/sec,ops/sec */
		printf("%lld,%d,%s,%.3f,%.3f\n",
			total, c, ts,
			tdiv((double)total, t2), tdiv((double)c, t2));
	}
done:
	if (infile)
		close(fd);
	return 0;
}

void
copy_file_range_init(void)
{
	copy_file_range_cmd.name = "copy_file_range";
	copy_file_range_cmd.altname = "copy_file_range";
	copy_file_range_cmd.cfunc = copy_file_range_f;
	copy_file_range_cmd.argmin = 3;
	copy_file_range_cmd.argmax = 9;
	copy_file_range_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	copy_file_range_cmd.args =
		_("-i infile | -f N [-s src_off] [-t targ_off] [-F flags] len");
	copy_file_range_cmd.oneline =
		_("Copy data directly between file descriptors");
	copy_file_range_cmd.help = copy_file_range_help;

	add_command(&copy_file_range_cmd);
}
