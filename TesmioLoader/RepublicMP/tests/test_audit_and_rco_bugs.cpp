// SPDX-License-Identifier: GPL-3.0-only
// Regression test suites #51-#55, #60, #61:
//   - R2/R3/R1-second-pass sync bugs (#51-#53)
//   - M1 hook fixes & Run/kSites audit regressions (#54)
//   - Audit-7 sync fixes (#55)
//   - Hotfix v1.7.1 (#60) and RCO Rail Construction fixes (#61)

#include "test_harness.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <mutex>

// ---------------------------------------------------------------------------
// Test 51: Milestone M1 — R2 Critical & High Sync Bugs (#1–#4)
// ---------------------------------------------------------------------------

static int g_m1DenomMockCalls = 0;
static BYTE* g_m1DenomLastWorld = nullptr;
static void __fastcall MockCurrencyDenomination(BYTE* world)
{
    ++g_m1DenomMockCalls;
    g_m1DenomLastWorld = world;
}

static int g_m1AutoRepMockCalls = 0;
static BYTE* g_m1AutoRepLastMgr = nullptr;
static BYTE* g_m1AutoRepLastTarget = nullptr;
static BYTE* g_m1AutoRepLastNew = nullptr;
static void __fastcall MockSetAutoReplace(BYTE* mgr, BYTE* targetType, BYTE* newType)
{
    ++g_m1AutoRepMockCalls;
    g_m1AutoRepLastMgr = mgr;
    g_m1AutoRepLastTarget = targetType;
    g_m1AutoRepLastNew = newType;
}

static int g_m1LinesAssignMockCalls = 0;
static BYTE* g_m1LinesAssignLastMgr = nullptr;
static BYTE* g_m1LinesAssignLastLine = nullptr;
static BYTE* g_m1LinesAssignLastVeh = nullptr;
static unsigned char g_m1LinesAssignLastFlag = 0;
static BYTE* g_m1LinesAssignLastZero = (BYTE*)0xDEAD;
static void __fastcall MockLinesAssignVehicle(BYTE* mgr, BYTE* line, BYTE* veh, unsigned char flag, BYTE* zero)
{
    ++g_m1LinesAssignMockCalls;
    g_m1LinesAssignLastMgr = mgr;
    g_m1LinesAssignLastLine = line;
    g_m1LinesAssignLastVeh = veh;
    g_m1LinesAssignLastFlag = flag;
    g_m1LinesAssignLastZero = zero;
}

static bool SubTest_M1_Bug1_CurrencyDenomination()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    GameCalls_ResetSessionState();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    TEST_CHECK(SITE_DENOMINATION_RVA == 0x7FC870u);
    TEST_CHECK(FN_CURRENCY_DENOMINATION_RVA == 0x2F9E50u);
    TEST_CHECK(WINDOW_POPUP_TYPE_OFFSET == 0x34u);
    TEST_CHECK(WINDOW_POPUP_TYPE_DENOMINATION == 0x13);
    TEST_CHECK(CURRENCY_DENOMINATION_DIVISOR == 100.0);
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_DENOMINATION_RVA, FN_CURRENCY_DENOMINATION_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_DENOMINATION_RVA), "W") == 0);

    // Null-pointer safety
    TwoStates_CloseDenominationPopups(nullptr);
    TwoStates_ApplyDenomination(nullptr);

    // Resource table & history buffer
    std::vector<BYTE> resTable(4 * WORLD_RESOURCE_STRIDE, 0);
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x00) = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x08) = resTable.data() + resTable.size();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 0x10) = resTable.data() + resTable.size();
    const uintptr_t resSteel = (uintptr_t)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    const uintptr_t resCoal  = (uintptr_t)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);

    double initRub[2] = { 500000.0, 200000.0 };
    double initUsd[2] = { 100000.0,  50000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_ResetEconStatsForTest(world);

    // Period 1 -> push into history (resVecs[0] must divide moneyValue only, resVecs[8] must stay untouched)
    TwoStates_BeginCommand(1);
    {
        SovietStatResEntry row0 = { resSteel, 40.0f, 8000.0f };
        SovietStatResEntry row8 = { resCoal,  25.0f, 5000.0f };
        TwoStates_LedgerAddForTest(world, &row0, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
        TwoStates_LedgerAddForTest(world, &row8, curStat + STAT_RES_VEC_BASE_OFF + 8 * STAT_RES_VEC_STRIDE, 1.0f);
        *(float*)(curStat + STAT_VEHICLES_OFF + 4) += 15000.0f;
        *(float*)(curStat + STAT_TOURISM_OFF + 4 * sizeof(float)) += 4200.0f;
        *(double*)(world + WORLD_RUB_OFFSET) -= 15000.0; // seeds g_netTradeRub[0]
        *(double*)(world + WORLD_USD_OFFSET) -= 2000.0;  // seeds g_netTradeUsd[0]
    }
    TwoStates_EndCommand(1);

    std::vector<BYTE> histBuf(STAT_RECORD_SIZE, 0);
    memcpy(histBuf.data(), curStat, STAT_RECORD_SIZE);
    *(int32_t*)(histBuf.data() + 0) = 31;
    *(int32_t*)(histBuf.data() + 4) = 1965;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x00) = histBuf.data();
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x08) = histBuf.data() + STAT_RECORD_SIZE;
    *(BYTE**)(world + WORLD_STAT_HISTORY_OFFSET + 0x10) = histBuf.data() + histBuf.size();
    TwoStates_StatRecordClearForTest(nullptr, curStat);
    TEST_CHECK(TwoStates_GetStateEconHistoryCount(1) == 1);

    // Period 2 current econ activity + integer extraCount invariant check
    TwoStates_BeginCommand(2);
    {
        SovietStatResEntry row0 = { resSteel, 12.5f, 3500.0f };
        SovietStatResEntry row8 = { resCoal,  18.0f, 2200.0f };
        TwoStates_LedgerAddForTest(world, &row0, curStat + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE, 1.0f);
        TwoStates_LedgerAddForTest(world, &row8, curStat + STAT_RES_VEC_BASE_OFF + 8 * STAT_RES_VEC_STRIDE, 1.0f);
        *(float*)(curStat + STAT_LOANS_OFF + 8) += 9000.0f;
    }
    TwoStates_EndCommand(2);
    TwoStates_SetStateEconExtraCountForTest(1, 0, 77);

    // Open windows: one 0x13 denomination popup and one 0x05 non-denomination popup
    std::vector<BYTE> winDenom(0x40, 0);
    std::vector<BYTE> winOther(0x40, 0);
    *(int32_t*)(winDenom.data() + WINDOW_POPUP_TYPE_OFFSET) = WINDOW_POPUP_TYPE_DENOMINATION;
    *(int32_t*)(winOther.data() + WINDOW_POPUP_TYPE_OFFSET) = 0x05;
    std::vector<BYTE*> openWins = { winDenom.data(), winOther.data() };
    WriteMockVectorHeader(world + WORLD_OPEN_WINDOWS_BEGIN_OFFSET, openWins);

    const double rub1Before = TwoStates_GetRubles(1);
    const double rub2Before = TwoStates_GetRubles(2);
    const double usd1Before = TwoStates_GetDollars(1);
    const double usd2Before = TwoStates_GetDollars(2);
    double netRub1Before = 0.0, netUsd1Before = 0.0;
    TwoStates_GetNetTradeForTest(1, &netRub1Before, &netUsd1Before);

    TwoStates_ApplyDenomination(world);

    // Treasuries & world money divided by 100.0
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), rub1Before / 100.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), rub2Before / 100.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), usd1Before / 100.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), usd2Before / 100.0, 1e-4);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), (rub1Before + rub2Before) / 100.0, 1e-4);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), (usd1Before + usd2Before) / 100.0, 1e-4);

    double netRub1After = 0.0, netUsd1After = 0.0;
    TwoStates_GetNetTradeForTest(1, &netRub1After, &netUsd1After);
    TEST_CHECK_NEAR(netRub1After, netRub1Before / 100.0, 1e-4);
    TEST_CHECK_NEAR(netUsd1After, netUsd1Before / 100.0, 1e-4);

    // Current econ: resVecs[0] moneyValue / 100 (amountTons unchanged), resVecs[8] untouched, loans / 100, extraCount unchanged
    float tons = 0.0f, val = 0.0f;
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 0, resSteel, &tons, &val));
    TEST_CHECK_NEAR(tons, 12.5f, 1e-3f);
    TEST_CHECK_NEAR(val, 35.0f, 1e-2f);
    TEST_CHECK(TwoStates_GetStateEconResEntry(2, 8, resCoal, &tons, &val));
    TEST_CHECK_NEAR(tons, 18.0f, 1e-3f);
    TEST_CHECK_NEAR(val, 2200.0f, 1e-2f);

    float loans2[4] = {};
    TwoStates_GetStateEconFloat4ForTest(2, true, loans2);
    TEST_CHECK_NEAR(loans2[2], 90.0f, 1e-2f);
    TEST_CHECK(TwoStates_GetStateEconExtraCountForTest(1, 0) == 77);

    // Historical econ via TwoStates_UiBegin: resVecs[0] moneyValue / 100, resVecs[8] untouched
    TwoStates_UiBegin();
    {
        SovietStatResEntry* h0b = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE + 0x00);
        SovietStatResEntry* h0e = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 0 * STAT_RES_VEC_STRIDE + 0x08);
        SovietStatResEntry* h8b = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 8 * STAT_RES_VEC_STRIDE + 0x00);
        SovietStatResEntry* h8e = *(SovietStatResEntry**)(histBuf.data() + STAT_RES_VEC_BASE_OFF + 8 * STAT_RES_VEC_STRIDE + 0x08);
        TEST_CHECK(h0b && h0e == h0b + 1);
        TEST_CHECK_NEAR(h0b[0].amountTons, 40.0f, 1e-3f);
        TEST_CHECK_NEAR(h0b[0].moneyValue, 80.0f, 1e-2f);
        TEST_CHECK(h8b && h8e == h8b + 1);
        TEST_CHECK_NEAR(h8b[0].amountTons, 25.0f, 1e-3f);
        TEST_CHECK_NEAR(h8b[0].moneyValue, 5000.0f, 1e-2f);
    }
    TwoStates_UiEnd();

    // Only 0x13 popup closed; 0x05 untouched
    TEST_CHECK(winDenom[0] == 1);
    TEST_CHECK(winOther[0] == 0);

    // GameCalls_IsDenominationCommand + 150-tick cooldown dedup via GameCalls_ApplyCommand
    g_m1DenomMockCalls = 0;
    g_m1DenomLastWorld = nullptr;
    WriteAbsJmp12(g_mockExeBuffer + FN_CURRENCY_DENOMINATION_RVA, (void*)&MockCurrencyDenomination);

    Commands_SetSession(COMMANDS_HOST);
    std::vector<BYTE> msgBuf(256, 0);
    size_t msgSize = 0;
    const unsigned long long denomArgs[7] = { (unsigned long long)(uintptr_t)world, 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_DENOMINATION_RVA, denomArgs, msgBuf.data(), msgBuf.size(), &msgSize));
    TEST_CHECK(GameCalls_IsDenominationCommand(msgBuf.data(), msgSize));
    TEST_CHECK(!GameCalls_IsDenominationCommand(msgBuf.data(), msgSize - 1));
    TEST_CHECK(!GameCalls_IsDenominationCommand(nullptr, msgSize));

    std::vector<BYTE> otherMsgBuf(256, 0);
    size_t otherMsgSize = 0;
    std::vector<BYTE> bldDummy(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE*> bldPtrs = { bldDummy.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, bldPtrs);
    const unsigned long long factoryArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldDummy.data(), 1, 1, 0, 0, 0 };
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_FACTORY_REPLACE_MACHINES_2_RVA, factoryArgs, otherMsgBuf.data(), otherMsgBuf.size(), &otherMsgSize));
    TEST_CHECK(!GameCalls_IsDenominationCommand(otherMsgBuf.data(), otherMsgSize));

    const double rub1CmdBefore = TwoStates_GetRubles(1);
    TEST_CHECK(GameCalls_ApplyCommand(msgBuf.data(), msgSize));
    TEST_CHECK(g_m1DenomMockCalls == 1);
    TEST_CHECK(g_m1DenomLastWorld == world);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), rub1CmdBefore / 100.0, 1e-6);

    // Second simultaneous click on same tick: skipped within 150-tick cooldown, still closes open 0x13 popup
    winDenom[0] = 0;
    TEST_CHECK(GameCalls_ApplyCommand(msgBuf.data(), msgSize));
    TEST_CHECK(g_m1DenomMockCalls == 1);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), rub1CmdBefore / 100.0, 1e-6);
    TEST_CHECK(winDenom[0] == 1);

    // Singleplayer isolation (!Commands_TakeUi()): calls 0x2F9E50 directly without TwoStates
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    unsigned long long ret = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_DENOMINATION_RVA, denomArgs, &ret));
    TEST_CHECK(g_m1DenomMockCalls == 2);
    TEST_CHECK(!TwoStates_IsActive());

    return true;
}

static bool SubTest_M1_Bug2_FactoryReplaceMachines()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(SITE_FACTORY_REPLACE_MACHINES_1_RVA == 0x7762E4u);
    TEST_CHECK(SITE_FACTORY_REPLACE_MACHINES_2_RVA == 0x776557u);
    TEST_CHECK(RVA_START_PLANNED_CONSTRUCTION == 0x1A8D90u);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_FACTORY_REPLACE_MACHINES_1_RVA, RVA_START_PLANNED_CONSTRUCTION));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_FACTORY_REPLACE_MACHINES_2_RVA, RVA_START_PLANNED_CONSTRUCTION));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_FACTORY_REPLACE_MACHINES_1_RVA), "WObb") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_FACTORY_REPLACE_MACHINES_2_RVA), "WObb") == 0);

    std::vector<BYTE> factoryWest(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> factoryEast(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> officeWest(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> officeEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)factoryWest.data() = 101;
    *(float*)(factoryWest.data() + OBJ_POS_OFFSET) = -500.0f;
    *(int*)factoryEast.data() = 102;
    *(float*)(factoryEast.data() + OBJ_POS_OFFSET) = 500.0f;
    *(int*)officeWest.data() = 103;
    *(float*)(officeWest.data() + OBJ_POS_OFFSET) = -450.0f;
    *(int*)officeEast.data() = 104;
    *(float*)(officeEast.data() + OBJ_POS_OFFSET) = 450.0f;

    std::vector<BYTE*> blds = { factoryWest.data(), factoryEast.data(), officeWest.data(), officeEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    // Roundtrip "WObb" preserves r8d (0/1) AND r9b (1)
    const unsigned long long inArgs1[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)factoryWest.data(), 0, 1, 0, 0, 0 };
    unsigned long long outArgs1[7] = {};
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_FACTORY_REPLACE_MACHINES_1_RVA, inArgs1, outArgs1));
    TEST_CHECK(outArgs1[0] == (unsigned long long)(uintptr_t)world);
    TEST_CHECK(outArgs1[1] == (unsigned long long)(uintptr_t)factoryWest.data());
    TEST_CHECK(outArgs1[2] == 0);
    TEST_CHECK(outArgs1[3] == 1);

    const unsigned long long inArgs2[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)factoryWest.data(), 1, 1, 0, 0, 0 };
    unsigned long long outArgs2[7] = {};
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_FACTORY_REPLACE_MACHINES_2_RVA, inArgs2, outArgs2));
    TEST_CHECK(outArgs2[2] == 1);
    TEST_CHECK(outArgs2[3] == 1);

    // Critical TwoStates collision check:
    // factory ReplaceMachines (0x7762E4 / 0x776557) must check args[1] (the factory), NOT world+0xD298 (activeOffice)!
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    *(BYTE**)(world + WORLD_ACTIVE_WINDOW_BLD_OFF) = officeEast.data();
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_FACTORY_REPLACE_MACHINES_1_RVA, inArgs1));
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_FACTORY_REPLACE_MACHINES_2_RVA, inArgs2));

    *(BYTE**)(world + WORLD_ACTIVE_WINDOW_BLD_OFF) = officeWest.data();
    const unsigned long long eastFactoryArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)factoryEast.data(), 1, 1, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_FACTORY_REPLACE_MACHINES_1_RVA, eastFactoryArgs));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_FACTORY_REPLACE_MACHINES_2_RVA, eastFactoryArgs));

    // Meanwhile SITE_CONSTRUCT_TOOL_START_PLAN_RVA (0x1D9F7D) still checks activeOffice (officeWest allows building on east site)
    const unsigned long long planArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)factoryEast.data(), 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_CONSTRUCT_TOOL_START_PLAN_RVA, planArgs));

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_M1_Bug3_AutoReplaceClear()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(SITE_AUTO_REPLACE_CLEAR_RVA == 0x80F4FEu);
    TEST_CHECK(RVA_SET_AUTO_REPLACE == 0x6148A0u);
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_AUTO_REPLACE_CLEAR_RVA, RVA_SET_AUTO_REPLACE));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_AUTO_REPLACE_CLEAR_RVA), "Mxx") == 0);

    std::vector<BYTE> vTypes(2 * WORLD_VEHICLE_TYPE_STRIDE, 0);
    BYTE* typeA = vTypes.data();
    BYTE* typeB = vTypes.data() + WORLD_VEHICLE_TYPE_STRIDE;
    snprintf((char*)(typeA + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "vehicles/truck_a.ini");
    snprintf((char*)(typeB + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "media_soviet/vehicles/truck_b.ini");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x00) = vTypes.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x08) = vTypes.data() + vTypes.size();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x10) = vTypes.data() + vTypes.size();

    std::vector<BYTE> dummyMgr(64, 0);
    *(BYTE**)(world + WORLD_AUTO_REPLACE_MGR_OFFSET) = dummyMgr.data();

    g_m1AutoRepMockCalls = 0;
    g_m1AutoRepLastMgr = nullptr;
    g_m1AutoRepLastTarget = nullptr;
    g_m1AutoRepLastNew = (BYTE*)0xDEAD;
    WriteAbsJmp12(g_mockExeBuffer + RVA_SET_AUTO_REPLACE, (void*)&MockSetAutoReplace);

    // Positive: ReplayAutoReplace clear (kind=0xB, used=2) resolves targetType by scriptName even when typeIndex differs
    PurchaseMsg clearMsg = {};
    clearMsg.kind = 0xB;
    clearMsg.used = 2;
    clearMsg.typeIndex = 99;
    clearMsg.typeCheck = 0x12345679u;
    clearMsg.resIndex = -1;
    snprintf(clearMsg.scriptName, sizeof(clearMsg.scriptName), "vehicles/truck_b.ini");
    TEST_CHECK(Purchase_ApplyAutoReplaceForTest(clearMsg));
    TEST_CHECK(g_m1AutoRepMockCalls == 1);
    TEST_CHECK(g_m1AutoRepLastMgr == dummyMgr.data());
    TEST_CHECK(g_m1AutoRepLastTarget == typeB);
    TEST_CHECK(g_m1AutoRepLastNew == nullptr);

    // Negative checks: invalid used, invalid kind, unknown scriptName, null manager
    PurchaseMsg badUsed = clearMsg;
    badUsed.used = 1;
    TEST_CHECK(!Purchase_ApplyAutoReplaceForTest(badUsed));

    PurchaseMsg badKind = clearMsg;
    badKind.kind = 1;
    TEST_CHECK(!Purchase_ApplyAutoReplaceForTest(badKind));

    PurchaseMsg unknownType = clearMsg;
    snprintf(unknownType.scriptName, sizeof(unknownType.scriptName), "vehicles/nonexistent.ini");
    TEST_CHECK(!Purchase_ApplyAutoReplaceForTest(unknownType));

    *(BYTE**)(world + WORLD_AUTO_REPLACE_MGR_OFFSET) = nullptr;
    TEST_CHECK(!Purchase_ApplyAutoReplaceForTest(clearMsg));
    *(BYTE**)(world + WORLD_AUTO_REPLACE_MGR_OFFSET) = dummyMgr.data();
    TEST_CHECK(g_m1AutoRepMockCalls == 1);

    // Submit validation (null / out-of-range / unaligned rejected, valid accepted)
    TEST_CHECK(!Purchase_SubmitAutoReplaceClear(nullptr));
    TEST_CHECK(!Purchase_SubmitAutoReplaceClear(typeB + 16));
    TEST_CHECK(!Purchase_SubmitAutoReplaceClear(dummyMgr.data()));
    Commands_SetSession(COMMANDS_HOST);
    TEST_CHECK(Purchase_SubmitAutoReplaceClear(typeB));

    // MP capture via GameCalls_RunSiteForTest: submits command without mutating autoReplaceMgr locally
    const unsigned long long clearArgs[7] = { (unsigned long long)(uintptr_t)dummyMgr.data(), (unsigned long long)(uintptr_t)typeB, 0, 0, 0, 0, 0 };
    unsigned long long ret = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_AUTO_REPLACE_CLEAR_RVA, clearArgs, &ret));
    TEST_CHECK(g_m1AutoRepMockCalls == 1);

    // SP isolation (!Commands_TakeUi()): invokes 0x6148A0(mgr, typeB, nullptr) directly
    Commands_SetSession(COMMANDS_OFF);
    g_m1AutoRepLastTarget = nullptr;
    g_m1AutoRepLastNew = (BYTE*)0xDEAD;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_AUTO_REPLACE_CLEAR_RVA, clearArgs, &ret));
    TEST_CHECK(g_m1AutoRepMockCalls == 2);
    TEST_CHECK(g_m1AutoRepLastMgr == dummyMgr.data());
    TEST_CHECK(g_m1AutoRepLastTarget == typeB);
    TEST_CHECK(g_m1AutoRepLastNew == nullptr);

    return true;
}

