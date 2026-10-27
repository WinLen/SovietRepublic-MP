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
#include "../../session/save_sync.h"
#include "../../ui/native_menu.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#pragma intrinsic(_AddressOfReturnAddress)

#define OBJ_TYPE_OFFSET 0x318          // building type record
#define TYPE_CATEGORY_OFFSET 0x360     // int category in the type record
#define OBJ_DEMOLISH_FLAG_OFFSET 0xeb1 // zeroed by some player paths before the call, 1 in building window

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
    BuildSyncMsg msg;
    unsigned long long callerRva;
};
#define MAX_DEMOLISH_CAPTURES 32
static DemolishCapture g_captures[MAX_DEMOLISH_CAPTURES];
static int g_captureCount = 0;

static bool IsInBuildingsVector(__int64 building)
{
    const unsigned int kBeginRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
    const unsigned int kEndRvas[2]   = { VEC_BUILDINGS_END_RVA,   VEC_CONSTRUCTIONS_END_RVA };
    for (int v = 0; v < 2; ++v) {
        __int64* it = *(__int64**)(g_exeBase + kBeginRvas[v]);
        __int64* end = *(__int64**)(g_exeBase + kEndRvas[v]);
        for (; it && it < end; ++it)
            if (*it == building) return true;
    }
    return false;
}

static int BuildingCategory(__int64 building)
{
    __int64 type = *(__int64*)(building + OBJ_TYPE_OFFSET);
    return type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
}

static void PopulateDemolishMsg(__int64 thisPtr, __int64 building, const unsigned long long args[5],
                                unsigned long long callerRva, BuildSyncMsg& msg)
{
    const float* p = (const float*)(building + OBJ_POS_OFFSET);
    msg.kind = BUILD_SYNC_KIND_DEMOLISH;
    msg.mode = BuildMode_Read();
    msg.x = p[0];
    msg.y = p[1];
    msg.z = p[2];
    msg.demolishCategory = BuildingCategory(building);
    for (int k = 0; k < 5; k++) msg.demolishArgs[k] = args[k];
    msg.demolishEb1 = *(unsigned char*)(building + OBJ_DEMOLISH_FLAG_OFFSET);
    msg.demolishShift = (*(int*)(g_exeBase + 0x9D5A08) != 0) ? 1 : 0;
    msg.demolishCtrl  = (*(int*)(g_exeBase + 0x9D5A0C) != 0) ? 1 : 0;
    msg.fromDialog424 = (callerRva == 0x7FD000) ? 1 : 0;
    msg.autoSearchOffices = *(unsigned char*)(thisPtr + 0xC798);
    msg.assignFromTool = (callerRva == 0x1DAA78 || callerRva == 0x1DAFFE) ? 1 : 0;
    msg.assignPriority = (*(unsigned char*)(g_exeBase + 0xA54BAD) != 0) ? 1 : 0;
    msg.assignIsDemolishOffice = 0;
    msg.sliderValue = 0;
    void* sliderWidget = *(void**)(thisPtr + 0xC790);
    if (sliderWidget && (msg.demolishEb1 != 0 || msg.fromDialog424)) {
        typedef float (*ReadSliderFn)(void*);
        msg.sliderValue = (int)((ReadSliderFn)(g_exeBase + 0x0FF6A0))(sliderWidget);
    }
    msg.assignOffice = kNoRef;
    msg.assignDist = 0.0f;
    if (msg.assignFromTool) {
        U64 office = *(U64*)(thisPtr + 0xD298);
        if (office && Ref_Building(office, msg.assignOffice)) {
            msg.assignIsDemolishOffice = (BuildingCategory((__int64)office) == 104) ? 1 : 0;
        }
        if (callerRva == 0x1DAFFE)
            msg.assignDist = *(float*)(thisPtr + 0xF5C0);
    }
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
        memset(&c, 0, sizeof(c));
        PopulateDemolishMsg(thisPtr, building, args, callerRva, c.msg);
        c.callerRva = callerRva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DEMOLISH: <exception capturing demolition at caller 0x%llX>", callerRva);
    }
}

