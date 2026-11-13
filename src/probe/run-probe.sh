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
# STRIP=1 (default; the panel, PANEL=juce): the AZ's browse buttons on screen,
# right of the player's window (strip/azstrip.py)
strip_on() { [ "${XORG:-0}" = 1 ] && [ "${PANEL:-juce}" = juce ] && [ "${STRIP:-0}" = 1 ] && [ -f /opt/xdjaz/strip/azstrip.py ]; }
# DROP=1 (default when STRIP is off): the same buttons as a drop-down bar from a
# tab at the top of the screen, for any resolution (strip/azdrop.py)
drop_on() { [ "${XORG:-0}" = 1 ] && [ "${DROP:-1}" = 1 ] && ! strip_on && [ -f /opt/xdjaz/strip/azdrop.py ]; }
# keep the last run's logs: a player that dies and is restarted by the service
# otherwise takes the reason with it
cp -f /opt/xdjaz/ep147.log /opt/xdjaz/ep147.prev.log 2>/dev/null
cp -f /opt/xdjaz/bridge/azbridge.log /opt/xdjaz/bridge/azbridge.prev.log 2>/dev/null
cp -f /opt/xdjaz/mixer/azmix.log /opt/xdjaz/mixer/azmix.prev.log 2>/dev/null
cp -f /opt/xdjaz/xorg.log /opt/xdjaz/xorg.prev.log 2>/dev/null
# Files the bridge, the mixer and azvsync share (mixer frame, meters, vsync
# time). Not /dev/shm: logind (RemoveIPC) empties the user's files there when
# their last session closes, and a mixer restarted later (wake-up) would find
# no controls. xdjaz-prep.sh stop removes the directory.
sudo install -d -o "$(id -u)" -g "$(id -g)" /run/xdjaz

mkdir -p "$STATE"/{tmp,home-root,var}
# EP147 forks its library daemon (edb_streamd): pid file in /run (a tmpfs below,
# as on the deck), socket in /tmp. A stale socket from a killed run is dropped.
rm -f "$STATE/tmp/edb_daemon.sock"
# Sound: only card 7, a snd-aloop named like the AZ's own interface
# (modprobe snd-aloop index=7 id=ALPHATHETAD810 pcm_substreams=2; asound.conf
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
# The information screen's "Version No." is `fw_printenv -n release`: U-Boot's environment,
# which the AZ keeps in its eMMC (fw_env.config: /dev/mmcblk1). The Pi has no such flash and the
# rootfs has no /var/lock (fw_printenv's lock file), so the lookup failed. Here the environment is
# a plain file, made at each start: release = /opt/xdjaz/VERSION (src/deploy/VERSION), or
# AZ_RELEASE. The Serial No. is `fw_printenv -n serial_number`, in the same file.
mkdir -p "$ROOT/var/lock" "$ROOT/home/root/settings"
[ -e "$ROOT/etc/fw_env.config.stock" ] || cp "$ROOT/etc/fw_env.config" "$ROOT/etc/fw_env.config.stock"
printf '/home/root/settings/ubootenv.bin\t0x0\t0x8000\n' > "$ROOT/etc/fw_env.config"
# serial_number = the board's own serial (the Serial line of /proc/cpuinfo), or AZ_SERIAL.
SERIAL=${AZ_SERIAL:-$(awk '/^Serial/ { print $NF; exit }' /proc/cpuinfo 2>/dev/null)}
python3 -I /opt/xdjaz/deploy/mkubootenv.py "$ROOT/home/root/settings/ubootenv.bin" \
  "release=${AZ_RELEASE:-$(head -n1 /opt/xdjaz/VERSION 2>/dev/null || echo 1.30)}" \
  "serial_number=${SERIAL:-ddj400xdjaz}"
