#include "secret_store_credman.h"

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#include <stdexcept>
#include <string>
#include <cstring>

static constexpr wchar_t kPrefix[] = L"zima.assistant.";

static std::wstring make_target(std::string_view name) {
    std::wstring t(kPrefix);
    for (char c : name) t += static_cast<wchar_t>(c);
    return t;
}

void CredManSecretStore::put(std::string_view name, std::span<const std::byte> value) {
    std::wstring target = make_target(name);
    CREDENTIALW cred{};
    cred.Type               = CRED_TYPE_GENERIC;
    cred.TargetName         = target.data();
    cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
    cred.CredentialBlobSize = static_cast<DWORD>(value.size());
    cred.CredentialBlob     = reinterpret_cast<LPBYTE>(const_cast<std::byte*>(value.data()));
    if (!CredWriteW(&cred, 0))
        throw std::runtime_error("CredWrite failed: " + std::to_string(GetLastError()));
}

SecureBuffer CredManSecretStore::get(std::string_view name) {
    std::wstring target = make_target(name);
    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &pcred))
        throw std::runtime_error("CredRead failed: " + std::to_string(GetLastError()));
    SecureBuffer buf(pcred->CredentialBlobSize);
    // Copy into locked buffer; free the CREDENTIAL structure immediately.
    std::memcpy(buf.data(), pcred->CredentialBlob, pcred->CredentialBlobSize);
    CredFree(pcred);
    return buf;
}

void CredManSecretStore::erase(std::string_view name) {
    std::wstring target = make_target(name);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

bool CredManSecretStore::exists(std::string_view name) {
    std::wstring target = make_target(name);
    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &pcred)) return false;
    CredFree(pcred);
    return true;
}

#else
void CredManSecretStore::put(std::string_view, std::span<const std::byte>) {}
SecureBuffer CredManSecretStore::get(std::string_view) { return {}; }
void CredManSecretStore::erase(std::string_view) {}
bool CredManSecretStore::exists(std::string_view) { return false; }
#endif
