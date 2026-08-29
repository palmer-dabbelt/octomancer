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
#   tools/flash-dongle.sh --info               ask a bootloader what is on it
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

# Nordic's own nrfutil is the only thing here that works, and finding that out
# cost an afternoon, so it is written down.
#
# The dongle's Open Bootloader speaks Nordic's *secure* DFU: a protobuf init
# packet, and a transport that acknowledges each step. There is an older,
# simpler Nordic DFU protocol from the nRF5 SDK days, and the tools that speak
# it are the ones that are easy to install:
#
#   pip install nrfutil            resolves, installs, and is Python 2 all the
#                                  way down -- iteritems, xrange, integer
#                                  division. Patching the first two only gets
#                                  you to the third.
#
#   pip install adafruit-nrfutil   installs cleanly and runs, and speaks the
#                                  *old* protocol. Against this bootloader it
#                                  sends a legacy init packet, gets no
#                                  acknowledgement, times out -- and then exits
#                                  0, printing "done". A tool that reports
#                                  success when the device was not programmed
#                                  is worse than one that does not run, so it
#                                  is not accepted here at all.
#
#   nrfutil (the binary)           Nordic's current one, a native arm64 binary
#                                  fetched straight from Nordic. `nrfutil
#                                  install nrf5sdk-tools` adds the DFU
#                                  commands, which are pc-nrfutil 6.1.7 -- the
#                                  Python 3 release pip will not give you
#                                  because it predates your interpreter. This
#                                  one programs the dongle.
#
# --setup fetches it into third_party/bin, so none of the above has to be
# rediscovered.
NRFUTIL_URL=https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/aarch64-apple-darwin/nrfutil

dfu_tool() {
    if [ -x "$TP/bin/nrfutil" ]; then
        echo "$TP/bin/nrfutil"
    elif command -v nrfutil >/dev/null 2>&1; then
        command -v nrfutil
    else
        echo none
    fi
}

# Every nrfutil invocation needs this, or it scatters its downloaded commands
# through the user's home directory.
nrfutil_env() {
    NRFUTIL_HOME=$TP/.nrfutil
    export NRFUTIL_HOME
}

check() {
    ok=0
    tool=$(dfu_tool)
    if [ "$tool" = none ]; then
        echo "missing: nrfutil"
        echo "    $0 --setup   (fetches it; see the note above about the"
        echo "                  pip packages, which do not work)"
        ok=1
    else
        echo "found: nrfutil ($tool)"
    fi

    if [ -x "$TP/.venv/bin/west" ]; then
        echo "found: west ($TP/.venv/bin/west) -- hci_uart can be built here"
    else
        echo "missing: west -- run: $0 --setup"
        ok=1
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

    if [ ! -x "$TP/bin/nrfutil" ]; then
        echo "fetching nrfutil ..."
        mkdir -p "$TP/bin"
        curl -sSL --fail -o "$TP/bin/nrfutil" "$NRFUTIL_URL"
        chmod +x "$TP/bin/nrfutil"
        nrfutil_env
        "$TP/bin/nrfutil" install nrf5sdk-tools
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
# for this board, and nrfutil prints its own warning saying so.
package() {
    hex=$1
    [ -n "$hex" ] || die "--package needs a .hex file"
    [ -f "$hex" ] || die "$hex: no such file"
    tool=$(dfu_tool)
    [ "$tool" = none ] && die "no nrfutil; run: $0 --setup"
    nrfutil_env

    out=$(dirname "$hex")/hci_uart_dfu.zip
    rm -f "$out"
    "$tool" nrf5sdk-tools pkg generate \
        --hw-version 52 \
        --sd-req 0x00 \
        --application "$hex" \
        --application-version 1 \
        "$out"
    echo "packaged: $out"
    echo "next:  put the dongle in DFU mode, then: $0 --flash $out"
}

flash() {
    pkg=$1
    [ -n "$pkg" ] || die "--flash needs a .zip package"
    [ -f "$pkg" ] || die "$pkg: no such file"
    tool=$(dfu_tool)
    [ "$tool" = none ] && die "no nrfutil; run: $0 --setup"
    nrfutil_env

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

    # Worth checking rather than discovering halfway through a write: a dongle
    # that is not in DFU mode still has a serial port, and it will simply not
    # answer.
    if command -v ioreg >/dev/null 2>&1; then
        if ! ioreg -l -w 0 | grep -q "Open DFU Bootloader"; then
            echo "warning: no 'Open DFU Bootloader' on the USB bus." >&2
            echo "         The dongle is probably running its application," >&2
            echo "         not its bootloader. See README.md for the button." >&2
        fi
    fi

    echo "flashing $pkg to $port ..."
    "$tool" nrf5sdk-tools dfu usb-serial -pkg "$pkg" -p "$port"

    echo
    echo "done. Unplug and replug the dongle -- it does not come back on its"
    echo "own after programming. It should return as 'Zephyr HCI UART sample';"
    echo "check with:"
    echo "    ioreg -l -w 0 | grep -A4 Nordic"
    echo "    octomancer-zoom --scan 10"
}

case "${1:-}" in
    --check)   shift; check ;;
    --setup)   shift; setup ;;
    --ports)   shift; ports ;;
    --info)    shift; exec "$(dirname "$0")/dfu-info.py" "$@" ;;
    --build)   shift; build "$@" ;;
    --package) shift; package "$@" ;;
    --flash)   shift; flash "$@" ;;
    --help|-h|"") usage 0 ;;
    *) die "unknown option $1 (try --help)" ;;
esac
