# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTCOMMAND = mkfs.xfs
XFS_PROTOFILE = xfs_protofile

HFILES =
CFILES = proto.c xfs_mkfs.c

LLDLIBS += $(LIBXFS) $(LIBXCMD) $(LIBFROG) $(LIBRT) $(LIBPTHREAD) $(LIBBLKID) \
	$(LIBUUID) $(LIBINIH)
LTDEPENDENCIES += $(LIBXFS) $(LIBXCMD) $(LIBFROG)
LLDFLAGS = -static-libtool-libs

default: depend $(LTCOMMAND) $(XFS_PROTOFILE)

include $(BUILDRULES)

install: default
	$(INSTALL) -m 755 -d $(PKG_ROOT_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_ROOT_SBIN_DIR)
	$(INSTALL) -m 755 $(XFS_PROTOFILE) $(PKG_ROOT_SBIN_DIR)
install-dev:

$(XFS_PROTOFILE): $(XFS_PROTOFILE).in
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" < $< > $@
	$(Q)chmod a+x $@

-include .dep
