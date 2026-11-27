// SPDX-License-Identifier: GPL-3.0-only
// Phase 4: True Space Race (dlc5) per-state milestone, orbit ownership & UI isolation.
#include "two_states_internal.h"
#include "../../lockstep/world_refs.h"

void TwoStates_BindSpace(const TsmHost* host) { if (host) TsmBind(host); }

// ---------------------------------------------------------------------------
// Cross-Module Shared Space State (owned by two_states_space.cpp)
// ---------------------------------------------------------------------------

StateSpaceMilestones g_spaceMilestones[2];
std::vector<uint8_t> g_orbitOwners;

// ---------------------------------------------------------------------------
// Module-Internal Caches, TLS Context & Hook Trampolines
// ---------------------------------------------------------------------------

struct SpaceVehKeyCache {
    const BYTE* world = nullptr;
    const BYTE* vecBegin = nullptr;
    int         count = 0;
    uint64_t    worldGen = 0;
    std::vector<std::string> keys;
};

static SpaceVehKeyCache g_spaceVehKeyCache;
static const BYTE* g_mountedSpaceWorld = nullptr;
static const BYTE* g_mountedSpaceBegin = nullptr;
static int         g_mountedSpaceCount = 0;
static int         g_mountedSpaceState = 0;
static uint64_t    g_mountedSpaceGen   = 0;

static thread_local int t_prodLineOwner    = 0;
static thread_local int t_spaceActiveOwner = 0;
static thread_local const uintptr_t* t_mcSavedBlds = nullptr;
static thread_local const uintptr_t* t_mcSavedVehs = nullptr;

typedef void  (__fastcall* ProdLineStepFn)(BYTE* world, BYTE* building);
typedef BYTE* (__fastcall* CreateVehicleFn)(BYTE* world, BYTE* vt, BYTE* bld, uintptr_t a4, uintptr_t a5,
                                            uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9,
                                            uintptr_t a10, uintptr_t a11);
typedef void  (__fastcall* SpaceVehStepFn)(BYTE* veh);
typedef void  (__fastcall* RemoveOrbitFn)(BYTE* world, int orbitIdx);
typedef void  (__fastcall* SpawnSpaceCargoFn)(BYTE* world);
typedef void  (__fastcall* MissionControlWinFn)(BYTE* world, BYTE* window);
typedef void  (__fastcall* TopBarHudDrawFn)(BYTE* world);

static ProdLineStepFn      o_prodLineStep = nullptr;
static CreateVehicleFn     o_createVehicle = nullptr;
static SpaceVehStepFn      o_spaceVehOrbitStep = nullptr;
static SpaceVehStepFn      o_spaceVehReentryStep = nullptr;
static RemoveOrbitFn       o_removeOrbit = nullptr;
static SpawnSpaceCargoFn   o_spawnSpaceCargoAtBorder = nullptr;
static MissionControlWinFn o_missionControlWin = nullptr;
static TopBarHudDrawFn     o_topBarHudDraw = nullptr;

// Verified prologues against SOVIET64.exe v1.2.0.9 (PE 0x6B05A574).
static const BYTE kProdLineStepExpect[15] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kCreateVehicleExpect[15] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC
};
static const BYTE kSpaceVehOrbitStepExpect[21] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0x90, 0xFE, 0xFF, 0xFF
};
static const BYTE kSpaceVehReentryStepExpect[16] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x78, 0x10, 0x55, 0x48, 0x8D, 0x68, 0xA1
};
static const BYTE kRemoveOrbitExpect[14] = {
    0x85, 0xD2, 0x0F, 0x88, 0x9D, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57
};
static const BYTE kSpawnSpaceCargoExpect[16] = {
    0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60
};
static const BYTE kMissionControlWinExpect[14] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};
static const BYTE kTopBarHudDrawExpect[19] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xA8, 0xE8, 0xF3, 0xFF, 0xFF
};

// ---------------------------------------------------------------------------
// VehicleType Normalized Key Cache & POD SEH Batch Helpers
// ---------------------------------------------------------------------------

static bool SafeReadSpaceVehTypeVecHeader(const BYTE* world, BYTE** outBegin, int* outCount)
{
    return SafeReadStridedWorldVecHeader(world, WORLD_VEHICLE_TYPES_OFFSET, WORLD_VEHICLE_TYPE_STRIDE,
                                         8192, outBegin, outCount);
}

static std::string BuildVehTypeSpaceKey(const BYTE* vt, int idx)
{
    char normName[128] = {};
    if (Ref_ReadVehicleTypeScriptName((U64)(uintptr_t)vt, normName, sizeof(normName)) && normName[0]) {
        return std::string(normName);
    }
    return "__idx_" + std::to_string(idx);
}

static bool EnsureSpaceVehKeyCache(const BYTE* world, BYTE** outBegin, int* outCount)
{
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadSpaceVehTypeVecHeader(world, &begin, &count)) return false;
    if (outBegin) *outBegin = begin;
    if (outCount) *outCount = count;

    if (g_spaceVehKeyCache.world == world &&
        g_spaceVehKeyCache.vecBegin == begin &&
        g_spaceVehKeyCache.count == count &&
        (int)g_spaceVehKeyCache.keys.size() == count) {
        return true;
    }

    g_spaceVehKeyCache.keys.resize((size_t)count);
    for (int i = 0; i < count; ++i) {
        const BYTE* vt = begin + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE;
        g_spaceVehKeyCache.keys[(size_t)i] = BuildVehTypeSpaceKey(vt, i);
    }
    g_spaceVehKeyCache.world    = world;
    g_spaceVehKeyCache.vecBegin = begin;
    g_spaceVehKeyCache.count    = count;
    g_spaceVehKeyCache.worldGen++;
    g_mountedSpaceState = 0;
    return true;
}

static bool ReadSpaceMilestonesSeh(const BYTE* begin, int count, SpaceMilestoneTriple* outTriples)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            const BYTE* vt = begin + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE;
            outTriples[i].built   = *(const int32_t*)(vt + VEH_TYPE_BUILT_IN_PROD_OFFSET);
            outTriples[i].orbit   = *(const int32_t*)(vt + VEH_TYPE_SPACE_ORBIT_OFFSET);
            outTriples[i].reentry = *(const int32_t*)(vt + VEH_TYPE_SPACE_REENTRY_OFFSET);
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool WriteSpaceMilestonesSeh(BYTE* begin, int count, const SpaceMilestoneTriple* triples)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            BYTE* vt = begin + (size_t)i * WORLD_VEHICLE_TYPE_STRIDE;
            *(int32_t*)(vt + VEH_TYPE_BUILT_IN_PROD_OFFSET) = triples[i].built;
            *(int32_t*)(vt + VEH_TYPE_SPACE_ORBIT_OFFSET)   = triples[i].orbit;
            *(int32_t*)(vt + VEH_TYPE_SPACE_REENTRY_OFFSET) = triples[i].reentry;
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void SyncSpaceSliceCache(const StateSpaceMilestones& slice, int count)
{
    if (slice.cachedGen == slice.gen &&
        slice.cachedWorldGen == g_spaceVehKeyCache.worldGen &&
        (int)slice.cachedTriples.size() == count) {
        return;
    }
    slice.cachedTriples.assign((size_t)count, SpaceMilestoneTriple{});
    for (int i = 0; i < count; ++i) {
        auto it = slice.byKey.find(g_spaceVehKeyCache.keys[(size_t)i]);
        if (it != slice.byKey.end()) {
            slice.cachedTriples[(size_t)i] = it->second;
        }
    }
    slice.cachedGen = slice.gen;
    slice.cachedWorldGen = g_spaceVehKeyCache.worldGen;
}

