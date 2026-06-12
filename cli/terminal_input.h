#pragma once
// Secure terminal input for API key entry (§7.2).
// Disables ECHO and reads into a SecureBuffer. Restores terminal state on
// completion, on signal, and via an atexit handler. Also clears the lines
// containing the prompt from the terminal's scrollback on POSIX.

#include "../agent/secure_buffer.h"
#include <string_view>

// Prompt the user (displayed without echo) and return the input in a
// SecureBuffer. The prompt string is shown, then ECHO is disabled, then
// input is read byte-by-byte. Backspace is honoured. Ctrl-C causes abort.
// After the call the terminal is restored and the prompt lines are cleared.
SecureBuffer read_secret(std::string_view prompt_text);
