/* SPDX-License-Identifier: GPL-3.0-only */
/* SHARD XL's engine: the audio thread. Voices (SAMPLE, SHARD) -> each track's deck (2 to 4 bands, Linkwitz-Riley
 * 12 or 24 dB/oct, a shaper and feedback per band, a DJ filter) -> its channel (level, pan, sends A, B, FX) ->
 * the master, with Bus A and B through the crossfader and back, the FX loop (an internal delay stands in for the
 * outside effects on a stereo output), a limiter. Plus the sequencer, scenes and their morphs, the capture ring,
 * resampling and beat repeat. */
#include "xl.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

xl_state XL;
xl_queue XL_CMD, XL_MIDI, XL_EV;

static const char *const ENG_L[] = {"SAMPLE", "SHARD"};
static const char *const SH_L[] = {"OFF", "OVRDRV", "DIST", "FOLD", "CRUSH", "DECIM", "HALF", "TUBE", "FUZZ"};
static const char *const SLOPE_L[] = {"12 dB", "24 dB"};
static const char *const DIV_L[] = {"1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
static const char *const OFFON_L[] = {"OFF", "ON"};
static const char *const CURVE_L[] = {"SMOOTH", "DJ", "CUT"};
static const char *const PAD_L[] = {"SCENES", "SLICES"};
static const char *const QUANT_L[] = {"NOW", "BEAT", "BAR"};
static const char *const TRK_L[] = {"SELECTED", "1", "2", "3", "4", "5", "6", "7", "8"};

const xl_pdesc XL_P[NP] = {
    [P_ENGINE] = {"ENGINE", 0, 1, 0, PF_ENUM, ENG_L, ""},
    [P_TUNE] = {"TUNE", -24, 24, 0, 0, 0, "st"},
    [P_ATK] = {"ATTACK", 0.1f, 2000, 1, PF_LOG, 0, "ms"},
    [P_REL] = {"RELEASE", 1, 5000, 60, PF_LOG, 0, "ms"},
    [P_SENS] = {"SENS", 0, 100, 50, PF_INT, 0, ""},
    [P_LEVEL] = {"LEVEL", -60, 6, -6, 0, 0, "dB"},
    [P_PAN] = {"PAN", -1, 1, 0, 0, 0, ""},
    [P_SENDA] = {"SEND A", 0, 1, 0, 0, 0, "%"},
    [P_SENDB] = {"SEND B", 0, 1, 0, 0, 0, "%"},
    [P_SENDFX] = {"SEND FX", 0, 1, 0, 0, 0, "%"},
    [P_MUTE] = {"MUTE", 0, 1, 0, PF_ENUM, OFFON_L, ""},
    [P_SOLO] = {"SOLO", 0, 1, 0, PF_ENUM, OFFON_L, ""},
    [P_BANDS] = {"BANDS", 2, 4, 3, PF_INT, 0, ""},
    [P_SLOPE] = {"SLOPE", 0, 1, 1, PF_ENUM, SLOPE_L, ""},
    [P_X1] = {"XLO", 30, 16000, 200, PF_LOG, 0, "Hz"},
    [P_X2] = {"XMID", 30, 16000, 2000, PF_LOG, 0, "Hz"},
    [P_X3] = {"XHI", 30, 16000, 6000, PF_LOG, 0, "Hz"},
    [P_LV0] = {"LEVEL", -60, 12, 0, 0, 0, "dB"}, [P_LV1] = {"LEVEL", -60, 12, 0, 0, 0, "dB"},
    [P_LV2] = {"LEVEL", -60, 12, 0, 0, 0, "dB"}, [P_LV3] = {"LEVEL", -60, 12, 0, 0, 0, "dB"},
    [P_TY0] = {"TYPE", 0, SH_N - 1, 0, PF_ENUM, SH_L, ""}, [P_TY1] = {"TYPE", 0, SH_N - 1, 0, PF_ENUM, SH_L, ""},
    [P_TY2] = {"TYPE", 0, SH_N - 1, 0, PF_ENUM, SH_L, ""}, [P_TY3] = {"TYPE", 0, SH_N - 1, 0, PF_ENUM, SH_L, ""},
    [P_DR0] = {"DRIVE", 0, 1, 0, 0, 0, "%"}, [P_DR1] = {"DRIVE", 0, 1, 0, 0, 0, "%"},
    [P_DR2] = {"DRIVE", 0, 1, 0, 0, 0, "%"}, [P_DR3] = {"DRIVE", 0, 1, 0, 0, 0, "%"},
    [P_FB0] = {"FEEDBACK", 0, 0.95f, 0, 0, 0, "%"}, [P_FB1] = {"FEEDBACK", 0, 0.95f, 0, 0, 0, "%"},
    [P_FB2] = {"FEEDBACK", 0, 0.95f, 0, 0, 0, "%"}, [P_FB3] = {"FEEDBACK", 0, 0.95f, 0, 0, 0, "%"},
    [P_DJ] = {"DJ FILTER", -1, 1, 0, 0, 0, ""},
    [P_SLEN] = {"LENGTH", 1, 64, 16, PF_INT, 0, ""},
    [P_SDIV] = {"DIV", 0, DIV_N - 1, DIV_16, PF_ENUM, DIV_L, ""},
};
const xl_pdesc XL_G[NG] = {
    [G_BPM] = {"BPM", 40, 240, 120, 0, 0, ""},
    [G_SWING] = {"SWING", 50, 75, 50, PF_INT, 0, "%"},
    [G_XFADE] = {"CROSSFADER", 0, 1, 0.5f, 0, 0, ""},
    [G_XCURVE] = {"CURVE", 0, 2, 1, PF_ENUM, CURVE_L, ""},
    [G_RETA] = {"RETURN A", -60, 6, -6, 0, 0, "dB"},
    [G_RETB] = {"RETURN B", -60, 6, -6, 0, 0, "dB"},
    [G_FXRET] = {"FX RETURN", -60, 6, -6, 0, 0, "dB"},
    [G_MASTER] = {"MASTER", -60, 6, 0, 0, 0, "dB"},
    [G_SPLIT] = {"SPLIT", 0, 127, 0, PF_INT, 0, ""},
    [G_LOWTRK] = {"LOW TRACK", 0, 8, 1, PF_ENUM, TRK_L, ""},
    [G_HITRK] = {"HIGH TRACK", 0, 8, 0, PF_ENUM, TRK_L, ""},
    [G_PADMODE] = {"PADS", 0, 1, 0, PF_ENUM, PAD_L, ""},
    [G_QUANT] = {"LAUNCH", 0, 2, 2, PF_ENUM, QUANT_L, ""},
    [G_MORPHBARS] = {"MORPH BARS", 0, 16, 4, PF_INT, 0, ""},
    [G_SCENEA] = {"SCENE A", 1, 8, 1, PF_INT, 0, ""},
    [G_SCENEB] = {"SCENE B", 1, 8, 2, PF_INT, 0, ""},
    [G_MORPH] = {"MORPH", 0, 1, 0, 0, 0, "%"},
    [G_MUTATE] = {"MUTATE", 0, 1, 0.3f, 0, 0, "%"},
    [G_RSBARS] = {"RESAMPLE", 1, 8, 2, PF_INT, 0, "bars"},
    [G_PREVIEW] = {"PREVIEW", -60, 6, -6, 0, 0, "dB"},
};

float xl_pval(const xl_pdesc *d, float v)
{
    if (!(v >= d->min)) v = d->min;      /* (NaN too) */
    if (v > d->max) v = d->max;
    if (d->flags & (PF_INT | PF_ENUM)) v = floorf(v + 0.5f);
    return v;
}
void xl_track_defaults(xl_track *t)
{
    int i;
    for (i = 0; i < NP; i++) t->p[i] = XL_P[i].def;
    memset(t->st, 0, sizeof t->st);
    for (i = 0; i < XL_NSTEP; i++) { t->st[i].note = 60; t->st[i].vel = 100; t->st[i].prob = 100;
                                     t->st[i].ratchet = 1; t->st[i].cond = 1; }
    for (i = 0; i < XL_NSLICE; i++) { t->slv[i] = 0; t->spit[i] = 0; t->srev[i] = 0; }
}
void xl_globals_defaults(float *g)
{
    int i;
    for (i = 0; i < NG; i++) g[i] = XL_G[i].def;
}

/* ---- the queues: one writer, one reader each ---- */
int xl_q_push(xl_queue *q, xl_msg m)
{
    unsigned w = atomic_load_explicit(&q->w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&q->r, memory_order_acquire);
    if (w - r >= XL_QN) return 0;
    q->m[w % XL_QN] = m;
    atomic_store_explicit(&q->w, w + 1, memory_order_release);
    return 1;
}
int xl_q_pop(xl_queue *q, xl_msg *m)
{
    unsigned r = atomic_load_explicit(&q->r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&q->w, memory_order_acquire);
    if (r == w) return 0;
    *m = q->m[r % XL_QN];
    atomic_store_explicit(&q->r, r + 1, memory_order_release);
    return 1;
}

/* ---- DSP pieces ---- */
typedef struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; } bq;
static inline float bq_run(bq *f, int c, float x)
{
    float y = f->b0 * x + f->z1[c];
    f->z1[c] = f->b1 * x - f->a1 * y + f->z2[c];
    f->z2[c] = f->b2 * x - f->a2 * y;
    return y;
}
static void bq_set(bq *f, int kind, float hz)   /* kind: 0 LP2 (Q .7071), 1 HP2, 2 AP2, 3 LP1, 4 HP1, 5 AP1 */
{
    float w = 2.0f * (float)M_PI * hz / XL_SR;
    if (kind <= 2) {
        float cs = cosf(w), al = sinf(w) / (2.0f * 0.70710678f), a0 = 1.0f + al;
        if (kind == 0) { f->b0 = (1 - cs) / 2 / a0; f->b1 = (1 - cs) / a0; f->b2 = f->b0; }
        else if (kind == 1) { f->b0 = (1 + cs) / 2 / a0; f->b1 = -(1 + cs) / a0; f->b2 = f->b0; }
        else { f->b0 = (1 - al) / a0; f->b1 = -2 * cs / a0; f->b2 = 1; }
        f->a1 = -2 * cs / a0; f->a2 = (1 - al) / a0;
    } else {
        float k = tanf(w / 2), a0 = 1 + k;
        if (kind == 3) { f->b0 = k / a0; f->b1 = k / a0; }
        else if (kind == 4) { f->b0 = 1 / a0; f->b1 = -1 / a0; }
        else { f->b0 = (k - 1) / a0; f->b1 = 1; }
        f->b2 = 0; f->a1 = (k - 1) / a0; f->a2 = 0;
    }
}
static inline float db2lin(float db) { return db <= -59.5f ? 0.0f : powf(10.0f, db / 20.0f); }
/* ---- the band shapers: SHARD's DRIVE types (SHARD eng_shard.c shd_shape), the same circuits in float. 1.0 is full
 * scale. OVRDRV, DIST, FOLD, HALF, TUBE and FUZZ run at 4x (a 97-tap windowed-sinc FIR up and down: 24 samples of
 * latency at 48 kHz); CRUSH and DECIM at 1x, as on SHARD (their aliasing is the sound). Every band is delayed to
 * the same 24 samples, so the bands stay in line whatever each one's TYPE. */
