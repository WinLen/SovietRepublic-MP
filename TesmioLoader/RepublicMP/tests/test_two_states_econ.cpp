// SPDX-License-Identifier: GPL-3.0-only
// Two-States Economy, Territory, Loans, Stats, Singleplayer Isolation, Camera, Civic Stress & Research Desync Suites.

#include "test_harness.h"
#include <crtdbg.h>
#include <cmath>
#include <initializer_list>

// ---------------------------------------------------------------------------
// Test 4: TwoStates Territory Zones, Ownership, Economy Attribution & Invariant #10
// ---------------------------------------------------------------------------

static bool RunLazyHooksAndLoadAbortSubTest()
{
    static std::vector<void*> s_hookTargets;
    static int s_installCalls = 0;
    s_hookTargets.clear();
    s_installCalls = 0;

    struct HookCaptureGuard {
        int (*orig)(void*, void*, void**, const unsigned char*, size_t, const char*);
        int savedMode;
        float savedHalfWidth;
        bool savedExpandBorders;
        bool savedFullCap;
        bool savedShow3D;
        bool savedShowMinimap;
        bool savedSendLogs;
        bool savedUploadCrashes;
        char savedJoinAddr[128];
        explicit HookCaptureGuard(int (*cap)(void*, void*, void**, const unsigned char*, size_t, const char*))
            : orig(g_mockHost.installInlineHook)
            , savedMode(TwoStates_GetMode())
            , savedHalfWidth(TwoStates_GetBorderHalfWidth())
            , savedExpandBorders(TwoStates_GetExpandBorders())
            , savedFullCap(TwoStates_GetFullStartingCapital())
            , savedShow3D(TwoStates_GetShow3DBorder())
            , savedShowMinimap(TwoStates_GetShowMinimapBorders())
            , savedSendLogs(TwoStates_GetSendClientLogs())
            , savedUploadCrashes(TwoStates_GetUploadCrashReports())
        {
            const char* addr = TwoStates_GetSavedJoinAddress();
            snprintf(savedJoinAddr, sizeof(savedJoinAddr), "%s", addr ? addr : "");
            g_mockHost.installInlineHook = cap;
            TwoStates_Bind(&g_mockHost);
        }
        ~HookCaptureGuard()
        {
            g_mockHost.installInlineHook = orig;
            TwoStates_Bind(&g_mockHost);
            TwoStates_ResetLazyHooksStateForTest();
            TwoStates_OnSessionEnded();
            TwoStates_MountSaveBank(false);
            TwoStates_OnAnyWorldLoaded(false);
            TwoStates_SetMode(savedMode);
            TwoStates_SetBorderHalfWidth(savedHalfWidth);
            TwoStates_SetExpandBorders(savedExpandBorders);
            TwoStates_SetFullStartingCapital(savedFullCap);
            TwoStates_SetShow3DBorder(savedShow3D);
            TwoStates_SetShowMinimapBorders(savedShowMinimap);
            TwoStates_SetSendClientLogs(savedSendLogs);
            TwoStates_SetUploadCrashReports(savedUploadCrashes);
            TwoStates_SetSavedJoinAddress(savedJoinAddr);
        }
    } captureGuard([](void* target, void*, void**, const unsigned char*, size_t, const char*) -> int {
        ++s_installCalls;
        s_hookTargets.push_back(target);
        return 1;
    });

    auto hasTarget = [&](uintptr_t rva) {
        void* ptr = (void*)(g_mockExeBuffer + rva);
        return std::find(s_hookTargets.begin(), s_hookTargets.end(), ptr) != s_hookTargets.end();
    };

    // Unloaded real-game guard predicate & mid-load WM_DESTROY fast-exit predicate
    TEST_CHECK(TwoStates_ShouldBlockUnloadedWorldAccessForTest(true, false, false));
    TEST_CHECK(TwoStates_ShouldBlockUnloadedWorldAccessForTest(false, true, false));
    TEST_CHECK(TwoStates_ShouldBlockUnloadedWorldAccessForTest(true, true, false));
    TEST_CHECK(!TwoStates_ShouldBlockUnloadedWorldAccessForTest(true, true, true));
    TEST_CHECK(!TwoStates_ShouldBlockUnloadedWorldAccessForTest(false, false, false));
    TEST_CHECK(!ImGuiHook_ShouldFastExitOnDestroyDuringLoad(false, false));
    TEST_CHECK(ImGuiHook_ShouldFastExitOnDestroyDuringLoad(true, false));
    TEST_CHECK(ImGuiHook_ShouldFastExitOnDestroyDuringLoad(false, true));
    TEST_CHECK(ImGuiHook_ShouldFastExitOnDestroyDuringLoad(true, true));

    TwoStates_ResetLazyHooksStateForTest();
    ResetMockWorldMemory();
    TwoStates_MountSaveBank(false);
    TwoStates_Install();
    TEST_CHECK(s_installCalls == 1);
    TEST_CHECK(!hasTarget(RVA_GET_MAX_AVAILABLE_BORROW));
    TEST_CHECK(!hasTarget(RVA_CITIZEN_CAN_MOVE_TO_BLD));
    TEST_CHECK(!TwoStates_AreSessionHooksInstalled());
    TEST_CHECK(!TwoStates_HasPendingSessionHooksInstall());
    TEST_CHECK(!TwoStates_IsWorldLoadPrepared());
    TEST_CHECK(!SaveSync_IsWorldLoaded());

    // Browsing Co-op save bank and backing out must NOT arm or leak lazy session hooks into SP
    TwoStates_MountSaveBank(true);
    TEST_CHECK(!TwoStates_HasPendingSessionHooksInstall());
    TEST_CHECK(s_installCalls == 1);
    TwoStates_MountSaveBank(false);
    TEST_CHECK(!TwoStates_HasPendingSessionHooksInstall());

    // SaveSync_StartHost when !SaveSync_IsWorldLoaded() must NOT arm lazy hooks
    SaveSync_StartHost();
    TEST_CHECK(!TwoStates_HasPendingSessionHooksInstall());
    TEST_CHECK(!TwoStates_AreSessionHooksInstalled());
    TEST_CHECK(s_installCalls == 1);

    // Aborting/failing a co-op join cancels a pending request before any hooks patch
    TwoStates_RequestSessionHooksInstall();
    TEST_CHECK(TwoStates_HasPendingSessionHooksInstall());
    TwoStates_OnSessionEnded();
    TEST_CHECK(!TwoStates_HasPendingSessionHooksInstall());
    TEST_CHECK(!TwoStates_AreSessionHooksInstalled());
    TEST_CHECK(s_installCalls == 1);

    // Singleplayer load: zero session hooks installed, g_beforeLoadPrepared transitions cleanly
    TwoStates_OnAnyWorldLoaded(false);
    TEST_CHECK(!TwoStates_IsWorldLoadPrepared());
    TwoStates_BeforeAnyWorldLoad();
    TEST_CHECK(TwoStates_IsWorldLoadPrepared());
    TEST_CHECK(s_installCalls == 1);
    TEST_CHECK(!TwoStates_AreSessionHooksInstalled());
    TEST_CHECK(!TwoStates_HasPendingSessionHooksInstall());
    TwoStates_OnAnyWorldLoaded(false);
    TEST_CHECK(!TwoStates_IsWorldLoadPrepared());

    // InstallPendingSessionHooksOnMainThread with g_exeBase == nullptr must NOT latch installed=true
    {
        struct NullExeBindGuard {
            TsmHost nullHost;
            NullExeBindGuard() : nullHost(g_mockHost)
            {
                nullHost.exeBase = nullptr;
                TwoStates_Bind(&nullHost);
            }
            ~NullExeBindGuard() { TwoStates_Bind(&g_mockHost); }
        } nullBindGuard;

        TwoStates_RequestSessionHooksInstall();
        TEST_CHECK(TwoStates_HasPendingSessionHooksInstall());
        TEST_CHECK(TwoStates_InstallPendingSessionHooksOnMainThread() == 0);
        TEST_CHECK(!TwoStates_AreSessionHooksInstalled());
        TEST_CHECK(TwoStates_HasPendingSessionHooksInstall());
        TEST_CHECK(s_installCalls == 1);
    }

    // Main-Thread quiescent install patches all heavy session hooks once and is idempotent
    int installedNow = TwoStates_InstallPendingSessionHooksOnMainThread();
    TEST_CHECK(installedNow >= 64);
    TEST_CHECK(s_installCalls >= 65);
    TEST_CHECK(TwoStates_AreSessionHooksInstalled());
    TEST_CHECK(!TwoStates_HasPendingSessionHooksInstall());

    TEST_CHECK(hasTarget(RVA_GET_MAX_AVAILABLE_BORROW));
    TEST_CHECK(hasTarget(RVA_CITIZEN_CAN_MOVE_TO_BLD));
    TEST_CHECK(hasTarget(RVA_MINIMAP_DRAW_CONTENT));
    TEST_CHECK(hasTarget(RVA_PROD_LINE_STEP));
    TEST_CHECK(hasTarget(RVA_TOP_BAR_HUD_DRAW));

    const int countAfterFirst = s_installCalls;
    TEST_CHECK(TwoStates_InstallPendingSessionHooksOnMainThread() == installedNow);
    TEST_CHECK(s_installCalls == countAfterFirst);
    return true;
}

static __int64 MockPlanStartFn(__int64 network, unsigned char /*dollars*/, unsigned char /*rubles*/, __int64 /*count*/, __int64 /*group*/)
{
    __int64* begin = *(__int64**)(network + 0x2B0);
    __int64* end   = *(__int64**)(network + 0x2B8);
    __int64 started = 0;
    for (__int64* it = begin; it && it < end; ++it) {
        if (*it && *(unsigned char*)(*it + 0x140) == 1) {
            *(unsigned char*)(*it + 0x140) = 0;
            started++;
        }
    }
    return started;
}

static __int64 MockCancelSegFn(__int64 /*network*/, __int64 seg, unsigned char /*p3*/, int /*p4*/, unsigned char /*p5*/)
{
    if (seg && *(unsigned char*)(seg + 0x140) == 1) {
        *(unsigned char*)(seg + 0x140) = 0;
        *(unsigned char*)(seg + 0x141) = 0xFF;
    }
    return 1;
}

bool Test_TwoStates_ZonesAndEconomy()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
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
    std::vector<BYTE> newBld(BUILDING_STRUCT_BYTES, 0);
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

    std::vector<BYTE> b1(BUILDING_STRUCT_BYTES, 0), b2(BUILDING_STRUCT_BYTES, 0), b3(BUILDING_STRUCT_BYTES, 0);
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
    *(int*)(world + WORLD_UNLIMITED_MONEY_OFFSET) = 0;

    // Planned road segment ownership & per-republic separation for "Build planned" / "Cancel all planned"
    WriteAbsJmp12(g_mockExeBuffer + 0x54BF10, (void*)&MockPlanStartFn);
    WriteAbsJmp12(g_mockExeBuffer + 0x569980, (void*)&MockCancelSegFn);
    g_mockExeBuffer[0x521440] = 0xC3;

    auto makeNode = [](float x, float y, float z) {
        std::vector<BYTE> n(0x40, 0);
        *(float*)(n.data() + 4)  = x;
        *(float*)(n.data() + 8)  = y;
        *(float*)(n.data() + 12) = z;
        return n;
    };
    auto makePlannedSeg = [](BYTE* n0, BYTE* n1) {
        std::vector<BYTE> s(0x200, 0);
        *(__int64*)(s.data() + 0x20) = (__int64)n0;
        *(__int64*)(s.data() + 0x28) = (__int64)n1;
        *(unsigned char*)(s.data() + 0x140) = 1; // planned
        *(unsigned char*)(s.data() + 0x141) = 0; // not built
        return s;
    };

    auto nHost0 = makeNode(-600.0f, 10.0f, 0.0f);
    auto nHost1 = makeNode(-500.0f, 10.0f, 0.0f);
    auto nCli0  = makeNode( 500.0f, 10.0f, 0.0f);
    auto nCli1  = makeNode( 600.0f, 10.0f, 0.0f);
    auto nBrd0  = makeNode(  40.0f, 10.0f, 0.0f);
    auto nBrd1  = makeNode(  60.0f, 10.0f, 0.0f);

    auto segHost   = makePlannedSeg(nHost0.data(), nHost1.data());
    auto segClient = makePlannedSeg(nCli0.data(),  nCli1.data());
    auto segBorder = makePlannedSeg(nBrd0.data(),  nBrd1.data());
    RoadSync_SetPlannedSegmentOwnerForTest((__int64)segBorder.data(), 2);

    TEST_CHECK(RoadSync_GetPlannedSegmentOwner((__int64)segHost.data()) == 1);
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner((__int64)segClient.data()) == 2);
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner((__int64)segBorder.data()) == 2);

    std::vector<BYTE> roadMgr(0x800, 0);
    std::vector<BYTE*> roadSegs = { segHost.data(), segClient.data(), segBorder.data() };
    WriteMockVectorHeader(roadMgr.data() + 0x2B0, roadSegs);
    *(__int64*)(g_mockExeBuffer + UNIVERSAL_THIS_RVA + 0xF840) = (__int64)roadMgr.data();

    // Host (State 1) presses "Build planned": only segHost starts; segClient and segBorder stay planned
    RoadSyncPlanStartMsg msgStart = {};
    msgStart.category = ROAD_SYNC_CATEGORY_ROAD;
    msgStart.exceptGroup = 0;
    msgStart.rubles = 1;
    msgStart.count = -1;
    msgStart.group = -1;
    TwoStates_BeginCommand(1);
    TEST_CHECK(RoadSync_ApplyCommandPlanStart(msgStart));
    TwoStates_EndCommand(1);

    TEST_CHECK(*(unsigned char*)(segHost.data() + 0x140) == 0);
    TEST_CHECK(*(unsigned char*)(segClient.data() + 0x140) == 1);
    TEST_CHECK(*(unsigned char*)(segClient.data() + 0x141) == 0);
    TEST_CHECK(*(unsigned char*)(segBorder.data() + 0x140) == 1);
    TEST_CHECK(*(unsigned char*)(segBorder.data() + 0x141) == 0);

    // Client (State 2) presses "Cancel all planned": only segClient and segBorder are cancelled; segHost stays untouched
    *(unsigned char*)(segHost.data() + 0x140) = 1;
    RoadSyncPlanStartMsg msgCancel = {};
    msgCancel.category = ROAD_SYNC_CATEGORY_ROAD;
    msgCancel.exceptGroup = 2;
    TwoStates_BeginCommand(2);
    TEST_CHECK(RoadSync_ApplyCommandPlanStart(msgCancel));
    TwoStates_EndCommand(2);

    TEST_CHECK(*(unsigned char*)(segHost.data() + 0x140) == 1);
    TEST_CHECK(*(unsigned char*)(segHost.data() + 0x141) == 0);
    TEST_CHECK(*(unsigned char*)(segClient.data() + 0x140) == 0);
    TEST_CHECK(*(unsigned char*)(segClient.data() + 0x141) == 0xFF);
    TEST_CHECK(*(unsigned char*)(segBorder.data() + 0x140) == 0);
    TEST_CHECK(*(unsigned char*)(segBorder.data() + 0x141) == 0xFF);

    RoadSync_SetPlannedSegmentOwnerForTest((__int64)segBorder.data(), 0);
    TwoStates_OnSessionEnded();

    TEST_CHECK(RunLazyHooksAndLoadAbortSubTest());
    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Invariant #4 (script.ini Safety) & Invariant #5 (LC_NUMERIC Cents Meta)
