/* Street Rod 2 Enhanced: road-side objects of the normal renderer at continuous scale (ENHANCED.md §3.6).
 * S2.5 (modes 0 and 1, pulled forward before S2.4): the calls of front_objects (race_road.c) for one boundary, and
 * (S2.6) of the mirror's banner and mirror_objects, re-made from the track flags at the interpolated view: SR2_SHP
 * format-A shapes (trees, posts, signs' shapes, banner text, hydrants), the location sign and billboard pictures, the
 * banner frame. Positions are the original's row values (enh_bound, from enh_road.c).
 *
 * Sizes (user feedback after the S2.5 playtest: the original's LOD steps, and the interpolation between their sizes,
 * made objects balloon on approach): every object is drawn from its largest, most detailed source only, scaled by
 * true perspective. The largest LOD l of a shape set is the one the original uses at row depth 9 - l (scenery_draw_shape:
 * LOD 9 - depth, clamped), whose boundary lies at z = 8 (9 - l) + 4 - q, on average zt = 8 (9 - l) + 0.5; at depth z
 * the shape is scaled by zt / z (a far object, which the original draws `off` rows smaller, at zt / (z + 8 off)). Signs:
 * the sheet's largest size (64 x 43, row depth 3) by 24.5 / z. The original's smaller LODs are smaller than that
 * perspective (rows 7..9: 45..75 %), the reason for the fast growth. Where the original draws nothing (a set with
 * fewer LODs than its clamp, near rows) nothing is drawn. The mirror's depth is 4 x its own z in these units (its
 * projection is the front's at 1/4 scale). When comparing (SR_ENH_COMPARE >= 2) the drawn LOD and the byte placement
 * of pictures are the original's, so the compare tool can match edges; the perspective size of each shape is then
 * measured from the spans it would emit and checked against the largest LOD's height x zt / z.
 *
 * Painter's order (the fix for trees showing in front of buildings): the original draws all objects of a row after
 * the row's walls, so an object standing behind the building line (the outer edge XOL / XOR and beyond: trees,
 * lamp posts, the far trees of flag 2) is painted over the wall of its own segment, which is nearer. Those objects
 * (ENH_OBJ_OUT) are drawn before the segment's walls in the town; the objects between the road and the building line
 * (ENH_OBJ_IN) after the band, as the original. The street drag race has no walls (one pass, the original's order). */
#include "enh_internal.h"
#include "enhanced.h"

#include <math.h>
#include <string.h>

#include "game/race_int.h"
#include "platform/ega.h"

typedef struct {
    const enh_bound *b;
    int mode, pass;
    bool compare;
} ctx;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float half(const ctx *c, float v) { return c->compare ? floorf(v / 2) : v / 2; }
static bool drawn(const ctx *c, bool out) { return c->pass == ENH_OBJ_ALL || c->pass == (out ? ENH_OBJ_OUT : ENH_OBJ_IN); }
static int rec_kind(const ctx *c, int kind) { return kind + (c->b->mirror ? 4 : 0); }

/* ---------------------------------------------------------------- SR2_SHP format A (shape_draw, race_gfx.c) */

typedef struct {
    FarPtr sp;                  /* spans {s8 dx, s8 dy, s8 w} */
    u8 col[6], cnt[6];
    int groups;
    float x0, x1, y0, y1;      /* bounding box of the pixels (side 1, relative to the anchor) */
} lod_t;

static int lod_count(int type)
{
    FarPtr set = ds_far((u16)(DS_shp_a_ptrs + 4 * type));
    return far_is_null(set) ? 0 : (s16)rd16(set.seg, set.off);
}

