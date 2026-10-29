// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP Automated Unit & Integration Test Suite with Mock Engine Harness.
// Verifies refactored modules and all critical invariants from AGENTS.md without launching SOVIET64.exe.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <locale.h>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>

#include "../../src/tesmio_plugin.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "../lockstep/world_refs.h"
#include "../lockstep/commands.h"
#include "../lockstep/game_calls.h"
#include "../lockstep/building_switches.h"
#include "../determinism/determinism.h"
#include "../determinism/state_hash.h"
#include "../determinism/held_frame.h"
#include "../session/game_speed.h"
#include "../session/two_states.h"
#include "../session/save_sync.h"
#include "../session/mod_sync.h"
#include "../save/save_zip.h"
#include "../lockstep/purchase.h"
#include "../world_sync/vehicles/vehicle_soft_sync.h"
#include "../ui/sync_block.h"
#include "../world_sync/networks/road_sync.h"
#include "../game/session_patches.h"
#include "../game/input_block.h"
#include "../game/imgui_hook.h"
#include "../ui/native_menu.h"
#include "imgui.h"

extern "C" int TsmPluginInit(const TsmHost* origHost, TsmPluginInfo* info);

// ---------------------------------------------------------------------------
// Minimal Test Framework Macros
// ---------------------------------------------------------------------------

static int g_totalTests = 0;
static int g_passedTests = 0;
static int g_failedAssertions = 0;

#define TEST_CHECK(expr)                                                                                  \
    do {                                                                                                  \
        if (!(expr)) {                                                                                    \
            printf("  [FAIL] %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr);                  \
            ++g_failedAssertions;                                                                         \
            return false;                                                                                 \
        }                                                                                                 \
    } while (0)

#define TEST_CHECK_NEAR(a, b, eps)                                                                        \
    do {                                                                                                  \
        double _va = (double)(a);                                                                         \
        double _vb = (double)(b);                                                                         \
        if (fabs(_va - _vb) > (eps)) {                                                                    \
            printf("  [FAIL] %s:%d: %.6f != %.6f (eps=%g) [%s vs %s]\n",                                  \
                   __FILE__, __LINE__, _va, _vb, (double)(eps), #a, #b);                                  \
            ++g_failedAssertions;                                                                         \
            return false;                                                                                 \
        }                                                                                                 \
    } while (0)

// ---------------------------------------------------------------------------
// Mock Engine Memory Harness (16 MB fake SOVIET64.exe image)
// ---------------------------------------------------------------------------

static constexpr size_t MOCK_EXE_SIZE = 0x1000000; // 16 MB covers all RVAs in SOVIET64.exe
static BYTE* g_mockExeBuffer = nullptr;
static char  g_mockBaseDir[MAX_PATH] = {};
static TsmHost g_mockHost = {};

static void MockHost_Log(const char* fmt, ...)
{
    (void)fmt;
}

static void** MockHost_FindIatSlot(void*, const char*, const char*)
{
    return nullptr;
}

static int MockHost_PatchIat(void*, const char*, const char*, void*, void**, const char*)
{
    return 0;
}

static int MockHost_InstallInlineHook(void*, void*, void**, const unsigned char*, size_t, const char*)
{
    return 0;
}

static unsigned char* MockHost_AllocNear(unsigned char*, size_t)
{
    return nullptr;
}

static int MockHost_ReadablePtr(const void* p, size_t n)
{
    if (!p || n == 0) return 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    uintptr_t endRegion = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return ((uintptr_t)p + n <= endRegion) ? 1 : 0;
}

static long MockHost_FaultFilter(const char*, void*)
{
    return EXCEPTION_EXECUTE_HANDLER;
}

static const void* MockHost_Consume(const char*, unsigned)
{
    return nullptr;
}

static void InitMockHarness()
{
    if (!g_mockExeBuffer) {
        g_mockExeBuffer = (BYTE*)VirtualAlloc(nullptr, MOCK_EXE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    memset(g_mockExeBuffer, 0, MOCK_EXE_SIZE);
    g_mockExeBuffer[0x542A30] = 0xC3; // ret for MANAGER_RESET_RVA
    g_mockExeBuffer[0x5434C0] = 0xC3; // ret for CANCEL_SEG_RVA

    char tmpPath[MAX_PATH] = {};
    GetTempPathA(sizeof(tmpPath), tmpPath);
    _snprintf_s(g_mockBaseDir, sizeof(g_mockBaseDir), _TRUNCATE, "%sRepublicMP_TestRun_%lu",
                tmpPath, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_mockBaseDir, nullptr);

    g_mockHost.structSize        = sizeof(TsmHost);
    g_mockHost.apiVersion        = TSM_API_VERSION;
    g_mockHost.exeModule         = (void*)g_mockExeBuffer;
    g_mockHost.exeBase           = g_mockExeBuffer;
    g_mockHost.exeSize           = MOCK_EXE_SIZE;
    g_mockHost.engineModule      = nullptr;
    g_mockHost.baseDir           = g_mockBaseDir;
    g_mockHost.log               = &MockHost_Log;
    g_mockHost.findIatSlot       = &MockHost_FindIatSlot;
    g_mockHost.patchIat          = &MockHost_PatchIat;
    g_mockHost.installInlineHook = &MockHost_InstallInlineHook;
    g_mockHost.allocNear         = &MockHost_AllocNear;
    g_mockHost.readablePtr       = &MockHost_ReadablePtr;
    g_mockHost.faultFilter       = &MockHost_FaultFilter;
    g_mockHost.consume           = &MockHost_Consume;
    g_mockHost.vfsRoot           = g_mockBaseDir;

    TsmPluginInfo pluginInfo = {};
    TsmPluginInit(&g_mockHost, &pluginInfo);
    TsmBind(&g_mockHost);
}

static void ResetMockWorldMemory()
{
    TwoStates_ResetEconStatsForTest(nullptr);
    memset(g_mockExeBuffer, 0, MOCK_EXE_SIZE);
    g_mockExeBuffer[0x542A30] = 0xC3; // ret for MANAGER_RESET_RVA
    g_mockExeBuffer[0x5434C0] = 0xC3; // ret for CANCEL_SEG_RVA
}

// Helper to write a fake MSVC std::vector<T> header { begin, end, cap } at a target address
template <typename T>
static void WriteMockVectorHeader(BYTE* vecHeaderAddr, std::vector<T>& backingVec)
{
    T* beginPtr = backingVec.empty() ? nullptr : backingVec.data();
    T* endPtr   = beginPtr + backingVec.size();
    T* capPtr   = beginPtr + backingVec.capacity();
    *(T**)(vecHeaderAddr + 0x00) = beginPtr;
    *(T**)(vecHeaderAddr + 0x08) = endPtr;
    *(T**)(vecHeaderAddr + 0x10) = capPtr;
}

// ---------------------------------------------------------------------------
// Test 1: RVA Invariants, POD SEH Memory Helpers & Thread-Safe InitOnce Locks
// ---------------------------------------------------------------------------

static bool Test_RvaAndSafeMem()
{
    ResetMockWorldMemory();

    // Verify canonical RVAs match AGENTS.md Section 3
    TEST_CHECK(WORLD_RVA == 0x9D4F10);
    TEST_CHECK(VEC_BUILDINGS_BEGIN_RVA == 0x9E6A18);
    TEST_CHECK(VEC_CONSTRUCTIONS_BEGIN_RVA == 0x9E6B38);
    TEST_CHECK(WORKSHOP_MGR_PTR_RVA == 0x9E22B8);
    TEST_CHECK(MANAGER_RESET_STATE_OFFSET == 0x408);
    TEST_CHECK(MGR_ROUTE_BEGIN_OFFSET == 0x410);
    TEST_CHECK(HEADER_BIN_DATA_OFFSET == 0x260);
    TEST_CHECK(HEADER_BIN_RECORD_SIZE == 0x80);
    TEST_CHECK(WORLD_WORK_RESOURCE_PTR_OFF == 0xC2C8);
    TEST_CHECK(WORLD_ECON_TAB_OFFSET == 0xC65C);
    TEST_CHECK(WORLD_LOANS_OFFSET == 0x10BD0);
    TEST_CHECK(WORLD_LOAN_SELECTED_OFFSET == 0x10BE8);
    TEST_CHECK(WORLD_LOAN_REPAY_AMOUNT_OFF == 0x10C08);
    TEST_CHECK(WORLD_LOAN_STATS_OFFSET == 0x11718);
    TEST_CHECK(RVA_VEC_LOAN_PUSH_BACK == 0x448AA0);
    TEST_CHECK(RVA_TAKE_LOAN == 0x4B9140);
    TEST_CHECK(RVA_GET_MAX_AVAILABLE_BORROW == 0x4B9380);
    TEST_CHECK(RVA_STEP_ONE_LOAN_DAY == 0x4B9490);
    TEST_CHECK(RVA_STEP_ALL_LOANS_DAILY == 0x4B9660);
    TEST_CHECK(RVA_CALC_INTEREST_RATE == 0x4B9A80);
    TEST_CHECK(sizeof(SovietLoan) == 0x28);
    TEST_CHECK(offsetof(SovietLoan, yearInterestRate) == 0x00);
    TEST_CHECK(offsetof(SovietLoan, loanSubType) == 0x0C);
    TEST_CHECK(offsetof(SovietLoan, remainingPrincipal) == 0x14);
    TEST_CHECK(offsetof(SovietLoan, unpaidPenaltyBalance) == 0x18);
    TEST_CHECK(offsetof(SovietLoan, totalPaidSoFar) == 0x24);
    TEST_CHECK(sizeof(LsLoanCommand) == 84);

    // SafeReadPod / SafeWritePod on valid memory
    uint64_t val = 0;
    uint64_t target = 0x1122334455667788ULL;
    TEST_CHECK(SafeReadPod(&target, &val));
    TEST_CHECK(val == 0x1122334455667788ULL);
    TEST_CHECK(SafeWritePod(&target, 0xAABBCCDDEEFF0011ULL));
    TEST_CHECK(target == 0xAABBCCDDEEFF0011ULL);

    // SafeReadPod / SafeWritePod on nullptr and PAGE_NOACCESS memory (SEH protection)
    TEST_CHECK(!SafeReadPod<uint64_t>(nullptr, &val));
    void* noAccessPage = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    TEST_CHECK(noAccessPage != nullptr);
    TEST_CHECK(!SafeReadPod<uint64_t>(noAccessPage, &val));
    TEST_CHECK(!SafeWritePod<uint64_t>(noAccessPage, 42ULL));
    VirtualFree(noAccessPage, 0, MEM_RELEASE);

    // SafeCountStdVector tests
    std::vector<uint64_t> items = { 10, 20, 30, 40, 50 };
    BYTE vecHdr[24] = {};
    WriteMockVectorHeader(vecHdr, items);
    TEST_CHECK(SafeCountStdVector(vecHdr, sizeof(uint64_t), 100) == 5);
    TEST_CHECK(SafeCountStdVector(vecHdr, sizeof(uint64_t), 3) == 0); // exceeds maxElements

    // Corrupted vector header (end < begin)
    *(uintptr_t*)(vecHdr + 0x00) = 0x2000;
    *(uintptr_t*)(vecHdr + 0x08) = 0x1000;
    TEST_CHECK(SafeCountStdVector(vecHdr, sizeof(uint64_t), 100) == 0);

    // Invariant #12: IsPointerLikeX64 including low Windows x64 heap segments (high >= 1 && high < 0x100)
    TEST_CHECK(!IsPointerLikeX64(0ULL));
    TEST_CHECK(!IsPointerLikeX64(100ULL));
    TEST_CHECK(!IsPointerLikeX64(0x00000000FFFFFFFFULL)); // two 32-bit ints/floats with high == 0
    TEST_CHECK(IsPointerLikeX64((uint64_t)(uintptr_t)&target));

    // Unmapped low-heap address (high == 2, but not committed) -> must return false
    TEST_CHECK(!IsPointerLikeX64(0x00000002deadbeefULL & ~3ULL));

    // Try allocating a real page in low x64 address space (high in 1..0xFF) to verify low segment detection
    void* lowPage = nullptr;
    for (uintptr_t hi = 1; hi < 0x100 && !lowPage; ++hi) {
        lowPage = VirtualAlloc((LPVOID)(hi << 32), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (lowPage) {
        *(uint64_t*)lowPage = 0x12345678ULL;
        TEST_CHECK(IsPointerLikeX64((uint64_t)(uintptr_t)lowPage));
        VirtualFree(lowPage, 0, MEM_RELEASE);
    }

    // Multithreaded stress test of TsmEnsureCriticalSection + TsmScopedCsLock
    static INIT_ONCE s_testOnce = INIT_ONCE_STATIC_INIT;
    static CRITICAL_SECTION s_testCs;
    int sharedCounter = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            for (int k = 0; k < 1000; ++k) {
                TsmScopedCsLock guard(&s_testOnce, &s_testCs);
                ++sharedCounter;
            }
        });
    }
    for (auto& th : threads) th.join();
    TEST_CHECK(sharedCounter == 8000);

    // SafeBytesMatch & TsmCheckRoutineTable tests
    const unsigned char pattern[4] = { 0x48, 0x89, 0x5C, 0x24 };
    memcpy(g_mockExeBuffer + 0x12340, pattern, 4);
    TEST_CHECK(SafeBytesMatch(g_mockExeBuffer + 0x12340, pattern, 4));
    TEST_CHECK(!SafeBytesMatch(g_mockExeBuffer + 0x12341, pattern, 4));
    TEST_CHECK(!SafeBytesMatch(nullptr, pattern, 4));

    const TsmRoutineSig sigs[1] = { { 0x12340, { 0x48, 0x89, 0x5C, 0x24 }, 4 } };
    int sigCache = -1;
    unsigned badRva = 0;
    TEST_CHECK(TsmCheckRoutineTable(g_mockExeBuffer, &sigCache, sigs, 1, &badRva));
    TEST_CHECK(sigCache == 1 && badRva == 0);
    g_mockExeBuffer[0x12340] = 0x90;
    sigCache = -1;
    TEST_CHECK(!TsmCheckRoutineTable(g_mockExeBuffer, &sigCache, sigs, 1, &badRva));
    TEST_CHECK(sigCache == 0 && badRva == 0x12340);

    // SafeIsWorldLoaded & SafeIsToolGuardBusy tests
    TEST_CHECK(!SafeIsWorldLoaded(g_mockExeBuffer));
    std::vector<BYTE*> dummyBlds = { g_mockExeBuffer + 0x2000 };
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_BUILDINGS_OFFSET, dummyBlds);
    TEST_CHECK(SafeIsWorldLoaded(g_mockExeBuffer));

    BYTE* worldPtr = g_mockExeBuffer + WORLD_RVA;
    TEST_CHECK(!SafeIsToolGuardBusy((uintptr_t)worldPtr));
    worldPtr[TOOL_GUARD2_OFFSET] = 1;
    TEST_CHECK(SafeIsToolGuardBusy((uintptr_t)worldPtr));
    worldPtr[TOOL_GUARD2_OFFSET] = 0;
    TEST_CHECK(!SafeIsToolGuardBusy((uintptr_t)worldPtr));

    return true;
}

// ---------------------------------------------------------------------------
// Test 2: World Object References (WorldRefs: Buildings, Constructions, Vehicles, Lines, Nodes)
// ---------------------------------------------------------------------------

static bool Test_WorldRefs()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Create 2 completed buildings (list 0 at WORLD_BUILDINGS_OFFSET = 0x11B08)
    // and 2 unfinished/demolition constructions (list 1 at WORLD_CONSTRUCTIONS_OFFSET = 0x11C28)
    std::vector<BYTE> bld0(128, 0), bld1(128, 0), con0(128, 0), con1(128, 0);
    *(unsigned*)bld0.data() = 1001u;
    *(unsigned*)bld1.data() = 1002u;
    *(unsigned*)con0.data() = 2001u;
    *(unsigned*)con1.data() = 2002u;

    std::vector<BYTE*> bldVec = { bld0.data(), bld1.data() };
    std::vector<BYTE*> conVec = { con0.data(), con1.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);
    WriteMockVectorHeader(world + WORLD_CONSTRUCTIONS_OFFSET, conVec);

    RouteRef refB1 = kNoRef, refC1 = kNoRef;
    TEST_CHECK(Ref_Building((U64)bld1.data(), refB1));
    TEST_CHECK(refB1.list == 0 && refB1.place == 1 && refB1.check == 1002u);
    TEST_CHECK(Ref_BuildingAt(refB1) == (U64)bld1.data());

    TEST_CHECK(Ref_Building((U64)con1.data(), refC1));
    TEST_CHECK(refC1.list == 1 && refC1.place == 1 && refC1.check == 2002u);
    TEST_CHECK(Ref_BuildingAt(refC1) == (U64)con1.data());

    // Tamper check ID -> Ref_BuildingAt must reject
    RouteRef badRef = refB1;
    badRef.check = 9999u;
    TEST_CHECK(Ref_BuildingAt(badRef) == 0);
    badRef = refB1;
    badRef.place = 5;
    TEST_CHECK(Ref_BuildingAt(badRef) == 0);

    // Vehicles (WORLD_VEHICLES_OFFSET = 0x12810, ID at +0x04)
    std::vector<BYTE> veh0(64, 0), veh1(64, 0);
    *(unsigned*)(veh0.data() + VEH_ID_OFFSET) = 501u;
    *(unsigned*)(veh1.data() + VEH_ID_OFFSET) = 502u;
    std::vector<BYTE*> vehVec = { veh0.data(), veh1.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehVec);

    RouteRef refV = kNoRef;
    TEST_CHECK(Ref_Vehicle((U64)veh1.data(), refV));
    TEST_CHECK(refV.place == 1 && refV.check == 502u);
    TEST_CHECK(Ref_VehicleAt(refV) == (U64)veh1.data());

    // Lines (WORLD_LINES_OFFSET = 0x12800 points to a lines manager with 2 vectors at +0x00 and +0x18)
    std::vector<BYTE> lineMgr(0x40, 0), lineObj(32, 0);
    *(unsigned*)(lineObj.data() + 4) = 77u;
    *(unsigned*)(lineObj.data() + 8) = 88u;
    std::vector<BYTE*> lineList1 = { lineObj.data() };
    WriteMockVectorHeader(lineMgr.data() + 0x18, lineList1);
    *(BYTE**)(world + WORLD_LINES_OFFSET) = lineMgr.data();

    RouteRef refL = kNoRef;
    TEST_CHECK(Ref_Line((U64)lineObj.data(), refL));
    TEST_CHECK(refL.list == 1 && refL.place == 0);
    TEST_CHECK(Ref_LineAt(refL) == (U64)lineObj.data());

    // Nodes (road network at world + 0xF908, rail network at world + 0xFA28, node vector at +0x298)
    std::vector<BYTE> railNet(0x300, 0), nodeObj(32, 0);
    *(unsigned*)(nodeObj.data() + 4)   = 0x41200000u; // float X
    *(unsigned*)(nodeObj.data() + 0xC) = 0x42200000u; // float Z
    std::vector<BYTE*> nodeVec = { nodeObj.data() };
    WriteMockVectorHeader(railNet.data() + 0x298, nodeVec);
    *(BYTE**)(world + 0xFA28) = railNet.data();

    RouteRef refN = kNoRef;
    TEST_CHECK(Ref_Node((U64)nodeObj.data(), refN));
    TEST_CHECK(refN.list == 0xFA28 && refN.place == 0);
    TEST_CHECK(Ref_NodeAt(refN) == (U64)nodeObj.data());
    TEST_CHECK(Ref_NodeCheck((U64)nodeObj.data()) == refN.check);

    // Ref_SetStandInVector, Ref_IndexInVector, Ref_AtVectorIndex
    BYTE standIn[24] = {};
    U64 ptrArray[3] = { 0x1001, 0x1002, 0x1003 };
    Ref_SetStandInVector(standIn, ptrArray, sizeof(ptrArray));
    TEST_CHECK(Ref_IndexInVector((U64)standIn, 0x1002) == 1u);
    TEST_CHECK(Ref_IndexInVector((U64)standIn, 0x9999) == 0xFFFFFFFFu);
    TEST_CHECK(Ref_AtVectorIndex((U64)standIn, 2u) == 0x1003ULL);
    TEST_CHECK(Ref_AtVectorIndex((U64)standIn, 3u) == 0ULL);

    // Ref_DefPtr and Ref_DefPtrAt (Resource stride 0x340 and VehicleType stride 0x9F08)
    std::vector<BYTE> fakeResources(WORLD_RESOURCE_STRIDE * 4, 0);
    std::vector<BYTE> fakeVehTypes(WORLD_VEHICLE_TYPE_STRIDE * 3, 0);
    *(U64*)(world + WORLD_RESOURCES_OFFSET)     = (U64)fakeResources.data();
    *(U64*)(world + WORLD_RESOURCES_OFFSET + 8) = (U64)(fakeResources.data() + fakeResources.size());
    *(U64*)(world + WORLD_VEHICLE_TYPES_OFFSET)     = (U64)fakeVehTypes.data();
    *(U64*)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = (U64)(fakeVehTypes.data() + fakeVehTypes.size());

    unsigned char defTag = 99;
    unsigned defIdx = 99;
    U64 resolvedDef = 123;
    TEST_CHECK(Ref_DefPtr(0, defTag, defIdx) && defTag == 0 && defIdx == 0);
    TEST_CHECK(Ref_DefPtrAt(0, 0, resolvedDef) && resolvedDef == 0);

    U64 res2Ptr = (U64)(fakeResources.data() + 2 * WORLD_RESOURCE_STRIDE);
    TEST_CHECK(Ref_DefPtr(res2Ptr, defTag, defIdx) && defTag == 1 && defIdx == 2u);
    TEST_CHECK(Ref_DefPtrAt(defTag, defIdx, resolvedDef) && resolvedDef == res2Ptr);

    U64 vt1Ptr = (U64)(fakeVehTypes.data() + 1 * WORLD_VEHICLE_TYPE_STRIDE);
    TEST_CHECK(Ref_DefPtr(vt1Ptr, defTag, defIdx) && defTag == 2 && defIdx == 1u);
    TEST_CHECK(Ref_DefPtrAt(defTag, defIdx, resolvedDef) && resolvedDef == vt1Ptr);

    // Unaligned pointer inside Resource table and out-of-bounds index must fail
    TEST_CHECK(!Ref_DefPtr(res2Ptr + 4, defTag, defIdx));
    TEST_CHECK(!Ref_DefPtrAt(1, 10, resolvedDef));

    return true;
}

// ---------------------------------------------------------------------------
// Test 3: Deterministic State Hashing (StateHash_Compute & Per-Object Records)
// ---------------------------------------------------------------------------

static bool Test_StateHash()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(int*)(world + WORLD_DAY_OFFSET)        = 145;
    *(int*)(world + WORLD_YEAR_OFFSET)       = 1965;
    *(float*)(world + WORLD_PROGRESS_OFFSET) = 0.375f;
    *(double*)(world + WORLD_USD_OFFSET)     = 250000.0;
    *(double*)(world + WORLD_RUB_OFFSET)     = 1500000.0;

    // Construct a mock building with 1 storage slot containing 1 resource entry {"steel", 42.5f}
    std::vector<BYTE> bld(0x1000, 0);
    std::vector<BYTE> slot(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> entry(0x10, 0);
    const char* steelName = "steel";
    *(const char**)(entry.data() + 0x00) = steelName;
    *(float*)(entry.data() + 0x08)       = 42.5f;

    // Slot is a vector of 0x10-byte entries at slot + 0x00
    *(BYTE**)(slot.data() + 0x00) = entry.data();
    *(BYTE**)(slot.data() + 0x08) = entry.data() + 0x10;

    *(int*)(bld.data() + 0x00)                  = 10;     // index
    *(float*)(bld.data() + BLD_PROGRESS_OFFSET) = 1.0f;   // finished
    *(float*)(bld.data() + BLD_PRODUCED_OFFSET) = 120.0f; // produced
    *(BYTE**)(bld.data() + BLD_STORAGE_OFFSET)       = slot.data();
    *(BYTE**)(bld.data() + BLD_STORAGE_OFFSET + 0x8) = slot.data() + BLD_STORAGE_SLOT_SIZE;

    std::vector<BYTE*> bldVec = { bld.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    // Construct a mock vehicle
    std::vector<BYTE> veh(0x1000, 0);
    *(int*)(veh.data() + VEH_ID_OFFSET)        = 7;
    *(int*)(veh.data() + VEH_SCHED_IDX_OFFSET) = 2;
    *(float*)(veh.data() + VEH_POS_OFFSET + 0) = 125.0f;
    *(float*)(veh.data() + VEH_POS_OFFSET + 4) = 15.0f;
    *(float*)(veh.data() + VEH_POS_OFFSET + 8) = -340.0f;
    std::vector<BYTE*> vehVec = { veh.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehVec);

    // Construct a mock citizen living in `bld`
    std::vector<BYTE> cit(0x800, 0);
    *(BYTE**)(cit.data() + 0x20)  = bld.data();
    *(float*)(cit.data() + 0xD4)  = 28.5f;
    *(float*)(cit.data() + 0x620) = 10.0f;
    std::vector<BYTE*> citVec = { cit.data() };
    WriteMockVectorHeader(world + WORLD_CITIZENS_OFFSET, citVec);

    StateSummary s1 = {}, s2 = {};
    TEST_CHECK(StateHash_Compute(&s1));
    TEST_CHECK(StateHash_Compute(&s2));
    TEST_CHECK(memcmp(&s1, &s2, sizeof(StateSummary)) == 0);
    TEST_CHECK(s1.buildings == 1 && s1.vehicles == 1 && s1.citizens == 1);
    TEST_CHECK_NEAR(s1.storageTotal, 42.5, 1e-4);
    TEST_CHECK_NEAR(s1.productionTotal, 120.0, 1e-4);
    TEST_CHECK_NEAR(s1.ageTotal, 28.5, 1e-4);

    // Mutate storage amount -> only hashBuildings should change
    *(float*)(entry.data() + 0x08) = 43.0f;
    StateSummary sBldMut = {};
    TEST_CHECK(StateHash_Compute(&sBldMut));
    TEST_CHECK(sBldMut.hashBuildings != s1.hashBuildings);
    TEST_CHECK(sBldMut.hashClock == s1.hashClock);
    TEST_CHECK(sBldMut.hashVehicles == s1.hashVehicles);
    TEST_CHECK(sBldMut.hashCitizens == s1.hashCitizens);

    // Mutate vehicle position -> only hashVehiclesPos should change (not hashVehicles cargo/schedule)
    *(float*)(veh.data() + VEH_POS_OFFSET + 0) = 126.0f;
    StateSummary sVehPosMut = {};
    TEST_CHECK(StateHash_Compute(&sVehPosMut));
    TEST_CHECK(sVehPosMut.hashVehicles == sBldMut.hashVehicles);
    TEST_CHECK(sVehPosMut.hashVehiclesPos != sBldMut.hashVehiclesPos);

    // Mutate treasury balance -> rub/usd updated, while hashClock stays strictly tied to simulation clock
    *(double*)(world + WORLD_RUB_OFFSET) = 1500004.14;
    StateSummary sRubMut = {};
    TEST_CHECK(StateHash_Compute(&sRubMut));
    TEST_CHECK(sRubMut.hashClock == sVehPosMut.hashClock);
    TEST_CHECK_NEAR(sRubMut.rub, 1500004.14, 1e-4);

    // Mutate simulation dayProgress -> hashClock changes
    *(float*)(world + WORLD_PROGRESS_OFFSET) = 0.500f;
    StateSummary sClkMut = {};
    TEST_CHECK(StateHash_Compute(&sClkMut));
    TEST_CHECK(sClkMut.hashClock != sRubMut.hashClock);

    // DLC building token extraction & list checks
    char dlcTok[16] = {};
    TEST_CHECK(SaveSync_ExtractBuildingDlcToken("DLC3_swimming_hall", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc3") == 0);
    TEST_CHECK(SaveSync_ExtractBuildingDlcToken("dlc2_residential01", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc2") == 0);
    TEST_CHECK(!SaveSync_ExtractBuildingDlcToken("shop_prior", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(SaveSync_IsDlcTokenInList("dlc2,dlc3", "dlc2"));
    TEST_CHECK(SaveSync_IsDlcTokenInList("dlc2,dlc3", "dlc3"));
    TEST_CHECK(!SaveSync_IsDlcTokenInList("dlc2,dlc3", "dlc1"));
    TEST_CHECK(!SaveSync_IsDlcTokenInList("", "dlc3"));

    // Per-object records extraction
    std::vector<BuildingRecord> bldRecs;
    std::vector<VehicleRecord> vehRecs;
    std::vector<CitizenRecord> citRecs;
    TEST_CHECK(StateHash_BuildingRecords(bldRecs) && bldRecs.size() == 1);
    TEST_CHECK(bldRecs[0].index == 10 && bldRecs[0].slots == 1 && bldRecs[0].slotEntries[0] == 1);
    TEST_CHECK(StateHash_VehicleRecords(vehRecs) && vehRecs.size() == 1);
    TEST_CHECK(vehRecs[0].index == 7);
    TEST_CHECK(StateHash_CitizenRecords(citRecs) && citRecs.size() == 1);
    TEST_CHECK(citRecs[0].home == 10);

    return true;
}

// ---------------------------------------------------------------------------
// Test 4: TwoStates Territory Zones, Ownership, Economy Attribution & Invariant #10
// ---------------------------------------------------------------------------

static bool Test_TwoStates_ZonesAndEconomy()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(int*)(world + WORLD_UNLIMITED_MONEY_OFFSET) = 0;
    *(double*)(world + WORLD_RUB_OFFSET) = 2000000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 500000.0;

    double initRub[2] = { 1200000.0, 800000.0 };
    double initUsd[2] = { 300000.0,  200000.0 };
    TwoStates_OnSessionJoined(/*isHost=*/true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    TEST_CHECK(TwoStates_IsActive());
    TEST_CHECK(TwoStates_LocalState() == 1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1200000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 800000.0, 1e-3);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 2000000.0, 1e-3);

    // Authoritative treasury soft-sync heal from Host
    TwoStates_SyncBalancesFromHost(1150000.0, 750000.0, 290000.0, 195000.0);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1150000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 750000.0, 1e-3);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1900000.0, 1e-3);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 485000.0, 1e-3);
    TwoStates_SyncBalancesFromHost(1200000.0, 800000.0, 300000.0, 200000.0);

    // Zone geometry checks: West (< -250), Shared ([-250 .. +250]), East (> +250)
    TEST_CHECK(TwoStates_GetZoneAt(-500.0f, 0.0f) == 1);
    TEST_CHECK(TwoStates_GetZoneAt(   0.0f, 0.0f) == 0);
    TEST_CHECK(TwoStates_GetZoneAt(-250.0f, 0.0f) == 0);
    TEST_CHECK(TwoStates_GetZoneAt( 250.0f, 0.0f) == 0);
    TEST_CHECK(TwoStates_GetZoneAt( 500.0f, 0.0f) == 2);

    // State 1 (West) can build in West (1) and Shared Border Zone (0), but NOT East (2)
    TEST_CHECK(TwoStates_CanBuildAt(-600.0f, 0.0f, 1));
    TEST_CHECK(TwoStates_CanBuildAt( 100.0f, 0.0f, 1));
    TEST_CHECK(!TwoStates_CanBuildAt( 600.0f, 0.0f, 1));

    // State 2 (East) can build in East (2) and Shared Border Zone (0), but NOT West (1)
    TEST_CHECK(!TwoStates_CanBuildAt(-600.0f, 0.0f, 2));
    TEST_CHECK(TwoStates_CanBuildAt(-100.0f, 0.0f, 2));
    TEST_CHECK(TwoStates_CanBuildAt( 600.0f, 0.0f, 2));

    // Command execution wallet swapping + automatic registration of newly placed buildings
    std::vector<BYTE> newBld(0x800, 0);
    *(int*)newBld.data() = 42;
    *(float*)(newBld.data() + OBJ_POS_OFFSET) = 50.0f; // Placed in shared border zone by State 2!
    std::vector<BYTE*> bldVec;
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    TwoStates_BeginCommand(/*ownerState=*/2);
    // Inside State 2's command, world RUB must equal State 2's RUB (800,000)
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 800000.0, 1e-3);
    // Simulate spending 50,000 RUB and spawning building #42
    *(double*)(world + WORLD_RUB_OFFSET) -= 50000.0;
    bldVec.push_back(newBld.data());
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);
    TwoStates_EndCommand(/*ownerState=*/2);

    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1200000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 750000.0, 1e-3);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1950000.0, 1e-3);
    // Building #42 was placed in shared zone (X=+50), so its registered owner must be State 2
    TEST_CHECK(TwoStates_GetBuildingOwner(42, 50.0f, 0.0f) == 2);
    TEST_CHECK(TwoStates_CanControlBuilding(42, 50.0f, 0.0f, 2));
    TEST_CHECK(!TwoStates_CanControlBuilding(42, 50.0f, 0.0f, 1));

    // Proportional step delta splitting: 1 building in State 1 vs 3 buildings in State 2 -> w0 = 0.25
    TEST_CHECK_NEAR(TwoStates_ComputeStepWeightState1(1, 3), 0.25, 1e-6);
    TEST_CHECK_NEAR(TwoStates_ComputeStepWeightState1(0, 0), 0.50, 1e-6);

    std::vector<BYTE> b1(0x800, 0), b2(0x800, 0), b3(0x800, 0);
    *(int*)b1.data() = 101; *(float*)(b1.data() + OBJ_POS_OFFSET) = -1000.0f; *(float*)(b1.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(int*)b2.data() = 102; *(float*)(b2.data() + OBJ_POS_OFFSET) =  1000.0f; *(float*)(b2.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(int*)b3.data() = 103; *(float*)(b3.data() + OBJ_POS_OFFSET) =  1500.0f; *(float*)(b3.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    // Total buildings: b1 (West, State 1) + newBld, b2, b3 (State 2) = 1 West vs 3 East
    bldVec = { b1.data(), newBld.data(), b2.data(), b3.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    TwoStates_BeforeStep(world);
    // Simulate unattributed simulation expense of -10,000 RUB
    *(double*)(world + WORLD_RUB_OFFSET) -= 10000.0;
    TwoStates_AfterStep(world);

    // State 1 pays 25% (2,500 RUB), State 2 pays 75% (7,500 RUB)
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1197500.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 742500.0, 1e-3);

    // Inter-state money transfer
    LsStateTransfer tr = { 1, 2, 97500.0, 50000.0 };
    TEST_CHECK(TwoStates_ApplyTransferCommand(tr));
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1100000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 840000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 250000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 250000.0, 1e-3);

    // Invalid transfers must be rejected
    LsStateTransfer badTr1 = { 1, 1, 1000.0, 0.0 };
    LsStateTransfer badTr2 = { 1, 2, -500.0, 0.0 };
    TEST_CHECK(!TwoStates_ApplyTransferCommand(badTr1));
    TEST_CHECK(!TwoStates_ApplyTransferCommand(badTr2));

    // Invariant #10 (AGENTS.md): Unlimited Money mode (world + 0x578 != 0)
    // Must NEVER sum 100M + 100M = 200M into WorldRubles()/WorldDollars().
    *(int*)(world + WORLD_UNLIMITED_MONEY_OFFSET) = 1;
    *(double*)(world + WORLD_RUB_OFFSET) = 100000000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 100000000.0;
    TwoStates_BeforeStep(world);
    TwoStates_AfterStep(world);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 100000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 100000000.0, 1e-2);

    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Invariant #4 (script.ini Safety) & Invariant #5 (LC_NUMERIC Cents Meta)
// ---------------------------------------------------------------------------

static bool Test_TwoStates_SaveMetaAndScriptIniSafety()
{
    ResetMockWorldMemory();

    // Invariant #4: Base terrain directories must NEVER be considered safe for script.ini patching
    TEST_CHECK(!TwoStates_IsSafeSaveScriptPath(nullptr));
    TEST_CHECK(!TwoStates_IsSafeSaveScriptPath(""));
    TEST_CHECK(!TwoStates_IsSafeSaveScriptPath("C:\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\terrains\\hills"));
    TEST_CHECK(!TwoStates_IsSafeSaveScriptPath("D:/SovietRepublic/media_soviet/dlc1/terrains/island"));
    TEST_CHECK(!TwoStates_IsSafeSaveScriptPath("terrains\\test_map"));
    TEST_CHECK(TwoStates_IsSafeSaveScriptPath("C:\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save\\my_coop_save"));
    TEST_CHECK(TwoStates_IsSafeSaveScriptPath("C:\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save_coop\\coop_2states"));
    TEST_CHECK(TwoStates_IsSafeSaveScriptPath("save\\mp_syncsave_host"));

    // Create a temporary terrain directory AND a temporary save directory on disk
    std::filesystem::path baseDir(g_mockBaseDir);
    std::filesystem::path fakeTerrainDir = baseDir / "media_soviet" / "terrains" / "hills";
    std::filesystem::path fakeSaveDir    = baseDir / "media_soviet" / "save" / "test_coop_meta";
    std::filesystem::create_directories(fakeTerrainDir);
    std::filesystem::create_directories(fakeSaveDir);

    const char* sampleScriptIni =
        "$WORLDSIZE 20000.000000 20000.000000\r\n"
        "$BOUNDARY_XMIN 1800.000000\r\n"
        "$BOUNDARY_ZMIN 1800.000000\r\n"
        "$BOUNDARY_XMAX -1800.000000\r\n"
        "$BOUNDARY_ZMAX -1800.000000\r\n"
        "$BORDER_POLYGON 4\r\n"
        "-8200.0 0.0 -8200.0 1\r\n"
        "8200.0 0.0 -8200.0 1\r\n"
        "8200.0 0.0 8200.0 1\r\n"
        "-8200.0 0.0 8200.0 1\r\n";

    auto writeTextFile = [](const std::filesystem::path& p, const char* txt) {
        FILE* f = nullptr;
        if (fopen_s(&f, p.string().c_str(), "wb") == 0 && f) {
            fwrite(txt, 1, strlen(txt), f);
            fclose(f);
        }
    };
    auto readTextFile = [](const std::filesystem::path& p) -> std::string {
        FILE* f = nullptr;
        if (fopen_s(&f, p.string().c_str(), "rb") != 0 || !f) return "";
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string s((size_t)sz, '\0');
        if (sz > 0) fread(&s[0], 1, (size_t)sz, f);
        fclose(f);
        return s;
    };

    writeTextFile(fakeTerrainDir / "script.ini", sampleScriptIni);
    writeTextFile(fakeSaveDir / "script.ini", sampleScriptIni);

    TwoStates_SetExpandBorders(true);
    // Attempting to patch a terrain directory MUST fail and leave the file untouched
    TEST_CHECK(!TwoStates_PatchSaveScriptIni(fakeTerrainDir.string().c_str(), 0.0f));
    TEST_CHECK(readTextFile(fakeTerrainDir / "script.ini") == sampleScriptIni);

    // Patching a save directory when g_expandBorders == true MUST succeed
    TEST_CHECK(TwoStates_PatchSaveScriptIni(fakeSaveDir.string().c_str(), 0.0f));
    std::string patchedScript = readTextFile(fakeSaveDir / "script.ini");
    TEST_CHECK(patchedScript.find("$BOUNDARY_XMIN 150.000000") != std::string::npos);
    TEST_CHECK(patchedScript.find("-9850.0 0.0 -9850.0 1") != std::string::npos);
    TwoStates_SetExpandBorders(false);

    // Invariant #5: republicmp_coop.ini serialization/parsing in integer cents/kopecks under comma LC_NUMERIC locale
    double saveRub[2] = { 1234567.89, 987654.32 };
    double saveUsd[2] = { 54321.15,   10999.95 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 350.0f, true, saveRub, saveUsd);
    TwoStates_RegisterBuildingOwner(777, 2);
    TwoStates_RegisterVehicleOwner(888, 2);

    const char* prevLocale = setlocale(LC_NUMERIC, nullptr);
    std::string savedLocale = prevLocale ? prevLocale : "C";
    setlocale(LC_NUMERIC, "Russian_Russia.1251");

    TEST_CHECK(TwoStates_WriteSaveMetaToDir(fakeSaveDir.string().c_str()));

    // Reset state to different values and load back from republicmp_coop.ini
    double zeroMoney[2] = { 0.0, 0.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_ONE_STATE, 100.0f, false, zeroMoney, zeroMoney);
    TEST_CHECK(TwoStates_GetMode() == COOP_MODE_ONE_STATE);

    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(fakeSaveDir.string().c_str(), /*updateBudgetsAndMode=*/true));
    setlocale(LC_NUMERIC, savedLocale.c_str());

    TEST_CHECK(TwoStates_GetMode() == COOP_MODE_TWO_STATES);
    TEST_CHECK_NEAR(TwoStates_GetBorderHalfWidth(), 350.0f, 1e-3);
    TEST_CHECK(TwoStates_GetExpandBorders() == true);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1234567.89, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 987654.32, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 54321.15, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 10999.95, 1e-2);
    TEST_CHECK(TwoStates_GetBuildingOwner(777, -500.0f, 0.0f) == 2);

    // Calling TwoStates_OnSessionJoined again (SaveSync_StartHost -> RunHostJoin) must NOT wipe building/vehicle owners
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 350.0f, true, saveRub, saveUsd);
    TEST_CHECK(TwoStates_GetBuildingOwner(777, -500.0f, 0.0f) == 2);

    // Ending a session while the world remains loaded must NOT double treasuries on subsequent PrepareHostSave/save
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1; // WORLD_ROAD_NETWORK_RVA != 0 (world loaded)
    TwoStates_OnSessionEnded();
    double prepRub[2] = {}, prepUsd[2] = {};
    TwoStates_PrepareHostSave(false, prepRub, prepUsd);
    TEST_CHECK_NEAR(prepRub[0], 1234567.89, 1e-2);
    TEST_CHECK_NEAR(prepRub[1], 987654.32, 1e-2);
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;

    // Staging save promotion: a newer mp_syncsave_* folder with header.bin, stats.ini, and republicmp_coop.ini
    // must be promoted to coop_last_session so user saves into mp_syncsave_* are never lost.
    std::filesystem::path bankDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save_promo_test";
    std::filesystem::path stagingSave = bankDir / "mp_syncsave_test99";
    std::error_code promoEc;
    std::filesystem::create_directories(stagingSave, promoEc);
    writeTextFile(stagingSave / "header.bin", "HEADER_V2");
    writeTextFile(stagingSave / "stats.ini", "STATS_V2");
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(stagingSave.string().c_str()));
    TEST_CHECK(TwoStates_PromoteStagingSavesInDir(bankDir.string().c_str()));
    TEST_CHECK(readTextFile(bankDir / "coop_last_session" / "header.bin") == "HEADER_V2");
    // Second call with identical timestamp must be a no-op (returns false)
    TEST_CHECK(!TwoStates_PromoteStagingSavesInDir(bankDir.string().c_str()));
    std::filesystem::remove_all(bankDir, promoEc);

    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Test 6: ModSync header.bin Binary Parser (Invariant #9) & Workshop Manager Filter (Invariant #6)
// ---------------------------------------------------------------------------

static bool Test_ModSync_HeaderBinAndWorkshopFilter()
{
    ResetMockWorldMemory();

    // 1. Test ModSync_ExtractHeaderBinModIdsFromBuffer (Invariant #9: 0x260 header + 0x80 records)
    const size_t numRecs = 6;
    std::vector<BYTE> hdrBin(HEADER_BIN_DATA_OFFSET + numRecs * HEADER_BIN_RECORD_SIZE, 0);

    auto writeRec = [&](size_t idx, const char* str) {
        char* dst = (char*)(hdrBin.data() + HEADER_BIN_DATA_OFFSET + idx * HEADER_BIN_RECORD_SIZE);
        strncpy_s(dst, HEADER_BIN_RECORD_SIZE, str, _TRUNCATE);
    };

    writeRec(0, "2850123456/custom_heating_plant");
    writeRec(1, "MIRRORZ_2999888777/mirrored_bus_station");
    writeRec(2, "vanilla_building_without_slash");
    writeRec(3, "2850123456/duplicate_id_another_building");
    writeRec(4, "3823543010/republicmp_self_id_must_be_ignored");
    writeRec(5, "1911223344\\backslash_vehicle_item");

    uint64_t extracted[16] = {};
    int extractedCount = 0;
    ModSync_ExtractHeaderBinModIdsFromBuffer(hdrBin.data(), hdrBin.size(), extracted, 16, &extractedCount);

    TEST_CHECK(extractedCount == 3);
    TEST_CHECK(extracted[0] == 2850123456ULL);
    TEST_CHECK(extracted[1] == 2999888777ULL);
    TEST_CHECK(extracted[2] == 1911223344ULL);

    // Truncated buffer smaller than 0x260 + 0x80 must extract 0 records safely
    int shortCount = 0;
    ModSync_ExtractHeaderBinModIdsFromBuffer(hdrBin.data(), HEADER_BIN_DATA_OFFSET + 10, extracted, 16, &shortCount);
    TEST_CHECK(shortCount == 0);

    // 2. Test SafeFilterAndSortWorkshopMgr & SafeRestoreWorkshopMgr (Invariant #6)
    auto makeMockWorkshopItem = [](uint64_t id, int itemType, bool isSubscribed) -> std::vector<BYTE> {
        std::vector<BYTE> item(0x4900, 0);
        *(WORD*)(item.data() + ITEM_IS_SUBSCRIBED_OFF) = isSubscribed ? 1 : 0;
        *(int*)(item.data() + ITEM_TYPE_OFF)           = itemType;
        *(uint64_t*)(item.data() + ITEM_ID_QWORD_OFF)  = id;
        _snprintf_s((char*)(item.data() + ITEM_ID_STR_OFF), 0x400, _TRUNCATE, "%llu", (unsigned long long)id);
        _snprintf_s((char*)(item.data() + ITEM_NAME_OFF), 0x400, _TRUNCATE, "Mod_%llu", (unsigned long long)id);
        return item;
    };

    auto itemA = makeMockWorkshopItem(3000000003ULL, 1 /*Building*/, true);
    auto itemB = makeMockWorkshopItem(1000000001ULL, 0 /*Vehicle*/,  true);
    auto itemC = makeMockWorkshopItem(2000000002ULL, 1 /*Building*/, false);
    auto itemD = makeMockWorkshopItem(4000000004ULL, 4 /*Text localization - always kept!*/, true);
    auto itemE = makeMockWorkshopItem(1500000005ULL, 1 /*Building*/, true);

    std::vector<BYTE*> mgrItems = {
        itemA.data(), itemB.data(), itemC.data(), itemD.data(), itemE.data()
    };
    BYTE mgrHeader[24] = {};
    WriteMockVectorHeader(mgrHeader, mgrItems);

    // Allow only 3000000003 and 1000000001 (plus itemD 4000000004 which is itemType == 4 text localization)
    uint64_t allowed[2] = { 3000000003ULL, 1000000001ULL };
    int hiddenTail = 0;
    int kept = ModSync_FilterAndSortWorkshopMgrForTest(mgrHeader, allowed, 2, &hiddenTail);

    TEST_CHECK(kept == 3);
    TEST_CHECK(hiddenTail == 2); // 5 total - 3 kept = 2 preserved in tail

    BYTE** activeBegin = *(BYTE***)(mgrHeader + 0x00);
    BYTE** activeEnd   = *(BYTE***)(mgrHeader + 0x08);
    TEST_CHECK((activeEnd - activeBegin) == 3);

    // Verify ascending ID sort order in [0 .. kept - 1]: 1000000001, 3000000003, 4000000004
    TEST_CHECK(activeBegin[0] == itemB.data());
    TEST_CHECK(activeBegin[1] == itemA.data());
    TEST_CHECK(activeBegin[2] == itemD.data());

    // Verify excluded items are preserved in the tail [kept .. total) so nothing is leaked
    TEST_CHECK((activeBegin[3] == itemC.data() && activeBegin[4] == itemE.data()) ||
               (activeBegin[3] == itemE.data() && activeBegin[4] == itemC.data()));

    // Restore manager vector and verify all 5 items are back in the active vector
    ModSync_RestoreWorkshopMgrForTest(mgrHeader, &hiddenTail);
    activeEnd = *(BYTE***)(mgrHeader + 0x08);
    TEST_CHECK(hiddenTail == 0);
    TEST_CHECK((activeEnd - activeBegin) == 5);

    // 3. Test Layer 2 Mod Checks (ModSync_IsModIdAllowedInSession, ModSync_CheckBuildingTypeAllowed, ModSync_CheckVehicleTypeAllowed)
    // When filter is inactive, all are allowed
    ModSync_SetTestSessionFilter(false, nullptr, 0);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(9999999999ULL) == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("9999999999/some_building") == true);

    // Activate session filter with allowed IDs { 1000000001ULL, 3000000003ULL }
    ModSync_SetTestSessionFilter(true, allowed, 2);

    TEST_CHECK(ModSync_IsModIdAllowedInSession(1000000001ULL) == true);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(3000000003ULL) == true);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(2000000002ULL) == false);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(3823543010ULL) == true); // RepublicMP mod itself is always allowed
    TEST_CHECK(ModSync_IsModIdAllowedInSession(0) == true);

    // Building type strings
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed(nullptr) == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("media_soviet/buildings/hospital") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("dlc1_school") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("3000000003/large_school") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("MIRRORZ_1000000001/bus_station") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("1000000001\\truck_depot") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("9999999999/super_plant") == false);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("MIRRORZ_8888888888/factory") == false);

    // Vehicle type pointers with name at offset +0x200
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(nullptr) == true);

    std::vector<BYTE> vehVanilla(0x250, 0);
    strncpy_s((char*)(vehVanilla.data() + 0x200), 64, "media_soviet/vehicles/bus_vanilla", _TRUNCATE);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehVanilla.data()) == true);

    std::vector<BYTE> vehAllowed(0x250, 0);
    strncpy_s((char*)(vehAllowed.data() + 0x200), 64, "1000000001/trolley_ziu9", _TRUNCATE);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehAllowed.data()) == true);

    std::vector<BYTE> vehUnallowed(0x250, 0);
    strncpy_s((char*)(vehUnallowed.data() + 0x200), 64, "7777777777/electric_locomotive", _TRUNCATE);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehUnallowed.data()) == false);

    // Restore / deactivate filter
    ModSync_SetTestSessionFilter(false, nullptr, 0);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("9999999999/super_plant") == true);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehUnallowed.data()) == true);

    return true;
}

