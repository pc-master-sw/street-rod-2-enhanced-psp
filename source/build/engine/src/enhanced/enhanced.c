#ifdef STREET_ROD_GE
#include "engine_road_ge.h"
#endif
/* Street Rod 2 Enhanced: options, hooks, compose, written mask and layers (ENHANCED.md §2, §2.1, §3.9), snapshots
 * and interpolation (§3.1).
 * S2.1: the host and coverage foundation. The output frame is the EGA picture scaled nearest-neighbour; the
 * enhanced view (later subtasks) is drawn by enh_compose where coverage allows.
 * S2.2: a snapshot per road_frame and the interpolated view at the display rate (SR_ENH_TRACE).
 * S2.3: the enhanced view of the town and the street drag race (enh_road.c, enh_raster.c) laid over the scaled EGA
 * picture where coverage allows; the compare tool (enh_compare.c). */
#include "enhanced.h"
#include "enh_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "host.h"
#include "game/race_int.h"
#include "platform/ega.h"
#include "platform/vga.h"
#include "platform/vga.h"

enh_options enh = {
    .res_scale = 4,
    .aa = 2,
    .motion_delay = 110,
    .modes = ENH_MODE_ALL,
    .draw_scale = 60.0 / 14.0,
    .haze = 30,
    .aspect_w = 4,
    .aspect_h = 3,
    .gear_keys = true,
    .key_shift_up = 0x1E,
    .key_shift_down = 0x2C,
    .pad_extras = true,
};

static FILE *trace;         /* SR_ENH_TRACE=file ("-" = stderr): snapshots and interpolated frames (§5) */
static struct { uint64_t n, sum, max; } rstat;  /* enhanced render time, per race (SR_ENH_TRACE) */
static int compare;         /* SR_ENH_COMPARE: 1 = deterministic clock, >= 2 also the compare tool (§5) */

void enh_init(void)
{
    if (enh.classic) return;
    while (enh.res_scale * enh.aa > 16 && enh.aa > 1) enh.aa--;       /* res_scale * aa <= 16 (§3.9) */
    const char *t = getenv("SR_ENH_TRACE");
    if (t && *t) {
        trace = strcmp(t, "-") ? fopen(t, "w") : stderr;
        if (trace)
            fprintf(trace, "# S frame t_ms ticks mode seg sub step cam_x heading opp_seg opp_frac opp_lat "
                           "front_s/v/drawn mirror_s/v/drawn page wall_hit\n"
                           "# C t_ms render_ms frame_a frame_b x w snap P rel_heading cam_x O opp_lat jerk us\n");
    }
    const char *c = getenv("SR_ENH_COMPARE");
    compare = c && *c ? atoi(c) : 0;
    if (c && *c && !compare && strcmp(c, "0")) compare = 1;            /* as host.c: any other value = the clock */
    if (compare >= 2) enh_compare_init(compare);
}

bool enh_hires(void) { return !enh.classic; }

/* ---------------------------------------------------------------- race state, layers */

#define PAGE_BYTES  (EGA_BYTES_PER_LINE * 200)      /* 8000: one bit per pixel, as a plane */
#define PAGE1_OFF   0x2000u                         /* A200:0000 */
#define PAGE2_OFF   0x4000u                         /* A400:0000 */

static struct {
    unsigned slow_frames;
    bool overlay;           /* a race of an enabled mode is running */
    bool bg;                /* inside a race frame, background layer */
    bool fg_opaque;         /* foreground copy whose every touched pixel is opaque (enh_fg_opaque) */
    bool draws_view;        /* the enhanced view is drawn over the EGA picture (S2.3: road modes 0 and 1; S2.4: 2, 4, 0x10) */
    bool draws_objects;     /* the enhanced view draws the road-side objects (S2.5: road modes 0 and 1) */
    bool in_objects;        /* inside front_objects (enh_objects_begin .. end) or the mirror's objects */
    bool in_mirror;         /* inside the mirror's objects (enh_mirror_objects_begin .. end) */
    int shown_page;         /* page (1, 2) presented by road_page_flip with a frame, 0 = none */
    /* After the race (enh_race_end) the last rendered view (the raster's samples: nothing is rendered again, the
     * race's data is gone) stays over the race picture while it is displayed: race_run copies the shown race page to
     * page 0 and the back page and shows page 0, then the finish's message boxes and the pause before the results
     * follow. The hold follows the displayed page (hold_off): when it changes, the new page's pixels that still equal
     * the race picture (hold_px) and were the enhanced view's at the race end (hold_fg0) show the hold, and later
     * writes to that page take their pixels over (hold_mask, bit set = the original's pixel shows). It ends when no
     * view pixel is left or the display leaves the three pages. */
    bool hold, rendered;
    u16 hold_off;
    unsigned hold_n0;       /* view pixels of the hold at the race end */
    u8 hold_mask[PAGE_BYTES], hold_fg0[PAGE_BYTES];
    u8 hold_px[ENH_SPLIT_Y - ENH_VIEW_Y0][320];
    u8 written[2][PAGE_BYTES];  /* per race page: bit set = the original's pixel shows (§2.1) */
} rs;

