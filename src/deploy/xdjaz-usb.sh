#!/bin/sh
# xdjaz-usb.sh -- USB sticks for the AZ player while it runs, as root: the
# deck's udev rules and device-mount.sh, for a Pi.
#
#   xdjaz-usb.sh STATE-DIR      (run-probe.sh starts it; ends with the player)
#
# A stick that comes is mounted read-only at $MEDIA/<partition>, the player's
# /media/usb (run-probe.sh binds $MEDIA there; $MEDIA is a shared mount, so
# mounts made here later show up in the player's view too). The player is told
# the way the deck's kernel and udev rules tell it, lines in its
# /proc/udev_usb1 or _usb2 (the first free of the AZ's two USB ports):
#   connect,                                   a stick came (the port)
#   mount /media/usb/sda1 exfat protect:0      once mounted
#   umount /media/usb/sda1                     once the stick is gone
#   disconnect,                                (the port)
# Without the port events the player keeps a port whose stick went while a
# deck had a track from it as busy, and does not take the stick back.
# azshim hands those lines over one per read (see azshim.c).
#
# The player sees the stick read-only, but for one folder: PIONEER/rekordbox
# (where SQLite keeps its -wal/-shm next to exportLibrary.db, ~10 MB) is a
# writable copy in RAM bound over it, dropped when the stick goes, and
# "ALPHATHETA REC" (MASTER REC's WAVs) is the stick's own, writable. For that
# the stick is mounted read-write at $COPY/rw-<partition> and bound read-only
# at $MEDIA/<partition>; one that cannot be written (or mounted read-write)
# is mounted read-only, without REC.
REC_DIR="ALPHATHETA REC"
set -u
STATE=${1:?usage: xdjaz-usb.sh STATE-DIR}
MEDIA=/run/xdjaz-media
COPY=/run/xdjaz-usbcopy
UDEV=$STATE/tmp/udev
OWNER=$(stat -c %u:%g "$STATE")

log() { echo "xdjaz-usb: $*"; }

# Append one event line for the player (azshim takes them under the same lock).
event() {
    flock "$UDEV/udev_$1" sh -c 'printf "%s\n" "$2" >> "$1"' sh "$UDEV/udev_$1" "$2"
    log "udev_$1: $2"
}

# Filesystems on USB disks, as "NAME FSTYPE".
sticks() {
    for disk in $(lsblk -dnro NAME,TRAN | awk '$2 == "usb" {print $1}'); do
        lsblk -nro NAME,FSTYPE "/dev/$disk" | awk 'NF == 2'
    done
}

attach() {    # name fstype
    slot=""
    for s in usb1 usb2; do [ -e "$COPY/$s" ] || { slot=$s; break; }; done
    [ -n "$slot" ] || { log "$1: both USB ports in use"; return; }
    mp=$MEDIA/$1
    rw=$COPY/rw-$1
    mkdir -p "$mp" "$rw"
    own=""
    case $2 in vfat|exfat) own=",uid=${OWNER%:*},gid=${OWNER#*:}" ;; esac
    if mount -o rw,noatime$own "/dev/$1" "$rw" &&
       mount --bind "$rw" "$mp" && mount -o remount,bind,ro "$mp"; then
        mkdir -p "$rw/$REC_DIR" && chown "$OWNER" "$rw/$REC_DIR" 2>/dev/null
        mount --bind "$rw/$REC_DIR" "$mp/$REC_DIR" || log "$1: no $REC_DIR"
    else
        mountpoint -q "$mp" && umount "$mp"
        mountpoint -q "$rw" && umount "$rw"
        rmdir "$rw"
        if ! mount -o ro,noatime "/dev/$1" "$mp"; then
            log "$1: cannot mount ($2)"
            rmdir "$mp"
            return
        fi
        log "$1: read-only, no MASTER REC"
    fi
    if [ -d "$mp/PIONEER/rekordbox" ]; then
        rm -rf "$COPY/$1"
        cp -a "$mp/PIONEER/rekordbox" "$COPY/$1" && chown -R "$OWNER" "$COPY/$1" &&
            mount --bind "$COPY/$1" "$mp/PIONEER/rekordbox"
    fi
    echo "$1" > "$COPY/$slot"
    event "$slot" "connect,"
    event "$slot" "mount /media/usb/$1 $2 protect:0"
}

detach() {    # name
    for s in usb1 usb2; do
        [ "$(cat "$COPY/$s" 2>/dev/null)" = "$1" ] && { event "$s" "umount /media/usb/$1"; event "$s" "disconnect,"; rm -f "$COPY/$s"; }
    done
    mp=$MEDIA/$1
    mountpoint -q "$mp/PIONEER/rekordbox" && umount -l "$mp/PIONEER/rekordbox"
    mountpoint -q "$mp/$REC_DIR" && umount -l "$mp/$REC_DIR"
    mountpoint -q "$mp" && umount -l "$mp"
    rmdir "$mp" 2>/dev/null
    rw=$COPY/rw-$1
    mountpoint -q "$rw" && umount -l "$rw"
    rmdir "$rw" 2>/dev/null
    rm -rf "$COPY/$1"
}

# SIGHUP: a restarted player knows no stick yet; announce the attached ones
# again (run-probe.sh, when the player wakes up with the controller).
announce() {
    for s in usb1 usb2; do
        [ -e "$COPY/$s" ] || continue
        name=$(cat "$COPY/$s")
        fs=$(lsblk -nro FSTYPE "/dev/$name" 2>/dev/null)
        event "$s" "connect,"
        event "$s" "mount /media/usb/$name $fs protect:0"
    done
}
trap announce HUP

cleanup() {
    for s in usb1 usb2; do
        [ -e "$COPY/$s" ] && detach "$(cat "$COPY/$s")"
    done
    exit 0
}
trap cleanup INT TERM

mkdir -p "$MEDIA" "$COPY"
if ! mountpoint -q "$MEDIA"; then
    mount -t tmpfs -o mode=755,size=64k xdjaz-media "$MEDIA" && mount --make-shared "$MEDIA"
fi
rm -f "$COPY/usb1" "$COPY/usb2"
known=""
while :; do
    now=$(sticks)
    for name in $(echo "$known" | awk '{print $1}'); do
        echo "$now" | grep -q "^$name " || detach "$name"
    done
    echo "$now" | while read -r name fs; do
        [ -n "$name" ] || continue
        echo "$known" | grep -q "^$name " || attach "$name" "$fs"
    done
    known=$now
    sleep 1 &
    wait $!
done
