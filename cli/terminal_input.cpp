#include "terminal_input.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#include <sodium.h>
#else
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#endif

// ---------- POSIX ----------
#if !defined(_WIN32)

static struct termios g_saved_termios;
static bool           g_termios_saved = false;

static void restore_terminal() {
    if (g_termios_saved)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
}

static void term_signal_handler(int) {
    restore_terminal();
    // Re-raise so the process actually terminates.
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    raise(SIGINT);
}

SecureBuffer read_secret(std::string_view prompt_text) {
    // Print the prompt.
    std::cerr << prompt_text << std::flush;

    // Save current terminal settings.
    if (tcgetattr(STDIN_FILENO, &g_saved_termios) != 0)
        throw std::runtime_error("terminal_input: tcgetattr failed");
    g_termios_saved = true;
    std::atexit(restore_terminal);

    // Install signal handler so we always restore on Ctrl-C.
    struct sigaction sa{};
    sa.sa_handler = term_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    // Disable ECHO, set raw mode (§7.2).
    struct termios raw = g_saved_termios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // Read bytes one at a time into a SecureBuffer.
    SecureBuffer buf(1024);
    size_t len = 0;
    while (true) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) break;
        if (c == '\n' || c == '\r') break;
        if ((c == 127 || c == '\b') && len > 0) {
            --len;
            // Zero the removed byte.
            static_cast<char*>(buf.data())[len] = '\0';
            continue;
        }
        if (len < buf.size() - 1) {
            static_cast<char*>(buf.data())[len++] = c;
        } else {
            buf.resize(buf.size() + 512);
            static_cast<char*>(buf.data())[len++] = c;
        }
    }

    // Restore terminal (§7.2).
    restore_terminal();
    g_termios_saved = false;
    std::cerr << "\n";

    // Clear the prompt line from the terminal scrollback (§7.2).
    // \033[1A = move cursor up one line; \033[2K = erase entire line.
    std::cerr << "\033[1A\033[2K" << std::flush;

    // Return a right-sized buffer containing only the entered bytes.
    SecureBuffer result(len);
    std::memcpy(result.data(), buf.data(), len);
    // buf destructor zeroes the oversized scratch space.
    return result;
}

// ---------- Windows ----------
#else

SecureBuffer read_secret(std::string_view prompt_text) {
    std::cerr << prompt_text << std::flush;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  mode = 0;
    GetConsoleMode(hIn, &mode);
    // Clear ENABLE_ECHO_INPUT (§7.2).
    SetConsoleMode(hIn, mode & ~ENABLE_ECHO_INPUT);

    SecureBuffer buf(1024);
    size_t len = 0;

    wchar_t wc;
    DWORD   read_count;
    while (ReadConsoleW(hIn, &wc, 1, &read_count, nullptr) && read_count == 1) {
        if (wc == L'\n' || wc == L'\r') break;
        if ((wc == L'\b' || wc == 127) && len > 0) {
            --len;
            static_cast<char*>(buf.data())[len] = '\0';
            continue;
        }
        // Convert BMP UTF-16 to UTF-8 naively (sufficient for API keys).
        if (wc < 0x80) {
            if (len < buf.size() - 1)
                static_cast<char*>(buf.data())[len++] = static_cast<char>(wc);
        }
    }

    // Restore console mode.
    SetConsoleMode(hIn, mode);
    std::cerr << "\n";

    // Wipe the scratch buffer. SecureZeroMemory is used on Windows (§7.2).
    SecureBuffer result(len);
    std::memcpy(result.data(), buf.data(), len);
    SecureZeroMemory(buf.data(), buf.size());
    return result;
}
#endif
