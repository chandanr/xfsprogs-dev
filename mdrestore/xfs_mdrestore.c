// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2007 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "xfs_metadump.h"

struct mdrestore_ops {
	void (*read_header)(void *header, FILE *mdfp);
	void (*show_info)(void *header, const char *mdfile);
	void (*restore)(FILE *mdfp, int data_fd, bool is_data_target_file,
			int log_fd, bool is_log_target_file, void *header);
};

static int	show_progress = 0;
static int	show_info = 0;
static int	progress_since_warning = 0;

static void
fatal(const char *msg, ...)
{
	va_list		args;

	va_start(args, msg);
	fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, msg, args);
	exit(1);
}

static void
print_progress(const char *fmt, ...)
{
	char		buf[60];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[sizeof(buf)-1] = '\0';

	printf("\r%-59s", buf);
	fflush(stdout);
	progress_since_warning = 1;
}

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [-V] [-g] [-i] [-l logdev] [-v 1|2] source target\n", progname);
	exit(1);
}

extern int	platform_check_ismounted(char *, char *, struct stat *, int);

static int
open_device(char *path, bool *is_file)
{
	struct stat	statbuf;
	int open_flags;
	int fd;

	open_flags = O_RDWR;
	*is_file = false;

	if (stat(path, &statbuf) < 0)  {
		/* ok, assume it's a file and create it */
		open_flags |= O_CREAT;
		*is_file = true;
	} else if (S_ISREG(statbuf.st_mode))  {
		open_flags |= O_TRUNC;
		*is_file = true;
	} else  {
		/*
		 * check to make sure a filesystem isn't mounted on the device
		 */
		if (platform_check_ismounted(path, NULL, &statbuf, 0))
			fatal("a filesystem is mounted on device \"%s\","
				" cannot restore to a mounted filesystem.\n",
				path);
	}

	fd = open(path, open_flags, 0644);
	if (fd < 0)
		fatal("couldn't open \"%s\"\n", path);

	return fd;
}


void read_header1(void *header, FILE *mdfp)
{
	struct xfs_metablock	*mb = header;

	if (fread(mb, sizeof(*mb), 1, mdfp) != 1)
		fatal("error reading from metadump file\n");
	if (mb->mb_magic != cpu_to_be32(XFS_MD_MAGIC))
		fatal("specified file is not a metadata dump\n");
}

void show_info1(void *header, const char *mdfile)
{
	struct xfs_metablock	*mb = header;

	if (mb->mb_info & XFS_METADUMP_INFO_FLAGS) {
		printf("%s: %sobfuscated, %s log, %s metadata blocks\n",
			mdfile,
			mb->mb_info & XFS_METADUMP_OBFUSCATED ? "":"not ",
			mb->mb_info & XFS_METADUMP_DIRTYLOG ? "dirty":"clean",
			mb->mb_info & XFS_METADUMP_FULLBLOCKS ? "full":"zeroed");
	} else {
		printf("%s: no informational flags present\n", mdfile);
	}
}

