// SPDX-License-Identifier: GPL-3.0-only
// See session_patches.h.
#include "session_patches.h"
#include "../core/safe_mem.h"
#include "../session/save_sync.h"                 // SaveSync_IsBusy
#include "../world_sync/buildings/build_sync.h" // BuildSync_IsConnected
#include "../determinism/determinism.h"        // Determinism_IsActive (the probe)
#include <string.h>
#include <limits.h>

struct SessionPatch {
    unsigned int rva;
    unsigned char size;
    unsigned char original[10];
    unsigned char patch[10];
    const char* what;
};

#define CITIZEN_INIT_RVA 0x8244f0    // FUN_1408244f0(owner, citizen): sets a citizen's +18 if not above 0
#define CITIZEN_INIT_GATED_CALL 0x2fd517 // its call in FUN_1402fd2e0 (Late body and interface)

// The last entries' patches (relays) are filled in at Apply.
static SessionPatch kPatches[] = {
    // jne +15 -> 2-byte nop: always slow the timer by MultiplyFPS(1/3)
    { 0x13cb5b, 2, { 0x75, 0x15 }, { 0x66, 0x90 }, "building pass: frame-rate test, timer slowdown" },
    // jne +3a -> nop: always update only this pass's third of the buildings
    { 0x13dbc1, 2, { 0x75, 0x3a }, { 0x66, 0x90 }, "building pass: frame-rate test, building third" },
    // jne +15 -> nop: always restore the timer at the end
    { 0x13e594, 2, { 0x75, 0x15 }, { 0x66, 0x90 }, "building pass: frame-rate test, timer restore" },
    // movzx ecx,byte [rax+178] -> xor ecx,ecx; nop5
    { 0x13d963, 7, { 0x0f, 0xb6, 0x88, 0x78, 0x01, 0x00, 0x00 }, { 0x31, 0xc9, 0x0f, 0x1f, 0x44, 0x00, 0x00 },
      "construction site: camera visibility" },
    // Citizen init called from interface code -> nop5
    { 0x4a6aa6, 5, { 0xe8, 0x45, 0xda, 0x37, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: building overlay" },
    { 0x7645ae, 5, { 0xe8, 0x3d, 0xff, 0x0b, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: window 761300" },
    { 0x7bb8b3, 5, { 0xe8, 0x38, 0x8c, 0x06, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: window 7bada0" },
    { 0x8243c2, 5, { 0xe8, 0x29, 0x01, 0x00, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: getter 8243b0" },
    // Background window / Alt-Tab responsiveness: eliminate 1600ms/1000ms WndProc Sleep() on focus loss,
    // never minimize the window (ShowWindow SW_MINIMIZE) when losing focus, and never freeze or skip 3D
    // rendering in the main loop when unfocused.
    { 0x10366e, 5, { 0xb9, 0x40, 0x06, 0x00, 0x00 }, { 0xb9, 0x00, 0x00, 0x00, 0x00 }, "WndProc WM_KILLFOCUS: Sleep(1600) -> Sleep(0)" },
    { 0x103690, 2, { 0x74, 0x19 }, { 0x66, 0x90 }, "WndProc WM_KILLFOCUS: never minimize on WINDOW_MINIMIZE" },
    { 0x1036a9, 2, { 0x75, 0x12 }, { 0xeb, 0x12 }, "WndProc WM_KILLFOCUS: never call ShowWindow(SW_MINIMIZE)" },
    { 0x103748, 5, { 0xb9, 0xe8, 0x03, 0x00, 0x00 }, { 0xb9, 0x00, 0x00, 0x00, 0x00 }, "WndProc WM_ACTIVATE: Sleep(1000) -> Sleep(0)" },
    { 0x103771, 6, { 0x0f, 0x84, 0x34, 0xff, 0xff, 0xff }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, "WndProc WM_ACTIVATE: never jump to ShowWindow(SW_MINIMIZE)" },
    { 0x105b94, 6, { 0x0f, 0x85, 0xa4, 0x00, 0x00, 0x00 }, { 0x90, 0xe9, 0xa4, 0x00, 0x00, 0x00 }, "main loop: never freeze when unfocused (WINDOW_PAUSE)" },
    { 0x105c7f, 3, { 0x0f, 0x44, 0xdd }, { 0x90, 0x90, 0x90 }, "main loop: never skip 3D render when unfocused" },
    // Vehicle window (0x7D6B00): route Change Stop '>>' (r14d==5) to 0x7DA085 -> 0x7DA088 (CALL_CHANGE_STOP)
    // and route Ship launch (cat==7, r14d==6) through 0x7DA2D2 -> 0x7DA2F1 (CALL_RELEASE_VEHICLE).
    { 0x7d9fbd, 8, { 0x49, 0x83, 0xbf, 0x28, 0x05, 0x00, 0x00, 0x00 }, { 0x41, 0x8b, 0x94, 0x24, 0x64, 0x02, 0x00, 0x00 },
      "vehicle window: change stop load stopIdx into edx" },
    { 0x7d9fc5, 5, { 0x75, 0x6f, 0x49, 0x83, 0xbf }, { 0xe9, 0xbb, 0x00, 0x00, 0x00 },
      "vehicle window: change stop jump to 0x7DA085 call site" },
    { 0x7da2c4, 2, { 0x74, 0x0c }, { 0xeb, 0x0c },
      "vehicle window: ship launch routes through 0x7DA2F1" },
    // The call shared by Late and the interface -> the gate's relay (simulation thread only)
    { CITIZEN_INIT_GATED_CALL, 5, { 0xe8, 0xd4, 0x6f, 0x52, 0x00 }, { 0 }, "citizen init: Late / interface" },
    // World load HUD widget null guards (prevent AV read at 0x6563 / 0x657A)
    { 0x6563, 9, { 0x48, 0x8b, 0x01, 0xff, 0x90, 0x98, 0x00, 0x00, 0x00 }, { 0 }, "world load widget #1 null guard" },
    { 0x657a, 9, { 0x48, 0x8b, 0x01, 0xff, 0x90, 0x98, 0x00, 0x00, 0x00 }, { 0 }, "world load widget #2 null guard" },
};
#define PATCH_COUNT (sizeof(kPatches) / sizeof(kPatches[0]))

static bool g_applied = false;
static bool g_refused = false; // the bytes didn't match - never tried again

void SessionPatches_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// Citizen init gate
// ---------------------------------------------------------------------------
//
// A new citizen's +18 is set by FUN_1408244f0 the first time someone needs it:
// the citizen update (simulation), but also the interface on the render
// thread (an overlay, two windows, a getter) - so it got its value on
// whichever PC's interface reached it first, at different times. The four
// interface-only calls are nops in a session (they then read the value the
// simulation set, or 0), and the call shared by the Late body and the
// interface goes through a gate that lets only the simulation thread in.
// With deterministic frames it also requires a step: held Late uses the same
// thread but must not initialize a citizen before the next admitted tick.
// (by SoylentGreed)

typedef void (*CitizenInitFn)(void* owner, void* citizen);
static volatile DWORD g_simThreadId = 0;
static BYTE* g_gateRelay = nullptr;

static void CitizenInitGate(void* owner, void* citizen)
{
    // With the layer on, its own step test (the main thread on a step frame):
    // g_simThreadId is only known after the first tick, and a player who
    // joined from the main menu had none yet, so its first tick refused the
    // initializations the host's made.
    bool allowed = Determinism_IsActive() ? Determinism_IsStepFrame() : GetCurrentThreadId() == g_simThreadId;
    if (allowed) ((CitizenInitFn)(g_exeBase + CITIZEN_INIT_RVA))(owner, citizen);
}

static void OnSimStep()
{
    g_simThreadId = GetCurrentThreadId(); // the main thread, which runs Early and Late
}

// The relay near the exe (a rel32 call reaches it). The game's callers (2fd517 in
// 2fd2e0) keep rdx, r9 and r11 across the call, which 8244f0 leaves alone; the gate
// is an ordinary function, so the relay saves every volatile register around it
// (a building window crashed reading a citizen through the clobbered rdx).
static const unsigned char kRelayHead[] = {
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, // push rax rcx rdx r8 r9 r10 r11
    0x48, 0x83, 0xec, 0x20,                                           // sub rsp, 20h
    0x48, 0xb8 };                                                     // mov rax, imm64 (the gate)
static const unsigned char kRelayTail[] = {
    0xff, 0xd0,                                                       // call rax
    0x48, 0x83, 0xc4, 0x20,                                           // add rsp, 20h
    0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58, // pop r11 r10 r9 r8 rdx rcx rax
    0xc3 };                                                           // ret

static void SafeInvokeWidgetMethod98(void* widget)
{
    if (!widget || !IsPointerLikeX64((uint64_t)widget)) return;
    DiagLog_BeginSafeSeh();
    __try {
        void** vtable = *(void***)widget;
        if (vtable && IsPointerLikeX64((uint64_t)vtable)) {
            typedef void (__fastcall* WidgetMethodFn)(void*);
            WidgetMethodFn fn = (WidgetMethodFn)vtable[0x98 / 8];
            if (fn && IsPointerLikeX64((uint64_t)fn)) {
                fn(widget);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  SESSION_PATCHES: caught exception invoking widget method 98h on %p", widget);
    }
    DiagLog_EndSafeSeh();
}

void SessionPatches_TestSafeInvokeWidgetMethod98(void* widget)
{
    SafeInvokeWidgetMethod98(widget);
}

static bool PrepareGate()
{
    if (g_gateRelay) return true;
    static bool s_observing = false;
    if (!s_observing) {
        Determinism_AddStepObserver(OnSimStep);
        s_observing = true;
    }
    BYTE* relay = AllocNear(g_exeBase + CITIZEN_INIT_GATED_CALL, 4096);
    if (!relay) return false;
    long long rel = (long long)relay - (long long)(g_exeBase + CITIZEN_INIT_GATED_CALL + 5);
    if (rel < INT_MIN || rel > INT_MAX) return false;

    long long relWidget1 = (long long)(relay + 64) - (long long)(g_exeBase + 0x6563 + 5);
    long long relWidget2 = (long long)(relay + 64) - (long long)(g_exeBase + 0x657A + 5);
    if (relWidget1 < INT_MIN || relWidget1 > INT_MAX || relWidget2 < INT_MIN || relWidget2 > INT_MAX) return false;

    DWORD old;
    const size_t size = 128;
    if (!VirtualProtect(relay, size, PAGE_EXECUTE_READWRITE, &old)) return false;

    // 1. Citizen init gate at relay + 0
    unsigned long long to = (unsigned long long)&CitizenInitGate;
    memcpy(relay, kRelayHead, sizeof(kRelayHead));
    memcpy(relay + sizeof(kRelayHead), &to, 8);
    memcpy(relay + sizeof(kRelayHead) + 8, kRelayTail, sizeof(kRelayTail));

    // 2. Widget null guard relay at relay + 64
    // sub rsp, 28h; mov rax, &SafeInvokeWidgetMethod98; call rax; add rsp, 28h; ret
    const unsigned char kWidgetHead[] = { 0x48, 0x83, 0xec, 0x28, 0x48, 0xb8 };
    const unsigned char kWidgetTail[] = { 0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3 };
    unsigned long long toWidget = (unsigned long long)&SafeInvokeWidgetMethod98;
    memcpy(relay + 64, kWidgetHead, sizeof(kWidgetHead));
    memcpy(relay + 64 + sizeof(kWidgetHead), &toWidget, 8);
    memcpy(relay + 64 + sizeof(kWidgetHead) + 8, kWidgetTail, sizeof(kWidgetTail));

    VirtualProtect(relay, size, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, size);

    for (size_t i = 0; i < PATCH_COUNT; i++) {
        if (kPatches[i].rva == CITIZEN_INIT_GATED_CALL) {
            int rel32 = (int)rel;
            kPatches[i].patch[0] = 0xe8;
            memcpy(kPatches[i].patch + 1, &rel32, 4);
        } else if (kPatches[i].rva == 0x6563) {
            int rel32 = (int)relWidget1;
            kPatches[i].patch[0] = 0xe8;
            memcpy(kPatches[i].patch + 1, &rel32, 4);
            kPatches[i].patch[5] = 0x0f; kPatches[i].patch[6] = 0x1f; kPatches[i].patch[7] = 0x40; kPatches[i].patch[8] = 0x00;
        } else if (kPatches[i].rva == 0x657A) {
            int rel32 = (int)relWidget2;
            kPatches[i].patch[0] = 0xe8;
            memcpy(kPatches[i].patch + 1, &rel32, 4);
            kPatches[i].patch[5] = 0x0f; kPatches[i].patch[6] = 0x1f; kPatches[i].patch[7] = 0x40; kPatches[i].patch[8] = 0x00;
        }
    }
    g_gateRelay = relay;
    return true;
}

static bool BytesAre(const SessionPatch& p, const unsigned char* bytes)
{
    return SafeBytesMatch(g_exeBase + p.rva, bytes, p.size);
}

static bool Write(const SessionPatch& p, const unsigned char* bytes)
{
    BYTE* at = g_exeBase + p.rva;
    DWORD old;
    if (!VirtualProtect(at, p.size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, p.size);
    VirtualProtect(at, p.size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, p.size);
    return true;
}

static void Apply()
{
    if (!PrepareGate()) {
        Logf("republicmp  SESSION_PATCHES: no relay page near the executable - none applied");
        g_refused = true;
        return;
    }
    for (size_t i = 0; i < PATCH_COUNT; i++) {
        if (!BytesAre(kPatches[i], kPatches[i].original)) {
            Logf("republicmp  SESSION_PATCHES: unexpected bytes at 0x%X (%s) - none applied",
                 kPatches[i].rva, kPatches[i].what);
            g_refused = true;
            return;
        }
    }
    for (size_t i = 0; i < PATCH_COUNT; i++) Write(kPatches[i], kPatches[i].patch);
    g_applied = true;
    Logf("republicmp  SESSION_PATCHES: %d applied (frame-rate tests, construction-site camera, citizen init)", (int)PATCH_COUNT);
}

void SessionPatches_Restore()
{
    for (size_t i = 0; i < PATCH_COUNT; i++)
        if (BytesAre(kPatches[i], kPatches[i].patch)) Write(kPatches[i], kPatches[i].original);
    g_applied = false;
    Logf("republicmp  SESSION_PATCHES: restored");
}


bool SessionPatches_IsApplied()
{
    return g_applied;
}

void SessionPatches_SetAppliedForTest(bool applied)
{
    g_applied = applied;
}

void SessionPatches_Tick()
{
    bool want = IsDualTestbed() || SaveSync_IsBusy() || BuildSync_IsConnected() ||
                Determinism_IsActive() || SaveSync_IsWorldLoadInProgress();
    if (want && !g_applied && !g_refused) Apply();
    else if (!want && g_applied) SessionPatches_Restore();
}
