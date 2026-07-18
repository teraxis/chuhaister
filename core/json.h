// Minimal JSON for the cswap native core (header-only).
//
// Two tools, deliberately narrow:
//  * JVal/JParser — a read-only recursive-descent parser for API responses
//    and small config payloads (same one the tray uses).
//  * spliceTopLevelKey — byte-precise surgery on a large JSON *text*: replace
//    (or insert) the value of one top-level key and leave every other byte
//    untouched. Used on ~/.claude.json, which holds lots of Claude Code state
//    we must never round-trip through a lossy parse/serialize cycle.
//  * jsonEscape/jsonString — escaping for the small payloads we author.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace cswap {

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const char* key) const {
        if (type != OBJ) return nullptr;
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    // Nested lookup that tolerates missing links: get2("a","b") == obj a -> b.
    const JVal* get2(const char* k1, const char* k2) const {
        const JVal* v = get(k1);
        return v ? v->get(k2) : nullptr;
    }
    double numOr(double def) const { return type == NUM ? num : def; }
    std::string strOr(const char* def) const { return type == STR ? str : def; }
    bool boolOr(bool def) const { return type == BOOL ? b : def; }
};

struct JParser {
    const char* p;
    const char* end;
    bool ok = true;

    explicit JParser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {
        // Skip a UTF-8 BOM. Editors and PowerShell's Set-Content happily add
        // one, and without this the whole document fails to parse.
        if (end - p >= 3 && (unsigned char)p[0] == 0xEF
                         && (unsigned char)p[1] == 0xBB
                         && (unsigned char)p[2] == 0xBF)
            p += 3;
    }

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }

    JVal parse() { JVal v = value(); ws(); return v; }

    JVal value() {
        ws();
        if (p >= end) { ok = false; return {}; }
        switch (*p) {
            case '{': return object();
            case '[': return array();
            case '"': { JVal v; v.type = JVal::STR; v.str = string(); return v; }
            case 't': p += 4; { JVal v; v.type = JVal::BOOL; v.b = true; return v; }
            case 'f': p += 5; { JVal v; v.type = JVal::BOOL; v.b = false; return v; }
            case 'n': p += 4; return {};
            default: return number();
        }
    }

    JVal object() {
        JVal v; v.type = JVal::OBJ; ++p; // '{'
        ws();
        if (eat('}')) return v;
        while (ok && p < end) {
            ws();
            if (*p != '"') { ok = false; break; }
            std::string key = string();
            if (!eat(':')) { ok = false; break; }
            v.obj.emplace_back(std::move(key), value());
            if (eat(',')) continue;
            eat('}');
            break;
        }
        return v;
    }

    JVal array() {
        JVal v; v.type = JVal::ARR; ++p; // '['
        ws();
        if (eat(']')) return v;
        while (ok && p < end) {
            v.arr.push_back(value());
            if (eat(',')) continue;
            eat(']');
            break;
        }
        return v;
    }

    std::string string() {
        std::string out; ++p; // '"'
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (p + 4 < end) {
                            unsigned cp = 0;
                            sscanf(p + 1, "%4x", &cp);
                            p += 4;
                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) {
                                out += (char)(0xC0 | (cp >> 6));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | (cp >> 12));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += *p; break;
                }
            } else {
                out += *p;
            }
            ++p;
        }
        if (p < end) ++p; // closing '"'
        return out;
    }

    JVal number() {
        JVal v; v.type = JVal::NUM;
        char* np = nullptr;
        v.num = strtod(p, &np);
        if (np == p) { ok = false; return {}; }
        p = np;
        return v;
    }
};

// ---- authoring helpers -------------------------------------------------------

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

inline std::string jsonString(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

// ---- top-level key splice ----------------------------------------------------

// Span [start, end) of the *value* of a top-level key in JSON text, or
// (npos, npos) when the key is absent. Understands strings/escapes and nesting,
// so it cannot be fooled by the same key at depth > 1 or inside a string.
inline std::pair<size_t, size_t> findTopLevelValue(const std::string& text, const std::string& key) {
    const size_t npos = std::string::npos;
    int depth = 0;
    bool inStr = false, esc = false;
    size_t i = 0, n = text.size();
    std::string lastStr;
    size_t lastStrDepth = 0;
    bool haveKeyCandidate = false;

    while (i < n) {
        char c = text[i];
        if (inStr) {
            if (esc) { esc = false; ++i; continue; }
            if (c == '\\') { esc = true; ++i; continue; }
            if (c == '"') { inStr = false; }
            else lastStr += c;
            ++i;
            continue;
        }
        switch (c) {
            case '"':
                inStr = true;
                lastStr.clear();
                lastStrDepth = (size_t)depth;
                haveKeyCandidate = true;
                ++i;
                continue;
            case ':': {
                if (haveKeyCandidate && lastStrDepth == 1 && depth == 1 && lastStr == key) {
                    ++i;
                    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
                    size_t start = i;
                    // find the value's end
                    if (i < n && (text[i] == '{' || text[i] == '[')) {
                        int d = 0;
                        bool s2 = false, e2 = false;
                        for (; i < n; ++i) {
                            char ch = text[i];
                            if (s2) {
                                if (e2) e2 = false;
                                else if (ch == '\\') e2 = true;
                                else if (ch == '"') s2 = false;
                                continue;
                            }
                            if (ch == '"') s2 = true;
                            else if (ch == '{' || ch == '[') ++d;
                            else if (ch == '}' || ch == ']') {
                                --d;
                                if (d == 0) { ++i; break; }
                            }
                        }
                    } else if (i < n && text[i] == '"') {
                        bool e2 = false;
                        ++i;
                        for (; i < n; ++i) {
                            if (e2) { e2 = false; continue; }
                            if (text[i] == '\\') { e2 = true; continue; }
                            if (text[i] == '"') { ++i; break; }
                        }
                    } else {
                        while (i < n && text[i] != ',' && text[i] != '}' && text[i] != '\n' && text[i] != '\r')
                            ++i;
                        while (i > start && (text[i - 1] == ' ' || text[i - 1] == '\t')) --i;
                    }
                    return {start, i};
                }
                haveKeyCandidate = false;
                ++i;
                continue;
            }
            case '{': case '[': ++depth; haveKeyCandidate = false; ++i; continue;
            case '}': case ']': --depth; haveKeyCandidate = false; ++i; continue;
            case ',': haveKeyCandidate = false; ++i; continue;
            default: ++i; continue;
        }
    }
    return {npos, npos};
}

// Replace (or insert) `key`'s value in a JSON object text, touching no other
// bytes. `newValue` must be valid JSON. Returns false when text isn't an object.
inline bool spliceTopLevelKey(std::string& text, const std::string& key, const std::string& newValue) {
    auto span = findTopLevelValue(text, key);
    if (span.first != std::string::npos) {
        text.replace(span.first, span.second - span.first, newValue);
        return true;
    }
    // insert before the final top-level '}'
    size_t close = text.find_last_of('}');
    if (close == std::string::npos) return false;
    // does the object already have members? (any non-ws between first '{' and close)
    size_t open = text.find_first_of('{');
    if (open == std::string::npos || open > close) return false;
    bool empty = true;
    for (size_t i = open + 1; i < close; ++i) {
        char c = text[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { empty = false; break; }
    }
    std::string insert = std::string(empty ? "\n  " : ",\n  ") + jsonString(key) + ": " + newValue + "\n";
    text.insert(close, insert);
    return true;
}

} // namespace cswap
