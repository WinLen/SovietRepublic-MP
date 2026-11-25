// SPDX-License-Identifier: GPL-3.0-only
// Core & Determinism Test Suites (#1, #2, #3, #48, #49, #50).

#include "test_harness.h"
#include <atomic>
#include <limits>
#include <thread>

// ---------------------------------------------------------------------------
// Test 1: RVA Invariants, POD SEH Memory Helpers & Thread-Safe InitOnce Locks
// ---------------------------------------------------------------------------

static bool SubTest_Sub4GbHeapPointersAndDepotCheck()
{
    // Allocate a block below 4 GB (high 32 bits == 0) to simulate Windows x64 sub-4 GB heap addresses.
    void* lowMem = nullptr;
    for (uintptr_t addr = 0x10000000ULL; addr <= 0xEC000000ULL && !lowMem; addr += 0x04000000ULL) {
        lowMem = VirtualAlloc((LPVOID)addr, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!lowMem) {
        // Fallback loop in case all strided slots are occupied on the host machine.
        for (uintptr_t addr = 0x10000000ULL; addr <= 0xEFFF0000ULL && !lowMem; addr += 0x00100000ULL) {
            lowMem = VirtualAlloc((LPVOID)addr, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
    }
    TEST_CHECK(lowMem != nullptr);
    if (!lowMem) return false;

    const uint64_t lowAddr = (uint64_t)(uintptr_t)lowMem;
    TEST_CHECK((lowAddr >> 32) == 0ULL);
    TEST_CHECK(IsPointerLikeX64(lowAddr));
    TEST_CHECK(!IsPointerLikeX64(lowAddr | 1ULL));
    TEST_CHECK(!IsPointerLikeX64(lowAddr | 4ULL));

    BYTE* base = (BYTE*)lowMem;
    memset(base, 0, 0x4000);
    BYTE* lowBld = base;
    BYTE* lowBldType = base + 0x0800;
    BYTE* lowVeh = base + 0x1000;
    BYTE* lowVehType = base + 0x2800;

    *(BYTE**)(lowBld + 0x5B0) = nullptr;
    *(BYTE**)(lowBld + 0x318) = lowBldType;
    *(int32_t*)(lowBldType + BLD_TYPE_CAT_OFF) = 0x0E; // Road depot category
    *(int32_t*)(lowBldType + 0x36C) = 0;

    *(BYTE**)(lowVeh + 0x1748) = lowVehType;
    *(int32_t*)(lowVehType + VEH_TYPE_CATEGORY_OFFSET) = 1;    // Road vehicle category

    TEST_CHECK(SimPatches_TestIsVehicleAtDepotOrStop(lowBld, lowVeh, 0) == 1);
    TEST_CHECK(SimPatches_TestIsVehicleAtDepotOrStop(lowBld, nullptr, 0) == 1);

    TEST_CHECK(VirtualFree(lowMem, 0, MEM_RELEASE) != 0);
    return true;
}

bool Test_RvaAndSafeMem()
{
    ResetMockWorldMemory();

    // Verify canonical RVAs match AGENTS.md Section 3
    TEST_CHECK(WORLD_RVA == 0xA1EF20);
    TEST_CHECK(VEC_BUILDINGS_BEGIN_RVA == 0xA30AD0);
    TEST_CHECK(VEC_CONSTRUCTIONS_BEGIN_RVA == 0xA30BF0);
    TEST_CHECK(WORKSHOP_MGR_PTR_RVA == 0xA2C360);
    TEST_CHECK(MANAGER_RESET_STATE_OFFSET == 0x408);
    TEST_CHECK(MGR_ROUTE_BEGIN_OFFSET == 0x410);
    TEST_CHECK(HEADER_BIN_DATA_OFFSET == 0x260);
    TEST_CHECK(HEADER_BIN_RECORD_SIZE == 0x80);
    TEST_CHECK(WORLD_WORK_RESOURCE_PTR_OFF == 0xC360);
    TEST_CHECK(WORLD_ECON_TAB_OFFSET == 0xC6F4);
    TEST_CHECK(WORLD_LOANS_OFFSET == 0x10C68);
    TEST_CHECK(WORLD_LOAN_SELECTED_OFFSET == 0x10C80);
    TEST_CHECK(WORLD_LOAN_REPAY_AMOUNT_OFF == 0x10CA0);
    TEST_CHECK(WORLD_LOAN_STATS_OFFSET == 0x117B0);
    TEST_CHECK(RVA_VEC_LOAN_PUSH_BACK == 0x46D200);
    TEST_CHECK(RVA_TAKE_LOAN == 0x4DF2D0);
    TEST_CHECK(RVA_GET_MAX_AVAILABLE_BORROW == 0x4DF510);
    TEST_CHECK(RVA_STEP_ONE_LOAN_DAY == 0x4DF620);
    TEST_CHECK(RVA_STEP_ALL_LOANS_DAILY == 0x4DF7F0);
    TEST_CHECK(RVA_CALC_INTEREST_RATE == 0x4DFC10);
    TEST_CHECK(sizeof(SovietLoan) == 0x28);
    TEST_CHECK(offsetof(SovietLoan, yearInterestRate) == 0x00);
    TEST_CHECK(offsetof(SovietLoan, loanSubType) == 0x0C);
    TEST_CHECK(offsetof(SovietLoan, remainingPrincipal) == 0x14);
    TEST_CHECK(offsetof(SovietLoan, unpaidPenaltyBalance) == 0x18);
    TEST_CHECK(offsetof(SovietLoan, totalPaidSoFar) == 0x24);
    TEST_CHECK(sizeof(LsLoanCommand) == 84);
    TEST_CHECK(RVA_HUD_DRAW_BTN_FRAME == 0x46A7F0);
    TEST_CHECK(RVA_MINIMAP_DRAW_CONTENT == 0x4E4010);
    TEST_CHECK(RVA_MINIMAP_DRAW_BUTTONS == 0x4E60D0);
    TEST_CHECK(WORLD_MINIMAP_PTR_OFFSET == 0xD498);
    TEST_CHECK(MINIMAP_TOOLTIP_WSTR_RVA == 0xA2C558);
    TEST_CHECK(HUD_QUAD_STAGE_RVA == 0xA08070);
    TEST_CHECK(HUD_SOLID_TEX_PTR_RVA == 0xA2ABE0);
    TEST_CHECK(HUD_PIN_ICON_TEX_PTR_RVA == 0xA2A460);
    TEST_CHECK(HUD_MAP_CAMERA_TEX_PTR_RVA == 0xA2A868);
    TEST_CHECK_NEAR(MINIMAP_CAMERA_ICON_MAP_FRACTION, 0.15f, 1e-4f);
    TEST_CHECK_NEAR(MINIMAP_CAMERA_ICON_MAX_PX, 70.0f, 1e-4f);
    TEST_CHECK(HUD_QUAD_ROT_RVA == 0xA08318);
    TEST_CHECK(HUD_MIDDLEPOINT_OBJ_RVA == 0xA34D90);
    TEST_CHECK(HUD_FMT_WSTR_RVA == 0x8FD708);
    TEST_CHECK(HUD_BASIC_PASS_STR_RVA == 0x8B708C);
    TEST_CHECK(RVA_IAT_CREATE_MANAGED_TEX == 0x8B1518);
    TEST_CHECK(RVA_IAT_HUD_HIT_TEST_QUAD == 0x8B18F0);
    TEST_CHECK(RVA_IAT_HUD_DRAW_QUAD == 0x8B1900);
    TEST_CHECK(RVA_IAT_HUD_GET_MOUSE_POS == 0x8B1908);
    TEST_CHECK(RVA_IAT_TEXT_PRINT_ALL == 0x8B1910);
    TEST_CHECK(RVA_IAT_TEXT_DRAW_W == 0x8B1920);
    TEST_CHECK(RVA_IAT_HUD_END_DRAW == 0x8B1998);
    TEST_CHECK(RVA_IAT_HUD_BEGIN_DRAW == 0x8B19B8);
    TEST_CHECK(WORLD_STUDIOS_BEGIN_OFFSET == 0xD2C8u);
    TEST_CHECK(WORLD_STUDIOS_END_OFFSET == 0xD2D0u);
    TEST_CHECK(STUDIO_BUILDING_OFFSET == 0x150u);
    TEST_CHECK(STUDIO_BROADCAST_SLIDERS_OFFSET == 0x138u);
    TEST_CHECK(BLD_UPBRINGING_SLIDERS_OFFSET == 0x13A0u);
    TEST_CHECK(BLD_MIN_LOYALTY_EMPLOYEES_OFFSET == 0x13D8u);
    TEST_CHECK(BLD_MIN_LOYALTY_CITIZENS_OFFSET == 0x13E0u);
    TEST_CHECK(BLD_ALERT_GOODS_MISSING_OFFSET == 0x126Du);
    TEST_CHECK(BLD_ALERT_MAIN_TOGGLE_OFFSET == 0x1270u);
    TEST_CHECK(BLD_ALERT_FINISHED_OFFSET == 0x0E10u);
    TEST_CHECK(BLD_POLLUTION_ALERT_TOGGLE_OFFSET == 0x0DFCu);
    TEST_CHECK(BLD_POLLUTION_ALERT_THRESH_OFFSET == 0x0E3Cu);
    TEST_CHECK(SETTINGS_FIELD_UPBRINGING == 1002);
    TEST_CHECK(SETTINGS_FIELD_BROADCAST == 1003);
    TEST_CHECK(RVA_CIT_SEVERE_ILLNESS_TICK == 0x874739u);
    TEST_CHECK(RVA_CIT_SEVERE_ILLNESS_RESUME == 0x874753u);
    TEST_CHECK(RVA_WATER_HEALTH_STEP == 0x1C98A0u);
    TEST_CHECK(RVA_VEHICLE_CARGO_TRADE == 0x6BDA30u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_CHECK == 0x61AA20u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_GET == 0x61AAC0u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_TOOLTIP == 0x7EA247u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_TOOLTIP_FOUND == 0x7EA2DAu);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_TOOLTIP_SKIP == 0x7EA4E0u);

    TEST_CHECK(TwoStates_GetShowMinimapBorders() == true);
    TwoStates_SetShowMinimapBorders(false);
    TEST_CHECK(TwoStates_GetShowMinimapBorders() == false);
    TwoStates_SetShowMinimapBorders(true);
    TEST_CHECK(TwoStates_GetShowMinimapBorders() == true);

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

    // Invariant #12: IsPointerLikeX64 including low Windows x64 heap segments (v >= 0x00100000 && high < 0x100, including < 4 GB)
    TEST_CHECK(!IsPointerLikeX64(0ULL));
    TEST_CHECK(!IsPointerLikeX64(100ULL));
    TEST_CHECK(!IsPointerLikeX64(0x00000000FFFFFFFFULL)); // two 32-bit ints/floats with high == 0
    TEST_CHECK(IsPointerLikeX64((uint64_t)(uintptr_t)&target));

    // Unmapped low-heap address (high == 2, but not committed) -> must return false
    TEST_CHECK(!IsPointerLikeX64(0x00000002deadbeefULL & ~3ULL));

    TEST_CHECK(SubTest_Sub4GbHeapPointersAndDepotCheck());
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

    // Cold-start concurrent SaveSync_GetLocalDllCrc() race test (first call in process)
    {
        std::atomic<int> ready{ 0 };
        std::atomic<bool> go{ false };
        unsigned int crcs[8] = {};
        std::vector<std::thread> crcThreads;
        for (int t = 0; t < 8; ++t) {
            crcThreads.emplace_back([&, t]() {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go.load(std::memory_order_acquire)) {
                    YieldProcessor();
                }
                crcs[t] = SaveSync_GetLocalDllCrc();
            });
        }
        while (ready.load(std::memory_order_acquire) < 8) {
            Sleep(0);
        }
        go.store(true, std::memory_order_release);
        for (auto& th : crcThreads) {
            th.join();
        }
        TEST_CHECK(crcs[0] != 0u);
        for (int t = 1; t < 8; ++t) {
            TEST_CHECK(crcs[t] == crcs[0]);
        }
    }

    // Lock-free QPC profiler, timer resolution probe, and SteamP2P perf query checks
    {
        DiagPerfSnapshot drain = {};
        DiagPerf_TakeSnapshot(&drain);

        DiagPerf_Record(DIAG_PERF_EARLY, 1000);
        DiagPerf_Record(DIAG_PERF_EARLY, 3000);
        DiagPerf_RecordVirtualQuery(2000);
        DiagPerf_RecordMalloc(4096);
        DiagPerf_RecordMalloc(8192);

        DiagPerfSnapshot snap = {};
        DiagPerf_TakeSnapshot(&snap);
        TEST_CHECK(snap.buckets[DIAG_PERF_EARLY].count == 2);
        TEST_CHECK(snap.buckets[DIAG_PERF_EARLY].totalMs > 0.0);
        TEST_CHECK(snap.buckets[DIAG_PERF_EARLY].avgMs > 0.0);
        TEST_CHECK(snap.buckets[DIAG_PERF_EARLY].maxMs >= snap.buckets[DIAG_PERF_EARLY].avgMs);
        TEST_CHECK(snap.buckets[DIAG_PERF_VIRTUAL_QUERY].count >= 1);
        TEST_CHECK(snap.mallocCalls >= 2);
        TEST_CHECK(snap.mallocBytes >= 12288);
        TEST_CHECK(snap.winTimerResMs > 0.0 && snap.winTimerResMs <= 20.0);

        DiagPerfSnapshot cleared = {};
        DiagPerf_TakeSnapshot(&cleared);
        TEST_CHECK(cleared.buckets[DIAG_PERF_EARLY].count == 0);
        TEST_CHECK(cleared.mallocCalls == 0);
        TEST_CHECK(cleared.mallocBytes == 0);

        const double timerMs = DiagLog_QueryTimerResolutionMs();
        TEST_CHECK(timerMs > 0.0 && timerMs <= 20.0);

        bool p2pConn = true, p2pRelay = true;
        int32_t p2pQueued = -1;
        SteamP2P_GetPerfStats(&p2pConn, &p2pRelay, &p2pQueued);
        TEST_CHECK(!p2pConn && !p2pRelay && p2pQueued == 0);
    }

    // Phase 0 telemetry: new buckets, batch recording, formatting, and SimPatches_WaitRender(false)
    {
        DiagPerfSnapshot drain = {};
        DiagPerf_TakeSnapshot(&drain);

        const DiagPerfBucket newBuckets[] = {
            DIAG_PERF_INTERFRAME_GAP,
            DIAG_PERF_PREPARE_NATIVE,
            DIAG_PERF_PREPARE_MOD,
            DIAG_PERF_BEFORE_AFTER_EARLY,
            DIAG_PERF_PRIVATE_LATE,
            DIAG_PERF_UI_SWITCHES,
            DIAG_PERF_MALLOC_ZERO,
        };
        for (DiagPerfBucket b : newBuckets) {
            const int64_t cnt = (b == DIAG_PERF_MALLOC_ZERO) ? 256 : 4;
            const int64_t tot = (b == DIAG_PERF_MALLOC_ZERO) ? 64000 : 10000;
            const int64_t mx  = (b == DIAG_PERF_MALLOC_ZERO) ? 1000 : 4000;
            DiagPerf_RecordBatch(b, cnt, tot, mx);
        }
        DiagPerf_RecordBatch(DIAG_PERF_TS_ECON_HOOK, 5, 10000, 4000);
        // Boundary checks: count<=0, invalid bucket, negative ticks
        DiagPerf_RecordBatch(DIAG_PERF_INTERFRAME_GAP, 0, 999, 999);
        DiagPerf_RecordBatch(DIAG_PERF_BUCKET_COUNT, 1, 100, 100);
        DiagPerf_RecordBatch(DIAG_PERF_PRIVATE_LATE, 1, -50, -50);
        DiagPerf_RecordMallocBatch(256, 65536);
        DiagPerf_RecordMalloc(4096);

        SimPatches_WaitRender(true);
        SimPatches_WaitRender(false);

        DiagPerfSnapshot snap = {};
        DiagPerf_TakeSnapshot(&snap);
        TEST_CHECK(snap.buckets[DIAG_PERF_WAIT_RENDER].count == 0);
        TEST_CHECK(snap.buckets[DIAG_PERF_INTERFRAME_GAP].count == 4);
        TEST_CHECK(snap.buckets[DIAG_PERF_PREPARE_NATIVE].count == 4);
        TEST_CHECK(snap.buckets[DIAG_PERF_PREPARE_MOD].count == 4);
        TEST_CHECK(snap.buckets[DIAG_PERF_BEFORE_AFTER_EARLY].count == 4);
        TEST_CHECK(snap.buckets[DIAG_PERF_PRIVATE_LATE].count == 5);
        TEST_CHECK(snap.buckets[DIAG_PERF_UI_SWITCHES].count == 4);
        TEST_CHECK(snap.buckets[DIAG_PERF_MALLOC_ZERO].count == 256);
        TEST_CHECK(snap.buckets[DIAG_PERF_TS_ECON_HOOK].count == 5);
        TEST_CHECK_NEAR(snap.buckets[DIAG_PERF_TS_ECON_HOOK].avgMs,
                        snap.buckets[DIAG_PERF_TS_ECON_HOOK].totalMs / 5.0, 1e-9);
        TEST_CHECK(snap.buckets[DIAG_PERF_TS_ECON_HOOK].maxMs > 0.0);
        TEST_CHECK(snap.mallocCalls >= 257);
        TEST_CHECK(snap.mallocBytes >= 65536 + 4096);

        char mainLine[1024] = {};
        char rndLine[2048] = {};
        DiagLog_FormatPerfMainLine(snap, mainLine, sizeof(mainLine));
        DiagLog_FormatPerfRndLine(snap, false, true, 128, rndLine, sizeof(rndLine));
        TEST_CHECK(strstr(mainLine, "PerfMain:") != nullptr);
        TEST_CHECK(strstr(mainLine, "tsHook=") != nullptr);
        TEST_CHECK(strstr(mainLine, "ms/") != nullptr);
        TEST_CHECK(strstr(mainLine, "us(n=5)") != nullptr);
        TEST_CHECK(strstr(rndLine, "PerfRnd: fg=") != nullptr);
        TEST_CHECK(strstr(rndLine, "uiSw=") != nullptr);
        TEST_CHECK(strstr(rndLine, "gap=") != nullptr);
        TEST_CHECK(strstr(rndLine, "prepNat=") != nullptr);
        TEST_CHECK(strstr(rndLine, "prepMod=") != nullptr);
        TEST_CHECK(strstr(rndLine, "baEarly=") != nullptr);
        TEST_CHECK(strstr(rndLine, "privLate=") != nullptr);
        TEST_CHECK(strstr(rndLine, "mzEst=") != nullptr);
        TEST_CHECK(strstr(rndLine, "P2P(conn=0 relay=1 sendQ=128B)") != nullptr);

        // Zero-count snapshot must not emit NaN/Inf
        DiagPerfSnapshot zeroSnap = {};
        DiagLog_FormatPerfMainLine(zeroSnap, mainLine, sizeof(mainLine));
        DiagLog_FormatPerfRndLine(zeroSnap, false, false, 0, rndLine, sizeof(rndLine));
        TEST_CHECK(strstr(mainLine, "tsHook=0.0ms/0.00us(n=0)") != nullptr);
        TEST_CHECK(strstr(rndLine, "fg=0 ") != nullptr);
        TEST_CHECK(strstr(mainLine, "nan") == nullptr && strstr(mainLine, "inf") == nullptr);
        TEST_CHECK(strstr(rndLine, "nan") == nullptr && strstr(rndLine, "inf") == nullptr);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Test 2: World Object References (WorldRefs: Buildings, Constructions, Vehicles, Lines, Nodes)
// ---------------------------------------------------------------------------

bool Test_WorldRefs()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Create 2 completed buildings (list 0 at WORLD_BUILDINGS_OFFSET = 0x11BB0)
    // and 2 unfinished/demolition constructions (list 1 at WORLD_CONSTRUCTIONS_OFFSET = 0x11CD0)
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

    // Vehicles (WORLD_VEHICLES_OFFSET = 0x128C0, ID at +0x04)
    std::vector<BYTE> veh0(64, 0), veh1(64, 0);
    *(unsigned*)(veh0.data() + VEH_ID_OFFSET) = 501u;
    *(unsigned*)(veh1.data() + VEH_ID_OFFSET) = 502u;
    std::vector<BYTE*> vehVec = { veh0.data(), veh1.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehVec);

    RouteRef refV = kNoRef;
    TEST_CHECK(Ref_Vehicle((U64)veh1.data(), refV));
    TEST_CHECK(refV.place == 1 && refV.check == 502u);
    TEST_CHECK(Ref_VehicleAt(refV) == (U64)veh1.data());

    // Lines (WORLD_LINES_OFFSET = 0x128B0 points to a lines manager with 2 vectors at +0x00 and +0x18)
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

    // Nodes (road network at world + 0xF908, rail network at world + 0xFAC0, node vector at +0x298)
    std::vector<BYTE> railNet(0x300, 0), nodeObj(32, 0);
    *(unsigned*)(nodeObj.data() + 4)   = 0x41200000u; // float X
    *(unsigned*)(nodeObj.data() + 0xC) = 0x42200000u; // float Z
    std::vector<BYTE*> nodeVec = { nodeObj.data() };
    WriteMockVectorHeader(railNet.data() + 0x298, nodeVec);
    *(BYTE**)(world + 0xFAC0) = railNet.data();

    RouteRef refN = kNoRef;
    TEST_CHECK(Ref_Node((U64)nodeObj.data(), refN));
    TEST_CHECK(refN.list == 0xFAC0 && refN.place == 0);
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

    // Ref_DefPtr and Ref_DefPtrAt (Resource stride 0x340 and VehicleType stride 0xA0C8)
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

    // Tag 3: Heap-allocated utility & waste Resource* pointer slots at WORLD_RES_PTR_SLOTS_BEGIN_OFF .. END_OFF
    // (e.g. waste_toxic at world + 0xC400 -> slot index (0xC400 - 0xC360) / 8 = 39)
    std::vector<BYTE> fakeWasteToxic(WORLD_RESOURCE_STRIDE, 0);
    strcpy_s((char*)fakeWasteToxic.data(), 32, "waste_toxic");
    const unsigned kToxicSlotIdx = (0xC498u - WORLD_RES_PTR_SLOTS_BEGIN_OFF) / 8u;
    *(U64*)(world + WORLD_RES_PTR_SLOTS_BEGIN_OFF + (size_t)kToxicSlotIdx * 8u) = (U64)fakeWasteToxic.data();
    TEST_CHECK(Ref_DefPtr((U64)fakeWasteToxic.data(), defTag, defIdx) && defTag == 3 && defIdx == kToxicSlotIdx);
    TEST_CHECK(Ref_DefPtrAt(defTag, defIdx, resolvedDef) && resolvedDef == (U64)fakeWasteToxic.data());
    TEST_CHECK(!Ref_DefPtrAt(3, 999, resolvedDef));

    // Tag 4: Waste resource vector at WORLD_WASTE_RESOURCES_OFFSET (world + 0x12060)
    std::vector<BYTE> fakeWasteOther(WORLD_RESOURCE_STRIDE, 0);
    strcpy_s((char*)fakeWasteOther.data(), 32, "waste_other");
    std::vector<BYTE*> wasteVec = { fakeWasteToxic.data(), fakeWasteOther.data() };
    WriteMockVectorHeader(world + WORLD_WASTE_RESOURCES_OFFSET, wasteVec);
    TEST_CHECK(Ref_DefPtr((U64)fakeWasteOther.data(), defTag, defIdx) && defTag == 4 && defIdx == 1u);
    TEST_CHECK(Ref_DefPtrAt(defTag, defIdx, resolvedDef) && resolvedDef == (U64)fakeWasteOther.data());
    TEST_CHECK(!Ref_DefPtrAt(4, 10, resolvedDef));

    bldVec.clear(); conVec.clear(); wasteVec.clear();
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);
    WriteMockVectorHeader(world + WORLD_CONSTRUCTIONS_OFFSET, conVec);
    WriteMockVectorHeader(world + WORLD_WASTE_RESOURCES_OFFSET, wasteVec);
    *(U64*)(world + WORLD_RES_PTR_SLOTS_BEGIN_OFF + (size_t)kToxicSlotIdx * 8u) = 0;
    *(BYTE**)(world + 0xFAC0) = nullptr;
    *(U64*)(world + WORLD_RESOURCES_OFFSET) = 0;
    *(U64*)(world + WORLD_RESOURCES_OFFSET + 8) = 0;
    *(U64*)(world + WORLD_VEHICLE_TYPES_OFFSET) = 0;
    *(U64*)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = 0;

    return true;
}

// ---------------------------------------------------------------------------
// Test 3: Deterministic State Hashing (StateHash_Compute & Per-Object Records)
// ---------------------------------------------------------------------------

bool Test_StateHash()
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
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
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
// Test 48: StateHash, VehicleSoftSync, Legacy Elimination, TwoStates Isolation (Milestone M9.4, Features 37-40)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct LsHash_M9_4 {
    unsigned long long tick;
    StateSummary s;
};
#pragma pack(pop)
static_assert(sizeof(LsHash_M9_4) == 208, "LsHash_M9_4 must be exactly 208 bytes");

