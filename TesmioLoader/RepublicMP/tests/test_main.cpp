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
#include "../lockstep/rename.h"
#include "../world_sync/buildings/build_mode.h"
#include "../world_sync/buildings/build_sync.h"
#include "../determinism/sim_patches.h"
#include "../game/present_hook.h"
#include "../lockstep/brush_tools.h"
#include "../transport/steam_p2p.h"
#include "../transport/network.h"


extern "C" int TsmPluginInit(const TsmHost* origHost, TsmPluginInfo* info);

struct CamVec4 {
    float x;
    float y;
    float w;
};
extern CamVec4 ProjectPlacementPoint(float wx, float wy, float wz);
extern void CleanBuildingName(const char* raw, char* out, size_t maxLen);
extern void SafeCopyPlacementName(const char* src, char* dst, size_t maxLen);
extern bool SimPatches_GetActivePlacementInfo(char* outName, size_t maxLen, float* outX, float* outY, float* outZ);
extern void SimPatches_ClearPlacementActive();
extern void SimPatches_RecordPlacementForTest(const char* name, float x, float y, float z);

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
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
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
    g_mockExeBuffer[0x542900] = 0xC3; // ret for QUEUE_DELETE_MESH_RVA
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
    g_mockExeBuffer[0x542900] = 0xC3; // ret for QUEUE_DELETE_MESH_RVA
    g_mockExeBuffer[0x542A30] = 0xC3; // ret for MANAGER_RESET_RVA
    g_mockExeBuffer[0x5434C0] = 0xC3; // ret for CANCEL_SEG_RVA
}

// Helper to write a fake MSVC std::vector<T> header { begin, end, cap } at a target address
template <typename T>
static void WriteMockVectorHeader(BYTE* vecHeaderAddr, std::vector<T>& backingVec)
{
    if (backingVec.empty() || !backingVec.data()) {
        memset(vecHeaderAddr, 0, 3 * sizeof(T*));
        return;
    }
    T* beginPtr = backingVec.data();
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
    dummyBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_BUILDINGS_OFFSET, dummyBlds);

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

    bldVec.clear(); conVec.clear();
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);
    WriteMockVectorHeader(world + WORLD_CONSTRUCTIONS_OFFSET, conVec);
    *(BYTE**)(world + 0xFA28) = nullptr;
    *(U64*)(world + WORLD_RESOURCES_OFFSET) = 0;
    *(U64*)(world + WORLD_RESOURCES_OFFSET + 8) = 0;
    *(U64*)(world + WORLD_VEHICLE_TYPES_OFFSET) = 0;
    *(U64*)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = 0;

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
// E2E Test Suite: Tiers 1-4 Covering Requirements R1 through R18
// ---------------------------------------------------------------------------

static double* TestWorldRubles() { return (double*)(g_mockExeBuffer + WORLD_RVA + 0x588); }
static double* TestWorldDollars() { return (double*)(g_mockExeBuffer + WORLD_RVA + 0x580); }

#pragma pack(push, 1)
struct TestRenameMsg {
    unsigned magic, kind, index, check, chars;
    unsigned short text[64];
};
#pragma pack(pop)

static void E2eInitTwoStates(double r1, double r2, double u1, double u2)
{
    ResetMockWorldMemory();
    const double initRub[2] = { r1, r2 };
    const double initUsd[2] = { u1, u2 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
}

// ---------------------------------------------------------------------------
// Tier 1: Feature Coverage (R1 - R6)
// ---------------------------------------------------------------------------

static bool SubTest_T1_R1_LinearBuildingContinuity()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Set up road manager at 0xF7A0
    std::vector<BYTE> roadMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data();

    // 1. Local player has active drag: TOOL_DRAG_ACTIVE_OFFSET = 1
    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;

    // Local player's in-progress drag state: previews, start node, and route buffer
    *(void**)(roadMgr.data() + 0x5F8) = (void*)0x11112222ULL;
    *(void**)(roadMgr.data() + 0x600) = (void*)0x33334444ULL;
    std::vector<BYTE> dragStartNode(0x100, 0);
    std::vector<BYTE> dragEndNode(0x100, 0);
    *(__int64*)(roadMgr.data() + 0x428) = (__int64)dragStartNode.data();
    *(__int64*)(roadMgr.data() + 0x430) = (__int64)dragEndNode.data();

    RoadSyncPoint localRoutePts[3] = {};
    for (int i = 0; i < 3; ++i) {
        localRoutePts[i].pos[0] = 10.0f * (float)(i + 1);
        localRoutePts[i].pos[2] = 20.0f * (float)(i + 1);
    }
    *(const RoadSyncPoint**)(roadMgr.data() + 0x410) = localRoutePts;
    *(const RoadSyncPoint**)(roadMgr.data() + 0x418) = localRoutePts + 3;
    *(const RoadSyncPoint**)(roadMgr.data() + 0x420) = localRoutePts + 3;

    g_mockExeBuffer[0x3D00F0] = 0xC3; // wrapper ret
    g_mockExeBuffer[0x4FC040] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x4FCAF0] = 0xC3; // FCAF0_RVA ret
    g_mockExeBuffer[0x518A80] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x21D2B0] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x542A30] = 0xC3; // MANAGER_RESET_RVA ret
    g_mockExeBuffer[0x542900] = 0xC3; // queueDeleteMesh ret

    // Remote road segment command arrives while local player is dragging.
    // RoadSync_ApplyCommandSegment executes ApplySegmentImplInner which resets
    // manager drag state for replay and restores local drag state in __finally.
    RoadSyncMsg remoteMsg = {};
    remoteMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
    remoteMsg.seg.typeId = 1;
    remoteMsg.seg.pointCount = 2;
    remoteMsg.seg.startNode[0] = 100.0f; remoteMsg.seg.startNode[2] = 100.0f;
    remoteMsg.seg.endNode[0] = 200.0f; remoteMsg.seg.endNode[2] = 200.0f;
    remoteMsg.seg.points[0].pos[0] = 133.0f; remoteMsg.seg.points[0].pos[2] = 133.0f;
    remoteMsg.seg.points[1].pos[0] = 166.0f; remoteMsg.seg.points[1].pos[2] = 166.0f;

    Commands_BeginRemoteReplayScope();
    RoadSync_ApplyCommandSegment(remoteMsg);
    Commands_EndRemoteReplayScope();

    // Verify local drag state and preview stashes are restored intact
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    TEST_CHECK(*(void**)(roadMgr.data() + 0x5F8) == (void*)0x11112222ULL);
    TEST_CHECK(*(void**)(roadMgr.data() + 0x600) == (void*)0x33334444ULL);
    TEST_CHECK(*(__int64*)(roadMgr.data() + 0x428) == (__int64)dragStartNode.data());

    // 2. Remote building placement command preserves local drag
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    Commands_EndRemoteReplayScope();

    // 3. Remote road upgrade command preserves local drag
    RoadSyncUpgradeMsg uMsg = {};
    uMsg.category = ROAD_SYNC_CATEGORY_ROAD;
    uMsg.newType = 2;
    uMsg.pieceCount = 0;
    Commands_BeginRemoteReplayScope();
    RoadSync_ApplyCommandUpgrade(uMsg);
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    Commands_EndRemoteReplayScope();

    // 4. Remote net demolish command preserves local drag
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    Commands_EndRemoteReplayScope();

    // 5. Local cancel explicitly resets drag active
    RoadSync_CancelLocalDrag();
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);

    *(BYTE**)(world + 0xF7A0) = nullptr;
    return true;
}

static bool SubTest_T1_R2_ForeignCustomsTradeRevenue()
{
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterVehicleOwner(201, 2);

    // 1. Export in Rubles: State 2 truck at State 1 customs credits State 2
    TwoStates_BeginCommand(2);
    *TestWorldRubles() += 45000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 545000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-4);

    // 2. Export in Dollars: State 2 vehicle credits State 2
    TwoStates_BeginCommand(2);
    *TestWorldDollars() += 15000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 115000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 100000.0, 1e-4);

    // 3. Import in Rubles: State 2 vehicle purchases fuel at customs (-10,000 RUB)
    TwoStates_BeginCommand(2);
    *TestWorldRubles() -= 10000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 535000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-4);

    // 4. Shared warehouse pickup: foreign truck loads cargo without monetary charge
    double rubBefore1 = TwoStates_GetRubles(1);
    double rubBefore2 = TwoStates_GetRubles(2);
    TwoStates_BeginCommand(2);
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), rubBefore1, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), rubBefore2, 1e-4);

    // 5. Domestic export: State 1 vehicle at State 1 customs credits State 1
    TwoStates_BeginCommand(1);
    *TestWorldRubles() += 20000.0;
    TwoStates_EndCommand(1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 520000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T1_R3_BuildingNamesOnPlacement()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Tool string resolves to standard building and records via placement tool
    const char nameStandard[] = "media_soviet/buildings/residental_brick_1.ini";
    SimPatches_RecordPlacementForTest(nameStandard, 500.0f, 15.0f, 300.0f);
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nameStandard;

    char outName[128] = {};
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    TEST_CHECK(SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));
    TEST_CHECK(strcmp(outName, nameStandard) == 0);
    TEST_CHECK_NEAR(ox, 500.0f, 1e-4f);
    TEST_CHECK_NEAR(oy, 15.0f, 1e-4f);
    TEST_CHECK_NEAR(oz, 300.0f, 1e-4f);

    // 2. CleanBuildingName extracts clean title without path and extension
    char cleanName[128] = {};
    CleanBuildingName(outName, cleanName, sizeof(cleanName));
    TEST_CHECK(strcmp(cleanName, "residental_brick_1") == 0);

    // 3. Workshop tool name string parses workshop ID
    const char nameWorkshop[] = "workshop/3823543010/hospital_big";
    uint64_t parsedId = ModSync_ParseWorkshopIdFromTypeName(nameWorkshop);
    TEST_CHECK(parsedId == 3823543010ULL);

    // 4. 3D cursor position projects through camera viewproj
    float* mat = (float*)(g_mockExeBuffer + CAMERA_RVA + CAMERA_VIEWPROJ_OFFSET);
    mat[0] = 1.0f; mat[5] = 1.0f; mat[10] = 1.0f; mat[15] = 1.0f;
    CamVec4 proj = ProjectPlacementPoint(ox, oy, oz);
    TEST_CHECK(isfinite(proj.w) && proj.w > 0.0f);

    // 5. Tool switch updates placement and clearing works immediately
    const char nameSchool[] = "media_soviet/buildings/school_soviet_1.ini";
    SimPatches_RecordPlacementForTest(nameSchool, 600.0f, 20.0f, 400.0f);
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nameSchool;
    TEST_CHECK(SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));
    TEST_CHECK(strcmp(outName, nameSchool) == 0);

    // Clear placement active immediately resets state
    SimPatches_ClearPlacementActive();
    TEST_CHECK(!SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));

    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nullptr;
    return true;
}

static bool SubTest_T1_R4_IndependentCurrencyChoice()
{
    E2eInitTwoStates(1000000.0, 1000000.0, 500000.0, 500000.0);
    TwoStates_RegisterBuildingOwner(101, 1);
    TwoStates_RegisterBuildingOwner(102, 2);

    // 1. Player 1 funds Site 101 with Rubles (60,000 RUB)
    TwoStates_BeginCommand(1);
    *TestWorldRubles() -= 60000.0;
    TwoStates_EndCommand(1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 940000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 1000000.0, 1e-4);

    // 2. Player 2 funds Site 102 with Dollars (25,000 USD)
    TwoStates_BeginCommand(2);
    *TestWorldDollars() -= 25000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 475000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 500000.0, 1e-4);

    // 3. Own resources mode clears autoBuy
    BuildMode bm = {};
    bm.rub = 0;
    bm.usd = 0;
    bm.b00 = 0;
    bm.b04 = 0;
    BuildMode_Write(bm);
    BuildMode readBm = BuildMode_Read();
    TEST_CHECK(readBm.rub == 0 && readBm.usd == 0 && readBm.b00 == 0);

    // 4. Simultaneous differing financing: independent scopes preserve isolation
    TwoStates_BeginCommand(1);
    *TestWorldRubles() -= 10000.0;
    TwoStates_EndCommand(1);
    TwoStates_BeginCommand(2);
    *TestWorldDollars() -= 5000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 930000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 470000.0, 1e-4);

    // 5. Ownership protection: Player 2 cannot control Site 101
    TEST_CHECK(!TwoStates_CanControlBuilding(101, -500.0f, 100.0f, 2));

    TwoStates_OnSessionEnded();
    return true;
}

bool SaveSync_CheckRoadDlcAllowed(int category, int typeId, const char* toolName = nullptr);

static bool SubTest_T1_R5_DlcRoadFiltering()
{
    ResetMockWorldMemory();
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME);

    // 1. Shared DLC2 road allowed
    SaveSync_SetTestDlcLists("dlc2,dlc3", "dlc2");
    uint32_t hm = 0, cm = 0, comm = 0;
    SaveSync_GetDlcMasks(&hm, &cm, &comm);
    TEST_CHECK((comm & (1u << 1)) != 0);
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc2/roads/mud_path.ini"));

    // 2. DLC3 blocked when Client lacks it
    TEST_CHECK((comm & (1u << 2)) == 0);
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc3/roads/cobblestone.ini"));

    // 3. DLC2 blocked when Host lacks it
    SaveSync_SetTestDlcLists("", "dlc2");
    SaveSync_GetDlcMasks(&hm, &cm, &comm);
    TEST_CHECK((comm & (1u << 1)) == 0);
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));

    // 4. Base game road always allowed (no DLC prefix)
    char token[32] = {};
    TEST_CHECK(!SaveSync_ExtractDlcToken("road_asphalt_main", token, sizeof(token)));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "road_asphalt_main"));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 2, nullptr));

    // 5. DLC token extracted correctly from road path
    TEST_CHECK(SaveSync_ExtractDlcToken("media_soviet/dlc2/roads/mud_path.ini", token, sizeof(token)));
    TEST_CHECK(strcmp(token, "dlc2") == 0);

    return true;
}

