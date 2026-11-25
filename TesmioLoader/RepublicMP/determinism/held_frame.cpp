// SPDX-License-Identifier: GPL-3.0-only
#include "held_frame.h"
#include "determinism.h"
#include "../core/engine_rva.h"
#include "../core/diag_log.h"
#include "../core/safe_mem.h"
#include <vector>
#include <new>
#include <unordered_map>
#include <algorithm>
#include <string.h>
#include <math.h>

void HeldFrame_Bind(const TsmHost* host) { TsmBind(host); }

// 2593d0 draws world+12810 through 3fa850 (6712b0, 67c280, 66edd0).
// Those builders recurse through parent +398, children +3a0 and link +4a0,
// and write render words back into vehicles. Copy every reachable object,
// then restore changed words after the worker joins. Never restore a changed
// heap pointer: the old allocation may have been freed. (by SoylentGreed)
static bool Read(const void* at, void* out, size_t n)
{
    if (!n) return true;
    uintptr_t addr = (uintptr_t)at;
    if (addr < 0x10000ull || addr >= 0x800000000000ull || n > 0x800000000000ull - addr) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        memcpy(out, at, n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool IsHeapPtrCandidate(unsigned long long v)
{
    return v >= (1ull << 32) && v < 0x800000000000ull && (v & 7) == 0;
}

static bool HeapPointer(unsigned long long v)
{
    if (!IsHeapPtrCandidate(v)) return false;
    unsigned long long probe = 0;
    return Read((const void*)v, &probe, sizeof(probe));
}

struct FlatPtrSet {
    std::vector<BYTE*> slots;
    size_t count = 0;

    void Clear()
    {
        slots.clear();
        count = 0;
    }

    void Reset(size_t expectedItems = 64)
    {
        size_t targetCap = 128;
        const size_t minCap = (expectedItems < 16) ? 64 : (expectedItems * 2);
        while (targetCap < minCap && targetCap < (1u << 20)) {
            targetCap <<= 1;
        }
        if (slots.size() < targetCap) {
            slots.assign(targetCap, nullptr);
        } else if (!slots.empty()) {
            memset(slots.data(), 0, slots.size() * sizeof(BYTE*));
        }
        count = 0;
    }

    void Grow()
    {
        const size_t newCap = slots.empty() ? 128 : (slots.size() * 2);
        std::vector<BYTE*> next(newCap, nullptr);
        const size_t mask = newCap - 1;
        for (BYTE* cur : slots) {
            if (!cur) continue;
            size_t idx = (size_t)(((uintptr_t)cur >> 4) * 11400714819323198485ull) & mask;
            while (next[idx]) idx = (idx + 1) & mask;
            next[idx] = cur;
        }
        slots.swap(next);
    }

    bool Insert(BYTE* p)
    {
        if (!p) return false;
        if (slots.empty() || (count + 1) * 2 >= slots.size()) {
            Grow();
        }
        const size_t mask = slots.size() - 1;
        size_t idx = (size_t)(((uintptr_t)p >> 4) * 11400714819323198485ull) & mask;
        for (;;) {
            BYTE* cur = slots[idx];
            if (cur == p) return false;
            if (!cur) {
                slots[idx] = p;
                count++;
                return true;
            }
            idx = (idx + 1) & mask;
        }
    }
};

// Each VEHICLE (0x1AD8 bytes) contains 5 primary C3D_NODE transforms (0xBC bytes each)
// saved by VEHICLE::Save (0x6C6770): +0xD60, +0xE1C, +0xED8, +0xF94, +0x1080, plus an
// optional 6th C3D_NODE at +0x1318 (active when *(int32_t*)(v + 0x1314) >= 0, rendered by 0x66EDD0).
// Inside each C3D_NODE (0xBC bytes = 47 floats, verified in C3DDLL64.dll 0x2A660..0x2A6E0 & 0xB8340):
//   +0x00..+0x3F: 4x4 Matrix (translation row at +0x30..+0x3B)
//   +0x40..+0x7F: 4x4 MatrixInv (translation row at +0x70..+0x7B)
//   +0x80..+0x8B: C3DVECTOR3 Position    (float[32..34])
//   +0x8C..+0x97: C3DVECTOR3 Rotation    (float[35..37], Euler angles)
//   +0x98..+0xA3: C3DVECTOR3 Scale       (float[38..40], typically (1, 1, 1))
//   +0xA4..+0xAF: C3DVECTOR3 Orientation (float[41..43], forward unit vector)
//   +0xB0..+0xBB: C3DVECTOR3 UpVector    (float[44..46], up unit vector)
static constexpr int VEH_MAX_NODES = 6;
static constexpr int VEH_MAX_WHEELS = 16;
static constexpr int NODE_SNAPSHOT_FLOATS = 47; // 0xBC bytes: m[16], minv[16], pos[3], rot[3], scale[3], ori[3], up[3]
static const unsigned kVehicleNodeOffsets[VEH_MAX_NODES] = { 0xD60, 0xE1C, 0xED8, 0xF94, 0x1080, VEH_SIXTH_NODE_OFFSET };

static int GetActiveVehicleNodeCount(const BYTE* p)
{
    return (*(const int32_t*)(p + VEH_SIXTH_NODE_ACTIVE_OFFSET) >= 0) ? 6 : 5;
}

// Caller MUST invoke inside an active __try block.
static int GetVehicleWheelCountUnsafe(const BYTE* p, BYTE** outBegin)
{
    *outBegin = nullptr;
    uintptr_t begin = *(const uintptr_t*)(p + VEH_WHEELS_BEGIN_OFFSET);
    uintptr_t end   = *(const uintptr_t*)(p + VEH_WHEELS_END_OFFSET);
    if (!begin || end <= begin || (end - begin) % VEH_WHEEL_STRUCT_STRIDE != 0) return 0;
    size_t count = (end - begin) / VEH_WHEEL_STRUCT_STRIDE;
    if (count > 64 || !IsPointerLikeX64((uint64_t)begin)) return 0;
    if (count > (size_t)VEH_MAX_WHEELS) count = (size_t)VEH_MAX_WHEELS;
    volatile uint64_t probe = *(const uint64_t*)begin;
    probe ^= *(const uint64_t*)(begin + count * VEH_WHEEL_STRUCT_STRIDE - sizeof(uint64_t));
    (void)probe;
    *outBegin = (BYTE*)begin;
    return (int)count;
}

float HeldFrame_LerpAngleRadians(float prevRad, float currRad, float alpha)
{
    if (!isfinite(prevRad) || !isfinite(currRad) || !isfinite(alpha)) return currRad;
    if (alpha < 0.0f) alpha = 0.0f;
    else if (alpha > 1.0f) alpha = 1.0f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    float delta = fmodf(currRad - prevRad, kTwoPi);
    if (delta > kPi) delta -= kTwoPi;
    else if (delta < -kPi) delta += kTwoPi;
    return prevRad + delta * alpha;
}

struct VehicleInterpRecord {
    unsigned id;
    unsigned long long prevTick;
    unsigned long long currTick;
    int nodeCount;
    float prevNode[VEH_MAX_NODES][NODE_SNAPSHOT_FLOATS];
    float currNode[VEH_MAX_NODES][NODE_SNAPSHOT_FLOATS];
    float smoothOffset[3];
    float softSyncShift[3];
    bool softSynced;
    float prevSteer;
    float currSteer;
    float currSteerTarget;
    int wheelCount;
    float prevWheelAngle[VEH_MAX_WHEELS];
    float currWheelAngle[VEH_MAX_WHEELS];
};

struct LiveVehicleVisualBackup {
    BYTE* vehicle;
    unsigned id;
    int nodeCount;
    float nodes[VEH_MAX_NODES][NODE_SNAPSHOT_FLOATS];
    float steer;
    float steerTarget;
    int wheelCount;
    float wheelAngles[VEH_MAX_WHEELS];
};

static std::unordered_map<BYTE*, VehicleInterpRecord> g_vehicleInterp;
static unsigned long long g_interpTick = 0;
static HeldFrameDiagStats g_hfDiag = {};
static std::vector<LiveVehicleVisualBackup> g_camPrepModified;
static BYTE g_savedCamMgr[CAM_MGR_STRUCT_BYTES] = {};
static BYTE g_savedCameraNode[C3D_NODE_BYTE_SIZE] = {};
static BYTE* g_savedCamPidPtrs[CAM_MGR_PID_COUNT] = {};
static BYTE g_savedCamPidData[CAM_MGR_PID_COUNT][CAM_MGR_PID_STRUCT_BYTES] = {};
static BYTE* g_savedCamMgrPtr = nullptr;
static BYTE* g_savedRideVeh = nullptr;
static BYTE* g_savedFollowWin = nullptr;
static float g_savedCamFps = 60.0f;
static bool g_hasCamSnapshot = false;
static bool g_hasSavedCameraNode = false;

struct DeferredVehicleHighlight {
    BYTE* vehicle;
    unsigned char a2;
    unsigned char a3;
    float color[4];
    bool hasColor;
    unsigned char flag;
    bool isExtraPass;
};
static std::vector<DeferredVehicleHighlight> g_deferredPrepHighlights;
static void (*g_testHighlightCb)(BYTE* veh, bool isExtraPass, void* ud) = nullptr;
static void* g_testHighlightUd = nullptr;

void HeldFrame_SetHighlightCaptureForTest(void (*cb)(BYTE* veh, bool isExtraPass, void* ud), void* ud)
{
    g_testHighlightCb = cb;
    g_testHighlightUd = ud;
}

struct BuildingAnimState {
    float visualFrame;
    float stepPerTick;
    bool hasMeasuredStep;
};
struct AnimBldSnapshot {
    BYTE* b;
    uint32_t listIndex;
    float maxFrames;
    float typeSpeed;
    float curFrame;
    float fallbackStep;
};
struct AnimBldWrite {
    BYTE* b;
    float frame;
};
static std::unordered_map<BYTE*, BuildingAnimState> g_buildingAnimState;
static std::vector<AnimBldSnapshot> g_animReadScratch;
static std::vector<AnimBldWrite> g_animWriteScratch;
static std::vector<BYTE*> g_animStillScratch;
static std::vector<BYTE*> g_tickVehListScratch;
static std::vector<BYTE*> g_tickFollowRootsScratch;
static std::vector<BYTE*> g_tickQueueScratch;
static std::vector<BYTE*> g_tickAttachedScratch;
static FlatPtrSet g_tickVisitedScratch;
static void ClearViewCaches();

void HeldFrame_Reset()
{
    g_vehicleInterp.clear();
    g_buildingAnimState.clear();
    g_animReadScratch.clear();
    g_animWriteScratch.clear();
    g_animStillScratch.clear();
    g_tickVehListScratch.clear();
    g_tickFollowRootsScratch.clear();
    g_tickQueueScratch.clear();
    g_tickAttachedScratch.clear();
    g_tickVisitedScratch.Clear();
    g_interpTick = 0;
    memset(&g_hfDiag, 0, sizeof(g_hfDiag));
    g_camPrepModified.clear();
    g_deferredPrepHighlights.clear();
    memset(g_savedCamMgr, 0, sizeof(g_savedCamMgr));
    memset(g_savedCameraNode, 0, sizeof(g_savedCameraNode));
    memset(g_savedCamPidPtrs, 0, sizeof(g_savedCamPidPtrs));
    memset(g_savedCamPidData, 0, sizeof(g_savedCamPidData));
    g_savedCamMgrPtr = nullptr;
    g_savedRideVeh = nullptr;
    g_savedFollowWin = nullptr;
    g_savedCamFps = 60.0f;
    g_hasCamSnapshot = false;
    g_hasSavedCameraNode = false;
    ClearViewCaches();
}

void HeldFrame_GetDiagStats(HeldFrameDiagStats* out)
{
    if (!out) return;
    *out = g_hfDiag;
}

static void NormalizeVec3(float v[3], const float fallback[3])
{
    float lenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (lenSq > 1e-8f && isfinite(lenSq)) {
        float inv = 1.0f / sqrtf(lenSq);
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    } else {
        v[0] = fallback[0];
        v[1] = fallback[1];
        v[2] = fallback[2];
    }
}

void HeldFrame_InterpolateC3DNode(BYTE* node, const float prevNode[47], const float currNode[47],
                                  float alpha, const float smoothOff[3])
{
    if (alpha < 0.0f) alpha = 0.0f;
    else if (alpha > 1.0f) alpha = 1.0f;

    float* m = (float*)node;
    float* minv = (float*)(node + 0x40);
    float* pos = (float*)(node + 0x80);
    float* rot = (float*)(node + 0x8C);
    float* scale = (float*)(node + 0x98);
    float* ori = (float*)(node + 0xA4);
    float* up = (float*)(node + 0xB0);

    rot[0] = HeldFrame_LerpAngleRadians(prevNode[35], currNode[35], alpha);
    rot[1] = HeldFrame_LerpAngleRadians(prevNode[36], currNode[36], alpha);
    rot[2] = HeldFrame_LerpAngleRadians(prevNode[37], currNode[37], alpha);
    scale[0] = currNode[38]; scale[1] = currNode[39]; scale[2] = currNode[40];

    const float sox = smoothOff ? smoothOff[0] : 0.0f;
    const float soy = smoothOff ? smoothOff[1] : 0.0f;
    const float soz = smoothOff ? smoothOff[2] : 0.0f;

    const float px = prevNode[32] + (currNode[32] - prevNode[32]) * alpha + sox;
    const float py = prevNode[33] + (currNode[33] - prevNode[33]) * alpha + soy;
    const float pz = prevNode[34] + (currNode[34] - prevNode[34]) * alpha + soz;
    pos[0] = m[12] = px;
    pos[1] = m[13] = py;
    pos[2] = m[14] = pz;

    const float sx = sqrtf(currNode[0] * currNode[0] + currNode[1] * currNode[1] + currNode[2] * currNode[2]);
    const float sy = sqrtf(currNode[4] * currNode[4] + currNode[5] * currNode[5] + currNode[6] * currNode[6]);
    const float sz = sqrtf(currNode[8] * currNode[8] + currNode[9] * currNode[9] + currNode[10] * currNode[10]);
    const float dotZ = prevNode[8] * currNode[8] + prevNode[9] * currNode[9] + prevNode[10] * currNode[10];

    if (sx > 1e-4f && sy > 1e-4f && sz > 1e-4f && sx < 100.0f && sy < 100.0f && sz < 100.0f &&
        dotZ > 0.25f * sz * sz && isfinite(sx + sy + sz + dotZ)) {
        float uz[3] = {
            prevNode[8] + (currNode[8] - prevNode[8]) * alpha,
            prevNode[9] + (currNode[9] - prevNode[9]) * alpha,
            prevNode[10] + (currNode[10] - prevNode[10]) * alpha,
        };
        float currUz[3] = { currNode[8] / sz, currNode[9] / sz, currNode[10] / sz };
        NormalizeVec3(uz, currUz);

        float ry[3] = {
            prevNode[4] + (currNode[4] - prevNode[4]) * alpha,
            prevNode[5] + (currNode[5] - prevNode[5]) * alpha,
            prevNode[6] + (currNode[6] - prevNode[6]) * alpha,
        };
        float projYZ = ry[0] * uz[0] + ry[1] * uz[1] + ry[2] * uz[2];
        float uy[3] = { ry[0] - projYZ * uz[0], ry[1] - projYZ * uz[1], ry[2] - projYZ * uz[2] };
        float currUy[3] = { currNode[4] / sy, currNode[5] / sy, currNode[6] / sy };
        NormalizeVec3(uy, currUy);

        float cx = currNode[5] * currNode[10] - currNode[6] * currNode[9];
        float cy = currNode[6] * currNode[8] - currNode[4] * currNode[10];
        float cz = currNode[4] * currNode[9] - currNode[5] * currNode[8];
        float handedness = (cx * currNode[0] + cy * currNode[1] + cz * currNode[2] >= 0.0f) ? 1.0f : -1.0f;
        float ux[3] = {
            (uy[1] * uz[2] - uy[2] * uz[1]) * handedness,
            (uy[2] * uz[0] - uy[0] * uz[2]) * handedness,
            (uy[0] * uz[1] - uy[1] * uz[0]) * handedness,
        };

        m[0] = ux[0] * sx; m[1] = ux[1] * sx; m[2] = ux[2] * sx;
        m[4] = uy[0] * sy; m[5] = uy[1] * sy; m[6] = uy[2] * sy;
        m[8] = uz[0] * sz; m[9] = uz[1] * sz; m[10] = uz[2] * sz;

        const float invSx = 1.0f / sx, invSy = 1.0f / sy, invSz = 1.0f / sz;
        minv[0] = ux[0] * invSx; minv[4] = ux[1] * invSx; minv[8]  = ux[2] * invSx;
        minv[1] = uy[0] * invSy; minv[5] = uy[1] * invSy; minv[9]  = uy[2] * invSy;
        minv[2] = uz[0] * invSz; minv[6] = uz[1] * invSz; minv[10] = uz[2] * invSz;
    }

    minv[12] = -(px * minv[0] + py * minv[4] + pz * minv[8]);
    minv[13] = -(px * minv[1] + py * minv[5] + pz * minv[9]);
    minv[14] = -(px * minv[2] + py * minv[6] + pz * minv[10]);

    float newOri[3] = {
        prevNode[41] + (currNode[41] - prevNode[41]) * alpha,
        prevNode[42] + (currNode[42] - prevNode[42]) * alpha,
        prevNode[43] + (currNode[43] - prevNode[43]) * alpha,
    };
    NormalizeVec3(newOri, &currNode[41]);
    ori[0] = newOri[0]; ori[1] = newOri[1]; ori[2] = newOri[2];

    float newUp[3] = {
        prevNode[44] + (currNode[44] - prevNode[44]) * alpha,
        prevNode[45] + (currNode[45] - prevNode[45]) * alpha,
        prevNode[46] + (currNode[46] - prevNode[46]) * alpha,
    };
    NormalizeVec3(newUp, &currNode[44]);
    up[0] = newUp[0]; up[1] = newUp[1]; up[2] = newUp[2];
}

static void InitVehicleInterpRecord(VehicleInterpRecord& rec, const BYTE* p, unsigned id,
                                    unsigned long long tick, int nNodes)
{
    rec.id = id;
    rec.prevTick = tick;
    rec.currTick = tick;
    rec.nodeCount = nNodes;
    rec.smoothOffset[0] = rec.smoothOffset[1] = rec.smoothOffset[2] = 0.0f;
    rec.softSyncShift[0] = rec.softSyncShift[1] = rec.softSyncShift[2] = 0.0f;
    rec.softSynced = false;
    for (int k = 0; k < nNodes; k++) {
        const float* nodeData = (const float*)(p + kVehicleNodeOffsets[k]);
        memcpy(rec.prevNode[k], nodeData, NODE_SNAPSHOT_FLOATS * sizeof(float));
        memcpy(rec.currNode[k], nodeData, NODE_SNAPSHOT_FLOATS * sizeof(float));
    }
    float steer = *(const float*)(p + VEH_STEER_CURRENT_OFFSET);
    float steerTarget = *(const float*)(p + VEH_STEER_TARGET_OFFSET);
    rec.prevSteer = isfinite(steer) ? steer : 0.0f;
    rec.currSteer = rec.prevSteer;
    rec.currSteerTarget = isfinite(steerTarget) ? steerTarget : rec.currSteer;
    BYTE* wheelBegin = nullptr;
    rec.wheelCount = GetVehicleWheelCountUnsafe(p, &wheelBegin);
    for (int w = 0; w < rec.wheelCount; w++) {
        float ang = *(const float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE);
        rec.prevWheelAngle[w] = isfinite(ang) ? ang : 0.0f;
        rec.currWheelAngle[w] = rec.prevWheelAngle[w];
    }
}

static void AdvanceVehicleSteerAndWheels(VehicleInterpRecord& rec, BYTE* p)
{
    rec.prevSteer = rec.currSteer;
    float liveSteer = *(const float*)(p + VEH_STEER_CURRENT_OFFSET);
    float targetSteer = *(const float*)(p + VEH_STEER_TARGET_OFFSET);
    if (!isfinite(liveSteer)) liveSteer = rec.prevSteer;
    if (!isfinite(targetSteer)) targetSteer = liveSteer;
    rec.currSteerTarget = targetSteer;
    if (fabsf(liveSteer - rec.prevSteer) > 1e-6f) {
        rec.currSteer = liveSteer;
    } else {
        constexpr float kSteerStepPerTick = 1.5f / 30.0f;
        if (liveSteer < targetSteer) {
            liveSteer += kSteerStepPerTick;
            if (liveSteer > targetSteer) liveSteer = targetSteer;
        } else if (liveSteer > targetSteer) {
            liveSteer -= kSteerStepPerTick;
            if (liveSteer < targetSteer) liveSteer = targetSteer;
        }
        rec.currSteer = liveSteer;
        *(float*)(p + VEH_STEER_CURRENT_OFFSET) = liveSteer;
    }

    BYTE* wheelBegin = nullptr;
    int wCount = GetVehicleWheelCountUnsafe(p, &wheelBegin);
    if (wCount != rec.wheelCount) {
        rec.wheelCount = wCount;
        for (int w = 0; w < wCount; w++) {
            float ang = *(const float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE);
            rec.prevWheelAngle[w] = isfinite(ang) ? ang : 0.0f;
            rec.currWheelAngle[w] = rec.prevWheelAngle[w];
        }
    } else {
        for (int w = 0; w < wCount; w++) {
            rec.prevWheelAngle[w] = rec.currWheelAngle[w];
            float ang = *(const float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE);
            rec.currWheelAngle[w] = isfinite(ang) ? ang : rec.prevWheelAngle[w];
        }
    }
}

static void RefreshVehicleCurrSnapshot(VehicleInterpRecord& rec, const BYTE* p, int nNodes)
{
    for (int k = 0; k < nNodes; k++) {
        memcpy(rec.currNode[k], p + kVehicleNodeOffsets[k], NODE_SNAPSHOT_FLOATS * sizeof(float));
    }
    float steer = *(const float*)(p + VEH_STEER_CURRENT_OFFSET);
    float steerTarget = *(const float*)(p + VEH_STEER_TARGET_OFFSET);
    if (isfinite(steer)) rec.currSteer = steer;
    if (isfinite(steerTarget)) rec.currSteerTarget = steerTarget;

    BYTE* wheelBegin = nullptr;
    int wCount = GetVehicleWheelCountUnsafe(p, &wheelBegin);
    if (wCount != rec.wheelCount) {
        rec.wheelCount = wCount;
        for (int w = 0; w < wCount; w++) {
            float ang = *(const float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE);
            rec.prevWheelAngle[w] = isfinite(ang) ? ang : 0.0f;
            rec.currWheelAngle[w] = rec.prevWheelAngle[w];
        }
    } else {
        for (int w = 0; w < wCount; w++) {
            float ang = *(const float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE);
            if (isfinite(ang)) rec.currWheelAngle[w] = ang;
        }
    }
}

static void RecordOneVehicleTick(BYTE* p, unsigned long long tick)
{
    if (!p) return;
    unsigned id = *(unsigned*)(p + 0x04);
    int nNodes = GetActiveVehicleNodeCount(p);
    auto it = g_vehicleInterp.find(p);
    if (it == g_vehicleInterp.end()) {
        VehicleInterpRecord rec = {};
        InitVehicleInterpRecord(rec, p, id, tick, nNodes);
        g_vehicleInterp.emplace(p, rec);
        return;
    }
    VehicleInterpRecord& rec = it->second;
    if (rec.id == id && rec.currTick == tick) {
        if (rec.nodeCount != nNodes) {
            InitVehicleInterpRecord(rec, p, id, tick, nNodes);
            return;
        }
        RefreshVehicleCurrSnapshot(rec, p, nNodes);
        return;
    }
    if (rec.id != id || rec.currTick + 1 != tick || rec.nodeCount != nNodes) {
        InitVehicleInterpRecord(rec, p, id, tick, nNodes);
        return;
    }
    rec.prevTick = rec.currTick;
    rec.currTick = tick;
    for (int k = 0; k < nNodes; k++) {
        memcpy(rec.prevNode[k], rec.currNode[k], NODE_SNAPSHOT_FLOATS * sizeof(float));
        memcpy(rec.currNode[k], p + kVehicleNodeOffsets[k], NODE_SNAPSHOT_FLOATS * sizeof(float));
    }
    if (rec.softSynced) {
        float dx = rec.currNode[0][32] - rec.prevNode[0][32];
        float dy = rec.currNode[0][33] - rec.prevNode[0][33];
        float dz = rec.currNode[0][34] - rec.prevNode[0][34];
        float dot = dx * rec.softSyncShift[0] + dy * rec.softSyncShift[1] + dz * rec.softSyncShift[2];
        if (dot < -1.0f) {
            for (int k = 0; k < nNodes; k++) {
                memcpy(rec.prevNode[k], rec.currNode[k], NODE_SNAPSHOT_FLOATS * sizeof(float));
            }
            rec.smoothOffset[0] = rec.smoothOffset[1] = rec.smoothOffset[2] = 0.0f;
        }
        rec.softSynced = false;
        rec.softSyncShift[0] = rec.softSyncShift[1] = rec.softSyncShift[2] = 0.0f;
    }
    AdvanceVehicleSteerAndWheels(rec, p);
}

void HeldFrame_OnVehicleSoftSynced(BYTE* vehicle)
{
    if (!vehicle) return;
    auto it = g_vehicleInterp.find(vehicle);
    if (it == g_vehicleInterp.end()) return;
    DiagLog_BeginSafeSeh();
    __try {
        int nNodes = GetActiveVehicleNodeCount(vehicle);
        it->second.nodeCount = nNodes;
        for (int k = 0; k < nNodes; k++) {
            const float* nodeData = (const float*)(vehicle + kVehicleNodeOffsets[k]);
            const float* pos = nodeData + 32;
            float sx = pos[0] - it->second.currNode[k][32];
            float sy = pos[1] - it->second.currNode[k][33];
            float sz = pos[2] - it->second.currNode[k][34];
            if (k == 0) {
                it->second.softSyncShift[0] = sx;
                it->second.softSyncShift[1] = sy;
                it->second.softSyncShift[2] = sz;
                it->second.softSynced = true;
            }
            float shiftSq = sx * sx + sy * sy + sz * sz;
            if (shiftSq < 2500.0f && isfinite(shiftSq)) {
                it->second.prevNode[k][32] += sx;
                it->second.prevNode[k][33] += sy;
                it->second.prevNode[k][34] += sz;
                it->second.prevNode[k][12] += sx;
                it->second.prevNode[k][13] += sy;
                it->second.prevNode[k][14] += sz;
            } else {
                memcpy(it->second.prevNode[k], nodeData, NODE_SNAPSHOT_FLOATS * sizeof(float));
            }
            memcpy(it->second.currNode[k], nodeData, NODE_SNAPSHOT_FLOATS * sizeof(float));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void HeldFrame_AddVehicleSmoothOffset(BYTE* vehicle, float ox, float oy, float oz)
{
    if (!vehicle || !isfinite(ox) || !isfinite(oy) || !isfinite(oz)) return;
    auto it = g_vehicleInterp.find(vehicle);
    if (it == g_vehicleInterp.end()) return;
    float magSq = ox * ox + oy * oy + oz * oz;
    if (magSq > 2500.0f) return;
    float nx = it->second.smoothOffset[0] + ox;
    float ny = it->second.smoothOffset[1] + oy;
    float nz = it->second.smoothOffset[2] + oz;
    float totalSq = nx * nx + ny * ny + nz * nz;
    if (totalSq > 2500.0f && isfinite(totalSq)) {
        float scale = 50.0f / sqrtf(totalSq);
        nx *= scale;
        ny *= scale;
        nz *= scale;
    }
    it->second.smoothOffset[0] = nx;
    it->second.smoothOffset[1] = ny;
    it->second.smoothOffset[2] = nz;
}

static void SafeRecordOneVehicleTick(BYTE* p, unsigned long long tick)
{
    if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) return;
    uint64_t probe = 0;
    if (!Read(p, &probe, sizeof(probe)) || !Read(p + 0x1AD8 - sizeof(probe), &probe, sizeof(probe))) return;
    DiagLog_BeginSafeSeh();
    __try {
        RecordOneVehicleTick(p, tick);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void ReadAttachedVehicleVector(const BYTE* veh, unsigned beginOff, std::vector<BYTE*>& outList)
{
    outList.clear();
    BYTE** vec[2] = {};
    if (!Read(veh + beginOff, vec, sizeof(vec))) return;
    uintptr_t begin = (uintptr_t)vec[0], end = (uintptr_t)vec[1];
    if (end <= begin || (end - begin) % sizeof(BYTE*) != 0 || (end - begin) / sizeof(BYTE*) > 1024 ||
        !IsPointerLikeX64((uint64_t)begin)) {
        return;
    }
    size_t count = (end - begin) / sizeof(BYTE*);
    outList.resize(count);
    if (!Read((const void*)begin, outList.data(), count * sizeof(BYTE*))) {
        outList.clear();
    }
}

static void RecordVehicleGraph(BYTE* start, unsigned long long tick, FlatPtrSet& visited,
                               std::vector<BYTE*>& queue, std::vector<BYTE*>& attached)
{
    if (!start || !IsPointerLikeX64((uint64_t)(uintptr_t)start)) return;
    queue.clear();
    if (visited.Insert(start)) queue.push_back(start);

    for (size_t qi = 0; qi < queue.size() && qi < 2048; ++qi) {
        BYTE* curr = queue[qi];
        SafeRecordOneVehicleTick(curr, tick);

        if (queue.size() >= 2048) continue;

        BYTE* link = nullptr;
        if (Read(curr + VEH_LINK_PTR_OFFSET, &link, sizeof(link)) && link &&
            IsPointerLikeX64((uint64_t)(uintptr_t)link) && visited.Insert(link)) {
            queue.push_back(link);
        }

        if (queue.size() >= 2048) continue;

        BYTE* parent = nullptr;
        if (Read(curr + VEH_PARENT_PTR_OFFSET, &parent, sizeof(parent)) && parent &&
            IsPointerLikeX64((uint64_t)(uintptr_t)parent) && visited.Insert(parent)) {
            queue.push_back(parent);
        }

        const unsigned kVecOffsets[2] = { VEH_CHILDREN_BEGIN_OFFSET, VEH_CARRIED_BEGIN_OFFSET };
        for (unsigned off : kVecOffsets) {
            if (queue.size() >= 2048) break;
            ReadAttachedVehicleVector(curr, off, attached);
            for (BYTE* child : attached) {
                if (queue.size() >= 2048) break;
                if (child && IsPointerLikeX64((uint64_t)(uintptr_t)child) && visited.Insert(child)) {
                    queue.push_back(child);
                }
            }
        }
    }
}

static bool ReadEngineVehicleList(const void* listAt, std::vector<BYTE*>& outList)
{
    BYTE** v[2] = {};
    if (!Read(listAt, v, sizeof(v)) || (uintptr_t)v[1] < (uintptr_t)v[0] ||
        ((uintptr_t)v[1] - (uintptr_t)v[0]) % sizeof(BYTE*) != 0 ||
        ((uintptr_t)v[1] - (uintptr_t)v[0]) / sizeof(BYTE*) > 65536) return false;
    size_t count = ((uintptr_t)v[1] - (uintptr_t)v[0]) / sizeof(BYTE*);
    outList.resize(count);
    return Read(v[0], outList.data(), count * sizeof(BYTE*));
}

static void AddVisibleVehicleTarget(BYTE* p, FlatPtrSet& seen, std::vector<BYTE*>& targets)
{
    if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) return;
    uint64_t probe = 0;
    if (!Read(p, &probe, sizeof(probe)) || !Read(p + 0x1AD8 - sizeof(probe), &probe, sizeof(probe))) return;
    if (seen.Insert(p)) targets.push_back(p);
}

static void CollectFollowCameraVehicles(std::vector<BYTE*>& outVehicles)
{
    outVehicles.clear();
    if (!g_exeBase) return;
    BYTE* camMgr = nullptr;
    if (!Read(g_exeBase + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF, &camMgr, sizeof(camMgr)) ||
        !camMgr || !IsPointerLikeX64((uint64_t)(uintptr_t)camMgr)) {
        return;
    }
    BYTE followActive = 0;
    if (!Read(camMgr + CAM_MGR_FOLLOW_ACTIVE_OFF, &followActive, sizeof(followActive)) || !followActive) {
        return;
    }
    FlatPtrSet seen;
    seen.Reset(16);
    BYTE* rideVeh = nullptr;
    if (Read(camMgr + CAM_MGR_RIDE_VEH_PTR_OFF, &rideVeh, sizeof(rideVeh)) && rideVeh) {
        AddVisibleVehicleTarget(rideVeh, seen, outVehicles);
    }
    BYTE* followWin = nullptr;
    if (Read(camMgr + CAM_MGR_FOLLOW_WIN_PTR_OFF, &followWin, sizeof(followWin)) &&
        followWin && IsPointerLikeX64((uint64_t)(uintptr_t)followWin)) {
        BYTE* followVeh = nullptr;
        if (Read(followWin + FOLLOW_WIN_VEHICLE_PTR_OFF, &followVeh, sizeof(followVeh)) && followVeh) {
            AddVisibleVehicleTarget(followVeh, seen, outVehicles);
        }
        BYTE* followCit = nullptr;
        if (Read(followWin + FOLLOW_WIN_CITIZEN_PTR_OFF, &followCit, sizeof(followCit)) &&
            followCit && IsPointerLikeX64((uint64_t)(uintptr_t)followCit)) {
            BYTE* citVeh = nullptr;
            if (Read(followCit + CIT_RIDING_VEHICLE_PTR_OFF, &citVeh, sizeof(citVeh)) && citVeh) {
                AddVisibleVehicleTarget(citVeh, seen, outVehicles);
            }
        }
    }
}

void HeldFrame_OnSimTick(unsigned long long tick)
{
    g_interpTick = tick;
    if (!ReadEngineVehicleList((const void*)(g_exeBase + 0x9E7720), g_tickVehListScratch)) return;

    g_tickVisitedScratch.Reset(g_tickVehListScratch.size() * 2 + 32);
    for (BYTE* p : g_tickVehListScratch) {
        if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) continue;
        RecordVehicleGraph(p, tick, g_tickVisitedScratch, g_tickQueueScratch, g_tickAttachedScratch);
    }
    CollectFollowCameraVehicles(g_tickFollowRootsScratch);
    for (BYTE* p : g_tickFollowRootsScratch) {
        RecordVehicleGraph(p, tick, g_tickVisitedScratch, g_tickQueueScratch, g_tickAttachedScratch);
    }

    if ((tick % 600) == 0 && g_vehicleInterp.size() > 8192) {
        for (auto it = g_vehicleInterp.begin(); it != g_vehicleInterp.end(); ) {
            if (it->second.currTick != tick) it = g_vehicleInterp.erase(it);
            else ++it;
        }
    }
    g_hfDiag.trackedVehicles = (unsigned)g_vehicleInterp.size();
}

static void ApplyWheelAndSteerInterp(BYTE* p, const VehicleInterpRecord& rec, float alpha)
{
    float steer = HeldFrame_LerpAngleRadians(rec.prevSteer, rec.currSteer, alpha);
    *(float*)(p + VEH_STEER_CURRENT_OFFSET) = steer;
    *(float*)(p + VEH_STEER_TARGET_OFFSET) = steer;

    BYTE* wheelBegin = nullptr;
    int wCount = GetVehicleWheelCountUnsafe(p, &wheelBegin);
    if (wCount > rec.wheelCount) wCount = rec.wheelCount;
    for (int w = 0; w < wCount; w++) {
        *(float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) =
            HeldFrame_LerpAngleRadians(rec.prevWheelAngle[w], rec.currWheelAngle[w], alpha);
    }
}

static bool ApplySubTickVisualInterpRaw(BYTE* p, VehicleInterpRecord& rec, float alpha, bool decaySmooth)
{
    bool moved = false;
    if (*(unsigned*)(p + 0x04) == rec.id) {
        if (decaySmooth) {
            for (int c = 0; c < 3; c++) {
                rec.smoothOffset[c] *= 0.85f;
                if (fabsf(rec.smoothOffset[c]) < 1e-4f) rec.smoothOffset[c] = 0.0f;
            }
        }
        const float* cur0 = (const float*)(p + kVehicleNodeOffsets[0] + 0x80);
        float ex0 = cur0[0] - rec.currNode[0][32];
        float ey0 = cur0[1] - rec.currNode[0][33];
        float ez0 = cur0[2] - rec.currNode[0][34];
        float devSq0 = ex0 * ex0 + ey0 * ey0 + ez0 * ez0;
        if (devSq0 < 2500.0f && isfinite(devSq0)) {
            float dx0 = rec.currNode[0][32] - rec.prevNode[0][32];
            float dy0 = rec.currNode[0][33] - rec.prevNode[0][33];
            float dz0 = rec.currNode[0][34] - rec.prevNode[0][34];
            float distSq0 = dx0 * dx0 + dy0 * dy0 + dz0 * dz0;
            float rdx0 = rec.currNode[0][35] - rec.prevNode[0][35];
            float rdy0 = rec.currNode[0][36] - rec.prevNode[0][36];
            float rdz0 = rec.currNode[0][37] - rec.prevNode[0][37];
            float rotSq0 = rdx0 * rdx0 + rdy0 * rdy0 + rdz0 * rdz0;
            float smoothSq = rec.smoothOffset[0] * rec.smoothOffset[0] +
                             rec.smoothOffset[1] * rec.smoothOffset[1] +
                             rec.smoothOffset[2] * rec.smoothOffset[2];
            if ((distSq0 > 1e-8f || rotSq0 > 1e-8f || smoothSq > 1e-8f) && distSq0 < 400.0f && isfinite(distSq0)) {
                int nNodes = GetActiveVehicleNodeCount(p);
                if (nNodes > rec.nodeCount) nNodes = rec.nodeCount;
                for (int k = 0; k < nNodes; k++) {
                    float dx = rec.currNode[k][32] - rec.prevNode[k][32];
                    float dy = rec.currNode[k][33] - rec.prevNode[k][33];
                    float dz = rec.currNode[k][34] - rec.prevNode[k][34];
                    float distSq = dx * dx + dy * dy + dz * dz;
                    if (distSq < 400.0f && isfinite(distSq)) {
                        HeldFrame_InterpolateC3DNode(p + kVehicleNodeOffsets[k], rec.prevNode[k], rec.currNode[k],
                                                     alpha, rec.smoothOffset);
                        moved = true;
                    }
                }
            }
            ApplyWheelAndSteerInterp(p, rec, alpha);
            moved = true;
        }
    }
    return moved;
}

static bool ApplySubTickVisualInterp(BYTE* p, float alpha, bool decaySmooth = true)
{
    if (!p || !isfinite(alpha) || alpha >= 0.999f) return false;
    if (alpha < 0.0f) alpha = 0.0f;
    auto it = g_vehicleInterp.find(p);
    if (it == g_vehicleInterp.end()) return false;
    VehicleInterpRecord& rec = it->second;
    if (rec.currTick != g_interpTick || rec.prevTick + 1 != rec.currTick) return false;

    bool moved = false;
    DiagLog_BeginSafeSeh();
    __try {
        moved = ApplySubTickVisualInterpRaw(p, rec, alpha, decaySmooth);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        moved = false;
    }
    DiagLog_EndSafeSeh();
    return moved;
}

static void SaveLiveVehicleVisualRaw(BYTE* p, LiveVehicleVisualBackup* out)
{
    out->vehicle = p;
    out->id = *(const unsigned*)(p + 0x04);
    int nNodes = GetActiveVehicleNodeCount(p);
    if (nNodes < 0) nNodes = 0;
    if (nNodes > VEH_MAX_NODES) nNodes = VEH_MAX_NODES;
    out->nodeCount = nNodes;
    for (int k = 0; k < nNodes; k++) {
        memcpy(out->nodes[k], p + kVehicleNodeOffsets[k], NODE_SNAPSHOT_FLOATS * sizeof(float));
    }
    out->steer = *(const float*)(p + VEH_STEER_CURRENT_OFFSET);
    out->steerTarget = *(const float*)(p + VEH_STEER_TARGET_OFFSET);
    BYTE* wheelBegin = nullptr;
    int wCount = GetVehicleWheelCountUnsafe(p, &wheelBegin);
    if (wCount > VEH_MAX_WHEELS) wCount = VEH_MAX_WHEELS;
    out->wheelCount = wCount;
    for (int w = 0; w < wCount; w++) {
        out->wheelAngles[w] = *(const float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE);
    }
}

static bool SaveLiveVehicleVisual(BYTE* p, LiveVehicleVisualBackup* out)
{
    if (!p || !out || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        SaveLiveVehicleVisualRaw(p, out);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void RestoreLiveVehicleVisualRaw(const LiveVehicleVisualBackup* b)
{
    if (!b || !b->vehicle) return;
    BYTE* p = b->vehicle;
    if (*(const unsigned*)(p + 0x04) == b->id) {
        int nNodes = GetActiveVehicleNodeCount(p);
        if (nNodes > b->nodeCount) nNodes = b->nodeCount;
        for (int k = 0; k < nNodes; k++) {
            memcpy(p + kVehicleNodeOffsets[k], b->nodes[k], NODE_SNAPSHOT_FLOATS * sizeof(float));
        }
        *(float*)(p + VEH_STEER_CURRENT_OFFSET) = b->steer;
        *(float*)(p + VEH_STEER_TARGET_OFFSET) = b->steerTarget;
        BYTE* wheelBegin = nullptr;
        int wCount = GetVehicleWheelCountUnsafe(p, &wheelBegin);
        if (wCount > b->wheelCount) wCount = b->wheelCount;
        for (int w = 0; w < wCount; w++) {
            *(float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) = b->wheelAngles[w];
        }
    }
}

static void RestoreLiveVehicleVisualsBatchPod(const LiveVehicleVisualBackup* list, size_t n, volatile size_t* ioIdx)
{
    if (!list || n == 0) return;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    volatile size_t cur = idx;
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            RestoreLiveVehicleVisualRaw(&list[idx]);
        }
        *ioIdx = idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
    }
    DiagLog_EndSafeSeh();
}

static bool ApplyInterpWithBackup(BYTE* p, float alpha, bool decaySmooth,
                                  std::vector<LiveVehicleVisualBackup>& outModified)
{
    outModified.emplace_back();
    if (!SaveLiveVehicleVisual(p, &outModified.back())) {
        outModified.pop_back();
        return false;
    }
    if (!ApplySubTickVisualInterp(p, alpha, decaySmooth)) {
        outModified.pop_back();
        return false;
    }
    return true;
}

static void RestoreAllLiveVehicleVisuals(std::vector<LiveVehicleVisualBackup>& list)
{
    const size_t n = list.size();
    volatile size_t idx = 0;
    while (idx < n) {
        RestoreLiveVehicleVisualsBatchPod(list.data(), n, &idx);
    }
    list.clear();
}

struct InterpWorkItem {
    BYTE* p;
    VehicleInterpRecord* rec;
};

static size_t SaveAndApplyInterpBatchPod(const InterpWorkItem* items, size_t n, float alpha,
                                          LiveVehicleVisualBackup* outBackups, volatile size_t* ioIdx,
                                          size_t* ioBackupCount)
{
    if (!items || n == 0 || !outBackups) return 0;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    size_t bCnt = *ioBackupCount;
    volatile size_t cur = idx;
    volatile size_t curBCnt = bCnt;
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            curBCnt = bCnt;
            BYTE* p = items[idx].p;
            SaveLiveVehicleVisualRaw(p, &outBackups[bCnt]);
            if (ApplySubTickVisualInterpRaw(p, *items[idx].rec, alpha, true)) {
                bCnt++;
            }
        }
        *ioIdx = idx;
        *ioBackupCount = bCnt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioBackupCount = curBCnt;
    }
    DiagLog_EndSafeSeh();
    return *ioBackupCount;
}

static size_t CollectVehicleGraphStepBatchPod(const BYTE* const* srcList, size_t n,
                                              BYTE** outCand, size_t maxCand,
                                              volatile size_t* ioIdx, size_t* ioCandCount)
{
    if (!srcList || n == 0 || !outCand || maxCand == 0) return *ioCandCount;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    size_t outN = *ioCandCount;
    volatile size_t cur = idx;
    volatile size_t curOut = outN;
    const unsigned kVecOffsets[2] = { VEH_CHILDREN_BEGIN_OFFSET, VEH_CARRIED_BEGIN_OFFSET };
    __try {
        for (; idx < n && outN < maxCand; ++idx) {
            cur = idx;
            curOut = outN;
            const BYTE* target = srcList[idx];
            if (!target || !IsPointerLikeX64((uint64_t)(uintptr_t)target)) continue;
            BYTE* parent = *(BYTE* const*)(target + VEH_PARENT_PTR_OFFSET);
            if (parent && IsPointerLikeX64((uint64_t)(uintptr_t)parent) && outN < maxCand) {
                outCand[outN++] = parent;
            }
            BYTE* link = *(BYTE* const*)(target + VEH_LINK_PTR_OFFSET);
            if (link && IsPointerLikeX64((uint64_t)(uintptr_t)link) && outN < maxCand) {
                outCand[outN++] = link;
            }
            for (unsigned off : kVecOffsets) {
                BYTE* const* begin = *(BYTE* const* const*)(target + off);
                BYTE* const* end   = *(BYTE* const* const*)(target + off + 8);
                if (begin && end > begin && ((uintptr_t)end - (uintptr_t)begin) % sizeof(BYTE*) == 0 &&
                    (size_t)(end - begin) <= 1024 && IsPointerLikeX64((uint64_t)(uintptr_t)begin)) {
                    for (BYTE* const* it = begin; it < end && outN < maxCand; ++it) {
                        BYTE* child = *it;
                        if (child && IsPointerLikeX64((uint64_t)(uintptr_t)child)) {
                            outCand[outN++] = child;
                        }
                    }
                }
            }
        }
        *ioIdx = idx;
        *ioCandCount = outN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioCandCount = curOut;
    }
    DiagLog_EndSafeSeh();
    return *ioCandCount;
}

static size_t CollectVisibleSeedsBatchPod(const BYTE* const* list, size_t n,
                                          BYTE** outSeeds, size_t maxSeeds,
                                          volatile size_t* ioIdx, size_t* ioOutCount)
{
    if (!list || n == 0 || !outSeeds || maxSeeds == 0) return *ioOutCount;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    size_t outN = *ioOutCount;
    volatile size_t cur = idx;
    volatile size_t curOut = outN;
    __try {
        for (; idx < n && outN < maxSeeds; ++idx) {
            cur = idx;
            curOut = outN;
            BYTE* p = const_cast<BYTE*>(list[idx]);
            if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) continue;
            BYTE visible = *(const BYTE*)(p + VEH_RENDER_VISIBLE_OFFSET);
            BYTE linked = 0;
            BYTE* link = *(BYTE* const*)(p + VEH_LINK_PTR_OFFSET);
            if (link && IsPointerLikeX64((uint64_t)(uintptr_t)link)) {
                linked = *(const BYTE*)(link + VEH_RENDER_VISIBLE_OFFSET);
            } else {
                link = nullptr;
            }
            if (visible || linked) {
                outSeeds[outN++] = p;
                if (link && outN < maxSeeds) outSeeds[outN++] = link;
            }
        }
        *ioIdx = idx;
        *ioOutCount = outN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioOutCount = curOut;
    }
    DiagLog_EndSafeSeh();
    return *ioOutCount;
}

static size_t FilterUniqueValidTargetsBatchPod(const BYTE* const* cand, size_t nCand,
                                              FlatPtrSet& seen, std::vector<BYTE*>& targets,
                                              size_t maxTargets, volatile size_t* ioIdx)
{
    if (!cand || nCand == 0) return 0;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    size_t added = 0;
    volatile size_t cur = idx;
    __try {
        for (; idx < nCand && targets.size() < maxTargets; ++idx) {
            cur = idx;
            BYTE* p = const_cast<BYTE*>(cand[idx]);
            if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) continue;
            volatile uint64_t probe0 = *(const volatile uint64_t*)p;
            volatile uint64_t probe1 = *(const volatile uint64_t*)(p + 0x1AD8 - sizeof(uint64_t));
            (void)probe0;
            (void)probe1;
            if (seen.Insert(p)) {
                targets.push_back(p);
                added++;
            }
        }
        *ioIdx = idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
    }
    DiagLog_EndSafeSeh();
    return added;
}

