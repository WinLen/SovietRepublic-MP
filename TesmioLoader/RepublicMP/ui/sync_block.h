// SPDX-License-Identifier: GPL-3.0-only
// Full-screen block shown while a player joins (host: from the moment a
// player connects until that player's world is loaded; client: from Connect
// until loaded). It greys the screen and shows which step the join is in -
// "Step 4/7 - Sending the world to the player  62%" - so nobody needs the
// menu to see what is happening. Meanwhile nothing can change the world:
//   - the game is paused (re-asserted every frame in SyncBlock_Draw);
//   - the game's input (C3D_INPUT) answers "nothing pressed"
//     (game/input_block), and window messages are swallowed
//     (game/imgui_hook);
//   - building placement refuses while active.
#pragma once

// Raises/lowers the block. reason (if given) becomes the stage text. Any thread.
void SyncBlock_SetActive(bool active, const char* reason);

// Current join step, shown on the block. percent < 0 hides the progress bar.
// Any thread.
void SyncBlock_SetStep(int step, int totalSteps, const char* text, int percent = -1);

// Updates only the progress bar of the current step. Any thread.
void SyncBlock_SetPercent(int percent);

bool SyncBlock_IsActive();

// Copies the current step for other UI (main menu). Returns false if inactive.
bool SyncBlock_GetStep(int* step, int* totalSteps, char* text, int textSize, int* percent);

// Called every frame from ImGuiHook_NewFrameAndRender, even while the
// overlay is hidden; no-op when inactive.
void SyncBlock_Draw();
