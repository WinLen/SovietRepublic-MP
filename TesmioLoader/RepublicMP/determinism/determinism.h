// SPDX-License-Identifier: GPL-3.0-only
// Determinism layer and determinism probe.
//
// Goal: two machines that load the same save and run the same number of
// simulation steps end in the same state, so that later only player actions
// have to travel (lockstep). What makes the stock game non-deterministic, and
// what this module does about each (all of it only while active):
//
// Time - every step is scaled by the frame time: C3D_TIMER::Stop stores
//   QPCfrequency / elapsed ticks (FPS) at timer+0/+4, and PowerTime(x) =
//   x * 1000 / FPS. The game timer is exe+0x9D4EE0. The exe's import of
//   C3D_TIMER::Stop is redirected; after the real Stop the game timer's FPS
//   is overwritten with DETERMINISM_FIXED_FPS, so every step has the same
//   length (the speed factor the main loop multiplies in is synced anyway).
//
// Random numbers - C3DRandom_Int/Float/Double are rand() wrappers, seeded
//   from timeGetTime()/_time64(). The exe's imports of rand, srand and
//   C3DRandom_* are redirected to a counter-based generator keyed by CALL
//   SITE (return address as an exe RVA) and a per-thread, per-site counter:
//   value = mix(seed, site, n-th call of that site on this thread). Code that
//   depends on the camera or on sounds therefore has its own sequences and
//   can never shift the simulation's - no need to classify the ~180 users.
//   The counters restart (new epoch) whenever a save is loaded under the
//   probe, so both runs start from the same generator state.
//
// Math - the exe's and the engine's imports of sinf, powf, atan2f, ... come
//   from the system UCRT, whose results may differ between Windows builds
//   and CPUs (FMA3 code paths). They are redirected to this DLL's statically
//   linked CRT with FMA3 disabled - the same code on every machine.
//
// Threads and local input inside the step - also only while active:
//   - the "Filling" worker (FUN_1402593d0) normally runs in parallel with
//     the rest of the step and both modify vehicles and citizens: the main
//     thread now waits for it right after starting it (SetEvent import);
//   - the off-road pathfinder thread (FUN_1405d0ae0 via _beginthread in
//     FUN_1406e1930) delivers its route whenever it finishes: it now runs
//     synchronously inside the call;
//   - FUN_140139a70 / FUN_140699420 read a held key (and the real FPS, which
//     the fixed timer covers): GetKeyDown answers "not pressed" there.
//
// Probe (offline, one machine): saves the current game as "mp_probe", loads
// it, runs N steps recording a state hash (determinism/state_hash) every K steps,
// loads it again, runs again, and compares. Identical sequences = the
// simulation is deterministic under this layer; otherwise the first
// differing step and subsystem point at the next source to fix.
#pragma once

#include "tesmio_plugin.h"

#define DETERMINISM_FIXED_FPS 30.0f

void Determinism_Bind(const TsmHost* host);

// Installs the import redirections and the step/load hooks (inactive until
// Determinism_SetActive). Call once from TsmPluginStart.
bool Determinism_Install();

// Turns the fixed timer and the call-site generator on/off. Turning it on
// (or calling Determinism_Restart) starts a new generator epoch.
void Determinism_SetActive(bool active, unsigned long long seed);
bool Determinism_IsActive();
void Determinism_Restart();

// Simulation steps since the layer was last activated (main thread).
unsigned long long Determinism_StepCount();
// A lockstep session's pause and tick speed (1 normal, 2 fast, 3 Hyper, 4 Plaid; the host's on the player).
void Determinism_SessionSpeed(bool* paused, int* speed);
// Sub-tick phase [0..1) between simulation ticks for smooth visual interpolation on held frames.
float Determinism_RenderAlpha();

struct DeterminismDiagInfo {
    unsigned long long frameBeats;
    int framePhase; // 1 Prepare, 2 step Early, 3 held Early, 4 after Early, 5 Late, 6 after Late
    bool isStepFrame;
    double tickDebt;
    double subTickAlpha;
};
void Determinism_GetDiagInfo(DeterminismDiagInfo* out);

// Called after a complete tick (Early + Late) while active; after native Early otherwise.
// Main thread, whether or not the layer
// is active - for modules that sample or set state at a fixed point of the
// step (session/sync_check, session/weather). Up to DETERMINISM_MAX_OBSERVERS.
#define DETERMINISM_MAX_OBSERVERS 4
typedef void (*StepObserverFn)(void);
void Determinism_AddStepObserver(StepObserverFn fn);

// Money trace (Debug window, off by default): every PC keeps its balances and
// its construction sites after each tick of a lockstep session, and on a
// desync logs the ticks up to the one that differed - the two PCs' logs then
// show the tick and the amount where the money split.
void Determinism_SetMoneyTrace(bool on);
bool Determinism_GetMoneyTrace();
void Determinism_DumpMoneyTrace(unsigned long long tick);

// Probe control (UI). steps: complete ticks per run, interval: ticks between hashes.
// Fuzz: run 2 inserts held frames; both runs change speed every 300 ticks.
// recordCommands: the player builds during run 1; the actions become commands
// (lockstep/commands), and run 2 makes the recorded ones after the same ticks.
bool Probe_Start(int steps, int interval, bool fuzzFrames = false, bool recordCommands = false);
bool Probe_IsRunning();
// Current state / last result, for the Debug window.
const char* Probe_Status();

// Frame admission and native phase boundaries. Only used while the layer is on.
void Determinism_BeforePrepare();
void Determinism_BeforeLate();
void Determinism_AfterLate();
bool Determinism_IsStepFrame();

// This PC's own tool work on a tick's frame (Prepare, the tool handlers in
// Late; determinism/sim_patches): random draws are private and the actions
// become commands, as on a held frame (Determinism_InStepFrame is false).
void Determinism_ToolScopeBegin();
void Determinism_ToolScopeEnd();
// Lockstep session (session/save_sync thread). BeginSession turns the layer on;
// the next world load starts the shared simulation (both PCs load the same
// save). EndSession turns it off again.
bool Determinism_BeginSession(bool host);
bool Determinism_SessionRunning();
// A world load for the layer is pending (a session's or the probe's).
bool Determinism_WorldLoading();
void Determinism_EndSession();
// Any thread: a tick is being run in this frame (from Prepare to the end of
// Late) - the early worker's calls included.
bool Determinism_InStepFrame();
// F10 queues a toggle; no speed word changes in the middle of a tick.
bool Determinism_RequestPauseToggle();
// Around the interface routine (render thread): it shows the speed the player
// asked for instead of the 0 the frame keeps outside Early; a key it handled
// there becomes a request, the word goes back to 0 after it.
void Determinism_UiSpeedBegin();
void Determinism_UiSpeedEnd();
void Determinism_PrivateBegin();
void Determinism_PrivateEnd();
void Determinism_SetPrivateWorker(DWORD thread);
class Determinism_PrivateScope {
    bool on_;
public:
    explicit Determinism_PrivateScope(bool on = true) : on_(on && Determinism_IsActive()) { if (on_) Determinism_PrivateBegin(); }
    ~Determinism_PrivateScope() { if (on_) Determinism_PrivateEnd(); }
    Determinism_PrivateScope(const Determinism_PrivateScope&) = delete;
    Determinism_PrivateScope& operator=(const Determinism_PrivateScope&) = delete;
};
