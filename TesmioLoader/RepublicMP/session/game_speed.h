// SPDX-License-Identifier: GPL-3.0-only
// Game pause/speed control: a plain int at a fixed address.
//
// thisPtr+0x5a4 (exe+0x9D54B4): 0 = paused, 1 = x1, 2.. = faster. The main
// loop (FUN_140105a90) rebuilds the game timer's time scale from it every
// frame, so writing the int is all it takes to change the speed.
// thisPtr+0x5a8: the speed to restore when unpausing.
// thisPtr+0x5ac: "turbo" - with speed 2, a nonzero value makes the main loop
//   use a larger multiplier. The speed panel (FUN_140317db0) clears it on
//   every speed click and sets it when the fast button is clicked with keys
//   0x47+0x48 held. Synced together with the speed.
// thisPtr+0x5a2: fast-forward while held - cleared by the speed panel every
//   frame and set while the left button is held on its button.
//
// At speed 2 the main loop multiplies the game timer by (smaller = faster):
//   0.35  fast (>>)
//   0.05  "Hyper"  - turbo != 0, or left Ctrl + Numpad1 held
//   0.01  "Plaid"  - hold flag set, or left Ctrl + Numpad2 held
// The keys are read straight from the C3D_INPUT key bytes (exe+0xA54B90 +
// 0x1d / 0x4f / 0x50) every frame, so Hyper/Plaid only ever speed up the
// machine whose player holds them. Multiplayer: each side sends its current
// boost (none / Hyper / Plaid) when it changes; a side that is not boosting
// itself applies the other side's boost - Hyper through turbo (its previous
// value restored afterwards), Plaid through the hold flag, re-set every frame.
//
// Multiplayer: the host owns the speed. The host's speed is sent to the
// client whenever it changes; a change the client makes is applied locally
// at once and sent to the host as a request, which the host applies and
// sends back - so even simultaneous presses end up equal on both sides.
// Carried on the build_sync connection.
//
// The clocks still drift apart: every speed change and Hyper/Plaid toggle
// reaches the other side ~0.1 s late, and at Plaid that is hours of game
// time. So the host also sends its clock (day, year, day progress) every
// second and right after any speed change, and the client corrects its own
// clock when it is off by more than the network delay can explain - within
// a day it takes the host's progress; across midnight it either lets its own
// day roll over (behind) or holds at the start of the day (ahead), so the
// daily logic always runs exactly once per day.
#pragma once

#include "tesmio_plugin.h"

#define GAME_SPEED_DAT_RVA 0x9D4F10
#define GAME_SPEED_OFFSET       0x5a4
#define GAME_SPEED_SAVED_OFFSET 0x5a8
#define GAME_SPEED_TURBO_OFFSET 0x5ac // int: extra-fast on top of speed 2 (see below)
#define GAME_SPEED_HOLD_OFFSET  0x5a2 // byte: fast-forward while a button is held

// Game clock (same object). FUN_140334780 adds the timer's PowerTime to the
// day progress every tick; past GAME_DAY_LENGTH it resets it to 0, runs the
// daily logic and increments the day; at day 365 the year rolls.
#define GAME_DAY_OFFSET          0x590 // int: day of the year
#define GAME_YEAR_OFFSET         0x594 // int: year
#define GAME_DAY_PROGRESS_OFFSET 0x59c // float: 0..60 through the day (2.5 = one hour)
#define GAME_DAY_LENGTH          60.0f

void GameSpeed_Bind(const TsmHost* host);

int ReadGameSpeed();       // 0 = paused, 1/2
int ReadGameSpeedSaved();  // speed saved before pause (-1 = none)
void WriteGameSpeed(int speed);

void EnsureGamePaused();   // idempotent
void EnsureGameUnpaused(); // idempotent

// Per-frame (game thread, Present hook, after BuildSync_Tick): detects local
// speed changes and sends them (host: SET, client: REQUEST), sends the local
// Hyper/Plaid boost and applies the other side's.
void GameSpeed_SyncTick();

// A speed message from the other side (game thread, from BuildSync_Tick).
// kind: BUILD_SYNC_KIND_SPEED_SET / _SPEED_REQUEST / _SPEED_BOOST (speed = boost).
void GameSpeed_OnRemote(int kind, int speed, int speedSaved, int turbo);

// The host's clock (client side; game thread, from BuildSync_Tick).
void GameSpeed_OnRemoteTime(int day, int year, float progress);
