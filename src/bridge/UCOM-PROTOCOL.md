# XDJ-AZ (fw 1.30) player <-> UCOM protocol

Static analysis of `XDJAZ/v130/rootfs/home/root/pdj/EP147` (aarch64, non-PIE, base 0x400000) and
`subucom_read`. Nothing was run. The IDA database and scratch output
(`EP147.i64`, full disassembly `all.s`, helper scripts) stayed in a local
scratch folder.

Confidence marks:
- **[C] confirmed**: read directly from code. The address is given.
- **[I] inferred**: likely, but not proven by code.

Addresses are EP147 virtual addresses unless the text says `subucom_read`.

---

## 0. Summary

| Device | Name in code | Role | RX size | TX size | RX CRC | TX CRC | TX timer |
|---|---|---|---|---|---|---|---|
| `/dev/subucom_spi1.0` | `erp_ucom` ("ErP") | deck buttons for both sides, pads, power and USB-stop switches | 128 | 128 | bytes 0..27, stored at 28 | bytes 0..119, stored at 120 | 10 ms |
| `/dev/subucom_spi2.0` | `deck_ucom`, side 1 ("Right") | jog wheel, tempo slider, jog display | 152 | 152 | bytes 0..37, stored at 38 | bytes 0..147, stored at 148 | 4 ms |
| `/dev/subucom_spi3.0` | `mixer_control_ucom` ("MC") | whole mixer plus browse section (encoder, LOAD 1-4, etc.) | 128 | 128 | bytes 0..95, stored at 96 | bytes 0..103, stored at 104 | 3 ms |
| `/dev/subucom_spi5.0` | `deck_ucom`, side 0 ("Left") | jog wheel, tempo slider, jog display | 152 | 152 | bytes 0..37, stored at 38 | bytes 0..147, stored at 148 | 4 ms |

- **[C]** Device-to-side mapping. `sub_2100AA8` builds index 0 from `spi5.0` and index 1 from `spi2.0`. `sub_21371F0` logs "Left" for index 0 and "Right" for index 1. `sub_2100D18` builds `spi1.0` with the ErP dispatcher and builder (`sub_2142660` / `sub_2149A20`). `sub_21023C8` builds `spi3.0` with the MC dispatcher and builder (`sub_2153B70` / `sub_215C2D8`). `subucom_read` names spi1.0 "ErP" and spi3.0 "MC".
- **[I]** MPNL has no device node of its own. Its version arrives inside the MC RX packet (bytes 2..3, see §2.3). The MPNL MCU probably sits behind the MC MCU.
- **[C]** Every packet uses the same checksum: CRC-16/X-25 (also called IBM-SDLC). Details are in §1.5.
- **[C]** The player waits for no handshake. A packet is accepted when two things hold: the length equals the full frame size and the CRC matches, and the u16 at offset 0 (the "version") is not 0. See §3.

---

## 1. Driver interface (all four devices)

`SubCpu` methods. Vtable at `0x2e5a568`:
- +0x10 `getFd` = `21c5330`
- +0x18 `initialise` = `21c4eb0`
- +0x20 no-op = `21c38d0`
- +0x28 `readData` = `21c38e0`
- +0x30 `writeData` = `21c3970`
- +0x38 timer-off = `21c3ac0`
- +0x40 `closeDevice` = `21c3a20`

### 1.1 Open and init: `SubCpu::initialise(path, interval, size)` @ `21c4eb0` [C]

The call order is:
1. `open(path, O_RDWR)`, flags = 2.
2. `setBits(16)` @ `21c3f50`.
3. `setTransferSizeInBytes(size)` @ `21c40b0`.
4. `setTransferIntervalMillis(interval)` @ `21c4200`.
5. `setTimerStatus(1)` @ `21c4ae0`.

Each setter first reads the current value with the `_IOR` request. It writes with `_IOW` only when the value differs. The ioctl magic is `'p'` (0x70).

