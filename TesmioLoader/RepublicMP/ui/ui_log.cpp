// SPDX-License-Identifier: GPL-3.0-only
#include "ui_log.h"
#include <windows.h>
#include <mutex>
#include <cstdio>

static std::mutex g_logMutex;
static std::vector<std::string> g_lines;
static const size_t kMaxLines = 500;

void UiLog_Add(const char* line)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[600];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d  %s", t.wHour, t.wMinute, t.wSecond, line);

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_lines.push_back(buf);
    if (g_lines.size() > kMaxLines)
        g_lines.erase(g_lines.begin(), g_lines.begin() + (g_lines.size() - kMaxLines));
}

void UiLog_Clear()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_lines.clear();
}

std::vector<std::string> UiLog_Snapshot()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_lines;
}
