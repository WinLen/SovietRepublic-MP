// SPDX-License-Identifier: GPL-3.0-only
// See determinism.h.
#include "determinism.h"
#include "state_hash.h"
#include "sim_patches.h"
#include "probe_trace.h"
#include "probe_watch.h"
#include "held_frame.h"
#include "../lockstep/commands.h"
#include "../lockstep/lockstep_net.h"
#include "../game/save_load.h"
#include "../session/game_speed.h"
#include "../session/save_sync.h" // SaveSync_IsBusy - no probe during a session
#include "../session/two_states.h"
#include <intrin.h>           // _ReturnAddress
#include <math.h>
#include <stdio.h>
#include <unordered_map>

#pragma intrinsic(_ReturnAddress)

#define GAME_TIMER_RVA 0x9D4EE0
#define FAST_FACTOR_RVA 0x909D5C
// The main loop's other speed-2 factors (105d66 / 105d9f): Hyper (left Ctrl +
// Numpad1, or turbo) and Plaid (left Ctrl + Numpad2, or the fast button held).
#define HYPER_FACTOR_RVA 0x909C14   // 0.05
#define PLAID_FACTOR_RVA 0x909B90   // 0.01
#define KEYS_RVA 0xA54B90           // C3D_INPUT key bytes
#define KEY_LCTRL 0x1D
#define KEY_NUMPAD1 0x4F
#define KEY_NUMPAD2 0x50
#define FAST_HELD_RVA (0x9D4F10 + 0x5A2) // the speed panel's fast button held
#define SIM_STEP_RVA 0x30C790   // FUN_14030c790(world): one simulation step
#define LOAD_WORLD_RVA 0x430FC0 // FUN_140430fc0(): loads the save named in the load globals

#define DLL_C3D "C3DDLL64.dll"
#define DLL_CRT_UTILITY "api-ms-win-crt-utility-l1-1-0.dll"
#define DLL_CRT_MATH "api-ms-win-crt-math-l1-1-0.dll"

#define PROBE_SAVE_NAME "mp_probe"
#define PROBE_MAX_SAMPLES 4096
#define PROBE_SEED 0x5EED5EED12345678ull

// Prologues of the two inline-hooked functions (SOVIET64.exe, PE 6A3EB6AD):
// mov rax,rsp / push rdi / sub rsp,60h / mov [rax+10h],rbx / [rax+18h],rbp / [rax+20h],rsi
static const unsigned char kSimStepExpect[20] = {
    0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x89,
    0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20,
};
// push rbp / push r12..r15 / lea rbp,[rsp-0C800h]
static const unsigned char kLoadWorldExpect[18] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0x00, 0x38, 0xFF, 0xFF,
};

void Determinism_Bind(const TsmHost* host)
{
    TsmBind(host);
    HeldFrame_Bind(host);
    Commands_Bind(host);
    LockstepNet_Bind(host);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile bool g_active = false;
static volatile LONG g_epoch = 1;
static unsigned long long g_seed = 0;
static volatile unsigned long long g_stepCount = 0;
static StepObserverFn g_stepObservers[DETERMINISM_MAX_OBSERVERS] = {};
static int g_stepObserverCount = 0;
static bool g_mathRedirected = false;
// For the probe: the simulation thread's draws inside Early and Late in this epoch
// (count and a hash of site and value) - a split shows here first. The
// rest of the main thread's frame (camera, tools, interface) is left out.
static volatile DWORD g_simThread = 0;
static volatile bool g_inEarly = false;
static unsigned long long g_simDraws = 0, g_simDrawHash = 0;
// Set while the main thread waits for the early worker inside Early: the
// worker's draws then go to the probe's ledger too.
static volatile bool g_workerPhase = false;
static void LedgerDraw(unsigned site, int value, bool worker);

// Game call sites whose numbers only feed sound and visuals and run for what
// the camera sees or hears (vehicle sounds, electric wire sparks, vehicle
// animation starts, ambient sounds): each has its own stream here, so they
// shift nothing else, and the probe leaves them out of its counts.
// (by SoylentGreed)
static const unsigned kPresentationSites[] = {
    0x6A3B75, 0x6A4B83, 0x6A5164, 0x6AAC3B, // vehicle sounds
    0x699AB7, 0x6A7686,                     // electric wire sparks
    0x66DA63, 0x66FCE0, 0x6701A0,           // vehicle animation starts
    0x13D748, 0x13D7DE,                     // ambient sounds
};
static bool IsPresentationSite(unsigned site)
{
    for (unsigned s : kPresentationSites)
        if (s == site) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Call-site generator
// ---------------------------------------------------------------------------

struct SiteCounters {
    LONG epoch;
    std::unordered_map<unsigned, unsigned> calls;
};
static thread_local SiteCounters* t_sites = nullptr;
static thread_local SiteCounters* t_privateSites = nullptr;
static thread_local unsigned t_privateDepth = 0;
static volatile LONG g_privateWorker = 0;
void Determinism_PrivateBegin() { ++t_privateDepth; }
void Determinism_PrivateEnd() { --t_privateDepth; }
void Determinism_SetPrivateWorker(DWORD thread) { InterlockedExchange(&g_privateWorker, (LONG)thread); }

static inline unsigned long long SplitMix(unsigned long long x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// 0..32767, like the CRT's rand(). site = return address of the game's call.
static int SiteRand(void* returnAddress)
{
    unsigned site = (unsigned)((BYTE*)returnAddress - g_exeBase);
    // Held presentation and UI can call the same native routines as a tick.
    // Separate counters keep any number of local frames out of that stream.
    bool privateDraw = t_privateDepth || GetCurrentThreadId() == (DWORD)g_privateWorker;
    SiteCounters*& counters = privateDraw ? t_privateSites : t_sites;
    if (!counters) counters = new SiteCounters{ 0, {} };
    LONG epoch = g_epoch;
    if (counters->epoch != epoch) {
        counters->calls.clear();
        counters->epoch = epoch;
    }
    unsigned n = counters->calls[site]++;
    unsigned long long x = SplitMix(g_seed ^ ((unsigned long long)site << 32) ^ n);
    int value = (int)((x >> 33) & 0x7FFF);
    if (!privateDraw && g_inEarly && !IsPresentationSite(site)) {
        if (GetCurrentThreadId() == g_simThread) {
            g_simDraws++;
            g_simDrawHash = (g_simDrawHash ^ (((unsigned long long)site << 32) | (unsigned)value)) * 1099511628211ull;
            LedgerDraw(site, value, false);
        } else if (g_workerPhase) {
            LedgerDraw(site, value, true);
        }
    }
    return value;
}

typedef int (__cdecl* RandFn)(void);
typedef void (__cdecl* SrandFn)(unsigned);
typedef unsigned (__cdecl* RandIntFn)(unsigned);
typedef float (__cdecl* RandFloatFn)(float, float);
typedef double (__cdecl* RandDoubleFn)(double, double);
typedef void (__cdecl* RandStartFn)(void);
static RandFn o_rand;
static SrandFn o_srand;
static RandIntFn o_randInt;
static RandFloatFn o_randFloat;
static RandDoubleFn o_randDouble;
static RandStartFn o_randStart;

// Same arithmetic as the engine's C3DRandom_* (C3DDLL64 0x77f10..0x77f80):
// Int = rand() % n, Float = rand() / 32767.0f * (b - a) + a, Double alike.
static __declspec(noinline) int __cdecl H_rand(void)
{
    if (!g_active) return o_rand();
    return SiteRand(_ReturnAddress());
}
static __declspec(noinline) void __cdecl H_srand(unsigned seed)
{
    if (!g_active) o_srand(seed);
}
static __declspec(noinline) unsigned __cdecl H_randInt(unsigned n)
{
    if (!g_active) return o_randInt(n);
    return (unsigned)SiteRand(_ReturnAddress()) % n;
}
static __declspec(noinline) float __cdecl H_randFloat(float a, float b)
{
    if (!g_active) return o_randFloat(a, b);
    return ((float)SiteRand(_ReturnAddress()) / 32767.0f) * (b - a) + a;
}
static __declspec(noinline) double __cdecl H_randDouble(double a, double b)
{
    if (!g_active) return o_randDouble(a, b);
    return ((double)SiteRand(_ReturnAddress()) / 32767.0) * (b - a) + a;
}
static __declspec(noinline) void __cdecl H_randStart(void)
{
    if (!g_active) o_randStart();
}

// ---------------------------------------------------------------------------
// Fixed timer
// ---------------------------------------------------------------------------

typedef void (*TimerStopFn)(void* timer);
static TimerStopFn o_timerStop, o_timerStart;

// The frame loop's first call (105c90): the timer is started and rescaled for
// the game speed right after. The render thread must not be reading it then.
static void H_timerStart(void* timer)
{
    if (g_active && (BYTE*)timer == g_exeBase + GAME_TIMER_RVA) SimPatches_WaitRender();
    o_timerStart(timer);
}

static void H_timerStop(void* timer)
{
    o_timerStop(timer);
    if (g_active && (BYTE*)timer == g_exeBase + GAME_TIMER_RVA) {
        *(float*)timer = DETERMINISM_FIXED_FPS;
        *(float*)((BYTE*)timer + 4) = DETERMINISM_FIXED_FPS;
        *(int*)((BYTE*)timer + 8) = (int)DETERMINISM_FIXED_FPS;
    }
}

// ---------------------------------------------------------------------------
// Threads and local input inside the simulation
// ---------------------------------------------------------------------------

// RVA of the function containing `addr` (follows chained unwind entries back
// to the primary one); 0 if it is not in the exe.
static DWORD ExeFunctionRva(void* addr)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)addr, &imageBase, nullptr);
    if (!rf || (BYTE*)imageBase != g_exeBase) return 0;
    for (int i = 0; i < 8; i++) {
        BYTE* info = (BYTE*)imageBase + rf->UnwindData;
        const BYTE kChainInfo = 0x4; // UNW_FLAG_CHAININFO
        if (!((info[0] >> 3) & kChainInfo)) break;
        BYTE codes = info[2];
        rf = (PRUNTIME_FUNCTION)(info + 4 + ((codes + 1) & ~1) * 2);
    }
    return rf->BeginAddress;
}

