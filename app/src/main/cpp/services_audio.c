/* audren:u: the audio renderer service, enough for the SDK to initialise and run its update loop.
 * Voices are accepted and never mixed yet: RequestUpdate answers with a well-formed "everything idle"
 * response. Real mixing and the audio sink come in the audio milestone.
 * Structure layouts follow the public switchbrew audio notes. */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "guest.h"
#include "ipc.h"
#include "sync.h"

/* AudioRendererParameter (0x34 bytes) fields we care about */
typedef struct {
    uint32_t sample_rate, sample_count, mixes, sub_mixes, voices, sinks, effects, perf_frames;
} RenderParams;

typedef struct Renderer {
    RenderParams p;
    uint64_t next_ns;
    uint32_t work_size;
    uint32_t revision;
    uint32_t started;
    uint32_t updates;
    uint32_t ev_w, ev_r;
} Renderer;

static uint64_t work_buffer_size(const RenderParams *p) {
    /* generous: the SDK only checks that the transfer memory it passes is at least this big */
    uint64_t s = 0x4000;
    s += (uint64_t)p->voices * 0x400 + (uint64_t)p->effects * 0x400 + (uint64_t)p->sinks * 0x200;
    s += (uint64_t)p->mixes * 0x400 + (uint64_t)p->sub_mixes * 0x400 + (uint64_t)p->sample_count * 0x80;
    return (s + 0xFFF) & ~0xFFFull;
}

static void load_params(RenderParams *p, const IpcRequest *rq) {
    const uint32_t *a = ipc_arg(rq, 0, 0x20);
    memcpy(p, a, sizeof *p);
}

/* ---- pacing: a clock thread signals the renderer events and releases audout buffers on time ---- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct AudOut;
#define MAX_PACED 8
static struct Renderer *g_renderers[MAX_PACED];
static struct AudOut *g_audouts[MAX_PACED];
static pthread_mutex_t g_pace_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_pace_once = PTHREAD_ONCE_INIT;
static void *pace_thread(void *arg);

static void pace_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, pace_thread, NULL) == 0) pthread_detach(t);
}

static void renderer_close(void *st) { (void)st; /* stays registered with the pacing thread */ }

static void renderer_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    Renderer *r = self->state;
    switch (rq->cmd) {
    case 0: { uint32_t v = r->p.sample_rate; IPC_OUT(rs, v); return; }  /* GetSampleRate */
    case 1: { uint32_t v = r->p.sample_count; IPC_OUT(rs, v); return; } /* GetSampleCount */
    case 2: { uint32_t v = r->p.mixes; IPC_OUT(rs, v); return; }        /* GetMixBufferCount */
    case 3: { uint32_t v = r->started ? 0 : 1; IPC_OUT(rs, v); return; } /* GetState (0 = started) */
    case 4: case 10: {                                                  /* RequestUpdate / RequestUpdateAuto */
        const IpcBuffer *in = rq->na ? &rq->a[0] : rq->nx ? &rq->x[0] : NULL;
        const IpcBuffer *out = rq->nb ? &rq->b[0] : rq->nc ? &rq->c[0] : NULL;
        if (!in || !out) som_stop("audren: RequestUpdate buffers (a%u x%u b%u c%u)", rq->na, rq->nx, rq->nb, rq->nc);
        const uint32_t *ih = ipc_guest(in->addr, in->size);
        if (in->size < 0x40) som_stop("audren: update input too small (%llu)", (unsigned long long)in->size);
        uint32_t pools = ih[2] / 0x20, voices = ih[3] / 0x170, effects = ih[5] / 0xC0, sinks = ih[7] / 0x140;
        uint32_t behavior = 0xB0, pool_sz = pools * 0x10, voice_sz = voices * 0x10, fx_sz = effects * 0x10,
                 sink_sz = sinks * 0x20, render_sz = 0;
        uint32_t total = 0x40 + behavior + pool_sz + voice_sz + fx_sz + sink_sz + render_sz;
        if (total > out->size) som_stop("audren: update output needs 0x%x, buffer is 0x%llx", total, (unsigned long long)out->size);
        uint8_t *o = ipc_guest(out->addr, out->size);
        memset(o, 0, total);
        uint32_t *oh = (uint32_t *)o;
        oh[0] = ih[0]; /* revision */
        oh[1] = behavior;
        oh[2] = pool_sz;
        oh[3] = voice_sz;
        oh[4] = 0;
        oh[5] = fx_sz;
        oh[6] = 0;
        oh[7] = sink_sz;
        oh[8] = 0;
        oh[10] = render_sz;
        oh[0x10] = total;
        if (!r->updates++) som_log("audren: first update, rev 0x%x pools %u voices %u effects %u sinks %u, out 0x%x", ih[0], pools, voices, effects, sinks, total);
        return;
    }
    case 5: r->started = 1; return; /* Start */
    case 6: r->started = 0; return; /* Stop */
    case 7: {                       /* QuerySystemEvent -> copy handle */
        rs->copy[rs->ncopy++] = r->ev_r;
        return;
    }
    case 8: return;                                              /* SetRenderingTimeLimit */
    case 9: { uint32_t v = 80; IPC_OUT(rs, v); return; }         /* GetRenderingTimeLimit */
    case 11: return;                                             /* ExecuteAudioRendererRendering */
    default: ipc_unimplemented(self, rq);
    }
}

