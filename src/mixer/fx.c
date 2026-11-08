// fx.c: beat FX and sound colour FX for azmix. See fx.h for the contract.
//
// Everything runs at two rates: controls are read and slewed once per block
// (FX_BLOCK frames, or 16-frame sub-blocks for swept filters), and audio is
// computed per frame with gains interpolated linearly across the block, so
// knob moves never step. Effects that keep a tail (echo, ping pong, spiral,
// reverb, space, dub echo) gate their input on/off and let the wet part run
// until it is below -100 dB; then they go idle and cost nothing.
//
// Build: gcc -O2 -Wall -c fx.c (link with -lm)

#include "fx.h"
#include <math.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#define PI	3.14159265358979f
#define BMASK	(FX_BEAT_FRAMES - 1)
#define DMASK	(FX_DUB_FRAMES - 1)
#define QUIET	1e-5f		// a tail below this (-100 dB) counts as gone
#define SUB	16		// sub-block for filter sweeps

// ---- small helpers ---------------------------------------------------------

// NaN goes to lo, so a broken control value cannot poison the state
static inline float clampf(float x, float lo, float hi) { return !(x >= lo) ? lo : x > hi ? hi : x; }

static inline int mini(int a, int b) { return a < b ? a : b; }

// Soft limit: linear up to +-1, then bends towards +-2. Sits in every
// feedback loop so no setting can run away.
static inline float sat(float x)
{
	if (x > 1) return 2 - 1 / x;
	if (x < -1) return -2 - 1 / x;
	return x;
}

// one-pole slew towards tgt, snapping when within 1e-4 (-80 dB of a step)
static inline float slew(float cur, float tgt, float c)
{
	float v = cur + (tgt - cur) * c;
	return fabsf(tgt - v) < 1e-4f ? tgt : v;
}

// block slew coefficient scaled for a short block
static inline float bc(float c, int n) { return n == FX_BLOCK ? c : c * n / FX_BLOCK; }

static float block_coef(float tau, float rate) { return 1 - expf(-FX_BLOCK / (tau * rate)); }

static float onepole(float fc, float rate) { return 1 - expf(-2 * PI * fc / rate); }

static inline uint32_t xorshift(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

void fx_flush_denormals(void)
{
#if defined(__aarch64__)
	uint64_t fpcr;
	__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
	fpcr |= 1u << 24;		// FZ: flush denormals to zero
	__asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__) || defined(__i386__)
	_mm_setcsr(_mm_getcsr() | 0x8040);	// FTZ | DAZ
#endif
}

// ---- state-variable filter -------------------------------------------------
//
// Trapezoidal-integrated SVF: stays well behaved when the cutoff moves every
// few samples, which a direct-form biquad does not. Gives low, band and
// high (= x - k*band - low) from one state.

struct svf_k { float a1, a2, a3, k; };

static void svf_coef(struct svf_k *c, float fc, float q, float rate)
{
	fc = clampf(fc, 10, 0.45f * rate);
	float g = tanf(PI * fc / rate);
	c->k = 1 / q;
	c->a1 = 1 / (1 + g * (g + c->k));
	c->a2 = g * c->a1;
	c->a3 = g * c->a2;
}

static inline void svf_run(struct fx_svf *s, int ch, const struct svf_k *c, float x, float *lp, float *bp)
{
	float v3 = x - s->ic2[ch];
	float v1 = c->a1 * s->ic1[ch] + c->a2 * v3;
	float v2 = s->ic2[ch] + c->a2 * s->ic1[ch] + c->a3 * v3;
	s->ic1[ch] = 2 * v1 - s->ic1[ch];
	s->ic2[ch] = 2 * v2 - s->ic2[ch];
	*lp = v2;
	*bp = v1;
}

// ---- delay lines -----------------------------------------------------------

// Stereo ring read at fractional delay d >= 1 (1 = newest frame). Frames not
// written since the reset read as silence, so a reset never has to clear
// megabytes of old audio.
static inline void ring_read(const float *buf, uint32_t mask, const struct fx_line *ln, float d,
			     float *l, float *r)
{
	uint32_t n = (uint32_t)d;
	if (n + 1 > ln->fill) {
		*l = *r = 0;
		return;
	}
	float f = d - n;
	const float *a = buf + 2 * ((ln->wpos - n) & mask), *b = buf + 2 * ((ln->wpos - n - 1) & mask);
	*l = a[0] + (b[0] - a[0]) * f;
	*r = a[1] + (b[1] - a[1]) * f;
}

static inline void ring_write(float *buf, uint32_t mask, struct fx_line *ln, float l, float r)
{
	float *p = buf + 2 * (ln->wpos & mask);
	p[0] = l;
	p[1] = r;
	ln->wpos++;
	if (ln->fill <= mask)
		ln->fill++;
}

// mono ring read, q = index of the frame being written now (not yet written)
static inline float mono_read(const float *b, uint32_t mask, uint32_t q, float d)
{
	uint32_t n = (uint32_t)d;
	float f = d - n, x = b[(q - n) & mask];
	return x + (b[(q - n - 1) & mask] - x) * f;
}

// A delay time change starts a second read position and crossfades to it
// over 20 ms; a jump in a delay line would click, a slew would bend pitch.
static void tap_target(struct fx_tap *t, float d, float rate)
{
	if (t->cur < 1) {		// first use
		t->cur = t->nxt = d;
		t->step = 1 / (0.02f * rate);
		return;
	}
	if (!t->fading && fabsf(d - t->cur) >= 1) {
		t->nxt = d;
		t->x = 0;
		t->fading = 1;
	}
}

static inline void tap_read(const float *buf, uint32_t mask, struct fx_line *ln, float mod,
			    float *l, float *r)
{
	struct fx_tap *t = &ln->tap;
	ring_read(buf, mask, ln, t->cur + mod, l, r);
	if (t->fading) {
		float l2, r2;
		ring_read(buf, mask, ln, t->nxt + mod, &l2, &r2);
		*l += (l2 - *l) * t->x;
		*r += (r2 - *r) * t->x;
		t->x += t->step;
		if (t->x >= 1) {
			t->cur = t->nxt;
			t->fading = 0;
		}
	}
}

static inline void quiet_update(struct fx_line *ln, float pk, int n)
{
	if (pk > QUIET)
		ln->quiet = 0;
	else if (ln->quiet < (1u << 30))
		ln->quiet += n;
}

// nothing above QUIET went into the line for longer than it is long
static int line_gone(const struct fx_line *ln)
{
	float d = ln->tap.cur > ln->tap.nxt ? ln->tap.cur : ln->tap.nxt;
	return ln->quiet > d + FX_BLOCK;
}

// ---- reverb ----------------------------------------------------------------
//
// Pre-delay, four Schroeder allpasses per side to smear the attack, then an
// 8-line feedback delay network mixed by a Householder matrix (lossless, so
// the decay is set only by the per-line gains) with a one-pole damping filter
// in every line. Line lengths are primes spread over 25..53 ms.

static const int verb_len[FX_VERB_N] = { 1123, 1291, 1453, 1627, 1789, 1979, 2141, 2333 };
static const int verb_aplen[2][FX_VERB_AP] = { { 151, 229, 337, 491 }, { 163, 241, 353, 503 } };