// FUN_140139a70 decides from a held key and the real FPS whether all objects
// update this step or a third of them with a tripled step; FUN_140699420
// also reads a key. Under the layer both see "not pressed".
#define KEY_SIM_FUNC1_RVA 0x139A70
#define KEY_SIM_FUNC2_RVA 0x699420

typedef bool (*GetKeyDownFn)(void* input, int key);
static GetKeyDownFn o_getKeyDown;

static __declspec(noinline) bool H_getKeyDown(void* input, int key)
{
    if (g_active) {
        DWORD fn = ExeFunctionRva(_ReturnAddress());
        if (fn == KEY_SIM_FUNC1_RVA || fn == KEY_SIM_FUNC2_RVA) return false;
    }
    return o_getKeyDown(input, key);
}

// The off-road pathfinder (FUN_1405d0ae0, started by FUN_1406e1930 through
// _beginthread) hands its route back on whichever step it finishes. Under the
// layer it runs to completion inside the call, so the vehicle picks the route
// up on the next step everywhere. Its only other effect is the handle the
// caller stores - a permanently signalled event stands in for it.
typedef uintptr_t (__cdecl* BeginThreadFn)(void (__cdecl*)(void*), unsigned, void*);
static BeginThreadFn o_beginThread;
static HANDLE g_doneThreadHandle = nullptr;

static uintptr_t __cdecl H_beginThread(void (__cdecl* start)(void*), unsigned stackSize, void* arg)
{
    if (!g_active) return o_beginThread(start, stackSize, arg);
    start(arg);
    return (uintptr_t)g_doneThreadHandle;
}

// The "Filling" worker (FUN_1402593d0) runs alongside the rest of the
// simulation step in FUN_14030c790 and both modify vehicles and citizens.
// Under the layer the main thread waits for the worker right after starting
// it, so the two never overlap and always run in the same order.
#define FILLING_OBJ_RVA 0x995290       // DAT_140995290: the worker's object
#define FILLING_DONE_OFFSET 0x59       // byte: 1 when the step's work is finished
#define FILLING_START_EVENT_OFFSET 0x60

typedef BOOL (WINAPI* SetEventFn)(HANDLE);
static SetEventFn o_setEvent;

