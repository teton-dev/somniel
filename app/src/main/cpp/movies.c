/* Hardware-decodable copies of the game's VP9 movies (see movies.h). */
#include "movies.h"

#include <android/native_window.h>
#include <errno.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "debug_log.h"

extern const char *som_data_dir(void);
extern const char *som_game_dir(void);

#define COPY_SUFFIX ".h264v1.mp4" /* bump when the copies' encoding changes: old copies are then ignored */

typedef struct {
    uint64_t start, size; /* byte range in romfs.bin */
    char name[96];        /* file name without ".webm" */
    volatile int state;   /* 0 not converted, 1 copy complete, -1 conversion failed (not retried this run) */
    volatile int64_t read_ns; /* when the game last read it */
} Movie;

static Movie *g_mv;
static int g_nmv;
static volatile int g_recent = -1, g_want = -1;
static volatile int64_t g_recent_ns, g_tick_ns;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void copy_path(const Movie *m, char *out, size_t n) { snprintf(out, n, "%s/movies/%s" COPY_SUFFIX, som_data_dir(), m->name); }

/* ---- the .webm files of romfs.bin (tools/pack_romfs.py layout: 0x50-byte header of ten u64, file entries
 *      {u32 parent, u32 sibling, u64 data, u64 size, u32 hash_next, u32 name_len, name padded to 4}) ---- */

static int by_start(const void *a, const void *b) {
    const Movie *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static int load_index(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/romfs.bin", som_game_dir());
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint64_t h[10];
    if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h || h[0] != 0x50 || h[8] > (64u << 20)) {
        close(fd);
        return 0;
    }
    uint8_t *meta = malloc(h[8]);
    if (!meta || pread(fd, meta, h[8], (off_t)h[7]) != (ssize_t)h[8]) {
        free(meta);
        close(fd);
        return 0;
    }
    close(fd);
    int cap = 0;
    for (uint64_t o = 0; o + 0x20 <= h[8];) {
        uint64_t data, size;
        uint32_t nl;
        memcpy(&data, meta + o + 8, 8);
        memcpy(&size, meta + o + 16, 8);
        memcpy(&nl, meta + o + 28, 4);
        if (o + 0x20 + nl > h[8]) break;
        const char *nm = (const char *)meta + o + 0x20;
        if (nl > 5 && nl < sizeof g_mv->name + 5 && !strncasecmp(nm + nl - 5, ".webm", 5)) {
            if (g_nmv == cap) {
                cap = cap ? cap * 2 : 64;
                Movie *n = realloc(g_mv, (size_t)cap * sizeof *n);
                if (!n) break;
                g_mv = n;
            }
            Movie *m = &g_mv[g_nmv++];
            memset(m, 0, sizeof *m);
            m->start = h[9] + data, m->size = size;
            memcpy(m->name, nm, nl - 5);
        }
        o += 0x20 + ((nl + 3) & ~3u);
    }
    free(meta);
    if (g_nmv) qsort(g_mv, (size_t)g_nmv, sizeof *g_mv, by_start);
    return g_nmv;
}

void movies_note_read(uint64_t off, uint64_t size) {
    if (!g_nmv || !size) return;
    int lo = 0, hi = g_nmv - 1, hit = -1; /* the last movie starting at or before the end of the read */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_mv[mid].start < off + size) hit = mid, lo = mid + 1;
        else hi = mid - 1;
    }
    if (hit < 0 || off >= g_mv[hit].start + g_mv[hit].size) return;
    if (g_recent != hit) som_log("movies: the game reads %s.webm", g_mv[hit].name);
    g_recent = hit;
    g_recent_ns = g_mv[hit].read_ns = now_ns();
    if (!g_mv[hit].state) g_want = hit; /* convert this one next */
}

int movies_recent_copies(char paths[][MOVIES_PATH_N], char names[][MOVIES_NAME_N], int max) {
    int64_t now = now_ns(), after = now;
    int n = 0;
    while (n < max) { /* the next most recent movie read before `after` */
        int best = -1;
        for (int i = 0; i < g_nmv; i++)
            if (g_mv[i].read_ns && g_mv[i].read_ns < after && now - g_mv[i].read_ns < 20000000000ll &&
                (best < 0 || g_mv[i].read_ns > g_mv[best].read_ns))
                best = i;
        if (best < 0) break;
        after = g_mv[best].read_ns;
        copy_path(&g_mv[best], paths[n], MOVIES_PATH_N);
        if (access(paths[n], R_OK) == 0) snprintf(names[n], MOVIES_NAME_N, "%s", g_mv[best].name), n++;
    }
    return n;
}

void movies_playback_tick(void) { __atomic_store_n(&g_tick_ns, now_ns(), __ATOMIC_RELAXED); }

static int playing(void) { return now_ns() - __atomic_load_n(&g_tick_ns, __ATOMIC_RELAXED) < 2000000000ll; }

