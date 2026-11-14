#include "dsui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dsjson.h"

namespace dsui {

// =====================================================================================================================
// Expressions: numbers, point names, @flag:x, ! - * / % + - < <= > >= == != && || ?:
// =====================================================================================================================
struct Expr {
    enum Op { Num, Var, Not, Neg, Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Cond } op = Num;
    double n = 0;
    std::string name;
    std::shared_ptr<Expr> a, b, c;
};

namespace {

struct Tok { enum T { End, Num, Id, Op } t = End; double n = 0; std::string s; };

struct Lexer {
    std::string s;
    size_t i = 0;
    explicit Lexer(const std::string &str) : s(str) {}
    Tok next() {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        Tok t;
        if (i >= s.size()) return t;
        char c = s[i];
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < s.size() && isdigit((unsigned char)s[i + 1]))) {
            char *end;
            t.n = strtod(s.c_str() + i, &end);
            i = (size_t)(end - s.c_str());
            t.t = Tok::Num;
            return t;
        }
        if (isalpha((unsigned char)c) || c == '_' || c == '@') {
            size_t j = i;
            bool at = c == '@';
            while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '.' || s[j] == '@' || s[j] == '{' || s[j] == '}' ||
                                    (at && s[j] == ':')))
                j++;
            t.t = Tok::Id;
            t.s = s.substr(i, j - i);
            i = j;
            return t;
        }
        static const char *two[] = {"&&", "||", "==", "!=", "<=", ">="};
        for (const char *o : two)
            if (s.compare(i, 2, o) == 0) { t.t = Tok::Op; t.s = o; i += 2; return t; }
        t.t = Tok::Op;
        t.s = std::string(1, c);
        i++;
        return t;
    }
};

struct Parser {
    Lexer lx;
    Tok cur;
    bool ok = true;
    explicit Parser(const std::string &s) : lx(s) { cur = lx.next(); }
    void adv() { cur = lx.next(); }
    bool isop(const char *o) const { return cur.t == Tok::Op && cur.s == o; }
    using P = std::shared_ptr<Expr>;
    P mk(Expr::Op op, P a = nullptr, P b = nullptr, P c = nullptr) {
        auto e = std::make_shared<Expr>();
        e->op = op; e->a = a; e->b = b; e->c = c;
        return e;
    }
    P cond() {
        P c = orx();
        if (isop("?")) {
            adv();
            P a = cond();
            if (!isop(":")) { ok = false; return c; }
            adv();
            P b = cond();
            return mk(Expr::Cond, c, a, b);
        }
        return c;
    }
    P orx() { P l = andx(); while (isop("||")) { adv(); l = mk(Expr::Or, l, andx()); } return l; }
    P andx() { P l = eq(); while (isop("&&")) { adv(); l = mk(Expr::And, l, eq()); } return l; }
    P eq() {
        P l = rel();
        for (;;) {
            if (isop("==")) { adv(); l = mk(Expr::Eq, l, rel()); }
            else if (isop("!=")) { adv(); l = mk(Expr::Ne, l, rel()); }
            else return l;
        }
    }
    P rel() {
        P l = add();
        for (;;) {
            if (isop("<")) { adv(); l = mk(Expr::Lt, l, add()); }
            else if (isop("<=")) { adv(); l = mk(Expr::Le, l, add()); }
            else if (isop(">")) { adv(); l = mk(Expr::Gt, l, add()); }
            else if (isop(">=")) { adv(); l = mk(Expr::Ge, l, add()); }
            else return l;
        }
    }
    P add() {
        P l = mul();
        for (;;) {
            if (isop("+")) { adv(); l = mk(Expr::Add, l, mul()); }
            else if (isop("-")) { adv(); l = mk(Expr::Sub, l, mul()); }
            else return l;
        }
    }
    P mul() {
        P l = un();
        for (;;) {
            if (isop("*")) { adv(); l = mk(Expr::Mul, l, un()); }
            else if (isop("/")) { adv(); l = mk(Expr::Div, l, un()); }
            else if (isop("%")) { adv(); l = mk(Expr::Mod, l, un()); }
            else return l;
        }
    }
    P un() {
        if (isop("!")) { adv(); return mk(Expr::Not, un()); }
        if (isop("-")) { adv(); return mk(Expr::Neg, un()); }
        return prim();
    }
    P prim() {
        if (cur.t == Tok::Num) { auto e = mk(Expr::Num); e->n = cur.n; adv(); return e; }
        if (cur.t == Tok::Id) { auto e = mk(Expr::Var); e->name = cur.s; adv(); return e; }
        if (isop("(")) {
            adv();
            P e = cond();
            if (!isop(")")) { ok = false; return e; }
            adv();
            return e;
        }
        ok = false;
        return mk(Expr::Num);
    }
};

