#ifdef STREET_ROD_GE
#include "engine_road_ge.h"
#endif
/* Street Rod 2 Enhanced: declarations shared by the files of src/enhanced/ (ENHANCED.md §2). */
#ifndef ENH_INTERNAL_H
#define ENH_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mem.h"
#include "types.h"

/* One snapshot per road_frame (ENHANCED.md §3.1), read from DGROUP at enh_frame_begin; the Aqueduct group and the
 * row of the horizon band are computed by the frame's projection and read at enh_layer_fg. */
typedef struct {
    /* time */
    uint64_t t_ns;              /* host clock of the tick at the snapshot (host_tick_ns) */
    u32 frame;                  /* road_frame counter since race start */
    u32 ticks;                  /* DS_ticks */
    /* mode */
    s16 mode, racing, start_seg, track_end, fallen, fall_dir;
    /* player */
    s16 seg, substep, step, cam_x, cam_y, heading, cur_height, bounce_y, wheel;
    /* opponent (also the police car once police_spawn ran) */
    s16 opp_seg, opp_frac, opp_lat, opp_lat_step, opp_model, opp_sprite, opp_variant;
    s8 opp_s[2], opp_v[2];      /* per view (0 front, 1 mirror): sprite level / variant chosen, -1 = no call */
    bool opp_drawn[2];          /* the original blitted it */
    bool opp_special[2];        /* placed by the crash / contact rules (res == 3, opp_contact): the original's rect */
    s16 opp_rx[2], opp_ry[2];   /* the original's blit rect corner (r.dx - r.sx, r.dy) */
    /* police, start */
    s16 police_state, police_seg;
    s16 start_light_pos, start_lights, light_latch, starter_seg;
    /* Aqueduct (at enh_layer_fg) */
    s16 roll, rot_cos, rot_sin, rot_cx, rot_cy, aq_pitch, aq_dy;
    s16 floor_h, top_h, xl, xr, wall_l_shift, wall_r_shift, opp_bank, wall_hit_shift;
    s16 top_xl, top_xr;         /* the wall-top shifts of the frame's deformation (at enh_frame_begin, S2.7) */
    /* palette: 16 attribute registers + overscan */
    u8 pal[17];
    /* compare */
    s16 top_band_row;           /* at enh_layer_fg */
    s8 page;                    /* race page (1, 2) that presented this frame, 0 = not yet */
} enh_snap;

/* The interpolated view at a render time. Continuous values are blended; everything discrete (mode, sprite levels,
 * palette, start lights) is the newer snapshot's (`b`). */
typedef struct {
    const enh_snap *a, *b;      /* the pair around the render time (a == b with one snapshot) */
    uint64_t render_ns;         /* host clock time shown: now - motion delay */
    float x;                   /* position of the render time in the pair, 0..1.5 (> 1 = extrapolated) */
    float w;                   /* smoothstep weight of the pair against the previous pair's line (1 = pair only) */
    bool snap;                  /* the pair is discontinuous: b is shown unblended */
    float P;                   /* player position in sub-steps: seg * 64 + substep */
    float rel_heading;         /* heading - YAW(P) */
    float heading;             /* YAW(P) + rel_heading (raw YAW units, not wrapped) */
    float cam_x, cam_y, cur_height, bounce_y, wheel;
    float O, opp_lat;          /* opponent position in sub-steps, lateral position */
    float roll, rot_cx, rot_cy, aq_pitch, aq_dy;
    float aq_fh, aq_th, aq_xl, aq_xr, aq_txl, aq_txr;     /* the Aqueduct's channel deformation (S2.7) */
    bool compare;               /* the compare view (SR_ENH_COMPARE >= 2): the original's constant-per-row details */
    int rows;                   /* compare aid: front segments drawn, seg + 1 .. seg + rows (0 = all, -1 = none) */
} enh_view;

/* index offset of YAW / HGT / BL / BR / LANE in this race (0xC0 on the drag strip, whose L2 / R2 / C2 flags are read at
 * the segment itself) and the track reads of the enhanced renderer, clamped to the arrays (S2.4) */
