// SPDX-License-Identifier: GPL-3.0-only
// See commands.h.
#include "commands.h"
#include "../determinism/determinism.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/replay_menus.h"
#include "../world_sync/local_builds.h"
#include "../world_sync/vehicles/vehicle_soft_sync.h"
#include "lockstep_net.h"
#include "../game/input_block.h"
#include "../session/settings_sync.h"
#include "../world_sync/terrain/terrain_hook.h"
#include "purchase.h"
#include "route_tools.h"
#include "game_calls.h"
#include "line_accept.h"
#include "world_refs.h"
#include "vehicle_switches.h"
#include "building_switches.h"
#include "brush_tools.h"
#include "hover_tools.h"
#include "junction_accept.h"
#include "cargo_settings.h"
#include "source_slots.h"
#include "rename.h"
#include "../session/two_states.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <vector>
#include <string.h>
#include <math.h>

void Commands_Bind(const TsmHost* host)
{
    TsmBind(host);
    Purchase_Bind(host);
    RouteTools_Bind(host);
    GameCalls_Bind(host);
    LineAccept_Bind(host);
    WorldRefs_Bind(host);
    VehicleSwitches_Bind(host);
    BuildingSwitches_Bind(host);
    BrushTools_Bind(host);
    HoverTools_Bind(host);
    JunctionAccept_Bind(host);
    CargoSettings_Bind(host);
    SourceSlots_Bind(host);
    Rename_Bind(host);
    VehicleSoftSync_Bind(host);
}

struct Command {
    int kind;
    int owner;
    unsigned long long afterTick;
    unsigned seq;
    std::vector<BYTE> data;
};

static INIT_ONCE g_commandsLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_commandsLock;
static std::vector<Command> g_outbox;    // captured, not yet given a boundary
static std::vector<Command> g_record;    // probe run 1: made, in order
static size_t g_replayAt = 0;            // probe run 2: next recorded command
static size_t g_replayed = 0;
static volatile bool g_capturing = false;
static bool ApplyGameRules(const void* data, size_t size);
static bool g_replaying = false;
static volatile LONG g_applying = 0;
static DWORD g_applyThread = 0;
static volatile LONG64 g_applyEpoch = 0;
static unsigned g_seq = 0;
static int g_session = COMMANDS_OFF; // in a session the player keeps the host's commands in g_record

static void Lock()
{
    TsmEnsureCriticalSection(&g_commandsLockOnce, &g_commandsLock);
    EnterCriticalSection(&g_commandsLock);
}

static void Unlock()
{
    LeaveCriticalSection(&g_commandsLock);
}

bool Commands_Capturing()
{
    return g_capturing;
}

bool Commands_TakeLocal()
{
    return g_capturing && g_applying == 0 && !Determinism_InStepFrame();
}

unsigned long long Commands_ApplyEpoch()
{
    return (unsigned long long)InterlockedCompareExchange64(&g_applyEpoch, 0, 0);
}

bool Commands_TakeUi()
{
    return g_capturing && !(g_applying != 0 && GetCurrentThreadId() == g_applyThread);
}

static const char* const kCmdKindNames[] = {
    "?",                              // 0
    "segment",                        // LS_CMD_SEGMENT (1)
    "upgrade",                        // LS_CMD_UPGRADE (2)
    "network demolition",             // LS_CMD_NET_DEMOLISH (3)
    "placement",                      // LS_CMD_PLACE (4)
    "building demolition",            // LS_CMD_DEMOLISH (5)
    "building setting",               // LS_CMD_SETTING (6)
    "start of planned construction",  // LS_CMD_PLAN_START (7)
    "group tool",                     // LS_CMD_GROUP_TOOL (8)
    "factory connection direction",   // LS_CMD_DIRECTION (9)
    "terrain edits",                  // LS_CMD_TERRAIN (10)
    "money",                          // LS_CMD_MONEY (11)
    "purchase",                       // LS_CMD_PURCHASE (12)
    "route tool",                     // LS_CMD_ROUTE (13)
    "window button",                  // LS_CMD_GAME_CALL (14)
    "line accept",                    // LS_CMD_LINE_ACCEPT (15)
    "vehicle setting",                // LS_CMD_VEHICLE_SWITCH (16)
    "window setting",                 // LS_CMD_BUILDING_SWITCH (17)
    "brush",                          // LS_CMD_BRUSH (18)
    "road hover tool",                // LS_CMD_HOVER (19)
    "junction accept",                // LS_CMD_JUNCTION (20)
    "game rules",                     // LS_CMD_GAME_RULES (21)
    "cargo settings",                 // LS_CMD_CARGO_SETTINGS (22)
    "source slots",                   // LS_CMD_SOURCE_SLOTS (23)
    "rename",                         // LS_CMD_RENAME (24)
    "ledger row",                     // LS_CMD_LEDGER (25)
    "inter-state money transfer",     // LS_CMD_STATE_TRANSFER (26)
    "vehicle soft sync",              // LS_CMD_VEHICLE_SOFT_SYNC (27)
    "bank loan",                      // LS_CMD_LOAN (28)
};

static const char* KindName(int kind)
{
    const int n = (int)(sizeof(kCmdKindNames) / sizeof(kCmdKindNames[0]));
    return (kind > 0 && kind < n) ? kCmdKindNames[kind] : "?";
}

// Terrain brushes, their cost, and background soft-sync batches come frequently: not logged one by one.
static bool Frequent(int kind)
{
    return kind == LS_CMD_TERRAIN || kind == LS_CMD_MONEY || kind == LS_CMD_BRUSH || kind == LS_CMD_LEDGER ||
           kind == LS_CMD_VEHICLE_SOFT_SYNC;
}

