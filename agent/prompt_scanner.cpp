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
        { "Cloudinary",                    std::regex(R"(cloudinary://.*)")                                                                       },
        { "Firebase URL",                  std::regex(R"(.*firebaseio\.com)")                                                                     },
        { "Slack Token",                   std::regex(R"((xox[p|b|o|a]-[0-9]{12}-[0-9]{12}-[0-9]{12}-[a-z0-9]{32}))")                             },
        { "RSA private key",               std::regex(R"(-----BEGIN RSA PRIVATE KEY-----)")                                                       },
        { "SSH (DSA) private key",         std::regex(R"(-----BEGIN DSA PRIVATE KEY-----)")                                                       },
        { "SSH (EC) private key",          std::regex(R"(-----BEGIN EC PRIVATE KEY-----)")                                                        },
        { "PGP private key block",         std::regex(R"(-----BEGIN PGP PRIVATE KEY BLOCK-----)")                                                 },
        { "Amazon AWS Access Key ID",      std::regex(R"(AKIA[0-9A-Z]{16})")                                                                       },
        { "Amazon MWS Auth Token",         std::regex(R"(amzn\.mws\.[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})")                },
        { "AWS API Key",                   std::regex(R"(AKIA[0-9A-Z]{16})")                                                                       },
        { "Facebook Access Token",         std::regex(R"(EAACEdEose0cBA[0-9A-Za-z]+)")                                                            },
        { "Facebook OAuth",                std::regex(R"([f|F][a|A][c|C][e|E][b|B][o|O][o|O][k|K].*['|"][0-9a-f]{32}['|"])")                      },
        { "GitHub",                        std::regex(R"([g|G][i|I][t|T][h|H][u|U][b|B].*['|"][0-9a-zA-Z]{35,40}['|"])")                          },
        { "Generic API Key",               std::regex(R"([a|A][p|P][i|I][_]?[k|K][e|E][y|Y].*['|"][0-9a-zA-Z]{32,45}['|"])")                       },
        { "Generic Secret",                std::regex(R"([s|S][e|E][c|C][r|R][e|E][t|T].*['|"][0-9a-zA-Z]{32,45}['|"])")                          },
        { "Google API Key",                std::regex(R"(AIza[0-9A-Za-z\-_]{35})")                                                                 },
        { "Google Cloud Platform API Key", std::regex(R"(AIza[0-9A-Za-z\-_]{35})")                                                                 },
        { "Google Cloud Platform OAuth",   std::regex(R"([0-9]+-[0-9A-Za-z_]{32}\.apps\.googleusercontent\.com)")                                 },
        { "Google Drive API Key",          std::regex(R"(AIza[0-9A-Za-z\-_]{35})")                                                                 },
        { "Google Drive OAuth",            std::regex(R"([0-9]+-[0-9A-Za-z_]{32}\.apps\.googleusercontent\.com)")                                 },
        { "Google (GCP) Service-account",  std::regex(R"("type": "service_account")")                                                             },
        { "Google Gmail API Key",          std::regex(R"(AIza[0-9A-Za-z\-_]{35})")                                                                 },
        { "Google Gmail OAuth",            std::regex(R"([0-9]+-[0-9A-Za-z_]{32}\.apps\.googleusercontent\.com)")                                 },
        { "Google OAuth Access Token",     std::regex(R"(ya29\.[0-9A-Za-z\-_]+)")                                                                  },
        { "Google YouTube API Key",        std::regex(R"(AIza[0-9A-Za-z\-_]{35})")                                                                 },
        { "Google YouTube OAuth",          std::regex(R"([0-9]+-[0-9A-Za-z_]{32}\.apps\.googleusercontent\.com)")                                 },
        { "Heroku API Key",                std::regex(R"([h|H][e|E][r|R][o|O][k|K][u|U].*[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12})") },
        { "MailChimp API Key",             std::regex(R"([0-9a-f]{32}-us[0-9]{1,2})")                                                              },
        { "Mailgun API Key",               std::regex(R"(key-[0-9a-zA-Z]{32})")                                                                    },
        { "Password in URL",               std::regex(R"([a-zA-Z]{3,10}://[^/\s:@]{3,20}:[^/\s:@]{3,20}@.{1,100}["'\s])")                         },
        { "PayPal Braintree Access Token", std::regex(R"(access_token\$production\$[0-9a-z]{16}\$[0-9a-f]{32})")                                  },
        { "Picatic API Key",               std::regex(R"(sk_live_[0-9a-z]{32})")                                                                   },
        { "Slack Webhook",                 std::regex(R"(https://hooks.slack.com/services/T[a-zA-Z0-9_]{8}/B[a-zA-Z0-9_]{8}/[a-zA-Z0-9_]{24})")    },
        { "Stripe API Key",                std::regex(R"(sk_live_[0-9a-zA-Z]{24})")                                                                },
        { "Stripe Restricted API Key",     std::regex(R"(rk_live_[0-9a-zA-Z]{24})")                                                                },
        { "Square Access Token",           std::regex(R"(sq0atp-[0-9A-Za-z\-_]{22})")                                                              },
        { "Square OAuth Secret",           std::regex(R"(sq0csp-[0-9A-Za-z\-_]{43})")                                                              },
        { "Twilio API Key",                std::regex(R"(SK[0-9a-fA-F]{32})")                                                                      },
        { "Twitter Access Token",          std::regex(R"([t|T][w|W][i|I][t|T][t|T][e|E][r|R].*[1-9][0-9]+-[0-9a-zA-Z]{40})")                      },
        { "Twitter OAuth",                 std::regex(R"([t|T][w|W][i|I][t|T][t|T][e|E][r|R].*['|"][0-9a-zA-Z]{35,44}['|"])")                      },
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