[ -n "${AZ_OPEN_LOG:-}" ] && : > "$STATE/tmp/open.log"	# AZ_OPEN_LOG=1: azshim lists every open
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
  # With the drop-down/fixed bar (azdrop) the SOURCE test follows the view that is really
  # open: azdrop makes the flag file while it is (azshim.c, getenv). AZMOD_ALWAYS=1, or no
  # bar, keeps it forced on, the mods' entry then shows on every screen.
  if drop_on && [ "${AZMOD_ALWAYS:-0}" != 1 ]; then
    rm -f "$STATE/tmp/azmod-source"
    MODBIND="$MODBIND --setenv AZMOD_SOURCE_FLAG /tmp/azmod-source"
  else
    MODBIND="$MODBIND --setenv AZMOD_FORCE_SOURCE 1"
  fi
fi

# Bluetooth (AZ_BT=1, default off): the player carries its own Bluetooth stack and opens
# /dev/ttyS0 to speak HCI to the chip. bt/azbt.py (root) takes the Pi's hci0 in user-channel mode
# (the kernel's Bluetooth, bluetoothd with it, lets go of it) and offers it as a pty, which is
# bound into the sandbox as /dev/ttyS0. Its log: /opt/xdjaz/bt/azbt.log.
BTBIND=""
if [ "${AZ_BT:-0}" = 1 ] && [ -f /opt/xdjaz/bt/azbt.py ]; then
  if ! pgrep -f "bt/azbt[.]py" >/dev/null; then
    rm -f "$STATE/ttyBT.path"
    sudo STATE="$STATE" setsid python3 /opt/xdjaz/bt/azbt.py ${AZ_BT_HCI:-0} > /opt/xdjaz/bt/azbt.log 2>&1 < /dev/null &
  fi
  for i in $(seq 50); do [ -s "$STATE/ttyBT.path" ] && break; sleep 0.1; done
  [ -s "$STATE/ttyBT.path" ] && BTBIND="--dev-bind $(cat "$STATE/ttyBT.path") /dev/ttyS0"
fi

# OneLibrary key for azshim's sqlite3_key hook (user-supplied, kept outside the tree)
KEYBIND=""
[ -r /opt/xdjaz/keys/onelibrary.key ] && KEYBIND="--ro-bind /opt/xdjaz/keys/onelibrary.key /etc/azshim/onelibrary.key"

