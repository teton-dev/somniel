/* audren:u: the audio renderer service, enough for the SDK to initialise and run its update loop.
 * Voices are accepted and never mixed yet: RequestUpdate answers with a well-formed "everything idle"
 * response. Real mixing and the audio sink come in the audio milestone.
 * Structure layouts follow the public switchbrew audio notes. */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
 * between that clock and the device's, dropping the oldest audio past 250 ms and playing silence when it runs dry.
 *
 * Robustness (a user on a Snapdragon 8 Gen 1 phone had no sound and we cannot test there):
 *  - the stream is opened through a fallback chain (low latency -> plain -> stereo -> float) and a configuration is
 *    only taken when the device gives back exactly the rate, channel count and format asked for; the last step takes
 *    whatever the device offers and the callback converts to it;
 *  - an error callback (DISCONNECTED: route change, audio server restart, MMAP stream stolen) reopens the stream on a
 *    helper thread instead of leaving it dead for the rest of the session;
 *  - the stat line reports callbacks, state, sharing/performance mode, xruns and reopens so a user's log tells which
 *    case happened. */
#include <aaudio/AAudio.h>
#include <stdatomic.h>

#define SINK_FRAMES (48000 / 2) /* ring capacity: half a second */
typedef struct Sink {
    pthread_mutex_t mu;     /* stream open/close/start/pause */
    AAudioStream *st;
    uint32_t rate, channels; /* what the game writes (ring layout) */
    uint32_t out_ch;         /* what the stream takes */
    uint32_t prime_frames;   /* queued frames needed before playback (re)starts */
    int out_float;
    int16_t *ring;          /* SINK_FRAMES * channels samples */
    _Atomic uint64_t wr, rd; /* frame counters */
    int primed;
    uint64_t underruns;
    _Atomic uint64_t callbacks;
    _Atomic int reopening;
    uint32_t reopens, open_fails;
    int last_error;
    uint64_t wd_cb, wd_wr; /* watchdog: callbacks / frames written at the previous stat line */
    int wd_stuck;          /* stat lines in a row where the game wrote audio but the device asked for none */
} Sink;

static _Atomic int g_audio_paused;

/* ---- audio failure report: once per run, copy somniel.log to Download/Somniel_log (LogExporter.java) and show a
 * toast, so a user on a device we cannot test can send us the log without adb. ---- */
#include <jni.h>
void *SDL_GetAndroidJNIEnv(void);
void *SDL_GetAndroidActivity(void);

static _Atomic int g_fail_reported;
static char g_fail_reason[256];