static bool SubTest_M1_Bug4_LineAssignFromLinesList()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(SITE_LINES_LIST_ASSIGN_VEHICLE_RVA == 0x35A870u);
    TEST_CHECK(RVA_LINES_ASSIGN_VEHICLE == 0x497AA0u);
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_LINES_LIST_ASSIGN_VEHICLE_RVA, RVA_LINES_ASSIGN_VEHICLE));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_LINES_LIST_ASSIGN_VEHICLE_RVA), "MLVbo") == 0);

    std::vector<BYTE> lineMgr(0x40, 0);
    std::vector<BYTE> lineObj(0x90, 0);
    *(int*)(lineObj.data() + 0x04) = 0x11223344;
    *(int*)(lineObj.data() + 0x08) = 0x55667788;
    std::vector<BYTE*> linesVec = { lineObj.data() };
    WriteMockVectorHeader(lineMgr.data() + 0x00, linesVec);
    WriteMockVectorHeader(lineMgr.data() + 0x18, linesVec);
    *(BYTE**)(world + WORLD_LINES_OFFSET) = lineMgr.data();

    std::vector<BYTE> vehWest(0x1800, 0);
    std::vector<BYTE> vehEast(0x1800, 0);
    *(int*)(vehWest.data() + VEH_ID_OFFSET) = 401;
    *(int*)(vehEast.data() + VEH_ID_OFFSET) = 402;
    std::vector<BYTE*> vehVec = { vehWest.data(), vehEast.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehVec);

    const unsigned long long inArgs[7] = {
        (unsigned long long)(uintptr_t)lineMgr.data(),
        (unsigned long long)(uintptr_t)lineObj.data(),
        (unsigned long long)(uintptr_t)vehWest.data(),
        1, 0, 0, 0
    };
    unsigned long long outArgs[7] = {};
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_LINES_LIST_ASSIGN_VEHICLE_RVA, inArgs, outArgs));
    TEST_CHECK(outArgs[0] == (unsigned long long)(uintptr_t)lineMgr.data());
    TEST_CHECK(outArgs[1] == (unsigned long long)(uintptr_t)lineObj.data());
    TEST_CHECK(outArgs[2] == (unsigned long long)(uintptr_t)vehWest.data());
    TEST_CHECK(outArgs[3] == 1);
    TEST_CHECK(outArgs[4] == 0);

    // TwoStates vehicle ownership check ('V')
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_RegisterVehicleOwner(401, 1);
    TwoStates_RegisterVehicleOwner(402, 2);

    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_LINES_LIST_ASSIGN_VEHICLE_RVA, inArgs));
    const unsigned long long eastArgs[7] = {
        (unsigned long long)(uintptr_t)lineMgr.data(),
        (unsigned long long)(uintptr_t)lineObj.data(),
        (unsigned long long)(uintptr_t)vehEast.data(),
        1, 0, 0, 0
    };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_LINES_LIST_ASSIGN_VEHICLE_RVA, eastArgs));
    TwoStates_OnSessionEnded();

    // SP isolation (!Commands_TakeUi()): passes through directly to 0x497AA0(linesMgr, line, veh, 1, 0)
    g_m1LinesAssignMockCalls = 0;
    WriteAbsJmp12(g_mockExeBuffer + RVA_LINES_ASSIGN_VEHICLE, (void*)&MockLinesAssignVehicle);
    unsigned long long ret = 0;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_LINES_LIST_ASSIGN_VEHICLE_RVA, inArgs, &ret));
    TEST_CHECK(g_m1LinesAssignMockCalls == 1);
    TEST_CHECK(g_m1LinesAssignLastMgr == lineMgr.data());
    TEST_CHECK(g_m1LinesAssignLastLine == lineObj.data());
    TEST_CHECK(g_m1LinesAssignLastVeh == vehWest.data());
    TEST_CHECK(g_m1LinesAssignLastFlag == 1);
    TEST_CHECK(g_m1LinesAssignLastZero == nullptr);

    return true;
}

bool Test_R2Bugs_DenomReplaceAutoReplaceLineSet()
{
    printf("    [Bug #1: Currency Denomination (0x7FC870 -> 0x2F9E50, /100.0, TwoStates & Popups)] ...\n"); fflush(stdout);
    if (!SubTest_M1_Bug1_CurrencyDenomination()) return false;

    printf("    [Bug #2: Factory ReplaceMachines (0x7762E4 & 0x776557 WObb, TwoStates Collision)] ...\n"); fflush(stdout);
    if (!SubTest_M1_Bug2_FactoryReplaceMachines()) return false;

    printf("    [Bug #3: Auto-Replace Rule Clear (0x80F4FE -> 0x6148A0, kind=0xB used=2)] ...\n"); fflush(stdout);
    if (!SubTest_M1_Bug3_AutoReplaceClear()) return false;

    printf("    [Bug #4: Line Assignment from Lines List (0x35A870 -> 0x497AA0, MLVbo)] ...\n"); fflush(stdout);
    if (!SubTest_M1_Bug4_LineAssignFromLinesList()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 52: Milestone M2 — R3 Medium Sync Bugs (#5–#7)
// ---------------------------------------------------------------------------

static int g_m2DetachMockCalls = 0;
static BYTE* g_m2DetachLastWorld = nullptr;
static BYTE* g_m2DetachLastCitizen = nullptr;
static void __fastcall MockDetachPersonalCar(BYTE* world, BYTE* citizen)
{
    ++g_m2DetachMockCalls;
    g_m2DetachLastWorld = world;
    g_m2DetachLastCitizen = citizen;
    if (!citizen) return;
    BYTE* veh = *(BYTE**)(citizen + CIT_PERSONAL_CAR_OFFSET);
    *(BYTE**)(citizen + CIT_PERSONAL_CAR_OFFSET) = nullptr;
    if (veh) {
        *(BYTE**)(veh + VEH_OWNER_CITIZEN_OFFSET) = nullptr;
    }
}

static int g_m2DepotMockCalls = 0;
static BYTE* g_m2DepotLastWorld = nullptr;
static BYTE* g_m2DepotLastVeh = nullptr;
static unsigned char g_m2DepotLastArgs[4] = {};
static void __fastcall MockSendToDepot(BYTE* world, BYTE* veh, unsigned char a3, unsigned char a4, unsigned char a5, unsigned char a6)
{
    ++g_m2DepotMockCalls;
    g_m2DepotLastWorld = world;
    g_m2DepotLastVeh = veh;
    g_m2DepotLastArgs[0] = a3;
    g_m2DepotLastArgs[1] = a4;
    g_m2DepotLastArgs[2] = a5;
    g_m2DepotLastArgs[3] = a6;
}

static int g_m2InitSlidersMockCalls = 0;
static unsigned char g_m2InitSlidersLastFlag = 0xFF;
static void __fastcall MockInitResSliders(BYTE* /*world*/, BYTE* /*bld*/, unsigned char zeroThresholds)
{
    ++g_m2InitSlidersMockCalls;
    g_m2InitSlidersLastFlag = zeroThresholds;
}

static int g_m2FactoryQueueMockCalls = 0;
static BYTE* g_m2FactoryQueueLastBld = nullptr;
static void __fastcall MockRecomputeFactoryQueueLimit(BYTE* /*unused*/, BYTE* bld)
{
    ++g_m2FactoryQueueMockCalls;
    g_m2FactoryQueueLastBld = bld;
    if (!bld) return;
    BYTE* qBeg = *(BYTE**)(bld + BLD_FACTORY_QUEUE_BEGIN_OFFSET);
    BYTE* qEnd = *(BYTE**)(bld + BLD_FACTORY_QUEUE_END_OFFSET);
    if (!qBeg || qEnd <= qBeg) return;
    const size_t count = (size_t)(qEnd - qBeg) / FACTORY_QUEUE_ENTRY_STRIDE;
    const int maxWorkers = *(const int*)(bld + BLD_FACTORY_MAX_WORKERS_OFFSET);
    for (size_t i = 0; i < count; ++i) {
        BYTE* entry = qBeg + i * FACTORY_QUEUE_ENTRY_STRIDE;
        const BYTE* tmplVeh = *(const BYTE* const*)(entry + 0x20);
        int limit = tmplVeh ? *(const int*)tmplVeh : 0;
        if (maxWorkers > 0 && limit > maxWorkers) limit = maxWorkers;
        if (limit < 1) limit = 1;
        *(int*)(entry + 0x0C) = limit;
    }
}

static bool SubTest_M2_Bug5_RecallPersonalCars()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(RVA_DETACH_PERSONAL_CAR == 0x831DF0u);
    TEST_CHECK(SITE_RECALL_DETACH_PERSONAL_CAR_RVA == 0x7A1C28u);
    TEST_CHECK(SITE_RECALL_SEND_TO_DEPOT_RVA == 0x7A1C4Bu);
    TEST_CHECK(VEH_OWNER_CITIZEN_OFFSET == 0x04E8u);
    TEST_CHECK(CIT_PERSONAL_CAR_OFFSET == 0x0688u);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA, RVA_DETACH_PERSONAL_CAR));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_RECALL_SEND_TO_DEPOT_RVA, 0x3FBD00u));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA), "WV") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_RECALL_SEND_TO_DEPOT_RVA), "WVbbbb") == 0);

    g_m2DetachMockCalls = 0;
    g_m2DetachLastWorld = nullptr;
    g_m2DetachLastCitizen = nullptr;
    g_m2DepotMockCalls = 0;
    g_m2DepotLastWorld = nullptr;
    g_m2DepotLastVeh = nullptr;
    memset(g_m2DepotLastArgs, 0, sizeof(g_m2DepotLastArgs));
    WriteAbsJmp12(g_mockExeBuffer + RVA_DETACH_PERSONAL_CAR, (void*)&MockDetachPersonalCar);
    WriteAbsJmp12(g_mockExeBuffer + 0x3FBD00u, (void*)&MockSendToDepot);

    std::vector<BYTE> bldWest(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> bldEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)bldWest.data() = 201;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -400.0f;
    *(int*)bldEast.data() = 202;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = 400.0f;
    std::vector<BYTE*> blds = { bldWest.data(), bldEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    std::vector<BYTE> vehWest(0x1800, 0);
    std::vector<BYTE> vehEast(0x1800, 0);
    std::vector<BYTE> citWest(0x800, 0);
    std::vector<BYTE> citEast(0x800, 0);
    *(int*)(vehWest.data() + VEH_ID_OFFSET) = 501;
    *(BYTE**)(vehWest.data() + VEH_HOME_OFFSET) = bldWest.data();
    *(BYTE**)(vehWest.data() + VEH_OWNER_CITIZEN_OFFSET) = citWest.data();
    *(BYTE**)(citWest.data() + CIT_PERSONAL_CAR_OFFSET) = vehWest.data();

    *(int*)(vehEast.data() + VEH_ID_OFFSET) = 502;
    *(BYTE**)(vehEast.data() + VEH_HOME_OFFSET) = bldEast.data();
    *(BYTE**)(vehEast.data() + VEH_OWNER_CITIZEN_OFFSET) = citEast.data();
    *(BYTE**)(citEast.data() + CIT_PERSONAL_CAR_OFFSET) = vehEast.data();

    std::vector<BYTE*> vehs = { vehWest.data(), vehEast.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);

    // Capture converts citizen -> vehWest, rebuild restores vehWest
    const unsigned long long detachInArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)citWest.data(), 0, 0, 0, 0, 0 };
    unsigned long long detachOutArgs[7] = {};
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA, detachInArgs, detachOutArgs));
    TEST_CHECK(detachOutArgs[0] == (unsigned long long)(uintptr_t)world);
    TEST_CHECK(detachOutArgs[1] == (unsigned long long)(uintptr_t)vehWest.data());

    // TwoStates ownership check on converted personal car
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA, detachInArgs));
    const unsigned long long eastDetachArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)citEast.data(), 0, 0, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA, eastDetachArgs));

    // Capture message before running site (for idempotent replay check later)
    std::vector<BYTE> detachMsg(256, 0);
    size_t detachMsgSize = 0;
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA, detachInArgs, detachMsg.data(), detachMsg.size(), &detachMsgSize));

    // MP capture (COMMANDS_HOST): queues both calls without mutating citizen/vehicle locally
    Commands_SetSession(COMMANDS_HOST);
    const unsigned long long depotArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)vehWest.data(), 0, 0, 1, 1, 0 };
    unsigned long long ret = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA, detachInArgs, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_RECALL_SEND_TO_DEPOT_RVA, depotArgs, &ret));
    TEST_CHECK(g_m2DetachMockCalls == 0);
    TEST_CHECK(g_m2DepotMockCalls == 0);
    TEST_CHECK(*(BYTE**)(vehWest.data() + VEH_OWNER_CITIZEN_OFFSET) == citWest.data());
    TEST_CHECK(*(BYTE**)(citWest.data() + CIT_PERSONAL_CAR_OFFSET) == vehWest.data());

    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_m2DetachMockCalls == 1);
    TEST_CHECK(g_m2DetachLastWorld == world);
    TEST_CHECK(g_m2DetachLastCitizen == citWest.data());
    TEST_CHECK(*(BYTE**)(vehWest.data() + VEH_OWNER_CITIZEN_OFFSET) == nullptr);
    TEST_CHECK(*(BYTE**)(citWest.data() + CIT_PERSONAL_CAR_OFFSET) == nullptr);
    TEST_CHECK(g_m2DepotMockCalls == 1);
    TEST_CHECK(g_m2DepotLastWorld == world);
    TEST_CHECK(g_m2DepotLastVeh == vehWest.data());
    TEST_CHECK(g_m2DepotLastArgs[0] == 0 && g_m2DepotLastArgs[1] == 0 &&
               g_m2DepotLastArgs[2] == 1 && g_m2DepotLastArgs[3] == 1);

    // Idempotent second replay after already detached: cleanly skips without calling 0x831DF0
    TEST_CHECK(GameCalls_ApplyCommand(detachMsg.data(), detachMsgSize));
    TEST_CHECK(g_m2DetachMockCalls == 1);

    // Singleplayer isolation (!Commands_TakeUi()): passes straight through to 0x831DF0 and 0x3FBD00
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    *(BYTE**)(vehWest.data() + VEH_OWNER_CITIZEN_OFFSET) = citWest.data();
    *(BYTE**)(citWest.data() + CIT_PERSONAL_CAR_OFFSET) = vehWest.data();
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_RECALL_DETACH_PERSONAL_CAR_RVA, detachInArgs, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_RECALL_SEND_TO_DEPOT_RVA, depotArgs, &ret));
    TEST_CHECK(g_m2DetachMockCalls == 2);
    TEST_CHECK(g_m2DepotMockCalls == 2);
    TEST_CHECK(*(BYTE**)(citWest.data() + CIT_PERSONAL_CAR_OFFSET) == nullptr);

    return true;
}

