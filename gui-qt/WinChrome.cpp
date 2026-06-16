// Isolated Windows chrome code. We keep WIN32_LEAN_AND_MEAN (avoids MinGW's
// winscard.h/propidl.h pulling in COM types) and forward-declare the single DWM
// function we need rather than including the troublesome <dwmapi.h>.
#include <windows.h>
#include "WinChrome.h"

extern "C" HRESULT WINAPI DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD);

void applyDarkWindowChrome(unsigned long long winId) {
    HWND hwnd = reinterpret_cast<HWND>(winId);
    if (!hwnd) return;

    // Dark native title bar. 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 2004+);
    // 19 is the value on 1809–1909.
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    // Remove the title-bar icon (keeps the taskbar icon) → only window controls.
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_DLGMODALFRAME);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
