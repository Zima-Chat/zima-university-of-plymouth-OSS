#include "secret_store_keychain.h"

#ifdef __APPLE__
#include <Security/Security.h>
#include <stdexcept>
#include <string>

static constexpr const char* kService = "com.zima.assistant";

// Build the base query dictionary shared between all Keychain operations.
static CFMutableDictionaryRef make_query(std::string_view account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass,   kSecClassGenericPassword);
    CFStringRef svc = CFStringCreateWithCString(nullptr, kService, kCFStringEncodingUTF8);
    CFStringRef acc = CFStringCreateWithCString(nullptr,
        std::string(account).c_str(), kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecAttrService, svc);
    CFDictionarySetValue(q, kSecAttrAccount, acc);
    CFRelease(svc); CFRelease(acc);
    return q;
}

void KeychainSecretStore::put(std::string_view name, std::span<const std::byte> value) {
    // Delete any existing item first (update via SecItemUpdate is more complex).
    {
        CFMutableDictionaryRef q = make_query(name);
        SecItemDelete(q);
        CFRelease(q);
    }
    CFMutableDictionaryRef q = make_query(name);
    CFDataRef data = CFDataCreate(nullptr,
        reinterpret_cast<const UInt8*>(value.data()),
        static_cast<CFIndex>(value.size()));
    CFDictionarySetValue(q, kSecValueData, data);
    // Accessible only when device is unlocked; never backed up or migrated.
    CFDictionarySetValue(q, kSecAttrAccessible,
                         kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    OSStatus st = SecItemAdd(q, nullptr);
    CFRelease(data); CFRelease(q);
    if (st != errSecSuccess)
        throw std::runtime_error("Keychain put failed: " + std::to_string(st));
}

SecureBuffer KeychainSecretStore::get(std::string_view name) {
    CFMutableDictionaryRef q = make_query(name);
    CFDictionarySetValue(q, kSecReturnData,       kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit,        kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(q, &result);
    CFRelease(q);
    if (st == errSecItemNotFound)
        throw std::runtime_error("Keychain: item not found: " + std::string(name));
    if (st != errSecSuccess)
        throw std::runtime_error("Keychain get failed: " + std::to_string(st));
    CFDataRef data = static_cast<CFDataRef>(result);
    CFIndex len = CFDataGetLength(data);
    SecureBuffer buf(static_cast<size_t>(len));
    // Copy byte-by-byte into the locked buffer, then release CFData immediately.
    std::memcpy(buf.data(), CFDataGetBytePtr(data), static_cast<size_t>(len));
    CFRelease(data);
    return buf;
}

void KeychainSecretStore::erase(std::string_view name) {
    CFMutableDictionaryRef q = make_query(name);
    SecItemDelete(q);
    CFRelease(q);
}

bool KeychainSecretStore::exists(std::string_view name) {
    CFMutableDictionaryRef q = make_query(name);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    OSStatus st = SecItemCopyMatching(q, nullptr);
    CFRelease(q);
    return st == errSecSuccess;
}

#else
// Non-Apple stub: should never be linked on non-Apple platforms.
void KeychainSecretStore::put(std::string_view, std::span<const std::byte>) {}
SecureBuffer KeychainSecretStore::get(std::string_view) { return {}; }
void KeychainSecretStore::erase(std::string_view) {}
bool KeychainSecretStore::exists(std::string_view) { return false; }
#endif
