/* SPDX-License-Identifier: GPL-3.0-only */
/* A small software renderer: the screens draw into one 0xAARRGGBB framebuffer in logical 1280 x 720
 * coordinates, scaled by S (2 on a Retina Mac). Everything is anti-aliased by coverage. */
#include "xl.h"
#include "font_data.h"
#include <math.h>
#include <string.h>

static uint32_t *FB;
static int FW, FH, S = 1;

static void d_begin(uint32_t *fb, int w, int h, int scale) { FB = fb; FW = w; FH = h; S = scale < 1 ? 1 : scale > 2 ? 2 : scale; }

static inline void blend(int x, int y, uint32_t c, float cov)
{
    uint32_t *p, d;
    float a;
    if ((unsigned)x >= (unsigned)FW || (unsigned)y >= (unsigned)FH || cov <= 0) return;
    a = ((c >> 24) / 255.0f) * (cov > 1 ? 1 : cov);
    p = &FB[y * FW + x]; d = *p;
    if (a >= 0.999f) { *p = c | 0xFF000000u; return; }
    {
        int r = (int)(((c >> 16) & 255) * a + ((d >> 16) & 255) * (1 - a));
        int g = (int)(((c >> 8) & 255) * a + ((d >> 8) & 255) * (1 - a));
        int b = (int)((c & 255) * a + (d & 255) * (1 - a));
        *p = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
}
static void d_clear(uint32_t c) { int i, n = FW * FH; for (i = 0; i < n; i++) FB[i] = c; }
/* a filled rectangle with optional rounded corners */
static void d_rect(float x, float y, float w, float h, uint32_t c, float rad)
{
    float X0 = x * S, Y0 = y * S, X1 = (x + w) * S, Y1 = (y + h) * S, R = rad * S;
    int ix0 = (int)floorf(X0), iy0 = (int)floorf(Y0), ix1 = (int)ceilf(X1), iy1 = (int)ceilf(Y1), i, j;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > FW) ix1 = FW;
    if (iy1 > FH) iy1 = FH;
    for (j = iy0; j < iy1; j++) {
        float py = j + 0.5f, cy = (py < Y0 + 0.5f ? py - Y0 + 0.5f : py > Y1 - 0.5f ? Y1 - py + 0.5f : 1);
        if ((c >> 24) == 255 && cy >= 1 && py > Y0 + R && py < Y1 - R) {   /* fast: a full row, edges blended */
            int a = (int)ceilf(X0), e = (int)floorf(X1), k;
            if (a < ix0) a = ix0;
            if (e > ix1) e = ix1;
            for (i = ix0; i < a; i++) blend(i, j, c, i + 1 - X0);
            for (k = a; k < e; k++) FB[j * FW + k] = c;
            for (i = e > a ? e : a; i < ix1; i++) blend(i, j, c, X1 - i);
            continue;
        }
        for (i = ix0; i < ix1; i++) {
            float px = i + 0.5f, cx = (px < X0 + 0.5f ? px - X0 + 0.5f : px > X1 - 0.5f ? X1 - px + 0.5f : 1), cov;
            cov = (cx < cy ? cx : cy);
            if (R > 0.5f) {   /* the corners */
                float qx = px < X0 + R ? X0 + R - px : px > X1 - R ? px - (X1 - R) : 0;
                float qy = py < Y0 + R ? Y0 + R - py : py > Y1 - R ? py - (Y1 - R) : 0;
                if (qx > 0 && qy > 0) { float dd = R - sqrtf(qx * qx + qy * qy) + 0.5f; if (dd < cov) cov = dd; }
            }
            blend(i, j, c, cov);
        }
    }
}
static void d_frame(float x, float y, float w, float h, uint32_t c, float th, float rad)
{
    (void)rad;
    d_rect(x, y, w, th, c, 0); d_rect(x, y + h - th, w, th, c, 0);
    d_rect(x, y + th, th, h - 2 * th, c, 0); d_rect(x + w - th, y + th, th, h - 2 * th, c, 0);
}
/* a line of width th (a capsule) */
static void d_line(float x0, float y0, float x1, float y1, uint32_t c, float th)
{
    float X0 = x0 * S, Y0 = y0 * S, X1 = x1 * S, Y1 = y1 * S, r = th * S * 0.5f;
    float dx = X1 - X0, dy = Y1 - Y0, L2 = dx * dx + dy * dy;
    int ix0 = (int)floorf((X0 < X1 ? X0 : X1) - r - 1), ix1 = (int)ceilf((X0 > X1 ? X0 : X1) + r + 1);
    int iy0 = (int)floorf((Y0 < Y1 ? Y0 : Y1) - r - 1), iy1 = (int)ceilf((Y0 > Y1 ? Y0 : Y1) + r + 1), i, j;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > FW) ix1 = FW;
    if (iy1 > FH) iy1 = FH;
    for (j = iy0; j < iy1; j++)
        for (i = ix0; i < ix1; i++) {
            float px = i + 0.5f - X0, py = j + 0.5f - Y0, t = L2 > 0 ? (px * dx + py * dy) / L2 : 0, ex, ey, d;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            ex = px - t * dx; ey = py - t * dy;
            d = sqrtf(ex * ex + ey * ey);
            blend(i, j, c, r + 0.5f - d);
        }
}
/* a vertical span in device pixels at logical x (for waveforms and spectra) */
static void d_vspan_dev(int X, float y0, float y1, uint32_t c)
{
    float Y0 = (y0 < y1 ? y0 : y1) * S, Y1 = (y0 < y1 ? y1 : y0) * S;
    int j;
    if (Y1 - Y0 < 1) { float m = (Y0 + Y1) / 2; Y0 = m - 0.5f; Y1 = m + 0.5f; }
    for (j = (int)floorf(Y0); j < (int)ceilf(Y1); j++) {
        float cov = 1;
        if (j < Y0) cov = j + 1 - Y0;
        if (j + 1 > Y1) cov = Y1 - j;
        blend(X, j, c, cov);
    }
}
/* a ring segment (knobs): from angle a0 to a1 (radians, 0 = 3 o'clock, clockwise) */
static void d_arc(float cx, float cy, float rad, float th, float a0, float a1, uint32_t c)
{
    float CX = cx * S, CY = cy * S, R = rad * S, T = th * S * 0.5f;
    int ix0 = (int)(CX - R - T - 1), ix1 = (int)(CX + R + T + 2), iy0 = (int)(CY - R - T - 1), iy1 = (int)(CY + R + T + 2), i, j;
    for (j = iy0; j < iy1; j++)
        for (i = ix0; i < ix1; i++) {
            float px = i + 0.5f - CX, py = j + 0.5f - CY, d = sqrtf(px * px + py * py), a = atan2f(py, px), cov;
            cov = T + 0.5f - fabsf(d - R);
            if (cov <= 0) continue;
            while (a < a0) a += 2 * (float)M_PI;
            while (a > a0 + 2 * (float)M_PI) a -= 2 * (float)M_PI;
            if (a > a1) continue;
            blend(i, j, c, cov);
        }
}
static void d_circle(float cx, float cy, float rad, uint32_t c)
{
    float CX = cx * S, CY = cy * S, R = rad * S;
    int i, j;
    for (j = (int)(CY - R - 1); j < (int)(CY + R + 2); j++)
        for (i = (int)(CX - R - 1); i < (int)(CX + R + 2); i++) {
            float px = i + 0.5f - CX, py = j + 0.5f - CY;
            blend(i, j, c, R + 0.5f - sqrtf(px * px + py * py));
        }
}