static bool SubTest_M9_4_StateSummaryAndHashing()
{
    // Feature 37: StateSummary struct layout and expanded hashing (200 bytes, 8-byte aligned)
    TEST_CHECK(sizeof(StateSummary) == 200);
    TEST_CHECK(sizeof(LsHash_M9_4) == 208);

    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(int*)(world + WORLD_DAY_OFFSET)        = 210;
    *(int*)(world + WORLD_YEAR_OFFSET)       = 1968;
    *(float*)(world + WORLD_PROGRESS_OFFSET) = 0.450f;
    *(double*)(world + WORLD_USD_OFFSET)     = 500000.0;
    *(double*)(world + WORLD_RUB_OFFSET)     = 2000000.0;

    // Base building vector at WORLD_BUILDINGS_OFFSET
    std::vector<BYTE> bld1(BUILDING_STRUCT_BYTES, 0);
    *(int*)(bld1.data() + 0x00) = 101;
    *(float*)(bld1.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    std::vector<BYTE*> bldVec = { bld1.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    // Constructions vector at WORLD_CONSTRUCTIONS_OFFSET (0x11CD0)
    std::vector<BYTE> con1(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> con2(BUILDING_STRUCT_BYTES, 0);
    *(int*)(con1.data() + 0x00) = 501;
    *(float*)(con1.data() + BLD_PROGRESS_OFFSET) = 0.35f;
    *(int*)(con2.data() + 0x00) = 502;
    *(float*)(con2.data() + BLD_PROGRESS_OFFSET) = 0.80f;
    std::vector<BYTE*> conVec = { con1.data(), con2.data() };
    WriteMockVectorHeader(world + WORLD_CONSTRUCTIONS_OFFSET, conVec);

    // Engineering networks at 0xF7A0 .. 0xFAF0 (roads at 0xF7A0, rails at 0xFA28)
    std::vector<BYTE> roadMgr(0x300, 0);
    std::vector<BYTE> roadSeg(0x200, 0);
    roadSeg[SEG_TYPE_OFFSET] = 1;
    *(short*)(roadSeg.data() + SEG_DIRECTION_OFFSET) = 2;
    std::vector<BYTE*> roadSegVec = { roadSeg.data() };
    WriteMockVectorHeader(roadMgr.data() + MGR_SEGMENTS_BEGIN_OFFSET, roadSegVec);
    *(BYTE**)(world + 0xF838) = roadMgr.data();

    // Lines manager at 0x128B0 with lines vector at +0x18
    std::vector<BYTE> linesMgr(0x100, 0);
    std::vector<BYTE> line1(0x80, 0);
    *(int*)(line1.data() + 0x04) = 42;
    *(int*)(line1.data() + 0x08) = 3;
    std::vector<BYTE*> linesList = { line1.data() };
    WriteMockVectorHeader(linesMgr.data() + 0x18, linesList);
    *(BYTE**)(world + WORLD_LINES_OFFSET) = linesMgr.data();

    // Loans vector at 0x10BD0
    SovietLoan loan1 = {};
    loan1.loanType = 1;
    loan1.initialDays = 360;
    loan1.daysRemaining = 360;
    loan1.initialPrincipal = 100000.0f;
    loan1.remainingPrincipal = 100000.0f;
    loan1.yearInterestRate = 0.05f;
    std::vector<SovietLoan> loansVec = { loan1 };
    WriteMockVectorHeader(world + WORLD_LOANS_OFFSET, loansVec);

    // Research unlocks bitmask at 0x1175C and items at 0x11778
    memset(world + WORLD_RESEARCH_UNLOCKS_OFFSET, 0, WORLD_RESEARCH_UNLOCKS_BYTES);
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET) = 0x08; // tech 3 unlocked

    std::vector<BYTE> resBuf(2 * RESEARCH_ITEM_STRIDE, 0);
    *(float*)(resBuf.data() + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.75f;
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();

    // TwoStates per-state balances
    double initRub[2] = { 1200000.0, 800000.0 };
    double initUsd[2] = { 300000.0, 200000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    StateSummary sBase = {};
    TEST_CHECK(StateHash_Compute(&sBase));
    TEST_CHECK(sBase.constructions == 2);
    TEST_CHECK(sBase.lines == 1);
    TEST_CHECK(sBase.loans == 1);
    TEST_CHECK_NEAR(sBase.rubState[0], 1200000.0, 1e-4);
    TEST_CHECK_NEAR(sBase.rubState[1], 800000.0, 1e-4);
    TEST_CHECK_NEAR(sBase.usdState[0], 300000.0, 1e-4);
    TEST_CHECK_NEAR(sBase.usdState[1], 200000.0, 1e-4);
    TEST_CHECK(sBase.hashConstructions != 0);
    TEST_CHECK(sBase.hashNetworks != 0);
    TEST_CHECK(sBase.hashLines != 0);
    TEST_CHECK(sBase.hashLoans != 0);
    TEST_CHECK(sBase.hashResearch != 0);

    // Mutate construction progress -> hashConstructions changes
    *(float*)(con1.data() + BLD_PROGRESS_OFFSET) = 0.50f;
    StateSummary sConMut = {};
    TEST_CHECK(StateHash_Compute(&sConMut));
    TEST_CHECK(sConMut.hashConstructions != sBase.hashConstructions);
    TEST_CHECK(sConMut.hashLines == sBase.hashLines);

    // Mutate loan interest -> hashLoans changes
    loansVec[0].yearInterestRate = 0.08f;
    WriteMockVectorHeader(world + WORLD_LOANS_OFFSET, loansVec);
    StateSummary sLoanMut = {};
    TEST_CHECK(StateHash_Compute(&sLoanMut));
    TEST_CHECK(sLoanMut.hashLoans != sBase.hashLoans);

    // Mutate research progress -> hashResearch changes
    *(float*)(resBuf.data() + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.90f;
    TwoStates_BeforeStep(world);
    TwoStates_AfterStep(world);
    StateSummary sResMut = {};
    TEST_CHECK(StateHash_Compute(&sResMut));
    TEST_CHECK(sResMut.hashResearch != sBase.hashResearch);

    // Compare with LockstepNet_CompareSummaryForTest
    char what[256] = {};
    TEST_CHECK(LockstepNet_CompareSummaryForTest(sBase, sConMut, what, sizeof(what)));
    TEST_CHECK(strstr(what, "con-progress") != nullptr);

    // Mutate construction count -> "constructions " in what
    StateSummary sConCountMut = sBase;
    sConCountMut.constructions++;
    what[0] = '\0';
    TEST_CHECK(LockstepNet_CompareSummaryForTest(sBase, sConCountMut, what, sizeof(what)));
    TEST_CHECK(strstr(what, "constructions") != nullptr);

    what[0] = '\0';
    TEST_CHECK(LockstepNet_CompareSummaryForTest(sBase, sLoanMut, what, sizeof(what)));
    TEST_CHECK(strstr(what, "loans") != nullptr);

    // Pure position drift (hashVehiclesPos) must NOT trigger hard desync
    StateSummary sVehPosOnly = sBase;
    sVehPosOnly.hashVehiclesPos ^= 0xCAFEBABEULL;
    what[0] = '\0';
    TEST_CHECK(!LockstepNet_CompareSummaryForTest(sBase, sVehPosOnly, what, sizeof(what)));
    TEST_CHECK(what[0] == '\0');

    return true;
}

static bool SubTest_M9_4_VehicleSoftSyncRemediation()
{
    // Feature 38: Fuel, speed, 4-class passengers, machinery on construction sites, cargo mismatch resilience
    TEST_CHECK(VEH_FUEL_OFFSET == 0x5F0u);
    TEST_CHECK(VEH_SPEED_OFFSET == 0xD44u);
    TEST_CHECK(sizeof(VehicleSoftSyncEntry_M4) == 148);

    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> bus(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 888;
    *(float*)(bus.data() + VEH_POS_OFFSET + 0) = 150.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 4) = 20.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 8) = 300.0f;
    *(int*)(bus.data() + VEH_SCHED_IDX_OFFSET) = 2;
    *(float*)(bus.data() + VEH_FUEL_OFFSET) = 65.5f;
    *(float*)(bus.data() + VEH_SPEED_OFFSET) = 52.0f;

    struct MockPersonEntry {
        const char* name;
        float amount;
        unsigned int pad;
    };
    static const char kPassLabel[] = "passengers";
    static const char kWorkLabel[] = "workers";
    static const char kStudLabel[] = "students";
    static const char kTourLabel[] = "tourists";
    std::vector<MockPersonEntry> passVector = {
        { kPassLabel, 14.0f, 0 },
        { kWorkLabel, 22.0f, 0 },
        { kStudLabel,  8.0f, 0 },
        { kTourLabel, 11.0f, 0 }
    };
    *(BYTE**)(bus.data() + 0x458) = (BYTE*)passVector.data();
    *(BYTE**)(bus.data() + 0x460) = (BYTE*)(passVector.data() + passVector.size());

    BYTE* vehList[1] = { bus.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    std::vector<BYTE> pkt(4096, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    VehicleSoftSyncMsg_M4* msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);
    TEST_CHECK_NEAR(msg->entries[0].fuel, 65.5f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].speed, 52.0f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].passengers, 14.0f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].workers, 22.0f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].students, 8.0f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].tourists, 11.0f, 1e-4f);

    // Apply remote updates to fuel, speed, and passenger categories
    msg->entries[0].fuel = 80.0f;
    msg->entries[0].speed = 60.0f;
    msg->entries[0].passengers = 16.0f;
    msg->entries[0].workers = 25.0f;
    msg->entries[0].students = 10.0f;
    msg->entries[0].tourists = 15.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));

    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_FUEL_OFFSET), 80.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_SPEED_OFFSET), 60.0f, 1e-4f);
    TEST_CHECK_NEAR(passVector[0].amount, 16.0f, 1e-4f);
    TEST_CHECK_NEAR(passVector[1].amount, 25.0f, 1e-4f);
    TEST_CHECK_NEAR(passVector[2].amount, 10.0f, 1e-4f);
    TEST_CHECK_NEAR(passVector[3].amount, 15.0f, 1e-4f);

    // Machinery on construction site resolution
    std::vector<BYTE> conBld(BUILDING_STRUCT_BYTES, 0);
    *(float*)(conBld.data() + BLD_PROGRESS_OFFSET) = 0.40f;
    std::vector<BYTE> excavator(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(excavator.data() + VEH_STOPPED_BLD_OFFSET) = conBld.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(excavator.data()) == conBld.data());

    // In-transit vehicle with active engine path smoothed via EMA (< 50m drift)
    *(uintptr_t*)(bus.data() + 0x6A0) = 0x1000;
    *(uintptr_t*)(bus.data() + 0x6A8) = 0x1020;
    TEST_CHECK(VehicleSoftSync_HasActiveEnginePathForTest(bus.data()));

    msg->entries[0].pos[0] = 162.0f; // 12m drift
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSyncDiagStats vssStats = {};
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended >= 1);

    return true;
}

