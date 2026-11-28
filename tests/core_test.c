/* SPDX-License-Identifier: GPL-3.0-only */
/* The core's own checks, on the host:  make test
 * the deck's bands add back up flat (12 and 24 dB/oct, 2 to 4 bands), KILL kills, extreme settings stay finite,
 * the sequencer's timing, ratchets, chance and "every N", scenes and their morph, the project file round trip,
 * stretching, slicing, the capture ring. */
#include "../src/xl_core.c"
#include <stdio.h>

static int bad;
static void ck(const char *what, int ok) { printf("%-70s %s\n", what, ok ? "ok" : "FAIL"); bad += !ok; }

/* the magnitude response of track 0's deck, from its impulse response */
static void deck_response(int bands, float slope, float *maxdev, float *at50)
{
    enum { N = 8192 };
    static float buf[N * 2];
    static float re[N], im[N];
    int i, b;
    xl_track *t = &XL.trk[0];
    xl_track_defaults(t);
    t->p[P_BANDS] = (float)bands; t->p[P_SLOPE] = slope;
    t->p[P_X1] = 150; t->p[P_X2] = 1500; t->p[P_X3] = 6000;
    for (b = 0; b < 4; b++) { t->p[P_DR0 + b] = 0; t->p[P_FB0 + b] = 0; }
    memset(&R.deck[0], 0, sizeof R.deck[0]);
    R.deck[0].cbands = -1;
    memset(buf, 0, sizeof buf);
    buf[0] = buf[1] = 1;
    for (i = 0; i < 64; i++) deck_run(0, buf, 1), memmove(buf, buf, 0);   /* settle the smoothed gains on silence */
    memset(&R.deck[0], 0, sizeof R.deck[0]);
    R.deck[0].cbands = -1;
    for (b = 0; b < 4; b++) R.deck[0].glv[b] = t->p[P_LV0 + b] <= -59.5f ? 0 : powf(10, t->p[P_LV0 + b] / 20);
    memset(buf, 0, sizeof buf);
    buf[0] = buf[1] = 1;
    for (i = 0; i < N; i += 64) deck_run(0, buf + 2 * i, 64);
    for (i = 0; i < N; i++) { re[i] = buf[2 * i]; im[i] = 0; }
    fft(re, im, N);
    *maxdev = 0;
    for (i = 1; i < N / 2; i++) {
        float hz = (float)i * XL_SR / N, db = 10 * log10f(re[i] * re[i] + im[i] * im[i] + 1e-20f);
        if (hz > 40 && hz < 16000 && fabsf(db) > *maxdev) *maxdev = fabsf(db);
        if (fabsf(hz - 50) < (float)XL_SR / N) *at50 = db;
    }
}

