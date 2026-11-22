/* SPDX-License-Identifier: GPL-3.0-only */
/* SHARD XL: the shared core. The engine (audio thread), the screens (UI thread) and the platform layer
 * (src/plat_mac.m, src/plat_headless.c) meet here. Everything is float: the Pi 5 and the Mac have FPUs. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#define XL_VERSION "0.1.0"
#define XL_SR 48000
#define XL_BLOCK 64
#define XL_NTRK 8
#define XL_NVOICE 16
#define XL_NSTEP 64
#define XL_NSCENE 8
#define XL_NSLICE 64
#define XL_CAPTURE_SEC 60

enum { ENG_SAMPLE, ENG_SHARD, ENG_N };
/* a band's shaper: SHARD's DRIVE types, in SHARD's order (the project file stores the index) */
enum { SH_OFF, SH_OVRDRV, SH_DIST, SH_FOLD, SH_CRUSH, SH_DECIM, SH_HALF, SH_TUBE, SH_FUZZ, SH_N };
enum { DIV_4, DIV_8, DIV_16, DIV_32, DIV_8T, DIV_16T, DIV_N };

/* A track's parameters: floats, so a scene can morph between two of them. */
enum {
    P_ENGINE, P_TUNE, P_ATK, P_REL, P_SENS,
    P_LEVEL, P_PAN, P_SENDA, P_SENDB, P_SENDFX, P_MUTE, P_SOLO,
    P_BANDS, P_SLOPE, P_X1, P_X2, P_X3,
    P_LV0, P_LV1, P_LV2, P_LV3,
    P_TY0, P_TY1, P_TY2, P_TY3,
    P_DR0, P_DR1, P_DR2, P_DR3,
    P_FB0, P_FB1, P_FB2, P_FB3,
    P_DJ, P_SLEN, P_SDIV,
    NP
};
/* Global settings (not per track). */
enum {
    G_BPM, G_SWING, G_XFADE, G_XCURVE, G_RETA, G_RETB, G_FXRET, G_MASTER,
    G_SPLIT, G_LOWTRK, G_HITRK, G_PADMODE, G_QUANT, G_MORPHBARS, G_SCENEA, G_SCENEB, G_MORPH,
    G_MUTATE, G_RSBARS, G_PREVIEW,
    NG
};

#define PF_LOG 1   /* the knob moves on a log scale */
#define PF_INT 2   /* whole numbers only */
#define PF_ENUM 4  /* whole numbers, shown as labels */
typedef struct { const char *name; float min, max, def; int flags; const char *const *labels; const char *unit; } xl_pdesc;
extern const xl_pdesc XL_P[NP], XL_G[NG];

typedef struct {               /* one step of a track's pattern */
    uint8_t on, note, vel, prob, ratchet, cond;
} xl_step;

typedef struct xl_sample {     /* stereo, 48 kHz, interleaved */
    float *d;
    int len;
    int nsl;                   /* slices: sl[0..nsl-1] starts, sl[nsl] = len */
    int sl[XL_NSLICE + 1];
    float *pk;                 /* peaks: min, max per 256 frames (mono) */
    int npk;
    char name[96];
    char path[1024];
} xl_sample;
#define XL_PKBLK 256

typedef struct {
    float p[NP];
    xl_sample *smp;            /* owned: swapped only through the audio thread (CMD_SAMPLE) */
    xl_step st[XL_NSTEP];
    float slv[XL_NSLICE], spit[XL_NSLICE];   /* per slice: level (dB), pitch (semitones) */
    uint8_t srev[XL_NSLICE];
} xl_track;

typedef struct {
    int valid;
    float p[XL_NTRK][NP];
    xl_step st[XL_NTRK][XL_NSTEP];
} xl_scene;

typedef struct {               /* what the engine shows the screens (written by the audio thread) */
    float meter[XL_NTRK], mmeter[2], cpu;
    double beat;               /* the transport's position, in beats */
    int step[XL_NTRK];         /* the step each track is on */
    int vpos[XL_NTRK][XL_NVOICE];   /* voice playheads (frames), -1 idle */
    int scene_playing, scene_queued;
    float morph_t;             /* 0..1 while a scene morphs, else -1 */
    int resampling;            /* 0 idle, 1 waiting for the bar, 2 recording */
    int capture_filled;        /* frames in the capture buffer */
    int repeat;                /* beat repeat on (1..4) or 0 */
} xl_view;

