// azmix: stand-in for the XDJ-AZ's hardware mixer.
//
// The player (EP147) writes the deck audio to hw:ALPHATHETAD810,0 (8 ch, one
// stereo pair per deck) and the preview/headphone audio to hw:ALPHATHETAD810,1
// (2 ch), both float at 44100 Hz. On a Pi that card is snd-aloop, so the same
// audio comes out of the capture side of the other device number. azmix reads
// both, mixes and plays the result on the controller's sound card:
//   out 1/2  master  = the four channels after trim, EQ, colour FX,
//                      channel fader and crossfader, then the beat FX, times
//                      master level (+ preview with -p)
//   out 3/4  phones  = CUE'd channels (pre-fader) mixed with master by the
//                      headphones mixing knob, times headphones level, + preview
//                      (MONO SPLIT: cue left, master right)
//   REC (to the player)  the master as out 1/2 (no preview), into the player's
//                      audio input (snd-aloop ,1,1 -> ,0,1, 8 ch, master in
//                      1/2) for MASTER REC
//
// MIC: the controller's USB input (FLX4: the bridge turns off its own mic
// mix), through the Feedback Reducer (fx.c), times MIC LEVEL, into the master
// before the beat FX, bringing the music down while it speaks (talkover); in
// REC unless MIC Output to Master REC is off.
// The DDJ-GRV6 plays its mic into its master itself, and cannot be told not
// to; its USB input is its master as it plays it (ours with the mic). That
// is what REC takes from it while MIC Output to Master REC is on.
//
// MIXER SETTINGS on the player's screen reach azmix through the bridge
// (MC bytes 122..127): crossfader assign and curve, channel fader curve,
// EQ/ISO, headphones MONO SPLIT, LINK preview volume.
// The DDJ-GRV6 mixes its headphones itself (MASTER CUE, MIX, LEVEL): the
// bridge then holds MIX at CUE and LEVEL up, so out 3/4 is the cue bus alone.
//
// The control positions come from the bridge (azbridge), which publishes its
// mixer UCOM frame (the AZ's own layout, see UCOM-PROTOCOL.md §2.3) in
// MC_SHM. Without it the mixer is neutral: faders up, EQs flat.
//
// Per channel: trim +-12 dB; 3-band isolator (Linkwitz-Riley 4th order at
// 250 Hz and 2.5 kHz, mid = rest), each band kill..+6 dB; colour FX (fx.c)
// on the colour knob; channel CUE buttons toggle.
//
// COLOR FX: the AZ's six buttons (MC byte 21 bits 5..0 = index 0..5, the
// bridge presses them for SMART CFX) pick colorfx type 0..5 (fx.h order);
// pressing the one that is on turns it off, and while off the knob does
// nothing, as on the AZ.
//
// BEAT FX, on the master: the bridge plays the AZ's mixer MCU and keeps the
// effect's state in the frame bytes the MCU reports to the player
// (UCOM-PROTOCOL.md §2.3): 84 bit 7 on, 85 high nibble type (the player's
// order, see bfx_of_az),
// 90..91 the effect time in ms; the LEVEL/DEPTH knob at 69/70. Byte 120
// (after the CRC, the bridge's own) names the target: a channel, where the
// effect takes the channel after its fader and crossfader, so pulling the
// fader leaves the echo's tail (the user's habit), or 0xFF for the master.
//
// BRAKE: byte 121 (the bridge's too), bit n held = channel n slows to a stop
// over BRAKE_S, as a record does (the GRV6's DRUM RELEASE lever); let go, the
// channel is live again.
//
// The channels' peak levels (pre-fader, as the FLX4's meters show them) go
// to METER_SHM for the bridge, four floats.
//
// Clock: the player runs 64-frame periods, finer than the kernel tick (HZ=250)
// that snd-aloop uses by default, which makes its audio thread stall and the
// sound choppy. snd-aloop is therefore loaded with timer_source=hw:DDJGRV6,0
// (or DDJFLX4), so the loopback runs on the controller's clock. That clock only
// ticks while our output runs, so the output is opened and started first.
//
//   azmix [-o device] [-p]     default: hw:DDJGRV6 if there, else hw:DDJFLX4
//
// Build: gcc -O2 -Wall -o azmix azmix.c fx.c -lasound -lm

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fx.h"

#define RATE	44100
#define PERIOD	64		// frames, the player's own period (1.45 ms)
#define IN_BUFFER	(16 * PERIOD)
#define OUT_BUFFER	(16 * PERIOD)
// output latency kept after a (re)start: snd-aloop moves data in kernel ticks
// (4 ms at HZ=250) whatever its timer source, so more than one tick
#define PREFILL	(6 * PERIOD)
#define DUMP_FRAMES	(5 * RATE / PERIOD * PERIOD)	// SIGUSR1: last ~5 s of the phones
						// input (whole periods: ring_put)
#define DUMP_FILE	"/tmp/azmix-phones.f32"
// snd-aloop has two substreams per device: ours are named, so the player's
// opens get the other ones (playback ,0,0 / ,1,0 and capture ,0,1).
#define DECKS_DEV	"hw:ALPHATHETAD810,1,0"	// capture of the player's ,0
#define PHONES_DEV	"hw:ALPHATHETAD810,0,0"	// capture of the player's ,1
#define REC_DEV		"hw:ALPHATHETAD810,1,1"	// the player's input, ,0,1
#define REC_CH		8

static volatile sig_atomic_t stop;

static volatile sig_atomic_t dump;

static void on_signal(int sig) { if (sig == SIGUSR1) dump = 1; else stop = 1; }

