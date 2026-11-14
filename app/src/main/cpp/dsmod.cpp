// Dual-screen companion SPIKE (Eden Duo .dsmod package, FE Engage).
//
// Proves, without any UI, that the native game can drive an Eden Duo companion module:
//   1. dsmod_apply_plan(): applies the package's guest load plan (instruction patches + payload code that talks to a
//      4 KB "mailbox" page) to the main NSO image, before the text segment is sealed.
//   2. dsmod_start(): loads the package's android-arm64 module, gives it a stub host API (guest memory is directly
//      readable: guest VA == host VA), then ticks it ten times a second and logs what it publishes.
// Package layout expected at <data>/dsmod/ (an unzipped .dsmod.zip: dualscreen/guest/<build>/load-plan.json and
// dualscreen/modules/android-arm64-v8a/<title>.so). Nothing happens when <data>/dsmod/dualscreen is absent.
//
// The package is bundled in the APK (assets/dsmod, installed on second-display devices by DualScreen.installBundled).
// The ABI headers belong to Eden Duo (GPL-3.0-or-later, igawa6) and are vendored in third_party/eden_duo.

#include <android/dlext.h>
#include <sched.h>
#include <sys/resource.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <algorithm>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "debug_log.h"
#include "guest.h"
}
#include "astc_decode.h"
#include "dsui/dsraster.h"
#include "dsui/dsui.h"
#include "dsmod_module_abi.h"
#include "dsmod_module_extensions.h"

extern "C" const char *som_data_dir(void);
extern "C" const char *som_game_dir(void);

namespace {

// ---------------------------------------------------------------- tiny JSON reader (enough for load-plan.json)
struct J {
    enum T { Null, Num, Str, Arr, Obj, Bool } t = Null;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J *get(const char *k) const {
        for (auto &p : o)
            if (p.first == k) return &p.second;
        return nullptr;
    }
};

struct Parser {
    const char *p, *e;
    bool ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        return p < e;
    }
    bool str(std::string &out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                out += *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                p++;
            } else out += *p++;
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(J &j) {
        if (!ws()) return false;
        if (*p == '{') {
            p++;
            j.t = J::Obj;
            if (!ws()) return false;
            if (*p == '}') return p++, true;
            for (;;) {
                if (!ws()) return false;
                std::string k;
                J v;
                if (!str(k) || !ws() || *p++ != ':' || !val(v)) return false;
                j.o.emplace_back(std::move(k), std::move(v));
                if (!ws()) return false;
                if (*p == ',') { p++; continue; }
                return *p++ == '}';
            }
        }
        if (*p == '[') {
            p++;
            j.t = J::Arr;
            if (!ws()) return false;
            if (*p == ']') return p++, true;
            for (;;) {
                J v;
                if (!val(v)) return false;
                j.a.push_back(std::move(v));
                if (!ws()) return false;
                if (*p == ',') { p++; continue; }
                return *p++ == ']';
            }
        }
        if (*p == '"') { j.t = J::Str; return str(j.s); }
        if (!strncmp(p, "true", 4)) { p += 4; j.t = J::Bool; j.n = 1; return true; }
        if (!strncmp(p, "false", 5)) { p += 5; j.t = J::Bool; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char *end;
        j.n = strtod(p, &end);
        if (end == p) return false;
        j.t = J::Num;
        p = end;
        return true;
    }
};

bool read_file(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool hexbytes(const std::string &h, std::vector<uint8_t> &out) {
    if (h.size() % 2) return false;
    for (size_t i = 0; i < h.size(); i += 2) {
        unsigned v;
        if (sscanf(h.c_str() + i, "%2x", &v) != 1) return false;
        out.push_back((uint8_t)v);
    }
    return true;
}

std::string g_pkg; // <data>/dsmod/dualscreen
uint64_t g_main_base, g_main_size;
uint8_t g_build_id[32];
uint64_t g_mailbox;
constexpr uint64_t kMailboxSize = 4096;
bool g_plan_applied;

std::string hex(const uint8_t *b, size_t n) {
    static const char d[] = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) s += d[b[i] >> 4], s += d[b[i] & 15];
    return s;
}

// ---------------------------------------------------------------- load plan
struct Reloc { uint32_t off; bool adrp; int64_t addend; };
struct Write { uint64_t off; std::vector<uint8_t> expected, repl; std::vector<Reloc> relocs; };

bool parse_writes(const J *list, bool payload, std::vector<Write> &out) {
    if (!list) return true;
    for (auto &it : list->a) {
        Write w;
        const J *off = it.get("offset"), *exp = it.get("expected"), *rep = it.get(payload ? "bytes" : "replacement");
        if (!off || !exp || !rep || !hexbytes(exp->s, w.expected) || !hexbytes(rep->s, w.repl) ||
            w.expected.size() != w.repl.size())
            return false;
        w.off = (uint64_t)off->n;
        if (const J *rl = it.get("relocations")) {
            for (auto &r : rl->a) {
                const J *ro = r.get("offset"), *kind = r.get("kind"), *tgt = r.get("target"), *ad = r.get("addend");
                if (!ro || !kind || !tgt || tgt->s != "mailbox") return false;
                if (kind->s != "aarch64_adrp" && kind->s != "aarch64_add_lo12") return false;
                w.relocs.push_back({(uint32_t)ro->n, kind->s == "aarch64_adrp", ad ? (int64_t)ad->n : 0});
            }
        }
        out.push_back(std::move(w));
    }
    return true;
}

} // namespace

