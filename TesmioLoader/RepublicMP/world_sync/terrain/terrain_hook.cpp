// SPDX-License-Identifier: GPL-3.0-only
// See terrain_hook.h. Every hook records the call, then calls the original
// engine function (the IAT value from before the patch) with the arguments
// unchanged - except a synced edit that terrain_sync asks to swallow (a
// remote road/building replay's own terrain work, see terrain_sync.h).
// Recording never calls a hooked function and never lets an exception
// escape into the game.
#include "terrain_hook.h"
#include "../../lockstep/commands.h"
#include "../networks/road_sync.h"
#include <vector>
#include "terrain_sync.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#define WORLD_THIS_RVA 0x9D4F10
#define TERRAIN_HOLDER_RVA 0x9941F0 // *(exe+0x9941F0) + 0xED8 = C3D_TERRAIN*
#define TERRAIN_HOLDER_OFFSET 0xED8
#define ACTIVE_TOOL_OFFSET 0xD428 // *(char**)(thisPtr+0xD428) = active tool name (FUN_14030d1a0 strcmp's it)

volatile long g_terrainApplyingRemote = 0;

void TerrainHook_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ------------------------------------------------------------------ methods

static const char* const kMethodNames[TM_COUNT] = {
    "EditRaiseLower",
    "EditRaiseLowerAngleLimit",
    "EditRaiseLowerAngleLimitGetCost",
    "EditSmooth",
    "EditLevel",
    "EditToHeight",
    "EditSmoothWithLimits",
    "EditMask",
    "EditMaskGen",
    "EditMaskGenClear",
    "HeightmapTextureOpen",
    "HeightmapTextureClose",
    "MaskTextureOpen",
    "MaskTextureClose",
    "RegenerateCollisionDataSoviet",
    "RegenerateCollisionNormalDataSoviet",
    "SetTerrainHeight",
};

