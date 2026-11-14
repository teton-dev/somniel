// PC harness for the dual-screen host core: loads a manifest and a values dump (lines "dsmod: name = value" from
// somniel.log), lays out a page and renders it to a PNG, with decoded images from a directory of PNGs written by the
// device spike (dsmod_dump). Build: tools/dsui_test/build.sh
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "../../app/src/main/cpp/dsui/dsraster.h"
#include "../../app/src/main/cpp/dsui/dsui.h"

using namespace dsui;

static std::string slurp(const char *p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- tiny PNG reader/writer (stored deflate blocks only: what the device spike writes) ----
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static bool read_png(const std::string &path, Image &im) {
    std::string d = slurp(path.c_str());
    if (d.size() < 33) return false;
    const uint8_t *b = (const uint8_t *)d.data();
    size_t p = 8;
    std::string z;
    while (p + 12 <= d.size()) {
        uint32_t n = be32(b + p);
        if (!memcmp(b + p + 4, "IHDR", 4)) { im.w = (int)be32(b + p + 8); im.h = (int)be32(b + p + 12); }
        if (!memcmp(b + p + 4, "IDAT", 4)) z.append((const char *)b + p + 8, n);
        p += 12 + n;
    }
    if (z.size() < 6) return false;
    std::string raw;
    size_t q = 2;
    for (;;) {
        if (q + 5 > z.size()) return false;
        bool fin = z[q] & 1;
        uint16_t n = (uint8_t)z[q + 1] | (uint8_t)z[q + 2] << 8;
        raw.append(z, q + 5, n);
        q += 5 + n;
        if (fin) break;
    }
    size_t stride = (size_t)im.w * 4 + 1;
    if (raw.size() < stride * im.h) return false;
    im.rgba.resize((size_t)im.w * im.h * 4);
    for (int y = 0; y < im.h; y++) memcpy(&im.rgba[(size_t)y * im.w * 4], raw.data() + y * stride + 1, (size_t)im.w * 4);
    return true;
}

static uint32_t crc_t[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_t[i] = x; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_t[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void w32(std::string &s, uint32_t v) { for (int i = 3; i >= 0; i--) s += (char)(v >> (i * 8)); }
static void chunk(std::string &out, const char *t, const std::string &d) {
    w32(out, (uint32_t)d.size());
    std::string td = std::string(t, 4) + d;
    out += td;
    w32(out, crc(0, (const uint8_t *)td.data(), td.size()));
}
static void write_png(const char *path, int w, int h, const std::vector<uint8_t> &rgba) {
    std::string out("\x89PNG\r\n\x1a\n", 8), ih;
    w32(ih, w); w32(ih, h); ih += std::string("\x08\x06\x00\x00\x00", 5);
    chunk(out, "IHDR", ih);
    std::string raw;
    for (int y = 0; y < h; y++) { raw += (char)0; raw.append((const char *)&rgba[(size_t)y * w * 4], (size_t)w * 4); }
    std::string z("\x78\x01", 2);
    for (size_t o = 0; o < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - o);
        z += (char)(o + n >= raw.size()); z += (char)(n & 255); z += (char)(n >> 8); z += (char)(~n & 255); z += (char)((~n >> 8) & 255);
        z.append(raw, o, n);
        o += n;
    }
    uint32_t a = 1, bb = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; bb = (bb + a) % 65521; }
    w32(z, (bb << 16) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", "");
    std::ofstream(path, std::ios::binary).write(out.data(), (std::streamsize)out.size());
}

static std::string sanitize(const std::string &k) {
    std::string n;
    for (char c : k) n += isalnum((unsigned char)c) ? c : '_';
    if (n.size() > 90) n = n.substr(n.size() - 90);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: dsui_test manifest.json values.log imgdir page out.png [now]\n"); return 2; }
    Host h;
    std::string err;
    if (!h.load(slurp(argv[1]), err)) { fprintf(stderr, "manifest: %s\n", err.c_str()); return 1; }
    printf("manifest: %zu pages, %zu derived, %zu actions, %zu page binds\n", h.man.pages.size(), h.man.derived.size(), h.man.actions.size(), h.man.page_binds.size());
    // values from the log
    std::ifstream f(argv[2]);
    std::string line;
    int nv = 0;
    while (std::getline(f, line)) {
        if (line.compare(0, 7, "dsmod: ") != 0) continue;
        size_t eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        std::string name = line.substr(7, eq - 7), val = line.substr(eq + 3);
        if (name.find(' ') != std::string::npos) continue;
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
        if (!val.empty() && val[0] == '"') h.values.set_t(name, val.substr(1, val.size() - 2));
        else if (val.find('.') != std::string::npos && val.compare(0, 2, "0x") != 0) h.values.set_f(name, atof(val.c_str()));
        else h.values.set_i(name, (int64_t)strtoll(val.c_str(), nullptr, 0));
        nv++;
    }
    printf("values: %d\n", nv);
    // images
    std::map<std::string, std::string> files; // sanitized key -> path
    std::string cmd = std::string("ls \"") + argv[3] + "\"";
    FILE *ls = popen(cmd.c_str(), "r");
    char buf[512];
    while (ls && fgets(buf, sizeof buf, ls)) {
        std::string n = buf;
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".png") == 0 && n.size() > 3 && isdigit((unsigned char)n[0])) files[n.substr(3, n.size() - 7)] = std::string(argv[3]) + "/" + n;
    }
    if (ls) pclose(ls);
    std::map<std::string, Image> cache;
    int missing = 0;
    std::map<std::string, int> miss;
    Resources res;
    res.image = [&](const std::string &key) -> const Image * {
        auto c = cache.find(key);
        if (c != cache.end()) return c->second.w ? &c->second : nullptr;
        Image im;
        auto it = files.find(sanitize(key));
        if (it != files.end()) read_png(it->second, im);
        if (!im.w) { miss[key]++; missing++; }
        auto &slot = cache[key];
        slot = std::move(im);
        return slot.w ? &slot : nullptr;
    };
    h.current_page = argv[4];
    double now = argc > 6 ? atof(argv[6]) : 100.0;
    h.update(now);
    std::vector<Op> ops;
    h.layout(ops);
    int nt = 0, ni = 0, nr = 0;
    for (auto &o : ops) (o.type == Op::Text ? nt : o.type == Op::Image ? ni : nr)++;
    printf("page '%s': %zu ops (%d text, %d image, %d rect)\n", h.current_page.c_str(), ops.size(), nt, ni, nr);
    std::vector<uint8_t> px;
    render(h.man, ops, h.man.canvas_w, h.man.canvas_h, px, res, 1.0); // warm: loads images
    auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 20; k++) render(h.man, ops, h.man.canvas_w, h.man.canvas_h, px, res, 1.0);
    printf("render: %.2f ms per frame (PC)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20);
    write_png(argv[5], h.man.canvas_w, h.man.canvas_h, px);
    printf("wrote %s; %d image lookups missed (%zu keys)\n", argv[5], missing, miss.size());
    int shown = 0;
    for (auto &kv : miss) if (shown++ < 12) printf("  missing %s\n", kv.first.c_str());
    for (auto &o : ops) if (o.type == Op::Text && shown++ < 40) printf("  text '%s' at %d,%d scale %d\n", o.text.c_str(), o.x, o.y, o.scale);
    return 0;
}