#define OS 4
#define OS_N 97
#define OS_LAT 24
static float OS_H[OS_N];                  /* the FIR, at 4 x 48 kHz */
static float A_[11], ENV_UP, ENV_DN;      /* one-poles at 4x: 20 60 80 120 200 400 1100 1539 4500 5500 9000 Hz */
enum { A20, A60, A80, A120, A200, A400, A1100, A1539, A4500, A5500, A9000 };
int xl_os_off;                            /* (tests: run the shapers at 1x, to hear what 4x saves) */
static float onepole(float hz, float fs) { return 1.0f - expf(-2.0f * (float)M_PI * hz / fs); }
static void shaper_init(void)
{
    static const float HZ[11] = {20, 60, 80, 120, 200, 400, 1100, 1539, 4500, 5500, 9000};
    const float fs = XL_SR * OS, fc = 24000.0f / fs, beta = 7.0f;   /* cut at 24 kHz: passes 20k, stops 28k images */
    double i0b = 1, t = 1;
    int i, k;
    for (k = 1; k < 30; k++) { t *= (beta / 2 / k) * (beta / 2 / k); i0b += t; }
    for (i = 0; i < OS_N; i++) {
        double m = i - (OS_N - 1) / 2.0, x = 2 * fc * m, sn = fabs(x) < 1e-9 ? 1 : sin(M_PI * x) / (M_PI * x);
        double q = m / ((OS_N - 1) / 2.0), w = 1, tt = 1, arg = beta * sqrt(1 - q * q);
        for (k = 1; k < 30; k++) { tt *= (arg / 2 / k) * (arg / 2 / k); w += tt; }
        OS_H[i] = (float)(2 * fc * sn * w / i0b);
    }
    for (i = 0; i < 11; i++) A_[i] = onepole(HZ[i], fs);
    ENV_UP = 1.0f - expf(-1.0f / (0.000714f * fs));   /* OVRDRV's grid: ~0.7 ms to charge, ~46 ms to let go */
    ENV_DN = 1.0f - expf(-1.0f / (0.0464f * fs));
}
static inline float satf(float x, float l) { return x > l ? l : x < -l ? -l : x; }
static inline float softf(float x)          /* tanh, rational (SHARD's shd_soft) */
{
    if (x >= 3) return 1;
    if (x <= -3) return -1;
    return x * (27 + x * x) / (27 + 9 * x * x);
}
static inline float diodef(float x)         /* two silicon diodes: linear to 0.5, a knee to 1 at 1.5 */
{
    float a = fabsf(x), y, d;
    if (a <= 0.5f) return x;
    if (a >= 1.5f) y = 1;
    else { d = a - 0.5f; y = 0.5f + d - d * d * 0.5f; }
    return x < 0 ? -y : y;
}
static inline float cubicf(float x)         /* a transistor stage: 1.5 x - 0.5 x^3, flat past 1 */
{
    if (x >= 1) return 1;
    if (x <= -1) return -1;
    return (3 * x - x * x * x) * 0.5f;
}
static inline float triodef(float p)        /* the grid rounds the top early, cutoff clips the bottom later */
{
    return p >= 0 ? softf(p) : -diodef(-p * 0.75f) * 1.5f;
}
#define LPF(st, in, a) ((st) += ((in) - (st)) * (a))
typedef struct {                            /* one band's shaper, one channel */
    float f[6], env, prev, held;
    int cnt;
    float up[64], dn[2 * OS_N];             /* the 4x FIR's histories (each written twice: read without wrapping) */
    int upw, dnw;
    float dl[OS_LAT];                       /* the delay that keeps a 1x band in line */
    int dlw;
} shp_t;
typedef struct {                            /* what DRIVE sets, worked out once a block */
    float d, g, makeup, tube_g, tube_lp, rat_hi, rat_lo, rat_lp, war_g, war_th;
    int crush_sh, decim_n;
} shk_t;
static void shk_set(shk_t *k, float d)
{
    float R = 100000.0f * powf(d, 2.4f), fs = XL_SR * OS;
    k->d = d;
    k->g = powf(64.0f, d);
    k->makeup = powf(k->g, -0.4f);
    k->tube_g = powf(48.0f, d);
    k->tube_lp = onepole(9000.0f * powf(0.5f, d), fs);
    k->rat_hi = R / 47; k->rat_lo = R / 560;
    { float f = 1.6e6f / (1 + R / 47); k->rat_lp = onepole(f < 2500 ? 2500 : f > 20000 ? 20000 : f, fs); }
    k->war_g = powf(14.0f, d);
    k->war_th = 1800.0f * d * d / 32768.0f;
    k->crush_sh = 1 + (int)(d * 12);
    k->decim_n = 1 + (int)(d * 31);
}
/* OVRDRV: a 12AX7 stage: the coupling capacitor in, x1 .. x48, the grid biasing it colder after a hit, the triode's
 * curve, the plate's DC out, the Miller roll-off (9 kHz .. 4.5 kHz) */