// ---------------------------------------------------------------------------
// Test 7: BuildingSwitches Exclusions (Invariant #12), Lockstep Commands & SaveZip
// ---------------------------------------------------------------------------

struct GuardModifyContext {
    BYTE* loco;
    BYTE* wagon2;
    BYTE* wheelBuf;
    float interpSteerDuringGuard;
    float interpWheel0DuringGuard;
    float interpWheel1DuringGuard;
};

static void GuardModifyCallback(void* ud)
{
    auto* ctx = (GuardModifyContext*)ud;
    // Simulate engine render 0x6712B0 updating dynamic matrices in 0x1480..0x16F0
    // to values that look like heap pointers
    *(uintptr_t*)(ctx->loco + 0x1480) = (uintptr_t)ctx->wagon2;
    ctx->interpSteerDuringGuard = *(const float*)(ctx->loco + VEH_STEER_CURRENT_OFFSET);
    if (ctx->wheelBuf) {
        ctx->interpWheel0DuringGuard = *(const float*)(ctx->wheelBuf + 0 * VEH_WHEEL_STRUCT_STRIDE);
        ctx->interpWheel1DuringGuard = *(const float*)(ctx->wheelBuf + 1 * VEH_WHEEL_STRUCT_STRIDE);
    }
}

static bool SubTest_HeldFrameVehicleGraphAndSmoothOffset()
{
    HeldFrame_Reset();

    // 0. Shortest-arc angle interpolation across +/-pi
    constexpr float kPi = 3.14159265358979323846f;
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(0.0f, 1.0f, 0.25f), 0.25f, 1e-5f);
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(0.0f, 1.0f, -0.5f), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(0.0f, 1.0f, 1.5f), 1.0f, 1e-5f);
    // Wrap across +pi -> -pi (e.g. +3.04 rad -> -3.04 rad has shortest delta = +0.203 rad)
    float wrapPosToNeg = HeldFrame_LerpAngleRadians(kPi - 0.1f, -kPi + 0.1f, 0.5f);
    TEST_CHECK_NEAR(fabsf(wrapPosToNeg), kPi, 1e-4f);
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(NAN, 0.5f, 0.5f), 0.5f, 1e-5f);

    // 1. Setup 3 mock vehicles (size 0x1AD8): loco, wagon1, wagon2
    std::vector<BYTE> loco(0x1AD8, 0);
    std::vector<BYTE> wagon1(0x1AD8, 0);
    std::vector<BYTE> wagon2(0x1AD8, 0);
    // Setup 2 wheels (24 bytes each) for loco at +0x1438 and steering at +0x115C/+0x1160
    std::vector<BYTE> locoWheels(2 * VEH_WHEEL_STRUCT_STRIDE, 0);
    *(BYTE**)(loco.data() + VEH_WHEELS_BEGIN_OFFSET) = locoWheels.data();
    *(BYTE**)(loco.data() + VEH_WHEELS_END_OFFSET)   = locoWheels.data() + locoWheels.size();
    *(float*)(locoWheels.data() + 0 * VEH_WHEEL_STRUCT_STRIDE) = 1.0f;
    *(float*)(locoWheels.data() + 1 * VEH_WHEEL_STRUCT_STRIDE) = kPi - 0.1f;
    *(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET) = 0.10f;
    *(float*)(loco.data() + VEH_STEER_TARGET_OFFSET)  = 0.50f;

    // Setup follow camera manager at world+0x18 pointing to wagon2 (with wagon2 hidden externally)
    std::vector<BYTE> mockCamMgr(CAM_MGR_STRUCT_BYTES, 0);
    std::vector<BYTE> mockFollowWin(0x80, 0);
    mockCamMgr[CAM_MGR_FOLLOW_ACTIVE_OFF] = 1;
    *(BYTE**)(mockCamMgr.data() + CAM_MGR_FOLLOW_WIN_PTR_OFF) = mockFollowWin.data();
    *(BYTE**)(mockFollowWin.data() + FOLLOW_WIN_VEHICLE_PTR_OFF) = wagon2.data();
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF) = mockCamMgr.data();

    *(unsigned*)(loco.data() + 0x04)   = 1001;
    *(unsigned*)(wagon1.data() + 0x04) = 1002;
    *(unsigned*)(wagon2.data() + 0x04) = 1003;

    *(int32_t*)(loco.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET)   = -1;
    *(int32_t*)(wagon1.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
    *(int32_t*)(wagon2.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;

    loco[VEH_RENDER_VISIBLE_OFFSET]   = 1;
    wagon1[VEH_RENDER_VISIBLE_OFFSET] = 1;
    wagon2[VEH_RENDER_VISIBLE_OFFSET] = 1;

    loco[VEH_WORKER_SIM_FLAG_OFFSET]   = 1;
    wagon1[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
    wagon2[VEH_WORKER_SIM_FLAG_OFFSET] = 1;

    // Critical root cause reproduction: +0x1A68 is a 32-bit integer counter, NOT a pointer!
    *(uint32_t*)(loco.data() + 0x1A68)   = 0x42;
    *(uint32_t*)(wagon1.data() + 0x1A68) = 0x42;
    *(uint32_t*)(wagon2.data() + 0x1A68) = 0x42;

    // Pre-seed 0x1480 with a pointer-like value to test dynamic node restore
    *(uintptr_t*)(loco.data() + 0x1480) = (uintptr_t)wagon1.data();

    // Graph links:
    // loco -> link (+0x4A0) points to wagon1
    *(BYTE**)(loco.data() + VEH_LINK_PTR_OFFSET) = wagon1.data();
    // wagon1 -> parent (+0x398) points to loco
    *(BYTE**)(wagon1.data() + VEH_PARENT_PTR_OFFSET) = loco.data();

    // wagon1 -> children vector (+0x3A0..+0x3A8) points to wagon2
    std::vector<BYTE*> wagon1Children = { wagon2.data() };
    *(BYTE***)(wagon1.data() + VEH_CHILDREN_BEGIN_OFFSET) = wagon1Children.data();
    *(BYTE***)(wagon1.data() + VEH_CHILDREN_END_OFFSET)   = wagon1Children.data() + wagon1Children.size();
    // wagon2 -> parent (+0x398) points to wagon1
    *(BYTE**)(wagon2.data() + VEH_PARENT_PTR_OFFSET) = wagon1.data();

    // Node setup for 5 C3D_NODES at 0xD60, 0xE1C, 0xED8, 0xF94, 0x1080
    const unsigned nodeOffsets[5] = { 0xD60, 0xE1C, 0xED8, 0xF94, 0x1080 };
    for (int k = 0; k < 5; ++k) {
        float* nLoco   = (float*)(loco.data() + nodeOffsets[k]);
        float* nWagon1 = (float*)(wagon1.data() + nodeOffsets[k]);
        float* nWagon2 = (float*)(wagon2.data() + nodeOffsets[k]);
        nLoco[0]  = nWagon1[0]  = nWagon2[0]  = 1.0f;
        nLoco[5]  = nWagon1[5]  = nWagon2[5]  = 1.0f;
        nLoco[10] = nWagon1[10] = nWagon2[10] = 1.0f;
        nLoco[15] = nWagon1[15] = nWagon2[15] = 1.0f;
        nLoco[32]   = 100.0f + (float)k;
        nWagon1[32] = 80.0f + (float)k;
        nWagon2[32] = 60.0f + (float)k;
        nLoco[33] = nWagon1[33] = nWagon2[33] = 10.0f;
        nLoco[34] = nWagon1[34] = nWagon2[34] = 200.0f;
        nLoco[38] = nWagon1[38] = nWagon2[38] = 1.0f; // scale.x
        nLoco[39] = nWagon1[39] = nWagon2[39] = 1.0f; // scale.y
        nLoco[40] = nWagon1[40] = nWagon2[40] = 1.0f; // scale.z
        nLoco[43] = nWagon1[43] = nWagon2[43] = 1.0f; // ori.z (+0xA4)
        nLoco[45] = nWagon1[45] = nWagon2[45] = 1.0f; // up.y  (+0xB0)
    }

    // Set up engine vehicle render vector at 0x9E7720 with only the lead loco
    std::vector<BYTE*> engineVehList = { loco.data() };
    *(BYTE***)(g_mockExeBuffer + 0x9E7720) = engineVehList.data();
    *(BYTE***)(g_mockExeBuffer + 0x9E7728) = engineVehList.data() + engineVehList.size();

    // 2. Tick 1: HeldFrame_OnSimTick traverses graph without failing on +0x1A68
    HeldFrame_OnSimTick(1);
    HeldFrameDiagStats stats = {};
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.trackedVehicles >= 3);

    // 3. Move vehicles slightly on Tick 2 (dx = 2.0f, distSq = 4.0f < 400.0f) and advance wheels/steering
    for (int k = 0; k < 5; ++k) {
        float* nLoco   = (float*)(loco.data() + nodeOffsets[k]);
        float* nWagon1 = (float*)(wagon1.data() + nodeOffsets[k]);
        float* nWagon2 = (float*)(wagon2.data() + nodeOffsets[k]);
        nLoco[32]   += 2.0f;
        nWagon1[32] += 2.0f;
        nWagon2[32] += 2.0f;
    }
    *(float*)(locoWheels.data() + 0 * VEH_WHEEL_STRUCT_STRIDE) = 2.0f;
    *(float*)(locoWheels.data() + 1 * VEH_WHEEL_STRUCT_STRIDE) = -kPi + 0.1f; // wrapped across +pi/-pi
    // Leave VEH_STEER_CURRENT_OFFSET at 0.10f while target is 0.50f so AdvanceVehicleSteerAndWheels steps by +0.05f -> 0.15f
    HeldFrame_OnSimTick(2);
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.trackedVehicles >= 3);
    TEST_CHECK_NEAR(*(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET), 0.15f, 1e-5f);

    // 4. Test smooth offset rejection cap
    // a) magSq <= 2500.0f accepted (30^2 + 30^2 = 1800.0f)
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 30.0f, 0.0f, 30.0f);
    // b) magSq == 2500.0f exact boundary accepted (50^2 = 2500.0f)
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 50.0f, 0.0f, 0.0f);
    // c) magSq > 2500.0f rejected (50^2 + 10^2 = 2600.0f)
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 50.0f, 0.0f, 10.0f);
    // d) Non-finite inputs safely rejected
    HeldFrame_AddVehicleSmoothOffset(loco.data(), NAN, 0.0f, 0.0f);
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 0.0f, INFINITY, 0.0f);

    // 5. Test VehicleGuard Begin/End lifecycle with sub-tick interpolation & dynamic node restore
    GuardModifyContext ctx = { loco.data(), wagon2.data(), locoWheels.data(), 0.0f, 0.0f, 0.0f };
    TEST_CHECK(HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0x9E7720, 0.5f, GuardModifyCallback, &ctx));
    // Verify wheel pitch & steering were interpolated at alpha=0.5 during guard
    TEST_CHECK_NEAR(ctx.interpSteerDuringGuard, 0.125f, 1e-4f);
    TEST_CHECK_NEAR(ctx.interpWheel0DuringGuard, 1.5f, 1e-4f);
    TEST_CHECK_NEAR(fabsf(ctx.interpWheel1DuringGuard), kPi, 1e-4f);
    // Verify wheel pitch & steering were restored to Tick 2 values after guard End()
    TEST_CHECK_NEAR(*(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET), 0.15f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(loco.data() + VEH_STEER_TARGET_OFFSET), 0.50f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(locoWheels.data() + 0 * VEH_WHEEL_STRUCT_STRIDE), 2.0f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(locoWheels.data() + 1 * VEH_WHEEL_STRUCT_STRIDE), -kPi + 0.1f, 1e-5f);
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF) = nullptr;

    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.guardFallbacks == 0);
    TEST_CHECK(stats.lastTargetsCount >= 3);
    TEST_CHECK(stats.lastInterpolatedCount >= 1);
    TEST_CHECK(stats.restoreRefusals == 0);

    // Verify worker sim flag 0xD41 restored to 1 on all vehicles
    TEST_CHECK(loco[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
    TEST_CHECK(wagon1[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
    TEST_CHECK(wagon2[VEH_WORKER_SIM_FLAG_OFFSET] == 1);

    // Verify dynamic sub-node matrix at +0x1480 restored to original value
    TEST_CHECK(*(uintptr_t*)(loco.data() + 0x1480) == (uintptr_t)wagon1.data());

    // 6. Test HeldFrame_SubmitVehicleHighlight: highlighting a coupled wagon (wagon2) interpolates
    //    the entire train subgraph (loco, wagon1, wagon2) during the highlight call and restores after.
    struct HighlightTestCtx {
        BYTE* loco;
        BYTE* wagon2;
        float locoXDuring;
        float wagon2XDuring;
        int calls;
    } hlCtx = { loco.data(), wagon2.data(), 0.0f, 0.0f, 0 };
    auto HlCallback = [](BYTE* /*veh*/, bool /*isExtraPass*/, void* ud) {
        auto* c = (HighlightTestCtx*)ud;
        c->locoXDuring   = *(const float*)(c->loco + 0xD60 + 0x80);
        c->wagon2XDuring = *(const float*)(c->wagon2 + 0xD60 + 0x80);
        c->calls++;
    };
    HeldFrame_SetHighlightCaptureForTest(HlCallback, &hlCtx);
    const float hlColor[4] = { 1.0f, 0.8f, 0.2f, 1.0f };
    HeldFrame_SubmitVehicleHighlight(wagon2.data(), 0, 1, hlColor, 0, false);
    HeldFrame_SubmitVehicleHighlight(wagon2.data(), 0, 1, hlColor, 0, true);
    HeldFrame_SetHighlightCaptureForTest(nullptr, nullptr);
    TEST_CHECK(hlCtx.calls == 2);
    // wagon2 had prevX=60, currX=62, smoothOff=0 -> at alpha=0.5f, interpolated X is 61.0f during highlight
    TEST_CHECK_NEAR(hlCtx.wagon2XDuring, 61.0f, 1e-4f);
    // And after HeldFrame_SubmitVehicleHighlight returns, both loco and wagon2 are restored to Tick 2 currX (102.0f and 62.0f)
    TEST_CHECK_NEAR(*(const float*)(loco.data() + 0xD60 + 0x80), 102.0f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(wagon2.data() + 0xD60 + 0x80), 62.0f, 1e-4f);

    *(BYTE***)(g_mockExeBuffer + 0x9E7720) = nullptr;
    *(BYTE***)(g_mockExeBuffer + 0x9E7728) = nullptr;
    HeldFrame_Reset();

    return true;
}

static bool SubTest_HeldFrameQueueBoundAndCorruptedLinks()
{
    HeldFrame_Reset();

    // Setup lead loco
    std::vector<BYTE> leadLoco(0x1AD8, 0);
    *(unsigned*)(leadLoco.data() + 0x04) = 2001;
    *(int32_t*)(leadLoco.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
    leadLoco[VEH_RENDER_VISIBLE_OFFSET] = 1;
    leadLoco[VEH_WORKER_SIM_FLAG_OFFSET] = 1;

    // Create a pool of 2100 mock vehicles coupled in a chain via VEH_LINK_PTR_OFFSET
    const size_t kPoolSize = 2100;
    std::vector<std::vector<BYTE>> pool(kPoolSize, std::vector<BYTE>(0x1AD8, 0));

    for (size_t i = 0; i < kPoolSize; ++i) {
        *(unsigned*)(pool[i].data() + 0x04) = (unsigned)(3000 + i);
        *(int32_t*)(pool[i].data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        pool[i][VEH_RENDER_VISIBLE_OFFSET] = 1;
        pool[i][VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        if (i + 1 < kPoolSize) {
            *(BYTE**)(pool[i].data() + VEH_LINK_PTR_OFFSET) = pool[i + 1].data();
        }
    }

    // Lead loco links to the first wagon in the chain
    *(BYTE**)(leadLoco.data() + VEH_LINK_PTR_OFFSET) = pool[0].data();

    // Inject unmapped and malformed pointers into leadLoco children to verify traversal resilience
    std::vector<BYTE*> childPtrs = { (BYTE*)0x0000000400000000ull, (BYTE*)0x1234 };
    *(BYTE***)(leadLoco.data() + VEH_CHILDREN_BEGIN_OFFSET) = childPtrs.data();
    *(BYTE***)(leadLoco.data() + VEH_CHILDREN_END_OFFSET)   = childPtrs.data() + childPtrs.size();

    // Set engine vehicle list at 0x9E7720
    std::vector<BYTE*> engineVehList = { leadLoco.data() };
    *(BYTE***)(g_mockExeBuffer + 0x9E7720) = engineVehList.data();
    *(BYTE***)(g_mockExeBuffer + 0x9E7728) = engineVehList.data() + engineVehList.size();

    // OnSimTick: Queue must process all up to 2048 without aborting early
    HeldFrame_OnSimTick(10);
    HeldFrameDiagStats stats = {};
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.trackedVehicles >= 2048);

    // VehicleGuard Begin/End: Must discard invalid pointers without guard fallback
    TEST_CHECK(HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0x9E7720, 0.0f));
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.guardFallbacks == 0);

    // 3. Test SafeRestoreWorkerFlags & End() restoration resilience when intermediate vehicle memory becomes unmapped
    std::vector<BYTE> vehA(0x1AD8, 0);
    std::vector<BYTE> vehC(0x1AD8, 0);
    BYTE* vehPage = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    TEST_CHECK(vehPage != nullptr);
    if (vehPage) {
        *(unsigned*)(vehA.data() + 0x04) = 5001;
        *(unsigned*)(vehPage + 0x04) = 5002;
        *(unsigned*)(vehC.data() + 0x04) = 5003;

        *(int32_t*)(vehA.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        *(int32_t*)(vehPage + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        *(int32_t*)(vehC.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;

        vehA[VEH_RENDER_VISIBLE_OFFSET] = 1;
        vehPage[VEH_RENDER_VISIBLE_OFFSET] = 1;
        vehC[VEH_RENDER_VISIBLE_OFFSET] = 1;

        vehA[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        vehPage[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        vehC[VEH_WORKER_SIM_FLAG_OFFSET] = 1;

        std::vector<BYTE*> testVehList = { vehA.data(), vehPage, vehC.data() };
        *(BYTE***)(g_mockExeBuffer + 0x9E7720) = testVehList.data();
        *(BYTE***)(g_mockExeBuffer + 0x9E7728) = testVehList.data() + testVehList.size();

        struct UnmapContext {
            BYTE* page;
            DWORD oldProtect;
        } unmapCtx = { vehPage, 0 };

        auto UnmapCallback = [](void* ud) {
            auto* ctx = (UnmapContext*)ud;
            VirtualProtect(ctx->page, 0x2000, PAGE_NOACCESS, &ctx->oldProtect);
        };

        // Guard Begin succeeds on all 3; in callback vehPage is unmapped; End must safely restore vehA and vehC
        HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0x9E7720, 0.0f, UnmapCallback, &unmapCtx);

        VirtualProtect(vehPage, 0x2000, unmapCtx.oldProtect, &unmapCtx.oldProtect);
        TEST_CHECK(vehA[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
        TEST_CHECK(vehC[VEH_WORKER_SIM_FLAG_OFFSET] == 1);

        VirtualFree(vehPage, 0, MEM_RELEASE);
    }

    // Clean up mock engine vehicle list & reset HeldFrame state
    *(BYTE***)(g_mockExeBuffer + 0x9E7720) = nullptr;
    *(BYTE***)(g_mockExeBuffer + 0x9E7728) = nullptr;
    HeldFrame_Reset();

    return true;
}

static bool Test_BuildingSwitches_Commands_SaveZip()
{
    ResetMockWorldMemory();

    // 1. Invariant #12: BuildingSwitches_IsExcludedBuildingOffset must exclude std::vector headers in BUILDING
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBC0)); // Citizen* vector begin
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBC8)); // Citizen* vector end
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBD0)); // Citizen* vector cap
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBD8)); // Inmate* vector begin
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0x970)); // Storage slots vector
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0x9F8)); // Post-0x9E8 pointer block
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xF18)); // Citizen destinations vector

    // Legitimate building settings offsets must NOT be excluded
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(0x114)); // Building custom name
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(0x500)); // Setting word
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(0x9E8)); // Water reservoir / heating plant setting word

    // 1b. RoadSync_ClearCategoryPreview & RoadSync_CancelLocalDrag state cleanup
    {
        BYTE* world = g_mockExeBuffer + WORLD_RVA;
        std::vector<BYTE> pedMgr(0x1000, 0);
        std::vector<BYTE> secRoadMgr(0x1000, 0);
        *(BYTE**)(world + 0xF968) = pedMgr.data();     // PEDESTRIAN manager
        *(BYTE**)(world + 0xF7A8) = secRoadMgr.data(); // SECOND_ROAD manager
        *(unsigned long long*)(pedMgr.data() + 0x360) = 0x112233445566ULL;
        *(unsigned char*)(pedMgr.data() + 0x390) = 1;
        *(unsigned char*)(pedMgr.data() + 0x748) = 1;
        *(unsigned char*)(pedMgr.data() + 0x749) = 1;
        *(unsigned long long*)(g_mockExeBuffer + 0x9E4590) = 0xAABBCCDDEEFFULL; // HOVER_ROAD_EDGE_RVA
        *(unsigned long long*)(g_mockExeBuffer + 0x9E4560) = 0x123456789ABCULL; // HOVER_NODE_RVA (must stay for quick-click cancel Inv #13)
        RoadSync_ClearCategoryPreview(ROAD_SYNC_CATEGORY_PEDESTRIAN);
        TEST_CHECK(*(unsigned long long*)(pedMgr.data() + 0x360) == 0);
        TEST_CHECK(*(unsigned char*)(pedMgr.data() + 0x390) == 0);
        TEST_CHECK(*(unsigned char*)(pedMgr.data() + 0x748) == 0);
        TEST_CHECK(*(unsigned char*)(pedMgr.data() + 0x749) == 0);
        TEST_CHECK(*(unsigned long long*)(g_mockExeBuffer + 0x9E4590) == 0);
        TEST_CHECK(*(unsigned long long*)(g_mockExeBuffer + 0x9E4560) == 0x123456789ABCULL);

        // Verify RoadSync_CancelLocalDrag clears SECOND_ROAD square preview vector (world+0xF608..0xF610)
        std::vector<unsigned long long> sqSegs = { 0x1001ULL, 0x1002ULL };
        *(unsigned long long**)(world + 0xF608) = sqSegs.data();
        *(unsigned long long**)(world + 0xF610) = sqSegs.data() + sqSegs.size();
        *(unsigned char*)(world + 0xD2AC) = 1;
        RoadSync_CancelLocalDrag();
        TEST_CHECK(*(unsigned char*)(world + 0xD2AC) == 0);
        TEST_CHECK(*(unsigned long long**)(world + 0xF610) == *(unsigned long long**)(world + 0xF608));
    }

    // 2. Lockstep Commands queue & tick boundary execution
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    Commands_SetSession(COMMANDS_HOST);
    TEST_CHECK(Commands_Capturing());

    unsigned long long epochBefore = Commands_ApplyEpoch();
    LsStateTransfer cmdTr = { 1, 2, 25000.0, 10000.0 };
    Commands_Submit(LS_CMD_STATE_TRANSFER, &cmdTr, sizeof(cmdTr));
    TEST_CHECK(Commands_Due(1));
    Commands_Boundary(1);

    TEST_CHECK(Commands_ApplyEpoch() > epochBefore);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 475000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 525000.0, 1e-3);

    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();

    // 2b. Cheat / dev mode sanitization (C+H+E / D+E+V / cheat systab pages 5..7)
    {
        BYTE* world = g_mockExeBuffer + WORLD_RVA;
        world[WORLD_CHEAT_MODE_OFFSET] = 1;
        world[WORLD_DEV_MODE_OFFSET]   = 1;
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 6; // "cheatgeneral"
        TEST_CHECK(Commands_SanitizeCheatState(world));
        TEST_CHECK(world[WORLD_CHEAT_MODE_OFFSET] == 0);
        TEST_CHECK(world[WORLD_DEV_MODE_OFFSET] == 0);
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == -1);
        // Idempotent when already clean, and preserves normal sub-pages (0..4)
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 2;
        TEST_CHECK(!Commands_SanitizeCheatState(world));
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == 2);
    }

    // 2c. Rail/road depot row self-drop uncoupling guard (0x7C7E87 / 0x7C7ED8)
    {
        std::vector<BYTE> locoA(0x600, 0), wagonA1(0x600, 0), locoB(0x600, 0);
        unsigned long long ptrLocoA   = (unsigned long long)locoA.data();
        unsigned long long ptrWagonA1 = (unsigned long long)wagonA1.data();
        unsigned long long ptrLocoB   = (unsigned long long)locoB.data();
        *(unsigned long long*)(wagonA1.data() + VEH_PARENT_PTR_OFFSET) = ptrLocoA; // wagonA1 attached to locoA

        // Clicking/double-clicking locoA or its attached wagonA1 is a self-drop -> must be blocked
        TEST_CHECK(GameCalls_IsDepotRowSelfDrop(ptrLocoA, ptrLocoA, ptrLocoA, ptrLocoA));
        TEST_CHECK(GameCalls_IsDepotRowSelfDrop(ptrLocoA, ptrWagonA1, ptrWagonA1, ptrLocoA));
        TEST_CHECK(GameCalls_IsDepotRowSelfDrop(ptrWagonA1, ptrWagonA1, ptrWagonA1, ptrLocoA));
        // Dragging independent locoB onto locoA's row is a valid couple -> must be allowed
        TEST_CHECK(!GameCalls_IsDepotRowSelfDrop(ptrLocoB, ptrLocoA, ptrLocoA, ptrLocoA));
        TEST_CHECK(!GameCalls_IsDepotRowSelfDrop(ptrLocoB, ptrWagonA1, ptrWagonA1, ptrLocoA));
    }

    // 2d. Sub-tick C3D_NODE position + 3x3 rotation matrix interpolation & Gram-Schmidt orthonormalization
    //     Verifies the true 47-float (0xBC-byte) C3D_NODE layout:
    //       +0x80 (float[32..34]): Position
    //       +0x8C (float[35..37]): Rotation (Euler angles, preserved)
    //       +0x98 (float[38..40]): Scale ((1,1,1), preserved and never normalized to 0.577)
    //       +0xA4 (float[41..43]): Orientation (interpolated & normalized)
    //       +0xB0 (float[44..46]): UpVector (interpolated & normalized)
    {
        float prevNode[47] = {};
        float currNode[47] = {};
        // prevNode: identity rotation at (10, 0, 20)
        prevNode[0] = 1.0f; prevNode[5] = 1.0f; prevNode[10] = 1.0f; prevNode[15] = 1.0f;
        prevNode[16] = 1.0f; prevNode[21] = 1.0f; prevNode[26] = 1.0f; prevNode[31] = 1.0f;
        prevNode[12] = prevNode[32] = 10.0f;
        prevNode[13] = prevNode[33] = 0.0f;
        prevNode[14] = prevNode[34] = 20.0f;
        prevNode[38] = prevNode[39] = prevNode[40] = 1.0f; // scale = (1,1,1)
        prevNode[43] = 1.0f; // ori = (0,0,1) at +0xA4
        prevNode[45] = 1.0f; // up  = (0,1,0) at +0xB0

        // currNode: 45-degree yaw around Y at (20, 0, 30)
        const float c45 = 0.70710678f, s45 = 0.70710678f;
        currNode[0] =  c45; currNode[2]  = -s45;
        currNode[5] = 1.0f;
        currNode[8] =  s45; currNode[10] =  c45;
        currNode[15] = 1.0f;
        currNode[16] =  c45; currNode[24] = -s45;
        currNode[21] = 1.0f;
        currNode[18] =  s45; currNode[26] =  c45;
        currNode[31] = 1.0f;
        currNode[12] = currNode[32] = 20.0f;
        currNode[13] = currNode[33] = 0.0f;
        currNode[14] = currNode[34] = 30.0f;
        currNode[36] = 0.78539816f; // Euler yaw at +0x8C
        currNode[38] = currNode[39] = currNode[40] = 1.0f; // scale = (1,1,1) at +0x98
        currNode[41] = s45; currNode[43] = c45; // ori at +0xA4
        currNode[45] = 1.0f; // up at +0xB0

        alignas(16) BYTE nodeBuf[C3D_NODE_BYTE_SIZE] = {};
        memcpy(nodeBuf, currNode, sizeof(currNode));
        const float smoothOff[3] = { 1.0f, 0.0f, -1.0f };
        HeldFrame_InterpolateC3DNode(nodeBuf, prevNode, currNode, 0.5f, smoothOff);

        const float* m     = (const float*)nodeBuf;
        const float* minv  = (const float*)(nodeBuf + 0x40);
        const float* pos   = (const float*)(nodeBuf + 0x80);
        const float* rot   = (const float*)(nodeBuf + 0x8C);
        const float* scale = (const float*)(nodeBuf + 0x98);
        const float* ori   = (const float*)(nodeBuf + 0xA4);
        const float* up    = (const float*)(nodeBuf + 0xB0);
        TEST_CHECK_NEAR(pos[0], 16.0f, 1e-4f);
        TEST_CHECK_NEAR(pos[2], 24.0f, 1e-4f);
        TEST_CHECK_NEAR(m[12],  16.0f, 1e-4f);
        TEST_CHECK_NEAR(m[14],  24.0f, 1e-4f);
        // Scale (+0x98) and Euler rotation (+0x8C) must be preserved intact
        TEST_CHECK_NEAR(rot[1],   0.78539816f, 1e-5f);
        TEST_CHECK_NEAR(scale[0], 1.0f, 1e-5f);
        TEST_CHECK_NEAR(scale[1], 1.0f, 1e-5f);
        TEST_CHECK_NEAR(scale[2], 1.0f, 1e-5f);
        // Halfway between 0 deg and 45 deg is 22.5 deg (sin=0.382683, cos=0.923880)
        TEST_CHECK_NEAR(m[8],   0.382683f, 1e-3f);
        TEST_CHECK_NEAR(m[10],  0.923880f, 1e-3f);
        TEST_CHECK_NEAR(ori[0], 0.382683f, 1e-3f);
        TEST_CHECK_NEAR(ori[2], 0.923880f, 1e-3f);
        TEST_CHECK_NEAR(up[1],  1.0f,      1e-5f);
        // Orthonormality of m and exactness of m * minv translation
        float dotXZ = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
        TEST_CHECK_NEAR(dotXZ, 0.0f, 1e-5f);
        float tx = m[12] * minv[0] + m[13] * minv[4] + m[14] * minv[8]  + minv[12];
        float tz = m[12] * minv[2] + m[13] * minv[6] + m[14] * minv[10] + minv[14];
        TEST_CHECK_NEAR(tx, 0.0f, 1e-4f);
        TEST_CHECK_NEAR(tz, 0.0f, 1e-4f);

        // Verify alpha = 0.0f places node at prevNode + smoothOff (not currNode)
        memcpy(nodeBuf, currNode, sizeof(currNode));
        HeldFrame_InterpolateC3DNode(nodeBuf, prevNode, currNode, 0.0f, nullptr);
        TEST_CHECK_NEAR(pos[0], 10.0f, 1e-4f);
        TEST_CHECK_NEAR(pos[2], 20.0f, 1e-4f);
        TEST_CHECK_NEAR(ori[0], 0.0f,  1e-4f);
        TEST_CHECK_NEAR(ori[2], 1.0f,  1e-4f);
    }

    // 2e. HeldFrame OnSimTick & VehicleGuard with +0x1A68 integer counters, graph traversal, and soft-sync smoothing cap
    TEST_CHECK(SubTest_HeldFrameVehicleGraphAndSmoothOffset());
    TEST_CHECK(SubTest_HeldFrameQueueBoundAndCorruptedLinks());

    // 3. SaveZip pack & unpack roundtrip
    std::filesystem::path baseDir(g_mockBaseDir);
    std::filesystem::path srcSaveDir = baseDir / "zip_src_save";
    std::filesystem::path dstSaveDir = baseDir / "zip_dst_save";
    std::filesystem::path zipFile    = baseDir / "test_save_pack.zip";
    std::filesystem::create_directories(srcSaveDir);

    std::vector<BYTE> binPayload(4096);
    for (size_t i = 0; i < binPayload.size(); ++i) binPayload[i] = (BYTE)((i * 37 + 13) & 0xFF);
    {
        FILE* f = nullptr;
        TEST_CHECK(fopen_s(&f, (srcSaveDir / "header.bin").string().c_str(), "wb") == 0 && f);
        fwrite(binPayload.data(), 1, binPayload.size(), f);
        fclose(f);
    }

    size_t progressCalls = 0;
    auto onProg = [](size_t, size_t, void* ud) { ++(*(size_t*)ud); };
    TEST_CHECK(ZipSaveDirectory(srcSaveDir.string().c_str(), zipFile.string().c_str(), onProg, &progressCalls));
    TEST_CHECK(progressCalls > 0);
    TEST_CHECK(UnzipToDirectory(zipFile.string().c_str(), dstSaveDir.string().c_str(), onProg, &progressCalls));

    std::vector<BYTE> readBack(binPayload.size(), 0);
    {
        FILE* f = nullptr;
        TEST_CHECK(fopen_s(&f, (dstSaveDir / "header.bin").string().c_str(), "rb") == 0 && f);
        size_t got = fread(readBack.data(), 1, readBack.size(), f);
        fclose(f);
        TEST_CHECK(got == binPayload.size());
    }
    TEST_CHECK(memcmp(binPayload.data(), readBack.data(), binPayload.size()) == 0);

    // 4. InviteCitizensNow (0x6F8CC0 -> 0x823470): verify USD (mode 0), Soviet RUB (mode 2), and Expert RUB (mode 3)
    {
        ResetMockWorldMemory();
        BYTE* world = g_mockExeBuffer + WORLD_RVA;

        static std::vector<std::vector<BYTE>> s_spawnedCitizens;
        static std::vector<BYTE*> s_worldCitVec;
        static int s_mockRandCounter = 0;
        s_spawnedCitizens.clear();
        s_worldCitVec.clear();
        s_mockRandCounter = 0;

        auto mockSpawnCit = +[](unsigned long long w, unsigned long long bld, unsigned char /*isChild*/,
                                unsigned char /*isSoviet*/, unsigned char /*isThirdWorld*/, unsigned char /*unusedZero*/) -> unsigned long long {
            s_spawnedCitizens.emplace_back(0x750, 0);
            BYTE* cit = s_spawnedCitizens.back().data();
            *(unsigned long long*)(cit + CIT_HOME_BUILDING_OFFSET) = bld;
            s_worldCitVec.push_back(cit);
            *(BYTE***)(w + WORLD_CITIZENS_OFFSET)     = s_worldCitVec.data();
            *(BYTE***)(w + WORLD_CITIZENS_OFFSET + 8) = s_worldCitVec.data() + s_worldCitVec.size();
            return (unsigned long long)cit;
        };
        auto mockRand = +[]() -> int { return s_mockRandCounter++; };
        auto mockRandFloat = +[](float minVal, float maxVal) -> float { return 0.5f * (minVal + maxVal); };

        *(decltype(mockSpawnCit)*)(g_mockExeBuffer + RVA_SPAWN_CITIZEN) = mockSpawnCit;
        *(decltype(mockRand)*)(g_mockExeBuffer + RVA_IAT_RAND) = mockRand;
        *(decltype(mockRandFloat)*)(g_mockExeBuffer + RVA_IAT_RAND_FLOAT) = mockRandFloat;
        *(float*)(g_mockExeBuffer + RVA_INVITE_EXPERT_MULT_FLOAT)    = 2.5f;
        *(float*)(g_mockExeBuffer + RVA_INVITE_EXPERT_EDU_MIN_FLOAT) = 2.05f;
        *(float*)(g_mockExeBuffer + RVA_INVITE_EXPERT_EDU_MAX_FLOAT) = 2.30f;

        *(double*)(world + WORLD_USD_OFFSET) = 50000.0;
        *(double*)(world + WORLD_RUB_OFFSET) = 100000.0;
        *(float*)(world + WORLD_PRICE_IMMIGRANT_USD_OFF)      = 100.0f;
        *(float*)(world + WORLD_PRICE_IMMIGRANT_RUB_OFF)      = 200.0f;
        *(float*)(world + WORLD_IMMIGRANT_BASE_SPEND_USD_OFF) = 100.0f;
        *(float*)(world + WORLD_IMMIGRANT_BASE_SPEND_RUB_OFF) = 200.0f;
        *(float*)(world + WORLD_IMMIGRANT_RATE_MULT_USD_OFF)  = 1.5f;
        *(float*)(world + WORLD_IMMIGRANT_RATE_MULT_RUB_OFF)  = 1.2f;

        std::vector<BYTE> resSpec(0x100, 0);
        *(float*)(resSpec.data() + RES_SPEC_MAX_FLATS_FLOAT_OFF) = 12.0f; // 12 flats max
        std::vector<BYTE> bldType(0x400, 0);
        *(BYTE**)(bldType.data() + BLD_TYPE_RES_SPEC_PTR_OFF) = resSpec.data();
        std::vector<BYTE> cityStat(STAT_RECORD_SIZE, 0);
        std::vector<BYTE> bldArea(0x200, 0);
        *(BYTE**)(bldArea.data() + AREA_CITY_STAT_PTR_OFF) = cityStat.data();
        std::vector<BYTE> resBld(0x1420, 0);
        *(BYTE**)(resBld.data() + BLD_TYPE_OFFSET)   = bldType.data();
        *(BYTE**)(resBld.data() + BLD_AREA_PTR_OFF)  = bldArea.data();

        // Mode 3: Invite up to 5 educated Soviet experts (RUB)
        GameCalls_InviteCitizensForTest((unsigned long long)world, (unsigned long long)resBld.data(), 3);
        // 5 experts spawned (price = 200 * 1.2 * 2.5 = 600 RUB each -> 3000 RUB total)
        TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 97000.0, 1e-2);
        TEST_CHECK(*(int*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_COUNTS_OFF + 12) == 5);
        TEST_CHECK(*(int*)(cityStat.data() + STAT_CITIZEN_COUNTS_OFF + 12) == 5);
        TEST_CHECK_NEAR(*(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_IMMIGRANT_SPEND_OFF + 4), 3000.0f, 1e-1f);
        // First spawned adult (index 0) must have education ~2.175f and a linked child (index 1, since rand()==0 % 2 == 0)
        TEST_CHECK(s_worldCitVec.size() >= 2);
        TEST_CHECK_NEAR(*(float*)(s_worldCitVec[0] + CIT_EDUCATION_FLOAT_OFF), 2.175f, 1e-3f);
        TEST_CHECK(*(BYTE**)(s_worldCitVec[0] + CIT_CHILD_PTR_OFF)  == s_worldCitVec[1]);
        TEST_CHECK(*(BYTE**)(s_worldCitVec[1] + CIT_PARENT_PTR_OFF) == s_worldCitVec[0]);

        // Populate building's citizen vector so currentIndependent == 5, leaving 7 free flats out of 12
        *(BYTE***)(resBld.data() + BLD_CITIZENS_BEGIN_OFF) = s_worldCitVec.data();
        *(BYTE***)(resBld.data() + BLD_CITIZENS_END_OFF)   = s_worldCitVec.data() + s_worldCitVec.size();

        // Mode 0: Invite Third World immigrants (USD) -> capped by remaining 7 free flats (price = 100 * 1.5 = 150 USD each -> 1050 USD)
        GameCalls_InviteCitizensForTest((unsigned long long)world, (unsigned long long)resBld.data(), 0);
        TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 48950.0, 1e-2);
        TEST_CHECK(*(int*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_COUNTS_OFF + 16) == 7);
        TEST_CHECK(*(int*)(cityStat.data() + STAT_CITIZEN_COUNTS_OFF + 16) == 7);
        TEST_CHECK_NEAR(*(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_IMMIGRANT_SPEND_OFF), 1050.0f, 1e-1f);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Test 8: Waiting Lobby Hold, Action Lock, Map Inspect & Idempotent Settings
