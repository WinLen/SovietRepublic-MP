// SPDX-License-Identifier: GPL-3.0-only
// See vehicle_soft_sync.h.
#include "vehicle_soft_sync.h"
#include "../../lockstep/commands.h"
#include "../../lockstep/lockstep_net.h"
#include "../../determinism/held_frame.h"
#include "../../session/two_states.h"
#include "../../core/engine_rva.h"
#include "../../core/diag_log.h"
#include <string.h>
#include <math.h>

#define VEH_CUR_BLD_OFFSET 0x4F0
#define VEH_FUEL_OFFSET 0x770
#define VEH_SPEED_OFFSET 0xD24
#define VEH_NODE0_OFFSET 0xD60
#define VEH_WEAR0_OFFSET 0x17E8
#define VEH_WEAR1_OFFSET 0x1A54
#define VEH_WEAR2_OFFSET 0x1A7C

#define CARGO_ENTRY_SIZE 0x10
#define CARGO_AMOUNT_OFFSET 0x08

#define SOFT_SYNC_BATCH_MAX 20
#define SOFT_SYNC_BLD_BATCH_MAX 8
#define SOFT_SYNC_BLD_SLOTS_MAX 6
#define SOFT_SYNC_BLD_SLOT_ENTRIES_MAX 16
#define SOFT_SYNC_INTERVAL_TICKS 180 // every 6 seconds of game simulation
#define MAX_VEHICLES_SCAN 65536
#define MAX_BUILDINGS_SCAN 131072

#pragma pack(push, 1)
struct VehicleSoftSyncEntry {
    int place;
    unsigned vehicleId;
    int currentBldId;
    int scheduleIdx;
    float fuel;
    float speed;
    float wear[3];
    float pos[3];
    unsigned short cargoCount;
    unsigned short flags;
    unsigned cargoResHash[2];
    float cargoAmount[2];
    float passengers;
};

struct VehicleSoftSyncTreasury {
    unsigned magic; // 0x54525359 ('TRSY')
    unsigned flags; // bit 0: treasury valid, bit 1: TwoStates active
    double worldRub;
    double worldUsd;
    double stateRub[2];
    double stateUsd[2];
};

struct VehicleSoftSyncBldEntry {
    int place;
    int bldId;
    float progress;
    float produced;
    unsigned char slotCount;
    unsigned char slotEntryCount[SOFT_SYNC_BLD_SLOTS_MAX];
    unsigned char reserved;
    unsigned resHash[SOFT_SYNC_BLD_SLOTS_MAX][SOFT_SYNC_BLD_SLOT_ENTRIES_MAX];
    float amount[SOFT_SYNC_BLD_SLOTS_MAX][SOFT_SYNC_BLD_SLOT_ENTRIES_MAX];
};

struct VehicleSoftSyncBldBatch {
    unsigned magic; // 0x42535447 ('BSTG')
    unsigned count;
    VehicleSoftSyncBldEntry entries[SOFT_SYNC_BLD_BATCH_MAX];
};

struct VehicleSoftSyncMsg {
    unsigned count;
    VehicleSoftSyncEntry entries[SOFT_SYNC_BATCH_MAX];
};
#pragma pack(pop)

static_assert(sizeof(VehicleSoftSyncMsg) + sizeof(VehicleSoftSyncTreasury) + sizeof(VehicleSoftSyncBldBatch) <= 7900,
              "SoftSync wire packet must fit in LS_MAX_PAYLOAD");

#define VSS_TREASURY_MAGIC 0x54525359u
#define VSS_BLD_STORAGE_MAGIC 0x42535447u

static const unsigned kVehicleNodeOffsets[6] = { 0xD60, 0xE1C, 0xED8, 0xF94, 0x1080, VEH_SIXTH_NODE_OFFSET };

static volatile LONG g_immediateRequested = 0;
static int g_immediateSweepLeft = 0;
static size_t g_rollingCursor = 0;
static size_t g_bldRollingCursor = 0;
static unsigned long long g_lastImmediateTick = 0;
static VehicleSoftSyncDiagStats g_vssDiag = {};

void VehicleSoftSync_Bind(const TsmHost* host)
{
    TsmBind(host);
}

void VehicleSoftSync_Reset()
{
    InterlockedExchange(&g_immediateRequested, 0);
    g_immediateSweepLeft = 0;
    g_rollingCursor = 0;
    g_bldRollingCursor = 0;
    g_lastImmediateTick = 0;
    memset(&g_vssDiag, 0, sizeof(g_vssDiag));
}

void VehicleSoftSync_GetDiagStats(VehicleSoftSyncDiagStats* out)
{
    if (!out) return;
    *out = g_vssDiag;
}

