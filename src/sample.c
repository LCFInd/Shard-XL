/* SPDX-License-Identifier: GPL-3.0-only */
/* Samples: loading (WAV here, anything else through the platform: the Mac's own decoders), conversion to
 * 48 kHz stereo (windowed sinc), slicing (onsets or even), editing (reverse, normalize, stretch to the tempo
 * without changing the pitch), WAV writing (captures), and the demo kit that a first start plays. */
#include "xl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void xl_sample_free(xl_sample *s)
{
    if (!s) return;
    free(s->d); free(s->pk); free(s);
}
void xl_sample_peaks(xl_sample *s)
{
    int b, f;
    free(s->pk);
    s->npk = (s->len + XL_PKBLK - 1) / XL_PKBLK;
    s->pk = calloc((size_t)s->npk * 2 + 2, sizeof(float));
    for (b = 0; b < s->npk; b++) {
        float lo = 0, hi = 0;
        for (f = b * XL_PKBLK; f < (b + 1) * XL_PKBLK && f < s->len; f++) {
            float m = (s->d[2 * f] + s->d[2 * f + 1]) * 0.5f;
            if (m < lo) lo = m;
            if (m > hi) hi = m;
        }
        s->pk[2 * b] = lo; s->pk[2 * b + 1] = hi;
    }
}
static double bessel_i0(double x)
{
    double s = 1, t = 1; int k;
    for (k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
/* to 48 kHz stereo: a Kaiser-windowed sinc, 32 zero crossings each side */
xl_sample *xl_sample_new(const float *in, int frames, int rate, int channels, const char *name)
{
    xl_sample *s = calloc(1, sizeof *s);
    int f;
    if (!s || frames <= 0 || channels <= 0) { free(s); return 0; }
    if (rate == XL_SR) {
        s->len = frames;
        s->d = malloc(sizeof(float) * 2 * (size_t)frames);
        for (f = 0; f < frames; f++) {
            s->d[2 * f] = in[f * channels];
            s->d[2 * f + 1] = in[f * channels + (channels > 1)];
        }
    } else {
        const int Z = 32, WT = 4096;
        double ratio = (double)XL_SR / rate, cut = ratio < 1 ? ratio : 1.0, b = 8.0, i0b = bessel_i0(b);
        int out = (int)((double)frames * ratio), o, q;
        float *wt = malloc(sizeof(float) * (WT + 2));   /* the Kaiser window over 0..Z */
        for (q = 0; q <= WT + 1; q++) { double u = (double)q / WT; wt[q] = u >= 1 ? 0 : (float)(bessel_i0(b * sqrt(1 - u * u)) / i0b); }
        s->len = out;
        s->d = calloc((size_t)out * 2, sizeof(float));
        for (o = 0; o < out; o++) {
            double c = o / ratio, l = 0, r = 0, wsum = 0;
            int ci = (int)floor(c), k;
            for (k = ci - Z + 1; k <= ci + Z; k++) {
                double x = (c - k) * cut, w, sn;
                if (k < 0 || k >= frames) continue;
                if (fabs(c - k) >= Z) continue;
                sn = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
                { double u = fabs(c - k) / Z * WT; int ui = (int)u; double fr = u - ui;
                  w = wt[ui] + (wt[ui + 1] - wt[ui]) * fr; }
                w *= sn * cut;
                l += w * in[k * channels]; r += w * in[k * channels + (channels > 1)];
                wsum += w;
            }
            (void)wsum;
            s->d[2 * o] = (float)l; s->d[2 * o + 1] = (float)r;
        }
        free(wt);
    }
    snprintf(s->name, sizeof s->name, "%s", name ? name : "sample");
    s->nsl = 1; s->sl[0] = 0; s->sl[1] = s->len;
    xl_sample_peaks(s);
    return s;
}

/* ---- WAV ---- */
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
int xl_wav_read(const char *path, float **inter, int *frames, int *rate, int *channels)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *b; long n; int fmt = 0, ch = 0, bits = 0, sr = 0;
    const unsigned char *data = 0; unsigned dlen = 0; long p = 12;
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n < 44) { fclose(fp); return -1; }
    b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(b); return -1; }
    fclose(fp);
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); return -1; }
    while (p + 8 <= n) {
        unsigned cl = rd32(b + p + 4);
        if (!memcmp(b + p, "fmt ", 4) && cl >= 16) {
            fmt = (int)rd16(b + p + 8); ch = (int)rd16(b + p + 10); sr = (int)rd32(b + p + 12); bits = (int)rd16(b + p + 22);
            if (fmt == 0xFFFE && cl >= 40) fmt = (int)rd16(b + p + 32);
        } else if (!memcmp(b + p, "data", 4)) {
            data = b + p + 8; dlen = cl;
            if ((long)(p + 8 + dlen) > n) dlen = (unsigned)(n - p - 8);
            break;
        }
        p += 8 + cl + (cl & 1);
    }
    if (!data || ch <= 0 || sr <= 0 || !(fmt == 1 || fmt == 3) || bits % 8) { free(b); return -1; }
    {
        int bps = bits / 8, fr = (int)(dlen / (unsigned)(bps * ch)), i;
        float *o = malloc(sizeof(float) * (size_t)fr * ch);
        for (i = 0; i < fr * ch; i++) {
            const unsigned char *q = data + (size_t)i * bps;
            float v = 0;
            if (fmt == 3 && bits == 32) memcpy(&v, q, 4);
            else if (fmt == 3 && bits == 64) { double d; memcpy(&d, q, 8); v = (float)d; }
            else if (bits == 8) v = (q[0] - 128) / 128.0f;
            else if (bits == 16) v = (short)rd16(q) / 32768.0f;
            else if (bits == 24) v = ((int)(q[0] << 8 | q[1] << 16 | (unsigned)q[2] << 24) >> 8) / 8388608.0f;
            else if (bits == 32) v = (int)rd32(q) / 2147483648.0f;
            o[i] = v;
        }
        *inter = o; *frames = fr; *rate = sr; *channels = ch;
    }
    free(b);
    return 0;
}
int xl_wav_write(const char *path, const float *in, int frames)
{
    FILE *fp = fopen(path, "wb");
    unsigned char h[44];
    unsigned dl = (unsigned)frames * 6;
    int i;
    if (!fp) return -1;
    memcpy(h, "RIFF", 4);
    h[4] = (36 + dl) & 255; h[5] = (36 + dl) >> 8 & 255; h[6] = (36 + dl) >> 16 & 255; h[7] = (36 + dl) >> 24;
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = XL_SR & 255; h[25] = XL_SR >> 8 & 255; h[26] = XL_SR >> 16 & 255; h[27] = 0;
    { unsigned br = XL_SR * 6; h[28] = br & 255; h[29] = br >> 8 & 255; h[30] = br >> 16 & 255; h[31] = br >> 24; }
    h[32] = 6; h[33] = 0; h[34] = 24; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = dl & 255; h[41] = dl >> 8 & 255; h[42] = dl >> 16 & 255; h[43] = dl >> 24;
    fwrite(h, 1, 44, fp);
    for (i = 0; i < frames * 2; i++) {
        float v = in[i] > 1 ? 1 : in[i] < -1 ? -1 : in[i];
        int x = (int)lrintf(v * 8388607.0f);
        unsigned char q[3] = {(unsigned char)(x & 255), (unsigned char)(x >> 8 & 255), (unsigned char)(x >> 16 & 255)};
        fwrite(q, 1, 3, fp);
    }
    fclose(fp);
    return 0;
}
xl_sample *xl_sample_load(const char *path, char *err, int errlen)
{
    float *in = 0; int fr = 0, sr = 0, ch = 0;
    xl_sample *s;
    const char *base = strrchr(path, '/');
    if (xl_wav_read(path, &in, &fr, &sr, &ch) != 0 && plat_decode(path, &in, &fr, &sr, &ch) != 0) {
        if (err) snprintf(err, (size_t)errlen, "Can't read %s: not a sound file this build knows", base ? base + 1 : path);
        return 0;
    }
    if (fr > XL_SR * 60 * 10) fr = XL_SR * 60 * 10;   /* (ten minutes at most) */
    s = xl_sample_new(in, fr, sr, ch, base ? base + 1 : path);
    free(in);
    if (s) snprintf(s->path, sizeof s->path, "%s", path);
    return s;
}