// ---------------------------------------------------------------------------

static bool Test_WaitingLobbyAndActionLock()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Pure state matrix for SaveSync_ShouldBlockWorldEdits(sessionState, inGameplay, syncBlockActive)
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_IDLE, false, false));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_IDLE, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_IDLE, true, true));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_LISTENING, false, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_LISTENING, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_JOINING, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_CLIENT_JOINING, true, false));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_IN_GAME, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_IN_GAME, true, true));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_CLIENT_IN_GAME, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_CLIENT_IN_GAME, true, true));

    // Mark mock world as loaded (WORLD_ROAD_NETWORK_RVA = 0x9E46B0 != 0 and LOAD_TRIGGER_DAT_RVA == 0)
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    TEST_CHECK(SaveSync_IsWorldLoaded());

    // 2. Waiting Lobby & Reconnect state transitions
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TEST_CHECK(!SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(!SaveSync_IsWaitingForReconnect());

    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(!SaveSync_IsWaitingForReconnect());

    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, true, "127.0.0.1:5556");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(SaveSync_IsWaitingForReconnect());
    TEST_CHECK(strcmp(SaveSync_GetLastPartnerPeer(), "127.0.0.1:5556") == 0);

    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    TEST_CHECK(!SaveSync_IsWaitingLobbyActive());

    // 3. SaveSync_EnforceWaitingLobbyHold enforces pause (speed=0, timer+12=1), preserves saved speed (+0x5A8), and clears active tool & drag
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    *(int*)(world + 0x5A4) = 2;                               // GAME_SPEED_OFFSET
    *(int*)(world + 0x5A8) = 0;                               // GAME_SPEED_SAVED_OFFSET
    *(int*)(world + 0x5AC) = 1;                               // GAME_SPEED_TURBO_OFFSET
    *(unsigned char*)(world + 0x5A2) = 1;                     // GAME_SPEED_HOLD_OFFSET
    *(unsigned char*)(g_mockExeBuffer + 0x9D4EE0 + 12) = 0;   // timer paused byte (0 = running)
    *(void**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = (void*)0x12345678ULL;
    *(unsigned char*)(world + 0xD2AC) = 1;                    // TOOL_DRAG_ACTIVE_OFFSET
    *(unsigned char*)(g_mockExeBuffer + 0xA54E90) = 1;        // LMB_HELD_RVA
    *(unsigned char*)(g_mockExeBuffer + 0xA54E91) = 1;        // LMB_JUST_PRESSED_RVA

    SaveSync_EnforceWaitingLobbyHold();

    TEST_CHECK(*(int*)(world + 0x5A4) == 0);
    TEST_CHECK(*(int*)(world + 0x5A8) == 2); // saved speed preserved
    TEST_CHECK(*(int*)(world + 0x5AC) == 0);
    TEST_CHECK(*(unsigned char*)(world + 0x5A2) == 0);
    TEST_CHECK(*(unsigned char*)(g_mockExeBuffer + 0x9D4EE0 + 12) == 1);
    TEST_CHECK(*(void**)(world + WORLD_SELECTED_TOOL_PTR_OFF) == nullptr);
    TEST_CHECK(*(unsigned char*)(world + 0xD2AC) == 0);
    TEST_CHECK(*(unsigned char*)(g_mockExeBuffer + 0xA54E90) == 0);
    TEST_CHECK(*(unsigned char*)(g_mockExeBuffer + 0xA54E91) == 0);

    // 4. Map Inspection Mode ([Tab]) and automatic exit when partner begins joining
    SyncBlock_SetActive(false, nullptr);
    SyncBlock_SetMapInspectMode(false);
    TEST_CHECK(!SyncBlock_IsMapInspectMode());
    TEST_CHECK(SyncBlock_BlocksWorldInput());

    SyncBlock_SetMapInspectMode(true);
    TEST_CHECK(SyncBlock_IsMapInspectMode());
    TEST_CHECK(!SyncBlock_BlocksWorldInput());

    // Partner starts joining -> Map Inspection Mode must automatically exit and block input
    TEST_CHECK(!SyncBlock_UpdateMapInspectState((int)SESSION_HOST_JOINING, true));
    TEST_CHECK(!SyncBlock_IsMapInspectMode());

    // 5. Idempotent Waiting Lobby Settings re-application (never doubles money on repeated toggles)
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    *(int*)(world + WORLD_UNLIMITED_MONEY_OFFSET) = 0;
    *(double*)(world + WORLD_RUB_OFFSET) = 1000000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 400000.0;
    *(unsigned char*)(g_mockExeBuffer + TERRAIN_FLAG_DAT_RVA) = 1; // new map / non-coop save
    TwoStates_OnAnyWorldLoaded(false);
    TEST_CHECK(!TwoStates_IsLoadedFromCoopSave());

    TwoStates_SetMode(COOP_MODE_TWO_STATES);
    TwoStates_SetFullStartingCapital(true);
    TwoStates_ReapplyWaitingLobbySettings();
    TwoStates_ReapplyWaitingLobbySettings();
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 2000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 800000.0, 1e-2);

    // Toggle to 50/50 split -> each gets 500k RUB / 200k USD, world total returns to 1M RUB / 400k USD
    TwoStates_SetFullStartingCapital(false);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 500000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 400000.0, 1e-2);

    // Toggle back to One State -> world total stays 1M RUB / 400k USD
    TwoStates_SetMode(COOP_MODE_ONE_STATE);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 400000.0, 1e-2);

    // Stopping the server in the Waiting Lobby before any partner joins must restore the
    // Singleplayer balance (never leaving it doubled from 100% starting capital):
    TwoStates_SetMode(COOP_MODE_TWO_STATES);
    TwoStates_SetFullStartingCapital(true);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 2000000.0, 1e-2);
    TEST_CHECK(!SaveSync_HadPartnerInSession());
    TwoStates_OnSessionEnded();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 400000.0, 1e-2);

    // Mid-game Singleplayer balance change before opening server: TwoStates_CaptureLobbyBaseMoney()
    *(double*)(world + WORLD_RUB_OFFSET) = 650000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 210000.0;
    TwoStates_CaptureLobbyBaseMoney();
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 650000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 650000.0, 1e-2);
    TwoStates_OnSessionEnded();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 650000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 210000.0, 1e-2);

    // 6. Existing Co-op save protection: TwoStates_OnSessionJoined(true, ...) must preserve
    //    TwoStates_IsLoadedFromCoopSave() and never re-split or double loaded Co-op save balances.
    std::filesystem::path coopSaveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_coop_lobby";
    std::error_code ec;
    std::filesystem::create_directories(coopSaveDir, ec);
    TwoStates_SetMode(COOP_MODE_TWO_STATES);
    double savedRub[2] = { 725000.0, 315000.0 };
    double savedUsd[2] = { 180000.0, 95000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, true, savedRub, savedUsd);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(coopSaveDir.string().c_str()));
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(coopSaveDir.string().c_str(), true));
    TEST_CHECK(TwoStates_IsLoadedFromCoopSave());

    double prepRub[2] = {}, prepUsd[2] = {};
    TwoStates_PrepareHostSave(false, prepRub, prepUsd);
    TwoStates_OnSessionJoined(true, TwoStates_GetMode(), TwoStates_GetBorderHalfWidth(), TwoStates_GetExpandBorders(),
                              prepRub, prepUsd);
    TEST_CHECK(TwoStates_IsLoadedFromCoopSave());
    TwoStates_SetFullStartingCapital(true);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 725000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 315000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 180000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 95000.0, 1e-2);

    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SyncBlock_SetActive(false, nullptr);
    TwoStates_OnSessionEnded();
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Test 9: TwoStates Bank & Loan Subsystem (Ceiling, UI Filter, Lockstep & Daily Step)
// ---------------------------------------------------------------------------