void TwoStates_InvalidateSpaceMountCache()
{
    g_mountedSpaceState = 0;
}

static std::string ResolveVehTypeSpaceKey(const BYTE* world, const BYTE* vt)
{
    BYTE* begin = nullptr;
    int count = 0;
    if (world && vt && EnsureSpaceVehKeyCache(world, &begin, &count) && begin && vt >= begin) {
        const ptrdiff_t diff = vt - begin;
        if ((diff % (ptrdiff_t)WORLD_VEHICLE_TYPE_STRIDE) == 0) {
            const int idx = (int)(diff / (ptrdiff_t)WORLD_VEHICLE_TYPE_STRIDE);
            if (idx >= 0 && idx < count && (size_t)idx < g_spaceVehKeyCache.keys.size()) {
                return g_spaceVehKeyCache.keys[(size_t)idx];
            }
        }
    }
    return BuildVehTypeSpaceKey(vt, -1);
}

void CaptureWorldSpaceMilestonesToSlice(const BYTE* world, StateSpaceMilestones* dst)
{
    if (!world || !dst) return;
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureSpaceVehKeyCache(world, &begin, &count)) return;

    static thread_local std::vector<SpaceMilestoneTriple> t_rawTriples;
    t_rawTriples.resize((size_t)count);
    if (!ReadSpaceMilestonesSeh(begin, count, t_rawTriples.data())) return;

    // Update only local VehicleType keys in-place so Mixed-DLC keys from coop meta survive intact.
    for (int i = 0; i < count; ++i) {
        const std::string& key = g_spaceVehKeyCache.keys[(size_t)i];
        const SpaceMilestoneTriple& tr = t_rawTriples[(size_t)i];
        if (tr.built != 0 || tr.orbit != 0 || tr.reentry != 0) {
            dst->byKey[key] = tr;
        } else {
            dst->byKey.erase(key);
        }
    }
    dst->gen++;
    dst->initialized = true;
    dst->loadedFromMeta = false;
}

void WriteSliceToWorldSpaceMilestones(BYTE* world, const StateSpaceMilestones* src, int stateOwner)
{
    if (!world || !src || !src->initialized) return;
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureSpaceVehKeyCache(world, &begin, &count)) return;

    if (stateOwner != 0 &&
        g_mountedSpaceWorld == world &&
        g_mountedSpaceBegin == begin &&
        g_mountedSpaceCount == count &&
        g_mountedSpaceState == stateOwner &&
        g_mountedSpaceGen == src->gen) {
        return;
    }

    SyncSpaceSliceCache(*src, count);
    if (WriteSpaceMilestonesSeh(begin, count, src->cachedTriples.data())) {
        g_mountedSpaceWorld = world;
        g_mountedSpaceBegin = begin;
        g_mountedSpaceCount = count;
        g_mountedSpaceState = stateOwner;
        g_mountedSpaceGen   = src->gen;
    } else {
        g_mountedSpaceState = 0;
    }
}

void TwoStates_MountSpaceMilestonesForState(BYTE* world, int stateOwner)
{
    if (!world || (stateOwner != 1 && stateOwner != 2)) return;
    EnsureSpaceMilestonesInitialized(world);
    if (g_spaceMilestones[stateOwner - 1].initialized) {
        g_mountedSpaceState = 0;
        WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[stateOwner - 1], stateOwner);
    }
}

void EnsureSpaceMilestonesInitialized(BYTE* world)
{
    if (!world) return;
    if (g_spaceMilestones[0].initialized && g_spaceMilestones[1].initialized &&
        !g_spaceMilestones[0].loadedFromMeta && !g_spaceMilestones[1].loadedFromMeta) {
        return;
    }
    BYTE* begin = nullptr;
    int count = 0;
    if (!EnsureSpaceVehKeyCache(world, &begin, &count)) return;

    bool changed = false;
    for (int s = 0; s < 2; ++s) {
        if (!g_spaceMilestones[s].initialized) {
            CaptureWorldSpaceMilestonesToSlice(world, &g_spaceMilestones[s]);
            changed = true;
        } else if (g_spaceMilestones[s].loadedFromMeta) {
            g_spaceMilestones[s].loadedFromMeta = false;
            g_spaceMilestones[s].gen++;
            changed = true;
        }
    }
    if (changed) {
        g_mountedSpaceState = 0;
        int activeState = ResolveActiveResearchWorldState();
        WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[activeState - 1], activeState);
    }
}

void ResetSpaceStateSlices()
{
    for (int s = 0; s < 2; ++s) {
        g_spaceMilestones[s].byKey.clear();
        g_spaceMilestones[s].cachedTriples.clear();
        g_spaceMilestones[s].cachedGen = 0;
        g_spaceMilestones[s].cachedWorldGen = 0;
        g_spaceMilestones[s].gen++;
        g_spaceMilestones[s].initialized = false;
        g_spaceMilestones[s].loadedFromMeta = false;
    }
    g_spaceVehKeyCache.world    = nullptr;
    g_spaceVehKeyCache.vecBegin = nullptr;
    g_spaceVehKeyCache.count    = 0;
    g_spaceVehKeyCache.keys.clear();
    g_spaceVehKeyCache.worldGen++;

    g_mountedSpaceWorld = nullptr;
    g_mountedSpaceBegin = nullptr;
    g_mountedSpaceCount = 0;
    g_mountedSpaceState = 0;
    g_mountedSpaceGen   = 0;

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_orbitOwners.clear();
    LeaveCriticalSection(&g_ownerLock);
}

// ---------------------------------------------------------------------------
// Orbit Ownership Tracking (parallel 1:1 with WORLD + 0x20 ORBIT_SLOT vector)
// ---------------------------------------------------------------------------