void Commands_Submit(int kind, const void* data, size_t size)
{
    if (!g_capturing) return;
    Command c;
    c.kind = kind;
    c.owner = (g_session == COMMANDS_PLAYER) ? 2 : 1;
    c.afterTick = 0;
    c.seq = 0;
    c.data.assign((const BYTE*)data, (const BYTE*)data + size);
    Lock();
    g_outbox.push_back(std::move(c));
    Unlock();
    if (!Frequent(kind)) Logf("republicmp  LOCKSTEP: %s captured - made at the next boundary", KindName(kind));
}

// The republic's balances: world +0x588 rubles, +0x580 dollars (doubles).
#define MONEY_RUB_RVA (0x9D4F10 + 0x588)
#define MONEY_USD_RVA (0x9D4F10 + 0x580)
static double* Rubles() { return (double*)(g_exeBase + MONEY_RUB_RVA); }
static double* Dollars() { return (double*)(g_exeBase + MONEY_USD_RVA); }

static bool ApplyMoney(const LsMoneyDelta& d)
{
    *Rubles() += d.rubles;
    *Dollars() += d.dollars;
    return true;
}

static bool g_madeThisFrame = false;

static bool ApplySegmentCommand(const void* p, size_t n)
{
    if (n < ROAD_SYNC_MSG_HEADER_BYTES || n > sizeof(RoadSyncMsg)) return false;
    static RoadSyncMsg s_segMsg;
    memset(&s_segMsg, 0, sizeof(s_segMsg));
    memcpy(&s_segMsg, p, n);
    if (!RoadSync_PointCountValid(s_segMsg.seg.pointCount)) return false;
    if (n != sizeof(RoadSyncMsg) && n != ROAD_SYNC_MSG_HEADER_BYTES + RoadSync_PointBytes(s_segMsg.seg.pointCount))
        return false;
    return RoadSync_ApplyCommandSegment(s_segMsg);
}

static bool ApplyNetDemolishCommand(const void* p, size_t n)
{
    if (n == sizeof(RoadSyncDemolishMsg)) {
        const RoadSyncDemolishMsg& m = *(const RoadSyncDemolishMsg*)p;
        bool ok = RoadSync_ApplyCommandDemolish(m);
        LocalBuilds_DemolishMade(m);
        return ok;
    }
    if (n == sizeof(LsDemolishHoverMsg)) {
        const LsDemolishHoverMsg& h = *(const LsDemolishHoverMsg*)p;
        bool ok = RoadSync_ApplyCommandDemolishHover(h);
        LocalBuilds_DemolishMade(h.m, &h.hover);
        return ok;
    }
    return false;
}

typedef bool (*RawCmdHandlerFn)(const void*, size_t);

struct RawCmdHandlerEntry {
    int kind;
    RawCmdHandlerFn fn;
};

static const RawCmdHandlerEntry kRawCmdHandlers[] = {
    { LS_CMD_SEGMENT, ApplySegmentCommand },
    { LS_CMD_NET_DEMOLISH, ApplyNetDemolishCommand },
    { LS_CMD_TERRAIN, TerrainHook_ApplyCommand },
    { LS_CMD_PURCHASE, Purchase_ApplyCommand },
    { LS_CMD_ROUTE, RouteTools_ApplyCommand },
    { LS_CMD_GAME_CALL, GameCalls_ApplyCommand },
    { LS_CMD_LINE_ACCEPT, LineAccept_ApplyCommand },
    { LS_CMD_VEHICLE_SWITCH, VehicleSwitches_ApplyCommand },
    { LS_CMD_BUILDING_SWITCH, BuildingSwitches_ApplyCommand },
    { LS_CMD_BRUSH, BrushTools_ApplyCommand },
    { LS_CMD_HOVER, HoverTools_ApplyCommand },
    { LS_CMD_JUNCTION, JunctionAccept_ApplyCommand },
    { LS_CMD_GAME_RULES, ApplyGameRules },
    { LS_CMD_CARGO_SETTINGS, CargoSettings_ApplyCommand },
    { LS_CMD_SOURCE_SLOTS, SourceSlots_ApplyCommand },
    { LS_CMD_RENAME, Rename_ApplyCommand },
    { LS_CMD_LEDGER, BrushTools_ApplyLedger },
    { LS_CMD_VEHICLE_SOFT_SYNC, VehicleSoftSync_ApplyCommand },
};

static bool DispatchCommandPayload(int kind, const void* p, size_t n)
{
    for (const RawCmdHandlerEntry& entry : kRawCmdHandlers) {
        if (entry.kind == kind) return entry.fn(p, n);
    }
    switch (kind) {
    case LS_CMD_UPGRADE:
        return n == sizeof(RoadSyncUpgradeMsg) && RoadSync_ApplyCommandUpgrade(*(const RoadSyncUpgradeMsg*)p);
    case LS_CMD_PLACE:
        return n == sizeof(BuildSyncMsg) && BuildSync_ApplyPlacementCommand(*(const BuildSyncMsg*)p);
    case LS_CMD_DEMOLISH:
        return n == sizeof(BuildSyncMsg) && BuildSync_ApplyDemolishCommand(*(const BuildSyncMsg*)p);
    case LS_CMD_SETTING:
        return n == sizeof(SettingWire) && SettingsSync_ApplyCommand(*(const SettingWire*)p);
    case LS_CMD_PLAN_START:
        return n == sizeof(RoadSyncPlanStartMsg) && RoadSync_ApplyCommandPlanStart(*(const RoadSyncPlanStartMsg*)p);
    case LS_CMD_GROUP_TOOL:
        return n == sizeof(RoadSyncGroupToolMsg) && RoadSync_ApplyCommandGroupTool(*(const RoadSyncGroupToolMsg*)p);
    case LS_CMD_DIRECTION:
        return n == sizeof(RoadSyncDirectionMsg) && RoadSync_ApplyCommandDirection(*(const RoadSyncDirectionMsg*)p);
    case LS_CMD_MONEY:
        return n == sizeof(LsMoneyDelta) && ApplyMoney(*(const LsMoneyDelta*)p);
    case LS_CMD_STATE_TRANSFER:
        return n == sizeof(LsStateTransfer) && TwoStates_ApplyTransferCommand(*(const LsStateTransfer*)p);
    case LS_CMD_LOAN:
        return n == sizeof(LsLoanCommand) && TwoStates_ApplyLoanCommand(*(const LsLoanCommand*)p);
    default:
        return false;
    }
}

