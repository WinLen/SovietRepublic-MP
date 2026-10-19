// SPDX-License-Identifier: GPL-3.0-only
// See route_tools.h. The capture and the replay follow SoylentGreed's stop
// and copy line tool routes (RepublicLAN, read against the disassembly of
// 1.1.1.9).
#include "route_tools.h"
#include "commands.h"
#include "world_refs.h"
#include "../session/two_states.h"
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define TARGET_OFFSET 0xD298      // the tool's object: a vehicle or a line
#define WINDOW_OFFSET 0xD2A0      // the tool's window
#define SELECTED_OFFSET 0xD428    // the selected tool
#define MENU_OFFSETS { 0xFBC, 0xFBE, 0xFC3 }
#define WINDOW_INDEX 0x264        // the window's stop index
#define WINDOW_KIND 0x248         // 1: a vehicle window
#define WINDOW_VEHICLE 0x240
#define LINE_WINDOWS_RVA 0x9E19D0 // the line windows (a vector of window pointers)
#define LINE_WINDOW_COPY 0x270    // a line window's working copy
#define LINES_OFFSET 0x12800      // the lines manager: +0 and +18 line vectors
#define PRESS_RVA 0xA54E91        // the left press, as the tools read it
#define CTRL_RVA 0xA54BAD         // the left Ctrl key
#define STAND_IN_BYTES 0x400

static const unsigned kStopTools[2] = { 0x2B2760, 0x2B4A50 };
#define LINE_COPY_TOOL 0x2B4C60
#define LINE_MARK 0x40CDE0
#define LINE_TEST 0x498990
#define LINE_HOME_TEST 0x1AA540
#define LINE_PUT 0x497B10
#define LINE_START 0x3BB330
#define DEPOT_TOOL 0x2B2DD0
#define DEPOT_MARK 0x40BD60
#define DEPOT_MARK_RETURN 0x2B3795 // after the mark's call at 2b3790
#define DEPOT_LIST_BEGIN (0x100 - 0x58) // the tool's vehicle list, from the mark caller's rsp
#define DEPOT_LIST_END (0x100 - 0x50)
#define DEPOT_MAX 64

struct RoutineEntry {
    unsigned rva;
    unsigned char entry[16];
    size_t size; // 0: all 16 bytes
};
static const RoutineEntry kRoutines[] = {
    { 0x2B5120, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89 } },
    { 0x2B2760, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x99, 0x98, 0xd2, 0x00, 0x00, 0x48 } },
    { 0x2B4A50, { 0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0xb9, 0x98, 0xd2, 0x00, 0x00, 0x48, 0x8b } },
    { 0x2B4C60, { 0x40, 0x55, 0x56, 0x41, 0x54, 0x48, 0x8d, 0x6c, 0x24, 0xd0, 0x48, 0x81, 0xec, 0x30, 0x01, 0x00 } },
    { 0x40CDE0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    { 0x498990, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83 } },
    { 0x1AA540, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x9a, 0xf8, 0x04, 0x00 } },
    { 0x3BB330, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x50, 0x83, 0xb9, 0xbc, 0x05, 0x00, 0x00 } },
    { 0x497B10, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xf0 } },
    // The depot tool's frame (mov rax,rsp; five pushes; lea rbp,[rax-108h]) and its mark.
    { 0x2B2DD0, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xf8 } },
    { 0x2B2DE0, { 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 10 },
    { 0x40BD60, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    // The sending steps.
    { 0x68B2B0, { 0x48, 0x8b, 0x81, 0x08, 0x17, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x29, 0xf3, 0x0f, 0x10, 0x05 } },
    { 0x3E2900, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x44, 0x88, 0x4c, 0x24, 0x20, 0x56 } },
    { 0x3E3F40, { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec } },
    { 0x3BB640, { 0x4d, 0x8b, 0xd0, 0x48, 0x85, 0xd2, 0x75, 0x03, 0x32, 0xc0, 0xc3, 0x48, 0x8b, 0x82, 0xb0, 0x05 } },
    { 0x3A38F0, { 0x48, 0x8b, 0x82, 0x48, 0x0a, 0x00, 0x00, 0x45, 0x33, 0xc0, 0x48, 0x2b, 0x82, 0x40, 0x0a, 0x00 } },
    { 0x3A3830, { 0x45, 0x33, 0xd2, 0x0f, 0x28, 0xd9, 0x45, 0x85, 0xc9, 0x79, 0x56, 0x49, 0x8b, 0x80, 0x48, 0x0a } },
    { 0x6DE260, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x83, 0xb9, 0x04, 0x1a, 0x00, 0x00, 0x03, 0x48, 0x8b, 0xf9 } },
    { 0x1AE130, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xd9, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48 } },
    { 0x6B7070, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0xe0, 0x01, 0x00, 0x00, 0x48 } },
    { 0x6B7420, { 0x40, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x5c, 0x24, 0x40, 0x4d, 0x8b, 0xf8 } },
};

typedef U64 (*PickFn)(U64, U64, U64, U64, U64, U64, U64*, U64*);
typedef U64 (*ToolFn)(U64 world);
typedef U64 (*MarkFn)(U64 world, U64 vehicle, U64 a3, U64 refused);
typedef unsigned char (*LineTestFn)(U64 world, U64 vehicle, U64 line, int* result, U64 zero);
typedef unsigned char (*HomeTestFn)(U64 world, U64 vehicle);
typedef void (*LinePutFn)(U64 lines, U64 line, U64 vehicle, U64 one, U64 subject);
typedef void (*StartFn)(U64 world, U64 vehicle);

static int g_ready = -1;
static unsigned g_origin = 0; // this PC's commands, to use its own window

void RouteTools_Bind(const TsmHost* host)
{
    TsmBind(host);
    g_origin = (unsigned)(GetTickCount64() * 2654435761u) ^ GetCurrentProcessId() ^ (unsigned)(U64)&g_origin;
    if (!g_origin) g_origin = 1;
}

bool RouteTools_Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const RoutineEntry& e : kRoutines) {
            if (memcmp(g_exeBase + e.rva, e.entry, e.size ? e.size : sizeof(e.entry)) != 0) {
                Logf("republicmp  ROUTE: game routine at 0x%X differs - the route tools are put away in lockstep", e.rva);
                g_ready = 0;
                break;
            }
        }
    }
    return g_ready == 1;
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
enum {
    ROUTE_VEHICLE_STOP = 0,
    ROUTE_LINE_STOP = 1,
    ROUTE_LINE_COPY = 2,
    ROUTE_DEPOT = 3,
    ROUTE_SQUARE_ROAD = 4,
    ROUTE_GO_THERE = 5,
    ROUTE_BOJKA = 6,
    ROUTE_JOIN_HUB_STOP = 7,
    ROUTE_JOIN_HUB = 8,
    ROUTE_WORKER_RELOCATE = 9,
    ROUTE_VEHICLE_COPY = 10,
    ROUTE_VEHICLE_REPLACE = 11,
    ROUTE_VEHICLE_REPLACE_AUTO = 12,
    ROUTE_VEHICLE_TO_VEHICLE = 13,
    ROUTE_VEHICLE_SEND_CONSTR = 14
};

struct RouteMsg {
    unsigned kind;
    unsigned origin;
    int index;        // stops: the window's stop index; square: type; bojka/relocate/replace: mode
    RouteRef object;  // stops: the vehicle (kind 0) or the line (kind 1); line copy: the line
    RouteRef building;
    RouteRef node;
    RouteRef vehicle; // line copy: the picked vehicle
    RouteRef subject; // line copy: the window's vehicle, or none
    unsigned char ctrl;
    float posA[3];
    float posB[3];
};

// The depot tool: the depot and the vehicles the tool listed, in its order.
struct DepotMsg {
    unsigned kind; // ROUTE_DEPOT
    unsigned count;
    RouteRef depot;
    RouteRef vehicles[DEPOT_MAX];
};
#pragma pack(pop)


