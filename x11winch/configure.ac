AC_PREREQ([2.72])
AC_INIT([x11winch], [1.0.0], [k@iczelia.net])
AC_CONFIG_SRCDIR([x11winch.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign])

AS_IF([test -z "$CFLAGS"], [CFLAGS='-Os'])
AC_PROG_CC

PKG_PROG_PKG_CONFIG
AS_IF([test "x$PKG_CONFIG" = x],
  [AC_MSG_ERROR([pkg-config not found. Install it first.])])

PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([DEPS], [freetype2 xinerama fontconfig x11 xrender xft libpng dbus-1],
  [],
  [AC_MSG_ERROR([Required libraries not found.])])

AC_PATH_PROG([XXD], [xxd])
AS_IF([test -z "$XXD"], [AC_MSG_ERROR([xxd not found])])

AC_PATH_X
AC_CHECK_HEADERS([stdint.h sys/time.h unistd.h])

AC_C_INLINE
AC_TYPE_INT32_T
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T

AC_FUNC_MALLOC
AC_FUNC_MKTIME
AC_CHECK_FUNCS([memset select])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