extern "C" int dsmod_apply_plan(const NsoModule *mods, int nmods, uint64_t mailbox_hint) {
    std::string dir = std::string(som_data_dir()) + "/dsmod/dualscreen";
    if (access(dir.c_str(), F_OK) != 0) return 0; // no package: nothing to do
    const NsoModule *m = nullptr;
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].name, "main")) m = &mods[i];
    if (!m) return 0;
    g_pkg = dir;
    g_main_base = m->base;
    g_main_size = m->image_size;
    memcpy(g_build_id, m->build_id, 32);
    std::string bid = hex(m->build_id, 32);
    std::string planpath = dir + "/guest/" + bid + "/load-plan.json";
    std::string raw;
    if (!read_file(planpath, raw)) {
        som_log("dsmod: no load plan for build %s (%s): companion off", bid.c_str(), planpath.c_str());
        return 0;
    }
    J root;
    Parser ps{raw.data(), raw.data() + raw.size()};
    if (!ps.val(root) || root.t != J::Obj) {
        som_log("dsmod: load-plan.json does not parse");
        return -1;
    }
    std::vector<Write> writes;
    if (!parse_writes(root.get("patches"), false, writes) || !parse_writes(root.get("payloads"), true, writes)) {
        som_log("dsmod: load plan has unsupported entries");
        return -1;
    }
    // mailbox: one page the payload code addresses with adrp+add (so it must be within 4 GiB of main)
    uint64_t mb = 0;
    for (int k = 0; k < 64 && !mb; k++) {
        void *want = (void *)(uintptr_t)(mailbox_hint + (uint64_t)k * 0x100000);
        void *p = mmap(want, kMailboxSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && p == want) mb = (uint64_t)(uintptr_t)p;
        else if (p != MAP_FAILED) munmap(p, kMailboxSize);
    }
    if (!mb) {
        som_log("dsmod: cannot map the mailbox near 0x%llx", (unsigned long long)mailbox_hint);
        return -1;
    }
    guest_region_set(mb, kMailboxSize, MT_CODE_MUTABLE, PERM_R | PERM_W);
    // check everything first: one mismatch and nothing is written
    for (auto &w : writes) {
        if (w.off + w.expected.size() > m->image_size ||
            memcmp((const void *)(uintptr_t)(m->base + w.off), w.expected.data(), w.expected.size()) != 0) {
            uint32_t cur = 0;
            if (w.off + 4 <= m->image_size) memcpy(&cur, (const void *)(uintptr_t)(m->base + w.off), 4);
            som_log("dsmod: load plan refused: main+%#llx is %08x, not what the plan expects (another mod?)",
                    (unsigned long long)w.off, cur);
            munmap((void *)(uintptr_t)mb, kMailboxSize);
            return -1;
        }
    }
    int nrel = 0;
    for (auto &w : writes) {
        std::vector<uint8_t> r = w.repl;
        for (auto &rl : w.relocs) {
            uint32_t ins;
            memcpy(&ins, r.data() + rl.off, 4);
            uint64_t at = m->base + w.off + rl.off, target = mb + (uint64_t)rl.addend;
            if (rl.adrp) {
                int64_t pages = (int64_t)(target >> 12) - (int64_t)(at >> 12);
                if (pages < -(1 << 20) || pages >= (1 << 20) || (ins & 0x9F000000) != 0x90000000) {
                    som_log("dsmod: adrp out of range or malformed at main+%#llx", (unsigned long long)(w.off + rl.off));
                    return -1;
                }
                uint64_t imm = (uint64_t)pages & 0x1FFFFF;
                ins = (ins & 0x9F00001F) | (uint32_t)((imm & 3) << 29) | (uint32_t)((imm >> 2) << 5);
            } else {
                if ((ins & 0xFFC00000) != 0x91000000) return -1;
                ins = (ins & ~(0xFFFu << 10)) | (uint32_t)((target & 0xFFF) << 10);
            }
            memcpy(r.data() + rl.off, &ins, 4);
            nrel++;
        }
        memcpy((void *)(uintptr_t)(m->base + w.off), r.data(), r.size());
        __builtin___clear_cache((char *)(m->base + w.off), (char *)(m->base + w.off + r.size()));
    }
    g_plan_applied = true;
    som_log("dsmod: load plan applied to main (%zu writes, %d relocations), mailbox at 0x%llx", writes.size(), nrel,
            (unsigned long long)mb);
    g_mailbox = mb;
    return 1;
}

// ---------------------------------------------------------------- host API
namespace {

std::atomic<uint64_t> g_ticks;
std::map<std::string, std::string> g_pub;
int g_pub_logged;
int g_romfs_logged;
const EdenDsmodModuleApi *g_api;
const EdenDsmodModuleExtensions *g_ext;
void *g_inst;
EdenDsmodHostApi g_host;
EdenDsmodHostExtensions g_hext;

std::atomic<uint64_t> g_c_mapped{0}, g_c_read{0}, g_c_ptr{0}, g_c_bytes{0}, g_c_fail{0}, g_c_publish{0}, g_c_romfs{0};
std::atomic<uint64_t> g_t_cb_ns{0}; // time inside the host callbacks the module makes
struct CbTimer {
    struct timespec t0;
    CbTimer() { clock_gettime(CLOCK_MONOTONIC, &t0); }
    ~CbTimer() { struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1); g_t_cb_ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec)); }
};

// Validated regions are cached per thread (the module's thread and the image worker call concurrently) and dropped
// whenever the guest changes its memory map (g_map_epoch, guest.h). The module makes ~145,000 checks a second.
bool range_ok(uint64_t a, uint64_t n) {
    if (!a || n == 0 || n > (1u << 20) || a > UINT64_MAX - n) return false;
    struct Ent { uint64_t lo, hi; };
    static thread_local Ent cache[8];
    static thread_local int ncache, nextslot;
    static thread_local uint64_t cache_epoch = ~0ull;
    uint64_t ep = __atomic_load_n(&g_map_epoch, __ATOMIC_ACQUIRE);
    if (ep != cache_epoch) { ncache = 0; nextslot = 0; cache_epoch = ep; }
    for (int i = 0; i < ncache; i++)
        if (a >= cache[i].lo && a + n <= cache[i].hi) return true;
    MemInfo mi;
    guest_query(a, &mi);
    if (mi.type == MT_FREE || !(mi.perm & PERM_R)) return false;
    uint64_t hi = mi.base + mi.size;
    if (a + n > hi) { // spans regions: check the last byte too, do not cache the span
        guest_query(a + n - 1, &mi);
        return !(mi.type == MT_FREE || !(mi.perm & PERM_R));
    }
    if (__atomic_load_n(&g_map_epoch, __ATOMIC_ACQUIRE) == ep) { // the map did not change while we looked
        cache[nextslot] = {mi.base, hi};
        nextslot = (nextslot + 1) & 7;
        if (ncache < 8) ncache++;
    }
    return true;
}

dsui::Host *g_ui;
std::atomic<bool> g_ui_dirty{true};

void publish(const char *name, const std::string &v) {
    g_c_publish++;
    auto it = g_pub.find(name);
    if (it != g_pub.end() && it->second == v) return;
    bool isnew = it == g_pub.end();
    g_pub[name] = v;
    if (g_pub_logged < 400 && (isnew || !strncmp(name, "war.", 4) || !strncmp(name, "fee.", 4))) {
        g_pub_logged++;
        som_log("dsmod: %s = %s", name, v.c_str());
    }
}

} // namespace