static bool ReadWorldOrbitVecCount(const BYTE* world, size_t* outCount)
{
    *outCount = 0;
    if (!world) return false;
    uintptr_t ptrs[2] = {};
    if (!SafeMemcpyPod(ptrs, world + WORLD_ORBITS_VEC_OFFSET, sizeof(ptrs))) return false;
    if (!ptrs[0] || !ptrs[1]) return ptrs[0] == ptrs[1];
    if (!IsPointerLikeX64((uint64_t)ptrs[0]) || ptrs[1] < ptrs[0] ||
        ((ptrs[1] - ptrs[0]) % ORBIT_SLOT_STRIDE) != 0) {
        return false;
    }
    size_t n = (size_t)((ptrs[1] - ptrs[0]) / ORBIT_SLOT_STRIDE);
    if (n > 1024u) n = 1024u;
    *outCount = n;
    return true;
}

bool TwoStates_TryGetOrbitOwner(int orbitIdx, int* outOwner)
{
    if (orbitIdx < 0) return false;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    bool found = false;
    if ((size_t)orbitIdx < g_orbitOwners.size()) {
        uint8_t v = g_orbitOwners[(size_t)orbitIdx];
        if (outOwner) *outOwner = (v == 2) ? 2 : 1;
        found = true;
    }
    LeaveCriticalSection(&g_ownerLock);
    return found;
}

int TwoStates_GetOrbitOwner(int orbitIdx)
{
    int owner = 1;
    TwoStates_TryGetOrbitOwner(orbitIdx, &owner);
    return owner;
}

void TwoStates_OnOrbitsCreated(size_t countBefore, size_t countAfter, int ownerState)
{
    const uint8_t tag = (ownerState == 2) ? 2u : 1u;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    if (countBefore > g_orbitOwners.size()) {
        g_orbitOwners.resize(countBefore, 1u);
    }
    if (countAfter > g_orbitOwners.size()) {
        g_orbitOwners.resize(countAfter, tag);
    }
    LeaveCriticalSection(&g_ownerLock);
}

void TwoStates_OnOrbitRemoved(int orbitIdx)
{
    if (orbitIdx < 0) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    if ((size_t)orbitIdx < g_orbitOwners.size()) {
        g_orbitOwners.erase(g_orbitOwners.begin() + orbitIdx);
    }
    LeaveCriticalSection(&g_ownerLock);
}

static bool ReadVehOrbitAndIdSeh(const BYTE* v, int32_t* outOrbitIdx, int32_t* outVehId)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        *outOrbitIdx = *(const int32_t*)(v + VEH_ORBIT_INDEX_OFFSET);
        *outVehId    = *(const int32_t*)(v + VEH_ID_OFFSET);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void TwoStates_ReconcileOrbitOwnersFromWorld(const BYTE* world)
{
    if (!world) return;
    size_t liveCount = 0;
    if (!ReadWorldOrbitVecCount(world, &liveCount)) return;

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    const bool sizeMatches = (g_orbitOwners.size() == liveCount);
    LeaveCriticalSection(&g_ownerLock);
    if (sizeMatches) return;

    std::vector<uint8_t> inferred(liveCount, 1u);
    BYTE* vehBeg = nullptr;
    int vehCount = 0;
    if (SafeReadStridedWorldVecHeader(world, WORLD_VEHICLES_OFFSET, sizeof(BYTE*), 65536, &vehBeg, &vehCount)) {
        BYTE* const* vl = (BYTE* const*)vehBeg;
        for (int i = 0; i < vehCount; ++i) {
            BYTE* v = nullptr;
            if (!SafeReadPod(&vl[i], &v) || !v || !IsPointerLikeX64((uint64_t)(uintptr_t)v)) continue;
            int32_t orbIdx = -1, vehId = -1;
            if (ReadVehOrbitAndIdSeh(v, &orbIdx, &vehId) && orbIdx >= 0 && (size_t)orbIdx < liveCount) {
                EnsureOwnerLock();
                EnterCriticalSection(&g_ownerLock);
                auto it = g_vehicleOwners.find(vehId);
                int known = (it != g_vehicleOwners.end()) ? it->second : 0;
                LeaveCriticalSection(&g_ownerLock);
                if (known == 1 || known == 2) {
                    inferred[(size_t)orbIdx] = (uint8_t)known;
                }
            }
        }
    }

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    for (size_t i = 0; i < liveCount && i < g_orbitOwners.size(); ++i) {
        inferred[i] = g_orbitOwners[i];
    }
    g_orbitOwners.swap(inferred);
    LeaveCriticalSection(&g_ownerLock);
}

// ---------------------------------------------------------------------------
// StateHash & Desync Diagnostic Support (order-independent Mixed-DLC safe)
// ---------------------------------------------------------------------------

static void MixInt32SpaceHash(unsigned long long& h, int v)
{
    const unsigned char* p = (const unsigned char*)&v;
    for (size_t i = 0; i < sizeof(v); ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
}

static unsigned long long Finalize64SpaceHash(unsigned long long x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

bool TwoStates_HashSpaceMilestones(unsigned long long* ioHash)
{
    if (!ioHash || !IsTwoStatesResearchOrLicenseActive()) return false;
    if (g_exeBase) {
        EnsureSpaceMilestonesInitialized(g_exeBase + WORLD_RVA);
    }
    unsigned long long h = *ioHash;
    for (int s = 0; s < 2; ++s) {
        int activeCount = 0;
        unsigned long long accSum = 0;
        unsigned long long accXor = 0;
        for (const auto& kv : g_spaceMilestones[s].byKey) {
            const SpaceMilestoneTriple& tr = kv.second;
            if (tr.built == 0 && tr.orbit == 0 && tr.reentry == 0) continue;
            unsigned long long eh = 0xcbf29ce484222325ull;
            const size_t klen = std::min<size_t>(kv.first.size(), 64u);
            for (size_t i = 0; i < klen; ++i) {
                eh ^= (unsigned char)kv.first[i];
                eh *= 0x100000001b3ull;
            }
            MixInt32SpaceHash(eh, tr.built);
            MixInt32SpaceHash(eh, tr.orbit);
            MixInt32SpaceHash(eh, tr.reentry);
            eh = Finalize64SpaceHash(eh);
            accSum += eh;
            accXor ^= eh;
            ++activeCount;
        }
        MixInt32SpaceHash(h, activeCount);
        const unsigned char* sp = (const unsigned char*)&accSum;
        for (size_t i = 0; i < sizeof(accSum); ++i) { h ^= sp[i]; h *= 0x100000001b3ull; }
        const unsigned char* xp = (const unsigned char*)&accXor;
        for (size_t i = 0; i < sizeof(accXor); ++i) { h ^= xp[i]; h *= 0x100000001b3ull; }
    }
    *ioHash = h;
    return true;
}

void TwoStates_HashOrbitOwners(unsigned long long* ioHash)
{
    if (!ioHash || !TwoStates_IsActive()) return;
    unsigned long long h = *ioHash;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    MixInt32SpaceHash(h, (int)g_orbitOwners.size());
    for (uint8_t owner : g_orbitOwners) {
        h ^= owner;
        h *= 0x100000001b3ull;
    }
    LeaveCriticalSection(&g_ownerLock);
    *ioHash = h;
}

void TwoStates_LogSpaceDesyncDetails()
{
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    std::string owners = SerializeOrbitOwners();
    LeaveCriticalSection(&g_ownerLock);
    Logf("republicmp  DESYNC_DUMP[two_states_space] orbitOwners=[%s] m1=%zu m2=%zu",
         owners.c_str(), g_spaceMilestones[0].byKey.size(), g_spaceMilestones[1].byKey.size());
}

// ---------------------------------------------------------------------------
// republicmp_coop.ini Serialization & Parsing
// ---------------------------------------------------------------------------

static bool ComparePairByKeyAsc(const std::pair<std::string, SpaceMilestoneTriple>& a,
                                const std::pair<std::string, SpaceMilestoneTriple>& b)
{
    return a.first < b.first;
}

std::string SerializeSpaceMilestones(const StateSpaceMilestones& slice)
{
    std::vector<std::pair<std::string, SpaceMilestoneTriple>> sorted;
    sorted.reserve(slice.byKey.size());
    for (const auto& kv : slice.byKey) {
        const SpaceMilestoneTriple& tr = kv.second;
        if (tr.built != 0 || tr.orbit != 0 || tr.reentry != 0) {
            sorted.push_back(kv);
        }
    }
    std::sort(sorted.begin(), sorted.end(), ComparePairByKeyAsc);
    std::ostringstream out;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) out << ';';
        out << sorted[i].first << ':' << sorted[i].second.built << ':'
            << sorted[i].second.orbit << ':' << sorted[i].second.reentry;
    }
    return out.str();
}

