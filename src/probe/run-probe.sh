#!/bin/bash
# First probe: start the stock EP147 under bwrap with the AZ rootfs as /,
# on a private Xvfb display. Touches nothing of rbp / Engine: the player gets a
# minimal /dev (no snd/input/spidev: the first probe had the host /dev and the
# Pi locked up) and no network.
#   run-probe.sh [seconds]   (default 40; 0 = until the player exits, no
#                            screenshots: how xdjaz.service runs it)
#   XORG=1 run-probe.sh ...  on the real panel (host Xorg :0 on card1) instead
#                            of Xvfb; the boot menu must be stopped first.
set -u
ROOT=/opt/xdjaz/rootfs
STATE=/opt/xdjaz/state          # writable dirs the player expects
DISP=:7
[ "${XORG:-0}" = 1 ] && DISP=:0
T=${1:-40}

mkdir -p "$STATE"/{tmp,home-root,var}
# EP147 forks its library daemon (edb_streamd): pid file in /run (a tmpfs below,
# as on the deck), socket in /tmp. A stale socket from a killed run is dropped.
rm -f "$STATE/tmp/edb_daemon.sock"
# Sound: only card 7, a snd-aloop named like the AZ's own interface
# (modprobe snd-aloop index=7 id=ALPHATHETAD810 pcm_substreams=1; asound.conf
# wants hw:ALPHATHETAD810,0 x8 + ,1 x2), and the shared sequencer/timer;
# never the controller's own card.
SNDBIND=""
for f in /dev/snd/seq /dev/snd/timer /dev/snd/controlC7 /dev/snd/pcmC7D*; do
  [ -e "$f" ] && SNDBIND="$SNDBIND --dev-bind $f $f"
done
# /proc/udev_* stand-ins (azshim.so redirects /proc/udev_ -> /tmp/udev/udev_)
mkdir -p "$STATE/tmp/udev"
for n in usb1 usb2 sd1 nfs usbctn1 usbg1; do : > "$STATE/tmp/udev/udev_$n"; done
# What the deck's wlan-monitor.sh leaves when it finds no Wi-Fi module. The
# player waits up to 20 s for the country code at start.
printf XX > "$STATE/tmp/ccode"
printf 00:00:00:00:00:00 > "$STATE/tmp/mlan0Addr"
# USB sticks, inserted and pulled while the player runs: xdjaz-usb.sh (root)
# mounts them read-only under /run/xdjaz-media, the player's /media/usb, and
# tells the player through udev_usb1/_usb2 (emptied just above).
USBBIND=""
if [ -x /opt/xdjaz/xdjaz-usb.sh ]; then
  pgrep -x xdjaz-usb.sh >/dev/null ||
    sudo setsid /opt/xdjaz/xdjaz-usb.sh "$STATE" > /opt/xdjaz/xdjaz-usb.log 2>&1 < /dev/null &
  for i in $(seq 20); do mountpoint -q /run/xdjaz-media && break; sleep 0.1; done
  USBBIND="--bind /run/xdjaz-media /media/usb"
fi
# AZ-MODS (github.com/Kyle-Hosman/xdj-az-mods, GPL-3.0): its release azmod.so and
# azstem.so, put in /opt/xdjaz/azmod by hand, preloaded ahead of azshim as the deck's
# loader does (azmod, then azstem); their bus interposers then reach azshim's.
# AZMODS=0 runs the stock player. Art patches and settings: the rootfs's
# /home/root/settings (azmod-pngpatch/, azmod.conf).
PRELOAD=/usr/lib/azshim/azshim.so
MODBIND=""
if [ "${AZMODS:-1}" != 0 ] && [ -r /opt/xdjaz/azmod/azmod.so ]; then
  MODBIND="--ro-bind /opt/xdjaz/azmod /usr/lib/azmod"
  PRELOAD=/usr/lib/azmod/azmod.so
  [ -r /opt/xdjaz/azmod/azstem.so ] && PRELOAD=$PRELOAD:/usr/lib/azmod/azstem.so
  PRELOAD=$PRELOAD:/usr/lib/azshim/azshim.so
  # azmod tells the SOURCE screen by the title's pixels where a full-screen AZ UI
  # (1280x800 scaled to the screen) would have it; ours is smaller and off to the
  # side, so it never matched and the Mod Menu shut at once. Its test switch skips
  # that check (the menu still closes on the screen buttons and source picks).
  MODBIND="$MODBIND --setenv AZMOD_FORCE_SOURCE 1"
