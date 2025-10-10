// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef __LIBXFS_H__
#define __LIBXFS_H__

#include "libxfs_api_defs.h"
#include "platform_defs.h"
#include "xfs.h"

#include "list.h"
#include "hlist.h"
#include "cache.h"
#include "bitops.h"
#include "kmem.h"
#include "libfrog/delayed-work.h"
#include "libfrog/radix-tree.h"
#include "libfrog/rbtree.h"
#include "libfrog/bitmask.h"
#include "libfrog/div64.h"
#include "libfrog/percpu_counter.h"
#include "libfrog/waitqueue.h"
#include "atomic.h"
#include "spinlock.h"
#include "linux-err.h"

#include "xfs_types.h"
#include "xfs_fs.h"
#include "xfs_arch.h"

#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"


/* CRC stuff, buffer API dependent on it */
extern uint32_t crc32c_le(uint32_t crc, unsigned char const *p, size_t len);
#define crc32c(c,p,l)	crc32c_le((c),(unsigned char const *)(p),(l))

/* fake up kernel's iomap, (not) used in xfs_bmap.[ch] */
struct iomap;
#include "xfs_cksum.h"

#define unlikely(x) (x)

/*
 * This mirrors the kernel include for xfs_buf.h - it's implicitly included in
 * every files via a similar include in the kernel xfs_linux.h.
 */
#include "xfs_buf.h"

#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_errortag.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_inode.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_bmap_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_attr.h"
#include "xfs_attr_sf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode_buf.h"
#include "xfs_alloc.h"
#include "xfs_btree.h"
#include "xfs_bmap.h"
#include "xfs_trace.h"
#include "xfs_ag.h"
#include "xfs_rmap_btree.h"
#include "xfs_rmap.h"
#include "xfs_refcount_btree.h"
#include "xfs_refcount.h"
#include "xfs_btree_staging.h"

struct xfs_globals {
	int log_recovery_delay;
};
extern struct xfs_globals xfs_globals;

#define IS_ALIGNED(x, a)                (((x) & ((typeof(x))(a) - 1)) == 0)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef XFS_SUPER_MAGIC
#define XFS_SUPER_MAGIC 0x58465342
#endif

#define xfs_isset(a,i)	((a)[(i)/(sizeof(*(a))*NBBY)] & (1ULL<<((i)%(sizeof(*(a))*NBBY))))

/* Need to be able to handle this bare or in control flow */
static inline bool WARN_ON(bool expr) {
	return (expr);
}

#define WARN_ON_ONCE(e)			WARN_ON(e)

#define XFS_TEST_ERROR(expr, mp, tag) ((expr))

#define xfs_force_shutdown(d,n)		((void) 0)

#define XFS_STATS_INC(mp, count)	do { (mp) = (mp); } while (0)
#define XFS_STATS_DEC(mp, count, x)	do { (mp) = (mp); } while (0)
#define XFS_STATS_ADD(mp, count, x)	do { (mp) = (mp); } while (0)

/* TODO: chandan: Dummy mutex */
struct mutex {
	;
};

void mutex_lock(struct mutex *mutex) { return; }
void mutex_unlock(struct mutex *mutex) { return; }
int mutex_trylock(struct mutex *mutex) { return 1; }

/* TODO: chandan: Dummy completion */
struct completion {
	;
};

void complete(struct completion *completion) { return; }
void wait_for_completion(struct completion *completion) { return; }
int try_wait_for_completion(struct completion *completion) { return 1; }

#define uuid_equal(s, d) (platform_uuid_compare((s), (d)) == 0)
#define uuid_copy(s,d)		platform_uuid_copy((s),(d))

struct libxfs_dev {
	/* input parameters */
	char		*name;	/* pathname of the device */
	bool		isfile;	/* is the device a file? */
	bool		create;	/* create file if it doesn't exist */

	/* output parameters */
	dev_t		dev;	/* device name for the device */
	long long       size;	/* size of subvolume (BBs) */
	int		bsize;	/* device blksize */
	int		fd;	/* file descriptor */
};

/*
 * Argument structure for libxfs_init().
 */
struct libxfs_init {
	struct libxfs_dev	data;
	struct libxfs_dev	log;
	struct libxfs_dev	rt;

	/* input parameters */
	unsigned	flags;		/* LIBXFS_* flags below */
	int		bcache_flags;	/* cache init flags */
	int		setblksize;	/* value to set device blksizes to */
};

/* disallow all mounted filesystems: */
#define LIBXFS_ISREADONLY	(1U << 0)

/* allow mounted only if mounted ro: */
#define LIBXFS_ISINACTIVE	(1U << 1)

/* repairing a device mounted ro: */
#define LIBXFS_DANGEROUSLY	(1U << 2)

/* disallow other accesses (O_EXCL): */
#define LIBXFS_EXCLUSIVELY	(1U << 3)

/* can use direct I/O, not buffered: */
#define LIBXFS_DIRECT		(1U << 4)

/* lock xfs_buf's - for MT usage */
#define LIBXFS_USEBUFLOCK	(1U << 5)

