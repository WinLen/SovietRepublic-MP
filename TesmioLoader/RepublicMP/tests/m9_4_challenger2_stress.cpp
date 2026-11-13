// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone M9.4
// Challenger 2: Adversarial Stress Testing of Features 39 & 40:
//   1. Anti-Exploit Security on Research Switching (Feature 40)
//   2. Border Strip Demolition Permissions & Zone Boundaries (Feature 40)
//   3. Customs Tourist Spawn Edge Cases, Rating Indices & Dead Counter Isolation (Feature 40)
//   4. Strict Per-State Research & License Isolation (Zero OR-merging) (Feature 40)
//   5. Legacy Non-Lockstep Mode Elimination Audit (Feature 39)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <limits>
#include <algorithm>

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 1: Anti-Exploit Security on Research Switching
// ---------------------------------------------------------------------------

static bool SubTest_M9_4_C2_ResearchSwitchAntiExploit()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const int kTechCount = 6;
    std::vector<BYTE> resBuf(kTechCount * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < kTechCount; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
        *(float*)(it + RESEARCH_ITEM_PROGRESS_OFFSET)   = 0.0f;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();
    memset(world + WORLD_RESEARCH_UNLOCKS_OFFSET, 0, WORLD_RESEARCH_UNLOCKS_BYTES);

    std::vector<BYTE> univWest(0x1420, 0);
    *(int*)univWest.data() = 701;
    *(float*)(univWest.data() + OBJ_POS_OFFSET) = -800.0f;
    *(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    std::vector<BYTE> univWest2(0x1420, 0);
    *(int*)univWest2.data() = 702;
    *(float*)(univWest2.data() + OBJ_POS_OFFSET) = -1200.0f;
    *(float*)(univWest2.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    std::vector<BYTE*> bldVec = { univWest.data(), univWest2.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // 1. Threshold stress testing: switch away at multiple progress levels
    const float testThresholds[] = { 0.0001f, 0.05f, 0.25f, 0.50f, 0.75f, 0.95f, 0.985f, 0.999f };
    const int numThresholds = sizeof(testThresholds) / sizeof(testThresholds[0]);

    for (int t = 0; t < numThresholds; ++t) {
        float expectedProg = testThresholds[t];

        // Attach univWest to tech 0 with accumulated expectedProg
        BYTE* it0 = resBuf.data() + 0 * RESEARCH_ITEM_STRIDE;
        *(float*)(it0 + RESEARCH_ITEM_PROGRESS_OFFSET) = expectedProg;
        *(BYTE**)(it0 + RESEARCH_ITEM_BLD_PTR_OFFSET)  = univWest.data();
        it0[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        world[WORLD_RESEARCH_UNLOCKS_OFFSET + 0] = 0;
        TwoStates_ResetResearch();
        TwoStates_OnWorldLoaded();

        TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), expectedProg, 1e-4f);
        TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == 0);

        // Switch to tech 1
        TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 1));

        // Anti-exploit verification for tech 0:
        // Progress MUST remain exactly what was accumulated, NEVER 1.0f!
        float prog0 = TwoStates_GetEngineResearchProgress(world, 1, 0);
        TEST_CHECK_NEAR(prog0, expectedProg, 1e-4f);
        TEST_CHECK_NEAR(*(float*)(it0 + RESEARCH_ITEM_PROGRESS_OFFSET), expectedProg, 1e-4f);

        // Unlock byte MUST NOT be set!
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 0);
        TEST_CHECK(world[WORLD_RESEARCH_UNLOCKS_OFFSET + 0] == 0);

        // Building pointer MUST be detached from tech 0
        TEST_CHECK(*(BYTE**)(it0 + RESEARCH_ITEM_BLD_PTR_OFFSET) == nullptr);

        // Tech 0 remains available to research
        TEST_CHECK(it0[RESEARCH_ITEM_AVAILABLE_OFFSET] == 1);

        // Tech 1 is now actively attached to univWest
        BYTE* it1 = resBuf.data() + 1 * RESEARCH_ITEM_STRIDE;
        TEST_CHECK(*(BYTE**)(it1 + RESEARCH_ITEM_BLD_PTR_OFFSET) == univWest.data());
        TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == 1);
    }

    // 2. Adversarial rapid-fire switching fuzzing (100 switches between 4 techs)
    for (int i = 0; i < 4; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        *(float*)(it + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.0f;
        *(BYTE**)(it + RESEARCH_ITEM_BLD_PTR_OFFSET)  = nullptr;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        world[WORLD_RESEARCH_UNLOCKS_OFFSET + i] = 0;
    }
    TwoStates_ResetResearch();
    TwoStates_OnWorldLoaded();

    for (int iter = 0; iter < 100; ++iter) {
        int nextTech = iter % 4;
        TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, nextTech));
        TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == nextTech);

        // Advance slightly
        BYTE* it = resBuf.data() + (size_t)nextTech * RESEARCH_ITEM_STRIDE;
        float cur = *(float*)(it + RESEARCH_ITEM_PROGRESS_OFFSET);
        if (cur < 0.90f) {
            *(float*)(it + RESEARCH_ITEM_PROGRESS_OFFSET) = cur + 0.005f;
        }

        // Verify none of the techs have reached 1.0f or unlocked prematurely
        for (int i = 0; i < 4; ++i) {
            BYTE* checkIt = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
            float p = *(float*)(checkIt + RESEARCH_ITEM_PROGRESS_OFFSET);
            TEST_CHECK(p < 1.0f);
            TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, i) == 0);
            TEST_CHECK(world[WORLD_RESEARCH_UNLOCKS_OFFSET + i] == 0);
        }
    }

    // 3. Legitimate completion: step university until tech 0 reaches 1.0f
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 0));
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == 0);

    // Step 1: advances by 0.50f (reqWork = 20, 10 workers)
    TwoStates_BldUnivStepForTest(world, univWest.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), 0.50f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 0);

    // Step 2: advances from 0.50f to 1.0f -> completed & unlocked!
    TwoStates_BldUnivStepForTest(world, univWest.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), 1.0f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 1);
    TEST_CHECK(world[WORLD_RESEARCH_UNLOCKS_OFFSET + 0] == 1);
    BYTE* it0 = resBuf.data() + 0 * RESEARCH_ITEM_STRIDE;
    TEST_CHECK(*(BYTE**)(it0 + RESEARCH_ITEM_BLD_PTR_OFFSET) == nullptr); // detached upon finish
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == -1);

    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 2: Border Strip Demolition Permissions
