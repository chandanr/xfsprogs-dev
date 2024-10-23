# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2017 Oracle.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTLIBRARY = libfrog.la
LT_CURRENT = 0
LT_REVISION = 0
LT_AGE = 0
# we need a static build even if --disable-static is specified
LTLDFLAGS += -static

# TODO: chandan: Arrange files in alphabetical order
CFILES = \
avl64.c \
bitmap.c \
bulkstat.c \
convert.c \
crc32.c \
fsgeom.c \
list_sort.c \
linux.c \
logging.c \
paths.c \
projects.c \
ptvar.c \
radix-tree.c \
randbytes.c \
scrub.c \
util.c \
workqueue.c \
delayed-work.c \
rbtree.c \
pseudo_percpu.c

HFILES = \
avl64.h \
bulkstat.h \
bitmap.h \
convert.h \
crc32c.h \
crc32cselftest.h \
crc32defs.h \
crc32table.h \
dahashselftest.h \
div64.h \
fsgeom.h \
logging.h \
paths.h \
projects.h \
ptvar.h \
radix-tree.h \
randbytes.h \
scrub.h \
workqueue.h \
rbtree_augmented.h \
rbtree.h \
rbtree_types.h \
pseudo_percpu.c

LSRCFILES += gen_crc32table.c

ifeq ($(HAVE_GETMNTENT),yes)
LCFLAGS += -DHAVE_GETMNTENT
endif

LDIRT = gen_crc32table crc32table.h

default: ltdepend $(LTLIBRARY)

crc32table.h: gen_crc32table.c crc32defs.h
	@echo "    [CC]     gen_crc32table"
	$(Q) $(BUILD_CC) $(BUILD_CFLAGS) -o gen_crc32table $<
	@echo "    [GENERATE] $@"
	$(Q) ./gen_crc32table > crc32table.h

include $(BUILDRULES)

install install-dev: default

-include .ltdep
