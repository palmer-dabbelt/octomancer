## octomancer -- non-recursive build.
##
## One Makefile for the whole tree: the parts are small and interdependent, and
## a recursive build would buy nothing but slower dependency tracking.

ACLOCAL_AMFLAGS = -I m4

# The version is handed over on the command line rather than through a
# generated config.h. A config header is a file every translation unit
# depends on and that ./configure rewrites, so a no-op reconfigure rebuilds
# the entire tree; for the one string actually wanted from it, that trade is
# not worth making.
AM_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir) \
	-DOCTO_VERSION=\"$(PACKAGE_VERSION)\" \
	-DOCTO_BINDIR=\"$(EXPANDED_BINDIR)\" \
	-DOCTO_LOGDIR=\"$(OCTO_LOGDIR)\" \
	-DOCTO_PKGDATADIR=\"$(EXPANDED_DATADIR)/$(PACKAGE)\"

# The firmware is not built by this Makefile -- it is a Zephyr application,
# cross-compiled by tools/flash-dongle.sh -- but it is part of the source and
# belongs in a distribution. Most of what it compiles is listed above, in
# libocto: firmware/ is only the glue. See firmware/CMakeLists.txt.
EXTRA_DIST = autogen.sh README.md tests/data/adverts.golden \
	tools/flash-dongle.sh \
	firmware/CMakeLists.txt firmware/prj.conf \
	firmware/src/boxadmin.cc firmware/src/boxadmin.h \
	firmware/src/boxclock.cc firmware/src/boxclock.h \
	firmware/src/cdcpeer.cc firmware/src/cdcpeer.h \
	firmware/src/loop_zephyr.cc \
	firmware/src/main.cc \
	firmware/src/scanner_zephyr.cc firmware/src/scanner_zephyr.h

noinst_LIBRARIES = libocto.a
libocto_a_SOURCES = \
	src/advert.cc \
	src/advert.h \
	src/agents.cc \
	src/agents.h \
	src/att.cc \
	src/att.h \
	src/bmd.cc \
	src/bmd.h \
	src/boxmsg.cc \
	src/boxmsg.h \
	src/boxsock.cc \
	src/boxsock.h \
	src/camasync.h \
	src/camconf.cc \
	src/camconf.h \
	src/camdb.cc \
	src/camdb.h \
	src/camsync.cc \
	src/camsync.h \
	src/client.cc \
	src/client.h \
	src/control.cc \
	src/control.h \
	src/crypto.cc \
	src/crypto.h \
	src/devices.cc \
	src/devices.h \
	src/escape.cc \
	src/escape.h \
	src/hci.cc \
	src/hci.h \
	src/hcilink.cc \
	src/hcilink.h \
	src/hciport.h \
	src/hciport_posix.cc \
	src/hcishare.cc \
	src/hcishare.h \
	src/jsonlog.cc \
	src/jsonlog.h \
	src/logscan.cc \
	src/logscan.h \
	src/loop.cc \
	src/loop.h \
	src/loop_posix.cc \
	src/loopfake.cc \
	src/loopfake.h \
	src/pairing.cc \
	src/pairing.h \
	src/proclock.cc \
	src/proclock.h \
	src/proto.cc \
	src/proto.h \
	src/radio.cc \
	src/radio.h \
	src/radio_camera.cc \
	src/registry.cc \
	src/registry.h \
	src/render.cc \
	src/render.h \
	src/scanbridge.cc \
	src/scanbridge.h \
	src/server.cc \
	src/server.h \
	src/syncd.cc \
	src/syncd.h \
	src/smp.cc \
	src/smp.h \
	src/tentacle.cc \
	src/tentacle.h \
	src/timeutil.cc \
	src/timeutil.h \
	src/tui.cc \
	src/tui.h \
	src/tui_term.cc

##
## The service and its control tool. Both are macOS-only: the radio is
## CoreBluetooth, and there is no second backend to fall back to.
##

# The dongle backends are portable POSIX and go in the library with everything
# else. They are only pulled into a binary that actually calls a radio factory,
# which is why the tools with no radio in them do not grow a serial port.
libocto_a_SOURCES += \
	src/camera_hci.cc \
	src/camera_fake.cc \
	src/devicedb.cc \
	src/fakebench.cc \
	src/radiopick.cc \
	src/scanner_fake.cc \
	src/scanner_hci.cc

# radio.cc has to know whether there is a second backend to choose from. The
# version string is handed over on the command line for the reason AM_CPPFLAGS
# explains above, and this is the same trade: one define beats a config header
# that every translation unit depends on.
if DARWIN
AM_CPPFLAGS += -DOCTO_HAVE_COREBLUETOOTH
endif

