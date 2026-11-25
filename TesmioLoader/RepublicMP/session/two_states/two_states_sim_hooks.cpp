// SPDX-License-Identifier: GPL-3.0-only
#include "two_states_internal.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

void TwoStates_BindSimHooks(const TsmHost* host) { if (host) TsmBind(host); }

// ---------------------------------------------------------------------------
// Terrain Boundary IAT Hooks
// ---------------------------------------------------------------------------

typedef float (__fastcall* TerrainBoundaryFn)(BYTE* terrain);
typedef float (__fastcall* CollisionTerrainHeightFn)(BYTE* terrain, const float pos3[3]);
static TerrainBoundaryFn o_getBoundaryXMin = nullptr;
static TerrainBoundaryFn o_getBoundaryXMax = nullptr;
static TerrainBoundaryFn o_getBoundaryZMin = nullptr;
static TerrainBoundaryFn o_getBoundaryZMax = nullptr;

static void ApplyRuntimeTerrainBounds(BYTE* terrain)
{
    if (!terrain) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (g_expandBorders) {
            if (*(float*)(terrain + 0x8DC) > EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8DC) = EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E0) > EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E0) = EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E4) < -EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E4) = -EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E8) < -EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E8) = -EXPANDED_BOUNDARY_MARGIN;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static float __fastcall H_GetTerrainBoundaryXMin(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryXMin ? o_getBoundaryXMin(terrain) : EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryXMax(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryXMax ? o_getBoundaryXMax(terrain) : -EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryZMin(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryZMin ? o_getBoundaryZMin(terrain) : EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryZMax(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryZMax ? o_getBoundaryZMax(terrain) : -EXPANDED_BOUNDARY_MARGIN;
}

// ---------------------------------------------------------------------------
// Simulation Hook Constants & Trampolines
// ---------------------------------------------------------------------------

#define RVA_COURT_ASSIGN            0x270C90
#define RVA_PRISON_ASSIGN           0x871170
#define RVA_ORPHANAGE_ASSIGN        0x871AE0

static const BYTE kCitHomeStepExpect[18] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x49, 0x8B, 0x40, 0x20, 0x48, 0x8B, 0xDA, 0x0F, 0x29, 0x74, 0x24, 0x50 };
static const BYTE kCitizenChurchStepExpect[15] = { 0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0x6B, 0xA1, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00 };
static const BYTE kCitizenStudioEffectExpect[14] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0xDA, 0x0F, 0x29, 0x7C, 0x24, 0x20 };
static const BYTE kCitizenStudioSelectExpect[16] = { 0x40, 0x56, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x58, 0xF3, 0x0F, 0x10, 0x8A, 0xFC, 0x00, 0x00, 0x00 };
static const BYTE kCitSevereIllnessTickExpect[26] = {
    0xF3, 0x0F, 0x10, 0x05, 0x53, 0xAD, 0x1B, 0x00, 0xFF, 0x05, 0x7D, 0xAD, 0x1B, 0x00, 0xF3, 0x0F, 0x58, 0xC1, 0xF3, 0x0F, 0x11, 0x05, 0x41, 0xAD, 0x1B, 0x00
};
static const BYTE kCitizenBirthExpect[15] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x50 };
static const BYTE kCitizenStarveStepExpect[15] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00 };
static const BYTE kCitizenRemoveExpect[18] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
static const BYTE kCitizenEvalNeedsExpect[14] = { 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
static const BYTE kCitizenCanMoveToBldExpect[19] = { 0x80, 0xBA, 0x14, 0x14, 0x00, 0x00, 0x01, 0x75, 0x03, 0x32, 0xC0, 0xC3, 0x80, 0xBA, 0x17, 0x14, 0x00, 0x00, 0x01 };
static const BYTE kCitizenMoveToBldExpect[17] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
static const BYTE kBldSimLoopHeadExpect[16] = {
    0x48, 0x8B, 0x86, 0xB8, 0x1B, 0x01, 0x00, 0x48, 0x8B, 0x14, 0xC8, 0x48, 0x89, 0x54, 0x24, 0x40
};
static const BYTE kBldSimLoopTailExpect[17] = {
    0x45, 0x03, 0xF7, 0x48, 0x8B, 0x86, 0xC0, 0x1B, 0x01, 0x00, 0x48, 0x2B, 0x86, 0xB8, 0x1B, 0x01, 0x00
};
static const BYTE kCourtAssignExpect[17] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x42, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x2B, 0x42, 0x28 };
static const BYTE kPrisonAssignExpect[16] = { 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41 };
static const BYTE kOrphanageAssignExpect[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57 };
static const BYTE kLivingTickExpect[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55 };
static const BYTE kWaterHealthStepExpect[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55 };
static const BYTE kHeatingStepExpect[17] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x83, 0xB9, 0xD8, 0x05, 0x00, 0x00, 0x00 };
static const BYTE kBldCourthouseStepExpect[19] = { 0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };
static const BYTE kBldPoliceStepExpect[19] = { 0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };
static const BYTE kBldHotelStepExpect[19] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20 };
static const BYTE kBldAttraction1StepExpect[22] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00 };
static const BYTE kBldAttraction2StepExpect[22] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00 };
static const BYTE kCalcCustomsTouristSpawnExpect[14] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60 };
static const BYTE kBldCustomsTouristStepExpect[15] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00 };
static const BYTE kCitizenCommitCrimeExpect[15] = { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20 };
static const BYTE kCrimeExpireCaseExpect[14] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00 };
static const BYTE kCitizenFootArriveExpect[20] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x57, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x50, 0xC6, 0x82, 0x23, 0x07, 0x00, 0x00, 0x00 };
static const BYTE kCitizenVehicleAlightExpect[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
static const BYTE kAssignConstructionOfficeExpect[16] = { 0x48, 0x8B, 0xC4, 0xF3, 0x0F, 0x11, 0x58, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10 };
static const BYTE kBldCountLimitCheckExpect[15] = {
    0x4C, 0x8B, 0xCA, 0x48, 0x85, 0xD2, 0x0F, 0x84,
    0x8D, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x11
};
static const BYTE kBldCountLimitGetExpect[21] = {
    0x4C, 0x8B, 0xD2, 0x4D, 0x85, 0xC0, 0x74, 0x45,
    0x4C, 0x8B, 0x19, 0x48, 0xB8, 0xAB, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA, 0xAA, 0x2A
};
static const BYTE kBldCountLimitTooltipExpect[17] = {
    0x49, 0x8B, 0x9F, 0x80, 0x00, 0x00, 0x00, 0x45, 0x33, 0xD2, 0x48, 0x8B, 0x3B, 0x48, 0x8B, 0x4B, 0x08
};

typedef bool (__fastcall* CourtAssignFn)(BYTE* world, BYTE* caseObj, uintptr_t a3, uintptr_t a4);
typedef bool (__fastcall* PrisonAssignFn)(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef bool (__fastcall* OrphanageAssignFn)(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4);
typedef void (__fastcall* CitizenCommitCrimeFn)(BYTE* world, BYTE* citizen, int32_t severity,
                                                uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* CrimeExpireCaseFn)(BYTE* world, BYTE* caseObj, float a3,
                                             uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* CitizenFootArriveFn)(BYTE* world, BYTE* citizen, unsigned char flag,
                                               uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef unsigned char (__fastcall* CitizenVehicleAlightFn)(BYTE* world, BYTE* citizen, BYTE* stationBld,
                                                           unsigned char flag, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* AssignConstructionOfficeFn)(BYTE* world, BYTE* officeBld, BYTE* targetBld, float dist,
                                                      unsigned char checkFilters, unsigned char isDemolishOffice);
typedef __int64 (__fastcall* CalcCustomsTouristSpawnFn)(BYTE* world, BYTE* customsBld, float* outAvgRating, int32_t* outSpawnCount);
typedef float (__fastcall* WaterHealthStepFn)(BYTE* world, BYTE* building, float waterNeeded,
                                              unsigned char flag4, BYTE* citizen,
                                              unsigned char flag6, float factor7);
typedef void (__fastcall* CitHomeStepFn)(BYTE* world, BYTE* building, BYTE* citizen, float dt, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* CitizenChurchStepFn)(BYTE* world, BYTE* citizen);
typedef void (__fastcall* CitizenStudioEffectFn)(BYTE* world, BYTE* studio, BYTE* citizen, float dt);
typedef BYTE* (__fastcall* CitizenStudioSelectFn)(BYTE* world, BYTE* citizen);
typedef void (__fastcall* CitizenBirthFn)(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef unsigned char (__fastcall* CitizenStarveStepFn)(BYTE* world, BYTE* citizen);
typedef void (__fastcall* CitizenRemoveFn)(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* CitizenEvalNeedsFn)(BYTE* world, BYTE* citizen);
typedef unsigned char (__fastcall* CitizenCanMoveToBldFn)(BYTE* world, BYTE* targetBld, BYTE* citizen);
typedef void (__fastcall* CitizenMoveToBldFn)(BYTE* world, BYTE* citizen, BYTE* targetBld);

static CitHomeStepFn o_citHomeStep = nullptr;
static CitizenChurchStepFn o_citizenChurchStep = nullptr;
static CitizenStudioEffectFn o_citizenStudioEffect = nullptr;
static CitizenStudioSelectFn o_citizenStudioSelect = nullptr;
static CitizenBirthFn o_citizenBirth = nullptr;
static CitizenStarveStepFn o_citizenStarveStep = nullptr;
static CitizenRemoveFn o_citizenRemove = nullptr;
static CitizenEvalNeedsFn o_citizenEvalNeeds = nullptr;
static CitizenCanMoveToBldFn o_citizenCanMoveToBld = nullptr;
static CitizenMoveToBldFn o_citizenMoveToBld = nullptr;
static CourtAssignFn o_courtAssign = nullptr;
static PrisonAssignFn o_prisonAssign = nullptr;
static OrphanageAssignFn o_orphanageAssign = nullptr;
static BldEconFn o_livingTick = nullptr;
static WaterHealthStepFn o_waterHealthStep = nullptr;
static BldEconFn o_heatingStep = nullptr;
static BldEconFn o_bldCourthouseStep = nullptr;
static BldEconFn o_bldPoliceStep = nullptr;
static BldEconFn o_bldHotelStep = nullptr;
static BldEconFn o_bldAttraction1Step = nullptr;
static BldEconFn o_bldAttraction2Step = nullptr;
static CalcCustomsTouristSpawnFn o_calcCustomsTouristSpawn = nullptr;
static BldEconFn o_bldCustomsTouristStep = nullptr;
static CitizenCommitCrimeFn o_citizenCommitCrime = nullptr;
static CrimeExpireCaseFn o_crimeExpireCase = nullptr;
static CitizenFootArriveFn o_citizenFootArrive = nullptr;
static CitizenVehicleAlightFn o_citizenVehicleAlight = nullptr;
static AssignConstructionOfficeFn o_assignConstructionOffice = nullptr;

// ---------------------------------------------------------------------------
// Customs Tourism Ownership Helpers
// ---------------------------------------------------------------------------

static int ReadCustomsBorderVisaUnlockIdxRaw(const BYTE* customsBld)
{
    if (!customsBld) return -1;
    const BYTE* bldType = *(const BYTE* const*)(customsBld + 0x318);
    if (!IsPointerLikeX64((uintptr_t)bldType)) return -1;
    const uint8_t cat = *(const uint8_t*)(bldType + BLD_TYPE_CAT_OFF);
    if (cat != 0x13 && cat != 0x14) return -1;
    const uint8_t subtype = *(const uint8_t*)(bldType + BLD_TYPE_CUSTOMS_SUBTYPE_OFF);
    return (subtype == 1) ? 11 : 12;
}

static int ReadCustomsBorderVisaUnlockIdx(const BYTE* customsBld)
{
    if (!customsBld) return -1;
    int visaIdx = -1;
    DiagLog_BeginSafeSeh();
    __try {
        visaIdx = ReadCustomsBorderVisaUnlockIdxRaw(customsBld);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        visaIdx = -1;
    }
    DiagLog_EndSafeSeh();
    return visaIdx;
}

static inline int ResolveCustomsTourismOwnerFromPos(const BYTE* customsBld, int posOwner, int visaIdx)
{
    if (!IsTwoStatesResearchOrLicenseActive()) return posOwner;
    if (visaIdx < 0 || visaIdx >= (int)WORLD_RESEARCH_UNLOCKS_BYTES) return posOwner;
    int otherOwner = (posOwner == 1) ? 2 : 1;
    if (g_engineResearch[posOwner - 1].initialized && g_engineResearch[otherOwner - 1].initialized) {
        if (g_engineResearch[posOwner - 1].unlocks[visaIdx] == 0 &&
            g_engineResearch[otherOwner - 1].unlocks[visaIdx] != 0) {
            return otherOwner;
        }
    }
    return posOwner;
}

int ResolveCustomsTourismOwner(const BYTE* customsBld)
{
    return ResolveCustomsTourismOwnerFromPos(
        customsBld, LookupBuildingOwnerCached(customsBld), ReadCustomsBorderVisaUnlockIdx(customsBld));
}

int ResolveCustomsTourismOwnerRawUnsafe(const BYTE* customsBld, const BldPtrOwnerSlot* slots, size_t mask)
{
    return ResolveCustomsTourismOwnerFromPos(
        customsBld, LookupBldPtrOwnerInSlots(slots, mask, customsBld), ReadCustomsBorderVisaUnlockIdxRaw(customsBld));
}

// ---------------------------------------------------------------------------
// Citizen, Civic, Crime, Water, Heating & Tourism Hooks
// ---------------------------------------------------------------------------

struct CitizenEconScope {
    int owner = 0;
    bool active = false;

    CitizenEconScope(BYTE* world, const BYTE* building, const BYTE* citizen)
    {
        if (g_inSimStep && !TwoStates_IsInEconHook()) {
            owner = building ? LookupBuildingOwnerCached(building) : ResolveCitizenOwnerCached(citizen);
            active = owner ? BeginEconAttribution(world, owner) : false;
        }
    }

    ~CitizenEconScope()
    {
        if (active) EndEconAttribution(owner);
    }
};

static void __fastcall H_CitHomeStep(BYTE* world, BYTE* building, BYTE* citizen, float dt,
                                     uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, building, citizen);
    if (o_citHomeStep) o_citHomeStep(world, building, citizen, dt, a5, a6);
}

static void AttributePendingTouristHotelStay(BYTE* world, const BYTE* citizen)
{
    if (!g_inSimStep || g_inCommand || TwoStates_IsInEconHook() || !TwoStates_IsActive() || !world || !citizen) return;
    if (world != g_exeBase + WORLD_RVA || GetCurrentThreadId() != g_simStepThread) return;
    int32_t touristType = 0;
    if (!SafeReadPod((const int32_t*)(citizen + CIT_INFANT_FLAG_OFFSET), &touristType) || touristType == 0) return;
    int owner = ResolveCitizenOwnerCached(citizen);
    if (owner != 1 && owner != 2) return;
    int idx = owner - 1;
    const bool unlim = g_stepUnlimitedMoney;
    DiagLog_BeginSafeSeh();
    __try {
        if (!unlim) {
            double curRub = *WorldRubles();
            double curUsd = *WorldDollars();
            double dRub = curRub - g_lastWorldRub;
            double dUsd = curUsd - g_lastWorldUsd;
            if (isfinite(dRub) && fabs(dRub) <= 1.0e9) {
                g_stateRub[idx] += dRub;
                g_netTradeRub[idx] += dRub;
            }
            if (isfinite(dUsd) && fabs(dUsd) <= 1.0e9) {
                g_stateUsd[idx] += dUsd;
                g_netTradeUsd[idx] += dUsd;
            }
            g_lastWorldRub = curRub;
            g_lastWorldUsd = curUsd;
        }
        StateEconScalars curScalars = {};
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &curScalars);
        AddStatScalarsDelta(&g_stateCurEcon[idx].scalars, g_lastWorldScalars, curScalars, 1.0f);
        g_lastWorldScalars = curScalars;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void __fastcall H_CitizenBirth(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4,
                                      uintptr_t a5, uintptr_t a6)
{
    AttributePendingTouristHotelStay(world, citizen);
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenBirth) o_citizenBirth(world, citizen, a3, a4, a5, a6);
}

static void __fastcall H_CitizenChurchStep(BYTE* world, BYTE* citizen)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenChurchStep) o_citizenChurchStep(world, citizen);
}

static void __fastcall H_CitizenRemove(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4,
                                       uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenRemove) o_citizenRemove(world, citizen, a3, a4, a5, a6);
}

