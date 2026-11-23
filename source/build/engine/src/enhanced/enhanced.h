/* Street Rod 2 Enhanced: options on top of the faithful port (see ENHANCED.md).
 * Every edit to engine code outside src/enhanced/ is marked with an ENH: comment.
 * --classic switches every enhancement off: behaviour and picture as the faithful release. */
#ifndef ENHANCED_H
#define ENHANCED_H

#include <stdbool.h>

#include "types.h"

/* Race road modes (DS_road_mode) as bits of enh_options.modes (--enh-modes). */
#define ENH_MODE_TOWN       0x01    /* mode 0: town cruise */
#define ENH_MODE_DRAG       0x02    /* mode 1: street drag race */
#define ENH_MODE_STRIP      0x04    /* modes 2, 4: drag strip */
#define ENH_MODE_MULHOLLAND 0x08    /* mode 0x10 */
#define ENH_MODE_AQUEDUCT   0x10    /* mode 8 */
#define ENH_MODE_ALL        0x1F

typedef struct {
    bool classic;           /* --classic: all enhancements off */
    int res_scale;          /* --res-scale 1..8: output frame 320*N x 200*N (default 4) */
    int aa;                 /* --aa 1..4: samples per axis, res_scale * aa <= 16 (default 2) */
    int motion_delay;       /* --motion-delay MS: interpolation delay, 0 = latest snapshot (default 110) */
    unsigned modes;         /* --enh-modes LIST: ENH_MODE_* drawn enhanced (default all) */
    float draw_scale;      /* the enhanced view's draw distance as a multiple of the original's 14 segments:
                             * --draw-distance N (14..120 segments, default 60) or --draw-distance-scale X (N = 14 X);
                             * 1 = the original's; picture only (S2.9) */
    int haze;               /* --haze 0..100: far road blended towards the sky colour, percent at the far end (30) */
    int aspect_w, aspect_h; /* --aspect W:H (4:3 default, 16:9, 16:10 ...) or "window" (aspect_w = 0: fill the window):
                             * shape of the picture area; presentation only, the game's 320x200 coordinates stay (S2.10) */
    bool integer_scale;     /* --integer-scale: the picture is a whole multiple of its logical size (black bars), off by default */
    bool gear_keys;         /* --no-gear-keys turns the A / Z shift keys off (default on; the original release has them) */
    int key_shift_up, key_shift_down;   /* XT make codes of the shift keys: --key-shift-up / --key-shift-down NAME (A = 0x1E, Z = 0x2C) */
    bool pad_extras;        /* --no-pad-extras: shoulder buttons shift, triggers are gas / brake, Start = Esc (default on) */
    bool track;            /* engine hook gate: the written mask is kept (a race with the overlay on) */
} enh_options;

extern enh_options enh;

/* Parses one command-line argument `arg` (its value, if any, is `val`, may be NULL). Returns the number of
 * arguments used (1 or 2), 0 when it is not an Enhanced option or its value is missing / invalid. */
int enh_parse_arg(const char *arg, const char *val);
/* After host_init: validates the options. */
void enh_init(void);
/* True when the output frame is the Enhanced one (320*res_scale x 200*res_scale). */
bool enh_hires(void);
/* Compose hook of platform/ega.c: `ega_compose` fills a 320x200 picture; the result is the output frame. */
bool enh_compose(bool (*ega_compose)(u32 *xrgb), u32 *out);

/* Race hooks (ENHANCED.md §2). */
void enh_race_begin(int road_mode);     /* race_drive.c, after shp_load_set() */
void enh_race_end(void);                /* race_drive.c, before arena_reset() at the end of race_run */
void enh_frame_begin(void);             /* first line of road_frame: start of the background layer */
void enh_layer_fg(void);                /* before road_fillmode_end(): the foreground layer starts */
void enh_layer_bg(void);                /* back to the background layer (passthrough brackets) */
void enh_frame_presented(int page);
/* around front_objects (road_draw_front): its pixels belong to the enhanced view where it draws the objects (S2.5),
 * else the original's pixels show (passthrough) */
void enh_objects_begin(void);
void enh_objects_end(void);
/* the same around the mirror's banner and mirror_objects (road_draw_mirror; S2.6) */
void enh_mirror_objects_begin(void);
void enh_mirror_objects_end(void);
/* compare record of an original road-side object call (scenery_draw_*): kind 0 shape, 1 hydrant, 2 sign picture,
 * 3 banner; x2 = banner right edge */
void enh_obj_orig(int kind, int type, int x, int y, int depth, int side, int x2);     /* road_page_flip, before ega_show_page: page 1 or 2 shows this frame */
/* end of opponent_draw's choice: view 0 front / 1 mirror (level s >= 9), sprite level s, variant v, blitted or not */
void enh_opp_drawn(int view, int s, int v, bool drawn);
/* the original's blit rect corner of the car just chosen (r.dx - r.sx, r.dy) and whether the crash / contact rules placed it */
void enh_opp_place(int view, int rx, int ry, bool special);
/* around the car, the start tree and the starter figure of the mirror (S2.5): the enhanced view draws them where it draws
 * the view, else the original's pixels show (passthrough); mirror: the bracket is in the mirror */
void enh_prop_fg(bool mirror);
void enh_prop_bg(bool mirror);

/* Input (S3.1). XT make code of the up (dir > 0) or down shift key during a race; 0 = none. --classic: always A / Z. */
int enh_gear_sc(int dir);
/* A gamepad button went down / up (host.c): shoulder buttons shift during a race. Returns the XT make code to send to the
 * game for the button (Start = Esc), or 0. */
int enh_pad_button(int sdl_button, bool down);
/* Gamepad stick values as the joystick the game reads (host_joy_read): triggers add gas (up) / brake (down). */
void enh_pad_axes(int rt, int lt, short *ay);

/* Written mask (ENHANCED.md §2.1): bits changed in any of the written planes at off. */
void enh_vram_write(u16 off, u8 changed);
void enh_vram_mask_write(u16 off, u8 changed, u8 opaque);
/* Handle a contiguous full-plane write, if the mask does not need per-pixel deltas. */
bool enh_vram_fill(u16 first, unsigned count);
/* From the masked blits (rows_mask_or / rows_mask_blend): the sprite's opaque pixels of the 16-bit write at seg:off.
 * In the foreground they show the original's pixel even where it kept its colour (the starter's black jacket drawn
 * over the original's black sky would otherwise let the enhanced view through). */
void enh_vram_opaque16(u16 seg, u16 off, u16 bits);
/* Around an opaque foreground copy (the starter's part copied from page 2): every pixel it touches shows the original. */
void enh_fg_opaque(bool on);
/* True when pixel (x, y) of race page 1 or 2 was last written by the foreground (shows the original's pixel). */
bool enh_written(int page, int x, int y);

#endif
