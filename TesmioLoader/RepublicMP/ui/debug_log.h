// SPDX-License-Identifier: GPL-3.0-only
// Forwards verbose sync status lines to the unified diagnostic log.
#pragma once

#include <stdio.h>
#include <stdarg.h>

void DebugLog_Add(const char* line);

template <typename LogFnT>
inline void DebugLog_FormatStatus(LogFnT logFn, const char* prefix, const char* fmt, va_list ap)
{
    char body[512] = {};
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    if (logFn) logFn("republicmp  %s: %s", prefix, body);
    DebugLog_Add(body);
}
