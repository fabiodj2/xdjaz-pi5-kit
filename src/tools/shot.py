#!/usr/bin/env python3
# shot.py out.png: the panel (X :0) as a PNG. The modesetting server reports
# 24 bits per pixel, packed 3 bytes, with 4096-byte lines.
import struct, subprocess, sys
from PIL import Image
d = subprocess.run(["xwd", "-root", "-silent", "-display", ":0"], capture_output=True).stdout
h = struct.unpack(">25I", d[:100])
off = h[0] + h[19] * 12
bpp = h[11] // 8
mode = "BGR" if bpp == 3 else "BGRX"
Image.frombuffer("RGB" if bpp == 3 else "RGBX", (h[4], h[5]), d[off:off + h[12] * h[5]], "raw", mode, h[12], 1).convert("RGB").save(sys.argv[1])