// ---------------------------------------------------------------------------

bool Test_TwoStates_SaveMetaAndScriptIniSafety()
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
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1; // WORLD_ROAD_NETWORK_RVA != 0 (world loaded)
    TwoStates_OnSessionEnded();
    double prepRub[2] = {}, prepUsd[2] = {};
    TwoStates_PrepareHostSave(false, prepRub, prepUsd);
    TEST_CHECK_NEAR(prepRub[0], 1234567.89, 1e-2);
    TEST_CHECK_NEAR(prepRub[1], 987654.32, 1e-2);
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 0;

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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    double initRub[2] = { 1000000.0, 1000000.0 };
    double initUsd[2] = { 500000.0,  500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    // Work resource multipliers at world + 0xC360 -> +0x58 (USD = 1.5), +0x5C (RUB = 2.0)
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
    TwoStates_OnSessionEnded();
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1; // world loaded

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
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 0;
    return true;
}

static bool TestLoans_DailyStepAndBankruptcyIsolation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    double initRub[2] = { 500000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    // Asymmetric building count: 50 buildings in West (State 1) vs 1 building in East (State 2)
    std::vector<std::vector<BYTE>> westBlds(50, std::vector<BYTE>(BUILDING_STRUCT_BYTES, 0));
    std::vector<BYTE> eastBld(BUILDING_STRUCT_BYTES, 0);
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

bool Test_TwoStates_Loans()
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
    TwoStates_OnSessionEnded();
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
    TwoStates_OnSessionEnded();
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    std::vector<BYTE> resTable(4 * WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x00) = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x08) = resTable.data() + resTable.size();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x10) = resTable.data() + resTable.size();
    uintptr_t resFood = (uintptr_t)(resTable.data() + 2 * WORLD_RESOURCE_STRIDE);

    // Special heap-allocated resource pointer at world + WORLD_ELENERGY_RES_PTR_OFF (0xC450)
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
        *(SovietStatResEntry**)(world + 0xC7F8 + 0x00) = uiTmpPool;
        *(SovietStatResEntry**)(world + 0xC7F8 + 0x08) = uiTmpPool;
        *(SovietStatResEntry**)(world + 0xC7F8 + 0x10) = uiTmpPool + 4;
        TwoStates_LedgerAddForTest(world, &hb[0], world + 0xC7F8, 1.0f);

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
    TwoStates_OnSessionEnded();
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

    // Foreign Power Connection (0x1B9A40, category 0x1F) electricity import/export step runs outside
    // RVA_BLD_SIM_LOOP_HEAD..TAIL (at 0x13A4E4 and 0x13E5EA). Verify that State 2 (Client) importing
    // electricity in RUB deducts 100% of cost from State 2 and records 100% of R0 ledger in State 2
    // without charging State 1 (Host). Also verify unmapped 64-bit integer pairs (0x0000000A00000000)
    // do not falsely classify as pointers or raise access violations.
    TEST_CHECK(!IsPointerLikeX64(0x0000000A00000000ULL));
    uintptr_t resElEnergy = (uintptr_t)(resTable.data() + 3 * WORLD_RESOURCE_STRIDE);
    double rub1BeforePower = TwoStates_GetRubles(1);
    double rub2BeforePower = TwoStates_GetRubles(2);
    TwoStates_BeforeStep(world);
    TwoStates_ForeignPowerTradeForTest(world, bldEast.data(), resElEnergy, 61.1f, 87.47f, false /*RUB*/, false /*Import*/);
    TwoStates_AfterStep(world);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), rub1BeforePower, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), rub2BeforePower - 87.47, 1e-2);
    TEST_CHECK(!TwoStates_GetStateEconResEntry(1, 0, resElEnergy, &tons, &val));
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 0, resElEnergy, &tons, &val));
    TEST_CHECK_NEAR(tons, 61.1f, 1e-2f);
    TEST_CHECK_NEAR(val, 87.47f, 1e-2f);

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
    TEST_CHECK(*(int32_t*)(hudBlock + 0x2C) == 0);                             // Top HUD basic-educated workers = 0
    TEST_CHECK(*(int32_t*)(hudBlock + 0x30) == 3);                             // Top HUD university-educated workers = 3
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
    TEST_CHECK(*(int32_t*)(hudBlock + 0x2C) == 7);                             // Top HUD basic-educated workers = 7
    TEST_CHECK(*(int32_t*)(hudBlock + 0x30) == 0);                             // Top HUD university-educated workers = 0
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0x0C), 0.50f, 1e-3f);                 // Top HUD Health = 50%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xA8) / *(float*)(hudBlock + 0x00), 0.35f, 1e-3f); // Top HUD Loyalty = 35%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xAC) / *(float*)(hudBlock + 0x00), 0.42f, 1e-3f); // Top HUD Happiness = 42%
    TEST_CHECK_NEAR(*(float*)(hudBlock + 0xB8) / *(float*)(hudBlock + 0x00), 64.0f, 1e-2f);  // Top HUD Lifespan = 64y
    TEST_CHECK(*(int32_t*)(popWinStat + STAT_CITIZEN_COUNTS_OFF + 0) == 0);
    TEST_CHECK(*(int32_t*)(popWinStat + STAT_CITIZEN_ADULTS_OFF) == 5);
    TEST_CHECK(SafeCountStdVector(world + WORLD_CITIZENS_OFFSET, sizeof(void*)) == 7);
    *(float*)(world + WORLD_HUD_POP_TIMER_OFFSET) = 5.0f;
    TwoStates_UiEnd();
    TEST_CHECK(SafeCountStdVector(world + WORLD_CITIZENS_OFFSET, sizeof(void*)) == 10);

    // Graduate an East SSR citizen (citPtrs[3]) from basic (1.25f) to university (2.15f) with unchanged localPop = 7:
    // Top HUD educated slots (+0x2C/+0x30) and refresh timers must update immediately on TwoStates_UiBegin()!
    *(float*)(citPtrs[3] + 0xA8) = 2.15f;
    *(float*)(world + WORLD_HUD_POP_TIMER_OFFSET) = 5.0f;
    *(float*)(world + WORLD_POP_STAT_TIMER_OFF)   = 5.0f;
    TwoStates_UiBegin();
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_BRACKETS_OFF + 2 * 4) == 6);
    TEST_CHECK(*(int32_t*)(curStat + STAT_CITIZEN_BRACKETS_OFF + 3 * 4) == 1);
    TEST_CHECK(*(int32_t*)(hudBlock + 0x2C) == 6);
    TEST_CHECK(*(int32_t*)(hudBlock + 0x30) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_HUD_POP_TIMER_OFFSET), 0.0f, 1e-6f);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_POP_STAT_TIMER_OFF),   0.0f, 1e-6f);
    TwoStates_UiEnd();
    *(float*)(citPtrs[3] + 0xA8) = 1.25f;

    // Verify university hostel graduation / citizen relocation isolation (0x834DE0, 0x83C590, 0x83CA50):
    // A West SSR student/graduate (citPtrs[0], living in bldWest) must never relocate into East SSR (bldEast),
    // and H_CitizenStarveStep must filter WORLD_BUILDINGS_OFFSET to same-state buildings.
    std::vector<BYTE> bldWestApt(0x1600, 0);
    *(float*)(bldWestApt.data() + OBJ_POS_OFFSET)     = -900.0f;
    *(float*)(bldWestApt.data() + OBJ_POS_OFFSET + 8) =  200.0f;
    TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(-900.0f, 200.0f), 1);

    TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldEast.data(), citPtrs[0]));
    TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));

    // Verify native 0x87DCC0 (RVA_CITIZEN_CAN_MOVE_TO_BLD) filter parity (no 0x87DCC9 hook jump-back crash):
    // Master block (+0x1414), education filters (+0x1415..+0x1417), and loyalty buckets (+0x1418..+0x141B).
    {
        float savedEdu = *(float*)(citPtrs[0] + 0xA8);
        float savedLoy = *(float*)(citPtrs[0] + 0xE4);

        bldWestApt[0x1414] = 1;
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x1414] = 0;

        *(float*)(citPtrs[0] + 0xA8) = 0.75f; // edu == 0
        bldWestApt[0x1415] = 1;
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xA8) = 1.50f; // edu == 1
        TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x1415] = 0;

        bldWestApt[0x1416] = 1;
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xA8) = 2.10f; // edu == 2
        TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x1416] = 0;

        bldWestApt[0x1417] = 1;
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xA8) = 1.25f;
        TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x1417] = 0;

        bldWestApt[0x1418] = 1; // bucket 0: loy < 0.25f
        *(float*)(citPtrs[0] + 0xE4) = 0.24f;
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xE4) = 0.25f; // boundary -> bucket 1
        TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x1418] = 0;

        bldWestApt[0x1419] = 1; // bucket 1: 0.25f <= loy < 0.50f
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xE4) = 0.50f; // boundary -> bucket 2
        TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x1419] = 0;

        bldWestApt[0x141A] = 1; // bucket 2: 0.50f <= loy < 0.75f
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xE4) = 0.75f; // boundary -> bucket 3
        TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x141A] = 0;

        bldWestApt[0x141B] = 1; // bucket 3: loy >= 0.75f or NaN
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xE4) = std::numeric_limits<float>::quiet_NaN();
        TEST_CHECK(!TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        *(float*)(citPtrs[0] + 0xE4) = 0.50f;
        TEST_CHECK(TwoStates_CitizenCanMoveToBldForTest(world, bldWestApt.data(), citPtrs[0]));
        bldWestApt[0x141B] = 0;

        *(float*)(citPtrs[0] + 0xA8) = savedEdu;
        *(float*)(citPtrs[0] + 0xE4) = savedLoy;
    }

    TwoStates_CitizenMoveToBldForTest(world, citPtrs[0], bldEast.data());
    TEST_CHECK(*(BYTE**)(citPtrs[0] + CIT_HOME_BUILDING_OFFSET) == bldWest.data());

    BYTE* worldBlds3[3] = { bldWest.data(), bldWestApt.data(), bldEast.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x00) = worldBlds3;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x08) = worldBlds3 + 3;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x10) = worldBlds3 + 3;
    TwoStates_CitizenStarveStepForTest(world, citPtrs[0]);
    TEST_CHECK(*(BYTE**)(citPtrs[0] + CIT_HOME_BUILDING_OFFSET) == bldWestApt.data());
    TEST_CHECK(SafeCountStdVector(world + WORLD_BUILDINGS_OFFSET, sizeof(void*)) == 3);
    *(BYTE**)(citPtrs[0] + CIT_HOME_BUILDING_OFFSET) = bldWest.data();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x00) = bldPtrs;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x08) = bldPtrs + 2;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x10) = bldPtrs + 2;

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
    TwoStates_OnSessionEnded();
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

