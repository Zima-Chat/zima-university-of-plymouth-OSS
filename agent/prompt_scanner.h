#pragma once
// Outbound prompt scanner (§9.1).
// Runs before every Complete request to flag credentials/private keys.

#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ScanHit {
    std::string pattern_name; // e.g. "aws_key_id", "github_pat", "high_entropy"
    size_t      offset;       // byte offset in the prompt
    std::string excerpt;      // short excerpt (never more than 40 chars)
};

// Scan the prompt for secrets and return all hits.
// Checks built-in patterns plus user regexes from
// ~/.config/assistant/redact.toml (if present).
std::vector<ScanHit> scan_prompt(std::string_view prompt);

// Replace all hits in prompt with "[REDACTED]" and return the result.
std::string redact_prompt(std::string_view prompt);

// Returns true if the prompt contains any flagged content.
bool prompt_has_secrets(std::string_view prompt);