static bool CancelsLocalDrag(int kind)
{
    return kind == LS_CMD_SEGMENT || kind == LS_CMD_UPGRADE || kind == LS_CMD_NET_DEMOLISH ||
           kind == LS_CMD_PLACE || kind == LS_CMD_GROUP_TOOL || kind == LS_CMD_DIRECTION;
}

// Makes one command through the replay of its kind. The payload layout is the
// wire layout of the replay multiplayer; a size that does not match refuses it.
static void Apply(const Command& c)
{
    const void* p = c.data.data();
    size_t n = c.data.size();
    if (c.kind == LS_CMD_VEHICLE_SOFT_SYNC) {
        TwoStates_RestoreUiMoneySwap();
        if (!VehicleSoftSync_ApplyCommand(p, n))
            Logf("republicmp  LOCKSTEP: #%u %s after tick %llu - not made", c.seq, KindName(c.kind), c.afterTick);
        Commands_RecordMoney();
        return;
    }
    int owner = (c.owner == 2) ? 2 : 1;
    g_applyThread = GetCurrentThreadId();
    InterlockedIncrement(&g_applying);
    InterlockedIncrement64(&g_applyEpoch);
    TwoStates_BeginCommand(owner);
    double rubBefore = *Rubles(), usdBefore = *Dollars();
    InputBlock_BeginNeutral();
    if (CancelsLocalDrag(c.kind))
        RoadSync_CancelLocalDrag();
    bool isRemote = (c.owner != 0 && (int)c.owner != TwoStates_LocalState());
    ReplayUiScope menus;
    if (isRemote) Commands_BeginRemoteReplayScope();
    bool ok = DispatchCommandPayload(c.kind, p, n);
    if (isRemote) Commands_EndRemoteReplayScope();
    InputBlock_EndNeutral();
    InterlockedDecrement(&g_applying);
    g_madeThisFrame = true;
    double paidRub = *Rubles() - rubBefore, paidUsd = *Dollars() - usdBefore;
    TwoStates_EndCommand(owner);
    char money[128] = "";
    if (paidRub != 0.0 || paidUsd != 0.0)
        _snprintf_s(money, sizeof(money), _TRUNCATE, " (money %+.4f rubles %+.4f dollars)", paidRub, paidUsd);
    if (!Frequent(c.kind) || !ok || (money[0] && Determinism_GetMoneyTrace()))
        Logf("republicmp  LOCKSTEP: #%u %s after tick %llu - %s%s", c.seq, KindName(c.kind), c.afterTick,
             ok ? "made" : "not made", money);
}

bool Commands_Due(unsigned long long completed)
{
    Lock();
    bool any = false;
    if ((g_session == COMMANDS_PLAYER || g_replaying) && g_replayAt < g_record.size() && g_record[g_replayAt].afterTick <= completed)
        any = true;
    else if (!g_replaying && !g_outbox.empty())
        any = true;
    Unlock();
    return any;
}

// Player: its own actions go to the host; what the host made after this
// tick is made here now (TCP brought it before the grant of the next tick).
static void PlayerBoundary(unsigned long long completed)
{
    std::vector<Command> mine;
    std::vector<Command> due;
    Lock();
    mine.swap(g_outbox);
    while (g_replayAt < g_record.size() && g_record[g_replayAt].afterTick <= completed) {
        due.push_back(std::move(g_record[g_replayAt++]));
    }
    if (g_replayAt > 256) {
        g_record.erase(g_record.begin(), g_record.begin() + g_replayAt);
        g_replayAt = 0;
    }
    Unlock();
    for (const Command& c : mine) LockstepNet_SendCommand(c.kind, 0, c.data.data(), c.data.size());
    for (const Command& c : due) {
        if (c.afterTick < completed)
            Logf("republicmp  LOCKSTEP: #%u was made by the host after tick %llu, here after %llu - the games will differ",
                 c.seq, c.afterTick, completed);
        Apply(c);
    }
}

void Commands_Boundary(unsigned long long completed)
{
    if (g_session == COMMANDS_PLAYER) {
        PlayerBoundary(completed);
        return;
    }
    if (g_replaying) {
        std::vector<Command> due;
        Lock();
        while (g_replayAt < g_record.size() && g_record[g_replayAt].afterTick <= completed) {
            due.push_back(g_record[g_replayAt++]);
            g_replayed++;
        }
        Unlock();
        for (const Command& c : due) {
            if (c.afterTick < completed)
                Logf("republicmp  LOCKSTEP: #%u was recorded after tick %llu, made after %llu - the runs cannot match",
                     c.seq, c.afterTick, completed);
            Apply(c);
        }
        return;
    }
    std::vector<Command> due;
    Lock();
    due.swap(g_outbox);
    Unlock();
    for (Command& c : due) {
        c.afterTick = completed;
        c.seq = ++g_seq;
        if (c.kind == LS_CMD_VEHICLE_SOFT_SYNC) {
            TwoStates_RestoreUiMoneySwap();
            VehicleSoftSync_RefreshCommandTreasury(c.data.data(), c.data.size());
        }
        Apply(c);
        int wireKind = (c.kind & 0xFFFF) | (((c.owner == 2) ? 2 : 1) << 16);
        if (g_session == COMMANDS_HOST) LockstepNet_SendCommand(wireKind, c.afterTick, c.data.data(), c.data.size());
        else if (g_capturing) {
            Lock();
            g_record.push_back(std::move(c));
            Unlock();
        }
    }
}

