// SPDX-License-Identifier: GPL-3.0-only
// "Menus closed" for the length of a remote replay.
//
// world+0xfbc / +0xfbe / +0xfc3 are the game's menu-open flags: +0xfc3 is set
// every frame while the cursor is over a window (the window dispatcher
// FUN_1406e9b80), the other two while a menu is open. The building and
// network tools return at once while any is set - meant for the local
// player's clicks, but it also held every building and road from the other
// player until the receiver moved the cursor off a window (#21). A replay
// now writes them 0 for its call and puts this PC's values back after.
// The local player's drag, the join screen and the terrain fence still hold
// replays as before.
//
// (by TucKnock; the same as SoylentGreed's replays do)
#pragma once

#include "tesmio_plugin.h"

#define REPLAY_UI_WORLD_RVA 0x9D4F10
#define REPLAY_UI_FLAG_COUNT 3

static const unsigned int kReplayUiFlagOffsets[REPLAY_UI_FLAG_COUNT] = { 0xfbc, 0xfbe, 0xfc3 };

class ReplayUiScope {
public:
    ReplayUiScope()
    {
        BYTE* world = g_exeBase + REPLAY_UI_WORLD_RVA;
        for (int i = 0; i < REPLAY_UI_FLAG_COUNT; i++) {
            saved_[i] = world[kReplayUiFlagOffsets[i]];
            world[kReplayUiFlagOffsets[i]] = 0;
        }
    }
    ~ReplayUiScope()
    {
        BYTE* world = g_exeBase + REPLAY_UI_WORLD_RVA;
        for (int i = 0; i < REPLAY_UI_FLAG_COUNT; i++) world[kReplayUiFlagOffsets[i]] = saved_[i];
    }
    // True if a window or menu was in the way (for the log).
    bool WasBlocked() const { return saved_[0] || saved_[1] || saved_[2]; }

private:
    unsigned char saved_[REPLAY_UI_FLAG_COUNT];
    ReplayUiScope(const ReplayUiScope&);
    ReplayUiScope& operator=(const ReplayUiScope&);
};
