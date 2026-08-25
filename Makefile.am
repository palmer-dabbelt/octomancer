## octomancer -- non-recursive build.
##
## One Makefile for the whole tree: the parts are small and interdependent, and
## a recursive build would buy nothing but slower dependency tracking.

ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir)

EXTRA_DIST = autogen.sh README.md tests/data/adverts.golden

noinst_LIBRARIES = libocto.a
libocto_a_SOURCES = \
	src/client.cc \
	src/client.h \
	src/jsonlog.cc \
	src/jsonlog.h \
	src/proto.cc \
	src/proto.h \
	src/registry.cc \
	src/registry.h \
	src/render.cc \
	src/render.h \
	src/server.cc \
	src/server.h \
	src/tentacle.cc \
	src/tentacle.h \
	src/timeutil.cc \
	src/timeutil.h

##
## The service and its control tool. Both are macOS-only: the radio is
## CoreBluetooth, and there is no second backend to fall back to.
##

if DARWIN
bin_PROGRAMS = octomancerd octomancerctl

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

$(APPBUNDLE)/Contents/MacOS/Octomancer: octomancer-ui ui/Info.plist
	$(AM_V_GEN)rm -rf $(APPBUNDLE)
	$(AM_V_at)$(MKDIR_P) $(APPBUNDLE)/Contents/MacOS $(APPBUNDLE)/Contents/Resources
	$(AM_V_at)cp ui/Info.plist $(APPBUNDLE)/Contents/Info.plist
	$(AM_V_at)cp octomancer-ui $@
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

AGENT_PLIST = launchd/com.dabbelt.octomancerd.plist
AGENT_LABEL = com.dabbelt.octomancerd

install-agent: install $(AGENT_PLIST)
	$(MKDIR_P) "$$HOME/Library/LaunchAgents"
	cp $(AGENT_PLIST) "$$HOME/Library/LaunchAgents/$(AGENT_LABEL).plist"
	-launchctl bootout "gui/$$(id -u)/$(AGENT_LABEL)" 2>/dev/null
	launchctl bootstrap "gui/$$(id -u)" "$$HOME/Library/LaunchAgents/$(AGENT_LABEL).plist"
	@echo "octomancerd is running; check it with: octomancerctl status"

uninstall-agent:
	-launchctl bootout "gui/$$(id -u)/$(AGENT_LABEL)" 2>/dev/null
	rm -f "$$HOME/Library/LaunchAgents/$(AGENT_LABEL).plist"

.PHONY: install-agent uninstall-agent

# ARC everywhere in Objective-C++; manual retain/release in a program that
# hands blocks to a dispatch queue is a bug waiting to be written.
AM_OBJCXXFLAGS = -fobjc-arc

##
## Tests. Everything here is portable C++ and runs on any host, which is the
## reason the decoder is kept free of CoreBluetooth.
##

check_PROGRAMS = \
	tests/test_proto \
	tests/test_registry \
	tests/test_tentacle \
	tests/test_timeutil
TESTS = $(check_PROGRAMS)

tests_test_tentacle_SOURCES = tests/test_tentacle.cc tests/harness.h
tests_test_tentacle_LDADD = libocto.a

tests_test_timeutil_SOURCES = tests/test_timeutil.cc tests/harness.h
tests_test_timeutil_LDADD = libocto.a

tests_test_registry_SOURCES = tests/test_registry.cc tests/harness.h
tests_test_registry_LDADD = libocto.a

tests_test_proto_SOURCES = tests/test_proto.cc tests/harness.h
tests_test_proto_LDADD = libocto.a
