// SPDX-License-Identifier: GPL-3.0-only
// Regression tests for Rail Construction Office (RCO) & Rail Construction Fixes (Bugs #1-#6):
//   1. H_SiteStop (0x160579) & H_SiteResume (0x161440) UI caller filter + unconditional +0x879 = 1
//   2. 0x161CB0 6th param triggerImmediateScan vs real demolition detection (cat == 0x6A || +0xEA8 != 0)
//   3. ConstructStep rail next-segment auto-chain (0x159F4D) Two-States ownership enforcement
//   4. RCO/CO Queue Reorder popup (0x7707E0) permutation & Lockstep LS_CMD_OFFICE_REORDER round-trip
//   5. Read-only per-frame predicate 0x7D8738 removed; Assign CO rail sites preserved

#include <vector>
#include <string.h>

static bool RcoTest_SiteStopAndResumeUiCallerFilter()
{
    printf("    [RCO-1] H_SiteStop / H_SiteResume UI Caller Filter & Unconditional +0x879 = 1 ...\n");
    fflush(stdout);

    ResetMockWorldMemory();

    // 1. Verify exact H_SiteStop UI caller whitelist (ONLY 0x6FA0E7)
    TEST_CHECK(SettingsSync_IsSiteStopUiCallerRva(RVA_SITE_STOP_UI_RET));
    TEST_CHECK(SettingsSync_IsSiteStopUiCallerRva(0x6FA0E7u));
    TEST_CHECK(!SettingsSync_IsSiteStopUiCallerRva(0u));
    const unsigned kInternalStopCallers[] = {
        0x21C865u, 0x21CAF3u, 0x3BD7D7u, 0x3BD8D8u, 0x3BE8E5u, 0x3BE957u, 0x6FA0E2u,
    };
    for (unsigned rva : kInternalStopCallers) {
        TEST_CHECK(!SettingsSync_IsSiteStopUiCallerRva(rva));
    }

    // 2. Verify exact H_SiteResume UI caller whitelist (5 UI callers ONLY)
    const unsigned kUiResumeCallers[] = {
        RVA_SITE_RESUME_UI_RET_SUSPEND,
        RVA_SITE_RESUME_UI_RET_PAY_RUB,
        RVA_SITE_RESUME_UI_RET_PAY_USD,
        RVA_SITE_RESUME_UI_RET_WIN2_RUB,
        RVA_SITE_RESUME_UI_RET_WIN2_USD,
    };
    for (unsigned rva : kUiResumeCallers) {
        TEST_CHECK(SettingsSync_IsSiteResumeUiCallerRva(rva));
    }
    const unsigned kInternalResumeCallers[] = {
        0u, 0x138237u, 0x140160u, 0x15C126u, 0x21C8FFu, 0x3BD7C4u, 0x540CE3u, 0x543C07u, 0x6F9E6Du,
    };
    for (unsigned rva : kInternalResumeCallers) {
        TEST_CHECK(!SettingsSync_IsSiteResumeUiCallerRva(rva));
    }

    // 3. Test H_SiteStop capture behavior:
    //    Even when 3rd arg (autoAssignOffices / stopVal) == 0, UI click (0x6FA0E7) MUST write 1u to +0x879!
    std::vector<BYTE> site(BUILDING_STRUCT_BYTES, 0);
    site[BLD_STOPPED_OFFSET] = 0;
    TEST_CHECK(SettingsSync_TestSiteCallFilter(site.data(), true, 0, true, RVA_SITE_STOP_UI_RET));
    TEST_CHECK(site[BLD_STOPPED_OFFSET] == 1u);

    // Internal caller (e.g. network edit 0x3BD7D7) must NOT be intercepted and must NOT clobber +0x879
    site[BLD_STOPPED_OFFSET] = 0;
    TEST_CHECK(!SettingsSync_TestSiteCallFilter(site.data(), true, 0, true, 0x3BD7D7u));
    TEST_CHECK(site[BLD_STOPPED_OFFSET] == 0u);

    // When captureUi is false (Lockstep replay / inactive session), even UI caller passes through natively
    site[BLD_STOPPED_OFFSET] = 0;
    TEST_CHECK(!SettingsSync_TestSiteCallFilter(site.data(), true, 1, false, RVA_SITE_STOP_UI_RET));
    TEST_CHECK(site[BLD_STOPPED_OFFSET] == 0u);

    // 4. Test H_SiteResume capture behavior:
    //    UI caller clears +0x879 -> 0; simulation caller (0x15C126) is NOT intercepted and preserves +0x879 == 1
    site[BLD_STOPPED_OFFSET] = 1u;
    TEST_CHECK(!SettingsSync_TestSiteCallFilter(site.data(), false, 0, true, 0x15C126u));
    TEST_CHECK(site[BLD_STOPPED_OFFSET] == 1u);

    TEST_CHECK(SettingsSync_TestSiteCallFilter(site.data(), false, 0, true, RVA_SITE_RESUME_UI_RET_PAY_RUB));
    TEST_CHECK(site[BLD_STOPPED_OFFSET] == 0u);

    return true;
}

