// SPDX-License-Identifier: GPL-3.0-only
#include "held_frame.h"
#include "determinism.h"
#include "../core/engine_rva.h"
#include "../core/diag_log.h"
#include "../core/safe_mem.h"
#include <vector>
#include <new>
#include <unordered_set>
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

static bool HeapPointer(unsigned long long v)
{
    if (v < (1ull << 32) || v >= 0x800000000000ull || (v & 7)) return false;
    unsigned long long probe = 0;
    return Read((const void*)v, &probe, sizeof(probe));
}

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

static int GetVehicleWheelCount(const BYTE* p, BYTE** outBegin)
{
    *outBegin = nullptr;
    uintptr_t begin = 0, end = 0;
    if (!Read(p + VEH_WHEELS_BEGIN_OFFSET, &begin, sizeof(begin)) ||
        !Read(p + VEH_WHEELS_END_OFFSET, &end, sizeof(end))) {
        return 0;
    }
    if (!begin || end <= begin || (end - begin) % VEH_WHEEL_STRUCT_STRIDE != 0) return 0;
    size_t count = (end - begin) / VEH_WHEEL_STRUCT_STRIDE;
    if (count > 64 || !IsPointerLikeX64((uint64_t)begin)) return 0;
    if (count > (size_t)VEH_MAX_WHEELS) count = (size_t)VEH_MAX_WHEELS;
    uint64_t probe = 0;
    if (!Read((const void*)begin, &probe, sizeof(probe)) ||
        !Read((const void*)(begin + count * VEH_WHEEL_STRUCT_STRIDE - sizeof(probe)), &probe, sizeof(probe))) {
        return 0;
    }
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
    float prevSteer;
    float currSteer;
    float currSteerTarget;
    int wheelCount;
    float prevWheelAngle[VEH_MAX_WHEELS];
    float currWheelAngle[VEH_MAX_WHEELS];
};

static std::unordered_map<BYTE*, VehicleInterpRecord> g_vehicleInterp;
static unsigned long long g_interpTick = 0;
static HeldFrameDiagStats g_hfDiag = {};
static std::vector<BYTE*> g_camPrepModified;
static BYTE g_savedCamMgr[CAM_MGR_STRUCT_BYTES] = {};
static BYTE* g_savedCamMgrPtr = nullptr;
static BYTE* g_savedRideVeh = nullptr;
static BYTE* g_savedFollowWin = nullptr;
static float g_savedCamFps = 60.0f;
static bool g_hasCamSnapshot = false;

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