void ParseSpaceMilestones(const std::string& val, StateSpaceMilestones* dst)
{
    if (!dst) return;
    dst->byKey.clear();
    std::istringstream ss(val);
    std::string tok;
    while (std::getline(ss, tok, ';')) {
        if (tok.empty()) continue;
        size_t c3 = tok.rfind(':');
        if (c3 == std::string::npos || c3 == 0) continue;
        size_t c2 = tok.rfind(':', c3 - 1);
        if (c2 == std::string::npos || c2 == 0) continue;
        size_t c1 = tok.rfind(':', c2 - 1);
        if (c1 == std::string::npos || c1 == 0) continue;
        std::string rawKey = tok.substr(0, c1);
        char normBuf[128] = {};
        std::string normKey = rawKey;
        if (rawKey.rfind("__idx_", 0) != 0) {
            Ref_NormalizeVehicleScriptName(rawKey.c_str(), normBuf, sizeof(normBuf));
            if (normBuf[0]) normKey = normBuf;
        }
        SpaceMilestoneTriple tr = {};
        tr.built   = atoi(tok.substr(c1 + 1, c2 - c1 - 1).c_str());
        tr.orbit   = atoi(tok.substr(c2 + 1, c3 - c2 - 1).c_str());
        tr.reentry = atoi(tok.substr(c3 + 1).c_str());
        if (tr.built != 0 || tr.orbit != 0 || tr.reentry != 0) {
            dst->byKey[normKey] = tr;
        }
    }
    dst->gen++;
    dst->initialized = true;
    dst->loadedFromMeta = true;
}

std::string SerializeOrbitOwners()
{
    std::ostringstream out;
    for (size_t i = 0; i < g_orbitOwners.size(); ++i) {
        if (i > 0) out << ',';
        out << (int)(g_orbitOwners[i] == 2 ? 2 : 1);
    }
    return out.str();
}

void ParseOrbitOwners(const std::string& val)
{
    std::vector<uint8_t> parsed;
    std::istringstream ss(val);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        int o = atoi(tok.c_str());
        parsed.push_back((o == 2) ? 2u : 1u);
    }
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_orbitOwners.swap(parsed);
    LeaveCriticalSection(&g_ownerLock);
}

// ---------------------------------------------------------------------------
// Function-Entry Hook Implementations (Zero Mid-Function Patches)
// ---------------------------------------------------------------------------

static void RestoreSpaceHooksMountState(BYTE* world, int resState, int spaceState)
{
    if (resState == 1 || resState == 2) {
        WriteSliceToWorldResearch(world, &g_engineResearch[resState - 1]);
    }
    if (spaceState == 1 || spaceState == 2) {
        WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[spaceState - 1], spaceState);
    }
}

static void CallProdLineStepGuarded(ProdLineStepFn fn, BYTE* world, BYTE* building,
                                    int* prodOwnerSlot, int restoreOwner, int prevSpaceState)
{
    bool normalExit = false;
    __try {
        fn(world, building);
        normalExit = true;
    } __finally {
        *prodOwnerSlot = restoreOwner;
        if (!normalExit) {
            RestoreSpaceHooksMountState(world, 0, prevSpaceState);
        }
    }
}

static BYTE* CallCreateVehicleGuarded(CreateVehicleFn fn, BYTE* world, BYTE* vt, BYTE* bld,
                                      uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7,
                                      uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11,
                                      int prevSpaceState)
{
    BYTE* veh = nullptr;
    bool normalExit = false;
    __try {
        veh = fn(world, vt, bld, a4, a5, a6, a7, a8, a9, a10, a11);
        normalExit = true;
    } __finally {
        if (!normalExit && prevSpaceState != 0) {
            RestoreSpaceHooksMountState(world, 0, prevSpaceState);
        }
    }
    return veh;
}

static void RegisterVehicleListTail(const BYTE* world, int vehBefore, int vehAfter, int owner)
{
    if (!world || vehAfter <= vehBefore) return;
    BYTE* vehBeg = nullptr;
    int vehCount = 0;
    if (!SafeReadStridedWorldVecHeader(world, WORLD_VEHICLES_OFFSET, sizeof(BYTE*), 65536, &vehBeg, &vehCount)) {
        return;
    }
    if (vehAfter > vehCount) vehAfter = vehCount;
    BYTE* const* arr = (BYTE* const*)vehBeg;
    for (int i = vehBefore; i < vehAfter; ++i) {
        BYTE* v = nullptr;
        int32_t vehId = -1;
        if (SafeReadPod(&arr[i], &v) && IsPointerLikeX64((uint64_t)(uintptr_t)v) &&
            SafeReadPod((const int32_t*)(v + VEH_ID_OFFSET), &vehId) && vehId >= 0) {
            TwoStates_RegisterVehicleOwner(vehId, owner);
        }
    }
}

