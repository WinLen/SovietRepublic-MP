// SPDX-License-Identifier: GPL-3.0-only
// See game_calls.h. The sites and their argument kinds follow SoylentGreed's
// game-call commands (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "game_calls.h"
#include "commands.h"
#include "world_refs.h"
#include "line_accept.h"
#include "junction_accept.h"
#include "rename.h"
#include "building_switches.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "../game/session_patches.h"
#include "../session/two_states.h"
#include "../session/save_sync.h"
#include "../ui/native_menu.h"
#include "../ui/lang.h"
#include <array>
#include <limits.h>
#include <string.h>
#include <utility>
#include "../determinism/determinism.h"
#include <intrin.h>
#pragma intrinsic(_AddressOfReturnAddress)

#define SEND_TO_DEPOT 0x3FBD00
#define LINES_OFFSET 0x12800
#define DELETE_CLEANUP 0x496250
#define DRAG_RVA 0x996D98        // the depot window's dragged vehicle (interface state)
#define MAX_ARGS 7

// Argument kinds: x ignored (0), W the world, M the pointer world+worldPointer
// holds, V a vehicle, B a building, L a line, O a vehicle, building or line,
// o the same or null, b a byte, I a 32-bit integer, F a float (the fourth
// argument only: it travels in xmm3).
struct GameCallSite {
    unsigned site, target;
    const char* name;
    const char* schema;
    unsigned worldPointer;
    unsigned result; // what a captured call answers the window
    bool orDepot;    // when the routine answers false, the window sends the vehicle (argument 2) to its depot
    int special;
};

enum {
    CALL_PLAIN = 0,
    // Not made during a session at all: the window changes the simulation every
    // frame on this PC only (it shows what the simulation last computed instead).
    CALL_PRESENTATION,
    // The line window's Accept: the copy goes to line_accept.cpp.
    CALL_LINE_ACCEPT,
    // The Accept block's later call (presentation): the vehicles' +66c are put back first.
    CALL_ACCEPT_MEASURES,
    // The lines list's Delete: +1 = 0, 499600, then 4962c0(manager, 1).
    CALL_DELETE_LINE,
    // The junction window's Accept: the copy goes to junction_accept.cpp.
    CALL_JUNCTION_ACCEPT,
    // The settings page applies its copy of the game rules: the copy goes to commands.cpp.
    CALL_GAME_RULES,
    // Made on this PC only, never carried: a cache the simulation rebuilds before it reads it.
    CALL_LOCAL,
    // The routine reads the depot window's dragged vehicle (exe+996b98): it travels as argument 0,
    // and every PC puts it in its own global for the call.
    CALL_DRAG,
    // The rename window's write (5290 = swprintf): a city's or a crossroad's name goes to rename.cpp.
    CALL_RENAME,
    // Residential building window (6f8cc0): invite immigrants/experts buttons (6fee1e, 6ff31a, 6ff52d, 6ffa4e).
    CALL_INVITE_CITIZENS,
    // Depot window row drop (7c7e87 uncouple + 7c7ed8 couple): relay passes hovered vehicle r15 in r8
    // and raw slot vehicle r14 in r9 to guard against self-drops on click/double-click.
    CALL_DEPOT_ROW_UNCOUPLE,
    CALL_DEPOT_ROW_COUPLE,
    // Vehicle window (7d6b00): add Soviet/Western border stop, delete stop, change current stop, release vehicle.
    CALL_ADD_BORDER_STOP,
    CALL_DELETE_STOP,
    CALL_CHANGE_STOP,
    CALL_RELEASE_VEHICLE,
    CALL_MANUAL_BUY_RES,
};

