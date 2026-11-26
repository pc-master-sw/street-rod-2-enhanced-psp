/* Street Rod 2 Enhanced: the Aqueduct (road mode 8, ENHANCED.md §3.3, S2.7). The original's separate renderer
 * (race_aq.c: aq_project_front / _mirror, aq_draw_scene) re-made in float at the interpolated view:
 *
 * The channel is straight and its cross-section is the same at every depth: floor edges x 0x5C8 + XL .. 0xCF8 + XR at
 * height 2000 + FH, wall tops x 0x438 + TXL .. 0xE88 + TXR at height 0x67C + TH, where XL .. TH are the channel
 * deformation of the frame (aq_climb_left / _right while the car climbs a wall; zero otherwise). The projection is
 * road_proj_x / _y with dx = the heading and dy = cur_height - pitch for every depth (no YAW, no HGT), then the roll:
 * rotate_about_centre about the far floor point of the original's row 14 (z = 116; the floor's middle, its left or its
 * right end while climbing), cos / sin x 255/256 as sin256. So walls, floor, the trough and the features are planar
 * quads projected exactly (no slices). Segment s carries AQ[s] for the band between boundaries s - 1 and s (the
 * original's band k = AQ[seg + 14 - k]), at depth z = (Pw - P) / 8 + 4 without the original's truncation of the
 * substep and without its pinned rows 12..14; the far end is z = 4 + 8 x --draw-distance, past the finish
 * (track_end) the channel is plain (joints, trough gaps and openings only, §8 item 4), past the table its pattern.
 *
 * Painter's order as aq_draw_scene: night sky (colour 1, stars, sky shapes), concrete (colour 5: below the far floor
 * line, walls, floor), floor edges black, wall tops white; per band far to near: white band (0x400), joint lines
 * (0x01, at the band's far boundary), wall openings (0x104 left, 0x208 right: window 1/4..3/4 of the wall, door 0..3/4;
 * colour 2, black outline), format-B floor shapes (0x20 at 1/4 of the floor, 0x40 at 7/8); the centre trough (colour 2,
 * black outline) interrupted over the bands with bit 0x02 (the original handles only the first such run); then per
 * segment far to near: the low box (0x10, middle third, 1/7 of the wall high), the two pillars (0x80) and the car.
 * Objects fade in over the last segment. Stars, sky shapes and the floor shapes scroll / sit as the original's, the
 * sky at 1/8 of its rate (§3.5); the format-B shapes are drawn from their unrotated variant (3) rotated by the
 * continuous roll, the floor shapes from their largest group at true perspective. The car is its native picture rotated
 * by the continuous bank angle (enh_props.c), not the 7 pre-rotated sets.
 *
 * The mirror is the same renderer with aq_project_mirror (road_proj_x_mirror / _y_mirror, dx = -heading, dy = -dy,
 * wall tops at the race's wall shifts, depth (P - Pw) / 8 from 2 to the original's pinned 56, rotated about its own far
 * floor middle), the original's flags (mirror band [b, b + 1] carries AQ[b - 1]), the backdrop frame, its stars, the
 * starter on the trough's left line and the car. */
#include "enh_internal.h"
#include "enhanced.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "game/race_int.h"
#include "platform/video.h"

#define XOFF      0.5           /* enh_road.c: projected values -> raster coordinates */
#define YOFF    (-0.4)
#define SKY_RATE  (1.0 / 8.0)
#define FAR_Z     116.0         /* the original's row 14: the far end at --draw-distance 14, the roll centre */
#define HAZE_Z0   164.0         /* haze from 20 segments on (enh_haze_level): the channel is cut there in pieces */
#define FRONT_NEAR 4.0          /* row 0 */
#define MIR_NEAR  2.0           /* aq_project_mirror: row 0 at z 2, rows 5..7 pinned at 40, 48, 56 */
#define MIR_FAR   56.0
#define GEOM_NEAR 1.0           /* the walls and the floor start nearer than row 0 (the original extends them) */
#define ROT_K     (255.0 / 256.0)
#define LW        0.4           /* outline / line width, logical pixels (poly_fill's edge colour, clip_line) */
#define SHAPE_ZT  8.5           /* the floor shapes' group 0: row 1 (z = 12 - substep / 8, 8.5 on average) */
#define DEG       (3.14159265358979323846 / 180.0)
#define AQ_N      0x172
#define DS_star_colour_a 0xAD48 /* plane masks of the stars (race_aq.c) */
#define DS_star_colour_b 0xAD4A
#define DS_star_colour_c 0xAD54

