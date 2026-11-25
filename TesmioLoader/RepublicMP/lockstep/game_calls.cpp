// SPDX-License-Identifier: GPL-3.0-only
// See game_calls.h. The sites and their argument kinds follow SoylentGreed's
// game-call commands (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "game_calls.h"
#include "commands.h"
#include "purchase.h"
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
#include <math.h>
#include <string.h>
#include <utility>
#include "../determinism/determinism.h"
#include <intrin.h>
#pragma intrinsic(_AddressOfReturnAddress)

#define SEND_TO_DEPOT 0x41F0F0
#define LINES_OFFSET 0x128B8
#define DELETE_CLEANUP 0x4BC740
#define DRAG_RVA 0x9E0BA8        // the depot window's dragged vehicle (interface state)
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
    CALL_DENOMINATION,
    CALL_AUTO_REPLACE_CLEAR,
    CALL_DETACH_PERSONAL_CAR,
    CALL_AUTO_BUY_RES,
    CALL_WASTE_CONTAINER,
    CALL_SET_FOR_BIO,
    CALL_FIRE_DISPATCH,
    CALL_DISCARD_VEH_CARGO,
    CALL_EMPTY_STORAGE,
    CALL_CANCEL_CRIME_ASSIGN,
    CALL_DO_FIND_CONNECTIONS,
    CALL_GAME_RULES_REBUILD_STORAGES,
};

