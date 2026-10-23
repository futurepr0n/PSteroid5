#pragma once
// Small JSON reader/writer for the CSSteroids server API payloads. No exceptions;
// parse() returns false on malformed input or nesting deeper than max_depth.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace cssteroids::json {
struct Value {
    enum class Type { null, boolean, number, string, array, object } type = Type::null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> fields;

    bool is_object() const { return type == Type::object; }
    bool is_array() const { return type == Type::array; }
    const Value *get(const std::string &key) const {
        for (const auto &f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    std::string str(const std::string &key, const std::string &fallback = {}) const {
        const Value *v = get(key);
        return v && v->type == Type::string ? v->s : fallback;
    }
    double num(const std::string &key, double fallback = 0) const {
        const Value *v = get(key);
        return v && v->type == Type::number ? v->n : fallback;
    }

    static Value string_value(std::string text) { Value v; v.type = Type::string; v.s = std::move(text); return v; }
    static Value number_value(double x) { Value v; v.type = Type::number; v.n = x; return v; }
    static Value bool_value(bool x) { Value v; v.type = Type::boolean; v.b = x; return v; }
    static Value object() { Value v; v.type = Type::object; return v; }
    Value &set(const std::string &key, Value v) {
        for (auto &f : fields) if (f.first == key) { f.second = std::move(v); return *this; }
        fields.emplace_back(key, std::move(v));
        return *this;
    }
};

class Parser {
    const char *p, *end;
    static constexpr int max_depth = 64;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool lit(const char *word) {
        for (const char *w = word; *w; w++, p++) if (p >= end || *p != *w) return false;
        return true;
    }
    static void utf8(std::string &out, unsigned c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xc0 | c >> 6); out += char(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += char(0xe0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3f)); out += char(0x80 | (c & 0x3f)); }
        else { out += char(0xf0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3f)); out += char(0x80 | (c >> 6 & 0x3f)); out += char(0x80 | (c & 0x3f)); }
    }
    bool hex4(unsigned &c) {
        if (end - p < 4) return false;
        c = 0;
        for (int i = 0; i < 4; i++, p++) {
            char h = *p;
            c <<= 4;
            if (h >= '0' && h <= '9') c |= unsigned(h - '0');
            else if (h >= 'a' && h <= 'f') c |= unsigned(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') c |= unsigned(h - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string &out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { if ((unsigned char)c < 0x20) return false; out += c; continue; }
            if (p >= end) return false;
            switch (*p++) {
                case '"': out += '"'; break;  case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    unsigned c1;
                    if (!hex4(c1)) return false;
                    if (c1 >= 0xd800 && c1 < 0xdc00) {
                        unsigned c2;
                        if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        if (!hex4(c2) || c2 < 0xdc00 || c2 >= 0xe000) return false;
                        c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                    }
                    utf8(out, c1);
                    break;
                }
                default: return false;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Value &v, int depth) {
        if (depth > max_depth) return false;
        ws();
        if (p >= end) return false;
        switch (*p) {
            case 'n': v.type = Value::Type::null; return lit("null");
            case 't': v.type = Value::Type::boolean; v.b = true; return lit("true");
            case 'f': v.type = Value::Type::boolean; v.b = false; return lit("false");
            case '"': v.type = Value::Type::string; return string(v.s);
            case '[':
                v.type = Value::Type::array; p++; ws();
                if (p < end && *p == ']') { p++; return true; }
                for (;;) {
                    v.items.emplace_back();
                    if (!value(v.items.back(), depth + 1)) return false;
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; return true; }
                    return false;
                }
            case '{':
                v.type = Value::Type::object; p++; ws();
                if (p < end && *p == '}') { p++; return true; }
                for (;;) {
                    ws();
                    std::string key;
                    if (!string(key)) return false;
                    ws();
                    if (p >= end || *p++ != ':') return false;
                    v.fields.emplace_back(std::move(key), Value{});
                    if (!value(v.fields.back().second, depth + 1)) return false;
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; return true; }
                    return false;
                }
            default: {
                const char *start = p;
                if (p < end && *p == '-') p++;
                if (p >= end || *p < '0' || *p > '9') return false;
                while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++;
                std::string text(start, p);
                char *stop = nullptr;
                v.type = Value::Type::number;
                v.n = std::strtod(text.c_str(), &stop);
                return stop && *stop == 0 && std::isfinite(v.n);
            }
        }
    }
public:
    static bool parse(const std::string &text, Value &out) {
        Parser parser;
        parser.p = text.data(); parser.end = text.data() + text.size();
        out = Value{};
        if (!parser.value(out, 0)) return false;
        parser.ws();
        return parser.p == parser.end;
    }
};

inline bool parse(const std::string &text, Value &out) { return Parser::parse(text, out); }

inline void write_string(std::string &out, const std::string &s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += char(c);
        }
    }
    out += '"';
}

inline void write(std::string &out, const Value &v) {
    switch (v.type) {
        case Value::Type::null: out += "null"; break;
        case Value::Type::boolean: out += v.b ? "true" : "false"; break;
        case Value::Type::number: {
            char buf[32];
            if (v.n == std::floor(v.n) && std::fabs(v.n) < 9e15) std::snprintf(buf, sizeof buf, "%.0f", v.n);
            else std::snprintf(buf, sizeof buf, "%.17g", v.n);
            out += buf;
            break;
        }
        case Value::Type::string: write_string(out, v.s); break;
        case Value::Type::array:
            out += '[';
            for (size_t i = 0; i < v.items.size(); i++) { if (i) out += ','; write(out, v.items[i]); }
            out += ']';
            break;
        case Value::Type::object:
            out += '{';
            for (size_t i = 0; i < v.fields.size(); i++) {
                if (i) out += ',';
                write_string(out, v.fields[i].first); out += ':'; write(out, v.fields[i].second);
            }
            out += '}';
            break;
    }
}
inline std::string dump(const Value &v) { std::string out; write(out, v); return out; }
}