// Ring of the phones input, written raw (float, 2 ch) to DUMP_FILE on SIGUSR1,
// to look for dropouts in what the player delivers.
static float ring[2 * DUMP_FRAMES];
static size_t ring_pos;

static void ring_put(const float *buf)
{
	memcpy(ring + 2 * ring_pos, buf, sizeof(float) * 2 * PERIOD);
	ring_pos = (ring_pos + PERIOD) % DUMP_FRAMES;
}

static void ring_dump(void)
{
	FILE *f = fopen(DUMP_FILE, "wb");
	if (!f)
		return;
	fwrite(ring + 2 * ring_pos, sizeof(float) * 2, DUMP_FRAMES - ring_pos, f);
	fwrite(ring, sizeof(float) * 2, ring_pos, f);
	fclose(f);
	fprintf(stderr, "azmix: phones input dumped to %s\n", DUMP_FILE);
}

static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir,
			   snd_pcm_format_t fmt, unsigned ch, snd_pcm_uframes_t buffer)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t period = PERIOD;
	unsigned rate = RATE;
	int err;

	snd_pcm_hw_params_alloca(&hw);
	if ((err = snd_pcm_open(&pcm, dev, dir, 0)) < 0 ||
	    (err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, ch)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0 ||
	    (dir == SND_PCM_STREAM_CAPTURE && (err = snd_pcm_start(pcm)) < 0)) {
		fprintf(stderr, "azmix: %s (%s, %u ch): %s\n", dev,
			snd_pcm_format_name(fmt), ch, snd_strerror(err));
		if (pcm)
			snd_pcm_close(pcm);
		return NULL;
	}
	return pcm;
}

// Queues PREFILL frames of silence; the output starts on the first write.
static void prime(snd_pcm_t *out)
{
	static const unsigned char zero[PREFILL * 8 * 4];	// widest frame: 8 x 32 bit
	snd_pcm_writei(out, zero, PREFILL);
}

// ---- microphone -------------------------------------------------------------
//
// The controller's USB input carries its MIC (the FLX4 in its "EffectMode",
// which the bridge sets: the unit no longer mixes the mic itself). It runs on
// the controller's clock, as our output, but arrives in bursts of USB
// packets that do not line up with our periods, so it goes through a FIFO:
// whatever is there is taken each period, and a period is handed on once
// MIC_START frames are in. What the FIFO never runs below over a second
// (the slack the bursts leave) is dropped down to MIC_SLACK, so the voice
// stays a few milliseconds behind without running dry.
//
// The FLX4's output is clocked from this input (implicit feedback): stopping
// or restarting the capture upsets the output, and with it the loopback's
// clock, which stalls the whole mixer. Opening the capture while the output
// runs does the same, so it is opened first, with a buffer that holds until
// the mixer's loop starts, and never restarted: on an error the mic goes
// quiet (logged) and the output carries on.
#define MIC_FIFO	4096		// frames, a power of two
#define MIC_START	(4 * PERIOD)
#define MIC_SLACK	(PERIOD / 2)
#define MIC_WINDOW	689		// periods, ~1 s
// MIC LEVEL at the top: a voice comes from the FLX4 at about -35 dBFS
// (measured, its ATT up), so the knob goes up to +20 dB.
#define MIC_GAIN_MAX	10.f
#define MIC_DUMP_FILE	"/tmp/azmix-mic.f32"	// SIGUSR1: last ~5 s, raw L/R

struct mic {
	snd_pcm_t *pcm;
	snd_pcm_format_t fmt;
	float gain;			// last period's, for the ramp
	float fifo[2 * MIC_FIFO];	// L/R
	unsigned rd, wr;		// free-running frame counters
	int running;			// MIC_START reached
	unsigned low, periods;		// lowest fill in this window, its age
	int failed;			// capture error: mic off for good
	unsigned xruns, short_periods;
	float dump[2 * DUMP_FRAMES];
	size_t dump_pos;
};

static void mic_open(struct mic *m, const char *dev)
{
	m->fmt = SND_PCM_FORMAT_S24_3LE;
	m->pcm = open_pcm(dev, SND_PCM_STREAM_CAPTURE, m->fmt, 2, MIC_FIFO);
	if (!m->pcm) {
		m->fmt = SND_PCM_FORMAT_S16_LE;
		m->pcm = open_pcm(dev, SND_PCM_STREAM_CAPTURE, m->fmt, 2, MIC_FIFO);
	}
	if (m->pcm) {
		snd_pcm_nonblock(m->pcm, 1);
		fprintf(stderr, "azmix: mic from %s (%s)\n", dev, snd_pcm_format_name(m->fmt));
	}
}

// Moves what the device has into the FIFO.
static void mic_fill(struct mic *m)
{
	unsigned char raw[256 * 2 * 3];
	snd_pcm_sframes_t avail = snd_pcm_avail_update(m->pcm);
	if (avail < 0) {
		fprintf(stderr, "azmix: mic %s, mic off\n", snd_strerror((int)avail));
		m->failed = 1;
		return;
	}
	if (avail > MIC_FIFO)		// never more than the device can hold
		avail = MIC_FIFO;
	while (avail > 0) {
		snd_pcm_sframes_t n = snd_pcm_readi(m->pcm, raw, avail < 256 ? avail : 256);
		if (n == -EAGAIN || n == 0)
			break;
		if (n > 0)
			avail -= n;
		if (n < 0) {		// see above: no restart
			fprintf(stderr, "azmix: mic %s, mic off\n", snd_strerror((int)n));
			m->xruns++;
			m->failed = 1;
			return;
		}
		for (int i = 0; i < n; i++, m->wr++) {
			float *d = m->fifo + 2 * (m->wr & (MIC_FIFO - 1));
			for (int c = 0; c < 2; c++) {
				if (m->fmt == SND_PCM_FORMAT_S24_3LE) {
					const unsigned char *b = raw + 3 * (2 * i + c);
					d[c] = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 |
							 (uint32_t)b[2] << 24) / 2147483648.f;
				} else {
					d[c] = ((int16_t *)raw)[2 * i + c] / 32768.f;
				}
			}
		}
	}
	if (m->wr - m->rd > MIC_FIFO)	// not read for a while: start over
		m->rd = m->wr, m->running = 0;
}