void restore1(FILE *mdfp, int data_fd, bool is_data_target_file,
	int log_fd, bool is_log_target_file, void *header)
{
	struct xfs_metablock	*mb = header;
	struct xfs_metablock	*metablock;
	__be64			*block_index;
	char			*block_buffer;
	int			block_size;
	int			max_indices;
	int			cur_index;
	int			mb_count;
	xfs_sb_t		sb;
	int64_t			bytes_read;

	block_size = 1 << mb->mb_blocklog;
	max_indices = (block_size - sizeof(struct xfs_metablock)) / sizeof(__be64);

	metablock = calloc(max_indices + 1, block_size);
	if (metablock == NULL)
		fatal("memory allocation failure\n");

	mb_count = be16_to_cpu(mb->mb_count);
	if (mb_count == 0 || mb_count > max_indices)
		fatal("bad block count: %u\n", mb_count);

	block_index = (__be64 *)((char *)metablock + sizeof(struct xfs_metablock));
	block_buffer = (char *)metablock + block_size;

	if (fread(block_index, block_size - sizeof(struct xfs_metablock), 1, mdfp) != 1)
		fatal("error reading from metadump file\n");

	if (block_index[0] != 0)
		fatal("first block is not the primary superblock\n");

	if (fread(block_buffer, mb_count << mb->mb_blocklog, 1, mdfp) != 1)
		fatal("error reading from metadump file\n");

	libxfs_sb_from_disk(&sb, (struct xfs_dsb *)block_buffer);

	if (sb.sb_magicnum != XFS_SB_MAGIC)
		fatal("bad magic number for primary superblock\n");

	/*
	 * Normally the upper bound would be simply XFS_MAX_SECTORSIZE
	 * but the metadump format has a maximum number of BBSIZE blocks
	 * it can store in a single metablock.
	 */
	if (sb.sb_sectsize < XFS_MIN_SECTORSIZE ||
	    sb.sb_sectsize > XFS_MAX_SECTORSIZE ||
	    sb.sb_sectsize > max_indices * block_size)
		fatal("bad sector size %u in metadump image\n", sb.sb_sectsize);

	((struct xfs_dsb*)block_buffer)->sb_inprogress = 1;

	if (is_data_target_file) {
		/* ensure regular files are correctly sized */
		if (ftruncate(data_fd, sb.sb_dblocks * sb.sb_blocksize))
			fatal("cannot set filesystem image size: %s\n",
				strerror(errno));
	} else {
		/* ensure device is sufficiently large enough */
		char		lb[XFS_MAX_SECTORSIZE] = { 0 };
		off64_t		off;

		off = sb.sb_dblocks * sb.sb_blocksize - sizeof(lb);
		if (pwrite(data_fd, lb, sizeof(lb), off) < 0)
			fatal("failed to write last block, is target too "
				"small? (error: %s)\n", strerror(errno));
	}

	bytes_read = 0;

	for (;;) {
		if (show_progress && (bytes_read & ((1 << 20) - 1)) == 0)
			print_progress("%lld MB read", bytes_read >> 20);

		for (cur_index = 0; cur_index < mb_count; cur_index++) {
			if (pwrite(data_fd, &block_buffer[cur_index <<
					mb->mb_blocklog], block_size,
					be64_to_cpu(block_index[cur_index]) <<
						BBSHIFT) < 0)
				fatal("error writing block %llu: %s\n",
					be64_to_cpu(block_index[cur_index]) << BBSHIFT,
					strerror(errno));
		}

		if (mb_count < max_indices)
			break;

		if (fread(metablock, block_size, 1, mdfp) != 1)
			fatal("error reading from metadump file\n");

		mb_count = be16_to_cpu(metablock->mb_count);
		if (mb_count == 0)
			break;
		if (mb_count > max_indices)
			fatal("bad block count: %u\n", mb_count);

		if (fread(block_buffer, mb_count << mb->mb_blocklog,
				1, mdfp) != 1)
			fatal("error reading from metadump file\n");

		bytes_read += block_size + (mb_count << mb->mb_blocklog);
	}

	if (progress_since_warning)
		putchar('\n');

	memset(block_buffer, 0, sb.sb_sectsize);
	sb.sb_inprogress = 0;
	libxfs_sb_to_disk((struct xfs_dsb *)block_buffer, &sb);
	if (xfs_sb_version_hascrc(&sb)) {
		xfs_update_cksum(block_buffer, sb.sb_sectsize,
				 offsetof(struct xfs_sb, sb_crc));
	}

	if (pwrite(data_fd, block_buffer, sb.sb_sectsize, 0) < 0)
		fatal("error writing primary superblock: %s\n", strerror(errno));

	free(metablock);
}

struct mdrestore_ops ops1 = {
    .read_header = read_header1,
    .show_info = show_info1,
    .restore = restore1,
};

