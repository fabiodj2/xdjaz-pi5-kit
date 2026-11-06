#!/bin/sh
# extract-rootfs.sh ISO DEST -- the XDJ-AZ player's root filesystem out of a
# decrypted firmware 1.30 update ISO (XDJAZv130.iso, ISO 9660 'UsbAuto').
#
# The ISO holds images/images.tar.gz; in it the deck's ARM64 kernel `Image`,
# which carries the whole userland as its initramfs: a gzip'd cpio built into
# the kernel. The cpio is the stream that holds home/root/pdj/EP147.
#
# Runs on any Linux (the Pi itself included) as an ordinary user: device nodes
# in the cpio are skipped (bwrap gives the player its own /dev). Needs bsdtar
# (libarchive-tools), tar, cpio and python3.
set -eu
ISO=${1:?usage: extract-rootfs.sh XDJAZv130.iso DEST}
DEST=${2:?usage: extract-rootfs.sh XDJAZv130.iso DEST}
# EP147 of firmware 1.30, the build everything here is made for (azshim's
# patches, the UCOM layouts, AZ-MODS' tables)
EP147_MD5=3518275e8b4d5d63636d8d7b4468d7ba

[ -e "$DEST/home/root/pdj/EP147" ] && { echo "$DEST already has a rootfs"; exit 0; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "1/3 images.tar.gz out of $ISO"
bsdtar -xf "$ISO" -C "$WORK" images/images.tar.gz
echo "2/3 the kernel Image"
tar -xzf "$WORK/images/images.tar.gz" -C "$WORK" Image
rm -f "$WORK/images/images.tar.gz"

echo "3/3 the initramfs inside it -> $DEST"
mkdir -p "$DEST"
# Each gzip stream in the kernel is tried; the one whose cpio names the
# player is the rootfs.
python3 - "$WORK/Image" "$WORK/initramfs.cpio" <<'EOF'
import sys, zlib
data = open(sys.argv[1], 'rb').read()
at = data.find(b'\x1f\x8b\x08')
while at >= 0:
    try:
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        out = d.decompress(data[at:])
        if out[:6] in (b'070701', b'070702') and b'home/root/pdj/EP147' in out:
            open(sys.argv[2], 'wb').write(out)
            print('   initramfs at 0x%x, %d MB' % (at, len(out) >> 20))
            sys.exit(0)
    except zlib.error:
        pass
    at = data.find(b'\x1f\x8b\x08', at + 1)
sys.exit('no initramfs with EP147 in the kernel image')
EOF
(cd "$DEST" && cpio -idm --quiet --no-absolute-filenames < "$WORK/initramfs.cpio" 2>/dev/null || true)

sum=$(md5sum "$DEST/home/root/pdj/EP147" | cut -d' ' -f1)
if [ "$sum" != "$EP147_MD5" ]; then
    echo "EP147 md5 $sum is not firmware 1.30's ($EP147_MD5): this kit is made for 1.30 only" >&2
    exit 1
fi
echo "rootfs ready in $DEST (EP147 1.30, md5 ok)"
