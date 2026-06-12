#include "session.h"

#include <stdexcept>
#include <string>
#include <thread>

#include <sodium.h>
#include <windows.h>

// ---------- path helpers ----------

std::string agent_socket_path() {
    return R"(\\.\pipe\assistant-agent)";
}

std::string agent_token_file_path() {
    char tmp_path[MAX_PATH + 1] = {};
    DWORD len = GetTempPathA(static_cast<DWORD>(sizeof(tmp_path)), tmp_path);
    if (len == 0 || len >= sizeof(tmp_path))
        return "assistant-agent.token";
    return std::string(tmp_path) + "assistant-agent.token";
}

// ---------- token generation and persistence ----------

SecureBuffer generate_and_write_session_token() {
    SecureBuffer tok(32);
    randombytes_buf(tok.data(), 32);

    std::string path = agent_token_file_path();

    // Remove any stale token first so CREATE_NEW gives us an exclusive create
    // (mirrors the POSIX O_CREAT | O_EXCL intent). The file lives in the
    // per-user temp directory, so it is already scoped to the current user.
    DeleteFileA(path.c_str());

    HANDLE h = CreateFileA(
        path.c_str(),
        GENERIC_WRITE,
        0,                       // no sharing
        nullptr,                 // not inheritable
        CREATE_NEW,              // fail if it already exists
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("session: cannot create token file");

    DWORD written = 0;
    BOOL ok = WriteFile(h, tok.data(), 32, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != 32) {
        DeleteFileA(path.c_str());
        throw std::runtime_error("session: short write on token file");
    }
    return tok;
}

SecureBuffer load_session_token() {
    std::string path = agent_token_file_path();

    HANDLE h = CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("session: cannot open token file (is agent running?)");

    SecureBuffer tok(32);
    DWORD r = 0;
    BOOL ok = ReadFile(h, tok.data(), 32, &r, nullptr);
    CloseHandle(h);
    if (!ok || r != 32)
        throw std::runtime_error("session: token file has unexpected size");
    return tok;
}

bool token_matches(const SecureBuffer& a, const SecureBuffer& b) {
    if (a.size() != b.size()) return false;
    // sodium_memcmp is constant-time (§5.2).
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

SecureBuffer derive_ipc_key(std::span<const std::byte> session_token) {
    static const unsigned char kCtx[] = "zima-ipc-enc-v1";
    SecureBuffer key(crypto_secretbox_KEYBYTES); // 32 bytes

    // BLAKE2b(out=32, key=session_token, message=context-label)
    crypto_generichash_state st;
    crypto_generichash_init(
        &st,
        reinterpret_cast<const unsigned char*>(session_token.data()),
        session_token.size(),
        key.size());
    crypto_generichash_update(&st, kCtx, sizeof(kCtx) - 1);
    crypto_generichash_final(&st,
        static_cast<unsigned char*>(key.data()), key.size());
    return key;
}

// ---------- IdleTimer ----------

IdleTimer::IdleTimer(std::chrono::seconds timeout) : timeout_(timeout) {}

void IdleTimer::start(ZeroCallback on_idle) {
    cb_ = std::move(on_idle);
    last_activity_ = std::chrono::steady_clock::now();
    running_.store(true, std::memory_order_relaxed);
    std::thread([this] { thread_func(); }).detach();
}

void IdleTimer::stop() {
    running_.store(false, std::memory_order_relaxed);
}

void IdleTimer::mark_activity() {
    std::lock_guard lock(mutex_);
    last_activity_ = std::chrono::steady_clock::now();
}

void IdleTimer::thread_func() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(30s);
        auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last;
        { std::lock_guard lock(mutex_); last = last_activity_; }
        if (now - last >= timeout_) {
            if (cb_) cb_();
        }
    }
}