static void *fail_report_thread(void *arg) {
    (void)arg;
    sleep(3); /* let the next few log lines (stats, retries) land in the copy */
    char path[600];
    snprintf(path, sizeof path, "%s/somniel.log", som_log_dir());
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    int ok = 0;
    if (env && act) {
        /* app classes are not visible to FindClass on a native thread: go through the activity's class loader */
        jclass ak = (*env)->GetObjectClass(env, act);
        jmethodID gcl = (*env)->GetMethodID(env, ak, "getClassLoader", "()Ljava/lang/ClassLoader;");
        jobject loader = gcl ? (*env)->CallObjectMethod(env, act, gcl) : NULL;
        jclass lk = loader ? (*env)->GetObjectClass(env, loader) : NULL;
        jmethodID lc = lk ? (*env)->GetMethodID(env, lk, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : NULL;
        jstring cn = (*env)->NewStringUTF(env, "dev.teton.somniel.LogExporter");
        jclass ek = lc ? (jclass)(*env)->CallObjectMethod(env, loader, lc, cn) : NULL;
        if ((*env)->ExceptionCheck(env)) ek = NULL;
        jmethodID m = ek ? (*env)->GetStaticMethodID(env, ek, "saveAudioLog", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;)V") : NULL;
        if (m) {
            jstring jp = (*env)->NewStringUTF(env, path), jr = (*env)->NewStringUTF(env, g_fail_reason);
            (*env)->CallStaticVoidMethod(env, ek, m, act, jp, jr);
            ok = !(*env)->ExceptionCheck(env);
            (*env)->DeleteLocalRef(env, jp);
            (*env)->DeleteLocalRef(env, jr);
        }
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
        (*env)->DeleteLocalRef(env, cn);
        if (ek) (*env)->DeleteLocalRef(env, ek);
        if (lk) (*env)->DeleteLocalRef(env, lk);
        if (loader) (*env)->DeleteLocalRef(env, loader);
        (*env)->DeleteLocalRef(env, ak);
        (*env)->DeleteLocalRef(env, act);
    }
    som_log("audio: failure log export %s", ok ? "handed to LogExporter" : "FAILED (no JNI/activity/class)");
    return NULL;
}

static void audio_fail_report(const char *fmt, ...) {
    if (atomic_exchange(&g_fail_reported, 1)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fail_reason, sizeof g_fail_reason, fmt, ap);
    va_end(ap);
    som_log("audio: FAILURE: %s (saving the log to Download/Somniel_log)", g_fail_reason);
    pthread_t t;
    if (pthread_create(&t, NULL, fail_report_thread, NULL) == 0) pthread_detach(t);
}

/* one output frame from one ring frame: copy, downmix 5.1 (FL FR FC LFE BL BR) / anything to stereo or mono, or pad */
static inline void mix_frame(const Sink *s, const int16_t *in, float *o) {
    uint32_t ic = s->channels, oc = s->out_ch;
    if (ic == oc) {
        for (uint32_t c = 0; c < oc; c++) o[c] = in[c];
    } else if (oc == 2 && ic == 6) {
        o[0] = in[0] + 0.7071f * (in[2] + in[4]);
        o[1] = in[1] + 0.7071f * (in[2] + in[5]);
    } else if (oc == 1) {
        float m = ic >= 2 ? 0.5f * (in[0] + in[1]) : in[0];
        o[0] = m;
    } else if (ic == 1) {
        for (uint32_t c = 0; c < oc; c++) o[c] = c < 2 ? in[0] : 0;
    } else {
        for (uint32_t c = 0; c < oc; c++) o[c] = c < ic ? in[c] : 0;
    }
}

static aaudio_data_callback_result_t sink_cb(AAudioStream *st, void *user, void *data, int32_t frames) {
    (void)st;
    Sink *s = user;
    atomic_fetch_add_explicit(&s->callbacks, 1, memory_order_relaxed);
    size_t out_bytes = (size_t)frames * s->out_ch * (s->out_float ? 4 : 2);
    uint64_t wr = atomic_load_explicit(&s->wr, memory_order_acquire), rd = atomic_load_explicit(&s->rd, memory_order_relaxed);
    uint64_t have = wr - rd;
    if (!s->primed) { /* start once 40 ms (or a burst + 10 ms) are queued, so playback does not open on an underrun */
        if (have < s->prime_frames) {
            memset(data, 0, out_bytes);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        s->primed = 1;
    }
    uint64_t n = have < (uint64_t)frames ? have : (uint64_t)frames;
    if (!s->out_float && s->out_ch == s->channels) { /* the common case: straight copy */
        int16_t *out = data;
        for (uint64_t i = 0; i < n; i++)
            memcpy(out + i * s->channels, s->ring + ((rd + i) % SINK_FRAMES) * s->channels, s->channels * 2);
    } else {
        float f[8];
        for (uint64_t i = 0; i < n; i++) {
            mix_frame(s, s->ring + ((rd + i) % SINK_FRAMES) * s->channels, f);
            for (uint32_t c = 0; c < s->out_ch; c++) {
                float v = f[c];
                if (s->out_float) {
                    ((float *)data)[i * s->out_ch + c] = v * (1.0f / 32768.0f);
                } else {
                    if (v > 32767.f) v = 32767.f;
                    if (v < -32768.f) v = -32768.f;
                    ((int16_t *)data)[i * s->out_ch + c] = (int16_t)v;
                }
            }
        }
    }
    if (n < (uint64_t)frames) {
        size_t fb = s->out_ch * (s->out_float ? 4 : 2);
        memset((char *)data + n * fb, 0, (size_t)(frames - n) * fb);
        s->underruns++;
        s->primed = 0;
    }
    atomic_store_explicit(&s->rd, rd + n, memory_order_release);
    extern int movies_audio_muted(void); /* movies.c: Skip intro silences the intro until it is skipped */
    if (movies_audio_muted()) memset(data, 0, out_bytes);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void sink_err_cb(AAudioStream *st, void *user, aaudio_result_t err);

static const char *perf_name(int m) {
    return m == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "low-latency" : m == AAUDIO_PERFORMANCE_MODE_POWER_SAVING ? "power-saving" : "none";
}

/* Open (not start) a stream through the fallback chain. Called with s->mu held. */
static void sink_open_stream(Sink *s) {
    static const struct { int perf, ch2, fl; } chain[] = {
        {AAUDIO_PERFORMANCE_MODE_LOW_LATENCY, 0, 0},
        {AAUDIO_PERFORMANCE_MODE_NONE, 0, 0},
        {AAUDIO_PERFORMANCE_MODE_NONE, 1, 0},
        {AAUDIO_PERFORMANCE_MODE_NONE, 1, 1},
    };
    const int n = (int)(sizeof chain / sizeof chain[0]);
    for (int i = 0; i < n; i++) {
        int last = i == n - 1;
        uint32_t want_ch = chain[i].ch2 && s->channels > 2 ? 2 : s->channels;
        if (chain[i].ch2 && s->channels <= 2 && !chain[i].fl) continue; /* same as the step before */
        aaudio_format_t want_fmt = chain[i].fl ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16;
        AAudioStreamBuilder *b = NULL;
        aaudio_result_t r = AAudio_createStreamBuilder(&b);
        if (r != AAUDIO_OK) {
            som_log("audout: AAudio_createStreamBuilder failed: %s", AAudio_convertResultToText(r));
            s->open_fails++;
            return;
        }
        AAudioStreamBuilder_setFormat(b, want_fmt);
        AAudioStreamBuilder_setSampleRate(b, (int32_t)s->rate);
        AAudioStreamBuilder_setChannelCount(b, (int32_t)want_ch);
        AAudioStreamBuilder_setPerformanceMode(b, chain[i].perf);
        AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setDataCallback(b, sink_cb, s);
        AAudioStreamBuilder_setErrorCallback(b, sink_err_cb, s);
        AAudioStream *st = NULL;
        r = AAudioStreamBuilder_openStream(b, &st);
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK) {
            som_log("audout: AAudio open #%d (%s, %u ch, %s) failed: %s", i, perf_name(chain[i].perf), want_ch,
                    chain[i].fl ? "float" : "i16", AAudio_convertResultToText(r));
            s->open_fails++;
            continue;
        }
        int32_t got_rate = AAudioStream_getSampleRate(st), got_ch = AAudioStream_getChannelCount(st);
        aaudio_format_t got_fmt = AAudioStream_getFormat(st);
        int exact = got_rate == (int32_t)s->rate && got_ch == (int32_t)want_ch && got_fmt == want_fmt;
        int usable = got_ch >= 1 && got_ch <= 8 && (got_fmt == AAUDIO_FORMAT_PCM_I16 || got_fmt == AAUDIO_FORMAT_PCM_FLOAT);
        som_log("audout: AAudio open #%d asked %u Hz %u ch %s %s -> got %d Hz %d ch fmt %d, %s, %s sharing, burst %d, "
                "buffer %d/%d frames, device %d%s",
                i, s->rate, want_ch, chain[i].fl ? "float" : "i16", perf_name(chain[i].perf), got_rate, got_ch, (int)got_fmt,
                perf_name(AAudioStream_getPerformanceMode(st)),
                AAudioStream_getSharingMode(st) == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared",
                AAudioStream_getFramesPerBurst(st), AAudioStream_getBufferSizeInFrames(st),
                AAudioStream_getBufferCapacityInFrames(st), AAudioStream_getDeviceId(st),
                exact ? "" : usable && last ? " (mismatch, taking it anyway)" : " (mismatch, trying next)");
        if (!exact && !(usable && last)) {
            AAudioStream_close(st);
            continue;
        }
        if (got_rate != (int32_t)s->rate) som_log("audout: WARNING stream rate %d != game rate %u: pitch will be off", got_rate, s->rate);
        s->out_ch = (uint32_t)got_ch;
        s->out_float = got_fmt == AAUDIO_FORMAT_PCM_FLOAT;
        /* prime with at least one burst + 10 ms: non-low-latency streams ask for ~40 ms at a time */
        int32_t burst = AAudioStream_getFramesPerBurst(st);
        s->prime_frames = s->rate / 25;
        if (burst > 0 && (uint32_t)burst + s->rate / 100 > s->prime_frames) s->prime_frames = (uint32_t)burst + s->rate / 100;
        s->st = st;
        return;
    }
    som_log("audout: no AAudio configuration worked: NO SOUND");
}

static void sink_start_locked(Sink *s) {
    if (!s->st || atomic_load(&g_audio_paused)) return;
    s->primed = 0;
    aaudio_result_t r = AAudioStream_requestStart(s->st);
    som_log("audout: AAudio start %s", AAudio_convertResultToText(r));
}

/* AAudio forbids closing/reopening from its own error callback: do it on a helper thread. */
static void *sink_reopen_thread(void *arg) {
    Sink *s = arg;
    struct timespec ts = {0, 50000000}; /* let the audio server settle (route change, restart) */
    for (int attempt = 0; attempt < 20; attempt++) {
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&s->mu);
        if (s->st) {
            AAudioStream_close(s->st);
            s->st = NULL;
        }
        sink_open_stream(s);
        sink_start_locked(s);
        int ok = s->st != NULL;
        s->reopens++;
        pthread_mutex_unlock(&s->mu);
        if (ok) break;
        ts.tv_sec = 1, ts.tv_nsec = 0; /* keep trying once a second for a while */
        if (attempt == 19) audio_fail_report("the stream was lost (error %d) and could not be reopened after 20 tries", s->last_error);
    }
    atomic_store(&s->reopening, 0);
    return NULL;
}

static void sink_err_cb(AAudioStream *st, void *user, aaudio_result_t err) {
    (void)st;
    Sink *s = user;
    s->last_error = err;
    som_log("audout: AAudio stream error %s: reopening", AAudio_convertResultToText(err));
    if (atomic_exchange(&s->reopening, 1)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, sink_reopen_thread, s) == 0) pthread_detach(t);
    else atomic_store(&s->reopening, 0);
}

