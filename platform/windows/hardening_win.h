// platform/windows/hardening_win.h
#pragma once
#include "../interfaces.h"
#include <functional>
#include <windows.h>

class WindowsHardening final : public IProcessHardening {
public:
    void harden() override;
};

// ---------- Shared lifecycle interface ----------
// Matches the free functions in hardening_posix.h so main.cpp can use them
// unchanged on either platform.

// Register a callback to run during shutdown. Callbacks run in registration
// order from the console control handler thread (NOT atexit) because Windows
// hard-kills the process ~5s after CTRL_CLOSE_EVENT returns.
void register_zero_on_exit(std::function<void()> cb);

// Manual-reset event signalled on Ctrl-C / close / logoff / shutdown.
// Windows equivalent of signal_pipe_read_fd() -- pass to WaitForSingleObject
// or WaitForMultipleObjects to wake from blocking IPC reads.
HANDLE signal_event();

// True once a shutdown event has fired.
bool shutdown_requested();