static BOOL WINAPI H_setEvent(HANDLE h)
{
    BOOL r = o_setEvent(h);
    if (g_active) {
        BYTE* obj = *(BYTE**)(g_exeBase + FILLING_OBJ_RVA);
        if (obj && h == *(HANDLE*)(obj + FILLING_START_EVENT_OFFSET)) {
            volatile BYTE* done = obj + FILLING_DONE_OFFSET;
            g_workerPhase = true;
            while (!*done) YieldProcessor();
            g_workerPhase = false;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

struct MathImport { const char* name; void* impl; };

// This DLL's static CRT (/MT): identical code on every machine.
static const MathImport kMath[] = {
    { "sin",    (void*)static_cast<double (*)(double)>(&sin) },
    { "cos",    (void*)static_cast<double (*)(double)>(&cos) },
    { "acos",   (void*)static_cast<double (*)(double)>(&acos) },
    { "log",    (void*)static_cast<double (*)(double)>(&log) },
    { "floor",  (void*)static_cast<double (*)(double)>(&floor) },
    { "sqrt",   (void*)static_cast<double (*)(double)>(&sqrt) },
    { "pow",    (void*)static_cast<double (*)(double, double)>(&pow) },
    { "fmod",   (void*)static_cast<double (*)(double, double)>(&fmod) },
    { "sinf",   (void*)static_cast<float (*)(float)>(&sinf) },
    { "cosf",   (void*)static_cast<float (*)(float)>(&cosf) },
    { "tanf",   (void*)static_cast<float (*)(float)>(&tanf) },
    { "asinf",  (void*)static_cast<float (*)(float)>(&asinf) },
    { "acosf",  (void*)static_cast<float (*)(float)>(&acosf) },
    { "atanf",  (void*)static_cast<float (*)(float)>(&atanf) },
    { "sqrtf",  (void*)static_cast<float (*)(float)>(&sqrtf) },
    { "floorf", (void*)static_cast<float (*)(float)>(&floorf) },
    { "ceilf",  (void*)static_cast<float (*)(float)>(&ceilf) },
    { "expf",   (void*)static_cast<float (*)(float)>(&expf) },
    { "log10f", (void*)static_cast<float (*)(float)>(&log10f) },
    { "atan2f", (void*)static_cast<float (*)(float, float)>(&atan2f) },
    { "powf",   (void*)static_cast<float (*)(float, float)>(&powf) },
    { "fmodf",  (void*)static_cast<float (*)(float, float)>(&fmodf) },
    { "lroundf", (void*)static_cast<long (*)(float)>(&lroundf) },
};

// Once, on first activation: both machines then run the same math code.
static void RedirectMath()
{
    if (g_mathRedirected) return;
    g_mathRedirected = true;
    _set_FMA3_enable(0);
    int patched = 0;
    for (const MathImport& m : kMath) {
        void* orig = nullptr;
        char label[48];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "math %s", m.name);
        if (FindIatSlot(g_exe, DLL_CRT_MATH, m.name) && PatchIat(g_exe, DLL_CRT_MATH, m.name, m.impl, &orig, label))
            patched++;
        if (g_engine && FindIatSlot(g_engine, DLL_CRT_MATH, m.name) &&
            PatchIat(g_engine, DLL_CRT_MATH, m.name, m.impl, &orig, label))
            patched++;
    }
    Logf("republicmp  DETERMINISM: %d math imports redirected to the built-in CRT (FMA3 off)", patched);
}

// ---------------------------------------------------------------------------
// Step and load hooks, probe state
// ---------------------------------------------------------------------------

typedef void (*SimStepFn)(__int64 world);
typedef void (*LoadWorldFn)(void);
static SimStepFn o_simStep;
static LoadWorldFn o_loadWorld;

enum ProbeState { PROBE_IDLE = 0, PROBE_WAIT_LOAD, PROBE_RUNNING, PROBE_RUN_DONE, PROBE_FAILED };
static volatile LONG g_probeState = PROBE_IDLE;
static volatile bool g_probeBusy = false;
static int g_probeSteps = 0, g_probeInterval = 60, g_probeRun = 0;
static int g_probeSpeed = 1;
static bool g_probeFuzz = false;
// Lockstep session (session/save_sync): the next load starts the shared
// simulation; then ticks come from the host (lockstep/lockstep_net).
static volatile bool g_sessionLoadPending = false;
static volatile bool g_sessionRunning = false;
static volatile bool g_sessionArmed = false; // BeginSession turned the layer on for a session
static bool g_sessionHost = false;
#define LOCKSTEP_SEED 0x5EED10C557E90001ull
static bool g_probeRecord = false;
static size_t g_commandsRecorded = 0, g_commandsReplayed = 0;
static bool g_frameStep = false, g_frameEarly = false, g_frameFault = false, g_haveStep = false;
static bool g_framesReady = false, g_paused = false, g_buttonsArmed = false;
static int g_tickSpeed = 1, g_requestedSpeed = 1;
// Lockstep tick speeds: 1 normal, 2 fast, 3 Hyper, 4 Plaid (the last two only on
// top of fast, as in the game). The host's boost is its own keys or the player's
// request (a speed request of 10 + boost); the grant carries the tick speed.
static int g_remoteBoost = 0, g_sentBoost = 0;

// This PC's boost from the same inputs the main loop reads: 0 none, 1 Hyper, 2 Plaid.
static int LocalBoost()
{
    const BYTE* keys = g_exeBase + KEYS_RVA;
    if (keys[KEY_LCTRL] && keys[KEY_NUMPAD2]) return 2;
    if (g_exeBase[FAST_HELD_RVA]) return 2;
    if (keys[KEY_LCTRL] && keys[KEY_NUMPAD1]) return 1;
    return 0;
}

// The game's own speed (world +5a4) for a tick speed: Hyper and Plaid are fast with a smaller step.
static int GameSpeedOf(int tickSpeed) { return tickSpeed > 2 ? 2 : tickSpeed; }
static unsigned g_holdsLeft = 0, g_menuFrames = 0;
static unsigned long long g_holdCount[2] = {}, g_speedChanges[2] = {};
static volatile LONG g_pauseRequests = 0;
static unsigned g_tickTimer[4] = {}, g_uiTimer[4] = {};
static LARGE_INTEGER g_frameClock = {}, g_frameFrequency = {};
static double g_tickDebt = 0;
// For the hang report: frames started and where the current one stands
// (1 Prepare, 2 step Early, 3 held Early, 4 after Early, 5 Late, 6 after Late).
static volatile unsigned long long g_frameBeats = 0;
static volatile LONG g_framePhase = 0;
static const int kSpeedSentinel = 0x52424E53;
static void ResetFrames();
static unsigned long long g_runStep = 0;

struct ProbeSample {
    unsigned long long step;
    StateSummary s;
    unsigned long long draws, drawHash; // the simulation thread's random numbers so far
    int population;                     // world+0xc720
};
static ProbeSample g_samples[2][PROBE_MAX_SAMPLES];
static int g_sampleCount[2];

// Run 1 keeps every building and citizen at every sample (within a memory
// budget); run 2 compares its own at the same sample and logs the first
// objects that differ - which building, which field, both values.
#define PROBE_DETAIL_BUDGET (256ull << 20)
#define PROBE_DETAIL_LINES 12
static std::vector<std::vector<BuildingRecord>> g_detailBuildings;
static std::vector<std::vector<CitizenRecord>> g_detailCitizens;
static std::vector<std::vector<VehicleRecord>> g_detailVehicles;
static unsigned long long g_detailBytes = 0;
static bool g_detailFull = false, g_reportedBuildings = false, g_reportedCitizens = false, g_reportedVehicles = false;

static bool SameFloat(float a, float b) { return memcmp(&a, &b, 4) == 0; }

static void CompareBuildings(int sample, const std::vector<BuildingRecord>& a, const std::vector<BuildingRecord>& b)
{
    if (g_reportedBuildings) return;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int lines = 0;
    for (size_t i = 0; i < n && lines < PROBE_DETAIL_LINES; i++) {
        const BuildingRecord& x = a[i];
        const BuildingRecord& y = b[i];
        bool same = x.index == y.index && x.category == y.category && memcmp(x.pos, y.pos, 12) == 0 &&
                    SameFloat(x.progress, y.progress) && SameFloat(x.produced, y.produced) && x.workers == y.workers &&
                    x.storage == y.storage;
        if (same) continue;
        if (!lines)
            Logf("republicmp  PROBE: buildings first differ at step %llu (sample %d), %zu/%zu listed - run 1 / run 2:",
                 g_samples[0][sample].step, sample + 1, a.size(), b.size());
        Logf("republicmp  PROBE:   #%zu index %d/%d category %d at (%.2f,%.2f,%.2f): progress %.6f/%.6f produced %.6f/%.6f "
             "workers %d/%d storage %s", i, x.index, y.index, x.category, x.pos[0], x.pos[1], x.pos[2], x.progress,
             y.progress, x.produced, y.produced, x.workers, y.workers, x.storage == y.storage ? "same" : "DIFFERENT");
        if (x.storage != y.storage) {
            int ns = x.slots > y.slots ? x.slots : y.slots;
            for (int s = 0; s < ns && s < RECORD_SLOTS; s++)
                if (x.slotEntries[s] != y.slotEntries[s] || !SameFloat(x.slotAmount[s], y.slotAmount[s]))
                    Logf("republicmp  PROBE:     storage slot %d: %d/%d entries, amount %.6f/%.6f", s, x.slotEntries[s],
                         y.slotEntries[s], x.slotAmount[s], y.slotAmount[s]);
        }
        lines++;
    }
    if (lines || a.size() != b.size()) {
        if (!lines) Logf("republicmp  PROBE: buildings first differ at step %llu: %zu/%zu listed", g_samples[0][sample].step,
                         a.size(), b.size());
        g_reportedBuildings = true;
    }
}

static void CompareCitizens(int sample, const std::vector<CitizenRecord>& a, const std::vector<CitizenRecord>& b)
{
    if (g_reportedCitizens) return;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int lines = 0;
    for (size_t i = 0; i < n && lines < PROBE_DETAIL_LINES; i++) {
        const CitizenRecord& x = a[i];
        const CitizenRecord& y = b[i];
        if (x.home == y.home && SameFloat(x.age, y.age) && memcmp(x.pos, y.pos, 12) == 0) continue;
        if (!lines)
            Logf("republicmp  PROBE: citizens first differ at step %llu (sample %d), %zu/%zu listed - run 1 / run 2:",
                 g_samples[0][sample].step, sample + 1, a.size(), b.size());
        Logf("republicmp  PROBE:   #%zu home %d/%d age %.6f/%.6f at (%.2f,%.2f,%.2f)/(%.2f,%.2f,%.2f)", i, x.home, y.home,
             x.age, y.age, x.pos[0], x.pos[1], x.pos[2], y.pos[0], y.pos[1], y.pos[2]);
        lines++;
    }
    if (lines || a.size() != b.size()) {
        if (!lines) Logf("republicmp  PROBE: citizens first differ at step %llu: %zu/%zu listed", g_samples[0][sample].step,
                         a.size(), b.size());
        g_reportedCitizens = true;
    }
}

static void CompareVehicles(int sample, const std::vector<VehicleRecord>& a, const std::vector<VehicleRecord>& b)
{
    if (g_reportedVehicles) return;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int lines = 0;
    for (size_t i = 0; i < n && lines < PROBE_DETAIL_LINES; i++) {
        const VehicleRecord& x = a[i];
        const VehicleRecord& y = b[i];
        if (x.index == y.index && memcmp(x.pos, y.pos, 12) == 0 && x.cargo == y.cargo) continue;
        if (!lines)
            Logf("republicmp  PROBE: vehicles first differ at step %llu (sample %d), %zu/%zu listed - run 1 / run 2:",
                 g_samples[0][sample].step, sample + 1, a.size(), b.size());
        Logf("republicmp  PROBE:   #%zu vehicle %d/%d at (%.3f,%.3f,%.3f)/(%.3f,%.3f,%.3f) cargo %s", i, x.index, y.index,
             x.pos[0], x.pos[1], x.pos[2], y.pos[0], y.pos[1], y.pos[2], x.cargo == y.cargo ? "same" : "DIFFERENT");
        lines++;
    }
    if (lines || a.size() != b.size()) {
        if (!lines) Logf("republicmp  PROBE: vehicles first differ at step %llu: %zu/%zu listed", g_samples[0][sample].step,
                         a.size(), b.size());
        g_reportedVehicles = true;
    }
}

// Main thread, at a sample.
static void RecordDetail(int run, int sample)
{
    std::vector<BuildingRecord> buildings;
    std::vector<CitizenRecord> citizens;
    std::vector<VehicleRecord> vehicles;
    if (!StateHash_BuildingRecords(buildings) || !StateHash_CitizenRecords(citizens) ||
        !StateHash_VehicleRecords(vehicles))
        return;
    if (run == 0) {
        if (g_detailFull) return;
        g_detailBytes += buildings.size() * sizeof(BuildingRecord) + citizens.size() * sizeof(CitizenRecord) +
                         vehicles.size() * sizeof(VehicleRecord);
        if (g_detailBytes > PROBE_DETAIL_BUDGET) {
            g_detailFull = true;
            Logf("republicmp  PROBE: per-object detail kept for the first %d samples only (memory budget)", sample);
            return;
        }
        g_detailBuildings.push_back(std::move(buildings));
        g_detailCitizens.push_back(std::move(citizens));
        g_detailVehicles.push_back(std::move(vehicles));
        return;
    }
    if (sample >= (int)g_detailBuildings.size()) return;
    CompareBuildings(sample, g_detailBuildings[sample], buildings);
    CompareCitizens(sample, g_detailCitizens[sample], citizens);
    CompareVehicles(sample, g_detailVehicles[sample], vehicles);
}

// Every random number drawn inside Early (main thread and early worker), in
// order: run 1 records them, run 2 checks each against run 1's as it is
// drawn and logs the first that differs - its step, the game's call site
// (exe RVA of the return address) and the draws before it.
struct LedgerEntry {
    unsigned step;
    unsigned site;
    unsigned short value;
    unsigned char worker;
    unsigned char pad;
};
#define PROBE_LEDGER_MAX 4000000
static std::vector<LedgerEntry> g_ledger;
static size_t g_ledgerPos = 0;
static bool g_ledgerReported = false, g_ledgerFull = false;
static SRWLOCK g_ledgerLock = SRWLOCK_INIT;

static void LedgerDraw(unsigned site, int value, bool worker)
{
    if (g_probeState != PROBE_RUNNING) return;
    LedgerEntry e = { (unsigned)g_runStep, site, (unsigned short)value, (unsigned char)(worker ? 1 : 0), 0 };
    AcquireSRWLockExclusive(&g_ledgerLock);
    if (g_probeRun == 0) {
        if (g_ledger.size() < PROBE_LEDGER_MAX) g_ledger.push_back(e);
        else g_ledgerFull = true;
    } else if (!g_ledgerReported && g_ledgerPos < g_ledger.size()) {
        const LedgerEntry& r = g_ledger[g_ledgerPos];
        if (r.step != e.step || r.site != e.site || r.value != e.value || r.worker != e.worker) {
            g_ledgerReported = true;
            Logf("republicmp  PROBE: random numbers first differ at draw %zu: run 1 step %u site exe+0x%X value %u %s | "
                 "run 2 step %u site exe+0x%X value %u %s", g_ledgerPos, r.step, r.site, r.value, r.worker ? "worker" : "main",
                 e.step, e.site, e.value, e.worker ? "worker" : "main");
            size_t from = g_ledgerPos > 8 ? g_ledgerPos - 8 : 0;
            for (size_t i = from; i < g_ledgerPos; i++)
                Logf("republicmp  PROBE:   before it: draw %zu step %u site exe+0x%X value %u %s", i, g_ledger[i].step,
                     g_ledger[i].site, g_ledger[i].value, g_ledger[i].worker ? "worker" : "main");
        } else {
            g_ledgerPos++;
        }
    }
    ReleaseSRWLockExclusive(&g_ledgerLock);
}

static void ResetDetail()
{
    std::vector<LedgerEntry>().swap(g_ledger);
    g_ledgerPos = 0;
    g_ledgerReported = g_ledgerFull = false;
    std::vector<std::vector<BuildingRecord>>().swap(g_detailBuildings);
    std::vector<std::vector<CitizenRecord>>().swap(g_detailCitizens);
    std::vector<std::vector<VehicleRecord>>().swap(g_detailVehicles);
    g_detailBytes = 0;
    g_detailFull = g_reportedBuildings = g_reportedCitizens = g_reportedVehicles = false;
    ProbeTrace_Reset();
    ProbeWatch_Reset();
}

static CRITICAL_SECTION g_statusLock;
static char g_probeStatus[320] = "not run yet";

static void SetProbeStatus(const char* fmt, ...)
{
    char body[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_statusLock);
    strcpy_s(g_probeStatus, body);
    LeaveCriticalSection(&g_statusLock);
    Logf("republicmp  PROBE: %s", body);
}

// Frame counters the simulation steps by that live outside the world and
// outside the save, and are never reset: a load starts from whatever the
// frames before it left. The building pass updates a third of the
// buildings per pass by exe+0x9962a8 mod 3 (13cb52 inc, 13dbdb read); Early
// updates one of three agent buckets per frame by exe+0x9963f0 (30cd1e..).
// Two loads of the same save (two PCs) then step different buildings and
// agents from the first frame. A probe run starts both at 0.
static const unsigned int kFrameCounters[] = { 0x9962A8, 0x9963F0 };

// The exe's writable section (.data), compared between the two runs right
// after the load: whatever differs there is state the save does not restore.
#define DATA_RVA 0x992000
#define DATA_SIZE 0xC4718
#define DATA_DIFF_LINES 150
#define DATA_DIFF_LINES_REST 40
#define WORLD_RVA_PROBE 0x9D4F10
#define WORLD_SIZE_PROBE 0x15000 // up to +0x14d38 and a little beyond
static std::vector<BYTE> g_dataSnapshot;

// A qword that looks like a user-mode heap pointer (each load allocates anew,
// so pointers always differ - not state the simulation compares).
static bool PointerLike(unsigned long long q)
{
    unsigned hi = (unsigned)(q >> 32);
    return hi >= 0x1 && hi <= 0x7fff && (unsigned)q != 0;
}

struct DataDiffTotals {
    int ranges, pointers, lines;
    size_t bytes;
};

// Differing non-pointer qword runs of .data in [from, to), logged up to maxLines.
static void ScanData(size_t from, size_t to, int maxLines, DataDiffTotals& t)
{
    const BYTE* now = g_exeBase + DATA_RVA;
    const BYTE* was = g_dataSnapshot.data();
    int lines = 0;
    for (size_t i = from; i + 8 <= to;) {
        unsigned long long a = *(const unsigned long long*)(was + i), b = *(const unsigned long long*)(now + i);
        if (a == b) { i += 8; continue; }
        if (PointerLike(a) && PointerLike(b)) { t.pointers++; i += 8; continue; }
        size_t start = i;
        i += 8;
        while (i + 8 <= to) { // the run of differing, non-pointer qwords
            unsigned long long x = *(const unsigned long long*)(was + i), y = *(const unsigned long long*)(now + i);
            if (x == y || (PointerLike(x) && PointerLike(y))) break;
            i += 8;
        }
        size_t len = i - start;
        t.ranges++;
        t.bytes += len;
        if (lines < maxLines) {
            char ha[3 * 16 + 1] = "", hb[3 * 16 + 1] = "";
            for (size_t k = 0; k < len && k < 16; k++) {
                _snprintf_s(ha + 3 * k, 4, _TRUNCATE, "%02x ", was[start + k]);
                _snprintf_s(hb + 3 * k, 4, _TRUNCATE, "%02x ", now[start + k]);
            }
            unsigned rva = (unsigned)(DATA_RVA + start);
            char where[32];
            if (rva >= WORLD_RVA_PROBE && rva < WORLD_RVA_PROBE + WORLD_SIZE_PROBE)
                _snprintf_s(where, sizeof(where), _TRUNCATE, "world+0x%X", rva - WORLD_RVA_PROBE);
            else
                _snprintf_s(where, sizeof(where), _TRUNCATE, "exe+0x%X", rva);
            Logf("republicmp  PROBE:   .data %s (%zu bytes): run 1 %s| run 2 %s", where, len, ha, hb);
            lines++;
            t.lines++;
        }
    }
}

// The world object first (that is what the save restores), then the rest.
static void CompareData()
{
    const size_t worldFrom = WORLD_RVA_PROBE - DATA_RVA, worldTo = worldFrom + WORLD_SIZE_PROBE;
    DataDiffTotals world = {}, rest = {};
    ScanData(worldFrom, worldTo, DATA_DIFF_LINES, world);
    Logf("republicmp  PROBE: right after the load, the world object differs between the runs in %d range(s) / %zu byte(s), "
         "besides %d heap pointer(s)", world.ranges, world.bytes, world.pointers);
    ScanData(0, worldFrom, DATA_DIFF_LINES_REST, rest);
    ScanData(worldTo, DATA_SIZE, DATA_DIFF_LINES_REST - rest.lines, rest);
    Logf("republicmp  PROBE: the rest of .data differs in %d range(s) / %zu byte(s), besides %d heap pointer(s)%s",
         rest.ranges, rest.bytes, rest.pointers, rest.ranges > rest.lines ? " (first ones listed above)" : "");
}

// The game's search routine (424430) keeps its work and results in three
// vectors of the world (+14478, +14490, +144a8: 8-, 4- and 0x68-byte
// entries), and the chooser (420350) picks from the results. A load does not
// empty them, so the first choice after it saw what the earlier game last
// searched. Each probe run starts with them empty (capacity kept).
#define SEARCH_POOL_OFFSET 0x14478
static void EmptySearchPool()
{
    BYTE* pool = g_exeBase + WORLD_RVA_PROBE + SEARCH_POOL_OFFSET;
    static const size_t strides[3] = { 8, 4, 0x68 };
    long long counts[3] = {};
    for (int v = 0; v < 3; v++) {
        BYTE** header = (BYTE**)(pool + v * 0x18);
        if (header[0] && header[1] >= header[0]) counts[v] = (header[1] - header[0]) / (long long)strides[v];
        header[1] = header[0];
    }
    Logf("republicmp  PROBE: search pool after the load: %lld / %lld / %lld entries - emptied", counts[0], counts[1], counts[2]);
}

static void HookedLoadWorld(void)
{
    // A probe run starts its generator epoch exactly at the load, so the
    // random numbers the load itself draws are the same in both runs.
    bool session = g_sessionLoadPending;
    if (!session) {
        TwoStates_BeforeAnyWorldLoad();
    }
    if (g_probeState == PROBE_WAIT_LOAD || session) {
        if (!SimPatches_Apply()) {
            if (session) {
                g_sessionLoadPending = false;
                Logf("republicmp  LOCKSTEP: simulation patches refused - the session cannot run in lockstep");
            } else {
                InterlockedExchange(&g_probeState, PROBE_FAILED);
            }
            o_loadWorld();
            TwoStates_OnAnyWorldLoaded(false);
            return;
        }
        Determinism_Restart();
        g_simDraws = 0;
        g_simDrawHash = 1469598103934665603ull;
        g_runStep = 0;
        for (unsigned int rva : kFrameCounters) {
            Logf("republicmp  PROBE: frame counter exe+0x%X was %d - set to 0", rva, *(int*)(g_exeBase + rva));
            *(int*)(g_exeBase + rva) = 0;
        }
    }
    o_loadWorld();
    if (session) {
        // The same start on both PCs as a probe run gets: the shared
        // simulation begins at this load, paused until a player resumes.
        EmptySearchPool();
        SimPatches_ResetSearches();
        ResetFrames();
        g_paused = true;
        g_stepCount = 0;
        g_sessionLoadPending = false;
        g_sessionRunning = true;
        TwoStates_OnAnyWorldLoaded(true);
        Commands_SetSession(g_sessionHost ? COMMANDS_HOST : COMMANDS_PLAYER);
        Logf("republicmp  LOCKSTEP: shared simulation starts at this load (%s)", g_sessionHost ? "host" : "player");
        return;
    }
    TwoStates_OnAnyWorldLoaded(false);
    if (g_probeState == PROBE_WAIT_LOAD) {
        EmptySearchPool();
        SimPatches_ResetSearches();
        g_runStep = 0;
        ResetFrames();
        WriteGameSpeed(g_probeSpeed); // the first step already at the probe's speed
        if (g_probeRun == 0) {
            g_dataSnapshot.assign(g_exeBase + DATA_RVA, g_exeBase + DATA_RVA + DATA_SIZE);
        } else if (g_dataSnapshot.size() == DATA_SIZE) {
            CompareData();
            std::vector<BYTE>().swap(g_dataSnapshot);
        }
        Commands_ProbeRun(g_probeRun, g_probeRecord);
        InterlockedExchange(&g_probeState, PROBE_RUNNING);
    }
}

static void RecordSample()
{
    int run = g_probeRun;
    if (g_sampleCount[run] >= PROBE_MAX_SAMPLES) return;
    ProbeSample& p = g_samples[run][g_sampleCount[run]];
    p.step = g_runStep;
    p.draws = g_simDraws;
    p.drawHash = g_simDrawHash;
    p.population = SimPatches_LastPopulation();
    if (!StateHash_Compute(&p.s)) return;
    RecordDetail(run, g_sampleCount[run]);
    g_sampleCount[run]++;
}

// 105e56 prepares the view at speed 0. Early (30c790) and a tick's Late raise
// the native speed; UI writes are requests and a held Late sees speed 0, with
// the admitted tick's timer restored. (by SoylentGreed)
static void ResetFrames()
{
    g_frameStep = g_frameEarly = g_frameFault = g_paused = g_buttonsArmed = false;
    g_tickSpeed = g_requestedSpeed = g_probeSpeed > 1 ? 2 : 1;
    g_holdsLeft = g_menuFrames = 0;
    g_haveStep = false;
    InterlockedExchange(&g_pauseRequests, 0);
    g_tickDebt = 1.0;
    QueryPerformanceCounter(&g_frameClock);
    QueryPerformanceFrequency(&g_frameFrequency);
}

bool Determinism_IsStepFrame()
{
    return g_active && g_frameStep && GetCurrentThreadId() == g_simThread;
}

static int g_toolScope = 0; // main thread only

void Determinism_ToolScopeBegin()
{
    ++g_toolScope;
    Determinism_PrivateBegin();
}

void Determinism_ToolScopeEnd()
{
    Determinism_PrivateEnd();
    --g_toolScope;
}

bool Determinism_BeginSession(bool host)
{
    if (!g_framesReady || g_probeBusy) return false;
    g_remoteBoost = g_sentBoost = 0;
    g_sessionHost = host;
    g_sessionRunning = false;
    g_sessionArmed = true;
    Determinism_SetActive(true, LOCKSTEP_SEED);
    g_sessionLoadPending = true;
    Logf("republicmp  LOCKSTEP: the next load starts the shared simulation (%s)", host ? "host" : "player");
    return true;
}

bool Determinism_WorldLoading()
{
    return g_sessionLoadPending || g_probeState == PROBE_WAIT_LOAD;
}

bool Determinism_SessionRunning()
{
    return g_sessionRunning;
}

void Determinism_EndSession()
{
    // A session whose load refused the simulation patches left the layer on
    // with nothing running: the game stayed paused (no ticks admitted).
    if (!g_sessionRunning && !g_sessionLoadPending && !g_sessionArmed) return;
    g_sessionArmed = false;
    g_sessionRunning = false;
    g_sessionLoadPending = false;
    Commands_SetSession(COMMANDS_OFF);
    Determinism_SetActive(false, 0);
    Logf("republicmp  LOCKSTEP: session ended after %llu ticks", (unsigned long long)g_stepCount);
}

bool Determinism_InStepFrame()
{
    return g_active && g_frameStep && !(g_toolScope && GetCurrentThreadId() == g_simThread);
}

bool Determinism_RequestPauseToggle()
{
    if (!g_active) return false;
    InterlockedIncrement(&g_pauseRequests);
    return true;
}

static void TakeSpeedInput()
{
    bool wasPaused = g_paused;
    int wasSpeed = g_requestedSpeed;
    int* speed = (int*)(g_exeBase + 0x9D54B4);
    bool button = g_buttonsArmed && speed[2] != kSpeedSentinel;
    // Esc's timer hold and its remembered-speed restore are not player
    // pacing requests (105a90 / 317db0). (by SoylentGreed)
    if (g_buttonsArmed && !g_menuFrames && (*speed || button)) {
        if (button) {
            g_paused = *speed == 0;
            if (*speed > 0) g_requestedSpeed = *speed > 1 ? 2 : 1;
        } else g_paused = !g_paused;
    }
    LONG toggles = InterlockedExchange(&g_pauseRequests, 0);
    if (toggles & 1) g_paused = !g_paused;
    *speed = 0;
    speed[1] = g_requestedSpeed;
    speed[2] = kSpeedSentinel;
    g_buttonsArmed = true;
    // In a session the host owns pause and speed: the player's keys and
    // buttons become requests, and its own state stays the host's.
    if (g_sessionRunning && !g_sessionHost) {
        if (g_paused != wasPaused) LockstepNet_SendPauseToggle();
        if (g_requestedSpeed != wasSpeed) LockstepNet_SendSpeed(g_requestedSpeed);
        g_paused = wasPaused;
        g_requestedSpeed = wasSpeed;
    }
}

// The interface (317db0, render thread) draws the speed buttons from
// world+5a4, which stays 0 outside Early: the pause button looked pressed
// while the ticks ran. It runs between Late and the next frame start (the
// render wait at timer Start), never during this frame's own speed handling.
// A button click changes +5ac and is read by TakeSpeedInput as before; a
// changed speed without it is the pause key, queued as a toggle. Speed
// restores while the Esc menu closes are not requests.
static int g_uiShown = 0;

void Determinism_UiSpeedBegin()
{
    if (!g_active || !g_buttonsArmed) return;
    g_uiShown = g_paused ? 0 : g_requestedSpeed;
    *(int*)(g_exeBase + 0x9D54B4) = g_uiShown;
    TwoStates_UiBegin();
}

void Determinism_UiSpeedEnd()
{
    TwoStates_UiEnd();
    if (!g_active || !g_buttonsArmed) return;
    int* speed = (int*)(g_exeBase + 0x9D54B4);
    if (speed[2] != kSpeedSentinel) return;
    if (*speed != g_uiShown && !g_menuFrames) InterlockedIncrement(&g_pauseRequests);
    *speed = 0;
}

// Commands (lockstep/commands) are made at the start of a frame, before its
// tick: the point after `completed` ticks, the same on every PC whatever its
// frames did meanwhile. They also arrive on held frames, so the timer reads
// the same for them either way: the fixed rate and the paused byte, as on a
// held frame (Esc menu byte cleared), put back after.
static void MakeCommands(unsigned long long completed)
{
    if (!Commands_Due(completed)) return;
    unsigned* timer = (unsigned*)(g_exeBase + GAME_TIMER_RVA);
    unsigned saved[4];
    memcpy(saved, timer, sizeof(saved));
    *(float*)&timer[0] = DETERMINISM_FIXED_FPS;
    *(float*)&timer[1] = DETERMINISM_FIXED_FPS;
    timer[2] = (unsigned)(int)DETERMINISM_FIXED_FPS;
    timer[3] = (saved[3] & 0xFFFF0000u) | 1u;
    Commands_Boundary(completed);
    memcpy(timer, saved, sizeof(saved));
}

static void ProbeAdmission(BYTE* timer);
static void SessionAdmission(BYTE* timer);

// ---------------------------------------------------------------------------
// Money trace (see determinism.h)
// ---------------------------------------------------------------------------

struct MoneyTick {
    unsigned long long tick;
    double rub, usd;
    double progress; // construction progress (+0x604) of the sites, summed
    int sites, paid;
};
#define MONEY_TRACE_TICKS 256
#define MONEY_TRACE_DUMPED 70
static MoneyTick g_moneyTicks[MONEY_TRACE_TICKS];
static unsigned long long g_moneyTickCount = 0;
static bool g_moneyTrace = false;

void Determinism_SetMoneyTrace(bool on)
{
    g_moneyTrace = on;
    g_moneyTickCount = 0;
}

bool Determinism_GetMoneyTrace() { return g_moneyTrace; }

static void RecordMoneyTick()
{
    MoneyTick t = {};
    t.tick = g_stepCount;
    __try {
        BYTE* world = g_exeBase + 0x9D4F10;
        t.usd = *(double*)(world + 0x580);
        t.rub = *(double*)(world + 0x588);
        BYTE** it = *(BYTE***)(g_exeBase + 0x9E6A18);
        BYTE** end = *(BYTE***)(g_exeBase + 0x9E6A20);
        for (; it && it < end; ++it) {
            if (!*it) continue;
            float p = *(float*)(*it + 0x604);
            if (p >= 1.0f) continue;
            t.progress += p;
            t.sites++;
            if (*(*it + 0x608) || *(*it + 0x609)) t.paid++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    g_moneyTicks[g_moneyTickCount++ % MONEY_TRACE_TICKS] = t;
}

void Determinism_DumpMoneyTrace(unsigned long long tick)
{
    if (!g_moneyTrace || !g_moneyTickCount) return;
    unsigned long long first = g_moneyTickCount > MONEY_TRACE_TICKS ? g_moneyTickCount - MONEY_TRACE_TICKS : 0;
    Logf("republicmp  MONEY_TRACE: the ticks before %llu on this PC", tick);
    const MoneyTick* prev = nullptr;
    for (unsigned long long i = first; i < g_moneyTickCount; i++) {
        const MoneyTick& t = g_moneyTicks[i % MONEY_TRACE_TICKS];
        if (t.tick > tick || t.tick + MONEY_TRACE_DUMPED <= tick) {
            prev = &t;
            continue;
        }
        double dRub = prev ? t.rub - prev->rub : 0, dUsd = prev ? t.usd - prev->usd : 0;
        Logf("republicmp  MONEY_TRACE: tick %llu rubles %.6f (%+.6f) dollars %.6f (%+.6f), %d site(s) (%d paid), progress %.9g",
             t.tick, t.rub, dRub, t.usd, dUsd, t.sites, t.paid, t.progress);
        prev = &t;
    }
}

// Session log every 300 ticks: whether time, money and construction move -
// the sum of the construction progress (building +0x604, 0..1) of every
// building still under construction.
static void LogSessionProgress(const StateSummary& s)
{
    double progress = 0;
    int sites = 0, paid = 0, stopped = 0, free = 0;
    __try {
        free = *(int*)(g_exeBase + 0x9D4F10 + 0x578); // != 0: nothing is ever charged (session/money.h)
        BYTE** it = *(BYTE***)(g_exeBase + 0x9E6A18);
        BYTE** end = *(BYTE***)(g_exeBase + 0x9E6A20);
        for (; it && it < end; ++it) {
            float p = *it ? *(float*)(*it + 0x604) : 1.0f;
            if (p < 1.0f) {
                progress += p;
                sites++;
                if (*(*it + 0x608) || *(*it + 0x609)) paid++;
                if (*(*it + 0x879)) stopped++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Logf("republicmp  LOCKSTEP: tick %llu day %d/%d %.2f, rubles %.0f dollars %.0f%s, %d construction site(s) (%d paid with money, %d stopped), progress sum %.4f",
         (unsigned long long)g_stepCount, s.day, s.year, s.dayProgress, s.rub, s.usd, free ? " (free building)" : "", sites, paid,
         stopped, progress);
}

void Determinism_BeforePrepare()
{
    if (!g_active) return;
    g_simThread = GetCurrentThreadId();
    ++g_frameBeats;
    g_framePhase = 1;
    if (g_exeBase[GAME_TIMER_RVA + 13]) g_menuFrames = 30;
    else if (g_menuFrames) --g_menuFrames;
    TakeSpeedInput();
    g_frameStep = g_frameEarly = false;
    if (g_probeState == PROBE_RUNNING) {
        Commands_Collect();
        MakeCommands(g_runStep);
    }
    BYTE* timer = g_exeBase + GAME_TIMER_RVA;
    if (g_sessionRunning) SessionAdmission(timer);
    else ProbeAdmission(timer);
    // Speed 2 scales the timer by the game's own factor (105db9: exe+909d5c,
    // 0.345268), as the native loop would have done before this point.
    unsigned factor = g_tickSpeed == 4 ? PLAID_FACTOR_RVA : g_tickSpeed == 3 ? HYPER_FACTOR_RVA : FAST_FACTOR_RVA;
    *(float*)timer = DETERMINISM_FIXED_FPS * (g_tickSpeed >= 2 ? *(float*)(g_exeBase + factor) : 1.0f);
    *(float*)(timer + 4) = DETERMINISM_FIXED_FPS;
    *(int*)(timer + 8) = (int)DETERMINISM_FIXED_FPS;
    timer[12] = g_frameStep ? 0 : 1;
    memcpy(g_tickTimer, timer, sizeof(g_tickTimer));
    Commands_RecordMoney();
}

// Probe and plain layer: this PC's own clock admits the ticks.
static void ProbeAdmission(BYTE* timer)
{
    bool running = !g_probeBusy || g_probeState == PROBE_RUNNING;
    // Tool hold (by SoylentGreed): tools act only on held frames, so while
    // one is selected and actions become commands, no tick is admitted.
    bool toolHold = Commands_Capturing() && *(__int64*)(g_exeBase + 0x9D4F10 + 0xD428) != 0;
    bool paused = g_paused || timer[13] != 0 || toolHold;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = g_frameFrequency.QuadPart ? double(now.QuadPart - g_frameClock.QuadPart) / g_frameFrequency.QuadPart : 0;
    g_frameClock = now;
    // Pausing freezes admission, including any ticks still owed after a slow
    // frame. Dropping that debt would make low-FPS pauses lose simulation time.
    if (!g_probeBusy && !paused) g_tickDebt += elapsed * DETERMINISM_FIXED_FPS;
    if (running && !paused && !g_frameFault) {
        if (g_holdsLeft) --g_holdsLeft;
        else if (g_probeBusy || g_tickDebt >= 1.0) {
            g_frameStep = true;
            if (!g_probeBusy) g_tickDebt -= 1.0;
        }
    }
    int nextSpeed = g_probeBusy ? (g_probeFuzz ? 1 + int((g_runStep / 300) & 1) : g_probeSpeed) : g_requestedSpeed;
    nextSpeed = nextSpeed > 1 ? 2 : 1;
    if (g_frameStep && nextSpeed != g_tickSpeed && g_probeBusy) ++g_speedChanges[g_probeRun];
    if (g_frameStep) g_tickSpeed = nextSpeed;
}

// Lockstep session: what was received is handled, the commands due are made,
// then the host admits a tick by its clock - unless paused or the player is
// LOCKSTEP_WINDOW ticks behind - and grants it; the player runs a tick only
// once it was granted, at the host's speed. A selected tool does not stop the
// ticks: what it does stays on this PC (Determinism_ToolScopeBegin).
static void SessionAdmission(BYTE* timer)
{
    LockstepNet_Poll();
    Commands_Collect();
    MakeCommands(g_stepCount);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = g_frameFrequency.QuadPart ? double(now.QuadPart - g_frameClock.QuadPart) / g_frameFrequency.QuadPart : 0;
    g_frameClock = now;
    if (!g_sessionHost) {
        // The Esc menu sets the timer's menu byte (timer+13), which the tick
        // would run with; the player waits instead, and the host stops
        // LOCKSTEP_WINDOW ticks ahead.
        bool menu = timer[13] != 0;
        bool paused;
        int speed;
        if (LockstepNet_HostState(&paused, &speed)) {
            g_paused = paused;
            g_requestedSpeed = speed;
        }
        if (!menu && !g_frameFault && LockstepNet_TakeGrant(g_stepCount + 1, &speed)) {
            g_frameStep = true;
            g_tickSpeed = speed < 1 ? 1 : speed > 4 ? 4 : speed;
        }
        int boost = LocalBoost();
        if (boost != g_sentBoost) {
            LockstepNet_SendSpeed(10 + boost);
            g_sentBoost = boost;
        }
        return;
    }
    if (LockstepNet_TakePauseToggles() & 1) g_paused = !g_paused;
    int wanted = LockstepNet_TakeSpeedRequest();
    if (wanted >= 10) g_remoteBoost = wanted - 10 > 2 ? 2 : wanted - 10;
    else if (wanted > 0) g_requestedSpeed = wanted > 1 ? 2 : 1;
    static int s_sentPaused = -1, s_sentSpeed = -1;
    if ((int)g_paused != s_sentPaused || g_requestedSpeed != s_sentSpeed) {
        LockstepNet_SendState(g_paused, g_requestedSpeed);
        s_sentPaused = g_paused;
        s_sentSpeed = g_requestedSpeed;
    }
    bool hold = g_paused || timer[13] != 0 || !LockstepNet_IsConnected();
    if (!hold) g_tickDebt += elapsed * DETERMINISM_FIXED_FPS;
    if (g_tickDebt > 3.0) g_tickDebt = 3.0; // no burst after waiting for the player
    if (!hold && !g_frameFault && g_tickDebt >= 1.0 && LockstepNet_MayRun(g_stepCount + 1)) {
        g_frameStep = true;
        g_tickDebt -= 1.0;
        int boost = LocalBoost() > g_remoteBoost ? LocalBoost() : g_remoteBoost;
        g_tickSpeed = g_requestedSpeed > 1 ? 2 + boost : 1;
        LockstepNet_Grant(g_stepCount + 1, g_tickSpeed);
    }
}

static void HookedSimStep(__int64 world)
{
    if (!g_active) {
        o_simStep(world);
        for (int i = 0; i < g_stepObserverCount; i++) g_stepObservers[i]();
        return;
    }
    // Prepare (input) may have written a speed key after the frame was
    // admitted; take it as a request before the tick raises the speed.
    TakeSpeedInput();
    Commands_CheckMoney("the view preparation");
    if (!g_frameStep) {
        g_framePhase = 3;
        if (g_probeState == PROBE_RUNNING) ++g_holdCount[g_probeRun];
        if (!g_frameFault && g_probeState != PROBE_WAIT_LOAD && !g_sessionLoadPending && !HeldFrame_Draw(g_haveStep)) {
            g_frameFault = true;
            Logf("republicmp  PROBE: held presentation failed - no successful result can be reported");
        }
        g_framePhase = 4;
        return;
    }
    if (g_probeState == PROBE_RUNNING && g_runStep == 0) ProbeWatch_Step(g_probeRun, 0, false);
    g_framePhase = 2;
    WriteGameSpeed(GameSpeedOf(g_tickSpeed));
    // Early keeps the tool: its one read that changed the simulation (the
    // building pass, 13cc11) is patched in sim_patches. (by SoylentGreed)
    SimPatches_BeforeEarly(world);
    TwoStates_BeforeStep((BYTE*)world);
    g_inEarly = true;
    o_simStep(world);
    g_inEarly = false;
    TwoStates_AfterStep((BYTE*)world);
    SimPatches_AfterEarly();
    WriteGameSpeed(0);
    *(int*)(g_exeBase + 0x9D54BC) = kSpeedSentinel;
    memcpy(g_tickTimer, g_exeBase + GAME_TIMER_RVA, sizeof(g_tickTimer));
    g_frameEarly = g_haveStep = true;
    g_framePhase = 4;
}

void Determinism_BeforeLate()
{
    if (!g_active) return;
    g_framePhase = 5;
    TakeSpeedInput();
    g_inEarly = g_frameStep;
    memcpy(g_uiTimer, g_exeBase + GAME_TIMER_RVA, sizeof(g_uiTimer));
    memcpy(g_exeBase + GAME_TIMER_RVA, g_tickTimer, sizeof(g_tickTimer));
    // A tick's Late runs at the tick's speed, as the game's own running frame
    // does: at speed 0 with no time step 159070 treats the game as paused
    // (159234) and skips the building update, so construction never advanced
    // and nothing was charged for it.
    if (g_frameStep && g_frameEarly) {
        WriteGameSpeed(GameSpeedOf(g_tickSpeed));
        TwoStates_BeforeStep(g_exeBase + 0x9D4F10);
    }
}

void Determinism_AfterLate()
{
    if (!g_active) return;
    g_framePhase = 6;
    g_inEarly = false;
    if (g_frameStep && g_frameEarly) {
        // A speed the Late body changed itself stays as input for the next frame.
        int* speed = (int*)(g_exeBase + 0x9D54B4);
        if (*speed == GameSpeedOf(g_tickSpeed)) WriteGameSpeed(0);
        ++g_stepCount;
        TwoStates_AfterStep(g_exeBase + 0x9D4F10);
        for (int i = 0; i < g_stepObserverCount; i++) g_stepObservers[i]();
        if (g_sessionRunning) {
            if (g_moneyTrace) RecordMoneyTick();
            if (!g_sessionHost) LockstepNet_Ack(g_stepCount);
            if (g_stepCount % LOCKSTEP_HASH_TICKS == 0) {
                StateSummary s;
                if (StateHash_Compute(&s)) {
                    LockstepNet_StateAfter(g_stepCount, s);
                    if (g_stepCount % (LOCKSTEP_HASH_TICKS * 5) == 0) LogSessionProgress(s);
                }
            }
        }
        if (g_probeState == PROBE_RUNNING) {
            ++g_runStep;
            ProbeTrace_Step(g_probeRun, g_runStep);
            ProbeWatch_Step(g_probeRun, g_runStep, g_runStep >= (unsigned long long)g_probeSteps);
            if (g_runStep % g_probeInterval == 0) RecordSample();
            // Independent of game RNG: run 1 holds none, run 2 holds 0..3.
            if (g_probeFuzz && g_probeRun == 1) g_holdsLeft = (unsigned)(SplitMix(PROBE_SEED ^ g_runStep) & 3);
        }
    }
    Commands_AfterFrame(g_frameStep && g_frameEarly);
    memcpy(g_exeBase + GAME_TIMER_RVA, g_uiTimer, sizeof(g_uiTimer));
    g_frameStep = g_frameEarly = false;
    // Publish completion only after the final Late and all frame cleanup.
    if (g_probeState == PROBE_RUNNING && (g_frameFault || g_runStep >= (unsigned long long)g_probeSteps))
        InterlockedExchange(&g_probeState, PROBE_RUN_DONE);
}

// ---------------------------------------------------------------------------
// Zeroed heap
// ---------------------------------------------------------------------------
//
// The game leaves fields of new objects unset and reads them later: a
// citizen's activity entries (+0x118, 0x80 each) are filled by the load and by
// the code that starts an activity only in part, and 83a6d0 reads the whole
// entry (need types +0x18.., effects +0x38..) when it applies the activity to
// the needs. Unset words hold whatever the heap had there: zeros in a fresh
// process, an earlier game's data after a load in the same process, something
// else on every PC. Every malloc of the exe (operator new included) returns
// zeroed memory, as a fresh process would - always, not only in a session, so
// the objects a load creates are the same everywhere.

typedef void* (*MallocFn)(size_t size);
static MallocFn o_malloc;

static void* H_malloc(size_t size)
{
    void* p = o_malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool Determinism_Install()
{
    InitializeCriticalSection(&g_statusLock);
    int ok = 0;
    ok += PatchIat(g_exe, DLL_CRT_UTILITY, "rand", (void*)&H_rand, (void**)&o_rand, "det rand");
    ok += PatchIat(g_exe, DLL_CRT_UTILITY, "srand", (void*)&H_srand, (void**)&o_srand, "det srand");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Int@@YAII@Z", (void*)&H_randInt, (void**)&o_randInt, "det C3DRandom_Int");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Float@@YAMMM@Z", (void*)&H_randFloat, (void**)&o_randFloat, "det C3DRandom_Float");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Double@@YANNN@Z", (void*)&H_randDouble, (void**)&o_randDouble, "det C3DRandom_Double");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Start@@YAXXZ", (void*)&H_randStart, (void**)&o_randStart, "det C3DRandom_Start");
    ok += PatchIat(g_exe, DLL_C3D, "?Stop@C3D_TIMER@@QEAAXXZ", (void*)&H_timerStop, (void**)&o_timerStop, "det timer Stop");
    ok += PatchIat(g_exe, DLL_C3D, "?Start@C3D_TIMER@@QEAAXXZ", (void*)&H_timerStart, (void**)&o_timerStart, "det timer Start");
    ok += PatchIat(g_exe, DLL_C3D, "?GetKeyDown@C3D_INPUT@@QEAA_NH@Z", (void*)&H_getKeyDown, (void**)&o_getKeyDown, "det GetKeyDown");
    g_doneThreadHandle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    ok += PatchIat(g_exe, "api-ms-win-crt-runtime-l1-1-0.dll", "_beginthread", (void*)&H_beginThread, (void**)&o_beginThread, "det _beginthread");
    ok += PatchIat(g_exe, "KERNEL32.dll", "SetEvent", (void*)&H_setEvent, (void**)&o_setEvent, "det SetEvent");
    ok += PatchIat(g_exe, "api-ms-win-crt-heap-l1-1-0.dll", "malloc", (void*)&H_malloc, (void**)&o_malloc, "det malloc");
    bool step = InstallInlineHook(g_exeBase + SIM_STEP_RVA, (void*)&HookedSimStep, (void**)&o_simStep,
                                  kSimStepExpect, sizeof(kSimStepExpect), "republicmp sim step");
    bool load = InstallInlineHook(g_exeBase + LOAD_WORLD_RVA, (void*)&HookedLoadWorld, (void**)&o_loadWorld,
                                  kLoadWorldExpect, sizeof(kLoadWorldExpect), "republicmp load world");
    Logf("republicmp  DETERMINISM: %d/12 imports redirected, sim step hook %d, load hook %d", ok, step, load);
    bool presentation = HeldFrame_Verify();
    bool phases = SimPatches_Install();
    g_framesReady = ok == 12 && step && load && presentation && phases;
    Logf("republicmp  DETERMINISM: frame control ready %d (presentation %d, phases %d)", g_framesReady, presentation, phases);
    return g_framesReady;
}

void Determinism_SetActive(bool active, unsigned long long seed)
{
    if (active && !g_framesReady) {
        Logf("republicmp  DETERMINISM: activation refused - frame hooks are incomplete");
        return;
    }
    if (active) {
        RedirectMath();
        g_seed = seed;
        g_stepCount = 0;
        Determinism_Restart();
    }
    if (active) ResetFrames();
    else if (g_active) {
        WriteGameSpeed(0);
        *(int*)(g_exeBase + 0x9D54B8) = g_requestedSpeed;
        *(int*)(g_exeBase + 0x9D54BC) = 0;
        g_buttonsArmed = false;
    }
    g_active = active;
    Logf("republicmp  DETERMINISM: %s (seed %016llx)", active ? "on" : "off", seed);
}

bool Determinism_IsActive() { return g_active; }
void Determinism_Restart() { InterlockedIncrement(&g_epoch); }
unsigned long long Determinism_StepCount() { return g_stepCount; }

void Determinism_SessionSpeed(bool* paused, int* speed)
{
    *paused = g_paused;
    *speed = g_tickSpeed;
}
void Determinism_AddStepObserver(StepObserverFn fn)
{
    if (g_stepObserverCount < DETERMINISM_MAX_OBSERVERS) g_stepObservers[g_stepObserverCount++] = fn;
}

// ---------------------------------------------------------------------------
// Probe runner (worker thread)
// ---------------------------------------------------------------------------

static bool GetSaveDir(char* out, size_t outSize, const char* name)
{
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exePath, '\\');
    if (!slash) return false;
    *slash = 0;
    return _snprintf_s(out, outSize, _TRUNCATE, "%s\\media_soviet\\save\\%s", exePath, name) >= 0;
}

static void WriteRunLog(int run)
{
    char dir[MAX_PATH], path[MAX_PATH];
    if (!GetSaveDir(dir, sizeof(dir), PROBE_SAVE_NAME)) return;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\republicmp_probe_run%d.txt", dir, run + 1);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;
    fprintf(f, "# complete ticks; fuzz=%d held_frames=%llu speed_changes=%llu\n", g_probeFuzz,
            g_holdCount[run], g_speedChanges[run]);
    fprintf(f, "step day year progress buildings vehicles citizens storage cargo production clock bld veh cit "
               "draws drawhash population\n");
    for (int i = 0; i < g_sampleCount[run]; i++) {
        const ProbeSample& p = g_samples[run][i];
        fprintf(f, "%llu %d %d %.6f %d %d %d %.3f %.3f %.3f %016llx %016llx %016llx %016llx %llu %016llx %d\n", p.step,
                p.s.day, p.s.year, p.s.dayProgress, p.s.buildings, p.s.vehicles, p.s.citizens, p.s.storageTotal,
                p.s.cargoTotal, p.s.productionTotal, p.s.hashClock, p.s.hashBuildings, p.s.hashVehicles,
                p.s.hashCitizens, p.draws, p.drawHash, p.population);
    }
    fclose(f);
}

static void CompareRuns()
{
    int expected = g_probeSteps / g_probeInterval;
    if (g_sampleCount[0] != expected || g_sampleCount[1] != expected) {
        SetProbeStatus("INCOMPLETE: expected %d samples, got %d/%d", expected, g_sampleCount[0], g_sampleCount[1]);
        return;
    }
    if (g_probeFuzz && !ProbeTrace_Complete(g_probeSteps)) {
        SetProbeStatus("INCOMPLETE: fuzz requires a trace of all %d ticks", g_probeSteps);
        return;
    }
    if (g_probeRecord && g_commandsReplayed != g_commandsRecorded) {
        SetProbeStatus("INCOMPLETE: %zu of %zu recorded commands made again in run 2", g_commandsReplayed, g_commandsRecorded);
        return;
    }
    if (ProbeTrace_Different()) {
        SetProbeStatus("DIFFERENT from step %llu (per-tick trace; see PROBE TRACE)", ProbeTrace_FirstDifference());
        return;
    }
    int n = g_sampleCount[0] < g_sampleCount[1] ? g_sampleCount[0] : g_sampleCount[1];
    for (int i = 0; i < n; i++) {
        const StateSummary& a = g_samples[0][i].s;
        const StateSummary& b = g_samples[1][i].s;
        const ProbeSample& pa = g_samples[0][i];
        const ProbeSample& pb = g_samples[1][i];
        bool drawsSame = pa.draws == pb.draws && pa.drawHash == pb.drawHash;
        if (StateHash_All(a) == StateHash_All(b) && drawsSame && pa.population == pb.population) continue;
        char parts[128] = "";
        if (!drawsSame) strcat_s(parts, "random numbers ");
        if (pa.population != pb.population) strcat_s(parts, "population ");
        if (a.hashClock != b.hashClock) strcat_s(parts, "clock/money ");
        if (a.hashBuildings != b.hashBuildings) strcat_s(parts, "buildings ");
        if (a.hashVehicles != b.hashVehicles) strcat_s(parts, "vehicles ");
        if (a.hashCitizens != b.hashCitizens) strcat_s(parts, "citizens ");
        SetProbeStatus("DIFFERENT from step %llu (sample %d/%d): %s- draws %llu/%llu, vehicles %d/%d, citizens %d/%d, "
                       "storage %.1f/%.1f", g_samples[0][i].step, i + 1, n, parts, pa.draws, pb.draws, a.vehicles,
                       b.vehicles, a.citizens, b.citizens, a.storageTotal, b.storageTotal);
        return;
    }
    if (n == 0) SetProbeStatus("no samples recorded - was the world loaded?");
    else SetProbeStatus("IDENTICAL: both runs match at all %d samples (%llu steps)", n, g_samples[0][n - 1].step);
}

// Hang report: when the main thread has not started a frame for 10 s while
// a run simulates, the probe thread logs where the main thread and the early
// worker stand (module + offset per frame) and the frame phase. Once per hang.
static void LogThreadStack(const char* what, DWORD tid)
{
    if (!tid || tid == GetCurrentThreadId()) return;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!t) return;
    char line[1024];
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "republicmp  PROBE HANG: %s thread %lu:", what, tid);
    if (SuspendThread(t) != (DWORD)-1) {
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t, &c)) {
            for (int i = 0; i < 24 && c.Rip && len > 0 && len < (int)sizeof(line) - 64; i++) {
                HMODULE m = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)c.Rip, &m) && m) {
                    GetModuleFileNameA(m, name, sizeof(name));
                    const char* slash = strrchr(name, '\\');
                    if (slash) memmove(name, slash + 1, strlen(slash));
                }
                int n = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %s+%llx", name,
                                    (unsigned long long)(c.Rip - (DWORD64)m));
                if (n <= 0) break;
                len += n;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                if (!f) {
                    if (!c.Rsp) break;
                    c.Rip = *(DWORD64*)c.Rsp;
                    c.Rsp += 8;
                } else {
                    PVOID data = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
                }
            }
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    Logf("%s", line);
}

