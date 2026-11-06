#!/bin/sh
# make-kit.sh [AZ_PI5_DIR] -- refresh src/ from the working tree (../az-pi5 by
# default), the one place the sources are edited. Only what the Pi needs is
# copied: no firmware, keys, AZ-MODS files or screen dumps (az-pi5/.gitignore
# keeps those out of git too).
set -eu
KIT=$(cd "$(dirname "$0")" && pwd)
FROM=$(cd "${1:-$KIT/../az-pi5}" && pwd)
rm -rf "$KIT/src"
mkdir -p "$KIT/src"
cd "$FROM"
for f in bridge/azbridge.c bridge/UCOM-PROTOCOL.md \
         mixer/azmix.c mixer/fx.c mixer/fx.h \
         shims/azshim.c touch/aztouch.c vsync/azvsync.c \
         probe/run-probe.sh probe/xorg-az.conf probe/dconf \
         deploy/xdjaz-prep.sh deploy/xdjaz-usb.sh deploy/xdjaz.service \
         tools/midi.sh tools/shot.py docs/panel.md; do
    mkdir -p "$KIT/src/$(dirname "$f")"
    cp -p "$f" "$KIT/src/$f"
done
rev=$(git -C "$FROM" rev-parse --short HEAD 2>/dev/null || echo none)
dirty=$(git -C "$FROM" status --porcelain 2>/dev/null | grep -q . && echo " + uncommitted changes" || true)
echo "az-pi5 $rev$dirty, $(date -I)" > "$KIT/src/SOURCE"
echo "src/ refreshed from $FROM ($rev$dirty)"