/* ---------------------------------------------------------------- snapshots (§3.1) */

#define RING 8
static struct {
    enh_snap s[RING];
    unsigned n;             /* snapshots taken since race start; the newest is s[(n - 1) % RING] */
    unsigned race;          /* races begun (the trace restarts its jerk estimate) */
} ring;

static enh_snap *snap_at(unsigned age) { return &ring.s[(ring.n - 1 - age) % RING]; }    /* age 0 = newest */

static void snap_take(void)
{
    enh_snap *s = &ring.s[ring.n % RING];
    memset(s, 0, sizeof *s);
    s->t_ns = host_tick_ns();
    s->frame = ring.n;
    s->ticks = DSL(DS_ticks);
    s->mode = (s16)DSW(DS_road_mode);
    s->racing = DSB(DS_racing);
    s->start_seg = DSS(DS_start_seg);
    s->track_end = DSS(DS_track_end);
    s->fallen = DSS(DS_fallen);
    s->fall_dir = DSS(DS_fall_dir);
    s->seg = DSS(DS_seg);
    s->substep = DSS(DS_substep);
    s->step = DSS(DS_step_per_frame);
    s->cam_x = DSS(DS_cam_x);
    s->cam_y = DSS(DS_cam_y);
    s->heading = DSS(DS_heading);
    s->cur_height = DSS(DS_cur_height);
    s->bounce_y = DSS(DS_view_bounce_y);
    s->wheel = PHS(PLAYER, PH_WHEEL);
    s->opp_seg = DSS(DS_opp_seg);
    s->opp_frac = DSS(DS_opp_seg_frac64);
    s->opp_lat = DSS(DS_opp_lat);
    s->opp_lat_step = DSS(DS_lat_last_opp_step);
    s->opp_model = DSS(DS_opp_model);
    s->opp_sprite = DSS(DS_opp_sprite);
    s->opp_variant = DSS(DS_opp_variant);
    s->opp_s[0] = s->opp_s[1] = s->opp_v[0] = s->opp_v[1] = -1;
    s->police_state = DSS(DS_police_state);
    s->police_seg = DSS(DS_police_seg);
    s->start_light_pos = DSS(DS_start_light_pos);
    s->start_lights = DSS(DS_start_lights);
    s->light_latch = DSS(DS_light_latch);
    s->starter_seg = DSS(DS_starter_seg);
    s->wall_hit_shift = DSS(DS_wall_hit_shift);
    s->top_xl = DSS(DS_aq_def_top_xl);                      /* aq_project_front's deformation (the others are saved) */
    s->top_xr = DSS(DS_aq_def_top_xr);
    for (int i = 0; i < 17; i++) s->pal[i] = ega_get_palette_reg(i);
    ring.n++;
}

/* The values the frame's projection computed (Aqueduct roll and channel, horizon band row). */
static void snap_projected(void)
{
    if (!ring.n) return;
    enh_snap *s = snap_at(0);
    s->roll = DSS(DS_rot_enable);
    s->rot_cos = DSS(DS_rot_cos);
    s->rot_sin = DSS(DS_rot_sin);
    s->rot_cx = DSS(DS_rot_cx);
    s->rot_cy = DSS(DS_rot_cy);
    s->aq_pitch = DSS(DS_aq_pitch);
    s->aq_dy = DSS(DS_aq_dy);
    s->floor_h = DSS(DS_aq_saved_floor_h);
    s->top_h = DSS(DS_aq_saved_top_h);
    s->xl = DSS(DS_aq_saved_xl);
    s->xr = DSS(DS_aq_saved_xr);
    s->wall_l_shift = DSS(DS_aq_wall_l_shift);
    s->wall_r_shift = DSS(DS_aq_wall_r_shift);
    s->opp_bank = DSS(DS_opp_bank);
    s->top_band_row = DSS(DS_top_band_row);
}

static void trace_snap(const enh_snap *s)
{
    fprintf(trace, "S %u %.3f %u %d %d %d %d %d %d %d %d %d %d/%d/%d %d/%d/%d %d %d\n", s->frame, s->t_ns / 1e6,
            s->ticks, s->mode, s->seg, s->substep, s->step, s->cam_x, s->heading, s->opp_seg, s->opp_frac, s->opp_lat,
            s->opp_s[0], s->opp_v[0], s->opp_drawn[0], s->opp_s[1], s->opp_v[1], s->opp_drawn[1], s->page,
            s->wall_hit_shift);
}

/* ---------------------------------------------------------------- track reads (clamped, §1) */

