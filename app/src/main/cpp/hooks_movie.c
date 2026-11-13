/* movie::Decoder (the SDK's nn::movie video/audio decoder, subsdk0) on Android MediaCodec.
 *
 * The game demuxes its .webm movies with movie::Extractor (pure CPU, left as is) and decodes each track with a
 * movie::Decoder. The original DecoderImpl drives NVIDIA's hardware decoder (NvMM / OpenMAX over /dev/nvhost-nvdec),
 * which does not exist here, so every movie::Decoder method is replaced and the work goes to AMediaCodec.
 *
 * The contract, read from DecoderImpl in subsdk0 2.0.0:
 *   - the client takes free input slots (AcquireInputBufferIndex -> index, remaining), fills them through
 *     GetInputBuffer (a movie::Buffer pointed at the slot) and hands them back with SendInputBufferForDecode;
 *   - decoded frames are listed for AcquireOutputBufferIndex (index, pts, flags, remaining), copied out by
 *     GetOutputBuffer into the client's own movie::Buffer (CPU mode: packed NV12 or 32-bit RGBA video, PCM16 audio)
 *     and returned with ReleaseOutputBufferIndex;
 *   - four nn::os::EventType pointers (DecoderEvents: input available, output available, format changed, error)
 *     are signalled as slots appear. Signalling needs a guest thread, so one helper guest thread (made with the
 *     SDK's own nn::os::CreateThread) does it for the host worker threads.
 * Statuses: 0 ok, 0x100b bad argument, 0x1006 unsupported, 0x100e no output yet, 0x1011 no input slot free. */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include "debug_log.h"
#include "hook.h"
#include "movies.h"
#include "nvn/host.h"

enum { ST_OK = 0, ST_UNSUPPORTED = 0x1006, ST_BAD_ARG = 0x100b, ST_NO_OUTPUT = 0x100e, ST_NO_INPUT = 0x1011,
       ST_ERROR = 0x1000 };
enum { TYPE_AVC = 1, TYPE_VP8 = 2, TYPE_VP9 = 3, TYPE_AAC = 4, TYPE_VORBIS = 5 };
enum { FMT_PCM16 = 1, FMT_RGBA = 2, FMT_NV12 = 4 };
enum { EV_INPUT, EV_OUTPUT, EV_FORMAT, EV_ERROR };
#define FLAG_EOS 4u
#define NIN 8
#define NOUT 8
#define IN_CAP (2u << 20)
#define MAX_DEC 8

/* subsdk0 2.0.0 offsets: movie::Decoder::Start (to find the module base) and the operator delete PLT stub */
#define OFF_DECODER_START 0x11db20
#define OFF_PLT_DELETE 0x332390

typedef struct Dec {
    uint64_t guest; /* the movie::Decoder object */
    int type, fmt, mode, video;
    AMediaCodec *mc;
    int configured, started, broken;
    uint32_t last_error;
    uint64_t events[4];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t worker;
    int worker_on, stop;

    uint8_t *in_buf[NIN];
    int in_free[NIN], n_in_free; /* FIFO of slots the client may take */
    struct { int idx, off, size; int64_t pts; uint32_t flags; } in_q[NIN];
    int n_in_q; /* filled slots waiting for the codec */

    uint8_t *out_buf[NOUT];
    size_t out_cap[NOUT];
    int out_size[NOUT], out_state[NOUT]; /* 0 free, 1 listed, 2 taken by the client */
    int64_t out_pts[NOUT];
    uint32_t out_flags[NOUT];
    int out_ready[NOUT], n_out_ready;
    int eos_out;

    /* stream format */
    int width, height, channels, rate;
    int src_stride, src_slice, src_color, crop_l, crop_t;
    int fmt_known;
    unsigned frames;

    /* Playing the movie's H.264 copy (movies.c) on the hardware decoder instead of the VP9 stream, on devices whose VP9
     * decoder is software. The game's packets still arrive; each one is answered with the copy's frame of the same
     * timestamp. sq: the copy's samples in the codec, with the game timestamp each answers (drop: decoded only to reach
     * a frame after a seek, never shown). */
    AMediaExtractor *side;
    int side_fd, side_seek;
    struct { int64_t sample, guest; int drop; } sq[64];
    int sq_n;

    /* pacing counters, logged every 5 s while video plays: packets from the game, copy samples fed / decoded only,
     * frames listed for the game, frames it took, and the time spent copying frames out (ns) */
    unsigned st_in, st_fed, st_hidden, st_out, st_taken;
    uint64_t st_copy_ns, st_start_ns;
    /* timing: packets whose pts went backwards, flushes, and how far behind the newest packet each listed frame is */
    unsigned st_back, st_flush;
    int64_t st_last_in_pts, st_lag_sum_us, st_lag_max_us;
} Dec;

static const char *type_mime(int t);

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* caller holds d->mu */
static void stats_tick(Dec *d) {
    uint64_t now = mono_ns();
    if (!d->st_start_ns) d->st_start_ns = now;
    if (now - d->st_start_ns < 5000000000ull) return;
    double s = (double)(now - d->st_start_ns) / 1e9;
    som_log("movie: %.1f s of %s%s: game sent %.1f packets/s, fed %.1f/s (%u decoded only), listed %.1f frames/s, game took %.1f/s, "
            "copy-out %.1f ms/frame; %u packets went back in time, %u flushes; frames listed %.0f ms behind the newest packet "
            "(max %.0f)", s, type_mime(d->type), d->side ? " (H.264 copy)" : "", d->st_in / s, d->st_fed / s, d->st_hidden,
            d->st_out / s, d->st_taken / s, d->st_taken ? (double)d->st_copy_ns / 1e6 / d->st_taken : 0.0, d->st_back, d->st_flush,
            d->st_out ? (double)d->st_lag_sum_us / 1e3 / d->st_out : 0.0, (double)d->st_lag_max_us / 1e3);
    d->st_in = d->st_fed = d->st_hidden = d->st_out = d->st_taken = d->st_back = d->st_flush = 0;
    d->st_lag_sum_us = d->st_lag_max_us = 0;
    d->st_copy_ns = 0, d->st_start_ns = now;
}

static Dec *g_dec[MAX_DEC];
static pthread_mutex_t g_dec_lock = PTHREAD_MUTEX_INITIALIZER;

static Dec *dec_of(GuestCtx *c) {
    uint64_t self = c->x[0];
    pthread_mutex_lock(&g_dec_lock);
    Dec *d = NULL;
    for (int i = 0; i < MAX_DEC; i++)
        if (g_dec[i] && g_dec[i]->guest == self) d = g_dec[i];
    pthread_mutex_unlock(&g_dec_lock);
    return d;
}

