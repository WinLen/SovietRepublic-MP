// SPDX-License-Identifier: GPL-3.0-only
// See rename.h. Follows SoylentGreed's rename route (RepublicLAN, read against
// the disassembly of 1.1.1.9).
#include "rename.h"
#include "commands.h"
#include "../session/two_states.h"
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define CITY_LIST 0x10348
#define CITY_NAME 0x10
#define CITY_POSITION 4
#define CITY_CHARS 0x20
#define MAX_CITIES 4096
#define ROAD_NETWORK 0xF7A0
#define NETWORK_NODES 0x298
#define JUNCTION_OF_NODE 0xC0
#define JUNCTION_NODES 0x88
#define JUNCTION_NAME 4
#define JUNCTION_NAMED 0xD0
#define JUNCTION_CHARS 0x40
#define MAX_NODES 16
#define MAX_CHARS 0x40
#define MAGIC 0x314D414E

enum { KIND_CITY = 3, KIND_CROSSROAD = 0xA };

#pragma pack(push, 1)
struct RenameMsg {
    unsigned magic, kind, index, check, chars;
    unsigned short text[MAX_CHARS];
};
#pragma pack(pop)

void Rename_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

static unsigned Fnv(const BYTE* b, size_t n, unsigned h = 2166136261u)
{
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h | 1u;
}

static unsigned CityCheck(U64 city) { return Fnv((const BYTE*)(city + CITY_POSITION), 12); }

static unsigned NodeCheck(U64 node)
{
    unsigned w[3] = { 0x4e4f4445u, *(unsigned*)(node + 4), *(unsigned*)(node + 0xc) };
    return Fnv((const BYTE*)w, sizeof(w));
}

// A vector of pointers: begin and count (false: not one, or longer than max).
static bool List(U64 at, unsigned max, U64& begin, unsigned& count)
{
    U64 b = Q(at), e = Q(at + 8);
    begin = b;
    count = 0;
    if (!b && !e) return true;
    if (!b || e < b || (e - b) % 8 || (e - b) / 8 > max) return false;
    count = (unsigned)((e - b) / 8);
    return true;
}

// The field's owner: a listed city (its index) or a junction (its first node's index).
static bool Identify(U64 field, RenameMsg& m, bool* allowed)
{
    *allowed = true;
    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (List(world + CITY_LIST, MAX_CITIES, begin, n))
        for (unsigned i = 0; i < n; i++) {
            U64 city = Q(begin + (U64)i * 8);
            if (city && city + CITY_NAME == field) {
                m.kind = KIND_CITY;
                m.index = i;
                m.check = CityCheck(city);
                *allowed = TwoStates_CheckBuildAllowed(*(float*)(city + CITY_POSITION), *(float*)(city + CITY_POSITION + 8),
                                                       "переименование города");
                return true;
            }
        }
    // A junction: the field is +4 of an object whose first node's +c0 is that object.
    U64 junction = field - JUNCTION_NAME, nodes = 0, roads = Q(world + ROAD_NETWORK);
    unsigned count = 0;
    if (!roads || !List(junction + JUNCTION_NODES, MAX_NODES, nodes, count) || !count) return false;
    U64 first = Q(nodes);
    if (!first || Q(first + JUNCTION_OF_NODE) != junction) return false;
    U64 all = 0;
    unsigned total = 0;
    if (!List(roads + NETWORK_NODES, 10000000, all, total)) return false;
    for (unsigned i = 0; i < total; i++)
        if (Q(all + (U64)i * 8) == first) {
            m.kind = KIND_CROSSROAD;
            m.index = i;
            m.check = NodeCheck(first);
            *allowed = TwoStates_CheckBuildAllowed(*(float*)(first + 4), *(float*)(first + 0xC), "переименование перекрёстка");
            return true;
        }
    return false;
}

static bool GuardedIdentify(U64 field, RenameMsg& m, bool* allowed)
{
    __try {
        return Identify(field, m, allowed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CopyText(U64 text, unsigned length, RenameMsg& m)
{
    __try {
        const unsigned short* t = (const unsigned short*)text;
        unsigned n = 0;
        while (n < length && n < MAX_CHARS && t[n]) n++;
        if (n >= length || n >= MAX_CHARS) n = (length < MAX_CHARS ? length : MAX_CHARS) - 1; // as swprintf truncates
        memcpy(m.text, t, n * 2);
        m.chars = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Rename_Capture(U64 field, U64 length, U64 text)
{
    RenameMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    bool allowed = true;
    if (!field || !text || !GuardedIdentify(field, m, &allowed)) return false;
    if (!allowed) return true;
    unsigned want = m.kind == KIND_CITY ? CITY_CHARS : JUNCTION_CHARS;
    if ((unsigned)length != want || !CopyText(text, want, m)) {
        Logf("republicmp  RENAME: a %s name not shared - unexpected length %u; written on this PC only",
             m.kind == KIND_CITY ? "city" : "crossroad", (unsigned)length);
        return false;
    }
    Commands_Submit(LS_CMD_RENAME, &m, sizeof(m));
    return true;
}

static const char* Apply(const RenameMsg& m)
{
    U64 world = (U64)World(), begin = 0, field = 0, flag = 0;
    unsigned n = 0;
    if (m.kind == KIND_CITY) {
        if (!List(world + CITY_LIST, MAX_CITIES, begin, n) || m.index >= n) return "the city is not here";
        U64 city = Q(begin + (U64)m.index * 8);
        if (!city || CityCheck(city) != m.check) return "the city is not here";
        field = city + CITY_NAME;
    } else {
        U64 roads = Q(world + ROAD_NETWORK);
        if (!roads || !List(roads + NETWORK_NODES, 10000000, begin, n) || m.index >= n) return "the crossroad is not here";
        U64 node = Q(begin + (U64)m.index * 8);
        if (!node || NodeCheck(node) != m.check) return "the crossroad is not here";
        U64 junction = Q(node + JUNCTION_OF_NODE);
        if (!junction) return "the node has no crossroad here";
        field = junction + JUNCTION_NAME;
        flag = junction + JUNCTION_NAMED;
    }
    unsigned short t[MAX_CHARS + 1] = {};
    memcpy(t, m.text, m.chars * 2);
    memcpy((void*)field, t, (m.chars + 1) * 2);
    if (flag) *(BYTE*)flag = 1;
    return nullptr;
}

bool Rename_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(RenameMsg)) return false;
    RenameMsg m;
    memcpy(&m, data, sizeof(m));
    unsigned limit = m.kind == KIND_CITY ? CITY_CHARS : m.kind == KIND_CROSSROAD ? JUNCTION_CHARS : 0;
    if (m.magic != MAGIC || !limit || m.chars >= limit) return false;
    const char* why;
    __try {
        why = Apply(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while writing";
    }
    Logf("republicmp  RENAME: %s name (%u characters): %s", m.kind == KIND_CITY ? "city" : "crossroad", m.chars, why ? why : "made");
    return why == nullptr;
}