static void host_log(void *, uint32_t lvl, const char *msg) { som_log("dsmod module[%u]: %s", lvl, msg); }
static uint64_t host_tick(void *) { return g_ticks.load(); }
static uint64_t host_heap_begin(void *) { return HEAP_BASE; }
static uint64_t host_heap_end(void *) { return HEAP_BASE + HEAP_REGION; }
static EdenDsmodBool host_is_mapped(void *, uint64_t a, uint64_t n) { g_c_mapped++; CbTimer t; return range_ok(a, n); }
static EdenDsmodBool host_read(void *, uint64_t a, void *out, size_t n) {
    g_c_read++; g_c_bytes += n; CbTimer t;
    if (!out || !range_ok(a, n)) { g_c_fail++; return 0; }
    memcpy(out, (const void *)(uintptr_t)a, n);
    return 1;
}
static const uint8_t *host_ptr(void *, uint64_t a, size_t n) {
    g_c_ptr++; CbTimer t;
    if (n == 0 || n > 4096 - (a & 4095) || !range_ok(a, n)) return nullptr;
    return (const uint8_t *)(uintptr_t)a;
}
static void host_begin(void *) {}
static void host_pi64(void *, const char *n, int64_t v) { if (g_ui) g_ui->values.set_i(n, v); publish(n, std::to_string(v)); }
static void host_pf64(void *, const char *n, double v) { if (g_ui) g_ui->values.set_f(n, v); publish(n, std::to_string(v)); }
static void host_ptext(void *, const char *n, const char *v) { if (g_ui) g_ui->values.set_t(n, v ? v : ""); publish(n, std::string("\"") + (v ? v : "") + "\""); }
static void host_paddr(void *, const char *n, uint64_t v) { char b[32]; snprintf(b, sizeof b, "0x%llx", (unsigned long long)v); publish(n, b); }
static void host_pmap(void *, const EdenDsmodMapFrame *f) { publish("map.frame", f && f->area ? f->area : "?"); }
/* ---- romfs reader: <game dir>/romfs.bin is a standard Horizon RomFS image (tools/pack_romfs.py) ---- */
namespace {
struct RomEnt { uint64_t off, size; bool ok; };
int g_rom_fd = -2;
uint64_t g_hdr[10];
std::vector<uint8_t> g_dirs, g_files;
std::map<std::string, RomEnt> g_rom_cache;
pthread_mutex_t g_rom_mu = PTHREAD_MUTEX_INITIALIZER;

bool rom_open() {
    if (g_rom_fd != -2) return g_rom_fd >= 0;
    std::string p = std::string(som_game_dir()) + "/romfs.bin";
    g_rom_fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (g_rom_fd < 0) { som_log("dsmod: cannot open %s", p.c_str()); return false; }
    if (pread(g_rom_fd, g_hdr, sizeof g_hdr, 0) != (ssize_t)sizeof g_hdr || g_hdr[0] != 0x50 || g_hdr[4] > (64u << 20) ||
        g_hdr[8] > (256u << 20)) { close(g_rom_fd); g_rom_fd = -1; return false; }
    g_dirs.resize(g_hdr[4]);
    g_files.resize(g_hdr[8]);
    if (pread(g_rom_fd, g_dirs.data(), g_dirs.size(), g_hdr[3]) != (ssize_t)g_dirs.size() ||
        pread(g_rom_fd, g_files.data(), g_files.size(), g_hdr[7]) != (ssize_t)g_files.size()) {
        close(g_rom_fd); g_rom_fd = -1; return false;
    }
    som_log("dsmod: romfs open (%zu dir bytes, %zu file bytes)", g_dirs.size(), g_files.size());
    return true;
}

uint32_t rd32m(const std::vector<uint8_t> &v, uint32_t o) { uint32_t x; memcpy(&x, v.data() + o, 4); return x; }

/* walk the directory tree: dir entry = parent, sibling, child_dir, child_file, hash_next, name_len (6 x u32) + name;
 * file entry = parent, sibling, data (u64), size (u64), hash_next, name_len + name */
RomEnt rom_find(const std::string &path) {
    RomEnt none{0, 0, false};
    uint32_t dir = 0;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        bool last = j == std::string::npos;
        std::string part = path.substr(i, last ? std::string::npos : j - i);
        i = last ? path.size() : j + 1;
        if (part.empty()) continue;
        if (last) {
            for (uint32_t f = rd32m(g_dirs, dir + 12); f != 0xFFFFFFFFu && f + 32 <= g_files.size(); f = rd32m(g_files, f + 4)) {
                uint32_t nl = rd32m(g_files, f + 28);
                if (f + 32 + nl <= g_files.size() && nl == part.size() && !memcmp(&g_files[f + 32], part.data(), nl)) {
                    uint64_t data, size;
                    memcpy(&data, &g_files[f + 8], 8);
                    memcpy(&size, &g_files[f + 16], 8);
                    return {g_hdr[9] + data, size, true};
                }
            }
            return none;
        }
        uint32_t nd = 0xFFFFFFFFu;
        for (uint32_t d = rd32m(g_dirs, dir + 8); d != 0xFFFFFFFFu && d + 24 <= g_dirs.size(); d = rd32m(g_dirs, d + 4)) {
            uint32_t nl = rd32m(g_dirs, d + 20);
            if (d + 24 + nl <= g_dirs.size() && nl == part.size() && !memcmp(&g_dirs[d + 24], part.data(), nl)) { nd = d; break; }
        }
        if (nd == 0xFFFFFFFFu) return none;
        dir = nd;
    }
    return none;
}
} // namespace

static size_t host_romfs(void *, const char *path, uint64_t off, void *out, size_t n) {
    if (!path) return 0;
    std::string p = path;
    if (p.compare(0, 6, "romfs:") == 0) p.erase(0, 6);
    if (!p.empty() && p[0] == '/') p.erase(0, 1);
    pthread_mutex_lock(&g_rom_mu);
    size_t got = 0;
    if (rom_open()) {
        auto it = g_rom_cache.find(p);
        if (it == g_rom_cache.end()) {
            RomEnt e = rom_find(p);
            if (!e.ok && g_romfs_logged++ < 40) som_log("dsmod: romfs miss \"%s\"", p.c_str());
            it = g_rom_cache.emplace(p, e).first;
        }
        const RomEnt &e = it->second;
        if (e.ok) {
            if (!out) got = (size_t)e.size;
            else if (off < e.size) {
                size_t want = (size_t)std::min<uint64_t>(n, e.size - off);
                ssize_t r = pread(g_rom_fd, out, want, (off_t)(e.off + off));
                got = r > 0 ? (size_t)r : 0;
            }
        }
    }
    pthread_mutex_unlock(&g_rom_mu);
    return got;
}
static int64_t host_geti(void *, const char *name, int64_t fb) {
    if (name && !strcmp(name, "__aux_present")) return 0;
    return fb;
}
static double host_getf(void *, const char *, double fb) { return fb; }
static const char *host_gettext(void *, const char *) { return nullptr; }

static EdenDsmodBool mb_ok(uint32_t off, uint32_t sz) { return g_mailbox && (off & (sz - 1)) == 0 && off + sz <= kMailboxSize; }
static EdenDsmodBool mb_l32(void *, uint32_t off, uint32_t *v) {
    if (!mb_ok(off, 4) || !v) return 0;
    *v = __atomic_load_n((uint32_t *)(uintptr_t)(g_mailbox + off), __ATOMIC_ACQUIRE);
    return 1;
}
static EdenDsmodBool mb_l64(void *, uint32_t off, uint64_t *v) {
    if (!mb_ok(off, 8) || !v) return 0;
    *v = __atomic_load_n((uint64_t *)(uintptr_t)(g_mailbox + off), __ATOMIC_ACQUIRE);
    return 1;
}
static EdenDsmodBool mb_s32(void *, uint32_t off, uint32_t v) {
    if (!mb_ok(off, 4)) return 0;
    __atomic_store_n((uint32_t *)(uintptr_t)(g_mailbox + off), v, __ATOMIC_RELEASE);
    return 1;
}
static EdenDsmodBool mb_s64(void *, uint32_t off, uint64_t v) {
    if (!mb_ok(off, 8)) return 0;
    __atomic_store_n((uint64_t *)(uintptr_t)(g_mailbox + off), v, __ATOMIC_RELEASE);
    return 1;
}
static EdenDsmodBool host_astc(void *, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, const uint8_t *in, size_t isz,
                               uint8_t *out, size_t osz) {
    if (!in || !out || !w || !h || w > 8192 || h > 8192 || bw < 4 || bw > 12 || bh < 4 || bh > 12) return 0;
    size_t blocks = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    if (isz < blocks * 16 || osz < (size_t)w * h * 4) return 0;
    astc_decode_ldr(in, w, h, bw, bh, false, out);
    return 1;
}