static const char *type_mime(int t) {
    switch (t) {
    case TYPE_AVC: return "video/avc";
    case TYPE_VP8: return "video/x-vnd.on2.vp8";
    case TYPE_VP9: return "video/x-vnd.on2.vp9";
    case TYPE_AAC: return "audio/mp4a-latm";
    case TYPE_VORBIS: return "audio/vorbis";
    default: return NULL;
    }
}

/* ---- the signalling guest thread ---- */

static pthread_mutex_t g_pump_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pump_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_pump_q[64];
static int g_pump_n, g_pump_started;
static uint64_t g_signal_event;

static void pump_post(uint64_t ev) {
    if (!ev) return;
    pthread_mutex_lock(&g_pump_mu);
    int dup = 0;
    for (int i = 0; i < g_pump_n; i++) dup |= g_pump_q[i] == ev;
    if (!dup && g_pump_n < 64) g_pump_q[g_pump_n++] = ev;
    pthread_cond_signal(&g_pump_cv);
    pthread_mutex_unlock(&g_pump_mu);
}

static void pump_forget(const uint64_t *evs, int n) {
    pthread_mutex_lock(&g_pump_mu);
    int k = 0;
    for (int i = 0; i < g_pump_n; i++) {
        int drop = 0;
        for (int j = 0; j < n; j++) drop |= g_pump_q[i] == evs[j];
        if (!drop) g_pump_q[k++] = g_pump_q[i];
    }
    g_pump_n = k;
    pthread_mutex_unlock(&g_pump_mu);
}

/* the helper thread's entry (a guest function made by hook_register_dynamic); never returns */
static void pump_main(GuestCtx *c) {
    som_log("movie: event thread running");
    for (;;) {
        pthread_mutex_lock(&g_pump_mu);
        while (!g_pump_n) pthread_cond_wait(&g_pump_cv, &g_pump_mu);
        uint64_t ev = g_pump_q[0];
        memmove(g_pump_q, g_pump_q + 1, (size_t)(--g_pump_n) * sizeof g_pump_q[0]);
        pthread_mutex_unlock(&g_pump_mu);
        rt_call(c, g_signal_event, 1, ev);
    }
}

static int pump_start(GuestCtx *c) {
    if (g_pump_started) return 1;
    g_signal_event = module_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    uint64_t create = module_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    uint64_t start = module_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    uint64_t name = module_symbol("_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc");
    uint64_t memalign = module_symbol("memalign");
    if (!g_signal_event || !create || !start || !memalign) {
        som_log("movie: SDK thread functions not found; no decoder events");
        return 0;
    }
    const uint64_t stack_size = 0x10000;
    uint64_t tt = rt_call(c, memalign, 2, (uint64_t)0x40, (uint64_t)0x400);
    uint64_t stack = rt_call(c, memalign, 2, (uint64_t)0x1000, stack_size);
    if (!tt || !stack) {
        som_log("movie: no guest memory for the event thread");
        return 0;
    }
    memset((void *)(uintptr_t)tt, 0, 0x400);
    uint64_t entry = hook_register_dynamic("movie-event-thread", pump_main);
    uint32_t r = (uint32_t)rt_call(c, create, 6, tt, entry, (uint64_t)0, stack, stack_size, (uint64_t)10);
    if (r) {
        som_log("movie: CreateThread for the event thread failed: 0x%x", r);
        return 0;
    }
    if (name) rt_call(c, name, 2, tt, (uint64_t)(uintptr_t)"MovieDecoderEvents");
    rt_call(c, start, 1, tt);
    g_pump_started = 1;
    return 1;
}

static void dec_signal(Dec *d, int which) { pump_post(d->events[which]); }

/* ---- guest movie::Buffer and MediaData ---- */

typedef struct {
    int32_t offset, size;
    uint64_t pad8;
    uint8_t owns;
    uint8_t pad11[7];
    uint64_t capacity;
    uint64_t pad20;
    uint64_t data;
} GuestBuffer;

