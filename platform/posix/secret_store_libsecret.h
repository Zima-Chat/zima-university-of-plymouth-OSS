#pragma once
#include "../interfaces.h"

// Linux Secret Service (libsecret / D-Bus) implementation of ISecretStore (§6.1).
// Falls back to the kernel keyring when no Secret Service daemon is running.
class LibsecretSecretStore final : public ISecretStore {
public:
    void         put   (std::string_view name, std::span<const std::byte> value) override;
    SecureBuffer get   (std::string_view name) override;
    void         erase (std::string_view name) override;
    bool         exists(std::string_view name) override;
};