static void __fastcall H_ProdLineStep(BYTE* world, BYTE* building)
{
    if (!o_prodLineStep) return;
    if (!TwoStates_IsActive() || !world || !building || !IsPointerLikeX64((uintptr_t)building)) {
        o_prodLineStep(world, building);
        return;
    }
    const int owner = LookupBuildingOwnerCached(building);
    const int prevMounted = (g_mountedSpaceState == 1 || g_mountedSpaceState == 2)
        ? g_mountedSpaceState
        : (g_inSimStep ? 1 : TwoStates_LocalState());
    EnsureSpaceMilestonesInitialized(world);
    WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[owner - 1], owner);

    const int vehBefore = GetWorldVehicleCount();
    const int savedProdOwner = t_prodLineOwner;
    t_prodLineOwner = owner;
    CallProdLineStepGuarded(o_prodLineStep, world, building, &t_prodLineOwner, savedProdOwner, prevMounted);

    const int vehAfter = GetWorldVehicleCount();
    if (vehAfter > vehBefore) {
        RegisterVehicleListTail(world, vehBefore, vehAfter, owner);
        CaptureWorldSpaceMilestonesToSlice(world, &g_spaceMilestones[owner - 1]);
    }
    if (prevMounted == 1 || prevMounted == 2) {
        WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[prevMounted - 1], prevMounted);
    }
}

static BYTE* __fastcall H_CreateVehicle(BYTE* world, BYTE* vt, BYTE* bld, uintptr_t a4, uintptr_t a5,
                                        uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9,
                                        uintptr_t a10, uintptr_t a11)
{
    if (!o_createVehicle) return nullptr;
    const bool factoryBuilt = ((a7 & 0xFFu) != 0u);
    if (!TwoStates_IsActive() || !factoryBuilt || !world || !IsPointerLikeX64((uintptr_t)vt)) {
        return o_createVehicle(world, vt, bld, a4, a5, a6, a7, a8, a9, a10, a11);
    }

    int owner = t_prodLineOwner;
    if (owner != 1 && owner != 2) {
        if (bld && IsPointerLikeX64((uintptr_t)bld)) owner = LookupBuildingOwnerCached(bld);
        else if (g_inCommand) owner = g_cmdOwner;
        else owner = g_inSimStep ? 1 : TwoStates_LocalState();
        if (owner != 1 && owner != 2) owner = 1;
    }

    int prevMounted = 0;
    if (t_prodLineOwner == 0) {
        prevMounted = (g_mountedSpaceState == 1 || g_mountedSpaceState == 2)
            ? g_mountedSpaceState
            : (g_inSimStep ? 1 : TwoStates_LocalState());
        EnsureSpaceMilestonesInitialized(world);
        WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[owner - 1], owner);
    }

    int32_t builtBefore = 0;
    SafeReadPod((const int32_t*)(vt + VEH_TYPE_BUILT_IN_PROD_OFFSET), &builtBefore);
    BYTE* veh = CallCreateVehicleGuarded(o_createVehicle, world, vt, bld, a4, a5, a6, a7, a8, a9, a10, a11,
                                         (t_prodLineOwner == 0) ? prevMounted : 0);

    int32_t vehId = -1;
    if (veh && IsPointerLikeX64((uintptr_t)veh) &&
        SafeReadPod((const int32_t*)(veh + VEH_ID_OFFSET), &vehId) && vehId >= 0) {
        TwoStates_RegisterVehicleOwner(vehId, owner);
    }
    int32_t builtAfter = builtBefore;
    if (SafeReadPod((const int32_t*)(vt + VEH_TYPE_BUILT_IN_PROD_OFFSET), &builtAfter) && builtAfter != builtBefore) {
        const std::string key = ResolveVehTypeSpaceKey(world, vt);
        if (!key.empty()) {
            g_spaceMilestones[owner - 1].byKey[key].built = builtAfter;
            g_spaceMilestones[owner - 1].gen++;
        }
    }
    if (t_prodLineOwner == 0 && (prevMounted == 1 || prevMounted == 2)) {
        WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[prevMounted - 1], prevMounted);
    }
    return veh;
}

static void CallSpaceVehStepGuarded(SpaceVehStepFn fn, BYTE* veh, BYTE* world,
                                    int* tlsSlot, int restoreVal,
                                    int prevResState, int prevSpaceState)
{
    bool normalExit = false;
    __try {
        fn(veh);
        normalExit = true;
    } __finally {
        if (tlsSlot) *tlsSlot = restoreVal;
        if (!normalExit) {
            RestoreSpaceHooksMountState(world, prevResState, prevSpaceState);
        }
    }
}

static void AttributeOrbitPayoutScalarsAndMoney(BYTE* world, int owner, double prevRub, double prevUsd,
                                                float prevStatUsd, float prevStatRub)
{
    if (!IsUnlimitedMoney()) {
        double nowRub = 0.0, nowUsd = 0.0;
        SafeReadPod(WorldRubles(), &nowRub);
        SafeReadPod(WorldDollars(), &nowUsd);
        const double dRub = SanitizeMoneyStep(prevRub, nowRub) - prevRub;
        const double dUsd = SanitizeMoneyStep(prevUsd, nowUsd) - prevUsd;
        if (fabs(dRub) > 1e-6) {
            g_stateRub[owner - 1] += dRub;
            g_netTradeRub[owner - 1] += dRub;
            g_lastWorldRub += dRub;
        }
        if (fabs(dUsd) > 1e-6) {
            g_stateUsd[owner - 1] += dUsd;
            g_netTradeUsd[owner - 1] += dUsd;
            g_lastWorldUsd += dUsd;
        }
    }

    float nowStatUsd = prevStatUsd, nowStatRub = prevStatRub;
    SafeReadPod((const float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_EXPORT_USD_OFF), &nowStatUsd);
    SafeReadPod((const float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_EXPORT_RUB_OFF), &nowStatRub);
    const float dStatUsd = (isfinite(nowStatUsd) && isfinite(prevStatUsd)) ? (nowStatUsd - prevStatUsd) : 0.0f;
    const float dStatRub = (isfinite(nowStatRub) && isfinite(prevStatRub)) ? (nowStatRub - prevStatRub) : 0.0f;
    if (fabsf(dStatUsd) > 1e-6f) {
        g_stateCurEcon[owner - 1].scalars.vehicles[2] += dStatUsd;
        g_lastWorldScalars.vehicles[2] += dStatUsd;
    }
    if (fabsf(dStatRub) > 1e-6f) {
        g_stateCurEcon[owner - 1].scalars.vehicles[3] += dStatRub;
        g_lastWorldScalars.vehicles[3] += dStatRub;
    }
}