# The Zoom bench. Not in the DARWIN group: it speaks to the dongle over a
# serial port and needs nothing from CoreBluetooth, so it is the first program
# here that would build on any POSIX host.
bin_PROGRAMS = octomancer-zoom
octomancer_zoom_SOURCES = src/octomancer-zoom.cc
octomancer_zoom_LDADD = libocto.a

if DARWIN
bin_PROGRAMS += octomancerd octomancerctl octomancer-sync octomancer-report \
	octomancer

octomancerd_SOURCES = src/octomancerd.cc src/scanner_mac.mm src/scanner.h
octomancerd_LDADD = libocto.a
# The embedded Info.plist is what gives a plain executable an identity for the
# Bluetooth permission prompt. Without it macOS has no name to show the user
# and nothing stable to remember the answer against, so an agent started by
# launchd -- with no Terminal to inherit an approval from -- is refused the
# radio with no way to grant it.
octomancerd_LDFLAGS = $(BLE_LIBS) \
	-Wl,-sectcreate,__TEXT,__info_plist,$(abs_top_builddir)/src/octomancerd-Info.plist
octomancerd_DEPENDENCIES = libocto.a src/octomancerd-Info.plist

octomancerctl_SOURCES = src/octomancerctl.cc
octomancerctl_LDADD = libocto.a

# The front door. It talks to whichever daemon it needs over a socket and has
# no radio of its own, which is why it is not in the DARWIN-only group by
# necessity -- only by there being nothing to talk to elsewhere.
octomancer_SOURCES = src/octomancer.cc
octomancer_LDADD = libocto.a

# The one program here that is not passive. It connects to a camera and writes
# to it, so it is a separate binary with its own bundle identifier: the
# Bluetooth grant that lets a clock be set should be revocable without also
# blinding the listener.
#
# It links the Tentacle scanner too, for the case where octomancerd is not
# running and it has to listen for the bench itself.
octomancer_sync_SOURCES = \
	src/octomancer-sync.cc \
	src/camera_mac.mm \
	src/camera.h \
	src/scanner_mac.mm \
	src/scanner.h
octomancer_sync_LDADD = libocto.a
octomancer_sync_LDFLAGS = $(BLE_LIBS) \
	-Wl,-sectcreate,__TEXT,__info_plist,$(abs_top_builddir)/src/octomancer-sync-Info.plist
octomancer_sync_DEPENDENCIES = libocto.a src/octomancer-sync-Info.plist

# Reading a log needs no radio at all, so this one would build anywhere; it
# lives under DARWIN only because there is nothing on another host to produce
# a log for it to read.
octomancer_report_SOURCES = src/octomancer-report.cc
octomancer_report_LDADD = libocto.a
endif

if BUILD_UI
noinst_PROGRAMS = octomancer-ui
octomancer_ui_SOURCES = ui/main.mm
octomancer_ui_LDADD = libocto.a
octomancer_ui_LDFLAGS = $(UI_LIBS)

# The .app wrapper is not decoration: UNUserNotificationCenter refuses to
# deliver anything from a process without a bundle identifier and a signature,
# so a bare executable simply cannot post a notification.
APPBUNDLE = Octomancer.app

all-local: $(APPBUNDLE)/Contents/MacOS/Octomancer

$(APPBUNDLE)/Contents/MacOS/Octomancer: octomancer-ui ui/Info.plist \
		$(AGENT_PLISTS)
	$(AM_V_GEN)rm -rf $(APPBUNDLE)
	$(AM_V_at)$(MKDIR_P) $(APPBUNDLE)/Contents/MacOS $(APPBUNDLE)/Contents/Resources
	$(AM_V_at)cp ui/Info.plist $(APPBUNDLE)/Contents/Info.plist
	$(AM_V_at)cp octomancer-ui $@
# The app's "Start at boot" installs these. They are copied in rather than
# written out by the app so that the templates stay the one place a
# LaunchAgent's contents are decided.
	$(AM_V_at)cp $(AGENT_PLISTS) $(APPBUNDLE)/Contents/Resources/
	$(AM_V_at)if test "x$(CODESIGN)" != xno; then \
	  $(CODESIGN) --force --sign - "$(APPBUNDLE)" >/dev/null 2>&1 || \
	    echo "warning: could not sign $(APPBUNDLE); notifications may not appear"; \
	else \
	  echo "warning: codesign not found; notifications may not appear"; \
	fi

clean-local:
	rm -rf $(APPBUNDLE)

