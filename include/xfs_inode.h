// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef __XFS_INODE_H__
#define __XFS_INODE_H__

/* These match kernel side includes */
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"

struct xfs_trans;
struct xfs_mount;
struct xfs_inode_log_item;
struct inode;

/*
 * These are not actually used, they are only for userspace build
 * compatibility in code that looks at i_state
 */
#define I_DIRTY_TIME		0
#define I_DIRTY_TIME_EXPIRED	0

static inline bool IS_I_VERSION(const struct inode *inode) { return false; }
#define inode_maybe_inc_iversion(inode,flags)	(0)

/*
 * Inode interface. This fakes up a "VFS inode" to make the xfs_inode appear
 * similar to the kernel which now is used tohold certain parts of the on-disk
 * metadata.
 */
struct inode {
	mode_t			i_mode;
	uint32_t		i_uid;
	uint32_t		i_gid;
	uint32_t		i_nlink;
	xfs_dev_t		i_rdev;	 /* This actually holds xfs_dev_t */
	unsigned int		i_count;
	unsigned long		i_state; /* Not actually used in userspace */
	uint32_t		i_generation;
	uint64_t		i_version;
	struct timespec64	i_atime;
	struct timespec64	i_mtime;
	struct timespec64	__i_ctime; /* use inode_*_ctime accessors! */
	spinlock_t		i_lock;
};

static inline uint32_t i_uid_read(struct inode *inode)
{
	return inode->i_uid;
}
static inline uint32_t i_gid_read(struct inode *inode)
{
	return inode->i_gid;
}
static inline void i_uid_write(struct inode *inode, uint32_t uid)
{
	inode->i_uid = uid;
}
static inline void i_gid_write(struct inode *inode, uint32_t gid)
{
	inode->i_gid = gid;
}

static inline void ihold(struct inode *inode)
{
	inode->i_count++;
}

static inline struct timespec64 inode_get_ctime(const struct inode *inode)
{
	return inode->__i_ctime;
}

static inline struct timespec64 inode_set_ctime_to_ts(struct inode *inode,
						     struct timespec64 ts)
{
	inode->__i_ctime = ts;
	return ts;
}

extern struct timespec64 current_time(struct inode *inode);

static inline struct timespec64 inode_set_ctime_current(struct inode *inode)
{
	struct timespec64 now = current_time(inode);

	inode_set_ctime_to_ts(inode, now);
	return now;
}

/*
 * In-core inode flags.
 */
#define XFS_IRECLAIM            (1 << 0) /* started reclaiming this inode */
#define XFS_ISTALE              (1 << 1) /* inode has been staled */
#define XFS_IRECLAIMABLE        (1 << 2) /* inode can be reclaimed */
#define XFS_INEW                (1 << 3) /* inode has just been allocated */
#define XFS_IPRESERVE_DM_FIELDS (1 << 4) /* has legacy DMAPI fields set */
#define XFS_ITRUNCATED          (1 << 5) /* truncated down so flush-on-close */
#define XFS_IDIRTY_RELEASE      (1 << 6) /* dirty release already seen */
#define XFS_IFLUSHING           (1 << 7) /* inode is being flushed */
#define __XFS_IPINNED_BIT       8        /* wakeup key for zero pin count */
#define XFS_IPINNED             (1 << __XFS_IPINNED_BIT)
#define XFS_IEOFBLOCKS          (1 << 9) /* has the preallocblocks tag set */
#define XFS_NEED_INACTIVE       (1 << 10) /* see XFS_INACTIVATING below */
/*
 * If this unlinked inode is in the middle of recovery, don't let drop_inode
 * truncate and free the inode.  This can happen if we iget the inode during
 * log recovery to replay a bmap operation on the inode.
 */
#define XFS_IRECOVERY           (1 << 11)
#define XFS_ICOWBLOCKS          (1 << 12)/* has the cowblocks tag set */

/*
 * If we need to update on-disk metadata before this IRECLAIMABLE inode can be
 * freed, then NEED_INACTIVE will be set.  Once we start the updates, the
 * INACTIVATING bit will be set to keep iget away from this inode.  After the
 * inactivation completes, both flags will be cleared and the inode is a
 * plain old IRECLAIMABLE inode.
 */
