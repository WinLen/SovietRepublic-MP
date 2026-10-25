// SPDX-License-Identifier: GPL-3.0-only
// See native_menu.h.
#include "native_menu.h"
#include "lang.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../session/mod_sync.h"
#include "../determinism/determinism.h"
#include "../lockstep/lockstep_net.h"
#include "../transport/steam_p2p.h"
#include "../game/save_load.h"
#include "../game/window_focus.h"
#include "imgui.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <climits>
#include <cmath>
#include <vector>
#include <string>

// SOVIET64.exe native menu constants (verified via disassembly of 0x140261400 & 0x14028AF60)
#define MENU_CALL_SITE_RVA      0x261897
#define MENU_BUILD_FN_RVA       0x28AF60
#define MENU_OBJ_RVA            0x9D4F10

// SOVIET64.exe in-game Left Sidebar 'system' (Hammer & Sickle) tab constants
// (verified via disassembly of 0x14031E800, 0x140337450, 0x14021CC20)
#define SYSTAB_CALL_SITE_RVA    0x31EAF8
#define SYSTAB_BUILD_FN_RVA     0x337450
#define SYSTAB_ROOT_TITLE_RVA   0x337D7B  // 6-byte call [0x14086C880] drawing "Меню параметров" when C65C < 0
#define IMPORT_CREATE_TEX_RVA   0x86C4C0
#define IMPORT_DRAW_BOX_RVA     0x86C858
#define IMPORT_PANEL_DRAW_RVA   0x86C860
#define IMPORT_MOUSE_POS_RVA    0x86C868
#define IMPORT_DRAW_TEXT_RVA    0x86C880
#define IMPORT_GUI_YOFF_RVA     0x86CC60
#define IMPORT_HAS_SOUND_RVA    0x86CD10
#define IMPORT_GET_SOUND_RVA    0x86CD18
#define IMPORT_PLAY_SOUND_RVA   0x86D2A8
#define SND_CLICK_NAME_RVA      0x88F920
#define FMT_WSTR_RVA            0x8B5528  // L"%ls"
#define GUI_SCALE_RVA           0x992088
#define FONT_REGULAR_RVA        0x995220
#define FONT_HEADER_RVA         0x995240
#define FONT_MGR_RVA            0x996FB0
#define GUI_OBJ_RVA             0x9BE060
#define GUI_DIM_W_RVA           0x9BE2E8
#define GUI_DIM_H_RVA           0x9BE2EC
#define GUI_POS_X_RVA           0x9BE2F0
#define GUI_POS_Y_RVA           0x9BE2F4
#define GUI_POS_Z_RVA           0x9BE2F8
#define GUI_COLOR_RGBA_RVA      0x9BE30C
#define SOUND_MUTE_BYTE_RVA     0x9D4F24
#define BTN_FRAME_TEX_RVA       0x9DFF38
#define SOUND_ENGINE_PTR_RVA    0x9E4C18
#define SOUND_MGR_RVA           0x9E4C20
#define MIDDLEPOINT_OBJ_RVA     0x9EACD0
#define TEX_BIND_ARG_RVA        0x9EAD08
#define WINDOW_FOCUS_BYTE_RVA   0x99208F
#define INPUT_OBJ_RVA           0xA54B90
#define INPUT_LHELD_RVA         0xA54E90
#define INPUT_LCLICK_RVA        0xA54E91

#define SYSTAB_CHEAT_BYTE_OFF   0x0009
#define SYSTAB_PANEL_W_OFF      0xAE54
#define SYSTAB_PANEL_H_OFF      0xAE58
#define SYSTAB_TAB_PTR_OFF      0xC650
#define SYSTAB_SUBTAB_IDX_OFF   0xC65C

#define MENU_ITEM_COUNT_OFFSET  0x11B0
#define MENU_ITEMS_OFFSET       0x11B4
#define MENU_ITEM_STRIDE        0x204   // wchar_t text[256] (0x200 bytes) + int iconFlag (4 bytes)
#define MENU_ITEM_MAX_SLOTS     28
#define MENU_TITLE_OFFSET       0x4E2C  // wchar_t title[256]
#define MENU_HAS_CONTINUE_OFF   0x502C  // BYTE: 1 if "Continue" save exists
#define MENU_CLICKED_FLAG_OFF   0x5038  // int: 1 if a button was clicked on the previous frame
#define MENU_CLICKED_IDX_OFF    0x503C  // int: index of the clicked button
#define MENU_SCREEN_ID_OFF      0x5044  // int: 1=Main, 2=Load, 0x62=Campaign, 0x63=CustomMap, 3=CustomSetup

typedef void (__fastcall* MainMenuBuildFn)(BYTE* menuObj);
typedef void (__fastcall* SystemTabBuildFn)(BYTE* menuObj);
typedef void (__cdecl* FnDrawTextFmt)(void* fontMgr, void* font, float x, float y,
                                      unsigned int colorARGB, const wchar_t* fmt, ...);
typedef int* (__fastcall* FnGetTextRect)(void* font, int* outRect, float x, float y, const wchar_t* text);
typedef void* (__fastcall* FnCreateManagedTex)(void* middlePoint, const char* relPath);
typedef int (__fastcall* FnTexLoadFile)(void* tex, const char* relPath, int a3, int a4, int a5, int a6);
typedef void (__fastcall* FnBindTex)(void* tex, int stage, void* arg);
typedef void (__fastcall* FnDrawBox)(void* guiObj, const int* rect, float s0, float s1,
                                     float u0, float u1, float v0, float v1);
typedef void (__fastcall* FnPanelDraw)(void* guiObj, float u0, float v0, float u1, float v1, float rot, bool flag);
typedef float* (__fastcall* FnGetMousePos)(void* inputObj, float* outPt);
typedef float (__fastcall* FnGetGuiYOff)(void* guiObj);
typedef bool (__fastcall* FnHasSound)(void* soundCfg, const char* name);
typedef const char* (__fastcall* FnGetSound)(void* soundCfg, const char* name);
typedef bool (__fastcall* FnPlaySound2D)(void* soundEngine, const char* soundPath, bool flag, float vol, const char* tag);

static MainMenuBuildFn g_origMainMenuBuild = nullptr;
static SystemTabBuildFn g_origSystemTabBuild = nullptr;
static BYTE* g_menuRelay = nullptr;
static bool g_inGameCoopSubTab = false;

enum NativeMenuPage {
    NATIVE_PAGE_ROOT = 0,
    NATIVE_PAGE_COOP_HUB = 1,
    NATIVE_PAGE_HOST = 2,
    NATIVE_PAGE_JOIN = 3,
    NATIVE_PAGE_SETTINGS = 4,
};

static NativeMenuPage g_nativePage = NATIVE_PAGE_ROOT;
static NativeMenuPage g_settingsReturnPage = NATIVE_PAGE_COOP_HUB;
static volatile bool g_hostOnNextLoad = false;
static bool g_delegatedToLaunchScreen = false;
static bool g_wasWorldLoaded = false;
static int g_lastScreenId = 1;

static char g_joinAddress[64] = "127.0.0.1";
static bool g_joinAddressInit = false;
static bool g_showManualJoinModal = false;
static bool g_overlayHovered = false;
static bool g_escWasDown = false;

static std::vector<SteamFriendEntry> g_cachedFriends;
static ULONGLONG g_lastFriendsRefreshMs = 0;

void NativeMenu_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool NativeMenu_IsHostOnNextLoad(void)
{
    return g_hostOnNextLoad;
}

int NativeMenu_GetActiveScreenId(void)
{
    if (!g_exeBase) return 0;
    __try {
        return *(int*)(g_exeBase + MENU_OBJ_RVA + MENU_SCREEN_ID_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool NativeMenu_WantsInput(void)
{
    return g_overlayHovered || g_showManualJoinModal;
}

static BYTE* GetMenuItemSlot(BYTE* menuObj, int idx)
{
    return menuObj + MENU_ITEMS_OFFSET + (size_t)idx * MENU_ITEM_STRIDE;
}

static void SetMenuItemText(BYTE* menuObj, int idx, const wchar_t* text)
{
    if (idx < 0 || idx >= MENU_ITEM_MAX_SLOTS) return;
    BYTE* slot = GetMenuItemSlot(menuObj, idx);
    memset(slot, 0, MENU_ITEM_STRIDE);
    wcsncpy_s((wchar_t*)slot, 256, text ? text : L"", _TRUNCATE);
}

static void SetMenuTitleText(BYTE* menuObj, const wchar_t* title)
{
    wchar_t* dst = (wchar_t*)(menuObj + MENU_TITLE_OFFSET);
    wcsncpy_s(dst, 256, title ? title : L"", _TRUNCATE);
}

static void Utf8ToWide(const char* utf8, wchar_t* outWide, size_t maxChars)
{
    if (!outWide || maxChars == 0) return;
    outWide[0] = L'\0';
    if (!utf8 || !utf8[0]) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, outWide, (int)maxChars);
}

static const wchar_t* TrW(const wchar_t* ru, const wchar_t* en)
{
    return (Lang_Get() == MP_LANG_EN) ? en : ru;
}

static void RefreshFriendsIfNeeded(bool force)
{
    ULONGLONG now = GetTickCount64();
    if (force || SteamP2P_ConsumeHostProbeDirty() || now - g_lastFriendsRefreshMs > 1500) {
        g_lastFriendsRefreshMs = now;
        if (SteamP2P_IsAvailable()) {
            SteamP2P_GetOnlineFriends(g_cachedFriends);
        } else {
            g_cachedFriends.clear();
        }
    }
}

static void CopyMySteamCodeToClipboard(void)
{
    if (!SteamP2P_IsAvailable()) return;
    char code[64];
    _snprintf_s(code, sizeof(code), _TRUNCATE, "steam:%llu", (unsigned long long)SteamP2P_GetLocalSteamId());
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        size_t len = strlen(code) + 1;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hMem) {
            memcpy(GlobalLock(hMem), code, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
    TwoStates_AddToast(Tr("Ваш код steam:<ID> скопирован в буфер обмена!", "Your steam:<ID> code copied to clipboard!"), 1);
}

static void PasteAddressFromClipboard(void)
{
    if (!OpenClipboard(nullptr)) return;
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (hData) {
        const char* txt = (const char*)GlobalLock(hData);
        if (txt) {
            char buf[128] = {};
            strncpy_s(buf, txt, _TRUNCATE);
            GlobalUnlock(hData);
            char* start = buf;
            while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;
            char* end = start + strlen(start);
            while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';

            if (*start) {
                bool allDigits = true;
                size_t len = strlen(start);
                for (size_t i = 0; i < len; ++i) {
                    if (start[i] < '0' || start[i] > '9') { allDigits = false; break; }
                }
                if (allDigits && len >= 16 && strncmp(start, "7656", 4) == 0) {
                    _snprintf_s(g_joinAddress, sizeof(g_joinAddress), _TRUNCATE, "steam:%s", start);
                } else {
                    strncpy_s(g_joinAddress, start, _TRUNCATE);
                }
                TwoStates_AddToast(Tr("Адрес вставлен из буфера обмена", "Address pasted from clipboard"), 1);
            }
        }
    }
    CloseClipboard();
}

// ---------------------------------------------------------------------------
// Page 1: CO-OP HUB ("КООПЕРАТИВ") — 4 short buttons
// ---------------------------------------------------------------------------
static void BuildCoopHubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx);
static void BuildHostSubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx);
static void BuildSettingsSubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx);
static void BuildJoinSubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx);