static Sink *sink_open(uint32_t rate, uint32_t channels) {
    Sink *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    s->rate = rate, s->channels = channels > 8 ? 8 : channels;
    s->out_ch = s->channels;
    s->prime_frames = rate / 25;
    s->ring = calloc((size_t)SINK_FRAMES * s->channels, 2);
    pthread_mutex_lock(&s->mu);
    sink_open_stream(s);
    sink_start_locked(s);
    int failed = !s->st;
    pthread_mutex_unlock(&s->mu);
    if (failed) audio_fail_report("no AAudio stream could be opened for %u Hz %u ch (%u attempts failed)", rate, channels, s->open_fails);
    return s;
}

/* The ring is filled even while there is no stream (reopen in progress), so playback resumes with current audio. */
static void sink_write(Sink *s, const int16_t *pcm, uint64_t frames) {
    if (!s || !s->ring || !pcm) return;
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

/* one-line sink state for the periodic stat log */
static void sink_describe(Sink *s, char *buf, size_t len) {
    if (!s) { snprintf(buf, len, "no sink"); return; }
    if (pthread_mutex_trylock(&s->mu) != 0) { snprintf(buf, len, "sink busy (reopening)"); return; }
    if (!s->st) {
        snprintf(buf, len, "NO STREAM (open fails %u, reopens %u)", s->open_fails, s->reopens);
    } else {
        aaudio_stream_state_t state = AAudioStream_getState(s->st);
        uint64_t cb = atomic_load(&s->callbacks), wr = atomic_load(&s->wr);
        snprintf(buf, len, "stream state %s, %u ch %s, callbacks %llu, xruns %d, reopens %u, last error %d",
                 AAudio_convertStreamStateToText(state), s->out_ch, s->out_float ? "float" : "i16",
                 (unsigned long long)cb, AAudioStream_getXRunCount(s->st), s->reopens, s->last_error);
        /* watchdog (called every ~2 s): the game keeps writing audio but the device stopped asking for it */
        if (!atomic_load(&g_audio_paused) && !atomic_load(&s->reopening) && wr != s->wd_wr && cb == s->wd_cb) {
            if (++s->wd_stuck == 3)
                audio_fail_report("the stream is open (state %s, %u ch %s) but the device has not asked for audio in ~6 s; "
                                  "%llu callbacks so far", AAudio_convertStreamStateToText(state), s->out_ch,
                                  s->out_float ? "float" : "i16", (unsigned long long)cb);
        } else {
            s->wd_stuck = 0;
        }
        s->wd_cb = cb, s->wd_wr = wr;
    }
    pthread_mutex_unlock(&s->mu);
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
            char sd[200];
            sink_describe(a->sink, sd, sizeof sd);
            som_log("audout %p: appended %llu released %llu queued %d dropped %llu polls %llu started %u next due in %lld us; "
                    "sink %llu ms queued, %llu underruns; %s",
                    (void *)a, (unsigned long long)a->n_app, (unsigned long long)a->n_rel, a->count,
                    (unsigned long long)a->n_drop, (unsigned long long)a->n_polls, a->started,
                    a->count ? (long long)(a->q[a->head].done_ns - t) / 1000 : -1ll,
                    a->sink ? (unsigned long long)((a->sink->wr - a->sink->rd) * 1000 / a->rate) : 0ull,
                    a->sink ? (unsigned long long)a->sink->underruns : 0ull, sd);
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
    atomic_store(&g_audio_paused, on);
    pthread_mutex_lock(&g_pace_mu);
    for (int i = 0; i < MAX_PACED; i++) {
        AudOut *a = g_audouts[i];
        if (!a || !a->sink) continue;
        Sink *s = a->sink;
        pthread_mutex_lock(&s->mu);
        if (s->st) {
            if (on) {
                AAudioStream_requestPause(s->st);
            } else {
                atomic_store(&s->rd, atomic_load(&s->wr));
                sink_start_locked(s);
            }
        }
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_unlock(&g_pace_mu);
    som_log("audio %s", on ? "paused" : "resumed");
}
