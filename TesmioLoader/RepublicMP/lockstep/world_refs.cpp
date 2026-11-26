// SPDX-License-Identifier: GPL-3.0-only
// See world_refs.h.
#include "world_refs.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include <string.h>

#define LINES_OFFSET WORLD_LINES_OFFSET
#define VEHICLES_OFFSET WORLD_VEHICLES_OFFSET
#define BUILDINGS_OFFSET WORLD_BUILDINGS_OFFSET
#define CONSTRUCTIONS_OFFSET WORLD_CONSTRUCTIONS_OFFSET
#define NODE_LIST 0x298

static const unsigned kNodeFields[5] = { 0xF840, 0xF848, 0xF9A8, 0xFAC8, 0xFAD0 };

const RouteRef kNoRef = { 0, -1, 0 };

void WorldRefs_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

unsigned Ref_FnvWords(const unsigned* words, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 4; b++) {
            h ^= (words[i] >> (8 * b)) & 0xffu;
            h *= 16777619u;
        }
    return h | 1u;
}

unsigned Ref_NodeCheck(U64 node)
{
    unsigned w[3] = { 0x4e4f4445u, *(unsigned*)(node + 4), *(unsigned*)(node + 0xc) };
    return Ref_FnvWords(w, 3);
}

static void ReadVectorBounds(U64 vector, U64* outBegin, U64* outEnd)
{
    if (g_exeBase) {
        const U64 w = (U64)(uintptr_t)World();
        size_t vecOff = 0;
        if (vector == w + VEHICLES_OFFSET) vecOff = VEHICLES_OFFSET;
        else if (vector == w + BUILDINGS_OFFSET) vecOff = BUILDINGS_OFFSET;
        uintptr_t uBeg = 0, uEnd = 0;
        if (vecOff != 0 && TwoStates_GetUnfilteredWorldVecRange(vecOff, &uBeg, &uEnd)) {
            *outBegin = (U64)uBeg;
            *outEnd   = (U64)uEnd;
            return;
        }
    }
    *outBegin = Q(vector);
    *outEnd   = Q(vector + 8);
}

unsigned Ref_IndexInVector(U64 vector, U64 p)
{
    U64 begin = 0, end = 0;
    ReadVectorBounds(vector, &begin, &end);
    if (!p || !begin || end < begin || (end - begin) / 8 > 1000000) return 0xFFFFFFFFu;
    for (U64 at = begin; at < end; at += 8)
        if (Q(at) == p) return (unsigned)((at - begin) / 8);
    return 0xFFFFFFFFu;
}

U64 Ref_AtVectorIndex(U64 vector, unsigned index)
{
    U64 begin = 0, end = 0;
    ReadVectorBounds(vector, &begin, &end);
    if (!begin || end < begin || index >= (end - begin) / 8) return 0;
    return Q(begin + (U64)index * 8);
}

void Ref_SetStandInVector(unsigned char* at, const void* begin, size_t bytes)
{
    U64 v[3] = {};
    if (bytes && begin) {
        v[0] = (U64)begin;
        v[1] = v[0] + bytes;
        v[2] = v[1];
    }
    memcpy(at, v, 24);
}

bool Ref_DefPtr(U64 p, unsigned char& tag, unsigned& index)
{
    tag = 0;
    index = 0;
    if (!p) return true;
    const unsigned tables[2] = { WORLD_RESOURCES_OFFSET, WORLD_VEHICLE_TYPES_OFFSET };
    const unsigned strides[2] = { WORLD_RESOURCE_STRIDE, WORLD_VEHICLE_TYPE_STRIDE };
    for (int k = 0; k < 2; k++) {
        U64 v[2] = {};
        if (!SafeMemcpyPod(v, World() + tables[k], 16) || !v[0] || v[1] < v[0]) continue;
        if (p >= v[0] && p < v[1] && (p - v[0]) % strides[k] == 0) {
            tag = (unsigned char)(k + 1);
            index = (unsigned)((p - v[0]) / strides[k]);
            return true;
        }
    }
    const unsigned slotCount = (WORLD_RES_PTR_SLOTS_END_OFF - WORLD_RES_PTR_SLOTS_BEGIN_OFF) / 8u;
    for (unsigned i = 0; i < slotCount; i++) {
        U64 slotPtr = 0;
        if (SafeMemcpyPod(&slotPtr, World() + WORLD_RES_PTR_SLOTS_BEGIN_OFF + (size_t)i * 8u, 8) && slotPtr == p) {
            tag = 3;
            index = i;
            return true;
        }
    }
    U64 wasteVec[2] = {};
    if (SafeMemcpyPod(wasteVec, World() + WORLD_WASTE_RESOURCES_OFFSET, sizeof(wasteVec)) &&
        wasteVec[0] && wasteVec[1] >= wasteVec[0] && ((wasteVec[1] - wasteVec[0]) & 7u) == 0) {
        U64 wasteCount = (wasteVec[1] - wasteVec[0]) / 8u;
        if (wasteCount > 4096u) wasteCount = 4096u;
        for (U64 i = 0; i < wasteCount; ++i) {
            U64 item = 0;
            if (SafeReadPod((const U64*)(wasteVec[0] + i * 8u), &item) && item == p) {
                tag = 4;
                index = (unsigned)i;
                return true;
            }
        }
    }
    return false;
}

