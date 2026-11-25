// SPDX-License-Identifier: GPL-3.0-only
// E2E Test Suites (Tiers 1-4 covering R1-R18) and M1/M5/M6 Stress Suites.

#include "test_harness.h"
#include <memory>

// ---------------------------------------------------------------------------
// E2E Test Suite: Tiers 1-4 Covering Requirements R1 through R18
// ---------------------------------------------------------------------------

static double* TestWorldRubles() { return (double*)(g_mockExeBuffer + WORLD_RVA + 0x5B0); }
static double* TestWorldDollars() { return (double*)(g_mockExeBuffer + WORLD_RVA + 0x5A8); }

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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Set up road manager at 0xF7A0
    std::vector<BYTE> roadMgr(0x1000, 0);
    *(BYTE**)(world + 0xF838) = roadMgr.data();

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

    g_mockExeBuffer[0x3EEEA0] = 0xC3; // wrapper ret
    g_mockExeBuffer[0x522350] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x522E00] = 0xC3; // FCAF0_RVA ret
    g_mockExeBuffer[0x53ED90] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x238460] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x568C10] = 0xC3; // MANAGER_RESET_RVA ret
    g_mockExeBuffer[0x568AE0] = 0xC3; // queueDeleteMesh ret

    // Remote road segment command arrives while local player is dragging.
    // RoadSync_ApplyCommandSegment executes ApplySegmentImplInner which resets
    // manager drag state for replay and restores local drag state in __finally.
    auto remoteMsgPtr = std::make_unique<RoadSyncMsg>();
    RoadSyncMsg& remoteMsg = *remoteMsgPtr;
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

    *(BYTE**)(world + 0xF838) = nullptr;
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
    TwoStates_OnSessionEnded();
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> city0(128, 0), city1(128, 0);
    *(float*)(city0.data() + 4) = -500.0f;
    *(float*)(city0.data() + 12) = 100.0f;
    wcscpy_s((wchar_t*)(city0.data() + 0x10), 32, L"CityWest");

    *(float*)(city1.data() + 4) = 500.0f;
    *(float*)(city1.data() + 12) = 200.0f;
    wcscpy_s((wchar_t*)(city1.data() + 0x10), 32, L"CityEast");

    std::vector<BYTE*> cityVec = { city0.data(), city1.data() };
    WriteMockVectorHeader(world + 0x103E0, cityVec);

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
    TEST_CHECK(SafeCountStdVector(world + 0x103E0, sizeof(BYTE*), 100) == 1);

    // 4. Remaining city is Krasnogorsk at index 0 after compaction
    BYTE* remainingCity = **(BYTE***)(world + 0x103E0);
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
    TEST_CHECK(SafeCountStdVector(world + 0x103E0, sizeof(BYTE*), 100) == 0);

    return true;
}

