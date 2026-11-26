// SPDX-License-Identifier: GPL-3.0-only
// Internal shared contract for the session/two_states/ modules.
// Not part of the public API (use session/two_states.h externally).
#pragma once

#include "../two_states.h"
#include "../save_sync.h"
#include "../mod_sync.h"
#include "../../core/diag_log.h"
#include "../../core/engine_rva.h"
#include "../../core/safe_mem.h"
#include "../../game/save_load.h"
#include "../../game/window_focus.h"
#include "../../lockstep/building_switches.h"
#include "../../lockstep/commands.h"
#include "../../lockstep/lockstep_net.h"
#include "../../world_sync/buildings/build_sync.h"
#include "../../world_sync/networks/road_sync.h"
#include "../../determinism/determinism.h"
#include "../../ui/lang.h"
#include "../../ui/native_menu.h"
#include "../../ui/sync_block.h"
#include "../../ui/ui_common.h"
#include "imgui.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Shared Constants
// ---------------------------------------------------------------------------

#define DLL_C3D "C3DDLL64.dll"
#define EXPANDED_BOUNDARY_MARGIN 150.0f
#define COOP_BANK_MARKER_NAME ".republicmp_coop_bank"
#define COOP_SAVE_META_NAME   "republicmp_coop.ini"

// ---------------------------------------------------------------------------
// Shared Types & Callback Typedefs
// ---------------------------------------------------------------------------

struct EngineResearchItemSlice {
    uint8_t available = 0;            // +0x11C
    float   progress = 0.0f;          // +0x120
    BYTE*   researchingBld = nullptr; // +0x128
    int32_t bldPosKey = 0;            // deterministic TwoStates_BuildingPosKey(x, z) of researchingBld
    int32_t timerE0 = 0;              // +0x130
};

struct StateEngineResearch {
    uint8_t unlocks[WORLD_RESEARCH_UNLOCKS_BYTES] = {};
    uint8_t spaceCargoUnlocks[2] = {}; // WORLD+0x11BA8 (west), WORLD+0x11BA9 (east)
    std::vector<EngineResearchItemSlice> items;
    uint64_t gen = 1;
    bool hasUnresolvedBldPtr = false;
    bool initialized = false;
    bool loadedFromMeta = false;
};

struct SpaceMilestoneTriple {
    int32_t built = 0;   // +0x9CE4 ($VEHICLE_BUILT_IN_PRODUCTION_LINE)
    int32_t orbit = 0;   // +0x9CE8 ($VEHICLE_SPACE_REACHED_ORBIT)
    int32_t reentry = 0; // +0x9CEC ($VEHICLE_SPACE_REENTRY_LANDED)
};

struct StateSpaceMilestones {
    std::unordered_map<std::string, SpaceMilestoneTriple> byKey;
    mutable std::vector<SpaceMilestoneTriple> cachedTriples;
    mutable uint64_t cachedGen = 0;
    mutable uint64_t cachedWorldGen = 0;
    uint64_t gen = 1;
    bool initialized = false;
    bool loadedFromMeta = false;
};

struct StateEngineVehLicenses {
    std::vector<std::string> ownedKeys;
    mutable std::vector<uint8_t> cachedFlags;
    mutable uint64_t cachedFlagsGen = 0;
    mutable uint64_t cachedFlagsWorldGen = 0;
    uint64_t gen = 1;
    bool initialized = false;
    bool loadedFromMeta = false;
};

struct PendingCameraRestore {
    volatile bool active;
    float pos[3];
    float look[3];
    int framesRemaining;
    int checkFrameCounter;
};