bool Ref_DefPtrAt(unsigned char tag, unsigned index, U64& p)
{
    p = 0;
    if (tag == 0) return index == 0;
    if (tag == 3) {
        const unsigned slotCount = (WORLD_RES_PTR_SLOTS_END_OFF - WORLD_RES_PTR_SLOTS_BEGIN_OFF) / 8u;
        if (index >= slotCount) return false;
        U64 slotPtr = 0;
        if (!SafeMemcpyPod(&slotPtr, World() + WORLD_RES_PTR_SLOTS_BEGIN_OFF + (size_t)index * 8u, 8) || !slotPtr)
            return false;
        p = slotPtr;
        return true;
    }
    if (tag == 4) {
        U64 wasteVec[2] = {};
        if (!SafeMemcpyPod(wasteVec, World() + WORLD_WASTE_RESOURCES_OFFSET, sizeof(wasteVec)) ||
            !wasteVec[0] || wasteVec[1] < wasteVec[0] || ((wasteVec[1] - wasteVec[0]) & 7u) != 0) {
            return false;
        }
        const U64 wasteCount = (wasteVec[1] - wasteVec[0]) / 8u;
        if ((U64)index >= wasteCount) return false;
        U64 item = 0;
        if (!SafeReadPod((const U64*)(wasteVec[0] + (U64)index * 8u), &item) || !item) return false;
        p = item;
        return true;
    }
    if (tag > 2) return false;
    unsigned table = tag == 1 ? WORLD_RESOURCES_OFFSET : WORLD_VEHICLE_TYPES_OFFSET;
    unsigned stride = tag == 1 ? WORLD_RESOURCE_STRIDE : WORLD_VEHICLE_TYPE_STRIDE;
    U64 v[2] = {};
    if (!SafeMemcpyPod(v, World() + table, 16) || !v[0] || v[1] < v[0] || index >= (v[1] - v[0]) / stride) return false;
    p = v[0] + (U64)index * stride;
    return true;
}

void Ref_NormalizeVehicleScriptName(const char* src, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!src) return;
    if (_strnicmp(src, "media_soviet/", 13) == 0 || _strnicmp(src, "media_soviet\\", 13) == 0) {
        src += 13;
    }
    size_t i = 0;
    while (src[i] && i + 1 < outSize) {
        char c = src[i];
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        out[i++] = c;
    }
    out[i] = '\0';
}

bool Ref_ReadVehicleTypeScriptName(U64 vehTypePtr, char* outNorm, size_t outSize)
{
    if (!outNorm || outSize == 0) return false;
    outNorm[0] = '\0';
    if (!IsPointerLikeX64(vehTypePtr)) return false;
    char raw[128] = {};
    if (!SafeReadAsciiDirect((const char*)(vehTypePtr + VEH_TYPE_SCRIPT_NAME_OFFSET), raw, sizeof(raw)) || !raw[0]) {
        return false;
    }
    Ref_NormalizeVehicleScriptName(raw, outNorm, outSize);
    return outNorm[0] != '\0';
}

unsigned char* Ref_FindVehicleTypeByScriptName(unsigned char* world, const char* scriptName)
{
    if (!world || !scriptName) return nullptr;
    char targetName[128] = {};
    if (!SafeReadAsciiDirect(scriptName, targetName, sizeof(targetName)) || !targetName[0]) {
        return nullptr;
    }
    char normTarget[128] = {};
    Ref_NormalizeVehicleScriptName(targetName, normTarget, sizeof(normTarget));
    if (!normTarget[0]) return nullptr;

    BYTE* begin = nullptr;
    BYTE* end = nullptr;
    if (!SafeReadPod(world + WORLD_VEHICLE_TYPES_OFFSET, &begin) ||
        !SafeReadPod(world + WORLD_VEHICLE_TYPES_OFFSET + 8, &end) ||
        !begin || end <= begin || (end - begin) % WORLD_VEHICLE_TYPE_STRIDE != 0)
    {
        return nullptr;
    }
    size_t count = (size_t)((end - begin) / WORLD_VEHICLE_TYPE_STRIDE);
    if (count > 4096u) count = 4096u;
    for (size_t i = 0; i < count; ++i) {
        BYTE* t = begin + i * WORLD_VEHICLE_TYPE_STRIDE;
        char normCand[128] = {};
        if (Ref_ReadVehicleTypeScriptName((U64)t, normCand, sizeof(normCand)) && _stricmp(normCand, normTarget) == 0) {
            return t;
        }
    }
    return nullptr;
}