# Deliberately not part of `make install`: dropping a login item and a
# LaunchAgent into someone's home directory is not something a build should do
# without being asked.
install-app: $(APPBUNDLE)/Contents/MacOS/Octomancer
	$(MKDIR_P) "$$HOME/Applications"
	rm -rf "$$HOME/Applications/$(APPBUNDLE)"
	cp -R "$(APPBUNDLE)" "$$HOME/Applications/"
	@echo "installed $$HOME/Applications/$(APPBUNDLE)"

uninstall-app:
	rm -rf "$$HOME/Applications/$(APPBUNDLE)"

.PHONY: install-app uninstall-app
endif

# Two agents, because there are two daemons: the one that only listens and the
# one that can write to a camera. Keeping them separate is what makes the
# second one revocable on its own.
AGENT_PLISTS = launchd/com.dabbelt.octomancerd.plist \
	launchd/com.dabbelt.octomancer-sync.plist
AGENT_LABELS = com.dabbelt.octomancerd com.dabbelt.octomancer-sync

# Installed as data as well as loaded into launchd, because `octomancer start`
# has to be able to install an agent that was never installed, and it needs a
# copy of the plist to do that. The templates stay the one place their contents
# are decided; everything else copies.
nodist_pkgdata_DATA = $(AGENT_PLISTS)

install-agent: install $(AGENT_PLISTS)
	$(MKDIR_P) "$$HOME/Library/LaunchAgents"
	$(MKDIR_P) "$(OCTO_LOGDIR)"
	@for plist in $(AGENT_PLISTS); do \
	  label=`basename $$plist .plist`; \
	  cp "$$plist" "$$HOME/Library/LaunchAgents/$$label.plist"; \
	  launchctl bootout "gui/$$(id -u)/$$label" 2>/dev/null || true; \
	  launchctl bootstrap "gui/$$(id -u)" \
	    "$$HOME/Library/LaunchAgents/$$label.plist" || exit 1; \
	  echo "loaded $$label"; \
	done
	@echo "both agents are running; check them with: octomancer status"

uninstall-agent:
	@for label in $(AGENT_LABELS); do \
	  launchctl bootout "gui/$$(id -u)/$$label" 2>/dev/null || true; \
	  rm -f "$$HOME/Library/LaunchAgents/$$label.plist"; \
	  echo "removed $$label"; \
	done

.PHONY: install-agent uninstall-agent

# ARC everywhere in Objective-C++; manual retain/release in a program that
# hands blocks to a dispatch queue is a bug waiting to be written.
#
# The C++ standard has to be named again here: configure puts -std=c++17 in
# CXXFLAGS, which automake does not pass to the Objective-C++ compiler, so
# without this the .mm files build against an older dialect than the headers
# they include were written for.
AM_OBJCXXFLAGS = -fobjc-arc -std=c++17

##
## Tests. Everything here is portable C++ and runs on any host, which is the
## reason the decoder is kept free of CoreBluetooth.
##

check_PROGRAMS = \
	tests/test_att \
	tests/test_bmd \
	tests/test_boxmsg \
	tests/test_boxsock \
	tests/test_advert \
	tests/test_camdb \
	tests/test_camconf \
	tests/test_camsync \
	tests/test_control \
	tests/test_fakebench \
	tests/test_fakecamera \
	tests/test_fakedaemon \
	tests/test_radio \
	tests/test_crypto \
	tests/test_devices \
	tests/test_devicedb \
	tests/test_hci \
	tests/test_jsonlog \
	tests/test_logscan \
	tests/test_loop \
	tests/test_hcilink \
	tests/test_hcishare \
	tests/test_pairing \
	tests/test_proclock \
	tests/test_proto \
	tests/test_registry \
	tests/test_smp \
	tests/test_syncd \
	tests/test_tentacle \
	tests/test_timeutil \
	tests/test_tui
TESTS = $(check_PROGRAMS)

tests_test_tentacle_SOURCES = tests/test_tentacle.cc tests/harness.h
tests_test_tentacle_LDADD = libocto.a

tests_test_timeutil_SOURCES = tests/test_timeutil.cc tests/harness.h
tests_test_timeutil_LDADD = libocto.a

tests_test_tui_SOURCES = tests/test_tui.cc tests/harness.h
tests_test_tui_LDADD = libocto.a

tests_test_registry_SOURCES = tests/test_registry.cc tests/harness.h
tests_test_registry_LDADD = libocto.a

tests_test_proto_SOURCES = tests/test_proto.cc tests/harness.h
tests_test_proto_LDADD = libocto.a

tests_test_pairing_SOURCES = tests/test_pairing.cc tests/harness.h
tests_test_pairing_LDADD = libocto.a

