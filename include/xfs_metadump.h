// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2007 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef _XFS_METADUMP_H_
#define _XFS_METADUMP_H_

#define	XFS_MD_MAGIC_V1		0x5846534d	/* 'XFSM' */
#define	XFS_MD_MAGIC_V2		0x584D4432	/* 'XMD2' */

/* Metadump v1 */
typedef struct xfs_metablock {
	__be32		mb_magic;
	__be16		mb_count;
	uint8_t		mb_blocklog;
	uint8_t		mb_info;
	/* followed by an array of xfs_daddr_t */
} xfs_metablock_t;

/* These flags are informational only, not backwards compatible */
#define XFS_METADUMP_INFO_FLAGS	(1 << 0) /* This image has informative flags */
#define XFS_METADUMP_OBFUSCATED	(1 << 1)
#define XFS_METADUMP_FULLBLOCKS	(1 << 2)
#define XFS_METADUMP_DIRTYLOG	(1 << 3)

/* Metadump v2 */
struct xfs_metadump_header {
	__be32 xmh_magic;
	__be32 xmh_version;
	__be32 xmh_compat_flags;
	__be32 xmh_incompat_flags;
	__be64 xmh_reserved;
} __packed;

#define XFS_MD2_INCOMPAT_OBFUSCATED	(1 << 0)
#define XFS_MD2_INCOMPAT_FULLBLOCKS	(1 << 1)
#define XFS_MD2_INCOMPAT_DIRTYLOG	(1 << 2)

struct xfs_meta_extent {
        /*
	 * Lowest 54 bits are used to store 512 byte addresses.
	 * Next 2 bits is used for indicating the device.
	 * 00 - Data device
	 * 01 - External log
	 */
        __be64 xme_addr;
        /* In units of 512 byte blocks */
        __be32 xme_len;
} __packed;

#define XME_ADDR_DATA_DEVICE	(1UL << 54)
#define XME_ADDR_LOG_DEVICE	(1UL << 55)

#define XME_ADDR_DEVICE_MASK (~(XME_ADDR_DATA_DEVICE | XME_ADDR_LOG_DEVICE))

#endif /* _XFS_METADUMP_H_ */