// Every call site of these imports in SOVIET64.exe (2026-09-07 build), by
// return address - found by scanning .text for `call [rip+IAT]`. A caller not
// in this table is logged as "?" and is new information.
struct KnownCaller {
    unsigned int retRva;
    const char* label;
};
static const KnownCaller kKnownCallers[] = {
    // FUN_1403a8a70 - game terrain tool (mode 0 raise, 1 lower, 2 level, 3 level2, 4 level3)
    {0x3AAE5E, "tool.raise(a8a70)"},
    {0x3AAF86, "tool.lower(a8a70)"},
    {0x3AB188, "tool.level2-3a(a8a70)"},
    {0x3AB212, "tool.level2-3b(a8a70)"},
    {0x3AB33C, "tool.level3(a8a70)"},
    {0x3A9A28, "tool.cost-a(a8a70)"},
    {0x3A9AFB, "tool.cost-b(a8a70)"},
    {0x3A8C46, "tool.open(a8a70)"},
    {0x3A8C5A, "tool.open(a8a70)"},
    {0x3AB3B6, "tool.close(a8a70)"},
    {0x3AB3CA, "tool.close(a8a70)"},
    // FUN_140234a50 - map editor tool
    {0x234D34, "editor.raiselower(234a50)"},
    {0x234DDC, "editor.level(234a50)"},
    {0x234E84, "editor.smooth(234a50)"},
    {0x234F65, "editor.toheight(234a50)"},
    {0x235560, "editor?(235370)"},
    {0x235748, "editor?(235580)"},
    // FUN_1402bade0 - building placement
    {0x2CB402, "building.level-click(bade0)"},
    {0x2CBB47, "building.level-paid(bade0)"},
    {0x2CB2EB, "building.open(bade0)"},
    {0x2CB423, "building.close(bade0)"},
    {0x2CB9F3, "building.open(bade0)"},
    {0x2CBCD6, "building.close(bade0)"},
    {0x2C89E6, "building.mask-open(bade0)"},
    {0x2C8C60, "building.mask-close(bade0)"},
    {0x2C925A, "building.mask-open(bade0)"},
    {0x2CAF68, "building.mask-close(bade0)"},
    // FUN_1404fcaf0 - drag tick / FUN_140518a80 - drag commit
    {0x517692, "drag.tick-a(fcaf0)"},
    {0x51772F, "drag.tick-b(fcaf0)"},
    {0x5173ED, "drag.open(fcaf0)"},
    {0x5179E0, "drag.close(fcaf0)"},
    {0x516D38, "drag.mask-open(fcaf0)"},
    {0x516E24, "drag.mask-close(fcaf0)"},
    {0x519B34, "commit(a80)"},
    {0x5199C6, "commit.open(a80)"},
    {0x519B78, "commit.close(a80)"},
    // FUN_14040ece0 - per-frame drain of the mask-paint queue thisPtr+0x130d0
    // (filled by FUN_14040eaa0 next to its particle spawn)
    {0x40EE41, "maskqueue(40ece0)"},
    // FUN_1403ccd70 - terrain_restore_topsoil
    {0x3CCF92, "topsoil.open(ccd70)"},
    {0x3CD829, "topsoil.close(ccd70)"},
    {0x3CCFA6, "topsoil.mask-open(ccd70)"},
    {0x3CD83D, "topsoil.mask-close(ccd70)"},
    // Open/Close without any Edit* in the same function - possible direct
    // texture writes that the Edit* hooks cannot see
    {0x12289D, "direct?.mask-open(1226a0)"},
    {0x123936, "direct?.mask-close(1226a0)"},
    {0x1DD526, "direct?.mask-open(1dd200)"},
    {0x1DDE95, "direct?.mask-close(1dd200)"},
    {0x241EC6, "direct?.mask-open(2412f0)"},
    {0x241EDA, "direct?.open(2412f0)"},
    {0x241EF6, "direct?.mask-close(2412f0)"},
    {0x241F0A, "direct?.close(2412f0)"},
    {0x4E7527, "direct?.mask-open(4e7440)"},
    {0x4E753B, "direct?.open(4e7440)"},
    {0x4EF198, "direct?.mask-close(4e7440)"},
    {0x4EF1AC, "direct?.close(4e7440)"},
    {0x314BE6, "dispatcher.mask-open(30d1a0)"},
    {0x314CB2, "dispatcher.mask-close(30d1a0)"},
    // map generation / load
    {0x22FF4B, "mapgen(22eea0)"},
    {0x230338, "mapgen(22eea0)"},
    {0x2300F0, "mapgen(22eea0)"},
    {0x2304D0, "mapgen(22eea0)"},
    {0x244FF5, "smoothlimits(2436b0)"},
    {0x22D7EA, "regen(22d340)"},
    {0x22D7FE, "regen(22d340)"},
    {0x23AC1F, "regen(239b90)"},
    {0x23AC33, "regen(239b90)"},
    {0x24C583, "load(2479a0)"},
    {0x24C597, "load(2479a0)"},
    {0x24C47F, "load(2479a0)"},
    {0x31E34C, "load(317db0)"},
    {0x31E360, "load(317db0)"},
    {0x22EC5A, "setheight(22e1c0)"},
};

static const char* CallerLabel(unsigned long long rva)
{
    for (size_t i = 0; i < sizeof(kKnownCallers) / sizeof(kKnownCallers[0]); i++)
        if (kKnownCallers[i].retRva == rva) return kKnownCallers[i].label;
    return "?";
}

// ------------------------------------------------------------------ records

// One (method, caller) bucket of the current frame.
struct AggSlot {
    int method;
    unsigned long long caller;
    unsigned count;
    unsigned long tid;
    long depthHeight; // Heightmap Open/Close nesting at the first call
    long depthMask;
    long remote;      // g_terrainApplyingRemote at the first call
    TerrainArgs first;
    TerrainArgs last;
};

#define AGG_MAX_SLOTS 64

static SRWLOCK g_aggLock = SRWLOCK_INIT;
static AggSlot g_slots[AGG_MAX_SLOTS];
static int g_slotCount = 0;
static unsigned long long g_frameAtSlots = 0;

static volatile long long g_seq = 0;
static volatile long long g_frame = 0;
static volatile long g_depthHeight = 0;
static volatile long g_depthMask = 0;
// 0 = off (default: a busy brush logs several lines a frame on both machines),
// 1 = aggregated per frame, 2 = one line per call. F8 cycles.
static volatile long g_logMode = 0;
static LARGE_INTEGER g_qpcStart;
static LARGE_INTEGER g_qpcFreq;

static double NowMs()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_qpcStart.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