static void __fastcall H_CitizenEvalNeeds(BYTE* world, BYTE* citizen)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenEvalNeeds) o_citizenEvalNeeds(world, citizen);
}

static void AccumulateMockTrackerCause(BYTE* world, size_t sumBaseOff, size_t cntBaseOff,
                                       int causeIdx, int maxCount, float delta)
{
    if (!world || causeIdx < 0 || causeIdx >= maxCount || delta == 0.0f) return;
    *(float*)(world + sumBaseOff + (size_t)causeIdx * sizeof(float)) += delta;
    *(int32_t*)(world + cntBaseOff + (size_t)causeIdx * sizeof(int32_t)) += 1;
}

static void __fastcall H_CitizenSevereIllnessTick(BYTE* world, float delta, BYTE* citizen)
{
    if (!world || delta == 0.0f) return;
    const bool canAttribute =
        TwoStates_IsActive() && g_inSimStep && !g_inCommand &&
        (!Determinism_IsActive() || Determinism_InStepFrame()) &&
        (!g_exeBase || world == g_exeBase + WORLD_RVA) &&
        (GetCurrentThreadId() == g_simStepThread);
    if (!canAttribute) {
        TwoStates_AttributeHealthCauseDirect(world, 0, 9, delta);
        return;
    }
    const int owner = TwoStates_IsInEconHook()
        ? TwoStates_GetEconOwner()
        : ResolveCitizenOwnerCached(citizen);
    TwoStates_AttributeHealthCauseDirect(world, owner, 9, delta);
}

void TwoStates_CitizenSevereIllnessTickForTest(BYTE* world, float delta, BYTE* citizen)
{
    H_CitizenSevereIllnessTick(world, delta, citizen);
}

static float g_mockOrphanHappyDelta = 0.0f;
static float g_mockParentGriefDelta = 0.0f;

static void __fastcall MockEngineCitizenRemoveDeath(BYTE* world, BYTE* /*citizen*/, unsigned char /*a3*/,
                                                    unsigned char reason, uintptr_t /*a5*/, uintptr_t /*a6*/)
{
    if (!world || reason != 1) return;
    *(int32_t*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_COUNTS_OFF + 1 * sizeof(int32_t)) += 1;
    AccumulateMockTrackerCause(world, WORLD_TRACKER_HAPPY_CUR_SUM_OFF, WORLD_TRACKER_HAPPY_CUR_CNT_OFF,
                               11, TRACKER_HAPPY_CAUSE_COUNT, g_mockOrphanHappyDelta);
    AccumulateMockTrackerCause(world, WORLD_TRACKER_HAPPY_CUR_SUM_OFF, WORLD_TRACKER_HAPPY_CUR_CNT_OFF,
                               12, TRACKER_HAPPY_CAUSE_COUNT, g_mockParentGriefDelta);
}

void TwoStates_CitizenRemoveDeathForTest(BYTE* world, BYTE* citizen, float orphanHappyDelta, float parentGriefDelta)
{
    CitizenRemoveFn prev = o_citizenRemove;
    o_citizenRemove = &MockEngineCitizenRemoveDeath;
    g_mockOrphanHappyDelta = orphanHappyDelta;
    g_mockParentGriefDelta = parentGriefDelta;
    H_CitizenRemove(world, citizen, 0, 1, 0, 0);
    o_citizenRemove = prev;
}

static int   g_mockHealthCauseIdx = -1;
static float g_mockHealthDelta = 0.0f;
static int   g_mockHappyCauseIdx = -1;
static float g_mockHappyDelta = 0.0f;

static void __fastcall MockEngineCitizenEvalNeeds(BYTE* world, BYTE* /*citizen*/)
{
    if (!world) return;
    AccumulateMockTrackerCause(world, WORLD_TRACKER_HEALTH_CUR_SUM_OFF, WORLD_TRACKER_HEALTH_CUR_CNT_OFF,
                               g_mockHealthCauseIdx, TRACKER_HEALTH_CAUSE_COUNT, g_mockHealthDelta);
    AccumulateMockTrackerCause(world, WORLD_TRACKER_HAPPY_CUR_SUM_OFF, WORLD_TRACKER_HAPPY_CUR_CNT_OFF,
                               g_mockHappyCauseIdx, TRACKER_HAPPY_CAUSE_COUNT, g_mockHappyDelta);
}

void TwoStates_CitizenEvalNeedsForTest(BYTE* world, BYTE* citizen, int healthCauseIdx, float healthDelta,
                                       int happyCauseIdx, float happyDelta)
{
    CitizenEvalNeedsFn prev = o_citizenEvalNeeds;
    o_citizenEvalNeeds = &MockEngineCitizenEvalNeeds;
    g_mockHealthCauseIdx = healthCauseIdx;
    g_mockHealthDelta = healthDelta;
    g_mockHappyCauseIdx = happyCauseIdx;
    g_mockHappyDelta = happyDelta;
    H_CitizenEvalNeeds(world, citizen);
    o_citizenEvalNeeds = prev;
}