static void md_set_i32(GuestCtx *c, uint64_t md, const char *k, int v) {
    static uint64_t fn;
    if (!fn) fn = module_symbol("_ZN5movie9MediaData8SetInt32EPKci");
    if (fn && md) rt_call(c, fn, 3, md, (uint64_t)(uintptr_t)k, (uint64_t)(uint32_t)v);
}
static void md_set_str(GuestCtx *c, uint64_t md, const char *k, const char *v) {
    static uint64_t fn;
    if (!fn) fn = module_symbol("_ZN5movie9MediaData9SetStringEPKcS2_");
    if (fn && md) rt_call(c, fn, 3, md, (uint64_t)(uintptr_t)k, (uint64_t)(uintptr_t)v);
}
static void md_set_rect(GuestCtx *c, uint64_t md, const char *k, int l, int t, int r, int b) {
    static uint64_t fn;
    if (!fn) fn = module_symbol("_ZN5movie9MediaData7SetRectEPKciiii");
    if (fn && md)
        rt_call(c, fn, 6, md, (uint64_t)(uintptr_t)k, (uint64_t)(uint32_t)l, (uint64_t)(uint32_t)t, (uint64_t)(uint32_t)r,
                (uint64_t)(uint32_t)b);
}
static int md_find_i32(GuestCtx *c, uint64_t md, const char *k, int *out) {
    static uint64_t fn;
    if (!fn) fn = module_symbol("_ZNK5movie9MediaData9FindInt32EPKcPi");
    return fn && md && (rt_call(c, fn, 3, md, (uint64_t)(uintptr_t)k, (uint64_t)(uintptr_t)out) & 1);
}
static int md_find_i64(GuestCtx *c, uint64_t md, const char *k, int64_t *out) {
    static uint64_t fn;
    if (!fn) fn = module_symbol("_ZNK5movie9MediaData9FindInt64EPKcPl");
    return fn && md && (rt_call(c, fn, 3, md, (uint64_t)(uintptr_t)k, (uint64_t)(uintptr_t)out) & 1);
}
static GuestBuffer *md_find_buf(GuestCtx *c, uint64_t md, const char *k) {
    static uint64_t fn;
    if (!fn) fn = module_symbol("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
    uint64_t b = 0;
    if (!fn || !md || !(rt_call(c, fn, 3, md, (uint64_t)(uintptr_t)k, (uint64_t)(uintptr_t)&b) & 1)) return NULL;
    return (GuestBuffer *)(uintptr_t)b;
}

/* ---- output conversion ---- */

static size_t out_bytes(const Dec *d) {
    if (!d->video) return 0;
    size_t w = (size_t)d->width, h = (size_t)d->height;
    return d->fmt == FMT_RGBA ? w * h * 4 : w * h + (w / 2) * (h / 2) * 2;
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* copies the cropped picture of a decoder buffer (I420 or NV12 layout, any stride) into a packed slot */
static void convert_video(const Dec *d, const uint8_t *src, size_t src_len, uint8_t *dst) {
    int w = d->width, h = d->height, stride = d->src_stride, slice = d->src_slice;
    int planar = d->src_color == 19; /* COLOR_FormatYUV420Planar; everything else is read as semi-planar */
    const uint8_t *y0 = src + (size_t)d->crop_t * stride + d->crop_l;
    const uint8_t *uv0 = src + (size_t)stride * slice;
    size_t need = (size_t)stride * slice + (size_t)stride * ((slice + 1) / 2);
    if (src_len < (size_t)stride * (d->crop_t + h)) {
        static int warned;
        if (warned++ < 3) som_log("movie: decoder buffer of %zu bytes is smaller than the picture (stride %d)", src_len, stride);
        return;
    }
    int cw = (w + 1) / 2, ch = (h + 1) / 2, ct = d->crop_t / 2, cl = d->crop_l / 2;
    if (d->fmt == FMT_NV12) {
        for (int r = 0; r < h; r++) memcpy(dst + (size_t)r * w, y0 + (size_t)r * stride, (size_t)w);
        uint8_t *duv = dst + (size_t)w * h;
        if (planar) {
            int cs = stride / 2;
            const uint8_t *u = uv0, *v = uv0 + (size_t)cs * ((slice + 1) / 2);
            if (src_len < (size_t)(v - src) + (size_t)cs * (ct + ch)) return;
            for (int r = 0; r < h / 2; r++)
                for (int x = 0; x < w / 2; x++) {
                    duv[(size_t)r * w + 2 * x] = u[(size_t)(r + ct) * cs + cl + x];
                    duv[(size_t)r * w + 2 * x + 1] = v[(size_t)(r + ct) * cs + cl + x];
                }
        } else {
            if (src_len < need) {
                static int warned;
                if (warned++ < 3) som_log("movie: decoder buffer of %zu bytes has no full chroma plane (%zu needed)", src_len, need);
                return;
            }
            for (int r = 0; r < h / 2; r++) memcpy(duv + (size_t)r * w, uv0 + (size_t)(r + ct) * stride + 2 * cl, (size_t)w);
        }
        (void)cw;
        return;
    }
    /* 32-bit RGBA (BT.601 limited range) */
    int cs = planar ? stride / 2 : stride;
    const uint8_t *u = uv0, *v = planar ? uv0 + (size_t)cs * ((slice + 1) / 2) : uv0 + 1;
    for (int r = 0; r < h; r++) {
        uint8_t *o = dst + (size_t)r * w * 4;
        for (int x = 0; x < w; x++) {
            int Y = y0[(size_t)r * stride + x] - 16;
            size_t ci = planar ? (size_t)(r / 2 + ct) * cs + cl + x / 2 : (size_t)(r / 2 + ct) * cs + 2 * (cl + x / 2);
            int U = u[ci] - 128, V = v[ci] - 128;
            int Yc = 298 * Y + 128;
            o[4 * x] = clamp8((Yc + 409 * V) >> 8);
            o[4 * x + 1] = clamp8((Yc - 100 * U - 208 * V) >> 8);
            o[4 * x + 2] = clamp8((Yc + 516 * U) >> 8);
            o[4 * x + 3] = 255;
        }
    }
    (void)ch;
}

/* ---- the worker: feeds the codec and drains it ---- */

static void read_format(Dec *d) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(d->mc);
    if (!f) return;
    int32_t v;
    if (d->video) {
        int w = d->width, h = d->height;
        if (AMediaFormat_getInt32(f, "width", &v)) w = v;
        if (AMediaFormat_getInt32(f, "height", &v)) h = v;
        d->src_stride = AMediaFormat_getInt32(f, "stride", &v) && v > 0 ? v : w;
        d->src_slice = AMediaFormat_getInt32(f, "slice-height", &v) && v > 0 ? v : h;
        d->src_color = AMediaFormat_getInt32(f, "color-format", &v) ? v : 21;
        int32_t l, t, r, b;
        d->crop_l = d->crop_t = 0;
        if (AMediaFormat_getInt32(f, "crop-left", &l) && AMediaFormat_getInt32(f, "crop-top", &t) &&
            AMediaFormat_getInt32(f, "crop-right", &r) && AMediaFormat_getInt32(f, "crop-bottom", &b)) {
            d->crop_l = l, d->crop_t = t, w = r - l + 1, h = b - t + 1;
        }
        d->width = w & ~1, d->height = h & ~1;
    } else {
        if (AMediaFormat_getInt32(f, "channel-count", &v)) d->channels = v;
        if (AMediaFormat_getInt32(f, "sample-rate", &v)) d->rate = v;
    }
    som_log("movie: decoder %s output format %s", type_mime(d->type), AMediaFormat_toString(f));
    AMediaFormat_delete(f);
    d->fmt_known = 1;
}

/* the game's oldest waiting packet is done: its slot is free again (caller holds d->mu) */
static void in_q_pop(Dec *d) {
    int slot = d->in_q[0].idx;
    memmove(d->in_q, d->in_q + 1, (size_t)(--d->n_in_q) * sizeof d->in_q[0]);
    d->in_free[d->n_in_free++] = slot;
    dec_signal(d, EV_INPUT);
}

/* One step of answering the game's oldest packet (pts P) from the H.264 copy: feeds one sample of the copy. Samples
 * before P (after a seek to the copy's previous keyframe) are decoded but not shown; the sample at P answers the packet.
 * Called with d->mu held; returns 1 when something was fed. */
static int side_feed_step(Dec *d) {
    int64_t P = d->in_q[0].pts;
    uint32_t flags = d->in_q[0].flags;
    int size = d->in_q[0].size;
    if (d->sq_n >= (int)(sizeof d->sq / sizeof d->sq[0])) return 0; /* the codec holds enough: drain first */
    pthread_mutex_unlock(&d->mu);
    int fed = 0, answered = 0;
    if ((flags & FLAG_EOS) && size == 0) {
        ssize_t ci = AMediaCodec_dequeueInputBuffer(d->mc, 2000);
        if (ci >= 0) {
            AMediaCodec_queueInputBuffer(d->mc, (size_t)ci, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            fed = answered = 1;
        }
        pthread_mutex_lock(&d->mu);
        if (answered) in_q_pop(d);
        return fed;
    }
    int64_t t = AMediaExtractor_getSampleTime(d->side);
    /* positioned wrong: first packet, after a Flush, the game jumped back, or more than 2 s ahead of the copy */
    if (d->side_seek || t < 0 || t > P + 40000 || t < P - 2000000) {
        AMediaExtractor_seekTo(d->side, P, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
        d->side_seek = 0;
        t = AMediaExtractor_getSampleTime(d->side);
    }
    if (t < 0) { /* the copy has no frame this late: let the packet go unanswered */
        pthread_mutex_lock(&d->mu);
        in_q_pop(d);
        return 1;
    }
    ssize_t ci = AMediaCodec_dequeueInputBuffer(d->mc, 2000);
    if (ci >= 0) {
        size_t cap = 0;
        uint8_t *dst = AMediaCodec_getInputBuffer(d->mc, (size_t)ci, &cap);
        ssize_t n = dst ? AMediaExtractor_readSampleData(d->side, dst, cap) : -1;
        int drop = t < P - 2000; /* 2 ms of slack: both timelines come from millisecond WebM timestamps */
        AMediaCodec_queueInputBuffer(d->mc, (size_t)ci, 0, n > 0 ? (size_t)n : 0, (uint64_t)t,
                                     (!drop && (flags & FLAG_EOS)) ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
        AMediaExtractor_advance(d->side);
        pthread_mutex_lock(&d->mu);
        d->sq[d->sq_n].sample = t, d->sq[d->sq_n].guest = P, d->sq[d->sq_n].drop = drop;
        d->sq_n++;
        d->st_fed++;
        if (drop) d->st_hidden++;
        if (!drop) in_q_pop(d);
        return 1;
    }
    pthread_mutex_lock(&d->mu);
    return 0;
}

/* the game timestamp of a decoded sample of the copy, and whether to show it (caller holds d->mu) */
static int side_map_output(Dec *d, int64_t sample, int64_t *guest) {
    for (int i = 0; i < d->sq_n; i++)
        if (d->sq[i].sample == sample) {
            int drop = d->sq[i].drop;
            *guest = d->sq[i].guest;
            d->sq_n -= i + 1; /* earlier entries never came out: forget them */
            memmove(d->sq, d->sq + i + 1, (size_t)d->sq_n * sizeof d->sq[0]);
            return !drop;
        }
    *guest = sample;
    return 1;
}

static void *worker_main(void *arg) {
    Dec *d = arg;
    pthread_mutex_lock(&d->mu);
    while (!d->stop) {
        int did = 0;
        /* feed one waiting input slot (from the H.264 copy when there is one) */
        if (d->side && d->n_in_q > 0) {
            did |= side_feed_step(d);
        } else if (d->n_in_q > 0) {
            pthread_mutex_unlock(&d->mu);
            ssize_t ci = AMediaCodec_dequeueInputBuffer(d->mc, 2000);
            pthread_mutex_lock(&d->mu);
            if (ci >= 0 && d->n_in_q > 0 && !d->stop) {
                int slot = d->in_q[0].idx, off = d->in_q[0].off, size = d->in_q[0].size;
                int64_t pts = d->in_q[0].pts;
                uint32_t flags = d->in_q[0].flags;
                memmove(d->in_q, d->in_q + 1, (size_t)(--d->n_in_q) * sizeof d->in_q[0]);
                size_t cap = 0;
                uint8_t *dst = AMediaCodec_getInputBuffer(d->mc, (size_t)ci, &cap);
                if (size < 0) size = 0;
                if ((size_t)size > cap) {
                    som_log("movie: input of %d bytes does not fit the codec buffer (%zu)", size, cap);
                    size = (int)cap;
                }
                if (dst && size) memcpy(dst, d->in_buf[slot] + off, (size_t)size);
                media_status_t s = AMediaCodec_queueInputBuffer(d->mc, (size_t)ci, 0, (size_t)size, (uint64_t)pts,
                                                                (flags & FLAG_EOS) ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
                if (s != AMEDIA_OK) som_log("movie: queueInputBuffer failed: %d", s);
                d->in_free[d->n_in_free++] = slot;
                dec_signal(d, EV_INPUT);
                did = 1;
            } else if (ci >= 0) {
                AMediaCodec_queueInputBuffer(d->mc, (size_t)ci, 0, 0, 0, 0); /* flushed meanwhile: give it back empty */
            }
        }
        /* drain one output into a free slot */
        int free_slot = -1;
        for (int i = 0; i < NOUT && free_slot < 0; i++)
            if (!d->out_state[i]) free_slot = i;
        if (free_slot >= 0 && !d->eos_out) {
            AMediaCodecBufferInfo info;
            pthread_mutex_unlock(&d->mu);
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(d->mc, &info, d->n_in_q ? 0 : 4000);
            pthread_mutex_lock(&d->mu);
            if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                read_format(d);
                dec_signal(d, EV_FORMAT);
                did = 1;
            } else if (oi >= 0) {
                if (!d->fmt_known) {
                    read_format(d);
                    dec_signal(d, EV_FORMAT);
                }
                int64_t guest_pts = info.presentationTimeUs;
                if (d->side && !side_map_output(d, info.presentationTimeUs, &guest_pts) &&
                    !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
                    AMediaCodec_releaseOutputBuffer(d->mc, (size_t)oi, false); /* decoded to reach a frame: not shown */
                    continue;
                }
                size_t len = 0;
                uint8_t *src = AMediaCodec_getOutputBuffer(d->mc, (size_t)oi, &len);
                size_t need = d->video ? out_bytes(d) : (size_t)(info.size > 0 ? info.size : 0);
                if (info.size <= 0) need = 0;
                if (need > d->out_cap[free_slot]) {
                    free(d->out_buf[free_slot]);
                    d->out_buf[free_slot] = malloc(need);
                    d->out_cap[free_slot] = d->out_buf[free_slot] ? need : 0;
                    if (!d->out_buf[free_slot]) need = 0;
                }
                if (d->frames < 3) {
                    uint64_t sum = 0;
                    size_t span = src && len > (size_t)info.offset ? len - (size_t)info.offset : 0;
                    for (size_t k = 0; src && k < span && k < 1920 * 1080; k += 97) sum += src[info.offset + k];
                    som_log("movie: codec buffer %p len %zu offset %d size %d flags %#x, luma sample mean %llu (stride %d slice %d color %d)",
                            (void *)src, len, info.offset, info.size, info.flags,
                            (unsigned long long)(span ? sum / ((span < 1920 * 1080 ? span : 1920 * 1080) / 97 + 1) : 0),
                            d->src_stride, d->src_slice, d->src_color);
                }
                if (need && src) {
                    if (d->video) convert_video(d, src + info.offset, len - (size_t)info.offset, d->out_buf[free_slot]);
                    else memcpy(d->out_buf[free_slot], src + info.offset, need);
                }
                AMediaCodec_releaseOutputBuffer(d->mc, (size_t)oi, false);
                d->out_size[free_slot] = (int)need;
                d->out_pts[free_slot] = guest_pts;
                d->out_flags[free_slot] = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ? FLAG_EOS : 0;
                if (d->out_flags[free_slot] & FLAG_EOS) d->eos_out = 1;
                d->out_state[free_slot] = 1;
                d->out_ready[d->n_out_ready++] = free_slot;
                if (d->video) {
                    int64_t lag = d->st_last_in_pts - guest_pts;
                    d->st_lag_sum_us += lag;
                    if (lag > d->st_lag_max_us) d->st_lag_max_us = lag;
                    d->st_out++, stats_tick(d);
                }
                if (d->frames++ < 3 || d->frames % 300 == 0 || d->eos_out)
                    som_log("movie: %s output %u pts %lld size %zu%s", type_mime(d->type), d->frames,
                            (long long)info.presentationTimeUs, need, d->eos_out ? " (end of stream)" : "");
                dec_signal(d, EV_OUTPUT);
                did = 1;
            } else if (oi != AMEDIACODEC_INFO_TRY_AGAIN_LATER && oi != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                som_log("movie: dequeueOutputBuffer error %zd", oi);
                d->last_error = ST_ERROR;
                dec_signal(d, EV_ERROR);
                struct timespec ts = {0, 20000000};
                pthread_mutex_unlock(&d->mu);
                nanosleep(&ts, NULL);
                pthread_mutex_lock(&d->mu);
            }
        }
        if (!did && (free_slot < 0 || d->eos_out) && !d->n_in_q) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 4000000;
            if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
            pthread_cond_timedwait(&d->cv, &d->mu, &ts);
        }
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

static void worker_stop(Dec *d) {
    if (!d->worker_on) return;
    pthread_mutex_lock(&d->mu);
    d->stop = 1;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
    pthread_join(d->worker, NULL);
    d->worker_on = 0;
    d->stop = 0;
}

/* all slots back to their initial state (caller holds d->mu) */
static void slots_reset(Dec *d) {
    d->n_in_free = 0;
    for (int i = 0; i < NIN; i++) d->in_free[d->n_in_free++] = i;
    d->n_in_q = 0;
    for (int i = 0; i < NOUT; i++) d->out_state[i] = 0;
    d->n_out_ready = 0;
    d->eos_out = 0;
}

static void side_close(Dec *d) {
    if (d->side) AMediaExtractor_delete(d->side);
    if (d->side_fd > 0) close(d->side_fd);
    d->side = NULL, d->side_fd = 0, d->sq_n = 0;
}

/* The H.264 copy of the movie the game is reading, on the hardware decoder (in place of a software VP9 decoder).
 * Returns 1 with d->mc configured. */
static int side_open(Dec *d, const char *path, const char *name, int64_t want_dur) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    off_t len = lseek(fd, 0, SEEK_END);
    AMediaExtractor *ex = AMediaExtractor_new();
    AMediaFormat *tf = NULL;
    const char *mime = NULL;
    if (ex && AMediaExtractor_setDataSourceFd(ex, fd, 0, len) == AMEDIA_OK)
        for (size_t t = 0; t < AMediaExtractor_getTrackCount(ex) && !tf; t++) {
            AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, t);
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && !strncmp(mime, "video/", 6)) {
                AMediaExtractor_selectTrack(ex, t);
                tf = f;
            } else {
                AMediaFormat_delete(f);
            }
        }
    int64_t dur = 0;
    if (tf && want_dur > 0 && AMediaFormat_getInt64(tf, AMEDIAFORMAT_KEY_DURATION, &dur) &&
        (dur - want_dur > 200000 || want_dur - dur > 200000)) { /* another movie the game also read: not this one */
        AMediaFormat_delete(tf);
        AMediaExtractor_delete(ex);
        close(fd);
        return 0;
    }
    AMediaCodec *mc = tf ? AMediaCodec_createDecoderByType(mime) : NULL;
    if (tf) {
        AMediaFormat_setInt32(tf, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); /* NV12 into byte buffers, as for VP9 */
        /* The copies have no B-frames, but the stream does not say so, and an H.264 decoder then holds frames back for
         * reordering: frames came out ~400 ms (12 frames) after their packet, the game's player (which reads ~400 ms
         * ahead) found them late, dropped two thirds and flushed every second. Low-latency mode outputs each frame as it
         * is decoded. */
        AMediaFormat_setInt32(tf, "low-latency", 1);
    }
    if (!mc || AMediaCodec_configure(mc, tf, NULL, NULL, 0) != AMEDIA_OK) {
        som_log("movie: the H.264 copy of %s did not open (%s): playing the VP9 stream", name, mc ? "configure failed" : "no track or decoder");
        if (mc) AMediaCodec_delete(mc);
        if (tf) AMediaFormat_delete(tf);
        if (ex) AMediaExtractor_delete(ex);
        close(fd);
        return 0;
    }
    char *cn = NULL;
    AMediaCodec_getName(mc, &cn);
    som_log("movie: playing %s from its H.264 copy on %s (the VP9 packets only time it)", name, cn ? cn : "?");
    if (cn) AMediaCodec_releaseName(mc, cn);
    AMediaFormat_delete(tf);
    d->mc = mc, d->side = ex, d->side_fd = fd, d->side_seek = 1, d->sq_n = 0;
    d->src_stride = d->width, d->src_slice = d->height, d->src_color = 21;
    return 1;
}

static void dec_free(Dec *d) {
    worker_stop(d);
    pump_forget(d->events, 4);
    side_close(d);
    if (d->mc) {
        if (d->started) AMediaCodec_stop(d->mc);
        AMediaCodec_delete(d->mc);
    }
    for (int i = 0; i < NIN; i++) free(d->in_buf[i]);
    for (int i = 0; i < NOUT; i++) free(d->out_buf[i]);
    pthread_mutex_destroy(&d->mu);
    pthread_cond_destroy(&d->cv);
    free(d);
}

/* ---- movie::Decoder methods ---- */

#define CALL_LOG(name)                                                                                                 \
    do {                                                                                                               \
        static int n_;                                                                                                 \
        if (n_++ < 4) som_log("movie: Decoder::%s", name);                                                             \
    } while (0)

/* Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int) */
static void dec_ctor(GuestCtx *c) {
    uint64_t self = c->x[0];
    int type = (int)c->x[1], fmt = (int)c->x[2], mode = (int)c->x[3], extra = (int)c->x[4];
    static uint64_t vtable;
    if (!vtable) vtable = module_symbol("_ZTVN5movie7DecoderE") + 0x10;
    *(uint64_t *)(uintptr_t)self = vtable;
    *(uint64_t *)(uintptr_t)(self + 8) = 0; /* no DecoderImpl: every method is ours */
    Dec *d = calloc(1, sizeof *d);
    d->guest = self, d->type = type, d->fmt = fmt, d->mode = mode;
    d->video = type >= TYPE_AVC && type <= TYPE_VP9;
    d->width = 1920, d->height = 1080, d->channels = 2, d->rate = 48000;
    pthread_mutex_init(&d->mu, NULL);
    pthread_cond_init(&d->cv, NULL);
    for (int i = 0; i < NIN; i++) d->in_buf[i] = malloc(IN_CAP);
    slots_reset(d);
    if (!type_mime(type) || mode != 1 || (d->video && fmt != FMT_NV12 && fmt != FMT_RGBA) || (!d->video && fmt != FMT_PCM16))
        d->broken = 1;
    pthread_mutex_lock(&g_dec_lock);
    int placed = 0;
    for (int i = 0; i < MAX_DEC && !placed; i++)
        if (!g_dec[i]) g_dec[i] = d, placed = 1;
    pthread_mutex_unlock(&g_dec_lock);
    if (!placed) d->broken = 1;
    som_log("movie: Decoder(type %d %s, output format %d, mode %d, %d) at 0x%llx%s", type, type_mime(type) ? type_mime(type) : "?",
            fmt, mode, extra, (unsigned long long)self, d->broken ? " -- not supported" : "");
    pump_start(c);
}

static void dec_destroy(GuestCtx *c, int and_delete) {
    uint64_t self = c->x[0];
    Dec *d = NULL;
    pthread_mutex_lock(&g_dec_lock);
    for (int i = 0; i < MAX_DEC; i++)
        if (g_dec[i] && g_dec[i]->guest == self) d = g_dec[i], g_dec[i] = NULL;
    pthread_mutex_unlock(&g_dec_lock);
    if (d) {
        som_log("movie: ~Decoder(%s) after %u outputs", type_mime(d->type) ? type_mime(d->type) : "?", d->frames);
        dec_free(d);
    }
    if (and_delete) {
        uint64_t base = module_symbol("_ZN5movie7Decoder5StartEv") - OFF_DECODER_START;
        rt_call(c, base + OFF_PLT_DELETE, 1, self);
    }
}
static void dec_dtor(GuestCtx *c) { dec_destroy(c, 0); }
static void dec_dtor_delete(GuestCtx *c) { dec_destroy(c, 1); }

/* Configure(MediaData *trackFormat) */
static void dec_configure(GuestCtx *c) {
    Dec *d = dec_of(c);
    uint64_t md = c->x[1];
    CALL_LOG("Configure");
    if (!d || !md) return ret_x(c, ST_BAD_ARG);
    if (d->broken) return ret_x(c, ST_UNSUPPORTED);
    const char *mime = type_mime(d->type);
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    int v;
    int64_t dur = 0;
    if (d->video) {
        if (md_find_i32(c, md, "width", &v) && v > 0) d->width = v;
        if (md_find_i32(c, md, "height", &v) && v > 0) d->height = v;
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, d->width);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, d->height);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); /* COLOR_FormatYUV420SemiPlanar */
        d->src_stride = d->width, d->src_slice = d->height, d->src_color = 21;
    } else {
        if (md_find_i32(c, md, "channel-count", &v) && v > 0) d->channels = v;
        if (md_find_i32(c, md, "sample-rate", &v) && v > 0) d->rate = v;
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, d->channels);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, d->rate);
    }
    if (md_find_i64(c, md, "durationUs", &dur)) AMediaFormat_setInt64(f, AMEDIAFORMAT_KEY_DURATION, dur);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, (int32_t)IN_CAP);
    for (int i = 0; i < 3; i++) {
        char key[8] = "csd-0";
        key[4] = (char)('0' + i);
        GuestBuffer *b = md_find_buf(c, md, key);
        if (b && b->data && b->size > 0) {
            AMediaFormat_setBuffer(f, key, (void *)(uintptr_t)(b->data + (uint64_t)b->offset), (size_t)b->size);
            som_log("movie: %s %s: %d bytes", mime, key, b->size);
        }
    }
    if (d->type == TYPE_VP9 && movies_vp9_is_software()) { /* the H.264 copy of this movie, on the hardware decoder */
        static char sp[8][MOVIES_PATH_N], sn[8][MOVIES_NAME_N];
        int n = movies_recent_copies(sp, sn, 8);
        for (int k = 0; k < n; k++)
            if (side_open(d, sp[k], sn[k], dur)) {
                AMediaFormat_delete(f);
                d->configured = 1;
                return ret_x(c, ST_OK);
            }
        som_log("movie: no converted copy matches this movie (%d recent copies, duration %lld us): software VP9", n, (long long)dur);
    }
    d->mc = AMediaCodec_createDecoderByType(mime);
    media_status_t s = d->mc ? AMediaCodec_configure(d->mc, f, NULL, NULL, 0) : AMEDIA_ERROR_UNSUPPORTED;
    char *name = NULL;
    if (d->mc && AMediaCodec_getName(d->mc, &name) == AMEDIA_OK && name) {
        som_log("movie: %s on %s (%dx%d, %d ch %d Hz, duration %lld us): configure %d", mime, name, d->width, d->height,
                d->channels, d->rate, (long long)dur, s);
        AMediaCodec_releaseName(d->mc, name);
    }
    AMediaFormat_delete(f);
    if (s != AMEDIA_OK) {
        som_log("movie: no MediaCodec decoder for %s (%d)", mime, s);
        if (d->mc) AMediaCodec_delete(d->mc);
        d->mc = NULL;
        d->broken = 1;
        return ret_x(c, ST_UNSUPPORTED);
    }
    d->configured = 1;
    ret_x(c, ST_OK);
}