// One period of the mic, mono ((L + R) / 2: the FLX4 sends the same on
// both) and as it came (lr); silence until the FIFO has filled, and for what
// is missing after. Says whether the period was there.
static int mic_read(struct mic *m, float *buf, float *lr)
{
	memset(buf, 0, sizeof(float) * PERIOD);
	memset(lr, 0, sizeof(float) * 2 * PERIOD);
	if (!m->pcm || m->failed)
		return 0;
	mic_fill(m);
	unsigned have = m->wr - m->rd;
	if (!m->running && have >= MIC_START)
		m->running = 1, m->low = ~0u, m->periods = 0;
	if (!m->running)
		return 0;
	if (have < PERIOD) {
		m->short_periods++;
		if ((m->short_periods & (m->short_periods - 1)) == 0)
			fprintf(stderr, "azmix: mic short (%u)\n", m->short_periods);
		m->running = 0;			// refill to MIC_START
		return 0;
	}
	if (have - PERIOD < m->low)
		m->low = have - PERIOD;
	if (++m->periods >= MIC_WINDOW) {
		if (m->low > MIC_SLACK)
			m->rd += m->low - MIC_SLACK;
		m->low = ~0u, m->periods = 0;
	}
	for (int i = 0; i < PERIOD; i++, m->rd++) {
		const float *d = m->fifo + 2 * (m->rd & (MIC_FIFO - 1));
		buf[i] = (d[0] + d[1]) / 2;
		lr[2 * i] = d[0];
		lr[2 * i + 1] = d[1];
		m->dump[2 * m->dump_pos] = d[0];
		m->dump[2 * m->dump_pos + 1] = d[1];
		m->dump_pos = (m->dump_pos + 1) % DUMP_FRAMES;
	}
	return 1;
}

static void mic_dump(const struct mic *m)
{
	FILE *f = fopen(MIC_DUMP_FILE, "wb");
	if (!f)
		return;
	fwrite(m->dump + 2 * m->dump_pos, sizeof(float) * 2, DUMP_FRAMES - m->dump_pos, f);
	fwrite(m->dump, sizeof(float) * 2, m->dump_pos, f);
	fclose(f);
	fprintf(stderr, "azmix: mic dumped to %s\n", MIC_DUMP_FILE);
}

struct input {
	const char *dev;
	unsigned ch;
	snd_pcm_t *pcm;
};

// Reads exactly PERIOD frames; silence on any error. An overrun is recovered
// in place; anything else (the player gone or reconfigured) gets a fresh open.
static void read_period(struct input *in, float *buf)
{
	snd_pcm_sframes_t n = in->pcm ? snd_pcm_readi(in->pcm, buf, PERIOD) : -ENODEV;
	if (n == PERIOD)
		return;
	memset(buf, 0, sizeof(float) * in->ch * PERIOD);
	if (n == -EPIPE)
		fprintf(stderr, "azmix: %s overrun\n", in->dev);
	if (n >= 0 || (n == -EPIPE && snd_pcm_recover(in->pcm, (int)n, 1) == 0 &&
		       snd_pcm_start(in->pcm) == 0))
		return;
	if (in->pcm)
		snd_pcm_close(in->pcm);
	usleep(PERIOD * 1000000 / RATE);	// keep the output fed meanwhile
	in->pcm = open_pcm(in->dev, SND_PCM_STREAM_CAPTURE, SND_PCM_FORMAT_FLOAT_LE,
			   in->ch, IN_BUFFER);
	if (in->pcm)
		fprintf(stderr, "azmix: %s reopened\n", in->dev);
}

static float clip(float x) { return x > 1.f ? 1.f : x < -1.f ? -1.f : x; }

// ---- mixer controls -----------------------------------------------------------

#define MC_SHM	"/run/xdjaz/azmix-mc"	// the bridge's mixer frame, 128 bytes
#define NCH	4

static const uint8_t *mc;		// mapped MC_SHM, or NULL