/* ---- text ---- */
enum { F11, F13, F15, F18, F24 };
static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = p[0];
    if (c < 0x80) { *s += 1; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { *s += 2; return (c & 31) << 6 | (p[1] & 63); }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { *s += 3; return (c & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63); }
    *s += 1; return '?';
}
static int glyph_index(int c)
{
    int i;
    if (c >= 32 && c < 127) return c - 32;
    for (i = 95; i < XL_NGLYPH; i++) if ((int)XL_CODES[i] == c) return i;
    return '?' - 32;
}
static float d_textw(int font, const char *s)
{
    const xl_font *f = &XL_FONTS[0][font];
    float w = 0;
    while (*s) w += f->g[glyph_index(utf8_next(&s))].adv;
    return w;
}
/* align: 0 left, 1 center, 2 right; y is the baseline */
static float d_text(float x, float y, int font, uint32_t c, const char *s, int align)
{
    const xl_font *f = &XL_FONTS[S - 1][font];
    float w = d_textw(font, s), X;
    int Y;
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    X = x * S; Y = (int)lrintf(y * S) - f->ascent;
    while (*s) {
        const xl_glyph *g = &f->g[glyph_index(utf8_next(&s))];
        int gx = (int)lrintf(X) + g->xo, gy = Y + g->yo, i, j;
        for (j = 0; j < g->h; j++)
            for (i = 0; i < g->w; i++) {
                int a = f->a[(g->y + j) * f->aw + g->x + i];
                if (a) blend(gx + i, gy + j, c, a / 255.0f);
            }
        X += g->adv;
    }
    return w;
}
/* text cut with an ellipsis to fit maxw */
static void d_text_fit(float x, float y, int font, uint32_t c, const char *s, float maxw)
{
    char buf[256];
    size_t n = strlen(s);
    if (d_textw(font, s) <= maxw) { d_text(x, y, font, c, s, 0); return; }
    if (n > sizeof buf - 4) n = sizeof buf - 4;
    memcpy(buf, s, n); buf[n] = 0;
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
        memcpy(buf + n, "\xE2\x80\xA6", 4);
        if (d_textw(font, buf) <= maxw) break;
    }
    d_text(x, y, font, c, buf, 0);
}