static bool SubTest_M2_Bug6_AutoBuyExportSliders()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(SITE_BLD_INIT_SLIDERS_DISABLE_RVA == 0x711ACBu);
    TEST_CHECK(SITE_BLD_AUTO_BUY_DISABLE_RVA == 0x711ADDu);
    TEST_CHECK(SITE_AUTO_BUY_DISABLE_AFTER_RVA == 0x711AE2u);
    TEST_CHECK(SITE_AUTO_BUY_DISABLE_SKIP_RVA == 0x711AE9u);
    TEST_CHECK(SITE_BLD_INIT_SLIDERS_CANCEL_RVA == 0x711B60u);
    TEST_CHECK(SITE_MANUAL_BUY_AFTER_RVA == 0x711BF0u);
    TEST_CHECK(SITE_MANUAL_BUY_SKIP_RVA == 0x711BF7u);
    TEST_CHECK(SITE_BLD_AUTO_BUY_APPLY_RVA == 0x711C72u);
    TEST_CHECK(SITE_AUTO_BUY_APPLY_AFTER_RVA == 0x711C77u);
    TEST_CHECK(SITE_AUTO_BUY_APPLY_SKIP_RVA == 0x711C7Eu);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_BLD_INIT_SLIDERS_DISABLE_RVA, BLD_INIT_RES_SLIDERS_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_BLD_INIT_SLIDERS_CANCEL_RVA, BLD_INIT_RES_SLIDERS_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_BLD_AUTO_BUY_DISABLE_RVA, BLD_APPLY_AUTO_BUY_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_BLD_AUTO_BUY_APPLY_RVA, BLD_APPLY_AUTO_BUY_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_BLD_INIT_SLIDERS_DISABLE_RVA), "WBb") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_BLD_INIT_SLIDERS_CANCEL_RVA), "WBb") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_BLD_AUTO_BUY_DISABLE_RVA), "WBI") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_BLD_AUTO_BUY_APPLY_RVA), "WBI") == 0);

    g_m2InitSlidersMockCalls = 0;
    g_m2InitSlidersLastFlag = 0xFF;
    WriteAbsJmp12(g_mockExeBuffer + BLD_INIT_RES_SLIDERS_RVA, (void*)&MockInitResSliders);

    std::vector<BYTE> bldWest(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> bldEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)bldWest.data() = 301;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -350.0f;
    *(int*)bldEast.data() = 302;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = 350.0f;
    std::vector<BYTE*> blds = { bldWest.data(), bldEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    // Setup 1 storage slot with 3 entries on bldWest (non-zero initial +0x0C thresholds)
    std::vector<BYTE> slotBuf(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> bldEntries(3 * 0x10, 0);
    *(float*)(bldEntries.data() + 0 * 0x10 + 0x0C) = 99.0f;
    *(float*)(bldEntries.data() + 1 * 0x10 + 0x0C) = 50.0f;
    *(float*)(bldEntries.data() + 2 * 0x10 + 0x0C) = 25.0f;
    *(BYTE**)(slotBuf.data() + 0x00) = bldEntries.data();
    *(BYTE**)(slotBuf.data() + 0x08) = bldEntries.data() + bldEntries.size();
    *(BYTE**)(slotBuf.data() + 0x10) = bldEntries.data() + bldEntries.size();
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0x00) = slotBuf.data();
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0x08) = slotBuf.data() + slotBuf.size();
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0x10) = slotBuf.data() + slotBuf.size();
    *(int*)(bldWest.data() + BLD_EXPORT_MODE_OFFSET) = 1;

    // Setup UI slider vector at world + WORLD_UI_RES_SLIDERS_OFFSET
    std::vector<BYTE> uiSlot(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> uiEntries(3 * 0x10, 0);
    *(float*)(uiEntries.data() + 0 * 0x10 + 0x0C) = 12.0f;
    *(float*)(uiEntries.data() + 1 * 0x10 + 0x0C) = 0.0f;
    *(float*)(uiEntries.data() + 2 * 0x10 + 0x0C) = 3.5f;
    *(BYTE**)(uiSlot.data() + 0x00) = uiEntries.data();
    *(BYTE**)(uiSlot.data() + 0x08) = uiEntries.data() + uiEntries.size();
    *(BYTE**)(world + WORLD_UI_RES_SLIDERS_OFFSET + 0x00) = uiSlot.data();
    *(BYTE**)(world + WORLD_UI_RES_SLIDERS_OFFSET + 0x08) = uiSlot.data() + uiSlot.size();

    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    Commands_SetSession(COMMANDS_HOST);

    // 1. CALL_LOCAL slider init sites execute immediately in MP without submitting commands
    const unsigned long long initDisableArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldWest.data(), 1, 0, 0, 0, 0 };
    const unsigned long long initCancelArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldWest.data(), 0, 0, 0, 0, 0 };
    unsigned long long ret = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_BLD_INIT_SLIDERS_DISABLE_RVA, initDisableArgs, &ret));
    TEST_CHECK(g_m2InitSlidersMockCalls == 1 && g_m2InitSlidersLastFlag == 1);
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_BLD_INIT_SLIDERS_CANCEL_RVA, initCancelArgs, &ret));
    TEST_CHECK(g_m2InitSlidersMockCalls == 2 && g_m2InitSlidersLastFlag == 0);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // 2. Apply Auto-Buy (0x711C72, RUB=4): skips +7 retAddr, does not mutate locally, then applies at boundary
    const unsigned long long applyRubArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldWest.data(), 4, 0, 0, 0, 0 };
    unsigned long long retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_APPLY_RVA, applyRubArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_SKIP_RVA));
    TEST_CHECK(*(float*)(bldEntries.data() + 0 * 0x10 + 0x0C) == 99.0f);
    TEST_CHECK(*(int*)(bldWest.data() + BLD_EXPORT_MODE_OFFSET) == 1);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    // Reset BLD_INIT_RES_SLIDERS_RVA byte to 0 so ApplyAutoBuy exercises ApplyAutoBuyFallback directly
    g_mockExeBuffer[BLD_INIT_RES_SLIDERS_RVA] = 0;
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK_NEAR(*(float*)(bldEntries.data() + 0 * 0x10 + 0x0C), 12.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bldEntries.data() + 1 * 0x10 + 0x0C), 0.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bldEntries.data() + 2 * 0x10 + 0x0C), 3.5f, 1e-4f);
    TEST_CHECK(*(int*)(bldWest.data() + BLD_AUTO_BUY_MODE_OFFSET) == 4);
    TEST_CHECK(*(int*)(bldWest.data() + BLD_EXPORT_MODE_OFFSET) == 0);

    // 3. Apply Auto-Buy with empty UI slider vector (m.count == 0, USD=3): clears all +0x0C and sets +0x9E8 = 3
    memset(world + WORLD_UI_RES_SLIDERS_OFFSET, 0, 24);
    *(int*)(bldWest.data() + BLD_EXPORT_MODE_OFFSET) = 1;
    const unsigned long long applyUsdArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldWest.data(), 3, 0, 0, 0, 0 };
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_APPLY_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_SKIP_RVA));
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK_NEAR(*(float*)(bldEntries.data() + 0 * 0x10 + 0x0C), 0.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bldEntries.data() + 2 * 0x10 + 0x0C), 0.0f, 1e-4f);
    TEST_CHECK(*(int*)(bldWest.data() + BLD_AUTO_BUY_MODE_OFFSET) == 3);
    TEST_CHECK(*(int*)(bldWest.data() + BLD_EXPORT_MODE_OFFSET) == 0);

    // 4. Disable Auto-Buy (0x711ADD): skips +7 retAddr, clears +0x9E8 = 0 and zeroes +0x0C
    *(float*)(bldEntries.data() + 0 * 0x10 + 0x0C) = 7.0f;
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_DISABLE_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_DISABLE_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_DISABLE_SKIP_RVA));
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(int*)(bldWest.data() + BLD_AUTO_BUY_MODE_OFFSET) == 0);
    TEST_CHECK_NEAR(*(float*)(bldEntries.data() + 0 * 0x10 + 0x0C), 0.0f, 1e-4f);

    // 5. Manual-Buy (0x711BEB) also skips +7 retAddr (0x711BF0 -> 0x711BF7), and TwoStates refuses East building while still advancing retAddr
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_MANUAL_BUY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_MANUAL_BUY_RES_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_MANUAL_BUY_SKIP_RVA));

    const unsigned long long eastArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldEast.data(), 3, 0, 0, 0, 0 };
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_APPLY_RVA, eastArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_SKIP_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // 6. Waiting Lobby blocks capture AND advances +7 retAddr for all three sites
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_DISABLE_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_DISABLE_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_DISABLE_SKIP_RVA));

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_MANUAL_BUY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_MANUAL_BUY_RES_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_MANUAL_BUY_SKIP_RVA));

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_APPLY_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_SKIP_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");

    // 7. Singleplayer isolation (!Commands_TakeUi()): retAddr left untouched (0x711AE2/0x711BF0/0x711C77)
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    g_mockExeBuffer[BLD_APPLY_AUTO_BUY_RVA] = 0xC3;
    g_mockExeBuffer[BLD_MANUAL_BUY_RES_RVA] = 0xC3;

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_DISABLE_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_DISABLE_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_DISABLE_AFTER_RVA));

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_MANUAL_BUY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_MANUAL_BUY_RES_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_MANUAL_BUY_AFTER_RVA));

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_BLD_AUTO_BUY_APPLY_RVA, applyUsdArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_AUTO_BUY_APPLY_AFTER_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    return true;
}

static bool SubTest_M2_Bug7_FactoryQueueWorkerLimit()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    SettingsSync_LockstepReset();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT == 0x1AC720u);
    TEST_CHECK(SITE_FACTORY_QUEUE_LIMIT_DEC_RVA == 0x7522CEu);
    TEST_CHECK(SITE_FACTORY_QUEUE_LIMIT_INC_RVA == 0x75281Bu);
    TEST_CHECK(BLD_FACTORY_MAX_WORKERS_OFFSET == 0x0DF0u);
    TEST_CHECK(BLD_FACTORY_QUEUE_BEGIN_OFFSET == 0x0FF0u);
    TEST_CHECK(BLD_FACTORY_QUEUE_END_OFFSET == 0x0FF8u);
    TEST_CHECK(FACTORY_QUEUE_ENTRY_STRIDE == 0x48u);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_FACTORY_QUEUE_LIMIT_DEC_RVA, RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_FACTORY_QUEUE_LIMIT_INC_RVA, RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_FACTORY_QUEUE_LIMIT_DEC_RVA), "xB") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_FACTORY_QUEUE_LIMIT_INC_RVA), "xB") == 0);

    g_m2FactoryQueueMockCalls = 0;
    g_m2FactoryQueueLastBld = nullptr;
    WriteAbsJmp12(g_mockExeBuffer + RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT, (void*)&MockRecomputeFactoryQueueLimit);

    std::vector<BYTE> bldType(0x400, 0);
    *(int*)(bldType.data() + 0x360) = 0x15;
    std::vector<BYTE> factoryWest(BUILDING_STRUCT_BYTES, 0);
    *(int*)factoryWest.data() = 401;
    *(BYTE**)(factoryWest.data() + 0x318) = bldType.data();
    *(float*)(factoryWest.data() + OBJ_POS_OFFSET + 0) = -200.0f;
    *(float*)(factoryWest.data() + OBJ_POS_OFFSET + 8) = -150.0f;
    TEST_CHECK(SettingsSync_OwnsBuildingWord(factoryWest.data(), BLD_FACTORY_MAX_WORKERS_OFFSET));

    std::vector<BYTE> nonFactoryBld(BUILDING_STRUCT_BYTES, 0);
    *(int*)nonFactoryBld.data() = 402;
    *(BYTE**)(nonFactoryBld.data() + 0x318) = bldType.data();
    *(float*)(nonFactoryBld.data() + OBJ_POS_OFFSET + 0) = -260.0f;
    *(float*)(nonFactoryBld.data() + OBJ_POS_OFFSET + 8) = -150.0f;

    std::vector<BYTE> tmplVehA(16, 0);
    std::vector<BYTE> tmplVehB(16, 0);
    *(int*)tmplVehA.data() = 10;
    *(int*)tmplVehB.data() = 30;
    std::vector<BYTE> queueBuf(2 * FACTORY_QUEUE_ENTRY_STRIDE, 0);
    *(BYTE**)(queueBuf.data() + 0 * FACTORY_QUEUE_ENTRY_STRIDE + 0x20) = tmplVehA.data();
    *(int*)(queueBuf.data() + 0 * FACTORY_QUEUE_ENTRY_STRIDE + 0x0C) = 10;
    *(BYTE**)(queueBuf.data() + 1 * FACTORY_QUEUE_ENTRY_STRIDE + 0x20) = tmplVehB.data();
    *(int*)(queueBuf.data() + 1 * FACTORY_QUEUE_ENTRY_STRIDE + 0x0C) = 20;

    *(int*)(factoryWest.data() + BLD_FACTORY_MAX_WORKERS_OFFSET) = 20;
    *(BYTE**)(factoryWest.data() + BLD_FACTORY_QUEUE_BEGIN_OFFSET) = queueBuf.data();
    *(BYTE**)(factoryWest.data() + BLD_FACTORY_QUEUE_END_OFFSET) = queueBuf.data() + queueBuf.size();
    *(BYTE**)(factoryWest.data() + BLD_FACTORY_QUEUE_END_OFFSET + 8) = queueBuf.data() + queueBuf.size();

    std::vector<BYTE*> blds = { factoryWest.data(), nonFactoryBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    // 1. SettingsSync FOLLOW_FACTORY_QUEUE updates entry+0x0C when bld+0xDF0 changes on active queue
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    Commands_SetSession(COMMANDS_HOST);
    SettingsSync_LockstepBaseline();

    *(int*)(factoryWest.data() + BLD_FACTORY_MAX_WORKERS_OFFSET) = 12;
    SettingsSync_LockstepDetect();
    TEST_CHECK(*(int*)(factoryWest.data() + BLD_FACTORY_MAX_WORKERS_OFFSET) == 20);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(int*)(factoryWest.data() + BLD_FACTORY_MAX_WORKERS_OFFSET) == 12);
    TEST_CHECK(g_m2FactoryQueueMockCalls == 1);
    TEST_CHECK(g_m2FactoryQueueLastBld == factoryWest.data());
    TEST_CHECK(*(int*)(queueBuf.data() + 0 * FACTORY_QUEUE_ENTRY_STRIDE + 0x0C) == 10);
    TEST_CHECK(*(int*)(queueBuf.data() + 1 * FACTORY_QUEUE_ENTRY_STRIDE + 0x0C) == 12);

    // Clamp to >= 1 when maxWorkers == 1 and blueprint == 0
    SettingsSync_LockstepBaseline();
    *(int*)tmplVehA.data() = 0;
    *(int*)(factoryWest.data() + BLD_FACTORY_MAX_WORKERS_OFFSET) = 1;
    SettingsSync_LockstepDetect();
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_m2FactoryQueueMockCalls == 2);
    TEST_CHECK(*(int*)(queueBuf.data() + 0 * FACTORY_QUEUE_ENTRY_STRIDE + 0x0C) == 1);
    TEST_CHECK(*(int*)(queueBuf.data() + 1 * FACTORY_QUEUE_ENTRY_STRIDE + 0x0C) == 1);

    // 2. Empty queue (bld+0xFF0 == bld+0xFF8) must NOT invoke 0x1AC720
    SettingsSync_LockstepBaseline();
    *(int*)(nonFactoryBld.data() + BLD_FACTORY_MAX_WORKERS_OFFSET) = 8;
    SettingsSync_LockstepDetect();
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(int*)(nonFactoryBld.data() + BLD_FACTORY_MAX_WORKERS_OFFSET) == 8);
    TEST_CHECK(g_m2FactoryQueueMockCalls == 2);

    // 3. CALL_PRESENTATION sites (0x7522CE / 0x75281B, "xB"): suppressed in MP, direct in SP
    const unsigned long long siteArgs[7] = { 0, (unsigned long long)(uintptr_t)factoryWest.data(), 0, 0, 0, 0, 0 };
    unsigned long long ret = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_FACTORY_QUEUE_LIMIT_DEC_RVA, siteArgs, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_FACTORY_QUEUE_LIMIT_INC_RVA, siteArgs, &ret));
    TEST_CHECK(g_m2FactoryQueueMockCalls == 2);

    SettingsSync_LockstepReset();
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();

    TEST_CHECK(GameCalls_RunSiteForTest(SITE_FACTORY_QUEUE_LIMIT_DEC_RVA, siteArgs, &ret));
    TEST_CHECK(g_m2FactoryQueueMockCalls == 3);
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_FACTORY_QUEUE_LIMIT_INC_RVA, siteArgs, &ret));
    TEST_CHECK(g_m2FactoryQueueMockCalls == 4);
    (void)world;

    return true;
}

bool Test_R3Bugs_PersonalCarAutoBuyFactoryQueue()
{
    printf("    [Bug #5: Recall Personal Cars (0x7A1C28 -> 0x831DF0, 0x7A1C4B -> 0x3FBD00)] ...\n"); fflush(stdout);
    if (!SubTest_M2_Bug5_RecallPersonalCars()) return false;

    printf("    [Bug #6: Auto-Buy/Export Sliders (0x711ACB/0x711B60/0x711ADD/0x711C72, +7 Skip, KIND_AUTO_BUY)] ...\n"); fflush(stdout);
    if (!SubTest_M2_Bug6_AutoBuyExportSliders()) return false;

    printf("    [Bug #7: Factory Queue Worker Limit (0x7522CE/0x75281B, FOLLOW_FACTORY_QUEUE -> 0x1AC720)] ...\n"); fflush(stdout);
    if (!SubTest_M2_Bug7_FactoryQueueWorkerLimit()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 53: Milestone M3 — R1 Second-Pass Audit Fixes (BioWaste / SetForBio / 0x30D120)
// ---------------------------------------------------------------------------

static int g_m3WasteSplitCalls = 0;
static BYTE* g_m3WasteSplitLastBld = nullptr;
static BYTE* g_m3WasteSplitLastSlot = nullptr;
static unsigned char __fastcall MockWasteContainerSplit(BYTE* /*unused*/, BYTE* bld, BYTE* srcSlot)
{
    ++g_m3WasteSplitCalls;
    g_m3WasteSplitLastBld = bld;
    g_m3WasteSplitLastSlot = srcSlot;
    return 1;
}

static int g_m3WasteMergeCalls = 0;
static BYTE* g_m3WasteMergeLastWorld = nullptr;
static BYTE* g_m3WasteMergeLastBld = nullptr;
static BYTE* g_m3WasteMergeLastSlot = nullptr;
static unsigned char __fastcall MockWasteContainerMerge(BYTE* world, BYTE* bld, BYTE* dstSlot)
{
    ++g_m3WasteMergeCalls;
    g_m3WasteMergeLastWorld = world;
    g_m3WasteMergeLastBld = bld;
    g_m3WasteMergeLastSlot = dstSlot;
    return 1;
}

static int g_m3SetForBioCalls = 0;
static BYTE* g_m3SetForBioLastSrc = nullptr;
static BYTE* g_m3SetForBioLastDst = nullptr;
static float g_m3SetForBioLastMaxReq = 0.0f;
static float g_m3SetForBioCap = 100.0f;
static float __fastcall MockSetForBio(BYTE* /*unused*/, BYTE* srcSlot, BYTE* dstSlot, float maxReq)
{
    ++g_m3SetForBioCalls;
    g_m3SetForBioLastSrc = srcSlot;
    g_m3SetForBioLastDst = dstSlot;
    g_m3SetForBioLastMaxReq = maxReq;
    if (!isfinite(maxReq) || maxReq <= 0.0f) return 0.0f;
    return (maxReq < g_m3SetForBioCap) ? maxReq : g_m3SetForBioCap;
}

static int g_m3CrossroadAddToolCalls = 0;
static unsigned long long __fastcall MockCrossroadAddTool(unsigned long long /*world*/)
{
    ++g_m3CrossroadAddToolCalls;
    return 1;
}

static void __fastcall MockHubRemoveNode(unsigned long long node, unsigned long long vecAddr)
{
    unsigned long long beg = *(unsigned long long*)vecAddr;
    unsigned long long end = *(unsigned long long*)(vecAddr + 8);
    if (!beg || end <= beg) return;
    unsigned long long writePtr = beg;
    for (unsigned long long p = beg; p < end; p += 8) {
        if (*(unsigned long long*)p != node) {
            *(unsigned long long*)writePtr = *(unsigned long long*)p;
            writePtr += 8;
        }
    }
    *(unsigned long long*)(vecAddr + 8) = writePtr;
}

static unsigned char __fastcall MockHubUpdateArms(unsigned long long /*hub*/, unsigned long long /*zero*/)
{
    return 1;
}

static unsigned char __fastcall MockHubCanJoin(unsigned long long /*hub*/, unsigned long long /*node*/)
{
    return 1;
}

static float __fastcall MockHubDistance(unsigned long long /*hub*/, unsigned long long /*node*/)
{
    return 12.0f;
}

static void __fastcall MockHubAddNode(unsigned long long hub, unsigned long long node, unsigned char setOwner)
{
    const unsigned long long vecAddr = hub + 0x88;
    unsigned long long beg = *(unsigned long long*)vecAddr;
    unsigned long long end = *(unsigned long long*)(vecAddr + 8);
    const unsigned long long cap = *(unsigned long long*)(vecAddr + 16);
    for (unsigned long long p = beg; p && p < end; p += 8) {
        if (*(unsigned long long*)p == node) {
            if (setOwner != 0) *(unsigned long long*)(node + 0xC0) = hub;
            return;
        }
    }
    if (beg && end + 8 <= cap) {
        *(unsigned long long*)end = node;
        *(unsigned long long*)(vecAddr + 8) = end + 8;
    }
    if (setOwner != 0) *(unsigned long long*)(node + 0xC0) = hub;
}

static void SeedRouteToolsRoutinesForTest()
{
    static const TsmRoutineSig kRouteSigs[] = {
        { 0x2B50A0, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89 }, 0 },
        { 0x2B26E0, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x99, 0x98, 0xd2, 0x00, 0x00, 0x48 }, 0 },
        { 0x2B49D0, { 0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0xb9, 0x98, 0xd2, 0x00, 0x00, 0x48, 0x8b }, 0 },
        { 0x2B4BE0, { 0x40, 0x55, 0x56, 0x41, 0x54, 0x48, 0x8d, 0x6c, 0x24, 0xd0, 0x48, 0x81, 0xec, 0x30, 0x01, 0x00 }, 0 },
        { 0x40CD20, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b }, 0 },
        { 0x498920, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83 }, 0 },
        { 0x1AA560, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x9a, 0xf8, 0x04, 0x00 }, 0 },
        { 0x3BB270, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x50, 0x83, 0xb9, 0xbc, 0x05, 0x00, 0x00 }, 0 },
        { 0x497AA0, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xf0 }, 0 },
        { 0x2B2D50, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xf8 }, 0 },
        { 0x2B2D60, { 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 10 },
        { 0x40BCA0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b }, 0 },
        { 0x68B350, { 0x48, 0x8b, 0x81, 0x08, 0x17, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x29, 0xf3, 0x0f, 0x10, 0x05 }, 0 },
        { 0x3E2840, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x44, 0x88, 0x4c, 0x24, 0x20, 0x56 }, 0 },
        { 0x3E3E80, { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec }, 0 },
        { 0x3A3830, { 0x48, 0x8b, 0x82, 0x48, 0x0a, 0x00, 0x00, 0x45, 0x33, 0xc0, 0x48, 0x2b, 0x82, 0x40, 0x0a, 0x00 }, 0 },
        { 0x3A3770, { 0x45, 0x33, 0xd2, 0x0f, 0x28, 0xd9, 0x45, 0x85, 0xc9, 0x79, 0x56, 0x49, 0x8b, 0x80, 0x48, 0x0a }, 0 },
        { 0x6DE400, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x83, 0xb9, 0x04, 0x1a, 0x00, 0x00, 0x03, 0x48, 0x8b, 0xf9 }, 0 },
        { 0x1AE130, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xd9, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48 }, 0 },
        { 0x6B7210, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0xe0, 0x01, 0x00, 0x00, 0x48 }, 0 },
        { 0x6B75C0, { 0x40, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x5c, 0x24, 0x40, 0x4d, 0x8b, 0xf8 }, 0 },
    };
    for (const auto& s : kRouteSigs) {
        const size_t len = s.size ? s.size : 16u;
        memcpy(g_mockExeBuffer + s.rva, s.entry, len);
    }
    RouteTools_ResetReadyForTest();
}