static void CommitValidCandTargets(const std::vector<BYTE*>& cand, FlatPtrSet& seen,
                                   std::vector<BYTE*>& targets, size_t maxTargets)
{
    volatile size_t idx = 0;
    const size_t n = cand.size();
    while (idx < n && targets.size() < maxTargets) {
        FilterUniqueValidTargetsBatchPod(cand.data(), n, seen, targets, maxTargets, &idx);
    }
}

static void ExpandVehicleTargetGraph(FlatPtrSet& seen, std::vector<BYTE*>& targets, std::vector<BYTE*>& candScratch)
{
    const size_t maxTargets = (64ull << 20) / 0x1AD8;
    const size_t maxCand = maxTargets * 8;
    size_t processed = 0;
    while (processed < targets.size() && targets.size() < maxTargets) {
        const size_t waveEnd = targets.size();
        const size_t waveN = waveEnd - processed;
        candScratch.resize(std::min(maxCand, waveN * 16 + 64));
        size_t candCount = 0;
        volatile size_t idx = 0;
        while (idx < waveN && candCount < candScratch.size()) {
            CollectVehicleGraphStepBatchPod(targets.data() + processed, waveN,
                                            candScratch.data(), candScratch.size(),
                                            &idx, &candCount);
        }
        processed = waveEnd;
        candScratch.resize(candCount);
        CommitValidCandTargets(candScratch, seen, targets, maxTargets);
    }
}

