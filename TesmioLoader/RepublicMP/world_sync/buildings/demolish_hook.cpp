// SPDX-License-Identifier: GPL-3.0-only
// See demolish_hook.h.
#include "demolish_hook.h"
#include "../local_builds.h"          // LocalBuilds_InBuildingPlacement
#include "../terrain/terrain_hook.h"          // g_terrainApplyingRemote
#include "build_sync.h"     // BuildSync_SendDemolish, BuildSync_IsApplyingRemote
#include "../networks/road_sync.h"      // RoadSync_IsApplyingSegment
#include "../../lockstep/commands.h"
#include "../../determinism/determinism.h"
#include "../../session/two_states.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#define WORLD_THIS_RVA 0x9D4F10
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18 // = thisPtr+0x11b08, the list terrain_demolish hovers over
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define OBJ_POS_OFFSET 0x350
#define OBJ_TYPE_OFFSET 0x318          // building type record
#define TYPE_CATEGORY_OFFSET 0x360     // int category in the type record
#define OBJ_DEMOLISH_FLAG_OFFSET 0xeb1 // zeroed by both player paths before the call

// mov rax,rsp / mov [rax+20],r9b / mov [rax+18],r8b / mov [rax+10],rdx -
// 15 bytes, none RIP-relative (SOVIET64.exe, 2026-09-07).
static const unsigned char kDemolishExpect[15] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44,
    0x88, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10
};

typedef void (*DemolishFn)(__int64 thisPtr, __int64 building,
                           unsigned long long p3, unsigned long long p4,
                           unsigned long long p5, unsigned long long p6,
                           unsigned long long p7);
static DemolishFn g_origDemolish = nullptr;

static volatile long g_depth = 0;

void DemolishHook_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool DemolishHook_InDemolish()
{
    return g_depth > 0;
}

// Captured in the hook, sent from DemolishHook_Tick (game thread, after the
// frame's terrain batch is closed, so the fence stamp covers it).
struct DemolishCapture {
    float pos[3];
    int category;
    unsigned long long args[5];
    unsigned long long callerRva;
};
#define MAX_DEMOLISH_CAPTURES 32
static DemolishCapture g_captures[MAX_DEMOLISH_CAPTURES];
static int g_captureCount = 0;