static SovietLoan MakeTestLoan(int32_t loanType, int32_t subType, float principal, float penalty,
                               int32_t daysRem, float ratePct)
{
    SovietLoan ln = {};
    ln.initialPrincipal    = principal;
    ln.remainingPrincipal  = principal;
    ln.loanType            = loanType;
    ln.loanSubType         = subType;
    ln.daysRemaining       = daysRem;
    ln.initialDays         = daysRem;
    ln.unpaidPenaltyBalance = penalty;
    ln.yearInterestRate    = ratePct;
    return ln;
}

static bool TestLoans_MaxAvailableBorrow()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    double initRub[2] = { 1000000.0, 1000000.0 };
    double initUsd[2] = { 500000.0,  500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    // Work resource multipliers at world + 0xC2C8 -> +0x58 (USD = 1.5), +0x5C (RUB = 2.0)
    std::vector<BYTE> workRes(0x80, 0);
    *(float*)(workRes.data() + 0x58) = 1.5f;
    *(float*)(workRes.data() + 0x5C) = 2.0f;
    *(BYTE**)(world + WORLD_WORK_RESOURCE_PTR_OFF) = workRes.data();

    // Create 1 West residential building (X = -1000) and 1 East residential building (X = +1000)
    std::vector<BYTE> bldWest(0x1420, 0), bldEast(0x1420, 0);
    *(int*)bldWest.data() = 10; *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -1000.0f;
    *(int*)bldEast.data() = 20; *(float*)(bldEast.data() + OBJ_POS_OFFSET) =  1000.0f;

    // 10,000 adult housed citizens in West (popRatio = 0.1) and 50,000 in East (popRatio = 0.5),
    // plus 2,000 infants, 2,000 unborn, and 2,000 homeless citizens that must be excluded.
    std::vector<BYTE> citAdultWest(0x800, 0), citAdultEast(0x800, 0);
    std::vector<BYTE> citInfant(0x800, 0), citUnborn(0x800, 0), citHomeless(0x800, 0);
    *(BYTE**)(citAdultWest.data() + CIT_HOME_BUILDING_OFFSET) = bldWest.data();
    *(BYTE**)(citAdultEast.data() + CIT_HOME_BUILDING_OFFSET) = bldEast.data();
    *(BYTE**)(citInfant.data()    + CIT_HOME_BUILDING_OFFSET) = bldWest.data();
    *(int32_t*)(citInfant.data()  + CIT_INFANT_FLAG_OFFSET)   = 1;
    *(BYTE**)(citUnborn.data()    + CIT_HOME_BUILDING_OFFSET) = bldEast.data();
    *(int32_t*)(citUnborn.data()  + CIT_UNBORN_FLAG_OFFSET)   = 1;

    std::vector<BYTE*> citVec;
    citVec.reserve(66000);
    for (int i = 0; i < 10000; ++i) citVec.push_back(citAdultWest.data());
    for (int i = 0; i < 50000; ++i) citVec.push_back(citAdultEast.data());
    for (int i = 0; i < 2000;  ++i) citVec.push_back(citInfant.data());
    for (int i = 0; i < 2000;  ++i) citVec.push_back(citUnborn.data());
    for (int i = 0; i < 2000;  ++i) citVec.push_back(citHomeless.data());
    WriteMockVectorHeader(world + WORLD_CITIZENS_OFFSET, citVec);

    // Existing RUB loans: West has 100k principal + 20k penalty = 120k; East has 300k principal
    std::vector<SovietLoan> loans = {
        MakeTestLoan(LOAN_CURRENCY_RUB, 0, 100000.0f, 20000.0f, 365, 3.5f),
        MakeTestLoan(LOAN_CURRENCY_RUB, 1, 300000.0f,     0.0f, 365, 3.5f),
        MakeTestLoan(LOAN_CURRENCY_USD, 1,  50000.0f,     0.0f, 365, 4.0f),
    };
    loans.reserve(8);
    WriteMockVectorHeader(world + WORLD_LOANS_OFFSET, loans);

    // West RUB maxDebt = (200,000 + 2,000,000 * 0.1) * 2.0 = 800,000; minus 120,000 = 680,000
    TEST_CHECK_NEAR(TwoStates_CalcMaxAvailableBorrow(world, LOAN_CURRENCY_RUB, 1), 680000.0f, 1.0f);
    // East RUB maxDebt = (200,000 + 2,000,000 * 0.5) * 2.0 = 2,400,000; minus 300,000 = 2,100,000
    TEST_CHECK_NEAR(TwoStates_CalcMaxAvailableBorrow(world, LOAN_CURRENCY_RUB, 2), 2100000.0f, 1.0f);
    // East USD maxDebt = (200,000 + 2,000,000 * 0.5) * 1.5 = 1,800,000; minus 50,000 = 1,750,000
    TEST_CHECK_NEAR(TwoStates_CalcMaxAvailableBorrow(world, LOAN_CURRENCY_USD, 2), 1750000.0f, 1.0f);

    double dRub1 = 0.0, dUsd1 = 0.0, dRub2 = 0.0, dUsd2 = 0.0;
    int cnt1 = 0, cnt2 = 0;
    TwoStates_GetLoanDebt(1, &dRub1, &dUsd1, &cnt1);
    TwoStates_GetLoanDebt(2, &dRub2, &dUsd2, &cnt2);
    TEST_CHECK(cnt1 == 1 && cnt2 == 2);
    TEST_CHECK_NEAR(dRub1, 120000.0, 1e-2);
    TEST_CHECK_NEAR(dUsd1, 0.0, 1e-2);
    TEST_CHECK_NEAR(dRub2, 300000.0, 1e-2);
    TEST_CHECK_NEAR(dUsd2, 50000.0, 1e-2);

    TwoStates_OnSessionEnded();
    return true;
}

static bool TestLoans_UiSwapAndSelectionMapping()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    double initRub[2] = { 900000.0, 600000.0 };
    double initUsd[2] = { 400000.0, 250000.0 };
    TwoStates_OnSessionJoined(/*isHost=*/true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    std::vector<SovietLoan> loans = {
        MakeTestLoan(LOAN_CURRENCY_RUB, 0, 110000.0f, 0.0f, 365, 3.0f), // global [0] -> West [0]
        MakeTestLoan(LOAN_CURRENCY_RUB, 1, 220000.0f, 0.0f, 365, 3.5f), // global [1] -> East [0]
        MakeTestLoan(LOAN_CURRENCY_USD, 0, 330000.0f, 0.0f, 730, 4.0f), // global [2] -> West [1]
    };
    loans.reserve(8);
    WriteMockVectorHeader(world + WORLD_LOANS_OFFSET, loans);
    *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = 2; // global index 2 (West's 2nd loan)

    // Host (State 1) enters UI; call TwoStates_UiBegin twice to verify idempotency
    TwoStates_UiBegin();
    TwoStates_UiBegin();

    SovietLoan* uiB = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
    SovietLoan* uiE = *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8);
    TEST_CHECK(uiB != nullptr && (uiE - uiB) == 2);
    TEST_CHECK_NEAR(uiB[0].initialPrincipal, 110000.0f, 1.0f);
    TEST_CHECK_NEAR(uiB[1].initialPrincipal, 330000.0f, 1.0f);
    TEST_CHECK(*(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) == 1); // mapped 2 -> local 1

    // While UI swap is active, TwoStates_GetLoanDebt(2) must still report East's full debt from backup
    double eastRubDuringUi = 0.0;
    int eastCntDuringUi = 0;
    TwoStates_GetLoanDebt(2, &eastRubDuringUi, nullptr, &eastCntDuringUi);
    TEST_CHECK(eastCntDuringUi == 1);
    TEST_CHECK_NEAR(eastRubDuringUi, 220000.0, 1.0);

    // Simulate Host selecting local loan [0] in UI, then end UI
    *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = 0;
    TwoStates_UiEnd();

    uiB = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
    uiE = *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8);
    TEST_CHECK((uiE - uiB) == 3);
    TEST_CHECK(*(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) == 0);

    // Now test as Client (State 2) with global index 1 (East's loan) selected
    TwoStates_OnSessionJoined(/*isHost=*/false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = 1;
    TwoStates_UiBegin();
    uiB = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
    uiE = *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8);
    TEST_CHECK(uiB != nullptr && (uiE - uiB) == 1);
    TEST_CHECK_NEAR(uiB[0].initialPrincipal, 220000.0f, 1.0f);
    TEST_CHECK(*(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) == 0);
    TwoStates_UiEnd();

    uiB = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
    uiE = *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8);
    TEST_CHECK((uiE - uiB) == 3);
    TEST_CHECK(*(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) == 1);

    TwoStates_OnSessionEnded();
    return true;
}

static bool TestLoans_LockstepBorrowAndRepay()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1; // world loaded

    double initRub[2] = { 1000000.0, 500000.0 };
    double initUsd[2] = { 300000.0,  200000.0 };
    TwoStates_OnSessionJoined(/*isHost=*/false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    std::vector<SovietLoan> loans = {
        MakeTestLoan(LOAN_CURRENCY_RUB, 0, 150000.0f, 0.0f, 365, 3.0f), // West's existing loan
    };
    loans.reserve(8);
    WriteMockVectorHeader(world + WORLD_LOANS_OFFSET, loans);

    Commands_SetSession(COMMANDS_HOST);
    Commands_RecordMoney();

    // 1. Waiting Lobby block: borrowing while in SESSION_HOST_LISTENING must be reverted and emit NO command
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());
    TwoStates_UiBegin();
    {
        SovietLoan* b = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
        TEST_CHECK(b != nullptr);
        b[0] = MakeTestLoan(LOAN_CURRENCY_USD, 0, 100000.0f, 0.0f, 365, 4.0f);
        *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b + 1;
        *(double*)(world + WORLD_USD_OFFSET) += 100000.0;
    }
    TwoStates_UiEnd();
    Commands_CheckMoney("test_lobby_loan");
    TEST_CHECK(!Commands_Due(1));
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 200000.0, 1e-2);

    // 2. Active gameplay (SESSION_CLIENT_IN_GAME): State 2 borrows 100,000 USD in UI
    SaveSync_SetTestSessionState(SESSION_CLIENT_IN_GAME, true, "127.0.0.1:5556");
    TEST_CHECK(!SaveSync_IsWaitingLobbyActive());

    TwoStates_UiBegin();
    {
        SovietLoan* b = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
        SovietLoan* e = *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8);
        TEST_CHECK(b != nullptr && (e - b) == 0); // State 2 has 0 loans initially
        b[0] = MakeTestLoan(LOAN_CURRENCY_USD, 0 /*TakeLoan sets 0; hook tags 1*/, 100000.0f, 0.0f, 365, 4.0f);
        *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b + 1;
        *(double*)(world + WORLD_USD_OFFSET) += 100000.0;
        *(float*)(world + WORLD_LOAN_STATS_OFFSET + 0) += 100000.0f;
    }
    TwoStates_UiEnd();
    // Immediately after UiEnd (before tick boundary), world balance is reverted so LS_CMD_MONEY is NOT emitted
    Commands_CheckMoney("test_borrow");
    TEST_CHECK(Commands_Due(1));
    Commands_Boundary(1);

    // State 2's USD must increase by exactly +100,000 (not +200,000 from duplicate LS_CMD_MONEY)
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 300000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 300000.0, 1e-2);
    double eastDebtUsd = 0.0;
    int eastCnt = 0;
    TwoStates_GetLoanDebt(2, nullptr, &eastDebtUsd, &eastCnt);
    TEST_CHECK(eastCnt == 1);
    TEST_CHECK_NEAR(eastDebtUsd, 100000.0, 1.0);

    // 3. Partial repayment: State 2 repays 40,000 USD of its loan
    TwoStates_UiBegin();
    {
        SovietLoan* b = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
        SovietLoan* e = *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8);
        TEST_CHECK(b != nullptr && (e - b) == 1);
        b[0].remainingPrincipal -= 40000.0f;
        b[0].totalPaidSoFar     += 40000.0f;
        *(double*)(world + WORLD_USD_OFFSET) -= 40000.0;
        *(float*)(world + WORLD_LOAN_STATS_OFFSET + 0) -= 40000.0f;
    }
    TwoStates_UiEnd();
    Commands_CheckMoney("test_repay_partial");
    TEST_CHECK(Commands_Due(2));
    Commands_Boundary(2);

    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 300000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 260000.0, 1e-2);
    TwoStates_GetLoanDebt(2, nullptr, &eastDebtUsd, &eastCnt);
    TEST_CHECK(eastCnt == 1);
    TEST_CHECK_NEAR(eastDebtUsd, 60000.0, 1.0);

    // 4. Full repayment: State 2 pays off the remaining 60,000 USD
    TwoStates_UiBegin();
    {
        SovietLoan* b = *(SovietLoan**)(world + WORLD_LOANS_OFFSET);
        TEST_CHECK(b != nullptr);
        *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = 0;
        *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b; // erased
        *(double*)(world + WORLD_USD_OFFSET) -= 60000.0;
        *(float*)(world + WORLD_LOAN_STATS_OFFSET + 0) -= 60000.0f;
    }
    TwoStates_UiEnd();
    Commands_CheckMoney("test_repay_full");
    TEST_CHECK(Commands_Due(3));
    Commands_Boundary(3);

    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 300000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 200000.0, 1e-2);
    TwoStates_GetLoanDebt(2, nullptr, &eastDebtUsd, &eastCnt);
    TEST_CHECK(eastCnt == 0);
    TEST_CHECK_NEAR(eastDebtUsd, 0.0, 1.0);

    // West's original loan must still be intact
    double westDebtRub = 0.0;
    int westCnt = 0;
    TwoStates_GetLoanDebt(1, &westDebtRub, nullptr, &westCnt);
    TEST_CHECK(westCnt == 1);
    TEST_CHECK_NEAR(westDebtRub, 150000.0, 1.0);

    Commands_SetSession(COMMANDS_OFF);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;
    return true;
}

