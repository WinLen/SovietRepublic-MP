// SPDX-License-Identifier: GPL-3.0-only
#include "two_states_internal.h"

void TwoStates_BindResearch(const TsmHost* host) { if (host) TsmBind(host); }

// ---------------------------------------------------------------------------
// Cross-Module Shared Research & Vehicle License State
// ---------------------------------------------------------------------------

// R14 & R15: Fully Independent State Research Trees
// g_stateResearch[0] = State 1 (West SSR / Host), g_stateResearch[1] = State 2 (East SSR / Client)
StateResearchTree g_stateResearch[2] = {};
StateEngineResearch g_engineResearch[2];
StateEngineVehLicenses g_engineVehLicenses[2];

// ---------------------------------------------------------------------------
// Module-Internal Research / License State & Hook Trampolines
// ---------------------------------------------------------------------------

static bool  g_testUnivClickActive = false;
static int   g_testUnivClickTechIdx = -1;
static float g_testUnivClickRatio = 0.5f;

struct VehTypeKeyCache {
    const BYTE* world = nullptr;
    const BYTE* vecBegin = nullptr;
    int         count = 0;
    uint64_t    worldGen = 0;
    std::vector<std::string> keys;
};

static VehTypeKeyCache g_vehTypeKeyCache;
static const BYTE* g_mountedVehLicenseWorld = nullptr;
static const BYTE* g_mountedVehLicenseBegin = nullptr;
static int         g_mountedVehLicenseCount = 0;
static int         g_mountedVehLicenseState = 0;
static uint64_t    g_mountedVehLicenseGen   = 0;
static uint64_t    g_vehLicenseMountApplyCount = 0;

static const BYTE* g_mountedResearchWorld = nullptr;
static const BYTE* g_mountedResearchBegin = nullptr;
static int         g_mountedResearchCount = 0;
static int         g_mountedResearchState = 0;
static uint64_t    g_mountedResearchGen   = 0;
static uint64_t    g_researchMountApplyCount = 0;

void TwoStates_InvalidateResearchMountCache()
{
    g_mountedResearchState = 0;
}

void TwoStates_InvalidateMountCaches()
{
    g_mountedResearchState = 0;
    g_mountedVehLicenseState = 0;
}

uint64_t TwoStates_GetResearchMountGenForTest()        { return g_mountedResearchGen; }
int      TwoStates_GetResearchMountStateForTest()      { return g_mountedResearchState; }
uint64_t TwoStates_GetResearchMountApplyCountForTest() { return g_researchMountApplyCount; }
int      TwoStates_GetVehLicenseMountStateForTest()    { return g_mountedVehLicenseState; }
uint64_t TwoStates_GetVehLicenseMountApplyCountForTest() { return g_vehLicenseMountApplyCount; }

static const BYTE kBldUnivStepExpect[16] = {
    0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x48, 0x81, 0xEC, 0x38, 0x01, 0x00, 0x00
};
static const BYTE kResearchDailyUpdateExpect[19] = {
    0x40, 0x53,
    0x48, 0x8B, 0x91, 0x80, 0x17, 0x01, 0x00,
    0x45, 0x33, 0xDB,
    0x48, 0x2B, 0x91, 0x78, 0x17, 0x01, 0x00
};
static const BYTE kResearchRecomputeExpect[16] = {
    0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
    0xEC, 0x60, 0x48, 0x8D, 0x15, 0x97, 0x49, 0x5B
};
static const BYTE kUnivResearchPanelExpect[15] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};

typedef void (__fastcall* ResearchDailyUpdateFn)(BYTE* world);
typedef void (__fastcall* ResearchRecomputeFn)(BYTE* world);
typedef BYTE* (__fastcall* UnivResearchPanelFn)(BYTE* world, BYTE* building, BYTE* window,
                                                float* xPtr, float* yPtr, void* a6);

static BldEconFn o_bldUnivStep = nullptr;
static ResearchDailyUpdateFn o_researchDailyUpdate = nullptr;
static UnivResearchPanelFn o_univResearchPanel = nullptr;

// ---------------------------------------------------------------------------
// University & Engine Research Vector Isolation (world + 0x1175C..0x11780)
// ---------------------------------------------------------------------------

static bool SafeReadStridedWorldVecHeader(const BYTE* world, size_t vecOffset, size_t elemStride, int maxCount,
                                          BYTE** outBegin, int* outCount)
{
    if (!world || !outBegin || !outCount || elemStride == 0) return false;
    *outBegin = nullptr;
    *outCount = 0;
    uintptr_t ptrs[2] = {};
    if (!SafeMemcpyPod(ptrs, world + vecOffset, sizeof(ptrs))) return false;
    BYTE* b = (BYTE*)ptrs[0];
    BYTE* e = (BYTE*)ptrs[1];
    if (!b || e < b) return false;
    ptrdiff_t bytes = e - b;
    if (bytes < 0 || (bytes % (ptrdiff_t)elemStride) != 0) return false;
    int count = (int)(bytes / (ptrdiff_t)elemStride);
    if (count <= 0 || count > maxCount) return false;
    *outBegin = b;
    *outCount = count;
    return true;
}

static bool SafeReadResearchVecHeader(const BYTE* world, BYTE** outBegin, int* outCount)
{
    return SafeReadStridedWorldVecHeader(world, WORLD_RESEARCH_VEC_BEGIN_OFFSET, RESEARCH_ITEM_STRIDE, 256, outBegin, outCount);
}

int ComputeBuildingPosKeySafe(const BYTE* bld)
{
    if (!bld) return 0;
    float x = 0.0f, z = 0.0f;
    if (!SafeReadPod((const float*)(bld + OBJ_POS_OFFSET), &x) ||
        !SafeReadPod((const float*)(bld + OBJ_POS_OFFSET + 8), &z)) {
        return 0;
    }
    return TwoStates_BuildingPosKey(x, z);
}