static bool TestEconStats_VehicleSaleAbiAndRenameAndCarDealerSwitches()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    double initRub[2] = { 10000000.0, 10000000.0 };
    double initUsd[2] = {  2000000.0,  2000000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);
    *(double*)(world + WORLD_RUB_OFFSET) = 10000000.0;
    *(double*)(world + WORLD_USD_OFFSET) =  2000000.0;

    // 1. 7-parameter H_VehicleSale ABI & stack-garbage (-1.07448163e15f) sanitization
    std::vector<BYTE> veh(0x2000, 0);
    *(uint32_t*)(veh.data() + 4) = 777;
    *(float*)(veh.data() + OBJ_POS_OFFSET) = -400.0f;
    *(float*)(veh.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    TwoStates_RegisterVehicleOwner(777, 1);

    // Uninitialized stack garbage (-1.07448163e15f) in 7th arg must be sanitized to 0.0f
    TwoStates_BeforeStep(world);
    TwoStates_VehicleSaleForTest(world, veh.data(), -1.07448163e15f, 50000.0f);
    TwoStates_AfterStep(world);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 10050000.0, 1.0);
    float vehScalars[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, vehScalars);
    TEST_CHECK_NEAR(vehScalars[3], 50000.0f, 1.0f);

    // 2. Load-time healing of a save corrupted by the old 6-arg H_VehicleSale bug (-1.07448162e15 RUB)
    std::filesystem::path corrDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_corrupted_veh_sale";
    std::filesystem::create_directories(corrDir);
    std::string corrMeta =
        "[republicmp_coop]\r\n"
        "version=1\r\n"
        "coop_mode=1\r\n"
        "border_half_width_m=250\r\n"
        "expand_borders=0\r\n"
        "state1_rub_cents=-107448162050870752\r\n"
        "state2_rub_cents=1242009265\r\n"
        "state1_usd_cents=200000000\r\n"
        "state2_usd_cents=200000000\r\n"
        "net_trade1_rub_cents=-107448163914098352\r\n"
        "net_trade2_rub_cents=-2757990735\r\n"
        "net_trade1_usd_cents=0\r\n"
        "net_trade2_usd_cents=0\r\n"
        "econ_cur1=D:29,1975;V:16282194,0,0,-107448163211673600\r\n"
        "econ_cur2=D:29,1975;V:0,0,0,0\r\n"
        "econ_hist_count=0\r\n";
    {
        FILE* f = nullptr;
        std::string metaPath = (corrDir / "republicmp_coop.ini").string();
        if (fopen_s(&f, metaPath.c_str(), "wb") == 0 && f) {
            fwrite(corrMeta.data(), 1, corrMeta.size(), f);
            fclose(f);
        }
    }
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(corrDir.string().c_str(), true));
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 11608028.48, 1.0);
    TwoStates_GetStateEconFloat4ForTest(1, false, vehScalars);
    TEST_CHECK_NEAR(vehScalars[3], 0.0f, 1e-3f);

    // 3. BuildingSwitches_ApplyCommand on Car Dealer (+0xCD8..+0xCE4 = (0, 1) -> 0x0000000100000000ULL)
    //    and odd-length Cyrillic building name (+0x114 -> 0x0000041204110410ULL)
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(uint32_t*)(bld.data() + 0) = 501;
    *(float*)(bld.data() + 0x350) = -300.0f;
    *(float*)(bld.data() + 0x358) = 50.0f;
    *(uint32_t*)(bld.data() + BLD_CITIZEN_FILTER_AT + 0) = 0;
    *(uint32_t*)(bld.data() + BLD_CITIZEN_FILTER_AT + 4) = 1; // 8-byte pair is 0x0000000100000000ULL (4 GB!)
    *(uint32_t*)(bld.data() + 0x114) = 0x04110410u;           // L"АБ"
    *(uint32_t*)(bld.data() + 0x118) = 0x00000412u;           // L"В\0" -> 8-byte pair is 0x0000041204110410ULL (4.47 TB!)

    BYTE* bldPtrs[1] = { bld.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x00) = bldPtrs;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x08) = bldPtrs + 1;

    #pragma pack(push, 1)
    struct LocalSwitchWord { unsigned offset, old, now; };
    struct LocalSwitchMsg {
        RouteRef building;
        unsigned kind, shape, count;
        LocalSwitchWord words[60];
    };
    #pragma pack(pop)
    LocalSwitchMsg swMsg{};
    TEST_CHECK(Ref_Building((U64)bld.data(), swMsg.building));
    swMsg.kind = 0; // KIND_BUILDING
    swMsg.count = 2;
    swMsg.words[0] = { BLD_CITIZEN_FILTER_AT, 0u, 2u };
    swMsg.words[1] = { 0x114u, 0x04110410u, 0x04140413u };
    TEST_CHECK(BuildingSwitches_ApplyCommand(&swMsg, sizeof(swMsg)));
    TEST_CHECK(*(uint32_t*)(bld.data() + BLD_CITIZEN_FILTER_AT) == 2u);
    TEST_CHECK(*(uint32_t*)(bld.data() + 0x114) == 0x04140413u);

    // 4. On-the-fly Rename_ApplyCommand for City (with shifted index + different terrain Y),
    //    Building, Vehicle (including >32-char Cyrillic name), and Line
    std::vector<BYTE> cityObj(0x100, 0);
    *(float*)(cityObj.data() + 4)  = -500.0f;
    *(float*)(cityObj.data() + 8)  = 42.5f;   // Local terrain Y = 42.5f
    *(float*)(cityObj.data() + 12) = 200.0f;
    BYTE* cityPtrs[1] = { cityObj.data() };
    *(BYTE***)(world + 0x103E8 + 0x00) = cityPtrs;
    *(BYTE***)(world + 0x103E8 + 0x08) = cityPtrs + 1;

    #pragma pack(push, 1)
    struct LocalRenameMsg {
        unsigned magic, kind, index, check, chars;
        unsigned short text[0x40];
    };
    struct LocalCityPosExt {
        unsigned extMagic;
        float pos[3];
    };
    #pragma pack(pop)

    // City rename where sender had index=5 and Y=19.0f (so both index and CityCheck differ, resolved via CityPosExt XZ)
    LocalRenameMsg renCity{};
    renCity.magic = 0x314D414Eu;
    renCity.kind = 3; // KIND_CITY
    renCity.index = 5;
    renCity.check = 0x12345679u;
    const wchar_t* kNewCityName = L"Ленинград";
    renCity.chars = (unsigned)wcslen(kNewCityName);
    memcpy(renCity.text, kNewCityName, renCity.chars * sizeof(wchar_t));
    LocalCityPosExt cExt{ 0x43504F53u, { -500.0f, 19.0f, 200.0f } };
    memcpy(&renCity.text[0x20], &cExt, sizeof(cExt));
    TEST_CHECK(Rename_ApplyCommand(&renCity, sizeof(renCity)));
    TEST_CHECK(wcscmp((const wchar_t*)(cityObj.data() + 0x10), kNewCityName) == 0);

    // Vehicle rename with a 40-char Cyrillic name (> 32 chars)
    BYTE* vehPtrs[1] = { veh.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 0x00) = vehPtrs;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 0x08) = vehPtrs + 1;
    LocalRenameMsg renVeh{};
    renVeh.magic = 0x314D414Eu;
    renVeh.kind = 1; // KIND_VEHICLE_RENAME
    renVeh.index = 0;
    renVeh.check = 777;
    const wchar_t* kLongVehName = L"Магистральный_Автопоезд_Восточной_ССР_01";
    renVeh.chars = (unsigned)wcslen(kLongVehName);
    TEST_CHECK(renVeh.chars == 40);
    memcpy(renVeh.text, kLongVehName, renVeh.chars * sizeof(wchar_t));
    TEST_CHECK(Rename_ApplyCommand(&renVeh, sizeof(renVeh)));
    TEST_CHECK(*(BYTE*)(veh.data() + VEH_CUSTOM_NAME_FLAG_OFF) == 1);
    TEST_CHECK(wcscmp((const wchar_t*)(veh.data() + VEH_NAME_OFFSET), kLongVehName) == 0);

    // Line rename on the fly (updates line + 4 and sets line + 1 = 1 so Ref_Line stays synchronized)
    std::vector<BYTE> lineObj(0x100, 0);
    const wchar_t* kOldLineName = L"Line 1";
    memcpy(lineObj.data() + LINE_NAME_OFFSET, kOldLineName, (wcslen(kOldLineName) + 1) * sizeof(wchar_t));
    BYTE* linePtrs[1] = { lineObj.data() };
    std::vector<BYTE> linesMgr(0x40, 0);
    *(BYTE***)(linesMgr.data() + 0x00) = linePtrs;
    *(BYTE***)(linesMgr.data() + 0x08) = linePtrs + 1;
    *(BYTE**)(world + WORLD_LINES_OFFSET) = linesMgr.data();

    RouteRef lineRefBefore{};
    TEST_CHECK(Ref_Line((U64)lineObj.data(), lineRefBefore));
    LocalRenameMsg renLine{};
    renLine.magic = 0x314D414Eu;
    renLine.kind = 8; // KIND_LINE
    renLine.index = ((lineRefBefore.list & 1u) << 31) | (unsigned)lineRefBefore.place;
    renLine.check = lineRefBefore.check;
    const wchar_t* kNewLineName = L"Маршрут Уголь-Сталь";
    renLine.chars = (unsigned)wcslen(kNewLineName);
    memcpy(renLine.text, kNewLineName, renLine.chars * sizeof(wchar_t));
    TEST_CHECK(Rename_ApplyCommand(&renLine, sizeof(renLine)));
    TEST_CHECK(*(BYTE*)(lineObj.data() + LINE_CUSTOM_NAME_FLAG_OFF) == 1);
    TEST_CHECK(wcscmp((const wchar_t*)(lineObj.data() + LINE_NAME_OFFSET), kNewLineName) == 0);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool TestEconStats_HealthAndHappinessTrackerIsolation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    // 1 building in East SSR (X = +600), 0 citizens in West SSR, 4 citizens in East SSR
    std::vector<BYTE> bldEast(0x1420, 0);
    *(int32_t*)(bldEast.data() + 0) = 602;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = +600.0f;
    *(float*)(bldEast.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    BYTE* bldPtrs[1] = { bldEast.data() };
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = bldPtrs;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA)   = bldPtrs + 1;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x00) = bldPtrs;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x08) = bldPtrs + 1;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x10) = bldPtrs + 1;

    std::vector<BYTE> citBuf(4 * 0x800, 0);
    BYTE* citPtrs[4] = {};
    for (int i = 0; i < 4; ++i) {
        citPtrs[i] = citBuf.data() + (size_t)i * 0x800;
        *(BYTE**)(citPtrs[i] + CIT_HOME_BUILDING_OFFSET) = bldEast.data();
        *(float*)(citPtrs[i] + CIT_AGE_OFFSET) = 30.0f;
        *(float*)(citPtrs[i] + 0xD8) = 0.65f;
        *(float*)(citPtrs[i] + 0xF8) = 0.55f;
    }
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x00) = citPtrs;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x08) = citPtrs + 4;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x10) = citPtrs + 4;

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true /*Host = West SSR (0 citizens)*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // Simulate East SSR citizen needs evaluation accumulating health & happiness deterioration causes
    TwoStates_BeforeStep(world);
    TwoStates_CitizenEvalNeedsForTest(world, citPtrs[0], 0 /*alcohol*/, 0.25f, 3 /*food*/, 0.40f);
    TwoStates_CitizenEvalNeedsForTest(world, citPtrs[1], 7 /*Cold*/,    0.60f, 7 /*pub*/,  0.30f);
    TwoStates_AfterStep(world);

    // Roll over current period so $TrackerHealthCur/$TrackerHappyCur copy into $TrackerHealthOld/$TrackerHappyOld
    std::vector<BYTE> histBuf(STAT_RECORD_SIZE, 0);
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x00) = histBuf.data();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x08) = histBuf.data() + STAT_RECORD_SIZE;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x10) = histBuf.data() + STAT_RECORD_SIZE;
    TwoStates_StatRecordClearForTest(world, curStat);

    // Seed world Old tracker with East's values (as the engine's rollover at 0x2FC5BF does)
    *(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 0 * 4) = 0.25f;
    *(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 0 * 4) = 1;
    *(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 7 * 4) = 0.60f;
    *(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 7 * 4) = 1;
    *(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 3 * 4)  = 0.40f;
    *(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 3 * 4)  = 1;
    *(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 7 * 4)  = 0.30f;
    *(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 7 * 4)  = 1;

    // 1. Host (State 1, 0 citizens) enters UI -> MUST see 0 for all health & happiness deterioration causes!
    TwoStates_UiBegin();
    TEST_CHECK(*(int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET) == 0);
    for (int i = 0; i < TRACKER_HEALTH_CAUSE_COUNT; ++i) {
        TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + i * 4) == 0);
        TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + i * 4), 0.0f, 1e-6f);
    }
    for (int i = 0; i < TRACKER_HAPPY_CAUSE_COUNT; ++i) {
        TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + i * 4) == 0);
        TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + i * 4), 0.0f, 1e-6f);
    }
    TwoStates_UiEnd();

    // Verify world tracker block is restored after Host UI ends
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 0 * 4) == 1);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 3 * 4) == 1);

    // 2. Client (State 2, 4 citizens) enters UI -> MUST see East SSR's deterioration causes!
    TwoStates_OnSessionJoined(false /*Client = East SSR*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_UiBegin();
    TEST_CHECK(*(int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET) == 4);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 0 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 0 * 4), 0.25f, 1e-4f);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 7 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 7 * 4), 0.60f, 1e-4f);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 3 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 3 * 4), 0.40f, 1e-4f);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 7 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 7 * 4), 0.30f, 1e-4f);
    TwoStates_UiEnd();

    // 3. 7-parameter float-return H_WaterHealthStep (0x1B0950) ABI & hotel/service capacity (+0xDDC) NaN sanitization:
    //    Verifies XMM2 (waterNeeded), [rsp+30h] (factor7), and XMM0 float return value survive Begin/EndEconAttribution,
    //    and non-finite *(float*)(building + BLD_EFFECTIVE_CAP_FLOAT_OFFSET) is sanitized to 0.0f so cvttss2si at 0x7269B7
    //    never produces 0x80000000 (INT_MIN).
    {
        DiagPerfSnapshot drainPerf = {};
        DiagPerf_TakeSnapshot(&drainPerf);
    }
    *(double*)(world + WORLD_RUB_OFFSET) = 500000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 100000.0;
    *(float*)(bldEast.data() + BLD_EFFECTIVE_CAP_FLOAT_OFFSET) = NAN;
    TwoStates_BeforeStep(world);
    TEST_CHECK_NEAR(*(float*)(bldEast.data() + BLD_EFFECTIVE_CAP_FLOAT_OFFSET), 0.0f, 1e-6f);
    float waterEff = TwoStates_WaterHealthStepForTest(world, bldEast.data(), 0.75f, 0, citPtrs[0], 1, 0.80f, 1200.0f);
    {
        DiagPerfSnapshot midPerf = {};
        DiagPerf_TakeSnapshot(&midPerf);
        TEST_CHECK(midPerf.buckets[DIAG_PERF_TS_ECON_HOOK].count == 0);
    }
    TwoStates_AfterStep(world);
    {
        DiagPerfSnapshot afterPerf = {};
        DiagPerf_TakeSnapshot(&afterPerf);
        TEST_CHECK(afterPerf.buckets[DIAG_PERF_TS_ECON_HOOK].count == 2);
    }
    TEST_CHECK_NEAR(waterEff, 0.60f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(bldEast.data() + BLD_EFFECTIVE_CAP_FLOAT_OFFSET), 16.2f, 1e-3f);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1.0);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 498800.0, 1.0);

    // Non-finite waterNeeded / factor7 must be sanitized to 0.0f and never poison building + 0xDDC
    TwoStates_BeforeStep(world);
    float nanEff = TwoStates_WaterHealthStepForTest(world, bldEast.data(), NAN, 1, citPtrs[0], 1, -INFINITY, 0.0f);
    TwoStates_AfterStep(world);
    TEST_CHECK(isfinite(nanEff) && nanEff == 0.0f);
    TEST_CHECK(isfinite(*(float*)(bldEast.data() + BLD_EFFECTIVE_CAP_FLOAT_OFFSET)));

    // Verify TwoStates_OnSessionEnded() flushes buffered DIAG_PERF_TS_ECON_HOOK samples if AfterStep was not reached
    {
        DiagPerfSnapshot drainPerf2 = {};
        DiagPerf_TakeSnapshot(&drainPerf2);
        TwoStates_BeforeStep(world);
        TwoStates_WaterHealthStepForTest(world, bldEast.data(), 0.50f, 0, citPtrs[0], 1, 0.50f, 0.0f);
        DiagPerfSnapshot midPerf2 = {};
        DiagPerf_TakeSnapshot(&midPerf2);
        TEST_CHECK(midPerf2.buckets[DIAG_PERF_TS_ECON_HOOK].count == 0);
        TwoStates_OnSessionEnded();
        DiagPerfSnapshot endPerf = {};
        DiagPerf_TakeSnapshot(&endPerf);
        TEST_CHECK(endPerf.buckets[DIAG_PERF_TS_ECON_HOOK].count == 2);
    }
    ResetMockWorldMemory();
    return true;
}

static bool TestEconStats_CitizenDeathSevereIllnessAndStudioIsolation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    std::vector<BYTE> bldWest(0x1420, 0), bldEast(0x1420, 0);
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -700.0f;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = +700.0f;
    *(float*)(bldWest.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(float*)(bldEast.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    BYTE* bldPtrs[2] = { bldWest.data(), bldEast.data() };
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = bldPtrs;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA)   = bldPtrs + 2;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x00) = bldPtrs;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x08) = bldPtrs + 2;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 0x10) = bldPtrs + 2;

    std::vector<BYTE> citWest(0x800, 0), citEast(0x800, 0);
    *(BYTE**)(citWest.data() + CIT_HOME_BUILDING_OFFSET) = bldWest.data();
    *(BYTE**)(citEast.data() + CIT_HOME_BUILDING_OFFSET) = bldEast.data();
    *(float*)(citWest.data() + CIT_AGE_OFFSET) = 28.0f;
    *(float*)(citEast.data() + CIT_AGE_OFFSET) = 34.0f;
    BYTE* citPtrs[2] = { citWest.data(), citEast.data() };
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x00) = citPtrs;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x08) = citPtrs + 2;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 0x10) = citPtrs + 2;

    std::vector<BYTE> studioWest(0x200, 0), studioEast(0x200, 0);
    *(BYTE**)(studioWest.data() + STUDIO_BUILDING_OFFSET) = bldWest.data();
    *(BYTE**)(studioEast.data() + STUDIO_BUILDING_OFFSET) = bldEast.data();
    BYTE* studioPtrs[2] = { studioWest.data(), studioEast.data() };
    *(BYTE***)(world + WORLD_STUDIOS_BEGIN_OFFSET) = studioPtrs;
    *(BYTE***)(world + WORLD_STUDIOS_END_OFFSET)   = studioPtrs + 2;
    *(BYTE***)(world + WORLD_STUDIOS_BEGIN_OFFSET + 0x10) = studioPtrs + 2;

    double initRub[2] = { 300000.0, 300000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // 1. Radio/TV studio selection (0x4C2610) filters WORLD_STUDIOS_BEGIN_OFFSET to the citizen's own state
    TEST_CHECK(TwoStates_CitizenStudioSelectForTest(world, citWest.data()) == studioWest.data());
    TEST_CHECK(TwoStates_CitizenStudioSelectForTest(world, citEast.data()) == studioEast.data());
    TEST_CHECK(SafeCountStdVector(world + WORLD_STUDIOS_BEGIN_OFFSET, sizeof(void*)) == 2);

    // 2. Severe illness tick (0x833509, health cause 9), studio broadcast effect (0x4C21F0, happy cause 9),
    //    and citizen death (0x835D70, $Citizens_Dead + happy causes 11 & 12) on an East SSR citizen followed
    //    immediately by a West SSR citizen must attribute 100% to East SSR (State 2) and 0% to West SSR (State 1).
    TwoStates_BeforeStep(world);
    TwoStates_CitizenSevereIllnessTickForTest(world, 0.35f, citEast.data());
    TwoStates_CitizenStudioEffectForTest(world, studioEast.data(), citEast.data(), 0.50f);
    TwoStates_CitizenRemoveDeathForTest(world, citEast.data(), 0.45f, 0.25f);
    TwoStates_CitizenEvalNeedsForTest(world, citWest.data(), 0 /*alcohol*/, 0.15f, -1, 0.0f);
    TwoStates_AfterStep(world);

    std::vector<BYTE> histBuf(STAT_RECORD_SIZE, 0);
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x00) = histBuf.data();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x08) = histBuf.data() + STAT_RECORD_SIZE;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x10) = histBuf.data() + STAT_RECORD_SIZE;
    TwoStates_StatRecordClearForTest(world, curStat);

    // Host UI (State 1 = West SSR): 0 deaths, 0 severe illness, 0 studio/orphan/grief happiness effects
    TwoStates_UiBegin();
    TEST_CHECK(*(int32_t*)(histBuf.data() + STAT_CITIZEN_COUNTS_OFF + 1 * sizeof(int32_t)) == 0);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 9 * 4) == 0);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 9 * 4), 0.0f, 1e-6f);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 9 * 4) == 0);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 11 * 4) == 0);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 12 * 4) == 0);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 0 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 0 * 4), 0.15f, 1e-4f);
    TwoStates_UiEnd();

    // Client UI (State 2 = East SSR): 1 death, 0.35 severe illness, 0.50 studio, 0.45 orphan, 0.25 grief
    TwoStates_OnSessionJoined(false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_UiBegin();
    TEST_CHECK(*(int32_t*)(histBuf.data() + STAT_CITIZEN_COUNTS_OFF + 1 * sizeof(int32_t)) == 1);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 9 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 9 * 4), 0.35f, 1e-4f);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 9 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 9 * 4), 0.50f, 1e-4f);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 11 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 11 * 4), 0.45f, 1e-4f);
    TEST_CHECK(*(int32_t*)(world + WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 12 * 4) == 1);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 12 * 4), 0.25f, 1e-4f);
    TwoStates_UiEnd();

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool TestEconStats_CustomsTouristSpawnAndVehicleTouristOwner()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    // Set up 39 research items at WORLD_RESEARCH_VEC_BEGIN_OFFSET (matching SOVIET64.exe)
    std::vector<BYTE> researchItems(39 * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < 39; ++i) {
        BYTE* item = researchItems.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        item[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
        item[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(item + RESEARCH_ITEM_REQ_WORK_OFFSET) = 10; // 1 step = +1.0f progress
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET)     = researchItems.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)       = researchItems.data() + researchItems.size();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET + 8)   = researchItems.data() + researchItems.size();

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true /*Host = State 1*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // State 2 (Client) university at X = +600 completes Soviet Tourism Visa (unlock[12]) and Marketing (unlock[14]),
    // while State 1 (Host) has NOT researched tourism (unlock[12] == 0, unlock[14] == 0).
    std::vector<BYTE> univEast(0x1420, 0);
    *(float*)(univEast.data() + OBJ_POS_OFFSET) = +600.0f;
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univEast.data(), 2, 12));
    TwoStates_BldUnivStepForTest(world, univEast.data());
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univEast.data(), 2, 14));
    TwoStates_BldUnivStepForTest(world, univEast.data());
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 12) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 14) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 12) == 0);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 14) == 0);

    // Verify Host's live world unlocks at WORLD_RESEARCH_UNLOCKS_OFFSET are still 0 (State 1 has not unlocked tourism)
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 12) == 0);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 14) == 0);

    // Record 4 completed Soviet tourist trips for State 2 with total rating sum 16.0f (avgRating = 4.0f)
    TwoStates_BeginCommand(2);
    {
        *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 4 * sizeof(int32_t)) += 4;
        float ratingSum = 16.0f;
        memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 10 * sizeof(int32_t), &ratingSum, sizeof(float));
    }
    TwoStates_EndCommand(2);

    // Create a Soviet Beyond-the-Borders / Customs House (category 0x14, subtype 0 = Soviet)
    // Even if placed at X = -800 (airspace border point on West side), ResolveCustomsTourismOwner
    // must attribute it to State 2 because only State 2 has unlocked Soviet tourism visa (unlock[12] != 0)!
    std::vector<BYTE> bldTypeSovietBorder(0xC00, 0);
    *(uint8_t*)(bldTypeSovietBorder.data() + BLD_TYPE_CAT_OFF) = 0x14; // Beyond the Borders (Air/Sea)
    *(uint8_t*)(bldTypeSovietBorder.data() + BLD_TYPE_CUSTOMS_SUBTYPE_OFF) = 0;    // 0 = Soviet border

    std::vector<BYTE> customsEast(0x1420, 0);
    *(BYTE**)(customsEast.data() + 0x318) = bldTypeSovietBorder.data();
    *(float*)(customsEast.data() + OBJ_POS_OFFSET) = -800.0f;

    float avgRating = 0.0f;
    int32_t spawnCount = 0;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsEast.data(), &avgRating, &spawnCount);
    TEST_CHECK_NEAR(avgRating, 4.0f, 1e-3f);
    TEST_CHECK(spawnCount == 8); // 2 base + 2 marketing + 4 rating = 8 waiting tourists!

    // Verify first tick of H_BldCustomsTouristStep primes BLD_CUSTOMS_TOURISTS_SPAWNED_OFF (+0x1234) and spawns immediately
    *(int32_t*)(customsEast.data() + BLD_CUSTOMS_TOURISTS_SPAWNED_OFF) = 0;
    *(float*)(customsEast.data() + BLD_CUSTOMS_TOURIST_TIMER_OFFSET)   = 0.0f;
    TwoStates_BldCustomsTouristStepForTest(world, customsEast.data());
    TEST_CHECK(*(int32_t*)(customsEast.data() + BLD_CUSTOMS_TOURISTS_SPAWNED_OFF) == 1 + spawnCount);
    TEST_CHECK_NEAR(*(float*)(customsEast.data() + BLD_CUSTOMS_TOURIST_TIMER_OFFSET), 0.0f, 1e-3f);

    // Verify ResolveCitizenOwnerFast for a tourist waiting at customsEast or riding a State 2 airplane/bus (cit + 0x60)
    std::vector<BYTE> touristCit(0x800, 0);
    *(int32_t*)(touristCit.data() + CIT_INFANT_FLAG_OFFSET) = 1; // Soviet tourist
    *(BYTE**)(touristCit.data() + 0x58) = customsEast.data();
    TEST_CHECK(TwoStates_ResolveCitizenOwnerForTest(touristCit.data()) == 2);

    // When the tourist boards a State 2 airplane (0x831120 clears cit+0x58 = nullptr and sets cit+0x60 = vehicle),
    // ResolveCitizenOwnerFast must still resolve to State 2 via cit + 0x60!
    std::vector<BYTE> planeEast(0x1400, 0);
    *(int32_t*)(planeEast.data() + VEH_ID_OFFSET) = 777;
    TwoStates_RegisterVehicleOwner(777, 2);
    *(BYTE**)(touristCit.data() + 0x58) = nullptr;
    *(BYTE**)(touristCit.data() + 0x60) = planeEast.data();
    *(float*)(touristCit.data() + CIT_POS_OFFSET) = -1200.0f; // flying over West airspace
    TEST_CHECK(TwoStates_ResolveCitizenOwnerForTest(touristCit.data()) == 2);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

bool Test_TwoStates_EconomyStats()
{
    return TestEconStats_ResourceLedgerAndScalarsIsolation() &&
           TestEconStats_UiSwapAndExactSimulationRestore() &&
           TestEconStats_PeriodRolloverAndSaveMetaPersistence() &&
           TestEconStats_BatchConstructionAndDemographics() &&
           TestEconStats_LeafHookRelayStubPreservesRegisters() &&
           TestEconStats_PosKeyAndPoisonedSaveHeal() &&
           TestEconStats_VehicleSaleAbiAndRenameAndCarDealerSwitches() &&
           TestEconStats_HealthAndHappinessTrackerIsolation() &&
           TestEconStats_CitizenDeathSevereIllnessAndStudioIsolation() &&
           TestEconStats_CustomsTouristSpawnAndVehicleTouristOwner();
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
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1; // mock world loaded
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1; // world loaded
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

bool Test_SingleplayerIsolationAndReset()
{
    return TestSingleplayer_InitialDormancy() &&
           TestSingleplayer_OverlayZeroDrawCalls() &&
           TestSingleplayer_InputBlockAndPatchesDormancy() &&
           TestSingleplayer_SessionTransitionReset() &&
           TestSingleplayer_WindowTitleRestoration();
}

// ---------------------------------------------------------------------------
// Test 14: Per-Player Camera Persistence, Resync Restore & Initial Framing
// ---------------------------------------------------------------------------

bool Test_CameraPersistenceAndFraming()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
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

    // 4. Pending Camera Restore: re-apply client target over first 10 frames against engine overwrite, then unlock
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1;
    *(volatile char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;

    const float clientSavedTargetPos[3]  = { 4500.0f, 220.0f, -3200.0f };
    const float clientSavedTargetLook[3] = { 4800.0f,  40.0f, -3500.0f };
    TwoStates_SetSavedCameraForTest(2, clientSavedTargetPos, clientSavedTargetLook);

    TwoStates_RestoreOrFrameCameraForTest(false);

    float pendPos[3] = {}, pendLook[3] = {};
    int remFrames = 0, checkFrames = 0;
    TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(pendPos, pendLook, &remFrames, &checkFrames));
    TEST_CHECK(remFrames == 10);
    TEST_CHECK(checkFrames == 0);
    TEST_CHECK_NEAR(pendPos[0], clientSavedTargetPos[0], 1e-2f);

    // Overwrite with host's camera (simulate engine late initialization overwriting camera)
    const float hostOverwritePos[3]  = { -5000.0f, 300.0f, 1000.0f };
    const float hostOverwriteLook[3] = { -5200.0f,  50.0f, 1200.0f };

    for (int frame = 1; frame <= 10; ++frame) {
        TEST_CHECK(TwoStates_ApplyCameraView(hostOverwritePos, hostOverwriteLook));
        TwoStates_TickPendingCameraRestore();
        float curP[3] = {}, curL[3] = {};
        TEST_CHECK(TwoStates_GetCameraView(curP, curL));
        TEST_CHECK_NEAR(curP[0], clientSavedTargetPos[0], 1e-2f);
        TEST_CHECK_NEAR(curL[2], clientSavedTargetLook[2], 1e-2f);

        int curRem = 0, curCnt = 0;
        TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(nullptr, nullptr, &curRem, &curCnt));
        TEST_CHECK(curRem == 10 - frame);
        TEST_CHECK(curCnt == frame);
    }

    // After 10 frames, camera is unlocked for player input
    const float playerMovedPos[3]  = { 5555.0f, 250.0f, -2222.0f };
    const float playerMovedLook[3] = { 5800.0f,  30.0f, -2400.0f };
    TEST_CHECK(TwoStates_ApplyCameraView(playerMovedPos, playerMovedLook));

    TwoStates_TickPendingCameraRestore(); // Frame 11
    float afterUnlockPos[3] = {}, afterUnlockLook[3] = {};
    TEST_CHECK(TwoStates_GetCameraView(afterUnlockPos, afterUnlockLook));
    TEST_CHECK_NEAR(afterUnlockPos[0], playerMovedPos[0], 1e-2f);
    TEST_CHECK_NEAR(afterUnlockLook[2], playerMovedLook[2], 1e-2f);

    // Advance to frame 60 verification check
    for (int frame = 12; frame <= 60; ++frame) {
        TwoStates_TickPendingCameraRestore();
    }
    TEST_CHECK(!TwoStates_GetPendingCameraRestoreForTest(nullptr, nullptr, nullptr, nullptr));

    // 5. Yaw calculation helper & edge cases
    const float p0[3] = { 0.0f, 0.0f, 0.0f };
    const float lNorth[3] = { 0.0f, 0.0f, 100.0f };
    const float lEast[3]  = { 100.0f, 0.0f, 0.0f };
    const float lSouth[3] = { 0.0f, 0.0f, -100.0f };
    const float lWest[3]  = { -100.0f, 0.0f, 0.0f };
    TEST_CHECK_NEAR(MinimapCameraYaw(p0, lNorth), 0.0f, 1e-4f);
    TEST_CHECK_NEAR(MinimapCameraYaw(p0, lEast), 1.570796f, 1e-4f);
    TEST_CHECK_NEAR(fabsf(MinimapCameraYaw(p0, lSouth)), 3.141592f, 1e-4f);
    TEST_CHECK_NEAR(MinimapCameraYaw(p0, lWest), -1.570796f, 1e-4f);

    // Degenerate vectors
    TEST_CHECK_NEAR(MinimapCameraYaw(p0, p0), 0.0f, 1e-5f);
    const float pNaN[3] = { NAN, 0.0f, 0.0f };
    TEST_CHECK_NEAR(MinimapCameraYaw(pNaN, lNorth), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(MinimapCameraYaw(nullptr, lNorth), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(MinimapCameraYaw(p0, nullptr), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(MinimapCameraYaw(nullptr, nullptr), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(MinimapCameraYaw(1e38f, 0.0f, -1e38f, 0.0f), -1.570796f, 1e-4f);
    TEST_CHECK_NEAR(MinimapCameraYaw(3e38f, 0.0f, -3e38f, 0.0f), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(MinimapCameraYaw(0.0f, 0.0f, 1e20f, 1e20f), 0.785398f, 1e-4f);

    // 6. World-to-minimap pixel coordinate mapping and bounding checks
    float outPx = 0.0f, outPy = 0.0f;
    TEST_CHECK(MinimapWorldToPx(0.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy));
    TEST_CHECK_NEAR(outPx, 100.0f, 1e-2f);
    TEST_CHECK_NEAR(outPy, 100.0f, 1e-2f);

    // Positive X goes right, Positive Z goes North (up on screen => py decreases)
    TEST_CHECK(MinimapWorldToPx(5000.0f, 5000.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy));
    TEST_CHECK_NEAR(outPx, 150.0f, 1e-2f);
    TEST_CHECK_NEAR(outPy, 50.0f, 1e-2f);

    // Boundary check with default margin 2.0f:
    // map bounds: cx +/- half = 100 +/- 100 => [0..200]. Visible interior: [2..198].
    TEST_CHECK(!MinimapWorldToPx(10000.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy));
    TEST_CHECK(!MinimapWorldToPx(-10000.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy));
    TEST_CHECK(!MinimapWorldToPx(25000.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy));
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, 100.0f, 100.0f, -50.0f, &outPx, &outPy));
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, 100.0f, 100.0f, 200.0f, nullptr, &outPy));
    // Additional adversarial edge cases: NaN cx/cy/margin, negative margin, and margin >= half
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, NAN, 100.0f, 200.0f, &outPx, &outPy));
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, 100.0f, NAN, 200.0f, &outPx, &outPy));
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy, NAN));
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy, -5.0f));
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy, 100.0f));
    TEST_CHECK(!MinimapWorldToPx(0.0f, 0.0f, 100.0f, 100.0f, 200.0f, &outPx, &outPy, 150.0f));

    // Output pointer preservation check on failed boundary
    float untouchedPx = 1234.5f, untouchedPy = 6789.0f;
    TEST_CHECK(!MinimapWorldToPx(50000.0f, 0.0f, 100.0f, 100.0f, 200.0f, &untouchedPx, &untouchedPy));
    TEST_CHECK_NEAR(untouchedPx, 1234.5f, 1e-4f);
    TEST_CHECK_NEAR(untouchedPy, 6789.0f, 1e-4f);
    TEST_CHECK(!MinimapWorldToPx(1e38f, 0.0f, 100.0f, 100.0f, 200.0f, &untouchedPx, &untouchedPy));
    TEST_CHECK(!MinimapWorldToPx(-1e38f, 0.0f, 100.0f, 100.0f, 200.0f, &untouchedPx, &untouchedPy));

    // 7. Verify HUD_QUAD_ROT_RVA is reset to 0 after rendering
    TEST_CHECK(TwoStates_TestMinimapRotResetForTest());

    // 8. Verify pending camera restore reset on BeforeAnyWorldLoad and OnSessionEnded
    TwoStates_RestoreOrFrameCameraForTest(false);
    TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(nullptr, nullptr, nullptr, nullptr));
    TwoStates_BeforeAnyWorldLoad();
    TEST_CHECK(!TwoStates_GetPendingCameraRestoreForTest(nullptr, nullptr, nullptr, nullptr));
    TwoStates_OnAnyWorldLoaded(false);

    TwoStates_RestoreOrFrameCameraForTest(false);
    TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(nullptr, nullptr, nullptr, nullptr));
    TwoStates_OnSessionEnded();
    TEST_CHECK(!TwoStates_GetPendingCameraRestoreForTest(nullptr, nullptr, nullptr, nullptr));

    // 9. Verify SEH depth balance across Camera/Cursor access functions
    int initialSehDepth = DiagLog_GetSafeSehDepthForTest();
    TEST_CHECK(initialSehDepth == 0);

    float probePos[3] = { 1200.0f, 180.0f, -800.0f };
    float probeLook[3] = { 1500.0f, 50.0f, -1100.0f };
    TEST_CHECK(TwoStates_ApplyCameraView(probePos, probeLook));
    TEST_CHECK(DiagLog_GetSafeSehDepthForTest() == initialSehDepth);

    float readBackPos[3] = {}, readBackLook[3] = {};
    TEST_CHECK(TwoStates_GetCameraView(readBackPos, readBackLook));
    TEST_CHECK(DiagLog_GetSafeSehDepthForTest() == initialSehDepth);

    float curWx = 0.0f, curWy = 0.0f, curWz = 0.0f;
    TwoStates_GetCursorWorldPos(&curWx, &curWy, &curWz);
    TEST_CHECK(DiagLog_GetSafeSehDepthForTest() == initialSehDepth);

    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 0;
    std::filesystem::remove_all(camSaveDir, ec);
    return true;
}

// ---------------------------------------------------------------------------
// Tests 30 & 31: Milestone 3 Civic & Demographic Empirical Stress Suites
// ---------------------------------------------------------------------------

#define Test_Stress_M3_Challenger2 Test_Stress_M3_Challenger2_Impl
#include "m3_challenger2_stress.cpp"
#undef Test_Stress_M3_Challenger2

#define Test_Stress_M3_Challenger1_R9_R12 Test_Stress_M3_Challenger1_R9_R12_Impl
#include "m3_challenger1_stress.cpp"
#undef Test_Stress_M3_Challenger1_R9_R12

bool Test_Stress_M3_Challenger2()
{
    return Test_Stress_M3_Challenger2_Impl();
}

bool Test_Stress_M3_Challenger1_R9_R12()
{
    return Test_Stress_M3_Challenger1_R9_R12_Impl();
}

// ---------------------------------------------------------------------------
// Test 62: Two-States Research Desync & Mid-Step Clobbering Fixes
// ---------------------------------------------------------------------------

bool Test_TwoStatesResearchDesyncFixes()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(int*)(world + WORLD_DAY_OFFSET)  = 100;
    *(int*)(world + WORLD_YEAR_OFFSET) = 1970;

    std::vector<BYTE> resBuf(24 * RESEARCH_ITEM_STRIDE, 0);
    auto resetWorldResearchToState1 = [&]() {
        memset(world + WORLD_RESEARCH_UNLOCKS_OFFSET, 0, WORLD_RESEARCH_UNLOCKS_BYTES);
        memset(resBuf.data(), 0, resBuf.size());
        for (int i = 0; i < 24; ++i) {
            BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
            it[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
            it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
            *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
        }
        *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET)   = resBuf.data();
        *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)     = resBuf.data() + resBuf.size();
        *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET + 8) = resBuf.data() + resBuf.size();
    };
    resetWorldResearchToState1();

    std::vector<BYTE> bldTypeUsd(0xC00, 0);
    *(uint8_t*)(bldTypeUsd.data() + BLD_TYPE_CAT_OFF) = 0x14;
    *(uint8_t*)(bldTypeUsd.data() + BLD_TYPE_CUSTOMS_SUBTYPE_OFF) = 1; // 1 = USD customs -> visa unlock idx 11

    std::vector<BYTE> univWest(0x1420, 0), univEast(0x1420, 0), customsUsdEast(0x1420, 0);
    *(int*)univWest.data() = 501;
    *(float*)(univWest.data() + OBJ_POS_OFFSET) = -800.0f;
    *(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;
    *(int*)univEast.data() = 502;
    *(float*)(univEast.data() + OBJ_POS_OFFSET) = 800.0f;
    *(float*)(univEast.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;
    *(int*)customsUsdEast.data() = 503;
    *(float*)(customsUsdEast.data() + OBJ_POS_OFFSET) = 2000.0f;
    *(BYTE**)(customsUsdEast.data() + 0x318) = bldTypeUsd.data();

    std::vector<BYTE*> bldVec = { univWest.data(), univEast.data(), customsUsdEast.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true /*Host = State 1*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // State 2 unlocks techs {0, 1, 4, 11} and reaches 0.50f progress on tech 2; State 1 stays at 0
    const int completedTechs[] = { 0, 1, 4, 11 };
    for (int techIdx : completedTechs) {
        TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univEast.data(), 2, techIdx));
        TwoStates_BldUnivStepForTest(world, univEast.data());
        TwoStates_BldUnivStepForTest(world, univEast.data());
    }
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univEast.data(), 2, 2));
    TwoStates_BldUnivStepForTest(world, univEast.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 2), 0.50f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 11) == 0);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 11) == 1);

    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_research_desync_fixes";
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir.string().c_str()));

    auto loadSaveAsRole = [&](bool asHost) -> bool {
        TwoStates_OnSessionEnded();
        TwoStates_ResetResearch();
        resetWorldResearchToState1();
        TwoStates_OnSessionJoined(asHost, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
        if (!TwoStates_ReadSaveMetaFromDir(saveDir.string().c_str(), true)) return false;
        TwoStates_OnWorldLoaded();
        return true;
    };

    // 1. Host vs Client hashResearch parity after AfterStep + bidirectional sensitivity
    TwoStates_BeforeStep(world);
    TwoStates_AfterStep(world);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 11) == 0);
    StateSummary hostSum = {};
    TEST_CHECK(StateHash_Compute(&hostSum));

    TEST_CHECK(loadSaveAsRole(false));
    TwoStates_BeforeStep(world);
    TwoStates_AfterStep(world);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 11) == 1);
    StateSummary clientSum = {};
    TEST_CHECK(StateHash_Compute(&clientSum));

    TEST_CHECK(hostSum.hashResearch == clientSum.hashResearch);
    char diffBuf[256] = {};
    TEST_CHECK(!LockstepNet_CompareSummaryForTest(hostSum, clientSum, diffBuf, sizeof(diffBuf)));
    TEST_CHECK(diffBuf[0] == '\0');

    // Mutate State 1 on Client -> hashResearch must change
    TwoStates_SetEngineResearchUnlockByteForTest(1, 5, 1);
    StateSummary mutS1OnClient = {};
    TEST_CHECK(StateHash_Compute(&mutS1OnClient));
    TEST_CHECK(mutS1OnClient.hashResearch != clientSum.hashResearch);
    TwoStates_SetEngineResearchUnlockByteForTest(1, 5, 0);

    // Mutate State 2 on Host -> hashResearch must change
    TEST_CHECK(loadSaveAsRole(true));
    TwoStates_SetEngineResearchUnlockByteForTest(2, 5, 1);
    StateSummary mutS2OnHost = {};
    TEST_CHECK(StateHash_Compute(&mutS2OnHost));
    TEST_CHECK(mutS2OnHost.hashResearch != hostSum.hashResearch);
    TwoStates_SetEngineResearchUnlockByteForTest(2, 5, 0);

    // Switch back to Client (LocalState = 2) for mid-step & idempotency checks
    TEST_CHECK(loadSaveAsRole(false));

    // 2. Mid-SimStep H_ResearchDailyUpdate must keep canonical State 1 on Client
    TwoStates_BeforeStep(world);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) == 0);
    TwoStates_ResearchDailyUpdateForTest(world);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) == 0);
    TwoStates_AfterStep(world);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) == 1);

    // 3. Mid-SimStep CustomsTourismWorldSwapScope must keep canonical State 1 on Client
    TwoStates_SetStateEconExtraCountForTest(2, 5, 4);
    TwoStates_SetStateEconExtraFloatForTest(2, 11, 16.0f);
    TwoStates_BeforeStep(world);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 11) == 0);
    float avgRating = 0.0f;
    int32_t spawnCount = 0;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsdEast.data(), &avgRating, &spawnCount);
    TEST_CHECK(spawnCount > 0);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 11) == 0);
    TwoStates_AfterStep(world);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 11) == 1);

    // 4. TwoStates_BeforeStep idempotency: consecutive BeforeStep must not clobber State 2 on Client
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 4) == 1);
    TwoStates_BeforeStep(world);
    TwoStates_BeforeStep(world);
    TwoStates_AfterStep(world);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 4) == 1);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 4) == 1);

    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    std::filesystem::remove_all(saveDir, ec);
    ResetMockWorldMemory();
    return true;
}

bool TestPerf_BldPtrOwnerCacheParityAndInvalidation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Determinism_SetSessionRunningForTest(false);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 50000.0, 50000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    // Allocate >512 structures (560 buildings + 32 constructions) to exercise overflow table growth
    const int kBldCount = 560;
    const int kConCount = 32;
    std::vector<BYTE> bldPool((size_t)kBldCount * 0x1420u, 0);
    std::vector<BYTE> conPool((size_t)kConCount * 0x1420u, 0);
    std::vector<BYTE*> bldPtrs((size_t)kBldCount, nullptr);
    std::vector<BYTE*> conPtrs((size_t)kConCount, nullptr);

    for (int i = 0; i < kBldCount; ++i) {
        BYTE* b = bldPool.data() + (size_t)i * 0x1420u;
        *(int*)b = 1000 + i;
        float x = (i % 2 == 0) ? (-1200.0f - (float)i) : (1200.0f + (float)i);
        if (i == 10) x = -40.0f; // shared border West -> override to East
        if (i == 11) x =  40.0f; // shared border East -> override to West
        *(float*)(b + OBJ_POS_OFFSET) = x;
        *(float*)(b + OBJ_POS_OFFSET + 8) = (float)i * 5.0f;
        bldPtrs[(size_t)i] = b;
    }
    for (int i = 0; i < kConCount; ++i) {
        BYTE* c = conPool.data() + (size_t)i * 0x1420u;
        *(int*)c = 5000 + i;
        float x = (i % 2 == 0) ? -600.0f : 600.0f;
        *(float*)(c + OBJ_POS_OFFSET) = x;
        *(float*)(c + OBJ_POS_OFFSET + 8) = 100.0f + (float)i * 3.0f;
        conPtrs[(size_t)i] = c;
    }
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldPtrs);
    WriteMockVectorHeader(world + WORLD_CONSTRUCTIONS_OFFSET, conPtrs);

    // Explicit ownership overrides inside shared border
    TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(-40.0f, 50.0f), 2);
    TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey( 40.0f, 55.0f), 1);

    // 1. Parity vs TwoStates_GetBuildingOwnerSlot across all buildings, constructions, unindexed & invalid pointers
    for (int i = 0; i < kBldCount; ++i) {
        TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[(size_t)i]) ==
                   TwoStates_GetBuildingOwnerSlot(bldPtrs[(size_t)i]));
    }
    for (int i = 0; i < kConCount; ++i) {
        TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(conPtrs[(size_t)i]) ==
                   TwoStates_GetBuildingOwnerSlot(conPtrs[(size_t)i]));
    }
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[10]) == 2);
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[11]) == 1);

    std::vector<BYTE> unindexedBld(0x1420u, 0);
    *(float*)(unindexedBld.data() + OBJ_POS_OFFSET) = 900.0f;
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(unindexedBld.data()) == 2);
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(nullptr) == 1);
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest((const BYTE*)(uintptr_t)0x1234) == 1);
    TEST_CHECK(DiagLog_GetSafeSehDepthForTest() == 0);

    // 2. Generation counter bumps on every mutation/clear site
    uint64_t g0 = TwoStates_GetOwnerMapGenForTest();
    TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(-300.0f, 10.0f), 2);
    uint64_t g1 = TwoStates_GetOwnerMapGenForTest();
    TEST_CHECK(g1 > g0);

    TwoStates_SetBorderHalfWidth(300.0f);
    uint64_t g2 = TwoStates_GetOwnerMapGenForTest();
    TEST_CHECK(g2 > g1);

    // Prune dead entity (-300, 10 is not in world buildings vec)
    TwoStates_PruneDeadEntities(false);
    uint64_t g3 = TwoStates_GetOwnerMapGenForTest();
    TEST_CHECK(g3 > g2);

    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_owner_cache_meta";
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir.string().c_str()));
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir.string().c_str(), false));
    uint64_t g4 = TwoStates_GetOwnerMapGenForTest();
    TEST_CHECK(g4 > g3);

    // 3. SessionRunning rebuild skip gating vs Waiting Lobby always-rebuild
    Determinism_SetSessionRunningForTest(true);
    uint64_t r0 = TwoStates_GetBldPtrCacheRebuildCountForTest();
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[0]) == 1);
    uint64_t r1 = TwoStates_GetBldPtrCacheRebuildCountForTest();
    TEST_CHECK(r1 == r0 + 1);
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[1]) == 2);
    uint64_t r2 = TwoStates_GetBldPtrCacheRebuildCountForTest();
    TEST_CHECK(r2 == r1); // skipped rebuild while SessionRunning == true

    // Mutation bumps gen -> rebuild occurs
    TwoStates_SetBorderHalfWidth(150.0f);
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[0]) == 1);
    uint64_t r3 = TwoStates_GetBldPtrCacheRebuildCountForTest();
    TEST_CHECK(r3 == r2 + 1);

    // Waiting Lobby (!SessionRunning) always rebuilds per entry
    Determinism_SetSessionRunningForTest(false);
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[0]) == 1);
    uint64_t r4 = TwoStates_GetBldPtrCacheRebuildCountForTest();
    TEST_CHECK(TwoStates_LookupBuildingOwnerCachedForTest(bldPtrs[0]) == 1);
    uint64_t r5 = TwoStates_GetBldPtrCacheRebuildCountForTest();
    TEST_CHECK(r4 > r3 && r5 > r4);

    // OnSessionEnded & OnAnyWorldLoaded non-coop clear bumps gen
    uint64_t g5 = TwoStates_GetOwnerMapGenForTest();
    TwoStates_OnSessionEnded();
    uint64_t g6 = TwoStates_GetOwnerMapGenForTest();
    TEST_CHECK(g6 > g5);

    std::filesystem::remove_all(saveDir, ec);
    TwoStates_ResetResearch();
    ResetMockWorldMemory();
    return true;
}

bool TestPerf_SinglePassDemographicsAndUiCacheParity()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    Determinism_SetActiveForTest(false);
    Determinism_SetSessionRunningForTest(false);
    Determinism_SetStepCountForTest(0);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Setup research vector so Early/Late flip-back can be checked cleanly
    std::vector<BYTE> resBuf(8 * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < 8; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET] = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET)   = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)     = resBuf.data() + resBuf.size();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET + 8) = resBuf.data() + resBuf.size();

    // Setup buildings: West home, East home, East dorm, East prison, under-construction West bld
    std::vector<BYTE> btypeHome(0xC00, 0), btypeDorm(0xC00, 0), btypePrison(0xC00, 0);
    *(int32_t*)(btypeHome.data() + BLD_TYPE_CAT_OFF) = 2;
    *(int32_t*)(btypeHome.data() + 0x36C) = 0;
    *(int32_t*)(btypeDorm.data() + BLD_TYPE_CAT_OFF) = 2;
    *(int32_t*)(btypeDorm.data() + 0x36C) = 1;
    *(BYTE*)(btypeDorm.data() + BLD_TYPE_IS_DORM_OFF) = 1;
    *(int32_t*)(btypePrison.data() + BLD_TYPE_CAT_OFF) = 0x4C;
    *(BYTE*)(btypePrison.data() + BLD_TYPE_IS_DORM_OFF) = 1;

    std::vector<BYTE> homeW(0x1420, 0), homeE(0x1420, 0), dormE(0x1420, 0), prisonE(0x1420, 0), conW(0x1420, 0);
    *(int*)homeW.data() = 1; *(float*)(homeW.data() + OBJ_POS_OFFSET) = -800.0f; *(BYTE**)(homeW.data() + 0x318) = btypeHome.data();
    *(int*)homeE.data() = 2; *(float*)(homeE.data() + OBJ_POS_OFFSET) =  800.0f; *(BYTE**)(homeE.data() + 0x318) = btypeHome.data();
    *(int*)dormE.data() = 3; *(float*)(dormE.data() + OBJ_POS_OFFSET) =  900.0f; *(BYTE**)(dormE.data() + 0x318) = btypeDorm.data();
    *(int*)prisonE.data() = 4; *(float*)(prisonE.data() + OBJ_POS_OFFSET) = 950.0f; *(BYTE**)(prisonE.data() + 0x318) = btypePrison.data();
    *(int*)conW.data() = 5; *(float*)(conW.data() + OBJ_POS_OFFSET) = -700.0f;
    *(float*)(conW.data() + BLD_PROGRESS_OFFSET) = 0.5f;
    conW[BLD_PAY_RUB_OFFSET] = 1;
    std::vector<BYTE*> bldVec = { homeW.data(), homeE.data(), dormE.data(), prisonE.data(), conW.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    // Setup 11 citizens covering all 3 UI/demo predicates
    std::vector<BYTE> citPool(11 * 0x800, 0);
    std::vector<BYTE*> citVec(11, nullptr);
    auto initCit = [&](int idx, BYTE* home, BYTE* parent, float age, float edu, float elec,
                       bool hasCar, bool dormFlag, int infant, int unborn, float xPos, float lifespan) {
        BYTE* c = citPool.data() + (size_t)idx * 0x800;
        citVec[(size_t)idx] = c;
        *(BYTE**)(c + CIT_HOME_BUILDING_OFFSET) = home;
        *(BYTE**)(c + CIT_PARENT_OFFSET) = parent;
        *(float*)(c + CIT_AGE_OFFSET) = age;
        *(float*)(c + 0xA8) = edu;
        *(float*)(c + 0xFC) = elec;
        *(uintptr_t*)(c + 0x688) = hasCar ? 1ULL : 0ULL;
        *(BYTE*)(c + 0x2D) = dormFlag ? 1 : 0;
        *(int32_t*)(c + CIT_INFANT_FLAG_OFFSET) = infant;
        *(int32_t*)(c + CIT_UNBORN_FLAG_OFFSET) = unborn;
        *(float*)(c + CIT_POS_OFFSET) = xPos;
        *(float*)(c + 0x18) = lifespan;
        for (int s = 0; s < 10; ++s) *(float*)(c + 0xD8 + (size_t)s * 4) = 0.7f + 0.01f * (float)idx;
    };
    initCit(0, homeW.data(), nullptr, 30.0f, 1.5f, 2.5f, true, false, 0, 0, -800.0f, 75.0f);  // West adult
    initCit(1, homeE.data(), nullptr, 28.0f, 2.5f, 3.5f, false, false, 0, 0, 800.0f, 78.0f);  // East adult
    initCit(2, homeE.data(), citVec[1], 4.0f, 0.5f, 0.0f, false, false, 0, 0, 800.0f, 70.0f); // East young child
    initCit(3, homeE.data(), citVec[1], 14.0f, 1.2f, 0.0f, false, false, 0, 0, 800.0f, 70.0f);// East teen
    initCit(4, homeE.data(), citVec[1], 24.0f, 2.2f, 0.0f, false, false, 0, 0, 800.0f, 70.0f);// East dependent adult
    initCit(5, dormE.data(), nullptr, 22.0f, 1.5f, 1.5f, false, true, 0, 0, 900.0f, 72.0f);   // Dorm student (skipped demo/pop, kept in localCits)
    initCit(6, prisonE.data(), nullptr, 35.0f, 0.5f, 0.5f, false, true, 0, 0, 950.0f, 68.0f); // Prison inmate (skipped demo/pop)
    initCit(7, homeE.data(), nullptr, 1.0f, 0.0f, 0.0f, false, false, 1, 0, 800.0f, 70.0f);   // Infant (skipped demo/pop)
    initCit(8, homeE.data(), nullptr, 0.0f, 0.0f, 0.0f, false, false, 0, 1, 800.0f, 70.0f);   // Unborn (skipped demo/pop)
    initCit(9, nullptr, nullptr, 40.0f, 1.5f, 1.5f, false, false, 0, 0, 650.0f, 77.5f);       // Unhoused East (counted in localPop & localCits, NOT demo!)
    initCit(10, homeW.data(), citVec[1], 10.0f, 1.5f, 0.0f, false, false, 0, 0, -800.0f, 71.0f); // Split-home: cit->home=West, parent->home=East!
    WriteMockVectorHeader(world + WORLD_CITIZENS_OFFSET, citVec);

    // Populate 365 mock history records (with 0 citizenAverages[1] to test one-shot backfill)
    std::vector<BYTE> histBuf(365 * STAT_RECORD_SIZE, 0);
    for (int h = 0; h < 365; ++h) {
        BYTE* rec = histBuf.data() + (size_t)h * STAT_RECORD_SIZE;
        *(int32_t*)(rec + 0) = h + 1;
        *(int32_t*)(rec + 4) = 1970;
        *(int32_t*)(rec + STAT_CITIZEN_DEMOGRAPHICS_OFF + 12) = 10 + (h % 5);
    }
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET)      = histBuf.data();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 8)  = histBuf.data() + histBuf.size();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 16) = histBuf.data() + histBuf.size();

    // Join as Client (localState = 2)
    double initRub[2] = { 0.0, 500000.0 };
    double initUsd[2] = { 50000.0, 50000.0 };
    TwoStates_OnSessionJoined(false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_SetEngineResearchUnlockByteForTest(1, 0, 0);
    TwoStates_SetEngineResearchUnlockByteForTest(2, 0, 1);
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) = 1; // Client enters tick with local State 2 mounted

    // 1. Single-pass ComputeBothStatesCitizenDemographics parity vs 2x ComputeStateCitizenDemographics
    int popS1 = 0, popS2 = 0;
    float satS1 = 0.0f, satS2 = 0.0f;
    bool exactBoth = false;
    TwoStates_ComputeDemographicsParityForTest(world, &popS1, &popS2, &satS1, &satS2, &exactBoth);
    TEST_CHECK(exactBoth);
    TEST_CHECK(popS1 == 1);
    TEST_CHECK(popS2 == 5); // cits 1, 2, 3, 4, plus split-home cit 10 (parent->home=East)!

    // 2. Early/Late sub-step split
    TwoStates_BeforeSubStep(world, true);
    TEST_CHECK(TwoStates_IsInSimStepForTest() == true);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) == 0); // State 1 mounted

    TwoStates_AfterSubStep(world, true);
    TEST_CHECK(TwoStates_IsInSimStepForTest() == false); // ALWAYS false between Early and Late
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) == 0); // Early skipped research flip-back

    // West building had BLD_PAY_RUB_OFFSET=1 with g_stateRub[0]==0 -> Late BeforeSubStep clears it
    conW[BLD_PAY_RUB_OFFSET] = 1;
    TwoStates_BeforeSubStep(world, false);
    TEST_CHECK(TwoStates_IsInSimStepForTest() == true);
    TEST_CHECK(conW[BLD_PAY_RUB_OFFSET] == 0);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) == 0); // State 2 NOT clobbered

    TwoStates_AfterSubStep(world, false);
    TEST_CHECK(TwoStates_IsInSimStepForTest() == false);
    TEST_CHECK(*(const uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 0) == 1); // State 2 restored

    // 3. UI Citizen Cache, 3 Predicates, One-Shot Backfill & 365-Record History SEH Coalescing
    Determinism_SetActiveForTest(true);
    Determinism_SetSessionRunningForTest(true);
    Determinism_SetStepCountForTest(10);

    uint64_t uiBuilds0 = TwoStates_GetUiCitizenCacheBuildCountForTest();
    uint64_t bf0 = TwoStates_GetHistBackfillScanCountForTest();

    TwoStates_UiBegin();
    uint64_t uiBuilds1 = TwoStates_GetUiCitizenCacheBuildCountForTest();
    uint64_t bf1 = TwoStates_GetHistBackfillScanCountForTest();
    TEST_CHECK(uiBuilds1 == uiBuilds0 + 1);
    TEST_CHECK(bf1 == bf0 + 1); // backfill ran once

    // Check swapped UI citizen vector: cit 1,2,3,4,5,6,7,8,9 (East, including unhoused 9, excluding 0 & 10 whose cit->home=West!)
    BYTE** uiCitB = *(BYTE***)(world + WORLD_CITIZENS_OFFSET);
    BYTE** uiCitE = *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8);
    TEST_CHECK((uiCitE - uiCitB) == 9);
    // Check HUD pop: cits 1,2,3,4 + unhoused 9 = 5 (excludes dorm 5, prison 6, infant 7, unborn 8, split-home cit 10)
    TEST_CHECK(*(const int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET) == 5);
    // Check demo in WORLD_STAT_CURRENT_OFFSET: cits 1,2,3,4,10 = 5 adults+children
    TEST_CHECK(*(const int32_t*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_DEMOGRAPHICS_OFF + 12) == 1); // 1 adult (cit 1)

    // Mutate unhoused East citizen lifespan during UI -> UiEnd must restore it to 77.5f
    *(float*)(citVec[9] + 0x18) = 12.0f;
    // Verify 365th history record was swapped to State 2's backfilled record
    TEST_CHECK(*(const float*)(histBuf.data() + 364 * STAT_RECORD_SIZE + STAT_CITIZEN_AVERAGES_OFF + 4) > 0.0f);
    TwoStates_UiEnd();
    TEST_CHECK_NEAR(*(const float*)(citVec[9] + 0x18), 77.5f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(histBuf.data() + 364 * STAT_RECORD_SIZE + STAT_CITIZEN_AVERAGES_OFF + 4), 0.0f, 1e-6f);

    // Held frame (same step 10): UiBegin hits UI citizen cache (build count stays constant) & backfill skips,
    // yet Save/RestoreUiCitizenLifespans MUST still run!
    TwoStates_UiBegin();
    TEST_CHECK(TwoStates_GetUiCitizenCacheBuildCountForTest() == uiBuilds1);
    TEST_CHECK(TwoStates_GetHistBackfillScanCountForTest() == bf1);
    uiCitB = *(BYTE***)(world + WORLD_CITIZENS_OFFSET);
    uiCitE = *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8);
    TEST_CHECK((uiCitE - uiCitB) == 9);
    *(float*)(citVec[9] + 0x18) = 9.0f;
    TwoStates_UiEnd();
    TEST_CHECK_NEAR(*(const float*)(citVec[9] + 0x18), 77.5f, 1e-4f);

    // Bump StepCount -> cache misses and rebuilds, backfill still skipped!
    Determinism_SetStepCountForTest(11);
    TwoStates_UiBegin();
    TEST_CHECK(TwoStates_GetUiCitizenCacheBuildCountForTest() == uiBuilds1 + 1);
    TEST_CHECK(TwoStates_GetHistBackfillScanCountForTest() == bf1);
    TwoStates_UiEnd();

    // Bump g_vehOwnerMapGen (via TwoStates_RegisterVehicleOwner) on same step -> UiCitizenCache misses and rebuilds
    TwoStates_RegisterVehicleOwner(99001, 2);
    TwoStates_UiBegin();
    TEST_CHECK(TwoStates_GetUiCitizenCacheBuildCountForTest() == uiBuilds1 + 2);
    TwoStates_UiEnd();

    // Waiting Lobby / !SessionRunning bypasses held-frame cache
    Determinism_SetSessionRunningForTest(false);
    uint64_t uiBuildsBeforeLobby = TwoStates_GetUiCitizenCacheBuildCountForTest();
    TwoStates_UiBegin();
    TwoStates_UiEnd();
    TwoStates_UiBegin();
    TwoStates_UiEnd();
    TEST_CHECK(TwoStates_GetUiCitizenCacheBuildCountForTest() == uiBuildsBeforeLobby + 2);

    Determinism_SetStepCountForTest(0);
    Determinism_SetActiveForTest(false);
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    ResetMockWorldMemory();
    return true;
}

bool TestPerf_InPlaceEconAttributionAndNestedVehicleOverride()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    Determinism_SetActiveForTest(false);
    Determinism_SetSessionRunningForTest(false);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> bldW(0x1420, 0), bldE(0x1420, 0);
    *(int*)bldW.data() = 101; *(float*)(bldW.data() + OBJ_POS_OFFSET) = -500.0f;
    *(int*)bldE.data() = 102; *(float*)(bldE.data() + OBJ_POS_OFFSET) =  500.0f;
    std::vector<BYTE*> bldVec = { bldW.data(), bldE.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    std::vector<BYTE> vehW(VEHICLE_STRUCT_BYTES, 0), vehE(VEHICLE_STRUCT_BYTES, 0), vehW2(VEHICLE_STRUCT_BYTES, 0);
    *(int*)(vehW.data() + VEH_ID_OFFSET) = 1; *(BYTE**)(vehW.data() + VEH_HOME_OFFSET) = bldW.data();
    *(int*)(vehE.data() + VEH_ID_OFFSET) = 2; *(BYTE**)(vehE.data() + VEH_HOME_OFFSET) = bldE.data();
    *(int*)(vehW2.data() + VEH_ID_OFFSET) = 3; *(BYTE**)(vehW2.data() + VEH_HOME_OFFSET) = bldW.data();

    std::vector<BYTE> citE(0x800, 0);
    *(BYTE**)(citE.data() + CIT_HOME_BUILDING_OFFSET) = bldE.data();
    std::vector<BYTE*> citVec = { citE.data() };
    WriteMockVectorHeader(world + WORLD_CITIZENS_OFFSET, citVec);

    double initRub[2] = { 10000.0, 10000.0 };
    double initUsd[2] = { 5000.0, 5000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // 1. Zero-delta flush & no-change attribution
    TwoStates_BeforeStep(world);
    TwoStates_FlushUnattributedScalarsForTest(world);
    TwoStates_AfterStep(world);
    float s1Veh[4] = {}, s2Veh[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, s1Veh);
    TwoStates_GetStateEconFloat4ForTest(2, false, s2Veh);
    TEST_CHECK_NEAR(s1Veh[0], 0.0f, 1e-6f);
    TEST_CHECK_NEAR(s2Veh[0], 0.0f, 1e-6f);

    // 2. Nested VehicleEconOverrideScope (Outer=1 -> Mid=2 -> Inner=1 -> Same=1 -> unwind)
    TwoStates_BeforeStep(world);
    TwoStates_NestedVehicleOverrideForTest(world, bldW.data(), vehE.data(), vehW.data(), vehW2.data());
    TwoStates_AfterStep(world);

    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 10000.0 - 150.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 10000.0 - 200.0, 1e-4);
    TwoStates_GetStateEconFloat4ForTest(1, false, s1Veh);
    TwoStates_GetStateEconFloat4ForTest(2, false, s2Veh);
    TEST_CHECK_NEAR(s1Veh[1], 30.0f, 1e-4f);  // Inner same-owner (State 1)
    TEST_CHECK_NEAR(s2Veh[0], 50.0f, 1e-4f);  // Mid (State 2)
    TEST_CHECK_NEAR(s2Veh[2], 15.0f, 1e-4f);  // Mid after Inner unwind (State 2)
    TEST_CHECK_NEAR(TwoStates_GetStateEconTourismFloatForTest(1, 0), 10.0f, 1e-4f); // Outer (State 1)
    TEST_CHECK_NEAR(TwoStates_GetStateEconTourismFloatForTest(1, 1), 25.0f, 1e-4f); // Inner (State 1)
    TEST_CHECK_NEAR(TwoStates_GetStateEconTourismFloatForTest(1, 2),  5.0f, 1e-4f); // Outer after unwind (State 1)

    // 3. Direct H_CitizenSevereIllnessTick fast-path + zero-delta after step
    TwoStates_BeforeStep(world);
    TwoStates_CitizenSevereIllnessTickForTest(world, 0.0f, citE.data());
    StateTrackerSlice tr2 = {}, tr1 = {};
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(2, false, &tr2));
    TEST_CHECK(tr2.healthCnt[9] == 0);
    TwoStates_CitizenSevereIllnessTickForTest(world, 2.5f, citE.data());
    TwoStates_AfterStep(world);
    TEST_CHECK_NEAR(*(const float*)(world + WORLD_TRACKER_HEALTH_CUR_SUM_OFF + 36), 2.5f, 1e-5f);
    TEST_CHECK(*(const int32_t*)(world + WORLD_TRACKER_HEALTH_CUR_CNT_OFF + 36) == 1);
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(2, false, &tr2));
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(1, false, &tr1));
    TEST_CHECK_NEAR(tr2.healthSum[9], 2.5f, 1e-5f);
    TEST_CHECK(tr2.healthCnt[9] == 1);
    TEST_CHECK(tr1.healthCnt[9] == 0);

    // 4. thread_local scratch reuse across consecutive H_LedgerAdd & ConstructEconScope batches
    TwoStates_BeforeStep(world);
    SovietStatResEntry row1 = { 0x1111ULL, 3.0f, 120.0f };
    SovietStatResEntry row2 = { 0x2222ULL, 5.0f, 250.0f };
    BYTE* vec0 = world + WORLD_STAT_CURRENT_OFFSET + STAT_RES_VEC_BASE_OFF;
    TwoStates_LedgerAddForTest(world, &row1, vec0, 1.0f);
    TwoStates_LedgerAddForTest(world, &row2, vec0, 1.0f);
    int vecIdxs[2] = { 8, 9 };
    SovietStatResEntry cEntries[2] = { { 0x3333ULL, 4.0f, 80.0f }, { 0x4444ULL, 6.0f, 90.0f } };
    TwoStates_ConstructStepBatchForTest(world, bldE.data(), cEntries, vecIdxs, 2);
    TwoStates_ConstructStepBatchForTest(world, bldE.data(), cEntries, vecIdxs, 2);
    TwoStates_AfterStep(world);

    float tons = 0.0f, val = 0.0f;
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 8, 0x3333ULL, &tons, &val));
    TEST_CHECK_NEAR(tons, 8.0f, 1e-4f);
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 9, 0x4444ULL, &tons, &val));
    TEST_CHECK_NEAR(tons, 12.0f, 1e-4f);

    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    ResetMockWorldMemory();
    return true;
}

bool TestPerf_ResearchAndVehLicenseMountCache()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Heap-allocate 4 vehicle types with distinct script names
    std::vector<BYTE> vehTypes(4 * WORLD_VEHICLE_TYPE_STRIDE, 0);
    const char* names[4] = { "skoda_706", "tatra_138", "kmz_5320", "bus_karosa" };
    for (int i = 0; i < 4; ++i) {
        BYTE* vt = vehTypes.data() + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE;
        strncpy_s((char*)(vt + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, names[i], _TRUNCATE);
        *(vt + VEH_TYPE_HAS_LICENCE_OFFSET) = 0;
    }
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET)      = vehTypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8)  = vehTypes.data() + vehTypes.size();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 16) = vehTypes.data() + vehTypes.size();

    // 16 research items
    std::vector<BYTE> resBuf(16 * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < 16; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET] = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET)   = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)     = resBuf.data() + resBuf.size();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET + 8) = resBuf.data() + resBuf.size();

    std::vector<BYTE> univWest(0x1420, 0), univEast(0x1420, 0);
    *(int*)univWest.data() = 601; *(float*)(univWest.data() + OBJ_POS_OFFSET) = -800.0f;
    *(int*)univEast.data() = 602; *(float*)(univEast.data() + OBJ_POS_OFFSET) =  800.0f;
    std::vector<BYTE*> bldVec = { univWest.data(), univEast.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 50000.0, 50000.0 };
    TwoStates_OnSessionJoined(false /*Client = State 2*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    Determinism_SetSessionRunningForTest(true);

    // 1. Vehicle license mask equivalence skip (both states identical -> 0 writes between State 1 and State 2!)
    TwoStates_SetVehicleLicenseOwnedForTest(world, 1, 0, true);
    TwoStates_SetVehicleLicenseOwnedForTest(world, 2, 0, true);
    uint64_t licApplies0 = TwoStates_GetVehLicenseMountApplyCountForTest();
    TwoStates_BeforeSubStep(world, true);
    TwoStates_AfterSubStep(world, true);
    TwoStates_BeforeSubStep(world, false);
    TwoStates_AfterSubStep(world, false);
    // Identical masks skip writing on both Early mount and Late restore!
    TEST_CHECK(TwoStates_GetVehLicenseMountApplyCountForTest() == licApplies0);

    // Give State 2 license 1 -> masks differ -> mounts apply when switching State 1 <-> State 2
    TwoStates_SetVehicleLicenseOwnedForTest(world, 2, 1, true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 1) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 1) == true);
    uint64_t licApplies1 = TwoStates_GetVehLicenseMountApplyCountForTest();
    TwoStates_BeforeSubStep(world, true);
    TEST_CHECK(*(const uint8_t*)(vehTypes.data() + 1 * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_HAS_LICENCE_OFFSET) == 0);
    TwoStates_BeforeSubStep(world, false); // State 1 already mounted -> O(1) skip
    uint64_t licAppliesAfterLateMount = TwoStates_GetVehLicenseMountApplyCountForTest();
    TEST_CHECK(licAppliesAfterLateMount == licApplies1 + 1);
    TwoStates_AfterSubStep(world, false);
    TEST_CHECK(*(const uint8_t*)(vehTypes.data() + 1 * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_HAS_LICENCE_OFFSET) == 1);

    // Move vehicle types buffer + OnWorldLoaded -> key cache refreshes
    std::vector<BYTE> vehTypes2 = vehTypes;
    *(vehTypes2.data() + 2 * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_HAS_LICENCE_OFFSET) = 1;
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET)      = vehTypes2.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8)  = vehTypes2.data() + vehTypes2.size();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 16) = vehTypes2.data() + vehTypes2.size();
    TwoStates_OnWorldLoaded();
    TwoStates_NotifyVehicleLicenseChanged(world);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 2) == true);

    // 2. Research mount cache + hasUnresolvedBldPtr refusal + all invalidation paths
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univEast.data(), 2, 3));
    TwoStates_BldUnivStepForTest(world, univEast.data());
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univEast.data(), 2) == 3);

    // Host (State 1) steady-state step should skip redundant research writes on subsequent ticks
    TwoStates_OnSessionJoined(true /*Host = State 1*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_BeforeSubStep(world, true);
    TwoStates_AfterSubStep(world, true);
    TwoStates_BeforeSubStep(world, false);
    TwoStates_AfterSubStep(world, false);
    uint64_t resAppliesSteady0 = TwoStates_GetResearchMountApplyCountForTest();
    TwoStates_BeforeSubStep(world, true);
    TwoStates_AfterSubStep(world, true);
    TwoStates_BeforeSubStep(world, false);
    TwoStates_AfterSubStep(world, false);
    TEST_CHECK(TwoStates_GetResearchMountApplyCountForTest() == resAppliesSteady0);

    // Verify invalidation points reset mountedResearchState to 0
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 1);
    TEST_CHECK(TwoStates_GetResearchMountGenForTest() > 0);
    TwoStates_OnWorldLoaded();
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 0);

    TwoStates_BeforeSubStep(world, false);
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 1);
    TwoStates_BeginCommand(1);
    TwoStates_EndCommand(1);
    // EndCommand leaves localSt (1) mounted, having invalidated before Capture/Write
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 1);

    TwoStates_MasterSaveForTest(world);
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 1);

    uint64_t resBeforeDaily = TwoStates_GetResearchMountApplyCountForTest();
    TwoStates_ResearchDailyUpdateForTest(world);
    TEST_CHECK(TwoStates_GetResearchMountApplyCountForTest() > resBeforeDaily);

    uint64_t resBeforePanel = TwoStates_GetResearchMountApplyCountForTest();
    TwoStates_UnivResearchPanelForTest(world, univWest.data(), 1, 0.5f);
    TEST_CHECK(TwoStates_GetResearchMountApplyCountForTest() > resBeforePanel);

    TwoStates_SetEngineResearchUnlockByteForTest(1, 2, 1);
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 0);

    // Save meta and reload with empty WORLD_BUILDINGS_OFFSET -> hasUnresolvedBldPtr refuses skip until buildings exist
    TwoStates_OnSessionJoined(false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_res_mount_cache";
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir.string().c_str()));

    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 0);
    std::vector<BYTE*> emptyBlds;
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, emptyBlds);
    TwoStates_OnSessionJoined(false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir.string().c_str(), true));

    // While buildings are empty, State 2's univEast pointer is unresolved -> mount state stays 0 (refuses skip)
    TwoStates_BeforeSubStep(world, true);
    TwoStates_AfterSubStep(world, false);
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 0);

    // Restore buildings vector -> next mount resolves univEast and marks mount state valid (2)!
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);
    TwoStates_BeforeSubStep(world, true);
    TwoStates_AfterSubStep(world, false);
    TEST_CHECK(TwoStates_GetResearchMountStateForTest() == 2);
    BYTE* worldBldPtr = *(BYTE**)(resBuf.data() + 3 * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_BUILDING_PTR_OFFSET);
    TEST_CHECK(worldBldPtr == univEast.data());

    std::filesystem::remove_all(saveDir, ec);
    Determinism_SetSessionRunningForTest(false);
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    ResetMockWorldMemory();
    return true;
}

static void __fastcall MockCitComputeLifespan(BYTE* /*world*/, BYTE* cit)
{
    if (cit && *(const float*)(cit + 0x18) <= 0.0f) {
        *(float*)(cit + 0x18) = 74.5f;
    }
}