typedef struct { float x, y; } pt;

typedef struct {
    bool mirror, compare;
    float P, cx, cy, dx, dy, bounce, heading;
    float xl, xr, xc, yf, yt, xtl, xtr, tl, tr;    /* world cross-section; tl / tr: the trough's edges */
    float rc, rs, rcx, rcy, roll;                  /* roll: K cos, K sin, centre (raw projected), degrees */
    float znear, zfar;
    int seg0, synth;
    float O, lat;
    const enh_snap *b;
} aqc;

/* AQ[s] (aq_build), clamped below; past the table its pattern (joints every 6, trough gaps and openings every 24);
 * past the finish (live front view) without objects */
static u16 aqf(const aqc *c, int s)
{
    if (s < 0) s = 0;
    u16 f;
    if (s < AQ_N) f = TRKW(TRK_AQ, s);
    else {
        int m = s % 24;
        f = (u16)((s % 6 == 1 ? 1 : 0) | (m >= 3 && m <= 6 ? 2 : 0) | (m == 17 ? 4 : 0) | (m == 16 ? 8 : 0));
    }
    if (s > c->synth) f &= 0x30F;
    return f;
}

/* ---------------------------------------------------------------- projection */

static float zof(const aqc *c, float Pw) { return c->mirror ? (c->P - Pw) / 8.0 : (Pw - c->P) / 8.0 + 4.0; }
static float pwof(const aqc *c, float z) { return c->mirror ? c->P - 8.0 * z : c->P + (z - 4.0) * 8.0; }

static float raw_x(const aqc *c, float z, float X)
{
    return c->mirror ? 280.0 + c->dx / 4.0 - (c->cx - X) / z : 160.0 + c->dx - 4.0 * (c->cx - X) / z;
}

/* the original's cdiv truncates towards zero: above the eye (negative numerator) its row is one lower than below */
static float raw_y(const aqc *c, float z, float Y)
{
    float t = c->mirror ? c->dy + (Y - c->cy) / z : 2.0 * c->dy + 4.0 * (Y - c->cy) / z;
    return (c->mirror ? 36.0 : c->bounce + 60.0) + t + (t < 0 ? 1.0 : 0.0);
}

/* rotate_about_centre of a raw projected point, then into raster coordinates */
static pt rot(const aqc *c, float x, float y)
{
    float dx = x - c->rcx, dy = y - c->rcy;
    pt p = { c->rcx + c->rc * dx + c->rs * dy + XOFF, c->rcy + c->rc * dy - c->rs * dx + YOFF };
    return p;
}

static pt P3(const aqc *c, float z, float X, float Y) { return rot(c, raw_x(c, z, X), raw_y(c, z, Y)); }

/* ---------------------------------------------------------------- primitives */

static void poly(const pt *p, int n, u8 col)
{
    float xy[16];
    for (int i = 0; i < n && i < 8; i++) { xy[2 * i] = p[i].x; xy[2 * i + 1] = p[i].y; }
    ras_poly(xy, n, col);
}

static float lw = LW;              /* the line width of this view (comparing: the original's 1 px) */

static void line(pt a, pt b, u8 col)
{
    float dx = b.x - a.x, dy = b.y - a.y, L = hypotf(dx, dy);
    if (L < 1e-9) return;
    float nx = -dy / L * lw / 2, ny = dx / L * lw / 2, ex = dx / L * lw / 2, ey = dy / L * lw / 2;
    pt q[4] = { { a.x + nx - ex, a.y + ny - ey }, { b.x + nx + ex, b.y + ny + ey },
                { b.x - nx + ex, b.y - ny + ey }, { a.x - nx - ex, a.y - ny - ey } };
    poly(q, 4, col);
}

/* poly_fill with an edge colour (edge < 0: none) */
static void poly_e(const pt *p, int n, u8 fill, int edge)
{
    poly(p, n, fill);
    if (edge >= 0 && edge != fill)
        for (int i = 0; i < n; i++) line(p[i], p[(i + 1) % n], (u8)edge);
}

