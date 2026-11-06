#!/bin/sh
# setup-pi.sh -- install the XDJ-AZ player (firmware 1.30) on a Raspberry Pi 5.
# Run on the Pi, from the kit's directory, with sudo:
#
#   sudo ./scripts/setup-pi.sh --iso ~/XDJAZv130.iso [--key ~/onelibrary.key]
#                              [--azmods ~/mods-v1.2.8.tar.gz] [--no-autostart]
#
#   --iso FILE       the decrypted 1.30 update ISO (or --rootfs DIR: one
#                    already extracted with extract-rootfs.sh)
#   --key FILE       the OneLibrary key (one line), for the stick's playlists;
#                    kept in /opt/xdjaz/keys, mode 600, never in the kit
#   --azmods FILE    AZ-MODS' mods-vX.Y.Z.tar.gz (their release): Mod Menu,
#                    stems, ...
#   --no-autostart   do not start the player at boot (by default the Pi boots
#                    straight into it, and the tty1 login is turned off)
#   --user NAME      the user the player runs as (default: who ran sudo)
#
# Safe to run again: it rebuilds and reinstalls everything but the rootfs and
# the player's settings.
set -eu
KIT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$KIT/src
OPT=/opt/xdjaz
ISO="" ROOTFS_IN="" KEY="" AZMODS="" AUTOSTART=1 RUNAS=${SUDO_USER:-}

while [ $# -gt 0 ]; do
    case "$1" in
    --iso) ISO=$2; shift ;;
    --rootfs) ROOTFS_IN=$2; shift ;;
    --key) KEY=$2; shift ;;
    --azmods) AZMODS=$2; shift ;;
    --autostart) AUTOSTART=1 ;;
    --no-autostart) AUTOSTART=0 ;;
    --user) RUNAS=$2; shift ;;
    *) echo "unknown option $1 (see the top of $0)" >&2; exit 2 ;;
    esac
    shift
done
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 1; }
[ -n "$RUNAS" ] && [ "$RUNAS" != root ] || { echo "--user NAME: the player must not run as root" >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo "this is for a 64-bit Raspberry Pi OS (aarch64)" >&2; exit 1; }
step() { echo; echo "== $*"; }

step "packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
    bubblewrap xserver-xorg-core xserver-xorg-input-libinput x11-xserver-utils \
    x11-utils x11-apps xdotool picom xvfb \
    gcc libc6-dev pkg-config libasound2-dev libdrm-dev \
    libarchive-tools cpio python3 python3-pil

step "rootfs -> $OPT/rootfs"
install -d -o "$RUNAS" "$OPT"
if [ -e "$OPT/rootfs/home/root/pdj/EP147" ]; then
    echo "already there, kept (delete $OPT/rootfs to extract again)"
elif [ -n "$ROOTFS_IN" ]; then
    cp -a "$ROOTFS_IN" "$OPT/rootfs"
else
    [ -n "$ISO" ] || { echo "--iso FILE (or --rootfs DIR) is needed the first time" >&2; exit 1; }
    sudo -u "$RUNAS" sh "$KIT/scripts/extract-rootfs.sh" "$ISO" "$OPT/rootfs"
fi
# The player runs as root inside its own user namespace, i.e. as $RUNAS
# outside: it writes its settings and caches into the rootfs.
chown -R "$RUNAS:" "$OPT/rootfs"
install -d -o "$RUNAS" "$OPT/state"

step "build (on the Pi: azshim must link against this glibc, see azshim.c)"
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
# azshim: only symbols the player's glibc 2.29 has (GLIBC_2.17), so no stack
# protector and no fortify
gcc -O2 -Wall -shared -fPIC -fno-stack-protector -U_FORTIFY_SOURCE \
    -o "$B/azshim.so" "$SRC/shims/azshim.c"
if objdump -T "$B/azshim.so" | grep -E 'GLIBC_2\.([2-9][0-9]|1[89])' | grep -v GLIBC_2.17; then
    echo "azshim.so needs a newer glibc than the player has" >&2; exit 1
fi
gcc -O2 -Wall -o "$B/azbridge" "$SRC/bridge/azbridge.c" -lasound -lm
gcc -O2 -Wall -o "$B/azmix" "$SRC/mixer/azmix.c" "$SRC/mixer/fx.c" -lasound -lm
gcc -O2 -Wall -o "$B/aztouch" "$SRC/touch/aztouch.c"
gcc -O2 -Wall -o "$B/azvsync" "$SRC/vsync/azvsync.c" $(pkg-config --cflags --libs libdrm)

