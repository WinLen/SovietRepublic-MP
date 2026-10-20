// SPDX-License-Identifier: GPL-3.0-only
#include "debug_log.h"
#include "../core/diag_log.h"

void DebugLog_Add(const char* line)
{
    DiagLog_Printf("DEBUG", "%s", line ? line : "");
}