extern int enh_toff;
u16 enh_flag(u16 arr, int s);           /* TRK_L / TRK_R / TRK_C (L2 / R2 / C2 on the strip) and TRK_LANE at segment s */
s16 enh_bank(u16 arr, int s);           /* TRK_BL / TRK_BR at boundary s */

/* Stage 2 (S2.9, §4): the coverage of an object at depth z near the far end far_z (fades in over the last 10 % of
 * the distance, at least one segment) and the haze level 0..15 at depth z (smoothstep from 20 segments to far_z) */
float enh_far_fade(float far_z, float z);
/* the opponent of snapshot b is ahead beyond the original's 14 rows (no opponent_draw call this frame; not the police
 * car, which comes from behind): drawn up to the far end from its rear picture (§4 item 4) */
bool enh_far_opp(const enh_snap *b, float O, float P);
/* the mirror's rows behind the car (§4 item 7): the original's 7, live min(N / 2, 30); and whether the opponent or the
 * police car behind lies beyond the original's 7 rows within them (no mirror call this frame), drawn from its front
 * picture */
int enh_mirror_rows(bool compare);
bool enh_far_opp_mirror(const enh_snap *b, float O, float P);
int enh_haze_level(float far_z, float z);
/* YAW / HGT at a continuous position P (sub-steps), linear between segments; indices clamped to the track (§1). */
float enh_track_yaw(float P);
float enh_track_hgt(float P);
/* The interpolated view at host time `now_ns`; false when no snapshot exists (no race frame yet). */
bool enh_interp(uint64_t now_ns, enh_view *out);

/* ---- raster (enh_raster.c, §3.4): palette-index samples of the view rows at S = res_scale * aa per logical pixel */
#define ENH_VIEW_Y0 18              /* the original's view clip: rows 18 .. 101 */
#define ENH_VIEW_Y1 102
#define ENH_SPLIT_Y 0x63            /* race split (race_drive.c): rows >= 99 show the dashboard page */

static inline float enh_front_edge(bool left)
{
#ifdef STREET_ROD_GE
    if (sr_ge_recording) return sr_ge_front_edge(left);
#endif
    return left ? -1.0f : 321.0f;
}
bool ras_begin(int S);
int ras_S(void);
u8 *ras_row(int j);                 /* sample row j (0 = top of logical row ENH_VIEW_Y0), 320 * S samples */
void ras_poly(const float *xy, int n, u8 colour);                  /* convex polygon, logical coordinates */
void ras_rect(float x0, float y0, float x1, float y1, u8 colour);
void ras_clip(float x0, float y0, float x1, float y1);         /* fills cut to [x0, x1) x [y0, y1) (ras_begin: view) */
void ras_alpha(float a);           /* coverage 0..1 of the following fills (screen door; ras_begin: 1) */
void ras_haze(int level);           /* haze level 0..15 of the following fills (ras_begin: 0; S2.9) */
void ras_haze_target(float y, float strength);    /* haze colour: the most frequent index of logical row y */
void ras_resolve(const u8 pal[17], const u8 *cover, u32 *out, int res_scale, int aa);
void ras_image(const u8 *px, int w, int h, float x0, float y0, float x1, float y1, float ymax);
#define ENH_TRANSPARENT 0xFF
void ras_sprite(const u8 *px, int w, int h, float x0, float y0, float x1, float y1, float ymax);   /* ras_image, index 0xFF skipped */
/* ras_sprite scaled by f and rotated (c, s = cos, sin; dest offset X = f (c U + s V), Y = f (-s U + c V), the
 * rotate_about_centre convention) about the picture point (pu, pv), which lands at (x, y) */
void ras_sprite_rot(const u8 *px, int w, int h, float pu, float pv, float x, float y, float f, float c, float s,
                    float ymax);
void ras_downscale(u8 *idx);      /* 320 x (ENH_VIEW_Y1 - ENH_VIEW_Y0) indices, most frequent per pixel */
u32 enh_pal_rgb(u8 attribute_reg);

