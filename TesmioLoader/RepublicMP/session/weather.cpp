// SPDX-License-Identifier: GPL-3.0-only
// See weather.h.
#include "weather.h"
#include "../determinism/determinism.h"
#include "../world_sync/buildings/build_sync.h"
#include "../ui/sync_block.h"
#include <math.h>

#define WORLD_RVA 0x9D4F10
#define W_SNOWING 0xE28
#define W_SNOW_TIMER 0xE2C
#define W_TEMPERATURE 0xE30
#define W_TEMPERATURE_TARGET 0xE34
#define W_TEMPERATURE_TIMER 0xE38
#define W_WIND 0xE60
#define W_WIND_TARGET 0xE64
#define W_WIND_TIMER 0xE68
#define W_SKY 0xE70
#define W_WETNESS 0xE78
#define W_DAY_START_RESET 0x598      // zeroed by FUN_140333bd0 before a sky switch
#define SET_SKY_RVA 0x334020         // FUN_140334020(_, type, -1, blend)
#define SKY_BLEND 0.001f             // DAT_140909afc, the blend FUN_140333bd0 uses

void Weather_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static CRITICAL_SECTION g_weatherLock;
static WeatherWire g_received;
static bool g_hasReceived = false;
static ULONGLONG g_lastSendMs = 0;
static bool g_loggedFirst = false;
static int g_hostSky = -1;          // client: the host's last sky (main thread), -1 = none

static BYTE* World() { return g_exeBase + WORLD_RVA; }

