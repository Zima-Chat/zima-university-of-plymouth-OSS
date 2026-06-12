#pragma once
// Minimal JSON string helpers shared by the agent (response parsing, request
// building) and the CLI (tool-call parsing). Header-only / inline so both
// translation units can use them without an extra source file.

#include <string>
#include <string_view>

namespace jsonutil {

// Escape a UTF-8 string for embedding as a JSON string value.
inline std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Decode the body of a JSON string (the characters between the surrounding
// quotes), resolving escape sequences. \uXXXX is decoded for the Basic
// Multilingual Plane and emitted as UTF-8; surrogate pairs are passed through
// best-effort (rare in generated source).
inline std::string unescape(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c != '\\' || i + 1 >= body.size()) { out += c; continue; }
        char e = body[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '/': out += '/';  break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case 'u': {
                if (i + 4 < body.size()) {
                    unsigned cp = 0; bool ok = true;
                    for (int k = 1; k <= 4; ++k) {
                        char h = body[i + k]; cp <<= 4;
                        if      (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (ok) {
                        i += 4;
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) {
                            out += char(0xC0 | (cp >> 6));
                            out += char(0x80 | (cp & 0x3F));
                        } else {
                            out += char(0xE0 | (cp >> 12));
                            out += char(0x80 | ((cp >> 6) & 0x3F));
                            out += char(0x80 | (cp & 0x3F));
                        }
                    } else { out += e; }
                } else { out += e; }
                break;
            }
            default: out += e; break;
        }
    }
    return out;
}

// Return the first complete, brace-balanced JSON object {...} in s, skipping
// over braces that appear inside string literals. Empty view if none found.
inline std::string_view find_object(std::string_view s) {
    size_t start = s.find('{');
    if (start == std::string_view::npos) return {};
    int depth = 0; bool in_str = false;
    for (size_t i = start; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return s.substr(start, i - start + 1);
        }
    }
    return {};
}

// Extract a string-valued field "key":"..." from a JSON object, returning the
// decoded value. Returns false if the key is absent or its value is not a
// string. Heuristic (first match wins) — sufficient for the flat tool-call and
// API-response objects this codebase handles.
inline bool get_string(std::string_view obj, std::string_view key, std::string& out) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string_view::npos) return false;
    pos += needle.size();
    auto skip_ws = [&] {
        while (pos < obj.size() &&
               (obj[pos] == ' ' || obj[pos] == '\t' || obj[pos] == '\n' || obj[pos] == '\r'))
            ++pos;
    };
    skip_ws();
    if (pos >= obj.size() || obj[pos] != ':') return false;
    ++pos;
    skip_ws();
    if (pos >= obj.size() || obj[pos] != '"') return false;
    ++pos;
    size_t start = pos;
    while (pos < obj.size()) {
        if (obj[pos] == '\\') { pos += 2; continue; }
        if (obj[pos] == '"') break;
        ++pos;
    }
    if (pos >= obj.size()) return false;
    out = unescape(obj.substr(start, pos - start));
    return true;
}

} // namespace jsonutil