bool Ref_EncodeDefPtrBytes(U64 p, unsigned char* outBuf, size_t outCap, size_t& outLen)
{
    outLen = 0;
    if (!outBuf) return false;
    unsigned char tag = 0;
    unsigned index = 0;
    if (!Ref_DefPtr(p, tag, index)) return false;
    if (tag == 2u) {
        char normName[128] = {};
        if (Ref_ReadVehicleTypeScriptName(p, normName, sizeof(normName))) {
            size_t len = strlen(normName);
            if (len >= 1u && len <= 127u) {
                if (outCap < 6u + len) return false;
                outBuf[0] = 5u;
                memcpy(outBuf + 1, &index, 4);
                outBuf[5] = (unsigned char)len;
                memcpy(outBuf + 6, normName, len);
                outLen = 6u + len;
                return true;
            }
        }
    }
    if (outCap < 5u) return false;
    outBuf[0] = tag;
    memcpy(outBuf + 1, &index, 4);
    outLen = 5u;
    return true;
}

bool Ref_DecodeDefPtrBytes(const unsigned char* data, size_t size, size_t& at, U64& p)
{
    p = 0;
    if (!data || at + 5u > size) return false;
    unsigned char tag = data[at];
    unsigned index = 0;
    memcpy(&index, data + at + 1, 4);
    at += 5u;
    if (tag == 5u) {
        if (at >= size) return false;
        unsigned char len = data[at++];
        if (len == 0u || len > 127u || at + (size_t)len > size) return false;
        char name[128] = {};
        memcpy(name, data + at, len);
        name[len] = '\0';
        at += len;
        BYTE* found = Ref_FindVehicleTypeByScriptName(World(), name);
        if (!found || !SaveSync_CheckVehicleDlcAllowed(found)) return false;
        p = (U64)found;
        return true;
    }
    if (!Ref_DefPtrAt(tag, index, p)) return false;
    if (tag == 2u && p && !SaveSync_CheckVehicleDlcAllowed((const void*)p)) {
        p = 0;
        return false;
    }
    return true;
}

bool Ref_CheckRoutineTable(int* cache, const TsmRoutineSig* table, size_t count, const char* moduleTag)
{
    unsigned badRva = 0;
    bool prev = cache && *cache < 0;
    bool ok = TsmCheckRoutineTable(g_exeBase, cache, table, count, &badRva);
    if (prev && !ok && moduleTag) {
        Logf("republicmp  %s: game routine at 0x%X differs - refused in lockstep", moduleTag, badRva);
    }
    return ok;
}


static bool Find(U64 vector, U64 object, int* place)
{
    unsigned idx = Ref_IndexInVector(vector, object);
    if (idx == 0xFFFFFFFFu) return false;
    *place = (int)idx;
    return true;
}

static U64 At(U64 vector, int place)
{
    return place >= 0 ? Ref_AtVectorIndex(vector, (unsigned)place) : 0;
}

bool Ref_Vehicle(U64 vehicle, RouteRef& r)
{
    r.list = 0;
    if (!vehicle || !Find((U64)World() + VEHICLES_OFFSET, vehicle, &r.place)) return false;
    r.check = *(unsigned*)(vehicle + 4);
    return true;
}

U64 Ref_VehicleAt(const RouteRef& r)
{
    U64 v = At((U64)World() + VEHICLES_OFFSET, r.place);
    return v && *(unsigned*)(v + 4) == r.check ? v : 0;
}

bool Ref_Building(U64 building, RouteRef& r)
{
    if (!building) return false;
    if (Find((U64)World() + BUILDINGS_OFFSET, building, &r.place)) {
        r.list = 0;
        r.check = *(unsigned*)building;
        return true;
    }
    if (Find((U64)World() + CONSTRUCTIONS_OFFSET, building, &r.place)) {
        r.list = 1;
        r.check = *(unsigned*)building;
        return true;
    }
    return false;
}

U64 Ref_BuildingAt(const RouteRef& r)
{
    U64 vec = (U64)World() + (r.list == 1 ? CONSTRUCTIONS_OFFSET : BUILDINGS_OFFSET);
    U64 b = At(vec, r.place);
    return b && *(unsigned*)b == r.check ? b : 0;
}

static unsigned LineCheck(U64 line)
{
    unsigned w[3] = { 0x4c494e45u, *(unsigned*)(line + 4), *(unsigned*)(line + 8) };
    return Ref_FnvWords(w, 3);
}

