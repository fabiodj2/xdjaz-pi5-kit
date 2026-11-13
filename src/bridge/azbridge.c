// azbridge: a DJ controller in place of the XDJ-AZ's own microcontrollers.
//
// The player reads its physical controls from four SubCpu (UCOM) devices
// (layouts in UCOM-PROTOCOL.md). azshim turns those opens into connections
// to this daemon's socket; each connection first sends the device name. We
// keep one RX frame per device, update it from the controller's MIDI and
// send it every TICK_MS and on every change. Of the frames the player sends
// back, the ErP one (deck LEDs) lights the controller; the rest (jog display,
// the mixer's deck data) are not used yet.
//
// The AZ's mixer is hardware, so the audio side (azmix) needs the mixer
// controls too: the mixer frame is also kept in MC_SHM for it.
//
//   ErP     subucom_spi1.0  128 B  deck buttons, pads, power switch
//   deck R  subucom_spi2.0  152 B  jog, tempo
//   mixer   subucom_spi3.0  128 B  mixer, browse, LOAD
//   deck L  subucom_spi5.0  152 B  jog, tempo
//
// The controller map is a table (struct ctl), one per controller (struct
// ctlr): the DDJ-FLX4's, the DDJ-400's (same layout as the FLX4's, so the same
// table) and the DDJ-GRV6's. -c picks one; by default the GRV6 when its card is
// there, else the DDJ-400, else the FLX4. Deck 1 drives the AZ's left
// deck and mixer channel 1, deck 2 the right deck and channel 2. The GRV6's
// deck layers 3 and 4 drive the same two sides, switched to the AZ's DECK 3
// and DECK 4 (see "Deck layers"), and mixer channels 3 and 4.
//
//   azbridge [-c flx4|ddj400|grv6] [-k] [-n] [-s socket] [-m midi-device] [-v] [-t txlog] [-i fifo]
//            [-L fifo]
//
// -k also sends the GRV6 its "host here" message (see grv6_keepalive).
// -n sends the controller no LEDs and no VU (inputs and the mixer still work): to
// tell a controller that hangs on the MIDI it is sent from one that hangs on its own.
//
// The player's TX frames drive the controller's LEDs (see "LEDs" below); -t
// logs every byte that changes in them, with the MIDI input interleaved.
//
// -i FIFO takes MIDI as text (hex bytes, e.g. "96 41 7F" then "96 41 00")
// and handles it as if it came from the controller: drives the player from a
// script while testing. A line starting with "o" goes to the controller
// instead (e.g. "o F0 00 40 05 ... F7"); "s DEV BYTE VALUE" (hex) sets a
// byte of an RX frame (DEV 0 ErP, 1 deck R, 2 mixer, 3 deck L); "b DEV BYTE
// BIT VALUE" (hex) sets or clears one bit of it (the on-screen buttons of
// strip/azstrip.py press the AZ's browse buttons this way, through aztouch);
// "h" goes back to the waveform screen (presses the open view's button, see
// "views").
//
// -L FIFO: the on-screen buttons' (strip/azstrip.py), told the open views
// ("L mask", bit i = views[i]) so the open one's button is lit.
//
// SHIFT + SMART FADER (GRV6: SHIFT + BACK) held RETURN_HOLD_S asks the Pi's
// boot menu to come back (writes RETURN_REQUEST; rblaunch-return.path does the
// rest), the same combo as on rekordbox.
//
// Build: gcc -O2 -Wall -o azbridge azbridge.c -lasound -lm

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <math.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TICK_MS		10
#define MC_SHM		"/run/xdjaz/azmix-mc"
#define MAXCONN		8
#define RETURN_REQUEST	"/tmp/rblaunch.request"
#define RETURN_HOLD_S	2.0

// ---- frames ---------------------------------------------------------------

enum { DEV_ERP, DEV_DECK_R, DEV_MC, DEV_DECK_L, NDEV };

static const struct {
	const char *name;
	int size, crc_at;	// RX frame size; CRC covers bytes 0..crc_at-1
	uint16_t version;	// BCD, must be non-zero
} devs[NDEV] = {
	[DEV_ERP]    = { "subucom_spi1.0", 128, 28, 0x0100 },
	[DEV_DECK_R] = { "subucom_spi2.0", 152, 38, 0x0102 },
	[DEV_MC]     = { "subucom_spi3.0", 128, 96, 0x0103 },
	[DEV_DECK_L] = { "subucom_spi5.0", 152, 38, 0x0102 },
};

static uint8_t frame[NDEV][152];
static int dirty[NDEV];

// CRC-16/X-25: reflected 0x8408, init 0xFFFF, inverted; stored little-endian
static uint16_t crc16(const uint8_t *p, int n)
{
	uint16_t crc = 0xFFFF;
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return ~crc;
}

static void set_bit(int dev, int byte, int bit, int on)
{
	uint8_t old = frame[dev][byte];
	if (on)
		frame[dev][byte] |= 1 << bit;
	else
		frame[dev][byte] &= ~(1 << bit);
	dirty[dev] |= frame[dev][byte] != old;
}

// Deck tempo: 10 bits as (b[at+1] & 3) << 8 | b[at].
static void set_deck10(int dev, int at, unsigned v)
{
	frame[dev][at] = v & 0xFF;
	frame[dev][at + 1] = (v >> 8) & 3;
	dirty[dev] = 1;
}

// Mixer analog: high 8 bits at hi, low 2 bits at lo >> shift.
static void set_mc10(int hi, int lo, int shift, unsigned v)
{
	frame[DEV_MC][hi] = v >> 2;
	frame[DEV_MC][lo] = (frame[DEV_MC][lo] & ~(3 << shift)) | (v & 3) << shift;
	dirty[DEV_MC] = 1;
}

static void init_frames(void)
{
	for (int d = 0; d < NDEV; d++) {
		frame[d][0] = devs[d].version & 0xFF;
		frame[d][1] = devs[d].version >> 8;
	}
	frame[DEV_MC][2] = 0x02, frame[DEV_MC][3] = 0x01;	// MPNL 1.02
	frame[DEV_ERP][24] = 1;		// power switch on
	frame[DEV_MC][102] = 0xFF;	// COLOR FX: left to the buttons (MC_CFX)
	for (int d = DEV_DECK_R; d <= DEV_DECK_L; d += DEV_DECK_L - DEV_DECK_R) {
		frame[d][12] = frame[d][13] = 0xFF;	// jog stopped
		set_deck10(d, 16, 512);			// tempo at the centre
		set_deck10(d, 18, 512);			// its reference
	}
	// Knobs the controller has not reported yet start at their centre (its
	// positions are only known once moved). Faders stay at 0.
	set_mc10(15, 16, 0, 512);			// crossfader centre
	for (int ch = 0; ch < 4; ch++)			// COLOR: centre = no effect
		set_mc10(61 + ch, 65, 6 - 2 * ch, 512);
	set_mc10(69, 70, 0, 512);			// beat FX level/depth
	for (int ch = 0; ch < 4; ch++)			// trim and EQs at 12 o'clock
		for (int k = 0; k < 4; k++)
			set_mc10(36 + 5 * ch + k, 40 + 5 * ch, 6 - 2 * k, 512);
	for (int k = 1; k < 4; k++)			// master EQ flat
		set_mc10(56 + k, 60, 6 - 2 * k, 512);
	set_mc10(56, 60, 6, 1023);	// master level, headphones level: the FLX4
	set_mc10(77, 80, 4, 1023);	// has its own knobs for these
	set_mc10(76, 80, 6, 0);		// headphones mixing: all CUE
}

// The mixer frame, shared with azmix. Kept (not unlinked) across restarts so
// azmix's mapping stays valid.
static uint8_t *mc_shm;

static void mc_shm_open(void)
{
	int fd = open(MC_SHM, O_RDWR | O_CREAT, 0644);
	if (fd < 0 || ftruncate(fd, 128) < 0) {
		perror("azbridge: " MC_SHM);
		if (fd >= 0)
			close(fd);
		return;
	}
	void *p = mmap(NULL, 128, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p != MAP_FAILED)
		mc_shm = p;
}

// ---- jog --------------------------------------------------------------------

// The AZ jog counts 3240 per turn and reports its speed as a period, where
// 555.56 / period = 1.0 at 33 1/3 rpm: microseconds per count.
#define AZ_JOG_COUNTS	3240

// The speed is measured over at least JOG_WINDOW_S: the GRV6 sends about 1800
// messages a turn, and a speed per message jumps between neighbours
// (measured on the unit).
#define JOG_WINDOW_S	.008

struct jog {
	int dev;
	double pos;		// in AZ counts
	double rate;		// counts per second, smoothed
	double last;		// time of the last movement
	double win, acc;	// start of the speed window, counts in it
	int touch;
};

static struct jog jogs[2] = { { DEV_DECK_L }, { DEV_DECK_R } };	// per side

// Deck layers (GRV6): the side's live layer, 0 for DECK 1/2 and 1 for 3/4.
static int layer[2];
// Turns of each deck's record, for the GRV6's jog indicator (see there).
static double jog_rev[4];

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void jog_write(struct jog *j)
{
	uint8_t *f = frame[j->dev];
	uint16_t p = (uint16_t)(long)j->pos;
	f[10] = p & 0xFF, f[11] = p >> 8;
	unsigned period = j->rate > 1 ? 1e6 / j->rate : 0xFFFF;
	if (period > 0xFFFE)
		period = 0xFFFF;
	f[12] = period & 0xFF, f[13] = period >> 8;
	dirty[j->dev] = 1;
}

static void jog_move(struct jog *j, double counts)
{
	double t = now();
	// From rest the window starts now: started at the last move (maybe
	// seconds ago), the first speed came out near 0 and the smoothing took
	// ~100 ms to catch up, so the player (loop OUT adjust goes by the speed)
	// lagged at every new turn. The first window's speed is taken as is.
	if (t - j->last > 0.05)
		j->win = t, j->acc = 0;
	j->acc += counts < 0 ? -counts : counts;
	double dt = t - j->win;
	if (dt >= JOG_WINDOW_S) {
		double r = j->acc / dt;
		j->rate = j->rate == 0 ? r : 0.7 * j->rate + 0.3 * r;
		j->win = t, j->acc = 0;
	}
	j->last = t;
	j->pos += counts;
	int side = j - jogs;
	jog_rev[side + 2 * layer[side]] += counts / AZ_JOG_COUNTS;
	set_bit(j->dev, 9, 2, counts > 0);	// direction: 1 = forward
	jog_write(j);
}

static void jog_idle(struct jog *j)
{
	if (j->rate > 0 && now() - j->last > 0.04) {
		j->rate = 0;
		jog_write(j);
	}
}

// ---- controller map ---------------------------------------------------------

enum kind {
	BUTTON,		// note on/off -> bit
	FADER14,	// 14-bit CC (MSB at cc, LSB at cc + 0x20) -> 10-bit value
	JOG,		// relative CC around 64 -> jog counts
	JOGTOUCH,	// note -> jog touch
	ENCODER,	// relative CC (1 = +1, 127 = -1) -> s16 counter
	FX_SELECT,	// BEAT FX select: type by c (see "Beat FX")
	FX_BEAT,	// BEAT < / >: beat by c
	LOOP4,		// 4 BEAT/EXIT on deck a: exits a running loop, else a 4-beat loop
	CFX_PRESS,	// SMART CFX: press the selected COLOR FX button
	CFX_NEXT,	// SHIFT + SMART CFX: select the next one and press it
	FX_TYPE,	// GRV6 Beat FX knob position: type c (absolute)
	FX_CH,		// GRV6 FX channel button: target c (-1 master), Note On only
	STEP,		// note -> counter at (dev, a) += c (browse tilt)
	LAYER,		// GRV6 note 60: deck a's layer is live (see "Deck layers")
	BRAKE,		// held: azmix brakes mixer channel a (MC_BRAKE)
	HOME,		// back to the waveform screen (go_home)
};

enum target { T_BIT, T_DECKTEMPO, T_MC10 };

struct ctl {
	uint8_t status;		// 0x9n note / 0xBn CC, n = MIDI channel
	uint8_t num;
	enum kind kind;
	int dev, a, b, c;	// target: bit (dev, byte a, bit b) / deck10 at a /
				// mc10 (hi a, lo b, shift c) / jog index a
	int invert;		// faders: 1023 - v
};

// AZ ErP deck block offsets: left deck at 8, right at 16.
#define ERP(deck, byte, bit)	BUTTON, DEV_ERP, (deck ? 16 : 8) + (byte), (bit)

