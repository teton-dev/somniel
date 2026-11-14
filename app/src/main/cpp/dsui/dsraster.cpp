#include "dsraster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace dsui {

namespace {

struct Clip { int x0, y0, x1, y1; };

inline void blend8(uint8_t *d, uint32_t argb, int a) { // a: 0..255 effective alpha
    if (a <= 0) return;
    if (a >= 255) {
        d[0] = (uint8_t)(argb >> 16); d[1] = (uint8_t)(argb >> 8); d[2] = (uint8_t)argb; d[3] = 255;
        return;
    }
    int ia = 255 - a;
    d[0] = (uint8_t)((d[0] * ia + ((argb >> 16) & 255) * a + 127) / 255);
    d[1] = (uint8_t)((d[1] * ia + ((argb >> 8) & 255) * a + 127) / 255);
    d[2] = (uint8_t)((d[2] * ia + (argb & 255) * a + 127) / 255);
    d[3] = 255;
}

inline int eff_alpha(uint32_t argb, float alpha) {
    int a = (int)(((argb >> 24) & 255) * alpha + 0.5f);
    return a > 255 ? 255 : a;
}

inline void blend(uint8_t *d, uint32_t argb, float alpha) { blend8(d, argb, eff_alpha(argb, alpha)); }

void fill(std::vector<uint8_t> &px, int W, int H, Clip c, int x, int y, int w, int h, uint32_t argb, float alpha) {
    int x0 = std::max(x, std::max(c.x0, 0)), y0 = std::max(y, std::max(c.y0, 0));
    int x1 = std::min(x + w, std::min(c.x1, W)), y1 = std::min(y + h, std::min(c.y1, H));
    int a = eff_alpha(argb, alpha);
    if (a <= 0 || x1 <= x0) return;
    uint32_t solid = 0xFF000000u | ((argb & 0xFF) << 16) | (argb & 0xFF00) | ((argb >> 16) & 0xFF); // RGBA bytes as a little-endian word
    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = &px[((size_t)yy * W + x0) * 4];
        if (a >= 255) { uint32_t *p = (uint32_t *)row; for (int xx = x0; xx < x1; xx++) *p++ = solid; }
        else for (int xx = x0; xx < x1; xx++, row += 4) blend8(row, argb, a);
    }
}

inline void sample(const Image &im, float u, float v, float out[4]) { // bilinear, clamped, straight alpha
    u -= 0.5f; v -= 0.5f;
    int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
    float fx = u - x0, fy = v - y0;
    for (int k = 0; k < 4; k++) out[k] = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int xx = std::min(std::max(x0 + i, 0), im.w - 1), yy = std::min(std::max(y0 + j, 0), im.h - 1);
            float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const uint8_t *p = &im.rgba[((size_t)yy * im.w + xx) * 4];
            float a = p[3] * (1.0f / 255.0f);
            out[0] += p[0] * a * wgt; out[1] += p[1] * a * wgt; out[2] += p[2] * a * wgt; out[3] += a * wgt;
        }
    if (out[3] > 0) { out[0] /= out[3]; out[1] /= out[3]; out[2] /= out[3]; }
}