static void RunSpaceVehMilestoneStep(BYTE* veh, bool isOrbitStep, SpaceVehStepFn origFn)
{
    if (!origFn) return;
    if (!TwoStates_IsActive() || !veh || !g_exeBase || !IsPointerLikeX64((uintptr_t)veh)) {
        origFn(veh);
        return;
    }

    BYTE* world = g_exeBase + WORLD_RVA;
    int owner = ResolveVehicleOwnerFast(veh);
    if (owner != 1 && owner != 2) owner = 1;

    const int prevMountedSpace = (g_mountedSpaceState == 1 || g_mountedSpaceState == 2)
        ? g_mountedSpaceState
        : (g_inSimStep ? 1 : TwoStates_LocalState());
    const int prevMountedRes = ResolveActiveResearchWorldState();

    EnsureSpaceMilestonesInitialized(world);
    EnsureEngineResearchInitialized(world);

    BYTE* vt = nullptr;
    SafeReadPod((BYTE* const*)(veh + VEH_TYPE_PTR_OFFSET), &vt);
    const unsigned milestoneOff = isOrbitStep ? VEH_TYPE_SPACE_ORBIT_OFFSET : VEH_TYPE_SPACE_REENTRY_OFFSET;

    double prevRub = 0.0, prevUsd = 0.0;
    float prevStatUsd = 0.0f, prevStatRub = 0.0f;
    if (isOrbitStep) {
        if (!IsUnlimitedMoney()) {
            SafeReadPod(WorldRubles(), &prevRub);
            SafeReadPod(WorldDollars(), &prevUsd);
        }
        SafeReadPod((const float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_EXPORT_USD_OFF), &prevStatUsd);
        SafeReadPod((const float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_EXPORT_RUB_OFF), &prevStatRub);
    }

    WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[owner - 1], owner);
    WriteSliceToWorldResearch(world, &g_engineResearch[owner - 1]);

    // Read milestoneBefore AFTER mounting owner's slice so prior mount of another state does not mask 0->1.
    int32_t milestoneBefore = 0;
    if (vt && IsPointerLikeX64((uintptr_t)vt)) {
        SafeReadPod((const int32_t*)(vt + milestoneOff), &milestoneBefore);
    }

    const int savedSpaceOwner = t_spaceActiveOwner;
    if (isOrbitStep) {
        t_spaceActiveOwner = owner;
    }
    CallSpaceVehStepGuarded(origFn, veh, world, isOrbitStep ? &t_spaceActiveOwner : nullptr,
                            savedSpaceOwner, prevMountedRes, prevMountedSpace);

    if (isOrbitStep) {
        AttributeOrbitPayoutScalarsAndMoney(world, owner, prevRub, prevUsd, prevStatUsd, prevStatRub);
        size_t orbitCountAfter = 0;
        if (ReadWorldOrbitVecCount(world, &orbitCountAfter)) {
            TwoStates_OnOrbitsCreated(0, orbitCountAfter, owner);
        }
    }

    int32_t milestoneAfter = milestoneBefore;
    if (vt && IsPointerLikeX64((uintptr_t)vt) &&
        SafeReadPod((const int32_t*)(vt + milestoneOff), &milestoneAfter) &&
        milestoneAfter != milestoneBefore) {
        const std::string key = ResolveVehTypeSpaceKey(world, vt);
        if (!key.empty()) {
            if (isOrbitStep) g_spaceMilestones[owner - 1].byKey[key].orbit = milestoneAfter;
            else g_spaceMilestones[owner - 1].byKey[key].reentry = milestoneAfter;
            g_spaceMilestones[owner - 1].gen++;
        }
        TwoStates_RecomputeAndCaptureResearchForState(world, owner);
    }

    WriteSliceToWorldResearch(world, &g_engineResearch[prevMountedRes - 1]);
    WriteSliceToWorldSpaceMilestones(world, &g_spaceMilestones[prevMountedSpace - 1], prevMountedSpace);
}

static void __fastcall H_SpaceVehOrbitStep(BYTE* veh)
{
    RunSpaceVehMilestoneStep(veh, true, o_spaceVehOrbitStep);
}

static void __fastcall H_SpaceVehReentryStep(BYTE* veh)
{
    RunSpaceVehMilestoneStep(veh, false, o_spaceVehReentryStep);
}

static void __fastcall H_RemoveOrbit(BYTE* world, int orbitIdx)
{
    // Match 0x41DAB0 prologue: test edx, edx; js return; (the unrelocated 14B trampoline skips js).
    if (!o_removeOrbit || !world || orbitIdx < 0) return;
    size_t countBefore = 0;
    ReadWorldOrbitVecCount(world, &countBefore);
    o_removeOrbit(world, orbitIdx);
    size_t countAfter = 0;
    if (TwoStates_IsActive() && ReadWorldOrbitVecCount(world, &countAfter) && countAfter < countBefore) {
        TwoStates_OnOrbitRemoved(orbitIdx);
    }
}

struct ResearchProgBackup {
    int   idx;
    float prog;
};