double eval(const Expr &e, const std::function<double(const std::string &)> &var) {
    switch (e.op) {
    case Expr::Num: return e.n;
    case Expr::Var: return var(e.name);
    case Expr::Not: return eval(*e.a, var) == 0 ? 1 : 0;
    case Expr::Neg: return -eval(*e.a, var);
    case Expr::Add: return eval(*e.a, var) + eval(*e.b, var);
    case Expr::Sub: return eval(*e.a, var) - eval(*e.b, var);
    case Expr::Mul: return eval(*e.a, var) * eval(*e.b, var);
    case Expr::Div: { double d = eval(*e.b, var); return d == 0 ? 0 : eval(*e.a, var) / d; }
    case Expr::Mod: { double d = eval(*e.b, var); return d == 0 ? 0 : std::fmod(eval(*e.a, var), d); }
    case Expr::Lt: return eval(*e.a, var) < eval(*e.b, var);
    case Expr::Le: return eval(*e.a, var) <= eval(*e.b, var);
    case Expr::Gt: return eval(*e.a, var) > eval(*e.b, var);
    case Expr::Ge: return eval(*e.a, var) >= eval(*e.b, var);
    case Expr::Eq: return std::fabs(eval(*e.a, var) - eval(*e.b, var)) < 1e-9;
    case Expr::Ne: return std::fabs(eval(*e.a, var) - eval(*e.b, var)) >= 1e-9;
    case Expr::And: return eval(*e.a, var) != 0 && eval(*e.b, var) != 0;
    case Expr::Or: return eval(*e.a, var) != 0 || eval(*e.b, var) != 0;
    case Expr::Cond: return eval(*e.a, var) != 0 ? eval(*e.b, var) : eval(*e.c, var);
    }
    return 0;
}

// =====================================================================================================================
// Manifest parsing
// =====================================================================================================================
uint32_t parse_color(const std::string &s, uint32_t def) {
    if (s.size() < 2 || s[0] != '#') return def;
    uint32_t v = (uint32_t)strtoul(s.c_str() + 1, nullptr, 16);
    if (s.size() == 7) v |= 0xFF000000u;
    return v;
}

int parse_align(const std::string &s) { return s == "center" ? 1 : s == "right" ? 2 : 0; }

// "{i}", "{i}+N", "{i}-N" or a number
void parse_keep(const J *j, double &val, std::string &s, bool &is_i) {
    if (!j) return;
    if (j->t == J::Num) { val = j->n; return; }
    if (j->t == J::Str) {
        const std::string &t = j->s;
        if (t.compare(0, 3, "{i}") == 0) {
            is_i = true;
            val = t.size() > 3 ? atof(t.c_str() + 3) : 0; // "+N" / "-N" parse as signed
            return;
        }
        val = atof(t.c_str());
    }
}