void draw_image(std::vector<uint8_t> &px, int W, int H, Clip c, const Image &im, int x, int y, int w, int h, float alpha) {
    if (im.w <= 0 || im.h <= 0) return;
    if (w <= 0) w = im.w;
    if (h <= 0) h = im.h;
    int x0 = std::max(x, std::max(c.x0, 0)), y0 = std::max(y, std::max(c.y0, 0));
    int x1 = std::min(x + w, std::min(c.x1, W)), y1 = std::min(y + h, std::min(c.y1, H));
    if (x1 <= x0 || y1 <= y0) return;
    int ga = (int)(alpha * 256 + 0.5f);
    if (ga > 256) ga = 256;
    if (w == im.w && h == im.h) { // 1:1: no resampling
        bool opaque = im.is_opaque();
        for (int yy = y0; yy < y1; yy++) {
            const uint8_t *sp = &im.rgba[((size_t)(yy - y) * im.w + (x0 - x)) * 4];
            uint8_t *d = &px[((size_t)yy * W + x0) * 4];
            if (opaque && ga >= 256) { memcpy(d, sp, (size_t)(x1 - x0) * 4); continue; }
            for (int xx = x0; xx < x1; xx++, sp += 4, d += 4) {
                int a = (sp[3] * ga) >> 8;
                if (a <= 0) continue;
                if (a >= 255) { d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = 255; continue; }
                int ia = 255 - a;
                d[0] = (uint8_t)((d[0] * ia + sp[0] * a + 127) / 255);
                d[1] = (uint8_t)((d[1] * ia + sp[1] * a + 127) / 255);
                d[2] = (uint8_t)((d[2] * ia + sp[2] * a + 127) / 255);
                d[3] = 255;
            }
        }
        return;
    }
    // scaled: bilinear with the horizontal taps worked out once per column
    int n = x1 - x0;
    std::vector<int> xi((size_t)n), xf((size_t)n);
    float sx = (float)im.w / w, sy = (float)im.h / h;
    for (int i = 0; i < n; i++) {
        float u = (x0 + i - x + 0.5f) * sx - 0.5f;
        int u0 = (int)std::floor(u);
        xf[(size_t)i] = (int)((u - u0) * 256);
        xi[(size_t)i] = u0;
    }
    bool opaque = im.is_opaque();
    for (int yy = y0; yy < y1; yy++) {
        float v = (yy - y + 0.5f) * sy - 0.5f;
        int v0 = (int)std::floor(v), fy = (int)((v - v0) * 256);
        int ya = std::min(std::max(v0, 0), im.h - 1), yb = std::min(std::max(v0 + 1, 0), im.h - 1);
        const uint8_t *ra = &im.rgba[(size_t)ya * im.w * 4], *rb = &im.rgba[(size_t)yb * im.w * 4];
        uint8_t *d = &px[((size_t)yy * W + x0) * 4];
        for (int i = 0; i < n; i++, d += 4) {
            int xa = std::min(std::max(xi[(size_t)i], 0), im.w - 1), xb = std::min(std::max(xi[(size_t)i] + 1, 0), im.w - 1), fx = xf[(size_t)i];
            const uint8_t *p00 = ra + xa * 4, *p10 = ra + xb * 4, *p01 = rb + xa * 4, *p11 = rb + xb * 4;
            int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy; // sum 65536
            if (opaque) {
                int r = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11) >> 16;
                int g = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11) >> 16;
                int b = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11) >> 16;
                if (ga >= 256) { d[0] = (uint8_t)r; d[1] = (uint8_t)g; d[2] = (uint8_t)b; d[3] = 255; }
                else { int a = ga, ia = 256 - a; d[0] = (uint8_t)((d[0] * ia + r * a) >> 8); d[1] = (uint8_t)((d[1] * ia + g * a) >> 8); d[2] = (uint8_t)((d[2] * ia + b * a) >> 8); d[3] = 255; }
            } else { // premultiplied interpolation so transparent texels do not bleed their colour
                int a00 = p00[3] * w00, a10 = p10[3] * w10, a01 = p01[3] * w01, a11 = p11[3] * w11;
                int asum = (a00 + a10 + a01 + a11) >> 16; // 0..255
                if (asum <= 0) continue;
                int r = (p00[0] * a00 + p10[0] * a10 + p01[0] * a01 + p11[0] * a11) / (asum << 16 > 0 ? (a00 + a10 + a01 + a11) : 1);
                int g = (p00[1] * a00 + p10[1] * a10 + p01[1] * a01 + p11[1] * a11) / (a00 + a10 + a01 + a11);
                int b = (p00[2] * a00 + p10[2] * a10 + p01[2] * a01 + p11[2] * a11) / (a00 + a10 + a01 + a11);
                int a = (asum * ga) >> 8;
                if (a <= 0) continue;
                int ia = 255 - a;
                d[0] = (uint8_t)((d[0] * ia + r * a + 127) / 255);
                d[1] = (uint8_t)((d[1] * ia + g * a + 127) / 255);
                d[2] = (uint8_t)((d[2] * ia + b * a + 127) / 255);
                d[3] = 255;
            }
        }
    }
}