static void quad_e(pt a, pt b, pt d, pt e, u8 fill, int edge)
{
    pt q[4] = { a, b, d, e };
    poly_e(q, 4, fill, edge);
}

/* ---------------------------------------------------------------- format-B shapes (shp_b_draw) */

/* group `group` of format-B set `set`, unrotated variant 3, at the raw anchor (ax, ay) (rotated about the view's
 * centre), its spans scaled by f about the anchor pixel's centre and rotated by the roll */
static void shape_b(const aqc *c, int set, int group, float ax, float ay, float f)
{
    FarPtr st = ds_far((u16)(DS_shp_b_ptrs + 4 * set));
    if (far_is_null(st) || group < 0 || group >= (s16)rd16(st.seg, st.off) || f <= 0) return;
    FarPtr tab = far_rd(st.seg, (u16)(st.off + 4));
    FarPtr g = far_rd(tab.seg, (u16)(tab.off + 4 * group));
    u16 r = (u16)(g.off + 3 * 16);
    FarPtr sp = far_make(rd16(g.seg, (u16)(r + 14)), rd16(g.seg, (u16)(r + 12)));
    pt a = rot(c, ax, ay);
    float ca = cosf(c->roll * DEG), sa = sinf(c->roll * DEG), cx = a.x - XOFF + 0.5, cy = a.y - YOFF + 0.5;
    for (int k = 0; k < 6; k++) {
        int col = rd8(g.seg, (u16)(r + 2 * k)), n = rd8(g.seg, (u16)(r + 2 * k + 1));
        if (!n) return;
        for (; n; n--, sp.off = (u16)(sp.off + 3)) {
            int dx = (s8)rd8(sp.seg, sp.off), dy = (s8)rd8(sp.seg, (u16)(sp.off + 1)), w = (s8)rd8(sp.seg, (u16)(sp.off + 2));
            float u0 = (w < 0 ? dx + w : dx) - 0.5, u1 = (w < 0 ? dx : dx + w) + 0.5, v0 = dy - 0.5, v1 = dy + 0.5;
            float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
            pt q[4];
            for (int i = 0; i < 4; i++) {
                q[i].x = cx + f * (ca * us[i] + sa * vs[i]);
                q[i].y = cy + f * (-sa * us[i] + ca * vs[i]);
            }
            poly(q, 4, (u8)col);
        }
    }
}

/* ---------------------------------------------------------------- sky (aq_draw_sky, aq_draw_mirror_stars) */

/* plot_points of n {x, y} at DS:tab, x + xoff wrapped into [x0, x0 + period), rotated (pure rotation, as the original
 * rotates them only while rolling); one output pixel or two */
static void stars(const aqc *c, u16 tab, int n, float xoff, float x0, float period, u8 col)
{
    float st = c->compare ? 1.0 : fminf(1.0, 2.0 / enh.res_scale), ca = cosf(c->roll * DEG), sa = sinf(c->roll * DEG);
    for (int i = 0; i < n; i++) {
        float x = DSS((u16)(tab + 4 * i)) + xoff, y = DSS((u16)(tab + 4 * i + 2));
        x = x0 + fmodf(fmodf(x - x0, period) + period, period);
        float dx = x - c->rcx, dy = y - c->rcy;
        float rx = c->rcx + ca * dx + sa * dy + 0.5, ry = c->rcy + ca * dy - sa * dx + 0.5;
        ras_rect(rx - st / 2, ry - st / 2, rx + st / 2, ry + st / 2, col);
    }
}

static void sky(const aqc *c)
{
    ras_rect(0, ENH_VIEW_Y0, 320, ENH_VIEW_Y1, 1);
    float h = c->heading, rate = c->compare ? 1.0 : SKY_RATE;     /* comparing: the original's scroll */
    stars(c, 0x7F2E, 18, rate * h, 0, 320, (u8)DSW(DS_star_colour_a));
    stars(c, 0x7F76, 6, rate * h, 0, 320, (u8)DSW(DS_star_colour_b));
    float sc = fmodf(2.0 * rate * h, 320.0);              /* the sky shapes: 2 px per heading unit originally */
    for (int k = -1; k <= 1; k++) {                           /* wrapped copies: no shape pops at the edge */
        float o = sc + 320.0 * k;
        shape_b(c, 0, 0, o + 0x5C, 0x20, 1.0);
        shape_b(c, 1, 0, o + 0xD2, 0x2D, 1.0);
        shape_b(c, 1, 0, o + 0xB4, 0x16, 1.0);
    }
}

