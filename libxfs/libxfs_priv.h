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

/* space allocation */
#define xfs_rotorstep				1
#define xfs_bmap_rtalloc(a)			(-ENOSYS)
#define xfs_get_extsz_hint(ip)			(0)
#define xfs_get_cowextsz_hint(ip)		(0)
#define xfs_inode_is_filestream(ip)		(0)
#define xfs_filestream_lookup_ag(ip)		(0)
#define xfs_filestream_new_ag(ip,ag)		(0)
#define xfs_filestream_select_ag(...)		(-ENOSYS)

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

/* Zones used in libxfs allocations that aren't in shared header files */
extern struct kmem_cache *xfs_buf_cache;
extern struct kmem_cache *xfs_inode_cache;

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

/* xfs_icache.c */
#define xfs_inode_set_cowblocks_tag(ip)	do { } while (0)
#define xfs_inode_set_eofblocks_tag(ip)	do { } while (0)

/* xfs_stats.h */
#define XFS_STATS_CALC_INDEX(member)	0
#define XFS_STATS_INC_OFF(mp, off)
#define XFS_STATS_ADD_OFF(mp, off, val)

unsigned int hweight8(unsigned int w);
unsigned int hweight32(unsigned int w);
unsigned int hweight64(__u64 w);

static inline int xfs_buf_hash_init(struct xfs_perag *pag) { return 0; }
static inline void xfs_buf_hash_destroy(struct xfs_perag *pag) { }

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

#endif	/* __LIBXFS_INTERNAL_XFS_H__ */