// ---------------------------------------------------------------------------

static bool SubTest_M9_4_C2_BorderDemolitionPermissions()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    // Border half-width = 250.0m. Neutral strip is X in [-250.0m, +250.0m].
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // Verify zone designations
    TEST_CHECK(TwoStates_GetZoneAt(-500.0f, 0.0f) == 1); // West
    TEST_CHECK(TwoStates_GetZoneAt(500.0f, 0.0f) == 2);  // East
    TEST_CHECK(TwoStates_GetZoneAt(0.0f, 0.0f) == 0);    // Neutral strip
    TEST_CHECK(TwoStates_GetZoneAt(-200.0f, 50.0f) == 0);
    TEST_CHECK(TwoStates_GetZoneAt(200.0f, -50.0f) == 0);
    TEST_CHECK(TwoStates_GetZoneAt(-250.0f, 0.0f) == 0);
    TEST_CHECK(TwoStates_GetZoneAt(250.0f, 0.0f) == 0);

    // 1. Inside neutral border strip: building owner verification
    // Object 1: State 1 owned at (0.0f, 150.0f)
    int key1 = TwoStates_BuildingPosKey(0.0f, 150.0f);
    TwoStates_RegisterBuildingOwner(key1, 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(key1, 0.0f, 150.0f) == 1);

    // Object 2: State 2 owned at (0.0f, -150.0f)
    int key2 = TwoStates_BuildingPosKey(0.0f, -150.0f);
    TwoStates_RegisterBuildingOwner(key2, 2);
    TEST_CHECK(TwoStates_GetBuildingOwner(key2, 0.0f, -150.0f) == 2);

    // Object 3: State 1 owned near East edge of neutral strip (+200.0f, 80.0f)
    int key3 = TwoStates_BuildingPosKey(200.0f, 80.0f);
    TwoStates_RegisterBuildingOwner(key3, 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(key3, 200.0f, 80.0f) == 1);

    // Object 4: State 2 owned near West edge of neutral strip (-200.0f, -80.0f)
    int key4 = TwoStates_BuildingPosKey(-200.0f, -80.0f);
    TwoStates_RegisterBuildingOwner(key4, 2);
    TEST_CHECK(TwoStates_GetBuildingOwner(key4, -200.0f, -80.0f) == 2);

    // State 1 demolition attempts:
    TEST_CHECK(TwoStates_CanDemolishAt(0.0f, 150.0f, 1) == true);   // Owns Object 1 -> ALLOWED
    TEST_CHECK(TwoStates_CanDemolishAt(0.0f, -150.0f, 1) == false);  // Object 2 owned by State 2 -> FORBIDDEN
    TEST_CHECK(TwoStates_CanDemolishAt(200.0f, 80.0f, 1) == true);   // Owns Object 3 -> ALLOWED
    TEST_CHECK(TwoStates_CanDemolishAt(-200.0f, -80.0f, 1) == false);// Object 4 owned by State 2 -> FORBIDDEN

    // State 2 demolition attempts:
    TEST_CHECK(TwoStates_CanDemolishAt(0.0f, 150.0f, 2) == false);  // Object 1 owned by State 1 -> FORBIDDEN
    TEST_CHECK(TwoStates_CanDemolishAt(0.0f, -150.0f, 2) == true);   // Owns Object 2 -> ALLOWED
    TEST_CHECK(TwoStates_CanDemolishAt(200.0f, 80.0f, 2) == false);  // Object 3 owned by State 1 -> FORBIDDEN
    TEST_CHECK(TwoStates_CanDemolishAt(-200.0f, -80.0f, 2) == true);  // Owns Object 4 -> ALLOWED

    // 2. Unregistered/neutral objects in border strip fall back to (x < 0.0f ? 1 : 2)
    TEST_CHECK(TwoStates_CanDemolishAt(-100.0f, 500.0f, 1) == true);
    TEST_CHECK(TwoStates_CanDemolishAt(-100.0f, 500.0f, 2) == false);
    TEST_CHECK(TwoStates_CanDemolishAt(100.0f, 500.0f, 1) == false);
    TEST_CHECK(TwoStates_CanDemolishAt(100.0f, 500.0f, 2) == true);

    // 3. Outside border strip (sovereign territories):
    // West zone 1 (X = -600.0m)
    TEST_CHECK(TwoStates_CanDemolishAt(-600.0f, 0.0f, 1) == true);
    TEST_CHECK(TwoStates_CanDemolishAt(-600.0f, 0.0f, 2) == false);

    // East zone 2 (X = +600.0m)
    TEST_CHECK(TwoStates_CanDemolishAt(600.0f, 0.0f, 1) == false);
    TEST_CHECK(TwoStates_CanDemolishAt(600.0f, 0.0f, 2) == true);

    // 4. TwoStates_CheckDemolishAllowed user-facing wrapper
    // Local player is Host (State 1)
    TEST_CHECK(TwoStates_CheckDemolishAllowed(0.0f, 150.0f, "test demo") == true);
    TEST_CHECK(TwoStates_CheckDemolishAllowed(0.0f, -150.0f, "test demo") == false); // logs refusal toast

    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 3: Customs Tourist Spawn Calculations
