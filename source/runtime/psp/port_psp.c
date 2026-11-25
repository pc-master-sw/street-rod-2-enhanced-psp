#include "port_common.h"
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <string.h>
#ifdef PORT_PSP_GE_LAYERS
#include <pspge.h>
#include "port_psp_layers.h"
static unsigned char scene_pixels[512*128] __attribute__((aligned(16)));
static unsigned int scene_clut[16] __attribute__((aligned(16)));
static volatile unsigned ge_scene_frames, ge_scene_vertices;
static bool scene_drawing, swap_queued, measure_scene;
static unsigned swap_vcount, submitted_vertices;
static uint64_t submitted_at, swapped_at;
static PortPspSceneTiming scene_timing;
static SceUID present_thread = -1, present_job = -1, present_done = -1;
static volatile int present_stop;
static bool present_pending;
static struct {
    bool ge_ready, displayed;
    uint64_t ge_us, vblank_us;
} present_result;
static PortPspRaceAudio race_audio;
static PortPspAudioPhase audio_phase;
static PortPspAudioPhaseStats audio_phases[PORT_PSP_AUDIO_PHASES];
static PortAudioPlayback audio_playback;
static bool audio_in_race;
static unsigned resume_count;
#endif
enum { PSP_PCM_BLOCK_FRAMES = 1024 };
static unsigned int list[4096] __attribute__((aligned(16)));
static unsigned char pixels[512*512] __attribute__((aligned(16)));
static unsigned int clut[256] __attribute__((aligned(16)));
static short pcm[PSP_PCM_BLOCK_FRAMES * 2] __attribute__((aligned(64)));
static PortAudioRing ring;
static PortStats stats;
static PortConfig cfg;
static char asset_root[768];
static SceUID lock=-1, audio_thread=-1, exit_cb=-1, power_cb=-1;
static int audio_channel=-1;
static volatile int stopping, quit, suspended;
static bool initialized, gu_ready;
static uint32_t previous;
static int exit_callback(int a,int b,void *c) { (void)a;(void)b;(void)c; quit=1; return 0; }
static int power_callback(int a,int flags,void *c) {
    (void)a;(void)c;
    if(flags&(PSP_POWER_CB_SUSPENDING|PSP_POWER_CB_STANDBY)) suspended=1;
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
#ifdef PORT_PSP_GE_LAYERS
        ++resume_count;
#endif
        suspended = 0;
    }
    return 0;
}
static int play_audio(SceSize n,void *p) {
    (void)n;(void)p;
    while(!stopping) {
        if(suspended) { sceKernelDelayThread(10000); continue; }
        sceKernelWaitSema(lock,1,NULL);
#ifdef PORT_PSP_GE_LAYERS
        size_t got = port_ring_pop_buffered(&ring, &audio_playback, pcm, PSP_PCM_BLOCK_FRAMES);
        ++audio_phases[audio_phase].blocks;
        if (audio_playback.started && got < PSP_PCM_BLOCK_FRAMES) {
            ++stats.audio_underruns;
            ++audio_phases[audio_phase].underruns;
        }
        if (audio_in_race) {
            ++race_audio.blocks;
            if (got < PSP_PCM_BLOCK_FRAMES) ++race_audio.underruns;
            race_audio.missing_frames += PSP_PCM_BLOCK_FRAMES - got;
        }
#else
        size_t got = port_ring_pop(&ring, pcm, PSP_PCM_BLOCK_FRAMES);
        if (got < PSP_PCM_BLOCK_FRAMES) ++stats.audio_underruns;
#endif
        sceKernelSignalSema(lock,1);
        int result=sceAudioOutputBlocking(audio_channel,PSP_AUDIO_VOLUME_MAX,pcm);
        if(result<0) { quit=1; break; }
        sceKernelWaitSema(lock,1,NULL); stats.audio_played+=got; sceKernelSignalSema(lock,1);
    }
    return 0;
}
#ifdef PORT_PSP_GE_LAYERS
unsigned port_psp_resume_count(void)
{
    /* Power callbacks are serviced on the game thread. */
    sceKernelCheckCallback();
    return resume_count;
}

