#ifdef STREET_ROD_GE
#include "engine_road_ge.h"
#endif
/* Street Rod 2 Enhanced: float road geometry and sky of the normal renderer (ENHANCED.md §3.2, §3.5).
 * S2.3 (Milestone A): the town cruise and the street drag race (road modes 0, 1), since S2.9's first part at
 * --draw-distance N segments (the original's 14; default 60, §4; S2.9). The road is projected in float with the formulas of road_proj_x / road_proj_y (race_road.c) at the
 * interpolated view, each segment cut into slices so that YAW / HGT / edges vary continuously, and emitted far to
 * near in the original's painter's order (road_draw_front: town walls, then the band of each row, then the row's
 * road-side objects, enh_sprite.c, S2.5). The opponent / police car, the drag strip's start tree and the starter
 * are drawn from their native pictures (enh_props.c, S2.5). The mirror (S2.6, §3.7) is the same renderer with road_proj_x_mirror /
 * road_proj_y_mirror (1/4 of the front's scale, centre 280, 36), 7 rows behind, clipped to (240, 22)-(319, 42), over
 * the original's backdrop pictures; frame lines redrawn.
 *
 * S2.4 adds the drag strip (modes 2, 4: flags L2 / R2 / C2, YAW / HGT / banks at seg + 0xC0, flat, barrier walls) and
 * Mulholland (mode 0x10: banks BL / BR, hillsides, verges, guard rails, trusses, its own sky, outer band 200); their
 * mirror is the same renderer (S2.6: hillsides, verges, rails, trusses, sky strip).
 *
 * World: a position Pw along the road in sub-steps (64 per segment), lateral x and height y in road units (road
 * 0x5DC..0xCE4, surface y 2000, eye 0x76C). Depth z = (Pw - P) / 8 + 4, so segment seg + k lies at the original's row
 * depth 8k + 4 - substep / 8 (without the original's truncation of substep / 8; the compare quantizes P). Row 0
 * (z = 4) is the near clip; the far end is z = 4 + 112 x scale (116 at 1: the original pins row 14 there). Segment s carries the flags
 * L/R/C[s] for the band from boundary s - 1 to boundary s, and the edges, YAW and HGT of boundary s.
 *
 * Stage 2 (S2.9, §4): past the arrays the track is flat and straight (YAW / HGT clamped), without banks and flags but
 * the centre dashes of the course builder's pattern; the far road of a level stretch stops at the horizon (Z_HORIZON),
 * far segments are hazed (raster haze levels), objects fade in over the last 10 %, the opponent ahead beyond the
 * original's 14 rows is drawn from its rear picture, and the town's street ends in a closing house front. The mirror
 * reaches min(N / 2, 30) rows behind (the car behind beyond its 7 rows from its front picture), objects fading in. */
#include "enh_internal.h"
#include "enhanced.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/race_int.h"
#include "platform/ega.h"

/* The original truncates its divisions (cdiv16): x towards the screen centre, y down to the row above; its spans
 * include both end pixels and a band covers the rows from its far row to the row above its near row. Projected values
 * are moved by XOFF / YOFF into the continuous pixel coordinates of the raster (a sample is inside when its centre is),
 * so that the edges land on the original's pixels. */
#define XOFF    0.5
#define YOFF  (-0.4)           /* -0.5, less a margin: an exact row value (the eye level 60) stays on its row */
#define SKY_RATE (1.0 / 8.0)    /* sky scroll: 1/8 of the original's 2 px (panorama 4 px) per heading unit (§3.5) */
#define FAR_Z   116.0           /* the original's row 14: the far end at --draw-distance 14 */
#define Z_HORIZON 340.0         /* a level road meets the sky panorama's horizon at this depth (S2.9 part 1) */
#define FAR_FADE 2.0            /* far banks / ground: how fast their rise over the horizon shrinks with depth */
#define FAR_SLOPE 3.3           /* far banks / ground: width / height of their slopes (the near banks' look) */
#define TOWN_END 0xB3           /* the last town segment (track_build_all: town 0 .. 0xB3, race road from 0xB4) */
#define WATER   8               /* Mulholland: under the bridges, palette entry 8 (dark cyan, fixed; 6/7 are the paint) */
#define SURFACE 2000.0
#define EYE     1900.0          /* 0x76C */

typedef struct {
    float P;                   /* player position, sub-steps */
    float cx, cy, hgt, heading, bounce;
    int mode, seg0;             /* seg0 = floorf(P / 64) */
    bool racing, compare;
    int start_seg;
    float w;                   /* outer band width: 400 (200 Mulholland) */
    bool mirror;                /* the mirror's projection (proj_x / proj_y) */
    float far_z;               /* the front's far end: 4 + 112 x --draw-distance-scale (z = 116 at 1) */
    int rows;                   /* enh_view.rows (compare aid) */
    /* stage 2 table ends (ENHANCED.md §4 item 2), live view only: segments from `synth` on are plain road (no objects) */
    int synth;
    int fall_dir;               /* DS_fall_dir: -1, 0, 1 (the hillsides' side offset, Mulholland) */
    bool l16;                   /* Mulholland: a bridge ends within rows 10..14 (front_hillsides' free-standing peaks) */
    const enh_snap *snap;       /* the newer snapshot: the car's level, the starter's segment */
    float O, lat;              /* the car: position in sub-steps, lateral position (interpolated) */
} cam;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* Past the arrays (§4 item 2): no flags but the centre dashes, every 4th (8th on a slow machine) segment as
 * track_build_all's course (the strip: C2 always has them), no banks. */
u16 enh_flag(u16 arr, int s)
{
    if (arr == TRK_LANE) return s + enh_toff >= TRK_N ? 0 : TRKW(arr, clampi(s + enh_toff, 0, TRK_N - 1));
    if (enh_toff) {                                             /* drag strip: its own flag arrays at the segment */
        if (s > 0x73) return arr == TRK_C ? 8 : 0;
        arr = arr == TRK_L ? TRK_L2 : arr == TRK_R ? TRK_R2 : arr == TRK_C ? TRK_C2 : arr;
        return TRKW(arr, clampi(s, 0, 0x73));
    }
    if (s >= TRK_N) return arr == TRK_C && (s + 1) % ((DSS(DS_slow_machine) + 1) * 4) == 0 ? 8 : 0;
    return TRKW(arr, clampi(s, 0, TRK_N - 1));
}

s16 enh_bank(u16 arr, int s) { return s + enh_toff >= TRK_N ? 0 : TRKS(arr, clampi(s + enh_toff, 0, TRK_N - 1)); }

static u16 flag(u16 arr, int s) { return enh_flag(arr, s); }

/* bank height (BL / BR) at a continuous position, linear between the boundaries */
static float bank_at(u16 arr, float Pw)
{
    float f = floorf(Pw / 64.0);
    int i = (int)f;
    float v0 = enh_bank(arr, i), v1 = enh_bank(arr, i + 1);
    return v0 + (v1 - v0) * (Pw / 64.0 - f);
}

/* The front view's track: past the street drag race's finish the table holds the road course (curves, hills, its
 * objects); the live view shows plain road there: from segment `synth` (track_end) on only the centre dashes (C & 8,
 * the course's dash pattern) are kept. YAW / HGT stay the table's (the course's first hill): the original places the
 * opponent waiting past the finish (its pixels, passthrough) at those heights, so car and road agree. The compare view
 * and the mirror read the table as the original. */
static u16 cflag(const cam *c, u16 arr, int s) { return s < c->synth ? flag(arr, s) : arr == TRK_C ? flag(TRK_C, s) & 8 : 0; }

/* ---------------------------------------------------------------- mirror projection (road_project_mirror) */

#define MIR_X0   0xF0           /* clip_set_mirror: x 240..319, rows 22..42 */
#define MIR_Y0   0x16
#define MIR_Y1   0x2B
#define MIR_NEAR 8.0            /* row 0 of road_project_mirror */

/* The mirror's depth of Pw (behind the car): continuous (P - Pw) / 8, at least 8 (the original's row 0); comparing
 * (Pw a boundary) the original's: row 0: 8, rows 1..4: 8k + q, rows 5..7: 8k (the k >= 5 step, §3.7). */
static float mir_z(const cam *c, float Pw)
{
    if (!c->compare) return fmaxf((c->P - Pw) / 8.0, MIR_NEAR);
    int k = c->seg0 - (int)lroundf(Pw / 64.0), q = (int)fmodf(c->P, 64.0) >> 3;
    return k <= 0 ? 8.0 : k <= 4 ? 8.0 * k + q : 8.0 * k;
}

static float mir_dx(const cam *c, float Pw) { return enh_track_yaw(Pw) - c->heading; }
static float mir_dy(const cam *c, float Pw) { return (c->mode & 6) ? 0.0 : enh_track_hgt(Pw) - c->hgt; }

static float proj_x(const cam *c, float Pw, float xw)
{
    if (c->mirror) return 280.0 + mir_dx(c, Pw) / 4.0 - (c->cx - xw) / mir_z(c, Pw) + XOFF;
    float z = (Pw - c->P) / 8.0 + 4.0;
    float dx = c->heading - enh_track_yaw(Pw);
    return 160.0 + dx - 4.0 * (c->cx - xw) / z + XOFF;
}

static float proj_y(const cam *c, float Pw, float yw)
{
    if (c->mirror) return 36.0 + mir_dy(c, Pw) + (yw - c->cy) / mir_z(c, Pw) + YOFF;
    float z = (Pw - c->P) / 8.0 + 4.0;
    float dy = (c->mode & 6) ? 0.0 : c->hgt - enh_track_hgt(Pw);
    return 2.0 * dy + 4.0 * (yw - c->cy) / z + c->bounce + 60.0 + YOFF;
}

/* road edges of boundary s (edges_of in race_road.c: C & 0xE0) */
static void edges_at(const cam *c, int s, float *xl, float *xr)
{
    u16 cm = cflag(c, TRK_C, s) & 0xE0;
    if (cm == 0)        { *xl = 0x5DC; *xr = 0xCE4; }
    else if (cm & 0x80) { *xl = 0x7D0; *xr = 0xAF0; }
    else if (cm & 0x20) { *xl = 0xCE4; *xr = 0xCE4; }      /* one-sided closures (zero width) */
    else                { *xl = 0x5DC; *xr = 0x5DC; }
}

/* The projected values of one cross-section at Pw. */
typedef struct { float Pw, xl, xr, xol, xor_, y, ye, yk, yt1, yt2; } sect;

