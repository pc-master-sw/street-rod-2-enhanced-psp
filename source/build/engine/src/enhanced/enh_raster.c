#ifdef STREET_ROD_GE
#include "engine_road_ge.h"
#endif
/* Street Rod 2 Enhanced: sample buffer, polygon fill, resolve through the live palette (ENHANCED.md §3.4).
 * The buffer covers the road view rows ENH_VIEW_Y0 .. ENH_VIEW_Y1 - 1 (the original's view clip) at S = res_scale * aa
 * samples per logical pixel, one palette index per sample. Coordinates given to the fill functions are logical
 * (320 x 200 screen) floats: pixel (x, y) covers [x, x + 1) x [y, y + 1); a sample is inside a polygon when its centre
 * is. Fills are cut to the clip rectangle (ras_clip; the view, or the mirror) and written through a screen door of
 * coverage ras_alpha (1 = opaque; the fade-in of far objects). The resolve averages the aa x aa samples of an output pixel in linear light.
 * Haze (S2.9, §4 item 5): a sample's high nibble is its haze level (ras_haze, 0..15); the resolve blends its colour
 * towards the haze colour (ras_haze_target) by level / 15 x the strength, in linear light (extended colours). */
#include "enh_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform/ega.h"

static struct {
    int S, w, h;            /* samples per logical pixel, buffer size in samples */
    u8 *buf;
    int ci0, ci1, cj0, cj1; /* clip rectangle in samples (ras_clip) */
    int alpha;              /* screen-door coverage 0..16 (ras_alpha), 16 = opaque */
    u8 hz;                  /* haze level << 4 of the following fills (ras_haze) */
    u8 haze_idx;            /* haze colour (palette index) and strength 0..1 at level 15 (ras_haze_target) */
    float haze_k;
} rb;

/* 4 x 4 ordered-dither thresholds: a sample is written when its threshold is below the coverage (screen door; the
 * resolve's aa x aa average turns it into partial coverage) */
static const u8 bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

bool ras_begin(int S)
{
    int w = 320 * S, h = (ENH_VIEW_Y1 - ENH_VIEW_Y0) * S;
    if (S != rb.S || !rb.buf) {
        u8 *b = realloc(rb.buf, (size_t)w * h);
        if (!b) return false;
        rb.buf = b;
        rb.S = S;
        rb.w = w;
        rb.h = h;
    }
    ras_clip(0, ENH_VIEW_Y0, 320, ENH_VIEW_Y1);
    rb.alpha = 16;
    rb.hz = 0;
    return true;
}

void ras_clip(float x0, float y0, float x1, float y1)
{
    int S = rb.S;
    rb.ci0 = (int)lroundf(x0 * S); rb.ci1 = (int)lroundf(x1 * S);
    rb.cj0 = (int)lroundf((y0 - ENH_VIEW_Y0) * S); rb.cj1 = (int)lroundf((y1 - ENH_VIEW_Y0) * S);
    if (rb.ci0 < 0) rb.ci0 = 0;
    if (rb.ci1 > rb.w) rb.ci1 = rb.w;
    if (rb.cj0 < 0) rb.cj0 = 0;
    if (rb.cj1 > rb.h) rb.cj1 = rb.h;
}

void ras_alpha(float a) { rb.alpha = a >= 1.0 ? 16 : a <= 0.0 ? 0 : (int)lroundf(a * 16.0); }
void ras_haze(int level) { rb.hz = (u8)((level < 0 ? 0 : level > 15 ? 15 : level) << 4); }

void ras_haze_target(float y, float strength)
{
    rb.haze_k = strength;
    if (!rb.buf || strength <= 0) return;
    int j = (int)floorf((y - ENH_VIEW_Y0) * rb.S) + rb.S / 2, n[16] = { 0 }, best = 0;
    if (j < 0) j = 0;
    if (j >= rb.h) j = rb.h - 1;
    const u8 *r = rb.buf + (size_t)j * rb.w;
    for (int i = 0; i < rb.w; i++) n[r[i] & 15]++;
    for (int c = 1; c < 16; c++) if (n[c] > n[best]) best = c;
    rb.haze_idx = (u8)best;
}

static bool row_in(int j) { return j >= rb.cj0 && j < rb.cj1; }

int ras_S(void) { return rb.S; }
u8 *ras_row(int j) { return rb.buf + (size_t)j * rb.w; }

/* Pixel centers are sampled at i + 0.5. Clamp before converting to an integer. */
static int sample_limit(float edge, int low, int high)
{
    float value = edge - 0.5f;
    if (!(value > low)) return low;
    if (value >= high) return high;
    int whole = (int)value;
    return whole + (value > whole);
}

