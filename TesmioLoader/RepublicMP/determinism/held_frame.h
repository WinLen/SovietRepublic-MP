// SPDX-License-Identifier: GPL-3.0-only
// Presentation without advancing Early's entity groups (by SoylentGreed).
#pragma once
#include "tesmio_plugin.h"

void HeldFrame_Bind(const TsmHost* host);
bool HeldFrame_Verify();
// Main thread, under the renderer lock. False stops the probe on a guard fault.
bool HeldFrame_Draw(bool entitiesReady);
// Main thread, right after Early: the doors and the citizens this PC sees.
void HeldFrame_AfterEarly();