static void verb_init(struct fx_verb *v, float rate, float pre_s)
{
	memset(v, 0, sizeof(*v));
	float s = rate / 44100;
	for (int i = 0; i < FX_VERB_N; i++)
		v->len[i] = (int)(verb_len[i] * s + .5f);
	for (int c = 0; c < 2; c++)
		for (int k = 0; k < FX_VERB_AP; k++)
			v->aplen[c][k] = (int)(verb_aplen[c][k] * s + .5f);
	v->prelen = (int)clampf(pre_s * rate, 1, FX_PRE_LEN - 1);
	v->rate = rate;
	v->t60 = v->damp_hz = -1;
}

// cheap to call every block: recomputes only on a 1% change
static void verb_set(struct fx_verb *v, float t60, float damp_hz)
{
	if (fabsf(t60 - v->t60) > 0.01f * t60) {
		v->t60 = t60;
		for (int i = 0; i < FX_VERB_N; i++)
			v->g[i] = powf(10, -3 * v->len[i] / (t60 * v->rate));
	}
	if (fabsf(damp_hz - v->damp_hz) > 0.01f * damp_hz) {
		v->damp_hz = damp_hz;
		v->damp = expf(-2 * PI * damp_hz / v->rate);
	}
}

static inline void verb_tick(struct fx_verb *v, float il, float ir, float *ol, float *or)
{
	uint32_t p = v->pos++;
	const uint32_t pm = FX_PRE_LEN - 1, am = FX_AP_LEN - 1, lm = FX_VERB_LEN - 1;

	v->pre[0][p & pm] = il;
	v->pre[1][p & pm] = ir;
	float x[2] = { v->pre[0][(p - v->prelen) & pm], v->pre[1][(p - v->prelen) & pm] };
	for (int c = 0; c < 2; c++)
		for (int k = 0; k < FX_VERB_AP; k++) {
			float *a = v->ap[c][k];
			float d = a[(p - v->aplen[c][k]) & am], w = x[c] + .6f * d;
			a[p & am] = w;
			x[c] = d - .6f * w;
		}

	float o[FX_VERB_N], s = 0;
	for (int i = 0; i < FX_VERB_N; i++) {
		float y = v->line[i][(p - v->len[i]) & lm];
		v->lp[i] = y + v->damp * (v->lp[i] - y);
		o[i] = v->lp[i] * v->g[i];
		s += o[i];
	}
	s *= 2.f / FX_VERB_N;
	for (int i = 0; i < FX_VERB_N; i++)
		v->line[i][p & lm] = o[i] - s + x[i & 1];
	// two orthogonal sign patterns: decorrelated left and right
	*ol = (o[0] + o[1] + o[2] + o[3] - o[4] - o[5] - o[6] - o[7]) * .25f;
	*or = (o[0] - o[1] + o[2] - o[3] + o[4] - o[5] + o[6] - o[7]) * .25f;
}

// ---- beat FX ---------------------------------------------------------------

static float beat_frames(const struct beatfx *fx, float beat_s, float lo)
{
	float d = clampf(beat_s, 0.001f, FX_MAX_BEAT_S) * fx->rate;
	return clampf(d, lo, FX_BEAT_FRAMES - 4 * FX_BLOCK - 2 * fx->xf);
}

static inline float loopf(struct beatfx *fx, int c, float x)
{
	fx->lpz[c] += fx->lpa * (x - fx->lpz[c]);
	fx->hpz[c] += fx->hpa * (fx->lpz[c] - fx->hpz[c]);
	return fx->lpz[c] - fx->hpz[c];
}

static void beat_reset(struct beatfx *fx)
{
	float lp = 7000, hp = 90;

	fx->mix = fx->ing = fx->lvl = fx->fb = 0;
	fx->phase = fx->build = 0;
	fx->gate = fx->gin = 1;
	fx->count = 0;
	fx->fresh = 1;
	fx->lpz[0] = fx->lpz[1] = fx->hpz[0] = fx->hpz[1] = 0;
	memset(&fx->ln, 0, sizeof(fx->ln));
	switch (fx->type) {
	case BFX_SPIRAL:
		memset(&fx->u.spiral, 0, sizeof(fx->u.spiral));
		fx->u.spiral.aplen[0] = (int)(0.0071f * fx->rate);
		fx->u.spiral.aplen[1] = (int)(0.0113f * fx->rate);
		fx->u.spiral.win = 0.03f * fx->rate;
		lp = 6000;
		hp = 150;
		break;
	case BFX_REVERB:
		verb_init(&fx->u.verb, fx->rate, 0.012f);
		hp = 150;
		break;
	case BFX_ROLL: case BFX_TRIPLET_ROLL:
		memset(&fx->u.roll, 0, sizeof(fx->u.roll));
		break;
	case BFX_FLANGER:
		memset(&fx->u.flanger, 0, sizeof(fx->u.flanger));
		break;
	case BFX_PHASER:
		memset(&fx->u.phaser, 0, sizeof(fx->u.phaser));
		break;
	case BFX_MOBIUS:
		memset(&fx->u.mobius, 0, sizeof(fx->u.mobius));
		break;
	case BFX_FILTER: case BFX_TRIPLET_FILTER:
		memset(&fx->u.svf, 0, sizeof(fx->u.svf));
		break;
	case BFX_HELIX:
		memset(&fx->u.helix, 0, sizeof(fx->u.helix));
		lp = 12000;
		hp = 40;
		break;
	}
	fx->lpa = onepole(lp, fx->rate);
	fx->hpa = onepole(hp, fx->rate);
}

void beatfx_init(struct beatfx *fx, float rate)
{
	memset(fx, 0, sizeof(*fx));
	fx->rate = clampf(rate, 8000, FX_MAX_RATE);
	fx->type = -1;
	fx->idle = 1;
	fx->sw = 1;
	fx->c_fast = block_coef(0.005f, fx->rate);
	fx->c_slow = block_coef(0.03f, fx->rate);
	fx->xf = (int)(0.006f * fx->rate);
}

// DELAY, ECHO, PING PONG. A tap at beat_s whose repeats go through gentle
// low and high cuts, so they get darker and thinner like a tape echo. DELAY
// repeats once (no feedback, as the DJM's: "outputs a delay sound once") and
// drops its wet part when off;
// ECHO and PING PONG only close their input, the repeats ring out.
static void bx_echo(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	int t = fx->type;
	float ing0 = fx->ing, ing1 = fx->ing = slew(ing0, on, bc(fx->c_fast, n));
	float lv0 = fx->lvl, lv1 = fx->lvl = slew(lv0, t == BFX_DELAY && !on ? 0 : depth, bc(fx->c_slow, n));
	float fb0 = fx->fb, fb1 = fx->fb = slew(fb0, t == BFX_DELAY ? 0 : .45f + .25f * depth,
						bc(fx->c_slow, n));
	float inv = 1.f / n, pk = 0;

	tap_target(&fx->ln.tap, beat_frames(fx, beat, FX_BLOCK), fx->rate);
	for (int i = 0; i < n; i++) {
		float r = (i + 1) * inv, g = ing0 + (ing1 - ing0) * r;
		float lv = lv0 + (lv1 - lv0) * r, fb = fb0 + (fb1 - fb0) * r;
		float *x = lr + 2 * i, wl, wr, a, b;
		tap_read(fx->buf, BMASK, &fx->ln, 0, &wl, &wr);
		wl = loopf(fx, 0, wl);
		wr = loopf(fx, 1, wr);
		if (t == BFX_PINGPONG) {	// mono in -> left -> right -> left ...
			a = sat(.5f * (x[0] + x[1]) * g + fb * wr);
			b = sat(fb * wl);
		} else {
			a = sat(x[0] * g + fb * wl);
			b = sat(x[1] * g + fb * wr);
		}
		ring_write(fx->buf, BMASK, &fx->ln, a, b);
		pk = fmaxf(pk, fabsf(a) + fabsf(b));
		x[0] += lv * wl;
		x[1] += lv * wr;
	}
	quiet_update(&fx->ln, pk, n);
	if (!on && ing1 == 0 && (t != BFX_DELAY || lv1 == 0) && line_gone(&fx->ln))
		fx->idle = 1;
}