static float sh_tube(shp_t *s, const shk_t *k, float x)
{
    float u, m, y;
    LPF(s->f[0], x, A_[A80]);
    u = satf((x - s->f[0]) * k->tube_g, 256);
    if (u > s->env) s->env += (u - s->env) * ENV_UP;
    else s->env -= s->env * ENV_DN;
    u -= s->env * 0.25f;
    m = (s->prev + u) * 0.5f; s->prev = u;
    y = (triodef(m) + triodef(u)) * 0.5f;
    LPF(s->f[1], y, A_[A20]);
    y -= s->f[1];
    LPF(s->f[2], y, k->tube_lp);
    return s->f[2] * (17000.0f / 32768);
}
/* DIST: a ProCo RAT: the highs (above 1.5 kHz) gain up to x2100, the lows (above 60 Hz) x180; the op-amp closes the
 * top as the gain rises; the rails round it; two diodes clip it hard; the FILTER after, ~4.5 kHz */
static float sh_rat(shp_t *s, const shk_t *k, float x)
{
    float g, u, m, y;
    LPF(s->f[0], x, A_[A60]);
    LPF(s->f[1], x, A_[A1539]);
    g = x + (x - s->f[1]) * k->rat_hi + (x - s->f[0]) * k->rat_lo;
    u = satf(g, 256);
    LPF(s->f[2], u, k->rat_lp);
    u = softf(s->f[2] * 2 / 15) * 7.5f;
    m = (s->prev + u) * 0.5f; s->prev = u;
    y = (diodef(m) + diodef(u)) * 0.5f;
    LPF(s->f[3], y, A_[A4500]);
    return s->f[3] * (18000.0f / 32768);
}
/* FUZZ: after Death By Audio's Fuzz War: three stages of x1 .. x14, a starved first stage's sputter, the Muff's
 * scooped tone stack, the diodes */
static float sh_war(shp_t *s, const shk_t *k, float x)
{
    float a, m, y, g = k->war_g, th = k->war_th;
    LPF(s->f[0], x, A_[A120]);
    a = x - s->f[0];
    a = a > th ? a - th : a < -th ? a + th : 0;
    a = cubicf(satf(a * g, 96));
    LPF(s->f[1], a, A_[A200]);
    a -= s->f[1];
    LPF(s->f[2], a, A_[A5500]);
    a = cubicf(s->f[2] * g);
    a = cubicf(a * g);
    LPF(s->f[3], a, A_[A400]);
    LPF(s->f[4], a, A_[A1100]);
    a = (s->f[3] + a - s->f[4]) * 1.5f;
    m = (s->prev + a) * 0.5f; s->prev = a;
    y = (diodef(m) + diodef(a)) * 0.5f;
    LPF(s->f[5], y, A_[A20]);
    return (y - s->f[5]) * (19000.0f / 32768);
}
/* one sample through a shaper (at whatever rate it is called) */
static float shape1(int ty, shp_t *s, const shk_t *k, float x)
{
    float p, y;
    x = satf(x, 4);
    switch (ty) {
    case SH_OVRDRV: return sh_tube(s, k, x);
    case SH_DIST: return sh_rat(s, k, x);
    case SH_FUZZ: return sh_war(s, k, x);
    case SH_CRUSH: { float st = ldexpf(1.0f, k->crush_sh) / 32768; return floorf(satf(x, 1) / st) * st; }
    case SH_DECIM: if (++s->cnt >= k->decim_n) { s->cnt = 0; s->held = x; } return s->held;
    default: break;
    }
    p = x * k->g;
    switch (ty) {
    case SH_FOLD: { float f = fmodf(satf(p, 96) + 1, 4); if (f < 0) f += 4; y = f < 2 ? f - 1 : 3 - f; break; }
    case SH_HALF: y = p > 0 ? softf(p) : softf(p) * 0.25f; break;
    case SH_TUBE: y = p >= 0 ? softf(p) : softf(p * 1.5f) * 0.75f; break;
    default: return x;
    }
    return y * k->makeup;
}
/* a band through its shaper: 4x for the analog types, else 1x and delayed to match; OFF only delays */
static float shape_band(int ty, shp_t *s, const shk_t *k, float x)
{
    if (ty == SH_OFF || ty == SH_CRUSH || ty == SH_DECIM || xl_os_off) {
        float y = ty == SH_OFF ? x : shape1(ty, s, k, x), o = s->dl[s->dlw];
        s->dl[s->dlw] = y;
        s->dlw = (s->dlw + 1) % OS_LAT;
        return o;
    }
    {
        float acc = 0;
        const float *u0, *d0;
        int ph, j;
        s->upw = (s->upw + 1) & 31;
        s->up[s->upw] = s->up[s->upw + 32] = x;
        u0 = &s->up[s->upw + 32];               /* u0[-j]: the input j samples ago */
        for (ph = 0; ph < OS; ph++) {
            float u = 0, v;
            for (j = 0; ph + OS * j < OS_N; j++) u += OS_H[ph + OS * j] * u0[-j];
            v = shape1(ty, s, k, u * OS);
            s->dnw = s->dnw + 1 >= OS_N ? 0 : s->dnw + 1;
            s->dn[s->dnw] = s->dn[s->dnw + OS_N] = v;
        }
        d0 = &s->dn[s->dnw + OS_N];             /* d0[-j]: the 4x signal j samples ago */
        for (j = 0; j < OS_N; j++) acc += OS_H[j] * d0[-j];
        return acc;
    }
}
/* the transfer curve the DECK page draws (a still picture of each type at its DRIVE) */
static float shape_curve(int ty, float d, float x)
{
    shk_t k;
    shk_set(&k, d);
    switch (ty) {
    case SH_OFF: return x;
    case SH_OVRDRV: return triodef(x * k.tube_g) * 0.6f;
    case SH_DIST: { float g = 1 + k.rat_hi; return diodef(softf(x * (g > 200 ? 200 : g) * 2 / 15) * 7.5f) * 0.6f; }
    case SH_FUZZ: return cubicf(cubicf(x * k.war_g) * k.war_g) * 0.6f;
    case SH_DECIM: { float n = (float)k.decim_n; return floorf(x * 8 / n * 2) / (8 / n * 2); }
    default: { shp_t s; memset(&s, 0, sizeof s); return shape1(ty, &s, &k, x); }
    }
}

typedef struct {
    bq lp[3][2], hp[3][2], ap[3];   /* per crossover: two sections each (LR4), the compensation allpass */
    bq ap0b, ap0c, ap1c;            /* band 0 needs AP at x2 and x3, band 1 at x3 (built per block) */
    float cx[3], cslope;            /* the settings the coefficients were made for */
    int cbands;
    shp_t sh[4][2];                 /* each band's shaper, per channel */
    float fl[4][2], fb[4][2], fy[4][2];   /* FEEDBACK: the loop's band-pass (low, band) and the band's last output */
    float fai[4][2], fao[4][2];     /* .. the shaper's mean level in and out (its gain, undone in the loop) */
    float sv1[2], sv2[2];           /* DJ filter (TPT SVF) */
    float dc_x[2], dc_y[2];
    float glv[4], gdr[4], gfb[4];   /* smoothed band gains */
} deck_t;