// DDJ-FLX4 (same MIDI layout as the DDJ-400): deck n on channel n, pads on
// channel 7 + 2n (shifted 8 + 2n), browse and crossfader on channel 6, Beat FX
// on 4 and 5. Numbers measured on the unit.
#define FLX4_DECK(n, dev)						\
	{ 0x90 + n, 0x0B, ERP(n, 2, 1) },	/* PLAY/PAUSE */	\
	{ 0x90 + n, 0x0C, ERP(n, 2, 2) },	/* CUE */		\
	{ 0x90 + n, 0x3F, ERP(n, 1, 7) },	/* SHIFT */		\
	{ 0x90 + n, 0x58, ERP(n, 0, 1) },	/* BEAT SYNC */		\
	{ 0x90 + n, 0x10, ERP(n, 0, 6) },	/* LOOP IN */		\
	{ 0x90 + n, 0x11, ERP(n, 0, 5) },	/* LOOP OUT */		\
	{ 0x90 + n, 0x4D, LOOP4, 0, n },	/* 4 BEAT/EXIT */	\
	{ 0x90 + n, 0x50, ERP(n, 1, 3) },	/* SHIFT + it: RELOOP/EXIT */ \
	{ 0x90 + n, 0x5C, ERP(n, 0, 4) },	/* SYNC long: MASTER */	\
	{ 0x90 + n, 0x60, ERP(n, 1, 4) },	/* SHIFT + SYNC: TEMPO RANGE */ \
	{ 0x90 + n, 0x51, ERP(n, 4, 5) },	/* CUE/LOOP CALL < */	\
	{ 0x90 + n, 0x53, ERP(n, 4, 4) },	/* CUE/LOOP CALL > */	\
	{ 0x90 + n, 0x3D, ERP(n, 4, 7) },	/* SHIFT + CALL >: MEMORY */ \
	{ 0x90 + n, 0x3E, BUTTON, DEV_MC, 33, 7 },	/* SHIFT + CALL <: BACK */ \
	{ 0x90 + n, 0x1B, ERP(n, 5, 0) },	/* pad mode HOT CUE */	\
	{ 0x90 + n, 0x1E, ERP(n, 5, 1) },	/* PAD FX1 -> BEAT LOOP */ \
	{ 0x90 + n, 0x20, ERP(n, 5, 3) },	/* BEAT JUMP */		\
	{ 0x90 + n, 0x22, ERP(n, 5, 2) },	/* SAMPLER -> SLIP LOOP */ \
	/* with SHIFT the same, but SHIFT + BEAT JUMP is printed BEAT LOOP */ \
	{ 0x90 + n, 0x69, ERP(n, 5, 0) },	/* SHIFT + HOT CUE */	\
	{ 0x90 + n, 0x6B, ERP(n, 5, 1) },	/* SHIFT + PAD FX1 */	\
	{ 0x90 + n, 0x6D, ERP(n, 5, 1) },	/* SHIFT + BEAT JUMP: BEAT LOOP */ \
	{ 0x90 + n, 0x6F, ERP(n, 5, 2) },	/* SHIFT + SAMPLER */	\
	{ 0xB0 + n, 0x00, FADER14, dev, 16, 0, 0, 0 },	/* TEMPO */	\
	{ 0xB0 + n, 0x21, JOG, 0, n, 0, 0 },	/* jog ring: bend */	\
	{ 0xB0 + n, 0x22, JOG, 0, n, 0, 0 },	/* platter, vinyl on */	\
	{ 0xB0 + n, 0x23, JOG, 0, n, 0, 0 },	/* platter, vinyl off */ \
	{ 0xB0 + n, 0x29, JOG, 0, n, 0, 0 },	/* SHIFT + platter */	\
	{ 0x90 + n, 0x36, JOGTOUCH, 0, n, 0, 0 },			\
	{ 0x90 + n, 0x67, JOGTOUCH, 0, n, 0, 0 },	/* shifted */	\
	STRIP(n)

// Mixer channel n + 1, on MIDI channel n (both controllers).
#define STRIP(n)							\
	{ 0xB0 + n, 0x13, FADER14, DEV_MC, 10 + n, 14, 6 - 2 * n },	\
	{ 0xB0 + n, 0x04, FADER14, DEV_MC, 36 + 5 * n, 40 + 5 * n, 6 },	\
	{ 0xB0 + n, 0x07, FADER14, DEV_MC, 37 + 5 * n, 40 + 5 * n, 4 },	\
	{ 0xB0 + n, 0x0B, FADER14, DEV_MC, 38 + 5 * n, 40 + 5 * n, 2 },	\
	{ 0xB0 + n, 0x0F, FADER14, DEV_MC, 39 + 5 * n, 40 + 5 * n, 0 },	\
	{ 0x90 + n, 0x54, BUTTON, DEV_MC, 20, 3 - n }	/* headphone CUE */

// MIC LEVEL for azmix: 10 bits at MC bytes 98/99 (bits 6-7), after the CRC
// (96..97) like the private bytes 120..127; the AZ's own MIC LEVEL is analog.
#define MC_MIC		98
#define MIC_LEVEL_0DB	324	// 1023 / sqrt(10): azmix's gain 1 (see above)

static const struct ctl flx4[] = {
	FLX4_DECK(0, DEV_DECK_L),
	{ 0xB6, 0x05, FADER14, DEV_MC, MC_MIC, MC_MIC + 1, 6 },	// MIC LEVEL
	FLX4_DECK(1, DEV_DECK_R),
	{ 0xB6, 0x1F, FADER14, DEV_MC, 15, 16, 0 },	// crossfader
	{ 0xB6, 0x17, FADER14, DEV_MC, 61, 65, 6 },	// CFX ch1 -> COLOR ch1
	{ 0xB6, 0x18, FADER14, DEV_MC, 62, 65, 4 },	// CFX ch2 -> COLOR ch2
	{ 0xB6, 0x0C, FADER14, DEV_MC, 76, 80, 6 },	// HEADPHONES MIX
	{ 0xB6, 0x0D, FADER14, DEV_MC, 77, 80, 4 },	// HEADPHONES LEVEL
	{ 0x96, 0x63, BUTTON, DEV_MC, 21, 6 },		// MASTER CUE
	{ 0x90, 0x68, BUTTON, DEV_MC, 24, 4 },		// SHIFT + CUE (channel 1): MIXER SETTINGS
	{ 0x96, 0x00, CFX_PRESS },			// SMART CFX
	{ 0x96, 0x08, CFX_NEXT },			// SHIFT + SMART CFX
	// Beat FX: the FLX4 sends its section on channel 4 and/or 5 (one leg per
	// target channel; this unit was seen on 4 only); the channel lever is
	// handled ahead of the table (see "Beat FX")
	{ 0xB4, 0x02, FADER14, DEV_MC, 69, 70, 0 },	// BEAT FX LEVEL/DEPTH
	{ 0xB5, 0x02, FADER14, DEV_MC, 69, 70, 0 },
	{ 0x94, 0x47, BUTTON, DEV_MC, 22, 0 },		// BEAT FX ON/OFF
	{ 0x95, 0x47, BUTTON, DEV_MC, 22, 0 },
	{ 0x94, 0x63, FX_SELECT, 0, 0, 0, 1 },		// FX SELECT: next
	{ 0x95, 0x63, FX_SELECT, 0, 0, 0, 1 },
	{ 0x94, 0x64, FX_SELECT, 0, 0, 0, -1 },		// SHIFT + FX SELECT: previous
	{ 0x95, 0x64, FX_SELECT, 0, 0, 0, -1 },
	{ 0x94, 0x4A, FX_BEAT, 0, 0, 0, -1 },		// BEAT <
	{ 0x95, 0x4A, FX_BEAT, 0, 0, 0, -1 },
	{ 0x94, 0x4B, FX_BEAT, 0, 0, 0, 1 },		// BEAT >
	{ 0x95, 0x4B, FX_BEAT, 0, 0, 0, 1 },
	{ 0xB6, 0x40, ENCODER, DEV_MC, 34 },		// browse rotary
	{ 0xB6, 0x64, ENCODER, DEV_MC, 34 },		// SHIFT + browse rotary
	{ 0x96, 0x41, BUTTON, DEV_MC, 31, 0 },		// browse push
	{ 0x96, 0x42, BUTTON, DEV_MC, 33, 7 },		// shift + push: BACK
	{ 0x96, 0x01, BUTTON, DEV_MC, 32, 7 },		// SMART FADER: SOURCE (as on rekordbox)
	// No FLX4 control sends this one; for scripts through -i (the FLX4 has
	// no BROWSE button).
	{ 0x96, 0x7D, BUTTON, DEV_MC, 32, 6 },		// BROWSE
	{ 0x96, 0x46, BUTTON, DEV_MC, 33, 3 },		// LOAD deck 1
	{ 0x96, 0x47, BUTTON, DEV_MC, 33, 2 },		// LOAD deck 2
	{ 0x96, 0x68, BUTTON, DEV_MC, 33, 5 },		// SHIFT + LOAD 1: TRACK FILTER
	{ 0x96, 0x7A, BUTTON, DEV_MC, 33, 4 },		// SHIFT + LOAD 2: SHORTCUT
};

// DDJ-GRV6: the FLX4's layout with four decks (layers 3 and 4 of the two
// sides on channels 2 and 3), measured on the unit. Deck n plays on side n & 1. Differences that matter here: BEAT SYNC
// has no long-press note (SHIFT + SYNC is MASTER), MASTER TEMPO, KEY SYNC,
// SLIP, QUANTIZE and MEMORY are buttons of their own, the Beat FX knob sends
// one note per position and the FX channel is six radio buttons. Pad modes
// as on the FLX4 (STEMS where the FLX4 has PAD FX1). QUANTIZE is one
// setting for all decks in the player, which reads it from the left side
// only (the right one's bit does nothing): every deck's button presses that.
// The DRUM RELEASE lever brakes its deck's channel while tilted (azmix does
// it: the AZ has no VINYL BRAKE effect).
#define SIDE(n)	((n) & 1)
#define GRV6_DECK(n, dev)						\
	{ 0x90 + n, 0x0B, ERP(SIDE(n), 2, 1) },	/* PLAY/PAUSE */	\
	{ 0x90 + n, 0x47, ERP(SIDE(n), 2, 1) },	/* SHIFT + PLAY */	\
	{ 0x90 + n, 0x0C, ERP(SIDE(n), 2, 2) },	/* CUE */		\
	{ 0x90 + n, 0x48, ERP(SIDE(n), 2, 2) },	/* SHIFT + CUE */	\
	{ 0x90 + n, 0x3F, ERP(SIDE(n), 1, 7) },	/* SHIFT */		\
	{ 0x90 + n, 0x58, ERP(SIDE(n), 0, 1) },	/* BEAT SYNC */		\
	{ 0x90 + n, 0x5C, ERP(SIDE(n), 0, 4) },	/* SHIFT + SYNC: MASTER */ \
	{ 0x90 + n, 0x1A, ERP(SIDE(n), 0, 3) },	/* MASTER TEMPO */	\
	{ 0x90 + n, 0x60, ERP(SIDE(n), 1, 4) },	/* SHIFT + it: TEMPO RANGE */ \
	{ 0x90 + n, 0x65, ERP(SIDE(n), 1, 1) },	/* KEY SYNC */		\
	{ 0x90 + n, 0x40, ERP(SIDE(n), 0, 2) },	/* SLIP */		\
	{ 0x90 + n, 0x17, ERP(SIDE(n), 4, 1) },	/* SHIFT + SLIP (VINYL): JOG MODE */ \
	{ 0x90 + n, 0x35, ERP(0, 2, 0) },	/* QUANTIZE: see below */ \
	{ 0x90 + n, 0x10, ERP(SIDE(n), 0, 6) },	/* LOOP IN */		\
	{ 0x90 + n, 0x11, ERP(SIDE(n), 0, 5) },	/* LOOP OUT */		\
	{ 0x90 + n, 0x4D, LOOP4, 0, SIDE(n) },	/* 4 BEAT/EXIT */	\
	{ 0x90 + n, 0x50, ERP(SIDE(n), 1, 3) },	/* SHIFT + it (FLX4's): RELOOP/EXIT */ \
	{ 0x90 + n, 0x51, ERP(SIDE(n), 4, 5) },	/* CUE/LOOP CALL < */	\
	{ 0x90 + n, 0x53, ERP(SIDE(n), 4, 4) },	/* CUE/LOOP CALL > */	\
	{ 0x90 + n, 0x3D, ERP(SIDE(n), 4, 7) },	/* MEMORY */		\
	{ 0x90 + n, 0x3E, ERP(SIDE(n), 4, 6) },	/* SHIFT + MEMORY: DELETE */ \
	{ 0x90 + n, 0x1B, ERP(SIDE(n), 5, 0) },	/* HOT CUE */		\
	{ 0x90 + n, 0x1E, ERP(SIDE(n), 5, 1) },	/* STEMS -> BEAT LOOP */ \
	{ 0x90 + n, 0x20, ERP(SIDE(n), 5, 3) },	/* BEAT JUMP */		\
	{ 0x90 + n, 0x22, ERP(SIDE(n), 5, 2) },	/* SAMPLER -> SLIP LOOP */ \
	{ 0x90 + n, 0x69, ERP(SIDE(n), 5, 0) },	/* SHIFT: KEYBOARD */	\
	{ 0x90 + n, 0x6B, ERP(SIDE(n), 5, 1) },	/* SHIFT: PAD FX */	\
	{ 0x90 + n, 0x6D, ERP(SIDE(n), 5, 1) },	/* SHIFT: BEAT LOOP */	\
	{ 0x90 + n, 0x6F, ERP(SIDE(n), 5, 2) },	/* SHIFT: KEY SHIFT */	\
	{ 0xB0 + n, 0x00, FADER14, dev, 16, 0, 0, 0 },	/* TEMPO */	\
	{ 0xB0 + n, 0x21, JOG, 0, SIDE(n), 0, 0 },	/* jog ring */	\
	{ 0xB0 + n, 0x22, JOG, 0, SIDE(n), 0, 0 },	/* platter */	\
	{ 0xB0 + n, 0x23, JOG, 0, SIDE(n), 0, 0 },			\
	{ 0xB0 + n, 0x29, JOG, 0, SIDE(n), 0, 0 },	/* SHIFT + platter */ \
	{ 0x90 + n, 0x36, JOGTOUCH, 0, SIDE(n), 0, 0 },			\
	{ 0x90 + n, 0x67, JOGTOUCH, 0, SIDE(n), 0, 0 },			\
	{ 0x90 + n, 0x3C, LAYER, 0, n },	/* this deck's layer is live */ \
	{ 0x90 + n, 0x24, BRAKE, 0, n },	/* DRUM RELEASE lever */ \
	STRIP(n)