namespace {

// Android refuses to dlopen a library from the app's writable data directory (untrusted_app, targetSdk >= 29): the
// module is copied into a memfd and opened from its descriptor instead.
void *open_module(const std::string &path) {
    if (void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        som_log("dsmod: module opened directly from %s", path.c_str());
        return h;
    }
    som_log("dsmod: direct dlopen failed (%s); trying a memfd", dlerror());
    std::string bytes;
    if (!read_file(path, bytes)) return nullptr;
    int fd = (int)syscall(SYS_memfd_create, "dsmod_module", 0);
    if (fd < 0) { som_log("dsmod: memfd_create failed: %s", strerror(errno)); return nullptr; }
    if (write(fd, bytes.data(), bytes.size()) != (ssize_t)bytes.size()) { close(fd); return nullptr; }
    android_dlextinfo ext;
    memset(&ext, 0, sizeof ext);
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    ext.library_fd = fd;
    void *h = android_dlopen_ext("dsmod_module.so", RTLD_NOW | RTLD_LOCAL, &ext);
    if (!h) {
        som_log("dsmod: memfd dlopen failed: %s", dlerror());
        // second attempt: through /proc/self/fd
        char p[64];
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
        if (!h) som_log("dsmod: /proc/self/fd dlopen failed: %s", dlerror());
    }
    if (h) som_log("dsmod: module opened from a memfd");
    return h;
}

// ---- spike check: render every image key the module publishes to a PNG ----
uint32_t crc_table[256];
void crc_init() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
uint32_t crc32u(uint32_t c, const uint8_t *p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
void put32(std::string &s, uint32_t v) { for (int i = 3; i >= 0; i--) s += (char)(v >> (i * 8)); }
void chunk(FILE *f, const char *type, const std::string &data) {
    std::string h;
    put32(h, (uint32_t)data.size());
    fwrite(h.data(), 1, 4, f);
    std::string td = std::string(type, 4) + data;
    fwrite(td.data(), 1, td.size(), f);
    std::string c;
    put32(c, crc32u(0, (const uint8_t *)td.data(), td.size()));
    fwrite(c.data(), 1, 4, f);
}
bool write_png(const std::string &path, uint32_t w, uint32_t h, const uint8_t *rgba) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    fwrite(sig, 1, 8, f);
    std::string ihdr;
    put32(ihdr, w); put32(ihdr, h);
    ihdr += (char)8; ihdr += (char)6; ihdr += (char)0; ihdr += (char)0; ihdr += (char)0;
    chunk(f, "IHDR", ihdr);
    std::string raw; // scanlines with filter byte 0, stored in uncompressed deflate blocks
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (uint32_t y = 0; y < h; y++) { raw += (char)0; raw.append((const char *)rgba + (size_t)y * w * 4, (size_t)w * 4); }
    std::string z;
    z += (char)0x78; z += (char)0x01;
    for (size_t o = 0; o < raw.size() || o == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - o);
        z += (char)(o + n >= raw.size() ? 1 : 0);
        z += (char)(n & 255); z += (char)(n >> 8); z += (char)(~n & 255); z += (char)((~n >> 8) & 255);
        z.append(raw, o, n);
        o += n;
        if (n == 0) break;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", "");
    fclose(f);
    return true;
}

/* Debug dumps (module images, font atlas, settled frames, published values) into <data>/dsmod_dump: only when
 * <data>/dsmod_dump_on exists, so players' devices are not filled with PNGs. */
bool dump_on() {
    static int on = -1;
    if (on < 0) {
        on = access((std::string(som_data_dir()) + "/dsmod_dump_on").c_str(), F_OK) == 0;
        if (on) { crc_init(); mkdir((std::string(som_data_dir()) + "/dsmod_dump").c_str(), 0775); } // for write_png
    }
    return on;
}

struct DumpCtx { std::string key; };
int g_dumped;
void image_sink(void *recv, uint32_t w, uint32_t h, const uint8_t *px, size_t size) {
    DumpCtx *c = (DumpCtx *)recv;
    if (size < (size_t)w * h * 4) return;
    std::string name;
    for (char ch : c->key) name += (isalnum((unsigned char)ch) ? ch : '_');
    if (name.size() > 90) name = name.substr(name.size() - 90);
    char num[16];
    snprintf(num, sizeof num, "%02d_", g_dumped);
    std::string path = std::string(som_data_dir()) + "/dsmod_dump/" + num + name + ".png";
    bool ok = write_png(path, w, h, px);
    som_log("dsmod: image \"%s\" %ux%u -> %s", c->key.c_str(), w, h, ok ? path.c_str() : "(write failed)");
}

void dump_new_images() {
    static std::map<std::string, int> done;
    if (!dump_on() || !g_ext || !g_ext->load_image || g_dumped >= 60) return;
    std::vector<std::string> keys;
    for (auto &kv : g_pub) {
        const std::string &v = kv.second;
        if (v.size() > 9 && v.compare(0, 9, "\"module:f") == 0) {
            std::string k = v.substr(1, v.size() - 2);
            if (!done.count(k)) keys.push_back(k);
        }
    }
    for (auto &k : keys) {
        if (g_dumped >= 60) break;
        done[k] = 1;
        DumpCtx c{k};
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        bool ok = g_ext->load_image(g_inst, &g_host, k.c_str(), &c, image_sink);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        som_log("dsmod: load_image \"%s\" %s in %.1f ms", k.c_str(), ok ? "ok" : "FAILED",
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        g_dumped++;
    }
}

// ---- font: metrics from the module's decode_font, atlas from load_image("module:fee:font/regular") ----
dsui::Image g_font_img;
struct FontData {
    bool ready = false;
    uint32_t line_height = 0, first = 0;
    std::vector<EdenDsmodFontGlyph> glyphs;
} g_font;
const EdenDsmodFontExtensions *g_fext;
int g_font_tries;
int64_t g_font_epoch_done = -1;

void font_sink(void *, uint32_t line_height, uint32_t first, const EdenDsmodFontGlyph *g, uint32_t n) {
    g_font.line_height = line_height;
    g_font.first = first;
    g_font.glyphs.assign(g, g + n);
}

void font_atlas_sink(void *, uint32_t w, uint32_t h, const uint8_t *px, size_t size) {
    if (size < (size_t)w * h * 4) return;
    std::string path = std::string(som_data_dir()) + "/dsmod_dump/font_atlas.png";
    g_font_img.w = (int)w;
    g_font_img.h = (int)h;
    g_font_img.rgba.assign(px, px + (size_t)w * h * 4);
    if (dump_on()) write_png(path, w, h, px);
    som_log("dsmod: font atlas %ux%u", w, h);
}

void try_font() {
    if (!g_fext || !g_fext->decode_font || !g_ext || !g_ext->load_image) return;
    int64_t epoch = 0;
    auto it = g_pub.find("__font_epoch");
    if (it != g_pub.end()) epoch = atoll(it->second.c_str());
    if (g_font.ready && epoch == g_font_epoch_done) return;
    if (g_font_tries >= 30 && epoch == g_font_epoch_done) return;
    g_font_tries++;
    std::string bytes;
    read_file(g_pkg + "/fee_font.txt", bytes);
    g_font.glyphs.clear();
    bool ok = g_fext->decode_font(g_inst, (const uint8_t *)bytes.data(), bytes.size(), nullptr, font_sink);
    if (!ok || g_font.glyphs.empty()) {
        if (g_font_tries <= 3 || g_font_tries % 10 == 0) som_log("dsmod: decode_font declined (try %d, epoch %lld)", g_font_tries, (long long)epoch);
        return;
    }
    g_font.ready = true;
    g_font_epoch_done = epoch;
    som_log("dsmod: font decoded: line height %u, first codepoint %u, %zu glyphs (epoch %lld, try %d)", g_font.line_height,
            g_font.first, g_font.glyphs.size(), (long long)epoch, g_font_tries);
    for (uint32_t cp : {(uint32_t)'A', (uint32_t)'g', (uint32_t)' '}) {
        if (cp < g_font.first || cp - g_font.first >= g_font.glyphs.size()) continue;
        const auto &g = g_font.glyphs[cp - g_font.first];
        som_log("dsmod:   '%c' atlas %u,%u %ux%u bearing %d,%d advance %u", (char)cp, g.x, g.y, g.w, g.h, g.bearing_x, g.bearing_y, g.advance);
    }
    int total = 0, with = 0;
    for (auto &g : g_font.glyphs) { total++; if (g.w && g.h) with++; }
    som_log("dsmod:   %d glyph slots, %d drawable", total, with);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool lok = g_ext->load_image(g_inst, &g_host, "module:fee:font/regular", nullptr, font_atlas_sink);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    som_log("dsmod: font atlas load_image %s in %.1f ms", lok ? "ok" : "FAILED", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}

// ---- companion UI: layout + CPU render of the current page, frames kept for the presenter ----
// The companion must not take CPU from the game: its threads run on the middle cores at a lower priority (nice 10).
// <data>/dsmod_affinity holds a hex CPU mask to override ("0" = no pinning), for experiments.
void low_impact_thread(const char *what) {
    static int mask_logged;
    int n = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (n > 32) n = 32;
    long fr[32];
    long minf = 1L << 60;
    for (int i = 0; i < n; i++) {
        fr[i] = 0;
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if (FILE *f = fopen(p, "r")) { if (fscanf(f, "%ld", &fr[i]) != 1) fr[i] = 0; fclose(f); }
        if (fr[i] > 0 && fr[i] < minf) minf = fr[i];
    }
    long maxf = 0;
    for (int i = 0; i < n; i++) maxf = std::max(maxf, fr[i]);
    unsigned long mask = 0; // the middle cluster: not the slow cores (3x slower, too slow for 30 fps), not the prime core the game's main thread likes
    for (int i = 0; i < n; i++)
        if (fr[i] > minf && fr[i] < maxf) mask |= 1ul << i;
    if (!mask)
        for (int i = 0; i < n; i++)
            if (fr[i] > 0 && fr[i] < maxf) mask |= 1ul << i;
    std::string fp = std::string(som_data_dir()) + "/dsmod_affinity";
    if (FILE *f = fopen(fp.c_str(), "r")) { unsigned long m = 0; if (fscanf(f, "%lx", &m) == 1) mask = m; fclose(f); }
    if (mask) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int i = 0; i < n; i++)
            if (mask & (1ul << i)) CPU_SET(i, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
    setpriority(PRIO_PROCESS, 0, 10);
    if (mask_logged++ < 2) som_log("dsmod: %s thread on CPU mask 0x%lx, nice 10", what, mask);
}

double g_img_cpu; // image worker thread CPU seconds (written by the worker)
double thread_cpu() { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
struct ImgSlot { dsui::Image img; bool ready = false, failed = false; };
std::map<std::string, ImgSlot> g_images; // nodes are stable; a slot is written once, under g_img_mu, then only read
std::vector<std::string> g_want;
pthread_mutex_t g_img_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_img_cv = PTHREAD_COND_INITIALIZER;
std::atomic<uint64_t> g_img_loaded{0};
EdenDsmodHostApi g_whost; // the module's image loader runs off the tick thread with a host whose publish calls do nothing
bool g_worker_started;

void ui_image_sink(void *recv, uint32_t w, uint32_t h, const uint8_t *px, size_t size) {
    dsui::Image *s = (dsui::Image *)recv;
    if (size < (size_t)w * h * 4) return;
    s->w = (int)w;
    s->h = (int)h;
    s->rgba.assign(px, px + (size_t)w * h * 4);
}

void *image_worker(void *) {
    pthread_setname_np(pthread_self(), "dsmod-img");
    low_impact_thread("image worker");
    for (;;) {
        pthread_mutex_lock(&g_img_mu);
        while (g_want.empty()) pthread_cond_wait(&g_img_cv, &g_img_mu);
        std::string key = g_want.front();
        g_want.erase(g_want.begin());
        pthread_mutex_unlock(&g_img_mu);
        dsui::Image img;
        bool ok = g_ext && g_ext->load_image && g_ext->load_image(g_inst, &g_whost, key.c_str(), &img, ui_image_sink) && img.w > 0;
        pthread_mutex_lock(&g_img_mu);
        ImgSlot &slot = g_images[key];
        if (ok) { slot.img = std::move(img); slot.ready = true; }
        else slot.failed = true;
        pthread_mutex_unlock(&g_img_mu);
        g_img_loaded.fetch_add(1);
        g_img_cpu = thread_cpu();
    }
    return nullptr;
}

void start_image_worker() {
    if (g_worker_started) return;
    g_worker_started = true;
    g_whost = g_host;
    g_whost.publish_i64 = [](void *, const char *, int64_t) {};
    g_whost.publish_f64 = [](void *, const char *, double) {};
    g_whost.publish_text = [](void *, const char *, const char *) {};
    g_whost.publish_address = [](void *, const char *, uint64_t) {};
    g_whost.publish_map = [](void *, const EdenDsmodMapFrame *) {};
    g_whost.get_i64 = [](void *, const char *, int64_t fb) -> int64_t { return fb; };
    g_whost.get_f64 = [](void *, const char *, double fb) -> double { return fb; };
    g_whost.get_text = [](void *, const char *) -> const char * { return nullptr; };
    pthread_t t;
    pthread_create(&t, nullptr, image_worker, nullptr);
    pthread_detach(t);
}

dsui::FontMetrics g_fm;
std::vector<dsui::Op> g_last_ops;  // the page's ops as last laid out (touch hit tests)
std::vector<uint8_t> g_frame;      // latest rendered frame, RGBA8, canvas size
int g_frame_w, g_frame_h;
uint64_t g_frame_seq;
std::string g_frame_page;
double g_last_dump = -100;
bool g_throttled; // a frame is waiting for its 30 Hz slot
double g_wake_at;  // when (CLOCK_MONOTONIC seconds) the loop should run again to draw it
pthread_mutex_t g_frame_mu = PTHREAD_MUTEX_INITIALIZER; // guards g_frame, its size and g_frame_seq against the presenter thread
pthread_cond_t g_present_cv = PTHREAD_COND_INITIALIZER;
struct Stat { double sum = 0, max = 0; int n = 0; void add(double v) { sum += v; if (v > max) max = v; n++; } void reset() { *this = Stat(); } };
Stat g_st_module, g_st_render, g_st_layout, g_st_present;
int g_st_presented;
uint64_t g_img_seen;
bool g_dump_pending;

double mono_s() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

uint64_t hash_ops(const std::vector<dsui::Op> &ops, double page_alpha) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; };
    for (auto &o : ops) {
        int v[8] = {o.type, o.x, o.y, o.w, o.h, (int)o.color, o.scale, (int)(o.alpha * 255)};
        mix(v, sizeof v);
        mix(o.src.data(), o.src.size());
        mix(o.text.data(), o.text.size());
    }
    int pa = (int)(page_alpha * 255);
    mix(&pa, sizeof pa);
    return h;
}

void ui_init() {
    if (g_ui) return;
    std::string json;
    if (!read_file(g_pkg + "/manifest.json", json)) return;
    auto *h = new dsui::Host();
    std::string err;
    if (!h->load(json, err)) { som_log("dsmod: ui manifest: %s", err.c_str()); delete h; return; }
    som_log("dsmod: ui host loaded: %zu pages, %zu derived values, canvas %dx%d", h->man.pages.size(), h->man.derived.size(),
            h->man.canvas_w, h->man.canvas_h);
    g_ui = h;
}

void ui_frame() {
    g_throttled = false;
    if (!g_ui) return;
    if (g_dump_pending && dump_on() && mono_s() - g_last_dump > 3.0) { // spike check: the settled frame on disk
        g_last_dump = mono_s();
        g_dump_pending = false;
        write_png(std::string(som_data_dir()) + "/dsmod_dump/frame_" + g_frame_page + ".png", (uint32_t)g_frame_w, (uint32_t)g_frame_h, g_frame.data());
    }
    static uint64_t last_hash;
    static bool fonts_sent;
    double now = mono_s();
    // font -> ui structures once decoded
    if (g_font.ready && !fonts_sent && g_font_img.w) {
        g_fm.line_height = g_font.line_height;
        g_fm.first = g_font.first;
        g_fm.glyphs.resize(g_font.glyphs.size());
        for (size_t i = 0; i < g_font.glyphs.size(); i++) {
            const auto &s = g_font.glyphs[i];
            g_fm.glyphs[i] = {s.x, s.y, s.w, s.h, s.bearing_x, s.bearing_y, s.advance};
        }
        fonts_sent = true;
        last_hash = 0;
    }
    double l0 = mono_s();
    g_ui->update(now);
    std::vector<dsui::Op> ops;
    g_ui->layout(ops);
    g_last_ops = ops;
    g_st_layout.add((mono_s() - l0) * 1e3);
    uint64_t loaded_now = g_img_loaded.load();
    bool loaded = loaded_now != g_img_seen;
    g_img_seen = loaded_now;
    dsui::Resources res;
    size_t pending = 0;
    res.image = [&](const std::string &key) -> const dsui::Image * {
        const dsui::Image *r = nullptr;
        pthread_mutex_lock(&g_img_mu);
        auto it = g_images.find(key);
        if (it == g_images.end()) {
            g_images[key];
            g_want.push_back(key);
            pthread_cond_signal(&g_img_cv);
        } else if (it->second.ready) r = &it->second.img;
        pthread_mutex_unlock(&g_img_mu);
        return r;
    };
    res.font = fonts_sent ? &g_fm : nullptr;
    res.font_atlas = fonts_sent ? &g_font_img : nullptr;
    double fade = g_ui->page_alpha();
    static bool image_pending; // an image arrived since the last render
    if (loaded) image_pending = true;
    uint64_t hsh = hash_ops(ops, fade);
    if (hsh == last_hash && !image_pending) return;
    static double next_render = 0;
    if (now < next_render) { g_throttled = true; g_wake_at = next_render; return; } // 30 frames a second at most: wake exactly for the next slot
    next_render = (now - next_render > 0.0333 ? now : next_render) + 0.0333;
    last_hash = hsh;
    image_pending = false;
    std::vector<uint8_t> px;
    double r0 = mono_s();
    dsui::render(g_ui->man, ops, g_ui->man.canvas_w, g_ui->man.canvas_h, px, res, fade);
    double r1 = mono_s();
    g_st_render.add((r1 - r0) * 1e3);
    pthread_mutex_lock(&g_frame_mu);
    g_frame.swap(px);
    g_frame_w = g_ui->man.canvas_w;
    g_frame_h = g_ui->man.canvas_h;
    g_frame_seq++;
    pthread_cond_signal(&g_present_cv);
    pthread_mutex_unlock(&g_frame_mu);
    g_frame_page = g_ui->current_page;
    static int logged;
    if (logged++ < 12) som_log("dsmod: ui frame #%llu page '%s': %zu ops, render %.1f ms, %zu images cached", (unsigned long long)g_frame_seq,
                               g_frame_page.c_str(), ops.size(), (r1 - r0) * 1e3, g_images.size());
    (void)pending;
    g_dump_pending = true;
}

// ---- presenter (the second display's surface) and touch ----
ANativeWindow *g_win;
pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_win_dirty;
uint64_t g_presented_seq;

struct TouchEv { int action; float x, y; };
std::vector<TouchEv> g_touch_q;
pthread_mutex_t g_touch_mu = PTHREAD_MUTEX_INITIALIZER;

// The presenter copies the newest frame into the second display's buffer on its own thread: dequeuing a buffer can wait
// for the display (up to a few ms), which must not delay the next frame's render.
void *presenter_thread(void *) {
    pthread_setname_np(pthread_self(), "dsmod-present");
    low_impact_thread("presenter");
    for (;;) {
        pthread_mutex_lock(&g_frame_mu);
        while (!g_win || g_frame.empty() || (g_presented_seq == g_frame_seq && !g_win_dirty)) pthread_cond_wait(&g_present_cv, &g_frame_mu);
        pthread_mutex_unlock(&g_frame_mu);
        double p0 = mono_s();
        pthread_mutex_lock(&g_win_mu);
        if (g_win) {
            ANativeWindow_Buffer b;
            if (ANativeWindow_lock(g_win, &b, nullptr) == 0) {
                pthread_mutex_lock(&g_frame_mu);
                if (b.format == WINDOW_FORMAT_RGBA_8888 && b.width >= g_frame_w && b.height >= g_frame_h && !g_frame.empty()) {
                    for (int y = 0; y < g_frame_h; y++)
                        memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4, &g_frame[(size_t)y * g_frame_w * 4], (size_t)g_frame_w * 4);
                } else {
                    static int n;
                    if (n++ < 3) som_log("dsmod: surface buffer %dx%d fmt %d does not fit the frame %dx%d", b.width, b.height, b.format, g_frame_w, g_frame_h);
                }
                g_presented_seq = g_frame_seq;
                g_win_dirty = false;
                pthread_mutex_unlock(&g_frame_mu);
                ANativeWindow_unlockAndPost(g_win);
                g_st_present.add((mono_s() - p0) * 1e3);
                g_st_presented++;
                static int logged;
                if (logged++ < 3) som_log("dsmod: frame #%llu presented to the second display", (unsigned long long)g_presented_seq);
            } else usleep(5000);
        }
        pthread_mutex_unlock(&g_win_mu);
    }
    return nullptr;
}

bool g_force_tick;

void run_action(const std::string &name, bool has_payload, int64_t payload) {
    auto it = g_ui->man.actions.find(name);
    static int logged;
    bool say = logged++ < 60;
    if (it == g_ui->man.actions.end()) { if (say) som_log("dsmod: tap: no action '%s'", name.c_str()); return; }
    const dsui::Action &a = it->second;
    if (!a.enabled_bind.empty() && g_ui->num(a.enabled_bind) == 0) { if (say) som_log("dsmod: tap '%s' refused (%s is 0)", name.c_str(), a.enabled_bind.c_str()); return; }
    if (a.kind == "module") {
        if (a.arg_payload && !has_payload) { if (say) som_log("dsmod: tap '%s' needs a gesture payload (not supported yet)", name.c_str()); return; }
        int64_t arg = a.arg_payload ? payload : a.argument;
        bool ok = g_ext && g_ext->on_action && g_ext->on_action(g_inst, a.action.c_str(), arg);
        if (ok) g_force_tick = true; // let the module publish the result now, not at the next 20 Hz tick
        if (say) som_log("dsmod: tap '%s' -> module action %s(%lld): %s", name.c_str(), a.action.c_str(), (long long)arg, ok ? "accepted" : "declined");
    } else if (a.kind == "flag") {
        g_ui->flags[a.flag] = a.value;
        if (say) som_log("dsmod: tap '%s' -> flag %s = %lld", name.c_str(), a.flag.c_str(), (long long)a.value);
    } else if (a.kind == "page") {
        if (a.page != "@settings") g_ui->current_page = a.page;
        if (say) som_log("dsmod: tap '%s' -> page %s", name.c_str(), a.page.c_str());
    } else if (say) som_log("dsmod: tap '%s': action kind '%s' not supported yet", name.c_str(), a.kind.c_str());
}

void tap_at(float x, float y) {
    for (size_t i = g_last_ops.size(); i-- > 0;) {
        const dsui::Op &o = g_last_ops[i];
        if (o.on_tap.empty() || o.w <= 0 || o.h <= 0) continue;
        if (x < o.x || y < o.y || x >= o.x + o.w || y >= o.y + o.h) continue;
        if (o.clip[2] > 0 && (x < o.clip[0] || y < o.clip[1] || x >= o.clip[0] + o.clip[2] || y >= o.clip[1] + o.clip[3])) continue;
        run_action(o.on_tap, o.has_payload, o.payload);
        return;
    }
}

// one finger: a tap, or a vertical drag of the scroll list it started on (with a fling when released)
struct Drag {
    bool down = false, scrolling = false;
    std::string region;
    int start_off = 0;
    float x0 = 0, y0 = 0, maxd = 0, last_y = 0;
    double t0 = 0, last_t = 0, vel = 0; // vel: px/s of the content (offset units)
} g_drag;
struct Fling { std::string region; double vel = 0; } g_fling;

int region_max(const std::string &id) {
    const dsui::Page *pg = g_ui->page();
    if (pg)
        for (auto &r : pg->scrolls)
            if (r.id == id) return g_ui->scroll_offset_max(r);
    return 0;
}

void set_offset(const std::string &id, double off) {
    int mx = region_max(id);
    g_ui->set_scroll(id, (int)std::lround(std::min<double>(std::max<double>(off, 0), mx)));
}

void process_touch() {
    std::vector<TouchEv> q;
    pthread_mutex_lock(&g_touch_mu);
    q.swap(g_touch_q);
    pthread_mutex_unlock(&g_touch_mu);
    for (auto &e : q) {
        float cx = e.x * g_ui->man.canvas_w, cy = e.y * g_ui->man.canvas_h;
        double now = mono_s();
        if (e.action == 0) {
            g_fling.vel = 0;
            g_drag = Drag();
            g_drag.down = true;
            g_drag.x0 = cx; g_drag.y0 = cy; g_drag.last_y = cy; g_drag.t0 = g_drag.last_t = now;
            if (const dsui::ScrollRegion *r = g_ui->scroll_at((int)cx, (int)cy)) {
                g_drag.region = r->id;
                g_drag.start_off = g_ui->scroll_offset(r->id);
            }
        } else if (e.action == 2 && g_drag.down) {
            g_drag.maxd = std::max(g_drag.maxd, std::max(std::fabs(cx - g_drag.x0), std::fabs(cy - g_drag.y0)));
            if (!g_drag.region.empty() && !g_drag.scrolling && std::fabs(cy - g_drag.y0) > 16) g_drag.scrolling = true;
            if (g_drag.scrolling) {
                set_offset(g_drag.region, g_drag.start_off - (cy - g_drag.y0));
                double dt = now - g_drag.last_t;
                if (dt > 0.001) { double v = -(cy - g_drag.last_y) / dt; g_drag.vel = g_drag.vel * 0.6 + v * 0.4; }
                g_drag.last_y = cy;
                g_drag.last_t = now;
            }
        } else if (e.action == 1 && g_drag.down) {
            if (g_drag.scrolling) {
                if (now - g_drag.last_t < 0.1 && std::fabs(g_drag.vel) > 120) { g_fling.region = g_drag.region; g_fling.vel = g_drag.vel; }
            } else if (g_drag.maxd < 24 && now - g_drag.t0 < 0.8) tap_at(g_drag.x0, g_drag.y0);
            g_drag.down = false;
        } else if (e.action == 3) g_drag = Drag();
    }
    // fling: exponential decay, stops at the ends
    static double last = 0;
    double now = mono_s(), dt = last > 0 ? std::min(0.05, now - last) : 0;
    last = now;
    if (std::fabs(g_fling.vel) > 15 && !g_fling.region.empty() && !g_drag.down) {
        double off = g_ui->scroll_offset(g_fling.region) + g_fling.vel * dt;
        int mx = region_max(g_fling.region);
        if (off <= 0 || off >= mx) g_fling.vel = 0;
        set_offset(g_fling.region, off);
        g_fling.vel *= std::pow(0.02, dt); // about 98% gone after a second
    } else g_fling.vel = 0;
}

void *tick_thread(void *) {
    pthread_setname_np(pthread_self(), "dsmod");
    low_impact_thread("companion");
    // wait for the guest to have mapped its heap before sampling
    for (int i = 0; i < 600; i++) { usleep(100000); if (g_ticks.load() > 0 || i > 20) break; }
    uint64_t n = 0, loop = 0;
    double last_tick = 0;
    for (;;) {
        if (g_throttled) { // sleep exactly until the next 30 Hz slot
            double dt = g_wake_at - mono_s();
            if (dt > 0.0005) { struct timespec ts; ts.tv_sec = (time_t)dt; ts.tv_nsec = (long)((dt - (double)(time_t)dt) * 1e9); nanosleep(&ts, nullptr); }
        } else usleep(8000); // touch, layout and the finger follow at ~100 Hz; the module ticks at 20 Hz
        loop++;
        double now = mono_s();
        if (g_force_tick || now - last_tick >= 0.05) {
            g_force_tick = false;
            last_tick = now;
            g_ticks.fetch_add(1);
            double m0 = mono_s();
            g_api->tick(g_inst, &g_host);
            g_api->sample(g_inst, &g_host);
            g_st_module.add((mono_s() - m0) * 1e3);
            n++;
            if (n % 20 == 0) dump_new_images();
            if (n % 10 == 0) try_font();
            if (n % 100 == 50 && dump_on()) { // fixture: every published value
                FILE *vf = fopen((std::string(som_data_dir()) + "/dsmod_dump/values.txt").c_str(), "w");
                if (vf) { for (auto &kv : g_pub) fprintf(vf, "dsmod: %s = %s\n", kv.first.c_str(), kv.second.c_str()); fclose(vf); }
            }
            if (n % 200 == 0) som_log("dsmod: %llu ticks, %zu published names", (unsigned long long)n, g_pub.size());
        }
        if (!g_win) { // no second display surface (not shown, or switched off in the sidebar): no layout, render or touch work
            g_throttled = false;
            usleep(40000);
            continue;
        }
        process_touch();
        static int autoscroll = -1; // test aid: <data>/dsmod_autoscroll makes the first scroll list move continuously
        if (autoscroll < 0) autoscroll = access((std::string(som_data_dir()) + "/dsmod_autoscroll").c_str(), F_OK) == 0;
        if (autoscroll && g_ui) {
            const dsui::Page *pg = g_ui->page();
            if (pg && !pg->scrolls.empty()) {
                double ph = std::fmod(now * 0.5, 2.0), f = ph < 1 ? ph : 2 - ph;
                set_offset(pg->scrolls[0].id, f * region_max(pg->scrolls[0].id));
            }
        }
        ui_frame();
        static double rep_t = 0, cpu0 = 0, icpu0 = 0;
        if (now - rep_t >= 5.0) {
            double cpu = thread_cpu();
            if (rep_t > 0)
                som_log("dsmod: perf %.1fs: module tick avg %.2f max %.2f ms (%d), layout avg %.2f max %.2f ms, render avg %.2f max %.2f ms (%d), "
                        "present avg %.2f max %.2f ms, %.1f frames/s to the display | cpu: ui thread %.0f%% image worker %.0f%% of one core",
                        now - rep_t, g_st_module.n ? g_st_module.sum / g_st_module.n : 0, g_st_module.max, g_st_module.n,
                        g_st_layout.n ? g_st_layout.sum / g_st_layout.n : 0, g_st_layout.max,
                        g_st_render.n ? g_st_render.sum / g_st_render.n : 0, g_st_render.max, g_st_render.n,
                        g_st_present.n ? g_st_present.sum / g_st_present.n : 0, g_st_present.max, g_st_presented / (now - rep_t),
                        (cpu - cpu0) / (now - rep_t) * 100, (g_img_cpu - icpu0) / (now - rep_t) * 100);
            if (rep_t > 0)
                som_log("dsmod: module calls per second: is_mapped %.0f read %.0f (%.0f failed, %.1f MB) pointer %.0f publish %.0f; %.1f ms/s inside host callbacks",
                        g_c_mapped.exchange(0) / (now - rep_t), g_c_read.exchange(0) / (now - rep_t), g_c_fail.exchange(0) / (now - rep_t),
                        g_c_bytes.exchange(0) / 1e6 / (now - rep_t), g_c_ptr.exchange(0) / (now - rep_t), g_c_publish.exchange(0) / (now - rep_t),
                        g_t_cb_ns.exchange(0) / 1e6 / (now - rep_t));
            rep_t = now; cpu0 = cpu; icpu0 = g_img_cpu;
            g_st_module.reset(); g_st_render.reset(); g_st_layout.reset(); g_st_present.reset(); g_st_presented = 0;
        }
    }
    return nullptr;
}

} // namespace

