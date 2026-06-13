#include "prompt_scanner.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

// ---------- Shannon entropy (§9.1: >4.5 bits/char, length >= 32) ----------

static double shannon_entropy(std::string_view s) {
    if (s.empty()) return 0.0;
    int freq[256]{};
    for (unsigned char c : s) freq[c]++;
    double h = 0.0;
    double n = static_cast<double>(s.size());
    for (int f : freq) {
        if (f == 0) continue;
        double p = f / n;
        h -= p * std::log2(p);
    }
    return h;
}

// ---------- Built-in patterns (§9.1) ----------

struct BuiltinPattern {
    const char* name;
    std::regex  re;
};

static const std::vector<BuiltinPattern>& builtin_patterns() {
    static std::vector<BuiltinPattern> pats = {
        { "aws_key_id",     std::regex(R"(AKIA[0-9A-Z]{16})")                        },
        { "aws_secret",     std::regex(R"([A-Za-z0-9/+]{40})")                       },
        { "github_pat",     std::regex(R"(gh[pos]_[A-Za-z0-9]{36,})")                },
        { "pem_private_key",std::regex(R"(-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----)")},
    };
    return pats;
}

// ---------- User-configurable regexes ----------

static std::vector<std::pair<std::string, std::regex>> load_user_patterns() {
    std::vector<std::pair<std::string, std::regex>> result;
#if !defined(_WIN32)
    const char* home = getenv("HOME");
    if (!home) return result;
    std::string path = std::string(home) + "/.config/assistant/redact.toml";
#else
    const char* appdata = getenv("APPDATA");
    if (!appdata) return result;
    std::string path = std::string(appdata) + "\\assistant\\redact.toml";
#endif
    std::ifstream f(path);
    if (!f.is_open()) return result;
    std::string line;
    while (std::getline(f, line)) {
        // Simple TOML line: pattern = "regex"
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Strip whitespace and quotes
        auto strip = [](std::string& s) {
            while (!s.empty() && (s.front()==' '||s.front()=='"'||s.front()=='\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() ==' '||s.back() =='"'||s.back() =='\t'||s.back()=='\r')) s.pop_back();
        };
        strip(key); strip(val);
        if (val.empty()) continue;
        try {
            result.emplace_back(key, std::regex(val));
        } catch (...) {}
    }
    return result;
}

// ---------- scan_prompt ----------

std::vector<ScanHit> scan_prompt(std::string_view prompt) {
    std::vector<ScanHit> hits;
    std::string s(prompt);

    // Built-in regex patterns
    for (const auto& pat : builtin_patterns()) {
        std::sregex_iterator it(s.begin(), s.end(), pat.re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::smatch m = *it;
            ScanHit h;
            h.pattern_name = pat.name;
            h.offset       = static_cast<size_t>(m.position());
            h.excerpt      = m.str().substr(0, 40);
            hits.push_back(std::move(h));
        }
    }

    // High-entropy strings of length >= 32 (§9.1)
    // Slide a 32-char window over non-whitespace runs.
    size_t i = 0;
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        size_t run_len = j - i;
        if (run_len >= 32) {
            for (size_t k = i; k + 32 <= j; k += 8) {
                std::string_view window(s.data() + k, std::min<size_t>(32, j - k));
                if (shannon_entropy(window) > 4.5) {
                    ScanHit h;
                    h.pattern_name = "high_entropy";
                    h.offset       = k;
                    h.excerpt      = std::string(window.substr(0, 40));
                    hits.push_back(std::move(h));
                    break; // one hit per run
                }
            }
        }
        i = j;
    }

    // User-configured patterns (loaded once and cached)
    static auto user_pats = load_user_patterns();
    for (const auto& [name, re] : user_pats) {
        std::sregex_iterator it(s.begin(), s.end(), re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::smatch m = *it;
            ScanHit h;
            h.pattern_name = name;
            h.offset       = static_cast<size_t>(m.position());
            h.excerpt      = m.str().substr(0, 40);
            hits.push_back(std::move(h));
        }
    }
    return hits;
}

bool prompt_has_secrets(std::string_view prompt) {
    return !scan_prompt(prompt).empty();
}

std::string redact_prompt(std::string_view prompt) {
    auto hits = scan_prompt(prompt);
    if (hits.empty()) return std::string(prompt);
    // Sort by offset descending so replacements don't shift earlier offsets.
    std::sort(hits.begin(), hits.end(),
        [](const ScanHit& a, const ScanHit& b){ return a.offset > b.offset; });
    std::string result(prompt);
    for (const auto& h : hits) {
        size_t len = h.excerpt.size();
        if (h.offset + len > result.size()) continue;
        result.replace(h.offset, len, "[REDACTED]");
    }
    return result;
}
