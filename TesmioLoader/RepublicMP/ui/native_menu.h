// SPDX-License-Identifier: GPL-3.0-only
// Native Main Menu & Load/New Game Co-op integration (RimWorld Multiplayer style).
//
// Hooks SOVIET64.exe's native Main Menu builder (RVA 0x261897 -> 0x28AF60) to inject
// native buttons directly into the game's 3D/2D Main Menu list, and provides a
// seamless Co-op Server launch toggle on the native Load Game and New Game screens.
// Pure C++/POD header: intentionally avoids <windows.h>, <winsock2.h>, and steam_p2p.h.
#pragma once

#include "tesmio_plugin.h"
#include "../transport/lan_discovery.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

static constexpr int NATIVE_JOIN_MAX_SLOTS = 28;
static constexpr size_t NATIVE_JOIN_MAX_LABEL_CHARS = 21;

struct NativeMenuFriendInfo {
    uint64_t steamId;
    char name[64];
    bool inThisGame;
    bool isHosting;
};

enum JoinMenuActionKind {
    JOIN_ACT_LOCAL_PC_HOST = 0,
    JOIN_ACT_LAN_HOST,
    JOIN_ACT_STEAM_FRIEND,
    JOIN_ACT_INFO_STEAM_OFFLINE,
    JOIN_ACT_INFO_NO_HOSTS,
    JOIN_ACT_CONNECT_ADDR,
    JOIN_ACT_TYPE_MANUAL,
    JOIN_ACT_PASTE_CLIPBOARD,
    JOIN_ACT_REFRESH_HOSTS,
    JOIN_ACT_BACK,
};

struct JoinPlanEntry {
    JoinMenuActionKind kind;
    uint64_t steamId;
    LanHostEntry lanHost;
    char connectTarget[64];
    std::wstring label;
};

void NativeMenu_Bind(const TsmHost* host);
bool NativeMenu_Install(void);

// True if the player selected "Host Server" from the Main Menu or enabled
// "Launch as Co-op Server" on the Load Save / New Game screen.
bool NativeMenu_IsHostOnNextLoad(void);
void NativeMenu_ClearHostOnNextLoad(void);
void NativeMenu_SetHostOnNextLoadForTest(bool host);

// Returns the game's current menu screen ID (1 = Root Main Menu, 2 = Load Game,
// 0x62 = Campaign, 0x63 = Custom Game Map Select, 3 = Custom Game Setup).
#define MENU_OBJ_RVA            0xA1EF20
#define MENU_SCREEN_ID_OFF      0x5074
int NativeMenu_GetActiveScreenId(void);

// True when the Load/New Game Co-op bar or manual IP input modal is hovered/active.
bool NativeMenu_WantsInput(void);

// Per-frame overlay for the Load/New Game Co-op toggle bar, manual IP modal,
// and automatic server start when the map finishes loading.
void NativeMenu_DrawOverlay(void);

// Pure unit-testable Join Menu slot planner and label/address helpers.
std::wstring NativeMenu_Utf8ToWideSafe(const char* utf8);
std::wstring NativeMenu_TruncateLabel(const std::wstring& text,
                                      size_t maxChars = NATIVE_JOIN_MAX_LABEL_CHARS);
std::wstring NativeMenu_FormatLanHostLabel(const LanHostEntry& host);
bool NativeMenu_FormatConnectTarget(const LanHostEntry& host, char* outTarget, size_t outCap);
std::vector<JoinPlanEntry> NativeMenu_PlanJoinEntries(
    bool localPcHostWaiting,
    const std::vector<LanHostEntry>& lanHosts,
    bool steamAvailable,
    const std::vector<NativeMenuFriendInfo>& steamFriends,
    const char* currentJoinAddress,
    bool isEnglish,
    int maxSlots = NATIVE_JOIN_MAX_SLOTS);
bool NativeMenu_ResolveJoinClick(const std::vector<JoinPlanEntry>& renderedSnapshot,
                                 int clickedIdx, JoinPlanEntry* outSelected);
bool NativeMenu_IsLanHostVersionCompatible(const LanHostEntry& host,
                                           const char* localVersion,
                                           uint32_t localDllCrc);
std::vector<LanAdapterInfo> NativeMenu_SelectSidebarExtraAdapters(
    const std::vector<LanAdapterInfo>& displayAdapters, size_t maxExtra = 2);