| Purpose | Read request | Write request | Argument | Value used by EP147 |
|---|---|---|---|---|
| TX frame ("writeData") | — | `0x40107000` (`_IOW('p',0,16)`) | struct, see §1.3 | — |
| timer_status | `0x80017001` | `0x40017001` | u8 | 1 at init; 0 to stop (`21c3ac0`) |
| transfer interval | `0x80047002` | `0x40047002` | u32 | 1 for every device |
| bits_per_word | `0x80027003` | `0x40027003` | int (encoded size 2) | 16 |
| rx_bytes / transfer size | `0x80047004` | `0x40047004` | u32 | 152 (deck), 128 (ErP, MC) |

- Error strings: "failed to read/write timer_status / bits_per_word / rx_bytes / transfer interval".
- The interval setter's error text says "polling_interval_micro_seconds", so the unit (ms or µs) is ambiguous **[I]**.
- The values come from per-device configs `{path, 1, period, size}` built in `2100AA8`, `2100D18` and `21023C8`:
  - deck: `{1, 4, 152}`
  - ErP: `{1, 10, 128}`
  - MC: `{1, 3, 128}`
- **[C]** In `SubCpuCommunicator::SubCpuCommunicator` @ `21c5660`, the call is `initialise(path, cfg+8, cfg+16)`. `cfg+12` is the TX timer period.

### 1.2 RX path: who drives the cadence [C]

- After init, the communicator registers the fd with the HUI event manager for readability only (`21efe60`, events |= 1 = EPOLLIN). The single `epoll_wait` call is at `21f66ec`.
- On readable, `21c5500` calls `readData`. `readData` is `read(fd, buf, rx_size)` @ `21c38e0`, then `dispatcher->dispatch(buf)` runs.
- The RX buffer size comes from `dispatcher->size()`: deck `2138088` = 152, ErP `2143f30` = 128, MC `21550a8` = 128.
- **[C]** The return value of `read()` is only checked for < 0 ("failed to read."). A short read is **not** detected. `interpret()` checks the buffer length, not the byte count read. A bridge must therefore always return a full frame.
- **[I]** The kernel driver (`subucom_spi`, built into the kernel, no `.ko` file) does the full-duplex SPI transfers on its own timer (`timer_status`=1, interval). `read()` blocks or becomes readable once per transfer. `subucom_read` instead sets timer_status to 0 and calls `read()` directly with 10 ms sleeps, so a read probably triggers a single transfer when the timer is off.

### 1.3 TX path [C]

- `huiTimerCallback` @ `21c5340` calls `builder->build(txBuf)` and then `writeData(txBuf)`.
- The timer starts in `21c55e0` with period `cfg+12`: 4 ms (deck), 10 ms (ErP), 3 ms (MC). The time unit is **[I]** ms.
- `writeData` @ `21c3970` does **not** use `write()`. It calls `ioctl(fd, 0x40107000, &s)` with this struct:
  ```
  struct { uint32 a = 0x01000000; uint32 len; uint64 buf_ptr; }   // 16 bytes
  ```
  - `len` is the TX size (152 or 128).
  - The meaning of `a` is unknown. It reads as bytes `00 00 00 01`.
  - When this ioctl fails, the log shows "failed to write." That matches what you saw on the Pi.

### 1.4 Stop
- `21c5620`: stops the timer, then calls `closeDevice` (`close()`, fd = -1).
- `21c3ac0` reads timer_status and writes 0 when it is set **[C]**.

### 1.5 Checksum [C]

Table-driven CRC: `subucom_read` `0x400c10`, EP147 deck `213bdd0`, ErP `2147408`, MC `2159a08`. The TX copies are at `215a098`, `2147ea8` and `213c468`; they use the same table-generation code **[I]**.

