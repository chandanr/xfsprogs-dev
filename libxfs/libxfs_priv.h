// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

/*
 * This header is effectively a "namespace multiplexor" for the
 * user level XFS code.  It provides all of the necessary stuff
 * such that we can build some parts of the XFS kernel code in
 * user space in a controlled fashion, and translates the names
 * used in the kernel into the names which libxfs is going to
 * make available to user tools.
 *
 * It should only ever be #include'd by XFS "kernel" code being
 * compiled in user space.
 *
 * Our goals here are to...
 *      o  "share" large amounts of complex code between user and
 *         kernel space;
 *      o  shield the user tools from changes in the bleeding
 *         edge kernel code, merging source changes when
 *         convenient and not immediately (no symlinks);
 *      o  i.e. be able to merge changes to the kernel source back
 *         into the affected user tools in a controlled fashion;
 *      o  provide a _minimalist_ life-support system for kernel
 *         code in user land, not the "everything + the kitchen
 *         sink" model which libsim had mutated into;
 *      o  allow the kernel code to be completely free of code
 *         specifically there to support the user level build.
 */

/*
 * define a guard and something we can check to determine what include context
 * we are running from.
 */
#ifndef __LIBXFS_INTERNAL_XFS_H__
#define __LIBXFS_INTERNAL_XFS_H__

#include "libxfs_api_defs.h"
#include "platform_defs.h"
#include "xfs.h"

#include "list.h"
#include "hlist.h"
#include "cache.h"
#include "bitops.h"
#include "kmem.h"
#include "libfrog/radix-tree.h"
#include "libfrog/bitmask.h"
#include "libfrog/div64.h"
#include "atomic.h"
#include "spinlock.h"

#include "xfs_types.h"
#include "xfs_arch.h"

#include "xfs_fs.h"
#include "libfrog/crc32c.h"

#include <sys/xattr.h>

/* Zones used in libxfs allocations that aren't in shared header files */
extern struct kmem_cache *xfs_buf_item_cache;
extern struct kmem_cache *xfs_ili_cache;
extern struct kmem_cache *xfs_buf_cache;
extern struct kmem_cache *xfs_inode_cache;
extern struct kmem_cache *xfs_trans_cache;

/* fake up iomap, (not) used in xfs_bmap.[ch] */
#define IOMAP_F_SHARED				0x04
#define xfs_bmbt_to_iomap(a, b, c, d, e, f)	((void) 0)

/* CRC stuff, buffer API dependent on it */
#define crc32c(c,p,l)	crc32c_le((c),(unsigned char const *)(p),(l))

/* fake up kernel's iomap, (not) used in xfs_bmap.[ch] */
struct iomap;

#define cancel_delayed_work_sync(work) do { } while(0)

#include "xfs_cksum.h"

/*
 * This mirrors the kernel include for xfs_buf.h - it's implicitly included in
 * every files via a similar include in the kernel xfs_linux.h.
 */
#include "libxfs_io.h"

/* for all the support code that uses progname in error messages */
extern char    *progname;

#undef ASSERT
#define ASSERT(ex) assert(ex)

/*
 * We have no need for the "linux" dev_t in userspace, so these
 * are no-ops, and an xfs_dev_t is stored in VFS_I(ip)->i_rdev
 */
#define xfs_to_linux_dev_t(dev)	dev
#define linux_to_xfs_dev_t(dev) dev

#ifndef EWRONGFS
#define EWRONGFS	EINVAL
#endif

#define xfs_error_level			0

#define STATIC				static

#define xfs_buf_ioerror_alert(bp,f)	((void) 0);

#define xfs_hex_dump(d,n)		((void) 0)
#define xfs_stack_trace()		((void) 0)

#define xfs_mod_delalloc(a,b) 		((void) 0)

#define __section(section)	__attribute__((__section__(section)))