static bool SubTest_M3_R1_1A_WasteContainerSplitMerge()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(SITE_WASTE_CONTAINER_SPLIT_RVA == 0x78EB6Au);
    TEST_CHECK(FN_WASTE_CONTAINER_SPLIT_RVA == 0x19D8D0u);
    TEST_CHECK(SITE_WASTE_CONTAINER_MERGE_RVA == 0x78ED9Bu);
    TEST_CHECK(FN_WASTE_CONTAINER_MERGE_RVA == 0x19D3D0u);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_WASTE_CONTAINER_SPLIT_RVA, FN_WASTE_CONTAINER_SPLIT_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_WASTE_CONTAINER_MERGE_RVA, FN_WASTE_CONTAINER_MERGE_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_WASTE_CONTAINER_SPLIT_RVA), "xBI") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_WASTE_CONTAINER_MERGE_RVA), "WBI") == 0);

    g_m3WasteSplitCalls = 0;
    g_m3WasteSplitLastBld = nullptr;
    g_m3WasteSplitLastSlot = nullptr;
    g_m3WasteMergeCalls = 0;
    g_m3WasteMergeLastWorld = nullptr;
    g_m3WasteMergeLastBld = nullptr;
    g_m3WasteMergeLastSlot = nullptr;
    WriteAbsJmp12(g_mockExeBuffer + FN_WASTE_CONTAINER_SPLIT_RVA, (void*)&MockWasteContainerSplit);
    WriteAbsJmp12(g_mockExeBuffer + FN_WASTE_CONTAINER_MERGE_RVA, (void*)&MockWasteContainerMerge);

    std::vector<BYTE> bldWest(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> bldEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)bldWest.data() = 601;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -300.0f;
    *(int*)bldEast.data() = 602;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = 300.0f;

    std::vector<BYTE> westSlots(3 * BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> eastSlots(2 * BLD_STORAGE_SLOT_SIZE, 0);
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0x00) = westSlots.data();
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0x08) = westSlots.data() + westSlots.size();
    *(BYTE**)(bldEast.data() + BLD_STORAGE_OFFSET + 0x00) = eastSlots.data();
    *(BYTE**)(bldEast.data() + BLD_STORAGE_OFFSET + 0x08) = eastSlots.data() + eastSlots.size();

    std::vector<BYTE*> blds = { bldWest.data(), bldEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    BYTE* westSlot1 = westSlots.data() + 1 * BLD_STORAGE_SLOT_SIZE;
    BYTE* westSlot2 = westSlots.data() + 2 * BLD_STORAGE_SLOT_SIZE;
    BYTE* eastSlot0 = eastSlots.data();

    // Capture converts slotPtr -> slotIndex, rebuild restores slotIndex
    const unsigned long long splitIn[7] = { 0, (unsigned long long)(uintptr_t)bldWest.data(), (unsigned long long)(uintptr_t)westSlot1, 0, 0, 0, 0 };
    unsigned long long splitOut[7] = {};
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_WASTE_CONTAINER_SPLIT_RVA, splitIn, splitOut));
    TEST_CHECK(splitOut[1] == (unsigned long long)(uintptr_t)bldWest.data());
    TEST_CHECK(splitOut[2] == 1ULL);

    // Reject foreign slot pointer (eastSlot0 not owned by bldWest)
    const unsigned long long badSlotIn[7] = { 0, (unsigned long long)(uintptr_t)bldWest.data(), (unsigned long long)(uintptr_t)eastSlot0, 0, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_WASTE_CONTAINER_SPLIT_RVA, badSlotIn, splitOut));

    // TwoStates blocks East building split/merge
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_WASTE_CONTAINER_SPLIT_RVA, splitIn));
    const unsigned long long eastMergeIn[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldEast.data(), (unsigned long long)(uintptr_t)eastSlot0, 0, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_WASTE_CONTAINER_MERGE_RVA, eastMergeIn));

    // MP capture (COMMANDS_HOST): queues split + merge without immediate execution
    Commands_SetSession(COMMANDS_HOST);
    const unsigned long long mergeIn[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldWest.data(), (unsigned long long)(uintptr_t)westSlot2, 0, 0, 0, 0 };
    unsigned long long ret = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_WASTE_CONTAINER_SPLIT_RVA, splitIn, &ret));
    TEST_CHECK(ret == 0);
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_WASTE_CONTAINER_MERGE_RVA, mergeIn, &ret));
    TEST_CHECK(ret == 0);
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_WASTE_CONTAINER_MERGE_RVA, eastMergeIn, &ret));
    TEST_CHECK(g_m3WasteSplitCalls == 0);
    TEST_CHECK(g_m3WasteMergeCalls == 0);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_m3WasteSplitCalls == 1);
    TEST_CHECK(g_m3WasteSplitLastBld == bldWest.data());
    TEST_CHECK(g_m3WasteSplitLastSlot == westSlot1);
    TEST_CHECK(g_m3WasteMergeCalls == 1);
    TEST_CHECK(g_m3WasteMergeLastWorld == world);
    TEST_CHECK(g_m3WasteMergeLastBld == bldWest.data());
    TEST_CHECK(g_m3WasteMergeLastSlot == westSlot2);

    // Waiting Lobby blocks capture
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_WASTE_CONTAINER_SPLIT_RVA, splitIn, &ret));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    TEST_CHECK(g_m3WasteSplitCalls == 1);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");

    // Singleplayer isolation (!Commands_TakeUi()): passes straight through
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_WASTE_CONTAINER_SPLIT_RVA, splitIn, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_WASTE_CONTAINER_MERGE_RVA, mergeIn, &ret));
    TEST_CHECK(g_m3WasteSplitCalls == 2);
    TEST_CHECK(g_m3WasteMergeCalls == 2);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    return true;
}

static bool SubTest_M3_R1_1B_SetForBioAndExclusion()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_OFF);

    TEST_CHECK(SITE_SET_FOR_BIO_WASTE_WIN_RVA == 0x708D78u);
    TEST_CHECK(SITE_SET_FOR_BIO_RES_WIN_RVA == 0x6F8480u);
    TEST_CHECK(FN_SET_FOR_BIO_RVA == 0x18A8B0u);
    TEST_CHECK(BLD_BIO_TRANSFER_LOG0_OFFSET == 0x1308u);
    TEST_CHECK(BLD_BIO_TRANSFER_LOG1_OFFSET == 0x130Cu);
    TEST_CHECK(FLT_BIO_SCALE_130C_RVA == 0x90AA88u);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_SET_FOR_BIO_WASTE_WIN_RVA, FN_SET_FOR_BIO_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, FN_SET_FOR_BIO_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_SET_FOR_BIO_WASTE_WIN_RVA), "BIIF") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_SET_FOR_BIO_RES_WIN_RVA), "BIIF") == 0);
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(BLD_BIO_TRANSFER_LOG0_OFFSET));
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(BLD_BIO_TRANSFER_LOG1_OFFSET));

    *(float*)(g_mockExeBuffer + FLT_BIO_SCALE_130C_RVA) = 60.0f;
    g_m3SetForBioCalls = 0;
    g_m3SetForBioLastSrc = nullptr;
    g_m3SetForBioLastDst = nullptr;
    g_m3SetForBioLastMaxReq = 0.0f;
    g_m3SetForBioCap = 100.0f;
    WriteAbsJmp12(g_mockExeBuffer + FN_SET_FOR_BIO_RVA, (void*)&MockSetForBio);

    std::vector<BYTE> bldWest(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> bldEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)bldWest.data() = 611;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -300.0f;
    *(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG0_OFFSET) = 10.0f;
    *(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG1_OFFSET) = 600.0f;

    *(int*)bldEast.data() = 612;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = 300.0f;

    std::vector<BYTE> westSlots(3 * BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> eastSlots(2 * BLD_STORAGE_SLOT_SIZE, 0);
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0x00) = westSlots.data();
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0x08) = westSlots.data() + westSlots.size();
    *(BYTE**)(bldEast.data() + BLD_STORAGE_OFFSET + 0x00) = eastSlots.data();
    *(BYTE**)(bldEast.data() + BLD_STORAGE_OFFSET + 0x08) = eastSlots.data() + eastSlots.size();
    std::vector<BYTE*> blds = { bldWest.data(), bldEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    BYTE* slot0 = westSlots.data() + 0 * BLD_STORAGE_SLOT_SIZE;
    BYTE* slot1 = westSlots.data() + 1 * BLD_STORAGE_SLOT_SIZE;
    BYTE* slot2 = westSlots.data() + 2 * BLD_STORAGE_SLOT_SIZE;
    BYTE* eastSlot0 = eastSlots.data();

    auto floatBits = [](float f) -> unsigned long long {
        unsigned u = 0;
        memcpy(&u, &f, 4);
        return (unsigned long long)u;
    };

    // Boundary validation checks
    unsigned long long outArgs[7] = {};
    const unsigned long long zeroReq[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)slot2, floatBits(0.0f), 0, 0, 0 };
    const unsigned long long negReq[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)slot2, floatBits(-5.0f), 0, 0, 0 };
    const unsigned long long hugeReq[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)slot2, floatBits(1.0e7f), 0, 0, 0 };
    const unsigned long long nanReq[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)slot2, 0x7FC00000ULL, 0, 0, 0 };
    const unsigned long long sameSlot[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)slot0, floatBits(5.0f), 0, 0, 0 };
    const unsigned long long crossBld[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)eastSlot0, floatBits(5.0f), 0, 0, 0 };
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, zeroReq, outArgs));
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, negReq, outArgs));
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, hugeReq, outArgs));
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, nanReq, outArgs));
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, sameSlot, outArgs));
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, crossBld, outArgs));

    // Valid capture & rebuild resolves (bldWest, 0, 2, 12.5f)
    const unsigned long long resWinArgs[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)slot2, floatBits(12.5f), 0, 0, 0 };
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, resWinArgs, outArgs));
    TEST_CHECK(outArgs[0] == (unsigned long long)(uintptr_t)bldWest.data());
    TEST_CHECK(outArgs[1] == 0ULL && outArgs[2] == 2ULL && outArgs[3] == floatBits(12.5f));

    // TwoStates blocks East building
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, resWinArgs));
    const unsigned long long eastWinArgs[7] = { 0, (unsigned long long)(uintptr_t)eastSlots.data(), (unsigned long long)(uintptr_t)(eastSlots.data() + BLD_STORAGE_SLOT_SIZE), floatBits(5.0f), 0, 0, 0 };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, eastWinArgs));

    // MP capture (0x6F8480): returns 0.0f bits locally, updates +0x1308 / +0x130C on boundary replay
    Commands_SetSession(COMMANDS_HOST);
    unsigned long long retBits = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, resWinArgs, &retBits));
    TEST_CHECK(retBits == 0ULL);
    TEST_CHECK(g_m3SetForBioCalls == 0);
    TEST_CHECK_NEAR(*(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG0_OFFSET), 10.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG1_OFFSET), 600.0f, 1e-4f);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_m3SetForBioCalls == 1);
    TEST_CHECK(g_m3SetForBioLastSrc == slot0);
    TEST_CHECK(g_m3SetForBioLastDst == slot2);
    TEST_CHECK_NEAR(g_m3SetForBioLastMaxReq, 12.5f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG0_OFFSET), 22.5f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG1_OFFSET), 1350.0f, 1e-4f);

    // MP capture (0x708D78): replays 0x18A8B0 WITHOUT modifying bld+0x1308 / +0x130C
    const unsigned long long wasteWinArgs[7] = { 0, (unsigned long long)(uintptr_t)slot1, (unsigned long long)(uintptr_t)slot2, floatBits(5.0f), 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SET_FOR_BIO_WASTE_WIN_RVA, wasteWinArgs, &retBits));
    TEST_CHECK(retBits == 0ULL);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_m3SetForBioCalls == 2);
    TEST_CHECK(g_m3SetForBioLastSrc == slot1);
    TEST_CHECK(g_m3SetForBioLastDst == slot2);
    TEST_CHECK_NEAR(*(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG0_OFFSET), 22.5f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG1_OFFSET), 1350.0f, 1e-4f);

    // Waiting Lobby: returns 0.0f bits and does not queue
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    retBits = 99;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, resWinArgs, &retBits));
    TEST_CHECK(retBits == 0ULL);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");

    // Singleplayer isolation (!Commands_TakeUi()): passes straight through and returns exact xmm0 float bits
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    const unsigned long long spArgs[7] = { 0, (unsigned long long)(uintptr_t)slot0, (unsigned long long)(uintptr_t)slot1, floatBits(7.25f), 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_SET_FOR_BIO_RES_WIN_RVA, spArgs, &retBits));
    TEST_CHECK(g_m3SetForBioCalls == 3);
    TEST_CHECK(retBits == floatBits(7.25f));
    TEST_CHECK_NEAR(*(float*)(bldWest.data() + BLD_BIO_TRANSFER_LOG0_OFFSET), 22.5f, 1e-4f);

    return true;
}

static bool SubTest_M3_R1_2A_CrossroadAddCopyAndMultiHubGuard()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    SeedRouteToolsRoutinesForTest();
    TEST_CHECK(RouteTools_Ready());

    *(float*)(g_mockExeBuffer + 0x90AA00) = 1000.0f;
    g_m3CrossroadAddToolCalls = 0;
    WriteAbsJmp12(g_mockExeBuffer + 0x2B2330, (void*)&MockCrossroadAddTool);
    WriteAbsJmp12(g_mockExeBuffer + 0x0F98D0, (void*)&MockHubRemoveNode);
    WriteAbsJmp12(g_mockExeBuffer + 0x222AD0, (void*)&MockHubUpdateArms);
    WriteAbsJmp12(g_mockExeBuffer + 0x223320, (void*)&MockHubAddNode);
    WriteAbsJmp12(g_mockExeBuffer + 0x225150, (void*)&MockHubCanJoin);
    WriteAbsJmp12(g_mockExeBuffer + 0x2254B0, (void*)&MockHubDistance);

    std::vector<BYTE> net(0x300, 0);
    std::vector<BYTE> firstNode(0x100, 0), stopNode(0x100, 0), otherNode(0x100, 0);
    *(float*)(firstNode.data() + 0x04) = 100.0f;
    *(float*)(firstNode.data() + 0x0C) = 200.0f;
    *(float*)(stopNode.data() + 0x04) = 110.0f;
    *(float*)(stopNode.data() + 0x0C) = 205.0f;
    *(float*)(otherNode.data() + 0x04) = 120.0f;
    *(float*)(otherNode.data() + 0x0C) = 210.0f;

    std::vector<BYTE*> netNodes = { firstNode.data(), stopNode.data(), otherNode.data() };
    WriteMockVectorHeader(net.data() + 0x298, netNodes);
    *(BYTE**)(world + 0xF7A0) = net.data();
    *(int*)(world + 0x14034) = 1;
    *(BYTE**)(world + 0x14038) = net.data();

    std::vector<BYTE> liveHub(0x148, 0), winCopy(0x148, 0), otherHub(0x148, 0), winObj(0x300, 0);
    std::vector<BYTE*> liveNodes(8, nullptr);
    liveNodes[0] = firstNode.data();
    *(BYTE**)(liveHub.data() + 0x88) = (BYTE*)liveNodes.data();
    *(BYTE**)(liveHub.data() + 0x90) = (BYTE*)(liveNodes.data() + 1);
    *(BYTE**)(liveHub.data() + 0x98) = (BYTE*)(liveNodes.data() + liveNodes.size());

    std::vector<BYTE*> copyNodes(8, nullptr);
    copyNodes[0] = firstNode.data();
    *(BYTE**)(winCopy.data() + 0x88) = (BYTE*)copyNodes.data();
    *(BYTE**)(winCopy.data() + 0x90) = (BYTE*)(copyNodes.data() + 1);
    *(BYTE**)(winCopy.data() + 0x98) = (BYTE*)(copyNodes.data() + copyNodes.size());

    std::vector<BYTE*> otherNodes = { stopNode.data(), otherNode.data() };
    WriteMockVectorHeader(otherHub.data() + 0x88, otherNodes);

    *(BYTE**)(firstNode.data() + 0xC0) = liveHub.data();
    *(BYTE**)(stopNode.data() + 0xC0) = otherHub.data();
    *(BYTE**)(otherNode.data() + 0xC0) = otherHub.data();

    *(BYTE**)(winObj.data() + 0x240) = liveHub.data();
    *(BYTE**)(winObj.data() + 0x270) = winCopy.data();
    *(int*)(winObj.data() + 0x264) = 1;

    *(BYTE**)(world + 0xD298) = winCopy.data();
    *(BYTE**)(world + 0xD2A0) = winObj.data();
    *(BYTE**)(world + 0xF650) = stopNode.data();

    Commands_SetSession(COMMANDS_HOST);
    BYTE* press = g_mockExeBuffer + 0xA54E91;
    *press = 1;

    // 1. 0xFA9 multi-hub conflict guard: refuse both on tool capture and direct replay when otherHub has > 1 node
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // Verify direct replay also refuses 0xFA9 multi-hub conflict
    *(BYTE**)(stopNode.data() + 0xC0) = nullptr;
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    *(BYTE**)(stopNode.data() + 0xC0) = otherHub.data();
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK((*(BYTE**)(liveHub.data() + 0x90) - *(BYTE**)(liveHub.data() + 0x88)) == 8);
    TEST_CHECK((*(BYTE**)(winCopy.data() + 0x90) - *(BYTE**)(winCopy.data() + 0x88)) == 8);

    // 2. 1-node otherHub IS allowed: updates BOTH liveHub and winCopy, cleans old 1-node hub, restores stopNode->0xC0 = liveHub
    *(BYTE**)(otherHub.data() + 0x90) = (BYTE*)(otherNodes.data() + 1);
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());

    TEST_CHECK((*(BYTE**)(liveHub.data() + 0x90) - *(BYTE**)(liveHub.data() + 0x88)) == 16);
    TEST_CHECK(((BYTE**)(*(BYTE**)(liveHub.data() + 0x88)))[1] == stopNode.data());
    TEST_CHECK((*(BYTE**)(winCopy.data() + 0x90) - *(BYTE**)(winCopy.data() + 0x88)) == 16);
    TEST_CHECK(((BYTE**)(*(BYTE**)(winCopy.data() + 0x88)))[1] == stopNode.data());
    TEST_CHECK(*(BYTE**)(otherHub.data() + 0x90) == *(BYTE**)(otherHub.data() + 0x88));
    TEST_CHECK(*(BYTE**)(stopNode.data() + 0xC0) == liveHub.data());
    TEST_CHECK(*(int*)(winObj.data() + 0x264) == 0);

    // JunctionAccept_Capture sees both nodes in winCopy (does not revert crossroad_add)
    JunctionAccept_Capture((unsigned long long)(uintptr_t)liveHub.data(), (unsigned long long)(uintptr_t)winCopy.data());
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_SetSession(COMMANDS_OFF);
    Commands_SetSession(COMMANDS_HOST);

    // 3. Toggle again (inHub == true): removes stopNode from BOTH liveHub and winCopy, clears stopNode->0xC0
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());

    TEST_CHECK((*(BYTE**)(liveHub.data() + 0x90) - *(BYTE**)(liveHub.data() + 0x88)) == 8);
    TEST_CHECK((*(BYTE**)(winCopy.data() + 0x90) - *(BYTE**)(winCopy.data() + 0x88)) == 8);
    TEST_CHECK(*(BYTE**)(stopNode.data() + 0xC0) == nullptr);

    // 3b. Unrelated window at world+0xD2A0 MUST NOT be corrupted on add or remove when m.origin == g_origin
    std::vector<BYTE> diffHub(0x148, 0), diffCopy(0x148, 0), diffWin(0x300, 0);
    std::vector<BYTE*> diffNodes(8, nullptr);
    diffNodes[0] = otherNode.data();
    *(BYTE**)(diffHub.data() + 0x88) = (BYTE*)diffNodes.data();
    *(BYTE**)(diffHub.data() + 0x90) = (BYTE*)(diffNodes.data() + 1);
    *(BYTE**)(diffHub.data() + 0x98) = (BYTE*)(diffNodes.data() + diffNodes.size());
    std::vector<BYTE*> diffCopyNodes(8, nullptr);
    diffCopyNodes[0] = otherNode.data();
    *(BYTE**)(diffCopy.data() + 0x88) = (BYTE*)diffCopyNodes.data();
    *(BYTE**)(diffCopy.data() + 0x90) = (BYTE*)(diffCopyNodes.data() + 1);
    *(BYTE**)(diffCopy.data() + 0x98) = (BYTE*)(diffCopyNodes.data() + diffCopyNodes.size());
    *(BYTE**)(diffWin.data() + 0x240) = diffHub.data();
    *(BYTE**)(diffWin.data() + 0x270) = diffCopy.data();

    // Add with unrelated window open before boundary
    *(int*)(diffWin.data() + 0x264) = 42;
    *(BYTE**)(world + 0xD298) = winCopy.data();
    *(BYTE**)(world + 0xD2A0) = winObj.data();
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    *(BYTE**)(world + 0xD298) = diffCopy.data();
    *(BYTE**)(world + 0xD2A0) = diffWin.data();
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK((*(BYTE**)(liveHub.data() + 0x90) - *(BYTE**)(liveHub.data() + 0x88)) == 16);
    TEST_CHECK(*(BYTE**)(stopNode.data() + 0xC0) == liveHub.data());
    TEST_CHECK(*(int*)(diffWin.data() + 0x264) == 42);
    TEST_CHECK((*(BYTE**)(diffCopy.data() + 0x90) - *(BYTE**)(diffCopy.data() + 0x88)) == 8);
    TEST_CHECK(((BYTE**)(*(BYTE**)(diffCopy.data() + 0x88)))[0] == otherNode.data());

    // Remove with unrelated window open before boundary
    *(int*)(diffWin.data() + 0x264) = 9;
    *(BYTE**)(world + 0xD298) = liveHub.data();
    *(BYTE**)(world + 0xD2A0) = winObj.data();
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    *(BYTE**)(world + 0xD298) = diffCopy.data();
    *(BYTE**)(world + 0xD2A0) = diffWin.data();
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK((*(BYTE**)(liveHub.data() + 0x90) - *(BYTE**)(liveHub.data() + 0x88)) == 8);
    TEST_CHECK(*(BYTE**)(stopNode.data() + 0xC0) == nullptr);
    TEST_CHECK(*(int*)(diffWin.data() + 0x264) == 9);
    TEST_CHECK((*(BYTE**)(diffCopy.data() + 0x90) - *(BYTE**)(diffCopy.data() + 0x88)) == 8);
    TEST_CHECK(((BYTE**)(*(BYTE**)(diffCopy.data() + 0x88)))[0] == otherNode.data());

    // Mismatched TARGET_OFFSET == liveHub with unrelated WINDOW_OFFSET == diffWin also preserves diffWin+0x264,
    // while matching direct-hub window (TARGET_OFFSET == liveHub && directWin+0x240 == liveHub) updates +0x264.
    *(int*)(diffWin.data() + 0x264) = 77;
    *(BYTE**)(world + 0xD298) = liveHub.data();
    *(BYTE**)(world + 0xD2A0) = diffWin.data();
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(int*)(diffWin.data() + 0x264) == 77);

    std::vector<BYTE> directWin(0x300, 0);
    *(BYTE**)(directWin.data() + 0x240) = liveHub.data();
    *(int*)(directWin.data() + 0x264) = 1;
    *(BYTE**)(world + 0xD298) = liveHub.data();
    *(BYTE**)(world + 0xD2A0) = directWin.data();
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(int*)(directWin.data() + 0x264) == 0);
    TEST_CHECK(*(BYTE**)(stopNode.data() + 0xC0) == nullptr);

    *(BYTE**)(world + 0xD298) = winCopy.data();
    *(BYTE**)(world + 0xD2A0) = winObj.data();

    // 4. Singleplayer isolation (!Commands_TakeLocal()): calls 0x2B2330 directly without submitting command
    Commands_SetSession(COMMANDS_OFF);
    TEST_CHECK(g_m3CrossroadAddToolCalls == 8);
    *press = 1;
    RouteTools_JoinHubStopTool((unsigned long long)(uintptr_t)world);
    TEST_CHECK(g_m3CrossroadAddToolCalls == 9);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    return true;
}