void read_header2(void *header, FILE *mdfp)
{
	struct xfs_metadump_header	*xmh = header;

	if (fread(xmh, sizeof(*xmh), 1, mdfp) != 1)
		fatal("error reading from metadump file\n");
	if (xmh->xmh_magic != XFS_MD_MAGIC_V2)
		fatal("specified file is not a metadata dump\n");
}

void show_info2(void *header, const char *mdfile)
{
	struct xfs_metadump_header	*xmh = header;
	uint32_t incompat_flags = be32_to_cpu(xmh->xmh_incompat_flags);

	printf("%s: %sobfuscated, %s log, %s metadata blocks\n",
		mdfile,
		incompat_flags & XFS_MD2_INCOMPAT_OBFUSCATED ? "":"not ",
		incompat_flags & XFS_MD2_INCOMPAT_DIRTYLOG ? "dirty":"clean",
		incompat_flags & XFS_MD2_INCOMPAT_FULLBLOCKS ? "full":"zeroed");
}

void restore2(FILE *mdfp, int data_fd, bool is_data_target_file,
	int log_fd, bool is_log_target_file, void *header)
{
	struct xfs_sb sb;
	struct xfs_meta_extent		xme;
	uint8_t *block_buffer;
	int64_t bytes_read;
	uint64_t offset;
	int prev_len;
	int len;

	if (fread(&xme, sizeof(xme), 1, mdfp) != 1)
		fatal("error reading from metadump file\n");

	len = be32_to_cpu(xme.xme_len);
	block_buffer = calloc(1, len);
	if (block_buffer == NULL)
		fatal("memory allocation failure\n");

	if (fread(block_buffer, len, 1, mdfp) != 1)
		fatal("error reading from metadump file\n");

	libxfs_sb_from_disk(&sb, (struct xfs_dsb *)block_buffer);

	if (sb.sb_magicnum != XFS_SB_MAGIC)
		fatal("bad magic number for primary superblock\n");

	if (sb.sb_logstart == 0 && log_fd == -1)
		fatal("External Log device is required\n");

	((struct xfs_dsb *)block_buffer)->sb_inprogress = 1;

	if (is_data_target_file)  {
		/* ensure regular files are correctly sized */
		if (ftruncate(data_fd, sb.sb_dblocks * sb.sb_blocksize))
			fatal("cannot set data device image size: %s\n",
				strerror(errno));
	} else  {
		/* ensure device is sufficiently large enough */
		char		lb[XFS_MAX_SECTORSIZE] = { 0 };
		off64_t		off;

		off = sb.sb_dblocks * sb.sb_blocksize - sizeof(lb);
		if (pwrite(data_fd, lb, sizeof(lb), off) < 0)
			fatal("failed to write last block, is data device target too "
				"small? (error: %s)\n", strerror(errno));
	}

	if (sb.sb_logstart == 0) {
		if (is_log_target_file)  {
			/* ensure regular files are correctly sized */
			if (ftruncate(log_fd, sb.sb_logblocks * sb.sb_blocksize))
				fatal("cannot set log device image size: %s\n",
					strerror(errno));
		} else {
			/* ensure device is sufficiently large enough */
			char		lb[XFS_MAX_SECTORSIZE] = { 0 };
			off64_t		off;

			off = sb.sb_logblocks * sb.sb_blocksize - sizeof(lb);
			if (pwrite(log_fd, lb, sizeof(lb), off) < 0)
				fatal("failed to write last block, is log device target too "
					"small? (error: %s)\n", strerror(errno));
		}
	}

	bytes_read = 0;

	do {
		int fd;

		if (show_progress && (bytes_read & ((1 << 20) - 1)) == 0)
			print_progress("%lld MB read", bytes_read >> 20);

		offset = be64_to_cpu(xme.xme_addr) & XME_ADDR_DEVICE_MASK;

		if (be64_to_cpu(xme.xme_addr) & XME_ADDR_DATA_DEVICE)
			fd = data_fd;
		else if (be64_to_cpu(xme.xme_addr) & XME_ADDR_LOG_DEVICE)
			fd = log_fd;
		else
			ASSERT(0);

		if (pwrite(fd, block_buffer, len, offset) < 0)
			fatal("error writing to %s device at offset %llu: %s\n",
				fd == data_fd ? "data": "log", offset,
				strerror(errno));

                if (fread(&xme, sizeof(xme), 1, mdfp) != 1) {
			if (feof(mdfp))
				break;
			fatal("error reading from metadump file\n");
		}

		prev_len = len;
		len = be32_to_cpu(xme.xme_len);
		if (len > prev_len) {
			void *p;
			p = realloc(block_buffer, len);
			if (p == NULL) {
				free(block_buffer);
				fatal("memory allocation failure\n");
			}
			block_buffer = p;
		}

		if (fread(block_buffer, len, 1, mdfp) != 1)
			fatal("error reading from metadump file\n");

		bytes_read += len;
	} while (1);

	return;
}