static BYTE* FindWorldBuildingByPosKey(const BYTE* world, int posKey)
{
    if (!world || posKey <= 0) return nullptr;
    BYTE* found = nullptr;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* const* b = *(BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET);
        BYTE* const* e = *(BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET + 8);
        if (b && e > b && (e - b) <= 200000) {
            for (BYTE* const* it = b; it < e; ++it) {
                BYTE* bld = *it;
                if (!bld) continue;
                float x = *(const float*)(bld + OBJ_POS_OFFSET);
                float z = *(const float*)(bld + OBJ_POS_OFFSET + 8);
                if (TwoStates_BuildingPosKey(x, z) == posKey) {
                    found = bld;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = nullptr;
    }
    DiagLog_EndSafeSeh();
    return found;
}

static bool IsBuildingInWorldList(const BYTE* world, const BYTE* target)
{
    if (!world || !target) return false;
    bool present = false;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* const* b = *(const BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET);
        const BYTE* const* e = *(const BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET + 8);
        if (!b || e <= b) {
            present = true; // standalone test world without populated building vector
        } else if ((e - b) <= 200000) {
            for (const BYTE* const* it = b; it < e; ++it) {
                if (*it == target) {
                    present = true;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        present = false;
    }
    DiagLog_EndSafeSeh();
    return present;
}

static int FindEngineResearchIdxForBuilding(const BYTE* world, const BYTE* building)
{
    if (!world || !building) return -1;
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) return -1;
    for (int i = 0; i < count; ++i) {
        const BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
        const BYTE* curBld = nullptr;
        if (SafeReadPod((const BYTE* const*)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), &curBld) &&
            curBld == building) {
            return i;
        }
    }
    return -1;
}

static void RecomputeEngineResearchFallback(BYTE* world)
{
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) return;
    uint8_t unlocks[WORLD_RESEARCH_UNLOCKS_BYTES] = {};
    for (int i = 0; i < count; ++i) {
        BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
        float prog = 0.0f;
        uint8_t initTech = 0;
        SafeReadPod((const float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET), &prog);
        SafeReadPod(item + RESEARCH_ITEM_INITIAL_OFFSET, &initTech);
        if (prog >= 1.0f) {
            uint8_t zeroAvail = 0;
            SafeWritePod(item + RESEARCH_ITEM_AVAILABLE_OFFSET, zeroAvail);
            unlocks[(unsigned)i % WORLD_RESEARCH_UNLOCKS_BYTES] = 1;
        } else if (initTech != 0) {
            uint8_t oneAvail = 1;
            SafeWritePod(item + RESEARCH_ITEM_AVAILABLE_OFFSET, oneAvail);
        }
    }
    SafeMemcpyPod(world + WORLD_RESEARCH_UNLOCKS_OFFSET, unlocks, WORLD_RESEARCH_UNLOCKS_BYTES);
}

static void RecomputeEngineResearch(BYTE* world)
{
    if (!world) return;
    if (g_exeBase && TwoStates_MatchesResearchRecomputePrologueForTest(
                         g_exeBase + RVA_RESEARCH_RECOMPUTE, sizeof(kResearchRecomputeExpect))) {
        DiagLog_BeginSafeSeh();
        __try {
            ResearchRecomputeFn fn = (ResearchRecomputeFn)(g_exeBase + RVA_RESEARCH_RECOMPUTE);
            fn(world);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
        return;
    }
    RecomputeEngineResearchFallback(world);
}

static bool SetEngineResearchBuildingAt(BYTE* world, BYTE* building, int techIdx)
{
    if (!world || !building) return false;
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) return false;
    if (techIdx >= count) return false;
    for (int i = 0; i < count; ++i) {
        BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
        BYTE* curBld = nullptr;
        if (SafeReadPod((BYTE* const*)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), &curBld) &&
            curBld == building) {
            BYTE* zero = nullptr;
            SafeWritePod((BYTE**)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), zero);
        }
    }
    if (techIdx >= 0) {
        BYTE* targetItem = begin + (size_t)techIdx * RESEARCH_ITEM_STRIDE;
        SafeWritePod((BYTE**)(targetItem + RESEARCH_ITEM_BUILDING_PTR_OFFSET), building);
    }
    return true;
}

static void FinalizePreviousBuildingResearch(BYTE* world, BYTE* begin, int count, BYTE* building, int techIdx)
{
    (void)world;
    (void)techIdx;
    for (int i = 0; i < count; ++i) {
        BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
        BYTE* curBld = nullptr;
        if (!SafeReadPod((BYTE* const*)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), &curBld) ||
            curBld != building) {
            continue;
        }
        // Retain accumulated curProg without writing 1.0f (eliminates instant science exploit).
        BYTE* zero = nullptr;
        SafeWritePod((BYTE**)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), zero);
    }
}

static bool ApplyEngineResearchSelectionAt(BYTE* world, BYTE* building, int techIdx)
{
    if (!world || !building) return false;
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) return false;
    if (techIdx >= count) return false;

    FinalizePreviousBuildingResearch(world, begin, count, building, techIdx);
    if (techIdx < 0) return true;

    BYTE* targetItem = begin + (size_t)techIdx * RESEARCH_ITEM_STRIDE;
    float targetProg = 0.0f;
    BYTE* targetBld = nullptr;
    SafeReadPod((const float*)(targetItem + RESEARCH_ITEM_PROGRESS_OFFSET), &targetProg);
    SafeReadPod((BYTE* const*)(targetItem + RESEARCH_ITEM_BUILDING_PTR_OFFSET), &targetBld);
    if (targetProg >= 1.0f || (targetProg >= 0.98f && targetBld == nullptr)) {
        if (targetProg < 1.0f) {
            SafeWritePod((float*)(targetItem + RESEARCH_ITEM_PROGRESS_OFFSET), 1.0f);
        }
        BYTE* zero = nullptr;
        SafeWritePod((BYTE**)(targetItem + RESEARCH_ITEM_BUILDING_PTR_OFFSET), zero);
        RecomputeEngineResearch(world);
    } else {
        uint8_t oneAvail = 1;
        SafeWritePod(targetItem + RESEARCH_ITEM_AVAILABLE_OFFSET, oneAvail);
        SafeWritePod((BYTE**)(targetItem + RESEARCH_ITEM_BUILDING_PTR_OFFSET), building);
    }
    return true;
}

static bool HealWorldOrphanedNearCompleteResearch(BYTE* world)
{
    if (!world) return false;
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) return false;
    bool healed = false;
    for (int i = 0; i < count; ++i) {
        BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
        float prog = 0.0f;
        BYTE* curBld = nullptr;
        SafeReadPod((const float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET), &prog);
        SafeReadPod((BYTE* const*)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), &curBld);
        if (prog >= 0.98f && prog < 1.0f && curBld == nullptr) {
            SafeWritePod((float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET), 1.0f);
            healed = true;
        }
    }
    if (healed) {
        RecomputeEngineResearch(world);
    }
    return healed;
}

static bool HealSliceOrphanedNearCompleteResearch(StateEngineResearch* slice)
{
    if (!slice) return false;
    bool healed = false;
    for (size_t i = 0; i < slice->items.size(); ++i) {
        EngineResearchItemSlice& it = slice->items[i];
        if (it.progress >= 0.98f && it.progress < 1.0f &&
            it.researchingBld == nullptr && it.bldPosKey <= 0) {
            it.progress = 1.0f;
            it.available = 0;
            healed = true;
        }
    }
    return healed;
}

bool IsTwoStatesResearchOrLicenseActive()
{
    if (g_beforeLoadPrepared || Determinism_WorldLoading()) return false;
    if (TwoStates_IsActive()) return true;
    if (g_loadedFromCoopSave && TwoStates_GetMode() == COOP_MODE_TWO_STATES) return true;
    return false;
}

