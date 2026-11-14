// Minimal JSON reader for the dual-screen host (manifest.json, load-plan.json). No dependencies.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dsui {

struct J {
    enum T { Null, Num, Str, Arr, Obj, Bool } t = Null;
    double n = 0;
    bool is_int = false;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    const J *get(const char *k) const {
        if (t != Obj) return nullptr;
        for (auto &p : o)
            if (p.first == k) return &p.second;
        return nullptr;
    }
    bool is_str() const { return t == Str; }
    bool is_num() const { return t == Num || t == Bool; }
    std::string str(const char *k, const char *def = "") const {
        const J *j = get(k);
        return j && j->t == Str ? j->s : def;
    }
    double num(const char *k, double def = 0) const {
        const J *j = get(k);
        return j && (j->t == Num || j->t == Bool) ? j->n : def;
    }
    bool has(const char *k) const { return get(k) != nullptr; }
};

class JsonParser {
public:
    JsonParser(const char *b, const char *e) : p_(b), e_(e) {}
    bool parse(J &out) {
        if (!val(out)) return false;
        ws();
        return true;
    }

private:
    const char *p_, *e_;
    bool ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) p_++;
        return p_ < e_;
    }
    static void put_utf8(std::string &o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t &v) {
        if (e_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool str(std::string &out) {
        if (p_ >= e_ || *p_ != '"') return false;
        p_++;
        while (p_ < e_ && *p_ != '"') {
            if (*p_ == '\\' && p_ + 1 < e_) {
                p_++;
                char c = *p_++;
                switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t v;
                    if (!hex4(v)) return false;
                    if (v >= 0xD800 && v < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, v);
                    break;
                }
                default: out += c;
                }
            } else out += *p_++;
        }
        if (p_ >= e_) return false;
        p_++;
        return true;
    }
    bool val(J &j) {
        if (!ws()) return false;
        if (*p_ == '{') {
            p_++;
            j.t = J::Obj;
            if (!ws()) return false;
            if (*p_ == '}') return p_++, true;
            for (;;) {
                if (!ws()) return false;
                std::string k;
                J v;
                if (!str(k) || !ws() || *p_++ != ':' || !val(v)) return false;
                j.o.emplace_back(std::move(k), std::move(v));
                if (!ws()) return false;
                if (*p_ == ',') { p_++; continue; }
                return *p_++ == '}';
            }
        }
        if (*p_ == '[') {
            p_++;
            j.t = J::Arr;
            if (!ws()) return false;
            if (*p_ == ']') return p_++, true;
            for (;;) {
                J v;
                if (!val(v)) return false;
                j.a.push_back(std::move(v));
                if (!ws()) return false;
                if (*p_ == ',') { p_++; continue; }
                return *p_++ == ']';
            }
        }
        if (*p_ == '"') { j.t = J::Str; return str(j.s); }
        if (e_ - p_ >= 4 && !strncmp(p_, "true", 4)) { p_ += 4; j.t = J::Bool; j.n = 1; return true; }
        if (e_ - p_ >= 5 && !strncmp(p_, "false", 5)) { p_ += 5; j.t = J::Bool; return true; }
        if (e_ - p_ >= 4 && !strncmp(p_, "null", 4)) { p_ += 4; return true; }
        char *end;
        j.n = strtod(p_, &end);
        if (end == p_) return false;
        j.is_int = true;
        for (const char *q = p_; q < end; q++)
            if (*q == '.' || *q == 'e' || *q == 'E') j.is_int = false;
        j.t = J::Num;
        p_ = end;
        return true;
    }
};

} // namespace dsui
