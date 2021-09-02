// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_errortag.h"
#include "xfs_trace.h"
#include "xfs_health.h"
#include "xfs_shared.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_ag.h"
#include "xfs_ialloc.h"

struct xfs_name xfs_name_dotdot = { (unsigned char *)"..", 2, XFS_DIR3_FT_DIR };

/*
 * Convert inode mode to directory entry filetype
 */
unsigned char
xfs_mode_to_ftype(
	int		mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
		return XFS_DIR3_FT_REG_FILE;
	case S_IFDIR:
		return XFS_DIR3_FT_DIR;
	case S_IFCHR:
		return XFS_DIR3_FT_CHRDEV;
	case S_IFBLK:
		return XFS_DIR3_FT_BLKDEV;
	case S_IFIFO:
		return XFS_DIR3_FT_FIFO;
	case S_IFSOCK:
		return XFS_DIR3_FT_SOCK;
	case S_IFLNK:
		return XFS_DIR3_FT_SYMLINK;
	default:
		return XFS_DIR3_FT_UNKNOWN;
	}
}

/*
 * ASCII case-insensitive (ie. A-Z) support for directories that was
 * used in IRIX.
 */
xfs_dahash_t
xfs_ascii_ci_hashname(
	struct xfs_name	*name)
{
	xfs_dahash_t	hash;
	int		i;

	for (i = 0, hash = 0; i < name->len; i++)
		hash = tolower(name->name[i]) ^ rol32(hash, 7);

	return hash;
}

enum xfs_dacmp
xfs_ascii_ci_compname(
	struct xfs_da_args	*args,
	const unsigned char	*name,
	int			len)
{
	enum xfs_dacmp		result;
	int			i;

	if (args->namelen != len)
		return XFS_CMP_DIFFERENT;

	result = XFS_CMP_EXACT;
	for (i = 0; i < len; i++) {
		if (args->name[i] == name[i])
			continue;
		if (tolower(args->name[i]) != tolower(name[i]))
			return XFS_CMP_DIFFERENT;
		result = XFS_CMP_CASE;
	}

	return result;
}

int
xfs_da_mount(
	struct xfs_mount	*mp)
{
	struct xfs_da_geometry	*dageo;


	ASSERT(mp->m_sb.sb_versionnum & XFS_SB_VERSION_DIRV2BIT);
	ASSERT(xfs_dir2_dirblock_bytes(&mp->m_sb) <= XFS_MAX_BLOCKSIZE);

	mp->m_dir_geo = kmem_zalloc(sizeof(struct xfs_da_geometry),
				    KM_MAYFAIL);
	mp->m_attr_geo = kmem_zalloc(sizeof(struct xfs_da_geometry),
				     KM_MAYFAIL);
	if (!mp->m_dir_geo || !mp->m_attr_geo) {
		kmem_free(mp->m_dir_geo);
		kmem_free(mp->m_attr_geo);
		return -ENOMEM;
	}

	/* set up directory geometry */
	dageo = mp->m_dir_geo;
	dageo->blklog = mp->m_sb.sb_blocklog + mp->m_sb.sb_dirblklog;
	dageo->fsblog = mp->m_sb.sb_blocklog;
	dageo->blksize = xfs_dir2_dirblock_bytes(&mp->m_sb);
	dageo->fsbcount = 1 << mp->m_sb.sb_dirblklog;
	if (xfs_has_crc(mp)) {
		dageo->node_hdr_size = sizeof(struct xfs_da3_node_hdr);
		dageo->leaf_hdr_size = sizeof(struct xfs_dir3_leaf_hdr);
		dageo->free_hdr_size = sizeof(struct xfs_dir3_free_hdr);
		dageo->data_entry_offset =
				sizeof(struct xfs_dir3_data_hdr);
	} else {
		dageo->node_hdr_size = sizeof(struct xfs_da_node_hdr);
		dageo->leaf_hdr_size = sizeof(struct xfs_dir2_leaf_hdr);
		dageo->free_hdr_size = sizeof(struct xfs_dir2_free_hdr);
		dageo->data_entry_offset =
				sizeof(struct xfs_dir2_data_hdr);
	}
	dageo->leaf_max_ents = (dageo->blksize - dageo->leaf_hdr_size) /
			sizeof(struct xfs_dir2_leaf_entry);
	dageo->free_max_bests = (dageo->blksize - dageo->free_hdr_size) /
			sizeof(xfs_dir2_data_off_t);

	dageo->data_first_offset = dageo->data_entry_offset +
			xfs_dir2_data_entsize(mp, 1) +
			xfs_dir2_data_entsize(mp, 2);

