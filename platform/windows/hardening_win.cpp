// platform/windows/hardening_win.cpp
#include "hardening_win.h"

#ifdef _WIN32

#include <atomic>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>

#include <windows.h>
#include <sodium.h>

// ---------- lifecycle globals ----------

static HANDLE                              g_signal_event = nullptr;
static std::atomic<bool>                   g_shutdown{false};
static std::mutex                          g_cb_mutex;
static std::vector<std::function<void()>>  g_callbacks;

// ---------- console control handler ----------
//
// Runs on a dedicated OS thread (NOT signal context, so std::mutex etc. are
// fine). For CTRL_CLOSE_EVENT / CTRL_LOGOFF_EVENT / CTRL_SHUTDOWN_EVENT,
// Windows allows ~5 seconds before hard-killing the process. We run the
// zero-on-exit callbacks synchronously inside the handler so secrets are
// wiped before that window closes.
static BOOL WINAPI win_console_ctrl_handler(DWORD ctrl) {
    g_shutdown.store(true, std::memory_order_release);
    if (g_signal_event) (void)SetEvent(g_signal_event);

    switch (ctrl) {
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            std::lock_guard lock(g_cb_mutex);
            for (auto& cb : g_callbacks) {
                try { cb(); } catch (...) { /* swallow */ }
            }
            return TRUE;
        }
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            return TRUE;  // main() polls shutdown_requested() and unwinds
        default:
            return FALSE;
    }
}

static void suppress_wer_dialogs() {
    HMODULE wer = GetModuleHandleW(L"wer.dll");
    bool loaded = false;
    if (!wer) {
        wer = LoadLibraryW(L"wer.dll");
        loaded = (wer != nullptr);
    }
    if (!wer) return;

    using WerAddExcludedApplicationFn = BOOL (WINAPI *)(PCWSTR, BOOL);
    auto add_excluded_application = reinterpret_cast<WerAddExcludedApplicationFn>(
        GetProcAddress(wer, "WerAddExcludedApplication"));
    if (add_excluded_application) {
        add_excluded_application(L"zima-agent.exe", FALSE);
    }

    if (loaded) FreeLibrary(wer);
}

// ---------- WindowsHardening::harden() ----------
//
// Windows process hardening (§4.1). All mitigation policies are one-way:
// once set they cannot be relaxed for the lifetime of the process.
void WindowsHardening::harden() {
    // 1. Suppress WER dialogs and exclude the process from crash-dump capture.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    suppress_wer_dialogs();

    // 2. Binary signature policy: prefer static linking so this is not hit,
    //    but set ProcessImageLoadPolicy instead of MicrosoftSignedOnly to allow
    //    our own Authenticode-signed DLLs if any are present.
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig{};
    sig.MicrosoftSignedOnly = 0; // relaxed; see §4.1 note on static linking
    SetProcessMitigationPolicy(ProcessSignaturePolicy, &sig, sizeof sig);

    // 3. Prohibit dynamic code generation (JIT, VirtualAlloc(PAGE_EXECUTE_*)).
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn{};
    dyn.ProhibitDynamicCode = 1;
    SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dyn, sizeof dyn);

    // 4. Disable legacy extension points (AppInit_DLLs, SetWindowsHookEx chains).
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext{};
    ext.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof ext);

    // 5. Reject remote and low-IL images.
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY img{};
    img.NoRemoteImages            = 1;
    img.NoLowMandatoryLabelImages = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &img, sizeof img);

    // 6. Initialise libsodium.
    if (sodium_init() < 0) std::abort();

    // 7. Create the shutdown event (Windows equivalent of POSIX self-pipe).
    //    Manual-reset so multiple waiters all see the signal until reset.
    g_signal_event = CreateEventW(/*lpEventAttributes=*/nullptr,
                                  /*bManualReset  =*/TRUE,
                                  /*bInitialState =*/FALSE,
                                  /*lpName        =*/nullptr);
    if (!g_signal_event) std::abort();

    // 8. Install console control handler. Replaces the POSIX self-pipe +
    //    sigaction setup.
    if (!SetConsoleCtrlHandler(win_console_ctrl_handler, TRUE)) std::abort();
}

// ---------- public lifecycle interface ----------

void register_zero_on_exit(std::function<void()> cb) {
    std::lock_guard lock(g_cb_mutex);
    g_callbacks.push_back(std::move(cb));
}

HANDLE signal_event() { return g_signal_event; }

bool shutdown_requested() {
    return g_shutdown.load(std::memory_order_acquire);
}

#else
void WindowsHardening::harden() {}
#endif