static void mc_map(void)
{
	int fd = open(MC_SHM, O_RDONLY);
	if (fd < 0)
		return;
	void *p = mmap(NULL, 128, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (p != MAP_FAILED) {
		mc = p;
		fprintf(stderr, "azmix: mixer controls from %s\n", MC_SHM);
	}
}

// 10-bit value: high 8 bits at hi, low 2 bits at lo >> shift (0..1)
static float mc10(const uint8_t *f, int hi, int lo, int shift)
{
	return ((f[hi] << 2) | ((f[lo] >> shift) & 3)) / 1023.f;
}

// The player's beat FX numbering (read off its BEAT FX panel, 2026-11-06) ->
// fx.h's beatfx_type.
static const int bfx_of_az[14] = {
	BFX_DELAY, BFX_ECHO, BFX_PINGPONG, BFX_SPIRAL, BFX_HELIX, BFX_REVERB,
	BFX_FLANGER, BFX_PHASER, BFX_FILTER, BFX_TRIPLET_FILTER, BFX_TRANS,
	BFX_ROLL, BFX_TRIPLET_ROLL, BFX_MOBIUS,
};

struct controls {
	float trim[NCH], eq[NCH][3], color[NCH], fader[NCH];
	int cue_button[NCH];
	float xfader, master, hp_mix, hp_level;
	int cfx_buttons;		// bit i = COLOR FX button i held
	struct beatfx_params bfx;
	int bfx_ch;			// beat FX target channel, -1 master
	int bfx_mic;			// beat FX on the mic alone (bridge: SHIFT + lever)
	int brake;			// bit n: brake channel n
	// MIXER SETTINGS (bytes 122..127, see read_controls)
	int xf_assign[NCH];		// 0 THRU, 1 A, 2 B
	int fader_curve, xf_curve;	// 0..2, as the panel lists them
	int iso, mono_split;
	float preview_gain;		// LINK preview volume
	float mic_level;		// MIC LEVEL, 0..1
	int mic_rec;			// MIC Output to Master REC
	int fbr_mode;			// Feedback Reducer: 0 off, 1 light, 2 heavy
	int talk_adv;			// Talkover Mode: Advanced (mids only)
	float talk_gain;		// Talkover Level
};

static void read_controls(struct controls *c)
{
	if (!mc) {			// neutral
		for (int n = 0; n < NCH; n++) {
			c->trim[n] = c->color[n] = .5f;
			c->eq[n][0] = c->eq[n][1] = c->eq[n][2] = .5f;
			c->fader[n] = 1;
			c->cue_button[n] = 0;
		}
		c->xfader = .5f, c->master = 1, c->hp_mix = 1, c->hp_level = 1;
		c->cfx_buttons = 0;
		c->bfx = (struct beatfx_params){ -1, 0, .5f, 0 };
		c->bfx_ch = -1, c->bfx_mic = 0;
		c->brake = 0;
		for (int n = 0; n < NCH; n++)
			c->xf_assign[n] = n % 2 + 1;	// 1/3 on A, 2/4 on B
		c->fader_curve = 1, c->xf_curve = 1;
		c->iso = 1, c->mono_split = 0;
		c->preview_gain = 1;
		c->mic_level = 0, c->mic_rec = 1, c->fbr_mode = 0;
		c->talk_adv = 1, c->talk_gain = powf(10, -18 / 20.f);
		return;
	}
	uint8_t f[128];
	memcpy(f, mc, sizeof(f));	// one consistent copy per period
	for (int n = 0; n < NCH; n++) {
		int b = 36 + 5 * n;
		c->fader[n] = mc10(f, 10 + n, 14, 6 - 2 * n);
		c->trim[n] = mc10(f, b, b + 4, 6);
		c->eq[n][2] = mc10(f, b + 1, b + 4, 4);	// hi
		c->eq[n][1] = mc10(f, b + 2, b + 4, 2);	// mid
		c->eq[n][0] = mc10(f, b + 3, b + 4, 0);	// low
		c->color[n] = mc10(f, 61 + n, 65, 6 - 2 * n);
		c->cue_button[n] = (f[20] >> (3 - n)) & 1;
	}
	c->xfader = ((f[15] << 2) | (f[16] & 3)) / 1023.f;
	c->master = mc10(f, 56, 60, 6);
	c->hp_mix = mc10(f, 76, 80, 6);
	c->hp_level = mc10(f, 77, 80, 4);
	c->cfx_buttons = 0;
	for (int i = 0; i < 6; i++)
		c->cfx_buttons |= ((f[21] >> (5 - i)) & 1) << i;
	int ms = f[90] | f[91] << 8;
	c->bfx.type = f[85] >> 4 < 14 ? bfx_of_az[f[85] >> 4] : -1;
	c->bfx.on = f[84] >> 7;
	c->bfx.beat_s = (ms > 0 ? ms : 500) / 1000.f;
	c->bfx.depth = mc10(f, 69, 70, 0);
	c->bfx_ch = f[120] < NCH ? f[120] : -1;
	c->bfx_mic = f[120] == NCH;
	c->brake = f[121];
	// MIXER SETTINGS, from the player through the bridge (its MC_SETTINGS):
	// 122 input select (no inputs here), 123 crossfader assign, 124 curves,
	// EQ/ISO and headphones split, 125 LINK preview volume, 126..127 mic
	for (int n = 0; n < NCH; n++)
		c->xf_assign[n] = (f[123] >> (6 - 2 * n)) & 3;
	c->fader_curve = f[124] & 3;
	c->xf_curve = (f[124] >> 2) & 3;
	c->iso = (f[124] >> 4) & 1;
	c->mono_split = (f[124] >> 6) & 1;
	c->preview_gain = powf(10, -3 * (f[125] & 3) / 20.f);
	// MIC LEVEL (the bridge's, bytes 98/99) and MIC Output to Master REC
	// (byte 100, the player's TX 10 bit 6)
	c->mic_level = mc10(f, 98, 99, 6);
	c->mic_rec = (f[100] >> 6) & 1;
	c->fbr_mode = f[126] & 3;
	if (c->fbr_mode > 2)
		c->fbr_mode = 0;
	// Talkover Mode and Level (UTILITY > Mixer; byte 101, the player's TX 11)
	c->talk_adv = (f[101] >> 4) & 1;
	c->talk_gain = powf(10, -6 * ((f[101] & 3) + 1) / 20.f);
}

#define METER_SHM	"/run/xdjaz/azmix-meter"	// float peak[NCH], linear
static float *meter;

static void meter_map(void)
{
	int fd = open(METER_SHM, O_RDWR | O_CREAT, 0644);
	if (fd < 0 || ftruncate(fd, NCH * sizeof(float)) < 0) {
		if (fd >= 0)
			close(fd);
		return;
	}
	void *p = mmap(NULL, NCH * sizeof(float), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p != MAP_FAILED)
		meter = p;
}

// ---- DSP -------------------------------------------------------------------

struct biquad {
	float b0, b1, b2, a1, a2;
	float s1[2], s2[2];		// stereo state
};

// RBJ low/high-pass
static void bq_set(struct biquad *q, int highpass, float fc, float Q)
{
	float w = 2 * (float)M_PI * fc / RATE, cw = cosf(w), al = sinf(w) / (2 * Q);
	float a0 = 1 + al;
	if (highpass) {
		q->b0 = q->b2 = (1 + cw) / 2 / a0;
		q->b1 = -(1 + cw) / a0;
	} else {
		q->b0 = q->b2 = (1 - cw) / 2 / a0;
		q->b1 = (1 - cw) / a0;
	}
	q->a1 = -2 * cw / a0;
	q->a2 = (1 - al) / a0;
}

static inline float bq_run(struct biquad *q, int c, float x)
{
	float y = q->b0 * x + q->s1[c];
	q->s1[c] = q->b1 * x - q->a1 * y + q->s2[c];
	q->s2[c] = q->b2 * x - q->a2 * y;
	return y;
}

#define BRAKE_S		1.0		// slowing down to a stop
#define BRAKE_RING	65536		// frames, > the lag a brake builds (BRAKE_S / 2)

#define XOVER_LOW	250.f
#define XOVER_HIGH	2500.f

struct channel {
	struct biquad low[2], high[2];		// LR4 = two Butterworth stages
	float g_trim, g_eq[3], g_post;		// gains of the last period (ramps)
	int cue_on, cue_was;
	float peak;				// meter, decaying
	float buf[2 * PERIOD];			// this period, after trim, EQ, colour
	float ring[2 * BRAKE_RING];		// brake: the last frames
	unsigned wpos;
	int braking;
	double rpos, speed;
	float live;				// back from a brake: fade-in gain
};

static struct channel chans[NCH];
static struct colorfx cfx[NCH];
static struct beatfx bfx;
static struct beatfx bfx_rec;	// the master's beat FX without the mic, for REC
static struct fbr fbr;		// MIC Feedback Reducer
static int cfx_type = -1, cfx_was;	// -1: COLOR FX off

static void chans_init(void)
{
	for (int n = 0; n < NCH; n++)
		for (int k = 0; k < 2; k++) {
			bq_set(&chans[n].low[k], 0, XOVER_LOW, (float)M_SQRT1_2);
			bq_set(&chans[n].high[k], 1, XOVER_HIGH, (float)M_SQRT1_2);
		}
}

// The channel as a record braked by hand while held: read back from its own
// recent audio at a speed falling from 1 to 0, quieter as it slows.
static void brake_run(struct channel *ch, int held)
{
	if (held && !ch->braking)
		ch->speed = 1, ch->rpos = ch->wpos;
	if (!held && ch->braking)
		ch->live = 0;
	ch->braking = held;
	for (int i = 0; i < PERIOD; i++) {
		unsigned w = ch->wpos;
		ch->wpos = (w + 1) % BRAKE_RING;
		ch->ring[2 * w] = ch->buf[2 * i];
		ch->ring[2 * w + 1] = ch->buf[2 * i + 1];
		if (!held) {
			if (ch->live < 1) {
				ch->live = fminf(1, ch->live + 1 / (.005f * RATE));
				ch->buf[2 * i] *= ch->live, ch->buf[2 * i + 1] *= ch->live;
			}
			continue;
		}
		unsigned r = (unsigned)ch->rpos;
		float a = (float)(ch->rpos - r), g = (float)ch->speed;
		for (int c = 0; c < 2; c++) {
			float x0 = ch->ring[2 * (r % BRAKE_RING) + c];
			float x1 = ch->ring[2 * ((r + 1) % BRAKE_RING) + c];
			ch->buf[2 * i + c] = (x0 + (x1 - x0) * a) * g;
		}
		ch->rpos += ch->speed;
		if (ch->rpos >= BRAKE_RING)
			ch->rpos -= BRAKE_RING;
		ch->speed = fmax(0, ch->speed - 1 / (BRAKE_S * RATE));
	}
}

static float trim_gain(float v) { return powf(10, (v - .5f) * 24 / 20); }	// +-12 dB

// flat at the centre, +6 dB at the top; at 0 ISO kills the band, EQ only
// takes it down to -26 dB (the DJM's two modes)
static float eq_gain(float v, int iso)
{
	float floor = iso ? 0 : .05f;
	return v < .5f ? floor + (1 - floor) * (v * 2) * (v * 2) : 1 + (v - .5f) * 2;
}

// Channel fader curve, the panel's three: 0 rises late, 1 the usual,
// 2 comes up at once.
static float fader_gain(float v, int curve)
{
	if (curve == 0)
		return v * v * v * v;
	if (curve == 2)
		return 1 - (1 - v) * (1 - v) * (1 - v);
	return v * v;
}

// Crossfader gain of the side the crossfader moves away from at x = 1 (A),
// by the panel's curves: 2 a sharp cut (scratching), 1 full to the centre
// then down, 0 a smooth blend.
#define XF_CUT	.04f
static float xf_gain(float x, int curve)
{
	if (curve == 2)
		return x < 1 - XF_CUT ? 1 : (1 - x) / XF_CUT;
	if (curve == 0)
		return cosf(x * (float)M_PI / 2);
	return fminf(1, 2 * (1 - x));
}

// ---- talkover -----------------------------------------------------------------
//
// The AZ's mic switch has a TALK OVER position: while the voice is above a
// threshold, the music goes down by Talkover Level, all of it (Normal) or
// only its middle, where a voice is (Advanced: 250 Hz .. 2.5 kHz, the
// isolator's bands). No controller here has that switch, so it is on while
// the mic is: the voice itself brings the music down.
#define TALK_THRESHOLD	.03f		// mic peak after MIC LEVEL, ~-30 dBFS
#define TALK_HOLD_S	.4f		// stays down this long after the voice
#define TALK_ATTACK_S	.02f
#define TALK_RELEASE_S	.5f

static struct {
	struct biquad low[2], high[2];	// LR4 split of the master
	float env;			// 0 music as is .. 1 fully talked over
	float hold;			// seconds left
} talk;

static void talk_init(void)
{
	for (int k = 0; k < 2; k++) {
		bq_set(&talk.low[k], 0, XOVER_LOW, (float)M_SQRT1_2);
		bq_set(&talk.high[k], 1, XOVER_HIGH, (float)M_SQRT1_2);
	}
}

// Brings the music (lr, PERIOD frames) down while the voice (mono, PERIOD)
// speaks.
static void talk_run(float *lr, const float *voice, const struct controls *c)
{
	float pk = 0;
	for (int i = 0; i < PERIOD; i++)
		pk = fmaxf(pk, fabsf(voice[i]));
	float dt = (float)PERIOD / RATE;
	if (pk > TALK_THRESHOLD)
		talk.hold = TALK_HOLD_S;
	else
		talk.hold = fmaxf(0, talk.hold - dt);
	float e0 = talk.env, target = talk.hold > 0;
	float e1 = talk.env = target > e0 ? fminf(target, e0 + dt / TALK_ATTACK_S)
					  : fmaxf(target, e0 - dt / TALK_RELEASE_S);
	if (e0 == 0 && e1 == 0)
		return;
	for (int i = 0; i < PERIOD; i++) {
		float e = e0 + (e1 - e0) * (i + 1) / PERIOD;
		float g = 1 + (c->talk_gain - 1) * e;
		for (int ch = 0; ch < 2; ch++) {
			float x = lr[2 * i + ch];
			if (!c->talk_adv) {
				lr[2 * i + ch] = x * g;
				continue;
			}
			float lo = bq_run(&talk.low[1], ch, bq_run(&talk.low[0], ch, x));
			float hi = bq_run(&talk.high[1], ch, bq_run(&talk.high[0], ch, x));
			lr[2 * i + ch] = lo + hi + (x - lo - hi) * g;
		}
	}
}

int main(int argc, char **argv)
{
	const char *out_dev = NULL;
	int preview_master = 0, opt;

	while ((opt = getopt(argc, argv, "o:p")) != -1) {
		if (opt == 'o') out_dev = optarg;
		else if (opt == 'p') preview_master = 1;
		else { fprintf(stderr, "usage: azmix [-o device] [-p]\n"); return 2; }
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_signal);
	struct sched_param sp = { .sched_priority = 80 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		fprintf(stderr, "azmix: no realtime priority, running normal\n");

	// the controllers take S24_3LE or S16_LE
	static const char *const cards[] = { "hw:DDJGRV6", "hw:DDJFLX4" };
	snd_pcm_format_t ofmt = SND_PCM_FORMAT_S24_3LE;
	snd_pcm_t *out = NULL;
	static struct mic mic;
	int mic_hw = 0;			// GRV6: the controller plays the mic itself
	for (int i = 0; !out && i < (out_dev ? 1 : 2); i++) {
		char path[32];
		snprintf(path, sizeof(path), "/proc/asound/%s", cards[i] + 3);
		if (!out_dev && access(path, F_OK) < 0)
			continue;
		const char *dev = out_dev ? out_dev : cards[i];
		mic_open(&mic, dev);	// before the output: see the mic's notes
		ofmt = SND_PCM_FORMAT_S24_3LE;
		out = open_pcm(dev, SND_PCM_STREAM_PLAYBACK, ofmt, 4, OUT_BUFFER);
		if (!out) {
			ofmt = SND_PCM_FORMAT_S16_LE;
			out = open_pcm(dev, SND_PCM_STREAM_PLAYBACK, ofmt, 4, OUT_BUFFER);
		}
		if (out) {
			out_dev = dev;
			mic_hw = strstr(dev, "GRV6") != NULL;
		}
	}
	if (!out)
		return 1;
	prime(out);

	struct input decks = { DECKS_DEV, 8, NULL }, phones = { PHONES_DEV, 2, NULL };
	decks.pcm = open_pcm(decks.dev, SND_PCM_STREAM_CAPTURE, SND_PCM_FORMAT_FLOAT_LE,
			     decks.ch, IN_BUFFER);
	phones.pcm = open_pcm(phones.dev, SND_PCM_STREAM_CAPTURE, SND_PCM_FORMAT_FLOAT_LE,
			      phones.ch, IN_BUFFER);
	if (!decks.pcm || !phones.pcm)
		return 1;
	// MASTER REC: the player records its audio input (the AZ's mixer sends
	// it the master); here the master as it is heard, after the MASTER level
	// (before it the sum of four channels has no headroom and clips). Never
	// waits: what the loopback cannot take is dropped.
	// The player's input is the capture side of its "outs" (asound.conf: a
	// multi of ,0 x8 and ,1 x2; JUCE probes "outs" for input and uses the
	// first two channels), so this side has 8 channels, the master in 1/2.
	snd_pcm_t *rec = open_pcm(REC_DEV, SND_PCM_STREAM_PLAYBACK, SND_PCM_FORMAT_S32_LE,
				  REC_CH, IN_BUFFER);
	if (rec) {
		snd_pcm_nonblock(rec, 1);
		prime(rec);
		snd_pcm_start(rec);
	}
	fprintf(stderr, "azmix: decks %s + phones %s -> %s (%s)\n", DECKS_DEV,
		PHONES_DEV, out_dev, snd_pcm_format_name(ofmt));

	float dbuf[8 * PERIOD], pbuf[2 * PERIOD];
	unsigned char obuf[4 * 3 * PERIOD];
	unsigned underruns = 0, map_tries = 0;

	chans_init();
	talk_init();
	for (int n = 0; n < NCH; n++)
		colorfx_init(&cfx[n], RATE);
	beatfx_init(&bfx, RATE);
	beatfx_init(&bfx_rec, RATE);
	fbr_init(&fbr, RATE);
	fx_flush_denormals();
	mc_map();
	meter_map();

	while (!stop) {
		read_period(&decks, dbuf);
		read_period(&phones, pbuf);
		ring_put(pbuf);
		if (dump) {
			dump = 0;
			ring_dump();
			mic_dump(&mic);
		}

		if (!mc && ++map_tries % 700 == 0)	// about once a second
			mc_map();
		struct controls ctl;
		read_controls(&ctl);

		// target gains for this period; each ramps from the last period's
		float t_trim[NCH], t_eq[NCH][3], t_post[NCH];
		float xa = xf_gain(ctl.xfader, ctl.xf_curve), xb = xf_gain(1 - ctl.xfader, ctl.xf_curve);
		for (int n = 0; n < NCH; n++) {
			struct channel *ch = &chans[n];
			if (ctl.cue_button[n] && !ch->cue_was)	// CUE toggles
				ch->cue_on = !ch->cue_on;
			ch->cue_was = ctl.cue_button[n];
			t_trim[n] = trim_gain(ctl.trim[n]);
			for (int k = 0; k < 3; k++)
				t_eq[n][k] = eq_gain(ctl.eq[n][k], ctl.iso);
			// CROSSFADER ASSIGN: THRU, A or B
			int as = ctl.xf_assign[n];
			t_post[n] = fader_gain(ctl.fader[n], ctl.fader_curve) *
				    (as == 1 ? xa : as == 2 ? xb : 1);
		}
		for (int i = 0; i < 6; i++)		// COLOR FX: a press picks or ends
			if ((ctl.cfx_buttons & ~cfx_was) & (1 << i))
				cfx_type = cfx_type == i ? -1 : i;
		cfx_was = ctl.cfx_buttons;
		float g_master = ctl.master * ctl.master, g_hp = ctl.hp_level * ctl.hp_level;
		// one beat FX state: on a lever move it carries on at the new target
		int bfx_at = ctl.bfx_ch;

		// channels: trim, EQ, colour FX, meter
		for (int n = 0; n < NCH; n++) {
			struct channel *ch = &chans[n];
			float peak = 0;
			for (int i = 0; i < PERIOD; i++) {
				const float *d = dbuf + 8 * i;
				float r = (i + 1) / (float)PERIOD;	// ramp position
				float gt = ch->g_trim + (t_trim[n] - ch->g_trim) * r;
				float gl = ch->g_eq[0] + (t_eq[n][0] - ch->g_eq[0]) * r;
				float gm = ch->g_eq[1] + (t_eq[n][1] - ch->g_eq[1]) * r;
				float gh = ch->g_eq[2] + (t_eq[n][2] - ch->g_eq[2]) * r;
				for (int c = 0; c < 2; c++) {
					float x = d[2 * n + c] * gt;
					float lo = bq_run(&ch->low[1], c, bq_run(&ch->low[0], c, x));
					float hi = bq_run(&ch->high[1], c, bq_run(&ch->high[0], c, x));
					ch->buf[2 * i + c] = lo * gl + (x - lo - hi) * gm + hi * gh;
				}
			}
			colorfx_process(&cfx[n], cfx_type, cfx_type < 0 ? .5f : ctl.color[n],
					ch->buf, PERIOD);
			brake_run(ch, (ctl.brake >> n) & 1);
			for (int i = 0; i < 2 * PERIOD; i++)
				peak = fmaxf(peak, fabsf(ch->buf[i]));
			ch->peak = fmaxf(peak, ch->peak * .97f);	// ~50 ms per 6 dB
			if (meter)
				meter[n] = ch->peak;
		}

		// master and cue sums, then the beat FX on the master; a channel's
		// beat FX takes it after the fader, into fbuf
		float mbuf[2 * PERIOD], cbuf[2 * PERIOD], fbuf[2 * PERIOD];
		int32_t rbuf[REC_CH * PERIOD] = { 0 };
		for (int i = 0; i < PERIOD; i++) {
			float r = (i + 1) / (float)PERIOD;
			for (int c = 0; c < 2; c++) {
				float m = 0, cue = 0;
				for (int n = 0; n < NCH; n++) {
					struct channel *ch = &chans[n];
					float gp = ch->g_post + (t_post[n] - ch->g_post) * r;
					float y = ch->buf[2 * i + c];
					if (n == bfx_at)
						fbuf[2 * i + c] = y * gp;
					else
						m += y * gp;
					if (ch->cue_on)
						cue += y;
				}
				mbuf[2 * i + c] = m;
				cbuf[2 * i + c] = cue;
			}
		}
		// MIC: into the master before its beat FX (an effect on the master
		// takes the voice too); left out of REC when MIC Output to Master
		// REC is off, which with the effect on the master needs the master's
		// effect a second time without it.
		float micb[PERIOD], mic_dry[2 * PERIOD], hw_rec[2 * PERIOD];
		int hw_ok = mic_read(&mic, micb, hw_rec);
		if (mic_hw)		// not the mic alone: the controller's master
			memset(micb, 0, sizeof(micb));
		float howl_hz;
		if (fbr_process(&fbr, ctl.fbr_mode, micb, PERIOD, &howl_hz))
			fprintf(stderr, "azmix: feedback at %.0f Hz, notch %u\n", howl_hz, fbr.set);
		float g_mic = ctl.mic_level * ctl.mic_level * MIC_GAIN_MAX;
		for (int i = 0; i < PERIOD; i++) {
			float gm = mic.gain + (g_mic - mic.gain) * (i + 1) / PERIOD;
			mic_dry[2 * i] = mic_dry[2 * i + 1] = micb[i] * gm;
		}
		mic.gain = g_mic;
		float voice[PERIOD];
		for (int i = 0; i < PERIOD; i++)
			voice[i] = mic_dry[2 * i];
		talk_run(mbuf, voice, &ctl);
		int rec_nomic = !ctl.mic_rec && mic.pcm;
		float rmbuf[2 * PERIOD];
		if (rec_nomic)
			memcpy(rmbuf, mbuf, sizeof(rmbuf));
		if (ctl.bfx_mic)	// the beat FX on the voice alone
			beatfx_process(&bfx, &ctl.bfx, mic_dry, PERIOD);
		for (int i = 0; i < 2 * PERIOD; i++)
			mbuf[i] += mic_dry[i];
		if (ctl.bfx_mic)
			;
		else if (bfx_at < 0) {
			beatfx_process(&bfx, &ctl.bfx, mbuf, PERIOD);
			if (rec_nomic)
				beatfx_process(&bfx_rec, &ctl.bfx, rmbuf, PERIOD);
		} else {
			beatfx_process(&bfx, &ctl.bfx, fbuf, PERIOD);
			for (int i = 0; i < 2 * PERIOD; i++)
				mbuf[i] += fbuf[i];
			if (rec_nomic)
				for (int i = 0; i < 2 * PERIOD; i++)
					rmbuf[i] = mbuf[i] - mic_dry[i];
		}
		const float *recm = rec_nomic ? rmbuf : mbuf;
		// GRV6: REC from its master (with the mic) while that is on and
		// there, else ours; a period's crossfade between them
		static float hw_mix;
		float hw0 = hw_mix, hw1 = hw_mix = mic_hw && ctl.mic_rec && hw_ok;

		for (int i = 0; i < PERIOD; i++) {
			const float *p = pbuf + 2 * i, *m = mbuf + 2 * i, *cue = cbuf + 2 * i;
			float o[4];
			for (int c = 0; c < 2; c++) {
				float pv = p[c] * ctl.preview_gain;
				o[c] = m[c] * g_master + (preview_master ? pv : 0);
				o[2 + c] = (cue[c] * (1 - ctl.hp_mix) + m[c] * g_master * ctl.hp_mix) * g_hp + pv;
				float h = hw0 + (hw1 - hw0) * (i + 1) / PERIOD;
				float rv = recm[2 * i + c] * g_master * (1 - h) + hw_rec[2 * i + c] * h;
				rbuf[REC_CH * i + c] = (int32_t)(clip(rv) * 2147483520.f);
			}
			if (ctl.mono_split) {	// MONO SPLIT: cue left, master right
				float pv = (p[0] + p[1]) / 2 * ctl.preview_gain;
				o[2] = (cue[0] + cue[1]) / 2 * g_hp + pv;
				o[3] = (m[0] + m[1]) / 2 * g_master * g_hp;
			}
			for (int c = 0; c < 4; c++) {
				float x = clip(o[c]);
				if (ofmt == SND_PCM_FORMAT_S24_3LE) {
					int32_t v = (int32_t)(x * 8388607.f);
					unsigned char *b = obuf + 3 * (4 * i + c);
					b[0] = v; b[1] = v >> 8; b[2] = v >> 16;
				} else {
					((int16_t *)obuf)[4 * i + c] = (int16_t)(x * 32767.f);
				}
			}
		}
		for (int n = 0; n < NCH; n++) {
			chans[n].g_trim = t_trim[n];
			memcpy(chans[n].g_eq, t_eq[n], sizeof(t_eq[n]));
			chans[n].g_post = t_post[n];
		}
		if (rec) {
			snd_pcm_sframes_t r = snd_pcm_writei(rec, rbuf, PERIOD);
			if (r < 0 && r != -EAGAIN && snd_pcm_recover(rec, (int)r, 1) == 0) {
				prime(rec);
				snd_pcm_start(rec);
			}
		}
		snd_pcm_sframes_t n = snd_pcm_writei(out, obuf, PERIOD);
		if (n < 0) {
			underruns++;
			if ((underruns & (underruns - 1)) == 0)	// log 1, 2, 4, 8, ...
				fprintf(stderr, "azmix: output %s (%u)\n", snd_strerror((int)n), underruns);
			if (snd_pcm_recover(out, (int)n, 1) == 0)
				prime(out);
		}
	}
	snd_pcm_close(out);
	if (rec) snd_pcm_close(rec);
	if (mic.pcm) snd_pcm_close(mic.pcm);
	if (phones.pcm) snd_pcm_close(phones.pcm);
	if (decks.pcm) snd_pcm_close(decks.pcm);
	return 0;
}