static bool BoostMaxResearchProgressSeh(BYTE* begin, int count, const StateEngineResearch* r0,
                                        const StateEngineResearch* r1, ResearchProgBackup* backups,
                                        int maxBackups, int* outBackupCount)
{
    *outBackupCount = 0;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        const int n = (int)std::min<size_t>((size_t)count, std::min(r0->items.size(), r1->items.size()));
        for (int i = 0; i < n && *outBackupCount < maxBackups; ++i) {
            const float best = std::max(r0->items[(size_t)i].progress, r1->items[(size_t)i].progress);
            float* liveProg = (float*)(begin + (size_t)i * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET);
            const float cur = *liveProg;
            if (best > cur) {
                backups[(*outBackupCount)++] = { i, cur };
                *liveProg = best;
            }
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void RestoreResearchProgBackupsSeh(BYTE* begin, int count, const ResearchProgBackup* backups, int backupCount)
{
    DiagLog_BeginSafeSeh();
    __try {
        for (int k = 0; k < backupCount; ++k) {
            if (backups[k].idx >= 0 && backups[k].idx < count) {
                *(float*)(begin + (size_t)backups[k].idx * RESEARCH_ITEM_STRIDE + RESEARCH_ITEM_PROGRESS_OFFSET) =
                    backups[k].prog;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void CallSpawnSpaceCargoWithRestore(SpawnSpaceCargoFn fn, BYTE* world, BYTE* begin, int count,
                                           const ResearchProgBackup* backups, int backupCount)
{
    __try {
        fn(world);
    } __finally {
        RestoreResearchProgBackupsSeh(begin, count, backups, backupCount);
    }
}

static void __fastcall H_SpawnSpaceCargoAtBorder(BYTE* world)
{
    if (!o_spawnSpaceCargoAtBorder) return;
    if (!TwoStates_IsActive() || !IsTwoStatesResearchOrLicenseActive() || !world) {
        o_spawnSpaceCargoAtBorder(world);
        return;
    }
    EnsureEngineResearchInitialized(world);
    BYTE* begin = nullptr;
    int count = 0;
    if (!SafeReadStridedWorldVecHeader(world, WORLD_RESEARCH_VEC_BEGIN_OFFSET, RESEARCH_ITEM_STRIDE, 256, &begin, &count)) {
        o_spawnSpaceCargoAtBorder(world);
        return;
    }
    ResearchProgBackup backups[256] = {};
    int backupCount = 0;
    BoostMaxResearchProgressSeh(begin, count, &g_engineResearch[0], &g_engineResearch[1],
                                backups, (int)(sizeof(backups) / sizeof(backups[0])), &backupCount);
    CallSpawnSpaceCargoWithRestore(o_spawnSpaceCargoAtBorder, world, begin, count, backups, backupCount);
}

static void CallMissionControlWithRestore(MissionControlWinFn fn, BYTE* world, BYTE* window,
                                          bool swappedBlds, const uintptr_t savedBlds[3],
                                          bool swappedVehs, const uintptr_t savedVehs[3])
{
    const uintptr_t* prevMcBlds = t_mcSavedBlds;
    const uintptr_t* prevMcVehs = t_mcSavedVehs;
    t_mcSavedBlds = swappedBlds ? savedBlds : nullptr;
    t_mcSavedVehs = swappedVehs ? savedVehs : nullptr;
    __try {
        fn(world, window);
    } __finally {
        t_mcSavedVehs = prevMcVehs;
        t_mcSavedBlds = prevMcBlds;
        if (swappedVehs) SafeMemcpyPod(world + WORLD_VEHICLES_OFFSET, savedVehs, 3 * sizeof(uintptr_t));
        if (swappedBlds) SafeMemcpyPod(world + WORLD_BUILDINGS_OFFSET, savedBlds, 3 * sizeof(uintptr_t));
    }
}

bool TwoStates_GetUnfilteredWorldVecRange(size_t vecOffset, uintptr_t* outBegin, uintptr_t* outEnd)
{
    if (!outBegin || !outEnd) return false;
    const uintptr_t* saved = nullptr;
    if (vecOffset == WORLD_BUILDINGS_OFFSET) saved = t_mcSavedBlds;
    else if (vecOffset == WORLD_VEHICLES_OFFSET) saved = t_mcSavedVehs;
    if (!saved) return false;
    *outBegin = saved[0];
    *outEnd   = saved[1];
    return true;
}

static void __fastcall H_MissionControlWindow(BYTE* world, BYTE* window)
{
    if (!o_missionControlWin) return;
    if (!TwoStates_IsActive() || !world || !window) {
        o_missionControlWin(world, window);
        return;
    }
    const int localSt = TwoStates_LocalState();
    uintptr_t savedBlds[3] = {};
    uintptr_t savedVehs[3] = {};
    static thread_local std::vector<BYTE*> t_filteredMcBlds;
    static thread_local std::vector<BYTE*> t_filteredMcVehs;
    const bool swappedBlds = SwapWorldPtrVecForState(
        world, WORLD_BUILDINGS_OFFSET, localSt, ResolveBuildingOwnerFast, 200000, savedBlds, t_filteredMcBlds);
    const bool swappedVehs = SwapWorldPtrVecForState(
        world, WORLD_VEHICLES_OFFSET, localSt, ResolveVehicleOwnerFast, 65536, savedVehs, t_filteredMcVehs);
    CallMissionControlWithRestore(o_missionControlWin, world, window, swappedBlds, savedBlds, swappedVehs, savedVehs);
}

static BYTE* FindLocalMissionControlBuilding(const BYTE* world, int localState)
{
    BYTE* bldBeg = nullptr;
    int bldCount = 0;
    if (!SafeReadStridedWorldVecHeader(world, WORLD_BUILDINGS_OFFSET, sizeof(BYTE*), 200000, &bldBeg, &bldCount)) {
        return nullptr;
    }
    BYTE* const* arr = (BYTE* const*)bldBeg;
    BYTE* found = nullptr;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < bldCount; ++i) {
            BYTE* b = arr[i];
            if (!b || !IsPointerLikeX64((uint64_t)(uintptr_t)b)) continue;
            const BYTE* bt = *(const BYTE* const*)(b + BLD_TYPE_OFFSET);
            if (!bt || !IsPointerLikeX64((uint64_t)(uintptr_t)bt)) continue;
            const uint32_t cat = *(const uint32_t*)(bt + BLD_TYPE_CAT_OFF);
            const uint32_t sub = *(const uint32_t*)(bt + BLD_TYPE_SUBTYPE_OFF);
            if (cat == BLD_CAT_MISSION_CONTROL && sub == BLD_SUBTYPE_MISSION_CONTROL &&
                *(const float*)(b + BLD_PROGRESS_OFFSET) >= 0.9999f &&
                ResolveBuildingOwnerFast(b) == localState) {
                found = b;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = nullptr;
    }
    DiagLog_EndSafeSeh();
    return found;
}

static void CallTopBarHudWithRestore(TopBarHudDrawFn fn, BYTE* world, BYTE* origMc)
{
    __try {
        fn(world);
    } __finally {
        SafeWritePod((BYTE**)(world + WORLD_MISSION_CONTROL_BLD_PTR_OFF), origMc);
    }
}

static void __fastcall H_TopBarHudDraw(BYTE* world)
{
    if (!o_topBarHudDraw) return;
    if (!TwoStates_IsActive() || !world) {
        o_topBarHudDraw(world);
        return;
    }
    const int localSt = TwoStates_LocalState();
    BYTE* origMc = nullptr;
    if (!SafeReadPod((BYTE* const*)(world + WORLD_MISSION_CONTROL_BLD_PTR_OFF), &origMc)) {
        o_topBarHudDraw(world);
        return;
    }
    BYTE* localMc = origMc;
    if (!origMc || (IsPointerLikeX64((uint64_t)(uintptr_t)origMc) && ResolveBuildingOwnerFast(origMc) != localSt)) {
        localMc = FindLocalMissionControlBuilding(world, localSt);
    }
    if (localMc != origMc) {
        SafeWritePod((BYTE**)(world + WORLD_MISSION_CONTROL_BLD_PTR_OFF), localMc);
        CallTopBarHudWithRestore(o_topBarHudDraw, world, origMc);
    } else {
        o_topBarHudDraw(world);
    }
}

// ---------------------------------------------------------------------------
// Public Runtime Accessors, Hook Installer & Test Seams
// ---------------------------------------------------------------------------

int TwoStates_GetSpaceActiveOwner()
{
    return t_spaceActiveOwner;
}

int TwoStates_InstallSpaceHooks()
{
    const TwoStatesInlineHookSpec kSpecs[] = {
        { RVA_PROD_LINE_STEP, (void*)&H_ProdLineStep, (void**)&o_prodLineStep,
          kProdLineStepExpect, sizeof(kProdLineStepExpect), "republicmp two_states prod_line_step" },
        { RVA_CREATE_VEHICLE, (void*)&H_CreateVehicle, (void**)&o_createVehicle,
          kCreateVehicleExpect, sizeof(kCreateVehicleExpect), "republicmp two_states create_vehicle" },
        { RVA_SPACE_VEH_ORBIT_STEP, (void*)&H_SpaceVehOrbitStep, (void**)&o_spaceVehOrbitStep,
          kSpaceVehOrbitStepExpect, sizeof(kSpaceVehOrbitStepExpect), "republicmp two_states space_veh_orbit_step" },
        { RVA_SPACE_VEH_REENTRY_STEP, (void*)&H_SpaceVehReentryStep, (void**)&o_spaceVehReentryStep,
          kSpaceVehReentryStepExpect, sizeof(kSpaceVehReentryStepExpect), "republicmp two_states space_veh_reentry_step" },
        { RVA_REMOVE_ORBIT, (void*)&H_RemoveOrbit, (void**)&o_removeOrbit,
          kRemoveOrbitExpect, sizeof(kRemoveOrbitExpect), "republicmp two_states remove_orbit" },
        { RVA_SPAWN_SPACE_CARGO_AT_BORDER, (void*)&H_SpawnSpaceCargoAtBorder, (void**)&o_spawnSpaceCargoAtBorder,
          kSpawnSpaceCargoExpect, sizeof(kSpawnSpaceCargoExpect), "republicmp two_states spawn_space_cargo" },
        { RVA_MISSION_CONTROL_WINDOW, (void*)&H_MissionControlWindow, (void**)&o_missionControlWin,
          kMissionControlWinExpect, sizeof(kMissionControlWinExpect), "republicmp two_states mission_control_win" },
        { RVA_TOP_BAR_HUD_DRAW, (void*)&H_TopBarHudDraw, (void**)&o_topBarHudDraw,
          kTopBarHudDrawExpect, sizeof(kTopBarHudDrawExpect), "republicmp two_states top_bar_hud_draw" },
    };
    return TwoStates_InstallInlineHookTable(kSpecs, sizeof(kSpecs) / sizeof(kSpecs[0]));
}

void TwoStates_ResetSpaceForTest()
{
    ResetSpaceStateSlices();
    g_spaceVehKeyCache = {};
    t_prodLineOwner = 0;
    t_spaceActiveOwner = 0;
}

void TwoStates_SetOrbitOwnersForTest(const int* owners, size_t count)
{
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_orbitOwners.clear();
    if (owners && count > 0) {
        g_orbitOwners.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            g_orbitOwners.push_back((owners[i] == 2) ? 2u : 1u);
        }
    }
    LeaveCriticalSection(&g_ownerLock);
}

size_t TwoStates_GetOrbitOwnerCountForTest()
{
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    size_t n = g_orbitOwners.size();
    LeaveCriticalSection(&g_ownerLock);
    return n;
}

void TwoStates_SetSpaceMilestoneForTest(int state, const char* scriptName, int built, int orbit, int reentry)
{
    if ((state != 1 && state != 2) || !scriptName || !scriptName[0]) return;
    char norm[128] = {};
    Ref_NormalizeVehicleScriptName(scriptName, norm, sizeof(norm));
    const std::string key = norm[0] ? std::string(norm) : std::string(scriptName);
    StateSpaceMilestones& slice = g_spaceMilestones[state - 1];
    slice.byKey[key] = { built, orbit, reentry };
    slice.gen++;
    slice.initialized = true;
    slice.loadedFromMeta = false;
}

bool TwoStates_GetSpaceMilestoneForTest(int state, const char* scriptName, int* outBuilt, int* outOrbit, int* outReentry)
{
    if ((state != 1 && state != 2) || !scriptName || !scriptName[0]) return false;
    char norm[128] = {};
    Ref_NormalizeVehicleScriptName(scriptName, norm, sizeof(norm));
    const std::string key = norm[0] ? std::string(norm) : std::string(scriptName);
    const StateSpaceMilestones& slice = g_spaceMilestones[state - 1];
    auto it = slice.byKey.find(key);
    if (it == slice.byKey.end()) return false;
    if (outBuilt) *outBuilt = it->second.built;
    if (outOrbit) *outOrbit = it->second.orbit;
    if (outReentry) *outReentry = it->second.reentry;
    return true;
}

int TwoStates_GetSpaceMilestoneMountStateForTest()
{
    return g_mountedSpaceState;
}

void TwoStates_InvokeProdLineStepForTest(BYTE* world, BYTE* bld, void (*origFn)(BYTE*, BYTE*))
{
    ProdLineStepFn saved = o_prodLineStep;
    o_prodLineStep = origFn;
    H_ProdLineStep(world, bld);
    o_prodLineStep = saved;
}

BYTE* TwoStates_InvokeCreateVehicleForTest(BYTE* world, BYTE* vt, BYTE* bld, unsigned char factoryBuilt,
                                           BYTE* (*origFn)(BYTE*, BYTE*, BYTE*, uintptr_t, uintptr_t, uintptr_t,
                                                           uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))
{
    CreateVehicleFn saved = o_createVehicle;
    o_createVehicle = origFn;
    BYTE* res = H_CreateVehicle(world, vt, bld, 0, 0, 0, (uintptr_t)factoryBuilt, 0, 0, 0, 0);
    o_createVehicle = saved;
    return res;
}

void TwoStates_InvokeSpaceOrbitStepForTest(BYTE* veh, void (*origFn)(BYTE*))
{
    SpaceVehStepFn saved = o_spaceVehOrbitStep;
    o_spaceVehOrbitStep = origFn;
    H_SpaceVehOrbitStep(veh);
    o_spaceVehOrbitStep = saved;
}

void TwoStates_InvokeSpaceReentryStepForTest(BYTE* veh, void (*origFn)(BYTE*))
{
    SpaceVehStepFn saved = o_spaceVehReentryStep;
    o_spaceVehReentryStep = origFn;
    H_SpaceVehReentryStep(veh);
    o_spaceVehReentryStep = saved;
}

void TwoStates_InvokeRemoveOrbitForTest(BYTE* world, int orbitIdx, void (*origFn)(BYTE*, int))
{
    RemoveOrbitFn saved = o_removeOrbit;
    o_removeOrbit = origFn;
    H_RemoveOrbit(world, orbitIdx);
    o_removeOrbit = saved;
}

void TwoStates_InvokeSpawnSpaceCargoForTest(BYTE* world, void (*origFn)(BYTE*))
{
    SpawnSpaceCargoFn saved = o_spawnSpaceCargoAtBorder;
    o_spawnSpaceCargoAtBorder = origFn;
    H_SpawnSpaceCargoAtBorder(world);
    o_spawnSpaceCargoAtBorder = saved;
}

void TwoStates_InvokeMissionControlWinForTest(BYTE* world, BYTE* window, void (*origFn)(BYTE*, BYTE*))
{
    MissionControlWinFn saved = o_missionControlWin;
    o_missionControlWin = origFn;
    H_MissionControlWindow(world, window);
    o_missionControlWin = saved;
}

void TwoStates_InvokeTopBarHudDrawForTest(BYTE* world, void (*origFn)(BYTE*))
{
    TopBarHudDrawFn saved = o_topBarHudDraw;
    o_topBarHudDraw = origFn;
    H_TopBarHudDraw(world);
    o_topBarHudDraw = saved;
}