tests_test_bmd_SOURCES = tests/test_bmd.cc tests/harness.h
tests_test_bmd_LDADD = libocto.a

# The message layer the box speaks over all three transports. Hostile input is
# most of what it pins: names arrive from the air, and a box on a rig answers
# anyone in range.
tests_test_boxmsg_SOURCES = tests/test_boxmsg.cc tests/harness.h
tests_test_boxmsg_LDADD = libocto.a

# The same language over a real socket, with a real poll(2) under it. Nothing
# is faked here on purpose: this file is system calls, and a fake socket would
# be testing the fake.
tests_test_boxsock_SOURCES = tests/test_boxsock.cc tests/harness.h
tests_test_boxsock_LDADD = libocto.a

tests_test_radio_SOURCES = tests/test_radio.cc tests/harness.h
tests_test_radio_LDADD = libocto.a

tests_test_advert_SOURCES = tests/test_advert.cc tests/harness.h
tests_test_advert_LDADD = libocto.a

tests_test_devicedb_SOURCES = tests/test_devicedb.cc tests/harness.h
tests_test_devicedb_LDADD = libocto.a

tests_test_fakebench_SOURCES = tests/test_fakebench.cc tests/harness.h
tests_test_fakebench_LDADD = libocto.a

tests_test_fakecamera_SOURCES = tests/test_fakecamera.cc tests/harness.h
tests_test_fakecamera_LDADD = libocto.a

tests_test_fakedaemon_SOURCES = tests/test_fakedaemon.cc tests/fakedaemon.h \
	tests/harness.h
tests_test_fakedaemon_LDADD = libocto.a

tests_test_camsync_SOURCES = tests/test_camsync.cc tests/harness.h
tests_test_camsync_LDADD = libocto.a

tests_test_camconf_SOURCES = tests/test_camconf.cc tests/harness.h
tests_test_camconf_LDADD = libocto.a

tests_test_camdb_SOURCES = tests/test_camdb.cc tests/harness.h
tests_test_camdb_LDADD = libocto.a

tests_test_control_SOURCES = tests/test_control.cc tests/harness.h
tests_test_control_LDADD = libocto.a

tests_test_devices_SOURCES = tests/test_devices.cc tests/harness.h
tests_test_devices_LDADD = libocto.a

tests_test_proclock_SOURCES = tests/test_proclock.cc tests/harness.h
tests_test_proclock_LDADD = libocto.a

tests_test_jsonlog_SOURCES = tests/test_jsonlog.cc tests/harness.h
tests_test_jsonlog_LDADD = libocto.a

tests_test_logscan_SOURCES = tests/test_logscan.cc tests/harness.h
tests_test_logscan_LDADD = libocto.a

# The event loop, driven on a clock that is a variable. Nothing here waits on
# the wall clock, which is the whole reason the backend is abstract.
tests_test_loop_SOURCES = tests/test_loop.cc tests/harness.h
tests_test_loop_LDADD = libocto.a

tests_test_hcilink_SOURCES = tests/test_hcilink.cc tests/harness.h
tests_test_hcilink_LDADD = libocto.a

# One controller, several jobs. The arbitration this pins down is invisible
# when it is wrong: two Links on one serial port is not an error, it is a radio
# that appears to have switched itself off.
tests_test_hcishare_SOURCES = tests/test_hcishare.cc tests/harness.h
tests_test_hcishare_LDADD = libocto.a

# The dongle's codecs. All four run on a machine with no dongle in it, which is
# the whole argument for having written them as portable C++ rather than as
# firmware: the hard parts are the ones that can be checked here.
tests_test_hci_SOURCES = tests/test_hci.cc tests/harness.h
tests_test_hci_LDADD = libocto.a

tests_test_att_SOURCES = tests/test_att.cc tests/harness.h
tests_test_att_LDADD = libocto.a

tests_test_crypto_SOURCES = tests/test_crypto.cc tests/harness.h
tests_test_crypto_LDADD = libocto.a

tests_test_smp_SOURCES = tests/test_smp.cc tests/harness.h
tests_test_smp_LDADD = libocto.a

# The sync daemon, driven to completion with no radio and no wall-clock time:
# the camera is a scripted object, the boxes are bytes, and the clock is a
# variable. The cases worth having are the ones a bench cannot produce on
# demand -- a camera that answers a connect and then goes silent, one that
# walks out mid-cycle, a write that is taken and changes nothing.
tests_test_syncd_SOURCES = tests/test_syncd.cc tests/harness.h
tests_test_syncd_LDADD = libocto.a