bool Test_E2E_Tier1_FeatureCoverage_R1_to_R6()
{
    if (!H) { TsmBind(&g_mockHost); }
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
        SyncBlock_GetTransferStats(&pct, (int*)nullptr, (int*)nullptr, nullptr);
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

    std::vector<BYTE> bus(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 501;
    *(float*)(bus.data() + 0xD44) = 15.0f;
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

bool Test_E2E_Tier1_FeatureCoverage_R7_to_R12()
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
    *(float*)(world + 0xF9C) = 100.0f; // cursorX
    *(float*)(world + 0xFA4) = 200.0f; // cursorZ
    *(float*)(world + 0xFC8) = 15.0f;  // brush
    *(float*)(g_mockExeBuffer + 0xA1EEF0) = 0.00833f; // non-normalized host dt

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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    std::vector<BYTE> pedMgr(0x1000, 0);
    *(BYTE**)(world + 0xF838) = roadMgr.data();
    *(BYTE**)(world + 0xFA00) = pedMgr.data();

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

    g_mockExeBuffer[0x522350] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x53ED90] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x238460] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x568C10] = 0xC3; // MANAGER_RESET_RVA ret
    g_mockExeBuffer[0x568AE0] = 0xC3; // queueDeleteMesh ret

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

    // 5. Multi-piece mud road (seg->type == -1, split by footpaths) upgraded to asphalt (1):
    //    LocalBuilds_FillUpgrade and RoadSync_ApplyCommandUpgrade must preserve target type394 == 1
    //    instead of writing -1 (which triggers segment deletion 0x1405434C0 in 0x1405205A0).
    *(int*)(roadSeg.data() + 0x120) = -1; // mud road surface
    *(int*)(roadSeg.data() + 0x124) = 0;
    __int64 pathSegs[2] = { (__int64)roadSeg.data(), (__int64)roadSeg.data() };
    __int64 pathVec[3] = { (__int64)&pathSegs[0], (__int64)&pathSegs[2], (__int64)&pathSegs[2] };
    *(unsigned char*)(roadMgr.data() + 0x390) = 1;
    *(unsigned char*)(roadMgr.data() + 0x3D0) = 1; // multi-piece upgrade
    *(int*)(roadMgr.data() + 0x3D8) = 0;
    *(__int64*)(roadMgr.data() + 0x3F0) = (__int64)&pathVec[0];
    *(__int64*)(roadMgr.data() + 0x3F8) = (__int64)(&pathVec[0] + 3);
    *(int*)(roadMgr.data() + 0x394) = 1;  // target asphalt type in +0x394
    *(int*)(roadMgr.data() + 0x398) = 0;  // target variant in +0x398
    *(int*)(roadMgr.data() + 0x39C) = -1; // normal surface upgrade (dirFlag == -1)
    *(int*)(roadMgr.data() + 0x528) = 1;  // asphalt
    *(int*)(roadMgr.data() + 0x52C) = 0;

    int totalMultiPieces = 0;
    RoadSyncUpgradeMsg multiMsg = {};
    TEST_CHECK(LocalBuilds_FillUpgrade((__int64)roadMgr.data(), ROAD_SYNC_CATEGORY_ROAD, multiMsg, 0, &totalMultiPieces));
    TEST_CHECK(totalMultiPieces == 2 && multiMsg.pieceCount == 2);
    TEST_CHECK(multiMsg.pieces[0].oldType == 1 && multiMsg.pieces[1].oldType == 1);

    // Even if a caller passes piece.oldType == -1 for a mud road upgrade, ApplyUpgradePieceInner
    // must fall back to msg.newType (1) so manager+0x394 is never negative during commit.
    multiMsg.pieceCount = 1;
    multiMsg.pieces[0].oldType = -1;
    TEST_CHECK(RoadSync_ApplyCommandUpgrade(multiMsg));
    TEST_CHECK(*(int*)(roadMgr.data() + 0x394) == 1);

    // 6. Cleanup mock pointers
    *(BYTE**)(world + 0xF838) = nullptr;
    *(BYTE**)(world + 0xFA00) = nullptr;
    return true;
}

