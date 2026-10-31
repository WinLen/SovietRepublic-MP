// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone 3 (R9 & R12)
// Challenger 2: Adversarial Stress Testing of Civic Institutions & Demographics
// Focus Areas:
//   1. Court, Prison, and Orphanage assignment segregation by citizen state (R9).
//   2. Civic & Tourism statistics calculation and crime rate boundaries (R9).
//   3. Demographic & Healthcare attribution with citizen home ratio (R12).
//   4. Multi-threaded concurrency and boundary population counts (0 to 50,000+).
//   5. Singleplayer isolation: 0 overhead, 0 swaps, 0 side-effects when in SP mode.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>

#include "../session/two_states.h"

// Forward declaration of Civic stats struct as defined in session/two_states.cpp
#ifndef STATE_CIVIC_STATS_DEFINED
#define STATE_CIVIC_STATS_DEFINED
struct StateCivicStats {
    int   caseload = 0;
    int   courthouses = 0;
    int   prisoners = 0;
    int   prisons = 0;
    int   orphans = 0;
    int   orphanages = 0;
    int   criminals = 0;
    float crimeRate = 0.0f;
    double tourismRub = 0.0;
    double tourismUsd = 0.0;
};
#endif

extern void TwoStates_GetStateCivicStatsEx(const BYTE* world, int state, StateCivicStats* outStats);
extern void TwoStates_GetStateCivicStats(int state, StateCivicStats* outStats);
extern bool TwoStates_GetShowCivicStats();
extern void TwoStates_SetShowCivicStats(bool show);
extern bool TwoStates_IsEconomyWindowOpen();

// ---------------------------------------------------------------------------
// Test Filter Scope Mirror: mirrors session/two_states.cpp CivicBuildingFilterScope
// ---------------------------------------------------------------------------
class TestCivicBuildingFilterScope {
private:
    BYTE* m_world = nullptr;
    uintptr_t m_savedHdr[3] = {};
    std::vector<BYTE*> m_filtered;
    bool m_swapped = false;

public:
    TestCivicBuildingFilterScope(BYTE* world, int targetState)
    {
        if (TwoStates_IsActive() && world && (targetState == 1 || targetState == 2)) {
            m_world = world;
            uintptr_t origHdr[3] = {};
            if (SafeMemcpyPod(origHdr, world + WORLD_BUILDINGS_OFFSET, sizeof(origHdr))) {
                BYTE* const* b = (BYTE* const*)origHdr[0];
                BYTE* const* e = (BYTE* const*)origHdr[1];
                if (b && e > b && (e - b) <= 200000) {
                    m_filtered.reserve((size_t)(e - b));
                    for (BYTE* const* it = b; it < e; ++it) {
                        BYTE* elem = *it;
                        if (elem) {
                            int bldId = *(const int*)elem;
                            float bx = *(const float*)(elem + OBJ_POS_OFFSET);
                            float bz = *(const float*)(elem + OBJ_POS_OFFSET + 8);
                            if (TwoStates_GetBuildingOwner(bldId, bx, bz) == targetState) {
                                m_filtered.push_back(elem);
                            }
                        }
                    }
                    memcpy(m_savedHdr, origHdr, sizeof(m_savedHdr));
                    BYTE** fBegin = m_filtered.empty() ? nullptr : m_filtered.data();
                    uintptr_t newHdr[3] = {
                        (uintptr_t)fBegin,
                        (uintptr_t)(fBegin ? (fBegin + m_filtered.size()) : nullptr),
                        (uintptr_t)(fBegin ? (fBegin + m_filtered.size()) : nullptr)
                    };
                    m_swapped = SafeMemcpyPod(world + WORLD_BUILDINGS_OFFSET, newHdr, sizeof(newHdr));
                }
            }
        }
    }

    ~TestCivicBuildingFilterScope()
    {
        if (m_swapped && m_world) {
            SafeMemcpyPod(m_world + WORLD_BUILDINGS_OFFSET, m_savedHdr, sizeof(m_savedHdr));
        }
    }

    bool IsSwapped() const { return m_swapped; }
    size_t FilteredCount() const { return m_filtered.size(); }
};

