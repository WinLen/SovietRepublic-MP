// SPDX-License-Identifier: GPL-3.0-only
// See state_hash.h.
#include "state_hash.h"
#include "../lockstep/world_refs.h"
#include "../session/two_states.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define BLD_INDEX_OFFSET 0x00
#define VEH_INDEX_OFFSET VEH_ID_OFFSET

#define CIT_HOME_OFFSET 0x20
#define CIT_AGE_OFFSET 0xD4
#define CIT_POS_OFFSET 0x620
#define CIT_PLACED_AGE 1.0f // a citizen in their first year may not be placed yet

#define ENTRY_SIZE 0x10        // {Resource*, float amount, float}
#define ENTRY_AMOUNT_OFFSET 0x08
#define MAX_OBJECTS 2000000    // sanity bound on any container
#define MAX_ENTRIES 4096
#define MAX_NAME 64

void StateHash_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// FNV-1a, 64 bit.
static inline void Mix(unsigned long long& h, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
}
static inline void MixInt(unsigned long long& h, int v) { Mix(h, &v, sizeof(v)); }
static inline void MixFloat(unsigned long long& h, float v) { Mix(h, &v, sizeof(v)); }

static inline void MixName(unsigned long long& h, const char* s)
{
    uintptr_t addr = (uintptr_t)s;
    if (addr < 0x10000ull || addr >= 0x800000000000ull) return;
    for (int i = 0; i < MAX_NAME && s[i]; i++) Mix(h, &s[i], 1);
    Mix(h, "", 1);
}

static inline long long Count(BYTE* vec, long long elemSize)
{
    BYTE* b = *(BYTE**)vec;
    BYTE* e = *(BYTE**)(vec + 8);
    if (!b || e < b) return 0;
    long long n = (e - b) / elemSize;
    return n > MAX_OBJECTS ? 0 : n;
}