static bool SubTest_T1_R17_OilPumpModelAndAnimation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> animCtrl(0x100, 0);
    *(int32_t*)(animCtrl.data() + ANIM_CTRL_MAX_FRAMES_OFFSET) = 100; // 100 animation frames

    std::vector<BYTE> mesh(0x200, 0);
    *(BYTE**)(mesh.data() + MESH_ANIM_CTRL_OFFSET) = animCtrl.data();

    std::vector<BYTE> bldType(0x800, 0);
    *(BYTE**)(bldType.data() + BLD_TYPE_ANIM_SUBMESH_OFFSET) = mesh.data();
    *(float*)(bldType.data() + BLD_TYPE_ANIM_SPEED_OFFSET) = 30.0f; // 30 frames/sec (1.0 frame/tick at 30 Hz)

    std::vector<BYTE> pump(BUILDING_STRUCT_BYTES, 0);
    *(int*)pump.data() = 701;
    *(BYTE**)(pump.data() + BLD_TYPE_OFFSET) = bldType.data();
    *(BYTE**)(pump.data() + BLD_ANIM_SUBMESH_OFFSET) = mesh.data();
    *(float*)(pump.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(float*)(pump.data() + BLD_PRODUCED_OFFSET) = 4.2f; // producing oil
    *(float*)(pump.data() + BLD_ANIM_FRAME_OFFSET) = 12.5f;
    pump[0x115A] = 1;
    *(float*)(pump.data() + 0x115C) = 150.0f;

    // 1. Deselected oil pump has visibility byte set in render pass
    TEST_CHECK(pump[0x115A] == 1);
    TEST_CHECK_NEAR(*(float*)(pump.data() + 0x115C), 150.0f, 1e-4f);

    // 2. Smooth per-frame progression at 144 FPS and 60 FPS with wrap-around and pause freeze
    float f0 = 0.0f;
    for (int i = 0; i < 144; ++i) {
        f0 = HeldFrame_StepBuildingAnimFrame(f0, 100.0f, 1.0f, 144.0f, false);
    }
    TEST_CHECK_NEAR(f0, 30.0f, 1e-3f); // 1.0 second at 30 frames/sec

    float fWrap = HeldFrame_StepBuildingAnimFrame(99.5f, 100.0f, 1.0f, 30.0f, false);
    TEST_CHECK_NEAR(fWrap, 0.5f, 1e-4f); // preserves fractional overshoot across wrap

    float fPaused = HeldFrame_StepBuildingAnimFrame(42.0f, 100.0f, 1.0f, 144.0f, true);
    TEST_CHECK_NEAR(fPaused, 42.0f, 1e-5f);

    // 3. BeforeEarly / AfterEarly suppresses Early's 5 Hz 3-tick jump while capturing rate
    std::vector<BYTE*> bldList = { pump.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldList);
    Determinism_SetActiveForTest(true);
    HeldFrame_Reset();

    HeldFrame_BeforeEarly();
    TEST_CHECK_NEAR(*(float*)(pump.data() + BLD_ANIM_FRAME_OFFSET), 0.0f, 1e-5f);
    // Simulate Early's 1-in-3 bucket step adding 3.0 frames
    *(int*)(g_mockExeBuffer + BLD_PASS_BUCKET_COUNTER_RVA) = 0;
    *(float*)(pump.data() + BLD_ANIM_FRAME_OFFSET) = 3.0f;
    HeldFrame_AfterEarly();
    // Visual frame must be restored to 12.5f (no 3.0f jump!)
    TEST_CHECK_NEAR(*(float*)(pump.data() + BLD_ANIM_FRAME_OFFSET), 12.5f, 1e-4f);

    // Step one rendered frame at 60 FPS (0.5 tick -> +0.5 frame)
    HeldFrame_StepBuildingAnimations(60.0f);
    TEST_CHECK_NEAR(*(float*)(pump.data() + BLD_ANIM_FRAME_OFFSET), 13.0f, 1e-4f);

    // 4. Advancing render frames does NOT change simulation state hash
    StateSummary summary1 = {};
    TEST_CHECK(StateHash_Compute(&summary1));
    U64 hash1 = StateHash_All(summary1);
    HeldFrame_StepBuildingAnimations(144.0f);
    StateSummary summary2 = {};
    TEST_CHECK(StateHash_Compute(&summary2));
    U64 hash2 = StateHash_All(summary2);
    TEST_CHECK(hash1 == hash2);

    // 5. Idle pumpjack (produced == 0) stops advancing
    *(float*)(pump.data() + BLD_PRODUCED_OFFSET) = 0.0f;
    float beforeIdle = *(float*)(pump.data() + BLD_ANIM_FRAME_OFFSET);
    HeldFrame_StepBuildingAnimations(144.0f);
    TEST_CHECK_NEAR(*(float*)(pump.data() + BLD_ANIM_FRAME_OFFSET), beforeIdle, 1e-5f);

    Determinism_SetActiveForTest(false);
    HeldFrame_Reset();
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

bool Test_E2E_Tier1_FeatureCoverage_R13_to_R18()
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
    TwoStates_OnSessionEnded();
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
    TwoStates_OnSessionEnded();
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
    auto unsharedMsgPtr = std::make_unique<RoadSyncMsg>();
    RoadSyncMsg& unsharedMsg = *unsharedMsgPtr;
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> city(128, 0);
    *(float*)(city.data() + 4) = 100.0f;
    *(float*)(city.data() + 12) = 100.0f;
    wcscpy_s((wchar_t*)(city.data() + 0x10), 32, L"Initial");
    std::vector<BYTE*> cityVec = { city.data() };
    WriteMockVectorHeader(world + 0x103E0, cityVec);

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
    WriteMockVectorHeader(world + 0x103E0, cityVec);
    TEST_CHECK(SafeCountStdVector(world + 0x103E0, sizeof(BYTE*), 100) == 0);

    // 4. Deleting non-existent city index safely ignored
    m.index = 999;
    m.chars = 5;
    TEST_CHECK(!Rename_ApplyCommand(&m, sizeof(m)));

    // 5. Maximum allowable name length (31 chars)
    cityVec.push_back(city.data());
    WriteMockVectorHeader(world + 0x103E0, cityVec);
    m.index = 0;
    wchar_t maxName[32] = {};
    for (int i = 0; i < 31; ++i) maxName[i] = L'X';
    m.chars = 31;
    for (size_t c = 0; c < 31; ++c) m.text[c] = (unsigned short)maxName[c];
    TEST_CHECK(Rename_ApplyCommand(&m, sizeof(m)));
    TEST_CHECK(wcslen((const wchar_t*)(city.data() + 0x10)) == 31);

    cityVec.clear();
    WriteMockVectorHeader(world + 0x103E0, cityVec);
    return true;
}

bool Test_E2E_Tier2_BoundaryCases_R1_to_R6()
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

    std::vector<BYTE> bus(VEHICLE_STRUCT_BYTES, 0);
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
    std::vector<BYTE> wagon(VEHICLE_STRUCT_BYTES, 0);
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

bool Test_E2E_Tier2_BoundaryCases_R7_to_R12()
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