static sect section(const cam *c, float Pw)
{
    sect p;
    float s1 = ceilf(Pw / 64.0), f = (Pw - (s1 - 1) * 64.0) / 64.0, l0, r0, l1, r1;
    edges_at(c, (int)s1 - 1, &l0, &r0);
    edges_at(c, (int)s1, &l1, &r1);
    p.Pw = Pw;
    p.xl = proj_x(c, Pw, l0 + (l1 - l0) * f);
    p.xr = proj_x(c, Pw, r0 + (r1 - r0) * f);
    p.xol = proj_x(c, Pw, 0x5DC - c->w);
    p.xor_ = proj_x(c, Pw, 0xCE4 + c->w);
    p.y = proj_y(c, Pw, SURFACE);
    p.ye = proj_y(c, Pw, EYE);
    p.yk = proj_y(c, Pw, 0x79E);
    p.yt1 = c->mode == 0x10 ? proj_y(c, Pw, SURFACE - bank_at(TRK_BL, Pw)) : p.y;
    p.yt2 = c->mode == 0x10 ? proj_y(c, Pw, SURFACE - bank_at(TRK_BR, Pw)) : p.y;
    return p;
}

static void quad(u8 col, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
    float p[8] = { x0, y0, x1, y1, x2, y2, x3, y3 };
    ras_poly(p, 4, col);
}

/* a band from x = a to x = b between the far section f and the near section n (road plane) */
static void band_quad(u8 col, float af, float bf, float an, float bn, const sect *f, const sect *n)
{
    quad(col, af, f->y, bf, f->y, bn, n->y, an, n->y);
}

/* ---------------------------------------------------------------- town walls (town_walls_front, racing off) */

static void town_walls(const cam *c, int s, const sect *p, int n, bool left)
{
    u16 F = cflag(c, left ? TRK_L : TRK_R, s), Fm1 = cflag(c, left ? TRK_L : TRK_R, s - 1);
    u16 C = cflag(c, TRK_C, s), Cm1 = cflag(c, TRK_C, s - 1);
    if (C & 4) return;                                          /* intersection */
    int k = (F & 8) ? 1 : 2;
    float top = EYE - (SURFACE - EYE) / k;                     /* the tall part */
    for (int i = 0; i < n; i++) {
        const sect *a = &p[i], *b = &p[i + 1];                  /* near, far */
        float xa = left ? a->xol : a->xor_, xb = left ? b->xol : b->xor_;
        /* poly_fill includes the bottom row (the bands' spans do not): bottom edges one row lower */
        quad((F & 0x10) ? 2 : 3, xa, a->y + 1, xb, b->y + 1, xb, b->ye, xa, a->ye);
        if (F & 4)
            quad(4, xa, a->ye + 1, xb, b->ye + 1, xb, proj_y(c, b->Pw, top), xa, proj_y(c, a->Pw, top));
    }
    const sect *nr = &p[0], *fr = &p[n];
    float xn = left ? nr->xol : nr->xor_, xf = left ? fr->xol : fr->xor_;
    if (Cm1 & 4) {                                              /* the end face towards the intersection */
        float x0 = left ? enh_front_edge(true) : xn, x1 = left ? xn : enh_front_edge(false);
        ras_rect(x0, nr->ye, x1, nr->y - 1.0, 2);
        ras_rect(x0, nr->y - 1.0, x1, nr->y, 4);
    }
    if (!(F & 4) || (Fm1 & 4)) return;
    /* the end face of the tall part (a screen-space rectangle as wide as the wall, as the original) */
    float h = (nr->y - nr->ye) / k, wd = fabsf(xn - xf);
    if (left) ras_rect(xn - wd, nr->ye - h, xn, nr->ye, 2);
    else      ras_rect(xn, nr->ye - h, xn + wd, nr->ye, 2);
}

/* ---------------------------------------------------------------- road bands (road_draw_band_front) */

/* dash width in pixels (table DS:7E52, index = row): constant per band when comparing, else continuous in depth */
static float dash_width(const cam *c, int kband, float Pw)
{
    if (c->compare) return DSS((u16)(DS_dash_width + 2 * clampi(kband, 0, 24)));
    float kf = (Pw - c->P) / 64.0 + 0.5;
    int k0 = clampi((int)floorf(kf), 0, 24), k1 = clampi(k0 + 1, 0, 24);
    float f = fminf(fmaxf(kf - floorf(kf), 0.0), 1.0);
    s16 w0 = DSS((u16)(DS_dash_width + 2 * k0)), w1 = DSS((u16)(DS_dash_width + 2 * k1));
    return w0 + (w1 - w0) * f;
}

/* Mulholland's hill tables (DS:7E1E..: width, height per row; the extension E at 7E3A) read at a continuous index */
static float tabv(u16 addr, float m, int off)
{
    m = fminf(fmaxf(m, 1.0), 16.0);
    int i = (int)floorf(m), j = i < 16 ? i + 1 : 16;
    float f = m - i;
    s16 a = DSS((u16)(addr + 4 * i + off)), b = DSS((u16)(addr + 4 * j + off));
    return a + (b - a) * f;
}

static float rowf(const cam *c, float Pw) { return Pw / 64.0 - c->seg0; }     /* the boundary's row, continuous */

/* Mulholland and the street drag beyond the original's rows: the ground of a road higher than the car's level horizon h0
 * (a hill ahead) is not a full-width slab over the mountain panorama but a mound: below h0 it is full width, above it the
 * earth under the road and its flanks fall from the road edges to h0 at FAR_SLOPE, narrowed by (FAR_Z / z)^FAR_FADE as
 * the far banks' rise, so the far road always stands on ground (grass beside a flat bank, else the hillside colour).
 * Returns h0, or -1. */
static float ground_h0(const cam *c, const sect *a)
{
    if (c->compare || c->mirror || !c->racing || (c->mode != 0x10 && c->mode != 1) || (a->Pw - c->P) / 8.0 + 4.0 <= FAR_Z)
        return -1.0;
    return 4.0 * (SURFACE - c->cy) / Z_HORIZON + c->bounce + 60.0 + YOFF;
}

#define MOUND_MIN_SLOPE 1.5     /* street drag: a far hill's flank width / height at least */
static float flank(const cam *c, const sect *p, float h0)
{
    /* the street drag's open hills are never steeper than MOUND_MIN_SLOPE: narrowed by FAR_FADE alone a tall far hill
     * became a pair of green spikes, while a low one (the start's gantry hill) stays narrow below the mountains */
    float h = fmaxf(h0 - p->y, 0.0), w = FAR_SLOPE * h * powf(FAR_Z / ((p->Pw - c->P) / 8.0 + 4.0), FAR_FADE);
    return c->mode == 1 ? fmaxf(w, MOUND_MIN_SLOPE * h) : w;
}

/* the street drag's hills are open country, not Mulholland's banks: a near hill's ground is full width as in the
 * original's rows (z <= FAR_Z), so its mound's top widens by up to MOUND_FULL beside the road, from 0 at
 * MOUND_BLEND * FAR_Z to the whole view at FAR_Z (no green wedges with sky between them and the near ground when the hill
 * comes close; a hill farther away stays a mound, the start's gantry hill no slab over the mountains) */
#define MOUND_FULL 400.0
#define MOUND_BLEND 1.25
static float mound_wide(const cam *c, float Pw)
{
    if (c->mode != 1) return 0.0;
    float z = (Pw - c->P) / 8.0 + 4.0;
    return MOUND_FULL * fminf(fmaxf((MOUND_BLEND * FAR_Z - z) / ((MOUND_BLEND - 1.0) * FAR_Z), 0.0), 1.0);
}

/* the mounds of segment s's slices, before its bands (a slice's mound reaches below its nearer slices' road) */
static void mounds(const cam *c, const sect *p, int n, float hclip, bool flat_l, bool flat_r)
{
    for (int i = 0; i < n; i++) {
        const sect *a = &p[i], *b = &p[i + 1];
        float h0 = ground_h0(c, a);
        if (b->y >= a->y || h0 <= hclip || fminf(a->y, b->y) >= h0) continue;
        float fb = flank(c, b, h0), fa = flank(c, a, h0), eb = mound_wide(c, b->Pw), ea = mound_wide(c, a->Pw);
        float bl = b->xol - eb, br = b->xor_ + eb, al = a->xol - ea, ar = a->xor_ + ea;
        u8 cl = flat_l ? 9 : 0x0E, cr = flat_r ? 9 : 0x0E;
        ras_clip(0, hclip, 320, h0);
        quad(flat_l && flat_r ? 9 : 0x0E, bl, b->y, br, b->y, br, h0, bl, h0);
        quad(cl, bl, b->y, bl - fb, h0, al - fa, h0, al, a->y);
        quad(cr, br, b->y, ar, a->y, ar + fa, h0, br + fb, h0);
        ras_clip(0, hclip, 320, ENH_VIEW_Y1);
    }
}

/* The town's cross streets (intersection segments, C & 5), live view: the original draws nothing there but a one-row kerb
 * under the far corner's face, so the near corner wall seemed to stand on the road. Each side of the cross street gets
 * a sidewalk CROSS_WALK deep, from the main road's edges out to the screen edges (the cross street runs across the
 * view, so its edges are rows); its own road between stays the background's road colour. */
#define CROSS_WALK 40.0         /* sub-steps (64 per segment): a cross street is 3 segments, its road keeps 7 / 12 of it */

static bool crossing(const cam *c, int s) { return (cflag(c, TRK_C, s) & 5) == 5; }

static void cross_walk(const cam *c, float Pa, float Pb)
{
    if (Pb <= Pa) return;
    sect a = section(c, Pa), b = section(c, Pb);                /* near, far */
    if (b.y >= a.y) return;
    float left = enh_front_edge(true), right = enh_front_edge(false);
    band_quad(4, left, b.xl, left, a.xl, &b, &a);
    band_quad(4, b.xr, right, a.xr, right, &b, &a);
}

static void cross_street(const cam *c, int s, const sect *p, int n)
{
    float lo = p[0].Pw, hi = p[n].Pw, P0 = (s - 1) * 64.0, P1 = s * 64.0;
    if (!crossing(c, s + 1)) cross_walk(c, fmaxf(P1 - CROSS_WALK, lo), fminf(P1, hi));      /* the far sidewalk */
    if (!crossing(c, s - 1)) cross_walk(c, fmaxf(P0, lo), fminf(P0 + CROSS_WALK, hi));      /* the near sidewalk */
}

/* the outer edge of a bridge's water beside the deck edge x at p (sg -1 left, +1 right): the screen edge up to the
 * original's far row; beyond it the water narrows to the deck and is gone at 2 x FAR_Z, so a far bridge's water no
 * longer runs as a full-width line along the horizon */