void parse_widget(const J &j, Widget &w) {
    std::string t = j.str("type");
    w.type = t == "image" ? Widget::Image : t == "value" ? Widget::Value : t == "rect" ? Widget::Rect : Widget::Label;
    if (const J *r = j.get("rect"))
        for (size_t i = 0; i < 4 && i < r->a.size(); i++) w.rect[i] = (int)r->a[i].n;
    w.need_bind = j.str("need_bind");
    w.hide_bind = j.str("hide_bind");
    w.bind = j.str("bind");
    w.bind_text = j.str("bind_text");
    w.text = j.str("text");
    w.src = j.str("src");
    w.src_bind = j.str("src_bind");
    w.max_bind = j.str("max_bind");
    w.max_sep = j.str("max_sep");
    w.suffix = j.str("suffix");
    w.x_bind = j.str("x_bind");
    w.y_bind = j.str("y_bind");
    w.repeat_bind = j.str("repeat_bind");
    w.scroll = j.str("scroll");
    w.id = j.str("id");
    w.on_tap = j.str("on_tap");
    w.payload = j.str("payload");
    w.on_pan = j.str("on_pan");
    w.on_pinch = j.str("on_pinch");
    w.on_swipe_left = j.str("on_swipe_left");
    w.on_swipe_right = j.str("on_swipe_right");
    w.swipe_px = (int)j.num("swipe_px");
    parse_keep(j.get("keep_min"), w.keep_min, w.keep_min_s, w.keep_min_i);
    parse_keep(j.get("keep_max"), w.keep_max, w.keep_max_s, w.keep_max_i);
    w.repeat = (int)j.num("repeat");
    w.repeat_dx = (int)j.num("repeat_dx");
    w.repeat_dy = (int)j.num("repeat_dy");
    w.repeat_cols = (int)j.num("repeat_cols");
    w.repeat_row_dy = (int)j.num("repeat_row_dy");
    w.x_scale = (float)j.num("x_scale", 1.0);
    w.y_scale = (float)j.num("y_scale", 1.0);
    w.color = parse_color(j.str("color"), w.type == Widget::Rect ? 0u : 0xFFE6ECF2u);
    w.bg = parse_color(j.str("bg"), 0);
    w.frame = (int)j.num("frame", 2);
    w.text_scale = (int)j.num("text_scale", 3);
    w.align = parse_align(j.str("align"));
    w.wrap_width = (int)j.num("wrap_width");
    w.max_lines = (int)j.num("max_lines");
    w.text_min_scale = (int)j.num("text_min_scale", 1);
    w.line_gap = (int)j.num("line_spacing", j.num("line_gap", -1));
    w.fit_text = j.num("fit_text") != 0;
    w.color_markup = j.num("color_markup") != 0;
    w.pad = (int)j.num("pad");
    w.div = (int)j.num("div", 1);
    if (w.div == 0) w.div = 1;
    w.mul = (int)j.num("mul", 1);
    w.add = (int)j.num("add");
    w.group = j.num("group_digits") != 0 || j.num("group_sep_on") != 0;
    w.pan_zoom = j.num("pan_zoom") != 0;
    if (const J *n = j.get("names"))
        for (auto &e : n->a) w.names.push_back(e.s);
    if (const J *a = j.get("anim")) {
        w.anim.reset(new Anim());
        w.anim->group = a->str("group");
        w.anim->bind = a->str("bind");
        w.anim->from = a->str("from", "fade");
        w.anim->easing = a->str("easing", "ease_out");
        w.anim->ms = (int)a->num("ms", 200);
    }
}

} // namespace