/* ---- slicing ---- */
void xl_slice_even(xl_sample *s, int n)
{
    int i;
    if (n < 1) n = 1;
    if (n > XL_NSLICE) n = XL_NSLICE;
    s->nsl = n;
    for (i = 0; i <= n; i++) s->sl[i] = (int)((long long)s->len * i / n);
}
/* onsets: the rise of a fast envelope of the high-passed signal over a slow one */
void xl_slice_auto(xl_sample *s, float sens)
{
    int f, last = -XL_SR, n = 0, mingap = XL_SR / 20;
    float fast = 0, slow = 0, prev = 0, thr = 3.0f - 2.4f * sens / 100.0f, peak = 1e-6f;
    for (f = 0; f < s->len; f++) { float a = fabsf(s->d[2 * f]) + fabsf(s->d[2 * f + 1]); if (a > peak) peak = a; }
    s->sl[n++] = 0;
    for (f = 1; f < s->len && n < XL_NSLICE; f++) {
        float m = (s->d[2 * f] + s->d[2 * f + 1]) * 0.5f, hp = m - prev, a = fabsf(hp);
        prev = m;
        fast += (a - fast) * 0.02f;
        slow += (a - slow) * 0.0005f;
        if (f - last > mingap && fast > slow * thr && fast > peak * 0.004f) {
            int back = f - 256 > s->sl[n - 1] ? f - 256 : f;   /* (start just before the hit) */
            if (back - s->sl[n - 1] > mingap) s->sl[n++] = back;
            last = f;
            slow = fast;
        }
    }
    s->nsl = n;
    s->sl[n] = s->len;
}
void xl_sample_reverse(xl_sample *s)
{
    int i, j, k;
    for (i = 0, j = s->len - 1; i < j; i++, j--)
        for (k = 0; k < 2; k++) { float t = s->d[2 * i + k]; s->d[2 * i + k] = s->d[2 * j + k]; s->d[2 * j + k] = t; }
    for (k = 0; k <= s->nsl; k++) s->sl[k] = s->len - s->sl[k];
    for (i = 0, j = s->nsl; i < j; i++, j--) { int t = s->sl[i]; s->sl[i] = s->sl[j]; s->sl[j] = t; }
    xl_sample_peaks(s);
}
void xl_sample_normalize(xl_sample *s)
{
    float pk = 0, g; int i;
    for (i = 0; i < s->len * 2; i++) if (fabsf(s->d[i]) > pk) pk = fabsf(s->d[i]);
    if (pk < 1e-6f) return;
    g = 0.98f / pk;
    for (i = 0; i < s->len * 2; i++) s->d[i] *= g;
    xl_sample_peaks(s);
}
/* WSOLA: overlap-add windows of 40 ms, each placed where it matches the last best (within 10 ms) */
xl_sample *xl_sample_stretch(const xl_sample *s, int newlen)
{
    const int W = XL_SR / 25, H = W / 2, SR = XL_SR / 100;
    xl_sample *o = calloc(1, sizeof *o);
    float *acc, *wsum, *win;
    double ratio = (double)s->len / newlen;
    int op, i, prevsrc = 0;
    if (!o || newlen < W * 2 || s->len < W * 2) { free(o); return 0; }
    acc = calloc((size_t)(newlen + W) * 2, sizeof(float));
    wsum = calloc((size_t)(newlen + W), sizeof(float));
    win = malloc(sizeof(float) * (size_t)W);
    for (i = 0; i < W; i++) win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / W);
    for (op = 0; op < newlen; op += H) {
        int nominal = (int)(op * ratio), best = nominal, d;
        if (op > 0) {   /* look for the offset that best continues the previous window */
            double bc = -1e30;
            int want = prevsrc + H;
            for (d = -SR; d <= SR; d += 4) {
                int c = nominal + d, k; double cc = 0;
                if (c < 0 || c + W >= s->len || want + W >= s->len) continue;
                for (k = 0; k < W / 2; k += 8)
                    cc += (double)(s->d[2 * (c + k)] + s->d[2 * (c + k) + 1]) * (s->d[2 * (want + k)] + s->d[2 * (want + k) + 1]);
                if (cc > bc) { bc = cc; best = c; }
            }
        }
        if (best < 0) best = 0;
        if (best + W >= s->len) best = s->len - W - 1;
        for (i = 0; i < W && op + i < newlen + W; i++) {
            acc[2 * (op + i)] += s->d[2 * (best + i)] * win[i];
            acc[2 * (op + i) + 1] += s->d[2 * (best + i) + 1] * win[i];
            wsum[op + i] += win[i];
        }
        prevsrc = best;
    }
    o->len = newlen;
    o->d = malloc(sizeof(float) * 2 * (size_t)newlen);
    for (i = 0; i < newlen; i++) {
        float w = wsum[i] > 1e-3f ? wsum[i] : 1;
        o->d[2 * i] = acc[2 * i] / w; o->d[2 * i + 1] = acc[2 * i + 1] / w;
    }
    free(acc); free(wsum); free(win);
    snprintf(o->name, sizeof o->name, "%s", s->name);
    snprintf(o->path, sizeof o->path, "%s", s->path);
    o->nsl = s->nsl;
    for (i = 0; i <= s->nsl; i++) o->sl[i] = (int)((long long)s->sl[i] * newlen / s->len);
    xl_sample_peaks(o);
    return o;
}