void port_psp_buffer_audio(void)
{
    audio_playback.prebuffer = 3 * PSP_PCM_BLOCK_FRAMES;
}

void port_psp_track_race_audio(bool active)
{
    sceKernelWaitSema(lock, 1, NULL);
    audio_in_race = active;
    sceKernelSignalSema(lock, 1);
}

void port_psp_race_audio(PortPspRaceAudio *out)
{
    sceKernelWaitSema(lock, 1, NULL);
    *out = race_audio;
    out->prebuffer_frames = audio_playback.prebuffer;
    sceKernelSignalSema(lock, 1);
}

PortPspAudioPhase port_psp_audio_phase(PortPspAudioPhase phase)
{
    sceKernelWaitSema(lock, 1, NULL);
    PortPspAudioPhase previous = audio_phase;
    audio_phase = phase;
    sceKernelSignalSema(lock, 1);
    return previous;
}

void port_psp_audio_phases(PortPspAudioPhaseStats out[PORT_PSP_AUDIO_PHASES])
{
    sceKernelWaitSema(lock, 1, NULL);
    memcpy(out, audio_phases, sizeof audio_phases);
    sceKernelSignalSema(lock, 1);
}

static int present_worker(SceSize n, void *p)
{
    (void)n;
    (void)p;
    while (sceKernelWaitSema(present_job, 1, NULL) >= 0 && !present_stop) {
        int status = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        present_result.ge_ready = status == PSP_GE_LIST_DONE;
        present_result.displayed = false;
        uint64_t ge_ready_at = measure_scene ? sceKernelGetSystemTimeWide() : 0;
        if (present_result.ge_ready && !suspended && sceDisplayWaitVblankStart() >= 0 && !suspended) {
            sceGuSwapBuffers();
            present_result.displayed = true;
        }
        if (measure_scene) {
            present_result.ge_us = ge_ready_at - submitted_at;
            present_result.vblank_us = sceKernelGetSystemTimeWide() - ge_ready_at;
        }
        sceKernelSignalSema(present_done, 1);
    }
    return 0;
}

bool port_psp_start_present_worker(void)
{
    present_job = sceKernelCreateSema("native_present_job", 0, 0, 1, NULL);
    present_done = sceKernelCreateSema("native_present_done", 0, 0, 1, NULL);
    if (present_job >= 0 && present_done >= 0) {
        present_stop = 0;
        present_thread = sceKernelCreateThread("native_present", present_worker, 0x1C, 8192,
                                               PSP_THREAD_ATTR_USER, NULL);
        if (present_thread >= 0 && sceKernelStartThread(present_thread, 0, NULL) >= 0) return true;
        if (present_thread >= 0) sceKernelDeleteThread(present_thread);
    }
    if (present_job >= 0) sceKernelDeleteSema(present_job);
    if (present_done >= 0) sceKernelDeleteSema(present_done);
    present_thread = present_job = present_done = -1;
    return false;
}

static void record_presented(void)
{
    ++stats.presented;
    if (submitted_vertices) {
        ++ge_scene_frames;
        ge_scene_vertices += submitted_vertices;
    }
}