bool parse_manifest(const std::string &json, Manifest &m, std::string &err) {
    J root;
    JsonParser p(json.data(), json.data() + json.size());
    if (!p.parse(root) || root.t != J::Obj) { err = "manifest.json does not parse"; return false; }
    m = Manifest();
    m.name = root.str("name");
    m.title_id = root.str("title_id");
    m.canvas_w = (int)root.num("canvas_w", 1240);
    m.canvas_h = (int)root.num("canvas_h", 1080);
    m.background = parse_color(root.str("background"), 0xFF000000u);
    m.font_atlas = root.str("font_atlas");
    if (const J *f = root.get("flags"))
        for (auto &kv : f->o) m.flag_defaults[kv.first] = (int64_t)kv.second.n;
    if (const J *d = root.get("derived")) {
        for (auto &e : d->a) {
            Derived dv;
            dv.name = e.str("name");
            if (e.has("expr")) {
                dv.kind = Derived::Expression;
                Parser ps(e.str("expr"));
                dv.expr = ps.cond();
                if (!ps.ok || ps.cur.t != Tok::End) { err = "bad expression in derived '" + dv.name + "': " + e.str("expr"); return false; }
            } else if (e.has("cmp")) {
                dv.kind = Derived::Compare;
                dv.a = e.str("a");
                dv.b = e.num("b");
                std::string c = e.str("cmp");
                dv.cmp = c == "eq" ? 0 : c == "ne" ? 1 : c == "lt" ? 2 : c == "le" ? 3 : c == "gt" ? 4 : 5;
            } else if (const J *an = e.get("all_nonzero")) {
                dv.kind = Derived::AllNonzero;
                for (auto &n : an->a) dv.all.push_back(n.s);
            } else continue;
            m.derived.push_back(std::move(dv));
        }
    }
    if (const J *a = root.get("actions")) {
        for (auto &kv : a->o) {
            Action ac;
            ac.name = kv.first;
            ac.kind = kv.second.str("kind");
            ac.action = kv.second.str("action");
            ac.enabled_bind = kv.second.str("enabled_bind");
            ac.flag = kv.second.str("flag");
            ac.page = kv.second.str("page");
            ac.view = kv.second.str("view");
            ac.haptic = kv.second.str("haptic");
            ac.value = (int64_t)kv.second.num("value");
            if (const J *arg = kv.second.get("argument")) {
                if (arg->t == J::Str) ac.arg_payload = true;
                else ac.argument = (int64_t)arg->n;
            }
            m.actions[ac.name] = ac;
        }
    }
    if (const J *b = root.get("page_binds")) {
        for (auto &e : b->a) {
            PageBind pb;
            pb.point = e.str("point");
            pb.equals = e.num("equals");
            pb.ready_bind = e.str("ready_bind");
            if (const J *we = e.get("when_equal")) { pb.page_equal = we->str("page"); pb.duration_ms = (int)we->num("duration_ms", 200); }
            if (const J *wn = e.get("when_not_equal")) pb.page_not_equal = wn->str("page");
            m.page_binds.push_back(pb);
        }
    }
    if (const J *pf = root.get("persist_flags"))
        for (auto &e : pf->a) m.persist_flags.push_back(e.s);
    if (const J *pg = root.get("pages")) {
        for (auto &pj : pg->a) {
            Page pgm;
            pgm.id = pj.str("id");
            pgm.title = pj.str("title");
            if (const J *ws = pj.get("widgets")) {
                for (auto &wj : ws->a) {
                    Widget w;
                    parse_widget(wj, w);
                    pgm.widgets.push_back(std::move(w));
                }
            }
            if (const J *sc = pj.get("scrolls")) {
                for (auto &s : sc->a) {
                    ScrollRegion r;
                    r.id = s.str("id");
                    r.count_bind = s.str("count_bind");
                    if (const J *rc = s.get("rect"))
                        for (size_t i = 0; i < 4 && i < rc->a.size(); i++) r.rect[i] = (int)rc->a[i].n;
                    r.row_h = (int)s.num("row_h");
                    r.cols = std::max(1, (int)s.num("cols", 1));
                    r.show_bind = s.str("show_bind");
                    r.bar = parse_color(s.str("bar"), 0);
                    r.bar_track = parse_color(s.str("bar_track"), 0);
                    r.bar_w = (int)s.num("bar_w", 6);
                    pgm.scrolls.push_back(r);
                }
            }
            m.pages.push_back(std::move(pgm));
        }
    }
    if (m.pages.empty()) { err = "manifest has no pages"; return false; }
    return true;
}

// =====================================================================================================================
// Host
// =====================================================================================================================
bool Host::load(const std::string &json, std::string &err) {
    if (!parse_manifest(json, man, err)) return false;
    derived_index.clear();
    for (size_t i = 0; i < man.derived.size(); i++) derived_index[man.derived[i].name] = (int)i;
    flags = man.flag_defaults;
    current_page = man.pages[0].id;
    bind_state.assign(man.page_binds.size(), DState());
    anim.clear();
    for (auto &p : man.pages)
        for (auto &w : p.widgets)
            if (w.anim && !w.anim->group.empty()) {
                AnimState &a = anim[w.anim->group];
                a.bind = w.anim->bind;
                a.ms = w.anim->ms;
                a.easing = w.anim->easing;
            }
    return true;
}