void SyncSummaryResearchFromSlice(int stateOwner, const StateEngineResearch& src)
{
    if (stateOwner != 1 && stateOwner != 2) return;
    if (src.items.empty()) return;
    StateResearchTree& tree = g_stateResearch[stateOwner - 1];
    int activeIdx = -1;
    float activeProg = 0.0f;
    for (size_t i = 0; i < src.items.size(); ++i) {
        const EngineResearchItemSlice& s = src.items[i];
        if (i < 32) {
            if (s.progress > 0.0f || s.researchingBld != nullptr || s.bldPosKey > 0) {
                tree.techProgress[i] = s.progress;
            }
            uint32_t bit = (1u << i);
            if (s.progress >= 1.0f) {
                if ((tree.unlockedMask & bit) == 0) {
                    tree.unlockedMask |= bit;
                    tree.researchedCount++;
                }
            }
        }
        if (activeIdx < 0 && (s.researchingBld != nullptr || s.bldPosKey > 0)) {
            activeIdx = (int)i;
            activeProg = s.progress;
        }
    }
    if (activeIdx >= 0) {
        tree.activeTechId = activeIdx;
        tree.activeTechProgress = activeProg;
    } else if (tree.activeTechId > 0 && (size_t)tree.activeTechId < src.items.size() &&
               src.items[(size_t)tree.activeTechId].progress >= 1.0f) {
        tree.activeTechId = 0;
        tree.activeTechProgress = 0.0f;
    }
}

struct RawWorldResearchItem {
    uint8_t available;
    float   progress;
    BYTE*   researchingBld;
    int32_t timerE0;
};

static bool ReadWorldResearchRawSeh(const BYTE* world, uint8_t* outUnlocks, const BYTE* begin, int count,
                                    RawWorldResearchItem* outRaw)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        memcpy(outUnlocks, world + WORLD_RESEARCH_UNLOCKS_OFFSET, WORLD_RESEARCH_UNLOCKS_BYTES);
        for (int i = 0; i < count; ++i) {
            const BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
            outRaw[i].available      = *(item + RESEARCH_ITEM_AVAILABLE_OFFSET);
            outRaw[i].progress       = *(const float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET);
            outRaw[i].researchingBld = *(BYTE* const*)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET);
            outRaw[i].timerE0        = *(const int32_t*)(item + RESEARCH_ITEM_TIMER_E0_OFFSET);
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool WriteResearchSlicePodSeh(BYTE* world, const uint8_t* unlocks, BYTE* begin, int n,
                                     const EngineResearchItemSlice* items)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        memcpy(world + WORLD_RESEARCH_UNLOCKS_OFFSET, unlocks, WORLD_RESEARCH_UNLOCKS_BYTES);
        for (int i = 0; i < n; ++i) {
            BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
            const EngineResearchItemSlice& s = items[i];
            *(item + RESEARCH_ITEM_AVAILABLE_OFFSET) = s.available;
            *(float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET) = s.progress;
            *(BYTE**)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET) = s.researchingBld;
            *(int32_t*)(item + RESEARCH_ITEM_TIMER_E0_OFFSET) = s.timerE0;
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void CaptureWorldResearchToSlice(const BYTE* world, StateEngineResearch* dst, int filterOwnerState)
{
    if (!world || !dst) return;
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) {
        SafeMemcpyPod(dst->unlocks, world + WORLD_RESEARCH_UNLOCKS_OFFSET, WORLD_RESEARCH_UNLOCKS_BYTES);
        return;
    }

    static thread_local std::vector<EngineResearchItemSlice> t_prevItems;
    static thread_local std::vector<RawWorldResearchItem> t_rawItems;
    t_prevItems.swap(dst->items);
    dst->items.resize((size_t)count);
    t_rawItems.resize((size_t)count);

    if (!ReadWorldResearchRawSeh(world, dst->unlocks, begin, count, t_rawItems.data())) {
        t_prevItems.swap(dst->items);
        return;
    }

    for (int i = 0; i < count; ++i) {
        EngineResearchItemSlice s = {};
        s.available      = t_rawItems[(size_t)i].available;
        s.progress       = t_rawItems[(size_t)i].progress;
        s.researchingBld = t_rawItems[(size_t)i].researchingBld;
        s.timerE0        = t_rawItems[(size_t)i].timerE0;
        if (s.researchingBld && (filterOwnerState == 1 || filterOwnerState == 2)) {
            if (LookupBuildingOwnerCached(s.researchingBld) != filterOwnerState) {
                s.researchingBld = nullptr;
                if (s.progress < 1.0f) {
                    s.progress = ((size_t)i < t_prevItems.size()) ? t_prevItems[(size_t)i].progress : 0.0f;
                }
            }
        }
        s.bldPosKey = s.researchingBld ? ComputeBuildingPosKeySafe(s.researchingBld) : 0;
        if (s.bldPosKey <= 0 && s.researchingBld != nullptr && (size_t)i < t_prevItems.size()) {
            s.bldPosKey = t_prevItems[(size_t)i].bldPosKey;
        }
        dst->items[(size_t)i] = s;
    }
    dst->hasUnresolvedBldPtr = false;
    dst->gen++;
    dst->initialized = true;
    dst->loadedFromMeta = false;
    if (filterOwnerState == 1 || filterOwnerState == 2) {
        SyncSummaryResearchFromSlice(filterOwnerState, *dst);
    }
}

void WriteSliceToWorldResearch(BYTE* world, StateEngineResearch* src)
{
    if (!world || !src || !src->initialized) return;
    const int targetState = (src == &g_engineResearch[0]) ? 1 : ((src == &g_engineResearch[1]) ? 2 : 0);

    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) {
        SafeMemcpyPod(world + WORLD_RESEARCH_UNLOCKS_OFFSET, src->unlocks, WORLD_RESEARCH_UNLOCKS_BYTES);
        g_mountedResearchState = 0;
        return;
    }

    if (!src->hasUnresolvedBldPtr && targetState != 0 &&
        g_mountedResearchWorld == world &&
        g_mountedResearchBegin == begin &&
        g_mountedResearchCount == count &&
        g_mountedResearchState == targetState &&
        g_mountedResearchGen == src->gen) {
        return;
    }

    const int n = (int)std::min((size_t)count, src->items.size());
    bool anyUnresolved = false;
    bool anyResolved = false;
    for (int i = 0; i < n; ++i) {
        EngineResearchItemSlice& s = src->items[(size_t)i];
        if (!s.researchingBld && s.bldPosKey > 0) {
            s.researchingBld = FindWorldBuildingByPosKey(world, s.bldPosKey);
            if (s.researchingBld) anyResolved = true;
            else anyUnresolved = true;
        }
    }
    if (anyResolved) {
        src->gen++;
    }
    src->hasUnresolvedBldPtr = anyUnresolved;

    if (WriteResearchSlicePodSeh(world, src->unlocks, begin, n, src->items.data())) {
        g_researchMountApplyCount++;
        g_mountedResearchWorld = world;
        g_mountedResearchBegin = begin;
        g_mountedResearchCount = count;
        g_mountedResearchState = anyUnresolved ? 0 : targetState;
        g_mountedResearchGen   = src->gen;
    } else {
        g_mountedResearchState = 0;
    }
}

void ResetEngineResearchSlices()
{
    for (int s = 0; s < 2; ++s) {
        memset(g_engineResearch[s].unlocks, 0, sizeof(g_engineResearch[s].unlocks));
        g_engineResearch[s].items.clear();
        g_engineResearch[s].hasUnresolvedBldPtr = false;
        g_engineResearch[s].gen++;
        g_engineResearch[s].initialized = false;
        g_engineResearch[s].loadedFromMeta = false;
    }
    g_mountedResearchWorld = nullptr;
    g_mountedResearchBegin = nullptr;
    g_mountedResearchCount = 0;
    g_mountedResearchState = 0;
    g_mountedResearchGen   = 0;
}

