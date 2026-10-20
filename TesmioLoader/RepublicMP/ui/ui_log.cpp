// SPDX-License-Identifier: GPL-3.0-only
#include "ui_log.h"
#include "../core/diag_log.h"

void UiLog_Add(const char* line)
{
    DiagLog_Printf("UI", "%s", line ? line : "");
}