struct StateEconScalars {
    float   vehicles[4];         // +0x188..+0x197 ($Vehicles_ImportUSD/RUB, $Vehicles_ExportUSD/RUB)
    float   immigrantSpend[2];   // +0x240..+0x247 ($Economy_ImigrantCitySpendUSD/RUB)
    int32_t citizenCounts[5];    // +0x248..+0x25B ($Citizens_Born, Dead, Escaped, ImigrantSoviet, ImigrantAfrica)
    float   satisfaction[10];    // +0x25C..+0x283 (Health, Food, Clothes, Alcohol, Culture, Sport, Religion, Loyalty, Happiness, Electronics)
    float   citizenAverages[3];  // +0x2AC..+0x2B7 (Productivity, LifeExpectancy, AverageAge)
    int32_t demographics[4];     // +0x2B8..+0x2C7 ($Citizens_SmallChilds, MediumChilds, AdultsParent, Adults at +0x2C4)
    int32_t citizenBrackets[9];  // +0x2C8..+0x2EB (AdultsWithParents, EduNone, EduBasic, EduUniv, ElecNone, Radio, TV, Comp, Cars)
    int32_t extraCounts[21];     // +0x2EC..+0x33F (foreign worker border hires, tourist arrivals/departures/deaths, graduations)
    float   tourism[44];         // +0x340..+0x3EF ($Tourism_SpendUSD/RUB[16], Shops/Cinemas/Pubs/Sport/Hotels/Rides)
    float   loans[4];            // +0x3F0..+0x3FF ($Loan_Ballance_USD, $Loan_Interest_USD, $Loan_Ballance_RUB, $Loan_Interest_RUB)
};

struct StateEconPeriodStats {
    int32_t day = 0;
    int32_t year = 0;
    std::vector<SovietStatResEntry> resVecs[STAT_RES_VEC_COUNT];
    StateEconScalars scalars = {};
};

struct RawStatResVecHeader {
    SovietStatResEntry* begin;
    SovietStatResEntry* end;
    SovietStatResEntry* cap;
};
static_assert(sizeof(RawStatResVecHeader) == 24, "RawStatResVecHeader must be 24 bytes");

struct StatRecordUiBackup {
    RawStatResVecHeader resVecHeaders[STAT_RES_VEC_COUNT];
    StateEconScalars    scalars;
};

struct SavedCitizenLifespan {
    BYTE* cit;
    float lifespan;
};

struct ConstructResSnapshot {
    bool active = false;
    bool prevSuppress = false;
    std::vector<SovietStatResEntry> before[STAT_RES_VEC_COUNT];
};

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

