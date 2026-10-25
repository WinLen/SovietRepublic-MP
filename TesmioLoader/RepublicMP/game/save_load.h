// SPDX-License-Identifier: GPL-3.0-only
// Triggers the game's own save and load actions by writing the same memory
// its menus write; the game's main loop performs the actual I/O on its next
// frame. No engine code is called, so these are safe from any thread.
//
// Load: the Load-menu click handler (FUN_140285fc0) writes the save path and
// a trigger flag; the main loop (FUN_140430fc0) then loads it.
// Save: the per-frame dispatcher (FUN_14030d1a0) runs the master save routine
// (FUN_14042cc70) when a trigger flag inside the world object is set, and
// clears the flag when done.
#pragma once

#include "tesmio_plugin.h"
#include "../core/engine_rva.h"

// Load side (plain globals).
#define RETURN_MENU_DAT_RVA   0x995289  // cleared together with the trigger
#define SAVE_NAME_MAX_LEN     120
#define SAVE_FOLDER_PREFIX    "save\\"

// Save side (fields of the world object at WORLD_THIS_RVA).
#define SAVE_DELAY_COUNTER_OFFSET   0x14D38   // float: frames to wait - forced to 0
#define SAVE_NAME_FIELD_PREFIX      "save/"

void SaveLoad_Bind(const TsmHost* host);

// Requests loading "save\\<saveName>".
void TriggerLoadSave(const char* saveName);

// Requests saving under "save/<saveName>". Always returns true; use
// WaitForSaveToFinish to know when the files are written.
bool TriggerSaveGame(const char* saveName);

// Polls the trigger flag until the game clears it or timeoutMs elapses
// (0 = no timeout). Returns false on timeout or when *cancelFlag becomes true
// (checked every ~100ms).
bool WaitForSaveToFinish(DWORD timeoutMs, volatile bool* cancelFlag = nullptr);
// abortCheck(abortCtx), polled with the cancel flag, ends the wait early too
// (e.g. the join connection died). The load itself is not stopped.
bool WaitForLoadToFinish(DWORD timeoutMs, volatile bool* cancelFlag = nullptr,
                         bool (*abortCheck)(void*) = nullptr, void* abortCtx = nullptr);