// ---------------------------------------------------------------------------
// Changes found by comparing with a record
// ---------------------------------------------------------------------------

static bool g_haveFrameRecord = false;
static double g_frameRubles = 0, g_frameDollars = 0;
static bool g_inLateRecord = false;
static double g_lateRubles = 0, g_lateDollars = 0;
static double g_lateSimRubles = 0, g_lateSimDollars = 0;

// Balances changed since (rub, usd): the new ones become a command, the
// recorded ones are written back exactly (not by subtracting).
static void TakeMoney(double rub, double usd, const char* where)
{
    if (*(const int*)(g_exeBase + 0x9D4F10 + 0x578) != 0) {
        *Rubles() = 100000000.0;
        *Dollars() = 100000000.0;
        return;
    }
    double nowRub = *Rubles(), nowUsd = *Dollars();
    if (!isfinite(nowRub) || !isfinite(nowUsd) || !isfinite(rub) || !isfinite(usd)) {
        *Rubles() = rub;
        *Dollars() = usd;
        return;
    }
    if (nowRub == rub && nowUsd == usd) return;
    LsMoneyDelta d = { nowRub - rub, nowUsd - usd };
    *Rubles() = rub;
    *Dollars() = usd;
    static int s_logged = 0;
    if (s_logged < 40) {
        ++s_logged;
        Logf("republicmp  LOCKSTEP: money changed during %s on this PC (%+.4f rubles, %+.4f dollars) - made a command", where,
             d.rubles, d.dollars);
    }
    Commands_Submit(LS_CMD_MONEY, &d, sizeof(d));
}

// Selected tools whose actions are not commands yet act on one PC only; they
// are put away (world +d428 = 0, as the game's own cancel does). Prefixes of
// the build menu names; the carried route tools (route_tools.cpp) by full name.
static const char* const kCarriedTools[] = {
    "vehicle_addstop", "line_addstop", "line_addstop_copy", "line_addstop_copy2",
    "vehicle_godepo", "vehicle_godepo_batch",
    "vehicle_addstop_copy", "vehicle_replace", "vehicle_replace_auto", "vehicle_tovehicle", "vehicle_sendconstruction",
    "worker_relocate", "worker_relocate_educated", "worker_relocate_inmate",
    "bojka_red", "bojka_cancel",
    "join_hub_stop", "join_hub",
    "terrain_go_there",
    "water_square",
    "second_road_airplane", "second_road_airplane_square",
    "second_road_basicroad", "second_road_basicroad_square",
    "second_road_l2", "second_road_l2_square",
    "second_road_l3", "second_road_l3_square",
    "second_road_l4", "second_road_l4_square",
    "second_road_cancel"
};
static const char* const kUncarriedTools[] = {
    "vehicle_", "worker_relocate", "building_addsource", "line_addstop", "second_road", "water_square",
    "bojka", "christmas_", "join_hub", "terrain_go_there",
};
// The single-tree tools and the tree demolition brush (brush_tools.cpp): put away only when their routine differs.
static const char* const kTreeTools[] = { "tree_", "palma", "jungle_", "fjord_", "afgan_", "afghan_", "tallkriak", "leafkriak" };