static bool SubTest_M3_R1_2B_EditorModeOffsetAndBrushes()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    TEST_CHECK(WORLD_MAP_EDITOR_MODE_OFFSET == 0x10398u);

    // 1. Commands_SanitizeCheatState clears world[0x10398] and is idempotent
    world[WORLD_MAP_EDITOR_MODE_OFFSET] = 1;
    TEST_CHECK(Commands_SanitizeCheatState(world));
    TEST_CHECK(world[WORLD_MAP_EDITOR_MODE_OFFSET] == 0);
    TEST_CHECK(!Commands_SanitizeCheatState(world));

    // 2. Editor brush classifier & live-tool exclusions
    const char* kBlockedBrushes[] = {
        "paint_coal", "paint_trees", "erase_coal", "erase_trees",
        "terrain_raise_editor", "terrain_smooth_editor", "terrain_fromcenter_editor",
        "building_near_road", "random_selected_building"
    };
    for (const char* toolName : kBlockedBrushes) {
        TEST_CHECK(Commands_IsEditorBrushToolBlockedForTest(toolName));
    }
    const char* kAllowedTools[] = {
        "erase_airroad", "erase_cableway", "terrain_raise",
        "terrain_restore_topsoil", "terrain_go_there", "road_asphalt", "building_distribution"
    };
    for (const char* toolName : kAllowedTools) {
        TEST_CHECK(!Commands_IsEditorBrushToolBlockedForTest(toolName));
    }

    // 3. PutAwayUncarriedTool under COMMANDS_HOST puts away editor brushes but preserves erase_airroad / erase_cableway / join_hub_stop
    SeedRouteToolsRoutinesForTest();
    Commands_SetSession(COMMANDS_HOST);
    for (const char* toolName : kBlockedBrushes) {
        *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = toolName;
        Commands_PutAwayUncarriedToolForTest();
        TEST_CHECK(*(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) == nullptr);
    }
    const char* kPreservedInMp[] = { "erase_airroad", "erase_cableway", "join_hub_stop" };
    for (const char* toolName : kPreservedInMp) {
        *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = toolName;
        Commands_PutAwayUncarriedToolForTest();
        TEST_CHECK(*(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) == toolName);
    }

    // 4. Singleplayer isolation (COMMANDS_OFF): world+0x10398 and selected editor brush remain untouched by Commands_Collect / PutAwayUncarriedTool
    Commands_SetSession(COMMANDS_OFF);
    world[WORLD_MAP_EDITOR_MODE_OFFSET] = 1;
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = "paint_coal";
    Commands_Collect();
    Commands_PutAwayUncarriedToolForTest();
    TEST_CHECK(world[WORLD_MAP_EDITOR_MODE_OFFSET] == 1);
    TEST_CHECK(strcmp(*(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF), "paint_coal") == 0);

    return true;
}

bool Test_R1SecondPass_WasteSetForBioCrossroadEditor()
{
    printf("    [R1-1A: Waste Container Split/Merge (0x78EB6A -> 0x19D8D0, 0x78ED9B -> 0x19D3D0)] ...\n"); fflush(stdout);
    if (!SubTest_M3_R1_1A_WasteContainerSplitMerge()) return false;

    printf("    [R1-1B: SetForBio (0x708D78 / 0x6F8480 -> 0x18A8B0, bld+0x1308/+0x130C, kViewWords)] ...\n"); fflush(stdout);
    if (!SubTest_M3_R1_1B_SetForBioAndExclusion()) return false;

    printf("    [R1-2A: crossroad_add (0xFA9 Multi-Hub Guard, world+0xD298 Copy Sync, +0xC0)] ...\n"); fflush(stdout);
    if (!SubTest_M3_R1_2A_CrossroadAddCopyAndMultiHubGuard()) return false;

    printf("    [R1-2B: Map Editor Mode (world+0x10398) & Editor Brushes (0x30D120)] ...\n"); fflush(stdout);
    if (!SubTest_M3_R1_2B_EditorModeOffsetAndBrushes()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 54: M1 Hook Fixes (9 Removed, FireDispatch, AssignCo) & Run/kSites Bug Regressions
// ---------------------------------------------------------------------------

static int g_m1FireDispatchCalls = 0;
static BYTE* g_m1FireLastObj = nullptr;
static BYTE* g_m1FireLastVeh = nullptr;
static void __fastcall MockFireDispatch(BYTE* /*unusedRcx*/, BYTE* fireObj, BYTE* veh)
{
    ++g_m1FireDispatchCalls;
    g_m1FireLastObj = fireObj;
    g_m1FireLastVeh = veh;
}

static int g_m1AssignStartCalls = 0;
static BYTE* g_m1AssignStartLastBld = nullptr;
static void __fastcall MockAssignStartPlanned(BYTE* /*world*/, BYTE* bld, unsigned char /*flag*/, unsigned char /*zero*/)
{
    ++g_m1AssignStartCalls;
    g_m1AssignStartLastBld = bld;
}

static int g_m1AssignOfficeCalls = 0;
static BYTE* g_m1AssignOfficeLastCo = nullptr;
static BYTE* g_m1AssignOfficeLastTarget = nullptr;
static void __fastcall MockAssignOffice(BYTE* /*world*/, BYTE* co, BYTE* target, float /*dist*/, unsigned char /*a5*/, unsigned char /*a6*/)
{
    ++g_m1AssignOfficeCalls;
    g_m1AssignOfficeLastCo = co;
    g_m1AssignOfficeLastTarget = target;
}

static int g_m1UnassignOfficeCalls = 0;
static BYTE* g_m1UnassignOfficeLastCo = nullptr;
static BYTE* g_m1UnassignOfficeLastTarget = nullptr;
static void __fastcall MockUnassignOffice(BYTE* /*world*/, BYTE* co, BYTE* target)
{
    ++g_m1UnassignOfficeCalls;
    g_m1UnassignOfficeLastCo = co;
    g_m1UnassignOfficeLastTarget = target;
}

static void __fastcall MockPlaySoundNoop(BYTE* /*world*/, const char* /*path*/, unsigned char /*flag*/)
{
}

static bool SubTest_M1_RemovedAndAddedCallSites()
{
    const unsigned kRemoved[9][2] = {
        { 0x31484Du, 0x5EED80u },
        { 0x3142D6u, 0x1D4820u },
        { 0x3142F6u, 0x1DB130u },
        { 0x314356u, 0x1D7140u },
        { 0x314893u, 0x5F7860u },
        { 0x3135F9u, 0x418210u },
        { 0x1A131Eu, 0x1A0510u },
        { 0x1A1343u, 0x1A0510u },
        { 0x1A15A8u, 0x1BC010u },
    };
    for (size_t i = 0; i < sizeof(kRemoved) / sizeof(kRemoved[0]); ++i) {
        TEST_CHECK(!GameCalls_HasCallSiteForTest(kRemoved[i][0], kRemoved[i][1]));
        TEST_CHECK(GameCalls_GetSiteSchemaForTest(kRemoved[i][0]) == nullptr);
    }

    // Kept factory site
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_FACTORY_REPLACE_MACHINES_2_RVA, RVA_START_PLANNED_CONSTRUCTION));

    struct ExpectedSite {
        unsigned site;
        unsigned target;
        const char* schema;
    };
    const ExpectedSite kAdded[12] = {
        { SITE_FIRESTATION_DISPATCH_ROAD_RVA,     FN_FIRE_DISPATCH_RVA,          "xBV" },
        { SITE_FIRESTATION_DISPATCH_HELI_RVA,     FN_FIRE_DISPATCH_RVA,          "xBV" },
        { SITE_ASSIGN_CO_SINGLE_START_RVA,        RVA_START_PLANNED_CONSTRUCTION,"WObx" },
        { SITE_ASSIGN_CO_SINGLE_ASSIGN_RVA,       RVA_ASSIGN_CONSTRUCTION_OFFICE,"WOOFbb" },
        { SITE_ASSIGN_CO_SINGLE_RAIL_START_RVA,   RVA_START_PLANNED_CONSTRUCTION,"WObx" },
        { SITE_ASSIGN_CO_SINGLE_RAIL_ASSIGN_RVA,  RVA_ASSIGN_CONSTRUCTION_OFFICE,"WOOFbb" },
        { SITE_ASSIGN_CO_RECT_START_RVA,          RVA_START_PLANNED_CONSTRUCTION,"WObx" },
        { SITE_ASSIGN_CO_RECT_ASSIGN_RVA,         RVA_ASSIGN_CONSTRUCTION_OFFICE,"WOOFbb" },
        { SITE_ASSIGN_CO_RECT_RAIL_START_RVA,     RVA_START_PLANNED_CONSTRUCTION,"WObx" },
        { SITE_ASSIGN_CO_RECT_RAIL_ASSIGN_RVA,    RVA_ASSIGN_CONSTRUCTION_OFFICE,"WOOFbb" },
        { SITE_ASSIGN_CO_SINGLE_UNASSIGN_RVA,     RVA_UNASSIGN_CONSTRUCTION_OFFICE,"WOO" },
        { SITE_ASSIGN_CO_RECT_UNASSIGN_RVA,       RVA_UNASSIGN_CONSTRUCTION_OFFICE,"WOO" },
    };
    for (size_t i = 0; i < sizeof(kAdded) / sizeof(kAdded[0]); ++i) {
        TEST_CHECK(GameCalls_HasCallSiteForTest(kAdded[i].site, kAdded[i].target));
        const char* schema = GameCalls_GetSiteSchemaForTest(kAdded[i].site);
        TEST_CHECK(schema != nullptr && strcmp(schema, kAdded[i].schema) == 0);
    }
    return true;
}

static bool SubTest_M1_FireDispatchBehaviorAndSkip()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    g_m1FireDispatchCalls = 0;
    g_m1FireLastObj = nullptr;
    g_m1FireLastVeh = nullptr;
    WriteAbsJmp12(g_mockExeBuffer + FN_FIRE_DISPATCH_RVA, (void*)&MockFireDispatch);

    std::vector<BYTE> fireObjWest(0x60, 0), fireObjEast(0x60, 0);
    std::vector<BYTE> bldWest(BUILDING_STRUCT_BYTES, 0), bldEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)bldWest.data() = 801;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -400.0f;
    *(BYTE**)(bldWest.data() + BLD_FIRE_OBJ_OFFSET) = fireObjWest.data();

    *(int*)bldEast.data() = 802;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = 400.0f;
    *(BYTE**)(bldEast.data() + BLD_FIRE_OBJ_OFFSET) = fireObjEast.data();

    std::vector<BYTE*> blds = { bldWest.data(), bldEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    std::vector<BYTE> vehRoadWest(0x1B00, 0), vehHeliWest(0x1B00, 0), vehHeliEast(0x1B00, 0);
    *(int*)(vehRoadWest.data() + VEH_ID_OFFSET) = 901;
    *(float*)(vehRoadWest.data() + VEH_POS_OFFSET) = -390.0f;
    *(BYTE*)(vehRoadWest.data() + VEH_BAY_ARRIVED_FLAG_OFFSET) = 1;

    *(int*)(vehHeliWest.data() + VEH_ID_OFFSET) = 902;
    *(float*)(vehHeliWest.data() + VEH_POS_OFFSET) = -380.0f;
    *(BYTE*)(vehHeliWest.data() + VEH_BAY_ARRIVED_FLAG_OFFSET) = 1;

    *(int*)(vehHeliEast.data() + VEH_ID_OFFSET) = 903;
    *(float*)(vehHeliEast.data() + VEH_POS_OFFSET) = 380.0f;
    *(BYTE*)(vehHeliEast.data() + VEH_BAY_ARRIVED_FLAG_OFFSET) = 1;

    std::vector<BYTE*> vehs = { vehRoadWest.data(), vehHeliWest.data(), vehHeliEast.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);

    // 1. Capture & Rebuild converts FireObj* -> BUILDING* -> BUILDING*
    const unsigned long long roadArgs[7] = { 0, (unsigned long long)(uintptr_t)fireObjWest.data(), (unsigned long long)(uintptr_t)vehRoadWest.data(), 0, 0, 0, 0 };
    const unsigned long long heliArgs[7] = { 0, (unsigned long long)(uintptr_t)fireObjWest.data(), (unsigned long long)(uintptr_t)vehHeliWest.data(), 0, 0, 0, 0 };
    unsigned long long outArgs[7] = {};
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_FIRESTATION_DISPATCH_ROAD_RVA, roadArgs, outArgs));
    TEST_CHECK(outArgs[1] == (unsigned long long)(uintptr_t)bldWest.data());
    TEST_CHECK(outArgs[2] == (unsigned long long)(uintptr_t)vehRoadWest.data());
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_FIRESTATION_DISPATCH_HELI_RVA, heliArgs, outArgs));
    TEST_CHECK(outArgs[1] == (unsigned long long)(uintptr_t)bldWest.data());
    TEST_CHECK(outArgs[2] == (unsigned long long)(uintptr_t)vehHeliWest.data());

    // Invalid FireObj* rejected cleanly
    std::vector<BYTE> orphanFireObj(0x60, 0);
    const unsigned long long badArgs[7] = { 0, (unsigned long long)(uintptr_t)orphanFireObj.data(), (unsigned long long)(uintptr_t)vehRoadWest.data(), 0, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CaptureAndRebuildForTest(SITE_FIRESTATION_DISPATCH_ROAD_RVA, badArgs, outArgs));

    // 2. TwoStates checks both burning building and fire vehicle
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_RegisterVehicleOwner(901, 1);
    TwoStates_RegisterVehicleOwner(902, 1);
    TwoStates_RegisterVehicleOwner(903, 2);

    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_FIRESTATION_DISPATCH_HELI_RVA, heliArgs));
    const unsigned long long eastBldArgs[7] = { 0, (unsigned long long)(uintptr_t)fireObjEast.data(), (unsigned long long)(uintptr_t)vehHeliWest.data(), 0, 0, 0, 0 };
    const unsigned long long eastVehArgs[7] = { 0, (unsigned long long)(uintptr_t)fireObjWest.data(), (unsigned long long)(uintptr_t)vehHeliEast.data(), 0, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_FIRESTATION_DISPATCH_HELI_RVA, eastBldArgs));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_FIRESTATION_DISPATCH_HELI_RVA, eastVehArgs));

    // 3. MP capture on heli (0x1D4FC8) skips 0x1D4FCD -> 0x1D4FD4, suppresses local call, and clears +0xD39 on replay
    Commands_SetSession(COMMANDS_HOST);
    unsigned long long ret = 99;
    unsigned long long retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_FIRESTATION_DISPATCH_HELI_RVA, heliArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_HELI_SKIP_RVA));
    TEST_CHECK(g_m1FireDispatchCalls == 0);
    TEST_CHECK(*(BYTE*)(vehHeliWest.data() + VEH_BAY_ARRIVED_FLAG_OFFSET) == 1);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    // Road capture + replay
    unsigned long long roadRetAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_ROAD_RVA + 5u);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_FIRESTATION_DISPATCH_ROAD_RVA, roadArgs, &ret, &roadRetAddr));
    TEST_CHECK(roadRetAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_ROAD_RVA + 5u));

    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_m1FireDispatchCalls == 2);
    TEST_CHECK(g_m1FireLastObj == fireObjWest.data());
    TEST_CHECK(g_m1FireLastVeh == vehRoadWest.data());
    TEST_CHECK(*(BYTE*)(vehHeliWest.data() + VEH_BAY_ARRIVED_FLAG_OFFSET) == 0);
    TEST_CHECK(*(BYTE*)(vehRoadWest.data() + VEH_BAY_ARRIVED_FLAG_OFFSET) == 1);

    // 4. Waiting lobby blocks heli dispatch and still skips 0x1D4FCD -> 0x1D4FD4
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_FIRESTATION_DISPATCH_HELI_RVA, heliArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_HELI_SKIP_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");

    // 5. Singleplayer isolation (!Commands_TakeUi()): passes through without skipping retAddr
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_FIRESTATION_DISPATCH_HELI_RVA, heliArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA));
    TEST_CHECK(g_m1FireDispatchCalls == 3);
    TEST_CHECK(g_m1FireLastObj == fireObjWest.data());
    TEST_CHECK(g_m1FireLastVeh == vehHeliWest.data());

    return true;
}

