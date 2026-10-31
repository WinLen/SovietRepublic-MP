// SPDX-License-Identifier: GPL-3.0-only
// See rename.h. Follows SoylentGreed's rename route (RepublicLAN, read against
// the disassembly of 1.1.1.9).
#include "rename.h"
#include "commands.h"
#include "../session/two_states.h"
#include "../ui/lang.h"
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

enum { KIND_CITY = 3, KIND_CROSSROAD = 0xA, KIND_CITY_DELETE = 0xD, KIND_RESEARCH = 0xE };

#define CITY_DELETE_RVA 0x5BF670

#pragma pack(push, 1)
struct RenameMsg {
    unsigned magic, kind, index, check, chars;
    unsigned short text[MAX_CHARS];
};
#pragma pack(pop)

typedef __int64 (__fastcall *FnCityDelete)(void* rcx, void* rdx, void* r8, void* r9);
static FnCityDelete o_cityDelete = nullptr;
static bool g_inReplayDelete = false;

static bool ApplyCityDelete(const RenameMsg& m);
static int FindCityIndex(void* p);

static __int64 __fastcall H_CityDelete(void* rcx, void* rdx, void* r8, void* r9);

bool Rename_InstallHooks()
{
    if (!g_exeBase) return false;
    int ok = InstallInlineHook(g_exeBase + CITY_DELETE_RVA, (void*)&H_CityDelete,
                               (void**)&o_cityDelete, nullptr, 0, "republicmp city_delete");
    Logf("republicmp  CITY: city delete hook installed: %d", ok);
    return ok != 0;
}

void Rename_Bind(const TsmHost* host)
{
    TsmBind(host);
    Rename_InstallHooks();
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
                                                       Tr("переименование города", "renaming city"));
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
            *allowed = TwoStates_CheckBuildAllowed(*(float*)(first + 4), *(float*)(first + 0xC), Tr("переименование перекрёстка", "renaming junction"));
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

static int FindCityIndex(void* p)
{
    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (!List(world + CITY_LIST, MAX_CITIES, begin, n) || !n) return -1;
    U64 target = (U64)p;
    for (unsigned i = 0; i < n; i++) {
        if (Q(begin + (U64)i * 8) == target) return (int)i;
    }
    if (target < n) return (int)target;
    if (target > 0x10000 && (target & 0x7) == 0) {
        __try {
            const U64* words = (const U64*)target;
            for (int w = 0; w < 64; ++w) {
                U64 candidate = words[w];
                for (unsigned i = 0; i < n; i++) {
                    if (Q(begin + (U64)i * 8) == candidate) return (int)i;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return -1;
}

static __int64 __fastcall H_CityDelete(void* rcx, void* rdx, void* r8, void* r9)
{
    if (g_inReplayDelete || !Commands_TakeLocal()) {
        return o_cityDelete ? o_cityDelete(rcx, rdx, r8, r9) : 0;
    }
    int idx = FindCityIndex(rcx);
    if (idx < 0) idx = FindCityIndex(rdx);
    if (idx < 0) {
        Logf("republicmp  CITY: delete for unknown target=%p, local only", rcx);
        return o_cityDelete ? o_cityDelete(rcx, rdx, r8, r9) : 0;
    }

    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (!List(world + CITY_LIST, MAX_CITIES, begin, n) || (unsigned)idx >= n) return 0;
    U64 city = Q(begin + (U64)idx * 8);
    if (!city) return 0;

    bool allowed = TwoStates_CheckBuildAllowed(*(float*)(city + CITY_POSITION),
                                               *(float*)(city + CITY_POSITION + 8),
                                               Tr("удаление города", "deleting city"));
    if (!allowed) {
        Logf("republicmp  CITY: delete blocked by border check");
        return 0;
    }

    RenameMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.kind = KIND_CITY_DELETE;
    m.index = (unsigned)idx;
    m.check = CityCheck(city);

    Commands_Submit(LS_CMD_CITY_DELETE, &m, sizeof(m));
    Logf("republicmp  CITY: captured delete of city #%d", idx);
    return 0;
}

static bool ApplyCityDelete(const RenameMsg& m)
{
    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (!List(world + CITY_LIST, MAX_CITIES, begin, n) || m.index >= n) return false;
    U64 city = Q(begin + (U64)m.index * 8);
    if (!city || CityCheck(city) != m.check) return false;

    if (o_cityDelete) {
        g_inReplayDelete = true;
        __try {
            o_cityDelete((void*)city, nullptr, nullptr, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_inReplayDelete = false;
    }

    U64 curBegin = 0;
    unsigned curCount = 0;
    if (List(world + CITY_LIST, MAX_CITIES, curBegin, curCount) && m.index < curCount) {
        if (Q(curBegin + (U64)m.index * 8) == city) {
            U64* ptrs = (U64*)curBegin;
            for (unsigned j = m.index; j + 1 < curCount; j++) {
                ptrs[j] = ptrs[j + 1];
            }
            *(U64*)(world + CITY_LIST + 8) = curBegin + (U64)(curCount - 1) * 8;
        }
    }
    Logf("republicmp  CITY: deleted city at index %u", m.index);
    return true;
}

bool Rename_ApplyDeleteCommand(const void* data, size_t size)
{
    if (size != sizeof(RenameMsg)) return false;
    RenameMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.kind != KIND_CITY_DELETE) return false;
    return ApplyCityDelete(m);
}

bool Rename_ApplyCommand(const void* data, size_t size)
{
    if (size == sizeof(LsResearchCommand)) {
        return TwoStates_ApplyResearchCommand(*(const LsResearchCommand*)data);
    }
    if (size != sizeof(RenameMsg)) return false;
    RenameMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC) return false;
    if (m.kind == KIND_CITY_DELETE) {
        return ApplyCityDelete(m);
    }
    if (m.kind == KIND_RESEARCH) {
        return TwoStates_ApplyResearchCommand(*(const LsResearchCommand*)m.text);
    }
    unsigned limit = m.kind == KIND_CITY ? CITY_CHARS : m.kind == KIND_CROSSROAD ? JUNCTION_CHARS : 0;
    if (!limit || m.chars >= limit) return false;
    const char* why;
    __try {
        why = Apply(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while writing";
    }
    Logf("republicmp  RENAME: %s name (%u characters): %s", m.kind == KIND_CITY ? "city" : "crossroad", m.chars, why ? why : "made");
    return why == nullptr;
}