- Reflected polynomial 0x8408 (constant `0x298d8e0` is 0x8408 ×8).
- Initial value 0xFFFF.
- `crc = T[(b ^ crc) & 0xFF] ^ (crc >> 8)`, final `~crc`.
- That is CRC-16/X-25: check value of "123456789" = 0x906E.
- Stored **little-endian** right after the covered range: low byte at offset N, high byte at N+1 (`subucom_read`: `[0x1c]`=lo, `[0x1d]`=hi).
- On mismatch: the log says "crc error: <type> (n)" and the packet is dropped.

---

## 2. RX layouts (MCU -> player)

Common format checks (`Format<…>::interpret`):
- **[C]** size equal to the frame size, otherwise "data size mismatch".
- **[C]** CRC.
- **[C]** the `version` u16 at offset 0 is not 0. Otherwise: "data->field.version!=0" (deck `2137e88`, ErP `21435c0`) or "data->field.version.mixerControlUcom!=0" (MC `2154ec0`).
- **[C]** Version encoding is BCD, `0xMMmm` -> "M.mm" (`sub_6f4068`). Example: 0x0103 = "1.03".
- Shipped firmware revisions (`iso/images/*.rev`): DECK 1.02, ErP 1.00, MIXER_CONT 1.03, MPNL 1.02.
- **[I]** The versions are only displayed (`sub_6F4800`, "ErP:… MPNL:… MC:… DECK:…"). Nothing in EP147 compares them with the bundled firmware. The names `erp.bin`, `deck1.bin`, `mixer.bin` in EP147 are recordings of the "aging" test robot, not firmware.

Button events go through `sub_2147BF0(btn, bool)`. Knobs, faders and sliders go through `sub_2159560(obj, {0, raw})` with a linear adapter whose raw maximum is 1023 and output maximum 1.0 (`2156580` -> `21f0210(…,1023,1.0f)`). So:
- **[C]** analog values are 10-bit, 0..1023.
- **[I]** the normalised value is raw/1023.

How control names were obtained: each HUI control is registered under a 64-bit EasyID. The ID is either `h(name)` or `(h(name) << 8) | index`, where `h(s) = s[0] + 137*h(s+1) mod 0x1DE5D6E3F8868A2`. I recovered the names in two independent ways:
1. **[C]** by pairing the ID constants with the `linkToObserver (IdStruct::cue(), …)` assert strings in `midi_adapter::HuiEventHandler::linkToDeckHui` (`6cbd28`);
2. **[C]** by hashing candidate names, e.g. `h("cueButton")<<8 == 0x40045926d6629d00`.

All names below are confirmed by both methods unless marked otherwise.

### 2.1 ErP (`spi1.0`): 128 bytes, `erp_ucom::RxDataDispatcher::dispatch` @ `21435c0` [C]

| Offset | Content |
|---|---|
| 0..1 | u16 version (BCD, not 0) [C] |
| 2..7 | not read by the dispatcher [C] |
| 8..14 | **Left** deck buttons (side 0), parsed by `21431b0(deck0, rx+8)` [C] |
| 16..22 | **Right** deck buttons (side 1), parsed by `21431b0(deck1, rx+16)` [C] |
| 24 | global switches, parsed by `2142920` [C] |
| 28..29 | CRC over 0..27 [C] |

Per-deck block, with `D` = 8 for Left and 16 for Right. Bit = 1 means pressed [I]:

| Byte | bit0 | bit1 | bit2 | bit3 | bit4 | bit5 | bit6 | bit7 |
|---|---|---|---|---|---|---|---|---|
| D+0 | primaryDeckSelect | **beatSync** | slip | masterTempo | master | loopOut | loopIn | slipRev |
| D+1 | secondaryDeckSelect | keySync | tempoReset | reloopExit | tempoRange | beatJumpFwd | beatJumpRev | **shift** |
| D+2 | quantize (*) | **playPause** | **cue** | – | – | – | – | – |
| D+3 | searchFwd | searchRev | trackSearchFwd | trackSearchRev | – | – | – | – |
| D+4 | vinylSpeedAdjustButton | jogMode | fourBeatLoop | eightBeatLoop | memoryCueFwd | memoryCueRev | memoryCueDelete | memoryCueMemory |
| D+5 | padModeHotCue | padModeBeatLoop | padModeSlipLoop | padModeBeatJump | – | – | – | – |
| D+6 | **pad1** | pad2 | pad3 | pad4 | pad5 | pad6 | pad7 | pad8 |