static bool SubTest_M1_AssignCoClickMutatorsAndTwoStates()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    g_m1AssignStartCalls = 0;
    g_m1AssignOfficeCalls = 0;
    g_m1UnassignOfficeCalls = 0;
    WriteAbsJmp12(g_mockExeBuffer + RVA_START_PLANNED_CONSTRUCTION, (void*)&MockAssignStartPlanned);
    WriteAbsJmp12(g_mockExeBuffer + RVA_ASSIGN_CONSTRUCTION_OFFICE, (void*)&MockAssignOffice);
    WriteAbsJmp12(g_mockExeBuffer + RVA_UNASSIGN_CONSTRUCTION_OFFICE, (void*)&MockUnassignOffice);

    std::vector<BYTE> officeWest(BUILDING_STRUCT_BYTES, 0), officeEast(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> siteWest(BUILDING_STRUCT_BYTES, 0), siteEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)officeWest.data() = 811;
    *(float*)(officeWest.data() + OBJ_POS_OFFSET) = -500.0f;
    *(int*)officeEast.data() = 812;
    *(float*)(officeEast.data() + OBJ_POS_OFFSET) = 500.0f;
    *(int*)siteWest.data() = 813;
    *(float*)(siteWest.data() + OBJ_POS_OFFSET) = -450.0f;
    *(int*)siteEast.data() = 814;
    *(float*)(siteEast.data() + OBJ_POS_OFFSET) = 450.0f;

    std::vector<BYTE*> blds = { officeWest.data(), officeEast.data(), siteWest.data(), siteEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    // 1. Capture & Rebuild roundtrip across WObx, WOOFbb, WOO sites
    const unsigned long long startArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)siteEast.data(), 1, 0, 0, 0, 0 };
    const unsigned long long assignArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)officeWest.data(), (unsigned long long)(uintptr_t)siteEast.data(), 0, 0, 0, 0 };
    const unsigned long long unassignArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)officeWest.data(), (unsigned long long)(uintptr_t)siteEast.data(), 0, 0, 0, 0 };
    unsigned long long outArgs[7] = {};

    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_ASSIGN_CO_SINGLE_START_RVA, startArgs, outArgs));
    TEST_CHECK(outArgs[1] == (unsigned long long)(uintptr_t)siteEast.data() && outArgs[2] == 1);
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_ASSIGN_CO_RECT_RAIL_ASSIGN_RVA, assignArgs, outArgs));
    TEST_CHECK(outArgs[1] == (unsigned long long)(uintptr_t)officeWest.data() &&
               outArgs[2] == (unsigned long long)(uintptr_t)siteEast.data());
    TEST_CHECK(GameCalls_CaptureAndRebuildForTest(SITE_ASSIGN_CO_RECT_UNASSIGN_RVA, unassignArgs, outArgs));
    TEST_CHECK(outArgs[1] == (unsigned long long)(uintptr_t)officeWest.data() &&
               outArgs[2] == (unsigned long long)(uintptr_t)siteEast.data());

    // 2. TwoStates policy consistency: AssignCoTool holding officeWest allows BOTH 0x1A8D90 and 0x161CB0 on siteEast,
    //    while AssignCoTool holding only officeEast blocks BOTH on siteWest & siteEast.
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    std::vector<BYTE> assignCoTool(0x200, 0);
    std::vector<BYTE*> toolOffices = { officeWest.data() };
    WriteMockVectorHeader(assignCoTool.data() + ASSIGN_CO_TOOL_OFFICES_OFF, toolOffices);
    *(BYTE**)(world + WORLD_ASSIGN_CO_TOOL_OFFSET) = assignCoTool.data();

    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_ASSIGN_CO_SINGLE_START_RVA, startArgs));
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_ASSIGN_CO_RECT_START_RVA, startArgs));
    TEST_CHECK(GameCalls_CheckTwoStatesArgsForTest(SITE_ASSIGN_CO_SINGLE_ASSIGN_RVA, assignArgs));

    toolOffices[0] = officeEast.data();
    WriteMockVectorHeader(assignCoTool.data() + ASSIGN_CO_TOOL_OFFICES_OFF, toolOffices);
    const unsigned long long startWestArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)siteWest.data(), 0, 0, 0, 0, 0 };
    const unsigned long long assignForeignCoArgs[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)officeEast.data(), (unsigned long long)(uintptr_t)siteWest.data(), 0, 0, 0, 0 };
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_ASSIGN_CO_SINGLE_START_RVA, startArgs));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_ASSIGN_CO_SINGLE_START_RVA, startWestArgs));
    TEST_CHECK(!GameCalls_CheckTwoStatesArgsForTest(SITE_ASSIGN_CO_SINGLE_ASSIGN_RVA, assignForeignCoArgs));

    // Restore West office in AssignCoTool & execute MP roundtrip
    toolOffices[0] = officeWest.data();
    WriteMockVectorHeader(assignCoTool.data() + ASSIGN_CO_TOOL_OFFICES_OFF, toolOffices);
    Commands_SetSession(COMMANDS_HOST);
    unsigned long long ret = 0;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_ASSIGN_CO_RECT_START_RVA, startArgs, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_ASSIGN_CO_RECT_ASSIGN_RVA, assignArgs, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_ASSIGN_CO_SINGLE_UNASSIGN_RVA, unassignArgs, &ret));
    TEST_CHECK(g_m1AssignStartCalls == 0 && g_m1AssignOfficeCalls == 0 && g_m1UnassignOfficeCalls == 0);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_m1AssignStartCalls == 1 && g_m1AssignStartLastBld == siteEast.data());
    TEST_CHECK(g_m1AssignOfficeCalls == 1 && g_m1AssignOfficeLastCo == officeWest.data() &&
               g_m1AssignOfficeLastTarget == siteEast.data());
    TEST_CHECK(g_m1UnassignOfficeCalls == 1 && g_m1UnassignOfficeLastCo == officeWest.data() &&
               g_m1UnassignOfficeLastTarget == siteEast.data());

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_M1_RunAuditBugRegressions()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    GameCalls_ResetSessionState();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Survey 2 Bug #1: TwoStates refusal on 0x7DA491 (CALL_RELEASE_VEHICLE, category == 6 cableway)
    //    must NOT clear veh+VEH_CABLEWAY_START_FLAG_OFF (0x198C) locally.
    std::vector<BYTE> vTypeCable(0x300, 0);
    *(int*)(vTypeCable.data() + VEH_TYPE_CATEGORY_OFFSET) = 6;
    std::vector<BYTE> vehWest(0x1B00, 0), vehEast(0x1B00, 0);
    *(int*)(vehWest.data() + VEH_ID_OFFSET) = 951;
    *(float*)(vehWest.data() + VEH_POS_OFFSET) = -300.0f;
    *(BYTE**)(vehWest.data() + VEH_TYPE_PTR_OFFSET) = vTypeCable.data();
    *(BYTE*)(vehWest.data() + VEH_CABLEWAY_START_FLAG_OFF) = 1;

    *(int*)(vehEast.data() + VEH_ID_OFFSET) = 952;
    *(float*)(vehEast.data() + VEH_POS_OFFSET) = 300.0f;
    *(BYTE**)(vehEast.data() + VEH_TYPE_PTR_OFFSET) = vTypeCable.data();
    *(BYTE*)(vehEast.data() + VEH_CABLEWAY_START_FLAG_OFF) = 1;

    std::vector<BYTE*> vehs = { vehWest.data(), vehEast.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);

    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_RegisterVehicleOwner(951, 1);
    TwoStates_RegisterVehicleOwner(952, 2);
    Commands_SetSession(COMMANDS_HOST);

    unsigned long long ret = 0;
    const unsigned long long relEastArgs[7] = { (unsigned long long)(uintptr_t)vehEast.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(0x7DA491u, relEastArgs, &ret));
    TEST_CHECK(*(BYTE*)(vehEast.data() + VEH_CABLEWAY_START_FLAG_OFF) == 1);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    const unsigned long long relWestArgs[7] = { (unsigned long long)(uintptr_t)vehWest.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(0x7DA491u, relWestArgs, &ret));
    TEST_CHECK(*(BYTE*)(vehWest.data() + VEH_CABLEWAY_START_FLAG_OFF) == 0);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();

    // 2. Survey 2 Bug #3: GameCalls_ResetSessionState() and HandleWaitingLobbyBlock() clear stale g_skipDepotRowCouple.
    Commands_SetSession(COMMANDS_HOST);
    const unsigned long long selfDropUncouple[7] = {
        (unsigned long long)(uintptr_t)world,
        (unsigned long long)(uintptr_t)vehWest.data(),
        (unsigned long long)(uintptr_t)vehWest.data(),
        0, 0, 0, 0
    };
    TEST_CHECK(GameCalls_RunSiteForTest(0x7C8027u, selfDropUncouple, &ret));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // Reset session state and verify subsequent non-self couple at 0x7C8078 is NOT suppressed
    GameCalls_ResetSessionState();
    const unsigned long long validCouple[7] = {
        (unsigned long long)(uintptr_t)world,
        (unsigned long long)(uintptr_t)vehWest.data(),
        (unsigned long long)(uintptr_t)vehEast.data(),
        0, 1, 0, 0
    };
    TEST_CHECK(GameCalls_RunSiteForTest(0x7C8078u, validCouple, &ret));
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    // Repeat self-drop, trigger waiting lobby block, and verify g_skipDepotRowCouple was cleared
    Commands_SetSession(COMMANDS_OFF);
    Commands_SetSession(COMMANDS_HOST);
    TEST_CHECK(GameCalls_RunSiteForTest(0x7C8027u, selfDropUncouple, &ret));
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    const unsigned long long dummySkin[7] = { (unsigned long long)(uintptr_t)vehWest.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(0x7DA8ABu, dummySkin, &ret));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TEST_CHECK(GameCalls_RunSiteForTest(0x7C8078u, validCouple, &ret));
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    // 3. Survey 2 Bug #2: SafeReadPod / SEH protection on HandleInviteCitizensUi and ExecuteNonCaptureCall
    Commands_SetSession(COMMANDS_OFF);
    WriteAbsJmp12(g_mockExeBuffer + 0x3FFA10u, (void*)&MockPlaySoundNoop);
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    const unsigned long long badInviteArgs[7] = { (unsigned long long)(uintptr_t)world, 0, 0, 0x12345678ULL, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(0x6FEFBEu, badInviteArgs, &ret));
    const unsigned long long badReleaseArgs[7] = { 0x12345678ULL, 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(0x7DA491u, badReleaseArgs, &ret));
    TwoStates_OnSessionEnded();

    return true;
}