// ---------------------------------------------------------------------------

static bool SubTest_M9_4_C2_CustomsTouristSpawnEdgeCases()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // 1. Setup USD customs building
    std::vector<BYTE> bldTypeUsd(0x600, 0);
    *(uint8_t*)(bldTypeUsd.data() + 0x360) = 0x14;
    *(uint8_t*)(bldTypeUsd.data() + 0x528) = 1; // 1 = USD customs

    std::vector<BYTE> customsUsd(0x1420, 0);
    *(float*)(customsUsd.data() + OBJ_POS_OFFSET) = -1000.0f; // West
    *(BYTE**)(customsUsd.data() + 0x318) = bldTypeUsd.data();

    // 2. Setup Soviet customs building
    std::vector<BYTE> bldTypeRub(0x600, 0);
    *(uint8_t*)(bldTypeRub.data() + 0x360) = 0x14;
    *(uint8_t*)(bldTypeRub.data() + 0x528) = 0; // 0 = Soviet customs

    std::vector<BYTE> customsRub(0x1420, 0);
    *(float*)(customsRub.data() + OBJ_POS_OFFSET) = 1000.0f; // East
    *(BYTE**)(customsRub.data() + 0x318) = bldTypeRub.data();

    // Contaminate dead tourist slots (indices 8 and 9) with large integer counts
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 8 * sizeof(int32_t)) = 987654;
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 9 * sizeof(int32_t)) = 123456;

    // Visa 11 (USD) and Visa 12 (Soviet) unlocked
    TwoStates_SetEngineResearchUnlockByteForTest(1, 11, 1);
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 11) = 1;
    TwoStates_SetEngineResearchUnlockByteForTest(2, 12, 1);
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 12) = 1;

    // --- Scenario A: USD Normal (trips=12, ratingSum=54.0f -> avg=4.5f) ---
    // USD uses tripIdx=5 and rateIdx=11
    TwoStates_SetStateEconExtraCountForTest(1, 5, 12);
    TwoStates_SetStateEconExtraFloatForTest(1, 11, 54.0f);
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 5 * sizeof(int32_t)) = 12;
    float sumUsd = 54.0f;
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 11 * sizeof(int32_t), &sumUsd, sizeof(float));

    float avgUsd = 0.0f;
    int32_t spawnUsd = 0;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 4.5f, 1e-3f);
    TEST_CHECK(spawnUsd == 6); // 2 base + 0 mkt + (int)4.5 = 6

    // --- Scenario B: Soviet Normal (trips=8, ratingSum=28.0f -> avg=3.5f) ---
    // Soviet uses tripIdx=4 and rateIdx=10
    TwoStates_SetStateEconExtraCountForTest(2, 4, 8);
    TwoStates_SetStateEconExtraFloatForTest(2, 10, 28.0f);
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 4 * sizeof(int32_t)) = 8;
    float sumRub = 28.0f;
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 10 * sizeof(int32_t), &sumRub, sizeof(float));

    float avgRub = 0.0f;
    int32_t spawnRub = 0;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsRub.data(), &avgRub, &spawnRub);
    TEST_CHECK_NEAR(avgRub, 3.5f, 1e-3f);
    TEST_CHECK(spawnRub == 5); // 2 base + 0 mkt + (int)3.5 = 5

    // Verify dead tourist counters at indices 8 and 9 were not disturbed or used
    TEST_CHECK(*(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 8 * sizeof(int32_t)) == 987654);
    TEST_CHECK(*(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 9 * sizeof(int32_t)) == 123456);

    // --- Scenario C: Marketing Bonus (+2) ---
    // USD marketing is tech 13 (11 + 2)
    TwoStates_SetEngineResearchUnlockByteForTest(1, 13, 1);
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 13) = 1;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 4.5f, 1e-3f);
    TEST_CHECK(spawnUsd == 8); // 2 base + 2 mkt + (int)4.5 = 8

    // Soviet marketing is tech 14 (12 + 2)
    TwoStates_SetEngineResearchUnlockByteForTest(2, 14, 1);
    *(uint8_t*)(world + WORLD_RESEARCH_UNLOCKS_OFFSET + 14) = 1;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsRub.data(), &avgRub, &spawnRub);
    TEST_CHECK_NEAR(avgRub, 3.5f, 1e-3f);
    TEST_CHECK(spawnRub == 7); // 2 base + 2 mkt + (int)3.5 = 7

    // --- Scenario D: Zero Trips (trips = 0) -> defaults to 3.0f ---
    TwoStates_SetStateEconExtraCountForTest(1, 5, 0);
    TwoStates_SetStateEconExtraFloatForTest(1, 11, 0.0f);
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 5 * sizeof(int32_t)) = 0;
    float zeroSum = 0.0f;
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 11 * sizeof(int32_t), &zeroSum, sizeof(float));
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 3.0f, 1e-4f);
    TEST_CHECK(spawnUsd == 7); // 2 base + 2 mkt + 3 avg = 7

    // --- Scenario E: Negative Trips or Negative Rating -> defaults to 3.0f ---
    TwoStates_SetStateEconExtraCountForTest(1, 5, -10);
    TwoStates_SetStateEconExtraFloatForTest(1, 11, 50.0f);
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 5 * sizeof(int32_t)) = -10;
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 3.0f, 1e-4f);

    TwoStates_SetStateEconExtraCountForTest(1, 5, 10);
    TwoStates_SetStateEconExtraFloatForTest(1, 11, -25.0f);
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 5 * sizeof(int32_t)) = 10;
    float negSum = -25.0f;
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 11 * sizeof(int32_t), &negSum, sizeof(float));
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 3.0f, 1e-4f);

    // --- Scenario F: NaN and +Infinity Protection ---
    float nanVal = std::numeric_limits<float>::quiet_NaN();
    TwoStates_SetStateEconExtraCountForTest(1, 5, 10);
    TwoStates_SetStateEconExtraFloatForTest(1, 11, nanVal);
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 5 * sizeof(int32_t)) = 10;
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 11 * sizeof(int32_t), &nanVal, sizeof(float));
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 3.0f, 1e-4f); // Safely defaulted to 3.0f!

    float infVal = std::numeric_limits<float>::infinity();
    TwoStates_SetStateEconExtraFloatForTest(1, 11, infVal);
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 11 * sizeof(int32_t), &infVal, sizeof(float));
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 3.0f, 1e-4f); // Safely defaulted to 3.0f!

    // --- Scenario G: Huge Trips Scale (2,000,000 trips) ---
    TwoStates_SetStateEconExtraCountForTest(1, 5, 2000000);
    TwoStates_SetStateEconExtraFloatForTest(1, 11, 9600000.0f);
    *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 5 * sizeof(int32_t)) = 2000000;
    float hugeSum = 9600000.0f;
    memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 11 * sizeof(int32_t), &hugeSum, sizeof(float));
    TwoStates_CalcCustomsTouristSpawnForTest(world, customsUsd.data(), &avgUsd, &spawnUsd);
    TEST_CHECK_NEAR(avgUsd, 4.8f, 1e-3f);
    TEST_CHECK(spawnUsd == 8); // 2 base + 2 mkt + (int)4.8 = 8

    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 4: Strict Research & License Isolation (Zero OR-merging)