fi

# OneLibrary key for azshim's sqlite3_key hook (user-supplied, kept outside the tree)
KEYBIND=""
[ -r /opt/xdjaz/keys/onelibrary.key ] && KEYBIND="--ro-bind /opt/xdjaz/keys/onelibrary.key /etc/azshim/onelibrary.key"

if [ "${XORG:-0}" = 1 ]; then
  # the panel's touch controller glitches; X gets it through the aztouch filter
  [ -x /opt/xdjaz/touch/aztouch ] && ! pgrep -x aztouch >/dev/null &&
    sudo setsid /opt/xdjaz/touch/aztouch > /opt/xdjaz/touch/aztouch.log 2>&1 < /dev/null &
  sudo Xorg $DISP vt${XVT:-1} -keeptty -novtswitch -nolisten tcp -nocursor -config /opt/xdjaz/xorg-az.conf \
    -logfile /opt/xdjaz/xorg.log >/dev/null 2>&1 &
  for i in $(seq 10); do DISPLAY=$DISP xrandr >/dev/null 2>&1 && break; sleep 1; done
  OUT=$(DISPLAY=$DISP xrandr | awk '/ connected/{print $1; exit}')
  # The picture comes out the right way up as is; only the touchscreen is
  # turned 180 degrees (CalibrationMatrix in xorg-az.conf).
  # The panel is 1024x600; how the 1280x800 UI gets onto it, PANEL=
  #   fit      see below
  #   stretch  the player's JUCE draws at 0.8 (1024x640) itself,
  #            told so by our dconf stand-in (probe/dconf), on a 1024x640
  #            screen; X then fits the height to 600 (bilinear): full panel,
  #            6% squashed, text drawn at the larger size
  #   fit      like stretch, but the panel's own scaler fits the height: once
  #            the window is up X sends a 1024x640 mode (the native timings
  #            with 40 more lines; not taken right after X starts), so X does
  #            no scaling at all (needs a panel that takes it)
  #   juce     (default) JUCE draws at 0.75 (960x600) in the native mode: no distortion,
  #            bars left and right
  #   panel    1280x800 to the panel and its own scaler
  PANEL=${PANEL:-juce}
  case "$PANEL" in
  stretch|fit) DISPLAY=$DISP xrandr --fb 1024x640 --output "$OUT" --mode 1024x600 2>&1 || PANEL=panel ;;
  juce)    # X normally starts in it already (xorg-az.conf); a second mode set
           # once the player is up blanks the panel for a second or two
           DISPLAY=$DISP xrandr | grep -q "^ *1024x600_${REFRESH:-59.24} .*\*" ||
           DISPLAY=$DISP xrandr --output "$OUT" --mode 1024x600 2>&1 || PANEL=panel ;;
  esac
  [ "$PANEL" = panel ] && DISPLAY=$DISP xrandr --output "$OUT" --mode 1280x800 2>&1
  echo "xorg: output $OUT, PANEL=$PANEL"
  # A compositor gives the screen double buffering with vsync (the player
  # draws plain X images; Xorg 21.1 modesetting has no TearFree). With the
  # player fed real vsync times (azvsync below) it already paints right after
  # each vblank, and without a compositor there is neither a tear line nor its
  # extra pacing step: the smoothest result, so no compositor by default.
  # picom's GL backend wants OpenGL 3.3; the Pi 5's V3D reports 3.1, and Mesa's
  # version override is enough for picom.
  # COMP=0 (default), glx, or xrender (no GL, vsync through Present).
  # AZ-MODS' 60 FPS tears without one: the X server lands the player's lane
  # images in a burst at a phase of its own (measured 2026-11-06; nothing on
  # the player's side moves it), so with the mods the default is glx. Without
  # a compositor azshim's blit guard is on (see azshim.c).
  [ -n "$MODBIND" ] && COMP=${COMP:-glx}
  [ "${COMP:-0}" = 0 ] && GUARD="--setenv AZ_BLITGUARD ${AZ_BLITGUARD:-1} --setenv AZ_VSYNC_OFFSET_US ${VSYNC_OFFSET_US:-11000} --setenv AZ_SCAN_UP ${AZ_SCAN_UP:-1} --setenv AZ_BLIT_US ${AZ_BLIT_US:-1500} --setenv AZ_SCAN_SHIFT_US ${AZ_SCAN_SHIFT_US:-0}"
  case "${COMP:-0}" in
  glx) DISPLAY=$DISP MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 \
         setsid picom --backend glx --vsync > /opt/xdjaz/picom.log 2>&1 < /dev/null & ;;
  xrender) DISPLAY=$DISP setsid picom --backend xrender --vsync > /opt/xdjaz/picom.log 2>&1 < /dev/null & ;;
  esac