extern "C" void dsmod_start(void) {
    if (g_pkg.empty()) return;
    std::string title = "0100A6301214E000";
    std::string so = g_pkg + "/modules/android-arm64-v8a/" + title + ".so";
    void *h = open_module(so);
    if (!h) return;
    auto get = (EdenDsmodGetModuleFn)dlsym(h, "eden_dsmod_get_module");
    if (!get) { som_log("dsmod: module has no eden_dsmod_get_module"); return; }
    g_api = get(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    if (!g_api || g_api->struct_size != sizeof(EdenDsmodModuleApi)) {
        som_log("dsmod: module refused the host ABI (api=%p size=%u want %zu)", (const void *)g_api,
                g_api ? g_api->struct_size : 0, sizeof(EdenDsmodModuleApi));
        return;
    }
    std::string bid = hex(g_build_id, 32);
    som_log("dsmod: module \"%s\" caps=0x%llx", g_api->name ? g_api->name : "?", (unsigned long long)g_api->capabilities);
    if (!g_api->supports_build(bid.c_str())) { som_log("dsmod: module rejects build %s", bid.c_str()); return; }

    memset(&g_host, 0, sizeof g_host);
    g_host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    g_host.struct_size = sizeof g_host;
    g_host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    g_host.capabilities = EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_MAP_OUTPUT | EDEN_DSMOD_CAP_EXTENSIONS |
                          EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_FONT_EPOCH |
                          EDEN_DSMOD_CAP_SOURCE_PREFIXES;
    g_host.title_id = 0x0100A6301214E000ull;
    memcpy(g_host.build_id, g_build_id, 32);
    g_host.main_base = g_main_base;
    g_host.main_size = g_main_size;
    g_host.get_tick = host_tick;
    g_host.get_heap_begin = host_heap_begin;
    g_host.get_heap_end = host_heap_end;
    g_host.is_mapped = host_is_mapped;
    g_host.read_memory = host_read;
    g_host.get_read_pointer = host_ptr;
    g_host.log = host_log;
    g_host.begin_output = host_begin;
    g_host.end_output = host_begin;
    g_host.publish_i64 = host_pi64;
    g_host.publish_f64 = host_pf64;
    g_host.publish_text = host_ptext;
    g_host.publish_address = host_paddr;
    g_host.publish_map = host_pmap;
    g_host.read_romfs = host_romfs;
    g_host.get_i64 = host_geti;
    g_host.get_f64 = host_getf;
    g_host.get_text = host_gettext;
    if (g_api->capabilities & ~g_host.capabilities) {
        som_log("dsmod: module needs capabilities the spike host lacks: 0x%llx",
                (unsigned long long)(g_api->capabilities & ~g_host.capabilities));
        return;
    }
    std::string cfg;
    read_file(g_pkg + "/manifest.json", cfg);
    // the module reads its config; hand it the real manifest plus the two keys Eden adds
    if (cfg.size() > 2 && cfg[cfg.size() - 1] != '}') { while (!cfg.empty() && cfg.back() != '}') cfg.pop_back(); }
    if (!cfg.empty() && cfg.back() == '}') {
        cfg.pop_back();
        cfg += ",\"_build_match\":true,\"_build_id\":\"" + bid + "\"}";
    } else cfg = "{}";
    ui_init();
    g_inst = g_api->create(&g_host, cfg.c_str());
    if (!g_inst) { som_log("dsmod: module create() failed"); return; }
    auto getext = (EdenDsmodGetExtensionsFn)dlsym(h, "eden_dsmod_get_extensions");
    if (getext) g_ext = getext(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    if (g_ext && g_ext->configure) {
        memset(&g_hext, 0, sizeof g_hext);
        g_hext.version = EDEN_DSMOD_EXT_VERSION;
        g_hext.struct_size = sizeof g_hext;
        g_hext.abi_hash = EDEN_DSMOD_EXT_HASH;
        g_hext.mailbox_address = g_mailbox;
        g_hext.mailbox_size = kMailboxSize;
        g_hext.load_u32 = mb_l32;
        g_hext.load_u64 = mb_l64;
        g_hext.store_u32 = mb_s32;
        g_hext.store_u64 = mb_s64;
        g_hext.decode_astc = host_astc;
        g_ext->configure(g_inst, &g_hext);
    }
    {
        auto gf = (EdenDsmodGetFontExtensionsFn)dlsym(h, "eden_dsmod_get_font_extensions");
        if (gf) g_fext = gf(EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH);
    }
    som_log("dsmod: module created (extensions %s, load plan %s); ticking at 10 Hz", g_ext ? "yes" : "no",
            g_plan_applied ? "applied" : "NOT applied");
    start_image_worker();
    { pthread_t pt; pthread_create(&pt, nullptr, presenter_thread, nullptr); pthread_detach(pt); }
    pthread_t t;
    pthread_create(&t, nullptr, tick_thread, nullptr);
    pthread_detach(t);
}


extern "C" JNIEXPORT void JNICALL Java_dev_teton_somniel_DualScreen_nativeSurface(JNIEnv *env, jclass, jobject surface) {
    pthread_mutex_lock(&g_win_mu);
    if (g_win) { ANativeWindow_release(g_win); g_win = nullptr; }
    if (surface) {
        g_win = ANativeWindow_fromSurface(env, surface);
        if (g_win) {
            ANativeWindow_setBuffersGeometry(g_win, g_ui ? g_ui->man.canvas_w : 1240, g_ui ? g_ui->man.canvas_h : 1080, WINDOW_FORMAT_RGBA_8888);
            pthread_mutex_lock(&g_frame_mu);
            g_win_dirty = true;
            pthread_cond_signal(&g_present_cv);
            pthread_mutex_unlock(&g_frame_mu);
        }
    }
    pthread_mutex_unlock(&g_win_mu);
    som_log("dsmod: second display surface %s", surface ? "attached" : "detached");
}

extern "C" JNIEXPORT void JNICALL Java_dev_teton_somniel_DualScreen_nativeTouch(JNIEnv *, jclass, jint action, jfloat nx, jfloat ny) {
    pthread_mutex_lock(&g_touch_mu);
    if (g_touch_q.size() < 256) g_touch_q.push_back({(int)action, nx, ny});
    pthread_mutex_unlock(&g_touch_mu);
}
