#include "secret_store_libsecret.h"

#ifdef __linux__
#include <stdexcept>
#include <string>
#include <cstring>

// libsecret header (package: libsecret-1-dev)
#include <libsecret/secret.h>

static const SecretSchema* zima_schema() {
    static const SecretSchema s = {
        "com.zima.assistant", SECRET_SCHEMA_NONE,
        {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
    };
    return &s;
}

void LibsecretSecretStore::put(std::string_view name, std::span<const std::byte> value) {
    // Secret Service works with strings; base64-encode the raw bytes.
    // We use sodium_bin2base64 to avoid introducing another dependency.
    size_t b64len = sodium_base64_encoded_len(value.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string b64(b64len, '\0');
    sodium_bin2base64(b64.data(), b64len,
        reinterpret_cast<const unsigned char*>(value.data()), value.size(),
        sodium_base64_VARIANT_ORIGINAL);
    b64.resize(std::strlen(b64.data())); // trim null terminator

    GError* err = nullptr;
    secret_password_store_sync(
        zima_schema(), SECRET_COLLECTION_DEFAULT,
        "Zima assistant credential",
        b64.c_str(), nullptr, &err,
        "account", std::string(name).c_str(), nullptr);
    if (err) {
        std::string msg = err->message;
        g_error_free(err);
        throw std::runtime_error("libsecret put: " + msg);
    }
}

SecureBuffer LibsecretSecretStore::get(std::string_view name) {
    GError* err = nullptr;
    gchar* pw = secret_password_lookup_sync(
        zima_schema(), nullptr, &err,
        "account", std::string(name).c_str(), nullptr);
    if (err) {
        std::string msg = err->message;
        g_error_free(err);
        throw std::runtime_error("libsecret get: " + msg);
    }
    if (!pw) throw std::runtime_error("libsecret: item not found: " + std::string(name));

    size_t b64len = std::strlen(pw);
    // Worst case decoded size
    SecureBuffer buf(b64len);
    size_t decoded_len = 0;
    int rc = sodium_base642bin(
        reinterpret_cast<unsigned char*>(buf.data()), buf.size(),
        pw, b64len, nullptr, &decoded_len, nullptr,
        sodium_base64_VARIANT_ORIGINAL);
    secret_password_free(pw);
    if (rc != 0) throw std::runtime_error("libsecret: base64 decode failed");
    buf.resize(decoded_len);
    return buf;
}

void LibsecretSecretStore::erase(std::string_view name) {
    GError* err = nullptr;
    secret_password_clear_sync(
        zima_schema(), nullptr, &err,
        "account", std::string(name).c_str(), nullptr);
    if (err) g_error_free(err);
}

bool LibsecretSecretStore::exists(std::string_view name) {
    GError* err = nullptr;
    gchar* pw = secret_password_lookup_sync(
        zima_schema(), nullptr, &err,
        "account", std::string(name).c_str(), nullptr);
    if (err) { g_error_free(err); return false; }
    bool found = (pw != nullptr);
    if (pw) secret_password_free(pw);
    return found;
}

#else
void LibsecretSecretStore::put(std::string_view, std::span<const std::byte>) {}
SecureBuffer LibsecretSecretStore::get(std::string_view) { return {}; }
void LibsecretSecretStore::erase(std::string_view) {}
bool LibsecretSecretStore::exists(std::string_view) { return false; }
#endif