static bool SubTest_M9_4_LegacyModeElimination()
{
    // Feature 39: Unconditional lockstep mode, legacy channels eliminated
    TEST_CHECK(SaveSync_GetLockstep() == true);

    SaveSync_SetLockstep(false);
    TEST_CHECK(SaveSync_GetLockstep() == true); // Cannot be turned off

    TEST_CHECK(LOCKSTEP_PORT == 5560);

    return true;
}

static bool SubTest_M9_4_TwoStatesRemediation()
{
    // Feature 40: Demolish checks in neutral zone, tourist spawn rating indices, world switches, research retention
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> resBuf(4 * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < 4; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();

    std::vector<BYTE> univWest(0x1420, 0), univEast(0x1420, 0);
    *(int*)univWest.data() = 501;
    *(float*)(univWest.data() + OBJ_POS_OFFSET) = -800.0f;
    *(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;
    *(int*)univEast.data() = 502;
    *(float*)(univEast.data() + OBJ_POS_OFFSET) = 800.0f;
    *(float*)(univEast.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    std::vector<BYTE*> bldVec = { univWest.data(), univEast.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // Select tech 0 for univWest
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 0));
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == 0);

    // Step university once to accumulate 0.50f progress on tech 0
    TwoStates_BldUnivStepForTest(world, univWest.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), 0.50f, 1e-4f);

    BYTE* it0 = resBuf.data() + 0 * RESEARCH_ITEM_STRIDE;
    BYTE* it1 = resBuf.data() + 1 * RESEARCH_ITEM_STRIDE;

    // University research switch must retain accumulated progress without forcing 1.0f instant completion
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 1)); // switch from tech 0 to tech 1
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), 0.50f, 1e-4f); // Preserved! Not 1.0f!
    TEST_CHECK_NEAR(*(float*)(it0 + RESEARCH_ITEM_PROGRESS_OFFSET), 0.50f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 0); // Not exploit-unlocked!
    TEST_CHECK(*(BYTE**)(it0 + RESEARCH_ITEM_BLD_PTR_OFFSET) == nullptr);
    TEST_CHECK(*(BYTE**)(it1 + RESEARCH_ITEM_BLD_PTR_OFFSET) == univWest.data());

    // Neutral border strip (zone == 0): ownership gate
    int posKey = TwoStates_BuildingPosKey(0.0f, 100.0f);
    TwoStates_RegisterBuildingOwner(posKey, 1);
    TEST_CHECK(TwoStates_CanDemolishAt(0.0f, 100.0f, 1) == true);
    TEST_CHECK(TwoStates_CanDemolishAt(0.0f, 100.0f, 2) == false);

    // Customs tourist spawn calculations with corrected rating indices (USD: trips=5, rate=11; RUB: trips=4, rate=10)
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    // A. USD customs (West, owner 1)
    std::vector<BYTE> bldTypeUsd(0xC00, 0);
    *(uint8_t*)(bldTypeUsd.data() + BLD_TYPE_CAT_OFF) = 0x14;
    *(uint8_t*)(bldTypeUsd.data() + BLD_TYPE_CUSTOMS_SUBTYPE_OFF) = 1; // 1 = USD customs

    std::vector<BYTE> customsUsd(0x1420, 0);
    *(float*)(customsUsd.data() + OBJ_POS_OFFSET) = -1000.0f; // West side
    *(BYTE**)(customsUsd.data() + 0x318) = bldTypeUsd.data();
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 11) = 1; // USD visa unlocked
    TwoStates_SetEngineResearchUnlockByteForTest(1, 11, 1);
    TwoStates_SetStateEconExtraCountForTest(1, 5, 4); // 4 trips (tripIdx 5)
    TwoStates_SetStateEconExtraFloatForTest(1, 11, 18.0f); // 18.0f sum (rateIdx 11) -> avg = 4.5f

    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 5 * sizeof(int32_t)) = 4; // 4 trips
    float ratingUsdSum = 18.0f; // 18.0f sum -> avg = 4.5f
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 11 * sizeof(int32_t), &ratingUsdSum, sizeof(float));

    float avgUsd = 0.0f;
    int32_t spawnUsd = 0;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 4.5f, 1e-3f);
    TEST_CHECK(spawnUsd == 6); // 2 base + 0 marketing + 4 rating = 6

    // B. RUB customs (East, owner 2)
    std::vector<BYTE> bldTypeRub(0xC00, 0);
    *(uint8_t*)(bldTypeRub.data() + BLD_TYPE_CAT_OFF) = 0x14;
    *(uint8_t*)(bldTypeRub.data() + BLD_TYPE_CUSTOMS_SUBTYPE_OFF) = 0; // 0 = Soviet customs

    std::vector<BYTE> customsRub(0x1420, 0);
    *(float*)(customsRub.data() + OBJ_POS_OFFSET) = 1000.0f; // East side
    *(BYTE**)(customsRub.data() + 0x318) = bldTypeRub.data();
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 12) = 1; // RUB visa unlocked
    TwoStates_SetEngineResearchUnlockByteForTest(2, 12, 1);
    TwoStates_SetStateEconExtraCountForTest(2, 4, 5); // 5 trips (tripIdx 4)
    TwoStates_SetStateEconExtraFloatForTest(2, 10, 20.0f); // 20.0f sum (rateIdx 10) -> avg = 4.0f

    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 4 * sizeof(int32_t)) = 5; // 5 trips
    float ratingRubSum = 20.0f; // 20.0f sum -> avg = 4.0f
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 10 * sizeof(int32_t), &ratingRubSum, sizeof(float));

    float avgRub = 0.0f;
    int32_t spawnRub = 0;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsRub.data(), &avgRub, &spawnRub);
    TEST_CHECK_NEAR(avgRub, 4.0f, 1e-3f);
    TEST_CHECK(spawnRub == 6);

    // World settings local-per-state exclusion
    TEST_CHECK(BuildingSwitches_IsLocalPerStateWorldSettingForTest(0xB20));
    TEST_CHECK(BuildingSwitches_IsLocalPerStateWorldSettingForTest(0xB30));
    TEST_CHECK(BuildingSwitches_IsLocalPerStateWorldSettingForTest(0xB33));
    TEST_CHECK(BuildingSwitches_IsLocalPerStateWorldSettingForTest(0xC830));
    TEST_CHECK(BuildingSwitches_IsLocalPerStateWorldSettingForTest(0xC833));
    TEST_CHECK(!BuildingSwitches_IsLocalPerStateWorldSettingForTest(0x1000));

    return true;
}

static bool SubTest_M9_4_StrictResearchSimulationIsolation()
{
    // Mandatory R4 requirement:
    // Research and licenses in TwoStates must remain STRICTLY ISOLATED per republic.
    // In TwoStates_BeforeStep / AfterStep, NO OR-merging or union mixing is allowed.
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> resBuf(4 * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < 4; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();

    memset(world + WORLD_RESEARCH_UNLOCKS_OFFSET, 0, WORLD_RESEARCH_UNLOCKS_BYTES);

    std::vector<BYTE> vehTypesBuf(3 * WORLD_VEHICLE_TYPE_STRIDE, 0);
    BYTE* vt0 = vehTypesBuf.data() + 0 * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vt1 = vehTypesBuf.data() + 1 * WORLD_VEHICLE_TYPE_STRIDE;
    strcpy_s((char*)(vt0 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "t815_dump");
    strcpy_s((char*)(vt1 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "ikarus_260");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x00) = vehTypesBuf.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x08) = vehTypesBuf.data() + vehTypesBuf.size();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x10) = vehTypesBuf.data() + vehTypesBuf.size();

    std::vector<BYTE> univWest(0x1420, 0), univEast(0x1420, 0);
    *(int*)univWest.data() = 501;
    *(float*)(univWest.data() + OBJ_POS_OFFSET) = -800.0f;
    *(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;
    *(int*)univEast.data() = 502;
    *(float*)(univEast.data() + OBJ_POS_OFFSET) = 800.0f;
    *(float*)(univEast.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    std::vector<BYTE*> bldVec = { univWest.data(), univEast.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, false, "127.0.0.1:5556");
    TwoStates_OnWorldLoaded();

    // State 1 acquires License 0 ("t815_dump")
    TwoStates_BeginCommand(1);
    vt0[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    TwoStates_NotifyVehicleLicenseChanged(world);
    TwoStates_EndCommand(1);

    // State 2 acquires License 1 ("ikarus_260")
    TwoStates_BeginCommand(2);
    vt1[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    TwoStates_NotifyVehicleLicenseChanged(world);
    TwoStates_EndCommand(2);

    // Verify initial license isolation
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 0) == true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 1) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 0) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 1) == true);

    // State 1 researches tech 1 to completion (progress 1.0)
    TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 1);
    TwoStates_BldUnivStepForTest(world, univWest.data());
    TwoStates_BldUnivStepForTest(world, univWest.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 1), 1.00f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 1) == 0); // State 2 has NOT unlocked tech 1

    // State 2 researches tech 2 to completion (progress 1.0)
    TwoStates_ApplyUnivResearchSelection(world, univEast.data(), 2, 2);
    TwoStates_BldUnivStepForTest(world, univEast.data());
    TwoStates_BldUnivStepForTest(world, univEast.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 2), 1.00f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 2) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 2) == 0); // State 1 has NOT unlocked tech 2

    // Now test BeforeStep: canonical State 1 slice is mounted for simulation without OR-merging
    TwoStates_BeforeStep(world);

    // Verify simulation baseline on world: reflects State 1, NOT the union of State 1 and State 2!
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 2) == 0);
    TEST_CHECK(vt0[VEH_TYPE_HAS_LICENCE_OFFSET] == 1);
    TEST_CHECK(vt1[VEH_TYPE_HAS_LICENCE_OFFSET] == 0); // NOT merged!

    // Step finishes: AfterStep restores local player's UI slice
    TwoStates_AfterStep(world);

    // Host (State 1) sees its own licenses and research
    TEST_CHECK(vt0[VEH_TYPE_HAS_LICENCE_OFFSET] == 1);
    TEST_CHECK(vt1[VEH_TYPE_HAS_LICENCE_OFFSET] == 0);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 2) == 0);

    // Slices of both states remain bit-exact and isolated
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 0) == true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 1) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 0) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 1) == true);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 1) == 0);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 2) == 1);

    return true;
}