static inline unsigned long long Mix64Finalizer(unsigned long long x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Resource entries {Resource*, amount, x} of one vector: order-independent,
// zero-slot-invariant, quantized names and amounts (eliminates -0.0f / 1 ULP noise).
static void MixEntries(unsigned long long& h, BYTE* vec, double& total)
{
    long long n = Count(vec, ENTRY_SIZE);
    if (n > MAX_ENTRIES) n = MAX_ENTRIES;
    BYTE* e = *(BYTE**)vec;
    int activeCount = 0;
    unsigned long long accSum = 0;
    unsigned long long accXor = 0;
    for (long long i = 0; i < n; i++, e += ENTRY_SIZE) {
        const char* res = *(const char**)e;
        float amount = *(float*)(e + ENTRY_AMOUNT_OFFSET);
        if (!isfinite(amount)) continue;
        total += amount;
        if (fabsf(amount) <= 1e-4f) continue;
        double scaled = (double)amount * 10000.0;
        if (scaled > 9.0e15) scaled = 9.0e15;
        else if (scaled < -9.0e15) scaled = -9.0e15;
        long long qAmt = llround(scaled);
        if (qAmt == 0) continue;
        unsigned long long eh = 0xcbf29ce484222325ull;
        if (res) MixName(eh, res);
        Mix(eh, &qAmt, sizeof(qAmt));
        eh = Mix64Finalizer(eh);
        accSum += eh;
        accXor ^= eh;
        ++activeCount;
    }
    MixInt(h, activeCount);
    Mix(h, &accSum, sizeof(accSum));
    Mix(h, &accXor, sizeof(accXor));
}

static void HashConstructions(BYTE* w, StateSummary* out)
{
    long long nc = Count(w + WORLD_CONSTRUCTIONS_OFFSET, 8);
    out->constructions = (int)nc;
    if (nc <= 0) return;
    BYTE** cl = *(BYTE***)(w + WORLD_CONSTRUCTIONS_OFFSET);
    if (!cl) return;
    long long limit = (nc > 1024) ? 1024 : nc;
    for (long long i = 0; i < limit; i++) {
        BYTE* b = cl[i];
        if (!b) { MixInt(out->hashConstructions, -1); continue; }
        MixInt(out->hashConstructions, *(int*)(b + BLD_INDEX_OFFSET));
        MixFloat(out->hashConstructions, *(float*)(b + BLD_PROGRESS_OFFSET));
        MixInt(out->hashConstructions, (int)Count(b + BLD_WORKERS_OFFSET, 8));
        BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
        long long ns = Count(b + BLD_STORAGE_OFFSET, BLD_STORAGE_SLOT_SIZE);
        if (ns > 64) ns = 64;
        for (long long s = 0; s < ns; s++) {
            double dummy = 0;
            MixEntries(out->hashConstructions, slots + s * BLD_STORAGE_SLOT_SIZE, dummy);
        }
    }
}

static void HashNetworks(BYTE* w, StateSummary* out)
{
    static const unsigned int kNetworks[14] = {
        0xF840, 0xF848, 0xF8A0, 0xF8F8, 0xF950, 0xF9A8, 0xFA08,
        0xFA60, 0xFAB8, 0xFAC0, 0xFAD0, 0xFAE0, 0xFB38, 0xFB90
    };
    for (unsigned int off : kNetworks) {
        BYTE* mgr = *(BYTE**)(w + off);
        if (!mgr) { MixInt(out->hashNetworks, -1); continue; }
        long long nNodes = Count(mgr + NET_NODES_OFFSET, 8);
        long long nSegs  = Count(mgr + MGR_SEGMENTS_BEGIN_OFFSET, 8);
        MixInt(out->hashNetworks, (int)nNodes);
        MixInt(out->hashNetworks, (int)nSegs);
        if (nSegs > 0) {
            BYTE** segList = *(BYTE***)(mgr + MGR_SEGMENTS_BEGIN_OFFSET);
            if (segList) {
                long long limit = (nSegs > 256) ? 256 : nSegs;
                for (long long s = 0; s < limit; s++) {
                    BYTE* seg = segList[s];
                    if (!seg) { MixInt(out->hashNetworks, -1); continue; }
                    MixInt(out->hashNetworks, *(int*)(seg + SEG_TYPE_OFFSET));
                    MixInt(out->hashNetworks, *(int*)(seg + SEG_DIRECTION_OFFSET));
                }
            }
        }
    }
}

static void HashLines(BYTE* w, StateSummary* out)
{
    BYTE* linesMgr = *(BYTE**)(w + WORLD_LINES_OFFSET);
    if (!linesMgr) { MixInt(out->hashLines, -1); return; }
    int total = 0;
    for (unsigned k = 0; k < 2; k++) {
        BYTE* vec = linesMgr + k * 0x18;
        long long nl = Count(vec, 8);
        total += (int)nl;
        MixInt(out->hashLines, (int)nl);
        BYTE** ll = *(BYTE***)vec;
        if (ll && nl > 0) {
            long long limit = (nl > 256) ? 256 : nl;
            for (long long i = 0; i < limit; i++) {
                BYTE* l = ll[i];
                if (!l) { MixInt(out->hashLines, -1); continue; }
                MixInt(out->hashLines, *(int*)(l + 4));
                MixInt(out->hashLines, *(int*)(l + 8));
            }
        }
    }
    out->lines = total;
}

static void HashLoans(BYTE* w, StateSummary* out)
{
    SovietLoan* b = *(SovietLoan**)(w + WORLD_LOANS_OFFSET);
    SovietLoan* e = *(SovietLoan**)(w + WORLD_LOANS_OFFSET + 8);
    if (!b || e < b) { out->loans = 0; return; }
    uintptr_t diff = (uintptr_t)e - (uintptr_t)b;
    if ((diff % sizeof(SovietLoan)) != 0) { out->loans = 0; return; }
    long long count = (long long)(diff / sizeof(SovietLoan));
    if (count < 0) count = 0;
    out->loans = (int)count;
    MixInt(out->hashLoans, (int)count);
    long long limit = (count > 256) ? 256 : count;
    for (long long i = 0; i < limit; i++) {
        Mix(out->hashLoans, &b[i], sizeof(SovietLoan));
    }
}

static void HashResearch(BYTE* w, StateSummary* out)
{
    if (TwoStates_HashEngineResearch(&out->hashResearch)) {
        return;
    }
    Mix(out->hashResearch, w + WORLD_RESEARCH_UNLOCKS_OFFSET, WORLD_RESEARCH_UNLOCKS_BYTES);
    Mix(out->hashResearch, w + WORLD_SPACE_CARGO_WEST_UNLOCK_OFF, 2u);
    long long n = Count(w + WORLD_RESEARCH_VEC_BEGIN_OFFSET, RESEARCH_ITEM_STRIDE);
    if (n < 0) n = 0;
    long long limit = (n > 256) ? 256 : n;
    MixInt(out->hashResearch, (int)limit);
    if (limit > 0) {
        BYTE* items = *(BYTE**)(w + WORLD_RESEARCH_VEC_BEGIN_OFFSET);
        if (items) {
            for (long long i = 0; i < limit; i++) {
                BYTE* it = items + i * RESEARCH_ITEM_STRIDE;
                Mix(out->hashResearch, it + RESEARCH_ITEM_AVAILABLE_OFFSET, 1);
                MixFloat(out->hashResearch, *(float*)(it + RESEARCH_ITEM_PROGRESS_OFFSET));
            }
        }
    }
}

static void HashBalances(BYTE* w, StateSummary* out)
{
    (void)w;
    if (TwoStates_IsActive()) {
        out->rubState[0] = TwoStates_GetRubles(1);
        out->rubState[1] = TwoStates_GetRubles(2);
        out->usdState[0] = TwoStates_GetDollars(1);
        out->usdState[1] = TwoStates_GetDollars(2);
    } else {
        out->rubState[0] = out->rub;
        out->rubState[1] = 0.0;
        out->usdState[0] = out->usd;
        out->usdState[1] = 0.0;
    }
}

static void MixQuantizedFloat(unsigned long long& h, float v)
{
    if (!isfinite(v) || fabsf(v) <= 1e-6f) {
        long long q = 0;
        Mix(h, &q, sizeof(q));
        return;
    }
    double scaled = (double)v * 10000.0;
    if (scaled > 9.0e15) scaled = 9.0e15;
    else if (scaled < -9.0e15) scaled = -9.0e15;
    long long q = llround(scaled);
    Mix(h, &q, sizeof(q));
}

static bool HasActiveSpaceVehicleState(const BYTE* v)
{
    if (!v || !IsPointerLikeX64((uint64_t)(uintptr_t)v)) return false;
    int flightState = *(const int*)(v + VEH_ROCKET_FLIGHT_STATE_OFFSET);
    const BYTE* dockPartner = *(const BYTE* const*)(v + VEH_SPACE_DOCK_PARTNER_OFFSET);
    if (flightState != 0 || (dockPartner != nullptr && IsPointerLikeX64((uint64_t)(uintptr_t)dockPartner))) return true;
    int orbitIdx = *(const int*)(v + VEH_ORBIT_INDEX_OFFSET);
    int targetOrbitIdx = *(const int*)(v + VEH_TARGET_ORBIT_INDEX_OFFSET);
    float flightProgress = *(const float*)(v + VEH_ROCKET_PROGRESS_OFFSET);
    const BYTE* vt = *(const BYTE* const*)(v + VEH_TYPE_PTR_OFFSET);
    // CreateVehicle (0x403FBD) sets +0x1A68 = 1.0f for all vehicles; active space transfers count down inside (0, 1).
    const bool activeProgress = isfinite(flightProgress) && flightProgress > 1e-6f && flightProgress < (1.0f - 1e-5f);
    if (Ref_IsSpaceVehicleType((U64)(uintptr_t)vt) &&
        (orbitIdx >= 0 || targetOrbitIdx != -1 || activeProgress)) {
        return true;
    }
    return false;
}

static void HashOrbits(BYTE* w, unsigned long long& h)
{
    MixQuantizedFloat(h, *(const float*)(w + WORLD_ORBIT_CLOCK_OFFSET));
    const BYTE* b = *(const BYTE* const*)(w + WORLD_ORBITS_VEC_OFFSET);
    const BYTE* e = *(const BYTE* const*)(w + WORLD_ORBITS_VEC_OFFSET + 8);
    if (!b || e < b || !IsPointerLikeX64((uint64_t)(uintptr_t)b)) {
        MixInt(h, 0);
        return;
    }
    uintptr_t diff = (uintptr_t)e - (uintptr_t)b;
    if ((diff % ORBIT_SLOT_STRIDE) != 0) {
        MixInt(h, 0);
        return;
    }
    long long nOrbits = (long long)(diff / ORBIT_SLOT_STRIDE);
    if (nOrbits > 256) nOrbits = 256;
    MixInt(h, (int)nOrbits);
    for (long long i = 0; i < nOrbits; ++i) {
        const BYTE* slot = b + i * ORBIT_SLOT_STRIDE;
        for (unsigned off = 0; off < ORBIT_SLOT_STRIDE; off += 4u) {
            if (off == ORBIT_SLOT_TYPE_OFFSET) {
                MixInt(h, *(const int*)(slot + off));
            } else {
                MixQuantizedFloat(h, *(const float*)(slot + off));
            }
        }
    }
    TwoStates_HashOrbitOwners(&h);
}

static void HashSpaceVehicles(BYTE* w, unsigned long long& h)
{
    long long nv = Count(w + WORLD_VEHICLES_OFFSET, 8);
    BYTE** vl = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
    int activeCount = 0;
    if (vl && nv > 0) {
        for (long long i = 0; i < nv; ++i) {
            const BYTE* v = vl[i];
            if (!HasActiveSpaceVehicleState(v)) continue;
            const BYTE* dockPartner = *(const BYTE* const*)(v + VEH_SPACE_DOCK_PARTNER_OFFSET);
            int dockPartnerId = (dockPartner && IsPointerLikeX64((uint64_t)(uintptr_t)dockPartner))
                                    ? *(const int*)(dockPartner + VEH_INDEX_OFFSET)
                                    : -1;
            MixInt(h, *(const int*)(v + VEH_INDEX_OFFSET));
            MixInt(h, *(const int*)(v + VEH_ROCKET_FLIGHT_STATE_OFFSET));
            MixInt(h, *(const int*)(v + VEH_ORBIT_INDEX_OFFSET));
            MixInt(h, *(const int*)(v + VEH_TARGET_ORBIT_INDEX_OFFSET));
            MixInt(h, dockPartnerId);
            MixInt(h, *(const int*)(v + VEH_SPACE_DOCK_SLOT_OFFSET));
            MixQuantizedFloat(h, *(const float*)(v + VEH_ROCKET_PROGRESS_OFFSET));
            ++activeCount;
        }
    }
    MixInt(h, activeCount);
}

static void HashVehicleTypeMilestones(BYTE* w, unsigned long long& h)
{
    if (TwoStates_HashSpaceMilestones(&h)) return;
    const BYTE* vtBeg = *(const BYTE* const*)(w + WORLD_VEHICLE_TYPES_OFFSET);
    const BYTE* vtEnd = *(const BYTE* const*)(w + WORLD_VEHICLE_TYPES_OFFSET + 8);
    int activeCount = 0;
    unsigned long long accSum = 0;
    unsigned long long accXor = 0;
    if (vtBeg && vtEnd >= vtBeg && IsPointerLikeX64((uint64_t)(uintptr_t)vtBeg)) {
        uintptr_t diff = (uintptr_t)vtEnd - (uintptr_t)vtBeg;
        if ((diff % WORLD_VEHICLE_TYPE_STRIDE) == 0) {
            size_t nTypes = (size_t)(diff / WORLD_VEHICLE_TYPE_STRIDE);
            if (nTypes > 4096u) nTypes = 4096u;
            for (size_t i = 0; i < nTypes; ++i) {
                const BYTE* t = vtBeg + i * WORLD_VEHICLE_TYPE_STRIDE;
                int built = *(const int*)(t + VEH_TYPE_BUILT_IN_PROD_OFFSET);
                int orbit = *(const int*)(t + VEH_TYPE_SPACE_ORBIT_OFFSET);
                int reentry = *(const int*)(t + VEH_TYPE_SPACE_REENTRY_OFFSET);
                if (built == 0 && orbit == 0 && reentry == 0) continue;
                char normName[128] = {};
                Ref_ReadVehicleTypeScriptName((U64)t, normName, sizeof(normName));
                unsigned long long eh = 0xcbf29ce484222325ull;
                MixName(eh, normName);
                MixInt(eh, built);
                MixInt(eh, orbit);
                MixInt(eh, reentry);
                eh = Mix64Finalizer(eh);
                accSum += eh;
                accXor ^= eh;
                ++activeCount;
            }
        }
    }
    MixInt(h, activeCount);
    Mix(h, &accSum, sizeof(accSum));
    Mix(h, &accXor, sizeof(accXor));
}

static void HashSpace(BYTE* w, StateSummary* out)
{
    HashOrbits(w, out->hashSpace);
    HashSpaceVehicles(w, out->hashSpace);
    HashVehicleTypeMilestones(w, out->hashSpace);
}

bool StateHash_Compute(StateSummary* out)
{
    memset(out, 0, sizeof(*out));
    const unsigned long long seed = 0xcbf29ce484222325ull;
    out->hashClock = out->hashBuildings = out->hashConstructions = out->hashVehicles =
        out->hashVehiclesPos = out->hashCitizens = out->hashNetworks = out->hashLines =
        out->hashLoans = out->hashResearch = out->hashSpace = seed;
    BYTE* w = g_exeBase + WORLD_RVA;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        out->day = *(int*)(w + WORLD_DAY_OFFSET);
        out->year = *(int*)(w + WORLD_YEAR_OFFSET);
        out->dayProgress = *(float*)(w + WORLD_PROGRESS_OFFSET);
        out->usd = *(double*)(w + WORLD_USD_OFFSET);
        out->rub = *(double*)(w + WORLD_RUB_OFFSET);
        MixInt(out->hashClock, out->day);
        MixInt(out->hashClock, out->year);
        MixFloat(out->hashClock, out->dayProgress);

        HashBalances(w, out);

        BYTE* bvec = w + WORLD_BUILDINGS_OFFSET;
        long long nb = Count(bvec, 8);
        if (nb > 0 || *(void**)bvec != nullptr) {
            out->buildings = (int)nb;
            BYTE** bl = *(BYTE***)bvec;
            for (long long i = 0; i < nb; i++) {
                BYTE* b = bl[i];
                if (!b) { MixInt(out->hashBuildings, -1); continue; }
                MixInt(out->hashBuildings, *(int*)(b + BLD_INDEX_OFFSET));
                MixFloat(out->hashBuildings, *(float*)(b + BLD_PROGRESS_OFFSET));
                float produced = *(float*)(b + BLD_PRODUCED_OFFSET);
                MixFloat(out->hashBuildings, produced);
                out->productionTotal += produced;
                MixInt(out->hashBuildings, (int)Count(b + BLD_WORKERS_OFFSET, 8));
                BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
                long long ns = Count(b + BLD_STORAGE_OFFSET, BLD_STORAGE_SLOT_SIZE);
                if (ns > 64) ns = 64;
                for (long long s = 0; s < ns; s++)
                    MixEntries(out->hashBuildings, slots + s * BLD_STORAGE_SLOT_SIZE, out->storageTotal);
            }

            HashConstructions(w, out);

            long long nv = Count(w + WORLD_VEHICLES_OFFSET, 8);
            out->vehicles = (int)nv;
            BYTE** vl = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
            for (long long i = 0; i < nv; i++) {
                BYTE* v = vl[i];
                if (!v) {
                    MixInt(out->hashVehicles, -1);
                    MixInt(out->hashVehiclesPos, -1);
                    continue;
                }
                int vid = *(int*)(v + VEH_INDEX_OFFSET);
                MixInt(out->hashVehicles, vid);
                MixInt(out->hashVehicles, *(int*)(v + VEH_SCHED_IDX_OFFSET));
                MixEntries(out->hashVehicles, v + VEH_CARGO_OFFSET, out->cargoTotal);

                MixInt(out->hashVehiclesPos, vid);
                Mix(out->hashVehiclesPos, v + VEH_POS_OFFSET, 12);
            }

            long long nc = Count(w + WORLD_CITIZENS_OFFSET, 8);
            out->citizens = (int)nc;
            BYTE** cl = *(BYTE***)(w + WORLD_CITIZENS_OFFSET);
            for (long long i = 0; i < nc; i++) {
                BYTE* c = cl[i];
                if (!c) { MixInt(out->hashCitizens, -1); continue; }
                BYTE* home = *(BYTE**)(c + CIT_HOME_OFFSET);
                MixInt(out->hashCitizens, home ? *(int*)(home + BLD_INDEX_OFFSET) : -1);
                float age = *(float*)(c + CIT_AGE_OFFSET);
                MixFloat(out->hashCitizens, age);
                out->ageTotal += age;
            }

            HashNetworks(w, out);
            HashLines(w, out);
            HashLoans(w, out);
            HashResearch(w, out);
            HashSpace(w, out);

            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

// ---------------------------------------------------------------------------
// Per-object records (probe)
// ---------------------------------------------------------------------------

#define BLD_TYPE_OFFSET 0x318
#define BLD_POS_OFFSET 0x3A0
#define TYPE_CATEGORY_OFFSET 0x368

static long long ListCount(int listOffset)
{
    long long res = -1;
    DiagLog_BeginSafeSeh();
    __try {
        res = Count(g_exeBase + WORLD_RVA + listOffset, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        res = -1;
    }
    DiagLog_EndSafeSeh();
    return res;
}

static bool FillBuildings(BuildingRecord* out, long long n)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** bl = *(BYTE***)(g_exeBase + WORLD_RVA + WORLD_BUILDINGS_OFFSET);
        for (long long i = 0; i < n; i++) {
            BuildingRecord& r = out[i];
            memset(&r, 0, sizeof(r));
            BYTE* b = bl[i];
            if (!b) { r.index = -1; r.category = -2; continue; }
            r.index = *(int*)(b + BLD_INDEX_OFFSET);
            BYTE* type = *(BYTE**)(b + BLD_TYPE_OFFSET);
            r.category = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
            memcpy(r.pos, b + BLD_POS_OFFSET, 12);
            r.progress = *(float*)(b + BLD_PROGRESS_OFFSET);
            r.produced = *(float*)(b + BLD_PRODUCED_OFFSET);
            r.workers = (int)Count(b + BLD_WORKERS_OFFSET, 8);
            unsigned long long h = 0xcbf29ce484222325ull;
            double unused = 0;
            BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
            long long ns = Count(b + BLD_STORAGE_OFFSET, BLD_STORAGE_SLOT_SIZE);
            if (ns > 64) ns = 64;
            for (long long s = 0; s < ns; s++) {
                double amount = 0;
                MixEntries(h, slots + s * BLD_STORAGE_SLOT_SIZE, amount);
                if (s < RECORD_SLOTS) {
                    r.slotEntries[s] = (int)Count(slots + s * BLD_STORAGE_SLOT_SIZE, ENTRY_SIZE);
                    r.slotAmount[s] = (float)amount;
                }
            }
            r.slots = (int)ns;
            r.storage = h;
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool FillCitizens(CitizenRecord* out, long long n)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** cl = *(BYTE***)(g_exeBase + WORLD_RVA + WORLD_CITIZENS_OFFSET);
        for (long long i = 0; i < n; i++) {
            CitizenRecord& r = out[i];
            memset(&r, 0, sizeof(r));
            BYTE* c = cl[i];
            if (!c) { r.home = -2; continue; }
            BYTE* home = *(BYTE**)(c + CIT_HOME_OFFSET);
            r.home = home ? *(int*)(home + BLD_INDEX_OFFSET) : -1;
            r.age = *(float*)(c + CIT_AGE_OFFSET);
            if (home && r.age >= CIT_PLACED_AGE) memcpy(r.pos, c + CIT_POS_OFFSET, 12); // not placed yet: left 0
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool FillVehicles(VehicleRecord* out, long long n)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** vl = *(BYTE***)(g_exeBase + WORLD_RVA + WORLD_VEHICLES_OFFSET);
        for (long long i = 0; i < n; i++) {
            VehicleRecord& r = out[i];
            memset(&r, 0, sizeof(r));
            BYTE* v = vl[i];
            if (!v) { r.index = -1; continue; }
            r.index = *(int*)(v + VEH_INDEX_OFFSET);
            memcpy(r.pos, v + VEH_POS_OFFSET, 12);
            unsigned long long h = 0xcbf29ce484222325ull;
            double unused = 0;
            MixEntries(h, v + VEH_CARGO_OFFSET, unused);
            r.cargo = h;
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

bool StateHash_VehicleRecords(std::vector<VehicleRecord>& out)
{
    long long n = ListCount(WORLD_VEHICLES_OFFSET);
    if (n < 0) return false;
    out.resize((size_t)n);
    return n == 0 || FillVehicles(out.data(), n);
}

bool StateHash_BuildingRecords(std::vector<BuildingRecord>& out)
{
    long long n = ListCount(WORLD_BUILDINGS_OFFSET);
    if (n < 0) return false;
    out.resize((size_t)n);
    return n == 0 || FillBuildings(out.data(), n);
}

bool StateHash_CitizenRecords(std::vector<CitizenRecord>& out)
{
    long long n = ListCount(WORLD_CITIZENS_OFFSET);
    if (n < 0) return false;
    out.resize((size_t)n);
    return n == 0 || FillCitizens(out.data(), n);
}

struct DesyncVehInfo {
    unsigned id;
    int sched;
    int curBld;
    int bayBld;
    long long cargoCount;
    unsigned long long cargoHash;
    double cargoSum;
    float pos[3];
    char cargoDetail[128];
};

struct DesyncBldInfo {
    int id;
    float progress;
    float produced;
    long long workers;
    unsigned long long storageHash;
    double storageSum;
};

static int SafeReadBldIndexPtr(const BYTE* ptr)
{
    return IsPointerLikeX64((uint64_t)(uintptr_t)ptr) ? *(const int*)(ptr + BLD_INDEX_OFFSET) : -1;
}

static void FormatCargoDetail(BYTE* vec, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    long long n = Count(vec, ENTRY_SIZE);
    if (n <= 0) return;
    if (n > MAX_ENTRIES) n = MAX_ENTRIES;
    BYTE* e = *(BYTE**)vec;
    if (!e) return;
    size_t pos = 0;
    int shown = 0;
    for (long long i = 0; i < n && shown < 6; i++, e += ENTRY_SIZE) {
        if (pos + 8 >= outSize) break;
        const char* res = *(const char**)e;
        float amount = *(float*)(e + ENTRY_AMOUNT_OFFSET);
        char nameBuf[24] = "?";
        uintptr_t addr = (uintptr_t)res;
        if (addr >= 0x10000ull && addr < 0x800000000000ull) {
            size_t len = 0;
            while (len < 20 && res[len]) {
                unsigned char ch = (unsigned char)res[len];
                nameBuf[len] = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : '?';
                ++len;
            }
            nameBuf[len] = '\0';
            if (len == 0) strcpy_s(nameBuf, "?");
        }
        float safeAmt = isfinite(amount) ? amount : 0.0f;
        int written = _snprintf_s(out + pos, outSize - pos, _TRUNCATE, "%s%s=%.3f",
                                  shown > 0 ? "," : "", nameBuf, safeAmt);
        if (written <= 0) break;
        pos += (size_t)written;
        ++shown;
    }
}

static bool ReadDesyncVehRow(BYTE* w, long long idx, DesyncVehInfo* out)
{
    memset(out, 0, sizeof(*out));
    out->curBld = -1;
    out->bayBld = -1;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** vl = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
        BYTE* v = vl ? vl[idx] : nullptr;
        if (v && IsPointerLikeX64((uint64_t)(uintptr_t)v)) {
            out->id = *(const unsigned*)(v + VEH_INDEX_OFFSET);
            out->sched = *(const int*)(v + VEH_SCHED_IDX_OFFSET);
            out->curBld = SafeReadBldIndexPtr(*(const BYTE* const*)(v + 0x4F0));
            out->bayBld = SafeReadBldIndexPtr(*(const BYTE* const*)(v + 0x528));
            out->cargoCount = Count(v + VEH_CARGO_OFFSET, ENTRY_SIZE);
            out->cargoHash = 0xcbf29ce484222325ull;
            MixEntries(out->cargoHash, v + VEH_CARGO_OFFSET, out->cargoSum);
            memcpy(out->pos, v + VEH_POS_OFFSET, sizeof(out->pos));
            FormatCargoDetail(v + VEH_CARGO_OFFSET, out->cargoDetail, sizeof(out->cargoDetail));
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool ReadDesyncBldRow(BYTE* w, int listOff, long long idx, DesyncBldInfo* out)
{
    memset(out, 0, sizeof(*out));
    out->id = -1;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** bl = *(BYTE***)(w + listOff);
        BYTE* b = bl ? bl[idx] : nullptr;
        if (b && IsPointerLikeX64((uint64_t)(uintptr_t)b)) {
            out->id = *(const int*)(b + BLD_INDEX_OFFSET);
            out->progress = *(const float*)(b + BLD_PROGRESS_OFFSET);
            out->produced = *(const float*)(b + BLD_PRODUCED_OFFSET);
            out->workers = Count(b + BLD_WORKERS_OFFSET, 8);
            out->storageHash = 0xcbf29ce484222325ull;
            BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
            long long ns = Count(b + BLD_STORAGE_OFFSET, BLD_STORAGE_SLOT_SIZE);
            if (ns > 64) ns = 64;
            for (long long s = 0; s < ns; s++) {
                MixEntries(out->storageHash, slots + s * BLD_STORAGE_SLOT_SIZE, out->storageSum);
            }
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void LogDesyncVehicles(BYTE* w, unsigned long long tick)
{
    long long nv = ListCount(WORLD_VEHICLES_OFFSET);
    if (nv <= 0) return;
    long long limit = (nv > 256) ? 256 : nv;
    for (long long i = 0; i < limit; i++) {
        DesyncVehInfo info = {};
        if (!ReadDesyncVehRow(w, i, &info)) continue;
        Logf("republicmp  DESYNC_DUMP[veh] t=%llu i=%lld id=%u sched=%d bld=(%d,%d) cargoN=%lld cargoH=%016llX cargoSum=%.3f pos=(%.2f,%.2f,%.2f) cargo=[%s]",
             tick, i, info.id, info.sched, info.curBld, info.bayBld, info.cargoCount,
             info.cargoHash, info.cargoSum, info.pos[0], info.pos[1], info.pos[2], info.cargoDetail);
    }
}

static void LogDesyncBuildingsOrConstructions(BYTE* w, int listOff, const char* tag,
                                              long long cap, bool onlyActive, unsigned long long tick)
{
    long long n = ListCount(listOff);
    if (n <= 0) return;
    long long limit = (n > cap) ? cap : n;
    const unsigned long long seed = 0xcbf29ce484222325ull;
    for (long long i = 0; i < limit; i++) {
        DesyncBldInfo info = {};
        if (!ReadDesyncBldRow(w, listOff, i, &info)) continue;
        if (onlyActive && info.storageHash == seed && info.progress >= 0.9999f && info.produced == 0.0f) {
            continue;
        }
        Logf("republicmp  DESYNC_DUMP[%s] t=%llu i=%lld id=%d prog=%.4f prod=%.2f wrk=%lld storH=%016llX storSum=%.3f",
             tag, tick, i, info.id, info.progress, info.produced, info.workers,
             info.storageHash, info.storageSum);
    }
}

static void LogDesyncSpace(BYTE* w, unsigned long long tick)
{
    float orbitClock = 0.0f;
    long long nOrbits = 0;
    const BYTE* ob = nullptr;
    const BYTE* oe = nullptr;
    DiagLog_BeginSafeSeh();
    __try {
        orbitClock = *(const float*)(w + WORLD_ORBIT_CLOCK_OFFSET);
        ob = *(const BYTE* const*)(w + WORLD_ORBITS_VEC_OFFSET);
        oe = *(const BYTE* const*)(w + WORLD_ORBITS_VEC_OFFSET + 8);
        if (ob && oe >= ob && ((uintptr_t)(oe - ob) % ORBIT_SLOT_STRIDE) == 0) {
            nOrbits = (long long)((oe - ob) / ORBIT_SLOT_STRIDE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nOrbits = 0;
    }
    DiagLog_EndSafeSeh();
    Logf("republicmp  DESYNC_DUMP[space] t=%llu orbitClock=%.4f orbitCount=%lld", tick, orbitClock, nOrbits);

    long long orbLimit = (nOrbits > 16) ? 16 : nOrbits;
    for (long long i = 0; i < orbLimit; ++i) {
        int orbType = 0;
        float f0 = 0.0f, startClock = 0.0f;
        DiagLog_BeginSafeSeh();
        __try {
            const BYTE* slot = ob + i * ORBIT_SLOT_STRIDE;
            f0 = *(const float*)slot;
            orbType = *(const int*)(slot + ORBIT_SLOT_TYPE_OFFSET);
            startClock = *(const float*)(slot + ORBIT_SLOT_START_CLOCK_OFFSET);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
        Logf("republicmp  DESYNC_DUMP[orbit] t=%llu i=%lld type=%d f0=%.3f start=%.3f",
             tick, i, orbType, f0, startClock);
    }

    long long nv = ListCount(WORLD_VEHICLES_OFFSET);
    long long vehLimit = (nv > 256) ? 256 : nv;
    for (long long i = 0; i < vehLimit; ++i) {
        bool active = false;
        unsigned id = 0;
        int orbIdx = -1, tgtIdx = -1, flState = 0, dockSlot = -1;
        float prog = 0.0f;
        DiagLog_BeginSafeSeh();
        __try {
            BYTE** vl = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
            BYTE* v = vl ? vl[i] : nullptr;
            if (v && IsPointerLikeX64((uint64_t)(uintptr_t)v) && HasActiveSpaceVehicleState(v)) {
                active = true;
                id = *(const unsigned*)(v + VEH_INDEX_OFFSET);
                flState = *(const int*)(v + VEH_ROCKET_FLIGHT_STATE_OFFSET);
                orbIdx = *(const int*)(v + VEH_ORBIT_INDEX_OFFSET);
                tgtIdx = *(const int*)(v + VEH_TARGET_ORBIT_INDEX_OFFSET);
                dockSlot = *(const int*)(v + VEH_SPACE_DOCK_SLOT_OFFSET);
                prog = *(const float*)(v + VEH_ROCKET_PROGRESS_OFFSET);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            active = false;
        }
        DiagLog_EndSafeSeh();
        if (active) {
            Logf("republicmp  DESYNC_DUMP[space_veh] t=%llu i=%lld id=%u state=%d orb=%d tgt=%d dock=%d prog=%.4f",
                 tick, i, id, flState, orbIdx, tgtIdx, dockSlot, prog);
        }
    }
    if (TwoStates_IsActive()) {
        TwoStates_LogSpaceDesyncDetails();
    }
}

void StateHash_LogDesyncBreakdown(const char* what, unsigned long long tick)
{
    if (!g_exeBase || !H || !H->log) return;
    BYTE* w = g_exeBase + WORLD_RVA;
    if (!what || strstr(what, "veh") != nullptr) {
        LogDesyncVehicles(w, tick);
    }
    if (what && (strstr(what, "bld") != nullptr || strstr(what, "buildings") != nullptr)) {
        LogDesyncBuildingsOrConstructions(w, WORLD_BUILDINGS_OFFSET, "bld", 256, true, tick);
    }
    if (what && strstr(what, "constructions") != nullptr) {
        LogDesyncBuildingsOrConstructions(w, WORLD_CONSTRUCTIONS_OFFSET, "con", 128, false, tick);
    }
    if (!what || strstr(what, "space") != nullptr) {
        LogDesyncSpace(w, tick);
    }
}

unsigned long long StateHash_All(const StateSummary& s)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    Mix(h, &s.hashClock, 8);
    Mix(h, &s.hashBuildings, 8);
    Mix(h, &s.hashConstructions, 8);
    Mix(h, &s.hashVehicles, 8);
    Mix(h, &s.hashVehiclesPos, 8);
    Mix(h, &s.hashCitizens, 8);
    Mix(h, &s.hashNetworks, 8);
    Mix(h, &s.hashLines, 8);
    Mix(h, &s.hashLoans, 8);
    Mix(h, &s.hashResearch, 8);
    Mix(h, &s.hashSpace, 8);
    return h;
}