static void span(int j, float xa, float xb, u8 c)
{
    if (!row_in(j) || !rb.alpha) return;
    int left = sample_limit(xa, rb.ci0, rb.ci1);
    int right = sample_limit(xb, rb.ci0, rb.ci1);
    if (right <= left) return;
    u8 *row = rb.buf + (size_t)j * rb.w;
    c |= rb.hz;
    if (rb.alpha >= 16) {
        memset(row + left, c, (size_t)(right - left));
        return;
    }
    const u8 *threshold = bayer[j & 3];
    for (int i = left; i < right; i++)
        if (threshold[i & 3] < rb.alpha) row[i] = c;
}

void ras_rect(float x0, float y0, float x1, float y1, u8 c)
{
#ifdef STREET_ROD_GE
    if (sr_ge_recording) {
        if (x0 >= x1 || y0 >= y1) return;
        const float xy[] = {x0, y0, x1, y0, x1, y1, x0, y1};
        sr_ge_polygon(xy, 4, c, rb.ci0, rb.cj0 + ENH_VIEW_Y0, rb.ci1, rb.cj1 + ENH_VIEW_Y0);
        return;
    }
#endif
    if (!rb.buf) return;
    float S = rb.S;
    int j0 = (int)ceilf((y0 - ENH_VIEW_Y0) * S - 0.5), j1 = (int)ceilf((y1 - ENH_VIEW_Y0) * S - 0.5);
    if (j0 < 0) j0 = 0;
    if (j1 > rb.h) j1 = rb.h;
    for (int j = j0; j < j1; j++) span(j, x0 * S, x1 * S, c);
}

/* A w x h index picture stretched over [x0, x1) x [y0, y1) (logical coordinates), rows below `ymax` cut; nearest
 * texel per sample (the caller picks a picture at most one size step larger than the target, so no mips are needed). */
static void image(const u8 *px, int w, int h, float x0, float y0, float x1, float y1, float ymax, int trans);

void ras_image(const u8 *px, int w, int h, float x0, float y0, float x1, float y1, float ymax)
{
    image(px, w, h, x0, y0, x1, y1, ymax, -1);
}

void ras_sprite(const u8 *px, int w, int h, float x0, float y0, float x1, float y1, float ymax)
{
    image(px, w, h, x0, y0, x1, y1, ymax, ENH_TRANSPARENT);
}

static void image(const u8 *px, int w, int h, float x0, float y0, float x1, float y1, float ymax, int trans)
{
    if (!rb.buf || w <= 0 || h <= 0 || x1 <= x0 || y1 <= y0) return;
    if ((x1 - x0) * rb.S < 1.0 && (y1 - y0) * rb.S < 1.0) return;     /* under one sample (§4 item 3) */
    float S = rb.S, sl = x0 * S, sr = x1 * S, st = (y0 - ENH_VIEW_Y0) * S, sb = (y1 - ENH_VIEW_Y0) * S;
    float sm = (ymax - ENH_VIEW_Y0) * S;
    int i0 = (int)ceilf(sl - 0.5), i1 = (int)ceilf(sr - 0.5), j0 = (int)ceilf(st - 0.5), j1 = (int)ceilf(fminf(sb, sm) - 0.5);
    if (i0 < rb.ci0) i0 = rb.ci0;
    if (i1 > rb.ci1) i1 = rb.ci1;
    if (j0 < rb.cj0) j0 = rb.cj0;
    if (j1 > rb.cj1) j1 = rb.cj1;
    float du = w / (sr - sl), dv = h / (sb - st);
    for (int j = j0; j < j1; j++) {
        int v = (int)((j + 0.5 - st) * dv);
        if (v < 0 || v >= h) continue;
        const u8 *src = px + (size_t)v * w, *t = bayer[j & 3];
        u8 *out = rb.buf + (size_t)j * rb.w;
        for (int i = i0; i < i1; i++) {
            int u = (int)((i + 0.5 - sl) * du);
            if (u >= 0 && u < w && t[i & 3] < rb.alpha && src[u] != trans) out[i] = (u8)(src[u] | rb.hz);
        }
    }
}

