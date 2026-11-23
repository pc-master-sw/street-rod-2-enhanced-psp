/* Street Rod 2 Enhanced: the opponent / police car, the drag strip's start tree and the starter at continuous scale
 * (ENHANCED.md §3.6, S2.5). The pictures are the game's own masked LIB3 pictures read from DGROUP at render time: the
 * car's levels 0 (side), 1 (rear 3/4), 2 (rear, native; the original shrinks it to levels 3..8) and 9 (front, native;
 * shrunk to 10..12), the start tree's largest sub-picture (lamps are painted into the panel picture as the game goes)
 * and the starter's largest size. The level the original chose (snapshot) selects the picture, never its shrunk copy; the
 * size follows true perspective k / z (k fitted to the original's shrunk levels at their depths, capped at 1.3 x native),
 * the anchor is the original's placement rule at the interpolated depth. Each picture is drawn with the nearest-source
 * sampling of ras_sprite, which the AA resolve then averages. */
#include "enh_internal.h"
#include "enhanced.h"

#include <math.h>

#include "game/race_int.h"
#include "platform/video.h"

#define CAR_K_FRONT 17.0        /* size x depth of the original's levels 3..8 (average 15..19), in native-picture units */
#define CAR_K_MIRROR 15.0       /* levels 10..12 against the mirror's z */
#define CAR_MAX 1.3             /* SRE: at most 1.3 x native */
#define TREE_K 21.7             /* the original's 32 x 58 tree at row 4 (z ~ 32) against the 56 x 87 picture */
#define STARTER_K 12.0          /* starter size 0 at the nearest mirror rows */
#define DEG (3.14159265358979323846 / 180.0)

unsigned enh_prop_n[4];          /* pictures drawn: front car, mirror car, tree, starter (SR_ENH_TRACE) */
static u8 buf[160 * 128];

/* A w x h rectangle at (sx, sy) of the masked picture d as palette indices, 0xFF where the mask is set (transparent);
 * *ow = the width read (whole bytes). */
static bool pic_rect(FarPtr d, int sx, int sy, int w, int h, int *ow, int *oh)
{
    if (far_is_null(d)) return false;
    FarPtr pl = desc_planes(d), m = desc_mask(d);
    bool mask = m.off != 0 || m.seg != 0;
    int stride = desc_stride(d), dh = desc_h(d), bx = sx >> 3, bw = (w + 7) >> 3;
    if (bx + bw > stride) bw = stride - bx;
    if (sy + h > dh) h = dh - sy;
    if (bw <= 0 || h <= 0 || bw * 8 * h > (int)sizeof buf) return false;
    int pw = bw * 8;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < pw; i++) {
            u16 o = (u16)((sy + j) * stride + bx + (i >> 3));
            int bit = 7 - (i & 7), v = 0;
            for (int k = 0; k < 4; k++) v |= (rd8(pl.seg, (u16)(pl.off + o + k * stride * dh)) >> bit & 1) << k;
            if (mask && (rd8(m.seg, (u16)(m.off + o)) >> bit & 1)) v = ENH_TRANSPARENT;
            buf[j * pw + i] = (u8)v;
        }
    *ow = pw;
    *oh = h;
    return true;
}

void enh_prop_car(const enh_snap *b, int view, float sx, float sy, float z)
{
    int s = b->opp_s[view];
    if (s < 0 || s > 12) return;
    int lvl = s <= 1 ? s : s <= 8 ? 2 : 9;
    FarPtr d = ds_far((u16)(DS_opp_sprites + 4 * lvl));
    int w, h;
    if (!pic_rect(d, 0, 0, desc_w(d), desc_h(d), &w, &h)) return;
    float ymax = view ? 0x2B : 0x66, x0, y0, f = 1.0;
    if (b->opp_special[view]) {                             /* crash / contact: the original's own rect */
        x0 = b->opp_rx[view];
        y0 = b->opp_ry[view];
    } else {
        if (s >= 2) f = fminf(CAR_MAX, (view ? CAR_K_MIRROR : CAR_K_FRONT) / fmaxf(z, 1.0));
        float vis = bitmap_visible_width(d) * f, dw = desc_w(d);     /* the picture's width, not the bytes read */
        if (s == 0)      { x0 = fminf(0.0, sx - 0x40 - dw); y0 = sy; }
        else if (s == 1) { x0 = sx - dw;                   y0 = sy - h + 0x11; }
        else {
            float base = (s == 2 || s == 3) ? fminf(sy, 100.0) : sy;
            x0 = sx - ((s > 3 && s < 9) ? dw * f : vis) / 2;
            y0 = base - h * f;
        }
    }
    enh_prop_n[view]++;
    ras_sprite(buf, w, h, x0, y0, x0 + w * f, y0 + h * f, ymax);
}

