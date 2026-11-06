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
#define DECKS_DEV	"hw:ALPHATHETAD810,1"	// capture of the player's ,0
#define PHONES_DEV	"hw:ALPHATHETAD810,0"	// capture of the player's ,1

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
	static const unsigned char zero[PREFILL * 4 * 3];	// widest frame: 4 x 24 bit
	snd_pcm_writei(out, zero, PREFILL);
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

#define MC_SHM	"/dev/shm/azmix-mc"	// the bridge's mixer frame, 128 bytes
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
	int brake;			// bit n: brake channel n
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
		c->bfx_ch = -1;
		c->brake = 0;
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
	c->brake = f[121];
}

#define METER_SHM	"/dev/shm/azmix-meter"	// float peak[NCH], linear
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

// kill at 0, flat at the centre, +6 dB at the top
static float eq_gain(float v) { return v < .5f ? (v * 2) * (v * 2) : 1 + (v - .5f) * 2; }

static float fader_gain(float v) { return v * v; }

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
	for (int i = 0; !out && i < (out_dev ? 1 : 2); i++) {
		char path[32];
		snprintf(path, sizeof(path), "/proc/asound/%s", cards[i] + 3);
		if (!out_dev && access(path, F_OK) < 0)
			continue;
		const char *dev = out_dev ? out_dev : cards[i];
		ofmt = SND_PCM_FORMAT_S24_3LE;
		out = open_pcm(dev, SND_PCM_STREAM_PLAYBACK, ofmt, 4, OUT_BUFFER);
		if (!out) {
			ofmt = SND_PCM_FORMAT_S16_LE;
			out = open_pcm(dev, SND_PCM_STREAM_PLAYBACK, ofmt, 4, OUT_BUFFER);
		}
		if (out)
			out_dev = dev;
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
	fprintf(stderr, "azmix: decks %s + phones %s -> %s (%s)\n", DECKS_DEV,
		PHONES_DEV, out_dev, snd_pcm_format_name(ofmt));

	float dbuf[8 * PERIOD], pbuf[2 * PERIOD];
	unsigned char obuf[4 * 3 * PERIOD];
	unsigned underruns = 0, map_tries = 0;

	chans_init();
	for (int n = 0; n < NCH; n++)
		colorfx_init(&cfx[n], RATE);
	beatfx_init(&bfx, RATE);
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
		}

		if (!mc && ++map_tries % 700 == 0)	// about once a second
			mc_map();
		struct controls ctl;
		read_controls(&ctl);

		// target gains for this period; each ramps from the last period's
		float t_trim[NCH], t_eq[NCH][3], t_post[NCH];
		float xa = fminf(1, 2 * (1 - ctl.xfader)), xb = fminf(1, 2 * ctl.xfader);
		for (int n = 0; n < NCH; n++) {
			struct channel *ch = &chans[n];
			if (ctl.cue_button[n] && !ch->cue_was)	// CUE toggles
				ch->cue_on = !ch->cue_on;
			ch->cue_was = ctl.cue_button[n];
			t_trim[n] = trim_gain(ctl.trim[n]);
			for (int k = 0; k < 3; k++)
				t_eq[n][k] = eq_gain(ctl.eq[n][k]);
			// channels 1 and 3 on the crossfader's A side, 2 and 4 on B
			t_post[n] = fader_gain(ctl.fader[n]) * (n % 2 ? xb : xa);
		}
		for (int i = 0; i < 6; i++)		// COLOR FX: a press picks or ends
			if ((ctl.cfx_buttons & ~cfx_was) & (1 << i))
				cfx_type = cfx_type == i ? -1 : i;
		cfx_was = ctl.cfx_buttons;
		float g_master = fader_gain(ctl.master), g_hp = fader_gain(ctl.hp_level);
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
		if (bfx_at < 0) {
			beatfx_process(&bfx, &ctl.bfx, mbuf, PERIOD);
		} else {
			beatfx_process(&bfx, &ctl.bfx, fbuf, PERIOD);
			for (int i = 0; i < 2 * PERIOD; i++)
				mbuf[i] += fbuf[i];
		}

		for (int i = 0; i < PERIOD; i++) {
			const float *p = pbuf + 2 * i, *m = mbuf + 2 * i, *cue = cbuf + 2 * i;
			float o[4];
			for (int c = 0; c < 2; c++) {
				o[c] = m[c] * g_master + (preview_master ? p[c] : 0);
				o[2 + c] = (cue[c] * (1 - ctl.hp_mix) + m[c] * g_master * ctl.hp_mix) * g_hp + p[c];
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
	if (phones.pcm) snd_pcm_close(phones.pcm);
	if (decks.pcm) snd_pcm_close(decks.pcm);
	return 0;
}
