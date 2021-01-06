// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs_priv.h"
#include "libxfs.h"
#include "libxfs_io.h"
#include "init.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode_buf.h"
#include "xfs_inode_fork.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_ialloc.h"
#include "xfs_alloc.h"
#include "xfs_bit.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_dir2_priv.h"

void
xfs_setup_inode(
	struct xfs_inode	*ip)
{
	/* empty */
}

/*
 * Set up an incore inode for a newly allocated ondisk inode and return it to
 * the caller locked exclusively.
 */
static int
xfs_inode_ialloc_iget(
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	struct xfs_inode	**ipp)
{
	struct xfs_mount	*mp = tp->t_mountp;

	return libxfs_iget(mp, tp, ino, 0, ipp);
}

/*
 * Writes a modified inode's changes out to the inode's on disk home.
 * Originally based on xfs_iflush_int() from xfs_inode.c in the kernel.
 */
int
libxfs_iflush_int(
	struct xfs_inode		*ip,
	struct xfs_buf			*bp)
{
	struct xfs_inode_log_item	*iip;
	struct xfs_dinode		*dip;
	struct xfs_mount		*mp;

	ASSERT(ip->i_df.if_format != XFS_DINODE_FMT_BTREE ||
		ip->i_df.if_nextents > ip->i_df.if_ext_max);

	iip = ip->i_itemp;
	mp = ip->i_mount;

	/* set *dip = inode's place in the buffer */
	dip = xfs_buf_offset(bp, ip->i_imap.im_boffset);

	if (XFS_ISREG(ip)) {
		ASSERT( (ip->i_df.if_format == XFS_DINODE_FMT_EXTENTS) ||
			(ip->i_df.if_format == XFS_DINODE_FMT_BTREE) );
	} else if (XFS_ISDIR(ip)) {
		ASSERT( (ip->i_df.if_format == XFS_DINODE_FMT_EXTENTS) ||
			(ip->i_df.if_format == XFS_DINODE_FMT_BTREE)   ||
			(ip->i_df.if_format == XFS_DINODE_FMT_LOCAL) );
	}
	ASSERT(ip->i_df.if_nextents+ip->i_afp->if_nextents <= ip->i_nblocks);
	ASSERT(ip->i_forkoff <= mp->m_sb.sb_inodesize);

	/* bump the change count on v3 inodes */
	if (xfs_sb_version_has_v3inode(&mp->m_sb))
		VFS_I(ip)->i_version++;

	/*
	 * If there are inline format data / attr forks attached to this inode,
	 * make sure they are not corrupt.
	 */
	if (ip->i_df.if_format == XFS_DINODE_FMT_LOCAL &&
	    xfs_ifork_verify_local_data(ip))
		return -EFSCORRUPTED;
	if (ip->i_afp && ip->i_afp->if_format == XFS_DINODE_FMT_LOCAL &&
	    xfs_ifork_verify_local_attr(ip))
		return -EFSCORRUPTED;

	/*
	 * Copy the dirty parts of the inode into the on-disk
	 * inode.  We always copy out the core of the inode,
	 * because if the inode is dirty at all the core must
	 * be.
	 */
	xfs_inode_to_disk(ip, dip, iip->ili_item.li_lsn);

	xfs_iflush_fork(ip, dip, iip, XFS_DATA_FORK);
	if (XFS_IFORK_Q(ip))
		xfs_iflush_fork(ip, dip, iip, XFS_ATTR_FORK);

	/* generate the checksum. */
	xfs_dinode_calc_crc(mp, dip);

	return 0;
}

/*
 * Wrapper around call to libxfs_ialloc. Takes care of committing and
 * allocating a new transaction as needed.
 *
 * Originally there were two copies of this code - one in mkfs, the
 * other in repair - now there is just the one.
 */
int
libxfs_dir_ialloc(
	struct xfs_trans	**tpp,
	const struct xfs_ialloc_args *args,
	struct xfs_inode	**ipp)
{
	struct xfs_inode	*dp = args->pip;
	struct xfs_buf		*agibp;
	xfs_ino_t		parent_ino = dp ? dp->i_ino : 0;
	xfs_ino_t		ino;
	int			error;

	ASSERT((*tpp)->t_flags & XFS_TRANS_PERM_LOG_RES);

	/*
	 * Call the space management code to pick the on-disk inode to be
	 * allocated.
	 */
	error = xfs_dialloc_select_ag(tpp, parent_ino, args->mode, &agibp);
	if (error)
		return error;

	if (!agibp)
		return -ENOSPC;