int ResolveActiveResearchWorldState()
{
    int st = g_inCommand ? g_cmdOwner : (g_inSimStep ? 1 : TwoStates_LocalState());
    return (st == 1 || st == 2) ? st : 1;
}

static void Fnv1aMixBytes(unsigned long long& h, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
}

void EnsureEngineResearchInitialized(BYTE* world)
{
    if (!world) return;
    if (g_engineResearch[0].initialized && g_engineResearch[1].initialized &&
        !g_engineResearch[0].loadedFromMeta && !g_engineResearch[1].loadedFromMeta &&
        !g_engineResearch[0].hasUnresolvedBldPtr && !g_engineResearch[1].hasUnresolvedBldPtr) {
        return;
    }
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) return;
    bool needWriteToWorld = false;
    if (!g_engineResearch[0].initialized || !g_engineResearch[1].initialized) {
        HealWorldOrphanedNearCompleteResearch(world);
    }
    for (int s = 0; s < 2; ++s) {
        if (!g_engineResearch[s].initialized) {
            CaptureWorldResearchToSlice(world, &g_engineResearch[s], s + 1);
            needWriteToWorld = true;
        }
    }
    for (int s = 0; s < 2; ++s) {
        int stateOwner = s + 1;
        bool anyUnresolved = false;
        bool anyResolved = false;
        for (size_t i = 0; i < g_engineResearch[s].items.size(); ++i) {
            EngineResearchItemSlice& it = g_engineResearch[s].items[i];
            if (!it.researchingBld && it.bldPosKey > 0) {
                it.researchingBld = FindWorldBuildingByPosKey(world, it.bldPosKey);
                if (it.researchingBld) anyResolved = true;
                else anyUnresolved = true;
            }
        }
        if (anyResolved) g_engineResearch[s].gen++;
        g_engineResearch[s].hasUnresolvedBldPtr = anyUnresolved;

        bool healed = HealSliceOrphanedNearCompleteResearch(&g_engineResearch[s]);
        if (g_engineResearch[s].loadedFromMeta || healed) {
            g_mountedResearchState = 0;
            WriteSliceToWorldResearch(world, &g_engineResearch[s]);
            RecomputeEngineResearch(world);
            SafeMemcpyPod(g_engineResearch[s].unlocks, world + WORLD_RESEARCH_UNLOCKS_OFFSET, WORLD_RESEARCH_UNLOCKS_BYTES);
            int n = (int)std::min((size_t)count, g_engineResearch[s].items.size());
            for (int i = 0; i < n; ++i) {
                const BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
                SafeReadPod(item + RESEARCH_ITEM_AVAILABLE_OFFSET, &g_engineResearch[s].items[(size_t)i].available);
            }
            g_engineResearch[s].loadedFromMeta = false;
            g_engineResearch[s].gen++;
            g_mountedResearchState = 0;
            SyncSummaryResearchFromSlice(stateOwner, g_engineResearch[s]);
            needWriteToWorld = true;
        }
    }
    if (needWriteToWorld) {
        g_mountedResearchState = 0;
        int activeState = ResolveActiveResearchWorldState();
        WriteSliceToWorldResearch(world, &g_engineResearch[activeState - 1]);
    }
}

static void BldUnivStepFallback(BYTE* world, BYTE* building)
{
    int idx = FindEngineResearchIdxForBuilding(world, building);
    if (idx < 0) return;
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count) || idx >= count) return;
    BYTE* item = begin + (size_t)idx * RESEARCH_ITEM_STRIDE;
    float prog = 0.0f;
    int32_t reqWork = 100;
    SafeReadPod((const float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET), &prog);
    SafeReadPod((const int32_t*)(item + RESEARCH_ITEM_REQ_WORK_OFFSET), &reqWork);
    float delta = (reqWork > 0) ? (10.0f / (float)reqWork) : 0.25f;
    prog += delta;
    if (prog >= 1.0f) {
        prog = 1.0f;
        BYTE* nullBld = nullptr;
        SafeWritePod((float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET), prog);
        SafeWritePod((BYTE**)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), nullBld);
        RecomputeEngineResearch(world);
    } else {
        SafeWritePod((float*)(item + RESEARCH_ITEM_PROGRESS_OFFSET), prog);
    }
}

static __int64 __fastcall H_BldUnivStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                        uintptr_t a5, uintptr_t a6)
{
    bool twoStates = IsTwoStatesResearchOrLicenseActive() && world && building;
    int owner = twoStates ? LookupBuildingOwnerCached(building) : 1;
    if (owner != 1 && owner != 2) owner = 1;
    if (twoStates) {
        EnsureEngineResearchInitialized(world);
        WriteSliceToWorldResearch(world, &g_engineResearch[owner - 1]);
    }
    bool econActive = (g_inSimStep && !TwoStates_IsInEconHook() && twoStates)
                          ? BeginEconAttribution(world, owner)
                          : false;
    g_mountedResearchState = 0;
    __int64 r = 0;
    if (o_bldUnivStep) {
        r = o_bldUnivStep(world, building, a3, a4, a5, a6);
    } else {
        BldUnivStepFallback(world, building);
    }
    if (building) {
        SanitizeBuildingEffectiveCapFloat(building);
    }
    if (econActive) {
        EndEconAttribution(owner);
    }
    if (twoStates) {
        CaptureWorldResearchToSlice(world, &g_engineResearch[owner - 1], owner);
        int restoreState = ResolveActiveResearchWorldState();
        WriteSliceToWorldResearch(world, &g_engineResearch[restoreState - 1]);
    }
    return r;
}

static void ResearchDailyUpdateFallback(BYTE* world)
{
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count)) return;
    for (int i = 0; i < count; ++i) {
        BYTE* item = begin + (size_t)i * RESEARCH_ITEM_STRIDE;
        int32_t timer = 0;
        if (SafeReadPod((const int32_t*)(item + RESEARCH_ITEM_TIMER_E0_OFFSET), &timer) && timer > 0) {
            timer--;
            SafeWritePod((int32_t*)(item + RESEARCH_ITEM_TIMER_E0_OFFSET), timer);
        }
        BYTE* bld = nullptr;
        if (SafeReadPod((BYTE* const*)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), &bld) && bld) {
            if (!IsBuildingInWorldList(world, bld)) {
                BYTE* nullBld = nullptr;
                SafeWritePod((BYTE**)(item + RESEARCH_ITEM_BUILDING_PTR_OFFSET), nullBld);
            }
        }
    }
}

static void __fastcall H_ResearchDailyUpdate(BYTE* world)
{
    if (!IsTwoStatesResearchOrLicenseActive() || !world) {
        g_mountedResearchState = 0;
        if (o_researchDailyUpdate) o_researchDailyUpdate(world);
        else ResearchDailyUpdateFallback(world);
        return;
    }
    EnsureEngineResearchInitialized(world);
    for (int s = 1; s <= 2; ++s) {
        WriteSliceToWorldResearch(world, &g_engineResearch[s - 1]);
        if (o_researchDailyUpdate) o_researchDailyUpdate(world);
        else ResearchDailyUpdateFallback(world);
        g_mountedResearchState = 0;
        CaptureWorldResearchToSlice(world, &g_engineResearch[s - 1], s);
    }
    int restoreState = ResolveActiveResearchWorldState();
    WriteSliceToWorldResearch(world, &g_engineResearch[restoreState - 1]);
}