static bool finish_present_job(bool wait)
{
    if (!present_pending) return true;
    int result = wait ? sceKernelWaitSema(present_done, 1, NULL) : sceKernelPollSema(present_done, 1);
    if (result < 0) {
        if (measure_scene) ++scene_timing.skipped_polls;
        return false;
    }
    present_pending = false;
    if (measure_scene && present_result.ge_ready) {
        ++scene_timing.ready;
        scene_timing.ge_until_poll_us += present_result.ge_us;
    }
    if (present_result.displayed) {
        record_presented();
        if (measure_scene) {
            ++scene_timing.frames;
            scene_timing.display_latch_us += present_result.vblank_us;
        }
    }
    return true;
}
#endif
void port_platform_shutdown(void) {
#ifdef PORT_PSP_GE_LAYERS
    if (present_thread >= 0) {
        finish_present_job(true);
        present_stop = 1;
        sceKernelSignalSema(present_job, 1);
        sceKernelWaitThreadEnd(present_thread, NULL);
        sceKernelDeleteThread(present_thread);
        sceKernelDeleteSema(present_job);
        sceKernelDeleteSema(present_done);
        present_thread = present_job = present_done = -1;
    }
    if (scene_drawing) sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    scene_drawing = swap_queued = false;
#endif
    stopping=1;
    if(audio_thread>=0) { sceKernelWaitThreadEnd(audio_thread,NULL); sceKernelDeleteThread(audio_thread); audio_thread=-1; }
    if(audio_channel>=0) { sceAudioChRelease(audio_channel); audio_channel=-1; }
    if(exit_cb>=0) { sceKernelDeleteCallback(exit_cb); exit_cb=-1; }
    if(power_cb>=0) { scePowerUnregisterCallback(0); sceKernelDeleteCallback(power_cb); power_cb=-1; }
    if(lock>=0) { sceKernelDeleteSema(lock); lock=-1; }
    if(gu_ready) { sceGuDisplay(GU_FALSE); sceGuTerm(); gu_ready=false; }
    initialized=false;
}
bool port_platform_init(const PortConfig *c) {
    if(initialized || !c || c->width<1 || c->height<1 || c->width>512 || c->height>512 ||
       !c->asset_root || strlen(c->asset_root)>=sizeof(asset_root)) return false;
    cfg=*c; strcpy(asset_root,c->asset_root); cfg.asset_root=asset_root;
    stopping=quit=suspended=0; previous=0; memset(&ring,0,sizeof(ring)); memset(&stats,0,sizeof(stats));
#ifdef PORT_PSP_GE_LAYERS
    resume_count = 0;
    audio_playback.started = audio_playback.refilling = audio_in_race = false;
    memset(&race_audio, 0, sizeof(race_audio));
    audio_phase = PORT_PSP_AUDIO_MENU;
    memset(audio_phases, 0, sizeof audio_phases);
#endif
    /* Callbacks are created on main thread and serviced by DelayThreadCB below. */
    exit_cb=sceKernelCreateCallback("native_exit",exit_callback,NULL);
    power_cb=sceKernelCreateCallback("native_power",power_callback,NULL);
    if(exit_cb<0 || power_cb<0 || sceKernelRegisterExitCallback(exit_cb)<0 || scePowerRegisterCallback(0,power_cb)<0) goto fail;
    lock=sceKernelCreateSema("native_audio_lock",0,1,1,NULL); if(lock<0) goto fail;
    if(c->enable_audio) { audio_channel=sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL,PSP_PCM_BLOCK_FRAMES,PSP_AUDIO_FORMAT_STEREO);
      if(audio_channel<0) goto fail; }
    sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    sceGuInit(); gu_ready=true; sceGuStart(GU_DIRECT,list);
    /* Two RGB565 buffers: 557056 bytes in standard 2 MiB eDRAM. */
    sceGuDrawBuffer(GU_PSM_5650,(void*)0,512);
    sceGuDispBuffer(480,272,(void*)(512*272*2),512);
    sceGuOffset(2048-240,2048-136); sceGuViewport(2048,2048,480,272);
    sceGuScissor(0,0,480,272); sceGuEnable(GU_SCISSOR_TEST); sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D); sceGuTexWrap(GU_CLAMP,GU_CLAMP);
    sceGuFinish(); sceGuSync(0,0); sceDisplayWaitVblankStart(); sceGuDisplay(GU_TRUE);
    if(c->enable_audio) { audio_thread=sceKernelCreateThread("native_audio",play_audio,0x12,8192,PSP_THREAD_ATTR_USER,NULL);
      if(audio_thread<0 || sceKernelStartThread(audio_thread,0,NULL)<0) goto fail; }
    initialized=true; return true;