#define xfs_printk_once(func, dev, fmt, ...)			\
({								\
	static bool __section(".data.once") __print_once;	\
	bool __ret_print_once = !__print_once;			\
								\
	if (!__print_once) {					\
		__print_once = true;				\
		func(dev, fmt, ##__VA_ARGS__);			\
	}							\
	unlikely(__ret_print_once);				\
})

#define xfs_info_once(dev, fmt, ...)				\
	xfs_printk_once(xfs_info, dev, fmt, ##__VA_ARGS__)

/* miscellaneous kernel routines not in user space */
#define likely(x)		(x)
#define unlikely(x)		(x)

/*
 * get_random_u32 is used for di_gen inode allocation, it must be zero for
 * libxfs or all sorts of badness can occur!
 */
#define get_random_u32()	(0)

#define PAGE_SIZE		getpagesize()

#define inode_peek_iversion(inode)	(inode)->i_version
#define inode_set_iversion_queried(inode, version) do { \
	(inode)->i_version = (version);	\
} while (0)

/**
 * swap - swap values of @a and @b
 * @a: first value
 * @b: second value
 */
#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

static inline int
xfs_buf_incore(
	struct xfs_buftarg	*target,
	xfs_daddr_t		blkno,
	size_t			numblks,
	xfs_buf_flags_t		flags,
	struct xfs_buf		**bpp)
{
	*bpp = NULL;
	return -ENOENT;
}

#define xfs_buf_oneshot(bp)		((void) 0)

#define xfs_buf_zero(bp, off, len) \
	memset((bp)->b_addr + off, 0, len);

void __xfs_buf_mark_corrupt(struct xfs_buf *bp, xfs_failaddr_t fa);
#define xfs_buf_mark_corrupt(bp) __xfs_buf_mark_corrupt((bp), __this_address)

/* no readahead, need to avoid set-but-unused var warnings. */
#define xfs_buf_readahead(a,d,c,ops)		({	\
	xfs_daddr_t __d = d;				\
	__d = __d; /* no set-but-unused warning */	\
})
#define xfs_buf_readahead_map(a,b,c,ops)	((void) 0)	/* no readahead */

#define xfs_sort					qsort

/* space allocation */
#define XFS_EXTENT_BUSY_DISCARDED	0x01	/* undergoing a discard op. */
#define XFS_EXTENT_BUSY_SKIP_DISCARD	0x02	/* do not discard */

#define xfs_rotorstep				1
#define xfs_bmap_rtalloc(a)			(-ENOSYS)
#define xfs_get_extsz_hint(ip)			(0)
#define xfs_get_cowextsz_hint(ip)		(0)
#define xfs_inode_is_filestream(ip)		(0)
#define xfs_filestream_lookup_ag(ip)		(0)
#define xfs_filestream_new_ag(ip,ag)		(0)
#define xfs_filestream_select_ag(...)		(-ENOSYS)

/* hack too silence gcc */
static inline int retzero(void) { return 0; }
#define xfs_trans_unreserve_quota_nblks(t,i,b,n,f)	retzero()

#define xfs_icreate_log(tp, agno, agbno, cnt, isize, len, gen) ((void) 0)
#define xfs_sb_validate_fsb_count(sbp, nblks)		(0)

/*
 * Prototypes for kernel static functions that are aren't in their
 * associated header files.
 */
struct xfs_da_args;
struct xfs_bmap_free;
struct xfs_bmap_free_item;
struct xfs_mount;
struct xfs_sb;
struct xfs_trans;
struct xfs_inode;
struct xfs_log_item;
struct xfs_buf;
struct xfs_buf_map;
struct xfs_buf_log_item;
struct xfs_buftarg;

/* xfs_attr.c */
int xfs_attr_rmtval_get(struct xfs_da_args *);

/* xfs_bmap.c */
void xfs_bmap_del_free(struct xfs_bmap_free *, struct xfs_bmap_free_item *);

/* xfs_mount.c */
void xfs_mount_common(struct xfs_mount *, struct xfs_sb *);

/*
 * logitem.c and trans.c prototypes
 */
void xfs_trans_init(struct xfs_mount *);
int xfs_trans_roll(struct xfs_trans **);

/* xfs_trans_item.c */
void xfs_trans_add_item(struct xfs_trans *, struct xfs_log_item *);
void xfs_trans_del_item(struct xfs_log_item *);

void xfs_trans_mod_sb(struct xfs_trans *, uint, long);

void xfs_verifier_error(struct xfs_buf *bp, int error,
			xfs_failaddr_t failaddr);
void xfs_inode_verifier_error(struct xfs_inode *ip, int error,
			const char *name, void *buf, size_t bufsz,
			xfs_failaddr_t failaddr);

#define xfs_buf_verifier_error(bp,e,n,bu,bus,fa) \
	xfs_verifier_error(bp, e, fa)
void
xfs_buf_corruption_error(struct xfs_buf *bp, xfs_failaddr_t fa);

/* XXX: this is clearly a bug - a shared header needs to export this */
/* xfs_rtalloc.c */
int libxfs_rtfree_extent(struct xfs_trans *, xfs_rtblock_t, xfs_extlen_t);
bool libxfs_verify_rtbno(struct xfs_mount *mp, xfs_rtblock_t rtbno);

struct xfs_rtalloc_rec {
	xfs_rtblock_t		ar_startext;
	xfs_rtblock_t		ar_extcount;
};

typedef int (*xfs_rtalloc_query_range_fn)(
	struct xfs_mount		*mp,
	struct xfs_trans		*tp,
	const struct xfs_rtalloc_rec	*rec,
	void				*priv);

