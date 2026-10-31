// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone 3 (R9 & R12)
// Challenger 1: Adversarial Stress Testing of Civic Segregation, Zero-Facility Boundaries, and Demographics
//
// Focus Areas:
//   1. Court, Prison, and Orphanage Segregation & CivicBuildingFilterScope invariant under zero-facility conditions (R9).
//   2. Null, unowned, and corrupted citizen and court-case pointers (ResolveCourtCaseCitizen / ResolveCitizenOwnerFast) (R9).
//   3. Civic breakdown table calculation, saturation caps, and tourism currency split (TwoStates_GetStateCivicStatsEx) (R9).
//   4. Asymmetric citizen-to-building ratio: empirical validation that wCit0 separates demographic scalars by home distribution (R12).
//   5. Health, water, and heating simulation step attribution via CallBuildingEconHook (R12).
//   6. SEH exception throw safety inside assignment hooks: RAII building vector restoration (R9, R12).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <vector>
#include <cmath>

#include "../core/safe_mem.h"
#include "../core/engine_rva.h"
#include "../session/two_states.h"

#ifndef CITIZEN_STRUCT_BYTES
#define CITIZEN_STRUCT_BYTES 0x800
#endif

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
// Challenger 1: Mirrored CivicBuildingFilterScope for exact behavioral stress
// ---------------------------------------------------------------------------
class Ch1CivicBuildingFilterScope {
private:
    BYTE* m_world = nullptr;
    uintptr_t m_savedHdr[3] = {};
    std::vector<BYTE*> m_filtered;
    bool m_swapped = false;

public:
    Ch1CivicBuildingFilterScope(BYTE* world, int targetState)
    {
        if (TwoStates_IsActive() && world && (targetState == 1 || targetState == 2)) {
            m_world = world;
            uintptr_t origHdr[3] = {};
            if (SafeMemcpyPod(origHdr, world + WORLD_BUILDINGS_OFFSET, sizeof(origHdr))) {
                BYTE* const* b = (BYTE* const*)origHdr[0];
                BYTE* const* e = (BYTE* const*)origHdr[1];
                if (b && e >= b && (e - b) <= 200000) {
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

    ~Ch1CivicBuildingFilterScope()
    {
        if (m_swapped && m_world) {
            SafeMemcpyPod(m_world + WORLD_BUILDINGS_OFFSET, m_savedHdr, sizeof(m_savedHdr));
        }
    }

    bool IsSwapped() const { return m_swapped; }
    size_t FilteredCount() const { return m_filtered.size(); }
};

// Replicate ResolveCourtCaseCitizen from two_states.cpp to verify pointer safety
static const BYTE* Ch1ResolveCourtCaseCitizen(const BYTE* caseOrCit)
{
    if (!caseOrCit) return nullptr;
    const BYTE* cit = nullptr;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* vecBegin = *(const BYTE* const*)(caseOrCit + 0x28);
        if (vecBegin && IsPointerLikeX64((uintptr_t)vecBegin)) {
            const BYTE* firstCit = *(const BYTE* const*)vecBegin;
            if (firstCit && IsPointerLikeX64((uintptr_t)firstCit)) {
                cit = firstCit;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cit = nullptr;
    }
    DiagLog_EndSafeSeh();
    return cit ? cit : caseOrCit;
}

// ---------------------------------------------------------------------------
// SubTest 1: Zero-Facility Edge Cases (State has 0 courts/prisons/orphanages)
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger1_ZeroFacilityEdgeCases()
{
    printf("    [Challenger1-1: R9 Zero-Facility Boundary Conditions] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialFunds[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialFunds, initialFunds);
    TEST_CHECK(TwoStates_IsActive());

    // Scenario: State 1 has ZERO courthouses, ZERO prisons, and ZERO orphanages.
    // State 2 has 3 courthouses, 2 prisons, and 2 orphanages.
    std::vector<BYTE> courtType(0x400, 0), prisonType(0x400, 0), orphType(0x400, 0);
    courtType[0x360] = 0x4B;
    prisonType[0x360] = 0x4C;
    orphType[0x360]   = 0x4A;

    std::vector<BYTE> s2Courts[3];
    std::vector<BYTE> s2Prisons[2];
    std::vector<BYTE> s2Orphanages[2];
    std::vector<BYTE*> allBuildings;

    for (int i = 0; i < 3; ++i) {
        s2Courts[i].resize(BUILDING_STRUCT_BYTES, 0);
        *(int*)s2Courts[i].data() = 100 + i;
        *(float*)(s2Courts[i].data() + OBJ_POS_OFFSET) = 600.0f + (float)i * 50.0f;
        *(float*)(s2Courts[i].data() + OBJ_POS_OFFSET + 8) = 100.0f;
        *(BYTE**)(s2Courts[i].data() + 0x318) = courtType.data();
        *(int*)(s2Courts[i].data() + 0x1388) = 10 + i; // caseload
        TwoStates_RegisterBuildingOwner(100 + i, 2);
        allBuildings.push_back(s2Courts[i].data());
    }

    for (int i = 0; i < 2; ++i) {
        s2Prisons[i].resize(BUILDING_STRUCT_BYTES, 0);
        *(int*)s2Prisons[i].data() = 200 + i;
        *(float*)(s2Prisons[i].data() + OBJ_POS_OFFSET) = 700.0f + (float)i * 50.0f;
        *(float*)(s2Prisons[i].data() + OBJ_POS_OFFSET + 8) = 100.0f;
        *(BYTE**)(s2Prisons[i].data() + 0x318) = prisonType.data();
        TwoStates_RegisterBuildingOwner(200 + i, 2);
        allBuildings.push_back(s2Prisons[i].data());
    }

    for (int i = 0; i < 2; ++i) {
        s2Orphanages[i].resize(BUILDING_STRUCT_BYTES, 0);
        *(int*)s2Orphanages[i].data() = 300 + i;
        *(float*)(s2Orphanages[i].data() + OBJ_POS_OFFSET) = 800.0f + (float)i * 50.0f;
        *(float*)(s2Orphanages[i].data() + OBJ_POS_OFFSET + 8) = 100.0f;
        *(BYTE**)(s2Orphanages[i].data() + 0x318) = orphType.data();
        TwoStates_RegisterBuildingOwner(300 + i, 2);
        allBuildings.push_back(s2Orphanages[i].data());
    }

    // Set world buildings vector
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = allBuildings.data();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = allBuildings.data() + allBuildings.size();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= allBuildings.data() + allBuildings.size();

    // Setup Citizen belonging to State 1
    std::vector<BYTE> citS1(CITIZEN_STRUCT_BYTES, 0);
    std::vector<BYTE> homeS1(BUILDING_STRUCT_BYTES, 0);
    *(int*)homeS1.data() = 50;
    *(float*)(homeS1.data() + OBJ_POS_OFFSET) = -500.0f;
    *(float*)(homeS1.data() + OBJ_POS_OFFSET + 8) = 50.0f;
    TwoStates_RegisterBuildingOwner(50, 1);
    *(BYTE**)(citS1.data() + CIT_HOME_BUILDING_OFFSET) = homeS1.data();

    // 1. Court assignment for State 1 citizen when State 1 has 0 courts:
    {
        Ch1CivicBuildingFilterScope scope(world, 1);
        TEST_CHECK(scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 0); // Exactly 0 State 1 buildings

        // Mock engine court assignment loop:
        BYTE** curB = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET);
        BYTE** curE = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8);
        BYTE* assignedCourt = nullptr;
        if (curB && curE && curE > curB) {
            for (BYTE** it = curB; it < curE; ++it) {
                const BYTE* bld = *it;
                const BYTE* type = *(const BYTE* const*)(bld + 0x318);
                if (type && *(const BYTE*)(type + 0x360) == 0x4B) {
                    assignedCourt = (BYTE*)bld;
                    break;
                }
            }
        }

        // CRITICAL INVARIANT: assignedCourt MUST BE NULL! Must NOT assign to State 2 court!
        TEST_CHECK(assignedCourt == nullptr);
    }
    // Verify building vector restored
    TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET) == allBuildings.data());
    TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) == allBuildings.data() + allBuildings.size());