static BYTE* __fastcall H_UnivResearchPanel(BYTE* world, BYTE* building, BYTE* window,
                                            float* xPtr, float* yPtr, void* a6)
{
    if (!world || !building) {
        return o_univResearchPanel ? o_univResearchPanel(world, building, window, xPtr, yPtr, a6) : nullptr;
    }
    bool twoStates = IsTwoStatesResearchOrLicenseActive();
    if (!twoStates && !Commands_Capturing() && !Determinism_SessionRunning() &&
        SaveSync_GetState() == SESSION_IDLE && !g_testUnivClickActive) {
        return o_univResearchPanel ? o_univResearchPanel(world, building, window, xPtr, yPtr, a6) : nullptr;
    }
    int bldOwner = twoStates ? LookupBuildingOwnerCached(building) : 1;
    if (bldOwner != 1 && bldOwner != 2) bldOwner = 1;
    int localState = TwoStates_LocalState();

    if (twoStates) {
        EnsureEngineResearchInitialized(world);
        WriteSliceToWorldResearch(world, &g_engineResearch[bldOwner - 1]);
    } else if (Commands_Capturing() || Determinism_SessionRunning()) {
        HealWorldOrphanedNearCompleteResearch(world);
    }

    int oldTechIdx = FindEngineResearchIdxForBuilding(world, building);
    unsigned oldRatioBits = 0;
    SafeReadPod((const unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET), &oldRatioBits);

    BYTE* ret = nullptr;
    if (o_univResearchPanel) {
        ret = o_univResearchPanel(world, building, window, xPtr, yPtr, a6);
    } else if (g_testUnivClickActive) {
        SetEngineResearchBuildingAt(world, building, g_testUnivClickTechIdx);
        unsigned clickBits = 0;
        memcpy(&clickBits, &g_testUnivClickRatio, sizeof(clickBits));
        SafeWritePod((unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET), clickBits);
    }
    g_mountedResearchState = 0;

    int newTechIdx = FindEngineResearchIdxForBuilding(world, building);
    unsigned newRatioBits = oldRatioBits;
    SafeReadPod((const unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET), &newRatioBits);

    if (newTechIdx != oldTechIdx || newRatioBits != oldRatioBits) {
        if (twoStates && bldOwner != localState) {
            if (newTechIdx != oldTechIdx) SetEngineResearchBuildingAt(world, building, oldTechIdx);
            SafeWritePod((unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET), oldRatioBits);
            TwoStates_CheckBuildingControlAllowed(building,
                Tr("управление исследованиями чужого ВУЗа", "controlling foreign university research"));
        } else if (SaveSync_IsWaitingLobbyActive()) {
            if (newTechIdx != oldTechIdx) SetEngineResearchBuildingAt(world, building, oldTechIdx);
            SafeWritePod((unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET), oldRatioBits);
        } else if (Commands_Capturing()) {
            if (newTechIdx != oldTechIdx) SetEngineResearchBuildingAt(world, building, oldTechIdx);
            SafeWritePod((unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET), oldRatioBits);
            BuildingSwitches_CaptureUnivResearch((unsigned long long)building, bldOwner,
                                                 oldTechIdx, newTechIdx, oldRatioBits, newRatioBits);
        } else {
            if (!BuildingSwitches_CaptureUnivResearch((unsigned long long)building, bldOwner,
                                                      oldTechIdx, newTechIdx, oldRatioBits, newRatioBits)) {
                SafeWritePod((unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET), newRatioBits);
                if (newTechIdx != oldTechIdx) {
                    TwoStates_ApplyUnivResearchSelection(world, building, bldOwner, newTechIdx);
                }
            }
        }
    }

    if (twoStates) {
        int restoreState = ResolveActiveResearchWorldState();
        WriteSliceToWorldResearch(world, &g_engineResearch[restoreState - 1]);
    }
    return ret;
}

bool TwoStates_ApplyUnivResearchSelection(BYTE* world, BYTE* building, int ownerState, int techIdx)
{
    if (!world || !building) return false;
    int state = (ownerState == 2) ? 2 : 1;
    bool twoStates = IsTwoStatesResearchOrLicenseActive();
    if (twoStates) {
        EnsureEngineResearchInitialized(world);
        WriteSliceToWorldResearch(world, &g_engineResearch[state - 1]);
    }
    bool ok = ApplyEngineResearchSelectionAt(world, building, techIdx);
    g_mountedResearchState = 0;
    if (twoStates && ok) {
        CaptureWorldResearchToSlice(world, &g_engineResearch[state - 1], state);
        int restoreState = ResolveActiveResearchWorldState();
        WriteSliceToWorldResearch(world, &g_engineResearch[restoreState - 1]);
    }
    return ok;
}

int TwoStates_GetUnivResearchIdxForBuilding(const BYTE* world, const BYTE* building, int stateOwner)
{
    if (!world || !building) return -1;
    if (IsTwoStatesResearchOrLicenseActive() && (stateOwner == 1 || stateOwner == 2) &&
        g_engineResearch[stateOwner - 1].initialized) {
        const std::vector<EngineResearchItemSlice>& items = g_engineResearch[stateOwner - 1].items;
        int targetKey = ComputeBuildingPosKeySafe(building);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].researchingBld == building ||
                (targetKey > 0 && items[i].bldPosKey == targetKey)) {
                return (int)i;
            }
        }
        return -1;
    }
    return FindEngineResearchIdxForBuilding(world, building);
}

float TwoStates_GetEngineResearchProgress(const BYTE* world, int stateOwner, int techIdx)
{
    if (techIdx < 0) return 0.0f;
    if (IsTwoStatesResearchOrLicenseActive() && (stateOwner == 1 || stateOwner == 2) &&
        g_engineResearch[stateOwner - 1].initialized) {
        const std::vector<EngineResearchItemSlice>& items = g_engineResearch[stateOwner - 1].items;
        return ((size_t)techIdx < items.size()) ? items[(size_t)techIdx].progress : 0.0f;
    }
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadResearchVecHeader(world, &begin, &count) || techIdx >= count) return 0.0f;
    float prog = 0.0f;
    SafeReadPod((const float*)(begin + (size_t)techIdx * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET), &prog);
    return prog;
}

uint8_t TwoStates_GetEngineResearchUnlockByte(const BYTE* world, int stateOwner, unsigned byteIdx)
{
    if (byteIdx >= WORLD_RESEARCH_UNLOCKS_BYTES) return 0;
    if (IsTwoStatesResearchOrLicenseActive() && (stateOwner == 1 || stateOwner == 2) &&
        g_engineResearch[stateOwner - 1].initialized) {
        return g_engineResearch[stateOwner - 1].unlocks[byteIdx];
    }
    uint8_t val = 0;
    if (world) SafeReadPod(world + WORLD_RESEARCH_UNLOCKS_OFFSET + byteIdx, &val);
    return val;
}