static const char* KindText(unsigned kind)
{
    switch (kind) {
    case ROUTE_VEHICLE_STOP: return "a vehicle's stop";
    case ROUTE_LINE_STOP: return "a line's stop";
    case ROUTE_LINE_COPY: return "a vehicle on a line";
    case ROUTE_DEPOT: return "vehicles sent to a depot";
    case ROUTE_SQUARE_ROAD: return "a square road/water area";
    case ROUTE_GO_THERE: return "a vehicle's go-there target";
    case ROUTE_BOJKA: return "a buoy action";
    case ROUTE_JOIN_HUB_STOP: return "a hub stop link";
    case ROUTE_JOIN_HUB: return "a network node join";
    case ROUTE_WORKER_RELOCATE: return "a worker relocation";
    case ROUTE_VEHICLE_COPY: return "a vehicle's schedule copy";
    case ROUTE_VEHICLE_REPLACE: return "a vehicle replacement";
    case ROUTE_VEHICLE_REPLACE_AUTO: return "an auto-replace vehicle pick";
    case ROUTE_VEHICLE_TO_VEHICLE: return "a vehicle loaded onto a vehicle";
    case ROUTE_VEHICLE_SEND_CONSTR: return "a construction vehicle dispatch";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

// An open line window whose working copy is this object.
static bool LineWindowCopy(U64 object)
{
    U64 v = (U64)(g_exeBase + LINE_WINDOWS_RVA);
    U64 begin = Q(v), end = Q(v + 8);
    if (!begin || end < begin || (end - begin) / 8 > 4096) return false;
    for (U64 at = begin; at < end; at += 8) {
        U64 w = Q(at);
        if (w && Q(w + LINE_WINDOW_COPY) == object) return true;
    }
    return false;
}

static void Refused(const char* what, const char* why)
{
    Logf("republicmp  ROUTE: %s not shared - %s; nothing is done", what, why);
}

static const char* CaptureStop(int kind, U64 building, U64 node, RouteMsg& m)
{
    U64 world = (U64)World();
    U64 object = Q(world + TARGET_OFFSET), window = Q(world + WINDOW_OFFSET);
    memset(&m, 0, sizeof(m));
    m.kind = kind == 0 ? ROUTE_VEHICLE_STOP : ROUTE_LINE_STOP;
    m.origin = g_origin;
    m.building = m.node = m.vehicle = m.subject = kNoRef;
    if (!object || !window) return "no window";
    m.index = *(int*)(window + WINDOW_INDEX);
    if (kind == 0 ? !Ref_Vehicle(object, m.object) : !Ref_Line(object, m.object)) return kind == 0 ? "the vehicle" : "the line";
    if (building && !Ref_Building(building, m.building)) return "the building";
    if (node && !Ref_Node(node, m.node)) return "the node";
    if (!building && !node) return "nothing picked";
    return nullptr;
}

// The replay (main thread): while set, the pick answers 1 with the carried result.
static bool t_forced = false;
static U64 t_forcedBuilding = 0, t_forcedNode = 0;
static int t_forcedUsed = 0;

U64 RouteTools_StopPick(int kind, U64 world, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64* building, U64* node)
{
    if (t_forced) {
        t_forcedUsed++;
        if (building) *building = t_forcedBuilding;
        if (node) *node = t_forcedNode;
        return 1;
    }
    U64 r = ((PickFn)(g_exeBase + 0x2B5120))(world, a2, a3, a4, a5, a6, building, node);
    if (!(r & 0xff) || !Commands_TakeLocal()) return r;
    __try {
        // The line window's "New stop" edits its working copy: local until the window's Accept.
        if (kind == 1 && LineWindowCopy(Q((U64)World() + TARGET_OFFSET))) return r;
        if (kind == 0 && !TwoStates_CheckVehicleControlAllowed((const BYTE*)Q((U64)World() + TARGET_OFFSET),
                                                               "изменение маршрута чужого транспорта")) {
            if (building) *building = 0;
            if (node) *node = 0;
            return r & ~(U64)0xff;
        }
        RouteMsg m;
        const char* why = RouteTools_Ready() ? CaptureStop(kind, building ? *building : 0, node ? *node : 0, m)
                                              : "a game routine differs";
        if (why)
            Refused(KindText(kind), why);
        else
            Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(kind), "a fault while reading it");
    }
    if (building) *building = 0;
    if (node) *node = 0;
    return r & ~(U64)0xff; // the tool adds nothing now
}

// The copy line tool with the click hidden: its mark tells the pick.
static bool g_markArmed = false, g_markSeen = false, g_markAllowed = false;
static U64 g_markVehicle = 0;

U64 RouteTools_LineMark(U64 world, U64 vehicle, U64 a3, U64 refused)
{
    if (g_markArmed && !g_markSeen) {
        g_markSeen = true;
        g_markVehicle = vehicle;
        g_markAllowed = (refused & 0xff) == 0;
    }
    return ((MarkFn)(g_exeBase + LINE_MARK))(world, vehicle, a3, refused);
}

U64 RouteTools_LineCopyTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + LINE_COPY_TOOL);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    U64 window = Q(world + WINDOW_OFFSET), subject = 0;
    if (window && *(int*)(window + WINDOW_KIND) == 1) subject = Q(window + WINDOW_VEHICLE);
    unsigned char ctrl = *(g_exeBase + CTRL_RVA);
    BYTE kept = *press;
    *press = 0;
    g_markArmed = true;
    g_markSeen = g_markAllowed = false;
    g_markVehicle = 0;
    U64 r = tool(world);
    g_markArmed = false;
    *press = kept;
    if (!g_markSeen || !g_markAllowed || !g_markVehicle) return r;
    if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)g_markVehicle, "назначение маршрута чужому транспорту"))
        return r;
    const char* what = KindText(ROUTE_LINE_COPY);
    if (!RouteTools_Ready()) {
        Refused(what, "a game routine differs");
        return r;
    }
    RouteMsg m;
    memset(&m, 0, sizeof(m));
    m.kind = ROUTE_LINE_COPY;
    m.origin = g_origin;
    m.index = 0;
    m.building = m.node = m.subject = kNoRef;
    m.ctrl = ctrl ? 1 : 0;
    __try {
        if (!Ref_Line(Q(world + TARGET_OFFSET), m.object)) Refused(what, "the line");
        else if (!Ref_Vehicle(g_markVehicle, m.vehicle)) Refused(what, "the vehicle");
        else if (subject && !Ref_Vehicle(subject, m.subject)) Refused(what, "the window's vehicle");
        else Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(what, "a fault while reading it");
    }
    return r;
}

// The depot tool (vehicle_godepo, vehicle_godepo_batch) and other 40bd60-marked tools
// with the click hidden: its mark gives the target and whether it is allowed.
static bool g_depotArmed = false, g_depotSeen = false, g_depotOverflow = false, g_depotAllowed = false;
static U64 g_depot = 0, g_depotReturnTo = 0;
static U64 g_depotList[DEPOT_MAX];
static unsigned g_depotCount = 0;

