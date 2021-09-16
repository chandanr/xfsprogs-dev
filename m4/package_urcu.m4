AC_DEFUN([AC_PACKAGE_NEED_URCU_H],
  [ AC_CHECK_HEADERS([urcu.h])
    if test $ac_cv_header_urcu_h = no; then
	echo
	echo 'FATAL ERROR: could not find a valid URCU header.'
	echo 'Install the Userspace RCU development package.'
	exit 1
    fi
  ])

AC_DEFUN([AC_PACKAGE_NEED_RCU_SET_POINTER_SYM],
  [ AC_CHECK_FUNCS(rcu_set_pointer_sym)
    if test $ac_cv_func_rcu_set_pointer_sym = yes; then
	liburcu=""
    else
	AC_CHECK_LIB(urcu, rcu_set_pointer_sym,, [
	    echo
	    echo 'FATAL ERROR: could not find a valid urcu library.'
	    echo 'Install the Userspace RCU development package.'
	    exit 1])
	liburcu="-lurcu"
    fi
    AC_SUBST(liburcu)
  ])