bool Test_HookFixesAndRunAudit_M1()
{
    printf("    [M1-1: 9 Removed Broken Sites & 12 Verified Click Mutators] ...\n"); fflush(stdout);
    if (!SubTest_M1_RemovedAndAddedCallSites()) return false;

    printf("    [M1-2: CALL_FIRE_DISPATCH Behavior, Skip & Replay] ...\n"); fflush(stdout);
    if (!SubTest_M1_FireDispatchBehaviorAndSkip()) return false;

    printf("    [M1-3: assign_co Click Mutators & TwoStates Policy] ...\n"); fflush(stdout);
    if (!SubTest_M1_AssignCoClickMutatorsAndTwoStates()) return false;

    printf("    [M1-4: Run/kSites Audit Regressions (Pre-Check Mutation, Depot Couple, SEH)] ...\n"); fflush(stdout);
    if (!SubTest_M1_RunAuditBugRegressions()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Audit-7 Sync Fixes Test Suite (Bugs #1-#7)
// ---------------------------------------------------------------------------

static int g_a7DiscardCargoCalls = 0;
static BYTE* g_a7DiscardLastVeh = nullptr;
static unsigned char g_a7DiscardLastFlag = 0xFFu;
static void __fastcall MockA7DiscardVehCargo(BYTE* veh, unsigned char flag)
{
    ++g_a7DiscardCargoCalls;
    g_a7DiscardLastVeh = veh;
    g_a7DiscardLastFlag = flag;
}

static int g_a7DiscardCarriedCalls = 0;
static BYTE* g_a7CarriedWorld = nullptr;
static BYTE* g_a7CarriedVeh = nullptr;
static unsigned long long g_a7CarriedArgs[5] = {};
static void __fastcall MockA7DiscardCarriedVeh(BYTE* world, BYTE* veh, unsigned char a3, unsigned char a4,
                                               unsigned char a5, unsigned char a6, unsigned a7)
{
    ++g_a7DiscardCarriedCalls;
    g_a7CarriedWorld = world;
    g_a7CarriedVeh = veh;
    g_a7CarriedArgs[0] = a3;
    g_a7CarriedArgs[1] = a4;
    g_a7CarriedArgs[2] = a5;
    g_a7CarriedArgs[3] = a6;
    g_a7CarriedArgs[4] = a7;
}

static int g_a7CanDiscardStorageCalls = 0;
static unsigned char g_a7CanDiscardStorageRet = 1;
static unsigned char __fastcall MockA7CanDiscardStorage(BYTE* /*slot*/)
{
    ++g_a7CanDiscardStorageCalls;
    return g_a7CanDiscardStorageRet;
}

static int g_a7RepairCancelCalls = 0;
static BYTE* g_a7RepairLastWorld = nullptr;
static BYTE* g_a7RepairLastBld = nullptr;
static int g_a7RepairLastIdx = -1;
static unsigned g_a7RepairLastFlag = 0xFFu;
static void __fastcall MockA7RepairCancelQueue(BYTE* world, BYTE* bld, int idx, unsigned flag)
{
    ++g_a7RepairCancelCalls;
    g_a7RepairLastWorld = world;
    g_a7RepairLastBld = bld;
    g_a7RepairLastIdx = idx;
    g_a7RepairLastFlag = flag;
}

static int g_a7PlaySoundCalls = 0;
static unsigned char g_a7PlaySoundLastR8 = 0xFFu;
static void __fastcall MockA7PlaySound(BYTE* /*world*/, const char* /*path*/, unsigned char flag)
{
    ++g_a7PlaySoundCalls;
    g_a7PlaySoundLastR8 = flag;
}

static int g_a7CrimeExpireCalls = 0;
static BYTE* g_a7CrimeExpireLastCase = nullptr;
static float g_a7CrimeExpireLastArg = -1.0f;
static void __fastcall MockA7CrimeExpireCase(BYTE* /*world*/, BYTE* caseObj, float a3)
{
    ++g_a7CrimeExpireCalls;
    g_a7CrimeExpireLastCase = caseObj;
    g_a7CrimeExpireLastArg = a3;
}

static int g_a7CrimeDeleteCalls = 0;
static BYTE* g_a7CrimeDeleteLastCase = nullptr;
static void __fastcall MockA7CrimeDeleteCase(BYTE* /*world*/, BYTE* caseObj)
{
    ++g_a7CrimeDeleteCalls;
    g_a7CrimeDeleteLastCase = caseObj;
    if (caseObj) memset(caseObj, 0xDD, 0x78u); // simulate heap free / clobber
}

static int g_a7PfCtorCalls = 0;
static int g_a7PfDtorCalls = 0;
static int g_a7DoRemoveLinkCalls = 0;
static BYTE* g_a7DoRemoveLastDo = nullptr;
static BYTE* g_a7DoRemoveLastTarget = nullptr;
static int g_a7PfAddStartCalls = 0;
static unsigned long long g_a7PfAddLastNode = 0;
static float g_a7PfAddLastArg5 = 0.0f;
static int g_a7PfRunCalls = 0;
static int32_t g_a7PfSeenB4 = 0;
static int32_t g_a7PfSeenB0 = 0;
static float g_a7PfSeen2C = 0.0f;

static std::vector<BYTE*>* g_a7PfMockResultsPtr = nullptr;
static std::vector<BYTE*>* g_a7DoVecStoragePtr = nullptr;
static std::vector<std::vector<BYTE>>* g_a7OpNewArenaPtr = nullptr;
static void* __fastcall MockA7OpNew(size_t bytes)
{
    if (!g_a7OpNewArenaPtr) return nullptr;
    g_a7OpNewArenaPtr->emplace_back(bytes, (BYTE)0);
    return g_a7OpNewArenaPtr->back().data();
}
static void* __fastcall MockA7CfgCtor(void* cfg)
{
    if (cfg) {
        *(uint64_t*)((BYTE*)cfg + 0x08) = 0x11112222ULL;
        *(uint64_t*)((BYTE*)cfg + 0xD0) = 0x33334444ULL;
    }
    return cfg;
}
static void __fastcall MockA7VecPush(void* vec, void** elem)
{
    if (!vec || !elem || !g_a7DoVecStoragePtr) return;
    g_a7DoVecStoragePtr->push_back((BYTE*)*elem);
    BYTE** beg = g_a7DoVecStoragePtr->data();
    BYTE** end = beg + g_a7DoVecStoragePtr->size();
    *(BYTE***)((BYTE*)vec + 0) = beg;
    *(BYTE***)((BYTE*)vec + 8) = end;
    *(BYTE***)((BYTE*)vec + 16) = end;
}

static void* __fastcall MockA7PfCtor(void* pf)
{
    ++g_a7PfCtorCalls;
    if (pf) memset(pf, 0, 0x400u);
    return pf;
}

static void __fastcall MockA7PfDtor(void* /*pf*/)
{
    ++g_a7PfDtorCalls;
}

static void __fastcall MockA7DoRemoveLink(unsigned long long /*unused*/, BYTE* doBld, BYTE* targetBld)
{
    ++g_a7DoRemoveLinkCalls;
    g_a7DoRemoveLastDo = doBld;
    g_a7DoRemoveLastTarget = targetBld;
}

static void __fastcall MockA7PfAddStart(void* pf, unsigned long long nodePtr, float /*f3*/, int /*i4*/, float f5)
{
    ++g_a7PfAddStartCalls;
    g_a7PfAddLastNode = nodePtr;
    g_a7PfAddLastArg5 = f5;
    if (pf) {
        g_a7PfSeenB4 = *(const int32_t*)((const BYTE*)pf + 0xB4);
        g_a7PfSeenB0 = *(const int32_t*)((const BYTE*)pf + 0xB0);
        g_a7PfSeen2C = *(const float*)((const BYTE*)pf + 0x2C);
    }
}

static void __fastcall MockA7PfRun(void* pf)
{
    ++g_a7PfRunCalls;
    if (pf && g_a7PfMockResultsPtr && !g_a7PfMockResultsPtr->empty()) {
        BYTE** beg = g_a7PfMockResultsPtr->data();
        BYTE** end = beg + g_a7PfMockResultsPtr->size();
        *(BYTE***)((BYTE*)pf + 0x1B0) = beg;
        *(BYTE***)((BYTE*)pf + 0x1B8) = end;
    }
}

static int g_a7CopyToolCalls = 0;
static BYTE* g_a7CopyHoverDst = nullptr;
static unsigned long long MockA7CopyWeightsTool(unsigned long long world, unsigned long long /*a2*/)
{
    ++g_a7CopyToolCalls;
    if (!world) return 0;
    *(BYTE**)(world + WORLD_HOVERED_BLD_OFF) = g_a7CopyHoverDst;
    if (g_mockExeBuffer && g_mockExeBuffer[0xA54E91] != 0 && g_a7CopyHoverDst) {
        BYTE* srcBld = *(BYTE**)(world + WORLD_COPY_TOOL_SRC_BLD_OFF);
        if (srcBld && srcBld != g_a7CopyHoverDst) {
            memcpy(g_a7CopyHoverDst + BLD_COPY_WEIGHTS_BEGIN_OFFSET,
                   srcBld + BLD_COPY_WEIGHTS_BEGIN_OFFSET,
                   BLD_COPY_WEIGHTS_END_OFFSET - BLD_COPY_WEIGHTS_BEGIN_OFFSET);
        }
    }
    return 1;
}

static int g_a7RebuildStoragesCalls = 0;
static std::vector<BYTE*> g_a7RebuildStoragesSeenBlds;
static void MockA7RebuildBldStorages(BYTE* /*world*/, BYTE* bld)
{
    ++g_a7RebuildStoragesCalls;
    g_a7RebuildStoragesSeenBlds.push_back(bld);
}

static bool SubTest_A7_Bug1_ResearchRecomputeAndSpIsolation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    Commands_SetSession(COMMANDS_OFF);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Verify exact 16-byte SOVIET64.exe v1.1.1.13 RVA_RESEARCH_RECOMPUTE prologue match & stale rejection
    const BYTE validPrologue[16] = {
        0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
        0xEC, 0x60, 0x48, 0x8D, 0x15, 0x97, 0x49, 0x5B
    };
    const BYTE stalePrologue[16] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
        0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
    };
    TEST_CHECK(TwoStates_MatchesResearchRecomputePrologueForTest(validPrologue, sizeof(validPrologue)));
    TEST_CHECK(!TwoStates_MatchesResearchRecomputePrologueForTest(stalePrologue, sizeof(stalePrologue)));
    TEST_CHECK(!TwoStates_MatchesResearchRecomputePrologueForTest(validPrologue, 15));

    // 2. Fallback clears stale unlocks[] rather than preserving unlocks copied from another state/save
    std::vector<BYTE> resBuf(8 * RESEARCH_ITEM_STRIDE, 0);
    BYTE* t0 = resBuf.data() + 0 * RESEARCH_ITEM_STRIDE;
    BYTE* t2 = resBuf.data() + 2 * RESEARCH_ITEM_STRIDE;
    BYTE* t5 = resBuf.data() + 5 * RESEARCH_ITEM_STRIDE;
    t0[RESEARCH_ITEM_INITIAL_OFFSET] = 1;
    t2[RESEARCH_ITEM_PROGRESS_OFFSET] = 0; // set float 1.0f below
    *(float*)(t2 + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;
    t2[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
    *(float*)(t5 + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.0f;
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();

    memset(world + WORLD_RESEARCH_UNLOCKS_OFFSET, 0, WORLD_RESEARCH_UNLOCKS_BYTES);
    world[WORLD_RESEARCH_UNLOCKS_OFFSET + 5] = 1; // stale unlock on incomplete tech 5
    g_mockExeBuffer[RVA_RESEARCH_RECOMPUTE] = 0xCC; // force fallback path
    TwoStates_RecomputeEngineResearchForTest(world);
    TEST_CHECK(world[WORLD_RESEARCH_UNLOCKS_OFFSET + 5] == 0);
    TEST_CHECK(world[WORLD_RESEARCH_UNLOCKS_OFFSET + 2] == 1);
    TEST_CHECK(t2[RESEARCH_ITEM_AVAILABLE_OFFSET] == 0);
    TEST_CHECK(t0[RESEARCH_ITEM_AVAILABLE_OFFSET] == 1);

    // 3. Singleplayer H_UnivResearchPanel isolation: never auto-completes near-complete orphaned tech in SP
    std::vector<BYTE> univSp(BUILDING_STRUCT_BYTES, 0);
    *(int*)univSp.data() = 510;
    *(float*)(univSp.data() + OBJ_POS_OFFSET) = -400.0f;
    *(float*)(t5 + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.9999f;
    *(BYTE**)(t5 + RESEARCH_ITEM_BLD_PTR_OFFSET) = nullptr;
    world[WORLD_RESEARCH_UNLOCKS_OFFSET + 5] = 0;
    TwoStates_UnivResearchPanelForTest(world, univSp.data(), -1, 0.5f);
    TEST_CHECK(*(float*)(t5 + RESEARCH_ITEM_PROGRESS_OFFSET) < 1.0f);
    TEST_CHECK(world[WORLD_RESEARCH_UNLOCKS_OFFSET + 5] == 0);
    return true;
}

static bool SubTest_A7_Bug2_DiscardAndEmptySites()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    GameCalls_ResetSessionState();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    g_a7DiscardCargoCalls = 0;
    g_a7DiscardCarriedCalls = 0;
    g_a7CanDiscardStorageCalls = 0;
    g_a7CanDiscardStorageRet = 1;
    WriteAbsJmp12(g_mockExeBuffer + FN_DISCARD_VEH_CARGO_RVA, (void*)&MockA7DiscardVehCargo);
    WriteAbsJmp12(g_mockExeBuffer + FN_DISCARD_CARRIED_VEH_RVA, (void*)&MockA7DiscardCarriedVeh);
    WriteAbsJmp12(g_mockExeBuffer + FN_CAN_DISCARD_STORAGE_RVA, (void*)&MockA7CanDiscardStorage);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_DISCARD_VEH_CARGO_RVA, FN_DISCARD_VEH_CARGO_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_DISCARD_CARRIED_VEH_RVA, FN_DISCARD_CARRIED_VEH_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_EMPTY_STORAGE_RVA, FN_CAN_DISCARD_STORAGE_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_EMPTY_BLD_VEH_CARGO_POPUP_RVA, FN_DISCARD_VEH_CARGO_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_EMPTY_BLD_VEH_CARGO_CONFIRM_RVA, FN_DISCARD_VEH_CARGO_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_DISCARD_VEH_CARGO_RVA), "Vb") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_DISCARD_CARRIED_VEH_RVA), "WVbbbbI") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_EMPTY_STORAGE_RVA), "BI") == 0);

    // Build West/East vehicles + wagon for unloading counter reset
    std::vector<BYTE> vehWest(0x1B00, 0), vehWagon(0x1B00, 0), vehEast(0x1B00, 0);
    *(int*)(vehWest.data() + VEH_ID_OFFSET) = 101;
    *(float*)(vehWest.data() + VEH_POS_OFFSET) = -500.0f;
    *(int*)(vehWagon.data() + VEH_ID_OFFSET) = 102;
    *(float*)(vehWagon.data() + VEH_POS_OFFSET) = -500.0f;
    *(int*)(vehEast.data() + VEH_ID_OFFSET) = 103;
    *(float*)(vehEast.data() + VEH_POS_OFFSET) = 500.0f;
    std::vector<BYTE*> wagons = { vehWagon.data() };
    WriteMockVectorHeader(vehWest.data() + VEH_ATTACHED_WAGONS_BEGIN_OFF, wagons);
    std::vector<BYTE*> vehs = { vehWest.data(), vehWagon.data(), vehEast.data() };
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);

    // Build West/East buildings + storage slots
    std::vector<BYTE> bldWest(BUILDING_STRUCT_BYTES, 0), bldEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)bldWest.data() = 201;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -500.0f;
    *(int*)bldEast.data() = 202;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = 500.0f;
    *(BYTE**)(vehWest.data() + VEH_BAY_TARGET_BLD_OFFSET) = bldWest.data();

    auto setUnloadBusy = [](BYTE* v) {
        *(uint64_t*)(v + VEH_LOAD_TIMER_55C_OFF) = 42ULL;
        *(uint64_t*)(v + VEH_LOAD_TIMER_594_OFF) = 99ULL;
        *(float*)(v + VEH_LOAD_RATE_568_OFF) = 7.5f;
        *(uint32_t*)(v + VEH_LOAD_STATE_179C_OFF) = 3u;
        *(uint32_t*)(v + VEH_LOAD_STATE_564_OFF) = 4u;
        *(uint32_t*)(v + VEH_LOAD_STATE_1310_OFF) = 5u;
        v[VEH_LOAD_FLAG_59C_OFF] = 1;
    };
    setUnloadBusy(vehWest.data());
    setUnloadBusy(vehWagon.data());

    std::vector<BYTE> slotSubWest(2 * 0x10u, 0), slotSubEast(0x10u, 0);
    *(float*)(slotSubWest.data() + 0 * 0x10u + 8u) = 12.5f;
    *(float*)(slotSubWest.data() + 1 * 0x10u + 8u) = 7.0f;
    *(float*)(slotSubEast.data() + 0 * 0x10u + 8u) = 9.0f;
    std::vector<BYTE> slotsWest(BLD_STORAGE_SLOT_SIZE, 0), slotsEast(BLD_STORAGE_SLOT_SIZE, 0);
    *(BYTE**)(slotsWest.data() + 0) = slotSubWest.data();
    *(BYTE**)(slotsWest.data() + 8) = slotSubWest.data() + slotSubWest.size();
    *(BYTE**)(slotsEast.data() + 0) = slotSubEast.data();
    *(BYTE**)(slotsEast.data() + 8) = slotSubEast.data() + slotSubEast.size();
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 0) = slotsWest.data();
    *(BYTE**)(bldWest.data() + BLD_STORAGE_OFFSET + 8) = slotsWest.data() + slotsWest.size();
    *(BYTE**)(bldEast.data() + BLD_STORAGE_OFFSET + 0) = slotsEast.data();
    *(BYTE**)(bldEast.data() + BLD_STORAGE_OFFSET + 8) = slotsEast.data() + slotsEast.size();

    std::vector<BYTE*> blds = { bldWest.data(), bldEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    // 1. SP pass-through on SITE_EMPTY_STORAGE_RVA returns native CanDiscardStorage result (1)
    unsigned long long ret = 99;
    const unsigned long long slotWestArgs[7] = { (unsigned long long)(uintptr_t)slotsWest.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_EMPTY_STORAGE_RVA, slotWestArgs, &ret));
    TEST_CHECK(ret == 1 && g_a7CanDiscardStorageCalls == 1);

    // 2. TwoStates + Lockstep Capture
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_RegisterVehicleOwner(101, 1);
    TwoStates_RegisterVehicleOwner(102, 1);
    TwoStates_RegisterVehicleOwner(103, 2);
    Commands_SetSession(COMMANDS_HOST);

    // Foreign refusals
    const unsigned long long discardEast[7] = { (unsigned long long)(uintptr_t)vehEast.data(), 1, 0, 0, 0, 0, 0 };
    const unsigned long long carriedEast[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)vehEast.data(), 1, 0, 0, 0, 0x3F800000u };
    const unsigned long long slotEastArgs[7] = { (unsigned long long)(uintptr_t)slotsEast.data(), 0, 0, 0, 0, 0, 0 };
    unsigned long long retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DISCARD_VEH_CARGO_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_DISCARD_VEH_CARGO_RVA, discardEast, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DISCARD_VEH_CARGO_SKIP_RVA));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_DISCARD_CARRIED_VEH_RVA, carriedEast, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_EMPTY_STORAGE_RVA, slotEastArgs, &ret) && ret == 0);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // Waiting Lobby block on SITE_EMPTY_STORAGE_RVA
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_EMPTY_STORAGE_RVA, slotWestArgs, &ret) && ret == 0);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;

    // Own West actions: capture, skip retAddr, zero local mutation before boundary
    const unsigned long long discardWest[7] = { (unsigned long long)(uintptr_t)vehWest.data(), 1, 0, 0, 0, 0, 0 };
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DISCARD_VEH_CARGO_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_DISCARD_VEH_CARGO_RVA, discardWest, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DISCARD_VEH_CARGO_SKIP_RVA));
    TEST_CHECK(*(uint64_t*)(vehWest.data() + VEH_LOAD_TIMER_55C_OFF) == 42ULL);

    const unsigned long long carriedWest[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)vehWest.data(), 1, 0, 0, 0, 0x3F800000u };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_DISCARD_CARRIED_VEH_RVA, carriedWest, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_EMPTY_STORAGE_RVA, slotWestArgs, &ret) && ret == 0);
    TEST_CHECK(*(float*)(slotSubWest.data() + 0 * 0x10u + 8u) == 12.5f);

    const unsigned long long emptyParkedArgs[7] = { (unsigned long long)(uintptr_t)vehWagon.data(), 0, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_EMPTY_BLD_VEH_CARGO_POPUP_RVA, emptyParkedArgs, &ret));
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_EMPTY_BLD_VEH_CARGO_CONFIRM_RVA, emptyParkedArgs, &ret));
    TEST_CHECK(g_a7DiscardCarriedCalls == 0);

    // When CanDiscardStorage returns 0 at boundary, storage remains unchanged on first test
    g_a7CanDiscardStorageRet = 0;
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_a7DiscardCargoCalls == 3);
    TEST_CHECK(g_a7DiscardLastVeh == vehWagon.data() && g_a7DiscardLastFlag == 0);
    TEST_CHECK(*(uint64_t*)(vehWest.data() + VEH_LOAD_TIMER_55C_OFF) == 0ULL);
    TEST_CHECK(*(float*)(vehWest.data() + VEH_LOAD_RATE_568_OFF) == 1.0f);
    TEST_CHECK(vehWest[VEH_LOAD_FLAG_59C_OFF] == 0);
    TEST_CHECK(*(uint64_t*)(vehWagon.data() + VEH_LOAD_TIMER_55C_OFF) == 0ULL);
    TEST_CHECK(g_a7DiscardCarriedCalls == 1 && g_a7CarriedVeh == vehWest.data() && g_a7CarriedArgs[0] == 1 && g_a7CarriedArgs[4] == 0x3F800000u);
    TEST_CHECK(*(float*)(slotSubWest.data() + 0 * 0x10u + 8u) == 12.5f);

    // Solo vehicle (empty 0x3A0..0x3A8) discarding at bay preserves unloading counters (exact parity with 0x8084DC..0x8084F0),
    // and re-submitting empty storage with CanDiscardStorageRet == 1 zeros both subslots
    std::vector<BYTE> soloVeh(0x1B00, 0);
    *(int*)(soloVeh.data() + VEH_ID_OFFSET) = 104;
    *(float*)(soloVeh.data() + VEH_POS_OFFSET) = -500.0f;
    *(BYTE**)(soloVeh.data() + VEH_BAY_TARGET_BLD_OFFSET) = bldWest.data();
    setUnloadBusy(soloVeh.data());
    vehs.push_back(soloVeh.data());
    WriteMockVectorHeader(world + WORLD_VEHICLES_OFFSET, vehs);
    TwoStates_RegisterVehicleOwner(104, 1);
    const unsigned long long discardSolo[7] = { (unsigned long long)(uintptr_t)soloVeh.data(), 1, 0, 0, 0, 0, 0 };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_DISCARD_VEH_CARGO_RVA, discardSolo, &ret));

    g_a7CanDiscardStorageRet = 1;
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_EMPTY_STORAGE_RVA, slotWestArgs, &ret) && ret == 0);
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_a7DiscardCargoCalls == 4);
    TEST_CHECK(g_a7DiscardLastVeh == soloVeh.data() && g_a7DiscardLastFlag == 1);
    TEST_CHECK(*(uint64_t*)(soloVeh.data() + VEH_LOAD_TIMER_55C_OFF) == 42ULL);
    TEST_CHECK(*(float*)(soloVeh.data() + VEH_LOAD_RATE_568_OFF) == 7.5f);
    TEST_CHECK(*(float*)(slotSubWest.data() + 0 * 0x10u + 8u) == 0.0f);
    TEST_CHECK(*(float*)(slotSubWest.data() + 1 * 0x10u + 8u) == 0.0f);

    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_A7_Bug3_Bug4_RepairAndCrimeCancel()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    GameCalls_ResetSessionState();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    g_a7RepairCancelCalls = 0;
    g_a7PlaySoundCalls = 0;
    g_a7PlaySoundLastR8 = 0xFFu;
    g_a7CrimeExpireCalls = 0;
    g_a7CrimeDeleteCalls = 0;
    WriteAbsJmp12(g_mockExeBuffer + FN_REPAIR_CANCEL_QUEUE_RVA, (void*)&MockA7RepairCancelQueue);
    WriteAbsJmp12(g_mockExeBuffer + FN_UI_PLAY_SOUND_RVA, (void*)&MockA7PlaySound);
    WriteAbsJmp12(g_mockExeBuffer + RVA_CRIME_EXPIRE_CASE, (void*)&MockA7CrimeExpireCase);
    WriteAbsJmp12(g_mockExeBuffer + FN_CRIME_DELETE_CASE_RVA, (void*)&MockA7CrimeDeleteCase);

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_REPAIR_CANCEL_QUEUE_RVA, FN_REPAIR_CANCEL_QUEUE_RVA));
    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_CANCEL_CRIME_ASSIGN_RVA, FN_UI_PLAY_SOUND_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_REPAIR_CANCEL_QUEUE_RVA), "WBII") == 0);
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_CANCEL_CRIME_ASSIGN_RVA), "WxIBI") == 0);

    std::vector<BYTE> bldWest(BUILDING_STRUCT_BYTES, 0), bldEast(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> region(0x200, 0), cityStats(0x400, 0);
    *(int*)bldWest.data() = 301;
    *(float*)(bldWest.data() + OBJ_POS_OFFSET) = -600.0f;
    *(BYTE**)(bldWest.data() + BLD_CITY_REGION_PTR_OFF) = region.data();
    *(BYTE**)(region.data() + REGION_CITY_STATS_PTR_OFF) = cityStats.data();
    *(int*)bldEast.data() = 302;
    *(float*)(bldEast.data() + OBJ_POS_OFFSET) = 600.0f;
    std::vector<BYTE*> blds = { bldWest.data(), bldEast.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    std::vector<BYTE> crimeCase(0x78, 0);
    *(int32_t*)(crimeCase.data() + CASE_STATUS_OFF) = 5;
    *(BYTE**)(crimeCase.data() + CASE_BLD_OFF) = bldWest.data();
    *(int32_t*)(crimeCase.data() + CASE_TYPE_OFF) = 2;
    std::vector<BYTE*> cases = { crimeCase.data() };
    WriteMockVectorHeader(world + WORLD_CRIME_CASES_BEGIN_OFF, cases);

    // 1. SP path for SITE_CANCEL_CRIME_ASSIGN_RVA passes r8b == 1 to PlaySound & does not skip retAddr
    unsigned long long ret = 0;
    const unsigned long long crimeWestArgs[7] = {
        (unsigned long long)(uintptr_t)world, 0,
        (unsigned long long)(uintptr_t)crimeCase.data(),
        (unsigned long long)(uintptr_t)bldWest.data(), 0, 0, 0
    };
    unsigned long long retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_CANCEL_CRIME_ASSIGN_RVA, crimeWestArgs, &ret, &retAddr));
    TEST_CHECK(g_a7PlaySoundCalls == 1 && g_a7PlaySoundLastR8 == 1);
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA));

    // 2. TwoStates & Lockstep Capture + Boundary Replay
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    Commands_SetSession(COMMANDS_HOST);

    const unsigned long long repairEast[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldEast.data(), 2, 0, 0, 0, 0 };
    const unsigned long long repairWest[7] = { (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)bldWest.data(), 2, 0, 0, 0, 0 };
    const unsigned long long crimeEastArgs[7] = {
        (unsigned long long)(uintptr_t)world, 0,
        (unsigned long long)(uintptr_t)crimeCase.data(),
        (unsigned long long)(uintptr_t)bldEast.data(), 0, 0, 0
    };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_REPAIR_CANCEL_QUEUE_RVA, repairEast, &ret));
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_CANCEL_CRIME_ASSIGN_RVA, crimeEastArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_SKIP_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // Waiting Lobby block on both
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_REPAIR_CANCEL_QUEUE_RVA, repairWest, &ret));
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_CANCEL_CRIME_ASSIGN_RVA, crimeWestArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_SKIP_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;

    // Capture wrong-check rejection message while live, then submit live West repair + crime cancel
    BYTE rawCrimeMsg[256] = {};
    size_t crimeMsgSize = 0;
    TEST_CHECK(GameCalls_CaptureMsgForTest(SITE_CANCEL_CRIME_ASSIGN_RVA, crimeWestArgs, rawCrimeMsg, sizeof(rawCrimeMsg), &crimeMsgSize));

    TEST_CHECK(GameCalls_RunSiteForTest(SITE_REPAIR_CANCEL_QUEUE_RVA, repairWest, &ret));
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_CANCEL_CRIME_ASSIGN_RVA, crimeWestArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_CANCEL_CRIME_ASSIGN_SKIP_RVA));
    TEST_CHECK(g_a7RepairCancelCalls == 0 && g_a7CrimeDeleteCalls == 0);

    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_a7RepairCancelCalls == 1 && g_a7RepairLastWorld == world && g_a7RepairLastBld == bldWest.data() &&
               g_a7RepairLastIdx == 2 && g_a7RepairLastFlag == 0);
    TEST_CHECK(g_a7CrimeExpireCalls == 1 && g_a7CrimeExpireLastCase == crimeCase.data() && g_a7CrimeExpireLastArg == 0.0f);
    TEST_CHECK(g_a7CrimeDeleteCalls == 1 && g_a7CrimeDeleteLastCase == crimeCase.data());
    TEST_CHECK(*(int32_t*)(cityStats.data() + CITY_STATS_CRIME2_OFF) == 1);
    TEST_CHECK(*(int32_t*)(world + WORLD_CRIME_EXPIRED_STAT2_OFF) == 1);
    TEST_CHECK(*(uint16_t*)(bldWest.data() + BLD_CRIME_CANCEL_COUNTERS_OFF + 2u * 4u) == 1u);

    // Mismatched crimeCase check must cleanly abort without calling expire/delete or bumping stats again
    TEST_CHECK(GameCalls_ApplyCommand(rawCrimeMsg, crimeMsgSize));
    TEST_CHECK(g_a7CrimeExpireCalls == 1 && g_a7CrimeDeleteCalls == 1);
    TEST_CHECK(*(int32_t*)(world + WORLD_CRIME_EXPIRED_STAT2_OFF) == 1);

    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_A7_Bug5_DoFindConnections()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    GameCalls_ResetSessionState();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    g_a7PfCtorCalls = 0;
    g_a7PfDtorCalls = 0;
    g_a7DoRemoveLinkCalls = 0;
    g_a7PfAddStartCalls = 0;
    g_a7PfAddLastNode = 0;
    g_a7PfAddLastArg5 = 0.0f;
    g_a7PfRunCalls = 0;
    g_a7PfSeenB4 = 0;
    g_a7PfSeenB0 = 0;
    g_a7PfSeen2C = 0.0f;
    WriteAbsJmp12(g_mockExeBuffer + FN_PF_CTOR_RVA, (void*)&MockA7PfCtor);
    WriteAbsJmp12(g_mockExeBuffer + FN_PF_DTOR_RVA, (void*)&MockA7PfDtor);
    WriteAbsJmp12(g_mockExeBuffer + FN_DO_REMOVE_LINK_RVA, (void*)&MockA7DoRemoveLink);
    WriteAbsJmp12(g_mockExeBuffer + FN_PF_ADD_START_RVA, (void*)&MockA7PfAddStart);
    WriteAbsJmp12(g_mockExeBuffer + FN_PF_RUN_RVA, (void*)&MockA7PfRun);
    WriteAbsJmp12(g_mockExeBuffer + FN_GAME_OP_NEW_RVA, (void*)&MockA7OpNew);
    WriteAbsJmp12(g_mockExeBuffer + FN_DO_VEC_PUSH_RVA, (void*)&MockA7VecPush);
    WriteAbsJmp12(g_mockExeBuffer + FN_DO_CFG_CTOR_RVA, (void*)&MockA7CfgCtor);
    *(float*)(g_mockExeBuffer + FLT_NEG_ONE_RVA) = -1.0f;

    std::vector<BYTE*> pfResults;
    std::vector<BYTE*> doVecStorage;
    std::vector<std::vector<BYTE>> opNewArena;
    opNewArena.reserve(64);
    g_a7PfMockResultsPtr = &pfResults;
    g_a7DoVecStoragePtr = &doVecStorage;
    g_a7OpNewArenaPtr = &opNewArena;

    // 8 candidate path results: [0] is depot (cat 0x2D, skipped), [1..7] are 7 factories (cat 5, max 6 attached)
    std::vector<BYTE> btypeDepot(0x400, 0), btypeFactory(0x400, 0);
    *(int32_t*)(btypeDepot.data() + 0x360) = 0x2D;
    *(int32_t*)(btypeFactory.data() + 0x360) = 5;
    std::vector<BYTE> resNodes(8 * 0x80u, 0);
    std::vector<BYTE*> resTargets(8, nullptr);
    std::vector<BYTE> candBlds(8 * BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> resStructs(8 * 0x48u, 0);
    std::vector<BYTE*> resLastPtrs(8, nullptr);
    for (int i = 0; i < 8; ++i) {
        BYTE* cb = candBlds.data() + (size_t)i * BUILDING_STRUCT_BYTES;
        *(int32_t*)cb = 450 + i;
        *(BYTE**)(cb + BLD_TYPE_OFFSET) = (i == 0) ? btypeDepot.data() : btypeFactory.data();
        BYTE* rn = resNodes.data() + (size_t)i * 0x80u;
        resTargets[i] = cb;
        *(BYTE***)(rn + 0x70) = &resTargets[i];
        resLastPtrs[i] = rn;
        BYTE* rs = resStructs.data() + (size_t)i * 0x48u;
        *(BYTE***)(rs + 0x00) = &resLastPtrs[i];
        *(BYTE***)(rs + 0x08) = &resLastPtrs[i] + 1;
        *(BYTE**)(rs + 0x40) = cb;
        pfResults.push_back(rs);
    }

    TEST_CHECK(GameCalls_HasCallSiteForTest(SITE_DO_FIND_CONNECTIONS_RVA, FN_PF_CTOR_RVA));
    TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(SITE_DO_FIND_CONNECTIONS_RVA), "xWB") == 0);

    std::vector<BYTE> doWest(BUILDING_STRUCT_BYTES, 0), doEast(BUILDING_STRUCT_BYTES, 0), facBld(BUILDING_STRUCT_BYTES, 0);
    *(int*)doWest.data() = 401;
    *(float*)(doWest.data() + OBJ_POS_OFFSET) = -700.0f;
    *(int*)doEast.data() = 402;
    *(float*)(doEast.data() + OBJ_POS_OFFSET) = 700.0f;
    *(int*)facBld.data() = 403;
    std::vector<BYTE*> blds = { doWest.data(), doEast.data(), facBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    std::vector<BYTE> oldLinkEntry(0x18, 0);
    *(BYTE**)(oldLinkEntry.data() + 8) = facBld.data();
    std::vector<BYTE*> oldLinks = { oldLinkEntry.data() };
    WriteMockVectorHeader(doWest.data() + BLD_DO_CONNECTIONS_BEGIN_OFF, oldLinks);

    std::vector<BYTE> roadNode(0x80, 0);
    std::vector<BYTE> vehEntry(BLD_DO_VEH_ENTRY_STRIDE, 0);
    *(int32_t*)(vehEntry.data() + 0) = 3;
    *(BYTE**)(vehEntry.data() + 0x30) = roadNode.data();
    *(BYTE**)(doWest.data() + BLD_DO_VEH_ENTRIES_BEGIN_OFF) = vehEntry.data();
    *(BYTE**)(doWest.data() + BLD_DO_VEH_ENTRIES_END_OFF) = vehEntry.data() + vehEntry.size();

    // 1. SP pass-through invokes native PF ctor without skipping retAddr
    alignas(16) BYTE spPf[0x400] = {};
    unsigned long long ret = 0;
    const unsigned long long doWestArgs[7] = {
        (unsigned long long)(uintptr_t)spPf, (unsigned long long)(uintptr_t)world,
        (unsigned long long)(uintptr_t)doWest.data(), 0, 0, 0, 0
    };
    const unsigned long long doEastArgs[7] = {
        (unsigned long long)(uintptr_t)spPf, (unsigned long long)(uintptr_t)world,
        (unsigned long long)(uintptr_t)doEast.data(), 0, 0, 0, 0
    };
    unsigned long long retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_DO_FIND_CONNECTIONS_RVA, doWestArgs, &ret, &retAddr));
    TEST_CHECK(g_a7PfCtorCalls == 1 && retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_AFTER_RVA));

    // 2. TwoStates, WaitingLobby & Capture/Replay
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    Commands_SetSession(COMMANDS_HOST);

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_DO_FIND_CONNECTIONS_RVA, doEastArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_SKIP_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_DO_FIND_CONNECTIONS_RVA, doWestArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_SKIP_RVA));
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;

    retAddr = (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_AFTER_RVA);
    TEST_CHECK(GameCalls_RunSiteWithRetAddrForTest(SITE_DO_FIND_CONNECTIONS_RVA, doWestArgs, &ret, &retAddr));
    TEST_CHECK(retAddr == (unsigned long long)(uintptr_t)(g_mockExeBuffer + SITE_DO_FIND_CONNECTIONS_SKIP_RVA));
    TEST_CHECK(g_a7DoRemoveLinkCalls == 0);

    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(g_a7DoRemoveLinkCalls == 1 && g_a7DoRemoveLastDo == doWest.data() && g_a7DoRemoveLastTarget == facBld.data());
    TEST_CHECK(g_a7PfCtorCalls == 2 && g_a7PfAddStartCalls == 1 && g_a7PfRunCalls == 1 && g_a7PfDtorCalls == 1);
    TEST_CHECK(g_a7PfSeenB4 == 4 && g_a7PfSeenB0 == 2 && fabsf(g_a7PfSeen2C - 30.0f) < 1e-4f);
    TEST_CHECK(g_a7PfAddLastNode == (unsigned long long)(uintptr_t)roadNode.data() &&
               fabsf(g_a7PfAddLastArg5 - (-1.0f)) < 1e-4f);
    // Depot [0] excluded and attachment capped at 6 connections ([1..6] out of 7 valid factories)
    TEST_CHECK(doVecStorage.size() == 6);
    TEST_CHECK(doWest[BLD_DO_HAS_CONN_FLAG_OFF] == 1);
    TEST_CHECK(fabsf(*(float*)(doWest.data() + BLD_DO_TIMER_OFF) - 3.0f) < 1e-4f);
    for (size_t i = 0; i < 6; ++i) {
        BYTE* entry = doVecStorage[i];
        TEST_CHECK(*(BYTE**)(entry + 0x08) == candBlds.data() + (i + 1) * BUILDING_STRUCT_BYTES);
        BYTE* cfg = *(BYTE**)(entry + 0x10);
        TEST_CHECK(cfg != nullptr && cfg[0x00] == 1 && cfg[0xC8] == 1 && cfg[0x28] == 1 && cfg[0xF0] == 1);
        TEST_CHECK(*(uint64_t*)(cfg + 0x10) == 0x11112222ULL && *(uint64_t*)(cfg + 0xD8) == 0x33334444ULL);
    }

    g_a7PfMockResultsPtr = nullptr;
    g_a7DoVecStoragePtr = nullptr;
    g_a7OpNewArenaPtr = nullptr;
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_A7_Bug6_CopyWeightsTool()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    g_a7CopyToolCalls = 0;
    g_a7CopyHoverDst = nullptr;
    BuildingSwitches_SetCopyWeightsOverrideForTest(&MockA7CopyWeightsTool);

    std::vector<BYTE> btype5(0x440, 0), btype9(0x440, 0);
    *(int32_t*)(btype5.data() + 0x360) = 5;
    *(int32_t*)(btype9.data() + 0x360) = 9;

    std::vector<BYTE> srcBld(BUILDING_STRUCT_BYTES, 0), dstWest(BUILDING_STRUCT_BYTES, 0),
                      dstEast(BUILDING_STRUCT_BYTES, 0), dstPlanned(BUILDING_STRUCT_BYTES, 0),
                      dstWrongCat(BUILDING_STRUCT_BYTES, 0);
    auto initBld = [](BYTE* b, int id, float x, BYTE* btype, BYTE planned, float built) {
        *(int32_t*)b = id;
        *(float*)(b + OBJ_POS_OFFSET) = x;
        *(BYTE**)(b + 0x318) = btype;
        b[BLD_IS_PLANNED_OFF] = planned;
        *(float*)(b + BLD_BUILT_PROGRESS_OFF) = built;
    };
    initBld(srcBld.data(), 601, -400.0f, btype5.data(), 0, 1.0f);
    initBld(dstWest.data(), 602, -350.0f, btype5.data(), 0, 1.0f);
    initBld(dstEast.data(), 603, 450.0f, btype5.data(), 0, 1.0f);
    initBld(dstPlanned.data(), 604, -360.0f, btype5.data(), 1, 1.0f);
    initBld(dstWrongCat.data(), 605, -370.0f, btype9.data(), 0, 1.0f);

    // Put a pointer-like 64-bit value across weights [0..1] to verify IsNonPointerBuildingWord allows it
    *(uint64_t*)(srcBld.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET) = 0x00007FF812345678ULL;
    for (unsigned i = 2; i < 9; ++i) {
        *(uint32_t*)(srcBld.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET + i * 4u) = 100u + i;
    }

    std::vector<BYTE*> blds = { srcBld.data(), dstWest.data(), dstEast.data(), dstPlanned.data(), dstWrongCat.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);
    *(BYTE**)(world + WORLD_COPY_TOOL_SRC_BLD_OFF) = srcBld.data();

    // 1. SP path mutates directly without lockstep command
    g_a7CopyHoverDst = dstPlanned.data();
    g_mockExeBuffer[0xA54E91] = 1;
    TEST_CHECK(BuildingSwitches_CopyWeightsTool((unsigned long long)(uintptr_t)world, 0) == 1);
    TEST_CHECK(*(uint32_t*)(dstPlanned.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET + 8u) == 102u);
    memset(dstPlanned.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET, 0, BLD_COPY_WEIGHTS_END_OFFSET - BLD_COPY_WEIGHTS_BEGIN_OFFSET);

    // 2. Waiting lobby block clears PRESS_RVA temporarily and notifies without mutating or submitting
    Commands_SetSession(COMMANDS_HOST);
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    g_a7CopyHoverDst = dstWest.data();
    g_mockExeBuffer[0xA54E91] = 1;
    TEST_CHECK(BuildingSwitches_CopyWeightsTool((unsigned long long)(uintptr_t)world, 0) == 1);
    TEST_CHECK(g_mockExeBuffer[0xA54E91] == 1);
    TEST_CHECK(*(uint64_t*)(dstWest.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET) == 0ULL);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;

    // 3. Hover without press (PRESS_RVA == 0) submits nothing
    g_mockExeBuffer[0xA54E91] = 0;
    TEST_CHECK(BuildingSwitches_CopyWeightsTool((unsigned long long)(uintptr_t)world, 0) == 1);
    TEST_CHECK(*(uint64_t*)(dstWest.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET) == 0ULL);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // 4. Null dst restores prevHover; invalid planned/wrongCat/foreign targets submit nothing
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    g_mockExeBuffer[0xA54E91] = 1;
    *(BYTE**)(world + WORLD_HOVERED_BLD_OFF) = srcBld.data();
    g_a7CopyHoverDst = nullptr;
    TEST_CHECK(BuildingSwitches_CopyWeightsTool((unsigned long long)(uintptr_t)world, 0) == 1);
    TEST_CHECK(*(BYTE**)(world + WORLD_HOVERED_BLD_OFF) == srcBld.data());

    BYTE* const kRejected[3] = { dstPlanned.data(), dstWrongCat.data(), dstEast.data() };
    for (BYTE* badDst : kRejected) {
        g_a7CopyHoverDst = badDst;
        BuildingSwitches_CopyWeightsTool((unsigned long long)(uintptr_t)world, 0);
        TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));
    }

    // 5. Valid click on dstWest defers local mutation & replays all 9 dwords at Commands_Boundary
    g_a7CopyHoverDst = dstWest.data();
    TEST_CHECK(BuildingSwitches_CopyWeightsTool((unsigned long long)(uintptr_t)world, 0) == 1);
    TEST_CHECK(*(uint64_t*)(dstWest.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET) == 0ULL);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));

    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(uint64_t*)(dstWest.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET) == 0x00007FF812345678ULL);
    for (unsigned i = 2; i < 9; ++i) {
        TEST_CHECK(*(uint32_t*)(dstWest.data() + BLD_COPY_WEIGHTS_BEGIN_OFFSET + i * 4u) == 100u + i);
    }

    BuildingSwitches_SetCopyWeightsOverrideForTest(nullptr);
    g_mockExeBuffer[0xA54E91] = 0;
    Commands_SetSession(COMMANDS_OFF);
    BuildingSwitches_Reset();
    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_A7_Bug7_GameRulesRebuildStorages()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    GameCalls_ResetSessionState();
    Commands_SetSession(COMMANDS_OFF);
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    g_a7RebuildStoragesCalls = 0;
    g_a7RebuildStoragesSeenBlds.clear();
    WriteAbsJmp12(g_mockExeBuffer + FN_REBUILD_BLD_STORAGES_RVA, (void*)&MockA7RebuildBldStorages);
    Commands_SetRebuildBldStoragesOverrideForTest(&MockA7RebuildBldStorages);

    const unsigned kRebuildSites[3] = {
        SITE_GAME_RULES_REBUILD_1_RVA, SITE_GAME_RULES_REBUILD_2_RVA, SITE_GAME_RULES_REBUILD_3_RVA
    };
    for (unsigned site : kRebuildSites) {
        TEST_CHECK(GameCalls_HasCallSiteForTest(site, FN_REBUILD_BLD_STORAGES_RVA));
        TEST_CHECK(strcmp(GameCalls_GetSiteSchemaForTest(site), "WB") == 0);
    }

    std::vector<BYTE> plannedBld(BUILDING_STRUCT_BYTES, 0), builtBld(BUILDING_STRUCT_BYTES, 0);
    plannedBld[BLD_IS_PLANNED_OFF] = 1;
    builtBld[BLD_IS_PLANNED_OFF] = 0;
    std::vector<BYTE*> blds = { plannedBld.data(), builtBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);

    // 1. SP path invokes native FN_REBUILD_BLD_STORAGES_RVA
    unsigned long long ret = 0;
    const unsigned long long rebuildArgs[7] = {
        (unsigned long long)(uintptr_t)world, (unsigned long long)(uintptr_t)plannedBld.data(), 0, 0, 0, 0, 0
    };
    TEST_CHECK(GameCalls_RunSiteForTest(SITE_GAME_RULES_REBUILD_1_RVA, rebuildArgs, &ret));
    TEST_CHECK(g_a7RebuildStoragesCalls == 1);

    // 2. Lockstep UI & WaitingLobby no-op all 3 sites without calling 0x1A11E0 or queueing commands
    Commands_SetSession(COMMANDS_HOST);
    for (unsigned site : kRebuildSites) {
        TEST_CHECK(GameCalls_RunSiteForTest(site, rebuildArgs, &ret));
    }
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    for (unsigned site : kRebuildSites) {
        TEST_CHECK(GameCalls_RunSiteForTest(site, rebuildArgs, &ret));
    }
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    *(__int64*)(g_mockExeBuffer + 0x9E46B0) = 0;
    TEST_CHECK(g_a7RebuildStoragesCalls == 1);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // 3. ApplyGameRules invokes 0x1A11E0 ONLY on planned buildings when old5B8 != newC848 && newC848 > 1
    BYTE rulesCopy[16 * 4] = {};
    *(int32_t*)(world + WORLD_RULE_COMPLEX_STORAGES_OFF) = 1;
    *(int32_t*)(rulesCopy + 2 * 4) = 2; // kGameRules[2] maps 0xC848 -> 0x5B8
    g_a7RebuildStoragesSeenBlds.clear();
    TEST_CHECK(Commands_ApplyGameRulesForTest(rulesCopy, sizeof(rulesCopy)));
    TEST_CHECK(g_a7RebuildStoragesCalls == 2);
    TEST_CHECK(g_a7RebuildStoragesSeenBlds.size() == 1 && g_a7RebuildStoragesSeenBlds[0] == plannedBld.data());

    // Equal or <= 1 copy does not trigger rebuild
    *(int32_t*)(world + WORLD_RULE_COMPLEX_STORAGES_OFF) = 2;
    *(int32_t*)(rulesCopy + 2 * 4) = 2;
    TEST_CHECK(Commands_ApplyGameRulesForTest(rulesCopy, sizeof(rulesCopy)));
    TEST_CHECK(g_a7RebuildStoragesCalls == 2);

    *(int32_t*)(world + WORLD_RULE_COMPLEX_STORAGES_OFF) = 0;
    *(int32_t*)(rulesCopy + 2 * 4) = 1;
    TEST_CHECK(Commands_ApplyGameRulesForTest(rulesCopy, sizeof(rulesCopy)));
    TEST_CHECK(g_a7RebuildStoragesCalls == 2);

    Commands_SetRebuildBldStoragesOverrideForTest(nullptr);
    Commands_SetSession(COMMANDS_OFF);
    return true;
}

