// SPDX-License-Identifier: GPL-3.0-only
// Core lifecycle, mode/treasury state, step/command boundaries, ownership, and entity pruning.
#include "two_states_internal.h"

void TwoStates_Bind(const TsmHost* host)
{
    if (!host) return;
    TsmBind(host);
    TwoStates_BindEcon(host);
    TwoStates_BindSimHooks(host);
    TwoStates_BindResearch(host);
    TwoStates_BindOverlay(host);
    TwoStates_BindPersistence(host);
}

// ---------------------------------------------------------------------------
// Cross-Module Shared State (owned by two_states_core.cpp)
// ---------------------------------------------------------------------------

volatile int g_chosenMode = COOP_MODE_ONE_STATE;
volatile int g_sessionMode = COOP_MODE_ONE_STATE;
volatile bool g_inSession = false;
volatile bool g_isHost = true;
float g_borderHalfWidth = 250.0f; // [-250m .. +250m] = 500m shared border zone
bool g_fullStartingCapital = true;
volatile bool g_expandBorders = false; // default: standard 20x20 km map with borders (16x16 km playable zone)

double g_stateRub[2] = { 0.0, 0.0 };
double g_stateUsd[2] = { 0.0, 0.0 };
double g_netTradeRub[2] = { 0.0, 0.0 };
double g_netTradeUsd[2] = { 0.0, 0.0 };
double g_lobbyBaseWorldRub = 0.0;
double g_lobbyBaseWorldUsd = 0.0;
bool g_haveLobbyBaseMoney = false;
bool g_treasuryInitialized = false;
bool g_loadedFromCoopSave = false;

static INIT_ONCE g_ownerLockOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_ownerLock;
std::unordered_map<int, int> g_buildingOwners; // buildingId -> 1 (West) or 2 (East)
std::unordered_map<int, int> g_vehicleOwners;  // vehicleId  -> 1 (West) or 2 (East)
std::atomic<uint64_t> g_ownerMapGen{1};
std::atomic<uint64_t> g_vehOwnerMapGen{1};
static std::atomic<uint64_t> g_bldCacheRebuildCount{0};
bool g_stepUnlimitedMoney = false;

volatile bool g_inSimStep = false;
volatile DWORD g_simStepThread = 0;
BYTE* volatile g_simLoopCurrentBuilding = nullptr;
bool g_inCommand = false;
volatile DWORD g_cmdThread = 0;
int g_cmdOwner = 1;
bool g_beforeLoadPrepared = false;

static int g_cmdBuildingsBefore = 0;
static int g_cmdConstructionsBefore = 0;
static int g_cmdVehiclesBefore = 0;
static ULONGLONG g_lastRefuseLogMs = 0;

// ---------------------------------------------------------------------------
// Core Shared Helpers
// ---------------------------------------------------------------------------

void EnsureOwnerLock()
{
    TsmEnsureCriticalSection(&g_ownerLockOnce, &g_ownerLock);
}

void TwoStates_BumpOwnerMapGen()
{
    g_ownerMapGen.fetch_add(1, std::memory_order_acq_rel);
}

void TwoStates_BumpVehOwnerMapGen()
{
    g_vehOwnerMapGen.fetch_add(1, std::memory_order_acq_rel);
}

void TwoStates_ClearEntityOwners()
{
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_buildingOwners.clear();
    g_vehicleOwners.clear();
    TwoStates_BumpOwnerMapGen();
    TwoStates_BumpVehOwnerMapGen();
    LeaveCriticalSection(&g_ownerLock);
}

uint64_t TwoStates_GetOwnerMapGenForTest()
{
    return g_ownerMapGen.load(std::memory_order_acquire);
}

uint64_t TwoStates_GetBldPtrCacheRebuildCountForTest()
{
    return g_bldCacheRebuildCount.load(std::memory_order_relaxed);
}

bool TwoStates_IsInSimStepForTest()
{
    return g_inSimStep;
}

double* WorldRubles()  { return (double*)(g_exeBase + WORLD_RVA + WORLD_RUB_OFFSET); }
double* WorldDollars() { return (double*)(g_exeBase + WORLD_RVA + WORLD_USD_OFFSET); }

bool IsUnlimitedMoney()
{
    if (!g_exeBase) return false;
    bool result = false;
    DiagLog_BeginSafeSeh();
    __try {
        result = *(const int*)(g_exeBase + WORLD_RVA + WORLD_UNLIMITED_MONEY_OFFSET) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }
    DiagLog_EndSafeSeh();
    return result;
}

static int GetWorldBuildingCount()
{
    return (int)SafeCountStdVector(g_exeBase + VEC_BUILDINGS_BEGIN_RVA, sizeof(void*));
}

static int GetWorldConstructionCount()
{
    return (int)SafeCountStdVector(g_exeBase + VEC_CONSTRUCTIONS_BEGIN_RVA, sizeof(void*));
}

int GetWorldVehicleCount()
{
    return (int)SafeCountStdVector(g_exeBase + VEC_VEHICLES_BEGIN_RVA, sizeof(void*));
}

