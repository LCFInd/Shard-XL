/* SPDX-License-Identifier: GPL-3.0-only */
/* SHARD XL's screens, drawn the way the mockups show: a top bar (tracks, pages, transport), an action bar,
 * the page, and the 8 knobs along the bottom. Immediate mode: each frame draws and records the places that
 * take a click (regions); the next click is matched against them. The UI thread owns everything here. */
#include "xl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <strings.h>

#define C_BG 0xFF000000u
#define C_P 0xFF121212u
#define C_P2 0xFF1C1C1Cu
#define C_L 0xFF333333u
#define C_T 0xFFFFFFFFu
#define C_D 0xFFA6A6A6u
#define C_D2 0xFF8C8C8Cu
#define C_R 0xFFFF3B30u
#define C_RT 0xFF2A0E0Cu
#define C_W 0xFF555555u

enum { PG_WAVE, PG_DECK, PG_MIX, PG_SEQ, PG_SCENES, PG_FILES, PG_SETUP, PG_N };
static const char *const PG_NAME[PG_N] = {"WAVE", "DECK", "MIX", "SEQ", "SCENES", "FILES", "SETUP"};

#define TOP 56
#define ACT 40
#define KY 612
#define CY0 (TOP + ACT)

/* ---- regions ---- */
enum { R_NONE, R_TRK, R_TAB, R_PLAY, R_BPM, R_ACT, R_KNOB, R_WOV, R_WMAIN, R_CHIP, R_WBTN, R_DPLOT, R_DBANDS,
       R_DSLOPE, R_DCARD, R_MFADER, R_MPAN, R_MSEND, R_MMUTE, R_MSOLO, R_MSTRIP, R_XF, R_XCURVE, R_RET, R_SCELL,
       R_SLANE, R_SVEL, R_SPITCH, R_SBTN, R_SCENE, R_SCBTN, R_FILE, R_FSRC, R_FBTN, R_FSCROLL, R_SETBTN, R_HELP };
typedef struct { float x, y, w, h; int type, a, b; } region;
static region RG[1024];
static int NRG;
static void reg(float x, float y, float w, float h, int type, int a, int b)
{
    if (NRG < 1024) { region r = {x, y, w, h, type, a, b}; RG[NRG++] = r; }
}

/* ---- knobs ---- */
enum { KR_NONE, KR_TP, KR_BAND, KR_G, KR_SLSEL, KR_SLV, KR_SPIT, KR_STEP, KR_TLV, KR_XO };
typedef struct { int kind, id; } kref;

#define NUNDO 64
typedef struct { xl_track trk[XL_NTRK]; float g[NG]; } snap;

static struct {
    int scale, page, band, step, slice, help;
    double now, msg_t;
    char msg[200];
    /* dragging */
    int dtype, da, db, dmoved; float dx0, dy0, dv0, dlastx, dlasty;
    double down_t;
    /* the waveform view */
    float view0, zoom;
    /* learning MIDI */
    int learn, learn_i, kch[8], kcc[8], tch[3], tcc[3];
    char midi_last[64];
    /* files */
    char dir[1024], names[600][256];
    int isdir[600], nfiles, fsel, fscroll;
    xl_sample *preview;
    /* keys */
    int octave, keynote[256];
    int killkey[10], repkey;
    /* undo */
    snap *und; int nund, uhead, redo_n;
    double autosave_t;
    float spec_pre[512], spec_post[512];
    int plo, phi, pnote;   /* the pitch lane's range while dragging, and the note being heard */
} U;

static void msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.msg, sizeof U.msg, fmt, ap);
    va_end(ap);
    U.msg_t = U.now;
}
static int sel(void) { return atomic_load(&XL.sel_track); }
static xl_track *ST(void) { return &XL.trk[sel()]; }
static void cmd(int type, int a, int b, int c, void *p) { xl_msg m = {type, a, b, c, p}; xl_q_push(&XL_CMD, m); }

/* ---- undo ---- */
static void undo_push(void)
{
    snap *s;
    if (!U.und) U.und = calloc(NUNDO, sizeof(snap));
    s = &U.und[U.uhead % NUNDO];
    memcpy(s->trk, XL.trk, sizeof XL.trk);
    memcpy(s->g, XL.g, sizeof XL.g);
    U.uhead++;
    if (U.nund < NUNDO) U.nund++;
    U.redo_n = 0;
}
static void restore(const snap *s)
{
    int t;
    for (t = 0; t < XL_NTRK; t++) {   /* (the samples stay: they are not part of undo) */
        memcpy(XL.trk[t].p, s->trk[t].p, sizeof XL.trk[t].p);
        memcpy(XL.trk[t].st, s->trk[t].st, sizeof XL.trk[t].st);
        memcpy(XL.trk[t].slv, s->trk[t].slv, sizeof XL.trk[t].slv);
        memcpy(XL.trk[t].spit, s->trk[t].spit, sizeof XL.trk[t].spit);
        memcpy(XL.trk[t].srev, s->trk[t].srev, sizeof XL.trk[t].srev);
    }
    { float bpm = XL.g[G_BPM]; memcpy(XL.g, s->g, sizeof XL.g); (void)bpm; }
}
static void undo(void)
{
    snap cur;
    if (U.nund == 0) { msg("Nothing to undo"); return; }
    memcpy(cur.trk, XL.trk, sizeof XL.trk); memcpy(cur.g, XL.g, sizeof XL.g);
    U.uhead--; U.nund--;
    { snap tmp = U.und[U.uhead % NUNDO]; U.und[U.uhead % NUNDO] = cur; restore(&tmp); }
    U.redo_n++;
    msg("Undone");
}
static void redo(void)
{
    snap cur;
    if (U.redo_n == 0) { msg("Nothing to redo"); return; }
    memcpy(cur.trk, XL.trk, sizeof XL.trk); memcpy(cur.g, XL.g, sizeof XL.g);
    { snap tmp = U.und[U.uhead % NUNDO]; U.und[U.uhead % NUNDO] = cur; restore(&tmp); }
    U.uhead++; U.nund++; U.redo_n--;
    msg("Redone");
}

/* ---- values and their text ---- */
static const char *const COND_L[] = {"", "ALWAYS", "1 IN 2", "1 IN 3", "1 IN 4", "1 IN 5", "1 IN 6", "1 IN 7", "1 IN 8"};
static xl_pdesc KD_SLSEL = {"SLICE", 1, 64, 1, PF_INT, 0, ""}, KD_SLV = {"S.LEVEL", -60, 12, 0, 0, 0, "dB"},
                KD_SPIT = {"S.PITCH", -24, 24, 0, 0, 0, "st"}, KD_NOTE = {"NOTE", 0, 127, 60, PF_INT, 0, "note"},
                KD_SLNOTE = {"SLICE", 36, 99, 36, PF_INT, 0, "slice"}, KD_VEL = {"VELOCITY", 1, 127, 100, PF_INT, 0, ""},
                KD_PROB = {"CHANCE", 0, 100, 100, PF_INT, 0, "%"}, KD_RAT = {"RATCHET", 1, 4, 1, PF_INT, 0, "x"},
                KD_COND = {"EVERY", 1, 8, 1, PF_ENUM, COND_L, ""}, KD_NONE = {"", 0, 1, 0, 0, 0, ""};
static xl_pdesc KD_TLV[XL_NTRK], KD_XO[3];
static const char *const NOTE_N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static int is_shard(void) { return (int)ST()->p[P_ENGINE] == ENG_SHARD; }
static int nslices(void) { xl_sample *s = ST()->smp; return s ? s->nsl : 1; }
static float kget(kref k, const xl_pdesc **d)
{
    xl_track *t = ST();
    xl_step *s = &t->st[U.step];
    switch (k.kind) {
    case KR_TP: *d = &XL_P[k.id]; return t->p[k.id];
    case KR_BAND: *d = &XL_P[k.id + U.band]; return t->p[k.id + U.band];
    case KR_G: *d = &XL_G[k.id]; return XL.g[k.id];
    case KR_SLSEL: KD_SLSEL.max = (float)nslices(); *d = &KD_SLSEL; return (float)(U.slice + 1);
    case KR_SLV: *d = &KD_SLV; return t->slv[U.slice];
    case KR_SPIT: *d = &KD_SPIT; return t->spit[U.slice];
    case KR_STEP:
        switch (k.id) {
        case 0: *d = is_shard() ? &KD_SLNOTE : &KD_NOTE; return s->note;
        case 1: *d = &KD_VEL; return s->vel;
        case 2: *d = &KD_PROB; return s->prob;
        case 3: *d = &KD_RAT; return s->ratchet;
        default: *d = &KD_COND; return s->cond;
        }
    case KR_TLV: *d = &KD_TLV[k.id]; return XL.trk[k.id].p[P_LEVEL];
    case KR_XO: {
        static const char *const N[3][3] = {{"CROSSOVER"}, {"XLO", "XHI"}, {"XLO", "XMID", "XHI"}};
        int nb = (int)t->p[P_BANDS];
        KD_XO[k.id] = XL_P[P_X1 + k.id];
        KD_XO[k.id].name = N[nb - 2][k.id];
        *d = &KD_XO[k.id];
        return t->p[P_X1 + k.id];
    }
    }
    *d = &KD_NONE;
    return 0;
}
static void apply_manual_morph(void);
static void kset(kref k, float v)
{
    xl_track *t = ST();
    xl_step *s = &t->st[U.step];
    const xl_pdesc *d;
    kget(k, &d);
    v = xl_pval(d, v);
    switch (k.kind) {
    case KR_TP: t->p[k.id] = v; break;
    case KR_BAND: t->p[k.id + U.band] = v; break;
    case KR_G: XL.g[k.id] = v; if (k.id == G_MORPH || k.id == G_SCENEA || k.id == G_SCENEB) apply_manual_morph(); break;
    case KR_SLSEL: U.slice = (int)v - 1; break;
    case KR_SLV: t->slv[U.slice] = v; break;
    case KR_SPIT: t->spit[U.slice] = v; break;
    case KR_STEP:
        switch (k.id) {
        case 0: s->note = (uint8_t)v; break;
        case 1: s->vel = (uint8_t)v; break;
        case 2: s->prob = (uint8_t)v; break;
        case 3: s->ratchet = (uint8_t)v; break;
        default: s->cond = (uint8_t)v; break;
        }
        break;
    case KR_TLV: XL.trk[k.id].p[P_LEVEL] = v; break;
    case KR_XO: t->p[P_X1 + k.id] = v; break;
    }
}
static float knorm(const xl_pdesc *d, float v)
{
    if (d->max <= d->min) return 0;
    if (d->flags & PF_LOG) return logf(v / d->min) / logf(d->max / d->min);
    return (v - d->min) / (d->max - d->min);
}
static float kden(const xl_pdesc *d, float n)
{
    n = n < 0 ? 0 : n > 1 ? 1 : n;
    if (d->flags & PF_LOG) return d->min * powf(d->max / d->min, n);
    return d->min + (d->max - d->min) * n;
}
static void fmt(const xl_pdesc *d, float v, char *b, int n)
{
    const char *u = d->unit;
    if ((d->flags & PF_ENUM) && d->labels) { snprintf(b, (size_t)n, "%s", d->labels[(int)v]); return; }
    if (!strcmp(u, "dB")) { if (v <= -59.5f) snprintf(b, (size_t)n, "%s", d->max > 6.5f ? "KILL" : "OFF"); else snprintf(b, (size_t)n, "%+.1f dB", v); return; }
    if (!strcmp(u, "Hz")) { if (v < 1000) snprintf(b, (size_t)n, "%.0f Hz", v); else snprintf(b, (size_t)n, "%.2f kHz", v / 1000); return; }
    if (!strcmp(u, "%")) { snprintf(b, (size_t)n, "%.0f%%", d->max <= 1.0f ? v * 100 : v); return; }
    if (!strcmp(u, "st")) { snprintf(b, (size_t)n, "%+.1f st", v); return; }
    if (!strcmp(u, "ms")) { if (v < 10) snprintf(b, (size_t)n, "%.1f ms", v); else if (v < 1000) snprintf(b, (size_t)n, "%.0f ms", v); else snprintf(b, (size_t)n, "%.2f s", v / 1000); return; }
    if (!strcmp(u, "note")) { int nn = (int)v; snprintf(b, (size_t)n, "%s%d", NOTE_N[nn % 12], nn / 12 - 2); return; }
    if (!strcmp(u, "slice")) { snprintf(b, (size_t)n, "%d", (int)v - 35); return; }
    if (!strcmp(u, "x")) { snprintf(b, (size_t)n, "%dx", (int)v); return; }
    if (!strcmp(u, "bars")) { snprintf(b, (size_t)n, "%d bar%s", (int)v, v > 1.5f ? "s" : ""); return; }
    if (!strcmp(d->name, "PAN")) { if (fabsf(v) < 0.02f) snprintf(b, (size_t)n, "C"); else snprintf(b, (size_t)n, "%s %.0f", v < 0 ? "L" : "R", fabsf(v) * 100); return; }
    if (!strcmp(d->name, "DJ FILTER")) { if (fabsf(v) < 0.02f) snprintf(b, (size_t)n, "OFF"); else snprintf(b, (size_t)n, "%s %.0f%%", v < 0 ? "LP" : "HP", fabsf(v) * 100); return; }
    if (!strcmp(d->name, "CROSSFADER")) { snprintf(b, (size_t)n, "%.0f%% B", v * 100); return; }
    if (!strcmp(d->name, "SPLIT")) { int nn = (int)v; if (nn == 0) snprintf(b, (size_t)n, "OFF"); else snprintf(b, (size_t)n, "%s%d", NOTE_N[nn % 12], nn / 12 - 2); return; }
    if (d->flags & PF_INT) { snprintf(b, (size_t)n, "%d", (int)v); return; }
    snprintf(b, (size_t)n, "%.1f", v);
}
static void kref_page(int page, kref *k)
{
    int i;
    for (i = 0; i < 8; i++) { k[i].kind = KR_NONE; k[i].id = 0; }
    switch (page) {
    case PG_WAVE:
        k[0] = (kref){KR_TP, P_ENGINE}; k[1] = (kref){KR_TP, P_TUNE}; k[2] = (kref){KR_TP, P_ATK}; k[3] = (kref){KR_TP, P_REL};
        k[4] = (kref){KR_TP, P_SENS}; k[5] = (kref){KR_SLSEL, 0}; k[6] = (kref){KR_SLV, 0}; k[7] = (kref){KR_SPIT, 0};
        break;
    case PG_DECK:
        for (i = 0; i < (int)ST()->p[P_BANDS] - 1; i++) k[i] = (kref){KR_XO, i};
        k[3] = (kref){KR_BAND, P_LV0};
        k[4] = (kref){KR_BAND, P_TY0}; k[5] = (kref){KR_BAND, P_DR0}; k[6] = (kref){KR_BAND, P_FB0}; k[7] = (kref){KR_TP, P_DJ};
        break;
    case PG_MIX: for (i = 0; i < 8; i++) k[i] = (kref){KR_TLV, i}; break;
    case PG_SEQ:
        k[0] = (kref){KR_TP, P_SLEN}; k[1] = (kref){KR_TP, P_SDIV}; k[2] = (kref){KR_G, G_SWING}; k[3] = (kref){KR_STEP, 0};
        k[4] = (kref){KR_STEP, 1}; k[5] = (kref){KR_STEP, 2}; k[6] = (kref){KR_STEP, 3}; k[7] = (kref){KR_STEP, 4};
        break;
    case PG_SCENES:
        k[0] = (kref){KR_G, G_QUANT}; k[1] = (kref){KR_G, G_MORPHBARS}; k[2] = (kref){KR_G, G_SCENEA}; k[3] = (kref){KR_G, G_SCENEB};
        k[4] = (kref){KR_G, G_MORPH}; k[5] = (kref){KR_G, G_XFADE}; k[6] = (kref){KR_G, G_BPM}; k[7] = (kref){KR_G, G_SWING};
        break;
    case PG_FILES:
        k[0] = (kref){KR_G, G_PREVIEW}; k[1] = (kref){KR_TP, P_ENGINE}; k[2] = (kref){KR_TP, P_SENS}; k[3] = (kref){KR_TP, P_TUNE};
        k[4] = (kref){KR_TP, P_LEVEL};
        break;
    case PG_SETUP:
        k[0] = (kref){KR_G, G_BPM}; k[1] = (kref){KR_G, G_SPLIT}; k[2] = (kref){KR_G, G_LOWTRK}; k[3] = (kref){KR_G, G_HITRK};
        k[4] = (kref){KR_G, G_PADMODE}; k[5] = (kref){KR_G, G_MUTATE}; k[6] = (kref){KR_G, G_RSBARS}; k[7] = (kref){KR_G, G_MASTER};
        break;
    }
}

