#ifndef _LIBXLOG_PRIV_H
#define _LIBXLOG_PRIV_H

#define xfs_fs_writable(...) (true)

#define XFS_WQFLAGS(wqflags)   (wqflags)

#define xfs_buftarg_wait(targp)

#define evict_inodes(sb) cache_purge(libxfs_icache)

#endif	/* _LIBXLOG_PRIV_H */