void ras_sprite_rot(const u8 *px, int w, int h, float pu, float pv, float x, float y, float f, float c, float s,
                    float ymax)
{
    if (!rb.buf || w <= 0 || h <= 0 || f <= 0) return;
    if (w * f * rb.S < 1.0 && h * f * rb.S < 1.0) return;           /* under one sample */
    float S = rb.S, xmin = 1e30, xmax = -1e30, ymn = 1e30, ymx = -1e30;
    for (int k = 0; k < 4; k++) {                           /* the rotated picture's bounding box */
        float U = ((k & 1) ? w : 0) - pu, V = ((k & 2) ? h : 0) - pv;
        float X = x + f * (c * U + s * V), Y = y + f * (-s * U + c * V);
        xmin = fminf(xmin, X); xmax = fmaxf(xmax, X); ymn = fminf(ymn, Y); ymx = fmaxf(ymx, Y);
    }
    if (!isfinite(xmin) || !isfinite(ymn)) return;
    int i0 = (int)floorf(xmin * S), i1 = (int)ceilf(xmax * S);
    int j0 = (int)floorf((ymn - ENH_VIEW_Y0) * S), j1 = (int)ceilf((fminf(ymx, ymax) - ENH_VIEW_Y0) * S);
    if (i0 < rb.ci0) i0 = rb.ci0;
    if (i1 > rb.ci1) i1 = rb.ci1;
    if (j0 < rb.cj0) j0 = rb.cj0;
    if (j1 > rb.cj1) j1 = rb.cj1;
    float k = 1.0 / f;
    for (int j = j0; j < j1; j++) {
        float Y = (j + 0.5) / S + ENH_VIEW_Y0 - y;
        if (Y + y >= ymax) break;
        const u8 *t = bayer[j & 3];
        u8 *out = rb.buf + (size_t)j * rb.w;
        for (int i = i0; i < i1; i++) {
            float X = (i + 0.5) / S - x;
            int u = (int)floorf((c * X - s * Y) * k + pu), v = (int)floorf((s * X + c * Y) * k + pv);
            if (u < 0 || u >= w || v < 0 || v >= h || t[i & 3] >= rb.alpha) continue;
            u8 p = px[(size_t)v * w + u];
            if (p != ENH_TRANSPARENT) out[i] = (u8)(p | rb.hz);
        }
    }
}

/* A convex polygon of n points (x, y pairs, logical coordinates); a non-convex one fills its rows' extents. */
/* Visit only clipped scanlines. One slope per edge replaces divisions and
 * endpoint sorting on every scanline. Concave shapes still fill their extents. */
void ras_poly(const float *xy, int n, u8 c)
{
#ifdef STREET_ROD_GE
    if (sr_ge_recording) {
        sr_ge_polygon(xy, n, c, rb.ci0, rb.cj0 + ENH_VIEW_Y0, rb.ci1, rb.cj1 + ENH_VIEW_Y0);
        return;
    }
#endif
    if (!rb.buf || !rb.alpha || n < 3 || n > 16) return;
    float x[16], y[16];
    for (int k = 0; k < n; k++) {
        x[k] = xy[2 * k] * rb.S;
        y[k] = (xy[2 * k + 1] - ENH_VIEW_Y0) * rb.S;
        if (!isfinite(x[k]) || !isfinite(y[k])) return;
    }

    struct Edge {
        int first, end;
        float x, y, slope;
    } edges[16];
    int count = 0, first = rb.cj1, end = rb.cj0;
    for (int k = 0; k < n; k++) {
        int a = k, b = k + 1 == n ? 0 : k + 1;
        if (y[a] > y[b]) { int swap = a; a = b; b = swap; }
        int lo = sample_limit(y[a], rb.cj0, rb.cj1);
        int hi = sample_limit(y[b], rb.cj0, rb.cj1);
        if (hi <= lo) continue;
        edges[count++] = (struct Edge){lo, hi, x[a], y[a], (x[b] - x[a]) / (y[b] - y[a])};
        if (lo < first) first = lo;
        if (hi > end) end = hi;
    }
    for (int row = first; row < end; row++) {
        float left = 1e30f, right = -1e30f;
        for (int k = 0; k < count; k++) {
            const struct Edge *edge = &edges[k];
            if (row < edge->first || row >= edge->end) continue;
            float column = edge->x + (row + 0.5f - edge->y) * edge->slope;
            if (column < left) left = column;
            if (column > right) right = column;
        }
        if (right > left) span(row, left, right, c);
    }
}

/* ---------------------------------------------------------------- colour */

u32 enh_pal_rgb(u8 v) { return ega_palette[(v & 7) | (v >> 1 & 8)]; }     /* 200-line attribute decoding */

static u16 lin_of[256];             /* sRGB byte -> linear 0..4095 */
static u8 srgb_of[4096];            /* linear -> sRGB byte */