bool Ref_Line(U64 line, RouteRef& r)
{
    U64 lines = Q((U64)World() + LINES_OFFSET);
    if (!line || !lines) return false;
    for (unsigned k = 0; k < 2; k++)
        if (Find(lines + k * 0x18, line, &r.place)) {
            r.list = k;
            r.check = LineCheck(line);
            return true;
        }
    return false;
}

U64 Ref_LineAt(const RouteRef& r)
{
    U64 lines = Q((U64)World() + LINES_OFFSET);
    if (!lines || r.list > 1) return 0;
    U64 l = At(lines + r.list * 0x18, r.place);
    return l && LineCheck(l) == r.check ? l : 0;
}

bool Ref_Node(U64 node, RouteRef& r)
{
    for (unsigned field : kNodeFields) {
        U64 net = Q((U64)World() + field);
        if (net && Find(net + NODE_LIST, node, &r.place)) {
            r.list = field;
            r.check = Ref_NodeCheck(node);
            return true;
        }
    }
    return false;
}

U64 Ref_NodeAt(const RouteRef& r)
{
    bool valid = false;
    for (unsigned field : kNodeFields) {
        if (r.list == field) {
            valid = true;
            break;
        }
    }
    if (!valid) return 0;
    U64 net = Q((U64)World() + r.list);
    U64 n = net ? At(net + NODE_LIST, r.place) : 0;
    return n && Ref_NodeCheck(n) == r.check ? n : 0;
}

U64 Ref_FindBuildMenuRecord(U64 world, int place, const char* name, size_t nameBytes)
{
    if (!world || !name || !name[0] || nameBytes < 2u) return 0;
    constexpr U64 kMenuOffset = 0xD320u;
    constexpr U64 kMenuStride = 0x2D0u;
    constexpr U64 kMaxMenuEntries = 4096u;
    U64 range[2] = {};
    if (!SafeMemcpyPod(range, (const void*)(world + kMenuOffset), sizeof(range))) return 0;
    const U64 begin = range[0], end = range[1];
    if (!IsPointerLikeX64(begin) || end < begin || ((end - begin) % kMenuStride) != 0) return 0;
    const U64 count = (end - begin) / kMenuStride;
    if (count == 0 || count > kMaxMenuEntries) return 0;
    const size_t cmpMax = (nameBytes - 1u < 63u) ? (nameBytes - 1u) : 63u;
    auto matchesAt = [&](U64 idx) -> U64 {
        const U64 rec = begin + idx * kMenuStride;
        char cand[64] = {};
        if (!SafeReadAsciiDirect((const char*)rec, cand, cmpMax + 1u)) return 0;
        return (strncmp(cand, name, cmpMax) == 0) ? rec : 0;
    };
    if (place >= 0 && (U64)place < count) {
        if (U64 direct = matchesAt((U64)place)) return direct;
    }
    for (U64 i = 0; i < count; ++i) {
        if (place >= 0 && i == (U64)place) continue;
        if (U64 hit = matchesAt(i)) return hit;
    }
    return 0;
}

bool Ref_IsSpaceVehicleType(U64 vehTypePtr)
{
    if (!IsPointerLikeX64(vehTypePtr)) return false;
    BYTE rocketFlag = 0, craftFlag = 0, reentryFlag = 0;
    if (!SafeReadPod((const BYTE*)(vehTypePtr + VEH_TYPE_ROCKET_FLAG_OFFSET), &rocketFlag) ||
        !SafeReadPod((const BYTE*)(vehTypePtr + VEH_TYPE_SPACECRAFT_FLAG_OFFSET), &craftFlag) ||
        !SafeReadPod((const BYTE*)(vehTypePtr + VEH_TYPE_SPACE_REENTRY_FLAG_OFFSET), &reentryFlag)) {
        return false;
    }
    return rocketFlag != 0 || craftFlag != 0 || reentryFlag != 0;
}

bool Ref_IsSpaceLaunchPadBuilding(U64 building)
{
    if (!IsPointerLikeX64(building)) return false;
    U64 bldType = 0;
    int category = -1;
    int subtype = -1;
    if (!SafeReadPod((const U64*)(building + BLD_TYPE_OFFSET), &bldType) || !IsPointerLikeX64(bldType)) {
        return false;
    }
    if (!SafeReadPod((const int*)(bldType + BLD_TYPE_CAT_OFF), &category) ||
        category != (int)BLD_CAT_SPACE_LAUNCH_PAD) {
        return false;
    }
    if (!SafeReadPod((const int*)(bldType + BLD_TYPE_SUBTYPE_OFF), &subtype)) return false;
    return subtype == (int)BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL ||
           subtype == (int)BLD_SUBTYPE_SPACE_LAUNCH_PAD_BIG;
}