#define XFS_INACTIVATING        (1 << 13)

/* Quotacheck is running but inode has not been added to quota counts. */
#define XFS_IQUOTAUNCHECKED (1 << 14)
/* All inode state flags related to inode reclaim. */
#define XFS_ALL_IRECLAIM_FLAGS  (XFS_IRECLAIMABLE | \
				 XFS_IRECLAIM | \
				 XFS_NEED_INACTIVE | \
				 XFS_INACTIVATING)
/*
 * Per-lifetime flags need to be reset when re-using a reclaimable inode during
 * inode lookup. This prevents unintended behaviour on the new inode from
 * ocurring.
 */
#define XFS_IRECLAIM_RESET_FLAGS        \
	(XFS_IRECLAIMABLE | XFS_IRECLAIM | \
	 XFS_IDIRTY_RELEASE | XFS_ITRUNCATED | XFS_NEED_INACTIVE | \
	 XFS_INACTIVATING | XFS_IQUOTAUNCHECKED)

/* TODO: chandan: Do we need to copy fs/xfs/xfs_inode.h from kernel to xfsprogs? */
typedef struct xfs_inode {
	struct cache_node	i_node;	   /* chandan: what is this used for? */
	struct xfs_mount	*i_mount;	/* fs mount struct ptr */
	struct xfs_dquot	*i_udquot;	/* user dquot */
	struct xfs_dquot	*i_gdquot;	/* group dquot */
	struct xfs_dquot	*i_pdquot;	/* project dquot */
	xfs_ino_t		i_ino;		/* inode number (agno/agino) */
	struct xfs_imap		i_imap;		/* location for xfs_imap() */
	struct xfs_buftarg	i_dev;		/* dev for this inode */
	struct xfs_ifork	*i_cowfp;	/* copy on write extents */
	struct xfs_ifork	i_df;		/* data fork */
	struct xfs_ifork	i_af;		/* attribute fork */
	struct xfs_inode_log_item *i_itemp;	/* logging information */
	unsigned int		i_delayed_blks;	/* count of delay alloc blks */
	xfs_fsize_t		i_disk_size;	/* number of bytes in file */
	xfs_rfsblock_t		i_nblocks;	/* # of direct & btree blocks */
	prid_t			i_projid;	/* owner's project id */
	xfs_extlen_t		i_extsize;	/* basic/minimum extent size */
	/* cowextsize is only used for v3 inodes, flushiter for v1/2 */
	union {
		xfs_extlen_t	i_cowextsize;	/* basic cow extent size */
		uint16_t	i_flushiter;	/* incremented on flush */
	};
	uint8_t			i_forkoff;	/* attr fork offset >> 3 */
	uint16_t		i_diflags;	/* XFS_DIFLAG_... */
	uint64_t		i_diflags2;	/* XFS_DIFLAG2_... */
	struct timespec64	i_crtime;	/* time created */

	/* unlinked list pointers */
	xfs_agino_t		i_next_unlinked;

	xfs_extnum_t		i_cnextents;	/* # of extents in cow fork */
	unsigned int		i_cformat;	/* format of cow fork */

	xfs_fsize_t		i_size;		/* in-memory size */
	atomic_t		i_pincount;
	unsigned long           i_flags;
	spinlock_t              i_flags_lock;
	struct inode		i_vnode;
} xfs_inode_t;

#define xfs_ipincount(ip)       ((unsigned int) atomic_read(&ip->i_pincount))

static inline bool xfs_inode_has_attr_fork(struct xfs_inode *ip)
{
	return ip->i_forkoff > 0;
}

static inline struct xfs_ifork *
xfs_ifork_ptr(
	struct xfs_inode	*ip,
	int			whichfork)
{
	switch (whichfork) {
	case XFS_DATA_FORK:
		return &ip->i_df;
	case XFS_ATTR_FORK:
		if (!xfs_inode_has_attr_fork(ip))
			return NULL;
		return &ip->i_af;
	case XFS_COW_FORK:
		return ip->i_cowfp;
	default:
		ASSERT(0);
		return NULL;
	}
}