typedef struct {
    int on, note, track, rel, age, gate;   /* gate: frames left before the step's note-off (-1: held) */
    double pos, inc;
    int start, end, rev;
    float gain, env, atk, relk;
    const xl_sample *s;
} voice_t;

typedef struct {
    voice_t v[XL_NTRK][XL_NVOICE];
    voice_t pv;                    /* the FILES page's preview */
    xl_sample *pvs;                /* (owned: handed back with EV_FREE when replaced) */
    deck_t deck[XL_NTRK];
    int age;
    /* sequencer */
    double next_beat[XL_NTRK], step_beat[XL_NTRK];
    int idx[XL_NTRK], pass[XL_NTRK];
    double rat_beat[XL_NTRK][4]; int rat_n[XL_NTRK], rat_note[XL_NTRK], rat_vel[XL_NTRK];
    double beat;
    int was_playing;
    /* scenes */
    int sq, sq_bars;               /* queued scene (-1 none) and its morph length */
    float mfrom[XL_NTRK][NP], mto[XL_NTRK][NP];
    double mstart, mlen;           /* in beats; mlen 0: none */
    /* effects */
    float *dly; int dlen, dw; float dlp[2];
    float lim_g;
    float *cap; int capw, capn;
    float *rsb; int rsmax, rsn, rslen, rs_src, rs_dst, rs_state;
    float *rep; int replen, repn, reppos, repon;
    int kill[XL_NTRK];
    float spre[4096], spost[4096]; int spw;
    float lvl[XL_NTRK][2], lpan[XL_NTRK][2];
    int idle[XL_NTRK];             /* blocks of silence in and out: the deck sleeps after a second */
} rt_t;
static rt_t R;

void xl_engine_init(void)
{
    int t;
    memset(&R, 0, sizeof R);
    R.sq = -1;
    R.lim_g = 1;
    R.dlen = XL_SR * 2;
    R.dly = calloc((size_t)R.dlen * 2, sizeof(float));
    R.capn = XL_SR * XL_CAPTURE_SEC;
    R.cap = calloc((size_t)R.capn * 2, sizeof(float));
    R.rsmax = XL_SR * 50;
    R.rsb = calloc((size_t)R.rsmax * 2, sizeof(float));
    R.rep = calloc((size_t)XL_SR * 2 * 2, sizeof(float));
    for (t = 0; t < XL_NTRK; t++) R.deck[t].cbands = -1;
    shaper_init();
    XL.v.scene_playing = XL.v.scene_queued = -1;
    XL.v.morph_t = -1;
    for (t = 0; t < XL_NTRK; t++) { int i; for (i = 0; i < XL_NVOICE; i++) XL.v.vpos[t][i] = -1; }
}

/* ---- voices ---- */
static void voice_start(voice_t *v, int t, const xl_sample *s, int note, int vel, int gate)
{
    const xl_track *tk = &XL.trk[t];
    float tune = tk->p[P_TUNE], lv = 0;
    int eng = (int)tk->p[P_ENGINE];
    v->s = s; v->note = note; v->track = t; v->rel = 0; v->gate = gate; v->age = ++R.age;
    v->env = 0;
    v->atk = 1.0f / (tk->p[P_ATK] * 0.001f * XL_SR + 1);
    v->relk = expf(-1.0f / (tk->p[P_REL] * 0.001f * XL_SR / 5.0f + 1));
    if (eng == ENG_SHARD && s->nsl > 0) {
        int k = ((note - 36) % s->nsl + s->nsl) % s->nsl;
        v->start = s->sl[k]; v->end = s->sl[k + 1];
        v->rev = tk->srev[k];
        tune += tk->spit[k];
        lv = tk->slv[k];
        v->inc = powf(2.0f, tune / 12.0f);
    } else {
        v->start = 0; v->end = s->len; v->rev = 0;
        v->inc = powf(2.0f, (note - 60 + tune) / 12.0f);
    }
    v->pos = v->rev ? v->end - 1 : v->start;
    v->gain = vel / 127.0f * db2lin(lv);
    if (lv <= -59.5f) v->gain = 0;
    v->on = 1;
}
static void note_on(int t, int note, int vel, int gate)
{
    xl_track *tk = &XL.trk[t];
    voice_t *best = 0;
    int i;
    if (!tk->smp || tk->smp->len < 4) return;
    for (i = 0; i < XL_NVOICE; i++) if (!R.v[t][i].on) { best = &R.v[t][i]; break; }
    if (!best) {
        best = &R.v[t][0];
        for (i = 1; i < XL_NVOICE; i++) if (R.v[t][i].age < best->age) best = &R.v[t][i];
    }
    voice_start(best, t, tk->smp, note, vel, gate);
}
static void note_off(int t, int note)
{
    int i;
    for (i = 0; i < XL_NVOICE; i++)
        if (R.v[t][i].on && R.v[t][i].note == note && R.v[t][i].gate < 0) R.v[t][i].rel = 1;
}
/* render one voice into acc (stereo, n frames); returns 0 when it ended */
static int voice_run(voice_t *v, float *acc, int n)
{
    const float *d = v->s->d;
    int f, lo = v->start, hi = v->end - 1;
    for (f = 0; f < n; f++) {
        int i = (int)v->pos;
        float fr = (float)(v->pos - i), l, r;
        int i0 = i - 1 < lo ? lo : i - 1, i2 = i + 1 > hi ? hi : i + 1, i3 = i + 2 > hi ? hi : i + 2;
        if (i < lo || i > hi) { v->on = 0; return 0; }
        {   /* cubic Hermite */
            float a, b, c, e;
            a = d[2 * i0]; b = d[2 * i]; c = d[2 * i2]; e = d[2 * i3];
            l = b + 0.5f * fr * (c - a + fr * (2 * a - 5 * b + 4 * c - e + fr * (3 * (b - c) + e - a)));
            a = d[2 * i0 + 1]; b = d[2 * i + 1]; c = d[2 * i2 + 1]; e = d[2 * i3 + 1];
            r = b + 0.5f * fr * (c - a + fr * (2 * a - 5 * b + 4 * c - e + fr * (3 * (b - c) + e - a)));
        }
        if (v->gate > 0 && --v->gate == 0) v->rel = 1;
        if (v->rel) { v->env *= v->relk; if (v->env < 0.0005f) { v->on = 0; return 0; } }
        else if (v->env < 1) { v->env += v->atk; if (v->env > 1) v->env = 1; }
        {   /* a short fade at the slice's end */
            double left = v->rev ? v->pos - lo : hi - v->pos;
            float fade = left < 48 * v->inc ? (float)(left / (48 * v->inc)) : 1.0f;
            float g = v->gain * v->env * fade;
            acc[2 * f] += l * g; acc[2 * f + 1] += r * g;
        }
        v->pos += v->rev ? -v->inc : v->inc;
    }
    return 1;
}