int enh_toff;               /* index offset of YAW / HGT / BL / BR / LANE: 0xC0 on the drag strip (road_set_seg_ptrs) */

static s16 trk(u16 arr, int i) { i += enh_toff; return TRKS(arr, i < 0 ? 0 : i >= TRK_N ? TRK_N - 1 : i); }

static float trk_at(u16 arr, float P)
{
    float f = floorf(P / 64.0);
    int i = (int)f;
    s16 v0 = trk(arr, i);
    return v0 + (P / 64.0 - f) * (s16)(trk(arr, i + 1) - v0);      /* wrap-safe step */
}

float enh_far_fade(float far_z, float z)
{
    float w = fmaxf(8.0, 0.1 * (far_z - 4.0));
    return fminf(fmaxf((far_z - z) / w, 0.0), 1.0);
}

bool enh_far_opp(const enh_snap *b, float O, float P)
{
    bool m = b->mode == 1 || b->mode == 4 || b->mode == 8 || b->mode == 0x10;
    return m && b->racing && b->opp_s[0] == -1 && b->police_state < 2 && O - P > 13.0 * 64.0;
}

int enh_mirror_rows(bool compare)
{
    if (compare) return 7;
    int r = (int)floorf(7.0 * enh.draw_scale + 1e-9);
    return r < 7 ? 7 : r > 30 ? 30 : r;
}

bool enh_far_opp_mirror(const enh_snap *b, float O, float P)
{
    bool m = b->mode == 1 || b->mode == 4 || b->mode == 8 || b->mode == 0x10 || b->police_state >= 2;
    return m && b->racing && b->opp_s[0] == -1 && b->opp_s[1] == -1 && P - O > 7.0 * 64.0
        && P - O < enh_mirror_rows(false) * 64.0;
}

int enh_haze_level(float far_z, float z)
{
    float z0 = 4.0 + 8.0 * 20.0;
    if (enh.haze <= 0 || far_z <= z0 + 8.0) return 0;
    float t = fminf(fmaxf((z - z0) / (far_z - z0), 0.0), 1.0);
    return (int)lroundf(15.0 * t * t * (3.0 - 2.0 * t));
}

float enh_track_yaw(float P) { return trk_at(TRK_YAW, P); }
float enh_track_hgt(float P) { return trk_at(TRK_HGT, P); }

/* ---------------------------------------------------------------- interpolation (§3.1) */

#define SNAP_SEGS     2           /* a jump of >= 2 segments */
#define SNAP_HEADING  16
#define SNAP_CAM_X    0x100       /* edge bounce */

static float pos(s16 seg, s16 sub) { return seg * 64.0 + sub; }

/* heading - YAW(P): the heading relative to the road at the car's continuous position */
static float rel_heading(const enh_snap *s)
{
    if (s->mode == 8) return s->heading;                    /* the Aqueduct is straight: the raw heading (S2.7) */
    s16 y0 = trk(TRK_YAW, s->seg);
    return (s16)(s->heading - y0) - (enh_track_yaw(pos(s->seg, s->substep)) - y0);
}

/* A pair that must not be blended: mode change, fall-off and wall-bounce frames, jumps (§3.1). */
static bool discontinuous(const enh_snap *a, const enh_snap *b)
{
    if (a == b) return false;
    return a->mode != b->mode || b->fall_dir || b->fallen != a->fallen || b->wall_hit_shift
        || fabsf(pos(b->seg, b->substep) - pos(a->seg, a->substep)) >= SNAP_SEGS * 64
        || abs((s16)(b->heading - a->heading)) >= SNAP_HEADING || abs((s16)(b->cam_x - a->cam_x)) >= SNAP_CAM_X;
}

enum { V_P, V_REL, V_CAMX, V_CAMY, V_HGT, V_BOUNCE, V_WHEEL, V_O, V_LAT, V_ROLL, V_RCX, V_RCY, V_PITCH, V_AQDY,
       V_FH, V_TH, V_XL, V_XR, V_TXL, V_TXR, V_N };

static void values(const enh_snap *s, float *v)
{
    v[V_P] = pos(s->seg, s->substep);
    v[V_REL] = rel_heading(s);
    v[V_CAMX] = s->cam_x;
    v[V_CAMY] = s->cam_y;
    v[V_HGT] = s->cur_height;
    v[V_BOUNCE] = s->bounce_y;
    v[V_WHEEL] = s->wheel;
    v[V_O] = pos(s->opp_seg, s->opp_frac);
    v[V_LAT] = s->opp_lat;
    v[V_ROLL] = s->roll;
    v[V_RCX] = s->rot_cx;
    v[V_RCY] = s->rot_cy;
    v[V_PITCH] = s->aq_pitch;
    v[V_AQDY] = s->aq_dy;
    v[V_FH] = s->floor_h;
    v[V_TH] = s->top_h;
    v[V_XL] = s->xl;
    v[V_XR] = s->xr;
    v[V_TXL] = s->top_xl;
    v[V_TXR] = s->top_xr;
}