/* ---- road and sky (enh_road.c, §3.2, §3.5) */
void enh_sky_capture(void);         /* page 1 rows 0x66.. (sky, panorama), at the first frame of a race */
bool enh_road_render(const enh_view *v);    /* false: not drawn in this mode (the original's picture stays) */
void enh_road_debug_rows(const enh_view *v, FILE *f);    /* compare aid (SR_ENH_COMPARE=3) */

/* ---- road-side objects (enh_sprite.c, §3.6): front_objects of one boundary, or the mirror's banner and
 * mirror_objects of one row, at the view */
typedef struct {
    int s, k;                   /* flags segment; the original's row (front: s - floorf(P / 64); mirror: row 1..7) */
    bool mirror;                /* road_draw_mirror's objects */
    int depth, depth_banner;    /* the original's depth argument (LOD / sign size 9 - depth): front k, k; mirror
                                 * (row >> 1) + 7, row + 5 */
    float z;                   /* depth for the perspective size in front units (front z; mirror 4 x the mirror's z) */
    float fade;                /* coverage 0..1 (the front's far end: objects fade in) */
    /* the original's row values at the boundary (its integers when comparing, else continuous estimates of them) */
    float xl, xr, xol, xor_, xfl, xfr, y, ye, y16;    /* y16: 4 * ye - 3 * y (banner top) */
    float dash;                /* dash width of the row (banner posts) */
    bool flat_l, flat_r;        /* Yq == YT1q, Yq == YT2q (no bank at the row in front) */
    bool gclip;                 /* ground only from gxl to gxr (Mulholland's far mounds): objects outside it are left out */
    float gxl, gxr;
} enh_bound;
enum { ENH_OBJ_ALL, ENH_OBJ_OUT, ENH_OBJ_IN };      /* all; behind the building line; between road and building line */
void enh_objects(const enh_bound *b, int mode, bool compare, int pass);
/* a w x h rectangle at (sx, sy) of picture / page descriptor d as palette indices (whole bytes; valid until the next
 * call); false when out of the picture */
bool enh_sheet(FarPtr d, int sx, int sy, int w, int h, const u8 **px, int *wout);

/* ---- props (enh_props.c, §3.6): the opponent / police car from its native LIB3 picture, the drag strip's start tree and
 * the starter figure in the mirror, at continuous scale. Positions are the original's pixel anchors (continuous). */
/* the car of snapshot b's view (0 front, 1 mirror) at anchor (sx, sy) and projection depth z (front units for the front,
 * the mirror's own z for the mirror) */
extern unsigned enh_prop_n[4];
void enh_prop_car(const enh_snap *b, int view, float sx, float sy, float z);
void enh_prop_tree(float x, float ybase, float z);       /* z: the tree's boundary depth in front units */
void enh_prop_starter(float x, float zm);                 /* zm: the mirror depth of its boundary */
/* the Aqueduct (S2.7): the car's native unrotated picture of its level at the rotated anchor (sx, sy), rotated by
 * `bank` degrees about its bottom centre; the starter in the mirror, feet at (x, ybase), rotated by `roll` */
void enh_prop_car_aq(const enh_snap *b, int view, float sx, float sy, float z, float bank);
void enh_prop_starter_aq(float x, float ybase, float zm, float roll);

/* ---- Aqueduct (enh_aq.c, §3.3, S2.7): road mode 8, front view and mirror */
bool enh_aq_render(const enh_view *v);
void enh_aq_debug_rows(FILE *f);       /* compare aid (SR_ENH_COMPARE=3) */

/* ---- compare (enh_compare.c, §5): SR_ENH_COMPARE >= 2 */
/* object records: kind 0 shape, 1 hydrant, 2 sign picture, 3 banner; orig = recorded from the original's calls */
void enh_compare_obj(bool orig, int kind, int type, int depth, int side, float x, float y, float x2);
void enh_compare_size(float err, float tol);
void enh_compare_objs_clear(void);      /* a new frame: the original's records start again */
void enh_compare_init(int level);
void enh_compare_frame(const enh_snap *s, int page);
void enh_compare_race_end(void);
bool enh_mirror_px(int x, int y);   /* (x, y) belongs to the mirror (its backdrop, frame and glass) */

#endif