/* ---------------------------------------------------------------- channel */

static void channel_piece(const aqc *c, float z0, float z1)
{
    pt L0 = P3(c, z0, c->xl, c->yf), L1 = P3(c, z1, c->xl, c->yf), R0 = P3(c, z0, c->xr, c->yf), R1 = P3(c, z1, c->xr, c->yf);
    pt T0 = P3(c, z0, c->xtl, c->yt), T1 = P3(c, z1, c->xtl, c->yt), U0 = P3(c, z0, c->xtr, c->yt), U1 = P3(c, z1, c->xtr, c->yt);
    quad_e(T0, T1, L1, L0, 5, -1);
    quad_e(U0, U1, R1, R0, 5, -1);
    quad_e(L1, R1, R0, L0, 5, -1);
    line(L1, L0, 0);                                        /* floor edges black, wall tops white */
    line(R1, R0, 0);
    line(T1, T0, 0x0F);
    line(U1, U0, 0x0F);
}

/* the channel; hazed (S2.9) in pieces of 8 segments beyond HAZE_Z0, far to near */
static void channel(const aqc *c)
{
    float z0 = GEOM_NEAR, z1 = c->zfar, yF = raw_y(c, z1, c->yf);
    bool hz = !c->mirror && !c->compare && enh_haze_level(z1, z1) > 0;
    if (hz) ras_haze(enh_haze_level(z1, z1));
    pt h[4] = { rot(c, -4000, yF), rot(c, 4320, yF), rot(c, 4320, 4000), rot(c, -4000, 4000) };
    poly(h, 4, 5);                                          /* below the far floor line (the original's fill_rows) */
    if (!hz) { channel_piece(c, z0, z1); return; }
    for (float zb = z1; zb > z0;) {
        float za = zb > HAZE_Z0 ? fmaxf(HAZE_Z0, zb - 64.0) : z0;
        ras_haze(enh_haze_level(z1, (za + zb) / 2.0));
        channel_piece(c, za, zb);
        zb = za;
    }
    ras_haze(0);
}

/* a point of the wall (left / right) at fraction t from its foot to its top */
static void wall_at(const aqc *c, bool right, float t, float *X, float *Y)
{
    float xb = right ? c->xr : c->xl, xt = right ? c->xtr : c->xtl;
    *X = xb + (xt - xb) * t;
    *Y = c->yf + (c->yt - c->yf) * t;
}

/* the features of band s between depths za (near) and zb (far); zs = the far boundary's depth (joint, shapes) */
static void band(const aqc *c, u16 f, float za, float zb, float zs)
{
    if (!c->mirror && !c->compare) ras_haze(enh_haze_level(c->zfar, (za + zb) / 2.0));
    if (f & 0x400) quad_e(P3(c, za, c->xl, c->yf), P3(c, za, c->xr, c->yf), P3(c, zb, c->xr, c->yf), P3(c, zb, c->xl, c->yf), 0x0F, -1);
    bool in = zs <= c->zfar;
    if ((f & 1) && in) {
        pt l = P3(c, zs, c->xl, c->yf), r = P3(c, zs, c->xr, c->yf);
        line(l, r, 0);
        line(l, P3(c, zs, c->xtl, c->yt), 0);
        line(r, P3(c, zs, c->xtr, c->yt), 0);
    }
    if (f & 0x30C) {                                        /* aq_wall_opening_quad: the right one wins */
        bool right = (f & 0x208) != 0;
        float t0 = (f & 0x0C) ? 0.25 : 0.0, X0, Y0, X1, Y1;
        wall_at(c, right, t0, &X0, &Y0);
        wall_at(c, right, 0.75, &X1, &Y1);
        quad_e(P3(c, za, X0, Y0), P3(c, za, X1, Y1), P3(c, zb, X1, Y1), P3(c, zb, X0, Y0), 2, 0);
    }
    if ((f & 0x60) && in) {                                 /* aq_draw_row_shape: set 2, the largest group */
        float fz = c->mirror ? SHAPE_ZT / (4.0 * zs) : SHAPE_ZT / zs, y = raw_y(c, zs, c->yf);
        ras_alpha(enh_far_fade(c->zfar, zs));
        if (f & 0x20) shape_b(c, 2, 0, raw_x(c, zs, c->xl + (c->xr - c->xl) / 4.0), y, fz);
        if (f & 0x40) shape_b(c, 2, 0, raw_x(c, zs, c->xr - (c->xr - c->xl) / 8.0), y, fz);
        ras_alpha(1.0);
    }
    ras_haze(0);
}