static void CheckHang(unsigned long long& lastBeat, ULONGLONG& beatAt, bool& reported)
{
    ULONGLONG now = GetTickCount64();
    if (g_frameBeats != lastBeat || g_probeState != PROBE_RUNNING) {
        lastBeat = g_frameBeats;
        beatAt = now;
        reported = false;
        return;
    }
    if (reported || now - beatAt < 10000) return;
    reported = true;
    BYTE* timer = g_exeBase + GAME_TIMER_RVA;
    Logf("republicmp  PROBE HANG: no frame for %llu ms - phase %ld, tick %llu, step %d, paused %d, holds %u, fault %d, timer %02x %02x",
         now - beatAt, g_framePhase, g_runStep, g_frameStep, g_paused, g_holdsLeft, g_frameFault, timer[12], timer[13]);
    LogThreadStack("main", g_simThread);
    BYTE* worker = *(BYTE**)(g_exeBase + 0x995290);
    if (worker) {
        Logf("republicmp  PROBE HANG: worker stop %d done %d", worker[0x28], worker[0x59]);
        LogThreadStack("worker", *(DWORD*)(worker + 0xC8));
    }
}

static bool WaitState(LONG want, DWORD timeoutMs)
{
    ULONGLONG start = GetTickCount64();
    unsigned long long lastBeat = g_frameBeats;
    ULONGLONG beatAt = start;
    bool reported = false;
    while (g_probeState != want) {
        if (g_probeState == PROBE_FAILED) return false;
        if (GetTickCount64() - start > timeoutMs) return false;
        CheckHang(lastBeat, beatAt, reported);
        Sleep(50);
    }
    return true;
}