static bool lod_read(int type, int l, lod_t *o)
{
    FarPtr set = ds_far((u16)(DS_shp_a_ptrs + 4 * type));
    if (far_is_null(set) || l < 0 || l >= (s16)rd16(set.seg, set.off)) return false;
    FarPtr recs = far_rd(set.seg, (u16)(set.off + 2));
    u16 r = (u16)(recs.off + l * 16);
    memset(o, 0, sizeof *o);
    o->sp = far_make(rd16(recs.seg, (u16)(r + 14)), rd16(recs.seg, (u16)(r + 12)));
    o->x0 = o->y0 = 1e9;
    o->x1 = o->y1 = -1e9;
    FarPtr sp = o->sp;
    for (int g = 0; g < 6; g++) {
        o->col[g] = rd8(recs.seg, (u16)(r + 2 * g));
        o->cnt[g] = rd8(recs.seg, (u16)(r + 2 * g + 1));
        if (!o->cnt[g]) break;
        o->groups++;
        for (int n = o->cnt[g]; n; n--, sp.off = (u16)(sp.off + 3)) {
            int dx = (s8)rd8(sp.seg, sp.off), dy = (s8)rd8(sp.seg, (u16)(sp.off + 1)), w = (s8)rd8(sp.seg, (u16)(sp.off + 2));
            int a = dx < dx + w ? dx : dx + w, b = (dx > dx + w ? dx : dx + w) + 1;
            o->x0 = fminf(o->x0, a); o->x1 = fmaxf(o->x1, b);
            o->y0 = fminf(o->y0, dy); o->y1 = fmaxf(o->y1, dy + 1);
        }
    }
    return o->groups > 0;
}

/* The spans of LOD `L` at anchor (x, y) scaled by f about the anchor pixel's centre (f = 1: the original's pixels;
 * side < 1 mirrored: x - dx - w .. x - dx). Drawn when `draw`; the vertical extent of what it covers in *y0, *y1. */
static void spans(const lod_t *L, float x, float y, int side, float f, bool draw, float *y0, float *y1)
{
    float cx = x + 0.5, cy = y + 0.5;
    *y0 = 1e9;
    *y1 = -1e9;
    FarPtr sp = L->sp;
    for (int g = 0; g < L->groups; g++)
        for (int k = L->cnt[g]; k; k--, sp.off = (u16)(sp.off + 3)) {
            int dx = (s8)rd8(sp.seg, sp.off), dy = (s8)rd8(sp.seg, (u16)(sp.off + 1)), w = (s8)rd8(sp.seg, (u16)(sp.off + 2));
            float a = dx < dx + w ? dx : dx + w, b = (dx > dx + w ? dx : dx + w) + 1;
            if (side < 1) { float t = 1 - b; b = 1 - a; a = t; }
            float xa = cx + f * (a - 0.5), xb = cx + f * (b - 0.5), ya = cy + f * (dy - 0.5), yb = cy + f * (dy + 0.5);
            if (draw) ras_rect(xa, ya, xb, yb, L->col[g]);
            *y0 = fminf(*y0, ya);
            *y1 = fmaxf(*y1, yb);
        }
}

/* scenery_draw_shape / scenery_draw_hydrant at anchor (x, y) (the original's integer row values, continuous), with the
 * original's depth argument `depth` + `off` (its LOD 9 - depth - off, clamped to 0..maxlod) */
static void shape(const ctx *c, float x, float y, int depth, int off, int side, int type, bool noclamp, int maxlod,
                  bool out, int kind)
{
    if (!drawn(c, out)) return;
    if (c->b->gclip && (x < c->b->gxl || x > c->b->gxr)) return;     /* no ground under it */
    if (!noclamp && y > 0x65) y = 0x65;
    int d = depth + off;
    if (c->compare) enh_compare_obj(false, rec_kind(c, kind), type, d, side, x, y, 0);
    int n = lod_count(type), lo = clampi(9 - d, 0, maxlod);
    if (lo >= n) return;                                            /* the original draws nothing at this depth */
    int lt = n - 1 < maxlod ? n - 1 : maxlod;                       /* the largest LOD */
    float zt = 8.0 * (9 - lt) + 0.5, ze = c->b->z + 8.0 * off;    /* its row depth; this object's */
    if (ze < 1.0) return;
    lod_t T, O;
    if (!lod_read(type, lt, &T)) return;
    float y0, y1;
    if (!c->compare) { spans(&T, x, y, side, zt / ze, true, &y0, &y1); return; }
    if (lod_read(type, lo, &O)) spans(&O, x, y, side, 1.0, true, &y0, &y1);       /* the edge compare */
    spans(&T, x, y, side, zt / ze, false, &y0, &y1);                /* the perspective size, measured */
    float expect = (T.y1 - T.y0) * zt / ze;
    enh_compare_size(fabsf((y1 - y0) - expect), 0.5);
}

