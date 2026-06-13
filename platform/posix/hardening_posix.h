#pragma once
#include "../interfaces.h"
#include <functional>
#include <vector>

// Common POSIX hardening shared between Linux and macOS (§4.1, §4.3).
// harden_common_posix() is called by both platform implementations.
void harden_common_posix();

// Register a callback invoked (in signal-handler context) to zero secrets
// before the process exits on a caught signal.
void register_zero_on_exit(std::function<void()> cb);

// Read end of the self-pipe written to by signal handlers (§4.3).
// The agent's main loop polls this fd via select/poll to detect shutdown.
int signal_pipe_read_fd();

// Returns true once a shutdown signal (SIGTERM/SIGINT/SIGHUP/SIGQUIT) has fired.
bool shutdown_requested();

// Concrete IProcessHardening for POSIX platforms.
// Derived by hardening_linux.cpp and hardening_macos.cpp which add their own
// platform-specific steps before delegating to harden_common_posix().
class PosixHardening final : public IProcessHardening {
public:
    void harden() override; // defined in hardening_linux.cpp or hardening_macos.cpp
};