static float water_edge(const cam *c, const sect *p, float x, int sg)
{
    float z = (p->Pw - c->P) / 8.0 + 4.0, e = sg < 0 ? -1.0 : 321.0;
    if (z <= FAR_Z) return e;
    float t = fminf((z - FAR_Z) / FAR_Z, 1.0);
    return x + (e - x) * (1.0 - t * t * (3.0 - 2.0 * t));
}

static void band(const cam *c, int s, int kband, const sect *p, int n, float hclip)
{
    u16 C = cflag(c, TRK_C, s), L = cflag(c, TRK_L, s), R = cflag(c, TRK_R, s);
    bool cross = C & 1;
    if (cross && (C & 4)) {                                     /* intersection: nothing drawn (live: its sidewalks) */
        if (!c->compare && !c->racing && !c->mirror) cross_street(c, s, p, n);
        return;
    }
    bool wide = (c->mode & 6) && s < c->start_seg + 5;
    bool mh = c->mode == 0x10;
    /* Mulholland: the verge strips (colour 2) while the car is not far below the road, the ground (9) beside the road
     * only where the bank in front is flat, and the white finish band from segment 0x3AD */
    bool verge = mh && !(C & 0x80) && enh_track_hgt(c->seg0 * 64.0) - 0x0B <= c->hgt;
    bool flat_l = !mh || enh_bank(TRK_BL, s - 1) == 0, flat_r = !mh || enh_bank(TRK_BR, s - 1) == 0;
    bool white = (C & 0x10) && (!mh || c->seg0 >= 0x3AD);
    /* Mulholland's bridges (C & 0x80) span water: live, the space beside and under the deck is filled with it (the
     * original leaves the sky picture there) */
    bool water = mh && (C & 0x80) && !c->compare;
    if (!cross && !(C & 0x80)) mounds(c, p, n, hclip, flat_l, flat_r);    /* Mulholland, street drag (ground_h0) */
    for (int i = 0; i < n; i++) {
        const sect *a = &p[i], *b = &p[i + 1];                  /* near, far */
        if (b->y >= a->y) continue;                             /* faces away (behind a crest) */
        if (!cross) {
            if (verge) {
                band_quad(2, b->xol, b->xl, a->xol, a->xl, b, a);
                band_quad(2, b->xr, b->xor_, a->xr, a->xor_, b, a);
            }
            float lb = enh_front_edge(true), la = lb, rb = enh_front_edge(false), ra = rb;
            /* beside the far rows the ground reaches `ext` only (the original; live, where water fills the bridge's sides
             * and the road goes on past row 14, the ground stays full width: no strip of mountains above the water and
             * no far road over the panorama) */
            if (mh && c->l16 && c->compare && s - c->seg0 >= 10) {
                float eb = tabv(DS_hill_peak_tab, 17.0 - rowf(c, b->Pw), 4), ea = tabv(DS_hill_peak_tab, 17.0 - rowf(c, a->Pw), 4);
                lb = b->xl - eb; la = a->xl - ea; rb = b->xr + eb; ra = a->xr + ea;
            }
            if (water) {
                band_quad(WATER, water_edge(c, b, b->xl, -1), b->xl, water_edge(c, a, a->xl, -1), a->xl, b, a);
                band_quad(WATER, b->xr, water_edge(c, b, b->xr, 1), a->xr, water_edge(c, a, a->xr, 1), b, a);
            } else {
                float h0 = ground_h0(c, a);                    /* above h0 the ground is the mound (mounds) */
                bool mound = (flat_l || flat_r) && h0 > hclip && fminf(a->y, b->y) < h0;                if (mound) ras_clip(0, h0, 320, ENH_VIEW_Y1);
                if (flat_l) band_quad(9, lb, b->xl, la, a->xl, b, a);
                if (flat_r) band_quad(9, b->xr, rb, a->xr, ra, b, a);
                if (mound) ras_clip(0, hclip, 320, ENH_VIEW_Y1);
            }
            band_quad(white ? 0x0F : 1, b->xl, b->xr, a->xl, a->xr, b, a);
        } else {
            if (!(L & 0x80)) band_quad(4, b->xol, b->xl, a->xol, a->xl, b, a);
            else {
                float mb = (b->xol + b->xl) / 2, ma = (a->xol + a->xl) / 2;
                band_quad(4, b->xol, mb, a->xol, ma, b, a);
                band_quad(9, mb, b->xl, ma, a->xl, b, a);
            }
            if (!(R & 0x80)) band_quad(4, b->xr, b->xor_, a->xr, a->xor_, b, a);
            else {
                float mb = (b->xor_ + b->xr) / 2, ma = (a->xor_ + a->xr) / 2;
                band_quad(4, mb, b->xor_, ma, a->xor_, b, a);
                band_quad(9, b->xr, mb, a->xr, ma, b, a);
            }
        }
        if ((C & 8) && b->xl < enh_front_edge(false) - 2.0f) {
            float wb = dash_width(c, kband, b->Pw), wa = dash_width(c, kband, a->Pw);
            if (wide) { wb *= 8; wa *= 8; }
            float cb = (b->xl + b->xr) / 2, ca = (a->xl + a->xr) / 2;
            band_quad(wide ? 2 : 4, cb - (wb + 1) / 2, cb + (wb + 1) / 2, ca - (wa + 1) / 2, ca + (wa + 1) / 2, b, a);
        }
    }
}

/* ---------------------------------------------------------------- Mulholland: hillsides, rails, trusses (S2.4) */

#define XEDGE_L  (-1.0)
#define XEDGE_R  321.0
#define YBOT     102.0          /* below the last view row (0x65) */

static void poly(u8 col, int n, const float *xy) { ras_poly(xy, n, col); }

/* a far bank's top (beyond the original's rows): its rise over the level horizon at Pw (or over the road, if higher)
 * shrinks by (FAR_Z / z)^FAR_FADE, so tall far banks stay a slope over the mountain panorama, not a slab across it */
static float far_top(const cam *c, float Pw, float yt)
{
    float z = (Pw - c->P) / 8.0 + 4.0;
    float y = proj_y(c, Pw, SURFACE), hz = fminf(y, y - 4.0 * (SURFACE - c->cy) * (1.0 / z - 1.0 / Z_HORIZON));
    return yt < hz ? hz - (hz - yt) * powf(FAR_Z / z, FAR_FADE) : yt;
}

/* a bank's outer vertex at slice boundary p (road edge x, top *yt in, out), from its own depth only so that adjacent
 * slices share it: up to FAR_Z the screen edge; beyond, the top reaches FAR_SLOPE times its height outward of the
 * road, blended in over FAR_BLEND (no seam between the last screen-wide slice and the first sloped one) */
#define FAR_BLEND 24.0
static void far_edge(const cam *c, const sect *p, float x, int sg, float *xe, float *yt)
{
    float z = (p->Pw - c->P) / 8.0 + 4.0, e = sg < 0 ? XEDGE_L : XEDGE_R;
    *xe = e;
    if (z <= FAR_Z) return;
    *yt = far_top(c, p->Pw, *yt);
    float t = fminf((z - FAR_Z) / FAR_BLEND, 1.0), xs = x + sg * FAR_SLOPE * fabsf(p->y - *yt);
    xs = sg < 0 ? fmaxf(xs, e) : fminf(xs, e);
    *xe = e + (xs - e) * t * t * (3.0 - 2.0 * t);
}

/* front_hillsides of segment s (race_road.c): the banks beside the road as polygons of colour 0x0E from the screen
 * edge to the outer band, on the left (sg -1) and the right (sg +1); `a` near, `b` far (a slice of the segment);
 * the conditions of the original's row (bank in front / behind, bridge neighbour) are the segment's, the geometry is
 * the slice's. Peak widths and heights come from the original's per-row tables at the continuous row. */
static void hill_side(const cam *c, int s, const sect *a, const sect *b, bool first, const sect *q1, int sg)
{
    bool left = sg < 0;
    u16 bank = left ? TRK_BL : TRK_BR;
    bool row1 = s == c->seg0 + 1;
    bool bridge_prev = flag(TRK_C, s - 1) & 0x80;
    s16 bq = enh_bank(bank, s - 1), bp = enh_bank(bank, s);        /* bank (up) at the near / far boundary */
    s16 bq1 = enh_bank(bank, s - 2);
    float o = c->fall_dir;
    float dyaw = c->heading - enh_track_yaw(c->seg0 * 64.0);
    if (left ? !(dyaw > -100) : !(dyaw < 100)) return;
    if (bq == 0) return;                                        /* Yq == YT1q: no bank at the near boundary */
    float xq = left ? a->xol : a->xor_, xp = left ? b->xol : b->xor_;
    float ytq = left ? a->yt1 : a->yt2, ytp = left ? b->yt1 : b->yt2;
    float xe = left ? XEDGE_L : XEDGE_R, xea = xe, xeb = xe;
    /* beyond the original's rows the bank does not run out to the screen edge (a slab over the mountains) but reaches
     * its top FAR_SLOPE times its height outward of the road, the screen slope of the near banks */
    if (!c->compare) {
        far_edge(c, a, xq, sg, &xea, &ytq);
        far_edge(c, b, xp, sg, &xeb, &ytp);
    }
    float ra = rowf(c, a->Pw), rb = rowf(c, b->Pw);
    float W1 = tabv(DS_hill_peak_tab, 15.0 - ra, 0), H1 = tabv(DS_hill_peak_tab, 15.0 - ra, 2);
    float W0 = tabv(DS_hill_peak_tab, 15.0 - rb, 0), H0 = tabv(DS_hill_peak_tab, 15.0 - rb, 2);
    float E = tabv(0x7E3A, 15.0 - rb, 0);
    float xl_q = left ? a->xl : a->xr;
    /* the original turns every bank into free-standing peaks while a bridge ends in rows 10..14; live only rows 10..14
     * (the far side of the valley) do: the nearer and the far rows keep their banks, so the road climbing to the bridge
     * and the far road do not run over the panorama with no ground beside them */
    if (c->l16 && (c->compare || (s - c->seg0 >= 10 && s - c->seg0 <= 14))) {
        if (bp < 0) return;                                     /* YT1p <= Yp */
        if (bridge_prev && first && q1) {
            float xq1 = left ? q1->xl : q1->xr;
            float v[12] = { xq1, YBOT, xq1, q1->y, xl_q, a->y, xq, a->y, xq + sg * W1, a->y, left ? E : 320.0 - E, YBOT };
            poly(0x0E, 6, v);
            if (bp > 0) { float t[6] = { xq, a->y, xq + sg * W1 / 2, a->y - H1, xq + sg * W1, a->y }; poly(0x0E, 3, t); }
            else { float t[8] = { xq, a->y, xq + sg * W1, a->y, xp + sg * W1, b->y, xp, b->y }; poly(0x0E, 4, t); }
        }
        if (bp > 0) {
            float t[8] = { xq, a->y, xq + sg * W1 / 2, a->y - H1, xp + sg * W0 / 2, b->y - H0, xp, b->y };
            poly(0x0E, 4, t);
        }
        return;
    }
    if (row1 || !bridge_prev) {
        /* the original skips a bank beside a road narrower than 30 px; beyond its rows every far road is, and its banks
         * (the only ground beside it) would vanish near the horizon */
        if (a->xol + 0x1E < a->xor_ || (!c->compare && (a->Pw - c->P) / 8.0 + 4.0 > FAR_Z)) {
            float t[8] = { xea, ytq, xq - o, a->y, xp + o, b->y, xeb, ytp };
            poly(0x0E, 4, t);
            bool tri = row1 ? a->y > ytq : bq1 == 0;
            if (tri && (left ? xq > 0 : xq < 0x13F)) {
                /* a far bank's front face above the level horizon h0 reaches down to h0 (the mound's flank at the road,
                 * the ground's clip line), not only to the road: no strip of mountains under it */
                float h0 = c->compare ? -1.0 : ground_h0(c, a);
                if (h0 > a->y) {
                    float xf = xq + sg * flank(c, a, h0), xo = left ? fminf(xea, xf) : fmaxf(xea, xf);
                    float u[8] = { xea, ytq, xq, a->y, xf, h0, xo, h0 };
                    poly(0x0E, 4, u);
                }
                else { float u[6] = { xea, ytq, xq, a->y, xea, a->y }; poly(0x0E, 3, u); }
            }
        }
    } else {
        if (first && q1) {
            float xq1 = left ? q1->xl : q1->xr;
            float v[12] = { xea, ytq, xq, a->y, xl_q, a->y, xq1, q1->y, xq1, YBOT, xea, YBOT };
            poly(0x0E, 6, v);
        }
        float t[8] = { xea, ytq, xq, a->y, xp, b->y, xeb, ytp };
        poly(0x0E, 4, t);
    }
}