/* Position of time t on the pair (a, b): 0 at a, 1 at b, extrapolated up to `xmax` past a. */
static float pair_x(const enh_snap *a, const enh_snap *b, uint64_t t, float xmax)
{
    if (a == b || b->t_ns <= a->t_ns) return 1.0;
    float x = ((float)t - (float)a->t_ns) / (float)(b->t_ns - a->t_ns);
    return x < 0 ? 0 : x > xmax ? xmax : x;
}

static void pair_at(const enh_snap *a, const enh_snap *b, float x, float *out)
{
    float va[V_N], vb[V_N];
    values(a, va);
    values(b, vb);
    if (vb[V_ROLL] - va[V_ROLL] > 180) vb[V_ROLL] -= 360;  /* the roll wraps at +-360 degrees: the short way */
    else if (vb[V_ROLL] - va[V_ROLL] < -180) vb[V_ROLL] += 360;
    for (int k = 0; k < V_N; k++) out[k] = va[k] + (vb[k] - va[k]) * x;
    /* the opponent jumps on its own (aq_wall_bounce pulls it away 2 segments a frame): no blend then */
    if (fabsf(vb[V_O] - va[V_O]) >= SNAP_SEGS * 64) out[V_O] = vb[V_O];
}

/* Render time t = now - motion delay. The pair (a, b) around t (or the newest pair) is blended linearly; while t is
 * inside it, the line of the previous pair (z, a), extrapolated, fades into it with a smoothstep, so the speed stays
 * continuous when a new snapshot arrives: between snapshots everything moves at the speed of the last pair (TD3E,
 * SRE). A discontinuous pair shows b unblended. */
bool enh_interp(uint64_t now_ns, enh_view *o)
{
    if (!ring.n) return false;
    uint64_t delay = (uint64_t)enh.motion_delay * 1000000u;
    uint64_t t = now_ns > delay ? now_ns - delay : 0;
    unsigned avail = ring.n < RING ? ring.n : RING, i = 0;
    while (i + 1 < avail && snap_at(i + 1)->t_ns > t) i++;  /* b = the oldest snapshot after t, else the newest */
    const enh_snap *b = snap_at(i);
    const enh_snap *a = i + 1 < avail ? snap_at(i + 1) : b;
    const enh_snap *z = i + 2 < avail ? snap_at(i + 2) : a;
    float v[V_N];
    memset(o, 0, sizeof *o);
    o->a = a;
    o->b = b;
    o->render_ns = t;
    o->w = 1.0;
    o->snap = a == b || discontinuous(a, b);
    if (o->snap) {
        o->x = 1.0;
        values(b, v);
    } else {
        o->x = pair_x(a, b, t, 1.5);             /* a late snapshot: half a period past b at most */
        pair_at(a, b, o->x, v);
        if (z != a && !discontinuous(z, a) && o->x < 1.0) {
            float vz[V_N];
            pair_at(z, a, pair_x(z, a, t, 2.5), vz);     /* the previous line through this pair */
            o->w = o->x * o->x * (3 - 2 * o->x);
            for (int k = 0; k < V_N; k++) v[k] = vz[k] + (v[k] - vz[k]) * o->w;
            /* a hard stop: the previous pair's line overshoots b; positions along the road never run backwards */
            float va[V_N], vb[V_N];
            values(a, va);
            values(b, vb);
            for (int k = V_P; k <= V_O; k += V_O - V_P)
                v[k] = fmaxf(fminf(va[k], vb[k]), fminf(fmaxf(va[k], vb[k]), v[k]));
        }
    }
    o->P = v[V_P];
    o->rel_heading = v[V_REL];
    o->heading = b->mode == 8 ? o->rel_heading : enh_track_yaw(o->P) + o->rel_heading;
    o->cam_x = v[V_CAMX];
    o->cam_y = v[V_CAMY];
    o->cur_height = v[V_HGT];
    o->bounce_y = v[V_BOUNCE];
    o->wheel = v[V_WHEEL];
    o->O = v[V_O];
    o->opp_lat = v[V_LAT];
    o->roll = v[V_ROLL];
    o->rot_cx = v[V_RCX];
    o->rot_cy = v[V_RCY];
    o->aq_pitch = v[V_PITCH];
    o->aq_dy = v[V_AQDY];
    o->aq_fh = v[V_FH];
    o->aq_th = v[V_TH];
    o->aq_xl = v[V_XL];
    o->aq_xr = v[V_XR];
    o->aq_txl = v[V_TXL];
    o->aq_txr = v[V_TXR];
    return true;
}

/* One line per interpolated (composed) frame; jerk = change of the camera speed along the road (sub-steps per
 * second) from the previous composed frame, us = time spent in enh_interp. */
