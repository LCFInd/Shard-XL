/* SPDX-License-Identifier: GPL-3.0-only */
/* Start-up: the folders, the engine, then the autosaved project, or on a first start the demo kit (written to
 * Samples/Demo so a project can find it again) with three scenes to switch between. */
#include "xl.h"
#include <stdio.h>
#include <string.h>

static void demo_scenes(void)
{
    int t;
    scene_store(0);
    /* 2: the machine loop in, the bass driven harder, a little filter on the drums */
    XL.trk[4].p[P_MUTE] = 0; XL.trk[1].p[P_DR1] = 0.6f; XL.trk[1].p[P_FB1] = 0.3f; XL.trk[0].p[P_DJ] = 0.15f;
    XL.trk[5].p[P_MUTE] = 0;
    scene_store(1);
    /* 3: a breakdown: the drone and the metal, the rest out */
    for (t = 0; t < XL_NTRK; t++) XL.trk[t].p[P_MUTE] = 1;
    XL.trk[3].p[P_MUTE] = 0; XL.trk[2].p[P_MUTE] = 0; XL.trk[2].p[P_SENDFX] = 0.8f; XL.trk[3].p[P_DJ] = -0.4f;
    scene_store(2);
    memcpy(XL.trk[0].p, XL.scene[0].p[0], sizeof XL.trk[0].p);
    for (t = 0; t < XL_NTRK; t++) { memcpy(XL.trk[t].p, XL.scene[0].p[t], sizeof XL.trk[t].p); memcpy(XL.trk[t].st, XL.scene[0].st[t], sizeof XL.trk[t].st); }
    XL.v.scene_playing = 0;
}
void xl_app_init(const char *home, int scale)
{
    char p[1200];
    int t;
    snprintf(XL.home, sizeof XL.home, "%s", home);
    plat_mkdir(XL.home);
    snprintf(p, sizeof p, "%s/Samples", home); plat_mkdir(p);
    snprintf(p, sizeof p, "%s/Samples/Demo", home); plat_mkdir(p);
    snprintf(p, sizeof p, "%s/Captures", home); plat_mkdir(p);
    snprintf(p, sizeof p, "%s/Projects", home); plat_mkdir(p);
    xl_engine_init();
    xl_globals_defaults(XL.g);
    for (t = 0; t < XL_NTRK; t++) xl_track_defaults(&XL.trk[t]);
    xl_ui_init(scale);
    snprintf(p, sizeof p, "%s/Projects/autosave.sxl", home);
    if (xl_project_load(p) != 0) {
        xl_demo_kit();
        for (t = 0; t < XL_NTRK; t++) {
            xl_sample *s = XL.trk[t].smp;
            if (!s) continue;
            snprintf(p, sizeof p, "%s/Samples/Demo/%s.wav", home, s->name);
            if (xl_wav_write(p, s->d, s->len) == 0) snprintf(s->path, sizeof s->path, "%s", p);
        }
        demo_scenes();
    }
}