// The GRV6 Beat FX knob, left to right, as the AZ's effect numbers (see "Beat
// FX"): by name, and the four the AZ has not got as their nearest kin.
#define FXK(note, type)	{ 0x94, note, FX_TYPE, 0, 0, 0, type }
static const struct ctl grv6[] = {
	GRV6_DECK(0, DEV_DECK_L),
	GRV6_DECK(1, DEV_DECK_R),
	GRV6_DECK(2, DEV_DECK_L),
	GRV6_DECK(3, DEV_DECK_R),
	{ 0xB6, 0x1F, FADER14, DEV_MC, 15, 16, 0 },	// crossfader
	{ 0xB6, 0x17, FADER14, DEV_MC, 61, 65, 6 },	// COLOR FX knobs ch1..4
	{ 0xB6, 0x18, FADER14, DEV_MC, 62, 65, 4 },
	{ 0xB6, 0x19, FADER14, DEV_MC, 63, 65, 2 },
	{ 0xB6, 0x1A, FADER14, DEV_MC, 64, 65, 0 },
	// MASTER CUE, HEADPHONES MIX and LEVEL work inside the GRV6, so its phones
	// get the cue bus alone (init_frames keeps MIX at CUE and LEVEL up)
	{ 0x96, 0x00, CFX_PRESS },			// SOUND COLOR FX ON/OFF
	{ 0x96, 0x08, CFX_NEXT },			// SHIFT + it: the next one
	{ 0xB4, 0x02, FADER14, DEV_MC, 69, 70, 0 },	// BEAT FX LEVEL/DEPTH
	{ 0x94, 0x47, BUTTON, DEV_MC, 22, 0 },		// BEAT FX ON/OFF
	{ 0x94, 0x4A, FX_BEAT, 0, 0, 0, -1 },		// BEAT <
	{ 0x94, 0x4B, FX_BEAT, 0, 0, 0, 1 },		// BEAT >
	FXK(0x20, 0), FXK(0x21, 1),			// DELAY, ECHO
	FXK(0x22, 2),					// LC ECHO -> PING PONG
	FXK(0x23, 3), FXK(0x24, 4), FXK(0x25, 5),	// SPIRAL, HELIX, REVERB
	FXK(0x26, 6), FXK(0x27, 7), FXK(0x28, 8),	// FLANGER, PHASER, FILTER
	FXK(0x29, 10),					// TRANS
	FXK(0x2A, 9),					// PITCH -> TRIPLET FILTER
	FXK(0x2B, 11),					// ROLL
	FXK(0x2C, 13),					// MOBIUS (saw) -> MOBIUS
	FXK(0x2D, 12),					// MOBIUS (tri) -> TRIPLET ROLL
	{ 0x94, 0x10, FX_CH, 0, 0, 0, 0 },		// FX channel 1..4 and MST
	{ 0x94, 0x11, FX_CH, 0, 0, 0, 1 },		// (SP, the sampler: the AZ
	{ 0x94, 0x12, FX_CH, 0, 0, 0, 2 },		// has none)
	{ 0x94, 0x13, FX_CH, 0, 0, 0, 3 },
	{ 0x94, 0x14, FX_CH, 0, 0, 0, -1 },
	{ 0xB6, 0x40, ENCODER, DEV_MC, 34 },		// browse rotary
	{ 0xB6, 0x64, ENCODER, DEV_MC, 34 },		// SHIFT + browse rotary
	{ 0x96, 0x38, STEP, DEV_MC, 34, 0, -1 },	// tilt UP
	{ 0x96, 0x3A, STEP, DEV_MC, 34, 0, 1 },		// tilt DOWN
	{ 0x96, 0x39, STEP, DEV_MC, 34, 0, -10 },	// SHIFT + UP: a page
	{ 0x96, 0x3B, STEP, DEV_MC, 34, 0, 10 },	// SHIFT + DOWN
	{ 0x96, 0x3C, BUTTON, DEV_MC, 33, 7 },		// tilt LEFT: BACK
	{ 0x96, 0x3D, BUTTON, DEV_MC, 33, 7 },		// (and with SHIFT)
	{ 0x96, 0x3E, BUTTON, DEV_MC, 31, 0 },		// tilt RIGHT: as the push
	{ 0x96, 0x3F, BUTTON, DEV_MC, 31, 0 },
	{ 0x96, 0x41, BUTTON, DEV_MC, 31, 0 },		// browse push
	{ 0x96, 0x42, BUTTON, DEV_MC, 33, 7 },		// SHIFT + push: BACK
	{ 0x96, 0x65, BUTTON, DEV_MC, 33, 7 },		// BACK
	{ 0x96, 0x7A, BUTTON, DEV_MC, 32, 6 },		// VIEW: BROWSE
	{ 0x96, 0x35, BUTTON, DEV_MC, 32, 7 },		// DISCOVER: SOURCE
	{ 0x96, 0x36, BUTTON, DEV_MC, 32, 3 },		// PREVIEW: SEARCH (bar + keyboard)
	// SHIFT + the browse-area buttons, measured 2026-11-09. The AZ has no
	// Related Tracks (SHIFT + DISCOVER): TRACK FILTER (its Related Key) instead;
	// SHIFT + PREVIEW (">>", rekordbox: preview +30 s) has no AZ counterpart
	// (the AZ seeks a preview by touching its waveform): HOME.
	{ 0x96, 0x67, BUTTON, DEV_MC, 33, 6 },		// SHIFT + VIEW: TAG TRACK
	{ 0x96, 0x66, BUTTON, DEV_MC, 32, 4 },		// SHIFT + BACK: PLAYLIST (palette)
	{ 0x96, 0x68, BUTTON, DEV_MC, 33, 5 },		// SHIFT + DISCOVER: TRACK FILTER
	{ 0x96, 0x37, HOME },				// SHIFT + PREVIEW: HOME
	{ 0x96, 0x46, BUTTON, DEV_MC, 33, 3 },		// LOAD left side
	{ 0x96, 0x47, BUTTON, DEV_MC, 33, 2 },		// LOAD right side
	{ 0x96, 0x58, BUTTON, DEV_MC, 33, 5 },		// SHIFT + LOAD 1: TRACK FILTER
	{ 0x96, 0x59, BUTTON, DEV_MC, 33, 4 },		// SHIFT + LOAD 2: SHORTCUT
	{ 0x96, 0x48, BUTTON, DEV_MC, 33, 1 },		// LOAD deck 3
	{ 0x96, 0x49, BUTTON, DEV_MC, 33, 0 },		// LOAD deck 4
};

// The FLX4 wants this vendor SysEx every 200 ms from its host (as Mixxx
// found); without it it may idle.
static const uint8_t flx4_keepalive[] = {
	0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x05, 0x00, 0x50, 0x02, 0xF7
};
#define KEEPALIVE_MS	200

// Asks the FLX4 for the position of all its knobs and faders; it answers with
// their CCs (found by trying the vendor's request family on the unit,
// 2026-11-06). Sent on open, so the player starts with the real positions.
static const uint8_t flx4_query[] = {
	0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x05, 0x00, 0x03, 0x01, 0xF7
};
// The GRV6 answers the same request with its model byte, 0x0C (found by
// trying them, 2026-11-06): every knob and fader, the live layer of each side
// (note 60), the Beat FX knob's position and the FX channel button.
// The GRV6's own "host here" message, from rekordbox's Android library
// (librekordbox.so keeps the vendor SysEx of each model side by side; 0x50 0x03
// for model 0x0C). Sent only with -k: the user keeps the GRV6's Auto Standby
// (it drops off USB after about half an hour untouched, and run-probe.sh puts
// the player to sleep with it). Whether this message holds it off is not
// tested yet.
// FLX4 MIC INPUT setting (rekordbox: MicInputSetting, its "LowLatencyMode"
// or "EffectMode"): 0x00 mixes the mic in the unit, 0x7F leaves it out and
// only sends it over USB (tried on the unit, 2026-11-06). azmix mixes it,
// with the Beat FX on the master.
static const uint8_t flx4_init[] = { 0x9F, 0x4A, 0x7F };

// The DDJ-400 answers the same request (model bytes 0x02 0x06, as Mixxx's own
// DDJ-400 script sends it on startup) with the position of every knob and
// fader. Without it the mixer's channel faders start at 0 and the sound stays
// silent until each one is moved.
static const uint8_t ddj400_query[] = {
	0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x02, 0x06, 0x00, 0x03, 0x01, 0xF7
};

static const uint8_t grv6_keepalive[] = {
	0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x0C, 0x00, 0x50, 0x03, 0xF7
};
static const uint8_t grv6_query[] = {
	0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x0C, 0x00, 0x03, 0x01, 0xF7
};

struct ctlr {
	const char *name, *card;
	const struct ctl *map;
	int nmap;
	int decks;		// 2, or 4 in two layers
	int nch;		// mixer channels (CUE LEDs, meters)
	int jog_ticks;		// per turn
	uint8_t return_note;	// on channel 6: held -> the boot menu
	const uint8_t *keepalive, *query;
	int keepalive_n;	// the query is 12 bytes
	int keepalive_opt;	// sent only with -k
	const uint8_t *init;	// sent once on open, after the query
	int init_n;
	int led_ms;		// resend every LED this often (0: LED_MS)
};

#define NROWS(t)	(int)(sizeof(t) / sizeof((t)[0]))
static const struct ctlr ctlrs[] = {
	// FLX4 jog: 720 ticks per turn (as for the DDJ-400); SHIFT + SMART FADER
	{ "flx4", "DDJFLX4", flx4, NROWS(flx4), 2, 2, 720, 0x09,
	  flx4_keepalive, flx4_query, sizeof(flx4_keepalive), 0,
	  flx4_init, sizeof(flx4_init) },
	// DDJ-400: the FLX4's MIDI layout (see FLX4_DECK), so the same table. Only
	// the position query is known for this unit (no keepalive, no init). No mic.
	// Jog 720 ticks a turn; 0x09 as for the FLX4. LEDs are resent once a second
	// (the FLX4's 100 ms is ~1.5 kB/s of MIDI, which seems to wedge this unit's
	// MIDI input: its Tx counter stopped at 831 bytes). Untested on the unit.
	{ "ddj400", "DDJ400", flx4, NROWS(flx4), 2, 2, 720, 0x09, NULL, ddj400_query,
	  .led_ms = 1000 },
	// GRV6 jog: 6449 a turn (measured on the unit); SHIFT + BACK
	{ "grv6", "DDJGRV6", grv6, NROWS(grv6), 4, 4, 6449, 0x66,
	  grv6_keepalive, grv6_query, sizeof(grv6_keepalive), 1 },
};
static const struct ctlr *cur = &ctlrs[0];
#define MAXROWS		256	// the largest map's rows

static int verbose;
static FILE *txlog;

// ---- LEDs -------------------------------------------------------------------
//
// The deck LEDs come from the player's ErP TX frame, read out of EP147 1.30's
// builder (erp_ucom::TxDataBuilder, 0x2148B78) and its indicator set
// (HuiIndicatorSet::Deck, 0x214F600; names from their HUI ids). One 56-byte
// block per deck, left deck at 8, right at 64:
//   +0  slip, masterTempo, master, slipRev      2 bits each, from bit 0
//   +1  reloopExit, keySync, tempoReset, beatSync
//   +2  -, quantize, loopOut, loopIn
//   +3  playPause, cue, (jog)
//   +4  deckSelect 1, deckSelect 2, jogMode, vinylSpeedAdjust
//   +6, +10, +14, +18  RGB of the pad mode buttons HOT CUE, BEAT LOOP,
//                      SLIP LOOP, BEAT JUMP
//   +22 + 4k           RGB of pad k (k = 0..7)
// A 2-bit LED is 0 off, 1 dim, 2 lit; the player blinks one by toggling it.
// The FLX4 has no dim and no colour: lit means 2 or more, and an RGB
// counts as lit when one component is bright (unset pads and the modes not
// selected are dim colours).
//
// The mixer has no LED of the controller's in the player's frames (the AZ's
// mixer is hardware: headphone CUE, BEAT FX ON and the COLOR FX are latched
// there), so those follow the bridge's own idea of their state.
//
// FLX4 LEDs are its controls' own input notes, velocity 0x7F lit and 0 dark,
// and it wants them repeated (measured on the unit: a single message is
// often missed), so the whole set is resent every LED_MS.
#define LED_MS		100
#define ERP_BLOCK(deck)	(8 + 56 * (deck))

struct led { uint8_t status, note; };

//
// The GRV6 takes the same notes, on the channel of the side's live layer
// (deck 3's PLAY on channel 2). It lights its pad mode buttons, MASTER
// TEMPO, MASTER CUE and the FX channel buttons by itself; it blinks BEAT FX
// ON/OFF by itself at 0x7F and shows it steady at 0 (measured on the unit).
enum {
	L_PLAY, L_CUE, L_SYNC, L_LOOPIN, L_LOOPOUT, L_SLIP, L_QUANT, L_KEYSYNC, L_MT,
	L_MODE0, L_MODE1, L_MODE2, L_MODE3,		// in AZ mode order
	L_PAD0, L_PAD7 = L_PAD0 + 7,
	L_DECK						// per side
};
enum { L_HPCUE1 = 2 * L_DECK, L_HPCUE4 = L_HPCUE1 + 3, L_MASTERCUE, L_BEATFX, L_CFX, NLED };

