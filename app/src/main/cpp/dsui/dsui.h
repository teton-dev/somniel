// Dual-screen companion host core: manifest, published values, derived expressions, page binds, widget layout.
// Portable C++ (no Android, no Vulkan): it is built on the PC for tests (tools/dsui_test) and into libsomniel.
// It consumes the Eden Duo manifest format (format 1, runtime 22) that companion packages ship; the code here is
// written for Somniel from the manifest's behaviour, not taken from Eden's GPL runtime.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dsui {

// ---- published values -------------------------------------------------------------------------------------------
struct Val {
    enum K { Int, Float, Text } k = Int;
    int64_t i = 0;
    double f = 0;
    std::string s;
};

class Values {
public:
    void set_i(const std::string &n, int64_t v) { auto &x = m[n]; x.k = Val::Int; x.i = v; x.f = (double)v; }
    void set_f(const std::string &n, double v) { auto &x = m[n]; x.k = Val::Float; x.f = v; x.i = (int64_t)v; }
    void set_t(const std::string &n, const std::string &v) { auto &x = m[n]; x.k = Val::Text; x.s = v; x.i = 0; x.f = 0; }
    bool has(const std::string &n) const { return m.count(n) != 0; }
    double num(const std::string &n, double def = 0) const {
        auto it = m.find(n);
        if (it == m.end() || it->second.k == Val::Text) return def;
        return it->second.k == Val::Float ? it->second.f : (double)it->second.i;
    }
    const std::string *text(const std::string &n) const {
        auto it = m.find(n);
        return it != m.end() && it->second.k == Val::Text ? &it->second.s : nullptr;
    }
    std::unordered_map<std::string, Val> m;
};

// ---- expressions ------------------------------------------------------------------------------------------------
struct Expr; // compiled expression tree (dsui.cpp)

struct Derived {
    std::string name;
    enum Kind { Expression, Compare, AllNonzero } kind = Expression;
    std::shared_ptr<Expr> expr;
    std::string a; // Compare
    double b = 0;
    int cmp = 0;   // 0 eq, 1 ne, 2 lt, 3 le, 4 gt, 5 ge
    std::vector<std::string> all;
};

// ---- manifest ---------------------------------------------------------------------------------------------------
struct Anim {
    std::string group, bind, from = "fade", easing = "ease_out";
    int ms = 200;
};

struct Widget {
    enum Type { Label, Image, Value, Rect } type = Label;
    int rect[4] = {0, 0, 0, 0};
    std::string need_bind, hide_bind, bind, bind_text, text, src, src_bind, max_bind, max_sep, suffix, x_bind, y_bind;
    std::string repeat_bind, scroll, id, on_tap, payload;
    std::string keep_min_s, keep_max_s; // when "{i}", "{i}+N", "{i}-N"
    double keep_min = -1e18, keep_max = 1e18;
    bool keep_min_i = false, keep_max_i = false;
    int repeat = 0, repeat_dx = 0, repeat_dy = 0, repeat_cols = 0, repeat_row_dy = 0;
    float x_scale = 1.0f, y_scale = 1.0f;
    uint32_t color = 0xFFE6ECF2u, bg = 0;
    int frame = 2, text_scale = 3, align = 0, wrap_width = 0, max_lines = 0, text_min_scale = 1, line_gap = -1;
    bool fit_text = false, color_markup = false;
    std::vector<std::string> names;
    int pad = 0, div = 1, mul = 1, add = 0;
    bool group = false;
    std::unique_ptr<Anim> anim;
    bool pan_zoom = false;
    std::string on_pan, on_pinch, on_swipe_left, on_swipe_right;
    int swipe_px = 0;
    Widget() = default;
    Widget(Widget &&) = default;
    Widget &operator=(Widget &&) = default;
};

struct ScrollRegion {
    std::string id, count_bind;
    int rect[4] = {0, 0, 0, 0};
    int row_h = 0, cols = 1;
    std::string show_bind;
    uint32_t bar = 0, bar_track = 0;
    int bar_w = 6;
};

struct Page {
    std::string id, title;
    std::vector<Widget> widgets;
    std::vector<ScrollRegion> scrolls;
};