/* RegisterClientEvents(DecoderEvents *events): four nn::os::EventType pointers */
static void dec_register_events(GuestCtx *c) {
    Dec *d = dec_of(c);
    const uint64_t *ev = arg_p(c, 1);
    CALL_LOG("RegisterClientEvents");
    if (!d) return ret_x(c, ST_BAD_ARG);
    for (int i = 0; i < 4; i++) d->events[i] = ev ? ev[i] : 0;
    ret_x(c, ST_OK);
}

static void dec_start(GuestCtx *c) {
    Dec *d = dec_of(c);
    CALL_LOG("Start");
    if (!d || !d->configured || !d->mc) return ret_x(c, d && d->broken ? ST_UNSUPPORTED : ST_BAD_ARG);
    if (!d->started) {
        media_status_t s = AMediaCodec_start(d->mc);
        if (s != AMEDIA_OK) {
            som_log("movie: MediaCodec start failed: %d", s);
            return ret_x(c, ST_ERROR);
        }
        d->started = 1;
    }
    if (!d->worker_on && pthread_create(&d->worker, NULL, worker_main, d) == 0) d->worker_on = 1;
    dec_signal(d, EV_INPUT);
    ret_x(c, ST_OK);
}

static void dec_stop(GuestCtx *c) {
    Dec *d = dec_of(c);
    CALL_LOG("Stop");
    if (!d) return ret_x(c, ST_BAD_ARG);
    worker_stop(d);
    if (d->mc && d->started) AMediaCodec_stop(d->mc);
    d->started = 0;
    pthread_mutex_lock(&d->mu);
    slots_reset(d);
    pthread_mutex_unlock(&d->mu);
    ret_x(c, ST_OK);
}

