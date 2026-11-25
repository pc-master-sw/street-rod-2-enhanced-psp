#include "engine_road_ge.h"
#include "port_common.h"

#include <math.h>

enum { ROAD_WIDTH = 320, SCREEN_WIDTH = 480, MAX_VERTICES = 12288, CLIP_CAPACITY = 32 };
typedef struct { float x, y; } Point;

static _Alignas(16) PortPspVertex vertices[MAX_VERTICES];
static PortPspScene scene;
static uint32_t palette[16];
static float sx, sy, ox, oy, clip_left, clip_right;
static bool overflow;
static const uint8_t *mask;
bool sr_ge_enabled, sr_ge_recording, sr_ge_widescreen;

void sr_ge_reset(void)
{
    sr_ge_recording = false;
    mask = NULL;
}

void sr_ge_begin(const uint32_t colors[16], unsigned top, unsigned height, bool wide)
{
    sr_ge_reset();
    PortViewport viewport = port_viewport(SCREEN_WIDTH, 272, ROAD_WIDTH, 200, true);
    sx = (float)viewport.width / ROAD_WIDTH;
    sy = (float)viewport.height / 200;
    ox = (float)viewport.x;
    oy = (float)viewport.y;
    for (unsigned i = 0; i < 16; i++) palette[i] = colors[i];
    clip_left = wide ? -ox / sx : 0;
    clip_right = wide ? (SCREEN_WIDTH - ox) / sx : ROAD_WIDTH;
    scene = (PortPspScene){.palette = palette, .vertices = vertices, .top = top,
                          .height = height, .wide = wide};
    overflow = false;
    sr_ge_recording = true;
}

/* Geometry that reaches the horizon or the side of the screen has to
 * extend through the clip planes even when its old endpoint was off-screen. */
float sr_ge_front_edge(bool left)
{
    return left ? clip_left - 1.0f : clip_right + 1.0f;
}

void sr_ge_background(const uint8_t *pixels)
{
    scene.background = pixels;
}

static float plane_distance(Point p, int edge, float limit)
{
    switch (edge) {
    case 0: return p.x - limit;
    case 1: return limit - p.x;
    case 2: return p.y - limit;
    default: return limit - p.y;
    }
}

static PortPspVertex vertex(Point p, uint32_t color)
{
    return (PortPspVertex){color, ox + sx * p.x, oy + sy * p.y, 0.0f};
}

void sr_ge_polygon(const float *xy, int n, uint8_t color, int left, int top, int right, int bottom)
{
    if (overflow || n < 3 || n > 16 || left >= right || top >= bottom) return;
    Point planes[2][CLIP_CAPACITY];
    unsigned inside = 0;
    const bool full_view = left == 0 && right == ROAD_WIDTH;
    const float bounds[] = {full_view ? clip_left : (float)left,
                            full_view ? clip_right : (float)right, (float)top, (float)bottom};
    for (int i = 0; i < n; i++) {
        float x = xy[2 * i], y = xy[2 * i + 1];
        if (!isfinite(x) || !isfinite(y)) return;
        planes[0][i] = (Point){x, y};
        inside += x >= bounds[0] && x <= bounds[1] && y >= top && y <= bottom;
    }
    Point *input = planes[0], *output = planes[1];
    if (inside != (unsigned)n) {
        for (int edge = 0; edge < 4; edge++) {
            int used = 0;
            Point last = input[n - 1];
            float previous = plane_distance(last, edge, bounds[edge]);
            for (int i = 0; i < n; i++) {
                Point current = input[i];
                float distance = plane_distance(current, edge, bounds[edge]);
                if (used + 2 > CLIP_CAPACITY) { overflow = true; return; }
                if ((previous >= 0) != (distance >= 0)) {
                    float t = previous / (previous - distance);
                    Point cut = {last.x + (current.x - last.x) * t,
                                 last.y + (current.y - last.y) * t};
                    if (edge < 2) cut.x = bounds[edge];
                    else cut.y = bounds[edge];
                    output[used++] = cut;
                }
                if (distance >= 0) output[used++] = current;
                last = current;
                previous = distance;
            }
            if (used < 3) return;
            n = used;
            Point *swap = input;
            input = output;
            output = swap;
        }
    }
    if (scene.vertex_count + 3u * (unsigned)(n - 2) > MAX_VERTICES) {
        overflow = true;
        return;
    }
    uint32_t abgr = port_rgba_to_abgr(palette[color & 15]);
    PortPspVertex first = vertex(input[0], abgr);
    for (int i = 1; i < n - 1; i++) {
        vertices[scene.vertex_count++] = first;
        vertices[scene.vertex_count++] = vertex(input[i], abgr);
        vertices[scene.vertex_count++] = vertex(input[i + 1], abgr);
    }
}

bool sr_ge_commit(const uint8_t *coverage, unsigned height)
{
    sr_ge_recording = false;
    if (overflow || !scene.vertex_count || !coverage || !height || height > scene.height)
        return false;
    mask = coverage;
    scene.view_bottom = scene.top + height;
    return true;
}

const PortPspScene *sr_ge_scene(uint8_t *overlay, uint8_t transparent)
{
    if (!mask) return NULL;
    overlay += scene.top * ROAD_WIDTH;
    for (unsigned i = 0; i < (scene.view_bottom - scene.top) * ROAD_WIDTH; i++)
        if (mask[i]) overlay[i] = transparent;
    return &scene;
}