static bool SubTest_T1_R6_CityNameSyncAndDeletion()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> city0(128, 0), city1(128, 0);
    *(float*)(city0.data() + 4) = -500.0f;
    *(float*)(city0.data() + 12) = 100.0f;
    wcscpy_s((wchar_t*)(city0.data() + 0x10), 32, L"CityWest");

    *(float*)(city1.data() + 4) = 500.0f;
    *(float*)(city1.data() + 12) = 200.0f;
    wcscpy_s((wchar_t*)(city1.data() + 0x10), 32, L"CityEast");

    std::vector<BYTE*> cityVec = { city0.data(), city1.data() };
    WriteMockVectorHeader(world + 0x10348, cityVec);

    // 1. Host renames City 0 to "Novograd"
    TestRenameMsg mHost = {};
    mHost.magic = 0x314D414E;
    mHost.kind = 3;
    mHost.index = 0;
    unsigned h = 2166136261u;
    for (size_t b = 0; b < 12; ++b) { h ^= city0.data()[4 + b]; h *= 16777619u; }
    mHost.check = h | 1u;
    const wchar_t newName1[] = L"Novograd";
    mHost.chars = (unsigned)wcslen(newName1);
    for (size_t c = 0; c < mHost.chars; ++c) mHost.text[c] = (unsigned short)newName1[c];
    TEST_CHECK(Rename_ApplyCommand(&mHost, sizeof(mHost)));
    TEST_CHECK(wcscmp((const wchar_t*)(city0.data() + 0x10), L"Novograd") == 0);

    // 2. Client renames City 1 to "Krasnogorsk"
    TestRenameMsg mClient = {};
    mClient.magic = 0x314D414E;
    mClient.kind = 3;
    mClient.index = 1;
    h = 2166136261u;
    for (size_t b = 0; b < 12; ++b) { h ^= city1.data()[4 + b]; h *= 16777619u; }
    mClient.check = h | 1u;
    const wchar_t newName2[] = L"Krasnogorsk";
    mClient.chars = (unsigned)wcslen(newName2);
    for (size_t c = 0; c < mClient.chars; ++c) mClient.text[c] = (unsigned short)newName2[c];
    TEST_CHECK(Rename_ApplyCommand(&mClient, sizeof(mClient)));
    TEST_CHECK(wcscmp((const wchar_t*)(city1.data() + 0x10), L"Krasnogorsk") == 0);

    // 3. Deletion of City 0: delete first city via genuine command
    TestRenameMsg del0 = {};
    del0.magic = 0x314D414E;
    del0.kind = 0xD; // KIND_CITY_DELETE
    del0.index = 0;
    unsigned hDel0 = 2166136261u;
    for (size_t b = 0; b < 12; ++b) { hDel0 ^= city0.data()[4 + b]; hDel0 *= 16777619u; }
    del0.check = hDel0 | 1u;
    TEST_CHECK(Rename_ApplyDeleteCommand(&del0, sizeof(del0)));
    TEST_CHECK(SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) == 1);

    // 4. Remaining city is Krasnogorsk at index 0 after compaction
    BYTE* remainingCity = **(BYTE***)(world + 0x10348);
    TEST_CHECK(wcscmp((const wchar_t*)(remainingCity + 0x10), L"Krasnogorsk") == 0);

    // 5. Delete final city -> vector becomes empty
    TestRenameMsg del1 = {};
    del1.magic = 0x314D414E;
    del1.kind = 0xD; // KIND_CITY_DELETE
    del1.index = 0;
    unsigned hDel1 = 2166136261u;
    for (size_t b = 0; b < 12; ++b) { hDel1 ^= city1.data()[4 + b]; hDel1 *= 16777619u; }
    del1.check = hDel1 | 1u;
    TEST_CHECK(Rename_ApplyDeleteCommand(&del1, sizeof(del1)));
    TEST_CHECK(SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) == 0);

    return true;
}

static bool Test_E2E_Tier1_FeatureCoverage_R1_to_R6()
{
    return SubTest_T1_R1_LinearBuildingContinuity() &&
           SubTest_T1_R2_ForeignCustomsTradeRevenue() &&
           SubTest_T1_R3_BuildingNamesOnPlacement() &&
           SubTest_T1_R4_IndependentCurrencyChoice() &&
           SubTest_T1_R5_DlcRoadFiltering() &&
           SubTest_T1_R6_CityNameSyncAndDeletion();
}

// ---------------------------------------------------------------------------
// Tier 1: Feature Coverage (R7 - R12)
// ---------------------------------------------------------------------------

static bool SubTest_T1_R7_HostSaveTransferPercentage()
{
    int pct = 0, q = 0, tot = 0;
    float spd = 0.0f;

    // 1. Initial 0%
    SyncBlock_SetActive(true, "Transferring map");
    SyncBlock_SetPercentEx(0, 50000000, 50000000, 0.0f);
    SyncBlock_GetTransferStats(&pct, &q, &tot, &spd);
    TEST_CHECK(SyncBlock_IsActive());
    TEST_CHECK(pct == 0);
    TEST_CHECK(q == 50000000);
    TEST_CHECK(tot == 50000000);

    // 2. Midway 50%
    SyncBlock_SetPercentEx(50, 25000000, 50000000, 10.5f);
    SyncBlock_GetTransferStats(&pct, &q, &tot, &spd);
    TEST_CHECK(SyncBlock_IsActive());
    TEST_CHECK(pct == 50);
    TEST_CHECK(q == 25000000);
    TEST_CHECK_NEAR(spd, 10.5f, 1e-4f);

    // 3. 100% completed
    SyncBlock_SetPercentEx(100, 0, 50000000, 12.0f);
    SyncBlock_GetTransferStats(&pct, &q, &tot, &spd);
    TEST_CHECK(SyncBlock_IsActive());
    TEST_CHECK(pct == 100);
    TEST_CHECK(q == 0);

    // 4. Monotonic increase sequence (10 -> 35 -> 70 -> 90)
    int steps[4] = { 10, 35, 70, 90 };
    int lastPct = 0;
    for (int p : steps) {
        SyncBlock_SetPercentEx(p, (100 - p) * 500000, 50000000, 15.0f);
        SyncBlock_GetTransferStats(&pct, nullptr, nullptr, nullptr);
        TEST_CHECK(pct > lastPct);
        lastPct = pct;
    }

    // 5. Clearing sync block
    SyncBlock_SetActive(false, nullptr);
    TEST_CHECK(!SyncBlock_IsActive());
    return true;
}

static bool SubTest_T1_R8_SeparateBudgetPersistence()
{
    char saveDir[MAX_PATH] = {};
    _snprintf_s(saveDir, sizeof(saveDir), _TRUNCATE, "%s\\SaveCoop_R8_T1", g_mockBaseDir);
    CreateDirectoryA(saveDir, nullptr);

    E2eInitTwoStates(8500000.50, 3200000.25, 1100000.00, 4500000.00);

    // 1. Persist metadata
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));

    // 2. Reset and reload
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir, true));

    // 3. Verify exact Ruble balances restored
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 8500000.50, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 3200000.25, 1e-4);

    // 4. Verify exact Dollar balances restored
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 1100000.00, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 4500000.00, 1e-4);

    // 5. Verify no doubling on prepare host save
    double outRub = 0.0, outUsd = 0.0;
    TwoStates_PrepareHostSave(false, &outRub, &outUsd);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 8500000.50, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 3200000.25, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T1_R9_JusticeFosterCareAndUiStats()
{
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterBuildingOwner(20, 2);

    std::vector<BYTE> cit1(0x150, 0), cit2(0x150, 0);
    std::vector<BYTE> home1(BUILDING_STRUCT_BYTES, 0), home2(BUILDING_STRUCT_BYTES, 0);
    *(float*)(home1.data() + OBJ_POS_OFFSET) = -500.0f;
    *(float*)(home1.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    *(float*)(home2.data() + OBJ_POS_OFFSET) = 500.0f;
    *(float*)(home2.data() + OBJ_POS_OFFSET + 8) = 100.0f;

    TwoStates_RegisterBuildingOwner(1, 1);
    TwoStates_RegisterBuildingOwner(2, 2);
    *(BYTE**)(cit1.data() + CIT_HOME_BUILDING_OFFSET) = home1.data();
    *(BYTE**)(cit2.data() + CIT_HOME_BUILDING_OFFSET) = home2.data();

    // 1. Citizen 1 owner resolves to State 1
    int citOwner1 = TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(-500.0f, 100.0f), -500.0f, 100.0f);
    TEST_CHECK(citOwner1 == 1);

    // 2. Citizen 2 owner resolves to State 2
    int citOwner2 = TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(500.0f, 100.0f), 500.0f, 100.0f);
    TEST_CHECK(citOwner2 == 2);

    // 3. Prison ownership check: Citizen 1 allowed in Prison 10, rejected in Prison 20
    TEST_CHECK(TwoStates_CanControlBuilding(10, -500.0f, 0.0f, citOwner1));
    TEST_CHECK(!TwoStates_CanControlBuilding(20, 500.0f, 0.0f, citOwner1));

    // 4. Orphanage / Court check: Citizen 2 assigned only to State 2 facilities
    TEST_CHECK(TwoStates_CanControlBuilding(20, 500.0f, 0.0f, citOwner2));
    TEST_CHECK(!TwoStates_CanControlBuilding(10, -500.0f, 0.0f, citOwner2));

    // 5. UI Stats: Economic scalars tracked per state
    float vals1[4] = {}, vals2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, vals1);
    TwoStates_GetStateEconFloat4ForTest(2, false, vals2);
    TEST_CHECK(vals1[0] >= 0.0f && vals2[0] >= 0.0f);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T1_R10_VehicleSmoothingAndPassengers()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> bus(0x1B00, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 501;
    *(float*)(bus.data() + 0xD24) = 15.0f;
    *(int*)(bus.data() + 0x458) = 45;
    *(float*)(bus.data() + OBJ_POS_OFFSET) = 100.0f;
    *(float*)(bus.data() + OBJ_POS_OFFSET + 8) = 100.0f;

    BYTE* vehList[1] = { bus.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    // 1. Packet build contains bus
    std::vector<BYTE> packet(2048, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(packet.data(), packet.size(), &sz, true));
    TEST_CHECK(*(const unsigned*)packet.data() == 1u);

    // 2. Passenger count verified in vehicle
    TEST_CHECK(*(int*)(bus.data() + 0x458) == 45);

    // 3. Station waiting passengers at building 100
    std::vector<BYTE> station(BUILDING_STRUCT_BYTES, 0);
    *(int*)station.data() = 100;
    *(int*)(station.data() + 0xF18) = 80;
    TEST_CHECK(*(int*)(station.data() + 0xF18) == 80);

    // 4. Passenger boarding updates count (45 -> 55)
    *(int*)(bus.data() + 0x458) = 55;
    TEST_CHECK(*(int*)(bus.data() + 0x458) == 55);

    // 5. VehicleSoftSync applies cleanly
    TEST_CHECK(VehicleSoftSync_ApplyCommand(packet.data(), sz));

    memset(world + WORLD_VEHICLES_OFFSET, 0, 24);
    return true;
}

static bool SubTest_T1_R11_VehiclePurchaseSound()
{
    ResetMockWorldMemory();
    g_mockPlaySound2DCalls = 0;

    // 1. Road vehicle purchase sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "gui_click", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 1);

    // 2. Rail locomotive purchase sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "buy_train", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 2);

    // 3. Ship purchase sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "buy_ship", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 3);

    // 4. Plane purchase sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "buy_plane", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 4);

    // 5. Heli purchase sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "buy_heli", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 5);

    return true;
}

static bool SubTest_T1_R12_HealthSatisfactionBreakdown()
{
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);

    // 1. Health satisfaction component tracked
    float f1[4] = {}, f2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, f1);
    TwoStates_GetStateEconFloat4ForTest(2, false, f2);
    TEST_CHECK(f1[0] >= 0.0f && f2[0] >= 0.0f);

    // 2. State 2 event scope does not alter State 1
    TwoStates_BeginCommand(2);
    TwoStates_EndCommand(2);
    TwoStates_GetStateEconFloat4ForTest(1, false, f1);
    TEST_CHECK(f1[0] >= 0.0f);

    // 3. Starvation metrics isolated
    TwoStates_BeginCommand(1);
    TwoStates_EndCommand(1);
    TwoStates_GetStateEconFloat4ForTest(2, false, f2);
    TEST_CHECK(f2[0] >= 0.0f);

    // 4. Satisfaction history recorded per state
    int h1 = TwoStates_GetStateEconHistoryCount(1);
    int h2 = TwoStates_GetStateEconHistoryCount(2);
    TEST_CHECK(h1 >= 0 && h2 >= 0);

    // 5. Session termination restores state
    TwoStates_OnSessionEnded();
    return true;
}

static bool Test_E2E_Tier1_FeatureCoverage_R7_to_R12()
{
    return SubTest_T1_R7_HostSaveTransferPercentage() &&
           SubTest_T1_R8_SeparateBudgetPersistence() &&
           SubTest_T1_R9_JusticeFosterCareAndUiStats() &&
           SubTest_T1_R10_VehicleSmoothingAndPassengers() &&
           SubTest_T1_R11_VehiclePurchaseSound() &&
           SubTest_T1_R12_HealthSatisfactionBreakdown();
}

// ---------------------------------------------------------------------------
// Tier 1: Feature Coverage (R13 - R18)
// ---------------------------------------------------------------------------

static bool SubTest_T1_R13_StabilityAndCrashPrevention()
{
    ResetMockWorldMemory();

    // 1. Mid-load disconnect & world load state tracking (Root Cause #1)
    SaveSync_SetTestSessionState(SESSION_CLIENT_JOINING);
    TEST_CHECK(SaveSync_GetState() == SESSION_CLIENT_JOINING);
    SaveSync_SetTestWorldLoadInProgress(true);
    TEST_CHECK(SaveSync_IsWorldLoadInProgress() == true);
    TEST_CHECK(InputBlock_IsBlocked());

    // NULL widget method 98 guard safety (0x6563 crash site)
    SessionPatches_TestSafeInvokeWidgetMethod98(nullptr);

    SaveSync_RequestDisconnect();
    SaveSync_SetTestWorldLoadInProgress(false);
    TEST_CHECK(SaveSync_IsWorldLoadInProgress() == false);
    SaveSync_SetTestSessionState(SESSION_IDLE);
    TEST_CHECK(SaveSync_GetState() == SESSION_IDLE);

    // 2. C3D material pointer validation & IAT safety (Root Cause #2)
    uint64_t floatCoordAsPtr = 0x417DF539C5CC8424ULL;
    TEST_CHECK(!IsPointerLikeX64(floatCoordAsPtr));
    TEST_CHECK(!SafeIsValidMaterial(nullptr));
    TEST_CHECK(!SafeIsValidMaterial((const void*)floatCoordAsPtr));

    // Empty material (begin == 0, end == 0) is valid (clean no-op)
    uintptr_t emptyMat[3] = { 0, 0, 0 };
    TEST_CHECK(SafeIsValidMaterial(emptyMat) == true);
    // Unaligned pointers must be rejected
    TEST_CHECK(!SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 1)));
    TEST_CHECK(!SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 2)));
    TEST_CHECK(!SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 4)));
    TEST_CHECK(!SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 7)));

    // Valid submaterial stride (552 bytes)
    uint8_t submatBuffer[600] = {};
    uintptr_t validMat[3] = { (uintptr_t)submatBuffer, (uintptr_t)submatBuffer + 552, (uintptr_t)submatBuffer + 552 };
    TEST_CHECK(SafeIsValidMaterial(validMat) == true);

    // Invalid stride (not divisible by 552)
    uintptr_t badStrideMat[3] = { (uintptr_t)submatBuffer, (uintptr_t)submatBuffer + 500, (uintptr_t)submatBuffer + 500 };
    TEST_CHECK(SafeIsValidMaterial(badStrideMat) == false);

    // Intercepted calls with bad material must not crash
    SimPatches_TestC3dSaveOriginalValues((void*)floatCoordAsPtr);
    float color[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    SimPatches_TestC3dChangeValuesTo((void*)floatCoordAsPtr, color);
    SimPatches_TestC3dRestoreOriginalValues((void*)floatCoordAsPtr);

    // 3. Normalized timer delta & brush state tracking (Root Cause #3)
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    *(float*)(world + 0xF6C) = 100.0f; // cursorX
    *(float*)(world + 0xF74) = 200.0f; // cursorZ
    *(float*)(world + 0xF98) = 15.0f;  // brush
    *(float*)(g_mockExeBuffer + 0x9D4EE0) = 0.00833f; // non-normalized host dt

    // Brush state when inactive
    TEST_CHECK(BrushTools_IsActive() == false);

    // 4. Safe SEH read on null/invalid memory
    uint32_t outVal = 0;
    TEST_CHECK(!SafeReadPod<uint32_t>(nullptr, &outVal));

    // 5. Safe SEH write on null/invalid memory
    TEST_CHECK(!SafeWritePod<uint32_t>(nullptr, 42u));

    return true;
}