else
  Xvfb $DISP -screen 0 1920x1200x24 -nolisten tcp >/opt/xdjaz/xvfb.log 2>&1 &
  sleep 1
fi

# Mixer (before the player: the loopback runs on the controller's clock) and
# controller bridge (UCOM frames on $STATE/tmp/azbridge.sock), if built.
[ -x /opt/xdjaz/mixer/azmix ] && ! pgrep -x azmix >/dev/null &&
  sudo setsid /opt/xdjaz/mixer/azmix -p > /opt/xdjaz/mixer/azmix.log 2>&1 < /dev/null &
[ -x /opt/xdjaz/bridge/azbridge ] && ! pgrep -x azbridge >/dev/null &&
  setsid /opt/xdjaz/bridge/azbridge -v ${AZBRIDGE_OPTS:-} -s "$STATE/tmp/azbridge.sock" \
    > /opt/xdjaz/bridge/azbridge.log 2>&1 < /dev/null &
# The panel's vsync times for the player's repaint timing (it reads the
# deck's /sys/module/rockchipdrm/parameters/vsync_time; see vsync/azvsync.c).
# Reported 11 ms into the frame, so the player repaints after the scanout has
# passed the waveforms (no tearing there without a compositor). At 60 frames a
# second (AZ-MODS) neither that nor writing it later (-w, VSYNC_DELAY_US) holds
# the repaint in place: it drifts through the frame, so the mods get the
# compositor (below).
VSYNC=/dev/shm/az-vsync_time
[ -e $VSYNC ] || echo 0 > $VSYNC
[ "${XORG:-0}" = 1 ] && [ -x /opt/xdjaz/vsync/azvsync ] && ! pgrep -x azvsync >/dev/null &&
  setsid /opt/xdjaz/vsync/azvsync -f $VSYNC -o "${VSYNC_OFFSET_US:-11000}" -w "${VSYNC_DELAY_US:-0}" \
    > /opt/xdjaz/vsync/azvsync.log 2>&1 < /dev/null &
sleep 1

${STRACE:-} bwrap \
  --bind "$ROOT" / \
  --dev /dev \
  $SNDBIND \
  --proc /proc \
  --tmpfs /run \
  --ro-bind /sys /sys \
  --tmpfs /sys/module --dir /sys/module/rockchipdrm/parameters \
  --ro-bind $VSYNC /sys/module/rockchipdrm/parameters/vsync_time \
  --bind "$STATE/tmp" /tmp \
  --ro-bind /tmp/.X11-unix /tmp/.X11-unix \
  --ro-bind /opt/xdjaz/shims /usr/lib/azshim \
  --ro-bind /opt/xdjaz/dconf /usr/bin/dconf \
  --setenv AZ_SCALE8 "$(case "${PANEL:-}" in stretch|fit) echo 6.4;; juce) echo 6;; *) echo 8;; esac)" \
  --setenv LD_PRELOAD $PRELOAD \
  $USBBIND $KEYBIND $MODBIND ${GUARD:-} \
  --setenv DISPLAY $DISP \
  --setenv HOME /home/root \
  --setenv PATH /usr/bin:/bin:/usr/sbin:/sbin \
  --chdir /home/root/pdj \
  --unshare-user --uid 0 --gid 0 \
  --unshare-pid --unshare-net --unshare-ipc --die-with-parent \
  ./EP147 > /opt/xdjaz/ep147.log 2>&1 &