fail: port_platform_shutdown(); return false;
}
bool port_platform_poll_input(PortInput *o) {
    if(!initialized || !o) return false;
    sceKernelCheckCallback(); SceCtrlData pad;
    if(sceCtrlPeekBufferPositive(&pad,1)!=1) return false;
    uint32_t held=0;
    if((pad.Buttons&PSP_CTRL_UP)||pad.Ly<64) held|=PORT_UP;
    if((pad.Buttons&PSP_CTRL_DOWN)||pad.Ly>192) held|=PORT_DOWN;
    if((pad.Buttons&PSP_CTRL_LEFT)||pad.Lx<64) held|=PORT_LEFT;
    if((pad.Buttons&PSP_CTRL_RIGHT)||pad.Lx>192) held|=PORT_RIGHT;
    if(pad.Buttons&PSP_CTRL_CROSS) held|=PORT_FIRE;
    if(pad.Buttons&PSP_CTRL_CIRCLE) held|=PORT_ALT;
    if(pad.Buttons&PSP_CTRL_START) held|=PORT_START;
    if(pad.Buttons&PSP_CTRL_SELECT) held|=PORT_SELECT;
    if(pad.Buttons&PSP_CTRL_SQUARE) held|=PORT_ACTION3;
    if(pad.Buttons&PSP_CTRL_TRIANGLE) held|=PORT_ACTION4;
    if(pad.Buttons&PSP_CTRL_LTRIGGER) held|=PORT_LEFT_TRIGGER;
    if(pad.Buttons&PSP_CTRL_RTRIGGER) held|=PORT_RIGHT_TRIGGER;
    if((held&(PORT_START|PORT_SELECT))==(PORT_START|PORT_SELECT)) quit=1;
    memset(o,0,sizeof(*o)); port_input_update(o,held,&previous); o->quit=quit; o->suspended=suspended;
    return true;
}
static inline void draw_indexed_sprite(float u0, float v0, float u1, float v1,
                                       float x0, float y0, float x1, float y1)
{
    typedef struct { float u, v, x, y, z; } Sprite;
    Sprite *v = sceGuGetMemory(2 * sizeof(*v));
    v[0] = (Sprite){u0, v0, x0, y0, 0};
    v[1] = (Sprite){u1, v1, x1, y1, 0};
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, v);
}

#ifdef PORT_PSP_GE_LAYERS
bool port_psp_frame_ready(void)
{
    if (!initialized || suspended) return false;
    if (present_thread >= 0) return finish_present_job(false);
    if (scene_drawing) {
        int status = sceGuSync(GU_SYNC_FINISH, GU_SYNC_NOWAIT);
        if (status == PSP_GE_LIST_CANCEL_DONE || status < 0) {
            scene_drawing = false;
            return true;
        }
        if (status != PSP_GE_LIST_DONE) {
            if (measure_scene) ++scene_timing.skipped_polls;
            return false;
        }
        if (measure_scene) {
            ++scene_timing.ready;
            scene_timing.ge_until_poll_us += sceKernelGetSystemTimeWide() - submitted_at;
        }
        guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTFRAME);
        sceGuSwapBuffers();
        guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_IMMEDIATE);
        /* Until the pending swap latches, the display still owns the old front buffer. */
        swap_vcount = sceDisplayGetVcount();
        if (measure_scene) swapped_at = sceKernelGetSystemTimeWide();
        scene_drawing = false;
        swap_queued = true;
        record_presented();
    }
    if (swap_queued) {
        if (sceDisplayGetVcount() == swap_vcount) {
            if (measure_scene) ++scene_timing.skipped_polls;
            return false;
        }
        swap_queued = false;
        if (measure_scene) {
            ++scene_timing.frames;
            scene_timing.display_latch_us += sceKernelGetSystemTimeWide() - swapped_at;
        }
    }
    return true;
}

void port_psp_scene_timing(PortPspSceneTiming *out)
{
    finish_present_job(true);
    *out = scene_timing;
}

static void draw_scene_background(const PortPspScene *scene, PortViewport dst, int w, int h)
{
    for (unsigned y = 0; y < scene->height; y++)
        memcpy(scene_pixels + y * 512, scene->background + y * w, (size_t)w);
    for (unsigned i = 0; i < 16; i++) scene_clut[i] = port_rgba_to_abgr(scene->palette[i]);
    sceKernelDcacheWritebackRange(scene_pixels, scene->height * 512);
    sceKernelDcacheWritebackRange(scene_clut, sizeof scene_clut);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 15, 0);
    sceGuClutLoad(2, scene_clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, 512, 128, 512, scene_pixels);
    sceGuTexFlush();
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    float top = dst.y + (float)scene->top * dst.height / h;
    float bottom = dst.y + (float)(scene->top + scene->height) * dst.height / h;
    for (int left = 0; left < w; left += 32) {
        int right = left + 32 < w ? left + 32 : w;
        draw_indexed_sprite(left, 0, right, scene->height,
                            dst.x + (float)left * dst.width / w, top,
                            dst.x + (float)right * dst.width / w, bottom);
    }
    if (scene->wide) {
        /* The same wrapped panorama is already used by the center of the view. */
        float left = (float)dst.x * w / dst.width;
        float right = (float)(480 - dst.x - dst.width) * w / dst.width;
        draw_indexed_sprite(w - left, 0, w, scene->height, 0, top, dst.x, bottom);
        draw_indexed_sprite(0, 0, right, scene->height,
                            dst.x + dst.width, top, 480, bottom);
    }
}