// Lockstep (lockstep/commands): a local demolition becomes a command and is
// not made here; every PC demolishes the building at the next tick boundary
// (BuildSync_ApplyDemolishCommand finds it by position and category, as the
// replay does). True when it was taken.
static bool DemolishAsCommand(__int64 thisPtr, __int64 building, const unsigned long long args[5],
                              unsigned long long callerRva)
{
    BuildSyncMsg msg = {};
    int bldId = 0;
    __try {
        if (thisPtr != (__int64)(g_exeBase + WORLD_THIS_RVA) || !building || !IsInBuildingsVector(building))
            return false;
        bldId = *(int*)building;
        PopulateDemolishMsg(thisPtr, building, args, callerRva, msg);
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
    void** retSlot = (void**)_AddressOfReturnAddress();
    unsigned long long callerRva = (unsigned long long)((BYTE*)ret - g_exeBase);
    long depth = InterlockedIncrement(&g_depth);

    // Only outermost calls by local play; everything else is either a
    // side effect of one, or remote work being replayed.
    if (depth == 1 && !LocalBuilds_InBuildingPlacement() && !BuildSync_IsApplyingRemote() &&
        !RoadSync_IsApplyingSegment() && g_terrainApplyingRemote == 0) {
        if (SaveSync_IsWaitingLobbyActive() || NativeMenu_IsHostOnNextLoad()) {
            if (callerRva == 0x7FD000) {
                *retSlot = g_exeBase + 0x7FD04F;
            }
            SaveSync_NotifyWaitingLobbyActionBlocked();
            InterlockedDecrement(&g_depth);
            return;
        }
        unsigned long long args[5] = { p3, p4, p5, p6, p7 };
        if (Commands_TakeLocal() && DemolishAsCommand(thisPtr, building, args, callerRva)) {
            if (callerRva == 0x7FD000) {
                // Skip post-call 0x7FD000..0x7FD04E on the UI pass so it does not write
                // +0x879/+0x630 before 0x168730 runs at the Lockstep boundary.
                *retSlot = g_exeBase + 0x7FD04F;
            }
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
             "args=[%llx,%llx,%llx,%llx,%llx] eb1=%u shift=%u ctrl=%u - sending",
             c.msg.x, c.msg.y, c.msg.z, c.msg.demolishCategory, c.callerRva,
             c.msg.demolishArgs[0] & 0xff, c.msg.demolishArgs[1] & 0xff, c.msg.demolishArgs[2] & 0xff,
             c.msg.demolishArgs[3] & 0xff, c.msg.demolishArgs[4] & 0xff,
             c.msg.demolishEb1, c.msg.demolishShift, c.msg.demolishCtrl);
        BuildSync_SendDemolish(c.msg);
    }
    g_captureCount = 0;
}

bool DemolishHook_CallOriginal(__int64 thisPtr, __int64 building, const BuildSyncMsg& msg)
{
    DemolishFn fn = g_origDemolish ? g_origDemolish : (DemolishFn)(g_exeBase + HOOK_DEMOLISH_RVA);
    __try {
        BuildMode savedMode = BuildMode_Read();
        int* shiftFlag = (int*)(g_exeBase + 0x9D5A08);
        int* ctrlFlag  = (int*)(g_exeBase + 0x9D5A0C);
        unsigned long long* af8Ptr = (unsigned long long*)(thisPtr + 0xAF8);
        int savedShift = *shiftFlag;
        int savedCtrl  = *ctrlFlag;
        unsigned long long savedAf8 = *af8Ptr;

        BuildMode_Write(msg.mode);
        *shiftFlag = msg.demolishShift ? 1 : 0;
        *ctrlFlag  = msg.demolishCtrl ? 1 : 0;
        if (msg.assignFromTool)
            *af8Ptr = 0;
        *(unsigned char*)(building + OBJ_DEMOLISH_FLAG_OFFSET) = msg.demolishEb1;

        InterlockedIncrement(&g_depth);
        __try {
            fn(thisPtr, building, msg.demolishArgs[0], msg.demolishArgs[1], msg.demolishArgs[2],
               msg.demolishArgs[3], msg.demolishArgs[4]);
        } __finally {
            InterlockedDecrement(&g_depth);
            *af8Ptr = savedAf8;
            *ctrlFlag = savedCtrl;
            *shiftFlag = savedShift;
            BuildMode_Write(savedMode);
        }

        if (IsInBuildingsVector(building)) {
            if (msg.demolishEb1 != 0) {
                *(int*)(building + 0x630) = msg.sliderValue;
            }
            if (msg.fromDialog424 && *(int*)(thisPtr + 0x5B8) > 1) {
                if (msg.autoSearchOffices == 0)
                    *(unsigned char*)(building + 0x879) = 0;
                int maxW = *(int*)(building + 0x62C);
                *(int*)(building + 0x630) = (msg.sliderValue < maxW) ? msg.sliderValue : maxW;
            }
            if (msg.assignFromTool && *(unsigned char*)(building + 0xEA8) != 0) {
                U64 office = Ref_BuildingAt(msg.assignOffice);
                if (office) {
                    typedef void (*AssignOfficeFn)(__int64 world, __int64 office, __int64 target,
                                                   float dist, unsigned char p5, unsigned char isDemolishOffice);
                    ((AssignOfficeFn)(g_exeBase + 0x161C90))(thisPtr, (__int64)office, building,
                                                             msg.assignDist, 0, msg.assignIsDemolishOffice);
                    if (msg.assignPriority) {
                        typedef void (*SetPriorityFn)(__int64 world, __int64 target, unsigned char prio);
                        ((SetPriorityFn)(g_exeBase + 0x1A04F0))(thisPtr, building, 1);
                    }
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DEMOLISH: <exception replaying demolition>");
        return false;
    }
}