static bool TestLoans_DailyStepAndBankruptcyIsolation()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    double initRub[2] = { 500000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    // Asymmetric building count: 50 buildings in West (State 1) vs 1 building in East (State 2)
    std::vector<std::vector<BYTE>> westBlds(50, std::vector<BYTE>(0x800, 0));
    std::vector<BYTE> eastBld(0x800, 0);
    std::vector<BYTE*> bldVec;
    for (int i = 0; i < 50; ++i) {
        *(int*)westBlds[(size_t)i].data() = 100 + i;
        *(float*)(westBlds[(size_t)i].data() + OBJ_POS_OFFSET) = -1000.0f;
        *(float*)(westBlds[(size_t)i].data() + BLD_PROGRESS_OFFSET) = 1.0f;
        bldVec.push_back(westBlds[(size_t)i].data());
    }
    *(int*)eastBld.data() = 999;
    *(float*)(eastBld.data() + OBJ_POS_OFFSET) = 1000.0f;
    *(float*)(eastBld.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    bldVec.push_back(eastBld.data());
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    // East has 1 RUB loan: 36,500 RUB over 365 days at 0% interest -> 100 RUB/day installment
    SovietLoan eastLoan = MakeTestLoan(LOAN_CURRENCY_RUB, 1 /*East*/, 36500.0f, 0.0f, 365, 0.0f);

    TwoStates_BeforeStep(world);
    TwoStates_StepOneLoanDayForTest(world, &eastLoan);
    TwoStates_AfterStep(world);

    // West MUST pay 0 RUB (despite owning 50/51 buildings); East pays 100% of its 100 RUB installment
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2),  99900.0, 1e-3);
    TEST_CHECK_NEAR(eastLoan.remainingPrincipal, 36400.0f, 1e-2f);
    TEST_CHECK_NEAR(eastLoan.unpaidPenaltyBalance, 0.0f, 1e-3f);

    // Bankruptcy / penalty isolation: East has 0 RUB while West still has 500,000 RUB
    TwoStates_SyncBalancesFromHost(500000.0, 0.0, 100000.0, 100000.0);
    TwoStates_BeforeStep(world);
    TwoStates_StepOneLoanDayForTest(world, &eastLoan);
    TwoStates_AfterStep(world);

    // West's 500,000 RUB is untouched, East stays at 0 RUB, and East's loan accrues unpaidPenaltyBalance
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2),      0.0, 1e-3);
    TEST_CHECK(eastLoan.unpaidPenaltyBalance > 99.0f);

    TwoStates_OnSessionEnded();
    return true;
}

static bool Test_TwoStates_Loans()
{
    return TestLoans_MaxAvailableBorrow() &&
           TestLoans_UiSwapAndSelectionMapping() &&
           TestLoans_LockstepBorrowAndRepay() &&
           TestLoans_DailyStepAndBankruptcyIsolation();
}

// ---------------------------------------------------------------------------
// Test 10: Two-States Per-State Economic Statistics (Ledgers, Scalars, UI Swap, Rollover & INI)
// ---------------------------------------------------------------------------

static bool TestEconStats_ResourceLedgerAndScalarsIsolation()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    // Set up fake resource table at WORLD_RESOURCES_OFFSET (stride = WORLD_RESOURCE_STRIDE = 0x340)
    std::vector<BYTE> resTable(4 * WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x00) = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x08) = resTable.data() + resTable.size();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x10) = resTable.data() + resTable.size();
    uintptr_t resGravel = (uintptr_t)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    uintptr_t resSteel  = (uintptr_t)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);

    double initRub[2] = { 500000.0, 400000.0 };
    double initUsd[2] = { 200000.0, 150000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // State 1 (West) imports 25t of Gravel for 1,500 RUB (vecIdx 0 = Import RUB) & buys a vehicle for 12,000 RUB
    TwoStates_BeginCommand(1);
    {
        SovietStatResEntry rowGravel = { resGravel, 25.0f, 1500.0f };
        TwoStates_LedgerAddForTest(world, &rowGravel, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
        *(float*)(curStat + STAT_VEHICLES_OFF + 4) += 12000.0f; // ImportRUB
        *(float*)(curStat + STAT_IMMIGRANT_SPEND_OFF + 4) += 600.0f; // Soviet immigrant invite RUB
        *(double*)(world + WORLD_RUB_OFFSET) -= 14100.0;
    }
    TwoStates_EndCommand(1);

    // State 2 (East) imports 10t of Steel for 4,800 RUB (vecIdx 0) & buys a western vehicle for 9,500 USD
    TwoStates_BeginCommand(2);
    {
        SovietStatResEntry rowSteel = { resSteel, 10.0f, 4800.0f };
        TwoStates_LedgerAddForTest(world, &rowSteel, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
        *(float*)(curStat + STAT_VEHICLES_OFF + 0) += 9500.0f; // ImportUSD
        *(float*)(curStat + STAT_TOURISM_OFF + 4 * sizeof(float)) += 3200.0f; // Soviet tourist spend RUB
        *(double*)(world + WORLD_RUB_OFFSET) -= 1600.0;
        *(double*)(world + WORLD_USD_OFFSET) -= 9500.0;
    }
    TwoStates_EndCommand(2);

    float tons = 0.0f, val = 0.0f;
    TEST_CHECK(TwoStates_GetStateEconResEntry(1, 0, resGravel, &tons, &val));
    TEST_CHECK_NEAR(tons, 25.0f, 1e-3f);
    TEST_CHECK_NEAR(val, 1500.0f, 1e-2f);
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 0, resSteel, &tons, &val));

    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 0, resSteel, &tons, &val));
    TEST_CHECK_NEAR(tons, 10.0f, 1e-3f);
    TEST_CHECK_NEAR(val, 4800.0f, 1e-2f);
    TEST_CHECK(!TwoStates_GetStateEconResEntry(2, 0, resGravel, &tons, &val));

    float veh1[4] = {}, veh2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, veh1);
    TwoStates_GetStateEconFloat4ForTest(2, false, veh2);
    TEST_CHECK_NEAR(veh1[0],     0.0f, 1e-2f);
    TEST_CHECK_NEAR(veh1[1], 12000.0f, 1e-2f);
    TEST_CHECK_NEAR(veh2[0],  9500.0f, 1e-2f);
    TEST_CHECK_NEAR(veh2[1],     0.0f, 1e-2f);

    TwoStates_OnSessionEnded();
    return true;
}