static void dec_reset(GuestCtx *c) {
    Dec *d = dec_of(c);
    CALL_LOG("Reset");
    if (!d) return ret_x(c, ST_BAD_ARG);
    worker_stop(d);
    if (d->mc) {
        if (d->started) AMediaCodec_stop(d->mc);
        AMediaCodec_delete(d->mc);
        d->mc = NULL;
    }
    side_close(d);
    d->started = d->configured = d->fmt_known = 0;
    pthread_mutex_lock(&d->mu);
    slots_reset(d);
    pthread_mutex_unlock(&d->mu);
    ret_x(c, ST_OK);
}

static void dec_flush(GuestCtx *c) {
    Dec *d = dec_of(c);
    CALL_LOG("Flush");
    if (!d) return ret_x(c, ST_BAD_ARG);
    int was = d->worker_on;
    worker_stop(d);
    if (d->mc && d->started) AMediaCodec_flush(d->mc);
    pthread_mutex_lock(&d->mu);
    slots_reset(d);
    d->sq_n = 0, d->side_seek = 1; /* the copy is repositioned at the next packet */
    if (d->video) {
        d->st_flush++;
        static int nf;
        if (nf++ < 12) som_log("movie: Flush after packet pts %lld", (long long)d->st_last_in_pts);
    }
    pthread_mutex_unlock(&d->mu);
    if (was && pthread_create(&d->worker, NULL, worker_main, d) == 0) d->worker_on = 1;
    dec_signal(d, EV_INPUT);
    ret_x(c, ST_OK);
}

