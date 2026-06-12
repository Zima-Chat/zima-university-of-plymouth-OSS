#include "hardening_posix.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <sodium.h>

// ---------- globals ----------

static int g_pipe_rd = -1;
static int g_pipe_wr = -1;
static std::atomic<bool> g_shutdown{false};

static std::mutex              g_cb_mutex;
static std::vector<std::function<void()>> g_callbacks;

// ---------- signal handler (async-signal-safe) ----------
// Sets the atomic flag and writes one byte to the self-pipe.
// We do NOT handle SIGSEGV/SIGBUS/SIGILL — the address space is untrustworthy
// by the time those fire; RLIMIT_CORE = 0 means no on-disk artefact.
static void posix_signal_handler(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
    const char b = 1;
    // write() is async-signal-safe; ignore return value in handler context.
    (void)write(g_pipe_wr, &b, 1);
}

static void install_handlers() {
    struct sigaction sa{};
    sa.sa_handler = posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    for (int sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) {
        if (sigaction(sig, &sa, nullptr) != 0) abort();
    }
}

// ---------- public interface ----------

void harden_common_posix() {
    // 1. Disable core dumps unconditionally (§4.1 step 1).
    struct rlimit rl{0, 0};
    if (setrlimit(RLIMIT_CORE, &rl) != 0) abort();

    // 2. Initialise libsodium (§4.1 step 4).
    if (sodium_init() < 0) abort();

    // 3. Create the self-pipe for signal delivery (§4.3).
    int fds[2];
    if (pipe(fds) != 0) abort();
    g_pipe_rd = fds[0];
    g_pipe_wr = fds[1];

    // 4. Install signal handlers (§4.1 step 5).
    install_handlers();
}

void register_zero_on_exit(std::function<void()> cb) {
    std::lock_guard lock(g_cb_mutex);
    g_callbacks.push_back(std::move(cb));
}

int signal_pipe_read_fd() { return g_pipe_rd; }

bool shutdown_requested() { return g_shutdown.load(std::memory_order_relaxed); }
