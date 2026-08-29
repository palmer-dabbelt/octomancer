#!/bin/sh
#
# Put an HCI firmware on an nRF52840 dongle.
#
# The dongle needs to speak raw HCI over a serial port for anything in this
# project to talk to it. Nordic ships it with a DFU bootloader, so the job is
# to build (or fetch) a Zephyr `hci_uart` image and push it over that
# bootloader -- no debugger, no soldering, nothing but the USB port.
#
# Deliberately not part of `make install`. Flashing a device is not something a
# build should do on anybody's behalf, and the image this writes is the whole
# personality of the radio.
#
# Usage:
#   tools/flash-dongle.sh --check              is the toolchain here?
#   tools/flash-dongle.sh --setup              fetch west, the modules and the SDK
#   tools/flash-dongle.sh --build              build hci_uart from the submodule
#   tools/flash-dongle.sh --package IMAGE.hex  wrap a hex file for DFU
#   tools/flash-dongle.sh --flash PACKAGE.zip  push it to a dongle in DFU mode
#   tools/flash-dongle.sh --ports              list serial ports that look right
#
# Putting the dongle into DFU mode: press the small side button (SW1, next to
# the USB connector, not the one on the end) and hold it while plugging in --
# or press RESET while holding it. The red LED pulses slowly when it is
# listening for a firmware image.

set -e

PROG=$(basename "$0")

die() {
    echo "$PROG: $*" >&2
    exit 1
}

usage() {
    sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# Two different programs can speak Nordic's DFU protocol, and on a current macOS
# neither of them is simply "install nrfutil":
#
#   nrfutil           Nordic's own. `pip install nrfutil` still resolves and
#                     still installs, which is the trap -- what it installs is
#                     Python 2 code, and `pkg generate` dies inside
#                     `dict.iteritems` on any modern interpreter. The current
#                     nrfutil is a prebuilt binary instead; the Homebrew cask
#                     for it was deprecated for failing macOS's Gatekeeper
#                     check and is disabled from 2026-09-01.
#
#   adafruit-nrfutil  A Python 3 fork of that same tool, speaking the same
#                     nRF5 SDK DFU protocol over the same serial transport.
#                     It installs and it runs. The commands are spelled
#                     differently, which is all this script has to absorb.
#
# So take whichever is present rather than insisting on a name. Preferring
# nrfutil when both exist keeps Nordic's own tool authoritative.
dfu_tool() {
    if command -v nrfutil >/dev/null 2>&1; then
        echo nrfutil
    elif command -v adafruit-nrfutil >/dev/null 2>&1; then
        echo adafruit-nrfutil
    else
        echo none
    fi
}

check() {
    ok=0
    tool=$(dfu_tool)
    if [ "$tool" = none ]; then
        echo "missing: a DFU tool -- no nrfutil, no adafruit-nrfutil"
        echo "    pip install adafruit-nrfutil"
        echo "  (pip install nrfutil also resolves, but installs Python 2"
        echo "   code that cannot generate a package on a modern interpreter)"
        ok=1
    else
        echo "found: $tool ($(command -v "$tool"))"
    fi

    if command -v west >/dev/null 2>&1; then
        echo "found: west ($(command -v west)) -- you can build hci_uart yourself"
    else
        echo "note:  no west on PATH."
        echo "    third_party/.venv/bin/west is the one this tree builds with;"
        echo "    see doc/dongle-notes.md. Failing that, download a prebuilt"
        echo "    hci_uart image for the nrf52840dongle board and use --package."
    fi

    echo
    echo "serial ports that look like a dongle:"
    ports
    return $ok
}

ports() {
    found=0
    for p in /dev/cu.usbmodem* /dev/ttyACM*; do
        [ -e "$p" ] || continue
        echo "    $p"
        found=1
    done
    [ "$found" = 1 ] || echo "    (none)"
}

# Where the firmware side of this tree lives. third_party/zephyr is a
# submodule pinned at a Zephyr release, so the image built here is a function
# of this repository's commit and nothing else -- west takes every module
# revision from that pinned tree's own manifest.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP=$ROOT/third_party
WEST=$TP/.venv/bin/west
SDK=$(echo "$TP"/zephyr-sdk-* 2>/dev/null | tr ' ' '\n' | tail -1)

# Every GitHub URL on this machine may be rewritten to ssh:// by a global
# `url.<ssh>.insteadOf` rule, and an agent or a CI runner with no key then
# fails on a clone that never mentions URLs. Trying to rewrite it back does
# not work: insteadOf is applied once, picking the longest matching prefix,
# so a rule mapping ssh:// to https:// simply never matches an https:// URL
# that is about to be rewritten. Dropping the global config for the duration
# is the thing that does work.
nokeys() {
    GIT_CONFIG_GLOBAL=/dev/null "$@"
}

# One command from a fresh clone to something that can build. Safe to re-run:
# every step below either already exists or is idempotent.
setup() {
    [ -f "$TP/zephyr/VERSION" ] || die "third_party/zephyr is empty; run: git submodule update --init"

    if [ ! -x "$WEST" ]; then
        echo "creating $TP/.venv ..."
        python3 -m venv "$TP/.venv"
        "$TP/.venv/bin/pip" install --quiet --upgrade pip west pyelftools patool packaging
        "$TP/.venv/bin/pip" install --quiet -r "$TP/zephyr/scripts/requirements-base.txt"
    fi

    if [ ! -d "$TP/.west" ]; then
        echo "initialising the west workspace ..."
        (cd "$TP" && nokeys "$WEST" init -l zephyr)
        # Zephyr's manifest carries every vendor HAL there is, and all but one
        # of them is megabytes of silicon this project will never run on.
        (cd "$TP" && "$WEST" config manifest.project-filter -- \
            "-.*,+cmsis,+cmsis_6,+hal_nordic,+picolibc,+segger,+mbedtls,+tf-psa-crypto,+zcbor")
    fi
    echo "fetching modules ..."
    (cd "$TP" && nokeys "$WEST" update --narrow -o=--depth=1)

    SDK=$(echo "$TP"/zephyr-sdk-* 2>/dev/null | tr ' ' '\n' | tail -1)
    if [ ! -d "$SDK" ]; then
        echo "installing the Zephyr SDK (arm-zephyr-eabi only) ..."
        (cd "$TP" && "$WEST" sdk install -b "$TP" -t arm-zephyr-eabi)
    fi

    echo
    echo "ready. next: $0 --build"
}

# hci_uart, not hci_usb, and the difference is the whole thing.
#
# hci_usb builds a USB Bluetooth *class* device: its prj.conf sets
# CONFIG_SERIAL=n and CONFIG_USBD_BT_HCI=y, so there is no serial port on it
# at all. Linux binds such a device with btusb and gives you an hci0; macOS
# has no driver for the class and gives you nothing. This project's transport
# is a serial port either way -- src/hciport_posix.cc opens /dev/cu.usbmodem*
# or /dev/ttyACM* -- so hci_usb produces a dongle nothing here can reach.
#
# hci_uart carries the same raw HCI over a UART, and the dongle's board file
# in Zephyr points that UART at CDC ACM. The dongle then enumerates as an
# ordinary serial port with H:4 packets flowing over it, which is exactly what
# hcilink.cc expects to find.
#
# Building it here rather than shipping a binary keeps this project free of
# somebody else's compiled code.
build() {
    out=${1:-$TP/build-hci}
    [ -x "$WEST" ] || die "no west in the tree; run: $0 --setup"
    [ -d "$SDK" ] || die "no Zephyr SDK in the tree; run: $0 --setup"

    echo "building hci_uart for nrf52840dongle into $out ..."
    ZEPHYR_BASE=$TP/zephyr \
    ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
    ZEPHYR_SDK_INSTALL_DIR=$SDK \
        "$WEST" build -b nrf52840dongle/nrf52840 -d "$out" \
        "$TP/zephyr/samples/bluetooth/hci_uart"
    echo "built: $out/zephyr/zephyr.hex"
    echo "next:  $0 --package $out/zephyr/zephyr.hex"
}

# The dongle's bootloader will not take a bare hex file: it wants a DFU
# package. No signing key is passed, because the Open Bootloader the dongle
# ships with accepts an unsigned application -- that is the standard procedure
# for this board and is not a security measure being worked around.
package() {
    hex=$1
    [ -n "$hex" ] || die "--package needs a .hex file"
    [ -f "$hex" ] || die "$hex: no such file"
    tool=$(dfu_tool)
    [ "$tool" = none ] && die "no DFU tool installed; see --check"

    out=$(dirname "$hex")/hci_uart_dfu.zip
    rm -f "$out"
    case $tool in
        nrfutil)
            nrfutil pkg generate \
                --hw-version 52 \
                --sd-req 0x00 \
                --application "$hex" \
                --application-version 1 \
                "$out"
            ;;
        adafruit-nrfutil)
            # 0x0052 is the nRF52840; --sd-req has no equivalent here and is
            # not needed, since the image carries no SoftDevice.
            adafruit-nrfutil dfu genpkg \
                --dev-type 0x0052 \
                --application "$hex" \
                --application-version 1 \
                "$out"
            ;;
    esac
    echo "packaged: $out"
    echo "next:  put the dongle in DFU mode (hold SW1 while plugging in),"
    echo "       then: $0 --flash $out"
}