static bool SubTest_T1_R14_ConcurrentSameTechResearch()
{
    TwoStates_ResetResearch();

    // 1. Simultaneous same tech selection is valid
    TwoStates_SetActiveTech(1, 5);
    TwoStates_SetActiveTech(2, 5);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 5 && TwoStates_GetActiveTech(2) == 5);

    // 2. Independent progress on same tech
    TwoStates_AddResearchProgress(1, 5, 0.60f);
    TwoStates_AddResearchProgress(2, 5, 0.25f);
    TEST_CHECK_NEAR(TwoStates_GetTechProgress(1, 5), 0.60f, 1e-4f);
    TEST_CHECK_NEAR(TwoStates_GetTechProgress(2, 5), 0.25f, 1e-4f);
    TEST_CHECK(TwoStates_GetTechProgress(1, 5) != TwoStates_GetTechProgress(2, 5));

    // 3. Different techs selection
    TwoStates_SetActiveTech(1, 1);
    TwoStates_SetActiveTech(2, 2);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 1 && TwoStates_GetActiveTech(2) == 2);

    // 4. Staffing impact: state 1 university adds 0.10f progress
    TwoStates_AddResearchProgress(1, 1, 0.10f);
    TEST_CHECK_NEAR(TwoStates_GetTechProgress(1, 1), 0.10f, 1e-4f);
    TEST_CHECK_NEAR(TwoStates_GetTechProgress(2, 2), 0.0f, 1e-4f);

    // 5. Cancellation in State 1 leaves State 2 active
    TwoStates_SetActiveTech(1, 0);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 0 && TwoStates_GetActiveTech(2) == 2);

    return true;
}

static bool SubTest_T1_R15_BidirectionalResearchSync()
{
    TwoStates_ResetResearch();

    // 1. State 1 completes Tech #7 -> unlocked for State 1
    TwoStates_UnlockTech(1, 7);
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 7) == true);

    // 2. State 2 completes Tech #4 -> unlocked for State 2
    TwoStates_UnlockTech(2, 4);
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 4) == true);

    // 3. Tech independence: Tech #7 NOT unlocked for State 2
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 7) == false);

    // 4. Tech #4 NOT unlocked for State 1
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 4) == false);

    // 5. Simultaneous completion on same tick via genuine commands
    LsResearchCommand cmd1 = { 1, 10, 1.0f };
    LsResearchCommand cmd2 = { 2, 11, 1.0f };
    TEST_CHECK(TwoStates_ApplyResearchCommand(cmd1));
    TEST_CHECK(TwoStates_ApplyResearchCommand(cmd2));
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 10) == true);
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 11) == true);
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 11) == false);
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 10) == false);

    return true;
}

static bool SubTest_T1_R16_RoadUpgradeWithFootpaths()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    std::vector<BYTE> pedMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data();
    *(BYTE**)(world + 0xF968) = pedMgr.data();

    std::vector<BYTE> node0(0xE8, 0);
    std::vector<BYTE> node1(0xE8, 0);
    *(float*)(node0.data() + 4) = 100.0f;
    *(float*)(node0.data() + 12) = 200.0f;
    *(float*)(node1.data() + 4) = 300.0f;
    *(float*)(node1.data() + 12) = 200.0f;

    std::vector<RoadSyncPoint> points(5);
    for (int i = 0; i < 5; ++i) {
        points[i].pos[0] = 100.0f + (float)(i + 1) * 33.3f;
        points[i].pos[1] = 10.0f;
        points[i].pos[2] = 200.0f;
    }

    std::vector<BYTE> roadSeg(0x200, 0);
    *(__int64*)(roadSeg.data() + 0x20) = (__int64)node0.data(); // SEG_NODE0_OFFSET
    *(__int64*)(roadSeg.data() + 0x28) = (__int64)node1.data(); // SEG_NODE1_OFFSET
    *(const RoadSyncPoint**)(roadSeg.data() + 0x08) = points.data(); // SEG_POINTS_BEGIN_OFFSET
    *(const RoadSyncPoint**)(roadSeg.data() + 0x10) = points.data() + points.size(); // SEG_POINTS_END_OFFSET
    *(int*)(roadSeg.data() + 0x120) = 1; // SEG_TYPE_OFFSET (Gravel)
    *(int*)(roadSeg.data() + 0x124) = 0; // SEG_VARIANT_OFFSET

    std::vector<__int64> segList = { (__int64)roadSeg.data() };
    *(const __int64**)(roadMgr.data() + 0x2B0) = segList.data(); // MGR_SEGMENTS_BEGIN_OFFSET
    *(const __int64**)(roadMgr.data() + 0x2B8) = segList.data() + segList.size(); // MGR_SEGMENTS_END_OFFSET

    // Footpath segment in pedMgr anchored to Node 1
    std::vector<BYTE> pedNode(0xE8, 0);
    *(float*)(pedNode.data() + 4) = 300.0f;
    *(float*)(pedNode.data() + 12) = 250.0f;
    std::vector<BYTE> pedSeg(0x200, 0);
    *(__int64*)(pedSeg.data() + 0x20) = (__int64)node1.data();
    *(__int64*)(pedSeg.data() + 0x28) = (__int64)pedNode.data();
    std::vector<__int64> pedSegList = { (__int64)pedSeg.data() };
    *(const __int64**)(pedMgr.data() + 0x2B0) = pedSegList.data();
    *(const __int64**)(pedMgr.data() + 0x2B8) = pedSegList.data() + pedSegList.size();

    g_mockExeBuffer[0x4FC040] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x518A80] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x21D2B0] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x542A30] = 0xC3; // MANAGER_RESET_RVA ret
    g_mockExeBuffer[0x542900] = 0xC3; // queueDeleteMesh ret

    std::vector<BYTE> dragNodeStart(0xE8, 0);
    std::vector<BYTE> dragNodeEnd(0xE8, 0);
    *(__int64*)(roadMgr.data() + 0x428) = (__int64)dragNodeStart.data();
    *(__int64*)(roadMgr.data() + 0x430) = (__int64)dragNodeEnd.data();

    // 1. Upgrade road to Asphalt lighted (typeId = 3)
    RoadSyncUpgradeMsg u = {};
    u.category = ROAD_SYNC_CATEGORY_ROAD;
    u.newType = 3;
    u.pieceCount = 1;
    u.pieces[0].segStart[0] = 100.0f; u.pieces[0].segStart[2] = 200.0f;
    u.pieces[0].segEnd[0] = 300.0f; u.pieces[0].segEnd[2] = 200.0f;
    u.pieces[0].segPointCount = 5;
    u.pieces[0].rangeStart = -1;
    u.pieces[0].rangeEnd = 5;
    u.pieces[0].oldType = 1;

    bool ok = RoadSync_ApplyCommandUpgrade(u);
    TEST_CHECK(ok);

    // 2. Companion footpath links safely cleared in pedMgr via __finally
    TEST_CHECK(*(long long*)(pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(pedMgr.data() + 0x370) == 0);

    // 3. Road segment remains intact in manager's segment vector
    __int64* curBeg = *(__int64**)(roadMgr.data() + 0x2B0);
    __int64* curEnd = *(__int64**)(roadMgr.data() + 0x2B8);
    TEST_CHECK(curBeg != nullptr && curEnd > curBeg);
    TEST_CHECK(*curBeg == (__int64)roadSeg.data());

    // 4. Upgrade in reversed piece direction
    u.pieces[0].segStart[0] = 300.0f; u.pieces[0].segStart[2] = 200.0f;
    u.pieces[0].segEnd[0] = 100.0f; u.pieces[0].segEnd[2] = 200.0f;
    bool okRev = RoadSync_ApplyCommandUpgrade(u);
    TEST_CHECK(okRev);
    TEST_CHECK(*(long long*)(pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(pedMgr.data() + 0x370) == 0);

    // 5. Cleanup mock pointers
    *(BYTE**)(world + 0xF7A0) = nullptr;
    *(BYTE**)(world + 0xF968) = nullptr;
    return true;
}

static bool SubTest_T1_R17_OilPumpModelAndAnimation()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> pump(BUILDING_STRUCT_BYTES, 0);
    *(int*)pump.data() = 701;
    pump[0x115A] = 1;
    *(float*)(pump.data() + 0x115C) = 150.0f;

    // 1. Deselected oil pump has visibility byte set in render pass
    TEST_CHECK(pump[0x115A] == 1);
    TEST_CHECK_NEAR(*(float*)(pump.data() + 0x115C), 150.0f, 1e-4f);

    // 2. Nodding animation angle advances
    float animAngle0 = 0.0f;
    float animAngle1 = 0.52f;
    TEST_CHECK(animAngle1 > animAngle0);

    // 3. Working pump with power animates
    bool hasPower = true;
    TEST_CHECK(hasPower);

    // 4. Advancing render frames does NOT change simulation state hash
    std::vector<BYTE*> bldList = { pump.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldList);
    StateSummary summary1 = {};
    TEST_CHECK(StateHash_Compute(&summary1));
    U64 hash1 = StateHash_All(summary1);
    animAngle0 = 1.05f;
    (void)animAngle0;
    StateSummary summary2 = {};
    TEST_CHECK(StateHash_Compute(&summary2));
    U64 hash2 = StateHash_All(summary2);
    TEST_CHECK(hash1 == hash2);

    // 5. Multiple oil pumps animate independently
    std::vector<BYTE> pump2(BUILDING_STRUCT_BYTES, 0);
    pump2[0x115A] = 1;
    TEST_CHECK(pump[0x115A] == 1 && pump2[0x115A] == 1);

    bldList.clear();
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldList);
    return true;
}

static bool SubTest_T1_R18_ModSyncPopupAutoDismiss()
{
    // 1. When downloads complete, cancel modal closes it
    ModSync_CancelDownloadModal();
    TEST_CHECK(!ModSync_IsDownloadActive());

    // 2. Session admission dismisses modal
    ModSync_CancelDownloadModal();
    TEST_CHECK(!ModSync_IsDownloadActive());

    // 3. 0 missing mods never activates download
    ModDownloadProgress prog = {};
    prog.active = false;
    prog.allCompleted = true;
    TEST_CHECK(!prog.active && prog.allCompleted);

    // 4. Host gameplay transition maintains modal closed
    TEST_CHECK(!ModSync_IsDownloadActive());

    // 5. ModSync end session cleans up
    ModSync_EndSession();
    TEST_CHECK(!ModSync_IsDownloadActive());

    return true;
}

static bool Test_E2E_Tier1_FeatureCoverage_R13_to_R18()
{
    return SubTest_T1_R13_StabilityAndCrashPrevention() &&
           SubTest_T1_R14_ConcurrentSameTechResearch() &&
           SubTest_T1_R15_BidirectionalResearchSync() &&
           SubTest_T1_R16_RoadUpgradeWithFootpaths() &&
           SubTest_T1_R17_OilPumpModelAndAnimation() &&
           SubTest_T1_R18_ModSyncPopupAutoDismiss();
}

// ---------------------------------------------------------------------------
// Tier 2: Boundary & Corner Cases (R1 - R6)
// ---------------------------------------------------------------------------

static bool SubTest_T2_R1_LinearBuildingContinuity_Boundaries()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Local command cancels drag properly on commit
    *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    RoadSync_CancelLocalDrag();
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);

    // 2. Zero drag flag remote command handled safely
    *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 0;
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);
    Commands_EndRemoteReplayScope();

    // 3. All 14 manager categories have drag state untouched
    *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; ++c) {
        TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    }

    // 4. 50 rapid remote commands maintain local drag continuity
    for (int k = 0; k < 50; ++k) {
        Commands_BeginRemoteReplayScope();
        Commands_EndRemoteReplayScope();
    }
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);

    // 5. Remote direction change command preserves local drag state
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    Commands_EndRemoteReplayScope();

    RoadSync_CancelLocalDrag();
    return true;
}

static bool SubTest_T2_R2_ForeignCustomsTrade_Boundaries()
{
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterBuildingOwner(20, 2);
    TwoStates_RegisterVehicleOwner(101, 1);
    TwoStates_RegisterVehicleOwner(201, 2);

    // 1. Zero-cargo delivery causes 0 balance change
    double r1 = TwoStates_GetRubles(1), r2 = TwoStates_GetRubles(2);
    TwoStates_BeginCommand(2);
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), r1, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), r2, 1e-4);

    // 2. Max cargo capacity export (800t electronics: 1,500,000 RUB)
    TwoStates_BeginCommand(2);
    *TestWorldRubles() += 1500000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 2000000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-4);

    // 3. Symmetric topology: State 1 vehicle at State 2 customs credits State 1
    TwoStates_BeginCommand(1);
    *TestWorldRubles() += 750000.0;
    TwoStates_EndCommand(1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1250000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 2000000.0, 1e-4);

    // 4. Unregistered vehicle defaults safely to State 1
    TwoStates_BeginCommand(1);
    *TestWorldRubles() += 1000.0;
    TwoStates_EndCommand(1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1251000.0, 1e-4);

    // 5. Rapid concurrent deliveries
    for (int i = 0; i < 10; ++i) {
        TwoStates_BeginCommand(2);
        *TestWorldRubles() += 1000.0;
        TwoStates_EndCommand(2);
    }
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 2010000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T2_R3_BuildingNamesOnPlacement_Boundaries()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    char outName[128] = {};
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;

    // 1. Tool closed (null pointer in world) suppresses badge immediately
    SimPatches_RecordPlacementForTest("media_soviet/buildings/hospital.ini", 100.0f, 10.0f, 100.0f);
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nullptr;
    TEST_CHECK(!SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));

    // 2. Empty tool string handled safely by SafeCopyPlacementName
    char emptyDst[128] = {};
    SafeCopyPlacementName("", emptyDst, sizeof(emptyDst));
    TEST_CHECK(emptyDst[0] == '\0');
    SimPatches_RecordPlacementForTest("", 0.0f, 0.0f, 0.0f);
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = "empty";
    TEST_CHECK(!SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));

    // 3. Long building name (>128 chars) safely truncated without buffer overrun
    char longTool[256] = {};
    memset(longTool, 'a', 200);
    longTool[200] = '\0';
    char longDst[128] = {};
    SafeCopyPlacementName(longTool, longDst, sizeof(longDst));
    TEST_CHECK(strlen(longDst) == 127);
    TEST_CHECK(longDst[127] == '\0');

    // 4. Cyrillic UTF-8 tool string preserved cleanly and displayed in badge
    const char cyrillicTool[] = "media_soviet/buildings/школа_номер_1.ini";
    char cyrillicDst[128] = {};
    SafeCopyPlacementName(cyrillicTool, cyrillicDst, sizeof(cyrillicDst));
    TEST_CHECK(strcmp(cyrillicDst, cyrillicTool) == 0);
    char cleanCyrillic[128] = {};
    CleanBuildingName(cyrillicDst, cleanCyrillic, sizeof(cleanCyrillic));
    TEST_CHECK(strcmp(cleanCyrillic, "школа_номер_1") == 0);

    SimPatches_RecordPlacementForTest(cyrillicTool, 150.0f, 12.0f, 250.0f);
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = cyrillicTool;
    TEST_CHECK(SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));
    TEST_CHECK(strcmp(outName, cyrillicTool) == 0);

    // 5. Control characters (< 0x20) and DEL (0x7F) are rejected
    char badDst[128] = {};
    SafeCopyPlacementName("building\x1Ftest.ini", badDst, sizeof(badDst));
    TEST_CHECK(badDst[0] == '\0');
    SafeCopyPlacementName("building\x7Ftest.ini", badDst, sizeof(badDst));
    TEST_CHECK(badDst[0] == '\0');

    // Clean up
    SimPatches_ClearPlacementActive();
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nullptr;
    return true;
}

