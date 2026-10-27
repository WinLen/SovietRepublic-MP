// SPDX-License-Identifier: GPL-3.0-only
// See game_speed.h.
#include "game_speed.h"
#include "save_sync.h"
#include "../determinism/determinism.h"
#include "../ui/sync_block.h"
#include "../world_sync/buildings/build_sync.h"
#include <math.h>

#pragma comment(lib, "User32.lib")


void GameSpeed_Bind(const TsmHost* host)
{
    TsmBind(host);
}

int ReadGameSpeed()
{
    return *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET);
}

int ReadGameSpeedSaved()
{
    return *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_SAVED_OFFSET);
}

void WriteGameSpeed(int speed)
{
    *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET) = speed;
}

static void WriteGameSpeedSaved(int speed)
{
    *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_SAVED_OFFSET) = speed;
}

static int ReadTurbo()
{
    return *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_TURBO_OFFSET);
}

static void WriteTurbo(int turbo)
{
    *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_TURBO_OFFSET) = turbo;
}

static int ReadHold()
{
    return *(unsigned char*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_HOLD_OFFSET);
}

static void WriteHold(int hold)
{
    *(unsigned char*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_HOLD_OFFSET) = (unsigned char)hold;
}

// C3D_INPUT key bytes the main loop (FUN_140105a90) reads for Hyper/Plaid.
#define INPUT_OBJECT_RVA 0xA54B90
#define KEY_LCTRL_OFFSET 0x1d
#define KEY_NUMPAD1_OFFSET 0x4f
#define KEY_NUMPAD2_OFFSET 0x50

enum SpeedBoost { BOOST_NONE = 0, BOOST_HYPER = 1, BOOST_PLAID = 2 };

#define TIME_SYNC_INTERVAL_MS 1000
#define TIME_SYNC_MIN_ERROR 0.25f   // day-progress units (= 6 game minutes)
#define TIME_SYNC_DELAY_SEC 0.4f    // worst network delay the tolerance allows for

static int ReadDay()        { return *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_DAY_OFFSET); }
static int ReadYear()       { return *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_YEAR_OFFSET); }
static float ReadProgress() { return *(float*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_DAY_PROGRESS_OFFSET); }
static void WriteProgress(float v) { *(float*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_DAY_PROGRESS_OFFSET) = v; }

static bool g_timeSendNow = false;     // host: send the clock on the next tick
static ULONGLONG g_lastTimeSendMs = 0;
static float g_progressRate = 0.0f;    // client: measured day-progress units per second

// Client: how fast the local clock runs right now (units/s), smoothed. Used
// to size the correction tolerance - at Plaid one network delay is a lot.
static void MeasureClockRate()
{
    static float s_lastProgress = -1.0f;
    static ULONGLONG s_lastMs = 0;
    float p = ReadProgress();
    ULONGLONG now = GetTickCount64();
    if (s_lastMs && now > s_lastMs && p >= s_lastProgress) {
        float rate = (p - s_lastProgress) * 1000.0f / (float)(now - s_lastMs);
        g_progressRate = g_progressRate * 0.9f + rate * 0.1f;
    }
    s_lastProgress = p;
    s_lastMs = now;
}
static const char* const kBoostNames[] = { "none", "Hyper", "Plaid" };

// Called every frame by the sync screen - logs only when it changes something.
void EnsureGamePaused()
{
    if (ReadGameSpeed() != 0) {
        int cur = ReadGameSpeed();
        WriteGameSpeedSaved(cur);
        WriteGameSpeed(0);
        Logf("republicmp  GAME_SPEED: EnsureGamePaused -> paused (was speed %d)", cur);
    }
}

void EnsureGameUnpaused()
{
    if (ReadGameSpeed() == 0) {
        int restore = ReadGameSpeedSaved();
        if (restore <= 0) restore = 1; // 0/-1/uninitialized all mean "no real saved speed"
        WriteGameSpeed(restore);
        Logf("republicmp  GAME_SPEED: EnsureGameUnpaused -> unpaused, speed=%d", restore);
    }
}

// ---------------------------------------------------------------------------
// Multiplayer sync (game thread)
// ---------------------------------------------------------------------------

static int g_lastSeenSpeed = -1;   // values at the end of the previous tick
static int g_lastSeenTurbo = -1;
static bool g_wasSyncing = false;  // connected and not behind the sync screen last tick