struct Action {
    std::string name, kind, action, enabled_bind, flag, page, view, haptic;
    bool arg_payload = false;
    int64_t argument = 0;
    int64_t value = 0;
};

struct PageBind {
    std::string point, ready_bind;
    double equals = 0;
    std::string page_equal, page_not_equal;
    int duration_ms = 200;
};

struct Manifest {
    std::string name, title_id, font_atlas, background_s;
    int canvas_w = 1240, canvas_h = 1080;
    uint32_t background = 0xFF000000u;
    std::map<std::string, int64_t> flag_defaults;
    std::vector<Derived> derived;
    std::map<std::string, Action> actions;
    std::vector<PageBind> page_binds;
    std::vector<Page> pages;
    std::vector<std::string> persist_flags;
};

bool parse_manifest(const std::string &json, Manifest &out, std::string &err);

// ---- glyph metrics (from the module's decode_font) ---------------------------------------------------------------
struct Glyph {
    uint16_t x = 0, y = 0, w = 0, h = 0;
    int16_t bx = 0, by = 0;
    uint16_t adv = 0;
};
struct FontMetrics {
    uint32_t line_height = 32, first = 32;
    std::vector<Glyph> glyphs;
    const Glyph *find(uint32_t cp) const {
        if (cp < first || cp - first >= glyphs.size()) return nullptr;
        const Glyph &g = glyphs[cp - first];
        return g.adv || g.w ? &g : nullptr;
    }
    bool ready() const { return !glyphs.empty(); }
};

// ---- layout output ------------------------------------------------------------------------------------------------
struct Op {
    enum Type { FillRect, Outline, Image, Text, Hit } type = FillRect; // Hit: an undrawn tap zone
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0xFFFFFFFFu;
    int frame = 0;
    float alpha = 1.0f;
    int clip[4] = {0, 0, 0, 0}; // x, y, w, h; w == 0: none
    std::string src;            // Image key
    std::string text;           // Text
    int scale = 3, align = 0, wrap_width = 0, max_lines = 0, min_scale = 1, line_gap = -1;
    bool fit = false, markup = false;
    int widget = -1;            // index in the page, for hit tests
    bool has_payload = false;   // a tapped repeat row passes its index
    int64_t payload = 0;
    std::string on_tap;
};

struct Host {
    Manifest man;
    Values values;
    std::map<std::string, int64_t> flags;
    std::string current_page;
    double page_t0 = -1e9; // time the current page was entered, for the fade-in
    int page_fade_ms = 0;

    bool load(const std::string &manifest_json, std::string &err);
    // once per frame: clears the derived cache, drives page binds and widget animations. `now` is in seconds.
    void update(double now);
    void layout(std::vector<Op> &out);
    const Page *page() const;
    double num(const std::string &name);                 // a point, derived value or "@flag:x"
    bool gate(const std::string &name) { return name.empty() || num(name) != 0; }
    std::string text_of(const std::string &name);        // a published text point ("" if absent)
    double page_alpha() const;                            // 0..1 fade-in of the current page
    void set_scroll(const std::string &id, int offset) { scroll_off[id] = offset; }
    int scroll_content_height(const ScrollRegion &r);
    int scroll_offset(const std::string &id) const { auto it = scroll_off.find(id); return it == scroll_off.end() ? 0 : it->second; }
    int scroll_offset_max(const ScrollRegion &r);
    const ScrollRegion *scroll_at(int x, int y); // the open scroll region of the current page under a point

private:
    struct DState { bool armed = false, last_equal = false; };
    std::vector<DState> bind_state;
    struct AnimState { bool init = false; double last = 0, t0 = -1e9; int ms = 200; std::string bind; std::string easing; };
    std::map<std::string, AnimState> anim;
    std::map<std::string, int> scroll_off;
    std::unordered_map<std::string, int> derived_index;
    std::unordered_map<std::string, double> memo;
    int depth_ = 0;
    double now_ = 0;
    double eval_derived(int idx);
    double anim_alpha(const Anim &a);
    friend struct LayoutCtx;
};

} // namespace dsui
