// SPDX-License-Identifier: GPL-3.0-only
// See sim_patches.h.
#include "sim_patches.h"
#include "../lockstep/commands.h"
#include "../world_sync/local_builds.h"
#include "../world_sync/networks/road_sync.h"
#include "../lockstep/purchase.h"
#include "../lockstep/route_tools.h"
#include "../lockstep/game_calls.h"
#include "../lockstep/vehicle_switches.h"
#include "../lockstep/building_switches.h"
#include "../lockstep/brush_tools.h"
#include "../lockstep/hover_tools.h"
#include "../lockstep/cargo_settings.h"
#include "../lockstep/source_slots.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../ui/native_menu.h"
#include "determinism.h"
#include "held_frame.h"
#include "../core/safe_mem.h"
#include <string.h>
#include <intrin.h>
#include <limits.h>
#include <vector>
#include <unordered_set>

#define WORLD_RVA 0x9D4F10

void SimPatches_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// Byte patches
// ---------------------------------------------------------------------------

struct DetPatch {
    unsigned int rva;
    unsigned char size;
    unsigned char original[26];
    unsigned char patch[26];
    const char* what;
};

#define NOP2 0x66, 0x90
#define NOP5 0x0f, 0x1f, 0x44, 0x00, 0x00
#define NOP6 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00
#define NOP8 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00
#define NOP21 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, \
              0x90, 0x90, 0x90, 0x90

static const DetPatch kPatches[] = {
    // cmp +13d8 / je skip / cmp +13d9 / jne skip -> jmp skip
    { 0x6a5475, 26, { 0x80, 0xbb, 0xd8, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xe6, 0x08, 0x00, 0x00,
                      0x80, 0xbb, 0xd9, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x85, 0xd9, 0x08, 0x00, 0x00 },
      { 0xe9, 0xee, 0x08, 0x00, 0x00, NOP21 }, "vehicle became visible (6a5410)" },
    { 0x6ab9d4, 26, { 0x80, 0xbe, 0xd8, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xbf, 0x03, 0x00, 0x00,
                      0x80, 0xbe, 0xd9, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x85, 0xb2, 0x03, 0x00, 0x00 },
      { 0xe9, 0xc7, 0x03, 0x00, 0x00, NOP21 }, "vehicle became visible (6a7520)" },
    // jne camera branch -> nop6
    { 0x6af131, 6, { 0x0f, 0x85, 0x0d, 0x01, 0x00, 0x00 }, { NOP6 }, "road vehicle placement (6af0c0)" },
    { 0x6ada40, 6, { 0x0f, 0x85, 0x16, 0x01, 0x00, 0x00 }, { NOP6 }, "rail vehicle placement (6ad960)" },
    // movzx eax,byte [rbx+13d8] -> xor eax,eax; nop5
    { 0x66e74c, 7, { 0x0f, 0xb6, 0x83, 0xd8, 0x13, 0x00, 0x00 }, { 0x31, 0xc0, NOP5 }, "vehicle checked flag" },
    // jne (piece on the camera) -> nop
    { 0x548b36, 2, { 0x75, 0x10 }, { NOP2 }, "road sign zones" },
    // The building pass's visible branch (13ce79..13cec7) stores the camera distance (+115c) and
    // +115a = 1: it stores the off-camera values (100000.0 and 0) instead and still pushes the
    // building on the visible list: mov dword [rax+115c],47c35000; mov byte [rax+115a],0; nop3
    { 0x13cea7, 20, { 0xf3, 0x0f, 0x11, 0x80, 0x5c, 0x11, 0x00, 0x00, 0x48, 0x8b, 0x44, 0x24, 0x40, 0xc6, 0x80, 0x5a, 0x11, 0x00, 0x00, 0x01 },
      { 0xc7, 0x80, 0x5c, 0x11, 0x00, 0x00, 0x00, 0x50, 0xc3, 0x47, 0xc6, 0x80, 0x5a, 0x11, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x00 },
      "building pass: off-camera flags on the visible branch" },
    // The construction update (159070, from the tick's Late) writes a site's stage piece count +63c
    // (the pieces drawn, and read by the workers' targeting 82e590 and the save) and re-targets its
    // workers (82e590, random draws) only when the site is on the camera (+115a) and near it (+115c).
    // The tick's Late runs with this PC's view (the hints over the buildings), so that ran on the PC
    // looking at the site only; left out everywhere, a site's +63c never changed and the sites of
    // the type being placed flickered. Every site now takes it on every PC, as one on the camera:
    // cmp byte [rdi+115a],0; je 15ba4c -> jmp 15b8d8 (past the distance test); nop8
    { 0x15b8b6, 13, { 0x80, 0xbf, 0x5a, 0x11, 0x00, 0x00, 0x00, 0x0f, 0x84, 0x89, 0x01, 0x00, 0x00 },
      { 0xe9, 0x1d, 0x00, 0x00, 0x00, NOP8 }, "construction update: every site as one on the camera" },
    // the camera's own update of a followed vehicle -> nop5; the worker's skip -> nop
    { 0x10e4f, 5, { 0xe8, 0xdc, 0xc6, 0x65, 0x00 }, { NOP5 }, "follow camera: train head" },
    { 0x10e87, 5, { 0xe8, 0xa4, 0xc6, 0x65, 0x00 }, { NOP5 }, "follow camera: wagon" },
    { 0x10ebd, 5, { 0xe8, 0x6e, 0xc6, 0x65, 0x00 }, { NOP5 }, "follow camera: vehicle" },
    { 0x10f22, 5, { 0xe8, 0x09, 0xc6, 0x65, 0x00 }, { NOP5 }, "ride camera: vehicle" },
    { 0x3fadbc, 2, { 0x75, 0x29 }, { NOP2 }, "follow camera: worker list" },
    // debug keys: je -> jmp (same target), jne -> nop
    { 0x13d81e, 2, { 0x74, 0x6a }, { 0xeb, 0x6a }, "building pass: Ctrl+N" },
    { 0x13d89e, 6, { 0x0f, 0x84, 0xe2, 0x00, 0x00, 0x00 }, { 0xe9, 0xe3, 0x00, 0x00, 0x00, 0x90 }, "building pass: Ctrl+M" },
    { 0x69e256, 6, { 0x0f, 0x85, 0x5c, 0x38, 0x00, 0x00 }, { NOP6 }, "vehicle routine: T" },
    { 0x31603a, 6, { 0x0f, 0x84, 0x06, 0x01, 0x00, 0x00 }, { 0xe9, 0x07, 0x01, 0x00, 0x00, 0x90 }, "Late body: H (networks)" },
    { 0x3161c3, 2, { 0x74, 0x48 }, { 0xeb, 0x48 }, "Late body: H (cursor)" },
    { 0x738e83, 6, { 0x0f, 0x84, 0x2b, 0x01, 0x00, 0x00 }, { 0xe9, 0x2c, 0x01, 0x00, 0x00, 0x90 }, "school window: Ctrl list cleanup" },
    { 0x14c6b5, 2, { 0x74, 0x6f }, { 0xeb, 0x6f }, "car building: Ctrl+I list cleanup" },
    { 0x1bd2ab, 2, { 0x74, 0x21 }, { 0xeb, 0x21 }, "depot: E+L+C slot word" },
    // Early (30cc94): with K held (exe+a54bb5, the input's key byte 0x25) a debug branch updated every
    // agent without the agents' prepare 832ae0 (which also sets world +127c8/+127cc for the agent
    // update 83a6d0), on that PC only: je -> jmp, the prepare always runs.
    { 0x30cc9b, 2, { 0x74, 0x41 }, { 0xeb, 0x41 }, "Early: K (the agents' debug branch)" },
    // 832ae0 put on the every-frame list (+126c0, the full step) the agents tied to a building on the
    // camera, the rest in three buckets (a third a frame, three times the step): with every building
    // off the camera every citizen stepped a third of the time, visibly in jumps. After its own tests
    // (+2c, +10) every agent now goes to the every-frame list, the same on every PC:
    // mov rdx,[rbx+38]; test rdx,rdx -> jmp 832cba; nop2
    { 0x832bd3, 7, { 0x48, 0x8b, 0x53, 0x38, 0x48, 0x85, 0xd2 }, { 0xe9, 0xe2, 0x00, 0x00, 0x00, NOP2 },
      "agents: every one on the every-frame list" },
    { 0xfa41, 2, { 0x74, 0x5a }, { 0xeb, 0x5a }, "render: debug calendar keys" },
    // je (no tool) -> jmp: a selected tool otherwise forces traversal from
    // building zero in the forward direction (13cc2f..13cc32), instead of
    // the shared random start/direction. Keep the shared order (by SoylentGreed).
    { 0x13cc19, 2, { 0x74, 0x1a }, { 0xeb, 0x1a }, "building pass: selected tool" },
};
#define PATCH_COUNT (sizeof(kPatches) / sizeof(kPatches[0]))

static volatile bool g_on = false;     // byte patches written, relays acting
static bool g_refused = false;         // a site didn't match - never tried again
static volatile DWORD g_simThread = 0; // the main thread (Early and Late)

static bool BytesAre(const DetPatch& p, const unsigned char* bytes)
{
    return SafeBytesMatch(g_exeBase + p.rva, bytes, p.size);
}

