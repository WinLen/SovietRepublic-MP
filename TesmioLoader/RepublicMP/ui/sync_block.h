// SPDX-License-Identifier: GPL-3.0-only
// Full-screen block shown while a player joins (host: from the moment a
// player connects until that player's world is loaded; client: from Connect
// until loaded). It greys the screen and shows which step the join is in.
#pragma once

// Raises/lowers the block. reason (if given) becomes the stage text. Any thread.
void SyncBlock_SetActive(bool active, const char* reason);

// Current join step, shown on the block. percent < 0 hides the progress bar.
// Any thread.
void SyncBlock_SetStep(int step, int totalSteps, const char* text, int percent = -1);

// Updates only the progress bar of the current step. Any thread.
void SyncBlock_SetPercent(int percent);

bool SyncBlock_IsActive();

// Called every frame from ImGuiHook_NewFrameAndRender; no-op when inactive.
void SyncBlock_Draw();