static void BuildCoopHubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx)
{
    if (clicked == 1) {
        if (clickedIdx == 0) {
            g_nativePage = NATIVE_PAGE_HOST;
            BuildHostSubMenu(menuObj, hasContinue, 0, 0);
            return;
        } else if (clickedIdx == 1) {
            g_nativePage = NATIVE_PAGE_JOIN;
            RefreshFriendsIfNeeded(true);
            BuildJoinSubMenu(menuObj, hasContinue, 0, 0);
            return;
        } else if (clickedIdx == 2) {
            g_settingsReturnPage = NATIVE_PAGE_COOP_HUB;
            g_nativePage = NATIVE_PAGE_SETTINGS;
            BuildSettingsSubMenu(menuObj, hasContinue, 0, 0);
            return;
        } else if (clickedIdx == 3) {
            g_nativePage = NATIVE_PAGE_ROOT;
            *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
            g_origMainMenuBuild(menuObj);
            return;
        }
    }

    *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
    g_origMainMenuBuild(menuObj);

    SetMenuTitleText(menuObj, TrW(L"КООПЕРАТИВ v" REPUBLICMP_VERSION_W, L"CO-OP v" REPUBLICMP_VERSION_W));
    SetMenuItemText(menuObj, 0, TrW(L"Создать сервер",  L"Host Server"));
    SetMenuItemText(menuObj, 1, TrW(L"Подключиться",    L"Join Server"));
    SetMenuItemText(menuObj, 2, TrW(L"Настройки коопа", L"Co-op Settings"));
    SetMenuItemText(menuObj, 3, TrW(L"<< Назад",        L"<< Back"));
    *(int*)(menuObj + MENU_ITEM_COUNT_OFFSET) = 4;
}

// ---------------------------------------------------------------------------
// Page 2: HOST SERVER ("СОЗДАТЬ СЕРВЕР") — only game start options + Settings
// All labels <= 20 chars so they fit cleanly inside the native button boxes!
// ---------------------------------------------------------------------------
enum HostMenuAction {
    HACT_CONTINUE = 0,
    HACT_LOAD_SAVE,
    HACT_IMPORT_SP_SAVE,
    HACT_NEW_CUSTOM,
    HACT_NEW_CAMPAIGN,
    HACT_OPEN_SETTINGS,
    HACT_BACK,
};

struct HostMenuEntry {
    HostMenuAction action;
    wchar_t label[256];
};

static void BuildHostSubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx)
{
    char latestCoopSave[128] = {};
    bool hasCoopContinue = TwoStates_GetLatestCoopSaveName(latestCoopSave, sizeof(latestCoopSave));

    std::vector<HostMenuEntry> entries;
    auto addEntry = [&](HostMenuAction act, const wchar_t* text) {
        HostMenuEntry e = {};
        e.action = act;
        wcsncpy_s(e.label, text, _TRUNCATE);
        entries.push_back(e);
    };

    auto populate = [&]() {
        entries.clear();
        if (hasCoopContinue) {
            addEntry(HACT_CONTINUE,       TrW(L"Продолжить кооп",    L"Continue Co-op"));
        }
        addEntry(HACT_LOAD_SAVE,          TrW(L"Кооп-сохранения",    L"Co-op Saves"));
        addEntry(HACT_IMPORT_SP_SAVE,     TrW(L"Импорт одиночного",  L"Import SP Save"));
        addEntry(HACT_NEW_CUSTOM,         TrW(L"Новая своя игра",    L"New Custom Game"));
        addEntry(HACT_NEW_CAMPAIGN,       TrW(L"Новая кампания",     L"New Campaign"));
        addEntry(HACT_OPEN_SETTINGS, (TwoStates_GetMode() == COOP_MODE_TWO_STATES)
            ? TrW(L"Настройки (2 гос.)", L"Settings (2 States)")
            : TrW(L"Настройки (1 гос.)", L"Settings (1 State)"));
        addEntry(HACT_BACK,               TrW(L"<< Назад",           L"<< Back"));
    };

    populate();

    if (clicked == 1 && clickedIdx >= 0 && clickedIdx < (int)entries.size()) {
        HostMenuAction act = entries[clickedIdx].action;
        if (act == HACT_CONTINUE && hasCoopContinue) {
            TwoStates_MountSaveBank(true);
            TwoStates_SetImportingSpSave(false);
            g_hostOnNextLoad = true;
            g_delegatedToLaunchScreen = false;
            g_nativePage = NATIVE_PAGE_ROOT;
            *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
            DiagLog_Printf("UI", "Native Co-op Host menu: continuing latest Co-op save [%s]", latestCoopSave);
            TriggerLoadSave(latestCoopSave);
            return;
        }

        int delegateIdx = -1;
        if (act == HACT_NEW_CAMPAIGN) {
            TwoStates_MountSaveBank(true);
            TwoStates_SetImportingSpSave(false);
            delegateIdx = hasContinue ? 1 : 0;
        } else if (act == HACT_NEW_CUSTOM) {
            TwoStates_MountSaveBank(true);
            TwoStates_SetImportingSpSave(false);
            delegateIdx = hasContinue ? 2 : 1;
        } else if (act == HACT_LOAD_SAVE) {
            TwoStates_MountSaveBank(true);
            TwoStates_SetImportingSpSave(false);
            delegateIdx = hasContinue ? 3 : 2;
        } else if (act == HACT_IMPORT_SP_SAVE) {
            TwoStates_MountSaveBank(false);
            TwoStates_SetImportingSpSave(true);
            delegateIdx = hasContinue ? 3 : 2;
        }

        if (delegateIdx >= 0) {
            g_hostOnNextLoad = true;
            g_delegatedToLaunchScreen = true;
            g_nativePage = NATIVE_PAGE_ROOT;
            *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 1;
            *(int*)(menuObj + MENU_CLICKED_IDX_OFF)  = delegateIdx;
            DiagLog_Printf("UI", "Native Co-op Host menu: starting action %d (nativeIdx=%d) with HostOnNextLoad=1, importSp=%d",
                           (int)act, delegateIdx, TwoStates_IsImportingSpSave() ? 1 : 0);
            g_origMainMenuBuild(menuObj);
            return;
        }

        if (act == HACT_OPEN_SETTINGS) {
            g_settingsReturnPage = NATIVE_PAGE_HOST;
            g_nativePage = NATIVE_PAGE_SETTINGS;
            BuildSettingsSubMenu(menuObj, hasContinue, 0, 0);
            return;
        } else if (act == HACT_BACK) {
            g_nativePage = NATIVE_PAGE_COOP_HUB;
            BuildCoopHubMenu(menuObj, hasContinue, 0, 0);
            return;
        }
    }

    *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
    g_origMainMenuBuild(menuObj);
    populate();

    SetMenuTitleText(menuObj, TrW(L"СОЗДАТЬ СЕРВЕР", L"HOST SERVER"));
    int count = (int)entries.size();
    for (int i = 0; i < count; ++i) {
        SetMenuItemText(menuObj, i, entries[i].label);
    }
    *(int*)(menuObj + MENU_ITEM_COUNT_OFFSET) = count;
}

// ---------------------------------------------------------------------------
// Page 3: CO-OP SETTINGS ("НАСТРОЙКИ КООПА") — dedicated sub-menu for toggles
// Every label is <= 21 chars so nothing overflows native buttons!
// ---------------------------------------------------------------------------
enum SettingsMenuAction {
    SACT_MODE = 0,
    SACT_CAPITAL,
    SACT_BORDER_WIDTH,
    SACT_MAP_BORDERS,
    SACT_MODS,
    SACT_DLC_CHECK,
    SACT_SEND_LOGS,
    SACT_LANG,
    SACT_BACK,
};

struct SettingsMenuEntry {
    SettingsMenuAction action;
    wchar_t label[256];
};