static void device_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 6: { /* ListAudioDeviceName(Auto) -> s32 count; names (0x80 each) */
        const IpcBuffer *b = rq->nb ? &rq->b[0] : rq->nc ? &rq->c[0] : NULL;
        if (b && b->size >= 0x80) {
            char *o = ipc_guest(b->addr, 0x80);
            memset(o, 0, 0x80);
            strcpy(o, "AudioStereoJackOutput");
        }
        int32_t n = 1;
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 7: return; /* SetAudioDeviceOutputVolume(Auto) */
    case 2: case 8: { float v = 1.0f; IPC_OUT(rs, v); return; } /* GetAudioDeviceOutputVolume(Auto) */
    case 3: case 10: { /* GetActiveAudioDeviceName(Auto) */
        const IpcBuffer *b = rq->nb ? &rq->b[0] : rq->nc ? &rq->c[0] : NULL;
        if (b && b->size >= 0x80) {
            char *o = ipc_guest(b->addr, 0x80);
            memset(o, 0, 0x80);
            strcpy(o, "AudioStereoJackOutput");
        }
        return;
    }
    case 4: case 11: case 12: { /* QueryAudioDeviceSystemEvent / Input / Output -> copy handle */
        uint32_t w, r;
        kevent_create_handles(&w, &r);
        rs->copy[rs->ncopy++] = r;
        return;
    }
    case 5: { uint32_t v = 2; IPC_OUT(rs, v); return; } /* GetActiveChannelCount */
    case 13: { float v = 1.0f; IPC_OUT(rs, v); return; } /* GetAudioSystemMasterVolumeSetting */
    default: ipc_unimplemented(self, rq);
    }
}

static void audren_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { /* OpenAudioRenderer(params 0x34, u64 work size, u64 aruid; copy: transfer memory, process) -> IAudioRenderer */
        Renderer *r = calloc(1, sizeof *r);
        load_params(&r->p, rq);
        r->work_size = (uint32_t)IPC_ARG(rq, uint64_t, 0x38);
        som_log("audren: OpenAudioRenderer rate %u samples %u mixes %u voices %u sinks %u effects %u", r->p.sample_rate,
                r->p.sample_count, r->p.mixes, r->p.voices, r->p.sinks, r->p.effects);
        pthread_once(&g_pace_once, pace_start);
        kevent_create_handles(&r->ev_w, &r->ev_r);
        pthread_mutex_lock(&g_pace_mu);
        for (int i = 0; i < MAX_PACED; i++)
            if (!g_renderers[i]) { g_renderers[i] = r; break; }
        pthread_mutex_unlock(&g_pace_mu);
        ipc_return_object(rq, rs, "audren:renderer", renderer_handler, r, renderer_close);
        return;
    }
    case 1: { /* GetWorkBufferSize(params 0x34) -> u64 */
        RenderParams p;
        load_params(&p, rq);
        uint64_t sz = work_buffer_size(&p);
        som_log("audren: GetWorkBufferSize -> 0x%llx", (unsigned long long)sz);
        IPC_OUT(rs, sz);
        return;
    }
    case 2: case 4: /* GetAudioDeviceService(aruid) / WithRevisionInfo(revision, aruid) -> IAudioDevice */
        ipc_return_object(rq, rs, "audren:device", device_handler, NULL, NULL);
        return;
    case 3: /* OpenAudioRendererForManualExecution: not used by games */
    default: ipc_unimplemented(self, rq);
    }
}

/* ---- audout:u ---- */

#define AO_MAX 256

/* ---- the host sink: each audout's PCM16 goes into a ring that an AAudio stream plays. The service keeps pacing the
 * game by its own clock (buffers are released when they would have finished playing); the ring absorbs the drift
 * between that clock and the device's, dropping the oldest audio past 250 ms and playing silence when it runs dry. */