static void hillsides(const cam *c, int s, const sect *p, int n)
{
    if (c->mode != 0x10 || (cflag(c, TRK_C, s) & 0x80)) return;
    sect q1 = section(c, fmaxf((s - 2) * 64.0, c->P));
    for (int i = 0; i < n; i++)
        for (int sg = -1; sg <= 1; sg += 2) hill_side(c, s, &p[i], &p[i + 1], i == 0, &q1, sg);
}

/* a thin line as a quad (the 1-pixel edge colour of poly_fill / clip_line) */
static void thin(u8 col, float x0, float y0, float x1, float y1)
{
    float th = 0.3;
    quad(col, x0, y0, x1, y1, x1, y1 + th, x0, y0 + th);
}

/* a quad with an edge colour: the edge drawn first, a little wider */
static void edged(u8 fill, u8 edge, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
    float cx = (x0 + x1 + x2 + x3) / 4, cy = (y0 + y1 + y2 + y3) / 4, e = 0.4;
    float xs[4] = { x0, x1, x2, x3 }, ys[4] = { y0, y1, y2, y3 }, o[8];
    for (int k = 0; k < 4; k++) {
        o[2 * k] = xs[k] + (xs[k] > cx ? e : -e);
        o[2 * k + 1] = ys[k] + (ys[k] > cy ? e : -e);
    }
    ras_poly(o, 4, edge);
    quad(fill, x0, y0, x1, y1, x2, y2, x3, y3);
}

/* guard_rail (right side of the road, R & 0x8000): a short post at the near boundary on every other segment and the
 * rail between the kerb height and the eye level */
static void guard_rail(const cam *c, int s, const sect *p, int n, int m)
{
    int run = s;
    while (run > 0 && (flag(TRK_R, run - 1) & 0x8000)) run--;      /* the run's first segment: the phase */
    if (((run - s + 14) & 1) != 0) {
        const sect *a = &p[0];
        float z = (a->Pw - c->P) / 8.0 + 4.0, x = a->xor_ - 240.0 / (z + 12.0);
        edged(5, 2, x, a->yk - 1, x, a->y, a->xor_, a->y, a->xor_, a->yk - 1);
    }
    for (int i = 0; i < n; i++) {
        const sect *a = &p[i], *b = &p[i + 1];
        quad(5, b->xor_, b->ye - m, b->xor_, b->yk, a->xor_, a->yk, a->xor_, a->ye - m);
        thin(2, b->xor_, b->ye - m, a->xor_, a->ye - m);
        thin(2, b->xor_, b->yk, a->xor_, a->yk);
    }
}

/* scenery_draw_truss (bridges, C & 0x80) over a whole segment, colour 10, lines `N` px thick; `alt`: the diagonal's direction */
static void truss_n(float N, bool alt, const sect *a, const sect *b, const cam *c)
{
    float yt1 = proj_y(c, a->Pw, 1600.0), yt2 = proj_y(c, b->Pw, 1600.0);
    float xl1 = a->xl, xr1 = a->xr, xl2 = b->xl, xr2 = b->xr, yb1 = a->y, yb2 = b->y;
    quad(10, xl1 - N, yt1, xl1, yt1, xl1, yb1 + 1, xl1 - N, yb1 + 1);          /* posts and top bar at the near end */
    quad(10, xr1 + 1, yt1, xr1 + 1 + N, yt1, xr1 + 1 + N, yb1 + 1, xr1 + 1, yb1 + 1);
    quad(10, xl1, yt1, xr1 + 1, yt1, xr1 + 1, yt1 + N, xl1, yt1 + N);
    for (int sd = 0; sd < 2; sd++) {
        float x1 = sd ? xr1 + 1 : xl1 - N, x2 = sd ? xr2 + 1 : xl2 - N;
        if (alt) quad(10, x1, yb1, x1 + N, yb1, x2 + N, yt2, x2, yt2);
        else     quad(10, x1, yt1, x1 + N, yt1, x2 + N, yb2, x2, yb2);
        quad(10, x1, yt1, x1 + N, yt1, x2 + N, yt2, x2, yt2);                  /* top and bottom rails */
        quad(10, x1, yb1, x1 + N, yb1, x2 + N, yb2, x2, yb2);
    }
}

static void truss(const cam *c, int s, int k, const sect *a, const sect *b)
{
    truss_n(dash_width(c, k, b->Pw) + 1, (s & 1) == 0 || k >= 7, a, b, c);
}

static void truss_end_n(const cam *c, float N, const sect *b)
{
    float yt = proj_y(c, b->Pw, 1600.0);
    quad(10, b->xl - N, yt, b->xl, yt, b->xl, b->y + 1, b->xl - N, b->y + 1);
    quad(10, b->xr + 1, yt, b->xr + 1 + N, yt, b->xr + 1 + N, b->y + 1, b->xr + 1, b->y + 1);
    quad(10, b->xl, yt, b->xr + 1, yt, b->xr + 1, yt + N, b->xl, yt + N);
}

static void truss_end(const cam *c, int k, const sect *b) { truss_end_n(c, dash_width(c, k, b->Pw) + 1, b); }

/* mulholland_extras_front: after the row's objects */
static void extras(const cam *c, int s, int k, const sect *p, int n)
{
    u16 C = cflag(c, TRK_C, s), R = cflag(c, TRK_R, s);
    if (C & 1) return;
    if ((C & 0x80) && (flag(TRK_C, s - 1) & 0x80)) truss(c, s, k, &p[0], &p[n]);
    if ((C & 0x80) && !(flag(TRK_C, s + 1) & 0x80)) truss_end(c, k, &p[n]);
    if ((R & 0x8000) && !(C & 0x80)) guard_rail(c, s, p, n, 0);
}

/* drag strip: the barrier walls along the road edges, stripes of 5 segments (road_draw_barrier_walls) */
static void barriers(const cam *c, int s, const sect *p, int n)
{
    int v = abs(14 + c->start_seg - s) % 10;
    u8 col = v < 5 ? 0x0B : 4;
    /* the original's bottom row (poly_fill includes it) reaches below the road edge, where the nearer segment's band,
     * drawn later, cuts it level at its far row: a stair step per segment. Live, the wall ends on the road edge itself. */
    float d = c->compare ? 1.0 : 0.0;
    for (int i = 0; i < n; i++) {
        const sect *a = &p[i], *b = &p[i + 1];
        quad(col, b->xr, b->yk, b->xr, b->y + d, a->xr, a->y + d, a->xr, a->yk);
        quad(col, b->xl, b->yk, b->xl, b->y + d, a->xl, a->y + d, a->xl, a->yk);
    }
}

/* ---------------------------------------------------------------- road-side objects (enh_sprite.c) */

/* The original's integer row values at boundary Pw: road_proj_x / _y themselves when comparing (the snapshot's state is
 * still in DGROUP then); otherwise the exact projection moved by half a pixel the way the original truncates (towards
 * the screen centre in x, towards the eye level in y), blended linearly over the 2 px around the centre so that an
 * object crossing it does not jump. */
static float est_x(const cam *c, float Pw, float xw)
{
    float z = (Pw - c->P) / 8.0 + 4.0, dx = c->heading - enh_track_yaw(Pw);
    if (c->compare) return road_proj_x((s16)lroundf(z), (s16)lroundf(xw), (s16)(int)lroundf(dx));
    float u = 4.0 * (c->cx - xw) / z - dx;
    return 160.0 - u + 0.5 * fminf(fmaxf(u / 2.0, -1.0), 1.0);
}

static float est_y(const cam *c, float Pw, float yw)
{
    float z = (Pw - c->P) / 8.0 + 4.0, dy = (c->mode & 6) ? 0.0 : c->hgt - enh_track_hgt(Pw);
    if (c->compare) return road_proj_y((s16)lroundf(z), (s16)lroundf(yw), (s16)(int)lroundf(dy));
    float v = 2.0 * dy + 4.0 * (yw - c->cy) / z;
    return v - 0.5 * fminf(fmaxf(v / 2.0, -1.0), 1.0) + c->bounce + 60.0;
}

/* front_objects of segment s (row k = s - seg0; the original's rows < 11, live out to the road's far end) at its far
 * boundary, the objects of `pass`; none on the plain road past a table end */
