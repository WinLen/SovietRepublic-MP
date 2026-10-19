// SPDX-License-Identifier: GPL-3.0-only
// True only while the game window has focus, so INSERT does not toggle the
// overlay while alt-tabbed.
#pragma once

#include "tesmio_plugin.h"

inline bool IsGameWindowFocused()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgProcessId = 0;
    GetWindowThreadProcessId(fg, &fgProcessId);
    return fgProcessId == GetCurrentProcessId();
}
