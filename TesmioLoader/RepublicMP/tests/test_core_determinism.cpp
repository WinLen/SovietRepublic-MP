// SPDX-License-Identifier: GPL-3.0-only
// Core & Determinism Test Suites (#1, #2, #3, #48, #49, #50).

#include "test_harness.h"
#include <atomic>
#include <limits>
#include <stdarg.h>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Test 1: RVA Invariants, POD SEH Memory Helpers & Thread-Safe InitOnce Locks
// ---------------------------------------------------------------------------

static bool SubTest_PowerHighAndLateToolStubRegressions()
{
    // 1. All 14 category wrappers are non-zero, pairwise distinct, and match SOVIET64.exe v1.2.0.9.
    static const unsigned int kExpectedWrappers[ROAD_SYNC_CATEGORY_COUNT] = {
        0x3EF150u, 0x3CFEC0u, 0x3D0310u, 0x3D3290u, 0x3F3790u, 0x3F5910u, 0x3D2880u,
        0x3D77E0u, 0x3D9F30u, 0x3CDC90u, 0x3EB000u, 0x3E8190u, 0x3F5ED0u, 0x3EE750u,
    };
    unsigned int seenWrappers[ROAD_SYNC_CATEGORY_COUNT] = {};
    for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; ++c) {
        const unsigned int rva = RoadSync_GetCategoryWrapperRvaForTest(c);
        TEST_CHECK(rva != 0u);
        TEST_CHECK(rva == kExpectedWrappers[c]);
        for (int j = 0; j < c; ++j) {
            TEST_CHECK(seenWrappers[j] != rva);
        }
        seenWrappers[c] = rva;
    }
    TEST_CHECK(RoadSync_GetCategoryWrapperRvaForTest(-1) == 0u);
    TEST_CHECK(RoadSync_GetCategoryWrapperRvaForTest(ROAD_SYNC_CATEGORY_COUNT) == 0u);

    // 2. kToolCalls table has zero duplicates and covers all 186 Late tool dispatch sites.
    TEST_CHECK(SimPatches_AllToolCallSitesUniqueForTest());
    TEST_CHECK(SimPatches_GetToolCallCountForTest() == 186u);

    static const unsigned kAddedToolSites[][2] = {
        { 0x3318ECu, 0x3EE750u }, { 0x331B04u, 0x3EF150u }, { 0x331C30u, 0x3EF150u }, { 0x33208Du, 0x43C3E0u },
        { 0x3321C9u, 0x3CFEC0u }, { 0x3321EFu, 0x3CFEC0u }, { 0x332322u, 0x3D0310u }, { 0x3325F1u, 0x3D77E0u },
        { 0x33263Fu, 0x3D77E0u }, { 0x332666u, 0x3D77E0u }, { 0x33268Du, 0x3D77E0u }, { 0x3326B4u, 0x3D77E0u },
        { 0x332727u, 0x3D77E0u }, { 0x3327C0u, 0x3D9F30u }, { 0x33285Au, 0x3D9F30u }, { 0x332DB7u, 0x1EF1D0u },
        { 0x332DD7u, 0x1F5AE0u }, { 0x333334u, 0x6168F0u },
    };
    for (const auto& pair : kAddedToolSites) {
        TEST_CHECK(SimPatches_HasToolCallForTest(pair[0], pair[1]));
    }
    return true;
}