// SPIRAL: an echo whose loop also holds two diffusing allpasses and a pitch
// shifter (+1 semitone per pass), so every repeat comes back smeared and a
// little higher; the feedback builds up over ~8 cycles while it is on. The
// delay time slews rather than crossfades: changing the beat bends the
// pitch of what is in the loop, as on the DJM.
static void bx_spiral(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	struct fx_spiral *s = &fx->u.spiral;
	const uint32_t shm = FX_SHIFT_LEN - 1, apm = FX_SAP_LEN - 1;
	const float pinc = (1.0594631f - 1) / s->win;	// 2^(1/12)
	float ing0 = fx->ing, ing1 = fx->ing = slew(ing0, on, bc(fx->c_fast, n));
	float lv0 = fx->lvl, lv1 = fx->lvl = slew(lv0, depth, bc(fx->c_slow, n));

	fx->build = on ? fminf(1, fx->build + n / (8 * beat * fx->rate)) : fmaxf(0, fx->build - n / fx->rate);
	float fb0 = fx->fb, fb1 = fx->fb = slew(fb0, .5f + .36f * fx->build, bc(fx->c_slow, n));

	// the shifter adds about half its window of delay to every pass
	float dt = beat_frames(fx, beat, s->win + 2 * FX_BLOCK) - .5f * s->win;
	float d0 = fx->fresh ? dt : s->d;
	float dd = (dt - d0) * bc(block_coef(0.08f, fx->rate), n), lim = .5f * n;
	float d1 = s->d = d0 + clampf(dd, -lim, lim);	// at most +-50% pitch bend

	float inv = 1.f / n, pk = 0;
	for (int i = 0; i < n; i++) {
		float r = (i + 1) * inv, g = ing0 + (ing1 - ing0) * r;
		float lv = lv0 + (lv1 - lv0) * r, fb = fb0 + (fb1 - fb0) * r;
		float *x = lr + 2 * i, y[2], z[2];
		uint32_t q = s->pos++;
		ring_read(fx->buf, BMASK, &fx->ln, d0 + (d1 - d0) * r, &y[0], &y[1]);

		float ph = s->ph += pinc;
		if (ph >= 1)
			ph = s->ph -= 1;
		float ph2 = ph < .5f ? ph + .5f : ph - .5f;
		float g1 = 1 - fabsf(2 * ph - 1);	// triangle windows, sum to 1
		float da = 2 + s->win * (1 - ph), db = 2 + s->win * (1 - ph2);
		for (int c = 0; c < 2; c++) {
			for (int k = 0; k < 2; k++) {
				float *a = s->ap[c][k];
				float d = a[(q - s->aplen[k]) & apm], w = y[c] + .5f * d;
				a[q & apm] = w;
				y[c] = d - .5f * w;
			}
			s->sh[c][q & shm] = y[c];
			z[c] = g1 * mono_read(s->sh[c], shm, q, da) + (1 - g1) * mono_read(s->sh[c], shm, q, db);
			z[c] = loopf(fx, c, z[c]);
		}
		float a = sat(x[0] * g + fb * z[0]), b = sat(x[1] * g + fb * z[1]);
		ring_write(fx->buf, BMASK, &fx->ln, a, b);
		pk = fmaxf(pk, fabsf(a) + fabsf(b));
		x[0] += lv * y[0];
		x[1] += lv * y[1];
	}
	quiet_update(&fx->ln, pk, n);
	if (!on && ing1 == 0 && fx->ln.quiet > d1 + 2 * s->win + FX_BLOCK)
		fx->idle = 1;
}

// REVERB: depth sets both the wet level and the decay (1.2..4.5 s). The
// send is high-passed at 150 Hz so the kick does not turn into mud.
static void bx_reverb(struct beatfx *fx, float *lr, int n, int on, float depth)
{
	struct fx_verb *v = &fx->u.verb;
	float ing0 = fx->ing, ing1 = fx->ing = slew(ing0, on, bc(fx->c_fast, n));
	float lv0 = fx->lvl, lv1 = fx->lvl = slew(lv0, depth, bc(fx->c_slow, n));
	float inv = 1.f / n, pk = 0;

	verb_set(v, 1.2f + 3.3f * depth, 7000);
	for (int i = 0; i < n; i++) {
		float r = (i + 1) * inv, g = ing0 + (ing1 - ing0) * r, lv = lv0 + (lv1 - lv0) * r;
		float *x = lr + 2 * i, s[2], ol, or;
		for (int c = 0; c < 2; c++) {
			fx->hpz[c] += fx->hpa * (x[c] * g - fx->hpz[c]);
			s[c] = x[c] * g - fx->hpz[c];
		}
		verb_tick(v, s[0], s[1], &ol, &or);
		pk = fmaxf(pk, fabsf(ol) + fabsf(or));
		x[0] += lv * sat(ol);
		x[1] += lv * sat(or);
	}
	quiet_update(&fx->ln, pk, n);
	if (!on && ing1 == 0 && fx->ln.quiet > 2 * FX_VERB_LEN)
		fx->idle = 1;
}

// TRANS: open for the first half of each beat_s cycle, cut for the second;
// the edges take ~1.5 ms so the cut is sharp but does not click.
static void bx_trans(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	float m0 = fx->mix, m1 = fx->mix = slew(m0, on ? depth : 0, bc(fx->c_fast, n));
	float inc = 1 / (beat * fx->rate), ga = 1 - expf(-1 / (0.0015f * fx->rate)), inv = 1.f / n;

	for (int i = 0; i < n; i++) {
		fx->phase += inc;
		if (fx->phase >= 1)
			fx->phase -= floorf(fx->phase);
		fx->gate += ((fx->phase < .5f) - fx->gate) * ga;
		float m = m0 + (m1 - m0) * (i + 1) * inv, k = 1 - m * (1 - fx->gate);
		lr[2 * i] *= k;
		lr[2 * i + 1] *= k;
	}
	if (!on && m1 == 0)
		fx->idle = 1;
}

// advance an LFO phase by m frames of a cycle of period seconds
static inline float lfo_step(float *ph, int m, float period, float rate)
{
	*ph += m / (period * rate);
	*ph -= floorf(*ph);
	return .5f - .5f * cosf(2 * PI * *ph);	// 0..1, starts at 0
}