static void Write(BYTE* at, const unsigned char* bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

// ---------------------------------------------------------------------------
// Building visibility (GetNode at 13ce50)
// ---------------------------------------------------------------------------
//
// The Early building pass asks the camera's quadtree for a building's node
// and reads its visible byte (+0x25); the visible branch writes the
// building's visibility and distance (+115a/+115c), which decide how often
// its agents update, and pushes the building on the frame's visible list
// (world +11dc0). With this PC's camera those flags put the agents tied to a
// building on the every-frame list (832ae0), ran the building's update every
// tick instead of every third (16e3b0 -> 82e590: citizen targets, positions
// and random draws) and animated it - on that PC only. The branch now stores
// the off-camera values on every PC (the patch at 13cea7, by SoylentGreed)
// but still fills the visible list: the forklift paths' port search reads it
// and finds no port with it empty. The call `call [GetNode]` (6 bytes) is
// still relayed (nop + call) and answers the native node. Construction sites
// take the same branch through game/session_patches (13d963). This PC's view
// for its tools is the preview scope below; the doors the pass no longer
// moves are moved and drawn for this PC by held_frame.cpp.

#define GETNODE_SITE 0x13CE50
#define GETNODE_IMPORT 0x86C560 // the import slot the site calls through
typedef void* (*GetNodeFn)(void* tree, int index);
static const unsigned char kGetNodeOriginal[6] = { 0xff, 0x15, 0x0a, 0xf7, 0x72, 0x00 };

static void* DetGetNode(void* tree, int index)
{
    return (*(GetNodeFn*)(g_exeBase + GETNODE_IMPORT))(tree, index);
}

// ---------------------------------------------------------------------------
// Saplings (2f8e10 from the render thread, 2f8f20 from Late)
// ---------------------------------------------------------------------------
//
// A new planting entry (world+0x11740, 0x34 bytes: name, timer +20, position
// +28) starts growing when 2f8e10 gives its timer 0.001 - called by the
// render routine every fourth rendered frame, so on each PC at its own tick.
// While the layer is on, Late starts the new entries itself right before the
// growth 2f8f20 (every tick, every PC) and queues the forest add, which the
// render thread still makes where it used to (adding trees on the simulation
// thread raced the forest's renderer).

#define SAPLING_START_RVA 0x2F8E10
#define SAPLING_GROWTH_RVA 0x2F8F20
#define SAPLING_RENDER_CALL 0x9E4D
#define SAPLING_LATE_CALL 0x30D9B2
#define SAPLING_ENGINE_RVA 0x9941F0  // engine object
#define SAPLING_ENGINE_MAP 0xEE0     // +ee0 map, its trees +2f8..+300
#define SAPLING_ENGINE_FOREST 0xFC0  // +fc0 forest
#define SAPLING_ADD_IMPORT 0x86D270  // C3D_FOREST::EditorAddCursorTreesSoviet
#define SAPLING_LIST 0x11740
#define SAPLING_ENTRY 0x34
#define SAPLING_TIMER 0x20
#define SAPLING_NAME 0x20
#define SAPLING_POSITION 0x28
#define SAPLING_START_BITS 0x3a83126f // 0.001f
static const unsigned char kSaplingRenderOriginal[5] = { 0xe8, 0xbe, 0xef, 0x2e, 0x00 };
static const unsigned char kSaplingLateOriginal[5] = { 0xe8, 0x69, 0xb5, 0xfe, 0xff };

typedef void (*SaplingStartFn)(__int64, __int64, __int64, __int64);
typedef void (*SaplingGrowthFn)(__int64 world);
typedef bool (*ForestAddFn)(void* forest, const float* position, char* name);

struct QueuedSapling {
    char name[SAPLING_NAME + 1];
    float position[3];
};
static SRWLOCK g_saplingLock = SRWLOCK_INIT;
static std::vector<QueuedSapling> g_saplingQueue;

static bool TreesLoaded()
{
    __try {
        __int64 engine = *(__int64*)(g_exeBase + SAPLING_ENGINE_RVA);
        if (!engine) return false;
        __int64 map = *(__int64*)(engine + SAPLING_ENGINE_MAP);
        if (!map) return false;
        __int64 begin = *(__int64*)(map + 0x2f8), end = *(__int64*)(map + 0x300);
        return ((end - begin) >> 4) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 2f8e70..2f8ed5 without the forest add: entries whose timer is 0.0 start.
// Returns how many were started (at most cap are written to out).
#define SAPLING_BATCH 64
static int CollectSaplings(__int64 world, QueuedSapling* out, int cap)
{
    int n = 0;
    __try {
        BYTE* begin = *(BYTE**)(world + SAPLING_LIST);
        BYTE* end = *(BYTE**)(world + SAPLING_LIST + 8);
        if (!begin || end < begin || (end - begin) % SAPLING_ENTRY) return 0;
        for (BYTE* e = begin; e < end && n < cap; e += SAPLING_ENTRY) {
            if (*(float*)(e + SAPLING_TIMER) != 0.0f) continue;
            *(unsigned int*)(e + SAPLING_TIMER) = SAPLING_START_BITS;
            memset(&out[n], 0, sizeof(out[n]));
            memcpy(out[n].name, e, SAPLING_NAME);
            memcpy(out[n].position, e + SAPLING_POSITION, 12);
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static void StartSaplings(__int64 world)
{
    QueuedSapling found[SAPLING_BATCH];
    int n = CollectSaplings(world, found, SAPLING_BATCH); // more start on the next tick
    if (!n) return;
    AcquireSRWLockExclusive(&g_saplingLock);
    g_saplingQueue.insert(g_saplingQueue.end(), found, found + n);
    ReleaseSRWLockExclusive(&g_saplingLock);
}

static void AddToForest(const QueuedSapling* q, size_t n)
{
    __try {
        __int64 engine = *(__int64*)(g_exeBase + SAPLING_ENGINE_RVA);
        void* forest = engine ? *(void**)(engine + SAPLING_ENGINE_FOREST) : nullptr;
        ForestAddFn add = *(ForestAddFn*)(g_exeBase + SAPLING_ADD_IMPORT);
        if (!forest || !add) return;
        for (size_t i = 0; i < n; i++) {
            QueuedSapling copy = q[i]; // the engine takes a writable name
            add(forest, copy.position, copy.name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Late, at its growth call (rcx = world).
static void DetSaplingLate(__int64 world)
{
    if (g_on && Determinism_IsActive() && !Determinism_IsStepFrame()) return;
    if (g_on && GetCurrentThreadId() == g_simThread && TreesLoaded()) StartSaplings(world);
    ((SaplingGrowthFn)(g_exeBase + SAPLING_GROWTH_RVA))(world);
}

// Render routine, at its sapling start call (registers passed through).
static void DetSaplingRender(__int64 a, __int64 b, __int64 c, __int64 d)
{
    if (!g_on) {
        ((SaplingStartFn)(g_exeBase + SAPLING_START_RVA))(a, b, c, d);
        return;
    }
    std::vector<QueuedSapling> work;
    AcquireSRWLockExclusive(&g_saplingLock);
    work.swap(g_saplingQueue);
    ReleaseSRWLockExclusive(&g_saplingLock);
    if (!work.empty()) AddToForest(work.data(), work.size());
}

// ---------------------------------------------------------------------------
// Population count, autosave (Late body 30d1a0)
// ---------------------------------------------------------------------------
//
// The simulation scales by min(1, population / K) (citizen update 256080,
// 2571e0 from the building pass and Late), but the count world+0xc720 is
// written only by the interface, every ~3 game seconds of its own frames.
// While the layer is on the simulation thread writes it itself before Early
// and before Late, with the interface's rule over the citizens +126a8: a
// citizen counts when +71c and +748 are 0 and it is not marked +2d in a
// building whose type has +8e8 set (its assignment's +c8 [+20], else its
// home +20).
//
// A save writes every vehicle and ages it on the way (6c6770 -> 6c1bd0); the
// autosave fires on each PC's own real time (world float +0, accumulated in
// Late). While the layer is on, every Late starts with that timer at 0.

#define LATE_RVA 0x30D1A0
#define POPULATION_OFFSET 0xC720
#define CITIZENS_OFFSET 0x126A8
#define AUTOSAVE_TIMER_OFFSET 0x0
// push rbp / push r12..r15 / lea rbp,[rsp-2420h]
static const unsigned char kLateExpect[18] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xE0, 0xDB, 0xFF, 0xFF
};
typedef void (*LateFn)(__int64 world);
static LateFn o_late = nullptr;
static volatile int g_lastPopulation = -1;

static volatile bool g_populationLogged = false; // the first recount after Apply goes to the log
static volatile LONG g_populationFaults = 0;

// The interface's rule (31aee1..31b13c). Returns -1 when a read faulted.
static int CountPopulation(__int64 world, int* listed)
{
    *listed = 0;
    __try {
        __int64* begin = *(__int64**)(world + CITIZENS_OFFSET);
        __int64* end = *(__int64**)(world + CITIZENS_OFFSET + 8);
        int n = 0;
        if (begin && end >= begin) {
            *listed = (int)(end - begin);
            for (__int64* it = begin; it < end; ++it) {
                __int64 c = *it;
                if (!c || *(int*)(c + 0x71c) || *(int*)(c + 0x748)) continue;
                __int64 job = *(__int64*)(c + 0xc8);
                __int64 holder = job ? job : c;
                __int64 building = *(__int64*)(holder + 0x20);
                __int64 type = building ? *(__int64*)(building + 0x318) : 0;
                if (type && *(unsigned char*)(type + 0x8e8) == 1 && *(unsigned char*)(holder + 0x2d) == 1) continue;
                n++;
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void RecountPopulation(__int64 world)
{
    int listed = 0;
    int n = CountPopulation(world, &listed);
    int* count = (int*)(world + POPULATION_OFFSET);
    if (!g_populationLogged) {
        g_populationLogged = true;
        Logf("republicmp  SIM_PATCHES: population - the game's %d, recounted %d of %d citizens listed", *count, n, listed);
    }
    if (n < 0) {
        if (InterlockedIncrement(&g_populationFaults) <= 3) Logf("republicmp  SIM_PATCHES: population recount faulted");
        return;
    }
    *count = n;
    g_lastPopulation = n;
}

// Set while Early or Late runs.
static volatile bool g_inPhase = false;
static BYTE g_privateToolSearch[72] = {};
// ---------------------------------------------------------------------------
// This PC's own view for its tools
// ---------------------------------------------------------------------------
//
// Early fills world+11dc0 and building +115a/+115c natively in command
// sessions. Held frames skip Early, so Prepare and Late still need a fresh
// local list for newly placed buildings and camera movement while paused.
// This existing preview scope is limited to those phases; it does not replace
// the native building update or wrap the renderer.
#define VISIBLE_OFFSET 0x11DC0
#define ENGINE_RVA 0x9941F0       // engine object: +ff8 models, +ee0 map quadtree
#define FRAGMENT_IMPORT 0x86CFB8  // a model's fragment (called at 13ce39)
#define BUILDINGS_BEGIN_RVA 0x9E6A18
#define BUILDINGS_END_RVA 0x9E6A20
static BYTE g_privateVisible[24] = {}; // a vector header: begin, end, capacity
static bool g_visibleFresh = false;
// The visible branch also gives a building its camera distance (+115c, from
// C3D_CAMERA::GetZDistance at 13ce9c) and +115a = 1; the hints over the
// buildings (30e653: a transformer's connection points) need a near
// distance. The simulation keeps them off the camera (0 / 100000.0, its agent
// level of detail); this PC's view gets the real ones.
#define CAMERA_RVA 0x9E9C90
#define NODE_POSITION_IMPORT 0x86C7B8 // C3D_NODE::GetPosition
#define CAMERA_ZDISTANCE_IMPORT 0x86D008 // C3D_CAMERA::GetZDistance
struct VisibleDistance {
    BYTE* building;
    float distance;
};
struct VisibleKept {
    BYTE* building;
    BYTE visible;
    float distance;
};
static std::vector<VisibleDistance> g_visibleDistance;
static std::vector<VisibleKept> g_visibleKept;

static void FillPrivateVisible()
{
    typedef void (*PushFn)(void* vector, void* value);
    typedef BYTE* (*FragmentFn)(void* models, int model, int variant);
    PushFn push = (PushFn)(g_exeBase + 0xB0E90);
    FragmentFn fragment = *(FragmentFn*)(g_exeBase + FRAGMENT_IMPORT);
    GetNodeFn getNode = *(GetNodeFn*)(g_exeBase + GETNODE_IMPORT);
    typedef void* (*PositionFn)(void* node, float* out);
    typedef float (*ZDistanceFn)(void* camera, float* position);
    PositionFn position = *(PositionFn*)(g_exeBase + NODE_POSITION_IMPORT);
    ZDistanceFn zDistance = *(ZDistanceFn*)(g_exeBase + CAMERA_ZDISTANCE_IMPORT);
    memcpy(g_privateVisible + 8, g_privateVisible, 8); // end = begin
    g_visibleDistance.clear();
    __try {
        BYTE* engine = *(BYTE**)(g_exeBase + ENGINE_RVA);
        BYTE** it = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
        BYTE** end = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
        for (; engine && it && it < end; ++it) {
            BYTE* b = *it;
            if (!b) continue;
            BYTE* type = *(BYTE**)(b + 0x318);
            bool visible = false;
            if (type) {
                BYTE* f = fragment(*(void**)(engine + 0xFF8), *(int*)(type + 0x410), *(int*)(b + 0x5F4));
                BYTE* node = f ? (BYTE*)getNode(*(void**)(engine + 0xEE0), *(int*)(f + 0x3C)) : nullptr;
                visible = node && node[0x25];
                if (visible) {
                    float at[4] = {};
                    position(b + 0x320, at);
                    g_visibleDistance.push_back({ b, zDistance(g_exeBase + CAMERA_RVA, at) });
                }
            } else {
                BYTE* render = *(BYTE**)(b + 0x5D0);
                visible = render && render[0x178];
            }
            if (visible) push(g_privateVisible, &b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Preserve the native list and fields around the local preview phase.
// A nested tool scope reuses the outer phase's view.
static BYTE g_sharedVisible[24];
static bool g_visibleIn = false;

static bool VisibleIn()
{
    // While a world is being loaded the building list is torn down and
    // rebuilt: its buildings are not walked (the host crashed in C3DDLL64
    // right at the start of its session load).
    if (g_visibleIn || Determinism_WorldLoading()) {
        if (!g_visibleIn) g_visibleFresh = false;
        return false;
    }
    if (!g_visibleFresh) {
        FillPrivateVisible();
        g_visibleFresh = true;
    }
    BYTE* world = g_exeBase + WORLD_RVA;
    memcpy(g_sharedVisible, world + VISIBLE_OFFSET, sizeof(g_sharedVisible));
    memcpy(world + VISIBLE_OFFSET, g_privateVisible, sizeof(g_privateVisible));
    g_visibleKept.clear();
    for (const VisibleDistance& d : g_visibleDistance) {
        g_visibleKept.push_back({ d.building, d.building[0x115A], *(float*)(d.building + 0x115C) });
        d.building[0x115A] = 1;
        *(float*)(d.building + 0x115C) = d.distance;
    }
    g_visibleIn = true;
    return true;
}

static void VisibleOut(bool swapped)
{
    if (!swapped) return;
    BYTE* world = g_exeBase + WORLD_RVA;
    // Put the simulation's values back on every building that still exists
    // (one the phase removed is gone). The visible list itself is no guide:
    // the phase may empty it, and a building left with this PC's distance
    // made the next tick's Early animate it (16c900 from 13daf9: +0x1344) on
    // this PC only - the probe found it, and the sessions split on it.
    BYTE** listed = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
    BYTE** listedEnd = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
    std::unordered_set<BYTE*> still(listed, listedEnd);
    // What the simulation's own building pass writes with no building on the
    // camera (13ce61/13ce6d; a construction site keeps its distance, 13d963):
    // the same on every PC, whatever this phase or an earlier one left.
    for (const VisibleKept& k : g_visibleKept) {
        if (!still.count(k.building)) continue;
        bool typed = *(void**)(k.building + 0x318) != nullptr;
        k.building[0x115A] = 0;
        *(float*)(k.building + 0x115C) = typed ? 100000.0f : k.distance;
    }
    g_visibleKept.clear();
    memcpy(g_privateVisible, world + VISIBLE_OFFSET, sizeof(g_privateVisible));
    memcpy(world + VISIBLE_OFFSET, g_sharedVisible, sizeof(g_sharedVisible));
    g_visibleIn = false;
}

// Commands are made before Prepare (Determinism_BeforePrepare), with the
// visible list Early left: the buildings on this PC's camera. The building
// tool reads it when a placement is made (2b3106: a pick along this PC's
// mouse ray; 2c8942: the buildings around it that are on the camera), so the
// same placement did different things on each PC. While commands are made the
// list reads empty on every PC; the buildings that still exist go back on it
// after.
static std::vector<BYTE*> g_commandKept;
static bool g_commandFrame = false; // commands are being made (around Determinism_BeforePrepare)

static bool CommandViewIn()
{
    if (g_visibleIn || Determinism_WorldLoading()) return false;
    BYTE* world = g_exeBase + WORLD_RVA;
    BYTE** begin = *(BYTE***)(world + VISIBLE_OFFSET);
    BYTE** end = *(BYTE***)(world + VISIBLE_OFFSET + 8);
    g_commandKept.assign(begin, end);
    *(BYTE***)(world + VISIBLE_OFFSET + 8) = begin;
    return true;
}

static void CommandViewOut(bool emptied, BYTE** buildingsBegin, BYTE** buildingsEnd)
{
    if (!emptied) return;
    BYTE* world = g_exeBase + WORLD_RVA;
    BYTE** begin = *(BYTE***)(world + VISIBLE_OFFSET);
    BYTE** end = *(BYTE***)(world + VISIBLE_OFFSET + 8);
    BYTE** listed = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
    BYTE** listedEnd = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
    if (g_commandKept.empty() || Determinism_WorldLoading()) {
        g_commandKept.clear();
        return;
    }
    if (begin == end && listed == buildingsBegin && listedEnd == buildingsEnd) {
        // Nothing was made or removed: the list's memory is as it was.
        *(BYTE***)(world + VISIBLE_OFFSET + 8) = begin + g_commandKept.size();
    } else {
        typedef void (*PushFn)(void* vector, void* value);
        PushFn push = (PushFn)(g_exeBase + 0xB0E90);
        std::unordered_set<BYTE*> still(listed, listedEnd);
        std::unordered_set<BYTE*> there(begin, end);
        for (BYTE* b : g_commandKept)
            if (still.count(b) && !there.count(b)) push(world + VISIBLE_OFFSET, &b);
    }
    g_commandKept.clear();
}

struct PrivateView {
    BYTE search[72];
    bool visible;
};

static void PrivateViewIn(PrivateView& kept)
{
    BYTE* world = g_exeBase + WORLD_RVA;
    kept.visible = VisibleIn();
    memcpy(kept.search, world + 0x14478, sizeof(kept.search));
    memcpy(world + 0x14478, g_privateToolSearch, sizeof(kept.search));
}

static void PrivateViewOut(const PrivateView& kept)
{
    BYTE* world = g_exeBase + WORLD_RVA;
    memcpy(g_privateToolSearch, world + 0x14478, sizeof(kept.search));
    memcpy(world + 0x14478, kept.search, sizeof(kept.search));
    VisibleOut(kept.visible);
}

// A held frame's Late body runs this PC's tools: what they change outside the
// hooks is found around it and becomes commands (lockstep/commands).
static void PrivateLate(__int64 world)
{
    PrivateView kept;
    PrivateViewIn(kept);
    Commands_LateBegin();
    {
        Determinism_PrivateScope privateRng(true);
        g_inPhase = true;
        o_late(world);
        g_inPhase = false;
    }
    Commands_LateEnd();
    PrivateViewOut(kept);
}

static void HookedLate(__int64 world)
{
    bool active = g_on && Determinism_IsActive() && !Determinism_WorldLoading() && world == (__int64)(g_exeBase + WORLD_RVA);
    if (!active) {
        if (world == (__int64)(g_exeBase + WORLD_RVA) &&
            (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded()))) {
            SaveSync_EnforceWaitingLobbyHold();
            *(float*)(world + AUTOSAVE_TIMER_OFFSET) = 0.0f;
            o_late(world);
            SaveSync_EnforceWaitingLobbyHold();
            *(float*)(world + AUTOSAVE_TIMER_OFFSET) = 0.0f;
            return;
        }
        o_late(world);
        return;
    }
    Determinism_BeforeLate();
    g_simThread = GetCurrentThreadId();
    // Note (DET-20): H_MasterSave (0x42CC70 in two_states.cpp) snapshots and restores
    // vehicle aging fields (+0x17E8, +0x1A54, +0x1A7C) and runs under Determinism_ToolScopeBegin,
    // so saving during a Lockstep session does not mutate simulation state or RNG.
    bool step = Determinism_IsStepFrame();
    *(float*)(world + AUTOSAVE_TIMER_OFFSET) = 0.0f;
    if (step) RecountPopulation(world);
    // The tool stays selected on every frame. A tick's Late runs its handlers
    // through the tool stubs below; a held Late is this PC's own altogether.
    // The render thread has finished here (105eb8). (by SoylentGreed)
    if (step) {
        bool visible = VisibleIn();
        g_inPhase = true;
        o_late(world);
        g_inPhase = false;
        VisibleOut(visible);
    } else {
        PrivateLate(world);
    }
    *(float*)(world + AUTOSAVE_TIMER_OFFSET) = 0.0f;
    Determinism_AfterLate();
}

// ---------------------------------------------------------------------------
// Tool handlers in a tick's Late (312823..31496d)
// ---------------------------------------------------------------------------
//
// Late compares the selected tool's build-menu name and calls its handler
// every frame (one entry at 312823, one exit at 31496d). A handler drew from
// the shared random stream and searched in the shared vectors (run 17, by
// SoylentGreed), so every call in that stretch goes through a stub: on a
// tick's frame it runs in a tool scope (private draws, the private search
// vectors, its actions made commands and its other changes found as on a held
// frame); otherwise it calls straight through. The calls to the name compare
// 84f520 are left alone.
static const unsigned kToolCalls[][2] = {
    { 0x3128C1, 0x21C110 }, { 0x31294E, 0x214220 }, { 0x3129B8, 0x2BADE0 }, { 0x3129D8, 0x243570 },
    { 0x3129FC, 0x2F94C0 }, { 0x312A1E, 0x3A8A70 }, { 0x312A45, 0x3A8A70 }, { 0x312A6E, 0x3A8A70 },
    { 0x312A91, 0x3A8A70 }, { 0x312AB4, 0x3A8A70 }, { 0x312AD6, 0x32D240 }, { 0x312AF6, 0x3ABC90 },
    { 0x312B16, 0x3ADF80 }, { 0x312B36, 0x3CCD70 }, { 0x312B58, 0x234A50 }, { 0x312B7F, 0x234A50 },
    { 0x312BA8, 0x234A50 }, { 0x312BCB, 0x234A50 }, { 0x312BEE, 0x234A50 }, { 0x312C19, 0x235140 },
    { 0x312C44, 0x235140 }, { 0x312C70, 0x235140 }, { 0x312C9A, 0x235140 }, { 0x312CBF, 0x235140 },
    { 0x312CE4, 0x235140 }, { 0x312D0A, 0x235140 }, { 0x312D30, 0x235140 }, { 0x312D56, 0x235140 },
    { 0x312D7C, 0x235140 }, { 0x312D9E, 0x235370 }, { 0x312DC0, 0x235370 }, { 0x312DE2, 0x235580 },
    { 0x312E04, 0x235580 }, { 0x312E26, 0x238310 }, { 0x312E48, 0x238310 }, { 0x312E6D, 0x3CF6F0 },
    { 0x312E97, 0x3CF6F0 }, { 0x312ED8, 0x3CF6F0 }, { 0x312EFE, 0x3CF6F0 }, { 0x312F23, 0x3CF6F0 },
    { 0x312F4D, 0x3CF6F0 }, { 0x312F79, 0x3CF6F0 }, { 0x312F9F, 0x3CF6F0 }, { 0x312FC5, 0x3CF6F0 },
    { 0x312FEB, 0x3CF6F0 }, { 0x313011, 0x3CF6F0 }, { 0x313037, 0x3D00F0 }, { 0x31305C, 0x3D00F0 },
    { 0x313086, 0x3D00F0 }, { 0x3130AC, 0x3D00F0 }, { 0x3130D6, 0x3D00F0 }, { 0x313120, 0x3D00F0 },
    { 0x313146, 0x3D00F0 }, { 0x313171, 0x3D00F0 }, { 0x3131A1, 0x3D00F0 }, { 0x3131D2, 0x3D00F0 },
    { 0x3131FD, 0x3D00F0 }, { 0x31323E, 0x3D00F0 }, { 0x313264, 0x3D00F0 }, { 0x31328F, 0x3D00F0 },
    { 0x3132BB, 0x3D00F0 }, { 0x3132E7, 0x3D00F0 }, { 0x31330E, 0x3D00F0 }, { 0x313335, 0x3D00F0 },
    { 0x31335C, 0x3D00F0 }, { 0x313383, 0x3D00F0 }, { 0x3133AA, 0x3D00F0 }, { 0x3133D1, 0x3D00F0 },
    { 0x3133F8, 0x3D00F0 }, { 0x31341F, 0x3D00F0 }, { 0x313446, 0x3D00F0 }, { 0x31346D, 0x3D00F0 },
    { 0x313494, 0x3D00F0 }, { 0x3134BB, 0x3D00F0 }, { 0x3134E2, 0x3D00F0 }, { 0x313509, 0x3D00F0 },
    { 0x313530, 0x3D00F0 }, { 0x313557, 0x3D00F0 }, { 0x31357D, 0x3D00F0 }, { 0x3135A8, 0x3D00F0 },
    { 0x3135CE, 0x3D00F0 }, { 0x3135F4, 0x3D00F0 }, { 0x313614, 0x417A70 }, { 0x313634, 0x2B23B0 },
    { 0x313656, 0x3C9A00 }, { 0x313663, 0x3CE5F0 }, { 0x31366F, 0x3CE650 }, { 0x313679, 0x4182D0 },
    { 0x313683, 0x3CE1B0 }, { 0x3136A2, 0x4181D0 }, { 0x3136AC, 0x417D80 }, { 0x3136CF, 0x3AEC30 },
    { 0x3136FE, 0x3AEC30 }, { 0x313724, 0x3AEC30 }, { 0x31375C, 0x3B0E60 }, { 0x313782, 0x3B0E60 },
    { 0x3137A9, 0x3B0E60 }, { 0x3137CF, 0x3B0E60 }, { 0x313815, 0x3B12B0 }, { 0x313841, 0x3B12B0 },
    { 0x313912, 0x3B12B0 }, { 0x313953, 0x3B3820 }, { 0x313979, 0x3B3820 }, { 0x31399F, 0x3B3820 },
    { 0x3139C6, 0x3B3820 }, { 0x3139ED, 0x3B3820 }, { 0x313A14, 0x3B3820 }, { 0x313A3B, 0x3B3820 },
    { 0x313A62, 0x3B3820 }, { 0x313A89, 0x3B3820 }, { 0x313AB0, 0x3B3820 }, { 0x313AD6, 0x3B3820 },
    { 0x313B62, 0x3B4230 }, { 0x313B98, 0x3B8780 }, { 0x313BBE, 0x3B8780 }, { 0x313BE4, 0x3B8780 },
    { 0x313C0B, 0x3B8780 }, { 0x313C32, 0x3B8780 }, { 0x313C59, 0x3B8780 }, { 0x313C80, 0x3B8780 },
    { 0x313CA7, 0x3B8780 }, { 0x313CCD, 0x3B8780 }, { 0x313CF3, 0x3B8780 }, { 0x313D19, 0x3B8780 },
    { 0x313D3F, 0x3B8780 }, { 0x313D65, 0x3BAED0 }, { 0x313D8B, 0x3BAED0 }, { 0x313DB1, 0x3BAED0 },
    { 0x313DD8, 0x3BAED0 }, { 0x313DFE, 0x3BAED0 }, { 0x313E24, 0x3BAED0 }, { 0x313E4A, 0x3BAED0 },
    { 0x313E70, 0x3BAED0 }, { 0x3140A5, 0x3D4730 }, { 0x31415F, 0x418F90 }, { 0x3141DF, 0x2B4A50 },
    { 0x31422D, 0x2B2760 }, { 0x31424D, 0x2B18C0 }, { 0x31426F, 0x2B2DD0 }, { 0x314291, 0x2B2DD0 },
    { 0x3142B1, 0x2B3C80 }, { 0x3142D1, 0x2B4680 }, { 0x3142F1, 0x2B6AB0 }, { 0x314356, 0x1D48A0 },
    { 0x314376, 0x1DB1B0 }, { 0x314396, 0x1D51A0 }, { 0x3143B6, 0x1D6820 }, { 0x3143D6, 0x1D71C0 },
    { 0x3143F6, 0x1D9980 }, { 0x314416, 0x1DA580 }, { 0x314436, 0x1D6CB0 }, { 0x314456, 0x1D8410 },
    { 0x314476, 0x1D88A0 }, { 0x314496, 0x1DB9E0 }, { 0x3144B6, 0x1DBD40 }, { 0x3144D8, 0x3D6E70 },
    { 0x3144F8, 0x3D6E70 }, { 0x314518, 0x3D6E70 }, { 0x314522, 0x2B8CE0 }, { 0x31452C, 0x2B4C60 },
    { 0x31453C, 0x4184C0 }, { 0x314572, 0x3D68B0 }, { 0x314597, 0x3D68B0 }, { 0x3145BC, 0x3D68B0 },
    { 0x3145E2, 0x3D68B0 }, { 0x314607, 0x3D68B0 }, { 0x31462D, 0x3D68B0 }, { 0x314653, 0x3D68B0 },
    { 0x314679, 0x3D68B0 }, { 0x31469F, 0x3D68B0 }, { 0x3146C5, 0x3D68B0 }, { 0x3146E7, 0x3C85B0 },
    { 0x314709, 0x3C85B0 }, { 0x31472C, 0x3CBFA0 }, { 0x31474F, 0x3CBFA0 }, { 0x314774, 0x3C9130 },
    { 0x314799, 0x3C9130 }, { 0x3147BE, 0x3C9130 }, { 0x3147E3, 0x3C9130 }, { 0x314809, 0x3C9130 },
    { 0x314829, 0x2B6F30 }, { 0x314849, 0x2B78B0 }, { 0x314869, 0x2B8780 }, { 0x314889, 0x216780 },
    { 0x3148A9, 0x216970 }, { 0x3148CD, 0x5EEDF0 }, { 0x314913, 0x5F78D0 }, { 0x314930, 0x3CA560 },
    { 0x31494D, 0x3CB0D0 }, { 0x31495E, 0x214530 }, { 0x314968, 0x213870 },
};
#define TOOL_CALL_COUNT (sizeof(kToolCalls) / sizeof(kToolCalls[0]))
#define TOOL_STUB_SIZE 32

static bool CallTo(unsigned int site, BYTE* target, unsigned char out[5]);
static BYTE* g_toolStubs = nullptr;
static unsigned __int64 g_toolTarget = 0, g_toolReturn = 0;
static BYTE g_toolSaved[96] = {};
static bool g_toolActed = false;
static PrivateView g_toolKept;

static void ToolEnter()
{
    g_toolActed = g_on && Determinism_IsStepFrame();
    if (!g_toolActed) return;
    Determinism_ToolScopeBegin();
    PrivateViewIn(g_toolKept);
    Commands_RecordMoney(); // the tick's own charges are not the tool's
    Commands_LateBegin();
}

static void ToolLeave()
{
    if (!g_toolActed) return;
    g_toolActed = false;
    Commands_LateEnd();
    PrivateViewOut(g_toolKept);
    Determinism_ToolScopeEnd();
}

static BYTE* Emit(BYTE* p, const unsigned char* bytes, size_t n)
{
    memcpy(p, bytes, n);
    return p + n;
}

static BYTE* EmitImm(BYTE* p, unsigned char rex, unsigned char op, unsigned __int64 v)
{
    *p++ = rex;
    *p++ = op;
    memcpy(p, &v, 8);
    return p + 8;
}

// The common stub. Entered by jmp with the target in r11 and the caller's
// return address on the stack: the target and the return are kept, the
// argument registers saved around ToolEnter, the target called with the
// caller's own stack, and the result saved around ToolLeave.
static BYTE* EmitCommon(BYTE* p)
{
    static const unsigned char storeR11[] = { 0x4D, 0x89, 0x1A }; // mov [r10], r11
    static const unsigned char popR11[] = { 0x41, 0x5B };         // pop r11
    static const unsigned char saveArgs[] = {
        0x49, 0x89, 0x0A, 0x49, 0x89, 0x52, 0x08, 0x4D, 0x89, 0x42, 0x10, 0x4D, 0x89, 0x4A, 0x18, // rcx rdx r8 r9
        0xF3, 0x41, 0x0F, 0x7F, 0x42, 0x20, 0xF3, 0x41, 0x0F, 0x7F, 0x4A, 0x30,                   // xmm0 xmm1
        0xF3, 0x41, 0x0F, 0x7F, 0x52, 0x40, 0xF3, 0x41, 0x0F, 0x7F, 0x5A, 0x50 };                 // xmm2 xmm3
    static const unsigned char loadArgs[] = {
        0x49, 0x8B, 0x0A, 0x49, 0x8B, 0x52, 0x08, 0x4D, 0x8B, 0x42, 0x10, 0x4D, 0x8B, 0x4A, 0x18,
        0xF3, 0x41, 0x0F, 0x6F, 0x42, 0x20, 0xF3, 0x41, 0x0F, 0x6F, 0x4A, 0x30,
        0xF3, 0x41, 0x0F, 0x6F, 0x52, 0x40, 0xF3, 0x41, 0x0F, 0x6F, 0x5A, 0x50 };
    static const unsigned char subRsp[] = { 0x48, 0x83, 0xEC, 0x20 };
    static const unsigned char callRax[] = { 0xFF, 0xD0 };
    static const unsigned char addRsp[] = { 0x48, 0x83, 0xC4, 0x20 };
    static const unsigned char callMemR10[] = { 0x41, 0xFF, 0x12 }; // call [r10]
    static const unsigned char saveResult[] = { 0x49, 0x89, 0x02, 0xF3, 0x41, 0x0F, 0x7F, 0x42, 0x20 }; // rax, xmm0
    static const unsigned char loadResult[] = { 0x49, 0x8B, 0x02, 0xF3, 0x41, 0x0F, 0x6F, 0x42, 0x20 };
    static const unsigned char jmpMemR10[] = { 0x41, 0xFF, 0x22 }; // jmp [r10]
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)&g_toolTarget);
    p = Emit(p, storeR11, sizeof(storeR11));
    p = Emit(p, popR11, sizeof(popR11));
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)&g_toolReturn);
    p = Emit(p, storeR11, sizeof(storeR11));
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)g_toolSaved);
    p = Emit(p, saveArgs, sizeof(saveArgs));
    p = Emit(p, subRsp, sizeof(subRsp));
    p = EmitImm(p, 0x48, 0xB8, (unsigned __int64)&ToolEnter);
    p = Emit(p, callRax, sizeof(callRax));
    p = Emit(p, addRsp, sizeof(addRsp));
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)g_toolSaved);
    p = Emit(p, loadArgs, sizeof(loadArgs));
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)&g_toolTarget);
    p = Emit(p, callMemR10, sizeof(callMemR10));
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)g_toolSaved);
    p = Emit(p, saveResult, sizeof(saveResult));
    p = Emit(p, subRsp, sizeof(subRsp));
    p = EmitImm(p, 0x48, 0xB8, (unsigned __int64)&ToolLeave);
    p = Emit(p, callRax, sizeof(callRax));
    p = Emit(p, addRsp, sizeof(addRsp));
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)g_toolSaved);
    p = Emit(p, loadResult, sizeof(loadResult));
    p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)&g_toolReturn);
    return Emit(p, jmpMemR10, sizeof(jmpMemR10));
}