typedef struct {               /* the whole shared state */
    xl_track trk[XL_NTRK];
    float g[NG];
    xl_scene scene[XL_NSCENE];
    atomic_int playing, rec, sel_track, spec_track, shift_held;
    xl_view v;
    char home[1024];           /* ~/Music/SHARD-XL */
} xl_state;
extern xl_state XL;

/* ---- the queues between the threads ---- */
enum { CMD_NOTE_ON, CMD_NOTE_OFF, CMD_PLAY, CMD_STOP, CMD_SCENE, CMD_SAMPLE, CMD_PREVIEW, CMD_PREVIEW_STOP,
       CMD_RESAMPLE, CMD_REPEAT, CMD_KILL, CMD_PANIC };
enum { EV_FREE, EV_MIDI, EV_RESAMPLED, EV_SCENE };
typedef struct { int type, a, b, c; void *ptr; } xl_msg;
#define XL_QN 1024
typedef struct { xl_msg m[XL_QN]; atomic_uint w, r; } xl_queue;
int xl_q_push(xl_queue *q, xl_msg m);
int xl_q_pop(xl_queue *q, xl_msg *m);
extern xl_queue XL_CMD, XL_MIDI, XL_EV;   /* UI->audio, MIDI->audio, audio->UI */

/* ---- engine (engine.c) ---- */
void xl_engine_init(void);
void xl_render(float *out, int frames, int channels);   /* the audio callback: interleaved */
void xl_midi_in(uint8_t st, uint8_t d1, uint8_t d2);    /* any thread (the MIDI callback) */
float xl_pval(const xl_pdesc *d, float v);              /* clamp (and round) to the descriptor */
void xl_track_defaults(xl_track *t);
void xl_globals_defaults(float *g);
/* capture ring: the last 60 s of the main output */
int xl_capture_copy(float *dst, int maxframes);         /* UI thread: the newest frames, in order */
const float *xl_resample_buf(int *frames);              /* UI thread, after EV_RESAMPLED */
void xl_spec_read(float *pre, float *post, int n);      /* the selected track's deck in/out, newest n */

/* ---- samples (sample.c) ---- */
xl_sample *xl_sample_new(const float *inter, int frames, int rate, int channels, const char *name);
xl_sample *xl_sample_load(const char *path, char *err, int errlen);
void xl_sample_free(xl_sample *s);
void xl_sample_peaks(xl_sample *s);
void xl_slice_auto(xl_sample *s, float sens);
void xl_slice_even(xl_sample *s, int n);
void xl_sample_reverse(xl_sample *s);
void xl_sample_normalize(xl_sample *s);
xl_sample *xl_sample_stretch(const xl_sample *s, int newlen);
int xl_wav_write(const char *path, const float *inter, int frames);
int xl_wav_read(const char *path, float **inter, int *frames, int *rate, int *channels);
void xl_demo_kit(void);                                  /* fills the tracks with generated sounds */

/* ---- screens (ui.c) ---- */
enum { MS_DOWN, MS_DRAG, MS_UP, MS_MOVE };
#define MOD_SHIFT 1
#define MOD_ALT 2
#define MOD_CMD 4
#define MOD_CTRL 8
enum { K_NONE = 0, K_TAB = 9, K_RETURN = 13, K_ESC = 27, K_SPACE = 32, K_DELETE = 127,
       K_LEFT = 0x100, K_RIGHT, K_UP, K_DOWN };
void xl_ui_init(int scale);
void xl_ui_frame(uint32_t *fb, int w, int h, double now);   /* fb: 0xAARRGGBB, w x h = 1280 x 720 times scale */
void xl_ui_mouse(int type, float x, float y, int mods, int clicks);   /* logical 1280 x 720 coordinates */
void xl_ui_scroll(float dx, float dy, float x, float y, int mods);
void xl_ui_key(int down, int key, int mods);
void xl_ui_drop(const char *path, float x, float y);
void xl_ui_quit(void);

/* ---- project (project.c) ---- */
int xl_project_save(const char *path);
int xl_project_load(const char *path);

/* ---- start-up (app.c) ---- */
void xl_app_init(const char *home, int scale);   /* home: ~/Music/SHARD-XL; scale: 1, or 2 on a Retina screen */

/* ---- the platform layer provides these ---- */
int plat_decode(const char *path, float **inter, int *frames, int *rate, int *channels);   /* 0: ok */
int plat_choose_folder(char *out, int outlen);   /* 1: chosen */
void plat_mkdir(const char *path);
int plat_list_dir(const char *path, char names[][256], int *isdir, int max);
double plat_time(void);