// ---------------------------------------------------------------------------

static bool SubTest_M9_4_C2_StrictResearchAndLicenseIsolationStress()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const int kTechCount = 8;
    std::vector<BYTE> resBuf(kTechCount * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < kTechCount; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();
    memset(world + WORLD_RESEARCH_UNLOCKS_OFFSET, 0, WORLD_RESEARCH_UNLOCKS_BYTES);

    // 4 vehicle types for production licenses
    std::vector<BYTE> vehTypesBuf(4 * WORLD_VEHICLE_TYPE_STRIDE, 0);
    BYTE* vt0 = vehTypesBuf.data() + 0 * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vt1 = vehTypesBuf.data() + 1 * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vt2 = vehTypesBuf.data() + 2 * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vt3 = vehTypesBuf.data() + 3 * WORLD_VEHICLE_TYPE_STRIDE;
    strcpy_s((char*)(vt0 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "skoda_706");
    strcpy_s((char*)(vt1 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "t815_dump");
    strcpy_s((char*)(vt2 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "ikarus_260");
    strcpy_s((char*)(vt3 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "liaz_100");

    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x00) = vehTypesBuf.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x08) = vehTypesBuf.data() + vehTypesBuf.size();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x10) = vehTypesBuf.data() + vehTypesBuf.size();

    // 4 Universities: 2 West (State 1), 2 East (State 2)
    std::vector<BYTE> univW1(0x1420, 0), univW2(0x1420, 0);
    std::vector<BYTE> univE1(0x1420, 0), univE2(0x1420, 0);

    *(int*)univW1.data() = 801;
    *(float*)(univW1.data() + OBJ_POS_OFFSET) = -800.0f;
    *(float*)(univW1.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    *(int*)univW2.data() = 802;
    *(float*)(univW2.data() + OBJ_POS_OFFSET) = -1200.0f;
    *(float*)(univW2.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    *(int*)univE1.data() = 803;
    *(float*)(univE1.data() + OBJ_POS_OFFSET) = 800.0f;
    *(float*)(univE1.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    *(int*)univE2.data() = 804;
    *(float*)(univE2.data() + OBJ_POS_OFFSET) = 1200.0f;
    *(float*)(univE2.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    std::vector<BYTE*> bldVec = { univW1.data(), univW2.data(), univE1.data(), univE2.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, false, "127.0.0.1:5556");
    TwoStates_OnWorldLoaded();

    // 1. Vehicle Blueprint Licenses Assignment:
    // State 1 acquires "skoda_706" (idx 0) and "liaz_100" (idx 3)
    TwoStates_BeginCommand(1);
    vt0[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    vt3[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    TwoStates_NotifyVehicleLicenseChanged(world);
    TwoStates_EndCommand(1);

    // State 2 acquires "t815_dump" (idx 1) and "ikarus_260" (idx 2)
    TwoStates_BeginCommand(2);
    vt1[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    vt2[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    TwoStates_NotifyVehicleLicenseChanged(world);
    TwoStates_EndCommand(2);

    // Verify initial license isolation
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 0) == true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 1) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 2) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 3) == true);

    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 0) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 1) == true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 2) == true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 3) == false);

    // 2. Simultaneous Research Setup:
    // West Univ 1 -> Tech 0
    // East Univ 1 -> Tech 1
    // West Univ 2 -> Tech 4 (shared target)
    // East Univ 2 -> Tech 4 (shared target)
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univW1.data(), 1, 0));
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univE1.data(), 2, 1));
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univW2.data(), 1, 4));
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univE2.data(), 2, 4));

    // Univ W1 finishes Tech 0 (steps 2 times to reach 1.0f)
    TwoStates_BldUnivStepForTest(world, univW1.data());
    TwoStates_BldUnivStepForTest(world, univW1.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), 1.0f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 0) == 0); // State 2 DOES NOT have Tech 0!

    // Univ E1 finishes Tech 1 (steps 2 times to reach 1.0f)
    TwoStates_BldUnivStepForTest(world, univE1.data());
    TwoStates_BldUnivStepForTest(world, univE1.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 1), 1.0f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 1) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 0); // State 1 DOES NOT have Tech 1!

    // Competing research on Tech 4:
    // West Univ W2 steps ONCE -> progress 0.50f (still in progress)
    TwoStates_BldUnivStepForTest(world, univW2.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 4), 0.50f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 4) == 0);

    // East Univ E2 steps TWICE -> progress reaches 1.0f (completed!)
    TwoStates_BldUnivStepForTest(world, univE2.data());
    TwoStates_BldUnivStepForTest(world, univE2.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 4), 1.0f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 4) == 1);

    // CRITICAL: East completing Tech 4 MUST NOT complete or alter Tech 4 for West!
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 4), 0.50f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 4) == 0);

    // 3. Simulation Step Stress Loop (20 cycles of BeforeStep / AfterStep):
    // Verifies that NO OR-merging or cross-state unlock bleeding occurs across ticks!
    for (int step = 0; step < 20; ++step) {
        // BeforeStep: mounts canonical State 1 baseline
        TwoStates_BeforeStep(world);

        // Simulation baseline check: reflects State 1, NOT OR-union!
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 1); // West unlocked Tech 0
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 0); // East's Tech 1 NOT merged!
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 4) == 0); // East's Tech 4 NOT merged!

        // Vehicle license baseline check: reflects State 1
        TEST_CHECK(vt0[VEH_TYPE_HAS_LICENCE_OFFSET] == 1); // skoda_706
        TEST_CHECK(vt1[VEH_TYPE_HAS_LICENCE_OFFSET] == 0); // t815_dump (owned by State 2, NOT merged!)
        TEST_CHECK(vt2[VEH_TYPE_HAS_LICENCE_OFFSET] == 0); // ikarus_260 (owned by State 2, NOT merged!)
        TEST_CHECK(vt3[VEH_TYPE_HAS_LICENCE_OFFSET] == 1); // liaz_100

        // AfterStep: restores local player UI view
        TwoStates_AfterStep(world);

        // State 1 view intact
        TEST_CHECK(vt0[VEH_TYPE_HAS_LICENCE_OFFSET] == 1);
        TEST_CHECK(vt1[VEH_TYPE_HAS_LICENCE_OFFSET] == 0);
        TEST_CHECK(vt2[VEH_TYPE_HAS_LICENCE_OFFSET] == 0);
        TEST_CHECK(vt3[VEH_TYPE_HAS_LICENCE_OFFSET] == 1);

        // Internal slices of both republics remain strictly isolated
        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 0) == true);
        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 1) == false);
        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 2) == false);
        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 3) == true);

        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 0) == false);
        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 1) == true);
        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 2) == true);
        TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 3) == false);

        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 1);
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 0);
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 4) == 0);

        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 0) == 0);
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 1) == 1);
        TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 4) == 1);
    }

    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 5: Legacy Non-Lockstep Mode Elimination Audit