int main(void)
{
    int b, s, i;
    float dev, at50;
    char home[] = "/tmp/xl_core_test";
    plat_mkdir(home);
    xl_app_init(home, 1);

    for (s = 0; s < 2; s++)
        for (b = 2; b <= 4; b++) {
            char w[100];
            deck_response(b, (float)s, &dev, &at50);
            snprintf(w, sizeof w, "deck: %d bands at %s/oct add back up flat (%.2f dB off at most)", b, s ? "24 dB" : "12 dB", dev);
            ck(w, dev < 0.5f);
        }
    XL.trk[0].p[P_LV0] = -60;
    {
        /* (deck_response resets the defaults: set KILL after it, by hand) */
        enum { N = 8192 };
        static float buf[N * 2];
        xl_track *t = &XL.trk[0];
        float pk = 0;
        xl_track_defaults(t);
        t->p[P_BANDS] = 3; t->p[P_X1] = 300; t->p[P_LV0] = -60;
        memset(&R.deck[0], 0, sizeof R.deck[0]); R.deck[0].cbands = -1;
        for (i = 0; i < N; i++) buf[2 * i] = buf[2 * i + 1] = sinf(2 * (float)M_PI * 50 * i / XL_SR);
        for (i = 0; i < N; i += 64) deck_run(0, buf + 2 * i, 64);
        for (i = N / 2; i < N; i++) if (fabsf(buf[2 * i]) > pk) pk = fabsf(buf[2 * i]);
        ck("deck: LOW at KILL takes a 50 Hz tone down by 30 dB or more", pk < 0.0316f);
    }
    {   /* everything at its extreme: noise in, finite and bounded out, every type in every band */
        static float buf[XL_SR * 2];
        xl_track *t = &XL.trk[0];
        float pk = 0;
        int fin = 1, ty;
        xl_track_defaults(t);
        t->p[P_BANDS] = 4;
        for (ty = 1; ty < SH_N; ty++) {
            for (b = 0; b < 4; b++) { t->p[P_DR0 + b] = 1; t->p[P_FB0 + b] = 0.95f; t->p[P_TY0 + b] = (float)((ty + b - 1) % (SH_N - 1) + 1); t->p[P_LV0 + b] = 12; }
            t->p[P_DJ] = ty % 3 == 0 ? -1.0f : ty % 3 == 1 ? 1.0f : 0.5f;
            for (i = 0; i < XL_SR / 2; i++) buf[2 * i] = buf[2 * i + 1] = (float)(rand() % 20001 - 10000) / 5000;
            for (i = 0; i < XL_SR / 2; i += 64) deck_run(0, buf + 2 * i, 64);
            for (i = 0; i < XL_SR; i++) { if (buf[i] != buf[i]) fin = 0; if (fabsf(buf[i]) > pk) pk = fabsf(buf[i]); }
        }
        ck("deck: every TYPE with DRIVE and FEEDBACK at their extremes stays finite", fin && pk < 64);
    }
    {   /* 4x: a 7 kHz tone through DIST at full DRIVE: what folds back below 20 kHz (not a harmonic) */
        enum { N = 16384 };
        static float buf[N * 2], re[N], im[N];
        float alias[2];
        int os;
        for (os = 0; os < 2; os++) {
            float fund = 0, al = 0;
            xl_track *t = &XL.trk[0];
            xl_os_off = !os;
            xl_track_defaults(t);
            t->p[P_BANDS] = 2; t->p[P_X1] = 100; t->p[P_TY1] = SH_DIST; t->p[P_DR1] = 1;
            memset(&R.deck[0], 0, sizeof R.deck[0]); R.deck[0].cbands = -1;
            for (i = 0; i < N; i++) buf[2 * i] = buf[2 * i + 1] = 0.5f * sinf(2 * (float)M_PI * 7000 * i / XL_SR);
            for (i = 0; i < N; i += 64) deck_run(0, buf + 2 * i, 64);
            for (i = 0; i < N; i++) { re[i] = buf[2 * i] * (0.5f - 0.5f * cosf(2 * (float)M_PI * i / N)); im[i] = 0; }
            fft(re, im, N);
            for (i = 4; i < N / 2; i++) {
                float hz = (float)i * XL_SR / N, pw = re[i] * re[i] + im[i] * im[i], h = hz / 7000;
                if (fabsf(hz - 7000) < 60) fund += pw;
                else if (hz < 20000 && fabsf(h - floorf(h + 0.5f)) * 7000 > 150) al += pw;
            }
            alias[os] = 10 * log10f(al / fund + 1e-20f);
        }
        xl_os_off = 0;
        {
            char w[120];
            snprintf(w, sizeof w, "deck: 4x cuts DIST's aliasing (%.0f dB at 1x, %.0f dB at 4x)", alias[0], alias[1]);
            ck(w, alias[1] < alias[0] - 15);
        }
    }
    {   /* every band in line: one band shaped, the rest OFF, still adds up flat at low DRIVE */
        float dv, a5;
        deck_response(3, 1, &dv, &a5);
        ck("deck: with OFF everywhere the bands still add up flat (24 samples late)", dv < 0.5f);
    }
    /* the sequencer: 16ths at 120 BPM, 2 bars -> 32 notes; ratchets, chance, conditions, chords, ties, slides */
    {
        static float out[2 * 4096];
        int age0, frames = (int)(2 * 4 * 0.5 * XL_SR), done;
        for (i = 0; i < XL_NTRK; i++) { xl_track_defaults(&XL.trk[i]); }
        XL.g[G_BPM] = 120; XL.g[G_SWING] = 50;
        for (i = 0; i < 16; i++) note1(&XL.trk[0].st[i], 60);
        { xl_msg m = {CMD_PLAY, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        age0 = R.age;
        for (done = 0; done < frames - 32; done += 64) xl_render(out, 64, 2);
        ck("sequencer: 16 steps of 1/16 at 120 BPM play 32 notes in 2 bars", R.age - age0 == 32);
        XL.trk[0].st[0].flags = 3u << SF_RATCH_SH;          /* x4 */
        XL.trk[0].st[1].prob = 0;                           /* never */
        XL.trk[0].st[2].prob = COND_FIRST + 0;              /* 1:2 */
        XL.trk[0].st[3].prob = COND_FIRST + 9;              /* FILL (not held) */
        XL.trk[0].st[4].note[1] = 64; XL.trk[0].st[4].note[2] = 67; XL.trk[0].st[4].n = 3;   /* a chord: +2 */
        XL.trk[0].st[6].time = ST_TIE;                      /* a tie: no note of its own */
        { xl_msg m = {CMD_PLAY, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        age0 = R.age;
        for (done = 0; done < frames - 32; done += 64) xl_render(out, 64, 2);
        ck("sequencer: ratchet x4, chance 0, 1:2, FILL, a 3-note chord, a tie (32 +6 -2 -1 -2 +4 -2)", R.age - age0 == 32 + 6 - 2 - 1 - 2 + 4 - 2);
        { xl_msg m = {CMD_STOP, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        xl_render(out, 64, 2);
    }
    {   /* a note with 3 TIEs after it sounds 3.5 steps (GATE 50%); a SLIDE glides without a new attack */
        static float out[2 * 64];
        int done, i0, held = 0, attacks;
        double gone = -1;
        xl_track *t = &XL.trk[1];
        for (i = 0; i < XL_NTRK; i++) xl_track_defaults(&XL.trk[i]);
        t->p[P_ENGINE] = ENG_SAMPLE; t->p[P_REL] = 1; t->p[P_GATE] = 50;
        if (!t->smp) { float *z = calloc(XL_SR * 2 * 4, sizeof(float)); for (i = 0; i < XL_SR * 4 * 2; i++) z[i] = 0.1f; t->smp = xl_sample_new(z, XL_SR * 4, XL_SR, 2, "flat"); free(z); }
        note1(&t->st[0], 60); t->st[1].time = t->st[2].time = t->st[3].time = ST_TIE;
        { xl_msg m = {CMD_PLAY, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        for (done = 0; done < XL_SR; done += 64) {
            xl_render(out, 64, 2);
            for (i0 = 0, held = 0; i0 < XL_NVOICE; i0++) held += R.v[1][i0].on && !R.v[1][i0].rel;
            if (!held && gone < 0 && done > 64) gone = done / (XL_SR * 0.125);   /* in steps (1/16 at 120 = 0.125 s) */
        }
        { char w[100]; snprintf(w, sizeof w, "sequencer: a note and 3 TIEs at GATE 50%% holds 3.5 steps (%.2f)", gone); ck(w, fabs(gone - 3.5) < 0.1); }
        { xl_msg m = {CMD_STOP, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        xl_render(out, 64, 2);
        memset(t->st, 0, sizeof t->st);
        note1(&t->st[0], 60); t->st[0].flags = SF_SLIDE; note1(&t->st[1], 67);
        t->p[P_SLEN] = 4;
        { xl_msg m = {CMD_PLAY, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        attacks = R.age;
        for (done = 0; done < XL_SR * 0.16; done += 64) xl_render(out, 64, 2);   /* into step 2 (it ends at 0.19 s) */
        attacks = R.age - attacks;
        {
            int glided = 0;
            for (i0 = 0; i0 < XL_NVOICE; i0++) if (R.v[1][i0].on && R.v[1][i0].note == 67 && fabs(R.v[1][i0].tinc - pow(2, 7 / 12.0)) < 0.001 && R.v[1][i0].inc > 1.2) glided = 1;   /* (on its way: ~60 ms) */
            ck("sequencer: SLIDE into the next note: one attack, the pitch glides there", attacks == 1 && glided);
        }
        { xl_msg m = {CMD_STOP, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        xl_render(out, 64, 2);
    }
    {   /* step entry: REC while stopped: keys held together make the cursor step, letting go moves on */
        static float out[2 * 64];
        xl_track *t = &XL.trk[0];
        memset(t->st, 0, sizeof t->st); t->p[P_SLEN] = 16;
        atomic_store(&XL.sel_track, 0); atomic_store(&XL.cursor, 5); atomic_store(&XL.rec, 1);
        xl_midi_in(0x90, 60, 100); xl_midi_in(0x90, 63, 100); xl_midi_in(0x90, 67, 90);
        xl_render(out, 64, 2);
        xl_midi_in(0x80, 60, 0); xl_midi_in(0x80, 63, 0); xl_midi_in(0x80, 67, 0);
        xl_render(out, 64, 2);
        atomic_store(&XL.rec, 0);
        ck("step entry: a chord of 3 on step 6, the cursor on 7", t->st[5].n == 3 && t->st[5].note[2] == 67 && atomic_load(&XL.cursor) == 6);
        while (xl_q_pop(&XL_EV, &(xl_msg){0})) ;
    }
    /* scenes: a jump, and a morph of 1 bar */
    {
        static float out[2 * 64];
        int done;
        for (i = 0; i < XL_NTRK; i++) xl_track_defaults(&XL.trk[i]);
        XL.trk[2].p[P_DJ] = 0; note1(&XL.trk[2].st[5], 60);
        scene_store(0);
        XL.trk[2].p[P_DJ] = 1; XL.trk[2].st[5].n = 0; XL.trk[2].p[P_SLEN] = 8;
        scene_store(1);
        XL.g[G_QUANT] = 2;
        { xl_msg m = {CMD_SCENE, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        xl_render(out, 64, 2);
        ck("scene: a jump while stopped sets the sound and the pattern at once",
           XL.trk[2].p[P_DJ] == 0 && step_on(&XL.trk[2].st[5]) && XL.trk[2].p[P_SLEN] == 16);
        { xl_msg m = {CMD_PLAY, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        xl_render(out, 64, 2);
        { xl_msg m = {CMD_SCENE, 1, 1, 0, 0}; xl_q_push(&XL_CMD, m); }
        for (done = 0; done < XL_SR * 2 + XL_SR * 2 / 2; done += 64) xl_render(out, 64, 2);   /* to the middle of bar 2 */
        ck("scene: a 1-bar morph is halfway at half a bar after the bar line",
           fabsf(XL.trk[2].p[P_DJ] - 0.5f) < 0.05f && XL.trk[2].p[P_SLEN] == 8);
        for (done = 0; done < XL_SR; done += 64) xl_render(out, 64, 2);
        ck("scene: and arrives", XL.trk[2].p[P_DJ] == 1 && XL.v.morph_t < 0);
        { xl_msg m = {CMD_STOP, 0, 0, 0, 0}; xl_q_push(&XL_CMD, m); }
        xl_render(out, 64, 2);
    }
    /* the project file */
    {
        char p[300];
        float keep;
        snprintf(p, sizeof p, "%s/Projects/test.sxl", home);
        XL.trk[5].p[P_X2] = 1234.5f; note1(&XL.trk[5].st[9], 61); XL.trk[5].st[9].note[1] = 65; XL.trk[5].st[9].n = 2;
        XL.trk[5].st[9].flags = SF_SLIDE | (2u << SF_RATCH_SH); XL.trk[5].st[10].time = ST_TIE; XL.trk[5].st[9].prob = COND_FIRST + 3;
        XL.g[G_BPM] = 133.5f;
        keep = XL.scene[1].p[2][P_DJ];
        ck("project: saved", xl_project_save(p) == 0);
        XL.trk[5].p[P_X2] = 100; memset(&XL.trk[5].st[9], 0, 2 * sizeof(xl_step)); XL.g[G_BPM] = 90; XL.scene[1].p[2][P_DJ] = 0;
        ck("project: loaded", xl_project_load(p) == 0);
        ck("project: the parameters, steps, globals and scenes come back",
           fabsf(XL.trk[5].p[P_X2] - 1234.5f) < 0.01f && XL.trk[5].st[9].n == 2 && XL.trk[5].st[9].note[1] == 65 &&
           step_ratchet(&XL.trk[5].st[9]) == 3 && (XL.trk[5].st[9].flags & SF_SLIDE) && XL.trk[5].st[10].time == ST_TIE &&
           XL.trk[5].st[9].prob == COND_FIRST + 3 &&
           XL.g[G_BPM] == 133.5f && XL.scene[1].p[2][P_DJ] == keep && XL.scene[1].valid);
    }
    {   /* a version 2 file's steps (on note vel chance ratchet every) convert */
        char p[300];
        FILE *f;
        snprintf(p, sizeof p, "%s/Projects/v2.sxl", home);
        f = fopen(p, "w");
        fprintf(f, "shardxl 2\nt 6 st 3 1 64 90 100 2 1\nt 6 st 4 1 65 90 50 1 3\nt 6 st 5 0 60 100 100 1 1\n");
        fclose(f);
        xl_project_load(p);
        {
            const xl_step *a = &XL.trk[6].st[3], *b = &XL.trk[6].st[4], *c = &XL.trk[6].st[5];
            ck("project: version 2 steps convert (on, note, ratchet, every N)",
               step_on(a) && a->note[0] == 64 && step_ratchet(a) == 2 && b->prob == COND_FIRST + 2 && !step_on(c));
        }
    }
    /* samples */
    {
        xl_sample *a, *o;
        float *z = calloc(XL_SR * 2 * 3, sizeof(float));
        for (i = 0; i < 6; i++) { int k; for (k = 0; k < 2000; k++) z[2 * (i * XL_SR / 2 + k)] = z[2 * (i * XL_SR / 2 + k) + 1] = sinf(k * 0.3f) * expf(-k / 500.0f); }
        a = xl_sample_new(z, XL_SR * 3, XL_SR, 2, "t");
        xl_slice_auto(a, 50);
        ck("slicing: 6 hits, 6 slices", a->nsl == 6);
        o = xl_sample_stretch(a, XL_SR * 2);
        ck("stretch: to the asked length", o && o->len == XL_SR * 2);
        xl_sample_free(o); xl_sample_free(a);
        a = xl_sample_new(z, XL_SR * 3, 44100, 2, "t");
        ck("44.1 kHz in: converted to 48 kHz", a && abs(a->len - (int)(XL_SR * 3 * 48000.0 / 44100)) <= 1);
        xl_sample_free(a); free(z);
    }
    {
        static float cap[XL_SR * 2];
        ck("capture: the newest frames come back", xl_capture_copy(cap, XL_SR) > XL_SR / 2);
    }
    printf(bad ? "core test FAILED (%d)\n" : "core test passed\n", bad);
    return bad != 0;
}
