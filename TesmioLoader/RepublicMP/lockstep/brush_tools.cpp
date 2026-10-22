// SPDX-License-Identifier: GPL-3.0-only
// See brush_tools.h. Follows SoylentGreed's terrain tool route (RepublicLAN,
// read against the disassembly of 1.1.1.9).
#include "brush_tools.h"
#include "commands.h"
#include "../session/two_states.h"
#include <limits.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define TIMER_RVA 0x9D4EE0      // the game timer the tools' PowerTime reads (16 bytes)
#define DOWN_RVA 0xA54E90
#define PRESS_RVA 0xA54E91
#define INPUT_RVA 0xA54B90      // C3D_INPUT
#define SELECTED_OFFSET 0xD428
#define MENU_OFFSET 0xD280      // the build menu's records: begin, end
#define MENU_STRIDE 0x2D0       // name at +0
#define NAME_BYTES 32
static const unsigned kMenus[3] = { 0xFBC, 0xFBE, 0xFC3 };

// World words of a sample and of the tool's state.
#define CURSOR_OFFSET 0xF6C
#define BRUSH_OFFSET 0xF98
#define HELD_OFFSET 0xD2BC
#define COST_OFFSET 0xD370
#define SETTING374_OFFSET 0xD374
#define SETTING338_OFFSET 0xD338
#define TARGET_OFFSET 0xD458    // the level height
#define TIMERS_OFFSET 0xD45C    // ten sound timers
#define TREE_OFFSET 0x11758     // the tree tool's accumulator

#define KINDS 4
#define TREE_KIND 3
static const unsigned kRoutines[KINDS] = { 0x3A8A70, 0x3ADF80, 0x3CCD70, 0x2F94C0 };
static const unsigned char kEntries[KINDS][16] = {
    { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xd8 },
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x80, 0xb9, 0xbc, 0x0f, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 },
    { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0x58 },
    { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8d, 0xa8, 0xd8, 0xfe, 0xff, 0xff },
};
static const char* const kKindText[KINDS] = { "terrain", "tree demolition", "topsoil", "tree" };

// 3a8a70's reads of C3D_INPUT (call [import], 6 bytes): 0 GetMouseLeftPressAOff, 1 GetMouseLeftPress.
struct InputSite {
    unsigned site, getter;
};
static const InputSite kInputSites[3] = { { 0x3AA850, 0 }, { 0x3AAA58, 1 }, { 0x3AAAA9, 1 } };
static const unsigned kInputSlots[2] = { 0x86C8B0, 0x86C8B8 };

// 3a8a70's, 2bade0's and 4fcaf0's construction ledger rows (call 2fdb00, 5 bytes): the stroke's
// cost in dollars (world +11330) and in rubles (+11360), written when it pays.
#define LEDGER_RVA 0x2FDB00
#define RESOURCE_BY_NAME_RVA 0x2AA830 // (world, name) -> resource definition
static const unsigned kLedgerCalls[6] = { 0x3AABCD, 0x3AACE3, 0x2CB854, 0x2CB942, 0x517244, 0x517341 };

#pragma pack(push, 1)
struct BrushState {
    float held, cost, timers[10], tree;
};
struct BrushSample {
    float cursor[3], brush, target, setting374;
    int setting338;
    unsigned char timer[16];
    unsigned char press, pressOff, pad[2];
    BrushState state;
};
struct BrushMsg {
    unsigned origin;          // the clicking PC (its own press prompt)
    unsigned serial, sequence; // the stroke and this sample's place in it
    int kind, mode;
    int record;               // the tool: its place in the build menu
    char name[NAME_BYTES];    // and its name, a check
    BrushSample sample;
};
// A construction ledger row the stroke wrote on its own PC (see RelayLedger).
struct LedgerMsg {
    unsigned ledger;          // world offset: 0x11330 dollars, 0x11360 rubles
    char resource[64];        // the row's resource definition, by name
    float value8;             // the row's floats
    unsigned valueC;
    float a4, a6;             // 2fdb00's other arguments
    unsigned a5, a7;
};
#pragma pack(pop)

typedef U64 (*ToolFn)(U64 world, U64 a2);
typedef U64 (*LedgerFn)(U64 world, U64 row, U64 ledger, float a4, U64 a5, float a6, U64 a7);
typedef U64 (*ResourceByNameFn)(U64 world, const char* name);
typedef bool (*GetterFn)(void* input);