bool Test_StateHashAndTwoStates_M9_4()
{
    printf("    [SubTest 1: StateSummary & Subsystems Hashing (Feature 37)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_StateSummaryAndHashing()) return false;

    printf("    [SubTest 2: VehicleSoftSync Remediation & Resilience (Feature 38)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_VehicleSoftSyncRemediation()) return false;

    printf("    [SubTest 3: Legacy Mode Elimination (Feature 39)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_LegacyModeElimination()) return false;

    printf("    [SubTest 4: TwoStates Demolish & World Switches (Feature 40)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_TwoStatesRemediation()) return false;

    printf("    [SubTest 5: Strict Research & License Simulation Isolation (Feature 40)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_StrictResearchSimulationIsolation()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 49: Stress_M9_4_Challenger1 (Adversarial Stress Testing of Features 37 & 38)
// ---------------------------------------------------------------------------

static bool SubTest_M9_4_Challenger1_StateHash14Networks()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(int*)(world + WORLD_DAY_OFFSET)        = 100;
    *(int*)(world + WORLD_YEAR_OFFSET)       = 1970;
    *(float*)(world + WORLD_PROGRESS_OFFSET) = 0.5f;

    std::vector<BYTE> bld1(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld1.data() = 1;
    *(float*)(bld1.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    std::vector<BYTE*> bldVec = { bld1.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    StateSummary sBase = {};
    TEST_CHECK(StateHash_Compute(&sBase));

    static const unsigned int kNetworks[14] = {
        0xF838, 0xF840, 0xF898, 0xF8F0, 0xF948, 0xF9A0, 0xFA00,
        0xFA58, 0xFAB0, 0xFAB8, 0xFAC8, 0xFAD8, 0xFB30, 0xFB88
    };

    std::vector<BYTE> netMgr(0x300, 0);
    std::vector<BYTE> nodeObj(32, 0);
    std::vector<BYTE*> nodeVec = { nodeObj.data() };
    WriteMockVectorHeader(netMgr.data() + NET_NODES_OFFSET, nodeVec);

    std::vector<BYTE> segObj(0x200, 0);
    segObj[SEG_TYPE_OFFSET] = 1;
    *(short*)(segObj.data() + SEG_DIRECTION_OFFSET) = 0;
    std::vector<BYTE*> segVec = { segObj.data() };
    WriteMockVectorHeader(netMgr.data() + MGR_SEGMENTS_BEGIN_OFFSET, segVec);

    for (unsigned int off : kNetworks) {
        *(BYTE**)(world + off) = netMgr.data();
        StateSummary sNet = {};
        TEST_CHECK(StateHash_Compute(&sNet));
        TEST_CHECK(sNet.hashNetworks != sBase.hashNetworks);

        // Mutate segment type
        segObj[SEG_TYPE_OFFSET] = 7;
        StateSummary sTypeMut = {};
        TEST_CHECK(StateHash_Compute(&sTypeMut));
        TEST_CHECK(sTypeMut.hashNetworks != sNet.hashNetworks);

        // Mutate segment direction
        *(short*)(segObj.data() + SEG_DIRECTION_OFFSET) = 3;
        StateSummary sDirMut = {};
        TEST_CHECK(StateHash_Compute(&sDirMut));
        TEST_CHECK(sDirMut.hashNetworks != sTypeMut.hashNetworks);

        // Reset
        segObj[SEG_TYPE_OFFSET] = 1;
        *(short*)(segObj.data() + SEG_DIRECTION_OFFSET) = 0;
        *(BYTE**)(world + off) = nullptr;
    }
    return true;
}

static bool SubTest_M9_4_Challenger1_StateHashSubsystemsMutations()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(int*)(world + WORLD_DAY_OFFSET)  = 100;
    *(int*)(world + WORLD_YEAR_OFFSET) = 1970;

    std::vector<BYTE> bld1(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld1.data() = 1;
    *(float*)(bld1.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    std::vector<BYTE*> bldVec = { bld1.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    // Constructions
    std::vector<BYTE> con1(BUILDING_STRUCT_BYTES, 0);
    *(int*)con1.data() = 501;
    *(float*)(con1.data() + BLD_PROGRESS_OFFSET) = 0.25f;
    std::vector<BYTE> worker(32, 0);
    std::vector<BYTE*> workerVec = { worker.data() };
    WriteMockVectorHeader(con1.data() + BLD_WORKERS_OFFSET, workerVec);

    std::vector<BYTE> slotBuf(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> resEntry(0x10, 0);
    const char gravelName[] = "gravel";
    *(const char**)resEntry.data() = gravelName;
    *(float*)(resEntry.data() + 0x08) = 10.0f;
    *(BYTE**)slotBuf.data() = resEntry.data();
    *(BYTE**)(slotBuf.data() + 8) = resEntry.data() + 0x10;
    std::vector<BYTE> slotList(BLD_STORAGE_SLOT_SIZE, 0);
    memcpy(slotList.data(), slotBuf.data(), BLD_STORAGE_SLOT_SIZE);
    *(BYTE**)(con1.data() + BLD_STORAGE_OFFSET) = slotList.data();
    *(BYTE**)(con1.data() + BLD_STORAGE_OFFSET + 8) = slotList.data() + BLD_STORAGE_SLOT_SIZE;

    std::vector<BYTE*> conVec = { con1.data() };
    WriteMockVectorHeader(world + WORLD_CONSTRUCTIONS_OFFSET, conVec);

    // Lines
    std::vector<BYTE> linesMgr(0x100, 0);
    std::vector<BYTE> lineA(0x80, 0);
    *(int*)(lineA.data() + 4) = 10;
    *(int*)(lineA.data() + 8) = 2;
    std::vector<BYTE*> lineListA = { lineA.data() };
    WriteMockVectorHeader(linesMgr.data() + 0x00, lineListA);
    std::vector<BYTE> lineB(0x80, 0);
    *(int*)(lineB.data() + 4) = 20;
    *(int*)(lineB.data() + 8) = 4;
    std::vector<BYTE*> lineListB = { lineB.data() };
    WriteMockVectorHeader(linesMgr.data() + 0x18, lineListB);
    *(BYTE**)(world + WORLD_LINES_OFFSET) = linesMgr.data();

    // Loans
    SovietLoan l1 = {};
    l1.loanType = 2;
    l1.initialDays = 180;
    l1.daysRemaining = 120;
    l1.initialPrincipal = 50000.0f;
    l1.remainingPrincipal = 40000.0f;
    l1.yearInterestRate = 0.06f;
    std::vector<SovietLoan> loans = { l1 };
    WriteMockVectorHeader(world + WORLD_LOANS_OFFSET, loans);

    // Research
    memset(world + WORLD_RESEARCH_UNLOCKS_OFFSET, 0, WORLD_RESEARCH_UNLOCKS_BYTES);
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 10) = 1;
    std::vector<BYTE> resItems(3 * RESEARCH_ITEM_STRIDE, 0);
    resItems[0 * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
    *(float*)(resItems.data() + 0 * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.40f;
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resItems.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resItems.data() + resItems.size();

    StateSummary sBase = {};
    TEST_CHECK(StateHash_Compute(&sBase));
    TEST_CHECK(sBase.constructions == 1);
    TEST_CHECK(sBase.lines == 2);
    TEST_CHECK(sBase.loans == 1);

    // Mutate construction workers
    workerVec.clear();
    WriteMockVectorHeader(con1.data() + BLD_WORKERS_OFFSET, workerVec);
    StateSummary sConWorkerMut = {};
    TEST_CHECK(StateHash_Compute(&sConWorkerMut));
    TEST_CHECK(sConWorkerMut.hashConstructions != sBase.hashConstructions);

    // Mutate construction storage slot
    *(float*)(resEntry.data() + 0x08) = 25.0f;
    memcpy(slotList.data(), slotBuf.data(), BLD_STORAGE_SLOT_SIZE);
    StateSummary sConStorageMut = {};
    TEST_CHECK(StateHash_Compute(&sConStorageMut));
    TEST_CHECK(sConStorageMut.hashConstructions != sConWorkerMut.hashConstructions);

    // Mutate loan remaining principal
    loans[0].remainingPrincipal = 25000.0f;
    WriteMockVectorHeader(world + WORLD_LOANS_OFFSET, loans);
    StateSummary sLoanMut = {};
    TEST_CHECK(StateHash_Compute(&sLoanMut));
    TEST_CHECK(sLoanMut.hashLoans != sBase.hashLoans);

    // Mutate research unlock bytes across buffer (WORLD_RESEARCH_UNLOCKS_BYTES = 0x16 = 22 bytes)
    const size_t testUnlockIndices[] = { 0, 5, 11, 21 };
    for (size_t idx : testUnlockIndices) {
        *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + idx) ^= 0x55;
        StateSummary sUnlockMut = {};
        TEST_CHECK(StateHash_Compute(&sUnlockMut));
        TEST_CHECK(sUnlockMut.hashResearch != sBase.hashResearch);
        *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + idx) ^= 0x55;
    }

    // Mutate research item availability
    resItems[0 * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_AVAILABLE_OFFSET] = 0;
    StateSummary sResAvailMut = {};
    TEST_CHECK(StateHash_Compute(&sResAvailMut));
    TEST_CHECK(sResAvailMut.hashResearch != sBase.hashResearch);

    return true;
}

static bool SubTest_M9_4_Challenger1_EvaluateStateSummaryDiffAudit()
{
    StateSummary a = {}, b = {};
    a.day = 10; a.year = 1970; a.dayProgress = 0.5f;
    a.rub = 100000.0; a.usd = 50000.0;
    a.rubState[0] = 60000.0; a.rubState[1] = 40000.0;
    a.usdState[0] = 30000.0; a.usdState[1] = 20000.0;
    a.buildings = 10; a.constructions = 5; a.vehicles = 20; a.citizens = 500; a.lines = 4; a.loans = 2;
    a.hashClock = 0x1111; a.hashBuildings = 0x2222; a.hashConstructions = 0x3333;
    a.hashVehicles = 0x4444; a.hashVehiclesPos = 0x5555; a.hashCitizens = 0x6666;
    a.hashNetworks = 0x7777; a.hashLines = 0x8888; a.hashLoans = 0x9999; a.hashResearch = 0xAAAA;

    b = a;
    char what[512] = {};

    // 1. Identical summaries -> no diff
    TEST_CHECK(!LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(what[0] == '\0');

    // 2. Pure vehicle position drift -> NO hard desync!
    b.hashVehiclesPos ^= 0xCAFEBABEULL;
    TEST_CHECK(!LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(what[0] == '\0');
    b = a;

    // 3. Count divergences
    b.buildings = a.buildings + 1;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "buildings ") != nullptr);
    b = a;

    b.constructions = a.constructions + 1;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "constructions ") != nullptr);
    b = a;

    b.vehicles = a.vehicles + 1;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "vehicles ") != nullptr);
    b = a;

    b.citizens = a.citizens + 5;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "citizens ") != nullptr);
    b = a;

    b.lines = a.lines + 1;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "lines ") != nullptr);
    b = a;

    b.loans = a.loans + 1;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "loans ") != nullptr);
    b = a;

    // 4. Hash divergences
    b.hashClock ^= 0xFF;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "clock ") != nullptr);
    b = a;

    b.hashNetworks ^= 0xFF;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "networks ") != nullptr);
    b = a;

    b.hashResearch ^= 0xFF;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "research ") != nullptr);
    b = a;

    b.hashVehicles ^= 0xFF; // logical diff
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "veh-logical ") != nullptr);
    b = a;

    b.hashConstructions ^= 0xFF; // soft progress
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "con-progress ") != nullptr);
    b = a;

    b.hashBuildings ^= 0xFF; // soft storage
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "bld-storage ") != nullptr);
    b = a;

    b.hashCitizens ^= 0xFF; // soft citizen age/home
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "citizens-state ") != nullptr);
    b = a;

    // 5. Money diffs (> 1.0)
    b.rub += 5.0;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "money ") != nullptr);
    b = a;

    b.rubState[1] += 5.0;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "money ") != nullptr);
    b = a;

    // 6. Compound divergence (4 simultaneous differences)
    b.constructions++;
    b.hashNetworks ^= 0x1234;
    b.hashResearch ^= 0x5678;
    b.usdState[0] += 10.0;
    TEST_CHECK(LockstepNet_CompareSummaryForTest(a, b, what, sizeof(what)));
    TEST_CHECK(strstr(what, "constructions ") != nullptr);
    TEST_CHECK(strstr(what, "networks ") != nullptr);
    TEST_CHECK(strstr(what, "research ") != nullptr);
    TEST_CHECK(strstr(what, "money ") != nullptr);

    return true;
}

static bool SubTest_M9_4_Challenger1_FuelAndSpeedBoundaryFuzzing()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> truck(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(truck.data() + VEH_ID_OFFSET) = 777;
    *(float*)(truck.data() + VEH_POS_OFFSET + 0) = 100.0f;
    *(float*)(truck.data() + VEH_POS_OFFSET + 4) = 10.0f;
    *(float*)(truck.data() + VEH_POS_OFFSET + 8) = 200.0f;
    *(int*)(truck.data() + VEH_SCHED_IDX_OFFSET) = 0;
    *(float*)(truck.data() + VEH_FUEL_OFFSET) = 50.0f;
    *(float*)(truck.data() + VEH_SPEED_OFFSET) = 40.0f;

    BYTE* vehList[1] = { truck.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    std::vector<BYTE> pkt(4096, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    VehicleSoftSyncMsg_M4* msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);

    // 1. Valid fuel boundaries: 0.0f (empty) and 150.0f (full)
    msg->entries[0].fuel = 0.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(*(float*)(truck.data() + VEH_FUEL_OFFSET), 0.0f, 1e-4f);

    msg->entries[0].fuel = 150.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(*(float*)(truck.data() + VEH_FUEL_OFFSET), 150.0f, 1e-4f);

    // 2. Adversarial fuel: negative, NaN, Inf must be REJECTED (retaining 150.0f)
    const float kBadFloats[] = {
        -0.001f, -1.0f, -9999.0f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()
    };
    for (float badVal : kBadFloats) {
        msg->entries[0].fuel = badVal;
        TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
        TEST_CHECK_NEAR(*(float*)(truck.data() + VEH_FUEL_OFFSET), 150.0f, 1e-4f); // Retained!
    }

    // 3. Valid speed boundaries: 0.0f and 90.0f
    msg->entries[0].fuel = 150.0f;
    msg->entries[0].speed = 0.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(*(float*)(truck.data() + VEH_SPEED_OFFSET), 0.0f, 1e-4f);

    msg->entries[0].speed = 90.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(*(float*)(truck.data() + VEH_SPEED_OFFSET), 90.0f, 1e-4f);

    // 4. Adversarial speed: negative, NaN, Inf must be REJECTED (retaining 90.0f)
    for (float badVal : kBadFloats) {
        msg->entries[0].speed = badVal;
        TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
        TEST_CHECK_NEAR(*(float*)(truck.data() + VEH_SPEED_OFFSET), 90.0f, 1e-4f); // Retained!
    }

    return true;
}

static bool SubTest_M9_4_Challenger1_Passenger4ClassesAndCargoAsymmetry()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> bus(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 333;
    *(float*)(bus.data() + VEH_POS_OFFSET + 0) = 200.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 4) = 15.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 8) = 250.0f;
    *(int*)(bus.data() + VEH_SCHED_IDX_OFFSET) = 1;
    *(float*)(bus.data() + VEH_FUEL_OFFSET) = 45.0f;
    *(float*)(bus.data() + VEH_SPEED_OFFSET) = 35.0f;

    // Setup 4 passenger categories in scrambled order
    struct MockPersonEntry {
        const char* name;
        float amount;
        unsigned int pad;
    };
    static const char kTourLabel[] = "tourists";
    static const char kStudLabel[] = "students";
    static const char kWorkLabel[] = "workers";
    static const char kPassLabel[] = "passengers";
    std::vector<MockPersonEntry> passVec = {
        { kTourLabel, 12.0f, 0 },
        { kStudLabel,  9.0f, 0 },
        { kWorkLabel, 28.0f, 0 },
        { kPassLabel, 15.0f, 0 }
    };
    *(BYTE**)(bus.data() + 0x458) = (BYTE*)passVec.data();
    *(BYTE**)(bus.data() + 0x460) = (BYTE*)(passVec.data() + passVec.size());

    // Setup cargo vector with 2 slots
    struct MockCargoEntry {
        const char* res;
        float amount;
        float pad;
    };
    static const char kResGravel[] = "gravel";
    static const char kResSteel[]  = "steel";
    std::vector<MockCargoEntry> cargoVec = {
        { kResGravel, 5.0f, 0.0f },
        { kResSteel, 10.0f, 0.0f }
    };
    *(BYTE**)(bus.data() + VEH_CARGO_OFFSET)     = (BYTE*)cargoVec.data();
    *(BYTE**)(bus.data() + VEH_CARGO_OFFSET + 8) = (BYTE*)(cargoVec.data() + cargoVec.size());

    BYTE* vehList[1] = { bus.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    std::vector<BYTE> pkt(4096, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    VehicleSoftSyncMsg_M4* msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);

    // Verify all 4 captured despite scrambled order
    TEST_CHECK_NEAR(msg->entries[0].passengers, 15.0f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].workers, 28.0f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].students, 9.0f, 1e-4f);
    TEST_CHECK_NEAR(msg->entries[0].tourists, 12.0f, 1e-4f);

    // Update with new values
    msg->entries[0].passengers = 20.0f;
    msg->entries[0].workers = 35.0f;
    msg->entries[0].students = 14.0f;
    msg->entries[0].tourists = 18.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVec[3].amount, 20.0f, 1e-4f); // passengers
    TEST_CHECK_NEAR(passVec[2].amount, 35.0f, 1e-4f); // workers
    TEST_CHECK_NEAR(passVec[1].amount, 14.0f, 1e-4f); // students
    TEST_CHECK_NEAR(passVec[0].amount, 18.0f, 1e-4f); // tourists

    // Adversarial passenger wire input (negative and NaN) must be ignored
    msg->entries[0].passengers = -5.0f;
    msg->entries[0].workers = std::numeric_limits<float>::quiet_NaN();
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVec[3].amount, 20.0f, 1e-4f); // Unchanged!
    TEST_CHECK_NEAR(passVec[2].amount, 35.0f, 1e-4f); // Unchanged!

    // Cargo slot count asymmetry: Peer claims 6 cargo slots, but local vehicle only has 2
    msg->entries[0].cargoCount = 6;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz)); // No crash, no overrun

    // Corrupted cargo count (250) on wire
    msg->entries[0].cargoCount = 250;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz)); // Clamped safely

    return true;
}

