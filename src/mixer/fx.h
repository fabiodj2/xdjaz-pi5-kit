// fx.h: beat FX and sound colour FX for azmix, the XDJ-AZ mixer stand-in.
//
// Plain C, no allocation: every instance is one fixed-size struct (put them
// in static storage, they are too big for a stack). *_init may memset; the
// process calls only do arithmetic on the struct, so they are safe in the
// SCHED_FIFO audio thread. Audio is interleaved stereo float, processed in
// place, any frame count (internally split into FX_BLOCK pieces).
//
// Controls may change on every call: levels and filter settings are slewed
// inside, delay time changes crossfade between two taps, effect type changes
// fade the old effect out (10 ms) before the new one starts.
//
// Call fx_flush_denormals() once from the audio thread: decaying tails and
// idle filters otherwise reach denormal floats, which are slow on x86.

#ifndef AZ_FX_H
#define AZ_FX_H

#include <stdint.h>

#define FX_MAX_RATE	48000
#define FX_BLOCK	64			// control rate: params read once per this many frames
#define FX_BEAT_FRAMES	(1 << 18)		// beat FX buffer: 5.9 s at 44.1 kHz (2 MB)
#define FX_DUB_FRAMES	(1 << 16)		// colour dub echo buffer: 1.48 s at 44.1 kHz
#define FX_MAX_BEAT_S	5.0f			// longest delay / roll / helix time
#define FX_VERB_N	8			// reverb: delay lines in the feedback network
#define FX_VERB_LEN	4096
#define FX_VERB_AP	4			// reverb: input diffusers per side
#define FX_AP_LEN	1024
#define FX_PRE_LEN	2048
#define FX_SHIFT_LEN	2048			// spiral: pitch shifter window buffer
#define FX_SAP_LEN	1024			// spiral: in-loop diffusers
#define FX_FLANGE_LEN	1024
#define FX_PHASER_N	6
#define FX_MOBIUS_N	6

// ---- shared pieces (internal, here so the structs are complete) ----------

struct fx_svf { float ic1[2], ic2[2]; };	// state of a stereo state-variable filter

struct fx_tap {				// delay read position, crossfaded on change
	float cur, nxt, x, step;
	int fading;
};

struct fx_line {			// write side of a stereo ring buffer
	uint32_t wpos, fill, quiet;	// fill: frames written since reset (reads beyond are 0)
	struct fx_tap tap;
};

struct fx_verb {			// 8-line feedback delay network reverb
	float line[FX_VERB_N][FX_VERB_LEN];
	float ap[2][FX_VERB_AP][FX_AP_LEN];
	float pre[2][FX_PRE_LEN];
	float lp[FX_VERB_N], g[FX_VERB_N];
	int len[FX_VERB_N], aplen[2][FX_VERB_AP], prelen;
	uint32_t pos;
	float rate, t60, damp_hz, damp;
};

// ---- beat FX -------------------------------------------------------------

enum beatfx_type {
	BFX_DELAY, BFX_ECHO, BFX_PINGPONG, BFX_SPIRAL, BFX_REVERB, BFX_TRANS,
	BFX_FILTER, BFX_TRIPLET_FILTER, BFX_FLANGER, BFX_PHASER, BFX_ROLL,
	BFX_TRIPLET_ROLL, BFX_HELIX, BFX_MOBIUS, BFX_COUNT
};

struct fx_spiral {
	float sh[2][FX_SHIFT_LEN];
	float ap[2][2][FX_SAP_LEN];
	int aplen[2];
	float ph, d, win;
	uint32_t pos;
};
struct fx_roll { uint32_t rec, pp, xsrc, xfc; };
struct fx_flanger { float buf[2][FX_FLANGE_LEN]; float d; uint32_t pos; };
struct fx_phaser { float s[2][FX_PHASER_N], y[2], a; };
struct fx_mobius { struct fx_svf f[FX_MOBIUS_N]; float w[FX_MOBIUS_N]; };
struct fx_helix { float lfo, mod; };

struct beatfx_params {
	int type;		// enum beatfx_type; anything else = bypass
	int on;
	float beat_s;		// effect time / cycle in seconds (selected beat fraction at the BPM)
	float depth;		// LEVEL/DEPTH knob 0..1
};

struct beatfx {
	float rate;
	int type;		// effect the state below belongs to, -1 none
	int idle;		// nothing sounding: process returns at once
	int was_on, fresh;
	float sw;		// type change fade-out gain
	float mix, ing, lvl;	// slewed: insert mix, input gate, wet level
	float phase, gate, build, gin, fb;
	uint32_t count;		// frames since engage (helix capture, flanger warm-up)
	int xf;			// splice crossfade length, frames
	float c_fast, c_slow;	// block-rate slew coefficients
	float lpz[2], hpz[2], lpa, hpa;	// one-pole loop filters
	struct fx_line ln;
	union {				// per-effect state, reset on engage
		struct fx_spiral spiral;
		struct fx_verb verb;
		struct fx_roll roll;
		struct fx_flanger flanger;
		struct fx_phaser phaser;
		struct fx_mobius mobius;
		struct fx_svf svf;	// filter
		struct fx_helix helix;
	} u;
	float buf[2 * FX_BEAT_FRAMES];		// echo family, roll, helix
};

void beatfx_init(struct beatfx *fx, float rate);
void beatfx_process(struct beatfx *fx, const struct beatfx_params *p, float *lr, int frames);

// ---- sound colour FX -----------------------------------------------------

enum colorfx_type { CFX_SPACE, CFX_DUB_ECHO, CFX_SWEEP, CFX_NOISE, CFX_CRUSH, CFX_FILTER, CFX_COUNT };

struct colorfx {
	float rate;
	int type, idle;
	float sw;
	float knob;		// slewed knob position
	float ahold;		// amount, held with a slow release (tail colour)
	int hside;		// side of the held amount, -1 left, 1 right
	float c_knob;
	float send, w, lvl;	// per-effect slewed gains
	struct fx_svf f[2];
	float env[2], gain;	// sweep gate
	float hold[2], hph;	// crush sample-and-hold
	uint32_t rng;		// noise
	float dub_s;		// dub echo time
	struct fx_line ln;
	union {
		struct fx_verb verb;
		float dub[2 * FX_DUB_FRAMES];
	} u;
};

void colorfx_init(struct colorfx *fx, float rate);
// knob 0..1, 0.5 = off (dead zone 0.485..0.515, same as azmix's colour filter)
void colorfx_process(struct colorfx *fx, int type, float knob, float *lr, int frames);
// dub echo time in seconds (default 0.375, e.g. 3/4 beat at 120 BPM); takes effect smoothly
void colorfx_set_time(struct colorfx *fx, float seconds);

void fx_flush_denormals(void);

#endif