// FILTER / TRIPLET FILTER: resonant low-pass swept 12 kHz -> 250 Hz -> 12 kHz
// once per cycle (triplet: 2/3 of beat_s). Starts open.
static void bx_filter(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	float period = fx->type == BFX_TRIPLET_FILTER ? beat * 2 / 3 : beat;
	float m0 = fx->mix, m1 = fx->mix = slew(m0, on ? depth : 0, bc(fx->c_fast, n)), inv = 1.f / n;
	struct svf_k k;

	for (int j = 0; j < n; j += SUB) {
		int m = mini(SUB, n - j);
		float u = lfo_step(&fx->phase, m, period, fx->rate);
		svf_coef(&k, 250 * powf(48, 1 - u), 2.f, fx->rate);
		for (int i = j; i < j + m; i++) {
			float mx = m0 + (m1 - m0) * (i + 1) * inv;
			for (int c = 0; c < 2; c++) {
				float *x = lr + 2 * i + c, lp, bp;
				svf_run(&fx->u.svf, c, &k, *x, &lp, &bp);
				*x += mx * (lp - *x);
			}
		}
	}
	if (!on && m1 == 0)
		fx->idle = 1;
}

// FLANGER: 0.8..6.8 ms modulated delay with feedback, mixed 50/50 with the
// input for the comb; the LFO period is beat_s.
static void bx_flanger(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	struct fx_flanger *f = &fx->u.flanger;
	const uint32_t mask = FX_FLANGE_LEN - 1;
	// fade in only once the delay line holds audio (it starts empty)
	int ready = fx->count >= FX_FLANGE_LEN;
	float m0 = fx->mix, m1 = fx->mix = slew(m0, on && ready ? depth : 0, bc(fx->c_fast, n)), inv = 1.f / n;

	if (!ready)
		fx->count += n;

	for (int j = 0; j < n; j += SUB) {
		int m = mini(SUB, n - j);
		float u = lfo_step(&fx->phase, m, beat, fx->rate);
		float d1 = (0.0008f + 0.006f * u) * fx->rate, d0 = fx->fresh && j == 0 ? d1 : f->d;
		f->d = d1;
		for (int i = j; i < j + m; i++) {
			float r = (float)(i - j + 1) / m, d = d0 + (d1 - d0) * r;
			float mx = m0 + (m1 - m0) * (i + 1) * inv;
			uint32_t q = f->pos++;
			for (int c = 0; c < 2; c++) {
				float *x = lr + 2 * i + c, w = mono_read(f->buf[c], mask, q, d);
				f->buf[c][q & mask] = sat(*x + .5f * w);
				*x += mx * (.62f * (*x + w) - *x);
			}
		}
	}
	if (!on && m1 == 0 && ready)
		fx->idle = 1;
}

// PHASER: six first-order allpasses swept 250 Hz..4 kHz, with feedback.
static void bx_phaser(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	struct fx_phaser *p = &fx->u.phaser;
	float m0 = fx->mix, m1 = fx->mix = slew(m0, on ? depth : 0, bc(fx->c_fast, n)), inv = 1.f / n;

	for (int j = 0; j < n; j += SUB) {
		int m = mini(SUB, n - j);
		float u = lfo_step(&fx->phase, m, beat, fx->rate);
		float t = tanf(PI * 250 * powf(16, u) / fx->rate), a1 = (t - 1) / (t + 1);
		float a0 = fx->fresh && j == 0 ? a1 : p->a;
		p->a = a1;
		for (int i = j; i < j + m; i++) {
			float a = a0 + (a1 - a0) * (i - j + 1) / m;
			float mx = m0 + (m1 - m0) * (i + 1) * inv;
			for (int c = 0; c < 2; c++) {
				float *x = lr + 2 * i + c, v = *x + .4f * p->y[c];
				for (int k = 0; k < FX_PHASER_N; k++) {
					float y = a * v + p->s[c][k];
					p->s[c][k] = v - a * y;
					v = y;
				}
				p->y[c] = v;
				*x += mx * (.5f * (*x + v) - *x);
			}
		}
	}
	if (!on && m1 == 0)
		fx->idle = 1;
}

// ROLL / TRIPLET ROLL. From the moment it is switched on the input is
// recorded (and heard as is); after beat_s (triplet: 2/3) playback jumps
// back to the start, and keeps looping until off. Every jump is a 6 ms
// crossfade from the audio that would have followed (also recorded) into
// the loop start. A beat change keeps the start point, so shorter rolls
// work as on the DJM.
static void bx_roll(struct beatfx *fx, float *lr, int n, int on, int edge, float beat, float depth)
{
	struct fx_roll *r = &fx->u.roll;
	const uint32_t cap = FX_BEAT_FRAMES, xf = fx->xf;
	float L = beat_frames(fx, fx->type == BFX_TRIPLET_ROLL ? beat * 2 / 3 : beat, 2 * xf);
	uint32_t len = (uint32_t)L;
	float m0 = fx->mix, m1 = fx->mix = slew(m0, on ? depth : 0, bc(fx->c_fast, n)), inv = 1.f / n;

	if (edge && !fx->fresh) {	// pressed again while fading out: start over
		r->xsrc = r->pp;
		r->xfc = xf;
		r->pp = r->rec = 0;
	}
	if (r->pp >= len) {		// loop got shorter than where we are
		r->xsrc = r->pp;
		r->xfc = xf;
		r->pp = 0;
	}
	for (int i = 0; i < n; i++) {
		float *x = lr + 2 * i;
		if (r->rec < cap) {
			fx->buf[2 * r->rec] = x[0];
			fx->buf[2 * r->rec + 1] = x[1];
			r->rec++;
		}
		const float *a = fx->buf + 2 * r->pp;
		float wl = a[0], wr = a[1];
		if (r->xfc) {
			const float *b = fx->buf + 2 * r->xsrc++;
			float g = (float)r->xfc-- / xf;
			wl += (b[0] - wl) * g;
			wr += (b[1] - wr) * g;
		}
		if (++r->pp >= len) {
			r->xsrc = r->pp;
			r->xfc = xf;
			r->pp = 0;
		}
		float m = m0 + (m1 - m0) * (i + 1) * inv;
		x[0] += m * (wl - x[0]);
		x[1] += m * (wr - x[1]);
	}
	if (!on && m1 == 0)
		fx->idle = 1;
}