static void trough_run(const aqc *c, float za, float zb)
{
    if (zb <= za) return;
    if (!c->mirror && !c->compare) ras_haze(enh_haze_level(c->zfar, (za + zb) / 2.0));
    quad_e(P3(c, zb, c->tl, c->yf), P3(c, za, c->tl, c->yf), P3(c, za, c->tr, c->yf), P3(c, zb, c->tr, c->yf), 2, 0);
    ras_haze(0);
}

/* aq_draw_box: p[0..7] = far bottom L, R, far top L, R, near bottom L, R, near top L, R (rotated) */
static bool thin(pt a, pt b) { return fabsf(a.x - b.x) <= 2 || fabsf(a.y - b.y) <= 2; }

static void box(const aqc *c, float za, float zb, float x0, float x1, float ytop)
{
    pt p[8] = { P3(c, zb, x0, c->yf), P3(c, zb, x1, c->yf), P3(c, zb, x0, ytop), P3(c, zb, x1, ytop),
                P3(c, za, x0, c->yf), P3(c, za, x1, c->yf), P3(c, za, x0, ytop), P3(c, za, x1, ytop) };
    if (p[4].x > p[0].x) quad_e(p[0], p[2], p[6], p[4], 2, thin(p[0], p[6]) ? -1 : 0);
    if (p[5].x < p[1].x) quad_e(p[1], p[3], p[7], p[5], 2, thin(p[1], p[7]) ? -1 : 0);
    if (p[2].y < p[6].y) quad_e(p[2], p[3], p[7], p[6], 5, thin(p[2], p[7]) ? -1 : 0);
    quad_e(p[5], p[4], p[6], p[7], 2, thin(p[5], p[6]) ? -1 : 0);
}

/* ---------------------------------------------------------------- car, starter */

/* opponent_draw's wall height of the car (climbing a wall) and opp_bank_variant's bank angle, continuous */
static float car_wy(const aqc *c)
{
    const enh_snap *b = c->b;
    float wy = c->yf, lat = c->lat, sxl = c->xl - 0x5C8, sxr = c->xr - 0xCF8, th = (c->yt - 0x67C) - (c->yf - 2000) + 0x154;
    if (lat < sxl + 0x5C8) {
        float den = 400 - b->wall_l_shift + sxl;
        if (den != 0) wy -= (sxl - lat + 0x5A0) * th / den;
    } else if (lat > sxr + 0xCF8) {
        float den = 400 - sxr + b->wall_r_shift;
        if (den != 0) wy -= th * (lat - sxr - 0xCF8) / den;
    }
    return wy;
}

static float car_bank(const aqc *c)
{
    float lat = c->lat, sxl = c->xl - 0x5C8, sxr = c->xr - 0xCF8, e = 0;
    if (lat < sxl + 0x5FA) e = sxl + 0x5FA - lat;
    else if (lat > sxr + 0xCC6) e = lat - sxr - 0xCC6;
    float bank = e >= 100 ? 45 : e * 45 / 100;
    if (lat - (sxr + sxl + 0x12C0) / 2 < 0) bank = -bank;
    bank += c->roll;
    if (c->roll < 0 && c->roll > -90) bank += c->roll / 4;
    return bank;
}