extern char	*progname;
extern xfs_lsn_t libxfs_max_lsn;

int		libxfs_init(struct libxfs_init *);
void		libxfs_destroy(struct libxfs_init *li);

extern int	libxfs_device_alignment (void);
extern void	libxfs_report(FILE *);

/* check or write log footer: specify device, log size in blocks & uuid */
typedef char	*(libxfs_get_block_t)(char *, int, void *);

/*
 * Helpers to clear the log to a particular log cycle.
 */
#define XLOG_INIT_CYCLE	1
extern int	libxfs_log_clear(struct xfs_buftarg *, char *, xfs_daddr_t,
				 uint, uuid_t *, int, int, int, int, bool);
extern int	libxfs_log_header(char *, uuid_t *, int, int, int, xfs_lsn_t,
				  xfs_lsn_t, libxfs_get_block_t *, void *);

#define XFS_ILOCK_EXCL			0

#define xfs_ilock(ip,mode)				((void) 0)
#define xfs_ilock_data_map_shared(ip)			(0)
#define xfs_ilock_attr_map_shared(ip)			(0)
#define xfs_iunlock(ip,mode)				({	\
	typeof(mode) __mode = mode;				\
	__mode = __mode; /* no set-but-unused warning */	\
})
#define xfs_lock_two_inodes(ip0,mode0,ip1,mode1)	((void) 0)

/*
 * Starting in Linux 4.15, the %p (raw pointer value) printk modifier
 * prints a hashed version of the pointer to avoid leaking kernel
 * pointers into dmesg.  If we're trying to debug the kernel we want the
 * raw values, so override this behavior as best we can.
 *
 * In userspace we don't have this problem.
 */
#define PTR_FMT "%p"

#define XFS_IGET_CREATE			0x1
#define XFS_IGET_UNTRUSTED		0x2

extern void cmn_err(int, char *, ...);
enum ce { CE_DEBUG, CE_CONT, CE_NOTE, CE_WARN, CE_ALERT, CE_PANIC };

#define xfs_info(mp,fmt,args...)	cmn_err(CE_CONT, _(fmt), ## args)
#define xfs_notice(mp,fmt,args...)	cmn_err(CE_NOTE, _(fmt), ## args)
#define xfs_warn(mp,fmt,args...)	cmn_err((mp) ? CE_WARN : CE_WARN, _(fmt), ## args)
#define xfs_err(mp,fmt,args...)		cmn_err(CE_ALERT, _(fmt), ## args)
#define xfs_alert(mp, fmt, args...) cmn_err(CE_ALERT, _(fmt), ##args)


/* stop unused var warnings by assigning mp to itself */

static inline
void xfs_corruption_error(
	const char *tag,
	int level,
	struct xfs_mount *mp,
	const void *buf,
	size_t bufsize,
	const char *filename,
	int linenum,
	void *failaddr)
{
	cmn_err(CE_ALERT, "%s: XFS_CORRUPTION_ERROR", tag);
}

#define XFS_CORRUPTION_ERROR(e, lvl, mp, buf, bufsize) \
	xfs_corruption_error((e), (lvl), (mp), (buf), (bufsize), __FILE__, __LINE__, NULL)

void
xfs_error_report(
	const char		*tag,
	int			level,
	struct xfs_mount	*mp,
	const char		*filename,
	int			linenum,
	xfs_failaddr_t		failaddr)
{
	cmn_err(CE_ALERT, "%s: XFS_ERROR_REPORT", tag);
}

#define XFS_ERROR_REPORT(e,l,mp) \
	xfs_error_report((e), (l), (mp), __FILE__, __LINE__, NULL)

#define XFS_WARN_CORRUPT(mp, expr) \
	( xfs_is_reporting_corruption(mp) ? \
	   (printf("%s: XFS_WARN_CORRUPT at %s:%d", #expr, \
		   __func__, __LINE__), true) : true)

#define XFS_IS_CORRUPT(mp, expr)	\
	(unlikely(expr) ? XFS_WARN_CORRUPT((mp), (expr)) : false)

#define XFS_ERRLEVEL_LOW		1

/* Shared utility routines */

extern int	libxfs_alloc_file_space (struct xfs_inode *, xfs_off_t,
				xfs_off_t, int, int);

/* XXX: this is messy and needs fixing */
#ifndef __LIBXFS_INTERNAL_XFS_H__
extern void cmn_err(int, char *, ...);

#endif

#include "xfs_ialloc.h"

#include "xfs_attr_leaf.h"
#include "xfs_attr_remote.h"
#include "xfs_trans_space.h"

#define XFS_INOBT_IS_FREE_DISK(rp,i)		\
			((be64_to_cpu((rp)->ir_free) & XFS_INOBT_MASK(i)) != 0)

static inline bool
xfs_inobt_is_sparse_disk(
	struct xfs_inobt_rec	*rp,
	int			offset)
{
	int			spshift;
	uint16_t		holemask;

	holemask = be16_to_cpu(rp->ir_u.sp.ir_holemask);
	spshift = offset / XFS_INODES_PER_HOLEMASK_BIT;
	if ((1 << spshift) & holemask)
		return true;

	return false;
}

