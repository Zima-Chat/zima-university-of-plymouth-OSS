#include "session.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <thread>

#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- path helpers ----------

std::string agent_socket_path() {
#ifdef __APPLE__
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    return std::string(tmp) + "assistant-agent-" + std::to_string(geteuid()) + ".sock";
#elif defined(__linux__)
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) return std::string(xdg) + "/assistant-agent.sock";
    return "/tmp/assistant-agent-" + std::to_string(geteuid()) + ".sock";
#else
    return "/tmp/assistant-agent.sock";
#endif
}

std::string agent_token_file_path() {
#ifdef __APPLE__
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    return std::string(tmp) + "assistant-agent-" + std::to_string(geteuid()) + ".token";
#elif defined(__linux__)
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) return std::string(xdg) + "/assistant-agent.token";
    return "/tmp/assistant-agent-" + std::to_string(geteuid()) + ".token";
#else
    return "/tmp/assistant-agent.token";
#endif
}

// ---------- token generation and persistence ----------

SecureBuffer generate_and_write_session_token() {
    SecureBuffer tok(32);
    randombytes_buf(tok.data(), 32);

    std::string path = agent_token_file_path();
    // Unlink stale file first so we never inherit wrong permissions.
    unlink(path.c_str());
    // O_CREAT | O_EXCL with mode 0600 — atomic; no race with open+chmod.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        throw std::runtime_error("session: cannot create token file: " + std::string(strerror(errno)));
    // Write raw 32 bytes.
    const ssize_t written = write(fd, tok.data(), 32);
    ::close(fd);
    if (written != 32) {
        unlink(path.c_str());
        throw std::runtime_error("session: short write on token file");
    }
    return tok;
}

SecureBuffer load_session_token() {
    std::string path = agent_token_file_path();
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("session: cannot open token file (is agent running?)");
    SecureBuffer tok(32);
    ssize_t r = read(fd, tok.data(), 32);
    ::close(fd);
    if (r != 32)
        throw std::runtime_error("session: token file has unexpected size");
    return tok;
}

bool token_matches(const SecureBuffer& a, const SecureBuffer& b) {
    if (a.size() != b.size()) return false;
    // sodium_memcmp is constant-time (§5.2).
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
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