static void CollectVisibleVehicleGraph(const std::vector<BYTE*>& list, std::vector<BYTE*>& targets,
                                       FlatPtrSet& seenScratch, std::vector<BYTE*>& followRootsScratch,
                                       std::vector<BYTE*>& candScratch)
{
    targets.clear();
    const size_t maxTargets = (64ull << 20) / 0x1AD8;
    seenScratch.Reset(list.size() * 2 + 32);

    candScratch.resize(list.size() * 2 + 64);
    size_t candCount = 0;
    volatile size_t idx = 0;
    while (idx < list.size() && candCount < candScratch.size()) {
        CollectVisibleSeedsBatchPod(list.data(), list.size(),
                                    candScratch.data(), candScratch.size(),
                                    &idx, &candCount);
    }
    CollectFollowCameraVehicles(followRootsScratch);
    for (BYTE* p : followRootsScratch) {
        if (candCount < candScratch.size()) candScratch[candCount++] = p;
        else candScratch.push_back(p);
    }
    for (const auto& h : g_deferredPrepHighlights) {
        if (candCount < candScratch.size()) candScratch[candCount++] = h.vehicle;
        else candScratch.push_back(h.vehicle);
    }
    candScratch.resize(candCount);
    CommitValidCandTargets(candScratch, seenScratch, targets, maxTargets);
    ExpandVehicleTargetGraph(seenScratch, targets, candScratch);
}

