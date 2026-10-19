// SPDX-License-Identifier: GPL-3.0-only
// Ring buffer of verbose sync status lines, shown in the Debug window.
// Same shape as ui_log.h.
#pragma once

#include <string>
#include <vector>

void DebugLog_Add(const char* line);
void DebugLog_Clear();

// Thread-safe copy for rendering.
std::vector<std::string> DebugLog_Snapshot();