/* AcquireInputBufferIndex(int *index, int *remaining) */
static void dec_acquire_input(GuestCtx *c) {
    Dec *d = dec_of(c);
    int *idx = arg_p(c, 1), *remaining = arg_p(c, 2);
    if (!d || !idx || !remaining) return ret_x(c, ST_BAD_ARG);
    pthread_mutex_lock(&d->mu);
    *idx = -1;
    *remaining = 0;
    uint32_t st = ST_NO_INPUT;
    if (d->n_in_free > 0) {
        *idx = d->in_free[0];
        memmove(d->in_free, d->in_free + 1, (size_t)(--d->n_in_free) * sizeof d->in_free[0]);
        *remaining = d->n_in_free;
        st = ST_OK;
    }
    pthread_mutex_unlock(&d->mu);
    ret_x(c, st);
}

/* GetInputBuffer(int index, Buffer *buffer) */
static void dec_get_input(GuestCtx *c) {
    Dec *d = dec_of(c);
    int idx = (int)c->x[1];
    GuestBuffer *b = arg_p(c, 2);
    if (!d || !b || idx < 0 || idx >= NIN) return ret_x(c, ST_BAD_ARG);
    b->offset = 0, b->size = 0;
    b->owns = 0;
    b->data = (uint64_t)(uintptr_t)d->in_buf[idx];
    b->capacity = IN_CAP;
    ret_x(c, ST_OK);
}

