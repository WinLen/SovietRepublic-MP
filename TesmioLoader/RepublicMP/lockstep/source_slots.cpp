// SPDX-License-Identifier: GPL-3.0-only
// See source_slots.h. Follows SoylentGreed's add-source route (RepublicLAN,
// link tool kind 7, read against the disassembly of 1.1.1.9).
#include "source_slots.h"
#include "commands.h"
#include "world_refs.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "../session/two_states.h"
#include "../ui/lang.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

#define WORLD_RVA 0x9D4F10
#define WINDOW_BUILDING 0xD298   // the building of the window the tool serves
#define BUILDINGS_OFFSET WORLD_BUILDINGS_OFFSET
#define CONSTRUCTIONS_OFFSET WORLD_CONSTRUCTIONS_OFFSET
#define DOWN_RVA 0xA54E90
#define PRESS_RVA 0xA54E91
#define ENTER_RVA 0xA54CAC
#define KEYPAD_ENTER_RVA 0xA54D2C
#define SLOTS_OFFSET 0xF78
#define SLOT_BYTES 0x10
#define MAX_SLOTS 64
#ifndef MAX_BUILDINGS
#define MAX_BUILDINGS SOURCE_SLOTS_MAX_BUILDINGS
#endif
#define MAX_LIST 1000000
#define ADD_SOURCE_RVA 0x1D5120
#define COPY_SOURCE_RVA 0x1D67A0
#define MAGIC 0x31535253

static const unsigned char kToolEntry[16] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x55,
                                              0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41 };
static const unsigned char kCopyToolEntry[16] = { 0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x81, 0xec, 0xc0,
                                                  0x00, 0x00, 0x00, 0x80, 0xb9, 0xbc, 0x0f, 0x00 };
static int g_ready = -1;
typedef U64 (*ToolFn)(U64 world, U64 a2);
static ToolFn g_testToolOverride = nullptr;

#pragma pack(push, 1)
struct SlotEntry {
    unsigned char slot, has;
    unsigned short pad;
    RouteRef source;
};
struct SourceMsg {
    unsigned magic;
    RouteRef building;
    unsigned slots; // the building's slot count (the shape must match)
    unsigned count;
    SlotEntry entries[MAX_SLOTS];
};
#pragma pack(pop)

void SourceSlots_Bind(const TsmHost* host)
{
    TsmBind(host);
}

void SourceSlots_SetToolCallbackForTest(ToolFn fn)
{
    g_testToolOverride = fn;
    g_ready = -1;
}

