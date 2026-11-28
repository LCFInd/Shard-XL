/* SPDX-License-Identifier: GPL-3.0-only */
/* The project file: plain text, one value per line, so it can be read and mended by hand.
 *   shardxl 3                          (1: before SHARD's DRIVE types, 2: before SHARD's steps; converted on load)
 *   g <index> <value>                  globals (xl.h G_*)
 *   t <track> p <index> <value>        track parameters (xl.h P_*)
 *   t <track> sample <path>            its sample (by path: the file itself stays where it is)
 *   t <track> slices <n> <start>...    its slice points (frames at 48 kHz)
 *   t <track> sl <k> <dB> <st> <rev>   per-slice level, pitch, reverse
 *   t <track> st <i> <time> <n> <note1..4> <vel> <flags> <chance>   (xl.h xl_step; versions 1 and 2:
 *                                      <on> <note> <vel> <chance> <ratchet> <every>)
 *   scene <k> ...                      the same p and st lines for a stored scene
 * Indices are positions in xl.h's enums: append to them, never reorder (old files keep their meaning). */
#include "xl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ui_save_midi(void);
int xl_project_save(const char *path)
{
    char tmp[1200];
    FILE *f;
    int t, i, k;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "shardxl 3\n");
    for (i = 0; i < NG; i++) fprintf(f, "g %d %.9g\n", i, XL.g[i]);
    for (t = 0; t < XL_NTRK; t++) {
        xl_track *tk = &XL.trk[t];
        for (i = 0; i < NP; i++) fprintf(f, "t %d p %d %.9g\n", t, i, tk->p[i]);
        if (tk->smp && tk->smp->path[0]) {
            fprintf(f, "t %d sample %s\n", t, tk->smp->path);
            fprintf(f, "t %d slices %d", t, tk->smp->nsl);
            for (k = 0; k < tk->smp->nsl; k++) fprintf(f, " %d", tk->smp->sl[k]);
            fprintf(f, "\n");
        }
        for (k = 0; k < XL_NSLICE; k++)
            if (tk->slv[k] != 0 || tk->spit[k] != 0 || tk->srev[k]) fprintf(f, "t %d sl %d %.9g %.9g %d\n", t, k, tk->slv[k], tk->spit[k], tk->srev[k]);
        for (i = 0; i < XL_NSTEP; i++) {
            xl_step *s = &tk->st[i];
            fprintf(f, "t %d st %d %d %d %d %d %d %d %d %d %d\n", t, i, s->time, s->n, s->note[0], s->note[1], s->note[2],
                    s->note[3], s->vel, s->flags, s->prob);
        }
    }
    for (k = 0; k < XL_NSCENE; k++) {
        xl_scene *sc = &XL.scene[k];
        if (!sc->valid) continue;
        fprintf(f, "scene %d valid\n", k);
        for (t = 0; t < XL_NTRK; t++) {
            for (i = 0; i < NP; i++) fprintf(f, "scene %d t %d p %d %.9g\n", k, t, i, sc->p[t][i]);
            for (i = 0; i < XL_NSTEP; i++) {
                xl_step *s = &sc->st[t][i];
                if (s->n || s->time != ST_NOTE || s->note[0] != 60)
                    fprintf(f, "scene %d t %d st %d %d %d %d %d %d %d %d %d %d\n", k, t, i, s->time, s->n, s->note[0], s->note[1],
                            s->note[2], s->note[3], s->vel, s->flags, s->prob);
            }
        }
    }
    if (fclose(f) != 0) return -1;
    if (rename(tmp, path) != 0) return -1;
    ui_save_midi();
    return 0;
}