// ---- text ----------------------------------------------------------------------------------------------------------
struct Ch { uint32_t cp; uint32_t color; };

uint32_t next_cp(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> n);
    while (n-- > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

std::vector<Ch> decode(const std::string &s, uint32_t base, bool markup) {
    std::vector<Ch> out;
    uint32_t col = base;
    for (size_t i = 0; i < s.size();) {
        if (markup && s[i] == '{') {
            if (s.compare(i, 4, "{/c}") == 0) { col = base; i += 4; continue; }
            if (s.compare(i, 4, "{c:#") == 0) {
                size_t e = s.find('}', i);
                if (e != std::string::npos) {
                    uint32_t v = (uint32_t)strtoul(s.substr(i + 4, e - i - 4).c_str(), nullptr, 16);
                    col = (e - i - 4) <= 6 ? (v | 0xFF000000u) : v;
                    i = e + 1;
                    continue;
                }
            }
        }
        out.push_back({next_cp(s, i), col});
    }
    return out;
}

float advance_of(const Resources &r, uint32_t cp, float ratio, float wanted) {
    if (r.font) {
        if (const Glyph *g = r.font->find(cp)) return g->adv * ratio;
    }
    return wanted * 0.5f;
}

float run_width(const Resources &r, const std::vector<Ch> &t, size_t a, size_t b, int scale) {
    float wanted = scale * 5.0f, ratio = r.font ? wanted / r.font->line_height : 1.0f, w = 0;
    for (size_t i = a; i < b; i++) w += advance_of(r, t[i].cp, ratio, wanted);
    return w;
}

struct Line { size_t a, b; };

std::vector<Line> wrap(const Resources &r, const std::vector<Ch> &t, int scale, int width, int max_lines, bool &truncated) {
    std::vector<Line> lines;
    truncated = false;
    size_t start = 0;
    size_t i = 0, last_space = (size_t)-1;
    float w = 0, wanted = scale * 5.0f, ratio = r.font ? wanted / r.font->line_height : 1.0f;
    while (i < t.size()) {
        if (t[i].cp == '\n') { lines.push_back({start, i}); start = ++i; last_space = (size_t)-1; w = 0; continue; }
        float adv = advance_of(r, t[i].cp, ratio, wanted);
        if (t[i].cp == ' ') last_space = i;
        if (width > 0 && w + adv > width && i > start) {
            size_t brk = last_space != (size_t)-1 && last_space > start ? last_space : i;
            lines.push_back({start, brk});
            start = brk;
            while (start < t.size() && t[start].cp == ' ') start++;
            i = start; last_space = (size_t)-1; w = 0;
            continue;
        }
        w += adv;
        i++;
    }
    if (start < t.size() || lines.empty()) lines.push_back({start, t.size()});
    if (max_lines > 0 && (int)lines.size() > max_lines) { lines.resize((size_t)max_lines); truncated = true; }
    return lines;
}

// Glyphs are resampled from the atlas once per (codepoint, size) and kept as coverage bitmaps; drawing a glyph is then a
// masked blend, not a bilinear lookup per pixel per frame.
struct GlyphBmp { int w = 0, h = 0; std::vector<uint8_t> cov; };
std::unordered_map<uint64_t, GlyphBmp> g_glyph_cache;
const Image *g_glyph_atlas;

const GlyphBmp &glyph_bitmap(const Resources &r, uint32_t cp, const Glyph &g, float ratio) {
    if (g_glyph_atlas != r.font_atlas) { g_glyph_cache.clear(); g_glyph_atlas = r.font_atlas; }
    uint64_t key = ((uint64_t)cp << 20) | (uint64_t)(int)std::lround(ratio * 1000.0f);
    auto it = g_glyph_cache.find(key);
    if (it != g_glyph_cache.end()) return it->second;
    if (g_glyph_cache.size() > 6000) g_glyph_cache.clear();
    GlyphBmp &bm = g_glyph_cache[key];
    const Image &at = *r.font_atlas;
    bm.w = std::max(1, (int)std::lround(g.w * ratio));
    bm.h = std::max(1, (int)std::lround(g.h * ratio));
    bm.cov.resize((size_t)bm.w * bm.h);
    for (int yy = 0; yy < bm.h; yy++)
        for (int xx = 0; xx < bm.w; xx++) {
            float u = g.x + (xx + 0.5f) / ratio, v = g.y + (yy + 0.5f) / ratio;
            u = std::min(std::max(u, g.x + 0.0f), g.x + g.w - 0.01f);
            v = std::min(std::max(v, g.y + 0.0f), g.y + g.h - 0.01f);
            float s[4];
            sample(at, u, v, s);
            bm.cov[(size_t)yy * bm.w + xx] = (uint8_t)std::min(255.0f, s[3] * 255.0f + 0.5f);
        }
    return bm;
}

void draw_glyph(std::vector<uint8_t> &px, int W, int H, Clip c, const Resources &r, uint32_t cp, const Glyph &g, float gx, float gy, float ratio,
                uint32_t argb, float alpha) {
    const GlyphBmp &bm = glyph_bitmap(r, cp, g, ratio);
    int x0 = (int)std::lround(gx), y0 = (int)std::lround(gy);
    int cx0 = std::max(x0, std::max(c.x0, 0)), cy0 = std::max(y0, std::max(c.y0, 0));
    int cx1 = std::min(x0 + bm.w, std::min(c.x1, W)), cy1 = std::min(y0 + bm.h, std::min(c.y1, H));
    int base_a = eff_alpha(argb, alpha);
    if (base_a <= 0) return;
    for (int yy = cy0; yy < cy1; yy++) {
        const uint8_t *cv = &bm.cov[(size_t)(yy - y0) * bm.w + (cx0 - x0)];
        uint8_t *d = &px[((size_t)yy * W + cx0) * 4];
        for (int xx = cx0; xx < cx1; xx++, cv++, d += 4) {
            if (!*cv) continue;
            blend8(d, argb, (*cv * base_a + 127) / 255);
        }
    }
}

void draw_text(std::vector<uint8_t> &px, int W, int H, Clip c, const Resources &r, const Op &op, float page_alpha) {
    std::vector<Ch> t = decode(op.text, op.color, op.markup);
    if (t.empty()) return;
    int scale = std::max(1, op.scale);
    if (!r.font || !r.font_atlas || !r.font->ready()) { // no font yet: boxes show where the text will go
        bool tr = false;
        std::vector<Line> ls = wrap(r, t, scale, op.wrap_width, op.max_lines, tr);
        int pitch = scale * 5 + (op.line_gap >= 0 ? op.line_gap : scale * 3);
        for (size_t li = 0; li < ls.size(); li++) {
            float lw = run_width(r, t, ls[li].a, ls[li].b, scale);
            float pen = (float)op.x - (op.align == 1 ? lw / 2 : op.align == 2 ? lw : 0);
            for (size_t k = ls[li].a; k < ls[li].b; k++, pen += scale * 2.5f)
                if (t[k].cp != ' ') fill(px, W, H, c, (int)pen, op.y + (int)li * pitch, std::max(1, (int)(scale * 2)), scale * 5, (t[k].color & 0x00FFFFFFu) | 0x80000000u, op.alpha * page_alpha);
        }
        return;
    }
    bool trunc = false;
    std::vector<Line> lines = wrap(r, t, scale, op.wrap_width, op.max_lines, trunc);
    if (op.fit && op.wrap_width > 0) { // shrink a one-line label until it fits
        std::vector<Ch> whole = t;
        while (scale > std::max(1, op.min_scale) && run_width(r, whole, 0, whole.size(), scale) > op.wrap_width) scale--;
        lines = wrap(r, t, scale, op.wrap_width, op.max_lines, trunc);
    }
    float wanted = scale * 5.0f, ratio = wanted / r.font->line_height;
    int pitch = scale * 5 + (op.line_gap >= 0 ? op.line_gap : scale * 3);
    const Glyph *A = r.font->find('A');
    float baseline = A ? A->by * ratio : wanted;
    for (size_t li = 0; li < lines.size(); li++) {
        size_t a = lines[li].a, b = lines[li].b;
        std::vector<Ch> ln(t.begin() + (long)a, t.begin() + (long)b);
        if (li + 1 == lines.size() && trunc) { // ellipsis: drop characters until "..." fits the wrap width
            for (int k = 0; k < 3; k++) ln.push_back({'.', ln.empty() ? op.color : ln.back().color});
            while (op.wrap_width > 0 && ln.size() > 3 && run_width(r, ln, 0, ln.size(), scale) > op.wrap_width) ln.erase(ln.end() - 4);
        }
        while (!ln.empty() && ln.back().cp == ' ') ln.pop_back();
        float lw = run_width(r, ln, 0, ln.size(), scale);
        float pen = (float)op.x - (op.align == 1 ? lw / 2 : op.align == 2 ? lw : 0);
        float top = (float)(op.y + (int)li * pitch);
        for (auto &ch : ln) {
            if (const Glyph *g = r.font->find(ch.cp)) {
                if (g->w && g->h) draw_glyph(px, W, H, c, r, ch.cp, *g, pen + g->bx * ratio, top + baseline - g->by * ratio, ratio, ch.color, op.alpha * page_alpha);
                pen += g->adv * ratio;
            } else pen += wanted * 0.5f;
        }
    }
}

} // namespace