struct mdrestore_ops ops2 = {
	.read_header = read_header2,
	.show_info = show_info2,
	.restore = restore2,
};

int
main(
	int 		argc,
	char 		**argv)
{
	FILE		*src_f;
	int		data_dev_fd;
	int		log_dev_fd;
	int		c;
	bool		is_data_dev_file;
	bool		is_log_dev_file;
	int		version;
	struct xfs_metablock	mb;
	struct xfs_metadump_header xmh;
	struct mdrestore_ops *ops;
	char		*logdev = NULL;
	char		*p;
	void		*header;

	progname = basename(argv[0]);

	while ((c = getopt(argc, argv, "gil:v:V")) != EOF) {
		switch (c) {
			case 'g':
				show_progress = 1;
				break;
			case 'i':
				show_info = 1;
				break;
			case 'l':
				logdev = optarg;
				/* chandan: Remove this later */
				fprintf(stderr, "logdev = %s\n", logdev);
				break;
			case 'v':
				version = (int)strtol(optarg, &p, 0);
				if (*p != '\0' || (version != 1 && version != 2)) {
					/* chandan: Define print_warning() */
					/*
					 * print_warning("bad metadump version: %s",
					 * 	optarg);
					 */
					return 0;
				}
				break;
			case 'V':
				printf("%s version %s\n", progname, VERSION);
				exit(0);
			default:
				usage();
		}
	}

	if (argc - optind < 1 || argc - optind > 2)
		usage();

	/* show_info without a target is ok */
	if (!show_info && argc - optind != 2)
		usage();

	/*
	 * open source and test if this really is a dump. The first block will
	 * be passed to mdrestore_ops->restore() which will continue to read the
	 * file from this point. This avoids rewind the stream, which causes
	 * restore to fail when source was being read from stdin.
 	 */
	if (strcmp(argv[optind], "-") == 0) {
		src_f = stdin;
		if (isatty(fileno(stdin)))
			fatal("cannot read from a terminal\n");
	} else {
		src_f = fopen(argv[optind], "rb");
		if (src_f == NULL)
			fatal("cannot open source dump file\n");
	}

	if (version == 1) {
		ops = &ops1;
		header = &mb;
	} else if (version == 2) {
		ops = &ops2;
		header = &xmh;
	} else {
		ASSERT(0);
	}

	ops->read_header(header, src_f);

        if (show_info) {
		ops->show_info(header, argv[optind]);

                if (argc - optind == 1)
			exit(0);
	}

	optind++;

	/* check and open data device */
	data_dev_fd = open_device(argv[optind], &is_data_dev_file);

	log_dev_fd = -1;
	if (logdev)
		log_dev_fd = open_device(logdev, &is_log_dev_file);

	ops->restore(src_f, data_dev_fd, is_data_dev_file, log_dev_fd,
		is_log_dev_file, header);

	close(data_dev_fd);
	if (logdev)
		close(log_dev_fd);
	if (src_f != stdin)
		fclose(src_f);

	return 0;
}