# Network (AZ_NET=1; default: none): the player shares the host network
# namespace as a deck with internet does -- its own /etc/hosts and CA store.
# Only DNS is the host's: the rootfs resolv.conf is a link to
# /run/resolv.conf, and /run is a tmpfs here. Its internet check wants eth0 or
# mlan0 UP: a dummy mlan0 (the deck's Wi-Fi name), and the link state files
# its link monitor reads ("up"/"up-pend"/"down", no newline).
NETNS="--unshare-net"
NETBIND=""
if [ "${AZ_NET:-0}" = 1 ]; then
  sudo ip link show mlan0 >/dev/null 2>&1 || sudo ip link add mlan0 type dummy
  sudo ip link set mlan0 up
  printf up   > "$STATE/net_mlan0_state.dat"
  printf down > "$STATE/net_eth0_state.dat"
  NETNS=""
  NETBIND="--ro-bind /etc/resolv.conf /run/resolv.conf \
           --ro-bind $STATE/net_mlan0_state.dat /var/net_mlan0_state.dat \
           --ro-bind $STATE/net_eth0_state.dat /var/net_eth0_state.dat"
  # Wi-Fi from the player's own screen (AZ_WIFI=1, default): azshim sends its
  # wpa_cli/iw/ifconfig on mlan0 to the host broker (wifi/azwifi.py, root,
  # NetworkManager or the host's wpa_supplicant) through this socket in the
  # player's /tmp. No broker running = the commands run in the sandbox as
  # before. The player's Wi-Fi MAC is the host's first Wi-Fi interface's
  # (AZWIFI_IFACE to pick another).
  if [ "${AZ_WIFI:-1}" = 1 ]; then
    NETBIND="$NETBIND --setenv AZ_WIFI_SOCK /tmp/azwifi.sock"
    WIF=${AZWIFI_IFACE:-}
    if [ -z "$WIF" ]; then
      for d in /sys/class/net/*/wireless; do
        [ -d "$d" ] && { WIF=$(basename "$(dirname "$d")"); break; }
      done
    fi
    [ -n "$WIF" ] && [ -r "/sys/class/net/$WIF/address" ] &&
      tr -d '\n' < "/sys/class/net/$WIF/address" > "$STATE/tmp/mlan0Addr"
  fi
fi

if [ "${XORG:-0}" = 1 ]; then
  # the panel's touch controller glitches; X gets it through the aztouch filter.
  # With the UI at 1:1 in the middle (PANEL=juce, the window at x 31..992),
  # it also widens the top bar's small icons at the panel's edge: the REC
  # timer (opens MIXER SETTINGS) and the gear / info (aztouch.c).
  TOUCHZONES=""
  # STRIP=1 (PANEL=juce): the window goes to x 0 instead and the free strip
  # on the right gets the AZ's browse buttons (strip/azstrip.py; aztouch
  # presses them through azbridge's FIFO), the icons 31 px to the left.
  if strip_on; then
    TOUCHZONES="-z 814,0,904,60,854,24 -z 909,0,960,60,935,24 $(python3 /opt/xdjaz/strip/azstrip.py --aztouch-args)"
  elif [ "${PANEL:-juce}" = juce ]; then
    TOUCHZONES="-z 845,0,935,60,885,24 -z 940,0,1023,60,966,24"
  fi
  # BEAT FX on the screen: zones in /opt/xdjaz/bfx.zones (aztouch.c, load_zones) press
  # the beat FX through azbridge's FIFO. AZ_TOUCH_DEBUG=1 logs where each touch lands
  # (aztouch.log), to measure the zones.
  BFXTOUCH=""
  [ -f /opt/xdjaz/bfx.zones ] && BFXTOUCH="-f $STATE/azbridge.in -c /opt/xdjaz/bfx.zones"
  [ "${AZ_TOUCH_DEBUG:-0}" = 1 ] && BFXTOUCH="$BFXTOUCH -d"
  [ -x /opt/xdjaz/touch/aztouch ] && ! pgrep -x aztouch >/dev/null &&
    sudo setsid /opt/xdjaz/touch/aztouch -S ${AZ_SCREEN:-1920x1080} $BFXTOUCH \
      > /opt/xdjaz/touch/aztouch.log 2>&1 < /dev/null &
  # AZ_SCREEN=1280x800 / 1024x600 / 1024x640: a 16:9 screen lists none of them.
  # 1024x640 with AZ_SCALE8=6.4 fills the 7" panel (it squeezes the 640 lines into its 600).
  # (a 16:9 screen lists neither of the first two.) The X config gets the mode
  # as a Modeline + PreferredMode (as the original kit does for its panel), so X starts in it.
  # Making the mode with xrandr at start does not hold: --newmode succeeds and the next
  # xrandr call no longer finds it.
  XCONF=/opt/xdjaz/xorg-az.conf
  case "${AZ_SCREEN:-}" in
  1280x800) AZ_TIMING="83.46 1280 1344 1480 1680 800 803 809 831 -hsync +vsync" ;;
  1024x600) AZ_TIMING="49.00 1024 1072 1168 1312 600 603 613 624 -hsync +vsync" ;;
  1024x640) AZ_TIMING="46.75 1024 1072 1104 1184 640 643 649 659 +hsync -vsync" ;;
  *) AZ_TIMING="" ;;
  esac
  if [ -n "$AZ_TIMING" ]; then
    XCONF=/run/xdjaz/xorg-az.conf
    awk -v m="${AZ_SCREEN}_60" -v t="$AZ_TIMING" '{ print } /Identifier "panel"/ { printf "    Modeline \"%s\" %s\n    Option \"PreferredMode\" \"%s\"\n", m, t, m }' \
      /opt/xdjaz/xorg-az.conf > "$XCONF"
  fi
  sudo Xorg $DISP vt${XVT:-1} -keeptty -novtswitch -nolisten tcp -nocursor -config "$XCONF" \
    -logfile /opt/xdjaz/xorg.log >/dev/null 2>&1 &
  for i in $(seq 10); do DISPLAY=$DISP xrandr >/dev/null 2>&1 && break; sleep 1; done
  OUT=$(DISPLAY=$DISP xrandr | awk '/ connected/{print $1; exit}')
  # AZ_SCREEN=1280x720: run the screen at a smaller mode (the compositor and the X
  # server move fewer pixels: on a Pi 4 the GPU is the limit with two decks). Pair it
  # with AZ_SCALE8=6 so the player (1280x800 at 1:1) fits.
  # A 16:9 screen lists neither 1280x800 nor 1024x600: add them (CVT timings) under a
  # name of their own ("..._60": a plain "1280x800" is already a name in X's mode pool).
  # One xrandr call each for --newmode, --addmode and --mode (xrandr reads what follows
  # --newmode's timing as its flags, so the three cannot be chained), tried again, each
  # result logged. A black screen: DISPLAY=:0 xrandr --output HDMI-1 --mode 1920x1080.
  az_screen() {
    local want=$1 timing n r1 r2 r3
    if DISPLAY=$DISP xrandr | grep -q "^ *$want "; then
      DISPLAY=$DISP xrandr --output "$OUT" --mode "$want"; return
    fi
    if DISPLAY=$DISP xrandr | grep -q "^ *${want}_60 "; then	# from the X config
      DISPLAY=$DISP xrandr --output "$OUT" --mode "${want}_60"; echo "az_screen: ${want}_60 from the X config"; return
    fi
    case "$want" in
    1280x800) timing="83.46 1280 1344 1480 1680 800 803 809 831 -hsync +vsync" ;;
    1024x600) timing="49.00 1024 1072 1168 1312 600 603 613 624 -hsync +vsync" ;;
    1024x640) timing="46.75 1024 1072 1104 1184 640 643 649 659 +hsync -vsync" ;;
    *) echo "az_screen: no timing for $want"; return ;;
    esac
    for n in 1 2 3 4 5 6 7 8; do
      r1=-
      DISPLAY=$DISP xrandr | grep -q "^ *${want}_60 " || { DISPLAY=$DISP xrandr --newmode "${want}_60" $timing; r1=$?; }
      DISPLAY=$DISP xrandr --addmode "$OUT" "${want}_60"; r2=$?
      DISPLAY=$DISP xrandr --output "$OUT" --mode "${want}_60"; r3=$?
      echo "az_screen: try $n ${want}_60 newmode=$r1 addmode=$r2 mode=$r3"
      [ "$r3" = 0 ] && return
      sleep 1
    done
    echo "az_screen: could not set $want"
  }
  [ -n "${AZ_SCREEN:-}" ] && az_screen "$AZ_SCREEN"
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
start_mix() {
  [ -x /opt/xdjaz/mixer/azmix ] && ! pgrep -x azmix >/dev/null &&
    sudo setsid /opt/xdjaz/mixer/azmix >> /opt/xdjaz/mixer/azmix.log 2>&1 < /dev/null &
}
: > /opt/xdjaz/mixer/azmix.log
start_mix
# the on-screen buttons (STRIP) press through its FIFO
BRIDGE_FIFO=""
{ strip_on || drop_on || [ -f /opt/xdjaz/bfx.zones ]; } && case " ${AZBRIDGE_OPTS:-} " in *" -i "*) ;; *) BRIDGE_FIFO="-i $STATE/azbridge.in" ;; esac
# ... and tells them the open view (its LEDs), to light its button
{ strip_on || drop_on; } && BRIDGE_FIFO="$BRIDGE_FIFO -L $STATE/azstrip.in"
[ -x /opt/xdjaz/bridge/azbridge ] && ! pgrep -x azbridge >/dev/null &&
  setsid /opt/xdjaz/bridge/azbridge -v ${AZBRIDGE_OPTS:-} $BRIDGE_FIFO -s "$STATE/tmp/azbridge.sock" \
    > /opt/xdjaz/bridge/azbridge.log 2>&1 < /dev/null &
# The panel's vsync times for the player's repaint timing (it reads the
# deck's /sys/module/rockchipdrm/parameters/vsync_time; see vsync/azvsync.c).
# Reported 11 ms into the frame, so the player repaints after the scanout has
# passed the waveforms (no tearing there without a compositor). At 60 frames a
# second (AZ-MODS) neither that nor writing it later (-w, VSYNC_DELAY_US) holds
# the repaint in place: it drifts through the frame, so the mods get the
# compositor (below).
VSYNC=/run/xdjaz/az-vsync_time
[ -e $VSYNC ] || echo 0 > $VSYNC
[ "${XORG:-0}" = 1 ] && [ -x /opt/xdjaz/vsync/azvsync ] && ! pgrep -x azvsync >/dev/null &&
  setsid /opt/xdjaz/vsync/azvsync -f $VSYNC -o "${VSYNC_OFFSET_US:-11000}" -w "${VSYNC_DELAY_US:-0}" \
    > /opt/xdjaz/vsync/azvsync.log 2>&1 < /dev/null &

# The player's UI thread (it paints everything, in software) gets cpu 3 alone
# and nice -10, everything else cpu 0-2: no frames late from preemption (215
# a minute at 1.5 GHz without it; pin/azpin.py). AZ_PIN=0: off; AZ_PIN_CPU.
# Measured on a Pi 5 only: on a Pi 4 (same four cores, a slower machine) it took two
# decks from 60 to 30 fps, so it is on by default on a Pi 5 and off elsewhere
# (AZ_PIN=1 turns it on anywhere).
AZ_PIN_DEFAULT=0
grep -q "Raspberry Pi 5" /proc/device-tree/model 2>/dev/null && AZ_PIN_DEFAULT=1
[ "${AZ_PIN:-$AZ_PIN_DEFAULT}" = 1 ] && [ -f /opt/xdjaz/pin/azpin.py ] && ! pgrep -f "pin/azpin[.]py" >/dev/null &&
  sudo setsid python3 /opt/xdjaz/pin/azpin.py -c "${AZ_PIN_CPU:-3}" \
    > /opt/xdjaz/pin/azpin.log 2>&1 < /dev/null &
sleep 1

# MASTER REC: the player records its audio input, the capture side of
# "outs" (first two of its ten channels), which azmix feeds (azmix.c). Its
# JUCE only offers that input if a capture on ALSA's "default" opens, and the
# rootfs' default (dsnoop on the loopback) cannot. The player gets its own
# asound.conf, the rootfs' plus a default whose capture is "outs".
ASOUND=$STATE/asound.conf
{ cat "$ROOT/etc/asound.conf"; cat <<'EOC'

# xdjaz (run-probe.sh): capture for the MASTER REC input probe
pcm.!default {
	type asym
	playback.pcm { type plug; slave.pcm "dmix:ALPHATHETAD810" }
	capture.pcm "outs"
}
EOC
} > "$ASOUND"

start_player() {
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
  --ro-bind "$ASOUND" /etc/asound.conf \
  --setenv AZ_SCALE8 "${AZ_SCALE8:-$(case "${PANEL:-}" in stretch|fit) echo 6.4;; juce) echo 6;; *) echo 8;; esac)}" \
  --setenv LD_PRELOAD $PRELOAD \
  ${AZ_OPEN_LOG:+--setenv AZ_OPEN_LOG /tmp/open.log} \
  $USBBIND $KEYBIND $MODBIND $NETBIND $BTBIND ${GUARD:-} \
  --setenv DISPLAY $DISP \
  --setenv HOME /home/root \
  --setenv PATH /usr/bin:/bin:/usr/sbin:/sbin \
  --chdir /home/root/pdj \
  --unshare-user --uid 0 --gid 0 \
  --unshare-pid $NETNS --unshare-ipc --die-with-parent \
  ./EP147 >> /opt/xdjaz/ep147.log 2>&1 &
BPID=$!
}
: > /opt/xdjaz/ep147.log
start_player
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
place_window() {
[ "${XORG:-0}" = 1 ] && (for i in $(seq 30); do
  W=$(DISPLAY=$DISP xdotool search --name '^EP147$' 2>/dev/null | head -1)
  if [ -n "$W" ]; then
    sleep 2
    case "$PANEL" in
    juce)
      DISPLAY=$DISP xrandr | grep -q "^ *1024x600_$REFRESH .*\*" ||
        setmode 1024x600 600 625 50.25
      WW=$(DISPLAY=$DISP xdotool getwindowgeometry "$W" | awk '/Geometry/{split($2, g, "x"); print g[1]}')
      if strip_on; then
        DISPLAY=$DISP xdotool windowmove "$W" 0 0
        pgrep -f "strip/azstrip[.]py" >/dev/null ||
          DISPLAY=$DISP setsid python3 /opt/xdjaz/strip/azstrip.py -x "${WW:-961}" \
            > /opt/xdjaz/strip/azstrip.log 2>&1 < /dev/null &
      else
        DISPLAY=$DISP xdotool windowmove "$W" $(( (1024 - ${WW:-1024}) / 2 )) 0
      fi ;;
    panel)	# the window keeps the place it was made at (centred on 1920x1080);
		# on a smaller screen that cuts it: centre it on the screen it is on
      set -- $(DISPLAY=$DISP xdotool getdisplaygeometry)
      WG=$(DISPLAY=$DISP xdotool getwindowgeometry "$W" | awk '/Geometry/{print $2}')
      PX=$(( ($1 - ${WG%x*}) / 2 )); PY=$(( ($2 - ${WG#*x}) / 2 ))
      # AZ_TOPBAR=H: a fixed bar of H pixels across the top (azdrop, DROP_BAR); the window goes under it
      [ "${AZ_TOPBAR:-0}" -gt 0 ] 2>/dev/null && PY=$AZ_TOPBAR
      DISPLAY=$DISP xdotool windowmove "$W" $(( PX < 0 ? 0 : PX )) $(( PY < 0 ? 0 : PY ))
      # AZ_FIT=1: the player draws small (AZ_SCALE8=5: 800x500, the cheap size) and X
      # scales it up to fill the screen: the framebuffer is the window, the output shows
      # it stretched (bilinear). Costs a little GPU; the player's own work is unchanged.
      if [ "${AZ_FIT:-0}" = 1 ]; then
        MODE=$(DISPLAY=$DISP xrandr | awk '/\*/{print $1; exit}')
        FW=${WG%x*}; FH=${WG#*x}
        DISPLAY=$DISP xdotool windowmove "$W" 0 0
        DISPLAY=$DISP xrandr --fb "${FW}x${FH}" --output "$OUT" --mode "$MODE" --filter bilinear \
          --transform "$(awk -v fw="$FW" -v fh="$FH" -v sw="$1" -v sh="$2" 'BEGIN{printf "%.6f,0,0,0,%.6f,0,0,0,1", fw/sw, fh/sh}')"
        echo "azfit: ${FW}x${FH} shown on $MODE"
      fi ;;
    fit)
      setmode 1024x640 640 665 52.946
      DISPLAY=$DISP xdotool windowmove "$W" 0 0 ;;
    stretch)
      DISPLAY=$DISP xdotool windowmove "$W" 0 0
      DISPLAY=$DISP xrandr --fb 1024x640 --output "$OUT" --filter bilinear \
        --transform 1,0,0,0,1.066667,0,0,0,1 ;;
    esac
    drop_on && ! pgrep -f "strip/azdrop[.]py" >/dev/null &&
      (cd /opt/xdjaz/strip && DISPLAY=$DISP DROP_BAR=${AZ_TOPBAR:-0} setsid python3 azdrop.py \
        > /opt/xdjaz/strip/azdrop.log 2>&1 < /dev/null &)
    break
  fi
  sleep 1