static void BuildSettingsSubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx)
{
    std::vector<SettingsMenuEntry> entries;
    auto addEntry = [&](SettingsMenuAction act, const wchar_t* text) {
        SettingsMenuEntry e = {};
        e.action = act;
        wcsncpy_s(e.label, text, _TRUNCATE);
        entries.push_back(e);
    };

    auto populate = [&]() {
        entries.clear();
        bool twoStates = (TwoStates_GetMode() == COOP_MODE_TWO_STATES);
        addEntry(SACT_MODE, twoStates
            ? TrW(L"Режим: 2 государства", L"Mode: Two States")
            : TrW(L"Режим: 1 государство", L"Mode: One State"));

        if (twoStates) {
            bool fullCap = TwoStates_GetFullStartingCapital();
            addEntry(SACT_CAPITAL, fullCap
                ? TrW(L"Казна: 100% каждому", L"Budget: 100% Each")
                : TrW(L"Казна: Делить 50/50", L"Budget: Split 50/50"));

            wchar_t bwLine[64] = {};
            int bw = (int)(TwoStates_GetBorderHalfWidth() + 0.5f);
            _snwprintf_s(bwLine, _TRUNCATE,
                         TrW(L"Погранзона: %d м", L"Border Zone: %d m"), bw);
            addEntry(SACT_BORDER_WIDTH, bwLine);
        }

        bool expandBorders = TwoStates_GetExpandBorders();
        addEntry(SACT_MAP_BORDERS, expandBorders
            ? TrW(L"Карта: Вся без границ", L"Map: Full Borderless")
            : TrW(L"Карта: 20x20 с границей", L"Map: 20x20 w/ Borders"));

        ModPackMode pm = ModSync_GetPackMode();
        if (pm == MOD_PACK_ALL_SHARED) {
            addEntry(SACT_MODS, TrW(L"Моды: Общие (Авто)", L"Mods: Shared (Auto)"));
        } else if (pm == MOD_PACK_HOST_ONLY) {
            addEntry(SACT_MODS, TrW(L"Моды: Только Хоста", L"Mods: Host Only"));
        } else {
            addEntry(SACT_MODS, TrW(L"Моды: Из сейва", L"Mods: Save Only"));
        }

        bool dlc = SaveSync_GetDlcCheck();
        addEntry(SACT_DLC_CHECK, dlc
            ? TrW(L"Проверка DLC: ВКЛ",  L"DLC Check: ON")
            : TrW(L"Проверка DLC: ВЫКЛ", L"DLC Check: OFF"));

        bool sendLogs = TwoStates_GetSendClientLogs();
        addEntry(SACT_SEND_LOGS, sendLogs
            ? TrW(L"Логи Хосту: ВКЛ",  L"Logs to Host: ON")
            : TrW(L"Логи Хосту: ВЫКЛ", L"Logs to Host: OFF"));

        addEntry(SACT_LANG, (Lang_Get() == MP_LANG_EN)
            ? L"Language: English"
            : L"Язык: Русский");

        addEntry(SACT_BACK, TrW(L"<< Готово", L"<< Done"));
    };

    populate();

    if (clicked == 1 && clickedIdx >= 0 && clickedIdx < (int)entries.size()) {
        SettingsMenuAction act = entries[clickedIdx].action;
        if (act == SACT_MODE) {
            bool twoStates = (TwoStates_GetMode() == COOP_MODE_TWO_STATES);
            TwoStates_SetMode(twoStates ? COOP_MODE_ONE_STATE : COOP_MODE_TWO_STATES);
            TwoStates_SaveSettings();
        } else if (act == SACT_CAPITAL) {
            TwoStates_SetFullStartingCapital(!TwoStates_GetFullStartingCapital());
            TwoStates_SaveSettings();
        } else if (act == SACT_BORDER_WIDTH) {
            float cur = TwoStates_GetBorderHalfWidth();
            float next = 250.0f;
            if (cur < 200.0f) next = 250.0f;
            else if (cur < 400.0f) next = 500.0f;
            else if (cur < 800.0f) next = 1000.0f;
            else next = 150.0f;
            TwoStates_SetBorderHalfWidth(next);
            TwoStates_SaveSettings();
        } else if (act == SACT_MAP_BORDERS) {
            TwoStates_SetExpandBorders(!TwoStates_GetExpandBorders());
            TwoStates_SaveSettings();
        } else if (act == SACT_MODS) {
            ModSync_CyclePackMode();
            ModPackMode pm = ModSync_GetPackMode();
            if (pm == MOD_PACK_ALL_SHARED) {
                TwoStates_AddToast(Tr("Моды: Все моды Хоста, Клиента и сейва включены автоматически (обоюдная докачка из Steam)",
                                      "Mods: All Host, Client & Save mods enabled automatically (two-way Steam auto-download)"), 1);
            } else if (pm == MOD_PACK_HOST_ONLY) {
                TwoStates_AddToast(Tr("Моды: Все моды Хоста и сейва включены автоматически (Клиент докачивает моды Хоста)",
                                      "Mods: All Host & Save mods enabled automatically (Client auto-downloads Host mods)"), 0);
            } else {
                TwoStates_AddToast(Tr("Моды: Только моды, уже использованные в сохранении",
                                      "Mods: Only mods already used in the save"), 0);
            }
        } else if (act == SACT_DLC_CHECK) {
            SaveSync_SetDlcCheck(!SaveSync_GetDlcCheck());
            TwoStates_SaveSettings();
        } else if (act == SACT_SEND_LOGS) {
            bool next = !TwoStates_GetSendClientLogs();
            TwoStates_SetSendClientLogs(next);
            TwoStates_SaveSettings();
            TwoStates_AddToast(next
                ? Tr("Авто-отправка логов Хосту включена (при рассинхроне, вылете и выходе)",
                     "Auto-upload of client logs to Host enabled (on desync, crash & disconnect)")
                : Tr("Авто-отправка логов Хосту отключена",
                     "Auto-upload of client logs to Host disabled"), next ? 1 : 0);
        } else if (act == SACT_LANG) {
            Lang_Toggle();
            TwoStates_SaveSettings();
        } else if (act == SACT_BACK) {
            g_nativePage = g_settingsReturnPage;
            if (g_nativePage == NATIVE_PAGE_HOST) {
                BuildHostSubMenu(menuObj, hasContinue, 0, 0);
            } else {
                BuildCoopHubMenu(menuObj, hasContinue, 0, 0);
            }
            return;
        }
    }

    *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
    g_origMainMenuBuild(menuObj);
    populate();

    SetMenuTitleText(menuObj, TrW(L"НАСТРОЙКИ КООПА", L"CO-OP SETTINGS"));
    int count = (int)entries.size();
    for (int i = 0; i < count; ++i) {
        SetMenuItemText(menuObj, i, entries[i].label);
    }
    *(int*)(menuObj + MENU_ITEM_COUNT_OFFSET) = count;
}

// ---------------------------------------------------------------------------
// Page 4: JOIN SERVER ("ПОДКЛЮЧЕНИЕ") — short labels <= 21 chars
// ---------------------------------------------------------------------------
enum JoinMenuAction {
    JACT_FRIEND_BASE = 100,
    JACT_CONNECT_ADDR = 1,
    JACT_TYPE_MANUAL = 2,
    JACT_PASTE_CLIPBOARD = 3,
    JACT_REFRESH_FRIENDS = 4,
    JACT_BACK = 5,
    JACT_LOCAL_PC_HOST = 6,
};

struct JoinMenuEntry {
    int action;
    uint64_t steamId;
    wchar_t label[256];
};

static void BuildJoinSubMenu(BYTE* menuObj, bool hasContinue, int clicked, int clickedIdx)
{
    if (!g_joinAddressInit) {
        const char* savedAddr = TwoStates_GetSavedJoinAddress();
        if (savedAddr && savedAddr[0]) {
            strncpy_s(g_joinAddress, savedAddr, _TRUNCATE);
        } else {
            strncpy_s(g_joinAddress, SaveSync_GetTargetIp(), _TRUNCATE);
        }
        if (!g_joinAddress[0]) strncpy_s(g_joinAddress, "127.0.0.1", _TRUNCATE);
        g_joinAddressInit = true;
    }

    RefreshFriendsIfNeeded(false);

    std::vector<JoinMenuEntry> entries;
    auto addEntry = [&](int act, uint64_t sid, const wchar_t* text) {
        JoinMenuEntry e = {};
        e.action = act;
        e.steamId = sid;
        wcsncpy_s(e.label, text, _TRUNCATE);
        entries.push_back(e);
    };

    auto populateEntries = [&]() {
        entries.clear();
        if (SaveSync_IsLocalPcHostWaiting()) {
            addEntry(JACT_LOCAL_PC_HOST, 0,
                     TrW(L"Сервер на ПК (127.0.0.1)", L"Local PC Host (127.0.0.1)"));
        }

        size_t maxFriends = g_cachedFriends.size() < 6 ? g_cachedFriends.size() : 6;
        for (size_t i = 0; i < maxFriends; ++i) {
            const auto& f = g_cachedFriends[i];
            if (!f.inThisGame || !f.isHosting) continue;
            wchar_t wName[20] = {};
            Utf8ToWide(f.name, wName, 14);
            wchar_t line[64] = {};
            _snwprintf_s(line, _TRUNCATE, TrW(L"Хост: %s", L"Host: %s"), wName);
            addEntry(JACT_FRIEND_BASE + (int)i, f.steamId, line);
        }

        if (entries.empty() && SteamP2P_IsAvailable()) {
            addEntry(JACT_REFRESH_FRIENDS, 0,
                     TrW(L"Нет хостов в друзьях", L"No Friends Hosting"));
        }

        wchar_t connectLine[64] = {};
        if (_strnicmp(g_joinAddress, "steam:", 6) == 0) {
            wcsncpy_s(connectLine, TrW(L"Войти по SteamID", L"Join via SteamID"), _TRUNCATE);
        } else {
            wchar_t wAddr[24] = {};
            Utf8ToWide(g_joinAddress, wAddr, 16);
            _snwprintf_s(connectLine, _TRUNCATE, TrW(L"Войти: %s", L"Join: %s"), wAddr);
        }
        addEntry(JACT_CONNECT_ADDR, 0, connectLine);

        addEntry(JACT_TYPE_MANUAL, 0,
                 TrW(L"Ввести IP / SteamID", L"Enter IP / SteamID"));

        addEntry(JACT_PASTE_CLIPBOARD, 0,
                 TrW(L"Вставить из буфера", L"Paste Clipboard"));

        if (SteamP2P_IsAvailable() && !g_cachedFriends.empty()) {
            addEntry(JACT_REFRESH_FRIENDS, 0,
                     TrW(L"Обновить хостов", L"Refresh Hosts"));
        }

        addEntry(JACT_BACK, 0,
                 TrW(L"<< Назад", L"<< Back"));
    };

    populateEntries();

    if (clicked == 1 && clickedIdx >= 0 && clickedIdx < (int)entries.size()) {
        const JoinMenuEntry& sel = entries[clickedIdx];
        if (sel.action >= JACT_FRIEND_BASE) {
            _snprintf_s(g_joinAddress, sizeof(g_joinAddress), _TRUNCATE, "steam:%llu",
                        (unsigned long long)sel.steamId);
            DiagLog_Printf("UI", "Native Co-op Join menu: connecting to hosting Steam friend %s", g_joinAddress);
            SaveSync_StartClient(g_joinAddress);
        } else if (sel.action == JACT_LOCAL_PC_HOST) {
            strncpy_s(g_joinAddress, "127.0.0.1", _TRUNCATE);
            DiagLog_Printf("UI", "Native Co-op Join menu: connecting to Local PC Host (127.0.0.1)");
            SaveSync_StartClient("127.0.0.1");
        } else if (sel.action == JACT_CONNECT_ADDR) {
            DiagLog_Printf("UI", "Native Co-op Join menu: connecting to address %s", g_joinAddress);
            SaveSync_StartClient(g_joinAddress);
        } else if (sel.action == JACT_PASTE_CLIPBOARD) {
            PasteAddressFromClipboard();
            TwoStates_SetSavedJoinAddress(g_joinAddress);
            TwoStates_SaveSettings();
        } else if (sel.action == JACT_TYPE_MANUAL) {
            g_showManualJoinModal = true;
        } else if (sel.action == JACT_REFRESH_FRIENDS) {
            RefreshFriendsIfNeeded(true);
            if (g_cachedFriends.empty()) {
                TwoStates_AddToast(Tr("В списке друзей Steam сейчас нет открытых серверов Workers & Resources",
                                      "No open Workers & Resources Co-op servers found among your Steam friends"), 0);
            } else {
                TwoStates_AddToast(Tr("Список открытых серверов друзей обновлён!",
                                      "Open friends' servers list updated!"), 1);
            }
        } else if (sel.action == JACT_BACK) {
            g_nativePage = NATIVE_PAGE_COOP_HUB;
            BuildCoopHubMenu(menuObj, hasContinue, 0, 0);
            return;
        }
    }

    *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
    g_origMainMenuBuild(menuObj);
    populateEntries();

    SetMenuTitleText(menuObj, TrW(L"ПОДКЛЮЧЕНИЕ", L"JOIN SERVER"));
    int count = (int)entries.size();
    if (count > MENU_ITEM_MAX_SLOTS) count = MENU_ITEM_MAX_SLOTS;
    for (int i = 0; i < count; ++i) {
        SetMenuItemText(menuObj, i, entries[i].label);
    }
    *(int*)(menuObj + MENU_ITEM_COUNT_OFFSET) = count;
}

