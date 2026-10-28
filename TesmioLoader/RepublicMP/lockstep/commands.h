// SPDX-License-Identifier: GPL-3.0-only
// Lockstep commands: player actions made at a tick boundary on every PC.
//
// While the determinism layer runs the simulation in whole ticks
// (determinism/), an action of the local player must not change the world
// in the middle of a frame on this PC only. The hooks that see the local
// actions (world_sync/local_builds: building placement and every drag tool's
// commit; world_sync/buildings/demolish_hook: building demolition) hand them
// here instead of letting the game make them, and the action is made later
// through the same replay the receiving PC of the replay multiplayer uses -
// on this PC as well, at the start of a frame, before the next tick runs
// (Determinism_BeforePrepare). Every PC that makes the same commands after the
// same tick reaches the same world.
//
// Tools act privately on every frame (a held Late, and the tool scope around
// Prepare and the tool handlers on a tick's frame; see sim_patches.cpp); the
// probe still waits while one is selected ("tool hold", by SoylentGreed).
//
// In a lockstep session (lockstep/lockstep_net) the host orders the commands
// of both players: it makes each one after its current tick and sends it on;
// the player sends its own to the host and makes the ones it receives after
// the tick they name. The determinism probe uses them on one PC: it records
// them in run 1 (the player builds while it runs) and makes them again in
// run 2 after the same ticks, with no input; both runs must stay identical.
#pragma once

#include "tesmio_plugin.h"

enum LockstepCommandKind {
    LS_CMD_SEGMENT = 1,      // RoadSyncMsg: a drag tool's finished segment
    LS_CMD_UPGRADE = 2,      // RoadSyncUpgradeMsg: re-typing existing pieces
    LS_CMD_NET_DEMOLISH = 3, // RoadSyncDemolishMsg: the demolition tool on a network
    LS_CMD_PLACE = 4,        // BuildSyncMsg: building placement inputs
    LS_CMD_DEMOLISH = 5,     // BuildSyncMsg: building demolition
    LS_CMD_SETTING = 6,      // SettingWire: a building window setting, pay or stop flags
    LS_CMD_PLAN_START = 7,   // RoadSyncPlanStartMsg: start planned construction
    LS_CMD_GROUP_TOOL = 8,   // RoadSyncGroupToolMsg: a group tool's commit
    LS_CMD_DIRECTION = 9,    // RoadSyncDirectionMsg: a factory connection's direction
    LS_CMD_TERRAIN = 10,     // terrain edits of one frame (terrain_hook)
    LS_CMD_MONEY = 11,       // LsMoneyDelta: what a local action paid or got
    LS_CMD_PURCHASE = 12,    // purchase.cpp: a vehicle, licence or blueprint bought in a purchase window
    LS_CMD_ROUTE = 13,       // route_tools.cpp: a stop added to a schedule, a vehicle put on a line, sent to a depot
    LS_CMD_GAME_CALL = 14,   // game_calls.cpp: a window button's routine call
    LS_CMD_LINE_ACCEPT = 15, // line_accept.cpp: the line window's Accept with its working copy
    LS_CMD_VEHICLE_SWITCH = 16, // vehicle_switches.cpp: vehicle words a window click changed
    LS_CMD_BUILDING_SWITCH = 17, // building_switches.cpp: building, storage slot or world setting words a click changed
    LS_CMD_BRUSH = 18,       // brush_tools.cpp: one frame of a terrain brush or a single-tree tool
    LS_CMD_HOVER = 19,       // hover_tools.cpp: a click of a road sign, waypoint or rail signal tool
    LS_CMD_JUNCTION = 20,    // junction_accept.cpp: the junction window's Accept with its working copy
    LS_CMD_GAME_RULES = 21,  // commands.cpp: the game menu's settings page applied (its copy of the rules)
    LS_CMD_CARGO_SETTINGS = 22, // cargo_settings.cpp: a stop's or link's loading or unloading half
    LS_CMD_SOURCE_SLOTS = 23,   // source_slots.cpp: a building's specific sources (add-source tool, windows)
    LS_CMD_RENAME = 24,         // rename.cpp: a city's or a crossroad's new name
    LS_CMD_LEDGER = 25,         // brush_tools.cpp: the construction ledger row a terrain stroke wrote
    LS_CMD_STATE_TRANSFER = 26, // two_states.cpp: inter-state money transfer between West and East Republics
    LS_CMD_VEHICLE_SOFT_SYNC = 27, // vehicle_soft_sync.cpp: authoritative soft vehicle sync (Host -> Client)
    LS_CMD_LOAN = 28,           // two_states.cpp: bank loan borrow / partial repay / full repay
};