static const BYTE* ResolveCourtCaseCitizen(const BYTE* caseObj)
{
    if (!IsPointerLikeX64((uintptr_t)caseObj)) return nullptr;
    const BYTE* cit = nullptr;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* vecBegin = *(const BYTE* const*)(caseObj + 0x28);
        const BYTE* vecEnd   = *(const BYTE* const*)(caseObj + 0x30);
        if (IsPointerLikeX64((uintptr_t)vecBegin) && vecEnd > vecBegin) {
            const BYTE* firstCit = *(const BYTE* const*)vecBegin;
            if (IsPointerLikeX64((uintptr_t)firstCit)) {
                cit = firstCit;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cit = nullptr;
    }
    DiagLog_EndSafeSeh();
    return cit;
}

static int ResolveCrimeCaseOwner(const BYTE* caseObj)
{
    if (!IsPointerLikeX64((uintptr_t)caseObj)) return 1;
    const BYTE* crimeBld = nullptr;
    if (SafeReadPod((const BYTE* const*)(caseObj + 0x08), &crimeBld) && IsPointerLikeX64((uintptr_t)crimeBld)) {
        return LookupBuildingOwnerCached(crimeBld);
    }
    const BYTE* policeOrCourtBld = nullptr;
    if (SafeReadPod((const BYTE* const*)(caseObj + 0x10), &policeOrCourtBld) && IsPointerLikeX64((uintptr_t)policeOrCourtBld)) {
        return LookupBuildingOwnerCached(policeOrCourtBld);
    }
    const BYTE* cit = ResolveCourtCaseCitizen(caseObj);
    return cit ? ResolveCitizenOwnerCached(cit) : 1;
}

int TwoStates_ResolveCrimeCaseOwnerForTest(const BYTE* caseObj)
{
    return ResolveCrimeCaseOwner(caseObj);
}

class CivicBuildingFilterScope {
private:
    BYTE* m_world = nullptr;
    size_t m_vecOff = WORLD_BUILDINGS_OFFSET;
    uintptr_t m_savedHdr[3] = {};
    std::vector<BYTE*> m_filtered;
    bool m_swapped = false;

public:
    CivicBuildingFilterScope(BYTE* world, int targetState,
                             size_t vecOff = WORLD_BUILDINGS_OFFSET,
                             ResolveEntityOwnerFn resolveFn = &LookupBuildingOwnerCached)
        : m_vecOff(vecOff)
    {
        if (TwoStates_IsActive() && world && (targetState == 1 || targetState == 2)) {
            m_world = world;
            m_swapped = SwapWorldPtrVecForState(world, vecOff, targetState,
                                                resolveFn, 200000,
                                                m_savedHdr, m_filtered);
        }
    }

    ~CivicBuildingFilterScope()
    {
        if (m_swapped && m_world) {
            SafeMemcpyPod(m_world + m_vecOff, m_savedHdr, sizeof(m_savedHdr));
        }
    }

    bool IsSwapped() const { return m_swapped; }
};

static int ResolveStudioOwnerFast(const BYTE* studio)
{
    if (!studio) return 1;
    const BYTE* bld = nullptr;
    if (SafeReadPod((const BYTE* const*)(studio + STUDIO_BUILDING_OFFSET), &bld) && bld) {
        return LookupBuildingOwnerCached(bld);
    }
    return 1;
}

static BYTE* __fastcall H_CitizenStudioSelect(BYTE* world, BYTE* citizen)
{
    int citOwner = TwoStates_IsActive() ? ResolveCitizenOwnerCached(citizen) : 0;
    CivicBuildingFilterScope studioFilter(world, citOwner, WORLD_STUDIOS_BEGIN_OFFSET, &ResolveStudioOwnerFast);
    return o_citizenStudioSelect ? o_citizenStudioSelect(world, citizen) : nullptr;
}

static void __fastcall H_CitizenStudioEffect(BYTE* world, BYTE* studio, BYTE* citizen, float dt)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenStudioEffect) o_citizenStudioEffect(world, studio, citizen, dt);
}

static int ResolveExistingCitizenHomeOwner(const BYTE* cit)
{
    if (!cit || !IsPointerLikeX64((uintptr_t)cit)) return 0;
    int owner = 0;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* home = *(const BYTE* const*)(cit + CIT_HOME_BUILDING_OFFSET);
        if (!home || !IsPointerLikeX64((uintptr_t)home)) {
            const BYTE* parent = *(const BYTE* const*)(cit + CIT_PARENT_OFFSET);
            home = (parent && IsPointerLikeX64((uintptr_t)parent))
                ? *(const BYTE* const*)(parent + CIT_HOME_BUILDING_OFFSET)
                : nullptr;
        }
        if (home && IsPointerLikeX64((uintptr_t)home)) {
            owner = LookupBuildingOwnerCached(home);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 0;
    }
    DiagLog_EndSafeSeh();
    return owner;
}

static bool IsCrossStateCitizenRelocation(const BYTE* citizen, const BYTE* targetBld)
{
    if (!TwoStates_IsActive() || !citizen || !targetBld) return false;
    int citOwner = ResolveExistingCitizenHomeOwner(citizen);
    if (citOwner != 1 && citOwner != 2) return false;
    int bldOwner = LookupBuildingOwnerCached(targetBld);
    return (bldOwner == 1 || bldOwner == 2) && (citOwner != bldOwner);
}

static unsigned char __fastcall H_CitizenCanMoveToBld(BYTE* world, BYTE* targetBld, BYTE* citizen)
{
    if (IsCrossStateCitizenRelocation(citizen, targetBld)) {
        return 0;
    }
    return o_citizenCanMoveToBld ? o_citizenCanMoveToBld(world, targetBld, citizen) : 1;
}

static void __fastcall H_CitizenMoveToBld(BYTE* world, BYTE* citizen, BYTE* targetBld)
{
    if (IsCrossStateCitizenRelocation(citizen, targetBld)) {
        return;
    }
    CitizenEconScope scope(world, targetBld, citizen);
    if (o_citizenMoveToBld) {
        o_citizenMoveToBld(world, citizen, targetBld);
    } else if (citizen && targetBld) {
        SafeWritePod((BYTE**)(citizen + CIT_HOME_BUILDING_OFFSET), targetBld);
    }
}

static unsigned char __fastcall H_CitizenStarveStep(BYTE* world, BYTE* citizen)
{
    int citOwner = TwoStates_IsActive() ? ResolveCitizenOwnerCached(citizen) : 0;
    CitizenEconScope scope(world, nullptr, citizen);
    CivicBuildingFilterScope bldFilter(world, citOwner);
    return o_citizenStarveStep ? o_citizenStarveStep(world, citizen) : 0;
}

static BYTE* SelectLastElementFromWorldVec(const BYTE* world, size_t vecOff)
{
    if (!world) return nullptr;
    BYTE* bestElem = nullptr;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* const* b = *(BYTE* const* const*)(world + vecOff);
        BYTE* const* e = *(BYTE* const* const*)(world + vecOff + 8);
        if (b && e > b && (e - b) <= 200000) {
            for (BYTE* const* it = b; it < e; ++it) {
                if (*it) bestElem = *it;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bestElem = nullptr;
    }
    DiagLog_EndSafeSeh();
    return bestElem;
}

static unsigned char __fastcall MockEngineCitizenStarveStep(BYTE* world, BYTE* citizen)
{
    if (!world || !citizen) return 0;
    BYTE* bestBld = SelectLastElementFromWorldVec(world, WORLD_BUILDINGS_OFFSET);
    if (bestBld) {
        H_CitizenMoveToBld(world, citizen, bestBld);
    }
    return 0;
}

void TwoStates_CitizenStarveStepForTest(BYTE* world, BYTE* citizen)
{
    CitizenStarveStepFn prev = o_citizenStarveStep;
    o_citizenStarveStep = &MockEngineCitizenStarveStep;
    H_CitizenStarveStep(world, citizen);
    o_citizenStarveStep = prev;
}

static BYTE* __fastcall MockEngineCitizenStudioSelect(BYTE* world, BYTE* /*citizen*/)
{
    return SelectLastElementFromWorldVec(world, WORLD_STUDIOS_BEGIN_OFFSET);
}

BYTE* TwoStates_CitizenStudioSelectForTest(BYTE* world, BYTE* citizen)
{
    CitizenStudioSelectFn prev = o_citizenStudioSelect;
    o_citizenStudioSelect = &MockEngineCitizenStudioSelect;
    BYTE* res = H_CitizenStudioSelect(world, citizen);
    o_citizenStudioSelect = prev;
    return res;
}

static float g_mockStudioHappyDelta = 0.0f;

static void __fastcall MockEngineCitizenStudioEffect(BYTE* world, BYTE* /*studio*/, BYTE* /*citizen*/, float /*dt*/)
{
    AccumulateMockTrackerCause(world, WORLD_TRACKER_HAPPY_CUR_SUM_OFF, WORLD_TRACKER_HAPPY_CUR_CNT_OFF,
                               9, TRACKER_HAPPY_CAUSE_COUNT, g_mockStudioHappyDelta);
}

void TwoStates_CitizenStudioEffectForTest(BYTE* world, BYTE* studio, BYTE* citizen, float happyDelta)
{
    CitizenStudioEffectFn prev = o_citizenStudioEffect;
    o_citizenStudioEffect = &MockEngineCitizenStudioEffect;
    g_mockStudioHappyDelta = happyDelta;
    H_CitizenStudioEffect(world, studio, citizen, 1.0f);
    o_citizenStudioEffect = prev;
}

bool TwoStates_CitizenCanMoveToBldForTest(BYTE* world, BYTE* targetBld, BYTE* citizen)
{
    return H_CitizenCanMoveToBld(world, targetBld, citizen) != 0;
}

void TwoStates_CitizenMoveToBldForTest(BYTE* world, BYTE* citizen, BYTE* targetBld)
{
    H_CitizenMoveToBld(world, citizen, targetBld);
}

static bool __fastcall H_CourtAssign(BYTE* world, BYTE* caseObj, uintptr_t a3, uintptr_t a4)
{
    int citOwner = TwoStates_IsActive() ? ResolveCrimeCaseOwner(caseObj) : 0;
    CivicBuildingFilterScope scope(world, citOwner);
    return o_courtAssign ? o_courtAssign(world, caseObj, a3, a4) : false;
}

static bool __fastcall H_PrisonAssign(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4,
                                      uintptr_t a5, uintptr_t a6)
{
    int citOwner = TwoStates_IsActive() ? ResolveCitizenOwnerCached(citizen) : 0;
    CitizenEconScope econ(world, nullptr, citizen);
    CivicBuildingFilterScope scope(world, citOwner);
    return o_prisonAssign ? o_prisonAssign(world, citizen, a3, a4, a5, a6) : false;
}

static bool __fastcall H_OrphanageAssign(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4)
{
    int citOwner = TwoStates_IsActive() ? ResolveCitizenOwnerCached(citizen) : 0;
    CivicBuildingFilterScope scope(world, citOwner);
    return o_orphanageAssign ? o_orphanageAssign(world, citizen, a3, a4) : false;
}

static __int64 __fastcall H_LivingTick(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                       uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_livingTick);
}