static void ResetSidebarIcons();

static void Hook_MainMenuBuildInner(BYTE* menuObj)
{
    __try {
        ResetSidebarIcons();
        bool hasContinue = (*(BYTE*)(menuObj + MENU_HAS_CONTINUE_OFF) != 0);
        int clicked = *(int*)(menuObj + MENU_CLICKED_FLAG_OFF);
        int clickedIdx = *(int*)(menuObj + MENU_CLICKED_IDX_OFF);

        // If the user backed out of Load Game / Custom Game / Campaign (which we delegated from BuildHostSubMenu),
        // return them directly to the "СОЗДАТЬ СЕРВЕР" sub-menu and restore the Singleplayer save bank!
        if (g_delegatedToLaunchScreen) {
            BYTE loadTrig = 0;
            if (g_exeBase) loadTrig = *(volatile BYTE*)(g_exeBase + LOAD_TRIGGER_DAT_RVA);
            if (loadTrig == 0 && !SaveSync_IsWorldLoaded()) {
                g_delegatedToLaunchScreen = false;
                g_hostOnNextLoad = false;
                TwoStates_SetImportingSpSave(false);
                TwoStates_MountSaveBank(false);
                g_nativePage = NATIVE_PAGE_HOST;
                clicked = 0;
                *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
            }
        }

        // Allow ESC key to navigate one level back in the Co-op sub-menus
        bool escDown = IsGameWindowFocused() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (escDown && !g_escWasDown && g_nativePage != NATIVE_PAGE_ROOT && !g_showManualJoinModal) {
            if (g_nativePage == NATIVE_PAGE_SETTINGS) {
                g_nativePage = g_settingsReturnPage;
            } else if (g_nativePage == NATIVE_PAGE_HOST || g_nativePage == NATIVE_PAGE_JOIN) {
                g_nativePage = NATIVE_PAGE_COOP_HUB;
            } else {
                g_nativePage = NATIVE_PAGE_ROOT;
            }
            clicked = 0;
            *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
        }
        g_escWasDown = escDown;

        if (g_nativePage == NATIVE_PAGE_COOP_HUB) {
            BuildCoopHubMenu(menuObj, hasContinue, clicked, clickedIdx);
            return;
        }
        if (g_nativePage == NATIVE_PAGE_HOST) {
            BuildHostSubMenu(menuObj, hasContinue, clicked, clickedIdx);
            return;
        }
        if (g_nativePage == NATIVE_PAGE_SETTINGS) {
            BuildSettingsSubMenu(menuObj, hasContinue, clicked, clickedIdx);
            return;
        }
        if (g_nativePage == NATIVE_PAGE_JOIN) {
            BuildJoinSubMenu(menuObj, hasContinue, clicked, clickedIdx);
            return;
        }

        // Root Main Menu: insert 1 clean "КООПЕРАТИВ" / "CO-OP MULTIPLAYER" button right after "Load Game"
        int insertIdx = hasContinue ? 4 : 3;
        bool openCoopHub = false;

        if (clicked == 1) {
            if (clickedIdx == insertIdx) {
                *(int*)(menuObj + MENU_CLICKED_FLAG_OFF) = 0;
                openCoopHub = true;
            } else if (clickedIdx > insertIdx) {
                *(int*)(menuObj + MENU_CLICKED_IDX_OFF) = clickedIdx - 1;
            } else {
                g_hostOnNextLoad = false;
                g_delegatedToLaunchScreen = false;
                TwoStates_SetImportingSpSave(false);
                TwoStates_MountSaveBank(false);
            }
        }

        g_origMainMenuBuild(menuObj);

        const wchar_t* item0 = (const wchar_t*)GetMenuItemSlot(menuObj, 0);
        if (item0 && item0[0]) {
            if (item0[0] >= 0x0400 && item0[0] <= 0x04FF) {
                Lang_AutoDetectFromGame(MP_LANG_RU);
            } else if ((item0[0] >= L'A' && item0[0] <= L'Z') || (item0[0] >= L'a' && item0[0] <= L'z')) {
                Lang_AutoDetectFromGame(MP_LANG_EN);
            }
        }

        if (openCoopHub) {
            g_nativePage = NATIVE_PAGE_COOP_HUB;
            DiagLog_Printf("UI", "Native Main Menu: opened Co-op Hub");
            BuildCoopHubMenu(menuObj, hasContinue, 0, 0);
            return;
        }

        int count = *(int*)(menuObj + MENU_ITEM_COUNT_OFFSET);
        // In vanilla Russian localization (sovietRussian.btf), both string 0x247 ("Continue" = load saved_last)
        // and string 0x259 ("Load Game" = open save browser) are translated as "Продолжить", causing two
        // identical "Продолжить" buttons when saved_last exists. Disambiguate slot 3 as "Загрузить игру".
        if (hasContinue && Lang_Get() == MP_LANG_RU && count >= 4) {
            SetMenuItemText(menuObj, 3, L"Загрузить игру");
        }
        if (count >= insertIdx && count + 1 <= MENU_ITEM_MAX_SLOTS) {
            memmove(GetMenuItemSlot(menuObj, insertIdx + 1),
                    GetMenuItemSlot(menuObj, insertIdx),
                    (size_t)(count - insertIdx) * MENU_ITEM_STRIDE);

            SetMenuItemText(menuObj, insertIdx,
                            TrW(L"КООПЕРАТИВ", L"CO-OP MULTIPLAYER"));
            *(int*)(menuObj + MENU_ITEM_COUNT_OFFSET) = count + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_origMainMenuBuild) g_origMainMenuBuild(menuObj);
    }
}

static void __fastcall Hook_MainMenuBuild(BYTE* menuObj)
{
    DiagLog_BeginSafeSeh();
    Hook_MainMenuBuildInner(menuObj);
    DiagLog_EndSafeSeh();
}

// ---------------------------------------------------------------------------
// In-Game Left Sidebar 'system' (Hammer & Sickle) Tab Native Integration
// ---------------------------------------------------------------------------
struct SidebarIconEntry {
    const char* relPath;
    void* texPtr;
    bool attempted;
};

static SidebarIconEntry g_sidebarIcons[] = {
    { "editor/mp_icon_coop.png",    nullptr, false },
    { "editor/mp_icon_mode.png",    nullptr, false },
    { "editor/mp_icon_ruble.png",   nullptr, false },
    { "editor/mp_icon_dollar.png",  nullptr, false },
    { "editor/mp_icon_border.png",  nullptr, false },
    { "editor/mp_icon_steam.png",   nullptr, false },
    { "editor/mp_icon_camera.png",  nullptr, false },
    { "editor/mp_icon_ping.png",    nullptr, false },
    { "editor/mp_icon_sync.png",    nullptr, false },
    { "editor/mp_icon_lang.png",    nullptr, false },
    { "editor/mp_icon_start.png",   nullptr, false },
    { "editor/mp_icon_stop.png",    nullptr, false },
    { "editor/mp_icon_back.png",    nullptr, false },
    { "editor/settings_save.png",   nullptr, false },
};

static void* g_expectedTexVtbl = nullptr;

static void ResetSidebarIcons()
{
    for (size_t i = 0; i < sizeof(g_sidebarIcons) / sizeof(g_sidebarIcons[0]); ++i) {
        g_sidebarIcons[i].texPtr = nullptr;
        g_sidebarIcons[i].attempted = false;
    }
}

static void* GetOrLoadSidebarIcon(const char* relPath)
{
    if (!g_exeBase || !relPath || !relPath[0]) return nullptr;
    for (size_t i = 0; i < sizeof(g_sidebarIcons) / sizeof(g_sidebarIcons[0]); ++i) {
        if (strcmp(g_sidebarIcons[i].relPath, relPath) == 0) {
            if (g_sidebarIcons[i].attempted && g_sidebarIcons[i].texPtr) {
                void* cached = g_sidebarIcons[i].texPtr;
                if (ReadablePtr(cached, 64) &&
                    g_expectedTexVtbl != nullptr &&
                    *(void**)cached == g_expectedTexVtbl &&
                    ReadablePtr(g_expectedTexVtbl, 16 * sizeof(void*))) {
                    return cached;
                }
                g_sidebarIcons[i].texPtr = nullptr;
                g_sidebarIcons[i].attempted = false;
            }
            if (g_sidebarIcons[i].attempted) {
                return g_sidebarIcons[i].texPtr;
            }
            g_sidebarIcons[i].attempted = true;
            uintptr_t createAddr = *(uintptr_t*)(g_exeBase + IMPORT_CREATE_TEX_RVA);
            if (!createAddr) return nullptr;
            void* mpObj = (void*)(g_exeBase + MIDDLEPOINT_OBJ_RVA);
            void* tex = ((FnCreateManagedTex)createAddr)(mpObj, relPath);
            if (tex && ReadablePtr(tex, 64)) {
                void** vtbl = *(void***)tex;
                if (vtbl && ReadablePtr(vtbl, 16 * sizeof(void*)) && vtbl[2]) {
                    g_expectedTexVtbl = (void*)vtbl;
                    ((FnTexLoadFile)vtbl[2])(tex, relPath, 0, 0, 1, 0);
                }
                g_sidebarIcons[i].texPtr = tex;
            }
            return g_sidebarIcons[i].texPtr;
        }
    }
    return nullptr;
}

