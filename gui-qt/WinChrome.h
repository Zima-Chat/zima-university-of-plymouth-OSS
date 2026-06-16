#pragma once
// Windows-only native window chrome tweaks, isolated in their own translation
// unit so <windows.h>/<dwmapi.h> never mix with Qt headers (and so this file
// can undefine WIN32_LEAN_AND_MEAN before including them).

// Give the window a dark title bar and drop its title-bar icon, leaving only
// the min/max/close controls. `winId` is the value from QWidget::winId().
void applyDarkWindowChrome(unsigned long long winId);
