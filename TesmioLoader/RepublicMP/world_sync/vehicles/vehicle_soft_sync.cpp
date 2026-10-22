// SPDX-License-Identifier: GPL-3.0-only
// See vehicle_soft_sync.h.
#include "vehicle_soft_sync.h"
#include "../../lockstep/commands.h"
#include "../../lockstep/lockstep_net.h"
#include "../../determinism/held_frame.h"
#include "../../core/diag_log.h"
#include <string.h>
#include <math.h>

#define WORLD_RVA 0x9D4F10
#define WORLD_VEHICLES_OFFSET 0x12810

#define VEH_ID_OFFSET 0x04
#define VEH_CARGO_OFFSET 0x3B8
#define VEH_CUR_BLD_OFFSET 0x4F0
#define VEH_SCHED_IDX_OFFSET 0x698
#define VEH_FUEL_OFFSET 0x770
#define VEH_SPEED_OFFSET 0xD24
#define VEH_NODE0_OFFSET 0xD60
#define VEH_POS_OFFSET 0xDE0
#define VEH_WEAR0_OFFSET 0x17E8
#define VEH_WEAR1_OFFSET 0x1A54
#define VEH_WEAR2_OFFSET 0x1A7C

#define CARGO_ENTRY_SIZE 0x10
#define CARGO_AMOUNT_OFFSET 0x08

#define SOFT_SYNC_BATCH_MAX 24
#define SOFT_SYNC_INTERVAL_TICKS 180 // every 6 seconds of game simulation
#define MAX_VEHICLES_SCAN 65536

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
};

struct VehicleSoftSyncMsg {
    unsigned count;
    VehicleSoftSyncEntry entries[SOFT_SYNC_BATCH_MAX];
};
#pragma pack(pop)

static const unsigned kVehicleNodeOffsets[5] = { 0xD60, 0xE1C, 0xED8, 0xF94, 0x1080 };

static volatile LONG g_immediateRequested = 0;
static int g_immediateSweepLeft = 0;
static size_t g_rollingCursor = 0;
static unsigned long long g_lastImmediateTick = 0;

void VehicleSoftSync_Bind(const TsmHost* host)
{
    TsmBind(host);
}

void VehicleSoftSync_Reset()
{
    InterlockedExchange(&g_immediateRequested, 0);
    g_immediateSweepLeft = 0;
    g_rollingCursor = 0;
    g_lastImmediateTick = 0;
}

void VehicleSoftSync_RequestImmediate()
{
    InterlockedExchange(&g_immediateRequested, 1);
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
    for (int k = 0; k < 5; k++) {
        TranslateNode(v + kVehicleNodeOffsets[k], dx, dy, dz);
    }
}

