#!/bin/sh
# xdjaz-prep.sh start|stop -- the host state the AZ player needs, as root.
# Run by xdjaz.service (ExecStartPre=+ / ExecStopPost=+) around run-probe.sh.
#
# start  waits for the controller, loads the loopback sound card the player
#        plays into (clocked by the controller, see mixer/azmix.c), and lets go
#        of rekordbox's stick mounts (xdjaz-usb.sh mounts the sticks).
# stop   unloads the card and drops any stick mount left behind, so rekordbox
#        and Engine DJ find the sticks and the sound cards as they left them.
set -u
MEDIA=/run/xdjaz-media
# The controller: AZ_CARD, else the DDJ-GRV6 if it is there, else the DDJ-400, else
# the DDJ-FLX4 (the bridge and the mixer choose the same way).
CARDS=${AZ_CARD:-DDJGRV6 DDJ400 DDJFLX4}
CARD=

log() { echo "xdjaz-prep: $*"; }

case "${1:-}" in
start)
    for i in $(seq 20); do
        for c in $CARDS; do [ -e "/proc/asound/$c" ] && { CARD=$c; break 2; }; done
        sleep 0.5
    done
    [ -n "$CARD" ] || { log "no controller ($CARDS)"; exit 1; }
    log "controller $CARD"
    # two substreams: the player's audio out (,x,0) and its MASTER REC input
    # (capture ,0,1, which azmix feeds), see azmix.c
    ALOOP="index=7 id=ALPHATHETAD810 pcm_substreams=2 timer_source=hw:$CARD,0"
    # A loopback loaded by hand without the controller's clock (or from before
    # the REC input) is reloaded.
    if [ -e /sys/module/snd_aloop ] &&
       { ! grep -q "^hw:$CARD,0" /sys/module/snd_aloop/parameters/timer_source 2>/dev/null ||
         ! grep -q "^2," /sys/module/snd_aloop/parameters/pcm_substreams; }; then
        modprobe -r snd-aloop || { log "snd-aloop in use with the wrong clock"; exit 1; }
    fi
    [ -e /sys/module/snd_aloop ] || modprobe snd-aloop $ALOOP || exit 1
    # rekordbox's own stick mounts outlive it (see the launcher's
    # release-rbp-media.sh); the sticks are ours now (xdjaz-usb.sh)
    [ -x /opt/rblaunch/release-rbp-media.sh ] && sh /opt/rblaunch/release-rbp-media.sh
    exit 0 ;;
stop)
    # whatever xdjaz-usb.sh left mounted if it was killed
    for m in $(findmnt -rno TARGET | grep "^$MEDIA/" | sort -r); do umount -l "$m"; done
    mountpoint -q "$MEDIA" && umount -l "$MEDIA"
    # the sticks' read-write mounts live in there (xdjaz-usb.sh): off first,
    # and never delete across a mount
    for m in $(findmnt -rno TARGET | grep "^/run/xdjaz-usbcopy/" | sort -r); do umount -l "$m"; done
    rm -rf --one-file-system /run/xdjaz-usbcopy
    rm -rf /run/xdjaz          # the mixer frame and meters (run-probe.sh)
    # the mixer and the player may still be closing the card
    for i in $(seq 10); do
        [ -e /sys/module/snd_aloop ] || break
        modprobe -r snd-aloop 2>/dev/null || sleep 0.5
    done
    exit 0 ;;
*)
    echo "usage: xdjaz-prep.sh start|stop" >&2; exit 2 ;;
esac