static void objects(const cam *c, int s, int pass)
{
#ifdef STREET_ROD_GE
    if (sr_ge_recording) return;
#endif
    int k = s - c->seg0;
    if (k < 1 || (c->compare && k > 10) || s >= c->synth) return;      /* the original: rows < 11 */
    float Pw = s * 64.0, l, r;
    enh_bound b = { .s = s, .k = k, .depth = k, .depth_banner = k };
    b.z = (Pw - c->P) / 8.0 + 4.0;
    b.fade = c->compare ? 1.0 : enh_far_fade(c->far_z, b.z);  /* fade in over the last 10 % */
    if (b.fade <= 0.0) return;
    edges_at(c, s, &l, &r);
    b.xl = est_x(c, Pw, l);
    b.xr = est_x(c, Pw, r);
    b.xol = est_x(c, Pw, 0x5DC - c->w);
    b.xor_ = est_x(c, Pw, 0xCE4 + c->w);
    b.xfl = est_x(c, Pw, (c->w - 0x177) * -4);
    b.xfr = est_x(c, Pw, (c->w + 0x339) * 4);
    b.y = est_y(c, Pw, SURFACE);
    b.ye = est_y(c, Pw, EYE);
    b.y16 = c->compare ? 4 * b.ye - 3 * b.y : est_y(c, Pw, 1600.0);
    float Pq = fmaxf((s - 1) * 64.0, c->P), yq = est_y(c, Pq, SURFACE);    /* row k - 1 (row 0 at z = 4) */
    float ybl = est_y(c, Pq, SURFACE - enh_bank(TRK_BL, s - 1)), ybr = est_y(c, Pq, SURFACE - enh_bank(TRK_BR, s - 1));
    b.flat_l = c->compare ? ybl == yq : fabsf(ybl - yq) < 0.5;
    b.flat_r = c->compare ? ybr == yq : fabsf(ybr - yq) < 0.5;
    b.dash = dash_width(c, k, Pw);
    /* Mulholland's far road above the level horizon h0 stands on a mound only as wide as the road at its top (mounds):
     * objects beside it (beyond a verge's width of the outer edges) would float over the mountains, so they are left
     * out until their ground drops below h0 */
    sect sb = section(c, Pw);
    float h0 = ground_h0(c, &sb);
    if (h0 > 0.0 && sb.y < h0) {
        float tol = fmaxf(b.xor_ - b.xr, b.xl - b.xol) + 1.0 + mound_wide(c, Pw);
        b.gclip = true;
        b.gxl = b.xol - tol;
        b.gxr = b.xor_ + tol;
    }
    enh_objects(&b, c->mode, c->compare, pass);
}

/* ---------------------------------------------------------------- props (S2.5, enh_props.c) */

/* The car of the front view (levels 0..8) at the interpolated position O: the original's anchor (opponent_draw: z =
 * |d / 8| + 4, road_proj_x / _y with the YAW / HGT at the car), continuous. Not in the compare view (the original's
 * pixels stay there) and not on the drag strip's mode 2 (no opponent). */
static void car_front(const cam *c)
{
#ifdef STREET_ROD_GE
    if (sr_ge_recording) return;
#endif
    const enh_snap *b = c->snap;
    int s = b->opp_s[0];
    if (c->compare || c->mode == 2 || !c->racing) return;
    if (enh_far_opp(b, c->O, c->P)) {                         /* beyond the original's rows (§4 item 4) */
        float z = (c->O - c->P) / 8.0 + 4.0;
        if (z >= c->far_z) return;
        float sx = 160.0 + c->heading - enh_track_yaw(c->O) - 4.0 * (c->cx - c->lat) / z;
        float dy = (c->mode & 6) ? 0.0 : c->hgt - enh_track_hgt(c->O);
        float sy = 2.0 * dy + 4.0 * (SURFACE + b->floor_h - c->cy) / z + c->bounce + 60.0;
        enh_snap q = *b;
        q.opp_s[0] = 8;
        q.opp_special[0] = false;
        ras_alpha(enh_far_fade(c->far_z, z));
        enh_prop_car(&q, 0, sx, sy, z);
        ras_alpha(1.0);
        return;
    }
    if (!b->opp_drawn[0] || s < 0 || s > 8) return;
    float z8 = (c->O - c->P) / 8.0;
    if (s == 0) z8 += 16.0;
    if (z8 < -2.0 && s == 2) z8 += 8.0;
    float z = fabsf(z8) + 4.0;
    float dx = c->heading - enh_track_yaw(c->O), dy = (c->mode & 6) ? 0.0 : c->hgt - enh_track_hgt(c->O);
    float sx = 160.0 + dx - 4.0 * (c->cx - c->lat) / z;
    float sy = 2.0 * dy + 4.0 * (SURFACE + b->floor_h - c->cy) / z + c->bounce + 60.0;
    enh_prop_car(b, 0, sx, sy, z);
}

/* The car of the mirror (levels 9..12): road_proj_x_mirror / _y_mirror with the original's +10 on the heading term. */
static void car_mirror(const cam *c)
{
    const enh_snap *b = c->snap;
    enh_snap q;
    bool far = !c->compare && enh_far_opp_mirror(b, c->O, c->P);      /* beyond the original's rows (§4 item 7) */
    if (far) {
        q = *b;
        q.opp_s[1] = 12;
        q.opp_drawn[1] = true;
        q.opp_special[1] = false;
        b = &q;
    }
    int s = b->opp_s[1];
    if (c->compare || c->mode == 2 || !c->racing || !b->opp_drawn[1] || s < 9 || s > 12) return;
    float zm = fmaxf((c->P - c->O) / 8.0, 1.0);
    float dx = 10.0 + enh_track_yaw(c->O) - c->heading, dy = (c->mode & 6) ? 0.0 : enh_track_hgt(c->O) - c->hgt;
    float sx = 280.0 + dx / 4.0 - (c->cx - c->lat) / zm;
    float sy = 36.0 + dy + (SURFACE + b->floor_h - c->cy) / zm;
    if (far) ras_alpha(enh_far_fade(8.0 * enh_mirror_rows(false), zm));
    enh_prop_car(b, 1, sx, sy, zm);
    ras_alpha(1.0);
}

/* the drag strip's start tree: 4 segments past the start line, centred between the road edges of that boundary */
static void tree(const cam *c, int s)
{
#ifdef STREET_ROD_GE
    if (sr_ge_recording) return;
#endif
    if (c->compare || s != c->start_seg + 4 || !(c->mode & 6)) return;
    float Pw = s * 64.0, l, r, z = (Pw - c->P) / 8.0 + 4.0;
    edges_at(c, s, &l, &r);
    ras_alpha(enh_far_fade(c->far_z, z));
    enh_prop_tree(floorf((est_x(c, Pw, l) + est_x(c, Pw, r)) / 2.0), est_y(c, Pw, SURFACE), z);
    ras_alpha(1.0);
}

/* One segment s (band kband of the original) between Pa (near) and Pb (far), in slices. */
static void segment(const cam *c, int s, int kband, float Pa, float Pb, float *hrun)
{
    sect p[9];
    float ya = proj_y(c, Pa, SURFACE), yb = proj_y(c, Pb, SURFACE);
    int n = clampi((int)ceilf(fabsf(ya - yb) / 2.0), 1, 8);          /* far segments: one slice */
    for (int i = 0; i <= n; i++) p[i] = section(c, Pa + (Pb - Pa) * i / n);
    float zb = (Pb - c->P) / 8.0 + 4.0;
    if (!c->compare) ras_haze(enh_haze_level(c->far_z, ((Pa + Pb) / 2.0 - c->P) / 8.0 + 4.0));
    /* the horizon clip (§4 items 1, 6): beyond the original's rows the road surface of a race stays below the horizon of
     * its own level (the segment's higher end), so a level road ends at the sky panorama and a hill ahead still rises
     * above it; Mulholland too: its sky picture's horizon is the original's hy = road_proj_y(0xF0, 2000, dy), the same
     * level horizon, so the far road no longer covers its mountains; its far verges stay below it too */
    float hclip = ENH_VIEW_Y0;
    if (c->racing && !c->compare && zb > FAR_Z) {
        float dy = (c->mode & 6) ? 0.0 : c->hgt - fmaxf(enh_track_hgt(Pa), enh_track_hgt(Pb));
        hclip = fmaxf(hclip, 2.0 * dy + 4.0 * (SURFACE - c->cy) / Z_HORIZON + c->bounce + 60.0 + YOFF);
    }
    /* never below a farther segment's clip: on a road climbing beyond Z_HORIZON each segment's own level horizon lies
     * ~2 rows below the farther one's and cut the top of its band where the farther band does not reach, leaving a
     * line of panorama per segment across the far ground */
    hclip = fminf(hclip, *hrun);
    *hrun = hclip;
    /* the objects behind the building line go behind the segment's walls (enh_sprite.c); comparing: the original's
     * order, all objects after the band */
    bool split = !c->racing && !c->compare;
    if (split) objects(c, s, ENH_OBJ_OUT);
    if (!c->racing) {
        town_walls(c, s, p, n, true);
        town_walls(c, s, p, n, false);
    }
    /* the town's far road and sidewalks run on to the walls' vanishing point (eye level, above the rooftop panorama's
     * base at row 63): clipped there, the far objects and walls stood over the panorama with no ground under them */
    hillsides(c, s, p, n);                                         /* unclipped: far banks end in their own slope */
    if (hclip > ENH_VIEW_Y0) ras_clip(0, hclip, 320, ENH_VIEW_Y1);
    band(c, s, kband, p, n, hclip);
    if (split || hclip > ENH_VIEW_Y0) ras_clip(0, ENH_VIEW_Y0, 320, ENH_VIEW_Y1);
    objects(c, s, split ? ENH_OBJ_IN : ENH_OBJ_ALL);
    tree(c, s);
    if (c->mode == 0x10) extras(c, s, kband, p, n);
    if (s == (int)floorf(c->O / 64.0)) car_front(c);                 /* after the row's objects, before the barrier walls */
    if (c->mode & 6) {
        if (hclip > ENH_VIEW_Y0) ras_clip(0, hclip, 320, ENH_VIEW_Y1);
        barriers(c, s, p, n);
        ras_clip(0, ENH_VIEW_Y0, 320, ENH_VIEW_Y1);
    }
}

/* The town's street seen beyond its last segment (§4 item 2): a closing house front across the whole width at the
 * clamped far end, the walls' colours (wall 3, the tall part 4 up to the tall walls' top), behind everything. */
static void town_close(const cam *c, float Pw)
{
    sect f = section(c, Pw);
    float top = proj_y(c, Pw, EYE - (SURFACE - EYE));
    ras_haze(enh_haze_level(c->far_z, (Pw - c->P) / 8.0 + 4.0));
    quad(3, f.xol, f.y + 1, f.xor_, f.y + 1, f.xor_, f.ye, f.xol, f.ye);
    quad(4, f.xol, f.ye + 1, f.xor_, f.ye + 1, f.xor_, top, f.xol, top);
    ras_haze(0);
}