// HELIX: a loop of beat_s length. For the first pass it records the input
// at full level (heard as is), then plays the loop with 0.95 feedback while
// the live input keeps leaking in at -18 dB, so the held slice slowly
// morphs. A slow +-0.5 ms wobble on the loop and the loop filters keep it
// moving instead of freezing into a buzz.
static void bx_helix(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	struct fx_helix *h = &fx->u.helix;
	const float xf = fx->xf;
	float m0 = fx->mix, m1 = fx->mix = slew(m0, on ? depth : 0, bc(fx->c_fast, n)), inv = 1.f / n;

	tap_target(&fx->ln.tap, beat_frames(fx, beat, 2 * xf + 2 * FX_BLOCK), fx->rate);
	float u = lfo_step(&h->lfo, n, 2.7f, fx->rate);
	float mod0 = h->mod, mod1 = h->mod = (u - .5f) * 0.001f * fx->rate;
	float cap = fx->ln.tap.cur, pk = 0;
	for (int i = 0; i < n; i++) {
		float r = (i + 1) * inv, m = m0 + (m1 - m0) * r;
		float t = fx->count < 0x7fffffff ? fx->count++ : fx->count;
		float capg = t < cap ? 1 : t < cap + xf ? 1 - (t - cap) / xf : 0;
		float gin = .12f + .88f * capg, *x = lr + 2 * i, wl, wr;
		tap_read(fx->buf, BMASK, &fx->ln, mod0 + (mod1 - mod0) * r, &wl, &wr);
		wl = loopf(fx, 0, wl);
		wr = loopf(fx, 1, wr);
		// no feedback until the first pass is in: the wobble makes the
		// read reach recorded audio a little before the pass is complete
		float fb = .95f * (1 - capg);
		float a = sat(x[0] * gin + fb * wl), b = sat(x[1] * gin + fb * wr);
		ring_write(fx->buf, BMASK, &fx->ln, a, b);
		pk = fmaxf(pk, fabsf(a) + fabsf(b));
		wl += (x[0] - wl) * capg;	// live while capturing, then the loop
		wr += (x[1] - wr) * capg;
		x[0] += m * (wl - x[0]);
		x[1] += m * (wr - x[1]);
	}
	quiet_update(&fx->ln, pk, n);
	if (!on && m1 == 0)
		fx->idle = 1;
}

// MOBIUS: six resonant band-passes spaced evenly over 6.5 octaves
// (90 Hz..8 kHz), all gliding upwards; each fades in at the bottom and out
// at the top (raised cosine over its position), so the sweep seems to rise
// forever. A band takes 6 * beat_s to cross; the pattern repeats every
// beat_s. Added on top of the slightly lowered dry signal.
static void bx_mobius(struct beatfx *fx, float *lr, int n, int on, float beat, float depth)
{
	struct fx_mobius *mb = &fx->u.mobius;
	float m0 = fx->mix, m1 = fx->mix = slew(m0, on ? depth : 0, bc(fx->c_fast, n)), inv = 1.f / n;
	struct svf_k k[FX_MOBIUS_N];
	float w0[FX_MOBIUS_N], w1[FX_MOBIUS_N];

	for (int j = 0; j < n; j += SUB) {
		int m = mini(SUB, n - j);
		fx->phase += m / (FX_MOBIUS_N * beat * fx->rate);
		fx->phase -= floorf(fx->phase);
		for (int b = 0; b < FX_MOBIUS_N; b++) {
			float u = fx->phase + (float)b / FX_MOBIUS_N;
			u -= floorf(u);
			svf_coef(&k[b], 90 * exp2f(6.5f * u), 5, fx->rate);
			w0[b] = mb->w[b];
			w1[b] = mb->w[b] = .5f - .5f * cosf(2 * PI * u);
		}
		for (int i = j; i < j + m; i++) {
			float r = (float)(i - j + 1) / m, mx = m0 + (m1 - m0) * (i + 1) * inv;
			for (int c = 0; c < 2; c++) {
				float *x = lr + 2 * i + c, acc = 0, lp, bp;
				for (int b = 0; b < FX_MOBIUS_N; b++) {
					svf_run(&mb->f[b], c, &k[b], *x, &lp, &bp);
					acc += (w0[b] + (w1[b] - w0[b]) * r) * k[b].k * bp;
				}
				*x = *x * (1 - .3f * mx) + mx * 2.4f * acc;
			}
		}
	}
	if (!on && m1 == 0)
		fx->idle = 1;
}

static void beat_block(struct beatfx *fx, const struct beatfx_params *p, float *lr, int n)
{
	int want = p->type >= 0 && p->type < BFX_COUNT ? p->type : -1;
	int on = p->on && want >= 0, leaving = 0;
	float beat = clampf(p->beat_s, 0.001f, 64), depth = clampf(p->depth, 0, 1);
	float dry[2 * FX_BLOCK];

	if (want != fx->type) {
		if (fx->idle || fx->type < 0) {	// nothing sounding: switch now
			fx->type = want;
			fx->idle = 1;
			fx->sw = 1;
		} else {			// fade the old effect out first
			leaving = 1;
			on = 0;
		}
	}
	if (fx->type < 0)
		return;
	if (fx->idle) {
		fx->was_on = 0;
		if (!on)
			return;
		beat_reset(fx);
		fx->idle = 0;
	}
	int edge = on && !fx->was_on;
	fx->was_on = on;
	int fading = leaving || fx->sw < 1;
	if (fading)
		memcpy(dry, lr, 2 * n * sizeof(float));

	switch (fx->type) {
	case BFX_DELAY: case BFX_ECHO: case BFX_PINGPONG:
		bx_echo(fx, lr, n, on, beat, depth); break;
	case BFX_SPIRAL: bx_spiral(fx, lr, n, on, beat, depth); break;
	case BFX_REVERB: bx_reverb(fx, lr, n, on, depth); break;
	case BFX_TRANS: bx_trans(fx, lr, n, on, beat, depth); break;
	case BFX_FILTER: case BFX_TRIPLET_FILTER:
		bx_filter(fx, lr, n, on, beat, depth); break;
	case BFX_FLANGER: bx_flanger(fx, lr, n, on, beat, depth); break;
	case BFX_PHASER: bx_phaser(fx, lr, n, on, beat, depth); break;
	case BFX_ROLL: case BFX_TRIPLET_ROLL:
		bx_roll(fx, lr, n, on, edge, beat, depth); break;
	case BFX_HELIX: bx_helix(fx, lr, n, on, beat, depth); break;
	case BFX_MOBIUS: bx_mobius(fx, lr, n, on, beat, depth); break;
	}
	fx->fresh = 0;

	if (fading) {			// 10 ms fade out on a type change, back in if cancelled
		float s0 = fx->sw, st = n / (0.01f * fx->rate);
		float s1 = leaving ? fmaxf(0, s0 - st) : fminf(1, s0 + st);
		for (int i = 0; i < 2 * n; i++) {
			float g = s0 + (s1 - s0) * (i / 2 + 1) / n;
			lr[i] = dry[i] + g * (lr[i] - dry[i]);
		}
		fx->sw = s1;
		if (leaving && (s1 == 0 || fx->idle)) {
			fx->type = want;
			fx->idle = 1;
			fx->sw = 1;
		}
	}
}

void beatfx_process(struct beatfx *fx, const struct beatfx_params *p, float *lr, int frames)
{
	for (int i = 0; i < frames; i += FX_BLOCK)
		beat_block(fx, p, lr + 2 * i, mini(FX_BLOCK, frames - i));
}

// ---- sound colour FX -------------------------------------------------------

// 0 in the dead zone, rising to 1 at either end
static float knob_amount(float k)
{
	return k < .485f ? (.485f - k) / .485f : k > .515f ? (k - .515f) / .485f : 0;
}

static void color_reset(struct colorfx *fx)
{
	fx->send = fx->w = fx->lvl = 0;
	memset(fx->f, 0, sizeof(fx->f));
	fx->env[0] = fx->env[1] = 0;
	fx->gain = 1;
	fx->hold[0] = fx->hold[1] = 0;
	fx->hph = 1;
	memset(&fx->ln, 0, sizeof(fx->ln));
	if (fx->type == CFX_SPACE)
		verb_init(&fx->u.verb, fx->rate, 0.02f);
}