void TwoStates_SetEngineResearchUnlockByteForTest(int stateOwner, unsigned byteIdx, uint8_t val)
{
    if ((stateOwner == 1 || stateOwner == 2) && byteIdx < WORLD_RESEARCH_UNLOCKS_BYTES) {
        g_engineResearch[stateOwner - 1].initialized = true;
        g_engineResearch[stateOwner - 1].unlocks[byteIdx] = val;
        g_engineResearch[stateOwner - 1].gen++;
        g_mountedResearchState = 0;
    }
}

bool TwoStates_HashEngineResearch(unsigned long long* inOutHash)
{
    if (!inOutHash || !IsTwoStatesResearchOrLicenseActive()) return false;
    if (!g_engineResearch[0].initialized && !g_engineResearch[1].initialized) return false;
    unsigned long long h = *inOutHash;
    for (int s = 0; s < 2; ++s) {
        const StateEngineResearch& slice = g_engineResearch[s];
        Fnv1aMixBytes(h, slice.unlocks, WORLD_RESEARCH_UNLOCKS_BYTES);
        int limit = (int)std::min<size_t>(slice.items.size(), 256u);
        Fnv1aMixBytes(h, &limit, sizeof(limit));
        for (int i = 0; i < limit; ++i) {
            const EngineResearchItemSlice& it = slice.items[(size_t)i];
            Fnv1aMixBytes(h, &it.available, sizeof(it.available));
            Fnv1aMixBytes(h, &it.progress, sizeof(it.progress));
        }
    }
    *inOutHash = h;
    return true;
}

void TwoStates_UnivResearchPanelForTest(BYTE* world, BYTE* building, int clickTechIdx, float clickRatio)
{
    g_testUnivClickActive = true;
    g_testUnivClickTechIdx = clickTechIdx;
    g_testUnivClickRatio = clickRatio;
    H_UnivResearchPanel(world, building, nullptr, nullptr, nullptr, nullptr);
    g_testUnivClickActive = false;
}

void TwoStates_BldUnivStepForTest(BYTE* world, BYTE* building)
{
    H_BldUnivStep(world, building, 0, 0, 0, 0);
}

void TwoStates_ResearchDailyUpdateForTest(BYTE* world)
{
    H_ResearchDailyUpdate(world);
}

void TwoStates_RecomputeEngineResearchForTest(BYTE* world)
{
    RecomputeEngineResearch(world);
}

bool TwoStates_MatchesResearchRecomputePrologueForTest(const BYTE* bytes, size_t len)
{
    if (!bytes || len < sizeof(kResearchRecomputeExpect)) return false;
    return SafeBytesMatch(bytes, kResearchRecomputeExpect, sizeof(kResearchRecomputeExpect));
}

// ---------------------------------------------------------------------------
// Per-State Vehicle Production License Isolation (world + 0x12840, +0x9C88)
// ---------------------------------------------------------------------------

static bool SafeReadVehTypeVecHeader(const BYTE* world, BYTE** outBegin, int* outCount)
{
    return SafeReadStridedWorldVecHeader(world, WORLD_VEHICLE_TYPES_OFFSET, WORLD_VEHICLE_TYPE_STRIDE, 8192, outBegin, outCount);
}

static std::string NormalizeVehTypeScriptName(const char raw[64], int idx)
{
    std::string key;
    key.reserve(64);
    for (size_t i = 0; i < 64 && raw[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)raw[i];
        if (c < 0x20 || c > 0x7E || c == ',' || c == ';') continue;
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        key.push_back((char)c);
    }
    if (!key.empty()) return key;
    return "__idx_" + std::to_string(idx);
}

static bool ReadAllVehTypeScriptNamesSeh(const BYTE* begin, int count, std::array<char, 64>* outRaw)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            const BYTE* type = begin + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE;
            memcpy(outRaw[i].data(), type + VEH_TYPE_SCRIPT_NAME_OFFSET, 64);
            outRaw[i][63] = '\0';
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool ReadVehLicenseFlagsSeh(const BYTE* begin, int count, uint8_t* outFlags)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            const BYTE* type = begin + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE;
            outFlags[i] = (*(type + VEH_TYPE_HAS_LICENCE_OFFSET) != 0) ? 1u : 0u;
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool WriteVehLicenseFlagsSeh(BYTE* begin, int count, const uint8_t* flags)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            BYTE* type = begin + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE;
            *(type + VEH_TYPE_HAS_LICENCE_OFFSET) = flags[i];
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool EnsureVehTypeKeyCache(const BYTE* world, BYTE** outBegin, int* outCount)
{
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadVehTypeVecHeader(world, &begin, &count)) return false;
    if (outBegin) *outBegin = begin;
    if (outCount) *outCount = count;

    if (g_vehTypeKeyCache.world == world &&
        g_vehTypeKeyCache.vecBegin == begin &&
        g_vehTypeKeyCache.count == count &&
        (int)g_vehTypeKeyCache.keys.size() == count) {
        return true;
    }

    static thread_local std::vector<std::array<char, 64>> t_rawNames;
    t_rawNames.resize((size_t)count);
    if (!ReadAllVehTypeScriptNamesSeh(begin, count, t_rawNames.data())) {
        for (int i = 0; i < count; ++i) t_rawNames[(size_t)i].fill('\0');
    }

    g_vehTypeKeyCache.keys.resize((size_t)count);
    for (int i = 0; i < count; ++i) {
        g_vehTypeKeyCache.keys[(size_t)i] = NormalizeVehTypeScriptName(t_rawNames[(size_t)i].data(), i);
    }
    g_vehTypeKeyCache.world    = world;
    g_vehTypeKeyCache.vecBegin = begin;
    g_vehTypeKeyCache.count    = count;
    g_vehTypeKeyCache.worldGen++;
    g_mountedVehLicenseState = 0;
    return true;
}

static bool HasLicenseKeyInSlice(const StateEngineVehLicenses& slice, const std::string& key)
{
    if (key.empty()) return false;
    for (size_t i = 0; i < slice.ownedKeys.size(); ++i) {
        if (slice.ownedKeys[i] == key) return true;
    }
    return false;
}

static void SyncVehSliceCachedFlags(const StateEngineVehLicenses& slice, int count)
{
    if (slice.cachedFlagsGen == slice.gen &&
        slice.cachedFlagsWorldGen == g_vehTypeKeyCache.worldGen &&
        (int)slice.cachedFlags.size() == count) {
        return;
    }
    slice.cachedFlags.assign((size_t)count, 0u);
    if (slice.ownedKeys.size() > 16u) {
        std::unordered_set<std::string> ownedSet(slice.ownedKeys.begin(), slice.ownedKeys.end());
        for (int i = 0; i < count; ++i) {
            if (ownedSet.find(g_vehTypeKeyCache.keys[(size_t)i]) != ownedSet.end()) {
                slice.cachedFlags[(size_t)i] = 1u;
            }
        }
    } else if (!slice.ownedKeys.empty()) {
        for (int i = 0; i < count; ++i) {
            if (HasLicenseKeyInSlice(slice, g_vehTypeKeyCache.keys[(size_t)i])) {
                slice.cachedFlags[(size_t)i] = 1u;
            }
        }
    }
    slice.cachedFlagsGen = slice.gen;
    slice.cachedFlagsWorldGen = g_vehTypeKeyCache.worldGen;
}