const Page *Host::page() const {
    for (auto &p : man.pages)
        if (p.id == current_page) return &p;
    return nullptr;
}

double Host::eval_derived(int idx) {
    const Derived &d = man.derived[idx];
    if (depth_ > 40) return 0;
    depth_++;
    double r = 0;
    switch (d.kind) {
    case Derived::Expression:
        r = eval(*d.expr, [this](const std::string &n) { return num(n); });
        break;
    case Derived::Compare: {
        double a = num(d.a);
        switch (d.cmp) {
        case 0: r = std::fabs(a - d.b) < 1e-9; break;
        case 1: r = std::fabs(a - d.b) >= 1e-9; break;
        case 2: r = a < d.b; break;
        case 3: r = a <= d.b; break;
        case 4: r = a > d.b; break;
        default: r = a >= d.b; break;
        }
        break;
    }
    case Derived::AllNonzero:
        r = 1;
        for (auto &n : d.all)
            if (num(n) == 0) { r = 0; break; }
        break;
    }
    depth_--;
    return r;
}

double Host::num(const std::string &name) {
    if (name.compare(0, 6, "@flag:") == 0) {
        auto it = flags.find(name.substr(6));
        return it == flags.end() ? 0 : (double)it->second;
    }
    auto di = derived_index.find(name);
    if (di != derived_index.end()) {
        auto mi = memo.find(name);
        if (mi != memo.end()) return mi->second;
        double v = eval_derived(di->second);
        memo[name] = v;
        return v;
    }
    return values.num(name, 0);
}

std::string Host::text_of(const std::string &name) {
    const std::string *s = values.text(name);
    return s ? *s : std::string();
}

double Host::page_alpha() const {
    if (page_fade_ms <= 0) return 1.0;
    double t = (now_ - page_t0) * 1000.0 / page_fade_ms;
    return t >= 1 ? 1.0 : t < 0 ? 0.0 : t;
}

static double ease(const std::string &e, double t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    if (e == "ease_in") return t * t;
    if (e == "linear") return t;
    return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
}

double Host::anim_alpha(const Anim &a) {
    auto it = anim.find(a.group);
    if (it == anim.end()) return 1.0;
    double t = (now_ - it->second.t0) * 1000.0 / std::max(1, it->second.ms);
    if (t >= 1) return 1.0;
    return ease(it->second.easing, t);
}

void Host::update(double now) {
    now_ = now;
    memo.clear();
    // page binds: edge triggered, armed by a first valid read (a package's first page stays until a real edge)
    for (size_t i = 0; i < man.page_binds.size(); i++) {
        const PageBind &b = man.page_binds[i];
        DState &st = bind_state[i];
        auto known = [&](const std::string &n) { return values.has(n) || derived_index.count(n) != 0; };
        bool ready = known(b.point) && (b.ready_bind.empty() || (known(b.ready_bind) && num(b.ready_bind) != 0));
        if (!ready) { st.armed = false; continue; }
        bool eq = std::fabs(num(b.point) - b.equals) < 0.5;
        if (!st.armed) { st.armed = true; st.last_equal = eq; continue; }
        if (eq == st.last_equal) continue;
        st.last_equal = eq;
        const std::string &target = eq ? b.page_equal : b.page_not_equal;
        if (target.empty() || target == current_page) continue;
        for (auto &p : man.pages)
            if (p.id == target) {
                current_page = target;
                page_t0 = now;
                page_fade_ms = b.duration_ms;
                break;
            }
    }
    for (auto &kv : anim) {
        AnimState &a = kv.second;
        double v = num(a.bind);
        if (!a.init) { a.init = true; a.last = v; a.t0 = -1e9; }
        else if (v != a.last) { a.last = v; a.t0 = now; }
    }
}