void enh_prop_tree(float x, float ybase, float z)
{
    int w, h;
    if (!pic_rect(ds_far(DS_start_panel_pic), 0x48, 0, 0x38, 0x57, &w, &h)) return;
    float f = fminf(CAR_MAX, TREE_K / fmaxf(z, 1.0)), x0 = x - w * f / 2, y1 = ybase + 1;
    enh_prop_n[2]++;
    ras_sprite(buf, w, h, x0, y1 - h * f, x0 + w * f, y1, 0x66);
}

void enh_prop_starter(float x, float zm)
{
    FarPtr d = ds_far(DS_start_obj_sizes);
    int w, h;
    if (!pic_rect(d, 0, 0, desc_w(d), desc_h(d), &w, &h)) return;
    float f = fminf(CAR_MAX, STARTER_K / fmaxf(zm, 1.0)), x0 = x - bitmap_visible_width(d) * f / 2;
    enh_prop_n[3]++;
    ras_sprite(buf, w, h, x0, 0x27 - h * f, x0 + w * f, 0x27, 0x2B);    /* the original pins the feet at y 0x27 */
}

/* ---------------------------------------------------------------- the Aqueduct (S2.7) */

/* opponent_draw in mode 8: the level's unrotated picture (race_load_graphics_rot: side, rear 3/4, rear, front; the
 * original rotates them to 7 angles or by the bank angle, and shrinks the rear / front ones) rotated by the continuous
 * bank angle about its bottom centre (the pivot of drv_rotate), which lands at the rotated anchor. The side and 3/4
 * views are drawn only level (the original's v == 3) and placed as in the other modes. */
void enh_prop_car_aq(const enh_snap *b, int view, float sx, float sy, float z, float bank)
{
    int s = b->opp_s[view];
    if (s < 0 || s > 12) return;
    FarPtr d = ds_far(s == 0 ? DS_rot_side : s == 1 ? DS_rot_r34 : s <= 8 ? DS_rot_rear + 12 : DS_rot_front + 12);
    int w, h;
    if (!pic_rect(d, 0, 0, desc_w(d), desc_h(d), &w, &h)) return;
    float ymax = view ? 0x2B : 0x66, dw = desc_w(d), vis = bitmap_visible_width(d);
    enh_prop_n[view]++;
    if (b->opp_special[view]) {                             /* crash / contact: at the original's rect, level */
        ras_sprite(buf, w, h, b->opp_rx[view], b->opp_ry[view], b->opp_rx[view] + w, b->opp_ry[view] + h, ymax);
        return;
    }
    if (s == 0) { float x0 = fminf(0.0, sx - 0x40 - dw); ras_sprite(buf, w, h, x0, sy, x0 + w, sy + h, ymax); return; }
    if (s == 1) { ras_sprite(buf, w, h, sx - dw, sy - h + 0x11, sx - dw + w, sy + 0x11, ymax); return; }
    float f = fminf(CAR_MAX, (view ? CAR_K_MIRROR : CAR_K_FRONT) / fmaxf(z, 1.0)), a = bank * DEG;
    ras_sprite_rot(buf, w, h, vis / 2, h, sx, sy, f, cosf(a), sinf(a), ymax);
}

/* starter_draw_mirror_aq: the largest starter picture, feet at (x, ybase), rotated with the roll */
void enh_prop_starter_aq(float x, float ybase, float zm, float roll)
{
    FarPtr d = ds_far(DS_start_obj_sizes);
    int w, h;
    if (!pic_rect(d, 0, 0, desc_w(d), desc_h(d), &w, &h)) return;
    float f = fminf(CAR_MAX, STARTER_K / fmaxf(zm, 1.0)), a = roll * DEG;
    enh_prop_n[3]++;
    ras_sprite_rot(buf, w, h, bitmap_visible_width(d) / 2.0, h, x, ybase, f, cosf(a), sinf(a), 0x2B);
}