	/*
	 * Now we've set up the block conversion variables, we can calculate the
	 * segment block constants using the geometry structure.
	 */
	dageo->datablk = xfs_dir2_byte_to_da(dageo, XFS_DIR2_DATA_OFFSET);
	dageo->leafblk = xfs_dir2_byte_to_da(dageo, XFS_DIR2_LEAF_OFFSET);
	dageo->freeblk = xfs_dir2_byte_to_da(dageo, XFS_DIR2_FREE_OFFSET);
	dageo->node_ents = (dageo->blksize - dageo->node_hdr_size) /
				(uint)sizeof(xfs_da_node_entry_t);
	dageo->magicpct = (dageo->blksize * 37) / 100;

	/* set up attribute geometry - single fsb only */
	dageo = mp->m_attr_geo;
	dageo->blklog = mp->m_sb.sb_blocklog;
	dageo->fsblog = mp->m_sb.sb_blocklog;
	dageo->blksize = 1 << dageo->blklog;
	dageo->fsbcount = 1;
	dageo->node_hdr_size = mp->m_dir_geo->node_hdr_size;
	dageo->node_ents = (dageo->blksize - dageo->node_hdr_size) /
				(uint)sizeof(xfs_da_node_entry_t);
	dageo->magicpct = (dageo->blksize * 37) / 100;
	return 0;
}

void
xfs_da_unmount(
	struct xfs_mount	*mp)
{
	kmem_free(mp->m_dir_geo);
	kmem_free(mp->m_attr_geo);
}

/*
 * Return 1 if directory contains only "." and "..".
 */
int
xfs_dir_isempty(
	xfs_inode_t	*dp)
{
	xfs_dir2_sf_hdr_t	*sfp;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	if (dp->i_disk_size == 0)	/* might happen during shutdown. */
		return 1;
	if (dp->i_disk_size > XFS_IFORK_DSIZE(dp))
		return 0;
	sfp = (xfs_dir2_sf_hdr_t *)dp->i_df.if_u1.if_data;
	return !sfp->count;
}

/*
 * Validate a given inode number.
 */
int
xfs_dir_ino_validate(
	xfs_mount_t	*mp,
	xfs_ino_t	ino)
{
	bool		ino_ok = xfs_verify_dir_ino(mp, ino);

	if (XFS_IS_CORRUPT(mp, !ino_ok) ||
	    XFS_TEST_ERROR(false, mp, XFS_ERRTAG_DIR_INO_VALIDATE)) {
		xfs_warn(mp, "Invalid inode number 0x%Lx",
				(unsigned long long) ino);
		return -EFSCORRUPTED;
	}
	return 0;
}

/*
 * Initialize a directory with its "." and ".." entries.
 */
int
xfs_dir_init(
	xfs_trans_t	*tp,
	xfs_inode_t	*dp,
	xfs_inode_t	*pdp)
{
	struct xfs_da_args *args;
	int		error;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	error = xfs_dir_ino_validate(tp->t_mountp, pdp->i_ino);
	if (error)
		return error;

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->dp = dp;
	args->trans = tp;
	error = xfs_dir2_sf_create(args, pdp->i_ino);
	kmem_free(args);
	return error;
}

/*
 * Enter a name in a directory, or check for available space.
 * If inum is 0, only the available space test is performed.
 */
int
xfs_dir_createname(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	struct xfs_name		*name,
	xfs_ino_t		inum,		/* new entry inode number */
	xfs_extlen_t		total)		/* bmap's total block count */
{
	struct xfs_da_args	*args;
	int			rval;
	int			v;		/* type-checking value */

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));

	if (inum) {
		rval = xfs_dir_ino_validate(tp->t_mountp, inum);
		if (rval)
			return rval;
		XFS_STATS_INC(dp->i_mount, xs_dir_create);
	}

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = xfs_dir2_hashname(dp->i_mount, name);
	args->inumber = inum;
	args->dp = dp;
	args->total = total;
	args->whichfork = XFS_DATA_FORK;
	args->trans = tp;
	args->op_flags = XFS_DA_OP_ADDNAME | XFS_DA_OP_OKNOENT;
	if (!inum)
		args->op_flags |= XFS_DA_OP_JUSTCHECK;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		rval = xfs_dir2_sf_addname(args);
		goto out_free;
	}

	rval = xfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = xfs_dir2_block_addname(args);
		goto out_free;
	}

	rval = xfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = xfs_dir2_leaf_addname(args);
	else
		rval = xfs_dir2_node_addname(args);

out_free:
	kmem_free(args);
	return rval;
}

/*
 * If doing a CI lookup and case-insensitive match, dup actual name into
 * args.value. Return EEXIST for success (ie. name found) or an error.
 */