void AddLicenseKeyToSlice(StateEngineVehLicenses* slice, const std::string& key)
{
    if (!slice || key.empty()) return;
    if (!HasLicenseKeyInSlice(*slice, key)) {
        slice->ownedKeys.push_back(key);
        slice->gen++;
    }
}

static void RemoveLicenseKeyFromSlice(StateEngineVehLicenses* slice, const std::string& key)
{
    if (!slice || key.empty()) return;
    const size_t before = slice->ownedKeys.size();
    slice->ownedKeys.erase(
        std::remove(slice->ownedKeys.begin(), slice->ownedKeys.end(), key),
        slice->ownedKeys.end());
    if (slice->ownedKeys.size() != before) {
        slice->gen++;
    }
}

void CaptureWorldVehLicensesToSlice(const BYTE* world, StateEngineVehLicenses* dst)
{
    if (!world || !dst) return;
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureVehTypeKeyCache(world, &begin, &count)) return;

    static thread_local std::vector<uint8_t> t_tmpFlags;
    t_tmpFlags.resize((size_t)count);
    if (!ReadVehLicenseFlagsSeh(begin, count, t_tmpFlags.data())) return;

    if (dst->initialized &&
        dst->cachedFlagsGen == dst->gen &&
        dst->cachedFlagsWorldGen == g_vehTypeKeyCache.worldGen &&
        t_tmpFlags == dst->cachedFlags) {
        dst->loadedFromMeta = false;
        return;
    }

    dst->ownedKeys.clear();
    for (int i = 0; i < count; ++i) {
        if (t_tmpFlags[(size_t)i] != 0) {
            dst->ownedKeys.push_back(g_vehTypeKeyCache.keys[(size_t)i]);
        }
    }
    dst->cachedFlags.swap(t_tmpFlags);
    dst->gen++;
    dst->cachedFlagsGen = dst->gen;
    dst->cachedFlagsWorldGen = g_vehTypeKeyCache.worldGen;
    dst->initialized = true;
    dst->loadedFromMeta = false;
}

void WriteSliceToWorldVehLicenses(BYTE* world, const StateEngineVehLicenses* src)
{
    if (!world || !src || !src->initialized) return;
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureVehTypeKeyCache(world, &begin, &count)) return;

    const int targetState = (src == &g_engineVehLicenses[0]) ? 1 : ((src == &g_engineVehLicenses[1]) ? 2 : 0);
    SyncVehSliceCachedFlags(*src, count);

    if (targetState != 0 &&
        g_mountedVehLicenseWorld == world &&
        g_mountedVehLicenseBegin == begin &&
        g_mountedVehLicenseCount == count) {
        if (g_mountedVehLicenseState == targetState && g_mountedVehLicenseGen == src->gen) {
            return;
        }
        if ((g_mountedVehLicenseState == 1 || g_mountedVehLicenseState == 2) &&
            g_mountedVehLicenseGen == g_engineVehLicenses[g_mountedVehLicenseState - 1].gen &&
            g_engineVehLicenses[0].initialized && g_engineVehLicenses[1].initialized) {
            SyncVehSliceCachedFlags(g_engineVehLicenses[0], count);
            SyncVehSliceCachedFlags(g_engineVehLicenses[1], count);
            if (g_engineVehLicenses[0].cachedFlags == g_engineVehLicenses[1].cachedFlags) {
                g_mountedVehLicenseState = targetState;
                g_mountedVehLicenseGen   = src->gen;
                return;
            }
        }
    }

    if (WriteVehLicenseFlagsSeh(begin, count, src->cachedFlags.data())) {
        g_vehLicenseMountApplyCount++;
        g_mountedVehLicenseWorld = world;
        g_mountedVehLicenseBegin = begin;
        g_mountedVehLicenseCount = count;
        g_mountedVehLicenseState = targetState;
        g_mountedVehLicenseGen   = src->gen;
    } else {
        g_mountedVehLicenseState = 0;
    }
}

void ResetEngineVehLicenseSlices()
{
    for (int s = 0; s < 2; ++s) {
        g_engineVehLicenses[s].ownedKeys.clear();
        g_engineVehLicenses[s].cachedFlags.clear();
        g_engineVehLicenses[s].cachedFlagsGen = 0;
        g_engineVehLicenses[s].cachedFlagsWorldGen = 0;
        g_engineVehLicenses[s].gen++;
        g_engineVehLicenses[s].initialized = false;
        g_engineVehLicenses[s].loadedFromMeta = false;
    }
    g_vehTypeKeyCache.world = nullptr;
    g_vehTypeKeyCache.vecBegin = nullptr;
    g_vehTypeKeyCache.count = 0;
    g_vehTypeKeyCache.keys.clear();
    g_vehTypeKeyCache.worldGen++;
    g_mountedVehLicenseWorld = nullptr;
    g_mountedVehLicenseBegin = nullptr;
    g_mountedVehLicenseCount = 0;
    g_mountedVehLicenseState = 0;
    g_mountedVehLicenseGen   = 0;
}

void EnsureEngineVehLicensesInitialized(BYTE* world)
{
    if (!world) return;
    if (g_engineVehLicenses[0].initialized && g_engineVehLicenses[1].initialized &&
        !g_engineVehLicenses[0].loadedFromMeta && !g_engineVehLicenses[1].loadedFromMeta) {
        return;
    }
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureVehTypeKeyCache(world, &begin, &count)) return;
    bool needWriteToWorld = false;
    for (int s = 0; s < 2; ++s) {
        if (!g_engineVehLicenses[s].initialized) {
            CaptureWorldVehLicensesToSlice(world, &g_engineVehLicenses[s]);
        } else if (g_engineVehLicenses[s].loadedFromMeta) {
            g_engineVehLicenses[s].loadedFromMeta = false;
            needWriteToWorld = true;
        }
    }
    if (needWriteToWorld) {
        g_mountedVehLicenseState = 0;
        int activeState = ResolveActiveResearchWorldState();
        WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[activeState - 1]);
    }
}

void TwoStates_NotifyVehicleLicenseChanged(BYTE* world)
{
    if (!world || !IsTwoStatesResearchOrLicenseActive()) return;
    g_mountedVehLicenseState = 0;
    int owner = ResolveActiveResearchWorldState();
    EnsureEngineVehLicensesInitialized(world);
    CaptureWorldVehLicensesToSlice(world, &g_engineVehLicenses[owner - 1]);
    if (!g_inCommand) {
        int restoreSt = ResolveActiveResearchWorldState();
        WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[restoreSt - 1]);
    }
}