static bool FillEntrySafe(BYTE* v, int place, VehicleSoftSyncEntry& out)
{
    bool ok = false;
    __try {
        memset(&out, 0, sizeof(out));
        out.place = place;
        out.vehicleId = *(unsigned*)(v + VEH_ID_OFFSET);
        BYTE* curBld = *(BYTE**)(v + VEH_CUR_BLD_OFFSET);
        out.currentBldId = curBld ? *(int*)curBld : -1;
        out.scheduleIdx = *(int*)(v + VEH_SCHED_IDX_OFFSET);
        out.fuel = *(float*)(v + VEH_FUEL_OFFSET);
        out.speed = *(float*)(v + VEH_SPEED_OFFSET);
        out.wear[0] = *(float*)(v + VEH_WEAR0_OFFSET);
        out.wear[1] = *(float*)(v + VEH_WEAR1_OFFSET);
        out.wear[2] = *(float*)(v + VEH_WEAR2_OFFSET);
        memcpy(out.pos, v + VEH_POS_OFFSET, 12);

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

static bool BuildBatch(VehicleSoftSyncMsg& msg, bool immediate)
{
    msg.count = 0;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* w = g_exeBase + WORLD_RVA;
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
                    BYTE* curBld = *(BYTE**)(v + VEH_CUR_BLD_OFFSET);
                    if (speed == 0.0f && curBld != nullptr) continue;
                    if (FillEntrySafe(v, (int)idx, msg.entries[msg.count])) {
                        msg.count++;
                    }
                }

                // Pass 2: during immediate sweeps, fill any remaining batch capacity with parked vehicles.
                if (immediate && msg.count < SOFT_SYNC_BATCH_MAX) {
                    for (size_t p = 0; p < total && msg.count < SOFT_SYNC_BATCH_MAX; p++) {
                        size_t idx = (startCursor + p) % total;
                        BYTE* v = begin[idx];
                        if (!v) continue;
                        float speed = *(float*)(v + VEH_SPEED_OFFSET);
                        BYTE* curBld = *(BYTE**)(v + VEH_CUR_BLD_OFFSET);
                        if (speed != 0.0f || curBld == nullptr) continue;
                        if (FillEntrySafe(v, (int)idx, msg.entries[msg.count])) {
                            msg.count++;
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        msg.count = 0;
    }
    DiagLog_EndSafeSeh();
    return msg.count > 0;
}

void VehicleSoftSync_HostTick(unsigned long long tick)
{
    if (!LockstepNet_IsConnected() || !LockstepNet_IsHost() || !Commands_Capturing()) return;

    if (tick >= g_lastImmediateTick + 12) {
        if (InterlockedCompareExchange(&g_immediateRequested, 0, 1) == 1) {
            g_immediateSweepLeft = 8;
            g_lastImmediateTick = tick;
        }
    }

    bool immediate = false;
    if (g_immediateSweepLeft > 0 && (tick % 2) == 0) {
        --g_immediateSweepLeft;
        immediate = true;
    }

    if (!immediate && (tick % SOFT_SYNC_INTERVAL_TICKS) != 0) return;

    VehicleSoftSyncMsg msg = {};
    if (!BuildBatch(msg, immediate)) return;

    size_t wireBytes = sizeof(unsigned) + msg.count * sizeof(VehicleSoftSyncEntry);
    Commands_Submit(LS_CMD_VEHICLE_SOFT_SYNC, &msg, wireBytes);
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

bool VehicleSoftSync_ApplyCommand(const void* data, size_t size)
{
    if (size < sizeof(unsigned)) return false;
    const VehicleSoftSyncMsg* msg = (const VehicleSoftSyncMsg*)data;
    if (msg->count == 0 || msg->count > SOFT_SYNC_BATCH_MAX) return false;
    if (size != sizeof(unsigned) + msg->count * sizeof(VehicleSoftSyncEntry)) return false;

    // The Host generated these authoritative values; only the Client needs soft self-healing.
    if (LockstepNet_IsHost()) return true;

    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* w = g_exeBase + WORLD_RVA;
        BYTE** begin = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
        BYTE** end = *(BYTE***)(w + WORLD_VEHICLES_OFFSET + 8);
        if (begin && end > begin) {
            size_t total = (size_t)(end - begin);
            if (total <= MAX_VEHICLES_SCAN) {
                for (unsigned i = 0; i < msg->count; i++) {
                    const VehicleSoftSyncEntry& e = msg->entries[i];
                    BYTE* v = FindVehicle(begin, total, e.place, e.vehicleId);
                    if (!v) continue;

                    // 1. Always synchronize scalar fuel and wear/aging fields.
                    if (isfinite(e.fuel)) *(float*)(v + VEH_FUEL_OFFSET) = e.fuel;
                    if (isfinite(e.wear[0])) *(float*)(v + VEH_WEAR0_OFFSET) = e.wear[0];
                    if (isfinite(e.wear[1])) *(float*)(v + VEH_WEAR1_OFFSET) = e.wear[1];
                    if (isfinite(e.wear[2])) *(float*)(v + VEH_WEAR2_OFFSET) = e.wear[2];

                    // 2. Synchronize cargo amounts when the cargo slot structure matches.
                    BYTE* cBegin = *(BYTE**)(v + VEH_CARGO_OFFSET);
                    BYTE* cEnd = *(BYTE**)(v + VEH_CARGO_OFFSET + 8);
                    if (cBegin && cEnd >= cBegin && (cEnd - cBegin) % CARGO_ENTRY_SIZE == 0) {
                        size_t cn = (size_t)((cEnd - cBegin) / CARGO_ENTRY_SIZE);
                        if (cn == e.cargoCount) {
                            for (size_t k = 0; k < cn && k < 2; k++) {
                                BYTE* entry = cBegin + k * CARGO_ENTRY_SIZE;
                                const char* resName = *(const char**)entry;
                                if (HashResourceName(resName) == e.cargoResHash[k] && isfinite(e.cargoAmount[k])) {
                                    *(float*)(entry + CARGO_AMOUNT_OFFSET) = e.cargoAmount[k];
                                }
                            }
                        }
                    }

                    // 3. Soft route & position convergence when heading to the same schedule stop.
                    int localSched = *(int*)(v + VEH_SCHED_IDX_OFFSET);
                    if (localSched != e.scheduleIdx) continue;

                    float* pos = (float*)(v + VEH_POS_OFFSET);
                    float dx = e.pos[0] - pos[0];
                    float dy = e.pos[1] - pos[1];
                    float dz = e.pos[2] - pos[2];
                    float distSq = dx * dx + dy * dy + dz * dz;
                    if (!isfinite(distSq)) continue;

                    BYTE* curBld = *(BYTE**)(v + VEH_CUR_BLD_OFFSET);
                    int localBldId = curBld ? *(int*)curBld : -1;

                    if (e.currentBldId >= 0 && localBldId == e.currentBldId) {
                        if (distSq > 0.0f && distSq < 400.0f) {
                            TranslateVehicleNodes(v, dx, dy, dz);
                            HeldFrame_OnVehicleSoftSynced(v);
                        }
                    } else if (e.currentBldId < 0 && localBldId < 0) {
                        float localSpeed = *(float*)(v + VEH_SPEED_OFFSET);
                        if (isfinite(e.speed) && isfinite(localSpeed)) {
                            *(float*)(v + VEH_SPEED_OFFSET) = localSpeed + (e.speed - localSpeed) * 0.35f;
                        }
                        if (distSq > 0.04f && distSq < 64.0f) {
                            TranslateVehicleNodes(v, dx * 0.25f, dy * 0.25f, dz * 0.25f);
                            HeldFrame_OnVehicleSoftSynced(v);
                        } else if (distSq > 0.0f && distSq <= 0.04f) {
                            TranslateVehicleNodes(v, dx, dy, dz);
                            HeldFrame_OnVehicleSoftSynced(v);
                        }
                    }
                }
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}
