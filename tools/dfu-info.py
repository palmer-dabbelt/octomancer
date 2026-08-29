#!/usr/bin/env python3
#
# Ask a dongle's bootloader what is actually on it.
#
# When a freshly flashed image does not come up, the useful question is not
# "did the write succeed" -- DFU says so, and says so truthfully -- but "where
# did the bootloader put it, and what else is in the way". Nordic's secure DFU
# protocol will answer both, and nothing else in the toolchain surfaces it.
#
# This writes nothing. It leaves the dongle in DFU mode, so whatever you decide
# to do about the answer can be done in the same session, without asking a
# person to hold a button again.
#
# Usage: tools/dfu-info.py [PORT]
#
# Stdlib only, deliberately: termios can set a USB CDC port raw perfectly well,
# and a diagnostic that needs a virtualenv is one you cannot run at the moment
# you need it.

import glob
import os
import select
import struct
import sys
import termios

END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD

# Nordic secure DFU opcodes. Only the read-only ones are here on purpose.
OP_PING = 0x09
OP_HARDWARE_VERSION = 0x0A
OP_FIRMWARE_VERSION = 0x0B

IMAGE_TYPES = {0: "SOFTDEVICE", 1: "APPLICATION", 2: "BOOTLOADER", 0xFF: "none"}


def slip_encode(data):
    out = bytearray()
    for b in data:
        if b == END:
            out += bytes([ESC, ESC_END])
        elif b == ESC:
            out += bytes([ESC, ESC_ESC])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


def open_raw(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
    iflag = 0
    oflag = 0
    lflag = 0
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(cc)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
    return fd


def exchange(fd, payload, timeout=3.0):
    """Send one DFU request, return the decoded response body."""
    os.write(fd, slip_encode(payload))
    out = bytearray()
    esc = False
    deadline = select.select  # local alias, keeps the loop below readable
    import time
    end_at = time.time() + timeout
    while time.time() < end_at:
        r, _, _ = deadline([fd], [], [], 0.1)
        if not r:
            continue
        chunk = os.read(fd, 256)
        for b in chunk:
            if b == END:
                if out:
                    return bytes(out)
                continue
            if esc:
                out.append(END if b == ESC_END else (ESC if b == ESC_ESC else b))
                esc = False
            elif b == ESC:
                esc = True
            else:
                out.append(b)
    return None


def ok(resp, opcode):
    # Every response is 0x60, the opcode it answers, then a result code.
    return resp is not None and len(resp) >= 3 and resp[0] == 0x60 \
        and resp[1] == opcode and resp[2] == 1


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else None
    if not port:
        candidates = sorted(glob.glob("/dev/cu.usbmodem*")) + \
                     sorted(glob.glob("/dev/ttyACM*"))
        if not candidates:
            sys.exit("no serial port found; is the dongle plugged in?")
        port = candidates[0]

    fd = open_raw(port)
    try:
        resp = exchange(fd, bytes([OP_PING, 0x01]))
        if not ok(resp, OP_PING):
            sys.exit(f"{port}: no answer to a DFU ping.\n"
                     "This is not a bootloader. Hold the dongle's button while\n"
                     "plugging it in (see README.md) and try again.")
        print(f"port: {port}")

        resp = exchange(fd, bytes([OP_HARDWARE_VERSION]))
        if ok(resp, OP_HARDWARE_VERSION) and len(resp) >= 23:
            part, variant, rom, ram, page = struct.unpack("<IIIII", resp[3:23])
            var = struct.pack(">I", variant).decode("ascii", "replace")
            print(f"chip: nRF{part:X} variant {var} "
                  f"rom {rom // 1024}K ram {ram // 1024}K page {page}")

        # Image 0 is whatever was installed first; the type field is what
        # matters, not the index. A SOFTDEVICE here is the thing to look for:
        # it sits in front of the application and moves where the application
        # has to be linked.
        for n in range(4):
            resp = exchange(fd, bytes([OP_FIRMWARE_VERSION, n]))
            if not ok(resp, OP_FIRMWARE_VERSION) or len(resp) < 16:
                continue
            kind = IMAGE_TYPES.get(resp[3], f"0x{resp[3]:02X}")
            version, addr, length = struct.unpack("<III", resp[4:16])
            if kind == "none" and length == 0:
                continue
            print(f"image {n}: {kind:<11} addr 0x{addr:08X} "
                  f"len {length} version {version}")
    finally:
        os.close(fd)

    print()
    print("The dongle is still in DFU mode; nothing was written.")


if __name__ == "__main__":
    main()
