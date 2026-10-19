// SPDX-License-Identifier: GPL-3.0-only
// See sync_check.h.
#include "sync_check.h"
#include "../world_sync/buildings/build_sync.h"
#include "save_sync.h"
#include "../determinism/state_hash.h"
#include "../determinism/determinism.h"
#include "../ui/sync_block.h"
#include <math.h>
#include <vector>

#define HOUR_LENGTH 2.5f        // day progress units per game hour (60 per day)
#define HISTORY 64              // host: own summaries kept for matching

void SyncCheck_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// g_checkLock guards everything below except g_lastHourKey (main thread
// writes it, the host reads it to decide whether a player summary waits).
static CRITICAL_SECTION g_checkLock;
static volatile long long g_lastHourKey = -1;
static SyncCheckWire g_history[HISTORY];   // host: own summaries, ring
static int g_historyNext = 0;
static std::vector<SyncCheckWire> g_waiting; // host: player summaries ahead of the host's clock
static SyncCheckWire g_pending;            // client: summary waiting to be sent
static bool g_hasPending = false;
static SyncDrift g_drift = {};
static int g_overLimit = 0;                // compared hours in a row above the limit
static bool g_cooldownNoted = false;       // logged "would re-sync, cooling down" for this streak
static volatile int g_autoPercent = 0;

static long long HourKey(int year, int day, int hour)
{
    return ((long long)year * 400 + day) * 24 + hour;
}

static long long KeyOf(const SyncCheckWire& w)
{
    return HourKey(w.year, w.day, w.hour);
}

// Session over or paused behind the sync screen: nothing measured so far
// belongs to the next world.
static void ResetSession()
{
    g_lastHourKey = -1;
    EnterCriticalSection(&g_checkLock);
    memset(g_history, 0, sizeof(g_history));
    g_historyNext = 0;
    g_waiting.clear();
    g_hasPending = false;
    g_overLimit = 0;
    g_cooldownNoted = false;
    g_drift.valid = false; // the last figure belongs to the old world
    LeaveCriticalSection(&g_checkLock);
}

// Percent of the larger value, but of at least `floor`.
static float Percent(double a, double b, double floor)
{
    double base = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    if (base < floor) base = floor;
    return (float)(fabs(a - b) / base * 100.0);
}

unsigned long long SyncCheck_GetResyncCooldownMs()
{
    if (SaveSync_GetState() != SESSION_HOST_IN_GAME) return 0;
    ULONGLONG since = SaveSync_GetMsSinceWorldSent();
    return since >= SYNC_CHECK_RESYNC_COOLDOWN_MS ? 0 : SYNC_CHECK_RESYNC_COOLDOWN_MS - since;
}

// Host: compares one hour and decides about an automatic re-sync. No lock held.
static void Compare(const SyncCheckWire& h, const SyncCheckWire& c)
{
    SyncDrift d = {};
    d.valid = true;
    d.year = c.year; d.day = c.day; d.hour = c.hour;
    d.citizens = Percent(h.citizens, c.citizens, SYNC_CHECK_FLOOR_CITIZENS);
    d.vehicles = Percent(h.vehicles, c.vehicles, SYNC_CHECK_FLOOR_VEHICLES);
    d.storage = Percent(h.storage + h.cargo, c.storage + c.cargo, SYNC_CHECK_FLOOR_STORAGE);
    d.production = Percent(h.production, c.production, SYNC_CHECK_FLOOR_PRODUCTION);
    // Production stays out (its window resets all the time): see sync_check.h.
    d.total = d.citizens > d.storage ? d.citizens : d.storage;
    if (d.vehicles > d.total) d.total = d.vehicles;
    d.hostVehicles = h.vehicles; d.clientVehicles = c.vehicles;
    d.hostCitizens = h.citizens; d.clientCitizens = c.citizens;

    int limit = g_autoPercent;
    bool over = limit > 0 && d.total > (float)limit;
    bool resync = false, noteCooldown = false;
    unsigned long long cooldown = 0;

    EnterCriticalSection(&g_checkLock);
    g_overLimit = over ? g_overLimit + 1 : 0;
    if (!over) g_cooldownNoted = false;
    d.overLimitChecks = g_overLimit;
    if (over && g_overLimit >= SYNC_CHECK_CONFIRM_CHECKS && !SaveSync_IsResyncing()) {
        cooldown = SyncCheck_GetResyncCooldownMs();
        if (cooldown == 0) {
            resync = true;
            g_overLimit = 0;
            g_cooldownNoted = false;
        } else if (!g_cooldownNoted) {
            noteCooldown = true;
            g_cooldownNoted = true;
        }
    }
    g_drift = d;
    LeaveCriticalSection(&g_checkLock);

    Logf("republicmp  SYNC_CHECK: day %d hour %d drift %.1f%% (citizens %d/%d %.1f%%, storage %.0f/%.0f %.1f%%, "
         "vehicles %d/%d %.1f%%, buildings %d/%d)%s",
         c.day, c.hour, d.total, h.citizens, c.citizens, d.citizens, h.storage + h.cargo, c.storage + c.cargo,
         d.storage, h.vehicles, c.vehicles, d.vehicles, h.buildings, c.buildings,
         over ? " - above the limit" : "");
    if (noteCooldown)
        Logf("republicmp  SYNC_CHECK: drift above %d%% for %d hours, but the player got the world %llus ago - "
             "re-sync waits %llus more", limit, d.overLimitChecks,
             (SYNC_CHECK_RESYNC_COOLDOWN_MS - cooldown) / 1000, cooldown / 1000);
    if (resync) {
        Logf("republicmp  SYNC_CHECK: drift %.1f%% above %d%% for %d hours in a row - re-syncing", d.total, limit,
             d.overLimitChecks);
        SaveSync_RequestResync();
    }
}