done) &
}
place_window

# The controller's sleep (the GRV6's Auto Standby) takes it off USB, and the
# loopback loses the clock it runs on (snd-aloop's timer source): the player
# would fail. Like a real AZ in standby it goes to sleep instead: stopped,
# the screen black (no menu), and when the controller comes back it starts
# again (tracks unloaded, as after the AZ's own standby). The sticks are
# announced again (xdjaz-usb.sh, on SIGHUP). AZ_SLEEP=0: the old way, the
# player fails and the menu comes back.
CTL_CARD=""
for c in ${AZ_CARD:-DDJGRV6 DDJ400 DDJFLX4}; do [ -e "/proc/asound/$c" ] && { CTL_CARD=$c; break; }; done
asleep() { [ "$T" = 0 ] && [ "${AZ_SLEEP:-1}" != 0 ] && [ -n "$CTL_CARD" ] && [ ! -e "/proc/asound/$CTL_CARD" ]; }
# azmix drives the loopback's clock through the controller's PCM: when it is gone
# the player's audio and the VU stop (kernel: snd_timer_start failed with -19).
# It is started again whenever it dies, except while the controller sleeps.
( while sleep 3; do
    asleep && continue
    pgrep -x azmix >/dev/null || start_mix
  done ) &
# Back from sleep: the controller's audio side can take a while after its USB
# comes back, and the player needs the mixer running (its clock) before it
# starts. A player that fails soon after waking is started again (WAKE_TRIES).
WAKE_TRIES=3
wake() {
  echo "controller $CTL_CARD back: the player wakes"
  local k
  for k in $(seq 15); do
    sleep 2
    asleep && return 1
    start_mix; sleep 2
    pgrep -x azmix >/dev/null && break
    echo "wake: the mixer has no output yet ($k)"
  done
  rm -f "$STATE/tmp/edb_daemon.sock"
  start_player; place_window
  ( sleep 5; sudo pkill -HUP -x xdjaz-usb.sh ) &
  WOKE_AT=$i
}
WOKE_AT=-1000; WAKES=0
i=0
while [ "$T" = 0 ] || [ $i -lt "$T" ]; do
  if asleep; then
    echo "controller $CTL_CARD gone: the player sleeps"
    kill -INT $BPID 2>/dev/null; sleep 3; kill -KILL $BPID 2>/dev/null; wait $BPID 2>/dev/null
    sudo pkill -x azmix
    pkill -f "strip/azstrip[.]py"; pkill -f "strip/azdrop[.]py"	# black screen while asleep; place_window brings it back
    while asleep; do sleep 1; done
    wake; WAKES=0
    continue
  fi
  if ! kill -0 $BPID 2>/dev/null; then
    # died soon after waking, with the controller there: once more
    if [ $((i - WOKE_AT)) -lt 60 ] && [ $WAKES -lt $WAKE_TRIES ] && ! asleep; then
      wait $BPID 2>/dev/null; echo "the player failed after waking (exit $?): again"
      WAKES=$((WAKES + 1)); sudo pkill -x azmix; wake
      continue
    fi
    break
  fi
  i=$((i + 1))
  # keep the last frame the player drew, even if it dies later
  [ "$T" != 0 ] && [ $((i % 5)) = 0 ] && DISPLAY=$DISP xwd -root -silent > /opt/xdjaz/shot.xwd 2>/dev/null
  sleep 1
done
if kill -0 $BPID 2>/dev/null; then echo "still running after ${T}s"; kill -INT $BPID; sleep 3; kill -KILL $BPID 2>/dev/null; fi
wait $BPID; RC=$?; echo "exit=$RC"
sudo pkill -f "pin/azpin[.]py"	# gives the threads all cores back
if [ "${XORG:-0}" = 1 ]; then
  [ "${KEEPX:-0}" = 1 ] || { pkill -f "strip/azstrip[.]py"; pkill -f "strip/azdrop[.]py"; pkill -x picom; pkill -x azvsync; sudo pkill -x xdjaz-usb.sh; sudo pkill -f "Xorg $DISP vt"; sudo pkill -x aztouch; sudo pkill -f "bt/azbt[.]py"; }
else
  pkill -f "Xvfb $DISP"
fi
exit $RC
