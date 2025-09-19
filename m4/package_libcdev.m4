#
# Check if we have a pwritev2 libc call (Linux)
#
AC_DEFUN([AC_HAVE_PWRITEV2],
  [ AC_MSG_CHECKING([for pwritev2])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/uio.h>
	]], [[
pwritev2(0, 0, 0, 0, 0);
	]])
    ], have_pwritev2=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_pwritev2)
  ])

#
# Check if we have a copy_file_range system call (Linux)
#
AC_DEFUN([AC_HAVE_COPY_FILE_RANGE],
  [ AC_MSG_CHECKING([for copy_file_range])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
	]], [[
syscall(__NR_copy_file_range, 0, 0, 0, 0, 0, 0);
	]])
    ], have_copy_file_range=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_copy_file_range)
  ])

#
# Check if we have a cachestat system call (Linux)
#
AC_DEFUN([AC_HAVE_CACHESTAT],
  [ AC_MSG_CHECKING([for cachestat])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#include <unistd.h>
#include <linux/mman.h>
#include <asm/unistd.h>
	]], [[
syscall(__NR_cachestat, 0, 0, 0, 0);
	]])
    ], have_cachestat=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_cachestat)
  ])

#
# Check if we need to override the system struct fsxattr with
# the internal definition.  This /only/ happens if the system
# actually defines struct fsxattr /and/ the system definition
# is missing certain fields.
#
AC_DEFUN([AC_NEED_INTERNAL_FSXATTR],
  [
    AC_CHECK_TYPE(struct fsxattr,
      [
        AC_CHECK_MEMBER(struct fsxattr.fsx_cowextsize,
          ,
          need_internal_fsxattr=yes,
          [#include <linux/fs.h>]
        )
      ],,
      [#include <linux/fs.h>]
    )
    AC_SUBST(need_internal_fsxattr)
  ])

#
# Check if we need to override the system struct fscrypt_add_key_arg
# with the internal definition.  This /only/ happens if the system
# actually defines struct fscrypt_add_key_arg /and/ the system
# definition is missing certain fields.
#
AC_DEFUN([AC_NEED_INTERNAL_FSCRYPT_ADD_KEY_ARG],
  [
    AC_CHECK_TYPE(struct fscrypt_add_key_arg,
      [
        AC_CHECK_MEMBER(struct fscrypt_add_key_arg.key_id,
          ,
          need_internal_fscrypt_add_key_arg=yes,
          [#include <linux/fs.h>]
        )
      ],,
      [#include <linux/fs.h>]
    )
    AC_SUBST(need_internal_fscrypt_add_key_arg)
  ])

#
# Check if we need to override the system struct fscrypt_policy_v2
# with the internal definition.  This /only/ happens if the system
# actually defines struct fscrypt_policy_v2 /and/ the system
# definition is missing certain fields.
#
AC_DEFUN([AC_NEED_INTERNAL_FSCRYPT_POLICY_V2],
  [
    AC_CHECK_TYPE(struct fscrypt_policy_v2,
      [
        AC_CHECK_MEMBER(struct fscrypt_policy_v2.log2_data_unit_size,
          ,
          need_internal_fscrypt_policy_v2=yes,
          [#include <linux/fs.h>]
        )
      ],,
      [#include <linux/fs.h>]
    )
    AC_SUBST(need_internal_fscrypt_policy_v2)
  ])

#
# Check if we need to override the system struct statx with
# the internal definition.  This /only/ happens if the system
# actually defines struct statx /and/ the system definition
# is missing certain fields.
#
AC_DEFUN([AC_NEED_INTERNAL_STATX],
  [ AC_CHECK_TYPE(struct statx,
      [
        AC_CHECK_MEMBER(struct statx.stx_atomic_write_unit_max_opt,
          ,
          need_internal_statx=yes,
          [[
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>

#ifndef STATX_TYPE
#include <linux/stat.h>
#endif
]]
        )
      ],need_internal_statx=yes,
      [#include <linux/stat.h>]
    )
    AC_SUBST(need_internal_statx)
  ])

#
# Check if we have a FS_IOC_GETFSMAP ioctl (Linux)
#
AC_DEFUN([AC_HAVE_GETFSMAP],
  [ AC_MSG_CHECKING([for GETFSMAP])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/fsmap.h>
	]], [[
unsigned long x = FS_IOC_GETFSMAP;
struct fsmap_head fh;
	]])
    ], have_getfsmap=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_getfsmap)
  ])

#
# Check if we have MAP_SYNC defines (Linux)
#
AC_DEFUN([AC_HAVE_MAP_SYNC],
  [ AC_MSG_CHECKING([for MAP_SYNC])
    AC_COMPILE_IFELSE(
    [	AC_LANG_PROGRAM([[
#include <sys/mman.h>
	]], [[
int flags = MAP_SYNC | MAP_SHARED_VALIDATE;
	]])
    ], have_map_sync=yes
	AC_MSG_RESULT(yes),
	AC_MSG_RESULT(no))
    AC_SUBST(have_map_sync)
  ])

#
# Check if we have a mallinfo libc call
#
AC_DEFUN([AC_HAVE_MALLINFO],
  [ AC_MSG_CHECKING([for mallinfo ])
    AC_COMPILE_IFELSE(
    [	AC_LANG_PROGRAM([[
#include <malloc.h>
	]], [[
struct mallinfo test;

test.arena = 0; test.hblkhd = 0; test.uordblks = 0; test.fordblks = 0;
test = mallinfo();
	]])
    ], have_mallinfo=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_mallinfo)
  ])

#
# Check if we have a mallinfo2 libc call
#
AC_DEFUN([AC_HAVE_MALLINFO2],
  [ AC_MSG_CHECKING([for mallinfo2 ])
    AC_COMPILE_IFELSE(
    [	AC_LANG_PROGRAM([[
#include <malloc.h>
        ]], [[
struct mallinfo2 test;

test.arena = 0; test.hblkhd = 0; test.uordblks = 0; test.fordblks = 0;
test = mallinfo2();
        ]])
    ], have_mallinfo2=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_mallinfo2)
  ])

#
# Check if we have a memfd_create libc call (Linux)
#
AC_DEFUN([AC_HAVE_MEMFD_CREATE],
  [ AC_MSG_CHECKING([for memfd_create])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/mman.h>
	]], [[
memfd_create(0, 0);
	]])
    ], have_memfd_create=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_memfd_create)
  ])

