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
#include "../ui/lang.h"
#include "../ui/native_menu.h"
#include "determinism.h"
#include "held_frame.h"
#include "../core/safe_mem.h"
#include "../core/engine_rva.h"
#include <string.h>
#include <intrin.h>
#include <limits.h>
#include <vector>
#include <algorithm>
#include <functional>

#define WORLD_RVA 0xA1EF20

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
    unsigned char original[32];
    unsigned char patch[32];
    const char* what;
};

#define NOP2 0x66, 0x90
#define NOP5 0x0f, 0x1f, 0x44, 0x00, 0x00
#define NOP6 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00
#define NOP8 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00
#define NOP21 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, \
              0x90, 0x90, 0x90, 0x90
#define NOP23 NOP21, 0x90, 0x90

static const DetPatch kPatches[] = {
    { 0x6d8bb5, 26, { 0x80, 0xbb, 0x18, 0x14, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xe6, 0x08, 0x00, 0x00, 0x80, 0xbb, 0x19, 0x14, 0x00, 0x00, 0x00, 0x0f, 0x85, 0xd9, 0x08, 0x00, 0x00 }, { 0xe9, 0xee, 0x08, 0x00, 0x00, NOP21 }, "vehicle became visible (6d89b0)" },
    { 0x6df5a2, 28, { 0x41, 0x80, 0xbf, 0x18, 0x14, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xc2, 0x03, 0x00, 0x00, 0x41, 0x80, 0xbf, 0x19, 0x14, 0x00, 0x00, 0x00, 0x0f, 0x85, 0xb4, 0x03, 0x00, 0x00 }, { 0xe9, 0xc7, 0x03, 0x00, 0x00, NOP23 }, "vehicle became visible (6daec0)" },
    { 0x6e2dc1, 6, { 0x0f, 0x85, 0x0d, 0x01, 0x00, 0x00 }, { 0xe9, 0x0e, 0x01, 0x00, 0x00, 0x90 }, "road vehicle placement (6e2bb0)" },
    { 0x6e1630, 6, { 0x0f, 0x85, 0x16, 0x01, 0x00, 0x00 }, { 0xe9, 0x17, 0x01, 0x00, 0x00, 0x90 }, "rail vehicle placement (6e13b0)" },
    { 0x69ef0c, 7, { 0x0f, 0xb6, 0x83, 0x18, 0x14, 0x00, 0x00 }, { 0x31, 0xc0, NOP5 }, "vehicle checked flag" },
    { 0x69f751, 2, { 0x74, 0x0f }, { 0xeb, 0x0f }, "docked rail wagon: camera-visible parent step" },
    { 0x56ed36, 2, { 0x75, 0x10 }, { NOP2 }, "road sign zones" },
    { 0x155957, 20, { 0xf3, 0x0f, 0x11, 0x80, 0x5c, 0x11, 0x00, 0x00, 0x48, 0x8b, 0x44, 0x24, 0x40, 0xc6, 0x80, 0x5a, 0x11, 0x00, 0x00, 0x01 }, { 0xc7, 0x80, 0x5c, 0x11, 0x00, 0x00, 0x00, 0x50, 0xc3, 0x47, 0xc6, 0x80, 0x5a, 0x11, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x00 }, "building pass: off-camera flags on the visible branch" },
    { 0x174356, 13, { 0x80, 0xbf, 0x5a, 0x11, 0x00, 0x00, 0x00, 0x0f, 0x84, 0x89, 0x01, 0x00, 0x00 }, { 0xe9, 0x1d, 0x00, 0x00, 0x00, NOP8 }, "construction update: every site as one on the camera" },
    { 0x138c2, 5, { 0xe8, 0xa9, 0xa6, 0x68, 0x00 }, { NOP5 }, "follow camera: train head" },
    { 0x138f7, 5, { 0xe8, 0x74, 0xa6, 0x68, 0x00 }, { NOP5 }, "follow camera: wagon" },
    { 0x1392d, 5, { 0xe8, 0x3e, 0xa6, 0x68, 0x00 }, { NOP5 }, "follow camera: vehicle" },
    { 0x139a2, 5, { 0xe8, 0xc9, 0xa5, 0x68, 0x00 }, { NOP5 }, "ride camera: vehicle" },
    { 0x41de3f, 2, { 0x75, 0x29 }, { NOP2 }, "follow camera: worker list" },
    { 0x1562ce, 2, { 0x74, 0x6a }, { 0xeb, 0x6a }, "building pass: Ctrl+N" },
    { 0x15634e, 6, { 0x0f, 0x84, 0xe2, 0x00, 0x00, 0x00 }, { 0xe9, 0xe3, 0x00, 0x00, 0x00, 0x90 }, "building pass: Ctrl+M" },
    { 0x6d186c, 6, { 0x0f, 0x85, 0x83, 0x38, 0x00, 0x00 }, { NOP6 }, "vehicle routine: T" },
    { 0x33483a, 6, { 0x0f, 0x84, 0x06, 0x01, 0x00, 0x00 }, { 0xe9, 0x07, 0x01, 0x00, 0x00, 0x90 }, "Late body: H (networks)" },
    { 0x3349c3, 2, { 0x74, 0x48 }, { 0xeb, 0x48 }, "Late body: H (cursor)" },
    { 0x76ef33, 6, { 0x0f, 0x84, 0x2b, 0x01, 0x00, 0x00 }, { 0xe9, 0x2c, 0x01, 0x00, 0x00, 0x90 }, "school window: Ctrl list cleanup" },
    { 0x165305, 2, { 0x74, 0x6f }, { 0xeb, 0x6f }, "car building: Ctrl+I list cleanup" },
    { 0x1d655b, 2, { 0x74, 0x21 }, { 0xeb, 0x21 }, "depot: E+L+C slot word" },
    { 0x32b17b, 2, { 0x74, 0x41 }, { 0xeb, 0x41 }, "Early: K (the agents' debug branch)" },
    { 0x873e03, 7, { 0x48, 0x8b, 0x53, 0x38, 0x48, 0x85, 0xd2 }, { 0xe9, 0xe2, 0x00, 0x00, 0x00, NOP2 }, "agents: every one on the every-frame list" },
    { 0x12411, 2, { 0x74, 0x5a }, { 0xeb, 0x5a }, "render: debug calendar keys" },
    { 0x1b1f63, 2, { 0x74, 0x13 }, { 0xeb, 0x13 }, "building consume: LCtrl+Numpad5 debug 5x" },
    { 0x1556c9, 2, { 0x74, 0x1a }, { 0xeb, 0x1a }, "building pass: selected tool" },
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

#define GETNODE_SITE 0x155900
#define GETNODE_IMPORT 0x8B15D0 // the import slot the site calls through
typedef void* (*GetNodeFn)(void* tree, int index);
static const unsigned char kGetNodeOriginal[6] = { 0xff, 0x15, 0xca, 0xbc, 0x75, 0x00 };

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

#define SAPLING_START_RVA 0x317160
#define SAPLING_GROWTH_RVA 0x317270
#define SAPLING_RENDER_CALL 0xAC48
#define SAPLING_LATE_CALL 0x32BF85
#define SAPLING_ENGINE_RVA 0x9DDC88  // engine object
#define SAPLING_ENGINE_MAP 0xF80     // +f80 map, its trees +2f8..+300
#define SAPLING_ENGINE_FOREST 0x1060 // +1060 forest
#define SAPLING_ADD_IMPORT 0x8B22B0  // C3D_FOREST::EditorAddCursorTreesSoviet
#define SAPLING_LIST 0x117D8
#define SAPLING_ENTRY 0x34
#define SAPLING_TIMER 0x20
#define SAPLING_NAME 0x20
#define SAPLING_POSITION 0x28
#define SAPLING_START_BITS 0x3a83126f // 0.001f
static const unsigned char kSaplingRenderOriginal[5] = { 0xe8, 0x13, 0xc5, 0x30, 0x00 };
static const unsigned char kSaplingLateOriginal[5] = { 0xe8, 0xe6, 0xb2, 0xfe, 0xff };

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

#define LATE_RVA 0x32B680
#define POPULATION_OFFSET 0xC7B8
#define CITIZENS_OFFSET 0x12758
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
                if (type && *(unsigned char*)(type + 0xaf8) == 1 && *(unsigned char*)(holder + 0x2d) == 1) continue;
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
#define VISIBLE_OFFSET 0x11E70
#define ENGINE_RVA 0x9DDC88       // engine object: +1098 models, +f80 map quadtree
#define FRAGMENT_IMPORT 0x8B2008  // a model's fragment
#define BUILDINGS_BEGIN_RVA 0xA30AD0
#define BUILDINGS_END_RVA 0xA30AD8
static BYTE g_privateVisible[24] = {}; // a vector header: begin, end, capacity
static bool g_visibleFresh = false;
// The visible branch also gives a building its camera distance (+115c, from
// C3D_CAMERA::GetZDistance at 13ce9c) and +115a = 1; the hints over the
// buildings (30e653: a transformer's connection points) need a near
// distance. The simulation keeps them off the camera (0 / 100000.0, its agent
// level of detail); this PC's view gets the real ones.
#define CAMERA_RVA 0xA33D50
#define NODE_POSITION_IMPORT 0x8B1858 // C3D_NODE::GetPosition
#define CAMERA_ZDISTANCE_IMPORT 0x8B2058 // C3D_CAMERA::GetZDistance
struct VisibleDistance {
    BYTE* building;
    float distance;
};
static std::vector<VisibleDistance> g_visibleDistance;
static std::vector<VisibleKept> g_visibleKept;
static thread_local std::vector<BYTE*> g_tlsSortedStill;
static thread_local std::vector<BYTE*> g_tlsSortedThere;
static thread_local std::vector<VisibleKept> g_tlsRenderKept;
static thread_local bool g_tlsInBuildingRenderPass = false;

static bool BuildSortedPtrRange(BYTE** begin, BYTE** end, std::vector<BYTE*>& out)
{
    out.clear();
    if (!begin || !end || end < begin) return false;
    const size_t count = (size_t)(end - begin);
    if (count >= 200000u) return false;
    out.assign(begin, end);
    std::sort(out.begin(), out.end(), std::less<BYTE*>{});
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

static bool BuildSortedWorldBuildings(std::vector<BYTE*>& out)
{
    BYTE** ptrs[2] = { nullptr, nullptr };
    if (!SafeMemcpyPod(ptrs, g_exeBase + BUILDINGS_BEGIN_RVA, sizeof(ptrs))) {
        out.clear();
        return false;
    }
    return BuildSortedPtrRange(ptrs[0], ptrs[1], out);
}

static inline bool ContainsSortedPtr(const std::vector<BYTE*>& sorted, BYTE* ptr)
{
    return std::binary_search(sorted.begin(), sorted.end(), ptr, std::less<BYTE*>{});
}

static bool IsValidVisibleBuilding(BYTE* b)
{
    if (!IsPointerLikeX64((uintptr_t)b)) return false;
    uintptr_t conn[2] = { 0, 0 };
    if (!SafeMemcpyPod(conn, b + 0xA10, sizeof(conn)))
        return false;
    const uintptr_t connBegin = conn[0];
    const uintptr_t connEnd = conn[1];
    if (connBegin == 0 && connEnd == 0) return true;
    if (!IsPointerLikeX64(connBegin) || connEnd < connBegin) return false;
    uintptr_t byteSpan = connEnd - connBegin;
    if ((byteSpan % 96u) != 0 || (byteSpan / 96u) > 4096u) return false;
    return true;
}

static bool PruneVisibleVector(BYTE* vecHeader, const std::vector<BYTE*>& sortedStill)
{
    if (!vecHeader) return false;
    BYTE** ptrs[2] = { nullptr, nullptr };
    if (!SafeMemcpyPod(ptrs, vecHeader, sizeof(ptrs))) return false;
    BYTE** begin = ptrs[0];
    BYTE** end = ptrs[1];
    if (!begin || end <= begin) return false;
    BYTE** writeIt = begin;
    bool pruned = false;
    for (BYTE** readIt = begin; readIt < end; ++readIt) {
        BYTE* b = *readIt;
        if (b && ContainsSortedPtr(sortedStill, b) && IsValidVisibleBuilding(b)) {
            *writeIt++ = b;
        } else {
            pruned = true;
        }
    }
    *(BYTE***)(vecHeader + 8) = writeIt;
    return pruned;
}

static void SanitizeWorldVisibleVector()
{
    if (!g_exeBase || Determinism_WorldLoading()) return;
    if (BuildSortedWorldBuildings(g_tlsSortedStill)) {
        PruneVisibleVector(g_exeBase + WORLD_RVA + VISIBLE_OFFSET, g_tlsSortedStill);
    }
    g_tlsSortedStill.clear();
}

bool SimPatches_ShouldIncludeMeshlessStorageInVisible(bool nodeVisible, unsigned __int64 storageBegin,
                                                      unsigned __int64 storageEnd, float cameraZDist)
{
    if (nodeVisible) return true;
    if (!storageBegin || storageEnd <= storageBegin) return false;
    return (cameraZDist == cameraZDist) && cameraZDist >= 0.0f && cameraZDist < 1200.0f;
}

static void FillPrivateVisible()
{
    typedef void (*PushFn)(void* vector, void* value);
    typedef BYTE* (*FragmentFn)(void* models, int model, int variant);
    PushFn push = (PushFn)(g_exeBase + 0x54960);
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
            if (!b || !IsValidVisibleBuilding(b)) continue;
            BYTE* type = *(BYTE**)(b + 0x318);
            bool visible = false;
            if (type) {
                BYTE* f = fragment(*(void**)(engine + 0x1098), *(int*)(type + 0x418), *(int*)(b + 0x5F4));
                BYTE* node = f ? (BYTE*)getNode(*(void**)(engine + 0xF80), *(int*)(f + 0x3C)) : nullptr;
                bool nodeVis = (node && node[0x25] != 0);
                float at[4] = {};
                position(b + 0x320, at);
                float dist = zDistance(g_exeBase + CAMERA_RVA, at);
                unsigned __int64 sBeg = *(unsigned __int64*)(b + BLD_STORAGE_OFFSET);
                unsigned __int64 sEnd = *(unsigned __int64*)(b + BLD_STORAGE_OFFSET + 8);
                visible = SimPatches_ShouldIncludeMeshlessStorageInVisible(nodeVis, sBeg, sEnd, dist);
                if (visible) {
                    g_visibleDistance.push_back({ b, dist });
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
    BuildSortedWorldBuildings(g_tlsSortedStill);
    for (const VisibleDistance& d : g_visibleDistance) {
        if (!ContainsSortedPtr(g_tlsSortedStill, d.building) || !IsValidVisibleBuilding(d.building)) continue;
        g_visibleKept.push_back({ d.building, d.building[0x115A], *(float*)(d.building + 0x115C) });
        d.building[0x115A] = 1;
        *(float*)(d.building + 0x115C) = d.distance;
    }
    g_tlsSortedStill.clear();
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
    BuildSortedWorldBuildings(g_tlsSortedStill);
    // What the simulation's own building pass writes with no building on the
    // camera (13ce61/13ce6d; a construction site keeps its distance, 13d963):
    // the same on every PC, whatever this phase or an earlier one left.
    for (const VisibleKept& k : g_visibleKept) {
        if (!ContainsSortedPtr(g_tlsSortedStill, k.building) || !IsValidVisibleBuilding(k.building)) continue;
        bool typed = *(void**)(k.building + 0x318) != nullptr;
        k.building[0x115A] = 0;
        *(float*)(k.building + 0x115C) = typed ? 100000.0f : k.distance;
    }
    g_visibleKept.clear();
    // Prune both the private and shared visible vectors against `still` so a building
    // deleted during Late (e.g. finished demolition site) never remains in world+0x11DC0
    // on subsequent held frames (which skip Early and call 0x2F0F10 -> 0x3C4850 -> 0x3C779F).
    bool prunedPriv = PruneVisibleVector(world + VISIBLE_OFFSET, g_tlsSortedStill);
    bool prunedShared = PruneVisibleVector(g_sharedVisible, g_tlsSortedStill);
    g_tlsSortedStill.clear();
    if (prunedPriv || prunedShared) {
        g_visibleFresh = false;
    }
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
        PushFn push = (PushFn)(g_exeBase + 0x54960);
        BuildSortedPtrRange(listed, listedEnd, g_tlsSortedStill);
        BuildSortedPtrRange(begin, end, g_tlsSortedThere);
        for (BYTE* b : g_commandKept)
            if (ContainsSortedPtr(g_tlsSortedStill, b) && !ContainsSortedPtr(g_tlsSortedThere, b)) push(world + VISIBLE_OFFSET, &b);
        g_tlsSortedStill.clear();
        g_tlsSortedThere.clear();
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
    memcpy(kept.search, world + 0x14528, sizeof(kept.search));
    memcpy(world + 0x14528, g_privateToolSearch, sizeof(kept.search));
}

static void PrivateViewOut(const PrivateView& kept)
{
    BYTE* world = g_exeBase + WORLD_RVA;
    memcpy(g_privateToolSearch, world + 0x14528, sizeof(kept.search));
    memcpy(world + 0x14528, kept.search, sizeof(kept.search));
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
    // The tool stays selected on every frame. A tick's Late runs its handlers
    // through the tool stubs below; a held Late is this PC's own altogether.
    // The render thread has finished here (105eb8). (by SoylentGreed)
    if (step) {
        const int64_t lateStepModT0 = DiagPerf_Now();
        RecountPopulation(world);
        bool visible = VisibleIn();
        g_inPhase = true;
        const int64_t lateT0 = DiagPerf_Now();
        o_late(world);
        int64_t lateTicks = DiagPerf_Now() - lateT0;
        if (lateTicks < 0) lateTicks = 0;
        DiagPerf_Record(DIAG_PERF_LATE, lateTicks);
        g_inPhase = false;
        VisibleOut(visible);
        const int64_t lateStepTotalTicks = DiagPerf_Now() - lateStepModT0;
        const int64_t lateStepModTicks = (lateStepTotalTicks > lateTicks) ? (lateStepTotalTicks - lateTicks) : 0;
        DiagPerf_Record(DIAG_PERF_BEFORE_AFTER_EARLY, lateStepModTicks);
    } else {
        const int64_t privLateT0 = DiagPerf_Now();
        PrivateLate(world);
        DiagPerf_Record(DIAG_PERF_PRIVATE_LATE, DiagPerf_Now() - privLateT0);
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
    { 0x330EC1, 0x2372C0 }, { 0x330F4E, 0x22F370 }, { 0x331188, 0x2D7670 }, { 0x3311AA, 0x25E7B0 },
    { 0x3311D0, 0x317810 }, { 0x3311F4, 0x3C7820 }, { 0x33121F, 0x3C7820 }, { 0x331242, 0x3C7820 },
    { 0x331267, 0x3C7820 }, { 0x33128C, 0x3C7820 }, { 0x3312B0, 0x34BBA0 }, { 0x3312D2, 0x3CAA40 },
    { 0x3312F4, 0x3CCD30 }, { 0x331316, 0x3EBB20 }, { 0x33133A, 0x24FC60 }, { 0x331365, 0x24FC60 },
    { 0x331388, 0x24FC60 }, { 0x3313AD, 0x24FC60 }, { 0x3313D2, 0x24FC60 }, { 0x3313FA, 0x250350 },
    { 0x331422, 0x250350 }, { 0x331451, 0x250350 }, { 0x33147C, 0x250350 }, { 0x3314A1, 0x250350 },
    { 0x3314C8, 0x250350 }, { 0x3314F0, 0x250350 }, { 0x331518, 0x250350 }, { 0x331540, 0x250350 },
    { 0x331568, 0x250350 }, { 0x33158C, 0x250580 }, { 0x3315B0, 0x250580 }, { 0x3315D4, 0x2507B0 },
    { 0x3315F8, 0x2507B0 }, { 0x33161C, 0x253550 }, { 0x331640, 0x253550 }, { 0x331667, 0x3EE4A0 },
    { 0x331751, 0x3EE4A0 }, { 0x3316D4, 0x3EE4A0 }, { 0x3316FC, 0x3EE4A0 }, { 0x331723, 0x3EE4A0 },
    { 0x331751, 0x3EE4A0 }, { 0x331751, 0x3EE4A0 }, { 0x3316D4, 0x3EE4A0 }, { 0x3316FC, 0x3EE4A0 },
    { 0x331723, 0x3EE4A0 }, { 0x331667, 0x3EE4A0 }, { 0x33180A, 0x3EEEA0 }, { 0x33182F, 0x3EEEA0 },
    { 0x33180A, 0x3EEEA0 }, { 0x331870, 0x3EEEA0 }, { 0x33189C, 0x3EEEA0 }, { 0x3318EA, 0x3EEEA0 },
    { 0x331910, 0x3EEEA0 }, { 0x33193C, 0x3EEEA0 }, { 0x33196B, 0x3EEEA0 }, { 0x331999, 0x3EEEA0 },
    { 0x3319C7, 0x3EEEA0 }, { 0x331A16, 0x3EEEA0 }, { 0x331A3C, 0x3EEEA0 }, { 0x331A68, 0x3EEEA0 },
    { 0x331A91, 0x3EEEA0 }, { 0x331AB8, 0x3EEEA0 }, { 0x331ADF, 0x3EEEA0 }, { 0x331B06, 0x3EEEA0 },
    { 0x331B2D, 0x3EEEA0 }, { 0x331B54, 0x3EEEA0 }, { 0x331B7B, 0x3EEEA0 }, { 0x331BA2, 0x3EEEA0 },
    { 0x331BC9, 0x3EEEA0 }, { 0x331BF0, 0x3EEEA0 }, { 0x331C17, 0x3EEEA0 }, { 0x331C3E, 0x3EEEA0 },
    { 0x331C65, 0x3EEEA0 }, { 0x331C8C, 0x3EEEA0 }, { 0x331CB3, 0x3EEEA0 }, { 0x331CDA, 0x3EEEA0 },
    { 0x331D01, 0x3EEEA0 }, { 0x331D28, 0x3EEEA0 }, { 0x331D4E, 0x3EEEA0 }, { 0x331D7A, 0x3EEEA0 },
    { 0x331DA2, 0x3EEEA0 }, { 0x331DC8, 0x3EEEA0 }, { 0x331DE8, 0x43B7B0 }, { 0x331E08, 0x2CDC80 },
    { 0x331E2A, 0x3E87B0 }, { 0x331E37, 0x3ED3A0 }, { 0x331E43, 0x3ED400 }, { 0x331E57, 0x3ECF60 },
    { 0x331E76, 0x43C030 }, { 0x331E80, 0x43BAC0 }, { 0x331EA3, 0x3CD9E0 }, { 0x331EDC, 0x3CD9E0 },
    { 0x331F02, 0x3CD9E0 }, { 0x331FF6, 0x3D0060 }, { 0x331FF6, 0x3D0060 }, { 0x331FF6, 0x3D0060 },
    { 0x331FF6, 0x3D0060 }, { 0x331F62, 0x3CFC10 }, { 0x331F62, 0x3CFC10 }, { 0x331F3C, 0x3CFC10 },
    { 0x33211E, 0x3D25D0 }, { 0x332144, 0x3D25D0 }, { 0x33216B, 0x3D25D0 }, { 0x332192, 0x3D25D0 },
    { 0x3321B9, 0x3D25D0 }, { 0x3321E0, 0x3D25D0 }, { 0x332207, 0x3D25D0 }, { 0x33222E, 0x3D25D0 },
    { 0x332255, 0x3D25D0 }, { 0x33227C, 0x3D25D0 }, { 0x3322A2, 0x3D25D0 }, { 0x33232E, 0x3D2FE0 },
    { 0x332533, 0x3D9C80 }, { 0x332533, 0x3D9C80 }, { 0x332559, 0x3D9C80 }, { 0x3325A7, 0x3D9C80 },
    { 0x332533, 0x3D9C80 }, { 0x332533, 0x3D9C80 }, { 0x332533, 0x3D9C80 }, { 0x332533, 0x3D9C80 },
    { 0x3325CD, 0x3D9C80 }, { 0x3325CD, 0x3D9C80 }, { 0x3325F3, 0x3D9C80 }, { 0x332640, 0x3D9C80 },
    { 0x332364, 0x3D7530 }, { 0x332364, 0x3D7530 }, { 0x33238A, 0x3D7530 }, { 0x3323D8, 0x3D7530 },
    { 0x33249A, 0x3D7530 }, { 0x33249A, 0x3D7530 }, { 0x3324C0, 0x3D7530 }, { 0x33250D, 0x3D7530 },
    { 0x332877, 0x3F34E0 }, { 0x332931, 0x43CE10 }, { 0x332993, 0x2D03E0 }, { 0x3329E1, 0x2CE030 },
    { 0x332A01, 0x2CD190 }, { 0x332A23, 0x2CE6A0 }, { 0x332A45, 0x2CE6A0 }, { 0x332A65, 0x2CF610 },
    { 0x332A85, 0x2D0010 }, { 0x332AA5, 0x2D3100 }, { 0x332BB7, 0x1EFBD0 }, { 0x332BD7, 0x1F1250 },
    { 0x332C17, 0x1F43B0 }, { 0x332C37, 0x1F4FB0 }, { 0x332C57, 0x1F16E0 }, { 0x332C77, 0x1F2E40 },
    { 0x332C97, 0x1F32D0 }, { 0x332CB7, 0x1F6410 }, { 0x332CD7, 0x1F6770 }, { 0x332CFA, 0x3F5C20 },
    { 0x332D1A, 0x3F5C20 }, { 0x332D3A, 0x3F5C20 }, { 0x332D44, 0x2D5470 }, { 0x332D4E, 0x2D05F0 },
    { 0x332D5E, 0x43C320 }, { 0x332D94, 0x3F5660 }, { 0x332DB9, 0x3F5660 }, { 0x332DDF, 0x3F5660 },
    { 0x332E05, 0x3F5660 }, { 0x332E2B, 0x3F5660 }, { 0x332E51, 0x3F5660 }, { 0x332E77, 0x3F5660 },
    { 0x332E9D, 0x3F5660 }, { 0x332EC3, 0x3F5660 }, { 0x332EE9, 0x3F5660 }, { 0x332F0B, 0x3E7360 },
    { 0x332F2E, 0x3E7360 }, { 0x332F51, 0x3EAD50 }, { 0x332F74, 0x3EAD50 }, { 0x332F99, 0x3E7EE0 },
    { 0x332FBF, 0x3E7EE0 }, { 0x332FE4, 0x3E7EE0 }, { 0x33300A, 0x3E7EE0 }, { 0x333030, 0x3E7EE0 },
    { 0x333050, 0x2D3580 }, { 0x333070, 0x2D3F00 }, { 0x333090, 0x2D4F10 }, { 0x3330B0, 0x2318D0 },
    { 0x3330D0, 0x231AC0 }, { 0x333157, 0x3E9310 }, { 0x333174, 0x3E9E80 }, { 0x333185, 0x22F680 },
    { 0x33318F, 0x22E9C0 }, { SITE_TOOL_COPY_WEIGHTS_RVA, FN_TOOL_COPY_WEIGHTS_RVA },
};
#define TOOL_CALL_COUNT (sizeof(kToolCalls) / sizeof(kToolCalls[0]))
#define TOOL_STUB_SIZE 32

static bool CallTo(unsigned int site, BYTE* target, unsigned char out[5]);
static BYTE* g_toolStubs = nullptr;
static unsigned __int64 g_toolTarget = 0, g_toolReturn = 0;
static BYTE g_toolSaved[96] = {};
static bool g_toolActed = false;
static PrivateView g_toolKept;

void SimPatches_ClearPlacementActive();
static void SimPatches_BuildingPlacementTool(__int64 param_1, char* param_2, char param_3, char param_4,
                                             unsigned __int64 param_5, unsigned int param_6,
                                             char param_7, char param_8);

static void ToolEnter()
{
    if (g_toolTarget != (unsigned __int64)&SimPatches_BuildingPlacementTool) {
        SimPatches_ClearPlacementActive();
    }
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

typedef void (*UniversalFn)(__int64 param_1, char* param_2, char param_3, char param_4,
                             unsigned __int64 param_5, unsigned int param_6,
                             char param_7, char param_8);
static char g_lastPlacementName[128] = {};
static float g_lastPlacementX = 0.0f;
static float g_lastPlacementY = 0.0f;
static float g_lastPlacementZ = 0.0f;
static ULONGLONG g_lastPlacementTick = 0;
static bool g_placementActive = false;
static INIT_ONCE g_placementInitOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_placementCs;

void SafeCopyPlacementName(const char* src, char* dst, size_t maxLen)
{
    if (!dst || maxLen == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t i = 0;
    while (i < maxLen - 1) {
        char c = src[i];
        if (c == '\0') break;
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) {
            dst[0] = '\0';
            return;
        }
        dst[i++] = c;
    }
    dst[i] = '\0';
}

static bool ReadPlacementCursorSafe(__int64 param_1, float* ox, float* oy, float* oz)
{
    if (!param_1) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        if (ox) *ox = *(float*)(param_1 + CURSOR_X_OFFSET);
        if (oy) *oy = *(float*)(param_1 + CURSOR_Y_OFFSET);
        if (oz) *oz = *(float*)(param_1 + CURSOR_Z_OFFSET);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void SimPatches_ClearPlacementActive()
{
    TsmScopedCsLock lock(&g_placementInitOnce, &g_placementCs);
    g_placementActive = false;
    g_lastPlacementName[0] = '\0';
}

void SimPatches_RecordPlacementForTest(const char* name, float x, float y, float z)
{
    char tempName[128] = {};
    SafeCopyPlacementName(name, tempName, sizeof(tempName));
    TsmScopedCsLock lock(&g_placementInitOnce, &g_placementCs);
    strncpy_s(g_lastPlacementName, sizeof(g_lastPlacementName), tempName, _TRUNCATE);
    g_lastPlacementX = x;
    g_lastPlacementY = y;
    g_lastPlacementZ = z;
    g_lastPlacementTick = GetTickCount64();
    g_placementActive = (tempName[0] != '\0');
}

static void SimPatches_BuildingPlacementTool(__int64 param_1, char* param_2, char param_3, char param_4,
                                             unsigned __int64 param_5, unsigned int param_6,
                                             char param_7, char param_8)
{
    if (param_1 && param_2) {
        char tempName[128] = {};
        SafeCopyPlacementName(param_2, tempName, sizeof(tempName));
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        if (ReadPlacementCursorSafe(param_1, &cx, &cy, &cz) && tempName[0] != '\0') {
            TsmScopedCsLock lock(&g_placementInitOnce, &g_placementCs);
            strncpy_s(g_lastPlacementName, sizeof(g_lastPlacementName), tempName, _TRUNCATE);
            g_lastPlacementX = cx;
            g_lastPlacementY = cy;
            g_lastPlacementZ = cz;
            g_lastPlacementTick = GetTickCount64();
            g_placementActive = true;
        } else {
            SimPatches_ClearPlacementActive();
        }
    } else {
        SimPatches_ClearPlacementActive();
    }

    ((UniversalFn)(g_exeBase + 0x2D75B0))(param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8);
}

static bool SimPatches_BlockAreaCloneTool()
{
    if (SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning())
        return false;
    *(uintptr_t*)(g_exeBase + WORLD_RVA + 0xD4C0) = 0;
    static ULONGLONG s_lastToast = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastToast > 2000) {
        s_lastToast = now;
        TwoStates_AddToast(Tr("⛔ Клонирование районов (копипаст) заблокировано в коопе для предотвращения рассинхрона!",
                              "⛔ Area cloning (copy/paste) is disabled in co-op to prevent desync!"), 3);
    }
    return true;
}

static void SimPatches_AreaCopyTool(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
    if (SimPatches_BlockAreaCloneTool()) return;
    typedef void (*Fn)(__int64, __int64, __int64, __int64);
    ((Fn)(g_exeBase + 0x231430))(a1, a2, a3, a4);
}

static void SimPatches_AreaPasteTool(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
    if (SimPatches_BlockAreaCloneTool()) return;
    typedef void (*Fn)(__int64, __int64, __int64, __int64);
    ((Fn)(g_exeBase + 0x231620))(a1, a2, a3, a4);
}

bool SimPatches_GetActivePlacementInfo(char* outName, size_t maxLen, float* outX, float* outY, float* outZ)
{
    if (!outName || maxLen == 0) return false;
    TsmScopedCsLock lock(&g_placementInitOnce, &g_placementCs);
    if (!g_placementActive) return false;

    if (g_exeBase) {
        BYTE* world = g_exeBase + WORLD_RVA;
        if (world) {
            uintptr_t toolPtr = 0;
            if (SafeReadPod<uintptr_t>(world + WORLD_SELECTED_TOOL_PTR_OFF, &toolPtr) && toolPtr == 0) {
                g_placementActive = false;
                g_lastPlacementName[0] = '\0';
                return false;
            }
        }
    }

    if (GetTickCount64() - g_lastPlacementTick > 200) {
        g_placementActive = false;
        g_lastPlacementName[0] = '\0';
        return false;
    }

    strncpy_s(outName, maxLen, g_lastPlacementName, _TRUNCATE);
    if (outX) *outX = g_lastPlacementX;
    if (outY) *outY = g_lastPlacementY;
    if (outZ) *outZ = g_lastPlacementZ;
    return true;
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
    g_toolStubs = AllocNear(g_exeBase + 0x330E23, size);
    if (!g_toolStubs) return false;
    DWORD old;
    VirtualProtect(g_toolStubs, size, PAGE_EXECUTE_READWRITE, &old);
    BYTE* common = g_toolStubs + TOOL_STUB_SIZE * TOOL_CALL_COUNT;
    EmitCommon(common);
    for (size_t i = 0; i < TOOL_CALL_COUNT; i++) {
        static const unsigned char jmpR10[] = { 0x41, 0xFF, 0xE2 };
        BYTE* p = g_toolStubs + TOOL_STUB_SIZE * i;
        // The copy line and depot tools go through their routes (lockstep/route_tools), which call them.
        unsigned __int64 target = kToolCalls[i][1] == 0x2D05F0   ? (unsigned __int64)&RouteTools_LineCopyTool
                                  : kToolCalls[i][1] == 0x2CE6A0 ? (unsigned __int64)&RouteTools_DepotTool
                                  : kToolCalls[i][1] == 0x3EE4A0 ? (unsigned __int64)&RouteTools_SecondRoadTool
                                  : kToolCalls[i][1] == 0x3E9310 ? (unsigned __int64)&RouteTools_GoThereTool
                                  : kToolCalls[i][1] == 0x3E7360 ? (unsigned __int64)&RouteTools_BojkaTool
                                  : kToolCalls[i][1] == 0x2CDC80 ? (unsigned __int64)&RouteTools_JoinHubStopTool
                                  : kToolCalls[i][1] == 0x3E87B0 ? (unsigned __int64)&RouteTools_JoinHubTool
                                  : kToolCalls[i][1] == 0x2D5470 ? (unsigned __int64)&RouteTools_WorkerRelocateTool
                                  : kToolCalls[i][1] == 0x2CD190 ? (unsigned __int64)&RouteTools_VehicleCopyTool
                                  : kToolCalls[i][1] == 0x2CF610 ? (unsigned __int64)&RouteTools_VehicleReplaceTool
                                  : kToolCalls[i][1] == 0x2D0010 ? (unsigned __int64)&RouteTools_VehicleReplaceAutoTool
                                  : kToolCalls[i][1] == 0x2D3100 ? (unsigned __int64)&RouteTools_VehicleToVehicleTool
                                  : kToolCalls[i][1] == 0x1F32D0 ? (unsigned __int64)&RouteTools_VehicleSendConstrTool
                                  : kToolCalls[i][1] == 0x317810 ? (unsigned __int64)&BrushTools_Tree
                                  : kToolCalls[i][1] == 0x3C7820 ? (unsigned __int64)&BrushTools_Terrain
                                  : kToolCalls[i][1] == 0x3CCD30 ? (unsigned __int64)&BrushTools_DemolishTrees
                                  : kToolCalls[i][1] == 0x3EBB20 ? (unsigned __int64)&BrushTools_Topsoil
                                  : kToolCalls[i][1] == 0x43B7B0 ? (unsigned __int64)&HoverTools_Waypoint
                                  : kToolCalls[i][1] == 0x43C030 ? (unsigned __int64)&HoverTools_SignErase
                                  : kToolCalls[i][1] == 0x43BAC0 ? (unsigned __int64)&HoverTools_Sign
                                  : kToolCalls[i][1] == 0x43C320 ? (unsigned __int64)&HoverTools_RailPlace
                                  : kToolCalls[i][1] == 0x43CE10 ? (unsigned __int64)&HoverTools_RailRemove
                                  : kToolCalls[i][1] == 0x43C130 ? (unsigned __int64)&HoverTools_Junction
                                  : kToolCalls[i][1] == 0x1EFBD0 ? (unsigned __int64)&SourceSlots_AddSourceTool
                                  : kToolCalls[i][1] == 0x1F1250 ? (unsigned __int64)&SourceSlots_CopySourceTool
                                  : kToolCalls[i][1] == 0x22F680 ? (unsigned __int64)&RoadSync_CtrlRectUpgradeTool
                                  : kToolCalls[i][1] == 0x22E9C0 ? (unsigned __int64)&RoadSync_CtrlRectDemolishTool
                                  : kToolCalls[i][1] == 0x22F370 ? (unsigned __int64)&RoadSync_ShiftCancelTool
                                  : kToolCalls[i][1] == 0x2D3580 ? (unsigned __int64)&CargoSettings_AddStationTool
                                  : kToolCalls[i][1] == 0x2D3F00 ? (unsigned __int64)&CargoSettings_AddStationDistTool
                                  : kToolCalls[i][1] == 0x2D7670 ? (unsigned __int64)&SimPatches_BuildingPlacementTool
                                  : kToolCalls[i][1] == 0x2318D0 ? (unsigned __int64)&SimPatches_AreaCopyTool
                                  : kToolCalls[i][1] == 0x231AC0 ? (unsigned __int64)&SimPatches_AreaPasteTool
                                  : kToolCalls[i][1] == FN_TOOL_COPY_WEIGHTS_RVA ? (unsigned __int64)&BuildingSwitches_CopyWeightsTool
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
    if (g_on) {
        RecountPopulation(world);
        HeldFrame_BeforeEarly();
    }
    g_inPhase = true;
}

void SimPatches_AfterEarly()
{
    g_inPhase = false;
    SanitizeWorldVisibleVector();
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

#define FRAME_LOCK_SITE 0x11D9F3   // call [EnterCriticalSection] on renderer+0x80
#define FRAME_LOCK_IMPORT 0x8B2830
#define RENDERER_RVA 0x9E03A0      // renderer object; +0x68 its "finished" event
#define RENDERER_FINISHED 0x68
#define RENDER_WAIT_MS 2000
static const unsigned char kFrameLockOriginal[6] = { 0xff, 0x15, 0x37, 0x4e, 0x79, 0x00 };
typedef void (WINAPI* EnterCsFn)(LPCRITICAL_SECTION);
static volatile LONG g_renderWaitTimeouts = 0;

// The frame already waits at its very start (the timer Start hook, see
// SimPatches_WaitRender): the frame loop rescales the game timer there, and
// the interface on the render thread reads it (PowerTime) for its own
// countdowns. The wait here then returns at once; it stays for safety.
void SimPatches_WaitRender(bool recordTelemetry)
{
    if (!g_on) return;
    BYTE* renderer = *(BYTE**)(g_exeBase + RENDERER_RVA);
    HANDLE finished = renderer ? *(HANDLE*)(renderer + RENDERER_FINISHED) : nullptr;
    if (!finished) return;
    const bool shouldRecord = recordTelemetry && (WaitForSingleObject(finished, 0) == WAIT_TIMEOUT);
    const int64_t waitT0 = shouldRecord ? DiagPerf_Now() : 0;
    if (WaitForSingleObject(finished, RENDER_WAIT_MS) == WAIT_TIMEOUT) {
        if (InterlockedIncrement(&g_renderWaitTimeouts) <= 3)
            Logf("republicmp  SIM_PATCHES: render took over %d ms - waiting before the next frame", RENDER_WAIT_MS);
        // A slow/background render must not overlap a deterministic tick.
        // The native wait at 105eb8 is unbounded too. (by SoylentGreed)
        WaitForSingleObject(finished, INFINITE);
    }
    if (shouldRecord) {
        DiagPerf_Record(DIAG_PERF_WAIT_RENDER, DiagPerf_Now() - waitT0);
    }
}

static void DetFrameLock(LPCRITICAL_SECTION cs)
{
    SimPatches_WaitRender(false);
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

#define UI_ROUTINE_RVA 0x3365B0
#define UI_ROUTINE_CALL 0x4A8CE7
#define SEARCH_WORKSPACE_OFFSET 0x14528
#define SEARCH_WORKSPACE_SIZE 72 // three vector headers {begin, end, capacity}
#define GAME_TIMER_RVA_VIEW 0xA1EEF0
static const unsigned char kUiCallOriginal[5] = { 0xe8, 0xc4, 0xd8, 0xe8, 0xff };
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
    const int64_t uiT0 = DiagPerf_Now();
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
    const int64_t uiSwBeforeT0 = DiagPerf_Now();
    VehicleSwitches_Before(g_inPhase);
    BuildingSwitches_Before(g_inPhase);
    CargoSettings_Before(g_inPhase);
    SourceSlots_Before(g_inPhase);
    int64_t uiSwBeforeTicks = DiagPerf_Now() - uiSwBeforeT0;
    if (uiSwBeforeTicks < 0) uiSwBeforeTicks = 0;
    BYTE* uiTimer = g_exeBase + GAME_TIMER_RVA_VIEW;
    float savedUnscaledFps = *(float*)(uiTimer + 4);
    *(float*)(uiTimer + 4) = MeasureClockFps(&g_uiClock);
    __int64 r = original(world);
    *(float*)(uiTimer + 4) = savedUnscaledFps;
    // The cargo settings first: a half they put back is the same again for the switches.
    const int64_t uiSwAfterT0 = DiagPerf_Now();
    CargoSettings_After(g_inPhase);
    SourceSlots_After(g_inPhase);
    BuildingSwitches_After(g_inPhase);
    VehicleSwitches_After(g_inPhase);
    Commands_GuardSettings();
    int64_t uiSwAfterTicks = DiagPerf_Now() - uiSwAfterT0;
    if (uiSwAfterTicks < 0) uiSwAfterTicks = 0;
    DiagPerf_Record(DIAG_PERF_UI_SWITCHES, uiSwBeforeTicks + uiSwAfterTicks);
    Determinism_UiSpeedEnd();
    VisibleOut(view);
    memcpy(g_privateSearch, shared, sizeof(saved));
    memcpy(shared, saved, sizeof(saved));
    InterlockedIncrement(&g_uiEnclosed);
    DiagPerf_Record(DIAG_PERF_UI_ROUTINE, DiagPerf_Now() - uiT0);
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

#define PREPARE_RVA 0x3F73C0
#define PREPARE_CALL 0x11DA56
#define WORLD_SPEED_OFFSET 0x5CC
static const unsigned char kPrepareCallOriginal[5] = { 0xe8, 0x65, 0x99, 0x2d, 0x00 };

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
    const int64_t prepTotalT0 = DiagPerf_Now();
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
    HeldFrame_BeginCameraPrepare(fps);
    int64_t prepNativeTicks = 0;
    {
        Determinism_PrivateScope privateRng;
        const int64_t prepNativeT0 = DiagPerf_Now();
        original(world);
        prepNativeTicks = DiagPerf_Now() - prepNativeT0;
        if (prepNativeTicks < 0) prepNativeTicks = 0;
    }
    HeldFrame_EndCameraPrepare();
    memcpy(timer, tickTimer, sizeof(tickTimer));
    if (scope) Determinism_ToolScopeEnd();
    PrivateViewOut(kept);
    const int64_t prepTotalTicks = DiagPerf_Now() - prepTotalT0;
    const int64_t prepModTicks = (prepTotalTicks > prepNativeTicks) ? (prepTotalTicks - prepNativeTicks) : 0;
    DiagPerf_Record(DIAG_PERF_PREPARE_NATIVE, prepNativeTicks);
    DiagPerf_Record(DIAG_PERF_PREPARE_MOD, prepModTicks);
}

// Late runs even at speed 0: 547ef0 clamps road speeds without time passing;
// 333bd0 can roll weather with a zero timer, and 5cfea0 changes road wetness.
// Only the admitted tick may run these calls; loading paths stay native.
// The existing session patches already own citizen initialization and the
// building FPS branches; those are not patched twice. (by SoylentGreed)
static void DetPieceSpeed(void* network)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*))(g_exeBase + 0x56E0F0))(network);
}
static void DetWeather(void* world, unsigned long long init)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*, unsigned long long))(g_exeBase + 0x352540))(world, init);
}
static void DetRoadWeather(void* world, unsigned long long init)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*, unsigned long long))(g_exeBase + 0x5F76A0))(world, init);
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
        ((void (*)(void*, unsigned long long))(g_exeBase + 0x148770))(world, all);
}
static void DetConnectionChecks(void* world)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*))(g_exeBase + 0x147630))(world);
}
// The ground-paint queue world+130d0 (filled by 40eaa0 when roads and
// buildings are built) is drained by 40ece0 at 3155f4 on every frame too: a
// held frame painted the ground of the newest construction on this PC first,
// and with commands captured those EditMask calls looked like a tool's.
static void DetGroundPaint(void* world)
{
    if (!Determinism_IsActive() || Determinism_IsStepFrame())
        ((void (*)(void*))(g_exeBase + 0x432A50))(world);
}
// The building windows (6f8cc0, 7fc880) show a construction cost every frame
// through 1a48c0, whose 1a0570 starts with building +0x600 = +0x604 (1a0612).
// The simulation reads +0x600 (15e18e, 168900), so an open window changed
// what that building's construction charged on its PC only. The word is put
// back when the call made it the copy. (by SoylentGreed)
static float DetDisplayCost(void* world, BYTE* building, unsigned long long flag)
{
    typedef float (*CostFn)(void*, BYTE*, unsigned long long);
    CostFn original = (CostFn)(g_exeBase + 0x1BD700);
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
#define PLACE_PRESS_SITE 0x2E8DBC
#define PLACE_PRESS_IMPORT 0x8B1958 // C3D_INPUT::GetMouseLeftPress
static const unsigned char kPlacePressOriginal[6] = { 0xff, 0x15, 0x96, 0x8b, 0x5c, 0x00 };
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
#define BUY_PRESS_SITE 0x852642
static const unsigned char kBuyPressOriginal[6] = { 0xff, 0x15, 0x10, 0xf3, 0x05, 0x00 };

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
#define NORMALIZE_IMPORT 0x8B1118 // C3DVec3Normalize
static const unsigned kPickRaySites[PICK_RAY_SITES] = { 0x3E3751, 0x3F39FF, 0x3CD494, 0x3EF520 };
static const unsigned char kPickRayOriginal[PICK_RAY_SITES][6] = { { 0xff, 0x15, 0xc1, 0xd9, 0x4c, 0x00 },
                                                                   { 0xff, 0x15, 0x13, 0xd7, 0x4b, 0x00 },
                                                                   { 0xff, 0x15, 0x7e, 0x3c, 0x4e, 0x00 },
                                                                   { 0xff, 0x15, 0xf2, 0x1b, 0x4c, 0x00 } };
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

static void DetVehHighlightMain(void* vehicle, unsigned char a2, unsigned char a3, const float* colorRgba, unsigned char flag)
{
    HeldFrame_SubmitVehicleHighlight(vehicle, a2, a3, colorRgba, flag, false);
}

static void DetVehHighlightExtra(void* vehicle, unsigned char a2, unsigned char a3, const float* colorRgba, unsigned char flag)
{
    HeldFrame_SubmitVehicleHighlight(vehicle, a2, a3, colorRgba, flag, true);
}

#define HOVER_PICK_RVA 0x30DBC0
static void DetHoverPick(__int64 world)
{
    typedef void (*HoverPickFn)(__int64);
    HoverPickFn original = (HoverPickFn)(g_exeBase + HOVER_PICK_RVA);
    if (!g_on || !Determinism_IsActive() || Determinism_WorldLoading() || world != (__int64)(g_exeBase + WORLD_RVA)) {
        SanitizeWorldVisibleVector();
        original(world);
        return;
    }
    PrivateView kept;
    PrivateViewIn(kept);
    SanitizeWorldVisibleVector();
    {
        Determinism_PrivateScope privateRng;
        original(world);
    }
    PrivateViewOut(kept);
}

#define STEP_CALL_COUNT 39
static const unsigned kStepCalls[STEP_CALL_COUNT] = { 0x32C9AC, 0x32C9B8, 0x32C9C4, 0x32BFB0, 0x32BFBA, 0x32BC98, 0x32BCA0, 0x333DF4, 0x72EF89, 0x72F078, 0x72F1BE, 0x7303F9, 0x73044D, 0x83CFE6, 0x83D0DC, 0x83D200, 0x2CE0D0, 0x2D0493, 0x2D09EC, 0x2CF0BE, 0x2D351A, 0x2D59F3, 0x2CDA22, 0x2CFDEF, 0x2CFF5B, 0x2D036F, 0x1F38D6, 0x1F3BB8, CHEAT_KEYS_CALL_RVA, 0x274865, 0x43018F, 0x4301A8, 0x430A16, 0x430A2D, 0x431623, 0x43163B, 0x4321E9, 0x4321FE, 0x11DB39 };
static const unsigned kStepTargets[STEP_CALL_COUNT] = { 0x56E0F0, 0x56E0F0, 0x56E0F0, 0x352540, 0x5F76A0, 0x148770, 0x147630, 0x432A50, 0x1BD700, 0x1BD700, 0x1BD700, 0x1BD700, 0x1BD700, 0x1BD700, 0x1BD700, 0x1BD700, 0x2D0AB0, 0x2D0AB0, 0x430A90, 0x42F980, 0x430A90, 0x42F980, 0x42F980, 0x42F980, 0x42F980, 0x42F980, 0x42F980, 0x42F980, CHEAT_KEYS_STEP_RVA, 0x41BA30, 0x6A2010, 0x69FB10, 0x6A2010, 0x69FB10, 0x6A2010, 0x69FB10, 0x6A2010, 0x69FB10, HOVER_PICK_RVA };
static const int kStepSlots[STEP_CALL_COUNT] = { 6, 6, 6, 7, 8, 9, 10, 11, 12, 12, 12, 12, 12, 12, 12, 12, 15, 16, 17, 18,
                                                 17, 18, 18, 18, 18, 18, 18, 18, 20, 21,
                                                 22, 23, 22, 23, 22, 23, 22, 23,
                                                 24 }; // relay slot of each site
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
#define RELAY_SLOTS 25

static bool CallTo(unsigned int site, BYTE* target, unsigned char out[5])
{
    long long rel = (long long)target - (long long)(g_exeBase + site + 5);
    if (rel < INT_MIN || rel > INT_MAX) return false;
    int rel32 = (int)rel;
    out[0] = 0xe8;
    memcpy(out + 1, &rel32, 4);
    return true;
}

#define BUILDING_RENDER_PASS_RVA 0x2F8F50
static const unsigned char kBuildingRenderExpect[18] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xCE, 0xFF, 0xFF
};
typedef void* (*BuildingRenderPassFn)(void* rcx, void* rdx, void* r8, void* r9);
static BuildingRenderPassFn o_buildingRenderPass = nullptr;