static uint8_t led_on[NLED], led_sent[NLED];
static int pad_base[4];		// note of pad 0 in the deck's current pad mode

// AZ pad mode (HOT CUE, BEAT LOOP, SLIP LOOP, BEAT JUMP) -> FLX4 button that
// selects it (see FLX4_DECK) and lights for it.
static const uint8_t mode_note[4] = { 0x1B, 0x1E, 0x22, 0x20 };
// The pad mode buttons, plain and with SHIFT, and the note of pad 1 in that
// mode (FLX4: HOT CUE, PAD FX1, BEAT JUMP, SAMPLER, KEYBOARD, PAD FX2, BEAT
// LOOP, KEY SHIFT; the GRV6 the same with STEMS for PAD FX1).
static const uint8_t flx4_mode_note[8] = { 0x1B, 0x1E, 0x20, 0x22, 0x69, 0x6B, 0x6D, 0x6F };

// A LED's message, status 0 for none.
static struct led led_addr(int i)
{
	int grv6 = cur->decks == 4;
	if (i >= L_HPCUE1 && i <= L_HPCUE4) {
		int ch = i - L_HPCUE1;
		return ch < cur->nch ? (struct led){ 0x90 + ch, 0x54 } : (struct led){ 0 };
	}
	if (i >= L_MASTERCUE) {
		static const struct led mixer[] = { { 0x96, 0x63 }, { 0x94, 0x47 }, { 0x96, 0x00 } };
		if (grv6 && i == L_MASTERCUE)
			return (struct led){ 0 };
		return mixer[i - L_MASTERCUE];
	}
	int side = i / L_DECK, k = i % L_DECK, deck = side + 2 * layer[side];
	static const uint8_t note[] = { 0x0B, 0x0C, 0x58, 0x10, 0x11, 0x40, 0x35, 0x65, 0x1A };
	if (k < L_SLIP || (grv6 && k < L_MODE0))
		return (struct led){ 0x90 + deck, note[k] };
	if (k < L_MODE0 || (grv6 && k < L_PAD0))
		return (struct led){ 0 };
	if (k < L_PAD0)
		return (struct led){ 0x90 + deck, mode_note[k - L_MODE0] };
	return (struct led){ 0x97 + 2 * deck, pad_base[deck] + k - L_PAD0 };
}

static int rgb_lit(const uint8_t *c)
{
	return c[0] > 0x40 || c[1] > 0x40 || c[2] > 0x40;
}

// The tempo master (0..3, -1 none) and the player's deck on each side (0 DECK
// 1/2, 1 DECK 3/4, -1 not seen yet), from its deck TX frames (deck_tx).
static int master_deck = -1;
static int az_layer[2] = { -1, -1 };
// What the player's deck TX frames say of each deck (deck_tx, see there).
static double deck_pos[4], deck_at[4];		// playhead in s; frame seen at
static int deck_loaded[4], deck_mode[4], deck_flags[4];
#define DECK_MODE_THROUGH	0x81	// byte 24: playing through, no loop
#define DECK_ADJUST_IN		0x80	// byte 17: loop IN being adjusted
#define DECK_ADJUST_OUT		0x40	// byte 17: loop OUT being adjusted

// The deck a side plays now (0..3).
static int live_deck(int side)
{
	return side + 2 * layer[side];
}

static int deck_fresh(int d)
{
	return now() - deck_at[d] < .5;
}

// A loop runs on the side's live deck: the player's mode (byte 24) is not
// "through". (Taken from LOOP IN's blinking LED before, which was wrong while
// loop OUT was being adjusted: 4 BEAT/EXIT then halved the loop.)
static int loop_running(int side)
{
	int d = live_deck(side);
	return deck_fresh(d) && deck_loaded[d] && deck_mode[d] != DECK_MODE_THROUGH;
}

// Loop IN or OUT being adjusted with the jog on the side's live deck (byte 17,
// confirmed on the unit for both, 2026-11-08): the jog is then made JOG_FINE
// times less sensitive, for a fine fit (the user's wish). AZ_JOG_FINE sets
// the factor (1 = off).
static double jog_fine = 3;

static int loop_adjusting(int side)
{
	int d = live_deck(side);
	return deck_fresh(d) && deck_mode[d] != DECK_MODE_THROUGH &&
	       (deck_flags[d] & (DECK_ADJUST_IN | DECK_ADJUST_OUT));
}

static uint8_t sync_raw[2];	// the player's BEAT SYNC LED per deck, before the SLIP pulse

static void leds_from_erp(const uint8_t *buf, int n)
{
	if (n < ERP_BLOCK(1) + 56)
		return;
	for (int deck = 0; deck < 2; deck++) {
		const uint8_t *b = buf + ERP_BLOCK(deck);
		uint8_t *on = led_on + deck * L_DECK;
		on[L_PLAY] = (b[3] & 3) >= 2;
		on[L_CUE] = (b[3] >> 2 & 3) >= 2;
		on[L_SYNC] = (b[1] >> 6 & 3) >= 2;
		sync_raw[deck] = on[L_SYNC];
		on[L_LOOPIN] = (b[2] >> 6 & 3) >= 2;
		on[L_LOOPOUT] = (b[2] >> 4 & 3) >= 2;
		on[L_SLIP] = (b[0] & 3) >= 2;
		on[L_QUANT] = (b[2] >> 2 & 3) >= 2;
		on[L_KEYSYNC] = (b[1] >> 2 & 3) >= 2;
		on[L_MT] = (b[0] >> 2 & 3) >= 2;
		// QUANTIZE is the left side's (see grv6[])
		on[L_QUANT] = (buf[ERP_BLOCK(0) + 2] >> 2 & 3) >= 2;
		for (int m = 0; m < 4; m++)
			on[L_MODE0 + m] = rgb_lit(b + 6 + 4 * m);
		for (int k = 0; k < 8; k++)
			on[L_PAD0 + k] = rgb_lit(b + 22 + 4 * k);
	}
}

// Resend a side's deck LEDs (to new notes or a new channel).
static void leds_resend_side(int side)
{
	memset(led_sent + side * L_DECK, 0xFF, L_DECK);
}

// The controller re-addresses its pads with its pad mode buttons; the LEDs
// have to go to the notes of the mode the deck (0..3) is in.
static void leds_pad_mode(int deck, int base)
{
	if (pad_base[deck] == base)
		return;
	pad_base[deck] = base;
	leds_resend_side(deck & 1);
}

// ---- Deck layers ------------------------------------------------------------
//
// The GRV6's DECK buttons switch a side between decks 1 and 3 (left) or 2 and
// 4 (right): note 60 On on the new deck's channel says which is live
// (measured on the unit). The AZ has the same model, a deck select pair per side (ErP RX D+0
// bit 0 primary = DECK 1/2, D+1 bit 0 secondary = DECK 3/4, said in deck TX byte 0), so
// the bridge presses the AZ's one until the player's deck frames agree. A side's
// controls go to the same AZ side whichever layer is live, as on the AZ.
#define LAYER_RETRY_S	.3
#define LAYER_TRIES	3
static int layer_tries[2];
static double layer_at[2];

static void pulse(int dev, int byte, int bit);

static void layer_set(int side, int l)
{
	if (layer[side] == l)
		return;
	layer[side] = l;
	layer_tries[side] = 0;
	leds_resend_side(side);
}

static void layers_tick(void)
{
	if (cur->decks < 4)
		return;
	for (int side = 0; side < 2; side++) {
		if (az_layer[side] == layer[side] || layer_tries[side] >= LAYER_TRIES ||
		    now() - layer_at[side] < LAYER_RETRY_S)
			continue;
		pulse(DEV_ERP, (side ? 16 : 8) + layer[side], 0);
		layer_tries[side]++;
		layer_at[side] = now();
	}
}

// LED groups, to find which messages hang a controller (AZ_LEDS=core,loop,... lists the
// groups that are sent; unset: all). core = PLAY, CUE, SYNC, headphone CUE (the
// DDJ-400's own LED notes); loop = LOOP IN/OUT; extra = SLIP, QUANT, KEY SYNC, MASTER TEMPO
// (notes the DDJ-400's map does not list); modes = the pad mode buttons; pads = the 8 pads;
// mixer = MASTER CUE, BEAT FX ON, SMART CFX; vu = the channel meters.
enum { G_CORE = 1, G_LOOP = 2, G_EXTRA = 4, G_MODES = 8, G_PADS = 16, G_MIXER = 32, G_VU = 64 };
static unsigned led_groups = ~0u;

static int meter_ms = 40;	// AZ_METER_MS: gap between level meter updates
static int meter_step = 1;	// AZ_METER_STEP: a meter value is sent only if it moved this much (or to 0)
static int led_ms_over;		// AZ_LED_MS: gap between the full LED refreshes (0: the controller's)

static void led_groups_init(void)
{
	const char *m = getenv("AZ_METER_MS");
	if (m && atoi(m) >= 10) {
		meter_ms = atoi(m);
		fprintf(stderr, "azbridge: AZ_METER_MS=%d\n", meter_ms);
	}
	const char *st = getenv("AZ_METER_STEP");
	if (st && atoi(st) >= 1) {
		meter_step = atoi(st);
		fprintf(stderr, "azbridge: AZ_METER_STEP=%d\n", meter_step);
	}
	const char *lm = getenv("AZ_LED_MS");
	if (lm && atoi(lm) >= 100) {
		led_ms_over = atoi(lm);
		fprintf(stderr, "azbridge: AZ_LED_MS=%d\n", led_ms_over);
	}
	const char *e = getenv("AZ_LEDS");
	if (!e)
		return;
	led_groups = 0;
	if (strstr(e, "all")) led_groups = ~0u;
	if (strstr(e, "core")) led_groups |= G_CORE;
	if (strstr(e, "loop")) led_groups |= G_LOOP;
	if (strstr(e, "extra")) led_groups |= G_EXTRA;
	if (strstr(e, "modes")) led_groups |= G_MODES;
	if (strstr(e, "pads")) led_groups |= G_PADS;
	if (strstr(e, "mixer")) led_groups |= G_MIXER;
	if (strstr(e, "vu")) led_groups |= G_VU;
	fprintf(stderr, "azbridge: AZ_LEDS=%s (groups %#x)\n", e, led_groups);
}

static unsigned led_group(int i)
{
	if (i >= L_HPCUE1 && i <= L_HPCUE4)
		return G_CORE;
	if (i >= L_MASTERCUE)
		return G_MIXER;
	int k = i % L_DECK;
	if (k == L_PLAY || k == L_CUE || k == L_SYNC)
		return G_CORE;
	if (k == L_LOOPIN || k == L_LOOPOUT)
		return G_LOOP;
	if (k < L_MODE0)
		return G_EXTRA;
	return k < L_PAD0 ? G_MODES : G_PADS;
}

// ---- MIDI output with a trail -------------------------------------------------
//
// midi_out is non-blocking, so when the controller stops taking data (the DDJ-400
// hangs its MIDI OUT endpoint and only a cable pull recovers it) writes fail with
// EAGAIN and used to be dropped without a word. midi_write keeps the last
// TRAIL_N writes and, the first time one fails or comes up short, logs the failure
// and dumps the trail (time, bytes), then logs again every 10 s while it lasts and
// when it clears.
#define TRAIL_N	4096
#define TRAIL_RAW 60
static struct { double t; uint8_t n, b[3]; } trail[TRAIL_N];
static unsigned trail_at, midi_fail, midi_fail_total;
static double midi_fail_since, midi_fail_logged, midi_last_ok;
static unsigned long midi_bytes;

// Per second: writes, bytes, level meter messages (CC 2), the rest; then the last
// TRAIL_RAW writes as they were.
static void trail_dump(const char *why)
{
	fprintf(stderr, "azbridge: MIDI out trail (%s), up to %d writes, per second:\n", why, TRAIL_N);
	long sec = -1;
	unsigned w = 0, bytes = 0, vu = 0;
	for (unsigned i = 0; i <= TRAIL_N; i++) {
		unsigned k = (trail_at + i) % TRAIL_N;
		int end = i == TRAIL_N;
		if (!end && !trail[k].n)
			continue;
		long sc = end ? -2 : (long)trail[k].t;
		if (sc != sec) {
			if (sec >= 0)
				fprintf(stderr, "  t=%ld writes=%u bytes=%u meter=%u other=%u\n", sec, w, bytes, vu, w - vu);
			sec = sc, w = bytes = vu = 0;
		}
		if (end)
			break;
		w++;
		bytes += trail[k].n;
		vu += trail[k].n == 3 && (trail[k].b[0] & 0xF0) == 0xB0 && trail[k].b[1] == 2;
	}
	fprintf(stderr, "azbridge: last %d writes, oldest first:\n", TRAIL_RAW);
	for (unsigned i = TRAIL_N - TRAIL_RAW; i < TRAIL_N; i++) {
		unsigned k = (trail_at + i) % TRAIL_N;
		if (!trail[k].n)
			continue;
		fprintf(stderr, "  %.3f", trail[k].t);
		for (int j = 0; j < trail[k].n; j++)
			fprintf(stderr, " %02X", trail[k].b[j]);
		fputc('\n', stderr);
	}
}