static bool SubTest_M9_4_Challenger1_MachineryAndTransitSmoothing()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Construction building
    std::vector<BYTE> conBld(BUILDING_STRUCT_BYTES, 0);
    *(int*)conBld.data() = 901;
    *(float*)(conBld.data() + BLD_PROGRESS_OFFSET) = 0.45f; // Unfinished construction!

    std::vector<BYTE> crane(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(crane.data() + VEH_ID_OFFSET) = 555;
    *(float*)(crane.data() + VEH_SPEED_OFFSET) = 0.0f; // Stationary!
    *(BYTE**)(crane.data() + VEH_STOPPED_BLD_OFFSET) = conBld.data();

    BYTE* vehList[1] = { crane.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    // In regular sweep (immediate = false), active machinery on construction sites MUST NOT be skipped!
    std::vector<BYTE> pkt(4096, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, false));
    VehicleSoftSyncMsg_M4* msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);
    TEST_CHECK(msg->entries[0].vehicleId == 555);

    // Completed building with normal stationary vehicle
    *(float*)(conBld.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(unsigned char*)(crane.data() + VEH_WORKER_SIM_FLAG_OFFSET) = 0;
    sz = 0;
    VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, false);
    msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 0); // Vehicle was skipped as stationary parked!

    // In immediate sweep, even finished-site parked vehicle is captured
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);

    // In-transit position smoothing vs snapping
    std::vector<BYTE> bus(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 777;
    *(float*)(bus.data() + VEH_POS_OFFSET + 0) = 500.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 4) = 20.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 8) = 500.0f;
    *(int*)(bus.data() + VEH_SCHED_IDX_OFFSET) = 0;
    *(float*)(bus.data() + VEH_SPEED_OFFSET) = 30.0f;

    // Active engine path via waypoints
    *(uintptr_t*)(bus.data() + 0x6A0) = 0x2000;
    *(uintptr_t*)(bus.data() + 0x6A8) = 0x2030;
    TEST_CHECK(VehicleSoftSync_HasActiveEnginePathForTest(bus.data()));

    vehList[0] = bus.data();
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    msg = (VehicleSoftSyncMsg_M4*)pkt.data();

    // 10m drift (< 50m): blended
    msg->entries[0].pos[0] = 510.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSyncDiagStats stats = {};
    VehicleSoftSync_GetDiagStats(&stats);
    TEST_CHECK(stats.posBlended >= 1);

    // 65m drift (>= 50m) with active path: NOT snapped
    unsigned long long snappedBefore = stats.posSnapped;
    msg->entries[0].pos[0] = 565.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&stats);
    TEST_CHECK(stats.posSnapped == snappedBefore); // Not snapped while navigating!

    // 65m drift without active path: snapped!
    *(uintptr_t*)(bus.data() + 0x6A0) = 0;
    *(uintptr_t*)(bus.data() + 0x6A8) = 0;
    TEST_CHECK(!VehicleSoftSync_HasActiveEnginePathForTest(bus.data()));
    msg->entries[0].pos[0] = 565.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&stats);
    TEST_CHECK(stats.posSnapped > snappedBefore); // Snapped!

    return true;
}

