// SPDX-License-Identifier: GPL-3.0-only
// Ring buffer of everyday status lines (save sync), shown in the main window.
#pragma once

#include <string>
#include <vector>

void UiLog_Add(const char* line);
void UiLog_Clear();

// Thread-safe copy for rendering.
std::vector<std::string> UiLog_Snapshot();