/* ---------------------------------------------------------------- pictures (scenery_draw_sign_picture) */

static u8 img[200 * 320];

/* A w x h pixel rectangle at (sx, sy) of the picture or page `d`, whole bytes as the blit (opaque). */
static bool sheet_read(FarPtr d, int sx, int sy, int *w, int h)
{
    if (far_is_null(d)) return false;
    FarPtr pl = desc_planes(d);
    bool vram = desc_type(d) == DESC_TYPE_EGA;
    int stride = vram ? EGA_BYTES_PER_LINE : desc_stride(d), dh = desc_h(d);
    int bx = sx >> 3, bw = *w >> 3;
    u16 base = vram ? (u16)((pl.seg - 0xA000) * 16 + pl.off) : pl.off;     /* a page: A000 / A200 / A400 / A600 */
    if (bx + bw > stride) bw = stride - bx;
    if (sy + h > dh) h = dh - sy;
    if (bw <= 0 || h <= 0 || bw * 8 > 320 || h > 200) return false;
    *w = bw * 8;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < *w; i++) {
            u16 o = (u16)(base + (sy + j) * stride + bx + (i >> 3));
            int bit = 7 - (i & 7), v = 0;
            for (int k = 0; k < 4; k++) {
                u8 byte = vram ? ega_plane(k)[o] : rd8(pl.seg, (u16)(o + k * stride * dh));
                v |= (byte >> bit & 1) << k;
            }
            img[j * *w + i] = (u8)v;
        }
    return true;
}

bool enh_sheet(FarPtr d, int sx, int sy, int w, int h, const u8 **px, int *wout)
{
    if (!sheet_read(d, sx, sy, &w, h)) return false;
    *px = img;
    *wout = w;
    return true;
}

#define SIGN_TOP 6              /* the sheet's largest size (DS:sign_sizes entry 6: 64 x 43), made for row depth 3 */

static void sign(const ctx *c, float x, float y, int depth, int align, int kind, bool out)
{
    if (!drawn(c, out)) return;
    if (c->compare) enh_compare_obj(false, rec_kind(c, 2), kind, depth, align, x, y, 0);
    FarPtr src;
    int sy;
    if (kind == 1) {
        if (DSS(DS_driver_id) == -2) { sy = 100; src = ds_far(DS_page0_ptr); }
        else                         { sy = 0;   src = ds_far(DS_page2_ptr); }
    } else if (kind == 2 || kind == 3) { sy = 0; src = ds_far((u16)(DS_billboard_pics + 4 * (kind - 2))); }
    else return;
    int size = c->compare ? clampi(9 - depth, 0, SIGN_TOP) : SIGN_TOP;
    u16 e = (u16)(DS_sign_sizes + 6 * size);
    int iw = DSS((u16)(e + 2)), ih = DSS((u16)(e + 4));
    if (!sheet_read(src, DSS(e), sy, &iw, ih)) return;
    float x0, y0, w, h;
    if (c->compare) {
        int xi = (int)lroundf(x), wi = DSS((u16)(e + 2));
        x0 = (float)(((xi - (align == 1 ? wi : align == 0 ? wi >> 1 : 0))) >> 3) * 8;  /* the blit's byte column */
        y0 = lroundf(y) - (align == 0 ? ih / 2 : ih);
        w = iw;
        h = ih;
    } else {
        float f = (8.0 * (9 - SIGN_TOP) + 0.5) / c->b->z;      /* 24.5 / z */
        w = iw * f;
        h = ih * f;
        x0 = x - (align == 1 ? w : align == 0 ? w / 2 : 0);
        y0 = y - (align == 0 ? h / 2 : h);
    }
    ras_image(img, iw, ih, x0, y0, x0 + w, y0 + h, c->b->mirror ? 0x2B : 0x65);    /* blit_clip_*: rows < YMAX */
}