/* ---- actions ---- */
static void play_toggle(void) { if (atomic_load(&XL.playing)) cmd(CMD_STOP, 0, 0, 0, 0); else cmd(CMD_PLAY, 0, 0, 0, 0); }
static void reslice(xl_sample *s, float sens) { if (s) xl_slice_auto(s, sens); U.slice = 0; }
static void set_sample(int t, xl_sample *s) { cmd(CMD_SAMPLE, t, 0, 0, s); }
static void timestamp(char *b, int n)
{
    time_t tt = time(0);
    struct tm *tm = localtime(&tt);
    strftime(b, (size_t)n, "%Y%m%d-%H%M%S", tm);
}
static void capture(void)
{
    int n;
    float *buf = malloc(sizeof(float) * 2 * XL_SR * XL_CAPTURE_SEC);
    char path[1200], ts[32];
    if (!buf) return;
    n = xl_capture_copy(buf, XL_SR * XL_CAPTURE_SEC);
    if (n < XL_SR / 10) { msg("Nothing to capture yet"); free(buf); return; }
    timestamp(ts, sizeof ts);
    snprintf(path, sizeof path, "%s/Captures/capture-%s.wav", XL.home, ts);
    if (xl_wav_write(path, buf, n) == 0) msg("Captured the last %.0f s: Captures/capture-%s.wav", n / (double)XL_SR, ts);
    else msg("Couldn't write %s", path);
    free(buf);
}
static void resample(void)
{
    if (XL.v.resampling) { msg("Already resampling"); return; }
    cmd(CMD_RESAMPLE, -1, sel(), (int)XL.g[G_RSBARS], 0);
    msg("Resampling the master into track %d: %d bar%s, from the next bar", sel() + 1, (int)XL.g[G_RSBARS],
        XL.g[G_RSBARS] > 1.5f ? "s" : "");
}
static float frand(void) { return (float)rand() / RAND_MAX * 2 - 1; }
static void mutate(void)
{
    xl_track *t = ST();
    float a = XL.g[G_MUTATE];
    int b, i, nb = (int)t->p[P_BANDS];
    undo_push();
    for (b = 0; b < nb; b++) {
        float *lv = &t->p[P_LV0 + b];
        if (*lv > -59.5f) *lv = xl_pval(&XL_P[P_LV0 + b], *lv + frand() * a * 12);
        t->p[P_DR0 + b] = xl_pval(&XL_P[P_DR0 + b], t->p[P_DR0 + b] + frand() * a);
        t->p[P_FB0 + b] = xl_pval(&XL_P[P_FB0 + b], t->p[P_FB0 + b] + frand() * a * 0.5f);
        if (fabsf(frand()) < a * 0.5f) t->p[P_TY0 + b] = (float)(rand() % SH_N);
    }
    for (i = P_X1; i <= P_X3; i++) t->p[i] = xl_pval(&XL_P[i], t->p[i] * powf(2.0f, frand() * a * 1.5f));
    if (fabsf(frand()) < a) t->p[P_DJ] = xl_pval(&XL_P[P_DJ], t->p[P_DJ] + frand() * a * 0.4f);
    for (i = 0; i < (int)t->p[P_SLEN]; i++) {
        xl_step *s = &t->st[i];
        if (s->on && is_shard() && fabsf(frand()) < a * 0.5f) s->note = (uint8_t)(36 + rand() % (nslices() > 0 ? nslices() : 1));
        if (fabsf(frand()) < a * 0.12f) { s->on = !s->on; if (s->on && is_shard()) s->note = (uint8_t)(36 + rand() % nslices()); }
    }
    msg("Mutated track %d by %.0f%%  (Cmd+Z undoes it)", sel() + 1, a * 100);
}
static void scene_store(int i)
{
    int t;
    xl_scene *s = &XL.scene[i];
    for (t = 0; t < XL_NTRK; t++) { memcpy(s->p[t], XL.trk[t].p, sizeof s->p[t]); memcpy(s->st[t], XL.trk[t].st, sizeof s->st[t]); }
    s->valid = 1;
    msg("Scene %d stored", i + 1);
}
static void scene_launch(int i, int morph)
{
    if (!XL.scene[i].valid) { scene_store(i); return; }
    undo_push();
    cmd(CMD_SCENE, i, morph ? (int)XL.g[G_MORPHBARS] : 0, 0, 0);
    if (morph && XL.g[G_MORPHBARS] > 0.5f) msg("Scene %d: morphing over %d bars", i + 1, (int)XL.g[G_MORPHBARS]);
}
static void apply_manual_morph(void)
{
    int a = (int)XL.g[G_SCENEA] - 1, b = (int)XL.g[G_SCENEB] - 1, t, i;
    float u = XL.g[G_MORPH];
    if (a < 0 || b < 0 || a >= XL_NSCENE || b >= XL_NSCENE || !XL.scene[a].valid || !XL.scene[b].valid) return;
    for (t = 0; t < XL_NTRK; t++) {
        for (i = 0; i < NP; i++) {
            float x = XL.scene[a].p[t][i], y = XL.scene[b].p[t][i];
            if (XL_P[i].flags & (PF_INT | PF_ENUM)) XL.trk[t].p[i] = u < 0.5f ? x : y;
            else if (XL_P[i].flags & PF_LOG) XL.trk[t].p[i] = x * powf(y / x, u);
            else XL.trk[t].p[i] = x + (y - x) * u;
        }
        memcpy(XL.trk[t].st, u < 0.5f ? XL.scene[a].st[t] : XL.scene[b].st[t], sizeof XL.trk[t].st);
    }
}
static void save_project(int quiet)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/Projects/autosave.sxl", XL.home);
    if (xl_project_save(p) == 0) { if (!quiet) msg("Saved: Projects/autosave.sxl"); }
    else msg("Couldn't save %s", p);
}

