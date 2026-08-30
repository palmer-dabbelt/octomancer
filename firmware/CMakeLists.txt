# octomancer-sync, built as firmware for an nRF52840 dongle.
#
# Almost every file listed here is the same file the Mac builds. That is the
# point of the seam described in CLAUDE.md and in src/syncd.h: the
# decision-making is kept free of any radio, so it can be tested on a machine
# with no hardware -- and the same property is what lets it be compiled for a
# device with no operating system. Only the four files under firmware/src are
# new, and all four are glue: a loop backend, a radio, a serial port, and a
# main() to tie them together.
#
# What is deliberately absent: src/loop_posix.cc, src/server.cc, src/boxsock.cc
# and everything else that assumes a file descriptor. The Zephyr backend in
# firmware/src/loop_zephyr.cc replaces the first, and firmware/src/cdcpeer.cc
# replaces the rest.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(octomancer_sync LANGUAGES C CXX)

set(OCTO ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_include_directories(app PRIVATE ${OCTO} ${CMAKE_CURRENT_SOURCE_DIR}/src)

# src/timeutil.cc calls localtime_r, which is the reentrant form and the only
# one that is safe to call at all. Picolibc declares it only when POSIX is
# visible, and Zephyr compiles C++ as strict ISO -- CONFIG_STD_CPP20 means
# -std=c++20, not -std=gnu++20 -- which defines __STRICT_ANSI__ and hides it.
# The implementation is there either way (CONFIG_POSIX_C_LANG_SUPPORT_R); this
# is only about the declaration. Asking for POSIX explicitly is the fix that
# does not require editing a portable file for one target's feature macros.
target_compile_definitions(app PRIVATE _POSIX_C_SOURCE=200809L)

# The glue, which exists only on this target.
target_sources(app PRIVATE
  src/main.cc
  src/boxadmin.cc
  src/boxclock.cc
  src/cdcpeer.cc
  src/loop_zephyr.cc
  src/scanner_zephyr.cc
)

# The program. Every one of these is compiled unchanged for the Mac as well,
# and is covered by `make check` there.
target_sources(app PRIVATE
  ${OCTO}/advert.cc
  ${OCTO}/bmd.cc
  ${OCTO}/boxmsg.cc
  ${OCTO}/camconf.cc
  ${OCTO}/camsync.cc
  ${OCTO}/escape.cc
  ${OCTO}/hci.cc
  ${OCTO}/loop.cc
  ${OCTO}/registry.cc
  ${OCTO}/syncd.cc
  ${OCTO}/tentacle.cc
  ${OCTO}/timeutil.cc
)