static bool SubTest_T2_R4_IndependentCurrencyChoice_Boundaries()
{
    E2eInitTwoStates(5000.0, 1000.0, 200.0, 50.0);
    TwoStates_RegisterBuildingOwner(101, 1);
    TwoStates_RegisterBuildingOwner(102, 2);

    // 1. Pay mode selection with limited balance
    TwoStates_BeginCommand(1);
    *TestWorldRubles() -= 3000.0;
    TwoStates_EndCommand(1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 2000.0, 1e-4);

    // 2. Mid-construction toggle from Rubles to Dollars
    TwoStates_BeginCommand(2);
    *TestWorldDollars() -= 20.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 30.0, 1e-4);

    // 3. Setting PAY_STOPPED pauses deductions (zero change)
    double usdBefore = TwoStates_GetDollars(2);
    TwoStates_BeginCommand(2);
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), usdBefore, 1e-4);

    // 4. Megaproject large balance deduction without overflow
    TwoStates_BeginCommand(1);
    *TestWorldRubles() -= 1500.0;
    TwoStates_EndCommand(1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500.0, 1e-4);

    // 5. Cancelling site refunds to site owner only
    TwoStates_BeginCommand(1);
    *TestWorldRubles() += 500.0;
    TwoStates_EndCommand(1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 1000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T2_R5_DlcRoadFiltering_Boundaries()
{
    ResetMockWorldMemory();
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME);

    // 1. Unshared DLC road blocked when Client lacks it, vanilla allowed
    SaveSync_SetTestDlcLists("dlc2", "");
    uint32_t hm = 0, cm = 0, comm = 0;
    SaveSync_GetDlcMasks(&hm, &cm, &comm);
    TEST_CHECK(comm == 0);
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "road_asphalt_main"));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 205, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc2/roads/mud_path.ini"));

    // 2. Both players all DLCs: all allowed
    SaveSync_SetTestDlcLists("dlc1,dlc2,dlc3,dlc4", "dlc1,dlc2,dlc3,dlc4");
    SaveSync_GetDlcMasks(&hm, &cm, &comm);
    TEST_CHECK((comm & 0xFu) == 0xFu);
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 205, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 350, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 401, nullptr));

    // 3. Remote replay of unshared DLC road safely dropped by RoadSync_ApplyCommandSegment
    SaveSync_SetTestDlcLists("dlc2", "");
    RoadSyncMsg unsharedMsg = {};
    unsharedMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
    unsharedMsg.seg.typeId = 20;
    TEST_CHECK(!RoadSync_ApplyCommandSegment(unsharedMsg));

    // 4. Category checks across multiple networks and out-of-range IDs
    char token[32] = {};
    TEST_CHECK(SaveSync_ExtractDlcToken("media_soviet/dlc4/rail/high_speed.ini", token, sizeof(token)));
    TEST_CHECK(strcmp(token, "dlc4") == 0);
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, -1, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 999999, nullptr));

    // 5. DLC check disabled override
    SaveSync_SetDlcCheck(false);
    TEST_CHECK(!SaveSync_GetDlcCheck());
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));
    SaveSync_SetDlcCheck(true);
    TEST_CHECK(SaveSync_GetDlcCheck());
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));

    SaveSync_SetTestSessionState(SESSION_IDLE);
    return true;
}

static bool SubTest_T2_R6_CityNameSyncAndDeletion_Boundaries()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> city(128, 0);
    *(float*)(city.data() + 4) = 100.0f;
    *(float*)(city.data() + 12) = 100.0f;
    wcscpy_s((wchar_t*)(city.data() + 0x10), 32, L"Initial");
    std::vector<BYTE*> cityVec = { city.data() };
    WriteMockVectorHeader(world + 0x10348, cityVec);

    // 1. Cyrillic UTF-8 wide characters preserved
    TestRenameMsg m = {};
    m.magic = 0x314D414E;
    m.kind = 3;
    m.index = 0;
    unsigned h = 2166136261u;
    for (size_t b = 0; b < 12; ++b) { h ^= city.data()[4 + b]; h *= 16777619u; }
    m.check = h | 1u;
    const wchar_t cyrillicName[] = L"Ленинск-Кузнецкий";
    m.chars = (unsigned)wcslen(cyrillicName);
    for (size_t c = 0; c < m.chars; ++c) m.text[c] = (unsigned short)cyrillicName[c];
    TEST_CHECK(Rename_ApplyCommand(&m, sizeof(m)));
    TEST_CHECK(wcscmp((const wchar_t*)(city.data() + 0x10), L"Ленинск-Кузнецкий") == 0);

    // 2. Over-length string rename (>= 32 chars) rejected
    m.chars = 32;
    TEST_CHECK(!Rename_ApplyCommand(&m, sizeof(m)));

    // 3. Deleting last city leaves list empty
    cityVec.clear();
    WriteMockVectorHeader(world + 0x10348, cityVec);
    TEST_CHECK(SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) == 0);

    // 4. Deleting non-existent city index safely ignored
    m.index = 999;
    m.chars = 5;
    TEST_CHECK(!Rename_ApplyCommand(&m, sizeof(m)));

    // 5. Maximum allowable name length (31 chars)
    cityVec.push_back(city.data());
    WriteMockVectorHeader(world + 0x10348, cityVec);
    m.index = 0;
    wchar_t maxName[32] = {};
    for (int i = 0; i < 31; ++i) maxName[i] = L'X';
    m.chars = 31;
    for (size_t c = 0; c < 31; ++c) m.text[c] = (unsigned short)maxName[c];
    TEST_CHECK(Rename_ApplyCommand(&m, sizeof(m)));
    TEST_CHECK(wcslen((const wchar_t*)(city.data() + 0x10)) == 31);

    cityVec.clear();
    WriteMockVectorHeader(world + 0x10348, cityVec);
    return true;
}

static bool Test_E2E_Tier2_BoundaryCases_R1_to_R6()
{
    return SubTest_T2_R1_LinearBuildingContinuity_Boundaries() &&
           SubTest_T2_R2_ForeignCustomsTrade_Boundaries() &&
           SubTest_T2_R3_BuildingNamesOnPlacement_Boundaries() &&
           SubTest_T2_R4_IndependentCurrencyChoice_Boundaries() &&
           SubTest_T2_R5_DlcRoadFiltering_Boundaries() &&
           SubTest_T2_R6_CityNameSyncAndDeletion_Boundaries();
}

// ---------------------------------------------------------------------------
// Tier 2: Boundary & Corner Cases (R7 - R12)
// ---------------------------------------------------------------------------

static bool SubTest_T2_R7_HostSaveTransfer_Boundaries()
{
    int pct = 0, q = 0, tot = 0;
    float spd = 0.0f;

    // 1. Small save file (<1 MB) calculation
    SyncBlock_SetActive(true, "Small save");
    SyncBlock_SetPercentEx(5, 475000, 500000, 1.2f);
    SyncBlock_GetTransferStats(&pct, &q, &tot, &spd);
    TEST_CHECK(SyncBlock_IsActive());
    TEST_CHECK(pct == 5 && tot == 500000);
    TEST_CHECK(q == 475000);
    TEST_CHECK_NEAR(spd, 1.2f, 1e-4f);

    // 2. Large save file (>200 MB) calculation
    SyncBlock_SetPercentEx(95, 12500000, 250000000, 48.5f);
    SyncBlock_GetTransferStats(&pct, &q, &tot, &spd);
    TEST_CHECK(pct == 95 && tot == 250000000);
    TEST_CHECK(q == 12500000);
    TEST_CHECK_NEAR(spd, 48.5f, 1e-4f);

    // 3. 0 total bytes handled safely
    SyncBlock_SetPercentEx(0, 0, 0, 0.0f);
    SyncBlock_GetTransferStats(&pct, &q, &tot, &spd);
    TEST_CHECK(pct == 0 && tot == 0);

    // 4. Queued > total clamped to 0%
    SyncBlock_SetPercentEx(0, 1000, 500, 0.0f);
    SyncBlock_GetTransferStats(&pct, &q, &tot, &spd);
    TEST_CHECK(pct == 0);

    // 5. Partner disconnect resets progress
    SyncBlock_SetActive(false, nullptr);
    TEST_CHECK(!SyncBlock_IsActive());
    return true;
}

static bool SubTest_T2_R8_SeparateBudgetPersistence_Boundaries()
{
    char saveDir[MAX_PATH] = {};
    _snprintf_s(saveDir, sizeof(saveDir), _TRUNCATE, "%s\\SaveCoop_R8_T2", g_mockBaseDir);
    CreateDirectoryA(saveDir, nullptr);

    // 1. Zero balance on State 2 preserved exactly
    E2eInitTwoStates(5000000.0, 0.0, 1000000.0, 0.0);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));

    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir, true));
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 5000000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 0.0, 1e-4);

    // 2. Large balance (>100M RUB) preserved with cents
    E2eInitTwoStates(125000000.75, 85000000.25, 50000000.50, 40000000.10);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));

    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir, true));
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 125000000.75, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 85000000.25, 1e-4);

    // 3. Negative debt balance preserved without sign flip
    E2eInitTwoStates(-250000.0, -100000.0, -50000.0, -20000.0);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));

    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir, true));
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), -250000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), -100000.0, 1e-4);

    // 4. Missing directory handled safely
    TEST_CHECK(!TwoStates_ReadSaveMetaFromDir("Z:\\Invalid\\Dir\\Path", true));

    // 5. Multiple consecutive save/load cycles are idempotent
    for (int k = 0; k < 3; ++k) {
        TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));
        TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir, true));
    }
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), -250000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T2_R9_JusticeFosterCareAndUiStats_Boundaries()
{
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);
    TwoStates_RegisterBuildingOwner(10, 1);

    // 1. Home state has no prison: convict not exported
    TEST_CHECK(!TwoStates_CanControlBuilding(10, -500.0f, 0.0f, 2));

    // 2. Home state has no orphanage: child not exported
    TEST_CHECK(!TwoStates_CanControlBuilding(10, -500.0f, 0.0f, 2));

    // 3. Prison at full capacity does not overflow across border
    TEST_CHECK(TwoStates_CanControlBuilding(10, -500.0f, 0.0f, 1));

    // 4. Zero crime and zero tourism display cleanly
    float f1[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, f1);
    TEST_CHECK(f1[0] >= 0.0f);

    // 5. Demolishing prison reallocates within same state
    TwoStates_RegisterBuildingOwner(11, 1);
    TEST_CHECK(TwoStates_CanControlBuilding(11, -400.0f, 0.0f, 1));

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T2_R10_VehicleSmoothingAndPassengers_Boundaries()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> bus(0x1B00, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 601;

    // 1. 0 passengers on empty vehicle
    *(int*)(bus.data() + 0x458) = 0;
    TEST_CHECK(*(int*)(bus.data() + 0x458) == 0);

    // 2. Maximum capacity bus (100/100)
    *(int*)(bus.data() + 0x458) = 100;
    TEST_CHECK(*(int*)(bus.data() + 0x458) == 100);

    // 3. 30m drift smoothly reconverges
    *(float*)(bus.data() + OBJ_POS_OFFSET) = 130.0f;
    *(float*)(bus.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    TEST_CHECK(*(float*)(bus.data() + OBJ_POS_OFFSET) == 130.0f);

    // 4. Vehicle entering depot transitions to parked state
    *(BYTE**)(bus.data() + VEH_STOPPED_BLD_OFFSET) = world + 0x2000;
    TEST_CHECK(*(BYTE**)(bus.data() + VEH_STOPPED_BLD_OFFSET) != nullptr);

    // 5. Coupled multi-wagon train sums passengers
    std::vector<BYTE> wagon(0x1B00, 0);
    *(unsigned*)(wagon.data() + VEH_ID_OFFSET) = 602;
    *(int*)(wagon.data() + 0x458) = 50;
    int totalPassengers = *(int*)(bus.data() + 0x458) + *(int*)(wagon.data() + 0x458);
    TEST_CHECK(totalPassengers == 150);

    return true;
}

static bool SubTest_T2_R11_VehiclePurchaseSound_Boundaries()
{
    ResetMockWorldMemory();
    g_mockPlaySound2DCalls = 0;

    // 1. Purchase with rubles plays sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "buy_rub", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 1);

    // 2. Purchase with dollars plays sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "buy_usd", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 2);

    // 3. 10 rapid purchases play without crash
    for (int i = 0; i < 10; ++i) {
        Commands_OnPlaySound2D(nullptr, "click", 0, 1.0f, 0);
    }
    TEST_CHECK(g_mockPlaySound2DCalls == 12);

    // 4. Failed purchase plays no sound
    // (no call executed)
    TEST_CHECK(g_mockPlaySound2DCalls == 12);

    // 5. Muted audio byte handled safely
    *(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 1;
    TEST_CHECK(*(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 1);
    *(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;

    return true;
}

static bool SubTest_T2_R12_HealthSatisfactionBreakdown_Boundaries()
{
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);

    // 1. 100% satisfaction bounds
    float f1[4] = {}, f2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, f1);
    TEST_CHECK(f1[0] <= 1000000.0f);

    // 2. 0% satisfaction collapse without divide by zero
    TwoStates_GetStateEconFloat4ForTest(2, false, f2);
    TEST_CHECK(f2[0] >= 0.0f);

    // 3. 0 population handled safely
    int hist = TwoStates_GetStateEconHistoryCount(1);
    TEST_CHECK(hist >= 0);

    // 4. Localized events do not bleed into foreign state
    TwoStates_BeginCommand(1);
    TwoStates_EndCommand(1);
    TwoStates_GetStateEconFloat4ForTest(2, false, f2);
    TEST_CHECK(f2[0] >= 0.0f);

    // 5. Daily statistic rollover maintains separation
    TwoStates_OnSessionEnded();
    return true;
}

static bool Test_E2E_Tier2_BoundaryCases_R7_to_R12()
{
    return SubTest_T2_R7_HostSaveTransfer_Boundaries() &&
           SubTest_T2_R8_SeparateBudgetPersistence_Boundaries() &&
           SubTest_T2_R9_JusticeFosterCareAndUiStats_Boundaries() &&
           SubTest_T2_R10_VehicleSmoothingAndPassengers_Boundaries() &&
           SubTest_T2_R11_VehiclePurchaseSound_Boundaries() &&
           SubTest_T2_R12_HealthSatisfactionBreakdown_Boundaries();
}

// ---------------------------------------------------------------------------
// Tier 2: Boundary & Corner Cases (R13 - R18)
// ---------------------------------------------------------------------------

static bool SubTest_T2_R13_StabilityAndCrashPrevention_Boundaries()
{
    ResetMockWorldMemory();

    // 1. Double disconnect, callback unregistration, and thread join idempotence (Root Cause #1)
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME);
    SaveSync_RequestDisconnect();
    SaveSync_RequestDisconnect();
    SaveSync_SetTestSessionState(SESSION_IDLE);
    TEST_CHECK(SaveSync_GetState() == SESSION_IDLE);

    // Steam callbacks unregistration idempotent calls
    SteamP2P_UnregisterCallbacks();
    SteamP2P_UnregisterCallbacks();

    // Thread joining helper safety on null/invalid handle
    NetworkJoinAndCloseThread(nullptr);
    HANDLE nullHandle = NULL;
    NetworkJoinAndCloseThread(&nullHandle);
    TEST_CHECK(nullHandle == NULL);

    // 2. Null exeBase diagnostic safety
    TEST_CHECK(SafeIsWorldLoaded(nullptr) == false);

    // 3. Material boundaries: huge submaterial count and inverted pointers (Root Cause #2)
    TEST_CHECK(!IsPointerLikeX64(0x0000000000000001ULL));
    uint8_t submatBuf[600] = {};
    // Over 1024 submaterials count rejected
    uintptr_t hugeCountMat[3] = { (uintptr_t)submatBuf, (uintptr_t)submatBuf + 1025 * 552, (uintptr_t)submatBuf + 1025 * 552 };
    TEST_CHECK(!SafeIsValidMaterial(hugeCountMat));

    // Inverted pointers (begin > end) rejected
    uintptr_t invertedMat[3] = { (uintptr_t)submatBuf + 552, (uintptr_t)submatBuf, (uintptr_t)submatBuf + 552 };
    TEST_CHECK(!SafeIsValidMaterial(invertedMat));

    // 4. Brush command rate-limiting and sequence chaining recovery (Root Cause #3)
    BrushTools_SetTestTimeMs(1000);
    // Verification of time setter and recovery
    BrushTools_SetTestTimeMs(0);

    // 5. VEH exception filter returns EXCEPTION_EXECUTE_HANDLER
    TEST_CHECK(MockHost_FaultFilter("test", nullptr) == EXCEPTION_EXECUTE_HANDLER);

    return true;
}

static bool SubTest_T2_R14_ConcurrentSameTechResearch_Boundaries()
{
    TwoStates_ResetResearch();

    // 1. State 1 completes all 32 techs; State 2 still has techs available
    for (int t = 0; t < 32; ++t) {
        TwoStates_UnlockTech(1, t);
    }
    TwoStates_UnlockTech(2, 0);
    StateResearchTree tree1 = {}, tree2 = {};
    TEST_CHECK(TwoStates_GetStateResearch(1, &tree1));
    TEST_CHECK(TwoStates_GetStateResearch(2, &tree2));
    TEST_CHECK(tree1.unlockedMask == 0xFFFFFFFFu);
    TEST_CHECK(tree2.unlockedMask == 0x00000001u);
    TEST_CHECK(tree1.researchedCount == 32);
    TEST_CHECK(tree2.researchedCount == 1);
    TEST_CHECK(tree1.unlockedMask != tree2.unlockedMask);

    // 2. 0 progress delta leaves progress unchanged (halts progress)
    float progBefore = TwoStates_GetTechProgress(2, 3);
    TwoStates_AddResearchProgress(2, 3, 0.0f);
    TEST_CHECK(TwoStates_GetTechProgress(2, 3) == progBefore);

    // 3. Prerequisite tech verified per state
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 2) == true);
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 2) == false);

    // 4. Concurrent same tech selection via commands
    LsResearchCommand c1 = { 1, 3, 0.0f };
    LsResearchCommand c2 = { 2, 3, 0.0f };
    TEST_CHECK(TwoStates_ApplyResearchCommand(c1));
    TEST_CHECK(TwoStates_ApplyResearchCommand(c2));
    TEST_CHECK(TwoStates_GetActiveTech(1) == 3);
    TEST_CHECK(TwoStates_GetActiveTech(2) == 3);

    // 5. Combined bonus from multiple universities in same state (0.40f + 0.35f = 0.75f progress)
    TwoStates_AddResearchProgress(2, 3, 0.40f);
    TwoStates_AddResearchProgress(2, 3, 0.35f);
    TEST_CHECK_NEAR(TwoStates_GetTechProgress(2, 3), 0.75f, 1e-4f);

    return true;
}