static void car_front(const aqc *c)
{
    const enh_snap *b = c->b;
    int s = b->opp_s[0];
    if (!c->compare && enh_far_opp(b, c->O, c->P)) {          /* beyond the original's rows (S2.9, §4 item 4) */
        float z = (c->O - c->P) / 8.0 + 4.0;
        if (z >= c->zfar) return;
        pt p = rot(c, raw_x(c, z, c->lat), raw_y(c, z, car_wy(c)));
        enh_snap q = *b;
        q.opp_s[0] = 8;
        q.opp_special[0] = false;
        ras_alpha(enh_far_fade(c->zfar, z));
        ras_haze(enh_haze_level(c->zfar, z));
        enh_prop_car_aq(&q, 0, p.x - XOFF, p.y - YOFF, z, car_bank(c));
        ras_haze(0);
        ras_alpha(1.0);
        return;
    }
    if (!b->racing || !b->opp_drawn[0] || s < 0 || s > 8) return;
    float z8 = (c->O - c->P) / 8.0;
    if (s == 0) z8 += 16.0;
    if (z8 < -2.0 && s == 2) z8 += 8.0;
    float z = fabsf(z8) + 4.0, sx = raw_x(c, z, c->lat), sy = raw_y(c, z, car_wy(c));
    if ((s == 2 || s == 3) && sy > 100) sy = 100;
    pt p = rot(c, sx, sy);
    enh_prop_car_aq(b, 0, p.x - XOFF, p.y - YOFF, z, car_bank(c));
}

static void car_mirror(const aqc *c)
{
    const enh_snap *b = c->b;
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
    if (!b->racing || !b->opp_drawn[1] || s < 9 || s > 12) return;
    float zm = fmaxf((c->P - c->O) / 8.0, 1.0);
    float sx = 280.0 + (10.0 + c->dx) / 4.0 - (c->cx - c->lat) / zm, sy = raw_y(c, zm, car_wy(c));
    pt p = rot(c, sx, sy);
    if (far) ras_alpha(enh_far_fade(c->zfar, zm));
    enh_prop_car_aq(b, 1, p.x - XOFF, p.y - YOFF, zm, car_bank(c));
    ras_alpha(1.0);
}

static void starter(const aqc *c, int bst)
{
    float z = fmaxf(zof(c, 64.0 * bst), MIR_NEAR);
    pt p = rot(c, raw_x(c, z, c->tl) + 1, raw_y(c, z, c->yf));
    enh_prop_starter_aq(p.x - XOFF, p.y - YOFF, z, c->roll);
}

/* ---------------------------------------------------------------- views */

/* One band of the view, far to near: its flags, near / far depth (clipped to the view), the far boundary's depth
 * (unclipped: joints, shapes, boxes) and the car's segment drawn after its objects (INT_MIN: none). */
typedef struct { u16 f; float za, zb, zs; int car; } aqband;

/* aq_draw_scene after the sky: channel, bands, trough, then boxes, pillars, starter and car per band */
static void scene(const aqc *c, const aqband *B, int n, int starter_band, int bst)
{
    channel(c);
    for (int i = 0; i < n; i++) band(c, B[i].f, B[i].za, B[i].zb, B[i].zs);
    float far = -1, near = 0;                              /* the trough: runs of bands without bit 0x02 */
    for (int i = 0; i <= n; i++) {
        if (i < n && !(B[i].f & 2)) {
            if (far < 0) far = B[i].zb;
            near = B[i].za;
        } else if (far >= 0) {
            trough_run(c, near, far);
            far = -1;
        }
    }
    int so = (int)floorf(c->O / 64.0);
    for (int i = 0; i < n; i++) {
        const aqband *q = &B[i];
        if ((q->f & 0x90) && q->za < c->zfar) {
            ras_alpha(enh_far_fade(c->zfar, q->za));
            if (!c->mirror && !c->compare) ras_haze(enh_haze_level(c->zfar, q->za));
            if (q->f & 0x10)
                box(c, q->za, q->zs, c->xl + (c->xc - c->xl) / 3, c->xr + (c->xc - c->xr) / 3, c->yf + (c->yt - c->yf) / 7);
            if (q->f & 0x80) {
                float d = (c->xr - c->xl) / 7;
                box(c, q->za, q->zs, c->xl + d, c->xl + d + d / 8, c->yt);
                box(c, q->za, q->zs, c->xr - d / 8 - d, c->xr - d, c->yt);
            }
            ras_alpha(1.0);
            ras_haze(0);
        }
        if (i == starter_band) starter(c, bst);
        if (q->car == so) { if (c->mirror) car_mirror(c); else car_front(c); }
    }
}

