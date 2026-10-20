// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

enum MpLang {
    MP_LANG_RU = 0,
    MP_LANG_EN = 1
};

__declspec(selectany) volatile int g_mpLang = MP_LANG_RU;
__declspec(selectany) bool g_mpLangInit = false;

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
    if (pos == std::string::npos) return "republicmp_updater.ini";
    return dll.substr(0, pos + 1) + "republicmp_updater.ini";
}

inline void Lang_EnsureInit()
{
    if (g_mpLangInit) return;
    g_mpLangInit = true;
    std::string ini = Lang_GetConfigPath();
    char buf[16] = {};
    GetPrivateProfileStringA("ui", "lang", "", buf, sizeof(buf), ini.c_str());
    if (_stricmp(buf, "en") == 0) {
        g_mpLang = MP_LANG_EN;
    } else if (_stricmp(buf, "ru") == 0) {
        g_mpLang = MP_LANG_RU;
    } else {
        LANGID lid = GetUserDefaultUILanguage();
        WORD prim = PRIMARYLANGID(lid);
        if (prim == LANG_RUSSIAN || prim == LANG_UKRAINIAN || prim == LANG_BELARUSIAN)
            g_mpLang = MP_LANG_RU;
        else
            g_mpLang = MP_LANG_EN;
    }
}

inline int Lang_Get()
{
    if (!g_mpLangInit) Lang_EnsureInit();
    return g_mpLang;
}

inline void Lang_Set(int lang)
{
    g_mpLangInit = true;
    g_mpLang = (lang == MP_LANG_EN) ? MP_LANG_EN : MP_LANG_RU;
    std::string ini = Lang_GetConfigPath();
    WritePrivateProfileStringA("ui", "lang", (g_mpLang == MP_LANG_EN) ? "en" : "ru", ini.c_str());
}

inline void Lang_Toggle()
{
    Lang_Set(Lang_Get() == MP_LANG_RU ? MP_LANG_EN : MP_LANG_RU);
}

inline const char* Tr(const char* ru, const char* en)
{
    return (Lang_Get() == MP_LANG_EN) ? en : ru;
}