/* ---------------------------------------------------------------- banner (scenery_draw_banner) */

static void banner(const ctx *c, u16 flags, bool cross)
{
    const enh_bound *b = c->b;
    int depth = b->depth_banner;
    float xl = b->xl, xr = b->xr, ytop = b->y16, ybot = b->y;
    if (c->compare) enh_compare_obj(false, rec_kind(c, 3), flags >> 12, depth, 0, xl, ytop, xr);
    float n = b->dash + 1, y1 = ybot + 1;                              /* road_vline: rows ytop .. ybot, clipped */
    ras_rect(xl - n, ytop, xl, y1, 10);                                 /* posts: road_vline at xl - si, xr + si */
    ras_rect(xr + 1, ytop, xr + 1 + n, y1, 10);
    ras_rect(xl, ytop, xr + 1, ytop + n, 10);                           /* the bar: rows ytop .. ytop + si - 1 */
    float mid = half(c, xl + xr);
    if (flags & 0x1000) { shape(c, mid, ytop, depth, 0, 1, 0x0D, false, 6, false, 0); return; }
    if (!(flags & 0x2000)) {
        if (!cross) { shape(c, mid, ytop, depth, 0, 1, (flags & 0x4000) ? 0x0C : 0x0E, false, 6, false, 0); return; }
        sign(c, mid, ytop, depth, 0, (flags & 0x4000) ? 2 : 3, false);
    } else sign(c, mid, ytop, depth, 0, 1, false);
}

/* ---------------------------------------------------------------- front_objects (race_road.c), modes 0 and 1 */

static u16 flag(u16 arr, int s) { return enh_flag(arr, s); }

/* scenery_draw_bush_group (Mulholland L / R & 0x10): five shapes placed from the row-indexed tables DS_bush_group_hw
 * (half width W, height H) and DS_bush_group_dx (A, B2), here read at the continuous row depth; the first rows are
 * pushed outwards by 100 / 40 / 10 px */
static float bush_tab(u16 base, int idx, float d)
{
    d = d < 0 ? 0 : d > 12 ? 12 : d;
    int i = (int)floorf(d), j = i < 12 ? i + 1 : 12;
    float f = d - i;
    s16 a = DSS((u16)(base + 4 * i + idx)), b = DSS((u16)(base + 4 * j + idx));
    return a + (b - a) * f;
}

static void bush_group(const ctx *c, float x, int side)
{
    const enh_bound *b = c->b;
    float df = c->compare ? b->depth : b->mirror ? 7.0 + b->z / 64.0 : (b->z - 4.0) / 8.0, Y = b->y;
    int d = b->depth;
    static const float OFS[4] = { 100, 40, 10, 0 };
    float dd = df < 0 ? 0 : df > 3 ? 3 : df, ofs = c->compare ? (d >= 0 && d <= 2 ? OFS[d] : 0) : OFS[(int)dd] + (OFS[(int)dd + (dd < 3)] - OFS[(int)dd]) * (dd - floorf(dd));
    x += side * ofs;
    float W = bush_tab(DS_bush_group_hw, 2, df), H = bush_tab(DS_bush_group_hw, 0, df);
    float A = bush_tab(DS_bush_group_dx, 0, df), B2 = bush_tab(DS_bush_group_dx, 2, df + 2);
    if (c->compare) { W = (int)W; H = (int)H; A = (int)A; B2 = (int)B2; }
    /* beyond the tables' last row (12, z = 100) the spacings shrink with the perspective instead of staying at row 12's
     * pixels (the back bushes, H above the road, otherwise float over the far ground) */
    else if (!b->mirror && df > 12.0) { float f = 100.0 / b->z; W *= f; H *= f; A *= f; B2 *= f; }
    shape(c, x, Y, d, 0, side, 8, true, 6, false, 0);
    shape(c, W * side + x, Y, d, 0, side, 7, true, 6, false, 0);
    shape(c, (W + A) * side + x, Y, d, 0, side, 8, true, 6, false, 0);
    shape(c, floorf(W / 2) * side + x, Y - H, d, 2, side, 7, true, 6, false, 0);
    shape(c, (floorf(W / 2) + B2) * side + x, Y - H, d, 2, side, 8, true, 6, false, 0);
}