void HeldFrame_Reset()
{
    g_vehicleInterp.clear();
    g_interpTick = 0;
    memset(&g_hfDiag, 0, sizeof(g_hfDiag));
    g_camPrepModified.clear();
    g_deferredPrepHighlights.clear();
    memset(g_savedCamMgr, 0, sizeof(g_savedCamMgr));
    g_savedCamMgrPtr = nullptr;
    g_savedRideVeh = nullptr;
    g_savedFollowWin = nullptr;
    g_savedCamFps = 60.0f;
    g_hasCamSnapshot = false;
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

    rot[0] = currNode[35]; rot[1] = currNode[36]; rot[2] = currNode[37];
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
    rec.wheelCount = GetVehicleWheelCount(p, &wheelBegin);
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
    int wCount = GetVehicleWheelCount(p, &wheelBegin);
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
    if (rec.id == id && rec.currTick == tick) return;
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
            float shiftSq = sx * sx + sy * sy + sz * sz;
            if (shiftSq < 16.0f && isfinite(shiftSq)) {
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
    it->second.smoothOffset[0] += ox;
    it->second.smoothOffset[1] += oy;
    it->second.smoothOffset[2] += oz;
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

static void RecordVehicleGraph(BYTE* start, unsigned long long tick, std::unordered_set<BYTE*>& visited)
{
    if (!start || !IsPointerLikeX64((uint64_t)(uintptr_t)start)) return;
    std::vector<BYTE*> queue;
    if (visited.insert(start).second) queue.push_back(start);

    for (size_t qi = 0; qi < queue.size() && qi < 2048; ++qi) {
        BYTE* curr = queue[qi];
        SafeRecordOneVehicleTick(curr, tick);

        if (queue.size() >= 2048) continue;

        BYTE* link = nullptr;
        if (Read(curr + VEH_LINK_PTR_OFFSET, &link, sizeof(link)) && link &&
            IsPointerLikeX64((uint64_t)(uintptr_t)link) && visited.insert(link).second) {
            queue.push_back(link);
        }

        if (queue.size() >= 2048) continue;

        BYTE* parent = nullptr;
        if (Read(curr + VEH_PARENT_PTR_OFFSET, &parent, sizeof(parent)) && parent &&
            IsPointerLikeX64((uint64_t)(uintptr_t)parent) && visited.insert(parent).second) {
            queue.push_back(parent);
        }

        if (queue.size() >= 2048) continue;

        BYTE** children[2] = {};
        if (Read(curr + VEH_CHILDREN_BEGIN_OFFSET, children, sizeof(children))) {
            uintptr_t cBegin = (uintptr_t)children[0];
            uintptr_t cEnd = (uintptr_t)children[1];
            if (cEnd > cBegin && (cEnd - cBegin) % sizeof(BYTE*) == 0 &&
                (cEnd - cBegin) / sizeof(BYTE*) <= 1024 &&
                IsPointerLikeX64((uint64_t)cBegin)) {
                size_t nChildren = (cEnd - cBegin) / sizeof(BYTE*);
                std::vector<BYTE*> c(nChildren);
                if (Read((const void*)cBegin, c.data(), nChildren * sizeof(BYTE*))) {
                    for (BYTE* child : c) {
                        if (queue.size() >= 2048) break;
                        if (child && IsPointerLikeX64((uint64_t)(uintptr_t)child) && visited.insert(child).second) {
                            queue.push_back(child);
                        }
                    }
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

static void AddVisibleVehicleTarget(BYTE* p, std::unordered_set<BYTE*>& seen, std::vector<BYTE*>& targets)
{
    if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) return;
    uint64_t probe = 0;
    if (!Read(p, &probe, sizeof(probe)) || !Read(p + 0x1AD8 - sizeof(probe), &probe, sizeof(probe))) return;
    if (seen.insert(p).second) targets.push_back(p);
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
    std::unordered_set<BYTE*> seen;
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
    std::vector<BYTE*> list;
    if (!ReadEngineVehicleList((const void*)(g_exeBase + 0x9E7720), list)) return;

    std::unordered_set<BYTE*> visited;
    for (BYTE* p : list) {
        if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) continue;
        RecordVehicleGraph(p, tick, visited);
    }
    std::vector<BYTE*> followRoots;
    CollectFollowCameraVehicles(followRoots);
    for (BYTE* p : followRoots) {
        RecordVehicleGraph(p, tick, visited);
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
    int wCount = GetVehicleWheelCount(p, &wheelBegin);
    if (wCount > rec.wheelCount) wCount = rec.wheelCount;
    for (int w = 0; w < wCount; w++) {
        *(float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) =
            HeldFrame_LerpAngleRadians(rec.prevWheelAngle[w], rec.currWheelAngle[w], alpha);
    }
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
            if (ex0 * ex0 + ey0 * ey0 + ez0 * ez0 < 1e-4f) {
                float dx0 = rec.currNode[0][32] - rec.prevNode[0][32];
                float dy0 = rec.currNode[0][33] - rec.prevNode[0][33];
                float dz0 = rec.currNode[0][34] - rec.prevNode[0][34];
                float distSq0 = dx0 * dx0 + dy0 * dy0 + dz0 * dz0;
                float smoothSq = rec.smoothOffset[0] * rec.smoothOffset[0] +
                                 rec.smoothOffset[1] * rec.smoothOffset[1] +
                                 rec.smoothOffset[2] * rec.smoothOffset[2];
                if ((distSq0 > 1e-8f || smoothSq > 1e-8f) && distSq0 < 400.0f && isfinite(distSq0)) {
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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        moved = false;
    }
    DiagLog_EndSafeSeh();
    return moved;
}

static void RestoreVehicleSnapshotNodes(BYTE* p)
{
    if (!p) return;
    auto it = g_vehicleInterp.find(p);
    if (it == g_vehicleInterp.end()) return;
    const VehicleInterpRecord& rec = it->second;
    DiagLog_BeginSafeSeh();
    __try {
        if (*(const unsigned*)(p + 0x04) == rec.id) {
            int nNodes = GetActiveVehicleNodeCount(p);
            if (nNodes > rec.nodeCount) nNodes = rec.nodeCount;
            for (int k = 0; k < nNodes; k++) {
                memcpy(p + kVehicleNodeOffsets[k], rec.currNode[k], NODE_SNAPSHOT_FLOATS * sizeof(float));
            }
            *(float*)(p + VEH_STEER_CURRENT_OFFSET) = rec.currSteer;
            *(float*)(p + VEH_STEER_TARGET_OFFSET) = rec.currSteerTarget;
            BYTE* wheelBegin = nullptr;
            int wCount = GetVehicleWheelCount(p, &wheelBegin);
            if (wCount > rec.wheelCount) wCount = rec.wheelCount;
            for (int w = 0; w < wCount; w++) {
                *(float*)(wheelBegin + (size_t)w * VEH_WHEEL_STRUCT_STRIDE) = rec.currWheelAngle[w];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void ExpandVehicleTargetGraph(std::unordered_set<BYTE*>& seen, std::vector<BYTE*>& targets)
{
    const size_t maxTargets = (64ull << 20) / 0x1AD8;
    for (size_t i = 0; i < targets.size() && targets.size() < maxTargets; i++) {
        BYTE* target = targets[i];

        BYTE* parent = nullptr;
        if (Read(target + VEH_PARENT_PTR_OFFSET, &parent, sizeof(parent)) && parent &&
            IsPointerLikeX64((uint64_t)(uintptr_t)parent)) {
            AddVisibleVehicleTarget(parent, seen, targets);
        }
        if (targets.size() >= maxTargets) break;

        BYTE* link = nullptr;
        if (Read(target + VEH_LINK_PTR_OFFSET, &link, sizeof(link)) && link &&
            IsPointerLikeX64((uint64_t)(uintptr_t)link)) {
            AddVisibleVehicleTarget(link, seen, targets);
        }
        if (targets.size() >= maxTargets) break;

        BYTE** children[2] = {};
        if (Read(target + VEH_CHILDREN_BEGIN_OFFSET, children, sizeof(children))) {
            uintptr_t begin = (uintptr_t)children[0], end = (uintptr_t)children[1];
            if (end > begin && (end - begin) % sizeof(BYTE*) == 0 &&
                (end - begin) / sizeof(BYTE*) <= 1024 &&
                IsPointerLikeX64((uint64_t)begin)) {
                size_t count = (end - begin) / sizeof(BYTE*);
                std::vector<BYTE*> c(count);
                if (Read((const void*)begin, c.data(), count * sizeof(BYTE*))) {
                    for (BYTE* child : c) {
                        if (targets.size() >= maxTargets) break;
                        if (child && IsPointerLikeX64((uint64_t)(uintptr_t)child)) {
                            AddVisibleVehicleTarget(child, seen, targets);
                        }
                    }
                }
            }
        }
    }
}

static void CollectVisibleVehicleGraph(const std::vector<BYTE*>& list, std::vector<BYTE*>& targets)
{
    targets.clear();
    std::unordered_set<BYTE*> seen;
    for (BYTE* p : list) {
        if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p)) continue;
        BYTE visible = 0, linked = 0;
        BYTE* link = nullptr;
        if (!Read(p + VEH_RENDER_VISIBLE_OFFSET, &visible, sizeof(visible))) continue;
        if (Read(p + VEH_LINK_PTR_OFFSET, &link, sizeof(link)) && link) {
            if (IsPointerLikeX64((uint64_t)(uintptr_t)link)) {
                Read(link + VEH_RENDER_VISIBLE_OFFSET, &linked, sizeof(linked));
            } else {
                link = nullptr;
            }
        }
        if (visible || linked) {
            AddVisibleVehicleTarget(p, seen, targets);
            if (link) AddVisibleVehicleTarget(link, seen, targets);
        }
    }
    std::vector<BYTE*> followRoots;
    CollectFollowCameraVehicles(followRoots);
    for (BYTE* p : followRoots) {
        AddVisibleVehicleTarget(p, seen, targets);
    }
    for (const auto& h : g_deferredPrepHighlights) {
        AddVisibleVehicleTarget(h.vehicle, seen, targets);
    }
    ExpandVehicleTargetGraph(seen, targets);
}

class VehicleGuard {
    static const size_t Bytes = 0x1AD8;
    std::vector<BYTE*> targets_;
    std::vector<BYTE> copies_;
    std::vector<std::pair<BYTE*, BYTE>> flags_;

    void SnapshotCopies()
    {
        std::vector<BYTE*> validTargets;
        validTargets.reserve(targets_.size());
        copies_.clear();
        copies_.reserve(targets_.size() * Bytes);
        std::vector<BYTE> vehBuf(Bytes);
        for (BYTE* target : targets_) {
            if (Read(target, vehBuf.data(), Bytes)) {
                validTargets.push_back(target);
                copies_.insert(copies_.end(), vehBuf.begin(), vehBuf.end());
            }
        }
        targets_ = std::move(validTargets);
    }

    void SuppressFlags(const std::vector<BYTE*>& list)
    {
        std::unordered_set<BYTE*> flagSeen;
        auto AddFlag = [&](BYTE* p) {
            if (!p || !IsPointerLikeX64((uint64_t)(uintptr_t)p) || !flagSeen.insert(p).second) return;
            BYTE f = 0;
            if (Read(p + VEH_WORKER_SIM_FLAG_OFFSET, &f, sizeof(f)) && f) {
                flags_.push_back({ p, f });
            }
        };
        for (BYTE* p : list) AddFlag(p);
        for (BYTE* p : targets_) AddFlag(p);

        SafeClearWorkerFlags(flags_.data(), flags_.size());
    }

    static void SafeClearWorkerFlags(const std::pair<BYTE*, BYTE>* flags, size_t count)
    {
        if (!flags || count == 0) return;
        const BYTE zero = 0;
        for (size_t i = 0; i < count; ++i) {
            SafeWritePod(flags[i].first + VEH_WORKER_SIM_FLAG_OFFSET, zero);
        }
    }

    static void SafeRestoreWorkerFlags(const std::pair<BYTE*, BYTE>* flags, size_t count)
    {
        if (!flags || count == 0) return;
        for (size_t i = 0; i < count; ++i) {
            SafeWritePod(flags[i].first + VEH_WORKER_SIM_FLAG_OFFSET, flags[i].second);
        }
    }

public:
    bool Begin(BYTE* listAt, float alpha)
    {
        targets_.clear(); flags_.clear();
        std::vector<BYTE*> list;
        if (!ReadEngineVehicleList(listAt, list)) return false;

        CollectVisibleVehicleGraph(list, targets_);
        SnapshotCopies();
        SuppressFlags(list);

        g_hfDiag.lastTargetsCount = (unsigned)targets_.size();
        g_hfDiag.lastAlpha = alpha;
        unsigned interpCount = 0;
        if (alpha >= 0.0f && alpha < 0.999f) {
            for (BYTE* p : targets_) {
                if (ApplySubTickVisualInterp(p, alpha)) interpCount++;
            }
        }
        g_hfDiag.lastInterpolatedCount = interpCount;
        return true;
    }

    bool End()
    {
        SafeRestoreWorkerFlags(flags_.data(), flags_.size());
        flags_.clear();

        bool ok = true;
        unsigned long long current[Bytes / 8];
        for (size_t i = 0; i < targets_.size(); i++) {
            BYTE* p = targets_[i];
            RestoreVehicleSnapshotNodes(p);
            if (!Read(p, current, Bytes)) { ok = false; continue; }
            for (size_t o = 0; o < Bytes; o += 8) {
                unsigned long long before, now;
                memcpy(&before, copies_.data() + i * Bytes + o, 8);
                now = current[o / 8];
                if (now == before) continue;
                bool inNodes = (o >= 0xD60 && o < 0x1050) || (o >= 0x1080 && o < 0x1140) ||
                               (o >= 0x1318 && o < 0x13D8) || (o >= 0x1480 && o < 0x16F0);
                if (!inNodes && HeapPointer(before) && HeapPointer(now)) {
                    Logf("republicmp  HELD FRAME: vehicle pointer changed at +0x%zX - probe refused", o);
                    g_hfDiag.restoreRefusals++;
                    ok = false;
                } else {
                    if (!SafeMemcpyPod(p + o, &before, 8)) {
                        ok = false;
                    }
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
    unsigned rva = isExtraPass ? 0x66EDD0u : 0x6712B0u;
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
    std::vector<BYTE*> modified;
    if (alpha >= 0.0f && alpha < 0.999f) {
        std::vector<BYTE*> targets;
        std::unordered_set<BYTE*> seen;
        AddVisibleVehicleTarget((BYTE*)vehicle, seen, targets);
        ExpandVehicleTargetGraph(seen, targets);
        modified.reserve(targets.size());
        for (BYTE* p : targets) {
            if (ApplySubTickVisualInterp(p, alpha, false)) {
                modified.push_back(p);
            }
        }
    }
    {
        Determinism_PrivateScope privateRng;
        CallNativeVehicleHighlight(vehicle, a2, a3, colorRgba, flag, isExtraPass);
    }
    for (BYTE* p : modified) {
        RestoreVehicleSnapshotNodes(p);
    }
}

void HeldFrame_DrawStepVehicles(void* world)
{
    typedef void (*DrawVehiclesFn)(void*);
    DrawVehiclesFn nativeDraw = (DrawVehiclesFn)(g_exeBase + 0x3FA850);
    if (!Determinism_IsActive() || !Determinism_SessionRunning() || Probe_IsRunning() || g_hfDiag.inHeldDraw) {
        nativeDraw(world);
        FlushDeferredVehicleHighlights();
        return;
    }
    HeldFrame_OnSimTick(Determinism_StepCount() + 1);
    float alpha = Determinism_RenderAlpha();
    g_hfDiag.lastAlpha = alpha;
    std::vector<BYTE*> list;
    std::vector<BYTE*> modified;
    if (alpha >= 0.0f && alpha < 0.999f && ReadEngineVehicleList((const void*)(g_exeBase + 0x9E7720), list)) {
        std::vector<BYTE*> targets;
        CollectVisibleVehicleGraph(list, targets);
        modified.reserve(targets.size());
        for (BYTE* p : targets) {
            if (ApplySubTickVisualInterp(p, alpha)) {
                modified.push_back(p);
            }
        }
        g_hfDiag.lastTargetsCount = (unsigned)targets.size();
        g_hfDiag.lastInterpolatedCount = (unsigned)modified.size();
    }
    nativeDraw(world);
    FlushDeferredVehicleHighlights();
    for (BYTE* p : modified) {
        RestoreVehicleSnapshotNodes(p);
    }
}

static void CollectFollowCameraSubgraph(std::vector<BYTE*>& targets)
{
    targets.clear();
    std::vector<BYTE*> roots;
    CollectFollowCameraVehicles(roots);
    if (roots.empty()) return;
    std::unordered_set<BYTE*> seen;
    for (BYTE* p : roots) {
        AddVisibleVehicleTarget(p, seen, targets);
    }
    ExpandVehicleTargetGraph(seen, targets);
}

void HeldFrame_BeginCameraPrepare(float fps)
{
    g_camPrepModified.clear();
    g_hasCamSnapshot = false;
    if (!Determinism_IsActive() || !Determinism_SessionRunning() || Probe_IsRunning() || Determinism_WorldLoading()) {
        return;
    }
    std::vector<BYTE*> targets;
    CollectFollowCameraSubgraph(targets);
    if (targets.empty()) return;

    if (Determinism_IsStepFrame()) {
        BYTE* camMgr = nullptr;
        if (Read(g_exeBase + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF, &camMgr, sizeof(camMgr)) &&
            camMgr && IsPointerLikeX64((uint64_t)(uintptr_t)camMgr) &&
            Read(camMgr, g_savedCamMgr, CAM_MGR_STRUCT_BYTES)) {
            g_savedCamMgrPtr = camMgr;
            g_savedRideVeh = *(BYTE**)(g_savedCamMgr + CAM_MGR_RIDE_VEH_PTR_OFF);
            g_savedFollowWin = *(BYTE**)(g_savedCamMgr + CAM_MGR_FOLLOW_WIN_PTR_OFF);
            g_savedCamFps = (fps > 1.0f && isfinite(fps)) ? fps : 60.0f;
            g_hasCamSnapshot = true;
        }
        return;
    }

    float alpha = Determinism_RenderAlpha();
    if (alpha >= 0.0f && alpha < 0.999f) {
        g_camPrepModified.reserve(targets.size());
        for (BYTE* p : targets) {
            if (ApplySubTickVisualInterp(p, alpha, false)) {
                g_camPrepModified.push_back(p);
            }
        }
    }
}

void HeldFrame_EndCameraPrepare()
{
    for (BYTE* p : g_camPrepModified) {
        RestoreVehicleSnapshotNodes(p);
    }
    g_camPrepModified.clear();
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

    if (!SafeMemcpyPod(camMgr, g_savedCamMgr, CAM_MGR_STRUCT_BYTES)) return;

    float alpha = Determinism_RenderAlpha();
    std::vector<BYTE*> modified;
    if (alpha >= 0.0f && alpha < 0.999f) {
        modified.reserve(targets.size());
        for (BYTE* p : targets) {
            if (ApplySubTickVisualInterp(p, alpha, false)) {
                modified.push_back(p);
            }
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

    for (BYTE* p : modified) {
        RestoreVehicleSnapshotNodes(p);
    }
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
        { 0x83CEA0, { 0x48,0x8b,0xc4,0x48,0x89,0x48,0x08,0x55,0x53,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8d,0xa8,0x98,0xfc,0xff,0xff }, 22 },
        { 0x2595AE, { 0x48,0x8d,0x0d,0x5b,0xb9,0x77,0x00,0xe8 }, 8 },
        { 0x3FA907, { 0x49,0x8b,0x86,0x18,0x28,0x01,00,0x49,0x2b,0x86,0x10,0x28,0x01,00 }, 14 },
        { 0x3FAA13, { 0x41,0x0f,0xb6,0xd7,0xe8,0x94,0x68,0x27,0x00 }, 9 },
        { 0x67135B, { 0x80,0xb9,0xd8,0x13,0x00,0x00,0x00 }, 7 },
        { 0x671368, { 0x48,0x8b,0x89,0x98,0x03,0x00,0x00 }, 7 },
        { 0x6713D0, { 0x48,0x8b,0x9e,0xa8,0x03,0x00,0x00,0x48,0x2b,0x9e,0xa0 }, 11 },
        { 0x66EDF9, { 0x80,0xb9,0xd8,0x13,0x00,0x00,0x00 }, 7 },
        { 0x67C2AE, { 0x48,0x8b,0xb1,0xa0,0x04,00,00,0x49,0x8b,0xd8,0x44,0x0f,0xb6,0xe2,0x4c,0x8b,0xf1,0x80,0xbe,0xd8,0x13,0x00,0x00,0x00 }, 24 },
    };
    for (const auto& s : sites) {
        if (memcmp(g_exeBase + s.rva, s.bytes, s.size)) {
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
        g_junctionsChecked = memcmp(g_exeBase + 0x30C851, load, sizeof(load)) == 0 && CallTargets(0x30C858, 0x228B50) &&
                             CallTargets(0x30C89D, 0x2239D0) ? 1 : 0;
        if (!g_junctionsChecked) Logf("republicmp  HELD FRAME: Early's junction drawing differs - junction lights drawn on tick frames only");
    }
    if (!g_junctionsChecked) return;
    BYTE* world = g_exeBase + 0x9D4F10;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* manager = *(BYTE**)(world + 0x12808);
        if (manager) {
            ((void (*)(void*))(g_exeBase + 0x228B50))(manager);
            if (world[0x14451]) {
                BYTE** begin = *(BYTE***)(manager + 0x50);
                size_t n = begin ? (size_t)(*(BYTE***)(manager + 0x58) - begin) : 0;
                for (size_t i = 0; i < n; i++) {
                    BYTE* junction = (*(BYTE***)(manager + 0x50))[i];
                    if (junction) ((void (*)(void*))(g_exeBase + 0x2239D0))(junction);
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
static int g_doorsChecked = -1;

static size_t Count(const BYTE* vector, size_t elem)
{
    uintptr_t begin = *(const uintptr_t*)vector, end = *(const uintptr_t*)(vector + 8);
    return begin && end > begin && (end - begin) % elem == 0 ? (end - begin) / elem : 0;
}

static void DrawDoors()
{
    // This PC's frame time since the last call (once a frame), at most a quarter second.
    static LARGE_INTEGER s_last = {};
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = s_last.QuadPart && frequency.QuadPart ? (float)(now.QuadPart - s_last.QuadPart) / frequency.QuadPart : 0.0f;
    s_last = now;
    if (seconds < 0.0f) seconds = 0.0f;
    if (seconds > 0.25f) seconds = 0.25f;
    if (g_doorsChecked < 0) {
        static const unsigned char list[14] = { 0x48, 0x8b, 0x1d, 0xe2, 0xbc, 0x85, 0x00, 0x48, 0x81, 0xc3, 0xa8, 0x00, 0x00, 0x00 };
        g_doorsChecked = CallTargets(0x13DB61, 0x138B90) && memcmp(g_exeBase + 0x13957F, list, sizeof(list)) == 0 ? 1 : 0;
        if (!g_doorsChecked) Logf("republicmp  HELD FRAME: Early's door drawing differs - doors drawn on tick frames only");
    }
    if (!g_doorsChecked) return;
    typedef BYTE* (*FragmentFn)(void* models, int model, int variant);
    typedef BYTE* (*GetNodeFn)(void* tree, int index);
    FragmentFn fragment = *(FragmentFn*)(g_exeBase + FRAGMENT_IMPORT);
    GetNodeFn getNode = *(GetNodeFn*)(g_exeBase + GETNODE_IMPORT);
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* engine = *(BYTE**)(g_exeBase + ENGINE_RVA);
        BYTE** it = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
        BYTE** end = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
        for (; engine && it && it < end; ++it) {
            BYTE* b = *it;
            BYTE* type = b ? *(BYTE**)(b + 0x318) : nullptr;
            // As the pass (13db03..13db61): a finished building (+604 1.0, no site +5d0) with doors.
            if (!type || *(float*)(b + 0x604) != 1.0f || *(void**)(b + 0x5D0)) continue;
            if (!Count(type + 0xA18, 0x28) && !Count(type + 0xA30, 0x30)) continue;
            BYTE* f = fragment(*(void**)(engine + 0xFF8), *(int*)(type + 0x410), *(int*)(b + 0x5F4));
            BYTE* node = f ? getNode(*(void**)(engine + 0xEE0), *(int*)(f + 0x3C)) : nullptr;
            if (node && node[0x25]) ((void (*)(void*, float))(g_exeBase + 0x138B90))(b, seconds);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

// Early submits only the citizens of its every-frame list (world +126c0,
// 83cea0) for drawing; 832ae0 puts there the agents tied to a building on the
// camera and nearer than [exe+90af6c] (550 m): with +38 and +40 set, either
// one (the visibility of +38 with the distance of +40, or both of +40), else
// +598; never one with +2c set or +10 above 0. Every other agent is updated
// in three buckets and not drawn. The simulation now sees every building off
// the camera, so that list stays empty: this PC builds it itself, by the same
// rules from its own view, and submits it every frame (83cea0 culls it and
// writes only the frame's render lists; the simulation never sees the list).
#define AGENTS_ALL 0x126A8
#define DRAW_AGENTS_RVA 0x83CEA0
#define AGENT_NEAR_RVA 0x90AF6C
#define CAMERA_RVA 0x9E9C90
#define NODE_POSITION_IMPORT 0x86C7B8   // C3D_NODE::GetPosition
#define CAMERA_ZDISTANCE_IMPORT 0x86D008 // C3D_CAMERA::GetZDistance

struct ViewBuilding {
    BYTE* building;
    float distance;
};
static std::vector<ViewBuilding> g_viewBuildings; // sorted by address
static std::vector<BYTE*> g_nearAgents;

// The buildings this PC sees and their camera distance (as the building pass's visible branch).
static size_t CollectView(ViewBuilding* out, size_t max)
{
    typedef BYTE* (*FragmentFn)(void* models, int model, int variant);
    typedef BYTE* (*GetNodeFn)(void* tree, int index);
    typedef void* (*PositionFn)(void* node, float* out);
    typedef float (*ZDistanceFn)(void* camera, float* position);
    FragmentFn fragment = *(FragmentFn*)(g_exeBase + FRAGMENT_IMPORT);
    GetNodeFn getNode = *(GetNodeFn*)(g_exeBase + GETNODE_IMPORT);
    PositionFn position = *(PositionFn*)(g_exeBase + NODE_POSITION_IMPORT);
    ZDistanceFn zDistance = *(ZDistanceFn*)(g_exeBase + CAMERA_ZDISTANCE_IMPORT);
    size_t n = 0;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* engine = *(BYTE**)(g_exeBase + ENGINE_RVA);
        BYTE** it = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
        BYTE** end = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
        for (; engine && it && it < end && n < max; ++it) {
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
            float at[4] = {};
            position(b + 0x320, at);
            out[n++] = { b, zDistance(g_exeBase + CAMERA_RVA, at) };
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

static void DrawCitizens()
{
    size_t buildings = 0, agents = 0;
    if (!ListSizes(&buildings, &agents)) return;
    if (!agents || buildings > 1000000 || agents > 10000000) return;
    try {
        g_viewBuildings.resize(buildings);
        size_t nView = buildings ? CollectView(g_viewBuildings.data(), buildings) : 0;
        g_viewBuildings.resize(nView);
        std::sort(g_viewBuildings.begin(), g_viewBuildings.end(),
                  [](const ViewBuilding& x, const ViewBuilding& y) { return x.building < y.building; });
        g_nearAgents.resize(agents);
        size_t nNear = CollectNear(g_viewBuildings.data(), nView, g_nearAgents.data(), agents);
        g_keptVisible.resize(nView);
        if (nNear) Submit(g_nearAgents.data(), nNear, g_viewBuildings.data(), nView, g_keptVisible.data());
    } catch (const std::bad_alloc&) {
    }
}

void HeldFrame_AfterEarly()
{
    Determinism_PrivateScope privateRng;
    DrawDoors();
    DrawCitizens();
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
    if (entitiesReady) {
        Determinism_PrivateScope privateRng;
        DrawJunctions();
        DrawDoors();
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
    if (entitiesReady) {
        Determinism_PrivateScope privateRng;
        DrawCitizens();
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