static bool SubTest_PurchaseV1209CaptureRegressions()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 3 mock buildings: [0] depot/customs, [1] customsRub, [2] customsUsd
    std::vector<BYTE> depot(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> cRub(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> cUsd(BUILDING_STRUCT_BYTES, 0);
    *(int*)(depot.data() + 0) = 101;
    *(int*)(cRub.data() + 0)  = 102;
    *(int*)(cUsd.data() + 0)  = 103;
    std::vector<BYTE*> blds = { depot.data(), cRub.data(), cUsd.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, blds);

    // 2 vehicle types: [0] container (cat=9), [1] vanilla truck (cat=1)
    std::vector<BYTE> vtypes(2u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    BYTE* vtCont = vtypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vtTruck = vtypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE;
    strcpy_s((char*)(vtCont + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "vehicles/container_iso.ini");
    strcpy_s((char*)(vtTruck + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "vehicles/skoda_706_rth.ini");
    *(unsigned*)(vtCont + VEH_TYPE_CATEGORY_OFFSET) = 9u;
    *(unsigned*)(vtTruck + VEH_TYPE_CATEGORY_OFFSET) = 1u;
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = vtypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = vtypes.data() + vtypes.size();

    // Used vehicle market at world + 0x78 (cat 1 record = 0x28 bytes)
    std::vector<BYTE> market(0xC0, 0);
    std::vector<BYTE> usedRec(0x28, 0);
    *(BYTE**)(usedRec.data() + 0) = vtTruck;
    *(double*)(usedRec.data() + 8) = 4.25;
    *(double*)(usedRec.data() + 16) = 9.5;
    *(BYTE**)(market.data() + 0x00) = usedRec.data();
    *(BYTE**)(market.data() + 0x08) = usedRec.data() + usedRec.size();
    *(BYTE**)(world + 0x78) = market.data();

    // Container resource table at world + 0xC350 and lookup vector at world + 0x12928
    std::vector<BYTE> resPool(0x380, 0);
    *(BYTE**)(world + 0xC350) = resPool.data();
    *(BYTE**)(world + 0xC358) = resPool.data() + resPool.size();
    std::vector<BYTE*> rowPtrs = { resPool.data() };
    WriteMockVectorHeader(world + 0x12928, rowPtrs);

    // Root + child window linked through WINDOW_PARENT_OFFSET (+0x6C8)
    std::vector<BYTE> rootWin(0x700, 0);
    std::vector<BYTE> subWin(0x700, 0);
    *(BYTE**)(rootWin.data() + 0x2C0) = cRub.data();
    *(BYTE**)(rootWin.data() + 0x2C8) = cUsd.data();
    subWin[0x20] = 1; // hover
    *(unsigned*)(subWin.data() + 0x23C) = 1u; // RUB purchase
    *(BYTE**)(subWin.data() + 0x240) = depot.data();
    *(unsigned*)(subWin.data() + 0x248) = 14u; // USED_TARGET
    subWin[0x2B8] = 1; // refill
    *(BYTE**)(subWin.data() + 0x6C8) = rootWin.data();

    // Mock v1.2.0.9 stack frame at [rsp+0x68]=win, [rsp+0x70]=type, [rsp+0x7C]=row
    std::vector<BYTE> mockStack(0x100, 0);
    *(BYTE**)(mockStack.data() + 0x68) = subWin.data();
    *(BYTE**)(mockStack.data() + 0x70) = vtTruck;
    *(int*)(mockStack.data() + 0x7C) = 0;

    CONTEXT ctx = {};
    ctx.Rsp = (DWORD64)(uintptr_t)mockStack.data();
    ctx.R14 = (DWORD64)(uintptr_t)world;
    ctx.R12 = (DWORD64)(uintptr_t)vtTruck;
    ctx.Rsi = (DWORD64)(uintptr_t)subWin.data();
    ctx.R13 = 0xDEADBEEFULL; // stale v1.1.1.9 register MUST be ignored
    ctx.Rbx = 0ULL;
    const double rub = 12345.0;
    const double usd = 6789.0;
    const float avail = 0.25f;
    memcpy(&ctx.Xmm9, &rub, sizeof(rub));
    memcpy(&ctx.Xmm10, &usd, sizeof(usd));
    memcpy(&ctx.Xmm8, &avail, sizeof(avail));

    PurchaseMsg msg = {};
    const char* why = "";
    BYTE* outType = nullptr;
    TEST_CHECK(Purchase_CaptureForTest(&ctx, &msg, &why, &outType) == 1);
    TEST_CHECK(outType == vtTruck);
    TEST_CHECK(msg.kind == 1u && msg.typeIndex == 1u);
    TEST_CHECK(msg.depot.place == 0 && msg.depot.id == 101);
    TEST_CHECK(msg.customsRub.place == 1 && msg.customsRub.id == 102);
    TEST_CHECK(msg.customsUsd.place == 2 && msg.customsUsd.id == 103);
    TEST_CHECK(msg.rubles == 12345.0 && msg.dollars == 6789.0 && msg.avail == 0.25f);
    TEST_CHECK(msg.refill == 1 && msg.used == 1);
    TEST_CHECK_NEAR(msg.usedAge, 4.25, 1e-9);
    TEST_CHECK_NEAR(msg.usedAge2, 9.5, 1e-9);

    // Container category == 9 reads row from [rsp+0x7C]
    *(BYTE**)(mockStack.data() + 0x70) = vtCont;
    ctx.R12 = (DWORD64)(uintptr_t)vtCont;
    *(unsigned*)(subWin.data() + 0x248) = 0u;
    TEST_CHECK(Purchase_CaptureForTest(&ctx, &msg, &why, &outType) == 1);
    TEST_CHECK(outType == vtCont && msg.resIndex == 0);

    // kind == 0xB auto-replace resolves both newType ([rsp+0x70]) and targetType (*win+0x240)
    BYTE* targetEntry = vtCont;
    *(unsigned*)(subWin.data() + 0x23C) = 0xBu;
    *(BYTE**)(subWin.data() + 0x240) = (BYTE*)&targetEntry;
    *(BYTE**)(mockStack.data() + 0x70) = vtTruck;
    ctx.R13 = 0ULL;
    TEST_CHECK(Purchase_CaptureForTest(&ctx, &msg, &why, &outType) == 1);
    TEST_CHECK(outType == vtTruck && msg.kind == 0xBu && msg.typeIndex == 1u && msg.resIndex == 0);

    // Wrong world register (R14 != world) refuses with "registers"
    ctx.R14 = 0ULL;
    TEST_CHECK(Purchase_CaptureForTest(&ctx, &msg, &why, &outType) == 2);
    TEST_CHECK(strcmp(why, "registers") == 0);

    // End-to-end Purchase_OnPress: uses capturedType (NOT stale R13=0) for SaveSync_CheckVehicleDlcAllowed
    static const struct { unsigned rva; unsigned char b[16]; } kPurchaseSigs[] = {
        { 0x4033A0, { 0x44,0x89,0x4c,0x24,0x20,0x53,0x56,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x83,0xec } },
        { 0x64FA50, { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 } },
        { 0x7A76D0, { 0x48,0x85,0xd2,0x0f,0x84,0xee,0x01,0x00,0x00,0x48,0x89,0x5c,0x24,0x20,0x57,0x48 } },
        { 0x403E60, { 0x48,0x8b,0xc4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48 } },
        { 0x420960, { 0x44,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x48 } },
        { 0x003430, { 0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0xfa,0x48,0x8b,0xf1 } },
        { 0x2C7380, { 0x48,0x85,0xd2,0x0f,0x84,0xef,0x05,0x00,0x00,0x4c,0x8b,0xdc,0x4d,0x89,0x43,0x18 } },
        { 0x054960, { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x08,0x48,0x8b } },
        { 0x409EB0, { 0x40,0x55,0x56,0x57,0x48,0x8d,0xac,0x24,0x70,0xff,0xff,0xff,0x48,0x81,0xec,0x90 } },
        { 0x409930, { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48 } },
        { 0x0E42F0, { 0x48,0x83,0xec,0x28,0x4c,0x8b,0x49,0x10,0x4c,0x8b,0xc1,0x48,0x8b,0x51,0x08,0x49 } },
        { 0x68A0B0, { 0x48,0x85,0xd2,0x0f,0x84,0x26,0x01,0x00,0x00,0x57,0x48,0x83,0xec,0x20,0x66,0x0f } },
        { 0x688650, { 0x48,0x85,0xd2,0x0f,0x84,0xde,0x00,0x00,0x00,0x53,0x48,0x83,0xec,0x20,0x48,0x8b } },
        { 0x63C410, { 0x48,0x85,0xd2,0x0f,0x84,0xf0,0x00,0x00,0x00,0x57,0x48,0x83,0xec,0x20,0x8b,0x82 } },
    };
    for (const auto& s : kPurchaseSigs) {
        memcpy(g_mockExeBuffer + s.rva, s.b, sizeof(s.b));
    }
    strcpy_s((char*)(vtTruck + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "dlc2/vehicles/biomes_truck.ini");
    *(unsigned*)(subWin.data() + 0x23C) = 1u;
    *(BYTE**)(subWin.data() + 0x240) = depot.data();
    *(unsigned*)(subWin.data() + 0x248) = 0u;
    *(BYTE**)(mockStack.data() + 0x70) = vtTruck;
    ctx.R14 = (DWORD64)(uintptr_t)world;
    ctx.R12 = (DWORD64)(uintptr_t)vtTruck;
    ctx.R13 = 0ULL;
    ctx.Rip = (DWORD64)(uintptr_t)(g_mockExeBuffer + 0x852B08u);
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "");
    SaveSync_SetTestDlcLists("dlc1,dlc2", "dlc1"); // dlc2 unshared -> blocked!
    Commands_SetSession(COMMANDS_HOST);
    Purchase_ResetDiagStatsForTest();
    TEST_CHECK(!Purchase_OnPress(true, &ctx));
    VehicleSyncDiagStats diag = {};
    Purchase_GetDiagStats(&diag);
    TEST_CHECK(diag.purchasesAttempted == 1 && diag.purchasesBlockedDlc == 1);
    Commands_SetSession(COMMANDS_OFF);
    SaveSync_SetTestDlcLists("", "");
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    Purchase_ResetDiagStatsForTest();

    ResetMockWorldMemory();
    return true;
}

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
    TEST_CHECK(VEC_BUILDINGS_BEGIN_RVA == 0xA30AD8);
    TEST_CHECK(VEC_CONSTRUCTIONS_BEGIN_RVA == 0xA30BF8);
    TEST_CHECK(WORKSHOP_MGR_PTR_RVA == 0xA2C368);
    TEST_CHECK(MANAGER_RESET_STATE_OFFSET == 0x408);
    TEST_CHECK(MGR_ROUTE_BEGIN_OFFSET == 0x410);
    TEST_CHECK(HEADER_BIN_DATA_OFFSET == 0x260);
    TEST_CHECK(HEADER_BIN_RECORD_SIZE == 0x80);
    TEST_CHECK(WORLD_WORK_RESOURCE_PTR_OFF == 0xC368);
    TEST_CHECK(WORLD_ECON_TAB_OFFSET == 0xC6FC);
    TEST_CHECK(WORLD_LOANS_OFFSET == 0x10C70);
    TEST_CHECK(WORLD_LOAN_SELECTED_OFFSET == 0x10C88);
    TEST_CHECK(WORLD_LOAN_REPAY_AMOUNT_OFF == 0x10CA8);
    TEST_CHECK(WORLD_LOAN_STATS_OFFSET == 0x117B8);
    TEST_CHECK(RVA_VEC_LOAN_PUSH_BACK == 0x46D480);
    TEST_CHECK(RVA_TAKE_LOAN == 0x4DF5B0);
    TEST_CHECK(RVA_GET_MAX_AVAILABLE_BORROW == 0x4DF7F0);
    TEST_CHECK(RVA_STEP_ONE_LOAN_DAY == 0x4DF900);
    TEST_CHECK(RVA_STEP_ALL_LOANS_DAILY == 0x4DFAD0);
    TEST_CHECK(RVA_CALC_INTEREST_RATE == 0x4DFEF0);
    TEST_CHECK(sizeof(SovietLoan) == 0x28);
    TEST_CHECK(offsetof(SovietLoan, yearInterestRate) == 0x00);
    TEST_CHECK(offsetof(SovietLoan, loanSubType) == 0x0C);
    TEST_CHECK(offsetof(SovietLoan, remainingPrincipal) == 0x14);
    TEST_CHECK(offsetof(SovietLoan, unpaidPenaltyBalance) == 0x18);
    TEST_CHECK(offsetof(SovietLoan, totalPaidSoFar) == 0x24);
    TEST_CHECK(sizeof(LsLoanCommand) == 84);
    TEST_CHECK(RVA_HUD_DRAW_BTN_FRAME == 0x46AA70);
    TEST_CHECK(RVA_MINIMAP_DRAW_CONTENT == 0x4E42F0);
    TEST_CHECK(RVA_MINIMAP_DRAW_BUTTONS == 0x4E63B0);
    TEST_CHECK(WORLD_MINIMAP_PTR_OFFSET == 0xD4A0);
    TEST_CHECK(MINIMAP_TOOLTIP_WSTR_RVA == 0xA2C560);
    TEST_CHECK(HUD_QUAD_STAGE_RVA == 0xA08070);
    TEST_CHECK(HUD_SOLID_TEX_PTR_RVA == 0xA2ABE8);
    TEST_CHECK(HUD_PIN_ICON_TEX_PTR_RVA == 0xA2A460);
    TEST_CHECK(HUD_MAP_CAMERA_TEX_PTR_RVA == 0xA2A868);
    TEST_CHECK_NEAR(MINIMAP_CAMERA_ICON_MAP_FRACTION, 0.15f, 1e-4f);
    TEST_CHECK_NEAR(MINIMAP_CAMERA_ICON_MAX_PX, 70.0f, 1e-4f);
    TEST_CHECK(HUD_QUAD_ROT_RVA == 0xA08318);
    TEST_CHECK(HUD_MIDDLEPOINT_OBJ_RVA == 0xA34D90);
    TEST_CHECK(HUD_FMT_WSTR_RVA == 0x8FD6E0);
    TEST_CHECK(HUD_BASIC_PASS_STR_RVA == 0x8B708C);
    TEST_CHECK(RVA_IAT_CREATE_MANAGED_TEX == 0x8B1518);
    TEST_CHECK(RVA_IAT_HUD_HIT_TEST_QUAD == 0x8B18F0);
    TEST_CHECK(RVA_IAT_HUD_DRAW_QUAD == 0x8B1900);
    TEST_CHECK(RVA_IAT_HUD_GET_MOUSE_POS == 0x8B1908);
    TEST_CHECK(RVA_IAT_TEXT_PRINT_ALL == 0x8B1910);
    TEST_CHECK(RVA_IAT_TEXT_DRAW_W == 0x8B1920);
    TEST_CHECK(RVA_IAT_HUD_END_DRAW == 0x8B1998);
    TEST_CHECK(RVA_IAT_HUD_BEGIN_DRAW == 0x8B19B8);
    TEST_CHECK(WORLD_STUDIOS_BEGIN_OFFSET == 0xD2D0u);
    TEST_CHECK(WORLD_STUDIOS_END_OFFSET == 0xD2D8u);
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
    TEST_CHECK(RVA_CIT_SEVERE_ILLNESS_TICK == 0x874C29u);
    TEST_CHECK(RVA_CIT_SEVERE_ILLNESS_RESUME == 0x874C43u);
    TEST_CHECK(RVA_WATER_HEALTH_STEP == 0x1C9740u);
    TEST_CHECK(RVA_VEHICLE_CARGO_TRADE == 0x6BDD00u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_CHECK == 0x61ACF0u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_GET == 0x61AD90u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_TOOLTIP == 0x7EA697u);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_TOOLTIP_FOUND == 0x7EA72Au);
    TEST_CHECK(RVA_BLD_COUNT_LIMIT_TOOLTIP_SKIP == 0x7EA930u);

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

    if (!SubTest_PowerHighAndLateToolStubRegressions()) return false;
    if (!SubTest_PurchaseV1209CaptureRegressions()) return false;

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
    *(BYTE**)(world + 0xFAC8) = railNet.data();

    RouteRef refN = kNoRef;
    TEST_CHECK(Ref_Node((U64)nodeObj.data(), refN));
    TEST_CHECK(refN.list == 0xFAC8 && refN.place == 0);
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
    const unsigned kToxicSlotIdx = (0xC4A0u - WORLD_RES_PTR_SLOTS_BEGIN_OFF) / 8u;
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
    *(BYTE**)(world + 0xFAC8) = nullptr;
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
    std::vector<BYTE> veh(VEHICLE_STRUCT_BYTES, 0);
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
static_assert(sizeof(LsHash_M9_4) == 216, "LsHash_M9_4 must be exactly 216 bytes");

static bool SubTest_M9_4_StateSummaryAndHashing()
{
    // Feature 37: StateSummary struct layout and expanded hashing (208 bytes, 8-byte aligned)
    TEST_CHECK(sizeof(StateSummary) == 208);
    TEST_CHECK(sizeof(LsHash_M9_4) == 216);

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
    *(BYTE**)(world + 0xF840) = roadMgr.data();

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
    TEST_CHECK(BuildingSwitches_IsLocalPerStateWorldSettingForTest(0xC838));
    TEST_CHECK(BuildingSwitches_IsLocalPerStateWorldSettingForTest(0xC83B));
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
        0xF840, 0xF848, 0xF8A0, 0xF8F8, 0xF950, 0xF9A8, 0xFA08,
        0xFA60, 0xFAB8, 0xFAC0, 0xFAD0, 0xFAE0, 0xFB38, 0xFB90
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

    // Verify per-network DESYNC_DUMP[net] breakdown logging on networks mismatch
    {
        static std::string s_capturedNetLog;
        s_capturedNetLog.clear();
        struct LogCaptureGuard {
            void (*orig)(const char*, ...);
            explicit LogCaptureGuard(void (*capture)(const char*, ...)) : orig(g_mockHost.log)
            {
                g_mockHost.log = capture;
            }
            ~LogCaptureGuard() { g_mockHost.log = orig; }
        } captureGuard([](const char* fmt, ...) {
            if (!fmt) return;
            char buf[512] = {};
            va_list ap;
            va_start(ap, fmt);
            vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
            va_end(ap);
            s_capturedNetLog.append(buf);
            s_capturedNetLog.push_back('\n');
        });

        *(BYTE**)(world + 0xF840) = netMgr.data();
        StateHash_LogDesyncBreakdown("networks ", 6780);
        *(BYTE**)(world + 0xF840) = nullptr;

        TEST_CHECK(s_capturedNetLog.find("DESYNC_DUMP[net] t=6780 i=0 off=0xF840 name=ROAD nodes=1 segs=1 ") != std::string::npos);
        TEST_CHECK(s_capturedNetLog.find("last=(1,0)") != std::string::npos);
        TEST_CHECK(s_capturedNetLog.find("DESYNC_DUMP[net] t=6780 i=13 off=0xFB90 name=WATERPIPE nodes=-1 segs=-1") != std::string::npos);

        // Verify large-map breakdown (>256 segments) reports the true newest segList[nSegs - 1]
        std::vector<BYTE> segLast(0x200, 0);
        segLast[SEG_TYPE_OFFSET] = 9;
        *(short*)(segLast.data() + SEG_DIRECTION_OFFSET) = 2;
        std::vector<BYTE*> largeSegVec(260, segObj.data());
        largeSegVec[259] = segLast.data();
        WriteMockVectorHeader(netMgr.data() + MGR_SEGMENTS_BEGIN_OFFSET, largeSegVec);

        s_capturedNetLog.clear();
        *(BYTE**)(world + 0xF840) = netMgr.data();
        StateHash_LogDesyncBreakdown("networks ", 6781);
        *(BYTE**)(world + 0xF840) = nullptr;

        TEST_CHECK(s_capturedNetLog.find("DESYNC_DUMP[net] t=6781 i=0 off=0xF840 name=ROAD nodes=1 segs=260 ") != std::string::npos);
        TEST_CHECK(s_capturedNetLog.find("last=(9,2)") != std::string::npos);
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
    std::vector<BYTE> animType(BLD_TYPE_ANIM_SPEED_OFFSET + 0x40u, 0);
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
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC350) = resTable.data();
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC358) = resTable.data() + resTable.size();

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
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC350) = nullptr;
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + 0xC358) = nullptr;
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
    WriteMockVectorHeader(g_mockExeBuffer + 0xA317E8, vehList);
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
    TEST_CHECK(HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E8, 0.5f, [](void* ud) {
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
    bool cleanAfterPtrChange = HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E8, 0.0f, [](void* ud) {
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
        WriteMockVectorHeader(g_mockExeBuffer + 0xA317E8, badList);
        HeldFrame_OnSimTick(3);
        struct UnmapVehCtx { BYTE* page; DWORD oldProt; } uctx = { badVehPage, 0 };
        HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E8, 0.5f, [](void* ud) {
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
    *(float*)(g_mockExeBuffer + 0x95256C) = 100.0f;                  // AGENT_NEAR_RVA
    WriteAbsJmp12(g_mockExeBuffer + 0x87E5D0, (void*)mockDrawAgents); // DRAW_AGENTS_RVA

    std::vector<BYTE> viewRender(0x180, 0);
    viewRender[0x178] = 1;
    std::vector<BYTE> viewBld(0x1170, 0);
    *(BYTE**)(viewBld.data() + 0x5D0) = viewRender.data();
    std::vector<BYTE> viewAgent(0x5A0, 0);
    *(BYTE**)(viewAgent.data() + 0x598) = viewBld.data();

    // Frame 1: buildings=1, agents=1 -> DRAW_AGENTS_RVA called once, view caches populated
    std::vector<BYTE*> viewBlds = { viewBld.data() };
    std::vector<BYTE*> viewAgents = { viewAgent.data() };
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD8, viewBlds);
    WriteMockVectorHeader(g_mockExeBuffer + 0xA1EF20 + 0x12760, viewAgents);
    HeldFrame_AfterEarly();
    size_t vSz = 0, nSz = 0, kSz = 0;
    HeldFrame_GetViewCacheSizesForTest(&vSz, &nSz, &kSz);
    TEST_CHECK(s_mockDrawAgentsCalls == 1);
    TEST_CHECK(vSz == 1 && nSz == 1 && kSz == 1);

    // Frame 2: buildings=0, agents=1 -> g_viewBuildings MUST be cleared and DRAW_AGENTS_RVA MUST NOT be called
    viewBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD8, viewBlds);
    viewBld[0x115A] = 0x55;
    HeldFrame_AfterEarly();
    HeldFrame_GetViewCacheSizesForTest(&vSz, nullptr, nullptr);
    TEST_CHECK(s_mockDrawAgentsCalls == 1);
    TEST_CHECK(vSz == 0);
    TEST_CHECK(viewBld[0x115A] == 0x55);

    // Frame 3 + HeldFrame_Reset(): populates view caches again, then HeldFrame_Reset clears all three
    viewBlds = { viewBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD8, viewBlds);
    HeldFrame_AfterEarly();
    HeldFrame_GetViewCacheSizesForTest(&vSz, &nSz, &kSz);
    TEST_CHECK(s_mockDrawAgentsCalls == 2);
    TEST_CHECK(vSz == 1 && nSz == 1 && kSz == 1);

    HeldFrame_Reset();
    HeldFrame_GetViewCacheSizesForTest(&vSz, &nSz, &kSz);
    TEST_CHECK(vSz == 0 && nSz == 0 && kSz == 0);

    viewBlds.clear();
    viewAgents.clear();
    WriteMockVectorHeader(g_mockExeBuffer + 0xA30AD8, viewBlds);
    WriteMockVectorHeader(g_mockExeBuffer + 0xA1EF20 + 0x12760, viewAgents);

    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = nullptr;
    *(BYTE***)(g_mockExeBuffer + 0xA317F0) = nullptr;
    ResetMockWorldMemory();
    return true;
}

// ---------------------------------------------------------------------------
// Test 71: Space Program Phase 2 (Presentation RNG, hashSpace, SoftSync, -119 VehicleType, orbits.dta Zip)
// ---------------------------------------------------------------------------

static bool SubTest_Phase2_PresentationRngSites()
{
    TEST_CHECK(Determinism_IsPresentationSiteForTest(RVA_SPACE_MC_RAND1_RET));
    TEST_CHECK(Determinism_IsPresentationSiteForTest(RVA_SPACE_MC_RAND2_RET));
    TEST_CHECK(Determinism_IsPresentationSiteForTest(RVA_SPACE_EXHAUST_RAND_RET));
    TEST_CHECK(Determinism_IsPresentationSiteForTest(RVA_SPACE_SOUND_RAND1_RET));
    TEST_CHECK(Determinism_IsPresentationSiteForTest(RVA_SPACE_SOUND_RAND2_RET));
    TEST_CHECK(Determinism_IsPresentationSiteForTest(RVA_SPACE_SOUND_RAND3_RET));

    // Space simulation RNG call sites must remain part of deterministic simulation stream
    TEST_CHECK(!Determinism_IsPresentationSiteForTest(0x6961AEu));
    TEST_CHECK(!Determinism_IsPresentationSiteForTest(0x6985CCu));
    TEST_CHECK(!Determinism_IsPresentationSiteForTest(0x697458u));
    return true;
}

static bool SubTest_Phase2_StateHashSpaceAndMixedDlcMilestones()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Minimal buildings vector required for StateHash_Compute to walk world
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE*> bldVec = { bld.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    // 2 orbit slots (0x60 bytes each) + global clock at +0x18 + local UI view at +0x14
    std::vector<BYTE> orbits(2u * ORBIT_SLOT_STRIDE, 0);
    *(float*)(orbits.data() + 0x00) = 250.0f;
    *(int32_t*)(orbits.data() + ORBIT_SLOT_TYPE_OFFSET) = 1;
    *(float*)(orbits.data() + ORBIT_SLOT_START_CLOCK_OFFSET) = 12.5f;
    *(float*)(orbits.data() + ORBIT_SLOT_STRIDE + 0x00) = 400.0f;
    *(int32_t*)(orbits.data() + ORBIT_SLOT_STRIDE + ORBIT_SLOT_TYPE_OFFSET) = 2;
    *(float*)(orbits.data() + ORBIT_SLOT_STRIDE + ORBIT_SLOT_START_CLOCK_OFFSET) = 33.25f;

    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = 0;
    *(float*)(world + WORLD_ORBIT_CLOCK_OFFSET) = 45.5f;
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbits.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbits.data() + orbits.size();

    // Host VehicleType pool: 119 dlc5 types (zero milestones) + 3 shared types
    std::vector<BYTE> hostTypes(122u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    for (int i = 0; i < 119; ++i) {
        _snprintf_s((char*)(hostTypes.data() + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET),
                    64, _TRUNCATE, "media_soviet/dlc5/vehicles/rocket_%03d.ini", i);
    }
    BYTE* hS0 = hostTypes.data() + 119u * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* hS1 = hostTypes.data() + 120u * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* hS2 = hostTypes.data() + 121u * WORLD_VEHICLE_TYPE_STRIDE;
    strcpy_s((char*)(hS0 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "media_soviet/vehicles/soyuz_capsule.ini");
    strcpy_s((char*)(hS1 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "media_soviet/vehicles/tatra_148.ini");
    strcpy_s((char*)(hS2 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "media_soviet/dlc2/vehicles/skoda_706.ini");
    hS0[VEH_TYPE_ROCKET_FLAG_OFFSET] = 1;
    *(int32_t*)(hS0 + VEH_TYPE_BUILT_IN_PROD_OFFSET) = 5;
    *(int32_t*)(hS0 + VEH_TYPE_SPACE_ORBIT_OFFSET) = 2;
    *(int32_t*)(hS0 + VEH_TYPE_SPACE_REENTRY_OFFSET) = 1;

    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = hostTypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = hostTypes.data() + hostTypes.size();

    std::vector<BYTE> station(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> capsule(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(station.data() + VEH_ID_OFFSET) = 9001u;
    *(int32_t*)(station.data() + VEH_ORBIT_INDEX_OFFSET) = 0;
    *(int32_t*)(station.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(station.data() + VEH_SPACE_DOCK_SLOT_OFFSET) = -1;
    *(int32_t*)(station.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;
    *(float*)(station.data() + VEH_ROCKET_PROGRESS_OFFSET) = 0.5f;

    *(unsigned*)(capsule.data() + VEH_ID_OFFSET) = 9002u;
    *(BYTE**)(capsule.data() + VEH_TYPE_PTR_OFFSET) = hS0;
    *(int32_t*)(capsule.data() + VEH_ORBIT_INDEX_OFFSET) = 0;
    *(int32_t*)(capsule.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = 1;
    *(BYTE**)(capsule.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) = station.data();
    *(int32_t*)(capsule.data() + VEH_SPACE_DOCK_SLOT_OFFSET) = 2;
    *(int32_t*)(capsule.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;
    *(float*)(capsule.data() + VEH_ROCKET_PROGRESS_OFFSET) = 0.75f;

    std::vector<BYTE*> vehVec = { station.data(), capsule.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehVec);

    StateSummary sHost = {};
    TEST_CHECK(StateHash_Compute(&sHost));

    // Local UI orbit view change (+0x14) must NEVER affect hashSpace
    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = 1;
    StateSummary sUiOnly = {};
    TEST_CHECK(StateHash_Compute(&sUiOnly));
    TEST_CHECK(sUiOnly.hashSpace == sHost.hashSpace);

    // Mixed-DLC Client (3 types only, reversed order, mixed path case & slashes) must match Host hashSpace!
    std::vector<BYTE> clientTypes(3u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    BYTE* cS2 = clientTypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* cS1 = clientTypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* cS0 = clientTypes.data() + 2u * WORLD_VEHICLE_TYPE_STRIDE;
    strcpy_s((char*)(cS2 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "DLC2\\VEHICLES\\SKODA_706.INI");
    strcpy_s((char*)(cS1 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "vehicles/tatra_148.ini");
    strcpy_s((char*)(cS0 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "MEDIA_SOVIET\\VEHICLES\\SOYUZ_CAPSULE.INI");
    cS0[VEH_TYPE_ROCKET_FLAG_OFFSET] = 1;
    *(int32_t*)(cS0 + VEH_TYPE_BUILT_IN_PROD_OFFSET) = 5;
    *(int32_t*)(cS0 + VEH_TYPE_SPACE_ORBIT_OFFSET) = 2;
    *(int32_t*)(cS0 + VEH_TYPE_SPACE_REENTRY_OFFSET) = 1;
    *(BYTE**)(capsule.data() + VEH_TYPE_PTR_OFFSET) = cS0;
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = clientTypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = clientTypes.data() + clientTypes.size();

    StateSummary sClient = {};
    TEST_CHECK(StateHash_Compute(&sClient));
    TEST_CHECK(sClient.hashSpace == sHost.hashSpace);

    // Negative zero vs positive zero in orbit slot must normalize identically
    *(uint32_t*)(orbits.data() + ORBIT_SLOT_STRIDE + 0x04) = 0x80000000u;
    StateSummary sNegZero = {};
    TEST_CHECK(StateHash_Compute(&sNegZero));
    TEST_CHECK(sNegZero.hashSpace == sClient.hashSpace);

    // Mutate orbit clock, orbit slot type, rocket progress, and milestone counter -> each mutates hashSpace only
    *(float*)(world + WORLD_ORBIT_CLOCK_OFFSET) = 46.0f;
    StateSummary sClockMut = {};
    TEST_CHECK(StateHash_Compute(&sClockMut));
    TEST_CHECK(sClockMut.hashSpace != sClient.hashSpace);
    TEST_CHECK(sClockMut.hashVehicles == sClient.hashVehicles && sClockMut.hashClock == sClient.hashClock);
    char what[128] = {};
    TEST_CHECK(LockstepNet_CompareSummaryForTest(sClient, sClockMut, what, sizeof(what)));
    TEST_CHECK(strstr(what, "space") != nullptr);

    *(float*)(world + WORLD_ORBIT_CLOCK_OFFSET) = 45.5f;
    *(int32_t*)(orbits.data() + ORBIT_SLOT_TYPE_OFFSET) = 3;
    StateSummary sSlotMut = {};
    TEST_CHECK(StateHash_Compute(&sSlotMut));
    TEST_CHECK(sSlotMut.hashSpace != sClient.hashSpace);

    *(int32_t*)(orbits.data() + ORBIT_SLOT_TYPE_OFFSET) = 1;
    *(float*)(capsule.data() + VEH_ROCKET_PROGRESS_OFFSET) = 0.80f;
    StateSummary sVehSpaceMut = {};
    TEST_CHECK(StateHash_Compute(&sVehSpaceMut));
    TEST_CHECK(sVehSpaceMut.hashSpace != sClient.hashSpace);
    TEST_CHECK(sVehSpaceMut.hashVehicles == sClient.hashVehicles);

    *(float*)(capsule.data() + VEH_ROCKET_PROGRESS_OFFSET) = 0.75f;
    *(int32_t*)(cS0 + VEH_TYPE_SPACE_ORBIT_OFFSET) = 3;
    StateSummary sMilestoneMut = {};
    TEST_CHECK(StateHash_Compute(&sMilestoneMut));
    TEST_CHECK(sMilestoneMut.hashSpace != sClient.hashSpace);

    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase2_VehicleSoftSyncSpaceExclusion()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> orbits(ORBIT_SLOT_STRIDE, 0);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbits.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbits.data() + orbits.size();

    std::vector<BYTE> rocketType(WORLD_VEHICLE_TYPE_STRIDE, 0);
    rocketType[VEH_TYPE_ROCKET_FLAG_OFFSET] = 1;
    std::vector<BYTE> rocketPartType(WORLD_VEHICLE_TYPE_STRIDE, 0);
    rocketPartType[VEH_TYPE_ROCKETPART_FLAG_OFFSET] = 1;
    std::vector<BYTE> reentryType(WORLD_VEHICLE_TYPE_STRIDE, 0);
    reentryType[VEH_TYPE_SPACECRAFT_FLAG_OFFSET] = 1;
    reentryType[VEH_TYPE_SPACE_REENTRY_FLAG_OFFSET] = 1;

    std::vector<BYTE> rocketFlight(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> rocketOrbit(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> rocketDocked(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> rocketPad(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> landedCapsule(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> rocketPartOnTruck(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> rocketPartOnRocket(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> groundTruck(VEHICLE_STRUCT_BYTES, 0);

    *(unsigned*)(rocketFlight.data() + VEH_ID_OFFSET) = 1001u;
    *(int32_t*)(rocketFlight.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(rocketFlight.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 2; // reentry
    *(float*)(rocketFlight.data() + VEH_SPEED_OFFSET) = 150.0f;
    *(float*)(rocketFlight.data() + VEH_POS_OFFSET) = 500.0f;

    *(unsigned*)(rocketOrbit.data() + VEH_ID_OFFSET) = 1002u;
    *(BYTE**)(rocketOrbit.data() + VEH_TYPE_PTR_OFFSET) = rocketType.data();
    *(int32_t*)(rocketOrbit.data() + VEH_ORBIT_INDEX_OFFSET) = 0; // in valid orbit 0
    *(float*)(rocketOrbit.data() + VEH_SPEED_OFFSET) = 10.0f;

    *(unsigned*)(rocketDocked.data() + VEH_ID_OFFSET) = 1003u;
    *(int32_t*)(rocketDocked.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(BYTE**)(rocketDocked.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) = rocketOrbit.data();
    *(float*)(rocketDocked.data() + VEH_SPEED_OFFSET) = 5.0f;

    // Pad rocket ($SPACE_ROCKET) and landed capsule ($SPACE_SHIP_REENTRY) must be protected from SoftSync
    // (even with real CreateVehicle 0x403FBD defaults: progress = 1.0f, orbitIdx = -1, targetOrbitIdx = -1).
    *(unsigned*)(rocketPad.data() + VEH_ID_OFFSET) = 1004u;
    *(BYTE**)(rocketPad.data() + VEH_TYPE_PTR_OFFSET) = rocketType.data();
    *(int32_t*)(rocketPad.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(rocketPad.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(rocketPad.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(int32_t*)(rocketPad.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 0;
    *(float*)(rocketPad.data() + VEH_SPEED_OFFSET) = 1.0f;

    *(unsigned*)(landedCapsule.data() + VEH_ID_OFFSET) = 1006u;
    *(BYTE**)(landedCapsule.data() + VEH_TYPE_PTR_OFFSET) = reentryType.data();
    *(int32_t*)(landedCapsule.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(landedCapsule.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(landedCapsule.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(int32_t*)(landedCapsule.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 0;

    // Ground truck with REAL CreateVehicle (0x403FBD) default progress == 1.0f MUST still sync when orbits exist!
    *(unsigned*)(groundTruck.data() + VEH_ID_OFFSET) = 1005u;
    *(int32_t*)(groundTruck.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(groundTruck.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(groundTruck.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(float*)(groundTruck.data() + VEH_SPEED_OFFSET) = 45.0f;
    *(float*)(groundTruck.data() + VEH_POS_OFFSET) = 123.0f;

    *(unsigned*)(rocketPartOnTruck.data() + VEH_ID_OFFSET) = 1007u;
    *(BYTE**)(rocketPartOnTruck.data() + VEH_TYPE_PTR_OFFSET) = rocketPartType.data();
    *(int32_t*)(rocketPartOnTruck.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(rocketPartOnTruck.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(rocketPartOnTruck.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(BYTE**)(rocketPartOnTruck.data() + VEH_PARENT_PTR_OFFSET) = groundTruck.data();

    // Rocket part (+0x296) attached to a flying rocket IS excluded via parent recursion
    *(unsigned*)(rocketPartOnRocket.data() + VEH_ID_OFFSET) = 1008u;
    *(BYTE**)(rocketPartOnRocket.data() + VEH_TYPE_PTR_OFFSET) = rocketPartType.data();
    *(int32_t*)(rocketPartOnRocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(rocketPartOnRocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(rocketPartOnRocket.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(BYTE**)(rocketPartOnRocket.data() + VEH_PARENT_PTR_OFFSET) = rocketFlight.data();

    // Earth ground transport of dlc5 vehicles: $SPACE_SHIP container on flatbed truck & $SPACE_ROCKET wagon towed by locomotive
    std::vector<BYTE> carriedShip(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(carriedShip.data() + VEH_ID_OFFSET) = 1009u;
    *(BYTE**)(carriedShip.data() + VEH_TYPE_PTR_OFFSET) = reentryType.data();
    *(int32_t*)(carriedShip.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(carriedShip.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(carriedShip.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(BYTE**)(carriedShip.data() + VEH_PARENT_PTR_OFFSET) = groundTruck.data();

    std::vector<BYTE> towedRocket(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(towedRocket.data() + VEH_ID_OFFSET) = 1010u;
    *(BYTE**)(towedRocket.data() + VEH_TYPE_PTR_OFFSET) = rocketType.data();
    *(int32_t*)(towedRocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(towedRocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(towedRocket.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(BYTE**)(towedRocket.data() + VEH_LINK_PTR_OFFSET) = groundTruck.data();

    std::vector<BYTE> padType(0x400, 0);
    *(int32_t*)(padType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD;
    *(int32_t*)(padType.data() + BLD_TYPE_SUBTYPE_OFF) = (int32_t)BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL;
    std::vector<BYTE> launchPad(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(launchPad.data() + BLD_TYPE_OFFSET) = padType.data();

    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, rocketFlight.data()));
    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, rocketOrbit.data()));
    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, rocketDocked.data()));
    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, rocketPad.data()));
    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, landedCapsule.data()));
    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, rocketPartOnRocket.data()));
    TEST_CHECK(!VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, rocketPartOnTruck.data()));
    TEST_CHECK(!VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, groundTruck.data()));
    TEST_CHECK(!VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, carriedShip.data()));
    TEST_CHECK(!VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, towedRocket.data()));
    // Once the towed rocket enters a Launch Pad building, it IS excluded from SoftSync immediately
    *(BYTE**)(towedRocket.data() + VEH_CUR_BLD_OFFSET) = launchPad.data();
    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, towedRocket.data()));
    *(BYTE**)(towedRocket.data() + VEH_CUR_BLD_OFFSET) = nullptr;

    // Active transfer progress strictly inside (0, 1) with valid world orbits IS excluded
    std::vector<BYTE> activeTransfer(VEHICLE_STRUCT_BYTES, 0);
    *(int32_t*)(activeTransfer.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(activeTransfer.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    *(float*)(activeTransfer.data() + VEH_ROCKET_PROGRESS_OFFSET) = 0.5f;
    TEST_CHECK(VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, activeTransfer.data()));

    // Verify 0-initialized truck is NOT falsely skipped even when world has active orbits
    std::vector<BYTE> zeroTruck(VEHICLE_STRUCT_BYTES, 0);
    TEST_CHECK(!VehicleSoftSync_IsVehicleInSpaceFlightForTest(world, zeroTruck.data()));
    TEST_CHECK(!VehicleSoftSync_IsVehicleInSpaceFlightForTest(nullptr, zeroTruck.data()));

    std::vector<BYTE*> vehList = {
        rocketFlight.data(), rocketOrbit.data(), rocketDocked.data(), rocketPad.data(),
        landedCapsule.data(), rocketPartOnRocket.data(), rocketPartOnTruck.data(), groundTruck.data()
    };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehList);

    // Build immediate packet: only rocketPartOnTruck (1007) and groundTruck (1005) may be captured
    std::vector<BYTE> pkt(4096, 0);
    size_t pktSz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &pktSz, true));
    const VehicleSoftSyncMsg_M4* msg = (const VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 2u);
    TEST_CHECK((msg->entries[0].vehicleId == 1007u && msg->entries[1].vehicleId == 1005u) ||
               (msg->entries[0].vehicleId == 1005u && msg->entries[1].vehicleId == 1007u));

    // Client defense-in-depth: spoofed packet targeting rocketFlight (place 0, id 1001) must NOT alter its position
    VehicleSoftSyncMsg_M4 spoof = {};
    spoof.count = 1u;
    spoof.entries[0].place = 0;
    spoof.entries[0].vehicleId = 1001u;
    spoof.entries[0].currentBldId = -1;
    spoof.entries[0].pos[0] = 9999.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(&spoof, sizeof(spoof)));
    TEST_CHECK_NEAR(*(float*)(rocketFlight.data() + VEH_POS_OFFSET), 500.0f, 1e-4);

    // Child translation guard: carried $SPACE_SHIP container translates WITH flatbed truck, while active rocket does NOT
    *(float*)(carriedShip.data() + VEH_POS_OFFSET) = 123.0f;
    std::vector<BYTE*> children = { rocketFlight.data(), carriedShip.data() };
    WriteMockVectorHeader(groundTruck.data() + VEH_CHILDREN_BEGIN_OFFSET, children);
    VehicleSoftSync_TranslateWithChildrenForTest(groundTruck.data(), 10.0f, 0.0f, 0.0f, false);
    TEST_CHECK_NEAR(*(float*)(groundTruck.data() + VEH_POS_OFFSET), 133.0f, 1e-4);
    TEST_CHECK_NEAR(*(float*)(carriedShip.data() + VEH_POS_OFFSET), 133.0f, 1e-4);
    TEST_CHECK_NEAR(*(float*)(rocketFlight.data() + VEH_POS_OFFSET), 500.0f, 1e-4);

    ResetMockWorldMemory();
    return true;
}

static U64 g_phase2CapturedHalfTypePtr = 0;
static void __fastcall Phase2_MockCargoDestroy(U64) {}
static void __fastcall Phase2_MockCargoCopy(U64, U64 src)
{
    U64 b = 0, e = 0;
    if (SafeReadPod((const BYTE*)(src + 0x68), &b) && SafeReadPod((const BYTE*)(src + 0x70), &e) && b && e >= b + 8) {
        SafeReadPod((const BYTE*)b, &g_phase2CapturedHalfTypePtr);
    }
}

static bool SubTest_Phase2_MixedDlcWorldRefsShift119()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Host has 122 VehicleTypes: 0..118 dlc5 rockets, 119 Workshop truck, 120 vanilla truck, 121 unnamed mock
    std::vector<BYTE> hostTypes(122u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    for (int i = 0; i < 119; ++i) {
        _snprintf_s((char*)(hostTypes.data() + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET),
                    64, _TRUNCATE, "media_soviet/dlc5/vehicles/rocket_%03d.ini", i);
    }
    strcpy_s((char*)(hostTypes.data() + 119u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET),
             64, "media_soviet/workshop_wip/3123456789/kamaz_5320.ini");
    strcpy_s((char*)(hostTypes.data() + 120u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET),
             64, "media_soviet/vehicles/tatra_148.ini");

    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = hostTypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = hostTypes.data() + hostTypes.size();

    U64 hostWorkshopVt = (U64)(hostTypes.data() + 119u * WORLD_VEHICLE_TYPE_STRIDE);
    U64 hostDlc5Vt = (U64)(hostTypes.data() + 12u * WORLD_VEHICLE_TYPE_STRIDE);
    U64 hostUnnamedVt = (U64)(hostTypes.data() + 121u * WORLD_VEHICLE_TYPE_STRIDE);

    BYTE encWs[136] = {}, encDlc5[136] = {}, encUnnamed[136] = {};
    size_t lenWs = 0, lenDlc5 = 0, lenUnnamed = 0;
    TEST_CHECK(Ref_EncodeDefPtrBytes(hostWorkshopVt, encWs, sizeof(encWs), lenWs));
    TEST_CHECK(encWs[0] == 5u && lenWs > 6u);
    TEST_CHECK(Ref_EncodeDefPtrBytes(hostDlc5Vt, encDlc5, sizeof(encDlc5), lenDlc5));
    TEST_CHECK(encDlc5[0] == 5u && lenDlc5 > 6u);
    TEST_CHECK(Ref_EncodeDefPtrBytes(hostUnnamedVt, encUnnamed, sizeof(encUnnamed), lenUnnamed));
    TEST_CHECK(encUnnamed[0] == 2u && lenUnnamed == 5u);

    // Switch world to Client without dlc5 (2 types only: Workshop at index 0, vanilla at index 1)
    std::vector<BYTE> clientTypes(2u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    strcpy_s((char*)(clientTypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET),
             64, "WORKSHOP_WIP\\3123456789\\KAMAZ_5320.INI");
    strcpy_s((char*)(clientTypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET),
             64, "media_soviet/vehicles/tatra_148.ini");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = clientTypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = clientTypes.data() + clientTypes.size();

    U64 decoded = 0;
    size_t at = 0;
    TEST_CHECK(Ref_DecodeDefPtrBytes(encWs, lenWs, at, decoded));
    TEST_CHECK(at == lenWs && decoded == (U64)(clientTypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE));

    // Unshared dlc5 rocket must fail cleanly instead of aliasing clientTypes[0]
    at = 0;
    TEST_CHECK(!Ref_DecodeDefPtrBytes(encDlc5, lenDlc5, at, decoded));

    // End-to-end CargoSettings_ApplyCommand decode using tag==5 Workshop VehicleType inside vector index 3 (+0x68)
    const unsigned char sigDestroy[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xff, 0x48 };
    const unsigned char sigCopy[16]    = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe };
    const unsigned char sigRefresh[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 };
    memcpy(g_mockExeBuffer + 0x0F6520, sigDestroy, 16);
    memcpy(g_mockExeBuffer + 0x21FBF0, sigCopy, 16);
    memcpy(g_mockExeBuffer + 0x4BFC10, sigRefresh, 16);

    std::vector<BYTE> stopVeh(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> stopRec(0x198, 0);
    *(unsigned*)(stopVeh.data() + VEH_ID_OFFSET) = 4242u;
    *(BYTE**)(stopVeh.data() + 0x638) = stopRec.data();
    *(BYTE**)(stopVeh.data() + 0x640) = stopRec.data() + 0x198;
    std::vector<BYTE*> vehList = { stopVeh.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehList);

    RouteRef stopRef = kNoRef, noTarget = kNoRef;
    TEST_CHECK(Ref_Vehicle((U64)stopVeh.data(), stopRef));

    BYTE primeBuf[64] = {};
    *(unsigned*)primeBuf = 0x32534743u;
    CargoSettings_ApplyCommand(primeBuf, sizeof(primeBuf));
    WriteAbsJmp12(g_mockExeBuffer + 0x0F6520, (void*)&Phase2_MockCargoDestroy);
    WriteAbsJmp12(g_mockExeBuffer + 0x21FBF0, (void*)&Phase2_MockCargoCopy);

    std::vector<BYTE> pkt;
    auto pushRaw = [&](const void* p, size_t n) { pkt.insert(pkt.end(), (const BYTE*)p, (const BYTE*)p + n); };
    unsigned magic = 0x32534743u, recTotal = 1u, zeroVec = 0u, oneVec = 1u;
    BYTE kind = 0, half = 0, linkActive = 0, scalars[22] = {};
    unsigned short recIdx = 0;
    U64 tail190 = 0;
    pushRaw(&magic, 4);
    pushRaw(&kind, 1);
    pushRaw(&half, 1);
    pushRaw(&recIdx, 2);
    pushRaw(&recTotal, 4);
    pushRaw(&stopRef, sizeof(stopRef));
    pushRaw(&noTarget, sizeof(noTarget));
    pushRaw(&linkActive, 1);
    pushRaw(&tail190, 8);
    pushRaw(scalars, sizeof(scalars));
    for (int v = 0; v < 7; ++v) {
        if (v == 3) {
            pushRaw(&oneVec, 4);
            pushRaw(encWs, lenWs);
        } else {
            pushRaw(&zeroVec, 4);
        }
    }
    g_phase2CapturedHalfTypePtr = 0;
    TEST_CHECK(CargoSettings_ApplyCommand(pkt.data(), pkt.size()));
    TEST_CHECK(g_phase2CapturedHalfTypePtr == (U64)(clientTypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE));

    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase2_OrbitsDtaSaveZipRoundTrip()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path tmpRoot = fs::path(g_mockBaseDir) / "phase2_orbits_zip";
    fs::path srcDir = tmpRoot / "save_src";
    fs::path dstDir = tmpRoot / "save_dst";
    fs::path zipPath = tmpRoot / "save.zip";
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(srcDir, ec);

    // Build exact SOVIET64 orbits.dta payload: float clock + int32 count(2) + 2 x 0x60-byte slots
    std::vector<BYTE> orbitsBytes(4u + 4u + 2u * ORBIT_SLOT_STRIDE, 0);
    *(float*)(orbitsBytes.data() + 0) = 123.75f;
    *(int32_t*)(orbitsBytes.data() + 4) = 2;
    for (size_t s = 0; s < 2u; ++s) {
        BYTE* slot = orbitsBytes.data() + 8u + s * ORBIT_SLOT_STRIDE;
        *(float*)(slot + 0x00) = (float)(200 + s * 100);
        *(int32_t*)(slot + ORBIT_SLOT_TYPE_OFFSET) = (int32_t)(s + 1);
        *(float*)(slot + ORBIT_SLOT_START_CLOCK_OFFSET) = (float)(10.5f + s * 5.25f);
    }

    auto writeBin = [](const fs::path& p, const void* data, size_t len) -> bool {
        FILE* f = nullptr;
        if (_wfopen_s(&f, p.wstring().c_str(), L"wb") != 0 || !f) return false;
        size_t wr = fwrite(data, 1, len, f);
        fclose(f);
        return wr == len;
    };
    const char hdr[] = "HDR";
    TEST_CHECK(writeBin(srcDir / "header.bin", hdr, sizeof(hdr)));
    TEST_CHECK(writeBin(srcDir / "orbits.dta", orbitsBytes.data(), orbitsBytes.size()));

    TEST_CHECK(ZipSaveDirectory(srcDir.string().c_str(), zipPath.string().c_str(), nullptr, nullptr));
    TEST_CHECK(UnzipToDirectory(zipPath.string().c_str(), dstDir.string().c_str(), nullptr, nullptr));

    FILE* fIn = nullptr;
    TEST_CHECK(_wfopen_s(&fIn, (dstDir / "orbits.dta").wstring().c_str(), L"rb") == 0 && fIn != nullptr);
    std::vector<BYTE> roundTrip(orbitsBytes.size(), 0);
    size_t rd = fread(roundTrip.data(), 1, roundTrip.size(), fIn);
    int extraByte = fgetc(fIn);
    fclose(fIn);

    TEST_CHECK(rd == orbitsBytes.size() && extraByte == EOF);
    TEST_CHECK(memcmp(roundTrip.data(), orbitsBytes.data(), orbitsBytes.size()) == 0);
    fs::remove_all(tmpRoot, ec);
    return true;
}

bool Test_SpaceProgramPhase2_DeterminismOrbitsSoftSyncMixedDlc()
{
    return SubTest_Phase2_PresentationRngSites() &&
           SubTest_Phase2_StateHashSpaceAndMixedDlcMilestones() &&
           SubTest_Phase2_VehicleSoftSyncSpaceExclusion() &&
           SubTest_Phase2_MixedDlcWorldRefsShift119() &&
           SubTest_Phase2_OrbitsDtaSaveZipRoundTrip();
}

// ---------------------------------------------------------------------------
// Test 72: Space Program DLC Phase 3 (4 Space kSites, Zero-Pre-Mutation
//          Deorbit/Reentry/Abort, Pad Switches, VehicleSwitches, Camera+0x14)
// ---------------------------------------------------------------------------

static int g_p3HasReentryCalls = 0;
static int g_p3SetupReentryCalls = 0;
static int g_p3SetupDeorbitCalls = 0;
static unsigned char g_p3HasReentryRet = 1;

static int g_p3LaunchCalls = 0;
static int g_p3AttachCalls = 0;
static int g_p3DetachCalls = 0;
static uint32_t g_p3SeenPadWords[5] = {};
static int g_p3ShiftCancelOrigCalls = 0;
static BYTE* g_p3AutoReplaceSeenTarget = nullptr;
static BYTE* g_p3AutoReplaceSeenNew = nullptr;

static void __fastcall Phase3_MockLaunchRocket(U64 veh, int)
{
    ++g_p3LaunchCalls;
    U64 curBld = 0;
    if (IsPointerLikeX64(veh) && SafeReadPod((const U64*)(veh + VEH_CUR_BLD_OFFSET), &curBld) && IsPointerLikeX64(curBld)) {
        SafeReadPod((const uint32_t*)(curBld + BLD_LAUNCH_PAD_ORBIT_TYPE_OFF), &g_p3SeenPadWords[0]);
        SafeMemcpyPod(&g_p3SeenPadWords[1], (const void*)(curBld + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF), 16u);
    }
    // Simulate vanilla 0x698CF1..0x698E88 + 0x17BB0 camera hijack on launch
    BYTE* cam = g_mockExeBuffer + CAMERA_RVA;
    *(float*)(cam + CAMERA_POS_OFFSET) = 9876.0f;
    BYTE* camMgr = *(BYTE**)(g_mockExeBuffer + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF);
    if (camMgr && IsPointerLikeX64((U64)(uintptr_t)camMgr)) {
        camMgr[CAM_MGR_FOLLOW_ACTIVE_OFF] = 1;
        *(U64*)(camMgr + CAM_MGR_RIDE_VEH_PTR_OFF) = veh;
        BYTE* pid0 = *(BYTE**)(camMgr + CAM_MGR_PID_PTRS_OFFSET);
        if (pid0 && IsPointerLikeX64((U64)(uintptr_t)pid0)) {
            *(uint64_t*)(pid0 + 0x58) = 0xDEADBEEFULL;
        }
    }
}

static void __fastcall Phase3_MockOrbitAttach(U64, U64, int)
{
    ++g_p3AttachCalls;
}

static void __fastcall Phase3_MockOrbitDetach(U64)
{
    ++g_p3DetachCalls;
}

static void __fastcall Phase3_MockShiftCancelOrig(__int64)
{
    ++g_p3ShiftCancelOrigCalls;
}

static void __fastcall Phase3_MockSetAutoReplace(BYTE*, BYTE* targetType, BYTE* newType)
{
    g_p3AutoReplaceSeenTarget = targetType;
    g_p3AutoReplaceSeenNew = newType;
}

static unsigned char __fastcall Phase3_MockHasReentry(unsigned long long)
{
    ++g_p3HasReentryCalls;
    return g_p3HasReentryRet;
}

static void __fastcall Phase3_MockSetupReentry(unsigned long long)
{
    ++g_p3SetupReentryCalls;
}

static void __fastcall Phase3_MockSetupDeorbit(unsigned long long)
{
    ++g_p3SetupDeorbitCalls;
}

static void Phase3_ResetSpaceActionMockCounters(unsigned char hasReentryRet)
{
    g_p3HasReentryCalls = 0;
    g_p3SetupReentryCalls = 0;
    g_p3SetupDeorbitCalls = 0;
    g_p3HasReentryRet = hasReentryRet;
}

static bool SubTest_Phase3_SpaceKSitesAndPreconditions()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    double initRub[2] = { 1000000.0, 1000000.0 };
    double initUsd[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, FN_SPACE_LAUNCH_ROCKET_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA, FN_SPACE_LAUNCH_ROCKET_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_SPACE_ORBIT_ATTACH_RVA, FN_SPACE_ORBIT_ATTACH_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_SPACE_ORBIT_DETACH_RVA, FN_SPACE_ORBIT_DETACH_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA), "VI") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA), "VI") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_SPACE_ORBIT_ATTACH_RVA), "VVI") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_SPACE_ORBIT_DETACH_RVA), "V") == 0);

    // Install real executable mock handlers at Space target RVAs so preconditions prove invocation vs skip
    g_p3LaunchCalls = g_p3AttachCalls = g_p3DetachCalls = 0;
    memset(g_p3SeenPadWords, 0, sizeof(g_p3SeenPadWords));
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_LAUNCH_ROCKET_RVA, (void*)&Phase3_MockLaunchRocket);
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_ORBIT_ATTACH_RVA, (void*)&Phase3_MockOrbitAttach);
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_ORBIT_DETACH_RVA, (void*)&Phase3_MockOrbitDetach);

    // Setup mock camera + camera manager + 7 PID structs (0x78 bytes each) for remote launch camera isolation
    std::vector<BYTE> camMgr(CAM_MGR_STRUCT_BYTES, 0);
    std::vector<std::vector<BYTE>> pids(CAM_MGR_PID_COUNT, std::vector<BYTE>(CAM_MGR_PID_STRUCT_BYTES, 0));
    for (int i = 0; i < CAM_MGR_PID_COUNT; ++i) {
        *(uint64_t*)(pids[i].data() + 0x58) = 0x11110000ULL + (uint64_t)i;
        *(BYTE**)(camMgr.data() + CAM_MGR_PID_PTRS_OFFSET + (size_t)i * sizeof(BYTE*)) = pids[i].data();
    }
    *(BYTE**)(world + WORLD_CAMERA_MGR_PTR_OFF) = camMgr.data();
    *(float*)(g_mockExeBuffer + CAMERA_RVA + CAMERA_POS_OFFSET) = 321.5f;

    std::vector<BYTE> padType(0x400u, 0);
    *(int32_t*)(padType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD;
    *(int32_t*)(padType.data() + BLD_TYPE_SUBTYPE_OFF) = (int32_t)BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL;

    std::vector<BYTE> pad(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)(pad.data() + 0x00) = 6001u;
    *(BYTE**)(pad.data() + BLD_TYPE_OFFSET) = padType.data();
    *(float*)(pad.data() + OBJ_POS_OFFSET) = -300.0f;
    *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_TYPE_OFF) = 2u;
    *(float*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 0u) = 51.6f;
    *(float*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 4u) = 12.5f;
    *(float*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 8u) = 350.0f;
    *(float*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 12u) = 300.0f;

    std::vector<BYTE> rocket(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> station(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> foreignRocket(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> foreignStation(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(rocket.data() + VEH_ID_OFFSET) = 5001u;
    *(float*)(rocket.data() + VEH_POS_OFFSET) = 100.0f;
    *(U64*)(rocket.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad.data();
    *(int32_t*)(rocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 0;

    *(unsigned*)(station.data() + VEH_ID_OFFSET) = 5002u;
    *(float*)(station.data() + VEH_POS_OFFSET) = 100.0f;
    *(int32_t*)(station.data() + VEH_ORBIT_INDEX_OFFSET) = 0;
    *(int32_t*)(station.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;

    *(unsigned*)(foreignRocket.data() + VEH_ID_OFFSET) = 5003u;
    *(float*)(foreignRocket.data() + VEH_POS_OFFSET) = 400.0f;
    *(U64*)(foreignRocket.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad.data();
    *(int32_t*)(foreignRocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;

    *(unsigned*)(foreignStation.data() + VEH_ID_OFFSET) = 5004u;
    *(float*)(foreignStation.data() + VEH_POS_OFFSET) = 400.0f;
    *(int32_t*)(foreignStation.data() + VEH_ORBIT_INDEX_OFFSET) = 0;
    *(int32_t*)(foreignStation.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;

    std::vector<BYTE*> vehList = { rocket.data(), station.data(), foreignRocket.data(), foreignStation.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehList);

    std::vector<BYTE> orbits(ORBIT_SLOT_STRIDE, 0);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbits.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbits.data() + orbits.size();

    TwoStates_RegisterVehicleOwner(5001, 1);
    TwoStates_RegisterVehicleOwner(5002, 1);
    TwoStates_RegisterVehicleOwner(5003, 2);
    TwoStates_RegisterVehicleOwner(5004, 2);

    // Two-States ownership enforcement on Space sites (including (ownCraft, foreignStation))
    U64 launchOkArgs[7] = { (U64)(uintptr_t)rocket.data(), (U64)(unsigned)-1, 0, 0, 0, 0, 0 };
    U64 launchForeignArgs[7] = { (U64)(uintptr_t)foreignRocket.data(), (U64)(unsigned)-1, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchOkArgs));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchForeignArgs));

    U64 attachOkArgs[7] = { (U64)(uintptr_t)rocket.data(), (U64)(uintptr_t)station.data(), 2, 0, 0, 0, 0 };
    U64 attachForeignCraft[7] = { (U64)(uintptr_t)foreignRocket.data(), (U64)(uintptr_t)station.data(), 2, 0, 0, 0, 0 };
    U64 attachForeignStation[7] = { (U64)(uintptr_t)rocket.data(), (U64)(uintptr_t)foreignStation.data(), 2, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachOkArgs));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachForeignCraft));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachForeignStation));

    U64 detachOkArgs[7] = { (U64)(uintptr_t)rocket.data(), 0, 0, 0, 0, 0, 0 };
    U64 detachForeignArgs[7] = { (U64)(uintptr_t)foreignRocket.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_DETACH_RVA, detachOkArgs));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_DETACH_RVA, detachForeignArgs));

    // Item #11 + #12: Launch captures 5 pad orbit dwords, applies them before 0x698720 even after clamping,
    // and preserves local camera + PIDs when replaying a REMOTE launch!
    BYTE msg[256] = {};
    size_t msgSize = 0;
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchOkArgs, msg, sizeof(msg), &msgSize));
    // Simulate Mission Control clamping pad fields on non-click frames before boundary
    *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_TYPE_OFF) = 0u;
    *(float*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 0u) = 0.0f;
    *(float*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 8u) = 100.0f;

    TwoStates_BeginCommand(1);
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    Commands_EndRemoteReplayScope();
    TwoStates_EndCommand(1);
    TEST_CHECK(g_p3LaunchCalls == 1);
    TEST_CHECK(g_p3SeenPadWords[0] == 2u);
    float seenIncl = 0.0f, seenApo = 0.0f;
    memcpy(&seenIncl, &g_p3SeenPadWords[1], 4);
    memcpy(&seenApo, &g_p3SeenPadWords[3], 4);
    TEST_CHECK_NEAR(seenIncl, 51.6f, 1e-4f);
    TEST_CHECK_NEAR(seenApo, 350.0f, 1e-4f);
    // Remote replay MUST restore CAMERA_RVA, camMgr, and all 7 PID structs (no camera hijack)
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + CAMERA_RVA + CAMERA_POS_OFFSET), 321.5f, 1e-4f);
    TEST_CHECK(camMgr[CAM_MGR_FOLLOW_ACTIVE_OFF] == 0);
    TEST_CHECK(*(U64*)(camMgr.data() + CAM_MGR_RIDE_VEH_PTR_OFF) == 0);
    TEST_CHECK(*(uint64_t*)(pids[0].data() + 0x58) == 0x11110000ULL);

    // Local replay (not remote): camera follow IS allowed to activate
    TwoStates_BeginCommand(1);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TwoStates_EndCommand(1);
    TEST_CHECK(g_p3LaunchCalls == 2);
    TEST_CHECK(camMgr[CAM_MGR_FOLLOW_ACTIVE_OFF] == 1);
    TEST_CHECK(*(U64*)(camMgr.data() + CAM_MGR_RIDE_VEH_PTR_OFF) == (U64)(uintptr_t)rocket.data());

    // Invalid launches MUST NOT invoke 0x698720:
    // (a) foreign owner on replay, (b) double-click flightState!=0 or orbitIdx!=-1, (c) off-pad, (d) OOB orbit
    TwoStates_BeginCommand(2);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TwoStates_EndCommand(2);
    TEST_CHECK(g_p3LaunchCalls == 2);

    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TEST_CHECK(g_p3LaunchCalls == 2);
    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 0;
    *(int32_t*)(rocket.data() + VEH_ORBIT_INDEX_OFFSET) = 0;
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TEST_CHECK(g_p3LaunchCalls == 2);
    *(int32_t*)(rocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;

    *(U64*)(rocket.data() + VEH_CUR_BLD_OFFSET) = 0;
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TEST_CHECK(g_p3LaunchCalls == 2);
    *(U64*)(rocket.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad.data();

    U64 launchOob[7] = { (U64)(uintptr_t)rocket.data(), 99u, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA, launchOob, msg, sizeof(msg), &msgSize));
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TEST_CHECK(g_p3LaunchCalls == 2);

    // Attach preconditions: mismatch orbit, flightState!=1, dockPartner!=0, negative slot, (ownCraft, foreignStation)
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachOkArgs, msg, sizeof(msg), &msgSize));
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize)); // rocket orbit (-1) != station orbit (0)
    TEST_CHECK(g_p3AttachCalls == 0);
    *(int32_t*)(rocket.data() + VEH_ORBIT_INDEX_OFFSET) = 0;
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize)); // flightState == 0 != 1
    TEST_CHECK(g_p3AttachCalls == 0);
    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;
    *(U64*)(rocket.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) = (U64)(uintptr_t)station.data();
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize)); // dockPartner != 0
    TEST_CHECK(g_p3AttachCalls == 0);
    *(U64*)(rocket.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) = 0;
    // Valid attach -> MUST invoke 0x693B00!
    TwoStates_BeginCommand(1);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TwoStates_EndCommand(1);
    TEST_CHECK(g_p3AttachCalls == 1);
    // Replay-side (ownCraft, foreignStation) -> MUST NOT invoke 0x693B00!
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachForeignStation, msg, sizeof(msg), &msgSize));
    TwoStates_BeginCommand(1);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TwoStates_EndCommand(1);
    TEST_CHECK(g_p3AttachCalls == 1);

    // Detach preconditions: (a) link==0 && dockPartner==0 skipped, (b) approach-only link==0 && dockPartner!=0 skipped without invoking 0x693D40 ([P2] fix),
    // (c) docked link!=0 invokes 0x693D40, (d) foreign craft skipped on replay
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_SPACE_ORBIT_DETACH_RVA, detachOkArgs, msg, sizeof(msg), &msgSize));
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TEST_CHECK(g_p3DetachCalls == 0);
    *(U64*)(rocket.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) = (U64)(uintptr_t)station.data();
    *(int32_t*)(rocket.data() + VEH_SPACE_DOCK_SLOT_OFFSET) = 2;
    rocket[VEH_SPACE_DOCK_APPROACH_FLAG_OFF] = 1;
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TEST_CHECK(g_p3DetachCalls == 0);
    TEST_CHECK(*(U64*)(rocket.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) == (U64)(uintptr_t)station.data());
    *(U64*)(rocket.data() + VEH_LINK_PTR_OFFSET) = (U64)(uintptr_t)station.data();
    TwoStates_BeginCommand(2);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize)); // foreign owner skipped
    TwoStates_EndCommand(2);
    TEST_CHECK(g_p3DetachCalls == 0);
    TwoStates_BeginCommand(1);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize)); // valid docked detach invokes 0x693D40!
    TwoStates_EndCommand(1);
    TEST_CHECK(g_p3DetachCalls == 1);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase3_SpaceKSitesSpLobbyAndClientTwoStates()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    g_p3LaunchCalls = g_p3AttachCalls = g_p3DetachCalls = 0;
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_LAUNCH_ROCKET_RVA, (void*)&Phase3_MockLaunchRocket);
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_ORBIT_ATTACH_RVA, (void*)&Phase3_MockOrbitAttach);
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_ORBIT_DETACH_RVA, (void*)&Phase3_MockOrbitDetach);

    std::vector<BYTE> padType(0x400u, 0);
    *(int32_t*)(padType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD;
    *(int32_t*)(padType.data() + BLD_TYPE_SUBTYPE_OFF) = (int32_t)BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL;

    std::vector<BYTE> pad(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(pad.data() + BLD_TYPE_OFFSET) = padType.data();
    std::vector<BYTE> rocket(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> station(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(rocket.data() + VEH_ID_OFFSET) = 5201u;
    *(U64*)(rocket.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad.data();
    *(int32_t*)(rocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(unsigned*)(station.data() + VEH_ID_OFFSET) = 5202u;
    *(int32_t*)(station.data() + VEH_ORBIT_INDEX_OFFSET) = 0;
    std::vector<BYTE*> vehList = { rocket.data(), station.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehList);

    U64 launchNew[7] = { (U64)(uintptr_t)rocket.data(), (U64)(unsigned)-1, 0, 0, 0, 0, 0 };
    U64 launchCatch[7] = { (U64)(uintptr_t)rocket.data(), 0, 0, 0, 0, 0, 0 };
    U64 attachArgs[7] = { (U64)(uintptr_t)rocket.data(), (U64)(uintptr_t)station.data(), 1, 0, 0, 0, 0 };
    U64 detachArgs[7] = { (U64)(uintptr_t)rocket.data(), 0, 0, 0, 0, 0, 0 };

    // 1. Single-Player passthrough: all 4 Space kSites invoke native target directly with 0 Lockstep commands
    Commands_SetSession(COMMANDS_OFF);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchNew, nullptr));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA, launchCatch, nullptr));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachArgs, nullptr));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_ORBIT_DETACH_RVA, detachArgs, nullptr));
    TEST_CHECK(g_p3LaunchCalls == 2 && g_p3AttachCalls == 1 && g_p3DetachCalls == 1);
    TEST_CHECK(!Commands_Due(0));

    // 2. Waiting Lobby blocks all 4 Space kSites without invoking target or submitting Lockstep commands
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchNew, nullptr));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA, launchCatch, nullptr));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachArgs, nullptr));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SPACE_ORBIT_DETACH_RVA, detachArgs, nullptr));
    TEST_CHECK(g_p3LaunchCalls == 2 && g_p3AttachCalls == 1 && g_p3DetachCalls == 1);
    TEST_CHECK(!Commands_Due(0));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");

    // 3. Client-side (State 2) Two-States ownership enforcement (including (ownCraft, foreignStation))
    double initRub[2] = { 1000000.0, 1000000.0 };
    double initUsd[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();
    TwoStates_RegisterVehicleOwner(5201, 2); // Client's own craft
    TwoStates_RegisterVehicleOwner(5202, 1); // Host's foreign station

    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchNew));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachArgs)); // (ownCraft, foreignStation)
    U64 hostDetach[7] = { (U64)(uintptr_t)station.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_DETACH_RVA, hostDetach));
    TwoStates_RegisterVehicleOwner(5202, 2);
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_ORBIT_ATTACH_RVA, attachArgs));

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase3_SpaceActionZeroPreMutationAndReplay()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> reentryType(WORLD_VEHICLE_TYPE_STRIDE, 0);
    reentryType[VEH_TYPE_SPACECRAFT_FLAG_OFFSET] = 1;
    reentryType[VEH_TYPE_SPACE_REENTRY_FLAG_OFFSET] = 1; // +0x29A $SPACE_SHIP_REENTRY
    std::vector<BYTE> nonReentryType(WORLD_VEHICLE_TYPE_STRIDE, 0);
    nonReentryType[VEH_TYPE_SPACECRAFT_FLAG_OFFSET] = 1; // +0x298 only, +0x29A == 0

    std::vector<BYTE> rocket(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> foreignRocket(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(rocket.data() + VEH_ID_OFFSET) = 5101u;
    *(float*)(rocket.data() + VEH_POS_OFFSET) = 100.0f;
    *(BYTE**)(rocket.data() + VEH_TYPE_PTR_OFFSET) = reentryType.data();
    *(float*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET) = 0.25f;
    *(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = 3;
    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;

    *(unsigned*)(foreignRocket.data() + VEH_ID_OFFSET) = 5102u;
    *(float*)(foreignRocket.data() + VEH_POS_OFFSET) = 400.0f;
    *(BYTE**)(foreignRocket.data() + VEH_TYPE_PTR_OFFSET) = reentryType.data();
    *(float*)(foreignRocket.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(int32_t*)(foreignRocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = 3;
    *(int32_t*)(foreignRocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;

    std::vector<BYTE*> vehList = { rocket.data(), foreignRocket.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehList);

    Commands_SetSession(COMMANDS_HOST);
    GameCalls_SetSpaceActionFnsForTest(&Phase3_MockHasReentry, &Phase3_MockSetupReentry, &Phase3_MockSetupDeorbit);

    // 1. Targeted Reentry: ZERO pre-mutation at capture, applied only at Commands_Boundary
    Phase3_ResetSpaceActionMockCounters(1);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    TEST_CHECK_NEAR(*(float*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET), 0.25f, 1e-6f);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == 3);
    TEST_CHECK(g_p3HasReentryCalls == 0 && g_p3SetupReentryCalls == 0 && g_p3SetupDeorbitCalls == 0);
    TEST_CHECK(Commands_Due(10));
    Commands_Boundary(10);
    TEST_CHECK(*(uint32_t*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET) == 0x3F7FBE77u);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == -2);
    TEST_CHECK(g_p3HasReentryCalls == 1 && g_p3SetupReentryCalls == 1 && g_p3SetupDeorbitCalls == 1);

    // Duplicate reentry while already reentering (progress < 1.0f && targetOrbit == -2) MUST be skipped
    Phase3_ResetSpaceActionMockCounters(1);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    Commands_Boundary(10);
    TEST_CHECK(g_p3HasReentryCalls == 0 && g_p3SetupReentryCalls == 0 && g_p3SetupDeorbitCalls == 0);

    // Non-reentry craft (+0x29A == 0) MUST be rejected for targeted reentry
    *(float*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = 2;
    *(BYTE**)(rocket.data() + VEH_TYPE_PTR_OFFSET) = nonReentryType.data();
    Phase3_ResetSpaceActionMockCounters(1);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    Commands_Boundary(10);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == 2 && g_p3SetupDeorbitCalls == 0);
    *(BYTE**)(rocket.data() + VEH_TYPE_PTR_OFFSET) = reentryType.data();

    // 2. Simple Deorbit (flightState == 1, hasReentry == 0 -> SetupReentry skipped, SetupDeorbit called)
    *(float*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET) = 0.50f;
    *(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = 2;
    Phase3_ResetSpaceActionMockCounters(0);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 0));
    TEST_CHECK_NEAR(*(float*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET), 0.50f, 1e-6f);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == 2);
    Commands_Boundary(11);
    TEST_CHECK(*(uint32_t*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET) == 0x3F7FBE77u);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == -1);
    TEST_CHECK(g_p3HasReentryCalls == 1 && g_p3SetupReentryCalls == 0 && g_p3SetupDeorbitCalls == 1);

    // Duplicate deorbit while already deorbiting (progress < 1.0f && targetOrbit == -1) MUST be skipped
    Phase3_ResetSpaceActionMockCounters(0);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 0));
    Commands_Boundary(11);
    TEST_CHECK(g_p3SetupDeorbitCalls == 0);

    // Targeted Reentry while ALREADY deorbiting (progress < 1.0f && targetOrbit == -1) MUST ALSO be skipped (0x816C40)
    Phase3_ResetSpaceActionMockCounters(1);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    Commands_Boundary(11);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == -1 && g_p3SetupDeorbitCalls == 0);

    // 3. Emergency Abort: rejected while on Launch Pad (curBld != 0) or carried (parent != 0) per 0x813BB6..0x813BE0;
    //    succeeds when airborne (flightState == 0, curBld == 0, parent == 0, progress == 1.0f, variant == 1 -> 11)
    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 0;
    *(float*)(rocket.data() + VEH_ROCKET_PROGRESS_OFFSET) = 1.0f;
    *(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = -1;
    std::vector<BYTE> dummyPad(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(rocket.data() + VEH_CUR_BLD_OFFSET) = dummyPad.data();
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 1));
    Commands_Boundary(12);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) == 0);
    *(BYTE**)(rocket.data() + VEH_CUR_BLD_OFFSET) = nullptr;
    *(BYTE**)(rocket.data() + VEH_PARENT_PTR_OFFSET) = foreignRocket.data();
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 1));
    Commands_Boundary(12);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) == 0);
    *(BYTE**)(rocket.data() + VEH_PARENT_PTR_OFFSET) = nullptr;

    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 1));
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) == 0);
    Commands_Boundary(12);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) == 11);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 0));
    Commands_Boundary(13);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) == 11);

    // 4. Stale reentry blocked if trajectory (+0x738) non-empty, docked (+0x4A0 != 0), or docked children (+0x488..+0x490)
    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;
    *(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) = 4;
    BYTE dummyTraj[64] = {};
    *(U64*)(rocket.data() + VEH_WAYPOINTS_BEGIN_OFFSET) = (U64)(uintptr_t)dummyTraj;
    *(U64*)(rocket.data() + VEH_WAYPOINTS_BEGIN_OFFSET + 8) = (U64)(uintptr_t)(dummyTraj + 16);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    Commands_Boundary(14);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == 4);
    *(U64*)(rocket.data() + VEH_WAYPOINTS_BEGIN_OFFSET) = 0;
    *(U64*)(rocket.data() + VEH_WAYPOINTS_BEGIN_OFFSET + 8) = 0;
    *(U64*)(rocket.data() + VEH_LINK_PTR_OFFSET) = (U64)(uintptr_t)foreignRocket.data();
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    Commands_Boundary(15);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == 4);
    *(U64*)(rocket.data() + VEH_LINK_PTR_OFFSET) = 0;
    // Docked children vector (+0x488..+0x490) >= 8 bytes MUST block reentry
    U64 childSlot[1] = { (U64)(uintptr_t)foreignRocket.data() };
    *(U64*)(rocket.data() + VEH_CARRIED_BEGIN_OFFSET) = (U64)(uintptr_t)childSlot;
    *(U64*)(rocket.data() + VEH_CARRIED_END_OFFSET) = (U64)(uintptr_t)(childSlot + 1);
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    Commands_Boundary(15);
    TEST_CHECK(*(int32_t*)(rocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == 4);
    *(U64*)(rocket.data() + VEH_CARRIED_BEGIN_OFFSET) = 0;
    *(U64*)(rocket.data() + VEH_CARRIED_END_OFFSET) = 0;

    // 5. Two-States (capture & replay) + Waiting Lobby block with zero mutation
    double initRub[2] = { 1000000.0, 1000000.0 };
    double initUsd[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();
    TwoStates_RegisterVehicleOwner(5101, 1);
    TwoStates_RegisterVehicleOwner(5102, 2);
    TEST_CHECK(!GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)foreignRocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 0));
    TEST_CHECK(!Commands_Due(16));

    // Replay-side ownership validation: State 1 command targeting State 2 foreignRocket MUST be ignored
    struct TestSpaceActionWire { RouteRef veh; uint8_t kind; uint8_t var; uint16_t res; } foreignCmd = {};
    TEST_CHECK(Ref_Vehicle((U64)(uintptr_t)foreignRocket.data(), foreignCmd.veh));
    foreignCmd.kind = SPACE_ACT_REENTRY_TARGETED;
    TwoStates_BeginCommand(1);
    TEST_CHECK(GameCalls_ApplySpaceActionCommand(&foreignCmd, sizeof(foreignCmd)));
    TwoStates_EndCommand(1);
    TEST_CHECK(*(int32_t*)(foreignRocket.data() + VEH_TARGET_ORBIT_INDEX_OFFSET) == 3);

    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(!GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_REENTRY_TARGETED, 0));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");

    GameCalls_SetSpaceActionFnsForTest(nullptr, nullptr, nullptr);
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase3_SpaceActionPatchBytesAndJumpTargets()
{
    ResetMockWorldMemory();
    static const BYTE kReentryOrig[22] = {
        0x41, 0xC7, 0x87, 0x68, 0x1A, 0x00, 0x00, 0x77, 0xBE, 0x7F, 0x3F,
        0x41, 0xC7, 0x87, 0x60, 0x17, 0x00, 0x00, 0xFE, 0xFF, 0xFF, 0xFF
    };
    static const BYTE kDeorbitOrig[22] = {
        0x41, 0x83, 0xBF, 0x6C, 0x1A, 0x00, 0x00, 0x01, 0x75, 0x1B,
        0x41, 0xC7, 0x87, 0x68, 0x1A, 0x00, 0x00, 0x77, 0xBE, 0x7F, 0x3F, 0x41
    };
    BYTE* pReentry = g_mockExeBuffer + SITE_SPACE_REENTRY_PATCH_RVA;
    BYTE* pDeorbit = g_mockExeBuffer + SITE_SPACE_DEORBIT_ABORT_PATCH_RVA;
    BYTE* pRelaySlot = g_mockExeBuffer + 0x7E0000u;

    // Atomic check: if either site differs, NEITHER site may be half-patched
    memcpy(pReentry, kReentryOrig, 22);
    memcpy(pDeorbit, kDeorbitOrig, 22);
    pReentry[0] = 0xCC;
    TEST_CHECK(!GameCalls_InstallSpaceActionPatchesForTest(pRelaySlot));
    TEST_CHECK(pReentry[0] == 0xCC);
    TEST_CHECK(memcmp(pDeorbit, kDeorbitOrig, 22) == 0);

    // Exact bytes at both sites -> 2/2 patched with exact mov rcx, r15; mov edx, kind; call; jmp 0x819815; nop*4
    memcpy(pReentry, kReentryOrig, 22);
    memcpy(pDeorbit, kDeorbitOrig, 22);
    TEST_CHECK(GameCalls_InstallSpaceActionPatchesForTest(pRelaySlot));

    const BYTE* sites[2] = { pReentry, pDeorbit };
    const uint32_t kinds[2] = { SPACE_SITE_REENTRY_TARGETED, SPACE_SITE_DEORBIT_OR_ABORT };
    for (int i = 0; i < 2; ++i) {
        const BYTE* s = sites[i];
        TEST_CHECK(s[0] == 0x49 && s[1] == 0x8B && s[2] == 0xCF && s[3] == 0xBA);
        uint32_t kindImm = 0;
        memcpy(&kindImm, s + 4, 4);
        TEST_CHECK(kindImm == kinds[i]);
        TEST_CHECK(s[8] == 0xE8 && s[13] == 0xE9);
        TEST_CHECK(s[18] == 0x90 && s[19] == 0x90 && s[20] == 0x90 && s[21] == 0x90);

        int32_t callRel = 0, jmpRel = 0;
        memcpy(&callRel, s + 9, 4);
        memcpy(&jmpRel, s + 14, 4);
        const BYTE* relay = (s + 13) + callRel;
        const BYTE* jmpTgt = (s + 18) + jmpRel;
        TEST_CHECK(jmpTgt == g_mockExeBuffer + SITE_SPACE_VEH_WIN_EXIT_RVA);
        TEST_CHECK(relay[0] == 0xFF && relay[1] == 0x25);
        U64 absFn = 0;
        memcpy(&absFn, relay + 6, 8);
        TEST_CHECK(absFn != 0);
    }

    memset(pReentry, 0, 22);
    memset(pDeorbit, 0, 22);
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase3_LaunchPadOrbitSwitches()
{
    ResetMockWorldMemory();
    std::vector<BYTE> padType(0x400, 0);
    *(int32_t*)(padType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD; // 0x2F
    *(int32_t*)(padType.data() + BLD_TYPE_SUBTYPE_OFF) = (int32_t)BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL; // 0x2D
    std::vector<BYTE> bigPadType(0x400, 0);
    *(int32_t*)(bigPadType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD; // 0x2F
    *(int32_t*)(bigPadType.data() + BLD_TYPE_SUBTYPE_OFF) = (int32_t)BLD_SUBTYPE_SPACE_LAUNCH_PAD_BIG; // 0x30
    std::vector<BYTE> airportParkType(0x400, 0);
    *(int32_t*)(airportParkType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD; // 0x2F ($TYPE_AIRPLANE_PARKING)
    *(int32_t*)(airportParkType.data() + BLD_TYPE_SUBTYPE_OFF) = 0; // ordinary airport apron (NOT launch pad!)
    std::vector<BYTE> pollType(0x400, 0);
    *(int32_t*)(pollType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_POLLUTION_MONITOR; // 0x24

    std::vector<BYTE> pad(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)(pad.data() + 0x00) = 6201u;
    *(BYTE**)(pad.data() + 0x318) = padType.data();
    *(float*)(pad.data() + OBJ_POS_OFFSET) = 100.0f;
    *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_TYPE_OFF) = 1u;
    *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF) = 0x42B40000u; // 90.0f
    *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 4u) = 0x42340000u; // 45.0f

    std::vector<BYTE> bigPad(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)(bigPad.data() + 0x00) = 6203u;
    *(BYTE**)(bigPad.data() + 0x318) = bigPadType.data();

    std::vector<BYTE> airportPark(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)(airportPark.data() + 0x00) = 6204u;
    *(BYTE**)(airportPark.data() + 0x318) = airportParkType.data();

    std::vector<BYTE> pollBld(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)(pollBld.data() + 0x00) = 6202u;
    *(BYTE**)(pollBld.data() + 0x318) = pollType.data();

    // Launch Pads (0x2F + subtype 0x2D/0x30) whitelist 0x0CD8 and 0x0E34..0x0E40;
    // normal Airplane Parking (0x2F/0x00) and Pollution Monitor (0x24) MUST NOT whitelist 0x0E34..0x0E40!
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(BLD_LAUNCH_PAD_ORBIT_TYPE_OFF));
    TEST_CHECK(BuildingSwitches_IsNonPointerBuildingWordForTest(BLD_LAUNCH_PAD_ORBIT_TYPE_OFF, (U64)(uintptr_t)pad.data()));
    TEST_CHECK(BuildingSwitches_IsNeverNoiseBuildingWordForTest(BLD_LAUNCH_PAD_ORBIT_TYPE_OFF, (U64)(uintptr_t)pad.data()));
    for (unsigned off = BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF; off < BLD_LAUNCH_PAD_ORBIT_PARAMS_END_OFF; off += 4u) {
        TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(off));
        TEST_CHECK(BuildingSwitches_IsNonPointerBuildingWordForTest(off, (U64)(uintptr_t)pad.data()));
        TEST_CHECK(BuildingSwitches_IsNeverNoiseBuildingWordForTest(off, (U64)(uintptr_t)pad.data()));
        TEST_CHECK(BuildingSwitches_IsNonPointerBuildingWordForTest(off, (U64)(uintptr_t)bigPad.data()));
        TEST_CHECK(BuildingSwitches_IsNeverNoiseBuildingWordForTest(off, (U64)(uintptr_t)bigPad.data()));
        TEST_CHECK(!BuildingSwitches_IsNonPointerBuildingWordForTest(off, (U64)(uintptr_t)airportPark.data()));
        TEST_CHECK(!BuildingSwitches_IsNeverNoiseBuildingWordForTest(off, (U64)(uintptr_t)airportPark.data()));
        TEST_CHECK(!BuildingSwitches_IsNonPointerBuildingWordForTest(off, (U64)(uintptr_t)pollBld.data()));
        TEST_CHECK(!BuildingSwitches_IsNeverNoiseBuildingWordForTest(off, (U64)(uintptr_t)pollBld.data()));
        TEST_CHECK(!BuildingSwitches_IsNonPointerBuildingWordForTest(off, 0));
    }

    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE*> bldVec = { pad.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_HOST);

    // Non-click sampled frames changing 0xE34 on Launch Pad must NOT mark it as noise
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;
    for (int f = 0; f < 16; ++f) {
        BuildingSwitches_Before(false);
        if (f == 15) *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF) = 0x42700000u; // 60.0f
        BuildingSwitches_After(false);
    }

    // Click frame changing 0xCD8 and 0xE34 must revert immediately and apply at Commands_Boundary
    const uint32_t expectedBeforeClick = *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 1;
    BuildingSwitches_Before(false);
    *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_TYPE_OFF) = 2u;
    *(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF) = 0x43340000u; // 180.0f
    BuildingSwitches_After(false);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;

    TEST_CHECK(*(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_TYPE_OFF) == 1u);
    TEST_CHECK(*(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF) == expectedBeforeClick);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_TYPE_OFF) == 2u);
    TEST_CHECK(*(uint32_t*)(pad.data() + BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF) == 0x43340000u);

    Commands_SetSession(COMMANDS_OFF);
    BuildingSwitches_Reset();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase3_VehicleSwitchesSpaceExclusions()
{
    const unsigned kExcludedSpaceOffs[] = {
        0x0664u, 0x1418u,
        0x175Cu, 0x1760u, 0x1764u, 0x1768u, 0x176Cu, 0x1770u,
        0x1774u, 0x1790u, 0x17B4u,
        0x1860u, 0x1864u, 0x1938u, 0x193Cu,
        0x1A68u, 0x1A6Cu,
        0x1AD8u, 0x1AE0u, 0x1AF0u, 0x1B08u, 0x1B10u,
        0x1B50u, 0x1B60u, 0x1B74u, 0x1B78u, 0x1B80u, 0x1B94u
    };
    for (unsigned off : kExcludedSpaceOffs) {
        TEST_CHECK(VehicleSwitches_IsListWordForTest(0, off));
    }
    // Tight boundary checks outside newly excluded Space ranges
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(0, 0x0660u));
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(0, 0x0668u));
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(0, 0x185Cu));
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(0, 0x1868u));
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(0, 0x1934u));
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(0, 0x1940u));
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(0, 0x1B98u));
    return true;
}