static DWORD WINAPI ProbeThread(LPVOID)
{
    do {
        char dir[MAX_PATH];
        if (!GetSaveDir(dir, sizeof(dir), PROBE_SAVE_NAME)) { SetProbeStatus("couldn't build the save path"); break; }
        CreateDirectoryA(dir, nullptr);
        SetProbeStatus("saving the current game as '%s'...", PROBE_SAVE_NAME);
        TriggerSaveGame(PROBE_SAVE_NAME);
        if (!WaitForSaveToFinish(120000)) { SetProbeStatus("saving timed out"); break; }

        // world+0xC turns on a pass sorted by distance to the local camera
        // (FUN_1400f7930 from FUN_140139a70) - worth knowing if runs differ.
        Logf("republicmp  PROBE: world+0xC = %d", (int)*(BYTE*)(g_exeBase + 0x9D4F10 + 0xC));
        ResetDetail();
        ProbeTrace_SetDisk(g_probeFuzz);
        Determinism_SetActive(true, PROBE_SEED);
        bool failed = false;
        for (g_probeRun = 0; g_probeRun < 2 && !failed; g_probeRun++) {
            g_sampleCount[g_probeRun] = 0;
            SetProbeStatus("run %d/2: loading '%s'...", g_probeRun + 1, PROBE_SAVE_NAME);
            InterlockedExchange(&g_probeState, PROBE_WAIT_LOAD);
            TriggerLoadSave(PROBE_SAVE_NAME);
            if (!WaitState(PROBE_RUNNING, 300000)) { SetProbeStatus("run %d: load timed out or simulation patches refused (see log)", g_probeRun + 1); failed = true; break; }
            if (g_probeRecord && g_probeRun == 0)
                SetProbeStatus("run 1/2: simulating %d ticks - build now: roads, networks, buildings, demolition (ticks wait while a tool is open)",
                               g_probeSteps);
            else
                SetProbeStatus("run %d/2: simulating %d ticks - Space/F10 may pause; avoid other actions", g_probeRun + 1, g_probeSteps);
            // Generous: the steps run at the game's own frame rate.
            if (!WaitState(PROBE_RUN_DONE, (DWORD)g_probeSteps * 1000 + 60000)) {
                SetProbeStatus("run %d: timed out after %llu steps", g_probeRun + 1, g_runStep);
                failed = true;
                break;
            }
            if (g_frameFault) { SetProbeStatus("run %d: held-frame guard failed", g_probeRun + 1); failed = true; break; }
            Logf("republicmp  PROBE: run %d complete: %llu ticks, %llu held frames, %llu speed changes, fuzz %d",
                 g_probeRun + 1, g_runStep, g_holdCount[g_probeRun], g_speedChanges[g_probeRun], g_probeFuzz);
            WriteRunLog(g_probeRun);
        }
        Commands_ProbeEnd(&g_commandsRecorded, &g_commandsReplayed);
        if (g_probeRecord)
            Logf("republicmp  PROBE: commands recorded in run 1: %zu, made again in run 2: %zu", g_commandsRecorded, g_commandsReplayed);
        InterlockedExchange(&g_probeState, PROBE_IDLE);
        Determinism_SetActive(false, 0);
        if (!failed) {
            CompareRuns();
            ProbeTrace_Finish();
            ProbeWatch_Finish();
        }
        ResetDetail();
    } while (false);
    InterlockedExchange(&g_probeState, PROBE_IDLE);
    Commands_ProbeEnd(nullptr, nullptr);
    if (g_active) Determinism_SetActive(false, 0);
    g_probeBusy = false;
    return 0;
}