bool SourceSlots_Ready()
{
    if (g_testToolOverride) return true;
    if (g_ready != 1) {
        bool ok = memcmp(g_exeBase + ADD_SOURCE_RVA, kToolEntry, sizeof(kToolEntry)) == 0 &&
                  memcmp(g_exeBase + COPY_SOURCE_RVA, kCopyToolEntry, sizeof(kCopyToolEntry)) == 0;
        if (!ok && g_ready < 0)
            Logf("republicmp  SOURCES: the add/copy-source tool's routine differs - the tool is put away in a session");
        g_ready = ok ? 1 : 0;
    }
    return g_ready == 1;
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

struct Slots {
    U64 building;
    unsigned count;
    U64 source[MAX_SLOTS];
};

// A building's slot sources (false: unreadable or too many).
static bool ReadSlots(U64 building, Slots& s)
{
    U64 v[2] = {};
    s.building = building;
    s.count = 0;
    if (!building || !SafeMemcpyPod(v, (const void*)(building + SLOTS_OFFSET), 16)) return false;
    if (!v[0] && !v[1]) return true;
    if (!v[0] || v[1] < v[0] || (v[1] - v[0]) % SLOT_BYTES || (v[1] - v[0]) / SLOT_BYTES > MAX_SLOTS) return false;
    s.count = (unsigned)((v[1] - v[0]) / SLOT_BYTES);
    for (unsigned i = 0; i < s.count; i++)
        if (!SafeMemcpyPod(&s.source[i], (const void*)(v[0] + (U64)i * SLOT_BYTES), 8)) return false;
    return true;
}

static bool WriteSlot(U64 building, unsigned slot, U64 source)
{
    U64 v[2] = {};
    if (!SafeMemcpyPod(v, (const void*)(building + SLOTS_OFFSET), 16) || !v[0] || v[1] < v[0] || slot >= (v[1] - v[0]) / SLOT_BYTES) return false;
    return SafeWritePod((void*)(v[0] + (U64)slot * SLOT_BYTES), source);
}

static bool GuardedRef(U64 building, RouteRef& r)
{
    __try {
        return Ref_Building(building, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static U64 GuardedAt(const RouteRef& r)
{
    __try {
        return Ref_BuildingAt(r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The slots that changed since `before`: put back at once and sent as one command.
static void Take(const Slots& before, const char* where)
{
    Slots now;
    if (!ReadSlots(before.building, now) || now.count != before.count) return; // a slot added or removed: not compared
    SourceMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.slots = now.count;
    const char* why = nullptr;
    for (unsigned i = 0; i < now.count; i++) {
        if (now.source[i] == before.source[i]) continue;
        if (!why && m.count == 0 && !GuardedRef(before.building, m.building)) why = "the building is not in the world's list";
        SlotEntry& e = m.entries[m.count++];
        e.slot = (unsigned char)i;
        e.has = now.source[i] ? 1 : 0;
        e.source = kNoRef;
        if (!why && now.source[i] && !GuardedRef(now.source[i], e.source)) why = "the source is not in the world's building list";
        WriteSlot(before.building, i, before.source[i]);
    }
    if (!m.count) return;
    if (!TwoStates_CheckBuildingControlAllowed((const BYTE*)before.building, Tr("назначение источника снабжения чужому зданию", "assigning supply source to a foreign building"))) return;
    if (why) {
        Logf("republicmp  SOURCES: %u slot(s) changed by %s not shared - %s; nothing is done", m.count, where, why);
        return;
    }
    Commands_Submit(LS_CMD_SOURCE_SLOTS, &m, sizeof(m));
}

// ---------------------------------------------------------------------------
// The clicking PC: the tool and the windows
// ---------------------------------------------------------------------------

static void CollectSlotBuildings(std::vector<Slots>& out)
{
    out.clear();
    U64 winBld = 0;
    if (SafeMemcpyPod(&winBld, World() + WINDOW_BUILDING, 8) && winBld) {
        Slots s;
        if (ReadSlots(winBld, s) && s.count) out.push_back(s);
    }
    const unsigned offsets[2] = { BUILDINGS_OFFSET, CONSTRUCTIONS_OFFSET };
    for (int idx = 0; idx < 2; idx++) {
        U64 v[2] = {};
        if (!SafeMemcpyPod(v, World() + offsets[idx], 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) % 8 || (v[1] - v[0]) / 8 > MAX_LIST) continue;
        size_t count = (size_t)((v[1] - v[0]) / 8);
        if (!count) continue;
        std::vector<U64> buildings(count);
        if (!SafeMemcpyPod(buildings.data(), (const void*)v[0], count * 8)) continue;
        for (U64 b : buildings) {
            if (!b || b == winBld || out.size() >= MAX_BUILDINGS) continue;
            Slots s;
            if (ReadSlots(b, s) && s.count) out.push_back(s);
        }
    }
}

U64 SourceSlots_AddSourceTool(U64 world, U64 a2)
{
    ToolFn tool = g_testToolOverride ? g_testToolOverride : (ToolFn)(g_exeBase + ADD_SOURCE_RVA);
    if (!Commands_TakeLocal() || !SourceSlots_Ready()) return tool(world, a2);
    Slots before;
    U64 building = 0;
    bool held = SafeMemcpyPod(&building, (const void*)(world + WINDOW_BUILDING), 8) && building && ReadSlots(building, before);
    U64 r = tool(world, a2);
    if (held) Take(before, "the add-source tool");
    return r;
}

U64 SourceSlots_CopySourceTool(U64 world, U64 a2)
{
    ToolFn tool = g_testToolOverride ? g_testToolOverride : (ToolFn)(g_exeBase + COPY_SOURCE_RVA);
    if (!Commands_TakeLocal() || !SourceSlots_Ready()) return tool(world, a2);
    std::vector<Slots> beforeList;
    CollectSlotBuildings(beforeList);
    U64 r = tool(world, a2);
    for (const Slots& b : beforeList)
        Take(b, "the copy-source tool");
    return r;
}

static std::vector<Slots> g_held;
static bool g_taken = false;
static unsigned long long g_epoch = 0;

static bool Clicked()
{
    float wheel = 0.0f;
    SafeReadPod((const float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA), &wheel);
    return g_exeBase[DOWN_RVA] || g_exeBase[PRESS_RVA] || g_exeBase[ENTER_RVA] || g_exeBase[KEYPAD_ENTER_RVA] ||
           (isfinite(wheel) && wheel != 0.0f);
}

void SourceSlots_Before(bool simBusy)
{
    g_taken = false;
    g_held.clear();
    if (simBusy || !Commands_TakeUi() || !Clicked()) return;
    CollectSlotBuildings(g_held);
    g_epoch = Commands_ApplyEpoch();
    g_taken = !g_held.empty();
}

void SourceSlots_After(bool simBusy)
{
    if (!g_taken) return;
    g_taken = false;
    if (!simBusy && Commands_TakeUi() && Commands_ApplyEpoch() == g_epoch)
        for (const Slots& s : g_held) Take(s, "a window");
    g_held.clear();
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

bool SourceSlots_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SourceMsg)) return false;
    SourceMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || !m.count || m.count > MAX_SLOTS) return false;
    const char* why = nullptr;
    U64 building = GuardedAt(m.building);
    Slots now;
    if (!building) why = "the building is gone";
    else if (!ReadSlots(building, now) || now.count != m.slots) why = "its slots differ here";
    U64 sources[MAX_SLOTS] = {};
    for (unsigned i = 0; i < m.count && !why; i++) {
        if (m.entries[i].slot >= now.count) why = "a slot is outside";
        else if (m.entries[i].has && !(sources[i] = GuardedAt(m.entries[i].source))) why = "a source building is gone";
    }
    for (unsigned i = 0; i < m.count && !why; i++)
        if (!WriteSlot(building, m.entries[i].slot, sources[i])) why = "a fault while writing";
    Logf("republicmp  SOURCES: %u slot(s) of building #%d: %s", m.count, m.building.place, why ? why : "made");
    return why == nullptr;
}