void RestoreVisibleKeptSafe(VisibleKept* kept, size_t keptCount)
{
    if (!kept || keptCount == 0) return;
    DiagLog_BeginSafeSeh();
    size_t i = 0;
    while (i < keptCount) {
        volatile size_t cur = i;
        __try {
            for (; i < keptCount; i++) {
                cur = i;
                if (!kept[i].building) continue;
                kept[i].building[0x115A] = kept[i].visible;
                *(float*)(kept[i].building + 0x115C) = kept[i].distance;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            i = cur + 1u;
        }
    }
    DiagLog_EndSafeSeh();
}

static void CollectAndApplyRenderPassVisible(VisibleKept* keptBuf, size_t maxCap, size_t* outKeptCount)
{
    size_t keptCount = 0;
    *outKeptCount = 0;
    DiagLog_BeginSafeSeh();
    __try {
        typedef void* (*PositionFn)(void* node, float* out);
        typedef float (*ZDistanceFn)(void* camera, float* position);
        PositionFn position = *(PositionFn*)(g_exeBase + NODE_POSITION_IMPORT);
        ZDistanceFn zDistance = *(ZDistanceFn*)(g_exeBase + CAMERA_ZDISTANCE_IMPORT);
        BYTE** it = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
        BYTE** end = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
        if (it && end > it && position && zDistance) {
            for (; it < end && keptCount < maxCap; ++it) {
                BYTE* b = *it;
                if (!b) continue;
                BYTE* type = *(BYTE**)(b + 0x318);
                if (type) {
                    float at[4] = {};
                    position(b + 0x320, at);
                    float dist = zDistance(g_exeBase + CAMERA_RVA, at);
                    if (dist < 4000.0f && isfinite(dist)) {
                        keptBuf[keptCount].building = b;
                        keptBuf[keptCount].visible = b[0x115A];
                        keptBuf[keptCount].distance = *(float*)(b + 0x115C);
                        keptCount++;
                        *outKeptCount = keptCount;
                        b[0x115A] = 1;
                        *(float*)(b + 0x115C) = dist;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void* InvokeBuildingRenderPassWithRestore(
    void* rcx, void* rdx, void* r8, void* r9,
    VisibleKept* keptBuf, size_t keptCount, bool* inPassFlag)
{
    *inPassFlag = true;
    void* res = nullptr;
    __try {
        res = o_buildingRenderPass(rcx, rdx, r8, r9);
    } __finally {
        RestoreVisibleKeptSafe(keptBuf, keptCount);
        *inPassFlag = false;
    }
    return res;
}

void SimPatches_TestBuildingRenderPassRestore(
    VisibleKept* kept, size_t keptCount, void (*passBody)(void*), void* ctx, bool* outInPassAfter)
{
    g_tlsInBuildingRenderPass = true;
    __try {
        if (passBody) passBody(ctx);
    } __finally {
        RestoreVisibleKeptSafe(kept, keptCount);
        g_tlsInBuildingRenderPass = false;
    }
    if (outInPassAfter) *outInPassAfter = g_tlsInBuildingRenderPass;
}

static void* HookedBuildingRenderPass(void* rcx, void* rdx, void* r8, void* r9)
{
    if (Determinism_WorldLoading() || !SafeIsWorldLoaded(g_exeBase) || !o_buildingRenderPass || g_tlsInBuildingRenderPass) {
        return o_buildingRenderPass ? o_buildingRenderPass(rcx, rdx, r8, r9) : nullptr;
    }
    BYTE** ptrs[2] = { nullptr, nullptr };
    if (!SafeMemcpyPod(ptrs, g_exeBase + BUILDINGS_BEGIN_RVA, sizeof(ptrs)) || !ptrs[0] || ptrs[1] <= ptrs[0]) {
        return o_buildingRenderPass(rcx, rdx, r8, r9);
    }
    const size_t total = (size_t)(ptrs[1] - ptrs[0]);
    if (total == 0 || total >= 100000) {
        return o_buildingRenderPass(rcx, rdx, r8, r9);
    }
    g_tlsRenderKept.clear();
    g_tlsRenderKept.resize(total);
    size_t keptCount = 0;
    CollectAndApplyRenderPassVisible(g_tlsRenderKept.data(), total, &keptCount);
    void* res = InvokeBuildingRenderPassWithRestore(
        rcx, rdx, r8, r9, g_tlsRenderKept.data(), keptCount, &g_tlsInBuildingRenderPass);
    g_tlsRenderKept.clear();
    return res;
}

typedef void (__fastcall* C3dSaveOriginalValuesFn)(void* material);
typedef void (__fastcall* C3dChangeValuesToFn)(void* material, const float color[4]);
typedef void (__fastcall* C3dRestoreOriginalValuesFn)(void* material);

static C3dSaveOriginalValuesFn    o_c3dSaveOriginalValues    = nullptr;
static C3dChangeValuesToFn        o_c3dChangeValuesTo        = nullptr;
static C3dRestoreOriginalValuesFn o_c3dRestoreOriginalValues = nullptr;

static void __fastcall H_C3dSaveOriginalValues(void* material)
{
    if (!SafeIsValidMaterial(material)) {
        static unsigned s_blockedCount = 0;
        if (H && H->log && ((++s_blockedCount <= 10) || (s_blockedCount % 1000 == 0))) {
            Logf("republicmp  STABILITY: intercepted invalid C3D_MATERIAL (%p) in SaveOriginalValues (count=%u)", material, s_blockedCount);
        }
        return;
    }
    DiagLog_BeginSafeSeh();
    __try {
        if (o_c3dSaveOriginalValues) o_c3dSaveOriginalValues(material);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (H && H->log) Logf("republicmp  STABILITY: SEH caught exception in SaveOriginalValues for material %p", material);
    }
    DiagLog_EndSafeSeh();
}

static void __fastcall H_C3dChangeValuesTo(void* material, const float color[4])
{
    if (!SafeIsValidMaterial(material)) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (o_c3dChangeValuesTo) o_c3dChangeValuesTo(material, color);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (H && H->log) Logf("republicmp  STABILITY: SEH caught exception in ChangeValuesTo for material %p", material);
    }
    DiagLog_EndSafeSeh();
}

static void __fastcall H_C3dRestoreOriginalValues(void* material)
{
    if (!SafeIsValidMaterial(material)) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (o_c3dRestoreOriginalValues) o_c3dRestoreOriginalValues(material);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (H && H->log) Logf("republicmp  STABILITY: SEH caught exception in RestoreOriginalValues for material %p", material);
    }
    DiagLog_EndSafeSeh();
}

void SimPatches_TestC3dSaveOriginalValues(void* material)
{
    H_C3dSaveOriginalValues(material);
}

void SimPatches_TestC3dChangeValuesTo(void* material, const float color[4])
{
    H_C3dChangeValuesTo(material, color);
}

void SimPatches_TestC3dRestoreOriginalValues(void* material)
{
    H_C3dRestoreOriginalValues(material);
}

// ---------------------------------------------------------------------------
// Terminal & Attached Aircraft Stand Arrival/Corridor Lock Sanitizer (0x6CD1E0)
// ---------------------------------------------------------------------------

static const unsigned char kVehArrivalStepExpect[15] = {
    0x40, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00
};
typedef void (__fastcall* VehicleArrivalStepFn)(void* vehicle);
static VehicleArrivalStepFn o_vehicleArrivalStep = nullptr;

static int GetVehicleCategory(BYTE* veh)
{
    if (!IsPointerLikeX64((uintptr_t)veh)) return -1;
    BYTE* vtype = *(BYTE**)(veh + VEH_TYPE_PTR_OFFSET);
    if (!IsPointerLikeX64((uintptr_t)vtype)) return -1;
    return *(int32_t*)(vtype + VEH_TYPE_CATEGORY_OFFSET);
}

static int GetBuildingBaySlotCount(BYTE* bld)
{
    if (!IsPointerLikeX64((uintptr_t)bld)) return 0;
    uintptr_t pBegin = *(uintptr_t*)(bld + BLD_BAY_POINTS_BEGIN_OFFSET);
    uintptr_t pEnd   = *(uintptr_t*)(bld + BLD_BAY_POINTS_END_OFFSET);
    if (pEnd <= pBegin || !IsPointerLikeX64(pBegin)) return 0;
    size_t count = (pEnd - pBegin) / BLD_BAY_POINT_STRIDE;
    return (count <= 32) ? (int)count : 0;
}

static bool ShouldReleaseCorridorLock(BYTE* bld, BYTE* lockVeh)
{
    if (!IsPointerLikeX64((uintptr_t)lockVeh)) return true;
    if (*(BYTE**)(lockVeh + VEH_CORRIDOR_LOCK_BLD_OFFSET) != bld) return true;
    const int cat = GetVehicleCategory(lockVeh);
    if (lockVeh[VEH_IS_RAIL_FLAG_OFFSET] != 0 || cat == 3 || cat == 4) return false;
    if (*(int32_t*)(lockVeh + VEH_BAY_SLOT_IDX_OFFSET) <= -10) return false;
    if (IsPointerLikeX64((uintptr_t)bld) && *(uintptr_t*)(bld + BLD_CONSTR_SITE_PTR_OFFSET) != 0) return false;
    if ((cat == 8 || cat == 10) && *(int32_t*)(lockVeh + VEH_HELI_FLIGHT_STATE_OFF) != 0) return true;
    if (cat == 8 &&
        *(uintptr_t*)(bld + BLD_ATTACHED_END_OFFSET) > *(uintptr_t*)(bld + BLD_ATTACHED_BEGIN_OFFSET)) {
        return true;
    }
    return (*(BYTE**)(lockVeh + VEH_BAY_TARGET_BLD_OFFSET) == nullptr &&
            *(int32_t*)(lockVeh + VEH_BAY_SLOT_IDX_OFFSET) == -1 &&
            *(int32_t*)(lockVeh + VEH_WAYPOINT_INDEX_OFFSET) >= 3);
}

static void SanitizeBuildingCorridorLock(BYTE* bld)
{
    if (!IsPointerLikeX64((uintptr_t)bld)) return;
    BYTE* lockVeh = *(BYTE**)(bld + BLD_CORRIDOR_LOCK_VEH_OFFSET);
    if (!lockVeh || !ShouldReleaseCorridorLock(bld, lockVeh)) return;
    *(BYTE**)(bld + BLD_CORRIDOR_LOCK_VEH_OFFSET) = nullptr;
    if (IsPointerLikeX64((uintptr_t)lockVeh) && *(BYTE**)(lockVeh + VEH_CORRIDOR_LOCK_BLD_OFFSET) == bld) {
        *(BYTE**)(lockVeh + VEH_CORRIDOR_LOCK_BLD_OFFSET) = nullptr;
    }
}

static void SanitizeTerminalAndAttachedLocks(BYTE* bld)
{
    if (!IsPointerLikeX64((uintptr_t)bld)) return;
    SanitizeBuildingCorridorLock(bld);
    BYTE** attBegin = *(BYTE***)(bld + BLD_ATTACHED_BEGIN_OFFSET);
    BYTE** attEnd   = *(BYTE***)(bld + BLD_ATTACHED_END_OFFSET);
    if (!IsPointerLikeX64((uintptr_t)attBegin) || attEnd <= attBegin || (attEnd - attBegin) > 32) return;
    for (BYTE** it = attBegin; it < attEnd; ++it) {
        SanitizeBuildingCorridorLock(*it);
    }
}

struct ReservedStandMatch {
    BYTE* hostBld;
    int   localSlot;
    int   globalSlot;
    float distSq;
};

static void ScanHostBldStandReservations(BYTE* hostBld, BYTE* veh, int baseSlotOffset, ReservedStandMatch* best)
{
    int slots = GetBuildingBaySlotCount(hostBld);
    if (slots <= 0) return;
    BYTE* ptsBase = *(BYTE**)(hostBld + BLD_BAY_POINTS_BEGIN_OFFSET);
    const float vx = *(const float*)(veh + VEH_POS_OFFSET + 0);
    const float vz = *(const float*)(veh + VEH_POS_OFFSET + 8);
    for (int s = 0; s < slots; ++s) {
        if (hostBld[BLD_BAY_STATE_BEGIN_OFFSET + s] == 0) continue;
        if (*(BYTE**)(hostBld + BLD_BAY_VEH_BEGIN_OFFSET + (size_t)s * 8) != veh) continue;
        const float* p2 = (const float*)(ptsBase + (size_t)s * BLD_BAY_POINT_STRIDE + 0x0C);
        const float dx = p2[0] - vx;
        const float dz = p2[2] - vz;
        const float d2 = dx * dx + dz * dz;
        if (!best->hostBld || d2 < best->distSq) {
            if (best->hostBld) {
                best->hostBld[BLD_BAY_STATE_BEGIN_OFFSET + best->localSlot] = 0;
                *(BYTE**)(best->hostBld + BLD_BAY_VEH_BEGIN_OFFSET + (size_t)best->localSlot * 8) = nullptr;
            }
            best->hostBld    = hostBld;
            best->localSlot  = s;
            best->globalSlot = baseSlotOffset + s;
            best->distSq     = d2;
        } else {
            hostBld[BLD_BAY_STATE_BEGIN_OFFSET + s] = 0;
            *(BYTE**)(hostBld + BLD_BAY_VEH_BEGIN_OFFSET + (size_t)s * 8) = nullptr;
        }
    }
}

static void PushVehicleWaypoint3f(BYTE* veh, const float* pt)
{
    uintptr_t begin = *(uintptr_t*)(veh + VEH_WAYPOINTS_BEGIN_OFFSET);
    uintptr_t end   = *(uintptr_t*)(veh + VEH_WAYPOINTS_END_OFFSET);
    uintptr_t cap   = *(uintptr_t*)(veh + VEH_WAYPOINTS_CAP_OFFSET);
    if (IsPointerLikeX64(begin) && end >= begin && end + 12 <= cap) {
        memcpy((void*)end, pt, 12);
        *(uintptr_t*)(veh + VEH_WAYPOINTS_END_OFFSET) = end + 12;
        return;
    }
    if (g_exeBase && g_exeBase[RVA_VEC_WAYPOINT_PUSH_BACK] != 0x00 &&
        g_exeBase[RVA_VEC_WAYPOINT_PUSH_BACK] != 0xC3) {
        typedef void(__fastcall* PushWaypointFn)(void* vec, const void* p);
        ((PushWaypointFn)(g_exeBase + RVA_VEC_WAYPOINT_PUSH_BACK))(veh + VEH_WAYPOINTS_BEGIN_OFFSET, pt);
    }
}

static void AssignReservedAircraftStandIfReady(BYTE* veh, BYTE* bld)
{
    if (GetVehicleCategory(veh) != 8) return;
    if (*(int32_t*)(veh + VEH_HELI_FLIGHT_STATE_OFF) != 0) return;
    if (*(int32_t*)(veh + VEH_BAY_SLOT_IDX_OFFSET) != -1) return;
    if (*(int32_t*)(veh + VEH_LINE_SLOT_IDX_OFFSET) != -1) return;

    ReservedStandMatch best = { nullptr, -1, -1, 1e30f };
    int runningSlots = GetBuildingBaySlotCount(bld);
    ScanHostBldStandReservations(bld, veh, 0, &best);

    BYTE** attBegin = *(BYTE***)(bld + BLD_ATTACHED_BEGIN_OFFSET);
    BYTE** attEnd   = *(BYTE***)(bld + BLD_ATTACHED_END_OFFSET);
    if (IsPointerLikeX64((uintptr_t)attBegin) && attEnd > attBegin && (attEnd - attBegin) <= 32) {
        for (BYTE** it = attBegin; it < attEnd; ++it) {
            BYTE* att = *it;
            ScanHostBldStandReservations(att, veh, runningSlots, &best);
            runningSlots += GetBuildingBaySlotCount(att);
        }
    }
    if (!best.hostBld || best.localSlot < 0) return;

    BYTE* hostLock = *(BYTE**)(best.hostBld + BLD_CORRIDOR_LOCK_VEH_OFFSET);
    BYTE* vehSelf  = *(BYTE**)(veh + 0x390);
    if (!IsPointerLikeX64((uintptr_t)vehSelf)) vehSelf = veh;
    if (hostLock != nullptr && hostLock != vehSelf) return;

    BYTE* oldLockBld = *(BYTE**)(veh + VEH_CORRIDOR_LOCK_BLD_OFFSET);
    if (IsPointerLikeX64((uintptr_t)oldLockBld) && *(BYTE**)(oldLockBld + BLD_CORRIDOR_LOCK_VEH_OFFSET) == vehSelf) {
        *(BYTE**)(oldLockBld + BLD_CORRIDOR_LOCK_VEH_OFFSET) = nullptr;
    }
    *(int32_t*)(veh + VEH_BAY_SLOT_IDX_OFFSET) = best.globalSlot;
    *(BYTE**)(best.hostBld + BLD_CORRIDOR_LOCK_VEH_OFFSET) = vehSelf;
    *(BYTE**)(veh + VEH_CORRIDOR_LOCK_BLD_OFFSET) = best.hostBld;
    *(uintptr_t*)(veh + 0x798) = 0;

    uintptr_t v7B8 = *(uintptr_t*)(veh + 0x7B8);
    uintptr_t v7C0 = *(uintptr_t*)(veh + 0x7C0);
    if (v7C0 > v7B8 && g_exeBase && g_exeBase[RVA_VEH_CLEAR_PATH_NODES] != 0x00 &&
        g_exeBase[RVA_VEH_CLEAR_PATH_NODES] != 0xC3) {
        ((void(__fastcall*)(void*))(g_exeBase + RVA_VEH_CLEAR_PATH_NODES))(veh);
    }
    *(uintptr_t*)(veh + 0x6A8) = *(uintptr_t*)(veh + 0x6A0);
    *(uintptr_t*)(veh + 0x6C0) = *(uintptr_t*)(veh + 0x6B8);
    *(uintptr_t*)(veh + VEH_WAYPOINTS_END_OFFSET) = *(uintptr_t*)(veh + VEH_WAYPOINTS_BEGIN_OFFSET);
    *(uintptr_t*)(veh + 0x758) = *(uintptr_t*)(veh + 0x750);
    *(int32_t*)(veh + VEH_WAYPOINT_INDEX_OFFSET) = 0;
    *(uint8_t*)(veh + VEH_CORNER_CUT_APPLIED_OFFSET) = 0;
    *(int32_t*)(veh + VEH_CORNER_TURN_ANGLE_OFFSET) = 0;
    *(float*)(veh + VEH_WAIT_TIMER_OFFSET) = 0.0f;
    *(uint8_t*)(veh + VEH_BAY_WAITING_FLAG_OFFSET) = 0;
    *(uint8_t*)(veh + VEH_BAY_ARRIVED_FLAG_OFFSET) = 0;

    BYTE* slotPts = *(BYTE**)(best.hostBld + BLD_BAY_POINTS_BEGIN_OFFSET) +
                    (size_t)best.localSlot * BLD_BAY_POINT_STRIDE;
    PushVehicleWaypoint3f(veh, (const float*)(slotPts + 0x0C));
    PushVehicleWaypoint3f(veh, (const float*)(slotPts + 0x00));
}

static void SanitizeVehicleArrivalBeforeStep(BYTE* veh)
{
    if (!IsPointerLikeX64((uintptr_t)veh)) return;
    BYTE* lockBld = *(BYTE**)(veh + VEH_CORRIDOR_LOCK_BLD_OFFSET);
    if (IsPointerLikeX64((uintptr_t)lockBld)) {
        SanitizeBuildingCorridorLock(lockBld);
    }
    BYTE* targetBld = *(BYTE**)(veh + VEH_BAY_TARGET_BLD_OFFSET);
    if (IsPointerLikeX64((uintptr_t)targetBld)) {
        SanitizeTerminalAndAttachedLocks(targetBld);
        AssignReservedAircraftStandIfReady(veh, targetBld);
    }
}

static void SanitizeVehicleArrivalAfterStep(BYTE* veh)
{
    if (!IsPointerLikeX64((uintptr_t)veh)) return;
    // Do NOT wipe targetBld (+0x528) or force +0xD39 = 1 on rail vehicles:
    // - RCO track builders (vehCat == 4 + rail construction skill at vehType + 0x85E0) rely on +0x528 != nullptr
    //   when wpCount == 0 for AssignBayPath (0x6B2C00), and wiping +0x528 with +0xD39 = 1 triggers 0x147B1F unassign/recall.
    // - Scheduled rail trains (vehCat == 3/4) natively handle wpIdx >= wpCount && (+0xD39 || +0xD3A) via
    //   0x6CE5FA..0x6CE67B -> 0x6D7DE0(veh, -1, 1), where 0x6D85EF reads +0x528 before orders[orderIdx - 1].
    BYTE* lockBld = *(BYTE**)(veh + VEH_CORRIDOR_LOCK_BLD_OFFSET);
    if (IsPointerLikeX64((uintptr_t)lockBld)) {
        SanitizeBuildingCorridorLock(lockBld);
    }
}

static void __fastcall HookedVehicleArrivalStep(void* vehicle)
{
    const bool sanitize = Determinism_IsActive();
    if (sanitize) {
        DiagLog_BeginSafeSeh();
        __try {
            SanitizeVehicleArrivalBeforeStep((BYTE*)vehicle);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
    }

    if (o_vehicleArrivalStep) {
        o_vehicleArrivalStep(vehicle);
    }

    if (sanitize) {
        DiagLog_BeginSafeSeh();
        __try {
            SanitizeVehicleArrivalAfterStep((BYTE*)vehicle);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
    }
}

void SimPatches_TestSanitizeVehicleArrival(void* vehicle)
{
    DiagLog_BeginSafeSeh();
    __try {
        SanitizeVehicleArrivalBeforeStep((BYTE*)vehicle);
        SanitizeVehicleArrivalAfterStep((BYTE*)vehicle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void SimPatches_TestHookedVehicleArrivalStep(void* vehicle)
{
    HookedVehicleArrivalStep(vehicle);
}

static const unsigned char kIsVehicleAtDepotOrStopExpect[18] = {
    0x4D, 0x8B, 0xD0, 0x48, 0x85, 0xD2, 0x75, 0x03, 0x32, 0xC0,
    0xC3, 0x48, 0x8B, 0x82, 0xB0, 0x05, 0x00, 0x00
};
static void* o_isVehicleAtDepotOrStop = nullptr;

static unsigned char IsVehicleAtDepotOrStopInner(const BYTE* bld, const BYTE* veh, unsigned char a4)
{
    if (!IsPointerLikeX64((uintptr_t)bld)) return 0;
    for (int depth = 0; depth < 16; ++depth) {
        const BYTE* parent = *(const BYTE* const*)(bld + 0x5B0);
        if (!parent) break;
        if (!IsPointerLikeX64((uintptr_t)parent)) return 0;
        bld = parent;
    }
    const BYTE* bldType = *(const BYTE* const*)(bld + 0x318);
    if (!IsPointerLikeX64((uintptr_t)bldType)) return 0;

    int32_t bldCat = *(const int32_t*)(bldType + 0x368);
    if (!veh) {
        if ((uint32_t)bldCat > 0x2Fu) return 0;
        return ((0x84000000C000ULL >> (uint32_t)bldCat) & 1ULL) != 0 ? 1 : 0;
    }
    if (!IsPointerLikeX64((uintptr_t)veh)) return 0;
    const BYTE* vehType = *(const BYTE* const*)(veh + 0x1748);
    if (!vehType) {
        if ((uint32_t)bldCat > 0x2Fu) return 0;
        return ((0x84000000C000ULL >> (uint32_t)bldCat) & 1ULL) != 0 ? 1 : 0;
    }
    if (!IsPointerLikeX64((uintptr_t)vehType)) return 0;

    int32_t vehCat = *(const int32_t*)(vehType + 0x2B8);
    if (bldCat == 0x0E) {
        int32_t bldSub = *(const int32_t*)(bldType + 0x36C);
        if (bldSub == 0x24) {
            if (g_exeBase && g_exeBase[0x6F15B0] != 0x00 && g_exeBase[0x6F15B0] != 0xC3) {
                typedef unsigned char (__fastcall* Sub24CheckFn)(const BYTE*, unsigned int);
                return ((Sub24CheckFn)(g_exeBase + 0x6F15B0))(veh, (unsigned int)a4);
            }
            return a4 ? 1 : 0;
        }
        if (bldSub == 0x1B) {
            return (*(const uint8_t*)(vehType + 0x86D0) != 0 && vehCat == 1) ? 1 : 0;
        }
        return (vehCat == 1) ? 1 : 0;
    }
    if (bldCat == 0x0F) {
        return ((uint32_t)(vehCat - 3) <= 1u) ? 1 : 0;
    }
    if (vehCat == 6 && bldCat == 0x2A) return 1;
    if ((vehCat == 8 || vehCat == 0x0A) && bldCat == 0x2F) return 1;
    return 0;
}

static unsigned char __fastcall HookedIsVehicleAtDepotOrStop(const void* /*world*/, const BYTE* bld,
                                                             const BYTE* veh, unsigned char a4)
{
    unsigned char res = 0;
    DiagLog_BeginSafeSeh();
    __try {
        res = IsVehicleAtDepotOrStopInner(bld, veh, a4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        res = 0;
    }
    DiagLog_EndSafeSeh();
    return res;
}

unsigned char SimPatches_TestIsVehicleAtDepotOrStop(const void* bld, const void* veh, unsigned char a4)
{
    return HookedIsVehicleAtDepotOrStop(nullptr, (const BYTE*)bld, (const BYTE*)veh, a4);
}

// Crash A: guard 0x41E9F0 (UpdateSignalBlock) against corrupt segment->signalBlock pointers
// such as 0x90 dereferenced at 0x41EA0C ([rdx+0x14]) or missing node ptr at 0x41EB5A ([rdx+0x08]).
static const unsigned char kUpdateSignalBlockExpect[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83,
    0xEC, 0x30, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xD9
};
typedef void (__fastcall* UpdateSignalBlockFn)(void* world, void* segPtr);
static UpdateSignalBlockFn o_updateSignalBlock = nullptr;

bool SimPatches_IsSignalSegmentSafe(const void* segPtr)
{
    if (!segPtr || !IsPointerLikeX64((uint64_t)(uintptr_t)segPtr)) {
        return false;
    }
    const uint8_t* seg = (const uint8_t*)segPtr;
    uint64_t sigBlockPtr = 0;
    if (!SafeReadPod(seg + SEG_SIGNAL_BLOCK_PTR_OFFSET, &sigBlockPtr)) {
        return false;
    }
    if (sigBlockPtr != 0) {
        if (!IsPointerLikeX64(sigBlockPtr)) {
            return false;
        }
        const uint8_t* sigBlock = (const uint8_t*)(uintptr_t)sigBlockPtr;
        uint32_t kind = 0;
        if (!SafeReadPod(sigBlock + SIGNAL_BLOCK_KIND_OFFSET, &kind)) {
            return false;
        }
        if (kind != SIGNAL_BLOCK_KIND_SKIP) {
            uint64_t nodePtr = 0;
            if (!SafeReadPod(sigBlock + SIGNAL_BLOCK_NODE_PTR_OFFSET, &nodePtr)) {
                return false;
            }
            return nodePtr != 0 && IsPointerLikeX64(nodePtr);
        }
    }
    uint64_t nodeBounds[2] = { 0, 0 };
    if (!SafeReadPod(seg + SEG_CONNECTED_NODES_BEGIN_OFF, &nodeBounds)) {
        return false;
    }
    const uint64_t beginAddr = nodeBounds[0];
    const uint64_t endAddr = nodeBounds[1];
    if (endAddr < beginAddr || ((endAddr - beginAddr) % sizeof(void*)) != 0) {
        return false;
    }
    const uint64_t count = (endAddr - beginAddr) / sizeof(void*);
    if (count > 1024ULL) {
        return false;
    }
    if (count > 0 && !IsPointerLikeX64(beginAddr)) {
        return false;
    }
    return true;
}

static bool __fastcall HookedUpdateSignalBlockImpl(void* world, void* segPtr)
{
    if (!SimPatches_IsSignalSegmentSafe(segPtr)) {
        static ULONGLONG s_lastWarnTick = 0;
        const ULONGLONG now = GetTickCount64();
        if (s_lastWarnTick == 0 || (now - s_lastWarnTick) >= 2000ULL) {
            s_lastWarnTick = now;
            Logf("republicmp  CRASH_FIX_A: skipped invalid signal segment %p in 0x41E9F0", segPtr);
        }
        return false;
    }
    if (o_updateSignalBlock) {
        o_updateSignalBlock(world, segPtr);
    }
    return true;
}

static void __fastcall HookedUpdateSignalBlock(void* world, void* segPtr)
{
    (void)HookedUpdateSignalBlockImpl(world, segPtr);
}

bool SimPatches_TestHookedUpdateSignalBlock(void* world, void* segPtr)
{
    return HookedUpdateSignalBlockImpl(world, segPtr);
}

void SimPatches_TestBuildVehicleWaypoints(void* vehicle, unsigned char a2)
{
    // Native SOVIET64.exe 0x683240 early-exits at 0x6832EB when veh[0x67D] != 0 && veh[0xD39] != 0.
    // We intentionally no longer hook RVA_VEH_BUILD_WAYPOINTS or clear +0xD39 before it runs.
    (void)vehicle;
    (void)a2;
}

bool SimPatches_Install()
{
    bool late = InstallInlineHook(g_exeBase + LATE_RVA, (void*)&HookedLate, (void**)&o_late,
                                  kLateExpect, sizeof(kLateExpect), "republicmp late body");
    InstallInlineHook(g_exeBase + BUILDING_RENDER_PASS_RVA, (void*)&HookedBuildingRenderPass,
                      (void**)&o_buildingRenderPass, kBuildingRenderExpect,
                      sizeof(kBuildingRenderExpect), "republicmp building render pass");
    InstallInlineHook(g_exeBase + RVA_VEH_ARRIVAL_STEP, (void*)&HookedVehicleArrivalStep,
                      (void**)&o_vehicleArrivalStep, kVehArrivalStepExpect,
                      sizeof(kVehArrivalStepExpect), "republicmp vehicle arrival step");
    InstallInlineHook(g_exeBase + RVA_IS_VEHICLE_AT_DEPOT_OR_STOP, (void*)&HookedIsVehicleAtDepotOrStop,
                      &o_isVehicleAtDepotOrStop, kIsVehicleAtDepotOrStopExpect,
                      sizeof(kIsVehicleAtDepotOrStopExpect), "republicmp is_vehicle_at_depot");
    InstallInlineHook(g_exeBase + RVA_UPDATE_SIGNAL_BLOCK, (void*)&HookedUpdateSignalBlock,
                      (void**)&o_updateSignalBlock, kUpdateSignalBlockExpect,
                      sizeof(kUpdateSignalBlockExpect), "republicmp update_signal_block");

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
                              (void*)&DetLineMark, (void*)&DetDepotMark, (void*)&DetPickRay, (void*)&DetCheatKeys,
                              (void*)&HeldFrame_DrawStepVehicles, (void*)&DetVehHighlightMain, (void*)&DetVehHighlightExtra,
                              (void*)&DetHoverPick };
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
    Commands_InstallHooks(); // remote command sound/popup isolation & 2-SSR notification filter

    PatchIat(g_exe, DLL_ENGINE, "?SaveOriginalValues@C3D_MATERIAL@@QEAAXXZ",
             (void*)&H_C3dSaveOriginalValues, (void**)&o_c3dSaveOriginalValues, "c3d mat save");
    PatchIat(g_exe, DLL_ENGINE, "?ChangeValuesTo@C3D_MATERIAL@@QEAAXVC3DVECTOR4@@@Z",
             (void*)&H_C3dChangeValuesTo, (void**)&o_c3dChangeValuesTo, "c3d mat change");
    PatchIat(g_exe, DLL_ENGINE, "?RestoreOriginalValues@C3D_MATERIAL@@QEAAXXZ",
             (void*)&H_C3dRestoreOriginalValues, (void**)&o_c3dRestoreOriginalValues, "c3d mat restore");

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