static inline void
libxfs_bmbt_disk_get_all(
	struct xfs_bmbt_rec	*rec,
	struct xfs_bmbt_irec	*irec)
{
	uint64_t		l0 = get_unaligned_be64(&rec->l0);
	uint64_t		l1 = get_unaligned_be64(&rec->l1);

	irec->br_startoff = (l0 & xfs_mask64lo(64 - BMBT_EXNTFLAG_BITLEN)) >> 9;
	irec->br_startblock = ((l0 & xfs_mask64lo(9)) << 43) | (l1 >> 21);
	irec->br_blockcount = l1 & xfs_mask64lo(21);
	if (l0 >> (64 - BMBT_EXNTFLAG_BITLEN))
		irec->br_state = XFS_EXT_UNWRITTEN;
	else
		irec->br_state = XFS_EXT_NORM;
}

/* XXX: this is clearly a bug - a shared header needs to export this */
/* xfs_rtalloc.c */
int libxfs_rtfree_extent(struct xfs_trans *, xfs_rtblock_t, xfs_extlen_t);
bool libxfs_verify_rtbno(struct xfs_mount *mp, xfs_rtblock_t rtbno);

#include "xfs_attr.h"
#include "topology.h"

static inline __attribute__((const))
int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

/*
 * xfs_iroundup: round up argument to next power of two
 */
static inline uint
roundup_pow_of_two(uint v)
{
	int	i;
	uint	m;

	if ((v & (v - 1)) == 0)
		return v;
	ASSERT((v & 0x80000000) == 0);
	if ((v & (v + 1)) == 0)
		return v + 1;
	for (i = 0, m = 1; i < 31; i++, m <<= 1) {
		if (v & m)
			continue;
		v |= m;
		if ((v & (v + 1)) == 0)
			return v + 1;
	}
	ASSERT(0);
	return 0;
}

/* local source files */
#define xfs_mod_fdblocks(mp, delta, rsvd) \
	libxfs_mod_incore_sb(mp, XFS_TRANS_SB_FDBLOCKS, delta, rsvd)
#define xfs_mod_frextents(mp, delta) \
	libxfs_mod_incore_sb(mp, XFS_TRANS_SB_FREXTENTS, delta, 0)
int  libxfs_mod_incore_sb(struct xfs_mount *, int, int64_t, int);

void xfs_reinit_percpu_counters(struct xfs_mount *mp);

/*
 * Superblock helpers for programs that act on independent superblock
 * structures.  These used to be part of xfs_format.h.
 */
static inline bool xfs_sb_version_haslazysbcount(struct xfs_sb *sbp)
{
	return (XFS_SB_VERSION_NUM(sbp) == XFS_SB_VERSION_5) ||
	       (xfs_sb_version_hasmorebits(sbp) &&
		(sbp->sb_features2 & XFS_SB_VERSION2_LAZYSBCOUNTBIT));
}

static inline bool xfs_sb_version_hascrc(struct xfs_sb *sbp)
{
	return XFS_SB_VERSION_NUM(sbp) == XFS_SB_VERSION_5;
}

static inline bool xfs_sb_version_hasmetauuid(struct xfs_sb *sbp)
{
	return (XFS_SB_VERSION_NUM(sbp) == XFS_SB_VERSION_5) &&
		(sbp->sb_features_incompat & XFS_SB_FEAT_INCOMPAT_META_UUID);
}

static inline bool xfs_sb_version_hasalign(struct xfs_sb *sbp)
{
	return (XFS_SB_VERSION_NUM(sbp) == XFS_SB_VERSION_5 ||
		(sbp->sb_versionnum & XFS_SB_VERSION_ALIGNBIT));
}

static inline bool xfs_sb_version_hasdalign(struct xfs_sb *sbp)
{
	return (sbp->sb_versionnum & XFS_SB_VERSION_DALIGNBIT);
}

static inline bool xfs_sb_version_haslogv2(struct xfs_sb *sbp)
{
	return XFS_SB_VERSION_NUM(sbp) == XFS_SB_VERSION_5 ||
	       (sbp->sb_versionnum & XFS_SB_VERSION_LOGV2BIT);
}

static inline bool xfs_sb_version_hassector(struct xfs_sb *sbp)
{
	return (sbp->sb_versionnum & XFS_SB_VERSION_SECTORBIT);
}

static inline bool xfs_sb_version_needsrepair(struct xfs_sb *sbp)
{
	return XFS_SB_VERSION_NUM(sbp) == XFS_SB_VERSION_5 &&
		(sbp->sb_features_incompat & XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR);
}

static inline bool xfs_sb_version_hassparseinodes(struct xfs_sb *sbp)
{
	return XFS_SB_VERSION_NUM(sbp) == XFS_SB_VERSION_5 &&
		xfs_sb_has_incompat_feature(sbp, XFS_SB_FEAT_INCOMPAT_SPINODES);
}

void libxfs_buftarg_init(struct xfs_mount *mp, struct libxfs_init *xi);

#endif	/* __LIBXFS_H__ */
