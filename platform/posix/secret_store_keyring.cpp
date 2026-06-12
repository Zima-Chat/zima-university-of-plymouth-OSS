#include "secret_store_keyring.h"

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <keyutils.h>   // -lkeyutils; package: libkeyutils-dev
#include <sodium.h>

KeyringSecretStore::KeyringSecretStore(int timeout_seconds)
    : timeout_s_(timeout_seconds) {}

// Key description in the user keyring: "zima:assistant:<account>"
static std::string make_desc(std::string_view account) {
    return "zima:assistant:" + std::string(account);
}

void KeyringSecretStore::put(std::string_view name, std::span<const std::byte> value) {
    std::string desc = make_desc(name);
    key_serial_t key = add_key("user", desc.c_str(),
        value.data(), value.size(), KEY_SPEC_USER_KEYRING);
    if (key == -1)
        throw std::runtime_error(std::string("keyring add_key: ") + strerror(errno));
    // Set expiry to match the agent's idle timeout.
    if (keyctl_set_timeout(key, static_cast<unsigned>(timeout_s_)) == -1)
        throw std::runtime_error(std::string("keyring set_timeout: ") + strerror(errno));
}

SecureBuffer KeyringSecretStore::get(std::string_view name) {
    std::string desc = make_desc(name);
    key_serial_t key = request_key("user", desc.c_str(), nullptr, KEY_SPEC_USER_KEYRING);
    if (key == -1) {
        if (errno == ENOKEY)
            throw std::runtime_error("keyring: item not found: " + std::string(name));
        throw std::runtime_error(std::string("keyring request_key: ") + strerror(errno));
    }
    // First call: get the payload size.
    long sz = keyctl_read(key, nullptr, 0);
    if (sz < 0) throw std::runtime_error(std::string("keyring read size: ") + strerror(errno));
    SecureBuffer buf(static_cast<size_t>(sz));
    long r = keyctl_read(key, static_cast<char*>(buf.data()), static_cast<size_t>(sz));
    if (r < 0) throw std::runtime_error(std::string("keyring read: ") + strerror(errno));
    return buf;
}

void KeyringSecretStore::erase(std::string_view name) {
    std::string desc = make_desc(name);
    key_serial_t key = request_key("user", desc.c_str(), nullptr, KEY_SPEC_USER_KEYRING);
    if (key != -1) keyctl_unlink(key, KEY_SPEC_USER_KEYRING);
}

bool KeyringSecretStore::exists(std::string_view name) {
    std::string desc = make_desc(name);
    key_serial_t key = request_key("user", desc.c_str(), nullptr, KEY_SPEC_USER_KEYRING);
    return key != -1;
}

#else
KeyringSecretStore::KeyringSecretStore(int) : timeout_s_(1800) {}
void KeyringSecretStore::put(std::string_view, std::span<const std::byte>) {}
SecureBuffer KeyringSecretStore::get(std::string_view) { return {}; }
void KeyringSecretStore::erase(std::string_view) {}
bool KeyringSecretStore::exists(std::string_view) { return false; }
#endif