int
xfs_dir_cilookup_result(
	struct xfs_da_args *args,
	const unsigned char *name,
	int		len)
{
	if (args->cmpresult == XFS_CMP_DIFFERENT)
		return -ENOENT;
	if (args->cmpresult != XFS_CMP_CASE ||
					!(args->op_flags & XFS_DA_OP_CILOOKUP))
		return -EEXIST;

	args->value = kmem_alloc(len, KM_NOFS | KM_MAYFAIL);
	if (!args->value)
		return -ENOMEM;

	memcpy(args->value, name, len);
	args->valuelen = len;
	return -EEXIST;
}

/*
 * Lookup a name in a directory, give back the inode number.
 * If ci_name is not NULL, returns the actual name in ci_name if it differs
 * to name, or ci_name->name is set to NULL for an exact match.
 */

int
xfs_dir_lookup(
	xfs_trans_t	*tp,
	xfs_inode_t	*dp,
	struct xfs_name	*name,
	xfs_ino_t	*inum,		/* out: inode number */
	struct xfs_name *ci_name)	/* out: actual name if CI match */
{
	struct xfs_da_args *args;
	int		rval;
	int		v;		/* type-checking value */
	int		lock_mode;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	XFS_STATS_INC(dp->i_mount, xs_dir_lookup);

	/*
	 * We need to use KM_NOFS here so that lockdep will not throw false
	 * positive deadlock warnings on a non-transactional lookup path. It is
	 * safe to recurse into inode recalim in that case, but lockdep can't
	 * easily be taught about it. Hence KM_NOFS avoids having to add more
	 * lockdep Doing this avoids having to add a bunch of lockdep class
	 * annotations into the reclaim path for the ilock.
	 */
	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = xfs_dir2_hashname(dp->i_mount, name);
	args->dp = dp;
	args->whichfork = XFS_DATA_FORK;
	args->trans = tp;
	args->op_flags = XFS_DA_OP_OKNOENT;
	if (ci_name)
		args->op_flags |= XFS_DA_OP_CILOOKUP;

	lock_mode = xfs_ilock_data_map_shared(dp);
	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		rval = xfs_dir2_sf_lookup(args);
		goto out_check_rval;
	}

	rval = xfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = xfs_dir2_block_lookup(args);
		goto out_check_rval;
	}

	rval = xfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = xfs_dir2_leaf_lookup(args);
	else
		rval = xfs_dir2_node_lookup(args);

out_check_rval:
	if (rval == -EEXIST)
		rval = 0;
	if (!rval) {
		*inum = args->inumber;
		name->type = args->filetype;
		if (ci_name) {
			ci_name->name = args->value;
			ci_name->len = args->valuelen;
		}
	}
out_free:
	xfs_iunlock(dp, lock_mode);
	kmem_free(args);
	return rval;
}

/*
 * Remove an entry from a directory.
 */
int
xfs_dir_removename(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	struct xfs_name		*name,
	xfs_ino_t		ino,
	xfs_extlen_t		total)		/* bmap's total block count */
{
	struct xfs_da_args	*args;
	int			rval;
	int			v;		/* type-checking value */

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	XFS_STATS_INC(dp->i_mount, xs_dir_remove);

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = xfs_dir2_hashname(dp->i_mount, name);
	args->inumber = ino;
	args->dp = dp;
	args->total = total;
	args->whichfork = XFS_DATA_FORK;
	args->trans = tp;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		rval = xfs_dir2_sf_removename(args);
		goto out_free;
	}

	rval = xfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = xfs_dir2_block_removename(args);
		goto out_free;
	}

	rval = xfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = xfs_dir2_leaf_removename(args);
	else
		rval = xfs_dir2_node_removename(args);
out_free:
	kmem_free(args);
	return rval;
}

/*
 * Replace the inode number of a directory entry.
 */
int
xfs_dir_replace(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	struct xfs_name		*name,		/* name of entry to replace */
	xfs_ino_t		inum,		/* new inode number */
	xfs_extlen_t		total)		/* bmap's total block count */
{
	struct xfs_da_args	*args;
	int			rval;
	int			v;		/* type-checking value */

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));

	rval = xfs_dir_ino_validate(tp->t_mountp, inum);
	if (rval)
		return rval;

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = xfs_dir2_hashname(dp->i_mount, name);
	args->inumber = inum;
	args->dp = dp;
	args->total = total;
	args->whichfork = XFS_DATA_FORK;
	args->trans = tp;

	if (dp->i_df.if_format == XFS_DINODE_FMT_LOCAL) {
		rval = xfs_dir2_sf_replace(args);
		goto out_free;
	}

	rval = xfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = xfs_dir2_block_replace(args);
		goto out_free;
	}

	rval = xfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = xfs_dir2_leaf_replace(args);
	else
		rval = xfs_dir2_node_replace(args);