static bool SubTest_Phase3_SpaceToolsAndWorldCamera14Isolation()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    Commands_SetSession(COMMANDS_HOST);

    static const char kOrbitalAttach[] = "orbital_attach";
    static const char kCatchOrbit[] = "catch_orbit";
    static const char kUncarriedVeh[] = "vehicle_unknown";

    TEST_CHECK(SimPatches_HasToolCallForTest(SITE_TOOL_ORBITAL_ATTACH_RVA, FN_TOOL_ORBITAL_ATTACH_RVA));

    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = kOrbitalAttach;
    Commands_PutAwayUncarriedToolForTest();
    TEST_CHECK(*(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) == kOrbitalAttach);

    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = kCatchOrbit;
    Commands_PutAwayUncarriedToolForTest();
    TEST_CHECK(*(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) == kCatchOrbit);

    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = kUncarriedVeh;
    Commands_PutAwayUncarriedToolForTest();
    TEST_CHECK(*(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) == nullptr);

    // WORLD+0x14 (local 3D Earth camera/orbit view) must never hash into StateSummary, enter BuildingSwitches,
    // be clobbered by remote command replay, or allow F6 flat-map camera jump
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE*> bldVec = { bld.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);
    *(float*)(world + WORLD_ORBIT_CLOCK_OFFSET) = 12.5f;
    std::vector<BYTE> orbit(ORBIT_SLOT_STRIDE, 0);
    *(int32_t*)(orbit.data() + ORBIT_SLOT_TYPE_OFFSET) = 2;
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbit.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbit.data() + orbit.size();

    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = -1;
    StateSummary sA = {};
    TEST_CHECK(StateHash_Compute(&sA));
    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = 0;
    StateSummary sB = {};
    TEST_CHECK(StateHash_Compute(&sB));
    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = 2;
    StateSummary sC = {};
    TEST_CHECK(StateHash_Compute(&sC));
    TEST_CHECK(sA.hashSpace == sB.hashSpace && sB.hashSpace == sC.hashSpace);
    TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(WORLD_ACTIVE_ORBIT_VIEW_OFFSET));
    TEST_CHECK(!Commands_Due(0));

    // Remote replay scope saves & restores WORLD+0x14
    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = 3;
    Commands_BeginRemoteReplayScope();
    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = -1;
    Commands_EndRemoteReplayScope();
    TEST_CHECK(*(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) == 3);

    // Non-tautological orbit-view suppression: in orbit view (WORLD+0x14 == 3), JumpCameraToPartner returns early
    // BEFORE emitting the "not connected" toast (0 toasts) and JumpCameraTo refuses; in ground view (-1), toast fires.
    TwoStates_ClearToastsForTest();
    TEST_CHECK(!TwoStates_JumpCameraToPartner());
    TEST_CHECK(TwoStates_GetToastCountForTest() == 0);
    TEST_CHECK(!TwoStates_JumpCameraTo(100.0f, 200.0f));
    *(int32_t*)(world + WORLD_ACTIVE_ORBIT_VIEW_OFFSET) = -1;
    TEST_CHECK(!TwoStates_JumpCameraToPartner());
    TEST_CHECK(TwoStates_GetToastCountForTest() == 1);
    TwoStates_ClearToastsForTest();

    Commands_SetSession(COMMANDS_OFF);
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase3_HardeningRegressions()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Item #1: Shift-Cancel uses v1.2.0.9 TOOL_GUARD1/2/3_OFFSET (0xFEC/0xFEE/0xFF3), NOT stale 0xFBC/0xFBE/0xFC3
    g_p3ShiftCancelOrigCalls = 0;
    WriteAbsJmp12(g_mockExeBuffer + 0x22F270u, (void*)&Phase3_MockShiftCancelOrig);
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "");
    Commands_SetSession(COMMANDS_HOST);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 1;
    world[TOOL_GUARD1_OFFSET] = 1;
    RoadSync_ShiftCancelTool((__int64)world);
    TEST_CHECK(!Commands_Due(0)); // blocked by v1.2.0.9 guard
    world[TOOL_GUARD1_OFFSET] = 0;
    world[0xFBC] = 1; // stale v1.1.1.13 offset must NOT suppress click
    RoadSync_ShiftCancelTool((__int64)world);
    TEST_CHECK(Commands_Due(0));
    Commands_Boundary(0);
    world[0xFBC] = 0;
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;
    Commands_SetSession(COMMANDS_OFF);

    // 2. Item #4: Mixed-DLC Auto-Replace resolves targetType by canonical targetScriptName across +-119 shift,
    //    strips "media_soviet/" so >64-char paths never truncate, and enforces DLC on both targetType & newType
    std::vector<BYTE> clientTypes(4u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    strcpy_s((char*)(clientTypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 128, "vehicles/kamaz_5320.ini");
    strcpy_s((char*)(clientTypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 128, "vehicles/tatra_148.ini");
    strcpy_s((char*)(clientTypes.data() + 2u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 128, "media_soviet/dlc5/vehicles/rocket_soyuz.ini");
    strcpy_s((char*)(clientTypes.data() + 3u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 128,
             "media_soviet/vehicles/heavy_tractor_k701_long_variant_name_1984/script.ini");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = clientTypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = clientTypes.data() + clientTypes.size();
    std::vector<BYTE> mockMgr(64, 0);
    *(BYTE**)(world + WORLD_AUTO_REPLACE_MGR_OFFSET) = mockMgr.data();
    g_p3AutoReplaceSeenTarget = g_p3AutoReplaceSeenNew = nullptr;
    WriteAbsJmp12(g_mockExeBuffer + RVA_SET_AUTO_REPLACE, (void*)&Phase3_MockSetAutoReplace);
    SaveSync_SetTestDlcLists("dlc1,dlc5", "dlc1"); // dlc5 unshared

    PurchaseMsg ar = {};
    ar.kind = 0xB;
    ar.typeIndex = 120u; // shifted Host index
    ar.resIndex = 119;   // shifted Host index (would be OOB on Client without targetScriptName!)
    strcpy_s(ar.scriptName, sizeof(ar.scriptName), "MEDIA_SOVIET\\VEHICLES\\TATRA_148.INI");
    strcpy_s(ar.targetScriptName, sizeof(ar.targetScriptName), "MEDIA_SOVIET\\VEHICLES\\KAMAZ_5320.INI");
    TEST_CHECK(Purchase_ApplyAutoReplaceForTest(ar));
    TEST_CHECK(g_p3AutoReplaceSeenTarget == clientTypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE);
    TEST_CHECK(g_p3AutoReplaceSeenNew == clientTypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE);

    // 74-char raw path ("media_soviet/...") normalizes via Ref_ReadVehicleTypeScriptName into 61 chars (< 64) cleanly!
    TEST_CHECK(Ref_ReadVehicleTypeScriptName((U64)(uintptr_t)(clientTypes.data() + 3u * WORLD_VEHICLE_TYPE_STRIDE),
                                             ar.targetScriptName, sizeof(ar.targetScriptName)));
    TEST_CHECK(Purchase_ApplyAutoReplaceForTest(ar));
    TEST_CHECK(g_p3AutoReplaceSeenTarget == clientTypes.data() + 3u * WORLD_VEHICLE_TYPE_STRIDE);

    // Unshared dlc5 targetType or newType MUST be refused on both replay and clear-submit
    strcpy_s(ar.targetScriptName, sizeof(ar.targetScriptName), "media_soviet/dlc5/vehicles/rocket_soyuz.ini");
    TEST_CHECK(!Purchase_ApplyAutoReplaceForTest(ar));
    TEST_CHECK(!Purchase_SubmitAutoReplaceClear(clientTypes.data() + 2u * WORLD_VEHICLE_TYPE_STRIDE));

    // 3. Item #6: CargoSettings decode failure MUST NOT call WriteRecordMeta or Replace
    const unsigned char sigDestroy[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xff, 0x48 };
    const unsigned char sigCopy[16]    = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe };
    const unsigned char sigRefresh[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 };
    memcpy(g_mockExeBuffer + 0x0F6520, sigDestroy, 16);
    memcpy(g_mockExeBuffer + 0x21FBF0, sigCopy, 16);
    memcpy(g_mockExeBuffer + 0x4BFC10, sigRefresh, 16);
    std::vector<BYTE> stopVeh(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> stopRec(0x198, 0);
    *(unsigned*)(stopVeh.data() + VEH_ID_OFFSET) = 4243u;
    *(U64*)(stopRec.data() + 0x190) = 0x12345678ULL;
    *(BYTE**)(stopVeh.data() + 0x638) = stopRec.data();
    *(BYTE**)(stopVeh.data() + 0x640) = stopRec.data() + 0x198;
    std::vector<BYTE*> vehList = { stopVeh.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehList);
    RouteRef stopRef = kNoRef, noTarget = kNoRef;
    TEST_CHECK(Ref_Vehicle((U64)(uintptr_t)stopVeh.data(), stopRef));
    WriteAbsJmp12(g_mockExeBuffer + 0x0F6520, (void*)&Phase2_MockCargoDestroy);
    WriteAbsJmp12(g_mockExeBuffer + 0x21FBF0, (void*)&Phase2_MockCargoCopy);
    // Build truncated/bad-decode packet with mutated recTail190 = 0x99999999: must return false & NOT overwrite +0x190
    std::vector<BYTE> badPkt;
    auto pushRaw = [&](const void* p, size_t n) { badPkt.insert(badPkt.end(), (const BYTE*)p, (const BYTE*)p + n); };
    unsigned magic = 0x32534743u, recTotal = 1u;
    BYTE kind = 0, half = 0, linkActive = 1, scalars[22] = {};
    unsigned short recIdx = 0;
    U64 newTail = 0x99999999ULL;
    pushRaw(&magic, 4); pushRaw(&kind, 1); pushRaw(&half, 1); pushRaw(&recIdx, 2); pushRaw(&recTotal, 4);
    pushRaw(&stopRef, sizeof(stopRef)); pushRaw(&noTarget, sizeof(noTarget)); pushRaw(&linkActive, 1); pushRaw(&newTail, 8);
    pushRaw(scalars, sizeof(scalars)); // intentionally omit vectors -> Decode fails
    g_phase2CapturedHalfTypePtr = 0xCAFEULL;
    TEST_CHECK(!CargoSettings_ApplyCommand(badPkt.data(), badPkt.size()));
    TEST_CHECK(*(U64*)(stopRec.data() + 0x190) == 0x12345678ULL); // WriteRecordMeta NOT called!
    TEST_CHECK(g_phase2CapturedHalfTypePtr == 0xCAFEULL);         // Replace NOT called!

    // 4. Item #7: Ref_FindBuildMenuRecord name-scan fallback when DLC shifts build-menu index
    std::vector<BYTE> menuBuf(3u * 0x2D0u, 0);
    strcpy_s((char*)(menuBuf.data() + 0u * 0x2D0u), 32, "terrain_raise");
    strcpy_s((char*)(menuBuf.data() + 1u * 0x2D0u), 32, "road_sign_stop");
    strcpy_s((char*)(menuBuf.data() + 2u * 0x2D0u), 32, "tree_pine");
    *(BYTE**)(world + 0xD320) = menuBuf.data();
    *(BYTE**)(world + 0xD328) = menuBuf.data() + menuBuf.size();
    TEST_CHECK(Ref_FindBuildMenuRecord((U64)(uintptr_t)world, 1, "road_sign_stop", 32) == (U64)(uintptr_t)(menuBuf.data() + 0x2D0u));
    TEST_CHECK(Ref_FindBuildMenuRecord((U64)(uintptr_t)world, 0, "tree_pine", 32) == (U64)(uintptr_t)(menuBuf.data() + 2u * 0x2D0u));
    TEST_CHECK(Ref_FindBuildMenuRecord((U64)(uintptr_t)world, 99, "road_sign_stop", 32) == (U64)(uintptr_t)(menuBuf.data() + 0x2D0u));
    TEST_CHECK(Ref_FindBuildMenuRecord((U64)(uintptr_t)world, 0, "missing_tool", 32) == 0);

    // 5. Item #17: Disconnect clears DLC session masks; Ref_DefPtr/Ref_DefPtrAt SEH-guards invalid waste vector
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_RequestDisconnect();
    uint32_t hMask = 1, cMask = 1, comMask = 1;
    SaveSync_GetDlcMasks(&hMask, &cMask, &comMask);
    TEST_CHECK(hMask == 0 && cMask == 0 && comMask == 0);

    *(U64*)(world + WORLD_WASTE_RESOURCES_OFFSET) = 0x10000000ULL;
    *(U64*)(world + WORLD_WASTE_RESOURCES_OFFSET + 8) = 0x10000020ULL;
    unsigned char tag = 0;
    unsigned idx = 0;
    U64 outPtr = 0;
    TEST_CHECK(!Ref_DefPtr(0x12345678ULL, tag, idx));
    TEST_CHECK(!Ref_DefPtrAt(4, 0, outPtr));

    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase3_CargoAndLineAcceptDlcFilter()
{
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    Commands_SetSession(COMMANDS_HOST);
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "");

    std::vector<BYTE> vtypes(2u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    strcpy_s((char*)(vtypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "vehicles/kamaz_5320.ini");
    strcpy_s((char*)(vtypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "media_soviet/dlc5/vehicles/rocket_soyuz.ini");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = vtypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = vtypes.data() + vtypes.size();
    BYTE* baseVt = vtypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* dlc5Vt = vtypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE;

    // 1. CargoSettings::Encode blocks unshared dlc5 VehicleType on vehicle stop half
    const unsigned char sigDestroy[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xff, 0x48 };
    const unsigned char sigCopy[16]    = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe };
    const unsigned char sigRefresh[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 };
    memcpy(g_mockExeBuffer + 0x0F6520, sigDestroy, 16);
    memcpy(g_mockExeBuffer + 0x21FBF0, sigCopy, 16);
    memcpy(g_mockExeBuffer + 0x4BFC10, sigRefresh, 16);

    std::vector<BYTE> stopVeh(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> stopRec(0x198, 0);
    *(unsigned*)(stopVeh.data() + VEH_ID_OFFSET) = 4299u;
    *(BYTE**)(stopVeh.data() + 0x638) = stopRec.data();
    *(BYTE**)(stopVeh.data() + 0x640) = stopRec.data() + 0x198;
    std::vector<BYTE*> vehList = { stopVeh.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehList);

    std::vector<BYTE> vehWin(0x600, 0);
    *(unsigned*)(vehWin.data() + 0x248) = 1u; // VEHICLE_WINDOW
    *(BYTE**)(vehWin.data() + 0x240) = stopVeh.data();
    std::vector<BYTE*> winList = { vehWin.data() };
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_OPEN_WINDOWS_BEGIN_RVA, winList);

    // Prime CargoSettings::Ready() before redirecting DESTROY/COPY to mocks
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 1;
    CargoSettings_Before(false);
    CargoSettings_After(false);
    WriteAbsJmp12(g_mockExeBuffer + 0x0F6520, (void*)&Phase2_MockCargoDestroy);
    WriteAbsJmp12(g_mockExeBuffer + 0x21FBF0, (void*)&Phase2_MockCargoCopy);

    SaveSync_SetTestDlcLists("dlc1,dlc5", "dlc1"); // dlc5 unshared
    U64 defSlot[1] = { (U64)(uintptr_t)dlc5Vt };
    CargoSettings_Before(false);
    *(U64*)(stopRec.data() + 0x68) = (U64)(uintptr_t)defSlot;
    *(U64*)(stopRec.data() + 0x70) = (U64)(uintptr_t)(defSlot + 1);
    CargoSettings_After(false);
    TEST_CHECK(!Commands_Due(0)); // blocked by SaveSync_CheckVehicleDlcAllowed

    // Shared dlc5 -> CargoSettings submits LS_CMD_CARGO_SETTINGS
    *(U64*)(stopRec.data() + 0x68) = 0;
    *(U64*)(stopRec.data() + 0x70) = 0;
    SaveSync_SetTestDlcLists("dlc1,dlc5", "dlc1,dlc5");
    CargoSettings_Before(false);
    *(U64*)(stopRec.data() + 0x68) = (U64)(uintptr_t)defSlot;
    *(U64*)(stopRec.data() + 0x70) = (U64)(uintptr_t)(defSlot + 1);
    CargoSettings_After(false);
    TEST_CHECK(Commands_Due(0));
    Commands_SetSession(COMMANDS_OFF);
    Commands_SetSession(COMMANDS_HOST);
    g_mockExeBuffer[SWITCH_PRESS_RVA] = 0;

    // 2. LineAccept::Encode blocks unshared dlc5 VehicleType in BOTH HALF_PAIRS (+0x08) and kHalfPtrs (+0x68)
    const unsigned char sigLCopy[16] = { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x11, 0x05, 0x00, 0x00, 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20 };
    const unsigned char sigLMerge[16] = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41 };
    const unsigned char sigLDirty[16] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 };
    const unsigned char sigLVeh[16]   = { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xf5, 0x02, 0x00, 0x00, 0x53, 0x41, 0x56, 0x48, 0x83, 0xec, 0x38 };
    const unsigned char sigLName[16]  = { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x78, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0xdc, 0x56, 0x48, 0x81, 0xec };
    memcpy(g_mockExeBuffer + 0x4BBCB0, sigLCopy, 16);
    memcpy(g_mockExeBuffer + 0x4BAA20, sigLMerge, 16);
    memcpy(g_mockExeBuffer + 0x245730, sigLDirty, 16);
    memcpy(g_mockExeBuffer + 0x4BDAD0, sigLVeh, 16);
    memcpy(g_mockExeBuffer + 0x4BFA80, sigLName, 16);

    std::vector<BYTE> lineMgr(0x60, 0);
    std::vector<BYTE> lineObj(0x290, 0);
    std::vector<BYTE> lineCopy(0x290, 0);
    std::vector<BYTE> lineRec(0x198, 0);
    std::vector<BYTE*> linePtrs = { lineObj.data() };
    WriteMockVectorHeader(lineMgr.data(), linePtrs);
    *(BYTE**)(world + 0x128B8) = lineMgr.data();
    *(BYTE**)(lineCopy.data() + 0x228) = lineRec.data();
    *(BYTE**)(lineCopy.data() + 0x230) = lineRec.data() + lineRec.size();

    // Test HALF_PAIRS (+0x08) with unshared dlc5 VehicleType
    SaveSync_SetTestDlcLists("dlc1,dlc5", "dlc1");
    U64 pairSlot[2] = { (U64)(uintptr_t)dlc5Vt, 0u };
    *(U64*)(lineRec.data() + 0x08) = (U64)(uintptr_t)pairSlot;
    *(U64*)(lineRec.data() + 0x10) = (U64)(uintptr_t)(pairSlot + 2);
    TEST_CHECK(LineAccept_Capture((U64)(uintptr_t)lineObj.data(), (U64)(uintptr_t)lineCopy.data()));
    TEST_CHECK(!Commands_Due(0));

    // Test kHalfPtrs (+0x68) with unshared dlc5 VehicleType
    *(U64*)(lineRec.data() + 0x08) = 0;
    *(U64*)(lineRec.data() + 0x10) = 0;
    *(U64*)(lineRec.data() + 0x68) = (U64)(uintptr_t)defSlot;
    *(U64*)(lineRec.data() + 0x70) = (U64)(uintptr_t)(defSlot + 1);
    TEST_CHECK(LineAccept_Capture((U64)(uintptr_t)lineObj.data(), (U64)(uintptr_t)lineCopy.data()));
    TEST_CHECK(!Commands_Due(0));

    // Test base-game VehicleType in kHalfPtrs (+0x68) -> allowed & submitted
    U64 baseSlot[1] = { (U64)(uintptr_t)baseVt };
    *(U64*)(lineRec.data() + 0x68) = (U64)(uintptr_t)baseSlot;
    *(U64*)(lineRec.data() + 0x70) = (U64)(uintptr_t)(baseSlot + 1);
    TEST_CHECK(LineAccept_Capture((U64)(uintptr_t)lineObj.data(), (U64)(uintptr_t)lineCopy.data()));
    TEST_CHECK(Commands_Due(0));

    // 3. Replay-side DLC defense-in-depth: Ref_DecodeDefPtrBytes blocks unshared dlc5 VehicleTypes
    //    for both tag==5 (scriptName) and tag==2 (indexed), defending CargoSettings & LineAccept Decode()
    BYTE encDlc5[136] = {};
    size_t lenDlc5 = 0;
    TEST_CHECK(Ref_EncodeDefPtrBytes((U64)(uintptr_t)dlc5Vt, encDlc5, sizeof(encDlc5), lenDlc5));
    TEST_CHECK(encDlc5[0] == 5u);
    SaveSync_SetTestDlcLists("dlc1,dlc5", "dlc1"); // dlc5 unshared
    U64 decPtr = 0;
    size_t decAt = 0;
    TEST_CHECK(!Ref_DecodeDefPtrBytes(encDlc5, lenDlc5, decAt, decPtr));
    BYTE encTag2[5] = { 2u, 1u, 0u, 0u, 0u }; // tag == 2, index == 1 (dlc5Vt)
    decAt = 0;
    TEST_CHECK(!Ref_DecodeDefPtrBytes(encTag2, sizeof(encTag2), decAt, decPtr));
    SaveSync_SetTestDlcLists("dlc1,dlc5", "dlc1,dlc5"); // dlc5 shared -> allowed
    decAt = 0;
    TEST_CHECK(Ref_DecodeDefPtrBytes(encDlc5, lenDlc5, decAt, decPtr) && decPtr == (U64)(uintptr_t)dlc5Vt);

    Commands_SetSession(COMMANDS_OFF);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    ResetMockWorldMemory();
    return true;
}

bool Test_SpaceProgramPhase3_MutatorsPadSwitchesAndCameraIsolation()
{
    return SubTest_Phase3_SpaceKSitesAndPreconditions() &&
           SubTest_Phase3_SpaceKSitesSpLobbyAndClientTwoStates() &&
           SubTest_Phase3_SpaceActionZeroPreMutationAndReplay() &&
           SubTest_Phase3_SpaceActionPatchBytesAndJumpTargets() &&
           SubTest_Phase3_LaunchPadOrbitSwitches() &&
           SubTest_Phase3_VehicleSwitchesSpaceExclusions() &&
           SubTest_Phase3_SpaceToolsAndWorldCamera14Isolation() &&
           SubTest_Phase3_HardeningRegressions() &&
           SubTest_Phase3_CargoAndLineAcceptDlcFilter();
}

// ---------------------------------------------------------------------------
// Test 73: Space Program Phase 4 (True Space Race in Two-States Mode)
// ---------------------------------------------------------------------------

static BYTE* g_p4SpawnedVehPtr = nullptr;
static BYTE* g_p4ProdSpawnBld = nullptr;
static int   g_p4RemoveOrbitCalls = 0;
static int   g_p4LastRemovedOrbitIdx = -99;
static int   g_p4BorderCargoCalls = 0;
static float g_p4BorderSeenProg0 = 0.0f;
static float g_p4BorderSeenProg1 = 0.0f;
static int   g_p4McWinCalls = 0;
static int   g_p4McSeenBlds = 0;
static int   g_p4McSeenVehs = 0;
static size_t g_p4McSeenOrbits = 0;
static int   g_p4TopBarCalls = 0;
static BYTE* g_p4TopBarSeenMcPtr = nullptr;
static double g_p4OrbitRewardRub = 0.0;
static double g_p4OrbitRewardUsd = 0.0;
static float  g_p4OrbitStatUsd = 0.0f;
static float  g_p4OrbitStatRub = 0.0f;
static bool   g_p4OrbitCreateNewOrbit = false;
static int    g_p4OrbitSeenPopupAllowed = -1;

static void Phase4_InitTwoStates(bool isHost)
{
    ResetMockWorldMemory();
    double initRub[2] = { 1000000.0, 1000000.0 };
    double initUsd[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(isHost, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();
    TwoStates_ResetSpaceForTest();
}

static BYTE* __fastcall Phase4_MockCreateVehicle(BYTE*, BYTE* vt, BYTE*, uintptr_t, uintptr_t,
                                                  uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                                  uintptr_t, uintptr_t)
{
    if (vt) {
        *(int32_t*)(vt + VEH_TYPE_BUILT_IN_PROD_OFFSET) += 1;
    }
    return g_p4SpawnedVehPtr;
}

static void __fastcall Phase4_MockProdLineStep(BYTE* world, BYTE* bld)
{
    if (!world || !bld || !g_p4SpawnedVehPtr) return;
    // Append spawned vehicle to WORLD_VEHICLES_OFFSET and increment +0x9CE4 on its VehicleType
    std::vector<BYTE*> vehList;
    BYTE* const* curB = *(BYTE* const**)(world + WORLD_VEHICLES_OFFSET);
    BYTE* const* curE = *(BYTE* const**)(world + WORLD_VEHICLES_OFFSET + 8);
    if (curB && curE >= curB) {
        vehList.assign(curB, curE);
    }
    vehList.push_back(g_p4SpawnedVehPtr);
    static std::vector<BYTE*> s_p4VehBacking;
    s_p4VehBacking = vehList;
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, s_p4VehBacking);

    BYTE* vt = *(BYTE**)(g_p4SpawnedVehPtr + VEH_TYPE_PTR_OFFSET);
    if (vt) {
        *(int32_t*)(vt + VEH_TYPE_BUILT_IN_PROD_OFFSET) += 1;
    }
    (void)g_p4ProdSpawnBld;
}

static void __fastcall Phase4_MockOrbitStep(BYTE* veh)
{
    if (!veh) return;
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* vt = *(BYTE**)(veh + VEH_TYPE_PTR_OFFSET);
    if (vt) {
        *(int32_t*)(vt + VEH_TYPE_SPACE_ORBIT_OFFSET) += 1;
    }
    if (fabs(g_p4OrbitRewardRub) > 1e-6) {
        *(double*)(world + WORLD_RUB_OFFSET) += g_p4OrbitRewardRub;
    }
    if (fabs(g_p4OrbitRewardUsd) > 1e-6) {
        *(double*)(world + WORLD_USD_OFFSET) += g_p4OrbitRewardUsd;
    }
    if (fabsf(g_p4OrbitStatUsd) > 1e-6f) {
        *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_EXPORT_USD_OFF) += g_p4OrbitStatUsd;
    }
    if (fabsf(g_p4OrbitStatRub) > 1e-6f) {
        *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_EXPORT_RUB_OFF) += g_p4OrbitStatRub;
    }
    if (g_p4OrbitCreateNewOrbit) {
        static std::vector<BYTE> s_p4OrbitBacking;
        BYTE* orbB = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET);
        BYTE* orbE = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8);
        size_t oldBytes = (orbB && orbE >= orbB) ? (size_t)(orbE - orbB) : 0u;
        s_p4OrbitBacking.assign(oldBytes + ORBIT_SLOT_STRIDE, 0);
        if (oldBytes > 0) memcpy(s_p4OrbitBacking.data(), orbB, oldBytes);
        *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = s_p4OrbitBacking.data();
        *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = s_p4OrbitBacking.data() + s_p4OrbitBacking.size();
    }
    g_p4OrbitSeenPopupAllowed = (int)Commands_OnCreatePopupWindow(
        (long long)(uintptr_t)world, 4, (long long)(uintptr_t)veh, 4, 0,
        WINDOW_POPUP_TYPE_SPACE_PAYOUT, 0, 0, 0);
}

static void __fastcall Phase4_MockReentryStep(BYTE* veh)
{
    if (!veh) return;
    BYTE* vt = *(BYTE**)(veh + VEH_TYPE_PTR_OFFSET);
    if (vt) {
        *(int32_t*)(vt + VEH_TYPE_SPACE_REENTRY_OFFSET) += 1;
    }
}

static void __fastcall Phase4_MockRemoveOrbit(BYTE* world, int orbitIdx)
{
    ++g_p4RemoveOrbitCalls;
    g_p4LastRemovedOrbitIdx = orbitIdx;
    BYTE* orbB = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET);
    BYTE* orbE = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8);
    if (orbB && orbE > orbB && orbitIdx >= 0) {
        size_t cnt = (size_t)(orbE - orbB) / ORBIT_SLOT_STRIDE;
        if ((size_t)orbitIdx < cnt) {
            for (size_t i = (size_t)orbitIdx; i + 1 < cnt; ++i) {
                memcpy(orbB + i * ORBIT_SLOT_STRIDE, orbB + (i + 1) * ORBIT_SLOT_STRIDE, ORBIT_SLOT_STRIDE);
            }
            *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbE - ORBIT_SLOT_STRIDE;
        }
    }
}

