#ifndef STREET_ROD_ROAD_GE_H
#define STREET_ROD_ROAD_GE_H

#include "port_psp_layers.h"

extern bool sr_ge_enabled;
extern bool sr_ge_recording;

void sr_ge_reset(void);
void sr_ge_begin(const uint32_t colors[16], unsigned top, unsigned height);
void sr_ge_background(const uint8_t *pixels);
void sr_ge_polygon(const float *xy, int n, uint8_t color, int left, int top, int right, int bottom);
bool sr_ge_commit(const uint8_t *coverage, unsigned height);
const PortPspScene *sr_ge_scene(uint8_t *overlay, uint8_t transparent);

void sr_ge_start_psp(void);
void sr_ge_file_reopened(bool success);
void sr_ge_race_audio(bool active);
PortPspAudioPhase sr_ge_audio_phase(PortPspAudioPhase phase);
void sr_ge_record_road(unsigned mode, uint64_t front_ns, uint64_t mirror_ns, uint64_t cockpit_ns,
                       uint64_t frame_ns);

#endif
