#                                               -*- Autoconf -*-
# octomancer -- keep a camera's clock on Tentacle Sync time.

AC_PREREQ([2.69])
AC_INIT([octomancer], [0.1.0], [palmer@dabbelt.com])
AC_CONFIG_SRCDIR([src/tentacle.cc])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([1.16 foreign subdir-objects -Wall])
AM_SILENT_RULES([yes])

AC_CANONICAL_HOST

AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_MKDIR_P
AC_PROG_RANLIB
AM_PROG_AR

# C++17 without depending on autoconf-archive being installed.
AC_LANG([C++])
AC_MSG_CHECKING([whether $CXX accepts -std=c++17])
octo_saved_cxxflags=$CXXFLAGS
CXXFLAGS="$CXXFLAGS -std=c++17"
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[#include <optional>
                     #include <string_view>]],
                   [[std::optional<int> v{1}; std::string_view s{"x"}; (void)v; (void)s;]])],
  [AC_MSG_RESULT([yes])],
  [CXXFLAGS=$octo_saved_cxxflags
   AC_MSG_RESULT([no])
   AC_MSG_ERROR([octomancer needs a C++17 compiler])])

# The radio backend is CoreBluetooth, so the daemon is macOS-only. The decoder
# and the wire protocol are portable C++ and their tests run anywhere, which is
# why the failure below is scoped to the daemon rather than to configure.
octo_darwin=no
case $host_os in
  darwin*) octo_darwin=yes ;;
esac
AM_CONDITIONAL([DARWIN], [test "x$octo_darwin" = xyes])

if test "x$octo_darwin" = xyes; then
  AC_PROG_OBJCXX
  AC_SUBST([BLE_LIBS], ["-framework Foundation -framework CoreBluetooth"])
  AC_SUBST([UI_LIBS],  ["-framework Foundation -framework AppKit -framework UserNotifications"])
else
  AC_MSG_WARN([not macOS: building the decoder and its tests, but not the daemon])
fi

# The status-bar app is optional; a headless install does not need AppKit.
AC_ARG_ENABLE([ui],
  [AS_HELP_STRING([--disable-ui], [do not build the Octomancer.app status-bar UI])],
  [octo_ui=$enableval], [octo_ui=$octo_darwin])
if test "x$octo_darwin" != xyes; then octo_ui=no; fi
AM_CONDITIONAL([BUILD_UI], [test "x$octo_ui" = xyes])

AC_ARG_ENABLE([werror],
  [AS_HELP_STRING([--enable-werror], [treat compiler warnings as errors])],
  [octo_werror=$enableval], [octo_werror=no])
if test "x$octo_werror" = xyes; then
  CXXFLAGS="$CXXFLAGS -Werror"
fi
CXXFLAGS="$CXXFLAGS -Wall -Wextra"

# codesign is needed to give the .app a stable identity; without one macOS
# will not deliver its notifications.
AC_CHECK_PROG([CODESIGN], [codesign], [codesign], [no])

# The LaunchAgent plist needs real paths, and $bindir is still ${exec_prefix}/bin
# at this point. Expand it here rather than shipping a plist launchd cannot read.
octo_saved_prefix=$prefix
octo_saved_exec_prefix=$exec_prefix
test "x$prefix" = xNONE && prefix=$ac_default_prefix
test "x$exec_prefix" = xNONE && exec_prefix='${prefix}'
eval EXPANDED_BINDIR="$bindir"
eval EXPANDED_BINDIR="$EXPANDED_BINDIR"
prefix=$octo_saved_prefix
exec_prefix=$octo_saved_exec_prefix
AC_SUBST([EXPANDED_BINDIR])

AC_ARG_WITH([logdir],
  [AS_HELP_STRING([--with-logdir=DIR], [where the agent writes its log
                   @<:@$HOME/Library/Logs/octomancer@:>@])],
  [OCTO_LOGDIR=$withval], [OCTO_LOGDIR=$HOME/Library/Logs/octomancer])
AC_SUBST([OCTO_LOGDIR])

AC_CONFIG_FILES([Makefile
                 src/octomancerd-Info.plist
                 ui/Info.plist
                 launchd/com.dabbelt.octomancerd.plist])
AC_OUTPUT

AC_MSG_NOTICE([

  octomancer $VERSION

    host          $host
    daemon        $octo_darwin
    status-bar UI $octo_ui
    CXX           $CXX $CXXFLAGS
])