/* ---- the deck ---- */
static void bq_coef(bq *dst, const bq *src)   /* take the coefficients, keep the state */
{
    dst->b0 = src->b0; dst->b1 = src->b1; dst->b2 = src->b2; dst->a1 = src->a1; dst->a2 = src->a2;
}
static void deck_coefs(deck_t *k, const xl_track *t)
{
    int bands = (int)t->p[P_BANDS], i;
    float slope = t->p[P_SLOPE], x[3];
    x[0] = t->p[P_X1]; x[1] = t->p[P_X2]; x[2] = t->p[P_X3];
    if (bands == k->cbands && slope == k->cslope && x[0] == k->cx[0] && x[1] == k->cx[1] && x[2] == k->cx[2]) return;
    if (x[1] < x[0] * 1.2f) x[1] = x[0] * 1.2f;   /* keep them in order */
    if (x[2] < x[1] * 1.2f) x[2] = x[1] * 1.2f;
    if (bands != k->cbands || slope != k->cslope) {   /* a new layout: start clean */
        memset(k->lp, 0, sizeof k->lp); memset(k->hp, 0, sizeof k->hp);
        memset(&k->ap0b, 0, sizeof k->ap0b); memset(&k->ap0c, 0, sizeof k->ap0c); memset(&k->ap1c, 0, sizeof k->ap1c);
        memset(k->fl, 0, sizeof k->fl); memset(k->fb, 0, sizeof k->fb); memset(k->fy, 0, sizeof k->fy);
    }
    for (i = 0; i < 3; i++) {
        bq c;
        float hz = x[i] > 20000 ? 20000 : x[i];
        int hi = slope >= 0.5f;
        bq_set(&c, hi ? 0 : 3, hz); bq_coef(&k->lp[i][0], &c); bq_coef(&k->lp[i][1], &c);
        bq_set(&c, hi ? 1 : 4, hz); bq_coef(&k->hp[i][0], &c); bq_coef(&k->hp[i][1], &c);
        bq_set(&k->ap[i], hi ? 2 : 5, hz);
    }
    bq_coef(&k->ap0b, &k->ap[1]); bq_coef(&k->ap0c, &k->ap[2]); bq_coef(&k->ap1c, &k->ap[2]);
    k->cbands = bands; k->cslope = slope;
    k->cx[0] = t->p[P_X1]; k->cx[1] = t->p[P_X2]; k->cx[2] = t->p[P_X3];
}
/* split one sample into bands (b[0..bands-1]) */
static inline void deck_split(deck_t *k, int c, float x, int bands, float sgn, float *b)
{
    float lo, hi;
    lo = bq_run(&k->lp[0][1], c, bq_run(&k->lp[0][0], c, x));
    hi = sgn * bq_run(&k->hp[0][1], c, bq_run(&k->hp[0][0], c, x));
    if (bands == 2) { b[0] = lo; b[1] = hi; return; }
    b[0] = bq_run(&k->ap0b, c, lo);
    lo = bq_run(&k->lp[1][1], c, bq_run(&k->lp[1][0], c, hi));
    hi = sgn * bq_run(&k->hp[1][1], c, bq_run(&k->hp[1][0], c, hi));
    if (bands == 3) { b[1] = lo; b[2] = hi; return; }
    b[0] = bq_run(&k->ap0c, c, b[0]);
    b[1] = bq_run(&k->ap1c, c, lo);
    b[2] = bq_run(&k->lp[2][1], c, bq_run(&k->lp[2][0], c, hi));
    b[3] = sgn * bq_run(&k->hp[2][1], c, bq_run(&k->hp[2][0], c, hi));
}
static void deck_run(int t, float *buf, int n)
{
    deck_t *k = &R.deck[t];
    const xl_track *tk = &XL.trk[t];
    int bands = (int)tk->p[P_BANDS], f, c, b, ty[4];
    float sgn = tk->p[P_SLOPE] >= 0.5f ? 1.0f : -1.0f;
    float tlv[4], tfb[4], fbf[4], dj = tk->p[P_DJ], g1, gk, gR, sm = 1.0f / 64;
    shk_t sk[4];
    deck_coefs(k, tk);
    for (b = 0; b < 4; b++) {
        tlv[b] = db2lin(tk->p[P_LV0 + b]);
        if (b == 0 && R.kill[t]) tlv[b] = 0;
        ty[b] = (int)tk->p[P_TY0 + b];
        if (ty[b] < 0 || ty[b] >= SH_N) ty[b] = SH_OFF;
        shk_set(&sk[b], tk->p[P_DR0 + b]);
        {   /* FEEDBACK: as SHARD's, 0 .. 127 -> a^2 x 4 (Q15): from about half the loop holds itself */
            float a = tk->p[P_FB0 + b] / 0.95f * 127;
            tfb[b] = a * a * 4 / 32768.0f;
        }
        {   /* the loop's band-pass sits in the band: between its crossovers (geometric), as SHARD's 150 / 900 / 4k */
            float lo = b == 0 ? 40.0f : tk->p[P_X1 + b - 1], hi = b == bands - 1 ? 12000.0f : tk->p[P_X1 + b], fc = sqrtf(lo * hi);
            fbf[b] = 2 * sinf((float)M_PI * (fc > 16000 ? 16000 : fc) / XL_SR);
        }
    }
    {   /* DJ filter: below 0 a low-pass falling from 20 kHz, above 0 a high-pass rising from 20 Hz */
        float hz = dj < 0 ? 20000.0f * powf(80.0f / 20000.0f, -dj) : 20.0f * powf(8000.0f / 20.0f, dj);
        g1 = tanf((float)M_PI * hz / XL_SR);
        gk = 2.0f - 2.0f * 0.55f;
        gR = 1.0f / (1.0f + g1 * (g1 + gk));
    }
    for (f = 0; f < n; f++) {
        for (b = 0; b < 4; b++) {
            k->glv[b] += (tlv[b] - k->glv[b]) * sm;
            k->gfb[b] += (tfb[b] - k->gfb[b]) * sm;
        }
        float inm = (buf[2 * f] + buf[2 * f + 1]) * 0.5f;
        for (c = 0; c < 2; c++) {
            float x = buf[2 * f + c], bb[4], y = 0;
            deck_split(k, c, x, bands, sgn, bb);
            for (b = 0; b < bands; b++) {
                float v = bb[b], fg = k->gfb[b];
                if (fg > 0.0001f) {   /* FEEDBACK: the band's last output, band-passed (Q 2), back in */
                    float hi, cq = 1;
                    k->fl[b][c] = satf(k->fl[b][c] + fbf[b] * k->fb[b][c], 4);
                    hi = satf(k->fy[b][c] - k->fl[b][c] - k->fb[b][c] * 0.5f, 4);
                    k->fb[b][c] = satf(k->fb[b][c] + fbf[b] * hi, 4);
                    if (ty[b] != SH_OFF) {   /* the shaper's gain undone (up to 4): quiet, the loop dies away */
                        float o = k->fao[b][c] > 0.0156f ? k->fao[b][c] : 0.0156f;
                        cq = k->fai[b][c] / o;
                        cq = cq > 4 ? 4 : cq;
                    }
                    v += k->fb[b][c] * fg * cq;
                }
                if (ty[b] != SH_OFF) {
                    float in = satf(v, 4), a = fabsf(in);
                    v = shape_band(ty[b], &k->sh[b][c], &sk[b], in);
                    if (fg > 0.0001f) {
                        float m = fabsf(v);
                        if (m > 0.366f) {   /* the howl's ceiling: bent over, 4:1, to 0.61 */
                            m = 0.366f + (m - 0.366f) * 0.25f;
                            m = m > 0.61f ? 0.61f : m;
                            v = v < 0 ? -m : m;
                        }
                        k->fai[b][c] += (a - k->fai[b][c]) / 128;
                        k->fao[b][c] += (fabsf(v) - k->fao[b][c]) / 128;
                    }
                } else {
                    if (fg > 0.0001f) v = softf(satf(v, 3));   /* (no shaper: a soft clip holds the loop) */
                    v = shape_band(SH_OFF, &k->sh[b][c], &sk[b], v);
                }
                k->fy[b][c] = satf(v, 2);
                y += v * k->glv[b];
            }
            if (dj < -0.01f || dj > 0.01f) {   /* TPT state-variable filter */
                float v3 = y - k->sv2[c];
                float v1 = gR * (k->sv1[c] + g1 * v3);
                float v2 = k->sv2[c] + g1 * v1;
                k->sv1[c] = 2 * v1 - k->sv1[c]; k->sv2[c] = 2 * v2 - k->sv2[c];
                y = dj < 0 ? v2 : y - gk * v1 - v2;
            }
            {   /* DC blocker */
                float o = y - k->dc_x[c] + 0.9995f * k->dc_y[c];
                k->dc_x[c] = y; k->dc_y[c] = o; y = o;
            }
            if (y != y || y > 64 || y < -64) {   /* (never NaN out) */
                memset(k->fl, 0, sizeof k->fl); memset(k->fb, 0, sizeof k->fb); memset(k->fy, 0, sizeof k->fy);
                memset(k->sh, 0, sizeof k->sh); y = 0;
            }
            buf[2 * f + c] = y;
        }
        if (t == atomic_load_explicit(&XL.spec_track, memory_order_relaxed)) {
            R.spre[R.spw & 4095] = inm;
            R.spost[R.spw & 4095] = (buf[2 * f] + buf[2 * f + 1]) * 0.5f;
            R.spw++;
        }
    }
}
void xl_spec_read(float *pre, float *post, int n)
{
    int w = R.spw, i;
    for (i = 0; i < n; i++) { pre[i] = R.spre[(w - n + i) & 4095]; post[i] = R.spost[(w - n + i) & 4095]; }
}

