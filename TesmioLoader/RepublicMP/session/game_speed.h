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
//   0.05  "Hyper"  - turbo != 0, or left Ctrl + Numpad1 held (max allowed MP speed)
//   0.01  "Plaid"  - disabled in multiplayer (LCtrl+Numpad2 ignored; [>>>] hold capped at Hyper)
// Multiplayer: each side sends its current boost (none / Hyper) when it changes;
// a side that is not boosting itself applies the other side's Hyper boost through
// turbo (its previous value restored afterwards).
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

#define GAME_SPEED_DAT_RVA 0xA1EF20
#define GAME_SPEED_OFFSET       0x5cc
#define GAME_SPEED_SAVED_OFFSET 0x5d0
#define GAME_SPEED_TURBO_OFFSET 0x5d4 // int: extra-fast on top of speed 2 (see below)
#define GAME_SPEED_HOLD_OFFSET  0x5ca // byte: fast-forward while a button is held

// Game clock (same object). FUN_140334780 adds the timer's PowerTime to the
// day progress every tick; past GAME_DAY_LENGTH it resets it to 0, runs the
// daily logic and increments the day; at day 365 the year rolls.
#define GAME_DAY_OFFSET          0x5b8 // int: day of the year
#define GAME_YEAR_OFFSET         0x5bc // int: year
#define GAME_DAY_PROGRESS_OFFSET 0x5c4 // float: 0..60 through the day (2.5 = one hour)
#define GAME_DAY_LENGTH          60.0f

void GameSpeed_Bind(const TsmHost* host);

int ReadGameSpeed();       // 0 = paused, 1/2
int ReadGameSpeedSaved();  // speed saved before pause (-1 = none)
void WriteGameSpeed(int speed);

void EnsureGamePaused();   // idempotent
void EnsureGameUnpaused(); // idempotent

// Per-frame (game thread, Present hook, after BuildSync_Tick): detects local
// speed changes and sends them (host: SET, client: REQUEST), sends the local
// Hyper boost and applies the other side's.
void GameSpeed_SyncTick();

// A speed message from the other side (game thread, from BuildSync_Tick).
// kind: BUILD_SYNC_KIND_SPEED_SET / _SPEED_REQUEST / _SPEED_BOOST (speed = boost).
void GameSpeed_OnRemote(int kind, int speed, int speedSaved, int turbo);

// The host's clock (client side; game thread, from BuildSync_Tick).
void GameSpeed_OnRemoteTime(int day, int year, float progress);