bool Test_Stress_M9_4_Challenger1()
{
    printf("    [Challenger1-1: Exhaustive 14-Network Sensitivity & Hashing] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_Challenger1_StateHash14Networks()) return false;

    printf("    [Challenger1-2: Subsystem Mutations (Constructions, Lines, Loans, Science)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_Challenger1_StateHashSubsystemsMutations()) return false;

    printf("    [Challenger1-3: EvaluateStateSummaryDiff Completeness & False Negative Audit] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_Challenger1_EvaluateStateSummaryDiffAudit()) return false;

    printf("    [Challenger1-4: Fuel & Speed Boundary Fuzzing (NaN, Inf, Negatives)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_Challenger1_FuelAndSpeedBoundaryFuzzing()) return false;

    printf("    [Challenger1-5: 4-Class Passengers & Cargo Slot Asymmetry Stress] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_Challenger1_Passenger4ClassesAndCargoAsymmetry()) return false;

    printf("    [Challenger1-6: Construction Machinery Capture & In-Transit Smoothing] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_Challenger1_MachineryAndTransitSmoothing()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 50: Stress_M9_4_Challenger2 (Adversarial Stress Testing of Features 39 & 40)
// ---------------------------------------------------------------------------

#define Test_Stress_M9_4_Challenger2 Test_Stress_M9_4_Challenger2_StaticImpl
#include "m9_4_challenger2_stress.cpp"
#undef Test_Stress_M9_4_Challenger2

bool Test_Stress_M9_4_Challenger2()
{
    return Test_Stress_M9_4_Challenger2_StaticImpl();
}

// ---------------------------------------------------------------------------
// Test: Perf_SiteRandFlatTableAndSwitchInPlaceCompare (M3 Phase 2A-2C)
// ---------------------------------------------------------------------------

static inline unsigned long long TestPerf_RefSplitMix(unsigned long long x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static inline int TestPerf_ExpectedSiteRand(unsigned long long seed, unsigned site, unsigned callIdx)
{
    unsigned long long x = TestPerf_RefSplitMix(seed ^ ((unsigned long long)site << 32) ^ callIdx);
    return (int)((x >> 33) & 0x7FFF);
}

static bool SubTest_Perf_SiteRandFlatTable()
{
    ResetMockWorldMemory();
    Determinism_SetActiveForTest(true);
    const unsigned long long kSeed = 0x123456789ABCDEF0ULL;
    Determinism_SetSeedForTest(kSeed);

    // 1. 64 distinct RVAs (63 normal + kSiteEmpty sentinel 0xFFFFFFFFu), interleaved 5 draws each
    std::vector<unsigned> sites(64, 0u);
    for (size_t i = 0; i < 63; ++i) {
        sites[i] = (i == 0) ? 0x6A3D15u : (0x100100u + (unsigned)i * 0x1234u);
    }
    sites[63] = 0xFFFFFFFFu; // kSiteEmpty sentinel must bypass flat slots into overflow

    for (unsigned draw = 0; draw < 5; ++draw) {
        for (size_t i = 0; i < sites.size(); ++i) {
            const int got = Determinism_SiteRandForTest(sites[i]);
            const int exp = TestPerf_ExpectedSiteRand(kSeed, sites[i], draw);
            TEST_CHECK(got == exp);
        }
    }
    unsigned flatCount = 0;
    size_t overflowCount = 0;
    Determinism_GetSiteTableStatsForTest(false, &flatCount, &overflowCount);
    TEST_CHECK(flatCount == 63u);
    TEST_CHECK(overflowCount == 1u);

    // 2. Restart clears flat table + overflow and resets call indices to 0
    Determinism_Restart();
    for (size_t i = 0; i < 64; ++i) {
        const unsigned s = 0x200000u + (unsigned)i * 0x100u;
        TEST_CHECK(Determinism_SiteRandForTest(s) == TestPerf_ExpectedSiteRand(kSeed, s, 0u));
    }
    Determinism_GetSiteTableStatsForTest(false, &flatCount, &overflowCount);
    TEST_CHECK(flatCount == 64u);
    TEST_CHECK(overflowCount == 0u);

    // 3. 1700 distinct RVAs (> kSiteMaxLoad 1536) + hash collisions trigger overflow with bit-exact parity
    Determinism_Restart();
    std::vector<unsigned> manySites;
    manySites.reserve(1700);
    for (unsigned i = 0; i < 1700u; ++i) {
        // Stride by 2048 every 4th entry to force hash/linear-probe collisions too
        const unsigned s = (i % 4u == 0u) ? (0x300000u + (i / 4u) * 2048u) : (0x500000u + i * 17u);
        manySites.push_back(s);
    }
    for (unsigned round = 0; round < 3u; ++round) {
        for (unsigned s : manySites) {
            const int got = Determinism_SiteRandForTest(s);
            const int exp = TestPerf_ExpectedSiteRand(kSeed, s, round);
            TEST_CHECK(got == exp);
        }
    }
    Determinism_GetSiteTableStatsForTest(false, &flatCount, &overflowCount);
    TEST_CHECK(flatCount == 1536u);
    TEST_CHECK(overflowCount >= 164u);

    // 4. Determinism_PrivateScope & g_privateWorker stream isolation
    Determinism_Restart();
    const unsigned kSharedSite = 0x412345u;
    for (unsigned c = 0; c < 3u; ++c) {
        TEST_CHECK(Determinism_SiteRandForTest(kSharedSite) == TestPerf_ExpectedSiteRand(kSeed, kSharedSite, c));
    }
    {
        Determinism_PrivateScope priv(true);
        for (unsigned c = 0; c < 5u; ++c) {
            TEST_CHECK(Determinism_SiteRandForTest(kSharedSite) == TestPerf_ExpectedSiteRand(kSeed, kSharedSite, c));
        }
    }
    // Public stream resumes at callIdx = 3
    TEST_CHECK(Determinism_SiteRandForTest(kSharedSite) == TestPerf_ExpectedSiteRand(kSeed, kSharedSite, 3u));

    Determinism_SetPrivateWorker(GetCurrentThreadId());
    TEST_CHECK(Determinism_SiteRandForTest(kSharedSite) == TestPerf_ExpectedSiteRand(kSeed, kSharedSite, 5u));
    Determinism_SetPrivateWorker(0);
    TEST_CHECK(Determinism_SiteRandForTest(kSharedSite) == TestPerf_ExpectedSiteRand(kSeed, kSharedSite, 4u));

    Determinism_SetActiveForTest(false);
    return true;
}

static bool SubTest_Perf_KeyDownAndFillingEventCache()
{
    ResetMockWorldMemory();
    Determinism_SetActiveForTest(true);

    // 1. 32-entry retAddr cache in H_getKeyDown
    static const uint8_t kDummyCode[256] = {};
    bool hit = true;
    TEST_CHECK(!Determinism_IsKeyDownSuppressedForTest(kDummyCode + 0, &hit));
    TEST_CHECK(!hit); // cold miss
    TEST_CHECK(!Determinism_IsKeyDownSuppressedForTest(kDummyCode + 0, &hit));
    TEST_CHECK(hit);  // warm hit

    // Insert 48 distinct return addresses (more than 32 slots -> evictions handled cleanly)
    for (size_t i = 0; i < 48; ++i) {
        TEST_CHECK(!Determinism_IsKeyDownSuppressedForTest(kDummyCode + i, &hit));
    }
    // Re-query last address: must be a warm cache hit
    TEST_CHECK(!Determinism_IsKeyDownSuppressedForTest(kDummyCode + 47, &hit));
    TEST_CHECK(hit);

    // 2. H_setEvent g_fillingStartEvent fast-path & invalidation
    std::vector<BYTE> fillingObj(0x100, 0);
    HANDLE hStart = (HANDLE)0x1234;
    HANDLE hOther = (HANDLE)0x5678;
    *(uint64_t*)(g_mockExeBuffer + FILLING_OBJ_RVA) = (uint64_t)(uintptr_t)fillingObj.data();
    *(HANDLE*)(fillingObj.data() + FILLING_START_EVENT_OFFSET) = hStart;
    *(BYTE*)(fillingObj.data() + FILLING_QUIT_OFFSET) = 0;

    Determinism_Restart(); // starts with empty g_fillingStartEvent cache
    bool matched = false;
    bool fastSkip = false;

    // First unrelated SetEvent runs slow path and populates g_fillingStartEvent
    TEST_CHECK(!Determinism_SetEventFastPathForTest(hOther, &matched, &fastSkip));
    TEST_CHECK(!matched);
    TEST_CHECK(!fastSkip);

    // Second unrelated SetEvent hits atomic fast-skip (zero SEH)
    TEST_CHECK(!Determinism_SetEventFastPathForTest(hOther, &matched, &fastSkip));
    TEST_CHECK(!matched);
    TEST_CHECK(fastSkip);

    // Matching Filling event runs check and returns true (active wait)
    TEST_CHECK(Determinism_SetEventFastPathForTest(hStart, &matched, &fastSkip));
    TEST_CHECK(matched);
    TEST_CHECK(!fastSkip);

    // Shutdown (quit = 1) clears queues AND invalidates g_fillingStartEvent
    std::vector<BYTE*> fillingVehs = { fillingObj.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, fillingVehs);
    *(BYTE*)(fillingObj.data() + FILLING_QUIT_OFFSET) = 1;
    TEST_CHECK(!Determinism_SetEventFastPathForTest(hStart, &matched, &fastSkip));
    TEST_CHECK(matched);
    TEST_CHECK(*(BYTE***)(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA) ==
               *(BYTE***)(g_mockExeBuffer + VEC_FILLING_VEHICLES_END_RVA));

    // Regression: unrelated SetEvent while quit == 1 must NOT re-cache dying hStart
    TEST_CHECK(!Determinism_SetEventFastPathForTest(hOther, &matched, &fastSkip));
    TEST_CHECK(!matched);
    TEST_CHECK(!fastSkip);

    // Swap to a new startEvent; next SetEvent(hNewStart) directly must NOT fast-skip on stale hStart
    HANDLE hNewStart = (HANDLE)0x9ABC;
    *(HANDLE*)(fillingObj.data() + FILLING_START_EVENT_OFFSET) = hNewStart;
    *(BYTE*)(fillingObj.data() + FILLING_QUIT_OFFSET) = 0;
    TEST_CHECK(Determinism_SetEventFastPathForTest(hNewStart, &matched, &fastSkip));
    TEST_CHECK(matched);
    TEST_CHECK(!fastSkip);

    // Next unrelated SetEvent warms cache to hNewStart, then hits fast-skip
    TEST_CHECK(!Determinism_SetEventFastPathForTest(hOther, &matched, &fastSkip));
    TEST_CHECK(!matched);
    TEST_CHECK(fastSkip);

    // Regression: interleaved H_createEventW(L"FillingrStart") + SetEvent(hOther) before obj+0x60 update
    HANDLE hCreatedSeq = Determinism_CreateEventForTest(L"FillingrStart");
    TEST_CHECK(hCreatedSeq != nullptr);
    TEST_CHECK(!Determinism_SetEventFastPathForTest(hOther, &matched, &fastSkip));
    TEST_CHECK(!matched);
    TEST_CHECK(!fastSkip);
    *(HANDLE*)(fillingObj.data() + FILLING_START_EVENT_OFFSET) = hCreatedSeq;
    TEST_CHECK(Determinism_SetEventFastPathForTest(hCreatedSeq, &matched, &fastSkip));
    TEST_CHECK(matched);
    TEST_CHECK(!fastSkip);
    CloseHandle(hCreatedSeq);

    // Regression: concurrent H_createEventW(L"FillingrStart") vs SetEvent(hOther) stress
    std::atomic<bool> stopWorker{ false };
    std::thread renderHammer([&]() {
        bool m = false;
        bool fs = false;
        while (!stopWorker.load(std::memory_order_acquire)) {
            (void)Determinism_SetEventFastPathForTest(hOther, &m, &fs);
        }
    });
    for (int iter = 0; iter < 100; ++iter) {
        HANDLE hOldIter = (HANDLE)(uintptr_t)(0x20000u + (unsigned)iter * 4u);
        *(HANDLE*)(fillingObj.data() + FILLING_START_EVENT_OFFSET) = hOldIter;
        Determinism_Restart();
        (void)Determinism_SetEventFastPathForTest(hOther, &matched, &fastSkip);
        HANDLE hNewIter = Determinism_CreateEventForTest(L"FillingrStart");
        TEST_CHECK(hNewIter != nullptr);
        YieldProcessor();
        *(HANDLE*)(fillingObj.data() + FILLING_START_EVENT_OFFSET) = hNewIter;
        const bool waitRes = Determinism_SetEventFastPathForTest(hNewIter, &matched, &fastSkip);
        TEST_CHECK(waitRes && matched && !fastSkip);
        CloseHandle(hNewIter);
    }
    stopWorker.store(true, std::memory_order_release);
    renderHammer.join();

    Determinism_SetActiveForTest(false);
    return true;
}

static bool SubTest_Perf_SwitchInPlaceCompareAndBackoff()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    VehicleSwitches_Reset();
    Commands_SetSession(COMMANDS_HOST);

    // Mock building + 1 storage slot
    std::vector<BYTE> bldType(0x400, 0);
    *(int*)(bldType.data() + BLD_TYPE_CAT_OFF) = 0x1C;
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)bld.data() = 201u;
    *(BYTE**)(bld.data() + 0x318) = bldType.data();
    float posB[3] = { 100.0f, 0.0f, 100.0f };
    memcpy(bld.data() + 0x350, posB, sizeof(posB));
    *(uint32_t*)(bld.data() + 0x500) = 0x11112222u;

    std::vector<BYTE> bldSlot(BLD_STORAGE_SLOT_SIZE, 0);
    *(uint32_t*)(bldSlot.data() + 0x50) = 0x22223333u;
    *(BYTE**)(bld.data() + BLD_STORAGE_OFFSET) = bldSlot.data();
    *(BYTE**)(bld.data() + BLD_STORAGE_OFFSET + 8) = bldSlot.data() + BLD_STORAGE_SLOT_SIZE;

    // Source slot target building + source slot entry at +0xF78
    std::vector<BYTE> srcBld1(BUILDING_STRUCT_BYTES, 0), srcBld2(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)srcBld1.data() = 202u;
    *(unsigned*)srcBld2.data() = 203u;
    *(BYTE**)(srcBld1.data() + 0x318) = bldType.data();
    *(BYTE**)(srcBld2.data() + 0x318) = bldType.data();
    float posS1[3] = { 120.0f, 0.0f, 100.0f };
    float posS2[3] = { 140.0f, 0.0f, 100.0f };
    memcpy(srcBld1.data() + 0x350, posS1, sizeof(posS1));
    memcpy(srcBld2.data() + 0x350, posS2, sizeof(posS2));

    struct MockSourceSlot {
        uint64_t source;
        uint64_t kind;
    };
    MockSourceSlot srcSlotEntry = { (uint64_t)(uintptr_t)srcBld1.data(), 1u };
    *(BYTE**)(bld.data() + 0xF78) = (BYTE*)&srcSlotEntry;
    *(BYTE**)(bld.data() + 0xF80) = (BYTE*)(&srcSlotEntry + 1);

    std::vector<BYTE*> mockBlds = { bld.data(), srcBld1.data(), srcBld2.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    // Mock vehicle + 1 stop record (0x198) + 1 cargo record (0x10)
    std::vector<BYTE> veh(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(veh.data() + VEH_ID_OFFSET) = 601u;
    *(uint32_t*)(veh.data() + 0x610) = 0x33334444u;
    std::vector<BYTE> stopRec(0x198, 0);
    *(uint32_t*)(stopRec.data() + 0x20) = 0x44445555u;
    *(BYTE**)(veh.data() + 0x638) = stopRec.data();
    *(BYTE**)(veh.data() + 0x640) = stopRec.data() + stopRec.size();
    std::vector<BYTE> cargoRec(0x10, 0);
    *(uint32_t*)(cargoRec.data() + 0x0C) = 1u;
    *(BYTE**)(veh.data() + 0x3B8) = cargoRec.data();
    *(BYTE**)(veh.data() + 0x3C0) = cargoRec.data() + cargoRec.size();
    std::vector<BYTE*> mockVehs = { veh.data() };
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_VEHICLES_OFFSET, mockVehs);

    // Mock open windows vector at WORLD_OPEN_WINDOWS_BEGIN_RVA (0x9E19D0)
    std::vector<BYTE> win0(0x300, 0), win1(0x300, 0);
    *(uint64_t*)(win0.data() + SWITCH_WINDOW_OBJECT_OFF) = (uint64_t)(uintptr_t)bld.data();
    *(unsigned*)(win0.data() + SWITCH_WINDOW_KIND_OFF) = 1u;
    *(uint64_t*)(win1.data() + SWITCH_WINDOW_OBJECT_OFF) = (uint64_t)(uintptr_t)veh.data();
    *(unsigned*)(win1.data() + SWITCH_WINDOW_KIND_OFF) = 2u;
    std::vector<BYTE*> openWins = { win0.data(), win1.data() };
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_OPEN_WINDOWS_BEGIN_RVA, openWins);

    // 1. 31 clean idle samples -> sampleEvery stays 16, zero mismatch copies
    SwitchPerfStatsForTest bsInit = {}, vsInit = {};
    BuildingSwitches_GetStatsForTest(&bsInit);
    VehicleSwitches_GetStatsForTest(&vsInit);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;
    for (int f = 0; f < 31 * 16; ++f) {
        BuildingSwitches_Before(false);
        BuildingSwitches_After(false);
        VehicleSwitches_Before(false);
        VehicleSwitches_After(false);
    }
    SwitchPerfStatsForTest bs = {}, vs = {};
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.sampleEvery == 16u && bs.cleanSamples == 31u);
    TEST_CHECK(vs.sampleEvery == 16u && vs.cleanSamples == 31u);
    TEST_CHECK(bs.mismatchCopyCount == 0 && bs.cmpScratchCapacity == bsInit.cmpScratchCapacity);
    TEST_CHECK(vs.mismatchCopyCount == 0 && vs.cmpScratchCapacity == vsInit.cmpScratchCapacity);

    // 2. 32nd clean sample engages 128-frame idle backoff
    for (int f = 0; f < 16; ++f) {
        BuildingSwitches_Before(false);
        BuildingSwitches_After(false);
        VehicleSwitches_Before(false);
        VehicleSwitches_After(false);
    }
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.sampleEvery == 128u && bs.cleanSamples == 32u);
    TEST_CHECK(vs.sampleEvery == 128u && vs.cleanSamples == 32u);

    // 3. 127 frames idle with unchanged WINDOWS_RVA must NOT snapshot; 128th frame snapshots
    const unsigned long long bSnapsAtBackoff = bs.snapshotCount;
    const unsigned long long vSnapsAtBackoff = vs.snapshotCount;
    for (int f = 0; f < 127; ++f) {
        BuildingSwitches_Before(false);
        BuildingSwitches_After(false);
        VehicleSwitches_Before(false);
        VehicleSwitches_After(false);
    }
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.snapshotCount == bSnapsAtBackoff);
    TEST_CHECK(vs.snapshotCount == vSnapsAtBackoff);

    BuildingSwitches_Before(false);
    BuildingSwitches_After(false);
    VehicleSwitches_Before(false);
    VehicleSwitches_After(false);
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.snapshotCount == bSnapsAtBackoff + 1ull);
    TEST_CHECK(vs.snapshotCount == vSnapsAtBackoff + 1ull);

    // 4. Changing WINDOWS_RVA window object/list resets sampleEvery back to 16 immediately
    *(uint64_t*)(win0.data() + SWITCH_WINDOW_OBJECT_OFF) = (uint64_t)(uintptr_t)srcBld1.data();
    BuildingSwitches_Before(false);
    BuildingSwitches_After(false);
    VehicleSwitches_Before(false);
    VehicleSwitches_After(false);
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.sampleEvery == 16u && bs.cleanSamples == 0u);
    TEST_CHECK(vs.sampleEvery == 16u && vs.cleanSamples == 0u);

    // Re-engage 128-frame backoff, then test click instant reset + mismatch copy/revert + scratch reuse
    for (int f = 0; f < 32 * 16; ++f) {
        BuildingSwitches_Before(false);
        BuildingSwitches_After(false);
        VehicleSwitches_Before(false);
        VehicleSwitches_After(false);
    }
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.sampleEvery == 128u && vs.sampleEvery == 128u);

    g_mockExeBuffer[SWITCH_PRESS_RVA] = 1;
    BuildingSwitches_Before(false);
    VehicleSwitches_Before(false);
    SourceSlots_Before(false);

    *(uint32_t*)(bld.data() + 0x500) = 0x3F800000u;
    *(uint32_t*)(bldSlot.data() + 0x50) = 0x40000000u;
    *(uint32_t*)(veh.data() + 0x610) = 0x40400000u;
    *(uint32_t*)(stopRec.data() + 0x20) = 0x40800000u;
    srcSlotEntry.source = (uint64_t)(uintptr_t)srcBld2.data();

    BuildingSwitches_After(false);
    VehicleSwitches_After(false);
    SourceSlots_After(false);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;

    // Verify local memory reverted & commands submitted
    TEST_CHECK(*(uint32_t*)(bld.data() + 0x500) == 0x11112222u);
    TEST_CHECK(*(uint32_t*)(bldSlot.data() + 0x50) == 0x22223333u);
    TEST_CHECK(*(uint32_t*)(veh.data() + 0x610) == 0x33334444u);
    TEST_CHECK(*(uint32_t*)(stopRec.data() + 0x20) == 0x44445555u);
    TEST_CHECK(srcSlotEntry.source == (uint64_t)(uintptr_t)srcBld1.data());

    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.sampleEvery == 16u);
    TEST_CHECK(vs.sampleEvery == 16u);
    TEST_CHECK(bs.mismatchCopyCount == 2ull && bs.cmpScratchCapacity >= BUILDING_STRUCT_BYTES);
    TEST_CHECK(vs.mismatchCopyCount == 2ull && vs.cmpScratchCapacity >= VEHICLE_STRUCT_BYTES);

    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(uint32_t*)(bld.data() + 0x500) == 0x3F800000u);
    TEST_CHECK(*(uint32_t*)(bldSlot.data() + 0x50) == 0x40000000u);
    TEST_CHECK(*(uint32_t*)(veh.data() + 0x610) == 0x40400000u);
    TEST_CHECK(*(uint32_t*)(stopRec.data() + 0x20) == 0x40800000u);
    TEST_CHECK(srcSlotEntry.source == (uint64_t)(uintptr_t)srcBld2.data());

    // Subsequent clean clicked frame reuses cmpScratchCapacity without incrementing mismatchCopyCount
    const size_t prevBCap = bs.cmpScratchCapacity;
    const size_t prevVCap = vs.cmpScratchCapacity;
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 1;
    BuildingSwitches_Before(false);
    BuildingSwitches_After(false);
    VehicleSwitches_Before(false);
    VehicleSwitches_After(false);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.mismatchCopyCount == 2ull && bs.cmpScratchCapacity == prevBCap);
    TEST_CHECK(vs.mismatchCopyCount == 2ull && vs.cmpScratchCapacity == prevVCap);

    // 5. SEH fault isolation with PAGE_NOACCESS / bad pointers alongside valid objects
    BYTE* badPtr = (BYTE*)0xDEAD000000001000ULL;
    mockBlds = { badPtr, bld.data(), srcBld1.data(), srcBld2.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    mockVehs = { badPtr, veh.data() };
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_VEHICLES_OFFSET, mockVehs);

    g_mockExeBuffer[SWITCH_PRESS_RVA] = 1;
    BuildingSwitches_Before(false);
    VehicleSwitches_Before(false);
    SourceSlots_Before(false);
    *(uint32_t*)(bld.data() + 0x500) = 0x41000000u;
    *(uint32_t*)(veh.data() + 0x610) = 0x41200000u;
    srcSlotEntry.source = (uint64_t)(uintptr_t)srcBld1.data();
    BuildingSwitches_After(false);
    VehicleSwitches_After(false);
    SourceSlots_After(false);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;

    TEST_CHECK(*(uint32_t*)(bld.data() + 0x500) == 0x3F800000u);
    TEST_CHECK(*(uint32_t*)(veh.data() + 0x610) == 0x40400000u);
    TEST_CHECK(srcSlotEntry.source == (uint64_t)(uintptr_t)srcBld2.data());

    SourceSlotsPerfStatsForTest sss = {};
    SourceSlots_GetStatsForTest(&sss);
    TEST_CHECK(sss.collectCount >= 2ull && sss.listScratchCapacity >= 4u);

    // 6. >64 open windows (70 windows): window #65 +0x240 mutation resets backoff 128 -> 16
    std::vector<std::vector<BYTE>> manyWins(70, std::vector<BYTE>(0x300, 0));
    std::vector<BYTE*> manyWinPtrs(70, nullptr);
    for (size_t i = 0; i < 70; ++i) {
        *(uint64_t*)(manyWins[i].data() + SWITCH_WINDOW_OBJECT_OFF) = (uint64_t)(uintptr_t)bld.data();
        *(unsigned*)(manyWins[i].data() + SWITCH_WINDOW_KIND_OFF) = 1u;
        manyWinPtrs[i] = manyWins[i].data();
    }
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_OPEN_WINDOWS_BEGIN_RVA, manyWinPtrs);
    BuildingSwitches_Reset();
    VehicleSwitches_Reset();
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;
    for (int f = 0; f < 32 * 16; ++f) {
        BuildingSwitches_Before(false);
        BuildingSwitches_After(false);
        VehicleSwitches_Before(false);
        VehicleSwitches_After(false);
    }
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.sampleEvery == 128u && vs.sampleEvery == 128u);

    *(uint64_t*)(manyWins[64].data() + SWITCH_WINDOW_OBJECT_OFF) = (uint64_t)(uintptr_t)srcBld1.data();
    BuildingSwitches_Before(false);
    BuildingSwitches_After(false);
    VehicleSwitches_Before(false);
    VehicleSwitches_After(false);
    BuildingSwitches_GetStatsForTest(&bs);
    VehicleSwitches_GetStatsForTest(&vs);
    TEST_CHECK(bs.sampleEvery == 16u && bs.cleanSamples == 0u);
    TEST_CHECK(vs.sampleEvery == 16u && vs.cleanSamples == 0u);

    // 7. Invalid WINDOWS_RVA ({PAGE_NOACCESS, +8}) must still sample at 1/16 (no starvation)
    BYTE* noAccessWinPage = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    TEST_CHECK(noAccessWinPage != nullptr);
    if (noAccessWinPage) {
        *(uint64_t*)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_BEGIN_RVA) = (uint64_t)(uintptr_t)noAccessWinPage;
        *(uint64_t*)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_END_RVA) = (uint64_t)(uintptr_t)(noAccessWinPage + 8);
        BuildingSwitches_Reset();
        VehicleSwitches_Reset();
        for (int f = 0; f < 64; ++f) {
            BuildingSwitches_Before(false);
            BuildingSwitches_After(false);
            VehicleSwitches_Before(false);
            VehicleSwitches_After(false);
        }
        BuildingSwitches_GetStatsForTest(&bs);
        VehicleSwitches_GetStatsForTest(&vs);
        TEST_CHECK(bs.snapshotCount >= 4ull && bs.sampleEvery == 16u);
        TEST_CHECK(vs.snapshotCount >= 4ull && vs.sampleEvery == 16u);
        VirtualFree(noAccessWinPage, 0, MEM_RELEASE);
    }

    Commands_SetSession(COMMANDS_OFF);
    BuildingSwitches_Reset();
    VehicleSwitches_Reset();
    mockBlds.clear();
    mockVehs.clear();
    openWins.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_VEHICLES_OFFSET, mockVehs);
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_OPEN_WINDOWS_BEGIN_RVA, openWins);
    return true;
}