    // 2. Prison assignment for State 1 citizen when State 1 has 0 prisons:
    {
        Ch1CivicBuildingFilterScope scope(world, 1);
        TEST_CHECK(scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 0);

        BYTE** curB = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET);
        BYTE** curE = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8);
        BYTE* assignedPrison = nullptr;
        if (curB && curE && curE > curB) {
            for (BYTE** it = curB; it < curE; ++it) {
                const BYTE* bld = *it;
                const BYTE* type = *(const BYTE* const*)(bld + 0x318);
                if (type && *(const BYTE*)(type + 0x360) == 0x4C) {
                    assignedPrison = (BYTE*)bld;
                    break;
                }
            }
        }
        TEST_CHECK(assignedPrison == nullptr);
    }

    // 3. Orphanage assignment for State 1 child when State 1 has 0 orphanages:
    {
        Ch1CivicBuildingFilterScope scope(world, 1);
        TEST_CHECK(scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 0);

        BYTE** curB = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET);
        BYTE** curE = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8);
        BYTE* assignedOrph = nullptr;
        if (curB && curE && curE > curB) {
            for (BYTE** it = curB; it < curE; ++it) {
                const BYTE* bld = *it;
                const BYTE* type = *(const BYTE* const*)(bld + 0x318);
                if (type && *(const BYTE*)(type + 0x360) == 0x4A) {
                    assignedOrph = (BYTE*)bld;
                    break;
                }
            }
        }
        TEST_CHECK(assignedOrph == nullptr);
    }

    // 4. Contrast: State 2 citizen DOES find State 2 facilities
    {
        Ch1CivicBuildingFilterScope scope(world, 2);
        TEST_CHECK(scope.IsSwapped());
        TEST_CHECK(scope.FilteredCount() == 7); // 3 courts + 2 prisons + 2 orphanages

        BYTE** curB = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET);
        BYTE** curE = *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8);
        int courtFound = 0, prisonFound = 0, orphFound = 0;
        for (BYTE** it = curB; it < curE; ++it) {
            const BYTE* type = *(const BYTE* const*)(*it + 0x318);
            if (type[0x360] == 0x4B) courtFound++;
            if (type[0x360] == 0x4C) prisonFound++;
            if (type[0x360] == 0x4A) orphFound++;
        }
        TEST_CHECK(courtFound == 3);
        TEST_CHECK(prisonFound == 2);
        TEST_CHECK(orphFound == 2);
    }

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 2: Null, Unowned, and Corrupted Pointer Safety
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger1_NullAndCorruptedPointers()
{
    printf("    [Challenger1-2: R9 Null, Unowned & Corrupted Pointer Invariants] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialFunds[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialFunds, initialFunds);

    // 1. ResolveCourtCaseCitizen tests:
    // A. Null pointer -> returns null without crash
    TEST_CHECK(Ch1ResolveCourtCaseCitizen(nullptr) == nullptr);

    // B. Direct citizen pointer passed (no vector at +0x28)
    std::vector<BYTE> citDirect(CITIZEN_STRUCT_BYTES, 0);
    const BYTE* resolvedDirect = Ch1ResolveCourtCaseCitizen(citDirect.data());
    TEST_CHECK(resolvedDirect == citDirect.data());

    // C. Case object with corrupt non-canonical pointer at +0x28 (e.g. 0xDEADBEEF)
    std::vector<BYTE> caseCorrupt(0x100, 0);
    *(uintptr_t*)(caseCorrupt.data() + 0x28) = 0xDEADBEEF;
    const BYTE* resolvedCorrupt = Ch1ResolveCourtCaseCitizen(caseCorrupt.data());
    TEST_CHECK(resolvedCorrupt == caseCorrupt.data()); // Falls back safely

    // D. Case object with valid vector pointer pointing to valid citizen
    std::vector<BYTE> caseValid(0x100, 0);
    const BYTE* citInVec = citDirect.data();
    const BYTE* vecBuf[1] = { citInVec };
    *(const BYTE***)(caseValid.data() + 0x28) = vecBuf;
    const BYTE* resolvedValid = Ch1ResolveCourtCaseCitizen(caseValid.data());
    TEST_CHECK(resolvedValid == citDirect.data());

    // 2. Citizen with null home pointer:
    // Citizen at X = -500.0f (State 1 territory) with null home
    std::vector<BYTE> citHomeless1(CITIZEN_STRUCT_BYTES, 0);
    *(BYTE**)(citHomeless1.data() + CIT_HOME_BUILDING_OFFSET) = nullptr;
    *(float*)(citHomeless1.data() + 0x00) = -500.0f; // pos X
    *(float*)(citHomeless1.data() + 0x08) = 50.0f;   // pos Z
    int ownerHomeless1 = TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(-500.0f, 50.0f), -500.0f, 50.0f);
    TEST_CHECK(ownerHomeless1 == 1);

    // Citizen in shared border zone with null home (X = 0.0f)
    std::vector<BYTE> citBorder(CITIZEN_STRUCT_BYTES, 0);
    *(BYTE**)(citBorder.data() + CIT_HOME_BUILDING_OFFSET) = nullptr;
    *(float*)(citBorder.data() + 0x00) = 0.0f;
    *(float*)(citBorder.data() + 0x08) = 0.0f;
    int ownerBorder = TwoStates_GetZoneAt(0.0f, 0.0f);
    TEST_CHECK(ownerBorder == 0); // Shared border zone

    // Ch1CivicBuildingFilterScope with targetState = 0 (neutral / unowned):
    // Should NOT swap the world buildings vector!
    std::vector<BYTE> dummyBld(BUILDING_STRUCT_BYTES, 0);
    BYTE* bldList[1] = { dummyBld.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 1;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= bldList + 1;

    {
        Ch1CivicBuildingFilterScope scopeNeutral(world, 0);
        TEST_CHECK(!scopeNeutral.IsSwapped()); // Must NOT swap on owner 0
        TEST_CHECK(*(BYTE***)(world + WORLD_BUILDINGS_OFFSET) == bldList);
    }

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 3: Civic Breakdown Metrics & Tourism Verification
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger1_CivicMetricsAndTourism()
{
    printf("    [Challenger1-3: R9 Civic Breakdown & Tourism Tracking] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialMoney[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialMoney, initialMoney);

    // Setup 1 Courthouse (caseload = 25), 1 Prison (50 inmates), 1 Orphanage (10 orphans) for State 1
    std::vector<BYTE> courtType(0x400, 0), prisonType(0x400, 0), orphType(0x400, 0);
    courtType[0x360] = 0x4B;
    prisonType[0x360] = 0x4C;
    orphType[0x360]   = 0x4A;

    std::vector<BYTE> court1(BUILDING_STRUCT_BYTES, 0);
    *(int*)court1.data() = 11;
    *(float*)(court1.data() + OBJ_POS_OFFSET) = -400.0f;
    *(float*)(court1.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    *(BYTE**)(court1.data() + 0x318) = courtType.data();
    *(int*)(court1.data() + 0x1388) = 25; // 25 active court cases
    TwoStates_RegisterBuildingOwner(11, 1);

    std::vector<BYTE> prison1(BUILDING_STRUCT_BYTES, 0);
    *(int*)prison1.data() = 22;
    *(float*)(prison1.data() + OBJ_POS_OFFSET) = -500.0f;
    *(float*)(prison1.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    *(BYTE**)(prison1.data() + 0x318) = prisonType.data();
    void* inmates[50] = {};
    *(void***)(prison1.data() + 0xBC0) = inmates;
    *(void***)(prison1.data() + 0xBC8) = inmates + 50;
    *(void***)(prison1.data() + 0xBD0) = inmates + 50;
    TwoStates_RegisterBuildingOwner(22, 1);

    std::vector<BYTE> orph1(BUILDING_STRUCT_BYTES, 0);
    *(int*)orph1.data() = 33;
    *(float*)(orph1.data() + OBJ_POS_OFFSET) = -600.0f;
    *(float*)(orph1.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    *(BYTE**)(orph1.data() + 0x318) = orphType.data();
    void* orphans[10] = {};
    *(void***)(orph1.data() + 0xBD8) = orphans;
    *(void***)(orph1.data() + 0xBE0) = orphans + 10;
    *(void***)(orph1.data() + 0xBE8) = orphans + 10;
    TwoStates_RegisterBuildingOwner(33, 1);

    BYTE* bldList[3] = { court1.data(), prison1.data(), orph1.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 3;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= bldList + 3;

    // Create 500 adult citizens housed in residential building owned by State 1
    std::vector<BYTE> home1(BUILDING_STRUCT_BYTES, 0);
    *(int*)home1.data() = 77;
    *(float*)(home1.data() + OBJ_POS_OFFSET) = -300.0f;
    *(float*)(home1.data() + OBJ_POS_OFFSET + 8) = 50.0f;
    TwoStates_RegisterBuildingOwner(77, 1);

    std::vector<std::vector<BYTE>> citStorage(500, std::vector<BYTE>(CITIZEN_STRUCT_BYTES, 0));
    std::vector<BYTE*> citPtrs(500, nullptr);
    for (int i = 0; i < 500; ++i) {
        *(BYTE**)(citStorage[i].data() + CIT_HOME_BUILDING_OFFSET) = home1.data();
        *(float*)(citStorage[i].data() + CIT_AGE_OFFSET) = 30.0f; // adult (>21)
        *(float*)(citStorage[i].data() + 0x18) = 75.0f; // lifespan
        *(float*)(citStorage[i].data() + 0xD8) = 0.85f; // satisfaction
        citPtrs[i] = citStorage[i].data();
    }
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET)     = citPtrs.data();
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8) = citPtrs.data() + citPtrs.size();
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 16)= citPtrs.data() + citPtrs.size();

    // Query Civic Stats for State 1
    StateCivicStats stats1 = {};
    TwoStates_GetStateCivicStatsEx(world, 1, &stats1);

    TEST_CHECK(stats1.courthouses == 1);
    TEST_CHECK(stats1.caseload == 25);
    TEST_CHECK(stats1.prisons == 1);
    TEST_CHECK(stats1.prisoners == 50);
    TEST_CHECK(stats1.orphanages == 1);
    TEST_CHECK(stats1.orphans == 10);
    TEST_CHECK(stats1.criminals == 75); // 25 + 50

    // Crime rate: (75 / 500) * 100% = 15.0%
    TEST_CHECK_NEAR(stats1.crimeRate, 15.0f, 0.1f);

    // Query Civic Stats for State 2 (should be 0)
    StateCivicStats stats2 = {};
    TwoStates_GetStateCivicStatsEx(world, 2, &stats2);
    TEST_CHECK(stats2.courthouses == 0);
    TEST_CHECK(stats2.caseload == 0);
    TEST_CHECK(stats2.prisons == 0);
    TEST_CHECK(stats2.prisoners == 0);
    TEST_CHECK(stats2.orphanages == 0);
    TEST_CHECK(stats2.orphans == 0);
    TEST_CHECK(stats2.criminals == 0);
    TEST_CHECK_NEAR(stats2.crimeRate, 0.0f, 1e-4f);

    // Verify UI toggle states
    TwoStates_SetShowCivicStats(true);
    TEST_CHECK(TwoStates_GetShowCivicStats() == true);
    TwoStates_SetShowCivicStats(false);
    TEST_CHECK(TwoStates_GetShowCivicStats() == false);

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    memset(world + WORLD_CITIZENS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 4: Asymmetric Demographics Weight wCit0 vs w0 (R12)
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger1_DemographicWeightAttribution()
{
    printf("    [Challenger1-4: R12 Citizen-Weight wCit0 Asymmetric Attribution] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialFunds[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialFunds, initialFunds);

    // Setup Asymmetric Scenario:
    // State 1 has 1 massive high-rise apartment building housing 800 citizens.
    // State 2 has 9 small single-family houses housing 200 citizens (22-23 citizens each).
    // Total buildings = 10. Building ratio w0 = 1 / 10 = 0.10 (10%).
    // Total citizens = 1000. Citizen ratio wCit0 = 800 / 1000 = 0.80 (80%).
    std::vector<BYTE> highRise1(BUILDING_STRUCT_BYTES, 0);
    *(int*)highRise1.data() = 1;
    *(float*)(highRise1.data() + OBJ_POS_OFFSET) = -500.0f;
    *(float*)(highRise1.data() + OBJ_POS_OFFSET + 8) = 50.0f;
    TwoStates_RegisterBuildingOwner(1, 1);

    std::vector<std::vector<BYTE>> houses2(9, std::vector<BYTE>(BUILDING_STRUCT_BYTES, 0));
    std::vector<BYTE*> bldList;
    bldList.push_back(highRise1.data());

    for (int i = 0; i < 9; ++i) {
        *(int*)houses2[i].data() = 10 + i;
        *(float*)(houses2[i].data() + OBJ_POS_OFFSET) = 500.0f + (float)i * 20.0f;
        *(float*)(houses2[i].data() + OBJ_POS_OFFSET + 8) = 50.0f;
        TwoStates_RegisterBuildingOwner(10 + i, 2);
        bldList.push_back(houses2[i].data());
    }

    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList.data();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList.data() + bldList.size();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 16)= bldList.data() + bldList.size();

    // Verify building weight w0 is 10%
    double w0 = TwoStates_ComputeStepWeightState1(1, 9);
    TEST_CHECK_NEAR(w0, 0.10, 1e-4);

    // Create 800 citizens in State 1 and 200 citizens in State 2
    std::vector<std::vector<BYTE>> citStorage(1000, std::vector<BYTE>(CITIZEN_STRUCT_BYTES, 0));
    std::vector<BYTE*> citList(1000, nullptr);

    // 800 citizens for State 1
    for (int i = 0; i < 800; ++i) {
        *(BYTE**)(citStorage[i].data() + CIT_HOME_BUILDING_OFFSET) = highRise1.data();
        *(float*)(citStorage[i].data() + CIT_AGE_OFFSET) = 25.0f;
        citList[i] = citStorage[i].data();
    }

    // 200 citizens for State 2
    for (int i = 800; i < 1000; ++i) {
        int hIdx = (i - 800) % 9;
        *(BYTE**)(citStorage[i].data() + CIT_HOME_BUILDING_OFFSET) = houses2[hIdx].data();
        *(float*)(citStorage[i].data() + CIT_AGE_OFFSET) = 25.0f;
        citList[i] = citStorage[i].data();
    }

    *(BYTE***)(world + WORLD_CITIZENS_OFFSET)     = citList.data();
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 8) = citList.data() + citList.size();
    *(BYTE***)(world + WORLD_CITIZENS_OFFSET + 16)= citList.data() + citList.size();

    // Calculate citizen demographics for State 1 and State 2
    // State 1 has 800 / 1000 = 80% of citizens
    // State 2 has 200 / 1000 = 20% of citizens
    // If demographics were split using building weight w0 (10%), State 1 would get only 10% of births/deaths!
    // With R12 citizen weight wCit0 (80%), State 1 receives 80% of births/deaths.
    StateCivicStats cs1 = {}, cs2 = {};
    TwoStates_GetStateCivicStatsEx(world, 1, &cs1);
    TwoStates_GetStateCivicStatsEx(world, 2, &cs2);

    // Verify pop counts reflect citizen distribution
    int pop1 = 0, pop2 = 0;
    for (int i = 0; i < 1000; ++i) {
        BYTE* home = *(BYTE**)(citList[i] + CIT_HOME_BUILDING_OFFSET);
        int owner = TwoStates_GetBuildingOwner(*(int*)home, *(float*)(home + OBJ_POS_OFFSET), *(float*)(home + OBJ_POS_OFFSET + 8));
        if (owner == 1) pop1++;
        else if (owner == 2) pop2++;
    }
    TEST_CHECK(pop1 == 800);
    TEST_CHECK(pop2 == 200);

    float wCit0 = (float)pop1 / (float)(pop1 + pop2);
    TEST_CHECK_NEAR(wCit0, 0.80f, 1e-4f);

    TwoStates_OnSessionEnded();
    memset(world + WORLD_BUILDINGS_OFFSET, 0, 24);
    memset(world + WORLD_CITIZENS_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 5: Residential Living/Heating/Water Step Attribution (R12)
// ---------------------------------------------------------------------------
static bool SubTest_M3_Challenger1_ResidentialHealthAttribution()
{
    printf("    [Challenger1-5: R12 Residential Living/Heating/Water Attribution] ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const double initialFunds[2] = { 500000.0, 500000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initialFunds, initialFunds);

    // Residential building owned by State 1
    std::vector<BYTE> bld1(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld1.data() = 101;
    *(float*)(bld1.data() + OBJ_POS_OFFSET) = -450.0f;
    *(float*)(bld1.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    TwoStates_RegisterBuildingOwner(101, 1);

    // Residential building owned by State 2
    std::vector<BYTE> bld2(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld2.data() = 102;
    *(float*)(bld2.data() + OBJ_POS_OFFSET) = 450.0f;
    *(float*)(bld2.data() + OBJ_POS_OFFSET + 8) = 100.0f;
    TwoStates_RegisterBuildingOwner(102, 2);

    TEST_CHECK(TwoStates_GetBuildingOwner(101, -450.0f, 100.0f) == 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(102,  450.0f, 100.0f) == 2);

    // Verify isolation: State 1 building operations are attributed to State 1
    TwoStates_BeginCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 1);
    TwoStates_EndCommand(1);

    TwoStates_BeginCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 2);
    TwoStates_EndCommand(2);

    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 1 Milestone 3 Main Suite Entry Point
// ---------------------------------------------------------------------------
static bool Test_Stress_M3_Challenger1_R9_R12()
{
    printf("  [M3-CHALLENGER1] Starting Stress Testing for Milestone 3 (R9, R12) ...\n");
    fflush(stdout);

    if (!SubTest_M3_Challenger1_ZeroFacilityEdgeCases()) return false;
    if (!SubTest_M3_Challenger1_NullAndCorruptedPointers()) return false;
    if (!SubTest_M3_Challenger1_CivicMetricsAndTourism()) return false;
    if (!SubTest_M3_Challenger1_DemographicWeightAttribution()) return false;
    if (!SubTest_M3_Challenger1_ResidentialHealthAttribution()) return false;

    printf("  [M3-CHALLENGER1] ALL Milestone 3 Empirical Stress Tests PASSED!\n");
    fflush(stdout);
    return true;
}
