#pragma once
// Session token management and idle-timeout logic (§3.3, §5.2).

#include "secure_buffer.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Derives the platform-specific socket/token-file path.
std::string agent_socket_path();
std::string agent_token_file_path();

// Generate and persist the session token (§5.2):
//  - 32 random bytes via randombytes_buf
//  - Written to agent_token_file_path() with 0600 permissions
// Returns the token in a SecureBuffer.
SecureBuffer generate_and_write_session_token();

// Load the session token from the well-known file.
SecureBuffer load_session_token();

// Constant-time token comparison (§5.2).
bool token_matches(const SecureBuffer& a, const SecureBuffer& b);

// Derive the 32-byte IPC channel encryption key (for crypto_secretbox) from
// the session token. Both peers already share the token via the ACL-protected
// token file, so neither side needs any other secret to derive the same key.
SecureBuffer derive_ipc_key(std::span<const std::byte> session_token);

// ---------------------------------------------------------------------------
// IdleTimer — zeroes the in-memory API key after a configurable idle period
// (default 30 min per §3.3 step 4). The agent calls mark_activity() on every
// successful Complete request.
// ---------------------------------------------------------------------------
class IdleTimer {
public:
    using ZeroCallback = std::function<void()>;

    explicit IdleTimer(std::chrono::seconds timeout = std::chrono::seconds(1800));

    // Start the background timer thread.
    void start(ZeroCallback on_idle);
    // Stop the timer thread (called on shutdown).
    void stop();
    // Reset the idle clock (call after every successful request).
    void mark_activity();

private:
    std::chrono::seconds             timeout_;
    std::atomic<bool>                running_{false};
    std::mutex                       mutex_;
    std::chrono::steady_clock::time_point last_activity_;
    ZeroCallback                     cb_;
    void thread_func();
};