static void gamma_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0;
        c = c <= 0.04045 ? c / 12.92 : powf((c + 0.055) / 1.055, 2.4);
        lin_of[i] = (u16)lroundf(c * 4095.0);
    }
    for (int i = 0; i < 4096; i++) {
        float c = i / 4095.0;
        c = c <= 0.0031308 ? c * 12.92 : 1.055 * powf(c, 1 / 2.4) - 0.055;
        srgb_of[i] = (u8)lroundf(c * 255.0);
    }
}

/* Output pixels of rows ENH_VIEW_Y0 * rs .. ENH_SPLIT_Y * rs - 1 whose logical pixel has cover[] set (cover: 320 per
 * logical row from ENH_VIEW_Y0) get the aa x aa samples averaged in linear light through palette `pal`. */
void ras_resolve(const u8 pal[17], const u8 *cover, u32 *out, int rs, int aa)
{
    if (!rb.buf || rb.S != rs * aa) return;
    if (rs == 1 && aa == 1 && rb.haze_k <= 0) {
        u32 colors[16];
        for (int color = 0; color < 16; color++) colors[color] = enh_pal_rgb(pal[color]);
        out += ENH_VIEW_Y0 * 320;
        for (int y = ENH_VIEW_Y0; y < ENH_SPLIT_Y; y++) {
            const u8 *row = ras_row(y - ENH_VIEW_Y0);
            for (int x = 0; x < 320; x++)
                if (cover[x]) out[x] = colors[row[x] & 15];
            out += 320;
            cover += 320;
        }
        return;
    }
    gamma_init();
    /* entry = colour | haze level << 4 */
    u32 rgb[256];
    u16 lr[256], lg[256], lb[256];
    u32 hc = enh_pal_rgb(pal[rb.haze_idx & 15]);
    float hr = lin_of[hc >> 16 & 0xFF], hg = lin_of[hc >> 8 & 0xFF], hb = lin_of[hc & 0xFF];
    int levels = rb.haze_k > 0 ? 16 : 1;
    for (int i = 0; i < 16 * levels; i++) {
        u32 c = enh_pal_rgb(pal[i & 15]);
        float a = rb.haze_k * (i >> 4) / 15.0;
        lr[i] = (u16)lroundf(lin_of[c >> 16 & 0xFF] * (1 - a) + hr * a);
        lg[i] = (u16)lroundf(lin_of[c >> 8 & 0xFF] * (1 - a) + hg * a);
        lb[i] = (u16)lroundf(lin_of[c & 0xFF] * (1 - a) + hb * a);
        rgb[i] = i < 16 ? c : (u32)srgb_of[lr[i]] << 16 | (u32)srgb_of[lg[i]] << 8 | srgb_of[lb[i]];
    }
    for (int i = 16 * levels; i < 256; i++) { lr[i] = lr[i & 15]; lg[i] = lg[i & 15]; lb[i] = lb[i & 15]; rgb[i] = rgb[i & 15]; }
    int ow = 320 * rs, n = aa * aa;
    for (int oy = ENH_VIEW_Y0 * rs; oy < ENH_SPLIT_Y * rs; oy++) {
        const u8 *cov = cover + (oy / rs - ENH_VIEW_Y0) * 320;
        u32 *o = out + (size_t)oy * ow;
        int sj = (oy - ENH_VIEW_Y0 * rs) * aa;
        for (int ox = 0; ox < ow; ox++) {
            if (!cov[ox / rs]) continue;
            if (aa == 1) { o[ox] = rgb[ras_row(sj)[ox]]; continue; }
            u32 r = 0, g = 0, b = 0;
            for (int dj = 0; dj < aa; dj++) {
                const u8 *s = ras_row(sj + dj) + ox * aa;
                for (int di = 0; di < aa; di++) {
                    u8 c = s[di];
                    r += lr[c]; g += lg[c]; b += lb[c];
                }
            }
            o[ox] = (u32)srgb_of[r / n] << 16 | (u32)srgb_of[g / n] << 8 | srgb_of[b / n];
        }
    }
}

/* The view at logical pixels, rows ENH_VIEW_Y0 .. Y1 - 1: the sample at each pixel's centre. (The most frequent index
 * of the S x S block gives the same edges on large areas but noise on the textured sky panorama.) */
void ras_downscale(u8 *idx)
{
    if (!rb.buf) return;
    int S = rb.S;
    for (int y = 0; y < ENH_VIEW_Y1 - ENH_VIEW_Y0; y++) {
        const u8 *s = ras_row(y * S + S / 2);
        for (int x = 0; x < 320; x++) idx[y * 320 + x] = s[x * S + S / 2] & 15;
    }
}