static void draw_scene(const PortPspScene *scene, PortViewport dst, int w, int h)
{
    if (scene->background) draw_scene_background(scene, dst, w, h);
    sceKernelDcacheWritebackRange(scene->vertices, scene->vertex_count * sizeof(*scene->vertices));
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   scene->vertex_count, NULL, scene->vertices);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuAlphaFunc(GU_GREATER, 0, 0xff);
    sceGuEnable(GU_ALPHA_TEST);
}

static void draw_frame_sides(PortViewport dst, int w, int h, unsigned top, unsigned bottom)
{
    float y0 = dst.y + (float)top * dst.height / h;
    float y1 = dst.y + (float)bottom * dst.height / h;
    int edge = dst.x + dst.width;
    /* Carry the border colors outside the scene, keeping the original frame centered. */
    draw_indexed_sprite(0, 0, 1, top, 0, dst.y, dst.x, y0);
    draw_indexed_sprite(w - 1, 0, w, top, edge, dst.y, 480, y0);
    draw_indexed_sprite(0, bottom, 1, h, 0, y1, dst.x, dst.y + dst.height);
    draw_indexed_sprite(w - 1, bottom, w, h, edge, y1, 480, dst.y + dst.height);
}

static bool present_indexed(const uint8_t *p,int w,int h,int pitch,const uint32_t *pal,int n,
                            const PortPspScene *scene, bool async, bool measure) {
    if (scene && (!scene->height || scene->height > 128 || scene->top + scene->height > (unsigned)h
        || scene->view_bottom <= scene->top || scene->view_bottom > scene->top + scene->height
        || !scene->palette || !scene->vertices || !scene->vertex_count || n >= 255))
        return false;
    if(!initialized || suspended || w!=cfg.width || h!=cfg.height ||
       !port_frame_valid(p,w,h,pitch,pal,n+(scene!=NULL))) return false;
    if (scene_drawing || swap_queued || present_pending) return false;
    uint64_t submit_started = measure ? sceKernelGetSystemTimeWide() : 0;
#else
bool port_platform_present_indexed(const uint8_t *p,int w,int h,int pitch,const uint32_t *pal,int n) {
    if(!initialized || suspended || w!=cfg.width || h!=cfg.height || !port_frame_valid(p,w,h,pitch,pal,n)) return false;
#endif
    for(int y=0;y<h;++y) memcpy(pixels+y*512,p+(size_t)y*pitch,(size_t)w);
    memset(clut,0,sizeof(clut)); for(int i=0;i<n;++i) clut[i]=port_rgba_to_abgr(pal[i]);
    sceKernelDcacheWritebackRange(pixels,h*512); sceKernelDcacheWritebackRange(clut,sizeof(clut));
    sceGuStart(GU_DIRECT,list); sceGuClearColor(0); sceGuClear(GU_COLOR_BUFFER_BIT);
    PortViewport dst=port_viewport(480,272,w,h,cfg.aspect_4_3);
#ifdef PORT_PSP_GE_LAYERS
    if (scene) draw_scene(scene, dst, w, h);
#endif
    sceGuClutMode(GU_PSM_8888,0,255,0); sceGuClutLoad(32,clut);
    sceGuTexMode(GU_PSM_T8,0,0,0); sceGuTexImage(0,512,512,512,pixels); sceGuTexFlush();
#ifdef PORT_PSP_GE_LAYERS
    sceGuTexFunc(GU_TFX_REPLACE,scene ? GU_TCC_RGBA : GU_TCC_RGB);
#else
    sceGuTexFunc(GU_TFX_REPLACE,GU_TCC_RGB);
#endif
    sceGuTexFilter(GU_NEAREST,GU_NEAREST);
    sceGuTexScale(1.0f,1.0f); sceGuTexOffset(0,0);
    /* Narrow sprites avoid the GE sprite rasterizer's wide-texture penalty. */
    for(int left=0;left<w;left+=32) {
        int right=left+32<w?left+32:w;
        draw_indexed_sprite(left, 0, right, h,
                            dst.x + (float)left * dst.width / w, dst.y,
                            dst.x + (float)right * dst.width / w, dst.y + dst.height);
    }
#ifdef PORT_PSP_GE_LAYERS
    if (scene && scene->wide) draw_frame_sides(dst, w, h, scene->top, scene->view_bottom);
    if (scene) sceGuDisable(GU_ALPHA_TEST);
    if (async) {
        sceGuFinish();
        measure_scene = measure;
        submitted_vertices = scene ? scene->vertex_count : 0;
        if (measure) {
            submitted_at = sceKernelGetSystemTimeWide();
            ++scene_timing.submitted;
            scene_timing.submit_us += submitted_at - submit_started;
        }
        /* The main thread keeps the list and textures unchanged until present_done. */
        if (present_thread >= 0) {
            present_pending = true;
            sceKernelSignalSema(present_job, 1);
        } else {
            scene_drawing = true;
        }
        return true;
    }
#endif
    sceGuFinish(); sceGuSync(0,0); sceDisplayWaitVblankStart(); sceGuSwapBuffers(); ++stats.presented;
    return true;
}
#ifdef PORT_PSP_GE_LAYERS
bool port_platform_present_indexed(const uint8_t *p,int w,int h,int pitch,const uint32_t *pal,int n)
{
    return present_indexed(p,w,h,pitch,pal,n,NULL,false,false);
}

bool port_psp_present_indexed_async(const uint8_t *p, int w, int h, const uint32_t *pal, int n)
{
    return present_indexed(p,w,h,w,pal,n,NULL,true,true);
}

bool port_psp_present_layers(const PortPspScene *scene, const uint8_t *overlay,
                             int w, int h, const uint32_t *palette, int n, bool measure)
{
    return present_indexed(overlay,w,h,w,palette,n,scene,true,measure);
}
#endif
size_t port_platform_audio_push_s16(const int16_t *s,size_t n) {
    if(!initialized || suspended || audio_channel<0) return 0;
    sceKernelWaitSema(lock,1,NULL); size_t got=port_ring_push(&ring,s,n);
#ifdef PORT_PSP_GE_LAYERS
    if (audio_in_race) race_audio.dropped_frames += n - got;
#endif
    stats.audio_accepted+=got; sceKernelSignalSema(lock,1); return got;
}
void port_platform_get_stats(PortStats *o) {
    if(!o) return;
    if(lock>=0) sceKernelWaitSema(lock,1,NULL);
    *o=stats; o->audio_queued=ring.count;
    if(lock>=0) sceKernelSignalSema(lock,1);
}
uint64_t port_platform_ticks_us(void) { return sceKernelGetSystemTimeWide(); }
void port_platform_sleep_us(uint32_t us) { sceKernelDelayThreadCB(us?us:1); }
bool port_platform_read_asset(const char *r,void *d,size_t c,size_t *n) { return initialized && port_asset_read(asset_root,r,d,c,n); }
bool port_platform_read_pixel(int x,int y,uint32_t *rgb) {
    if(!initialized || !rgb || x<0 || x>=480 || y<0 || y>=272) return false;
    void *base; int stride,format;
    if(sceDisplayGetFrameBuf(&base,&stride,&format,PSP_DISPLAY_SETBUF_IMMEDIATE)<0 || !base || format!=PSP_DISPLAY_PIXEL_FORMAT_565) return false;
    uint16_t c=((volatile uint16_t*)base)[y*stride+x];
    *rgb=(((c&31)*255/31)<<16)|((((c>>5)&63)*255/63)<<8)|(((c>>11)&31)*255/31); return true;
}