static bool InstallToolStubs()
{
    for (size_t i = 0; i < TOOL_CALL_COUNT; i++) {
        BYTE* call = g_exeBase + kToolCalls[i][0];
        int rel = 0;
        memcpy(&rel, call + 1, 4);
        if (*call != 0xE8 || kToolCalls[i][0] + 5 + rel != kToolCalls[i][1]) {
            Logf("republicmp  SIM_PATCHES: tool call at 0x%X differs - tool stubs not installed", kToolCalls[i][0]);
            return false;
        }
    }
    SIZE_T size = TOOL_STUB_SIZE * (TOOL_CALL_COUNT + 16);
    g_toolStubs = AllocNear(g_exeBase + 0x312823, size);
    if (!g_toolStubs) return false;
    DWORD old;
    VirtualProtect(g_toolStubs, size, PAGE_EXECUTE_READWRITE, &old);
    BYTE* common = g_toolStubs + TOOL_STUB_SIZE * TOOL_CALL_COUNT;
    EmitCommon(common);
    for (size_t i = 0; i < TOOL_CALL_COUNT; i++) {
        static const unsigned char jmpR10[] = { 0x41, 0xFF, 0xE2 };
        BYTE* p = g_toolStubs + TOOL_STUB_SIZE * i;
        // The copy line and depot tools go through their routes (lockstep/route_tools), which call them.
        unsigned __int64 target = kToolCalls[i][1] == 0x2B4C60   ? (unsigned __int64)&RouteTools_LineCopyTool
                                  : kToolCalls[i][1] == 0x2B2DD0 ? (unsigned __int64)&RouteTools_DepotTool
                                  : kToolCalls[i][1] == 0x3CF6F0 ? (unsigned __int64)&RouteTools_SecondRoadTool
                                  : kToolCalls[i][1] == 0x3CA560 ? (unsigned __int64)&RouteTools_GoThereTool
                                  : kToolCalls[i][1] == 0x3C85B0 ? (unsigned __int64)&RouteTools_BojkaTool
                                  : kToolCalls[i][1] == 0x2B23B0 ? (unsigned __int64)&RouteTools_JoinHubStopTool
                                  : kToolCalls[i][1] == 0x3C9A00 ? (unsigned __int64)&RouteTools_JoinHubTool
                                  : kToolCalls[i][1] == 0x2B8CE0 ? (unsigned __int64)&RouteTools_WorkerRelocateTool
                                  : kToolCalls[i][1] == 0x2B18C0 ? (unsigned __int64)&RouteTools_VehicleCopyTool
                                  : kToolCalls[i][1] == 0x2B3C80 ? (unsigned __int64)&RouteTools_VehicleReplaceTool
                                  : kToolCalls[i][1] == 0x2B4680 ? (unsigned __int64)&RouteTools_VehicleReplaceAutoTool
                                  : kToolCalls[i][1] == 0x2B6AB0 ? (unsigned __int64)&RouteTools_VehicleToVehicleTool
                                  : kToolCalls[i][1] == 0x1D88A0 ? (unsigned __int64)&RouteTools_VehicleSendConstrTool
                                  : kToolCalls[i][1] == 0x2F94C0 ? (unsigned __int64)&BrushTools_Tree
                                  : kToolCalls[i][1] == 0x3A8A70 ? (unsigned __int64)&BrushTools_Terrain
                                  : kToolCalls[i][1] == 0x3ADF80 ? (unsigned __int64)&BrushTools_DemolishTrees
                                  : kToolCalls[i][1] == 0x3CCD70 ? (unsigned __int64)&BrushTools_Topsoil
                                  : kToolCalls[i][1] == 0x417A70 ? (unsigned __int64)&HoverTools_Waypoint
                                  : kToolCalls[i][1] == 0x4181D0 ? (unsigned __int64)&HoverTools_SignErase
                                  : kToolCalls[i][1] == 0x417D80 ? (unsigned __int64)&HoverTools_Sign
                                  : kToolCalls[i][1] == 0x4184C0 ? (unsigned __int64)&HoverTools_RailPlace
                                  : kToolCalls[i][1] == 0x418F90 ? (unsigned __int64)&HoverTools_RailRemove
                                  : kToolCalls[i][1] == 0x4182D0 ? (unsigned __int64)&HoverTools_Junction
                                  : kToolCalls[i][1] == 0x1D51A0 ? (unsigned __int64)&SourceSlots_AddSourceTool
                                  : kToolCalls[i][1] == 0x214530 ? (unsigned __int64)&RoadSync_CtrlRectUpgradeTool
                                  : kToolCalls[i][1] == 0x213870 ? (unsigned __int64)&RoadSync_CtrlRectDemolishTool
                                  : kToolCalls[i][1] == 0x214220 ? (unsigned __int64)&RoadSync_ShiftCancelTool
                                                                 : (unsigned __int64)(g_exeBase + kToolCalls[i][1]);
        p = EmitImm(p, 0x49, 0xBB, target); // mov r11, target
        p = EmitImm(p, 0x49, 0xBA, (unsigned __int64)common);                        // mov r10, common
        Emit(p, jmpR10, sizeof(jmpR10));
    }
    VirtualProtect(g_toolStubs, size, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_toolStubs, size);
    for (size_t i = 0; i < TOOL_CALL_COUNT; i++) {
        unsigned char call[5];
        if (!CallTo(kToolCalls[i][0], g_toolStubs + TOOL_STUB_SIZE * i, call)) return false;
        Write(g_exeBase + kToolCalls[i][0], call, 5);
    }
    return true;
}