static float __fastcall H_WaterHealthStep(BYTE* world, BYTE* building, float waterNeeded,
                                          unsigned char flag4, BYTE* citizen,
                                          unsigned char flag6, float factor7)
{
    int owner = (g_inSimStep && !TwoStates_IsInEconHook())
        ? (building ? LookupBuildingOwnerCached(building) : ResolveCitizenOwnerCached(citizen))
        : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    float safeWater = isfinite(waterNeeded) ? waterNeeded : 0.0f;
    float safeFactor = isfinite(factor7) ? factor7 : 0.0f;
    float r = o_waterHealthStep
        ? o_waterHealthStep(world, building, safeWater, flag4, citizen, flag6, safeFactor)
        : 0.0f;
    if (active) EndEconAttribution(owner);
    return isfinite(r) ? r : 0.0f;
}

static float g_mockWaterSpendRub = 0.0f;
static float __fastcall MockEngineWaterHealthStep(BYTE* world, BYTE* building, float waterNeeded,
                                                  unsigned char flag4, BYTE* citizen,
                                                  unsigned char flag6, float factor7)
{
    if (!world || !isfinite(waterNeeded) || !isfinite(factor7) || waterNeeded <= 0.0f || factor7 <= 0.0f) {
        return 0.0f;
    }
    if (g_mockWaterSpendRub != 0.0f) {
        *(double*)(world + WORLD_RUB_OFFSET) -= (double)g_mockWaterSpendRub;
    }
    float eff = (flag4 != 0 || flag6 != 0 || citizen != nullptr || building != nullptr)
        ? (waterNeeded * factor7)
        : 0.0f;
    if (building) {
        *(float*)(building + BLD_EFFECTIVE_CAP_FLOAT_OFFSET) = 27.0f * eff;
    }
    return eff;
}

float TwoStates_WaterHealthStepForTest(BYTE* world, BYTE* building, float waterNeeded,
                                       unsigned char flag4, BYTE* citizen,
                                       unsigned char flag6, float factor7, float spendRub)
{
    WaterHealthStepFn prev = o_waterHealthStep;
    o_waterHealthStep = &MockEngineWaterHealthStep;
    g_mockWaterSpendRub = spendRub;
    float r = H_WaterHealthStep(world, building, waterNeeded, flag4, citizen, flag6, factor7);
    if (building) {
        SanitizeBuildingEffectiveCapFloat(building);
    }
    o_waterHealthStep = prev;
    return r;
}

static __int64 __fastcall H_HeatingStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                        uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_heatingStep);
}

static void __fastcall H_CitizenCommitCrime(BYTE* world, BYTE* citizen, int32_t severity,
                                            uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenCommitCrime) o_citizenCommitCrime(world, citizen, severity, a4, a5, a6);
}

static void __fastcall H_CrimeExpireCase(BYTE* world, BYTE* caseObj, float a3,
                                         uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !g_inCommand && !TwoStates_IsInEconHook() && TwoStates_IsActive())
        ? ResolveCrimeCaseOwner(caseObj)
        : 0;
    if (owner == 1 || owner == 2) {
        int32_t curUninv = 0;
        if (world && SafeReadPod((const int32_t*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_EXTRA_COUNTS_OFF + 17 * sizeof(int32_t)), &curUninv)) {
            int32_t d = curUninv - g_lastWorldScalars.extraCounts[17];
            if (d > 0) {
                g_stateCurEcon[owner - 1].scalars.extraCounts[17] += d;
                g_lastWorldScalars.extraCounts[17] = curUninv;
            }
        }
    }
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    if (o_crimeExpireCase) o_crimeExpireCase(world, caseObj, a3, a4, a5, a6);
    if (active) EndEconAttribution(owner);
}

static void __fastcall H_CitizenFootArrive(BYTE* world, BYTE* citizen, unsigned char flag,
                                           uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenFootArrive) o_citizenFootArrive(world, citizen, flag, a4, a5, a6);
}

static unsigned char __fastcall H_CitizenVehicleAlight(BYTE* world, BYTE* citizen, BYTE* stationBld,
                                                       unsigned char flag, uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, stationBld, citizen);
    return o_citizenVehicleAlight ? o_citizenVehicleAlight(world, citizen, stationBld, flag, a5, a6) : 0;
}

static void __fastcall MockEngineCitizenCommitCrime(BYTE* world, BYTE* /*citizen*/, int32_t severity,
                                                    uintptr_t /*a4*/, uintptr_t /*a5*/, uintptr_t /*a6*/)
{
    if (!world) return;
    int idx = (severity >= 2) ? 14 : ((severity == 1) ? 13 : 12);
    *(int32_t*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_EXTRA_COUNTS_OFF + (size_t)idx * sizeof(int32_t)) += 1;
}

void TwoStates_CitizenCommitCrimeForTest(BYTE* world, BYTE* citizen, int32_t severity)
{
    CitizenCommitCrimeFn prev = o_citizenCommitCrime;
    o_citizenCommitCrime = &MockEngineCitizenCommitCrime;
    H_CitizenCommitCrime(world, citizen, severity, 0, 0, 0);
    o_citizenCommitCrime = prev;
}

static int   g_mockTourismIdx = 0;
static float g_mockTourismSpend = 0.0f;
static bool  g_mockTourismIsUsd = true;

static __int64 __fastcall MockEngineBldHotelStep(BYTE* world, BYTE* /*building*/, uintptr_t /*a3*/,
                                                 uintptr_t /*a4*/, uintptr_t /*a5*/, uintptr_t /*a6*/)
{
    if (!world) return 0;
    if (g_mockTourismIdx >= 0 && g_mockTourismIdx < 44) {
        *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_TOURISM_OFF + (size_t)g_mockTourismIdx * sizeof(float)) += g_mockTourismSpend;
    }
    if (g_mockTourismIsUsd) {
        *(double*)(world + WORLD_USD_OFFSET) += (double)g_mockTourismSpend;
    } else {
        *(double*)(world + WORLD_RUB_OFFSET) += (double)g_mockTourismSpend;
    }
    return 1;
}

void TwoStates_BldHotelStepForTest(BYTE* world, BYTE* building, int tourismIdx, float spendAmount, bool isUsd)
{
    BldEconFn prev = o_bldHotelStep;
    o_bldHotelStep = &MockEngineBldHotelStep;
    g_mockTourismIdx = tourismIdx;
    g_mockTourismSpend = spendAmount;
    g_mockTourismIsUsd = isUsd;
    H_GenericBldEconStep<&o_bldHotelStep>(world, building, 0, 0, 0, 0);
    o_bldHotelStep = prev;
}

class CustomsTourismWorldSwapScope {
public:
    CustomsTourismWorldSwapScope(BYTE* world, const BYTE* customsBld)
    {
        if (!world || !customsBld || !IsTwoStatesResearchOrLicenseActive()) return;
        EnsureEngineResearchInitialized(world);
        EnsureStateEconInitialized(world);
        int owner = ResolveCustomsTourismOwner(customsBld);
        if (owner != 1 && owner != 2) return;
        int sIdx = owner - 1;
        m_world = world;
        m_active = true;
        TwoStates_InvalidateResearchMountCache();
        SafeMemcpyPod(m_savedUnlocks, world + WORLD_RESEARCH_UNLOCKS_OFFSET, WORLD_RESEARCH_UNLOCKS_BYTES);
        SafeMemcpyPod(world + WORLD_RESEARCH_UNLOCKS_OFFSET, g_engineResearch[sIdx].unlocks, WORLD_RESEARCH_UNLOCKS_BYTES);
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &m_savedCurScalars);
        SafeWriteStatScalars(world + WORLD_STAT_CURRENT_OFFSET, g_stateCurEcon[sIdx].scalars);
        if (SafeReadStatHistHeader(world, &m_histBegin, &m_histCount) &&
            m_histBegin && m_histCount > 0 && g_stateHistEcon[sIdx].size() == (size_t)m_histCount) {
            m_histStartIdx = (m_histCount > 365) ? (m_histCount - 365) : 0;
            size_t swapN = (size_t)(m_histCount - m_histStartIdx);
            m_savedHistScalars.resize(swapN);
            for (size_t i = 0; i < swapN; ++i) {
                int hIdx = m_histStartIdx + (int)i;
                BYTE* rec = m_histBegin + (size_t)hIdx * STAT_RECORD_SIZE;
                SafeReadStatScalars(rec, &m_savedHistScalars[i]);
                SafeWriteStatScalars(rec, g_stateHistEcon[sIdx][(size_t)hIdx].scalars);
            }
        }
    }

    ~CustomsTourismWorldSwapScope()
    {
        if (!m_active || !m_world) return;
        if (m_histBegin && !m_savedHistScalars.empty()) {
            for (size_t i = 0; i < m_savedHistScalars.size(); ++i) {
                int hIdx = m_histStartIdx + (int)i;
                SafeWriteStatScalars(m_histBegin + (size_t)hIdx * STAT_RECORD_SIZE, m_savedHistScalars[i]);
            }
        }
        SafeWriteStatScalars(m_world + WORLD_STAT_CURRENT_OFFSET, m_savedCurScalars);
        SafeMemcpyPod(m_world + WORLD_RESEARCH_UNLOCKS_OFFSET, m_savedUnlocks, WORLD_RESEARCH_UNLOCKS_BYTES);
        TwoStates_InvalidateResearchMountCache();
    }

    CustomsTourismWorldSwapScope(const CustomsTourismWorldSwapScope&) = delete;
    CustomsTourismWorldSwapScope& operator=(const CustomsTourismWorldSwapScope&) = delete;

private:
    BYTE* m_world = nullptr;
    bool m_active = false;
    uint8_t m_savedUnlocks[WORLD_RESEARCH_UNLOCKS_BYTES] = {};
    StateEconScalars m_savedCurScalars = {};
    BYTE* m_histBegin = nullptr;
    int m_histCount = 0;
    int m_histStartIdx = 0;
    std::vector<StateEconScalars> m_savedHistScalars;
};

static __int64 CalcCustomsTouristSpawnFallback(BYTE* world, BYTE* customsBld, float* outAvgRating, int32_t* outSpawnCount)
{
    if (outAvgRating)  *outAvgRating  = 0.0f;
    if (outSpawnCount) *outSpawnCount = 0;
    if (!world || !customsBld) return 0;
    int visaIdx = ReadCustomsBorderVisaUnlockIdx(customsBld);
    if (visaIdx < 0) return 0;
    uint8_t visaUnlocked = 0;
    SafeReadPod(world + WORLD_RESEARCH_UNLOCKS_OFFSET + (size_t)visaIdx, &visaUnlocked);
    if (visaUnlocked == 0) return 0;
    uint8_t mktUnlocked = 0;
    SafeReadPod(world + WORLD_RESEARCH_UNLOCKS_OFFSET + (size_t)(visaIdx + 2), &mktUnlocked);
    StateEconScalars cur = {};
    SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &cur);
    int tripIdx = (visaIdx == 11) ? 5 : 4;
    int rateIdx = (visaIdx == 11) ? 11 : 10;
    int32_t trips = cur.extraCounts[tripIdx];
    float ratingSum = 0.0f;
    memcpy(&ratingSum, &cur.extraCounts[rateIdx], sizeof(float));
    float avg = (trips > 0 && isfinite(ratingSum) && ratingSum > 0.0f) ? (ratingSum / (float)trips) : 3.0f;
    int32_t spawn = 2 + (mktUnlocked ? 2 : 0) + (int32_t)avg;
    if (outAvgRating)  *outAvgRating  = avg;
    if (outSpawnCount) *outSpawnCount = spawn;
    return 1;
}