static void road(const cam *c)
{
    /* out to z = far_z (116 = the original's row 14 at --draw-distance-scale 1, 144 at 1.25: 17.5 segments). Comparing,
     * the last segment seg + ceilf(N) is stretched or cut to the far end, as the original pins its row 14 at z = 116;
     * live, each segment up to the one holding the far end is drawn with its own flags. The town stops at its last
     * segment (race-road segments never show in the town; its last stop, Burger's at seg 0x9B, sees to 0xAC). */
    if (c->rows < 0) return;
    float Pnear = c->P, Pfar = c->P + (c->far_z - 4.0) * 8.0;
    if (!c->racing && Pfar > TOWN_END * 64.0) {
        Pfar = TOWN_END * 64.0;
        if (!c->compare && Pfar > Pnear) town_close(c, Pfar);
    }
    int s_end = c->compare ? c->seg0 + (int)ceilf((c->far_z - 4.0) / 8.0 - 1e-9) : (int)ceilf(Pfar / 64.0);
    int s_near = c->seg0 + 1, s_far = c->rows > 0 && s_end > c->seg0 + c->rows ? c->seg0 + c->rows : s_end;
    float hrun = ENH_VIEW_Y1;                                      /* the horizon clip so far (segment) */
    for (int s = s_far; s >= s_near; s--) {
        float Pa = fmaxf((s - 1) * 64.0, Pnear), Pb = s == s_end ? Pfar : fminf(s * 64.0, Pfar);
        if (Pb > Pa) segment(c, s, s - c->seg0, Pa, Pb, &hrun);
    }
    ras_haze(0);
    if ((int)floorf(c->O / 64.0) <= c->seg0) car_front(c);          /* the car at row 0 is drawn last */
}

/* ---------------------------------------------------------------- sky (road_draw_sky) */

#define SKY_Y0 0x66             /* page 1 rows 0x66..199: sky picture and horizon panorama (road_load_graphics) */
static u8 sky_src[200 - SKY_Y0][320];

void enh_sky_capture(void)
{
    const u8 *pl[4] = { ega_plane(0), ega_plane(1), ega_plane(2), ega_plane(3) };
    for (int y = SKY_Y0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            u16 o = (u16)(0x2000 + y * EGA_BYTES_PER_LINE + x / 8);
            int b = 7 - x % 8;
            sky_src[y - SKY_Y0][x] = (u8)((pl[0][o] >> b & 1) | (pl[1][o] >> b & 1) << 1 | (pl[2][o] >> b & 1) << 2
                                        | (pl[3][o] >> b & 1) << 3);
        }
}

/* One sample row from page-1 row `src` shifted right by `shift` logical pixels (wrapping at 320). */
static void sky_row(u8 *dst, int S, int src, float shift)
{
    const u8 *s = sky_src[clampi(src, SKY_Y0, 199) - SKY_Y0];
    float sh = fmodf(shift, 320.0);
    if (sh < 0) sh += 320.0;
    if (S == 1) {
        int first = (int)floorf(0.5f - sh);
        if (first < 0) first += 320;
        memcpy(dst, s + first, (size_t)(320 - first));
        if (first) memcpy(dst + 320 - first, s, (size_t)first);
        return;
    }
    /* the source column of every sample depends on the shift only: built once per shift (the rows of a frame share it) */
    static int xmap[320 * 16]; static float map_sh = -1e9; static int map_S;
    if (S > 16) {                                               /* beyond the documented res_scale * aa <= 16 */
        for (int i = 0; i < 320 * S; i++) { int x = (int)floorf((i + 0.5) / S - sh); dst[i] = s[x < 0 ? x + 320 : x]; }
        return;
    }
    if (sh != map_sh || S != map_S) {
        for (int i = 0; i < 320 * S; i++) {
            int x = (int)floorf((i + 0.5) / S - sh);
            xmap[i] = x < 0 ? x + 320 : x;
        }
        map_sh = sh;
        map_S = S;
    }
    for (int i = 0; i < 320 * S; i++) dst[i] = s[xmap[i]];
}

/* road_draw_sky_mulholland: one picture (sky, mountains) from page-1 row 0x6E - dy, dy = the car's height over the road
 * four segments behind (clamped to +-8), scrolled by the heading; the ground is the bands and hillsides. Falling off
 * the road (fall_dir) the rows below the picture are the hillside colour. */
static void sky_mulholland(const cam *c)
{
    int S = ras_S(), rows = (ENH_VIEW_Y1 - ENH_VIEW_Y0) * S;
    float dy = c->hgt - (c->compare ? TRKS(TRK_HGT, clampi(c->seg0 - 4, 0, TRK_N - 1)) : enh_track_hgt(c->P - 288.0));
    if (dy > 8) dy = 8;
    else if (dy < -8 && c->fall_dir == 0) dy = -8;
    float sy = 0x6E - dy, bottom = 0x12 + fminf(199 - sy, 0x53), shift = 2.0 * SKY_RATE * c->heading;
    if (c->compare) {                                           /* the original's scroll: 2 px per unit, whole bytes */
        int h = (int)lroundf(c->heading), m = (abs(h) * 2 % 320) & ~7;
        shift = h > 0 ? m : -m;
    }
    for (int j = 0; j < rows; j++) {
        float y = ENH_VIEW_Y0 + (j + 0.5) / S;
        u8 *d = ras_row(j);
        if (y >= bottom) memset(d, c->fall_dir ? 0x0E : 9, (size_t)320 * S);
        else sky_row(d, S, (int)floorf(y - 0x12 + sy), shift);
    }
}

static void sky(const cam *c)
{
    float hs = c->compare ? 0.0 : enh.haze / 100.0;        /* the haze colour: the sky just above the horizon */
    if (c->racing && c->mode == 0x10) { sky_mulholland(c); ras_haze_target(ENH_VIEW_Y0 + 2, hs); return; }
    int S = ras_S(), rows = (ENH_VIEW_Y1 - ENH_VIEW_Y0) * S;
    float hy = 0;
    if (c->racing) {
        /* the original holds HGT[seg - 1] for the whole segment: compare that, else the value at the hold's middle */
        float h = c->compare ? TRKS(TRK_HGT, clampi(c->seg0 - 1, 0, TRK_N - 1)) : enh_track_hgt(c->P - 96.0);
        float dy = (c->mode & 6) ? 0.0 : c->hgt - h;
        hy = 2.0 * dy + 400.0 / 240.0 + c->bounce + 60.0 - 0.5;    /* the original's rows start at the integer hy */
        hy = fminf(fmaxf(hy, 0x12), 0x52);
    }
    int last_layer = -1, last_src = -1;
    for (int j = 0; j < rows; j++) {
        float y = ENH_VIEW_Y0 + (j + 0.5) / S;
        int layer, src = 0;
        float shift = 0;
        u8 fill = 0;
        if (!c->racing) {                                       /* town: fixed sky and rooftops */
            if (y < 47)      { layer = 0; src = (int)floorf(y) + 84; }
            else if (y < 63) { layer = 1; src = (int)floorf(y) + 116; }
            else             { layer = 2; fill = 1; }
        } else if (y < hy - 12) {
            layer = 0; src = (int)floorf(y + 0xA6 - hy); shift = 2.0 * SKY_RATE * c->heading;
        } else if (y < hy + 8) {
            layer = 1; src = (int)floorf(y + 0xAF - hy); shift = 4.0 * SKY_RATE * c->heading;
        } else { layer = 2; fill = 9; }
        u8 *d = ras_row(j);
        if (layer == last_layer && src == last_src) memcpy(d, ras_row(j - 1), (size_t)320 * S);
        else if (layer == 2) memset(d, fill, (size_t)320 * S);
        else sky_row(d, S, src, shift);
        last_layer = layer;
        last_src = src;
    }
    ras_haze_target(c->racing ? fmaxf(hy - 13.0, ENH_VIEW_Y0) : 46.0, hs);
}

/* ---------------------------------------------------------------- mirror (road_draw_mirror, §3.7) */

/* Mirror row r (1..7) lies between boundaries seg - r (far, the row's values) and seg - r + 1 (near) and carries the
 * flags of segment seg - r - 1 (the original's flag pointers start at seg - 8 for row 7). */

/* dash width of mirror row r (table DS:7E52 from index 13 + r): constant per row when comparing, else continuous */
static float mir_dash(const cam *c, int r, float Pw)
{
    if (c->compare) return DSS((u16)(DS_dash_width + 2 * clampi(13 + r, 0, 24)));
    float kf = 12.5 + (c->P - Pw) / 64.0;
    int k0 = clampi((int)floorf(kf), 0, 24), k1 = clampi(k0 + 1, 0, 24);
    float f = fminf(fmaxf(kf - floorf(kf), 0.0), 1.0);
    s16 w0 = DSS((u16)(DS_dash_width + 2 * k0)), w1 = DSS((u16)(DS_dash_width + 2 * k1));
    return w0 + (w1 - w0) * f;
}

/* town_walls_mirror (town blocks only, C & 1): the tall part is half the wall's height on the left and as high on the
 * right (the original's k = 2 - (left == 0)); the end faces look at segment s + 1, nearer the car */
static void mir_walls(const cam *c, int s, const sect *p, int n, bool left)
{
    u16 C = flag(TRK_C, s);
    if (!(C & 1) || (C & 4)) return;
    u16 F = flag(left ? TRK_L : TRK_R, s), Fn = flag(left ? TRK_L : TRK_R, s + 1), Cn = flag(TRK_C, s + 1);
    int k = left ? 2 : 1;
    float top = EYE - (SURFACE - EYE) / k;
    for (int i = 0; i < n; i++) {
        const sect *a = &p[i], *b = &p[i + 1];                  /* near, far */
        float xa = left ? a->xol : a->xor_, xb = left ? b->xol : b->xor_;
        quad((F & 0x10) ? 2 : 3, xa, a->y + 1, xb, b->y + 1, xb, b->ye, xa, a->ye);
        if (F & 4)
            quad(4, xa, a->ye + 1, xb, b->ye + 1, xb, proj_y(c, b->Pw, top), xa, proj_y(c, a->Pw, top));
    }
    const sect *nr = &p[0], *fr = &p[n];
    float xn = left ? nr->xol : nr->xor_, xf = left ? fr->xol : fr->xor_;
    if (Cn & 4) {
        float x0 = left ? MIR_X0 - 1.0 : xn, x1 = left ? xn : 321.0;
        ras_rect(x0, nr->ye, x1, nr->y - 1.0, 2);
        ras_rect(x0, nr->y - 1.0, x1, nr->y, 4);
    }
    if (!(F & 4) || (Fn & 4)) return;
    float h = (nr->y - nr->ye) / k, wd = fabsf(xn - xf);
    if (left) ras_rect(xn - wd, nr->ye - h, xn, nr->ye, 2);
    else      ras_rect(xn, nr->ye - h, xn + wd, nr->ye, 2);
}

