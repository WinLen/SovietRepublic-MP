// SPDX-License-Identifier: GPL-3.0-only
// See world_refs.h.
#include "world_refs.h"

#define WORLD_RVA 0x9D4F10
#define LINES_OFFSET 0x12800
#define VEHICLES_OFFSET 0x12810
#define BUILDINGS_OFFSET 0x11B08
#define NODE_LIST 0x298

static const unsigned kNodeFields[2] = { 0xF908, 0xFA28 };

const RouteRef kNoRef = { 0, -1, 0 };

void WorldRefs_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

static unsigned Fnv(const unsigned* words, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 4; b++) {
            h ^= (words[i] >> (8 * b)) & 0xffu;
            h *= 16777619u;
        }
    return h | 1u;
}

static bool Find(U64 vector, U64 object, int* place)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    if (!begin || end < begin) return false;
    for (U64 at = begin; at < end; at += 8)
        if (Q(at) == object) {
            *place = (int)((at - begin) / 8);
            return true;
        }
    return false;
}

static U64 At(U64 vector, int place)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    if (!begin || place < 0 || end < begin || (U64)place >= (end - begin) / 8) return 0;
    return Q(begin + (U64)place * 8);
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
    r.list = 0;
    if (!building || !Find((U64)World() + BUILDINGS_OFFSET, building, &r.place)) return false;
    r.check = *(unsigned*)building;
    return true;
}

U64 Ref_BuildingAt(const RouteRef& r)
{
    U64 b = At((U64)World() + BUILDINGS_OFFSET, r.place);
    return b && *(unsigned*)b == r.check ? b : 0;
}

static unsigned LineCheck(U64 line)
{
    unsigned w[3] = { 0x4c494e45u, *(unsigned*)(line + 4), *(unsigned*)(line + 8) };
    return Fnv(w, 3);
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

static unsigned NodeCheck(U64 node)
{
    unsigned w[3] = { 0x4e4f4445u, *(unsigned*)(node + 4), *(unsigned*)(node + 0xc) };
    return Fnv(w, 3);
}

bool Ref_Node(U64 node, RouteRef& r)
{
    for (unsigned field : kNodeFields) {
        U64 net = Q((U64)World() + field);
        if (net && Find(net + NODE_LIST, node, &r.place)) {
            r.list = field;
            r.check = NodeCheck(node);
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
    return n && NodeCheck(n) == r.check ? n : 0;
}