void SimPatches_BeforeEarly(__int64 world)
{
    g_simThread = GetCurrentThreadId();
    if (g_on) RecountPopulation(world);
    g_inPhase = true;
}

void SimPatches_AfterEarly()
{
    g_inPhase = false;
    if (g_on) HeldFrame_AfterEarly(); // the pass left every building off the camera
    g_visibleFresh = false; // the native pass may have removed buildings
}

int SimPatches_LastPopulation() { return g_lastPopulation; }

// ---------------------------------------------------------------------------
// Render thread before Early (frame loop 105df3)
// ---------------------------------------------------------------------------
//
// The main loop schedules the next render at the end of a frame (47d320:
// start set, finished reset) and waits for it only after the next frame's
// Early (105eb8). So the render thread draws frame N while frame N+1's
// Prepare and Early run; vehicles test the vehicle ahead through scene-node
// matrices (699420), which rendering touches too, and two runs from the
// same save moved a vehicle differently with every random number equal.
// While the layer is on, the frame waits for that render to finish before
// it takes the renderer lock (where vanilla only takes the lock), so the
// render and the simulation never overlap. Costs frame rate, not visuals.

#define FRAME_LOCK_SITE 0x105DF3   // call [EnterCriticalSection] on renderer+0x80
#define FRAME_LOCK_IMPORT 0x86D7F8
#define RENDERER_RVA 0x995268      // renderer object; +0x68 its "finished" event
#define RENDERER_FINISHED 0x68
#define RENDER_WAIT_MS 2000
static const unsigned char kFrameLockOriginal[6] = { 0xff, 0x15, 0xff, 0x79, 0x76, 0x00 };
typedef void (WINAPI* EnterCsFn)(LPCRITICAL_SECTION);
static volatile LONG g_renderWaitTimeouts = 0;