/* road_draw_band_mirror (modes 0, 1): grass, road (white finish band), or the town's sidewalks; centre dashes */
static void mir_band(const cam *c, int s, int r, const sect *p, int n)
{
    u16 C = flag(TRK_C, s), L = flag(TRK_L, s), R = flag(TRK_R, s);
    bool cross = C & 1;
    if (cross && (C & 4)) return;
    bool mh = c->mode == 0x10;
    bool white = (C & 0x10) && c->seg0 >= (mh ? 0x3AD : 0x169);
    bool wide = (c->mode & 6) && c->seg0 - r < c->start_seg + 5;       /* mirror_row_seg = seg - r */
    bool verge = mh && !(C & 0x80) && enh_track_hgt(c->seg0 * 64.0) - 0x0B <= c->hgt;
    bool flat_l = !mh || enh_bank(TRK_BL, s + 2) == 0, flat_r = !mh || enh_bank(TRK_BR, s + 2) == 0;
    for (int i = 0; i < n; i++) {
        const sect *a = &p[i], *b = &p[i + 1];                  /* near, far */
        if (b->y > a->y) continue;                              /* faces away */
        if (!cross) {
            if (verge) {
                band_quad(2, b->xol - 6, b->xl, a->xol - 6, a->xl, b, a);
                band_quad(2, b->xr, b->xor_ + 6, a->xr, a->xor_ + 6, b, a);
            }
            if (mh && (C & 0x80) && !c->compare) {          /* bridges: water beside the deck (band) */
                band_quad(WATER, MIR_X0 - 1.0, b->xl, MIR_X0 - 1.0, a->xl, b, a);
                band_quad(WATER, b->xr, 321.0, a->xr, 321.0, b, a);
            } else {
                if (flat_l) band_quad(9, MIR_X0 - 1.0, b->xl, MIR_X0 - 1.0, a->xl, b, a);
                if (flat_r) band_quad(9, b->xr, 321.0, a->xr, 321.0, b, a);
            }
            band_quad(white ? 0x0F : 1, b->xl, b->xr, a->xl, a->xr, b, a);
        } else {
            if (!(L & 0x80)) band_quad(4, b->xol, b->xl, a->xol, a->xl, b, a);
            else {
                float mb = (b->xol + b->xl) / 2, ma = (a->xol + a->xl) / 2;
                band_quad(4, b->xol, mb, a->xol, ma, b, a);
                band_quad(9, mb, b->xl, ma, a->xl, b, a);
            }
            if (!(R & 0x80)) band_quad(4, b->xr, b->xor_, a->xr, a->xor_, b, a);
            else {
                float mb = (b->xor_ + b->xr) / 2, ma = (a->xor_ + a->xr) / 2;
                band_quad(4, mb, b->xor_, ma, a->xor_, b, a);
                band_quad(9, b->xr, mb, a->xr, ma, b, a);
            }
        }
        if (C & 8) {
            float wb = mir_dash(c, r, b->Pw), wa = mir_dash(c, r, a->Pw);
            if (wide) { wb *= 4; wa *= 4; }
            float cb = (b->xl + b->xr) / 2, ca = (a->xl + a->xr) / 2;
            band_quad(wide ? 2 : 4, cb - (wb + 1) / 2, cb + (wb + 1) / 2, ca - (wa + 1) / 2, ca + (wa + 1) / 2, b, a);
        }
    }
}

/* The original's integer row values of the mirror at boundary Pw (road_proj_x_mirror / _y_mirror themselves when
 * comparing), else the continuous projection moved by half a pixel the way the original truncates. */
static float mir_est_x(const cam *c, float Pw, float xw)
{
    float z = mir_z(c, Pw), dx = mir_dx(c, Pw);
    if (c->compare) return road_proj_x_mirror((s16)lroundf(z), (s16)lroundf(xw), (s16)(int)lroundf(dx));
    float u = (c->cx - xw) / z - dx / 4.0;
    return 280.0 - u + 0.5 * fminf(fmaxf(u / 2.0, -1.0), 1.0);
}

static float mir_est_y(const cam *c, float Pw, float yw)
{
    float z = mir_z(c, Pw), dy = mir_dy(c, Pw);
    if (c->compare) return road_proj_y_mirror((s16)lroundf(z), (s16)lroundf(yw), (s16)(int)lroundf(dy));
    float v = dy + (yw - c->cy) / z;
    return v - 0.5 * fminf(fmaxf(v / 2.0, -1.0), 1.0) + 36.0;
}

/* the banner and mirror_objects of mirror row r at its far boundary, the objects of `pass` */
static void mir_objects(const cam *c, int r, int pass)
{
    int bnd = c->seg0 - r;
    float Pw = bnd * 64.0, l, rr;
    edges_at(c, bnd, &l, &rr);
    enh_bound b = { .s = bnd - 1, .k = r, .mirror = true, .depth = (r >> 1) + 7, .depth_banner = r + 5, .fade = 1.0 };
    b.z = 4.0 * mir_z(c, Pw);
    int R = enh_mirror_rows(c->compare);
    if (R > 7) b.fade = enh_far_fade(32.0 * R, b.z);                   /* the extended mirror's far end (§4 item 7) */
    b.xl = mir_est_x(c, Pw, l);
    b.xr = mir_est_x(c, Pw, rr);
    b.xol = mir_est_x(c, Pw, 0x5DC - c->w);
    b.xor_ = mir_est_x(c, Pw, 0xCE4 + c->w);
    b.xfl = mir_est_x(c, Pw, (c->w - 0x177) * -4);
    b.xfr = mir_est_x(c, Pw, (c->w + 0x339) * 4);
    b.y = mir_est_y(c, Pw, SURFACE);
    b.ye = mir_est_y(c, Pw, EYE);
    b.y16 = c->compare ? 4 * b.ye - 3 * b.y : mir_est_y(c, Pw, 1600.0);
    /* row r - 1 (nearer); its banks: the original reads them forward (BL[seg + r - 1], RR 0ce4), the enhanced
     * mirror behind the car at that boundary (§3.7) */
    float Pq = c->compare ? Pw + 64.0 : fminf(Pw + 64.0, c->P - 64.0), yq = mir_est_y(c, Pq, SURFACE);
    int sq = c->compare ? c->seg0 + r - 1 : bnd + 1;
    float ybl = mir_est_y(c, Pq, SURFACE - enh_bank(TRK_BL, sq)), ybr = mir_est_y(c, Pq, SURFACE - enh_bank(TRK_BR, sq));
    b.flat_l = c->compare ? ybl == yq : fabsf(ybl - yq) < 0.5;
    b.flat_r = c->compare ? ybr == yq : fabsf(ybr - yq) < 0.5;
    b.dash = mir_dash(c, r, Pw);
    enh_objects(&b, c->mode, c->compare, pass);
}

/* mirror_hillsides of row r (boundaries bnd far, bnd + 1 near; `a` near, `b` far slice; q1: the row nearer still) on the
 * left (sg -1) and the right (sg +1): colour 0x0E from the mirror's edge to the outer band; fall_dir shifts the edge */
static void mir_hill_side(const cam *c, int bnd, int r, const sect *a, const sect *b, bool first, const sect *q1, int sg)
{
    bool left = sg < 0;
    u16 bank = left ? TRK_BL : TRK_BR;
    int o = c->fall_dir;
    if (left ? o == 1 : o == -1) return;
    if (enh_bank(bank, bnd + 1) == 0) return;                   /* no bank at the near boundary */
    bool row1 = r == 1, bridge_prev = flag(TRK_C, bnd - 2) & 0x80;
    float xq = left ? a->xol : a->xor_, xp = left ? b->xol : b->xor_;
    float ytq = left ? a->yt1 : a->yt2, ytp = left ? b->yt1 : b->yt2;
    float xe = left ? MIR_X0 : 319.0;
    if (row1 || !bridge_prev) {
        float t[8] = { xe, ytq, xq - o, a->y, xp + o, b->y, xe, ytp };
        poly(0x0E, 4, t);
        if (row1 && (left ? xp > MIR_X0 : xp < 0x13F)) { float u[6] = { xe, ytq, xp, b->y, xe, b->y }; poly(0x0E, 3, u); }
        else if (!row1 && enh_bank(bank, bnd + 2) == 0 && (left ? xq > MIR_X0 : xq < 0x13F)) {
            float u[6] = { xe, ytq, xq, a->y, xe, a->y };
            poly(0x0E, 3, u);
        }
    } else {
        if (first && q1) {
            float xq1 = left ? q1->xl : q1->xr, xl_q = left ? a->xl : a->xr;
            float v[12] = { xe, ytq, xq, a->y, xl_q, a->y, xq1, q1->y, xq1, MIR_Y1, xe, MIR_Y1 };
            poly(0x0E, 6, v);
        }
        float t[8] = { xe, ytq, xq, a->y, xp, b->y, xe, ytp };
        poly(0x0E, 4, t);
    }
}

/* mulholland_extras_mirror: trusses of a bridge, its end, the guard rail (R & 0x8000, post phase from the front's run) */
static void mir_extras(const cam *c, int bnd, int r, const sect *p, int n)
{
    int s = bnd - 1;
    u16 C = flag(TRK_C, s), R = flag(TRK_R, s);
    const sect *a = &p[0], *b = &p[n];
    float N = mir_dash(c, r, b->Pw) + 1;
    if ((C & 0x80) && (flag(TRK_C, s + 1) & 0x80) && (flag(TRK_C, s + 2) & 0x80))
        truss_n(N, (bnd & 1) == 0 || r >= 4, a, b, c);
    if ((C & 0x80) && !(flag(TRK_C, s - 1) & 0x80)) truss_end_n(c, N, b);
    if (!((R & 0x8000) && !(C & 0x80))) return;
    int phase = 0;                                              /* DS_rail_phase_seg: the front's farthest run start */
    for (int k = 14; k > 0; k--)
        if ((flag(TRK_R, c->seg0 + k) & 0x8000) && !(flag(TRK_R, c->seg0 + k - 1) & 0x8000)) { phase = c->seg0 + k; break; }
    if (((phase - r - c->seg0 + 7) & 1) != 0) {
        float x = a->xor_ - 10.0 / (r + 1);
        edged(5, 2, x, a->yk - 1, x, a->y, a->xor_, a->y, a->xor_, a->yk - 1);
    }
    for (int i = 0; i < n; i++) {
        const sect *q = &p[i], *f = &p[i + 1];
        quad(5, f->xor_, f->ye - 1, f->xor_, f->yk, q->xor_, q->yk, q->xor_, q->ye - 1);
        thin(2, f->xor_, f->ye - 1, q->xor_, q->ye - 1);
        thin(2, f->xor_, f->yk, q->xor_, q->yk);
    }
}