static bool TestEconStats_UiSwapAndExactSimulationRestore()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    std::vector<BYTE> resTable(4 * WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x00) = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x08) = resTable.data() + resTable.size();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x10) = resTable.data() + resTable.size();
    uintptr_t resCoal = (uintptr_t)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    uintptr_t resOil  = (uintptr_t)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);

    double initRub[2] = { 300000.0, 200000.0 };
    double initUsd[2] = { 100000.0,  80000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    TwoStates_BeginCommand(1);
    {
        SovietStatResEntry row = { resCoal, 40.0f, 2200.0f };
        TwoStates_LedgerAddForTest(world, &row, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
        *(float*)(curStat + STAT_VEHICLES_OFF + 4) += 5000.0f;
    }
    TwoStates_EndCommand(1);

    TwoStates_BeginCommand(2);
    {
        SovietStatResEntry row = { resOil, 80.0f, 9600.0f };
        TwoStates_LedgerAddForTest(world, &row, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
        *(float*)(curStat + STAT_VEHICLES_OFF + 4) += 15000.0f;
    }
    TwoStates_EndCommand(2);

    // Snapshot combined simulation StatRecord before UI swap
    std::vector<BYTE> simSnapshot(STAT_RECORD_SIZE, 0);
    memcpy(simSnapshot.data(), curStat, STAT_RECORD_SIZE);

    // 1. Host UI (State 1): must see only 1 row (resCoal, 40t, 2200 RUB) and 5000 RUB vehicle import
    TwoStates_UiBegin();
    {
        SovietStatResEntry* b = *(SovietStatResEntry**)(curStat + STAT_RES_VEC_BASE_OFF + 0x00);
        SovietStatResEntry* e = *(SovietStatResEntry**)(curStat + STAT_RES_VEC_BASE_OFF + 0x08);
        TEST_CHECK(b != nullptr && (e - b) == 1);
        TEST_CHECK(b[0].resourcePtr == resCoal);
        TEST_CHECK_NEAR(b[0].amountTons, 40.0f, 1e-3f);
        TEST_CHECK_NEAR(b[0].moneyValue, 2200.0f, 1e-2f);
        TEST_CHECK_NEAR(*(float*)(curStat + STAT_VEHICLES_OFF + 4), 5000.0f, 1e-2f);
    }
    TwoStates_UiEnd();
    TEST_CHECK(memcmp(simSnapshot.data(), curStat, STAT_RECORD_SIZE) == 0);

    // 2. Client UI (State 2): must see only 1 row (resOil, 80t, 9600 RUB) and 15000 RUB vehicle import
    TwoStates_OnSessionJoined(false /*Client*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_UiBegin();
    {
        SovietStatResEntry* b = *(SovietStatResEntry**)(curStat + STAT_RES_VEC_BASE_OFF + 0x00);
        SovietStatResEntry* e = *(SovietStatResEntry**)(curStat + STAT_RES_VEC_BASE_OFF + 0x08);
        TEST_CHECK(b != nullptr && (e - b) == 1);
        TEST_CHECK(b[0].resourcePtr == resOil);
        TEST_CHECK_NEAR(b[0].amountTons, 80.0f, 1e-3f);
        TEST_CHECK_NEAR(b[0].moneyValue, 9600.0f, 1e-2f);
        TEST_CHECK_NEAR(*(float*)(curStat + STAT_VEHICLES_OFF + 4), 15000.0f, 1e-2f);
    }
    TwoStates_UiEnd();
    TEST_CHECK(memcmp(simSnapshot.data(), curStat, STAT_RECORD_SIZE) == 0);

    TwoStates_OnSessionEnded();
    return true;
}

static bool TestEconStats_PeriodRolloverAndSaveMetaPersistence()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    std::vector<BYTE> resTable(4 * WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x00) = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x08) = resTable.data() + resTable.size();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x10) = resTable.data() + resTable.size();
    uintptr_t resFood = (uintptr_t)(resTable.data() + 2 * WORLD_RESOURCE_STRIDE);

    // Special heap-allocated resource pointer at world + WORLD_ELENERGY_RES_PTR_OFF (0xC3B8)
    std::vector<BYTE> elEnergyHostHeap(WORLD_RESOURCE_STRIDE, 0);
    std::vector<BYTE> elEnergyClientHeap(WORLD_RESOURCE_STRIDE, 0);
    *(uintptr_t*)(world + WORLD_ELENERGY_RES_PTR_OFF) = (uintptr_t)elEnergyHostHeap.data();

    // Fake engine history vector (world + WORLD_STAT_HISTORY_OFFSET) with capacity for 2 records
    std::vector<BYTE> histBuf(2 * STAT_RECORD_SIZE, 0);
    *(int32_t*)(histBuf.data() + 0) = 30;
    *(int32_t*)(histBuf.data() + 4) = 1960;

    double initRub[2] = { 400000.0, 400000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // Record Period 1 activity for State 1 (30t Food, 3000 RUB) and State 2 (12t Food, 1200 RUB)
    TwoStates_BeginCommand(1);
    {
        SovietStatResEntry row = { resFood, 30.0f, 3000.0f };
        TwoStates_LedgerAddForTest(world, &row, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
    }
    TwoStates_EndCommand(1);

    TwoStates_BeginCommand(2);
    {
        SovietStatResEntry row = { resFood, 12.0f, 1200.0f };
        TwoStates_LedgerAddForTest(world, &row, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
    }
    TwoStates_EndCommand(2);

    // Simulate engine pushing curStat into histVec and calling StatRecordClear (0x2FC290) with clobbered RCX (nullptr)
    memcpy(histBuf.data(), curStat, STAT_RECORD_SIZE);
    *(int32_t*)(histBuf.data() + 0) = 30;
    *(int32_t*)(histBuf.data() + 4) = 1960;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x00) = histBuf.data();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x08) = histBuf.data() + STAT_RECORD_SIZE;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x10) = histBuf.data() + histBuf.size();

    TwoStates_StatRecordClearForTest(nullptr, curStat);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(1) == 1);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(2) == 1);

    // Current period must now be cleared, while historical record 0 holds Period 1's per-state data
    float curTons = 0.0f, curVal = 0.0f;
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 0, resFood, &curTons, &curVal));

    // Add Period 2 activity: State 1 imports special heap resource (elenergy), State 2 imports 50t Food
    TwoStates_BeginCommand(1);
    {
        SovietStatResEntry elRow = { (uintptr_t)elEnergyHostHeap.data(), 120.0f, 4800.0f };
        TwoStates_LedgerAddForTest(world, &elRow, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
    }
    TwoStates_EndCommand(1);

    TwoStates_BeginCommand(2);
    {
        SovietStatResEntry row = { resFood, 50.0f, 5000.0f };
        TwoStates_LedgerAddForTest(world, &row, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
        *(float*)(curStat + STAT_LOANS_OFF + 8) -= 1400.0f; // PrincipalRUB
    }
    TwoStates_EndCommand(2);

    // Save to republicmp_coop.ini, relocate special resource pointer to Client heap, wipe stats, and reload
    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_econ_save";
    std::filesystem::create_directories(saveDir);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir.string().c_str()));

    *(uintptr_t*)(world + WORLD_ELENERGY_RES_PTR_OFF) = (uintptr_t)elEnergyClientHeap.data();
    TwoStates_ResetEconStatsForTest(nullptr);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(1) == 0);

    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir.string().c_str(), true));
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(1) == 1);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(2) == 1);

    // Verify P<off> token resolved elenergy to elEnergyClientHeap.data() on reload
    TEST_CHECK(TwoStates_GetStateEconResEntry(1, 0, (uintptr_t)elEnergyClientHeap.data(), &curTons, &curVal));
    TEST_CHECK_NEAR(curTons, 120.0f, 1e-3f);
    TEST_CHECK_NEAR(curVal, 4800.0f, 1e-2f);

    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 0, resFood, &curTons, &curVal));
    TEST_CHECK_NEAR(curTons, 50.0f, 1e-3f);
    TEST_CHECK_NEAR(curVal, 5000.0f, 1e-2f);
    float loans2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(2, true, loans2);
    TEST_CHECK_NEAR(loans2[2], -1400.0f, 1e-2f);

    // Verify historical record 0 swaps to State 1's 30t / 3000 RUB during Host UI frame
    TwoStates_OnSessionJoined(true /*Host*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_UiBegin();
    {
        SovietStatResEntry* hb = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 0x00);
        SovietStatResEntry* he = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 0x08);
        TEST_CHECK(hb != nullptr && (he - hb) == 1);
        TEST_CHECK(hb[0].resourcePtr == resFood);
        TEST_CHECK_NEAR(hb[0].amountTons, 30.0f, 1e-3f);
        TEST_CHECK_NEAR(hb[0].moneyValue, 3000.0f, 1e-2f);

        // Simulate Economy & Trade Window (0x361BB0) calling LedgerAdd (0x2FDB00) on temporary UI vector world+0xC7F0:
        // must NOT unswap curStat or histBuf mid-render!
        SovietStatResEntry uiTmpPool[4] = {};
        *(SovietStatResEntry**)(world + 0xC7F0 + 0x00) = uiTmpPool;
        *(SovietStatResEntry**)(world + 0xC7F0 + 0x08) = uiTmpPool;
        *(SovietStatResEntry**)(world + 0xC7F0 + 0x10) = uiTmpPool + 4;
        TwoStates_LedgerAddForTest(world, &hb[0], world + 0xC7F0, 1.0f);

        SovietStatResEntry* hbAfter = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 0x00);
        SovietStatResEntry* heAfter = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 0x08);
        TEST_CHECK(hbAfter != nullptr && (heAfter - hbAfter) == 1);
        TEST_CHECK_NEAR(hbAfter[0].amountTons, 30.0f, 1e-3f);
        TEST_CHECK_NEAR(hbAfter[0].moneyValue, 3000.0f, 1e-2f);
    }
    TwoStates_UiEnd();

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool TestEconStats_BatchConstructionAndDemographics()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    std::vector<BYTE> resTable(4 * WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x00) = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x08) = resTable.data() + resTable.size();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x10) = resTable.data() + resTable.size();
    uintptr_t resSteel  = (uintptr_t)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    uintptr_t resGravel = (uintptr_t)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);
    uintptr_t resBricks = (uintptr_t)(resTable.data() + 2 * WORLD_RESOURCE_STRIDE);

    // 1 building in West SSR (X = -500), 1 building in East SSR (X = +500); full 0x1420 building struct size
    std::vector<BYTE> bldWest(0x1420, 0), bldEast(0x1420, 0);
    *(int32_t*)(bldWest.data() + 0) = 501;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -500.0f;
    *(float*)(bldWest.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(int32_t*)(bldEast.data() + 0) = 502;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = +500.0f;
    *(float*)(bldEast.data() + BLD_PROGRESS_OFFSET) = 0.5f;
    BYTE* bldPtrs[2] = { bldWest.data(), bldEast.data() };
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = bldPtrs;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA)   = bldPtrs + 2;

    // 2 adult housed citizens + 1 child (age 4) in West SSR; 5 adult housed citizens + 1 student (age 14) + 1 21+ child (age 24) in East SSR
    std::vector<BYTE> citBuf(10 * 0x800, 0);
    BYTE* citPtrs[10] = {};
    for (int i = 0; i < 10; ++i) {
        citPtrs[i] = citBuf.data() + (size_t)i * 0x800;
        bool isWest = (i < 3);
        *(BYTE**)(citPtrs[i] + CIT_HOME_BUILDING_OFFSET) = isWest ? bldWest.data() : bldEast.data();
        *(float*)(citPtrs[i] + 0x18) = isWest ? 78.0f : 64.0f;  // Life expectancy
        *(float*)(citPtrs[i] + 0xA8) = isWest ? 2.2f  : 1.2f;   // Education (West: university >= 2.0, East: basic 1.0..2.0)
        *(float*)(citPtrs[i] + CIT_AGE_OFFSET) = isWest ? 32.0f : 40.0f;
        *(float*)(citPtrs[i] + 0xD8) = isWest ? 0.90f : 0.50f;  // Health [0]
        *(float*)(citPtrs[i] + 0xDC) = isWest ? 0.85f : 0.60f;  // Food [1]
        *(float*)(citPtrs[i] + 0xE0) = isWest ? 0.80f : 0.45f;  // Clothes [2]
        *(float*)(citPtrs[i] + 0xE4) = isWest ? 0.20f : 0.70f;  // Alcohol [3]
        *(float*)(citPtrs[i] + 0xF4) = isWest ? 0.75f : 0.35f;  // Loyalty [7]
        *(float*)(citPtrs[i] + 0xF8) = isWest ? 0.88f : 0.42f;  // Happiness [8]
        *(float*)(citPtrs[i] + 0xFC) = isWest ? 2.5f  : 1.2f;   // Electronics [9] (West: Radio+TV, East: Radio only)
    }
    // citPtrs[2]: child age 4.0f with parent citPtrs[0] in West SSR
    *(BYTE**)(citPtrs[2] + CIT_PARENT_OFFSET) = citPtrs[0];
    *(float*)(citPtrs[2] + CIT_AGE_OFFSET) = 4.0f;
    // citPtrs[8]: student age 14.0f with parent citPtrs[3] in East SSR
    *(BYTE**)(citPtrs[8] + CIT_PARENT_OFFSET) = citPtrs[3];
    *(float*)(citPtrs[8] + CIT_AGE_OFFSET) = 14.0f;
    // citPtrs[9]: 21+ living with parent age 24.0f with parent citPtrs[4] in East SSR
    *(BYTE**)(citPtrs[9] + CIT_PARENT_OFFSET) = citPtrs[4];
    *(float*)(citPtrs[9] + CIT_AGE_OFFSET) = 24.0f;

    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x00) = citPtrs;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x08) = citPtrs + 10;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x10) = citPtrs + 10;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x00) = bldPtrs;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x08) = bldPtrs + 2;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x10) = bldPtrs + 2;

    std::vector<BYTE> vehWest1(0x2000, 0), vehEast1(0x2000, 0), vehEast2(0x2000, 0);
    *(int32_t*)(vehWest1.data() + VEH_ID_OFFSET) = 701;
    *(float*)(vehWest1.data() + VEH_POS_OFFSET) = -500.0f;
    *(int32_t*)(vehEast1.data() + VEH_ID_OFFSET) = 702;
    *(float*)(vehEast1.data() + VEH_POS_OFFSET) = +500.0f;
    *(int32_t*)(vehEast2.data() + VEH_ID_OFFSET) = 703;
    *(float*)(vehEast2.data() + VEH_POS_OFFSET) = +600.0f;
    BYTE* vehPtrs[3] = { vehWest1.data(), vehEast1.data(), vehEast2.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 0x00) = vehPtrs;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 0x08) = vehPtrs + 3;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 0x10) = vehPtrs + 3;

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 200000.0, 200000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // Simulate multi-resource construction step on East SSR building (resVecs[0] USD, resVecs[2] RUB, resVecs[8] consumed)
    TwoStates_BeforeStep(world);
    {
        SovietStatResEntry batchEntries[3] = {
            { resSteel,  15.0f, 4500.0f },
            { resGravel, 80.0f, 1600.0f },
            { resBricks, 25.0f, 2500.0f }
        };
        int batchVecs[3] = { 0, 2, 8 };
        TwoStates_ConstructStepBatchForTest(world, bldEast.data(), batchEntries, batchVecs, 3);
    }
    TwoStates_AfterStep(world);

    float tons = 0.0f, val = 0.0f;
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 0, resSteel,  &tons, &val));
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 2, resGravel, &tons, &val));
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 8, resBricks, &tons, &val));

    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 0, resSteel, &tons, &val));
    TEST_CHECK_NEAR(tons, 15.0f, 1e-3f);
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 2, resGravel, &tons, &val));
    TEST_CHECK_NEAR(tons, 80.0f, 1e-3f);
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 8, resBricks, &tons, &val));
    TEST_CHECK_NEAR(tons, 25.0f, 1e-3f);

    // Record 3 births ($Citizens_Born at +0x248) and 2 university graduates (+0x2EC) for State 1
    TwoStates_BeginCommand(1);
    *(int32_t*)(curStat + STAT_CITIZEN_COUNTS_OFF + 0) += 3;
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 0) += 2;
    TwoStates_EndCommand(1);

    int bldCnt = 0, vehCnt = 0;
    TwoStates_CalcStoredResourcesForTest(world, &bldCnt, &vehCnt);
    TEST_CHECK(bldCnt == 1 && vehCnt == 1);

    BYTE* popWinStat = world + WORLD_POP_STAT_CURRENT_OFF;
    BYTE* hudBlock = world + WORLD_HUD_POP_BLOCK_OFFSET;
    // Simulate both a newborn child (0.0f) and an existing adult (78.0f) whose lifespans are mutated by UI (0x2FD2E0 -> 0x8244F0)
    *(float*)(citPtrs[2] + 0x18) = 0.0f;
    TwoStates_UiBegin();
    *(float*)(citPtrs[0] + 0x18) = 61.25f; // UI side-effect on positive lifespan citizen
    *(float*)(citPtrs[2] + 0x18) = 71.5f;  // UI side-effect on zero lifespan citizen
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_COUNTS_OFF + 0) == 3);
    TEST_CHECK(*(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 0) == 2);
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_DEMOGRAPHICS_OFF + 0) == 1); // 1 child < 6
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_DEMOGRAPHICS_OFF + 4) == 0); // 0 students 6..21
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_DEMOGRAPHICS_OFF + 8) == 0); // 0 21+ with parent
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_ADULTS_OFF) == 2);           // 2 independent adults
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 0 * 4), 0.90f, 1e-3f); // West Health = 90%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 1 * 4), 0.85f, 1e-3f); // West Food = 85%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 2 * 4), 0.80f, 1e-3f); // West Clothes = 80%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 7 * 4), 0.75f, 1e-3f); // West Loyalty = 75%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 8 * 4), 0.88f, 1e-3f); // West Happiness = 88%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_AVERAGES_OFF + 4), 75.333f, 1e-2f);       // (78 + 78 + 70 fallback)/3
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_BRACKETS_OFF + 3 * 4) == 3);                 // 3 university educated
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_BRACKETS_OFF + 6 * 4) == 2);                 // 2 adults with TV
    TEST_CHECK_NEAR(*(float*)(popWinStat + STAT_CITIZEN_SATISFACTION_OFF + 0 * 4), 0.90f, 1e-3f);
    TEST_CHECK_NEAR(*(float*)(popWinStat + STAT_CITIZEN_SATISFACTION_OFF + 8 * 4), 0.88f, 1e-3f);
    TEST_CHECK(*(int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET) == 3);          // Top HUD population = 3
    TEST_CHECK(*(int32_t*)(hudBlock + 0x24) == 2);                             // Top HUD independent adults = 2
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0x0C), 0.90f, 1e-3f);                 // Top HUD Health = 90%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xA8) / *(float*)(hudBlock + 0x00), 0.75f, 1e-3f); // Top HUD Loyalty = 75%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xAC) / *(float*)(hudBlock + 0x00), 0.88f, 1e-3f); // Top HUD Happiness = 88%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xB8) / *(float*)(hudBlock + 0x00), 75.333f, 1e-2f); // Top HUD Lifespan
    TEST_CHECK(*(int32_t*)(popWinStat + STAT_CITIZEN_COUNTS_OFF + 0) == 3);
    TEST_CHECK(*(int32_t*)(popWinStat + STAT_CITIZEN_ADULTS_OFF) == 2);
    TEST_CHECK(SafeCountStdVector(world + WORLD_CITIZENS_OFFSET, sizeof(void*)) == 3);
    TwoStates_UiEnd();
    TEST_CHECK_NEAR(*(float*)(citPtrs[0] + 0x18), 78.0f, 1e-5f); // Adult positive lifespan restored to 78.0f after UI!
    TEST_CHECK_NEAR(*(float*)(citPtrs[2] + 0x18), 0.0f, 1e-6f);  // Newborn lifespan restored to 0.0f after UI!
    *(float*)(citPtrs[2] + 0x18) = 78.0f;
    TEST_CHECK(SafeCountStdVector(world + WORLD_CITIZENS_OFFSET, sizeof(void*)) == 10);

    TwoStates_OnSessionJoined(false /*Client*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_CalcStoredResourcesForTest(world, &bldCnt, &vehCnt);
    TEST_CHECK(bldCnt == 1 && vehCnt == 2);

    TwoStates_UiBegin();
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_COUNTS_OFF + 0) == 0);
    TEST_CHECK(*(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 0) == 0);
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_DEMOGRAPHICS_OFF + 0) == 0); // 0 child < 6
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_DEMOGRAPHICS_OFF + 4) == 1); // 1 student 6..21
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_DEMOGRAPHICS_OFF + 8) == 1); // 1 21+ with parent
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_ADULTS_OFF) == 5);           // 5 independent adults
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 0 * 4), 0.50f, 1e-3f); // East Health = 50%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 1 * 4), 0.60f, 1e-3f); // East Food = 60%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 2 * 4), 0.45f, 1e-3f); // East Clothes = 45%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 7 * 4), 0.35f, 1e-3f); // East Loyalty = 35%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_SATISFACTION_OFF + 8 * 4), 0.42f, 1e-3f); // East Happiness = 42%
    TEST_CHECK_NEAR(*(float*)(curStat + STAT_CITIZEN_AVERAGES_OFF + 4), 64.0f, 1e-2f);         // East Life Expectancy = 64y
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_BRACKETS_OFF + 2 * 4) == 7);                 // 7 basic educated
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_BRACKETS_OFF + 5 * 4) == 5);                 // 5 adults with Radio
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_BRACKETS_OFF + 6 * 4) == 0);                 // 0 adults with TV
    TEST_CHECK_NEAR(*(float*)(popWinStat + STAT_CITIZEN_SATISFACTION_OFF + 0 * 4), 0.50f, 1e-3f);
    TEST_CHECK_NEAR(*(float*)(popWinStat + STAT_CITIZEN_SATISFACTION_OFF + 8 * 4), 0.42f, 1e-3f);
    TEST_CHECK(*(int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET) == 7);          // Top HUD population = 7
    TEST_CHECK(*(int32_t*)(hudBlock + 0x24) == 5);                             // Top HUD independent adults = 5
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0x0C), 0.50f, 1e-3f);                 // Top HUD Health = 50%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xA8) / *(float*)(hudBlock + 0x00), 0.35f, 1e-3f); // Top HUD Loyalty = 35%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xAC) / *(float*)(hudBlock + 0x00), 0.42f, 1e-3f); // Top HUD Happiness = 42%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xB8) / *(float*)(hudBlock + 0x00), 64.0f, 1e-2f);  // Top HUD Lifespan = 64y
    TEST_CHECK(*(int32_t*)(popWinStat + STAT_CITIZEN_COUNTS_OFF + 0) == 0);
    TEST_CHECK(*(int32_t*)(popWinStat + STAT_CITIZEN_ADULTS_OFF) == 5);
    TEST_CHECK(SafeCountStdVector(world + WORLD_CITIZENS_OFFSET, sizeof(void*)) == 7);
    TwoStates_UiEnd();
    TEST_CHECK(SafeCountStdVector(world + WORLD_CITIZENS_OFFSET, sizeof(void*)) == 10);

    // Verify period rollover + republicmp_coop.ini round-trip of ;S:, ;A:, ;P:, ;B: tags
    std::vector<BYTE> histBuf(STAT_RECORD_SIZE, 0);
    *(int32_t*)(histBuf.data() + 0) = 15;
    *(int32_t*)(histBuf.data() + 4) = 1961;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x00) = histBuf.data();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x08) = histBuf.data() + STAT_RECORD_SIZE;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x10) = histBuf.data() + STAT_RECORD_SIZE;
    TwoStates_StatRecordClearForTest(world, curStat);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(1) == 1);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(2) == 1);

    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_demo_save";
    std::filesystem::create_directories(saveDir);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir.string().c_str()));
    TwoStates_ResetEconStatsForTest(nullptr);
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir.string().c_str(), false));
    TwoStates_UiBegin();
    TEST_CHECK_NEAR(*(float*)(histBuf.data() + STAT_CITIZEN_SATISFACTION_OFF + 0 * 4), 0.50f, 1e-2f);
    TEST_CHECK_NEAR(*(float*)(histBuf.data() + STAT_CITIZEN_SATISFACTION_OFF + 8 * 4), 0.42f, 1e-2f);
    TEST_CHECK_NEAR(*(float*)(histBuf.data() + STAT_CITIZEN_AVERAGES_OFF + 4), 64.0f, 1e-2f);
    TEST_CHECK(*(int32_t*)(histBuf.data() + STAT_CITIZEN_ADULTS_OFF) == 5);
    TEST_CHECK(*(int32_t*)(histBuf.data() + STAT_CITIZEN_BRACKETS_OFF + 5 * 4) == 5);
    TwoStates_UiEnd();

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static uintptr_t g_relayClobberSeenRcx = 0;
static uintptr_t g_relayClobberSeenRdx = 0;

static float __fastcall MockLeafHookClobberVolatile(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    g_relayClobberSeenRcx = a1;
    g_relayClobberSeenRdx = a2;
    volatile double mathTmp = sin((double)(a3 & 0xFF)) + cos((double)(a4 & 0xFF));
    (void)mathTmp;
    return 42500.5f;
}

static bool TestEconStats_LeafHookRelayStubPreservesRegisters()
{
    void* stubVoidZeroRcx = TwoStates_BuildRegPreservingRelayStub((void*)&MockLeafHookClobberVolatile, false, true);
    void* stubFloatKeep   = TwoStates_BuildRegPreservingRelayStub((void*)&MockLeafHookClobberVolatile, true, false);
    TEST_CHECK(stubVoidZeroRcx != nullptr);
    TEST_CHECK(stubFloatKeep != nullptr);

    // Build a tiny machine-code verifier that calls stubVoidZeroRcx with known rcx/rdx/r8/r9
    // and stores post-call rcx/rdx/r8/r9 into an output array[4].
    BYTE* page = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    TEST_CHECK(page != nullptr);
    if (!page) return false;

    // Verifier signature: void (__fastcall*)(uintptr_t outRegs[4], void* targetStub)
    // Entry: rcx = outRegs, rdx = targetStub
    BYTE* p = page;
    auto emit = [&](std::initializer_list<BYTE> bytes) {
        for (BYTE b : bytes) *p++ = b;
    };
    emit({ 0x53 });                                             // push rbx
    emit({ 0x56 });                                             // push rsi
    emit({ 0x48, 0x83, 0xEC, 0x28 });                           // sub rsp, 0x28
    emit({ 0x48, 0x8B, 0xD9 });                                 // mov rbx, rcx (outRegs)
    emit({ 0x48, 0x8B, 0xF2 });                                 // mov rsi, rdx (targetStub)
    emit({ 0x48, 0xB9, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 }); // mov rcx, 0x8877665544332211
    emit({ 0x48, 0xBA, 0xA8, 0x01, 0x62, 0x0C, 0xF6, 0x7F, 0x00, 0x00 }); // mov rdx, 0x00007FF60C6201A8
    emit({ 0x49, 0xB8, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11 }); // mov r8,  0x1100FFEEDDCCBBAA
    emit({ 0x49, 0xB9, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22 }); // mov r9,  0x2233445566778899
    emit({ 0xFF, 0xD6 });                                       // call rsi
    emit({ 0x48, 0x89, 0x0B });                                 // mov [rbx+0x00], rcx
    emit({ 0x48, 0x89, 0x53, 0x08 });                           // mov [rbx+0x08], rdx
    emit({ 0x4C, 0x89, 0x43, 0x10 });                           // mov [rbx+0x10], r8
    emit({ 0x4C, 0x89, 0x4B, 0x18 });                           // mov [rbx+0x18], r9
    emit({ 0x48, 0x83, 0xC4, 0x28 });                           // add rsp, 0x28
    emit({ 0x5E });                                             // pop rsi
    emit({ 0x5B });                                             // pop rbx
    emit({ 0xC3 });                                             // ret
    FlushInstructionCache(GetCurrentProcess(), page, (size_t)(p - page));

    typedef float (__fastcall* VerifierFn)(uintptr_t outRegs[4], void* targetStub);
    VerifierFn runVerifier = (VerifierFn)page;

    uintptr_t outRegs[4] = {};
    g_relayClobberSeenRcx = 0;
    g_relayClobberSeenRdx = 0;
    runVerifier(outRegs, stubVoidZeroRcx);
    TEST_CHECK(g_relayClobberSeenRcx == 0x8877665544332211ULL);
    TEST_CHECK(g_relayClobberSeenRdx == 0x00007FF60C6201A8ULL);
    TEST_CHECK(outRegs[0] == 0ULL);                       // rcx zeroed on exit (matching 0x2FC290 xor ecx, ecx)
    TEST_CHECK(outRegs[1] == 0x00007FF60C6201A8ULL);      // rdx preserved (preventing 0x334CD1 crash at 0x2FC5E4!)
    TEST_CHECK(outRegs[2] == 0x1100FFEEDDCCBBAAULL);      // r8 preserved
    TEST_CHECK(outRegs[3] == 0x2233445566778899ULL);      // r9 preserved

    memset(outRegs, 0, sizeof(outRegs));
    float fRes = runVerifier(outRegs, stubFloatKeep);
    TEST_CHECK_NEAR(fRes, 42500.5f, 1e-2f);               // xmm0 float return preserved
    TEST_CHECK(outRegs[0] == 0x8877665544332211ULL);      // rcx preserved when zeroRcxOnExit == false
    TEST_CHECK(outRegs[1] == 0x00007FF60C6201A8ULL);      // rdx preserved
    TEST_CHECK(outRegs[2] == 0x1100FFEEDDCCBBAAULL);      // r8 preserved
    TEST_CHECK(outRegs[3] == 0x2233445566778899ULL);      // r9 preserved

    VirtualFree(page, 0, MEM_RELEASE);
    return true;
}

static bool TestEconStats_PosKeyAndPoisonedSaveHeal()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    int keyWest = TwoStates_BuildingPosKey(-650.0f, 120.0f);
    int keyEast = TwoStates_BuildingPosKey(+650.0f, 120.0f);
    TEST_CHECK(keyWest >= 0x40000000 && keyEast >= 0x40000000);
    TEST_CHECK(keyWest != keyEast);
    TEST_CHECK(TwoStates_BuildingPosKey(-650.0f, 120.0f) == keyWest);

    double initRub[2] = { 10000000.0, 10000000.0 };
    double initUsd[2] = {  2000000.0,  2000000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // 1. Attempting to register buildingId == 0 must be rejected so *(int*)b == 0 never poisons West/East lookup
    TwoStates_RegisterBuildingOwner(0, 2);
    TEST_CHECK(TwoStates_GetBuildingOwner(0, -650.0f, 120.0f) == 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(0, +650.0f, 120.0f) == 2);

    // 2. Placing a road/construction on East SSR in the shared border zone (x = +100.0f) with *(int*)c == 0
    //    must register by TwoStates_BuildingPosKey(+100.0f, -40.0f) and never poison West buildings with *(int*)b == 0
    std::vector<BYTE> conBorderEast(0x800, 0);
    *(int32_t*)(conBorderEast.data() + 0) = 0;
    *(float*)(conBorderEast.data() + OBJ_POS_OFFSET)     = 100.0f;
    *(float*)(conBorderEast.data() + OBJ_POS_OFFSET + 8) = -40.0f;
    BYTE* conPtrs[1] = { conBorderEast.data() };

    TwoStates_BeginCommand(2);
    *(BYTE***)(g_mockExeBuffer + VEC_CONSTRUCTIONS_BEGIN_RVA) = conPtrs;
    *(BYTE***)(g_mockExeBuffer + VEC_CONSTRUCTIONS_END_RVA)   = conPtrs + 1;
    TwoStates_EndCommand(2);

    TEST_CHECK(TwoStates_GetBuildingOwner(0, -500.0f, 0.0f) == 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(0, 100.0f, -40.0f) == 2);

    // 3. Loading a polluted republicmp_coop.ini with building_owners=0:2 and net_trade1=0, net_trade2=-41668554
    //    must reject 0:2 and automatically merge State 1's polluted R2 trade rows into State 2
    std::vector<BYTE> resTable(8 * WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x00) = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x08) = resTable.data() + resTable.size();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x10) = resTable.data() + resTable.size();
    uintptr_t resWork   = (uintptr_t)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    uintptr_t resGravel = (uintptr_t)(resTable.data() + 5 * WORLD_RESOURCE_STRIDE);

    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_poisoned_save";
    std::filesystem::create_directories(saveDir);
    std::string meta =
        "[republicmp_coop]\r\n"
        "version=1\r\n"
        "coop_mode=1\r\n"
        "border_half_width_m=250\r\n"
        "expand_borders=0\r\n"
        "state1_rub_cents=1000000000\r\n"
        "state2_rub_cents=957977759\r\n"
        "state1_usd_cents=200000000\r\n"
        "state2_usd_cents=200000000\r\n"
        "net_trade1_rub_cents=0\r\n"
        "net_trade2_rub_cents=-41668554\r\n"
        "net_trade1_usd_cents=0\r\n"
        "net_trade2_usd_cents=0\r\n"
        "building_owners=0:2\r\n"
        "vehicle_owners=0:2\r\n"
        "econ_cur1=D:1,1970;R2:R0:379077:0,R5:42135:133243\r\n"
        "econ_cur2=D:1,1970;R2:R0:331692:0,R5:36868:116588\r\n"
        "econ_hist_count=0\r\n";
    {
        FILE* f = nullptr;
        std::string metaPath = (saveDir / "republicmp_coop.ini").string();
        if (fopen_s(&f, metaPath.c_str(), "wb") == 0 && f) {
            fwrite(meta.data(), 1, meta.size(), f);
            fclose(f);
        }
    }
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir.string().c_str(), true));
    TEST_CHECK(TwoStates_GetBuildingOwner(0, -500.0f, 0.0f) == 1);

    float tons = 0.0f, val = 0.0f;
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 2, resWork,   &tons, &val));
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 2, resGravel, &tons, &val));
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 2, resWork, &tons, &val));
    TEST_CHECK_NEAR(tons, 710.769f, 1e-2f);
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 2, resGravel, &tons, &val));
    TEST_CHECK_NEAR(tons, 79.003f, 1e-2f);
    TEST_CHECK_NEAR(val, 2498.31f, 1e-1f);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool Test_TwoStates_EconomyStats()
{
    return TestEconStats_ResourceLedgerAndScalarsIsolation() &&
           TestEconStats_UiSwapAndExactSimulationRestore() &&
           TestEconStats_PeriodRolloverAndSaveMetaPersistence() &&
           TestEconStats_BatchConstructionAndDemographics() &&
           TestEconStats_LeafHookRelayStubPreservesRegisters() &&
           TestEconStats_PosKeyAndPoisonedSaveHeal();
}

// ---------------------------------------------------------------------------
// Test 11: Singleplayer Isolation & Session Transition State Reset
// ---------------------------------------------------------------------------

static bool TestSingleplayer_InitialDormancy()
{
    ResetMockWorldMemory();
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    NativeMenu_ClearHostOnNextLoad();

    // 1. Session state must be idle and not busy
    TEST_CHECK(SaveSync_GetState() == SESSION_IDLE);
    TEST_CHECK(!SaveSync_IsBusy());
    TEST_CHECK(!SaveSync_IsWaitingLobbyActive());

    // 2. Two States co-op subsystems must be inactive
    TEST_CHECK(!TwoStates_IsActive());
    TEST_CHECK(!TwoStates_IsCoopBankMounted());

    // 3. Native menu host trigger must be dormant
    TEST_CHECK(!NativeMenu_IsHostOnNextLoad());
    TEST_CHECK(!NativeMenu_WantsInput());

    // 4. Input blocking must be inactive
    TEST_CHECK(!InputBlock_IsBlocked());

    // 5. Session patches must not be applied
    SessionPatches_Tick();
    TEST_CHECK(!SessionPatches_IsApplied());

    return true;
}

static bool TestSingleplayer_OverlayZeroDrawCalls()
{
    ResetMockWorldMemory();
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1; // mock world loaded
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    TEST_CHECK(SaveSync_IsWorldLoaded());

    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    NativeMenu_ClearHostOnNextLoad();

    // Initialize headless ImGui context for draw call verification
    ImGuiContext* ctx = ImGui::CreateContext();
    TEST_CHECK(ctx != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    unsigned char* texPixels = nullptr;
    int texW = 0, texH = 0;
    io.Fonts->GetTexDataAsRGBA32(&texPixels, &texW, &texH);
    ImGui::NewFrame();

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    TEST_CHECK(bg != nullptr);
    const int initialCmdCount = bg->CmdBuffer.Size;
    const int initialVtxCount = bg->VtxBuffer.Size;

    // In singleplayer, TwoStates_DrawOverlay must be completely dormant:
    // 0 3D border lines, 0 partner cursors/pings, 0 minimap zone rects/badges, 0 toasts
    TwoStates_DrawOverlay();

    TEST_CHECK(bg->CmdBuffer.Size == initialCmdCount);
    TEST_CHECK(bg->VtxBuffer.Size == initialVtxCount);

    // Call NativeMenu_DrawOverlay for singleplayer screen (screenId = 1: Root Main Menu)
    *(int*)(g_mockExeBuffer + MENU_OBJ_RVA + MENU_SCREEN_ID_OFF) = 1;
    NativeMenu_DrawOverlay();
    TEST_CHECK(!NativeMenu_WantsInput());

    // Also verify native Load Game screen (screenId = 2) without co-op delegation:
    // DrawLaunchScreenCoopBadge must be suppressed in singleplayer
    *(int*)(g_mockExeBuffer + MENU_OBJ_RVA + MENU_SCREEN_ID_OFF) = 2;
    NativeMenu_DrawOverlay();
    TEST_CHECK(!NativeMenu_WantsInput());

    ImGui::EndFrame();
    ImGui::DestroyContext(ctx);
    return true;
}

static bool TestSingleplayer_InputBlockAndPatchesDormancy()
{
    ResetMockWorldMemory();
    BYTE* focusByte = g_mockExeBuffer + WINDOW_FOCUS_BYTE_RVA;

    // In singleplayer (SESSION_IDLE), losing window focus must NOT block input
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    *focusByte = 0; // window loses focus
    TEST_CHECK(!InputBlock_IsBlocked());

    *focusByte = 1; // window focused
    TEST_CHECK(!InputBlock_IsBlocked());

    // In co-op session, unfocused window or waiting lobby blocks input
    SaveSync_SetTestSessionState(SESSION_HOST_JOINING, false, "");
    *focusByte = 0;
    TEST_CHECK(InputBlock_IsBlocked());

    // Transition back to singleplayer: input blocking immediately drops
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TEST_CHECK(!InputBlock_IsBlocked());
    *focusByte = 1;

    // Verify SessionPatches state transitions:
    SessionPatches_SetAppliedForTest(true);
    TEST_CHECK(SessionPatches_IsApplied());

    // Ticking session patches in singleplayer must restore them
    SessionPatches_Tick();
    TEST_CHECK(!SessionPatches_IsApplied());

    return true;
}

static bool TestSingleplayer_SessionTransitionReset()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1; // world loaded
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;

    // 1. Setup co-op Two States session with distinct balances and ownership
    *(double*)(world + WORLD_RUB_OFFSET) = 2000000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 800000.0;
    double startRub[2] = { 1200000.0, 800000.0 };
    double startUsd[2] = {  500000.0, 300000.0 };
    TwoStates_OnSessionJoined(/*isHost=*/true, COOP_MODE_TWO_STATES, 250.0f, false, startRub, startUsd);
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    TwoStates_MountSaveBank(true);
    NativeMenu_SetHostOnNextLoadForTest(true);

    // Register cross-territory owners (entity 7001 owned by State 2 on West territory)
    TwoStates_RegisterBuildingOwner(7001, 2);
    TwoStates_RegisterVehicleOwner(8001, 2);
    TEST_CHECK(TwoStates_GetBuildingOwner(7001, -400.0f, 0.0f) == 2);
    TEST_CHECK(TwoStates_GetVehicleOwner(8001, nullptr) == 2);
    TEST_CHECK(TwoStates_IsActive());
    TEST_CHECK(TwoStates_IsCoopBankMounted());
    TEST_CHECK(NativeMenu_IsHostOnNextLoad());

    // 2. Exit co-op session (Main Menu transition)
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    NativeMenu_ClearHostOnNextLoad();

    // 3. Load singleplayer game
    *(double*)(world + WORLD_RUB_OFFSET) = 750000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 250000.0;
    *(unsigned char*)(g_mockExeBuffer + TERRAIN_FLAG_DAT_RVA) = 1;
    TwoStates_OnAnyWorldLoaded(false);

    // 4. Verify complete decoupling & state reset
    TEST_CHECK(!TwoStates_IsActive());
    TEST_CHECK(!TwoStates_IsCoopBankMounted());
    TEST_CHECK(!NativeMenu_IsHostOnNextLoad());
    TEST_CHECK(!SaveSync_IsBusy());

    // Building ownership map must be cleared: 7001 reverts to West territory default (1)
    TEST_CHECK(TwoStates_GetBuildingOwner(7001, -400.0f, 0.0f) == 1);
    // Vehicle ownership map must be cleared: 8001 reverts to 0 (unowned)
    TEST_CHECK(TwoStates_GetVehicleOwner(8001, nullptr) == 0);

    // Singleplayer money query must return current world currency (not stale co-op split)
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 750000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 250000.0, 1e-2);

    return true;
}

static bool TestSingleplayer_WindowTitleRestoration()
{
    // Create a temporary hidden Win32 STATIC window with the vanilla game title
    const wchar_t kVanillaTitle[] = L"Workers & Resources: Soviet Republic";
    HWND hwnd = CreateWindowExW(0, L"STATIC", kVanillaTitle, WS_POPUP,
                                0, 0, 10, 10, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    TEST_CHECK(hwnd != nullptr);

    ImGuiHook_ResetTitleTrackingForTest();

    // 1. Initial singleplayer state: title remains untouched
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    ImGuiHook_UpdateWindowTitleForTest(hwnd);
    wchar_t titleBuf[128] = {};
    GetWindowTextW(hwnd, titleBuf, (int)(sizeof(titleBuf) / sizeof(titleBuf[0])));
    TEST_CHECK(wcscmp(titleBuf, kVanillaTitle) == 0);

    // 2. Entering co-op session: title receives co-op status tag
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    ImGuiHook_UpdateWindowTitleForTest(hwnd);
    GetWindowTextW(hwnd, titleBuf, (int)(sizeof(titleBuf) / sizeof(titleBuf[0])));
    TEST_CHECK(wcsstr(titleBuf, L"Workers & Resources") != nullptr);
    TEST_CHECK(wcsstr(titleBuf, L"RepublicMP") != nullptr);

    // 3. Exiting back to singleplayer: title must be cleanly restored to vanilla
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    ImGuiHook_UpdateWindowTitleForTest(hwnd);
    GetWindowTextW(hwnd, titleBuf, (int)(sizeof(titleBuf) / sizeof(titleBuf[0])));
    TEST_CHECK(wcscmp(titleBuf, kVanillaTitle) == 0);

    DestroyWindow(hwnd);
    ImGuiHook_ResetTitleTrackingForTest();
    return true;
}

static bool Test_SingleplayerIsolationAndReset()
{
    return TestSingleplayer_InitialDormancy() &&
           TestSingleplayer_OverlayZeroDrawCalls() &&
           TestSingleplayer_InputBlockAndPatchesDormancy() &&
           TestSingleplayer_SessionTransitionReset() &&
           TestSingleplayer_WindowTitleRestoration();
}

// ---------------------------------------------------------------------------
// Test 12: Vehicle DLC Filtering, Canonical ScriptName Remapping & Diag Telemetry
// ---------------------------------------------------------------------------

static bool SubTest_DlcTokenExtractionAndMasks()
{
    char dlcTok[16] = {};

    // 1. Building DLC token extraction (dlc[0-9]+_)
    TEST_CHECK(SaveSync_ExtractDlcToken("dlc2_residential01", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc2") == 0);

    TEST_CHECK(SaveSync_ExtractDlcToken("DLC3_swimming_hall", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc3") == 0);

    TEST_CHECK(!SaveSync_ExtractDlcToken("building_vanilla_farm", dlcTok, sizeof(dlcTok)));

    // 2. Vehicle DLC token extraction (forward and back slashes, media_soviet/ prefix)
    TEST_CHECK(SaveSync_ExtractDlcToken("media_soviet/dlc2/vehicles/truck.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc2") == 0);

    TEST_CHECK(SaveSync_ExtractDlcToken("dlc3\\vehicles\\steam_p36.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc3") == 0);

    TEST_CHECK(SaveSync_ExtractDlcToken("media_soviet/dlc1/vehicles/bus.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc1") == 0);

    TEST_CHECK(!SaveSync_ExtractDlcToken("media_soviet/vehicles/bus.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(!SaveSync_ExtractDlcToken("workshop/123456/truck.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(!SaveSync_ExtractDlcToken(nullptr, dlcTok, sizeof(dlcTok)));
    TEST_CHECK(!SaveSync_ExtractDlcToken("", dlcTok, sizeof(dlcTok)));

    // 3. Bitmask mapping
    TEST_CHECK(SaveSync_GetDlcMask("dlc1") == 0x01);
    TEST_CHECK(SaveSync_GetDlcMask("dlc2") == 0x02);
    TEST_CHECK(SaveSync_GetDlcMask("dlc3") == 0x04);
    TEST_CHECK(SaveSync_GetDlcMask("dlc2,dlc3") == 0x06);
    TEST_CHECK(SaveSync_GetDlcMask("dlc_unknown") == 0x00);
    TEST_CHECK(SaveSync_GetDlcMask(nullptr) == 0x00);

    // 4. Formatting DLC names
    char namesBuf[128] = {};
    SaveSync_FormatDlcNames(0x02, namesBuf, sizeof(namesBuf));
    TEST_CHECK(strstr(namesBuf, "Biomes") != nullptr);

    SaveSync_FormatDlcNames(0x06, namesBuf, sizeof(namesBuf));
    TEST_CHECK(strstr(namesBuf, "Biomes") != nullptr && strstr(namesBuf, "Early Start") != nullptr);

    SaveSync_FormatDlcNames(0x00, namesBuf, sizeof(namesBuf));
    TEST_CHECK(_stricmp(namesBuf, "none") == 0);

    return true;
}

static bool SubTest_VehicleDlcSessionValidation()
{
    // 1. Singleplayer Isolation: All vehicles allowed regardless of DLC state
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_SetTestDlcLists("dlc2", ""); // host has dlc2, client has none

    std::vector<BYTE> vehType(0xA000, 0);
    char dlcPath[64] = "dlc3/vehicles/steam_p36.ini";
    strncpy_s((char*)vehType.data() + 0x200, 64, dlcPath, _TRUNCATE);

    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2. Co-op Session: Host has dlc2,dlc3 (0x06), Client has dlc2 (0x02) -> Common is dlc2 (0x02)
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    SaveSync_SetTestDlcLists("dlc2,dlc3", "dlc2");

    uint32_t hostM = 0, clientM = 0, commonM = 0;
    SaveSync_GetDlcMasks(&hostM, &clientM, &commonM);
    TEST_CHECK(hostM == 0x06 && clientM == 0x02 && commonM == 0x02);

    // 2a. Vehicle with shared DLC (dlc2) -> ALLOWED
    strncpy_s((char*)vehType.data() + 0x200, 64, "media_soviet/dlc2/vehicles/truck.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2b. Vehicle with unshared DLC (dlc3) -> BLOCKED
    strncpy_s((char*)vehType.data() + 0x200, 64, "dlc3\\vehicles\\steam_p36.ini", _TRUNCATE);
    TEST_CHECK(!SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(!ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2c. Vanilla vehicle -> ALLOWED
    strncpy_s((char*)vehType.data() + 0x200, 64, "media_soviet/vehicles/bus.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2d. Workshop mod vehicle -> ALLOWED by DLC check (falls through to workshop check)
    strncpy_s((char*)vehType.data() + 0x200, 64, "media_soviet/workshop/12345/truck.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));

    // Cleanup session state back to singleplayer
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_SetTestDlcLists("", "");
    return true;
}

static bool SubTest_TrailerDlcValidation()
{
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    SaveSync_SetTestDlcLists("dlc2,dlc3", "dlc2"); // dlc3 is unshared

    // Lead vehicle is vanilla (no DLC requirement)
    std::vector<BYTE> leadType(0xA000, 0);
    strncpy_s((char*)leadType.data() + 0x200, 64, "media_soviet/vehicles/train_loco.ini", _TRUNCATE);

    // Coupled trailer array at leadType + 0x7C08
    // Vector of elements with stride 0x110. Pointer to trailer VehicleType is at offset +0x100 within each record.
    const unsigned kNumTrailers = 2;
    std::vector<BYTE> trailerRecords(0x110 * kNumTrailers, 0);

    std::vector<BYTE> tr0Type(0x1000, 0);
    strncpy_s((char*)tr0Type.data() + 0x200, 64, "media_soviet/vehicles/boxcar.ini", _TRUNCATE); // vanilla trailer

    std::vector<BYTE> tr1Type(0x1000, 0);
    strncpy_s((char*)tr1Type.data() + 0x200, 64, "dlc3/vehicles/wagon_2axle.ini", _TRUNCATE); // unowned DLC!

    *(BYTE**)(trailerRecords.data() + 0 * 0x110 + 0x100) = tr0Type.data();
    *(BYTE**)(trailerRecords.data() + 1 * 0x110 + 0x100) = tr1Type.data();

    *(BYTE**)(leadType.data() + 0x7C08) = trailerRecords.data();
    *(BYTE**)(leadType.data() + 0x7C10) = trailerRecords.data() + trailerRecords.size();

    // Must block because trailer 1 requires unshared dlc3
    TEST_CHECK(!SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Change trailer 1 to owned dlc2 -> now both trailers allowed -> returns true
    strncpy_s((char*)tr1Type.data() + 0x200, 64, "media_soviet/dlc2/vehicles/flatcar.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Consist length 20+: verify trailer inspection extends past legacy 16 cap
    const unsigned kNumTrailers20 = 21;
    std::vector<BYTE> trailerRecords20(0x110 * kNumTrailers20, 0);
    std::vector<std::vector<BYTE>> trailerTypes20(kNumTrailers20, std::vector<BYTE>(0x1000, 0));

    for (unsigned i = 0; i < kNumTrailers20; ++i) {
        strncpy_s((char*)trailerTypes20[i].data() + 0x200, 64, "media_soviet/vehicles/boxcar.ini", _TRUNCATE);
        *(BYTE**)(trailerRecords20.data() + i * 0x110 + 0x100) = trailerTypes20[i].data();
    }

    // Trailer 17 (index 16) requires unshared DLC (dlc3)
    strncpy_s((char*)trailerTypes20[16].data() + 0x200, 64, "dlc3/vehicles/wagon_special.ini", _TRUNCATE);
    *(BYTE**)(leadType.data() + 0x7C08) = trailerRecords20.data();
    *(BYTE**)(leadType.data() + 0x7C10) = trailerRecords20.data() + trailerRecords20.size();

    // Must block because trailer 17 (index 16) requires unshared dlc3 (verifies cap > 16)
    TEST_CHECK(!SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Revert trailer 17 to owned dlc2 -> now all trailers allowed -> returns true
    strncpy_s((char*)trailerTypes20[16].data() + 0x200, 64, "media_soviet/dlc2/vehicles/flatcar.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Cleanup session state
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_SetTestDlcLists("", "");
    return true;
}

static bool SubTest_ScriptNameResolutionWithShiftedIndices()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Create 4 vehicle types, each of size WORLD_VEHICLE_TYPE_STRIDE (0x9F08)
    const size_t kTypeCount = 4;
    const size_t kStride = WORLD_VEHICLE_TYPE_STRIDE;
    std::vector<BYTE> typePool(kStride * kTypeCount, 0);

    BYTE* t0 = typePool.data() + 0 * kStride;
    BYTE* t1 = typePool.data() + 1 * kStride;
    BYTE* t2 = typePool.data() + 2 * kStride;
    BYTE* t3 = typePool.data() + 3 * kStride;

    strncpy_s((char*)t0 + 0x200, 64, "media_soviet/vehicles/bus_old.ini", _TRUNCATE);
    strncpy_s((char*)t1 + 0x200, 64, "media_soviet/dlc2/vehicles/bus_bi.ini", _TRUNCATE);
    strncpy_s((char*)t2 + 0x200, 64, "media_soviet/vehicles/truck_tatra.ini", _TRUNCATE);
    strncpy_s((char*)t3 + 0x200, 64, "dlc2\\vehicles\\loader_win.ini", _TRUNCATE);

    // Setup vehicle types vector at world + WORLD_VEHICLE_TYPES_OFFSET (0x12840)
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET)     = typePool.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = typePool.data() + typePool.size();

    // Direct match by full path
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2/vehicles/bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "dlc2/vehicles/bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/vehicles/bus_old.ini") == t0);

    // Case-insensitive match without "media_soviet/" prefix
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "vehicles/truck_tatra.ini") == t2);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "MEDIA_SOVIET/VEHICLES/TRUCK_TATRA.INI") == t2);

    // Slash variations (forward slashes, backslashes, mixed slashes)
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet\\dlc2\\vehicles\\bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "dlc2\\vehicles\\bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2\\vehicles/bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet\\dlc2/vehicles\\bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "dlc2/vehicles/loader_win.ini") == t3);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2/vehicles/loader_win.ini") == t3);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet\\dlc2\\vehicles\\loader_win.ini") == t3);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2\\vehicles/loader_win.ini") == t3);

    // Inexistent vehicle
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/vehicles/non_existent.ini") == nullptr);

    // Index drift test: simulated host has t2 at index 0, but on our client t2 is at index 2
    // Wire message has index 0 (which points to bus_old), but m.scriptName is "truck_tatra.ini"
    // Purchase_FindVehicleTypeByScriptName recovers the true type t2
    BYTE* recoveredType = Purchase_FindVehicleTypeByScriptName(world, "vehicles/truck_tatra.ini");
    TEST_CHECK(recoveredType == t2);
    TEST_CHECK(recoveredType != t0);

    return true;
}

static bool SubTest_CabinCategoryHandling()
{
    // Cable car cabin category is 7 (VEHICLETYPE_CABIN).
    // Categories 1, 4, 5 route to customs slots.
    // Categories 6, 8, 10 route to border points via PlaceRealisticVehicleAtBorder.
    // Category 7 MUST be excluded from border arrival logic to avoid navigation/aviation crashes.
    const unsigned catTrain = 1;
    const unsigned catRoad = 4;
    const unsigned catTram = 5;
    const unsigned catShip = 6;
    const unsigned catCabin = 7;
    const unsigned catPlane = 8;
    const unsigned catHeli = 10;

    auto IsCustomsSlotCategory = [](unsigned c) { return c == 1 || c - 4 <= 1; };
    auto IsBorderArrivalCategory = [](unsigned c) { return c == 6 || c == 8 || c == 10; };

    TEST_CHECK(IsCustomsSlotCategory(catTrain));
    TEST_CHECK(IsCustomsSlotCategory(catRoad));
    TEST_CHECK(IsCustomsSlotCategory(catTram));
    TEST_CHECK(!IsCustomsSlotCategory(catCabin));

    TEST_CHECK(IsBorderArrivalCategory(catShip));
    TEST_CHECK(IsBorderArrivalCategory(catPlane));
    TEST_CHECK(IsBorderArrivalCategory(catHeli));
    TEST_CHECK(!IsBorderArrivalCategory(catCabin));

    return true;
}

static bool SubTest_TelemetryCountersAndMemorySafety()
{
    Purchase_ResetDiagStatsForTest();
    VehicleSyncDiagStats stats = {};
    Purchase_GetDiagStats(&stats);
    TEST_CHECK(stats.purchasesAttempted == 0);
    TEST_CHECK(stats.purchasesMade == 0);
    TEST_CHECK(stats.vehiclesSold == 0);

    // Record sales across diverse vehicle categories
    Purchase_RecordDiagSale(1); // road (cat 1: ROAD)
    Purchase_RecordDiagSale(4); // rail (cat 4: RAIL_LOCOMOTIVE)
    Purchase_RecordDiagSale(6); // water (cat 6: SHIP)
    Purchase_RecordDiagSale(8); // air (cat 8: AIRPLANE)
    Purchase_RecordDiagSale(7); // special / cabin (cat 7: CABIN)

    Purchase_GetDiagStats(&stats);
    TEST_CHECK(stats.vehiclesSold == 5);
    TEST_CHECK(stats.countRoad == 1);
    TEST_CHECK(stats.countRail == 1);
    TEST_CHECK(stats.countWater == 1);
    TEST_CHECK(stats.countAir == 1);
    TEST_CHECK(stats.countSpecial == 1);

    // Memory Safety Probes: nullptr and unmapped pointers must not crash
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(nullptr));
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed((const void*)0x1234));
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(nullptr, "test.ini") == nullptr);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(g_mockExeBuffer + WORLD_RVA, nullptr) == nullptr);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(g_mockExeBuffer + WORLD_RVA, (const char*)0x1234) == nullptr);

    // Direct test of NormalizeVehicleScriptName
    {
        char out[64] = {};
        NormalizeVehicleScriptName("media_soviet\\dlc2\\vehicles\\bus.ini", out, sizeof(out));
        TEST_CHECK(strcmp(out, "dlc2/vehicles/bus.ini") == 0);
        NormalizeVehicleScriptName("MEDIA_SOVIET/VEHICLES/CAR.INI", out, sizeof(out));
        TEST_CHECK(strcmp(out, "vehicles/car.ini") == 0);
    }

    // Defense-in-depth: Malformed network packet with 64 non-null bytes in scriptName
    {
        std::vector<BYTE> rawPacket(208, 0xAA);
        memset(rawPacket.data() + rawPacket.size() - 64, 'X', 64);
        TEST_CHECK(!Purchase_ApplyCommand(rawPacket.data(), rawPacket.size()));
    }

    return true;
}