static unsigned long long CallerRva(void* returnAddress)
{
    BYTE* p = (BYTE*)returnAddress;
    if (p >= g_exeBase && p < g_exeBase + g_exeSize) return (unsigned long long)(p - g_exeBase);
    return (unsigned long long)p; // outside the exe: absolute address
}

// Formats the method's arguments. Reads only memory we were handed.
static int FormatArgs(char* out, size_t n, const TerrainArgs& a)
{
    int o = 0;
    out[0] = 0;
    if (a.suppressed)
        o += _snprintf_s(out + o, n - o, _TRUNCATE, "SUPPRESSED ");
    if (a.hasPos)
        o += _snprintf_s(out + o, n - o, _TRUNCATE, "pos=(%.3f,%.3f,%.3f) ", a.pos[0], a.pos[1], a.pos[2]);
    if (a.hasPos2)
        o += _snprintf_s(out + o, n - o, _TRUNCATE, "pos2=(%.3f,%.3f,%.3f) ", a.pos2[0], a.pos2[1], a.pos2[2]);
    if (a.ni) {
        o += _snprintf_s(out + o, n - o, _TRUNCATE, "i=[");
        for (int k = 0; k < a.ni; k++)
            o += _snprintf_s(out + o, n - o, _TRUNCATE, k ? ",%d" : "%d", a.i[k]);
        o += _snprintf_s(out + o, n - o, _TRUNCATE, "] ");
    }
    if (a.nf) {
        o += _snprintf_s(out + o, n - o, _TRUNCATE, "f=[");
        for (int k = 0; k < a.nf; k++)
            o += _snprintf_s(out + o, n - o, _TRUNCATE, k ? ",%.5g" : "%.5g", a.f[k]);
        o += _snprintf_s(out + o, n - o, _TRUNCATE, "] ");
    }
    if (a.hasB) o += _snprintf_s(out + o, n - o, _TRUNCATE, "b=%d ", a.b);
    if (a.hasRet) o += _snprintf_s(out + o, n - o, _TRUNCATE, "ret=%.5g ", a.ret);
    return o < 0 ? 0 : o;
}

static void ActiveToolName(char* out, size_t n)
{
    out[0] = 0;
    BYTE* thisPtr = g_exeBase + WORLD_THIS_RVA;
    void* tool = *(void**)(thisPtr + ACTIVE_TOOL_OFFSET);
    if (!SafeReadStr(tool, out, n)) _snprintf_s(out, n, _TRUNCATE, "-");
}