static void DrawSidebarText(float x, float y, unsigned int colorARGB, bool boldHeader, const wchar_t* text)
{
    if (!g_exeBase || !text || !text[0]) return;
    uintptr_t fnAddr = *(uintptr_t*)(g_exeBase + IMPORT_DRAW_TEXT_RVA);
    if (!fnAddr) return;
    void* fontMgr = (void*)(g_exeBase + FONT_MGR_RVA);
    void* font = *(void**)(g_exeBase + (boldHeader ? FONT_HEADER_RVA : FONT_REGULAR_RVA));
    if (!font) return;
    const wchar_t* fmt = (const wchar_t*)(g_exeBase + FMT_WSTR_RVA);
    float guiScale = *(float*)(g_exeBase + GUI_SCALE_RVA);
    FnDrawTextFmt drawText = (FnDrawTextFmt)fnAddr;
    drawText(fontMgr, font, floorf(x + 0.5f), floorf(y - 7.0f * guiScale + 0.5f), colorARGB, fmt, text);
}

static bool g_prevSidebarLmbDown = false;
static bool g_sidebarLClickAvailable = false;

static void PlayMenuClickSoundSafe()
{
    if (!g_exeBase) return;
    __try {
        if (*(BYTE*)(g_exeBase + SOUND_MUTE_BYTE_RVA) == 0 && *(void**)(g_exeBase + SOUND_ENGINE_PTR_RVA) != nullptr) {
            uintptr_t hasSndAddr  = *(uintptr_t*)(g_exeBase + IMPORT_HAS_SOUND_RVA);
            uintptr_t getSndAddr  = *(uintptr_t*)(g_exeBase + IMPORT_GET_SOUND_RVA);
            uintptr_t playSndAddr = *(uintptr_t*)(g_exeBase + IMPORT_PLAY_SOUND_RVA);
            void* soundEngine = *(void**)(g_exeBase + SOUND_ENGINE_PTR_RVA);
            void* soundCfg = (void*)(g_exeBase + SOUND_MGR_RVA);
            const char* clickName = (const char*)(g_exeBase + SND_CLICK_NAME_RVA);
            if (soundEngine && hasSndAddr && getSndAddr && playSndAddr && ((FnHasSound)hasSndAddr)(soundCfg, clickName)) {
                const char* sndPath = ((FnGetSound)getSndAddr)(soundCfg, clickName);
                if (sndPath && sndPath[0]) {
                    ((FnPlaySound2D)playSndAddr)(soundEngine, sndPath, true, 1.0f, nullptr);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool DrawSidebarButton(BYTE* menuObj, float x, float y, unsigned int colorARGB,
                              const wchar_t* label, const char* iconRelPath = nullptr)
{
    if (!g_exeBase || !menuObj || !label || !label[0]) return false;
    void* font = *(void**)(g_exeBase + FONT_REGULAR_RVA);
    void* btnTex = *(void**)(g_exeBase + BTN_FRAME_TEX_RVA);
    uintptr_t drawBoxAddr   = *(uintptr_t*)(g_exeBase + IMPORT_DRAW_BOX_RVA);
    uintptr_t panelDrawAddr = *(uintptr_t*)(g_exeBase + IMPORT_PANEL_DRAW_RVA);
    uintptr_t drawTextAddr  = *(uintptr_t*)(g_exeBase + IMPORT_DRAW_TEXT_RVA);
    if (!font || !btnTex || !drawBoxAddr || !drawTextAddr) return false;

    float guiScale = *(float*)(g_exeBase + GUI_SCALE_RVA);
    float panelRight = *(float*)(menuObj + SYSTAB_PANEL_W_OFF);
    if (panelRight < 340.0f || panelRight > 600.0f) panelRight = 390.0f;

    // Measure exact glyph bounding box via font->vtable[5]
    void** fontVtbl = *(void***)font;
    FnGetTextRect getTextRect = (FnGetTextRect)fontVtbl[5];
    int textRect[4] = { 0, 0, 0, 0 };
    if (getTextRect) {
        getTextRect(font, textRect, 0.0f, 0.0f, label);
    }

    void* iconTex = GetOrLoadSidebarIcon(iconRelPath);
    int hitLeft   = (int)floorf(x + 0.5f);
    int boxHeight = (int)floorf(30.0f * guiScale + 0.5f);
    int iconPad   = iconTex ? (boxHeight + (int)floorf(5.0f * guiScale + 0.5f)) : 0;

    int boxLeft   = hitLeft + iconPad;
    int boxRight  = (int)floorf((panelRight - 16.0f) * guiScale + 0.5f);
    int minRight  = boxLeft + (textRect[2] - textRect[0]) + (int)floorf(16.0f * guiScale + 0.5f);
    if (boxRight < minRight) boxRight = minRight;
    int boxTop    = (int)floorf(y + 0.5f);
    int boxBottom = boxTop + boxHeight;
    int boxRect[4] = { boxLeft, boxTop, boxRight, boxBottom };

    // Mouse hover & click test (matching 0x14021CD85..0x14021CE3B)
    // IMPORTANT: C3D_INPUT::GetMouseSolid writes a 12-byte C3DVECTOR3 (x, y, z), so reserve 4 floats!
    uintptr_t mouseFnAddr = *(uintptr_t*)(g_exeBase + IMPORT_MOUSE_POS_RVA);
    uintptr_t yOffFnAddr  = *(uintptr_t*)(g_exeBase + IMPORT_GUI_YOFF_RVA);
    float mousePt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (mouseFnAddr) {
        ((FnGetMousePos)mouseFnAddr)((void*)(g_exeBase + INPUT_OBJ_RVA), mousePt);
    }
    float yOff = yOffFnAddr ? ((FnGetGuiYOff)yOffFnAddr)((void*)(g_exeBase + GUI_OBJ_RVA)) : 0.0f;
    float mx = mousePt[0];
    float my = mousePt[1] - yOff;

    bool hovered = (mx > (float)hitLeft && mx < (float)boxRect[2] &&
                    my > (float)boxRect[1] && my < (float)boxRect[3]);
    bool clicked = false;
    if (hovered && (*(BYTE*)(g_exeBase + INPUT_LCLICK_RVA) != 0 || g_sidebarLClickAvailable)) {
        clicked = true;
        g_sidebarLClickAvailable = false;
        *(BYTE*)(g_exeBase + INPUT_LCLICK_RVA) = 0;
        PlayMenuClickSoundSafe();
    }

    float* guiRgba = (float*)(g_exeBase + GUI_COLOR_RGBA_RVA);

    // Draw authentic 96x96 Soviet square icon on the left if available
    if (iconTex && panelDrawAddr) {
        void** iconVtbl = *(void***)iconTex;
        FnBindTex bindIcon = (FnBindTex)iconVtbl[14];
        if (bindIcon) {
            bindIcon(iconTex, 0, *(void**)(g_exeBase + TEX_BIND_ARG_RVA));
        }
        *(float*)(g_exeBase + GUI_DIM_W_RVA) = (float)boxHeight;
        *(float*)(g_exeBase + GUI_DIM_H_RVA) = (float)boxHeight;
        *(float*)(g_exeBase + GUI_POS_X_RVA) = (float)hitLeft;
        *(float*)(g_exeBase + GUI_POS_Y_RVA) = (float)boxTop;
        *(float*)(g_exeBase + GUI_POS_Z_RVA) = 0.0f;
        guiRgba[0] = 1.0f;
        guiRgba[1] = 1.0f;
        guiRgba[2] = 1.0f;
        guiRgba[3] = hovered ? 1.0f : 0.80f;
        ((FnPanelDraw)panelDrawAddr)((void*)(g_exeBase + GUI_OBJ_RVA), 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, true);
    }

    // Bind native 9-slice button frame texture and draw uniform box (matching 0x14021CEB7..0x14021CF41)
    void** texVtbl = *(void***)btnTex;
    FnBindTex bindTex = (FnBindTex)texVtbl[14];
    if (bindTex) {
        bindTex(btnTex, 0, *(void**)(g_exeBase + TEX_BIND_ARG_RVA));
    }

    guiRgba[0] = 1.0f;
    guiRgba[1] = 1.0f;
    guiRgba[2] = 1.0f;
    guiRgba[3] = hovered ? 1.0f : 0.85f;

    float borderScale = 20.0f * guiScale;
    ((FnDrawBox)drawBoxAddr)((void*)(g_exeBase + GUI_OBJ_RVA), boxRect,
                             borderScale, borderScale, 0.25f, 0.75f, 0.25f, 0.75f);

    // Center label horizontally & vertically inside the button box
    float boxCenterX = 0.5f * (float)(boxRect[0] + boxRect[2]);
    float boxCenterY = 0.5f * (float)(boxRect[1] + boxRect[3]);
    float textX = floorf(boxCenterX - 0.5f * (float)(textRect[0] + textRect[2]) + 0.5f);
    float textY = floorf(boxCenterY - 0.5f * (float)(textRect[1] + textRect[3]) + 0.5f);

    unsigned int drawCol = hovered ? 0xFF000000 : colorARGB;
    const wchar_t* fmt = (const wchar_t*)(g_exeBase + FMT_WSTR_RVA);
    ((FnDrawTextFmt)drawTextAddr)((void*)(g_exeBase + FONT_MGR_RVA), font, textX, textY, drawCol, fmt, label);

    return clicked;
}

static void Hook_SystemTabBuildInner(BYTE* menuObj)
{
    __try {
        bool focused = (*(BYTE*)(g_exeBase + WINDOW_FOCUS_BYTE_RVA) != 0) && IsGameWindowFocused();
        bool lmbDown = focused &&
                       ((*(BYTE*)(g_exeBase + INPUT_LHELD_RVA) != 0) ||
                        ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0));
        bool lmbEdge = (lmbDown && !g_prevSidebarLmbDown);
        g_prevSidebarLmbDown = lmbDown;
        g_sidebarLClickAvailable = (*(BYTE*)(g_exeBase + INPUT_LCLICK_RVA) != 0) || lmbEdge;

        int curSubTab = *(int*)(menuObj + SYSTAB_SUBTAB_IDX_OFF);
        if (curSubTab != -1) {
            // Player is in one of the game's built-in sub-tabs (Audio, Graphics, Cheats, etc.)
            g_inGameCoopSubTab = false;
            g_sidebarLClickAvailable = false;
            g_origSystemTabBuild(menuObj);
            return;
        }

        if (g_inGameCoopSubTab) {
            // Render the native sub-tab frame and Back arrow button using C65C = -2,
            // while temporarily NOPing the root title draw call at 0x337D7B so
            // "Меню параметров" does not overlap the top-left Back arrow icon.
            BYTE* titleCallSite = g_exeBase + SYSTAB_ROOT_TITLE_RVA;
            BYTE savedCallBytes[6];
            DWORD oldProt = 0;
            VirtualProtect(titleCallSite, 6, PAGE_EXECUTE_READWRITE, &oldProt);
            memcpy(savedCallBytes, titleCallSite, 6);
            memset(titleCallSite, 0x90, 6);

            BYTE savedCheat = *(BYTE*)(menuObj + SYSTAB_CHEAT_BYTE_OFF);
            *(BYTE*)(menuObj + SYSTAB_CHEAT_BYTE_OFF) = 0;
            *(int*)(menuObj + SYSTAB_SUBTAB_IDX_OFF) = -2;

            g_origSystemTabBuild(menuObj);

            memcpy(titleCallSite, savedCallBytes, 6);
            VirtualProtect(titleCallSite, 6, oldProt, &oldProt);

            *(BYTE*)(menuObj + SYSTAB_CHEAT_BYTE_OFF) = savedCheat;
            int afterSubTab = *(int*)(menuObj + SYSTAB_SUBTAB_IDX_OFF);
            *(int*)(menuObj + SYSTAB_SUBTAB_IDX_OFF) = -1;

            if (!*(void**)(menuObj + SYSTAB_TAB_PTR_OFF)) {
                g_inGameCoopSubTab = false;
                g_sidebarLClickAvailable = false;
                return;
            }
            if (afterSubTab == -1) {
                // Player clicked the native top-left Back arrow icon!
                g_inGameCoopSubTab = false;
                g_sidebarLClickAvailable = false;
                *(BYTE*)(g_exeBase + INPUT_LCLICK_RVA) = 0;
                return;
            }

            float guiScale = *(float*)(g_exeBase + GUI_SCALE_RVA);
            float leftX = guiScale * 102.0f;
            float baseEndY = (*(float*)(menuObj + SYSTAB_PANEL_H_OFF) - guiScale * 730.0f);

            // Draw sub-tab title right next to the top-left Back arrow icon (which ends at x = 140 * guiScale)
            DrawSidebarText(guiScale * 150.0f, baseEndY - guiScale * 64.0f, 0xFF505050, true,
                            TrW(L"Кооператив", L"Co-op"));

            float currentY = baseEndY - guiScale * 4.0f;
            float textStep = guiScale * 22.0f;
            float btnStep  = guiScale * 35.0f;

            SessionState st = SaveSync_GetState();
            bool inActiveGame = (st == SESSION_HOST_LISTENING || st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME);

            if (!inActiveGame && !TwoStates_IsActive()) {
                DrawSidebarText(leftX, currentY, 0xFF881111, false,
                                TrW(L"Сессия: Одиночная игра", L"Session: Singleplayer"));
                currentY += textStep;

                DrawSidebarText(leftX, currentY, 0xFF333333, false,
                                TrW(L"Откройте сервер прямо из игры:", L"Start a Co-op server in-game:"));
                currentY += textStep + guiScale * 8.0f;

                int mode = TwoStates_GetMode();
                if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                      mode == COOP_MODE_TWO_STATES
                                          ? TrW(L"Режим: 2 государства", L"Mode: Two States")
                                          : TrW(L"Режим: 1 государство", L"Mode: One State"),
                                      "editor/mp_icon_mode.png")) {
                    TwoStates_SetMode(mode == COOP_MODE_TWO_STATES ? COOP_MODE_ONE_STATE : COOP_MODE_TWO_STATES);
                    TwoStates_SaveSettings();
                }
                currentY += btnStep;

                if (TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
                    bool fullCap = TwoStates_GetFullStartingCapital();
                    if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                          fullCap ? TrW(L"Казна: 100% каждому", L"Budget: 100% Each")
                                                  : TrW(L"Казна: Делить 50/50", L"Budget: Split 50/50"),
                                          "editor/mp_icon_ruble.png")) {
                        TwoStates_SetFullStartingCapital(!fullCap);
                        TwoStates_SaveSettings();
                    }
                    currentY += btnStep;

                    bool show3D = TwoStates_GetShow3DBorder();
                    if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                          show3D ? TrW(L"3D-граница: ВКЛ [F7]", L"3D Border: ON [F7]")
                                                 : TrW(L"3D-граница: ВЫКЛ [F7]", L"3D Border: OFF [F7]"),
                                          "editor/mp_icon_border.png")) {
                        TwoStates_SetShow3DBorder(!show3D);
                        TwoStates_SaveSettings();
                    }
                    currentY += btnStep;
                }

                if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                      Lang_Get() == MP_LANG_EN ? L"Language: English" : L"Язык: Русский",
                                      "editor/mp_icon_lang.png")) {
                    Lang_Toggle();
                    TwoStates_SaveSettings();
                }
                currentY += btnStep;

                if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF155518,
                                      TrW(L"Открыть Кооп-сервер", L"Start Co-op Server"),
                                      "editor/mp_icon_start.png")) {
                    SaveSync_StartHost();
                }
                currentY += btnStep;

                if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF881818,
                                      TrW(L"<< Назад", L"<< Back"),
                                      "editor/mp_icon_back.png")) {
                    g_inGameCoopSubTab = false;
                }
                currentY += btnStep;
            } else {
                // Active Co-op Session in-game view
                if (TwoStates_IsActive()) {
                    int me = TwoStates_LocalState();
                    DrawSidebarText(leftX, currentY, 0xFF881111, false,
                                    me == 1 ? TrW(L"Роль: Западная ССР (Хост)", L"Role: West SSR (Host)")
                                            : TrW(L"Роль: Восточная ССР (Игрок 2)", L"Role: East SSR (Player 2)"));
                } else {
                    DrawSidebarText(leftX, currentY, 0xFF881111, false,
                                    TrW(L"Режим: Общая республика", L"Mode: Shared Republic"));
                }
                currentY += textStep;

                if (st == SESSION_HOST_LISTENING) {
                    DrawSidebarText(leftX, currentY, 0xFF994000, false,
                                    TrW(L"Статус: Ждём 2-го игрока...", L"Status: Waiting Player 2..."));
                    currentY += textStep;
                } else {
                    wchar_t pingLine[96] = {};
                    unsigned long long rtt = 0;
                    if (LockstepNet_RoundTrip(&rtt)) {
                        _snwprintf_s(pingLine, _TRUNCATE,
                                     TrW(L"Статус: В сети (пинг %llu мс)", L"Status: Online (ping %llu ms)"), rtt);
                    } else {
                        wcsncpy_s(pingLine, TrW(L"Статус: В сети", L"Status: Online"), _TRUNCATE);
                    }
                    DrawSidebarText(leftX, currentY, 0xFF155518, false, pingLine);
                    currentY += textStep;
                }

                if (TwoStates_IsActive()) {
                    int me = TwoStates_LocalState();
                    int other = (me == 1) ? 2 : 1;

                    wchar_t myLine[96] = {};
                    _snwprintf_s(myLine, _TRUNCATE,
                                 TrW(L"Своя: %.0f РУБ | %.0f $", L"Own: %.0f RUB | %.0f $"),
                                 TwoStates_LocalRubles(), TwoStates_LocalDollars());
                    DrawSidebarText(leftX, currentY, 0xFF155518, false, myLine);
                    currentY += textStep;

                    wchar_t partLine[96] = {};
                    _snwprintf_s(partLine, _TRUNCATE,
                                 TrW(L"Партнёр: %.0f РУБ | %.0f $", L"Partner: %.0f RUB | %.0f $"),
                                 TwoStates_GetRubles(other), TwoStates_GetDollars(other));
                    DrawSidebarText(leftX, currentY, 0xFF333333, false, partLine);
                    currentY += textStep;

                    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                    if (TwoStates_GetCursorWorldPos(&cx, &cy, &cz)) {
                        int zone = TwoStates_GetZoneAt(cx, cz);
                        const wchar_t* zText = (zone == 0)
                            ? TrW(L"Курсор: Погранзона", L"Cursor: Border Zone")
                            : (zone == me ? TrW(L"Курсор: Своя земля", L"Cursor: Own Land")
                                          : TrW(L"Курсор: Чужая земля", L"Cursor: Partner's Land"));
                        unsigned int zCol = (zone == 0) ? 0xFF995500 : (zone == me ? 0xFF155518 : 0xFF881818);
                        DrawSidebarText(leftX, currentY, zCol, false, zText);
                        currentY += textStep;
                    }
                }

                currentY += guiScale * 8.0f;

                if (st == SESSION_HOST_LISTENING && SteamP2P_IsAvailable()) {
                    if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF183D5D,
                                          TrW(L"Скопировать код Steam", L"Copy Steam Code"),
                                          "editor/mp_icon_steam.png")) {
                        CopyMySteamCodeToClipboard();
                    }
                    currentY += btnStep;
                }

                if (st == SESSION_HOST_LISTENING || st == SESSION_HOST_IN_GAME) {
                    if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF155518,
                                          TrW(L"Сохранить кооп-игру", L"Save Co-op Game"),
                                          "editor/settings_save.png")) {
                        TwoStates_TriggerQuickCoopSave();
                    }
                    currentY += btnStep;
                }

                if (st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME) {
                    if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                          TrW(L"Камера к партнёру [F6]", L"Camera to Partner [F6]"),
                                          "editor/mp_icon_camera.png")) {
                        TwoStates_JumpCameraToPartner();
                    }
                    currentY += btnStep;

                    if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                          TrW(L"Сигнал на карте [Alt+ЛКМ]", L"Ping Map [Alt+LMB]"),
                                          "editor/mp_icon_ping.png")) {
                        TwoStates_PingAtCursor();
                    }
                    currentY += btnStep;

                    if (TwoStates_IsActive()) {
                        if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF155518,
                                              TrW(L"Перевести 50 000 РУБ", L"Send 50,000 RUB"),
                                              "editor/mp_icon_ruble.png")) {
                            TwoStates_RequestTransfer(50000.0, 0.0);
                        }
                        currentY += btnStep;

                        if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF155518,
                                              TrW(L"Перевести 50 000 $", L"Send 50,000 USD"),
                                              "editor/mp_icon_dollar.png")) {
                            TwoStates_RequestTransfer(0.0, 50000.0);
                        }
                        currentY += btnStep;
                    }

                    if (st == SESSION_HOST_IN_GAME) {
                        if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                              TrW(L"Синхронизировать мир", L"Re-sync World"),
                                              "editor/mp_icon_sync.png")) {
                            SaveSync_RequestResync();
                        }
                        currentY += btnStep;
                    }
                }

                if (TwoStates_IsActive() || TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
                    bool show3D = TwoStates_GetShow3DBorder();
                    if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                          show3D ? TrW(L"3D-граница: ВКЛ [F7]", L"3D Border: ON [F7]")
                                                 : TrW(L"3D-граница: ВЫКЛ [F7]", L"3D Border: OFF [F7]"),
                                          "editor/mp_icon_border.png")) {
                        TwoStates_SetShow3DBorder(!show3D);
                        TwoStates_SaveSettings();
                    }
                    currentY += btnStep;
                }

                if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                      Lang_Get() == MP_LANG_EN ? L"Language: English" : L"Язык: Русский",
                                      "editor/mp_icon_lang.png")) {
                    Lang_Toggle();
                    TwoStates_SaveSettings();
                }
                currentY += btnStep;

                if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF881818,
                                      st == SESSION_HOST_LISTENING
                                          ? TrW(L"Остановить сервер", L"Stop Server")
                                          : TrW(L"Отключиться от коопа", L"Disconnect"),
                                      "editor/mp_icon_stop.png")) {
                    SaveSync_RequestDisconnect();
                }
                currentY += btnStep;

                if (DrawSidebarButton(menuObj, leftX, currentY, 0xFF282420,
                                      TrW(L"<< Назад", L"<< Back"),
                                      "editor/mp_icon_back.png")) {
                    g_inGameCoopSubTab = false;
                }
                currentY += btnStep;
            }

            *(float*)(menuObj + SYSTAB_PANEL_H_OFF) = currentY + guiScale * 736.0f;
            return;
        }

        // Root 'system' (Hammer & Sickle) tab: let the game draw all standard icons/elements,
        // then append our native "КООПЕРАТИВ" / "CO-OP" button cleanly below "Достижения доступны".
        bool hadNativeClickBefore = (*(BYTE*)(g_exeBase + INPUT_LCLICK_RVA) != 0);
        g_origSystemTabBuild(menuObj);
        if (hadNativeClickBefore && *(BYTE*)(g_exeBase + INPUT_LCLICK_RVA) == 0) {
            g_sidebarLClickAvailable = false;
        }

        if (!*(void**)(menuObj + SYSTAB_TAB_PTR_OFF) || *(int*)(menuObj + SYSTAB_SUBTAB_IDX_OFF) != -1) {
            g_sidebarLClickAvailable = false;
            return;
        }

        float guiScale = *(float*)(g_exeBase + GUI_SCALE_RVA);
        float leftX = guiScale * 102.0f;
        float currentY = (*(float*)(menuObj + SYSTAB_PANEL_H_OFF) - guiScale * 730.0f) + guiScale * 30.0f;

        SessionState st = SaveSync_GetState();
        bool inActiveGame = (st == SESSION_HOST_LISTENING || st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME);
        unsigned int btnCol = (inActiveGame || TwoStates_IsActive()) ? 0xFF155518 : 0xFF881818;
        const wchar_t* btnText = (inActiveGame || TwoStates_IsActive())
            ? TrW(L"КООПЕРАТИВ [В СЕТИ]", L"CO-OP [ONLINE]")
            : TrW(L"КООПЕРАТИВ", L"CO-OP MENU");

        if (DrawSidebarButton(menuObj, leftX, currentY, btnCol, btnText, "editor/mp_icon_coop.png")) {
            g_inGameCoopSubTab = true;
            DiagLog_Printf("UI", "In-game Left Sidebar ('system' tab): opened native Co-op sub-tab");
        }
        currentY += guiScale * 38.0f;
        *(float*)(menuObj + SYSTAB_PANEL_H_OFF) = currentY + guiScale * 736.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_origSystemTabBuild) g_origSystemTabBuild(menuObj);
    }
}