static __int64 __fastcall H_CalcCustomsTouristSpawn(BYTE* world, BYTE* customsBld, float* outAvgRating, int32_t* outSpawnCount)
{
    CustomsTourismWorldSwapScope swap(world, customsBld);
    if (o_calcCustomsTouristSpawn) {
        return o_calcCustomsTouristSpawn(world, customsBld, outAvgRating, outSpawnCount);
    }
    return CalcCustomsTouristSpawnFallback(world, customsBld, outAvgRating, outSpawnCount);
}

static void PrimeCustomsFirstTouristSpawn(BYTE* world, BYTE* customsBld, int owner)
{
    if (!world || !customsBld || (owner != 1 && owner != 2) || !IsTwoStatesResearchOrLicenseActive()) return;
    EnsureEngineResearchInitialized(world);
    int visaIdx = ReadCustomsBorderVisaUnlockIdx(customsBld);
    if (visaIdx < 0 || visaIdx >= (int)WORLD_RESEARCH_UNLOCKS_BYTES) return;
    if (g_engineResearch[owner - 1].unlocks[visaIdx] == 0) return;
    int32_t totalSpawned = 0;
    if (!SafeReadPod((const int32_t*)(customsBld + BLD_CUSTOMS_TOURISTS_SPAWNED_OFF), &totalSpawned) ||
        totalSpawned != 0) {
        return;
    }
    if (SafeCountStdVector(customsBld + BLD_CUSTOMS_WAITING_TOURISTS_OFF, sizeof(void*)) != 0) return;
    SafeWritePod((int32_t*)(customsBld + BLD_CUSTOMS_TOURISTS_SPAWNED_OFF), (int32_t)1);
    SafeWritePod((float*)(customsBld + BLD_CUSTOMS_TOURIST_TIMER_OFFSET), 55.1f);
}

static __int64 BldCustomsTouristStepFallback(BYTE* world, BYTE* customsBld)
{
    if (!world || !customsBld) return 0;
    float timer = 0.0f;
    if (!SafeReadPod((const float*)(customsBld + BLD_CUSTOMS_TOURIST_TIMER_OFFSET), &timer) || timer <= 55.0f) {
        return 0;
    }
    SafeWritePod((float*)(customsBld + BLD_CUSTOMS_TOURIST_TIMER_OFFSET), 0.0f);
    float avgRating = 0.0f;
    int32_t spawnCount = 0;
    H_CalcCustomsTouristSpawn(world, customsBld, &avgRating, &spawnCount);
    int32_t curWaiting = (int32_t)SafeCountStdVector(customsBld + BLD_CUSTOMS_WAITING_TOURISTS_OFF, sizeof(void*));
    if (spawnCount > curWaiting) {
        int32_t totalSpawned = 0;
        SafeReadPod((const int32_t*)(customsBld + BLD_CUSTOMS_TOURISTS_SPAWNED_OFF), &totalSpawned);
        SafeWritePod((int32_t*)(customsBld + BLD_CUSTOMS_TOURISTS_SPAWNED_OFF),
                     totalSpawned + (spawnCount - curWaiting));
    }
    return 1;
}

static __int64 __fastcall H_BldCustomsTouristStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                                  uintptr_t a5, uintptr_t a6)
{
    int owner = building ? ResolveCustomsTourismOwner(building) : 0;
    PrimeCustomsFirstTouristSpawn(world, building, owner);
    bool active = (g_inSimStep && !TwoStates_IsInEconHook() && owner) ? BeginEconAttribution(world, owner) : false;
    __int64 r = o_bldCustomsTouristStep
        ? o_bldCustomsTouristStep(world, building, a3, a4, a5, a6)
        : BldCustomsTouristStepFallback(world, building);
    if (building) SanitizeBuildingEffectiveCapFloat(building);
    if (active) EndEconAttribution(owner);
    return r;
}

void TwoStates_CalcCustomsTouristSpawnForTest(BYTE* world, BYTE* customsBld, float* outAvgRating, int32_t* outSpawnCount)
{
    H_CalcCustomsTouristSpawn(world, customsBld, outAvgRating, outSpawnCount);
}