bool Test_Audit7Fixes_M1()
{
    printf("    [Audit7-1: RVA_RESEARCH_RECOMPUTE Prologue, Fallback Clear & SP Isolation] ...\n"); fflush(stdout);
    if (!SubTest_A7_Bug1_ResearchRecomputeAndSpIsolation()) return false;

    printf("    [Audit7-2: Discard Cargo, Empty Storage & Empty Parked Vehicles] ...\n"); fflush(stdout);
    if (!SubTest_A7_Bug2_DiscardAndEmptySites()) return false;

    printf("    [Audit7-3/4: Repair Queue Cancel & Crime/Emergency Cancel Assignment] ...\n"); fflush(stdout);
    if (!SubTest_A7_Bug3_Bug4_RepairAndCrimeCancel()) return false;

    printf("    [Audit7-5: Distribution Office Find Connections] ...\n"); fflush(stdout);
    if (!SubTest_A7_Bug5_DoFindConnections()) return false;

    printf("    [Audit7-6: building_copyweights Late Tool] ...\n"); fflush(stdout);
    if (!SubTest_A7_Bug6_CopyWeightsTool()) return false;

    printf("    [Audit7-7: Pre-0x3370F0 Building Storage Rebuild & ApplyGameRules] ...\n"); fflush(stdout);
    if (!SubTest_A7_Bug7_GameRulesRebuildStorages()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 60: Hotfix v1.7.1 (included with external-linkage forwarding wrapper)
// ---------------------------------------------------------------------------

#define Test_HotfixV171_All Test_HotfixV171_All_Impl
#include "hotfix_v171_tests.cpp"
#undef Test_HotfixV171_All

bool Test_HotfixV171_All()
{
    return Test_HotfixV171_All_Impl();
}

// ---------------------------------------------------------------------------
// Test 61: RCO & Rail Construction Fixes (included with external-linkage forwarding wrapper)
// ---------------------------------------------------------------------------

#define Test_RcoRailConstructionFixes_All Test_RcoRailConstructionFixes_All_Impl
#include "rco_rail_construction_tests.cpp"
#undef Test_RcoRailConstructionFixes_All

bool Test_RcoRailConstructionFixes_All()
{
    return Test_RcoRailConstructionFixes_All_Impl();
}