int libxfs_zero_extent(struct xfs_inode *ip, xfs_fsblock_t start_fsb,
                        xfs_off_t count_fsb);

/* xfs_log.c */
struct xfs_item_ops;
bool xfs_log_check_lsn(struct xfs_mount *, xfs_lsn_t);

#define xfs_log_in_recovery(mp)		(false)

/* xfs_icache.c */
#define xfs_inode_set_cowblocks_tag(ip)	do { } while (0)
#define xfs_inode_set_eofblocks_tag(ip)	do { } while (0)

/* xfs_stats.h */
#define XFS_STATS_CALC_INDEX(member)	0
#define XFS_STATS_INC_OFF(mp, off)
#define XFS_STATS_ADD_OFF(mp, off, val)

typedef unsigned char u8;
unsigned int hweight8(unsigned int w);
unsigned int hweight32(unsigned int w);
unsigned int hweight64(__u64 w);

static inline int xfs_buf_hash_init(struct xfs_perag *pag) { return 0; }
static inline void xfs_buf_hash_destroy(struct xfs_perag *pag) { }

static inline int xfs_iunlink_init(struct xfs_perag *pag) { return 0; }
static inline void xfs_iunlink_destroy(struct xfs_perag *pag) { }

xfs_agnumber_t xfs_set_inode_alloc(struct xfs_mount *mp,
		xfs_agnumber_t agcount);

/* Keep static checkers quiet about nonstatic functions by exporting */
int xfs_rtbuf_get(struct xfs_mount *mp, struct xfs_trans *tp,
		  xfs_rtblock_t block, int issum, struct xfs_buf **bpp);
int xfs_rtcheck_range(struct xfs_mount *mp, struct xfs_trans *tp,
		      xfs_rtblock_t start, xfs_extlen_t len, int val,
		      xfs_rtblock_t *new, int *stat);
int xfs_rtfind_back(struct xfs_mount *mp, struct xfs_trans *tp,
		    xfs_rtblock_t start, xfs_rtblock_t limit,
		    xfs_rtblock_t *rtblock);
int xfs_rtfind_forw(struct xfs_mount *mp, struct xfs_trans *tp,
		    xfs_rtblock_t start, xfs_rtblock_t limit,
		    xfs_rtblock_t *rtblock);
int xfs_rtmodify_range(struct xfs_mount *mp, struct xfs_trans *tp,
		       xfs_rtblock_t start, xfs_extlen_t len, int val);
int xfs_rtmodify_summary_int(struct xfs_mount *mp, struct xfs_trans *tp,
			     int log, xfs_rtblock_t bbno, int delta,
			     struct xfs_buf **rbpp, xfs_fsblock_t *rsb,
			     xfs_suminfo_t *sum);
int xfs_rtmodify_summary(struct xfs_mount *mp, struct xfs_trans *tp, int log,
			 xfs_rtblock_t bbno, int delta, struct xfs_buf **rbpp,
			 xfs_fsblock_t *rsb);
int xfs_rtfree_range(struct xfs_mount *mp, struct xfs_trans *tp,
		     xfs_rtblock_t start, xfs_extlen_t len,
		     struct xfs_buf **rbpp, xfs_fsblock_t *rsb);
int xfs_rtalloc_query_range(struct xfs_mount *mp,
			    struct xfs_trans *tp,
			    const struct xfs_rtalloc_rec *low_rec,
			    const struct xfs_rtalloc_rec *high_rec,
			    xfs_rtalloc_query_range_fn fn,
			    void *priv);
int xfs_rtalloc_query_all(struct xfs_mount *mp,
			  struct xfs_trans *tp,
			  xfs_rtalloc_query_range_fn fn,
			  void *priv);
bool xfs_verify_rtbno(struct xfs_mount *mp, xfs_rtblock_t rtbno);
int xfs_rtalloc_extent_is_free(struct xfs_mount *mp, struct xfs_trans *tp,
			       xfs_rtblock_t start, xfs_extlen_t len,
			       bool *is_free);
/* xfs_bmap_util.h */
struct xfs_bmalloca;
int xfs_bmap_extsize_align(struct xfs_mount *mp, struct xfs_bmbt_irec *gotp,
			   struct xfs_bmbt_irec *prevp, xfs_extlen_t extsz,
			   int rt, int eof, int delay, int convert,
			   xfs_fileoff_t *offp, xfs_extlen_t *lenp);
void xfs_bmap_adjacent(struct xfs_bmalloca *ap);
int xfs_bmap_last_extent(struct xfs_trans *tp, struct xfs_inode *ip,
			 int whichfork, struct xfs_bmbt_irec *rec,
			 int *is_empty);

/* xfs_inode.h */

#endif	/* __LIBXFS_INTERNAL_XFS_H__ */
