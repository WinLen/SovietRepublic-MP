// SPDX-License-Identifier: GPL-3.0-only
// Debug window (opened from the main menu): manual sync connections and the
// verbose sync log.
#pragma once

class DebugWindow
{
public:
    void Open();
    void Close();
    void Draw();

private:
    bool isOpen = false;
};

extern DebugWindow g_debug_window;