static void trace_view(uint64_t now, const enh_view *v, float us)
{
    static uint64_t t1, t2;
    static float p1, p2;
    static unsigned race;
    float jerk = 0;
    if (race != ring.race) { race = ring.race; t1 = t2 = 0; }
    if (t1 && t2 && now > t1 && t1 > t2)
        jerk = (v->P - p1) / ((float)(now - t1) / 1e9) - (p1 - p2) / ((float)(t1 - t2) / 1e9);
    fprintf(trace, "C %.3f %.3f %u %u %.4f %.4f %d %.3f %.3f %.1f %.3f %.1f %.1f %.1f\n", now / 1e6,
            v->render_ns / 1e6, v->a->frame, v->b->frame, v->x, v->w, v->snap, v->P, v->rel_heading, v->cam_x, v->O,
            v->opp_lat, jerk, us);
    t2 = t1; p2 = p1;
    t1 = now; p1 = v->P;
}

static enh_view view;               /* the interpolated view of the last compose (drawn from S2.3 on) */

static unsigned mode_bit(int road_mode)
{
    switch (road_mode) {
    case 0: return ENH_MODE_TOWN;
    case 1: return ENH_MODE_DRAG;
    case 2: case 4: return ENH_MODE_STRIP;
    case 8: return ENH_MODE_AQUEDUCT;
    case 0x10: return ENH_MODE_MULHOLLAND;
    default: return 0;
    }
}

void enh_race_begin(int road_mode)
{
    if (enh.classic) return;
    rs.slow_frames = 0;
    rs.overlay = (enh.modes & mode_bit(road_mode)) != 0;
    rs.hold = rs.rendered = false;
    rs.draws_view = rs.overlay;     /* S2.3: town, street drag race; S2.4: strip, Mulholland; S2.7: the Aqueduct */
    enh_toff = (road_mode & 6) ? 0xC0 : 0;
    rs.draws_objects = rs.draws_view && road_mode != 8;     /* S2.5 (the Aqueduct has no front_objects) */
#ifdef STREET_ROD_GE
    if (sr_ge_enabled) rs.draws_objects = false;
#endif
    rs.in_objects = rs.in_mirror = false;
    rs.bg = false;
    rs.shown_page = 0;
    memset(rs.written, 0xFF, sizeof rs.written);
    enh.track = rs.overlay;
    ring.n = 0;
    ring.race++;
    if (trace && rs.overlay) fprintf(trace, "# race mode %d\n", road_mode);
}

static u8 page_px(u16 base, int x, int y)
{
    u16 o = (u16)(base + y * EGA_BYTES_PER_LINE + x / 8);
    int b = 7 - x % 8;
    return (u8)((ega_plane(0)[o] >> b & 1) | (ega_plane(1)[o] >> b & 1) << 1 | (ega_plane(2)[o] >> b & 1) << 2
              | (ega_plane(3)[o] >> b & 1) << 3);
}

static void hold_start(int page)
{
    rs.hold_off = page == 1 ? PAGE1_OFF : PAGE2_OFF;
    memcpy(rs.hold_fg0, rs.written[page - 1], PAGE_BYTES);
    memcpy(rs.hold_mask, rs.hold_fg0, PAGE_BYTES);
    for (int y = ENH_VIEW_Y0; y < ENH_SPLIT_Y; y++)
        for (int x = 0; x < 320; x++) rs.hold_px[y - ENH_VIEW_Y0][x] = page_px(rs.hold_off, x, y);
    rs.hold_n0 = 0;
    for (int y = ENH_VIEW_Y0; y < ENH_SPLIT_Y; y++)
        for (int x = 0; x < 320; x++) rs.hold_n0 += !(rs.hold_fg0[y * EGA_BYTES_PER_LINE + x / 8] >> (7 - x % 8) & 1);
    if (trace) fprintf(trace, "# hold page %d t_ms %llu view pixels %u\n", page,
                       (unsigned long long)(host_clock_ns() / 1000000), rs.hold_n0);
}

/* the displayed page changed during the hold: its mask from its pixels; false when the hold ends */
static bool hold_follow(u16 start, uint64_t now)
{
    unsigned left = 0;
    if (start == 0 || start == PAGE1_OFF || start == PAGE2_OFF) {
        rs.hold_off = start;
        memset(rs.hold_mask, 0xFF, PAGE_BYTES);
        for (int y = ENH_VIEW_Y0; y < ENH_SPLIT_Y; y++)
            for (int x = 0; x < 320; x++) {
                int i = y * EGA_BYTES_PER_LINE + x / 8, bit = 7 - x % 8;
                if (!(rs.hold_fg0[i] >> bit & 1) && page_px(start, x, y) == rs.hold_px[y - ENH_VIEW_Y0][x]) {
                    rs.hold_mask[i] &= (u8)~(1 << bit);
                    left++;
                }
            }
    }
    if (trace) fprintf(trace, "# hold start %04x t_ms %llu view pixels %u\n", start, (unsigned long long)(now / 1000000), left);
    return left * 2 > rs.hold_n0;
}