/* ---- the demo kit: generated, so a first start makes sound with no files ---- */
static unsigned drng = 1234567u;
static float noise(void) { drng = drng * 1664525u + 1013904223u; return (float)(int)drng / 2147483648.0f; }
static xl_sample *mk(int frames, const char *name) { float *z = calloc((size_t)frames * 2, sizeof(float));
    xl_sample *s = xl_sample_new(z, frames, XL_SR, 2, name); free(z); return s; }
static void put(xl_sample *s, int f, float l, float r) { if (f >= 0 && f < s->len) { s->d[2 * f] += l; s->d[2 * f + 1] += r; } }
static void kick(xl_sample *s, int at, float g)
{
    int i; double ph = 0;
    for (i = 0; i < XL_SR / 3; i++) {
        float t = (float)i / XL_SR, f = 45 + 160 * expf(-t * 30), e = expf(-t * 9);
        ph += 2 * M_PI * f / XL_SR;
        { float v = tanhf(2.5f * (float)sin(ph)) * e * g; put(s, at + i, v, v); }
    }
}
static void snare(xl_sample *s, int at, float g)
{
    int i; float lp = 0;
    for (i = 0; i < XL_SR / 4; i++) {
        float t = (float)i / XL_SR, n = noise(), e = expf(-t * 18);
        lp += (n - lp) * 0.5f;
        { float v = ((n - lp) * 0.8f + 0.4f * sinf(2 * (float)M_PI * 190 * t) * expf(-t * 40)) * e * g; put(s, at + i, v, v * 0.95f); }
    }
}
static void hat(xl_sample *s, int at, float g, float len)
{
    int i; float hp = 0, pv = 0;
    for (i = 0; i < (int)(XL_SR * len); i++) {
        float t = (float)i / XL_SR, n = noise();
        hp = 0.85f * (hp + n - pv); pv = n;
        { float v = hp * expf(-t / len * 5) * g; put(s, at + i, v * 0.9f, v); }
    }
}
static void clang(xl_sample *s, int at, float f0, float g)
{
    int i;
    for (i = 0; i < XL_SR; i++) {
        float t = (float)i / XL_SR, m = sinf(2 * (float)M_PI * f0 * 1.414f * t) * 4 * expf(-t * 6);
        float v = (sinf(2 * (float)M_PI * f0 * t + m) * 0.6f + sinf(2 * (float)M_PI * f0 * 2.76f * t + m * 0.5f) * 0.3f) * expf(-t * 4) * g;
        put(s, at + i, v, sinf(2 * (float)M_PI * f0 * 1.01f * t + m) * 0.6f * expf(-t * 4) * g);
    }
}
static void set_track(int t, xl_sample *s, int eng)
{
    XL.trk[t].smp = s;
    XL.trk[t].p[P_ENGINE] = (float)eng;
}
void xl_demo_kit(void)
{
    int beat = XL_SR / 2, i, t;   /* 120 BPM */
    xl_sample *s;
    /* 1: drums, 2 bars, sliced per hit */
    s = mk(beat * 8, "demo drums");
    for (i = 0; i < 8; i++) {
        if (i % 2 == 0) kick(s, i * beat, 0.9f);
        else snare(s, i * beat, 0.7f);
        hat(s, i * beat + beat / 2, 0.35f, 0.08f);
    }
    xl_slice_auto(s, 60); set_track(0, s, ENG_SHARD);
    /* 2: bass, a saw with a falling filter, root C2 (played as note 60) */
    s = mk(XL_SR, "demo bass");
    { double ph = 0; float lp = 0, lp2 = 0;
      for (i = 0; i < s->len; i++) {
          float tt = (float)i / XL_SR, saw; ph += 65.41 / XL_SR; ph -= floor(ph); saw = (float)(2 * ph - 1);
          { float c = 0.02f + 0.3f * expf(-tt * 6); lp += (saw - lp) * c; lp2 += (lp - lp2) * c; }
          { float v = tanhf(lp2 * 2.5f) * 0.7f * expf(-tt * 1.5f); put(s, i, v, v); }
      } }
    xl_sample_peaks(s); set_track(1, s, ENG_SAMPLE);
    /* 3: metal: 8 clangs, one per slice */
    s = mk(XL_SR * 8, "demo metal");
    { const float f[8] = {220, 311, 415, 523, 147, 392, 277, 659};
      for (i = 0; i < 8; i++) clang(s, i * XL_SR, f[i], 0.5f); }
    xl_slice_even(s, 8); xl_sample_peaks(s); set_track(2, s, ENG_SHARD);
    /* 4: a noise drone */
    s = mk(XL_SR * 6, "demo drone");
    { float a = 0, b = 0;
      for (i = 0; i < s->len; i++) {
          float tt = (float)i / XL_SR, n = noise(), w = 0.002f + 0.01f * (0.5f + 0.5f * sinf(tt * 1.3f));
          a += (n - a) * w; b += (a - b) * w;
          { float v = b * 6 + 0.15f * sinf(2 * (float)M_PI * 55 * tt) + 0.1f * sinf(2 * (float)M_PI * 82.5f * tt);
            float e = tt < 0.5f ? tt * 2 : tt > 5.5f ? (6 - tt) * 2 : 1;
            put(s, i, v * e * 0.5f, (v * 0.9f + b * 2) * e * 0.5f); }
      } }
    xl_sample_peaks(s); set_track(3, s, ENG_SAMPLE);
    /* 5: a machine loop: bursts of filtered noise, 1 bar */
    s = mk(beat * 4, "demo machine");
    for (i = 0; i < 16; i++) if ((0x5A3Bu >> i) & 1) hat(s, i * beat / 4, 0.5f, i % 3 ? 0.03f : 0.12f);
    for (i = 0; i < 4; i++) kick(s, i * beat + beat / 4 * 3, 0.3f);
    xl_slice_even(s, 16); xl_sample_peaks(s); set_track(4, s, ENG_SHARD);
    /* 6: a stab chord */
    s = mk(XL_SR, "demo stab");
    { const float f[3] = {261.6f, 311.1f, 392.0f};
      for (i = 0; i < s->len; i++) {
          float tt = (float)i / XL_SR, v = 0; int k;
          for (k = 0; k < 3; k++) { double ph = f[k] * tt; v += (float)(2 * (ph - floor(ph)) - 1); }
          v = tanhf(v * 0.6f) * expf(-tt * 5) * 0.5f; put(s, i, v, v);
      } }
    xl_sample_peaks(s); set_track(5, s, ENG_SAMPLE);
    /* tracks 7 and 8: empty, for resampling */
    /* patterns */
    {
        xl_track *d = &XL.trk[0], *b = &XL.trk[1], *m = &XL.trk[2], *k = &XL.trk[4];
        int ns = d->smp->nsl;
        for (i = 0; i < 16; i++) {
            if (i % 2 == 0) { d->st[i].on = 1; d->st[i].note = (uint8_t)(36 + (i / 2) % (ns > 0 ? ns : 1)); }
        }
        d->st[14].ratchet = 3;
        { const int bn[16] = {36, 0, 36, 48, 0, 36, 39, 0, 36, 0, 43, 36, 0, 41, 39, 0};
          for (i = 0; i < 16; i++) if (bn[i]) { b->st[i].on = 1; b->st[i].note = (uint8_t)(bn[i] + 24); b->st[i].vel = (uint8_t)(i % 4 ? 90 : 120); } }
        m->st[3].on = 1; m->st[3].note = 36; m->st[11].on = 1; m->st[11].note = 38; m->st[11].prob = 60;
        m->p[P_SENDFX] = 0.5f; m->p[P_LEVEL] = -10;
        for (i = 0; i < 16; i++) { k->st[i].on = 1; k->st[i].note = (uint8_t)(36 + i); k->st[i].vel = 80; }
        k->p[P_MUTE] = 1;
        XL.trk[3].st[0].on = 1; XL.trk[3].st[0].note = 60; XL.trk[3].p[P_SLEN] = 32; XL.trk[3].p[P_LEVEL] = -12;
        XL.trk[3].p[P_REL] = 800;
        XL.trk[1].p[P_TY1] = SH_OVRDRV; XL.trk[1].p[P_DR1] = 0.25f; XL.trk[1].p[P_LV2] = -6;
        XL.trk[5].p[P_MUTE] = 1; XL.trk[5].st[0].on = 1; XL.trk[5].st[6].on = 1; XL.trk[5].st[0].note = 60; XL.trk[5].st[6].note = 63;
        XL.trk[5].p[P_SENDFX] = 0.4f;
        for (t = 0; t < 6; t++) XL.trk[t].p[P_SENDB] = t % 2 ? 0.5f : 0.2f;
    }
}