U64 RouteTools_DepotMark(U64 world, U64 depot, U64 a3, U64 a4, U64 a5, U64 callerRsp, U64 returnTo)
{
    if (g_depotArmed && !g_depotSeen) {
        g_depotSeen = true;
        g_depot = depot;
        g_depotAllowed = (a4 & 0xff) == 0;
        g_depotReturnTo = returnTo;
        __try {
            if (returnTo == (U64)(g_exeBase + DEPOT_MARK_RETURN)) {
                U64 begin = Q(callerRsp + DEPOT_LIST_BEGIN), end = Q(callerRsp + DEPOT_LIST_END);
                if (begin && end >= begin && (end - begin) % 8 == 0) {
                    U64 n = (end - begin) / 8;
                    if (n > DEPOT_MAX) g_depotOverflow = true;
                    else {
                        for (U64 i = 0; i < n; i++) g_depotList[i] = Q(begin + i * 8);
                        g_depotCount = (unsigned)n;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_depotCount = 0;
        }
    }
    typedef U64 (*DepotMarkFn)(U64, U64, U64, U64, U64);
    return ((DepotMarkFn)(g_exeBase + DEPOT_MARK))(world, depot, a3, a4, a5);
}

U64 RouteTools_DepotTool(U64 world, U64 batch)
{
    typedef U64 (*DepotToolFn)(U64, U64);
    DepotToolFn tool = (DepotToolFn)(g_exeBase + DEPOT_TOOL);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world, batch);
    BYTE kept = *press;
    *press = 0;
    g_depotArmed = true;
    g_depotSeen = g_depotOverflow = g_depotAllowed = false;
    g_depot = g_depotReturnTo = 0;
    g_depotCount = 0;
    U64 r = tool(world, batch);
    g_depotArmed = false;
    *press = kept;
    if (!g_depotSeen || !g_depot || (!g_depotCount && !g_depotOverflow)) return r;
    const char* what = KindText(ROUTE_DEPOT);
    if (g_depotOverflow) {
        Refused(what, "more vehicles than a command carries");
        return r;
    }
    if (!RouteTools_Ready()) {
        Refused(what, "a game routine differs");
        return r;
    }
    DepotMsg m;
    memset(&m, 0, sizeof(m));
    m.kind = ROUTE_DEPOT;
    m.count = g_depotCount;
    for (unsigned i = 0; i < DEPOT_MAX; i++) m.vehicles[i] = kNoRef;
    __try {
        if (!TwoStates_CheckBuildingControlAllowed(*(int*)g_depot, *(float*)(g_depot + 0x350), *(float*)(g_depot + 0x358),
                                                   "отправка в чужое депо"))
            return r;
        for (unsigned i = 0; i < g_depotCount; i++) {
            if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)g_depotList[i], "отправка чужого транспорта в депо"))
                return r;
        }
        const char* why = nullptr;
        if (!Ref_Building(g_depot, m.depot)) why = "the depot";
        for (unsigned i = 0; !why && i < g_depotCount; i++)
            if (!Ref_Vehicle(g_depotList[i], m.vehicles[i])) why = "a vehicle";
        if (why) Refused(what, why);
        else Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(what, "a fault while reading it");
    }
    return r;
}

U64 RouteTools_SecondRoadTool(U64 world, U64 type, U64 square)
{
    typedef U64 (*SecondRoadFn)(U64, U64, U64);
    SecondRoadFn tool = (SecondRoadFn)(g_exeBase + 0x3CF6F0);
    if ((square & 0xff) == 0) return tool(world, type, square);
    BYTE* down = g_exeBase + 0xA54E90;
    if (*down || !*(BYTE*)(world + 0xD2AC) || !Commands_TakeLocal()) return tool(world, type, square);
    // On mouse release while dragging a square: keep down = 1 during the call so 3cf6f0
    // updates the preview and checks validity (+11a10) without committing locally.
    *down = 1;
    U64 r = tool(world, type, square);
    *down = 0;
    __try {
        bool valid = *(int*)(world + 0x11A10) == -1 && Q(world + 0xF610) != Q(world + 0xF608);
        float start[3], end[3];
        memcpy(start, (const void*)(world + 0xF620), 12);
        memcpy(end, (const void*)(world + 0xF62C), 12);
        *(U64*)(world + 0xF610) = *(U64*)(world + 0xF608);
        *(BYTE*)(world + 0xD2AC) = 0;
        if (!valid) return r;
        if (!TwoStates_CheckBuildAllowed(start[0], start[2], "строительство площади/водоёма") ||
            !TwoStates_CheckBuildAllowed(end[0], end[2], "строительство площади/водоёма"))
            return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_SQUARE_ROAD;
        m.origin = g_origin;
        m.index = (int)type;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        m.ctrl = *(BYTE*)(world + 0xF63C);
        memcpy(m.posA, start, 12);
        memcpy(m.posB, end, 12);
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_SQUARE_ROAD), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_GoThereTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x3CA560);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    BYTE kept = *press;
    *press = 0;
    U64 r = tool(world);
    *press = kept;
    __try {
        U64 v = Q(world + TARGET_OFFSET);
        if (!v || !Q(world + SELECTED_OFFSET)) return r;
        if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)v, "отправка чужого транспорта в точку")) return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_GO_THERE;
        m.origin = g_origin;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        memcpy(m.posA, (const void*)(world + 0xF6C), 12);
        if (!Ref_Vehicle(v, m.vehicle)) {
            Refused(KindText(ROUTE_GO_THERE), "the vehicle");
            return r;
        }
        *(U64*)(world + SELECTED_OFFSET) = 0;
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_GO_THERE), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_BojkaTool(U64 world, U64 mode)
{
    typedef U64 (*BojkaFn)(U64, U64);
    BojkaFn tool = (BojkaFn)(g_exeBase + 0x3C85B0);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world, mode);
    BYTE kept = *press;
    *press = 0;
    U64 r = tool(world, mode);
    *press = kept;
    __try {
        float pos[3];
        memcpy(pos, (const void*)(world + 0xF6C), 12);
        if (!TwoStates_CheckBuildAllowed(pos[0], pos[2], "установка/снос буйка")) return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_BOJKA;
        m.origin = g_origin;
        m.index = (int)(mode & 0xff);
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        memcpy(m.posA, pos, 12);
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_BOJKA), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_JoinHubStopTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x2B23B0);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    BYTE kept = *press;
    *press = 0;
    U64 r = tool(world);
    *press = kept;
    __try {
        U64 hub = Q(world + TARGET_OFFSET);
        U64 stopNode = Q(world + 0xF650);
        if (!hub || !stopNode) return r;
        U64 hubBeg = Q(hub + 0x88), hubEnd = Q(hub + 0x90);
        if (!hubBeg || hubEnd <= hubBeg) return r;
        U64 firstNode = Q(hubBeg);
        if (!firstNode) return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_JOIN_HUB_STOP;
        m.origin = g_origin;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        memcpy(m.posA, (const void*)(firstNode + 4), 12);
        memcpy(m.posB, (const void*)(stopNode + 4), 12);
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_JOIN_HUB_STOP), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_JoinHubTool(U64 world, U64 a2)
{
    typedef U64 (*JoinHubFn)(U64, U64);
    JoinHubFn tool = (JoinHubFn)(g_exeBase + 0x3C9A00);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world, a2);
    BYTE kept = *press;
    *press = 0;
    U64 r = tool(world, a2);
    *press = kept;
    __try {
        U64 node = Q(world + 0xF650);
        if (!node) return r;
        float pos[3];
        memcpy(pos, (const void*)(node + 4), 12);
        if (!TwoStates_CheckBuildAllowed(pos[0], pos[2], "объединение узлов сети")) return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_JOIN_HUB;
        m.origin = g_origin;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        memcpy(m.posA, pos, 12);
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_JOIN_HUB), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_WorkerRelocateTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x2B8CE0);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    int mode = 0;
    __try {
        U64 sel = Q(world + SELECTED_OFFSET);
        const char* name = sel ? *(const char**)sel : nullptr;
        if (name) {
            if (strcmp(name, "worker_relocate_educated") == 0) mode = 1;
            else if (strcmp(name, "worker_relocate_inmate") == 0) mode = 2;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    unsigned char ctrl = *(g_exeBase + CTRL_RVA);
    BYTE kept = *press;
    *press = 0;
    g_depotArmed = true;
    g_depotSeen = g_depotOverflow = g_depotAllowed = false;
    g_depot = g_depotReturnTo = 0;
    U64 r = tool(world);
    g_depotArmed = false;
    *press = kept;
    if (!g_depotSeen || !g_depotAllowed || !g_depot) return r;
    __try {
        U64 srcBld = Q(world + TARGET_OFFSET);
        if (!srcBld) return r;
        if (!TwoStates_CheckBuildingControlAllowed(*(int*)srcBld, *(float*)(srcBld + 0x350), *(float*)(srcBld + 0x358),
                                                   "переселение граждан из чужого здания") ||
            !TwoStates_CheckBuildingControlAllowed(*(int*)g_depot, *(float*)(g_depot + 0x350), *(float*)(g_depot + 0x358),
                                                   "переселение граждан в чужое здание"))
            return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_WORKER_RELOCATE;
        m.origin = g_origin;
        m.index = mode;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        m.ctrl = ctrl ? 1 : 0;
        if (!Ref_Building(srcBld, m.object) || !Ref_Building(g_depot, m.building)) {
            Refused(KindText(ROUTE_WORKER_RELOCATE), "a building");
            return r;
        }
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_WORKER_RELOCATE), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_VehicleCopyTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x2B18C0);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    unsigned char ctrl = *(g_exeBase + CTRL_RVA);
    BYTE kept = *press;
    *press = 0;
    g_depotArmed = true;
    g_depotSeen = g_depotOverflow = g_depotAllowed = false;
    g_depot = g_depotReturnTo = 0;
    U64 r = tool(world);
    g_depotArmed = false;
    *press = kept;
    if (!g_depotSeen || !g_depotAllowed || !g_depot) return r;
    __try {
        U64 srcVeh = Q(world + TARGET_OFFSET);
        if (!srcVeh) return r;
        if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)g_depot, "копирование расписания чужому транспорту"))
            return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_VEHICLE_COPY;
        m.origin = g_origin;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        m.ctrl = ctrl ? 1 : 0;
        if (!Ref_Vehicle(srcVeh, m.object) || !Ref_Vehicle(g_depot, m.vehicle)) {
            Refused(KindText(ROUTE_VEHICLE_COPY), "a vehicle");
            return r;
        }
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_VEHICLE_COPY), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_VehicleReplaceTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x2B3C80);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    BYTE kept = *press;
    *press = 0;
    g_depotArmed = true;
    g_depotSeen = g_depotOverflow = g_depotAllowed = false;
    g_depot = g_depotReturnTo = 0;
    U64 r = tool(world);
    g_depotArmed = false;
    *press = kept;
    if (!g_depotSeen || !g_depotAllowed || !g_depot) return r;
    __try {
        U64 srcVeh = Q(world + TARGET_OFFSET);
        if (!srcVeh) return r;
        if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)srcVeh, "замена чужого транспорта")) return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_VEHICLE_REPLACE;
        m.origin = g_origin;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        if (!Ref_Vehicle(srcVeh, m.object)) {
            Refused(KindText(ROUTE_VEHICLE_REPLACE), "the source vehicle");
            return r;
        }
        if (g_depotReturnTo == (U64)(g_exeBase + 0x2B4464)) {
            if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)g_depot, "замена чужого транспорта")) return r;
            m.index = 0;
            if (!Ref_Vehicle(g_depot, m.vehicle)) {
                Refused(KindText(ROUTE_VEHICLE_REPLACE), "the target vehicle");
                return r;
            }
        } else {
            m.index = 1;
            if (!Ref_Building(g_depot, m.building)) {
                Refused(KindText(ROUTE_VEHICLE_REPLACE), "the target building");
                return r;
            }
        }
        *(U64*)(world + SELECTED_OFFSET) = 0;
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_VEHICLE_REPLACE), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_VehicleReplaceAutoTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x2B4680);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    BYTE kept = *press;
    *press = 0;
    g_depotArmed = true;
    g_depotSeen = g_depotOverflow = g_depotAllowed = false;
    g_depot = g_depotReturnTo = 0;
    U64 r = tool(world);
    g_depotArmed = false;
    *press = kept;
    if (!g_depotSeen || !g_depotAllowed || !g_depot) return r;
    __try {
        if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)g_depot, "автозамена чужого транспорта")) return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_VEHICLE_REPLACE_AUTO;
        m.origin = g_origin;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        if (!Ref_Vehicle(g_depot, m.vehicle)) {
            Refused(KindText(ROUTE_VEHICLE_REPLACE_AUTO), "the vehicle");
            return r;
        }
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_VEHICLE_REPLACE_AUTO), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_VehicleToVehicleTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x2B6AB0);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    BYTE kept = *press;
    *press = 0;
    g_markArmed = true;
    g_markSeen = g_markAllowed = false;
    g_markVehicle = 0;
    U64 r = tool(world);
    g_markArmed = false;
    *press = kept;
    if (!g_markSeen || !g_markAllowed || !g_markVehicle) return r;
    __try {
        U64 srcVeh = Q(world + TARGET_OFFSET);
        if (!srcVeh) return r;
        if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)srcVeh, "погрузка чужого транспорта") ||
            !TwoStates_CheckVehicleControlAllowed((const BYTE*)g_markVehicle, "погрузка на чужой транспорт"))
            return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_VEHICLE_TO_VEHICLE;
        m.origin = g_origin;
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        if (!Ref_Vehicle(srcVeh, m.object) || !Ref_Vehicle(g_markVehicle, m.vehicle)) {
            Refused(KindText(ROUTE_VEHICLE_TO_VEHICLE), "a vehicle");
            return r;
        }
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_VEHICLE_TO_VEHICLE), "a fault while reading it");
    }
    return r;
}

