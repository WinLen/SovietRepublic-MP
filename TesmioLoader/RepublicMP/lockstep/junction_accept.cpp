// SPDX-License-Identifier: GPL-3.0-only
// See junction_accept.h. The copy's layout and the block's calls follow
// SoylentGreed's junction accept route (RepublicLAN, read against the
// disassembly of 1.1.1.9).
#include "junction_accept.h"
#include "commands.h"
#include <math.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define ROAD_NETWORK 0xF7A0
#define NETWORK_NODES 0x298
#define JUNCTION_OF_NODE 0xC0   // a road node's junction
#define LIST_FIELD 0x12808      // [world+12808]+38: the traffic-light junctions
#define LIST_OFFSET 0x38
#define OBJECT_BYTES 0x148
#define NODES_OFFSET 0x88
#define ARMS_OFFSET 0xA0
#define ARM_BYTES 0x1D8
#define PHASES_OFFSET 0xB8
#define PHASE_BYTES 12
#define MAX_NODES 16
#define MAX_ARMS 24
#define MAX_PHASES 32
#define MAX_KIND 2

#define CACHE_BEGIN 0xC6B0    // after the block the window empties this world vector (7595f9..)
#define CACHE_END 0xC6B8
#define RESET_RVA 0x224300
#define COPY_RVA 0x226530
#define MARK_RVA 0x22A6D0
#define UNMARK_RVA 0x0F98D0

#pragma pack(push, 1)
struct NodeRef {
    unsigned index, check;
};
struct ArmRef {
    unsigned index;
    unsigned char slot, flag;
    unsigned short pad;
    unsigned mode;
};
struct JunctionMsg {
    unsigned kind;
    unsigned char nodes, arms, phases, pad;
    NodeRef anchor; // the junction's first node
    NodeRef node[MAX_NODES];
    ArmRef arm[MAX_ARMS];
    unsigned char phase[MAX_PHASES * PHASE_BYTES];
};
#pragma pack(pop)

void JunctionAccept_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

static unsigned NodeCheck(U64 node)
{
    unsigned w[3] = { 0x4e4f4445u, *(unsigned*)(node + 4), *(unsigned*)(node + 0xc) };
    unsigned h = 2166136261u;
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 4; b++) {
            h ^= (w[i] >> (8 * b)) & 0xffu;
            h *= 16777619u;
        }
    return h | 1u;
}

static unsigned IndexIn(U64 vector, U64 p)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    if (!p || !begin || end < begin || (end - begin) / 8 > 1000000) return 0xFFFFFFFFu;
    for (U64 at = begin; at < end; at += 8)
        if (Q(at) == p) return (unsigned)((at - begin) / 8);
    return 0xFFFFFFFFu;
}

static U64 AtIndex(U64 vector, unsigned index)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    if (!begin || end < begin || index >= (end - begin) / 8) return 0;
    return Q(begin + (U64)index * 8);
}

