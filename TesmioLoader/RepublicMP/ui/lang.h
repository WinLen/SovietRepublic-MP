// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <string>

enum MpLang {
    MP_LANG_RU = 0,
    MP_LANG_EN = 1
};

inline std::atomic<int>  g_mpLang{MP_LANG_RU};
inline std::atomic<bool> g_mpLangInit{false};
inline std::atomic<bool> g_mpLangUserSet{false};
inline INIT_ONCE         g_mpLangLockOnce = INIT_ONCE_STATIC_INIT;
inline CRITICAL_SECTION  g_mpLangLock;

struct LangScopedLock {
    LangScopedLock() {
        InitOnceExecuteOnce(&g_mpLangLockOnce, [](PINIT_ONCE, PVOID param, PVOID*) -> BOOL {
            InitializeCriticalSection(static_cast<CRITICAL_SECTION*>(param));
            return TRUE;
        }, &g_mpLangLock, nullptr);
        EnterCriticalSection(&g_mpLangLock);
    }
    ~LangScopedLock() {
        LeaveCriticalSection(&g_mpLangLock);
    }
    LangScopedLock(const LangScopedLock&) = delete;
    LangScopedLock& operator=(const LangScopedLock&) = delete;
};

inline std::string Lang_GetConfigPath()
{
    char path[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&g_mpLang, &hMod)) {
        GetModuleFileNameA(hMod, path, MAX_PATH);
    }
    std::string dll(path);
    size_t pos = dll.find_last_of("\\/");
    if (pos == std::string::npos) return "republicmp_settings.ini";
    return dll.substr(0, pos + 1) + "republicmp_settings.ini";
}

inline void Lang_EnsureInit()
{
    if (g_mpLangInit.load(std::memory_order_acquire)) return;
    LangScopedLock lock;
    if (g_mpLangInit.load(std::memory_order_relaxed)) return;

    std::string ini = Lang_GetConfigPath();
    char buf[16] = {};
    GetPrivateProfileStringA("ui", "lang", "", buf, sizeof(buf), ini.c_str());
    if (_stricmp(buf, "en") == 0) {
        g_mpLang.store(MP_LANG_EN, std::memory_order_relaxed);
        g_mpLangUserSet.store(true, std::memory_order_relaxed);
    } else if (_stricmp(buf, "ru") == 0) {
        g_mpLang.store(MP_LANG_RU, std::memory_order_relaxed);
        g_mpLangUserSet.store(true, std::memory_order_relaxed);
    } else {
        LANGID lid = GetUserDefaultUILanguage();
        WORD prim = PRIMARYLANGID(lid);
        if (prim == LANG_RUSSIAN || prim == LANG_UKRAINIAN || prim == LANG_BELARUSIAN)
            g_mpLang.store(MP_LANG_RU, std::memory_order_relaxed);
        else
            g_mpLang.store(MP_LANG_EN, std::memory_order_relaxed);
    }
    g_mpLangInit.store(true, std::memory_order_release);
}

inline int Lang_Get()
{
    Lang_EnsureInit();
    return g_mpLang.load(std::memory_order_acquire);
}

inline void Lang_Set(int lang)
{
    LangScopedLock lock;
    const int normalized = (lang == MP_LANG_EN) ? MP_LANG_EN : MP_LANG_RU;
    g_mpLang.store(normalized, std::memory_order_relaxed);
    g_mpLangUserSet.store(true, std::memory_order_relaxed);
    g_mpLangInit.store(true, std::memory_order_release);
    std::string ini = Lang_GetConfigPath();
    WritePrivateProfileStringA("ui", "lang", (normalized == MP_LANG_EN) ? "en" : "ru", ini.c_str());
}

inline void Lang_AutoDetectFromGame(int lang)
{
    Lang_EnsureInit();
    LangScopedLock lock;
    if (!g_mpLangUserSet.load(std::memory_order_relaxed)) {
        g_mpLang.store((lang == MP_LANG_EN) ? MP_LANG_EN : MP_LANG_RU, std::memory_order_relaxed);
    }
}

inline void Lang_Toggle()
{
    Lang_Set(Lang_Get() == MP_LANG_RU ? MP_LANG_EN : MP_LANG_RU);
}

inline const char* Tr(const char* ru, const char* en)
{
    return (Lang_Get() == MP_LANG_EN) ? en : ru;
}

inline const wchar_t* TrW(const wchar_t* ru, const wchar_t* en)
{
    return (Lang_Get() == MP_LANG_EN) ? en : ru;
}

inline const char* Lang_CopyIpLabel()
{
    return Tr("Копировать IP", "Copy IP");
}

inline const wchar_t* Lang_CopyIpLabelW()
{
    return TrW(L"Копировать IP", L"Copy IP");
}

inline const char* Lang_CopiedLabel()
{
    return Tr("Скопировано!", "Copied!");
}

inline const wchar_t* Lang_CopiedLabelW()
{
    return TrW(L"IP скопирован!", L"IP Copied!");
}

inline const char* Lang_RefreshIpLabel()
{
    return Tr("Обновить IP", "Refresh IP");
}

inline const char* Lang_SteamOfflineLabel()
{
    return Tr("Steam: офлайн (режим LAN)", "Steam: Offline (LAN mode)");
}

inline const wchar_t* Lang_SteamOfflineLabelW()
{
    return TrW(L"Steam: офлайн (LAN)", L"Steam: Offline (LAN)");
}

inline const char* Lang_CopySteamCodeLabel()
{
    return Tr("Копировать код Steam", "Copy Steam Code");
}

inline const wchar_t* Lang_CopySteamCodeLabelW()
{
    return TrW(L"Скопировать код Steam", L"Copy Steam Code");
}