static bool ReadWeather(WeatherWire* w)
{
    __try {
        BYTE* p = World();
        w->temperature = *(float*)(p + W_TEMPERATURE);
        w->temperatureTarget = *(float*)(p + W_TEMPERATURE_TARGET);
        w->temperatureTimer = *(float*)(p + W_TEMPERATURE_TIMER);
        w->wind = *(float*)(p + W_WIND);
        w->windTarget = *(float*)(p + W_WIND_TARGET);
        w->windTimer = *(float*)(p + W_WIND_TIMER);
        w->snowing = *(int*)(p + W_SNOWING);
        w->snowTimer = *(float*)(p + W_SNOW_TIMER);
        w->sky = *(int*)(p + W_SKY);
        w->wetness = *(float*)(p + W_WETNESS);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

typedef void (*SetSkyFn)(__int64 unused, int type, int index, float blend);

static void WriteWeather(const WeatherWire& w)
{
    __try {
        BYTE* p = World();
        *(float*)(p + W_TEMPERATURE) = w.temperature;
        *(float*)(p + W_TEMPERATURE_TARGET) = w.temperatureTarget;
        *(float*)(p + W_TEMPERATURE_TIMER) = w.temperatureTimer;
        *(float*)(p + W_WIND) = w.wind;
        *(float*)(p + W_WIND_TARGET) = w.windTarget;
        *(float*)(p + W_WIND_TIMER) = w.windTimer;
        *(int*)(p + W_SNOWING) = w.snowing;
        *(float*)(p + W_SNOW_TIMER) = w.snowTimer;
        *(float*)(p + W_WETNESS) = w.wetness;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Switches the sky the way FUN_140333bd0 does (it also zeroes +0x598 on a
// switch). False if it already was `sky` or could not be read.
static bool ApplySky(int sky, int* previous)
{
    if (sky < 0 || sky > 3) return false;
    __try {
        BYTE* p = World();
        int cur = *(int*)(p + W_SKY);
        if (cur == sky) return false;
        *(int*)(p + W_SKY) = sky;
        *(int*)(p + W_DAY_START_RESET) = 0;
        ((SetSkyFn)(g_exeBase + SET_SKY_RVA))(0, sky, -1, SKY_BLEND);
        *previous = cur;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool Plausible(const WeatherWire& w)
{
    return isfinite(w.temperature) && isfinite(w.temperatureTarget) && isfinite(w.temperatureTimer) &&
           isfinite(w.wind) && isfinite(w.windTarget) && isfinite(w.windTimer) && isfinite(w.snowTimer) &&
           isfinite(w.wetness) && fabsf(w.temperature) < 100.0f && w.wetness >= 0.0f && w.wetness <= 1.5f;
}

// Main thread, right after a simulation step.
//
// Only the RAIN decision is enforced on the sky, after every step. Sunrise /
// sunset (1) and night (2) are worked out by FUN_140333bd0 from the local
// clock every frame, and the player's clock is up to an hour off the host's
// between corrections: forcing the host's 0/1/2 made the two fight every
// frame at dawn and dusk. Rain (3) is the one random part - rolled at the
// start of a day - and the game keeps it once set, so the host's rain is
// re-applied right after the game's own roll (no second-long flash of the
// wrong weather), and the time-of-day sky is left to the local clock.
//
// Should the game switch it back again and again anyway, the correction
// stops for SKY_BACKOFF_MS instead of fighting.
#define SKY_FIGHT_WINDOW_MS 2000
#define SKY_FIGHT_LIMIT 4
#define SKY_BACKOFF_MS 10000

static ULONGLONG g_skyWindowStart = 0;
static int g_skyFixes = 0;
static ULONGLONG g_skyBackoffUntil = 0;

static void KeepHostRain()
{
    if (g_hostSky < 0) return;
    int cur;
    __try {
        cur = *(int*)(World() + W_SKY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if ((cur == 3) == (g_hostSky == 3)) return; // same rain state - time of day is local
    ULONGLONG now = GetTickCount64();
    if (now < g_skyBackoffUntil) return;
    if (now - g_skyWindowStart > SKY_FIGHT_WINDOW_MS) {
        g_skyWindowStart = now;
        g_skyFixes = 0;
    }
    if (++g_skyFixes > SKY_FIGHT_LIMIT) {
        g_skyBackoffUntil = now + SKY_BACKOFF_MS;
        Logf("republicmp  WEATHER: the game keeps switching the sky back - leaving it alone for %ds",
             SKY_BACKOFF_MS / 1000);
        return;
    }
    int previous;
    if (ApplySky(g_hostSky, &previous))
        Logf("republicmp  WEATHER: sky %d -> %d (host's %s)", previous, g_hostSky,
             g_hostSky == 3 ? "rain" : "dry weather");
}

static void OnStep()
{
    if (BuildSync_IsHostRole() || !BuildSync_IsConnected() || SyncBlock_IsActive()) {
        g_hostSky = -1;
        if (g_hasReceived) { // a block from before the pause/drop - the next one comes within a second
            EnterCriticalSection(&g_weatherLock);
            g_hasReceived = false;
            LeaveCriticalSection(&g_weatherLock);
        }
        return;
    }
    if (g_hasReceived) {
        WeatherWire w;
        EnterCriticalSection(&g_weatherLock);
        w = g_received;
        g_hasReceived = false;
        LeaveCriticalSection(&g_weatherLock);
        if (!g_loggedFirst) {
            g_loggedFirst = true;
            Logf("republicmp  WEATHER: following the host - %.1f°C, wind %.0f, sky %d, snowing %d", w.temperature,
                 w.wind, w.sky, w.snowing);
        }
        WriteWeather(w);
        g_hostSky = w.sky;
    }
    KeepHostRain();
}

void Weather_Install()
{
    InitializeCriticalSection(&g_weatherLock);
    Determinism_AddStepObserver(&OnStep);
}

void Weather_SyncTick()
{
    bool syncing = BuildSync_IsConnected() && !SyncBlock_IsActive();
    if (!syncing) {
        g_loggedFirst = false;
        return;
    }
    if (!BuildSync_IsHostRole()) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastSendMs < WEATHER_SYNC_INTERVAL_MS) return;
    g_lastSendMs = now;
    WeatherWire w;
    if (ReadWeather(&w)) BuildSync_SendWeather(w);
}

void Weather_OnRemote(const WeatherWire& w)
{
    if (BuildSync_IsHostRole() || !Plausible(w)) return;
    EnterCriticalSection(&g_weatherLock);
    g_received = w;
    g_hasReceived = true;
    LeaveCriticalSection(&g_weatherLock);
}
