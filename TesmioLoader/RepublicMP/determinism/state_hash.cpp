// SPDX-License-Identifier: GPL-3.0-only
// See state_hash.h.
#include "state_hash.h"
#include "../session/two_states.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
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

// Resource entries {Resource*, amount, x} of one vector: names and amounts.
static void MixEntries(unsigned long long& h, BYTE* vec, double& total)
{
    long long n = Count(vec, ENTRY_SIZE);
    if (n > MAX_ENTRIES) n = MAX_ENTRIES;
    BYTE* e = *(BYTE**)vec;
    MixInt(h, (int)n);
    for (long long i = 0; i < n; i++, e += ENTRY_SIZE) {
        const char* res = *(const char**)e;
        if (res) MixName(h, res);
        float amount = *(float*)(e + ENTRY_AMOUNT_OFFSET);
        MixFloat(h, amount);
        total += amount;
    }
}

bool StateHash_Compute(StateSummary* out)
{
    memset(out, 0, sizeof(*out));
    const unsigned long long seed = 0xcbf29ce484222325ull;
    out->hashClock = out->hashBuildings = out->hashVehicles = out->hashVehiclesPos = out->hashCitizens = seed;
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
#define TYPE_CATEGORY_OFFSET 0x360

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

unsigned long long StateHash_All(const StateSummary& s)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    Mix(h, &s.hashClock, 8);
    Mix(h, &s.hashBuildings, 8);
    Mix(h, &s.hashVehicles, 8);
    Mix(h, &s.hashVehiclesPos, 8);
    Mix(h, &s.hashCitizens, 8);
    return h;
}