BPID=$!
# Announce the stick the way the deck's device-mount.sh does, once the player polls.
# Once the player's window is up: the panel gets a mode at the refresh the
# player is built for (its repaint timer runs on a fixed 59.24 Hz period;
# the panel's own 59.80 judders), PANEL=juce centres the window and
# PANEL=stretch fits the 640 high screen into the panel's 600. Not before
# (new modes are not taken right after X starts), and no `xrandr` after a
# stretch: this Xorg crashes (ProcRRGetCrtcTransform) when a client asks for
# the CRTC transform while a filtered one is set. REFRESH= overrides the rate.
REFRESH=${REFRESH:-59.24}
# setmode WIDTHxHEIGHT VACTIVE VTOTAL CLOCK: the panel's native horizontal
# timings with the given lines; the line length is stretched so that CLOCK
# (MHz) gives $REFRESH. The clock must be one the HDMI block can make: the
# panel's own 50.25 is, 49.76 is not (it came out at 58.33 Hz).
setmode() {
  local name="$1_$REFRESH" ht
  ht=$(awk -v c="$4" -v vt="$3" -v r="$REFRESH" 'BEGIN { printf "%d", c * 1e6 / (vt * r) + 0.5 }')
  DISPLAY=$DISP xrandr --newmode "$name" "$4" 1024 1068 1156 "$ht" \
    "$2" $(($2 + 3)) $(($2 + 9)) "$3" +hsync -vsync
  DISPLAY=$DISP xrandr --addmode "$OUT" "$name" && DISPLAY=$DISP xrandr --output "$OUT" --mode "$name"
}
[ "${XORG:-0}" = 1 ] && (for i in $(seq 30); do
  W=$(DISPLAY=$DISP xdotool search --name '^EP147$' 2>/dev/null | head -1)
  if [ -n "$W" ]; then
    sleep 2
    case "$PANEL" in
    juce)
      DISPLAY=$DISP xrandr | grep -q "^ *1024x600_$REFRESH .*\*" ||
        setmode 1024x600 600 625 50.25
      WW=$(DISPLAY=$DISP xdotool getwindowgeometry "$W" | awk '/Geometry/{split($2, g, "x"); print g[1]}')
      DISPLAY=$DISP xdotool windowmove "$W" $(( (1024 - ${WW:-1024}) / 2 )) 0 ;;
    fit)
      setmode 1024x640 640 665 52.946
      DISPLAY=$DISP xdotool windowmove "$W" 0 0 ;;
    stretch)
      DISPLAY=$DISP xdotool windowmove "$W" 0 0
      DISPLAY=$DISP xrandr --fb 1024x640 --output "$OUT" --filter bilinear \
        --transform 1,0,0,0,1.066667,0,0,0,1 ;;
    esac
    break
  fi
  sleep 1
done) &
i=0
while [ "$T" = 0 ] || [ $i -lt "$T" ]; do
  kill -0 $BPID 2>/dev/null || break
  i=$((i + 1))
  # keep the last frame the player drew, even if it dies later
  [ "$T" != 0 ] && [ $((i % 5)) = 0 ] && DISPLAY=$DISP xwd -root -silent > /opt/xdjaz/shot.xwd 2>/dev/null
  sleep 1
done
if kill -0 $BPID 2>/dev/null; then echo "still running after ${T}s"; kill -INT $BPID; sleep 3; kill -KILL $BPID 2>/dev/null; fi
wait $BPID; RC=$?; echo "exit=$RC"
if [ "${XORG:-0}" = 1 ]; then
  [ "${KEEPX:-0}" = 1 ] || { pkill -x picom; pkill -x azvsync; sudo pkill -x xdjaz-usb.sh; sudo pkill -f "Xorg $DISP vt"; sudo pkill -x aztouch; }
else
  pkill -f "Xvfb $DISP"
fi
exit $RC
