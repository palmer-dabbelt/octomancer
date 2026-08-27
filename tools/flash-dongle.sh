#!/bin/sh
#
# Put an HCI firmware on an nRF52840 dongle.
#
# The dongle needs to speak raw HCI over USB for anything in this project to
# talk to it. Nordic ships it with a DFU bootloader and no application, so the
# job is to build (or fetch) a Zephyr `hci_usb` image and push it over that
# bootloader -- no debugger, no soldering, nothing but the USB port.
#
# Deliberately not part of `make install`. Flashing a device is not something a
# build should do on anybody's behalf, and the image this writes is the whole
# personality of the radio.
#
# Usage:
#   tools/flash-dongle.sh --check              is the toolchain here?
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

# nrfutil is Nordic's own tool and the only one that speaks their DFU protocol.
# Recent versions moved the DFU commands into a subcommand that has to be
# installed separately, which is worth saying out loud: "nrfutil: command not
# found" and "nrfutil has no idea what pkg means" look nothing alike and have
# the same cause.
check() {
    ok=0
    if command -v nrfutil >/dev/null 2>&1; then
        echo "found: nrfutil ($(command -v nrfutil))"
    else
        echo "missing: nrfutil"
        echo "    pip install nrfutil"
        echo "  or fetch it from https://www.nordicsemi.com/Products/Development-tools/nrf-util"
        ok=1
    fi

    if command -v west >/dev/null 2>&1; then
        echo "found: west ($(command -v west)) -- you can build hci_usb yourself"
    else
        echo "note:  no west, so no Zephyr build here."
        echo "    Either install the Zephyr SDK, or download a prebuilt"
        echo "    hci_usb image for the nrf52840dongle board and use --package."
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

# Zephyr's hci_usb sample is the reference HCI-over-USB application. Building
# it here rather than shipping a binary keeps this project free of somebody
# else's compiled code.
build() {
    command -v west >/dev/null 2>&1 || die "west is not installed; see --check"
    [ -n "$ZEPHYR_BASE" ] || die "ZEPHYR_BASE is not set; source your Zephyr env first"
    out=${1:-build-hci}
    echo "building hci_usb for nrf52840dongle into $out ..."
    west build -b nrf52840dongle/nrf52840 -d "$out" \
        "$ZEPHYR_BASE/samples/bluetooth/hci_usb"
    echo "built: $out/zephyr/zephyr.hex"
    echo "next:  $0 --package $out/zephyr/zephyr.hex"
}

# The dongle's bootloader will not take a bare hex file: it wants a signed DFU
# package. The debug key below is the one Nordic's own bootloader accepts by
# default -- this is not a security measure and is not treated as one.
package() {
    hex=$1
    [ -n "$hex" ] || die "--package needs a .hex file"
    [ -f "$hex" ] || die "$hex: no such file"
    command -v nrfutil >/dev/null 2>&1 || die "nrfutil is not installed; see --check"

    out=$(dirname "$hex")/hci_usb_dfu.zip
    rm -f "$out"
    nrfutil pkg generate \
        --hw-version 52 \
        --sd-req 0x00 \
        --application "$hex" \
        --application-version 1 \
        "$out"
    echo "packaged: $out"
    echo "next:  put the dongle in DFU mode (hold SW1 while plugging in),"
    echo "       then: $0 --flash $out"
}

flash() {
    pkg=$1
    [ -n "$pkg" ] || die "--flash needs a .zip package"
    [ -f "$pkg" ] || die "$pkg: no such file"
    command -v nrfutil >/dev/null 2>&1 || die "nrfutil is not installed; see --check"

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
    echo "flashing $pkg to $port ..."
    nrfutil dfu usb-serial -pkg "$pkg" -p "$port"

    echo
    echo "done. Unplug and replug the dongle -- it will come back as a plain"
    echo "serial port with no DFU button pressed. Then check it with:"
    echo "    octomancer-zoom --scan 10"
}

case "${1:-}" in
    --check)   shift; check ;;
    --ports)   shift; ports ;;
    --build)   shift; build "$@" ;;
    --package) shift; package "$@" ;;
    --flash)   shift; flash "$@" ;;
    --help|-h|"") usage 0 ;;
    *) die "unknown option $1 (try --help)" ;;
esac