bool SwapWorldPtrVecForState(BYTE* world, size_t vecOff, int targetState,
                             ResolveEntityOwnerFn resolveFn, ptrdiff_t maxCount,
                             uintptr_t outSavedHdr[3], std::vector<BYTE*>& outFiltered)
{
    outFiltered.clear();
    if (!world || !outSavedHdr || !resolveFn) return false;
    memset(outSavedHdr, 0, 3 * sizeof(uintptr_t));
    if (!SafeMemcpyPod(outSavedHdr, world + vecOff, 3 * sizeof(uintptr_t))) return false;
    BYTE* const* b = (BYTE* const*)outSavedHdr[0];
    BYTE* const* e = (BYTE* const*)outSavedHdr[1];
    BYTE* const* c = (BYTE* const*)outSavedHdr[2];
    if (b && (e < b || c < e || (e - b) > maxCount)) return false;

    if (b && e > b) {
        outFiltered.reserve((size_t)(e - b));
        DiagLog_BeginSafeSeh();
        __try {
            for (BYTE* const* it = b; it < e; ++it) {
                BYTE* elem = *it;
                if (elem && resolveFn(elem) == targetState) {
                    outFiltered.push_back(elem);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outFiltered.clear();
        }
        DiagLog_EndSafeSeh();
    }
    BYTE** fBegin = outFiltered.empty() ? nullptr : outFiltered.data();
    uintptr_t newHdr[3] = {
        (uintptr_t)fBegin,
        (uintptr_t)(fBegin ? (fBegin + outFiltered.size()) : nullptr),
        (uintptr_t)(fBegin ? (fBegin + outFiltered.size()) : nullptr)
    };
    return SafeMemcpyPod(world + vecOff, newHdr, sizeof(newHdr));
}

int ResolveBuildingOwnerFast(const BYTE* b)
{
    if (!b || !IsPointerLikeX64((uintptr_t)b)) return 1;
    int owner = 1;
    DiagLog_BeginSafeSeh();
    __try {
        float x = *(const float*)(b + OBJ_POS_OFFSET);
        float z = *(const float*)(b + OBJ_POS_OFFSET + 8);
        owner = TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(x, z), x, z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 1;
    }
    DiagLog_EndSafeSeh();
    return owner;
}

static constexpr size_t kInlineBldCacheSlots = 512;    // 8 KB (L1-resident)
static constexpr size_t kMaxBldCacheSlots    = 524288; // handles up to 200k engine cap at <50% load

struct BldPtrOwnerCache {
    BldPtrOwnerSlot inlineSlots[kInlineBldCacheSlots] = {};
    std::vector<BldPtrOwnerSlot> overflowSlots;
    BldPtrOwnerSlot* slots = nullptr;
    size_t mask = 0;
    uint64_t cachedGen = 0;
    const BYTE* cachedWorld = nullptr;
    const BYTE* const* cachedBldBegin = nullptr;
    const BYTE* const* cachedBldEnd   = nullptr;
    const BYTE* const* cachedConBegin = nullptr;
    const BYTE* const* cachedConEnd   = nullptr;
};
static thread_local BldPtrOwnerCache t_bldOwnerCache;

static void ReadOneWorldPtrVecRange(const BYTE* world, size_t vecOff,
                                    const BYTE* const** outB, const BYTE* const** outE)
{
    const BYTE* const* b = *(const BYTE* const* const*)(world + vecOff);
    const BYTE* const* e = *(const BYTE* const* const*)(world + vecOff + 8);
    const BYTE* const* c = *(const BYTE* const* const*)(world + vecOff + 16);
    if (!b || e <= b || c < e || (e - b) > 200000) {
        *outB = nullptr;
        *outE = nullptr;
    } else {
        *outB = b;
        *outE = e;
    }
}

static bool ReadWorldStructureVecsSeh(const BYTE* world,
                                      const BYTE* const** outBldB, const BYTE* const** outBldE,
                                      const BYTE* const** outConB, const BYTE* const** outConE)
{
    *outBldB = *outBldE = nullptr;
    *outConB = *outConE = nullptr;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        ReadOneWorldPtrVecRange(world, WORLD_BUILDINGS_OFFSET, outBldB, outBldE);
        ReadOneWorldPtrVecRange(world, WORLD_CONSTRUCTIONS_OFFSET, outConB, outConE);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outBldB = *outBldE = nullptr;
        *outConB = *outConE = nullptr;
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void InsertStructureVecIntoCache(BldPtrOwnerSlot* slots, size_t mask,
                                        const BYTE* const* vecB, const BYTE* const* vecE)
{
    if (!vecB || !vecE || vecE <= vecB) return;
    const bool hasMap = !g_buildingOwners.empty();
    for (const BYTE* const* it = vecB; it < vecE; ++it) {
        const BYTE* bld = *it;
        if (!bld || !IsPointerLikeX64((uintptr_t)bld)) continue;
        float x = *(const float*)(bld + OBJ_POS_OFFSET);
        float z = *(const float*)(bld + OBJ_POS_OFFSET + 8);
        int owner = (x < 0.0f) ? 1 : 2;
        if (hasMap) {
            int posKey = TwoStates_BuildingPosKey(x, z);
            if (posKey > 0) {
                auto foundIt = g_buildingOwners.find(posKey);
                if (foundIt != g_buildingOwners.end() &&
                    (foundIt->second == 1 || foundIt->second == 2)) {
                    owner = foundIt->second;
                }
            }
        }
        size_t idx = HashBldPtr(bld) & mask;
        for (size_t probe = 0; probe <= mask; ++probe) {
            if (slots[idx].ptr == nullptr || slots[idx].ptr == bld) {
                slots[idx].ptr = bld;
                slots[idx].owner = static_cast<uint8_t>(owner);
                break;
            }
            idx = (idx + 1) & mask;
        }
    }
}

static bool PopulateBldCacheLockedSeh(BldPtrOwnerSlot* slots, size_t mask,
                                      const BYTE* const* bldB, const BYTE* const* bldE,
                                      const BYTE* const* conB, const BYTE* const* conE)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        InsertStructureVecIntoCache(slots, mask, bldB, bldE);
        InsertStructureVecIntoCache(slots, mask, conB, conE);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void EnsureBldPtrOwnerCache(const BYTE* world)
{
    if (!world || !IsPointerLikeX64((uintptr_t)world)) return;
    const BYTE* const* bldB = nullptr;
    const BYTE* const* bldE = nullptr;
    const BYTE* const* conB = nullptr;
    const BYTE* const* conE = nullptr;
    if (!ReadWorldStructureVecsSeh(world, &bldB, &bldE, &conB, &conE)) return;

    const uint64_t curGen = g_ownerMapGen.load(std::memory_order_acquire);
    if (Determinism_SessionRunning() &&
        t_bldOwnerCache.slots != nullptr &&
        t_bldOwnerCache.cachedGen == curGen &&
        t_bldOwnerCache.cachedWorld == world &&
        t_bldOwnerCache.cachedBldBegin == bldB &&
        t_bldOwnerCache.cachedBldEnd   == bldE &&
        t_bldOwnerCache.cachedConBegin == conB &&
        t_bldOwnerCache.cachedConEnd   == conE) {
        return;
    }

    size_t totalCount = 0;
    if (bldB && bldE > bldB) totalCount += static_cast<size_t>(bldE - bldB);
    if (conB && conE > conB) totalCount += static_cast<size_t>(conE - conB);
    size_t cap = kInlineBldCacheSlots;
    while (cap < kMaxBldCacheSlots && totalCount * 2 > cap) {
        cap <<= 1;
    }

    BldPtrOwnerSlot* slots = nullptr;
    if (cap == kInlineBldCacheSlots) {
        memset(t_bldOwnerCache.inlineSlots, 0, sizeof(t_bldOwnerCache.inlineSlots));
        slots = t_bldOwnerCache.inlineSlots;
    } else {
        t_bldOwnerCache.overflowSlots.assign(cap, BldPtrOwnerSlot{});
        slots = t_bldOwnerCache.overflowSlots.data();
    }

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    const uint64_t lockedGen = g_ownerMapGen.load(std::memory_order_acquire);
    const bool ok = PopulateBldCacheLockedSeh(slots, cap - 1, bldB, bldE, conB, conE);
    LeaveCriticalSection(&g_ownerLock);

    if (ok) {
        t_bldOwnerCache.slots = slots;
        t_bldOwnerCache.mask = cap - 1;
        t_bldOwnerCache.cachedGen = lockedGen;
        t_bldOwnerCache.cachedWorld = world;
        t_bldOwnerCache.cachedBldBegin = bldB;
        t_bldOwnerCache.cachedBldEnd   = bldE;
        t_bldOwnerCache.cachedConBegin = conB;
        t_bldOwnerCache.cachedConEnd   = conE;
        g_bldCacheRebuildCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        t_bldOwnerCache.slots = nullptr;
        t_bldOwnerCache.mask = 0;
        t_bldOwnerCache.cachedGen = 0;
    }
}

void TwoStates_GetActiveBldPtrCacheView(const BldPtrOwnerSlot** outSlots, size_t* outMask)
{
    const uint64_t gen = g_ownerMapGen.load(std::memory_order_acquire);
    if (t_bldOwnerCache.slots && t_bldOwnerCache.cachedGen == gen) {
        *outSlots = t_bldOwnerCache.slots;
        *outMask  = t_bldOwnerCache.mask;
    } else {
        *outSlots = nullptr;
        *outMask  = 0;
    }
}

int LookupBuildingOwnerCached(const BYTE* bld)
{
    const BldPtrOwnerSlot* slots = nullptr;
    size_t mask = 0;
    TwoStates_GetActiveBldPtrCacheView(&slots, &mask);
    return LookupBldPtrOwnerInSlots(slots, mask, bld);
}

int TwoStates_LookupBuildingOwnerCachedForTest(const BYTE* bld)
{
    if (g_exeBase) EnsureBldPtrOwnerCache(g_exeBase + WORLD_RVA);
    return LookupBuildingOwnerCached(bld);
}

int TwoStates_GetBuildingOwnerSlot(const BYTE* buildingPtr)
{
    return ResolveBuildingOwnerFast(buildingPtr);
}

int ResolveVehicleOwnerFast(const BYTE* v)
{
    if (!v) return 1;
    int owner = 1;
    DiagLog_BeginSafeSeh();
    __try {
        int id = *(const int*)(v + VEH_ID_OFFSET);
        owner = TwoStates_GetVehicleOwner(id, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 1;
    }
    DiagLog_EndSafeSeh();
    return owner;
}

struct VehIdOwnerSlot {
    int32_t vehId;
    uint8_t owner;
    uint8_t occupied;
    uint8_t pad[2];
};
struct VehIdOwnerCache {
    VehIdOwnerSlot inlineSlots[512] = {};
    std::vector<VehIdOwnerSlot> overflowSlots;
    VehIdOwnerSlot* slots = nullptr;
    size_t mask = 0;
    uint64_t cachedGen = 0;
    bool empty = true;
};
static thread_local VehIdOwnerCache t_vehOwnerCache;

static void EnsureVehIdOwnerCache()
{
    const uint64_t gen = g_vehOwnerMapGen.load(std::memory_order_acquire);
    if (t_vehOwnerCache.slots != nullptr && t_vehOwnerCache.cachedGen == gen) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    const uint64_t lockedGen = g_vehOwnerMapGen.load(std::memory_order_acquire);
    const size_t n = g_vehicleOwners.size();
    t_vehOwnerCache.empty = (n == 0);
    if (n == 0) {
        t_vehOwnerCache.slots = t_vehOwnerCache.inlineSlots;
        t_vehOwnerCache.mask = 0;
        t_vehOwnerCache.cachedGen = lockedGen;
        LeaveCriticalSection(&g_ownerLock);
        return;
    }
    size_t cap = 512;
    while (cap < 262144 && n * 2 > cap) cap <<= 1;
    VehIdOwnerSlot* slots = nullptr;
    if (cap == 512) {
        memset(t_vehOwnerCache.inlineSlots, 0, sizeof(t_vehOwnerCache.inlineSlots));
        slots = t_vehOwnerCache.inlineSlots;
    } else {
        t_vehOwnerCache.overflowSlots.assign(cap, VehIdOwnerSlot{});
        slots = t_vehOwnerCache.overflowSlots.data();
    }
    const size_t mask = cap - 1;
    for (const auto& kv : g_vehicleOwners) {
        if (kv.first < 0 || (kv.second != 1 && kv.second != 2)) continue;
        size_t idx = (size_t)((uint32_t)kv.first * 2654435761u) & mask;
        for (size_t probe = 0; probe <= mask; ++probe) {
            if (!slots[idx].occupied || slots[idx].vehId == kv.first) {
                slots[idx].vehId = kv.first;
                slots[idx].owner = static_cast<uint8_t>(kv.second);
                slots[idx].occupied = 1;
                break;
            }
            idx = (idx + 1) & mask;
        }
    }
    t_vehOwnerCache.slots = slots;
    t_vehOwnerCache.mask = mask;
    t_vehOwnerCache.cachedGen = lockedGen;
    LeaveCriticalSection(&g_ownerLock);
}

static int LookupVehIdOwnerInCache(int vehicleId)
{
    if (vehicleId < 0) return 0;
    EnsureVehIdOwnerCache();
    if (t_vehOwnerCache.empty || !t_vehOwnerCache.slots) return 0;
    const size_t mask = t_vehOwnerCache.mask;
    const VehIdOwnerSlot* slots = t_vehOwnerCache.slots;
    size_t idx = (size_t)((uint32_t)vehicleId * 2654435761u) & mask;
    for (size_t probe = 0; probe <= mask; ++probe) {
        if (!slots[idx].occupied) break;
        if (slots[idx].vehId == vehicleId) return static_cast<int>(slots[idx].owner);
        idx = (idx + 1) & mask;
    }
    return 0;
}

static int ResolveVehicleOwnerFallbackRaw(const BYTE* veh, const BldPtrOwnerSlot* slots, size_t mask)
{
    const BYTE* home = *(const BYTE* const*)(veh + VEH_HOME_OFFSET);
    if (!home) home = *(const BYTE* const*)(veh + VEH_DEPOT_OFFSET);
    if (home && IsPointerLikeX64((uintptr_t)home)) {
        return LookupBldPtrOwnerInSlots(slots, mask, home);
    }
    const float vx = *(const float*)(veh + VEH_POS_OFFSET);
    return (vx < 0.0f) ? 1 : 2;
}

static int ResolveUnhousedCitizenOwner(const BYTE* cit, const BldPtrOwnerSlot* slots, size_t mask,
                                       int (*fallbackResolveBld)(const BYTE*))
{
    bool isTourist = (*(const int32_t*)(cit + CIT_INFANT_FLAG_OFFSET) != 0);
    static const size_t kCitFallbackBldOffs[7] = { 0x58, 0x28, 0x40, 0x38, 0x78, 0x80, 0x88 };
    for (size_t i = 0; i < 7; ++i) {
        const BYTE* b = *(const BYTE* const*)(cit + kCitFallbackBldOffs[i]);
        if (!IsPointerLikeX64((uintptr_t)b)) continue;
        float bx = *(const float*)(b + OBJ_POS_OFFSET);
        float bz = *(const float*)(b + OBJ_POS_OFFSET + 8);
        if (isfinite(bx) && isfinite(bz) && fabsf(bx) < 50000.0f && fabsf(bz) < 50000.0f) {
            if (isTourist) {
                return slots ? ResolveCustomsTourismOwnerRawUnsafe(b, slots, mask)
                             : ResolveCustomsTourismOwner(b);
            }
            return fallbackResolveBld ? fallbackResolveBld(b) : LookupBldPtrOwnerInSlots(slots, mask, b);
        }
    }
    const BYTE* veh = *(const BYTE* const*)(cit + 0x60);
    if (IsPointerLikeX64((uintptr_t)veh)) {
        int vehId = *(const int*)(veh + VEH_ID_OFFSET);
        if (vehId > 0) {
            int found = LookupVehIdOwnerInCache(vehId);
            if (found == 1 || found == 2) return found;
            return ResolveVehicleOwnerFallbackRaw(veh, slots, mask);
        }
    }
    if (g_simLoopCurrentBuilding) {
        if (isTourist) {
            return slots ? ResolveCustomsTourismOwnerRawUnsafe(g_simLoopCurrentBuilding, slots, mask)
                         : ResolveCustomsTourismOwner(g_simLoopCurrentBuilding);
        }
        return fallbackResolveBld ? fallbackResolveBld(g_simLoopCurrentBuilding)
                                  : LookupBldPtrOwnerInSlots(slots, mask, g_simLoopCurrentBuilding);
    }
    float x = *(const float*)(cit + CIT_POS_OFFSET);
    float z = *(const float*)(cit + CIT_POS_OFFSET + 8);
    return (isfinite(x) && isfinite(z) && x > 0.0f) ? 2 : 1;
}

template <int (*ResolveBldFn)(const BYTE*)>
static int ResolveCitizenOwnerImpl(const BYTE* cit)
{
    if (!cit || !IsPointerLikeX64((uintptr_t)cit)) return 1;
    int owner = 1;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* home = *(const BYTE* const*)(cit + CIT_HOME_BUILDING_OFFSET);
        if (home && IsPointerLikeX64((uintptr_t)home)) {
            owner = ResolveBldFn(home);
        } else {
            const BYTE* parent = *(const BYTE* const*)(cit + CIT_PARENT_OFFSET);
            const BYTE* parentHome = (parent && IsPointerLikeX64((uintptr_t)parent))
                ? *(const BYTE* const*)(parent + CIT_HOME_BUILDING_OFFSET)
                : nullptr;
            owner = (parentHome && IsPointerLikeX64((uintptr_t)parentHome))
                ? ResolveBldFn(parentHome)
                : ResolveUnhousedCitizenOwner(cit, nullptr, 0, ResolveBldFn);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 1;
    }
    DiagLog_EndSafeSeh();
    return owner;
}

int ResolveCitizenOwnerRawUnsafe(const BYTE* cit, const BYTE* home, const BYTE* parent,
                                 const BldPtrOwnerSlot* slots, size_t mask)
{
    if (home && IsPointerLikeX64((uintptr_t)home)) {
        return LookupBldPtrOwnerInSlots(slots, mask, home);
    }
    const BYTE* parentHome = (parent && IsPointerLikeX64((uintptr_t)parent))
        ? *(const BYTE* const*)(parent + CIT_HOME_BUILDING_OFFSET)
        : nullptr;
    if (parentHome && IsPointerLikeX64((uintptr_t)parentHome)) {
        return LookupBldPtrOwnerInSlots(slots, mask, parentHome);
    }
    return ResolveUnhousedCitizenOwner(cit, slots, mask, nullptr);
}

int ResolveCitizenOwnerFast(const BYTE* cit)
{
    return ResolveCitizenOwnerImpl<&ResolveBuildingOwnerFast>(cit);
}

int ResolveCitizenOwnerCached(const BYTE* cit)
{
    return ResolveCitizenOwnerImpl<&LookupBuildingOwnerCached>(cit);
}

int TwoStates_ResolveCitizenOwnerCachedForTest(const BYTE* cit)
{
    if (g_exeBase) EnsureBldPtrOwnerCache(g_exeBase + WORLD_RVA);
    return ResolveCitizenOwnerCached(cit);
}

int TwoStates_ResolveCitizenOwnerForTest(const BYTE* citizen)
{
    return ResolveCitizenOwnerFast(citizen);
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

bool TwoStates_Install()
{
    EnsureOwnerLock();
    TwoStates_LoadSettings();
    TwoStates_MountSaveBank(false);

    int ok = TwoStates_InstallTerrainIatHooks();
    int econOk = 0;
    econOk += TwoStates_InstallEconHooks();
    econOk += TwoStates_InstallSimHooks();
    econOk += TwoStates_InstallResearchHooks();
    econOk += TwoStates_InstallOverlayHooks();
    int saveOk = TwoStates_InstallPersistenceHooks();

    Logf("republicmp  TWO_STATES: terrain boundary hooks (%d/4), economy/loan/stats/research/minimap hooks (%d/67), master_save=%d, heightFn=%p",
         ok, econOk, saveOk, (void*)g_collisionTerrainHeight);
    return ok == 4;
}

// ---------------------------------------------------------------------------
// Configuration & Lifecycle
// ---------------------------------------------------------------------------

void TwoStates_SetMode(int mode)
{
    g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (!g_inSession) g_sessionMode = g_chosenMode;
}

int TwoStates_GetMode()
{
    return g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
}

bool TwoStates_IsActive()
{
    return g_inSession && g_sessionMode == COOP_MODE_TWO_STATES;
}

void TwoStates_SetBorderHalfWidth(float meters)
{
    if (meters < 50.0f) meters = 50.0f;
    if (meters > 2000.0f) meters = 2000.0f;
    g_borderHalfWidth = meters;
    TwoStates_BumpOwnerMapGen();
}

float TwoStates_GetBorderHalfWidth()
{
    return g_borderHalfWidth;
}

void TwoStates_SetFullStartingCapital(bool full)
{
    g_fullStartingCapital = full;
}

bool TwoStates_GetFullStartingCapital()
{
    return g_fullStartingCapital;
}

int TwoStates_LocalState()
{
    if (g_inSession) return g_isHost ? 1 : 2;
    SessionState st = SaveSync_GetState();
    if (st == SESSION_CLIENT_JOINING || st == SESSION_CLIENT_IN_GAME) return 2;
    return 1;
}

const char* TwoStates_StateName(int state)
{
    if (state == 1) return Tr("Западная ССР (Хост)", "West SSR (Host)");
    if (state == 2) return Tr("Восточная ССР (Игрок 2)", "East SSR (Player 2)");
    return Tr("Общая пограничная зона", "Shared Border Zone");
}

void TwoStates_PrepareHostSave(bool isResync, double* outRub, double* outUsd)
{
    TwoStates_RestoreUiMoneySwap();
    DiagLog_BeginSafeSeh();
    __try {
        double worldRub = *WorldRubles();
        double worldUsd = *WorldDollars();
        if (!isResync && !g_loadedFromCoopSave && !g_haveLobbyBaseMoney) {
            g_lobbyBaseWorldRub = worldRub;
            g_lobbyBaseWorldUsd = worldUsd;
            g_haveLobbyBaseMoney = true;
        }
        if (TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = 100000000.0;
                g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = 100000000.0;
                g_stateUsd[1] = 100000000.0;
                *WorldRubles()  = 100000000.0;
                *WorldDollars() = 100000000.0;
                g_treasuryInitialized = true;
            } else if (!isResync && !g_treasuryInitialized && !g_loadedFromCoopSave) {
                double baseRub = g_haveLobbyBaseMoney ? g_lobbyBaseWorldRub : worldRub;
                double baseUsd = g_haveLobbyBaseMoney ? g_lobbyBaseWorldUsd : worldUsd;
                if (g_fullStartingCapital) {
                    g_stateRub[0] = baseRub;
                    g_stateRub[1] = baseRub;
                    g_stateUsd[0] = baseUsd;
                    g_stateUsd[1] = baseUsd;
                    *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                    *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
                } else {
                    g_stateRub[0] = baseRub * 0.5;
                    g_stateRub[1] = baseRub - g_stateRub[0];
                    g_stateUsd[0] = baseUsd * 0.5;
                    g_stateUsd[1] = baseUsd - g_stateUsd[0];
                    *WorldRubles()  = baseRub;
                    *WorldDollars() = baseUsd;
                }
                g_treasuryInitialized = true;
            } else {
                // Already initialized (either from republicmp_coop.ini or while hosting/re-syncing):
                // attribute any pre-session singleplayer delta on Host to State 1.
                double diffRub = worldRub - (g_stateRub[0] + g_stateRub[1]);
                double diffUsd = worldUsd - (g_stateUsd[0] + g_stateUsd[1]);
                if (!Determinism_IsActive() && !g_loadedFromCoopSave && !g_inSession) {
                    g_stateRub[0] += diffRub;
                    g_stateUsd[0] += diffUsd;
                }
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            }
        } else {
            double baseRub = (!isResync && !g_loadedFromCoopSave && g_haveLobbyBaseMoney) ? g_lobbyBaseWorldRub : worldRub;
            double baseUsd = (!isResync && !g_loadedFromCoopSave && g_haveLobbyBaseMoney) ? g_lobbyBaseWorldUsd : worldUsd;
            g_stateRub[0] = baseRub;
            g_stateRub[1] = 0.0;
            g_stateUsd[0] = baseUsd;
            g_stateUsd[1] = 0.0;
            if (!IsUnlimitedMoney() && !isResync && !g_loadedFromCoopSave && g_haveLobbyBaseMoney) {
                *WorldRubles()  = baseRub;
                *WorldDollars() = baseUsd;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    if (outRub) { outRub[0] = g_stateRub[0]; outRub[1] = g_stateRub[1]; }
    if (outUsd) { outUsd[0] = g_stateUsd[0]; outUsd[1] = g_stateUsd[1]; }
}

bool TwoStates_IsLoadedFromCoopSave()
{
    return g_loadedFromCoopSave;
}

void TwoStates_CaptureLobbyBaseMoney()
{
    TwoStates_RestoreUiMoneySwap();
    if (g_loadedFromCoopSave || g_inSession || SaveSync_IsWaitingForReconnect()) return;
    DiagLog_BeginSafeSeh();
    __try {
        g_lobbyBaseWorldRub = *WorldRubles();
        g_lobbyBaseWorldUsd = *WorldDollars();
        g_haveLobbyBaseMoney = true;
        g_treasuryInitialized = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_haveLobbyBaseMoney = false;
    }
    DiagLog_EndSafeSeh();
}

void TwoStates_ReapplyWaitingLobbySettings()
{
    TwoStates_RestoreUiMoneySwap();
    g_sessionMode = g_chosenMode;
    if (g_loadedFromCoopSave || SaveSync_IsWaitingForReconnect()) return;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        if (!g_haveLobbyBaseMoney) {
            g_lobbyBaseWorldRub = *WorldRubles();
            g_lobbyBaseWorldUsd = *WorldDollars();
            g_haveLobbyBaseMoney = true;
        }
        g_treasuryInitialized = false;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    if (ok) {
        TwoStates_PrepareHostSave(false, nullptr, nullptr);
    }
}

void TwoStates_OnSessionJoined(bool isHost, int mode, float borderHalfWidth, bool expandBorders,
                               const double stateRub[2], const double stateUsd[2])
{
    EnsureOwnerLock();
    TwoStates_RestoreUiMoneySwap();

    g_isHost = isHost;
    g_sessionMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (borderHalfWidth >= 50.0f && borderHalfWidth <= 2000.0f)
        TwoStates_SetBorderHalfWidth(borderHalfWidth);
    else
        TwoStates_BumpOwnerMapGen();
    TwoStates_InvalidateMountCaches();
    g_expandBorders = expandBorders;
    g_stateRub[0] = stateRub[0];
    g_stateRub[1] = stateRub[1];
    g_stateUsd[0] = stateUsd[0];
    g_stateUsd[1] = stateUsd[1];
    g_treasuryInitialized = (g_sessionMode == COOP_MODE_TWO_STATES);
    g_inSession = true;
    if (!isHost) {
        g_loadedFromCoopSave = false;
    }

    if (g_sessionMode == COOP_MODE_TWO_STATES) {
        DiagLog_BeginSafeSeh();
        __try {
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
                *WorldRubles()  = 100000000.0;
                *WorldDollars() = 100000000.0;
            } else {
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            }
            if (g_exeBase && !g_beforeLoadPrepared && !Determinism_WorldLoading() &&
                (!TwoStates_AreEconHooksInstalled() || SaveSync_IsWorldLoaded())) {
                EnsureStateEconInitialized(g_exeBase + WORLD_RVA);
                EnsureEngineResearchInitialized(g_exeBase + WORLD_RVA);
                EnsureEngineVehLicensesInitialized(g_exeBase + WORLD_RVA);
            }
            Commands_RecordMoney();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
        SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Режим активен! Вы: %s | Граница X=0 (общая зона ±%.0f м)",
                      "[TWO STATES] Mode active! You: %s | Border X=0 (shared zone ±%.0f m)"),
                   TwoStates_StateName(TwoStates_LocalState()), g_borderHalfWidth);
        SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Казна Запада: %.0f ₽ / %.0f $ | Казна Востока: %.0f ₽ / %.0f $",
                      "[TWO STATES] West Treasury: %.0f RUB / %.0f USD | East Treasury: %.0f RUB / %.0f USD"),
                   g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
        char toast[220];
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("Режим «Два государства»: Вы — %s (F7: 3D-граница, F6: к другу, Alt+ЛКМ: сигнал)",
                       "Two States Mode: You are %s (F7: 3D border, F6: jump to partner, Alt+LMB: ping)"),
                    TwoStates_StateName(TwoStates_LocalState()));
        TwoStates_AddToast(toast, 1);
    } else {
        SaveStatus(Tr("[ОДНО ГОСУДАРСТВО] Совместное управление единой республикой (общая казна)",
                      "[ONE STATE] Co-op management of a single republic (shared treasury)"));
        TwoStates_AddToast(Tr("Режим «Одно государство»: общая казна и территория (F6: к другу, Alt+ЛКМ: сигнал)",
                              "One State Mode: shared treasury & territory (F6: jump to partner, Alt+LMB: ping)"), 1);
    }
}

void TwoStates_OnWorldLoaded()
{
    TwoStates_RestoreUiMoneySwap();
    TwoStates_BumpOwnerMapGen();
    TwoStates_InvalidateMountCaches();
    if (!TwoStates_IsActive()) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (IsUnlimitedMoney()) {
            g_stateRub[0] = g_stateRub[1] = 100000000.0;
            g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
            *WorldRubles()  = 100000000.0;
            *WorldDollars() = 100000000.0;
        } else {
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        }
        if (g_exeBase) {
            EnsureStateEconInitialized(g_exeBase + WORLD_RVA);
            EnsureEngineResearchInitialized(g_exeBase + WORLD_RVA);
            EnsureEngineVehLicensesInitialized(g_exeBase + WORLD_RVA);
        }
        Commands_RecordMoney();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void TwoStates_OnSessionEnded()
{
    TwoStates_RestoreUiMoneySwap();
    ResetStateHudPopCache();
    TwoStates_InvalidateUiCitizenCache();
    TwoStates_InvalidateUiHistBackupCache();
    TwoStates_InvalidateMountCaches();
    g_pendingCamRestore.active = false;
    MemoryBarrier();
    g_pendingCamRestore = {};
    g_inSession = false;
    g_inSimStep = false;
    g_stepUnlimitedMoney = false;
    TwoStates_FlushEconHookPerf();
    if (!SaveSync_IsWorldLoaded()) {
        g_treasuryInitialized = false;
        g_haveLobbyBaseMoney = false;
        g_stateRub[0] = g_stateRub[1] = 0.0;
        g_stateUsd[0] = g_stateUsd[1] = 0.0;
        g_loadedFromCoopSave = false;
        ResetSavedCameraState();
        TwoStates_ClearEntityOwners();
    } else if (!g_loadedFromCoopSave && g_haveLobbyBaseMoney && !SaveSync_HadPartnerInSession()) {
        // Server was stopped in the Waiting Lobby before any partner ever joined:
        // restore the exact pre-lobby Singleplayer world balance so 100%-each never doubles SP money.
        if (!IsUnlimitedMoney()) {
            SafeWriteWorldMoney(g_lobbyBaseWorldRub, g_lobbyBaseWorldUsd);
        }
        g_treasuryInitialized = false;
        g_haveLobbyBaseMoney = false;
        g_stateRub[0] = g_stateRub[1] = 0.0;
        g_stateUsd[0] = g_stateUsd[1] = 0.0;
        g_loadedFromCoopSave = false;
        ResetSavedCameraState();
        TwoStates_ClearEntityOwners();
    } else if (g_treasuryInitialized && TwoStates_GetMode() == COOP_MODE_TWO_STATES && !IsUnlimitedMoney()) {
        SafeWriteWorldMoney(g_stateRub[0] + g_stateRub[1], g_stateUsd[0] + g_stateUsd[1]);
    }

    g_localPingActive = false;
    g_hasResyncCam = false;

    // Preserve g_toasts across session teardown so join-failure, disconnect, and
    // copy notifications remain visible after returning to SESSION_IDLE.

    g_netTradeRub[0] = g_netTradeRub[1] = 0.0;
    g_netTradeUsd[0] = g_netTradeUsd[1] = 0.0;

    TwoStates_MountSaveBank(false);
    NativeMenu_ClearHostOnNextLoad();
    TwoStates_ResetResearch();
    ModSync_EndSession();
}

// ---------------------------------------------------------------------------
// Treasury & Deterministic Command / Step Hooks
// ---------------------------------------------------------------------------

static double GetStateTreasuryBalance(int state, const double balances[2], double* (*worldCurrencyPtr)())
{
    if (!TwoStates_IsActive() && !(g_treasuryInitialized && TwoStates_GetMode() == COOP_MODE_TWO_STATES)) {
        double v = 0.0;
        SafeReadPod(worldCurrencyPtr(), &v);
        return v;
    }
    if (state == 1) return balances[0];
    if (state == 2) return balances[1];
    return balances[0] + balances[1];
}

double TwoStates_GetRubles(int state)
{
    return GetStateTreasuryBalance(state, g_stateRub, WorldRubles);
}

double TwoStates_GetDollars(int state)
{
    return GetStateTreasuryBalance(state, g_stateUsd, WorldDollars);
}

double TwoStates_LocalRubles()
{
    return TwoStates_GetRubles(TwoStates_LocalState());
}

double TwoStates_LocalDollars()
{
    return TwoStates_GetDollars(TwoStates_LocalState());
}

void TwoStates_SyncBalancesFromHost(double r1, double r2, double u1, double u2)
{
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
    if (!isfinite(r1) || !isfinite(r2) || !isfinite(u1) || !isfinite(u2)) return;
    g_stateRub[0] = r1;
    g_stateRub[1] = r2;
    g_stateUsd[0] = u1;
    g_stateUsd[1] = u2;
    DiagLog_BeginSafeSeh();
    __try {
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void TwoStates_BeginCommand(int ownerState)
{
    TwoStates_RestoreUiMoneySwap();
    g_cmdOwner = (ownerState == 2) ? 2 : 1;
    g_cmdThread = GetCurrentThreadId();
    g_inCommand = true;
    g_cmdBuildingsBefore     = GetWorldBuildingCount();
    g_cmdConstructionsBefore = GetWorldConstructionCount();
    g_cmdVehiclesBefore      = GetWorldVehicleCount();
    g_cmdResSnapshot.active  = false;
    if (TwoStates_IsActive() && g_exeBase) {
        BYTE* world = g_exeBase + WORLD_RVA;
        TwoStates_InvalidateMountCaches();
        EnsureEngineResearchInitialized(world);
        WriteSliceToWorldResearch(world, &g_engineResearch[g_cmdOwner - 1]);
        EnsureEngineVehLicensesInitialized(world);
        WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[g_cmdOwner - 1]);
    }
    if (!TwoStates_IsActive()) {
        return;
    }
    bool unlim = IsUnlimitedMoney();
    DiagLog_BeginSafeSeh();
    __try {
        if (g_exeBase) {
            BYTE* world = g_exeBase + WORLD_RVA;
            EnsureStateEconInitialized(world);
            SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_cmdBeforeScalars);
            SafeReadTrackerSlice(world, false, &g_cmdBeforeTrackerCur);
            BeginConstructResSnapshot(world, g_cmdOwner, &g_cmdResSnapshot);
        }
        if (!unlim) {
            *WorldRubles()  = g_stateRub[g_cmdOwner - 1];
            *WorldDollars() = g_stateUsd[g_cmdOwner - 1];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

int TwoStates_GetCommandOwner()
{
    return g_inCommand ? g_cmdOwner : 0;
}

static void RegisterNewStructuresInVec(size_t beginRva, int countBefore, int countAfter, int owner)
{
    if (!g_exeBase || countAfter <= countBefore) return;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** arr = *(BYTE***)(g_exeBase + beginRva);
        for (int i = countBefore; arr && i < countAfter; i++) {
            if (arr[i]) {
                float x = *(const float*)(arr[i] + OBJ_POS_OFFSET);
                float z = *(const float*)(arr[i] + OBJ_POS_OFFSET + 8);
                TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(x, z), owner);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void TwoStates_EndCommand(int ownerState)
{
    int owner = (ownerState == 2) ? 2 : 1;
    // Register any new buildings and road/network construction sites placed during this command to `owner`
    // using their deterministic position key (since *(int*)b == 0 at creation time and is overwritten at 0x13CD0E).
    int bldAfter = GetWorldBuildingCount();
    int conAfter = GetWorldConstructionCount();
    RegisterNewStructuresInVec(VEC_BUILDINGS_BEGIN_RVA,     g_cmdBuildingsBefore,     bldAfter, owner);
    RegisterNewStructuresInVec(VEC_CONSTRUCTIONS_BEGIN_RVA, g_cmdConstructionsBefore, conAfter, owner);
    if (bldAfter != g_cmdBuildingsBefore || conAfter != g_cmdConstructionsBefore) {
        TwoStates_BumpOwnerMapGen();
    }

    // Register any new vehicles bought/spawned during this command to `owner`.
    int vehAfter = GetWorldVehicleCount();
    if (vehAfter > g_cmdVehiclesBefore) {
        DiagLog_BeginSafeSeh();
        __try {
            BYTE** v = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
            for (int i = g_cmdVehiclesBefore; v && i < vehAfter; i++) {
                if (v[i]) TwoStates_RegisterVehicleOwner(*(int*)(v[i] + VEH_ID_OFFSET), owner);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
    }
    if (g_inCommand && TwoStates_IsActive() && g_exeBase) {
        BYTE* world = g_exeBase + WORLD_RVA;
        TwoStates_InvalidateMountCaches();
        CaptureWorldResearchToSlice(world, &g_engineResearch[owner - 1], owner);
        CaptureWorldVehLicensesToSlice(world, &g_engineVehLicenses[owner - 1]);
        int localSt = TwoStates_LocalState();
        if (localSt != 1 && localSt != 2) localSt = 1;
        WriteSliceToWorldResearch(world, &g_engineResearch[localSt - 1]);
        WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[localSt - 1]);
    }
    if (!g_inCommand || !TwoStates_IsActive()) {
        if (g_cmdResSnapshot.active && g_exeBase) {
            EndConstructResSnapshot(g_exeBase + WORLD_RVA, owner, g_cmdResSnapshot);
            g_cmdResSnapshot.active = false;
        }
        g_inCommand = false;
        g_cmdThread = 0;
        return;
    }
    g_inCommand = false;
    g_cmdThread = 0;
    bool unlim = IsUnlimitedMoney();
    DiagLog_BeginSafeSeh();
    __try {
        if (g_exeBase) {
            BYTE* world = g_exeBase + WORLD_RVA;
            EndConstructResSnapshot(world, owner, g_cmdResSnapshot);
            g_cmdResSnapshot.active = false;
            StateEconScalars afterScalars = {};
            SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &afterScalars);
            AddStatScalarsDelta(&g_stateCurEcon[owner - 1].scalars, g_cmdBeforeScalars, afterScalars, 1.0f);
            g_lastWorldScalars = afterScalars;
            StateTrackerSlice afterTracker = {};
            SafeReadTrackerSlice(world, false, &afterTracker);
            AddTrackerSliceDelta(&g_stateTrackerCur[owner - 1], g_cmdBeforeTrackerCur, afterTracker, 1.0f);
            g_lastWorldTrackerCur = afterTracker;
        }
        if (!unlim) {
            double newRub = SanitizeMoneyStep(g_stateRub[owner - 1], *WorldRubles());
            double newUsd = SanitizeMoneyStep(g_stateUsd[owner - 1], *WorldDollars());
            g_netTradeRub[owner - 1] += (newRub - g_stateRub[owner - 1]);
            g_netTradeUsd[owner - 1] += (newUsd - g_stateUsd[owner - 1]);
            g_stateRub[owner - 1] = newRub;
            g_stateUsd[owner - 1] = newUsd;
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TwoStates_SetCurStatLedgerSuppressed(false);
        g_cmdResSnapshot.active = false;
    }
    DiagLog_EndSafeSeh();
}

bool TwoStates_ApplyTransferCommand(const LsStateTransfer& t)
{
    if (!TwoStates_IsActive()) return false;
    if ((t.fromState != 1 && t.fromState != 2) || (t.toState != 1 && t.toState != 2) || t.fromState == t.toState)
        return false;
    if (!isfinite(t.rubles) || !isfinite(t.dollars) || t.rubles < 0.0 || t.dollars < 0.0)
        return false;
    if (t.rubles == 0.0 && t.dollars == 0.0)
        return false;

    int src = t.fromState - 1;
    int dst = t.toState - 1;
    double sendRub = t.rubles <= g_stateRub[src] ? t.rubles : g_stateRub[src];
    double sendUsd = t.dollars <= g_stateUsd[src] ? t.dollars : g_stateUsd[src];
    if (sendRub <= 0.0 && sendUsd <= 0.0) return false;

    g_stateRub[src] -= sendRub;
    g_stateRub[dst] += sendRub;
    g_stateUsd[src] -= sendUsd;
    g_stateUsd[dst] += sendUsd;

    if (g_inCommand) {
        DiagLog_BeginSafeSeh();
        __try {
            *WorldRubles()  = g_stateRub[g_cmdOwner - 1];
            *WorldDollars() = g_stateUsd[g_cmdOwner - 1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
    }
    SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Перевод из %s в %s: %.0f ₽, %.0f $",
                  "[TWO STATES] Transfer from %s to %s: %.0f RUB, %.0f USD"),
               TwoStates_StateName(t.fromState), TwoStates_StateName(t.toState), sendRub, sendUsd);
    char toast[200];
    _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                Tr("★ Перевод из %s в %s: %.0f ₽ / %.0f $",
                   "★ Transfer from %s to %s: %.0f RUB / %.0f USD"),
                TwoStates_StateName(t.fromState), TwoStates_StateName(t.toState), sendRub, sendUsd);
    TwoStates_AddToast(toast, 1);
    return true;
}

void TwoStates_RequestTransfer(double rubles, double dollars)
{
    if (!TwoStates_IsActive()) return;
    if (rubles < 0.0) rubles = 0.0;
    if (dollars < 0.0) dollars = 0.0;
    if (rubles == 0.0 && dollars == 0.0) return;

    int me = TwoStates_LocalState();
    int other = (me == 1) ? 2 : 1;
    LsStateTransfer t = { me, other, rubles, dollars };
    if (Commands_Capturing()) {
        Commands_Submit(26 /* LS_CMD_STATE_TRANSFER */, &t, sizeof(t));
    } else {
        TwoStates_ApplyTransferCommand(t);
    }
}

static void PrepareStructureVectorBeforeStep(size_t beginRva, size_t endRva, bool unlim,
                                             const BldPtrOwnerSlot* slots, size_t mask)
{
    if (!g_exeBase) return;
    BYTE** it = *(BYTE***)(g_exeBase + beginRva);
    BYTE** end = *(BYTE***)(g_exeBase + endRva);
    if (!it || end <= it || (end - it) > 200000) return;
    for (; it < end; ++it) {
        BYTE* b = *it;
        if (!b) continue;
        int owner = LookupBldPtrOwnerInSlots(slots, mask, b);
        int idx = (owner == 2) ? 1 : 0;
        g_stepBldCount[idx]++;
        if (beginRva == VEC_BUILDINGS_BEGIN_RVA) {
            float cap = *(const float*)(b + BLD_EFFECTIVE_CAP_FLOAT_OFFSET);
            if (!isfinite(cap) || cap < 0.0f || cap > 1.0e6f) {
                *(float*)(b + BLD_EFFECTIVE_CAP_FLOAT_OFFSET) = 0.0f;
            }
        }
        if (!unlim) {
            float prog = *(float*)(b + BLD_PROGRESS_OFFSET);
            if (prog < 1.0f) {
                if (b[BLD_PAY_RUB_OFFSET] && g_stateRub[idx] <= 0.0) b[BLD_PAY_RUB_OFFSET] = 0;
                if (b[BLD_PAY_USD_OFFSET] && g_stateUsd[idx] <= 0.0) b[BLD_PAY_USD_OFFSET] = 0;
            }
        }
    }
}

void TwoStates_BeforeSubStep(BYTE* world, bool isEarly)
{
    TwoStates_RestoreUiMoneySwap();
    g_simLoopCurrentBuilding = nullptr;
    if (!TwoStates_IsActive() || !world) return;
    bool unlim = IsUnlimitedMoney();
    g_stepUnlimitedMoney = unlim;
    if (unlim) {
        g_stateRub[0] = g_stateRub[1] = 100000000.0;
        g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
    }
    TwoStates_BeginSubStepEconRead(world);
    EnsureBldPtrOwnerCache(world);
    const BldPtrOwnerSlot* bldSlots = nullptr;
    size_t bldMask = 0;
    TwoStates_GetActiveBldPtrCacheView(&bldSlots, &bldMask);
    g_stepBldCount[0] = g_stepBldCount[1] = 0;

    DiagLog_BeginSafeSeh();
    __try {
        if (!unlim) {
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            g_lastWorldRub = *WorldRubles();
            g_lastWorldUsd = *WorldDollars();
            g_unattributedRub = 0.0;
            g_unattributedUsd = 0.0;
        }
        TwoStates_ResetEconBeforeCursor();
        const bool wasInSimStep = g_inSimStep;
        g_simStepThread = GetCurrentThreadId();

        if (IsTwoStatesResearchOrLicenseActive()) {
            const bool resReadyBefore = g_engineResearch[0].initialized && g_engineResearch[1].initialized &&
                                        !g_engineResearch[0].loadedFromMeta && !g_engineResearch[1].loadedFromMeta &&
                                        !g_engineResearch[0].hasUnresolvedBldPtr && !g_engineResearch[1].hasUnresolvedBldPtr;
            const bool licReadyBefore = g_engineVehLicenses[0].initialized && g_engineVehLicenses[1].initialized &&
                                        !g_engineVehLicenses[0].loadedFromMeta && !g_engineVehLicenses[1].loadedFromMeta;
            EnsureEngineResearchInitialized(world);
            EnsureEngineVehLicensesInitialized(world);
            if (isEarly && !wasInSimStep && !Determinism_SessionRunning()) {
                int localSt = TwoStates_LocalState();
                if (localSt != 1 && localSt != 2) localSt = 1;
                if (resReadyBefore) {
                    CaptureWorldResearchToSlice(world, &g_engineResearch[localSt - 1], localSt);
                }
                if (licReadyBefore) {
                    CaptureWorldVehLicensesToSlice(world, &g_engineVehLicenses[localSt - 1]);
                }
            }
            g_inSimStep = true;
            // Both Host and Client mount the identical canonical simulation baseline (State 1)
            // without any OR-merging or union mixing between the two republics' trees!
            WriteSliceToWorldResearch(world, &g_engineResearch[0]);
            WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[0]);
        } else {
            g_inSimStep = true;
        }

        PrepareStructureVectorBeforeStep(VEC_BUILDINGS_BEGIN_RVA, VEC_BUILDINGS_END_RVA, unlim, bldSlots, bldMask);
        PrepareStructureVectorBeforeStep(VEC_CONSTRUCTIONS_BEGIN_RVA, VEC_CONSTRUCTIONS_END_RVA, unlim, bldSlots, bldMask);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void TwoStates_BeforeStep(BYTE* world)
{
    TwoStates_BeforeSubStep(world, true);
}

double TwoStates_ComputeStepWeightState1(int countState1, int countState2)
{
    if (countState1 < 0) countState1 = 0;
    if (countState2 < 0) countState2 = 0;
    int total = countState1 + countState2;
    if (total <= 0) return 0.5;
    return (double)countState1 / (double)total;
}

void TwoStates_AfterSubStep(BYTE* world, bool isEarly)
{
    g_simLoopCurrentBuilding = nullptr;
    if (!TwoStates_IsActive() || !world) {
        g_inSimStep = false;
        TwoStates_FlushEconHookPerf();
        return;
    }
    g_inSimStep = false;
    bool unlim = g_stepUnlimitedMoney;
    __try {
        TwoStates_ResetEconBeforeCursor();
        float wCit0 = -1.0f;
        if (!isEarly) {
            if (Determinism_IsActive() && Determinism_SessionRunning()) {
                TwoStates_ComputeStepDemographicsAndPrimeUiCache(world, &g_stateCurEcon[0].scalars, &g_stateCurEcon[1].scalars, &wCit0);
            } else {
                ComputeBothStatesCitizenDemographics(world, &g_stateCurEcon[0].scalars, &g_stateCurEcon[1].scalars);
            }
        }
        FlushUnattributedScalarsToStates(world, false, wCit0);

        if (!unlim) {
            double nowRub = *WorldRubles();
            double nowUsd = *WorldDollars();
            double dRub = g_unattributedRub + (nowRub - g_lastWorldRub);
            double dUsd = g_unattributedUsd + (nowUsd - g_lastWorldUsd);
            g_unattributedRub = 0.0;
            g_unattributedUsd = 0.0;

            double w0 = TwoStates_ComputeStepWeightState1(g_stepBldCount[0], g_stepBldCount[1]);

            if (fabs(dRub) > 1e-6) {
                double d0 = dRub * w0;
                g_stateRub[0] += d0;
                g_stateRub[1] += (dRub - d0);
                g_netTradeRub[0] += d0;
                g_netTradeRub[1] += (dRub - d0);
            }
            if (fabs(dUsd) > 1e-6) {
                double d0 = dUsd * w0;
                g_stateUsd[0] += d0;
                g_stateUsd[1] += (dUsd - d0);
                g_netTradeUsd[0] += d0;
                g_netTradeUsd[1] += (dUsd - d0);
            }
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        }
        if (!isEarly && IsTwoStatesResearchOrLicenseActive()) {
            int localSt = TwoStates_LocalState();
            if (localSt != 1 && localSt != 2) localSt = 1;
            WriteSliceToWorldResearch(world, &g_engineResearch[localSt - 1]);
            WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[localSt - 1]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    TwoStates_FlushEconHookPerf();
}

void TwoStates_AfterStep(BYTE* world)
{
    TwoStates_AfterSubStep(world, false);
}

void TwoStates_ToolMoneyBegin(double* outSavedSimRub, double* outSavedSimUsd)
{
    TwoStates_RestoreUiMoneySwap();
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
    DiagLog_BeginSafeSeh();
    __try {
        *outSavedSimRub = *WorldRubles();
        *outSavedSimUsd = *WorldDollars();
        *WorldRubles()  = TwoStates_LocalRubles();
        *WorldDollars() = TwoStates_LocalDollars();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void TwoStates_ToolMoneyEnd(double savedSimRub, double savedSimUsd)
{
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
    DiagLog_BeginSafeSeh();
    __try {
        *WorldRubles()  = savedSimRub;
        *WorldDollars() = savedSimUsd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

// ---------------------------------------------------------------------------
// Territory, Building & Vehicle Ownership
// ---------------------------------------------------------------------------

int TwoStates_BuildingPosKey(float x, float z)
{
    if (!isfinite(x) || !isfinite(z)) return 0;
    uint32_t qx = (uint32_t)(int32_t)lroundf(x * 10.0f);
    uint32_t qz = (uint32_t)(int32_t)lroundf(z * 10.0f);
    uint32_t h = qx * 2654435761u ^ (qz * 2246822519u);
    h ^= (h >> 16);
    h *= 0x85EBCA6Bu;
    h ^= (h >> 13);
    return (int)(0x40000000u | (h & 0x3FFFFFFFu));
}

static void RegisterEntityOwner(std::unordered_map<int, int>& ownersMap, int entityId, int minId, int ownerState, bool bumpBldGen)
{
    if (entityId < minId || (ownerState != 1 && ownerState != 2)) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    ownersMap[entityId] = ownerState;
    if (bumpBldGen) TwoStates_BumpOwnerMapGen();
    else TwoStates_BumpVehOwnerMapGen();
    LeaveCriticalSection(&g_ownerLock);
}

void TwoStates_RegisterBuildingOwner(int buildingId, int ownerState)
{
    RegisterEntityOwner(g_buildingOwners, buildingId, 1, ownerState, true);
}

int TwoStates_GetBuildingOwner(int buildingId, float x, float z)
{
    int posKey = TwoStates_BuildingPosKey(x, z);
    if (posKey > 0 || buildingId > 2) {
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        int found = 0;
        if (posKey > 0) {
            auto itPos = g_buildingOwners.find(posKey);
            if (itPos != g_buildingOwners.end()) found = itPos->second;
        }
        if (found != 1 && found != 2 && buildingId > 2 && buildingId != posKey) {
            auto itId = g_buildingOwners.find(buildingId);
            if (itId != g_buildingOwners.end()) found = itId->second;
        }
        LeaveCriticalSection(&g_ownerLock);
        if (found == 1 || found == 2) return found;
    }
    return (x < 0.0f) ? 1 : 2;
}

void TwoStates_RegisterVehicleOwner(int vehicleId, int ownerState)
{
    RegisterEntityOwner(g_vehicleOwners, vehicleId, 0, ownerState, false);
}

int TwoStates_GetVehicleOwner(int vehicleId, const BYTE* vehiclePtr)
{
    if (vehicleId >= 0) {
        int found = LookupVehIdOwnerInCache(vehicleId);
        if (found == 1 || found == 2) return found;
    }
    if (vehiclePtr) {
        int resolvedOwner = 0;
        DiagLog_BeginSafeSeh();
        __try {
            const BldPtrOwnerSlot* slots = nullptr;
            size_t mask = 0;
            TwoStates_GetActiveBldPtrCacheView(&slots, &mask);
            resolvedOwner = ResolveVehicleOwnerFallbackRaw(vehiclePtr, slots, mask);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            resolvedOwner = 0;
        }
        DiagLog_EndSafeSeh();
        if (resolvedOwner != 0) return resolvedOwner;
    }
    return 0;
}

int TwoStates_GetZoneAt(float x, float z)
{
    (void)z;
    if (x < -g_borderHalfWidth) return 1; // West Republic
    if (x >  g_borderHalfWidth) return 2; // East Republic
    return 0;                             // Shared Border Zone
}

int TwoStates_GetStateAtWorldPos(float x, float z)
{
    return TwoStates_GetBuildingOwner(0, x, z);
}

bool TwoStates_CanBuildAt(float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int zone = TwoStates_GetZoneAt(x, z);
    // Each state can build on its own side AND inside the shared border strip (zone == 0).
    return zone == 0 || zone == s;
}

bool TwoStates_CanDemolishAt(float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int zone = TwoStates_GetZoneAt(x, z);
    if (zone == 0) {
        int owner = TwoStates_GetBuildingOwner(0, x, z);
        return owner == s;
    }
    return zone == s;
}

bool TwoStates_CanControlBuilding(int buildingId, float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int owner = TwoStates_GetBuildingOwner(buildingId, x, z);
    return owner == s;
}

bool TwoStates_CanControlVehicle(const BYTE* vehiclePtr, int state)
{
    if (!TwoStates_IsActive() || !vehiclePtr) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int id = -1;
    bool readOk = false;
    DiagLog_BeginSafeSeh();
    __try {
        id = *(const int*)(vehiclePtr + VEH_ID_OFFSET);
        readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }
    DiagLog_EndSafeSeh();
    if (!readOk) return true;
    int owner = TwoStates_GetVehicleOwner(id, vehiclePtr);
    return owner == s;
}

static void NotifyTerritoryRefused(const char* actionDesc, float x)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastRefuseLogMs < 1500) return;
    g_lastRefuseLogMs = now;
    int other = (TwoStates_LocalState() == 1) ? 2 : 1;
    const char* act = actionDesc ? actionDesc : Tr("действие", "action");
    SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Запрещено (%s на X=%.0f): это территория/собственность «%s»!",
                  "[TWO STATES] Forbidden (%s at X=%.0f): territory/property of '%s'!"),
               act, x, TwoStates_StateName(other));
    char toast[220];
    _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                Tr("⛔ Запрещено (%s): собственность «%s»!",
                   "⛔ Forbidden (%s): property of '%s'!"),
                act, TwoStates_StateName(other));
    TwoStates_AddToast(toast, 3);
}

bool TwoStates_CheckBuildAllowed(float x, float z, const char* actionDesc)
{
    if (TwoStates_CanBuildAt(x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckDemolishAllowed(float x, float z, const char* actionDesc)
{
    if (TwoStates_CanDemolishAt(x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckBuildingControlAllowed(int buildingId, float x, float z, const char* actionDesc)
{
    if (TwoStates_CanControlBuilding(buildingId, x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckBuildingControlAllowed(const BYTE* buildingPtr, const char* actionDesc)
{
    if (!TwoStates_IsActive() || !buildingPtr) return true;
    float x = 0.0f, z = 0.0f;
    bool readOk = false;
    DiagLog_BeginSafeSeh();
    __try {
        x = *(const float*)(buildingPtr + OBJ_POS_OFFSET);
        z = *(const float*)(buildingPtr + OBJ_POS_OFFSET + 8);
        readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }
    DiagLog_EndSafeSeh();
    if (!readOk) return true;
    return TwoStates_CheckBuildingControlAllowed(TwoStates_BuildingPosKey(x, z), x, z,
                                                 actionDesc ? actionDesc : Tr("настройка чужого здания", "configuring foreign building"));
}

bool TwoStates_CheckVehicleControlAllowed(const BYTE* vehiclePtr, const char* actionDesc)
{
    if (TwoStates_CanControlVehicle(vehiclePtr)) return true;
    float vx = 0.0f;
    DiagLog_BeginSafeSeh();
    __try { if (vehiclePtr) vx = *(const float*)(vehiclePtr + VEH_POS_OFFSET); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DiagLog_EndSafeSeh();
    NotifyTerritoryRefused(actionDesc, vx);
    return false;
}

// ---------------------------------------------------------------------------
// Entity Pruning (Feature 35 & 36)
// ---------------------------------------------------------------------------

static void CollectLiveStructures(size_t beginRva, int count, std::unordered_set<int>& outLive)
{
    uintptr_t vecBegin = 0;
    if (count <= 0 || !SafeReadPod(g_exeBase + beginRva, &vecBegin) || vecBegin == 0) return;
    for (int i = 0; i < count; ++i) {
        uintptr_t ptr = 0;
        if (SafeReadPod((const void*)(vecBegin + (size_t)i * sizeof(void*)), &ptr) && ptr != 0) {
            float x = 0.0f, z = 0.0f;
            SafeReadPod((const void*)(ptr + OBJ_POS_OFFSET), &x);
            SafeReadPod((const void*)(ptr + OBJ_POS_OFFSET + 8), &z);
            outLive.insert(TwoStates_BuildingPosKey(x, z));
            int id = 0;
            if (SafeReadPod((const void*)ptr, &id) && id > 0) {
                outLive.insert(id);
            }
        }
    }
}

void TwoStates_PruneDeadEntities(bool checkWorldLoaded)
{
    if (!g_exeBase) return;
    if (checkWorldLoaded && !SafeIsWorldLoaded(g_exeBase)) return;

    std::unordered_set<int> liveVehicles;
    std::unordered_set<int> liveBuildings;

    // Collect live vehicles via SafeReadPod
    int vehCount = GetWorldVehicleCount();
    uintptr_t vehVecBegin = 0;
    if (vehCount > 0 && SafeReadPod(g_exeBase + VEC_VEHICLES_BEGIN_RVA, &vehVecBegin) && vehVecBegin != 0) {
        for (int i = 0; i < vehCount; ++i) {
            uintptr_t vehPtr = 0;
            if (SafeReadPod((const void*)(vehVecBegin + (size_t)i * sizeof(void*)), &vehPtr) && vehPtr != 0) {
                int vehId = -1;
                if (SafeReadPod((const void*)(vehPtr + VEH_ID_OFFSET), &vehId) && vehId >= 0) {
                    liveVehicles.insert(vehId);
                }
            }
        }
    }

    // Collect live buildings & constructions via SafeReadPod
    CollectLiveStructures(VEC_BUILDINGS_BEGIN_RVA, GetWorldBuildingCount(), liveBuildings);
    CollectLiveStructures(VEC_CONSTRUCTIONS_BEGIN_RVA, GetWorldConstructionCount(), liveBuildings);

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    bool erasedVeh = false;
    for (auto it = g_vehicleOwners.begin(); it != g_vehicleOwners.end(); ) {
        if (liveVehicles.find(it->first) == liveVehicles.end()) {
            it = g_vehicleOwners.erase(it);
            erasedVeh = true;
        } else {
            ++it;
        }
    }
    if (erasedVeh) TwoStates_BumpVehOwnerMapGen();
    bool erasedBld = false;
    for (auto it = g_buildingOwners.begin(); it != g_buildingOwners.end(); ) {
        if (liveBuildings.find(it->first) == liveBuildings.end()) {
            it = g_buildingOwners.erase(it);
            erasedBld = true;
        } else {
            ++it;
        }
    }
    if (erasedBld) TwoStates_BumpOwnerMapGen();
    LeaveCriticalSection(&g_ownerLock);
}