// The frame already waits at its very start (the timer Start hook, see
// SimPatches_WaitRender): the frame loop rescales the game timer there, and
// the interface on the render thread reads it (PowerTime) for its own
// countdowns. The wait here then returns at once; it stays for safety.
void SimPatches_WaitRender()
{
    if (!g_on) return;
    BYTE* renderer = *(BYTE**)(g_exeBase + RENDERER_RVA);
    HANDLE finished = renderer ? *(HANDLE*)(renderer + RENDERER_FINISHED) : nullptr;
    if (finished && WaitForSingleObject(finished, RENDER_WAIT_MS) == WAIT_TIMEOUT) {
        if (InterlockedIncrement(&g_renderWaitTimeouts) <= 3)
            Logf("republicmp  SIM_PATCHES: render took over %d ms - waiting before the next frame", RENDER_WAIT_MS);
        // A slow/background render must not overlap a deterministic tick.
        // The native wait at 105eb8 is unbounded too. (by SoylentGreed)
        WaitForSingleObject(finished, INFINITE);
    }
}

static void DetFrameLock(LPCRITICAL_SECTION cs)
{
    SimPatches_WaitRender();
    (*(EnterCsFn*)(g_exeBase + FRAME_LOCK_IMPORT))(cs);
}

// ---------------------------------------------------------------------------
// Interface searches (UI routine, render thread, 482b52)
// ---------------------------------------------------------------------------
//
// The game's search routine (424430) keeps its work and results in three
// vectors of the world (+14478 / +14490 / +144a8). The interface (317db0,
// called by the render thread at 482b52 under the renderer lock) searches
// there too, and the simulation's chooser (420350) draws a random number and
// picks only when the result pool is not empty - so what the interface last
// searched decided a vehicle's next target. While the layer is on, the
// interface searches in vectors of its own: the world's three headers are
// swapped for private ones around the call and put back. The renderer lock
// and the render wait before Early keep the simulation out meanwhile.
// (by SoylentGreed)