int movies_vp9_is_software(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    AMediaCodec *c = AMediaCodec_createDecoderByType("video/x-vnd.on2.vp9");
    char *nm = NULL;
    int sw = 0;
    if (c && AMediaCodec_getName(c, &nm) == AMEDIA_OK && nm) {
        sw = !strncmp(nm, "c2.android.", 11) || !strncmp(nm, "OMX.google.", 11) || !strncmp(nm, "c2.google.", 10);
        som_log("movies: VP9 decoder %s (%s)", nm, sw ? "software: movies are converted to H.264 for the hardware decoder" : "hardware");
        AMediaCodec_releaseName(c, nm);
    }
    if (c) AMediaCodec_delete(c);
    return cached = sw;
}

/* ---- the converter ---- */

static void wait_while_playing(void) {
    while (playing()) usleep(200000);
}

/* romfs .webm -> VP9 decoder -> encoder input surface -> hardware H.264 encoder -> MP4. Returns 1 on success. */
static int convert_one(Movie *m) {
    char src[600], part[640], dst[640];
    snprintf(src, sizeof src, "%s/romfs.bin", som_game_dir());
    copy_path(m, dst, sizeof dst);
    snprintf(part, sizeof part, "%s.part", dst);
    int ok = 0, fd_in = open(src, O_RDONLY | O_CLOEXEC), fd_out = -1, track = -1, muxing = 0;
    AMediaExtractor *ex = NULL;
    AMediaCodec *dec = NULL, *enc = NULL;
    AMediaMuxer *mux = NULL;
    ANativeWindow *win = NULL;
    AMediaFormat *tf = NULL;
    unsigned n_in = 0, n_dec = 0, n_enc = 0;
    int64_t t0 = now_ns(), last_progress = t0;
    if (fd_in < 0) goto out;
    ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(ex, fd_in, (off64_t)m->start, (off64_t)m->size) != AMEDIA_OK) goto out;
    const char *mime = NULL;
    for (size_t t = 0; t < AMediaExtractor_getTrackCount(ex) && !tf; t++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, t);
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && !strncmp(mime, "video/", 6)) {
            AMediaExtractor_selectTrack(ex, t);
            tf = f;
        } else {
            AMediaFormat_delete(f);
        }
    }
    int32_t w = 0, h = 0;
    if (!tf || !AMediaFormat_getInt32(tf, AMEDIAFORMAT_KEY_WIDTH, &w) || !AMediaFormat_getInt32(tf, AMEDIAFORMAT_KEY_HEIGHT, &h)) goto out;

    enc = AMediaCodec_createEncoderByType("video/avc");
    if (!enc) goto out;
    AMediaFormat *ef = AMediaFormat_new();
    AMediaFormat_setString(ef, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(ef, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(ef, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(ef, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7f000789); /* COLOR_FormatSurface */
    AMediaFormat_setInt32(ef, AMEDIAFORMAT_KEY_BIT_RATE, (int32_t)((int64_t)w * h * 30 * 18 / 100)); /* ~11 Mbps at 1080p30 */
    AMediaFormat_setInt32(ef, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
    AMediaFormat_setInt32(ef, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1); /* a keyframe a second: short catch-up after a seek */
    AMediaFormat_setInt32(ef, "max-bframes", 0);                     /* frames come out in the order they go in */
    AMediaFormat_setInt32(ef, "priority", 1);                        /* not real time */
    media_status_t s = AMediaCodec_configure(enc, ef, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(ef);
    if (s != AMEDIA_OK || AMediaCodec_createInputSurface(enc, &win) != AMEDIA_OK || AMediaCodec_start(enc) != AMEDIA_OK) {
        som_log("movies: H.264 encoder setup failed (%d)", s);
        goto out;
    }
    dec = AMediaCodec_createDecoderByType(mime);
    if (!dec || AMediaCodec_configure(dec, tf, win, NULL, 0) != AMEDIA_OK || AMediaCodec_start(dec) != AMEDIA_OK) goto out;
    {
        char *en = NULL, *dn = NULL;
        AMediaCodec_getName(enc, &en), AMediaCodec_getName(dec, &dn);
        som_log("movies: converting %s.webm (%dx%d) with %s -> %s", m->name, w, h, dn ? dn : "?", en ? en : "?");
        if (en) AMediaCodec_releaseName(enc, en);
        if (dn) AMediaCodec_releaseName(dec, dn);
    }
    fd_out = open(part, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd_out < 0 || !(mux = AMediaMuxer_new(fd_out, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4))) goto out;

    int in_eos = 0, dec_eos = 0, enc_eos = 0;
    while (!enc_eos) {
        wait_while_playing();
        int moved = 0;
        if (!in_eos) {
            ssize_t ii = AMediaCodec_dequeueInputBuffer(dec, 2000);
            if (ii >= 0) {
                size_t cap = 0;
                uint8_t *buf = AMediaCodec_getInputBuffer(dec, (size_t)ii, &cap);
                ssize_t n = buf ? AMediaExtractor_readSampleData(ex, buf, cap) : -1;
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(dec, (size_t)ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    in_eos = 1;
                } else {
                    AMediaCodec_queueInputBuffer(dec, (size_t)ii, 0, (size_t)n, (uint64_t)AMediaExtractor_getSampleTime(ex), 0);
                    AMediaExtractor_advance(ex);
                    n_in++;
                }
                moved = 1;
            }
        }
        if (!dec_eos) {
            AMediaCodecBufferInfo di;
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(dec, &di, 2000);
            if (oi >= 0) {
                if (di.size > 0) { /* render into the encoder's surface with the frame's own timestamp */
                    AMediaCodec_releaseOutputBufferAtTime(dec, (size_t)oi, di.presentationTimeUs * 1000);
                    n_dec++;
                } else {
                    AMediaCodec_releaseOutputBuffer(dec, (size_t)oi, false);
                }
                if (di.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                    AMediaCodec_signalEndOfInputStream(enc);
                    dec_eos = 1;
                }
                moved = 1;
            }
        }
        for (;;) {
            AMediaCodecBufferInfo ei;
            ssize_t eo = AMediaCodec_dequeueOutputBuffer(enc, &ei, dec_eos ? 10000 : 0);
            if (eo == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat *of = AMediaCodec_getOutputFormat(enc);
                track = (int)AMediaMuxer_addTrack(mux, of);
                AMediaFormat_delete(of);
                muxing = track >= 0 && AMediaMuxer_start(mux) == AMEDIA_OK;
                if (!muxing) goto out;
                continue;
            }
            if (eo < 0) break;
            size_t cap = 0;
            uint8_t *buf = AMediaCodec_getOutputBuffer(enc, (size_t)eo, &cap);
            if (muxing && buf && ei.size > 0 && !(ei.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                AMediaMuxer_writeSampleData(mux, (size_t)track, buf, &ei);
                n_enc++;
            }
            AMediaCodec_releaseOutputBuffer(enc, (size_t)eo, false);
            if (ei.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) enc_eos = 1;
            moved = 1;
        }
        if (moved) last_progress = now_ns();
        else if (now_ns() - last_progress > 15000000000ll && !playing()) {
            som_log("movies: %s stalled (in %u, decoded %u, encoded %u)", m->name, n_in, n_dec, n_enc);
            goto out;
        }
    }
    ok = muxing && n_enc > 0;

out:
    if (dec) AMediaCodec_stop(dec), AMediaCodec_delete(dec);
    if (enc) AMediaCodec_stop(enc), AMediaCodec_delete(enc);
    if (win) ANativeWindow_release(win);
    if (mux) {
        if (muxing) AMediaMuxer_stop(mux);
        AMediaMuxer_delete(mux);
    }
    if (tf) AMediaFormat_delete(tf);
    if (ex) AMediaExtractor_delete(ex);
    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0) close(fd_out);
    if (ok && rename(part, dst) != 0) ok = 0;
    if (!ok) unlink(part);
    struct stat st;
    som_log("movies: %s %s: %u samples, %u decoded, %u encoded%s, %.1f s, %lld KB", m->name, ok ? "converted" : "FAILED", n_in, n_dec,
            n_enc, n_dec != n_enc ? " (counts differ)" : "", (double)(now_ns() - t0) / 1e9,
            ok && !stat(dst, &st) ? (long long)st.st_size / 1024 : 0ll);
    return ok;
}

static void *converter_main(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)gettid(), 10); /* below the game's threads */
    sleep(5);
    int done = 0, failed = 0;
    for (;;) {
        int pick = g_want;
        if (pick < 0 || g_mv[pick].state) {
            pick = -1;
            for (int i = 0; i < g_nmv && pick < 0; i++)
                if (!g_mv[i].state) pick = i;
        }
        if (pick < 0) break;
        if (g_want == pick) g_want = -1;
        wait_while_playing();
        if (convert_one(&g_mv[pick])) g_mv[pick].state = 1, done++;
        else g_mv[pick].state = -1, failed++;
    }
    som_log("movies: converter finished (%d converted this run, %d failed)", done, failed);
    return NULL;
}

void movies_init(void) {
    if (!load_index()) {
        som_log("movies: no .webm files indexed in romfs.bin");
        return;
    }
    char p[600];
    snprintf(p, sizeof p, "%s/movies", som_data_dir());
    mkdir(p, 0755);
    int have = 0;
    for (int i = 0; i < g_nmv; i++) {
        char cp[640];
        copy_path(&g_mv[i], cp, sizeof cp);
        if (access(cp, R_OK) == 0) g_mv[i].state = 1, have++;
        snprintf(p, sizeof p, "%s.part", cp);
        unlink(p); /* an interrupted conversion */
    }
    som_log("movies: %d movies in romfs.bin, %d converted copies present", g_nmv, have);
    snprintf(p, sizeof p, "%s/no_movie_convert", som_data_dir());
    if (have == g_nmv || access(p, F_OK) == 0 || !movies_vp9_is_software()) return;
    pthread_t th;
    if (pthread_create(&th, NULL, converter_main, NULL) == 0) {
        pthread_setname_np(th, "som-movconv");
        pthread_detach(th);
    }
}