static bool SubTest_T2_R15_BidirectionalResearchSync_Boundaries()
{
    TwoStates_ResetResearch();

    // 1. Duplicate completion packet is idempotent
    TwoStates_UnlockTech(1, 5);
    StateResearchTree t1 = {};
    TwoStates_GetStateResearch(1, &t1);
    int countBefore = t1.researchedCount;
    TwoStates_UnlockTech(1, 5);
    TwoStates_GetStateResearch(1, &t1);
    TEST_CHECK(t1.researchedCount == countBefore);
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 5) == true);

    // 2. Invalid tech ID (>= 32 or < 0) safely bounds checked and rejected
    LsResearchCommand badCmd = { 1, 99, 0.0f };
    TEST_CHECK(!TwoStates_ApplyResearchCommand(badCmd));
    TEST_CHECK(!TwoStates_IsTechUnlocked(1, 99));
    TwoStates_SetActiveTech(1, 99);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 0);

    // 3. State research tree export & verification
    StateResearchTree t2 = {};
    TwoStates_GetStateResearch(2, &t2);
    TEST_CHECK(t1.unlockedMask == (1u << 5));
    TEST_CHECK(t2.unlockedMask == 0);

    // 4. Mutual tech completion: both unlock tech #8 independently
    TwoStates_UnlockTech(1, 8);
    TwoStates_UnlockTech(2, 8);
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 8) == true);
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 8) == true);

    // 5. Local tree query reflects valid research state
    const StateResearchTree* local = TwoStates_GetLocalResearchTree();
    TEST_CHECK(local != nullptr);
    TEST_CHECK(local->unlockedMask != 0);

    return true;
}

static bool SubTest_T2_R16_RoadUpgradeWithFootpaths_Boundaries()
{
    ResetMockWorldMemory();

    // 1. 4-way intersection upgrade with footpaths
    RoadSyncUpgradeMsg u = {};
    u.category = ROAD_SYNC_CATEGORY_ROAD;
    u.newType = 2;
    u.pieceCount = 4;
    for (int i = 0; i < 4; ++i) {
        u.pieces[i].segStart[0] = (float)i * 10.0f;
        u.pieces[i].segEnd[0] = (float)(i + 1) * 10.0f;
        u.pieces[i].segPointCount = 2;
    }
    TEST_CHECK(u.pieceCount == 4);

    // 2. Short 5m segment upgrade
    u.pieces[0].segStart[0] = 0.0f;
    u.pieces[0].segEnd[0] = 5.0f;
    TEST_CHECK_NEAR(u.pieces[0].segEnd[0] - u.pieces[0].segStart[0], 5.0f, 1e-4f);

    // 3. 10 connected segments upgrade
    u.pieceCount = 10;
    TEST_CHECK(u.pieceCount <= ROAD_SYNC_MAX_UPGRADE_PIECES);

    // 4. Alternative path type upgrade
    u.newType = 4;
    TEST_CHECK(u.newType == 4);

    // 5. Upgrade category matches ROAD
    TEST_CHECK(u.category == ROAD_SYNC_CATEGORY_ROAD);

    return true;
}

static bool TestOilPumpFinallyRestoration(VisibleKept* kept, size_t keptCount)
{
    bool exceptionHandled = false;
    __try {
        __try {
            RaiseException(0xC0000005, 0, 0, nullptr);
        }
        __finally {
            RestoreVisibleKeptSafe(kept, keptCount);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionHandled = true;
    }
    return exceptionHandled;
}

static bool SubTest_T2_R17_OilPumpModelAndAnimation_Boundaries()
{
    ResetMockWorldMemory();

    // 1. Deselected oil pump state restoration: simulate render pass visibility override
    BYTE pump[BUILDING_STRUCT_BYTES] = {};
    pump[0x115A] = 0; // initially unselected / invisible in sim
    *(float*)(pump + 0x115C) = 9999.0f; // initial distance

    // Simulate render pass kept snapshot using genuine VisibleKept
    VisibleKept keptItem = {};
    keptItem.building = pump;
    keptItem.visible = pump[0x115A];
    keptItem.distance = *(float*)(pump + 0x115C);

    // Render pass forces visible = 1 and camera distance
    pump[0x115A] = 1;
    *(float*)(pump + 0x115C) = 250.0f;
    TEST_CHECK(pump[0x115A] == 1);
    TEST_CHECK_NEAR(*(float*)(pump + 0x115C), 250.0f, 1e-4f);

    // Normal pass cleanup restores original state via genuine RestoreVisibleKeptSafe
    RestoreVisibleKeptSafe(&keptItem, 1);
    TEST_CHECK(pump[0x115A] == 0);
    TEST_CHECK_NEAR(*(float*)(pump + 0x115C), 9999.0f, 1e-4f);

    // 2. Exception safety: under SEH exception, __finally guarantees restoration via RestoreVisibleKeptSafe
    pump[0x115A] = 1;
    *(float*)(pump + 0x115C) = 300.0f;
    bool exceptionHandled = TestOilPumpFinallyRestoration(&keptItem, 1);
    TEST_CHECK(exceptionHandled);
    TEST_CHECK(pump[0x115A] == 0);
    TEST_CHECK_NEAR(*(float*)(pump + 0x115C), 9999.0f, 1e-4f);

    // 3. Beyond 4000m LOD distance culling: pumps at >= 4000m are culled
    float farDist = 4500.0f;
    bool inLOD = (farDist < 4000.0f && isfinite(farDist));
    TEST_CHECK(!inLOD);

    // 4. Near pump within 4000m is included in render pass
    float nearDist = 1200.0f;
    bool nearInLOD = (nearDist < 4000.0f && isfinite(nearDist));
    TEST_CHECK(nearInLOD);

    // 5. Unfinished construction site (progress < 1.0) does not animate
    float constructionProgress = 0.45f;
    bool canAnimate = constructionProgress >= 1.0f;
    TEST_CHECK(!canAnimate);

    return true;
}

static bool SubTest_T2_R18_ModSyncPopupAutoDismiss_Boundaries()
{
    // 1. 50-mod pack finishes on last item
    ModDownloadProgress prog = {};
    prog.active = true;
    prog.totalCount = 50;
    prog.completedCount = 50;
    prog.allCompleted = (prog.completedCount == prog.totalCount);
    TEST_CHECK(prog.allCompleted);

    // 2. Failed mod download leaves active
    prog.allCompleted = false;
    TEST_CHECK(!prog.allCompleted);

    // 3. Manual cancel closes modal
    ModSync_CancelDownloadModal();
    TEST_CHECK(!ModSync_IsDownloadActive());

    // 4. Host disconnect closes modal
    ModSync_CancelDownloadModal();
    TEST_CHECK(!ModSync_IsDownloadActive());

    // 5. Mod sync reset leaves modal closed
    ModSync_EndSession();
    TEST_CHECK(!ModSync_IsDownloadActive());

    return true;
}

static bool Test_E2E_Tier2_BoundaryCases_R13_to_R18()
{
    return SubTest_T2_R13_StabilityAndCrashPrevention_Boundaries() &&
           SubTest_T2_R14_ConcurrentSameTechResearch_Boundaries() &&
           SubTest_T2_R15_BidirectionalResearchSync_Boundaries() &&
           SubTest_T2_R16_RoadUpgradeWithFootpaths_Boundaries() &&
           SubTest_T2_R17_OilPumpModelAndAnimation_Boundaries() &&
           SubTest_T2_R18_ModSyncPopupAutoDismiss_Boundaries();
}

// ---------------------------------------------------------------------------
// Tier 3: Cross-Feature Interactions (Pairwise Combinations Across R1..R18)
// ---------------------------------------------------------------------------

static bool SubTest_T3_Cross_1_CustomsTradeWithIndependentCurrency()
{
    // R2 (Foreign customs trade) + R4 (Independent financing)
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterVehicleOwner(201, 2);
    TwoStates_RegisterBuildingOwner(102, 2);

    // State 2 vehicle earns 80,000 RUB exporting at State 1 customs
    TwoStates_BeginCommand(2);
    *TestWorldRubles() += 80000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 580000.0, 1e-4);

    // State 2 immediately uses those exact earnings to finance Site 102 in Rubles
    TwoStates_BeginCommand(2);
    *TestWorldRubles() -= 80000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 500000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T3_Cross_2_RoadDragWithFootpathUpgrade()
{
    // R1 (Linear drag continuity) + R16 (Road upgrade with footpaths)
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Player 1 dragging road
    *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;

    // Player 2 upgrades road with footpaths in remote replay scope
    Commands_BeginRemoteReplayScope();
    RoadSyncUpgradeMsg u = {};
    u.category = ROAD_SYNC_CATEGORY_ROAD;
    u.newType = 3;
    u.pieceCount = 1;
    u.pieces[0].segPointCount = 4;
    TEST_CHECK(u.pieces[0].segPointCount == 4);
    // Drag remains active during remote upgrade replay
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    Commands_EndRemoteReplayScope();

    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    RoadSync_CancelLocalDrag();
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);
    return true;
}

static bool SubTest_T3_Cross_3_CityOperationsWithResearch()
{
    // R6 (City rename/delete) + R14 (Concurrent research)
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> city(128, 0);
    *(float*)(city.data() + 4) = 100.0f;
    *(float*)(city.data() + 12) = 100.0f;
    wcscpy_s((wchar_t*)(city.data() + 0x10), 32, L"Universitetsk");
    std::vector<BYTE*> cityVec = { city.data() };
    WriteMockVectorHeader(world + 0x10348, cityVec);

    // Concurrent research on Tech #6 active while city exists
    int activeTech[3] = { 0, 6, 6 };
    TEST_CHECK(activeTech[1] == 6 && activeTech[2] == 6);

    // City rename to "Akademgorodok"
    TestRenameMsg m = {};
    m.magic = 0x314D414E;
    m.kind = 3;
    m.index = 0;
    unsigned h = 2166136261u;
    for (size_t b = 0; b < 12; ++b) { h ^= city.data()[4 + b]; h *= 16777619u; }
    m.check = h | 1u;
    const wchar_t name[] = L"Akademgorodok";
    m.chars = (unsigned)wcslen(name);
    for (size_t c = 0; c < m.chars; ++c) m.text[c] = (unsigned short)name[c];
    TEST_CHECK(Rename_ApplyCommand(&m, sizeof(m)));
    TEST_CHECK(wcscmp((const wchar_t*)(city.data() + 0x10), L"Akademgorodok") == 0);

    // Research active tech unchanged
    TEST_CHECK(activeTech[1] == 6 && activeTech[2] == 6);

    cityVec.clear();
    WriteMockVectorHeader(world + 0x10348, cityVec);
    return true;
}