static void WriteWorkerFlagsBatchPod(const std::pair<BYTE*, BYTE>* flags, size_t n,
                                     bool clearToZero, volatile size_t* ioIdx)
{
    if (!flags || n == 0) return;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    volatile size_t cur = idx;
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            *(flags[idx].first + VEH_WORKER_SIM_FLAG_OFFSET) = clearToZero ? (BYTE)0 : flags[idx].second;
        }
        *ioIdx = idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
    }
    DiagLog_EndSafeSeh();
}

static bool RestoreVehicleCopySeh(BYTE* p, const BYTE* beforeCopy, size_t bytes, BYTE* curBuf, uint64_t* ioRefusals)
{
    bool ok = false;
    bool refused = false;
    size_t refusalOff = 0;
    DiagLog_BeginSafeSeh();
    __try {
        memcpy(curBuf, p, bytes);
        if (memcmp(curBuf, beforeCopy, bytes) == 0) {
            ok = true;
        } else {
            ok = true;
            for (size_t o = 0; o + 8 <= bytes; o += 8) {
                unsigned long long before = 0, now = 0;
                memcpy(&before, beforeCopy + o, 8);
                memcpy(&now, curBuf + o, 8);
                if (now == before) continue;
                bool inNodes = (o >= 0xD60 && o < 0x1050) || (o >= 0x1080 && o < 0x1140) ||
                               (o >= 0x1318 && o < 0x13D8) || (o >= 0x1480 && o < 0x16F0);
                if (!inNodes && IsHeapPtrCandidate(before) && IsHeapPtrCandidate(now) &&
                    HeapPointer(before) && HeapPointer(now)) {
                    refused = true;
                    refusalOff = o;
                    ok = false;
                    if (ioRefusals) (*ioRefusals)++;
                } else {
                    *(unsigned long long*)(p + o) = before;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    if (refused) {
        Logf("republicmp  HELD FRAME: vehicle pointer changed at +0x%zX - probe refused", refusalOff);
    }
    return ok;
}

static void SnapshotVehicleCopiesBatchPod(BYTE* const* targets, size_t n, size_t bytes,
                                          BYTE** outValidTargets, BYTE* outCopies,
                                          volatile size_t* ioIdx, size_t* ioValidCount)
{
    if (!targets || n == 0) return;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    size_t vCnt = *ioValidCount;
    volatile size_t cur = idx;
    volatile size_t curV = vCnt;
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            curV = vCnt;
            BYTE* t = targets[idx];
            memcpy(outCopies + vCnt * bytes, t, bytes);
            outValidTargets[vCnt++] = t;
        }
        *ioIdx = idx;
        *ioValidCount = vCnt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioValidCount = curV;
    }
    DiagLog_EndSafeSeh();
}

static void ReadAndClearWorkerFlagsBatchPod(BYTE* const* ptrs, size_t n,
                                            std::pair<BYTE*, BYTE>* outFlags,
                                            volatile size_t* ioIdx, size_t* ioFlagCount)
{
    if (!ptrs || n == 0 || !outFlags) return;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    size_t fCnt = *ioFlagCount;
    volatile size_t cur = idx;
    volatile size_t curF = fCnt;
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            curF = fCnt;
            BYTE* p = ptrs[idx];
            BYTE f = *(p + VEH_WORKER_SIM_FLAG_OFFSET);
            if (f) {
                *(p + VEH_WORKER_SIM_FLAG_OFFSET) = 0;
                outFlags[fCnt++] = { p, f };
            }
        }
        *ioIdx = idx;
        *ioFlagCount = fCnt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioFlagCount = curF;
    }
    DiagLog_EndSafeSeh();
}