#include <aaudio/AAudio.h>
#include <stdatomic.h>

#define SINK_FRAMES (48000 / 2) /* ring capacity: half a second */
typedef struct Sink {
    AAudioStream *st;
    uint32_t rate, channels;
    int16_t *ring;          /* SINK_FRAMES * channels samples */
    _Atomic uint64_t wr, rd; /* frame counters */
    int primed;
    uint64_t underruns;
} Sink;

static aaudio_data_callback_result_t sink_cb(AAudioStream *st, void *user, void *data, int32_t frames) {
    (void)st;
    Sink *s = user;
    int16_t *out = data;
    uint64_t wr = atomic_load_explicit(&s->wr, memory_order_acquire), rd = atomic_load_explicit(&s->rd, memory_order_relaxed);
    uint64_t have = wr - rd;
    if (!s->primed) { /* start once 40 ms are queued, so playback does not open on an underrun */
        if (have < s->rate / 25) {
            memset(out, 0, (size_t)frames * s->channels * 2);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        s->primed = 1;
    }
    uint64_t n = have < (uint64_t)frames ? have : (uint64_t)frames;
    for (uint64_t i = 0; i < n; i++)
        memcpy(out + i * s->channels, s->ring + ((rd + i) % SINK_FRAMES) * s->channels, s->channels * 2);
    if (n < (uint64_t)frames) {
        memset(out + n * s->channels, 0, (size_t)(frames - n) * s->channels * 2);
        s->underruns++;
        s->primed = 0;
    }
    atomic_store_explicit(&s->rd, rd + n, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static Sink *sink_open(uint32_t rate, uint32_t channels) {
    Sink *s = calloc(1, sizeof *s);
    s->rate = rate, s->channels = channels;
    s->ring = calloc((size_t)SINK_FRAMES * channels, 2);
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return s;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)rate);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)channels);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, sink_cb, s);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s->st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        som_log("audout: AAudio stream failed to open: %s", AAudio_convertResultToText(r));
        s->st = NULL;
        return s;
    }
    r = AAudioStream_requestStart(s->st);
    som_log("audout: AAudio stream %u Hz %u ch, burst %d frames, start %s", AAudioStream_getSampleRate(s->st),
            (unsigned)AAudioStream_getChannelCount(s->st), AAudioStream_getFramesPerBurst(s->st), AAudio_convertResultToText(r));
    return s;
}

static void sink_write(Sink *s, const int16_t *pcm, uint64_t frames) {
    if (!s || !s->st || !pcm) return;
    uint64_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed), rd = atomic_load_explicit(&s->rd, memory_order_acquire);
    uint64_t limit = s->rate / 4; /* keep at most 250 ms queued: drop the oldest */
    if (frames > limit) pcm += (frames - limit) * s->channels, frames = limit;
    if (wr - rd + frames > limit) {
        uint64_t drop = wr - rd + frames - limit;
        uint64_t want = rd + drop; /* move the reader past the oldest frames (benign race with the callback) */
        atomic_compare_exchange_strong(&s->rd, &rd, want);
    }
    for (uint64_t i = 0; i < frames; i++) memcpy(s->ring + ((wr + i) % SINK_FRAMES) * s->channels, pcm + i * s->channels, s->channels * 2);
    atomic_store_explicit(&s->wr, wr + frames, memory_order_release);
}

typedef struct AudOut {
    Sink *sink;
    uint32_t rate, channels, started;
    uint32_t ev_w, ev_r;
    struct { uint64_t tag, done_ns, samples; } q[AO_MAX];
    int head, count; /* queued, in order */
    uint64_t n_app, n_rel, n_drop, n_polls, last_stat_ns;
    uint64_t last_done_ns, played;
} AudOut;

/* "Auto" commands send a B and a C descriptor and leave the unused one empty: pick the one with a size. (Taking an
 * empty b[0] released nothing, and the audio thread busy-polled while holding a lock the main thread needs.) */
static const IpcBuffer *out_buf(const IpcRequest *rq) {
    if (rq->nb && rq->b[0].size) return &rq->b[0];
    if (rq->nc && rq->c[0].size) return &rq->c[0];
    return rq->nb ? &rq->b[0] : rq->nc ? &rq->c[0] : NULL;
}