void colorfx_init(struct colorfx *fx, float rate)
{
	memset(fx, 0, sizeof(*fx));
	fx->rate = clampf(rate, 8000, FX_MAX_RATE);
	fx->type = -1;
	fx->idle = 1;
	fx->sw = 1;
	fx->knob = .5f;
	fx->hside = 1;
	fx->c_knob = block_coef(0.015f, fx->rate);
	fx->rng = 0x9e3779b9u;
	fx->dub_s = .375f;
}

void colorfx_set_time(struct colorfx *fx, float seconds)
{
	fx->dub_s = clampf(seconds, 0.02f, (FX_DUB_FRAMES - 4 * FX_BLOCK) / fx->rate - 0.03f);
}

// FILTER: low-pass 20 kHz -> 100 Hz left, high-pass 20 Hz -> 8 kHz right,
// resonance rising a little with the turn. Fades in over the first 3% so
// leaving the dead zone is seamless.
static void cx_filter(struct colorfx *fx, float *lr, int n, float a0, float a1, int side)
{
	struct svf_k k;
	for (int j = 0; j < n; j += SUB) {
		int m = mini(SUB, n - j);
		float a = a0 + (a1 - a0) * (j + m) / n;
		svf_coef(&k, side < 0 ? 20000 * powf(.005f, a) : 20 * powf(400, a), .75f + .75f * a, fx->rate);
		float w0 = fx->w, w1 = fx->w = fminf(1, a * 30);
		for (int i = j; i < j + m; i++) {
			float w = w0 + (w1 - w0) * (i - j + 1) / m;
			for (int c = 0; c < 2; c++) {
				float *x = lr + 2 * i + c, lp, bp;
				svf_run(&fx->f[0], c, &k, *x, &lp, &bp);
				*x += w * ((side < 0 ? lp : *x - k.k * bp - lp) - *x);
			}
		}
	}
	if (a0 == 0 && a1 == 0)
		fx->idle = 1;
}

// SWEEP. Right: a band-pass whose centre follows the knob (180 Hz..8 kHz),
// narrowing and gaining up to +9 dB as it goes, 15% dry kept underneath. Left: a transient gate: each onset opens it for a
// hold time that shrinks with the turn (250 -> 50 ms), between onsets the
// level drops, so the groove gets tighter and drier.
static void cx_sweep(struct colorfx *fx, float *lr, int n, float a0, float a1, int side)
{
	if (side > 0) {
		struct svf_k k;
		for (int j = 0; j < n; j += SUB) {
			int m = mini(SUB, n - j);
			float a = a0 + (a1 - a0) * (j + m) / n;
			svf_coef(&k, 180 * powf(45, a), .8f + 1.5f * a, fx->rate);
			float w0 = fx->w, w1 = fx->w = .85f * fminf(1, a * 8), boost = k.k * (1 + 2 * a);
			for (int i = j; i < j + m; i++) {
				float w = w0 + (w1 - w0) * (i - j + 1) / m;
				for (int c = 0; c < 2; c++) {
					float *x = lr + 2 * i + c, lp, bp;
					svf_run(&fx->f[0], c, &k, *x, &lp, &bp);
					*x += w * (boost * bp - *x);
				}
			}
		}
	} else {
		float rate = fx->rate;
		float hold = (0.25f - 0.2f * a1) * rate, closed = 1 - fminf(1, 1.1f * a1);
		float ff = expf(-1 / (0.004f * rate));	// fast envelope release
		float fs = 1 - expf(-1 / (0.03f * rate)), fr = expf(-1 / (0.15f * rate));
		float go = 1 - expf(-1 / (0.0005f * rate)), gc = 1 - expf(-1 / (0.012f * rate));
		for (int i = 0; i < n; i++) {
			float *x = lr + 2 * i, e = fmaxf(fabsf(x[0]), fabsf(x[1]));
			fx->env[0] = e > fx->env[0] ? e : fx->env[0] * ff;
			fx->env[1] = fx->env[0] > fx->env[1] ? fx->env[1] + (fx->env[0] - fx->env[1]) * fs
							     : fx->env[1] * fr;
			if (fx->env[0] > 1.5f * fx->env[1] + 1e-3f)	// onset
				fx->hold[0] = hold;
			float tgt = fx->hold[0] > 0 ? 1 : closed;
			if (fx->hold[0] > 0)
				fx->hold[0]--;
			fx->gain += (tgt - fx->gain) * (tgt > fx->gain ? go : gc);
			x[0] *= fx->gain;
			x[1] *= fx->gain;
		}
	}
	if (a0 == 0 && a1 == 0 && fx->gain > .9999f)
		fx->idle = 1;
}

// NOISE: white noise (independent per side) through a low-pass that closes
// towards 350 Hz on the left, a high-pass that opens up to 12 kHz on the
// right; level up to -8 dBFS at the ends. The music itself passes untouched.
static void cx_noise(struct colorfx *fx, float *lr, int n, float a0, float a1, int side)
{
	struct svf_k k;
	const float sc = 1.f / 2147483648.f;
	for (int j = 0; j < n; j += SUB) {
		int m = mini(SUB, n - j);
		float a = a0 + (a1 - a0) * (j + m) / n;
		svf_coef(&k, side < 0 ? 14000 * powf(.025f, a) : 300 * powf(40, a), 1, fx->rate);
		float l0 = fx->lvl, l1 = fx->lvl = .4f * a * sqrtf(a);
		for (int i = j; i < j + m; i++) {
			float g = l0 + (l1 - l0) * (i - j + 1) / m;
			for (int c = 0; c < 2; c++) {
				float z = (int32_t)xorshift(&fx->rng) * sc, lp, bp;
				svf_run(&fx->f[0], c, &k, z, &lp, &bp);
				lr[2 * i + c] += g * (side < 0 ? lp : z - k.k * bp - lp);
			}
		}
	}
	if (a0 == 0 && a1 == 0)
		fx->idle = 1;
}

// CRUSH: sample-and-hold downsampling plus bit reduction. Left: a lo-fi,
// muffled crunch (12 -> 3 bits, down to 1/16 rate). Right: high-passed and
// driven before crushing (10 -> 3 bits, down to 1/5 rate): thin and
// distorted.
static void cx_crush(struct colorfx *fx, float *lr, int n, float a0, float a1, int side)
{
	struct svf_k k;
	float a = a1, bits, R, drive = 1;
	if (side < 0) {
		bits = 12 - 9 * a;
		R = 1 + 15 * a * a;
	} else {
		bits = 10 - 7 * a;
		R = 1 + 4 * a;
		drive = 1 + 4 * a;
		svf_coef(&k, 60 * powf(15, a), .707f, fx->rate);
	}
	float step = exp2f(1 - bits), inc = 1 / R, out = 1;
	float w0 = fx->w, w1 = fx->w = fminf(1, a * 5), inv = 1.f / n;
	for (int i = 0; i < n; i++) {
		float *x = lr + 2 * i, v[2] = { x[0], x[1] }, lp, bp;
		if (side > 0)
			for (int c = 0; c < 2; c++) {
				svf_run(&fx->f[0], c, &k, x[c], &lp, &bp);
				v[c] = x[c] - k.k * bp - lp;
			}
		fx->hph += inc;
		if (fx->hph >= 1) {
			fx->hph -= floorf(fx->hph);
			for (int c = 0; c < 2; c++)
				fx->hold[c] = step * floorf(sat(v[c] * drive) / step + .5f) * out;
		}
		float w = w0 + (w1 - w0) * (i + 1) * inv;
		x[0] += w * (fx->hold[0] - x[0]);
		x[1] += w * (fx->hold[1] - x[1]);
	}
	if (a0 == 0 && a1 == 0)
		fx->idle = 1;
}