static int g_ready[KINDS] = { -1, -1, -1, -1 };
static bool g_inputsRelayed = false;
static GetterFn g_getters[2];
static unsigned g_origin = 0;

void BrushTools_Bind(const TsmHost* host)
{
    TsmBind(host);
    g_origin = (unsigned)(GetTickCount64() * 2246822519u) ^ (GetCurrentProcessId() << 8) ^ 0x42525348u;
    if (!g_origin) g_origin = 1;
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

bool BrushTools_Ready(int kind)
{
    if (kind < 0 || kind >= KINDS) return false;
    if (g_ready[kind] < 0) {
        g_ready[kind] = memcmp(g_exeBase + kRoutines[kind], kEntries[kind], 16) == 0 ? 1 : 0;
        if (kind == 0 && !g_inputsRelayed) g_ready[kind] = 0;
        if (!g_ready[kind]) Logf("republicmp  BRUSH: the %s tool's routine differs - its edits stay as before", kKindText[kind]);
    }
    return g_ready[kind] == 1;
}

// ---------------------------------------------------------------------------
// The press answers: forced on a replay, this PC's own otherwise
// ---------------------------------------------------------------------------

static DWORD g_forcedThread = 0;
static bool g_forced[2];

static bool Answer(int getter, void* input)
{
    if (g_forcedThread && g_forcedThread == GetCurrentThreadId()) return g_forced[getter];
    return g_getters[getter] ? g_getters[getter](input) : false;
}

static bool RelayPressOff(void* input) { return Answer(0, input); }
static bool RelayPress(void* input) { return Answer(1, input); }

// A stroke pays its cost on this PC's own run of the tool, the one with the
// button hidden (to the tool the button was let go): the money it takes becomes
// a command (lockstep/commands), and the row it writes into the construction
// ledger - which the construction sites' payments read - stayed on this PC
// alone: every site paid a little more there, the money split. The row is a
// command too, written on every PC; the replays (button down) write none.
static U64 RelayLedger(U64 world, U64 row, U64 ledger, float a4, U64 a5, float a6, U64 a7)
{
    LedgerFn original = (LedgerFn)(g_exeBase + LEDGER_RVA);
    if (!Commands_TakeLocal()) return original(world, row, ledger, a4, a5, a6, a7);
    LedgerMsg m = {};
    __try {
        m.ledger = (unsigned)(ledger - world);
        U64 resource = *(U64*)row;
        if ((m.ledger != 0x11330 && m.ledger != 0x11360) || !resource) return original(world, row, ledger, a4, a5, a6, a7);
        strncpy_s(m.resource, (const char*)resource, _TRUNCATE);
        m.value8 = *(float*)(row + 8);
        m.valueC = *(unsigned*)(row + 12);
        // The caller (3A8A70, 2BADE0, or 4FCAF0) subtracted `a4` from world+0x580 (USD) or world+0x588 (RUB)
        // immediately before calling 0x2FDB00. Refund it locally so the deduction only happens deterministically
        // inside BrushTools_ApplyLedger on both PCs (under TwoStates_BeginCommand/EndCommand).
        if (*(int*)(world + 0x578) == 0) {
            if (m.ledger == 0x11330) *(double*)(world + 0x580) += (double)a4;
            else if (m.ledger == 0x11360) *(double*)(world + 0x588) += (double)a4;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return original(world, row, ledger, a4, a5, a6, a7);
    }
    m.a4 = a4;
    m.a5 = (unsigned)(unsigned char)a5;
    m.a6 = a6;
    m.a7 = (unsigned)(unsigned char)a7;
    Commands_Submit(LS_CMD_LEDGER, &m, sizeof(m));
    return 0;
}

bool BrushTools_ApplyLedger(const void* data, size_t size)
{
    if (size != sizeof(LedgerMsg)) return false;
    LedgerMsg m;
    memcpy(&m, data, sizeof(m));
    m.resource[sizeof(m.resource) - 1] = 0;
    if (m.ledger != 0x11330 && m.ledger != 0x11360) return false;
    U64 world = (U64)World();
    __try {
        U64 resource = ((ResourceByNameFn)(g_exeBase + RESOURCE_BY_NAME_RVA))(world, m.resource);
        if (!resource) {
            Logf("republicmp  BRUSH: no resource [%s] here - the ledger row is not written", m.resource);
            return false;
        }
        if (*(int*)(world + 0x578) == 0) {
            if (m.ledger == 0x11330) *(double*)(world + 0x580) -= (double)m.a4;
            else if (m.ledger == 0x11360) *(double*)(world + 0x588) -= (double)m.a4;
        }
        unsigned char row[16];
        memcpy(row, &resource, 8);
        memcpy(row + 8, &m.value8, 4);
        memcpy(row + 12, &m.valueC, 4);
        ((LedgerFn)(g_exeBase + LEDGER_RVA))(world, (U64)row, world + m.ledger, m.a4, m.a5, m.a6, m.a7);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  BRUSH: writing a ledger row faulted");
        return false;
    }
    return true;
}

bool BrushTools_Install()
{
    for (const InputSite& s : kInputSites) {
        BYTE* at = g_exeBase + s.site;
        int rel = 0;
        memcpy(&rel, at + 2, 4);
        if (at[0] != 0xFF || at[1] != 0x15 || s.site + 6 + rel != kInputSlots[s.getter]) {
            Logf("republicmp  BRUSH: the terrain tool's press read at 0x%X differs - terrain brushes stay as before", s.site);
            return false;
        }
    }
    for (unsigned site : kLedgerCalls) {
        BYTE* at = g_exeBase + site;
        int rel = 0;
        memcpy(&rel, at + 1, 4);
        if (at[0] != 0xE8 || site + 5 + rel != LEDGER_RVA) {
            Logf("republicmp  BRUSH: the terrain tool's ledger call at 0x%X differs - terrain brushes stay as before", site);
            return false;
        }
    }
    for (int g = 0; g < 2; g++) {
        g_getters[g] = *(GetterFn*)(g_exeBase + kInputSlots[g]);
        if (!g_getters[g]) return false;
    }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    void* targets[3] = { (void*)&RelayPressOff, (void*)&RelayPress, (void*)&RelayLedger };
    DWORD old;
    VirtualProtect(relay, 48, PAGE_EXECUTE_READWRITE, &old);
    for (int g = 0; g < 3; g++) {
        BYTE* r = relay + g * 16;
        r[0] = 0xFF; r[1] = 0x25; r[2] = r[3] = r[4] = r[5] = 0;
        memcpy(r + 6, &targets[g], 8);
    }
    VirtualProtect(relay, 48, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 48);
    for (unsigned site : kLedgerCalls) {
        BYTE* at = g_exeBase + site;
        long long to = (long long)(relay + 2 * 16) - (long long)(at + 5);
        if (to < INT_MIN || to > INT_MAX) return false;
        unsigned char code[5] = { 0xE8, 0, 0, 0, 0 };
        int rel32 = (int)to;
        memcpy(code + 1, &rel32, 4);
        DWORD prot;
        VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &prot);
        memcpy(at, code, 5);
        VirtualProtect(at, 5, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
    }
    for (const InputSite& s : kInputSites) {
        BYTE* at = g_exeBase + s.site;
        long long to = (long long)(relay + s.getter * 16) - (long long)(at + 5);
        if (to < INT_MIN || to > INT_MAX) return false;
        unsigned char code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        int rel32 = (int)to;
        memcpy(code + 1, &rel32, 4);
        DWORD prot;
        VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &prot);
        memcpy(at, code, 6);
        VirtualProtect(at, 6, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), at, 6);
    }
    g_inputsRelayed = true;
    return true;
}