static bool RcoTest_Arg6SemanticsAndRailChainGuard()
{
    printf("    [RCO-2] 0x161CB0 Arg6 triggerImmediateScan & Rail Auto-Chain (0x159F4D) Guard ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> officeWest(BUILDING_STRUCT_BYTES, 0), officeEast(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> siteWest(BUILDING_STRUCT_BYTES, 0), siteEast(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> rcoType(0x400, 0), demolType(0x400, 0);
    *(int*)(rcoType.data() + BLD_TYPE_CAT_OFF) = (int)BLD_CAT_RAIL_CO;
    *(int*)(demolType.data() + BLD_TYPE_CAT_OFF) = (int)BLD_CAT_DEMOLISH_CO;

    TEST_CHECK(BLD_CAT_RAIL_CO == 0x1Cu);
    TEST_CHECK(BLD_CAT_DEMOLISH_CO == 0x6Au);

    *(int*)officeWest.data() = 801;
    *(float*)(officeWest.data() + OBJ_POS_OFFSET) = -900.0f;
    *(uintptr_t*)(officeWest.data() + BLD_TYPE_OFFSET) = (uintptr_t)rcoType.data();

    *(int*)officeEast.data() = 802;
    *(float*)(officeEast.data() + OBJ_POS_OFFSET) = 900.0f;
    *(uintptr_t*)(officeEast.data() + BLD_TYPE_OFFSET) = (uintptr_t)rcoType.data();

    *(int*)siteWest.data() = 803;
    *(float*)(siteWest.data() + OBJ_POS_OFFSET) = -600.0f;

    *(int*)siteEast.data() = 804;
    *(float*)(siteEast.data() + OBJ_POS_OFFSET) = 600.0f;

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // 1. Regular RCO + realistic mode (triggerImmediateScan == 1) on construction site: NOT demolition, mutual aid ALLOWED
    TEST_CHECK(!TwoStates_IsDemolitionOfficeOrSite(officeWest.data(), siteEast.data()));
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 1));
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 1));

    // 2. Demolition office (cat == 0x6A) even with arg6 == 0: detected as demolition, cross-SSR BLOCKED
    *(uintptr_t*)(officeWest.data() + BLD_TYPE_OFFSET) = (uintptr_t)demolType.data();
    TEST_CHECK(TwoStates_IsDemolitionOfficeOrSite(officeWest.data(), siteEast.data()));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 0));
    // Same-SSR demolition still allowed
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteWest.data(), 3000.0f, 0, 0));
    *(uintptr_t*)(officeWest.data() + BLD_TYPE_OFFSET) = (uintptr_t)rcoType.data();

    // 3. Demolition site (+0xEA8 != 0) targeted by non-demolition office with arg6 == 0: cross-SSR BLOCKED
    siteEast[BLD_IS_DEMOLITION_SITE_OFFSET] = 1;
    TEST_CHECK(TwoStates_IsDemolitionOfficeOrSite(officeWest.data(), siteEast.data()));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 0));
    siteEast[BLD_IS_DEMOLITION_SITE_OFFSET] = 0;

    // 4. ConstructStep rail next-segment auto-chain (0x159F4D, checkFilters == 0): must enforce same-SSR ownership!
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteWest.data(), 0.0f, 0, 0, RVA_CALLER_RAIL_CHAIN_ASSIGN));
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeEast.data(), siteEast.data(), 0.0f, 0, 0, RVA_CALLER_RAIL_CHAIN_ASSIGN));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 0.0f, 0, 0, RVA_CALLER_RAIL_CHAIN_ASSIGN));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeEast.data(), siteWest.data(), 0.0f, 0, 0, RVA_CALLER_RAIL_CHAIN_ASSIGN));

    TwoStates_OnSessionEnded();
    return true;
}

