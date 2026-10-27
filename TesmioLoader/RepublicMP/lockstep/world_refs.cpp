// SPDX-License-Identifier: GPL-3.0-only
// See world_refs.h.
#include "world_refs.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <string.h>

#define LINES_OFFSET WORLD_LINES_OFFSET
#define VEHICLES_OFFSET WORLD_VEHICLES_OFFSET
#define BUILDINGS_OFFSET WORLD_BUILDINGS_OFFSET
#define CONSTRUCTIONS_OFFSET WORLD_CONSTRUCTIONS_OFFSET
#define NODE_LIST 0x298

static const unsigned kNodeFields[2] = { 0xF908, 0xFA28 };

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

unsigned Ref_IndexInVector(U64 vector, U64 p)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    if (!p || !begin || end < begin || (end - begin) / 8 > 1000000) return 0xFFFFFFFFu;
    for (U64 at = begin; at < end; at += 8)
        if (Q(at) == p) return (unsigned)((at - begin) / 8);
    return 0xFFFFFFFFu;
}

U64 Ref_AtVectorIndex(U64 vector, unsigned index)
{
    U64 begin = Q(vector), end = Q(vector + 8);
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
    return false;
}

bool Ref_DefPtrAt(unsigned char tag, unsigned index, U64& p)
{
    p = 0;
    if (tag == 0) return index == 0;
    if (tag > 2) return false;
    unsigned table = tag == 1 ? WORLD_RESOURCES_OFFSET : WORLD_VEHICLE_TYPES_OFFSET;
    unsigned stride = tag == 1 ? WORLD_RESOURCE_STRIDE : WORLD_VEHICLE_TYPE_STRIDE;
    U64 v[2] = {};
    if (!SafeMemcpyPod(v, World() + table, 16) || !v[0] || v[1] < v[0] || index >= (v[1] - v[0]) / stride) return false;
    p = v[0] + (U64)index * stride;
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
    if (r.list != kNodeFields[0] && r.list != kNodeFields[1]) return 0;
    U64 net = Q((U64)World() + r.list);
    U64 n = net ? At(net + NODE_LIST, r.place) : 0;
    return n && Ref_NodeCheck(n) == r.check ? n : 0;
}