static void PutAwayUncarriedTool()
{
    __int64* tool = (__int64*)(g_exeBase + 0x9D4F10 + 0xD428);
    char name[64] = {};
    __try {
        if (!*tool) return;
        const char* s = (const char*)*tool;
        if (!s[0]) return;
        for (int i = 0; i < 63 && s[i]; i++) name[i] = s[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    for (const char* carried : kCarriedTools)
        if (strcmp(name, carried) == 0 && (RouteTools_Ready() || strncmp(name, "second_road", 11) == 0)) return;
    if (strcmp(name, "building_addsource") == 0 && SourceSlots_Ready()) return; // source_slots.cpp
    bool trees = false;
    for (const char* prefix : kTreeTools) trees |= strncmp(name, prefix, strlen(prefix)) == 0;
    if ((trees && !BrushTools_Ready(3)) || (strcmp(name, "terrain_demolish_trees") == 0 && !BrushTools_Ready(1))) {
        *tool = 0;
        Logf("republicmp  LOCKSTEP: tool [%s] put away - the game's routine for it differs", name);
        return;
    }
    for (const char* prefix : kUncarriedTools) {
        if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
        *tool = 0;
        Logf("republicmp  LOCKSTEP: tool [%s] put away - its actions are not shared in lockstep yet", name);
        return;
    }
}

// The game rules (the game menu's settings page: realistic mode, traffic,
// research and the rest): the page edits a copy (world +c81c..+c852) and
// "Save settings", closing the window or the "Change settings?" popup apply it
// with 337170(world) (game_calls takes those three calls). In a session the
// copy travels as a command and every PC writes it and calls 337170, so the
// rules and their side effects (road lanes, citizens, buildings) change alike.
// The popup's save-and-reload for a critical rule (+14d3c..+14d3f, made by the
// next Late body) is not made in a session. The cheats switch (world +9: cheat
// page, cheat keys and window actions on one PC) stays off. (by SoylentGreed)
struct GameRulePair {
    unsigned copy, rule, bytes;
};
static const GameRulePair kGameRules[] = {
    { 0xC81C, 0x5B0, 4 }, { 0xC844, 0x5B4, 4 }, { 0xC848, 0x5B8, 4 }, { 0xC84C, 0x5BC, 4 }, { 0xC820, 0x5C0, 4 },
    { 0xC824, 0x5C4, 4 }, { 0xC828, 0x5C8, 4 }, { 0xC82C, 0x5CC, 4 }, { 0xC830, 0x5D0, 4 }, { 0xC834, 0x5D4, 4 },
    { 0xC838, 0x5D8, 4 }, { 0xC83C, 0x5DC, 4 }, { 0xC840, 0x5E0, 4 }, { 0xC850, 0x14451, 1 }, { 0xC851, 0x14453, 1 },
    { 0xC852, 0x14452, 1 },
};
#define RELOAD_FLAGS_OFFSET 0x14D3C // 4 bytes: a critical rule's save and reload
#define GAME_RULES_APPLY_RVA 0x337170

#pragma pack(push, 1)
struct LsGameRules {
    BYTE copy[sizeof(kGameRules) / sizeof(kGameRules[0])][4];
};
#pragma pack(pop)

bool Commands_SanitizeCheatState(BYTE* world)
{
    if (!world) return false;
    bool changed = false;
    if (world[WORLD_CHEAT_MODE_OFFSET]) {
        world[WORLD_CHEAT_MODE_OFFSET] = 0;
        changed = true;
    }
    if (world[WORLD_DEV_MODE_OFFSET]) {
        world[WORLD_DEV_MODE_OFFSET] = 0;
        changed = true;
    }
    int32_t subPage = *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET);
    if (subPage >= 5 && subPage <= 7) {
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = -1;
        changed = true;
    }
    return changed;
}

void Commands_GuardSettings()
{
    static int s_logged = 0;
    if (!g_capturing) return;
    BYTE* world = g_exeBase + WORLD_RVA;
    bool reload = false, cheats = false;
    __try {
        cheats = Commands_SanitizeCheatState(world);
        if (*(unsigned*)(world + RELOAD_FLAGS_OFFSET)) {
            *(unsigned*)(world + RELOAD_FLAGS_OFFSET) = 0;
            reload = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if ((reload || cheats) && s_logged < 8) {
        ++s_logged;
        Logf("republicmp  LOCKSTEP: %s", reload ? "the game's save and reload after a critical rule is not made in a session"
                                                : "cheats stay off in a session");
    }
}

// game_calls, the interface's 337170 call: the page's copy as a command.
void Commands_CaptureGameRules()
{
    LsGameRules m = {};
    BYTE* world = g_exeBase + 0x9D4F10;
    __try {
        for (size_t i = 0; i < sizeof(kGameRules) / sizeof(kGameRules[0]); i++)
            memcpy(m.copy[i], world + kGameRules[i].copy, kGameRules[i].bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  LOCKSTEP: the game rules could not be read - not changed");
        return;
    }
    Commands_Submit(LS_CMD_GAME_RULES, &m, sizeof(m));
}

static bool ApplyGameRules(const void* data, size_t size)
{
    if (size != sizeof(LsGameRules)) return false;
    const LsGameRules& m = *(const LsGameRules*)data;
    BYTE* world = g_exeBase + 0x9D4F10;
    __try {
        for (size_t i = 0; i < sizeof(kGameRules) / sizeof(kGameRules[0]); i++)
            memcpy(world + kGameRules[i].copy, m.copy[i], kGameRules[i].bytes);
        ((void (*)(void*))(g_exeBase + GAME_RULES_APPLY_RVA))(world);
        *(unsigned*)(world + RELOAD_FLAGS_OFFSET) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Commands_Collect()
{
    if (!g_capturing) return;
    Commands_GuardSettings();
    PutAwayUncarriedTool();
    TerrainHook_LockstepFlush();
    if (!g_haveFrameRecord) return;
    SettingsSync_LockstepDetect();
    TakeMoney(g_frameRubles, g_frameDollars, "the interface");
}

void Commands_AfterFrame(bool tickRan)
{
    if (!g_capturing) {
        g_haveFrameRecord = false;
        return;
    }
    // The settings record is only taken again when something could have
    // changed them for every PC: a tick, a command, or the first frame.
    if (tickRan || g_madeThisFrame || !g_haveFrameRecord) SettingsSync_LockstepBaseline();
    g_frameRubles = *Rubles();
    g_frameDollars = *Dollars();
    g_haveFrameRecord = true;
    g_madeThisFrame = false;
}

void Commands_RecordMoney()
{
    if (!g_capturing || !g_haveFrameRecord) return;
    g_frameRubles = *Rubles();
    g_frameDollars = *Dollars();
}

void Commands_CheckMoney(const char* where)
{
    if (!g_capturing || !g_haveFrameRecord) return;
    TakeMoney(g_frameRubles, g_frameDollars, where);
}

void Commands_LateBegin()
{
    g_inLateRecord = g_capturing;
    if (!g_inLateRecord) return;
    TwoStates_RestoreUiMoneySwap();
    Commands_CheckMoney("the held frame before Late");
    SettingsSync_LockstepBaseline();
    RoadSync_LockstepDirectionsBaseline();
    g_lateSimRubles = *Rubles();
    g_lateSimDollars = *Dollars();
    TwoStates_ToolMoneyBegin(&g_lateSimRubles, &g_lateSimDollars);
    g_lateRubles = *Rubles();
    g_lateDollars = *Dollars();
}

void Commands_LateEnd()
{
    if (!g_inLateRecord) return;
    g_inLateRecord = false;
    SettingsSync_LockstepDetect();
    RoadSync_LockstepDirectionsDetect();
    TakeMoney(g_lateRubles, g_lateDollars, "a held frame's Late (tools)");
    TwoStates_ToolMoneyEnd(g_lateSimRubles, g_lateSimDollars);
}

void Commands_SetSession(int role)
{
    g_haveFrameRecord = false;
    VehicleSoftSync_Reset();
    SettingsSync_LockstepReset();
    BuildingSwitches_Reset();
    VehicleSwitches_Reset();
    Lock();
    g_outbox.clear();
    g_record.clear();
    g_replayAt = g_replayed = 0;
    g_replaying = false;
    g_seq = 0;
    g_session = role;
    g_capturing = role != COMMANDS_OFF;
    Unlock();
    if (role == COMMANDS_OFF) {
        while (Commands_IsApplyingRemote()) Commands_EndRemoteReplayScope();
    }
    Logf("republicmp  LOCKSTEP: commands %s", role == COMMANDS_HOST ? "ordered here (host)"
                                              : role == COMMANDS_PLAYER ? "ordered by the host" : "off");
}

void Commands_SubmitRemote(int kind, const void* data, size_t size)
{
    if (g_session != COMMANDS_HOST) return;
    int cleanKind = kind & 0xFFFF;
    Command c;
    c.kind = cleanKind;
    c.owner = 2;
    c.afterTick = 0;
    c.seq = 0;
    c.data.assign((const BYTE*)data, (const BYTE*)data + size);
    Lock();
    g_outbox.push_back(std::move(c));
    Unlock();
    if (!Frequent(cleanKind)) {
        Logf("republicmp  LOCKSTEP: %s from the player - made at the next boundary", KindName(cleanKind));
    }
}

void Commands_Receive(int kind, unsigned long long afterTick, const void* data, size_t size)
{
    if (g_session != COMMANDS_PLAYER) return;
    int owner = (kind >> 16) & 0xFF;
    Command c;
    c.kind = kind & 0xFFFF;
    c.owner = (owner == 2) ? 2 : 1;
    c.afterTick = afterTick;
    c.data.assign((const BYTE*)data, (const BYTE*)data + size);
    Lock();
    c.seq = ++g_seq;
    g_record.push_back(std::move(c));
    Unlock();
}

void Commands_ProbeRun(int run, bool record)
{
    g_haveFrameRecord = false;
    Lock();
    g_outbox.clear();
    g_replaying = record && run == 1;
    g_capturing = record && run == 0;
    if (run == 0) {
        g_record.clear();
        g_seq = 0;
    }
    g_replayAt = 0;
    g_replayed = 0;
    Unlock();
    if (record)
        Logf("republicmp  LOCKSTEP: probe run %d - %s", run + 1,
             run == 0 ? "player actions become commands and are recorded"
                      : "the recorded commands are made again after the same ticks");
}

void Commands_ProbeEnd(size_t* recorded, size_t* replayed)
{
    Lock();
    g_capturing = false;
    g_replaying = false;
    if (recorded) *recorded = g_record.size();
    if (replayed) *replayed = g_replayed;
    g_outbox.clear();
    std::vector<Command>().swap(g_record);
    g_replayAt = g_replayed = 0;
    Unlock();
}

// ---------------------------------------------------------------------------
// Remote Command Sound, Warning Banner & Notification Popup Isolation
// ---------------------------------------------------------------------------

typedef bool (__fastcall* PlaySound2DFn)(void* mgr, const char* name, unsigned char loop, float vol, unsigned char p5);
typedef bool (__fastcall* PlaySound3DFn)(void* mgr, const char* name, const float* pos, unsigned char loop, float param5);
typedef long long (__fastcall* CreatePopupWindowFn)(long long world, int kind, long long ptr, int subkind, long long p5,
                                                    int popupType, int stringId, long long p8, long long p9);
typedef void (__fastcall* RebuildNotificationPopupsFn)(long long world);

static PlaySound2DFn               o_playSound2D = nullptr;
static PlaySound3DFn               o_playSound3D = nullptr;
static CreatePopupWindowFn         o_createPopupWindow = nullptr;
static RebuildNotificationPopupsFn o_rebuildNotificationPopups = nullptr;

static const BYTE kCreatePopupWindowExpect[16] = {
    0x48, 0x8B, 0xC4, 0x44, 0x89, 0x48, 0x20, 0x89,
    0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56
};

static const BYTE kRebuildNotificationPopupsExpect[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C,
    0x24, 0x20, 0x56, 0x48, 0x83, 0xEC, 0x20
};

struct BrushSoundSnapshotItem {
    uintptr_t recAddr;
    float     vol;
    uint8_t   active;
};

static constexpr int MAX_BRUSH_SOUND_SNAPSHOT = 32;

struct RemoteReplaySnapshot {
    bool                   valid;
    uint8_t                savedMuteByte;
    bool                   hasWarningText;
    BYTE                   savedWarningText[WORLD_WARNING_TEXT_BYTES];
    float                  savedWarningTimer;
    int32_t                savedPlacementError;
    bool                   hasOpenWindows;
    void*                  savedOpenWinBegin;
    void*                  savedOpenWinEnd;
    int                    brushSoundCount;
    BrushSoundSnapshotItem brushSounds[MAX_BRUSH_SOUND_SNAPSHOT];
};

static volatile LONG        g_remoteReplayDepth = 0;
static RemoteReplaySnapshot g_remoteReplaySnap = {};

static void SaveRemoteReplaySnapshot()
{
    if (!g_exeBase) {
        g_remoteReplaySnap.valid = false;
        return;
    }
    BYTE* world = g_exeBase + WORLD_RVA;
    g_remoteReplaySnap.savedMuteByte = 0;
    SafeReadPod(g_exeBase + SOUND_MUTE_BYTE_RVA, &g_remoteReplaySnap.savedMuteByte);
    uint8_t muteVal = 1;
    SafeWritePod(g_exeBase + SOUND_MUTE_BYTE_RVA, muteVal);

    g_remoteReplaySnap.hasWarningText =
        SafeMemcpyPod(g_remoteReplaySnap.savedWarningText, world + WORLD_WARNING_TEXT_OFFSET, WORLD_WARNING_TEXT_BYTES);
    g_remoteReplaySnap.savedWarningTimer = 0.0f;
    g_remoteReplaySnap.savedPlacementError = 0;
    SafeReadPod(world + WORLD_WARNING_TIMER_OFFSET, &g_remoteReplaySnap.savedWarningTimer);
    SafeReadPod(world + WORLD_PLACEMENT_ERROR_OFFSET, &g_remoteReplaySnap.savedPlacementError);

    g_remoteReplaySnap.brushSoundCount = 0;
    uintptr_t bsBegin = 0, bsEnd = 0;
    if (SafeReadPod(world + WORLD_BRUSH_SOUNDS_BEGIN_OFFSET, &bsBegin) &&
        SafeReadPod(world + WORLD_BRUSH_SOUNDS_END_OFFSET, &bsEnd) &&
        bsBegin != 0 && bsEnd > bsBegin && ((bsEnd - bsBegin) % BRUSH_SOUND_REC_SIZE) == 0) {
        for (uintptr_t p = bsBegin; p < bsEnd && g_remoteReplaySnap.brushSoundCount < MAX_BRUSH_SOUND_SNAPSHOT; p += BRUSH_SOUND_REC_SIZE) {
            float vol = 0.0f;
            uint8_t act = 0;
            if (SafeReadPod((const void*)(p + BRUSH_SOUND_VOL_OFFSET), &vol) &&
                SafeReadPod((const void*)(p + BRUSH_SOUND_ACTIVE_OFFSET), &act)) {
                BrushSoundSnapshotItem& item = g_remoteReplaySnap.brushSounds[g_remoteReplaySnap.brushSoundCount++];
                item.recAddr = p;
                item.vol     = vol;
                item.active  = act;
            }
        }
    }

    g_remoteReplaySnap.hasOpenWindows =
        SafeReadPod(g_exeBase + WORLD_OPEN_WINDOWS_BEGIN_RVA, &g_remoteReplaySnap.savedOpenWinBegin) &&
        SafeReadPod(g_exeBase + WORLD_OPEN_WINDOWS_END_RVA, &g_remoteReplaySnap.savedOpenWinEnd);
    if (g_remoteReplaySnap.hasOpenWindows) {
        SafeWritePod(g_exeBase + WORLD_OPEN_WINDOWS_END_RVA, g_remoteReplaySnap.savedOpenWinBegin);
    }
    g_remoteReplaySnap.valid = true;
}

static void RestoreRemoteReplaySnapshot()
{
    if (!g_remoteReplaySnap.valid || !g_exeBase) return;
    BYTE* world = g_exeBase + WORLD_RVA;

    if (g_remoteReplaySnap.hasOpenWindows) {
        SafeWritePod(g_exeBase + WORLD_OPEN_WINDOWS_BEGIN_RVA, g_remoteReplaySnap.savedOpenWinBegin);
        SafeWritePod(g_exeBase + WORLD_OPEN_WINDOWS_END_RVA, g_remoteReplaySnap.savedOpenWinEnd);
    }

    uintptr_t bsBegin = 0, bsEnd = 0;
    if (SafeReadPod(world + WORLD_BRUSH_SOUNDS_BEGIN_OFFSET, &bsBegin) &&
        SafeReadPod(world + WORLD_BRUSH_SOUNDS_END_OFFSET, &bsEnd) &&
        bsBegin != 0 && bsEnd > bsBegin) {
        for (int i = 0; i < g_remoteReplaySnap.brushSoundCount; ++i) {
            const BrushSoundSnapshotItem& item = g_remoteReplaySnap.brushSounds[i];
            if (item.recAddr >= bsBegin && item.recAddr + BRUSH_SOUND_REC_SIZE <= bsEnd) {
                SafeWritePod((void*)(item.recAddr + BRUSH_SOUND_VOL_OFFSET), item.vol);
                SafeWritePod((void*)(item.recAddr + BRUSH_SOUND_ACTIVE_OFFSET), item.active);
            }
        }
    }

    if (g_remoteReplaySnap.hasWarningText) {
        SafeMemcpyPod(world + WORLD_WARNING_TEXT_OFFSET, g_remoteReplaySnap.savedWarningText, WORLD_WARNING_TEXT_BYTES);
        SafeWritePod(world + WORLD_WARNING_TIMER_OFFSET, g_remoteReplaySnap.savedWarningTimer);
        SafeWritePod(world + WORLD_PLACEMENT_ERROR_OFFSET, g_remoteReplaySnap.savedPlacementError);
    }

    SafeWritePod(g_exeBase + SOUND_MUTE_BYTE_RVA, g_remoteReplaySnap.savedMuteByte);
    g_remoteReplaySnap.valid = false;
}

bool Commands_IsApplyingRemote()
{
    return InterlockedCompareExchange(&g_remoteReplayDepth, 0, 0) > 0;
}

void Commands_BeginRemoteReplayScope()
{
    if (InterlockedIncrement(&g_remoteReplayDepth) == 1) {
        SaveRemoteReplaySnapshot();
    }
}

void Commands_EndRemoteReplayScope()
{
    LONG cur = InterlockedCompareExchange(&g_remoteReplayDepth, 0, 0);
    if (cur <= 0) return;
    if (InterlockedDecrement(&g_remoteReplayDepth) == 0) {
        RestoreRemoteReplaySnapshot();
    }
}

bool Commands_OnPlaySound2D(void* mgr, const char* name, unsigned char loop, float vol, unsigned char p5)
{
    if (Commands_IsApplyingRemote()) return false;
    return o_playSound2D ? o_playSound2D(mgr, name, loop, vol, p5) : true;
}

bool Commands_OnPlaySound3D(void* mgr, const char* name, const float* pos, unsigned char loop, float param5)
{
    if (Commands_IsApplyingRemote()) {
        if (!pos || !g_exeBase) return false;
        float src[3] = {};
        float eye[3] = {};
        if (!SafeMemcpyPod(src, pos, sizeof(src)) ||
            !SafeMemcpyPod(eye, g_exeBase + CAMERA_EYE_RVA, sizeof(eye))) {
            return false;
        }
        float dx = src[0] - eye[0];
        float dy = src[1] - eye[1];
        float dz = src[2] - eye[2];
        float distSq = dx * dx + dy * dy + dz * dz;
        if (!isfinite(distSq) || distSq > (REMOTE_3D_SOUND_MAX_DIST * REMOTE_3D_SOUND_MAX_DIST)) {
            return false;
        }
    }
    return o_playSound3D ? o_playSound3D(mgr, name, pos, loop, param5) : true;
}

static bool IsForeignTerritoryNotification(uintptr_t msgPtr)
{
    if (!msgPtr || !TwoStates_IsActive()) return false;
    float x = 0.0f, z = 0.0f;
    if (!SafeReadPod((const void*)(msgPtr + NOTIF_MSG_POS_X_OFFSET), &x) ||
        !SafeReadPod((const void*)(msgPtr + NOTIF_MSG_POS_Z_OFFSET), &z)) {
        return false;
    }
    if (!isfinite(x) || !isfinite(z)) return false;
    if (fabsf(x) <= 1.0f && fabsf(z) <= 1.0f) return false;
    return TwoStates_GetStateAtWorldPos(x, z) != TwoStates_LocalState();
}

long long Commands_OnCreatePopupWindow(long long world, int kind, long long ptr, int subkind, long long p5,
                                       int popupType, int stringId, long long p8, long long p9)
{
    if (Commands_IsApplyingRemote()) return 0;
    if (kind == 2 && subkind == 5 && IsForeignTerritoryNotification((uintptr_t)ptr)) {
        return 0;
    }
    return o_createPopupWindow ? o_createPopupWindow(world, kind, ptr, subkind, p5, popupType, stringId, p8, p9) : 1;
}

void Commands_OnRebuildNotificationPopups(long long world)
{
    if (o_rebuildNotificationPopups) {
        o_rebuildNotificationPopups(world);
    }
    if (!world || !TwoStates_IsActive()) return;
    uintptr_t* begin = nullptr;
    uintptr_t* end = nullptr;
    if (!SafeReadPod((const void*)((uintptr_t)world + WORLD_NOTIF_POPUPS_BEGIN_OFFSET), &begin) ||
        !SafeReadPod((const void*)((uintptr_t)world + WORLD_NOTIF_POPUPS_END_OFFSET), &end) ||
        !begin || end <= begin || (end - begin) > 4096) {
        return;
    }
    uintptr_t* writeIt = begin;
    for (uintptr_t* readIt = begin; readIt < end; ++readIt) {
        uintptr_t msgPtr = 0;
        if (!SafeReadPod(readIt, &msgPtr)) continue;
        if (IsForeignTerritoryNotification(msgPtr)) continue;
        if (writeIt != readIt) {
            SafeWritePod(writeIt, msgPtr);
        }
        ++writeIt;
    }
    if (writeIt != end) {
        SafeWritePod((void*)((uintptr_t)world + WORLD_NOTIF_POPUPS_END_OFFSET), writeIt);
    }
}

bool Commands_InstallHooks()
{
    if (!H || !g_exe || !g_exeBase) return false;
    bool snd2d = PatchIat(g_exe, DLL_ENGINE, "?PlaySound2D@C3D_SOUNDMANAGER@@QEAA_NPEAD_NM0@Z",
                          (void*)&Commands_OnPlaySound2D, (void**)&o_playSound2D, "republicmp play sound 2d");
    bool snd3d = PatchIat(g_exe, DLL_ENGINE, "?PlaySound3D@C3D_SOUNDMANAGER@@QEAA_NPEADVC3DVECTOR3@@_NM@Z",
                          (void*)&Commands_OnPlaySound3D, (void**)&o_playSound3D, "republicmp play sound 3d");
    bool popup = InstallInlineHook(g_exeBase + WARNING_POPUP_CREATE_RVA, (void*)&Commands_OnCreatePopupWindow,
                                   (void**)&o_createPopupWindow, kCreatePopupWindowExpect,
                                   sizeof(kCreatePopupWindowExpect), "republicmp warning popup create");
    bool notif = InstallInlineHook(g_exeBase + NOTIFICATION_REBUILD_POPUPS_RVA, (void*)&Commands_OnRebuildNotificationPopups,
                                   (void**)&o_rebuildNotificationPopups, kRebuildNotificationPopupsExpect,
                                   sizeof(kRebuildNotificationPopupsExpect), "republicmp notification rebuild popups");
    Logf("republicmp  COMMANDS: hooks installed (sound2D=%d, sound3D=%d, warningPopup=%d, notifPopups=%d)",
         snd2d ? 1 : 0, snd3d ? 1 : 0, popup ? 1 : 0, notif ? 1 : 0);
    return snd2d && snd3d && popup && notif;
}