static bool RcoTest_QueueReorderPermutationAndCommand()
{
    printf("    [RCO-3] RCO/CO Queue Reorder (0x7707E0) Permutation & Lockstep Round-Trip ...\n");
    fflush(stdout);

    // 1. Pure permutation unit tests
    {
        unsigned long long sites[4] = { 1001ULL, 1002ULL, 1003ULL, 1004ULL };
        float dists[4] = { 10.0f, 20.0f, 30.0f, 40.0f };
        const unsigned short reverseOrder[4] = { 3, 1, 2, 0 };
        TEST_CHECK(GameCalls_ApplyOfficeQueuePermutation(sites, dists, 4, reverseOrder, 4));
        TEST_CHECK(sites[0] == 1004ULL && sites[1] == 1002ULL && sites[2] == 1003ULL && sites[3] == 1001ULL);
        TEST_CHECK_NEAR(dists[0], 40.0f, 1e-4);
        TEST_CHECK_NEAR(dists[1], 20.0f, 1e-4);
        TEST_CHECK_NEAR(dists[2], 30.0f, 1e-4);
        TEST_CHECK_NEAR(dists[3], 10.0f, 1e-4);
    }

    // Duplicate/out-of-range fallback bijection check (never lose or duplicate pointers)
    {
        unsigned long long sites[3] = { 2001ULL, 2002ULL, 2003ULL };
        float dists[3] = { 1.0f, 2.0f, 3.0f };
        const unsigned short badOrder[3] = { 2, 2, 9 };
        TEST_CHECK(GameCalls_ApplyOfficeQueuePermutation(sites, dists, 3, badOrder, 3));
        TEST_CHECK(sites[0] == 2003ULL && sites[1] == 2001ULL && sites[2] == 2002ULL);
        TEST_CHECK_NEAR(dists[0], 3.0f, 1e-4);
        TEST_CHECK_NEAR(dists[1], 1.0f, 1e-4);
        TEST_CHECK_NEAR(dists[2], 2.0f, 1e-4);
    }

    // Invalid counts rejected cleanly without corrupting input
    {
        unsigned long long sites[2] = { 11ULL, 22ULL };
        float dists[2] = { 1.5f, 2.5f };
        const unsigned short order[2] = { 1, 0 };
        TEST_CHECK(!GameCalls_ApplyOfficeQueuePermutation(sites, dists, 0, order, 0));
        TEST_CHECK(!GameCalls_ApplyOfficeQueuePermutation(sites, dists, 2, order, 1));
        TEST_CHECK(!GameCalls_ApplyOfficeQueuePermutation(sites, dists, 257, order, 257));
        TEST_CHECK(sites[0] == 11ULL && sites[1] == 22ULL);
    }

    // 2. End-to-end mock world capture -> Lockstep command apply round-trip
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    GameCalls_Bind(&g_mockHost);
    WorldRefs_Bind(&g_mockHost);

    std::vector<BYTE> office(BUILDING_STRUCT_BYTES, 0);
    *(int*)office.data() = 910;
    *(float*)(office.data() + OBJ_POS_OFFSET) = -400.0f;

    std::vector<U64> bldVecStorage(1, (U64)(uintptr_t)office.data());
    *(U64*)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = (U64)(uintptr_t)bldVecStorage.data();
    *(U64*)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA) = (U64)(uintptr_t)(bldVecStorage.data() + bldVecStorage.size());

    alignas(8) U64 assignedSites[4] = { 0xAAA10000ULL, 0xAAA20000ULL, 0xAAA30000ULL, 0xAAA40000ULL };
    alignas(8) float assignedDists[4] = { 110.5f, 220.5f, 330.5f, 440.5f };
    *(U64*)(office.data() + BLD_ASSIGNED_SITES_BEGIN_OFF) = (U64)(uintptr_t)assignedSites;
    *(U64*)(office.data() + BLD_ASSIGNED_SITES_END_OFF) = (U64)(uintptr_t)(assignedSites + 4);
    *(U64*)(office.data() + BLD_ASSIGNED_DISTS_BEGIN_OFF) = (U64)(uintptr_t)assignedDists;
    *(U64*)(office.data() + BLD_ASSIGNED_DISTS_END_OFF) = (U64)(uintptr_t)(assignedDists + 4);

    // 24-byte popup items vector with new order [2, 0, 3, 1] encoded as floats at +0x10
    alignas(8) BYTE itemsBuf[4 * OFFICE_REORDER_ITEM_STRIDE] = {};
    const float newOrderFloats[4] = { 2.0f, 0.0f, 3.0f, 1.0f };
    for (size_t i = 0; i < 4; ++i) {
        memcpy(itemsBuf + i * OFFICE_REORDER_ITEM_STRIDE + OFFICE_REORDER_ITEM_IDX_OFF, &newOrderFloats[i], sizeof(float));
    }
    alignas(8) U64 itemsVecHdr[3] = {
        (U64)(uintptr_t)itemsBuf,
        (U64)(uintptr_t)(itemsBuf + sizeof(itemsBuf)),
        (U64)(uintptr_t)(itemsBuf + sizeof(itemsBuf)),
    };

    BYTE payload[1024] = {};
    size_t payloadBytes = 0;
    TEST_CHECK(GameCalls_BuildOfficeReorderPayloadForTest(
        (const BYTE*)itemsVecHdr, office.data(), payload, sizeof(payload), &payloadBytes));
    TEST_CHECK(payloadBytes == sizeof(RouteRef) + sizeof(unsigned short) + 4u * sizeof(unsigned short));

    TEST_CHECK(GameCalls_ApplyOfficeReorderCommand(payload, payloadBytes));
    TEST_CHECK(assignedSites[0] == 0xAAA30000ULL);
    TEST_CHECK(assignedSites[1] == 0xAAA10000ULL);
    TEST_CHECK(assignedSites[2] == 0xAAA40000ULL);
    TEST_CHECK(assignedSites[3] == 0xAAA20000ULL);
    TEST_CHECK_NEAR(assignedDists[0], 330.5f, 1e-4);
    TEST_CHECK_NEAR(assignedDists[1], 110.5f, 1e-4);
    TEST_CHECK_NEAR(assignedDists[2], 440.5f, 1e-4);
    TEST_CHECK_NEAR(assignedDists[3], 220.5f, 1e-4);

    // Identity permutation returns false from BuildOfficeReorderPayloadForTest (no redundant Lockstep send)
    for (size_t i = 0; i < 4; ++i) {
        float id = (float)i;
        memcpy(itemsBuf + i * OFFICE_REORDER_ITEM_STRIDE + OFFICE_REORDER_ITEM_IDX_OFF, &id, sizeof(float));
    }
    TEST_CHECK(!GameCalls_BuildOfficeReorderPayloadForTest(
        (const BYTE*)itemsVecHdr, office.data(), payload, sizeof(payload), &payloadBytes));

    // Mismatched site vector count safely no-ops without corrupting assignedSites
    for (size_t i = 0; i < 4; ++i) {
        memcpy(itemsBuf + i * OFFICE_REORDER_ITEM_STRIDE + OFFICE_REORDER_ITEM_IDX_OFF, &newOrderFloats[i], sizeof(float));
    }
    TEST_CHECK(GameCalls_BuildOfficeReorderPayloadForTest(
        (const BYTE*)itemsVecHdr, office.data(), payload, sizeof(payload), &payloadBytes));
    *(U64*)(office.data() + BLD_ASSIGNED_SITES_END_OFF) = (U64)(uintptr_t)(assignedSites + 3);
    *(U64*)(office.data() + BLD_ASSIGNED_DISTS_END_OFF) = (U64)(uintptr_t)(assignedDists + 3);
    TEST_CHECK(GameCalls_ApplyOfficeReorderCommand(payload, payloadBytes));
    TEST_CHECK(assignedSites[0] == 0xAAA30000ULL && assignedSites[1] == 0xAAA10000ULL && assignedSites[2] == 0xAAA40000ULL);

    return true;
}

