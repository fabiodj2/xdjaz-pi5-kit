# XDJ-AZ on a Raspberry Pi 5

**English** · [Русский](README.ru.md) · [Українська](README.uk.md)

Run the stock XDJ-AZ player (firmware **1.30**) on a Raspberry Pi 5 with a
**DDJ-GRV6** (4 decks), **DDJ-FLX4** or **DDJ-400** (2 decks; the DDJ-400 is a port, see below). It is the very player the AZ
runs: its screen, the rekordbox library browser, waveforms, hot cues, loops,
Beat FX, sync. What a Pi lacks compared to the AZ (the panel's
microcontrollers, the hardware mixer, the deck's kernel) is replaced by the
programs in this repository.

From "I have the 1.30 image" to a Pi that boots into the player: one command.

Questions, help and just AZ talk: [Discord](https://discord.gg/QgbsfBbgaa).

## What you need

| | |
|---|---|
| Raspberry Pi 5 | 4 or 8 GB; SD card or SSD of 32 GB or more (the player makes itself ~10 GB of cache files) |
| Raspberry Pi OS Lite **64-bit** (Debian 13 trixie) | no desktop: the player owns the screen and the controller. A user with password-less `sudo` (the default for Pi OS's first user), in the `audio`, `video` and `input` groups |
| Screen | HDMI; set up for a 1024×600 touch panel. Other screens: see [Another screen](#another-screen) |
| Controller | DDJ-GRV6, DDJ-FLX4 or DDJ-400 over USB. With several plugged in, the GRV6 is used, then the DDJ-400 |
| Firmware image | `XDJAZv130.iso`, the **decrypted** 1.30 update image (ISO 9660, label `UsbAuto`). Not included |
| OneLibrary key | a file with one line. Without it a stick shows only folders, no playlists or library. The key is the same for every deck; ours comes from [AZ-MODS](https://github.com/Kyle-Hosman/xdj-az-mods), which opens the library with it in its emulator. Not included |
| USB stick | a rekordbox export with OneLibrary (`PIONEER/rekordbox/exportLibrary.db`); the 1.30 player does not read the older `export.pdb` |
| AZ-MODS (optional) | `mods-vX.Y.Z.tar.gz` from the [AZ-MODS](https://github.com/Kyle-Hosman/xdj-az-mods) releases: Mod Menu, stems, playlist editing, themes |

## Quick start

**1. The Pi.** Write Raspberry Pi OS Lite 64-bit with Raspberry Pi Imager,
with SSH on. Boot it, plug in the screen and the controller.

**2. The kit, the image and the key on the Pi** (`pi` is your Pi's address):

```sh
ssh pi git clone https://github.com/r1x0s/xdjaz-pi5-kit
scp XDJAZv130.iso onelibrary.key pi:
scp mods-v1.2.8.tar.gz pi:          # for AZ-MODS
```

**3. Install** (on the Pi):

```sh
cd ~/xdjaz-pi5-kit
sudo ./scripts/setup-pi.sh --iso ~/XDJAZv130.iso --key ~/onelibrary.key
#   add --azmods ~/mods-v1.2.8.tar.gz for AZ-MODS
#   add --network for the network and the player's Wi-Fi screen
```

The script installs the packages, extracts the player's file system from the
image into `/opt/xdjaz/rootfs` (and checks that it is 1.30), builds our
programs on the Pi, puts everything in `/opt/xdjaz` and installs the
`xdjaz.service` unit. A few minutes, mostly `apt`. It can be run again: it
rebuilds and reinstalls everything but the player and its settings.

**4. Reboot** (`sudo reboot`). The Pi now boots straight into the player; the
console login on tty1 is turned off, as it would share the screen. To start
the player by hand instead, install with `--no-autostart`.

About 8 seconds after the service starts the player is on screen. Plug in a
stick: it shows in SOURCE as USB1.

- The **controller** can be plugged in later: until it is there, the service
  tries again every few seconds.
- If the **player ends or crashes**, it is started again after 3 seconds.
- **Stop** it: `sudo systemctl stop xdjaz` (until the next boot). No autostart
  at all: `sudo systemctl disable xdjaz && sudo systemctl enable getty@tty1`.
- While the player runs, the Pi's console is reached over SSH.

**Network and Wi-Fi** (`--network`, off by default). The player shares the
Pi's network, and its own Wi-Fi screen (the Wi-Fi icon at the top → Wi-Fi
Setting) runs the Pi's Wi-Fi: the list is what the Pi's Wi-Fi sees, the
network picked there is joined by the Pi, and Wi-Fi ON/OFF switches the Pi's
Wi-Fi. `azwifi.py` (the `azwifi` service, root) does it through
NetworkManager, Raspberry Pi OS' default, or else the Pi's own
`wpa_supplicant`; it finds the Wi-Fi interface itself (`AZWIFI_IFACE`,
`AZWIFI_BACKEND=nm|wpa` to choose). Networks joined from the player are kept
as `azwifi-<name>`; the Pi's own networks are left alone. SSH over Ethernet is
not touched.

## What works

- **The controller.** Every deck button, the pads, the jog (scratch, pitch
  bend, search), tempo, loops, hot cues, BEAT SYNC / MASTER, KEY SYNC, SLIP,
  QUANTIZE, the browser. The LEDs follow the player, as on the AZ. At start
  the player knows where every knob and fader is (the controller reports them
  on request).
- **4 decks on the GRV6.** The DECK button switches a side to deck 3/4, and
  the player switches with it.
- **SHIFT** of either side shifts both decks, as on the AZ.
- **The mixer.** Trim, EQ, channel faders, crossfader, CUE, the level meters
  on the controller. It follows the player's MIXER SETTINGS screen: crossfader
  assign per channel (the AZ's default is THRU on all four: set A/B there to
  use the crossfader), crossfader and channel fader curves, EQ or ISO,
  headphones MONO SPLIT, LINK preview volume.
- **COLOR FX.** SMART CFX press: on/off; with SHIFT: the next effect. The GRV6
  has a knob per channel.
- **Beat FX.** All 14 of the player's effects, beat, level, channel. On a
  channel the effect comes after the fader: pull the fader and the echo's tail
  stays. The GRV6's eight beat lamps show the chosen beat.
- **Master level.** The DDJ-400's MASTER LEVEL knob (a 14-bit MIDI CC) sets the
  mixer's master level. The FLX4's is analog and the GRV6's works inside the unit,
  so they send nothing. Until the knob first moves, the level is at full.
- **BEAT FX on the screen.** `aztouch` can press the beat FX from the panel: ON/OFF,
  the effect (next / previous / a given one), the beat (BEAT < >) and the X-PAD
  (a touch and a position along a strip, as the AZ's X-PAD sends them to the
  player). The zones are drawn in `/opt/xdjaz/bfx.zones` (example and format:
  `src/touch/bfx.zones.example`); `AZ_TOUCH_DEBUG=1` logs where each tap lands, to
  measure them. A touch in a zone never reaches the player. The virtual X-PAD is the
  panel's X-PAD PARAMETER strip (BEAT, 1/16 .. 1): while a finger is on it the beat
  FX is on at the beat under the finger (1/16 is taken as 1/8, which the effects
  have), and the lift puts the effect on/off and the beat back as they were. That
  momentary behaviour is from the AZ's own X-PAD and not checked on the unit.
- **Headphones.** FLX4: mixed by our mixer (its HEADPHONES MIX and LEVEL
  knobs). The GRV6 mixes its headphones itself and gets the CUE bus alone.
  The track preview (touch preview in the browser) plays in the headphones
  only, as on the AZ.
- **Microphone.**
  - FLX4: the mic goes through our mixer: MIC LEVEL, talkover (UTILITY >
    Mixer; the voice turns it on, Normal or Advanced), the Feedback Reducer
    (MIXER SETTINGS). SHIFT + the FX CH lever puts the Beat FX on the mic
    alone.
  - GRV6: the unit mixes the mic into its master itself; it gets no effects
    or level from here.
- **MASTER REC** records the master as heard to the stick's
  `ALPHATHETA REC` folder (WAV, 44.1 kHz). With MIC Output to Master REC on,
  the GRV6's recording includes the mic.
- **USB sticks** come and go while playing. A stick is read-only to the
  player except its `ALPHATHETA REC` folder; the player writes its database
  changes to a copy in memory, so your library on the stick is never changed.
- **Sleep.** When the controller goes into standby (its Auto Standby), the
  player sleeps with it: black screen. Wake the controller and the player is
  back in ~10 s with the sticks in SOURCE; loaded tracks are unloaded, as
  after an AZ's standby.
- **Buttons on screen.** With `PANEL=juce` the player sits at the left edge
  and the free strip on the right gets the buttons the AZ has above its
  screen: HOME (back to the waveforms), SOURCE, BROWSE, TAG LIST, PLAYLIST,
  SEARCH, SHORTCUT, MENU (hold: UTILITY), BACK, with the player's own icons.
  The open view's button is lit, however it was opened. `STRIP=0` turns them
  off (see [Settings](#settings)).

### Mapping that is not obvious

| Controller | On the AZ |
|---|---|
| Pad modes HOT CUE, BEAT JUMP | as printed |
| PAD FX1 / STEMS | BEAT LOOP |
| SAMPLER | SLIP LOOP |
| SHIFT + BEAT JUMP (BEAT LOOP) | BEAT LOOP |
| 4 BEAT / EXIT | a 4-beat loop, or out of the loop if one is playing |
| SMART FADER (FLX4), DISCOVER (GRV6) | SOURCE |
| VIEW (GRV6) | BROWSE |
| PREVIEW (GRV6) | SEARCH: the search bar with the on-screen keyboard |
| SHIFT + VIEW (GRV6) | TAG TRACK |
| SHIFT + BACK (GRV6) | PLAYLIST (the playlist palette, BANK 1..4) |
| SHIFT + DISCOVER (GRV6) | TRACK FILTER (the AZ has no Related Tracks) |
| SHIFT + PREVIEW (GRV6) | back to the waveform screen |
| Browse tilt (GRV6) | up/down: a row (with SHIFT: a page); left: BACK; right: select |
| SHIFT + SLIP (VINYL) | JOG MODE |
| SHIFT + FX CH lever (FLX4) | Beat FX on the mic alone; the lever without SHIFT goes back to the channel |
| SHIFT + MEMORY | delete a cue |
| DRUM RELEASE lever (GRV6) | while tilted, the deck's channel slows to a stop like a record |
| Beat FX knob (GRV6): LC ECHO, PITCH, MOBIUS saw, MOBIUS tri | PING PONG, TRIPLET FILTER, MOBIUS, TRIPLET ROLL (the AZ has none of the four) |
| BEAT < > | 1/8, 1/4, 1/2, 3/4, 1, 2, 4, 8 |
| SHIFT + SMART FADER (FLX4) / SHIFT + BACK (GRV6), held 2 s | back to the system menu, when the Pi runs several DJ systems under our launcher; nothing otherwise |

## DDJ-400 (port, untested on most controls)

The DDJ-400 shares the FLX4's MIDI layout, so it uses the FLX4's map (`azbridge -c ddj400`,
sound card `DDJ400`). Differences from the FLX4:

- It has no mic input, no SMART FADER and no FX CH lever; those controls do nothing.
- Its USB sound card has 4 playback channels at 44100 Hz and no capture.
- On startup the bridge asks it for the position of every knob and fader (the same request
  Mixxx's DDJ-400 script sends); without it the channel faders start at 0 and stay silent
  until moved.
- Jog resolution and the BACK / return-to-menu button are taken from the FLX4, not measured.

### Raspberry Pi 4 + DDJ-400: settings that were measured

Measured on a Pi 4 (4 GB, 1.8 GHz), a 1920x1080 HDMI screen, a touch panel, two decks
playing, AZ-MODS on (its 60 FPS Waveforms). The kit was written for a Pi 5, and the
player paints in software on one thread, so on a Pi 4 the size of the player is what
sets the frame rate (`/etc/systemd/system/xdjaz.service.d/local.conf`):

```ini
[Service]
Environment=AZMODS=1
Environment=COMP=0
Environment=AZ_SCALE8=5
```

| `AZ_SCALE8` | player | frame rate, two decks |
|---|---|---|
| 8 | 1280x800 | 30 |
| 6 | 960x600 | 37 |
| 5 | 800x500 | 60 |

What did not help here (each tried alone): the CPU at 2.0 GHz, `AZ_PIN=1` (pinning the UI
thread alone on a core: 30 against 60 fps with it off, at the settings above; `azpin` is
on by default only on a Pi 5), the LEDs on or off, a compositor
(`COMP=xrender`, the GPU gets busy), running the screen at 1280x720, and the blit guard
(`azshim` held 2 of 275 puts a second, 2 ms in all). Without the mods the player paints at
30 fps, so they stay on. On the screen, `AZ_SCREEN=1280x800` adds that mode if the screen
lacks it.

### 7-inch 1024x600 HDMI panel with a QDtech MPI7003 touch (measured)

The panel's EDID is generic: it lists only 16:9 modes (1920x1080 down to 800x450) and
`255mm x 255mm`, not 1024x600, so X starts in 1600x900 or 1920x1080 and the panel scales it.
The touch (USB `0483:5750`, input name `QDtech MPI7003`) reaches X directly, not through
`aztouch`. Checked with a grid and a tap-the-cross test (touch scale, 1.000 = the touch
lands on the picture):

| X mode | the panel shows | touch scale (x, y) |
|---|---|---|
| 1024x600 | the whole picture, 1:1 | 1.008, 0.983 |
| 1280x720 | the whole picture, scaled | 0.984, 0.962 |
| 1280x800 | the whole picture, scaled | 0.973, 0.979 (0.984, 0.998 on a second run) |

So the panel does not crop any of them, and the touch follows the picture in all of them.

The mods draw with coordinates of a 1280x800 screen (the FPS readout sits at x 1090-1278,
y 48-88), so the player is run at that size: no scaling by X (`AZ_FIT` stretches the picture
and is off), the screen in 1280x800 and the player at `AZ_SCALE8=8`:

```ini
[Service]
Environment=AZMODS=1
Environment=COMP=0
Environment=AZ_SCREEN=1280x800
Environment=AZ_SCALE8=8
```

With that the MOD MENU shows whole and the touch is aligned. Two decks play at 30 fps on a
Pi 4 (a smaller `AZ_SCALE8` gives more: 6 -> 37, 5 -> 60 with the screen at 1080p, but the
mods then draw outside the smaller window).

`AZ_SCREEN=1280x800` (or `1024x600`) is written into the X config as a `Modeline` plus
`PreferredMode`, because a mode made with `xrandr --newmode` at start did not hold: the next
`xrandr` call no longer found it (`newmode=0 addmode=1` in the journal). `~/testmode.sh`-style
checks by hand are `xrandr --newmode`, `--addmode`, `--output --mode`, one call each.

The DDJ-400 hung on the unit used here (still enumerated, no USB error, the Pi at
`throttled=0x0`) with the LEDs on, and only a power cycle (pulling the cable) brought it back;
with `AZBRIDGE_OPTS=-n` (no LEDs, no VU) it was stable. It was first blamed on power, but a
meter on the port read 5.01 V and a steady 400 mA while it hung, so it is not power: it is
something in what the bridge writes to the controller. `AZ_LEDS` lists the groups that are
sent (`core,loop,extra,modes,pads,mixer,vu`; unset = all) to find the one that does it:
`core` is PLAY, CUE, SYNC and headphone CUE (the DDJ-400's own LED notes), `loop` LOOP IN/OUT,
`extra` SLIP/QUANT/KEY SYNC/MASTER TEMPO and `mixer` MASTER CUE/BEAT FX/SMART CFX (notes its
map does not list), `modes` the pad mode buttons, `pads` the 8 pads, `vu` the channel meters.

## AZ-MODS

With `--azmods` the player runs with the mods: their `azmod.so` and
`azstem.so` are loaded into it as on an AZ. **MOD MENU** shows in SOURCE.
Stems: turn Stems on in the menu and make the stems with their desktop app,
which puts them on the stick in `STEMS/`. The mods' USB loader, their
self-update and SSH are not used here; `setup-pi.sh` installs the mods, run it
again with a newer archive to update.

To run without the mods: `AZMODS=0` (see [Settings](#settings)).

## Settings

Environment variables of the service. Create
`/etc/systemd/system/xdjaz.service.d/local.conf`:

```ini
[Service]
Environment=PANEL=fit
```

then `sudo systemctl daemon-reload && sudo systemctl restart xdjaz`.

| Variable | Values |
|---|---|
| `PANEL` | how the 1280×800 UI fits a 1024×600 screen: `juce` (default: exact proportions, bars at the sides), `fit` (full screen, 6% squashed in height), `stretch`, `panel`. Details: `src/docs/panel.md` (in Russian) |
| `REFRESH` | screen refresh, default 59.24 Hz like the AZ's: the waveforms run smoothest at it |
| `COMP` | compositor: `0` none, `glx`, `xrender`; with AZ-MODS the default is `glx` |
| `AZMODS` | `0`: the player without the mods |
| `AZ_CARD` | the controller's sound card name, if detection picks the wrong one |
| `STRIP` | `0`: no buttons on the right, the player in the middle between bars. Only with `PANEL=juce`; the other modes have none anyway |
| `AZ_SLEEP` | `0`: no sleep with the controller (the player then exits when the controller goes) |

## Another screen

Three things in `/opt/xdjaz/xorg-az.conf` (source: `src/probe/xorg-az.conf`)
are set for the 1024×600 panel:

- `Modeline` and `PreferredMode`: the panel's mode. For another screen remove
  them (X takes the mode from EDID) and set `PANEL=panel` if the screen does
  1280×800 itself.
- `CalibrationMatrix` turns touches by 180°: our panel's touch sensor is
  turned against its picture. If touches land right on your screen without
  it, remove that section.
- `kmsdev /dev/dri/card1`: the Pi 5's video output, leave it.

In `src/probe/run-probe.sh` the touch filter `aztouch` is started for a
1024×600 panel turned 180° (`-S 1024x600 -r`) and, with `PANEL=juce`, makes
the small icons at the top bar's edge (REC timer, gear) easier to hit
(`-z` zones). Change or drop those for another screen. With the buttons
(`STRIP`) those zones move with the window, and the buttons' own zones (`-b`)
come from `src/strip/azstrip.py`; for another screen set `STRIP=0`.

The scale the player draws itself at is set in `src/probe/dconf` (6 of 8 = 0.75).

## How it works

```
 controller (MIDI) ──► azbridge ──UCOM frames──► azshim ─┐
        ▲                │                                │   EP147 (the AZ 1.30 player)
        └──── LEDs ◄─────┘◄──── TX frames (LEDs) ─────────┤   in bwrap, root = /opt/xdjaz/rootfs
                                                          │
 controller audio ◄── azmix ◄── snd-aloop "ALPHATHETAD810" ◄── 8 deck channels + preview
```

| Program | Stands in for |
|---|---|
| `EP147` | the player from the firmware, unchanged. It runs in `bwrap`: its own root, user 0 in its own namespace, no network (the Pi's with `--network`), a minimal `/dev` |
| `azshim.so` | the AZ's kernel: connects the player to the bridge instead of the microcontrollers, USB stick events, the OneLibrary key, small drawing fixes |
| `azbridge` | the AZ panel's microcontrollers: button and knob frames from the controller's MIDI, controller LEDs from the player's frames. It also runs the Beat FX, as the AZ's mixer processor does |
| `azmix` | the AZ's hardware mixer: EQ, COLOR FX, faders, Beat FX, CUE, out to the controller's sound card in real time |
| `aztouch` | a filter for the panel's touch glitches; it also takes the touches on the on-screen buttons |
| `azstrip.py` | the buttons the AZ has above its screen, drawn right of the player (`STRIP`) |
| `azvsync` | the screen's frame times, which the player paints by |
| `xdjaz-usb.sh` | USB stick hot-plug |
| `azwifi.py` | the AZ's Wi-Fi (`wpa_supplicant`, `iw`): the player's Wi-Fi commands, carried out on the Pi's Wi-Fi (`--network`) |
| `azpin.py` | the AZ's six cores: the player pins its threads as for those, so this moves its UI thread onto a core of its own (`AZ_PIN=0` off, `AZ_PIN_CPU`, default 3) |
| `xdjaz-prep.sh`, `xdjaz.service` | the audio loopback on the controller's clock, start and stop |

The AZ's microcontroller protocol (UCOM frames) is described in
`src/bridge/UCOM-PROTOCOL.md`.

## Repository

```
scripts/extract-rootfs.sh  1.30 image -> the player's file system (any Linux)
scripts/setup-pi.sh        install on the Pi
systemd/xdjaz.service      the service: player at boot, restart, waits for the controller
src/                       the sources of our programs
```

`extract-rootfs.sh` works on its own too:
`./scripts/extract-rootfs.sh XDJAZv130.iso ./rootfs` takes
`images/images.tar.gz` out of the image, the deck's kernel out of that, the
kernel's built-in initramfs (the whole AZ system) out of the kernel, and
checks the player's md5.

## Troubleshooting

| Where | What |
|---|---|
| `journalctl -u xdjaz` | service start and stop |
| `/opt/xdjaz/ep147.log` | the player's output |
| `/opt/xdjaz/bridge/azbridge.log` | the controller: which one, frame connections, unknown messages |
| `/opt/xdjaz/mixer/azmix.log` | audio |
| `/opt/xdjaz/xdjaz-usb.log` | USB sticks |
| `/opt/xdjaz/xorg.log` | the screen |
| `journalctl -u azwifi` | Wi-Fi (`--network`) |

- **No player, `no controller` repeating in the journal**: the controller is
  not plugged in or not seen as a sound card (`cat /proc/asound/cards` should
  list `DDJGRV6` or `DDJFLX4`).
- **Only folders on the stick**: no OneLibrary key
  (`/opt/xdjaz/keys/onelibrary.key`), or no `exportLibrary.db` on the stick
  (export again with a current rekordbox).
- **Suspect the mods**: `AZMODS=0`.
- **A screenshot** without a camera: `/opt/xdjaz/tools/shot.py out.png`.
- **Press controller buttons from a shell**: add
  `Environment="AZBRIDGE_OPTS=-t /opt/xdjaz/bridge/tx.log -i /opt/xdjaz/state/azbridge.in"`
  to `local.conf`; then `echo "96 41 7F" > /opt/xdjaz/state/azbridge.in` acts
  as if it came from the controller, and `tx.log` records everything the
  player sends.

## Limits

- Firmware **1.30** only: the fixes and maps are made for this player build
  (md5 `3518275e8b4d5d63636d8d7b4468d7ba`); the script refuses any other.
  (If you have the 2.00 image or the LUKS key that unpacks Pioneer's `.upd`
  files — you know where to find me.)
- Without `--network` the player has no network. With it, the Wi-Fi screen
  is what has been tried; streaming, Pro DJ Link and updates have not.
- No Bluetooth.
- No limiter: hot tracks clip with TRIM above centre.
- The "1/3 1/2 2/3" hint on the BEAT FX panel is drawn by the player itself;
  BEAT < > never land on 1/3 or 2/3.

## Legal

This repository holds only its own programs, scripts and documentation. It
contains no firmware, no part of the player, no key and no AZ-MODS file, and
none of them may be added to it. You need your own copy of the firmware and
are responsible for using it lawfully. Not affiliated with AlphaTheta or
Pioneer DJ; XDJ-AZ, DDJ-GRV6, DDJ-FLX4, DDJ-400 and rekordbox are their trademarks.
AZ-MODS is a separate project under its own license (GPL-3.0).

## Thanks

- [fabiodj2](https://github.com/fabiodj2), also
  [@fabiodj2](https://www.instagram.com/fabiodj2) on Instagram, for
  helping me out with a question.
- [AZ-MODS](https://github.com/Kyle-Hosman/xdj-az-mods) — the OneLibrary
  key that opens the stick's library.

## License

Non-commercial use with attribution: see [LICENSE](LICENSE). Use in your
own projects with a credit to the author.

Author: [r1x0s](https://github.com/r1x0s)