out_free:
	kmem_free(args);
	return rval;
}

/*
 * See if this entry can be added to the directory without allocating space.
 */
int
xfs_dir_canenter(
	xfs_trans_t	*tp,
	xfs_inode_t	*dp,
	struct xfs_name	*name)		/* name of entry to add */
{
	return xfs_dir_createname(tp, dp, name, 0, 0);
}

/*
 * Utility routines.
 */

/*
 * Add a block to the directory.
 *
 * This routine is for data and free blocks, not leaf/node blocks which are
 * handled by xfs_da_grow_inode.
 */
int
xfs_dir2_grow_inode(
	struct xfs_da_args	*args,
	int			space,	/* v2 dir's space XFS_DIR2_xxx_SPACE */
	xfs_dir2_db_t		*dbp)	/* out: block number added */
{
	struct xfs_inode	*dp = args->dp;
	struct xfs_mount	*mp = dp->i_mount;
	xfs_fileoff_t		bno;	/* directory offset of new block */
	int			count;	/* count of filesystem blocks */
	int			error;

	trace_xfs_dir2_grow_inode(args, space);

	/*
	 * Set lowest possible block in the space requested.
	 */
	bno = XFS_B_TO_FSBT(mp, space * XFS_DIR2_SPACE_SIZE);
	count = args->geo->fsbcount;

	error = xfs_da_grow_inode_int(args, &bno, count);
	if (error)
		return error;

	*dbp = xfs_dir2_da_to_db(args->geo, (xfs_dablk_t)bno);

	/*
	 * Update file's size if this is the data space and it grew.
	 */
	if (space == XFS_DIR2_DATA_SPACE) {
		xfs_fsize_t	size;		/* directory file (data) size */

		size = XFS_FSB_TO_B(mp, bno + count);
		if (size > dp->i_disk_size) {
			dp->i_disk_size = size;
			xfs_trans_log_inode(args->trans, dp, XFS_ILOG_CORE);
		}
	}
	return 0;
}

/*
 * See if the directory is a single-block form directory.
 */
int
xfs_dir2_isblock(
	struct xfs_da_args	*args,
	int			*vp)	/* out: 1 is block, 0 is not block */
{
	xfs_fileoff_t		last;	/* last file offset */
	int			rval;

	if ((rval = xfs_bmap_last_offset(args->dp, &last, XFS_DATA_FORK)))
		return rval;
	rval = XFS_FSB_TO_B(args->dp->i_mount, last) == args->geo->blksize;
	if (XFS_IS_CORRUPT(args->dp->i_mount,
			   rval != 0 &&
			   args->dp->i_disk_size != args->geo->blksize)) {
		xfs_da_mark_sick(args);
		return -EFSCORRUPTED;
	}
	*vp = rval;
	return 0;
}

/*
 * See if the directory is a single-leaf form directory.
 */
int
xfs_dir2_isleaf(
	struct xfs_da_args	*args,
	int			*vp)	/* out: 1 is block, 0 is not block */
{
	xfs_fileoff_t		last;	/* last file offset */
	int			rval;

	if ((rval = xfs_bmap_last_offset(args->dp, &last, XFS_DATA_FORK)))
		return rval;
	*vp = last == args->geo->leafblk + args->geo->fsbcount;
	return 0;
}

/*
 * Remove the given block from the directory.
 * This routine is used for data and free blocks, leaf/node are done
 * by xfs_da_shrink_inode.
 */
int
xfs_dir2_shrink_inode(
	struct xfs_da_args	*args,
	xfs_dir2_db_t		db,
	struct xfs_buf		*bp)
{
	xfs_fileoff_t		bno;		/* directory file offset */
	xfs_dablk_t		da;		/* directory file offset */
	int			done;		/* bunmap is finished */
	struct xfs_inode	*dp;
	int			error;
	struct xfs_mount	*mp;
	struct xfs_trans	*tp;

	trace_xfs_dir2_shrink_inode(args, db);

	dp = args->dp;
	mp = dp->i_mount;
	tp = args->trans;
	da = xfs_dir2_db_to_da(args->geo, db);

