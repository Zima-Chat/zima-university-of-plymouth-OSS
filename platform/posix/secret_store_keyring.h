#pragma once
#include "../interfaces.h"

// Linux kernel keyring fallback (§6.1) — used when no Secret Service daemon
// is running (headless servers, minimal desktops).
// Keys are tied to the user keyring (@u) with a timeout matching the agent's
// idle policy (default 1800 s = 30 min).
class KeyringSecretStore final : public ISecretStore {
public:
    explicit KeyringSecretStore(int timeout_seconds = 1800);
    void         put   (std::string_view name, std::span<const std::byte> value) override;
    SecureBuffer get   (std::string_view name) override;
    void         erase (std::string_view name) override;
    bool         exists(std::string_view name) override;
private:
    int timeout_s_;
};