static void __fastcall Hook_SystemTabBuild(BYTE* menuObj)
{
    DiagLog_BeginSafeSeh();
    Hook_SystemTabBuildInner(menuObj);
    DiagLog_EndSafeSeh();
}

bool NativeMenu_Install(void)
{
    if (!g_exeBase) return false;
    g_origMainMenuBuild = (MainMenuBuildFn)(g_exeBase + MENU_BUILD_FN_RVA);
    g_origSystemTabBuild = (SystemTabBuildFn)(g_exeBase + SYSTAB_BUILD_FN_RVA);

    BYTE* at = g_exeBase + MENU_CALL_SITE_RVA;
    int rel = 0;
    memcpy(&rel, at + 1, 4);
    if (at[0] != 0xE8 || (MENU_CALL_SITE_RVA + 5 + rel) != MENU_BUILD_FN_RVA) {
        Logf("republicmp  NATIVE_MENU: call site at 0x%X did not match 0x%X (got op=%02X target=0x%X)",
             MENU_CALL_SITE_RVA, MENU_BUILD_FN_RVA, at[0], MENU_CALL_SITE_RVA + 5 + rel);
        return false;
    }

    g_menuRelay = AllocNear(g_exeBase + MENU_CALL_SITE_RVA, 64);
    if (!g_menuRelay) {
        Logf("republicmp  NATIVE_MENU: AllocNear failed");
        return false;
    }

    DWORD oldProt = 0;
    VirtualProtect(g_menuRelay, 32, PAGE_EXECUTE_READWRITE, &oldProt);
    g_menuRelay[0] = 0xFF;
    g_menuRelay[1] = 0x25;
    g_menuRelay[2] = g_menuRelay[3] = g_menuRelay[4] = g_menuRelay[5] = 0;
    void* dst = (void*)&Hook_MainMenuBuild;
    memcpy(g_menuRelay + 6, &dst, 8);

    BYTE* sysRelay = g_menuRelay + 16;
    sysRelay[0] = 0xFF;
    sysRelay[1] = 0x25;
    sysRelay[2] = sysRelay[3] = sysRelay[4] = sysRelay[5] = 0;
    void* sysDst = (void*)&Hook_SystemTabBuild;
    memcpy(sysRelay + 6, &sysDst, 8);

    VirtualProtect(g_menuRelay, 32, PAGE_EXECUTE_READ, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), g_menuRelay, 32);

    long long toRelay = (long long)g_menuRelay - (long long)(at + 5);
    if (toRelay < INT_MIN || toRelay > INT_MAX) {
        Logf("republicmp  NATIVE_MENU: relay out of rel32 range");
        return false;
    }

    BYTE patch[5] = { 0xE8 };
    int rel32 = (int)toRelay;
    memcpy(patch + 1, &rel32, 4);
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(at, patch, 5);
    VirtualProtect(at, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), at, 5);

    Logf("republicmp  NATIVE_MENU: installed native Main Menu hook at RVA 0x%X -> 0x%X",
         MENU_CALL_SITE_RVA, MENU_BUILD_FN_RVA);

    // Install In-Game Left Sidebar 'system' tab hook (RVA 0x31EAF8 -> 0x337450)
    BYTE* sysAt = g_exeBase + SYSTAB_CALL_SITE_RVA;
    int sysRel = 0;
    memcpy(&sysRel, sysAt + 1, 4);
    if (sysAt[0] == 0xE8 && (SYSTAB_CALL_SITE_RVA + 5 + sysRel) == SYSTAB_BUILD_FN_RVA) {
        long long toSysRelay = (long long)sysRelay - (long long)(sysAt + 5);
        if (toSysRelay >= INT_MIN && toSysRelay <= INT_MAX) {
            BYTE sysPatch[5] = { 0xE8 };
            int sysRel32 = (int)toSysRelay;
            memcpy(sysPatch + 1, &sysRel32, 4);
            VirtualProtect(sysAt, 5, PAGE_EXECUTE_READWRITE, &oldProt);
            memcpy(sysAt, sysPatch, 5);
            VirtualProtect(sysAt, 5, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), sysAt, 5);
            Logf("republicmp  NATIVE_MENU: installed in-game Left Sidebar ('system' tab) hook at RVA 0x%X -> 0x%X",
                 SYSTAB_CALL_SITE_RVA, SYSTAB_BUILD_FN_RVA);
        }
    } else {
        Logf("republicmp  NATIVE_MENU: system tab call site at 0x%X did not match 0x%X",
             SYSTAB_CALL_SITE_RVA, SYSTAB_BUILD_FN_RVA);
    }

    return true;
}

