// SPDX-License-Identifier: GPL-3.0-only
// See hover_tools.h. Follows SoylentGreed's hover tool route (RepublicLAN,
// read against the disassembly of 1.1.1.9).
#include "hover_tools.h"
#include "commands.h"
#include "world_refs.h"
#include "../core/safe_mem.h"
#include "../session/two_states.h"
#include "../session/save_sync.h"
#include "../ui/native_menu.h"
#include "../ui/lang.h"
#include "../world_sync/view_list_scope.h"
#include <math.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define PRESS_RVA 0xA54E91
#define SELECTED_OFFSET 0xD428
#define MENU_OFFSET 0xD280
#define MENU_STRIDE 0x2D0
#define NAME_BYTES 32
#define FLAG_OFFSET 0xFB9
#define ROAD_NETWORK 0xF7A0
#define RAIL_NETWORK 0xF908
#define NODES 0x298
#define EDGES 0x2B0
#define BUILDINGS 0x11B08
#define SCOPE_RADIUS 80.0f
static const unsigned kMenus[3] = { 0xFBC, 0xFBE, 0xFC3 };

#define KINDS 6
static const unsigned kRoutines[KINDS] = { 0x417A70, 0x4181D0, 0x417D80, 0x4184C0, 0x418F90, 0x4182D0 };
static const unsigned char kEntries[KINDS][16] = {
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0xbc, 0x0f, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 },
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0xbc, 0x0f, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 },
    { 0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x80, 0xb9, 0xbc, 0x0f, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9 },
    { 0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d },
    { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec, 0x48 },
    { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0xbc, 0x0f, 0x00, 0x00 },
};
static const char* const kKindText[KINDS] = { "waypoint", "sign erase", "sign", "rail signal", "rail signal removal", "junction" };
static const char* HoverKindToastDesc(int kind)
{
    switch (kind) {
    case 0: return Tr("установка путевой точки", "placing waypoint");
    case 1: return Tr("снос дорожного знака", "removing road sign");
    case 2: return Tr("установка дорожного знака", "placing road sign");
    case 3: return Tr("установка ж/д светофора", "placing rail signal");
    case 4: return Tr("снос ж/д светофора", "removing rail signal");
    case 5: return Tr("настройка перекрёстка", "configuring junction");
    default: return Tr("действие с дорогой/сетью", "road/network action");
    }
}
#define RAIL_PLACE 3
#define RAIL_REMOVE 4
#define JUNCTION 5
static bool IsRail(int kind) { return kind == RAIL_PLACE || kind == RAIL_REMOVE; }

// The pick state (world +f648..+f6e8, the cursor +f6c, +f90): the pointers as references.
enum { REF_NONE = 0, REF_NODE = 1, REF_EDGE = 2, REF_ROAD_NODE = 3, REF_ROAD_EDGE = 4, REF_BUILDING = 5 };
static const unsigned kRefOffsets[8] = { 0xF648, 0xF650, 0xF6A0, 0xF6D8, 0xF680, 0xF690, 0xF6C8, 0xF6D0 };

#pragma pack(push, 1)
struct PickRef {
    unsigned tag, index, check;
};
struct PickState {
    unsigned char b658, b668, b6e8, b698;
    unsigned f678, f6e0, f6e4, f90, f6a8, f688;
    unsigned nodePoint[3], edgePoint[3], cursor[3];
    PickRef refs[8];
};
struct HoverMsg {
    int kind, mode;
    unsigned char hover;
    unsigned char pad[3];
    int record;
    char name[NAME_BYTES];
    PickState pick;
};
#pragma pack(pop)

typedef U64 (*ToolFn)(U64 world, U64 a2);

static int g_ready[KINDS] = { -1, -1, -1, -1, -1, -1 };