#define UI_ROUTINE_RVA 0x317DB0
#define UI_ROUTINE_CALL 0x482B52
#define SEARCH_WORKSPACE_OFFSET 0x14478
#define SEARCH_WORKSPACE_SIZE 72 // three vector headers {begin, end, capacity}
#define GAME_TIMER_RVA_VIEW 0x9D4EE0
static const unsigned char kUiCallOriginal[5] = { 0xe8, 0x59, 0x52, 0xe9, 0xff };
static BYTE g_privateSearch[SEARCH_WORKSPACE_SIZE] = {};
static volatile LONG g_uiEnclosed = 0;
static LARGE_INTEGER g_viewClock = {}, g_uiClock = {}, g_viewFrequency = {};

static float MeasureClockFps(LARGE_INTEGER* clockState)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_viewFrequency.QuadPart) QueryPerformanceFrequency(&g_viewFrequency);
    double dt = clockState->QuadPart ? double(now.QuadPart - clockState->QuadPart) / g_viewFrequency.QuadPart : 0.0;
    *clockState = now;
    if (dt < 0.001) dt = 0.001;  // at most 1000 FPS
    if (dt > 0.25) dt = 0.25;    // a stall (loading, alt-tab) does not throw the camera away
    return (float)(1.0 / dt);
}

void SimPatches_ResetSearches()
{
    // The private buffers are not part of the loaded world. Keep capacity,
    // but no pointer to a search result from the previous city. (by SoylentGreed)
    for (BYTE* workspace : { g_privateSearch, g_privateToolSearch })
        for (int i = 0; i < 3; i++) memcpy(workspace + i * 24 + 8, workspace + i * 24, 8);
}

static __int64 DetUiRoutine(void* world)
{
    typedef __int64 (*UiFn)(void*);
    UiFn original = (UiFn)(g_exeBase + UI_ROUTINE_RVA);
    if (!g_on || !Determinism_IsActive() || Determinism_WorldLoading() || (BYTE*)world != g_exeBase + WORLD_RVA) {
        if ((BYTE*)world == g_exeBase + WORLD_RVA &&
            (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded()))) {
            SaveSync_EnforceWaitingLobbyHold();
            TwoStates_UiBegin();
            __int64 r = original(world);
            TwoStates_UiEnd();
            SaveSync_EnforceWaitingLobbyHold();
            return r;
        }
        return original(world);
    }
    BYTE* shared = (BYTE*)world + SEARCH_WORKSPACE_OFFSET;
    BYTE saved[SEARCH_WORKSPACE_SIZE];
    memcpy(saved, shared, sizeof(saved));
    memcpy(shared, g_privateSearch, sizeof(saved));
    // The interface draws and picks the buildings' connection points (4a12e0: a building with
    // +115a set and +115c under its limit, the forklift paths' ports among them) and its windows
    // read the visible list: it gets this PC's view, as the tools do in Prepare and Late.
    bool view = !g_inPhase && VisibleIn();
    Determinism_PrivateScope privateRng;
    Determinism_UiSpeedBegin();
    VehicleSwitches_Before(g_inPhase);
    BuildingSwitches_Before(g_inPhase);
    CargoSettings_Before(g_inPhase);
    SourceSlots_Before(g_inPhase);
    BYTE* uiTimer = g_exeBase + GAME_TIMER_RVA_VIEW;
    float savedUnscaledFps = *(float*)(uiTimer + 4);
    *(float*)(uiTimer + 4) = MeasureClockFps(&g_uiClock);
    __int64 r = original(world);
    *(float*)(uiTimer + 4) = savedUnscaledFps;
    // The cargo settings first: a half they put back is the same again for the switches.
    CargoSettings_After(g_inPhase);
    SourceSlots_After(g_inPhase);
    BuildingSwitches_After(g_inPhase);
    VehicleSwitches_After(g_inPhase);
    Commands_GuardSettings();
    Determinism_UiSpeedEnd();
    VisibleOut(view);
    memcpy(g_privateSearch, shared, sizeof(saved));
    memcpy(shared, saved, sizeof(saved));
    InterlockedIncrement(&g_uiEnclosed);
    return r;
}

// ---------------------------------------------------------------------------
// Prepare at speed 0 (frame loop 105e56)
// ---------------------------------------------------------------------------
//
// Prepare (3d85a0) is this PC's view preparation; its camera object (14250)
// moves only while the game runs (142ef tests world+5a4) and is timed with
// PowerTime, so the camera drifted on its own and the paths that still read
// the camera followed. While the layer is on, Prepare runs with the speed
// word at 0; the frame controller raises it only for Early. (by SoylentGreed)

#define PREPARE_RVA 0x3D85A0
#define PREPARE_CALL 0x105E56
#define WORLD_SPEED_OFFSET 0x5A4
static const unsigned char kPrepareCallOriginal[5] = { 0xe8, 0x45, 0x27, 0x2d, 0x00 };

// The frame controller gives the game timer (exe+9d4ee0) the tick's fixed rate
// and pauses it on held frames, which is right for the simulation but not for
// this PC's camera: Prepare moves the camera with the timer, so at 150 FPS the
// keys moved it as if every frame were a whole tick (it flew off), or it stood
// on held frames and jumped on tick frames. For Prepare the timer carries this
// PC's real frame rate and runs; the tick's values are put back after it.
// Prepare runs at speed 0, so nothing of the simulation advances with it.
static float RealFrameRate()
{
    return MeasureClockFps(&g_viewClock);
}