int measure_text(const Resources &res, const std::string &text, int scale, bool markup) {
    std::vector<Ch> t = decode(text, 0xFFFFFFFFu, markup);
    return (int)run_width(res, t, 0, t.size(), std::max(1, scale));
}

void render(const Manifest &man, const std::vector<Op> &ops, int W, int H, std::vector<uint8_t> &rgba, const Resources &res, double fade) {
    rgba.assign((size_t)W * H * 4, 0);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        rgba[i * 4 + 0] = (uint8_t)((man.background >> 16) & 255);
        rgba[i * 4 + 1] = (uint8_t)((man.background >> 8) & 255);
        rgba[i * 4 + 2] = (uint8_t)(man.background & 255);
        rgba[i * 4 + 3] = 255;
    }
    float pa = (float)fade;
    for (const Op &op : ops) {
        Clip c{0, 0, W, H};
        if (op.clip[2] > 0) c = {std::max(0, op.clip[0]), std::max(0, op.clip[1]), std::min(W, op.clip[0] + op.clip[2]), std::min(H, op.clip[1] + op.clip[3])};
        float a = op.alpha * pa;
        switch (op.type) {
        case Op::FillRect: fill(rgba, W, H, c, op.x, op.y, op.w, op.h, op.color, a); break;
        case Op::Outline: {
            int f = std::max(1, op.frame);
            fill(rgba, W, H, c, op.x, op.y, op.w, f, op.color, a);
            fill(rgba, W, H, c, op.x, op.y + op.h - f, op.w, f, op.color, a);
            fill(rgba, W, H, c, op.x, op.y + f, f, op.h - 2 * f, op.color, a);
            fill(rgba, W, H, c, op.x + op.w - f, op.y + f, f, op.h - 2 * f, op.color, a);
            break;
        }
        case Op::Image: {
            const Image *im = res.image ? res.image(op.src) : nullptr;
            if (im) draw_image(rgba, W, H, c, *im, op.x, op.y, op.w, op.h, a);
            break;
        }
        case Op::Text: draw_text(rgba, W, H, c, res, op, pa); break;
        case Op::Hit: break;
        }
    }
}

} // namespace dsui