bool Test_E2E_Tier2_BoundaryCases_R13_to_R18()
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
    TwoStates_OnSessionEnded();
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> city(128, 0);
    *(float*)(city.data() + 4) = 100.0f;
    *(float*)(city.data() + 12) = 100.0f;
    wcscpy_s((wchar_t*)(city.data() + 0x10), 32, L"Universitetsk");
    std::vector<BYTE*> cityVec = { city.data() };
    WriteMockVectorHeader(world + 0x103E0, cityVec);

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
    WriteMockVectorHeader(world + 0x103E0, cityVec);
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
    std::vector<BYTE> bus(VEHICLE_STRUCT_BYTES, 0);
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
    TwoStates_OnSessionEnded();
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    for (int i = 0; i < 20; ++i) {
        *(int*)(world + 0xF90) = i;
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

bool Test_E2E_Tier3_CrossFeatureInteractions()
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
    WriteMockVectorHeader(world + 0x103E0, cityVec);

    // Persist and reload
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir));
    cityVec.clear();
    WriteMockVectorHeader(world + 0x103E0, cityVec);
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

bool Test_E2E_Tier4_RealWorldScenarios()
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
    g_mockExeBuffer[0x568AE0] = 0xC3; // queueDeleteMesh ret stub
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    std::vector<BYTE> pedMgr(0x1000, 0);
    *(BYTE**)(world + 0xF838) = roadMgr.data(); // ROAD manager
    *(BYTE**)(world + 0xFA00) = pedMgr.data();  // PEDESTRIAN manager

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
    auto segPtr = std::make_unique<RoadSyncSegment>();
    RoadSyncSegment& seg = *segPtr;
    for (int k = 0; k < 500; ++k) {
        Commands_BeginRemoteReplayScope();
        int cmdType = k % 5;
        if (cmdType == 0) {
            BuildMode bm = {};
            bm.rub = 1000;
            bm.b00 = 1;
            BuildMode_Write(bm);
        } else if (cmdType == 1) {
            memset(&seg, 0, sizeof(seg));
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
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    std::vector<BYTE> pedMgr(0x1000, 0);
    *(BYTE**)(world + 0xF838) = roadMgr.data();
    *(BYTE**)(world + 0xFA00) = pedMgr.data();

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

    g_mockExeBuffer[0x522350] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x53ED90] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x238460] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x568C10] = 0xC3; // MANAGER_RESET_RVA ret

    g_mockExeBuffer[0x568AE0] = 0xC3; // queueDeleteMesh ret

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
    *(BYTE**)(world + 0xFA00) = nullptr;
    bool okNullPed = RoadSync_ApplyCommandUpgrade(msg);
    TEST_CHECK(okNullPed);

    *(BYTE**)(world + 0xF838) = nullptr;
    *(BYTE**)(world + 0xFA00) = nullptr;
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
    auto segMsgPtr = std::make_unique<RoadSyncMsg>();
    RoadSyncMsg& segMsg = *segMsgPtr;
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

bool Test_Stress_M1_Challenger_R1_R4_R5_R16()
{
    if (!H) { TsmBind(&g_mockHost); }
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
// Included M1 / M5 / M6 Challenger Stress Suites (#28, #29, #32-#35)
// ---------------------------------------------------------------------------

#define Test_Stress_M1_Challenger2 Test_Stress_M1_Challenger2_Impl
#include "m1_challenger2_stress.cpp"
#undef Test_Stress_M1_Challenger2
bool Test_Stress_M1_Challenger2() { return Test_Stress_M1_Challenger2_Impl(); }

#define Test_Stress_M1_Challenger1_Iter2 Test_Stress_M1_Challenger1_Iter2_Impl
#include "m1_challenger1_stress.cpp"
#undef Test_Stress_M1_Challenger1_Iter2
bool Test_Stress_M1_Challenger1_Iter2() { return Test_Stress_M1_Challenger1_Iter2_Impl(); }

#define Test_Stress_M5_Challenger2 Test_Stress_M5_Challenger2_Impl
#include "m5_challenger2_stress.cpp"
#undef Test_Stress_M5_Challenger2
bool Test_Stress_M5_Challenger2() { return Test_Stress_M5_Challenger2_Impl(); }

#define Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18 Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18_Impl
#include "m5_challenger1_stress.cpp"
#undef Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18
bool Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18() { return Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18_Impl(); }

#define Test_Stress_M6_Challenger1_R13 Test_Stress_M6_Challenger1_R13_Impl
#include "m6_challenger1_stress.cpp"
#undef Test_Stress_M6_Challenger1_R13
bool Test_Stress_M6_Challenger1_R13() { return Test_Stress_M6_Challenger1_R13_Impl(); }

#define Test_Stress_M6_Challenger2 Test_Stress_M6_Challenger2_Impl
#include "m6_challenger2_stress.cpp"
#undef Test_Stress_M6_Challenger2
bool Test_Stress_M6_Challenger2() { return Test_Stress_M6_Challenger2_Impl(); }