static void __fastcall Phase4_MockSpawnBorderCargo(BYTE* world)
{
    ++g_p4BorderCargoCalls;
    BYTE* resB = *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET);
    BYTE* resE = *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8);
    if (resB && resE >= resB + 2u * RESEARCH_ITEM_STRIDE) {
        g_p4BorderSeenProg0 = *(const float*)(resB + 0u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET);
        g_p4BorderSeenProg1 = *(const float*)(resB + 1u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET);
    }
}

static void __fastcall Phase4_MockMissionControlWin(BYTE* world, BYTE*)
{
    ++g_p4McWinCalls;
    BYTE* const* bb = *(BYTE* const**)(world + WORLD_BUILDINGS_OFFSET);
    BYTE* const* be = *(BYTE* const**)(world + WORLD_BUILDINGS_OFFSET + 8);
    g_p4McSeenBlds = (bb && be >= bb) ? (int)(be - bb) : -1;
    BYTE* const* vb = *(BYTE* const**)(world + WORLD_VEHICLES_OFFSET);
    BYTE* const* ve = *(BYTE* const**)(world + WORLD_VEHICLES_OFFSET + 8);
    g_p4McSeenVehs = (vb && ve >= vb) ? (int)(ve - vb) : -1;
    BYTE* ob = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET);
    BYTE* oe = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8);
    g_p4McSeenOrbits = (ob && oe >= ob) ? ((size_t)(oe - ob) / ORBIT_SLOT_STRIDE) : 0u;
}

