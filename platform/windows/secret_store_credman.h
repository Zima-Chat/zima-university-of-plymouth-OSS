#pragma once
#include "../interfaces.h"

// Windows Credential Manager implementation of ISecretStore (§6.1).
// Uses CRED_TYPE_GENERIC / CRED_PERSIST_LOCAL_MACHINE.
// DPAPI encrypts the blob at rest bound to the user's login credentials.
class CredManSecretStore final : public ISecretStore {
public:
    void         put   (std::string_view name, std::span<const std::byte> value) override;
    SecureBuffer get   (std::string_view name) override;
    void         erase (std::string_view name) override;
    bool         exists(std::string_view name) override;
};
