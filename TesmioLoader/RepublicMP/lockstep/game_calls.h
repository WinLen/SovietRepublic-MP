// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: window buttons as commands (by SoylentGreed).
//
// Some window buttons change the world through one routine call: the vehicle
// window's send to depot, release, stop / start, clear line and next skin, the
// line window's launch, copy schedule and replace, the lines list's new line,
// the building windows' "call all vehicles home". Each such call site (a CALL
// rel32 in the window's handler) is pointed at a relay. While commands are
// captured the call is not made on the clicking PC: its arguments become a
// command (vehicles, buildings and lines by identity, numbers as they are) and
// every PC makes the same call at the boundary.
#pragma once

#include "tesmio_plugin.h"

void GameCalls_Bind(const TsmHost* host);

// Startup: points the call sites at their relays. Sites whose bytes differ are left alone.
bool GameCalls_Install();

// The command (lockstep/commands).
bool GameCalls_ApplyCommand(const void* data, size_t size);

// Pure helper for rail/road depot row drag-drop self-drop filtering (0x7C7E87 / 0x7C7ED8).
bool GameCalls_IsDepotRowSelfDrop(unsigned long long dragged, unsigned long long hoveredResolved,
                                  unsigned long long hoveredRaw, unsigned long long rowHead);

// Test helper for residential building invite-citizens command (0x6F8CC0 -> 0x823470).
void GameCalls_InviteCitizensForTest(unsigned long long world, unsigned long long building, unsigned mode);

// Test helpers for vehicle window schedule & launch commands (0x7D6B00).
void GameCalls_AddBorderStopForTest(unsigned long long world, unsigned long long vehicle,
                                    unsigned long long borderBld, int insertIdx);
void GameCalls_DeleteStopForTest(unsigned long long world, unsigned long long vehicle, int stopIdx);
void GameCalls_ChangeCurrentStopForTest(unsigned long long vehicle, int stopIdx);
void GameCalls_ReleaseVehicleForTest(unsigned long long vehicle);