static bool Test_VehicleDlcFilter_ScriptName_DiagLog()
{
    return SubTest_DlcTokenExtractionAndMasks() &&
           SubTest_VehicleDlcSessionValidation() &&
           SubTest_TrailerDlcValidation() &&
           SubTest_ScriptNameResolutionWithShiftedIndices() &&
           SubTest_CabinCategoryHandling() &&
           SubTest_TelemetryCountersAndMemorySafety();
}

// ---------------------------------------------------------------------------
// Test 13: Train/Wagon Cargo Soft-Sync, Building Storage Healing & Vehicle Window Actions
// ---------------------------------------------------------------------------

static bool SubTest_StoppedTrainAndWagonEffectiveBld()
{
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld.data() = 64;

    std::vector<BYTE> truck(0x1B00, 0);
    *(BYTE**)(truck.data() + VEH_CUR_BLD_OFFSET) = bld.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(truck.data()) == bld.data());

    std::vector<BYTE> loco(0x1B00, 0);
    *(BYTE**)(loco.data() + VEH_STOPPED_BLD_OFFSET) = bld.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(loco.data()) == bld.data());

    std::vector<BYTE> wagon(0x1B00, 0);
    *(BYTE**)(wagon.data() + VEH_LINK_PTR_OFFSET) = loco.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(wagon.data()) == bld.data());

    *(BYTE**)(wagon.data() + VEH_LINK_PTR_OFFSET) = nullptr;
    *(BYTE**)(wagon.data() + VEH_PARENT_PTR_OFFSET) = loco.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(wagon.data()) == bld.data());

    *(BYTE**)(loco.data() + VEH_STOPPED_BLD_OFFSET) = nullptr;
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(wagon.data()) == nullptr);
    return true;
}

static bool SubTest_BuildingStorageAndWagonLoadingSoftSync()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Setup 3 canonical resources: iron, coal, steel
    std::vector<BYTE> resTable(WORLD_RESOURCE_STRIDE * 3, 0);
    const char* resIron  = (const char*)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    const char* resCoal  = (const char*)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);
    const char* resSteel = (const char*)(resTable.data() + 2 * WORLD_RESOURCE_STRIDE);
    strcpy_s((char*)resIron, 32, "iron");
    strcpy_s((char*)resCoal, 32, "coal");
    strcpy_s((char*)resSteel, 32, "steel");
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET)     = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 8) = resTable.data() + resTable.size();

    // 2. Setup Bld [64] steel_mill_v2 with 3 storage slots (0: empty on Host, 1: 120t coal on Host, 2: 10t steel on Host)
    std::vector<BYTE> bld64(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld64.data() = 64;
    *(float*)(bld64.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(float*)(bld64.data() + BLD_PRODUCED_OFFSET) = 10.0f;

    std::vector<BYTE> slots(BLD_STORAGE_SLOT_SIZE * 3, 0);
    std::vector<BYTE> slotEntries(0x10 * 3, 0);
    // Slot 0: empty on Host (sBeg == sEnd, sCap = sBeg + 16)
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x00) = slotEntries.data() + 0x00;
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x00;
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x10) = slotEntries.data() + 0x10;
    // Slot 1: 120t coal on Host
    *(const char**)(slotEntries.data() + 0x10) = resCoal;
    *(float*)(slotEntries.data() + 0x18) = 120.0f;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x00) = slotEntries.data() + 0x10;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x20;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x10) = slotEntries.data() + 0x20;
    // Slot 2: 0t steel on Host (empty)
    *(BYTE**)(slots.data() + 2 * BLD_STORAGE_SLOT_SIZE + 0x00) = slotEntries.data() + 0x20;
    *(BYTE**)(slots.data() + 2 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x20;
    *(BYTE**)(slots.data() + 2 * BLD_STORAGE_SLOT_SIZE + 0x10) = slotEntries.data() + 0x30;

    *(BYTE**)(bld64.data() + BLD_STORAGE_OFFSET)     = slots.data();
    *(BYTE**)(bld64.data() + BLD_STORAGE_OFFSET + 8) = slots.data() + slots.size();
    BYTE* bldList[1] = { bld64.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 1;

    // 3. Setup stopped locomotive #32 + coupled wagon #34 at Bld [64], plus moving truck #38
    std::vector<BYTE> loco(0x1B00, 0), wagon(0x1B00, 0), truck(0x1B00, 0);
    *(unsigned*)(loco.data() + VEH_ID_OFFSET) = 32;
    *(BYTE**)(loco.data() + VEH_STOPPED_BLD_OFFSET) = bld64.data();
    *(unsigned*)(wagon.data() + VEH_ID_OFFSET) = 34;
    *(BYTE**)(wagon.data() + VEH_LINK_PTR_OFFSET) = loco.data();
    *(unsigned*)(truck.data() + VEH_ID_OFFSET) = 38;
    *(float*)(truck.data() + 0xD24) = 42.0f; // moving

    std::vector<BYTE> wagonCargo(0x10, 0);
    *(const char**)wagonCargo.data() = resSteel;
    *(float*)(wagonCargo.data() + 0x08) = 10.0f; // Host wagon has 10.0t steel
    *(BYTE**)(wagon.data() + VEH_CARGO_OFFSET)     = wagonCargo.data();
    *(BYTE**)(wagon.data() + VEH_CARGO_OFFSET + 8) = wagonCargo.data() + 0x10;

    BYTE* vehList[3] = { loco.data(), wagon.data(), truck.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET)     = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 3;

    // 4. Build periodic soft-sync batch (immediate = false): stopped loco & wagon MUST be skipped!
    std::vector<BYTE> wire(8192, 0);
    size_t wireBytes = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, false));
    TEST_CHECK(*(const unsigned*)wire.data() == 1u); // only moving truck #38!

    // 5. Build immediate soft-sync batch (immediate = true): includes all 3 vehicles + Bld [64] storage
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, true));
    TEST_CHECK(*(const unsigned*)wire.data() == 3u);

    // 6. Simulate Client drift:
    //    - Slot 0 on Client has phantom 150.94t iron (sEnd = sBeg + 16)
    //    - Slot 1 on Client is empty (sEnd = sBeg)
    //    - Wagon #34 on Client is actively loading at 10.5t steel (only 0.5t ahead of Host's 10.0t)
    *(const char**)(slotEntries.data() + 0x00) = resIron;
    *(float*)(slotEntries.data() + 0x08) = 150.94f;
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x10;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x10; // empty on Client
    *(float*)(wagonCargo.data() + 0x08) = 10.5f;

    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireBytes));

    // Verify Client Slot 0 had its storedTons zeroed while preserving sEnd (slot + 8), and Slot 1 was populated with 120t coal!
    TEST_CHECK(*(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x08) == slotEntries.data() + 0x10);
    TEST_CHECK_NEAR(*(float*)(slotEntries.data() + 0x08), 0.0f, 1e-4f);
    TEST_CHECK(*(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x08) == slotEntries.data() + 0x20);
    TEST_CHECK(*(const char**)(slotEntries.data() + 0x10) == resCoal);
    TEST_CHECK_NEAR(*(float*)(slotEntries.data() + 0x18), 120.0f, 1e-4f);

    // Verify actively loading wagon #34 was NOT jittered back from 10.5t to 10.0t while stopped at Bld [64]
    TEST_CHECK_NEAR(*(float*)(wagonCargo.data() + 0x08), 10.5f, 1e-4f);

    // 7. Now test phantom cargo on stopped wagon when Host has 0.0t and Client has 0.24t:
    *(float*)(wagonCargo.data() + 0x08) = 0.0f;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, true));
    *(float*)(wagonCargo.data() + 0x08) = 0.24f; // Client has phantom 0.24t
    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireBytes));
    TEST_CHECK_NEAR(*(float*)(wagonCargo.data() + 0x08), 0.0f, 1e-4f);

    VehicleSoftSyncDiagStats vss = {};
    VehicleSoftSync_GetDiagStats(&vss);
    TEST_CHECK(vss.bldStorageUpdated >= 2);
    return true;
}

static bool SubTest_VehicleWindowScheduleAndLaunchCommands()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. AddBorderStopForTest on Airplane (cat == 8):
    std::vector<BYTE> planeType(0x400, 0), plane(0x1B00, 0), borderBld(0x400, 0);
    *(int*)(planeType.data() + VEH_TYPE_CATEGORY_OFFSET) = 8; // AIRPLANE
    *(BYTE**)(plane.data() + VEH_TYPE_PTR_OFFSET) = planeType.data();
    *(int*)(plane.data() + 0x1794) = 1;

    std::vector<BYTE> stopRecs(VEH_STOP_RECORD_STRIDE * 2, 0xFF);
    // First stop (nStops == 1): aircraft first stop must set load=0, unload=0
    *(BYTE**)(plane.data() + VEH_STOPS_LIST_OFFSET)     = stopRecs.data();
    *(BYTE**)(plane.data() + VEH_STOPS_LIST_OFFSET + 8) = stopRecs.data() + VEH_STOP_RECORD_STRIDE;
    GameCalls_AddBorderStopForTest((unsigned long long)world, (unsigned long long)plane.data(),
                                   (unsigned long long)borderBld.data(), -1);
    TEST_CHECK(stopRecs[0] == 0 && stopRecs[VEH_STOP_HALF_STRIDE] == 0);

    // Second stop (nStops == 2) with "passengers" in carried summary (+0x458): sets load=1, unload=1
    const char* passName = "passengers";
    BYTE carriedEntry[16] = {};
    *(const char**)carriedEntry = passName;
    *(BYTE**)(plane.data() + 0x458) = carriedEntry;
    *(BYTE**)(plane.data() + 0x460) = carriedEntry + 16;
    *(BYTE**)(plane.data() + VEH_STOPS_LIST_OFFSET + 8) = stopRecs.data() + 2 * VEH_STOP_RECORD_STRIDE;
    GameCalls_AddBorderStopForTest((unsigned long long)world, (unsigned long long)plane.data(),
                                   (unsigned long long)borderBld.data(), -1);
    TEST_CHECK(stopRecs[VEH_STOP_RECORD_STRIDE + 0] == 1);
    TEST_CHECK(stopRecs[VEH_STOP_RECORD_STRIDE + VEH_STOP_HALF_STRIDE] == 1);

    // 2. DeleteStopForTest + ChangeCurrentStopForTest on Train (cat == 4):
    std::vector<BYTE> trainType(0x400, 0), train(0x1B00, 0), stationA(0x100, 0), stationB(0x100, 0);
    *(int*)(trainType.data() + VEH_TYPE_CATEGORY_OFFSET) = 4; // RAIL_LOCOMOTIVE
    *(BYTE**)(train.data() + VEH_TYPE_PTR_OFFSET) = trainType.data();
    BYTE* stopBlds[2] = { stationA.data(), stationB.data() };
    *(BYTE***)(train.data() + VEH_STOP_BLD_PTR_VEC_OFF)     = stopBlds;
    *(BYTE***)(train.data() + VEH_STOP_BLD_PTR_VEC_OFF + 8) = stopBlds + 2;

    // In depot (+0x528 == 0 && +0x530 == 0): ChangeCurrentStop updates +0x698, +0xD3A, +0xD3B
    GameCalls_ChangeCurrentStopForTest((unsigned long long)train.data(), 1);
    TEST_CHECK(*(int*)(train.data() + VEH_SCHED_IDX_OFFSET) == 1);
    TEST_CHECK(train[0xD3A] == 1 && train[0xD3B] == 1);

    // Out of depot (+0x530 == stationA): ChangeCurrentStop updates +0x610 to stationB
    *(BYTE**)(train.data() + VEH_STOPPED_BLD_OFFSET) = stationA.data();
    GameCalls_ChangeCurrentStopForTest((unsigned long long)train.data(), 1);
    TEST_CHECK(*(BYTE**)(train.data() + VEH_TARGET_STOP_BLD_OFF) == stationB.data());

    // DeleteStopForTest on stop 0 (stationA): clears +0x530 since it matched stationA
    train[0xD3A] = 0;
    GameCalls_DeleteStopForTest((unsigned long long)world, (unsigned long long)train.data(), 0);
    TEST_CHECK(train[0xD3A] == 1);
    TEST_CHECK(*(BYTE**)(train.data() + VEH_STOPPED_BLD_OFFSET) == nullptr);

    // 3. ReleaseVehicleForTest on Ship (cat == 7) and Cableway (cat == 6):
    std::vector<BYTE> shipType(0x400, 0), ship(0x1B00, 0), dock(0x100, 0);
    *(int*)(shipType.data() + VEH_TYPE_CATEGORY_OFFSET) = 7; // SHIP in 0x7DA236
    *(BYTE**)(ship.data() + VEH_TYPE_PTR_OFFSET) = shipType.data();
    *(BYTE**)(ship.data() + VEH_CUR_BLD_OFFSET) = dock.data();
    GameCalls_ReleaseVehicleForTest((unsigned long long)ship.data());
    TEST_CHECK(*(BYTE**)(ship.data() + VEH_CUR_BLD_OFFSET) == nullptr);

    std::vector<BYTE> cableType(0x400, 0), cableVeh(0x1B00, 0);
    *(int*)(cableType.data() + VEH_TYPE_CATEGORY_OFFSET) = 6; // CABLEWAY in 0x7DA236
    *(BYTE**)(cableVeh.data() + VEH_TYPE_PTR_OFFSET) = cableType.data();
    GameCalls_ReleaseVehicleForTest((unsigned long long)cableVeh.data());
    TEST_CHECK(cableVeh[VEH_CABLEWAY_START_FLAG_OFF] == 1);

    return true;
}

static bool SubTest_MultiResourceStorageSoftSyncAndManualBuy()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Setup 6 construction/warehouse resources with USD (+0x58) and RUB (+0x5C) prices
    const char* const kResNames[6] = { "gravel", "steel", "bricks", "boards", "prefabpanels", "cement" };
    const float kRubPrices[6]      = { 20.0f, 300.0f, 100.0f, 80.0f, 150.0f, 120.0f };
    const float kHostTons[6]       = { 45.0f,  30.0f,  60.0f, 25.0f,  80.0f,  50.0f };
    std::vector<BYTE> resTable(WORLD_RESOURCE_STRIDE * 6, 0);
    const char* resPtrs[6] = {};
    for (int i = 0; i < 6; ++i) {
        BYTE* r = resTable.data() + (size_t)i * WORLD_RESOURCE_STRIDE;
        strcpy_s((char*)r, 32, kResNames[i]);
        *(float*)(r + RES_PRICE_USD_OFFSET) = kRubPrices[i] * 0.5f;
        *(float*)(r + RES_PRICE_RUB_OFFSET) = kRubPrices[i];
        resPtrs[i] = (const char*)r;
    }
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET)     = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 8) = resTable.data() + resTable.size();

    // 2. Setup multi-resource warehouse building #88 with 1 storage slot containing 6 resource entries
    std::vector<BYTE> bldType(0x400, 0), bld88(BUILDING_STRUCT_BYTES, 0);
    strcpy_s((char*)bldType.data(), 64, "storage_general");
    *(int*)bld88.data() = 88;
    *(BYTE**)(bld88.data() + BLD_TYPE_OFFSET) = bldType.data();
    *(float*)(bld88.data() + OBJ_POS_OFFSET) = -300.0f;
    *(float*)(bld88.data() + BLD_PROGRESS_OFFSET) = 1.0f;

    std::vector<BYTE> slotBuf(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> entriesBuf(0x10 * 6, 0);
    for (int i = 0; i < 6; ++i) {
        BYTE* e = entriesBuf.data() + (size_t)i * 0x10;
        *(const char**)e    = resPtrs[i];
        *(float*)(e + 0x08) = kHostTons[i];
        *(float*)(e + 0x0C) = 25.0f; // autoPurchaseTons slider target
    }
    *(BYTE**)(slotBuf.data() + 0x00) = entriesBuf.data();
    *(BYTE**)(slotBuf.data() + 0x08) = entriesBuf.data() + entriesBuf.size();
    *(BYTE**)(slotBuf.data() + 0x10) = entriesBuf.data() + entriesBuf.size();
    *(BYTE**)(bld88.data() + BLD_STORAGE_OFFSET)     = slotBuf.data();
    *(BYTE**)(bld88.data() + BLD_STORAGE_OFFSET + 8) = slotBuf.data() + slotBuf.size();

    BYTE* bldList[1] = { bld88.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 1;

    // 3. Build VehicleSoftSync packet on Host, then simulate Client having 0.0f storedTons and 25.0f autoPurchaseTons
    std::vector<BYTE> wire(8192, 0);
    size_t wireBytes = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, true));

    for (int i = 0; i < 6; ++i) {
        BYTE* e = entriesBuf.data() + (size_t)i * 0x10;
        *(float*)(e + 0x08) = 0.0f;
        *(float*)(e + 0x0C) = 25.0f;
    }
    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireBytes));

    // Verify all 6 entries are preserved (no truncation to 2), storedTons match Host, and autoPurchaseTons stay 25.0f!
    size_t finalEntryCount = (size_t)((*(BYTE**)(slotBuf.data() + 0x08) - *(BYTE**)(slotBuf.data() + 0x00)) / 0x10);
    TEST_CHECK(finalEntryCount == 6);
    for (int i = 0; i < 6; ++i) {
        const BYTE* e = entriesBuf.data() + (size_t)i * 0x10;
        TEST_CHECK_NEAR(*(const float*)(e + 0x08), kHostTons[i], 1e-4f);
        TEST_CHECK_NEAR(*(const float*)(e + 0x0C), 25.0f, 1e-4f);
    }

    // 4. Verify BuildingSwitches slot offset filtering:
    //    - Region 0 dynamic header (< 0x48, e.g. 0x38 + 8) -> true (excluded)
    //    - Region 1 (+0x1000) storedTons (+0x08) -> true (excluded so UI never reverts soft-synced stock)
    //    - Region 1 (+0x1000) autoPurchaseTons (+0x0C) -> false (carried as slider setting)
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(0x38 + 8));
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 0 * 16 + 8));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 0 * 16 + 12));
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 3 * 16 + 8));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 3 * 16 + 12));

    // 5. Verify BuildingSwitches_CaptureManualBuy + Lockstep replay (ApplyManualBuyFallback):
    for (int i = 0; i < 6; ++i) {
        *(float*)(entriesBuf.data() + (size_t)i * 0x10 + 0x08) = 0.0f;
    }
    *(int*)(bld88.data() + BLD_EXPORT_MODE_OFFSET) = 1;
    *(double*)(world + WORLD_RUB_OFFSET) = 100000.0;

    std::vector<BYTE> uiSliderSlot(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> uiSliderEntries(0x10 * 3, 0);
    *(float*)(uiSliderEntries.data() + 0 * 0x10 + 0x08) = 10.0f; // 10t gravel @ 20 RUB = 200 RUB
    *(float*)(uiSliderEntries.data() + 1 * 0x10 + 0x08) =  0.0f; // 0t steel
    *(float*)(uiSliderEntries.data() + 2 * 0x10 + 0x08) =  5.0f; // 5t bricks @ 100 RUB = 500 RUB
    *(BYTE**)(uiSliderSlot.data() + 0x00) = uiSliderEntries.data();
    *(BYTE**)(uiSliderSlot.data() + 0x08) = uiSliderEntries.data() + uiSliderEntries.size();
    *(BYTE**)(world + WORLD_UI_RES_SLIDERS_OFFSET + 0x00) = uiSliderSlot.data();
    *(BYTE**)(world + WORLD_UI_RES_SLIDERS_OFFSET + 0x08) = uiSliderSlot.data() + uiSliderSlot.size();

    Commands_SetSession(COMMANDS_HOST);
    TEST_CHECK(BuildingSwitches_CaptureManualBuy((unsigned long long)world, (unsigned long long)bld88.data(), 4 /*RUB*/));
    Commands_Boundary(1);
    Commands_SetSession(COMMANDS_OFF);

    TEST_CHECK_NEAR(*(float*)(entriesBuf.data() + 0 * 0x10 + 0x08), 10.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(entriesBuf.data() + 1 * 0x10 + 0x08),  0.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(entriesBuf.data() + 2 * 0x10 + 0x08),  5.0f, 1e-4f);
    TEST_CHECK(*(int*)(bld88.data() + BLD_EXPORT_MODE_OFFSET) == 0);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 99300.0, 1e-2);

    return true;
}