// Money a local action changed on this PC (an interface purchase, a terrain
// tool's cost): put back here and added on every PC by the command.
struct LsMoneyDelta {
    double rubles;
    double dollars;
};

void Commands_Bind(const TsmHost* host);

// True while local actions become commands instead of acting at once.
bool Commands_Capturing();

// For a hook: should the local action seen now become a command? False on
// the simulation's own calls (anything during a step frame - tools are
// hidden then) and while a command is being made.
bool Commands_TakeLocal();

// For a window's call (the interface, any thread): should it become a command?
// False only while this thread makes a command.
bool Commands_TakeUi();

// game_calls: the settings page applies its copy of the game rules (337170).
void Commands_CaptureGameRules();
// In a session: cheats off, and no save and reload after a critical rule
// (called before the frame's commands and after the interface).
void Commands_GuardSettings();
// Clears world cheat/dev flags (+0x9, +0xA) and closes cheat systab sub-pages (+0xC65C == 5..7).
bool Commands_SanitizeCheatState(BYTE* world);

// Counts the commands made so far (any thread): a comparison around the
// interface that saw it change knows a command, not the interface, wrote.
unsigned long long Commands_ApplyEpoch();

// Any thread: queues a captured action; it is made at the next boundary.
void Commands_Submit(int kind, const void* data, size_t size);

// Changes made outside the hooks, found by comparing with a record (main
// thread, while commands are captured): what the interface changed between
// the end of a frame and the start of the next (AfterFrame records, Collect
// compares - building settings, construction pay/stop flags, money), and what
// a tool changed inside a held frame's Late body (LateBegin records, LateEnd
// compares - the same plus factory connection directions). Each change is put
// back on this PC and becomes a command. Collect also hands over the terrain
// edits a tool made and puts away a selected tool whose actions are not
// carried yet.
void Commands_Collect();
void Commands_AfterFrame(bool tickRan);
void Commands_LateBegin();
void Commands_LateEnd();

// Money between the frame start and Late (main thread): RecordMoney takes the
// balances once the frame's commands are made; CheckMoney turns a change since
// then (this PC's view preparation or held drawing) into a command.
void Commands_RecordMoney();
void Commands_CheckMoney(const char* where);

// Main thread, at the start of a frame, with `completed` ticks run: queued
// actions get this boundary, and every command due here is made.
void Commands_Boundary(unsigned long long completed);
// True when Commands_Boundary has something to make at `completed`.
bool Commands_Due(unsigned long long completed);

// Probe (main thread, right after a run's load): run 0 with `record` captures
// and records; run 1 makes the recorded commands again. record = false: off.
void Commands_ProbeRun(int run, bool record);
// After the probe: capture off, record dropped. Returns the commands made in
// run 2 that were recorded in run 1 (all of them when the replay was complete).
void Commands_ProbeEnd(size_t* recorded, size_t* replayed);

// Lockstep session (main thread, after the session's load): 0 off, 1 host,
// 2 player. Capture is on while a session runs.
enum { COMMANDS_OFF = 0, COMMANDS_HOST = 1, COMMANDS_PLAYER = 2 };
void Commands_SetSession(int role);
// Host, game thread: a command the player sent; ordered at the next boundary.
void Commands_SubmitRemote(int kind, const void* data, size_t size);
// Player, game thread: a command the host made after `afterTick`.
void Commands_Receive(int kind, unsigned long long afterTick, const void* data, size_t size);