typedef int (*ResolveEntityOwnerFn)(const BYTE* entity);
typedef __int64 (__fastcall* BldEconFn)(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef float (__fastcall* CollisionTerrainHeightFn)(BYTE* terrain, const float pos3[3]);

// ---------------------------------------------------------------------------
// Cross-Module Shared State (single definition in owning .cpp)
// ---------------------------------------------------------------------------

// Owned by two_states_core.cpp
extern volatile int g_chosenMode;
extern volatile int g_sessionMode;
extern volatile bool g_inSession;
extern volatile bool g_isHost;
extern float g_borderHalfWidth;
extern bool g_fullStartingCapital;
extern volatile bool g_expandBorders;
extern double g_stateRub[2];
extern double g_stateUsd[2];
extern double g_netTradeRub[2];
extern double g_netTradeUsd[2];
extern double g_lobbyBaseWorldRub;
extern double g_lobbyBaseWorldUsd;
extern bool g_haveLobbyBaseMoney;
extern bool g_treasuryInitialized;
extern bool g_loadedFromCoopSave;
extern CRITICAL_SECTION g_ownerLock;
extern std::unordered_map<int, int> g_buildingOwners;
extern std::unordered_map<int, int> g_vehicleOwners;
extern std::atomic<uint64_t> g_ownerMapGen;
extern std::atomic<uint64_t> g_vehOwnerMapGen;
extern bool g_stepUnlimitedMoney;
extern volatile bool g_inSimStep;
extern volatile DWORD g_simStepThread;
extern BYTE* volatile g_simLoopCurrentBuilding;
extern bool g_inCommand;
extern volatile DWORD g_cmdThread;
extern int g_cmdOwner;
extern bool g_beforeLoadPrepared;

// Owned by two_states_econ.cpp
extern StateEconPeriodStats g_stateCurEcon[2];
extern std::vector<StateEconPeriodStats> g_stateHistEcon[2];
extern uint64_t g_stateHistSwapGen;
extern bool g_stateEconInitialized;
extern StateTrackerSlice g_stateTrackerCur[2];
extern StateTrackerSlice g_stateTrackerOld[2];
extern bool g_stateTrackerInitialized;
extern double g_lastWorldRub;
extern double g_lastWorldUsd;
extern double g_unattributedRub;
extern double g_unattributedUsd;
extern StateEconScalars g_lastWorldScalars;
extern StateEconScalars g_unattributedScalars;
extern StateEconScalars g_cmdBeforeScalars;
extern StateTrackerSlice g_lastWorldTrackerCur;
extern StateTrackerSlice g_unattributedTrackerCur;
extern StateTrackerSlice g_cmdBeforeTrackerCur;
extern ConstructResSnapshot g_cmdResSnapshot;
extern int g_stepBldCount[2];

// Owned by two_states_research.cpp
extern StateResearchTree g_stateResearch[2];
extern StateEngineResearch g_engineResearch[2];
extern StateEngineVehLicenses g_engineVehLicenses[2];

// Owned by two_states_space.cpp
extern StateSpaceMilestones g_spaceMilestones[2];
extern std::vector<uint8_t> g_orbitOwners;

// Owned by two_states_overlay.cpp
extern bool g_show3DBorder;
extern bool g_showMinimapBorders;
extern bool g_hasSavedCam1;
extern float g_savedCam1Pos[3];
extern float g_savedCam1Look[3];
extern bool g_hasSavedCam2;
extern float g_savedCam2Pos[3];
extern float g_savedCam2Look[3];
extern PendingCameraRestore g_pendingCamRestore;
extern bool g_hasResyncCam;
extern float g_resyncCamPos[3];
extern float g_resyncCamLook[3];
extern bool g_localPingActive;
extern CollisionTerrainHeightFn g_collisionTerrainHeight;

// ---------------------------------------------------------------------------
// Per-Module Host Bindings & Hook Installers (called by two_states_core.cpp)
// ---------------------------------------------------------------------------

void TwoStates_BindEcon(const TsmHost* host);        // two_states_econ.cpp
void TwoStates_BindSimHooks(const TsmHost* host);    // two_states_sim_hooks.cpp
void TwoStates_BindResearch(const TsmHost* host);    // two_states_research.cpp
void TwoStates_BindOverlay(const TsmHost* host);     // two_states_overlay.cpp
void TwoStates_BindPersistence(const TsmHost* host); // two_states_persistence.cpp
void TwoStates_BindSpace(const TsmHost* host);       // two_states_space.cpp

int TwoStates_InstallTerrainIatHooks();  // two_states_sim_hooks.cpp (returns 0..4)
int TwoStates_InstallEconHooks();        // two_states_econ.cpp
int TwoStates_InstallSimHooks();         // two_states_sim_hooks.cpp
int TwoStates_InstallResearchHooks();    // two_states_research.cpp
int TwoStates_InstallOverlayHooks();     // two_states_overlay.cpp
int TwoStates_InstallPersistenceHooks(); // two_states_persistence.cpp
int TwoStates_InstallSpaceHooks();       // two_states_space.cpp
bool TwoStates_AreEconHooksInstalled();  // two_states_econ.cpp

// ---------------------------------------------------------------------------
// Thread-Local Econ / Ledger State Accessors (implemented in two_states_econ.cpp)
// ---------------------------------------------------------------------------

bool TwoStates_IsInEconHook();
int  TwoStates_GetEconOwner();
bool TwoStates_IsCurStatLedgerSuppressed();
void TwoStates_SetCurStatLedgerSuppressed(bool suppressed);

// ---------------------------------------------------------------------------
// Cross-Module Shared Helpers
// ---------------------------------------------------------------------------

// Owned by two_states_core.cpp
struct BldPtrOwnerSlot {
    const BYTE* ptr;
    uint8_t     owner;
    uint8_t     pad[7];
};
static_assert(sizeof(BldPtrOwnerSlot) == 16, "BldPtrOwnerSlot must be 16 bytes");

static inline size_t HashBldPtr(const BYTE* p)
{
    uintptr_t x = reinterpret_cast<uintptr_t>(p);
    x ^= (x >> 33);
    x *= 0xff51afd7ed558ccdULL;
    x ^= (x >> 33);
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= (x >> 33);
    return static_cast<size_t>(x);
}

void EnsureOwnerLock();
void TwoStates_BumpOwnerMapGen();
void TwoStates_BumpVehOwnerMapGen();
void TwoStates_ClearEntityOwners();
void EnsureBldPtrOwnerCache(const BYTE* world);
void TwoStates_GetActiveBldPtrCacheView(const BldPtrOwnerSlot** outSlots, size_t* outMask);
int  LookupBuildingOwnerCached(const BYTE* bld);
double* WorldRubles();
double* WorldDollars();
bool IsUnlimitedMoney();
int GetWorldVehicleCount();
bool SwapWorldPtrVecForState(BYTE* world, size_t vecOff, int targetState,
                             ResolveEntityOwnerFn resolveFn, ptrdiff_t maxCount,
                             uintptr_t outSavedHdr[3], std::vector<BYTE*>& outFiltered);
int ResolveBuildingOwnerFast(const BYTE* b);
int ResolveVehicleOwnerFast(const BYTE* v);
int ResolveCitizenOwnerFast(const BYTE* cit);
int ResolveCitizenOwnerCached(const BYTE* cit);
int ResolveCitizenOwnerRawUnsafe(const BYTE* cit, const BYTE* home, const BYTE* parent,
                                 const BldPtrOwnerSlot* slots, size_t mask);

static inline int LookupBldPtrOwnerInSlots(const BldPtrOwnerSlot* slots, size_t mask, const BYTE* bld)
{
    if (!bld || !IsPointerLikeX64((uintptr_t)bld)) return 1;
    if (slots) {
        size_t idx = HashBldPtr(bld) & mask;
        for (size_t probe = 0; probe <= mask; ++probe) {
            const BYTE* p = slots[idx].ptr;
            if (p == bld) return static_cast<int>(slots[idx].owner);
            if (p == nullptr) break;
            idx = (idx + 1) & mask;
        }
    }
    return ResolveBuildingOwnerFast(bld);
}

// Owned by two_states_econ.cpp
void EnsureStateEconInitialized(const BYTE* world, BYTE** outHistBegin = nullptr, int* outHistCount = nullptr);
void TwoStates_BeginSubStepEconRead(const BYTE* world);
void TwoStates_BumpHistSwapGen();
void TwoStates_FlushEconHookPerf();
void TwoStates_ResetEconBeforeCursor();
void TwoStates_InvalidateUiCitizenCache();
void TwoStates_InvalidateHistBackfillCache();
void TwoStates_InvalidateUiHistBackupCache();
void TwoStates_ComputeStepDemographicsAndPrimeUiCache(const BYTE* world, StateEconScalars* outS0,
                                                       StateEconScalars* outS1, float* outWCit0 = nullptr);
void TwoStates_AttributeHealthCauseDirect(BYTE* world, int owner, int causeIdx, float delta);
void ResetStateHudPopCache();
void ResetStateTrackerSlices();
bool BeginEconAttribution(BYTE* world, int owner);
void EndEconAttribution(int owner, bool countInNetTrade = true);
double SanitizeMoneyStep(double oldBal, double newBal);
void SanitizeBuildingEffectiveCapFloat(BYTE* b);
void SanitizeAllWorldBuildingsEffectiveCap(const BYTE* world);
__int64 CallBuildingEconHook(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                             uintptr_t a5, uintptr_t a6, BldEconFn orig);
void BeginConstructResSnapshot(const BYTE* world, int owner, ConstructResSnapshot* snap);
void EndConstructResSnapshot(const BYTE* world, int owner, const ConstructResSnapshot& snap);
void SafeReadStatScalars(const BYTE* rec, StateEconScalars* out);
void SafeWriteStatScalars(BYTE* rec, const StateEconScalars& src);
void AddStatScalarsDelta(StateEconScalars* dst, const StateEconScalars& before,
                         const StateEconScalars& after, float scale = 1.0f);
void SafeReadTrackerSlice(const BYTE* world, bool readOld, StateTrackerSlice* out);
void AddTrackerSliceDelta(StateTrackerSlice* dst, const StateTrackerSlice& before,
                          const StateTrackerSlice& after, float scale = 1.0f);
bool SafeReadStatHistHeader(const BYTE* world, BYTE** outBegin, int* outCount);
void AddEntryDeltaToStateResVec(std::vector<SovietStatResEntry>& vec, uintptr_t resPtr,
                                bool isNegativeRow, float dTons, float dVal);
void ClearStateEconPeriod(StateEconPeriodStats* st, int32_t day = 0, int32_t year = 0);
void ComputeBothStatesCitizenDemographics(const BYTE* world, StateEconScalars* outS0, StateEconScalars* outS1);
void ComputeStateCitizenDemographics(const BYTE* world, int state, StateEconScalars* ioScalars);
void FlushUnattributedScalarsToStates(const BYTE* world, bool trackerFromOld = false, float precomputedWCit0 = -1.0f);
void TwoStates_GetStateCivicStatsEx(const BYTE* world, int state, StateCivicStats* outStats);

template <BldEconFn* OrigPtr>
inline __int64 __fastcall H_GenericBldEconStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                               uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, *OrigPtr);
}