U64 RouteTools_VehicleSendConstrTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + 0x1D88A0);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    BYTE kept = *press;
    *press = 0;
    g_depotArmed = true;
    g_depotSeen = g_depotOverflow = g_depotAllowed = false;
    g_depot = g_depotReturnTo = 0;
    U64 r = tool(world);
    g_depotArmed = false;
    *press = kept;
    if (!g_depotSeen || !g_depotAllowed || !g_depot) return r;
    __try {
        U64 srcVeh = Q(world + TARGET_OFFSET);
        if (!srcVeh) return r;
        if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)srcVeh, "отправка чужой строительной техники")) return r;
        RouteMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = ROUTE_VEHICLE_SEND_CONSTR;
        m.origin = g_origin;
        m.index = *(int*)(world + 0xD2A8);
        m.object = m.building = m.node = m.vehicle = m.subject = kNoRef;
        if (!Ref_Vehicle(srcVeh, m.object) || !Ref_Building(g_depot, m.building)) {
            Refused(KindText(ROUTE_VEHICLE_SEND_CONSTR), "the vehicle or building");
            return r;
        }
        Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(ROUTE_VEHICLE_SEND_CONSTR), "a fault while reading it");
    }
    return r;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

static BYTE g_standInWindow[STAND_IN_BYTES], g_standInTool[STAND_IN_BYTES];