(*) D+2 bit0 is read in `21435c0` / `21439f0` and fed to slot 16 (quantizeButton) of the same deck.

Pads have IDs `0x045926d60ea500xx | i`, i = 0..7, slots 36..43 (`2144df8`). primaryDeckSelect and secondaryDeckSelect are the DECK 1/3 and 2/4 selectors **[I]**.

Byte 24 (`2142920`) [C]:
- bit0: `powerButton`. When it reads 0, the log says "Receive powerSw=0". **[I]** Normal running state is 1; a bridge should keep it at 1.
- bit1: `usb1StopButton`.
- bit2: `usb2StopButton`.
- bit3: when 1, drives the `SL_PE` GPIO output (gpio0-3) through the GPIO object. **[I]** It powers a peripheral off; a bridge should keep it at 0.

ErP slot 31 is a 1024-step slider named `vinylSpeedAdjust`. The ErP RX parser never feeds it **[C]**.

Cross-check with `subucom_read`, which reads ErP at boot:
- `[8]==1 && [12]==1` -> testmode "on1" (primaryDeckSelect + vinylSpeedAdjustButton, Left).
- `[16]==1 && [17]==0x20` -> "on2".
- `[24]==7` -> "pmtest".

These combinations agree with the table.

### 2.2 Deck (`spi5.0` Left, `spi2.0` Right): 152 bytes, `deck_ucom::RxDataDispatcher::dispatch` @ `2137e88` [C]

| Offset | Content |
|---|---|
| 0..1 | u16 version (BCD, not 0) |
| 2 | bit7 = "shutdown preparation done" (calls `IShutdownPreparation::onPreparationResult` once) (`21371f0`) |
| 3 | bit4 -> flag (dispatcher +121), meaning unknown |
| 4..5 | u16 error code; logged as "EW_ERROR, Deck = {code, Left/Right}" when it changes |
| 6 | low nibble -> +136, high nibble -> +140, meaning unknown |
| 9 | jog flags: bit1 -> jog event byte4 (**touch** [I]), bit2 -> **direction** (0 means reverse; speed is negated), bit3 -> unknown flag |
| 10..11 | u16 LE **jog position counter**, wrapping |
| 12..13 | u16 LE **jog period**; 0xFFFF = stopped |
| 16..17 | tempo slider position: `((b17&3)<<8 \| b16)` (10-bit) << 6 |
| 18..19 | tempo slider reference/centre: `((b19&3)<<8 \| b18)` (10-bit) << 6 |
| 22..29 | 8 bytes copied to dispatcher +126 (not decoded) |
| 30..31 | u16 copied to +134 (not decoded) |
| 38..39 | CRC over 0..37 |
| 40..151 | not used by RX |

Jog conversion in `21380b8` (deck jog interpreter) [C]:
- `delta = signed16(pos - prevPos)`. Rotation output is `delta / 3240.0` turns, so 3240 counts = one revolution.
- When period ≠ 0xFFFF, `speed = 555.56 / period`.
- When speed > 0.5, a secondary value `(speed - 0.5) * 0.08` is also set.
- When the direction bit is 0, both values are negated.

Tempo conversion in `TempoSliderConverter::adapt` @ `216bff8` [C]. Input `{ref = bytes 18..19, pos = bytes 16..17}`, both in 16-bit units. The converter applies hysteresis (threshold 0x60, step 0x30), then:
- `pos > ref` -> `+(pos-ref)/(64767-ref)`.
- otherwise -> `-(ref-pos)/(ref-768)`.
- Output range -1..+1.
- **[I]** The usable raw range is 12..1011 (×64). Which physical direction is "+" is not known.