static void step_set(xl_step *s, const int *a)   /* time n note1..4 vel flags chance */
{
    int j;
    s->time = (uint8_t)(a[0] >= 0 && a[0] <= ST_REST ? a[0] : ST_NOTE);
    s->n = (uint8_t)(a[1] < 0 ? 0 : a[1] > 4 ? 4 : a[1]);
    for (j = 0; j < 4; j++) s->note[j] = (uint8_t)(a[2 + j] & 127);
    s->vel = (uint8_t)(a[6] < 1 ? 1 : a[6] > 127 ? 127 : a[6]);
    s->flags = (uint8_t)(a[7] & (SF_ACCENT | SF_SLIDE | SF_RATCH));
    s->prob = (uint8_t)(a[8] < 0 ? 0 : a[8] > 100 && a[8] < (int)COND_FIRST ? 100 : a[8] >= (int)(COND_FIRST + NCOND) ? 100 : a[8]);
}
/* versions 1 and 2: on, note, vel, chance, ratchet, every N (2..4 -> 1:2, 1:3, 1:4; more -> 1:4) */
static void step_old(xl_step *s, const int *o)
{
    int a[9] = {ST_NOTE, o[0] != 0, o[1], 60, 60, 60, o[2], 0, o[3]}, r = o[4] < 1 ? 1 : o[4] > 4 ? 4 : o[4];
    a[7] = (r - 1) << SF_RATCH_SH;
    if (o[5] >= 2) a[8] = (int)COND_FIRST + (o[5] == 2 ? 0 : o[5] == 3 ? 2 : 5);
    step_set(s, a);
}
static const int EMPTY_STEP[9] = {ST_NOTE, 0, 60, 60, 60, 60, 100, 0, 100};
/* version 1 had 4 shaper types (TUBE FOLD CLIP BIT) and DRIVE 0 meant clean: to SHARD's (2026-11-22) */
static void v1_types(float *p)
{
    static const int MAP[4] = {SH_TUBE, SH_FOLD, SH_DIST, SH_CRUSH};
    int b;
    for (b = 0; b < 4; b++) {
        int old = (int)p[P_TY0 + b];
        p[P_TY0 + b] = p[P_DR0 + b] > 0.001f && old >= 0 && old < 4 ? (float)MAP[old] : (float)SH_OFF;
    }
}
int xl_project_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    int t, i, k, a[12], ver = 0, n;
    float v, w;
    if (!f) return -1;
    if (!fgets(line, sizeof line, f) || sscanf(line, "shardxl %d", &ver) != 1) { fclose(f); return -1; }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "g %d %f", &i, &v) == 2) { if (i >= 0 && i < NG) XL.g[i] = xl_pval(&XL_G[i], v); }
        else if (sscanf(line, "t %d p %d %f", &t, &i, &v) == 3) { if (t >= 0 && t < XL_NTRK && i >= 0 && i < NP) XL.trk[t].p[i] = xl_pval(&XL_P[i], v); }
        else if (sscanf(line, "t %d sample %n", &t, &k) == 1 && strncmp(line, "t ", 2) == 0 && strstr(line, " sample ")) {
            const char *p = strstr(line, " sample ") + 8;
            if (t >= 0 && t < XL_NTRK) { xl_sample *s = xl_sample_load(p, 0, 0); if (s) { xl_sample_free(XL.trk[t].smp); XL.trk[t].smp = s; } }
        } else if (strstr(line, " slices ") && sscanf(line, "t %d slices %d", &t, &k) == 2) {
            xl_sample *s = t >= 0 && t < XL_NTRK ? XL.trk[t].smp : 0;
            const char *p = strstr(line, " slices ") + 8;
            int n = 0, x, used;
            if (!s || k < 1 || k > XL_NSLICE) continue;
            sscanf(p, "%d%n", &x, &used); p += used;
            while (n < k && sscanf(p, "%d%n", &x, &used) == 1) { if (x >= 0 && x < s->len && (n == 0 || x > s->sl[n - 1])) s->sl[n++] = x; p += used; }
            if (n >= 1) { s->sl[0] = 0; s->nsl = n; s->sl[n] = s->len; }
        } else if (sscanf(line, "t %d sl %d %f %f %d", &t, &k, &v, &w, &i) == 5) {
            if (t >= 0 && t < XL_NTRK && k >= 0 && k < XL_NSLICE) { XL.trk[t].slv[k] = v; XL.trk[t].spit[k] = w; XL.trk[t].srev[k] = (uint8_t)(i != 0); }
        } else if ((n = sscanf(line, "t %d st %d %d %d %d %d %d %d %d %d %d", &t, &i, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5],
                               &a[6], &a[7], &a[8])) >= 8) {
            if (t >= 0 && t < XL_NTRK && i >= 0 && i < XL_NSTEP) { if (n == 11) step_set(&XL.trk[t].st[i], a); else step_old(&XL.trk[t].st[i], a); }
        } else if (sscanf(line, "scene %d t %d p %d %f", &k, &t, &i, &v) == 4) {
            if (k >= 0 && k < XL_NSCENE && t >= 0 && t < XL_NTRK && i >= 0 && i < NP) XL.scene[k].p[t][i] = xl_pval(&XL_P[i], v);
        } else if ((n = sscanf(line, "scene %d t %d st %d %d %d %d %d %d %d %d %d %d", &k, &t, &i, &a[0], &a[1], &a[2], &a[3],
                               &a[4], &a[5], &a[6], &a[7], &a[8])) >= 9) {
            if (k >= 0 && k < XL_NSCENE && t >= 0 && t < XL_NTRK && i >= 0 && i < XL_NSTEP) {
                if (n == 12) step_set(&XL.scene[k].st[t][i], a); else step_old(&XL.scene[k].st[t][i], a);
            }
        } else if (sscanf(line, "scene %d valid", &k) == 1 && strstr(line, "valid")) {
            if (k >= 0 && k < XL_NSCENE) {
                XL.scene[k].valid = 1;
                for (t = 0; t < XL_NTRK; t++) {   /* (defaults for what the file leaves out) */
                    for (i = 0; i < XL_NSTEP; i++) step_set(&XL.scene[k].st[t][i], EMPTY_STEP);
                    for (i = 0; i < NP; i++) XL.scene[k].p[t][i] = XL_P[i].def;
                }
            }
        }
    }
    fclose(f);
    if (ver < 2) {
        for (t = 0; t < XL_NTRK; t++) v1_types(XL.trk[t].p);
        for (k = 0; k < XL_NSCENE; k++) if (XL.scene[k].valid) for (t = 0; t < XL_NTRK; t++) v1_types(XL.scene[k].p[t]);
    }
    return 0;
}