static bool SubTest_CoopGameSpeedAndTimerScaling()
{
    ResetMockWorldMemory();
    *(float*)(g_mockExeBuffer + 0x909D64) = 0.35f; // FAST_FACTOR_RVA (2x)
    *(float*)(g_mockExeBuffer + 0x909C14) = 0.05f; // HYPER_FACTOR_RVA (LCtrl+Numpad1)
    *(float*)(g_mockExeBuffer + 0x909B90) = 0.01f; // PLAID_FACTOR_RVA (LCtrl+Numpad2 / [>>>] held)
    *(uint8_t*)(g_mockExeBuffer + 0x9D4EE0 + 13) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0x9D4F10 + 0x5A2) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x1D) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x4F) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x50) = 0;

    // 1. Activate determinism and run initial Prepare: buttons armed, speed 1 (30.0f)
    Determinism_SetActiveForTest(true);
    Determinism_BeforePrepare();
    bool paused = true;
    int speed = 0;
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 1);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0x9D4EE0), 30.0f, 1e-4f);

    // 2. Click UI Speed 2 ([>>]) and verify a second UI frame before Prepare does not clobber it
    Determinism_UiSpeedBegin();
    *(int*)(g_mockExeBuffer + 0x9D54B4) = 2;
    *(int*)(g_mockExeBuffer + 0x9D54BC) = 0;
    Determinism_UiSpeedEnd();

    Determinism_UiSpeedBegin();
    Determinism_UiSpeedEnd();

    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 2);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0x9D4EE0), 10.5f, 1e-4f);

    // 3. Hold 4th UI speed button ([>>>]) across two UI frames without touching 0x9D54BC:
    //    must never spam-toggle pause, and must set Plaid (speed 4 -> 30.0f * 0.01f = 0.3f)
    for (int f = 0; f < 2; ++f) {
        Determinism_UiSpeedBegin();
        *(int*)(g_mockExeBuffer + 0x9D54B4) = 2;
        *(uint8_t*)(g_mockExeBuffer + 0x9D4F10 + 0x5A2) = 1;
        Determinism_UiSpeedEnd();
    }
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 4);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0x9D4EE0), 0.3f, 1e-4f);

    // 4. Release [>>>], switch to Speed 1 ([>]), and hold LCtrl + Numpad1:
    //    must boost to Hyper (speed 3 -> 30.0f * 0.05f = 1.5f) even from requested speed 1
    *(uint8_t*)(g_mockExeBuffer + 0x9D4F10 + 0x5A2) = 0;
    Determinism_UiSpeedBegin();
    *(int*)(g_mockExeBuffer + 0x9D54B4) = 1;
    *(int*)(g_mockExeBuffer + 0x9D54BC) = 0;
    Determinism_UiSpeedEnd();

    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x1D) = 1;
    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x4F) = 1;
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 3);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0x9D4EE0), 1.5f, 1e-4f);

    // 5. GameSpeed_OnRemote while Determinism_IsActive() must not overwrite 0x9D54BC (kSpeedSentinel)
    int sentinelBefore = *(int*)(g_mockExeBuffer + 0x9D54BC);
    GameSpeed_OnRemote(0, 1, 1, 0);
    TEST_CHECK(*(int*)(g_mockExeBuffer + 0x9D54BC) == sentinelBefore);

    // 6. Clean up
    *(uint8_t*)(g_mockExeBuffer + 0x9D4F10 + 0x5A2) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x1D) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x4F) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA54B90 + 0x50) = 0;
    Determinism_SetActiveForTest(false);
    return true;
}

static bool Test_VehicleCargoAndScheduleSync()
{
    return SubTest_StoppedTrainAndWagonEffectiveBld() &&
           SubTest_BuildingStorageAndWagonLoadingSoftSync() &&
           SubTest_VehicleWindowScheduleAndLaunchCommands() &&
           SubTest_MultiResourceStorageSoftSyncAndManualBuy() &&
           SubTest_CoopGameSpeedAndTimerScaling();
}

// ---------------------------------------------------------------------------
// Test 14: Per-Player Camera Persistence, Resync Restore & Initial Framing
// ---------------------------------------------------------------------------

static bool Test_CameraPersistenceAndFraming()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> mockCamMgr(CAM_MGR_STRUCT_BYTES, 0);
    *(BYTE**)(world + WORLD_CAMERA_MGR_PTR_OFF) = mockCamMgr.data();

    // Put dirty follow/transition state in camMgr to verify TwoStates_ApplyCameraView clears snapback triggers
    mockCamMgr[CAM_MGR_FOLLOW_ACTIVE_OFF] = 1;
    *(void**)(mockCamMgr.data() + CAM_MGR_RIDE_VEH_PTR_OFF) = (void*)0x12345678ULL;
    *(float*)(mockCamMgr.data() + CAM_MGR_TRANS_TIMER_OFFSET) = 2.5f;
    *(float*)(mockCamMgr.data() + CAM_MGR_TRANS_DUR_OFFSET) = 5.0f;

    std::filesystem::path camSaveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save_cam_test";
    std::error_code ec;
    std::filesystem::remove_all(camSaveDir, ec);
    std::filesystem::create_directories(camSaveDir, ec);

    // 1. First load without saved camera metadata -> initial central framing facing each player's territory
    double initRub[2] = { 500000.0, 500000.0 }, initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    TwoStates_RestoreOrFrameCameraForTest(true);
    float hostPos[3] = {}, hostLook[3] = {};
    TEST_CHECK(TwoStates_GetCameraView(hostPos, hostLook));
    TEST_CHECK_NEAR(hostPos[0], -80.0f, 1e-2f);
    TEST_CHECK_NEAR(hostPos[2], 0.0f, 1e-2f);
    TEST_CHECK(hostLook[0] < hostPos[0]); // Host looks West (X < 0)
    TEST_CHECK_NEAR(hostLook[0], -450.0f, 1e-2f);
    // Verify camMgr was synchronized and snapback triggers were cleared
    TEST_CHECK(mockCamMgr[CAM_MGR_FOLLOW_ACTIVE_OFF] == 0);
    TEST_CHECK(*(void**)(mockCamMgr.data() + CAM_MGR_RIDE_VEH_PTR_OFF) == nullptr);
    TEST_CHECK_NEAR(*(float*)(mockCamMgr.data() + CAM_MGR_TRANS_TIMER_OFFSET), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(mockCamMgr.data() + CAM_MGR_POS_OFFSET), -80.0f, 1e-2f);
    TEST_CHECK_NEAR(*(float*)(mockCamMgr.data() + CAM_MGR_CUR_POS_OFFSET), -80.0f, 1e-2f);

    TwoStates_RestoreOrFrameCameraForTest(false);
    float clientPos[3] = {}, clientLook[3] = {};
    TEST_CHECK(TwoStates_GetCameraView(clientPos, clientLook));
    TEST_CHECK_NEAR(clientPos[0], 80.0f, 1e-2f);
    TEST_CHECK_NEAR(clientPos[2], 0.0f, 1e-2f);
    TEST_CHECK(clientLook[0] > clientPos[0]); // Client looks East (X > 0)
    TEST_CHECK_NEAR(clientLook[0], 450.0f, 1e-2f);

    // 2. Save both players' custom cameras in republicmp_coop.ini and verify round-trip restore
    const float customCam1Pos[3]  = { -1234.56f, 210.25f, 456.78f };
    const float customCam1Look[3] = { -1550.00f,  25.50f, 600.00f };
    const float customCam2Pos[3]  = {  2345.67f, 195.75f, -789.12f };
    const float customCam2Look[3] = {  2700.00f,  32.00f, -900.00f };

    TEST_CHECK(TwoStates_ApplyCameraView(customCam1Pos, customCam1Look));
    TwoStates_SetSavedCameraForTest(2, customCam2Pos, customCam2Look);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(camSaveDir.string().c_str()));

    // Overwrite live camera with unrelated coordinates, then read save metadata back
    const float dummyPos[3] = { 10.0f, 50.0f, 10.0f }, dummyLook[3] = { 20.0f, 20.0f, 20.0f };
    TEST_CHECK(TwoStates_ApplyCameraView(dummyPos, dummyLook));
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(camSaveDir.string().c_str(), true));

    float readCam1Pos[3] = {}, readCam1Look[3] = {};
    float readCam2Pos[3] = {}, readCam2Look[3] = {};
    TEST_CHECK(TwoStates_GetSavedCameraForTest(1, readCam1Pos, readCam1Look));
    TEST_CHECK(TwoStates_GetSavedCameraForTest(2, readCam2Pos, readCam2Look));
    TEST_CHECK_NEAR(readCam1Pos[0], customCam1Pos[0], 0.02f);
    TEST_CHECK_NEAR(readCam1Look[0], customCam1Look[0], 0.02f);
    TEST_CHECK_NEAR(readCam2Pos[0], customCam2Pos[0], 0.02f);
    TEST_CHECK_NEAR(readCam2Look[0], customCam2Look[0], 0.02f);

    // Host restore applies Cam1; Client restore applies Cam2
    TwoStates_RestoreOrFrameCameraForTest(true);
    TEST_CHECK(TwoStates_GetCameraView(hostPos, hostLook));
    TEST_CHECK_NEAR(hostPos[0], customCam1Pos[0], 0.02f);
    TEST_CHECK_NEAR(hostLook[2], customCam1Look[2], 0.02f);

    TwoStates_RestoreOrFrameCameraForTest(false);
    TEST_CHECK(TwoStates_GetCameraView(clientPos, clientLook));
    TEST_CHECK_NEAR(clientPos[0], customCam2Pos[0], 0.02f);
    TEST_CHECK_NEAR(clientLook[2], customCam2Look[2], 0.02f);

    // 3. Pre-resync camera capture and restore takes priority over saved meta camera once
    const float resyncPos[3]  = { 777.0f, 240.0f, -333.0f };
    const float resyncLook[3] = { 999.0f,  30.0f, -555.0f };
    TEST_CHECK(TwoStates_ApplyCameraView(resyncPos, resyncLook));
    TwoStates_CapturePreResyncCamera();
    TEST_CHECK(TwoStates_HasCapturedResyncCamera());

    TEST_CHECK(TwoStates_ApplyCameraView(dummyPos, dummyLook));
    TwoStates_RestoreOrFrameCameraForTest(false);
    TEST_CHECK(!TwoStates_HasCapturedResyncCamera());
    TEST_CHECK(TwoStates_GetCameraView(clientPos, clientLook));
    TEST_CHECK_NEAR(clientPos[0], resyncPos[0], 1e-2f);
    TEST_CHECK_NEAR(clientLook[2], resyncLook[2], 1e-2f);

    *(BYTE**)(world + WORLD_CAMERA_MGR_PTR_OFF) = nullptr;
    TwoStates_OnSessionEnded();
    std::filesystem::remove_all(camSaveDir, ec);
    return true;
}

// ---------------------------------------------------------------------------
// Test 15: Remote Sound Isolation, Warning Banner Restore & Notification Filter
// ---------------------------------------------------------------------------

static int g_mockPlaySound2DCalls = 0;
static int g_mockPlaySound3DCalls = 0;
static int g_mockCreatePopupCalls = 0;
static int g_mockRebuildNotifCalls = 0;

static bool __fastcall MockOrig_PlaySound2D(void* mgr, const char* name, unsigned char loop, float vol, unsigned char p5)
{
    (void)mgr; (void)name; (void)loop; (void)vol; (void)p5;
    ++g_mockPlaySound2DCalls;
    return true;
}

static bool __fastcall MockOrig_PlaySound3D(void* mgr, const char* name, const float* pos, unsigned char loop, float param5)
{
    (void)mgr; (void)name; (void)pos; (void)loop; (void)param5;
    ++g_mockPlaySound3DCalls;
    return true;
}

static long long __fastcall MockOrig_CreatePopupWindow(long long world, int kind, long long ptr, int subkind, long long p5,
                                                       int popupType, int stringId, long long p8, long long p9)
{
    (void)world; (void)kind; (void)ptr; (void)subkind; (void)p5;
    (void)popupType; (void)stringId; (void)p8; (void)p9;
    ++g_mockCreatePopupCalls;
    return 0x7777;
}

static void __fastcall MockOrig_RebuildNotificationPopups(long long world)
{
    (void)world;
    ++g_mockRebuildNotifCalls;
}

static int MockHost_PatchIatForCommands(void*, const char*, const char* fn, void*, void** outOrig, const char*)
{
    if (!fn || !outOrig) return 0;
    if (strstr(fn, "PlaySound2D") != nullptr) {
        *outOrig = (void*)&MockOrig_PlaySound2D;
        return 1;
    }
    if (strstr(fn, "PlaySound3D") != nullptr) {
        *outOrig = (void*)&MockOrig_PlaySound3D;
        return 1;
    }
    return 0;
}

static int MockHost_InstallInlineHookForCommands(void* target, void*, void** outTramp, const unsigned char*, size_t, const char*)
{
    if (!target || !outTramp) return 0;
    if (target == (void*)(g_mockExeBuffer + WARNING_POPUP_CREATE_RVA)) {
        *outTramp = (void*)&MockOrig_CreatePopupWindow;
        return 1;
    }
    if (target == (void*)(g_mockExeBuffer + NOTIFICATION_REBUILD_POPUPS_RVA)) {
        *outTramp = (void*)&MockOrig_RebuildNotificationPopups;
        return 1;
    }
    return 0;
}

static bool SubTest_RemoteReplayScopeAndSnapshots()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    while (Commands_IsApplyingRemote()) {
        Commands_EndRemoteReplayScope();
    }
    TEST_CHECK(!Commands_IsApplyingRemote());

    // Set initial local state before remote command replay
    *(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    strcpy_s((char*)(world + WORLD_WARNING_TEXT_OFFSET), WORLD_WARNING_TEXT_BYTES, "Local warning");
    *(float*)(world + WORLD_WARNING_TIMER_OFFSET) = 2.5f;
    *(int32_t*)(world + WORLD_PLACEMENT_ERROR_OFFSET) = 0;

    void* dummyWinList[2] = { (void*)0x11110000ULL, (void*)0x22220000ULL };
    *(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_BEGIN_RVA) = dummyWinList;
    *(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_END_RVA)   = dummyWinList + 2;

    std::vector<BYTE> brushSoundBuf(2 * BRUSH_SOUND_REC_SIZE, 0);
    *(float*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 0.0f;
    *(uint8_t*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 0;
    *(float*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 0.35f;
    *(uint8_t*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 1;
    *(uintptr_t*)(world + WORLD_BRUSH_SOUNDS_BEGIN_OFFSET) = (uintptr_t)brushSoundBuf.data();
    *(uintptr_t*)(world + WORLD_BRUSH_SOUNDS_END_OFFSET)   = (uintptr_t)(brushSoundBuf.data() + brushSoundBuf.size());

    // Enter outer and nested remote replay scopes
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_IsApplyingRemote());
    TEST_CHECK(*(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 1);
    TEST_CHECK(*(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_END_RVA) == dummyWinList);

    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_IsApplyingRemote());

    // Simulate remote command overwriting warning banner, error code, and terrain brush loop sounds
    strcpy_s((char*)(world + WORLD_WARNING_TEXT_OFFSET), WORLD_WARNING_TEXT_BYTES, "> Remote error");
    *(float*)(world + WORLD_WARNING_TIMER_OFFSET) = 5.0f;
    *(int32_t*)(world + WORLD_PLACEMENT_ERROR_OFFSET) = 1;
    *(float*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 1.0f;
    *(uint8_t*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 1;
    *(float*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 1.0f;
    *(uint8_t*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 1;

    // Exit nested scope: still applying remote until outer scope exits
    Commands_EndRemoteReplayScope();
    TEST_CHECK(Commands_IsApplyingRemote());
    TEST_CHECK(*(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 1);

    // Exit outer scope: all local state must be restored
    Commands_EndRemoteReplayScope();
    TEST_CHECK(!Commands_IsApplyingRemote());
    TEST_CHECK(*(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 0);
    TEST_CHECK(strcmp((const char*)(world + WORLD_WARNING_TEXT_OFFSET), "Local warning") == 0);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_WARNING_TIMER_OFFSET), 2.5f, 1e-5f);
    TEST_CHECK(*(int32_t*)(world + WORLD_PLACEMENT_ERROR_OFFSET) == 0);
    TEST_CHECK(*(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_END_RVA) == dummyWinList + 2);
    TEST_CHECK_NEAR(*(float*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET), 0.0f, 1e-5f);
    TEST_CHECK(*(uint8_t*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) == 0);
    TEST_CHECK_NEAR(*(float*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET), 0.35f, 1e-5f);
    TEST_CHECK(*(uint8_t*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) == 1);

    return true;
}

static bool SubTest_Sound2DAnd3DDistanceHooks()
{
    ResetMockWorldMemory();
    g_mockHost.patchIat          = &MockHost_PatchIatForCommands;
    g_mockHost.installInlineHook = &MockHost_InstallInlineHookForCommands;
    Commands_Bind(&g_mockHost);
    TEST_CHECK(Commands_InstallHooks());
    g_mockHost.patchIat          = &MockHost_PatchIat;
    g_mockHost.installInlineHook = &MockHost_InstallInlineHook;

    g_mockPlaySound2DCalls = 0;
    g_mockPlaySound3DCalls = 0;

    // Set local camera eye at {1000.0f, 50.0f, 1000.0f}
    float* camEye = (float*)(g_mockExeBuffer + CAMERA_EYE_RVA);
    camEye[0] = 1000.0f;
    camEye[1] = 50.0f;
    camEye[2] = 1000.0f;

    const float nearPos[3] = { 1100.0f, 50.0f, 1000.0f }; // dist = 100m <= 250m
    const float farPos[3]  = { 2000.0f, 50.0f, 2000.0f }; // dist = 1414m > 250m

    // 1. Outside remote replay scope: 2D and both 3D sounds are forwarded
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "click", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 1);
    TEST_CHECK(Commands_OnPlaySound3D(nullptr, "build_near", nearPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 1);
    TEST_CHECK(Commands_OnPlaySound3D(nullptr, "build_far", farPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 2);

    // 2. Inside remote replay scope: 2D sound is muted; 3D near (100m <= 250m) is played; 3D far (> 250m) is muted
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(!Commands_OnPlaySound2D(nullptr, "remote_click", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 1);

    TEST_CHECK(Commands_OnPlaySound3D(nullptr, "remote_near", nearPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 3);

    TEST_CHECK(!Commands_OnPlaySound3D(nullptr, "remote_far", farPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 3);
    Commands_EndRemoteReplayScope();

    return true;
}

static bool SubTest_PopupAndForeignTerritoryNotificationFilter()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };

    // Create mock notification messages (size 0x4220 to cover NOTIF_MSG_POS_X/Z_OFFSET at +0x4200/+0x4208)
    std::vector<BYTE> msgWest(0x4220, 0), msgEast(0x4220, 0), msgGlobal(0x4220, 0);
    *(float*)(msgWest.data() + NOTIF_MSG_POS_X_OFFSET)   = -500.0f;
    *(float*)(msgWest.data() + NOTIF_MSG_POS_Z_OFFSET)   =  100.0f;
    *(float*)(msgEast.data() + NOTIF_MSG_POS_X_OFFSET)   = +500.0f;
    *(float*)(msgEast.data() + NOTIF_MSG_POS_Z_OFFSET)   =  100.0f;
    *(float*)(msgGlobal.data() + NOTIF_MSG_POS_X_OFFSET) =    0.0f;
    *(float*)(msgGlobal.data() + NOTIF_MSG_POS_Z_OFFSET) =    0.0f;

    // 1. One-State mode: remote replay scope blocks popups, outside scope all notifications are allowed
    TwoStates_OnSessionJoined(true, COOP_MODE_ONE_STATE, 250.0f, false, initRub, initUsd);
    g_mockCreatePopupCalls = 0;

    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 1, 0, 0, 0, 0, 100, 0, 0) == 0);
    TEST_CHECK(g_mockCreatePopupCalls == 0);
    Commands_EndRemoteReplayScope();

    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 2, (long long)msgEast.data(), 5, 0, 0, 0, 0, 0) == 0x7777);
    TEST_CHECK(g_mockCreatePopupCalls == 1);

    uintptr_t popupsOneState[3] = { (uintptr_t)msgWest.data(), (uintptr_t)msgEast.data(), (uintptr_t)msgGlobal.data() };
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_BEGIN_OFFSET) = popupsOneState;
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET)   = popupsOneState + 3;
    g_mockRebuildNotifCalls = 0;
    Commands_OnRebuildNotificationPopups((long long)world);
    TEST_CHECK(g_mockRebuildNotifCalls == 1);
    TEST_CHECK(*(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET) == popupsOneState + 3);

    // 2. Two-States mode (Host = State 1, West X < 0):
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    g_mockCreatePopupCalls = 0;

    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 1, 0, 0, 0, 0, 100, 0, 0) == 0);
    TEST_CHECK(g_mockCreatePopupCalls == 0);
    Commands_EndRemoteReplayScope();

    // East notification (+500.0f) suppressed; West notification (-500.0f) allowed
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 2, (long long)msgEast.data(), 5, 0, 0, 0, 0, 0) == 0);
    TEST_CHECK(g_mockCreatePopupCalls == 0);
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 2, (long long)msgWest.data(), 5, 0, 0, 0, 0, 0) == 0x7777);
    TEST_CHECK(g_mockCreatePopupCalls == 1);

    // RebuildNotificationPopups in Two-States mode removes East (+500.0f) and keeps West (-500.0f) + Global (0.0f)
    uintptr_t popupsTwoStates[3] = { (uintptr_t)msgEast.data(), (uintptr_t)msgWest.data(), (uintptr_t)msgGlobal.data() };
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_BEGIN_OFFSET) = popupsTwoStates;
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET)   = popupsTwoStates + 3;
    Commands_OnRebuildNotificationPopups((long long)world);
    uintptr_t* newEnd = *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET);
    TEST_CHECK((newEnd - popupsTwoStates) == 2);
    TEST_CHECK(popupsTwoStates[0] == (uintptr_t)msgWest.data());
    TEST_CHECK(popupsTwoStates[1] == (uintptr_t)msgGlobal.data());

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_CommandOwnerTrackingBothModes()
{
    ResetMockWorldMemory();
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };

    // 1. COOP_MODE_ONE_STATE
    TwoStates_OnSessionJoined(true, COOP_MODE_ONE_STATE, 250.0f, false, initRub, initUsd);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);
    TwoStates_BeginCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 1);
    TwoStates_EndCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    TwoStates_BeginCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 2);
    TwoStates_EndCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    // 2. COOP_MODE_TWO_STATES
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);
    TwoStates_BeginCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 1);
    TwoStates_EndCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    TwoStates_BeginCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 2);
    TwoStates_EndCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    TwoStates_OnSessionEnded();
    return true;
}

static bool Test_RemoteSoundAndWarningIsolation()
{
    return SubTest_RemoteReplayScopeAndSnapshots() &&
            SubTest_Sound2DAnd3DDistanceHooks() &&
            SubTest_PopupAndForeignTerritoryNotificationFilter() &&
            SubTest_CommandOwnerTrackingBothModes();
}

// ---------------------------------------------------------------------------
// Test Runner Entry Point
// ---------------------------------------------------------------------------

struct TestCase {
    const char* name;
    bool (*fn)();
};

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    printf("================================================================\n");
    printf(" RepublicMP Automated Unit & Integration Test Suite (v%s)\n", REPUBLICMP_VERSION);
    printf("================================================================\n");

    InitMockHarness();

    const TestCase kTests[] = {
        { "RvaAndSafeMem (Invariants #1, #12, #16, InitOnce)",   &Test_RvaAndSafeMem },
        { "WorldRefs (Buildings, Constructions, Vehicles, Net)", &Test_WorldRefs },
        { "StateHash (Determinism, Mutation, Corrupted Bounds)", &Test_StateHash },
        { "TwoStates_ZonesAndEconomy (Zones, Split, Inv #10)",   &Test_TwoStates_ZonesAndEconomy },
        { "TwoStates_SaveMetaAndScriptIni (Inv #4 & #5)",        &Test_TwoStates_SaveMetaAndScriptIniSafety },
        { "ModSync_HeaderBinAndWorkshopFilter (Inv #6 & #9)",    &Test_ModSync_HeaderBinAndWorkshopFilter },
        { "BuildingSwitches_Commands_SaveZip (Inv #12, Zip)",    &Test_BuildingSwitches_Commands_SaveZip },
        { "WaitingLobbyAndActionLock (Pause, Lock, Inspect)",    &Test_WaitingLobbyAndActionLock },
        { "TwoStates_Loans (Borrow Ceiling, UI Swap, Daily)",    &Test_TwoStates_Loans },
        { "TwoStates_EconomyStats (Ledgers, UI Swap, Rollover)", &Test_TwoStates_EconomyStats },
        { "SingleplayerIsolationAndReset (Dormancy, Reset, SP)", &Test_SingleplayerIsolationAndReset },
        { "VehicleDlcFilter_ScriptName_DiagLog (DLC, Remap, Diag)", &Test_VehicleDlcFilter_ScriptName_DiagLog },
        { "VehicleCargoAndScheduleSync (Train/BldSoftSync, Sched)", &Test_VehicleCargoAndScheduleSync },
        { "CameraPersistenceAndFraming (SaveMeta, Resync, Init)", &Test_CameraPersistenceAndFraming },
        { "RemoteSoundAndWarningIsolation (Sound2D/3D, Popups)", &Test_RemoteSoundAndWarningIsolation },
    };

    for (const auto& tc : kTests) {
        ++g_totalTests;
        printf("[RUN ] %s ...\n", tc.name);
        bool ok = tc.fn();
        if (ok) {
            ++g_passedTests;
            printf("[PASS] %s\n", tc.name);
        } else {
            printf("[FAIL] %s\n", tc.name);
        }
    }

    // Cleanup temporary test directory
    if (g_mockBaseDir[0]) {
        std::error_code ec;
        std::filesystem::remove_all(g_mockBaseDir, ec);
    }
    if (g_mockExeBuffer) {
        VirtualFree(g_mockExeBuffer, 0, MEM_RELEASE);
        g_mockExeBuffer = nullptr;
    }

    printf("================================================================\n");
    printf(" Result: %d / %d test suites passed (%d failed assertions)\n",
           g_passedTests, g_totalTests, g_failedAssertions);
    printf("================================================================\n");

    return (g_passedTests == g_totalTests && g_failedAssertions == 0) ? 0 : 1;
}
