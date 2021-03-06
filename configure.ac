AC_INIT([xfsprogs], [4.3.0-rc1])
AC_PREREQ(2.50)
AC_CONFIG_AUX_DIR([.])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([include/libxfs.h])
AC_CONFIG_HEADER(include/platform_defs.h)
AC_PREFIX_DEFAULT(/usr)

AC_PROG_LIBTOOL

AC_PROG_CC
if test $cross_compiling = no; then
  BUILD_CC="$CC"
  AC_SUBST(BUILD_CC)
else
  AC_CHECK_PROGS(BUILD_CC, gcc cc)
fi

AC_ARG_ENABLE(shared,
[ --enable-shared=[yes/no] Enable use of shared libraries [default=yes]],,
	enable_shared=yes)
AC_SUBST(enable_shared)

AC_ARG_ENABLE(gettext,
[ --enable-gettext=[yes/no] Enable alternate language support [default=yes]],,
	enable_gettext=yes)
AC_SUBST(enable_gettext)

AC_ARG_ENABLE(blkid,
[ --enable-blkid=[yes/no] Enable use of block device id library [default=yes]],,
	enable_blkid=yes)
AC_SUBST(enable_blkid)

AC_ARG_ENABLE(readline,
[ --enable-readline=[yes/no] Enable readline command editing [default=no]],
	test $enable_readline = yes && libreadline="-lreadline",
	enable_readline=no)
AC_SUBST(libreadline)
AC_SUBST(enable_readline)

AC_ARG_ENABLE(editline,
[ --enable-editline=[yes/no] Enable editline command editing [default=no]],
	test $enable_editline = yes && libeditline="-ledit",
	enable_editline=no)
AC_SUBST(libeditline)
AC_SUBST(enable_editline)

AC_ARG_ENABLE(termcap,
[ --enable-termcap=[yes/no] Enable terminal capabilities library [default=no]],
	test $enable_termcap = yes && libtermcap="-ltermcap",)
AC_SUBST(libtermcap)


AC_ARG_ENABLE(lib64,
[ --enable-lib64=[yes/no] Enable lib64 support [default=yes]],,
	enable_lib64=yes)
AC_SUBST(enable_lib64)

#
# If the user specified a libdir ending in lib64 do not append another
# 64 to the library names.
#
base_libdir=`basename "$libdir"`
case $base_libdir in
lib64)
  enable_lib64=no
esac

#
# Some important tools should be installed into the root partitions.
#
# Check whether exec_prefix=/usr: and install them to /sbin in that
# case.  If the user choses a different prefix assume he just wants
# a local install for testing and not a system install.
#
case $exec_prefix:$prefix in
NONE:NONE | NONE:/usr | /usr:*)
  root_sbindir='/sbin'
  root_libdir="/${base_libdir}"
  ;;
*)
  root_sbindir="${sbindir}"
  root_libdir="${libdir}"
  ;;
esac

AC_SUBST([root_sbindir])
AC_SUBST([root_libdir])

# Find localized files.  Don't descend into any "dot directories"
# (like .git or .pc from quilt).  Strangely, the "-print" argument
# to "find" is required, to avoid including such directories in the
# list.
LOCALIZED_FILES=""
for lfile in `find ${srcdir} -path './.??*' -prune -o -name '*.c' -type f -print || exit 1`; do
    LOCALIZED_FILES="$LOCALIZED_FILES \$(TOPDIR)/$lfile"
done
AC_SUBST(LOCALIZED_FILES)

AC_PACKAGE_GLOBALS(xfsprogs)
AC_PACKAGE_UTILITIES(xfsprogs)
AC_MULTILIB($enable_lib64)

have_richacl=no
librichacl=
AC_CHECK_HEADERS([sys/richacl.h])
if test "$ac_cv_header_sys_richacl_h" = yes; then
    AC_CHECK_HEADERS([linux/xattr.h])
    saved_LIBS=$LIBS
    librichacl=-l:librichacl.a
    LIBS="$LIBS $librichacl"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
	#include <sys/richacl.h>
    ],[
	int valid = richacl_valid((struct richacl *)0);
    ])], [
	have_richacl=yes
    ], [
	librichacl=
    ])
    LIBS=$saved_LIBS
fi
AC_SUBST([have_richacl])
AC_SUBST([librichacl])

AC_PACKAGE_NEED_AIO_H
AC_PACKAGE_NEED_LIO_LISTIO

AC_PACKAGE_NEED_UUID_H
AC_PACKAGE_NEED_UUIDCOMPARE

AC_PACKAGE_NEED_PTHREAD_H
AC_PACKAGE_NEED_PTHREADMUTEXINIT

AC_HAVE_FADVISE
AC_HAVE_MADVISE
AC_HAVE_MINCORE
AC_HAVE_SENDFILE
AC_HAVE_GETMNTENT
AC_HAVE_GETMNTINFO
AC_HAVE_FALLOCATE
AC_HAVE_FIEMAP
AC_HAVE_PREADV
AC_HAVE_SYNC_FILE_RANGE
AC_HAVE_MNTENT
AC_HAVE_FLS
AC_HAVE_READDIR
AC_HAVE_FSETXATTR
AC_HAVE_MREMAP

if test "$enable_blkid" = yes; then
AC_HAVE_BLKID_TOPO
fi

AC_CHECK_SIZEOF([long])
AC_CHECK_SIZEOF([char *])
AC_TYPE_UMODE_T
AC_MANUAL_FORMAT

AC_CONFIG_FILES([include/builddefs])
AC_OUTPUT
