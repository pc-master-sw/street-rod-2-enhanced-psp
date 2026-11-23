#ifndef PORT_PSP_LAYERS_H
#define PORT_PSP_LAYERS_H

#include "port_platform.h"

typedef struct {
    uint32_t color;
    float x, y, z;
} PortPspVertex;

typedef struct {
    const uint8_t *background;
    const uint32_t *palette;
    const PortPspVertex *vertices;
    unsigned top, height, vertex_count;
} PortPspScene;

typedef struct {
    uint64_t submitted, ready, frames, skipped_polls;
    uint64_t submit_us, ge_until_poll_us, display_latch_us;
} PortPspSceneTiming;

typedef struct {
    uint64_t blocks, underruns, missing_frames, dropped_frames;
    unsigned prebuffer_frames;
} PortPspRaceAudio;

typedef enum {
    PORT_PSP_AUDIO_MENU, PORT_PSP_AUDIO_LOAD, PORT_PSP_AUDIO_DRIVE,
    PORT_PSP_AUDIO_RACE, PORT_PSP_AUDIO_PHASES
} PortPspAudioPhase;

typedef struct {
    uint64_t blocks, underruns;
} PortPspAudioPhaseStats;

void port_psp_buffer_audio(void);
void port_psp_track_race_audio(bool active);
void port_psp_race_audio(PortPspRaceAudio *out);
PortPspAudioPhase port_psp_audio_phase(PortPspAudioPhase phase);
void port_psp_audio_phases(PortPspAudioPhaseStats out[PORT_PSP_AUDIO_PHASES]);
bool port_psp_start_present_worker(void);
bool port_psp_frame_ready(void);
void port_psp_scene_timing(PortPspSceneTiming *out);
bool port_psp_present_indexed_async(const uint8_t *pixels, int w, int h,
                                    const uint32_t *palette, int n);
/* Vertices use display coordinates. Overlay index n is transparent. */
bool port_psp_present_layers(const PortPspScene *scene, const uint8_t *overlay,
                             int w, int h, const uint32_t *palette, int n, bool measure);

#endif