static void front(const ctx *c)
{
    const enh_bound *b = c->b;
    int s = b->s, M = c->mode;
    bool compare = c->compare;
    u16 L = flag(TRK_L, s), R = flag(TRK_R, s), C = flag(TRK_C, s);
    bool cross = C & 1;
    int cpu = DSS(DS_cpu_speed), t = M == 0x10 ? 0x15 : 4, u = M == 0x10 ? 0x16 : 0xB;
    float Y = b->y;
    enum { IN = 0, OUT = 1 };
#define SHAPE(x, off, side, type, noclamp, out) shape(c, (x), Y, b->depth, (off), (side), (type), (noclamp), 6, (out), 0)
    if (M != 0x10 && (L & 0x20) && cpu > 3) SHAPE(b->xl, 0, -1, 0, false, IN);
    if (L & 0x100) {
        if (cross) SHAPE(b->xl, 0, -1, 4, true, IN);
        else if (!(M == 0x10 && !b->flat_l)) {
            if (L & 0x200) SHAPE(2 * b->xol - b->xl, 2, -1, t, true, OUT);
            else SHAPE(b->xol, 0, -1, t, true, OUT);
        }
    }
    if (!(M & 7) && (L & 0x800)) SHAPE(b->xl, 0, 1, 3, false, IN);
    if (!(M & 7) && (L & 0x1000)) SHAPE(b->xl, 0, 1, 6, false, IN);
    if (!(M & 6) && (L & 1)) {
        if (!cross) { if (b->flat_l) SHAPE(b->xl, 0, 1, 1, true, IN); }
        else sign(c, b->xl, half(c, Y + b->ye), b->depth, 1, 1, IN);
    }
    if (M == 0x10 && (L & 0x10)) bush_group(c, b->xl, 1);
    if (cpu > 3) {
        if ((M & 7) || b->flat_l) {
            if (L & 0x40) SHAPE(b->xol, 0, -1, 0xA, false, OUT);
            if (L & 0x80) {
                if (!cross && (L & 2)) SHAPE(half(c, b->xfl + 0), 3, -1, u, false, OUT);
                else SHAPE(b->xol, 0, -1, u, false, OUT);
            }
        }
        if ((M & 7) || b->flat_r) {
            if (R & 0x40) SHAPE(b->xor_, 0, 1, 0xA, false, OUT);
            if (R & 0x80) {
                if (!cross && (R & 2)) SHAPE(half(c, b->xfr + 0x13F), 3, 1, u, false, OUT);
                else SHAPE(b->xor_ + half(c, b->xor_ - b->xr), 0, 1, u, false, OUT);
            }
        }
    }
    if (!cross) {
        if (cpu > 3) {
            if ((M & 1) || b->flat_l) {
                if (L & 4) SHAPE(b->xl - 2, 0, -1, 7, true, IN);
                if (L & 8) SHAPE(b->xl - 2, 0, -1, 8, true, IN);
            }
            if ((M & 1) || b->flat_r) {
                if (R & 4) SHAPE(b->xr + 2, 0, 1, 7, true, IN);
                if (R & 8) SHAPE(b->xr + 2, 0, 1, 8, true, IN);
            }
        }
    } else {
        if (L & 0x40) shape(c, b->xl, Y, b->depth, 0, 1, 2, true, 7, IN, 1);     /* hydrants (set 2, 8 LODs) */
        if (R & 0x40) shape(c, b->xr, Y, b->depth, 0, -1, 2, true, 7, IN, 1);
    }
    if (!cross && (C & 2) && (M != 0x10 || b->flat_r) && !(M & 6)) {
        float span = b->xr - b->xl;
        SHAPE(b->xor_ - (compare ? floorf(span / 8) : span / 8), 0, 1, 9, false, IN);
    }
    if (M != 0x10 && (R & 0x20)) SHAPE(b->xr, 0, 1, 0, false, IN);
    if (R & 0x100) {
        if (cross) SHAPE(b->xr, 0, 1, 4, true, IN);
        else if (!(M == 0x10 && !b->flat_r)) {
            if (R & 0x200) SHAPE(2 * b->xor_ - b->xr, 3, 1, t, true, OUT);
            else SHAPE(b->xor_, 0, 1, t, true, OUT);
        }
    }
    if (R & 0x2000) SHAPE(b->xr, 0, 1, 0x12, false, IN);
    if (R & 0x4000) SHAPE(b->xr, 0, 1, 0x13, false, IN);
    if (R & 0x800) SHAPE(b->xr, 0, 1, 3, false, IN);
    if (R & 0x1000) SHAPE(b->xr, 0, -1, 6, false, IN);
    if (!(M & 6) && (R & 1)) {
        if (!cross) { if (b->flat_r) SHAPE(b->xr, 0, -1, 1, true, IN); }
        else sign(c, b->xr, half(c, Y + b->ye), b->depth, -1, 1, IN);
    }
    if (M == 0x10 && (R & 0x10)) bush_group(c, b->xr, -1);
    if (M == 0x10 && (C & 0x1004)) {      /* the rock (C & 4) and obstacle 0x14 (C & 0x1000): lane x6 of 16 */
        int x6 = (s16)flag(TRK_LANE, s);
        float xs = (compare ? floorf((s16)(x6 * (int)(b->xr - b->xl)) / 16.0) : x6 * (b->xr - b->xl) / 16.0) + b->xl;
        if (C & 4) SHAPE(xs, 0, x6 < 9 ? -1 : 1, 7, true, IN);
        if (C & 0x1000) SHAPE(xs, 0, x6 < 9 ? -1 : 1, 0x14, true, IN);
    }
    if ((C & 0xF000) && !(M & 0x16) && drawn(c, IN)) banner(c, (u16)(C & 0xF000), cross);
    if (C & 0x100) SHAPE(b->xr, 0, 1, 5, false, IN);
    if (C & 0x800) SHAPE(b->xr, 0, 1, 0xF, false, IN);
    if (C & 0x200) SHAPE(b->xr, 0, 1, 0x10, false, IN);
    if (C & 0x400) SHAPE(b->xr, 0, 1, 0x11, false, IN);
#undef SHAPE
}