	/* Unmap the fsblock(s). */
	error = xfs_bunmapi(tp, dp, da, args->geo->fsbcount, 0, 0, &done);
	if (error) {
		/*
		 * ENOSPC actually can happen if we're in a removename with no
		 * space reservation, and the resulting block removal would
		 * cause a bmap btree split or conversion from extents to btree.
		 * This can only happen for un-fragmented directory blocks,
		 * since you need to be punching out the middle of an extent.
		 * In this case we need to leave the block in the file, and not
		 * binval it.  So the block has to be in a consistent empty
		 * state and appropriately logged.  We don't free up the buffer,
		 * the caller can tell it hasn't happened since it got an error
		 * back.
		 */
		return error;
	}
	ASSERT(done);
	/*
	 * Invalidate the buffer from the transaction.
	 */
	xfs_trans_binval(tp, bp);
	/*
	 * If it's not a data block, we're done.
	 */
	if (db >= xfs_dir2_byte_to_db(args->geo, XFS_DIR2_LEAF_OFFSET))
		return 0;
	/*
	 * If the block isn't the last one in the directory, we're done.
	 */
	if (dp->i_disk_size > xfs_dir2_db_off_to_byte(args->geo, db + 1, 0))
		return 0;
	bno = da;
	if ((error = xfs_bmap_last_before(tp, dp, &bno, XFS_DATA_FORK))) {
		/*
		 * This can't really happen unless there's kernel corruption.
		 */
		return error;
	}
	if (db == args->geo->datablk)
		ASSERT(bno == 0);
	else
		ASSERT(bno > 0);
	/*
	 * Set the size to the new last block.
	 */
	dp->i_disk_size = XFS_FSB_TO_B(mp, bno);
	xfs_trans_log_inode(tp, dp, XFS_ILOG_CORE);
	return 0;
}

/* Returns true if the directory entry name is valid. */
bool
xfs_dir2_namecheck(
	const void	*name,
	size_t		length)
{
	/*
	 * MAXNAMELEN includes the trailing null, but (name/length) leave it
	 * out, so use >= for the length check.
	 */
	if (length >= MAXNAMELEN)
		return false;

	/* There shouldn't be any slashes or nulls here */
	return !memchr(name, '/', length) && !memchr(name, 0, length);
}

xfs_dahash_t
xfs_dir2_hashname(
	struct xfs_mount	*mp,
	struct xfs_name		*name)
{
	if (unlikely(xfs_has_asciici(mp)))
		return xfs_ascii_ci_hashname(name);
	return xfs_da_hashname(name->name, name->len);
}

enum xfs_dacmp
xfs_dir2_compname(
	struct xfs_da_args	*args,
	const unsigned char	*name,
	int			len)
{
	if (unlikely(xfs_has_asciici(args->dp->i_mount)))
		return xfs_ascii_ci_compname(args, name, len);
	return xfs_da_compname(args, name, len);
}

/*
 * Given a directory @dp, a newly allocated inode @ip, and a @name, link @ip
 * into @dp under the given @name.  If @ip is a directory, it will be
 * initialized.  Both inodes must have the ILOCK held and the transaction must
 * have sufficient blocks reserved.
 */
int
xfs_dir_create_new_child(
	struct xfs_trans		*tp,
	uint				resblks,
	struct xfs_inode		*dp,
	struct xfs_name			*name,
	struct xfs_inode		*ip)
{
	struct xfs_mount		*mp = tp->t_mountp;
	int				error;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(dp, XFS_ILOCK_EXCL));
	ASSERT(resblks == 0 || resblks > XFS_IALLOC_SPACE_RES(mp));

	error = xfs_dir_createname(tp, dp, name, ip->i_ino,
					resblks - XFS_IALLOC_SPACE_RES(mp));
	if (error) {
		ASSERT(error != -ENOSPC);
		return error;
	}

	xfs_trans_ichgtime(tp, dp, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	xfs_trans_log_inode(tp, dp, XFS_ILOG_CORE);

	if (!S_ISDIR(VFS_I(ip)->i_mode))
		return 0;

	error = xfs_dir_init(tp, ip, dp);
	if (error)
		return error;

	xfs_bumplink(tp, dp);
	return 0;
}

/*
 * Given a directory @dp, an existing non-directory inode @ip, and a @name,
 * link @ip into @dp under the given @name.  Both inodes must have the ILOCK
 * held.
 */
int
xfs_dir_link_existing_child(
	struct xfs_trans	*tp,
	uint			resblks,
	struct xfs_inode	*dp,
	struct xfs_name		*name,
	struct xfs_inode	*ip)
{
	struct xfs_mount	*mp = tp->t_mountp;
	int			error;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(dp, XFS_ILOCK_EXCL));
	ASSERT(!S_ISDIR(VFS_I(ip)->i_mode));

	if (!resblks) {
		error = xfs_dir_canenter(tp, dp, name);
		if (error)
			return error;
	}

	/*
	 * Handle initial link state of O_TMPFILE inode
	 */
	if (VFS_I(ip)->i_nlink == 0) {
		struct xfs_perag	*pag;

		pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, ip->i_ino));
		error = xfs_iunlink_remove(tp, pag, ip);
		xfs_perag_put(pag);
		if (error)
			return error;
	}

	error = xfs_dir_createname(tp, dp, name, ip->i_ino, resblks);
	if (error)
		return error;

	xfs_trans_ichgtime(tp, dp, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	xfs_trans_log_inode(tp, dp, XFS_ILOG_CORE);

	xfs_bumplink(tp, ip);
	return 0;
}