static void DetPrepare(__int64 world)
{
    typedef void (*PrepareFn)(__int64);
    PrepareFn original = (PrepareFn)(g_exeBase + PREPARE_RVA);
    if (!g_on || !Determinism_IsActive() || Determinism_WorldLoading() || world != (__int64)(g_exeBase + WORLD_RVA)) {
        if (world == (__int64)(g_exeBase + WORLD_RVA) &&
            (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded()))) {
            SaveSync_EnforceWaitingLobbyHold();
            original(world);
            SaveSync_EnforceWaitingLobbyHold();
            return;
        }
        original(world);
        return;
    }
    BYTE** buildingsBegin = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
    BYTE** buildingsEnd = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
    bool emptied = CommandViewIn();
    g_commandFrame = true;
    Determinism_BeforePrepare();
    g_commandFrame = false;
    CommandViewOut(emptied, buildingsBegin, buildingsEnd);
    // Keep the native tool for previews. Held frames have no Early pass,
    // so refresh the local preview list before Prepare.
    g_visibleFresh = false;
    PrivateView kept;
    PrivateViewIn(kept);
    bool scope = Determinism_IsStepFrame();
    if (scope) Determinism_ToolScopeBegin();
    BYTE* timer = g_exeBase + GAME_TIMER_RVA_VIEW;
    BYTE tickTimer[13];
    memcpy(tickTimer, timer, sizeof(tickTimer));
    float fps = RealFrameRate();
    *(float*)timer = fps;
    *(float*)(timer + 4) = fps;
    *(int*)(timer + 8) = (int)fps;
    timer[12] = 0;
    {
        Determinism_PrivateScope privateRng;
        original(world);
    }
    memcpy(timer, tickTimer, sizeof(tickTimer));
    if (scope) Determinism_ToolScopeEnd();
    PrivateViewOut(kept);
}

// Late runs even at speed 0: 547ef0 clamps road speeds without time passing;
// 333bd0 can roll weather with a zero timer, and 5cfea0 changes road wetness.
// Only the admitted tick may run these calls; loading paths stay native.
// The existing session patches already own citizen initialization and the
// building FPS branches; those are not patched twice. (by SoylentGreed)
static void DetPieceSpeed(void* network)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*))(g_exeBase + 0x547EF0))(network);
}
static void DetWeather(void* world, unsigned long long init)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*, unsigned long long))(g_exeBase + 0x333BD0))(world, init);
}
static void DetRoadWeather(void* world, unsigned long long init)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*, unsigned long long))(g_exeBase + 0x5CFEA0))(world, init);
}
// Two more queues the Late body works off by frame, not by tick (30d6bc,
// 30d6c4): 12fd80 drains the list at world+11fa0 with a budget of 1000 per
// call, and 12ec40 counts world+11f80 up every frame and, every 20th, checks
// the buildings queued in world+11f88 (12eab0 queues them with +ca0 = 1 when
// a network or a building changes near them; 12e6d0 checks one, 12eba0
// clears the flag). Both are empty most of the time, which is why held
// frames looked harmless; after a construction command the PC with more
// held frames worked the queues off first. On the save path (42cd57) 12fd80
// stays native.
static void DetBuildingLinks(void* world, unsigned long long all)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*, unsigned long long))(g_exeBase + 0x12FD80))(world, all);
}
static void DetConnectionChecks(void* world)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*))(g_exeBase + 0x12EC40))(world);
}
// The ground-paint queue world+130d0 (filled by 40eaa0 when roads and
// buildings are built) is drained by 40ece0 at 3155f4 on every frame too: a
// held frame painted the ground of the newest construction on this PC first,
// and with commands captured those EditMask calls looked like a tool's.
static void DetGroundPaint(void* world)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*))(g_exeBase + 0x40ECE0))(world);
}
// The building windows (6f8cc0, 7fc880) show a construction cost every frame
// through 1a48c0, whose 1a0570 starts with building +0x600 = +0x604 (1a0612).
// The simulation reads +0x600 (15e18e, 168900), so an open window changed
// what that building's construction charged on its PC only. The word is put
// back when the call made it the copy. (by SoylentGreed)
static float DetDisplayCost(void* world, BYTE* building, unsigned long long flag)
{
    typedef float (*CostFn)(void*, BYTE*, unsigned long long);
    CostFn original = (CostFn)(g_exeBase + 0x1A48C0);
    if (!Determinism_IsActive() || !building) return original(world, building, flag);
    unsigned kept = 0;
    __try {
        kept = *(unsigned*)(building + 0x600);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return original(world, building, flag);
    }
    float r = original(world, building, flag);
    __try {
        unsigned now = *(unsigned*)(building + 0x600);
        if (now != kept && now == *(unsigned*)(building + 0x604)) *(unsigned*)(building + 0x600) = kept;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}
// The building tool's placing press: 2bade0 reads the left press at 2cc37c
// and places when the spot is valid. In lockstep that press becomes a
// placement command instead (world_sync/local_builds), and the game reads it
// as not pressed; every other read of the mouse in the tool stays real.
#define PLACE_PRESS_SITE 0x2CC37C
#define PLACE_PRESS_IMPORT 0x86C8B8 // C3D_INPUT::GetMouseLeftPress
static const unsigned char kPlacePressOriginal[6] = { 0xff, 0x15, 0x36, 0x05, 0x5a, 0x00 };
typedef bool (*PressFn)(void* input);

static bool DetPlacePress(void* input)
{
    bool pressed = (*(PressFn*)(g_exeBase + PLACE_PRESS_IMPORT))(input);
    if (pressed && Determinism_IsActive() && LocalBuilds_PlacementPress()) return false;
    return pressed;
}

// The purchase window's Buy press (80cc60 reads it at 810943, from the
// interface): in lockstep it becomes a purchase command (lockstep/purchase),
// read from the caller's registers one frame up. (by SoylentGreed)
#define BUY_PRESS_SITE 0x810943
static const unsigned char kBuyPressOriginal[6] = { 0xff, 0x15, 0x6f, 0xbf, 0x05, 0x00 };

static bool DetBuyPress(void* input)
{
    alignas(16) CONTEXT c = {};
    RtlCaptureContext(&c);
    bool pressed = (*(PressFn*)(g_exeBase + PLACE_PRESS_IMPORT))(input);
    if (pressed && (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded()))) {
        SaveSync_NotifyWaitingLobbyActionBlocked();
        return false;
    }
    if (!pressed || !Determinism_IsActive()) return pressed;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
    if (!f) return pressed;
    void* data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
    return Purchase_OnPress(pressed, &c);
}

// The route tools (lockstep/route_tools): the stop pick 2b5120 of
// vehicle_addstop (2b2800) and line_addstop (2b4b03), and the copy line tool's
// mark 40cde0 (2b505c). (by SoylentGreed)
typedef unsigned __int64 U64;
static U64 DetStopPickVehicle(U64 w, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64* building, U64* node)
{
    return RouteTools_StopPick(0, w, a2, a3, a4, a5, a6, building, node);
}

static U64 DetStopPickLine(U64 w, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64* building, U64* node)
{
    return RouteTools_StopPick(1, w, a2, a3, a4, a5, a6, building, node);
}

static U64 DetLineMark(U64 w, U64 vehicle, U64 a3, U64 refused)
{
    return RouteTools_LineMark(w, vehicle, a3, refused);
}

// The depot tool's mark 40bd60 (2b3790): the tool's list is read from its caller's frame.
static U64 DetDepotMark(U64 w, U64 depot, U64 a3, U64 a4, U64 a5)
{
    U64* ret = (U64*)_AddressOfReturnAddress();
    return RouteTools_DepotMark(w, depot, a3, a4, a5, (U64)(ret + 1), *ret);
}
// The network picks take the node or edge nearest the line through the cursor
// along this PC's mouse ray (world +f60 minus the camera's position, then
// C3DVec3Normalize): 3c4850 (the hovered node or edge) and 3d4b40 (the road
// under a point), which the building tool calls to find what a building's
// access road joins, and 3ae610 and 3d05d0, which the planner's update 4fcaf0
// calls. With the cameras at other angles the same placement laid its access
// road on one PC only (a bus stop: one more construction site there, and the
// money split). While commands are made the ray points straight down on every
// PC; this PC's own hover and previews keep its mouse ray.
#define PICK_RAY_SITES 4
#define NORMALIZE_IMPORT 0x86C108 // C3DVec3Normalize
static const unsigned kPickRaySites[PICK_RAY_SITES] = { 0x3C49A1, 0x3D4C4F, 0x3AE6E4, 0x3D0770 };
static const unsigned char kPickRayOriginal[PICK_RAY_SITES][6] = { { 0xff, 0x15, 0x61, 0x77, 0x4a, 0x00 },
                                                                   { 0xff, 0x15, 0xb3, 0x74, 0x49, 0x00 },
                                                                   { 0xff, 0x15, 0x1e, 0xda, 0x4b, 0x00 },
                                                                   { 0xff, 0x15, 0x92, 0xb9, 0x49, 0x00 } };
static bool PickRaySitesAre()
{
    for (int i = 0; i < PICK_RAY_SITES; i++)
        if (memcmp(g_exeBase + kPickRaySites[i], kPickRayOriginal[i], 6) != 0) return false;
    return true;
}
typedef float* (*NormalizeFn)(float* out, const float* in);

static float* DetPickRay(float* out, const float* in)
{
    if (g_on && g_commandFrame) {
        out[0] = 0.0f;
        out[1] = -1.0f;
        out[2] = 0.0f;
        return out;
    }
    return (*(NormalizeFn*)(g_exeBase + NORMALIZE_IMPORT))(out, in);
}