// Owned by two_states_sim_hooks.cpp
int ResolveCustomsTourismOwner(const BYTE* customsBld);
int ResolveCustomsTourismOwnerRawUnsafe(const BYTE* customsBld, const BldPtrOwnerSlot* slots, size_t mask);

// Owned by two_states_research.cpp
bool SafeReadStridedWorldVecHeader(const BYTE* world, size_t vecOffset, size_t elemStride, int maxCount,
                                   BYTE** outBegin, int* outCount);
bool IsTwoStatesResearchOrLicenseActive();
int ResolveActiveResearchWorldState();
int ComputeBuildingPosKeySafe(const BYTE* bld);
void TwoStates_RecomputeAndCaptureResearchForState(BYTE* world, int stateOwner);
void EnsureEngineResearchInitialized(BYTE* world);
void EnsureEngineVehLicensesInitialized(BYTE* world);
void CaptureWorldResearchToSlice(const BYTE* world, StateEngineResearch* dst, int filterOwnerState);
void WriteSliceToWorldResearch(BYTE* world, StateEngineResearch* src);
void ResetEngineResearchSlices();
void SyncSummaryResearchFromSlice(int stateOwner, const StateEngineResearch& src);
void CaptureWorldVehLicensesToSlice(const BYTE* world, StateEngineVehLicenses* dst);
void WriteSliceToWorldVehLicenses(BYTE* world, const StateEngineVehLicenses* src);
void ResetEngineVehLicenseSlices();
void AddLicenseKeyToSlice(StateEngineVehLicenses* slice, const std::string& key);
void TwoStates_InvalidateResearchMountCache();
void TwoStates_InvalidateMountCaches();

// Owned by two_states_space.cpp
void TwoStates_InvalidateSpaceMountCache();
void EnsureSpaceMilestonesInitialized(BYTE* world);
void CaptureWorldSpaceMilestonesToSlice(const BYTE* world, StateSpaceMilestones* dst);
void WriteSliceToWorldSpaceMilestones(BYTE* world, const StateSpaceMilestones* src, int stateOwner);
void TwoStates_MountSpaceMilestonesForState(BYTE* world, int stateOwner);
void ResetSpaceStateSlices();
void TwoStates_ReconcileOrbitOwnersFromWorld(const BYTE* world);
bool TwoStates_TryGetOrbitOwner(int orbitIdx, int* outOwner);
std::string SerializeSpaceMilestones(const StateSpaceMilestones& slice);
void ParseSpaceMilestones(const std::string& val, StateSpaceMilestones* dst);
std::string SerializeOrbitOwners();
void ParseOrbitOwners(const std::string& val);

// Owned by two_states_overlay.cpp
void ResetSavedCameraState();
bool IsValidCameraPair(const float pos[3], const float look[3]);

// Owned by two_states_persistence.cpp
void SafeWriteWorldMoney(double rub, double usd);