/*
 * Given a directory @dp, a child @ip, and a @name, remove the (@name, @ip)
 * entry from the directory.  Both inodes must have the ILOCK held.
 */
int
xfs_dir_remove_child(
	struct xfs_trans		*tp,
	uint				resblks,
	struct xfs_inode		*dp,
	struct xfs_name			*name,
	struct xfs_inode		*ip)
{
	int				error;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(xfs_isilocked(dp, XFS_ILOCK_EXCL));

	/*
	 * If we're removing a directory perform some additional validation.
	 */
	if (S_ISDIR(VFS_I(ip)->i_mode)) {
		ASSERT(VFS_I(ip)->i_nlink >= 2);
		if (VFS_I(ip)->i_nlink != 2)
			return -ENOTEMPTY;
		if (!xfs_dir_isempty(ip))
			return -ENOTEMPTY;

		/* Drop the link from ip's "..".  */
		error = xfs_droplink(tp, dp);
		if (error)
			return error;

		/* Drop the "." link from ip to self.  */
		error = xfs_droplink(tp, ip);
		if (error)
			return error;

		/*
		 * Point the unlinked child directory's ".." entry to the root
		 * directory to eliminate back-references to inodes that may
		 * get freed before the child directory is closed.  If the fs
		 * gets shrunk, this can lead to dirent inode validation errors.
		 */
		if (dp->i_ino != tp->t_mountp->m_sb.sb_rootino) {
			error = xfs_dir_replace(tp, ip, &xfs_name_dotdot,
					tp->t_mountp->m_sb.sb_rootino, 0);
			if (error)
				return error;
		}
	} else {
		/*
		 * When removing a non-directory we need to log the parent
		 * inode here.  For a directory this is done implicitly
		 * by the xfs_droplink call for the ".." entry.
		 */
		xfs_trans_log_inode(tp, dp, XFS_ILOG_CORE);
	}
	xfs_trans_ichgtime(tp, dp, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

	/* Drop the link from dp to ip. */
	error = xfs_droplink(tp, ip);
	if (error)
		return error;

	error = xfs_dir_removename(tp, dp, name, ip->i_ino, resblks);
	if (error) {
		ASSERT(error != -ENOENT);
		return error;
	}

	return 0;
}

/*
 * Exchange the entry (@name1, @ip1) in directory @dp1 with the entry (@name2,
 * @ip2) in directory @dp2, and update '..' @ip1 and @ip2's entries as needed.
 * @ip1 and @ip2 need not be of the same type.
 *
 * All inodes must have the ILOCK held, and both entries must already exist.
 */
int
xfs_dir_exchange(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp1,
	struct xfs_name		*name1,
	struct xfs_inode	*ip1,
	struct xfs_inode	*dp2,
	struct xfs_name		*name2,
	struct xfs_inode	*ip2,
	unsigned int		spaceres)
{
	int			ip1_flags = 0;
	int			ip2_flags = 0;
	int			dp2_flags = 0;
	int			error;

	/* Swap inode number for dirent in first parent */
	error = xfs_dir_replace(tp, dp1, name1, ip2->i_ino, spaceres);
	if (error)
		return error;

	/* Swap inode number for dirent in second parent */
	error = xfs_dir_replace(tp, dp2, name2, ip1->i_ino, spaceres);
	if (error)
		return error;

	/*
	 * If we're renaming one or more directories across different parents,
	 * update the respective ".." entries (and link counts) to match the new
	 * parents.
	 */
	if (dp1 != dp2) {
		dp2_flags = XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG;

		if (S_ISDIR(VFS_I(ip2)->i_mode)) {
			error = xfs_dir_replace(tp, ip2, &xfs_name_dotdot,
						dp1->i_ino, spaceres);
			if (error)
				return error;

			/* transfer ip2 ".." reference to dp1 */
			if (!S_ISDIR(VFS_I(ip1)->i_mode)) {
				error = xfs_droplink(tp, dp2);
				if (error)
					return error;
				xfs_bumplink(tp, dp1);
			}

			/*
			 * Although ip1 isn't changed here, userspace needs
			 * to be warned about the change, so that applications
			 * relying on it (like backup ones), will properly
			 * notify the change
			 */
			ip1_flags |= XFS_ICHGTIME_CHG;
			ip2_flags |= XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG;
		}

		if (S_ISDIR(VFS_I(ip1)->i_mode)) {
			error = xfs_dir_replace(tp, ip1, &xfs_name_dotdot,
						dp2->i_ino, spaceres);
			if (error)
				return error;

			/* transfer ip1 ".." reference to dp2 */
			if (!S_ISDIR(VFS_I(ip2)->i_mode)) {
				error = xfs_droplink(tp, dp1);
				if (error)
					return error;
				xfs_bumplink(tp, dp2);
			}

			/*
			 * Although ip2 isn't changed here, userspace needs
			 * to be warned about the change, so that applications
			 * relying on it (like backup ones), will properly
			 * notify the change
			 */
			ip1_flags |= XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG;
			ip2_flags |= XFS_ICHGTIME_CHG;
		}
	}

	if (ip1_flags) {
		xfs_trans_ichgtime(tp, ip1, ip1_flags);
		xfs_trans_log_inode(tp, ip1, XFS_ILOG_CORE);
	}
	if (ip2_flags) {
		xfs_trans_ichgtime(tp, ip2, ip2_flags);
		xfs_trans_log_inode(tp, ip2, XFS_ILOG_CORE);
	}
	if (dp2_flags) {
		xfs_trans_ichgtime(tp, dp2, dp2_flags);
		xfs_trans_log_inode(tp, dp2, XFS_ILOG_CORE);
	}
	xfs_trans_ichgtime(tp, dp1, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	xfs_trans_log_inode(tp, dp1, XFS_ILOG_CORE);

	return 0;
}

/*
 * Given an entry (@src_name, @src_ip) in directory @src_dp, make the entry
 * @target_name in directory @target_dp point to @src_ip and remove the
 * original entry, cleaning up everything left behind.
 *
 * Cleanup involves dropping a link count on @target_ip, and either removing
 * the (@src_name, @src_ip) entry from @src_dp or simply replacing the entry
 * with (@src_name, @wip) if a whiteout inode @wip is supplied.
 *
 * All inodes must have the ILOCK held.  We assume that if @src_ip is a
 * directory then its '..' doesn't already point to @target_dp, and that @wip
 * is a freshly allocated whiteout.
 */
int
xfs_dir_rename(
	struct xfs_trans	*tp,
	struct xfs_inode	*src_dp,
	struct xfs_name		*src_name,
	struct xfs_inode	*src_ip,
	struct xfs_inode	*target_dp,
	struct xfs_name		*target_name,
	struct xfs_inode	*target_ip,
	unsigned int		spaceres,
	struct xfs_inode	*wip)
{
	struct xfs_mount	*mp = tp->t_mountp;
	bool			new_parent = (src_dp != target_dp);
	bool			src_is_directory;
	int			error;

	src_is_directory = S_ISDIR(VFS_I(src_ip)->i_mode);

	/*
	 * Check for expected errors before we dirty the transaction
	 * so we can return an error without a transaction abort.
	 *
	 * Extent count overflow check:
	 *
	 * From the perspective of src_dp, a rename operation is essentially a
	 * directory entry remove operation. Hence the only place where we check
	 * for extent count overflow for src_dp is in
	 * xfs_bmap_del_extent_real(). xfs_bmap_del_extent_real() returns
	 * -ENOSPC when it detects a possible extent count overflow and in
	 * response, the higher layers of directory handling code do the
	 * following:
	 * 1. Data/Free blocks: XFS lets these blocks linger until a
	 *    future remove operation removes them.
	 * 2. Dabtree blocks: XFS swaps the blocks with the last block in the
	 *    Leaf space and unmaps the last block.
	 *
	 * For target_dp, there are two cases depending on whether the
	 * destination directory entry exists or not.
	 *
	 * When destination directory entry does not exist (i.e. target_ip ==
	 * NULL), extent count overflow check is performed only when transaction
	 * has a non-zero sized space reservation associated with it.  With a
	 * zero-sized space reservation, XFS allows a rename operation to
	 * continue only when the directory has sufficient free space in its
	 * data/leaf/free space blocks to hold the new entry.
	 *
	 * When destination directory entry exists (i.e. target_ip != NULL), all
	 * we need to do is change the inode number associated with the already
	 * existing entry. Hence there is no need to perform an extent count
	 * overflow check.
	 */
	if (target_ip == NULL) {
		/*
		 * If there's no space reservation, check the entry will
		 * fit before actually inserting it.
		 */
		if (!spaceres) {
			error = xfs_dir_canenter(tp, target_dp, target_name);
			if (error)
				return error;
		} else {
			error = xfs_iext_count_may_overflow(target_dp,
					XFS_DATA_FORK,
					XFS_IEXT_DIR_MANIP_CNT(mp));
			if (error)
				return error;
		}
	} else {
		/*
		 * If target exists and it's a directory, check that whether
		 * it can be destroyed.
		 */
		if (S_ISDIR(VFS_I(target_ip)->i_mode) &&
		    (!xfs_dir_isempty(target_ip) ||
		     (VFS_I(target_ip)->i_nlink > 2)))
			return -EEXIST;
	}

	/*
	 * Directory entry creation below may acquire the AGF. Remove
	 * the whiteout from the unlinked list first to preserve correct
	 * AGI/AGF locking order. This dirties the transaction so failures
	 * after this point will abort and log recovery will clean up the
	 * mess.
	 *
	 * For whiteouts, we need to bump the link count on the whiteout
	 * inode. After this point, we have a real link, clear the tmpfile
	 * state flag from the inode so it doesn't accidentally get misused
	 * in future.
	 */
	if (wip) {
		struct xfs_perag	*pag;

		ASSERT(VFS_I(wip)->i_nlink == 0);

		pag = xfs_perag_get(mp, XFS_INO_TO_AGNO(mp, wip->i_ino));
		error = xfs_iunlink_remove(tp, pag, wip);
		xfs_perag_put(pag);
		if (error)
			return error;

		xfs_bumplink(tp, wip);
	}

	/*
	 * Set up the target.
	 */
	if (target_ip == NULL) {
		/*
		 * If target does not exist and the rename crosses
		 * directories, adjust the target directory link count
		 * to account for the ".." reference from the new entry.
		 */
		error = xfs_dir_createname(tp, target_dp, target_name,
					   src_ip->i_ino, spaceres);
		if (error)
			return error;

		xfs_trans_ichgtime(tp, target_dp,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

		if (new_parent && src_is_directory) {
			xfs_bumplink(tp, target_dp);
		}
	} else { /* target_ip != NULL */
		/*
		 * Link the source inode under the target name.
		 * If the source inode is a directory and we are moving
		 * it across directories, its ".." entry will be
		 * inconsistent until we replace that down below.
		 *
		 * In case there is already an entry with the same
		 * name at the destination directory, remove it first.
		 */
		error = xfs_dir_replace(tp, target_dp, target_name,
					src_ip->i_ino, spaceres);
		if (error)
			return error;

		xfs_trans_ichgtime(tp, target_dp,
					XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

		/*
		 * Decrement the link count on the target since the target
		 * dir no longer points to it.
		 */
		error = xfs_droplink(tp, target_ip);
		if (error)
			return error;

		if (src_is_directory) {
			/*
			 * Drop the link from the old "." entry.
			 */
			error = xfs_droplink(tp, target_ip);
			if (error)
				return error;
		}
	} /* target_ip != NULL */

	/*
	 * Remove the source.
	 */
	if (new_parent && src_is_directory) {
		/*
		 * Rewrite the ".." entry to point to the new
		 * directory.
		 */
		error = xfs_dir_replace(tp, src_ip, &xfs_name_dotdot,
					target_dp->i_ino, spaceres);
		ASSERT(error != -EEXIST);
		if (error)
			return error;
	}

	/*
	 * We always want to hit the ctime on the source inode.
	 *
	 * This isn't strictly required by the standards since the source
	 * inode isn't really being changed, but old unix file systems did
	 * it and some incremental backup programs won't work without it.
	 */
	xfs_trans_ichgtime(tp, src_ip, XFS_ICHGTIME_CHG);
	xfs_trans_log_inode(tp, src_ip, XFS_ILOG_CORE);

	/*
	 * Adjust the link count on src_dp.  This is necessary when
	 * renaming a directory, either within one parent when
	 * the target existed, or across two parent directories.
	 */
	if (src_is_directory && (new_parent || target_ip != NULL)) {

		/*
		 * Decrement link count on src_directory since the
		 * entry that's moved no longer points to it.
		 */
		error = xfs_droplink(tp, src_dp);
		if (error)
			return error;
	}

	/*
	 * For whiteouts, we only need to update the source dirent with the
	 * inode number of the whiteout inode rather than removing it
	 * altogether.
	 */
	if (wip) {
		error = xfs_dir_replace(tp, src_dp, src_name, wip->i_ino,
					spaceres);
	} else {
		/*
		 * NOTE: We don't need to check for extent count overflow here
		 * because the dir remove name code will leave the dir block in
		 * place if the extent count would overflow.
		 */
		error = xfs_dir_removename(tp, src_dp, src_name, src_ip->i_ino,
					   spaceres);
	}
	if (error)
		return error;

	xfs_trans_ichgtime(tp, src_dp, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);
	xfs_trans_log_inode(tp, src_dp, XFS_ILOG_CORE);
	if (new_parent)
		xfs_trans_log_inode(tp, target_dp, XFS_ILOG_CORE);

	return 0;
}
