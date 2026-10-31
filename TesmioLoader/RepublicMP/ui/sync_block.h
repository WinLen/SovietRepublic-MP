// SPDX-License-Identifier: GPL-3.0-only
// In-game Soviet Co-op Waiting Lobby & Save-Sync overlay.
// Shown while the Host is waiting for the second player (initial start or
// reconnect after partner disconnect) and while save transfer / world load
// synchronization is in progress.
#pragma once

// Raises/lowers the active sync progress state. reason (if given) becomes the stage text. Any thread.
void SyncBlock_SetActive(bool active, const char* reason);

// Current join step, shown on the lobby card. percent < 0 shows an indeterminate bar. Any thread.
void SyncBlock_SetStep(int step, int totalSteps, const char* text, int percent = -1);

// Updates only the progress bar of the current step. Any thread.
void SyncBlock_SetPercent(int percent);
void SyncBlock_SetPercentEx(int percent, int queuedBytes, int totalBytes, float speedMbSec);
void SyncBlock_GetTransferStats(int* outPercent, int* outQueuedBytes, int* outTotalBytes, float* outSpeedMbSec);

bool SyncBlock_IsActive();

// Map Inspection Mode ([Tab] while Host is waiting in SESSION_HOST_LISTENING):
// collapses the center modal into a top banner so the Host can pan/rotate/zoom
// the camera while simulation remains paused and building/terrain/demolition stay locked.
bool SyncBlock_IsMapInspectMode();
void SyncBlock_SetMapInspectMode(bool inspect);

// True when full world mouse/keyboard camera & click input should be blocked
// (true during active sync or when the full Waiting Lobby window is open;
// false when the Waiting Lobby is collapsed into [Tab] Map Inspection Mode).
bool SyncBlock_BlocksWorldInput();

// Updates and normalizes Map Inspection state for the given SessionState and sync flag.
// Automatically exits Map Inspection mode (returns false) when a partner begins joining
// or when no longer in SESSION_HOST_LISTENING.
bool SyncBlock_UpdateMapInspectState(int sessionState, bool syncActive);

// Called every frame from ImGuiHook_NewFrameAndRender.
void SyncBlock_Draw();