static bool SubTest_Perf_SettingsRoadAnimCoalescedSeh()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    SettingsSync_LockstepReset();
    HeldFrame_Reset();

    // 1. SettingsSync coalesced SEH + actual-size g_lsList with badBld resilience
    std::vector<BYTE> typeBld(0x400, 0);
    *(int*)(typeBld.data() + BLD_TYPE_CAT_OFF) = 0x1C;
    std::vector<BYTE> bld1(BUILDING_STRUCT_BYTES, 0), bld2(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bld1.data() + BLD_TYPE_OFFSET) = typeBld.data();
    *(BYTE**)(bld2.data() + BLD_TYPE_OFFSET) = typeBld.data();
    float pos1[3] = { 10.0f, 0.0f, 10.0f };
    float pos2[3] = { 20.0f, 0.0f, 20.0f };
    memcpy(bld1.data() + OBJ_POS_OFFSET, pos1, sizeof(pos1));
    memcpy(bld2.data() + OBJ_POS_OFFSET, pos2, sizeof(pos2));
    *(float*)(bld1.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.25f;
    *(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.40f;

    BYTE* badBld = (BYTE*)0xDEAD000000002000ULL;
    std::vector<BYTE*> mixedBlds = { bld1.data(), badBld, nullptr, bld2.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mixedBlds);

    Commands_SetSession(COMMANDS_HOST);
    SettingsSync_LockstepBaseline();
    *(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.90f;
    SettingsSync_LockstepDetect();
    TEST_CHECK_NEAR(*(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET), 0.40f, 1e-5f);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK_NEAR(*(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET), 0.90f, 1e-5f);
    SettingsSync_LockstepReset();

    // 2. RoadSync_LockstepDirections sorted vector baseline + detect + revert
    const RoadSyncCategoryInfo* fcInfo = RoadSync_GetCategoryInfo(ROAD_SYNC_CATEGORY_FACTORYCONN);
    TEST_CHECK(fcInfo != nullptr);
    std::vector<BYTE> fcMgr(0x300, 0);
    std::vector<std::vector<BYTE>> fcNodes(8, std::vector<BYTE>(0x80, 0));
    std::vector<std::vector<BYTE>> fcSegs(4, std::vector<BYTE>(0x200, 0));
    std::vector<BYTE*> fcSegPtrs(4, nullptr);
    for (int i = 0; i < 4; ++i) {
        *(float*)(fcNodes[i * 2 + 0].data() + 4) = 100.0f + (float)i * 20.0f;
        *(float*)(fcNodes[i * 2 + 1].data() + 4) = 110.0f + (float)i * 20.0f;
        *(BYTE**)(fcSegs[i].data() + 0x20) = fcNodes[i * 2 + 0].data();
        *(BYTE**)(fcSegs[i].data() + 0x28) = fcNodes[i * 2 + 1].data();
        *(int*)(fcSegs[i].data() + SEG_DIRECTION_OFFSET) = (i % 2) + 1;
        fcSegPtrs[i] = fcSegs[i].data();
    }
    *(BYTE***)(fcMgr.data() + MGR_SEGMENTS_BEGIN_OFFSET) = fcSegPtrs.data();
    *(BYTE***)(fcMgr.data() + MGR_SEGMENTS_END_OFFSET) = fcSegPtrs.data() + fcSegPtrs.size();
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + fcInfo->managerOffset) = fcMgr.data();

    RoadSync_LockstepDirectionsBaseline();
    *(int*)(fcSegs[1].data() + SEG_DIRECTION_OFFSET) = 0;
    *(int*)(fcSegs[3].data() + SEG_DIRECTION_OFFSET) = 1;
    RoadSync_LockstepDirectionsDetect();
    TEST_CHECK(*(int*)(fcSegs[1].data() + SEG_DIRECTION_OFFSET) == 2);
    TEST_CHECK(*(int*)(fcSegs[3].data() + SEG_DIRECTION_OFFSET) == 2);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(int*)(fcSegs[1].data() + SEG_DIRECTION_OFFSET) == 0);
    TEST_CHECK(*(int*)(fcSegs[3].data() + SEG_DIRECTION_OFFSET) == 1);

    // 3. HeldFrame building animations batch outer SEH + submesh == nullptr skip + badBld isolation
    std::vector<BYTE> animCtrl(0x100, 0);
    *(int32_t*)(animCtrl.data() + ANIM_CTRL_MAX_FRAMES_OFFSET) = 100;
    std::vector<BYTE> submesh(0x200, 0);
    *(BYTE**)(submesh.data() + MESH_ANIM_CTRL_OFFSET) = animCtrl.data();
    std::vector<BYTE> animType(0x450, 0);
    *(BYTE**)(animType.data() + BLD_TYPE_ANIM_SUBMESH_OFFSET) = submesh.data();
    *(float*)(animType.data() + BLD_TYPE_ANIM_SPEED_OFFSET) = 1.0f;

    std::vector<BYTE> animBld(BUILDING_STRUCT_BYTES, 0);
    *(float*)(animBld.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(BYTE**)(animBld.data() + BLD_TYPE_OFFSET) = animType.data();
    *(BYTE**)(animBld.data() + BLD_ANIM_SUBMESH_OFFSET) = submesh.data();
    *(float*)(animBld.data() + BLD_ANIM_FRAME_OFFSET) = 10.0f;
    *(float*)(animBld.data() + BLD_PRODUCED_OFFSET) = 5.0f;

    std::vector<BYTE> staticBld(BUILDING_STRUCT_BYTES, 0);
    *(float*)(staticBld.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(BYTE**)(staticBld.data() + BLD_TYPE_OFFSET) = animType.data();
    *(BYTE**)(staticBld.data() + BLD_ANIM_SUBMESH_OFFSET) = nullptr;
    *(float*)(staticBld.data() + BLD_ANIM_FRAME_OFFSET) = 7.0f;

    std::vector<BYTE*> animList = { badBld, staticBld.data(), animBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_BUILDINGS_OFFSET, animList);
    Determinism_SetActiveForTest(true);
    HeldFrame_Reset();

    HeldFrame_BeforeEarly();
    TEST_CHECK_NEAR(*(float*)(animBld.data() + BLD_ANIM_FRAME_OFFSET), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(staticBld.data() + BLD_ANIM_FRAME_OFFSET), 7.0f, 1e-5f);

    *(int*)(g_mockExeBuffer + BLD_PASS_BUCKET_COUNTER_RVA) = 2; // listIndex 2 matches activeBucket 2
    *(float*)(animBld.data() + BLD_ANIM_FRAME_OFFSET) = 3.0f;
    HeldFrame_AfterEarly();
    TEST_CHECK_NEAR(*(float*)(animBld.data() + BLD_ANIM_FRAME_OFFSET), 10.0f, 1e-4f);

    HeldFrame_StepBuildingAnimations(60.0f);
    TEST_CHECK_NEAR(*(float*)(animBld.data() + BLD_ANIM_FRAME_OFFSET), 10.5f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(staticBld.data() + BLD_ANIM_FRAME_OFFSET), 7.0f, 1e-5f);

    Determinism_SetActiveForTest(false);
    HeldFrame_Reset();
    Commands_SetSession(COMMANDS_OFF);
    mixedBlds.clear();
    animList.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mixedBlds);
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_BUILDINGS_OFFSET, animList);
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + fcInfo->managerOffset) = nullptr;
    return true;
}

struct MidLoopFaultCtx {
    BYTE* faultBld;
};

static void MidLoopFaultPassBody(void* rawCtx)
{
    MidLoopFaultCtx* ctx = (MidLoopFaultCtx*)rawCtx;
    DWORD oldProt = 0;
    (void)VirtualProtect(ctx->faultBld, 0x2000, PAGE_NOACCESS, &oldProt);
}

static bool SubTest_Perf_RestoreVisibleKeptMidLoopFault()
{
    BYTE* bld0 = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE* bld1 = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE* bld2 = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    TEST_CHECK(bld0 != nullptr && bld1 != nullptr && bld2 != nullptr);
    if (!bld0 || !bld1 || !bld2) {
        if (bld0) VirtualFree(bld0, 0, MEM_RELEASE);
        if (bld1) VirtualFree(bld1, 0, MEM_RELEASE);
        if (bld2) VirtualFree(bld2, 0, MEM_RELEASE);
        return false;
    }

    VisibleKept kept[3] = {
        { bld0, 0, 777.0f },
        { bld1, 0, 888.0f },
        { bld2, 0, 999.0f },
    };
    bld0[0x115A] = 1;
    *(float*)(bld0 + 0x115C) = 123.5f;
    bld1[0x115A] = 1;
    *(float*)(bld1 + 0x115C) = 123.5f;
    bld2[0x115A] = 1;
    *(float*)(bld2 + 0x115C) = 123.5f;

    MidLoopFaultCtx ctx = { bld1 };
    bool inPassAfter = true;
    SimPatches_TestBuildingRenderPassRestore(kept, 3, &MidLoopFaultPassBody, &ctx, &inPassAfter);

    TEST_CHECK(bld0[0x115A] == 0);
    TEST_CHECK_NEAR(*(float*)(bld0 + 0x115C), 777.0f, 1e-5f);
    TEST_CHECK(bld2[0x115A] == 0);
    TEST_CHECK_NEAR(*(float*)(bld2 + 0x115C), 999.0f, 1e-5f);
    TEST_CHECK(!inPassAfter);

    VirtualFree(bld0, 0, MEM_RELEASE);
    VirtualFree(bld1, 0, MEM_RELEASE);
    VirtualFree(bld2, 0, MEM_RELEASE);
    return true;
}

static bool SubTest_Perf_SettingsPartialFaultIsolation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    SettingsSync_LockstepReset();

    BYTE* noAccessPage = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    TEST_CHECK(noAccessPage != nullptr);
    if (!noAccessPage) return false;

    // A. FIELD_SLOT0 PAGE_NOACCESS on category 0x1F with nonzero pay/stopped: 0 false commands, preserves pay/stopped
    std::vector<BYTE> type1F(0x400, 0);
    *(int*)(type1F.data() + BLD_TYPE_CAT_OFF) = 0x1F;
    std::vector<BYTE> bld1F(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bld1F.data() + BLD_TYPE_OFFSET) = type1F.data();
    float pos1F[3] = { 30.0f, 0.0f, 10.0f };
    memcpy(bld1F.data() + OBJ_POS_OFFSET, pos1F, sizeof(pos1F));
    *(unsigned short*)(bld1F.data() + 0x608) = 100u;
    *(bld1F.data() + BLD_STOPPED_OFFSET) = 1u;
    *(BYTE**)(bld1F.data() + BLD_STORAGE_OFFSET) = noAccessPage;
    *(BYTE**)(bld1F.data() + BLD_STORAGE_OFFSET + 8) = noAccessPage + BLD_STORAGE_SLOT_SIZE;

    std::vector<BYTE*> bldVec1F = { bld1F.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, bldVec1F);
    Commands_SetSession(COMMANDS_HOST);
    SettingsSync_LockstepBaseline();
    SettingsSync_LockstepDetect();
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    TEST_CHECK(*(unsigned short*)(bld1F.data() + 0x608) == 100u);
    TEST_CHECK(*(bld1F.data() + BLD_STOPPED_OFFSET) == 1u);
    SettingsSync_LockstepReset();

    // B. 2-baseline reuse of g_lsBuildings when slot 0 faults on 2nd Baseline()
    std::vector<BYTE> resTable(WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC348) = resTable.data();
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC350) = resTable.data() + resTable.size();

    std::vector<BYTE> type4A(0x400, 0);
    *(int*)(type4A.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    std::vector<BYTE> bld4A(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bld4A.data() + BLD_TYPE_OFFSET) = type4A.data();
    float pos4A[3] = { 50.0f, 0.0f, 50.0f };
    memcpy(bld4A.data() + OBJ_POS_OFFSET, pos4A, sizeof(pos4A));
    *(float*)(bld4A.data() + BLD_UPBRINGING_SLIDERS_OFFSET) = 0.90f;

    std::vector<BYTE> slotsBuf(2u * BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> slot0Entries(0x10, 0);
    std::vector<BYTE> slot1Entries(0x10, 0);
    *(BYTE**)(slot0Entries.data() + 0x00) = resTable.data();
    *(float*)(slot0Entries.data() + 0x08) = 0.50f;
    *(float*)(slot0Entries.data() + 0x0C) = 1.0f;
    *(BYTE**)(slot1Entries.data() + 0x00) = resTable.data();
    *(float*)(slot1Entries.data() + 0x08) = 0.20f;
    *(float*)(slot1Entries.data() + 0x0C) = 1.0f;
    *(BYTE**)(slotsBuf.data() + 0u * BLD_STORAGE_SLOT_SIZE + 0x18) = slot0Entries.data();
    *(BYTE**)(slotsBuf.data() + 0u * BLD_STORAGE_SLOT_SIZE + 0x20) = slot0Entries.data() + 0x10;
    *(BYTE**)(slotsBuf.data() + 1u * BLD_STORAGE_SLOT_SIZE + 0x18) = slot1Entries.data();
    *(BYTE**)(slotsBuf.data() + 1u * BLD_STORAGE_SLOT_SIZE + 0x20) = slot1Entries.data() + 0x10;
    *(BYTE**)(bld4A.data() + BLD_STORAGE_OFFSET) = slotsBuf.data();
    *(BYTE**)(bld4A.data() + BLD_STORAGE_OFFSET + 8) = slotsBuf.data() + slotsBuf.size();

    std::vector<BYTE*> bldVec4A = { bld4A.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, bldVec4A);
    SettingsSync_LockstepBaseline();

    // Without Reset(), fault slot 0 before 2nd Baseline() reusing g_lsBuildings[0]
    *(BYTE**)(slotsBuf.data() + 0u * BLD_STORAGE_SLOT_SIZE + 0x18) = noAccessPage;
    *(BYTE**)(slotsBuf.data() + 0u * BLD_STORAGE_SLOT_SIZE + 0x20) = noAccessPage + 0x10;
    SettingsSync_LockstepBaseline();

    *(float*)(slot1Entries.data() + 0x08) = 0.70f;
    *(float*)(bld4A.data() + BLD_UPBRINGING_SLIDERS_OFFSET) = 0.10f;
    SettingsSync_LockstepDetect();
    TEST_CHECK_NEAR(*(float*)(slot1Entries.data() + 0x08), 0.20f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(bld4A.data() + BLD_UPBRINGING_SLIDERS_OFFSET), 0.90f, 1e-5f);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK_NEAR(*(float*)(slot1Entries.data() + 0x08), 0.70f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(bld4A.data() + BLD_UPBRINGING_SLIDERS_OFFSET), 0.10f, 1e-5f);

    SettingsSync_LockstepReset();
    Commands_SetSession(COMMANDS_OFF);
    bldVec4A.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, bldVec4A);
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC348) = nullptr;
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC350) = nullptr;
    VirtualFree(noAccessPage, 0, MEM_RELEASE);
    return true;
}