// How much of the output buffer the controller has not taken yet. A controller that
// drains it keeps this near zero; one that has hung lets it grow by the write rate.
// Checked about once a second; logs (and dumps the trail once) when it stays over
// QUEUE_WARN for QUEUE_S seconds, long before the buffer is full.
#define QUEUE_WARN	512
#define QUEUE_S		3
static void midi_queue_check(snd_rawmidi_t *out, double t)
{
	static double next, over_since, logged;
	static int dumped;
	if (t < next)
		return;
	next = t + 1;
	snd_rawmidi_status_t *st;
	snd_rawmidi_params_t *pr;
	snd_rawmidi_status_alloca(&st);
	snd_rawmidi_params_alloca(&pr);
	if (snd_rawmidi_status(out, st) < 0 || snd_rawmidi_params_current(out, pr) < 0)
		return;
	size_t queued = snd_rawmidi_params_get_buffer_size(pr) - snd_rawmidi_status_get_avail(st);
	if (queued < QUEUE_WARN) {
		if (over_since)
			fprintf(stderr, "azbridge: MIDI out drained again (%zu bytes queued)\n", queued);
		over_since = 0, dumped = 0;
		return;
	}
	if (!over_since)
		over_since = t;
	if (t - over_since >= QUEUE_S && t - logged >= 10) {
		logged = t;
		fprintf(stderr, "azbridge: MIDI out NOT DRAINING: %zu bytes queued for %.0f s, %lu bytes written so far\n",
			queued, t - over_since, midi_bytes);
		if (!dumped++)
			trail_dump("not draining");
	}
}

static ssize_t midi_write(snd_rawmidi_t *out, const void *buf, size_t n)
{
	double t = now();
	const uint8_t *p = buf;
	midi_queue_check(out, t);
	ssize_t r = snd_rawmidi_write(out, buf, n);
	size_t done = r > 0 ? (size_t)r : 0;
	// remember what was handed over (a long write as its first 3 bytes)
	trail[trail_at].t = t;
	trail[trail_at].n = n < 3 ? (uint8_t)n : 3;
	memcpy(trail[trail_at].b, p, trail[trail_at].n);
	trail_at = (trail_at + 1) % TRAIL_N;
	if (done == n) {
		midi_bytes += done;
		if (midi_fail) {
			fprintf(stderr, "azbridge: MIDI out accepting data again after %.1f s (%u writes failed)\n",
				t - midi_fail_since, midi_fail);
			midi_fail = 0;
		}
		midi_last_ok = t;
		return r;
	}
	midi_bytes += done;
	midi_fail_total++;
	if (!midi_fail++) {
		midi_fail_since = t;
		midi_fail_logged = t;
		fprintf(stderr, "azbridge: MIDI out STALLED at %.3f: wrote %zd of %zu (%s), %lu bytes written so far,"
			" last full write %.1f s ago\n", t, r, n, r < 0 ? snd_strerror((int)r) : "short",
			midi_bytes, midi_last_ok ? t - midi_last_ok : -1.0);
		trail_dump("first failure");
	} else if (t - midi_fail_logged >= 10) {
		midi_fail_logged = t;
		fprintf(stderr, "azbridge: MIDI out still stalled: %.1f s, %u writes failed\n",
			t - midi_fail_since, midi_fail);
	}
	return r;
}

// CH LEVEL meters: CC 2 on the deck channel, a level ramp (0x26..0x40 green,
// ..0x56 green 2, ..0x64 orange, ..0x76 orange 2, 0x77.. red; measured on
// the unit, also in rekordbox's GRV6 table), from azmix's
// peak levels. Each segment lights from a level in dBFS (meter_db): red only
// within half a dB of clipping, the oranges for a loud track's peaks.
#define METER_SHM	"/run/xdjaz/azmix-meter"
#define METER_MS	40
static const float *meter;
static int meter_sent[4] = { -1, -1, -1, -1 };