// Appended only: a command names its site by index.
static const GameCallSite kSites[] = {
    { 0x7E8161, 0x4BDF90, "line window: copy schedule to vehicle", "MOObo", 0x128B8, 0, false },
    { 0x7E8175, 0x3DA390, "line window: refresh vehicle after schedule copy", "WO", 0, 0, false },
    { 0x7E82A9, 0x1C3780, "line window: replace vehicle", "WoOb", 0, 0, false },
    { 0x7E88C9, 0x3DA390, "line window: refresh vehicle", "WO", 0, 0, false },
    { 0x8187AA, 0x6F3220, "vehicle window: rebuild route after assignment", "Vbbb", 0, 0, false, CALL_PRESENTATION },
    { 0x818A9C, 0x6F3220, "vehicle window: rebuild route after assignment (second)", "Vbbb", 0, 0, false, CALL_PRESENTATION },
    { 0x8190EE, 0x6F9320, "vehicle window: release vehicle", "V", 0, 0, false, CALL_RELEASE_VEHICLE },
    { 0x8190F9, 0x3DA390, "vehicle window: refresh vehicle", "WO", 0, 0, false },
    { 0x818F8F, 0x6EB6C0, "vehicle window: toggle vehicle state", "V", 0, 0, false },
    { 0x819173, 0x6EB6C0, "vehicle window: toggle vehicle state (second)", "V", 0, 0, false },
    { 0x815BE6, 0x41F0F0, "vehicle window: send to depot (keyboard)", "WVbbbb", 0, 0, false },
    { 0x818F2B, 0x41F0F0, "vehicle window: send to depot", "WVbbbb", 0, 0, false },
    { 0x82736A, 0x4BDF90, "vehicle window: clear the vehicle's line", "MoVbo", 0x128B8, 0, false },
    { 0x827372, 0x6AF130, "vehicle window: refresh the vehicle after clearing its line", "V", 0, 0, false },
    { 0x818F38, 0x41EC10, "vehicle window: send back to workplace", "xV", 0, 1, true },
    { 0x81964B, 0x6A20E0, "vehicle window: next color/skin", "VI", 0, 0, false },
    { 0x813708, 0x3DA390, "line window: launch vehicle", "WO", 0, 0, false },
    { 0x818FD5, 0x63C3B0, "vehicle window: locomotive back to auto-replacement", "MV", 0xD4E0, 0, false },
    { 0x811F77, 0x4B7A30, "line window: clear line data", "L", 0, 0, false },
    { 0x377D57, 0x4BD260, "lines list: create new line", "MI", 0x128B8, 0, false },
    { 0x737F2E, 0x41F0F0, "building window: call all vehicles home (each vehicle)", "WVbbbb", 0, 0, false },
    { 0x77B9E3, 0x204DA0, "building window: try sending all vehicles with cargo", "WO", 0, 0, false },
    { 0x77FA69, 0x204EA0, "building window (second): try sending all vehicles with cargo", "WO", 0, 0, false },
    // 6bf1d0 rebuilds the cargo summary (vehicle +458/+470, the window's "Carried" list) from the
    // train's cargo; it writes nothing else, and the simulation's readers rebuild it first.
    { 0x815A0B, 0x6F3220, "vehicle window: cargo summary for display", "Vbbb", 0, 0, false, CALL_LOCAL },
    { 0x829BE3, 0x6F3220, "vehicle window: cargo summary for the schedule list", "Vbbb", 0, 0, false, CALL_LOCAL },
    { 0x810390, 0x4BBCB0, "line window: accept changes", "xLxx", 0, 0, false, CALL_LINE_ACCEPT },
    { 0x81039B, 0x4BAA20, "line window: accept (merge equal lines)", "ML", 0x128B8, 0, false, CALL_PRESENTATION },
    { 0x81041E, 0x245730, "line window: accept (mark the line changed)", "Lx", 0, 0, false, CALL_ACCEPT_MEASURES },
    { 0x81042D, 0x4BDAD0, "line window: accept (vehicles take the stops)", "ML", 0x128B8, 0, false, CALL_PRESENTATION },
    { 0x81043B, 0x4BFA80, "line window: accept (name a private line)", "xL", 0, 0, false, CALL_PRESENTATION },
    { 0x377FD1, 0x4BFA80, "lines list: delete line", "xL", 0, 0, false, CALL_DELETE_LINE },
    { 0x377FDF, 0x4BC740, "lines list: delete line (cleanup)", "Mb", 0x128B8, 0, false, CALL_PRESENTATION },
    // A late-phase tool (main thread): building_addfield adds the picked field to the farm +d298.
    { 0x1F3104, 0x17AA50, "field tool: add a field to a farm", "WBOF", 0, 0, false },
    // The junction window's Accept block (7595bb..7595f9): the copy into the junction is carried,
    // the rest of the block is made by the command on every PC.
    { 0x7969E2, 0x23F360, "junction window: accept (reset the junction)", "x", 0, 0, false, CALL_PRESENTATION },
    { 0x7969ED, 0x241590, "junction window: accept (copy into the junction)", "xx", 0, 0, false, CALL_JUNCTION_ACCEPT },
    { 0x796A05, 0x245730, "junction window: accept (mark the junction changed)", "xx", 0, 0, false, CALL_PRESENTATION },
    { 0x796A0C, 0x111350, "junction window: accept (take the junction off the list)", "xx", 0, 0, false, CALL_PRESENTATION },
    // The game menu's settings page: "Save settings", closing the window and the "Change settings?"
    // popup apply the page's copy of the game rules with 337170(world).
    { 0x35923F, 0x355DD0, "settings page: save settings", "W", 0, 0, false, CALL_GAME_RULES },
    { 0x355D6F, 0x355DD0, "settings page: close", "W", 0, 0, false, CALL_GAME_RULES },
    { 0x83F49F, 0x355DD0, "settings popup: change settings", "W", 0, 0, false, CALL_GAME_RULES },
    // Selling a vehicle: the confirm window's Yes (7f2d50) and the building windows' "sell all
    // vehicles" prompts call 3dee50(world, vehicle, 1, 0, 0, 0, 0.0f), which queues the sale.
    { 0x833130, 0x3FE170, "sell a vehicle", "WObbbbF", 0, 0, false },
    { 0x833C00, 0x3FE170, "sell vehicles of a building", "WObbbbF", 0, 0, false },
    { 0x833CFE, 0x3FE170, "sell a listed vehicle", "WObbbbF", 0, 0, false },
    // Links and office assignments the building windows remove or make: lists of
    // the buildings (+f98/+fb0, the assigned building's +888), heap only.
    { 0x77C63F, 0x1E2120, "building window: remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x77C64A, 0x17B490, "building window: remove link record by index", "xOI", 0, 0, false },
    { 0x780A16, 0x1E2120, "building window (second): remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x780A24, 0x17B490, "building window (second): remove link record by index", "xOI", 0, 0, false },
    { 0x783A19, 0x17B490, "station window: remove link record by index", "xOI", 0, 0, false },
    { 0x7A4AFA, 0x1E2120, "link list window: remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x7A4B0A, 0x17B520, "link list window: remove the link record to a partner", "xOO", 0, 0, false },
    { 0x7A4C1E, 0x17ACB0, "link list window: unassign building from office", "WOO", 0, 0, false },
    { 0x7ACD1A, 0x17ACB0, "office window: unassign building from office", "WOO", 0, 0, false },
    { 0x7ADB95, 0x17ACB0, "office list: unassign building from office", "WOO", 0, 0, false },
    { 0x77CF89, 0x17B380, "clear all links (confirmation)", "xO", 0, 0, false },
    { 0x74A99A, 0x17A5E0, "assign construction office", "WOOFbb", 0, 0, false },
    { 0x74AA1F, 0x17ACB0, "unassign construction office", "WOO", 0, 0, false },
    { 0x74E19A, 0x17A5E0, "assign construction office (second window)", "WOOFbb", 0, 0, false },
    { 0x74E220, 0x17ACB0, "unassign construction office (second window)", "WOO", 0, 0, false },
    // The depot window composes trains by drag and drop: 3fdef0(world, vehicle) takes the dragged
    // vehicle off its head, 3fd5d0(world, head, vehicle, index, 1, 0, &result) couples it at the
    // index (-1 the end; the result only picks the window's message), 3fe520(-, head) moves the
    // dragged vehicle and its wagons onto the head. With the vehicle window's replace or line copy
    // tool selected, its rows act like the line window's.
    { 0x806058, 0x421280, "depot window: uncouple the dragged vehicle (train drop)", "WV", 0, 0, false },
    { 0x806065, 0x4218B0, "depot window: attach the dragged train to the vehicle", "VV", 0, 0, false, CALL_DRAG },
    { 0x8064D7, 0x421280, "depot window: uncouple the dragged vehicle (row drop)", "WV", 0, 0, false, CALL_DEPOT_ROW_UNCOUPLE },
    { 0x806528, 0x420960, "depot window: couple the dragged vehicle at the row", "WVVIbbx", 0, 0, false, CALL_DEPOT_ROW_COUPLE },
    { 0x807300, 0x421280, "depot window: uncouple the dragged vehicle (end drop)", "WV", 0, 0, false },
    { 0x807337, 0x420960, "depot window: couple the dragged vehicle at the end", "WVVIbbx", 0, 0, false },
    { 0x806844, 0x1C3F50, "depot window: replace tool (prepare the replaced vehicle)", "WV", 0, 0, false },
    { 0x806855, 0x1C3780, "depot window: replace tool (replace by the row's vehicle)", "WoOb", 0, 0, false },
    { 0x806AA8, 0x4BDF90, "depot window: line copy tool (put the row's vehicle on the line)", "MLVbo", 0x128B8, 0, false },
    { 0x806ABC, 0x3DA390, "depot window: line copy tool (refresh the vehicle)", "WO", 0, 0, false },
    { 0x842330, 0x52D0, "rename window: write the name", "xxx", 0, 0, false, CALL_RENAME },
    { 0x734FDE, 0x422E60, "building window: invite Third World immigrants (USD)", "WxxB", 0, 0x1CE, false, CALL_INVITE_CITIZENS },
    { 0x7354DA, 0x422E60, "building window: invite Soviet immigrants (tutorial)", "WxxB", 1, 0x187, false, CALL_INVITE_CITIZENS },
    { 0x7356ED, 0x422E60, "building window: invite Soviet immigrants (RUB)", "WxxB", 2, 0x1A2, false, CALL_INVITE_CITIZENS },
    { 0x735C0E, 0x422E60, "building window: invite educated Soviet experts (RUB)", "WxxB", 3, 0x1FA, false, CALL_INVITE_CITIZENS },
    { 0x1F5402, 0x17A5E0, "demolition tool: assign office to network building", "WOOFbb", 0, 0, false },
    { 0x1F5419, 0x1B91B0, "demolition tool: set priority on network building", "WOb", 0, 0, false },
    { 0x1F59AD, 0x17A5E0, "demolition tool: assign office to building", "WOOFbb", 0, 0, false },
    { 0x1F59D1, 0x1B91B0, "demolition tool: set priority on building", "WOb", 0, 0, false },
    { 0x7AD844, 0x1B91B0, "office window: toggle building priority", "WOb", 0, 0, false },
    { 0x7BB137, 0x1B91B0, "building window: toggle construction/demolition priority", "WOb", 0, 0, false },
    { 0x81873A, RVA_VEH_ADD_STOP, "vehicle window: add Soviet border stop", "VMIx", WORLD_SOVIET_BORDER_BLD_OFF, 0x1EF, false, CALL_ADD_BORDER_STOP },
    { 0x818A2C, RVA_VEH_ADD_STOP, "vehicle window: add Western border stop", "VMIx", WORLD_WESTERN_BORDER_BLD_OFF, 0x1ED, false, CALL_ADD_BORDER_STOP },
    { 0x818D5D, RVA_VEH_DEL_STOP, "vehicle window: delete stop", "VIx", 0, 0x50, false, CALL_DELETE_STOP },
    { 0x818E9C, RVA_HELI_REROUTE, "vehicle window: change current stop", "VI", 0, 0, false, CALL_CHANGE_STOP },
    { 0x8196B7, RVA_VEH_INVERT_STOPS, "vehicle window: invert stop order", "V", 0, 0, false },
    { SITE_BLD_MANUAL_BUY_RES_RVA, BLD_MANUAL_BUY_RES_RVA, "building window: manual buy resources", "WBI", 0, 0, false, CALL_MANUAL_BUY_RES },
    { SITE_COPY_OFFICE_ASSIGN_NET_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "copy office tool: assign office to network building", "WOOFbb", 0, 0, false },
    { SITE_COPY_OFFICE_ASSIGN_BLD_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "copy office tool: assign office to building", "WOOFbb", 0, 0, false },
    { SITE_CONSTRUCT_TOOL_START_PLAN_RVA, RVA_START_PLANNED_CONSTRUCTION, "construction tool: start planned network construction", "WObx", 0, 0, false },
    { SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "construction tool: assign office to network building", "WOOFbb", 0, 0, false },
    { SITE_CONSTRUCT_TOOL_ASSIGN_BLD_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "construction tool: assign office to building", "WOOFbb", 0, 0, false },
    { SITE_COPY_DIST_OFFICE_RVA, FN_COPY_DIST_OFFICE_RVA, "copy tool: copy distribution office settings", "WBB", 0, 0, false },
    { SITE_COPY_WASTE_SETTINGS_RVA, FN_COPY_WASTE_SETTINGS_RVA, "copy tool: copy technical services waste settings", "WBB", 0, 0, false },
    { SITE_COPY_TRANSFER_SETTINGS_RVA, FN_COPY_TRANSFER_SETTINGS_RVA, "copy tool: copy factory connection settings", "WBB", 0, 0, false },
    { SITE_FACTORY_REPLACE_MACHINES_2_RVA, RVA_START_PLANNED_CONSTRUCTION, "factory window: replace machines", "WObb", 0, 0, false },
    // 0x7D8738 -> 0x664940 omitted: pure read-only per-frame vehicle-window predicate (interception flooded Lockstep).
    { SITE_DENOMINATION_RVA, FN_CURRENCY_DENOMINATION_RVA, "denomination popup: redenominate currency", "W", 0, 0, false, CALL_DENOMINATION },
    { SITE_FACTORY_REPLACE_MACHINES_1_RVA, RVA_START_PLANNED_CONSTRUCTION, "factory window: replace machines (first currency)", "WObb", 0, 0, false },
    { SITE_AUTO_REPLACE_CLEAR_RVA, RVA_SET_AUTO_REPLACE, "purchase window: clear auto-replace rule", "Mxx", WORLD_AUTO_REPLACE_MGR_OFFSET, 0, false, CALL_AUTO_REPLACE_CLEAR },
    { SITE_LINES_LIST_ASSIGN_VEHICLE_RVA, RVA_LINES_ASSIGN_VEHICLE, "lines list: assign line to vehicle (line_set)", "MLVbo", WORLD_LINES_OFFSET, 0, false },
    { SITE_RECALL_DETACH_PERSONAL_CAR_RVA, RVA_DETACH_PERSONAL_CAR, "building window: detach personal car from citizen", "WV", 0, 0, false, CALL_DETACH_PERSONAL_CAR },
    { SITE_RECALL_SEND_TO_DEPOT_RVA, SEND_TO_DEPOT, "building window: recall personal car to depot", "WVbbbb", 0, 0, false },
    { SITE_BLD_INIT_SLIDERS_DISABLE_RVA, BLD_INIT_RES_SLIDERS_RVA, "building window: init auto-buy disable sliders", "WBb", 0, 0, false, CALL_LOCAL },
    { SITE_BLD_INIT_SLIDERS_CANCEL_RVA, BLD_INIT_RES_SLIDERS_RVA, "building window: restore manual-buy cancel sliders", "WBb", 0, 0, false, CALL_LOCAL },
    { SITE_BLD_AUTO_BUY_DISABLE_RVA, BLD_APPLY_AUTO_BUY_RVA, "building window: disable auto-buy resources", "WBI", 1, 0, false, CALL_AUTO_BUY_RES },
    { SITE_BLD_AUTO_BUY_APPLY_RVA, BLD_APPLY_AUTO_BUY_RVA, "building window: apply auto-buy resources", "WBI", 0, 0, false, CALL_AUTO_BUY_RES },
    { SITE_FACTORY_QUEUE_LIMIT_DEC_RVA, RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT, "factory window: recompute queue worker limit (-)", "xB", 0, 0, false, CALL_PRESENTATION },
    { SITE_FACTORY_QUEUE_LIMIT_INC_RVA, RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT, "factory window: recompute queue worker limit (+)", "xB", 0, 0, false, CALL_PRESENTATION },
    { SITE_WASTE_CONTAINER_SPLIT_RVA, FN_WASTE_CONTAINER_SPLIT_RVA, "waste window: split waste container", "xBI", 0, 0, false, CALL_WASTE_CONTAINER },
    { SITE_WASTE_CONTAINER_MERGE_RVA, FN_WASTE_CONTAINER_MERGE_RVA, "waste window: merge waste container", "WBI", 0, 0, false, CALL_WASTE_CONTAINER },
    { SITE_SET_FOR_BIO_WASTE_WIN_RVA, FN_SET_FOR_BIO_RVA, "waste window: transfer bio-waste to compost/fertilizer slot", "BIIF", 0, 0, false, CALL_SET_FOR_BIO },
    { SITE_SET_FOR_BIO_RES_WIN_RVA, FN_SET_FOR_BIO_RVA, "building window: transfer bio-waste to compost/fertilizer slot", "BIIF", 0, 0, false, CALL_SET_FOR_BIO },
    { SITE_FIRESTATION_DISPATCH_ROAD_RVA, FN_FIRE_DISPATCH_RVA, "firestation tool: dispatch road vehicle to fire", "xBV", 0, 0, false, CALL_FIRE_DISPATCH },
    { SITE_FIRESTATION_DISPATCH_HELI_RVA, FN_FIRE_DISPATCH_RVA, "firestation tool: dispatch helicopter to fire", "xBV", 0, 0, false, CALL_FIRE_DISPATCH },
    { SITE_ASSIGN_CO_SINGLE_START_RVA, RVA_START_PLANNED_CONSTRUCTION, "assign co tool: start planned construction (single)", "WObx", 0, 0, false },
    { SITE_ASSIGN_CO_SINGLE_ASSIGN_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "assign co tool: assign office (single)", "WOOFbb", 0, 0, false },
    { SITE_ASSIGN_CO_SINGLE_RAIL_START_RVA, RVA_START_PLANNED_CONSTRUCTION, "assign co tool: start planned construction (single rail)", "WObx", 0, 0, false },
    { SITE_ASSIGN_CO_SINGLE_RAIL_ASSIGN_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "assign co tool: assign rail office (single)", "WOOFbb", 0, 0, false },
    { SITE_ASSIGN_CO_RECT_START_RVA, RVA_START_PLANNED_CONSTRUCTION, "assign co tool: start planned construction (rect)", "WObx", 0, 0, false },
    { SITE_ASSIGN_CO_RECT_ASSIGN_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "assign co tool: assign office (rect)", "WOOFbb", 0, 0, false },
    { SITE_ASSIGN_CO_RECT_RAIL_START_RVA, RVA_START_PLANNED_CONSTRUCTION, "assign co tool: start planned construction (rect rail)", "WObx", 0, 0, false },
    { SITE_ASSIGN_CO_RECT_RAIL_ASSIGN_RVA, RVA_ASSIGN_CONSTRUCTION_OFFICE, "assign co tool: assign rail office (rect)", "WOOFbb", 0, 0, false },
    { SITE_ASSIGN_CO_SINGLE_UNASSIGN_RVA, RVA_UNASSIGN_CONSTRUCTION_OFFICE, "assign co tool: unassign office (single)", "WOO", 0, 0, false },
    { SITE_ASSIGN_CO_RECT_UNASSIGN_RVA, RVA_UNASSIGN_CONSTRUCTION_OFFICE, "assign co tool: unassign office (rect)", "WOO", 0, 0, false },
    { SITE_DISCARD_VEH_CARGO_RVA, FN_DISCARD_VEH_CARGO_RVA, "discard popup: discard vehicle cargo", "Vb", 0, 0, false, CALL_DISCARD_VEH_CARGO },
    { SITE_DISCARD_CARRIED_VEH_RVA, FN_DISCARD_CARRIED_VEH_RVA, "discard popup: discard carried vehicle", "WVbbbbI", 0, 0, false },
    { SITE_EMPTY_STORAGE_RVA, FN_CAN_DISCARD_STORAGE_RVA, "discard popup: empty whole building storage", "BI", 0, 0, false, CALL_EMPTY_STORAGE },
    { SITE_EMPTY_BLD_VEH_CARGO_POPUP_RVA, FN_DISCARD_VEH_CARGO_RVA, "discard popup: empty parked vehicle cargo", "Vb", 0, 0, false },
    { SITE_EMPTY_BLD_VEH_CARGO_CONFIRM_RVA, FN_DISCARD_VEH_CARGO_RVA, "building confirm: empty parked vehicle cargo", "Vb", 0, 0, false },
    { SITE_REPAIR_CANCEL_QUEUE_RVA, FN_REPAIR_CANCEL_QUEUE_RVA, "repair station: cancel vehicle repair queue item", "WBII", 0, 0, false },
    { SITE_CANCEL_CRIME_ASSIGN_RVA, FN_UI_PLAY_SOUND_RVA, "crime window: cancel case assignment", "WxIBI", 0, 0, false, CALL_CANCEL_CRIME_ASSIGN },
    { SITE_DO_FIND_CONNECTIONS_RVA, FN_PF_CTOR_RVA, "distribution office: find factory/storage connections", "xWB", 0, 0, false, CALL_DO_FIND_CONNECTIONS },
    { SITE_GAME_RULES_REBUILD_1_RVA, FN_REBUILD_BLD_STORAGES_RVA, "settings page: rebuild storages before close", "WB", 0, 0, false, CALL_GAME_RULES_REBUILD_STORAGES },
    { SITE_GAME_RULES_REBUILD_2_RVA, FN_REBUILD_BLD_STORAGES_RVA, "settings page: rebuild storages before save", "WB", 0, 0, false, CALL_GAME_RULES_REBUILD_STORAGES },
    { SITE_GAME_RULES_REBUILD_3_RVA, FN_REBUILD_BLD_STORAGES_RVA, "settings popup: rebuild storages before apply", "WB", 0, 0, false, CALL_GAME_RULES_REBUILD_STORAGES },
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
        if (*(const int*)(vehicle + 0x182C) == 1 && vType != 0 && nStops == 1 &&
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
    *(BYTE*)(vehicle + 0xD78) = 1;
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
        *(BYTE*)(vehicle + 0xD78) = 1;
        *(float*)(vehicle + 0x66C) = -1.0f;
        *(BYTE*)(vehicle + 0x668) = 1;
        if (cat == 4) *(BYTE*)(vehicle + 0xD79) = 1;
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
           special == CALL_DELETE_STOP || special == CALL_CHANGE_STOP || special == CALL_RELEASE_VEHICLE ||
           special == CALL_DENOMINATION || special == CALL_DETACH_PERSONAL_CAR ||
           special == CALL_WASTE_CONTAINER || special == CALL_SET_FOR_BIO ||
           special == CALL_FIRE_DISPATCH || special == CALL_DISCARD_VEH_CARGO ||
           special == CALL_EMPTY_STORAGE || special == CALL_CANCEL_CRIME_ASSIGN ||
           special == CALL_DO_FIND_CONNECTIONS;
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

static bool IsOrphanedCarriedMechanism(U64 carrier, U64 mech)
{
    if (!IsPointerLikeX64(carrier) || !IsPointerLikeX64(mech)) return false;
    return *(U64*)(carrier + VEH_WORKPLACE_BLD_OFFSET) != 0 &&
           *(U64*)(mech + VEH_LINK_PTR_OFFSET) == carrier &&
           *(U64*)(mech + VEH_WORKPLACE_BLD_OFFSET) == 0 &&
           *(U64*)(mech + VEH_DEPOT_PTR_OFFSET) == 0;
}

static void DetachSingleMechFromCarrier(U64 carrier, U64 mech)
{
    if (!IsPointerLikeX64(carrier) || !IsPointerLikeX64(mech)) return;
    U64 b = *(U64*)(carrier + VEH_CARRIED_BEGIN_OFFSET);
    U64 e = *(U64*)(carrier + VEH_CARRIED_END_OFFSET);
    bool found = false;
    if (IsPointerLikeX64(b) && e > b && (e - b) <= 64 * 8) {
        U64 writePtr = b;
        for (U64 p = b; p + 8 <= e; p += 8) {
            U64 cur = *(U64*)p;
            if (cur == mech) {
                found = true;
            } else {
                *(U64*)writePtr = cur;
                writePtr += 8;
            }
        }
        if (found && IsOrphanedCarriedMechanism(carrier, mech)) {
            *(U64*)(carrier + VEH_CARRIED_END_OFFSET) = writePtr;
        }
    }
    if (found && IsOrphanedCarriedMechanism(carrier, mech)) {
        RestoreMechToBuildingInventory(mech, carrier);
    }
}

static void RecoverOrphanedCarrierMechanisms(U64 veh)
{
    if (!IsPointerLikeX64(veh)) return;
    __try {
        U64 carrier = *(U64*)(veh + VEH_LINK_PTR_OFFSET);
        if (carrier) {
            DetachSingleMechFromCarrier(carrier, veh);
        }
        U64 b = *(U64*)(veh + VEH_CARRIED_BEGIN_OFFSET);
        U64 e = *(U64*)(veh + VEH_CARRIED_END_OFFSET);
        if (IsPointerLikeX64(b) && e > b && (e - b) <= 64 * 8) {
            U64 count = (e - b) / 8;
            U64 mechs[64] = {};
            U64 recoveredCount = 0;
            U64 writePtr = b;
            for (U64 i = 0; i < count; i++) {
                U64 cur = *(U64*)(b + i * 8);
                if (IsOrphanedCarriedMechanism(veh, cur)) {
                    mechs[recoveredCount++] = cur;
                } else {
                    *(U64*)writePtr = cur;
                    writePtr += 8;
                }
            }
            if (recoveredCount > 0) {
                *(U64*)(veh + VEH_CARRIED_END_OFFSET) = writePtr;
                for (U64 i = 0; i < recoveredCount; i++) {
                    RestoreMechToBuildingInventory(mechs[i], veh);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void GameCalls_RecoverOrphanedCarrierMechanismsForTest(unsigned long long vehicle)
{
    RecoverOrphanedCarrierMechanisms(vehicle);
}

static bool IsCarrierRecoverySite(unsigned site, unsigned target, const char* schema)
{
    if (target == SEND_TO_DEPOT || target == 0x41EC10) return true;
    return target == 0x4BDF90 && site == 0x82736a && schema && schema[2] == 'V';
}

bool GameCalls_ShouldRecoverCarrierForSiteForTest(unsigned callAt, unsigned target, const char* schema)
{
    return IsCarrierRecoverySite(callAt, target, schema);
}

static U64 ExtractCarrierRecoveryVehicle(const GameCallSite& s, const U64* a)
{
    if (!IsCarrierRecoverySite(s.site, s.target, s.schema)) return 0;
    if (s.target == SEND_TO_DEPOT || s.target == 0x41EC10) return a[1];
    if (s.target == 0x4BDF90) return a[2];
    return 0;
}

static const unsigned long long kDenominationCooldownTicks = 150ULL;
static bool g_hasDenominationStep = false;
static unsigned long long g_lastDenominationStep = 0;

void GameCalls_ResetSessionState()
{
    g_hasDenominationStep = false;
    g_lastDenominationStep = 0;
    g_skipDepotRowCouple = false;
}

bool GameCalls_IsDenominationCommand(const void* data, size_t size)
{
    if (!data || size != sizeof(GameCallMsg)) return false;
    GameCallMsg m;
    memcpy(&m, data, sizeof(m));
    return m.target == FN_CURRENCY_DENOMINATION_RVA && m.index < SITE_COUNT &&
           kSites[m.index].special == CALL_DENOMINATION;
}

static void ExecuteDenominationNow(const GameCallSite& s, const U64* a)
{
    BYTE* world = (BYTE*)a[0];
    const unsigned long long curStep = Determinism_StepCount();
    if (g_hasDenominationStep && curStep >= g_lastDenominationStep &&
        (curStep - g_lastDenominationStep) < kDenominationCooldownTicks) {
        Logf("republicmp  GAME CALL: skipping duplicate denomination within cooldown (%llu < %llu)",
             curStep - g_lastDenominationStep, kDenominationCooldownTicks);
        TwoStates_CloseDenominationPopups(world);
        return;
    }
    g_hasDenominationStep = true;
    g_lastDenominationStep = curStep;
    if (g_exeBase && g_exeBase[s.target] != 0) {
        CallSite(s, a);
    }
    TwoStates_ApplyDenomination(world);
}

static void DetachPersonalCarNow(U64 world, U64 veh)
{
    if (!world || !IsPointerLikeX64(veh)) return;
    U64 citizen = 0;
    if (!SafeReadPod((const U64*)(veh + VEH_OWNER_CITIZEN_OFFSET), &citizen) || !IsPointerLikeX64(citizen)) return;
    U64 linkedVeh = 0;
    if (!SafeReadPod((const U64*)(citizen + CIT_PERSONAL_CAR_OFFSET), &linkedVeh) || linkedVeh != veh) return;
    if (g_exeBase && g_exeBase[RVA_DETACH_PERSONAL_CAR] != 0) {
        typedef void (*DetachCarFn)(U64 world, U64 citizen);
        ((DetachCarFn)(g_exeBase + RVA_DETACH_PERSONAL_CAR))(world, citizen);
    }
}

static bool ReadBuildingStorageRange(U64 bld, U64& outBeg, U64& outCount)
{
    outBeg = 0;
    outCount = 0;
    if (!IsPointerLikeX64(bld)) return false;
    U64 v[2] = {};
    if (!SafeMemcpyPod(v, (const void*)(bld + BLD_STORAGE_OFFSET), sizeof(v))) return false;
    if (!IsPointerLikeX64(v[0]) || v[1] <= v[0] || (v[1] - v[0]) % BLD_STORAGE_SLOT_SIZE != 0) return false;
    const U64 count = (v[1] - v[0]) / BLD_STORAGE_SLOT_SIZE;
    if (count == 0 || count > 64) return false;
    outBeg = v[0];
    outCount = count;
    return true;
}

static bool FindSlotIndexInBuilding(U64 bld, U64 slotPtr, unsigned& outIdx)
{
    outIdx = 0;
    U64 beg = 0, count = 0;
    if (!IsPointerLikeX64(slotPtr) || !ReadBuildingStorageRange(bld, beg, count)) return false;
    const U64 end = beg + count * (U64)BLD_STORAGE_SLOT_SIZE;
    if (slotPtr < beg || slotPtr >= end || (slotPtr - beg) % BLD_STORAGE_SLOT_SIZE != 0) return false;
    outIdx = (unsigned)((slotPtr - beg) / BLD_STORAGE_SLOT_SIZE);
    return true;
}

template <typename Pred>
static U64 FindWorldBuilding(Pred pred)
{
    if (!g_exeBase) return 0;
    const unsigned kVecRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
    for (unsigned vecRva : kVecRvas) {
        U64 v[2] = {};
        if (!SafeMemcpyPod(v, g_exeBase + vecRva, sizeof(v)) || !IsPointerLikeX64(v[0]) || v[1] <= v[0]) continue;
        const U64 n = (v[1] - v[0]) / sizeof(U64);
        const U64 limit = n < 16384u ? n : 16384u;
        for (U64 i = 0; i < limit; ++i) {
            U64 b = 0;
            if (!SafeReadPod((const void*)(v[0] + i * sizeof(U64)), &b)) continue;
            if (pred(b)) return b;
        }
    }
    return 0;
}

static bool ReadVectorByteRange(U64 vecAddr, size_t stride, U64 outVec[2], U64& outCount)
{
    outVec[0] = 0;
    outVec[1] = 0;
    outCount = 0;
    if (!stride || !SafeMemcpyPod(outVec, (const void*)vecAddr, 2u * sizeof(U64))) return false;
    if (!IsPointerLikeX64(outVec[0]) || outVec[1] <= outVec[0]) return false;
    const U64 diff = outVec[1] - outVec[0];
    if ((diff % stride) != 0) return false;
    outCount = diff / stride;
    return true;
}

static bool ReadPtrVectorRange(U64 vecAddr, U64 outVec[2], U64& outCount)
{
    return ReadVectorByteRange(vecAddr, sizeof(U64), outVec, outCount);
}

static bool FindBuildingOwningSingleSlot(U64 slotPtr, U64& outBld, unsigned& outSlotIdx)
{
    outBld = 0;
    outSlotIdx = 0;
    if (!g_exeBase || !IsPointerLikeX64(slotPtr)) return false;
    U64 activeBld = 0;
    if (SafeReadPod(World() + WORLD_ACTIVE_WINDOW_BLD_OFF, &activeBld) &&
        FindSlotIndexInBuilding(activeBld, slotPtr, outSlotIdx)) {
        outBld = activeBld;
        return true;
    }
    const U64 matched = FindWorldBuilding([&](U64 b) {
        return FindSlotIndexInBuilding(b, slotPtr, outSlotIdx);
    });
    if (!matched) return false;
    outBld = matched;
    return true;
}

static bool FindBuildingOwningSlots(U64 srcSlot, U64 dstSlot, U64& outBld, unsigned& outSrcIdx, unsigned& outDstIdx)
{
    outBld = 0;
    outSrcIdx = 0;
    outDstIdx = 0;
    if (!g_exeBase || !IsPointerLikeX64(srcSlot) || !IsPointerLikeX64(dstSlot) || srcSlot == dstSlot) return false;
    U64 bld = 0;
    unsigned srcIdx = 0;
    unsigned dstIdx = 0;
    if (!FindBuildingOwningSingleSlot(srcSlot, bld, srcIdx) ||
        !FindSlotIndexInBuilding(bld, dstSlot, dstIdx)) {
        return false;
    }
    outBld = bld;
    outSrcIdx = srcIdx;
    outDstIdx = dstIdx;
    return true;
}

static U64 FindBuildingByFireObj(U64 fireObj)
{
    if (!g_exeBase || !IsPointerLikeX64(fireObj)) return 0;
    return FindWorldBuilding([fireObj](U64 b) {
        U64 fo = 0;
        return IsPointerLikeX64(b) &&
               SafeReadPod((const U64*)(b + BLD_FIRE_OBJ_OFFSET), &fo) &&
               fo == fireObj;
    });
}

static U64 ResolveBuildingSlotPtr(U64 bld, unsigned slotIdx)
{
    U64 beg = 0, count = 0;
    if (!ReadBuildingStorageRange(bld, beg, count) || (U64)slotIdx >= count) return 0;
    return beg + (U64)slotIdx * (U64)BLD_STORAGE_SLOT_SIZE;
}

static void ExecuteWasteContainerNow(const GameCallSite& s, const U64* a, U64& r)
{
    U64 slotPtr = ResolveBuildingSlotPtr(a[1], (unsigned)a[2]);
    if (!slotPtr) return;
    U64 callArgs[MAX_ARGS] = { a[0], a[1], slotPtr, a[3], a[4], a[5], a[6] };
    if (g_exeBase && g_exeBase[s.target] != 0) {
        r = CallSite(s, callArgs);
    }
}

static void ExecuteSetForBioNow(const GameCallSite& s, const U64* a, U64& r)
{
    const U64 bld = a[0];
    const float maxReq = AsFloat(a[3]);
    if (!IsPointerLikeX64(bld) || !isfinite(maxReq) || maxReq <= 0.0f || maxReq >= 1.0e7f) return;
    const U64 srcSlot = ResolveBuildingSlotPtr(bld, (unsigned)a[1]);
    const U64 dstSlot = ResolveBuildingSlotPtr(bld, (unsigned)a[2]);
    if (!srcSlot || !dstSlot || srcSlot == dstSlot) return;
    if (!g_exeBase || g_exeBase[s.target] == 0) return;
    typedef float (*SetForBioFn)(U64 unused, U64 src, U64 dst, float maxReqArg);
    const float moved = ((SetForBioFn)(g_exeBase + s.target))(0, srcSlot, dstSlot, maxReq);
    unsigned bits = 0;
    memcpy(&bits, &moved, 4);
    r = (U64)bits;
    if (s.site == SITE_SET_FOR_BIO_RES_WIN_RVA && isfinite(moved) && moved > 0.0f) {
        float scale = 60.0f;
        float romScale = 0.0f;
        if (SafeReadPod(g_exeBase + FLT_BIO_SCALE_130C_RVA, &romScale) &&
            isfinite(romScale) && romScale > 0.0f) {
            scale = romScale;
        }
        *(float*)(bld + BLD_BIO_TRANSFER_LOG0_OFFSET) += moved;
        *(float*)(bld + BLD_BIO_TRANSFER_LOG1_OFFSET) += moved * scale;
    }
}

static void ExecuteFireDispatchNow(const GameCallSite& s, const U64* a)
{
    U64 fireObj = 0;
    if (!IsPointerLikeX64(a[1]) || !IsPointerLikeX64(a[2])) return;
    if (!SafeReadPod((const U64*)(a[1] + BLD_FIRE_OBJ_OFFSET), &fireObj) || !IsPointerLikeX64(fireObj)) return;
    U64 callArgs[MAX_ARGS] = { 0, fireObj, a[2], 0, 0, 0, 0 };
    if (g_exeBase && g_exeBase[s.target] != 0) {
        CallSite(s, callArgs);
    }
    if (s.site == SITE_FIRESTATION_DISPATCH_HELI_RVA) {
        SafeWritePod((BYTE*)(a[2] + VEH_BAY_ARRIVED_FLAG_OFFSET), (BYTE)0);
    }
}

static void ResetVehicleUnloadingCounters(U64 veh)
{
    if (!IsPointerLikeX64(veh)) return;
    SafeWritePod((U64*)(veh + VEH_LOAD_TIMER_55C_OFF), (U64)0);
    SafeWritePod((U64*)(veh + VEH_LOAD_TIMER_594_OFF), (U64)0);
    SafeWritePod((float*)(veh + VEH_LOAD_RATE_568_OFF), 1.0f);
    SafeWritePod((uint32_t*)(veh + VEH_LOAD_STATE_179C_OFF), (uint32_t)0);
    SafeWritePod((uint32_t*)(veh + VEH_LOAD_STATE_564_OFF), (uint32_t)0);
    SafeWritePod((uint32_t*)(veh + VEH_LOAD_STATE_1310_OFF), (uint32_t)0);
    SafeWritePod((BYTE*)(veh + VEH_LOAD_FLAG_59C_OFF), (BYTE)0);
}

static void ExecuteDiscardVehCargoNow(const GameCallSite& s, const U64* a)
{
    const U64 veh = a[0];
    if (!IsPointerLikeX64(veh)) return;
    if (g_exeBase && g_exeBase[s.target] != 0) {
        CallSite(s, a);
    }
    U64 bayTargetBld = 0;
    if (!SafeReadPod((const U64*)(veh + VEH_BAY_TARGET_BLD_OFFSET), &bayTargetBld) || bayTargetBld == 0) {
        return;
    }
    U64 wagonsVec[2] = {};
    U64 count = 0;
    // Mirror 0x8084DC..0x8084F0 (je 0x808598): game resets unloading counters only when 0x3A0..0x3A8 is non-empty.
    if (!ReadPtrVectorRange(veh + VEH_ATTACHED_WAGONS_BEGIN_OFF, wagonsVec, count)) return;
    const U64 limit = count < 256u ? count : 256u;
    ResetVehicleUnloadingCounters(veh);
    for (U64 i = 0; i < limit; ++i) {
        U64 wagon = 0;
        if (SafeReadPod((const U64*)(wagonsVec[0] + i * sizeof(U64)), &wagon)) {
            ResetVehicleUnloadingCounters(wagon);
        }
    }
}

static bool CanDiscardStorageSlot(U64 slotPtr)
{
    if (!IsPointerLikeX64(slotPtr)) return false;
    if (g_exeBase && g_exeBase[FN_CAN_DISCARD_STORAGE_RVA] != 0) {
        typedef unsigned char (*CanDiscardStorageFn)(U64 slot);
        if ((((CanDiscardStorageFn)(g_exeBase + FN_CAN_DISCARD_STORAGE_RVA))(slotPtr) & 0xFFu) == 0) {
            return false;
        }
    }
    return true;
}

static void ExecuteEmptyStorageNow(const U64* a)
{
    const U64 slotPtr = ResolveBuildingSlotPtr(a[0], (unsigned)a[1]);
    if (!CanDiscardStorageSlot(slotPtr)) return;
    U64 vec[2] = {};
    if (!SafeMemcpyPod(vec, (const void*)slotPtr, sizeof(vec)) ||
        !IsPointerLikeX64(vec[0]) || vec[1] <= vec[0] || ((vec[1] - vec[0]) & 0xFu) != 0) {
        return;
    }
    const U64 count = (vec[1] - vec[0]) / 16u;
    const U64 limit = count < 256u ? count : 256u;
    for (U64 i = 0; i < limit; ++i) {
        SafeWritePod((uint32_t*)(vec[0] + i * 16u + 8u), (uint32_t)0);
    }
}

static bool ComputeCrimeCaseCheck(U64 crimeCase, unsigned& outCheck)
{
    outCheck = 0;
    if (!IsPointerLikeX64(crimeCase)) return false;
    int32_t status = 0;
    int32_t crimeType = 0;
    U64 crimeBld = 0;
    if (!SafeReadPod((const int32_t*)(crimeCase + CASE_STATUS_OFF), &status) ||
        !SafeReadPod((const int32_t*)(crimeCase + CASE_TYPE_OFF), &crimeType) ||
        !SafeReadPod((const U64*)(crimeCase + CASE_BLD_OFF), &crimeBld)) {
        return false;
    }
    uint32_t crimeBldId = 0;
    if (crimeBld != 0) {
        if (!IsPointerLikeX64(crimeBld) ||
            !SafeReadPod((const uint32_t*)crimeBld, &crimeBldId)) {
            return false;
        }
    }
    outCheck = ((unsigned)crimeType & 0xFFu) |
               (((unsigned)status & 0xFFu) << 8) |
               ((crimeBldId & 0xFFFFu) << 16);
    return true;
}

static void IncrementCrimeTypeStat(U64 baseAddr, int32_t crimeType, unsigned off0, unsigned off2, unsigned off3)
{
    unsigned off = 0;
    if (crimeType == 2) off = off2;
    else if (crimeType == 3) off = off3;
    else if (crimeType == 0) off = off0;
    else return;
    uint32_t val = 0;
    if (SafeReadPod((const uint32_t*)(baseAddr + off), &val)) {
        SafeWritePod((uint32_t*)(baseAddr + off), val + 1u);
    }
}

static void ExecuteCancelCrimeAssignNow(const U64* a)
{
    const U64 world = a[0];
    const U64 caseIdx = a[2];
    const U64 windowBld = a[3];
    const unsigned expectedCheck = (unsigned)a[4];
    if (world != (U64)World() || !IsPointerLikeX64(windowBld)) return;

    U64 casesVec[2] = {};
    U64 count = 0;
    if (!ReadPtrVectorRange(world + WORLD_CRIME_CASES_BEGIN_OFF, casesVec, count) ||
        caseIdx >= count || caseIdx >= 65536u) {
        return;
    }

    U64 crimeCase = 0;
    unsigned actualCheck = 0;
    if (!SafeReadPod((const U64*)(casesVec[0] + caseIdx * sizeof(U64)), &crimeCase) ||
        !ComputeCrimeCaseCheck(crimeCase, actualCheck) || actualCheck != expectedCheck) {
        return;
    }

    int32_t status = 0;
    int32_t crimeType = 0;
    U64 crimeBld = 0;
    if (!SafeReadPod((const int32_t*)(crimeCase + CASE_STATUS_OFF), &status) ||
        !SafeReadPod((const int32_t*)(crimeCase + CASE_TYPE_OFF), &crimeType) ||
        !SafeReadPod((const U64*)(crimeCase + CASE_BLD_OFF), &crimeBld)) {
        return;
    }

    if (IsPointerLikeX64(crimeBld)) {
        U64 region = 0;
        U64 cityStats = 0;
        if (SafeReadPod((const U64*)(crimeBld + BLD_CITY_REGION_PTR_OFF), &region) && IsPointerLikeX64(region) &&
            SafeReadPod((const U64*)(region + REGION_CITY_STATS_PTR_OFF), &cityStats) && IsPointerLikeX64(cityStats)) {
            IncrementCrimeTypeStat(cityStats, crimeType, CITY_STATS_CRIME0_OFF, CITY_STATS_CRIME2_OFF, CITY_STATS_CRIME3_OFF);
        }
    }
    IncrementCrimeTypeStat(world, crimeType, WORLD_CRIME_EXPIRED_STAT0_OFF, WORLD_CRIME_EXPIRED_STAT2_OFF, WORLD_CRIME_EXPIRED_STAT3_OFF);

    if (g_exeBase && g_exeBase[RVA_CRIME_EXPIRE_CASE] != 0) {
        typedef void (*CrimeExpireFn)(U64 w, U64 cc, float zero);
        ((CrimeExpireFn)(g_exeBase + RVA_CRIME_EXPIRE_CASE))(world, crimeCase, 0.0f);
    }
    if (g_exeBase && g_exeBase[FN_CRIME_DELETE_CASE_RVA] != 0) {
        typedef void (*CrimeDeleteFn)(U64 w, U64 cc);
        ((CrimeDeleteFn)(g_exeBase + FN_CRIME_DELETE_CASE_RVA))(world, crimeCase);
    }
    if (status >= 3 && status <= 12) {
        const U64 counterOff = BLD_CRIME_CANCEL_COUNTERS_OFF + (U64)(status - 3) * 4u;
        uint16_t cnt = 0;
        if (SafeReadPod((const uint16_t*)(windowBld + counterOff), &cnt)) {
            SafeWritePod((uint16_t*)(windowBld + counterOff), (uint16_t)(cnt + 1u));
        }
    }
}

static void ClearExistingDoConnections(U64 doBld)
{
    U64 connVec[2] = {};
    U64 count = 0;
    if (ReadPtrVectorRange(doBld + BLD_DO_CONNECTIONS_BEGIN_OFF, connVec, count) &&
        g_exeBase && g_exeBase[FN_DO_REMOVE_LINK_RVA] != 0) {
        typedef void (*DoRemoveLinkFn)(U64 unused, U64 doBuilding, U64 targetBuilding);
        const auto removeFn = (DoRemoveLinkFn)(g_exeBase + FN_DO_REMOVE_LINK_RVA);
        const U64 limit = count < 256u ? count : 256u;
        for (U64 i = 0; i < limit; ++i) {
            U64 entry = 0;
            U64 targetBld = 0;
            if (SafeReadPod((const U64*)(connVec[0] + i * sizeof(U64)), &entry) && IsPointerLikeX64(entry) &&
                SafeReadPod((const U64*)(entry + 8u), &targetBld) && IsPointerLikeX64(targetBld)) {
                removeFn(0, doBld, targetBld);
            }
        }
    }
    U64 begPtr = 0;
    if (SafeReadPod((const U64*)(doBld + BLD_DO_CONNECTIONS_BEGIN_OFF), &begPtr) &&
        (begPtr == 0 || IsPointerLikeX64(begPtr))) {
        SafeWritePod((U64*)(doBld + BLD_DO_CONNECTIONS_END_OFF), begPtr);
    }
}

static void SeedDoPathfinderStarts(U64 doBld, BYTE* pf)
{
    if (!g_exeBase || g_exeBase[FN_PF_ADD_START_RVA] == 0) return;
    U64 vehVec[2] = {};
    if (!SafeMemcpyPod(vehVec, (const void*)(doBld + BLD_DO_VEH_ENTRIES_BEGIN_OFF), sizeof(vehVec)) ||
        !IsPointerLikeX64(vehVec[0]) || vehVec[1] <= vehVec[0] ||
        ((vehVec[1] - vehVec[0]) % BLD_DO_VEH_ENTRY_STRIDE) != 0) {
        return;
    }
    float negOne = -1.0f;
    float romNegOne = 0.0f;
    if (SafeReadPod(g_exeBase + FLT_NEG_ONE_RVA, &romNegOne) && isfinite(romNegOne) && romNegOne < 0.0f) {
        negOne = romNegOne;
    }
    typedef void (*PfAddStartFn)(void* pfPtr, U64 nodePtr, float startCost, int zeroFlag, float maxCost);
    const auto addStartFn = (PfAddStartFn)(g_exeBase + FN_PF_ADD_START_RVA);
    const U64 count = (vehVec[1] - vehVec[0]) / BLD_DO_VEH_ENTRY_STRIDE;
    const U64 limit = count < 256u ? count : 256u;
    for (U64 i = 0; i < limit; ++i) {
        const U64 entryAddr = vehVec[0] + i * BLD_DO_VEH_ENTRY_STRIDE;
        int32_t entryKind = 0;
        U64 nodePtr = 0;
        if (SafeReadPod((const int32_t*)entryAddr, &entryKind) && entryKind == 3 &&
            SafeReadPod((const U64*)(entryAddr + 0x30), &nodePtr) && IsPointerLikeX64(nodePtr)) {
            addStartFn(pf, nodePtr, 0.0f, 0, negOne);
        }
    }
}

static U64 ResolveDoResultTargetBuilding(U64 res)
{
    U64 pathBld = 0;
    U64 bldType = 0;
    int32_t bldCat = 0;
    if (!IsPointerLikeX64(res) ||
        !SafeReadPod((const U64*)(res + 0x40), &pathBld) || !IsPointerLikeX64(pathBld) ||
        !SafeReadPod((const U64*)(pathBld + BLD_TYPE_OFFSET), &bldType) || !IsPointerLikeX64(bldType) ||
        !SafeReadPod((const int32_t*)(bldType + 0x368), &bldCat) || bldCat == 0x2D) {
        return 0;
    }
    U64 nodesVec[2] = {};
    U64 count = 0;
    if (!ReadPtrVectorRange(res, nodesVec, count)) return 0;
    U64 lastNode = 0;
    U64 bldListPtr = 0;
    U64 resolvedBld = 0;
    if (!SafeReadPod((const void*)(nodesVec[1] - sizeof(U64)), &lastNode) || !IsPointerLikeX64(lastNode) ||
        !SafeReadPod((const U64*)(lastNode + 0x70), &bldListPtr) || !IsPointerLikeX64(bldListPtr) ||
        !SafeReadPod((const U64*)bldListPtr, &resolvedBld) || !IsPointerLikeX64(resolvedBld)) {
        return 0;
    }
    return resolvedBld;
}

static void InitDoConnectionCfg(BYTE* cfg)
{
    cfg[0x00] = 1;
    cfg[0xC8] = 1;
    *(float*)(cfg + 0x04) = 1.0f;
    *(U64*)(cfg + 0x10) = *(const U64*)(cfg + 0x08);
    *(float*)(cfg + 0xCC) = 1.0f;
    *(U64*)(cfg + 0xD8) = *(const U64*)(cfg + 0xD0);
    *(U64*)(cfg + 0x190) = 0;
    *(U64*)(cfg + 0x20) = 0;
    cfg[0x28] = 1;
    *(U64*)(cfg + 0xE8) = 0;
    cfg[0xF0] = 1;
}

static bool AttachSingleDoConnection(U64 doBld, U64 res)
{
    const U64 targetBld = ResolveDoResultTargetBuilding(res);
    if (!targetBld) return false;
    typedef void* (*OpNewFn)(size_t bytes);
    typedef void (*VecPushFn)(void* vec, void** elem);
    typedef void* (*CfgCtorFn)(void* cfg);
    const auto opNew = (OpNewFn)(g_exeBase + FN_GAME_OP_NEW_RVA);
    const auto vecPush = (VecPushFn)(g_exeBase + FN_DO_VEC_PUSH_RVA);
    const auto cfgCtor = (CfgCtorFn)(g_exeBase + FN_DO_CFG_CTOR_RVA);

    void* rawCfg = opNew(0x198u);
    if (!rawCfg) return false;
    memset(rawCfg, 0, 0x198u);
    BYTE* cfg = (BYTE*)cfgCtor(rawCfg);
    if (!cfg) return false;
    InitDoConnectionCfg(cfg);

    BYTE* doEntry = (BYTE*)opNew(0x18u);
    if (!doEntry) return false;
    doEntry[0] = 0;
    *(U64*)(doEntry + 0x08) = targetBld;
    *(U64*)(doEntry + 0x10) = (U64)(uintptr_t)cfg;

    void* pushVal = doEntry;
    vecPush((void*)(doBld + BLD_DO_CONNECTIONS_BEGIN_OFF), &pushVal);

    SafeWritePod((BYTE*)(doBld + BLD_DO_HAS_CONN_FLAG_OFF), (BYTE)1);
    SafeWritePod((float*)(doBld + BLD_DO_TIMER_OFF), 3.0f);
    return true;
}

static void AttachDoFindConnectionResults(U64 doBld, const BYTE* pf)
{
    if (!g_exeBase || g_exeBase[FN_GAME_OP_NEW_RVA] == 0 ||
        g_exeBase[FN_DO_VEC_PUSH_RVA] == 0 || g_exeBase[FN_DO_CFG_CTOR_RVA] == 0) {
        return;
    }
    U64 resVec[2] = {};
    U64 count = 0;
    if (!ReadPtrVectorRange((U64)(uintptr_t)(pf + 0x1B0), resVec, count)) return;
    const U64 limit = count < 1024u ? count : 1024u;
    int attached = 0;
    for (U64 i = 0; i < limit; ++i) {
        U64 res = 0;
        if (!SafeReadPod((const U64*)(resVec[0] + i * sizeof(U64)), &res)) continue;
        if (AttachSingleDoConnection(doBld, res)) {
            ++attached;
            if (attached > 5) break;
        }
    }
}

static void ExecuteDoFindConnectionsNow(const U64* a)
{
    const U64 world = a[1];
    const U64 doBld = a[2];
    if (world != (U64)World() || !IsPointerLikeX64(doBld)) return;
    if (!g_exeBase || g_exeBase[FN_PF_CTOR_RVA] == 0) return;

    alignas(16) BYTE pf[0x400] = {};
    typedef void* (*PfCtorFn)(void* pfPtr);
    typedef void (*PfDtorFn)(void* pfPtr);
    ((PfCtorFn)(g_exeBase + FN_PF_CTOR_RVA))(pf);

    ClearExistingDoConnections(doBld);
    *(int32_t*)(pf + 0xB4) = 4;
    *(int32_t*)(pf + 0xB0) = 2;
    *(float*)(pf + 0x2C) = 30.0f;
    SeedDoPathfinderStarts(doBld, pf);
    if (g_exeBase[FN_PF_RUN_RVA] != 0) {
        typedef void (*PfRunFn)(void* pfPtr);
        ((PfRunFn)(g_exeBase + FN_PF_RUN_RVA))(pf);
    }
    AttachDoFindConnectionResults(doBld, pf);

    if (g_exeBase[FN_PF_DTOR_RVA] != 0) {
        ((PfDtorFn)(g_exeBase + FN_PF_DTOR_RVA))(pf);
    }
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
    case CALL_DENOMINATION:
        ExecuteDenominationNow(s, a);
        return;
    case CALL_DETACH_PERSONAL_CAR:
        DetachPersonalCarNow(a[0], a[1]);
        return;
    case CALL_WASTE_CONTAINER:
        ExecuteWasteContainerNow(s, a, r);
        return;
    case CALL_SET_FOR_BIO:
        ExecuteSetForBioNow(s, a, r);
        return;
    case CALL_FIRE_DISPATCH:
        ExecuteFireDispatchNow(s, a);
        return;
    case CALL_DISCARD_VEH_CARGO:
        ExecuteDiscardVehCargoNow(s, a);
        return;
    case CALL_EMPTY_STORAGE:
        ExecuteEmptyStorageNow(a);
        return;
    case CALL_CANCEL_CRIME_ASSIGN:
        ExecuteCancelCrimeAssignNow(a);
        return;
    case CALL_DO_FIND_CONNECTIONS:
        ExecuteDoFindConnectionsNow(a);
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
                                            const BYTE* targetBld, unsigned char triggerImmediateScan)
{
    (void)triggerImmediateScan; // 0x161CB0 arg6 only triggers RCO reachability scan, never identifies demolition.
    if (!TwoStates_IsActive()) return true;
    if (targetRva == RVA_ASSIGN_CONSTRUCTION_OFFICE) {
        if (!officeBld || !TwoStates_CheckBuildingControlAllowed(officeBld,
                Tr("назначение чужой стройконторы", "assigning a foreign construction office"))) {
            return false;
        }
        if (TwoStates_IsDemolitionOfficeOrSite(officeBld, targetBld) && targetBld &&
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
                                             const BYTE* targetBld, unsigned char triggerImmediateScan)
{
    return CheckTwoStatesOfficeCallAllowed(targetRva, officeBld, targetBld, triggerImmediateScan);
}

static bool IsAssignCoStartPlanSite(unsigned siteRva)
{
    return siteRva == SITE_ASSIGN_CO_SINGLE_START_RVA ||
           siteRva == SITE_ASSIGN_CO_SINGLE_RAIL_START_RVA ||
           siteRva == SITE_ASSIGN_CO_RECT_START_RVA ||
           siteRva == SITE_ASSIGN_CO_RECT_RAIL_START_RVA;
}

static const BYTE* ResolveAssignCoStartOffice()
{
    if (!g_exeBase) return nullptr;
    U64 toolPtr = 0;
    if (!SafeReadPod((const U64*)(World() + WORLD_ASSIGN_CO_TOOL_OFFSET), &toolPtr) || !IsPointerLikeX64(toolPtr)) {
        return nullptr;
    }
    U64 vec[2] = {};
    if (!SafeMemcpyPod(vec, (const void*)(toolPtr + ASSIGN_CO_TOOL_OFFICES_OFF), sizeof(vec)) ||
        !IsPointerLikeX64(vec[0]) || vec[1] <= vec[0] || ((vec[1] - vec[0]) & 7u) != 0) {
        return nullptr;
    }
    const U64 count = (vec[1] - vec[0]) / sizeof(U64);
    const U64 limit = count < 256u ? count : 256u;
    const int localSt = TwoStates_LocalState();
    const BYTE* firstValidCo = nullptr;
    for (U64 i = 0; i < limit; ++i) {
        U64 coPtr = 0;
        if (!SafeReadPod((const U64*)(vec[0] + i * sizeof(U64)), &coPtr) || !IsPointerLikeX64(coPtr)) continue;
        const BYTE* co = (const BYTE*)coPtr;
        if (!firstValidCo) firstValidCo = co;
        if (TwoStates_GetBuildingOwnerSlot(co) == localSt) return co;
    }
    return firstValidCo;
}

static bool CheckTwoStatesArgsAllowed(const GameCallMsg& m, const U64* args)
{
    if (!TwoStates_IsActive()) return true;
    if (m.target == RVA_ASSIGN_CONSTRUCTION_OFFICE || m.target == RVA_UNASSIGN_CONSTRUCTION_OFFICE) {
        return CheckTwoStatesOfficeCallAllowed(m.target, (const BYTE*)args[1], (const BYTE*)args[2],
                                               (unsigned char)(m.values[5] & 0xFF));
    }
    if (m.index < SITE_COUNT) {
        const unsigned siteRva = kSites[m.index].site;
        if (siteRva == SITE_CONSTRUCT_TOOL_START_PLAN_RVA) {
            const BYTE* activeOffice = nullptr;
            SafeReadPod((const BYTE* const*)(World() + WORLD_ACTIVE_WINDOW_BLD_OFF), &activeOffice);
            return CheckTwoStatesOfficeCallAllowed(m.target, activeOffice, (const BYTE*)args[1], 0);
        }
        if (IsAssignCoStartPlanSite(siteRva)) {
            const BYTE* office = ResolveAssignCoStartOffice();
            return CheckTwoStatesOfficeCallAllowed(m.target, office, (const BYTE*)args[1], 0);
        }
    }
    for (int j = 0; j < MAX_ARGS; j++) {
        if (m.kinds[j] == OBJ_VEHICLE && args[j]) {
            if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)args[j],
                Tr("управление/продажа чужого транспорта", "controlling/selling a foreign vehicle")))
                return false;
        } else if (m.kinds[j] == OBJ_BUILDING && args[j]) {
            const BYTE* b = (const BYTE*)args[j];
            int bldId = 0;
            float bx = 0.0f, bz = 0.0f;
            if (SafeReadPod((const int*)b, &bldId) &&
                SafeReadPod((const float*)(b + 0x350), &bx) &&
                SafeReadPod((const float*)(b + 0x358), &bz) &&
                !TwoStates_CheckBuildingControlAllowed(bldId, bx, bz,
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

static void SkipIfAtReturnRva(uintptr_t* retAddr, unsigned afterRva, unsigned skipRva)
{
    if (retAddr && g_exeBase && *retAddr == (uintptr_t)(g_exeBase + afterRva)) {
        *retAddr = (uintptr_t)(g_exeBase + skipRva);
    }
}

static void SkipCallSitePostMutation(int special, uintptr_t* retAddr)
{
    if (special == CALL_RENAME) {
        SkipIfAtReturnRva(retAddr, SITE_RENAME_AFTER_CALL_RVA, SITE_RENAME_SKIP_MUTATION_RVA);
    } else if (special == CALL_MANUAL_BUY_RES) {
        SkipIfAtReturnRva(retAddr, SITE_MANUAL_BUY_AFTER_RVA, SITE_MANUAL_BUY_SKIP_RVA);
    } else if (special == CALL_AUTO_BUY_RES) {
        SkipIfAtReturnRva(retAddr, SITE_AUTO_BUY_DISABLE_AFTER_RVA, SITE_AUTO_BUY_DISABLE_SKIP_RVA);
        SkipIfAtReturnRva(retAddr, SITE_AUTO_BUY_APPLY_AFTER_RVA, SITE_AUTO_BUY_APPLY_SKIP_RVA);
    } else if (special == CALL_FIRE_DISPATCH) {
        SkipIfAtReturnRva(retAddr, SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA, SITE_FIRESTATION_DISPATCH_HELI_SKIP_RVA);
    } else if (special == CALL_DISCARD_VEH_CARGO) {
        SkipIfAtReturnRva(retAddr, SITE_DISCARD_VEH_CARGO_AFTER_RVA, SITE_DISCARD_VEH_CARGO_SKIP_RVA);
    } else if (special == CALL_CANCEL_CRIME_ASSIGN) {
        SkipIfAtReturnRva(retAddr, SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA, SITE_CANCEL_CRIME_ASSIGN_SKIP_RVA);
    } else if (special == CALL_DO_FIND_CONNECTIONS) {
        SkipIfAtReturnRva(retAddr, SITE_DO_FIND_CONNECTIONS_AFTER_RVA, SITE_DO_FIND_CONNECTIONS_SKIP_RVA);
    }
}

static U64 HandleBuildingResBuyCapture(const GameCallSite& s, const U64* a, uintptr_t* retAddr)
{
    SkipCallSitePostMutation(s.special, retAddr);
    if (TwoStates_IsActive() && a[1]) {
        const BYTE* b = (const BYTE*)a[1];
        int bldId = 0;
        float bx = 0.0f, bz = 0.0f;
        if (SafeReadPod((const int*)b, &bldId) &&
            SafeReadPod((const float*)(b + 0x350), &bx) &&
            SafeReadPod((const float*)(b + 0x358), &bz)) {
            const char* msg = (s.special == CALL_AUTO_BUY_RES)
                ? Tr("настройка автозакупки ресурсов в чужом здании", "configuring resource auto-buy in foreign building")
                : Tr("закупка ресурсов в чужом здании", "purchasing resources in foreign building");
            if (!TwoStates_CheckBuildingControlAllowed(bldId, bx, bz, msg))
                return 0;
        }
    }
    if (s.special == CALL_AUTO_BUY_RES) {
        BuildingSwitches_CaptureAutoBuy(a[0], a[1], (int)a[2], s.worldPointer != 0);
    } else {
        BuildingSwitches_CaptureManualBuy(a[0], a[1], (int)a[2]);
    }
    return 0;
}

static bool HandleWaitingLobbyBlock(const GameCallSite& s, const U64* a, uintptr_t* retAddr, U64* outRet)
{
    if (s.special == CALL_LOCAL) return false;
    bool lobbyActive = SaveSync_IsWaitingLobbyActive() ||
                       (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded());
    if (!lobbyActive) return false;
    g_skipDepotRowCouple = false;
    if (s.special == CALL_DELETE_LINE && a[1]) {
        __try { *(BYTE*)(a[1] + 1) = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    SkipCallSitePostMutation(s.special, retAddr);
    SaveSync_NotifyWaitingLobbyActionBlocked();
    *outRet = s.result;
    return true;
}

static bool HandleDepotRowDrop(const GameCallSite& s, const U64* a, U64* outRet)
{
    if (s.special == CALL_DEPOT_ROW_UNCOUPLE && Commands_TakeUi()) {
        g_skipDepotRowCouple = GameCalls_IsDepotRowSelfDrop(a[1], a[2], a[3], 0);
        if (g_skipDepotRowCouple) {
            *outRet = s.result;
            return true;
        }
    } else if (s.special == CALL_DEPOT_ROW_COUPLE && Commands_TakeUi()) {
        bool skip = g_skipDepotRowCouple || GameCalls_IsDepotRowSelfDrop(a[2], 0, 0, a[1]);
        g_skipDepotRowCouple = false;
        if (skip) {
            *outRet = s.result;
            return true;
        }
    }
    return false;
}

static void SafeInviteCitizensNow(U64 world, U64 building, unsigned mode)
{
    DiagLog_BeginSafeSeh();
    __try {
        InviteCitizensNow(world, building, mode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static U64 SafeExecuteSpecialSiteNow(const GameCallSite& s, const U64* a)
{
    U64 r = 0;
    U64* drag = nullptr;
    U64 dragged = 0;
    DiagLog_BeginSafeSeh();
    __try {
        ExecuteSpecialSiteNow(s, a, r, drag, dragged);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (drag) *drag = dragged;
    }
    DiagLog_EndSafeSeh();
    return r;
}

static bool HandleInviteCitizensUi(const GameCallSite& s, const U64* a, U64* outRet)
{
    if (s.special != CALL_INVITE_CITIZENS) return false;
    if (TwoStates_IsActive() && a[3]) {
        const BYTE* b = (const BYTE*)a[3];
        int bldId = 0;
        float bx = 0.0f, bz = 0.0f;
        if (SafeReadPod((const int*)b, &bldId) &&
            SafeReadPod((const float*)(b + 0x350), &bx) &&
            SafeReadPod((const float*)(b + 0x358), &bz) &&
            !TwoStates_CheckBuildingControlAllowed(bldId, bx, bz,
                Tr("приглашение жителей в чужое здание", "inviting citizens to a foreign building"))) {
            *outRet = 0;
            return true;
        }
    }
    CallSite(s, a); // play click sound locally
    if (!Commands_TakeUi()) {
        SafeInviteCitizensNow(a[0], a[3], s.worldPointer);
        *outRet = 0;
        return true;
    }
    return false;
}

static U64 ExecuteNonCaptureCall(const GameCallSite& s, const U64* a)
{
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
        return SafeExecuteSpecialSiteNow(s, a);
    }
    if (s.special == CALL_SET_FOR_BIO) {
        if (!g_exeBase || g_exeBase[s.target] == 0) return 0;
        typedef float (*CallF4FloatRetFn)(U64, U64, U64, float, U64, U64, U64);
        const float rf = ((CallF4FloatRetFn)(g_exeBase + s.target))(
            a[0], a[1], a[2], AsFloat(a[3]), a[4], a[5], a[6]);
        unsigned bits = 0;
        memcpy(&bits, &rf, 4);
        return (U64)bits;
    }
    if (s.special == CALL_CANCEL_CRIME_ASSIGN) {
        if (!g_exeBase || g_exeBase[s.target] == 0) return 0;
        const U64 soundArgs[MAX_ARGS] = { a[0], a[1], 1, 0, 0, 0, 0 };
        return CallSite(s, soundArgs);
    }
    return CallSite(s, a);
}

static U64 HandleRenameCapture(const GameCallSite& s, const U64* a, uintptr_t* retAddr)
{
    if (Rename_Capture(a[0], a[1], a[2])) {
        SkipCallSitePostMutation(s.special, retAddr);
        return 0;
    }
    return CallSite(s, a);
}

static U64 HandleAutoReplaceClearCapture(const U64* a)
{
    Purchase_SubmitAutoReplaceClear((BYTE*)a[1]);
    return 0;
}

static const char* PrepareCancelCrimeAssignArgs(U64* args)
{
    const U64 world = (U64)World();
    if (args[0] != world || !IsPointerLikeX64(args[2]) || !IsPointerLikeX64(args[3])) {
        return "invalid crime case or building";
    }
    U64 casesVec[2] = {};
    U64 count = 0;
    if (!ReadPtrVectorRange(world + WORLD_CRIME_CASES_BEGIN_OFF, casesVec, count)) {
        return "crime cases vector invalid";
    }
    const U64 limit = count < 65536u ? count : 65536u;
    U64 caseIdx = limit;
    for (U64 i = 0; i < limit; ++i) {
        U64 curCase = 0;
        if (SafeReadPod((const U64*)(casesVec[0] + i * sizeof(U64)), &curCase) && curCase == args[2]) {
            caseIdx = i;
            break;
        }
    }
    if (caseIdx >= limit) return "crime case not found in world";
    unsigned check = 0;
    if (!ComputeCrimeCaseCheck(args[2], check)) return "invalid crime case data";
    args[1] = 0;
    args[2] = caseIdx;
    args[4] = check;
    return nullptr;
}

static const char* PrepareSpecialCaptureArgs(const GameCallSite& s, U64* args)
{
    if (s.special == CALL_DRAG) {
        if (!(args[0] = *(U64*)(g_exeBase + DRAG_RVA))) return "no vehicle is dragged";
    } else if (s.special == CALL_DETACH_PERSONAL_CAR) {
        U64 citizen = args[1];
        U64 veh = 0;
        if (!IsPointerLikeX64(citizen) ||
            !SafeReadPod((const U64*)(citizen + CIT_PERSONAL_CAR_OFFSET), &veh) ||
            !IsPointerLikeX64(veh)) {
            return "citizen has no personal car";
        }
        args[1] = veh;
    } else if (s.special == CALL_WASTE_CONTAINER) {
        unsigned slotIdx = 0;
        if (!FindSlotIndexInBuilding(args[1], args[2], slotIdx)) return "waste slot is not in the building";
        args[2] = slotIdx;
    } else if (s.special == CALL_SET_FOR_BIO) {
        const float maxReq = AsFloat(args[3]);
        if (!isfinite(maxReq) || maxReq <= 0.0f || maxReq >= 1.0e7f) return "invalid bio-waste transfer amount";
        U64 bld = 0;
        unsigned srcIdx = 0, dstIdx = 0;
        if (!FindBuildingOwningSlots(args[1], args[2], bld, srcIdx, dstIdx)) {
            return "bio-waste slots do not belong to a world building";
        }
        args[0] = bld;
        args[1] = srcIdx;
        args[2] = dstIdx;
    } else if (s.special == CALL_FIRE_DISPATCH) {
        if (!IsPointerLikeX64(args[1]) || !IsPointerLikeX64(args[2])) {
            return "invalid fire dispatch target or vehicle";
        }
        const U64 bld = FindBuildingByFireObj(args[1]);
        if (!bld) return "fire target building not found";
        args[0] = 0;
        args[1] = bld;
    } else if (s.special == CALL_EMPTY_STORAGE) {
        if (!IsPointerLikeX64(args[0])) return "invalid storage slot";
        if (!CanDiscardStorageSlot(args[0])) return "storage cannot be emptied";
        U64 bld = 0;
        unsigned slotIdx = 0;
        if (!FindBuildingOwningSingleSlot(args[0], bld, slotIdx)) {
            return "storage slot does not belong to a world building";
        }
        args[0] = bld;
        args[1] = slotIdx;
    } else if (s.special == CALL_CANCEL_CRIME_ASSIGN) {
        return PrepareCancelCrimeAssignArgs(args);
    } else if (s.special == CALL_DO_FIND_CONNECTIONS) {
        if (!IsPointerLikeX64(args[2])) return "invalid distribution office building";
    }
    return nullptr;
}

static const char* CaptureSiteArgs(unsigned index, const U64* inArgs, U64* outArgs, GameCallMsg& m)
{
    memcpy(outArgs, inArgs, MAX_ARGS * sizeof(U64));
    const char* why = PrepareSpecialCaptureArgs(kSites[index], outArgs);
    if (!why) why = Capture(index, outArgs, m);
    return why;
}

static U64 CaptureAndSubmitPlainCall(unsigned index, const GameCallSite& s, const U64* a, uintptr_t* retAddr)
{
    U64 args[MAX_ARGS] = {};
    GameCallMsg m;
    const char* why = nullptr;
    bool refusedByTwoStates = false;
    __try {
        why = CaptureSiteArgs(index, a, args, m);
        if (!why && !CheckTwoStatesArgsAllowed(m, args)) refusedByTwoStates = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the arguments";
    }
    SkipCallSitePostMutation(s.special, retAddr);
    if (refusedByTwoStates) return s.result;
    if (why)
        Logf("republicmp  GAME CALL: %s not shared - %s; nothing is done", s.name, why);
    else {
        PrepareUiSpecialCapture(s, a);
        Commands_Submit(LS_CMD_GAME_CALL, &m, sizeof(m));
    }
    return s.result;
}

static U64 Run(unsigned index, const U64* a, uintptr_t* retAddr = nullptr)
{
    const GameCallSite& s = kSites[index];
    U64 outRet = s.result;
    if (HandleWaitingLobbyBlock(s, a, retAddr, &outRet)) return outRet;
    if (HandleDepotRowDrop(s, a, &outRet)) return outRet;
    if (HandleInviteCitizensUi(s, a, &outRet)) return outRet;
    if (!Commands_TakeUi()) return ExecuteNonCaptureCall(s, a);

    switch (s.special) {
    case CALL_PRESENTATION:
        return s.result;
    case CALL_LOCAL:
        return CallSite(s, a);
    case CALL_RENAME:
        return HandleRenameCapture(s, a, retAddr);
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
    case CALL_AUTO_BUY_RES:
        return HandleBuildingResBuyCapture(s, a, retAddr);
    case CALL_AUTO_REPLACE_CLEAR:
        return HandleAutoReplaceClearCapture(a);
    case CALL_GAME_RULES_REBUILD_STORAGES:
        return s.result;
    default:
        return CaptureAndSubmitPlainCall(index, s, a, retAddr);
    }
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

template <unsigned I>
static float ThunkFloatRet(U64 a1, U64 a2, U64 a3, float a4, U64 a5, U64 a6, U64 a7)
{
    unsigned bits = 0;
    memcpy(&bits, &a4, 4);
    U64 a[MAX_ARGS] = { a1, a2, a3, bits, a5, a6, a7 };
    return AsFloat(Run(I, a));
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

template <size_t... I>
static constexpr std::array<void*, sizeof...(I)> MakeThunksFloatRet(std::index_sequence<I...>)
{
    return { { (void*)&ThunkFloatRet<(unsigned)I>... } };
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

static const size_t kMaxOfficeReorderCount = 256;

#pragma pack(push, 1)
struct OfficeReorderHeader {
    RouteRef office;
    unsigned short count;
};
#pragma pack(pop)

typedef void (__fastcall* OfficeReorderFn)(BYTE* itemsVec, BYTE* office);
static OfficeReorderFn g_origOfficeQueueReorder = nullptr;

static const unsigned char kOfficeReorderExpect[14] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x4B, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };

bool GameCalls_ApplyOfficeQueuePermutation(unsigned long long* sites, float* dists, size_t count,
                                           const unsigned short* newOrder, size_t orderCount)
{
    if (!sites || !dists || !newOrder || count == 0 || count != orderCount || count > kMaxOfficeReorderCount) {
        return false;
    }
    unsigned long long tmpSites[kMaxOfficeReorderCount] = {};
    float tmpDists[kMaxOfficeReorderCount] = {};
    bool used[kMaxOfficeReorderCount] = {};
    for (size_t i = 0; i < count; ++i) {
        tmpSites[i] = sites[i];
        tmpDists[i] = dists[i];
    }

    size_t out = 0;
    for (size_t i = 0; i < count && out < count; ++i) {
        const size_t idx = (size_t)newOrder[i];
        if (idx < count && !used[idx]) {
            sites[out] = tmpSites[idx];
            dists[out] = tmpDists[idx];
            used[idx] = true;
            ++out;
        }
    }
    for (size_t i = 0; i < count && out < count; ++i) {
        if (!used[i]) {
            sites[out] = tmpSites[i];
            dists[out] = tmpDists[i];
            ++out;
        }
    }
    return out == count;
}

static bool ReadOfficeAssignedVectors(const BYTE* office, U64 outSiteVec[2], U64 outDistVec[2], U64& outCount)
{
    outCount = 0;
    if (!office || !IsPointerLikeX64((uintptr_t)office)) return false;
    U64 siteCount = 0;
    U64 distCount = 0;
    const U64 base = (U64)(uintptr_t)office;
    if (!ReadVectorByteRange(base + BLD_ASSIGNED_SITES_BEGIN_OFF, sizeof(U64), outSiteVec, siteCount) ||
        !ReadVectorByteRange(base + BLD_ASSIGNED_DISTS_BEGIN_OFF, sizeof(float), outDistVec, distCount)) {
        return false;
    }
    if (siteCount == 0 || siteCount > kMaxOfficeReorderCount || siteCount != distCount) {
        return false;
    }
    outCount = siteCount;
    return true;
}

static bool ParseOfficeReorderItems(const BYTE* itemsVec, U64 expectedCount, unsigned short* outOrder, bool& outChanged)
{
    outChanged = false;
    if (!itemsVec || !IsPointerLikeX64((uintptr_t)itemsVec) || !outOrder) return false;
    U64 vec[2] = {};
    U64 itemCount = 0;
    if (!ReadVectorByteRange((U64)(uintptr_t)itemsVec, OFFICE_REORDER_ITEM_STRIDE, vec, itemCount) ||
        itemCount != expectedCount) {
        return false;
    }
    for (U64 i = 0; i < itemCount; ++i) {
        float rawIdx = -1.0f;
        if (!SafeReadPod((const void*)(vec[0] + i * OFFICE_REORDER_ITEM_STRIDE + OFFICE_REORDER_ITEM_IDX_OFF), &rawIdx) ||
            !isfinite(rawIdx)) {
            return false;
        }
        const int idx = (int)rawIdx;
        if (idx < 0 || (U64)idx >= itemCount) return false;
        outOrder[i] = (unsigned short)idx;
        if ((U64)idx != i) outChanged = true;
    }
    return true;
}

static bool BuildOfficeReorderPayload(const BYTE* itemsVec, const BYTE* office,
                                      BYTE* outBuf, size_t maxBytes, size_t* outBytes, bool* outChanged)
{
    if (outBytes) *outBytes = 0;
    if (outChanged) *outChanged = false;
    if (!outBuf || maxBytes < sizeof(OfficeReorderHeader)) return false;

    U64 siteVec[2] = {};
    U64 distVec[2] = {};
    U64 count = 0;
    if (!ReadOfficeAssignedVectors(office, siteVec, distVec, count)) return false;

    const size_t needBytes = sizeof(OfficeReorderHeader) + (size_t)count * sizeof(unsigned short);
    if (maxBytes < needBytes) return false;

    unsigned short order[kMaxOfficeReorderCount] = {};
    bool changed = false;
    if (!ParseOfficeReorderItems(itemsVec, count, order, changed)) return false;

    OfficeReorderHeader hdr = {};
    hdr.count = (unsigned short)count;
    if (!Ref_Building((U64)(uintptr_t)office, hdr.office)) return false;

    memcpy(outBuf, &hdr, sizeof(hdr));
    memcpy(outBuf + sizeof(hdr), order, (size_t)count * sizeof(unsigned short));
    if (outBytes) *outBytes = needBytes;
    if (outChanged) *outChanged = changed;
    return true;
}

bool GameCalls_BuildOfficeReorderPayloadForTest(const BYTE* itemsVec, const BYTE* office,
                                                void* outBuf, size_t maxBytes, size_t* outBytes)
{
    bool changed = false;
    return BuildOfficeReorderPayload(itemsVec, office, (BYTE*)outBuf, maxBytes, outBytes, &changed) && changed;
}

static void FreeOfficeReorderList(BYTE* itemsVec)
{
    if (!g_exeBase || g_exeBase[RVA_OFFICE_REORDER_FREE_LIST] == 0 || !IsPointerLikeX64((uintptr_t)itemsVec)) {
        return;
    }
    DiagLog_BeginSafeSeh();
    __try {
        ((void (__fastcall*)(BYTE*))(g_exeBase + RVA_OFFICE_REORDER_FREE_LIST))(itemsVec);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void __fastcall H_OfficeQueueReorder(BYTE* itemsVec, BYTE* office)
{
    if (!Commands_TakeUi()) {
        if (g_origOfficeQueueReorder) {
            g_origOfficeQueueReorder(itemsVec, office);
        } else {
            FreeOfficeReorderList(itemsVec);
        }
        return;
    }
    const bool allowed = IsPointerLikeX64((uintptr_t)office) &&
        (!TwoStates_IsActive() || TwoStates_CheckBuildingControlAllowed(
            office, Tr("изменение очереди чужой стройконторы", "reordering a foreign construction office queue")));
    if (allowed) {
        BYTE buf[sizeof(OfficeReorderHeader) + kMaxOfficeReorderCount * sizeof(unsigned short)] = {};
        size_t payloadBytes = 0;
        bool changed = false;
        if (BuildOfficeReorderPayload(itemsVec, office, buf, sizeof(buf), &payloadBytes, &changed) && changed) {
            Commands_Submit(LS_CMD_OFFICE_REORDER, buf, payloadBytes);
        }
    }
    FreeOfficeReorderList(itemsVec);
}

bool GameCalls_ApplyOfficeReorderCommand(const void* data, size_t size)
{
    if (!data || size < sizeof(OfficeReorderHeader)) return false;
    OfficeReorderHeader hdr = {};
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.count == 0 || hdr.count > kMaxOfficeReorderCount) return false;
    if (size != sizeof(OfficeReorderHeader) + (size_t)hdr.count * sizeof(unsigned short)) return false;

    const U64 officeAddr = Ref_BuildingAt(hdr.office);
    if (!officeAddr) {
        Logf("republicmp  GAME CALL: office queue reorder - office is gone");
        return true;
    }
    if (TwoStates_IsActive()) {
        const int cmdOwner = TwoStates_GetCommandOwner();
        if ((cmdOwner == 1 || cmdOwner == 2) &&
            TwoStates_GetBuildingOwnerSlot((const BYTE*)officeAddr) != cmdOwner) {
            return true;
        }
    }

    U64 siteVec[2] = {};
    U64 distVec[2] = {};
    U64 vecCount = 0;
    if (!ReadOfficeAssignedVectors((const BYTE*)officeAddr, siteVec, distVec, vecCount) ||
        vecCount != hdr.count) {
        Logf("republicmp  GAME CALL: office queue reorder (%u sites) - queue size mismatch, skipped",
             (unsigned)hdr.count);
        return true;
    }

    U64 sites[kMaxOfficeReorderCount] = {};
    float dists[kMaxOfficeReorderCount] = {};
    unsigned short order[kMaxOfficeReorderCount] = {};
    memcpy(order, (const BYTE*)data + sizeof(hdr), (size_t)hdr.count * sizeof(unsigned short));
    if (!SafeMemcpyPod(sites, (const void*)siteVec[0], (size_t)hdr.count * sizeof(U64)) ||
        !SafeMemcpyPod(dists, (const void*)distVec[0], (size_t)hdr.count * sizeof(float))) {
        return true;
    }
    if (!GameCalls_ApplyOfficeQueuePermutation(sites, dists, hdr.count, order, hdr.count)) {
        return true;
    }
    SafeMemcpyPod((void*)siteVec[0], sites, (size_t)hdr.count * sizeof(U64));
    SafeMemcpyPod((void*)distVec[0], dists, (size_t)hdr.count * sizeof(float));
    Logf("republicmp  GAME CALL: office queue reorder (%u sites): applied", (unsigned)hdr.count);
    return true;
}

static void EmitRelay20(BYTE* r, const BYTE prefix[6], void* to)
{
    memcpy(r, prefix, 6);
    r[6] = 0xFF;
    r[7] = 0x25;
    memset(r + 8, 0, 4);
    memcpy(r + 12, &to, 8);
}

bool GameCalls_Install()
{
    static const auto thunks = MakeThunks(std::make_index_sequence<SITE_COUNT>());
    static const auto thunksF4 = MakeThunksF4(std::make_index_sequence<SITE_COUNT>());
    static const auto thunksFloatRet = MakeThunksFloatRet(std::make_index_sequence<SITE_COUNT>());
    static const BYTE kDepotUncouplePrefix[6] = { 0x4D, 0x89, 0xF8, 0x4D, 0x89, 0xF1 };
    static const BYTE kCancelCrimePrefix[6]   = { 0x4D, 0x89, 0xE0, 0x4D, 0x89, 0xF1 };
    static const BYTE kDoFindConnPrefix[6]    = { 0x4C, 0x89, 0xF2, 0x4D, 0x89, 0xE8 };
    static_assert(SITE_COUNT * 32 <= 16384, "kSites relay allocation overflow");
    BYTE* relay = AllocNear(g_exeBase, 16384);
    if (!relay) {
        Logf("republicmp  GAME CALL: no relay page near the executable - window buttons are not shared");
        return false;
    }
    DWORD old;
    VirtualProtect(relay, SITE_COUNT * 32, PAGE_EXECUTE_READWRITE, &old);
    for (size_t i = 0; i < SITE_COUNT; i++) {
        BYTE* r = relay + i * 32;
        void* to = (kSites[i].special == CALL_SET_FOR_BIO)
            ? thunksFloatRet[i]
            : (Float4(kSites[i]) ? thunksF4[i] : thunks[i]);
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
            EmitRelay20(r, kDepotUncouplePrefix, to);
        } else if (kSites[i].special == CALL_CANCEL_CRIME_ASSIGN) {
            EmitRelay20(r, kCancelCrimePrefix, to);
        } else if (kSites[i].special == CALL_DO_FIND_CONNECTIONS) {
            EmitRelay20(r, kDoFindConnPrefix, to);
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
        on++;
    }
    if (g_exeBase && !InstallInlineHook(g_exeBase + RVA_OFFICE_QUEUE_REORDER, (void*)&H_OfficeQueueReorder,
                                        (void**)&g_origOfficeQueueReorder, kOfficeReorderExpect,
                                        sizeof(kOfficeReorderExpect), "republicmp office_queue_reorder")) {
        Logf("republicmp  GAME CALL: office_queue_reorder hook left uninstalled");
    }
    Logf("republicmp  GAME CALL: %d of %d window call sites relayed", on, (int)SITE_COUNT);
    return on == (int)SITE_COUNT;
}

bool GameCalls_HasCallSiteForTest(unsigned atSiteRva, unsigned targetRoutineRva)
{
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (kSites[i].site == atSiteRva && kSites[i].target == targetRoutineRva) return true;
    }
    return false;
}

static int FindSiteIndexByRva(unsigned atSiteRva)
{
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (kSites[i].site == atSiteRva) return (int)i;
    }
    return -1;
}

const char* GameCalls_GetSiteSchemaForTest(unsigned atSiteRva)
{
    const int idx = FindSiteIndexByRva(atSiteRva);
    return (idx >= 0) ? kSites[idx].schema : nullptr;
}

bool GameCalls_CheckTwoStatesArgsForTest(unsigned atSiteRva, const unsigned long long* args)
{
    const int idx = FindSiteIndexByRva(atSiteRva);
    if (idx < 0 || !args) return false;
    U64 prepared[MAX_ARGS] = {};
    GameCallMsg m = {};
    if (CaptureSiteArgs((unsigned)idx, args, prepared, m) != nullptr) return false;
    return CheckTwoStatesArgsAllowed(m, prepared);
}

bool GameCalls_CaptureAndRebuildForTest(unsigned atSiteRva, const unsigned long long* inArgs, unsigned long long* outArgs)
{
    const int idx = FindSiteIndexByRva(atSiteRva);
    if (idx < 0 || !inArgs || !outArgs) return false;
    U64 prepared[MAX_ARGS] = {};
    GameCallMsg m = {};
    if (CaptureSiteArgs((unsigned)idx, inArgs, prepared, m) != nullptr) return false;
    return Rebuild(m, outArgs) == nullptr;
}

bool GameCalls_CaptureMsgForTest(unsigned atSiteRva, const unsigned long long* inArgs,
                                 void* outMsg, size_t maxBytes, size_t* outSize)
{
    const int idx = FindSiteIndexByRva(atSiteRva);
    if (idx < 0 || !inArgs || !outMsg || maxBytes < sizeof(GameCallMsg)) return false;
    U64 prepared[MAX_ARGS] = {};
    GameCallMsg m = {};
    if (CaptureSiteArgs((unsigned)idx, inArgs, prepared, m) != nullptr) return false;
    memcpy(outMsg, &m, sizeof(m));
    if (outSize) *outSize = sizeof(m);
    return true;
}

bool GameCalls_RunSiteWithRetAddrForTest(unsigned atSiteRva, const unsigned long long* args,
                                         unsigned long long* outRet, unsigned long long* inOutRetAddr)
{
    const int idx = FindSiteIndexByRva(atSiteRva);
    if (idx < 0 || !args) return false;
    uintptr_t retAddr = inOutRetAddr ? (uintptr_t)*inOutRetAddr : 0;
    const U64 ret = Run((unsigned)idx, args, inOutRetAddr ? &retAddr : nullptr);
    if (outRet) *outRet = ret;
    if (inOutRetAddr) *inOutRetAddr = (unsigned long long)retAddr;
    return true;
}

bool GameCalls_RunSiteForTest(unsigned atSiteRva, const unsigned long long* args, unsigned long long* outRet)
{
    return GameCalls_RunSiteWithRetAddrForTest(atSiteRva, args, outRet, nullptr);
}