/* ---- the sequencer ---- */
static const double DIV_BEATS[DIV_N] = {1.0, 0.5, 0.25, 0.125, 1.0 / 3, 1.0 / 6};
static unsigned rng = 2463534242u;
static inline unsigned xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void seq_start(void)
{
    int t;
    R.beat = 0;
    for (t = 0; t < XL_NTRK; t++) { R.next_beat[t] = 0; R.idx[t] = 0; R.pass[t] = 0; R.rat_n[t] = 0; }
}
static int track_audible(int t)
{
    int s, any = 0;
    for (s = 0; s < XL_NTRK; s++) if (XL.trk[s].p[P_SOLO] >= 0.5f) any = 1;
    if (any) return XL.trk[t].p[P_SOLO] >= 0.5f;
    return XL.trk[t].p[P_MUTE] < 0.5f;
}
static void seq_block(double b0, double b1, double spb /* samples per beat */)
{
    int t;
    for (t = 0; t < XL_NTRK; t++) {
        xl_track *tk = &XL.trk[t];
        int len = (int)tk->p[P_SLEN], div = (int)tk->p[P_SDIV];
        double st = DIV_BEATS[div < 0 || div >= DIV_N ? DIV_16 : div];
        double sw = (XL.g[G_SWING] - 50) / 50.0;
        if (len < 1) len = 1;
        if (len > XL_NSTEP) len = XL_NSTEP;
        while (R.next_beat[t] < b1) {
            int i = R.idx[t] % len;
            xl_step *s = &tk->st[i];
            double dur = st;
            if (div == DIV_8 || div == DIV_16) dur = st * (i % 2 == 0 ? 1 + sw : 1 - sw);
            XL.v.step[t] = i;
            if (s->on && (s->cond <= 1 || R.pass[t] % s->cond == 0) && (int)(xr() % 100) < s->prob) {
                int r = s->ratchet < 1 ? 1 : s->ratchet > 4 ? 4 : s->ratchet, k;
                int gate = (int)(dur / r * 0.8 * spb);
                note_on(t, s->note, s->vel, gate < 32 ? 32 : gate);
                R.rat_n[t] = 0;
                for (k = 1; k < r; k++) R.rat_beat[t][R.rat_n[t]++] = R.next_beat[t] + dur * k / r;
                R.rat_note[t] = s->note; R.rat_vel[t] = s->vel;
            }
            R.step_beat[t] = R.next_beat[t];
            R.next_beat[t] += dur;
            R.idx[t]++;
            if (R.idx[t] % len == 0) R.pass[t]++;
        }
        {   /* ratchets due in this block */
            int k, m = 0;
            for (k = 0; k < R.rat_n[t]; k++) {
                if (R.rat_beat[t][k] < b1) {
                    int gate = (int)(st / 4 * 0.8 * spb);
                    note_on(t, R.rat_note[t], R.rat_vel[t], gate < 32 ? 32 : gate);
                } else R.rat_beat[t][m++] = R.rat_beat[t][k];
            }
            R.rat_n[t] = m;
        }
    }
    (void)b0;
}
/* a note played while REC is on lands on the nearest step */
static void rec_note(int t, int note, int vel)
{
    xl_track *tk = &XL.trk[t];
    int len = (int)tk->p[P_SLEN], div = (int)tk->p[P_SDIV], i;
    double st = DIV_BEATS[div < 0 || div >= DIV_N ? DIV_16 : div];
    if (len < 1) len = 1;
    i = (R.idx[t] - 1) % len;
    if (R.beat - R.step_beat[t] > st * 0.5) i = R.idx[t] % len;
    if (i < 0) i += len;
    tk->st[i].on = 1; tk->st[i].note = (uint8_t)note; tk->st[i].vel = (uint8_t)vel;
    tk->st[i].prob = 100; if (tk->st[i].ratchet < 1) tk->st[i].ratchet = 1; if (tk->st[i].cond < 1) tk->st[i].cond = 1;
}

/* ---- scenes ---- */
static int is_discrete(int i) { return (XL_P[i].flags & (PF_INT | PF_ENUM)) != 0; }
static void scene_apply(int s, int bars)
{
    int t, i;
    xl_scene *sc = &XL.scene[s];
    if (!sc->valid) return;
    for (t = 0; t < XL_NTRK; t++) memcpy(XL.trk[t].st, sc->st[t], sizeof sc->st[t]);
    if (bars <= 0) {
        for (t = 0; t < XL_NTRK; t++) memcpy(XL.trk[t].p, sc->p[t], sizeof sc->p[t]);
        R.mlen = 0; XL.v.morph_t = -1;
    } else {
        for (t = 0; t < XL_NTRK; t++) {
            memcpy(R.mfrom[t], XL.trk[t].p, sizeof R.mfrom[t]);
            memcpy(R.mto[t], sc->p[t], sizeof R.mto[t]);
            for (i = 0; i < NP; i++) if (is_discrete(i)) XL.trk[t].p[i] = sc->p[t][i];
        }
        R.mstart = R.beat; R.mlen = bars * 4.0;
    }
    XL.v.scene_playing = s;
    { xl_msg m = {EV_SCENE, s, 0, 0, 0}; xl_q_push(&XL_EV, m); }
}
static void morph_block(void)
{
    int t, i;
    float u;
    if (R.mlen <= 0) return;
    u = (float)((R.beat - R.mstart) / R.mlen);
    if (u >= 1) u = 1;
    for (t = 0; t < XL_NTRK; t++)
        for (i = 0; i < NP; i++)
            if (!is_discrete(i)) {
                float a = R.mfrom[t][i], b = R.mto[t][i];
                if (XL_P[i].flags & PF_LOG) XL.trk[t].p[i] = a * powf(b / a, u);
                else XL.trk[t].p[i] = a + (b - a) * u;
            }
    XL.v.morph_t = u;
    if (u >= 1) { R.mlen = 0; XL.v.morph_t = -1; }
}

/* ---- capture, resampling ---- */
int xl_capture_copy(float *dst, int maxframes)
{
    int filled = XL.v.capture_filled, w = R.capw, n = filled < maxframes ? filled : maxframes, i;
    n -= XL_BLOCK * 4;   /* (stay clear of the block being written) */
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        int k = (w - n + i) % R.capn;
        if (k < 0) k += R.capn;
        dst[2 * i] = R.cap[2 * k]; dst[2 * i + 1] = R.cap[2 * k + 1];
    }
    return n;
}
const float *xl_resample_buf(int *frames) { *frames = R.rsn; return R.rsb; }