void enh_race_end(void)
{
    if (rs.overlay) enh_compare_race_end();
    if (trace && rstat.n)
        fprintf(trace, "# render %llu frames, mean %.3f ms, max %.3f ms (%dx%d, aa %d)\n", (unsigned long long)rstat.n,
                rstat.sum / 1e6 / rstat.n, rstat.max / 1e6, 320 * enh.res_scale, 200 * enh.res_scale, enh.aa);
    if (trace) fprintf(trace, "# props drawn: front car %u, mirror car %u, tree %u, starter %u\n", enh_prop_n[0], enh_prop_n[1], enh_prop_n[2], enh_prop_n[3]);
    memset(enh_prop_n, 0, sizeof enh_prop_n);
    memset(&rstat, 0, sizeof rstat);
    if (trace) fflush(trace);
    rs.hold = rs.overlay && rs.draws_view && rs.rendered && rs.shown_page;
#ifdef STREET_ROD_GE
    if (sr_ge_enabled) rs.hold = false;
#endif
    if (rs.hold) hold_start(rs.shown_page);
    rs.overlay = false;
    rs.draws_view = false;
    rs.draws_objects = false;
    rs.in_objects = false;
    rs.bg = false;                                          /* the hold: every later write is foreground */
    rs.shown_page = 0;
    enh.track = rs.hold;
}

void enh_frame_begin(void)
{
    if (!rs.overlay) return;
    rs.bg = true;
    if (trace && ring.n) trace_snap(snap_at(0));           /* the previous frame, now complete */
    if (!ring.n && rs.draws_view && DSW(DS_road_mode) != 8) enh_sky_capture();        /* road_load_graphics has stored the sky in page 1 */
    if (compare >= 2) enh_compare_objs_clear();
    snap_take();
}

void enh_layer_fg(void)
{
    if (rs.bg) snap_projected();
    rs.bg = false;
}

void enh_opp_drawn(int view_id, int s, int v, bool drawn)
{
    if (!rs.overlay || !ring.n || view_id < 0 || view_id > 1) return;
    enh_snap *n = snap_at(0);
    n->opp_s[view_id] = (s8)s;
    n->opp_v[view_id] = (s8)v;
    n->opp_drawn[view_id] = drawn;
}
void enh_layer_bg(void) { if (rs.overlay) rs.bg = true; }
void enh_fg_opaque(bool on) { rs.fg_opaque = on && rs.overlay; }

void enh_opp_place(int view_id, int rx, int ry, bool special)
{
    if (!rs.overlay || !ring.n || view_id < 0 || view_id > 1) return;
    enh_snap *n = snap_at(0);
    n->opp_rx[view_id] = (s16)rx;
    n->opp_ry[view_id] = (s16)ry;
    n->opp_special[view_id] = special;
}

/* The car, the start tree and the starter (S2.5): drawn by the enhanced view where it draws the view (not in the
 * compare view, which keeps the original's pixels), else a passthrough bracket. */
static bool prop_native(bool mirror)
{
    (void)mirror;
#ifdef STREET_ROD_GE
    if (sr_ge_enabled) return false;
#endif
    return rs.draws_view && compare < 2;
}

void enh_prop_fg(bool mirror)
{
    if (rs.overlay && !prop_native(mirror)) enh_layer_fg();
}

void enh_prop_bg(bool mirror)
{
    if (rs.overlay && !prop_native(mirror)) enh_layer_bg();
}

/* front_objects: drawn by the enhanced renderer (the original's pixels are background, the enhanced view owns them),
 * or a passthrough bracket in the modes whose objects are not enhanced yet. */
void enh_objects_begin(void)
{
    if (!rs.overlay) return;
    if (rs.draws_objects) rs.in_objects = true;
    else enh_layer_fg();
}

void enh_objects_end(void)
{
    if (!rs.overlay) return;
    if (rs.draws_objects) rs.in_objects = false;
    else enh_layer_bg();
}

void enh_mirror_objects_begin(void)
{
    if (!rs.overlay) return;
    rs.in_mirror = true;
    enh_objects_begin();
}

void enh_mirror_objects_end(void)
{
    if (!rs.overlay) return;
    enh_objects_end();
    rs.in_mirror = false;
}

void enh_obj_orig(int kind, int type, int x, int y, int depth, int side, int x2)
{
    if (compare >= 2 && rs.in_objects) enh_compare_obj(true, kind + (rs.in_mirror ? 4 : 0), type, depth, side, x, y, x2);
}

void enh_frame_presented(int page)
{
    if (!rs.overlay) return;
    rs.shown_page = (page == 1 || page == 2) ? page : 0;
    if (ring.n) snap_at(0)->page = (s8)rs.shown_page;
    if (compare >= 2 && rs.draws_view && ring.n) enh_compare_frame(snap_at(0), rs.shown_page);
}