// SPACE: reverb send growing with the turn; the return stays open so the
// tail rings out after the knob comes back. Left: darker (send low-passed,
// heavy damping, 1..3 s). Right: brighter and longer (send high-passed,
// light damping, 1.6..5.6 s). Decay and colour follow the held amount.
static void cx_space(struct colorfx *fx, float *lr, int n, float a0, float a1)
{
	struct fx_verb *v = &fx->u.verb;
	struct svf_k k;
	float ah = fx->ahold, inv = 1.f / n, pk = 0;
	int left = fx->hside < 0;

	if (left) {
		verb_set(v, 1 + 2 * ah, 5000 * powf(.3f, ah));
		svf_coef(&k, 7000 * powf(.25f, ah), .707f, fx->rate);
	} else {
		verb_set(v, 1.6f + 4 * ah, 11000);
		svf_coef(&k, 150 * powf(6, ah), .707f, fx->rate);
	}
	float s0 = fx->send, s1 = fx->send = powf(a1, .7f);
	(void)a0;
	for (int i = 0; i < n; i++) {
		float s = s0 + (s1 - s0) * (i + 1) * inv, *x = lr + 2 * i, in[2], ol, or, lp, bp;
		for (int c = 0; c < 2; c++) {
			svf_run(&fx->f[0], c, &k, x[c] * s, &lp, &bp);
			in[c] = left ? lp : x[c] * s - k.k * bp - lp;
		}
		verb_tick(v, in[0], in[1], &ol, &or);
		pk = fmaxf(pk, fabsf(ol) + fabsf(or));
		x[0] += sat(ol);
		x[1] += sat(or);
	}
	quiet_update(&fx->ln, pk, n);
	if (a1 == 0 && s0 == 0 && fx->ln.quiet > 2 * FX_VERB_LEN)
		fx->idle = 1;
}

// DUB ECHO: echo at dub_s with filtered, slightly saturating feedback that
// rises with the turn (0.3..0.9). Left: repeats get darker (low-pass 3 kHz
// -> 600 Hz). Right: thinner and brighter (high-pass 250 Hz -> 2.5 kHz).
// Like SPACE the return stays open: a quick turn and back throws an echo.
static void cx_dub(struct colorfx *fx, float *lr, int n, float a0, float a1)
{
	struct svf_k kl, kh;
	float ah = fx->ahold, inv = 1.f / n, pk = 0;

	tap_target(&fx->ln.tap, clampf(fx->dub_s * fx->rate, FX_BLOCK, FX_DUB_FRAMES - 4 * FX_BLOCK),
		   fx->rate);
	if (fx->hside < 0) {
		svf_coef(&kl, 3000 * powf(.2f, ah), .707f, fx->rate);
		svf_coef(&kh, 70, .707f, fx->rate);
	} else {
		svf_coef(&kl, 9000, .707f, fx->rate);
		svf_coef(&kh, 250 * powf(10, ah), .707f, fx->rate);
	}
	float fb = .3f + .6f * ah;
	float s0 = fx->send, s1 = fx->send = fminf(1, 1.3f * a1);
	(void)a0;
	for (int i = 0; i < n; i++) {
		float s = s0 + (s1 - s0) * (i + 1) * inv, *x = lr + 2 * i, w[2], lp, bp;
		tap_read(fx->u.dub, DMASK, &fx->ln, 0, &w[0], &w[1]);
		for (int c = 0; c < 2; c++) {
			svf_run(&fx->f[0], c, &kl, w[c], &lp, &bp);
			svf_run(&fx->f[1], c, &kh, lp, &w[c], &bp);	// w = low-passed
			w[c] = lp - kh.k * bp - w[c];			// then high-passed
		}
		float a = sat(x[0] * s + fb * w[0]), b = sat(x[1] * s + fb * w[1]);
		ring_write(fx->u.dub, DMASK, &fx->ln, a, b);
		pk = fmaxf(pk, fabsf(a) + fabsf(b));
		x[0] += .8f * w[0];
		x[1] += .8f * w[1];
	}
	quiet_update(&fx->ln, pk, n);
	if (a1 == 0 && s0 == 0 && line_gone(&fx->ln))
		fx->idle = 1;
}

static void color_block(struct colorfx *fx, int type, float knob, float *lr, int n)
{
	int want = type >= 0 && type < CFX_COUNT ? type : -1, leaving = 0;
	float kt = knob == knob ? clampf(knob, 0, 1) : .5f, dry[2 * FX_BLOCK];

	if (want != fx->type) {
		if (fx->idle || fx->type < 0) {
			fx->type = want;
			fx->idle = 1;
			fx->sw = 1;
		} else {
			leaving = 1;
		}
	}
	// slewed, and never more than 0.02 per block, so one block cannot jump
	// across the dead zone from one side to the other
	float k0 = fx->knob, lim = .02f * n / FX_BLOCK;
	float k1 = fx->knob = k0 + clampf(slew(k0, kt, bc(fx->c_knob, n)) - k0, -lim, lim);
	float a0 = knob_amount(k0), a1 = knob_amount(k1);
	int side = k0 + k1 < 1 ? -1 : 1;
	if (a1 > 0)
		fx->hside = side;
	fx->ahold = fmaxf(a1, fx->ahold - n / (1.5f * fx->rate));

	if (fx->type < 0)
		return;
	if (fx->idle) {
		if (a1 == 0 || leaving)
			return;
		color_reset(fx);
		fx->idle = 0;
	}
	int fading = leaving || fx->sw < 1;
	if (fading)
		memcpy(dry, lr, 2 * n * sizeof(float));

	switch (fx->type) {
	case CFX_SPACE: cx_space(fx, lr, n, a0, a1); break;
	case CFX_DUB_ECHO: cx_dub(fx, lr, n, a0, a1); break;
	case CFX_SWEEP: cx_sweep(fx, lr, n, a0, a1, side); break;
	case CFX_NOISE: cx_noise(fx, lr, n, a0, a1, side); break;
	case CFX_CRUSH: cx_crush(fx, lr, n, a0, a1, side); break;
	case CFX_FILTER: cx_filter(fx, lr, n, a0, a1, side); break;
	}

	if (fading) {
		float s0 = fx->sw, st = n / (0.01f * fx->rate);
		float s1 = leaving ? fmaxf(0, s0 - st) : fminf(1, s0 + st);
		for (int i = 0; i < 2 * n; i++) {
			float g = s0 + (s1 - s0) * (i / 2 + 1) / n;
			lr[i] = dry[i] + g * (lr[i] - dry[i]);
		}
		fx->sw = s1;
		if (leaving && (s1 == 0 || fx->idle)) {
			fx->type = want;
			fx->idle = 1;
			fx->sw = 1;
		}
	}
}