void HoverTools_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool HoverTools_Ready(int kind)
{
    if (kind < 0 || kind >= KINDS) return false;
    if (g_ready[kind] < 0) {
        g_ready[kind] = SafeBytesMatch(g_exeBase + kRoutines[kind], kEntries[kind], 16) ? 1 : 0;
        if (!g_ready[kind]) Logf("republicmp  HOVER: the %s tool's routine differs - it stays as it was", kKindText[kind]);
    }
    return g_ready[kind] == 1;
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

static bool EdgeCheck(U64 network, U64 edge, unsigned* check)
{
    U64 a = Q(edge + 0x20), z = Q(edge + 0x28), pb = Q(edge + 8), pe = Q(edge + 0x10);
    if (pe < pb) return false;
    unsigned na = a ? Ref_IndexInVector(network + NODES, a) : 0xfffffffeu;
    unsigned nz = z ? Ref_IndexInVector(network + NODES, z) : 0xfffffffeu;
    if (na == 0xFFFFFFFFu || nz == 0xFFFFFFFFu) return false;
    unsigned w[4] = { 0x45444745u, na, nz, (unsigned)(pe - pb) };
    *check = Ref_FnvWords(w, 4);
    return true;
}

static unsigned BuildingCheck(U64 b)
{
    unsigned w[3] = { 0x42444e47u, *(unsigned*)(b + 0x3a0), *(unsigned*)(b + 0x3a8) };
    return Ref_FnvWords(w, 3);
}

// A pointer of the pick state as a reference (none for anything it cannot place).
static void ToRef(U64 world, U64 network, U64 roads, U64 p, PickRef& r)
{
    r = { REF_NONE, 0, 0 };
    if (!p) return;
    unsigned i, check = 0;
    if ((i = Ref_IndexInVector(network + NODES, p)) != 0xFFFFFFFFu) r = { REF_NODE, i, Ref_NodeCheck(p) };
    else if ((i = Ref_IndexInVector(network + EDGES, p)) != 0xFFFFFFFFu && EdgeCheck(network, p, &check)) r = { REF_EDGE, i, check };
    else if (roads && roads != network && (i = Ref_IndexInVector(roads + NODES, p)) != 0xFFFFFFFFu) r = { REF_ROAD_NODE, i, Ref_NodeCheck(p) };
    else if (roads && roads != network && (i = Ref_IndexInVector(roads + EDGES, p)) != 0xFFFFFFFFu && EdgeCheck(roads, p, &check)) r = { REF_ROAD_EDGE, i, check };
    else if ((i = Ref_IndexInVector(world + BUILDINGS, p)) != 0xFFFFFFFFu) r = { REF_BUILDING, i, BuildingCheck(p) };
}

static bool FromRef(U64 world, U64 network, U64 roads, const PickRef& r, U64* out)
{
    *out = 0;
    unsigned check = 0;
    switch (r.tag) {
    case REF_NONE: return true;
    case REF_NODE: *out = Ref_AtVectorIndex(network + NODES, r.index); return *out && Ref_NodeCheck(*out) == r.check;
    case REF_EDGE: *out = Ref_AtVectorIndex(network + EDGES, r.index); return *out && EdgeCheck(network, *out, &check) && check == r.check;
    case REF_ROAD_NODE: *out = roads ? Ref_AtVectorIndex(roads + NODES, r.index) : 0; return *out && Ref_NodeCheck(*out) == r.check;
    case REF_ROAD_EDGE: *out = roads ? Ref_AtVectorIndex(roads + EDGES, r.index) : 0; return *out && EdgeCheck(roads, *out, &check) && check == r.check;
    case REF_BUILDING: *out = Ref_AtVectorIndex(world + BUILDINGS, r.index); return *out && BuildingCheck(*out) == r.check;
    }
    return false;
}

// This PC's pick state as it is in memory.
struct RawPick {
    unsigned char b658, b668, b6e8, b698;
    unsigned f678, f6e0, f6e4, f90, f6a8, f688;
    unsigned nodePoint[3], edgePoint[3], cursor[3];
    U64 refs[8];
};

static void ReadPick(U64 world, RawPick& r)
{
    r.b658 = *(BYTE*)(world + 0xF658);
    r.b668 = *(BYTE*)(world + 0xF668);
    r.b6e8 = *(BYTE*)(world + 0xF6E8);
    r.b698 = *(BYTE*)(world + 0xF698);
    r.f678 = *(unsigned*)(world + 0xF678);
    r.f6e0 = *(unsigned*)(world + 0xF6E0);
    r.f6e4 = *(unsigned*)(world + 0xF6E4);
    r.f90 = *(unsigned*)(world + 0xF90);
    r.f6a8 = *(unsigned*)(world + 0xF6A8);
    r.f688 = *(unsigned*)(world + 0xF688);
    memcpy(r.nodePoint, (const void*)(world + 0xF65C), 12);
    memcpy(r.edgePoint, (const void*)(world + 0xF66C), 12);
    memcpy(r.cursor, (const void*)(world + 0xF6C), 12);
    for (int i = 0; i < 8; i++) r.refs[i] = Q(world + kRefOffsets[i]);
}

static void WritePick(U64 world, const RawPick& r)
{
    *(BYTE*)(world + 0xF658) = r.b658;
    *(BYTE*)(world + 0xF668) = r.b668;
    *(BYTE*)(world + 0xF6E8) = r.b6e8;
    *(BYTE*)(world + 0xF698) = r.b698;
    *(unsigned*)(world + 0xF678) = r.f678;
    *(unsigned*)(world + 0xF6E0) = r.f6e0;
    *(unsigned*)(world + 0xF6E4) = r.f6e4;
    *(unsigned*)(world + 0xF90) = r.f90;
    *(unsigned*)(world + 0xF6A8) = r.f6a8;
    *(unsigned*)(world + 0xF688) = r.f688;
    memcpy((void*)(world + 0xF65C), r.nodePoint, 12);
    memcpy((void*)(world + 0xF66C), r.edgePoint, 12);
    memcpy((void*)(world + 0xF6C), r.cursor, 12);
    for (int i = 0; i < 8; i++) *(U64*)(world + kRefOffsets[i]) = r.refs[i];
}

static bool Capture(int kind, U64 world, HoverMsg& m, const char** why)
{
    __try {
        U64 network = Q(world + (IsRail(kind) ? RAIL_NETWORK : ROAD_NETWORK));
        U64 roads = Q(world + ROAD_NETWORK);
        if (!network) {
            *why = IsRail(kind) ? "no rail network" : "no road network";
            return false;
        }
        RawPick r;
        ReadPick(world, r);
        PickState& s = m.pick;
        s.b658 = r.b658; s.b668 = r.b668; s.b6e8 = r.b6e8; s.b698 = r.b698;
        s.f678 = r.f678; s.f6e0 = r.f6e0; s.f6e4 = r.f6e4; s.f90 = r.f90; s.f6a8 = r.f6a8; s.f688 = r.f688;
        memcpy(s.nodePoint, r.nodePoint, 12);
        memcpy(s.edgePoint, r.edgePoint, 12);
        memcpy(s.cursor, r.cursor, 12);
        for (int i = 0; i < 8; i++) ToRef(world, network, roads, r.refs[i], s.refs[i]);
        for (int i = 0; i < 3; i++) {
            float v;
            memcpy(&v, &s.cursor[i], 4);
            if (!isfinite(v) || fabsf(v) > 1.0e6f) {
                *why = "the cursor is not a point";
                return false;
            }
        }
        // The tool: its place in the build menu.
        U64 record = Q(world + SELECTED_OFFSET), begin = Q(world + MENU_OFFSET), end = Q(world + MENU_OFFSET + 8);
        if (!record || !begin || end < begin || record < begin || record >= end || (record - begin) % MENU_STRIDE) {
            *why = "the selected tool is not a build menu record";
            return false;
        }
        m.record = (int)((record - begin) / MENU_STRIDE);
        memcpy(m.name, (const void*)record, NAME_BYTES);
        m.name[NAME_BYTES - 1] = 0;
        m.hover = *(BYTE*)(world + FLAG_OFFSET) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a fault while reading the hover state";
        return false;
    }
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

static U64 Run(int kind, U64 world, U64 a2)
{
    ToolFn tool = (ToolFn)(g_exeBase + kRoutines[kind]);
    BYTE* w = (BYTE*)world;
    BYTE* press = g_exeBase + PRESS_RVA;
    bool menus = w[kMenus[0]] || w[kMenus[1]] || w[kMenus[2]];
    if (*press && !menus &&
        (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded()))) {
        SaveSync_NotifyWaitingLobbyActionBlocked();
        BYTE kept = *press;
        *press = 0;
        U64 r = tool(world, a2);
        *press = kept;
        return r;
    }
    if (!*press || menus || !Commands_TakeLocal() || !HoverTools_Ready(kind)) {
        // A click in a session that is not taken stays on this PC: worth a line.
        static int s_logged = 0;
        if (*press && !menus && Commands_Capturing() && s_logged < 16) {
            ++s_logged;
            Logf("republicmp  HOVER: a %s click made on this PC only - %s", kKindText[kind],
                 !HoverTools_Ready(kind) ? "the tool is not taken over" : "a command or a tick was being made");
        }
        return tool(world, a2);
    }
    HoverMsg m;
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    // The signal tool's mode is the byte in dl (the rest of rdx is not set); the junction's an int.
    m.mode = kind == RAIL_PLACE ? (int)(a2 & 0xff) : kind == JUNCTION ? (int)(unsigned)a2 : 0;
    const char* why = nullptr;
    bool ok = Capture(kind, world, m, &why);
    // The tool with the click hidden: the highlight, nothing made here.
    BYTE kept = *press;
    *press = 0;
    U64 r = tool(world, a2);
    *press = kept;
    if (ok) {
        float cx = 0.0f, cz = 0.0f;
        memcpy(&cx, &m.pick.cursor[0], 4);
        memcpy(&cz, &m.pick.cursor[2], 4);
        const char* actDesc = HoverKindToastDesc(kind);
        bool allowed = (kind == 1 || kind == RAIL_REMOVE) ? TwoStates_CheckDemolishAllowed(cx, cz, actDesc)
                                                          : TwoStates_CheckBuildAllowed(cx, cz, actDesc);
        if (!allowed) return r;
        Commands_Submit(LS_CMD_HOVER, &m, sizeof(m));
    }
    else Logf("republicmp  HOVER: %s not shared - %s; nothing is done", kKindText[kind], why);
    return r;
}

U64 HoverTools_Waypoint(U64 world, U64 a2) { return Run(0, world, a2); }
U64 HoverTools_SignErase(U64 world, U64 a2) { return Run(1, world, a2); }
U64 HoverTools_Sign(U64 world, U64 a2) { return Run(2, world, a2); }
U64 HoverTools_RailPlace(U64 world, U64 a2) { return Run(3, world, a2); }
U64 HoverTools_RailRemove(U64 world, U64 a2) { return Run(4, world, a2); }
U64 HoverTools_Junction(U64 world, U64 a2) { return Run(5, world, a2); }

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

static int g_failedRef = -1;

// The network's node and edge counts, for the log.
static void Counts(U64 network, unsigned* nodes, unsigned* edges)
{
    *nodes = *edges = 0;
    if (!network) return;
    U64 b = Q(network + NODES), e = Q(network + NODES + 8);
    if (b && e >= b) *nodes = (unsigned)((e - b) / 8);
    b = Q(network + EDGES), e = Q(network + EDGES + 8);
    if (b && e >= b) *edges = (unsigned)((e - b) / 8);
}

static const char* Resolve(U64 world, const HoverMsg& m, RawPick& out)
{
    g_failedRef = -1;
    U64 network = Q(world + (IsRail(m.kind) ? RAIL_NETWORK : ROAD_NETWORK));
    U64 roads = Q(world + ROAD_NETWORK);
    if (!network) return "no network";
    const PickState& s = m.pick;
    out.b658 = s.b658; out.b668 = s.b668; out.b6e8 = s.b6e8; out.b698 = s.b698;
    out.f678 = s.f678; out.f6e0 = s.f6e0; out.f6e4 = s.f6e4; out.f90 = s.f90; out.f6a8 = s.f6a8; out.f688 = s.f688;
    memcpy(out.nodePoint, s.nodePoint, 12);
    memcpy(out.edgePoint, s.edgePoint, 12);
    memcpy(out.cursor, s.cursor, 12);
    for (int i = 0; i < 8; i++) {
        if (s.refs[i].tag > REF_BUILDING) return "unknown reference";
        if (!FromRef(world, network, roads, s.refs[i], &out.refs[i])) {
            g_failedRef = i;
            return "a node, edge or building is not here";
        }
    }
    return nullptr;
}

// The tool with the carried hover state, the click and closed menus (no C++ objects: SEH).
static bool RunReplay(U64 world, const HoverMsg& m, const RawPick& pick, U64 record)
{
    __try {
        WritePick(world, pick);
        *(U64*)(world + SELECTED_OFFSET) = record;
        *(BYTE*)(world + FLAG_OFFSET) = m.hover;
        for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = 0;
        g_exeBase[PRESS_RVA] = 1;
        ((ToolFn)(g_exeBase + kRoutines[m.kind]))(world, (U64)(unsigned)m.mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HoverTools_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(HoverMsg)) return false;
    HoverMsg m;
    memcpy(&m, data, sizeof(m));
    m.name[NAME_BYTES - 1] = 0;
    if (m.kind < 0 || m.kind >= KINDS || !HoverTools_Ready(m.kind)) return false;
    if (m.kind == JUNCTION ? (m.mode < 0 || m.mode > 2) : m.kind == RAIL_PLACE ? (m.mode < 0 || m.mode > 1) : m.mode != 0) return false;
    U64 world = (U64)World();
    const char* why = nullptr;
    U64 record = 0;
    RawPick pick;
    __try {
        U64 begin = Q(world + MENU_OFFSET), end = Q(world + MENU_OFFSET + 8);
        if (m.record >= 0 && begin && end >= begin && (U64)m.record < (end - begin) / MENU_STRIDE) {
            record = begin + (U64)m.record * MENU_STRIDE;
            if (strncmp((const char*)record, m.name, NAME_BYTES - 1) != 0) record = 0;
        }
        if (!record) why = "the tool is not in this PC's build menu";
        else why = Resolve(world, m, pick);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while resolving";
    }
    unsigned nodes = 0, edges = 0, nodesAfter = 0, edgesAfter = 0;
    U64 network = 0;
    __try {
        network = Q(world + (IsRail(m.kind) ? RAIL_NETWORK : ROAD_NETWORK));
        Counts(network, &nodes, &edges);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        network = 0;
    }
    if (why) {
        if (g_failedRef >= 0)
            Logf("republicmp  HOVER: %s (%s): %s - not made (reference %d at +0x%X: kind %u, index %u; this PC has %u nodes, %u edges)",
                 kKindText[m.kind], m.name, why, g_failedRef, kRefOffsets[g_failedRef], m.pick.refs[g_failedRef].tag,
                 m.pick.refs[g_failedRef].index, nodes, edges);
        else
            Logf("republicmp  HOVER: %s (%s): %s - not made", kKindText[m.kind], m.name, why);
        return false;
    }
    // This PC's own hover state, tool, flag, click and menus, put back after; the edges
    // near the cursor are the same on both PCs while the tool runs.
    RawPick own;
    float cx = 0, cz = 0;
    memcpy(&cx, &m.pick.cursor[0], 4);
    memcpy(&cz, &m.pick.cursor[2], 4);
    BYTE* w = (BYTE*)world;
    ReadPick(world, own);
    U64 ownTool = Q(world + SELECTED_OFFSET);
    BYTE ownFlag = w[FLAG_OFFSET], ownPress = g_exeBase[PRESS_RVA], menus[3];
    for (int i = 0; i < 3; i++) menus[i] = w[kMenus[i]];
    bool scoped = ViewListScope_Begin(cx - SCOPE_RADIUS, cz - SCOPE_RADIUS, cx + SCOPE_RADIUS, cz + SCOPE_RADIUS);
    bool ok = RunReplay(world, m, pick, record);
    // Put back whether the tool finished or faulted.
    g_exeBase[PRESS_RVA] = ownPress;
    for (int i = 0; i < 3; i++) w[kMenus[i]] = menus[i];
    w[FLAG_OFFSET] = ownFlag;
    *(U64*)(world + SELECTED_OFFSET) = ownTool;
    WritePick(world, own);
    if (scoped) ViewListScope_End();
    __try {
        Counts(network, &nodesAfter, &edgesAfter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Logf("republicmp  HOVER: %s (%s): %s - edge %d node %d, nodes %u -> %u, edges %u -> %u", kKindText[m.kind], m.name,
         ok ? "made" : "the tool faulted", m.pick.refs[0].tag ? (int)m.pick.refs[0].index : -1,
         m.pick.refs[1].tag ? (int)m.pick.refs[1].index : -1, nodes, nodesAfter, edges, edgesAfter);
    return ok;
}
