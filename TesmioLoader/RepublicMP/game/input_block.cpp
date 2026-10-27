// SPDX-License-Identifier: GPL-3.0-only
// See input_block.h.
#include "input_block.h"
#include "../ui/sync_block.h"
#include "../session/save_sync.h"
#include "../core/engine_rva.h"

#define LMB_HELD_RVA 0xA54E90         // byte: left mouse button held
#define LMB_JUST_PRESSED_RVA 0xA54E91 // byte: left mouse button pressed this frame

void InputBlock_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static bool Unfocused()
{
    if (!g_exeBase) return false;
    __try {
        return *(volatile unsigned char*)(g_exeBase + WINDOW_FOCUS_BYTE_RVA) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool Blocked()
{
    return SyncBlock_BlocksWorldInput() || Unfocused();
}

static bool WorldActionBlocked()
{
    return Blocked() || SaveSync_IsWaitingLobbyActive();
}

static volatile LONG g_neutral = 0;
void InputBlock_BeginNeutral() { InterlockedIncrement(&g_neutral); }
void InputBlock_EndNeutral() { InterlockedDecrement(&g_neutral); }
static bool Neutral() { return g_neutral != 0; }
static bool HeldByte() { return *(g_exeBase + LMB_HELD_RVA) != 0; }
static bool PressedByte() { return *(g_exeBase + LMB_JUST_PRESSED_RVA) != 0; }

// Cursor position (GetMouseSolid / GetMouseSolidWithInvZoom) at the moment
// the block started; returned instead of the live one while blocked.
static float g_frozenSolid[3];
static float g_frozenSolidInvZoom[3];
static bool g_haveFrozenSolid = false;
static bool g_haveFrozenSolidInvZoom = false;

// ------------------------------------------------------------------ hooks
// Member functions: this in rcx. A C3DVECTOR3 result is returned through a
// hidden pointer in rdx (checked at GetMouseSolid's call sites), which the
// function also returns in rax.

typedef bool (*BoolNoArgFn)(void*);
typedef bool (*BoolIntFn)(void*, int);
typedef bool (*BoolStrFn)(void*, char*);
typedef float (*AxisValueFn)(void*, char*, int*, float, float);
typedef float* (*VecFn)(void*, float*);

static BoolNoArgFn o_LeftPress, o_LeftPressAOff, o_LeftDown, o_RightPress, o_RightPressAOff,
                   o_RightDown, o_MiddleDown;
static BoolIntFn o_KeyDown, o_KeyPress;
static BoolStrFn o_ButtonDown, o_ButtonPress;
static AxisValueFn o_AxisValue;
static VecFn o_MouseRelative, o_MouseSolid, o_MouseSolidInvZoom;

#define BOOL_NOARG_HOOK(name, orig) \
    static bool name(void* self) { bool r = orig(self); return Blocked() || Neutral() ? false : r; }

static bool H_LeftPress(void* self) { bool r = o_LeftPress(self); return Neutral() ? PressedByte() : WorldActionBlocked() ? false : r; }
static bool H_LeftPressAOff(void* self) { bool r = o_LeftPressAOff(self); return Neutral() ? PressedByte() : WorldActionBlocked() ? false : r; }
static bool H_LeftDown(void* self) { bool r = o_LeftDown(self); return Neutral() ? HeldByte() : WorldActionBlocked() ? false : r; }
static bool H_RightPress(void* self) { bool r = o_RightPress(self); return WorldActionBlocked() || Neutral() ? false : r; }
static bool H_RightPressAOff(void* self) { bool r = o_RightPressAOff(self); return WorldActionBlocked() || Neutral() ? false : r; }
static bool H_RightDown(void* self) { bool r = o_RightDown(self); return WorldActionBlocked() || Neutral() ? false : r; }
BOOL_NOARG_HOOK(H_MiddleDown, o_MiddleDown)

static bool H_KeyDown(void* self, int key)       { bool r = o_KeyDown(self, key); return Blocked() || Neutral() ? false : r; }
static bool H_KeyPress(void* self, int key)      { bool r = o_KeyPress(self, key); return WorldActionBlocked() || Neutral() ? false : r; }
static bool H_ButtonDown(void* self, char* fn)   { bool r = o_ButtonDown(self, fn); return Blocked() || Neutral() ? false : r; }
static bool H_ButtonPress(void* self, char* fn)  { bool r = o_ButtonPress(self, fn); return WorldActionBlocked() || Neutral() ? false : r; }

static float H_AxisValue(void* self, char* fn, int* out, float a, float b)
{
    float r = o_AxisValue(self, fn, out, a, b);
    return Blocked() || Neutral() ? 0.0f : r;
}

static float* H_MouseRelative(void* self, float* out)
{
    float* r = o_MouseRelative(self, out);
    if (Blocked() || Neutral()) out[0] = out[1] = out[2] = 0.0f;
    return r;
}

static float* FrozenVec(VecFn orig, void* self, float* out, float* frozen, bool* have)
{
    float* r = orig(self, out);
    if (!Blocked()) {
        *have = false;
        return r;
    }
    if (!*have) {
        frozen[0] = out[0]; frozen[1] = out[1]; frozen[2] = out[2];
        *have = true;
    }
    out[0] = frozen[0]; out[1] = frozen[1]; out[2] = frozen[2];
    return r;
}

static float* H_MouseSolid(void* self, float* out)
{
    return FrozenVec(o_MouseSolid, self, out, g_frozenSolid, &g_haveFrozenSolid);
}

static float* H_MouseSolidInvZoom(void* self, float* out)
{
    return FrozenVec(o_MouseSolidInvZoom, self, out, g_frozenSolidInvZoom, &g_haveFrozenSolidInvZoom);
}

// ------------------------------------------------------------------ install

struct InputHookDef {
    const char* symbol;
    void* detour;
    void** original;
};

bool InstallInputBlockHooks()
{
    const InputHookDef defs[] = {
        {"?GetMouseLeftPress@C3D_INPUT@@QEAA_NXZ",          (void*)H_LeftPress,       (void**)&o_LeftPress},
        {"?GetMouseLeftPressAOff@C3D_INPUT@@QEAA_NXZ",      (void*)H_LeftPressAOff,   (void**)&o_LeftPressAOff},
        {"?GetMouseLeftDown@C3D_INPUT@@QEAA_NXZ",           (void*)H_LeftDown,        (void**)&o_LeftDown},
        {"?GetMouseRightPress@C3D_INPUT@@QEAA_NXZ",         (void*)H_RightPress,      (void**)&o_RightPress},
        {"?GetMouseRightPressAOff@C3D_INPUT@@QEAA_NXZ",     (void*)H_RightPressAOff,  (void**)&o_RightPressAOff},
        {"?GetMouseRightDown@C3D_INPUT@@QEAA_NXZ",          (void*)H_RightDown,       (void**)&o_RightDown},
        {"?GetMouseMiddleDown@C3D_INPUT@@QEAA_NXZ",         (void*)H_MiddleDown,      (void**)&o_MiddleDown},
        {"?GetKeyDown@C3D_INPUT@@QEAA_NH@Z",                (void*)H_KeyDown,         (void**)&o_KeyDown},
        {"?GetKeyPress@C3D_INPUT@@QEAA_NH@Z",               (void*)H_KeyPress,        (void**)&o_KeyPress},
        {"?CommonInterfaceGetButtonDown@C3D_INPUT@@QEAA_NPEAD@Z",  (void*)H_ButtonDown,  (void**)&o_ButtonDown},
        {"?CommonInterfaceGetButtonPress@C3D_INPUT@@QEAA_NPEAD@Z", (void*)H_ButtonPress, (void**)&o_ButtonPress},
        {"?CommonInterfaceGetAxisValue@C3D_INPUT@@QEAAMPEADPEAHMM@Z", (void*)H_AxisValue, (void**)&o_AxisValue},
        {"?GetMouseRelative@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ",      (void*)H_MouseRelative,     (void**)&o_MouseRelative},
        {"?GetMouseSolid@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ",         (void*)H_MouseSolid,        (void**)&o_MouseSolid},
        {"?GetMouseSolidWithInvZoom@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ", (void*)H_MouseSolidInvZoom, (void**)&o_MouseSolidInvZoom},
    };

    int ok = 0;
    const int total = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int k = 0; k < total; k++)
        if (PatchIat(g_exe, DLL_ENGINE, defs[k].symbol, defs[k].detour, defs[k].original, "input block"))
            ok++;
    Logf("republicmp  INPUT_BLOCK: %d/%d input hooks installed", ok, total);
    return ok == total;
}

void InputBlock_Tick()
{
    static bool s_wasSyncBlocked = false;
    bool syncBlocked = SyncBlock_BlocksWorldInput();
    if (syncBlocked != s_wasSyncBlocked) {
        Logf("republicmp  INPUT_BLOCK: game input %s", syncBlocked ? "BLOCKED" : "released");
        s_wasSyncBlocked = syncBlocked;
    }
    if (!WorldActionBlocked()) return;

    __try {
        *(unsigned char*)(g_exeBase + LMB_HELD_RVA) = 0;
        *(unsigned char*)(g_exeBase + LMB_JUST_PRESSED_RVA) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