// Appended only: a command names its site by index.
static const GameCallSite kSites[] = {
    { 0x7AA3C1, 0x497AA0, "line window: copy schedule to vehicle", "MOObo", 0x12800, 0, false },
    { 0x7AA3D5, 0x3BB270, "line window: refresh vehicle after schedule copy", "WO", 0, 0, false },
    { 0x7AA509, 0x1AAAC0, "line window: replace vehicle", "WoOb", 0, 0, false },
    { 0x7AAB29, 0x3BB270, "line window: refresh vehicle", "WO", 0, 0, false },
    { 0x7D9AF9, 0x6BF370, "vehicle window: rebuild route after assignment", "Vbbb", 0, 0, false, CALL_PRESENTATION },
    { 0x7D9DF1, 0x6BF370, "vehicle window: rebuild route after assignment (second)", "Vbbb", 0, 0, false, CALL_PRESENTATION },
    { 0x7DA491, 0x6C5460, "vehicle window: release vehicle", "V", 0, 0, false, CALL_RELEASE_VEHICLE },
    { 0x7DA49C, 0x3BB270, "vehicle window: refresh vehicle", "WO", 0, 0, false },
    { 0x7DA322, 0x6B7810, "vehicle window: toggle vehicle state", "V", 0, 0, false },
    { 0x7DA4E1, 0x6B7810, "vehicle window: toggle vehicle state (second)", "V", 0, 0, false },
    { 0x7D740E, 0x3FBD00, "vehicle window: send to depot (keyboard)", "WVbbbb", 0, 0, false },
    { 0x7DA2B9, 0x3FBD00, "vehicle window: send to depot", "WVbbbb", 0, 0, false },
    { 0x7E761B, 0x497AA0, "vehicle window: clear the vehicle's line", "MoVbo", 0x12800, 0, false },
    { 0x7E7623, 0x67CCE0, "vehicle window: refresh the vehicle after clearing its line", "V", 0, 0, false },
    { 0x7DA2C6, 0x3FB820, "vehicle window: send back to workplace", "xV", 0, 1, true },
    { 0x7DA8AB, 0x671150, "vehicle window: next color/skin", "VI", 0, 0, false },
    { 0x7D52C8, 0x3BB270, "line window: launch vehicle", "WO", 0, 0, false },
    { 0x7DA36B, 0x614840, "vehicle window: locomotive back to auto-replacement", "MV", 0xD440, 0, false },
    { 0x7D3B47, 0x491540, "line window: clear line data", "L", 0, 0, false },
    { 0x358FB7, 0x496D70, "lines list: create new line", "MI", 0x12800, 0, false },
    { 0x701F1E, 0x3FBD00, "building window: call all vehicles home (each vehicle)", "WVbbbb", 0, 0, false },
    { 0x743D43, 0x1EA120, "building window: try sending all vehicles with cargo", "WO", 0, 0, false },
    { 0x747DC9, 0x1EA220, "building window (second): try sending all vehicles with cargo", "WO", 0, 0, false },
    // 6bf1d0 rebuilds the cargo summary (vehicle +458/+470, the window's "Carried" list) from the
    // train's cargo; it writes nothing else, and the simulation's readers rebuild it first.
    { 0x7D723A, 0x6BF370, "vehicle window: cargo summary for display", "Vbbb", 0, 0, false, CALL_LOCAL },
    { 0x7E9EE6, 0x6BF370, "vehicle window: cargo summary for the schedule list", "Vbbb", 0, 0, false, CALL_LOCAL },
    { 0x7D1F60, 0x4957C0, "line window: accept changes", "xLxx", 0, 0, false, CALL_LINE_ACCEPT },
    { 0x7D1F6B, 0x494530, "line window: accept (merge equal lines)", "ML", 0x12800, 0, false, CALL_PRESENTATION },
    { 0x7D1FEE, 0x22A650, "line window: accept (mark the line changed)", "Lx", 0, 0, false, CALL_ACCEPT_MEASURES },
    { 0x7D1FFD, 0x4975E0, "line window: accept (vehicles take the stops)", "ML", 0x12800, 0, false, CALL_PRESENTATION },
    { 0x7D200B, 0x499590, "line window: accept (name a private line)", "xL", 0, 0, false, CALL_PRESENTATION },
    { 0x359231, 0x499590, "lines list: delete line", "xL", 0, 0, false, CALL_DELETE_LINE },
    { 0x35923F, 0x496250, "lines list: delete line (cleanup)", "Mb", 0x12800, 0, false, CALL_PRESENTATION },
    // A late-phase tool (main thread): building_addfield adds the picked field to the farm +d298.
    { 0x1D8754, 0x162120, "field tool: add a field to a farm", "WBOF", 0, 0, false },
    // The junction window's Accept block (7595bb..7595f9): the copy into the junction is carried,
    // the rest of the block is made by the command on every PC.
    { 0x759762, 0x224280, "junction window: accept (reset the junction)", "x", 0, 0, false, CALL_PRESENTATION },
    { 0x75976D, 0x2264B0, "junction window: accept (copy into the junction)", "xx", 0, 0, false, CALL_JUNCTION_ACCEPT },
    { 0x759785, 0x22A650, "junction window: accept (mark the junction changed)", "xx", 0, 0, false, CALL_PRESENTATION },
    { 0x75978C, 0x0F98D0, "junction window: accept (take the junction off the list)", "xx", 0, 0, false, CALL_PRESENTATION },
    // The game menu's settings page: "Save settings", closing the window and the "Change settings?"
    // popup apply the page's copy of the game rules with 337170(world).
    { 0x33A54F, 0x3370F0, "settings page: save settings", "W", 0, 0, false, CALL_GAME_RULES },
    { 0x33708F, 0x3370F0, "settings page: close", "W", 0, 0, false, CALL_GAME_RULES },
    { 0x7FEBDF, 0x3370F0, "settings popup: change settings", "W", 0, 0, false, CALL_GAME_RULES },
    // Selling a vehicle: the confirm window's Yes (7f2d50) and the building windows' "sell all
    // vehicles" prompts call 3dee50(world, vehicle, 1, 0, 0, 0, 0.0f), which queues the sale.
    { 0x7F3420, 0x3DED90, "sell a vehicle", "WObbbbF", 0, 0, false },
    { 0x7F3EF0, 0x3DED90, "sell vehicles of a building", "WObbbbF", 0, 0, false },
    { 0x7F3FEE, 0x3DED90, "sell a listed vehicle", "WObbbbF", 0, 0, false },
    // Links and office assignments the building windows remove or make: lists of
    // the buildings (+f98/+fb0, the assigned building's +888), heap only.
    { 0x74499F, 0x1C7E00, "building window: remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x7449AA, 0x162B60, "building window: remove link record by index", "xOI", 0, 0, false },
    { 0x748D69, 0x1C7E00, "building window (second): remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x748D77, 0x162B60, "building window (second): remove link record by index", "xOI", 0, 0, false },
    { 0x74BD69, 0x162B60, "station window: remove link record by index", "xOI", 0, 0, false },
    { 0x76773A, 0x1C7E00, "link list window: remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x76774A, 0x162BF0, "link list window: remove the link record to a partner", "xOO", 0, 0, false },
    { 0x76785E, 0x162380, "link list window: unassign building from office", "WOO", 0, 0, false },
    { 0x76F93A, 0x162380, "office window: unassign building from office", "WOO", 0, 0, false },
    { 0x7707B5, 0x162380, "office list: unassign building from office", "WOO", 0, 0, false },
    { 0x7452E9, 0x162A50, "clear all links (confirmation)", "xO", 0, 0, false },
    { 0x7146EA, 0x161CB0, "assign construction office", "WOOFbb", 0, 0, false },
    { 0x71476F, 0x162380, "unassign construction office", "WOO", 0, 0, false },
    { 0x717EEA, 0x161CB0, "assign construction office (second window)", "WOOFbb", 0, 0, false },
    { 0x717F70, 0x162380, "unassign construction office (second window)", "WOO", 0, 0, false },
    // The depot window composes trains by drag and drop: 3fdef0(world, vehicle) takes the dragged
    // vehicle off its head, 3fd5d0(world, head, vehicle, index, 1, 0, &result) couples it at the
    // index (-1 the end; the result only picks the window's message), 3fe520(-, head) moves the
    // dragged vehicle and its wagons onto the head. With the vehicle window's replace or line copy
    // tool selected, its rows act like the line window's.
    { 0x7C7BA8, 0x3FDE30, "depot window: uncouple the dragged vehicle (train drop)", "WV", 0, 0, false },
    { 0x7C7BB5, 0x3FE460, "depot window: attach the dragged train to the vehicle", "VV", 0, 0, false, CALL_DRAG },
    { 0x7C8027, 0x3FDE30, "depot window: uncouple the dragged vehicle (row drop)", "WV", 0, 0, false, CALL_DEPOT_ROW_UNCOUPLE },
    { 0x7C8078, 0x3FD510, "depot window: couple the dragged vehicle at the row", "WVVIbbx", 0, 0, false, CALL_DEPOT_ROW_COUPLE },
    { 0x7C8E50, 0x3FDE30, "depot window: uncouple the dragged vehicle (end drop)", "WV", 0, 0, false },
    { 0x7C8E87, 0x3FD510, "depot window: couple the dragged vehicle at the end", "WVVIbbx", 0, 0, false },
    { 0x7C8394, 0x1AB290, "depot window: replace tool (prepare the replaced vehicle)", "WV", 0, 0, false },
    { 0x7C83A5, 0x1AAAC0, "depot window: replace tool (replace by the row's vehicle)", "WoOb", 0, 0, false },
    { 0x7C85F8, 0x497AA0, "depot window: line copy tool (put the row's vehicle on the line)", "MLVbo", 0x12800, 0, false },
    { 0x7C860C, 0x3BB270, "depot window: line copy tool (refresh the vehicle)", "WO", 0, 0, false },
    { 0x801A70, 0x5290, "rename window: write the name", "xxx", 0, 0, false, CALL_RENAME },
    { 0x6FEFBE, 0x3FFA10, "building window: invite Third World immigrants (USD)", "WxxB", 0, 0x1CE, false, CALL_INVITE_CITIZENS },
    { 0x6FF4BA, 0x3FFA10, "building window: invite Soviet immigrants (tutorial)", "WxxB", 1, 0x187, false, CALL_INVITE_CITIZENS },
    { 0x6FF6CD, 0x3FFA10, "building window: invite Soviet immigrants (RUB)", "WxxB", 2, 0x1A2, false, CALL_INVITE_CITIZENS },
    { 0x6FFBEE, 0x3FFA10, "building window: invite educated Soviet experts (RUB)", "WxxB", 3, 0x1FA, false, CALL_INVITE_CITIZENS },
    { 0x1DAA52, 0x161CB0, "demolition tool: assign office to network building", "WOOFbb", 0, 0, false },
    { 0x1DAA69, 0x1A0510, "demolition tool: set priority on network building", "WOb", 0, 0, false },
    { 0x1DAFFD, 0x161CB0, "demolition tool: assign office to building", "WOOFbb", 0, 0, false },
    { 0x1DB021, 0x1A0510, "demolition tool: set priority on building", "WOb", 0, 0, false },
    { 0x770464, 0x1A0510, "office window: toggle building priority", "WOb", 0, 0, false },
    { 0x77DD57, 0x1A0510, "building window: toggle construction/demolition priority", "WOb", 0, 0, false },
    { 0x7D9A87, RVA_VEH_ADD_STOP, "vehicle window: add Soviet border stop", "VMIx", WORLD_SOVIET_BORDER_BLD_OFF, 0x1F4, false, CALL_ADD_BORDER_STOP },
    { 0x7D9D7F, RVA_VEH_ADD_STOP, "vehicle window: add Western border stop", "VMIx", WORLD_WESTERN_BORDER_BLD_OFF, 0x1FC, false, CALL_ADD_BORDER_STOP },
    { 0x7DA0CE, RVA_VEH_DEL_STOP, "vehicle window: delete stop", "VIx", 0, 0x65, false, CALL_DELETE_STOP },
    { 0x7DA228, RVA_HELI_REROUTE, "vehicle window: change current stop", "VI", 0, 0, false, CALL_CHANGE_STOP },
    { 0x7DA919, RVA_VEH_INVERT_STOPS, "vehicle window: invert stop order", "V", 0, 0, false },
    { SITE_BLD_MANUAL_BUY_RES_RVA, BLD_MANUAL_BUY_RES_RVA, "building window: manual buy resources", "WBI", 0, 0, false, CALL_MANUAL_BUY_RES },
    { SITE_COPY_OFFICE_ASSIGN_NET_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "copy office tool: assign office to network building", "WOOFbb", 0, 0, false },
    { SITE_COPY_OFFICE_ASSIGN_BLD_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "copy office tool: assign office to building", "WOOFbb", 0, 0, false },
    { SITE_CONSTRUCT_TOOL_START_PLAN_RVA, RVA_START_PLANNED_CONSTRUCTION, "construction tool: start planned network construction", "WObx", 0, 0, false },
    { SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "construction tool: assign office to network building", "WOOFbb", 0, 0, false },
    { SITE_CONSTRUCT_TOOL_ASSIGN_BLD_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "construction tool: assign office to building", "WOOFbb", 0, 0, false },
    { SITE_COPY_DIST_OFFICE_RVA, FN_COPY_DIST_OFFICE_RVA, "copy tool: copy distribution office settings", "WBB", 0, 0, false },
    { SITE_COPY_WASTE_SETTINGS_RVA, FN_COPY_WASTE_SETTINGS_RVA, "copy tool: copy technical services waste settings", "WBB", 0, 0, false },
    { SITE_COPY_TRANSFER_SETTINGS_RVA, FN_COPY_TRANSFER_SETTINGS_RVA, "copy tool: copy factory connection settings", "WBB", 0, 0, false },
};
#define SITE_COUNT (sizeof(kSites) / sizeof(kSites[0]))

enum { OBJ_NONE = 0, OBJ_VEHICLE = 1, OBJ_BUILDING = 2, OBJ_LINE = 3 };

#pragma pack(push, 1)
struct GameCallMsg {
    unsigned index;
    unsigned target;              // the site's routine, a check
    unsigned char kinds[MAX_ARGS]; // OBJ_* of an object argument
    RouteRef refs[MAX_ARGS];
    unsigned values[MAX_ARGS];    // b / I arguments
};
#pragma pack(pop)

static bool g_siteOn[SITE_COUNT];

void GameCalls_Bind(const TsmHost* host)
{
    TsmBind(host);
}
static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

typedef U64 (*CallFn)(U64, U64, U64, U64, U64, U64, U64);
typedef U64 (*CallF4Fn)(U64, U64, U64, float, U64, U64, U64);

// A site whose fourth argument is a float (kept as its bits in a[3]).
static bool Float4(const GameCallSite& s)
{
    return s.schema[0] && s.schema[1] && s.schema[2] && s.schema[3] == 'F';
}

static float AsFloat(U64 bits)
{
    unsigned b = (unsigned)bits;
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static U64 CallSite(const GameCallSite& s, const U64* a)
{
    if (Float4(s))
        return ((CallF4Fn)(g_exeBase + s.target))(a[0], a[1], a[2], AsFloat(a[3]), a[4], a[5], a[6]);
    return ((CallFn)(g_exeBase + s.target))(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
}

static void LinkSpawnedChildToParent(U64 world)
{
    U64 wBeg = Q(world + WORLD_CITIZENS_OFFSET), wEnd = Q(world + WORLD_CITIZENS_OFFSET + 8);
    if (!wBeg || wEnd < wBeg + 16) return;
    U64 count = (wEnd - wBeg) >> 3;
    U64 child  = Q(wBeg + (count - 1) * 8);
    U64 parent = Q(wBeg + (count - 2) * 8);
    if (child && parent) {
        *(U64*)(child + CIT_PARENT_PTR_OFF) = parent;
        *(U64*)(parent + CIT_CHILD_PTR_OFF) = child;
    }
}

static void InviteCitizensNow(U64 world, U64 building, unsigned mode)
{
    if (!world || !building || mode > 3) return;
    typedef U64 (*SpawnCitizenFn)(U64 world, U64 bld, unsigned char isChild, unsigned char isSoviet, unsigned char isThirdWorld, unsigned char unusedZero);
    typedef int (*RandFn)();
    typedef float (*RandFloatFn)(float minVal, float maxVal);

    const BYTE* spawnSite = g_exeBase + RVA_SPAWN_CITIZEN;
    SpawnCitizenFn spawnCit = (spawnSite[0] == 0x48) ? (SpawnCitizenFn)spawnSite : *(const SpawnCitizenFn*)spawnSite;
    if (!spawnCit) return;
    RandFn randFn = *(const RandFn*)(g_exeBase + RVA_IAT_RAND);
    RandFloatFn randFloatFn = *(const RandFloatFn*)(g_exeBase + RVA_IAT_RAND_FLOAT);

    U64 cBeg = Q(building + BLD_CITIZENS_BEGIN_OFF), cEnd = Q(building + BLD_CITIZENS_END_OFF);
    int currentIndependent = 0;
    for (U64 p = cBeg; p && p < cEnd; p += 8) {
        U64 cit = Q(p);
        if (cit && Q(cit + CIT_PARENT_PTR_OFF) == 0) currentIndependent++;
    }
    U64 bldType = Q(building + BLD_TYPE_OFFSET);
    U64 resSpec = bldType ? Q(bldType + BLD_TYPE_RES_SPEC_PTR_OFF) : 0;
    if (!resSpec) return;
    int maxFlats = (int)(*(const float*)(resSpec + RES_SPEC_MAX_FLATS_FLOAT_OFF));
    if (currentIndependent >= maxFlats) return;

    U64 bldArea  = Q(building + BLD_AREA_PTR_OFF);
    U64 cityStat = bldArea ? Q(bldArea + AREA_CITY_STAT_PTR_OFF) : 0;

    const bool isUsd = (mode == 0);
    const bool isExpert = (mode == 3);
    const int maxBatch = isExpert ? 5 : 10;
    const int childMod = isUsd ? 3 : 2;
    const unsigned char isSoviet = isUsd ? 0 : 1;
    const unsigned char isThirdWorld = isUsd ? 1 : 0;

    float kExpertMult = *(const float*)(g_exeBase + RVA_INVITE_EXPERT_MULT_FLOAT);
    if (kExpertMult <= 0.0f) kExpertMult = 2.5f;
    float kEduMin = *(const float*)(g_exeBase + RVA_INVITE_EXPERT_EDU_MIN_FLOAT);
    if (kEduMin <= 0.0f) kEduMin = 2.05f;
    float kEduMax = *(const float*)(g_exeBase + RVA_INVITE_EXPERT_EDU_MAX_FLOAT);
    if (kEduMax <= 0.0f) kEduMax = 2.30f;

    double* treasury = (double*)(world + (isUsd ? WORLD_USD_OFFSET : WORLD_RUB_OFFSET));
    const unsigned spendOff = STAT_IMMIGRANT_SPEND_OFF + (isUsd ? 0u : 4u);
    const unsigned countOff = STAT_CITIZEN_COUNTS_OFF + (isUsd ? 16u : 12u);

    for (int i = 0; i < maxBatch; i++) {
        float rateMult = 1.0f;
        if (mode == 0)      rateMult = *(const float*)(world + WORLD_IMMIGRANT_RATE_MULT_USD_OFF);
        else if (mode == 2) rateMult = *(const float*)(world + WORLD_IMMIGRANT_RATE_MULT_RUB_OFF);
        else if (mode == 3) rateMult = *(const float*)(world + WORLD_IMMIGRANT_RATE_MULT_RUB_OFF) * kExpertMult;

        float basePrice = *(const float*)(world + (isUsd ? WORLD_PRICE_IMMIGRANT_USD_OFF : WORLD_PRICE_IMMIGRANT_RUB_OFF));
        double price = (double)(basePrice * rateMult);
        if (*treasury <= price || currentIndependent >= maxFlats) break;

        *treasury -= price;
        spawnCit(world, building, 0, isSoviet, isThirdWorld, 0);
        if (isExpert) {
            float edu = randFloatFn ? randFloatFn(kEduMin, kEduMax) : kEduMin;
            U64 wBeg = Q(world + WORLD_CITIZENS_OFFSET), wEnd = Q(world + WORLD_CITIZENS_OFFSET + 8);
            if (wBeg && wEnd >= wBeg + 8) {
                U64 lastCit = Q(wEnd - 8);
                if (lastCit) *(float*)(lastCit + CIT_EDUCATION_FLOAT_OFF) = edu;
            }
        }
        float baseSpend = *(const float*)(world + (isUsd ? WORLD_IMMIGRANT_BASE_SPEND_USD_OFF : WORLD_IMMIGRANT_BASE_SPEND_RUB_OFF));
        float statSpend = baseSpend * rateMult;
        *(int*)(world + WORLD_STAT_CURRENT_OFFSET + countOff) += 1;
        *(float*)(world + WORLD_STAT_CURRENT_OFFSET + spendOff) += statSpend;
        if (cityStat) {
            *(int*)(cityStat + countOff) += 1;
            *(float*)(cityStat + spendOff) += statSpend;
        }
        currentIndependent++;
        if (randFn && (randFn() % childMod) == 0) {
            spawnCit(world, building, 1, isSoviet, isThirdWorld, 0);
            LinkSpawnedChildToParent(world);
        }
    }
}

void GameCalls_InviteCitizensForTest(unsigned long long world, unsigned long long building, unsigned mode)
{
    InviteCitizensNow(world, building, mode);
}

static bool HasPassengersInCarriedSummary(U64 vehicle)
{
    U64 cBeg = Q(vehicle + 0x458), cEnd = Q(vehicle + 0x460);
    for (U64 p = cBeg; p && p + 16 <= cEnd; p += 16) {
        const char* resName = *(const char**)p;
        if (resName && strcmp(resName, "passengers") == 0) return true;
    }
    return false;
}

static void AddBorderStopNow(U64 world, U64 vehicle, U64 borderBld, int insertIdx)
{
    if (!world || !vehicle || !borderBld) return;
    typedef unsigned char (*AddStopFn)(U64 veh, U64 bld, unsigned idx, unsigned char flag);
    typedef void (*RebuildCargoSumFn)(U64 veh, unsigned char a, unsigned char b, unsigned char c);
    typedef void (*LinesNotifyVehFn)(U64 mgr, U64 veh);

    const BYTE* addStopCode = g_exeBase + RVA_VEH_ADD_STOP;
    if (addStopCode[0] != 0) {
        if (!(((AddStopFn)addStopCode)(vehicle, borderBld, (unsigned)insertIdx, 0) & 0xFF)) return;
    }
    const BYTE* rebuildCode = g_exeBase + RVA_VEH_REBUILD_CARGO_SUM;
    if (rebuildCode[0] != 0) {
        ((RebuildCargoSumFn)rebuildCode)(vehicle, 1, 0, 0);
    }

    unsigned char hasPass = HasPassengersInCarriedSummary(vehicle) ? 1 : 0;
    U64 sBeg = Q(vehicle + VEH_STOPS_LIST_OFFSET), sEnd = Q(vehicle + VEH_STOPS_LIST_OFFSET + 8);
    long long nStops = (sBeg && sEnd >= sBeg) ? (long long)((sEnd - sBeg) / VEH_STOP_RECORD_STRIDE) : 0;
    if (nStops > 0) {
        U64 vType = Q(vehicle + VEH_TYPE_PTR_OFFSET);
        int cat = vType ? *(const int*)(vType + VEH_TYPE_CATEGORY_OFFSET) : 0;
        if (*(const int*)(vehicle + 0x1794) == 1 && vType != 0 && nStops == 1 &&
            (cat == 6 || cat == 8 || cat == 10)) {
            U64 stopRec = sBeg + (U64)(nStops - 1) * VEH_STOP_RECORD_STRIDE;
            *(BYTE*)(stopRec + 0x00) = 0;
            *(BYTE*)(stopRec + VEH_STOP_HALF_STRIDE) = 0;
        } else {
            int targetStop = (insertIdx == -1) ? (int)(nStops - 1) : (insertIdx + 1);
            if (targetStop >= 0 && targetStop < (int)nStops) {
                U64 stopRec = sBeg + (U64)targetStop * VEH_STOP_RECORD_STRIDE;
                *(BYTE*)(stopRec + 0x00) = hasPass;
                *(BYTE*)(stopRec + VEH_STOP_HALF_STRIDE) = 1;
            }
        }
    }

    U64 linesMgr = Q(world + LINES_OFFSET);
    const BYTE* notifyCode = g_exeBase + RVA_LINES_NOTIFY_VEH;
    if (linesMgr && notifyCode[0] != 0) {
        ((LinesNotifyVehFn)notifyCode)(linesMgr, vehicle);
    }
}

static void DeleteStopNow(U64 world, U64 vehicle, int stopIdx)
{
    if (!world || !vehicle || stopIdx < 0) return;
    U64 sBeg = Q(vehicle + VEH_STOP_BLD_PTR_VEC_OFF), sEnd = Q(vehicle + VEH_STOP_BLD_PTR_VEC_OFF + 8);
    if (!sBeg || sEnd <= sBeg || (U64)stopIdx >= (sEnd - sBeg) / 8) return;
    U64 stopBld = Q(sBeg + (U64)stopIdx * 8);

    typedef unsigned char (*DelStopFn)(U64 veh, int idx);
    const BYTE* delCode = g_exeBase + RVA_VEH_DEL_STOP;
    if (delCode[0] != 0) {
        if (!(((DelStopFn)delCode)(vehicle, stopIdx) & 0xFF)) return;
    }

    U64 linesMgr = Q(world + LINES_OFFSET);
    const BYTE* notifyCode = g_exeBase + RVA_LINES_NOTIFY_VEH;
    if (linesMgr && notifyCode[0] != 0) {
        typedef void (*LinesNotifyVehFn)(U64 mgr, U64 veh);
        ((LinesNotifyVehFn)notifyCode)(linesMgr, vehicle);
    }
    *(BYTE*)(vehicle + 0xD3A) = 1;
    if (Q(vehicle + VEH_STOPPED_BLD_OFFSET) == stopBld && *(const BYTE*)(vehicle + VEH_CUSTOM_LINE_FLAG_OFF) == 0) {
        *(U64*)(vehicle + VEH_STOPPED_BLD_OFFSET) = 0;
    }
}

static void ChangeCurrentStopNow(U64 vehicle, int stopIdx)
{
    if (!vehicle || stopIdx < 0) return;
    U64 sBeg = Q(vehicle + VEH_STOP_BLD_PTR_VEC_OFF), sEnd = Q(vehicle + VEH_STOP_BLD_PTR_VEC_OFF + 8);
    if (!sBeg || sEnd <= sBeg || (U64)stopIdx >= (sEnd - sBeg) / 8) return;
    U64 vType = Q(vehicle + VEH_TYPE_PTR_OFFSET);
    int cat = vType ? *(const int*)(vType + VEH_TYPE_CATEGORY_OFFSET) : 0;
    bool inDepot = (Q(vehicle + 0x528) == 0 && Q(vehicle + VEH_STOPPED_BLD_OFFSET) == 0 &&
                    (cat != 10 || Q(vehicle + 0xC80) == 0));
    if (inDepot) {
        *(int*)(vehicle + VEH_SCHED_IDX_OFFSET) = stopIdx;
        *(BYTE*)(vehicle + 0xD3A) = 1;
        *(float*)(vehicle + 0x66C) = -1.0f;
        *(BYTE*)(vehicle + 0x668) = 1;
        if (cat == 4) *(BYTE*)(vehicle + 0xD3B) = 1;
    } else {
        *(U64*)(vehicle + VEH_TARGET_STOP_BLD_OFF) = Q(sBeg + (U64)stopIdx * 8);
        *(float*)(vehicle + 0x66C) = -1.0f;
        *(BYTE*)(vehicle + 0x668) = 1;
        const BYTE* heliCode = g_exeBase + RVA_HELI_REROUTE;
        if (cat == 10 && *(const int*)(vehicle + VEH_HELI_FLIGHT_STATE_OFF) == 3 && heliCode[0] != 0) {
            typedef void (*HeliRerouteFn)(U64 veh);
            ((HeliRerouteFn)heliCode)(vehicle);
        }
    }
}

static void ReleaseVehicleNow(U64 vehicle)
{
    if (!vehicle) return;
    U64 vType = Q(vehicle + VEH_TYPE_PTR_OFFSET);
    int cat = vType ? *(const int*)(vType + VEH_TYPE_CATEGORY_OFFSET) : 0;
    if (cat == 7) {
        *(U64*)(vehicle + VEH_CUR_BLD_OFFSET) = 0;
        return;
    }
    if (cat == 6) *(BYTE*)(vehicle + VEH_CABLEWAY_START_FLAG_OFF) = 1;
    const BYTE* relCode = g_exeBase + RVA_VEH_RELEASE;
    if (relCode[0] != 0) {
        typedef void (*ReleaseVehFn)(U64 veh);
        ((ReleaseVehFn)relCode)(vehicle);
    }
}

void GameCalls_AddBorderStopForTest(unsigned long long world, unsigned long long vehicle,
                                    unsigned long long borderBld, int insertIdx)
{
    AddBorderStopNow(world, vehicle, borderBld, insertIdx);
}

void GameCalls_DeleteStopForTest(unsigned long long world, unsigned long long vehicle, int stopIdx)
{
    DeleteStopNow(world, vehicle, stopIdx);
}

void GameCalls_ChangeCurrentStopForTest(unsigned long long vehicle, int stopIdx)
{
    ChangeCurrentStopNow(vehicle, stopIdx);
}

void GameCalls_ReleaseVehicleForTest(unsigned long long vehicle)
{
    ReleaseVehicleNow(vehicle);
}

static bool ObjectRef(U64 object, unsigned char& kind, RouteRef& r)
{
    if (Ref_Vehicle(object, r)) kind = OBJ_VEHICLE;
    else if (Ref_Building(object, r)) kind = OBJ_BUILDING;
    else if (Ref_Line(object, r)) kind = OBJ_LINE;
    else return false;
    return true;
}

static U64 ObjectAt(unsigned char kind, const RouteRef& r)
{
    switch (kind) {
    case OBJ_VEHICLE: return Ref_VehicleAt(r);
    case OBJ_BUILDING: return Ref_BuildingAt(r);
    case OBJ_LINE: return Ref_LineAt(r);
    }
    return 0;
}

// The arguments as identities; nullptr, or why they cannot travel.
static const char* Capture(unsigned index, const U64* a, GameCallMsg& m)
{
    const GameCallSite& s = kSites[index];
    U64 world = (U64)World();
    memset(&m, 0, sizeof(m));
    m.index = index;
    m.target = s.target;
    for (int j = 0; j < MAX_ARGS; j++) m.refs[j] = kNoRef;
    for (int j = 0; s.schema[j]; j++) {
        switch (s.schema[j]) {
        case 'x': break;
        case 'W': if (a[j] != world) return "an argument is not the world"; break;
        case 'M': if (a[j] != Q(world + s.worldPointer)) return "an argument is not the world's object"; break;
        case 'V':
            if (!Ref_Vehicle(a[j], m.refs[j])) return "a vehicle is not in the world's list";
            m.kinds[j] = OBJ_VEHICLE;
            break;
        case 'B':
            if (!Ref_Building(a[j], m.refs[j])) return "a building is not in the world's list";
            m.kinds[j] = OBJ_BUILDING;
            break;
        case 'L':
            if (!Ref_Line(a[j], m.refs[j])) return "a line is not in the lines manager";
            m.kinds[j] = OBJ_LINE;
            break;
        case 'o':
            if (!a[j]) break;
            // fall through
        case 'O':
            if (!ObjectRef(a[j], m.kinds[j], m.refs[j])) return "an object is not in the world's lists";
            break;
        case 'b': m.values[j] = (unsigned)(a[j] & 0xff); break;
        case 'I': case 'F': m.values[j] = (unsigned)a[j]; break;
        default: return "unknown argument kind";
        }
    }
    return nullptr;
}

static const char* Rebuild(const GameCallMsg& m, U64* a)
{
    const GameCallSite& s = kSites[m.index];
    U64 world = (U64)World();
    for (int j = 0; j < MAX_ARGS; j++) a[j] = 0;
    for (int j = 0; s.schema[j]; j++) {
        switch (s.schema[j]) {
        case 'W': a[j] = world; break;
        case 'M': a[j] = Q(world + s.worldPointer); break;
        case 'V': case 'B': case 'L': case 'O': case 'o':
            if (m.kinds[j] == OBJ_NONE) {
                if (s.schema[j] != 'o') return "an object is missing";
                break;
            }
            if (!(a[j] = ObjectAt(m.kinds[j], m.refs[j]))) return "an object is gone";
            break;
        case 'b': a[j] = m.values[j] & 0xff; break;
        case 'I': case 'F': a[j] = m.values[j]; break;
        }
    }
    return nullptr;
}

static bool g_skipDepotRowCouple = false;

bool GameCalls_IsDepotRowSelfDrop(unsigned long long dragged, unsigned long long hoveredResolved,
                                  unsigned long long hoveredRaw, unsigned long long rowHead)
{
    if (!dragged) return false;
    if (dragged == hoveredResolved || dragged == hoveredRaw || dragged == rowHead) return true;
    __try {
        if (hoveredResolved && *(U64*)(hoveredResolved + VEH_PARENT_PTR_OFFSET) == dragged) return true;
        if (hoveredRaw && *(U64*)(hoveredRaw + VEH_PARENT_PTR_OFFSET) == dragged) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

static bool IsReturnSkipSite(int special)
{
    return special == CALL_INVITE_CITIZENS || special == CALL_ADD_BORDER_STOP || special == CALL_DELETE_STOP;
}

static bool IsCommandSpecialSite(int special)
{
    return special == CALL_PLAIN || special == CALL_DELETE_LINE || special == CALL_DRAG ||
           special == CALL_INVITE_CITIZENS || special == CALL_DEPOT_ROW_UNCOUPLE ||
           special == CALL_DEPOT_ROW_COUPLE || special == CALL_ADD_BORDER_STOP ||
           special == CALL_DELETE_STOP || special == CALL_CHANGE_STOP || special == CALL_RELEASE_VEHICLE;
}

static void RestoreMechToBuildingInventory(U64 mech, U64 carrier)
{
    if (!mech || !carrier) return;
    *(U64*)(mech + VEH_LINK_PTR_OFFSET) = 0;
    U64 wpBld = *(U64*)(carrier + VEH_WORKPLACE_BLD_OFFSET);
    U64 depBld = *(U64*)(carrier + VEH_DEPOT_PTR_OFFSET);
    U64 homeBld = wpBld ? wpBld : depBld;
    if (!homeBld) return;
    if (*(U64*)(mech + VEH_WORKPLACE_BLD_OFFSET) == 0 && *(U64*)(mech + VEH_DEPOT_PTR_OFFSET) == 0) {
        if (wpBld) *(U64*)(mech + VEH_WORKPLACE_BLD_OFFSET) = wpBld;
        else *(U64*)(mech + VEH_DEPOT_PTR_OFFSET) = depBld;
    }
    U64 b = *(U64*)(homeBld + BLD_VEHICLES_BEGIN_OFFSET);
    U64 e = *(U64*)(homeBld + BLD_VEHICLES_END_OFFSET);
    U64 cap = *(U64*)(homeBld + BLD_VEHICLES_END_OFFSET + 8);
    for (U64 p = b; p && p + 8 <= e; p += 8) {
        if (*(U64*)p == mech) return;
    }
    if (b && e >= b && e + 8 <= cap) {
        *(U64*)e = mech;
        *(U64*)(homeBld + BLD_VEHICLES_END_OFFSET) = e + 8;
    } else if (g_exeBase && g_exeBase[RVA_VEC_PTR_PUSH_BACK] != 0) {
        ((void (*)(U64, const U64*))(g_exeBase + RVA_VEC_PTR_PUSH_BACK))(homeBld + BLD_VEHICLES_BEGIN_OFFSET, &mech);
    }
}

static void DetachSingleMechFromCarrier(U64 carrier, U64 mech)
{
    if (!carrier || !mech) return;
    U64 b = *(U64*)(carrier + VEH_CARRIED_BEGIN_OFFSET);
    U64 e = *(U64*)(carrier + VEH_CARRIED_END_OFFSET);
    if (b && e > b && (e - b) <= 64 * 8) {
        U64 writePtr = b;
        for (U64 p = b; p + 8 <= e; p += 8) {
            U64 cur = *(U64*)p;
            if (cur != mech) {
                *(U64*)writePtr = cur;
                writePtr += 8;
            }
        }
        *(U64*)(carrier + VEH_CARRIED_END_OFFSET) = writePtr;
    }
    RestoreMechToBuildingInventory(mech, carrier);
}

static void RecoverOrphanedCarrierMechanisms(U64 veh)
{
    if (!veh) return;
    __try {
        U64 carrier = *(U64*)(veh + VEH_LINK_PTR_OFFSET);
        if (carrier) {
            DetachSingleMechFromCarrier(carrier, veh);
        }
        U64 b = *(U64*)(veh + VEH_CARRIED_BEGIN_OFFSET);
        U64 e = *(U64*)(veh + VEH_CARRIED_END_OFFSET);
        if (b && e > b && (e - b) <= 64 * 8) {
            U64 count = (e - b) / 8;
            U64 mechs[64] = {};
            for (U64 i = 0; i < count; i++) mechs[i] = *(U64*)(b + i * 8);
            *(U64*)(veh + VEH_CARRIED_END_OFFSET) = b;
            for (U64 i = 0; i < count; i++) {
                if (mechs[i]) RestoreMechToBuildingInventory(mechs[i], veh);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void GameCalls_RecoverOrphanedCarrierMechanismsForTest(unsigned long long vehicle)
{
    RecoverOrphanedCarrierMechanisms(vehicle);
}

static U64 ExtractCarrierRecoveryVehicle(const GameCallSite& s, const U64* a)
{
    if (s.target == RVA_VEH_RELEASE || s.target == 0x6B7810) return a[0];
    if (s.target == SEND_TO_DEPOT || s.target == 0x3FB820) return a[1];
    if (s.target == 0x497AA0 && s.schema[2] == 'V') return a[2];
    return 0;
}

static void ExecuteSpecialSiteNow(const GameCallSite& s, const U64* a, U64& r, U64*& drag, U64& dragged)
{
    U64 recoveryVeh = ExtractCarrierRecoveryVehicle(s, a);
    if (recoveryVeh) RecoverOrphanedCarrierMechanisms(recoveryVeh);
    switch (s.special) {
    case CALL_INVITE_CITIZENS:
        InviteCitizensNow(a[0], a[3], s.worldPointer);
        return;
    case CALL_ADD_BORDER_STOP:
        AddBorderStopNow((U64)World(), a[0], a[1], (int)a[2]);
        return;
    case CALL_DELETE_STOP:
        DeleteStopNow((U64)World(), a[0], (int)a[1]);
        return;
    case CALL_CHANGE_STOP:
        ChangeCurrentStopNow(a[0], (int)a[1]);
        return;
    case CALL_RELEASE_VEHICLE:
        ReleaseVehicleNow(a[0]);
        return;
    default:
        break;
    }
    if (s.special == CALL_DELETE_LINE) *(BYTE*)(a[1] + 1) = 0;
    if (s.special == CALL_DRAG) {
        drag = (U64*)(g_exeBase + DRAG_RVA);
        dragged = *drag;
        *drag = a[0];
    }
    r = CallSite(s, a);
    if (drag) *drag = dragged;
    if (s.special == CALL_DELETE_LINE) {
        typedef void (*CleanupFn)(U64, U64);
        ((CleanupFn)(g_exeBase + DELETE_CLEANUP))(Q((U64)World() + LINES_OFFSET), 1);
    }
    if (s.orDepot && !(r & 0xff) && a[1]) {
        typedef void (*DepotFn)(U64, U64, U64, U64, U64, U64);
        ((DepotFn)(g_exeBase + SEND_TO_DEPOT))((U64)World(), a[1], 0, 0, 1, 1);
    }
}

static bool CheckTwoStatesOfficeCallAllowed(unsigned targetRva, const BYTE* officeBld,
                                            const BYTE* targetBld, unsigned char isDemolishOffice)
{
    if (!TwoStates_IsActive()) return true;
    if (targetRva == RVA_ASSIGN_CONSTRUCTION_OFFICE) {
        if (!officeBld || !TwoStates_CheckBuildingControlAllowed(officeBld,
                Tr("назначение чужой стройконторы", "assigning a foreign construction office"))) {
            return false;
        }
        if (isDemolishOffice != 0 && targetBld &&
            !TwoStates_CheckBuildingControlAllowed(targetBld,
                Tr("снос чужого здания", "demolishing a foreign building"))) {
            return false;
        }
        return true;
    }
    if (targetRva == RVA_UNASSIGN_CONSTRUCTION_OFFICE) {
        int localSt = TwoStates_LocalState();
        if ((officeBld && TwoStates_GetBuildingOwnerSlot(officeBld) == localSt) ||
            (targetBld && TwoStates_GetBuildingOwnerSlot(targetBld) == localSt)) {
            return true;
        }
        return TwoStates_CheckBuildingControlAllowed(officeBld ? officeBld : targetBld,
            Tr("управление чужим зданием", "controlling a foreign building"));
    }
    if (targetRva == RVA_START_PLANNED_CONSTRUCTION) {
        const BYTE* checkBld = officeBld ? officeBld : targetBld;
        return checkBld && TwoStates_CheckBuildingControlAllowed(checkBld,
            Tr("назначение чужой стройконторы", "assigning a foreign construction office"));
    }
    return false;
}

bool GameCalls_CheckOfficeCallAllowedForTest(unsigned targetRva, const BYTE* officeBld,
                                             const BYTE* targetBld, unsigned char isDemolishOffice)
{
    return CheckTwoStatesOfficeCallAllowed(targetRva, officeBld, targetBld, isDemolishOffice);
}

static bool CheckTwoStatesArgsAllowed(const GameCallMsg& m, const U64* args)
{
    if (!TwoStates_IsActive()) return true;
    if (m.target == RVA_ASSIGN_CONSTRUCTION_OFFICE || m.target == RVA_UNASSIGN_CONSTRUCTION_OFFICE) {
        return CheckTwoStatesOfficeCallAllowed(m.target, (const BYTE*)args[1], (const BYTE*)args[2],
                                               (unsigned char)(m.values[5] & 0xFF));
    }
    if (m.target == RVA_START_PLANNED_CONSTRUCTION) {
        const BYTE* activeOffice = nullptr;
        SafeReadPod((const BYTE* const*)(World() + WORLD_ACTIVE_WINDOW_BLD_OFF), &activeOffice);
        return CheckTwoStatesOfficeCallAllowed(m.target, activeOffice, (const BYTE*)args[1], 0);
    }
    for (int j = 0; j < MAX_ARGS; j++) {
        if (m.kinds[j] == OBJ_VEHICLE && args[j]) {
            if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)args[j],
                Tr("управление/продажа чужого транспорта", "controlling/selling a foreign vehicle")))
                return false;
        } else if (m.kinds[j] == OBJ_BUILDING && args[j]) {
            const BYTE* b = (const BYTE*)args[j];
            if (!TwoStates_CheckBuildingControlAllowed(*(const int*)b, *(const float*)(b + 0x350), *(const float*)(b + 0x358),
                Tr("управление чужим зданием", "controlling a foreign building")))
                return false;
        }
    }
    return true;
}

static void PrepareUiSpecialCapture(const GameCallSite& s, const U64* a)
{
    __try {
        if (s.special == CALL_DELETE_LINE && a[1]) {
            *(BYTE*)(a[1] + 1) = 1;
        } else if (s.special == CALL_ADD_BORDER_STOP && a[0] && a[3]) {
            if (*(const BYTE*)(a[0] + VEH_CUSTOM_LINE_FLAG_OFF) == 0)
                *(int*)(a[3] + 0x264) = -1;
        } else if (s.special == CALL_RELEASE_VEHICLE && a[0]) {
            U64 vType = Q(a[0] + VEH_TYPE_PTR_OFFSET);
            if (vType && *(const int*)(vType + VEH_TYPE_CATEGORY_OFFSET) == 6)
                *(BYTE*)(a[0] + VEH_CABLEWAY_START_FLAG_OFF) = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static U64 HandleManualBuyCapture(const U64* a)
{
    if (TwoStates_IsActive() && a[1]) {
        const BYTE* b = (const BYTE*)a[1];
        int bldId = 0;
        float bx = 0.0f, bz = 0.0f;
        if (SafeReadPod((const int*)b, &bldId) &&
            SafeReadPod((const float*)(b + 0x350), &bx) &&
            SafeReadPod((const float*)(b + 0x358), &bz)) {
            if (!TwoStates_CheckBuildingControlAllowed(bldId, bx, bz,
                    Tr("закупка ресурсов в чужом здании", "purchasing resources in foreign building")))
                return 0;
        }
    }
    BuildingSwitches_CaptureManualBuy(a[0], a[1], (int)a[2]);
    return 0;
}

static void SkipRenamePostMutationIfAtCallSite(uintptr_t* retAddr)
{
    if (retAddr && g_exeBase && *retAddr == (uintptr_t)(g_exeBase + SITE_RENAME_AFTER_CALL_RVA)) {
        *retAddr = (uintptr_t)(g_exeBase + SITE_RENAME_SKIP_MUTATION_RVA);
    }
}

static U64 Run(unsigned index, const U64* a, uintptr_t* retAddr = nullptr)
{
    const GameCallSite& s = kSites[index];
    if (s.special != CALL_LOCAL &&
        (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded()))) {
        if (s.special == CALL_DELETE_LINE && a[1]) {
            __try { *(BYTE*)(a[1] + 1) = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (s.special == CALL_RENAME) {
            SkipRenamePostMutationIfAtCallSite(retAddr);
        }
        SaveSync_NotifyWaitingLobbyActionBlocked();
        return s.result;
    }
    if (s.special == CALL_DEPOT_ROW_UNCOUPLE && Commands_TakeUi()) {
        g_skipDepotRowCouple = GameCalls_IsDepotRowSelfDrop(a[1], a[2], a[3], 0);
        if (g_skipDepotRowCouple) return s.result;
    } else if (s.special == CALL_DEPOT_ROW_COUPLE && Commands_TakeUi()) {
        bool skip = g_skipDepotRowCouple || GameCalls_IsDepotRowSelfDrop(a[2], 0, 0, a[1]);
        g_skipDepotRowCouple = false;
        if (skip) return s.result;
    }
    if (s.special == CALL_INVITE_CITIZENS) {
        if (TwoStates_IsActive() && a[3]) {
            const BYTE* b = (const BYTE*)a[3];
            if (!TwoStates_CheckBuildingControlAllowed(*(const int*)b, *(const float*)(b + 0x350), *(const float*)(b + 0x358),
                                                       Tr("приглашение жителей в чужое здание", "inviting citizens to a foreign building")))
                return 0;
        }
        CallSite(s, a); // play click sound locally
        if (!Commands_TakeUi()) {
            InviteCitizensNow(a[0], a[3], s.worldPointer);
            return 0;
        }
    }
    if (!Commands_TakeUi()) {
        static int s_logged = 0;
        if ((s.special == CALL_JUNCTION_ACCEPT || s.special == CALL_GAME_RULES) && Commands_Capturing() && s_logged < 16) {
            ++s_logged;
            Logf("republicmp  GAME CALL: %s made on this PC only (while a command was being made)", s.name);
        }
        if (s.special == CALL_CHANGE_STOP && !SessionPatches_IsApplied()) {
            return CallSite(s, a);
        }
        if (s.special == CALL_ADD_BORDER_STOP || s.special == CALL_DELETE_STOP ||
            s.special == CALL_CHANGE_STOP || s.special == CALL_RELEASE_VEHICLE) {
            U64 r = 0, *drag = nullptr, dragged = 0;
            ExecuteSpecialSiteNow(s, a, r, drag, dragged);
            return r;
        }
        return CallSite(s, a);
    }
    switch (s.special) {
    case CALL_PRESENTATION:
        return s.result;
    case CALL_LOCAL:
        return CallSite(s, a);
    case CALL_RENAME:
        if (Rename_Capture(a[0], a[1], a[2])) {
            SkipRenamePostMutationIfAtCallSite(retAddr);
            return 0;
        }
        return CallSite(s, a);
    case CALL_ACCEPT_MEASURES:
        LineAccept_RestoreMeasures();
        return s.result;
    case CALL_LINE_ACCEPT:
        LineAccept_Capture(a[1], a[2]);
        return s.result;
    case CALL_JUNCTION_ACCEPT:
        JunctionAccept_Capture(a[0], a[1]);
        return s.result;
    case CALL_GAME_RULES:
        Commands_CaptureGameRules();
        return s.result;
    case CALL_MANUAL_BUY_RES:
        return HandleManualBuyCapture(a);
    default:
        PrepareUiSpecialCapture(s, a);
        break;
    }
    U64 args[MAX_ARGS];
    memcpy(args, a, sizeof(args));
    GameCallMsg m;
    const char* why = nullptr;
    bool refusedByTwoStates = false;
    __try {
        if (s.special == CALL_DRAG && !(args[0] = *(U64*)(g_exeBase + DRAG_RVA))) why = "no vehicle is dragged";
        if (!why) why = Capture(index, args, m);
        if (!why && !CheckTwoStatesArgsAllowed(m, args)) refusedByTwoStates = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the arguments";
    }
    if (refusedByTwoStates) return s.result;
    if (why)
        Logf("republicmp  GAME CALL: %s not shared - %s; nothing is done", s.name, why);
    else
        Commands_Submit(LS_CMD_GAME_CALL, &m, sizeof(m));
    return s.result;
}

template <unsigned I>
static U64 Thunk(U64 a1, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7)
{
    U64 a[MAX_ARGS] = { a1, a2, a3, a4, a5, a6, a7 };
    if (IsReturnSkipSite(kSites[I].special) &&
        SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning() && !IsDualTestbed()) {
        *(uintptr_t*)_AddressOfReturnAddress() -= kSites[I].result;
        if (kSites[I].special == CALL_ADD_BORDER_STOP) a[3] = 0;
        return CallSite(kSites[I], a);
    }
    return Run(I, a, (uintptr_t*)_AddressOfReturnAddress());
}

// The same for a site whose fourth argument is a float (xmm3): its bits go in a[3].
template <unsigned I>
static U64 ThunkF4(U64 a1, U64 a2, U64 a3, float a4, U64 a5, U64 a6, U64 a7)
{
    unsigned bits;
    memcpy(&bits, &a4, 4);
    U64 a[MAX_ARGS] = { a1, a2, a3, bits, a5, a6, a7 };
    return Run(I, a);
}

template <size_t... I>
static constexpr std::array<void*, sizeof...(I)> MakeThunks(std::index_sequence<I...>)
{
    return { { (void*)&Thunk<(unsigned)I>... } };
}

template <size_t... I>
static constexpr std::array<void*, sizeof...(I)> MakeThunksF4(std::index_sequence<I...>)
{
    return { { (void*)&ThunkF4<(unsigned)I>... } };
}

bool GameCalls_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(GameCallMsg)) return false;
    GameCallMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.index >= SITE_COUNT || kSites[m.index].target != m.target) {
        Logf("republicmp  GAME CALL: unknown site %u - not made", m.index);
        return false;
    }
    const GameCallSite& s = kSites[m.index];
    U64 a[MAX_ARGS];
    const char* why = nullptr;
    U64 r = 0;
    U64* drag = nullptr;
    U64 dragged = 0;
    __try {
        why = Rebuild(m, a);
        if (!why && !IsCommandSpecialSite(s.special))
            why = "not a command site";
        if (!why) {
            ExecuteSpecialSiteNow(s, a, r, drag, dragged);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the call faulted";
        if (drag) *drag = dragged;
    }
    Logf("republicmp  GAME CALL: %s: %s", s.name, why ? why : "made");
    return why == nullptr;
}

bool GameCalls_Install()
{
    static const auto thunks = MakeThunks(std::make_index_sequence<SITE_COUNT>());
    static const auto thunksF4 = MakeThunksF4(std::make_index_sequence<SITE_COUNT>());
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) {
        Logf("republicmp  GAME CALL: no relay page near the executable - window buttons are not shared");
        return false;
    }
    DWORD old;
    VirtualProtect(relay, SITE_COUNT * 32, PAGE_EXECUTE_READWRITE, &old);
    for (size_t i = 0; i < SITE_COUNT; i++) {
        BYTE* r = relay + i * 32;
        void* to = Float4(kSites[i]) ? thunksF4[i] : thunks[i];
        if (IsReturnSkipSite(kSites[i].special)) {
            // mov r9, r12 (4D 89 E1)
            // add qword ptr [rsp], skipImm32 (48 81 04 24 <imm32>)
            // jmp qword ptr [rip+0] (FF 25 00 00 00 00 <abs64>)
            r[0] = 0x4D; r[1] = 0x89; r[2] = 0xE1;
            r[3] = 0x48; r[4] = 0x81; r[5] = 0x04; r[6] = 0x24;
            unsigned skip = kSites[i].result;
            memcpy(r + 7, &skip, 4);
            r[11] = 0xFF; r[12] = 0x25; r[13] = r[14] = r[15] = r[16] = 0;
            memcpy(r + 17, &to, 8);
        } else if (kSites[i].special == CALL_DEPOT_ROW_UNCOUPLE) {
            // mov r8, r15 (4D 89 F8); mov r9, r14 (4D 89 F1); jmp qword ptr [rip+0]
            r[0] = 0x4D; r[1] = 0x89; r[2] = 0xF8;
            r[3] = 0x4D; r[4] = 0x89; r[5] = 0xF1;
            r[6] = 0xFF; r[7] = 0x25; r[8] = r[9] = r[10] = r[11] = 0;
            memcpy(r + 12, &to, 8);
        } else {
            r[0] = 0xff; r[1] = 0x25; r[2] = r[3] = r[4] = r[5] = 0;
            memcpy(r + 6, &to, 8);
        }
    }
    VirtualProtect(relay, SITE_COUNT * 32, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, SITE_COUNT * 32);
    int on = 0;
    for (size_t i = 0; i < SITE_COUNT; i++) {
        BYTE* at = g_exeBase + kSites[i].site;
        int rel = 0;
        memcpy(&rel, at + 1, 4);
        if (at[0] != 0xE8 || kSites[i].site + 5 + rel != kSites[i].target) {
            Logf("republicmp  GAME CALL: site 0x%X (%s) differs - left alone", kSites[i].site, kSites[i].name);
            continue;
        }
        long long to = (long long)(relay + i * 32) - (long long)(at + 5);
        if (to < INT_MIN || to > INT_MAX) continue;
        unsigned char call[5] = { 0xE8 };
        int rel32 = (int)to;
        memcpy(call + 1, &rel32, 4);
        DWORD prot;
        VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &prot);
        memcpy(at, call, 5);
        VirtualProtect(at, 5, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
        g_siteOn[i] = true;
        on++;
    }
    Logf("republicmp  GAME CALL: %d of %d window call sites relayed", on, (int)SITE_COUNT);
    return on == (int)SITE_COUNT;
}