/* SendInputBufferForDecode(int index, int offset, int size, long pts, uint32 flags) */
static void dec_send_input(GuestCtx *c) {
    Dec *d = dec_of(c);
    int idx = (int)c->x[1], off = (int)c->x[2], size = (int)c->x[3];
    int64_t pts = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    if (!d || idx < 0 || idx >= NIN || off < 0 || size < 0 || (size_t)off + (size_t)size > IN_CAP) return ret_x(c, ST_BAD_ARG);
    static int n;
    if (n++ < 3 || (flags & FLAG_EOS)) som_log("movie: %s input %d bytes pts %lld flags 0x%x", type_mime(d->type), size, (long long)pts, flags);
    if (d->video) movies_playback_tick(); /* the movie converter stays out of the way while a movie plays */
    pthread_mutex_lock(&d->mu);
    if (d->video) {
        d->st_in++;
        if (size > 0 && pts <= d->st_last_in_pts && d->st_in > 1) {
            static int nb;
            if (nb++ < 12) som_log("movie: packet pts %lld after %lld (back in time)", (long long)pts, (long long)d->st_last_in_pts);
            d->st_back++;
        }
        if (size > 0) d->st_last_in_pts = pts;
    }
    if (d->n_in_q < NIN) {
        d->in_q[d->n_in_q].idx = idx, d->in_q[d->n_in_q].off = off, d->in_q[d->n_in_q].size = size;
        d->in_q[d->n_in_q].pts = pts, d->in_q[d->n_in_q].flags = flags;
        d->n_in_q++;
        pthread_cond_broadcast(&d->cv);
    }
    pthread_mutex_unlock(&d->mu);
    ret_x(c, ST_OK);
}

/* AcquireOutputBufferIndex(int *index, long *pts, uint32 *flags, int *remaining) */
static void dec_acquire_output(GuestCtx *c) {
    Dec *d = dec_of(c);
    int *idx = arg_p(c, 1), *remaining = arg_p(c, 4);
    int64_t *pts = arg_p(c, 2);
    uint32_t *flags = arg_p(c, 3);
    if (!d || !idx || !pts || !flags || !remaining) return ret_x(c, ST_BAD_ARG);
    pthread_mutex_lock(&d->mu);
    *idx = -1, *pts = 0, *flags = 0, *remaining = 0;
    uint32_t st = ST_NO_OUTPUT;
    if (d->n_out_ready > 0) {
        int s = d->out_ready[0];
        memmove(d->out_ready, d->out_ready + 1, (size_t)(--d->n_out_ready) * sizeof d->out_ready[0]);
        d->out_state[s] = 2;
        *idx = s, *pts = d->out_pts[s], *flags = d->out_flags[s], *remaining = d->n_out_ready;
        st = ST_OK;
    }
    pthread_mutex_unlock(&d->mu);
    ret_x(c, st);
}