// A vector's element count (elem bytes each), or -1 when it is not one or too long.
static int Count(U64 at, unsigned elem, int max)
{
    U64 b = Q(at), e = Q(at + 8);
    if (!b && !e) return 0;
    if (!b || e < b || (e - b) % elem || (e - b) / elem > (U64)max) return -1;
    return (int)((e - b) / elem);
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

static const char* Encode(U64 junction, U64 copy, JunctionMsg& m)
{
    U64 world = (U64)World();
    U64 roads = Q(world + ROAD_NETWORK);
    if (!roads) return "no road network";
    memset(&m, 0, sizeof(m));
    m.kind = *(unsigned*)copy;
    if (m.kind > MAX_KIND) return "an unknown junction kind";
    // The anchor: the junction's own first node.
    int live = Count(junction + NODES_OFFSET, 8, MAX_NODES);
    if (live <= 0) return "the junction has no nodes";
    U64 first = Q(Q(junction + NODES_OFFSET));
    if (Q(first + JUNCTION_OF_NODE) != junction) return "the junction's first node is not its own";
    unsigned index = IndexIn(roads + NETWORK_NODES, first);
    if (index == 0xFFFFFFFFu) return "the junction's node is not in the road network";
    m.anchor = { index, NodeCheck(first) };
    // The copy's nodes (crossroad_add may have changed them).
    int nodes = Count(copy + NODES_OFFSET, 8, MAX_NODES);
    if (nodes <= 0) return "the copy has no nodes";
    U64 list = Q(copy + NODES_OFFSET);
    for (int i = 0; i < nodes; i++) {
        U64 node = Q(list + (U64)i * 8);
        unsigned at = IndexIn(roads + NETWORK_NODES, node);
        if (at == 0xFFFFFFFFu) return "a node of the copy is not in the road network";
        m.node[i] = { at, NodeCheck(node) };
    }
    m.nodes = (unsigned char)nodes;
    int arms = Count(copy + ARMS_OFFSET, ARM_BYTES, MAX_ARMS);
    if (arms < 0) return "too many arms";
    U64 armList = Q(copy + ARMS_OFFSET);
    for (int i = 0; i < arms; i++) {
        U64 e = armList + (U64)i * ARM_BYTES;
        U64 node = Q(e + 8);
        int slot = -1;
        for (int k = 0; k < nodes; k++)
            if (Q(list + (U64)k * 8) == node) slot = k;
        if (slot < 0) return "an arm's node is not in the copy";
        m.arm[i].index = *(unsigned*)e;
        m.arm[i].slot = (unsigned char)slot;
        m.arm[i].flag = *(BYTE*)(e + 0x10);
        m.arm[i].mode = *(unsigned*)(e + 0x14);
    }
    m.arms = (unsigned char)arms;
    int phases = Count(copy + PHASES_OFFSET, PHASE_BYTES, MAX_PHASES);
    if (phases < 0) return "too many light phases";
    if (phases) memcpy(m.phase, (const void*)Q(copy + PHASES_OFFSET), (size_t)phases * PHASE_BYTES);
    for (int i = 0; i < phases; i++) {
        float s;
        memcpy(&s, m.phase + i * PHASE_BYTES + 8, 4);
        if (!isfinite(s) || fabsf(s) > 1.0e7f) return "a phase's duration is not a number";
    }
    m.phases = (unsigned char)phases;
    return nullptr;
}

void JunctionAccept_Capture(U64 junction, U64 copy)
{
    JunctionMsg m;
    const char* why;
    __try {
        why = junction && copy ? Encode(junction, copy, m) : "no junction";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the copy";
    }
    if (why) {
        Logf("republicmp  JUNCTION: accept not shared - %s; nothing is done", why);
        return;
    }
    Commands_Submit(LS_CMD_JUNCTION, &m, sizeof(m));
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// The stand-in copy in mod memory (226530 only reads it): the kind, the
// nodes, each arm's +0/+8/+10/+14 and the phases; the rest stays zero.
static BYTE g_object[OBJECT_BYTES];
static U64 g_nodes[MAX_NODES];
static BYTE g_arms[MAX_ARMS * ARM_BYTES];
static BYTE g_phases[MAX_PHASES * PHASE_BYTES];

static void SetVec(BYTE* at, const void* begin, size_t bytes)
{
    U64 v[3] = {};
    if (bytes && begin) {
        v[0] = (U64)begin;
        v[1] = v[0] + bytes;
        v[2] = v[1];
    }
    memcpy(at, v, 24);
}

static const char* Apply(const JunctionMsg& m, unsigned* kindAfter)
{
    U64 world = (U64)World();
    U64 roads = Q(world + ROAD_NETWORK);
    if (!roads) return "no road network";
    if (m.kind > MAX_KIND || !m.nodes || m.nodes > MAX_NODES || m.arms > MAX_ARMS || m.phases > MAX_PHASES) return "a bad shape";
    // The junction: the anchor's, which must list the anchor.
    U64 anchor = AtIndex(roads + NETWORK_NODES, m.anchor.index);
    if (!anchor || NodeCheck(anchor) != m.anchor.check) return "the junction's node is not here";
    U64 junction = Q(anchor + JUNCTION_OF_NODE);
    int live = junction ? Count(junction + NODES_OFFSET, 8, MAX_NODES) : -1;
    bool listed = false;
    for (int i = 0; i < live; i++) listed |= Q(Q(junction + NODES_OFFSET) + (U64)i * 8) == anchor;
    if (!listed) return "the junction is not here";
    memset(g_object, 0, sizeof(g_object));
    memset(g_arms, 0, sizeof(g_arms));
    memcpy(g_object, &m.kind, 4);
    for (int i = 0; i < m.nodes; i++) {
        U64 node = AtIndex(roads + NETWORK_NODES, m.node[i].index);
        if (!node || NodeCheck(node) != m.node[i].check) return "a node of the copy is not here";
        g_nodes[i] = node;
    }
    for (int i = 0; i < m.arms; i++) {
        if (m.arm[i].slot >= m.nodes) return "a bad arm";
        BYTE* e = g_arms + i * ARM_BYTES;
        memcpy(e, &m.arm[i].index, 4);
        memcpy(e + 8, &g_nodes[m.arm[i].slot], 8);
        e[0x10] = m.arm[i].flag;
        memcpy(e + 0x14, &m.arm[i].mode, 4);
    }
    memcpy(g_phases, m.phase, (size_t)m.phases * PHASE_BYTES);
    SetVec(g_object + NODES_OFFSET, g_nodes, (size_t)m.nodes * 8);
    SetVec(g_object + ARMS_OFFSET, g_arms, (size_t)m.arms * ARM_BYTES);
    SetVec(g_object + PHASES_OFFSET, g_phases, (size_t)m.phases * PHASE_BYTES);
    U64 holder = Q(world + LIST_FIELD);
    if (!holder) return "no traffic light list";
    typedef U64 (*Call3)(U64, U64, U64);
    ((Call3)(g_exeBase + RESET_RVA))(junction, 0, 0);
    ((Call3)(g_exeBase + COPY_RVA))(junction, (U64)g_object, 0);
    *kindAfter = *(unsigned*)junction;
    ((Call3)(g_exeBase + (*kindAfter == 2 ? MARK_RVA : UNMARK_RVA)))(junction, holder + LIST_OFFSET, 0);
    // As the window does right after the block (the line window's Accept does the same).
    *(U64*)(world + CACHE_END) = *(U64*)(world + CACHE_BEGIN);
    Logf("republicmp  JUNCTION:   the junction now: kind %u, %d node(s), %d arm(s), %d phase(s), lights flag %u",
         *(unsigned*)junction, Count(junction + NODES_OFFSET, 8, 64), Count(junction + ARMS_OFFSET, ARM_BYTES, 64),
         Count(junction + PHASES_OFFSET, PHASE_BYTES, 256), (unsigned)*(BYTE*)(junction + 0xD1));
    return nullptr;
}

bool JunctionAccept_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(JunctionMsg)) return false;
    JunctionMsg m;
    memcpy(&m, data, sizeof(m));
    const char* why;
    unsigned kind = 0;
    __try {
        why = Apply(m, &kind);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the block faulted";
    }
    static const char* const kKinds[3] = { "right-hand rule", "main road", "traffic lights" };
    Logf("republicmp  JUNCTION: accept, %s, %u node(s), %u arm(s), %u phase(s): %s", m.kind <= MAX_KIND ? kKinds[m.kind] : "?",
         m.nodes, m.arms, m.phases, why ? why : "done");
    return why == nullptr;
}
