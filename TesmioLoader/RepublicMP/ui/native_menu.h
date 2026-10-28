// SPDX-License-Identifier: GPL-3.0-only
// Native Main Menu & Load/New Game Co-op integration (RimWorld Multiplayer style).
//
// Hooks SOVIET64.exe's native Main Menu builder (RVA 0x261897 -> 0x28AF60) to inject
// native buttons directly into the game's 3D/2D Main Menu list, and provides a
// seamless Co-op Server launch toggle on the native Load Game and New Game screens.
#pragma once

#include "tesmio_plugin.h"

void NativeMenu_Bind(const TsmHost* host);
bool NativeMenu_Install(void);

// True if the player selected "Host Server" from the Main Menu or enabled
// "Launch as Co-op Server" on the Load Save / New Game screen.
bool NativeMenu_IsHostOnNextLoad(void);
void NativeMenu_ClearHostOnNextLoad(void);
void NativeMenu_SetHostOnNextLoadForTest(bool host);

// Returns the game's current menu screen ID (1 = Root Main Menu, 2 = Load Game,
// 0x62 = Campaign, 0x63 = Custom Game Map Select, 3 = Custom Game Setup).
#define MENU_OBJ_RVA            0x9D4F10
#define MENU_SCREEN_ID_OFF      0x5044
int NativeMenu_GetActiveScreenId(void);

// True when the Load/New Game Co-op bar or manual IP input modal is hovered/active.
bool NativeMenu_WantsInput(void);

// Per-frame overlay for the Load/New Game Co-op toggle bar, manual IP modal,
// and automatic server start when the map finishes loading.
void NativeMenu_DrawOverlay(void);