// ---------------------------------------------------------------------------

static bool SubTest_M9_4_C2_LegacyModeEliminationAudit()
{
    // Feature 39: SaveSync_GetLockstep() MUST return true unconditionally
    TEST_CHECK(SaveSync_GetLockstep() == true);

    // Attempts to set lockstep to false MUST be ignored
    SaveSync_SetLockstep(false);
    TEST_CHECK(SaveSync_GetLockstep() == true);

    SaveSync_SetLockstep(true);
    TEST_CHECK(SaveSync_GetLockstep() == true);

    // SaveSync_IsLockstepSession() reflects session activity
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TEST_CHECK(SaveSync_IsLockstepSession() == false);

    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, false, "127.0.0.1:5556");
    TEST_CHECK(SaveSync_IsLockstepSession() == true);

    SaveSync_SetTestSessionState(SESSION_CLIENT_IN_GAME, false, "127.0.0.1:5556");
    TEST_CHECK(SaveSync_IsLockstepSession() == true);

    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");

    // Port definition checks
    TEST_CHECK(LOCKSTEP_PORT == 5560);
    TEST_CHECK(SAVE_SYNC_PORT == 5556);

    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Master Runner Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M9_4_Challenger2()
{
    printf("    [Challenger2-1: Research Switch Anti-Exploit Security] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_C2_ResearchSwitchAntiExploit()) return false;

    printf("    [Challenger2-2: Border Strip Demolition Permissions & Zone Boundaries] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_C2_BorderDemolitionPermissions()) return false;

    printf("    [Challenger2-3: Customs Tourist Spawn Edge Cases & Rating Indices] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_C2_CustomsTouristSpawnEdgeCases()) return false;

    printf("    [Challenger2-4: Strict Research & License Simulation Isolation (Zero OR-merging)] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_C2_StrictResearchAndLicenseIsolationStress()) return false;

    printf("    [Challenger2-5: Legacy Non-Lockstep Mode Elimination Audit] ...\n"); fflush(stdout);
    if (!SubTest_M9_4_C2_LegacyModeEliminationAudit()) return false;

    return true;
}