static void dec_release_output(GuestCtx *c) {
    Dec *d = dec_of(c);
    int idx = (int)c->x[1];
    if (!d || idx < 0 || idx >= NOUT) return ret_x(c, ST_BAD_ARG);
    pthread_mutex_lock(&d->mu);
    d->out_state[idx] = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
    ret_x(c, ST_OK);
}

static void fill_format(GuestCtx *c, Dec *d, uint64_t md) {
    md_set_str(c, md, "mime", d->video ? "video/raw" : "audio/raw");
    if (d->video) {
        int w = d->width, h = d->height;
        md_set_i32(c, md, "width", w);
        md_set_i32(c, md, "height", h);
        md_set_i32(c, md, "stride", w);
        md_set_i32(c, md, "slice-height", h);
        md_set_i32(c, md, "color-format", d->fmt == FMT_NV12 ? 21 : 0x7f00a000);
        md_set_rect(c, md, "crop", 0, 0, w - 1, h - 1);
        md_set_i32(c, md, "nv12-uv-offset", w * h);
        md_set_i32(c, md, "nv12-y-stride", w);
        md_set_i32(c, md, "nv12-uv-stride", w);
    } else {
        md_set_i32(c, md, "channel-count", d->channels);
        md_set_i32(c, md, "sample-rate", d->rate);
    }
}

/* GetOutputFormat(MediaData *out) */
static void dec_get_output_format(GuestCtx *c) {
    Dec *d = dec_of(c);
    CALL_LOG("GetOutputFormat");
    if (!d || !c->x[1]) return ret_x(c, ST_BAD_ARG);
    fill_format(c, d, c->x[1]);
    ret_x(c, ST_OK);
}

/* GetOutputBufferFormat(int index, MediaData *out) */
static void dec_get_output_buffer_format(GuestCtx *c) {
    Dec *d = dec_of(c);
    if (!d || !c->x[2]) return ret_x(c, ST_BAD_ARG);
    fill_format(c, d, c->x[2]);
    ret_x(c, ST_OK);
}

/* GetOutputBufferSize(int *size, int *count) */
static void dec_get_output_size(GuestCtx *c) {
    Dec *d = dec_of(c);
    int *size = arg_p(c, 1), *count = arg_p(c, 2);
    CALL_LOG("GetOutputBufferSize");
    if (!d || !size || !count) return ret_x(c, ST_BAD_ARG);
    *count = 1;
    *size = d->video ? (int)out_bytes(d) : d->channels << 12;
    ret_x(c, ST_OK);
}

static uint32_t copy_output(Dec *d, int idx, GuestBuffer *b) {
    if (idx < 0 || idx >= NOUT || !b) return ST_BAD_ARG;
    uint64_t t0 = mono_ns();
    pthread_mutex_lock(&d->mu);
    if (d->video) d->st_taken++;
    int n = d->out_size[idx];
    uint32_t st = ST_OK;
    b->offset = 0, b->size = 0;
    if (d->out_state[idx] != 2) st = ST_BAD_ARG;
    else if (n > 0) {
        if (!b->data || b->capacity < (uint64_t)n) {
            static int warned;
            if (warned++ < 5) som_log("movie: output of %d bytes, client buffer holds %llu", n, (unsigned long long)b->capacity);
            st = ST_BAD_ARG;
        } else {
            memcpy((void *)(uintptr_t)b->data, d->out_buf[idx], (size_t)n);
            b->size = n;
            static int nl;
            if (nl++ < 4) {
                extern const void *nvn_pool_find(uint64_t addr, uint64_t len);
                uint64_t sum = 0;
                for (int k = 0; k < n; k += 101) sum += d->out_buf[idx][k];
                som_log("movie: frame copied to 0x%llx (capacity %llu, %s), byte mean %llu", (unsigned long long)b->data,
                        (unsigned long long)b->capacity, nvn_pool_find(b->data, (uint64_t)n) ? "inside an NVN memory pool" : "plain memory",
                        (unsigned long long)(sum / ((uint64_t)n / 101 + 1)));
            }
        }
    }
    if (d->video) d->st_copy_ns += mono_ns() - t0;
    pthread_mutex_unlock(&d->mu);
    return st;
}

/* GetOutputBuffer(int index, Buffer *out) */
static void dec_get_output(GuestCtx *c) {
    Dec *d = dec_of(c);
    if (!d) return ret_x(c, ST_BAD_ARG);
    ret_x(c, copy_output(d, (int)c->x[1], arg_p(c, 2)));
}

/* GetOutputBuffer(int index, Buffer *out, MediaData *format) */
static void dec_get_output_with_format(GuestCtx *c) {
    Dec *d = dec_of(c);
    if (!d) return ret_x(c, ST_BAD_ARG);
    if (c->x[3]) fill_format(c, d, c->x[3]);
    uint32_t st = c->x[2] ? copy_output(d, (int)c->x[1], arg_p(c, 2)) : ST_OK;
    ret_x(c, st);
}

static void dec_get_last_error(GuestCtx *c) {
    Dec *d = dec_of(c);
    ret_x(c, d ? d->last_error : ST_BAD_ARG);
}

void hooks_movie_register(void) {
    hook_register("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", dec_ctor);
    hook_register("_ZN5movie7DecoderD1Ev", dec_dtor);
    hook_register("_ZN5movie7DecoderD0Ev", dec_dtor_delete);
    hook_register("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", dec_configure);
    hook_register("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", dec_register_events);
    hook_register("_ZN5movie7Decoder5StartEv", dec_start);
    hook_register("_ZN5movie7Decoder4StopEv", dec_stop);
    hook_register("_ZN5movie7Decoder5ResetEv", dec_reset);
    hook_register("_ZN5movie7Decoder5FlushEv", dec_flush);
    hook_register("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", dec_send_input);
    hook_register("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", dec_release_output);
    hook_register("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", dec_get_output_format);
    hook_register("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", dec_acquire_output);
    hook_register("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", dec_get_output_size);
    hook_register("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", dec_get_output);
    hook_register("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", dec_acquire_input);
    hook_register("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", dec_get_input);
    hook_register("_ZN5movie7Decoder12GetLastErrorEv", dec_get_last_error);
    hook_register("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", dec_get_output_with_format);
    hook_register("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", dec_get_output_buffer_format);
}