static void __fastcall Phase4_MockTopBarHudDraw(BYTE* world)
{
    ++g_p4TopBarCalls;
    g_p4TopBarSeenMcPtr = *(BYTE**)(world + WORLD_MISSION_CONTROL_BLD_PTR_OFF);
}

static bool SubTest_Phase4_HardeningAndRvaRegressions()
{
    // Verified SOVIET64.exe v1.2.0.9 RVA & struct offset invariants fixed in Phase 4
    TEST_CHECK(CIT_RIDING_VEHICLE_PTR_OFF == 0x060u);
    TEST_CHECK(RESEARCH_ITEM_REQ_WORK_OFFSET == 0x5Cu);
    TEST_CHECK(RVA_PROD_LINE_STEP == 0x1E42C0u);
    TEST_CHECK(RVA_CREATE_VEHICLE == 0x403E60u);
    TEST_CHECK(RVA_SPACE_VEH_ORBIT_STEP == 0x695AC0u);
    TEST_CHECK(RVA_SPACE_VEH_REENTRY_STEP == 0x692790u);
    TEST_CHECK(RVA_REMOVE_ORBIT == 0x41DAB0u);
    TEST_CHECK(RVA_SPAWN_SPACE_CARGO_AT_BORDER == 0x314A60u);
    TEST_CHECK(RVA_MISSION_CONTROL_WINDOW == 0x78DB70u);
    TEST_CHECK(RVA_TOP_BAR_HUD_DRAW == 0x94020u);

    Phase4_InitTwoStates(true);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    g_p3LaunchCalls = g_p3DetachCalls = 0;
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_LAUNCH_ROCKET_RVA, (void*)&Phase3_MockLaunchRocket);
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_ORBIT_DETACH_RVA, (void*)&Phase3_MockOrbitDetach);

    // Non-LaunchPad building ($TYPE_AIRPLANE_PARKING: cat=0x2F, sub=0) MUST be blocked on Launch replay ([P2] #2)
    std::vector<BYTE> nonPadType(0x400u, 0);
    *(int32_t*)(nonPadType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD;
    *(int32_t*)(nonPadType.data() + BLD_TYPE_SUBTYPE_OFF) = 0;
    std::vector<BYTE> nonPad(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(nonPad.data() + BLD_TYPE_OFFSET) = nonPadType.data();
    *(float*)(nonPad.data() + OBJ_POS_OFFSET) = -100.0f;

    std::vector<BYTE> krazType(WORLD_VEHICLE_TYPE_STRIDE, 0); // all space flags 0 ([P2] #3)
    std::vector<BYTE> rocket(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> partner(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(rocket.data() + VEH_ID_OFFSET) = 6101u;
    *(float*)(rocket.data() + VEH_POS_OFFSET) = -100.0f;
    *(BYTE**)(rocket.data() + VEH_TYPE_PTR_OFFSET) = krazType.data();
    *(U64*)(rocket.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)nonPad.data();
    *(int32_t*)(rocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(unsigned*)(partner.data() + VEH_ID_OFFSET) = 6102u;
    std::vector<BYTE*> vehs = { rocket.data(), partner.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);
    TwoStates_RegisterVehicleOwner(6101, 1);

    U64 launchArgs[7] = { (U64)(uintptr_t)rocket.data(), (U64)(unsigned)-1, 0, 0, 0, 0, 0 };
    BYTE msg[256] = {};
    size_t msgSize = 0;
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchArgs, msg, sizeof(msg), &msgSize));
    TwoStates_BeginCommand(1);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TwoStates_EndCommand(1);
    TEST_CHECK(g_p3LaunchCalls == 0); // [P2] #2 rejected non-launchpad

    // Non-spacecraft Kraz_255 rejected on replay by Deorbit/Reentry ([P2] #3)
    Commands_SetSession(COMMANDS_HOST);
    Phase3_ResetSpaceActionMockCounters(1);
    GameCalls_SetSpaceActionFnsForTest(&Phase3_MockHasReentry, &Phase3_MockSetupReentry, &Phase3_MockSetupDeorbit);
    *(U64*)(rocket.data() + VEH_CUR_BLD_OFFSET) = 0;
    *(int32_t*)(rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 1;
    TEST_CHECK(GameCalls_CaptureSpaceActionForTest((U64)(uintptr_t)rocket.data(), SPACE_SITE_DEORBIT_OR_ABORT, 0));
    Commands_Boundary(10);
    TEST_CHECK(g_p3SetupDeorbitCalls == 0);

    // Detach with linkPtr == 0 && dockPartner != 0 skips without touching partner ([P2] #1)
    *(U64*)(rocket.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) = (U64)(uintptr_t)partner.data();
    *(U64*)(rocket.data() + VEH_LINK_PTR_OFFSET) = 0;
    U64 detachArgs[7] = { (U64)(uintptr_t)rocket.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_SPACE_ORBIT_DETACH_RVA, detachArgs, msg, sizeof(msg), &msgSize));
    TwoStates_BeginCommand(1);
    TEST_CHECK(GameCalls_ApplyCommand(msg, msgSize));
    TwoStates_EndCommand(1);
    TEST_CHECK(g_p3DetachCalls == 0);
    TEST_CHECK(*(U64*)(rocket.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) == (U64)(uintptr_t)partner.data());

    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase4_MilestonesAndResearchUnlockIsolation()
{
    Phase4_InitTwoStates(true); // State 1 local
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 2 VehicleTypes: index 0 vs 1 (dlc5 rocket), plus 2 research items for $REQUIRE_MISSION parity
    std::vector<BYTE> vtypes(2u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    BYTE* vtSoyuz = vtypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vtCargo = vtypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE;
    strcpy_s((char*)(vtSoyuz + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "media_soviet/dlc5/vehicles/rocket_soyuz.ini");
    strcpy_s((char*)(vtCargo + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "dlc5\\vehicles\\cargo_progress.ini");
    vtSoyuz[VEH_TYPE_ROCKET_FLAG_OFFSET] = 1;
    vtCargo[VEH_TYPE_SPACECRAFT_FLAG_OFFSET] = 1;
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = vtypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = vtypes.data() + vtypes.size();

    std::vector<BYTE> resVec(2u * RESEARCH_ITEM_STRIDE, 0);
    *(float*)(resVec.data() + 0u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;
    *(float*)(resVec.data() + 1u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.0f;
    *(int32_t*)(resVec.data() + 1u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_REQ_WORK_OFFSET) = 1; // +0x5C verified
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resVec.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8) = resVec.data() + resVec.size();
    // Ensure State 2 has 0.0f progress on item 0 while State 1 holds 1.0f
    TwoStates_BeginCommand(2);
    *(float*)(resVec.data() + 0u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.0f;
    TwoStates_EndCommand(2);

    std::vector<BYTE> fac1(BUILDING_STRUCT_BYTES, 0);
    *(float*)(fac1.data() + OBJ_POS_OFFSET) = -350.0f; // State 1
    std::vector<BYTE> newRocket(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(newRocket.data() + VEH_ID_OFFSET) = 6201u;
    *(float*)(newRocket.data() + VEH_POS_OFFSET) = 50.0f; // near border: must inherit State 1 via H_ProdLineStep!
    *(BYTE**)(newRocket.data() + VEH_TYPE_PTR_OFFSET) = vtSoyuz;

    g_p4SpawnedVehPtr = newRocket.data();
    g_p4ProdSpawnBld = fac1.data();
    std::vector<BYTE*> initVehs;
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, initVehs);

    // 1. State 1 builds rocket via H_ProdLineStep -> +0x9CE4 == 1 ONLY in State 1; vehicle owner == 1
    TwoStates_InvokeProdLineStepForTest(world, fac1.data(), &Phase4_MockProdLineStep);
    int b1 = -1, o1 = -1, r1 = -1;
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(1, "dlc5/vehicles/rocket_soyuz.ini", &b1, &o1, &r1));
    TEST_CHECK(b1 == 1 && o1 == 0 && r1 == 0);
    TEST_CHECK(!TwoStates_GetSpaceMilestoneForTest(2, "dlc5/vehicles/rocket_soyuz.ini", &b1, &o1, &r1));
    TEST_CHECK(TwoStates_GetVehicleOwner(6201, newRocket.data()) == 1);

    // Non-factory CreateVehicle (factoryBuilt == 0) MUST NOT attribute milestone as factory production
    TwoStates_InvokeCreateVehicleForTest(world, vtCargo, fac1.data(), 0, &Phase4_MockCreateVehicle);
    TEST_CHECK(!TwoStates_GetSpaceMilestoneForTest(1, "dlc5/vehicles/cargo_progress.ini", &b1, &o1, &r1));

    // Factory CreateVehicle (factoryBuilt == 1) on fac2 (State 2) sets built ONLY in State 2
    std::vector<BYTE> fac2(BUILDING_STRUCT_BYTES, 0);
    *(float*)(fac2.data() + OBJ_POS_OFFSET) = 350.0f; // State 2
    std::vector<BYTE> cargo2(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(cargo2.data() + VEH_ID_OFFSET) = 6202u;
    *(BYTE**)(cargo2.data() + VEH_TYPE_PTR_OFFSET) = vtCargo;
    g_p4SpawnedVehPtr = cargo2.data();
    TwoStates_InvokeCreateVehicleForTest(world, vtCargo, fac2.data(), 1, &Phase4_MockCreateVehicle);
    int b2 = -1, o2 = -1, r2 = -1;
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(2, "MEDIA_SOVIET/DLC5/VEHICLES/CARGO_PROGRESS.INI", &b2, &o2, &r2));
    TEST_CHECK(b2 == 1 && o2 == 0 && r2 == 0);
    TEST_CHECK(!TwoStates_GetSpaceMilestoneForTest(1, "dlc5/vehicles/cargo_progress.ini", &b1, &o1, &r1));

    // 2. Orbit & Reentry steps isolate +0x9CE8 / +0x9CEC strictly to owner's slice and preserve previous mount;
    //    furthermore, State 2 launching the SAME vtSoyuz after State 1 ALREADY reached orbit==1 still records orbit==1 in State 2!
    g_p4OrbitRewardRub = 0.0;
    g_p4OrbitRewardUsd = 0.0;
    g_p4OrbitStatUsd = 0.0f;
    g_p4OrbitStatRub = 0.0f;
    g_p4OrbitCreateNewOrbit = false;
    TwoStates_InvokeSpaceOrbitStepForTest(newRocket.data(), &Phase4_MockOrbitStep);
    TwoStates_InvokeSpaceReentryStepForTest(cargo2.data(), &Phase4_MockReentryStep);
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(1, "dlc5/vehicles/rocket_soyuz.ini", &b1, &o1, &r1));
    TEST_CHECK(b1 == 1 && o1 == 1 && r1 == 0);
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(2, "dlc5/vehicles/cargo_progress.ini", &b2, &o2, &r2));
    TEST_CHECK(b2 == 1 && o2 == 0 && r2 == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 0) == 0);
    TEST_CHECK(TwoStates_GetSpaceMilestoneMountStateForTest() == 1);

    // Same vtSoyuz for State 2 right after State 1's mount: must detect 0 -> 1 cleanly!
    std::vector<BYTE> s2Soyuz(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(s2Soyuz.data() + VEH_ID_OFFSET) = 6203u;
    *(BYTE**)(s2Soyuz.data() + VEH_TYPE_PTR_OFFSET) = vtSoyuz;
    TwoStates_RegisterVehicleOwner(6203, 2);
    TwoStates_InvokeSpaceOrbitStepForTest(s2Soyuz.data(), &Phase4_MockOrbitStep);
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(2, "dlc5/vehicles/rocket_soyuz.ini", &b2, &o2, &r2));
    TEST_CHECK(o2 == 1);
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(1, "dlc5/vehicles/rocket_soyuz.ini", &b1, &o1, &r1));
    TEST_CHECK(b1 == 1 && o1 == 1);

    // 3. Space cargo unlocks (WORLD+0x11BA8..0x11BA9) remain isolated per state
    TwoStates_BeginCommand(1);
    world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] = 1;
    world[WORLD_SPACE_CARGO_EAST_UNLOCK_OFF] = 0;
    TwoStates_EndCommand(1);
    TwoStates_BeginCommand(2);
    TEST_CHECK(world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] == 0);
    world[WORLD_SPACE_CARGO_EAST_UNLOCK_OFF] = 1;
    TwoStates_EndCommand(2);
    TEST_CHECK(world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] == 1);
    TEST_CHECK(world[WORLD_SPACE_CARGO_EAST_UNLOCK_OFF] == 0);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase4_OrbitPayoutAndPopupSuppression()
{
    Phase4_InitTwoStates(true); // Host = State 1 local
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> vtypes(1u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    strcpy_s((char*)(vtypes.data() + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "dlc5/vehicles/sat_comm.ini");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = vtypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = vtypes.data() + vtypes.size();

    std::vector<BYTE> craft2(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(craft2.data() + VEH_ID_OFFSET) = 6302u;
    *(BYTE**)(craft2.data() + VEH_TYPE_PTR_OFFSET) = vtypes.data();
    TwoStates_RegisterVehicleOwner(6302, 2);

    std::vector<BYTE> craft1(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(craft1.data() + VEH_ID_OFFSET) = 6301u;
    *(BYTE**)(craft1.data() + VEH_TYPE_PTR_OFFSET) = vtypes.data();
    TwoStates_RegisterVehicleOwner(6301, 1);

    std::vector<BYTE*> vehs = { craft2.data(), craft1.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);

    // State 2 orbit commercial payout on Host (local == 1): USD/RUB + STAT+0x190/+0x194 attributed to State 2, popup 0x27 suppressed
    g_p4OrbitRewardRub = 8000.0;
    g_p4OrbitRewardUsd = 12000.0;
    g_p4OrbitStatUsd = 12000.0f;
    g_p4OrbitStatRub = 8000.0f;
    g_p4OrbitCreateNewOrbit = false;
    g_p4OrbitSeenPopupAllowed = -1;

    TwoStates_InvokeSpaceOrbitStepForTest(craft2.data(), &Phase4_MockOrbitStep);
    TEST_CHECK(g_p4OrbitSeenPopupAllowed == 0); // suppressed for local State 1!
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1000000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 500000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 1008000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 512000.0, 1e-3);
    float s1Vec[4] = {}, s2Vec[4] = {};
    TwoStates_GetStateEconFloat4ForTest(1, false, s1Vec);
    TwoStates_GetStateEconFloat4ForTest(2, false, s2Vec);
    TEST_CHECK_NEAR(s1Vec[2], 0.0f, 1e-2f);
    TEST_CHECK_NEAR(s2Vec[2], 12000.0f, 1e-2f);
    TEST_CHECK_NEAR(s2Vec[3], 8000.0f, 1e-2f);

    // State 1 orbit commercial payout on Host (local == 1): popup 0x27 ALLOWED (returns non-zero)
    g_p4OrbitSeenPopupAllowed = 0;
    TwoStates_InvokeSpaceOrbitStepForTest(craft1.data(), &Phase4_MockOrbitStep);
    TEST_CHECK(g_p4OrbitSeenPopupAllowed != 0);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 512000.0, 1e-3);
    TEST_CHECK(TwoStates_GetSpaceActiveOwner() == 0);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase4_OrbitOwnershipAndCrashSiteGuards()
{
    Phase4_InitTwoStates(true); // State 1 local
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> orbits(2u * ORBIT_SLOT_STRIDE, 0);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbits.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbits.data() + orbits.size();

    std::vector<BYTE> vtypes(1u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    strcpy_s((char*)(vtypes.data() + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "dlc5/vehicles/sat.ini");
    vtypes[VEH_TYPE_ROCKET_FLAG_OFFSET] = 1;
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = vtypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = vtypes.data() + vtypes.size();

    // State 2 launches and creates orbit index 2 via H_SpaceVehOrbitStep
    std::vector<BYTE> c2(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(c2.data() + VEH_ID_OFFSET) = 6402u;
    *(BYTE**)(c2.data() + VEH_TYPE_PTR_OFFSET) = vtypes.data();
    TwoStates_RegisterVehicleOwner(6402, 2);

    const int initOwners[2] = { 1, 1 };
    TwoStates_SetOrbitOwnersForTest(initOwners, 2);
    g_p4OrbitRewardRub = g_p4OrbitRewardUsd = 0.0;
    g_p4OrbitStatUsd = g_p4OrbitStatRub = 0.0f;
    g_p4OrbitCreateNewOrbit = true;
    TwoStates_InvokeSpaceOrbitStepForTest(c2.data(), &Phase4_MockOrbitStep);
    g_p4OrbitCreateNewOrbit = false;
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 3u);
    TEST_CHECK(TwoStates_GetOrbitOwner(0) == 1);
    TEST_CHECK(TwoStates_GetOrbitOwner(1) == 1);
    TEST_CHECK(TwoStates_GetOrbitOwner(2) == 2);

    // Negative orbitIdx in H_RemoveOrbit MUST short-circuit safely before origFn (js prologue guard)
    g_p4RemoveOrbitCalls = 0;
    TwoStates_InvokeRemoveOrbitForTest(world, -1, &Phase4_MockRemoveOrbit);
    TEST_CHECK(g_p4RemoveOrbitCalls == 0);
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 3u);

    // Valid removal of orbitIdx 1 shifts State 2's orbit down from index 2 -> index 1
    TwoStates_InvokeRemoveOrbitForTest(world, 1, &Phase4_MockRemoveOrbit);
    TEST_CHECK(g_p4RemoveOrbitCalls == 1 && g_p4LastRemovedOrbitIdx == 1);
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 2u);
    TEST_CHECK(TwoStates_GetOrbitOwner(0) == 1);
    TEST_CHECK(TwoStates_GetOrbitOwner(1) == 2);

    // Launch Pad ownership & catch_orbit ownership guards
    std::vector<BYTE> padType(0x400u, 0);
    *(int32_t*)(padType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD;
    *(int32_t*)(padType.data() + BLD_TYPE_SUBTYPE_OFF) = (int32_t)BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL;
    std::vector<BYTE> pad1(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> pad2(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(pad1.data() + BLD_TYPE_OFFSET) = padType.data();
    *(float*)(pad1.data() + OBJ_POS_OFFSET) = -300.0f; // State 1
    *(BYTE**)(pad2.data() + BLD_TYPE_OFFSET) = padType.data();
    *(float*)(pad2.data() + OBJ_POS_OFFSET) = +300.0f; // State 2

    std::vector<BYTE> rocket1(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(rocket1.data() + VEH_ID_OFFSET) = 6401u;
    *(BYTE**)(rocket1.data() + VEH_TYPE_PTR_OFFSET) = vtypes.data();
    *(U64*)(rocket1.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad2.data();
    *(int32_t*)(rocket1.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    std::vector<BYTE*> guardVehs = { rocket1.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, guardVehs);
    TwoStates_RegisterVehicleOwner(6401, 1);

    U64 catchForeignOrbit[7] = { (U64)(uintptr_t)rocket1.data(), 1u, 0, 0, 0, 0, 0 };
    U64 catchOwnOrbit[7] = { (U64)(uintptr_t)rocket1.data(), 0u, 0, 0, 0, 0, 0 };
    // Rocket1 on foreign Pad2 -> blocked even for own orbit
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA, catchOwnOrbit));
    // Rocket1 on own Pad1, catching foreign orbit 1 -> blocked
    *(U64*)(rocket1.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad1.data();
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA, catchForeignOrbit));
    // Rocket1 on own Pad1, catching own orbit 0 -> allowed!
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA, catchOwnOrbit));

    // Mid-orbit vehicle with $SPACE_SHIP_REENTRY flag without g_vehicleOwners entry + positive X coord inherits State 1 via orbit[0],
    // docked child inherits State 1 via dockPartner, and cosmonaut citizen riding vehId==0 (+0x60) resolves State 1!
    std::vector<BYTE> reentryVt(WORLD_VEHICLE_TYPE_STRIDE, 0);
    reentryVt[VEH_TYPE_SPACE_REENTRY_FLAG_OFFSET] = 1;
    std::vector<BYTE> midOrbitVeh(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(midOrbitVeh.data() + VEH_ID_OFFSET) = 0u; // vehId == 0!
    *(BYTE**)(midOrbitVeh.data() + VEH_TYPE_PTR_OFFSET) = reentryVt.data();
    *(float*)(midOrbitVeh.data() + VEH_POS_OFFSET) = +450.0f; // State 2 X-coord
    *(int32_t*)(midOrbitVeh.data() + VEH_ORBIT_INDEX_OFFSET) = 0; // State 1 orbit
    std::vector<BYTE> childVeh(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(childVeh.data() + VEH_ID_OFFSET) = 6499u;
    *(float*)(childVeh.data() + VEH_POS_OFFSET) = +450.0f;
    *(int32_t*)(childVeh.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(BYTE**)(childVeh.data() + VEH_SPACE_DOCK_PARTNER_OFFSET) = midOrbitVeh.data();

    std::vector<BYTE> cosmonaut(0x800u, 0);
    *(float*)(cosmonaut.data() + CIT_POS_OFFSET) = +450.0f;
    *(BYTE**)(cosmonaut.data() + CIT_RIDING_VEHICLE_PTR_OFF) = midOrbitVeh.data();

    TEST_CHECK(TwoStates_GetVehicleOwner(0, midOrbitVeh.data()) == 1);
    TEST_CHECK(TwoStates_GetVehicleOwner(6499, childVeh.data()) == 1);
    TEST_CHECK(TwoStates_ResolveCitizenOwnerForTest(cosmonaut.data()) == 1);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase4_MissionControlHudAndBorderContracts()
{
    Phase4_InitTwoStates(true); // State 1 local
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Setup buildings (pad1 [S1], mc1 [S1 built], mc2 [S2 built]) + vehicles (s1Veh, s2Veh) + 2 orbits
    std::vector<BYTE> mcType(0x400u, 0);
    *(uint32_t*)(mcType.data() + BLD_TYPE_CAT_OFF) = BLD_CAT_MISSION_CONTROL;
    *(uint32_t*)(mcType.data() + BLD_TYPE_SUBTYPE_OFF) = BLD_SUBTYPE_MISSION_CONTROL;

    std::vector<BYTE> pad1(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> mc1(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> mc2(BUILDING_STRUCT_BYTES, 0);
    *(float*)(pad1.data() + OBJ_POS_OFFSET) = -200.0f;
    *(float*)(mc1.data() + OBJ_POS_OFFSET) = -300.0f;
    *(BYTE**)(mc1.data() + BLD_TYPE_OFFSET) = mcType.data();
    *(float*)(mc1.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(float*)(mc2.data() + OBJ_POS_OFFSET) = +300.0f;
    *(BYTE**)(mc2.data() + BLD_TYPE_OFFSET) = mcType.data();
    *(float*)(mc2.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    std::vector<BYTE*> bldVec = { pad1.data(), mc1.data(), mc2.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    std::vector<BYTE> v1(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> v2(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(v1.data() + VEH_ID_OFFSET) = 6501u;
    *(unsigned*)(v2.data() + VEH_ID_OFFSET) = 6502u;
    TwoStates_RegisterVehicleOwner(6501, 1);
    TwoStates_RegisterVehicleOwner(6502, 2);
    std::vector<BYTE*> vehVec = { v1.data(), v2.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehVec);

    std::vector<BYTE> orbits(2u * ORBIT_SLOT_STRIDE, 0);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbits.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbits.data() + orbits.size();

    // 1. H_MissionControlWindow filters BLD & VEH to State 1, leaves ALL orbits visible, and restores vectors
    BYTE dummyWin[64] = {};
    g_p4McWinCalls = 0;
    TwoStates_InvokeMissionControlWinForTest(world, dummyWin, &Phase4_MockMissionControlWin);
    TEST_CHECK(g_p4McWinCalls == 1);
    TEST_CHECK(g_p4McSeenBlds == 2); // pad1 + mc1 only
    TEST_CHECK(g_p4McSeenVehs == 1); // v1 only
    TEST_CHECK(g_p4McSeenOrbits == 2u); // ALL orbits remain visible
    BYTE* const* postBb = *(BYTE* const**)(world + WORLD_BUILDINGS_OFFSET);
    BYTE* const* postBe = *(BYTE* const**)(world + WORLD_BUILDINGS_OFFSET + 8);
    BYTE* const* postVb = *(BYTE* const**)(world + WORLD_VEHICLES_OFFSET);
    BYTE* const* postVe = *(BYTE* const**)(world + WORLD_VEHICLES_OFFSET + 8);
    TEST_CHECK((postBe - postBb) == 3);
    TEST_CHECK((postVe - postVb) == 2);

    // 2. H_TopBarHudDraw swaps foreign WORLD+0x11BB0 (mc2) -> local mc1 during draw and restores afterwards
    *(BYTE**)(world + WORLD_MISSION_CONTROL_BLD_PTR_OFF) = mc2.data();
    g_p4TopBarCalls = 0;
    g_p4TopBarSeenMcPtr = nullptr;
    TwoStates_InvokeTopBarHudDrawForTest(world, &Phase4_MockTopBarHudDraw);
    TEST_CHECK(g_p4TopBarCalls == 1);
    TEST_CHECK(g_p4TopBarSeenMcPtr == mc1.data());
    TEST_CHECK(*(BYTE**)(world + WORLD_MISSION_CONTROL_BLD_PTR_OFF) == mc2.data());

    // 3. H_SpawnSpaceCargoAtBorder applies UNION max(s1, s2) research progress during 0x314A60 & restores
    std::vector<BYTE> resVec(2u * RESEARCH_ITEM_STRIDE, 0);
    *(float*)(resVec.data() + 0u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;
    *(float*)(resVec.data() + 1u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.25f;
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resVec.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8) = resVec.data() + resVec.size();
    TwoStates_BeginCommand(2);
    *(float*)(resVec.data() + 0u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.4f;
    *(float*)(resVec.data() + 1u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;
    TwoStates_EndCommand(2);

    g_p4BorderCargoCalls = 0;
    g_p4BorderSeenProg0 = g_p4BorderSeenProg1 = 0.0f;
    TwoStates_InvokeSpawnSpaceCargoForTest(world, &Phase4_MockSpawnBorderCargo);
    TEST_CHECK(g_p4BorderCargoCalls == 1);
    TEST_CHECK_NEAR(g_p4BorderSeenProg0, 1.0f, 1e-4f); // State 1 max
    TEST_CHECK_NEAR(g_p4BorderSeenProg1, 1.0f, 1e-4f); // State 2 max
    TEST_CHECK_NEAR(*(float*)(resVec.data() + 0u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET), 1.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(resVec.data() + 1u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET), 0.25f, 1e-4f);

    TwoStates_OnSessionEnded();
    ResetMockWorldMemory();
    return true;
}

static bool SubTest_Phase4_StateHashPersistenceAndSpIsolation()
{
    Phase4_InitTwoStates(true);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(float*)(bld.data() + OBJ_POS_OFFSET) = -100.0f;
    std::vector<BYTE*> bldVec = { bld.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    std::vector<BYTE> vtypes(2u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    strcpy_s((char*)(vtypes.data() + 0u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "dlc5/vehicles/rocket_soyuz.ini");
    strcpy_s((char*)(vtypes.data() + 1u * WORLD_VEHICLE_TYPE_STRIDE + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "dlc5/vehicles/cargo_progress.ini");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = vtypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = vtypes.data() + vtypes.size();

    std::vector<BYTE> resVec(2u * RESEARCH_ITEM_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resVec.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8) = resVec.data() + resVec.size();

    std::vector<BYTE> orbits(2u * ORBIT_SLOT_STRIDE, 0);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbits.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbits.data() + orbits.size();

    const int ownersInit[2] = { 1, 2 };
    TwoStates_SetOrbitOwnersForTest(ownersInit, 2);
    TwoStates_SetSpaceMilestoneForTest(1, "media_soviet/dlc5/vehicles/rocket_soyuz.ini", 3, 1, 0);
    TwoStates_SetSpaceMilestoneForTest(2, "dlc5/vehicles/cargo_progress.ini", 2, 2, 1);

    // StateHash detects any per-state milestone or orbit owner asymmetry
    StateSummary sumA = {};
    TEST_CHECK(StateHash_Compute(&sumA));
    TwoStates_SetSpaceMilestoneForTest(2, "dlc5/vehicles/cargo_progress.ini", 2, 2, 2);
    StateSummary sumB = {};
    TEST_CHECK(StateHash_Compute(&sumB));
    TEST_CHECK(sumA.hashSpace != sumB.hashSpace);
    const int ownersFlipped[2] = { 2, 1 };
    TwoStates_SetOrbitOwnersForTest(ownersFlipped, 2);
    StateSummary sumC = {};
    TEST_CHECK(StateHash_Compute(&sumC));
    TEST_CHECK(sumB.hashSpace != sumC.hashSpace);

    // Order-independent hash parity when Mixed-DLC case/slashes differ across Host vs Client
    unsigned long long h1 = 0x123456789ABCDEF0ULL, h2 = 0x123456789ABCDEF0ULL;
    TEST_CHECK(TwoStates_HashSpaceMilestones(&h1));
    TwoStates_ResetSpaceForTest();
    TwoStates_SetOrbitOwnersForTest(ownersFlipped, 2);
    TwoStates_SetSpaceMilestoneForTest(2, "MEDIA_SOVIET\\DLC5\\VEHICLES\\CARGO_PROGRESS.INI", 2, 2, 2);
    TwoStates_SetSpaceMilestoneForTest(1, "dlc5\\vehicles\\rocket_soyuz.ini", 3, 1, 0);
    TEST_CHECK(TwoStates_HashSpaceMilestones(&h2));
    TEST_CHECK(h1 == h2);

    // republicmp_coop.ini round-trip serialization + reload parity (for both states' cargo unlocks, milestones, and orbit_owners)
    TwoStates_BeginCommand(1);
    world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] = 1;
    world[WORLD_SPACE_CARGO_EAST_UNLOCK_OFF] = 0;
    TwoStates_EndCommand(1);
    TwoStates_BeginCommand(2);
    world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] = 0;
    world[WORLD_SPACE_CARGO_EAST_UNLOCK_OFF] = 1;
    TwoStates_EndCommand(2);

    std::filesystem::path coopDir = std::filesystem::path(g_mockBaseDir) / "phase4_coop_save";
    std::error_code ec;
    std::filesystem::create_directories(coopDir, ec);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(coopDir.string().c_str()));

    TwoStates_ResetSpaceForTest();
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 0u);
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(coopDir.string().c_str(), true));
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 2u);
    TEST_CHECK(TwoStates_GetOrbitOwner(0) == 2 && TwoStates_GetOrbitOwner(1) == 1);
    int rb = 0, ro = 0, rr = 0;
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(1, "dlc5/vehicles/rocket_soyuz.ini", &rb, &ro, &rr));
    TEST_CHECK(rb == 3 && ro == 1 && rr == 0);
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(2, "dlc5/vehicles/cargo_progress.ini", &rb, &ro, &rr));
    TEST_CHECK(rb == 2 && ro == 2 && rr == 2);
    TEST_CHECK(world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] == 1 && world[WORLD_SPACE_CARGO_EAST_UNLOCK_OFF] == 0);
    TwoStates_BeginCommand(2);
    TEST_CHECK(world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] == 0 && world[WORLD_SPACE_CARGO_EAST_UNLOCK_OFF] == 1);
    TwoStates_EndCommand(2);

    // Legacy meta upgrade without orbit_owners infers orbit owners from orbiting vehicles in WORLD_VEHICLES
    std::vector<BYTE> orbVeh(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(orbVeh.data() + VEH_ID_OFFSET) = 6602u;
    *(int32_t*)(orbVeh.data() + VEH_ORBIT_INDEX_OFFSET) = 1;
    std::vector<BYTE*> orbVehVec = { orbVeh.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, orbVehVec);
    TwoStates_RegisterVehicleOwner(6602, 2);
    TwoStates_SetOrbitOwnersForTest(nullptr, 0);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(coopDir.string().c_str()));
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(coopDir.string().c_str(), true));
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 2u);
    TEST_CHECK(TwoStates_GetOrbitOwner(1) == 2);
    std::filesystem::remove_all(coopDir, ec);

    // Single-Player isolation after TwoStates_OnSessionEnded()
    TwoStates_OnSessionEnded();
    TEST_CHECK(!TwoStates_IsActive());
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 0u);
    g_p4RemoveOrbitCalls = g_p4TopBarCalls = 0;
    TwoStates_InvokeRemoveOrbitForTest(world, 0, &Phase4_MockRemoveOrbit);
    TwoStates_InvokeTopBarHudDrawForTest(world, &Phase4_MockTopBarHudDraw);
    TEST_CHECK(g_p4RemoveOrbitCalls == 1 && g_p4TopBarCalls == 1);
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)(uintptr_t)world, 4, 0, 4, 0,
                                            WINDOW_POPUP_TYPE_SPACE_PAYOUT, 0, 0, 0) != 0);

    ResetMockWorldMemory();
    return true;
}

static BYTE* g_p4McCaptureRocket = nullptr;
static BYTE  g_p4McCapturedLaunchMsg[256] = {};
static size_t g_p4McCapturedLaunchSize = 0;
static bool  g_p4McCaptureOk = false;
static float g_p4BorderSeenProg120 = 0.0f;
static float g_p4BorderSeenProg121 = 0.0f;
static BYTE* g_p4OrbitMockRemoveTarget = nullptr;

static void __fastcall Phase4_MockMcCaptureLaunch(BYTE*, BYTE*)
{
    g_p4McCapturedLaunchSize = 0;
    U64 launchArgs[7] = { (U64)(uintptr_t)g_p4McCaptureRocket, (U64)(unsigned)-1, 0, 0, 0, 0, 0 };
    g_p4McCaptureOk = GameCalls_CaptureMsgForTest(
        SITE_SPACE_LAUNCH_NEW_ORBIT_RVA, launchArgs,
        g_p4McCapturedLaunchMsg, sizeof(g_p4McCapturedLaunchMsg), &g_p4McCapturedLaunchSize);
}

static void __fastcall Phase4_MockLaunchPushesOrbitAndClearsOwners(U64 veh, int)
{
    ++g_p3LaunchCalls;
    // Vanilla 0x698720 resets home/depot fields (+0x4F0/+0x4F8) on launch
    *(U64*)(veh + VEH_HOME_OFFSET) = 0;
    *(U64*)(veh + VEH_DEPOT_OFFSET) = 0;
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    static std::vector<BYTE> s_p4LaunchOrbitBacking;
    BYTE* orbB = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET);
    BYTE* orbE = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8);
    size_t oldBytes = (orbB && orbE >= orbB) ? (size_t)(orbE - orbB) : 0u;
    s_p4LaunchOrbitBacking.assign(oldBytes + ORBIT_SLOT_STRIDE, 0);
    if (oldBytes > 0) memcpy(s_p4LaunchOrbitBacking.data(), orbB, oldBytes);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = s_p4LaunchOrbitBacking.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = s_p4LaunchOrbitBacking.data() + s_p4LaunchOrbitBacking.size();
}

static void __fastcall Phase4_MockOrbitStepRemoveAndPush(BYTE*)
{
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    // Simulate 0x695AC0 removing orbit 0 AND pushing a new orbit in the same call (net count delta == 0)
    TwoStates_InvokeRemoveOrbitForTest(world, 0, &Phase4_MockRemoveOrbit);
    static std::vector<BYTE> s_p4RepushBacking;
    BYTE* orbB = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET);
    BYTE* orbE = *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8);
    size_t oldBytes = (orbB && orbE >= orbB) ? (size_t)(orbE - orbB) : 0u;
    s_p4RepushBacking.assign(oldBytes + ORBIT_SLOT_STRIDE, 0);
    if (oldBytes > 0) memcpy(s_p4RepushBacking.data(), orbB, oldBytes);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = s_p4RepushBacking.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = s_p4RepushBacking.data() + s_p4RepushBacking.size();
    (void)g_p4OrbitMockRemoveTarget;
}

static void __fastcall Phase4_MockSpawnBorder135(BYTE* world)
{
    ++g_p4BorderCargoCalls;
    BYTE* resB = *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET);
    BYTE* resE = *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8);
    if (resB && resE >= resB + 122u * RESEARCH_ITEM_STRIDE) {
        g_p4BorderSeenProg120 = *(const float*)(resB + 120u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET);
        g_p4BorderSeenProg121 = *(const float*)(resB + 121u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET);
    }
}

static bool SubTest_Phase4_ReviewRegressions()
{
    // 1. [P0-1] Launch captured INSIDE H_MissionControlWindow with preceding foreign vehicles resolves true WORLD index
    Phase4_InitTwoStates(false); // Client (State 2 local)
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    WriteAbsJmp12(g_mockExeBuffer + FN_SPACE_LAUNCH_ROCKET_RVA, (void*)&Phase4_MockLaunchPushesOrbitAndClearsOwners);

    std::vector<BYTE> padType(0x400u, 0);
    *(int32_t*)(padType.data() + BLD_TYPE_CAT_OFF) = (int32_t)BLD_CAT_SPACE_LAUNCH_PAD;
    *(int32_t*)(padType.data() + BLD_TYPE_SUBTYPE_OFF) = (int32_t)BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL;
    std::vector<BYTE> pad2(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> pad1(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(pad2.data() + BLD_TYPE_OFFSET) = padType.data();
    *(float*)(pad2.data() + OBJ_POS_OFFSET) = +320.0f; // State 2
    *(BYTE**)(pad1.data() + BLD_TYPE_OFFSET) = padType.data();
    *(float*)(pad1.data() + OBJ_POS_OFFSET) = -320.0f; // State 1

    std::vector<BYTE> s1VehA(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> s1VehB(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> s2Rocket(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(s1VehA.data() + VEH_ID_OFFSET) = 6701u;
    *(unsigned*)(s1VehB.data() + VEH_ID_OFFSET) = 6702u;
    *(unsigned*)(s2Rocket.data() + VEH_ID_OFFSET) = 6703u;
    *(float*)(s2Rocket.data() + VEH_POS_OFFSET) = +320.0f;
    *(U64*)(s2Rocket.data() + VEH_HOME_OFFSET) = (U64)(uintptr_t)pad2.data();
    *(U64*)(s2Rocket.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad2.data();
    *(int32_t*)(s2Rocket.data() + VEH_ORBIT_INDEX_OFFSET) = -1;
    *(int32_t*)(s2Rocket.data() + VEH_ROCKET_FLIGHT_STATE_OFFSET) = 0;

    std::vector<BYTE*> blds = { pad1.data(), pad2.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, blds);
    std::vector<BYTE*> vehs = { s1VehA.data(), s1VehB.data(), s2Rocket.data() }; // s2Rocket is at real index 2, filtered index 0!
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);
    TwoStates_RegisterVehicleOwner(6701, 1);
    TwoStates_RegisterVehicleOwner(6702, 1);
    TwoStates_RegisterVehicleOwner(6703, 2);

    BYTE dummyWin[64] = {};
    g_p4McCaptureRocket = s2Rocket.data();
    g_p4McCaptureOk = false;
    TwoStates_InvokeMissionControlWinForTest(world, dummyWin, &Phase4_MockMcCaptureLaunch);
    TEST_CHECK(g_p4McCaptureOk);

    g_p3LaunchCalls = 0;
    TwoStates_BeginCommand(2);
    TEST_CHECK(GameCalls_ApplyCommand(g_p4McCapturedLaunchMsg, g_p4McCapturedLaunchSize));
    TwoStates_EndCommand(2);
    TEST_CHECK(g_p3LaunchCalls == 1);
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 1u && TwoStates_GetOrbitOwner(0) == 2);
    // Replay Launch blocks foreign Launch Pad (s2Rocket moved to pad1 under cmdOwner 2)
    *(U64*)(s2Rocket.data() + VEH_CUR_BLD_OFFSET) = (U64)(uintptr_t)pad1.data();
    TwoStates_BeginCommand(2);
    TEST_CHECK(GameCalls_ApplyCommand(g_p4McCapturedLaunchMsg, g_p4McCapturedLaunchSize));
    TwoStates_EndCommand(2);
    TEST_CHECK(g_p3LaunchCalls == 1);

    // 2. [P0-3] Simultaneous RemoveOrbit + PushOrbit inside one H_SpaceVehOrbitStep attributes the pushed orbit cleanly
    std::vector<BYTE> orbInit(2u * ORBIT_SLOT_STRIDE, 0);
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET) = orbInit.data();
    *(BYTE**)(world + WORLD_ORBITS_VEC_OFFSET + 8) = orbInit.data() + orbInit.size();
    const int orbOwnersInit[2] = { 1, 1 };
    TwoStates_SetOrbitOwnersForTest(orbOwnersInit, 2);
    TwoStates_InvokeSpaceOrbitStepForTest(s2Rocket.data(), &Phase4_MockOrbitStepRemoveAndPush);
    TEST_CHECK(TwoStates_GetOrbitOwnerCountForTest() == 2u);
    TEST_CHECK(TwoStates_GetOrbitOwner(0) == 1 && TwoStates_GetOrbitOwner(1) == 2);

    // 3. [P0-2], [P1-1], [P1-2], [P1-6]: Space milestone mounts in ResearchDailyUpdate/BldUnivStep,
    //    135-tech #120/#121 union in H_SpawnSpaceCargoAtBorder, Mixed-DLC key preservation, and cargo unlock hashing
    std::vector<BYTE> vtypes(1u * WORLD_VEHICLE_TYPE_STRIDE, 0);
    strcpy_s((char*)(vtypes.data() + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "dlc5/vehicles/rocket_soyuz.ini");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET) = vtypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = vtypes.data() + vtypes.size();

    std::vector<BYTE> res135(135u * RESEARCH_ITEM_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = res135.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8) = res135.data() + res135.size();
    TwoStates_BeginCommand(2);
    for (int i = 0; i < 80; ++i) {
        *(float*)(res135.data() + (size_t)i * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;
    }
    *(float*)(res135.data() + 120u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;
    *(float*)(res135.data() + 121u * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;
    TwoStates_EndCommand(2);

    g_p4BorderCargoCalls = 0;
    g_p4BorderSeenProg120 = g_p4BorderSeenProg121 = 0.0f;
    TwoStates_InvokeSpawnSpaceCargoForTest(world, &Phase4_MockSpawnBorder135);
    TEST_CHECK(g_p4BorderCargoCalls == 1);
    TEST_CHECK_NEAR(g_p4BorderSeenProg120, 1.0f, 1e-4f);
    TEST_CHECK_NEAR(g_p4BorderSeenProg121, 1.0f, 1e-4f);

    TwoStates_SetSpaceMilestoneForTest(1, "dlc5/vehicles/uninstalled_dlc_craft.ini", 2, 1, 1);
    std::vector<BYTE> univ2(BUILDING_STRUCT_BYTES, 0);
    *(float*)(univ2.data() + OBJ_POS_OFFSET) = +350.0f;
    TwoStates_BldUnivStepForTest(world, univ2.data());
    TwoStates_ResearchDailyUpdateForTest(world);
    TEST_CHECK(TwoStates_GetSpaceMilestoneMountStateForTest() == 2);

    std::vector<BYTE> s1Soyuz(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(s1Soyuz.data() + VEH_ID_OFFSET) = 6799u;
    *(BYTE**)(s1Soyuz.data() + VEH_TYPE_PTR_OFFSET) = vtypes.data();
    g_p4SpawnedVehPtr = s1Soyuz.data();
    TwoStates_InvokeProdLineStepForTest(world, pad1.data(), &Phase4_MockProdLineStep);
    int ub = 0, uo = 0, ur = 0;
    TEST_CHECK(TwoStates_GetSpaceMilestoneForTest(1, "dlc5/vehicles/uninstalled_dlc_craft.ini", &ub, &uo, &ur));
    TEST_CHECK(ub == 2 && uo == 1 && ur == 1); // Mixed-DLC absent key NOT wiped by CaptureWorldSpaceMilestonesToSlice!

    // [P1-6] spaceCargoUnlocks[2] mutates hashResearch in Two-States AND One-State modes
    StateSummary rHashBefore = {}, rHashAfter = {};
    TEST_CHECK(StateHash_Compute(&rHashBefore));
    TwoStates_BeginCommand(1);
    world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] = 1;
    TwoStates_EndCommand(1);
    TEST_CHECK(StateHash_Compute(&rHashAfter));
    TEST_CHECK(rHashBefore.hashResearch != rHashAfter.hashResearch);

    TwoStates_OnSessionEnded();
    world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] = 0;
    StateSummary spBefore = {}, spAfter = {};
    TEST_CHECK(StateHash_Compute(&spBefore));
    world[WORLD_SPACE_CARGO_WEST_UNLOCK_OFF] = 1;
    TEST_CHECK(StateHash_Compute(&spAfter));
    TEST_CHECK(spBefore.hashResearch != spAfter.hashResearch);

    ResetMockWorldMemory();
    return true;
}

bool Test_SpaceProgramPhase4_TwoStatesSpaceRace()
{
    return SubTest_Phase4_HardeningAndRvaRegressions() &&
           SubTest_Phase4_MilestonesAndResearchUnlockIsolation() &&
           SubTest_Phase4_OrbitPayoutAndPopupSuppression() &&
           SubTest_Phase4_OrbitOwnershipAndCrashSiteGuards() &&
           SubTest_Phase4_MissionControlHudAndBorderContracts() &&
           SubTest_Phase4_StateHashPersistenceAndSpIsolation() &&
           SubTest_Phase4_ReviewRegressions();
}