// Cheat & dev key combinations (309d80, called from Late at 30d959: C+H+E,
// D+E+V, E+L+T, LCtrl+Z+Q) toggle world+0x9 / world+0xA and show the "Cheat mode
// enabled" popup on one PC only. Block the routine completely during any
// multiplayer session or waiting lobby and sanitize any cheat flags loaded from a save.
static void DetCheatKeys(void* world)
{
    bool blockCheats = Determinism_IsActive() || Commands_Capturing() ||
                       SaveSync_IsWaitingLobbyActive() ||
                       (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded());
    if (blockCheats) {
        if (world) {
            __try {
                Commands_SanitizeCheatState((BYTE*)world);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return;
    }
    ((void (*)(void*))(g_exeBase + CHEAT_KEYS_STEP_RVA))(world);
}

#define STEP_CALL_COUNT 29
static const unsigned kStepCalls[STEP_CALL_COUNT] = { 0x30E3AC, 0x30E3B8, 0x30E3C4, 0x30D9DD, 0x30D9E7, 0x30D6BC, 0x30D6C4, 0x3155F4,
                                                      0x6F91DF, 0x6F92C8, 0x6F940E, 0x6FA603, 0x6FA657, 0x7FCAE6, 0x7FCBDC, 0x7FCD00,
                                                      0x2B2800, 0x2B4B03, 0x2B505C, 0x2B3790,
                                                      0x2B6ECA, 0x2B9263, 0x2B2152, 0x2B445F, 0x2B45CB, 0x2B49DF, 0x1D8EA6, 0x1D9188,
                                                      CHEAT_KEYS_CALL_RVA };
static const unsigned kStepTargets[STEP_CALL_COUNT] = { 0x547EF0, 0x547EF0, 0x547EF0, 0x333BD0, 0x5CFEA0, 0x12FD80, 0x12EC40, 0x40ECE0,
                                                        0x1A48C0, 0x1A48C0, 0x1A48C0, 0x1A48C0, 0x1A48C0, 0x1A48C0, 0x1A48C0, 0x1A48C0,
                                                        0x2B5120, 0x2B5120, 0x40CDE0, 0x40BD60,
                                                        0x40CDE0, 0x40BD60, 0x40BD60, 0x40BD60, 0x40BD60, 0x40BD60, 0x40BD60, 0x40BD60,
                                                        CHEAT_KEYS_STEP_RVA };
static const int kStepSlots[STEP_CALL_COUNT] = { 6, 6, 6, 7, 8, 9, 10, 11, 12, 12, 12, 12, 12, 12, 12, 12, 15, 16, 17, 18,
                                                 17, 18, 18, 18, 18, 18, 18, 18, 20 }; // relay slot of each site
static bool VerifyStepCalls()
{
    for (int i = 0; i < STEP_CALL_COUNT; i++) {
        BYTE* call = g_exeBase + kStepCalls[i];
        int rel = 0;
        memcpy(&rel, call + 1, 4);
        if (*call != 0xE8 || kStepCalls[i] + 5 + rel != kStepTargets[i]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Install, apply, restore
// ---------------------------------------------------------------------------

// A relay page near the exe: one `jmp [rip+0]` per target, 16 bytes apart.
static BYTE* g_relay = nullptr;
#define RELAY_SLOTS 21

static bool CallTo(unsigned int site, BYTE* target, unsigned char out[5])
{
    long long rel = (long long)target - (long long)(g_exeBase + site + 5);
    if (rel < INT_MIN || rel > INT_MAX) return false;
    int rel32 = (int)rel;
    out[0] = 0xe8;
    memcpy(out + 1, &rel32, 4);
    return true;
}

bool SimPatches_Install()
{
    bool late = InstallInlineHook(g_exeBase + LATE_RVA, (void*)&HookedLate, (void**)&o_late,
                                  kLateExpect, sizeof(kLateExpect), "republicmp late body");

    bool sitesOk = memcmp(g_exeBase + GETNODE_SITE, kGetNodeOriginal, 6) == 0 &&
                   memcmp(g_exeBase + SAPLING_RENDER_CALL, kSaplingRenderOriginal, 5) == 0 &&
                   memcmp(g_exeBase + SAPLING_LATE_CALL, kSaplingLateOriginal, 5) == 0 &&
                   memcmp(g_exeBase + FRAME_LOCK_SITE, kFrameLockOriginal, 6) == 0 &&
                   memcmp(g_exeBase + UI_ROUTINE_CALL, kUiCallOriginal, 5) == 0 &&
                   memcmp(g_exeBase + PREPARE_CALL, kPrepareCallOriginal, 5) == 0 &&
                   memcmp(g_exeBase + PLACE_PRESS_SITE, kPlacePressOriginal, 6) == 0 &&
                   memcmp(g_exeBase + BUY_PRESS_SITE, kBuyPressOriginal, 6) == 0 &&
                   PickRaySitesAre() && VerifyStepCalls();
    bool relays = false;
    if (sitesOk) g_relay = AllocNear(g_exeBase, 4096);
    if (g_relay) {
        DWORD old;
        VirtualProtect(g_relay, RELAY_SLOTS * 16, PAGE_EXECUTE_READWRITE, &old);
        void* targets[RELAY_SLOTS] = { (void*)&DetGetNode, (void*)&DetSaplingRender, (void*)&DetSaplingLate, (void*)&DetFrameLock,
                              (void*)&DetUiRoutine, (void*)&DetPrepare, (void*)&DetPieceSpeed, (void*)&DetWeather, (void*)&DetRoadWeather,
                              (void*)&DetBuildingLinks, (void*)&DetConnectionChecks, (void*)&DetGroundPaint, (void*)&DetDisplayCost,
                              (void*)&DetPlacePress, (void*)&DetBuyPress, (void*)&DetStopPickVehicle, (void*)&DetStopPickLine,
                              (void*)&DetLineMark, (void*)&DetDepotMark, (void*)&DetPickRay, (void*)&DetCheatKeys };
        for (int i = 0; i < RELAY_SLOTS; i++) {
            BYTE* r = g_relay + i * 16;
            r[0] = 0xff; r[1] = 0x25; r[2] = r[3] = r[4] = r[5] = 0;
            memcpy(r + 6, &targets[i], 8);
        }
        VirtualProtect(g_relay, RELAY_SLOTS * 16, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), g_relay, RELAY_SLOTS * 16);
        unsigned char getNode[6], render[5], lateCall[5], frameLock[6], uiCall[5], prepareCall[5], placePress[6], buyPress[6],
            pickRay[PICK_RAY_SITES][6];
        getNode[0] = 0x90;
        bool pickRayOk = true;
        for (int i = 0; i < PICK_RAY_SITES; i++) {
            pickRay[i][0] = 0x90;
            pickRayOk = pickRayOk && CallTo(kPickRaySites[i] + 1, g_relay + 19 * 16, pickRay[i] + 1);
        }
        frameLock[0] = 0x90;
        placePress[0] = 0x90;
        buyPress[0] = 0x90;
        if (CallTo(GETNODE_SITE + 1, g_relay, getNode + 1) && CallTo(SAPLING_RENDER_CALL, g_relay + 16, render) &&
            CallTo(SAPLING_LATE_CALL, g_relay + 32, lateCall) && CallTo(FRAME_LOCK_SITE + 1, g_relay + 48, frameLock + 1) &&
            CallTo(UI_ROUTINE_CALL, g_relay + 64, uiCall) && CallTo(PREPARE_CALL, g_relay + 80, prepareCall) &&
            CallTo(PLACE_PRESS_SITE + 1, g_relay + 13 * 16, placePress + 1) &&
            CallTo(BUY_PRESS_SITE + 1, g_relay + 14 * 16, buyPress + 1) && pickRayOk) {
            Write(g_exeBase + GETNODE_SITE, getNode, 6);
            Write(g_exeBase + SAPLING_RENDER_CALL, render, 5);
            Write(g_exeBase + SAPLING_LATE_CALL, lateCall, 5);
            Write(g_exeBase + FRAME_LOCK_SITE, frameLock, 6);
            Write(g_exeBase + UI_ROUTINE_CALL, uiCall, 5);
            Write(g_exeBase + PREPARE_CALL, prepareCall, 5);
            Write(g_exeBase + PLACE_PRESS_SITE, placePress, 6);
            Write(g_exeBase + BUY_PRESS_SITE, buyPress, 6);
            for (int i = 0; i < PICK_RAY_SITES; i++) Write(g_exeBase + kPickRaySites[i], pickRay[i], 6);
            relays = true;
            for (int i = 0; i < STEP_CALL_COUNT; i++) {
                unsigned char call[5];
                if (!CallTo(kStepCalls[i], g_relay + kStepSlots[i] * 16, call)) { relays = false; break; }
                Write(g_exeBase + kStepCalls[i], call, 5);
            }
        }
    }
    bool tools = relays && InstallToolStubs();
    GameCalls_Install(); // window buttons; a site that differs is only left alone
    BrushTools_Install(); // the terrain brush's press reads; without them the terrain brushes stay as before
    Logf("republicmp  SIM_PATCHES: late hook %d, relays %d (building visibility, saplings, render before Early, interface searches, Prepare at speed 0, window cost display, placing press, buy press, route tools, pick ray), tool stubs %d (%d calls)%s",
         late, relays, tools, (int)TOOL_CALL_COUNT, sitesOk ? "" : " - unexpected bytes at a relay site");
    return late && relays && tools;
}

bool SimPatches_Apply()
{
    if (g_on) return true;
    if (g_refused) return false;
    for (size_t i = 0; i < PATCH_COUNT; i++) {
        if (!BytesAre(kPatches[i], kPatches[i].original)) {
            Logf("republicmp  SIM_PATCHES: unexpected bytes at 0x%X (%s) - none applied", kPatches[i].rva,
                 kPatches[i].what);
            g_refused = true;
            return false;
        }
    }
    for (size_t i = 0; i < PATCH_COUNT; i++) Write(g_exeBase + kPatches[i].rva, kPatches[i].patch, kPatches[i].size);
    g_populationLogged = false;
    g_populationFaults = 0;
    g_lastPopulation = -1;
    memset(g_privateSearch, 0, sizeof(g_privateSearch)); // each run's interface starts with empty vectors (the old ones leak)
    g_uiEnclosed = 0;
    g_on = true;
    Logf("republicmp  SIM_PATCHES: %d applied (vehicle visibility and placement, sign zones, follow camera, "
         "debug keys) + probe building visibility, saplings, population count, autosave hold", (int)PATCH_COUNT);
    return true;
}

void SimPatches_Tick()
{
    if (!g_on || Determinism_IsActive()) return;
    g_on = false;
    for (size_t i = 0; i < PATCH_COUNT; i++)
        if (BytesAre(kPatches[i], kPatches[i].patch)) Write(g_exeBase + kPatches[i].rva, kPatches[i].original, kPatches[i].size);
    AcquireSRWLockExclusive(&g_saplingLock);
    g_saplingQueue.clear(); // the loads that follow rebuild the forest from the list anyway
    ReleaseSRWLockExclusive(&g_saplingLock);
    Logf("republicmp  SIM_PATCHES: restored (%ld interface frame(s) searched in their own vectors)", g_uiEnclosed);
}