// Hyper/Plaid.
static int g_localBoost = BOOST_NONE;   // what this player is doing, last sent
static int g_remoteBoost = BOOST_NONE;  // what the other player is doing
static int g_appliedBoost = BOOST_NONE; // the remote boost currently forced here
static int g_turboBeforeHyper = 0;      // restored when a forced Hyper ends

// This player's boost, from the same inputs the main loop uses. The hold flag
// counts only when it is not ours (forced Plaid).
static int ReadLocalBoost()
{
    if (ReadGameSpeed() != 2) return BOOST_NONE;
    const unsigned char* keys = (const unsigned char*)(g_exeBase + INPUT_OBJECT_RVA);
    if (keys[KEY_LCTRL_OFFSET]) {
        if (keys[KEY_NUMPAD1_OFFSET]) return BOOST_HYPER;
        if (keys[KEY_NUMPAD2_OFFSET]) return BOOST_PLAID;
    }
    if (g_appliedBoost != BOOST_PLAID && ReadHold()) return BOOST_PLAID;
    return BOOST_NONE;
}

// Forces `boost` (the other side's) here, or releases a forced one.
static void ApplyRemoteBoost(int boost)
{
    if (boost != g_appliedBoost) {
        if (g_appliedBoost == BOOST_HYPER) {
            WriteTurbo(g_turboBeforeHyper);
            g_lastSeenTurbo = g_turboBeforeHyper; // not a local change
        }
        if (boost == BOOST_HYPER) {
            g_turboBeforeHyper = ReadTurbo();
            WriteTurbo(1);
            g_lastSeenTurbo = 1;
        }
        Logf("republicmp  GAME_SPEED: other player's boost %s -> %s", kBoostNames[g_appliedBoost], kBoostNames[boost]);
        g_appliedBoost = boost;
    }
    // The speed panel clears the hold flag every frame; set it again for the
    // main loop's next frame.
    if (g_appliedBoost == BOOST_PLAID) WriteHold(1);
}

void GameSpeed_SyncTick()
{
    if (SaveSync_IsWaitingLobbyActive()) {
        EnsureGamePaused();
        return;
    }

    int cur = ReadGameSpeed();
    int turbo = ReadTurbo();
    int saved = ReadGameSpeedSaved();

    bool syncing = BuildSync_IsConnected() && !SyncBlock_IsActive();
    if (!syncing) {
        if (g_appliedBoost != BOOST_NONE) ApplyRemoteBoost(BOOST_NONE);
        g_wasSyncing = false;
        g_lastSeenSpeed = cur;
        g_lastSeenTurbo = ReadTurbo();
        g_localBoost = BOOST_NONE;
        g_remoteBoost = BOOST_NONE;
        return;
    }

    bool host = BuildSync_IsHostRole();
    if (!g_wasSyncing) {
        // Sync (re)started: the host states its speed once.
        g_wasSyncing = true;
        g_lastSeenSpeed = cur;
        g_lastSeenTurbo = turbo;
        if (host) {
            Logf("republicmp  GAME_SPEED: sync started - sending host speed %d turbo %d (saved %d)", cur, turbo, saved);
            BuildSync_SendSpeed(BUILD_SYNC_KIND_SPEED_SET, cur, saved, turbo);
            g_timeSendNow = true;
        }
        return;
    }

    // Speed and turbo. A forced Hyper owns turbo while it lasts.
    bool turboChanged = g_appliedBoost != BOOST_HYPER && turbo != g_lastSeenTurbo;
    if (cur != g_lastSeenSpeed || turboChanged) {
        Logf("republicmp  GAME_SPEED: local speed %d/turbo %d -> %d/%d - %s", g_lastSeenSpeed, g_lastSeenTurbo,
             cur, turbo, host ? "sending to the client" : "asking the host");
        g_lastSeenSpeed = cur;
        g_lastSeenTurbo = turbo;
        BuildSync_SendSpeed(host ? BUILD_SYNC_KIND_SPEED_SET : BUILD_SYNC_KIND_SPEED_REQUEST, cur, saved,
                            g_appliedBoost == BOOST_HYPER ? g_turboBeforeHyper : turbo);
        g_timeSendNow = true;
    }

    // Hyper/Plaid: send ours when it changes, take theirs while not boosting.
    int local = ReadLocalBoost();
    if (local != g_localBoost) {
        Logf("republicmp  GAME_SPEED: my boost %s -> %s - sending", kBoostNames[g_localBoost], kBoostNames[local]);
        g_localBoost = local;
        BuildSync_SendSpeed(BUILD_SYNC_KIND_SPEED_BOOST, local, 0, 0);
        g_timeSendNow = true;
    }
    int applied = g_appliedBoost;
    ApplyRemoteBoost(local == BOOST_NONE ? g_remoteBoost : BOOST_NONE);
    if (g_appliedBoost != applied) g_timeSendNow = true;

    // Clock: the host states it; the client measures its own rate.
    if (host) {
        ULONGLONG now = GetTickCount64();
        if (g_timeSendNow || now - g_lastTimeSendMs >= TIME_SYNC_INTERVAL_MS) {
            BuildSync_SendTime(ReadDay(), ReadYear(), ReadProgress());
            g_lastTimeSendMs = now;
            g_timeSendNow = false;
        }
    } else {
        MeasureClockRate();
    }
}