flash() {
    pkg=$1
    [ -n "$pkg" ] || die "--flash needs a .zip package"
    [ -f "$pkg" ] || die "$pkg: no such file"
    tool=$(dfu_tool)
    [ "$tool" = none ] && die "no DFU tool installed; see --check"

    # In DFU mode the dongle appears as its own serial port. Which one it is
    # depends on what else is plugged in, so the first candidate is a guess
    # and is stated as one rather than used silently.
    port=$2
    if [ -z "$port" ]; then
        for p in /dev/cu.usbmodem* /dev/ttyACM*; do
            [ -e "$p" ] || continue
            port=$p
            break
        done
    fi
    [ -n "$port" ] || die "no serial port found; is the dongle in DFU mode?"
    echo "flashing $pkg to $port with $tool ..."
    case $tool in
        nrfutil)
            nrfutil dfu usb-serial -pkg "$pkg" -p "$port"
            ;;
        adafruit-nrfutil)
            adafruit-nrfutil dfu serial -pkg "$pkg" -p "$port" -b 115200
            ;;
    esac

    echo
    echo "done. Unplug and replug the dongle -- it will come back as a plain"
    echo "serial port with no DFU button pressed. Then check it with:"
    echo "    octomancer-zoom --scan 10"
}

case "${1:-}" in
    --check)   shift; check ;;
    --setup)   shift; setup ;;
    --ports)   shift; ports ;;
    --build)   shift; build "$@" ;;
    --package) shift; package "$@" ;;
    --flash)   shift; flash "$@" ;;
    --help|-h|"") usage 0 ;;
    *) die "unknown option $1 (try --help)" ;;
esac