/* ---------------------------------------------------------------- written mask (§2.1) */

void enh_vram_mask_write(u16 off, u8 changed, u8 opaque)
{
    u8 *mask;
    if (rs.hold) {                                          /* after the race: writes to the displayed page */
        if (off < rs.hold_off || off >= rs.hold_off + PAGE_BYTES) return;
        rs.hold_mask[off - rs.hold_off] |= changed;
        return;
    }
    if (off >= PAGE1_OFF && off < PAGE1_OFF + PAGE_BYTES) mask = &rs.written[0][off - PAGE1_OFF];
    else if (off >= PAGE2_OFF && off < PAGE2_OFF + PAGE_BYTES) mask = &rs.written[1][off - PAGE2_OFF];
    else return;
    if (rs.bg) {
        /* background: the enhanced view owns every pixel the write touched */
        u8 touched = (vga.map_mask & 0x0F) ? ((vga.mode & 3) == 1 ? 0xFF : vga.bit_mask) : 0;
        *mask &= (u8)~(touched | changed);
    } else {
        u8 touched = rs.fg_opaque && (vga.map_mask & 0x0F) ? ((vga.mode & 3) == 1 ? 0xFF : vga.bit_mask) : 0;
        *mask |= changed | touched | opaque;                             /* foreground: last writer wins per pixel */
    }
}

void enh_vram_write(u16 off, u8 changed)
{
    enh_vram_mask_write(off, changed, 0);
}

bool enh_vram_fill(u16 first, unsigned count)
{
    if (rs.hold || (!rs.bg && !rs.fg_opaque)) return false;
    const unsigned base[] = { PAGE1_OFF, PAGE2_OFF };
    unsigned end = first + count;
    for (unsigned page = 0; page < 2; page++) {
        unsigned left = first > base[page] ? first : base[page];
        unsigned right = end < base[page] + PAGE_BYTES ? end : base[page] + PAGE_BYTES;
        if (left < right) memset(rs.written[page] + (left - base[page]), rs.bg ? 0 : 0xff, right - left);
    }
    return true;
}

void enh_vram_opaque16(u16 seg, u16 off, u16 bits)
{
    if (!is_vram(seg) || rs.bg || rs.hold) return;
    for (int i = 0; i < 2; i++) {
        u16 o = (u16)(vram_off(seg, (u16)(off + i)));
        u8 b = (u8)(bits >> (8 * i));
        if (o >= PAGE1_OFF && o < PAGE1_OFF + PAGE_BYTES) rs.written[0][o - PAGE1_OFF] |= b;
        else if (o >= PAGE2_OFF && o < PAGE2_OFF + PAGE_BYTES) rs.written[1][o - PAGE2_OFF] |= b;
    }
}

bool enh_written(int page, int x, int y)
{
    if (page < 1 || page > 2 || x < 0 || x >= 320 || y < 0 || y >= 200) return true;
    return rs.written[page - 1][y * EGA_BYTES_PER_LINE + x / 8] >> (7 - x % 8) & 1;
}

/* ---------------------------------------------------------------- compose */

static u32 pic[320 * 200];
static u8 cover[(ENH_SPLIT_Y - ENH_VIEW_Y0) * 320];

/* The logical pixels of the view that show the enhanced picture (§2.1): above the split, not last written by the
 * foreground on the shown page (the mirror included: enh_road_render draws it, S2.6). */
static void cover_build(int page)
{
    for (int y = ENH_VIEW_Y0; y < ENH_SPLIT_Y; y++) {
        u8 *row = cover + (y - ENH_VIEW_Y0) * 320;
        if (page) {
            const u8 *written = rs.written[page - 1] + y * EGA_BYTES_PER_LINE;
            for (int byte = 0; byte < EGA_BYTES_PER_LINE; byte++) {
                unsigned mask = written[byte];
                for (int bit = 0; bit < 8; bit++)
                    row[byte * 8 + bit] = 1u ^ (mask >> (7 - bit) & 1u);
            }
        } else {
            for (int x = 0; x < 320; x++)
                row[x] = !(rs.hold_mask[y * EGA_BYTES_PER_LINE + x / 8] >> (7 - x % 8) & 1)
                    && page_px(rs.hold_off, x, y) == rs.hold_px[y - ENH_VIEW_Y0][x];
        }
        /* Keep the mirror pixels that the original pass already drew. */
        if (enh_mirror_px(319, y)) memset(row + 0xF0, 0, 320 - 0xF0);
        else if (enh_mirror_px(0x110, y)) memset(row + 0x110, 0, 0x10);
    }
}