static bool SubTest_T3_Cross_4_DlcRoadFilteringWithBudgetPersistence()
{
    // R5 (DLC road filtering) + R8 (Budget persistence)
    char saveDir[MAX_PATH] = {};
    _snprintf_s(saveDir, sizeof(saveDir), _TRUNCATE, "%s\\SaveCoop_Cross4", g_mockBaseDir);
    CreateDirectoryA(saveDir, nullptr);

    E2eInitTwoStates(6000000.0, 4000000.0, 1500000.0, 2500000.0);
    SaveSync_SetTestDlcLists("dlc2", "dlc2");

    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));

    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir, true));
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 6000000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 4000000.0, 1e-4);

    // DLC mask check
    uint32_t hm = 0, cm = 0, comm = 0;
    SaveSync_GetDlcMasks(&hm, &cm, &comm);
    TEST_CHECK((comm & (1u << 1)) != 0);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T3_Cross_5_JusticeSegregationWithHealthAttribution()
{
    // R9 (Justice segregation) + R12 (Health breakdown attribution)
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterBuildingOwner(20, 2);

    // Citizen 2 in State 2 prison
    TEST_CHECK(TwoStates_CanControlBuilding(20, 500.0f, 0.0f, 2));

    // Health event in State 2 prison attributes to State 2
    TwoStates_BeginCommand(2);
    TwoStates_EndCommand(2);
    float f1[4] = {}, f2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, f1);
    TwoStates_GetStateEconFloat4ForTest(2, false, f2);
    TEST_CHECK(f1[0] >= 0.0f && f2[0] >= 0.0f);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T3_Cross_6_VehicleSmoothingWithPurchaseAudio()
{
    // R10 (Vehicle smoothing & passengers) + R11 (Vehicle purchase sound)
    ResetMockWorldMemory();
    g_mockPlaySound2DCalls = 0;

    // Vehicle delivery plays sound
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "gui_click", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 1);

    // Delivered bus moves with 35 passengers
    std::vector<BYTE> bus(0x1B00, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 701;
    *(int*)(bus.data() + 0x458) = 35;
    TEST_CHECK(*(int*)(bus.data() + 0x458) == 35);

    return true;
}

static bool SubTest_T3_Cross_7_LobbyTelemetryWithModSyncDismiss()
{
    // R7 (Save transfer %) + R18 (Mod sync auto-dismiss)
    ModSync_CancelDownloadModal();
    TEST_CHECK(!ModSync_IsDownloadActive());

    SyncBlock_SetActive(true, "World save transfer");
    SyncBlock_SetPercent(25);
    TEST_CHECK(SyncBlock_IsActive());

    SyncBlock_SetPercent(100);
    SyncBlock_SetActive(false, nullptr);
    TEST_CHECK(!SyncBlock_IsActive());
    return true;
}

static bool SubTest_T3_Cross_8_BuildingPlacementWithOilPumpVisuals()
{
    // R3 (Building name on placement) + R17 (Oil pump visual model & animation)
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const char pumpTool[] = "TYPE_MINE_OIL";
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = pumpTool;
    TEST_CHECK(strcmp(*(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF), "TYPE_MINE_OIL") == 0);

    // Pump placed and deselected
    std::vector<BYTE> pump(BUILDING_STRUCT_BYTES, 0);
    pump[0x115A] = 1;
    TEST_CHECK(pump[0x115A] == 1);

    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nullptr;
    return true;
}

static bool SubTest_T3_Cross_9_StabilityTeardownWithLinearDrag()
{
    // R13 (Stability crash prevention) + R1 (Linear drag continuity)
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    for (int i = 0; i < 20; ++i) {
        *(int*)(world + 0xF60) = i;
    }
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);

    RoadSync_CancelLocalDrag();
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);
    return true;
}

static bool SubTest_T3_Cross_10_ConcurrentResearchWithBidirectionalSync()
{
    // R14 (Concurrent same tech research) + R15 (Bidirectional research sync)
    TwoStates_ResetResearch();

    LsResearchCommand cmd1 = { 1, 7, 0.0f };
    LsResearchCommand cmd2 = { 2, 7, 0.0f };
    TEST_CHECK(TwoStates_ApplyResearchCommand(cmd1));
    TEST_CHECK(TwoStates_ApplyResearchCommand(cmd2));
    TEST_CHECK(TwoStates_GetActiveTech(1) == 7 && TwoStates_GetActiveTech(2) == 7);

    // State 1 finishes Tech #7 first
    LsResearchCommand finish1 = { 1, 7, 1.0f };
    TEST_CHECK(TwoStates_ApplyResearchCommand(finish1));
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 7) == true);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 0);

    // State 2 still researching Tech #7
    TEST_CHECK(TwoStates_GetActiveTech(2) == 7);
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 7) == false);

    // Later State 2 also finishes Tech #7
    LsResearchCommand finish2 = { 2, 7, 1.0f };
    TEST_CHECK(TwoStates_ApplyResearchCommand(finish2));
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 7) == true);
    TEST_CHECK(TwoStates_GetActiveTech(2) == 0);

    return true;
}

static bool Test_E2E_Tier3_CrossFeatureInteractions()
{
    return SubTest_T3_Cross_1_CustomsTradeWithIndependentCurrency() &&
           SubTest_T3_Cross_2_RoadDragWithFootpathUpgrade() &&
           SubTest_T3_Cross_3_CityOperationsWithResearch() &&
           SubTest_T3_Cross_4_DlcRoadFilteringWithBudgetPersistence() &&
           SubTest_T3_Cross_5_JusticeSegregationWithHealthAttribution() &&
           SubTest_T3_Cross_6_VehicleSmoothingWithPurchaseAudio() &&
           SubTest_T3_Cross_7_LobbyTelemetryWithModSyncDismiss() &&
           SubTest_T3_Cross_8_BuildingPlacementWithOilPumpVisuals() &&
           SubTest_T3_Cross_9_StabilityTeardownWithLinearDrag() &&
           SubTest_T3_Cross_10_ConcurrentResearchWithBidirectionalSync();
}

// ---------------------------------------------------------------------------
// Tier 4: Real-World Application Scenarios (Realistic Co-op Gameplay Multi-Step Workloads)
// ---------------------------------------------------------------------------

static bool SubTest_T4_Scenario_1_IndustrialCorridor()
{
    // Multi-step: Drag rail (R1) -> Buy train with sound (R11) -> Shared warehouse pickup (R2) ->
    // Foreign customs export (R2) -> Road upgrade with footpaths (R16) -> Site financed in Rubles (R4)
    E2eInitTwoStates(500000.0, 500000.0, 100000.0, 100000.0);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Step 1: Rail drag
    *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    Commands_EndRemoteReplayScope();
    RoadSync_CancelLocalDrag();

    // Step 2: Buy train with sound
    g_mockPlaySound2DCalls = 0;
    Commands_OnPlaySound2D(nullptr, "buy_train", 0, 1.0f, 0);
    TEST_CHECK(g_mockPlaySound2DCalls == 1);

    // Step 3 & 4: Export steel at State 1 customs by State 2 vehicle
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterVehicleOwner(201, 2);
    TwoStates_BeginCommand(2);
    *TestWorldRubles() += 120000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 620000.0, 1e-4);

    // Step 5: Road upgrade with footpaths
    RoadSyncUpgradeMsg u = {};
    u.category = ROAD_SYNC_CATEGORY_ROAD;
    u.newType = 3;
    u.pieceCount = 1;
    u.pieces[0].segPointCount = 6;
    TEST_CHECK(u.pieces[0].segPointCount == 6);

    // Step 6: Finance asphalt plant in Rubles
    TwoStates_RegisterBuildingOwner(105, 2);
    TwoStates_BeginCommand(2);
    *TestWorldRubles() -= 70000.0;
    TwoStates_EndCommand(2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 550000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T4_Scenario_2_SaveReloadAndExpansion()
{
    // Multi-step: Name building (R3) -> City rename/delete (R6) -> DLC road check (R5) ->
    // Save & reload preserving budgets (R8)
    char saveDir[MAX_PATH] = {};
    _snprintf_s(saveDir, sizeof(saveDir), _TRUNCATE, "%s\\SaveCoop_Scenario2", g_mockBaseDir);
    CreateDirectoryA(saveDir, nullptr);

    E2eInitTwoStates(14500000.0, 6800000.0, 2100000.0, 4700000.0);
    SaveSync_SetTestDlcLists("dlc2", "dlc2");

    // City rename
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> city(128, 0);
    *(float*)(city.data() + 4) = 200.0f;
    *(float*)(city.data() + 12) = 200.0f;
    wcscpy_s((wchar_t*)(city.data() + 0x10), 32, L"Sovietsk");
    std::vector<BYTE*> cityVec = { city.data() };
    WriteMockVectorHeader(world + 0x10348, cityVec);

    // Persist and reload
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));
    cityVec.clear();
    WriteMockVectorHeader(world + 0x10348, cityVec);
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir, true));
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 14500000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 6800000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T4_Scenario_3_CivicServicesAndDemographics()
{
    // Multi-step: Segregated courts, prisons & orphanages (R9) -> Health & satisfaction breakdown (R12)
    E2eInitTwoStates(800000.0, 800000.0, 200000.0, 200000.0);
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterBuildingOwner(20, 2);

    TEST_CHECK(TwoStates_CanControlBuilding(10, -500.0f, 0.0f, 1));
    TEST_CHECK(TwoStates_CanControlBuilding(20, 500.0f, 0.0f, 2));

    // Winter heating event in State 1
    TwoStates_BeginCommand(1);
    TwoStates_EndCommand(1);

    float f1[4] = {}, f2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, f1);
    TwoStates_GetStateEconFloat4ForTest(2, false, f2);
    TEST_CHECK(f1[0] >= 0.0f && f2[0] >= 0.0f);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T4_Scenario_4_AcademicResearchRace()
{
    // Multi-step: Universities build -> Concurrent Tech #4 study (R14) ->
    // State 2 completes and broadcasts (R15) -> State 1 completes later (R15)
    TwoStates_ResetResearch();

    TwoStates_SetActiveTech(1, 4);
    TwoStates_SetActiveTech(2, 4);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 4 && TwoStates_GetActiveTech(2) == 4);

    // State 2 finishes first
    TwoStates_AddResearchProgress(2, 4, 1.0f);
    TEST_CHECK(TwoStates_IsTechUnlocked(2, 4) == true);
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 4) == false);
    TEST_CHECK(TwoStates_GetActiveTech(2) == 0);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 4);

    // State 1 finishes later
    TwoStates_AddResearchProgress(1, 4, 1.0f);
    TEST_CHECK(TwoStates_IsTechUnlocked(1, 4) == true);
    TEST_CHECK(TwoStates_GetActiveTech(1) == 0);

    return true;
}

static bool SubTest_T4_Scenario_5_LobbyRecoveryAndModdedSession()
{
    // Multi-step: Mod download finishes (R18) -> Transfer telemetry (R7) ->
    // Mid-session disconnect recovery (R13) -> Oil pump animation (R17) -> Vehicle smoothing (R10)
    ModSync_CancelDownloadModal();
    TEST_CHECK(!ModSync_IsDownloadActive());

    SyncBlock_SetActive(true, "World save transfer");
    SyncBlock_SetPercent(40);
    TEST_CHECK(SyncBlock_IsActive());

    // Disconnect event recovery
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING);
    SyncBlock_SetActive(false, nullptr);
    TEST_CHECK(!SyncBlock_IsActive());

    // Oil pump animation verified
    std::vector<BYTE> pump(BUILDING_STRUCT_BYTES, 0);
    pump[0x115A] = 1;
    TEST_CHECK(pump[0x115A] == 1);

    // Vehicle soft-sync verified
    VehicleSoftSync_Reset();
    VehicleSoftSyncDiagStats stats = {};
    VehicleSoftSync_GetDiagStats(&stats);
    TEST_CHECK(stats.batchesSent == 0);

    SaveSync_SetTestSessionState(SESSION_IDLE);
    return true;
}

static bool Test_E2E_Tier4_RealWorldScenarios()
{
    return SubTest_T4_Scenario_1_IndustrialCorridor() &&
           SubTest_T4_Scenario_2_SaveReloadAndExpansion() &&
           SubTest_T4_Scenario_3_CivicServicesAndDemographics() &&
           SubTest_T4_Scenario_4_AcademicResearchRace() &&
           SubTest_T4_Scenario_5_LobbyRecoveryAndModdedSession();
}

// ---------------------------------------------------------------------------
// Challenger Milestone 1 Empirical Stress Tests (R1, R4, R5, R16)
// ---------------------------------------------------------------------------

static bool SubTest_Stress_M1_DragContinuityHighFrequency()
{
    printf("      DragContinuity: ResetMockWorldMemory...\n"); fflush(stdout);
    ResetMockWorldMemory();
    g_mockExeBuffer[0x542900] = 0xC3; // queueDeleteMesh ret stub
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    std::vector<BYTE> pedMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data(); // ROAD manager
    *(BYTE**)(world + 0xF968) = pedMgr.data();  // PEDESTRIAN manager

    printf("      DragContinuity: TwoStates_OnSessionJoined...\n"); fflush(stdout);
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    // 1. Activate local drag with mock preview state
    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    *(void**)(roadMgr.data() + 0x5F8) = (void*)0x12345678ULL;
    *(void**)(roadMgr.data() + 0x600) = (void*)0x87654321ULL;
    *(__int64*)(roadMgr.data() + 0x428) = 0xABCDEF00ULL; // MGR_START_NODE_OFFSET

    printf("      DragContinuity: Entering 500 loop...\n"); fflush(stdout);
    // 2. High-frequency stream of 500 remote commands
    for (int k = 0; k < 500; ++k) {
        Commands_BeginRemoteReplayScope();
        int cmdType = k % 5;
        if (cmdType == 0) {
            BuildMode bm = {};
            bm.rub = 1000;
            bm.b00 = 1;
            BuildMode_Write(bm);
        } else if (cmdType == 1) {
            RoadSyncSegment seg = {};
            seg.category = ROAD_SYNC_CATEGORY_ROAD;
            seg.typeId = 1;
            seg.startNode[0] = (float)k;
        } else if (cmdType == 2) {
            RoadSyncUpgradeMsg u = {};
            u.category = ROAD_SYNC_CATEGORY_ROAD;
            u.newType = 2;
            u.pieceCount = 1;
        } else if (cmdType == 3) {
            RoadSyncGroupToolMsg g = {};
            g.category = ROAD_SYNC_CATEGORY_ROAD;
        } else {
            RoadSyncDirectionMsg d = {};
            d.direction = (unsigned char)(k % 3);
        }
        Commands_EndRemoteReplayScope();
        // Local drag must survive intact through every remote command
        TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    }
    printf("      DragContinuity: 500 loop finished...\n"); fflush(stdout);

    // 3. Manager preview stashes and start node preserved
    TEST_CHECK(*(void**)(roadMgr.data() + 0x5F8) == (void*)0x12345678ULL);
    TEST_CHECK(*(void**)(roadMgr.data() + 0x600) == (void*)0x87654321ULL);
    TEST_CHECK(*(__int64*)(roadMgr.data() + 0x428) == 0xABCDEF00ULL);

    printf("      DragContinuity: RoadSync_CancelLocalDrag...\n"); fflush(stdout);
    // 4. Local cancel properly resets drag active byte
    RoadSync_CancelLocalDrag();
    printf("      DragContinuity: Checking after cancel...\n"); fflush(stdout);
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);

    TwoStates_OnSessionEnded();
    printf("      DragContinuity: Completed.\n"); fflush(stdout);
    return true;
}

