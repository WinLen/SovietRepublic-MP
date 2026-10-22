// SPDX-License-Identifier: GPL-3.0-only
// Citizens' destination lists: where a home's (or a bus stop's) workers,
// students, passengers and tourists go - building +0xf18 / +0xf30 / +0xf48 /
// +0xf60, vectors of 16-byte entries (share, flags, target building). The
// destinations window edits them inline on the clicking PC only, so the
// other PC's citizens went elsewhere. A list changed right after a local
// click travels whole over the settings channel (field
// CITIZEN_DESTINATIONS_FIELD, up to 9 targets) and is rebuilt on the other
// side with the window's own add / remove calls, then read back.
//
// (by TucKnock)
#pragma once
#include "tesmio_plugin.h"
#include "../../session/settings_sync.h"

constexpr int CITIZEN_DESTINATIONS_FIELD = 1001;
void CitizenDestinations_Bind(const TsmHost* host);
void CitizenDestinations_Reset();
void CitizenDestinations_Scan(BYTE** buildings, int count, bool sendChanges, bool clicked);
void CitizenDestinations_LockstepBaseline(BYTE** buildings, int count);
void CitizenDestinations_LockstepDetect();
// true: consumed (applied or rejected); false: an endpoint is not present yet.
bool CitizenDestinations_Apply(const SettingWire& wire);
bool CitizenDestinations_ApplyLockstep(const SettingWire& wire);
// Event-only diagnostics: called when queued and once at timeout, never per frame.
void CitizenDestinations_LogPending(const SettingWire& wire, const char* event);