// ---------------------------------------------------------------------------
// Samples
// ---------------------------------------------------------------------------

static void ReadState(BYTE* world, BrushState& s)
{
    s.held = *(float*)(world + HELD_OFFSET);
    s.cost = *(float*)(world + COST_OFFSET);
    memcpy(s.timers, world + TIMERS_OFFSET, sizeof(s.timers));
    s.tree = *(float*)(world + TREE_OFFSET);
}

static void WriteState(BYTE* world, const BrushState& s)
{
    *(float*)(world + HELD_OFFSET) = s.held;
    *(float*)(world + COST_OFFSET) = s.cost;
    memcpy(world + TIMERS_OFFSET, s.timers, sizeof(s.timers));
    *(float*)(world + TREE_OFFSET) = s.tree;
}

static void ReadSample(BYTE* world, BrushSample& s)
{
    memcpy(s.cursor, world + CURSOR_OFFSET, 12);
    s.brush = *(float*)(world + BRUSH_OFFSET);
    s.target = *(float*)(world + TARGET_OFFSET);
    s.setting374 = *(float*)(world + SETTING374_OFFSET);
    s.setting338 = *(int*)(world + SETTING338_OFFSET);
    memcpy(s.timer, g_exeBase + TIMER_RVA, 16);
    ReadState(world, s.state);
}