static bool RcoTest_GameCallsSitesClean()
{
    printf("    [RCO-4] 0x7D8738 Removed & Rail Assign CO Tool Sites Preserved ...\n");
    fflush(stdout);

    TEST_CHECK(!GameCalls_HasCallSiteForTest(0x7D8738u, 0x664940u));

    const unsigned kRailAssignSites[][2] = {
        { SITE_ASSIGN_CO_SINGLE_RAIL_START_RVA, RVA_START_PLANNED_CONSTRUCTION },
        { SITE_ASSIGN_CO_SINGLE_RAIL_ASSIGN_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE },
        { SITE_ASSIGN_CO_RECT_RAIL_START_RVA, RVA_START_PLANNED_CONSTRUCTION },
        { SITE_ASSIGN_CO_RECT_RAIL_ASSIGN_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE },
    };
    const char* const kSchemas[4] = { "WObx", "WOOFbb", "WObx", "WOOFbb" };
    for (size_t i = 0; i < 4; ++i) {
        TEST_CHECK(GameCalls_HasCallSiteForTest(kRailAssignSites[i][0], kRailAssignSites[i][1]));
        const char* schema = GameCalls_GetSiteSchemaForTest(kRailAssignSites[i][0]);
        TEST_CHECK(schema != nullptr && strcmp(schema, kSchemas[i]) == 0);
    }
    return true;
}

static bool Test_RcoRailConstructionFixes_All()
{
    return RcoTest_SiteStopAndResumeUiCallerFilter() &&
           RcoTest_Arg6SemanticsAndRailChainGuard() &&
           RcoTest_QueueReorderPermutationAndCommand() &&
           RcoTest_GameCallsSitesClean();
}