// =====================================================================================================================
// Layout
// =====================================================================================================================
namespace {

std::string subst(const std::string &s, int i) {
    if (s.find("{i}") == std::string::npos) return s;
    std::string out;
    char buf[16];
    snprintf(buf, sizeof buf, "%d", i);
    for (size_t k = 0; k < s.size();) {
        if (s.compare(k, 3, "{i}") == 0) { out += buf; k += 3; }
        else out += s[k++];
    }
    return out;
}

std::string group_digits(const std::string &d) {
    std::string out;
    for (size_t i = 0; i < d.size(); i++) {
        if (i > 0 && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return out;
}

} // namespace

int Host::scroll_content_height(const ScrollRegion &r) {
    int n = r.count_bind.empty() ? 0 : (int)num(r.count_bind);
    int rows = (std::max(0, n) + r.cols - 1) / r.cols;
    return rows * r.row_h;
}

int Host::scroll_offset_max(const ScrollRegion &r) {
    return std::max(0, scroll_content_height(r) - r.rect[3]);
}

const ScrollRegion *Host::scroll_at(int x, int y) {
    const Page *pg = page();
    if (!pg) return nullptr;
    for (auto &s : pg->scrolls) {
        if (!s.show_bind.empty() && num(s.show_bind) == 0) continue;
        if (x >= s.rect[0] && y >= s.rect[1] && x < s.rect[0] + s.rect[2] && y < s.rect[1] + s.rect[3]) return &s;
    }
    return nullptr;
}

void Host::layout(std::vector<Op> &out) {
    const Page *pg = page();
    if (!pg) return;
    for (size_t wi = 0; wi < pg->widgets.size(); wi++) {
        const Widget &w = pg->widgets[wi];
        if (!w.need_bind.empty() && w.need_bind.find("{i}") == std::string::npos && num(w.need_bind) == 0) continue;
        int count = 1;
        if (w.repeat > 0) {
            count = w.repeat;
            if (!w.repeat_bind.empty()) {
                int c = (int)num(w.repeat_bind);
                count = std::max(0, std::min(count, c));
            }
        }
        const ScrollRegion *sr = nullptr;
        int soff = 0;
        if (!w.scroll.empty())
            for (auto &s : pg->scrolls)
                if (s.id == w.scroll) { sr = &s; soff = scroll_offset(s.id); break; }
        float alpha = w.anim ? (float)anim_alpha(*w.anim) : 1.0f;
        for (int i = 0; i < count; i++) {
            if (!w.need_bind.empty() && w.need_bind.find("{i}") != std::string::npos && num(subst(w.need_bind, i)) == 0) continue;
            if (!w.hide_bind.empty()) {
                double v = num(subst(w.hide_bind, i));
                double lo = w.keep_min_i ? i + w.keep_min : w.keep_min, hi = w.keep_max_i ? i + w.keep_max : w.keep_max;
                if (v < lo || v > hi) continue;
            }
            int dx = 0, dy = 0;
            if (w.repeat > 0) {
                if (w.repeat_cols > 0) { dx = (i % w.repeat_cols) * w.repeat_dx; dy = (i / w.repeat_cols) * w.repeat_row_dy; }
                else { dx = i * w.repeat_dx; dy = i * w.repeat_dy; }
            }
            Op op;
            op.widget = (int)wi;
            op.alpha = alpha;
            op.on_tap = w.on_tap;
            if (w.payload == "{i}") { op.has_payload = true; op.payload = i; }
            int x = w.rect[0] + dx, y = w.rect[1] + dy;
            if (!w.x_bind.empty()) x += (int)std::lround(num(subst(w.x_bind, i)) * w.x_scale);
            if (!w.y_bind.empty()) y += (int)std::lround(num(subst(w.y_bind, i)) * w.y_scale);
            if (sr) {
                y -= soff;
                op.clip[0] = sr->rect[0]; op.clip[1] = sr->rect[1]; op.clip[2] = sr->rect[2]; op.clip[3] = sr->rect[3];
                if (y + w.rect[3] < sr->rect[1] - 400 || y > sr->rect[1] + sr->rect[3] + 400) continue;
            }
            op.x = x; op.y = y; op.w = w.rect[2]; op.h = w.rect[3];
            switch (w.type) {
            case Widget::Rect:
                if ((w.bg >> 24) != 0) { Op f = op; f.type = Op::FillRect; f.color = w.bg; out.push_back(f); }
                if (w.frame > 0 && (w.color >> 24) != 0) { Op f = op; f.type = Op::Outline; f.color = w.color; f.frame = w.frame; out.push_back(f); }
                break;
            case Widget::Image: {
                std::string key = w.src;
                if (!w.src_bind.empty()) key = text_of(subst(w.src_bind, i));
                if (key.empty()) break;
                op.type = Op::Image;
                op.src = key;
                op.color = 0xFFFFFFFFu;
                out.push_back(op);
                break;
            }
            case Widget::Label:
            case Widget::Value: {
                std::string txt;
                if (w.type == Widget::Label) {
                    if (!w.bind_text.empty()) txt = text_of(subst(w.bind_text, i));
                    else txt = w.text;
                } else {
                    std::string b = subst(w.bind, i);
                    if (!values.has(b) && derived_index.find(b) == derived_index.end()) break;
                    double raw = num(b);
                    std::string numtxt;
                    if (!w.names.empty()) {
                        int64_t k = (int64_t)std::llround(raw);
                        if (k < 0 || k >= (int64_t)w.names.size()) break;
                        numtxt = w.names[(size_t)k];
                    } else {
                        int64_t v = (int64_t)std::llround(raw) / w.div * w.mul + w.add;
                        char buf[32];
                        snprintf(buf, sizeof buf, "%lld", (long long)v);
                        numtxt = buf;
                        if (w.pad > (int)numtxt.size()) numtxt.insert(0, (size_t)w.pad - numtxt.size(), '0');
                        if (w.group) numtxt = group_digits(numtxt);
                        if (!w.max_bind.empty()) {
                            int64_t mv = (int64_t)std::llround(num(subst(w.max_bind, i)));
                            snprintf(buf, sizeof buf, "%lld", (long long)mv);
                            numtxt += w.max_sep + buf;
                        }
                    }
                    txt = w.text + numtxt + w.suffix;
                }
                if (txt.empty()) break;
                op.type = Op::Text;
                op.text = txt;
                op.color = w.color;
                op.scale = w.text_scale;
                op.align = w.align;
                op.wrap_width = w.wrap_width;
                op.max_lines = w.max_lines;
                op.min_scale = w.text_min_scale;
                op.line_gap = w.line_gap;
                op.fit = w.fit_text;
                op.markup = w.color_markup;
                out.push_back(op);
                break;
            }
            }
            if (!w.on_tap.empty()) { // an undrawn zone the touch handler hit-tests (tab bars are transparent rects)
                Op hit = op;
                hit.type = Op::Hit;
                hit.w = w.rect[2];
                hit.h = w.rect[3];
                hit.on_tap = w.on_tap;
                out.push_back(hit);
            }
        }
    }
    // scroll bars
    for (auto &s : pg->scrolls) {
        int total = scroll_content_height(s);
        if (s.bar == 0 || total <= s.rect[3] || s.rect[3] <= 0) continue;
        Op tr;
        tr.type = Op::FillRect;
        tr.x = s.rect[0] + s.rect[2] - s.bar_w; tr.y = s.rect[1]; tr.w = s.bar_w; tr.h = s.rect[3];
        tr.color = s.bar_track;
        if ((tr.color >> 24) != 0) out.push_back(tr);
        Op th = tr;
        int off = scroll_offset(s.id);
        th.h = std::max(16, s.rect[3] * s.rect[3] / total);
        th.y = s.rect[1] + (s.rect[3] - th.h) * off / std::max(1, total - s.rect[3]);
        th.color = s.bar;
        out.push_back(th);
    }
}

} // namespace dsui