bool TestPerf_SiteRandFlatTableAndSwitchInPlaceCompare()
{
    printf("    [Perf 2C-1: SiteRand 2048-Slot Flat Table Parity, Collisions, Overflow & Scopes] ...\n"); fflush(stdout);
    if (!SubTest_Perf_SiteRandFlatTable()) return false;

    printf("    [Perf 2C-2: H_getKeyDown 32-Entry Cache & H_setEvent Filling Fast-Path] ...\n"); fflush(stdout);
    if (!SubTest_Perf_KeyDownAndFillingEventCache()) return false;

    printf("    [Perf 2B: Switch In-Place memcmp, Scratch Reuse, SEH Isolation & 16->128 Backoff] ...\n"); fflush(stdout);
    if (!SubTest_Perf_SwitchInPlaceCompareAndBackoff()) return false;

    printf("    [Perf 2C-3: SettingsSync, RoadSync Directions & HeldFrame Building Anim Coalesced SEH] ...\n"); fflush(stdout);
    if (!SubTest_Perf_SettingsRoadAnimCoalescedSeh()) return false;

    printf("    [Perf 2A-Reg: RestoreVisibleKeptSafe Mid-Loop Fault & Flag Reset] ...\n"); fflush(stdout);
    if (!SubTest_Perf_RestoreVisibleKeptMidLoopFault()) return false;

    printf("    [Perf 2C-Reg: SettingsSync Partial Fault & 2-Baseline Slot Isolation] ...\n"); fflush(stdout);
    if (!SubTest_Perf_SettingsPartialFaultIsolation()) return false;

    return true;
}

bool SubTest_M6_HeldFrameCoalescedGuardAndVisualParity()
{
    ResetMockWorldMemory();
    HeldFrame_Reset();

    constexpr float kPi = 3.14159265358979323846f;
    std::vector<BYTE> loco(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> wagon(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> locoWheels(4 * VEH_WHEEL_STRUCT_STRIDE, 0);
    std::vector<BYTE> wagonWheels(2 * VEH_WHEEL_STRUCT_STRIDE, 0);
    std::vector<BYTE> dummyTarget(0x40, 0);

    auto initVeh = [](std::vector<BYTE>& v, unsigned id, float x, float steer, BYTE* link, BYTE* wheels, size_t nWheels) {
        *(unsigned*)(v.data() + 0x04) = id;
        *(int32_t*)(v.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        v[VEH_RENDER_VISIBLE_OFFSET] = 1;
        v[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        *(BYTE**)(v.data() + VEH_LINK_PTR_OFFSET) = link;
        *(float*)(v.data() + 0xD98 + 0x80) = x;
        *(float*)(v.data() + 0xD98 + 0x30) = x;
        *(float*)(v.data() + VEH_STEER_CURRENT_OFFSET) = steer;
        *(float*)(v.data() + VEH_STEER_TARGET_OFFSET) = steer;
        if (wheels && nWheels > 0) {
            *(BYTE**)(v.data() + VEH_WHEELS_BEGIN_OFFSET) = wheels;
            *(BYTE**)(v.data() + VEH_WHEELS_END_OFFSET)   = wheels + nWheels * VEH_WHEEL_STRUCT_STRIDE;
        }
    };

    // Tick 1: initial positions & wheel angles
    initVeh(loco, 801, 100.0f, 0.10f, wagon.data(), locoWheels.data(), 4);
    initVeh(wagon, 802, 80.0f, 0.0f, nullptr, wagonWheels.data(), 2);
    for (int w = 0; w < 4; ++w) *(float*)(locoWheels.data() + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) = 0.2f * (float)(w + 1);
    for (int w = 0; w < 2; ++w) *(float*)(wagonWheels.data() + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) = 0.5f;

    std::vector<BYTE*> vehList = { loco.data() };
    WriteMockVectorHeader(g_mockExeBuffer + 0xA317E0, vehList);
    HeldFrame_OnSimTick(1);

    // Tick 2: advance positions, steer, wheels
    *(float*)(loco.data() + 0xD98 + 0x80) = 104.0f;
    *(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET) = 0.30f;
    *(float*)(loco.data() + VEH_STEER_TARGET_OFFSET) = 0.60f;
    for (int w = 0; w < 4; ++w) *(float*)(locoWheels.data() + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) = 0.4f * (float)(w + 1);
    *(float*)(wagon.data() + 0xD98 + 0x80) = 84.0f;
    for (int w = 0; w < 2; ++w) *(float*)(wagonWheels.data() + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) = 1.5f;
    HeldFrame_OnSimTick(2);

    struct GuardCtx {
        BYTE* loco;
        BYTE* wagon;
        BYTE* locoWheels;
        BYTE* wagonWheels;
        BYTE* dummyTarget;
        float locoSteerMid;
        float locoWheel0Mid;
        float wagonWheel1Mid;
        BYTE locoFlagMid;
        BYTE wagonFlagMid;
    } ctx = { loco.data(), wagon.data(), locoWheels.data(), wagonWheels.data(), dummyTarget.data(), 0, 0, 0, 1, 1 };

    // 1. Coalesced Guard Begin/End: interpolates wheels/steer/nodes, suppresses flags, restores non-pointer bytes
    TEST_CHECK(HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E0, 0.5f, [](void* ud) {
        auto* c = (GuardCtx*)ud;
        c->locoFlagMid = c->loco[VEH_WORKER_SIM_FLAG_OFFSET];
        c->wagonFlagMid = c->wagon[VEH_WORKER_SIM_FLAG_OFFSET];
        c->locoSteerMid = *(const float*)(c->loco + VEH_STEER_CURRENT_OFFSET);
        c->locoWheel0Mid = *(const float*)(c->locoWheels + 0 * VEH_WHEEL_STRUCT_STRIDE);
        c->wagonWheel1Mid = *(const float*)(c->wagonWheels + 1 * VEH_WHEEL_STRUCT_STRIDE);
        // Simulate worker writing non-pointer render word outside nodes
        *(uint64_t*)(c->wagon + 0x400) = 0x123456789ABCDEF0ull;
    }, &ctx));
    TEST_CHECK(ctx.locoFlagMid == 0 && ctx.wagonFlagMid == 0);
    TEST_CHECK_NEAR(ctx.locoSteerMid, 0.20f, 1e-4f);
    TEST_CHECK_NEAR(ctx.locoWheel0Mid, 0.30f, 1e-4f);
    TEST_CHECK_NEAR(ctx.wagonWheel1Mid, 1.00f, 1e-4f);
    TEST_CHECK(loco[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
    TEST_CHECK(wagon[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
    TEST_CHECK_NEAR(*(const float*)(loco.data() + VEH_STEER_CURRENT_OFFSET), 0.30f, 1e-5f);
    TEST_CHECK_NEAR(*(const float*)(loco.data() + VEH_STEER_TARGET_OFFSET), 0.60f, 1e-5f);
    TEST_CHECK_NEAR(*(const float*)(locoWheels.data() + 0 * VEH_WHEEL_STRUCT_STRIDE), 0.40f, 1e-5f);
    TEST_CHECK_NEAR(*(const float*)(wagonWheels.data() + 1 * VEH_WHEEL_STRUCT_STRIDE), 1.50f, 1e-5f);
    TEST_CHECK(*(const uint64_t*)(wagon.data() + 0x400) == 0);
    (void)kPi;

    // 2. HeapPointer refusal inside RestoreVehicleCopySeh (must refuse & increment restoreRefusals)
    *(BYTE**)(wagon.data() + 0x408) = dummyTarget.data();
    HeldFrameDiagStats statsBefore = {};
    HeldFrame_GetDiagStats(&statsBefore);
    bool cleanAfterPtrChange = HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E0, 0.0f, [](void* ud) {
        auto* c = (GuardCtx*)ud;
        *(BYTE**)(c->wagon + 0x408) = c->dummyTarget + 16;
    }, &ctx);
    HeldFrameDiagStats statsAfter = {};
    HeldFrame_GetDiagStats(&statsAfter);
    TEST_CHECK(!cleanAfterPtrChange);
    TEST_CHECK(statsAfter.restoreRefusals == statsBefore.restoreRefusals + 1);
    TEST_CHECK(*(BYTE**)(wagon.data() + 0x408) == dummyTarget.data() + 16);

    // 3. Batch worker-flag & visual restore fault isolation across unmapped intermediate vehicle
    BYTE* badVehPage = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    TEST_CHECK(badVehPage != nullptr);
    if (badVehPage) {
        *(unsigned*)(badVehPage + 0x04) = 803;
        *(int32_t*)(badVehPage + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        badVehPage[VEH_RENDER_VISIBLE_OFFSET] = 1;
        badVehPage[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        std::vector<BYTE*> badList = { loco.data(), badVehPage, wagon.data() };
        WriteMockVectorHeader(g_mockExeBuffer + 0xA317E0, badList);
        HeldFrame_OnSimTick(3);
        struct UnmapVehCtx { BYTE* page; DWORD oldProt; } uctx = { badVehPage, 0 };
        HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E0, 0.5f, [](void* ud) {
            auto* u = (UnmapVehCtx*)ud;
            VirtualProtect(u->page, 0x2000, PAGE_NOACCESS, &u->oldProt);
        }, &uctx);
        VirtualProtect(badVehPage, 0x2000, uctx.oldProt, &uctx.oldProt);
        TEST_CHECK(loco[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
        TEST_CHECK(wagon[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
        VirtualFree(badVehPage, 0, MEM_RELEASE);
    }

    // 4. HeldFrame_AfterEarly merged building scan safety + PrepareDoorsAndView empty-buildings regression + HeldFrame_Reset clears view caches
    static int s_mockDrawAgentsCalls = 0;
    s_mockDrawAgentsCalls = 0;
    auto mockNodePos = +[](void*, float* out) -> void* {
        if (out) { out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f; }
        return out;
    };
    auto mockCamZDist = +[](void*, float*) -> float { return 10.0f; };
    auto mockDrawAgents = +[](void*, void*) { ++s_mockDrawAgentsCalls; };

    *(BYTE**)(g_mockExeBuffer + 0x9DDC88) = g_mockExeBuffer + 0x100; // ENGINE_RVA non-null
    *(void**)(g_mockExeBuffer + 0x8B1858) = (void*)mockNodePos;      // NODE_POSITION_IMPORT
    *(void**)(g_mockExeBuffer + 0x8B2058) = (void*)mockCamZDist;     // CAMERA_ZDISTANCE_IMPORT
    *(float*)(g_mockExeBuffer + 0x9524B4) = 100.0f;                  // AGENT_NEAR_RVA
    WriteAbsJmp12(g_mockExeBuffer + 0x87E0E0, (void*)mockDrawAgents); // DRAW_AGENTS_RVA

    std::vector<BYTE> viewRender(0x180, 0);
    viewRender[0x178] = 1;
    std::vector<BYTE> viewBld(0x1170, 0);
    *(BYTE**)(viewBld.data() + 0x5D0) = viewRender.data();
    std::vector<BYTE> viewAgent(0x5A0, 0);
    *(BYTE**)(viewAgent.data() + 0x598) = viewBld.data();

    // Frame 1: buildings=1, agents=1 -> DRAW_AGENTS_RVA called once, view caches populated
    std::vector<BYTE*> viewBlds = { viewBld.data() };
    std::vector<BYTE*> viewAgents = { viewAgent.data() };
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD0, viewBlds);
    WriteMockVectorHeader(g_mockExeBuffer + 0xA1EF20 + 0x12758, viewAgents);
    HeldFrame_AfterEarly();
    size_t vSz = 0, nSz = 0, kSz = 0;
    HeldFrame_GetViewCacheSizesForTest(&vSz, &nSz, &kSz);
    TEST_CHECK(s_mockDrawAgentsCalls == 1);
    TEST_CHECK(vSz == 1 && nSz == 1 && kSz == 1);

    // Frame 2: buildings=0, agents=1 -> g_viewBuildings MUST be cleared and DRAW_AGENTS_RVA MUST NOT be called
    viewBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD0, viewBlds);
    viewBld[0x115A] = 0x55;
    HeldFrame_AfterEarly();
    HeldFrame_GetViewCacheSizesForTest(&vSz, nullptr, nullptr);
    TEST_CHECK(s_mockDrawAgentsCalls == 1);
    TEST_CHECK(vSz == 0);
    TEST_CHECK(viewBld[0x115A] == 0x55);

    // Frame 3 + HeldFrame_Reset(): populates view caches again, then HeldFrame_Reset clears all three
    viewBlds = { viewBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD0, viewBlds);
    HeldFrame_AfterEarly();
    HeldFrame_GetViewCacheSizesForTest(&vSz, &nSz, &kSz);
    TEST_CHECK(s_mockDrawAgentsCalls == 2);
    TEST_CHECK(vSz == 1 && nSz == 1 && kSz == 1);

    HeldFrame_Reset();
    HeldFrame_GetViewCacheSizesForTest(&vSz, &nSz, &kSz);
    TEST_CHECK(vSz == 0 && nSz == 0 && kSz == 0);

    viewBlds.clear();
    viewAgents.clear();
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD0, viewBlds);
    WriteMockVectorHeader(g_mockExeBuffer + 0xA1EF20 + 0x12758, viewAgents);

    *(BYTE***)(g_mockExeBuffer + 0xA317E0) = nullptr;
    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = nullptr;
    ResetMockWorldMemory();
    return true;
}
