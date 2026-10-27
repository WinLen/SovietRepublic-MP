// SPDX-License-Identifier: GPL-3.0-only
// See source_slots.h. Follows SoylentGreed's add-source route (RepublicLAN,
// link tool kind 7, read against the disassembly of 1.1.1.9).
#include "source_slots.h"
#include "commands.h"
#include "world_refs.h"
#include "../core/safe_mem.h"
#include "../session/two_states.h"
#include <algorithm>
#include <string.h>
#include <vector>

#define WORLD_RVA 0x9D4F10
#define WINDOW_BUILDING 0xD298   // the building of the window the tool serves
#define BUILDINGS_OFFSET 0x11B08
#define WINDOWS_RVA 0x9E19D0     // the open windows
#define WINDOW_OBJECT 0x240
#define WINDOW_KIND 0x248
#define WINDOW_PARENT 0x5C8      // a selection window's parent window
#define PICKER_WINDOW 0xD
#define DOWN_RVA 0xA54E90
#define PRESS_RVA 0xA54E91
#define ENTER_RVA 0xA54CAC
#define KEYPAD_ENTER_RVA 0xA54D2C
#define SLOTS_OFFSET 0xF78
#define SLOT_BYTES 0x10
#define MAX_SLOTS 64
#define MAX_BUILDINGS 8
#define MAX_WINDOWS 4096
#define MAX_LIST 1000000
#define ADD_SOURCE_RVA 0x1D51A0
#define MAGIC 0x31535253

static const unsigned char kToolEntry[16] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x55,
                                              0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41 };
static int g_ready = -1;

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

bool SourceSlots_Ready()
{
    if (g_ready < 0) {
        g_ready = memcmp(g_exeBase + ADD_SOURCE_RVA, kToolEntry, sizeof(kToolEntry)) == 0 ? 1 : 0;
        if (!g_ready) Logf("republicmp  SOURCES: the add-source tool's routine differs - the tool is put away in a session");
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
    if (!TwoStates_CheckBuildingControlAllowed((const BYTE*)before.building, "назначение источника снабжения чужому зданию")) return;
    if (why) {
        Logf("republicmp  SOURCES: %u slot(s) changed by %s not shared - %s; nothing is done", m.count, where, why);
        return;
    }
    Commands_Submit(LS_CMD_SOURCE_SLOTS, &m, sizeof(m));
}

// ---------------------------------------------------------------------------
// The clicking PC: the tool
// ---------------------------------------------------------------------------

typedef U64 (*ToolFn)(U64 world, U64 a2);

U64 SourceSlots_AddSourceTool(U64 world, U64 a2)
{
    ToolFn tool = (ToolFn)(g_exeBase + ADD_SOURCE_RVA);
    if (!Commands_TakeLocal() || !SourceSlots_Ready()) return tool(world, a2);
    Slots before;
    U64 building = 0;
    bool held = SafeMemcpyPod(&building, (const void*)(world + WINDOW_BUILDING), 8) && building && ReadSlots(building, before);
    U64 r = tool(world, a2);
    if (held) Take(before, "the add-source tool");
    return r;
}

// ---------------------------------------------------------------------------
// The clicking PC: the windows
// ---------------------------------------------------------------------------

static std::vector<Slots> g_held;
static bool g_taken = false;
static unsigned long long g_epoch = 0;

static bool Clicked()
{
    return g_exeBase[DOWN_RVA] || g_exeBase[PRESS_RVA] || g_exeBase[ENTER_RVA] || g_exeBase[KEYPAD_ENTER_RVA];
}

static void Hold(U64 building, const std::vector<U64>& buildings)
{
    if (!building || g_held.size() >= MAX_BUILDINGS || !std::binary_search(buildings.begin(), buildings.end(), building)) return;
    for (const Slots& s : g_held)
        if (s.building == building) return;
    Slots s;
    if (ReadSlots(building, s) && s.count) g_held.push_back(s);
}

// The window's building and the buildings of open windows (a selection window counts as its parent).
static void Shown()
{
    U64 v[2] = {};
    std::vector<U64> buildings;
    if (!SafeMemcpyPod(v, World() + BUILDINGS_OFFSET, 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) % 8 || (v[1] - v[0]) / 8 > MAX_LIST) return;
    buildings.resize((size_t)((v[1] - v[0]) / 8));
    if (buildings.empty() || !SafeMemcpyPod(buildings.data(), (const void*)v[0], buildings.size() * 8)) return;
    std::sort(buildings.begin(), buildings.end());
    U64 building = 0;
    if (SafeMemcpyPod(&building, World() + WINDOW_BUILDING, 8)) Hold(building, buildings);
    U64 w[2] = {};
    if (!SafeMemcpyPod(w, g_exeBase + WINDOWS_RVA, 16) || !w[0] || w[1] < w[0] || (w[1] - w[0]) / 8 > MAX_WINDOWS) return;
    std::vector<U64> windows((size_t)((w[1] - w[0]) / 8));
    if (windows.empty() || !SafeMemcpyPod(windows.data(), (const void*)w[0], windows.size() * 8)) return;
    for (U64 win : windows) {
        U64 object = 0, parent = 0;
        unsigned kind = 0;
        if (!win || !SafeMemcpyPod(&kind, (const void*)(win + WINDOW_KIND), 4) || !SafeMemcpyPod(&object, (const void*)(win + WINDOW_OBJECT), 8)) continue;
        Hold(object, buildings);
        if (kind == PICKER_WINDOW && SafeMemcpyPod(&parent, (const void*)(win + WINDOW_PARENT), 8) && parent &&
            SafeMemcpyPod(&object, (const void*)(parent + WINDOW_OBJECT), 8))
            Hold(object, buildings);
    }
}

void SourceSlots_Before(bool simBusy)
{
    g_taken = false;
    g_held.clear();
    if (simBusy || !Commands_TakeUi() || !Clicked()) return;
    Shown();
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