bool TwoStates_IsVehicleLicenseOwned(const BYTE* world, int stateOwner, int typeIdx)
{
    if (!world || typeIdx < 0) return false;
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureVehTypeKeyCache(world, &begin, &count) || typeIdx >= count) return false;
    if (IsTwoStatesResearchOrLicenseActive() && (stateOwner == 1 || stateOwner == 2) &&
        g_engineVehLicenses[stateOwner - 1].initialized) {
        SyncVehSliceCachedFlags(g_engineVehLicenses[stateOwner - 1], count);
        return g_engineVehLicenses[stateOwner - 1].cachedFlags[(size_t)typeIdx] != 0;
    }
    const BYTE* type = begin + (size_t)typeIdx * WORLD_VEHICLE_TYPE_STRIDE;
    uint8_t val = 0;
    SafeReadPod(type + VEH_TYPE_HAS_LICENCE_OFFSET, &val);
    return val != 0;
}

bool TwoStates_SetVehicleLicenseOwnedForTest(BYTE* world, int stateOwner, int typeIdx, bool owned)
{
    if (!world || typeIdx < 0 || (stateOwner != 1 && stateOwner != 2)) return false;
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureVehTypeKeyCache(world, &begin, &count) || typeIdx >= count) return false;
    EnsureEngineVehLicensesInitialized(world);
    const std::string& key = g_vehTypeKeyCache.keys[(size_t)typeIdx];
    if (owned) {
        AddLicenseKeyToSlice(&g_engineVehLicenses[stateOwner - 1], key);
    } else {
        RemoveLicenseKeyFromSlice(&g_engineVehLicenses[stateOwner - 1], key);
    }
    g_mountedVehLicenseState = 0;
    int activeState = g_inCommand ? g_cmdOwner : TwoStates_LocalState();
    if (activeState != 1 && activeState != 2) activeState = 1;
    WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[activeState - 1]);
    return true;
}

// ---------------------------------------------------------------------------
// R14 & R15: Fully Independent State Research Trees (Summary / Lockstep API)
// ---------------------------------------------------------------------------

void TwoStates_ResetResearch()
{
    memset(g_stateResearch, 0, sizeof(g_stateResearch));
    ResetEngineResearchSlices();
    ResetEngineVehLicenseSlices();
}

bool TwoStates_IsTechUnlocked(int stateOwner, int techId)
{
    if (stateOwner < 1 || stateOwner > 2 || techId < 0 || techId >= 32) return false;
    return (g_stateResearch[stateOwner - 1].unlockedMask & (1u << techId)) != 0;
}

void TwoStates_UnlockTech(int stateOwner, int techId)
{
    if (stateOwner < 1 || stateOwner > 2 || techId < 0 || techId >= 32) return;
    StateResearchTree& tree = g_stateResearch[stateOwner - 1];
    uint32_t bit = (1u << techId);
    if ((tree.unlockedMask & bit) == 0) {
        tree.unlockedMask |= bit;
        tree.researchedCount++;
    }
    tree.techProgress[techId] = 1.0f;
    if (tree.activeTechId == techId) {
        tree.activeTechId = 0;
        tree.activeTechProgress = 0.0f;
    }
    Logf("republicmp  RESEARCH: State %d unlocked Tech #%d (total: %d)",
         stateOwner, techId, tree.researchedCount);
}

void TwoStates_SetActiveTech(int stateOwner, int techId)
{
    if (stateOwner < 1 || stateOwner > 2) return;
    if (techId < 0 || techId >= 32) techId = 0;
    StateResearchTree& tree = g_stateResearch[stateOwner - 1];
    tree.activeTechId = techId;
    if (techId > 0) {
        tree.activeTechProgress = tree.techProgress[techId];
    } else {
        tree.activeTechProgress = 0.0f;
    }
    Logf("republicmp  RESEARCH: State %d active Tech set to #%d (progress: %.2f)",
         stateOwner, techId, tree.activeTechProgress);
}

int TwoStates_GetActiveTech(int stateOwner)
{
    if (stateOwner < 1 || stateOwner > 2) return 0;
    return g_stateResearch[stateOwner - 1].activeTechId;
}

void TwoStates_AddResearchProgress(int stateOwner, int techId, float delta)
{
    if (stateOwner < 1 || stateOwner > 2 || techId <= 0 || techId >= 32 || delta <= 0.0f) return;
    if (TwoStates_IsTechUnlocked(stateOwner, techId)) return;

    StateResearchTree& tree = g_stateResearch[stateOwner - 1];
    float p = tree.techProgress[techId] + delta;
    if (p >= 1.0f) {
        TwoStates_UnlockTech(stateOwner, techId);
    } else {
        tree.techProgress[techId] = p;
        if (tree.activeTechId == techId) {
            tree.activeTechProgress = p;
        }
    }
}

float TwoStates_GetTechProgress(int stateOwner, int techId)
{
    if (stateOwner < 1 || stateOwner > 2 || techId < 0 || techId >= 32) return 0.0f;
    return g_stateResearch[stateOwner - 1].techProgress[techId];
}

bool TwoStates_GetStateResearch(int stateOwner, StateResearchTree* outTree)
{
    if (stateOwner < 1 || stateOwner > 2 || !outTree) return false;
    *outTree = g_stateResearch[stateOwner - 1];
    return true;
}

bool TwoStates_SetStateResearch(int stateOwner, const StateResearchTree* inTree)
{
    if (stateOwner < 1 || stateOwner > 2 || !inTree) return false;
    g_stateResearch[stateOwner - 1] = *inTree;
    return true;
}

const StateResearchTree* TwoStates_GetLocalResearchTree()
{
    int local = TwoStates_LocalState();
    int idx = (local == 2) ? 1 : 0;
    return &g_stateResearch[idx];
}

bool TwoStates_ApplyResearchCommand(const LsResearchCommand& cmd)
{
    if (cmd.stateOwner != 1 && cmd.stateOwner != 2) return false;
    if (cmd.techId < 0 || cmd.techId >= 32) return false;

    if (cmd.progressDelta > 0.0f) {
        TwoStates_AddResearchProgress(cmd.stateOwner, cmd.techId, cmd.progressDelta);
    } else {
        TwoStates_SetActiveTech(cmd.stateOwner, cmd.techId);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Research Hook Installer
// ---------------------------------------------------------------------------

int TwoStates_InstallResearchHooks()
{
    if (!g_exeBase) return 0;
    int ok = 0;
    ok += InstallInlineHook(g_exeBase + RVA_BLD_UNIV_STEP, (void*)&H_BldUnivStep,
                            (void**)&o_bldUnivStep, kBldUnivStepExpect,
                            sizeof(kBldUnivStepExpect), "republicmp two_states bld_univ_step");
    ok += InstallInlineHook(g_exeBase + RVA_RESEARCH_DAILY_UPDATE, (void*)&H_ResearchDailyUpdate,
                            (void**)&o_researchDailyUpdate, kResearchDailyUpdateExpect,
                            sizeof(kResearchDailyUpdateExpect), "republicmp two_states research_daily_update");
    ok += InstallInlineHook(g_exeBase + RVA_UNIV_RESEARCH_PANEL, (void*)&H_UnivResearchPanel,
                            (void**)&o_univResearchPanel, kUnivResearchPanelExpect,
                            sizeof(kUnivResearchPanelExpect), "republicmp two_states univ_research_panel");
    return ok;
}