// Logs and empties the aggregate table. Caller holds g_aggLock.
static void FlushLocked()
{
    if (g_slotCount == 0) return;
    __try {
        char tool[64];
        ActiveToolName(tool, sizeof(tool));
        Logf("republicmp  TERRAIN frame=%llu t=%.1fms tool=%s buckets=%d",
             g_frameAtSlots, NowMs(), tool, g_slotCount);
        for (int k = 0; k < g_slotCount; k++) {
            const AggSlot& s = g_slots[k];
            char first[400], last[400];
            FormatArgs(first, sizeof(first), s.first);
            if (s.count > 1) FormatArgs(last, sizeof(last), s.last);
            else last[0] = 0;
            Logf("republicmp  TERRAIN   %-31s x%-4u caller=0x%llX %-28s tid=%lu depth(h=%ld m=%ld) remote=%ld first{%s}%s%s%s",
                 kMethodNames[s.method], s.count, s.caller, CallerLabel(s.caller), s.tid,
                 s.depthHeight, s.depthMask, s.remote, first,
                 s.count > 1 ? " last{" : "", last, s.count > 1 ? "}" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Logging must never take the game down.
    }
    g_slotCount = 0;
}

static void Record(int method, void* returnAddress, const TerrainArgs& args)
{
    __try {
        if (g_logMode == 0) return;
        unsigned long long caller = CallerRva(returnAddress);
        long long seq = InterlockedIncrement64(&g_seq);
        unsigned long long frame = (unsigned long long)g_frame;

        if (g_logMode == 2) {
            char buf[400];
            FormatArgs(buf, sizeof(buf), args);
            Logf("republicmp  TERRAIN #%lld frame=%llu t=%.2fms %s caller=0x%llX %s tid=%lu depth(h=%ld m=%ld) remote=%ld %s",
                 seq, frame, NowMs(), kMethodNames[method], caller, CallerLabel(caller),
                 GetCurrentThreadId(), g_depthHeight, g_depthMask, g_terrainApplyingRemote, buf);
            return;
        }

        AcquireSRWLockExclusive(&g_aggLock);
        if (g_slotCount > 0 && g_frameAtSlots != frame) FlushLocked();
        g_frameAtSlots = frame;

        AggSlot* slot = nullptr;
        for (int k = 0; k < g_slotCount; k++)
            if (g_slots[k].method == method && g_slots[k].caller == caller &&
                g_slots[k].first.suppressed == args.suppressed) { slot = &g_slots[k]; break; }
        if (!slot) {
            if (g_slotCount == AGG_MAX_SLOTS) FlushLocked();
            slot = &g_slots[g_slotCount++];
            slot->method = method;
            slot->caller = caller;
            slot->count = 0;
            slot->tid = GetCurrentThreadId();
            slot->depthHeight = g_depthHeight;
            slot->depthMask = g_depthMask;
            slot->remote = g_terrainApplyingRemote;
            slot->first = args;
        }
        slot->count++;
        slot->last = args;
        ReleaseSRWLockExclusive(&g_aggLock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A fault while the lock is held would leave it held; there is no
        // code in the locked region that can fault on game memory.
    }
}

static void CopyPos(float* dst, const float* src)
{
    __try {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = dst[1] = dst[2] = -1.0f;
    }
}

// ------------------------------------------------------------------ hooks

typedef void (*RaiseLowerFn)(void*, const float*, float, float, float, float, bool);
typedef void (*RaiseLowerAngleFn)(void*, const float*, float, float, float, float, bool, float);
typedef float (*RaiseLowerAngleCostFn)(void*, const float*, float, float, float, float, bool);
typedef void (*Vec3F3BoolFn)(void*, const float*, float, float, float, bool); // EditSmooth, EditLevel
typedef void (*ToHeightFn)(void*, const float*, const float*, float, float, float, bool);
typedef void (*SmoothLimitsFn)(void*, const float*, float, float, float, float, float, float, float, float, float);
typedef void (*MaskFn)(void*, const float*, int, float, float, int, int, bool);
typedef void (*MaskGenFn)(void*, int, float, float, float, float, float, float);
typedef void (*MaskGenClearFn)(void*, int);
typedef void (*VoidFn)(void*);
typedef void (*SetHeightFn)(void*, float);

static RaiseLowerFn          o_RaiseLower;
static RaiseLowerAngleFn     o_RaiseLowerAngle;
static RaiseLowerAngleCostFn o_RaiseLowerAngleCost;
static Vec3F3BoolFn          o_Smooth;
static Vec3F3BoolFn          o_Level;
static ToHeightFn            o_ToHeight;
static SmoothLimitsFn        o_SmoothLimits;
static MaskFn                o_Mask;
static MaskGenFn             o_MaskGen;
static MaskGenClearFn        o_MaskGenClear;
static VoidFn                o_HeightmapOpen;
static VoidFn                o_HeightmapClose;
static VoidFn                o_MaskOpen;
static VoidFn                o_MaskClose;
static VoidFn                o_RegenCollision;
static VoidFn                o_RegenCollisionNormal;
static SetHeightFn           o_SetTerrainHeight;

// Synced edits go through BeginEdit/EndEdit. BeginEdit logs the call and
// returns false when terrain_sync wants it swallowed; EndEdit hands an edit
// that really ran locally to terrain_sync. Remote replays (g_terrainApplyingRemote)
// always run and are never captured.
struct LsTerrainEdit {
    int method;
    TerrainArgs args;
};
static std::vector<LsTerrainEdit> g_lsTerrain; // main thread (the tools run in the Late body)

static bool BeginEdit(int method, void* returnAddress, TerrainArgs& x)
{
    bool remote = g_terrainApplyingRemote != 0 || RoadSync_IsApplyingSegment();
    if (!remote && Commands_TakeLocal()) {
        g_lsTerrain.push_back({ method, x });
        x.suppressed = 1;
        Record(method, returnAddress, x);
        return false;
    }
    x.suppressed = (!remote && TerrainSync_SuppressLocalEdits()) ? 1 : 0;
    Record(method, returnAddress, x);
    return !x.suppressed;
}

static void EndEdit(int method, const TerrainArgs& x)
{
    if (g_terrainApplyingRemote == 0 && !RoadSync_IsApplyingSegment()) TerrainSync_CaptureEdit(method, x);
}

static void H_RaiseLower(void* t, const float* pos, float a, float b, float c, float d, bool e)
{
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.f[3] = d; x.nf = 4;
    x.b = e; x.hasB = 1;
    if (!BeginEdit(TM_RAISE_LOWER, _ReturnAddress(), x)) return;
    o_RaiseLower(t, pos, a, b, c, d, e);
    EndEdit(TM_RAISE_LOWER, x);
}

static void H_RaiseLowerAngle(void* t, const float* pos, float a, float b, float c, float d, bool e, float g)
{
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.f[3] = d; x.f[4] = g; x.nf = 5; // f[4] is the arg after the bool
    x.b = e; x.hasB = 1;
    if (!BeginEdit(TM_RAISE_LOWER_ANGLE, _ReturnAddress(), x)) return;
    o_RaiseLowerAngle(t, pos, a, b, c, d, e, g);
    EndEdit(TM_RAISE_LOWER_ANGLE, x);
}

static float H_RaiseLowerAngleCost(void* t, const float* pos, float a, float b, float c, float d, bool e)
{
    void* ret = _ReturnAddress();
    float cost = o_RaiseLowerAngleCost(t, pos, a, b, c, d, e);
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.f[3] = d; x.nf = 4;
    x.b = e; x.hasB = 1;
    x.ret = cost; x.hasRet = 1;
    Record(TM_RAISE_LOWER_ANGLE_COST, ret, x);
    return cost;
}

static void H_Smooth(void* t, const float* pos, float a, float b, float c, bool e)
{
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.nf = 3;
    x.b = e; x.hasB = 1;
    if (!BeginEdit(TM_SMOOTH, _ReturnAddress(), x)) return;
    o_Smooth(t, pos, a, b, c, e);
    EndEdit(TM_SMOOTH, x);
}

static void H_Level(void* t, const float* pos, float a, float b, float c, bool e)
{
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.nf = 3;
    x.b = e; x.hasB = 1;
    if (!BeginEdit(TM_LEVEL, _ReturnAddress(), x)) return;
    o_Level(t, pos, a, b, c, e);
    EndEdit(TM_LEVEL, x);
}

static void H_ToHeight(void* t, const float* pos, const float* pos2, float a, float b, float c, bool e)
{
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    CopyPos(x.pos2, pos2); x.hasPos2 = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.nf = 3;
    x.b = e; x.hasB = 1;
    if (!BeginEdit(TM_TO_HEIGHT, _ReturnAddress(), x)) return;
    o_ToHeight(t, pos, pos2, a, b, c, e);
    EndEdit(TM_TO_HEIGHT, x);
}

static void H_SmoothLimits(void* t, const float* pos, float a, float b, float c, float d, float e,
                           float f, float g, float h, float k)
{
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.f[3] = d; x.f[4] = e;
    x.f[5] = f; x.f[6] = g; x.f[7] = h; x.f[8] = k; x.nf = 9;
    if (!BeginEdit(TM_SMOOTH_LIMITS, _ReturnAddress(), x)) return;
    o_SmoothLimits(t, pos, a, b, c, d, e, f, g, h, k);
    EndEdit(TM_SMOOTH_LIMITS, x);
}

// i[0] = channel, i[1], i[2] = the two ints after the floats.
static void H_Mask(void* t, const float* pos, int channel, float a, float b, int i1, int i2, bool e)
{
    TerrainArgs x = {};
    CopyPos(x.pos, pos); x.hasPos = 1;
    x.i[0] = channel; x.i[1] = i1; x.i[2] = i2; x.ni = 3;
    x.f[0] = a; x.f[1] = b; x.nf = 2;
    x.b = e; x.hasB = 1;
    if (!BeginEdit(TM_MASK, _ReturnAddress(), x)) return;
    o_Mask(t, pos, channel, a, b, i1, i2, e);
    EndEdit(TM_MASK, x);
}

static void H_MaskGen(void* t, int ch, float a, float b, float c, float d, float e, float f)
{
    TerrainArgs x = {};
    x.i[0] = ch; x.ni = 1;
    x.f[0] = a; x.f[1] = b; x.f[2] = c; x.f[3] = d; x.f[4] = e; x.f[5] = f; x.nf = 6;
    Record(TM_MASK_GEN, _ReturnAddress(), x);
    o_MaskGen(t, ch, a, b, c, d, e, f);
}

static void H_MaskGenClear(void* t, int ch)
{
    TerrainArgs x = {};
    x.i[0] = ch; x.ni = 1;
    Record(TM_MASK_GEN_CLEAR, _ReturnAddress(), x);
    o_MaskGenClear(t, ch);
}

// Open/Close: the depth recorded is the one AFTER this call took effect for
// Open and BEFORE it for Close, so a matched pair logs the same number.
static void H_HeightmapOpen(void* t)
{
    InterlockedIncrement(&g_depthHeight);
    TerrainArgs x = {};
    Record(TM_HEIGHTMAP_OPEN, _ReturnAddress(), x);
    o_HeightmapOpen(t);
}

static void H_HeightmapClose(void* t)
{
    TerrainArgs x = {};
    Record(TM_HEIGHTMAP_CLOSE, _ReturnAddress(), x);
    o_HeightmapClose(t);
    InterlockedDecrement(&g_depthHeight);
}

static void H_MaskOpen(void* t)
{
    InterlockedIncrement(&g_depthMask);
    TerrainArgs x = {};
    Record(TM_MASK_OPEN, _ReturnAddress(), x);
    o_MaskOpen(t);
}

static void H_MaskClose(void* t)
{
    TerrainArgs x = {};
    Record(TM_MASK_CLOSE, _ReturnAddress(), x);
    o_MaskClose(t);
    InterlockedDecrement(&g_depthMask);
}

static void H_RegenCollision(void* t)
{
    TerrainArgs x = {};
    Record(TM_REGEN_COLLISION, _ReturnAddress(), x);
    o_RegenCollision(t);
}

static void H_RegenCollisionNormal(void* t)
{
    TerrainArgs x = {};
    Record(TM_REGEN_COLLISION_NORMAL, _ReturnAddress(), x);
    o_RegenCollisionNormal(t);
}

static void H_SetTerrainHeight(void* t, float h)
{
    TerrainArgs x = {};
    x.f[0] = h; x.nf = 1;
    Record(TM_SET_TERRAIN_HEIGHT, _ReturnAddress(), x);
    o_SetTerrainHeight(t, h);
}

// ------------------------------------------------------------------ replay

void* TerrainHook_GetTerrain()
{
    __try {
        BYTE* holder = *(BYTE**)(g_exeBase + TERRAIN_HOLDER_RVA);
        if (!holder) return nullptr;
        return *(void**)(holder + TERRAIN_HOLDER_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// A by-value C3DVECTOR3 is passed as a pointer to a copy the callee owns and
// may modify, so every call gets fresh local copies.
// Always pass false for the trailing `bool close` parameter: C3D_TERRAIN::Edit*
// calls TextureAccessClose() when `close == true`, which unmaps the D3D11 staging
// buffer ([terrain + 0x158] / [terrain + 0x168]) mid-batch while subsequent edits
// in the same batch still write to it.
bool TerrainHook_ApplyEdit(void* t, int method, const TerrainArgs& a)
{
    float p[3] = { a.pos[0], a.pos[1], a.pos[2] };
    float p2[3] = { a.pos2[0], a.pos2[1], a.pos2[2] };
    const float* f = a.f;
    switch (method) {
    case TM_RAISE_LOWER:       o_RaiseLower(t, p, f[0], f[1], f[2], f[3], false); return true;
    case TM_RAISE_LOWER_ANGLE: o_RaiseLowerAngle(t, p, f[0], f[1], f[2], f[3], false, f[4]); return true;
    case TM_SMOOTH:            o_Smooth(t, p, f[0], f[1], f[2], false); return true;
    case TM_LEVEL:             o_Level(t, p, f[0], f[1], f[2], false); return true;
    case TM_TO_HEIGHT:         o_ToHeight(t, p, p2, f[0], f[1], f[2], false); return true;
    case TM_SMOOTH_LIMITS:     o_SmoothLimits(t, p, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]); return true;
    case TM_MASK:              o_Mask(t, p, a.i[0], f[0], f[1], a.i[1], a.i[2], false); return true;
    default:                   return false;
    }
}

void TerrainHook_HeightmapOpen(void* t)  { o_HeightmapOpen(t); }
void TerrainHook_HeightmapClose(void* t) { o_HeightmapClose(t); }
void TerrainHook_MaskOpen(void* t)       { o_MaskOpen(t); }
void TerrainHook_MaskClose(void* t)      { o_MaskClose(t); }

typedef float (*HeightOverPointFn)(void*, const float*);
static HeightOverPointFn g_heightOverPoint = nullptr;

void TerrainHook_LockstepFlush()
{
    if (g_lsTerrain.empty()) return;
    Commands_Submit(LS_CMD_TERRAIN, g_lsTerrain.data(), g_lsTerrain.size() * sizeof(LsTerrainEdit));
    g_lsTerrain.clear();
}

typedef void (*ClearGroundObjectsFn)(void* world, const float* pos, float radius, char flag);

bool TerrainHook_ApplyCommand(const void* data, size_t size)
{
    if (size % sizeof(LsTerrainEdit) != 0) return false;
    const LsTerrainEdit* e = (const LsTerrainEdit*)data;
    size_t n = size / sizeof(LsTerrainEdit);
    void* terrain = TerrainHook_GetTerrain();
    if (!terrain || !n) return false;
    InterlockedIncrement(&g_terrainApplyingRemote);
    bool heightOpen = false;
    bool maskOpen = false;
    ClearGroundObjectsFn clearGround = (ClearGroundObjectsFn)(g_exeBase + 0x3AE120);
    void* world = (void*)(g_exeBase + 0x9D4F10);
    for (size_t i = 0; i < n; i++) {
        int m = e[i].method;
        if (m == TM_SMOOTH_LIMITS) {
            // C3D_TERRAIN::EditSmoothWithLimits opens and closes the heightmap staging buffer internally.
            if (heightOpen) {
                TerrainHook_HeightmapClose(terrain);
                heightOpen = false;
            }
            TerrainHook_ApplyEdit(terrain, m, e[i].args);
            continue;
        }
        bool wantHeight = (m != TM_MASK);
        bool wantMask = (m == TM_MASK || m == TM_RAISE_LOWER_ANGLE);
        if (wantHeight && !heightOpen) {
            TerrainHook_HeightmapOpen(terrain);
            heightOpen = true;
        }
        if (wantMask && !maskOpen) {
            TerrainHook_MaskOpen(terrain);
            maskOpen = true;
        }
        if (m == TM_RAISE_LOWER && e[i].args.f[1] > 0.0f) {
            float p[3] = { e[i].args.pos[0], e[i].args.pos[1], e[i].args.pos[2] };
            clearGround(world, p, e[i].args.f[1], 0);
        }
        TerrainHook_ApplyEdit(terrain, m, e[i].args);
    }
    if (maskOpen) TerrainHook_MaskClose(terrain);
    if (heightOpen) TerrainHook_HeightmapClose(terrain);
    InterlockedDecrement(&g_terrainApplyingRemote);
    return true;
}

bool TerrainHook_HeightAt(const float pos[3], float* outHeight)
{
    void* t = TerrainHook_GetTerrain();
    if (!t || !g_heightOverPoint) return false;
    float p[3] = { pos[0], pos[1], pos[2] };
    __try {
        *outHeight = g_heightOverPoint(t, p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ------------------------------------------------------------------ install

struct TerrainHookDef {
    const char* symbol;
    void* detour;
    void** original;
    const char* label;
};

bool InstallTerrainHooks()
{
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);

    // Not hooked - only used to compare heights after a remote batch.
    g_heightOverPoint = (HeightOverPointFn)GetProcAddress(
        g_engine ? g_engine : GetModuleHandleA(DLL_ENGINE),"?CollisionTerrainHeightOverPoint@C3D_TERRAIN@@QEAAMVC3DVECTOR3@@@Z");
    Logf("republicmp  TERRAIN: CollisionTerrainHeightOverPoint %s", g_heightOverPoint ? "found" : "NOT FOUND (no height check)");

    const TerrainHookDef defs[] = {
        {"?EditRaiseLower@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@MMMM_N@Z",
         (void*)H_RaiseLower, (void**)&o_RaiseLower, "T.EditRaiseLower"},
        {"?EditRaiseLowerAngleLimit@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@MMMM_NM@Z",
         (void*)H_RaiseLowerAngle, (void**)&o_RaiseLowerAngle, "T.EditRaiseLowerAngle"},
        {"?EditRaiseLowerAngleLimitGetCost@C3D_TERRAIN@@QEAAMVC3DVECTOR3@@MMMM_N@Z",
         (void*)H_RaiseLowerAngleCost, (void**)&o_RaiseLowerAngleCost, "T.EditRaiseLowerCost"},
        {"?EditSmooth@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@MMM_N@Z",
         (void*)H_Smooth, (void**)&o_Smooth, "T.EditSmooth"},
        {"?EditLevel@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@MMM_N@Z",
         (void*)H_Level, (void**)&o_Level, "T.EditLevel"},
        {"?EditToHeight@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@0MMM_N@Z",
         (void*)H_ToHeight, (void**)&o_ToHeight, "T.EditToHeight"},
        {"?EditSmoothWithLimits@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@MMMMMMMMM@Z",
         (void*)H_SmoothLimits, (void**)&o_SmoothLimits, "T.EditSmoothWithLimits"},
        {"?EditMask@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@HMMHH_N@Z",
         (void*)H_Mask, (void**)&o_Mask, "T.EditMask"},
        {"?EditMaskGen@C3D_TERRAIN@@QEAAXHMMMMMM@Z",
         (void*)H_MaskGen, (void**)&o_MaskGen, "T.EditMaskGen"},
        {"?EditMaskGenClear@C3D_TERRAIN@@QEAAXH@Z",
         (void*)H_MaskGenClear, (void**)&o_MaskGenClear, "T.EditMaskGenClear"},
        {"?HeightmapTextureOpen@C3D_TERRAIN@@QEAAXXZ",
         (void*)H_HeightmapOpen, (void**)&o_HeightmapOpen, "T.HeightmapOpen"},
        {"?HeightmapTextureClose@C3D_TERRAIN@@QEAAXXZ",
         (void*)H_HeightmapClose, (void**)&o_HeightmapClose, "T.HeightmapClose"},
        {"?MaskTextureOpen@C3D_TERRAIN@@QEAAXXZ",
         (void*)H_MaskOpen, (void**)&o_MaskOpen, "T.MaskOpen"},
        {"?MaskTextureClose@C3D_TERRAIN@@QEAAXXZ",
         (void*)H_MaskClose, (void**)&o_MaskClose, "T.MaskClose"},
        {"?RegenerateCollisionDataSoviet@C3D_TERRAIN@@QEAAXXZ",
         (void*)H_RegenCollision, (void**)&o_RegenCollision, "T.RegenCollision"},
        {"?RegenerateCollisionNormalDataSoviet@C3D_TERRAIN@@QEAAXXZ",
         (void*)H_RegenCollisionNormal, (void**)&o_RegenCollisionNormal, "T.RegenCollisionNormal"},
        {"?SetTerrainHeight@C3D_TERRAIN@@QEAAXM@Z",
         (void*)H_SetTerrainHeight, (void**)&o_SetTerrainHeight, "T.SetTerrainHeight"},
    };

    int ok = 0;
    const int total = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int k = 0; k < total; k++)
        if (PatchIat(g_exe, DLL_ENGINE, defs[k].symbol, defs[k].detour, defs[k].original, defs[k].label))
            ok++;

    Logf("republicmp  TERRAIN: %d/%d hooks installed (log off; the Debug window sets it)", ok, total);
    return ok == total;
}

// ------------------------------------------------------------------ tick

void TerrainHook_SetLogMode(int mode)
{
    long v = mode < 0 || mode > 2 ? 0 : mode;
    InterlockedExchange(&g_logMode, v);
    Logf("republicmp  TERRAIN: log %s", v == 0 ? "OFF" : v == 1 ? "aggregated per frame" : "verbose (one line per call)");
}

int TerrainHook_GetLogMode() { return (int)g_logMode; }

void TerrainHook_Tick()
{
    AcquireSRWLockExclusive(&g_aggLock);
    FlushLocked();
    ReleaseSRWLockExclusive(&g_aggLock);
    InterlockedIncrement64(&g_frame);
}