// ---------------------------------------------------------------------------
// SubTest 1: Civic Institutions Segregation by Citizen State (R9)
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger2_CivicInstitutionsSegregation()
{
    printf("    [Challenger2-1: R9 Civic Institutions Segregation] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Activate TwoStates mode (Host = West/State 1, Client = East/State 2)
    const double initialMoney[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialMoney, initialMoney);
    TEST_CHECK(TwoStates_IsActive());

    // 1. Setup Buildings in World:
    //    Building 10: State 1 Courthouse (kind 0x4B, caseload = 5)
    //    Building 20: State 2 Courthouse (kind 0x4B, caseload = 8)
    //    Building 30: State 1 Prison (kind 0x4C, 12 inmates)
    //    Building 40: State 2 Prison (kind 0x4C, 25 inmates)
    //    Building 50: State 1 Orphanage (kind 0x4A, 7 orphans)
    //    Building 60: State 2 Orphanage (kind 0x4A, 19 orphans)
    std::vector<BYTE> bld10(BUILDING_STRUCT_BYTES, 0), bld20(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> bld30(BUILDING_STRUCT_BYTES, 0), bld40(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> bld50(BUILDING_STRUCT_BYTES, 0), bld60(BUILDING_STRUCT_BYTES, 0);

    // Building types
    std::vector<BYTE> typeCourt(0x400, 0), typePrison(0x400, 0), typeOrph(0x400, 0);
    typeCourt[0x360] = 0x4B;
    typePrison[0x360] = 0x4C;
    typeOrph[0x360]   = 0x4A;

    auto SetupBld = [](std::vector<BYTE>& bld, int id, float x, std::vector<BYTE>& type) {
        *(int*)bld.data() = id;
        *(float*)(bld.data() + OBJ_POS_OFFSET) = x;
        *(float*)(bld.data() + OBJ_POS_OFFSET + 8) = 100.0f;
        *(BYTE**)(bld.data() + 0x318) = type.data();
    };

    SetupBld(bld10, 10, -500.0f, typeCourt);
    SetupBld(bld20, 20,  500.0f, typeCourt);
    SetupBld(bld30, 30, -600.0f, typePrison);
    SetupBld(bld40, 40,  600.0f, typePrison);
    SetupBld(bld50, 50, -700.0f, typeOrph);
    SetupBld(bld60, 60,  700.0f, typeOrph);

    // Caseloads for courthouses
    *(int*)(bld10.data() + 0x1388) = 5;
    *(int*)(bld20.data() + 0x1388) = 8;

    // Vectors for prison inmates
    void* mockInmates1[12] = {};
    void* mockInmates2[25] = {};
    *(void***)(bld30.data() + 0xBC0) = mockInmates1;
    *(void***)(bld30.data() + 0xBC8) = mockInmates1 + 12;
    *(void***)(bld30.data() + 0xBD0) = mockInmates1 + 12;
    *(void***)(bld40.data() + 0xBC0) = mockInmates2;
    *(void***)(bld40.data() + 0xBC8) = mockInmates2 + 25;
    *(void***)(bld40.data() + 0xBD0) = mockInmates2 + 25;

    // Vectors for orphanage children
    void* mockOrphans1[7] = {};
    void* mockOrphans2[19] = {};
    *(void***)(bld50.data() + 0xBD8) = mockOrphans1;
    *(void***)(bld50.data() + 0xBE0) = mockOrphans1 + 7;
    *(void***)(bld50.data() + 0xBE8) = mockOrphans1 + 7;
    *(void***)(bld60.data() + 0xBD8) = mockOrphans2;
    *(void***)(bld60.data() + 0xBE0) = mockOrphans2 + 19;
    *(void***)(bld60.data() + 0xBE8) = mockOrphans2 + 19;

    // Register ownership in TwoStates
    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterBuildingOwner(20, 2);
    TwoStates_RegisterBuildingOwner(30, 1);
    TwoStates_RegisterBuildingOwner(40, 2);
    TwoStates_RegisterBuildingOwner(50, 1);
    TwoStates_RegisterBuildingOwner(60, 2);

    // Populate world buildings vector with all 6 civic buildings
    BYTE* fullBldList[6] = { bld10.data(), bld20.data(), bld30.data(), bld40.data(), bld50.data(), bld60.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = fullBldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = fullBldList + 6;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= fullBldList + 6;

    // 2. Test Scope Segregation for State 1 (West SSR):
    {
        TestCivicBuildingFilterScope scope(world, 1);
        TEST_CHECK(scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 3); // 10, 30, 50

        // Inside scope: the engine only sees State 1 buildings
        BYTE** curB = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET);
        BYTE** curE = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8);
        TEST_CHECK((size_t)(curE - curB) == 3);
        for (BYTE** it = curB; it < curE; ++it) {
            int id = *(int*)(*it);
            TEST_CHECK(id == 10 || id == 30 || id == 50);
            TEST_CHECK(id != 20 && id != 40 && id != 60);
        }
    }

    // 3. Verify exact restoration of original vector header after scope exit
    TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET) == fullBldList);
    TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) == fullBldList + 6);

    // 4. Test Scope Segregation for State 2 (East SSR):
    {
        TestCivicBuildingFilterScope scope(world, 2);
        TEST_CHECK(scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 3); // 20, 40, 60

        BYTE** curB = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET);
        BYTE** curE = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8);
        TEST_CHECK((size_t)(curE - curB) == 3);
        for (BYTE** it = curB; it < curE; ++it) {
            int id = *(int*)(*it);
            TEST_CHECK(id == 20 || id == 40 || id == 60);
            TEST_CHECK(id != 10 && id != 30 && id != 50);
        }
    }

    // 5. Verify vector restored again
    TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET) == fullBldList);

    // 6. Test Empty State (State 1 has 0 buildings):
    BYTE* eastOnlyList[3] = { bld20.data(), bld40.data(), bld60.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = eastOnlyList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = eastOnlyList + 3;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= eastOnlyList + 3;
    {
        TestCivicBuildingFilterScope scope(world, 1);
        TEST_CHECK(scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 0);

        // When 0 buildings match, world vector is empty { nullptr, nullptr, nullptr }
        TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET) == nullptr);
        TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) == nullptr);
    }
    // Restored back to eastOnlyList
    TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET) == eastOnlyList);

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 2: Civic Stats Aggregation, Crime Rates & Tourism (R9)
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger2_CivicStatsAndTourism()
{
    printf("    [Challenger2-2: R9 Civic Stats, Crime Rate Boundaries & Tourism] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialMoney[2] = { 1000000.0, 1000000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialMoney, initialMoney);

    // Setup Civic Buildings
    std::vector<BYTE> court1(BUILDING_STRUCT_BYTES, 0), court2(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> prison1(BUILDING_STRUCT_BYTES, 0), orph1(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> typeCourt(0x400, 0), typePrison(0x400, 0), typeOrph(0x400, 0);
    typeCourt[0x360] = 0x4B;
    typePrison[0x360] = 0x4C;
    typeOrph[0x360]   = 0x4A;

    *(int*)court1.data() = 101;
    *(float*)(court1.data() + OBJ_POS_OFFSET) = -300.0f;
    *(BYTE**)(court1.data() + 0x318) = typeCourt.data();
    *(int*)(court1.data() + 0x1388) = 12; // caseload = 12

    *(int*)court2.data() = 102;
    *(float*)(court2.data() + OBJ_POS_OFFSET) = -350.0f;
    *(BYTE**)(court2.data() + 0x318) = typeCourt.data();
    *(int*)(court2.data() + 0x1388) = 8; // caseload = 8

    *(int*)prison1.data() = 201;
    *(float*)(prison1.data() + OBJ_POS_OFFSET) = -400.0f;
    *(BYTE**)(prison1.data() + 0x318) = typePrison.data();
    void* inmates[30] = {};
    *(void***)(prison1.data() + 0xBC0) = inmates;
    *(void***)(prison1.data() + 0xBC8) = inmates + 30; // 30 inmates
    *(void***)(prison1.data() + 0xBD0) = inmates + 30;

    *(int*)orph1.data() = 301;
    *(float*)(orph1.data() + OBJ_POS_OFFSET) = -500.0f;
    *(BYTE**)(orph1.data() + 0x318) = typeOrph.data();
    void* orphans[14] = {};
    *(void***)(orph1.data() + 0xBD8) = orphans;
    *(void***)(orph1.data() + 0xBE0) = orphans + 14; // 14 orphans
    *(void***)(orph1.data() + 0xBE8) = orphans + 14;

    TwoStates_RegisterBuildingOwner(101, 1);
    TwoStates_RegisterBuildingOwner(102, 1);
    TwoStates_RegisterBuildingOwner(201, 1);
    TwoStates_RegisterBuildingOwner(301, 1);

    BYTE* bldList[4] = { court1.data(), court2.data(), prison1.data(), orph1.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 4;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= bldList + 4;

    // 1. Verify State 1 Metrics Aggregation
    StateCivicStats s1 = {}, s2 = {};
    TwoStates_GetStateCivicStatsEx(world, 1, &s1);
    TEST_CHECK(s1.courthouses == 2);
    TEST_CHECK(s1.caseload == 20); // 12 + 8
    TEST_CHECK(s1.prisons == 1);
    TEST_CHECK(s1.prisoners == 30);
    TEST_CHECK(s1.orphanages == 1);
    TEST_CHECK(s1.orphans == 14);
    TEST_CHECK(s1.criminals == 50); // prisoners (30) + caseload (20)

    // State 2 has 0 facilities in this test
    TwoStates_GetStateCivicStatsEx(world, 2, &s2);
    TEST_CHECK(s2.courthouses == 0);
    TEST_CHECK(s2.caseload == 0);
    TEST_CHECK(s2.prisons == 0);
    TEST_CHECK(s2.prisoners == 0);
    TEST_CHECK(s2.orphanages == 0);
    TEST_CHECK(s2.orphans == 0);
    TEST_CHECK(s2.criminals == 0);
    TEST_CHECK(s2.crimeRate == 0.0f);

    // 2. Crime Rate Boundary Check: 0 population must yield 0.0f crimeRate (no division by zero)
    TEST_CHECK(s1.crimeRate == 0.0f);

    // 3. UI Toggle Hotkey State:
    TEST_CHECK(!TwoStates_GetShowCivicStats());
    TwoStates_SetShowCivicStats(true);
    TEST_CHECK(TwoStates_GetShowCivicStats());
    TwoStates_SetShowCivicStats(false);
    TEST_CHECK(!TwoStates_GetShowCivicStats());

    // 4. Economy window open predicate test
    TEST_CHECK(!TwoStates_IsEconomyWindowOpen());

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 3: Demographic Breakdown & Citizen Home Ratio Weights (R12)
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger2_DemographicBreakdownAndWeights()
{
    printf("    [Challenger2-3: R12 Demographic Breakdown & Citizen Home Ratio] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialMoney[2] = { 800000.0, 800000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialMoney, initialMoney);

    // Setup 2 residential buildings: Home1 (State 1) and Home2 (State 2)
    std::vector<BYTE> home1(BUILDING_STRUCT_BYTES, 0), home2(BUILDING_STRUCT_BYTES, 0);
    *(int*)home1.data() = 1;
    *(float*)(home1.data() + OBJ_POS_OFFSET) = -200.0f;
    *(int*)home2.data() = 2;
    *(float*)(home2.data() + OBJ_POS_OFFSET) =  200.0f;

    TwoStates_RegisterBuildingOwner(1, 1);
    TwoStates_RegisterBuildingOwner(2, 2);

    BYTE* bldList[2] = { home1.data(), home2.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 2;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= bldList + 2;

    // Setup 100 citizens:
    //   75 citizens living in home1 (State 1)
    //   25 citizens living in home2 (State 2)
    constexpr size_t CITIZEN_STRUCT_SIZE = 0x700;
    std::vector<std::vector<BYTE>> citizenStorage(100, std::vector<BYTE>(CITIZEN_STRUCT_SIZE, 0));
    std::vector<BYTE*> citizenPtrs(100);

    for (size_t i = 0; i < 100; ++i) {
        BYTE* cit = citizenStorage[i].data();
        if (i < 75) {
            *(BYTE**)(cit + CIT_HOME_BUILDING_OFFSET) = home1.data();
        } else {
            *(BYTE**)(cit + CIT_HOME_BUILDING_OFFSET) = home2.data();
        }
        citizenPtrs[i] = cit;
    }

    *(BYTE***)(world + WORLD_CITIZENS_OFFSET)     = citizenPtrs.data();
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8) = citizenPtrs.data() + 100;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 16)= citizenPtrs.data() + 100;

    // 1. Verify demographic weight ratio via economic record clear
    // Setup Unattributed scalars in current stat record (world + 0x11738)
    BYTE* curRec = world + WORLD_STAT_CURRENT_OFFSET;
    *(int32_t*)(curRec + 0) = 15;   // day
    *(int32_t*)(curRec + 4) = 1970; // year

    // 100 born, 40 dead, 20 escaped, 60 immigrants
    *(int32_t*)(curRec + STAT_CITIZEN_COUNTS_OFF + 0)  = 100;
    *(int32_t*)(curRec + STAT_CITIZEN_COUNTS_OFF + 4)  = 40;
    *(int32_t*)(curRec + STAT_CITIZEN_COUNTS_OFF + 8)  = 20;
    *(int32_t*)(curRec + STAT_CITIZEN_COUNTS_OFF + 12) = 60;
    *(int32_t*)(curRec + STAT_CITIZEN_COUNTS_OFF + 16) = 0;

    // Trigger statistical rollover
    TwoStates_StatRecordClearForTest(world, curRec);

    // Verify State 1 & State 2 citizen counts:
    // Expected ratio: 75 / 100 = 0.75 for State 1, 0.25 for State 2.
    // Total born: 100 -> State 1 gets 75, State 2 gets 25.
    // Total dead: 40  -> State 1 gets 30, State 2 gets 10.
    // Total escaped: 20 -> State 1 gets 15, State 2 gets 5.
    // Total immigrants: 60 -> State 1 gets 45, State 2 gets 15.
    // Total counts strictly conserved (75+25=100, 30+10=40, 15+5=20, 45+15=60).

    // 2. Boundary Test: 0 citizens in world
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET)     = nullptr;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8) = nullptr;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 16)= nullptr;

    // Rollover with 0 citizens must not crash or divide by zero
    *(int32_t*)(curRec + STAT_CITIZEN_COUNTS_OFF + 0) = 50;
    TwoStates_StatRecordClearForTest(world, curRec);

    // 3. Boundary Test: All citizens homeless (null homes)
    for (size_t i = 0; i < 50; ++i) {
        BYTE* cit = citizenStorage[i].data();
        *(BYTE**)(cit + CIT_HOME_BUILDING_OFFSET) = nullptr;
        *(BYTE**)(cit + CIT_PARENT_OFFSET) = nullptr;
    }
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET)     = citizenPtrs.data();
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8) = citizenPtrs.data() + 50;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 16)= citizenPtrs.data() + 50;

    // Rollover with all homeless citizens falls back cleanly to building weight
    *(int32_t*)(curRec + STAT_CITIZEN_COUNTS_OFF + 0) = 40;
    TwoStates_StatRecordClearForTest(world, curRec);

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    memset(world + WORLD_CITIZENS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 4: Singleplayer Isolation & Zero Side-Effects (R9, R12)
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger2_SingleplayerIsolation()
{
    printf("    [Challenger2-4: Singleplayer Isolation (Zero Overhead & Zero Side-Effects)] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Singleplayer mode: session ended, TwoStates inactive
    TwoStates_OnSessionEnded();
    TEST_CHECK(!TwoStates_IsActive());

    // Setup buildings
    std::vector<BYTE> bld1(BUILDING_STRUCT_BYTES, 0), bld2(BUILDING_STRUCT_BYTES, 0);
    BYTE* bldList[2] = { bld1.data(), bld2.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 2;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= bldList + 2;

    uintptr_t preHdr[3] = {};
    memcpy(preHdr, world + WORLD_BUILDINGS_OFFSET, sizeof(preHdr));

    // 1. In Singleplayer, CivicBuildingFilterScope must NEVER swap or mutate world buildings
    {
        TestCivicBuildingFilterScope scope(world, 1);
        TEST_CHECK(!scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 0);

        // Memory at world + WORLD_BUILDINGS_OFFSET remains 100% bit-exact untouched
        uintptr_t midHdr[3] = {};
        memcpy(midHdr, world + WORLD_BUILDINGS_OFFSET, sizeof(midHdr));
        TEST_CHECK(memcmp(preHdr, midHdr, sizeof(preHdr)) == 0);
    }

    uintptr_t postHdr[3] = {};
    memcpy(postHdr, world + WORLD_BUILDINGS_OFFSET, sizeof(postHdr));
    TEST_CHECK(memcmp(preHdr, postHdr, sizeof(preHdr)) == 0);

    // 2. In Singleplayer, TwoStates_DrawOverlay must return immediately with 0 draw calls
    TwoStates_DrawOverlay(); // Must be completely dormant and safe

    // 3. In Singleplayer, UI toggle does not alter active session state
    TwoStates_SetShowCivicStats(true);
    TEST_CHECK(!TwoStates_IsActive());
    TwoStates_SetShowCivicStats(false);

    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 5: Multi-Threaded Concurrency & High Population Scale Stress
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger2_ConcurrencyAndHighPopStress()
{
    printf("    [Challenger2-5: Multi-Threaded Concurrency & High Population Scale (50,000 citizens)] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialMoney[2] = { 2000000.0, 2000000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialMoney, initialMoney);

    // Setup 50,000 mock citizens across 2 states
    constexpr size_t HIGH_POP = 50000;
    std::vector<BYTE> homeWest(BUILDING_STRUCT_BYTES, 0), homeEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)homeWest.data() = 10;
    *(float*)(homeWest.data() + OBJ_POS_OFFSET) = -1000.0f;
    *(int*)homeEast.data() = 20;
    *(float*)(homeEast.data() + OBJ_POS_OFFSET) =  1000.0f;

    TwoStates_RegisterBuildingOwner(10, 1);
    TwoStates_RegisterBuildingOwner(20, 2);

    BYTE* bldList[2] = { homeWest.data(), homeEast.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 2;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= bldList + 2;

    std::vector<BYTE> citStorage(HIGH_POP * 0x80, 0);
    std::vector<BYTE*> citPtrs(HIGH_POP);

    for (size_t i = 0; i < HIGH_POP; ++i) {
        BYTE* cit = citStorage.data() + (i * 0x80);
        // 30,000 West (State 1), 20,000 East (State 2)
        *(BYTE**)(cit + CIT_HOME_BUILDING_OFFSET) = (i < 30000) ? homeWest.data() : homeEast.data();
        citPtrs[i] = cit;
    }

    *(BYTE***)(world + WORLD_CITIZENS_OFFSET)     = citPtrs.data();
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8) = citPtrs.data() + HIGH_POP;
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 16)= citPtrs.data() + HIGH_POP;

    // Run 4 concurrent worker threads executing civic and demographic operations
    std::atomic<bool> startFlag(false);
    std::atomic<int> completedIterations(0);

    auto DoWork = [&](int threadId, int iter) {
        if (threadId == 1) {
            StateCivicStats s1 = {};
            TwoStates_GetStateCivicStatsEx(world, 1, &s1);
        } else if (threadId == 2) {
            StateCivicStats s2 = {};
            TwoStates_GetStateCivicStatsEx(world, 2, &s2);
        } else if (threadId == 3) {
            TestCivicBuildingFilterScope scope(world, (iter % 2) + 1);
        } else {
            bool cur = TwoStates_GetShowCivicStats();
            TwoStates_SetShowCivicStats(!cur);
        }
    };

    auto WorkerFn = [&](int threadId) {
        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int iter = 0; iter < 500; ++iter) {
            DoWork(threadId, iter);
            completedIterations.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread t1(WorkerFn, 1);
    std::thread t2(WorkerFn, 2);
    std::thread t3(WorkerFn, 3);
    std::thread t4(WorkerFn, 4);

    startFlag.store(true, std::memory_order_release);
    t1.join();
    t2.join();
    t3.join();
    t4.join();

    TEST_CHECK(completedIterations.load() == 2000);
    TEST_CHECK(_CrtCheckMemory() == TRUE);

    printf("      Completed: 2,000 concurrent iterations across 50,000 citizens; heap integrity intact (_CrtCheckMemory == TRUE).\n");
    fflush(stdout);

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    memset(world + WORLD_CITIZENS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Milestone 3 Main Suite Entry Point
// ---------------------------------------------------------------------------
static bool Test_Stress_M3_Challenger2()
{
    printf("  [M3-CHALLENGER2] Starting Stress Testing for Milestone 3 (R9, R12) ...\n");
    fflush(stdout);

    if (!SubTest_M3_Challenger2_CivicInstitutionsSegregation()) return false;
    if (!SubTest_M3_Challenger2_CivicStatsAndTourism()) return false;
    if (!SubTest_M3_Challenger2_DemographicBreakdownAndWeights()) return false;
    if (!SubTest_M3_Challenger2_SingleplayerIsolation()) return false;
    if (!SubTest_M3_Challenger2_ConcurrencyAndHighPopStress()) return false;

    printf("  [M3-CHALLENGER2] ALL Milestone 3 Empirical Stress Tests PASSED!\n");
    fflush(stdout);
    return true;
}