static void meters_send(snd_rawmidi_t *out)
{
	if (!(led_groups & G_VU))
		return;
	if (!meter) {
		int fd = open(METER_SHM, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return;
		void *p = mmap(NULL, 4 * sizeof(float), PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if (p == MAP_FAILED)
			return;
		meter = p;
	}
	for (int ch = 0; ch < cur->nch; ch++) {
		float db = meter[ch] > 1e-6f ? 20 * log10f(meter[ch]) : -120;
		static const struct { float db; int v; } meter_db[] = {
			{ -30, 0x26 }, { -18, 0x41 }, { -9, 0x57 }, { -4, 0x65 },
			{ -.5f, 0x77 }, { 0, 0x7F },
		};
		int v = db < meter_db[0].db ? 0 : 0x7F;
		for (int k = 1; k < 6 && v; k++)
			if (db < meter_db[k].db) {
				float a = (db - meter_db[k - 1].db) / (meter_db[k].db - meter_db[k - 1].db);
				v = meter_db[k - 1].v + (int)(a * (meter_db[k].v - meter_db[k - 1].v));
				break;
			}
		if (v != meter_sent[ch] && (meter_sent[ch] < 0 || !v || abs(v - meter_sent[ch]) >= meter_step)) {
			uint8_t msg[3] = { 0xB0 + ch, 0x02, v };
			midi_write(out, msg, 3);
			meter_sent[ch] = v;
		}
	}
}

static void leds_send(snd_rawmidi_t *out, int all)
{
	uint8_t msg[3 * NLED];
	int n = 0;
	for (int i = 0; i < NLED; i++) {
		if (!(led_groups & led_group(i)))
			continue;
		if (!all && led_on[i] == led_sent[i])
			continue;
		struct led a = led_addr(i);
		led_sent[i] = led_on[i];
		if (!a.status)
			continue;
		msg[n++] = a.status;
		msg[n++] = a.note;
		msg[n++] = led_on[i] ? 0x7F : 0;
	}
	if (n)
		midi_write(out, msg, n);
}

// TX frames as last seen, to log only the bytes that change.
static uint8_t tx_last[NDEV][152];
static int tx_seen[NDEV];

static void tx_log(int dev, const uint8_t *buf, int n)
{
	if (n > (int)sizeof(tx_last[0]))
		n = sizeof(tx_last[0]);
	// the TX CRC (and nothing of interest) follows the data
	int crc_at = dev == DEV_MC ? 104 : dev == DEV_ERP ? 120 : 148;
	if (tx_seen[dev]) {
		int any = 0;
		for (int i = 0; i < crc_at && i < n; i++) {
			if (buf[i] == tx_last[dev][i])
				continue;
			if (!any++)
				fprintf(txlog, "%.3f TX %s:", now(), devs[dev].name + 8);
			fprintf(txlog, " %d:%02x>%02x", i, tx_last[dev][i], buf[i]);
		}
		if (any)
			fputc('\n', txlog);
	}
	memcpy(tx_last[dev], buf, n);
	tx_seen[dev] = 1;
}
static uint8_t msb[16][128];	// last 14-bit CC MSB per channel/number

static int fader_target(const struct ctl *c)
{
	return c->dev == DEV_MC ? T_MC10 : T_DECKTEMPO;
}

// Momentary presses made up by the bridge: set now, released a few ticks
// later, after the player has seen at least one frame with the bit set.
#define PULSE_TICKS	3
static struct { int dev, byte, bit, ticks; } pulses[8];

static void pulse(int dev, int byte, int bit)
{
	for (size_t i = 0; i < sizeof(pulses) / sizeof(pulses[0]); i++)
		if (!pulses[i].ticks) {
			pulses[i].dev = dev, pulses[i].byte = byte, pulses[i].bit = bit;
			pulses[i].ticks = PULSE_TICKS;
			set_bit(dev, byte, bit, 1);
			return;
		}
}

static void pulses_tick(void)
{
	for (size_t i = 0; i < sizeof(pulses) / sizeof(pulses[0]); i++)
		if (pulses[i].ticks && !--pulses[i].ticks)
			set_bit(pulses[i].dev, pulses[i].byte, pulses[i].bit, 0);
}

// COLOR FX buttons: soundColorFx[i] is MC byte 21 bit 5 - i; azmix takes
// index i as its colour effect i (fx.h order: SPACE, DUB ECHO, SWEEP, NOISE,
// CRUSH, FILTER). SMART CFX starts on FILTER.
static int cfx_sel = 5;
// DDJ-400: the bridge itself keeps the COLOR FX that is on (the controller has no such
// button) in MC_CFX for azmix, and starts on FILTER, so its CHANNEL FILTER knobs (COLOR)
// work at once. -1: none.
#define MC_CFX	102
static int cfx_cur = -1;

static void cfx_pick(int e)	// e 0..5 (fx.h order), the same one again ends it
{
	cfx_cur = cfx_cur == e ? -1 : e;
	frame[DEV_MC][MC_CFX] = cfx_cur < 0 ? 6 : cfx_cur;
	dirty[DEV_MC] = 1;
}

// DDJ-400 PAD FX1 (SHIFT + BEAT LOOP: pad notes 0x10..0x17): pads 1..6 are the six COLOR
// FX, in the order of the AZ's SOUND COLOR FX buttons on its screen (SPACE, DUB ECHO,
// CRUSH, SWEEP / NOISE, FILTER); the pad of the one that is on is lit. Pads 7 and 8 do
// nothing there. Value: the effect's index in fx.h order.
#define PAD_FX1	0x10
static int cfx_own;		// the bridge keeps the COLOR FX (DDJ-400)
static int fx_mode[2];		// the deck is in PAD FX1: SHIFT + BEAT LOOP pressed, no other mode since
static const uint8_t cfx_pad[6] = { 0, 1, 4, 2, 3, 5 };

static void pad_fx_leds(void)	// over the pad LEDs the player's frame gave
{
	if (!cfx_own)
		return;
	for (int deck = 0; deck < 2; deck++) {
		if (!fx_mode[deck])
			continue;
		uint8_t *on = led_on + deck * L_DECK;
		for (int k = 0; k < 8; k++)
			on[L_PAD0 + k] = k < 6 && cfx_cur >= 0 && cfx_pad[k] == cfx_cur;
	}
}

// SLIP on a deck (the player's own state, read back from its TX frame): its BEAT SYNC LED
// pulses, once a second. With the deck synced the LED stays lit but dips for a moment each
// second, so SYNC and SLIP can be told apart and both show.
#define SLIP_PULSE_S	1.0
static void slip_leds(double t)
{
	if (!cfx_own)
		return;
	double ph = fmod(t, SLIP_PULSE_S) / SLIP_PULSE_S;
	for (int deck = 0; deck < 2; deck++) {
		uint8_t *on = led_on + deck * L_DECK;
		if (on[L_SLIP])
			on[L_SYNC] = sync_raw[deck] ? ph > .15 : ph < .5;
	}
}

// ---- Beat FX ------------------------------------------------------------------
//
// On the AZ the beat FX live in the mixer's MCU, which reports their state to
// the player for its BEAT FX panel (MC RX 84..93, EP147 0x21545E8, mapped on
// the panel 2026-11-06): 84 bit 7 on, bit 6 TAP (else AUTO BPM); 85 high
// nibble the effect (also read as the select encoder); 86 the beat as an
// index (2 1/16, 3 1/8, 5 1/4, 6 1/3, 7 1/2, 8 2/3, 9 3/4, 10 1, 12 2, 14 4,
// 16 8; the panel shows the nearest one the effect has); 88..89 BPM x 10;
// 90..91 the time in ms. The bridge plays that MCU: it keeps the state,
// writes it there for the player, and azmix reads it from the same frame.
//
// BPM (AUTO): the tempo master deck's, from the player's mixer TX (per
// channel BPM x 100 and tempo ratio x 10000, see UCOM-PROTOCOL.md §3).
#define BFX_TYPES	14		// the player's list (azmix maps it to fx.h)
// The beats BEAT < > step through: the ones the player's effects have (1/16
// none has; 1/3 and 2/3 left out, the user's choice: the GRV6 has no LED for
// them). grv6 is the GRV6's beat LED
// value (see bfxind_send).
static const struct { uint8_t index; float beats; uint8_t grv6; } bfx_beats[] = {
	{ 3, 1 / 8.f, 2 }, { 5, 1 / 4.f, 3 }, { 7, 1 / 2.f, 4 },
	{ 9, 3 / 4.f, 33 }, { 10, 1, 5 }, { 12, 2, 6 }, { 14, 4, 7 },
	{ 16, 8, 8 },
};
#define NBEATS	(int)(sizeof(bfx_beats) / sizeof(bfx_beats[0]))
static int bfx_type, bfx_beat = 2;	// DELAY at 1/2
// FX CH SELECT lever: two notes, 0x94 0x10 lit for CH1 and 0x95 0x11 for
// CH2, both for CH1&CH2 (taken as MASTER). The target
// goes to MC_BFX_CH, a byte the player does not read (after the CRC), for
// azmix: mixer channel 0.., or 0xFF for the master.
#define MC_BFX_CH	120
// Likewise byte 121: bit n = brake mixer channel n (GRV6 DRUM RELEASE).
#define MC_BRAKE	121
// Bytes 122..127: the player's MIXER SETTINGS, copied from its MC TX (bytes
// 5, 6, 8, 9, 12, 16, mapped by clicking each button, 2026-11-06), for azmix:
//   122 (TX 5)  input select, DECK3 bits 4-5, DECK4 bits 0-1: 3 DECK, 1 LINE, 2 PHONO
//   123 (TX 6)  crossfader assign, 2 bits per deck, deck 1 at bits 6-7 ..
//               deck 4 at bits 0-1: 0 THRU, 1 A, 2 B
//   124 (TX 8)  bits 0-1 channel fader curve (0 slow, 1 linear, 2 fast),
//               bits 2-3 crossfader curve (2 cut, 1 middle, 0 smooth),
//               bit 4 ISO (else EQ), bit 6 headphones MONO SPLIT
//   125 (TX 9)  LINK preview volume: 0 0 dB, 1 -3, 2 -6, 3 -9
//   126 (TX 12) MIC feedback reducer: 0 off, 1 light, 2 heavy
//   127 (TX 16) BEAT FX MIC: 0 MIC 1+2, 1 MIC 1, 2 MIC 2
// and byte 100 (TX 10): bit 6 MIC Output to Master REC (on); byte 101
// (TX 11, UTILITY > Mixer): bit 4 Talkover Mode Advanced (else Normal),
// bits 0-1 Talkover Level (0 -6 dB, 1 -12, 2 -18, 3 -24).
#define MC_SETTINGS	122
#define MC_MIC_REC	100
#define MC_TALKOVER	101
static const uint8_t settings_tx[6] = { 5, 6, 8, 9, 12, 16 };
static int lever_a, lever_b, bfx_ch = -1;
// SHIFT + a move of the FLX4's lever puts the beat FX on the mic alone
// (MC_BFX_CH BFX_MIC, for azmix); a move without SHIFT, back to the lever's
// channel. Moves only: the lever's stream repeats its position.
#define BFX_MIC	4
static int shift_held[2];
static double shift_at[2];		// when each side's SHIFT went down

// DDJ-400: SHIFT held SLIP_HOLD_S and then SYNC toggles the deck's SLIP. The unit tells a
// SYNC press only when it is let go and says nothing for SHIFT + SYNC held, so the hold
// is the SHIFT's. The AZ's SLIP button is pressed with its SHIFT bit cleared (SHIFT +
// SLIP is the vinyl/jog mode there), and the bit comes back a few ticks later.
#define SLIP_HOLD_S	2.0
#define SLIP_MASK_TICKS	8
static int slip_mask_ticks;

static void slip_toggle(int deck)
{
	for (int s = 0; s < 2; s++)
		set_bit(DEV_ERP, (s ? 16 : 8) + 1, 7, 0);
	pulse(DEV_ERP, (SIDE(deck) ? 16 : 8) + 0, 2);
	slip_mask_ticks = SLIP_MASK_TICKS;
}

static void slip_mask_tick(void)
{
	if (slip_mask_ticks && !--slip_mask_ticks)
		for (int s = 0; s < 2; s++)
			set_bit(DEV_ERP, (s ? 16 : 8) + 1, 7, shift_held[0] || shift_held[1]);
}
// The FLX4 reports the lever ten times a second, but from ~100 ms after the
// position query until the lever first moves that stream says the 0x95 0x11
// side is off whatever the lever (seen with the lever at MASTER: the query's
// answer has both on, the stream then CH1). So the answer to the query
// (LEVER_QUERY_S after opening) is taken as it is, and after it the stream
// only once the 0x94 0x10 side changes, i.e. the lever moved.
#define LEVER_QUERY_S	.06
static double lever_since;		// controller opened at
static int lever_trust;
static double ch_bpm[4];		// effective BPM per mixer channel, 0 unknown
static double ch_ratio[4];		// tempo ratio per channel (deck)
static int ch_state[4];			// 3 playing, 5 paused, 6 cued; 0 unknown

// The browse-area LEDs say which view is open (2026-11-09, each button
// pressed through the FIFO with the panel watched): MC TX 26..27, 2 bits per
// LED, 2 lit, 1 dark; all dark on the waveform screen. The views and their
// buttons (RX). MENU is lit while UTILITY is open (a long press opens it, a
// short one closes it); a second press of a lit view's button goes back to
// the waveform screen. TX 27 bits 0-1 stay at 2 (not one of these).
static const struct { uint8_t tx, shift, byte, bit; } views[] = {
	{ 26, 6, 32, 7 },	// SOURCE
	{ 26, 4, 32, 6 },	// BROWSE
	{ 26, 2, 32, 5 },	// TAG LIST
	{ 26, 0, 32, 4 },	// PLAYLIST
	{ 27, 6, 32, 3 },	// SEARCH
	{ 27, 4, 32, 2 },	// MENU (UTILITY)
};
#define NVIEWS	(int)(sizeof(views) / sizeof(views[0]))
static int views_lit = -1;		// bit i: views[i] open; -1 unknown
static const char *strip_fifo;		// -L: the on-screen buttons (strip/azstrip.py)
static double strip_sent;

// Tells the on-screen buttons the open views ("L mask"), on a change and
// once a second (the overlay may have started later).
static void strip_tell(void)
{
	if (!strip_fifo || views_lit < 0)
		return;
	int fd = open(strip_fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	char line[16];
	int k = snprintf(line, sizeof(line), "L %d\n", views_lit);
	if (write(fd, line, k) < 0) { /* nobody reading: fine */ }
	close(fd);
	strip_sent = now();
}

// Back to the waveform screen: press the open view's button.
static void go_home(void)
{
	for (int i = 0; i < NVIEWS; i++)
		if (views_lit > 0 && views_lit & 1 << i) {
			pulse(DEV_MC, views[i].byte, views[i].bit);
			return;
		}
}

static void mc_tx(const uint8_t *buf, int n)
{
	if (n < 92)
		return;
	int lit = 0;
	for (int i = 0; i < NVIEWS; i++)
		if ((buf[views[i].tx] >> views[i].shift & 3) == 2)
			lit |= 1 << i;
	if (lit != views_lit || now() - strip_sent > 1) {
		views_lit = lit;
		strip_tell();
	}
	if (frame[DEV_MC][MC_MIC_REC] != buf[10] || frame[DEV_MC][MC_TALKOVER] != buf[11]) {
		frame[DEV_MC][MC_MIC_REC] = buf[10];
		frame[DEV_MC][MC_TALKOVER] = buf[11];
		dirty[DEV_MC] = 1;
	}
	for (int i = 0; i < 6; i++)
		if (frame[DEV_MC][MC_SETTINGS + i] != buf[settings_tx[i]]) {
			frame[DEV_MC][MC_SETTINGS + i] = buf[settings_tx[i]];
			dirty[DEV_MC] = 1;
		}
	for (int ch = 0; ch < 4; ch++)
		if (buf[39] & (0x10 << ch)) {
			const uint8_t *r = buf + 40 + 12 * ch;
			uint32_t bpm = r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24;
			uint32_t ratio = r[4] | r[5] << 8 | r[6] << 16 | (uint32_t)r[7] << 24;
			ch_bpm[ch] = bpm / 100.0 * ratio / 10000.0;
			ch_ratio[ch] = ratio / 10000.0;
			ch_state[ch] = r[8];
		}
}

// ---- GRV6 jog indicator and Beat FX beat LEDs ------------------------------
//
// rekordbox's table for the GRV6 (DDJ-GRV6.midi.csv in its Android app) names
// the outputs: the jog's indicator is CC 0..3 on channel 15 for decks 1..4,
// an angle 1..72 (0 dark); the beat LEDs over the Beat FX knob are CC 0x64 on
// channel 4, "BeatMultiplier No".
//
// The indicator shows the playhead as a record at 33 1/3 rpm would: it turns
// while the deck plays, runs round inside a loop, goes back when CUE is let
// go, jumps with a hot cue and stands while paused. The playhead comes from
// the player's deck TX frames (deck_tx). Without them the ring is made up: it
// turns at 33 1/3 rpm with the deck's tempo while the live deck's PLAY LED is
// lit steadily (the MC TX status records, ch_state, come only now and then),
// and with the platter.
#define JOGIND_STEPS	72
static int jogind_sent[4] = { -1, -1, -1, -1 };

// Deck TX (152 bytes, spi5.0 left, spi2.0 right): the player sends the side's
// two decks in turn. Read off TX logs while the user played, held CUE, made
// and halved loops and hit hot cues (2026-11-08):
//   0         0x08: the side plays DECK 3/4 (in both decks' frames)
//   8         deck number in the high nibble (0x11 deck 1, 0x21 deck 2, ...)
//   17        flags: 0x80 loop IN being adjusted, 0x40 loop OUT being
//             adjusted (0x10 toggles while paused)
//   19        0x80: the tempo master (0x81, the others 0x01)
//   22        0x10 a track is loaded, 0x20 none
//   24        the mode: 0x81 playing through, 0x0C..0x10 in a loop by its
//             length (the position runs round the loop), 0x01 a loop of
//             another length (after IN/OUT adjust); 8, 0x0A seen too
//   25        the playhead: minutes
//   26        seconds
//   27..28    milliseconds, little-endian (0..999)
// The playhead is the track's, in every mode: CUE held runs on from the cue
// and goes back to it when let go, a loop runs round, a hot cue jumps. Now
// and then a frame carries 0:00.000; the position of those is left out.
// Bytes 0 and 19 were read off DECK select and MASTER presses (2026-11-08).
static void deck_tx(const uint8_t *buf, int n)
{
	if (n < 29)
		return;
	int d = (buf[8] >> 4) - 1;
	if (d < 0 || d > 3)
		return;
	az_layer[SIDE(d)] = buf[0] & 0x08 ? 1 : 0;	// both decks' frames say it
	if (buf[19] & 0x80)
		master_deck = d;
	else if (master_deck == d)
		master_deck = -1;
	deck_loaded[d] = buf[22] == 0x10;
	deck_mode[d] = buf[24];
	deck_flags[d] = buf[17];
	deck_at[d] = now();
	// now and then a loaded deck's frame says 0:00.000 between two real
	// positions (the rest of it as usual): kept at the last one
	int ms = buf[27] | buf[28] << 8;
	if (buf[25] || buf[26] || ms || !deck_loaded[d])
		deck_pos[d] = buf[25] * 60 + buf[26] + ms / 1000.0;
}

static void jogind_send(snd_rawmidi_t *out, double dt, int all)
{
	if (cur->decks < 4)
		return;
	for (int d = 0; d < 4; d++) {
		int side = SIDE(d), live = layer[side] == d >> 1;
		if (deck_fresh(d)) {			// the player's playhead
			double rev = deck_pos[d] * (100 / 3.0 / 60);
			int v = deck_loaded[d] ?
				1 + (int)((rev - floor(rev)) * JOGIND_STEPS) % JOGIND_STEPS : 0;
			if (v != jogind_sent[d] || all) {
				uint8_t msg[3] = { 0xBF, d, v };
				midi_write(out, msg, 3);
				jogind_sent[d] = v;
			}
			continue;
		}
		// no deck frames: made up, turning while the mixer's status says
		// the deck plays
		int plays = ch_state[d] == 3;
		if (plays && !(live && jogs[side].touch))
			jog_rev[d] += dt * (100 / 3.0 / 60) * (ch_ratio[d] > 0 ? ch_ratio[d] : 1);
		jog_rev[d] -= floor(jog_rev[d]);
		int v = ch_state[d] ? 1 + (int)(jog_rev[d] * JOGIND_STEPS) % JOGIND_STEPS : 0;
		if (v != jogind_sent[d] || all) {
			uint8_t msg[3] = { 0xBF, d, v };
			midi_write(out, msg, 3);
			jogind_sent[d] = v;
		}
	}
}

// The value is a doubling ladder, read off the unit (2026-11-06): 3 lights
// 1/4, 4 1/2, 5 1, 6 1 and 4 together (2: no LED of its own), 7 4, 8 8, 9 16;
// 10 32; 2 and below blink 1/4 (shorter than any LED). 3/4 is apart, 33. 34..46 blink the same LEDs backwards, 47.. repeat the ladder,
// 57 and up dark.
static int bfxind_sent = -1;

static void bfxind_send(snd_rawmidi_t *out, int all)
{
	int v = bfx_beats[bfx_beat].grv6;
	if (cur->decks < 4 || (!all && bfxind_sent == v))
		return;
	uint8_t msg[3] = { 0xB4, 0x64, v };
	midi_write(out, msg, 3);
	bfxind_sent = v;
}

// The virtual X-PAD (aztouch's X zone over the panel's X-PAD PARAMETER strip). Parameter
// BEAT: the finger's position picks the beat, six steps 1/16 1/8 1/4 1/2 3/4 1 (the beats
// BEAT < > step through have no 1/16, so the first step is 1/8). While it touches, the
// beat FX is on at that beat (for the player and the mixer only: nothing is written to the
// controller, its BEAT FX LED stays as the ON button left it); the lift puts the beat
// back (momentary, taken from the AZ's own X-PAD; not checked on the unit). A touch that
// lasts XPAD_MAX_S without news is let go, in case aztouch went away.
#define XPAD_MAX_S	30
static const uint8_t xpad_step_beat[6] = { 0, 0, 1, 2, 3, 4 };	// into bfx_beats[]
static int xpad_down, xpad_saved_beat;
static double xpad_at;

static void xpad_release(void)
{
	if (!xpad_down)
		return;
	xpad_down = 0;
	bfx_beat = xpad_saved_beat;
	frame[DEV_MC][28] &= ~1;
	dirty[DEV_MC] = 1;
}

static void bfx_report(void)
{
	if (xpad_down && now() - xpad_at > XPAD_MAX_S)
		xpad_release();
	uint8_t *f = frame[DEV_MC];
	int src = bfx_ch >= 0 && bfx_ch != BFX_MIC ? bfx_ch : master_deck;
	double bpm = src >= 0 ? ch_bpm[src] : 0;
	if (bpm <= 0)
		bpm = ch_bpm[0] > 0 ? ch_bpm[0] : ch_bpm[1];
	int ms = (int)(bfx_beats[bfx_beat].beats * 60000 / (bpm > 0 ? bpm : 120) + .5);
	int bpm10 = (int)(bpm * 10 + .5);
	uint8_t want[8] = {
		led_on[L_BEATFX] || xpad_down ? 0x80 : 0, (uint8_t)(bfx_type << 4 | (f[85] & 0x0F)),
		bfx_beats[bfx_beat].index, f[87], bpm10 & 0xFF, bpm10 >> 8, ms & 0xFF, ms >> 8,
	};
	uint8_t ch = bfx_ch >= 0 ? bfx_ch : 0xFF;
	if (f[MC_BFX_CH] != ch) {
		f[MC_BFX_CH] = ch;
		dirty[DEV_MC] = 1;
	}
	if (memcmp(f + 84, want, sizeof(want))) {
		memcpy(f + 84, want, sizeof(want));
		dirty[DEV_MC] = 1;
	}
}

// 4 BEAT/EXIT: which AZ button the press went to, per deck (released alike).
static int loop4_byte[2], loop4_bit[2];

#define MIN_PRESS_S	.03

// An s16 counter in a frame (the browse encoder)
static void counter_add(int dev, int at, int d)
{
	int16_t n = frame[dev][at] | frame[dev][at + 1] << 8;
	n += d;
	frame[dev][at] = n & 0xFF, frame[dev][at + 1] = (uint16_t)n >> 8;
	dirty[dev] = 1;
}

// BEAT FX driven from the screen (aztouch). These are the same state changes the
// controller's BEAT FX buttons make; the X-PAD is the mixer's bytes 28 (bit 0
// touch) and 29 (position), which the player draws on its BEAT FX panel.
#define MC_XPAD	28
static void bfx_touch_cmd(const char *text)
{
	char cmd[16] = "";
	int v = 0;
	if (text[0] == 'x') {
		unsigned t = 0, pos = 0;
		if (sscanf(text + 1, "%u %u", &t, &pos) < 1)
			return;
		if (pos > 255)
			pos = 255;
		uint8_t *f = frame[DEV_MC];
		if (!t) {
			xpad_release();
			return;
		}
		if (!xpad_down) {
			xpad_down = 1;
			xpad_saved_beat = bfx_beat;
		}
		xpad_at = now();
		bfx_beat = xpad_step_beat[pos * 6 / 256];
		f[MC_XPAD] |= 1;
		f[MC_XPAD + 1] = pos;
		dirty[DEV_MC] = 1;
		return;
	}
	if (sscanf(text + 1, " %15s %d", cmd, &v) < 1)
		return;
	if (!strcmp(cmd, "on"))
		led_on[L_BEATFX] ^= 1;
	else if (!strcmp(cmd, "next"))
		bfx_type = (bfx_type + 1) % BFX_TYPES;
	else if (!strcmp(cmd, "prev"))
		bfx_type = (bfx_type + BFX_TYPES - 1) % BFX_TYPES;
	else if (!strcmp(cmd, "type") && v >= 0 && v < BFX_TYPES)
		bfx_type = v;
	else if (!strcmp(cmd, "beat+") && bfx_beat + 1 < NBEATS)
		bfx_beat++;
	else if (!strcmp(cmd, "beat-") && bfx_beat > 0)
		bfx_beat--;
}

static void apply(const struct ctl *c, int value, int on)
{
	switch (c->kind) {
	case FX_SELECT:
		if (on)
			bfx_type = (bfx_type + c->c + BFX_TYPES) % BFX_TYPES;
		break;
	case FX_TYPE:
		if (on)
			bfx_type = c->c;
		break;
	case FX_CH:
		if (on)
			bfx_ch = c->c;
		break;
	case LAYER:
		if (on)
			layer_set(SIDE(c->a), c->a >> 1);
		break;
	case BRAKE:
		set_bit(DEV_MC, MC_BRAKE, c->a, on);
		break;
	case STEP:
		if (on)
			counter_add(c->dev, c->a, c->c);
		break;
	case HOME:
		if (on)
			go_home();
		break;
	case FX_BEAT:
		if (on && bfx_beat + c->c >= 0 && bfx_beat + c->c < NBEATS)
			bfx_beat += c->c;
		break;
	case LOOP4: {
		int d = c->a, base = d ? 16 : 8;
		if (on) {	// RELOOP/EXIT while a loop runs, else 4 BEAT LOOP
			loop4_byte[d] = base + (loop_running(d) ? 1 : 4);
			loop4_bit[d] = loop_running(d) ? 3 : 2;
		}
		if (loop4_byte[d])
			set_bit(DEV_ERP, loop4_byte[d], loop4_bit[d], on);
		break;
	}
	case CFX_NEXT:
		if (on)
			cfx_sel = (cfx_sel + 1) % 6;
		/* fall through */
	case CFX_PRESS:
		if (on)
			pulse(DEV_MC, 21, 5 - cfx_sel);
		break;
	case BUTTON: {
		// A press must last long enough for the player to see it: BEAT SYNC
		// sends press and release together, when it is let go. A release
		// that soon after the press clears the bit a few frames later.
		static double down_at[MAXROWS];
		size_t i = c - cur->map;
		if (on) {
			down_at[i] = now();
			set_bit(c->dev, c->a, c->b, 1);
		} else if (now() - down_at[i] < MIN_PRESS_S) {
			pulse(c->dev, c->a, c->b);
		} else {
			set_bit(c->dev, c->a, c->b, 0);
		}
		break;
	}
	case FADER14: {
		unsigned v = value >> 4;	// 14 -> 10 bits
		if (c->invert)
			v = 1023 - v;
		if (fader_target(c) == T_MC10)
			set_mc10(c->a, c->b, c->c, v);
		else
			set_deck10(c->dev, c->a, v);
		break;
	}
	case JOG:
		jog_move(&jogs[c->a], (value - 64) * (double)AZ_JOG_COUNTS / cur->jog_ticks /
			 (loop_adjusting(c->a) ? jog_fine : 1));
		break;
	case JOGTOUCH:
		jogs[c->a].touch = on;
		set_bit(jogs[c->a].dev, 9, 1, on);
		break;
	case ENCODER:
		counter_add(c->dev, c->a, value < 64 ? value : value - 128);
		break;
	}
}

static double return_since;	// SHIFT + SMART FADER down since, or 0

// Back to the boot menu once the combo has been held long enough.
static void return_check(void)
{
	if (!return_since || now() - return_since < RETURN_HOLD_S)
		return;
	return_since = 0;
	int fd = open(RETURN_REQUEST, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	fprintf(stderr, "azbridge: back to the menu%s\n", fd < 0 ? " (request failed)" : "");
	if (fd >= 0)
		close(fd);
}

static void midi_message(uint8_t st, uint8_t d1, uint8_t d2)
{
	if (txlog)
		fprintf(txlog, "%.3f MIDI %02X %02X %02X\n", now(), st, d1, d2);
	uint8_t type = st & 0xF0, ch = st & 0x0F;
	int on = type == 0x90 && d2 > 0;
	if (type == 0x80)
		st = 0x90 | ch;		// note off
	int fourdeck = cur->decks == 4;
	// DDJ-400 BEAT FX CH SELECT: a three-position switch, one note per position (CH1 0x10, CH2
	// 0x11, MASTER 0x14, on channel 4), the new position's note On when it slides in. It
	// sets the beat FX target as the AZ's CH SELECT does: channel 1, channel 2, or the master.
	if (cfx_own && st == 0x94 && (d1 == 0x10 || d1 == 0x11 || d1 == 0x14)) {
		if (on)
			bfx_ch = d1 == 0x10 ? 0 : d1 == 0x11 ? 1 : -1;
		return;
	}
	if (!fourdeck && ((st == 0x94 && d1 == 0x10) || (st == 0x95 && d1 == 0x11))) {
		if (!lever_trust && now() - lever_since > LEVER_QUERY_S) {
			if (st != 0x94 || on == lever_a)
				return;
			lever_trust = 1;
		}
		int *lv = st == 0x94 ? &lever_a : &lever_b;	// FLX4 FX CH lever
		int moved = *lv != on;
		*lv = on;
		if (moved && (shift_held[0] || shift_held[1]))
			bfx_ch = BFX_MIC;
		else if ((moved || bfx_ch != BFX_MIC) && (lever_a || lever_b))	// both off: mid-slide
			bfx_ch = lever_a && lever_b ? -1 : lever_a ? 0 : 1;
		return;
	}
	if (!fourdeck && ((st == 0x94 && d1 == 0x11) || (st == 0x95 && d1 == 0x10)))
		return;			// the lever's two other notes, always off
	// SHIFT: either side's holds both decks' (the controller shifts only its
	// own side's notes; the player sees the bit and shifts the rest itself)
	if (type == 0x90 && ch < cur->decks && d1 == 0x3F) {
		shift_held[SIDE(ch)] = on;
		if (on)
			shift_at[SIDE(ch)] = now();
		for (int s = 0; s < 2; s++)
			set_bit(DEV_ERP, (s ? 16 : 8) + 1, 7, shift_held[0] || shift_held[1]);
		return;
	}
	if (cfx_own && type == 0x90 && ch < 2 && d1 == 0x60 &&	// SHIFT + SYNC after a 2 s SHIFT: SLIP
	    shift_held[SIDE(ch)] && now() - shift_at[SIDE(ch)] >= SLIP_HOLD_S) {
		if (on)
			slip_toggle(ch);
		return;
	}
	// DDJ-400 SHIFT + PLAY (note 0x47 of the deck, nothing in the published map but censor):
	// the AZ's VINYL SPEED ADJUST button of that deck (ErP D+4 bit 0), held as long as the key.
	// The AZ's SHIFT bit is cleared while it is down (SHIFT + the button would be another function
	// there) and comes back on the release.
	if (cfx_own && (type == 0x90 || type == 0x80) && ch < 2 && d1 == 0x47) {
		if (on)
			for (int s = 0; s < 2; s++)
				set_bit(DEV_ERP, (s ? 16 : 8) + 1, 7, 0);
		set_bit(DEV_ERP, (SIDE(ch) ? 16 : 8) + 4, 0, on);
		if (!on)
			for (int s = 0; s < 2; s++)
				set_bit(DEV_ERP, (s ? 16 : 8) + 1, 7, shift_held[0] || shift_held[1]);
		return;
	}
	if (st == 0x96 && d1 == cur->return_note) {	// SHIFT + SMART FADER / BACK
		return_since = on ? now() : 0;
		if (cur->map == flx4)		// the GRV6's is PLAYLIST too (table)
			return;
	}
	if (on) {			// LEDs the bridge keeps itself
		if (type == 0x90 && ch < cur->nch && d1 == 0x54)
			led_on[L_HPCUE1 + ch] ^= 1;
		else if (st == 0x96 && d1 == 0x63)
			led_on[L_MASTERCUE] ^= 1;
		else if ((st == 0x94 || st == 0x95) && d1 == 0x47)
			led_on[L_BEATFX] ^= 1;
		else if (st == 0x96 && d1 == 0x00)
			led_on[L_CFX] ^= 1;
		else if (st == 0x96 && d1 == 0x08)
			led_on[L_CFX] = 1;
		for (int m = 0; m < 8; m++)
			if (type == 0x90 && ch < cur->decks && d1 == flx4_mode_note[m])
				leds_pad_mode(ch, 16 * m);
		if (cfx_own && type == 0x90 && ch < 2) {	// PAD FX1 = SHIFT + BEAT LOOP only
			if (d1 == flx4_mode_note[1])
				fx_mode[ch] = shift_held[0] || shift_held[1];
			else
				for (int m = 0; m < 8; m++)
					if (m != 1 && d1 == flx4_mode_note[m])
						fx_mode[ch] = 0;
		}
	}
	if (type != 0x80 && type != 0x90 && type != 0xB0)
		return;

	int value = d2, matched = 0;
	for (int i = 0; i < cur->nmap; i++) {
		const struct ctl *c = &cur->map[i];
		if (c->status != st)
			continue;
		if (c->kind == FADER14 && type == 0xB0) {
			if (d1 == c->num) {		// MSB
				msb[ch][d1] = d2;
				value = d2 << 7;
			} else if (d1 == c->num + 0x20) {	// LSB completes it
				value = msb[ch][c->num] << 7 | d2;
			} else {
				continue;
			}
		} else if (c->num != d1) {
			continue;
		}
		matched = 1;
		if (fourdeck && ch < 4 && (ch >> 1) != layer[SIDE(ch)]) {
			// The position query reports the TEMPO of both layers: only
			// the live one's moves the AZ's. A press or a jog on the other
			// says that one is live (missed note 60).
			if (c->kind == FADER14 && c->dev != DEV_MC)
				continue;
			if (c->kind == JOG || ((c->kind == JOGTOUCH || c->kind == LOOP4 ||
			    (c->kind == BUTTON && c->dev == DEV_ERP)) && on))
				layer_set(SIDE(ch), ch >> 1);
		}
		apply(c, value, on);
	}
	// pads: any pad mode, notes 0..0x7F on 0x97 + 2n (0x98 + 2n shifted),
	// deck n = 0..3
	if (!matched && (type == 0x90 || type == 0x80) && ch >= 7 && ch < 7 + 2 * cur->decks) {
		int deck = (ch - 7) / 2;
		if ((ch & 1) && on)	// unshifted (7, 9, ..): its mode shows in the note
			leds_pad_mode(deck, d1 & 0x70);
		if (fourdeck && on)
			layer_set(SIDE(deck), deck >> 1);
		if (cfx_own && (ch & 1) && deck < 2 && fx_mode[deck] && (d1 & 0x70) == PAD_FX1) {	// PAD FX1: COLOR FX
			if (on && (d1 & 7) < 6)
				cfx_pick(cfx_pad[d1 & 7]);
		} else {
			set_bit(DEV_ERP, (SIDE(deck) ? 16 : 8) + 6, d1 & 7, on);
		}
		matched = 1;
	}
	if (verbose && !matched)
		fprintf(stderr, "azbridge: unmapped %02X %02X %02X\n", st, d1, d2);
}

// Running-status MIDI byte parser.
static void midi_byte(uint8_t b)
{
	static uint8_t st, d[2], sx[64];
	static int n, nsx = -1;
	if (b >= 0xF8)			// realtime
		return;
	if (nsx >= 0 && (b & 0x80 || nsx == (int)sizeof(sx))) {	// SysEx: -t logs it
		if (txlog) {
			fprintf(txlog, "%.3f MIDI F0", now());
			for (int i = 0; i < nsx; i++)
				fprintf(txlog, " %02X", sx[i]);
			fprintf(txlog, "%s\n", b == 0xF7 ? " F7" : " ...");
		}
		nsx = -1;
	}
	if (b == 0xF0)
		nsx = 0;
	else if (nsx >= 0) {
		sx[nsx++] = b;
		return;
	}
	if (b & 0x80) {
		st = b >= 0xF0 ? 0 : b;	// sysex and system common: ignored
		n = 0;
		return;
	}
	if (!st)
		return;
	d[n++] = b;
	int need = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 1 : 2;
	if (n == need) {
		midi_message(st, d[0], need == 2 ? d[1] : 0);
		n = 0;
	}
}

// ---- main -------------------------------------------------------------------

static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

struct conn {
	int fd;
	int dev;	// -1 until the name arrives
};

static void send_frame(struct conn *c)
{
	int d = c->dev;
	uint16_t crc = crc16(frame[d], devs[d].crc_at);
	frame[d][devs[d].crc_at] = crc & 0xFF;
	frame[d][devs[d].crc_at + 1] = crc >> 8;
	if (send(c->fd, frame[d], devs[d].size, MSG_DONTWAIT) < 0 && errno != EAGAIN)
		fprintf(stderr, "azbridge: %s: %s\n", devs[d].name, strerror(errno));
}

int main(int argc, char **argv)
{
	const char *sock_path = "/opt/xdjaz/state/tmp/azbridge.sock";
	const char *midi_dev = NULL, *inject_path = NULL, *ctlr_name = NULL;
	int opt, keepalive_opt = 0, no_out = 0;
	while ((opt = getopt(argc, argv, "c:ks:m:vt:i:L:n")) != -1) {
		if (opt == 'c') ctlr_name = optarg;
		else if (opt == 'n') no_out = 1;
		else if (opt == 'k') keepalive_opt = 1;
		else if (opt == 's') sock_path = optarg;
		else if (opt == 'm') midi_dev = optarg;
		else if (opt == 'v') verbose = 1;
		else if (opt == 'i') inject_path = optarg;
		else if (opt == 'L') strip_fifo = optarg;
		else if (opt == 't') {
			if (!(txlog = fopen(optarg, "w"))) { perror(optarg); return 1; }
			setvbuf(txlog, NULL, _IOLBF, 0);
		}
		else { fprintf(stderr, "usage: azbridge [-c flx4|ddj400|grv6] [-k] [-n] [-s socket] [-m midi] [-v] [-t txlog] [-i fifo] [-L fifo]\n"); return 2; }
	}
	// the controller: named, else the GRV6 if its card is there, else the DDJ-400, else the FLX4
	cur = NULL;
	for (int i = NROWS(ctlrs) - 1; i >= 0 && !cur; i--) {
		char path[64];
		snprintf(path, sizeof(path), "/proc/asound/%s", ctlrs[i].card);
		if (ctlr_name ? !strcmp(ctlr_name, ctlrs[i].name) : !access(path, F_OK) || i == 0)
			cur = &ctlrs[i];
	}
	if (!cur || cur->nmap > MAXROWS) {
		fprintf(stderr, "azbridge: unknown controller %s\n", ctlr_name);
		return 2;
	}
	char dev_buf[64];
	if (!midi_dev) {
		snprintf(dev_buf, sizeof(dev_buf), "hw:%s,0,0", cur->card);
		midi_dev = dev_buf;
	}
	const char *fine = getenv("AZ_JOG_FINE");
	if (fine && atof(fine) >= 1)
		jog_fine = atof(fine);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	init_frames();
	if (!strcmp(cur->name, "ddj400")) {	// FILTER on from the start (see MC_CFX)
		cfx_own = 1;
		cfx_cur = 5;
		frame[DEV_MC][MC_CFX] = 5;
	}
	led_groups_init();
	if (cfx_own) {		// DDJ-400: the unit lights its pad mode buttons itself, and a mode LED
		led_groups &= ~G_MODES;	// note from the host (0x1E, its PAD FX1) switches its pad mode
		fprintf(stderr, "azbridge: DDJ-400, the pad mode LEDs are left to the unit\n");
	}
	mc_shm_open();

	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
	int lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	unlink(sock_path);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, MAXCONN) < 0) {
		perror("azbridge: socket");
		return 1;
	}
	fprintf(stderr, "azbridge: listening on %s, controller %s on %s\n", sock_path,
		cur->name, midi_dev);

	// O_RDWR: the FIFO never reports EOF when a writer goes away
	int ifd = -1;
	if (inject_path) {
		unlink(inject_path);
		if (mkfifo(inject_path, 0600) < 0 || (ifd = open(inject_path, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) {
			perror(inject_path);
			return 1;
		}
	}

	struct conn conns[MAXCONN];
	int nconn = 0;
	snd_rawmidi_t *midi = NULL, *midi_out = NULL;
	double next_keepalive = 0, next_led = 0, next_meter = 0;
	double next_tick = now(), next_midi_try = 0;

	while (!stop) {
		double t = now();
		if (!midi && t >= next_midi_try) {
			if (snd_rawmidi_open(&midi, &midi_out, midi_dev, SND_RAWMIDI_NONBLOCK) == 0) {
				fprintf(stderr, "azbridge: controller %s open\n", midi_dev);
				// it answers with the position of every knob and fader
				if (cur->query)
					midi_write(midi_out, cur->query, sizeof(flx4_query));
				if (cur->init)
					midi_write(midi_out, cur->init, cur->init_n);
				lever_since = now(), lever_trust = 0;
				// The FLX4 leaves MIC LEVEL out of its answer: until the
				// knob moves, azmix gets it at 0 dB (its +20 dB top is
				// 10x, so 1/sqrt(10) of the range)
				if (cur->init)
					set_mc10(MC_MIC, MC_MIC + 1, 6, MIC_LEVEL_0DB);
				memset(meter_sent, 0xFF, sizeof(meter_sent));
			}
			else
				midi = NULL, next_midi_try = t + 1;
		}

		struct pollfd p[MAXCONN + 4];
		int np = 0;
		p[np++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		for (int i = 0; i < nconn; i++)
			p[np++] = (struct pollfd){ .fd = conns[i].fd, .events = POLLIN };
		int inject_at = np;
		if (ifd >= 0)
			p[np++] = (struct pollfd){ .fd = ifd, .events = POLLIN };
		int midi_at = np;
		if (midi)
			np += snd_rawmidi_poll_descriptors(midi, p + np, 2);
		int wait = (int)((next_tick - t) * 1000);
		poll(p, np, wait < 0 ? 0 : wait);

		if (p[0].revents & POLLIN) {
			int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
			if (fd >= 0 && nconn < MAXCONN)
				conns[nconn++] = (struct conn){ fd, -1 };
			else if (fd >= 0)
				close(fd);
		}
		for (int i = 0; i < nconn; i++) {
			if (!(p[1 + i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			uint8_t buf[256];
			ssize_t n = recv(conns[i].fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
			if (n <= 0) {		// player closed it; dropped below
				if (conns[i].dev >= 0)
					fprintf(stderr, "azbridge: %s closed\n", devs[conns[i].dev].name);
				close(conns[i].fd);
				conns[i].fd = -1;
				continue;
			}
			if (conns[i].dev < 0) {	// first packet: the device name
				buf[n] = 0;
				for (int d = 0; d < NDEV; d++)
					if (strcmp((char *)buf, devs[d].name) == 0)
						conns[i].dev = d;
				fprintf(stderr, "azbridge: %s %s\n", (char *)buf,
					conns[i].dev < 0 ? "unknown" : "connected");
				if (conns[i].dev >= 0)
					send_frame(&conns[i]);
			}
			// later packets are TX frames (LEDs, display)
			else if (conns[i].dev >= 0) {
				if (conns[i].dev == DEV_ERP)
					leds_from_erp(buf, (int)n);
				else if (conns[i].dev == DEV_MC)
					mc_tx(buf, (int)n);
				else
					deck_tx(buf, (int)n);
				if (txlog)
					tx_log(conns[i].dev, buf, (int)n);
			}
		}
		for (int i = 0; i < nconn; i++)	// p[] no longer needed for them
			if (conns[i].fd < 0)
				conns[i--] = conns[--nconn];
		if (midi) {
			unsigned short rev = 0;
			snd_rawmidi_poll_descriptors_revents(midi, p + midi_at, np - midi_at, &rev);
			if (rev & (POLLERR | POLLHUP)) {
				fprintf(stderr, "azbridge: controller gone\n");
				snd_rawmidi_close(midi);
				snd_rawmidi_close(midi_out);
				midi = midi_out = NULL;
				next_midi_try = now() + 1;
			} else if (rev & POLLIN) {
				uint8_t buf[256];
				ssize_t n;
				while ((n = snd_rawmidi_read(midi, buf, sizeof(buf))) > 0)
					for (ssize_t k = 0; k < n; k++)
						midi_byte(buf[k]);
			}
		}

		if (ifd >= 0 && (p[inject_at].revents & POLLIN)) {
			char text[512];
			ssize_t n = read(ifd, text, sizeof(text) - 1);
			text[n > 0 ? n : 0] = 0;
			unsigned sd, sb, sv;
			if (text[0] == 's' && sscanf(text + 1, "%x %x %x", &sd, &sb, &sv) == 3 &&
			    sd < NDEV && sb < devs[sd].crc_at) {
				frame[sd][sb] = sv;
				dirty[sd] = 1;
				text[0] = 0;
			}
			if (text[0] == 'h') {
				go_home();
				text[0] = 0;
			}
			// On-screen BEAT FX (aztouch zones): "f on|next|prev|beat+|beat-|type N",
			// and the X-PAD, "x TOUCH POS" (POS 0..255).
			if (text[0] == 'f' || text[0] == 'x') {
				bfx_touch_cmd(text);
				text[0] = 0;
			}
			unsigned sbit;
			if (text[0] == 'b' && sscanf(text + 1, "%x %x %x %x", &sd, &sb, &sbit, &sv) == 4 &&
			    sd < NDEV && sb < devs[sd].crc_at && sbit < 8) {
				if (sv)
					frame[sd][sb] |= 1 << sbit;
				else
					frame[sd][sb] &= ~(1 << sbit);
				dirty[sd] = 1;
				text[0] = 0;
			}
			int to_ctl = text[0] == 'o';
			uint8_t raw[256];
			int nraw = 0;
			for (char *q = text + to_ctl, *end; *q; q = end) {
				long b = strtol(q, &end, 16);
				if (end == q) {
					end++;
					continue;
				}
				if (!to_ctl)
					midi_byte((uint8_t)b);
				else if (nraw < (int)sizeof(raw))
					raw[nraw++] = (uint8_t)b;
			}
			if (nraw && midi_out)
				midi_write(midi_out, raw, nraw);
		}

		t = now();
		int tick = t >= next_tick;
		if (tick) {
			next_tick += TICK_MS / 1000.0;
			if (next_tick < t)
				next_tick = t + TICK_MS / 1000.0;
			jog_idle(&jogs[0]);
			jog_idle(&jogs[1]);
			pulses_tick();
			slip_mask_tick();
			return_check();
			layers_tick();
			bfx_report();
		}
		if (midi_out && !no_out) {
			int all = t >= next_led;
			if (all)
				next_led = t + (led_ms_over ? led_ms_over : cur->led_ms ? cur->led_ms : LED_MS) / 1000.0;
			pad_fx_leds();
			slip_leds(t);
			leds_send(midi_out, all);
			if (t >= next_meter) {
				next_meter = t + meter_ms / 1000.0;
				meters_send(midi_out);
			}
			static double jog_t;
			if (t - jog_t >= .01) {
				jogind_send(midi_out, jog_t ? t - jog_t : 0, all);
				jog_t = t;
			}
			bfxind_send(midi_out, all);
		}
		if (midi_out && cur->keepalive && (keepalive_opt || !cur->keepalive_opt) &&
		    t >= next_keepalive) {
			midi_write(midi_out, cur->keepalive, cur->keepalive_n);
			next_keepalive = t + KEEPALIVE_MS / 1000.0;
		}
		for (int i = 0; i < nconn; i++)
			if (conns[i].dev >= 0 && (tick || dirty[conns[i].dev]))
				send_frame(&conns[i]);
		if (mc_shm && (tick || dirty[DEV_MC]))
			memcpy(mc_shm, frame[DEV_MC], 128);
		memset(dirty, 0, sizeof(dirty));
	}
	unlink(sock_path);
	return 0;
}
