// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef __XFS_INODE_H__
#define __XFS_INODE_H__

/*
 * Borrow the kernel's uid/gid types.  These are used by xfs_inode_util.h, so
 * they must come first in the header file.
 */

typedef struct {
	uid_t val;
} kuid_t;

typedef struct {
	gid_t val;
} kgid_t;

static inline kuid_t make_kuid(uid_t uid)
{
	kuid_t	v = { .val = uid };
	return v;
}

static inline kgid_t make_kgid(gid_t gid)
{
	kgid_t	v = { .val = gid };
	return v;
}

#define KUIDT_INIT(value) (kuid_t){ value }
#define KGIDT_INIT(value) (kgid_t){ value }
#define GLOBAL_ROOT_UID KUIDT_INIT(0)
#define GLOBAL_ROOT_GID KGIDT_INIT(0)

/* These match kernel side includes */
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode_util.h"

struct xfs_trans;
struct xfs_mount;
struct xfs_inode_log_item;

/*
 * These are not actually used, they are only for userspace build
 * compatibility in code that looks at i_state
 */
#define I_DIRTY_TIME		0
#define I_DIRTY_TIME_EXPIRED	0

#define IS_I_VERSION(inode)			(0)
#define inode_maybe_inc_iversion(inode,flags)	(0)

/*
 * Inode interface. This fakes up a "VFS inode" to make the xfs_inode appear
 * similar to the kernel which now is used tohold certain parts of the on-disk
 * metadata.
 */
struct inode {
	mode_t			i_mode;
	kuid_t			i_uid;
	kgid_t			i_gid;
	uint32_t		i_nlink;
	xfs_dev_t		i_rdev;	 /* This actually holds xfs_dev_t */
	unsigned int		i_count;
	unsigned long		i_state; /* Not actually used in userspace */
	uint32_t		i_generation;
	uint64_t		i_version;
	struct timespec64	i_atime;
	struct timespec64	i_mtime;
	struct timespec64	i_ctime;
};

static inline uint32_t i_uid_read(struct inode *inode)
{
	return inode->i_uid.val;
}
static inline uint32_t i_gid_read(struct inode *inode)
{
	return inode->i_gid.val;
}
static inline void i_uid_write(struct inode *inode, uint32_t uid)
{
	inode->i_uid.val = uid;
}
static inline void i_gid_write(struct inode *inode, uint32_t gid)
{
	inode->i_gid.val = gid;
}

static inline void ihold(struct inode *inode)
{
	inode->i_count++;
}

static inline void
inode_fsuid_set(
	struct inode		*inode,
	struct user_namespace	*mnt_userns)
{
	inode->i_uid = make_kuid(0);
}

typedef struct xfs_inode {
	struct cache_node	i_node;
	struct xfs_mount	*i_mount;	/* fs mount struct ptr */
	xfs_ino_t		i_ino;		/* inode number (agno/agino) */
	struct xfs_imap		i_imap;		/* location for xfs_imap() */
	struct xfs_buftarg	i_dev;		/* dev for this inode */
	struct xfs_ifork	*i_afp;		/* attribute fork pointer */
	struct xfs_ifork	*i_cowfp;	/* copy on write extents */
	struct xfs_ifork	i_df;		/* data fork */
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

	xfs_extnum_t		i_cnextents;	/* # of extents in cow fork */
	unsigned int		i_cformat;	/* format of cow fork */

	xfs_fsize_t		i_size;		/* in-memory size */
	struct inode		i_vnode;
} xfs_inode_t;

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
static inline void drop_nlink(struct inode *inode)
{
	inode->i_nlink--;
}

static inline bool xfs_is_reflink_inode(struct xfs_inode *ip)
{
	return ip->i_diflags2 & XFS_DIFLAG2_REFLINK;
}

static inline bool xfs_inode_has_bigtime(struct xfs_inode *ip)
{
	return ip->i_diflags2 & XFS_DIFLAG2_BIGTIME;
}

static inline bool xfs_inode_has_bigrtextents(struct xfs_inode *ip)
{
	return XFS_IS_REALTIME_INODE(ip) && ip->i_mount->m_sb.sb_rextsize > 1;
}

static inline bool xfs_is_always_cow_inode(struct xfs_inode *ip)
{
	return false;
}

static inline bool xfs_is_metadata_inode(struct xfs_inode *ip)
{
	return ip->i_diflags2 & XFS_DIFLAG2_METADATA;
}

typedef struct cred {
	uid_t	cr_uid;
	gid_t	cr_gid;
} cred_t;

extern int	libxfs_dir_ialloc(struct xfs_trans **tpp,
				  const struct xfs_ialloc_args *args,
				  struct xfs_inode **ipp);
extern void	libxfs_trans_inode_alloc_buf (struct xfs_trans *,
				struct xfs_buf *);

extern void	libxfs_trans_ichgtime(struct xfs_trans *,
				struct xfs_inode *, int);
extern int	libxfs_iflush_int (struct xfs_inode *, struct xfs_buf *);

void xfs_ialloc_internal_args(struct xfs_ialloc_args *args, umode_t mode);

extern struct timespec64 current_time(struct inode *inode);

/* Inode Cache Interfaces */
extern int	libxfs_iget(struct xfs_mount *, struct xfs_trans *, xfs_ino_t,
				uint, struct xfs_inode **);
extern void	libxfs_irele(struct xfs_inode *ip);

#define XFS_DEFAULT_COWEXTSZ_HINT 32

#define XFS_INHERIT_GID(pip)		(VFS_I(pip)->i_mode & S_ISGID)

static inline void
inode_set_iversion(struct inode *inode, uint64_t version)
{
	inode->i_version = version;
}

#define xfs_inherit_noatime	(false)
#define xfs_inherit_nodump	(false)
#define xfs_inherit_sync	(false)
#define xfs_inherit_nosymlinks	(false)
#define xfs_inherit_nodefrag	(false)
#define irix_sgid_inherit	(false)
#define in_group_p(g)		(false)

#endif /* __XFS_INODE_H__ */