#
# Check if we have a getrandom syscall with a GRND_NONBLOCK flag
#
AC_DEFUN([AC_HAVE_GETRANDOM_NONBLOCK],
  [ AC_MSG_CHECKING([for getrandom and GRND_NONBLOCK])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <sys/random.h>
    ]], [[
         unsigned int moo;
         return getrandom(&moo, sizeof(moo), GRND_NONBLOCK);
    ]])],[have_getrandom_nonblock=yes
       AC_MSG_RESULT(yes)],[AC_MSG_RESULT(no)])
    AC_SUBST(have_getrandom_nonblock)
  ])

AC_DEFUN([AC_PACKAGE_CHECK_LTO],
  [ AC_MSG_CHECKING([if C compiler supports LTO])
    OLD_CFLAGS="$CFLAGS"
    OLD_LDFLAGS="$LDFLAGS"
    LTO_FLAGS="-flto -ffat-lto-objects"
    CFLAGS="$CFLAGS $LTO_FLAGS"
    LDFLAGS="$LDFLAGS $LTO_FLAGS"
    AC_LINK_IFELSE([AC_LANG_PROGRAM([])],
        [AC_MSG_RESULT([yes])]
        [lto_cflags=$LTO_FLAGS]
        [lto_ldflags=$LTO_FLAGS]
        [AC_PATH_PROG(gcc_ar, gcc-ar,,)]
        [AC_PATH_PROG(gcc_ranlib, gcc-ranlib,,)],
        [AC_MSG_RESULT([no])])
    if test -x "$gcc_ar" && test -x "$gcc_ranlib"; then
        have_lto=yes
    fi
    CFLAGS="${OLD_CFLAGS}"
    LDFLAGS="${OLD_LDFLAGS}"
    AC_SUBST(gcc_ar)
    AC_SUBST(gcc_ranlib)
    AC_SUBST(have_lto)
    AC_SUBST(lto_cflags)
    AC_SUBST(lto_ldflags)
  ])

#
# Check if we have a file_getattr system call (Linux)
#
AC_DEFUN([AC_HAVE_FILE_GETATTR],
  [AC_MSG_CHECKING([for file_getattr syscall])
    AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
  ]], [[
syscall(__NR_file_getattr, 0, 0, 0, 0, 0);
  ]])
    ], have_file_getattr=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_file_getattr)
  ])

#
# Check if strerror_r returns an int, as opposed to a char *, because there are
# two versions of this function, with differences that are hard to detect.
#
# GNU strerror_r returns a pointer to a string on success, but the returned
# pointer might point to a static buffer and not buf, so you have to use the
# return value.  The declaration has the __warn_unused_result__ attribute to
# enforce this.
#
# XSI strerror_r always writes to buf and returns 0 on success, -1 on error.
#
# How do you select a particular version?  By defining macros, of course!
# _GNU_SOURCE always gets you the GNU version, and _POSIX_C_SOURCE >= 200112L
# gets you the XSI version but only if _GNU_SOURCE isn't defined.
#
# The build system #defines _GNU_SOURCE unconditionally, so when compiling
# against glibc we get the GNU version.  However, when compiling against musl,
# the _GNU_SOURCE definition does nothing and we get the XSI version anyway.
# Not definining _GNU_SOURCE breaks the build in many areas, so we'll create
# yet another #define for just this weird quirk so that we can patch around it
# in the one place we need it.
#
# Note that we have to force erroring out on the int conversion warnings
# because C doesn't consider it a hard error to cast a char pointer to an int
# even when CFLAGS contains -std=gnu11.
AC_DEFUN([AC_STRERROR_R_RETURNS_STRING],
  [AC_MSG_CHECKING([if strerror_r returns char *])
    OLD_CFLAGS="$CFLAGS"
    CFLAGS="$CFLAGS -Wall -Werror"
    AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
  ]], [[
char buf[1024];
puts(strerror_r(0, buf, sizeof(buf)));
  ]])
    ],
       strerror_r_returns_string=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    CFLAGS="$OLD_CFLAGS"
    AC_SUBST(strerror_r_returns_string)
  ])
