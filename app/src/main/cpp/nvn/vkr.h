/* Vulkan renderer: replays the NVN op streams the front end records onto the real GPU (first version: synchronous
 * submits, every image in GENERAL layout, guest buffers copied into a per-submit upload ring). */
#pragma once
#include <pthread.h>
#include <vulkan/vulkan.h>
#include "volk.h" /* Vulkan calls go through volk pointers (gpu_driver.c) */

#include "nvn_api.h"
#include "nvn_obj.h"

typedef struct VkrCtx {
    int ready;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t family;
    VkQueue queue;
    pthread_mutex_t qmu; /* the queue is shared with the window thread */
    int depth_clip_control;
    float max_aniso; /* 1 without the samplerAnisotropy feature */
    uint32_t subgroup_size; /* device subgroup size (Adreno 740: 64, Mali Valhall: 16); 0 until the device exists */
    uint32_t sbuf_align;    /* minStorageBufferOffsetAlignment (logged only: the translator always gets 64, see shader_cache.c) */
    int clip_distance;      /* shaderClipDistance (Adreno yes, Mali-G68 no: the translator then leaves gl_ClipDistance out) */
    int pipe_cache_control; /* VK_EXT_pipeline_creation_cache_control: ask the cache only, without compiling */
} VkrCtx;
extern VkrCtx g_vkr; /* filled by main.c once the device exists */

/* shader variant key of the program bound to an NVN stage under the current draw state (gfx_null.c) */
int ts_shader_key(int stage, char key[40], const uint8_t **ctl, const uint8_t **code, uint32_t *len);
uint32_t ts_stage_gen(int stage); /* changes whenever the stage's key (or its plain/texture-type answers) may have */
int ts_stage_plain(int stage); /* default draw state for the stage: the code-hash translation applies */
int ts_tex_type(int stage, int slot); /* translator type code of the texture bound to the slot (2D when none) */
/* the .ans record (u32 stage, tex[32], img[32], img_fmt[32], attr[32], fout[8], u32 shared) of the stage under the
 * current draw state; 0 unless it is the one the per-state key "<hash>_<answers hash>" names */
#define TS_ANS_SIZE (4 + 32 * 4 + 8 + 4)
int ts_stage_answers(int stage, uint8_t buf[TS_ANS_SIZE], const char *key);
/* 1 when <data>/capture_shaders exists: write the .bin/.ans shader inputs for the PC tools (nvn_api_obj.c) */
int som_shader_capture_on(void);

void vkr_texture_register(int id, const tex_t *t, const view_t *v);
void vkr_texture_release(const tex_t *t);
void vkr_sampler_register(int id, const samp_par *s);
void vkr_submit_begin(void);
void vkr_submit_end(void);
void vkr_op(const op_hdr *h, const void *payload);
/* crop: the window's crop rectangle (x, y, w, h; nvnWindowSetCrop): the part of the texture that is the picture. The
 * handheld profile renders 1280x720 into the top-left of a 1920x1080 window texture. NULL or w/h 0: the whole texture. */
void vkr_present(const tex_t *t, const int crop[4]);
/* the game's real frame rate over the last second, from the times of its presents (overlay.c) */
typedef struct { uint32_t frames; float fps, avg_ms, max_ms; } VkrFrameStats;
void vkr_frame_stats(VkrFrameStats *out);
/* the gaps between the last presents in ms, oldest first (up to max, at most 255); returns how many */
int vkr_frame_times(float *out, int max);
/* window thread: blit the last presented image into dst (layout TRANSFER_DST_OPTIMAL); 1 if it did */
int vkr_blit_present(VkCommandBuffer cb, VkImage dst, VkExtent2D ext);
