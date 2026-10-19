// SPDX-License-Identifier: GPL-3.0-only
// Weather: the host owns it.
//
// The whole weather state is one block on the world object (thisPtr =
// exe+0x9D4F10), updated every frame by FUN_140333bd0 (sky type: rain rolls
// with rand() at the start of a day) and FUN_1405cfea0 (temperature, wind,
// snowfall - all re-targeted with C3DRandom_Float on their own timers):
//
//   +0xE30 float  temperature (°C), +0xE34 target, +0xE38 re-target timer
//   +0xE60 float  wind, +0xE64 target, +0xE68 re-target timer
//   +0xE28 int    snowing, +0xE2C float re-roll timer
//   +0xE70 int    sky: 0 clear, 1 sunset/sunrise, 2 night, 3 overcast (rain)
//   +0xE78 float  rain wetness (1 dry .. 0.25 soaked)
//
// It is not saved - every load rolls new weather, so not even a re-join
// aligned it. Temperature drives heating demand, wind the wind turbines,
// snowfall the road snow and snow ploughs.
//
// The host sends the block every WEATHER_SYNC_INTERVAL_MS; the client writes
// it right after a simulation step on the main thread, where the game itself
// updates the weather. Copying the timers keeps the client from re-rolling in
// between. Of the sky only the rain decision (3 or not) is taken over - the
// time-of-day skies follow the local clock (see OnStep in weather.cpp). A
// changed sky type also needs the lighting switched the way FUN_140333bd0
// does it: FUN_140334020(_, type, -1, 0.001f).
//
// Not copied: season (+0xE1C..+0xE24) and night factor (+0xE74) follow from
// the synced clock; particles, UI and the per-road snow bytes stay local.
#pragma once

#include "tesmio_plugin.h"

#define WEATHER_SYNC_INTERVAL_MS 1000

struct WeatherWire {
    float temperature, temperatureTarget, temperatureTimer;
    float wind, windTarget, windTimer;
    int snowing;
    float snowTimer;
    int sky;
    float wetness;
};

void Weather_Bind(const TsmHost* host);

// Registers the step observer that applies the host's weather. Call once at start.
void Weather_Install();

// Per-frame (game thread, Present hook): the host sends its weather.
void Weather_SyncTick();

// The host's weather (client; game thread, from BuildSync_Tick).
void Weather_OnRemote(const WeatherWire& w);