void colorfx_process(struct colorfx *fx, int type, float knob, float *lr, int frames)
{
	for (int i = 0; i < frames; i += FX_BLOCK)
		color_block(fx, type, knob, lr + 2 * i, mini(FX_BLOCK, frames - i));
}

// ---- MIC Feedback Reducer --------------------------------------------------------

void fbr_init(struct fbr *r, float rate)
{
	memset(r, 0, sizeof(*r));
	r->rate = rate;
}

// RBJ peaking EQ, cut by gain_db at f with quality q
static void fbr_set(struct fbr_notch *n, float rate, float f, float gain_db)
{
	float A = powf(10, gain_db / 40), w = 2 * (float)M_PI * f / rate;
	float al = sinf(w) / (2 * 25.f), cw = cosf(w);	// Q 25: ~1/20 octave
	float a0 = 1 + al / A;
	n->f = f, n->gain_db = gain_db, n->age = 0;
	n->b0 = (1 + al * A) / a0;
	n->b1 = -2 * cw / a0;
	n->b2 = (1 - al * A) / a0;
	n->a1 = -2 * cw / a0;
	n->a2 = (1 - al / A) / a0;
}

// in-place radix-2 complex FFT, n a power of two
static void fbr_fft(float *re, float *im, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		float a = -2 * (float)M_PI / len, wr = cosf(a), wi = sinf(a);
		for (int i = 0; i < n; i += len) {
			float cr = 1, ci = 0;
			for (int k = 0; k < len / 2; k++) {
				float *ur = re + i + k, *ui = im + i + k;
				float *vr = re + i + k + len / 2, *vi = im + i + k + len / 2;
				float tr = *vr * cr - *vi * ci, ti = *vr * ci + *vi * cr;
				*vr = *ur - tr, *vi = *ui - ti;
				*ur += tr, *ui += ti;
				float ncr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr, cr = ncr;
			}
		}
	}
}

// One analysis: the strongest narrow peak, if it stands out enough, counts
// as a candidate; enough hits in a row and it gets a notch.
static int fbr_analyse(struct fbr *r, float *hz)
{
	static float re[FBR_N], im[FBR_N], db[FBR_N / 2];
	const int heavy = r->mode == 2;
	const float par = heavy ? 12 : 16;		// dB above the neighbourhood
	const int need = heavy ? 6 : 12;		// analyses in a row (35 / 70 ms)
	const int max = heavy ? FBR_NOTCHES : 4;
	const float depth = heavy ? -15 : -9, deepest = heavy ? -30 : -18;

	for (int i = 0; i < FBR_N; i++) {
		float w = .5f - .5f * cosf(2 * (float)M_PI * i / FBR_N);
		re[i] = r->in[(r->pos + i) % FBR_N] * w;
		im[i] = 0;
	}
	fbr_fft(re, im, FBR_N);
	for (int k = 1; k < FBR_N / 2; k++)
		db[k] = 10 * log10f(re[k] * re[k] + im[k] * im[k] + 1e-12f);

	// 200 Hz .. 8 kHz; the loudest local maximum
	float bw = r->rate / FBR_N;
	int lo = (int)(200 / bw), hi = mini((int)(8000 / bw), FBR_N / 2 - 20), best = 0;
	for (int k = lo; k <= hi; k++)
		if (db[k] > db[k - 1] && db[k] >= db[k + 1] && (!best || db[k] > db[best]))
			best = k;
	int found = 0;
	float f = 0;
	if (best) {
		float sum = 0;
		int cnt = 0;
		for (int d = 3; d <= 16; d++) {
			if (best - d > 0)
				sum += db[best - d], cnt++;
			sum += db[best + d], cnt++;
		}
		// -54 dB: a full-scale sine comes out about +48 here
		// howling is a pure tone: a voice or an instrument has partials at
		// twice (or half) the frequency, not far below
		float harm = -200;
		for (int m = 0; m < 2; m++) {
			int k = m ? best / 2 : best * 2;
			if (k > 1 && k < FBR_N / 2 - 1)
				harm = fmaxf(harm, fmaxf(db[k - 1], fmaxf(db[k], db[k + 1])));
		}
		if (db[best] > -6 && db[best] - sum / cnt > par && db[best] - harm > 20) {
			float a = db[best - 1], b = db[best], c = db[best + 1];
			float p = .5f * (a - c) / (a - 2 * b + c);
			f = (best + p) * bw;
			found = 1;
		}
	}
	// candidates: a hit near one counts on, the rest start over
	int hit = -1;
	for (int i = 0; i < FBR_NOTCHES; i++) {
		struct fbr_cand *c = &r->c[i];
		// it must hold its pitch (a voice glides): within half a bin of
		// where it was first seen
		if (found && c->hits && fabsf(c->f - f) < bw * .5f) {
			c->hits++;
			hit = i;
		} else {
			c->hits = 0;
		}
	}
	if (found && hit < 0)
		for (int i = 0; i < FBR_NOTCHES; i++)
			if (!r->c[i].hits) {
				r->c[i].f = f, r->c[i].hits = 1;
				break;
			}
	for (int i = 0; i < FBR_NOTCHES; i++)
		r->n[i].age++;
	if (hit < 0 || r->c[hit].hits < need)
		return 0;
	f = r->c[hit].f;
	r->c[hit].hits = 0;
	// a notch already there goes deeper; else a free one, else the oldest
	struct fbr_notch *use = NULL;
	for (int i = 0; i < max; i++)
		if (r->n[i].f && fabsf(r->n[i].f - f) < f * .03f)
			use = &r->n[i];
	if (use) {
		fbr_set(use, r->rate, use->f, fmaxf(deepest, use->gain_db - 3));
	} else {
		for (int i = 0; i < max && !use; i++)
			if (!r->n[i].f)
				use = &r->n[i];
		if (!use) {			// all taken: the oldest
			use = &r->n[0];
			for (int i = 1; i < max; i++)
				if (r->n[i].age > use->age)
					use = &r->n[i];
		}
		float s1 = use->s1, s2 = use->s2;
		fbr_set(use, r->rate, f, depth);
		use->s1 = s1, use->s2 = s2;
	}
	r->set++;
	*hz = f;
	return 1;
}

int fbr_process(struct fbr *r, int mode, float *x, int frames, float *hz)
{
	if (mode != r->mode) {
		float rate = r->rate;
		fbr_init(r, rate);
		r->mode = mode;
	}
	if (!mode)
		return 0;
	int news = 0;
	for (int i = 0; i < frames; i++) {
		float v = x[i];
		for (int k = 0; k < FBR_NOTCHES; k++) {
			struct fbr_notch *n = &r->n[k];
			if (!n->f)
				continue;
			float y = n->b0 * v + n->s1;
			n->s1 = n->b1 * v - n->a1 * y + n->s2;
			n->s2 = n->b2 * v - n->a2 * y;
			v = y;
		}
		x[i] = v;
		r->in[r->pos] = v;
		r->pos = (r->pos + 1) % FBR_N;
		if (++r->since >= FBR_HOP) {
			r->since = 0;
			news |= fbr_analyse(r, hz);
		}
	}
	return news;
}