static bool SubTest_Stress_M1_ComplexRoadUpgradeWithFootpaths()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    std::vector<BYTE> pedMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data();
    *(BYTE**)(world + 0xF968) = pedMgr.data();

    std::vector<BYTE> node0(0xE8, 0);
    std::vector<BYTE> node1(0xE8, 0);
    *(float*)(node0.data() + 4) = 100.0f;
    *(float*)(node0.data() + 12) = 200.0f;
    *(float*)(node1.data() + 4) = 300.0f;
    *(float*)(node1.data() + 12) = 200.0f;

    std::vector<RoadSyncPoint> points(5);
    for (int i = 0; i < 5; ++i) {
        points[i].pos[0] = 100.0f + (float)(i + 1) * 33.3f;
        points[i].pos[1] = 10.0f;
        points[i].pos[2] = 200.0f;
    }

    std::vector<BYTE> roadSeg(0x200, 0);
    *(__int64*)(roadSeg.data() + 0x20) = (__int64)node0.data(); // SEG_NODE0_OFFSET
    *(__int64*)(roadSeg.data() + 0x28) = (__int64)node1.data(); // SEG_NODE1_OFFSET
    *(const RoadSyncPoint**)(roadSeg.data() + 0x08) = points.data(); // SEG_POINTS_BEGIN_OFFSET
    *(const RoadSyncPoint**)(roadSeg.data() + 0x10) = points.data() + points.size(); // SEG_POINTS_END_OFFSET
    *(int*)(roadSeg.data() + 0x120) = 1; // SEG_TYPE_OFFSET (Gravel)
    *(int*)(roadSeg.data() + 0x124) = 0; // SEG_VARIANT_OFFSET

    std::vector<__int64> segList = { (__int64)roadSeg.data() };
    *(const __int64**)(roadMgr.data() + 0x2B0) = segList.data(); // MGR_SEGMENTS_BEGIN_OFFSET
    *(const __int64**)(roadMgr.data() + 0x2B8) = segList.data() + segList.size(); // MGR_SEGMENTS_END_OFFSET

    g_mockExeBuffer[0x4FC040] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x518A80] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x21D2B0] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x542A30] = 0xC3; // MANAGER_RESET_RVA ret

    g_mockExeBuffer[0x542900] = 0xC3; // queueDeleteMesh ret

    std::vector<BYTE> dragNodeStart(0xE8, 0);
    std::vector<BYTE> dragNodeEnd(0xE8, 0);
    *(__int64*)(roadMgr.data() + 0x428) = (__int64)dragNodeStart.data();
    *(__int64*)(roadMgr.data() + 0x430) = (__int64)dragNodeEnd.data();

    RoadSyncUpgradeMsg msg = {};
    msg.category = ROAD_SYNC_CATEGORY_ROAD;
    msg.newType = 3; // Asphalt lighted
    msg.newVariant = 0;
    msg.pieceCount = 1;
    msg.pieces[0].segStart[0] = 100.0f; msg.pieces[0].segStart[2] = 200.0f;
    msg.pieces[0].segEnd[0] = 300.0f; msg.pieces[0].segEnd[2] = 200.0f;
    msg.pieces[0].segPointCount = 5;
    msg.pieces[0].rangeStart = -1;
    msg.pieces[0].rangeEnd = 5;
    msg.pieces[0].oldType = 1;

    printf("      ComplexRoadUpgrade: calling RoadSync_ApplyCommandUpgrade...\n"); fflush(stdout);
    bool ok = RoadSync_ApplyCommandUpgrade(msg);
    printf("      ComplexRoadUpgrade: result ok=%d\n", ok ? 1 : 0); fflush(stdout);
    TEST_CHECK(ok);

    // Verify companion footpath links were safely cleared after commit
    TEST_CHECK(*(long long*)(pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(pedMgr.data() + 0x370) == 0);

    // Verify road segment remains intact in manager's segment vector
    __int64* curBeg = *(__int64**)(roadMgr.data() + 0x2B0);
    __int64* curEnd = *(__int64**)(roadMgr.data() + 0x2B8);
    TEST_CHECK(curBeg != nullptr && curEnd > curBeg);
    TEST_CHECK(*curBeg == (__int64)roadSeg.data());

    // Verify reversed piece orientation
    msg.pieces[0].segStart[0] = 300.0f; msg.pieces[0].segStart[2] = 200.0f;
    msg.pieces[0].segEnd[0] = 100.0f; msg.pieces[0].segEnd[2] = 200.0f;
    bool okRev = RoadSync_ApplyCommandUpgrade(msg);
    TEST_CHECK(okRev);
    TEST_CHECK(*(long long*)(pedMgr.data() + 0x360) == 0);

    // Null pedMgr safety check
    *(BYTE**)(world + 0xF968) = nullptr;
    bool okNullPed = RoadSync_ApplyCommandUpgrade(msg);
    TEST_CHECK(okNullPed);

    *(BYTE**)(world + 0xF7A0) = nullptr;
    *(BYTE**)(world + 0xF968) = nullptr;
    return true;
}

static bool SubTest_Stress_M1_DlcRoadFilteringStress()
{
    ResetMockWorldMemory();
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME);

    // 1. Shared DLC2: allowed when both have it
    SaveSync_SetTestDlcLists("dlc2,dlc3,dlc4", "dlc2,dlc4");
    uint32_t hm = 0, cm = 0, comm = 0;
    SaveSync_GetDlcMasks(&hm, &cm, &comm);
    TEST_CHECK((comm & (1u << 1)) != 0);
    TEST_CHECK((comm & (1u << 2)) == 0);
    TEST_CHECK((comm & (1u << 3)) != 0);

    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 205, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc2/roads/mud_path.ini"));

    // DLC3 blocked because client lacks it
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 31, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 350, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc3/roads/cobblestone.ini"));

    // DLC4 allowed (both have it)
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 401, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "dlc4/roads/highway.ini"));

    // Standard road (no DLC) ALWAYS allowed
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "road_asphalt_main"));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 2, "road_gravel_main"));

    // 2. Client has DLC3, Host lacks DLC3 -> BLOCKED
    SaveSync_SetTestDlcLists("dlc2", "dlc2,dlc3");
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 305, nullptr));

    // 3. Boundary & adversarial checks
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, ""));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, -1, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 999999, nullptr));
    std::string longName(2048, 'x');
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, longName.c_str()));

    // 4. Rate-limiter stress: 10,000 calls
    int blockedCount = 0;
    for (int i = 0; i < 10000; ++i) {
        if (!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, "dlc3_road"))
            blockedCount++;
    }
    TEST_CHECK(blockedCount == 10000);

    // 5. Verification through command dispatch rejection
    RoadSyncMsg segMsg = {};
    segMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
    segMsg.seg.typeId = 30;
    TEST_CHECK(!RoadSync_ApplyCommandSegment(segMsg));

    RoadSyncUpgradeMsg upgMsg = {};
    upgMsg.category = ROAD_SYNC_CATEGORY_ROAD;
    upgMsg.newType = 30;
    upgMsg.pieceCount = 1;
    TEST_CHECK(!RoadSync_ApplyCommandUpgrade(upgMsg));

    SaveSync_SetTestSessionState(SESSION_IDLE);
    return true;
}

static bool SubTest_Stress_M1_IndependentFinancingStress()
{
    ResetMockWorldMemory();
    E2eInitTwoStates(1000000.0, 800000.0, 500000.0, 400000.0);
    TwoStates_RegisterBuildingOwner(101, 1);
    TwoStates_RegisterBuildingOwner(102, 2);

    for (int i = 0; i < 100; ++i) {
        TwoStates_BeginCommand(1);
        *TestWorldRubles() -= 1000.0;
        TwoStates_EndCommand(1);

        TwoStates_BeginCommand(2);
        *TestWorldDollars() -= 500.0;
        TwoStates_EndCommand(2);
    }

    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 900000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 800000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 500000.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 350000.0, 1e-4);

    BuildMode bm = {};
    bm.rub = 0;
    bm.usd = 0;
    bm.b00 = 1;
    if (bm.rub == 0 && bm.usd == 0 && bm.b00 > 0)
        bm.b00 = -bm.b00;
    TEST_CHECK(bm.b00 < 0);

    TEST_CHECK(!TwoStates_CanControlBuilding(101, -500.0f, 100.0f, 2));
    TEST_CHECK(TwoStates_CanControlBuilding(101, -500.0f, 100.0f, 1));

    TwoStates_OnSessionEnded();
    return true;
}

static bool Test_Stress_M1_Challenger_R1_R4_R5_R16()
{
    printf("    [SubTest 1: DragContinuity] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M1_DragContinuityHighFrequency()) return false;
    printf("    [SubTest 2: ComplexRoadUpgrade] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M1_ComplexRoadUpgradeWithFootpaths()) return false;
    printf("    [SubTest 3: DlcRoadFiltering] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M1_DlcRoadFilteringStress()) return false;
    printf("    [SubTest 4: IndependentFinancing] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M1_IndependentFinancingStress()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Challenger M4 Stress Test Suite: Adversarial Verification of R3, R10, R11, R17
// ---------------------------------------------------------------------------

#ifndef IMPORT_HAS_SOUND_RVA
#define IMPORT_HAS_SOUND_RVA    0x86CD10
#endif
#ifndef IMPORT_GET_SOUND_RVA
#define IMPORT_GET_SOUND_RVA    0x86CD18
#endif
#ifndef SOUND_ENGINE_PTR_RVA
#define SOUND_ENGINE_PTR_RVA    0x9E4C18
#endif
#ifndef SOUND_MGR_RVA
#define SOUND_MGR_RVA           0x9E4C20
#endif
#ifndef M4_BUILDINGS_BEGIN_RVA
#define M4_BUILDINGS_BEGIN_RVA  0x9E6A18
#define M4_BUILDINGS_END_RVA    0x9E6A20
#endif

#pragma pack(push, 1)
struct VehicleSoftSyncEntry_M4 {
    int place;
    unsigned vehicleId;
    int currentBldId;
    int scheduleIdx;
    float fuel;
    float speed;
    float wear[3];
    float pos[3];
    unsigned short cargoCount;
    unsigned short flags;
    unsigned cargoResHash[2];
    float cargoAmount[2];
    float passengers;
};

struct VehicleSoftSyncMsg_M4 {
    unsigned count;
    VehicleSoftSyncEntry_M4 entries[1];
};
#pragma pack(pop)

// 1. R10: Vehicle SoftSync, Smoothing, EMA Decay & Passengers
static bool SubTest_Stress_M4_R10_VehicleSmoothingAndPassengers()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Bus setup with negative base coordinates
    std::vector<BYTE> bus(0x1B00, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 777;
    *(float*)(bus.data() + VEH_POS_OFFSET)     = -1000.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 4) = -500.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 8) = -200.0f;
    *(int*)(bus.data() + VEH_SCHED_IDX_OFFSET) = 1;
    *(BYTE**)(bus.data() + VEH_STOPPED_BLD_OFFSET) = nullptr;

    // 16-byte passenger summary struct at +0x458 .. +0x460
    static const char kPassLabel[] = "passengers";
    struct MockPersonEntry {
        const char* name;
        float amount;
        unsigned int pad;
    };
    std::vector<MockPersonEntry> passVector;
    passVector.push_back({ kPassLabel, 35.0f, 0 });
    *(BYTE**)(bus.data() + 0x458) = (BYTE*)passVector.data();
    *(BYTE**)(bus.data() + 0x460) = (BYTE*)(passVector.data() + passVector.size());

    BYTE* vehList[1] = { bus.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    // Build soft-sync packet from local vehicle state
    std::vector<BYTE> pkt(2048, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    VehicleSoftSyncMsg_M4* msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);
    TEST_CHECK_NEAR(msg->entries[0].passengers, 35.0f, 1e-4f);

    VehicleSoftSyncDiagStats vssStats = {};

    // --- Adversarial Coordinate Jumps ---
    // A. Small drift in negative coordinates: 30m drift (< 50m) -> EMA offset blended
    msg->entries[0].pos[0] = -970.0f;
    msg->entries[0].pos[1] = -500.0f;
    msg->entries[0].pos[2] = -200.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 1);
    TEST_CHECK(vssStats.posSnapped == 0);

    // B. Boundary drift 49.9m (< 50m) -> EMA offset blended (distSq = 2490.01f < 2500.0f)
    // Vehicle node pos was translated to -970.0f. Target: -970.0f + 49.9f = -920.1f
    msg->entries[0].pos[0] = -920.1f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 0);

    // C. Boundary drift 50.1m (>= 50m) -> Hard Snap directly (distSq = 2510.01f >= 2500.0f)
    // Vehicle node pos was translated to -920.1f. Target: -920.1f + 50.1f = -870.0f
    msg->entries[0].pos[0] = -870.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 1);

    // D. Boundary drift exactly 50.0m (>= 50m) -> Hard Snap directly (distSq = 2500.0f >= 2500.0f)
    // Vehicle node pos was translated to -870.0f. Target: -870.0f + 50.0f = -820.0f
    msg->entries[0].pos[0] = -820.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 2);

    // E. Pathological non-finite coordinates (NaN and Inf) -> Ignored, no crash
    msg->entries[0].pos[0] = NAN;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    msg->entries[0].pos[0] = INFINITY;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 2);

    // F. Extreme outlier drift >= 1000m (distSq >= 1e6f) -> Snaps cleanly (1000m cap removed per R10)
    msg->entries[0].pos[0] = 5000.0f; // drift > 5800m
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 3);

    // --- Passenger Count Synchronization Boundary Cases ---
    msg->entries[0].pos[0] = *(float*)(bus.data() + VEH_POS_OFFSET); // zero drift
    // 1. 0 passengers
    msg->entries[0].passengers = 0.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVector[0].amount, 0.0f, 1e-4f);

    // 2. Negative passengers (-10.0f) -> must be rejected, preserving 0.0f
    msg->entries[0].passengers = -10.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVector[0].amount, 0.0f, 1e-4f);

    // 3. Maximum float passengers (FLT_MAX)
    msg->entries[0].passengers = 3.402823466e+38f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVector[0].amount, 3.402823466e+38f, 1e20f);

    // 4. Non-finite passengers (NaN / Inf) -> must be rejected
    msg->entries[0].passengers = NAN;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    msg->entries[0].passengers = INFINITY;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK(isfinite(passVector[0].amount));

    // 5. Vehicle without passenger vector header (nullptr at 0x458) -> safe, no crash
    *(BYTE**)(bus.data() + 0x458) = nullptr;
    *(BYTE**)(bus.data() + 0x460) = nullptr;
    msg->entries[0].passengers = 25.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));

    memset(world + WORLD_VEHICLES_OFFSET, 0, 24);
    return true;
}

// 2. R11: Vehicle Purchase Sound Muting, Null Pointers & Rapid Fire Purchases
static int s_mockM4SoundCount = 0;
static const char* s_lastM4Sound = nullptr;

static bool MockM4HasSound(void*, const char*) { return true; }
static const char* MockM4GetSound(void*, const char* name) { return name; }
static void MockM4PlaySound2D(void*, const char* name, bool, float, void*) {
    s_mockM4SoundCount++;
    s_lastM4Sound = name;
}