static void front(const aqc *c)
{
    aqband B[128];                                          /* up to 121 bands at --draw-distance 120 */
    int n = 0;
    float Pnear = pwof(c, FRONT_NEAR), Pfar = pwof(c, c->zfar);
    for (int s = (int)ceilf(Pfar / 64.0); s > c->seg0 && n < 128; s--) {
        float Pa = fmaxf((s - 1) * 64.0, Pnear), Pb = fminf(s * 64.0, Pfar);
        if (Pb > Pa) B[n++] = (aqband){ aqf(c, s), zof(c, Pa), zof(c, Pb), zof(c, s * 64.0), s };
    }
    sky(c);
    ras_haze_target(ENH_VIEW_Y0 + 1, c->compare ? 0.0 : enh.haze / 100.0);    /* the night sky's fill */
    scene(c, B, n, -1, 0);
    if ((int)floorf(c->O / 64.0) <= c->seg0) car_front(c);          /* opponent_draw at row 0, last */
}

static void mir_pic(int sx, int sy, int w, int h, float dx, float dy)
{
    const u8 *px;
    int iw;
    if (enh_sheet(ds_far(DS_page2_ptr), sx, sy, w, h, &px, &iw)) ras_image(px, iw, h, dx, dy, dx + iw, dy + h, 200.0);
}

static void mirror(const aqc *c)
{
    mir_pic(232, 104, 80, 25, 240, 20);                     /* mirror_draw_backdrop: frame and mount */
    mir_pic(264, 102, 16, 2, 272, 18);
    ras_clip(0xF0, 0x16, 320, 0x2B);
    float sc = -fmodf(2.0 * (c->compare ? 1.0 : SKY_RATE) * c->heading, 320.0) / 4.0;   /* aq_draw_mirror_stars: -(sc >> 2) */
    stars(c, 0x7F8E, 9, sc, 240, 80, (u8)DSW(DS_star_colour_a));
    stars(c, 0x7FB2, 3, sc, 240, 80, (u8)DSW(DS_star_colour_c));
    /* band r between boundaries seg - r (far) and seg - r + 1 carries AQ[seg - r - 1]; the car behind at seg - r
     * (r >= 1) after it; the starter (boundary starter_seg + 1, rows 0..5) after its band */
    aqband B[32];
    int n = 0, bst = c->b->starter_seg + 1, rst = c->seg0 - bst, sb = -1;
    float Pfar = pwof(c, c->zfar), Pnear = pwof(c, c->znear);
    for (int r = (int)ceilf(c->zfar / 8.0); r >= 0; r--) {
        int bnd = c->seg0 - r;
        float Pb = fmaxf(64.0 * bnd, Pfar), Pa = fminf(64.0 * (bnd + 1), Pnear);
        if (Pa <= Pb) continue;
        if (r == rst) sb = n;
        B[n++] = (aqband){ aqf(c, bnd - 1), zof(c, Pa), zof(c, Pb), zof(c, 64.0 * bnd), r >= 1 ? bnd : INT_MIN };
    }
    if (DSW(DS_start_active) || rst < 0 || rst > 5) sb = -1;
    scene(c, B, n, sb, bst);
    ras_clip(0, ENH_VIEW_Y0, 320, ENH_VIEW_Y1);              /* mirror_finish: the frame lines */
    ras_rect(0xF0, 0x16, 0xF1, 0x2B, 2);
    ras_rect(0xF1, 0x16, 0xF2, 0x2B, 4);
    ras_rect(0x13E, 0x16, 0x13F, 0x2B, 4);
    ras_rect(0x13F, 0x16, 0x140, 0x2B, 2);
}

static void set_roll(aqc *c, float cxw)
{
    float z = c->mirror ? MIR_FAR : FAR_Z;
    c->rcx = raw_x(c, z, cxw);
    c->rcy = raw_y(c, z, c->yf);
    c->rc = ROT_K * cosf(c->roll * DEG);
    c->rs = ROT_K * sinf(c->roll * DEG);
}