static bool IsInBuildingsVector(__int64 building)
{
    __int64* it = *(__int64**)(g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
    __int64* end = *(__int64**)(g_exeBase + VEC_BUILDINGS_END_RVA);
    for (; it && it < end; ++it)
        if (*it == building) return true;
    return false;
}

static int BuildingCategory(__int64 building)
{
    __int64 type = *(__int64*)(building + OBJ_TYPE_OFFSET);
    return type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
}

static void Capture(__int64 thisPtr, __int64 building, const unsigned long long args[5], void* ret)
{
    unsigned long long callerRva = (unsigned long long)((BYTE*)ret - g_exeBase);
    __try {
        if (thisPtr != (__int64)(g_exeBase + WORLD_THIS_RVA)) {
            Logf("republicmp  DEMOLISH: call from 0x%llX with a foreign thisPtr - not sent", callerRva);
            return;
        }
        if (!building || !IsInBuildingsVector(building)) {
            Logf("republicmp  DEMOLISH: object %p (caller 0x%llX) is not in the buildings vector - not sent",
                 (void*)building, callerRva);
            return;
        }
        if (g_captureCount == MAX_DEMOLISH_CAPTURES) {
            Logf("republicmp  DEMOLISH: capture buffer full - demolition at caller 0x%llX not sent", callerRva);
            return;
        }
        DemolishCapture& c = g_captures[g_captureCount++];
        const float* p = (const float*)(building + OBJ_POS_OFFSET);
        c.pos[0] = p[0]; c.pos[1] = p[1]; c.pos[2] = p[2];
        c.category = BuildingCategory(building);
        for (int k = 0; k < 5; k++) c.args[k] = args[k];
        c.callerRva = callerRva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DEMOLISH: <exception capturing demolition at caller 0x%llX>", callerRva);
    }
}

// Lockstep (lockstep/commands): a local demolition becomes a command and is
// not made here; every PC demolishes the building at the next tick boundary
// (BuildSync_ApplyDemolishCommand finds it by position and category, as the
// replay does). True when it was taken.
static bool DemolishAsCommand(__int64 thisPtr, __int64 building, const unsigned long long args[5])
{
    BuildSyncMsg msg = {};
    int bldId = 0;
    __try {
        if (thisPtr != (__int64)(g_exeBase + WORLD_THIS_RVA) || !building || !IsInBuildingsVector(building))
            return false;
        bldId = *(int*)building;
        const float* p = (const float*)(building + OBJ_POS_OFFSET);
        msg.kind = BUILD_SYNC_KIND_DEMOLISH;
        msg.x = p[0];
        msg.y = p[1];
        msg.z = p[2];
        msg.demolishCategory = BuildingCategory(building);
        for (int k = 0; k < 5; k++) msg.demolishArgs[k] = args[k];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!TwoStates_CheckBuildingControlAllowed(bldId, msg.x, msg.z, "снос здания"))
        return true; // swallow demolition of another state's building
    Commands_Submit(LS_CMD_DEMOLISH, &msg, sizeof(msg));
    return true;
}

static void HookedDemolish(__int64 thisPtr, __int64 building,
                           unsigned long long p3, unsigned long long p4,
                           unsigned long long p5, unsigned long long p6,
                           unsigned long long p7)
{
    void* ret = _ReturnAddress();
    long depth = InterlockedIncrement(&g_depth);

    // Only outermost calls by local play; everything else is either a
    // side effect of one, or remote work being replayed.
    if (depth == 1 && !LocalBuilds_InBuildingPlacement() && !BuildSync_IsApplyingRemote() &&
        !RoadSync_IsApplyingSegment() && g_terrainApplyingRemote == 0) {
        unsigned long long args[5] = { p3, p4, p5, p6, p7 };
        if (Commands_TakeLocal() && DemolishAsCommand(thisPtr, building, args)) {
            InterlockedDecrement(&g_depth);
            return;
        }
        // The replay channels are not used with the determinism layer on.
        if (!Determinism_IsActive()) Capture(thisPtr, building, args, ret);
    }

    g_origDemolish(thisPtr, building, p3, p4, p5, p6, p7);
    InterlockedDecrement(&g_depth);
}

bool InstallDemolishHook()
{
    return InstallInlineHook(g_exeBase + HOOK_DEMOLISH_RVA, (void*)HookedDemolish,
                             (void**)&g_origDemolish, kDemolishExpect, sizeof(kDemolishExpect),
                             "republicmp demolish");
}

void DemolishHook_Tick()
{
    for (int k = 0; k < g_captureCount; k++) {
        const DemolishCapture& c = g_captures[k];
        Logf("republicmp  DEMOLISH: local demolition at (%.2f,%.2f,%.2f) category=%d caller=0x%llX "
             "args=[%llx,%llx,%llx,%llx,%llx] - sending",
             c.pos[0], c.pos[1], c.pos[2], c.category, c.callerRva,
             c.args[0] & 0xff, c.args[1] & 0xff, c.args[2] & 0xff, c.args[3] & 0xff, c.args[4] & 0xff);
        BuildSync_SendDemolish(c.pos, c.category, c.args);
    }
    g_captureCount = 0;
}

bool DemolishHook_CallOriginal(__int64 thisPtr, __int64 building, const unsigned long long args[5])
{
    DemolishFn fn = g_origDemolish ? g_origDemolish : (DemolishFn)(g_exeBase + HOOK_DEMOLISH_RVA);
    __try {
        *(unsigned char*)(building + OBJ_DEMOLISH_FLAG_OFFSET) = 0;
        InterlockedIncrement(&g_depth);
        __try {
            fn(thisPtr, building, args[0], args[1], args[2], args[3], args[4]);
        } __finally {
            InterlockedDecrement(&g_depth);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DEMOLISH: <exception replaying demolition>");
        return false;
    }
}