bool TestPerf_M6TsUiAndHeldFrameOptimizations()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    Determinism_SetActiveForTest(false);
    Determinism_SetSessionRunningForTest(false);
    Determinism_SetStepCountForTest(0);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> bldW(0x1420, 0), bldE(0x1420, 0);
    *(int*)bldW.data() = 901; *(float*)(bldW.data() + OBJ_POS_OFFSET) = -700.0f;
    *(int*)bldE.data() = 902; *(float*)(bldE.data() + OBJ_POS_OFFSET) =  700.0f;
    std::vector<BYTE*> bldVec = { bldW.data(), bldE.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    std::vector<BYTE> citPool(4 * 0x800, 0);
    std::vector<BYTE*> citVec(4, nullptr);
    for (int i = 0; i < 4; ++i) {
        BYTE* c = citPool.data() + (size_t)i * 0x800;
        citVec[(size_t)i] = c;
        *(BYTE**)(c + CIT_HOME_BUILDING_OFFSET) = (i < 2) ? bldW.data() : bldE.data();
        *(float*)(c + CIT_AGE_OFFSET) = 25.0f;
        *(float*)(c + 0xA8) = 1.5f;
        *(float*)(c + 0xFC) = 2.0f;
        *(float*)(c + 0x18) = (i == 2) ? 0.0f : 78.0f;
        for (int s = 0; s < 10; ++s) *(float*)(c + 0xD8 + (size_t)s * 4) = 0.8f;
    }
    WriteMockVectorHeader(world + WORLD_CITIZENS_OFFSET, citVec);

    // 10 history records: rec[0] identical between world and State 2, rec[1] differs in tourism[0], rec[2] has legacy 0 lifespan avg
    std::vector<BYTE> histBuf(10 * STAT_RECORD_SIZE, 0);
    for (int h = 0; h < 10; ++h) {
        BYTE* rec = histBuf.data() + (size_t)h * STAT_RECORD_SIZE;
        *(int32_t*)(rec + 0) = h + 1;
        *(int32_t*)(rec + 4) = 1971;
        *(int32_t*)(rec + STAT_CITIZEN_DEMOGRAPHICS_OFF + 12) = 2;
        if (h != 2) {
            *(float*)(rec + STAT_CITIZEN_AVERAGES_OFF + 4) = 78.0f;
        }
        if (h == 1) {
            *(float*)(rec + STAT_TOURISM_OFF) = 100.0f;
        }
    }
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET)      = histBuf.data();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 8)  = histBuf.data() + histBuf.size();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 16) = histBuf.data() + histBuf.size();

    // Loan capacity buffer for 0->1 borrow test
    std::vector<SovietLoan> loanCapBuf(4);
    *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 0x00) = loanCapBuf.data();
    *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 0x08) = loanCapBuf.data();
    *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 0x10) = loanCapBuf.data() + loanCapBuf.size();
    *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = -1;

    double initRub[2] = { 100000.0, 200000.0 };
    double initUsd[2] = { 10000.0, 20000.0 };
    TwoStates_OnSessionJoined(false /*Client = State 2*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(2) == 10);

    // 1. Rank #1: H_CitComputeLifespan dirty tracking, null safety, idempotence, and SaveUiCitizenLifespans bypass
    TwoStates_SetCitComputeLifespanTestHook(&MockCitComputeLifespan);
    TwoStates_UiBegin();
    TEST_CHECK(TwoStates_GetUiSavedLifespanCountForTest() == 0); // full-vector save bypassed when hooked
    TwoStates_InvokeCitComputeLifespanHookForTest(world, nullptr); // safe no-op on null cit
    TwoStates_InvokeCitComputeLifespanHookForTest(world, citVec[2]);
    TwoStates_InvokeCitComputeLifespanHookForTest(world, citVec[2]); // idempotent duplicate call
    TEST_CHECK_NEAR(*(const float*)(citVec[2] + 0x18), 74.5f, 1e-4f);
    TEST_CHECK(TwoStates_GetUiDirtyLifespanCountForTest() == 1);
    TwoStates_UiEnd();
    TEST_CHECK_NEAR(*(const float*)(citVec[2] + 0x18), 0.0f, 1e-6f);
    TEST_CHECK_NEAR(*(const float*)(citVec[3] + 0x18), 78.0f, 1e-6f);
    TEST_CHECK(TwoStates_GetUiDirtyLifespanCountForTest() == 0);

    // 2. Rank #3: TwoStates_AfterSubStep(world, false) primes g_uiCitizenCache (0-build hit on first UiBegin!)
    Determinism_SetActiveForTest(true);
    Determinism_SetSessionRunningForTest(true);
    Determinism_SetStepCountForTest(42);
    TwoStates_BeforeSubStep(world, false);
    TwoStates_AfterSubStep(world, false);
    const uint64_t buildsBeforeFirstUi = TwoStates_GetUiCitizenCacheBuildCountForTest();
    TwoStates_UiBegin();
    TEST_CHECK(TwoStates_GetUiCitizenCacheBuildCountForTest() == buildsBeforeFirstUi); // 100% cache HIT!
    BYTE** uiCitB = *(BYTE***)(world + WORLD_CITIZENS_OFFSET);
    BYTE** uiCitE = *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8);
    TEST_CHECK((uiCitE - uiCitB) == 2); // cit 2, 3
    TEST_CHECK(*(const int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET) == 2);
    TwoStates_UiEnd();

    // Invalidate primed cache via ownerMapGen bump -> next UiBegin misses and rebuilds
    TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(700.0f, 0.0f), 2);
    TwoStates_UiBegin();
    TEST_CHECK(TwoStates_GetUiCitizenCacheBuildCountForTest() == buildsBeforeFirstUi + 1);
    TwoStates_UiEnd();

    // 3. Rank #2: History prepared/backup cache & dirty-mask parity + gen invalidation
    uint64_t gen0 = TwoStates_GetHistSwapGenForTest();
    TwoStates_SetStateEconExtraCountForTest(2, 0, 0); // ensure init
    // Mutate histBuf[1] vs State 2 via denomination or rollover check; first verify UiBegin/UiEnd restores histBuf[1]
    *(float*)(histBuf.data() + 1 * STAT_RECORD_SIZE + STAT_TOURISM_OFF) = 100.0f;
    TwoStates_UiBegin();
    const float swappedVal1 = *(const float*)(histBuf.data() + 1 * STAT_RECORD_SIZE + STAT_TOURISM_OFF);
    TEST_CHECK(fabsf(swappedVal1 - 100.0f) > 1.0f); // swapped to State 2's share
    TwoStates_UiEnd();
    TEST_CHECK_NEAR(*(const float*)(histBuf.data() + 1 * STAT_RECORD_SIZE + STAT_TOURISM_OFF), 100.0f, 1e-4f);

    // Second UiBegin hits g_uiSavedHistStats & g_uiPreparedHistSwap cache and still swaps/restores histBuf[1] cleanly
    TwoStates_UiBegin();
    TEST_CHECK_NEAR(*(const float*)(histBuf.data() + 1 * STAT_RECORD_SIZE + STAT_TOURISM_OFF), swappedVal1, 1e-4f);
    TwoStates_UiEnd();
    TEST_CHECK_NEAR(*(const float*)(histBuf.data() + 1 * STAT_RECORD_SIZE + STAT_TOURISM_OFF), 100.0f, 1e-4f);

    // StatRecordClear bumps histSwapGen
    TwoStates_StatRecordClearForTest(world, world + WORLD_STAT_CURRENT_OFFSET);
    TEST_CHECK(TwoStates_GetHistSwapGenForTest() > gen0);

    // 4. Rank #4: Coalesced zero-loan fast-path & borrow command detection
    TwoStates_UiBegin();
    TEST_CHECK(*(SovietLoan**)(world + WORLD_LOANS_OFFSET + 0x08) == loanCapBuf.data());
    TEST_CHECK(*(const int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) == -1);
    TwoStates_UiEnd();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 300000.0, 1e-4);

    // Borrow during UI -> coalesced UiEnd detects & applies LS_CMD_LOAN
    TwoStates_UiBegin();
    SovietLoan newLoan = {};
    newLoan.yearInterestRate = 5.0f;
    newLoan.loanType = LOAN_CURRENCY_RUB;
    newLoan.loanSubType = 1;
    newLoan.daysRemaining = 365;
    newLoan.remainingPrincipal = 50000.0f;
    newLoan.initialPrincipal = 50000.0f;
    newLoan.initialDays = 365;
    loanCapBuf[0] = newLoan;
    *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 0x08) = loanCapBuf.data() + 1;
    *(double*)(world + WORLD_RUB_OFFSET) += 50000.0;
    TwoStates_UiEnd();
    double rubDebt = 0.0, usdDebt = 0.0;
    int loanCnt = 0;
    TwoStates_GetLoanDebt(2, &rubDebt, &usdDebt, &loanCnt);
    TEST_CHECK(loanCnt == 1);
    TEST_CHECK_NEAR(rubDebt, 50000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 250000.0, 1e-2);

    TwoStates_SetCitComputeLifespanTestHook(nullptr);
    Determinism_SetSessionRunningForTest(false);
    Determinism_SetActiveForTest(false);
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    ResetMockWorldMemory();

    // 5. Phase 4: HeldFrame coalesced VehicleGuard / GetVehicleWheelCountUnsafe / merged building scan
    printf("    [Perf M6: HeldFrame Coalesced Guard, Wheel Unsafe & Merged Building Scan] ...\n"); fflush(stdout);
    return SubTest_M6_HeldFrameCoalescedGuardAndVisualParity();
}