bool enh_compose(bool (*ega_compose)(u32 *xrgb), u32 *out)
{
    bool changed = ega_compose(pic);
    /* The view at the display rate. Without a drawn view the host calls compose on every pump (nothing presented):
     * evaluate at most every 8 ms, the host's present gate, so the trace shows the display cadence. */
    static uint64_t last_eval;
    uint64_t now = host_clock_ns();
    if (rs.hold && ega_get_start() != rs.hold_off && !hold_follow(ega_get_start(), now)) {
        rs.hold = false;                                    /* the race picture is gone */
        enh.track = false;
        if (trace) fprintf(trace, "# hold end t_ms %llu\n", (unsigned long long)(now / 1000000));
    }
    bool live = (rs.overlay && rs.draws_view && ring.n && rs.shown_page) || rs.hold;
    if (rs.overlay && ring.n && (rs.draws_view || now - last_eval >= 8000000u)) {
        uint64_t c0 = host_clock_ns();
        last_eval = now;
        enh_interp(now, &view);
        if (trace) trace_view(now, &view, (float)(host_clock_ns() - c0) / 1e3);
    }
    /* While the enhanced view is drawn it changes on every call: the host then presents at the display rate (§2).
     * With the compare clock it presents only when the EGA picture changes, so snapshot runs of --classic and of the
     * Enhanced graphics present at the same moments (tools/classic_check.py --clock). */
    if (!changed && !(live && !rs.hold && !compare)) return false;    /* the hold's view is frozen */
    int n = enh.res_scale, w = 320 * n;
    if (n == 1) memcpy(out, pic, sizeof pic);
    else for (int y = 0; y < 200; y++) {
        u32 *row = out + (size_t)y * n * w;
        const u32 *src = pic + y * 320;
        for (int x = 0; x < 320; x++)
            for (int k = 0; k < n; k++) *row++ = src[x];
        row -= w;
        for (int k = 1; k < n; k++) memcpy(row + (size_t)k * w, row, (size_t)w * sizeof *row);
    }
    if (live) {
        uint64_t c0 = host_clock_ns();
        /* The live palette, as the cockpit around the view: a race's first frame is drawn while the palette is still
         * black (drv_pal_normal follows road_frame), and the drag strip's "Drivers stage your vehicles!" box waits on
         * that frame; the hold follows the fades. */
        u8 pal[17];
        for (int i = 0; i < 17; i++) pal[i] = ega_get_palette_reg(i);
#ifdef STREET_ROD_GE
        if (sr_ge_enabled && !compare && !rs.hold && n == 1 && enh.aa == 1) {
            uint32_t colors[16];
            for (int color = 0; color < 16; color++)
                colors[color] = (enh_pal_rgb(pal[color]) << 8) | 255u;
            bool wide = sr_ge_widescreen && view.b &&
                (view.b->mode == 0 || view.b->mode == 1 || view.b->mode == 8);
            sr_ge_begin(colors, ENH_VIEW_Y0, ENH_VIEW_Y1 - ENH_VIEW_Y0, wide);
        }
#endif
        if (rs.hold) {                                      /* the last view */
            cover_build(0);
            /* another picture drawn over the race's (more than half of it): the hold ends, so that its pixels that
             * happen to have the race picture's colour do not show the old view */
            unsigned left = 0;
            for (size_t i = 0; i < sizeof cover; i++) left += cover[i];
            if (left * 2 <= rs.hold_n0) {
                rs.hold = false;
                enh.track = false;
                if (trace) fprintf(trace, "# hold end t_ms %llu view pixels %u\n", (unsigned long long)(now / 1000000), left);
            } else ras_resolve(pal, cover, out, n, enh.aa);
        } else if (enh_road_render(&view)) {
            rs.rendered = true;
            cover_build(rs.shown_page);
#ifdef STREET_ROD_GE
            if (sr_ge_recording) sr_ge_commit(cover, ENH_SPLIT_Y - ENH_VIEW_Y0);
            else
#endif
                ras_resolve(pal, cover, out, n, enh.aa);
            uint64_t dt = host_clock_ns() - c0;                /* render time per displayed frame (§3.10) */
            rstat.n++;
            rstat.sum += dt;
            rs.slow_frames = dt > 70000000 ? rs.slow_frames + 1 : 0;
            if (rs.slow_frames >= 4) {
                rs.overlay = rs.draws_view = rs.draws_objects = rs.rendered = false;
                enh.track = false;
                if (n == 1) memcpy(out, pic, sizeof pic);
#ifdef STREET_ROD_GE
                sr_ge_reset();
#endif
            }
            if (dt > rstat.max) rstat.max = dt;
            if (trace && rstat.n % 512 == 0)                    /* running figure: a race that is not left (S2.8) */
                fprintf(trace, "# render so far %llu frames, mean %.3f ms, max %.3f ms\n", (unsigned long long)rstat.n,
                        rstat.sum / 1e6 / rstat.n, rstat.max / 1e6);
        }
    }
    return true;
}