step "install into $OPT"
install -d "$OPT/shims" "$OPT/bridge" "$OPT/mixer" "$OPT/touch" "$OPT/vsync" "$OPT/tools"
install -m 755 "$B/azshim.so" "$OPT/shims/azshim.so"
install -m 755 "$B/azbridge" "$OPT/bridge/azbridge"
install -m 755 "$B/azmix" "$OPT/mixer/azmix"
install -m 755 "$B/aztouch" "$OPT/touch/aztouch"
install -m 755 "$B/azvsync" "$OPT/vsync/azvsync"
install -m 755 "$SRC/probe/run-probe.sh" "$OPT/run-probe.sh"
install -m 644 "$SRC/probe/xorg-az.conf" "$OPT/xorg-az.conf"
install -m 755 "$SRC/probe/dconf" "$OPT/dconf"
install -m 755 "$SRC/deploy/xdjaz-prep.sh" "$OPT/xdjaz-prep.sh"
install -m 755 "$SRC/deploy/xdjaz-usb.sh" "$OPT/xdjaz-usb.sh"
install -m 755 "$SRC/tools/midi.sh" "$SRC/tools/shot.py" "$OPT/tools/"
# the logs the pieces write next to themselves
chown "$RUNAS:" "$OPT" "$OPT/bridge" "$OPT/vsync"

if [ -n "$KEY" ]; then
    step "OneLibrary key"
    install -d -m 700 -o "$RUNAS" "$OPT/keys"
    tr -d '\r\n' < "$KEY" > "$OPT/keys/onelibrary.key"
    chown "$RUNAS:" "$OPT/keys/onelibrary.key"
    chmod 600 "$OPT/keys/onelibrary.key"
fi

if [ -n "$AZMODS" ]; then
    step "AZ-MODS from $AZMODS"
    M=$(mktemp -d)
    tar -xzf "$AZMODS" -C "$M" azmod.so azstem.so azmod-pngpatch
    install -d "$OPT/azmod"
    install -m 644 "$M/azmod.so" "$M/azstem.so" "$OPT/azmod/"
    S=$OPT/rootfs/home/root/settings
    install -d -o "$RUNAS" "$S"
    rm -rf "$S/azmod-pngpatch"
    cp -r "$M/azmod-pngpatch" "$S/"
    chown -R "$RUNAS:" "$S/azmod-pngpatch"
    rm -rf "$M"
fi

step "systemd unit xdjaz.service (as $RUNAS)"
# The kit's own unit (systemd/xdjaz.service): started at boot, started again
# when the player ends or fails, waits for the controller however long it takes.
if [ -e /etc/systemd/system/rblaunch.service ]; then
    # A Pi with the boot menu (rblaunch, several DJ systems): the menu starts
    # the player; its unit is the one made for that, and nothing starts at boot.
    echo "boot menu (rblaunch) found: the menu's unit, no autostart"
    sed "s/^User=.*/User=$RUNAS/" "$SRC/deploy/xdjaz.service" > /etc/systemd/system/xdjaz.service
    AUTOSTART=menu
else
    sed "s/@USER@/$RUNAS/" "$KIT/systemd/xdjaz.service" > /etc/systemd/system/xdjaz.service
fi
systemctl daemon-reload
if [ "$AUTOSTART" = 1 ]; then
    systemctl enable xdjaz.service
    # The console login on tty1 would share the screen with the player's X.
    systemctl disable getty@tty1.service 2>/dev/null || true
    echo "the Pi now boots straight into the player; tty1 login off"
    echo "(undo: sudo systemctl disable xdjaz; sudo systemctl enable getty@tty1)"
elif [ "$AUTOSTART" = 0 ]; then
    systemctl disable xdjaz.service 2>/dev/null || true
    systemctl enable getty@tty1.service 2>/dev/null || true
fi

step "done"
cat <<EOF
Start:   sudo systemctl start xdjaz      (or reboot; it waits for the controller)
Stop:    sudo systemctl stop xdjaz
Logs:    journalctl -u xdjaz; $OPT/ep147.log, $OPT/bridge/azbridge.log, $OPT/mixer/azmix.log
EOF