static void WriteSample(BYTE* world, const BrushSample& s, const BrushState& state)
{
    memcpy(world + CURSOR_OFFSET, s.cursor, 12);
    *(float*)(world + BRUSH_OFFSET) = s.brush;
    *(float*)(world + TARGET_OFFSET) = s.target;
    *(float*)(world + SETTING374_OFFSET) = s.setting374;
    *(int*)(world + SETTING338_OFFSET) = s.setting338;
    memcpy(g_exeBase + TIMER_RVA, s.timer, 16);
    WriteState(world, state);
}

static int RecordPlace(U64 record, char name[NAME_BYTES])
{
    BYTE* world = World();
    U64 begin = *(U64*)(world + MENU_OFFSET), end = *(U64*)(world + MENU_OFFSET + 8);
    if (!record || !begin || end < begin || record < begin || record >= end || (record - begin) % MENU_STRIDE) return -1;
    memcpy(name, (const void*)record, NAME_BYTES);
    name[NAME_BYTES - 1] = 0;
    return (int)((record - begin) / MENU_STRIDE);
}

static U64 RecordAt(int place, const char* name)
{
    BYTE* world = World();
    U64 begin = *(U64*)(world + MENU_OFFSET), end = *(U64*)(world + MENU_OFFSET + 8);
    if (place < 0 || !begin || end < begin || (U64)place >= (end - begin) / MENU_STRIDE) return 0;
    U64 record = begin + (U64)place * MENU_STRIDE;
    return strncmp((const char*)record, name, NAME_BYTES - 1) == 0 ? record : 0;
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

struct Stroke {
    bool active;
    unsigned serial, sequence;
    int kind, mode, record;
    float target;
};
static Stroke g_stroke = {};
static unsigned g_serials = 0;

static bool Sample(int kind, U64 world, int mode, BrushMsg& m, BYTE down, BYTE press)
{
    BYTE* w = (BYTE*)world;
    memset(&m, 0, sizeof(m));
    __try {
        m.record = RecordPlace(*(U64*)(w + SELECTED_OFFSET), m.name);
        if (m.record < 0) return false;
        ReadSample(w, m.sample);
        void* input = g_exeBase + INPUT_RVA;
        if (kind == 0) {
            m.sample.pressOff = g_getters[0] && g_getters[0](input) ? 1 : 0;
            m.sample.press = g_getters[1] && g_getters[1](input) ? 1 : 0;
        } else if (kind == TREE_KIND) {
            m.sample.press = press ? 1 : 0; // the tree tool reads the byte itself
        }
        (void)down;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static U64 Run(int kind, U64 world, U64 a2)
{
    ToolFn tool = (ToolFn)(g_exeBase + kRoutines[kind]);
    BYTE* w = (BYTE*)world;
    BYTE* down = g_exeBase + DOWN_RVA;
    BYTE* press = g_exeBase + PRESS_RVA;
    bool menus = w[kMenus[0]] || w[kMenus[1]] || w[kMenus[2]];
    bool acting = *down || (kind == TREE_KIND && *press);
    if (!acting || menus || !Commands_TakeLocal() || !BrushTools_Ready(kind)) {
        if (!acting) g_stroke.active = false; // the stroke ends with the button
        return tool(world, a2);
    }
    int mode = kind == 0 ? (int)(unsigned)a2 : 0;
    BrushMsg m;
    bool ok = Sample(kind, world, mode, m, *down, *press);
    // This PC: the tool with the button hidden (the brush preview; nothing is edited).
    BYTE keptDown = *down, keptPress = *press;
    *down = 0;
    *press = 0;
    U64 r = tool(world, a2);
    *down = keptDown;
    *press = keptPress;
    if (!ok) {
        Logf("republicmp  BRUSH: the selected tool is not a build menu record - nothing is done");
        return r;
    }
    if (!TwoStates_CheckBuildAllowed(m.sample.cursor[0], m.sample.cursor[2], "изменение рельефа/деревьев")) {
        g_stroke.active = false;
        return r;
    }
    // Another tool or mode: a new stroke.
    if (g_stroke.active && (g_stroke.kind != kind || g_stroke.mode != mode || g_stroke.record != m.record)) g_stroke.active = false;
    bool first = !g_stroke.active;
    // The level height: taken at the stroke's press (the hidden run computed it from the cursor), then kept.
    float now = *(float*)(w + TARGET_OFFSET);
    if (first || keptPress) g_stroke.target = now;
    *(float*)(w + TARGET_OFFSET) = g_stroke.target;
    m.sample.target = g_stroke.target;
    if (first) {
        g_stroke.active = true;
        g_stroke.serial = ++g_serials;
        g_stroke.sequence = 0;
        g_stroke.kind = kind;
        g_stroke.mode = mode;
        g_stroke.record = m.record;
    }
    m.origin = g_origin;
    m.serial = g_stroke.serial;
    m.sequence = g_stroke.sequence++;
    m.kind = kind;
    m.mode = mode;
    Commands_Submit(LS_CMD_BRUSH, &m, sizeof(m));
    return r;
}

U64 BrushTools_Terrain(U64 world, U64 mode) { return Run(0, world, mode); }
U64 BrushTools_DemolishTrees(U64 world, U64 a2) { return Run(1, world, a2); }
U64 BrushTools_Topsoil(U64 world, U64 a2) { return Run(2, world, a2); }
U64 BrushTools_Tree(U64 world, U64 a2) { return Run(3, world, a2); }

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// Per clicking PC: the stroke the replays are on and the state they left.
struct StrokeIn {
    unsigned origin, serial, next;
    BrushState state;
};
static StrokeIn g_in[4];

static StrokeIn& InFor(unsigned origin)
{
    for (StrokeIn& s : g_in)
        if (s.origin == origin) return s;
    for (StrokeIn& s : g_in)
        if (!s.origin) {
            s.origin = origin;
            return s;
        }
    g_in[0] = StrokeIn{ origin, 0, 0, {} };
    return g_in[0];
}

static bool Replay(const BrushMsg& m, U64 record, BrushState& state)
{
    BYTE* w = World();
    BrushSample own;
    ReadSample(w, own);
    U64 ownTool = *(U64*)(w + SELECTED_OFFSET);
    BYTE ownDown = g_exeBase[DOWN_RVA], ownPress = g_exeBase[PRESS_RVA];
    bool ok = true;
    __try {
        *(U64*)(w + SELECTED_OFFSET) = record;
        WriteSample(w, m.sample, state);
        g_exeBase[DOWN_RVA] = 1;
        g_exeBase[PRESS_RVA] = m.kind == TREE_KIND ? m.sample.press : 0;
        // The press prompt (GetMouseLeftPressAOff) only on the stroke's own PC; it edits nothing.
        g_forced[0] = m.origin == g_origin && m.sample.pressOff;
        g_forced[1] = m.sample.press != 0;
        g_forcedThread = GetCurrentThreadId();
        ((ToolFn)(g_exeBase + kRoutines[m.kind]))((U64)w, (U64)(unsigned)m.mode);
        g_forcedThread = 0;
        ReadState(w, state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_forcedThread = 0;
        ok = false;
    }
    g_exeBase[DOWN_RVA] = ownDown;
    g_exeBase[PRESS_RVA] = ownPress;
    *(U64*)(w + SELECTED_OFFSET) = ownTool;
    WriteSample(w, own, own.state);
    return ok;
}

bool BrushTools_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(BrushMsg)) return false;
    BrushMsg m;
    memcpy(&m, data, sizeof(m));
    m.name[NAME_BYTES - 1] = 0;
    if (m.kind < 0 || m.kind >= KINDS || !BrushTools_Ready(m.kind)) return false;
    U64 record = 0;
    __try {
        record = RecordAt(m.record, m.name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        record = 0;
    }
    if (!record) {
        Logf("republicmp  BRUSH: %s is not in this PC's build menu - not made", m.name);
        return false;
    }
    // The tool's state: carried at the stroke's first sample, then as the replays left it.
    StrokeIn& in = InFor(m.origin);
    bool chained = m.sequence > 0 && in.serial == m.serial && in.next == m.sequence;
    BrushState state = chained ? in.state : m.sample.state;
    bool ok = Replay(m, record, state);
    in.serial = m.serial;
    in.next = m.sequence + 1;
    in.state = state;
    if (!ok) Logf("republicmp  BRUSH: %s (%s) faulted", kKindText[m.kind], m.name);
    return ok;
}