/* A picture rectangle of page 2's mirror store (mirror_bg_prepare: frame 2005 at (232, 102), sky 2063 below it) at
 * (dx, dy) logical, opaque */
static void mir_pic(int sx, int sy, int w, int h, float dx, float dy)
{
    const u8 *px;
    int iw;
    if (enh_sheet(ds_far(DS_page2_ptr), sx, sy, w, h, &px, &iw)) ras_image(px, iw, h, dx, dy, dx + iw, dy + h, 200.0);
}

static void mirror(const cam *c0)
{
    cam c = *c0;
    c.mirror = true;
    c.synth = INT_MAX;          /* the mirror reads the table as the original (no table end behind the car) */
    /* mirror_draw_backdrop: the frame (rows 20..44) and its mount (rows 18, 19), then the horizon strip of the sky
     * picture above the far row, inside the mirror's clip */
    mir_pic(232, 104, 80, 25, 240, 20);
    mir_pic(264, 102, 16, 2, 272, 18);
    ras_clip(MIR_X0, MIR_Y0, 320, MIR_Y1);
    /* rows beyond the original's 7 (§4 item 7, min(N / 2, 30)) never reach below segment 0 (the strip: its start 8) */
    int R = enh_mirror_rows(c.compare), lo = (c.mode & 6) ? 8 : 0;
    while (R > 7 && c.seg0 - R - 2 < lo) R--;
    float Pfar = 64.0 * (c.seg0 - R);
    if (c.mode == 0x10) mir_pic(232, 129, 80, 20, 240, proj_y(&c, Pfar, SURFACE) - 10.0);
    else mir_pic(232, 129, 80, 2, 240, proj_y(&c, Pfar, SURFACE) - 2.0);
    bool split = !c.compare;
    for (int r = R; r >= 1; r--) {
        int bnd = c.seg0 - r, s = bnd - 1;
        float Pb = 64.0 * bnd, Pa = 64.0 * (bnd + 1);
        if (!c.compare) Pa = fminf(Pa, c.P - 64.0);
        if (Pa <= Pb) continue;
        float ya = proj_y(&c, Pa, SURFACE), yb = proj_y(&c, Pb, SURFACE);
        int n = c.compare ? 1 : clampi((int)ceilf(fabsf(ya - yb)), 1, 4);
        sect p[5];
        for (int i = 0; i <= n; i++) p[i] = section(&c, Pa + (Pb - Pa) * i / n);
        if (split) mir_objects(&c, r, ENH_OBJ_OUT);
        if (c.mode == 0x10 && !(flag(TRK_C, s) & 0x80)) {
            sect q1 = section(&c, c.compare ? Pa + 64.0 : fminf(Pa + 64.0, c.P - 64.0));
            for (int i = 0; i < n; i++)
                for (int sg = -1; sg <= 1; sg += 2) mir_hill_side(&c, bnd, r, &p[i], &p[i + 1], i == 0, &q1, sg);
        }
        mir_walls(&c, s, p, n, true);
        mir_walls(&c, s, p, n, false);
        mir_band(&c, s, r, p, n);
        mir_objects(&c, r, split ? ENH_OBJ_IN : ENH_OBJ_ALL);
        if (c.mode == 0x10) mir_extras(&c, bnd, r, p, n);
        if (!c.compare && (c.mode & 0x11) && r < R && bnd == c.snap->starter_seg && !DSW(DS_start_active)) {
            float l, rr, Pw = 64.0 * bnd;                      /* starter_draw_mirror: centred on the road of the boundary */
            edges_at(&c, bnd, &l, &rr);
            enh_prop_starter(floorf((mir_est_x(&c, Pw, l) + mir_est_x(&c, Pw, rr)) / 2.0), mir_z(&c, Pw));
        }
        if (bnd == (int)floorf(c.O / 64.0) || (r == 1 && bnd < (int)floorf(c.O / 64.0))) car_mirror(&c);
        if (c.mode & 6) {                                       /* barrier walls: the stripe index runs with seg + r (sic) */
            u8 col = abs(7 - r - c.seg0 + c.start_seg) % 10 < 5 ? 0x0B : 4;
            for (int i = 0; i < n; i++) {
                const sect *a = &p[i], *b = &p[i + 1];
                quad(col, b->xr, b->yk, b->xr, b->y + 1, a->xr, a->y + 1, a->xr, a->yk);
                quad(col, b->xl, b->yk, b->xl, b->y + 1, a->xl, a->y + 1, a->xl, a->yk);
            }
        }
    }
    /* mirror_finish: the frame lines (view clip) */
    ras_clip(0, ENH_VIEW_Y0, 320, ENH_VIEW_Y1);
    ras_rect(0xF0, MIR_Y0, 0xF1, MIR_Y1, 2);
    ras_rect(0xF1, MIR_Y0, 0xF2, MIR_Y1, 4);
    ras_rect(0x13E, MIR_Y0, 0x13F, MIR_Y1, 4);
    ras_rect(0x13F, MIR_Y0, 0x140, MIR_Y1, 2);
}

/* ---------------------------------------------------------------- view */

static cam cam_of(const enh_view *v)
{
    const enh_snap *b = v->b;
    cam c = {
        .P = v->P, .cx = v->cam_x, .cy = v->cam_y, .hgt = v->cur_height, .heading = v->heading,
        .bounce = v->bounce_y, .mode = b->mode, .seg0 = (int)floorf(v->P / 64.0), .racing = b->racing != 0,
        .compare = v->compare, .start_seg = b->start_seg, .w = b->mode == 0x10 ? 200 : 400,
        .far_z = 4.0 + (FAR_Z - 4.0) * enh.draw_scale, .rows = v->rows, .synth = INT_MAX, .fall_dir = b->fall_dir,
        .snap = b, .O = v->O, .lat = v->opp_lat,
    };
    if (c.mode == 0x10)                                         /* front_hillsides: a bridge ends within rows 10..14 */
        for (int k = 14; k > 9; k--)
            if (!(flag(TRK_C, c.seg0 + k) & 0x80) && (flag(TRK_C, c.seg0 + k - 1) & 0x80)) c.l16 = true;
    if (c.racing && !c.compare && b->track_end > 0) {          /* modes 0, 1: the finish at track_end 0x17C */
        c.synth = b->track_end;
    }
    return c;
}

/* Compare aid (SR_ENH_COMPARE=3): the original's front rows next to the float projection of the same boundaries
 * (rows 8..14 from its row tables, rows 1..7, which the mirror projection overwrites, through road_proj_x / _y),
 * then the mirror's rows 0..7 from the tables. */
void enh_road_debug_rows(const enh_view *v, FILE *f)
{
    if (v->b->mode == 8) { enh_aq_debug_rows(f); return; }
    cam c = cam_of(v);
    int q = (int)fmodf(c.P, 64.0) >> 3;
    for (int k = 0; k <= 14; k++) {        /* the flags and banks of the view (S2.4) */
        int s = c.seg0 + k;
        fprintf(f, "  seg %d L %04x R %04x C %04x BL %d BR %d\n", s, flag(TRK_L, s), flag(TRK_R, s), flag(TRK_C, s),
                enh_bank(TRK_BL, s), enh_bank(TRK_BR, s));
    }
    for (int k = 1; k < 8; k++) {           /* rows 1..7 recomputed with the original's own projection */
        int s = c.seg0 + k;
        s16 z = (s16)(12 - q + 8 * (k - 1)), dx = (s16)(DSS(DS_heading) - TRKS(TRK_YAW, s));
        s16 dy = (s16)(DSS(DS_cur_height) - TRKS(TRK_HGT, s));
        float l, r;
        edges_at(&c, s, &l, &r);
        sect p = section(&c, s * 64.0);
        fprintf(f, "  row %2d orig xl %4d xr %4d y %3d | float xl %7.2f xr %7.2f y %6.2f\n", k,
                road_proj_x(z, (s16)l, dx), road_proj_x(z, (s16)r, dx), road_proj_y(z, 2000, dy),
                p.xl - XOFF, p.xr - XOFF, p.y - YOFF);
    }
    cam m = c;                              /* the mirror's rows 0..7: the original's tables hold them now */
    m.mirror = true;
    for (int k = 0; k < 8; k++) {
        sect p = section(&m, (c.seg0 - k) * 64.0);
        fprintf(f, "  mirror row %d orig xl %4d xr %4d xol %4d xor %4d y %3d | float xl %7.2f xr %7.2f xol %7.2f xor %7.2f"
                " y %6.2f\n", k, DSS((u16)(DS_row_xl + 2 * k)), DSS((u16)(DS_row_xr + 2 * k)),
                DSS((u16)(DS_row_xol + 2 * k)), DSS((u16)(DS_row_xor + 2 * k)), DSS((u16)(DS_row_y + 2 * k)),
                p.xl - XOFF, p.xr - XOFF, p.xol - XOFF, p.xor_ - XOFF, p.y - YOFF);
    }
    for (int k = 8; k <= 14; k++) {
        float Pw = k == 14 ? c.P + (FAR_Z - 4.0) * 8.0 : (c.seg0 + k) * 64.0;
        sect p = section(&c, Pw);
        fprintf(f, "  row %2d orig xl %4d xr %4d y %3d | float xl %7.2f xr %7.2f y %6.2f\n", k,
                DSS((u16)(DS_row_xl + 2 * k)), DSS((u16)(DS_row_xr + 2 * k)), DSS((u16)(DS_row_y + 2 * k)),
                p.xl - XOFF, p.xr - XOFF, p.y - YOFF);
    }
}

bool enh_road_render(const enh_view *v)
{
    const enh_snap *b = v->b;
    if (b && b->mode == 8 && !b->fallen) return enh_aq_render(v);     /* S2.7: the Aqueduct's own renderer */
    if (!b || (b->mode != 0 && b->mode != 1 && b->mode != 2 && b->mode != 4 && b->mode != 0x10) || b->fallen)
        return false;                                           /* S2.3: modes 0 and 1; S2.4: 2, 4, 0x10 */
    if (!ras_begin(enh.res_scale * enh.aa)) return false;
    cam c = cam_of(v);
    sky(&c);
#ifdef STREET_ROD_GE
    if (sr_ge_recording) sr_ge_background(ras_row(0));
#endif
    road(&c);
    if (v->compare) mirror(&c);
    return true;
}