bool Probe_Start(int steps, int interval, bool fuzzFrames, bool recordCommands)
{
    if (g_probeBusy || SaveSync_IsBusy()) return false;
    if (!g_framesReady) { SetProbeStatus("frame hooks unavailable - see startup log"); return false; }
    if (steps < interval || interval < 1 || steps / interval > PROBE_MAX_SAMPLES) return false;
    StateSummary s;
    if (!StateHash_Compute(&s)) {
        SetProbeStatus("load a game first");
        return false;
    }
    g_probeFuzz = fuzzFrames;
    g_probeRecord = recordCommands;
    g_commandsRecorded = g_commandsReplayed = 0;
    memset(g_holdCount, 0, sizeof(g_holdCount));
    memset(g_speedChanges, 0, sizeof(g_speedChanges));
    g_probeBusy = true;
    g_probeSteps = steps;
    g_probeInterval = interval;
    g_probeSpeed = ReadGameSpeed() > 0 ? ReadGameSpeed() : 1;
    CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
    return true;
}

bool Probe_IsRunning() { return g_probeBusy; }

const char* Probe_Status()
{
    static char copy[320];
    EnterCriticalSection(&g_statusLock);
    strcpy_s(copy, g_probeStatus);
    LeaveCriticalSection(&g_statusLock);
    return copy;
}
