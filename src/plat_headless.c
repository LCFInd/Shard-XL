/* SPDX-License-Identifier: GPL-3.0-only */
/* A window-less build for testing on Linux: renders audio to a WAV file and every page to a picture.
 *   xl_headless HOME OUTDIR [seconds]   ->  OUTDIR/audio.wav, OUTDIR/<page>.ppm (2x) */
#include "xl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ppm(const char *path, const uint32_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) { unsigned char c[3] = {(unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i]}; fwrite(c, 1, 3, f); }
    fclose(f);
}
int main(int argc, char **argv)
{
    const char *home = argc > 1 ? argv[1] : "/tmp/xlhome", *out = argc > 2 ? argv[2] : "/tmp/xlout";
    double secs = argc > 3 ? atof(argv[3]) : 8;
    int scale = 2, W = 1280 * scale, H = 720 * scale, n = (int)(secs * XL_SR), done = 0, pg;
    float *audio = malloc(sizeof(float) * 2 * (size_t)n);
    uint32_t *fb = malloc(sizeof(uint32_t) * (size_t)W * H);
    char p[1200];
    xl_app_init(home, scale);
    xl_ui_key(1, ' ', 0); xl_ui_key(0, ' ', 0);   /* play */
    xl_ui_frame(fb, W, H, 0.0);
    while (done < n) {
        int k = n - done < 512 ? n - done : 512;
        xl_render(audio + 2 * done, k, 2);
        done += k;
        if (done % (XL_SR / 2) < 512) xl_ui_frame(fb, W, H, done / (double)XL_SR);
        if (done >= XL_SR * 4 && done - k < XL_SR * 4) { xl_ui_key(1, '2', MOD_CMD); }   /* (a scene change at 4 s: the SCENES page is 5) */
    }
    snprintf(p, sizeof p, "%s/audio.wav", out);
    xl_wav_write(p, audio, n);
    for (pg = 0; pg < 7; pg++) {
        char key = (char)('1' + pg);
        int r;
        xl_ui_key(1, key, MOD_CMD);
        for (r = 0; r < 12; r++) { xl_render(audio, 2048, 2); xl_ui_frame(fb, W, H, secs + pg + r * 0.05); }
        snprintf(p, sizeof p, "%s/page%d.ppm", out, pg + 1);
        ppm(p, fb, W, H);
    }
    xl_ui_key(1, '?', 0);
    xl_ui_frame(fb, W, H, secs + 10);
    snprintf(p, sizeof p, "%s/help.ppm", out);
    ppm(p, fb, W, H);
    xl_ui_quit();
    printf("rendered %.1f s and 8 pictures into %s\n", secs, out);
    return 0;
}
