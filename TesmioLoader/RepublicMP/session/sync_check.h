// SPDX-License-Identifier: GPL-3.0-only
// Sync check: how far the two independently simulated worlds have drifted.
//
// Both games take a state summary (determinism/state_hash) right after the first
// simulation step of every game hour - at the same game time on both sides,
// since the client follows the host's clock. The player sends each summary to
// the host on the build_sync channel; the host compares it with its own
// summary of the same hour and shows the drift per subsystem (citizens,
// vehicles, storage, production).
//
// A player's summary can arrive before the host has reached that hour (the
// player's clock runs slightly ahead between corrections): it waits on the
// host until the host's own summary of that hour exists.
//
// Drift percentages are taken against the larger of the two values, but
// never against less than a floor per subsystem (SYNC_CHECK_FLOOR_*), so a
// handful of citizens in a fresh city does not read as tens of percent.
//
// The drift that decides an automatic re-sync is the largest of citizens,
// storage+cargo and vehicles. Vehicles count fully: every vehicle carries
// the economy, and buying one is not synced yet, so only a re-sync brings
// it over - with the limit at 10%, 90% of the fleet must match. Their floor
// is low (SYNC_CHECK_FLOOR_VEHICLES): 1 bought vehicle against 0 is 10%.
// The automatic re-sync also needs the limit exceeded in
// SYNC_CHECK_CONFIRM_CHECKS compared hours in a row, and at least
// SYNC_CHECK_RESYNC_COOLDOWN_MS since the player last received the world
// (join or re-sync).
#pragma once

#include "tesmio_plugin.h"

#define SYNC_CHECK_FLOOR_CITIZENS 100.0
#define SYNC_CHECK_FLOOR_VEHICLES 10.0
#define SYNC_CHECK_FLOOR_STORAGE 500.0     // storage + cargo, resource units
#define SYNC_CHECK_FLOOR_PRODUCTION 100.0
#define SYNC_CHECK_CONFIRM_CHECKS 3
#define SYNC_CHECK_RESYNC_COOLDOWN_MS (10 * 60 * 1000)
#define SYNC_CHECK_MAX_WAITING 32          // host: player summaries waiting for the host's hour

// Wire form of one hourly summary (part of BuildSyncMsg).
struct SyncCheckWire {
    int year, day, hour;
    int buildings, vehicles, citizens;
    double storage, cargo, production;
};

struct SyncDrift {
    bool valid;
    int year, day, hour;
    float total;       // what decides an automatic re-sync: the largest of citizens, storage, vehicles; percent
    float citizens, vehicles, storage, production; // percent (production is informational)
    int hostVehicles, clientVehicles;
    int hostCitizens, clientCitizens;
    int overLimitChecks; // compared hours in a row with total above the auto re-sync limit
};

void SyncCheck_Bind(const TsmHost* host);

// Registers the step observer (determinism/determinism). Call once at start.
void SyncCheck_Install();

// Per-frame (game thread, Present hook): sends the player's pending summary.
void SyncCheck_Tick();

// A summary from the player (host side; game thread, from BuildSync_Tick).
void SyncCheck_OnRemote(const SyncCheckWire& w);

// Host: last measured drift.
SyncDrift SyncCheck_GetDrift();

// Host: ms the automatic re-sync still has to wait after the last world
// transfer (0 = may re-sync now).
unsigned long long SyncCheck_GetResyncCooldownMs();

// Host: re-sync automatically when the drift exceeds this many percent (0 = off).
void SyncCheck_SetAutoResyncPercent(int percent);
int SyncCheck_GetAutoResyncPercent();