void GameSpeed_OnRemoteTime(int day, int year, float progress)
{
    if (SyncBlock_IsActive() || BuildSync_IsHostRole()) return;
    if (!(progress >= 0.0f && progress <= GAME_DAY_LENGTH)) return; // also rejects NaN

    int localDay = ReadDay(), localYear = ReadYear();
    float local = ReadProgress();
    long long hostKey = (long long)year * 1000 + day;
    long long localKey = (long long)localYear * 1000 + localDay;

    if (hostKey == localKey) {
        float error = progress - local;
        float tolerance = g_progressRate * TIME_SYNC_DELAY_SEC;
        if (tolerance < TIME_SYNC_MIN_ERROR) tolerance = TIME_SYNC_MIN_ERROR;
        if (fabsf(error) > tolerance) {
            Logf("republicmp  GAME_SPEED: clock off by %+.2f (%+.1f game hours) - set to the host's %.2f",
                 error, error / 2.5f, progress);
            WriteProgress(progress < GAME_DAY_LENGTH ? progress : GAME_DAY_LENGTH - 0.001f);
        }
    } else if (hostKey > localKey) {
        // Behind across midnight: finish our day so our own day logic runs.
        if (local < GAME_DAY_LENGTH - 0.001f) {
            Logf("republicmp  GAME_SPEED: clock a day behind the host (day %d vs %d) - ending the day", localDay, day);
            WriteProgress(GAME_DAY_LENGTH - 0.001f);
        }
    } else {
        // Ahead across midnight: hold at the start of our day until the host gets here.
        if (local > 0.0f) {
            Logf("republicmp  GAME_SPEED: clock a day ahead of the host (day %d vs %d) - holding", localDay, day);
            WriteProgress(0.0f);
        }
    }
}

void GameSpeed_OnRemote(int kind, int speed, int speedSaved, int turbo)
{
    if (SyncBlock_IsActive()) return; // the sync screen owns the pause; the host re-sends after it
    if (kind == BUILD_SYNC_KIND_SPEED_BOOST) {
        if (speed >= BOOST_NONE && speed <= BOOST_PLAID) {
            if (speed != g_remoteBoost) g_timeSendNow = true;
            g_remoteBoost = speed;
        }
        return;
    }
    if (speed < 0 || speed > 100) {
        Logf("republicmp  GAME_SPEED: ignoring remote speed %d", speed);
        return;
    }
    bool host = BuildSync_IsHostRole();
    if (host && kind == BUILD_SYNC_KIND_SPEED_REQUEST) {
        // Applied here; the next SyncTick sees the change and sends it back
        // to the client as a SET, which settles both sides on this value.
        Logf("republicmp  GAME_SPEED: client asks for speed %d turbo %d - applying", speed, turbo);
        WriteGameSpeedSaved(speedSaved);
        if (g_appliedBoost == BOOST_HYPER) g_turboBeforeHyper = turbo;
        else WriteTurbo(turbo);
        WriteGameSpeed(speed);
        g_timeSendNow = true;
    } else if (!host && kind == BUILD_SYNC_KIND_SPEED_SET) {
        if (ReadGameSpeed() != speed || ReadTurbo() != turbo)
            Logf("republicmp  GAME_SPEED: host speed %d turbo %d - applying", speed, turbo);
        WriteGameSpeedSaved(speedSaved);
        if (g_appliedBoost == BOOST_HYPER) g_turboBeforeHyper = turbo;
        else WriteTurbo(turbo);
        WriteGameSpeed(speed);
        g_lastSeenSpeed = speed; // not a local change - do not send it back
        g_lastSeenTurbo = g_appliedBoost == BOOST_HYPER ? ReadTurbo() : turbo;
    }
}