// The tool itself with this PC's own state put aside: the object as +d298, a
// window (this PC's own when it is the clicking PC and still shows the object,
// else a stand-in holding the index) as +d2a0, a tool selected, menus closed,
// the pick forced to the carried result.
static const char* ApplyStop(const RouteMsg& m, int* used)
{
    U64 world = (U64)World();
    U64 object = m.kind == ROUTE_VEHICLE_STOP ? Ref_VehicleAt(m.object) : Ref_LineAt(m.object);
    if (!object) return m.kind == ROUTE_VEHICLE_STOP ? "the vehicle is gone" : "the line is gone";
    U64 building = 0, node = 0;
    if (m.building.place >= 0 && !(building = Ref_BuildingAt(m.building))) return "the building is gone";
    if (m.node.place >= 0 && !(node = Ref_NodeAt(m.node))) return "the node is gone";
    if (!building && !node) return "nothing picked";
    static const unsigned kMenus[3] = MENU_OFFSETS;
    U64 own298 = Q(world + TARGET_OFFSET), own2a0 = Q(world + WINDOW_OFFSET), own428 = Q(world + SELECTED_OFFSET);
    BYTE menus[3];
    for (int i = 0; i < 3; i++) menus[i] = *(BYTE*)(world + kMenus[i]);
    bool ownWindow = m.origin == g_origin && own2a0 && own298 == object;
    memset(g_standInWindow, 0, sizeof(g_standInWindow));
    memset(g_standInTool, 0, sizeof(g_standInTool));
    U64 window = ownWindow ? own2a0 : (U64)g_standInWindow;
    *(int*)(window + WINDOW_INDEX) = m.index;
    *(U64*)(world + TARGET_OFFSET) = object;
    *(U64*)(world + WINDOW_OFFSET) = window;
    *(U64*)(world + SELECTED_OFFSET) = own428 ? own428 : (U64)g_standInTool;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = 0;
    t_forced = true;
    t_forcedBuilding = building;
    t_forcedNode = node;
    t_forcedUsed = 0;
    bool faulted = false;
    __try {
        ((ToolFn)(g_exeBase + kStopTools[m.kind]))(world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    t_forced = false;
    *used = t_forcedUsed;
    *(U64*)(world + TARGET_OFFSET) = own298;
    *(U64*)(world + WINDOW_OFFSET) = own2a0;
    *(U64*)(world + SELECTED_OFFSET) = own428;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = menus[i];
    return faulted ? "the tool faulted" : nullptr;
}

// The copy line tool's click steps (2b4f69..2b4fd6).
static const char* ApplyLineCopy(const RouteMsg& m, int* result)
{
    U64 world = (U64)World();
    U64 line = Ref_LineAt(m.object), vehicle = Ref_VehicleAt(m.vehicle), subject = 0;
    if (!line) {
        Logf("republicmp  ROUTE: the line (list %u, place %d) is not in this PC's lines manager", m.object.list, m.object.place);
        return "the line is gone";
    }
    if (!vehicle) return "the vehicle is gone";
    if (m.subject.place >= 0 && !(subject = Ref_VehicleAt(m.subject))) return "the window's vehicle is gone";
    U64 lines = Q(world + LINES_OFFSET);
    if (!lines) return "no lines manager";
    *result = -1;
    __try {
        ((LineTestFn)(g_exeBase + LINE_TEST))(world, vehicle, line, result, 0);
        if (*result != -1) return nullptr;
        bool home = Q(vehicle + 0x4F8) != 0 && ((HomeTestFn)(g_exeBase + LINE_HOME_TEST))(world, vehicle) != 0;
        ((LinePutFn)(g_exeBase + LINE_PUT))(lines, line, vehicle, 1, subject);
        if (home && m.ctrl) ((StartFn)(g_exeBase + LINE_START))(world, vehicle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

// The depot tool's sending steps (2b380b..2b3a63), last listed vehicle first.
static const char* ApplyDepot(const DepotMsg& m, unsigned* sent)
{
    typedef unsigned char (*UnusableFn)(U64 vehicle);
    typedef unsigned char (*DepotTypeFn)(U64 world, U64 depot, U64 type, U64, U64);
    typedef unsigned char (*LinkedFn)(U64, U64 depot, U64 vehicle, U64);
    typedef int (*SlotFn)(U64, U64 depot);
    typedef unsigned char (*SlotFitsFn)(U64, float, U64 depot, int);
    typedef void (*UnloadFn)(U64 vehicle);
    typedef unsigned char (*AtHomeFn)(U64, U64 vehicle);
    typedef void (*RouteFn)(U64 world, U64, U64 vehicle, U64);
    typedef void (*SendFn)(U64 vehicle, U64 depot);
    typedef void (*SendResultFn)(U64 vehicle, U64 depot, int*, int*);
    U64 world = (U64)World();
    U64 depot = Ref_BuildingAt(m.depot);
    if (!depot) return "the depot is gone";
    if (!m.count || m.count > DEPOT_MAX) return "no vehicles";
    U64 list[DEPOT_MAX] = {};
    for (unsigned i = 0; i < m.count; i++)
        if (!(list[i] = Ref_VehicleAt(m.vehicles[i]))) return "a vehicle is gone";
    *sent = 0;
    __try {
        for (unsigned k = m.count; k-- > 0;) {
            U64 v = list[k];
            U64 type = Q(v + 0x1708);
            if (m.count > 1 && *(unsigned*)(type + 0x294) != 1) continue;
            if (((UnusableFn)(g_exeBase + 0x68B2B0))(v)) continue;
            if (!((DepotTypeFn)(g_exeBase + 0x3E2900))(world, depot, type, 0, 0) ||
                !((DepotTypeFn)(g_exeBase + 0x3E3F40))(world, depot, type, 0, 0))
                continue;
            if (*(BYTE*)(Q(v + 0x1710) + 0x25C)) {
                float load = *(float*)(type + 0x7A78);
                for (U64 at = Q(v + 0x3A0); at < Q(v + 0x3A8); at += 8) load += *(float*)(Q(Q(at) + 0x1708) + 0x7A78);
                if (((LinkedFn)(g_exeBase + 0x3BB640))(0, depot, v, 0) && load > *(float*)(Q(depot + 0x318) + 0x9E4))
                    continue;
            }
            if (Q(v + 0x4F8) == depot) continue;
            if (!Q(depot + 0x5D0) && *(unsigned*)(type + 0x294) == 6) {
                int slot = ((SlotFn)(g_exeBase + 0x3A38F0))(0, depot);
                if (slot < 0 || !((SlotFitsFn)(g_exeBase + 0x3A3830))(0, *(float*)(type + 0x7A78), depot, slot)) continue;
            }
            U64 dt = Q(depot + 0x318);
            if (dt && *(unsigned*)(dt + 0x360) == 0x2B && Q(v + 0x490) != Q(v + 0x488)) continue;
            if (*(unsigned*)(type + 0x294) == 8 && Q(v + 0xC80)) continue;
            if (*(unsigned*)(type + 0x294) == 0xA && *(unsigned*)(v + 0x1A04) == 3) ((UnloadFn)(g_exeBase + 0x6DE260))(v);
            if (Q(v + 0x4F8) && !((AtHomeFn)(g_exeBase + 0x1AA540))(0, v)) {
                *(U64*)(v + 0x600) = depot;
                U64 route = Q(v + 0x1AD0);
                if (!route) continue;
                ((RouteFn)(g_exeBase + 0x1AE130))(world, route, v, 0);
            } else {
                ((SendFn)(g_exeBase + 0x6B7070))(v, depot);
                int a = -1, c = -1;
                ((SendResultFn)(g_exeBase + 0x6B7420))(v, depot, &a, &c);
            }
            ++*sent;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplySquareRoad(const RouteMsg& m)
{
    U64 world = (U64)World();
    static const unsigned kMenus[3] = MENU_OFFSETS;
    BYTE* down = g_exeBase + 0xA54E90;
    BYTE* press = g_exeBase + PRESS_RVA;
    BYTE* rpress = g_exeBase + 0xA54C9F;
    float keptStart[3], keptEnd[3], keptCur[3];
    memcpy(keptStart, (const void*)(world + 0xF620), 12);
    memcpy(keptEnd, (const void*)(world + 0xF62C), 12);
    memcpy(keptCur, (const void*)(world + 0xF6C), 12);
    BYTE keptDrag = *(BYTE*)(world + 0xD2AC), keptPlan = *(BYTE*)(world + 0xF63C);
    BYTE keptDown = *down, keptPress = *press, keptRPress = *rpress;
    U64 own428 = Q(world + SELECTED_OFFSET);
    BYTE menus[3];
    for (int i = 0; i < 3; i++) menus[i] = *(BYTE*)(world + kMenus[i]);
    memset(g_standInTool, 0, sizeof(g_standInTool));
    memcpy((void*)(world + 0xF620), m.posA, 12);
    memcpy((void*)(world + 0xF62C), m.posB, 12);
    memcpy((void*)(world + 0xF6C), m.posB, 12);
    *(BYTE*)(world + 0xD2AC) = 1;
    *(BYTE*)(world + 0xF63C) = m.ctrl;
    *down = 0;
    *press = 0;
    *rpress = 0;
    *(U64*)(world + SELECTED_OFFSET) = own428 ? own428 : (U64)g_standInTool;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = 0;
    bool faulted = false;
    __try {
        typedef U64 (*SecondRoadFn)(U64, U64, U64);
        ((SecondRoadFn)(g_exeBase + 0x3CF6F0))(world, (U64)(unsigned)m.index, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    memcpy((void*)(world + 0xF620), keptStart, 12);
    memcpy((void*)(world + 0xF62C), keptEnd, 12);
    memcpy((void*)(world + 0xF6C), keptCur, 12);
    *(BYTE*)(world + 0xD2AC) = keptDrag;
    *(BYTE*)(world + 0xF63C) = keptPlan;
    *down = keptDown;
    *press = keptPress;
    *rpress = keptRPress;
    *(U64*)(world + SELECTED_OFFSET) = own428;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = menus[i];
    return faulted ? "the tool faulted" : nullptr;
}

static const char* ApplyGoThere(const RouteMsg& m)
{
    U64 v = Ref_VehicleAt(m.vehicle);
    if (!v) return "the vehicle is gone";
    __try {
        alignas(16) BYTE stopObj[0x200] = {};
        typedef void (*InitStopFn)(void*);
        typedef void (*ClearStopsFn)(U64);
        typedef void (*PushPtrFn)(U64, const U64*);
        typedef void (*AddStopFn)(U64, void*);
        typedef void (*DtorSubFn)(void*);
        ((InitStopFn)(g_exeBase + 0x1512A0))(stopObj);
        *(void**)(stopObj + 0x190) = g_exeBase + 0xA556B0;
        memcpy((void*)(v + 0x728), m.posA, 12);
        *(U64*)(v + 0x688) = *(U64*)(v + 0x680);
        ((ClearStopsFn)(g_exeBase + 0x1FF7A0))(v + 0x638);
        U64 zero = 0;
        ((PushPtrFn)(g_exeBase + 0x0B0E90))(v + 0x680, &zero);
        ((AddStopFn)(g_exeBase + 0x1FF8F0))(v + 0x638, stopObj);
        *(int*)(v + 0x698) = 0;
        *(BYTE*)(v + 0xD3A) = 1;
        ((DtorSubFn)(g_exeBase + 0x1513F0))(stopObj + 0xC8);
        ((DtorSubFn)(g_exeBase + 0x1513F0))(stopObj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyBojka(const RouteMsg& m)
{
    U64 world = (U64)World();
    static const unsigned kMenus[3] = MENU_OFFSETS;
    BYTE* press = g_exeBase + PRESS_RVA;
    float keptCur[3];
    memcpy(keptCur, (const void*)(world + 0xF6C), 12);
    BYTE keptPress = *press;
    BYTE menus[3];
    for (int i = 0; i < 3; i++) menus[i] = *(BYTE*)(world + kMenus[i]);
    memcpy((void*)(world + 0xF6C), m.posA, 12);
    *press = 1;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = 0;
    bool faulted = false;
    __try {
        typedef U64 (*BojkaFn)(U64, U64);
        ((BojkaFn)(g_exeBase + 0x3C85B0))(world, (U64)(unsigned)m.index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    memcpy((void*)(world + 0xF6C), keptCur, 12);
    *press = keptPress;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = menus[i];
    return faulted ? "the tool faulted" : nullptr;
}

static U64 FindNetworkNodeByPos(U64 world, const float pos[3], U64* outNet = nullptr)
{
    auto match = [&](U64 n) {
        if (!n) return false;
        float dx = *(float*)(n + 4) - pos[0];
        float dy = *(float*)(n + 8) - pos[1];
        float dz = *(float*)(n + 0xC) - pos[2];
        return dx * dx + dy * dy + dz * dz <= 0.05f;
    };
    auto checkNet = [&](U64 net) -> U64 {
        if (!net) return 0;
        for (U64 p = Q(net + 0x298); p && p < Q(net + 0x2A0); p += 8) {
            U64 n = Q(p);
            if (match(n)) {
                if (outNet) *outNet = net;
                return n;
            }
        }
        for (U64 p = Q(net + 0xAA0); p && p < Q(net + 0xAA8); p += 8) {
            U64 seg = Q(p);
            if (!seg) continue;
            for (int k = 0; k < 2; k++) {
                U64 n = Q(seg + 0x20 + k * 8);
                if (match(n)) {
                    if (outNet) *outNet = net;
                    return n;
                }
            }
        }
        return 0;
    };
    int netCount = *(int*)(world + 0x14034);
    for (int i = 0; i < netCount && i < 64; i++) {
        U64 n = checkNet(Q(world + 0x14038 + i * 8));
        if (n) return n;
    }
    static const unsigned kExtraNets[2] = { 0xF908u, 0xFA28u };
    for (unsigned off : kExtraNets) {
        U64 n = checkNet(Q(world + off));
        if (n) return n;
    }
    return 0;
}

static const char* ApplyJoinHubStop(const RouteMsg& m)
{
    U64 world = (U64)World();
    __try {
        U64 firstNode = FindNetworkNodeByPos(world, m.posA);
        U64 stopNode = FindNetworkNodeByPos(world, m.posB);
        U64 hub = firstNode ? Q(firstNode + 0xC0) : 0;
        if (!hub || !stopNode) return "the hub or stop is gone";
        U64 beg = Q(hub + 0x88), end = Q(hub + 0x90);
        bool inHub = false;
        for (U64 p = beg; p && p < end; p += 8) {
            if (Q(p) == stopNode) {
                inHub = true;
                break;
            }
        }
        typedef void (*RemoveFromHubFn)(U64 node, U64 hubVec);
        typedef unsigned char (*UpdateHubFn)(U64 hub, U64 zero);
        typedef unsigned char (*CanJoinHubFn)(U64 hub, U64 node);
        typedef float (*HubDistFn)(U64 hub, U64 node);
        typedef void (*AddToHubFn)(U64 hub, U64 node, unsigned char zero);
        if (inHub) {
            if ((end - beg) / 8 >= 2) {
                ((RemoveFromHubFn)(g_exeBase + 0x0F98D0))(stopNode, hub + 0x88);
                unsigned char ok = ((UpdateHubFn)(g_exeBase + 0x222B50))(hub, 0);
                if (m.origin == g_origin && Q(world + WINDOW_OFFSET))
                    *(int*)(Q(world + WINDOW_OFFSET) + 0x264) = ok ? 0 : 1;
            }
        } else {
            if (((CanJoinHubFn)(g_exeBase + 0x2251D0))(hub, stopNode) &&
                ((HubDistFn)(g_exeBase + 0x225530))(hub, stopNode) <= *(float*)(g_exeBase + 0x90A9F0)) {
                ((AddToHubFn)(g_exeBase + 0x2233A0))(hub, stopNode, 0);
                unsigned char ok = ((UpdateHubFn)(g_exeBase + 0x222B50))(hub, 0);
                if (m.origin == g_origin && Q(world + WINDOW_OFFSET))
                    *(int*)(Q(world + WINDOW_OFFSET) + 0x264) = ok ? 0 : 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyJoinHub(const RouteMsg& m)
{
    U64 world = (U64)World();
    __try {
        U64 net = 0;
        U64 node = FindNetworkNodeByPos(world, m.posA, &net);
        if (!node || !net) return "the node is gone";
        U64 eBeg = Q(node + 0x28), eEnd = Q(node + 0x30);
        if (!eBeg || (eEnd - eBeg) != 16) return "node does not join two edges";
        U64 e0 = Q(eBeg), e1 = Q(eBeg + 8);
        int idx0 = -1, idx1 = -1;
        U64 nBeg = Q(net + 0x2B0), nEnd = Q(net + 0x2B8);
        for (U64 p = nBeg; p && p < nEnd; p += 8) {
            int idx = (int)((p - nBeg) / 8);
            if (Q(p) == e0) idx0 = idx;
            if (Q(p) == e1) idx1 = idx;
        }
        if (idx0 < 0 || idx1 < 0) return "edges not found in network";
        if (idx0 != idx1) {
            int lo = idx0 < idx1 ? idx0 : idx1;
            int hi = idx0 < idx1 ? idx1 : idx0;
            typedef void (*JoinEdgesFn)(U64 net, int lo, int hi, unsigned char zero);
            ((JoinEdgesFn)(g_exeBase + 0x53E630))(net, lo, hi, 0);
        } else {
            typedef void (*RemoveLoopNodeFn)(U64 net, U64 node);
            ((RemoveLoopNodeFn)(g_exeBase + 0x541550))(net, node);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyWorkerRelocate(const RouteMsg& m)
{
    U64 world = (U64)World();
    U64 srcBld = Ref_BuildingAt(m.object), dstBld = Ref_BuildingAt(m.building);
    if (!srcBld || !dstBld) return "a building is gone";
    __try {
        if (m.index == 2) {
            typedef unsigned char (*RelocInmateFn)(U64 world, U64 src, U64 dst);
            int loops = m.ctrl ? 20 : 1;
            for (int i = 0; i < loops; i++)
                if (!((RelocInmateFn)(g_exeBase + 0x83CA50))(world, srcBld, dstBld)) break;
        } else {
            typedef unsigned char (*RelocWorkerFn)(U64 world, U64 src, U64 dst, unsigned char onlyEducated);
            if (m.index == 0 && !m.ctrl) {
                ((RelocWorkerFn)(g_exeBase + 0x83C6A0))(world, srcBld, dstBld, 0);
            } else {
                int count = (int)((Q(srcBld + 0x7A8) - Q(srcBld + 0x7A0)) >> 3);
                unsigned char edu = (m.index == 1 && !m.ctrl) ? 1 : 0;
                for (int i = 0; i < count; i++)
                    ((RelocWorkerFn)(g_exeBase + 0x83C6A0))(world, srcBld, dstBld, edu);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyVehicleCopy(const RouteMsg& m)
{
    U64 world = (U64)World();
    U64 srcVeh = Ref_VehicleAt(m.object), dstVeh = Ref_VehicleAt(m.vehicle);
    if (!srcVeh || !dstVeh) return "a vehicle is gone";
    __try {
        if (Q(srcVeh + 0x1AD0)) {
            U64 lines = Q(world + LINES_OFFSET);
            if (lines) ((LinePutFn)(g_exeBase + LINE_PUT))(lines, Q(srcVeh + 0x1AD0), dstVeh, 1, srcVeh);
        } else {
            typedef void (*CopySchedFn)(U64 dst638, U64 src638);
            typedef void (*PushPtrFn)(U64 vec, const U64* ptr);
            typedef void (*UpdateVehFn)(U64 world, U64 veh);
            typedef void (*ClearStopsFn)(U64 veh638);
            typedef void (*CopyFieldFn)(U64 dst, U64 src);
            ((CopySchedFn)(g_exeBase + 0x1FEF00))(dstVeh + 0x638, srcVeh + 0x638);
            *(U64*)(dstVeh + 0x688) = *(U64*)(dstVeh + 0x680);
            U64 n = (Q(srcVeh + 0x688) - Q(srcVeh + 0x680)) >> 3;
            if (*(BYTE*)(srcVeh + 0x5FD) && n > 0) n--;
            for (U64 i = 0; i < n; i++) {
                U64 stopPtr = Q(Q(srcVeh + 0x680) + i * 8);
                ((PushPtrFn)(g_exeBase + 0x0B0E90))(dstVeh + 0x680, &stopPtr);
            }
            if (*(BYTE*)(dstVeh + 0x5FD) && Q(dstVeh + 0x600)) {
                U64 depPtr = Q(dstVeh + 0x600);
                ((PushPtrFn)(g_exeBase + 0x0B0E90))(dstVeh + 0x680, &depPtr);
            }
            ((UpdateVehFn)(g_exeBase + 0x44A180))(world, dstVeh);
            *(int*)(dstVeh + 0x698) = *(int*)(srcVeh + 0x698);
            *(BYTE*)(dstVeh + 0xD3A) = 1;
            *(U64*)(dstVeh + 0x728) = *(U64*)(srcVeh + 0x728);
            *(unsigned*)(dstVeh + 0x730) = *(unsigned*)(srcVeh + 0x730);
            U64 bld = Q(srcVeh + 0x4F0);
            U64 btype = bld ? Q(bld + 0x318) : 0;
            int bkind = btype ? *(int*)(btype + 0x360) : -1;
            if (bkind == 0x26 || bkind == 0x2B || bkind == 0x16) {
                ((ClearStopsFn)(g_exeBase + 0x1FF7A0))(dstVeh + 0x638);
                *(U64*)(dstVeh + 0x688) = *(U64*)(dstVeh + 0x680);
                *(int*)(dstVeh + 0x698) = 0;
            }
            if (Q(dstVeh + 0x4F0) && Q(srcVeh + 0x4F0)) {
                U64 dType = Q(Q(dstVeh + 0x4F0) + 0x318), sType = Q(Q(srcVeh + 0x4F0) + 0x318);
                if (dType && sType && *(int*)(dType + 0x360) == 2 && *(int*)(sType + 0x360) == 2) {
                    for (U64 p = Q(srcVeh + 0x488); p < Q(srcVeh + 0x490); p += 8) {
                        U64 item = Q(p);
                        if (item) *(U64*)(item + 0x48) = dstVeh;
                    }
                    ((CopyFieldFn)(g_exeBase + 0x1E0FC0))(dstVeh + 0x488, srcVeh + 0x488);
                    *(U64*)(srcVeh + 0x490) = *(U64*)(srcVeh + 0x488);
                    if (Q(dstVeh + 0x490) != Q(dstVeh + 0x488)) {
                        U64 first = Q(Q(dstVeh + 0x488));
                        ((void (*)(U64, U64, int, U64))(g_exeBase + 0x67D5D0))(dstVeh, first, -1, 0);
                    }
                    *(int*)(srcVeh + 0x50C) = 3;
                }
            }
            *(BYTE*)(dstVeh + 0x1A6B) = *(BYTE*)(srcVeh + 0x1A6B);
            *(BYTE*)(dstVeh + 0x1A69) = *(BYTE*)(srcVeh + 0x1A69);
            *(BYTE*)(dstVeh + 0x1A6A) = *(BYTE*)(srcVeh + 0x1A6A);
            *(unsigned*)(dstVeh + 0x1700) = *(unsigned*)(srcVeh + 0x1700);
            ((void (*)(U64, U64))(g_exeBase + 0x67CD20))(dstVeh, srcVeh);
        }
        if (m.ctrl && Q(dstVeh + 0x4F8) != 0 && ((HomeTestFn)(g_exeBase + LINE_HOME_TEST))(world, dstVeh) != 0)
            ((StartFn)(g_exeBase + LINE_START))(world, dstVeh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyVehicleReplace(const RouteMsg& m)
{
    U64 world = (U64)World();
    U64 srcVeh = Ref_VehicleAt(m.object);
    if (!srcVeh) return "the source vehicle is gone";
    __try {
        if (m.index == 0) {
            U64 dstVeh = Ref_VehicleAt(m.vehicle);
            if (!dstVeh) return "the target vehicle is gone";
            typedef unsigned char (*CanReplaceVehFn)(U64 world, U64 src, U64 dst, U64 zero);
            typedef unsigned char (*DoReplaceVehFn)(U64 world, U64 src, U64 dst, unsigned char one);
            if (((CanReplaceVehFn)(g_exeBase + 0x1AA6E0))(world, srcVeh, dstVeh, 0))
                ((DoReplaceVehFn)(g_exeBase + 0x1AAAA0))(world, srcVeh, dstVeh, 1);
        } else {
            U64 dstBld = Ref_BuildingAt(m.building);
            if (!dstBld) return "the target building is gone";
            typedef unsigned char (*DoReplaceBldFn)(U64 world, U64 src, U64 dst);
            ((DoReplaceBldFn)(g_exeBase + 0x1AAD70))(world, srcVeh, dstBld);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyVehicleReplaceAuto(const RouteMsg& m)
{
    U64 world = (U64)World();
    U64 dstVeh = Ref_VehicleAt(m.vehicle);
    if (!dstVeh) return "the vehicle is gone";
    __try {
        U64 mgr = Q(world + 0xD440);
        if (!mgr) return "no auto-replace manager";
        typedef unsigned char (*AutoRepFn)(U64 mgr, U64 veh);
        if (((AutoRepFn)(g_exeBase + 0x614310))(mgr, dstVeh)) {
            U64 beg = Q(mgr + 0x40), end = Q(mgr + 0x48);
            bool found = false;
            for (U64 p = beg; p && p < end; p += 8)
                if (Q(p) == dstVeh) {
                    found = true;
                    break;
                }
            if (!found) {
                typedef void (*PushPtrFn)(U64 vec, const U64* ptr);
                ((PushPtrFn)(g_exeBase + 0x0B0E90))(mgr + 0x40, &dstVeh);
            }
            *(BYTE*)(dstVeh + 0x1A6C) = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyVehicleToVehicle(const RouteMsg& m)
{
    U64 world = (U64)World();
    U64 srcVeh = Ref_VehicleAt(m.object), dstVeh = Ref_VehicleAt(m.vehicle);
    if (!srcVeh || !dstVeh) return "a vehicle is gone";
    __try {
        typedef unsigned char (*LoadVehOntoVehFn)(U64 dst, U64 src, unsigned char one);
        if (((LoadVehOntoVehFn)(g_exeBase + 0x68A420))(dstVeh, srcVeh, 1) && m.origin == g_origin) {
            *(U64*)(world + SELECTED_OFFSET) = 0;
            *(BYTE*)(world + 0xFBC) = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

static const char* ApplyVehicleSendConstr(const RouteMsg& m)
{
    U64 world = (U64)World();
    U64 r15 = Ref_VehicleAt(m.object);
    U64 r13 = Ref_BuildingAt(m.building);
    if (!r15 || !r13) return "the vehicle or building is gone";
    __try {
        U64 r12 = Q(r15 + 0x4F0);
        if (!r12) return "no construction office";
        U64 vtype = Q(r15 + 0x1708);
        bool isMechanism = vtype && *(int*)(vtype + 0x8600) == 0x12;
        bool zeroSpeed = vtype && *(float*)(vtype + 0x8624) <= 0.0f;
        U64 carrier = 0;
        if (isMechanism || zeroSpeed) {
            U64 vec[3] = {};
            typedef void* (*CollectCarriersFn)(U64 world, U64* vec, U64 list, unsigned char one);
            typedef U64 (*PickCarrierFn)(U64 world, U64 veh, void* vec, unsigned char one);
            typedef void (*FreeVecFn)(U64 ptr, U64 count, U64 elemSize);
            void* vptr = ((CollectCarriersFn)(g_exeBase + 0x155440))(world, vec, r12 + 0xC70, 1);
            carrier = ((PickCarrierFn)(g_exeBase + 0x155D00))(world, r15, vptr, 1);
            if (vec[0]) ((FreeVecFn)(g_exeBase + 0x001A50))(vec[0], (vec[2] - vec[0]) >> 3, 8);
        }
        if (r13 == r12) {
            bool done = false;
            if (!zeroSpeed) {
                if (((unsigned char (*)(U64, U64))(g_exeBase + 0x3FB8E0))(world, r15)) done = true;
                else if (isMechanism && ((unsigned char (*)(U64, U64, U64, U64))(g_exeBase + 0x151150))(world, r15, carrier, r12))
                    done = true;
            } else {
                if (((unsigned char (*)(U64, U64, U64, U64))(g_exeBase + 0x151150))(world, r15, carrier, r12)) done = true;
            }
            if (!done)
                ((void (*)(U64, U64, U64, U64, unsigned char, unsigned char))(g_exeBase + 0x3FBDC0))(world, r15, 0, 0, 1, 1);
            if (m.origin == g_origin) *(U64*)(world + TARGET_OFFSET) = 0;
        } else {
            alignas(16) BYTE info[64] = {};
            typedef void* (*SiteInfoFn)(U64 world, void* out, U64 veh, U64 office, U64 site, int idx, int* idxOut);
            void* res = ((SiteInfoFn)(g_exeBase + 0x14FE20))(world, info, r15, r12, r13, m.index, nullptr);
            alignas(16) BYTE req[64] = {};
            memcpy(req, res, 64);
            U64 r14_site = *(U64*)(req + 0);
            U64 r11_veh = *(U64*)(req + 0x10);
            U64 rbx_host = r11_veh ? Q(r11_veh + 0x4A0) : 0;
            bool ok = false;
            if (rbx_host) {
                if (r14_site && ((unsigned char (*)(U64, U64))(g_exeBase + 0x0F99C0))(r11_veh, rbx_host + 0x488)) {
                    *(int*)(r11_veh + 0x50C) = 8;
                    *(int*)(rbx_host + 0x50C) = 8;
                    ((void (*)(U64, unsigned char))(g_exeBase + 0x67CB30))(rbx_host, 1);
                    ((void (*)(U64, U64, int, U64))(g_exeBase + 0x67D5D0))(rbx_host, r14_site, -1, 0);
                    *(int*)(rbx_host + 0x698) = 0;
                    *(BYTE*)(rbx_host + 0xD3A) = 1;
                    ok = true;
                }
            } else if (r11_veh) {
                bool sentViaCarrier = false;
                if (isMechanism || zeroSpeed) {
                    if (carrier && Q(r11_veh + 0x4F8) == Q(r11_veh + 0x4F0) && Q(r11_veh + 0x4F0) == Q(carrier + 0x4F0)) {
                        *(U64*)(req + 0x18) = carrier;
                    } else {
                        sentViaCarrier =
                            ((unsigned char (*)(U64, U64, U64, U64))(g_exeBase + 0x151150))(world, r11_veh, carrier, r14_site) != 0;
                    }
                }
                if (isMechanism && sentViaCarrier) {
                    ok = true;
                } else {
                    ok = ((unsigned char (*)(U64, void*, U64))(g_exeBase + 0x150960))(world, req, r12) != 0;
                }
            }
            if (ok && m.origin == g_origin) *(U64*)(world + TARGET_OFFSET) = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

bool RouteTools_ApplyCommand(const void* data, size_t size)
{
    if (!RouteTools_Ready()) return false;
    if (size == sizeof(DepotMsg) && *(const unsigned*)data == ROUTE_DEPOT) {
        DepotMsg d;
        memcpy(&d, data, sizeof(d));
        unsigned sent = 0;
        const char* why = ApplyDepot(d, &sent);
        Logf("republicmp  ROUTE: %s (%u listed, %u sent): %s", KindText(ROUTE_DEPOT), d.count, sent, why ? why : "done");
        return why == nullptr;
    }
    if (size != sizeof(RouteMsg)) return false;
    RouteMsg m;
    memcpy(&m, data, sizeof(m));
    const char* why = nullptr;
    char detail[64] = "";
    if (m.kind == ROUTE_VEHICLE_STOP || m.kind == ROUTE_LINE_STOP) {
        int used = 0;
        why = ApplyStop(m, &used);
        if (!why && !used) why = "the tool did not pick";
        sprintf_s(detail, ", stop index %d", m.index);
    } else if (m.kind == ROUTE_LINE_COPY) {
        int result = -1;
        why = ApplyLineCopy(m, &result);
        if (!why && result != -1) sprintf_s(detail, " - refused by the game (%d)", result);
    } else if (m.kind == ROUTE_SQUARE_ROAD) {
        why = ApplySquareRoad(m);
    } else if (m.kind == ROUTE_GO_THERE) {
        why = ApplyGoThere(m);
    } else if (m.kind == ROUTE_BOJKA) {
        why = ApplyBojka(m);
    } else if (m.kind == ROUTE_JOIN_HUB_STOP) {
        why = ApplyJoinHubStop(m);
    } else if (m.kind == ROUTE_JOIN_HUB) {
        why = ApplyJoinHub(m);
    } else if (m.kind == ROUTE_WORKER_RELOCATE) {
        why = ApplyWorkerRelocate(m);
    } else if (m.kind == ROUTE_VEHICLE_COPY) {
        why = ApplyVehicleCopy(m);
    } else if (m.kind == ROUTE_VEHICLE_REPLACE) {
        why = ApplyVehicleReplace(m);
    } else if (m.kind == ROUTE_VEHICLE_REPLACE_AUTO) {
        why = ApplyVehicleReplaceAuto(m);
    } else if (m.kind == ROUTE_VEHICLE_TO_VEHICLE) {
        why = ApplyVehicleToVehicle(m);
    } else if (m.kind == ROUTE_VEHICLE_SEND_CONSTR) {
        why = ApplyVehicleSendConstr(m);
    } else {
        why = "unknown kind";
    }
    Logf("republicmp  ROUTE: %s%s%s: %s", KindText(m.kind), detail, m.origin == g_origin ? " (this PC)" : "",
         why ? why : "done");
    return why == nullptr;
}
