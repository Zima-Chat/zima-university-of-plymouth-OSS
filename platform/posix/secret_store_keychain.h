#pragma once
#include "../interfaces.h"

// macOS Keychain implementation of ISecretStore (§6.1).
// Uses kSecClassGenericPassword with service = com.zima.assistant
// and kSecAttrAccessibleWhenUnlockedThisDeviceOnly so items cannot be
// backed up to iCloud or migrated to another device.
class KeychainSecretStore final : public ISecretStore {
public:
    void         put   (std::string_view name, std::span<const std::byte> value) override;
    SecureBuffer get   (std::string_view name) override;
    void         erase (std::string_view name) override;
    bool         exists(std::string_view name) override;
};