void NativeMenu_DrawOverlay(void)
{
    g_overlayHovered = false;
    bool worldLoaded = SaveSync_IsWorldLoaded();
    int screenId = NativeMenu_GetActiveScreenId();

    // Reset in-game Co-op sub-tab state if the left sidebar is closed
    if (g_exeBase && g_inGameCoopSubTab) {
        __try {
            if (!*(void**)(g_exeBase + MENU_OBJ_RVA + SYSTAB_TAB_PTR_OFF)) {
                g_inGameCoopSubTab = false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // 1. Detect transition from Main Menu / Load Screen -> World Loaded!
    if (worldLoaded != g_wasWorldLoaded) {
        ResetSidebarIcons();
    }
    if (worldLoaded && !g_wasWorldLoaded) {
        g_delegatedToLaunchScreen = false;
        if (g_hostOnNextLoad) {
            g_hostOnNextLoad = false;
            g_nativePage = NATIVE_PAGE_ROOT;
            if (SaveSync_GetState() == SESSION_IDLE) {
                DiagLog_Printf("SESSION", "World finished loading with HostOnNextLoad=1 -> auto-starting Co-op Host server");
                SaveSync_StartHost();
            }
        }
    }
    g_wasWorldLoaded = worldLoaded;

    // 2. If user backed out of Load Game / Custom Game / Campaign back to Root Main Menu (screenId == 1),
    //    reset g_hostOnNextLoad if load isn't triggered.
    if (!worldLoaded && screenId == 1 && g_lastScreenId != 1) {
        BYTE loadTrig = 0;
        __try { loadTrig = *(volatile BYTE*)(g_exeBase + LOAD_TRIGGER_DAT_RVA); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (loadTrig == 0 && !g_delegatedToLaunchScreen) {
            g_hostOnNextLoad = false;
            TwoStates_SetImportingSpSave(false);
            TwoStates_MountSaveBank(false);
        }
    }
    g_lastScreenId = screenId;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // 2b. Single-PC Dual-Window Testbed automation (--mp-autohost / --mp-autojoin or env vars):
    static bool s_autoFlagsChecked = false;
    static bool s_autoHostPending = false;
    static bool s_autoUnpausePending = false;
    static int  s_autoInGameFrames = 0;
    static bool s_autoJoinPending = false;
    static int  s_menuReadyFrames = 0;
    if (!s_autoFlagsChecked) {
        s_autoFlagsChecked = true;
        char envBuf[32] = {};
        const char* cmdLine = GetCommandLineA();
        if ((GetEnvironmentVariableA("REPUBLICMP_AUTO_HOST", envBuf, sizeof(envBuf)) > 0 && envBuf[0] == '1') ||
            (cmdLine && strstr(cmdLine, "--mp-autohost") != nullptr)) {
            s_autoHostPending = true;
            s_autoUnpausePending = true;
            DiagLog_Printf("UI", "Dual-Window Testbed: REPUBLICMP_AUTO_HOST=1 active (will auto-load & host from Main Menu)");
        }
        if ((GetEnvironmentVariableA("REPUBLICMP_AUTO_JOIN", envBuf, sizeof(envBuf)) > 0 && envBuf[0] != '0') ||
            (cmdLine && strstr(cmdLine, "--mp-autojoin") != nullptr)) {
            s_autoJoinPending = true;
            DiagLog_Printf("UI", "Dual-Window Testbed: REPUBLICMP_AUTO_JOIN=1 active (will auto-join 127.0.0.1 when local Host opens)");
        }
    }

    if (s_autoUnpausePending && worldLoaded && SaveSync_GetState() == SESSION_HOST_IN_GAME) {
        if (++s_autoInGameFrames >= 30) {
            s_autoUnpausePending = false;
            bool paused = false;
            int speed = 1;
            Determinism_SessionSpeed(&paused, &speed);
            if (paused) {
                DiagLog_Printf("UI", "Dual-Window Testbed: both instances in game -> auto-resuming Lockstep simulation at Speed 1");
                Determinism_RequestPauseToggle();
            }
        }
    }

    if (!worldLoaded && screenId == 1 && SaveSync_GetState() == SESSION_IDLE) {
        ++s_menuReadyFrames;
        if (s_autoHostPending && s_menuReadyFrames >= 45) {
            s_autoHostPending = false;
            TwoStates_MountSaveBank(true);
            TwoStates_SetImportingSpSave(false);
            char autoSaveName[128] = {};
            if (TwoStates_GetLatestCoopSaveName(autoSaveName, sizeof(autoSaveName)) && autoSaveName[0]) {
                g_hostOnNextLoad = true;
                g_delegatedToLaunchScreen = false;
                g_nativePage = NATIVE_PAGE_ROOT;
                DiagLog_Printf("UI", "Dual-Window Testbed: auto-starting Host with latest Co-op save [%s]", autoSaveName);
                TriggerLoadSave(autoSaveName);
            } else {
                DiagLog_Printf("UI", "Dual-Window Testbed: no Co-op save found for auto-host; opening Host menu");
                g_nativePage = NATIVE_PAGE_HOST;
            }
        } else if (s_autoJoinPending && s_menuReadyFrames >= 45) {
            if (SaveSync_IsLocalPcHostWaiting()) {
                s_autoJoinPending = false;
                strncpy_s(g_joinAddress, "127.0.0.1", _TRUNCATE);
                DiagLog_Printf("UI", "Dual-Window Testbed: detected open Local PC Host -> auto-connecting to 127.0.0.1");
                SaveSync_StartClient("127.0.0.1");
            } else {
                ImGui::SetNextWindowPos(ImVec2(displaySize.x - 16.0f, 14.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.14f, 0.10f, 0.07f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.75f, 0.25f, 0.95f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
                ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
                if (ImGui::Begin("##RepublicMP_AutoJoinWait", nullptr, wflags)) {
                    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "%s",
                                       Tr("Тестовый стенд: ожидание запуска окна Хоста (127.0.0.1)...",
                                          "Testbed: waiting for Host window to open server (127.0.0.1)..."));
                    ImGui::SameLine();
                    if (ImGui::SmallButton(Tr("Отмена", "Cancel"))) {
                        s_autoJoinPending = false;
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
            }
        }
    }

    // 3. On Load Game (2), Campaign (0x62), Custom Game Map (0x63), or Custom Game Settings (3):
    //    Place a neat badge in the TOP-RIGHT corner so it never overlaps the game's centered window title!
    bool onLaunchScreen = !worldLoaded && (screenId == 2 || screenId == 0x62 || screenId == 0x63 || screenId == 3);
    if (onLaunchScreen) {
        ImGui::SetNextWindowPos(ImVec2(displaySize.x - 16.0f, 14.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.11f, 0.08f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, g_hostOnNextLoad ? ImVec4(0.95f, 0.75f, 0.25f, 0.95f)
                                                                : ImVec4(0.55f, 0.42f, 0.28f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Button, g_hostOnNextLoad ? ImVec4(0.20f, 0.52f, 0.22f, 0.95f)
                                                                : ImVec4(0.58f, 0.18f, 0.14f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_hostOnNextLoad ? ImVec4(0.26f, 0.66f, 0.28f, 1.0f)
                                                                       : ImVec4(0.76f, 0.24f, 0.18f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##RepublicMP_LaunchScreenBar", nullptr, flags)) {
            if (g_hostOnNextLoad) {
                if (screenId == 2 && TwoStates_IsCoopBankMounted()) {
                    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.45f, 1.0f), "%s",
                                       Tr("КООП-СОХРАНЕНИЯ (режим и казна из сейва)",
                                          "CO-OP SAVES (mode & budget restored from save)"));
                } else {
                    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.45f, 1.0f), "%s",
                                       (screenId == 2 && TwoStates_IsImportingSpSave())
                                           ? Tr("Импорт одиночного сейва в КООП:", "Import SP Save to CO-OP:")
                                           : Tr("Запуск: КООП-СЕРВЕР", "Launch: CO-OP SERVER"));
                    ImGui::SameLine();
                    int mode = TwoStates_GetMode();
                    if (ImGui::SmallButton(mode == COOP_MODE_TWO_STATES
                                           ? Tr("2 государства", "Two States")
                                           : Tr("1 государство", "One State"))) {
                        TwoStates_SetMode(mode == COOP_MODE_TWO_STATES ? COOP_MODE_ONE_STATE : COOP_MODE_TWO_STATES);
                        TwoStates_SaveSettings();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(Tr("Выкл кооп", "Singleplayer"))) {
                        g_hostOnNextLoad = false;
                        TwoStates_SetImportingSpSave(false);
                    }
                }
            } else {
                ImGui::TextColored(ImVec4(0.85f, 0.80f, 0.70f, 1.0f), "%s",
                                   (screenId == 2)
                                       ? Tr("Одиночные сохранения", "Singleplayer Saves")
                                       : Tr("Запуск: Одиночная игра", "Launch: Singleplayer"));
                ImGui::SameLine();
                if (ImGui::SmallButton(screenId == 2
                                       ? Tr("Импорт в Кооп-сервер", "Import to Co-op Server")
                                       : Tr("Включить Кооп-сервер", "Enable Co-op Server"))) {
                    g_hostOnNextLoad = true;
                    if (screenId == 2 && !TwoStates_IsCoopBankMounted()) {
                        TwoStates_SetImportingSpSave(true);
                    }
                }
            }

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                g_overlayHovered = true;
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
    }

    // 4. Clean Soviet-styled manual IP / SteamID input dialog
    if (g_showManualJoinModal) {
        ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(460.0f, 195.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.16f, 0.11f, 0.08f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.90f, 0.70f, 0.25f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.45f, 0.14f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.60f, 0.18f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.18f, 0.14f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.25f, 0.18f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

        ImGuiWindowFlags mflags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin(Tr("Подключение к серверу — Адрес###ManualJoinModal",
                            "Join Server — Address###ManualJoinModal"), &g_showManualJoinModal, mflags)) {
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.60f, 1.0f), "%s",
                               Tr("IP-адрес хоста (например 127.0.0.1) или steam:<SteamID>:",
                                  "Host IP address (e.g. 127.0.0.1) or steam:<SteamID>:"));
            ImGui::SetNextItemWidth(320.0f);
            bool enterPressed = ImGui::InputText("##manual_join_ip", g_joinAddress, sizeof(g_joinAddress),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button(Tr("Вставить", "Paste"))) {
                PasteAddressFromClipboard();
            }

            if (SteamP2P_IsAvailable()) {
                ImGui::Spacing();
                if (ImGui::SmallButton(Tr("Скопировать мой код Steam для друга", "Copy My Steam Code for Friend"))) {
                    CopyMySteamCodeToClipboard();
                }
            }

            ImGui::Spacing();
            if (ImGui::Button(Tr("Подключиться", "Connect"), ImVec2(200.0f, 34.0f)) || enterPressed) {
                g_showManualJoinModal = false;
                SaveSync_StartClient(g_joinAddress);
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("Отмена", "Cancel"), ImVec2(120.0f, 34.0f))) {
                g_showManualJoinModal = false;
            }
            g_overlayHovered = true;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(6);
    }
}
