// SPDX-License-Identifier: GPL-3.0-only
// Money: the republic's two balances, plain doubles on the world object
// (thisPtr = exe+0x9D4F10).
//
//   +0x580  double: dollars
//   +0x588  double: rubles
//   +0x578  int:    != 0 - nothing is ever charged (purchases skip both the
//                   check and the deduction); part of the save, so equal on
//                   both sides after the join
//
// Found through the save routine FUN_14042cc70, which copies +0x580/+0x588
// (as floats), the day +0x590 and the year +0x594 into header.bin, and the
// vehicle import purchase (FUN_1401aad70: compares both balances with the
// price, subtracts the ruble price from +0x588 and the dollar price from
// +0x580, and adds the same prices to $Vehicles_ImportRUB / _ImportUSD of the
// current day's stats at thisPtr+0x11328+0x18c / +0x188).
//
// Multiplayer: the host owns the money. Both games simulate on their own, so
// the balances drift apart (wages, exports, imports); the host sends its two
// balances every second and the client takes them over. A purchase made on
// the client still shows up on the host, because the host replays the
// client's placement / path with the client's construction mode and pays for
// it there. Whatever the client's own simulation earns or spends is dropped.
// Carried on the build_sync connection.
#pragma once

#include "tesmio_plugin.h"

#define MONEY_THIS_RVA     0x9D4F10
#define MONEY_USD_OFFSET   0x580
#define MONEY_RUB_OFFSET   0x588
#define MONEY_NOCOST_OFFSET 0x578

void Money_Bind(const TsmHost* host);

// Per-frame (game thread, Present hook, after BuildSync_Tick): the host sends
// its balances every MONEY_SYNC_INTERVAL_MS while the sync is running.
void Money_SyncTick();

// The host's balances (client side; game thread, from BuildSync_Tick).
void Money_OnRemote(double rub, double usd);