// The game clock as an hour key; false if it could not be read.
static bool ReadHourKey(long long* out)
{
    __try {
        BYTE* w = g_exeBase + 0x9D4F10;
        int day = *(int*)(w + 0x590), year = *(int*)(w + 0x594);
        int hour = (int)(*(float*)(w + 0x59c) / HOUR_LENGTH);
        *out = HourKey(year, day, hour);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Main thread, after every simulation step.
static void OnStep()
{
    if (!BuildSync_IsConnected() || SyncBlock_IsActive()) {
        if (g_lastHourKey >= 0) ResetSession();
        return;
    }
    // Cheap check first: only the clock decides whether this step opens a new hour.
    long long key;
    if (!ReadHourKey(&key) || key == g_lastHourKey) return;
    bool first = g_lastHourKey < 0;
    g_lastHourKey = key;
    if (first) return; // joined mid-hour - start with the next full hour
    StateSummary s;
    if (!StateHash_Compute(&s)) return;

    SyncCheckWire wire = {};
    wire.year = s.year;
    wire.day = s.day;
    wire.hour = (int)(s.dayProgress / HOUR_LENGTH);
    wire.buildings = s.buildings;
    wire.vehicles = s.vehicles;
    wire.citizens = s.citizens;
    wire.storage = s.storageTotal;
    wire.cargo = s.cargoTotal;
    wire.production = s.productionTotal;

    if (!BuildSync_IsHostRole()) {
        EnterCriticalSection(&g_checkLock);
        g_pending = wire;
        g_hasPending = true;
        LeaveCriticalSection(&g_checkLock);
        return;
    }

    // Host: keep it, and settle the player summaries that were waiting for it.
    std::vector<SyncCheckWire> ready;
    int dropped = 0;
    EnterCriticalSection(&g_checkLock);
    g_history[g_historyNext] = wire;
    g_historyNext = (g_historyNext + 1) % HISTORY;
    for (size_t i = 0; i < g_waiting.size();) {
        long long k = KeyOf(g_waiting[i]);
        if (k == KeyOf(wire)) {
            ready.push_back(g_waiting[i]);
        } else if (k < KeyOf(wire)) {
            dropped++; // an hour the host skipped (clock set forward) - never comes
        } else {
            ++i;
            continue;
        }
        g_waiting.erase(g_waiting.begin() + i);
    }
    LeaveCriticalSection(&g_checkLock);

    if (dropped)
        Logf("republicmp  SYNC_CHECK: %d player summaries for hours the host skipped - dropped", dropped);
    for (const SyncCheckWire& c : ready) Compare(wire, c);
}

void SyncCheck_Install()
{
    InitializeCriticalSection(&g_checkLock);
    Determinism_AddStepObserver(&OnStep);
}

void SyncCheck_Tick()
{
    SyncCheckWire w;
    bool send = false;
    EnterCriticalSection(&g_checkLock);
    if (g_hasPending) {
        w = g_pending;
        g_hasPending = false;
        send = true;
    }
    LeaveCriticalSection(&g_checkLock);
    if (send && !BuildSync_IsHostRole()) BuildSync_SendState(w);
}

void SyncCheck_OnRemote(const SyncCheckWire& c)
{
    if (!BuildSync_IsHostRole()) return;
    long long key = KeyOf(c);
    SyncCheckWire h;
    bool found = false, waits = false;
    EnterCriticalSection(&g_checkLock);
    for (int i = 0; i < HISTORY; i++) {
        const SyncCheckWire& e = g_history[i];
        if ((e.year || e.day) && KeyOf(e) == key) {
            h = e;
            found = true;
            break;
        }
    }
    long long hostKey = g_lastHourKey;
    if (!found && hostKey >= 0 && key > hostKey) {
        // The player is ahead: wait for the host's own summary of that hour.
        if (g_waiting.size() >= SYNC_CHECK_MAX_WAITING) g_waiting.erase(g_waiting.begin());
        g_waiting.push_back(c);
        waits = true;
    }
    LeaveCriticalSection(&g_checkLock);

    if (found) {
        Compare(h, c);
    } else if (!waits) {
        Logf("republicmp  SYNC_CHECK: no own summary for year %d day %d hour %d - skipped", c.year, c.day, c.hour);
    }
}

SyncDrift SyncCheck_GetDrift()
{
    EnterCriticalSection(&g_checkLock);
    SyncDrift d = g_drift;
    LeaveCriticalSection(&g_checkLock);
    return d;
}

void SyncCheck_SetAutoResyncPercent(int percent)
{
    g_autoPercent = percent > 0 ? percent : 0;
    EnterCriticalSection(&g_checkLock);
    g_overLimit = 0; // a new limit starts a new streak
    g_cooldownNoted = false;
    LeaveCriticalSection(&g_checkLock);
}

int SyncCheck_GetAutoResyncPercent() { return g_autoPercent; }