/* ---------------------------------------------------------------- road_draw_mirror's banner and mirror_objects */

static void mirror(const ctx *c)
{
    const enh_bound *b = c->b;
    int s = b->s, M = c->mode;
    bool compare = c->compare;
    u16 L = flag(TRK_L, s), R = flag(TRK_R, s), C = flag(TRK_C, s);
    bool cross = C & 1;
    int cpu = DSS(DS_cpu_speed), t = M == 0x10 ? 0x15 : 4, u = M == 0x10 ? 0x16 : 0xB;
    float Y = b->y, span = b->xr - b->xl;
    enum { IN = 0, OUT = 1 };
    if ((C & 0xF000) && !(M & 0x16) && drawn(c, IN)) banner(c, (u16)(C & 0xF000), cross);
    if (b->k >= enh_mirror_rows(compare)) return;                       /* mirror_objects: rows 1..6 (S2.9: R - 1) */
#define SHAPE(x, side, type, noclamp, out) shape(c, (x), Y, b->depth, 0, (side), (type), (noclamp), 6, (out), 0)
    if (M != 0x10 && (L & 0x20) && cpu > 3) SHAPE(b->xl, -1, 0, false, IN);
    if (L & 0x100) {
        if (cross) SHAPE(b->xl, -1, 4, true, IN);
        else if (!(M == 0x10 && !b->flat_l)) SHAPE((L & 0x200) ? 2 * b->xol - b->xl : b->xol, -1, t, true, OUT);
    }
    if (!(M & 7) && (L & 0x800)) SHAPE(b->xl, -1, 3, false, IN);
    if (!(M & 7) && (L & 0x1000)) SHAPE(b->xl, 1, 6, false, IN);
    if (!(M & 6) && (L & 1) && b->flat_l) SHAPE(b->xl, 1, 1, true, IN);
    if (!(M & 6) && (R & 1) && b->flat_r) SHAPE(b->xr, -1, 1, true, IN);
    if (cpu > 3) {
        if ((M & 7) || b->flat_l) {
            if (L & 0x40) SHAPE(b->xol, -1, 0xA, false, OUT);
            if (L & 0x80) SHAPE((!cross && (L & 2)) ? half(c, b->xol + 0xF0) : b->xol, -1, u, false, OUT);
        }
        if ((M & 7) || b->flat_r) {
            if (R & 0x40) SHAPE(b->xor_, 1, 0xA, false, OUT);
            if (R & 0x80) SHAPE((!cross && (R & 2)) ? half(c, b->xfr + 0x13F) : b->xor_, 1, u, false, OUT);
        }
    }
    if (!cross) {
        if (cpu > 3) {
            if ((M & 1) || b->flat_l) {
                if (L & 4) SHAPE(b->xl, -1, 7, true, IN);
                if (L & 8) SHAPE(b->xl, -1, 8, true, IN);
            }
            if ((M & 1) || b->flat_r) {
                if (R & 4) SHAPE(b->xr, 1, 7, true, IN);
                if (R & 8) SHAPE(b->xr, 1, 8, true, IN);
            }
        }
    } else {
        if (L & 0x40) shape(c, b->xl, Y, b->depth, 0, -1, 2, true, 7, IN, 1);    /* hydrants */
        if (R & 0x40) shape(c, b->xr, Y, b->depth, 0, 1, 2, true, 7, IN, 1);
    }
    if (M == 0x10 && (L & 0x10)) bush_group(c, b->xl, 1);
    if (M == 0x10 && (R & 0x10)) bush_group(c, b->xr, -1);
    if (!cross && (C & 2) && (M != 0x10 || b->flat_r) && !(M & 6)) SHAPE(b->xor_ - (compare ? floorf(span / 8) : span / 8), 1, 9, false, IN);
    if (M != 0x10 && (R & 0x20)) SHAPE(b->xr, 1, 0, false, IN);
    if (R & 0x100) {
        if (cross) SHAPE(b->xr, 1, 4, true, IN);
        else if (!(M == 0x10 && !b->flat_r)) SHAPE((R & 0x200) ? 2 * b->xor_ - b->xr : b->xor_, 1, t, true, OUT);
    }
    if (R & 0x2000) SHAPE(b->xr, 1, 0x12, false, IN);
    if (R & 0x4000) SHAPE(b->xr, 1, 0x13, false, IN);
    if (R & 0x800) SHAPE(b->xr, 1, 3, false, IN);
    if (R & 0x1000) SHAPE(b->xr, -1, 6, false, IN);
    if ((!(M & 7) && (C & 4)) || (M == 0x10 && (C & 0x1000))) {
        int x6 = (s16)flag(TRK_LANE, s);
        float xs = compare ? floorf((s16)(x6 * (int)span) / 16.0) : x6 * span / 16.0;
        if (!(M & 7) && (C & 4)) SHAPE(xs + b->xl, x6 < 9 ? -1 : 1, 7, true, IN);
        if (M == 0x10 && (C & 0x1000)) SHAPE(xs + b->xl, x6 < 9 ? -1 : 1, 0x14, true, IN);
    }
    if (C & 0x100) SHAPE(b->xr, 1, 5, false, IN);
    if (C & 0x200) SHAPE(b->xr, 1, 0x10, false, IN);
    if (C & 0x400) SHAPE(b->xr, 1, 0x11, false, IN);
    if (C & 0x800) SHAPE(b->xr, 1, 0xF, false, IN);
    /* the starter (mode 1) is drawn by enh_props.c from enh_road.c's mirror() */
#undef SHAPE
}

void enh_objects(const enh_bound *b, int mode, bool compare, int pass)
{
    ctx cc = { .b = b, .mode = mode, .pass = pass, .compare = compare };
    u16 C = flag(TRK_C, b->s);
    if ((C & 4) && (C & 1)) return;
    if (compare && !b->mirror && b->k >= 11) return;                /* the original's rows; live: to the far end */
    ras_alpha(b->fade);
    if (b->mirror) mirror(&cc); else front(&cc);
    ras_alpha(1.0);
}