	/* Allocate an inode from the selected AG */
	error = xfs_dialloc_ag(*tpp, agibp, parent_ino, &ino);
	if (error)
		return error;
	ASSERT(ino != NULLFSINO);

	error = xfs_inode_ialloc_iget(*tpp, ino, ipp);
	if (error)
		return error;

	ASSERT(*ipp != NULL);
	xfs_inode_init(*tpp, args, *ipp);
	return 0;
}

/*
 * Inode cache stubs.
 */

struct kmem_zone		*xfs_inode_zone;
extern struct kmem_zone		*xfs_ili_zone;

int
libxfs_iget(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	uint			flags,
	struct xfs_inode	**ipp)
{
	struct xfs_inode	*ip;
	int			error = 0;

	ip = kmem_cache_zalloc(xfs_inode_zone, 0);
	if (!ip)
		return -ENOMEM;

	VFS_I(ip)->i_count = 1;
	ip->i_ino = ino;
	ip->i_mount = mp;
	ip->i_diflags2 = mp->m_ino_geo.new_diflags2;
	error = xfs_imap(mp, tp, ip->i_ino, &ip->i_imap, 0);
	if (error)
		goto out_destroy;

	/*
	 * For version 5 superblocks, if we are initialising a new inode and we
	 * are not utilising the XFS_MOUNT_IKEEP inode cluster mode, we can
	 * simply build the new inode core with a random generation number.
	 *
	 * For version 4 (and older) superblocks, log recovery is dependent on
	 * the di_flushiter field being initialised from the current on-disk
	 * value and hence we must also read the inode off disk even when
	 * initializing new inodes.
	 */
	if (xfs_sb_version_has_v3inode(&mp->m_sb) &&
	    (flags & XFS_IGET_CREATE) && !(mp->m_flags & XFS_MOUNT_IKEEP)) {
		VFS_I(ip)->i_generation = prandom_u32();
	} else {
		struct xfs_buf		*bp;

		error = xfs_imap_to_bp(mp, tp, &ip->i_imap, &bp);
		if (error)
			goto out_destroy;

		error = xfs_inode_from_disk(ip,
				xfs_buf_offset(bp, ip->i_imap.im_boffset));
		if (!error)
			xfs_buf_set_ref(bp, XFS_INO_REF);
		xfs_trans_brelse(tp, bp);

		if (error)
			goto out_destroy;
	}

	*ipp = ip;
	return 0;

out_destroy:
	kmem_cache_free(xfs_inode_zone, ip);
	*ipp = NULL;
	return error;
}

static void
libxfs_idestroy(
	struct xfs_inode	*ip)
{
	switch (VFS_I(ip)->i_mode & S_IFMT) {
		case S_IFREG:
		case S_IFDIR:
		case S_IFLNK:
			libxfs_idestroy_fork(&ip->i_df);
			break;
	}
	if (ip->i_afp) {
		libxfs_idestroy_fork(ip->i_afp);
		kmem_cache_free(xfs_ifork_zone, ip->i_afp);
	}
	if (ip->i_cowfp) {
		libxfs_idestroy_fork(ip->i_cowfp);
		kmem_cache_free(xfs_ifork_zone, ip->i_cowfp);
	}
}

void
libxfs_irele(
	struct xfs_inode	*ip)
{
	VFS_I(ip)->i_count--;

	if (VFS_I(ip)->i_count == 0) {
		ASSERT(ip->i_itemp == NULL);
		libxfs_idestroy(ip);
		kmem_cache_free(xfs_inode_zone, ip);
	}
}

void
inode_init_owner(
	struct user_namespace	*mnt_userns,
	struct inode		*inode,
	const struct inode	*dir,
	umode_t			mode)
{
	inode->i_uid = make_kuid(0);
	if (dir && dir->i_mode & S_ISGID) {
		inode->i_gid = dir->i_gid;

		/* Directories are special, and always inherit S_ISGID */
		if (S_ISDIR(mode))
			mode |= S_ISGID;
		else if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
			mode &= ~S_ISGID;
	} else
		inode->i_gid = make_kgid(0);
	inode->i_mode = mode;
}

/* Set up the inode allocation parameters for internal files. */
void
xfs_ialloc_internal_args(
	struct xfs_ialloc_args	*args,
	umode_t			mode)
{
	args->mnt_userns = NULL;
	args->uid = make_kuid(0);
	args->gid = make_kgid(0);
	args->prid = 0;
	args->mode = mode;
	args->flags = XFS_IALLOC_ARGS_FORCE_UID |
		      XFS_IALLOC_ARGS_FORCE_GID |
		      XFS_IALLOC_ARGS_FORCE_MODE;
}