static void RestoreVehicleCopiesBatchPod(BYTE* const* targets, const BYTE* beforeCopies,
                                         size_t n, size_t bytes, BYTE* curBuf,
                                         volatile size_t* ioIdx, size_t* outHeapCheckIdx,
                                         bool* outHadFault)
{
    if (!targets || !beforeCopies || n == 0 || !curBuf) return;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    volatile size_t cur = idx;
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            BYTE* p = targets[idx];
            const BYTE* beforeCopy = beforeCopies + idx * bytes;
            if (memcmp(p, beforeCopy, bytes) == 0) continue;
            memcpy(curBuf, p, bytes);
            for (size_t o = 0; o + 8 <= bytes; o += 8) {
                unsigned long long before = 0, now = 0;
                memcpy(&before, beforeCopy + o, 8);
                memcpy(&now, curBuf + o, 8);
                if (now == before) continue;
                bool inNodes = (o >= 0xD60 && o < 0x1050) || (o >= 0x1080 && o < 0x1140) ||
                               (o >= 0x1318 && o < 0x13D8) || (o >= 0x1480 && o < 0x16F0);
                if (!inNodes && IsHeapPtrCandidate(before) && IsHeapPtrCandidate(now)) {
                    *outHeapCheckIdx = idx;
                    *ioIdx = idx + 1u;
                    DiagLog_EndSafeSeh();
                    return;
                }
                *(unsigned long long*)(p + o) = before;
            }
        }
        *ioIdx = idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outHadFault = true;
        *ioIdx = cur + 1u;
    }
    DiagLog_EndSafeSeh();
}

class VehicleGuard {
    static const size_t Bytes = 0x1AD8;
    std::vector<BYTE*> targets_;
    std::vector<BYTE> copies_;
    std::vector<std::pair<BYTE*, BYTE>> flags_;
    std::vector<LiveVehicleVisualBackup> visualBackups_;
    std::vector<BYTE*> listScratch_;
    std::vector<BYTE*> followRootsScratch_;
    std::vector<BYTE*> attachedScratch_;
    std::vector<BYTE*> validTargetsScratch_;
    std::vector<BYTE*> flagCandidatesScratch_;
    std::vector<InterpWorkItem> interpWorkScratch_;
    std::vector<BYTE>  vehBufScratch_;
    FlatPtrSet seenScratch_;
    FlatPtrSet flagSeenScratch_;

    void SnapshotCopies()
    {
        const size_t n = targets_.size();
        validTargetsScratch_.resize(n);
        copies_.resize(n * Bytes);
        volatile size_t idx = 0;
        size_t validCount = 0;
        while (idx < n) {
            SnapshotVehicleCopiesBatchPod(targets_.data(), n, Bytes,
                                          validTargetsScratch_.data(), copies_.data(),
                                          &idx, &validCount);
        }
        validTargetsScratch_.resize(validCount);
        copies_.resize(validCount * Bytes);
        targets_.swap(validTargetsScratch_);
    }

    void SuppressFlags(const std::vector<BYTE*>& list)
    {
        flagSeenScratch_.Reset(list.size() + targets_.size() + 16);
        flagCandidatesScratch_.clear();
        flagCandidatesScratch_.reserve(list.size() + targets_.size());
        for (BYTE* p : list) {
            if (p && IsPointerLikeX64((uint64_t)(uintptr_t)p) && flagSeenScratch_.Insert(p)) {
                flagCandidatesScratch_.push_back(p);
            }
        }
        for (BYTE* p : targets_) {
            if (p && IsPointerLikeX64((uint64_t)(uintptr_t)p) && flagSeenScratch_.Insert(p)) {
                flagCandidatesScratch_.push_back(p);
            }
        }
        const size_t n = flagCandidatesScratch_.size();
        flags_.resize(n);
        volatile size_t idx = 0;
        size_t flagCount = 0;
        while (idx < n) {
            ReadAndClearWorkerFlagsBatchPod(flagCandidatesScratch_.data(), n, flags_.data(), &idx, &flagCount);
        }
        flags_.resize(flagCount);
    }

    static void SafeWriteWorkerFlags(const std::pair<BYTE*, BYTE>* flags, size_t count, bool clearToZero)
    {
        volatile size_t idx = 0;
        while (idx < count) {
            WriteWorkerFlagsBatchPod(flags, count, clearToZero, &idx);
        }
    }

public:
    bool Begin(BYTE* listAt, float alpha)
    {
        targets_.clear(); flags_.clear(); visualBackups_.clear();
        if (!ReadEngineVehicleList(listAt, listScratch_)) return false;

        CollectVisibleVehicleGraph(listScratch_, targets_, seenScratch_, followRootsScratch_, attachedScratch_);
        SnapshotCopies();
        SuppressFlags(listScratch_);

        g_hfDiag.lastTargetsCount = (unsigned)targets_.size();
        g_hfDiag.lastAlpha = alpha;
        size_t interpCount = 0;
        if (alpha >= 0.0f && alpha < 0.999f && isfinite(alpha)) {
            const float clampedAlpha = (alpha < 0.0f) ? 0.0f : alpha;
            interpWorkScratch_.clear();
            interpWorkScratch_.reserve(targets_.size());
            for (BYTE* p : targets_) {
                if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) continue;
                auto it = g_vehicleInterp.find(p);
                if (it == g_vehicleInterp.end()) continue;
                VehicleInterpRecord& rec = it->second;
                if (rec.currTick != g_interpTick || rec.prevTick + 1 != rec.currTick) continue;
                interpWorkScratch_.push_back({ p, &rec });
            }
            const size_t wn = interpWorkScratch_.size();
            visualBackups_.resize(wn);
            volatile size_t idx = 0;
            while (idx < wn) {
                SaveAndApplyInterpBatchPod(interpWorkScratch_.data(), wn, clampedAlpha,
                                           visualBackups_.data(), &idx, &interpCount);
            }
            visualBackups_.resize(interpCount);
        }
        g_hfDiag.lastInterpolatedCount = (unsigned)interpCount;
        return true;
    }

    bool End()
    {
        SafeWriteWorkerFlags(flags_.data(), flags_.size(), false);
        flags_.clear();
        RestoreAllLiveVehicleVisuals(visualBackups_);

        if (vehBufScratch_.size() < Bytes) vehBufScratch_.resize(Bytes);
        bool ok = true;
        const size_t n = targets_.size();
        volatile size_t idx = 0;
        while (idx < n) {
            size_t heapCheckIdx = SIZE_MAX;
            bool hadFault = false;
            RestoreVehicleCopiesBatchPod(targets_.data(), copies_.data(), n, Bytes,
                                         vehBufScratch_.data(), &idx, &heapCheckIdx, &hadFault);
            if (hadFault) ok = false;
            if (heapCheckIdx != SIZE_MAX) {
                if (!RestoreVehicleCopySeh(targets_[heapCheckIdx], copies_.data() + heapCheckIdx * Bytes, Bytes,
                                           vehBufScratch_.data(), &g_hfDiag.restoreRefusals)) {
                    ok = false;
                }
            }
        }
        return ok;
    }
};

typedef void (*SubmitVehHighlightFn)(void* vehicle, unsigned char a2, unsigned char a3, const float* colorRgba, unsigned char flag);