Controls registered for the deck UCOM in `HuiControlSet` `213b420`:
- `jog` (0x1dbb68xx)
- `tempoSlider` (0xce526aad3c48b6xx)
- 8 × `…HotCueButton` (0xbb6630f4b0bc00|i; the hash prefix is truncated)
- `beatLoop` (0x6b0ebd3ccc9276xx)

**[C]** The RX parser only feeds `jog` and `tempoSlider`.

### 2.3 Mixer control (`spi3.0`): 128 bytes, `mixer_control_ucom::RxDataDispatcher::dispatch` @ `2154ec0` -> `2154730` [C]

Header and status:

| Offset | Content |
|---|---|
| 0..1 | u16 **MC version** (must be ≠ 0) |
| 2..3 | u16 **MPNL version** (info getter +0x18) |
| 4..5 | u16 third version (getter +0x20), name unknown |
| 6 | bit7 = shutdown preparation done; bit3 (inverted) -> info flag `21323f0`; bit0 -> +875 (`2153478`) |
| 7 | bit7 -> +873, bit6 -> +874 (unknown) |

Analog controls. All are 10-bit values: an 8-bit high part plus 2 low bits.

| Bytes | Control |
|---|---|
| 10,11,12,13 + 14 | **channel faders 1..4** (`2153c48`): `ChN = (rx[10+N]<<2) \| ((rx[14] >> (6-2N)) & 3)` |
| 15 + 16 | **crossFader**: `(rx[15]<<2) \| (rx[16]&3)` |
| 26..27 | s16 counter, beatFxTime encoder (delta = new - old) |
| 28, 29 | beatFxXpad: bit0 of 28 = touch, 29 = position |
| 31..35 | browse block, see below |
| 36..55 | per-channel knobs, 5 bytes per channel `b = 36+5N`: trim = `rx[b]<<2 \| rx[b+4]>>6`, eqIsoHi = `rx[b+1]<<2 \| (rx[b+4]>>4)&3`, eqIsoMid = `rx[b+2]<<2 \| (rx[b+4]>>2)&3`, eqIsoLow = `rx[b+3]<<2 \| rx[b+4]&3` |
| 56..60 | masterLevel, masterEqHi, masterEqMid, masterEqLow (same 4+1 packing) (`2153dd0`) |
| 61..65 | color knobs Ch1..4 (sound colour FX per channel), same packing (`2153e80`) |
| 66..70 | mic1 eqHi = `66/70>>6`, mic1 eqMid = `67/70>>4`, mic1 eqLow = `68/70>>2`, beatFxLevelDepth = `69<<2 \| 70&3` |
| 71..75 | mic2 eqHi = `71/75>>6`, eqMid = `72/75>>4`, eqLow = `73/75>>2`, boothMonitor = `74<<2 \| 75&3` |
| 76..80 | headphonesMixing = `76/80>>6`, headphonesLevel = `77/80>>4`, soundColorFxParameter = `78/80>>2` |
| 81 | bit0 -> info object (`2132678`), meaning unknown |
| 84..93 | beatFxSelect encoder (4-bit counter in `rx[85]>>4`, delta) plus beat-FX status fields into the info object (`21545e8`). **Mapped on the panel 2026-11-06** by writing the bytes and reading the BEAT FX panel: the MCU owns the beat FX and reports their state here. 84 bit 7 = effect on (name shown blue), bit 6 = TAP (else AUTO), bits 2-3 non-zero = beat greyed; 85 high nibble = effect (0 DELAY, 1 ECHO, 2 PING PONG, 3 SPIRAL, 4 HELIX, 5 REVERB, 6 FLANGER, 7 PHASER, 8 FILTER, 9 TRIPLET FILTER, 10 TRANS, 11 ROLL, 12 TRIPLET ROLL, 13 MOBIUS; read off the panel, not `fx.h`'s order); 86 = beat index (2 1/16, 3 1/8, 5 1/4, 6 1/3, 7 1/2, 8 2/3, 9 3/4, 10 1, 12 2, 14 4, 16 8; odd ones in between are fractions the effects here do not have, the panel shows the nearest); 88..89 = BPM ×10; 90..91 = TIME in ms. 87, 92, 93 not seen to change the panel. The beatUp/beatDown bits (20.4/20.5) do not move the beat: the MCU is expected to. |
| 94 | bits 7,6,5,4 -> per-channel 0..3 flag (`2132268`), meaning unknown |
| 96..97 | CRC over 0..95 |

Buttons. Bit = 1 means pressed [I]:

| Byte.bit | Control | | Byte.bit | Control |
|---|---|---|---|---|
| 20.0..20.3 | **CUE ch4, ch3, ch2, ch1** (20.3 = ch1) | | 23.0 | channelSelect[4] |
| 20.4 | beatUp | | 23.1 | channelSelect[7] |
| 20.5 | beatDown | | 23.2 | channelSelect[6] |
| 20.7 | tap | | 23.3 | channelSelect[5] |
| 21.0..21.5 | soundColorFx[5..0] (21.5 = [0]) | | 23.4 | channelSelect[3] |
| 21.6 | masterCue | | 23.5 | channelSelect[0] |
| 21.7 | linkCue | | 23.6 | channelSelect[2] |
| 22.0 | beatFxOn | | 23.7 | channelSelect[1] |
| 22.1 | fxFreqLow | | 24.2 | fxQuantize |
| 22.2 | fxFreqMid | | 24.3 | autoTap |
| 22.3 | fxFreqHi | | 24.4 | mixerSettings |
| | | | 24.5 | talkOver |
| | | | 24.6 | mic2 on |
| | | | 24.7 | mic1 on |

The channel CUE mapping comes from `21540e0`: bits 3,2,1,0 of byte 20 go to slots `Ch<0..3>::cue`.

Browse block, `2154418(rx+31)`:

| Byte.bit | Control |
|---|---|
| 31.0 | flag sent together with the rotary selector event (**push of the browse encoder** [I]) |
| 32.2 | menu |
| 32.3 | search |
| 32.4 | playlist |
| 32.5 | taglist |
| 32.6 | **browse** |
| 32.7 | source |
| 33.0 | **LOAD deck 4** (`loadDeck4Button`) |
| 33.1 | **LOAD deck 3** |
| 33.2 | **LOAD deck 2** |
| 33.3 | **LOAD deck 1** |
| 33.4 | shortcut |
| 33.5 | trackFilter |
| 33.6 | tagTrack |
| 33.7 | back |
| 34..35 | s16 LE **rotarySelector** (browse encoder) absolute counter. The player sends `delta = new - previous`. The first packet only seeds the previous value. |

- Registered but never fed by the MC parser on this model **[C]**: up, down, left, right, loadLeft, loadRight, waveform, smoothEcho button and knob, zoneLevel, Mic `?` (type `2156da0`).
- `subucom_read` checks MC at boot. These checks agree with the table:
  - `[24]==0x10` (mixerSettings) plus `[21]&0x3f`: 0x10 = btdiag, 0x08 = ftm, 0x04 = sigma.
  - `[20]==0x30 && [24]==0x20` -> photomode.
- Not decoded: bytes 8..9, 17..19, 25, 30, 82..83 and 98..127.

---

## 3. TX layouts (player -> MCU), handshake, and missing replies

TX frames are built every 4, 10 or 3 ms and sent with the ioctl in §1.3. Each has the CRC described in §1.5.

- **Deck TX, 152 bytes** (`213c818`):
  - header 0..5 (`213c640`): mode bits and nibbles from controller state;
  - 6: bits 0-1 = two LED on/off flags (`213c768`);
  - 8..75: player info (`2178820`, `2178608`);
  - 76..147: track or display data queue (`2179c90`, `TrackInfoQueue` / `HidArtWork` / `HidOverviewWaveform`), i.e. the jog display;
  - CRC at 148.
  - **[C]** for the structure; fields are not decoded.
- **ErP TX, 128 bytes** (`2148b78`):
  - 0..1 flags (`2148080`): power-state bits from state word +328, LED-dimmer bit, mode nibble;
  - per deck `i`, a 56-byte block at `8+56i`:
    - bytes 8..12: 2-bit LED luminance fields (3 levels);
    - RGB triplets at 14..16, 18..20, 22..24 and 26..28;
    - **pad RGB** at 30..32, 34..36, … (8 pads, stride 4);
  - CRC at 120.
  - **Decoded 2026-11-06** (the indicator set `HuiIndicatorSet::Deck` @ `214f600` registers each LED by a HUI id; the ids are hashes of the LED names, `h = (137*h + c) mod 0x1DE5D6E3F8868A2` over the name's characters from last to first, 64-bit wrapping, then `<< 8`, so the names are known). Per deck, block base `B = 8 + 56*deck`, 2-bit fields from bit 0 up (0 off, 1 dim, 2 lit; the player blinks by toggling):

    | byte | bits 0-1 | bits 2-3 | bits 4-5 | bits 6-7 |
    |---|---|---|---|---|
    | B+0 | slipLed | masterTempoLed | masterLed | slipRevLed |
    | B+1 | reloopExitLed | keySyncLed | tempoResetLed | beatSyncLed |
    | B+2 | - | quantizeLed | loopOutLed | loopInLed |
    | B+3 | playPauseLed | cueLed | (a jog LED, from builder +56/+64) | - |
    | B+4 | primaryDeckSelectLed | secondaryDeckSelectLed | jogModeLed | vinylSpeedAdjustLed |

    RGB (R, G, B at +0..+2 of each, stride 4): B+6 `padModeHotCue`, B+10 `padModeBeatLoop`, B+14 `padModeSlipLoop`, B+18 `padModeBeatJump`, B+22+4k pad k (`padLed`|k). The mode not selected and an empty pad are dim colours (e.g. pad `08 0b 04`), the selected mode and a set hot cue bright.
    Seen in a session: after LOAD the CUE LED is lit and PLAY blinks; while a loop runs LOOP IN/OUT blink and RELOOP/EXIT is lit.
  - When state word +328 == (2,2), it cycles "updateReq/resetReq" bits into TX[1] bits 6-7 and TX[2] bit 6 ("Send updateReq and resetReq to erp-ucom.").
- **MC TX, 128 bytes** (`215bfe0`):
  - 0: flags (shutdown/power bits from the controller setters at `215caf0..215cb30`);
  - 1: low nibble = 2 or a brightness value;
  - 4..: LEDs (`215a2f8`);
  - 16..18: beat-FX info;
  - 24..: LEDs (`215a4a0`);
  - 36..38: LEDs (`215bd78`, `215be38`);
  - CRC at 104.
  - **Decoded 2026-11-06** (builder `215bfe0`, `215a4a0`, `215bd78`, `215be38`, and a session log):
    - 4..15 (`215a2f8`): mixer settings, 2- and 4-bit fields from the player's mixer settings object (curves, assignments; not mapped one by one).
    - 24: bit 1 = deck 1 SHIFT held, bit 0 = deck 2 SHIFT held.
    - 26..28: 2-bit LEDs of the browse/LOAD section (the player's mixer indicator set, 13 LEDs; `browseLed` and `menuLed` named by hash). After LOAD 1, 28 bits 6-7 blink; after LOAD 2, 28 bits 4-5.
    - 39 (=36+3): event flags per channel. Bits 0-3: a beat tick for channel 1..4, with the beat interval in ms (u32 LE) at 88 + 4*(ch-1) (e.g. `d6 01` = 470 ms at 128 BPM). Bits 4-7: a deck status record for channel 1..4 at 40 + 12*(ch-1): u32 BPM×100 (`00 32 00 00` = 128.00), u32 tempo ratio×10000 (`10 27` = 1.0000), u32 state (3 playing, 5 paused, 6 cued/loaded). Records are sent once (the next frame clears the flag).
    - Not in the TX at all: headphone CUE, BEAT FX on/select/beat, COLOR FX (latched in the AZ's mixer MCU).

Handshake and missing replies:
- **[C]** There is no request/response handshake and no version query. The RX path is passive (epoll -> read -> dispatch). Input is accepted once a frame passes the size, CRC and version≠0 checks.
- **[C]** No RX timeout or watchdog is tied to the UCOM path. With no frames, nothing happens: controls keep their last value. If TX ioctls fail, the only effect is the "failed to write" log.
- **[I]** The only reply-like signal is the "shutdown preparation done" bit: deck byte 2 bit7 and MC byte 6 bit7. Shutdown probably waits for it, so a bridge should set it once a shutdown is requested (TX power bits). This is not traced further.
- **[I]** A bridge does not need to parse TX for input to work. It only has to accept the ioctl (return 0) and hand out full RX frames.

---

## 4. `/dev/gpiodrv` and `/dev/hidg0`

- **[C]** `/dev/gpiodrv` (`2101b50` -> `20fba58`, "GpioThread") drives only these pins:
  - USB host over-current inputs `TUSBA1/2_xOC` (gpio4-28, gpio4-26);
  - USB power enables `TUSBA1/2_PE` (gpio0-9, gpio0-10);
  - SD-card power `SD33_CONT` (gpio2-5);
  - `SL_PE` (gpio0-3).
- **[C]** No UCOM function depends on it.
- **[I]** If the open fails, GPIO features are lost but controls are not affected. That error path was not traced.
- **[C]** `/dev/hidg0` (`2101bf0`) is the USB gadget HID link to a computer, used for PC/HID control mode (reader 1024 bytes, writer 256 bytes). It is unrelated to hardware detection.
- **[C]** Neither device is ioctl'd. The full ioctl list in EP147 contains only the `'p'` requests above, socket requests (0x89xx) and `FIONREAD` (0x541b).

## 5. `subucom_read` (run by `scripts/pre-setting.sh` and `emmc-fs.sh` before the app)

**[C]** Steps, from the disassembly of the tool:
1. Opens `spi1.0` (ErP) and `spi3.0` (MC) with O_RDWR.
2. On each: sets bits_per_word to 16 (`0x400ce0`) and forces timer_status to 0 (`0x400b90`).
3. Reads 128-byte frames, up to 10 tries with 10 ms between them. It requires a full 128 bytes and a valid CRC: ErP over 28 bytes (CRC at 0x1c), MC over 96 bytes (CRC at 0x60).
4. Decodes the boot button combinations listed in §2.1 and §2.3.
5. Writes `/tmp/testmode` with `echo "on1|on2|btdiag|ftm|sigma|pmtest|photomode|off"`.

`apl_start.sh` then starts EP147TestMode for on1, on2, btdiag, ftm and sigma; otherwise it starts EP147. The tool does not print packet contents.

**For a bridge:** have `subucom_read` produce all-zero buttons (or make `/tmp/testmode` contain "off") so the normal app starts.

---

## 6. Still unknown

- The exact meaning of the 0x01000000 word in the TX ioctl struct, and whether the interval unit is ms or µs.
- The real kernel driver semantics: blocking read versus a ring of frames, and what read() does while the timer is off. These are needed if the bridge emulates the char device (CUSE) rather than hooking calls with LD_PRELOAD.
- Deck RX bytes 3, 6, 22..31; the jog bit3 flag; confirmation that bit1 is touch.
- Tempo slider direction and which value is the centre reference (byte 18 versus byte 16 is from code; the physical meaning is inferred).
- MC RX bytes 81, 84..94 in detail (beat-FX status), byte 94 per-channel flags, and the third version field.
- Field-level TX layouts: which LED is at which bit, display data, and the shutdown/power state machine.
- The beatFxSelect encoder counter is 4 bits wide; wrap handling was not checked.
