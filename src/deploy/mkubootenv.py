#!/usr/bin/env python3
"""mkubootenv: write a U-Boot environment image, the file fw_printenv reads.

The AZ keeps its U-Boot environment in its eMMC (/etc/fw_env.config: /dev/mmcblk1 at
0x3f8000). The player asks it for the firmware release ("fw_printenv -n release", the
information screen's Version No.). The Pi has no such flash, so run-probe.sh points
fw_env.config at a plain file made by this script.

Layout (no redundant copy): a little-endian CRC-32 of the data area, then
"name=value\\0" for each variable, a final "\\0", zero padding up to SIZE.

  mkubootenv.py FILE [--size 0x8000] NAME=VALUE ...
"""
import struct
import sys
import zlib


def build(pairs, size=0x8000):
    data = b"".join(f"{k}={v}".encode() + b"\0" for k, v in pairs) + b"\0"
    if len(data) > size - 4:
        raise SystemExit("mkubootenv: variables do not fit in %#x bytes" % size)
    data = data.ljust(size - 4, b"\0")
    return struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF) + data


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    out, size, pairs = argv[0], 0x8000, []
    rest = argv[1:]
    while rest:
        a = rest.pop(0)
        if a == "--size" and rest:
            size = int(rest.pop(0), 0)
        elif "=" in a:
            pairs.append(tuple(a.split("=", 1)))
        else:
            raise SystemExit("mkubootenv: bad argument %r" % a)
    img = build(pairs, size)
    with open(out, "wb") as f:
        f.write(img)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
