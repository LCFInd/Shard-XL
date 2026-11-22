/* SPDX-License-Identifier: GPL-3.0-only */
/* The project file: plain text, one value per line, so it can be read and mended by hand.
 *   shardxl 1
 *   g <index> <value>                  globals (xl.h G_*)
 *   t <track> p <index> <value>        track parameters (xl.h P_*)
 *   t <track> sample <path>            its sample (by path: the file itself stays where it is)
 *   t <track> slices <n> <start>...    its slice points (frames at 48 kHz)
 *   t <track> sl <k> <dB> <st> <rev>   per-slice level, pitch, reverse
 *   t <track> st <i> <on> <note> <vel> <chance> <ratchet> <every>
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
    fprintf(f, "shardxl 1\n");
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
            fprintf(f, "t %d st %d %d %d %d %d %d %d\n", t, i, s->on, s->note, s->vel, s->prob, s->ratchet, s->cond);
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
                if (s->on || s->note != 60) fprintf(f, "scene %d t %d st %d %d %d %d %d %d %d\n", k, t, i, s->on, s->note, s->vel, s->prob, s->ratchet, s->cond);
            }
        }
    }
    if (fclose(f) != 0) return -1;
    if (rename(tmp, path) != 0) return -1;
    ui_save_midi();
    return 0;
}

static void step_set(xl_step *s, int on, int note, int vel, int pr, int ra, int co)
{
    s->on = (uint8_t)(on != 0); s->note = (uint8_t)(note & 127); s->vel = (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel);
    s->prob = (uint8_t)(pr < 0 ? 0 : pr > 100 ? 100 : pr); s->ratchet = (uint8_t)(ra < 1 ? 1 : ra > 4 ? 4 : ra);
    s->cond = (uint8_t)(co < 1 ? 1 : co > 8 ? 8 : co);
}
int xl_project_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    int t, i, k, a[8];
    float v, w;
    if (!f) return -1;
    if (!fgets(line, sizeof line, f) || strncmp(line, "shardxl ", 8)) { fclose(f); return -1; }
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
        } else if (sscanf(line, "t %d st %d %d %d %d %d %d %d", &t, &i, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 8) {
            if (t >= 0 && t < XL_NTRK && i >= 0 && i < XL_NSTEP) step_set(&XL.trk[t].st[i], a[0], a[1], a[2], a[3], a[4], a[5]);
        } else if (sscanf(line, "scene %d t %d p %d %f", &k, &t, &i, &v) == 4) {
            if (k >= 0 && k < XL_NSCENE && t >= 0 && t < XL_NTRK && i >= 0 && i < NP) XL.scene[k].p[t][i] = xl_pval(&XL_P[i], v);
        } else if (sscanf(line, "scene %d t %d st %d %d %d %d %d %d %d", &k, &t, &i, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 9) {
            if (k >= 0 && k < XL_NSCENE && t >= 0 && t < XL_NTRK && i >= 0 && i < XL_NSTEP) step_set(&XL.scene[k].st[t][i], a[0], a[1], a[2], a[3], a[4], a[5]);
        } else if (sscanf(line, "scene %d valid", &k) == 1 && strstr(line, "valid")) {
            if (k >= 0 && k < XL_NSCENE) {
                XL.scene[k].valid = 1;
                for (t = 0; t < XL_NTRK; t++) {   /* (defaults for what the file leaves out) */
                    for (i = 0; i < XL_NSTEP; i++) step_set(&XL.scene[k].st[t][i], 0, 60, 100, 100, 1, 1);
                    for (i = 0; i < NP; i++) XL.scene[k].p[t][i] = XL_P[i].def;
                }
            }
        }
    }
    fclose(f);
    return 0;
}