void TwoStates_BldCustomsTouristStepForTest(BYTE* world, BYTE* customsBld)
{
    H_BldCustomsTouristStep(world, customsBld, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Construction Office & Building Quota ($COUNT_LIMIT) Hooks
// ---------------------------------------------------------------------------

bool TwoStates_IsDemolitionOfficeOrSite(const BYTE* officeBld, const BYTE* targetBld)
{
    if (targetBld && IsPointerLikeX64((uintptr_t)targetBld)) {
        BYTE isDemolSite = 0;
        if (SafeReadPod(targetBld + BLD_IS_DEMOLITION_SITE_OFFSET, &isDemolSite) && isDemolSite != 0) {
            return true;
        }
    }
    if (officeBld && IsPointerLikeX64((uintptr_t)officeBld)) {
        uintptr_t bldType = 0;
        int cat = 0;
        if (SafeReadPod(officeBld + BLD_TYPE_OFFSET, &bldType) &&
            IsPointerLikeX64(bldType) &&
            SafeReadPod((const BYTE*)bldType + BLD_TYPE_CAT_OFF, &cat) &&
            cat == (int)BLD_CAT_DEMOLISH_CO) {
            return true;
        }
    }
    return false;
}

static bool ShouldAllowConstructionOfficeAssignment(const BYTE* officeBld, const BYTE* targetBld,
                                                    unsigned char checkFilters, unsigned char triggerImmediateScan,
                                                    uintptr_t callerRva)
{
    (void)triggerImmediateScan; // 0x161CB0 6th param only schedules RCO reachability scan, never marks demolition.
    if (!TwoStates_IsActive() || !officeBld || !targetBld) return true;
    int officeOwner = LookupBuildingOwnerCached(officeBld);
    int targetOwner = LookupBuildingOwnerCached(targetBld);
    bool isAutoSearch = (checkFilters != 0) ||
                        (callerRva == RVA_CALLER_OFFICE_ROAD_SCAN) ||
                        (callerRva == RVA_CALLER_RAIL_CHAIN_ASSIGN);
    bool isDemolition = TwoStates_IsDemolitionOfficeOrSite(officeBld, targetBld);
    if (isAutoSearch || isDemolition) {
        return officeOwner == targetOwner;
    }
    if (g_inCommand && (g_cmdOwner == 1 || g_cmdOwner == 2)) {
        return officeOwner == g_cmdOwner;
    }
    if (!g_inSimStep) {
        return officeOwner == TwoStates_LocalState();
    }
    return true;
}

static void __fastcall H_AssignConstructionOffice(BYTE* world, BYTE* officeBld, BYTE* targetBld, float dist,
                                                  unsigned char checkFilters, unsigned char triggerImmediateScan)
{
    uintptr_t retAddr = (uintptr_t)_ReturnAddress();
    uintptr_t exeBase = (uintptr_t)g_exeBase;
    uintptr_t callerRva = (exeBase && retAddr >= exeBase) ? (retAddr - exeBase) : 0;
    if (!ShouldAllowConstructionOfficeAssignment(officeBld, targetBld, checkFilters, triggerImmediateScan, callerRva)) {
        return;
    }
    if (o_assignConstructionOffice) {
        o_assignConstructionOffice(world, officeBld, targetBld, dist, checkFilters, triggerImmediateScan);
    }
}

bool TwoStates_AssignConstructionOfficeForTest(BYTE* world, BYTE* officeBld, BYTE* targetBld, float dist,
                                               unsigned char checkFilters, unsigned char triggerImmediateScan,
                                               uintptr_t callerRvaOverride)
{
    if (!ShouldAllowConstructionOfficeAssignment(officeBld, targetBld, checkFilters, triggerImmediateScan, callerRvaOverride)) {
        return false;
    }
    if (o_assignConstructionOffice) {
        o_assignConstructionOffice(world, officeBld, targetBld, dist, checkFilters, triggerImmediateScan);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-State Free / Limited Building Quota ($COUNT_LIMIT, world + 0x58)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct SovietCountLimitEntry {
    const BYTE* primaryType;         // +0x00
    const BYTE* mirroredType;        // +0x08 (_mirror variant)
    int32_t     currentCount;        // +0x10
    int32_t     customLimitOverride; // +0x14 (-1 = use *(int32_t*)(primaryType + 0x208))
};
#pragma pack(pop)
static_assert(sizeof(SovietCountLimitEntry) == 24, "SovietCountLimitEntry must be 24 bytes");

static bool LookupCountLimitEntry(const BYTE* limitTableVec, const BYTE* btype, SovietCountLimitEntry* outEntry)
{
    if (!limitTableVec || !btype || !outEntry) return false;
    uintptr_t ptrs[2] = {};
    if (!SafeMemcpyPod(ptrs, limitTableVec, sizeof(ptrs))) return false;
    const SovietCountLimitEntry* b = (const SovietCountLimitEntry*)ptrs[0];
    const SovietCountLimitEntry* e = (const SovietCountLimitEntry*)ptrs[1];
    if (!b || e <= b) return false;
    ptrdiff_t byteLen = (const BYTE*)e - (const BYTE*)b;
    if (byteLen <= 0 || (byteLen % (ptrdiff_t)sizeof(SovietCountLimitEntry)) != 0) return false;
    int count = (int)(byteLen / (ptrdiff_t)sizeof(SovietCountLimitEntry));
    if (count <= 0 || count > 4096) return false;

    bool found = false;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            if (b[i].primaryType == btype || b[i].mirroredType == btype) {
                *outEntry = b[i];
                found = true;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = false;
    }
    DiagLog_EndSafeSeh();
    return found;
}

static int32_t CountStateBuildingsForLimitEntry(const BYTE* world, const SovietCountLimitEntry& entry, int stateOwner)
{
    if (!world || (stateOwner != 1 && stateOwner != 2)) return entry.currentCount;
    EnsureBldPtrOwnerCache(world);
    int32_t matched = 0;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* const* b = *(const BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET);
        const BYTE* const* e = *(const BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET + 8);
        if (b && e > b && (e - b) <= 200000) {
            for (const BYTE* const* it = b; it < e; ++it) {
                const BYTE* bld = *it;
                if (!bld) continue;
                const BYTE* bldType = *(const BYTE* const*)(bld + BLD_TYPE_OFFSET);
                if (!bldType) continue;
                if ((entry.primaryType && bldType == entry.primaryType) ||
                    (entry.mirroredType && bldType == entry.mirroredType)) {
                    if (LookupBuildingOwnerCached(bld) == stateOwner) {
                        matched++;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        matched = entry.currentCount;
    }
    DiagLog_EndSafeSeh();
    return matched;
}

static int ResolveActiveCountLimitState(int stateOverride)
{
    if (stateOverride == 1 || stateOverride == 2) return stateOverride;
    if (!IsTwoStatesResearchOrLicenseActive()) return 0;
    bool onCmdThread = g_inCommand && (GetCurrentThreadId() == g_cmdThread);
    int st = onCmdThread
        ? g_cmdOwner
        : ((!g_inCommand && BuildSync_IsApplyingRemote())
               ? (3 - TwoStates_LocalState())
               : TwoStates_LocalState());
    return (st == 1 || st == 2) ? st : 1;
}

static bool ResolveEffectiveCountLimitEntry(const BYTE* world, const BYTE* limitTableVec,
                                            const BYTE* btype, int stateOverride,
                                            SovietCountLimitEntry* outEntry)
{
    if (!outEntry) return false;
    outEntry->primaryType = nullptr;
    outEntry->mirroredType = nullptr;
    outEntry->currentCount = 0;
    outEntry->customLimitOverride = -1;
    if (!LookupCountLimitEntry(limitTableVec, btype, outEntry)) return false;

    if (outEntry->customLimitOverride < 0) {
        const BYTE* limType = outEntry->primaryType ? outEntry->primaryType : outEntry->mirroredType;
        int32_t typeLim = -1;
        if (limType && SafeReadPod((const int32_t*)(limType + BLD_TYPE_COUNT_LIMIT_OFFSET), &typeLim)) {
            outEntry->customLimitOverride = typeLim;
        }
    }

    int activeState = ResolveActiveCountLimitState(stateOverride);
    if (activeState == 1 || activeState == 2) {
        const BYTE* effWorld = world ? world : (g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr);
        if (effWorld) {
            outEntry->currentCount = CountStateBuildingsForLimitEntry(effWorld, *outEntry, activeState);
        }
    }
    return true;
}

bool TwoStates_CheckBuildingCountLimitForTest(const BYTE* world, const BYTE* limitTableVec,
                                              const BYTE* btype, int stateOverride)
{
    if (!btype) return false;
    SovietCountLimitEntry entry = {};
    if (!ResolveEffectiveCountLimitEntry(world, limitTableVec, btype, stateOverride, &entry)) {
        return true;
    }
    if (entry.customLimitOverride < 0) return true;
    return entry.currentCount < entry.customLimitOverride;
}

bool TwoStates_GetBuildingCountLimitEntryForTest(const BYTE* world, const BYTE* limitTableVec,
                                                 const BYTE* btype, int stateOverride,
                                                 int32_t* outCur, int32_t* outMax)
{
    SovietCountLimitEntry entry = {};
    bool found = ResolveEffectiveCountLimitEntry(world, limitTableVec, btype, stateOverride, &entry);
    if (outCur) *outCur = entry.currentCount;
    if (outMax) *outMax = entry.customLimitOverride;
    return found;
}

static const BYTE* ResolveTooltipCountLimit(const BYTE* world, const BYTE* btype,
                                            int stateOverride, int32_t* outCountsPair)
{
    if (!world || !btype || !outCountsPair) return nullptr;
    const BYTE* limitTableVec = nullptr;
    if (!SafeReadPod((const BYTE* const*)(world + WORLD_BLD_COUNT_LIMIT_VEC_PTR_OFFSET), &limitTableVec) ||
        !limitTableVec) {
        return nullptr;
    }
    int effStateOverride = 0;
    if (stateOverride == 1 || stateOverride == 2) {
        effStateOverride = stateOverride;
    } else if (IsTwoStatesResearchOrLicenseActive()) {
        int localSt = TwoStates_LocalState();
        effStateOverride = (localSt == 1 || localSt == 2) ? localSt : 1;
    }
    SovietCountLimitEntry entry = {};
    if (!ResolveEffectiveCountLimitEntry(world, limitTableVec, btype, effStateOverride, &entry)) {
        return nullptr;
    }
    const BYTE* primary = entry.primaryType ? entry.primaryType : entry.mirroredType;
    if (!primary) return nullptr;
    outCountsPair[0] = entry.currentCount;
    outCountsPair[1] = entry.customLimitOverride;
    return primary;
}

static const BYTE* __fastcall H_BldCountLimitTooltipLookup(const BYTE* world, const BYTE* btype,
                                                           int32_t* outCountsPair)
{
    return ResolveTooltipCountLimit(world, btype, 0, outCountsPair);
}

const BYTE* TwoStates_ResolveTooltipCountLimitForTest(const BYTE* world, const BYTE* btype,
                                                      int stateOverride,
                                                      int32_t* outCur, int32_t* outMax)
{
    int32_t countsPair[2] = { 0, -1 };
    const BYTE* primary = ResolveTooltipCountLimit(world, btype, stateOverride, countsPair);
    if (outCur) *outCur = countsPair[0];
    if (outMax) *outMax = countsPair[1];
    return primary;
}

static unsigned char __fastcall H_BldCountLimitCheck(const BYTE* limitTableVec, const BYTE* btype)
{
    return TwoStates_CheckBuildingCountLimitForTest(nullptr, limitTableVec, btype, 0) ? 1 : 0;
}

static SovietCountLimitEntry* __fastcall H_BldCountLimitGet(const BYTE* limitTableVec,
                                                            SovietCountLimitEntry* outEntry,
                                                            const BYTE* btype)
{
    if (!outEntry) return nullptr;
    ResolveEffectiveCountLimitEntry(nullptr, limitTableVec, btype, 0, outEntry);
    return outEntry;
}

// ---------------------------------------------------------------------------
// Register-Preserving Relay Stubs & Mid-Function Patches
// ---------------------------------------------------------------------------

static BYTE*  g_econRelayStubPage = nullptr;
static size_t g_econRelayStubUsed = 0;

static bool EnsureRelayStubSpace(size_t needed)
{
    if (g_econRelayStubPage && g_econRelayStubUsed + needed <= 4096) return true;
    BYTE* page = g_exeBase ? AllocNear(g_exeBase, 4096) : nullptr;
    if (!page) {
        page = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!page) return false;
    g_econRelayStubPage = page;
    g_econRelayStubUsed = 0;
    return needed <= 4096;
}

static void EmitSaveVolatileRegs(BYTE*& p)
{
    static const BYTE kSaveBytes[] = {
        0x48, 0x89, 0x4C, 0x24, 0x20,                     // mov [rsp+0x20], rcx
        0x48, 0x89, 0x54, 0x24, 0x28,                     // mov [rsp+0x28], rdx
        0x4C, 0x89, 0x44, 0x24, 0x30,                     // mov [rsp+0x30], r8
        0x4C, 0x89, 0x4C, 0x24, 0x38,                     // mov [rsp+0x38], r9
        0x4C, 0x89, 0x54, 0x24, 0x40,                     // mov [rsp+0x40], r10
        0x4C, 0x89, 0x5C, 0x24, 0x48,                     // mov [rsp+0x48], r11
        0x0F, 0x11, 0x44, 0x24, 0x50,                     // movups [rsp+0x50], xmm0
        0x0F, 0x11, 0x4C, 0x24, 0x60,                     // movups [rsp+0x60], xmm1
        0x0F, 0x11, 0x54, 0x24, 0x70,                     // movups [rsp+0x70], xmm2
        0x0F, 0x11, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00,   // movups [rsp+0x80], xmm3
        0x0F, 0x11, 0xA4, 0x24, 0x90, 0x00, 0x00, 0x00,   // movups [rsp+0x90], xmm4
        0x0F, 0x11, 0xAC, 0x24, 0xA0, 0x00, 0x00, 0x00    // movups [rsp+0xA0], xmm5
    };
    memcpy(p, kSaveBytes, sizeof(kSaveBytes));
    p += sizeof(kSaveBytes);
}

static void EmitRestoreVolatileRegs(BYTE*& p, bool keepXmm0Result, bool zeroRcxOnExit)
{
    if (!keepXmm0Result) {
        static const BYTE kRestXmm0[] = { 0x0F, 0x10, 0x44, 0x24, 0x50 }; // movups xmm0, [rsp+0x50]
        memcpy(p, kRestXmm0, sizeof(kRestXmm0));
        p += sizeof(kRestXmm0);
    }
    static const BYTE kRestoreBytes[] = {
        0x0F, 0x10, 0x4C, 0x24, 0x60,                     // movups xmm1, [rsp+0x60]
        0x0F, 0x10, 0x54, 0x24, 0x70,                     // movups xmm2, [rsp+0x70]
        0x0F, 0x10, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00,   // movups xmm3, [rsp+0x80]
        0x0F, 0x10, 0xA4, 0x24, 0x90, 0x00, 0x00, 0x00,   // movups xmm4, [rsp+0x90]
        0x0F, 0x10, 0xAC, 0x24, 0xA0, 0x00, 0x00, 0x00,   // movups xmm5, [rsp+0xA0]
        0x4C, 0x8B, 0x5C, 0x24, 0x48,                     // mov r11, [rsp+0x48]
        0x4C, 0x8B, 0x54, 0x24, 0x40,                     // mov r10, [rsp+0x40]
        0x4C, 0x8B, 0x4C, 0x24, 0x38,                     // mov r9,  [rsp+0x38]
        0x4C, 0x8B, 0x44, 0x24, 0x30,                     // mov r8,  [rsp+0x30]
        0x48, 0x8B, 0x54, 0x24, 0x28                      // mov rdx, [rsp+0x28]
    };
    memcpy(p, kRestoreBytes, sizeof(kRestoreBytes));
    p += sizeof(kRestoreBytes);
    if (zeroRcxOnExit) {
        *p++ = 0x33; *p++ = 0xC9;                         // xor ecx, ecx
    } else {
        static const BYTE kRestRcx[] = { 0x48, 0x8B, 0x4C, 0x24, 0x20 }; // mov rcx, [rsp+0x20]
        memcpy(p, kRestRcx, sizeof(kRestRcx));
        p += sizeof(kRestRcx);
    }
}

void* TwoStates_BuildRegPreservingRelayStub(void* targetCppFn, bool keepXmm0Result, bool zeroRcxOnExit)
{
    if (!targetCppFn) return nullptr;
    if (!EnsureRelayStubSpace(192)) return targetCppFn;

    BYTE* stub = g_econRelayStubPage + g_econRelayStubUsed;
    BYTE* p = stub;

    // Leaf-function LTCG callers in SOVIET64.exe (e.g. 0x334CC9, 0x32F50D, 0x33381E calling 0x2FC290)
    // assume volatile registers (rdx, r8, r9, r10, r11, xmm0..xmm5) are preserved across the call.
    // On entry rsp & 0xF == 8; sub rsp, 0xB8 aligns rsp to 16 bytes for targetCppFn.
    static const BYTE kSubRspB8[] = { 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00 };
    memcpy(p, kSubRspB8, sizeof(kSubRspB8));
    p += sizeof(kSubRspB8);
    EmitSaveVolatileRegs(p);

    *p++ = 0x48; *p++ = 0xB8;                             // mov rax, imm64 (targetCppFn)
    memcpy(p, &targetCppFn, sizeof(void*));
    p += sizeof(void*);
    *p++ = 0xFF; *p++ = 0xD0;                             // call rax

    EmitRestoreVolatileRegs(p, keepXmm0Result, zeroRcxOnExit);
    static const BYTE kAddRspB8Ret[] = { 0x48, 0x81, 0xC4, 0xB8, 0x00, 0x00, 0x00, 0xC3 };
    memcpy(p, kAddRspB8Ret, sizeof(kAddRspB8Ret));
    p += sizeof(kAddRspB8Ret);

    size_t stubLen = (size_t)(p - stub);
    FlushInstructionCache(GetCurrentProcess(), stub, stubLen);
    g_econRelayStubUsed += ((stubLen + 15) & ~size_t(15));
    return stub;
}

static bool PatchJmp14WithNops(BYTE* site, size_t siteLen, const BYTE* expected, void* targetStub, const char* tag)
{
    if (!site || siteLen < 14 || !expected || !targetStub) return false;
    if (!SafeBytesMatch(site, expected, siteLen)) {
        Logf("republicmp  TWO_STATES: %s bytes mismatch at %p", tag, (void*)site);
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(site, siteLen, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    site[0] = 0xFF;
    site[1] = 0x25;
    *(uint32_t*)(site + 2) = 0;
    memcpy(site + 6, &targetStub, sizeof(void*));
    for (size_t i = 14; i < siteLen; ++i) site[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), site, siteLen);
    VirtualProtect(site, siteLen, oldProt, &oldProt);
    return true;
}

static int InstallBldSimLoopHooks()
{
    if (!g_exeBase || !EnsureRelayStubSpace(160)) return 0;

    void* curBldPtrAddr = (void*)&g_simLoopCurrentBuilding;

    BYTE* headStub = g_econRelayStubPage + g_econRelayStubUsed;
    BYTE* p = headStub;
    memcpy(p, kBldSimLoopHeadExpect, sizeof(kBldSimLoopHeadExpect));
    p += sizeof(kBldSimLoopHeadExpect);
    *p++ = 0x48; *p++ = 0xB8;
    memcpy(p, &curBldPtrAddr, sizeof(void*));
    p += sizeof(void*);
    *p++ = 0x48; *p++ = 0x89; *p++ = 0x10;
    memcpy(p, kBldSimLoopHeadExpect, 7);
    p += 7;
    *p++ = 0xFF; *p++ = 0x25; *(uint32_t*)p = 0; p += 4;
    void* headRet = (void*)(g_exeBase + RVA_BLD_SIM_LOOP_HEAD + sizeof(kBldSimLoopHeadExpect));
    memcpy(p, &headRet, sizeof(void*));
    p += sizeof(void*);
    size_t headLen = (size_t)(p - headStub);
    FlushInstructionCache(GetCurrentProcess(), headStub, headLen);
    g_econRelayStubUsed += ((headLen + 15) & ~size_t(15));

    BYTE* tailStub = g_econRelayStubPage + g_econRelayStubUsed;
    p = tailStub;
    *p++ = 0x48; *p++ = 0xB8;
    memcpy(p, &curBldPtrAddr, sizeof(void*));
    p += sizeof(void*);
    *p++ = 0x48; *p++ = 0xC7; *p++ = 0x00; *(uint32_t*)p = 0; p += 4;
    memcpy(p, kBldSimLoopTailExpect, sizeof(kBldSimLoopTailExpect));
    p += sizeof(kBldSimLoopTailExpect);
    *p++ = 0xFF; *p++ = 0x25; *(uint32_t*)p = 0; p += 4;
    void* tailRet = (void*)(g_exeBase + RVA_BLD_SIM_LOOP_TAIL + sizeof(kBldSimLoopTailExpect));
    memcpy(p, &tailRet, sizeof(void*));
    p += sizeof(void*);
    size_t tailLen = (size_t)(p - tailStub);
    FlushInstructionCache(GetCurrentProcess(), tailStub, tailLen);
    g_econRelayStubUsed += ((tailLen + 15) & ~size_t(15));

    int patched = 0;
    if (PatchJmp14WithNops(g_exeBase + RVA_BLD_SIM_LOOP_HEAD, sizeof(kBldSimLoopHeadExpect),
                           kBldSimLoopHeadExpect, headStub, "bld_sim_loop_head")) {
        patched++;
    }
    if (PatchJmp14WithNops(g_exeBase + RVA_BLD_SIM_LOOP_TAIL, sizeof(kBldSimLoopTailExpect),
                           kBldSimLoopTailExpect, tailStub, "bld_sim_loop_tail")) {
        patched++;
    }
    return patched;
}

#pragma pack(push, 1)
struct CitSevereIllnessPatchStub {
    uint8_t pushRcx;       // 0x51
    uint8_t pushR8[2];     // 0x41, 0x50
    uint8_t movRcxR14[3];  // 0x49, 0x8B, 0xCE
    uint8_t movR8Rbx[3];   // 0x4C, 0x8B, 0xC3
    uint8_t movRaxImm[2];  // 0x48, 0xB8
    void*   relayTarget;
    uint8_t callRax[2];    // 0xFF, 0xD0
    uint8_t popR8[2];      // 0x41, 0x58
    uint8_t popRcx;        // 0x59
    uint8_t jmpRip0[6];    // 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
    void*   resumeTarget;
};
#pragma pack(pop)
static_assert(sizeof(CitSevereIllnessPatchStub) == 38, "CitSevereIllnessPatchStub must be 38 bytes");

static int InstallCitSevereIllnessHook()
{
    if (!g_exeBase) return 0;
    void* stubSevereIllness = TwoStates_BuildRegPreservingRelayStub((void*)&H_CitizenSevereIllnessTick, false, false);
    if (!stubSevereIllness || !g_econRelayStubPage || g_econRelayStubUsed + 64 > 4096) return 0;

    CitSevereIllnessPatchStub tmpl = {
        0x51,
        { 0x41, 0x50 },
        { 0x49, 0x8B, 0xCE },
        { 0x4C, 0x8B, 0xC3 },
        { 0x48, 0xB8 },
        stubSevereIllness,
        { 0xFF, 0xD0 },
        { 0x41, 0x58 },
        0x59,
        { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 },
        (void*)(g_exeBase + RVA_CIT_SEVERE_ILLNESS_RESUME)
    };
    BYTE* patchStub = g_econRelayStubPage + g_econRelayStubUsed;
    memcpy(patchStub, &tmpl, sizeof(tmpl));
    FlushInstructionCache(GetCurrentProcess(), patchStub, sizeof(tmpl));
    g_econRelayStubUsed += ((sizeof(tmpl) + 15) & ~size_t(15));

    return PatchJmp14WithNops(g_exeBase + RVA_CIT_SEVERE_ILLNESS_TICK,
                              sizeof(kCitSevereIllnessTickExpect),
                              kCitSevereIllnessTickExpect,
                              patchStub,
                              "cit_severe_illness_tick") ? 1 : 0;
}

static void* BuildBldCountLimitTooltipStub()
{
    if (!g_exeBase || !EnsureRelayStubSpace(256)) return nullptr;

    BYTE* stub = g_econRelayStubPage + g_econRelayStubUsed;
    BYTE* p = stub;

    // Mid-function 0x7AC6B7 inside 0x7ABF20 has (rsp & 0xF) == 0 (8 pushes + sub rsp, 0x1368).
    // sub rsp, 0xB0 keeps 16-byte stack alignment before `call rax` while preserving live r8/r9/etc.
    static const BYTE kSubRspB0[] = { 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00 };
    memcpy(p, kSubRspB0, sizeof(kSubRspB0));
    p += sizeof(kSubRspB0);
    EmitSaveVolatileRegs(p);

    static const BYTE kArgsAndTargetPrep[] = {
        0x4C, 0x89, 0xF9,             // mov rcx, r15 (WORLD)
        0x4C, 0x89, 0xF2,             // mov rdx, r14 (btype)
        0x4C, 0x8D, 0x45, 0xA0        // lea r8, [rbp-0x60] (outCountsPair[2])
    };
    memcpy(p, kArgsAndTargetPrep, sizeof(kArgsAndTargetPrep));
    p += sizeof(kArgsAndTargetPrep);

    void* fnAddr = (void*)&H_BldCountLimitTooltipLookup;
    *p++ = 0x48; *p++ = 0xB8;         // mov rax, imm64
    memcpy(p, &fnAddr, sizeof(void*));
    p += sizeof(void*);
    *p++ = 0xFF; *p++ = 0xD0;         // call rax

    EmitRestoreVolatileRegs(p, false, false);
    static const BYTE kPostCallCommon[] = {
        0x48, 0x81, 0xC4, 0xB0, 0x00, 0x00, 0x00, // add rsp, 0xB0
        0x4C, 0x8B, 0x65, 0xB0,                   // mov r12, [rbp-0x50]
        0x48, 0x85, 0xC0                          // test rax, rax
    };
    memcpy(p, kPostCallCommon, sizeof(kPostCallCommon));
    p += sizeof(kPostCallCommon);

    *p++ = 0x74;                      // jz rel8 skipLabel
    BYTE* jzDisp = p++;

    // Found path: set rdi = primaryType and jump to 0x7AC748
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xC7; // mov rdi, rax
    *p++ = 0xFF; *p++ = 0x25; *(uint32_t*)p = 0; p += 4;
    void* foundTarget = (void*)(g_exeBase + RVA_BLD_COUNT_LIMIT_TOOLTIP_FOUND);
    memcpy(p, &foundTarget, sizeof(void*));
    p += sizeof(void*);

    // Skip path: jump to 0x7AC94E
    BYTE* skipLabel = p;
    *jzDisp = (BYTE)(skipLabel - (jzDisp + 1));
    *p++ = 0xFF; *p++ = 0x25; *(uint32_t*)p = 0; p += 4;
    void* skipTarget = (void*)(g_exeBase + RVA_BLD_COUNT_LIMIT_TOOLTIP_SKIP);
    memcpy(p, &skipTarget, sizeof(void*));
    p += sizeof(void*);

    size_t stubLen = (size_t)(p - stub);
    FlushInstructionCache(GetCurrentProcess(), stub, stubLen);
    g_econRelayStubUsed += ((stubLen + 15) & ~size_t(15));
    return stub;
}

static int InstallBldCountLimitHooks()
{
    if (!g_exeBase) return 0;
    void* stubCheck   = TwoStates_BuildRegPreservingRelayStub((void*)&H_BldCountLimitCheck, false, false);
    void* stubGet     = TwoStates_BuildRegPreservingRelayStub((void*)&H_BldCountLimitGet, false, false);
    void* stubTooltip = BuildBldCountLimitTooltipStub();
    if (!stubCheck || !stubGet) return 0;
    int patched = 0;
    if (PatchJmp14WithNops(g_exeBase + RVA_BLD_COUNT_LIMIT_CHECK,
                           sizeof(kBldCountLimitCheckExpect),
                           kBldCountLimitCheckExpect,
                           stubCheck,
                           "bld_count_limit_check")) {
        patched++;
    }
    if (PatchJmp14WithNops(g_exeBase + RVA_BLD_COUNT_LIMIT_GET,
                           sizeof(kBldCountLimitGetExpect),
                           kBldCountLimitGetExpect,
                           stubGet,
                           "bld_count_limit_get")) {
        patched++;
    }
    if (stubTooltip &&
        PatchJmp14WithNops(g_exeBase + RVA_BLD_COUNT_LIMIT_TOOLTIP,
                           sizeof(kBldCountLimitTooltipExpect),
                           kBldCountLimitTooltipExpect,
                           stubTooltip,
                           "bld_count_limit_tooltip")) {
        patched++;
    }
    return patched;
}

// ---------------------------------------------------------------------------
// Hook Installers
// ---------------------------------------------------------------------------

int TwoStates_InstallTerrainIatHooks()
{
    int ok = 0;
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryXMin@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryXMin, (void**)&o_getBoundaryXMin, "terrain BoundaryXMin");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryXMax@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryXMax, (void**)&o_getBoundaryXMax, "terrain BoundaryXMax");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryZMin@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryZMin, (void**)&o_getBoundaryZMin, "terrain BoundaryZMin");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryZMax@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryZMax, (void**)&o_getBoundaryZMax, "terrain BoundaryZMax");
    return ok;
}

int TwoStates_InstallSimHooks()
{
    if (!g_exeBase) return 0;
    int simOk = 0;
    simOk += InstallInlineHook(g_exeBase + RVA_CIT_HOME_STEP, (void*)&H_CitHomeStep,
                               (void**)&o_citHomeStep, kCitHomeStepExpect,
                               sizeof(kCitHomeStepExpect), "republicmp two_states cit_home_step");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_CHURCH_STEP, (void*)&H_CitizenChurchStep,
                               (void**)&o_citizenChurchStep, kCitizenChurchStepExpect,
                               sizeof(kCitizenChurchStepExpect), "republicmp two_states citizen_church_step");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_BIRTH, (void*)&H_CitizenBirth,
                               (void**)&o_citizenBirth, kCitizenBirthExpect,
                               sizeof(kCitizenBirthExpect), "republicmp two_states citizen_birth");
    // 0x834DE0 / 0x83C590 / 0x83CA50 are LTCG leaf-convention callees: callers such as 0x830720 keep live
    // values in volatile registers across the call (r10 = citizen at 0x830729), so the C++ hooks run behind
    // register-preserving relay stubs (rax/al result is kept, rcx/rdx/r8..r11/xmm0..xmm5 are restored).
    void* stubStarveStep   = TwoStates_BuildRegPreservingRelayStub((void*)&H_CitizenStarveStep, false, false);
    void* stubCanMoveBld   = TwoStates_BuildRegPreservingRelayStub((void*)&H_CitizenCanMoveToBld, false, false);
    void* stubMoveToBld    = TwoStates_BuildRegPreservingRelayStub((void*)&H_CitizenMoveToBld, false, false);
    void* stubStudioEffect = TwoStates_BuildRegPreservingRelayStub((void*)&H_CitizenStudioEffect, false, false);
    void* stubStudioSelect = TwoStates_BuildRegPreservingRelayStub((void*)&H_CitizenStudioSelect, false, false);
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_STUDIO_EFFECT, stubStudioEffect,
                               (void**)&o_citizenStudioEffect, kCitizenStudioEffectExpect,
                               sizeof(kCitizenStudioEffectExpect), "republicmp two_states citizen_studio_effect");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_STUDIO_SELECT, stubStudioSelect,
                               (void**)&o_citizenStudioSelect, kCitizenStudioSelectExpect,
                               sizeof(kCitizenStudioSelectExpect), "republicmp two_states citizen_studio_select");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_STARVE_STEP, stubStarveStep,
                               (void**)&o_citizenStarveStep, kCitizenStarveStepExpect,
                               sizeof(kCitizenStarveStepExpect), "republicmp two_states citizen_starve_step");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_REMOVE, (void*)&H_CitizenRemove,
                               (void**)&o_citizenRemove, kCitizenRemoveExpect,
                               sizeof(kCitizenRemoveExpect), "republicmp two_states citizen_remove");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_EVAL_NEEDS, (void*)&H_CitizenEvalNeeds,
                               (void**)&o_citizenEvalNeeds, kCitizenEvalNeedsExpect,
                               sizeof(kCitizenEvalNeedsExpect), "republicmp two_states citizen_eval_needs");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_CAN_MOVE_TO_BLD, stubCanMoveBld,
                               (void**)&o_citizenCanMoveToBld, kCitizenCanMoveToBldExpect,
                               sizeof(kCitizenCanMoveToBldExpect), "republicmp two_states citizen_can_move_to_bld");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_MOVE_TO_BLD, stubMoveToBld,
                               (void**)&o_citizenMoveToBld, kCitizenMoveToBldExpect,
                               sizeof(kCitizenMoveToBldExpect), "republicmp two_states citizen_move_to_bld");
    simOk += InstallInlineHook(g_exeBase + RVA_COURT_ASSIGN, (void*)&H_CourtAssign,
                               (void**)&o_courtAssign, kCourtAssignExpect,
                               sizeof(kCourtAssignExpect), "republicmp two_states court_assign");
    simOk += InstallInlineHook(g_exeBase + RVA_PRISON_ASSIGN, (void*)&H_PrisonAssign,
                               (void**)&o_prisonAssign, kPrisonAssignExpect,
                               sizeof(kPrisonAssignExpect), "republicmp two_states prison_assign");
    simOk += InstallInlineHook(g_exeBase + RVA_ORPHANAGE_ASSIGN, (void*)&H_OrphanageAssign,
                               (void**)&o_orphanageAssign, kOrphanageAssignExpect,
                               sizeof(kOrphanageAssignExpect), "republicmp two_states orphanage_assign");
    simOk += InstallInlineHook(g_exeBase + RVA_LIVING_TICK, (void*)&H_LivingTick,
                               (void**)&o_livingTick, kLivingTickExpect,
                               sizeof(kLivingTickExpect), "republicmp two_states living_tick");
    simOk += InstallInlineHook(g_exeBase + RVA_WATER_HEALTH_STEP, (void*)&H_WaterHealthStep,
                               (void**)&o_waterHealthStep, kWaterHealthStepExpect,
                               sizeof(kWaterHealthStepExpect), "republicmp two_states water_health_step");
    simOk += InstallInlineHook(g_exeBase + RVA_HEATING_STEP, (void*)&H_HeatingStep,
                               (void**)&o_heatingStep, kHeatingStepExpect,
                               sizeof(kHeatingStepExpect), "republicmp two_states heating_step");
    simOk += InstallInlineHook(g_exeBase + RVA_BLD_COURTHOUSE_STEP, (void*)&H_GenericBldEconStep<&o_bldCourthouseStep>,
                               (void**)&o_bldCourthouseStep, kBldCourthouseStepExpect,
                               sizeof(kBldCourthouseStepExpect), "republicmp two_states bld_courthouse_step");
    simOk += InstallInlineHook(g_exeBase + RVA_BLD_POLICE_STEP, (void*)&H_GenericBldEconStep<&o_bldPoliceStep>,
                               (void**)&o_bldPoliceStep, kBldPoliceStepExpect,
                               sizeof(kBldPoliceStepExpect), "republicmp two_states bld_police_step");
    simOk += InstallInlineHook(g_exeBase + RVA_BLD_HOTEL_STEP, (void*)&H_GenericBldEconStep<&o_bldHotelStep>,
                               (void**)&o_bldHotelStep, kBldHotelStepExpect,
                               sizeof(kBldHotelStepExpect), "republicmp two_states bld_hotel_step");
    simOk += InstallInlineHook(g_exeBase + RVA_BLD_ATTRACTION1_STEP, (void*)&H_GenericBldEconStep<&o_bldAttraction1Step>,
                               (void**)&o_bldAttraction1Step, kBldAttraction1StepExpect,
                               sizeof(kBldAttraction1StepExpect), "republicmp two_states bld_attraction1_step");
    simOk += InstallInlineHook(g_exeBase + RVA_BLD_ATTRACTION2_STEP, (void*)&H_GenericBldEconStep<&o_bldAttraction2Step>,
                               (void**)&o_bldAttraction2Step, kBldAttraction2StepExpect,
                               sizeof(kBldAttraction2StepExpect), "republicmp two_states bld_attraction2_step");
    simOk += InstallInlineHook(g_exeBase + RVA_CALC_CUSTOMS_TOURIST_SPAWN, (void*)&H_CalcCustomsTouristSpawn,
                               (void**)&o_calcCustomsTouristSpawn, kCalcCustomsTouristSpawnExpect,
                               sizeof(kCalcCustomsTouristSpawnExpect), "republicmp two_states calc_customs_tourist_spawn");
    simOk += InstallInlineHook(g_exeBase + RVA_BLD_CUSTOMS_TOURIST_STEP, (void*)&H_BldCustomsTouristStep,
                               (void**)&o_bldCustomsTouristStep, kBldCustomsTouristStepExpect,
                               sizeof(kBldCustomsTouristStepExpect), "republicmp two_states bld_customs_tourist_step");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_COMMIT_CRIME, (void*)&H_CitizenCommitCrime,
                               (void**)&o_citizenCommitCrime, kCitizenCommitCrimeExpect,
                               sizeof(kCitizenCommitCrimeExpect), "republicmp two_states citizen_commit_crime");
    simOk += InstallInlineHook(g_exeBase + RVA_CRIME_EXPIRE_CASE, (void*)&H_CrimeExpireCase,
                               (void**)&o_crimeExpireCase, kCrimeExpireCaseExpect,
                               sizeof(kCrimeExpireCaseExpect), "republicmp two_states crime_expire_case");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_FOOT_ARRIVE, (void*)&H_CitizenFootArrive,
                               (void**)&o_citizenFootArrive, kCitizenFootArriveExpect,
                               sizeof(kCitizenFootArriveExpect), "republicmp two_states citizen_foot_arrive");
    simOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_VEHICLE_ALIGHT, (void*)&H_CitizenVehicleAlight,
                               (void**)&o_citizenVehicleAlight, kCitizenVehicleAlightExpect,
                               sizeof(kCitizenVehicleAlightExpect), "republicmp two_states citizen_vehicle_alight");
    simOk += InstallInlineHook(g_exeBase + RVA_ASSIGN_CONSTRUCTION_OFFICE, (void*)&H_AssignConstructionOffice,
                               (void**)&o_assignConstructionOffice, kAssignConstructionOfficeExpect,
                               sizeof(kAssignConstructionOfficeExpect), "republicmp two_states assign_construction_office");
    simOk += InstallBldSimLoopHooks();
    simOk += InstallCitSevereIllnessHook();
    simOk += InstallBldCountLimitHooks();
    return simOk;
}