/* ---- MIDI in (the MIDI thread) ---- */
void xl_midi_in(uint8_t st, uint8_t d1, uint8_t d2)
{
    xl_msg m = {EV_MIDI, st, d1, d2, 0};
    xl_q_push(&XL_MIDI, m);
}
static int note_track(int note, int chan)
{
    int sel = atomic_load(&XL.sel_track), split = (int)XL.g[G_SPLIT];
    (void)chan;
    if (split > 0 && note < split) { int lt = (int)XL.g[G_LOWTRK]; return lt <= 0 ? sel : lt - 1; }
    { int ht = (int)XL.g[G_HITRK]; return ht <= 0 ? sel : ht - 1; }
}
static void handle_note(int t, int note, int vel)
{
    if (vel > 0) {
        note_on(t, note, vel, -1);
        if (atomic_load(&XL.rec) && atomic_load(&XL.playing)) rec_note(t, note, vel);
    } else note_off(t, note);
}
static void scene_request(int s, int bars)
{
    if ((int)XL.g[G_QUANT] == 0 || !atomic_load(&XL.playing)) { scene_apply(s, bars); R.sq = -1; }
    else { R.sq = s; R.sq_bars = bars; }
    XL.v.scene_queued = R.sq;
}
static void midi_block(void)
{
    xl_msg m;
    while (xl_q_pop(&XL_MIDI, &m)) {
        int st = m.a & 0xF0, ch = m.a & 0x0F;
        if (st == 0x90 || st == 0x80) {
            int vel = st == 0x90 ? m.c : 0;
            if (ch == 9) {   /* the Axiom's pads (channel 10) */
                if ((int)XL.g[G_PADMODE] == 0 && !atomic_load(&XL.shift_held)) {
                    if (vel > 0) {
                        int s = ((m.b - 36) % 8 + 8) % 8;
                        if (XL.scene[s].valid) scene_request(s, 0);
                    }
                } else handle_note(atomic_load(&XL.sel_track), 36 + (m.b - 36), vel);
            } else handle_note(note_track(m.b, ch), m.b, vel);
        }
        xl_q_push(&XL_EV, m);   /* the screens see everything (learning, the MIDI monitor) */
    }
}

/* ---- commands from the screens ---- */
static void cmd_block(void)
{
    xl_msg m;
    while (xl_q_pop(&XL_CMD, &m)) {
        switch (m.type) {
        case CMD_NOTE_ON: note_on(m.a, m.b, m.c, -1);
            if (atomic_load(&XL.rec) && atomic_load(&XL.playing)) rec_note(m.a, m.b, m.c);
            break;
        case CMD_NOTE_OFF: note_off(m.a, m.b); break;
        case CMD_PLAY: seq_start(); atomic_store(&XL.playing, 1); break;
        case CMD_STOP: {
            int t, i;
            atomic_store(&XL.playing, 0);
            for (t = 0; t < XL_NTRK; t++) for (i = 0; i < XL_NVOICE; i++) if (R.v[t][i].on) R.v[t][i].rel = 1;
            R.sq = -1; XL.v.scene_queued = -1;
            if (R.rs_state) { R.rs_state = 0; XL.v.resampling = 0; }
            break;
        }
        case CMD_SCENE: scene_request(m.a, m.b); break;
        case CMD_SAMPLE: {
            int t = m.a, i;
            xl_msg e = {EV_FREE, 0, 0, 0, XL.trk[t].smp};
            for (i = 0; i < XL_NVOICE; i++) R.v[t][i].on = 0;
            XL.trk[t].smp = (xl_sample *)m.ptr;
            if (e.ptr) xl_q_push(&XL_EV, e);
            break;
        }
        case CMD_PREVIEW: {
            xl_sample *s = (xl_sample *)m.ptr;
            R.pv.on = 0;
            if (R.pvs && R.pvs != s) { xl_msg e = {EV_FREE, 0, 0, 0, R.pvs}; xl_q_push(&XL_EV, e); }
            R.pvs = s;
            if (s && s->len > 4) {
                R.pv.s = s; R.pv.start = 0; R.pv.end = s->len; R.pv.pos = 0; R.pv.inc = 1; R.pv.rev = 0;
                R.pv.gain = 1; R.pv.env = 1; R.pv.atk = 1; R.pv.relk = 0.999f; R.pv.rel = 0; R.pv.gate = -1;
                R.pv.on = 1;
            }
            break;
        }
        case CMD_PREVIEW_STOP: if (R.pv.on) R.pv.rel = 1; break;
        case CMD_RESAMPLE:
            R.rs_src = m.a; R.rs_dst = m.b;
            R.rslen = (int)(m.c * 4 * 60.0 / XL.g[G_BPM] * XL_SR);
            if (R.rslen > R.rsmax) R.rslen = R.rsmax;
            R.rsn = 0; R.rs_state = atomic_load(&XL.playing) ? 1 : 2;
            XL.v.resampling = R.rs_state;
            break;
        case CMD_REPEAT:
            if (m.a > 0) {
                double beats = m.a == 1 ? 1.0 : m.a == 2 ? 0.5 : m.a == 3 ? 0.25 : 0.125;
                R.replen = (int)(beats * 60.0 / XL.g[G_BPM] * XL_SR);
                if (R.replen > XL_SR * 2) R.replen = XL_SR * 2;
                R.repn = 0; R.reppos = 0; R.repon = 1;
            } else R.repon = 0;
            XL.v.repeat = m.a;
            break;
        case CMD_KILL: if (m.a >= 0 && m.a < XL_NTRK) R.kill[m.a] = m.b; break;
        case CMD_PANIC: memset(R.v, 0, sizeof R.v); R.pv.on = 0; break;
        }
    }
}