static void CallNativeVehicleHighlight(void* vehicle, unsigned char a2, unsigned char a3,
                                       const float* colorRgba, unsigned char flag, bool isExtraPass)
{
    if (!vehicle) return;
    if (g_testHighlightCb) {
        g_testHighlightCb((BYTE*)vehicle, isExtraPass, g_testHighlightUd);
        return;
    }
    if (!g_exeBase) return;
    unsigned rva = isExtraPass ? 0x66EE70u : 0x671350u;
    if (g_exeBase[rva] == 0) return;
    DiagLog_BeginSafeSeh();
    __try {
        ((SubmitVehHighlightFn)(g_exeBase + rva))(vehicle, a2, a3, colorRgba, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void FlushDeferredVehicleHighlights()
{
    if (g_deferredPrepHighlights.empty()) return;
    std::vector<DeferredVehicleHighlight> pending;
    pending.swap(g_deferredPrepHighlights);
    Determinism_PrivateScope privateRng;
    for (const auto& h : pending) {
        CallNativeVehicleHighlight(h.vehicle, h.a2, h.a3, h.hasColor ? h.color : nullptr, h.flag, h.isExtraPass);
    }
}

void HeldFrame_SubmitVehicleHighlight(void* vehicle, unsigned char a2, unsigned char a3,
                                      const float* colorRgba, unsigned char flag, bool isExtraPass)
{
    if (!vehicle) return;
    bool activeSession = (Determinism_IsActive() && Determinism_SessionRunning() &&
                          !Probe_IsRunning() && !Determinism_WorldLoading()) ||
                         (g_testHighlightCb != nullptr);
    if (!activeSession) {
        CallNativeVehicleHighlight(vehicle, a2, a3, colorRgba, flag, isExtraPass);
        return;
    }
    if (Determinism_GetFramePhase() == 1 && !g_hfDiag.inHeldDraw && !g_testHighlightCb) {
        if (g_deferredPrepHighlights.size() < 256) {
            DeferredVehicleHighlight item = {};
            item.vehicle = (BYTE*)vehicle;
            item.a2 = a2;
            item.a3 = a3;
            item.hasColor = (colorRgba != nullptr) && Read(colorRgba, item.color, sizeof(item.color));
            item.flag = flag;
            item.isExtraPass = isExtraPass;
            g_deferredPrepHighlights.push_back(item);
        }
        return;
    }
    if (g_hfDiag.inHeldDraw) {
        Determinism_PrivateScope privateRng;
        CallNativeVehicleHighlight(vehicle, a2, a3, colorRgba, flag, isExtraPass);
        return;
    }
    float alpha = g_hfDiag.lastAlpha;
    std::vector<LiveVehicleVisualBackup> modified;
    if (alpha >= 0.0f && alpha < 0.999f) {
        static std::vector<BYTE*> s_hlTargets, s_hlAttached;
        static FlatPtrSet s_hlSeen;
        s_hlTargets.clear();
        s_hlSeen.Reset(16);
        AddVisibleVehicleTarget((BYTE*)vehicle, s_hlSeen, s_hlTargets);
        ExpandVehicleTargetGraph(s_hlSeen, s_hlTargets, s_hlAttached);
        modified.reserve(s_hlTargets.size());
        for (BYTE* p : s_hlTargets) {
            ApplyInterpWithBackup(p, alpha, false, modified);
        }
    }
    {
        Determinism_PrivateScope privateRng;
        CallNativeVehicleHighlight(vehicle, a2, a3, colorRgba, flag, isExtraPass);
    }
    RestoreAllLiveVehicleVisuals(modified);
}

void HeldFrame_DrawStepVehicles(void* world)
{
    typedef void (*DrawVehiclesFn)(void*);
    DrawVehiclesFn nativeDraw = (DrawVehiclesFn)(g_exeBase + 0x3FA790);
    if (!Determinism_IsActive() || !Determinism_SessionRunning() || Probe_IsRunning() || g_hfDiag.inHeldDraw) {
        nativeDraw(world);
        FlushDeferredVehicleHighlights();
        return;
    }
    HeldFrame_OnSimTick(Determinism_StepCount() + 1);
    float alpha = Determinism_RenderAlpha();
    g_hfDiag.lastAlpha = alpha;
    static std::vector<BYTE*> s_stepList, s_stepTargets, s_stepFollowRoots, s_stepAttached;
    static FlatPtrSet s_stepSeen;
    static std::vector<LiveVehicleVisualBackup> s_stepModified;
    s_stepModified.clear();
    if (alpha >= 0.0f && alpha < 0.999f && ReadEngineVehicleList((const void*)(g_exeBase + 0x9E7720), s_stepList)) {
        CollectVisibleVehicleGraph(s_stepList, s_stepTargets, s_stepSeen, s_stepFollowRoots, s_stepAttached);
        s_stepModified.reserve(s_stepTargets.size());
        for (BYTE* p : s_stepTargets) {
            ApplyInterpWithBackup(p, alpha, true, s_stepModified);
        }
        g_hfDiag.lastTargetsCount = (unsigned)s_stepTargets.size();
        g_hfDiag.lastInterpolatedCount = (unsigned)s_stepModified.size();
    }
    nativeDraw(world);
    FlushDeferredVehicleHighlights();
    RestoreAllLiveVehicleVisuals(s_stepModified);
}

static void CollectFollowCameraSubgraph(std::vector<BYTE*>& targets)
{
    targets.clear();
    static std::vector<BYTE*> s_camRoots, s_camAttached;
    static FlatPtrSet s_camSeen;
    CollectFollowCameraVehicles(s_camRoots);
    if (s_camRoots.empty()) return;
    s_camSeen.Reset(s_camRoots.size() * 2 + 16);
    for (BYTE* p : s_camRoots) {
        AddVisibleVehicleTarget(p, s_camSeen, targets);
    }
    ExpandVehicleTargetGraph(s_camSeen, targets, s_camAttached);
}

static void SaveCameraStepState(BYTE* camMgr, float fps)
{
    g_savedCamMgrPtr = camMgr;
    g_savedRideVeh = *(BYTE**)(g_savedCamMgr + CAM_MGR_RIDE_VEH_PTR_OFF);
    g_savedFollowWin = *(BYTE**)(g_savedCamMgr + CAM_MGR_FOLLOW_WIN_PTR_OFF);
    g_savedCamFps = (fps > 1.0f && isfinite(fps)) ? fps : 60.0f;
    g_hasSavedCameraNode = g_exeBase &&
                           Read(g_exeBase + CAMERA_RVA, g_savedCameraNode, C3D_NODE_BYTE_SIZE);
    for (int i = 0; i < CAM_MGR_PID_COUNT; ++i) {
        g_savedCamPidPtrs[i] = nullptr;
        BYTE* pidPtr = *(BYTE**)(g_savedCamMgr + CAM_MGR_PID_PTRS_OFFSET + (size_t)i * sizeof(BYTE*));
        if (pidPtr && IsPointerLikeX64((uint64_t)(uintptr_t)pidPtr) &&
            Read(pidPtr, g_savedCamPidData[i], CAM_MGR_PID_STRUCT_BYTES)) {
            g_savedCamPidPtrs[i] = pidPtr;
        }
    }
    g_hasCamSnapshot = true;
}

static bool RestoreCameraStepState(BYTE* camMgr)
{
    if (!SafeMemcpyPod(camMgr, g_savedCamMgr, CAM_MGR_STRUCT_BYTES)) return false;
    if (g_hasSavedCameraNode && g_exeBase) {
        SafeMemcpyPod(g_exeBase + CAMERA_RVA, g_savedCameraNode, C3D_NODE_BYTE_SIZE);
    }
    for (int i = 0; i < CAM_MGR_PID_COUNT; ++i) {
        BYTE* pidPtr = g_savedCamPidPtrs[i];
        if (!pidPtr) continue;
        BYTE* livePidPtr = *(BYTE**)(camMgr + CAM_MGR_PID_PTRS_OFFSET + (size_t)i * sizeof(BYTE*));
        if (livePidPtr == pidPtr && IsPointerLikeX64((uint64_t)(uintptr_t)pidPtr)) {
            SafeMemcpyPod(pidPtr, g_savedCamPidData[i], CAM_MGR_PID_STRUCT_BYTES);
        }
    }
    return true;
}

void HeldFrame_SaveCameraStepStateForTest(BYTE* camMgr, float fps)
{
    if (!camMgr || !Read(camMgr, g_savedCamMgr, CAM_MGR_STRUCT_BYTES)) return;
    SaveCameraStepState(camMgr, fps);
}

bool HeldFrame_RestoreCameraStepStateForTest(BYTE* camMgr)
{
    if (!camMgr || !g_hasCamSnapshot) return false;
    g_hasCamSnapshot = false;
    return RestoreCameraStepState(camMgr);
}

void HeldFrame_BeginCameraPrepare(float fps)
{
    g_camPrepModified.clear();
    g_hasCamSnapshot = false;
    g_hasSavedCameraNode = false;
    if (!Determinism_IsActive() || !Determinism_SessionRunning() || Probe_IsRunning() || Determinism_WorldLoading()) {
        return;
    }
    HeldFrame_StepBuildingAnimations(fps);
    std::vector<BYTE*> targets;
    CollectFollowCameraSubgraph(targets);
    if (targets.empty()) return;

    if (Determinism_IsStepFrame()) {
        BYTE* camMgr = nullptr;
        if (Read(g_exeBase + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF, &camMgr, sizeof(camMgr)) &&
            camMgr && IsPointerLikeX64((uint64_t)(uintptr_t)camMgr) &&
            Read(camMgr, g_savedCamMgr, CAM_MGR_STRUCT_BYTES)) {
            SaveCameraStepState(camMgr, fps);
        }
        return;
    }

    float alpha = Determinism_RenderAlpha();
    if (alpha >= 0.0f && alpha < 0.999f) {
        g_camPrepModified.reserve(targets.size());
        for (BYTE* p : targets) {
            ApplyInterpWithBackup(p, alpha, false, g_camPrepModified);
        }
    }
}

void HeldFrame_EndCameraPrepare()
{
    RestoreAllLiveVehicleVisuals(g_camPrepModified);
}

static void SafeCallCameraMgrUpdate(BYTE* camMgr)
{
    DiagLog_BeginSafeSeh();
    __try {
        ((bool (*)(void*))(g_exeBase + RVA_CAMERA_MGR_UPDATE))(camMgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void HeldFrame_UpdateCameraAfterSimStep()
{
    if (!g_hasCamSnapshot) return;
    g_hasCamSnapshot = false;
    if (!Determinism_IsActive() || !Determinism_SessionRunning() || Probe_IsRunning() || Determinism_WorldLoading()) {
        return;
    }
    BYTE* camMgr = nullptr;
    if (!Read(g_exeBase + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF, &camMgr, sizeof(camMgr)) ||
        camMgr != g_savedCamMgrPtr || !camMgr) {
        return;
    }
    BYTE followActive = 0;
    BYTE* curRideVeh = nullptr;
    BYTE* curFollowWin = nullptr;
    if (!Read(camMgr + CAM_MGR_FOLLOW_ACTIVE_OFF, &followActive, sizeof(followActive)) || !followActive ||
        !Read(camMgr + CAM_MGR_RIDE_VEH_PTR_OFF, &curRideVeh, sizeof(curRideVeh)) ||
        !Read(camMgr + CAM_MGR_FOLLOW_WIN_PTR_OFF, &curFollowWin, sizeof(curFollowWin)) ||
        curRideVeh != g_savedRideVeh || curFollowWin != g_savedFollowWin) {
        return;
    }

    std::vector<BYTE*> targets;
    CollectFollowCameraSubgraph(targets);
    if (targets.empty()) return;

    if (!RestoreCameraStepState(camMgr)) return;

    float alpha = Determinism_RenderAlpha();
    std::vector<LiveVehicleVisualBackup> modified;
    if (alpha >= 0.0f && alpha < 0.999f) {
        modified.reserve(targets.size());
        for (BYTE* p : targets) {
            ApplyInterpWithBackup(p, alpha, false, modified);
        }
    }

    if (g_exeBase && g_exeBase[RVA_CAMERA_MGR_UPDATE] != 0) {
        BYTE* timer = g_exeBase + 0x9D4EE0;
        BYTE savedTimer[13];
        memcpy(savedTimer, timer, sizeof(savedTimer));
        *(float*)timer = g_savedCamFps;
        *(float*)(timer + 4) = g_savedCamFps;
        *(int*)(timer + 8) = (int)g_savedCamFps;
        timer[12] = 0;
        {
            Determinism_PrivateScope privateRng;
            SafeCallCameraMgrUpdate(camMgr);
        }
        memcpy(timer, savedTimer, sizeof(savedTimer));
    }

    RestoreAllLiveVehicleVisuals(modified);
}

bool HeldFrame_RunVehicleGuardForTest(BYTE* listAt, float alpha, void (*modifyFn)(void* ud), void* ud)
{
    VehicleGuard guard;
    if (!guard.Begin(listAt, alpha)) {
        g_hfDiag.guardFallbacks++;
        return false;
    }
    if (modifyFn) {
        modifyFn(ud);
    }
    return guard.End();
}

bool HeldFrame_Verify()
{
    struct Site { unsigned rva; unsigned char bytes[24]; size_t size; };
    static const Site sites[] = {
        { 0x83D040, { 0x48,0x8b,0xc4,0x48,0x89,0x48,0x08,0x55,0x53,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8d,0xa8,0x98,0xfc,0xff,0xff }, 22 },
        { 0x25952E, { 0x48,0x8d,0x0d,0xdb,0xb9,0x77,0x00,0xe8 }, 8 },
        { 0x3FA847, { 0x49,0x8b,0x86,0x18,0x28,0x01,00,0x49,0x2b,0x86,0x10,0x28,0x01,00 }, 14 },
        { 0x3FA953, { 0x41,0x0f,0xb6,0xd7,0xe8,0xf4,0x69,0x27,0x00 }, 9 },
        { 0x6713FB, { 0x80,0xb9,0xd8,0x13,0x00,0x00,0x00 }, 7 },
        { 0x671408, { 0x48,0x8b,0x89,0x98,0x03,0x00,0x00 }, 7 },
        { 0x671470, { 0x48,0x8b,0x9e,0xa8,0x03,0x00,0x00,0x48,0x2b,0x9e,0xa0 }, 11 },
        { 0x66EE99, { 0x80,0xb9,0xd8,0x13,0x00,0x00,0x00 }, 7 },
        { 0x67C34E, { 0x48,0x8b,0xb1,0xa0,0x04,00,00,0x49,0x8b,0xd8,0x44,0x0f,0xb6,0xe2,0x4c,0x8b,0xf1,0x80,0xbe,0xd8,0x13,0x00,0x00,0x00 }, 24 },
    };
    for (const auto& s : sites) {
        if (memcmp(g_exeBase + s.rva, s.bytes, s.size) != 0) {
            Logf("republicmp  HELD FRAME: unexpected bytes at exe+0x%X", s.rva);
            return false;
        }
    }
    return true;
}

// Early draws the road junctions' signs and traffic lights itself (30c851..30c8b7):
// 228b50(junction manager [world+12808]) rebuilds the manager's list of
// junctions on camera-visible road edges (+50, from the road network's view
// list +aa0), and with the rule +14451 set, 2239d0(junction) queues each one's
// sign or light meshes (41de40). A held frame has no Early, so the lights were
// drawn only on tick frames and blinked; the held frame draws them the same way.
static int g_junctionsChecked = -1;

static bool CallTargets(unsigned site, unsigned target)
{
    BYTE* at = g_exeBase + site;
    int rel = 0;
    memcpy(&rel, at + 1, 4);
    return at[0] == 0xE8 && site + 5 + rel == target;
}

static void DrawJunctions()
{
    if (g_junctionsChecked < 0) {
        static const unsigned char load[7] = { 0x48, 0x8b, 0x8e, 0x08, 0x28, 0x01, 0x00 }; // mov rcx,[rsi+12808]
        g_junctionsChecked = memcmp(g_exeBase + 0x30C7D1, load, sizeof(load)) == 0 && CallTargets(0x30C7D8, 0x228AD0) &&
                             CallTargets(0x30C81D, 0x223950) ? 1 : 0;
        if (!g_junctionsChecked) Logf("republicmp  HELD FRAME: Early's junction drawing differs - junction lights drawn on tick frames only");
    }
    if (!g_junctionsChecked) return;
    BYTE* world = g_exeBase + 0x9D4F10;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* manager = *(BYTE**)(world + 0x12808);
        if (manager) {
            ((void (*)(void*))(g_exeBase + 0x228AD0))(manager);
            if (world[0x14451]) {
                BYTE** begin = *(BYTE***)(manager + 0x50);
                size_t n = begin ? (size_t)(*(BYTE***)(manager + 0x58) - begin) : 0;
                for (size_t i = 0; i < n; i++) {
                    BYTE* junction = (*(BYTE***)(manager + 0x50))[i];
                    if (junction) ((void (*)(void*))(g_exeBase + 0x223950))(junction);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

// Early's building pass (139a70) moves the doors of the buildings on camera:
// 138b90(building, seconds) steps each door's opening (+1238, +1250; read by
// nothing else) toward the vehicles at it and queues the door meshes on the
// frame's render list ([exe+995268]+a8), which every frame empties. The
// simulation now sees every building off the camera (sim_patches.cpp), so the
// pass moves no door; every frame (after Early, or on a held frame) the doors
// of the buildings this PC sees are moved by its own frame time and queued.
#define BUILDINGS_BEGIN_RVA 0x9E6A18
#define BUILDINGS_END_RVA 0x9E6A20
#define ENGINE_RVA 0x9941F0       // engine object: +ff8 models, +ee0 map quadtree
#define FRAGMENT_IMPORT 0x86CFB8  // a model's fragment
#define GETNODE_IMPORT 0x86C560   // the quadtree's node of a fragment (+25: visible)
#define AGENTS_ALL 0x126A8
#define DRAW_AGENTS_RVA 0x83D040
#define AGENT_NEAR_RVA 0x90AF7C
#define CAMERA_RVA 0x9E9C90
#define NODE_POSITION_IMPORT 0x86C7B8   // C3D_NODE::GetPosition
#define CAMERA_ZDISTANCE_IMPORT 0x86D008 // C3D_CAMERA::GetZDistance
static int g_doorsChecked = -1;

static size_t Count(const BYTE* vector, size_t elem)
{
    uintptr_t begin = *(const uintptr_t*)vector, end = *(const uintptr_t*)(vector + 8);
    return begin && end > begin && (end - begin) % elem == 0 ? (end - begin) / elem : 0;
}

struct ViewBuilding {
    BYTE* building;
    float distance;
};
static std::vector<ViewBuilding> g_viewBuildings; // sorted by address
static std::vector<BYTE*> g_nearAgents;

static float ComputeDoorFrameSeconds()
{
    static LARGE_INTEGER s_last = {};
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = s_last.QuadPart && frequency.QuadPart
        ? (float)(now.QuadPart - s_last.QuadPart) / frequency.QuadPart
        : 0.0f;
    s_last = now;
    if (seconds < 0.0f) seconds = 0.0f;
    if (seconds > 0.25f) seconds = 0.25f;
    return seconds;
}

static void EnsureDoorsChecked()
{
    if (g_doorsChecked < 0) {
        static const unsigned char list[14] = { 0x48, 0x8b, 0x1d, 0xe2, 0xbc, 0x85, 0x00, 0x48, 0x81, 0xc3, 0xa8, 0x00, 0x00, 0x00 };
        g_doorsChecked = CallTargets(0x13DB61, 0x138B90) && memcmp(g_exeBase + 0x13957F, list, sizeof(list)) == 0 ? 1 : 0;
        if (!g_doorsChecked) Logf("republicmp  HELD FRAME: Early's door drawing differs - doors drawn on tick frames only");
    }
}

// Single BUILDINGS pass for both DrawDoors (before Filling worker starts) and CollectView.
static size_t ScanBuildingsForDoorsAndView(bool runDoors, float seconds, ViewBuilding* outView, size_t maxView)
{
    if (runDoors) {
        EnsureDoorsChecked();
        if (!g_doorsChecked) runDoors = false;
    }
    const bool collectView = (outView != nullptr && maxView > 0);
    if (!runDoors && !collectView) return 0;

    typedef BYTE* (*FragmentFn)(void* models, int model, int variant);
    typedef BYTE* (*GetNodeFn)(void* tree, int index);
    typedef void* (*PositionFn)(void* node, float* out);
    typedef float (*ZDistanceFn)(void* camera, float* position);
    FragmentFn fragment = *(FragmentFn*)(g_exeBase + FRAGMENT_IMPORT);
    GetNodeFn getNode = *(GetNodeFn*)(g_exeBase + GETNODE_IMPORT);
    PositionFn position = collectView ? *(PositionFn*)(g_exeBase + NODE_POSITION_IMPORT) : nullptr;
    ZDistanceFn zDistance = collectView ? *(ZDistanceFn*)(g_exeBase + CAMERA_ZDISTANCE_IMPORT) : nullptr;
    size_t n = 0;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* engine = *(BYTE**)(g_exeBase + ENGINE_RVA);
        BYTE** it = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
        BYTE** end = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
        for (; engine && it && it < end; ++it) {
            BYTE* b = *it;
            if (!b) continue;
            BYTE* type = *(BYTE**)(b + 0x318);
            bool visible;
            if (type) {
                BYTE* f = fragment(*(void**)(engine + 0xFF8), *(int*)(type + 0x410), *(int*)(b + 0x5F4));
                BYTE* node = f ? getNode(*(void**)(engine + 0xEE0), *(int*)(f + 0x3C)) : nullptr;
                visible = node && node[0x25];
            } else {
                BYTE* render = *(BYTE**)(b + 0x5D0);
                visible = render && render[0x178];
            }
            if (!visible) continue;

            if (runDoors && type && *(float*)(b + 0x604) == 1.0f && *(void**)(b + 0x5D0) == nullptr) {
                if (Count(type + 0xA18, 0x28) || Count(type + 0xA30, 0x30)) {
                    ((void (*)(void*, float))(g_exeBase + 0x138B90))(b, seconds);
                }
            }
            if (collectView && n < maxView) {
                float at[4] = {};
                position(b + 0x320, at);
                outView[n++] = { b, zDistance(g_exeBase + CAMERA_RVA, at) };
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    return n;
}

// The building's camera distance on this PC, or a very large one when it is not seen.
static float ViewDistance(const ViewBuilding* view, size_t n, BYTE* b)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (view[mid].building < b) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && view[lo].building == b ? view[lo].distance : 1.0e30f;
}

static size_t CollectNear(const ViewBuilding* view, size_t nView, BYTE** out, size_t max)
{
    size_t n = 0;
    DiagLog_BeginSafeSeh();
    __try {
        float limit = *(float*)(g_exeBase + AGENT_NEAR_RVA);
        BYTE** begin = *(BYTE***)(g_exeBase + 0x9D4F10 + AGENTS_ALL);
        BYTE** end = *(BYTE***)(g_exeBase + 0x9D4F10 + AGENTS_ALL + 8);
        for (BYTE** it = begin; it && it < end && n < max; ++it) {
            BYTE* a = *it;
            if (!a || a[0x2C] || *(float*)(a + 0x10) > 0.0f) continue;
            BYTE* first = *(BYTE**)(a + 0x38);
            BYTE* second = *(BYTE**)(a + 0x40);
            bool isNear;
            if (first && second) {
                float d2 = ViewDistance(view, nView, second);
                isNear = (ViewDistance(view, nView, first) < 1.0e30f && d2 < limit) || d2 < limit;
            } else {
                BYTE* other = *(BYTE**)(a + 0x598);
                isNear = other && ViewDistance(view, nView, other) < limit;
            }
            if (isNear) out[n++] = a;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    return n;
}

// 83cea0 itself skips a citizen whose buildings are not on the camera (+115a of +38 / +40,
// 83d170; for one at a building +598, such as a stop, also its distance +115c, 83d585), which
// the simulation keeps at 0 / 100000: for the call this PC's seen buildings get its own view,
// and both are put back right after (the simulation does not run meanwhile).
struct KeptView {
    BYTE visible;
    float distance;
};
static std::vector<KeptView> g_keptVisible;

static void Submit(BYTE** list, size_t n, const ViewBuilding* view, size_t nView, KeptView* kept)
{
    BYTE** header[3] = { list, list + n, list + n };
    size_t set = 0;
    DiagLog_BeginSafeSeh();
    __try {
        for (; set < nView; set++) {
            BYTE* b = view[set].building;
            kept[set] = { b[0x115A], *(float*)(b + 0x115C) };
            b[0x115A] = 1;
            *(float*)(b + 0x115C) = view[set].distance;
        }
        ((void (*)(void*, void*))(g_exeBase + DRAW_AGENTS_RVA))(nullptr, header);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        for (size_t i = 0; i < set; i++) {
            view[i].building[0x115A] = kept[i].visible;
            *(float*)(view[i].building + 0x115C) = kept[i].distance;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static bool ListSizes(size_t* buildings, size_t* agents)
{
    *buildings = *agents = 0;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** b = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
        BYTE** e = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
        BYTE** ab = *(BYTE***)(g_exeBase + 0x9D4F10 + AGENTS_ALL);
        BYTE** ae = *(BYTE***)(g_exeBase + 0x9D4F10 + AGENTS_ALL + 8);
        if (b && e > b) *buildings = (size_t)(e - b);
        if (ab && ae > ab) *agents = (size_t)(ae - ab);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void PrepareDoorsAndView(size_t* outAgents, bool* outCanSubmitCitizens)
{
    if (outAgents) *outAgents = 0;
    if (outCanSubmitCitizens) *outCanSubmitCitizens = false;
    const float seconds = ComputeDoorFrameSeconds();
    size_t buildings = 0, agents = 0;
    const bool haveSizes = ListSizes(&buildings, &agents) && agents > 0 && buildings <= 1000000 && agents <= 10000000;
    if (!haveSizes || !buildings) {
        g_viewBuildings.clear();
        ScanBuildingsForDoorsAndView(true, seconds, nullptr, 0);
        return;
    }
    if (outAgents) *outAgents = agents;
    if (outCanSubmitCitizens) *outCanSubmitCitizens = true;
    try {
        if (g_viewBuildings.size() < buildings) g_viewBuildings.resize(buildings);
        size_t nView = ScanBuildingsForDoorsAndView(true, seconds, g_viewBuildings.data(), buildings);
        g_viewBuildings.resize(nView);
    } catch (const std::bad_alloc&) {
        g_viewBuildings.clear();
        if (outCanSubmitCitizens) *outCanSubmitCitizens = false;
    }
}

static void ClearViewCaches()
{
    g_viewBuildings.clear();
    g_nearAgents.clear();
    g_keptVisible.clear();
}

void HeldFrame_GetViewCacheSizesForTest(size_t* outView, size_t* outNear, size_t* outKept)
{
    if (outView) *outView = g_viewBuildings.size();
    if (outNear) *outNear = g_nearAgents.size();
    if (outKept) *outKept = g_keptVisible.size();
}

static void SubmitNearCitizensFromView(size_t agents)
{
    if (!agents) return;
    try {
        const size_t nView = g_viewBuildings.size();
        std::sort(g_viewBuildings.begin(), g_viewBuildings.end(),
                  [](const ViewBuilding& x, const ViewBuilding& y) { return x.building < y.building; });
        if (g_nearAgents.size() < agents) g_nearAgents.resize(agents);
        size_t nNear = CollectNear(g_viewBuildings.data(), nView, g_nearAgents.data(), agents);
        if (g_keptVisible.size() < nView) g_keptVisible.resize(nView);
        if (nNear) Submit(g_nearAgents.data(), nNear, g_viewBuildings.data(), nView, g_keptVisible.data());
    } catch (const std::bad_alloc&) {
    }
}

static float ComputeFallbackFromFields(float typeSpeed, float effCap, float util, float prod)
{
    const bool hasEff = isfinite(effCap) && effCap >= 0.0001f;
    const bool hasUtil = isfinite(util) && util >= 0.0001f;
    const bool hasProd = isfinite(prod) && prod >= 0.0001f;
    if (!hasEff && !hasUtil && !hasProd) return 0.0f;
    const float factor = (hasUtil && util <= 1.0f) ? util : ((hasEff && effCap <= 1.0f) ? effCap : 1.0f);
    return (typeSpeed * factor) / DETERMINISM_FIXED_FPS;
}

static bool ReadBuildingsBoundsPod(BYTE* exeBase, BYTE*** outIt, BYTE*** outEnd, int* outBucketCounter, bool readBucket)
{
    *outIt = nullptr;
    *outEnd = nullptr;
    if (outBucketCounter) *outBucketCounter = 0;
    if (!exeBase) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** it = *(BYTE***)(exeBase + BUILDINGS_BEGIN_RVA);
        BYTE** end = *(BYTE***)(exeBase + BUILDINGS_END_RVA);
        if (it && end > it && (size_t)(end - it) <= 1000000u) {
            *outIt = it;
            *outEnd = end;
            if (readBucket && outBucketCounter) {
                *outBucketCounter = *(const int*)(exeBase + BLD_PASS_BUCKET_COUNTER_RVA);
            }
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void CollectAnimatedBuildingsBatchPod(
    BYTE* const* it, size_t count, volatile size_t* ioIdx,
    AnimBldSnapshot* outBuf, size_t outCap, size_t* ioWritten, bool readFallback)
{
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    volatile size_t cur = idx;
    size_t written = *ioWritten;
    __try {
        for (; idx < count && written < outCap; ++idx) {
            cur = idx;
            BYTE* b = it[idx];
            if (!b) continue;
            // Check submesh pointer first so non-animated buildings exit in 1 read.
            const BYTE* mesh = *(const BYTE* const*)(b + BLD_ANIM_SUBMESH_OFFSET);
            if (!mesh) continue;
            const float progress = *(const float*)(b + BLD_PROGRESS_OFFSET);
            const void* site = *(const void* const*)(b + BLD_CONSTRUCTION_SITE_OFFSET);
            const BYTE* type = *(const BYTE* const*)(b + BLD_TYPE_OFFSET);
            if (progress < 1.0f || site != nullptr || !type) continue;
            const BYTE* typeMesh = *(const BYTE* const*)(type + BLD_TYPE_ANIM_SUBMESH_OFFSET);
            const float typeSpeed = *(const float*)(type + BLD_TYPE_ANIM_SPEED_OFFSET);
            const BYTE* animCtrl = *(const BYTE* const*)(mesh + MESH_ANIM_CTRL_OFFSET);
            if (typeMesh != mesh || !isfinite(typeSpeed) || typeSpeed <= 0.0f || !animCtrl) continue;
            const int32_t maxFramesInt = *(const int32_t*)(animCtrl + ANIM_CTRL_MAX_FRAMES_OFFSET);
            if (maxFramesInt <= 0 || maxFramesInt > 100000) continue;
            AnimBldSnapshot snap = {};
            snap.b = b;
            snap.listIndex = (uint32_t)idx;
            snap.maxFrames = (float)maxFramesInt;
            snap.typeSpeed = typeSpeed;
            snap.curFrame = *(const float*)(b + BLD_ANIM_FRAME_OFFSET);
            if (readFallback) {
                const float effCap = *(const float*)(b + BLD_EFFECTIVE_CAP_FLOAT_OFFSET);
                const float util = *(const float*)(b + BLD_UTILIZATION_RATIO_OFFSET);
                const float prod = *(const float*)(b + BLD_PRODUCED_OFFSET);
                snap.fallbackStep = ComputeFallbackFromFields(typeSpeed, effCap, util, prod);
            }
            outBuf[written++] = snap;
        }
        *ioIdx = idx;
        *ioWritten = written;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioWritten = written;
    }
    DiagLog_EndSafeSeh();
}

static void WriteBuildingAnimFramesBatchPod(const AnimBldWrite* writes, size_t n, volatile size_t* ioIdx)
{
    if (!writes || n == 0) return;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    volatile size_t cur = idx;
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            *(float*)(writes[idx].b + BLD_ANIM_FRAME_OFFSET) = writes[idx].frame;
        }
        *ioIdx = idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
    }
    DiagLog_EndSafeSeh();
}

static bool CopyBuildingPtrRangePod(BYTE* const* it, size_t count, BYTE** outPtrs)
{
    if (!it || !outPtrs || count == 0) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        memcpy(outPtrs, it, count * sizeof(BYTE*));
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static size_t CollectAllAnimatedBuildings(bool readFallback, bool readBucket, BYTE*** outIt, size_t* outCount, int* outBucketCounter)
{
    BYTE** it = nullptr;
    BYTE** end = nullptr;
    if (!ReadBuildingsBoundsPod(g_exeBase, &it, &end, outBucketCounter, readBucket)) return 0;
    const size_t count = (size_t)(end - it);
    if (outIt) *outIt = it;
    if (outCount) *outCount = count;
    if (g_animReadScratch.size() < count) g_animReadScratch.resize(count);
    volatile size_t idx = 0;
    size_t written = 0;
    while (idx < count) {
        CollectAnimatedBuildingsBatchPod(
            it, count, &idx, g_animReadScratch.data(), count, &written, readFallback);
    }
    return written;
}

static void FlushBuildingAnimWrites()
{
    const size_t n = g_animWriteScratch.size();
    volatile size_t idx = 0;
    while (idx < n) {
        WriteBuildingAnimFramesBatchPod(g_animWriteScratch.data(), n, &idx);
    }
    g_animWriteScratch.clear();
}

float HeldFrame_StepBuildingAnimFrame(float currentFrame, float maxFrames, float stepPerTick, float fps, bool paused)
{
    if (!isfinite(maxFrames) || maxFrames <= 0.0f) return 0.0f;
    float safeCurrent = (isfinite(currentFrame) && currentFrame >= 0.0f) ? currentFrame : 0.0f;
    if (safeCurrent >= maxFrames) {
        safeCurrent = fmodf(safeCurrent, maxFrames);
    }
    if (paused || !isfinite(stepPerTick) || stepPerTick <= 0.0f || !isfinite(fps) || fps <= 0.0f) {
        return safeCurrent;
    }
    float clampedFps = fps < 4.0f ? 4.0f : (fps > 1000.0f ? 1000.0f : fps);
    float ticksInFrame = DETERMINISM_FIXED_FPS / clampedFps;
    float next = safeCurrent + stepPerTick * ticksInFrame;
    if (next >= maxFrames) {
        next = fmodf(next, maxFrames);
    }
    return next;
}

void HeldFrame_BeforeEarly()
{
    if (!g_exeBase || !Determinism_IsActive() || Probe_IsRunning() || Determinism_WorldLoading()) return;
    const size_t written = CollectAllAnimatedBuildings(false, false, nullptr, nullptr, nullptr);
    g_animWriteScratch.clear();
    g_animWriteScratch.reserve(written);
    for (size_t i = 0; i < written; ++i) {
        const AnimBldSnapshot& snap = g_animReadScratch[i];
        float curFrame = snap.curFrame;
        if (!isfinite(curFrame) || curFrame < 0.0f || curFrame >= snap.maxFrames) {
            curFrame = (isfinite(curFrame) && curFrame > 0.0f) ? fmodf(curFrame, snap.maxFrames) : 0.0f;
        }
        BuildingAnimState& st = g_buildingAnimState[snap.b];
        st.visualFrame = curFrame;
        g_animWriteScratch.push_back({ snap.b, 0.0f });
    }
    FlushBuildingAnimWrites();
}

static void RestoreBuildingAnimationsAfterEarly()
{
    if (!g_exeBase || !Determinism_IsActive() || Probe_IsRunning() || Determinism_WorldLoading()) return;
    BYTE** it = nullptr;
    size_t count = 0;
    int bucketCounter = 0;
    const size_t liveAnimated = CollectAllAnimatedBuildings(false, true, &it, &count, &bucketCounter);
    const int activeBucket = bucketCounter % 3;
    g_animWriteScratch.clear();
    g_animWriteScratch.reserve(liveAnimated);
    for (size_t i = 0; i < liveAnimated; ++i) {
        const AnimBldSnapshot& snap = g_animReadScratch[i];
        const float delta3 = snap.curFrame;
        BuildingAnimState& st = g_buildingAnimState[snap.b];
        const bool inBucket = (((int)snap.listIndex % 3) == activeBucket);
        if (inBucket || (isfinite(delta3) && delta3 > 0.0f)) {
            if (isfinite(delta3) && delta3 >= 0.0f && delta3 < snap.maxFrames) {
                st.stepPerTick = delta3 / 3.0f;
                st.hasMeasuredStep = true;
            }
        }
        g_animWriteScratch.push_back({ snap.b, st.visualFrame });
    }
    FlushBuildingAnimWrites();
    if (g_buildingAnimState.size() > liveAnimated + 64 && it && count > 0) {
        g_animStillScratch.resize(count);
        if (CopyBuildingPtrRangePod(it, count, g_animStillScratch.data())) {
            std::sort(g_animStillScratch.begin(), g_animStillScratch.end(), std::less<BYTE*>{});
            for (auto mapIt = g_buildingAnimState.begin(); mapIt != g_buildingAnimState.end();) {
                if (!std::binary_search(g_animStillScratch.begin(), g_animStillScratch.end(),
                                        mapIt->first, std::less<BYTE*>{})) {
                    mapIt = g_buildingAnimState.erase(mapIt);
                } else {
                    ++mapIt;
                }
            }
        }
        g_animStillScratch.clear();
    }
}

void HeldFrame_StepBuildingAnimations(float fps)
{
    if (!g_exeBase || !Determinism_IsActive() || Probe_IsRunning() || Determinism_WorldLoading()) return;
    bool paused = false;
    int speed = 1;
    Determinism_SessionSpeed(&paused, &speed);
    BYTE menuByte = 0;
    Read(g_exeBase + 0x9D4EE0 + 13, &menuByte, sizeof(menuByte));
    if (paused || menuByte != 0 || !isfinite(fps) || fps <= 0.0f) return;

    const size_t written = CollectAllAnimatedBuildings(true, false, nullptr, nullptr, nullptr);
    g_animWriteScratch.clear();
    g_animWriteScratch.reserve(written);
    for (size_t i = 0; i < written; ++i) {
        const AnimBldSnapshot& snap = g_animReadScratch[i];
        BuildingAnimState& st = g_buildingAnimState[snap.b];
        float stepPerTick = 0.0f;
        if (snap.fallbackStep > 0.0f) {
            stepPerTick = (st.hasMeasuredStep && st.stepPerTick > 0.0f) ? st.stepPerTick : snap.fallbackStep;
        }
        if (stepPerTick <= 0.0f) continue;
        const float nextFrame = HeldFrame_StepBuildingAnimFrame(snap.curFrame, snap.maxFrames, stepPerTick, fps, false);
        st.visualFrame = nextFrame;
        g_animWriteScratch.push_back({ snap.b, nextFrame });
    }
    FlushBuildingAnimWrites();
}

void HeldFrame_AfterEarly()
{
    Determinism_PrivateScope privateRng;
    RestoreBuildingAnimationsAfterEarly();
    size_t agents = 0;
    bool canSubmitCitizens = false;
    PrepareDoorsAndView(&agents, &canSubmitCitizens);
    if (canSubmitCitizens) SubmitNearCitizensFromView(agents);
}

// Early (30cb74..30cf4e) normally starts and joins this worker. On a held
// frame its simulation vector exe+9e7738 and the queues at [9e7710]+60 and
// [9e2350]+40 are empty. The vehicle render vector stays visible under the
// guard; when the guard cannot capture it, the vector world+12810 and the
// realism setting exe+9d54cc are hidden for the pass instead (vehicles are
// not drawn on that frame). Both threads draw from private streams. Without
// a usable worker the entities are still drawn. (by SoylentGreed)
bool HeldFrame_Draw(bool entitiesReady)
{
    BYTE* worker = *(BYTE**)(g_exeBase + 0x995290);
    bool workerOk = worker && !worker[0x28] && *(HANDLE*)(worker + 0x60) && *(DWORD*)(worker + 0xC8);
    static VehicleGuard guard;
    static bool hiddenLogged = false;
    bool guarded = false;
    uintptr_t hidden[2] = {};
    int realism = 0;
    float alpha = (Determinism_SessionRunning() && !Probe_IsRunning()) ? Determinism_RenderAlpha() : 0.0f;
    g_hfDiag.inHeldDraw = true;
    g_hfDiag.heldFramesDrawn++;
    g_hfDiag.lastAlpha = alpha;
    if (workerOk) {
        try {
            guarded = guard.Begin(g_exeBase + 0x9E7720, alpha);
        } catch (const std::bad_alloc&) {
            guarded = false;
        }
        if (!guarded) {
            g_hfDiag.guardFallbacks++;
            if (!hiddenLogged) {
                hiddenLogged = true;
                Logf("republicmp  HELD FRAME: vehicle graph could not be captured - vehicles hidden on such frames");
            }
            hidden[0] = *(uintptr_t*)(g_exeBase + 0x9E7728);
            *(uintptr_t*)(g_exeBase + 0x9E7728) = *(uintptr_t*)(g_exeBase + 0x9E7720);
            hidden[1] = 1;
            realism = *(int*)(g_exeBase + 0x9D54CC);
            *(int*)(g_exeBase + 0x9D54CC) = 0;
        }
    }
    // Early queues the junctions and doors before it starts the worker, whose
    // vehicle builders push to the same render lists (+a8, +1f8, +258).
    size_t heldAgents = 0;
    bool canSubmitCitizens = false;
    if (entitiesReady) {
        Determinism_PrivateScope privateRng;
        DrawJunctions();
        PrepareDoorsAndView(&heldAgents, &canSubmitCitizens);
    }
    BYTE* queue = *(BYTE**)(g_exeBase + 0x9E7710);
    BYTE* paths = *(BYTE**)(g_exeBase + 0x9E2350);
    BYTE* headers[] = { g_exeBase + 0x9E7738, queue ? queue + 0x60 : nullptr, paths ? paths + 0x40 : nullptr };
    uintptr_t ends[3] = {};
    bool started = false;
    if (workerOk) {
        for (int i = 0; i < 3; i++) if (headers[i]) {
            ends[i] = *(uintptr_t*)(headers[i] + 8);
            *(uintptr_t*)(headers[i] + 8) = *(uintptr_t*)headers[i];
        }
        Determinism_SetPrivateWorker(*(DWORD*)(worker + 0xC8));
        worker[0x59] = 0;
        started = SetEvent(*(HANDLE*)(worker + 0x60)) != FALSE;
    }
    if (entitiesReady && canSubmitCitizens) {
        Determinism_PrivateScope privateRng;
        SubmitNearCitizensFromView(heldAgents);
    }
    if (started) {
        auto* lock = (CRITICAL_SECTION*)(worker + 0x70);
        for (;;) {
            EnterCriticalSection(lock);
            bool done = worker[0x59] != 0;
            LeaveCriticalSection(lock);
            if (done) break;
            SwitchToThread();
        }
    }
    bool clean = true;
    if (workerOk) {
        Determinism_SetPrivateWorker(0);
        if (guarded) {
            FlushDeferredVehicleHighlights();
            clean = guard.End();
        }
        for (int i = 0; i < 3; i++) if (headers[i]) *(uintptr_t*)(headers[i] + 8) = ends[i];
        if (hidden[1]) {
            *(uintptr_t*)(g_exeBase + 0x9E7728) = hidden[0];
            *(int*)(g_exeBase + 0x9D54CC) = realism;
        }
    }
    g_deferredPrepHighlights.clear();
    g_hfDiag.inHeldDraw = false;
    return clean;
}