static aqc dbg_c;                   /* the last front view (compare aid) */

/* Compare aid (SR_ENH_COMPARE=3): the original's front rows 8..14 (the mirror projection overwrote rows 0..7) next to
 * the float values at the same depths, before the roll */
void enh_aq_debug_rows(FILE *f)
{
    const aqc *c = &dbg_c;
    int q = ((int)fmodf(c->P, 64.0)) >> 3;
    for (int i = 8; i <= 14; i++) {
        float z = i == 12 ? 100 : i == 13 ? 108 : i == 14 ? 116 : 12 - q + 8 * (i - 1);
        fprintf(f, "  aq row %2d z %3.0f orig y %4d yt %4d xl %4d xr %4d xtl %4d xtr %4d | float y %7.2f yt %7.2f xl %7.2f"
                " xr %7.2f xtl %7.2f xtr %7.2f  cy %.0f dy %.0f\n", i, z, DSS((u16)(DS_row_y + 2 * i)),
                DSS((u16)(DS_row_yeye + 2 * i)), DSS((u16)(DS_row_xl + 2 * i)), DSS((u16)(DS_row_xr + 2 * i)),
                DSS((u16)(DS_row_xol + 2 * i)), DSS((u16)(DS_row_xor + 2 * i)), raw_y(c, z, c->yf), raw_y(c, z, c->yt),
                raw_x(c, z, c->xl), raw_x(c, z, c->xr), raw_x(c, z, c->xtl), raw_x(c, z, c->xtr), c->cy, c->dy);
    }
}

bool enh_aq_render(const enh_view *v)
{
    const enh_snap *b = v->b;
    if (!ras_begin(enh.res_scale * enh.aa)) return false;
    lw = v->compare ? 1.0 : LW;
    float xl = v->aq_xl, xr = v->aq_xr, mid = (xr + xl + 0x12C0) / 2.0;
    aqc c = {
        .P = v->P, .cx = fminf(fmaxf(v->cam_x, 0x564), 0xD5C), .cy = v->cam_y, .dx = v->heading, .dy = v->aq_dy,
        .bounce = v->bounce_y, .heading = v->heading,
        .xl = 0x5C8 + xl, .xr = 0xCF8 + xr, .xc = mid + xr, .yf = 2000 + v->aq_fh, .yt = 0x67C + v->aq_th,
        .xtl = 0x438 + v->aq_txl, .xtr = 0xE88 + v->aq_txr, .tl = mid + xl - 0x28, .tr = mid + xr + 0x28,
        .compare = v->compare, .roll = v->roll, .znear = FRONT_NEAR, .zfar = 4.0 + (FAR_Z - 4.0) * enh.draw_scale,
        .seg0 = (int)floorf(v->P / 64.0), .synth = b->track_end > 0 ? b->track_end : INT_MAX, .O = v->O, .lat = v->opp_lat,
        .b = b,
    };
    /* aq_draw_front: the roll centre is the far floor's middle, its left end while climbing the left wall, its right
     * end on the right wall (the car's edges at cam_x - 0x6E, cam_x + 0x208) */
    float cl = v->cam_x - 0x6E, cr = v->cam_x + 0x208;
    set_roll(&c, cl > 0x5C8 && cr < 0xCF8 ? (c.xl + c.xr) / 2 : cl <= 0x5C8 ? c.xl : c.xr);
    front(&c);
    dbg_c = c;
    aqc m = c;                                              /* aq_project_mirror */
    m.mirror = true;
    m.dx = -v->heading;
    m.dy = -v->aq_dy;
    m.bounce = 0;
    m.xtl = 0x438 + b->wall_l_shift;
    m.xtr = 0xE88 + b->wall_r_shift;
    m.znear = MIR_NEAR;
    /* the extended mirror (§4 item 7): min(N / 2, 30) rows, never below AQ[0] */
    int R = enh_mirror_rows(v->compare);
    while (R > 7 && m.seg0 - R - 1 < 0) R--;
    m.zfar = R > 7 ? 8.0 * R : MIR_FAR;
    m.synth = INT_MAX;
    set_roll(&m, (m.xl + m.xr) / 2);
    mirror(&m);
    return true;
}
