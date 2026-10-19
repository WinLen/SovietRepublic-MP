// SPDX-License-Identifier: GPL-3.0-only
// Diagnostic IAT hooks on C3D_TIMER (Start/Stop/MultiplyFPS): logs which
// timer instance fires and its caller RVA. Pause control itself lives in
// game_speed.h.
#pragma once

#include "tesmio_plugin.h"

void PauseHook_Bind(const TsmHost* host);
bool InstallPauseHooks();