/* ---- one block of 64 frames ---- */
static float tb[XL_NTRK][XL_BLOCK * 2];
static void render_block(float *mainout, float *busout, int n)
{
    float ma[XL_BLOCK * 2], ba[XL_BLOCK * 2], bb[XL_BLOCK * 2], fx[XL_BLOCK * 2];
    int t, i, f, playing;
    double spb, b0, b1;
    memset(ma, 0, sizeof ma); memset(ba, 0, sizeof ba); memset(bb, 0, sizeof bb); memset(fx, 0, sizeof fx);
    midi_block();
    cmd_block();
    playing = atomic_load(&XL.playing);
    spb = 60.0 / XL.g[G_BPM] * XL_SR; b0 = R.beat; b1 = R.beat + n / spb;
    if (playing) {
        if ((int)(b1 / 4) != (int)(b0 / 4) || b0 == 0) {   /* a bar starts in this block */
            if (R.sq >= 0) { scene_apply(R.sq, R.sq_bars); R.sq = -1; XL.v.scene_queued = -1; }
            if (R.rs_state == 1) { R.rs_state = 2; R.rsn = 0; XL.v.resampling = 2; }
        } else if ((int)XL.g[G_QUANT] == 1 && (int)b1 != (int)b0 && R.sq >= 0) {
            scene_apply(R.sq, R.sq_bars); R.sq = -1; XL.v.scene_queued = -1;
        }
        seq_block(b0, b1, spb);
        R.beat = b1;
        morph_block();
    }
    XL.v.beat = R.beat;
    for (t = 0; t < XL_NTRK; t++) {
        xl_track *tk = &XL.trk[t];
        float *buf = tb[t], pk = 0, lv, pan, gl, gr;
        int any = 0;
        memset(buf, 0, sizeof(float) * 2 * n);
        for (i = 0; i < XL_NVOICE; i++) {
            voice_t *v = &R.v[t][i];
            if (v->on && v->s == tk->smp) { voice_run(v, buf, n); any = 1; }
            else v->on = 0;
            XL.v.vpos[t][i] = v->on ? (int)v->pos : -1;
        }
        if (any) R.idle[t] = 0;
        if (R.idle[t] < XL_SR / XL_BLOCK) {
            float o = 0;
            deck_run(t, buf, n);
            for (f = 0; f < 2 * n; f++) if (fabsf(buf[f]) > o) o = fabsf(buf[f]);
            if (!any && o < 1e-5f) R.idle[t]++;
            else R.idle[t] = 0;
        }
        lv = track_audible(t) ? db2lin(tk->p[P_LEVEL]) : 0;
        pan = tk->p[P_PAN];
        gl = lv * cosf((pan + 1) * 0.25f * (float)M_PI) * 1.41421356f;
        gr = lv * sinf((pan + 1) * 0.25f * (float)M_PI) * 1.41421356f;
        if (gl > 1.41421356f * lv) gl = lv;
        for (f = 0; f < n; f++) {
            float wl = R.lvl[t][0] + (gl - R.lvl[t][0]) * (f + 1) / n;
            float wr = R.lvl[t][1] + (gr - R.lvl[t][1]) * (f + 1) / n;
            float l = buf[2 * f] * wl, r = buf[2 * f + 1] * wr, a;
            ma[2 * f] += l; ma[2 * f + 1] += r;
            a = tk->p[P_SENDA]; ba[2 * f] += l * a; ba[2 * f + 1] += r * a;
            a = tk->p[P_SENDB]; bb[2 * f] += l * a; bb[2 * f + 1] += r * a;
            a = tk->p[P_SENDFX]; fx[2 * f] += l * a; fx[2 * f + 1] += r * a;
            a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (a > pk) pk = a;
            if (R.rs_state == 2 && R.rs_src == t && R.rsn < R.rslen) {
                R.rsb[2 * R.rsn] = l; R.rsb[2 * R.rsn + 1] = r; R.rsn++;
            }
        }
        R.lvl[t][0] = gl; R.lvl[t][1] = gr;
        XL.v.meter[t] = pk > XL.v.meter[t] ? pk : XL.v.meter[t] * 0.93f;
    }
    /* buses through the crossfader, back into the master and out on their own */
    {
        float x = XL.g[G_XFADE], ga, gb, ra = db2lin(XL.g[G_RETA]), rb = db2lin(XL.g[G_RETB]);
        int cv = (int)XL.g[G_XCURVE];
        if (cv == 0) { ga = cosf(x * 1.5707963f); gb = sinf(x * 1.5707963f); }
        else if (cv == 1) { ga = x < 0.5f ? 1 : 2 - 2 * x; gb = x > 0.5f ? 1 : 2 * x; }
        else { ga = x < 0.95f ? 1 : 0; gb = x > 0.05f ? 1 : 0; }
        for (f = 0; f < n; f++) {
            float al = ba[2 * f] * ga, ar = ba[2 * f + 1] * ga, bl = bb[2 * f] * gb, br = bb[2 * f + 1] * gb;
            ma[2 * f] += al * ra + bl * rb; ma[2 * f + 1] += ar * ra + br * rb;
            if (busout) { busout[4 * f] = al; busout[4 * f + 1] = ar; busout[4 * f + 2] = bl; busout[4 * f + 3] = br; }
        }
    }
    /* the FX loop: a ping-pong delay (a dotted eighth) stands in for the outside effects */
    {
        int dt = (int)(0.75 * spb);
        float fr = db2lin(XL.g[G_FXRET]);
        if (dt >= R.dlen) dt = R.dlen - 1;
        for (f = 0; f < n; f++) {
            int rd = (R.dw - dt + R.dlen) % R.dlen;
            float yl = R.dly[2 * rd], yr = R.dly[2 * rd + 1];
            R.dlp[0] += (yr - R.dlp[0]) * 0.4f; R.dlp[1] += (yl - R.dlp[1]) * 0.4f;
            R.dly[2 * R.dw] = fx[2 * f] + R.dlp[0] * 0.45f;
            R.dly[2 * R.dw + 1] = fx[2 * f + 1] + R.dlp[1] * 0.45f;
            R.dw = (R.dw + 1) % R.dlen;
            ma[2 * f] += yl * fr; ma[2 * f + 1] += yr * fr;
        }
    }
    /* the preview (straight to the master) */
    if (R.pv.on) {
        float pv[XL_BLOCK * 2], g = db2lin(XL.g[G_PREVIEW]);
        memset(pv, 0, sizeof pv);
        voice_run(&R.pv, pv, n);
        for (f = 0; f < 2 * n; f++) ma[f] += pv[f] * g;
    }
    /* beat repeat */
    if (R.repon) {
        for (f = 0; f < n; f++) {
            if (R.repn < R.replen) { R.rep[2 * R.repn] = ma[2 * f]; R.rep[2 * R.repn + 1] = ma[2 * f + 1]; R.repn++; }
            else {
                ma[2 * f] = R.rep[2 * R.reppos]; ma[2 * f + 1] = R.rep[2 * R.reppos + 1];
                R.reppos = (R.reppos + 1) % R.replen;
            }
        }
    }
    /* master: level, limiter */
    {
        float mg = db2lin(XL.g[G_MASTER]), pl = 0, pr = 0;
        for (f = 0; f < n; f++) {
            float l = ma[2 * f] * mg, r = ma[2 * f + 1] * mg, a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (a * R.lim_g > 0.95f) R.lim_g = 0.95f / a;
            else R.lim_g += (1 - R.lim_g) * 0.0002f;
            l *= R.lim_g; r *= R.lim_g;
            l = l > 1 ? 1 : l < -1 ? -1 : l; r = r > 1 ? 1 : r < -1 ? -1 : r;
            mainout[2 * f] = l; mainout[2 * f + 1] = r;
            if (fabsf(l) > pl) pl = fabsf(l);
            if (fabsf(r) > pr) pr = fabsf(r);
            R.cap[2 * R.capw] = l; R.cap[2 * R.capw + 1] = r;
            R.capw = (R.capw + 1) % R.capn;
            if (R.rs_state == 2 && R.rs_src < 0 && R.rsn < R.rslen) {
                R.rsb[2 * R.rsn] = l; R.rsb[2 * R.rsn + 1] = r; R.rsn++;
            }
        }
        XL.v.mmeter[0] = pl > XL.v.mmeter[0] ? pl : XL.v.mmeter[0] * 0.93f;
        XL.v.mmeter[1] = pr > XL.v.mmeter[1] ? pr : XL.v.mmeter[1] * 0.93f;
        if (XL.v.capture_filled < R.capn) XL.v.capture_filled += n;
    }
    if (R.rs_state == 2 && R.rsn >= R.rslen) {
        xl_msg e = {EV_RESAMPLED, R.rs_dst, R.rsn, 0, 0};
        R.rs_state = 0; XL.v.resampling = 0;
        xl_q_push(&XL_EV, e);
    }
}

static void no_denormals(void)   /* (filters decaying toward silence would otherwise slow to a crawl) */
{
#if defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#endif
}
void xl_render(float *out, int frames, int channels)
{
    static float rem[XL_BLOCK * 2], remb[XL_BLOCK * 4];
    static int remn = 0, remp = 0;
    double t0 = plat_time();
    int f = 0, c;
    no_denormals();
    while (f < frames) {
        if (remp >= remn) { render_block(rem, remb, XL_BLOCK); remn = XL_BLOCK; remp = 0; }
        for (; remp < remn && f < frames; remp++, f++) {
            out[f * channels] = rem[2 * remp];
            if (channels > 1) out[f * channels + 1] = rem[2 * remp + 1];
            for (c = 2; c < channels; c++)   /* outputs 3-6: Bus A, Bus B (on an interface with them) */
                out[f * channels + c] = c < 6 ? remb[4 * remp + c - 2] : 0;
        }
    }
    {
        double used = (plat_time() - t0) / ((double)frames / XL_SR);
        XL.v.cpu = XL.v.cpu * 0.95f + (float)used * 0.05f;
    }
}