static void audout_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    AudOut *a = self->state;
    switch (rq->cmd) {
    case 0: { uint32_t v = a->started ? 0 : 1; IPC_OUT(rs, v); return; } /* GetAudioOutState: 0 started, 1 stopped */
    case 1: a->started = 1; return;                                    /* Start */
    case 2: a->started = 0; return;                                    /* Stop */
    case 3: case 7: {                                                  /* AppendAudioOutBuffer(u64 tag; buffer) */
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        const IpcBuffer *b = rq->na && rq->a[0].size >= 0x28 ? &rq->a[0] : rq->nx ? &rq->x[0] : NULL;
        if (!b || b->size < 0x28) som_stop("audout: AppendAudioOutBuffer buffer (a%u x%u b%u c%u w%u, size %llu)", rq->na, rq->nx, rq->nb, rq->nc, rq->nw, b ? (unsigned long long)b->size : 0ull);
        const uint64_t *ab = ipc_guest(b->addr, 0x28); /* AudioOutBuffer: next, data, capacity, size, offset */
        uint64_t samples = ab[3] / (a->channels * 2u);
        if (ab[1] && samples && a->started) sink_write(a->sink, (const int16_t *)(uintptr_t)(ab[1] + ab[4]), samples);
        pthread_mutex_lock(&g_pace_mu);
        if (a->count < AO_MAX) {
            uint64_t t = now_ns();
            uint64_t start = a->last_done_ns > t ? a->last_done_ns : t;
            a->last_done_ns = start + samples * 1000000000ull / a->rate;
            int i = (a->head + a->count++) % AO_MAX;
            a->q[i].tag = tag;
            a->q[i].done_ns = a->last_done_ns;
            a->q[i].samples = samples;
            a->n_app++;
        } else if (!a->n_drop++) {
            som_log("audout: queue full (%d), buffer 0x%llx dropped", AO_MAX, (unsigned long long)tag);
        }
        pthread_mutex_unlock(&g_pace_mu);
        return;
    }
    case 4: rs->copy[rs->ncopy++] = a->ev_r; return; /* RegisterBufferEvent */
    case 5: case 8: {                                /* GetReleasedAudioOutBuffers -> u32 count; tags */
        const IpcBuffer *b = out_buf(rq);
        uint64_t *out = b ? ipc_guest(b->addr, b->size) : NULL;
        uint32_t cap = b ? (uint32_t)(b->size / 8) : 0, n = 0;
        /* The service clears the unused entries and the SDK's GetReleasedAudioOutBuffer reads the first entry: a stale
         * tag from an earlier call reads as one more released buffer (CRI then counted -40 million queued buffers and
         * rendered forever with its lock held, freezing the main thread). */
        if (out && cap) memset(out, 0, (size_t)cap * 8);
        pthread_mutex_lock(&g_pace_mu);
        uint64_t t = now_ns();
        while (a->count && a->q[a->head].done_ns <= t && n < cap) {
            out[n++] = a->q[a->head].tag;
            a->played += a->q[a->head].samples;
            a->head = (a->head + 1) % AO_MAX;
            a->count--;
            a->n_rel++;
        }
        a->n_polls++;
        if (t - a->last_stat_ns > 2000000000ull) {
            a->last_stat_ns = t;
            som_log("audout %p: appended %llu released %llu queued %d dropped %llu polls %llu started %u next due in %lld us; "
                    "sink %llu ms queued, %llu underruns",
                    (void *)a, (unsigned long long)a->n_app, (unsigned long long)a->n_rel, a->count,
                    (unsigned long long)a->n_drop, (unsigned long long)a->n_polls, a->started,
                    a->count ? (long long)(a->q[a->head].done_ns - t) / 1000 : -1ll,
                    a->sink ? (unsigned long long)((a->sink->wr - a->sink->rd) * 1000 / a->rate) : 0ull,
                    a->sink ? (unsigned long long)a->sink->underruns : 0ull);
        }
        pthread_mutex_unlock(&g_pace_mu);
        static int logged;
        if (n && logged < 3) {
            logged++;
            som_log("audout: released %u buffers (cap %u)", n, cap);
        }
        if (!cap && a->count) {
            static int warned;
            if (!warned++) som_log("audout: GetReleasedAudioOutBuffers with no output buffer (b%u c%u)", rq->nb, rq->nc);
        }
        IPC_OUT(rs, n);
        return;
    }
    case 6: { /* ContainsAudioOutBuffer(tag) */
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        uint8_t found = 0;
        pthread_mutex_lock(&g_pace_mu);
        for (int i = 0; i < a->count; i++)
            if (a->q[(a->head + i) % AO_MAX].tag == tag) found = 1;
        pthread_mutex_unlock(&g_pace_mu);
        IPC_OUT(rs, found);
        return;
    }
    case 9: { uint32_t v = (uint32_t)a->count; IPC_OUT(rs, v); return; } /* GetAudioOutBufferCount */
    case 10: { uint64_t v = a->played; IPC_OUT(rs, v); return; }         /* GetAudioOutPlayedSampleCount */
    case 11: {                                                           /* FlushAudioOutBuffers -> bool */
        pthread_mutex_lock(&g_pace_mu);
        for (int i = 0; i < a->count; i++) a->q[(a->head + i) % AO_MAX].done_ns = 0;
        pthread_mutex_unlock(&g_pace_mu);
        uint8_t r = 1;
        IPC_OUT(rs, r);
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

static void put_name(const IpcBuffer *b, const char *name, size_t len) {
    if (b && b->size >= len) {
        char *o = ipc_guest(b->addr, len);
        memset(o, 0, len);
        strcpy(o, name);
    }
}

static void audout_service(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: { /* ListAudioOuts(Auto) -> u32 count; names (0x20 each) */
        put_name(out_buf(rq), "DeviceOut", 0x20);
        uint32_t n = 1;
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 3: { /* OpenAudioOut(u32 rate, u16 channels, u64 aruid; name in, name out) */
        uint32_t rate = IPC_ARG(rq, uint32_t, 0);
        uint16_t ch = IPC_ARG(rq, uint16_t, 4);
        AudOut *a = calloc(1, sizeof *a);
        a->rate = rate ? rate : 48000;
        a->channels = ch ? ch : 2;
        kevent_create_handles(&a->ev_w, &a->ev_r);
        a->sink = sink_open(a->rate, a->channels);
        put_name(out_buf(rq), "DeviceOut", 0x20);
        pthread_once(&g_pace_once, pace_start);
        pthread_mutex_lock(&g_pace_mu);
        for (int i = 0; i < MAX_PACED; i++)
            if (!g_audouts[i]) { g_audouts[i] = a; break; }
        pthread_mutex_unlock(&g_pace_mu);
        struct { uint32_t rate, channels, format, state; } out = {a->rate, a->channels, 2 /* PCM16 */, 1};
        som_log("audout: opened rate %u channels %u", a->rate, a->channels);
        IPC_OUT(rs, out);
        ipc_return_object(rq, rs, "audout:out", audout_handler, a, NULL);
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

/* ---- hwopus: sizes only for now; decoding needs libopus ---- */

static void hwopus_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: case 3: case 5: case 7: { /* GetWorkBufferSize[ForMultiStream][Ex] -> u32 */
        uint32_t sz = 0x8000;
        IPC_OUT(rs, sz);
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

static void *pace_thread(void *arg) {
    (void)arg;
    struct timespec ts = {0, 1000000};
    for (;;) {
        nanosleep(&ts, NULL);
        uint64_t t = now_ns();
        pthread_mutex_lock(&g_pace_mu);
        for (int i = 0; i < MAX_PACED; i++) {
            Renderer *r = g_renderers[i];
            if (r) {
                uint64_t period = (uint64_t)r->p.sample_count * 1000000000ull / (r->p.sample_rate ? r->p.sample_rate : 48000);
                if (!r->next_ns) r->next_ns = t + period;
                if (r->started && t >= r->next_ns) {
                    KEvent *e = kobj_get(r->ev_w, KO_EVENT_W);
                    if (e) kevent_signal(e);
                    r->next_ns += period;
                    if (r->next_ns < t) r->next_ns = t + period;
                }
            }
            AudOut *a = g_audouts[i];
            if (a && a->count && a->q[a->head].done_ns <= t) {
                KEvent *e = kobj_get(a->ev_w, KO_EVENT_W);
                if (e) kevent_signal(e);
            }
        }
        pthread_mutex_unlock(&g_pace_mu);
    }
    return NULL;
}

void services_audio_register(void) {
    ipc_register_service("audren:u", audren_handler);
    ipc_register_service("audout:u", audout_service);
    ipc_register_service("hwopus", hwopus_handler);
}

/* Android background pause: stop the AAudio streams (and drop what was queued, so resuming does not replay stale
 * audio). The game itself is parked by som_guest_pause_all, so nothing new is written meanwhile. */
void som_audio_pause_all(int on) {
    pthread_mutex_lock(&g_pace_mu);
    for (int i = 0; i < MAX_PACED; i++) {
        AudOut *a = g_audouts[i];
        if (!a || !a->sink || !a->sink->st) continue;
        Sink *s = a->sink;
        if (on) {
            AAudioStream_requestPause(s->st);
        } else {
            atomic_store(&s->rd, atomic_load(&s->wr));
            s->primed = 0;
            AAudioStream_requestStart(s->st);
        }
    }
    pthread_mutex_unlock(&g_pace_mu);
    som_log("audio %s", on ? "paused" : "resumed");
}
