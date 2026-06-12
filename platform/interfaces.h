#pragma once
// Four platform abstraction interfaces (§6).
// No business logic depends on any platform header; these are the only
// cross-cutting contracts between the agent core and OS-specific code.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "../agent/secure_buffer.h"

// §6.1 — Platform secret store (Keychain / libsecret / Credential Manager).
class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    virtual void         put   (std::string_view name, std::span<const std::byte> value) = 0;
    virtual SecureBuffer get   (std::string_view name) = 0; // throws if absent
    virtual void         erase (std::string_view name) = 0;
    virtual bool         exists(std::string_view name) = 0;
};

// §6.2 — Local IPC transport (Unix domain socket / named pipe).
class IIpcEndpoint {
public:
    virtual ~IIpcEndpoint() = default;
    // Server: block until a client connects; sets up recv/send for that client.
    virtual void                    accept    () = 0;
    // Framed send: caller provides the complete wire frame (length + type + payload).
    virtual void                    send_frame(const std::vector<std::byte>& frame) = 0;
    // Framed recv: returns the complete wire frame or throws on disconnect/error.
    virtual std::vector<std::byte>  recv_frame() = 0;
    virtual void                    close     () = 0;
};

// §6.3 — Process hardening, called once from main() before any other init.
// Returns void; calls abort() on failure — exception unwinding is not safe
// before secrets are in memory.
class IProcessHardening {
public:
    virtual ~IProcessHardening() = default;
    virtual void harden() = 0;
};

// §6.4 — Cryptographically secure random bytes.
// libsodium's randombytes_buf already abstracts this; this interface exists
// for cases where a non-libsodium-backed primitive is needed.
class ISecureRandom {
public:
    virtual ~ISecureRandom() = default;
    virtual void fill(std::span<std::byte> buf) = 0;
};