/* ---- files ---- */
static int is_audio(const char *n)
{
    const char *e = strrchr(n, '.');
    static const char *const ext[] = {".wav", ".wave", ".aif", ".aiff", ".aifc", ".mp3", ".m4a", ".flac", ".caf", ".ogg", ".mp4", 0};
    int i;
    if (!e) return 0;
    for (i = 0; ext[i]; i++) {
        const char *a = e, *b = ext[i];
        while (*a && *b && (*a | 32) == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}
static int name_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
static void files_open(const char *dir)
{
    static char nm[600][256];
    static int isd[600];
    int n, i, k = 0;
    snprintf(U.dir, sizeof U.dir, "%s", dir);
    n = plat_list_dir(dir, nm, isd, 600);
    if (n < 0) { U.nfiles = 0; msg("Can't open %s", dir); return; }
    for (i = 0; i < n; i++) if (isd[i] && nm[i][0] != '.') { memcpy(U.names[k], nm[i], 256); U.isdir[k++] = 1; }
    qsort(U.names, (size_t)k, 256, name_cmp);
    {
        int k0 = k;
        for (i = 0; i < n; i++) if (!isd[i] && nm[i][0] != '.' && is_audio(nm[i])) { memcpy(U.names[k], nm[i], 256); U.isdir[k++] = 0; }
        qsort(U.names[k0], (size_t)(k - k0), 256, name_cmp);
    }
    U.nfiles = k; U.fsel = -1; U.fscroll = 0;
}
static void file_path(int i, char *out, int n) { snprintf(out, (size_t)n, "%s/%s", U.dir, U.names[i]); }
static void load_to(int t, const char *path, int autoslice)
{
    char err[300];
    xl_sample *s = xl_sample_load(path, err, sizeof err);
    if (!s) { msg("%s", err); return; }
    if (autoslice || (int)XL.trk[t].p[P_ENGINE] == ENG_SHARD) xl_slice_auto(s, XL.trk[t].p[P_SENS]);
    if (autoslice) XL.trk[t].p[P_ENGINE] = ENG_SHARD;
    set_sample(t, s);
    U.slice = 0;
    msg("Loaded %s into track %d (%d slice%s)", s->name, t + 1, s->nsl, s->nsl == 1 ? "" : "s");
}
static void file_click(int i, int clicks)
{
    char p[1400];
    if (i < 0 || i >= U.nfiles) return;
    file_path(i, p, sizeof p);
    if (U.isdir[i]) { files_open(p); return; }
    U.fsel = i;
    if (clicks >= 2) { load_to(sel(), p, 0); return; }
    {
        char err[300];
        xl_sample *s = xl_sample_load(p, err, sizeof err);
        if (!s) { msg("%s", err); return; }
        U.preview = s;
        cmd(CMD_PREVIEW, 0, 0, 0, s);
    }
}

/* ---- MIDI from the engine: learning, knobs, transport ---- */
static kref PAGE_K[8];
static void midi_ev(int st, int d1, int d2)
{
    int ty = st & 0xF0, ch = st & 15, i;
    snprintf(U.midi_last, sizeof U.midi_last, "%02X %d %d (channel %d)", st, d1, d2, ch + 1);
    if (st == 0xFA) { if (!atomic_load(&XL.playing)) cmd(CMD_PLAY, 0, 0, 0, 0); return; }
    if (st == 0xFC) { cmd(CMD_STOP, 0, 0, 0, 0); return; }
    if (st == 0xF5) { atomic_store(&XL.rec, !atomic_load(&XL.rec)); return; }
    if (ty != 0xB0) return;
    if (U.learn == 1) {
        for (i = 0; i < U.learn_i; i++) if (U.kch[i] == ch && U.kcc[i] == d1) return;
        U.kch[U.learn_i] = ch; U.kcc[U.learn_i] = d1; U.learn_i++;
        if (U.learn_i >= 8) { U.learn = 0; msg("Knobs learned: your controller's 8 knobs now turn the 8 on screen"); }
        else msg("Knob %d learned. Now turn the knob for knob %d", U.learn_i, U.learn_i + 1);
        return;
    }
    if (U.learn == 2) {
        if (d2 == 0) return;
        U.tch[U.learn_i] = ch; U.tcc[U.learn_i] = d1; U.learn_i++;
        if (U.learn_i >= 3) { U.learn = 0; msg("Transport learned"); }
        else msg("Now press the button for %s", U.learn_i == 1 ? "STOP" : "RECORD");
        return;
    }
    for (i = 0; i < 3; i++)
        if (U.tcc[i] >= 0 && U.tch[i] == ch && U.tcc[i] == d1 && d2 > 0) {
            if (i == 0) cmd(CMD_PLAY, 0, 0, 0, 0);
            else if (i == 1) cmd(CMD_STOP, 0, 0, 0, 0);
            else atomic_store(&XL.rec, !atomic_load(&XL.rec));
            return;
        }
    for (i = 0; i < 8; i++)
        if (U.kcc[i] >= 0 && U.kch[i] == ch && U.kcc[i] == d1 && PAGE_K[i].kind != KR_NONE) {
            const xl_pdesc *d;
            kget(PAGE_K[i], &d);
            kset(PAGE_K[i], kden(d, d2 / 127.0f));
            return;
        }
}
static void events(void)
{
    xl_msg m;
    while (xl_q_pop(&XL_EV, &m)) {
        switch (m.type) {
        case EV_FREE: if (m.ptr == U.preview) U.preview = 0; xl_sample_free((xl_sample *)m.ptr); break;
        case EV_MIDI: midi_ev(m.a, m.b, m.c); break;
        case EV_SCENE: msg("Scene %d playing", m.a + 1); break;
        case EV_RESAMPLED: {
            int n, t = m.a, bars = (int)XL.g[G_RSBARS];
            const float *b = xl_resample_buf(&n);
            char name[64];
            xl_sample *s;
            snprintf(name, sizeof name, "resample %d", t + 1);
            s = xl_sample_new(b, n, XL_SR, 2, name);
            if (!s) break;
            {   /* keep it too */
                char p[1200], ts[32];
                timestamp(ts, sizeof ts);
                snprintf(p, sizeof p, "%s/Captures/resample-%s.wav", XL.home, ts);
                if (xl_wav_write(p, s->d, s->len) == 0) snprintf(s->path, sizeof s->path, "%s", p);
            }
            xl_slice_even(s, bars * 4 > XL_NSLICE ? XL_NSLICE : bars * 4);
            XL.trk[t].p[P_ENGINE] = ENG_SHARD;
            set_sample(t, s);
            msg("Resampled into track %d: %d slices, one per beat", t + 1, s->nsl);
            break;
        }
        }
    }
}

/* ---- drawing helpers ---- */
static void button(float x, float y, float w, float h, const char *label, int on, int type, int a, int b)
{
    d_rect(x, y, w, h, on ? C_R : C_P2, 6);
    if (!on) d_frame(x, y, w, h, C_L, 1, 6);
    d_text(x + w / 2, y + h / 2 + 5, F13, on ? C_BG : C_T, label, 1);
    reg(x, y, w, h, type, a, b);
}
static void knob_ring(float cx, float cy, float n, uint32_t col)
{
    float a0 = 0.75f * (float)M_PI, sweep = 1.5f * (float)M_PI;
    d_arc(cx, cy, 20, 5, a0, a0 + sweep, C_L);
    if (n > 0.001f) d_arc(cx, cy, 20, 5, a0, a0 + sweep * (n > 1 ? 1 : n), col);
}

static void draw_top(void)
{
    int i, pg;
    float x;
    char b[64];
    d_rect(0, TOP - 1, 1280, 1, C_L, 0);
    d_text(20, 36, F18, C_T, "SHARD", 0);
    d_text(20 + d_textw(F18, "SHARD "), 36, F18, C_D, "XL", 0);
    for (i = 0; i < XL_NTRK; i++) {
        int s = i == sel();
        x = 130 + i * 44;
        d_rect(x, 10, 40, 36, s ? C_R : C_P, 6);
        d_frame(x, 10, 40, 36, s ? C_R : C_L, 1, 6);
        snprintf(b, sizeof b, "%d", i + 1);
        d_text(x + 20, 30, F15, s ? C_BG : C_T, b, 1);
        {   /* a meter under the number */
            float m = XL.v.meter[i], w = m > 1 ? 32 : m * 32;
            d_rect(x + 4, 38, 32, 3, s ? 0x66000000u : C_L, 1.5f);
            d_rect(x + 4, 38, w, 3, s ? C_BG : C_T, 1.5f);
        }
        reg(x, 10, 40, 36, R_TRK, i, 0);
    }
    x = 494;
    for (pg = 0; pg < PG_N; pg++) {
        float w = d_textw(F15, PG_NAME[pg]) + 24;
        if (pg == U.page) d_rect(x, 10, w, 36, C_P2, 6);
        d_text(x + w / 2, 33, F15, pg == U.page ? C_T : C_D, PG_NAME[pg], 1);
        reg(x, 10, w, 36, R_TAB, pg, 0);
        x += w + 2;
    }
    {   /* transport */
        int pl = atomic_load(&XL.playing);
        double beat = XL.v.beat;
        float rx = 1060;
        d_rect(rx, 12, 32, 32, pl ? C_T : C_P2, 6);
        if (pl) d_rect(rx + 10, 22, 12, 12, C_BG, 1);
        else { d_line(rx + 12, 21, rx + 12, 35, C_T, 2); d_line(rx + 12, 21, rx + 23, 28, C_T, 2); d_line(rx + 12, 35, rx + 23, 28, C_T, 2); }
        reg(rx, 12, 32, 32, R_PLAY, 0, 0);
        snprintf(b, sizeof b, "%.1f", XL.g[G_BPM]);
        d_text(rx + 44, 34, F15, C_T, b, 0);
        reg(rx + 40, 12, 52, 32, R_BPM, 0, 0);
        snprintf(b, sizeof b, "%d.%d", (int)(beat / 4) + 1, (int)fmod(beat, 4) + 1);
        d_text(rx + 100, 34, F15, C_D, b, 0);
        snprintf(b, sizeof b, "CPU %.0f%%", XL.v.cpu * 100);
        d_text(1260, 34, F13, C_D, b, 2);
    }
}
static const char *const ACT_L[] = {"REC", "CAPTURE 60 S", "RESAMPLE", "MUTATE", "UNDO", "REDO", "SAVE", "HELP"};
static void draw_actions(void)
{
    int i;
    float x = 20;
    d_rect(0, CY0 - 1, 1280, 1, C_L, 0);
    for (i = 0; i < 8; i++) {
        float w = d_textw(F13, ACT_L[i]) + 26;
        int on = (i == 0 && atomic_load(&XL.rec)) || (i == 2 && XL.v.resampling);
        button(x, TOP + 6, w, 28, ACT_L[i], on, R_ACT, i, 0);
        x += w + 8;
    }
    if (U.now - U.msg_t < 6 && U.msg[0]) d_text_fit(x + 12, TOP + 25, F13, C_D, U.msg, 1260 - x - 12);
    else if (XL.v.scene_queued >= 0) { char b[64]; snprintf(b, sizeof b, "Scene %d starts on the next bar", XL.v.scene_queued + 1); d_text(x + 12, TOP + 25, F13, C_D, b, 0); }
}
static void draw_knobs(const char *hint)
{
    int i;
    d_rect(0, KY, 1280, 108, C_P, 0);
    d_rect(0, KY, 1280, 1, C_L, 0);
    d_text(20, KY + 18, F11, C_D2, hint, 0);
    kref_page(U.page, PAGE_K);
    for (i = 0; i < 8; i++) {
        float x = i * 160, cy = KY + 60;
        const xl_pdesc *d;
        float v;
        char b[48];
        if (i) d_rect(x, KY + 32, 1, 64, C_L, 0);
        if (PAGE_K[i].kind == KR_NONE) continue;
        v = kget(PAGE_K[i], &d);
        knob_ring(x + 40, cy + 2, knorm(d, v), PAGE_K[i].kind == KR_BAND && PAGE_K[i].id == P_LV0 ? C_R : (PAGE_K[i].kind == KR_TLV && PAGE_K[i].id != sel() ? C_T : C_R));
        d_text(x + 70, cy - 6, F11, C_D, d->name, 0);
        fmt(d, v, b, sizeof b);
        d_text_fit(x + 70, cy + 16, F15, C_T, b, 86);
        reg(x, KY + 28, 160, 80, R_KNOB, i, 0);
    }
}

/* ---- WAVE ---- */
static const char *const WB_L[] = {"AUTO SLICE", "EVEN x8", "EVEN x16", "REVERSE", "NORMALIZE", "STRETCH TO TEMPO", "REVERSE SLICE"};
static float wave_x0 = 20, wave_w = 1240;
static void draw_wave_area(xl_sample *s, float x, float y, float w, float h, double v0, double span, uint32_t col)
{
    int X, X0 = (int)(x * U.scale), X1 = (int)((x + w) * U.scale);
    double fpp = span * s->len / (X1 - X0);
    float mid = y + h / 2, amp = h / 2 * 0.92f;
    for (X = X0; X < X1; X++) {
        double f0 = v0 * s->len + (X - X0) * fpp, f1 = f0 + fpp;
        float lo = 0, hi = 0;
        int a = (int)f0, b = (int)f1 + 1, k;
        if (a < 0) a = 0;
        if (b > s->len) b = s->len;
        if (a >= b) continue;
        if (b - a > XL_PKBLK * 2) {
            int pa = a / XL_PKBLK, pb = b / XL_PKBLK;
            for (k = pa; k < pb && k < s->npk; k++) { if (s->pk[2 * k] < lo) lo = s->pk[2 * k]; if (s->pk[2 * k + 1] > hi) hi = s->pk[2 * k + 1]; }
        } else
            for (k = a; k < b; k++) { float m = (s->d[2 * k] + s->d[2 * k + 1]) * 0.5f; if (m < lo) lo = m; if (m > hi) hi = m; }
        if (lo < -1) lo = -1;
        if (hi > 1) hi = 1;
        d_vspan_dev(X, mid - hi * amp, mid - lo * amp, col);
    }
}
static float wave_fx(xl_sample *s, int frame) { return wave_x0 + (float)(((double)frame / s->len - U.view0) * U.zoom) * wave_w; }
static int wave_frame(xl_sample *s, float x) { double f = (U.view0 + (x - wave_x0) / wave_w / U.zoom) * s->len; return f < 0 ? 0 : f > s->len ? s->len : (int)f; }
static void page_wave(void)
{
    xl_track *t = ST();
    xl_sample *s = t->smp;
    char b[200];
    int i;
    float y = CY0 + 18;
    if (!s) {
        d_text(640, 300, F18, C_T, "This track has no sample", 1);
        d_text(640, 330, F13, C_D, "Open FILES to load one, drop a file on the window, or RESAMPLE into it.", 1);
        draw_knobs("Track has no sample");
        return;
    }
    if (U.slice >= s->nsl) U.slice = s->nsl - 1;
    if (U.slice < 0) U.slice = 0;
    snprintf(b, sizeof b, "%s", s->name);
    d_text_fit(20, y + 10, F18, C_T, b, 360);
    snprintf(b, sizeof b, "%.2f s · %d slice%s · %s · track %d", s->len / (double)XL_SR, s->nsl, s->nsl == 1 ? "" : "s",
             (int)t->p[P_ENGINE] == ENG_SHARD ? "SHARD" : "SAMPLE", sel() + 1);
    d_text(400, y + 10, F13, C_D, b, 0);
    {
        float x = 1260;
        for (i = 6; i >= 0; i--) {
            float w = d_textw(F11, WB_L[i]) + 20;
            x -= w;
            d_rect(x, y - 8, w, 26, C_P2, 5); d_frame(x, y - 8, w, 26, C_L, 1, 5);
            d_text(x + w / 2, y + 9, F11, C_T, WB_L[i], 1);
            reg(x, y - 8, w, 26, R_WBTN, i, 0);
            x -= 6;
        }
    }
    y += 30;
    /* overview */
    d_rect(20, y, 1240, 56, C_P, 6);
    draw_wave_area(s, 20, y, 1240, 56, 0, 1, C_W);
    d_rect(20 + U.view0 * 1240, y + 1, 1240 / U.zoom, 54, 0x30FF3B30u, 3);
    d_frame(20 + U.view0 * 1240, y + 1, 1240 / U.zoom, 54, C_R, 2, 3);
    reg(20, y, 1240, 56, R_WOV, 0, 0);
    y += 66;
    /* the zoomed view */
    {
        float h = 300;
        double span = 1.0 / U.zoom;
        d_rect(20, y, 1240, h, C_P, 6);
        if (U.slice < s->nsl) {
            float a = wave_fx(s, s->sl[U.slice]), e = wave_fx(s, s->sl[U.slice + 1]);
            if (a < 20) a = 20;
            if (e > 1260) e = 1260;
            if (e > a) d_rect(a, y, e - a, h, 0x22FF3B30u, 0);
        }
        d_rect(20, y + h / 2, 1240, 1, C_L, 0);
        draw_wave_area(s, 20, y + 24, 1240, h - 32, U.view0, span, C_R);
        for (i = 0; i < s->nsl; i++) {
            float x = wave_fx(s, s->sl[i]);
            int sl = i == U.slice;
            if (x < 20 || x > 1260) continue;
            d_rect(x, y, sl ? 2 : 1, h, sl ? C_R : 0xFF5A5A5Au, 0);
            d_rect(x, y, 28, 20, sl ? C_R : C_P2, 3);
            snprintf(b, sizeof b, "%d", i + 1);
            d_text(x + 14, y + 15, F11, sl ? C_BG : C_T, b, 1);
        }
        for (i = 0; i < XL_NVOICE; i++) {
            int p = XL.v.vpos[sel()][i];
            float x;
            if (p < 0) continue;
            x = wave_fx(s, p);
            if (x >= 20 && x <= 1260) { d_rect(x, y, 1.5f, h, C_T, 0); d_circle(x + 0.75f, y + h - 6, 4, C_T); }
        }
        reg(20, y, 1240, h, R_WMAIN, 0, 0);
        y += h + 10;
    }
    /* slice chips */
    {
        int first = U.slice / 16 * 16, n = s->nsl - first > 16 ? 16 : s->nsl - first;
        for (i = 0; i < n; i++) {
            int k = first + i, on = k == U.slice;
            float x = 20 + i * 78.5f;
            d_rect(x, y, 72, 50, on ? C_RT : C_P2, 6);
            d_frame(x, y, 72, 50, on ? C_R : C_L, 1, 6);
            snprintf(b, sizeof b, "%d%s", k + 1, t->srev[k] ? " · REV" : "");
            d_text(x + 8, y + 21, F13, C_T, b, 0);
            snprintf(b, sizeof b, "%+.0f st %+.0f dB", t->spit[k], t->slv[k] < -59.5f ? -60.0f : t->slv[k]);
            d_text(x + 8, y + 40, F11, C_D, b, 0);
            reg(x, y, 72, 50, R_CHIP, k, 0);
        }
        if (s->nsl > 16) { snprintf(b, sizeof b, "slices %d-%d of %d", first + 1, first + n, s->nsl); d_text(1260, y + 66, F11, C_D2, b, 2); }
    }
    snprintf(b, sizeof b, "Slice %d · click a marker to select, drag to move, double-click to add, Alt-click to delete · scroll to zoom", U.slice + 1);
    draw_knobs(b);
}

/* ---- DECK: the spectrum ---- */
static void fft(float *re, float *im, int n)
{
    int i, j, k, m;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (m = 2; m <= n; m <<= 1) {
        float ang = -2 * (float)M_PI / m;
        for (k = 0; k < n; k += m)
            for (j = 0; j < m / 2; j++) {
                float c = cosf(ang * j), s = sinf(ang * j);
                float tr = re[k + j + m / 2] * c - im[k + j + m / 2] * s, ti = re[k + j + m / 2] * s + im[k + j + m / 2] * c;
                re[k + j + m / 2] = re[k + j] - tr; im[k + j + m / 2] = im[k + j] - ti;
                re[k + j] += tr; im[k + j] += ti;
            }
    }
}
#define PW 840
#define PH 410
static float dfx(float hz) { return (log10f(hz) - log10f(20)) / 3 * PW; }
static float dfhz(float x) { return 20 * powf(10, x / PW * 3); }
static float ddb(float db) { return (12 - db) / 84 * PH; }
static float dydb(float y) { return 12 - y / PH * 84; }
static void spectrum(float *out, const float *x, int n)
{
    static float re[2048], im[2048];
    int i;
    for (i = 0; i < n; i++) { re[i] = x[i] * (0.5f - 0.5f * cosf(2 * (float)M_PI * i / n)); im[i] = 0; }
    fft(re, im, n);
    for (i = 0; i < 512; i++) {   /* 512 log-spaced points from 20 Hz to 20 kHz */
        float hz = 20 * powf(1000, i / 511.0f), bin = hz * n / XL_SR, mag;
        int b0 = (int)bin, b1 = (int)(bin * 1.012f) + 1, k;
        mag = 0;
        if (b1 > n / 2) b1 = n / 2;
        if (b1 <= b0) b1 = b0 + 1;
        for (k = b0; k < b1; k++) mag += re[k] * re[k] + im[k] * im[k];
        /* the power per 1/3 octave or so (a pink signal reads level), shown relative to a full-scale sine */
        mag = 10 * log10f(mag * 16.0f / ((float)n * n) * (hz * 0.23f / ((float)XL_SR / n)) / (b1 - b0) + 1e-12f) + 30;   /* (+30: music sits near the band lines) */
        out[i] += (mag - out[i]) * (mag > out[i] ? 0.5f : 0.15f);
    }
}
static void page_deck(void)
{
    xl_track *t = ST();
    int nb = (int)t->p[P_BANDS], i;
    float px = 20, py = CY0 + 40, bx[5];
    char b[200];
    static const char *const BN[4][4] = {{"LOW", "HIGH"}, {"LOW", "MID", "HIGH"}, {"LOW", "LOW MID", "HIGH MID", "HIGH"}};
    const char *const *names = BN[nb - 2];
    if (U.band >= nb) U.band = nb - 1;
    snprintf(b, sizeof b, "Deck · track %d", sel() + 1);
    d_text(20, CY0 + 26, F18, C_T, b, 0);
    d_text(20 + d_textw(F18, b) + 16, CY0 + 26, F13, C_D, "grey: into the deck · white: out of it · red: each band's level", 0);
    {
        float pre[2048], post[2048];
        xl_spec_read(pre, post, 2048);
        spectrum(U.spec_pre, pre, 2048);
        spectrum(U.spec_post, post, 2048);
    }
    d_rect(px, py, PW, PH, C_P, 8);
    {   /* grid */
        static const float GF[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
        static const char *const GL[] = {"50", "100", "200", "500", "1k", "2k", "5k", "10k"};
        for (i = 0; i < 8; i++) { d_rect(px + dfx(GF[i]), py, 1, PH, C_L, 0); d_text(px + dfx(GF[i]) + 4, py + PH - 6, F11, C_D2, GL[i], 0); }
        for (i = 0; i < 3; i++) { float db = i == 0 ? 0 : i == 1 ? -24 : -48; d_rect(px, py + ddb(db), PW, 1, C_L, 0);
            snprintf(b, sizeof b, "%.0f dB", db); d_text(px + 4, py + ddb(db) - 4, F11, C_D2, b, 0); }
    }
    bx[0] = 0; bx[nb] = PW;
    for (i = 1; i < nb; i++) bx[i] = dfx(t->p[P_X1 + i - 1]);
    for (i = 0; i < nb; i++) if (i == U.band) d_rect(px + bx[i], py, bx[i + 1] - bx[i], PH, 0x18FF3B30u, 0);
    {   /* the spectra */
        int X, X0 = (int)(px * U.scale), X1 = (int)((px + PW) * U.scale);
        float prevy = 0;
        for (X = X0; X < X1; X++) {
            float fr = (float)(X - X0) / (X1 - X0), k = fr * 511, v = U.spec_pre[(int)k], y = ddb(v);
            if (y < 0) y = 0;
            if (y > PH) y = PH;
            d_vspan_dev(X, py + y, py + PH, 0x60555555u);
        }
        for (i = 0; i < 511; i++) {
            float x0 = px + PW * i / 511.0f, x1 = px + PW * (i + 1) / 511.0f;
            float y0 = ddb(U.spec_post[i]), y1 = ddb(U.spec_post[i + 1]);
            y0 = y0 < 0 ? 0 : y0 > PH ? PH : y0; y1 = y1 < 0 ? 0 : y1 > PH ? PH : y1;
            if (i) d_line(x0, py + y0, x1, py + y1, C_T, 1.5f);
            prevy = y1;
        }
        (void)prevy;
    }
    for (i = 0; i < nb; i++) {   /* the band levels */
        float lv = t->p[P_LV0 + i], a = px + bx[i], e = px + bx[i + 1];
        int s = i == U.band;
        if (lv <= -59.5f) {
            d_rect(a, py, e - a, PH, 0x99000000u, 0);
            d_line(a + 6, py + PH - 24, e - 6, py + PH - 24, C_R, 3);
            d_text((a + e) / 2, py + PH - 32, F13, C_R, "KILL", 1);
        } else {
            float y = py + ddb(lv);
            d_line(a + 6, y, e - 6, y, C_R, s ? 4 : 3);
            d_rect((a + e) / 2 - 36, y - 26, 72, 20, s ? C_R : C_BG, 3);
            d_frame((a + e) / 2 - 36, y - 26, 72, 20, C_R, 1, 3);
            snprintf(b, sizeof b, "%+.1f dB", lv);
            d_text((a + e) / 2, y - 11, F11, s ? C_BG : C_R, b, 1);
        }
        d_text((a + e) / 2, py + 22, F13, s ? C_T : C_D, names[i], 1);
    }
    for (i = 1; i < nb; i++) {   /* the crossovers */
        float x = px + bx[i], hz = t->p[P_X1 + i - 1];
        d_rect(x - 1, py + 30, 2, PH - 30, C_T, 0);
        d_circle(x, py + PH - 44, 7, C_T); d_circle(x, py + PH - 44, 5, C_BG);
        if (hz < 1000) snprintf(b, sizeof b, "%.0f Hz", hz); else snprintf(b, sizeof b, "%.1f kHz", hz / 1000);
        d_rect(x + 9, py + PH - 54, d_textw(F11, b) + 8, 18, C_BG, 3);
        d_text(x + 13, py + PH - 41, F11, C_T, b, 0);
    }
    reg(px, py, PW, PH, R_DPLOT, 0, 0);
    {   /* the right panel */
        float rx = 880, ry = CY0 + 40, w = 380;
        d_text(rx, ry + 10, F11, C_D, "BANDS", 0);
        for (i = 0; i < 3; i++) { snprintf(b, sizeof b, "%d", i + 2);
            d_rect(rx + i * 128, ry + 18, 124, 32, nb == i + 2 ? C_T : C_P2, 6); d_text(rx + i * 128 + 62, ry + 39, F15, nb == i + 2 ? C_BG : C_T, b, 1);
            reg(rx + i * 128, ry + 18, 124, 32, R_DBANDS, i + 2, 0); }
        d_text(rx, ry + 72, F11, C_D, "SLOPE", 0);
        for (i = 0; i < 2; i++) { int on = (int)t->p[P_SLOPE] == i;
            d_rect(rx + i * 192, ry + 80, 188, 32, on ? C_T : C_P2, 6); d_text(rx + i * 192 + 94, ry + 101, F15, on ? C_BG : C_T, i ? "24 dB/oct" : "12 dB/oct", 1);
            reg(rx + i * 192, ry + 80, 188, 32, R_DSLOPE, i, 0); }
        for (i = 0; i < nb; i++) {
            float cy = ry + 128 + i * 64, lv = t->p[P_LV0 + i];
            int s = i == U.band, ty = (int)t->p[P_TY0 + i], k;
            d_rect(rx, cy, w, 56, s ? C_RT : C_P, 8);
            d_frame(rx, cy, w, 56, s ? C_R : C_L, 1, 8);
            d_rect(rx + 10, cy + 8, 40, 40, C_BG, 4);
            for (k = 0; k < 40; k++) {   /* the transfer curve */
                float x0 = -1 + k / 20.0f, x1 = -1 + (k + 1) / 20.0f, d = t->p[P_DR0 + i];
                float y0 = satf(shape_curve(ty, d, x0), 1.1f), y1 = satf(shape_curve(ty, d, x1), 1.1f);
                d_line(rx + 30 + x0 * 18, cy + 28 - y0 * 16, rx + 30 + x1 * 18, cy + 28 - y1 * 16, s ? C_R : C_D, 1.5f);
            }
            d_text(rx + 62, cy + 24, F15, C_T, names[i], 0);
            snprintf(b, sizeof b, "%s · DRIVE %.0f%% · FB %.0f%%", XL_P[P_TY0].labels[ty], t->p[P_DR0 + i] * 100, t->p[P_FB0 + i] * 100);
            d_text(rx + 62, cy + 44, F11, C_D, b, 0);
            if (lv <= -59.5f) snprintf(b, sizeof b, "KILL"); else snprintf(b, sizeof b, "%+.1f dB", lv);
            d_text(rx + w - 12, cy + 34, F15, C_R, b, 2);
            reg(rx, cy, w, 56, R_DCARD, i, 0);
        }
        { const xl_pdesc *d = &XL_P[P_DJ]; char v[32]; fmt(d, t->p[P_DJ], v, sizeof v); snprintf(b, sizeof b, "DJ FILTER  %s", v); }
        d_text(rx, ry + PH - 4, F13, C_D, b, 0);
    }
    snprintf(b, sizeof b, "%s band · drag a crossover or a red line, or turn the knobs · Shift+1-8 held kills a track's LOW", names[U.band]);
    draw_knobs(b);
}

/* ---- MIX ---- */
static void page_mix(void)
{
    int t;
    char b[64];
    for (t = 0; t < XL_NTRK; t++) {
        xl_track *tk = &XL.trk[t];
        float x = 20 + t * 112, y = CY0 + 16, w = 104, fh = 230, lv = (tk->p[P_LEVEL] + 60) / 66, m = XL.v.meter[t];
        int s = t == sel(), k;
        d_rect(x, y, w, 484, C_P, 8);
        d_frame(x, y, w, 484, s ? C_R : C_L, 1, 8);
        d_rect(x + 10, y + 10, w - 20, 4, s ? C_R : C_T, 2);
        snprintf(b, sizeof b, "%d %s", t + 1, (int)tk->p[P_ENGINE] == ENG_SHARD ? "SHARD" : "SAMPLE");
        d_text(x + 10, y + 34, F15, C_T, b, 0);
        d_text_fit(x + 10, y + 52, F11, C_D, tk->smp ? tk->smp->name : "empty", w - 20);
        reg(x, y, w, 60, R_MSTRIP, t, 0);
        {   /* meter + fader */
            float fy = y + 66, mh = m > 1 ? fh : m * fh, cx = x + 66;
            int mute = !((tk->p[P_SOLO] >= 0.5f) || tk->p[P_MUTE] < 0.5f);
            d_rect(x + 14, fy, 8, fh, C_L, 2); d_rect(x + 26, fy, 8, fh, C_L, 2);
            d_rect(x + 14, fy + fh - mh, 8, mh, mute ? C_W : (s ? C_R : C_T), 2);
            d_rect(x + 26, fy + fh - mh * 0.94f, 8, mh * 0.94f, mute ? C_W : (s ? C_R : C_T), 2);
            d_rect(cx - 2, fy, 4, fh, C_L, 2);
            d_rect(cx - 14, fy + fh * (1 - lv) - 9, 28, 18, C_T, 3);
            d_rect(cx - 10, fy + fh * (1 - lv), 20, 2, C_BG, 0);
            reg(x + 40, fy - 10, 56, fh + 20, R_MFADER, t, 0);
            fmt(&XL_P[P_LEVEL], tk->p[P_LEVEL], b, sizeof b);
            d_text(x + w / 2, fy + fh + 24, F13, C_T, b, 1);
        }
        {   /* pan */
            float py = y + 342, pv = tk->p[P_PAN];
            d_text(x + 10, py, F11, C_D, "PAN", 0);
            d_rect(x + 40, py - 6, 52, 4, C_L, 2);
            d_rect(x + 40 + 26 + (pv < 0 ? pv * 26 : 0), py - 7, fabsf(pv) * 26 + 1, 6, s ? C_R : C_T, 2);
            reg(x + 36, py - 14, 60, 20, R_MPAN, t, 0);
        }
        for (k = 0; k < 3; k++) {   /* sends */
            float sy = y + 366 + k * 22, v = tk->p[P_SENDA + k];
            static const char *const SN[3] = {"A", "B", "FX"};
            d_text(x + 10, sy + 4, F11, C_D, SN[k], 0);
            d_rect(x + 34, sy - 2, 58, 6, C_L, 3);
            d_rect(x + 34, sy - 2, 58 * v, 6, s ? C_R : C_T, 3);
            reg(x + 30, sy - 9, 66, 20, R_MSEND, t, k);
        }
        {   /* mute / solo */
            int mu = tk->p[P_MUTE] >= 0.5f, so = tk->p[P_SOLO] >= 0.5f;
            float by = y + 436;
            d_rect(x + 10, by, 40, 30, mu ? C_R : C_P2, 5); d_text(x + 30, by + 20, F13, mu ? C_BG : C_T, "M", 1);
            d_rect(x + 54, by, 40, 30, so ? C_T : C_P2, 5); d_text(x + 74, by + 20, F13, so ? C_BG : C_T, "S", 1);
            reg(x + 10, by, 40, 30, R_MMUTE, t, 0); reg(x + 54, by, 40, 30, R_MSOLO, t, 0);
        }
    }
    {   /* buses, crossfader, FX loop, master */
        float rx = 926, ry = CY0 + 16, w = 334, xv = XL.g[G_XFADE];
        int k;
        for (k = 0; k < 2; k++) {
            float bx = rx + k * 170, rv = XL.g[G_RETA + k];
            d_rect(bx, ry, 164, 104, C_P, 8); d_frame(bx, ry, 164, 104, C_L, 1, 8);
            d_text(bx + 12, ry + 24, F15, k ? C_R : C_T, k ? "BUS B" : "BUS A", 0);
            fmt(&XL_G[G_RETA], rv, b, sizeof b);
            d_text(bx + 12, ry + 48, F11, C_D, "return to the master", 0);
            d_text(bx + 12, ry + 70, F15, C_T, b, 0);
            d_rect(bx + 12, ry + 86, 140, 6, C_L, 3); d_rect(bx + 12, ry + 86, 140 * (rv + 60) / 66, 6, k ? C_R : C_T, 3);
            reg(bx, ry + 60, 164, 50, R_RET, k, 0);
        }
        ry += 114;
        d_rect(rx, ry, w, 124, C_P, 8); d_frame(rx, ry, w, 124, C_L, 1, 8);
        d_text(rx + 12, ry + 24, F15, C_T, "Crossfader", 0);
        d_rect(rx + 20, ry + 56, w - 40, 6, C_L, 3);
        d_rect(rx + 20, ry + 56, (w - 40) * xv, 6, C_R, 3);
        d_rect(rx + 20 + (w - 40) * xv - 11, ry + 42, 22, 34, C_T, 4);
        reg(rx + 10, ry + 36, w - 20, 46, R_XF, 0, 0);
        d_text(rx + 20, ry + 96, F11, C_D, "A", 0); d_text(rx + w - 20, ry + 96, F11, C_D, "B", 2);
        for (k = 0; k < 3; k++) {
            static const char *const CL[3] = {"SMOOTH", "DJ", "CUT"};
            int on = (int)XL.g[G_XCURVE] == k;
            d_rect(rx + 60 + k * 72, ry + 84, 68, 24, on ? C_T : C_P2, 4);
            d_text(rx + 60 + k * 72 + 34, ry + 101, F11, on ? C_BG : C_T, CL[k], 1);
            reg(rx + 60 + k * 72, ry + 84, 68, 24, R_XCURVE, k, 0);
        }
        ry += 134;
        d_rect(rx, ry, w, 100, C_P, 8); d_frame(rx, ry, w, 100, C_L, 1, 8);
        d_text(rx + 12, ry + 24, F15, C_T, "FX loop", 0);
        d_text(rx + 12, ry + 46, F11, C_D, "send FX → delay (stands in for outside effects)", 0);
        fmt(&XL_G[G_FXRET], XL.g[G_FXRET], b, sizeof b);
        d_text(rx + 12, ry + 76, F15, C_T, b, 0);
        d_rect(rx + 120, ry + 70, 200, 6, C_L, 3); d_rect(rx + 120, ry + 70, 200 * (XL.g[G_FXRET] + 60) / 66, 6, C_T, 3);
        reg(rx, ry + 56, w, 40, R_RET, 2, 0);
        ry += 110;
        d_rect(rx, ry, w, 106, C_P, 8); d_frame(rx, ry, w, 106, C_L, 1, 8);
        d_text(rx + 12, ry + 24, F15, C_T, "Master", 0);
        for (k = 0; k < 2; k++) {
            float mv = XL.v.mmeter[k] > 1 ? 1 : XL.v.mmeter[k];
            d_rect(rx + 12, ry + 44 + k * 14, w - 24, 8, C_L, 2);
            d_rect(rx + 12, ry + 44 + k * 14, (w - 24) * mv, 8, mv > 0.94f ? C_R : C_T, 2);
        }
        fmt(&XL_G[G_MASTER], XL.g[G_MASTER], b, sizeof b);
        d_text(rx + 12, ry + 96, F13, C_D, b, 0);
        reg(rx, ry + 76, w, 34, R_RET, 3, 0);
    }
    draw_knobs("Knobs: the 8 faders · drag faders, pans, sends and returns · M mutes, S solos");
}

/* ---- SEQ ---- */
static const char *const SB_L[] = {"EUCLID -", "EUCLID +", "SHIFT ←", "SHIFT →", "RANDOM", "CLEAR"};
static void euclid(xl_track *t, int k)
{
    int n = (int)t->p[P_SLEN], i, on = 0;
    for (i = 0; i < n; i++) on += t->st[i].on;
    k = on + k;
    if (k < 0) k = 0;
    if (k > n) k = n;
    undo_push();
    for (i = 0; i < n; i++) {
        int was = t->st[i].on;
        t->st[i].on = (uint8_t)(((i * k) % n) < k);
        if (t->st[i].on && !was) t->st[i].note = (uint8_t)((int)t->p[P_ENGINE] == ENG_SHARD ? 36 + i % (t->smp ? t->smp->nsl : 1) : 60);
    }
    msg("%d of %d steps, spread evenly", k, n);
}
/* what a step plays, as a cell shows it: the slice number, or the note's name */
static void step_label(const xl_track *t, const xl_step *st, char *b, int n)
{
    if ((int)t->p[P_ENGINE] == ENG_SHARD) snprintf(b, (size_t)n, "%d", st->note - 35);
    else snprintf(b, (size_t)n, "%s", NOTE_N[st->note % 12]);
}
/* the pitch lane's range: the slices, or the notes in use (at least an octave) */
static void seq_range(const xl_track *t, int *lo, int *hi)
{
    int s, len = (int)t->p[P_SLEN];
    if ((int)t->p[P_ENGINE] == ENG_SHARD) {
        int n = t->smp && t->smp->nsl > 0 ? t->smp->nsl : 1;
        *lo = 36; *hi = 36 + (n > 1 ? n - 1 : 1);
        return;
    }
    *lo = 127; *hi = 0;
    for (s = 0; s < len; s++) if (t->st[s].on) { if (t->st[s].note < *lo) *lo = t->st[s].note; if (t->st[s].note > *hi) *hi = t->st[s].note; }
    if (*lo > *hi) { *lo = 54; *hi = 66; }
    *lo -= 3; *hi += 3;
    while (*hi - *lo < 12) { (*lo)--; (*hi)++; }
    if (*lo < 0) *lo = 0;
    if (*hi > 127) *hi = 127;
}
static float seq_y(int v, int lo, int hi, float py, float ph) { return py + 10 + (float)(hi - v) / (hi - lo) * (ph - 20); }
static int seq_v(float y, int lo, int hi, float py, float ph)
{
    int v = hi - (int)floorf((y - py - 10) / (ph - 20) * (hi - lo) + 0.5f);
    return v < lo ? lo : v > hi ? hi : v;
}
#define SEQ_PY (CY0 + 34 + 8 * 34 + 8)
#define SEQ_PH 112
static void seq_pitch(float y, int first, int mods)
{
    xl_track *t = ST();
    xl_step *st = &t->st[U.step];
    int v;
    if (first) {
        undo_push();
        seq_range(t, &U.plo, &U.phi);
        U.pnote = -1;
        if (mods & MOD_ALT) { st->on = 0; return; }
    } else if (!st->on) return;
    v = seq_v(y, U.plo, U.phi, SEQ_PY, SEQ_PH);
    if (!st->on) { st->on = 1; if (st->prob == 0) st->prob = 100; if (st->vel == 0) st->vel = 100; }
    st->note = (uint8_t)v;
    if (v != U.pnote) {   /* hear it */
        if (U.pnote >= 0) cmd(CMD_NOTE_OFF, sel(), U.pnote, 0, 0);
        cmd(CMD_NOTE_ON, sel(), v, st->vel, 0);
        U.pnote = v;
    }
}
static void page_seq(void)
{
    int t, s;
    char b[200];
    float y0 = CY0 + 34;
    double beat = XL.v.beat;
    int pl = atomic_load(&XL.playing);
    for (s = 0; s < 4; s++) { snprintf(b, sizeof b, "%d", s * 16 + 1); d_text(158 + s * (16 * 16 + 6), y0 - 8, F11, C_D2, b, 0); }
    for (t = 0; t < XL_NTRK; t++) {
        xl_track *tk = &XL.trk[t];
        float y = y0 + t * 34;
        int len = (int)tk->p[P_SLEN], cur = XL.v.step[t];
        if (t == sel()) d_rect(14, y - 2, 1252, 32, C_RT, 6);
        snprintf(b, sizeof b, "%d", t + 1);
        d_text(22, y + 20, F15, t == sel() ? C_R : C_T, b, 0);
        d_text_fit(40, y + 20, F13, C_D, tk->smp ? tk->smp->name : "empty", 104);
        reg(14, y - 2, 138, 32, R_SLANE, t, 0);
        for (s = 0; s < XL_NSTEP; s++) {
            float x = 158 + s * 16 + (s / 16) * 6;
            xl_step *st = &tk->st[s];
            uint32_t c;
            if (s >= len) c = 0xFF0A0A0Au;
            else if (st->on) c = t == sel() ? C_R : C_T;
            else c = (s / 4) % 2 ? C_P : C_P2;
            d_rect(x, y + 2, 14, 24, c, 3);
            if (s < len && st->on) {   /* what it plays, and its marks */
                step_label(tk, st, b, sizeof b);
                d_text(x + 7, y + 18, strlen(b) > 1 ? F9 : F11, C_BG, b, 1);
                if (st->prob < 100) d_rect(x + 3, y + 4, 8, 2, C_BG, 1);
                if (st->ratchet > 1) { int r; for (r = 0; r < st->ratchet; r++) d_rect(x + 2 + r * 3, y + 22, 2, 3, C_BG, 0); }
                if (st->cond > 1) d_circle(x + 11.5f, y + 6, 1.8f, C_BG);
            }
            if (pl && s == cur) d_frame(x, y + 2, 14, 24, C_T, 2, 3);
            if (t == sel() && s == U.step) d_frame(x - 1, y + 1, 16, 26, C_R, 1, 3);
            if (s < len) reg(x, y + 2, 15, 24, R_SLANE + 100, t, s);
        }
    }
    {   /* the selected track: what each step plays (drag it up or down), and its velocity */
        xl_track *tk = ST();
        float py = y0 + 8 * 34 + 8, ph = 112, vy = py + ph + 6, vh = 34;
        int len = (int)tk->p[P_SLEN], shard = (int)tk->p[P_ENGINE] == ENG_SHARD, lo, hi, v;
        seq_range(tk, &lo, &hi);
        snprintf(b, sizeof b, "Track %d", sel() + 1);
        d_text(22, py + 18, F15, C_T, b, 0);
        d_text(22, py + 38, F11, C_D, shard ? "slice per step" : "note per step", 0);
        d_text(22, py + 54, F11, C_D2, "drag up or down", 0);
        d_text(22, py + 68, F11, C_D2, "Alt-click clears", 0);
        d_text(22, vy + 22, F11, C_D, "velocity", 0);
        d_rect(156, py, 1048, ph, C_P, 6);
        {
            int every = (int)ceilf(13.0f / ((ph - 20) / (float)(hi - lo)));   /* labels at least 13 px apart */
            if (every < 1) every = 1;
            for (v = lo; v <= hi; v++) {   /* a guide per slice, or per C */
            float yc = seq_y(v, lo, hi, py, ph);
            if (shard ? (v - lo) % every == 0 : v % 12 == 0) {
                d_rect(156, yc, 1048, 1, C_P2, 0);
                if (shard) snprintf(b, sizeof b, "%d", v - 35); else snprintf(b, sizeof b, "C%d", v / 12 - 2);
                d_text(1210, yc + 4, F11, C_D2, b, 0);
            }
            }
        }
        for (s = 0; s < len; s++) {
            float x = 158 + s * 16 + (s / 16) * 6;
            xl_step *st = &tk->st[s];
            if (st->on) {
                float yc = seq_y(st->note < lo ? lo : st->note > hi ? hi : st->note, lo, hi, py, ph);
                float vv = st->vel / 127.0f * vh;
                d_rect(x, yc - 8, 14, 16, s == U.step ? C_R : C_T, 3);
                step_label(tk, st, b, sizeof b);
                d_text(x + 7, yc + 4, strlen(b) > 1 ? F9 : F11, C_BG, b, 1);
                d_rect(x, vy + vh - vv, 14, vv, s == U.step ? C_R : C_D, 2);
            } else d_rect(x + 6, vy + vh - 2, 2, 2, C_L, 0);
            reg(x - 1, py, 16, ph, R_SPITCH, s, 0);
            reg(x - 1, vy, 16, vh, R_SVEL, s, 0);
        }
        {
            float x = 158, y = vy + vh + 6;
            int i;
            for (i = 0; i < 6; i++) {
                float w = d_textw(F11, SB_L[i]) + 22;
                button(x, y, w, 26, SB_L[i], 0, R_SBTN, i, 0);
                x += w + 8;
            }
            d_text(x + 8, y + 18, F11, C_D2, "bar at the top of a step: chance below 100% · ticks: ratchets · dot: plays 1 in N passes", 0);
        }
    }
    (void)beat;
    snprintf(b, sizeof b, "Step %d of track %d · click a step to turn it on or off, Shift-click to select it · drag a step in the lane below to change what it plays", U.step + 1, sel() + 1);
    draw_knobs(b);
}

/* ---- SCENES ---- */
static void page_scenes(void)
{
    int i;
    char b[96];
    float y0 = CY0 + 20;
    for (i = 0; i < XL_NSCENE; i++) {
        float x = 20 + (i % 4) * 214, y = y0 + (i / 4) * 196, w = 206, h = 186;
        xl_scene *s = &XL.scene[i];
        int playing = XL.v.scene_playing == i, queued = XL.v.scene_queued == i;
        d_rect(x, y, w, h, playing ? C_RT : C_P, 10);
        d_frame(x, y, w, h, playing ? C_R : queued ? C_T : C_L, playing || queued ? 2 : 1, 10);
        snprintf(b, sizeof b, "%d", i + 1);
        d_text(x + 18, y + 48, F24, s->valid ? C_T : C_W, b, 0);
        if (!s->valid) d_text(x + 18, y + 80, F13, C_D2, "empty · click to store", 0);
        else {
            int t, act = 0;
            for (t = 0; t < XL_NTRK; t++) { int k, on = 0; for (k = 0; k < XL_NSTEP; k++) on += s->st[t][k].on; act += on > 0 && s->p[t][P_MUTE] < 0.5f; }
            d_text(x + 18, y + 80, F13, C_D, playing ? (XL.v.morph_t >= 0 ? "morphing" : "playing") : queued ? "next bar" : "stored", 0);
            snprintf(b, sizeof b, "%d track%s playing", act, act == 1 ? "" : "s");
            d_text(x + 18, y + 100, F11, C_D2, b, 0);
            for (t = 0; t < XL_NTRK; t++) {   /* a little picture of each track's pattern */
                int k;
                for (k = 0; k < 16; k++) if (s->st[t][k].on) d_rect(x + 18 + k * 10.5f, y + 116 + t * 7, 8, 4, playing ? C_R : C_D, 1);
                    else d_rect(x + 18 + k * 10.5f, y + 116 + t * 7, 8, 4, C_P2, 1);
            }
            if (playing && XL.v.morph_t >= 0) { d_rect(x + 18, y + h - 10, (w - 36) * XL.v.morph_t, 4, C_R, 2); }
        }
        reg(x, y, w, h, R_SCENE, i, 0);
    }
    {
        float rx = 896, ry = y0, w = 364;
        int a = (int)XL.g[G_SCENEA], bb = (int)XL.g[G_SCENEB];
        d_rect(rx, ry, w, 382, C_P, 10); d_frame(rx, ry, w, 382, C_L, 1, 10);
        d_text(rx + 18, ry + 32, F18, C_T, "Morph by hand", 0);
        d_text(rx + 18, ry + 56, F11, C_D, "knobs 3-5: two scenes and the blend between them", 0);
        snprintf(b, sizeof b, "%d", a); d_text(rx + 30, ry + 130, F24, C_T, b, 1);
        snprintf(b, sizeof b, "%d", bb); d_text(rx + w - 30, ry + 130, F24, C_T, b, 1);
        d_rect(rx + 60, ry + 120, w - 120, 6, C_L, 3);
        d_rect(rx + 60, ry + 120, (w - 120) * XL.g[G_MORPH], 6, C_R, 3);
        d_rect(rx + 60 + (w - 120) * XL.g[G_MORPH] - 10, ry + 106, 20, 34, C_T, 4);
        d_text(rx + 18, ry + 200, F13, C_T, "Click a scene: it starts on the next bar.", 0);
        d_text(rx + 18, ry + 222, F13, C_T, "Hold it: it morphs in over MORPH BARS.", 0);
        d_text(rx + 18, ry + 244, F13, C_T, "Shift-click: store over it · Alt-click: clear it.", 0);
        d_text(rx + 18, ry + 266, F13, C_T, "The Axiom's pads launch scenes 1-8.", 0);
        snprintf(b, sizeof b, "Launch: %s · morph: %d bars", XL_G[G_QUANT].labels[(int)XL.g[G_QUANT]], (int)XL.g[G_MORPHBARS]);
        d_text(rx + 18, ry + 300, F13, C_D, b, 0);
    }
    draw_knobs("A scene holds every track's sound and pattern · samples stay as they are");
}

/* ---- FILES ---- */
static const char *const FB_L[] = {"LOAD TO TRACK", "LOAD AND AUTO SLICE", "UP A FOLDER", "CHOOSE A FOLDER…"};
static void page_files(void)
{
    int i;
    char b[1400];
    float y0 = CY0 + 18;
    static const char *const SRC[] = {"Samples", "Captures", "Demo", "Home"};
    d_text(20, y0 + 10, F11, C_D, "SOURCES", 0);
    for (i = 0; i < 4; i++) {
        float y = y0 + 22 + i * 40;
        d_rect(20, y, 200, 34, C_P, 6);
        d_text(34, y + 23, F15, C_T, SRC[i], 0);
        reg(20, y, 200, 34, R_FSRC, i, 0);
    }
    d_text_fit(240, y0 + 12, F15, C_T, U.dir, 640);
    {
        int rows = 11, k;
        for (k = 0; k < rows; k++) {
            int idx = U.fscroll + k;
            float y = y0 + 26 + k * 42;
            if (idx >= U.nfiles) break;
            d_rect(240, y, 640, 38, idx == U.fsel ? C_RT : (k % 2 ? C_BG : C_P), 6);
            if (idx == U.fsel) d_frame(240, y, 640, 38, C_R, 1, 6);
            snprintf(b, sizeof b, "%s%s", U.names[idx], U.isdir[idx] ? "/" : "");
            d_text_fit(256, y + 25, F15, U.isdir[idx] ? C_D : C_T, b, 600);
            reg(240, y, 640, 38, R_FILE, idx, 0);
        }
        if (U.nfiles == 0) d_text(256, y0 + 60, F13, C_D, "No sound files here. Drop files on the window, or choose another folder.", 0);
        if (U.nfiles > rows) { snprintf(b, sizeof b, "%d-%d of %d · scroll", U.fscroll + 1, U.fscroll + rows < U.nfiles ? U.fscroll + rows : U.nfiles, U.nfiles); d_text(880, y0 + 12, F11, C_D2, b, 2); }
        reg(240, y0 + 26, 640, rows * 42, R_FSCROLL, 0, 0);
    }
    {
        float rx = 904, ry = y0;
        xl_sample *p = U.preview;
        if (p) {
            d_text_fit(rx, ry + 12, F18, C_T, p->name, 356);
            d_rect(rx, ry + 26, 356, 140, C_P, 6);
            draw_wave_area(p, rx, ry + 26, 356, 140, 0, 1, C_R);
            snprintf(b, sizeof b, "%.2f s · converted to 48 kHz stereo", p->len / (double)XL_SR);
            d_text(rx, ry + 188, F13, C_D, b, 0);
        } else d_text(rx, ry + 40, F13, C_D, "Click a file to hear it; double-click loads it.", 0);
        for (i = 0; i < 4; i++) {
            if (i == 0) snprintf(b, sizeof b, "LOAD TO TRACK %d", sel() + 1); else snprintf(b, sizeof b, "%s", FB_L[i]);
            button(rx, ry + 220 + i * 44, 356, 36, b, i == 0 && U.fsel >= 0, R_FBTN, i, 0);
        }
    }
    draw_knobs("Files load into the selected track · drop files on a track button to load them there");
}

/* ---- SETUP ---- */
static const char *const SETB_L[] = {"LEARN KNOBS", "LEARN TRANSPORT", "FORGET MIDI MAPPING", "PANIC: ALL NOTES OFF"};
static void page_setup(void)
{
    int i;
    char b[1200];
    float y0 = CY0 + 24, x2 = 660;
    d_text(20, y0 + 10, F18, C_T, "MIDI", 0);
    d_text(20, y0 + 36, F13, C_D, "Every MIDI controller plugged into the Mac is heard. The M-Audio Axiom:", 0);
    d_text(20, y0 + 58, F13, C_T, "· pads (channel 10) launch scenes 1-8; with PADS on SLICES, or Shift held, they play slices", 0);
    d_text(20, y0 + 78, F13, C_T, "· keys play the selected track, or split: below SPLIT to LOW TRACK, above to HIGH TRACK", 0);
    d_text(20, y0 + 98, F13, C_T, "· knobs: press LEARN KNOBS, then turn the Axiom's knobs 1 to 8 in order", 0);
    d_text(20, y0 + 118, F13, C_T, "· transport: PLAY, STOP, REC by MIDI start/stop or MMC, or press LEARN TRANSPORT", 0);
    for (i = 0; i < 4; i++) button(20 + (i % 2) * 300, y0 + 140 + (i / 2) * 44, 290, 36, SETB_L[i], (i == 0 && U.learn == 1) || (i == 1 && U.learn == 2), R_SETBTN, i, 0);
    snprintf(b, sizeof b, "Last MIDI message: %s", U.midi_last[0] ? U.midi_last : "none yet");
    d_text(20, y0 + 254, F13, C_D, b, 0);
    {
        int n = 0;
        for (i = 0; i < 8; i++) n += U.kcc[i] >= 0;
        snprintf(b, sizeof b, "Knobs mapped: %d of 8 · transport buttons mapped: %s", n, U.tcc[0] >= 0 ? "yes" : "no");
        d_text(20, y0 + 276, F13, C_D, b, 0);
    }
    d_text(x2, y0 + 10, F18, C_T, "Keys on the Mac", 0);
    {
        static const char *const KL[] = {
            "Space: play / stop          1-8: select a track",
            "A W S E D F T G Y H U J K: play the track (Z X: octave)",
            "Tab: next page              Cmd+1-7: pages",
            "Shift+1-8, held: kill that track's LOW band",
            "Alt+1-4, held: beat repeat 1/4, 1/8, 1/16, 1/32",
            "Cmd+R: record   Cmd+K: capture   Cmd+M: mutate",
            "Cmd+Z / Cmd+Shift+Z: undo / redo   Cmd+S: save",
            "?: all of this, any time"};
        for (i = 0; i < 8; i++) d_text(x2, y0 + 40 + i * 22, F13, C_T, KL[i], 0);
    }
    d_text(x2, y0 + 240, F18, C_T, "Files", 0);
    snprintf(b, sizeof b, "%s", XL.home);
    d_text_fit(x2, y0 + 266, F13, C_D, b, 600);
    d_text(x2, y0 + 288, F13, C_D, "Samples/, Captures/ (captures and resamples), Projects/ (the autosave)", 0);
    snprintf(b, sizeof b, "SHARD XL %s · Mac test build · 48 kHz · CPU %.0f%%", XL_VERSION, XL.v.cpu * 100);
    d_text(20, KY - 16, F11, C_D2, b, 0);
    draw_knobs("BPM, the keyboard split, pads, mutate amount, resample length, master level");
}

static void draw_help(void)
{
    static const char *const H[] = {
        "Space  play / stop", "1-8  select a track", "Tab, Cmd+1-7  pages", "A W S E D F T G Y H U J K  play notes (Z X octave)",
        "Shift+1-8 held  kill a track's LOW band", "Alt+1-4 held  beat repeat 1/4 · 1/8 · 1/16 · 1/32",
        "Cmd+R  record into the pattern", "Cmd+K  capture the last 60 s to a file", "Cmd+M  mutate the selected track",
        "Cmd+Z, Cmd+Shift+Z  undo, redo", "Cmd+S  save (also every minute, and on quit)",
        "Drop sound files on the window, or on a track button", "Knobs: drag up and down, scroll, double-click resets",
        "Esc or click: close this"};
    int i;
    d_rect(0, 0, 1280, 720, 0xCC000000u, 0);
    d_rect(300, 90, 680, 540, C_P, 12); d_frame(300, 90, 680, 540, C_L, 1, 12);
    d_text(330, 130, F24, C_T, "SHARD XL · keys and moves", 0);
    for (i = 0; i < 14; i++) d_text(330, 172 + i * 31, F15, i == 13 ? C_D : C_T, H[i], 0);
    reg(0, 0, 1280, 720, R_HELP, 0, 0);
}

/* ---- the frame ---- */
void xl_ui_frame(uint32_t *fb, int w, int h, double now)
{
    U.now = now;
    events();
    atomic_store(&XL.spec_track, sel());
    d_begin(fb, w, h, U.scale);
    d_clear(C_BG);
    NRG = 0;
    draw_top();
    draw_actions();
    switch (U.page) {
    case PG_WAVE: page_wave(); break;
    case PG_DECK: page_deck(); break;
    case PG_MIX: page_mix(); break;
    case PG_SEQ: page_seq(); break;
    case PG_SCENES: page_scenes(); break;
    case PG_FILES: page_files(); break;
    case PG_SETUP: page_setup(); break;
    }
    if (U.help) draw_help();
    if (now - U.autosave_t > 60) { U.autosave_t = now; save_project(1); }
}

/* ---- input ---- */
static region *hit(float x, float y)
{
    int i;
    for (i = NRG - 1; i >= 0; i--)
        if (x >= RG[i].x && x < RG[i].x + RG[i].w && y >= RG[i].y && y < RG[i].y + RG[i].h) return &RG[i];
    return 0;
}
static void wave_down(float x, float y, int mods, int clicks)
{
    xl_sample *s = ST()->smp;
    int i, best = -1;
    float bd = 7;
    (void)y;
    if (!s) return;
    for (i = 0; i < s->nsl; i++) { float d = fabsf(wave_fx(s, s->sl[i]) - x); if (d < bd) { bd = d; best = i; } }
    if (best >= 0) {
        if ((mods & MOD_ALT) && best > 0) {   /* delete it */
            for (i = best; i < s->nsl; i++) s->sl[i] = s->sl[i + 1];
            s->nsl--;
            if (U.slice >= s->nsl) U.slice = s->nsl - 1;
            msg("Slice marker removed: %d slices", s->nsl);
            U.dtype = R_NONE;
            return;
        }
        U.slice = best < s->nsl ? best : s->nsl - 1;
        U.da = best;   /* drag that marker */
        return;
    }
    U.da = -1;
    {
        int f = wave_frame(s, x);
        if (clicks >= 2 && s->nsl < XL_NSLICE) {   /* add a marker */
            int k = 0;
            while (k < s->nsl && s->sl[k + 1] <= f) k++;
            if (f - s->sl[k] > 64 && s->sl[k + 1] - f > 64) {
                for (i = s->nsl + 1; i > k + 1; i--) s->sl[i] = s->sl[i - 1];
                s->sl[k + 1] = f; s->nsl++;
                U.slice = k + 1;
                msg("Slice marker added: %d slices", s->nsl);
            }
            return;
        }
        for (i = 0; i < s->nsl; i++) if (f >= s->sl[i] && f < s->sl[i + 1]) U.slice = i;
        cmd(CMD_NOTE_ON, sel(), (int)ST()->p[P_ENGINE] == ENG_SHARD ? 36 + U.slice : 60, 110, 0);
        U.db = 1;
    }
}
static void deck_drag(float x, float y, int first)
{
    xl_track *t = ST();
    int nb = (int)t->p[P_BANDS], i;
    float lx = x - 20, ly = y - (CY0 + 40);
    if (first) {
        U.da = -1; U.db = -1;
        for (i = 1; i < nb; i++) if (fabsf(dfx(t->p[P_X1 + i - 1]) - lx) < 10) U.da = i - 1;
        if (U.da < 0) {
            float bx0 = 0, bx1;
            for (i = 0; i < nb; i++) {
                bx1 = i == nb - 1 ? PW : dfx(t->p[P_X1 + i]);
                if (lx >= bx0 && lx < bx1) { U.band = i; U.db = i; }
                bx0 = bx1;
            }
        }
        undo_push();
    }
    if (U.da >= 0) t->p[P_X1 + U.da] = xl_pval(&XL_P[P_X1], dfhz(lx));
    else if (U.db >= 0) {
        float db = dydb(ly);
        if (ly > PH - 30) db = -60;
        t->p[P_LV0 + U.db] = xl_pval(&XL_P[P_LV0], db);
    }
}
void xl_ui_mouse(int type, float x, float y, int mods, int clicks)
{
    region *r;
    if (type == MS_DOWN) {
        if (U.help) { U.help = 0; return; }
        r = hit(x, y);
        U.dtype = r ? r->type : R_NONE;
        U.dmoved = 0; U.dx0 = x; U.dy0 = y; U.dlastx = x; U.dlasty = y; U.down_t = U.now;
        if (!r) return;
        U.da = r->a; U.db = r->b;
        switch (r->type) {
        case R_TRK: atomic_store(&XL.sel_track, r->a); U.step = U.step; break;
        case R_TAB: U.page = r->a; if (U.page == PG_FILES && !U.dir[0]) { char p[1100]; snprintf(p, sizeof p, "%s/Samples", XL.home); files_open(p); } break;
        case R_PLAY: play_toggle(); break;
        case R_BPM: U.dv0 = XL.g[G_BPM]; undo_push(); break;
        case R_ACT:
            switch (r->a) {
            case 0: atomic_store(&XL.rec, !atomic_load(&XL.rec)); msg(atomic_load(&XL.rec) ? "Recording: notes you play while the pattern runs land on its steps" : "Recording off"); break;
            case 1: capture(); break;
            case 2: resample(); break;
            case 3: mutate(); break;
            case 4: undo(); break;
            case 5: redo(); break;
            case 6: save_project(0); break;
            case 7: U.help = 1; break;
            }
            break;
        case R_KNOB: {
            const xl_pdesc *d;
            float v;
            kref_page(U.page, PAGE_K);
            v = kget(PAGE_K[r->a], &d);
            undo_push();
            if (clicks >= 2) { kset(PAGE_K[r->a], d->def); U.dtype = R_NONE; }
            U.dv0 = knorm(d, v);
            break;
        }
        case R_WOV: if (ST()->smp) { U.view0 = (x - 20) / 1240 - 0.5f / U.zoom; if (U.view0 < 0) U.view0 = 0; if (U.view0 > 1 - 1 / U.zoom) U.view0 = 1 - 1 / U.zoom; } break;
        case R_WMAIN: U.db = 0; wave_down(x, y, mods, clicks); break;
        case R_CHIP: U.slice = r->a; cmd(CMD_NOTE_ON, sel(), 36 + r->a, 110, 0); U.db = 1; break;
        case R_WBTN: {
            xl_sample *s = ST()->smp;
            if (!s) break;
            switch (r->a) {
            case 0: reslice(s, ST()->p[P_SENS]); ST()->p[P_ENGINE] = ENG_SHARD; msg("%d slices found (SENS %d)", s->nsl, (int)ST()->p[P_SENS]); break;
            case 1: xl_slice_even(s, 8); ST()->p[P_ENGINE] = ENG_SHARD; U.slice = 0; break;
            case 2: xl_slice_even(s, 16); ST()->p[P_ENGINE] = ENG_SHARD; U.slice = 0; break;
            case 3: xl_sample_reverse(s); msg("Reversed"); break;
            case 4: xl_sample_normalize(s); msg("Normalized"); break;
            case 5: {
                double sec = s->len / (double)XL_SR, beats = sec * XL.g[G_BPM] / 60;
                int bars = (int)floor(beats / 4 + 0.5), nl;
                xl_sample *o;
                if (bars < 1) bars = 1;
                nl = (int)(bars * 4 * 60.0 / XL.g[G_BPM] * XL_SR);
                o = xl_sample_stretch(s, nl);
                if (o) { set_sample(sel(), o); msg("Stretched to %d bar%s at %.1f BPM (%.2f s), pitch kept", bars, bars > 1 ? "s" : "", XL.g[G_BPM], nl / (double)XL_SR); }
                else msg("Too short to stretch");
                break;
            }
            case 6: if (U.slice < XL_NSLICE) { undo_push(); ST()->srev[U.slice] = !ST()->srev[U.slice]; } break;
            }
            break;
        }
        case R_DPLOT: deck_drag(x, y, 1); break;
        case R_DBANDS: undo_push(); ST()->p[P_BANDS] = (float)r->a; break;
        case R_DSLOPE: undo_push(); ST()->p[P_SLOPE] = (float)r->a; break;
        case R_DCARD: U.band = r->a; break;
        case R_MSTRIP: atomic_store(&XL.sel_track, r->a); break;
        case R_MFADER: case R_MPAN: case R_MSEND: case R_XF: case R_RET: undo_push(); xl_ui_mouse(MS_DRAG, x, y, mods, 0); break;
        case R_MMUTE: XL.trk[r->a].p[P_MUTE] = XL.trk[r->a].p[P_MUTE] >= 0.5f ? 0.0f : 1.0f; break;
        case R_MSOLO: XL.trk[r->a].p[P_SOLO] = XL.trk[r->a].p[P_SOLO] >= 0.5f ? 0.0f : 1.0f; break;
        case R_XCURVE: XL.g[G_XCURVE] = (float)r->a; break;
        case R_SLANE: atomic_store(&XL.sel_track, r->a); break;
        case R_SLANE + 100: {
            xl_track *t = &XL.trk[r->a];
            xl_step *s = &t->st[r->b];
            atomic_store(&XL.sel_track, r->a);
            U.step = r->b;
            if (!(mods & MOD_SHIFT)) {
                undo_push();
                s->on = !s->on;
                if (s->on) {
                    if ((int)t->p[P_ENGINE] == ENG_SHARD && (s->note < 36 || s->note >= 36 + (t->smp ? t->smp->nsl : 1)))
                        s->note = (uint8_t)(36 + r->b % (t->smp && t->smp->nsl > 0 ? t->smp->nsl : 1));
                    if (s->prob == 0) s->prob = 100;
                }
            }
            U.db = s->on;   /* drag across: paint the same */
            break;
        }
        case R_SVEL: U.step = r->a; undo_push(); xl_ui_mouse(MS_DRAG, x, y, mods, 0); break;
        case R_SPITCH: U.step = r->a; seq_pitch(y, 1, mods); break;
        case R_SBTN: {
            xl_track *t = ST();
            int n = (int)t->p[P_SLEN], i;
            switch (r->a) {
            case 0: euclid(t, -1); break;
            case 1: euclid(t, 1); break;
            case 2: case 3: {
                xl_step tmp[XL_NSTEP];
                undo_push();
                memcpy(tmp, t->st, sizeof tmp);
                for (i = 0; i < n; i++) t->st[i] = tmp[(i + (r->a == 2 ? 1 : n - 1)) % n];
                break;
            }
            case 4: {
                int ns = t->smp ? t->smp->nsl : 1;
                undo_push();
                for (i = 0; i < n; i++) {
                    t->st[i].on = (uint8_t)(rand() % 100 < 35);
                    t->st[i].note = (uint8_t)((int)t->p[P_ENGINE] == ENG_SHARD ? 36 + rand() % (ns > 0 ? ns : 1) : 60);
                    t->st[i].vel = (uint8_t)(70 + rand() % 57);
                }
                break;
            }
            case 5: undo_push(); for (i = 0; i < XL_NSTEP; i++) t->st[i].on = 0; break;
            }
            break;
        }
        case R_SCENE:
            if (mods & MOD_ALT) { XL.scene[r->a].valid = 0; msg("Scene %d cleared", r->a + 1); U.dtype = R_NONE; }
            else if (mods & MOD_SHIFT) { scene_store(r->a); U.dtype = R_NONE; }
            break;
        case R_FSRC: {
            char p[1200];
            if (r->a == 0) snprintf(p, sizeof p, "%s/Samples", XL.home);
            else if (r->a == 1) snprintf(p, sizeof p, "%s/Captures", XL.home);
            else if (r->a == 2) snprintf(p, sizeof p, "%s/Samples/Demo", XL.home);
            else { const char *h = getenv("HOME"); snprintf(p, sizeof p, "%s", h ? h : "/"); }
            files_open(p);
            break;
        }
        case R_FILE: file_click(r->a, clicks); break;
        case R_FBTN: {
            char p[1400];
            if (r->a <= 1) { if (U.fsel >= 0) { file_path(U.fsel, p, sizeof p); load_to(sel(), p, r->a == 1); } else msg("Pick a file first"); }
            else if (r->a == 2) { char *e; snprintf(p, sizeof p, "%s", U.dir); e = strrchr(p, '/'); if (e && e != p) { *e = 0; files_open(p); } else files_open("/"); }
            else if (plat_choose_folder(p, sizeof p)) files_open(p);
            break;
        }
        case R_SETBTN:
            if (r->a == 0) { U.learn = 1; U.learn_i = 0; msg("Turn your controller's knob 1 (then 2, 3 ... 8)"); }
            else if (r->a == 1) { U.learn = 2; U.learn_i = 0; msg("Press the controller's PLAY button"); }
            else if (r->a == 2) { int i; for (i = 0; i < 8; i++) U.kcc[i] = -1; for (i = 0; i < 3; i++) U.tcc[i] = -1; msg("MIDI mapping forgotten"); }
            else { cmd(CMD_PANIC, 0, 0, 0, 0); msg("All notes off"); }
            break;
        case R_HELP: U.help = 0; break;
        }
        return;
    }
    if (type == MS_DRAG) {
        float ddx = x - U.dlastx, ddy = y - U.dlasty, fine = (mods & MOD_SHIFT) ? 0.2f : 1.0f;
        U.dlastx = x; U.dlasty = y;
        if (fabsf(x - U.dx0) + fabsf(y - U.dy0) > 3) U.dmoved = 1;
        switch (U.dtype) {
        case R_BPM: XL.g[G_BPM] = xl_pval(&XL_G[G_BPM], U.dv0 + (U.dy0 - y) * 0.25f * fine); break;
        case R_KNOB: {
            const xl_pdesc *d;
            kget(PAGE_K[U.da], &d);
            if (d->flags & (PF_INT | PF_ENUM)) {
                float steps = d->max - d->min, per = steps > 40 ? 3 : 18;
                kset(PAGE_K[U.da], kden(d, U.dv0) + (U.dy0 - y) / per * fine);
            } else kset(PAGE_K[U.da], kden(d, U.dv0 + (U.dy0 - y) / 220 * fine));
            break;
        }
        case R_WOV: if (ST()->smp) { U.view0 = (x - 20) / 1240 - 0.5f / U.zoom; if (U.view0 < 0) U.view0 = 0; if (U.view0 > 1 - 1 / U.zoom) U.view0 = 1 - 1 / U.zoom; } break;
        case R_WMAIN: {
            xl_sample *s = ST()->smp;
            if (s && U.da > 0 && U.da < s->nsl) {
                int f = wave_frame(s, x);
                if (f > s->sl[U.da - 1] + 64 && f < s->sl[U.da + 1] - 64) s->sl[U.da] = f;
            }
            break;
        }
        case R_DPLOT: deck_drag(x, y, 0); break;
        case R_MFADER: { float fy = CY0 + 16 + 66; XL.trk[U.da].p[P_LEVEL] = xl_pval(&XL_P[P_LEVEL], -60 + 66 * (1 - (y - fy) / 230)); break; }
        case R_MPAN: { float x0 = 20 + U.da * 112 + 40; XL.trk[U.da].p[P_PAN] = xl_pval(&XL_P[P_PAN], (x - x0 - 26) / 26); break; }
        case R_MSEND: { float x0 = 20 + U.da * 112 + 34; XL.trk[U.da].p[P_SENDA + U.db] = xl_pval(&XL_P[P_SENDA], (x - x0) / 58); break; }
        case R_XF: XL.g[G_XFADE] = xl_pval(&XL_G[G_XFADE], (x - 946) / (334 - 40)); break;
        case R_RET: {
            int id = U.da == 0 ? G_RETA : U.da == 1 ? G_RETB : U.da == 2 ? G_FXRET : G_MASTER;
            XL.g[id] = xl_pval(&XL_G[id], XL.g[id] + ddx * 0.25f * fine);
            break;
        }
        case R_SLANE + 100: {
            region *r = hit(x, y);
            if (r && r->type == R_SLANE + 100 && r->a == U.da) {
                xl_track *t = &XL.trk[r->a];
                if (t->st[r->b].on != U.db) {
                    t->st[r->b].on = (uint8_t)U.db;
                    if (U.db && (int)t->p[P_ENGINE] == ENG_SHARD) t->st[r->b].note = (uint8_t)(36 + r->b % (t->smp && t->smp->nsl > 0 ? t->smp->nsl : 1));
                }
            }
            break;
        }
        case R_SVEL: {
            region *r = hit(x, y);
            float vy = SEQ_PY + SEQ_PH + 6, h = 34, v;
            if (r && r->type == R_SVEL) U.step = r->a;
            v = (vy + h - y) / h * 127;
            ST()->st[U.step].vel = (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
            break;
        }
        case R_SPITCH: seq_pitch(y, 0, mods); break;
        case R_FSCROLL: break;
        }
        (void)ddy;
        return;
    }
    if (type == MS_UP) {
        if (U.dtype == R_SPITCH && U.pnote >= 0) { cmd(CMD_NOTE_OFF, sel(), U.pnote, 0, 0); U.pnote = -1; }
        if (U.dtype == R_WMAIN || U.dtype == R_CHIP) {
            if (U.db == 1) cmd(CMD_NOTE_OFF, sel(), (int)ST()->p[P_ENGINE] == ENG_SHARD ? 36 + U.slice : 60, 0, 0);
        }
        if (U.dtype == R_SCENE) {
            int i = U.da;
            if (!XL.scene[i].valid) scene_store(i);
            else scene_launch(i, U.now - U.down_t > 0.35);
        }
        if (U.dtype == R_KNOB) {
            const xl_pdesc *d;
            kget(PAGE_K[U.da], &d);
            if (PAGE_K[U.da].kind == KR_TP && PAGE_K[U.da].id == P_SENS && ST()->smp && (int)ST()->p[P_ENGINE] == ENG_SHARD) {
                reslice(ST()->smp, ST()->p[P_SENS]);
                msg("%d slices at SENS %d", ST()->smp->nsl, (int)ST()->p[P_SENS]);
            }
        }
        U.dtype = R_NONE;
    }
}
void xl_ui_scroll(float dx, float dy, float x, float y, int mods)
{
    region *r = hit(x, y);
    if (!r) return;
    if (r->type == R_KNOB) {
        const xl_pdesc *d;
        float v;
        kref_page(U.page, PAGE_K);
        v = kget(PAGE_K[r->a], &d);
        if (d->flags & (PF_INT | PF_ENUM)) { static float acc; acc += dy; if (fabsf(acc) >= 1) { kset(PAGE_K[r->a], v + (acc > 0 ? 1 : -1)); acc = 0; } }
        else kset(PAGE_K[r->a], kden(d, knorm(d, v) + dy * ((mods & MOD_SHIFT) ? 0.001f : 0.005f)));
    } else if (r->type == R_WMAIN && ST()->smp) {
        float fx = U.view0 + (x - 20) / 1240 / U.zoom;
        U.zoom *= powf(1.02f, dy);
        if (U.zoom < 1) U.zoom = 1;
        if (U.zoom > 256) U.zoom = 256;
        U.view0 = fx - (x - 20) / 1240 / U.zoom - dx * 0.002f / U.zoom;
        if (U.view0 < 0) U.view0 = 0;
        if (U.view0 > 1 - 1 / U.zoom) U.view0 = 1 - 1 / U.zoom;
    } else if (r->type == R_FILE || r->type == R_FSCROLL) {
        static float acc;
        acc -= dy;
        while (acc > 3) { if (U.fscroll < U.nfiles - 11) U.fscroll++; acc -= 3; }
        while (acc < -3) { if (U.fscroll > 0) U.fscroll--; acc += 3; }
    }
}
static const char MUSIC_KEYS[] = "awsedftgyhujkolp;";
void xl_ui_key(int down, int key, int mods)
{
    const char *mk = key > 0 && key < 128 ? strchr(MUSIC_KEYS, key) : 0;
    if (down && U.help && (key == K_ESC || key == K_RETURN)) { U.help = 0; return; }
    if (key == K_ESC && down) { U.learn = 0; U.help = 0; return; }
    if (mods & MOD_CMD) {
        if (!down) return;
        if (key >= '1' && key <= '7') { U.page = key - '1'; if (U.page == PG_FILES && !U.dir[0]) { char p[1100]; snprintf(p, sizeof p, "%s/Samples", XL.home); files_open(p); } }
        else if (key == 'z') { if (mods & MOD_SHIFT) redo(); else undo(); }
        else if (key == 'r') { atomic_store(&XL.rec, !atomic_load(&XL.rec)); msg(atomic_load(&XL.rec) ? "Recording" : "Recording off"); }
        else if (key == 'k') capture();
        else if (key == 'm') mutate();
        else if (key == 's') save_project(0);
        return;
    }
    if (key >= '1' && key <= '8') {
        int t = key - '1';
        if (mods & MOD_SHIFT) { cmd(CMD_KILL, t, down, 0, 0); U.killkey[t] = down; if (down) msg("Track %d: LOW killed while held", t + 1); return; }
        if (U.killkey[t] && !down) { cmd(CMD_KILL, t, 0, 0, 0); U.killkey[t] = 0; return; }
        if ((mods & MOD_ALT) && t < 4) { if (down) { cmd(CMD_REPEAT, t + 1, 0, 0, 0); U.repkey = t + 1; } else if (U.repkey == t + 1) { cmd(CMD_REPEAT, 0, 0, 0, 0); U.repkey = 0; } return; }
        if (U.repkey == t + 1 && !down) { cmd(CMD_REPEAT, 0, 0, 0, 0); U.repkey = 0; return; }
        if (down) atomic_store(&XL.sel_track, t);
        return;
    }
    if (mk) {
        int base = (int)ST()->p[P_ENGINE] == ENG_SHARD ? 36 : 60, n = base + (int)(mk - MUSIC_KEYS) + U.octave * 12;
        if (n < 0) n = 0;
        if (n > 127) n = 127;
        if (down && !U.keynote[key]) { U.keynote[key] = n + 1; cmd(CMD_NOTE_ON, sel(), n, 100, 0); }
        else if (!down && U.keynote[key]) { cmd(CMD_NOTE_OFF, sel(), U.keynote[key] - 1, 0, 0); U.keynote[key] = 0; }
        return;
    }
    if (!down) return;
    switch (key) {
    case K_SPACE: play_toggle(); break;
    case K_TAB: U.page = (U.page + ((mods & MOD_SHIFT) ? PG_N - 1 : 1)) % PG_N;
        if (U.page == PG_FILES && !U.dir[0]) { char p[1100]; snprintf(p, sizeof p, "%s/Samples", XL.home); files_open(p); } break;
    case 'z': if (U.octave > -3) U.octave--; msg("Octave %+d", U.octave); break;
    case 'x': if (U.octave < 4) U.octave++; msg("Octave %+d", U.octave); break;
    case '?': case '/': U.help = !U.help; break;
    case K_LEFT: if (U.page == PG_SEQ) { if (U.step > 0) U.step--; } else if (U.slice > 0) U.slice--; break;
    case K_RIGHT: if (U.page == PG_SEQ) { if (U.step < (int)ST()->p[P_SLEN] - 1) U.step++; } else if (U.slice < nslices() - 1) U.slice++; break;
    case K_UP: if (U.band > 0) U.band--; break;
    case K_DOWN: if (U.band < (int)ST()->p[P_BANDS] - 1) U.band++; break;
    }
}
void xl_ui_drop(const char *path, float x, float y)
{
    region *r = hit(x, y);
    int t = r && r->type == R_TRK ? r->a : sel();
    load_to(t, path, 0);
}
void xl_ui_quit(void) { save_project(1); }

void xl_ui_init(int scale)
{
    int i;
    char p[1200];
    U.scale = scale;
    U.zoom = 1; U.view0 = 0; U.fsel = -1;
    for (i = 0; i < 8; i++) { U.kcc[i] = -1; KD_TLV[i] = XL_P[P_LEVEL]; }
    for (i = 0; i < 8; i++) { static char nm[8][12]; snprintf(nm[i], sizeof nm[i], "TRACK %d", i + 1); KD_TLV[i].name = nm[i]; }
    for (i = 0; i < 3; i++) U.tcc[i] = -1;
    snprintf(p, sizeof p, "%s/Projects/midi.txt", XL.home);
    {
        FILE *f = fopen(p, "r");
        if (f) {
            for (i = 0; i < 8; i++) if (fscanf(f, "%d %d", &U.kch[i], &U.kcc[i]) != 2) break;
            for (i = 0; i < 3; i++) if (fscanf(f, "%d %d", &U.tch[i], &U.tcc[i]) != 2) break;
            fclose(f);
        }
    }
    msg("SHARD XL %s · press Space to play, ? for the keys", XL_VERSION);
}
/* the MIDI mapping is kept next to the project (project.c calls this on save) */
static void ui_save_midi(void)
{
    char p[1200];
    FILE *f;
    int i;
    snprintf(p, sizeof p, "%s/Projects/midi.txt", XL.home);
    f = fopen(p, "w");
    if (!f) return;
    for (i = 0; i < 8; i++) fprintf(f, "%d %d\n", U.kch[i], U.kcc[i]);
    for (i = 0; i < 3; i++) fprintf(f, "%d %d\n", U.tch[i], U.tcc[i]);
    fclose(f);
}
