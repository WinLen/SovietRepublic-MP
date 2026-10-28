// SPDX-License-Identifier: GPL-3.0-only
// Presentation without advancing Early's entity groups (by SoylentGreed).
#pragma once
#include "tesmio_plugin.h"

void HeldFrame_Bind(const TsmHost* host);
bool HeldFrame_Verify();
void HeldFrame_Reset();
void HeldFrame_OnSimTick(unsigned long long tick);
void HeldFrame_OnVehicleSoftSynced(BYTE* vehicle);
void HeldFrame_AddVehicleSmoothOffset(BYTE* vehicle, float ox, float oy, float oz);
void HeldFrame_InterpolateC3DNode(BYTE* node, const float prevNode[41], const float currNode[41],
                                  float alpha, const float smoothOff[3]);
// Main thread, under the renderer lock. False stops the probe on a guard fault.
bool HeldFrame_Draw(bool entitiesReady);
// Main thread, right after Early: the doors and the citizens this PC sees.
void HeldFrame_AfterEarly();

struct HeldFrameDiagStats {
    unsigned long long heldFramesDrawn;
    unsigned long long guardFallbacks;
    unsigned long long restoreRefusals;
    unsigned trackedVehicles;
    unsigned lastTargetsCount;
    unsigned lastInterpolatedCount;
    float lastAlpha;
    bool inHeldDraw;
};
void HeldFrame_GetDiagStats(HeldFrameDiagStats* out);