static inline unsigned int xfs_inode_fork_boff(struct xfs_inode *ip)
{
	return ip->i_forkoff << 3;
}

static inline unsigned int xfs_inode_data_fork_size(struct xfs_inode *ip)
{
	if (xfs_inode_has_attr_fork(ip))
		return xfs_inode_fork_boff(ip);

	return XFS_LITINO(ip->i_mount);
}

static inline unsigned int xfs_inode_attr_fork_size(struct xfs_inode *ip)
{
	if (xfs_inode_has_attr_fork(ip))
		return XFS_LITINO(ip->i_mount) - xfs_inode_fork_boff(ip);
	return 0;
}

static inline unsigned int
xfs_inode_fork_size(
	struct xfs_inode	*ip,
	int			whichfork)
{
	switch (whichfork) {
	case XFS_DATA_FORK:
		return xfs_inode_data_fork_size(ip);
	case XFS_ATTR_FORK:
		return xfs_inode_attr_fork_size(ip);
	default:
		return 0;
	}
}

/* Convert from vfs inode to xfs inode */
static inline struct xfs_inode *XFS_I(struct inode *inode)
{
	return container_of(inode, struct xfs_inode, i_vnode);
}

/* convert from xfs inode to vfs inode */
static inline struct inode *VFS_I(struct xfs_inode *ip)
{
	return &ip->i_vnode;
}

/* We only have i_size in the xfs inode in userspace */
static inline loff_t i_size_read(struct inode *inode)
{
	return XFS_I(inode)->i_size;
}

/*
 * wrappers around the mode checks to simplify code
 */
static inline bool XFS_ISREG(struct xfs_inode *ip)
{
	return S_ISREG(VFS_I(ip)->i_mode);
}

static inline bool XFS_ISDIR(struct xfs_inode *ip)
{
	return S_ISDIR(VFS_I(ip)->i_mode);
}

/*
 * For regular files we only update the on-disk filesize when actually
 * writing data back to disk.  Until then only the copy in the VFS inode
 * is uptodate.
 */
static inline xfs_fsize_t XFS_ISIZE(struct xfs_inode *ip)
{
	if (XFS_ISREG(ip))
		return ip->i_size;
	return ip->i_disk_size;
}
#define XFS_IS_REALTIME_INODE(ip) ((ip)->i_diflags & XFS_DIFLAG_REALTIME)

/* inode link counts */
static inline void set_nlink(struct inode *inode, uint32_t nlink)
{
	inode->i_nlink = nlink;
}
static inline void inc_nlink(struct inode *inode)
{
	inode->i_nlink++;
}

static inline bool xfs_is_reflink_inode(struct xfs_inode *ip)
{
	return ip->i_diflags2 & XFS_DIFLAG2_REFLINK;
}

static inline bool xfs_inode_has_bigtime(struct xfs_inode *ip)
{
	return ip->i_diflags2 & XFS_DIFLAG2_BIGTIME;
}

static inline bool xfs_inode_has_large_extent_counts(struct xfs_inode *ip)
{
	return ip->i_diflags2 & XFS_DIFLAG2_NREXT64;
}

/* Always set the child's GID to this value, even if the parent is setgid. */
#define CRED_FORCE_GID	(1U << 0)
struct cred {
	uid_t		cr_uid;
	gid_t		cr_gid;
	unsigned int	cr_flags;
};

extern int	libxfs_dir_ialloc (struct xfs_trans **, struct xfs_inode *,
				mode_t, nlink_t, xfs_dev_t, struct cred *,
				struct fsxattr *, struct xfs_inode **);
extern void	libxfs_trans_inode_alloc_buf (struct xfs_trans *,
				struct xfs_buf *);

extern void	libxfs_trans_ichgtime(struct xfs_trans *,
				struct xfs_inode *, int);
extern int	libxfs_iflush_int (struct xfs_inode *, struct xfs_buf *);

/* Inode Cache Interfaces */
extern int	libxfs_iget(struct xfs_mount *, struct xfs_trans *, xfs_ino_t,
		uint, uint, struct xfs_inode **);
extern void	libxfs_irele(struct xfs_inode *ip);

#endif /* __XFS_INODE_H__ */