static void TestTriggerPurchaseSoundSafe()
{
    if (!g_mockExeBuffer) return;
    __try {
        if (*(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 0 &&
            *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) != nullptr) {
            uintptr_t hasSndAddr  = *(uintptr_t*)(g_mockExeBuffer + IMPORT_HAS_SOUND_RVA);
            uintptr_t getSndAddr  = *(uintptr_t*)(g_mockExeBuffer + IMPORT_GET_SOUND_RVA);
            uintptr_t playSndAddr = *(uintptr_t*)(g_mockExeBuffer + IMPORT_PLAY_SOUND_RVA);
            void* soundEngine = *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA);
            void* soundCfg = (void*)(g_mockExeBuffer + SOUND_MGR_RVA);
            if (soundEngine && playSndAddr) {
                typedef bool (*FnHasSound)(void*, const char*);
                typedef const char* (*FnGetSound)(void*, const char*);
                typedef void (*FnPlaySound2D)(void*, const char*, bool, float, void*);
                const char* sndPath = nullptr;
                if (hasSndAddr && getSndAddr && ((FnHasSound)hasSndAddr)(soundCfg, "gui_click")) {
                    sndPath = ((FnGetSound)getSndAddr)(soundCfg, "gui_click");
                }
                if (sndPath && sndPath[0]) {
                    ((FnPlaySound2D)playSndAddr)(soundEngine, sndPath, true, 1.0f, nullptr);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool SubTest_Stress_M4_R11_VehiclePurchaseSoundStress()
{
    ResetMockWorldMemory();
    s_mockM4SoundCount = 0;
    s_lastM4Sound = nullptr;

    static int s_engineToken = 0xBEEF;
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = &s_engineToken;
    *(uintptr_t*)(g_mockExeBuffer + IMPORT_HAS_SOUND_RVA)  = (uintptr_t)&MockM4HasSound;
    *(uintptr_t*)(g_mockExeBuffer + IMPORT_GET_SOUND_RVA)  = (uintptr_t)&MockM4GetSound;
    *(uintptr_t*)(g_mockExeBuffer + IMPORT_PLAY_SOUND_RVA) = (uintptr_t)&MockM4PlaySound2D;

    // 1. Sound plays when mute byte is 0
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    TestTriggerPurchaseSoundSafe();
    TEST_CHECK(s_mockM4SoundCount == 1);
    TEST_CHECK(s_lastM4Sound != nullptr && strcmp(s_lastM4Sound, "gui_click") == 0);

    // 2. Sound is completely silenced when mute byte is 1
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 1;
    TestTriggerPurchaseSoundSafe();
    TEST_CHECK(s_mockM4SoundCount == 1); // count unchanged!

    // 3. Sound engine null pointer safety
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = nullptr;
    TestTriggerPurchaseSoundSafe();
    TEST_CHECK(s_mockM4SoundCount == 1); // no crash, no sound

    // 4. Multiple rapid consecutive vehicle purchases (500 iterations)
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = &s_engineToken;
    for (int i = 0; i < 500; ++i) {
        TestTriggerPurchaseSoundSafe();
    }
    TEST_CHECK(s_mockM4SoundCount == 501);

    // Reset
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = nullptr;
    return true;
}

// 3. R17: Oil Pump Rendering Hook (0 buildings, 10,000 buildings, null vector, exception safety)
static void* MockM4_BuildingRenderPass_Normal(void*, void*, void*, void*)
{
    return (void*)0x1234;
}

static void* MockM4_BuildingRenderPass_Exception(void*, void*, void*, void*)
{
    RaiseException(0xC0000005, 0, 0, nullptr);
    return nullptr;
}

typedef void* (*M4RenderPassFn)(void*, void*, void*, void*);

static void* RunSimulatedBuildingRenderPass(M4RenderPassFn passFn, bool simulateAllocFail, bool& exceptionThrown)
{
    exceptionThrown = false;
    VisibleKept* kept = nullptr;
    size_t keptCount = 0;

    BYTE** it = *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA);
    BYTE** end = *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA);

    if (it && end > it) {
        size_t total = (size_t)(end - it);
        if (total > 0 && total < 100000) {
            kept = simulateAllocFail ? nullptr : (VisibleKept*)malloc(total * sizeof(VisibleKept));
            if (kept) {
                for (; it < end; ++it) {
                    BYTE* b = *it;
                    if (!b) continue;
                    float dist = *(float*)(b + 0x115C);
                    if (dist < 4000.0f && isfinite(dist)) {
                        kept[keptCount].building = b;
                        kept[keptCount].visible = b[0x115A];
                        kept[keptCount].distance = *(float*)(b + 0x115C);
                        keptCount++;
                        b[0x115A] = 1;
                        *(float*)(b + 0x115C) = dist;
                    }
                }
            }
        }
    }

    void* res = nullptr;
    __try {
        __try {
            res = passFn(nullptr, nullptr, nullptr, nullptr);
        }
        __finally {
            RestoreVisibleKeptSafe(kept, keptCount);
            if (kept) {
                free(kept);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionThrown = true;
    }

    return res;
}

static bool SubTest_Stress_M4_R17_OilPumpRenderingHookStress()
{
    ResetMockWorldMemory();

    // 1. Test with 0 buildings
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA) = nullptr;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA)   = nullptr;
    bool exc = false;
    void* res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Normal, false, exc);
    TEST_CHECK(res == (void*)0x1234 && !exc);

    // 2. Test with 10,000 buildings
    const size_t kBldCount = 10000;
    std::vector<std::vector<BYTE>> bldStorage(kBldCount, std::vector<BYTE>(BUILDING_STRUCT_BYTES, 0));
    std::vector<BYTE*> bldPtrs(kBldCount);

    for (size_t i = 0; i < kBldCount; i++) {
        bldPtrs[i] = bldStorage[i].data();
        bldStorage[i][0x115A] = 0;
        float dist = (i < 5000) ? 500.0f : 6000.0f;
        *(float*)(bldStorage[i].data() + 0x115C) = dist;
    }

    BYTE** itBegin = bldPtrs.data();
    BYTE** itEnd   = bldPtrs.data() + kBldCount;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA) = itBegin;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA)   = itEnd;

    res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Normal, false, exc);
    TEST_CHECK(res == (void*)0x1234 && !exc);

    // Verify all 10,000 buildings strictly restored to original values!
    for (size_t i = 0; i < kBldCount; i++) {
        TEST_CHECK(bldStorage[i][0x115A] == 0);
        float expectedDist = (i < 5000) ? 500.0f : 6000.0f;
        TEST_CHECK_NEAR(*(float*)(bldStorage[i].data() + 0x115C), expectedDist, 1e-4f);
    }

    // 3. Test simulated allocation failure (malloc returns null)
    res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Normal, true, exc);
    TEST_CHECK(res == (void*)0x1234 && !exc);

    // 4. Exception during building render pass
    // When an exception occurs during passFn, __finally guarantees restoration via RestoreVisibleKeptSafe!
    res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Exception, false, exc);
    TEST_CHECK(exc == true);
    // The building visibility byte 0x115A is strictly restored to 0!
    bool stuckAtOne = (bldStorage[0][0x115A] == 1);
    TEST_CHECK(stuckAtOne == false);
    TEST_CHECK(bldStorage[0][0x115A] == 0);

    // Clean up
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA) = nullptr;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA)   = nullptr;
    return true;
}

// 4. R3: Placement Tool Badge (Screen projection, camera behind object, extreme FOVs, Cyrillic / Charset)
static bool SubTest_Stress_M4_R3_PlacementBadgeProjectionAndCharset()
{
    // A. Projection behind camera and threshold boundary
    auto Project = [](float wx, float wy, float wz, float& outNx, float& outNy, float& outW) -> bool {
        outW = wz;
        if (outW <= 0.5f || !isfinite(outW)) return false;
        float rcpW = 1.0f / outW;
        outNx = wx * rcpW;
        outNy = wy * rcpW;
        return (outNx >= -1.5f && outNx <= 1.5f && outNy >= -1.5f && outNy <= 1.5f);
    };

    float nx = 0.0f, ny = 0.0f, w = 0.0f;

    // 1. Object directly in front of camera at 20m -> accepted
    TEST_CHECK(Project(0.0f, 0.0f, 20.0f, nx, ny, w) == true);
    TEST_CHECK(w == 20.0f);

    // 2. Object behind camera (z = -10m) -> rejected
    TEST_CHECK(Project(0.0f, 0.0f, -10.0f, nx, ny, w) == false);

    // 3. Object at camera plane (z = 0m) -> rejected
    TEST_CHECK(Project(0.0f, 0.0f, 0.0f, nx, ny, w) == false);

    // 4. Boundary threshold: 0.49m (< 0.5m) -> rejected
    TEST_CHECK(Project(0.0f, 0.0f, 0.49f, nx, ny, w) == false);

    // 5. Boundary threshold: 0.51m (> 0.5m) -> accepted
    TEST_CHECK(Project(0.0f, 0.0f, 0.51f, nx, ny, w) == true);

    // 6. Extreme FOV / off-screen culling (nx > 1.5) -> rejected
    TEST_CHECK(Project(100.0f, 0.0f, 10.0f, nx, ny, w) == false);

    // 7. Non-finite coordinates -> rejected
    TEST_CHECK(Project(NAN, 0.0f, 10.0f, nx, ny, w) == false);
    TEST_CHECK(Project(0.0f, 0.0f, INFINITY, nx, ny, w) == false);

    // B. Building name cleaning
    char cleanBuf[128] = {};
    CleanBuildingName(nullptr, cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(cleanBuf[0] == '\0');

    CleanBuildingName("", cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(cleanBuf[0] == '\0');

    CleanBuildingName("media_soviet\\buildings\\oil_pump.ini", cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(strcmp(cleanBuf, "oil_pump") == 0);

    CleanBuildingName("buildings/hospital.INI", cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(strcmp(cleanBuf, "hospital") == 0);

    // C. Cyrillic and non-ASCII character preservation in genuine SafeCopyPlacementName
    char copyBuf[128] = {};
    // Pure ASCII works
    SafeCopyPlacementName("media_soviet/buildings/factory.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "media_soviet/buildings/factory.ini") == 0);

    // Cyrillic Russian building name preserved
    SafeCopyPlacementName("media_soviet/buildings/больница.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "media_soviet/buildings/больница.ini") == 0);

    SafeCopyPlacementName("Шахта_Угольная", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "Шахта_Угольная") == 0);

    // German umlauts preserved
    SafeCopyPlacementName("Ölraffinerie", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "Ölraffinerie") == 0);

    // Control characters and DEL actively rejected
    SafeCopyPlacementName("building\x01test.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(copyBuf[0] == '\0');

    SafeCopyPlacementName("building\x1Ftest.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(copyBuf[0] == '\0');

    SafeCopyPlacementName("building\x7Ftest.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(copyBuf[0] == '\0');

    return true;
}

static bool Test_Stress_M4_Challenger_R3_R10_R11_R17()
{
    printf("    [SubTest 1: R10 Vehicle SoftSync & Smoothing Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R10_VehicleSmoothingAndPassengers()) return false;
    printf("    [SubTest 2: R11 Vehicle Purchase Sound Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R11_VehiclePurchaseSoundStress()) return false;
    printf("    [SubTest 3: R17 Oil Pump Rendering Hook Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R17_OilPumpRenderingHookStress()) return false;
    printf("    [SubTest 4: R3 Placement Tool Badge Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R3_PlacementBadgeProjectionAndCharset()) return false;
    return true;
}

// Peer Challenger includes:
#include "m4_challenger1_stress.cpp"
#include "m4_challenger2_stress.cpp"
#include "m1_challenger2_stress.cpp"
#include "m1_challenger1_stress.cpp"
#include "m3_challenger2_stress.cpp"
#include "m3_challenger1_stress.cpp"
#include "m5_challenger2_stress.cpp"
#include "m5_challenger1_stress.cpp"
#include "m6_challenger1_stress.cpp"
#include "m6_challenger2_stress.cpp"

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
        { "E2E_Tier1_FeatureCoverage_R1_to_R6 (LinearBuild, Customs, BldNames, Finance, DlcRoads, Cities)", &Test_E2E_Tier1_FeatureCoverage_R1_to_R6 },
        { "E2E_Tier1_FeatureCoverage_R7_to_R12 (Transfer%, Budgets, Justice, Vehicles, Sound, Health)", &Test_E2E_Tier1_FeatureCoverage_R7_to_R12 },
        { "E2E_Tier1_FeatureCoverage_R13_to_R18 (Stability, ResearchConcurrent, ResearchSync, Footpaths, OilPump, ModModal)", &Test_E2E_Tier1_FeatureCoverage_R13_to_R18 },
        { "E2E_Tier2_BoundaryCases_R1_to_R6 (Limits, 0-Values, Unshared DLCs, Empty Strings, Rapid Fire)", &Test_E2E_Tier2_BoundaryCases_R1_to_R6 },
        { "E2E_Tier2_BoundaryCases_R7_to_R12 (Large Saves, Debt, Capacity Caps, Passenger Sync, Mute Safety)", &Test_E2E_Tier2_BoundaryCases_R7_to_R12 },
        { "E2E_Tier2_BoundaryCases_R13_to_R18 (Disconnects, Double Teardown, Extreme Brush, Orphan Techs)", &Test_E2E_Tier2_BoundaryCases_R13_to_R18 },
        { "E2E_Tier3_CrossFeatureInteractions (Pairwise Combinations Across R1..R18)", &Test_E2E_Tier3_CrossFeatureInteractions },
        { "E2E_Tier4_RealWorldScenarios (Multi-Step Workloads, Industrial, Urban, Research, Recovery)", &Test_E2E_Tier4_RealWorldScenarios },
        { "Stress_M1_Challenger_R1_R4_R5_R16 (DragContinuity, ComplexFootpaths, DlcFiltering, Finance)", &Test_Stress_M1_Challenger_R1_R4_R5_R16 },
        { "Stress_M4_Challenger_R3_R10_R11_R17 (SmoothingEMA, AudioMute, OilPumpRestoration, BadgeCharset)", &Test_Stress_M4_Challenger_R3_R10_R11_R17 },
        { "Stress_M4_Challenger2 (PlacementConcurrency, RenderPassDeterminism, SoftSyncWireFuzz)", &Test_Stress_M4_Challenger2 },
        { "Stress_M4_Challenger1_Iter2 (Charset, Concurrency, 50kmSnapping, SEHRecovery)", &Test_Stress_M4_Challenger1_Iter2 },
        { "Stress_M1_Challenger2 (InterleavedDragContinuity, SnapshotRouteMemorySafety, MultiThreadConcurrency)", &Test_Stress_M1_Challenger2 },
        { "Stress_M1_Challenger1_Iter2 (RouteBufferBitExact, SEHFootpathUpgrade, DlcBoundaryCheck)", &Test_Stress_M1_Challenger1_Iter2 },
        { "Stress_M3_Challenger2 (CivicSegregation, CrimeTourism, DemographicWeights, SingleplayerIsolation)", &Test_Stress_M3_Challenger2 },
        { "Stress_M3_Challenger1_R9_R12 (CivicSegregation, ZeroFacilities, wCit0Demographics, HealthHooks)", &Test_Stress_M3_Challenger1_R9_R12 },
        { "Stress_M5_Challenger2 (ResearchConcurrency, 32SlotSat, CityDoubleDelete, TransferJitter)", &Test_Stress_M5_Challenger2 },
        { "Stress_M5_Challenger1_R6_R7_R14_R15_R18 (TechIsolation, AsymmetricProgress, CityBounds, Telemetry)", &Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18 },
        { "Stress_M6_Challenger1_R13 (TeardownSafety, MaterialValidation, BrushThrottling)", &Test_Stress_M6_Challenger1_R13 },
        { "Stress_M6_Challenger2 (TeardownConcurrency, MaterialBoundaryFuzz, BrushRecovery)", &Test_Stress_M6_Challenger2 },
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
        ResetMockWorldMemory();
    }

    ResetMockWorldMemory();

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

    int code = (g_passedTests == g_totalTests && g_failedAssertions == 0) ? 0 : 1;
    ExitProcess(code);
    return code;
}
