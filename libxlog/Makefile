# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2000-2002 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTLIBRARY = libxlog.la
LT_CURRENT = 0
LT_REVISION = 0
LT_AGE = 0
CFLAGS += -Wno-pointer-sign
# we need a static build even if --disable-static is specified
LTLDFLAGS += -static

CFILES = \
xfs_trans.c \
xfs_trans_ail.c \
xfs_trans_buf.c \
xfs_attr_item.c \
xfs_bmap_item.c \
xfs_buf_item.c \
xfs_buf_item_recover.c \
xfs_dquot_item.c \
xfs_dquot_item_recover.c \
xfs_extfree_item.c \
xfs_icreate_item.c \
xfs_inode_item.c \
xfs_inode_item_recover.c \
xfs_iunlink_item.c \
xfs_log.c \
xfs_log_cil.c \
xfs_log_recover.c \
xfs_refcount_item.c \
xfs_rmap_item.c

HFILES = \
libxlog_priv.h \
xfs_trans_quota.h \
xfs_attr_item.h \
xfs_bmap_item.h \
xfs_buf_item.h \
xfs_dquot_item.h \
xfs_extfree_item.h \
xfs_icreate_item.h \
xfs_inode_item.h \
xfs_iunlink_item.h \
xfs_log.h \
xfs_refcount_item.h \
xfs_rmap_item.h

# don't want to link xfs_repair with a debug libxlog.
DEBUG = -DNDEBUG

default: ltdepend $(LTLIBRARY)

include $(BUILDRULES)

install install-dev: default

-include .ltdep