void VehicleSoftSync_RequestImmediate()
{
    InterlockedExchange(&g_immediateRequested, 1);
}

static BYTE* ResolveVehicleDirectBld(const BYTE* v)
{
    if (!v) return nullptr;
    BYTE* b = *(BYTE* const*)(v + VEH_CUR_BLD_OFFSET);
    if (b) return b;
    return *(BYTE* const*)(v + VEH_STOPPED_BLD_OFFSET);
}

BYTE* VehicleSoftSync_ResolveEffectiveBld(const BYTE* v)
{
    BYTE* b = ResolveVehicleDirectBld(v);
    if (b || !v) return b;
    const BYTE* parent = *(const BYTE* const*)(v + VEH_LINK_PTR_OFFSET);
    if (!parent) parent = *(const BYTE* const*)(v + VEH_PARENT_PTR_OFFSET);
    if (parent && parent != v) return ResolveVehicleDirectBld(parent);
    return nullptr;
}

static unsigned HashResourceName(const char* s)
{
    uintptr_t addr = (uintptr_t)s;
    if (addr < 0x10000ull || addr >= 0x800000000000ull) return 0;
    unsigned h = 2166136261u;
    for (int i = 0; i < 64 && s[i]; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static const char* FindResourceByHash(const BYTE* w, unsigned resHash)
{
    if (!w || !resHash) return nullptr;
    const BYTE* resBegin = *(const BYTE* const*)(w + WORLD_RESOURCES_OFFSET);
    const BYTE* resEnd = *(const BYTE* const*)(w + WORLD_RESOURCES_OFFSET + 8);
    if (!resBegin || resEnd <= resBegin) return nullptr;
    size_t count = (size_t)((resEnd - resBegin) / WORLD_RESOURCE_STRIDE);
    if (count > 512) count = 512;
    for (size_t i = 0; i < count; i++) {
        const char* candidate = (const char*)(resBegin + i * WORLD_RESOURCE_STRIDE);
        if (HashResourceName(candidate) == resHash) return candidate;
    }
    return nullptr;
}

static void TranslateNode(BYTE* node, float dx, float dy, float dz)
{
    float* m = (float*)node;
    float* minv = (float*)(node + 0x40);
    float* pos = (float*)(node + 0x80);
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return;
    pos[0] += dx;
    pos[1] += dy;
    pos[2] += dz;
    m[12] += dx;
    m[13] += dy;
    m[14] += dz;
    minv[12] -= dx * minv[0] + dy * minv[4] + dz * minv[8];
    minv[13] -= dx * minv[1] + dy * minv[5] + dz * minv[9];
    minv[14] -= dx * minv[2] + dy * minv[6] + dz * minv[10];
}

static void TranslateVehicleNodes(BYTE* v, float dx, float dy, float dz)
{
    int nNodes = (*(const int32_t*)(v + VEH_SIXTH_NODE_ACTIVE_OFFSET) >= 0) ? 6 : 5;
    for (int k = 0; k < nNodes; k++) {
        TranslateNode(v + kVehicleNodeOffsets[k], dx, dy, dz);
    }
}

static float ReadVehiclePassengers(const BYTE* v)
{
    const BYTE* cBeg = *(const BYTE**)(v + 0x458);
    const BYTE* cEnd = *(const BYTE**)(v + 0x460);
    if (!cBeg || cEnd < cBeg || (cEnd - cBeg) % 16 != 0) return 0.0f;
    for (const BYTE* p = cBeg; p + 16 <= cEnd; p += 16) {
        const char* name = *(const char**)p;
        if (name && (strcmp(name, "passengers") == 0 || strcmp(name, "workers") == 0 ||
                     strcmp(name, "students") == 0 || strcmp(name, "tourists") == 0)) {
            float amt = *(const float*)(p + 8);
            if (isfinite(amt) && amt >= 0.0f) return amt;
        }
    }
    return 0.0f;
}

static void WriteVehiclePassengers(BYTE* v, float passengers)
{
    BYTE* cBeg = *(BYTE**)(v + 0x458);
    BYTE* cEnd = *(BYTE**)(v + 0x460);
    if (!cBeg || cEnd < cBeg || (cEnd - cBeg) % 16 != 0) return;
    for (BYTE* p = cBeg; p + 16 <= cEnd; p += 16) {
        const char* name = *(const char**)p;
        if (name && (strcmp(name, "passengers") == 0 || strcmp(name, "workers") == 0 ||
                     strcmp(name, "students") == 0 || strcmp(name, "tourists") == 0)) {
            *(float*)(p + 8) = passengers;
            return;
        }
    }
}

static bool FillEntrySafe(BYTE* v, int place, VehicleSoftSyncEntry& out)
{
    bool ok = false;
    __try {
        memset(&out, 0, sizeof(out));
        out.place = place;
        out.vehicleId = *(unsigned*)(v + VEH_ID_OFFSET);
        BYTE* effBld = VehicleSoftSync_ResolveEffectiveBld(v);
        out.currentBldId = effBld ? *(int*)effBld : -1;
        out.scheduleIdx = *(int*)(v + VEH_SCHED_IDX_OFFSET);
        out.fuel = *(float*)(v + VEH_FUEL_OFFSET);
        out.speed = *(float*)(v + VEH_SPEED_OFFSET);
        out.wear[0] = *(float*)(v + VEH_WEAR0_OFFSET);
        out.wear[1] = *(float*)(v + VEH_WEAR1_OFFSET);
        out.wear[2] = *(float*)(v + VEH_WEAR2_OFFSET);
        memcpy(out.pos, v + VEH_POS_OFFSET, 12);
        out.passengers = ReadVehiclePassengers(v);

        BYTE* cBegin = *(BYTE**)(v + VEH_CARGO_OFFSET);
        BYTE* cEnd = *(BYTE**)(v + VEH_CARGO_OFFSET + 8);
        if (cBegin && cEnd >= cBegin && (cEnd - cBegin) % CARGO_ENTRY_SIZE == 0) {
            size_t cn = (size_t)((cEnd - cBegin) / CARGO_ENTRY_SIZE);
            if (cn <= 256) {
                out.cargoCount = (unsigned short)cn;
                for (size_t k = 0; k < cn && k < 2; k++) {
                    BYTE* entry = cBegin + k * CARGO_ENTRY_SIZE;
                    const char* resName = *(const char**)entry;
                    out.cargoResHash[k] = HashResourceName(resName);
                    out.cargoAmount[k] = *(float*)(entry + CARGO_AMOUNT_OFFSET);
                }
            }
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

static bool FillBldEntrySafe(BYTE* b, int place, VehicleSoftSyncBldEntry& out)
{
    bool ok = false;
    __try {
        BYTE* sBeg = *(BYTE**)(b + BLD_STORAGE_OFFSET);
        BYTE* sEnd = *(BYTE**)(b + BLD_STORAGE_OFFSET + 8);
        float produced = *(float*)(b + BLD_PRODUCED_OFFSET);
        size_t ns = 0;
        if (sBeg && sEnd >= sBeg && (sEnd - sBeg) % BLD_STORAGE_SLOT_SIZE == 0) {
            ns = (size_t)((sEnd - sBeg) / BLD_STORAGE_SLOT_SIZE);
        }
        if (ns == 0 && produced == 0.0f) return false;
        if (ns > SOFT_SYNC_BLD_SLOTS_MAX) ns = SOFT_SYNC_BLD_SLOTS_MAX;

        memset(&out, 0, sizeof(out));
        out.place = place;
        out.bldId = *(int*)b;
        out.progress = *(float*)(b + BLD_PROGRESS_OFFSET);
        out.produced = produced;
        out.slotCount = (unsigned char)ns;
        for (size_t s = 0; s < ns; s++) {
            BYTE* slot = sBeg + s * BLD_STORAGE_SLOT_SIZE;
            BYTE* eBeg = *(BYTE**)slot;
            BYTE* eEnd = *(BYTE**)(slot + 8);
            if (!eBeg || eEnd < eBeg || (eEnd - eBeg) % CARGO_ENTRY_SIZE != 0) continue;
            size_t en = (size_t)((eEnd - eBeg) / CARGO_ENTRY_SIZE);
            if (en > SOFT_SYNC_BLD_SLOT_ENTRIES_MAX) en = SOFT_SYNC_BLD_SLOT_ENTRIES_MAX;
            out.slotEntryCount[s] = (unsigned char)en;
            for (size_t k = 0; k < en; k++) {
                BYTE* entry = eBeg + k * CARGO_ENTRY_SIZE;
                out.resHash[s][k] = HashResourceName(*(const char**)entry);
                out.amount[s][k] = *(float*)(entry + CARGO_AMOUNT_OFFSET);
            }
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

static void CaptureSoftSyncBuildings(const BYTE* w, VehicleSoftSyncBldBatch& bldBatch)
{
    memset(&bldBatch, 0, sizeof(bldBatch));
    if (!w) return;
    BYTE* const* begin = *(BYTE* const* const*)(w + WORLD_BUILDINGS_OFFSET);
    BYTE* const* end = *(BYTE* const* const*)(w + WORLD_BUILDINGS_OFFSET + 8);
    if (!begin || end <= begin) return;
    size_t total = (size_t)(end - begin);
    if (total == 0 || total > MAX_BUILDINGS_SCAN) return;
    if (g_bldRollingCursor >= total) g_bldRollingCursor = 0;
    size_t startCursor = g_bldRollingCursor;
    size_t scanned = 0;
    while (scanned < total && bldBatch.count < SOFT_SYNC_BLD_BATCH_MAX) {
        size_t idx = (startCursor + scanned) % total;
        scanned++;
        g_bldRollingCursor = (idx + 1) % total;
        BYTE* b = begin[idx];
        if (!b) continue;
        if (FillBldEntrySafe(b, (int)idx, bldBatch.entries[bldBatch.count])) {
            bldBatch.count++;
        }
    }
    if (bldBatch.count > 0) bldBatch.magic = VSS_BLD_STORAGE_MAGIC;
}

static void CaptureSoftSyncTreasury(const BYTE* w, VehicleSoftSyncTreasury& tr)
{
    memset(&tr, 0, sizeof(tr));
    if (!w || *(const int*)(w + WORLD_UNLIMITED_MONEY_OFFSET) != 0) return;
    tr.magic = VSS_TREASURY_MAGIC;
    tr.flags = 1u;
    tr.worldRub = *(const double*)(w + WORLD_RUB_OFFSET);
    tr.worldUsd = *(const double*)(w + WORLD_USD_OFFSET);
    if (TwoStates_IsActive()) {
        tr.flags |= 2u;
        tr.stateRub[0] = TwoStates_GetRubles(1);
        tr.stateRub[1] = TwoStates_GetRubles(2);
        tr.stateUsd[0] = TwoStates_GetDollars(1);
        tr.stateUsd[1] = TwoStates_GetDollars(2);
    }
}

static bool BuildBatch(VehicleSoftSyncMsg& msg, VehicleSoftSyncTreasury& tr,
                       VehicleSoftSyncBldBatch& bldBatch, bool immediate)
{
    msg.count = 0;
    memset(&tr, 0, sizeof(tr));
    memset(&bldBatch, 0, sizeof(bldBatch));
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* w = g_exeBase + WORLD_RVA;
        CaptureSoftSyncTreasury(w, tr);
        CaptureSoftSyncBuildings(w, bldBatch);
        BYTE** begin = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
        BYTE** end = *(BYTE***)(w + WORLD_VEHICLES_OFFSET + 8);
        if (begin && end > begin) {
            size_t total = (size_t)(end - begin);
            if (total <= MAX_VEHICLES_SCAN) {
                if (g_rollingCursor >= total) g_rollingCursor = 0;
                size_t startCursor = g_rollingCursor;

                // Pass 1: prioritize moving or out-of-building vehicles starting from g_rollingCursor.
                size_t scanned = 0;
                while (scanned < total && msg.count < SOFT_SYNC_BATCH_MAX) {
                    size_t idx = (startCursor + scanned) % total;
                    scanned++;
                    g_rollingCursor = (idx + 1) % total;
                    BYTE* v = begin[idx];
                    if (!v) continue;
                    float speed = *(float*)(v + VEH_SPEED_OFFSET);
                    BYTE* effBld = VehicleSoftSync_ResolveEffectiveBld(v);
                    if (speed == 0.0f && effBld != nullptr) continue;
                    if (FillEntrySafe(v, (int)idx, msg.entries[msg.count])) {
                        msg.count++;
                    }
                }

                // Pass 2: during immediate sweeps, fill any remaining batch capacity with parked/stopped vehicles.
                if (immediate && msg.count < SOFT_SYNC_BATCH_MAX) {
                    for (size_t p = 0; p < total && msg.count < SOFT_SYNC_BATCH_MAX; p++) {
                        size_t idx = (startCursor + p) % total;
                        BYTE* v = begin[idx];
                        if (!v) continue;
                        float speed = *(float*)(v + VEH_SPEED_OFFSET);
                        BYTE* effBld = VehicleSoftSync_ResolveEffectiveBld(v);
                        if (speed != 0.0f || effBld == nullptr) continue;
                        if (FillEntrySafe(v, (int)idx, msg.entries[msg.count])) {
                            msg.count++;
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        msg.count = 0;
        tr.magic = 0;
        bldBatch.magic = 0;
        bldBatch.count = 0;
    }
    DiagLog_EndSafeSeh();
    return msg.count > 0 || tr.magic == VSS_TREASURY_MAGIC || bldBatch.magic == VSS_BLD_STORAGE_MAGIC;
}

static size_t PackSoftSyncWire(const VehicleSoftSyncMsg& msg, const VehicleSoftSyncTreasury& tr,
                               const VehicleSoftSyncBldBatch& bldBatch, BYTE* wireBuf)
{
    size_t wireBytes = sizeof(unsigned) + msg.count * sizeof(VehicleSoftSyncEntry);
    memcpy(wireBuf, &msg, wireBytes);
    if (tr.magic == VSS_TREASURY_MAGIC) {
        memcpy(wireBuf + wireBytes, &tr, sizeof(tr));
        wireBytes += sizeof(tr);
    }
    if (bldBatch.magic == VSS_BLD_STORAGE_MAGIC && bldBatch.count > 0 && bldBatch.count <= SOFT_SYNC_BLD_BATCH_MAX) {
        size_t bldBytes = sizeof(unsigned) * 2 + bldBatch.count * sizeof(VehicleSoftSyncBldEntry);
        memcpy(wireBuf + wireBytes, &bldBatch, bldBytes);
        wireBytes += bldBytes;
    }
    return wireBytes;
}

bool VehicleSoftSync_BuildPacketForTest(void* outBuf, size_t maxBytes, size_t* outSize, bool immediate)
{
    if (outSize) *outSize = 0;
    if (!outBuf) return false;
    VehicleSoftSyncMsg msg = {};
    VehicleSoftSyncTreasury tr = {};
    VehicleSoftSyncBldBatch bldBatch = {};
    if (!BuildBatch(msg, tr, bldBatch, immediate)) return false;
    BYTE tmpBuf[sizeof(VehicleSoftSyncMsg) + sizeof(VehicleSoftSyncTreasury) + sizeof(VehicleSoftSyncBldBatch)];
    size_t wireBytes = PackSoftSyncWire(msg, tr, bldBatch, tmpBuf);
    if (wireBytes == 0 || wireBytes > maxBytes) return false;
    memcpy(outBuf, tmpBuf, wireBytes);
    if (outSize) *outSize = wireBytes;
    return true;
}

void VehicleSoftSync_HostTick(unsigned long long tick)
{
    if (!LockstepNet_IsConnected() || !LockstepNet_IsHost() || !Commands_Capturing()) return;

    if (tick >= g_lastImmediateTick + 12) {
        if (InterlockedCompareExchange(&g_immediateRequested, 0, 1) == 1) {
            g_immediateSweepLeft = 8;
            g_lastImmediateTick = tick;
            g_vssDiag.immediateSweeps++;
        }
    }

    bool immediate = false;
    if (g_immediateSweepLeft > 0 && (tick % 2) == 0) {
        --g_immediateSweepLeft;
        immediate = true;
    }

    if (!immediate && (tick % SOFT_SYNC_INTERVAL_TICKS) != 0) return;

    VehicleSoftSyncMsg msg = {};
    VehicleSoftSyncTreasury tr = {};
    VehicleSoftSyncBldBatch bldBatch = {};
    if (!BuildBatch(msg, tr, bldBatch, immediate)) return;

    g_vssDiag.batchesSent++;
    BYTE wireBuf[sizeof(VehicleSoftSyncMsg) + sizeof(VehicleSoftSyncTreasury) + sizeof(VehicleSoftSyncBldBatch)];
    size_t wireBytes = PackSoftSyncWire(msg, tr, bldBatch, wireBuf);
    Commands_Submit(LS_CMD_VEHICLE_SOFT_SYNC, wireBuf, wireBytes);
}

void VehicleSoftSync_RefreshCommandTreasury(void* data, size_t size)
{
    if (!data || size < sizeof(unsigned) + sizeof(VehicleSoftSyncTreasury) || !g_exeBase) return;
    const VehicleSoftSyncMsg* msg = (const VehicleSoftSyncMsg*)data;
    if (msg->count > SOFT_SYNC_BATCH_MAX) return;
    size_t baseBytes = sizeof(unsigned) + msg->count * sizeof(VehicleSoftSyncEntry);
    if (size < baseBytes + sizeof(VehicleSoftSyncTreasury)) return;
    if (*(const unsigned*)((const BYTE*)data + baseBytes) != VSS_TREASURY_MAGIC) return;

    DiagLog_BeginSafeSeh();
    __try {
        VehicleSoftSyncTreasury tr = {};
        CaptureSoftSyncTreasury(g_exeBase + WORLD_RVA, tr);
        if (tr.magic == VSS_TREASURY_MAGIC) {
            memcpy((BYTE*)data + baseBytes, &tr, sizeof(tr));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static BYTE* FindVehicle(BYTE** begin, size_t total, int place, unsigned vehicleId)
{
    if (place >= 0 && (size_t)place < total) {
        BYTE* v = begin[place];
        if (v && *(unsigned*)(v + VEH_ID_OFFSET) == vehicleId) return v;
    }
    for (size_t i = 0; i < total; i++) {
        BYTE* v = begin[i];
        if (v && *(unsigned*)(v + VEH_ID_OFFSET) == vehicleId) return v;
    }
    return nullptr;
}

static BYTE* FindBuilding(BYTE** begin, size_t total, int place, int bldId)
{
    if (place >= 0 && (size_t)place < total) {
        BYTE* b = begin[place];
        if (b && *(int*)b == bldId) return b;
    }
    for (size_t i = 0; i < total; i++) {
        BYTE* b = begin[i];
        if (b && *(int*)b == bldId) return b;
    }
    return nullptr;
}

static bool ApplySoftSyncTreasury(BYTE* w, const BYTE* trPtr)
{
    VehicleSoftSyncTreasury tr = {};
    memcpy(&tr, trPtr, sizeof(tr));
    if (tr.magic != VSS_TREASURY_MAGIC || (tr.flags & 1u) == 0 || *(int*)(w + WORLD_UNLIMITED_MONEY_OFFSET) != 0)
        return false;
    if ((tr.flags & 2u) != 0 && TwoStates_IsActive()) {
        TwoStates_SyncBalancesFromHost(tr.stateRub[0], tr.stateRub[1], tr.stateUsd[0], tr.stateUsd[1]);
    } else if (isfinite(tr.worldRub) && isfinite(tr.worldUsd)) {
        *(double*)(w + WORLD_RUB_OFFSET) = tr.worldRub;
        *(double*)(w + WORLD_USD_OFFSET) = tr.worldUsd;
    }
    return true;
}

static void ClearBuildingSlotAmounts(BYTE* sBeg, size_t localCount)
{
    for (size_t j = 0; j < localCount && j < 64; j++) {
        *(float*)(sBeg + j * CARGO_ENTRY_SIZE + CARGO_AMOUNT_OFFSET) = 0.0f;
    }
}

static void SyncSingleSlotResource(const BYTE* w, BYTE* slot, BYTE* sBeg, size_t& localCount,
                                   size_t capCount, unsigned resHash, float amount, bool& changed)
{
    if (!isfinite(amount) || !resHash) return;
    for (size_t j = 0; j < localCount && j < 64; j++) {
        BYTE* entry = sBeg + j * CARGO_ENTRY_SIZE;
        if (HashResourceName(*(const char**)entry) == resHash) {
            float prev = *(float*)(entry + CARGO_AMOUNT_OFFSET);
            if (fabsf(prev - amount) > 1e-4f) changed = true;
            *(float*)(entry + CARGO_AMOUNT_OFFSET) = amount;
            return;
        }
    }
    const char* resPtr = FindResourceByHash(w, resHash);
    if (!resPtr || localCount >= capCount || localCount >= 64) return;
    BYTE* entry = sBeg + localCount * CARGO_ENTRY_SIZE;
    *(const char**)entry = resPtr;
    *(float*)(entry + CARGO_AMOUNT_OFFSET) = amount;
    *(float*)(entry + 0x0C) = 0.0f;
    ++localCount;
    *(BYTE**)(slot + 8) = sBeg + localCount * CARGO_ENTRY_SIZE;
    changed = true;
}

static void SyncBuildingStorageSlot(const BYTE* w, BYTE* slot, unsigned char hostCount,
                                    const unsigned* resHashes, const float* amounts)
{
    BYTE* sBeg = *(BYTE**)slot;
    BYTE* sEnd = *(BYTE**)(slot + 8);
    BYTE* sCap = *(BYTE**)(slot + 16);
    if (!sBeg || sEnd < sBeg || sCap < sEnd || (sEnd - sBeg) % CARGO_ENTRY_SIZE != 0) return;
    size_t localCount = (size_t)((sEnd - sBeg) / CARGO_ENTRY_SIZE);
    if (hostCount == 0) {
        if (localCount > 0) {
            ClearBuildingSlotAmounts(sBeg, localCount);
            g_vssDiag.bldStorageUpdated++;
        }
        return;
    }
    if (hostCount > SOFT_SYNC_BLD_SLOT_ENTRIES_MAX) hostCount = SOFT_SYNC_BLD_SLOT_ENTRIES_MAX;
    size_t capCount = (size_t)((sCap - sBeg) / CARGO_ENTRY_SIZE);
    bool changed = false;
    for (unsigned char k = 0; k < hostCount; k++) {
        SyncSingleSlotResource(w, slot, sBeg, localCount, capCount, resHashes[k], amounts[k], changed);
    }
    if (changed) g_vssDiag.bldStorageUpdated++;
}

static bool ApplySoftSyncBuildings(BYTE* w, const BYTE* bldPtr, unsigned bldCount)
{
    BYTE** begin = *(BYTE***)(w + WORLD_BUILDINGS_OFFSET);
    BYTE** end = *(BYTE***)(w + WORLD_BUILDINGS_OFFSET + 8);
    if (!begin || end <= begin) return false;
    size_t total = (size_t)(end - begin);
    if (total > MAX_BUILDINGS_SCAN) return false;

    const VehicleSoftSyncBldEntry* entries = (const VehicleSoftSyncBldEntry*)(bldPtr + sizeof(unsigned) * 2);
    for (unsigned i = 0; i < bldCount; i++) {
        const VehicleSoftSyncBldEntry& e = entries[i];
        BYTE* b = FindBuilding(begin, total, e.place, e.bldId);
        if (!b) continue;
        if (isfinite(e.progress)) *(float*)(b + BLD_PROGRESS_OFFSET) = e.progress;
        if (isfinite(e.produced)) *(float*)(b + BLD_PRODUCED_OFFSET) = e.produced;

        BYTE* sBeg = *(BYTE**)(b + BLD_STORAGE_OFFSET);
        BYTE* sEnd = *(BYTE**)(b + BLD_STORAGE_OFFSET + 8);
        if (!sBeg || sEnd < sBeg || (sEnd - sBeg) % BLD_STORAGE_SLOT_SIZE != 0) continue;
        size_t ns = (size_t)((sEnd - sBeg) / BLD_STORAGE_SLOT_SIZE);
        size_t syncSlots = e.slotCount < ns ? e.slotCount : ns;
        if (syncSlots > SOFT_SYNC_BLD_SLOTS_MAX) syncSlots = SOFT_SYNC_BLD_SLOTS_MAX;
        for (size_t s = 0; s < syncSlots; s++) {
            SyncBuildingStorageSlot(w, sBeg + s * BLD_STORAGE_SLOT_SIZE,
                                    e.slotEntryCount[s], e.resHash[s], e.amount[s]);
        }
    }
    return true;
}

static void SyncVehicleCargo(BYTE* v, const VehicleSoftSyncEntry& e, bool stoppedAtBld)
{
    BYTE* cBegin = *(BYTE**)(v + VEH_CARGO_OFFSET);
    BYTE* cEnd = *(BYTE**)(v + VEH_CARGO_OFFSET + 8);
    if (!cBegin || cEnd < cBegin || (cEnd - cBegin) % CARGO_ENTRY_SIZE != 0) return;
    size_t cn = (size_t)((cEnd - cBegin) / CARGO_ENTRY_SIZE);
    if (cn != e.cargoCount) return;
    for (size_t k = 0; k < cn && k < 2; k++) {
        BYTE* entry = cBegin + k * CARGO_ENTRY_SIZE;
        const char* resName = *(const char**)entry;
        if (HashResourceName(resName) == e.cargoResHash[k] && isfinite(e.cargoAmount[k])) {
            float prevAmt = *(float*)(entry + CARGO_AMOUNT_OFFSET);
            float diff = fabsf(prevAmt - e.cargoAmount[k]);
            if (stoppedAtBld && e.cargoAmount[k] > 1e-4f && diff < 2.0f) continue;
            if (diff > 1e-4f) g_vssDiag.cargoUpdated++;
            *(float*)(entry + CARGO_AMOUNT_OFFSET) = e.cargoAmount[k];
        }
    }
}

static void SyncSingleVehicleEntry(BYTE* v, const VehicleSoftSyncEntry& e, float& batchMaxDriftSq)
{
    g_vssDiag.vehiclesSynced++;
    if (isfinite(e.fuel)) *(float*)(v + VEH_FUEL_OFFSET) = e.fuel;
    if (isfinite(e.wear[0])) *(float*)(v + VEH_WEAR0_OFFSET) = e.wear[0];
    if (isfinite(e.wear[1])) *(float*)(v + VEH_WEAR1_OFFSET) = e.wear[1];
    if (isfinite(e.wear[2])) *(float*)(v + VEH_WEAR2_OFFSET) = e.wear[2];

    BYTE* effBld = VehicleSoftSync_ResolveEffectiveBld(v);
    int localBldId = effBld ? *(int*)effBld : -1;
    bool stoppedAtBld = (effBld != nullptr || e.currentBldId >= 0);
    SyncVehicleCargo(v, e, stoppedAtBld);

    if (isfinite(e.passengers) && e.passengers >= 0.0f) {
        WriteVehiclePassengers(v, e.passengers);
    }

    if (*(int*)(v + VEH_SCHED_IDX_OFFSET) != e.scheduleIdx) return;

    float* pos = (float*)(v + VEH_POS_OFFSET);
    float dx = e.pos[0] - pos[0];
    float dy = e.pos[1] - pos[1];
    float dz = e.pos[2] - pos[2];
    float distSq = dx * dx + dy * dy + dz * dz;
    if (!isfinite(distSq)) return;
    if (distSq > batchMaxDriftSq && distSq < 1e6f) batchMaxDriftSq = distSq;

    if (e.currentBldId >= 0 && localBldId == e.currentBldId) {
        if (distSq > 0.0f && distSq < 400.0f) {
            TranslateVehicleNodes(v, dx, dy, dz);
            HeldFrame_OnVehicleSoftSynced(v);
            g_vssDiag.posSnapped++;
        }
        return;
    }
    if (e.currentBldId < 0 && localBldId < 0) {
        if (distSq > 1e-4f && distSq < 2500.0f) {
            TranslateVehicleNodes(v, dx, dy, dz);
            HeldFrame_OnVehicleSoftSynced(v);
            HeldFrame_AddVehicleSmoothOffset(v, -dx, -dy, -dz);
            g_vssDiag.posBlended++;
        } else if (distSq >= 2500.0f) {
            TranslateVehicleNodes(v, dx, dy, dz);
            HeldFrame_OnVehicleSoftSynced(v);
            g_vssDiag.posSnapped++;
        }
    }
}

static bool ParseSoftSyncTrailers(const BYTE* data, size_t size, size_t baseBytes,
                                  const BYTE** outTrPtr, const BYTE** outBldPtr, unsigned* outBldCount)
{
    *outTrPtr = nullptr;
    *outBldPtr = nullptr;
    *outBldCount = 0;
    size_t offset = baseBytes;
    if (offset + sizeof(VehicleSoftSyncTreasury) <= size &&
        *(const unsigned*)(data + offset) == VSS_TREASURY_MAGIC) {
        *outTrPtr = data + offset;
        offset += sizeof(VehicleSoftSyncTreasury);
    }
    if (offset + sizeof(unsigned) * 2 <= size &&
        *(const unsigned*)(data + offset) == VSS_BLD_STORAGE_MAGIC) {
        unsigned bldCount = *(const unsigned*)(data + offset + 4);
        if (bldCount == 0 || bldCount > SOFT_SYNC_BLD_BATCH_MAX) return false;
        size_t bldBytes = sizeof(unsigned) * 2 + bldCount * sizeof(VehicleSoftSyncBldEntry);
        if (offset + bldBytes != size) return false;
        *outBldPtr = data + offset;
        *outBldCount = bldCount;
        offset += bldBytes;
    }
    return offset == size;
}

bool VehicleSoftSync_ApplyCommand(const void* data, size_t size)
{
    if (!data || size < sizeof(unsigned)) return false;
    const VehicleSoftSyncMsg* msg = (const VehicleSoftSyncMsg*)data;
    if (msg->count > SOFT_SYNC_BATCH_MAX) return false;
    size_t baseBytes = sizeof(unsigned) + msg->count * sizeof(VehicleSoftSyncEntry);
    if (size < baseBytes) return false;

    const BYTE* trPtr = nullptr;
    const BYTE* bldPtr = nullptr;
    unsigned bldCount = 0;
    if (!ParseSoftSyncTrailers((const BYTE*)data, size, baseBytes, &trPtr, &bldPtr, &bldCount))
        return false;
    if (msg->count == 0 && !trPtr && !bldPtr) return false;
    if (LockstepNet_IsHost()) return true;

    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* w = g_exeBase + WORLD_RVA;
        if (trPtr && ApplySoftSyncTreasury(w, trPtr))
            ok = true;
        if (bldPtr && ApplySoftSyncBuildings(w, bldPtr, bldCount))
            ok = true;
        BYTE** begin = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
        BYTE** end = *(BYTE***)(w + WORLD_VEHICLES_OFFSET + 8);
        if (msg->count > 0 && begin && end > begin) {
            size_t total = (size_t)(end - begin);
            if (total <= MAX_VEHICLES_SCAN) {
                float batchMaxDriftSq = 0.0f;
                for (unsigned i = 0; i < msg->count; i++) {
                    const VehicleSoftSyncEntry& e = msg->entries[i];
                    BYTE* v = FindVehicle(begin, total, e.place, e.vehicleId);
                    if (v) SyncSingleVehicleEntry(v, e, batchMaxDriftSq);
                }
                float maxDriftM = sqrtf(batchMaxDriftSq);
                g_vssDiag.lastMaxDriftM = maxDriftM;
                if (maxDriftM > g_vssDiag.allTimeMaxDriftM) g_vssDiag.allTimeMaxDriftM = maxDriftM;
                g_vssDiag.batchesApplied++;
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}
