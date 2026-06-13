#pragma once
#include "interfaces.h"
#include <sodium.h>

// libsodium-backed ISecureRandom (§6.4).
// randombytes_buf routes to getrandom(2) on Linux, SecRandomCopyBytes on macOS,
// and BCryptGenRandom on Windows — all without any additional dispatch needed here.
class LibsodiumSecureRandom final : public ISecureRandom {
public:
    void fill(std::span<std::byte> buf) override {
        randombytes_buf(buf.data(), buf.size());
    }
};

// Process-lifetime singleton — sodium_init() must have been called first.
inline ISecureRandom& get_secure_random() {
    static LibsodiumSecureRandom inst;
    return inst;
}
