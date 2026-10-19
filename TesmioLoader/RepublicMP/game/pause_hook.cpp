// SPDX-License-Identifier: GPL-3.0-only
// See pause_hook.h. Logs the caller address (_ReturnAddress()) of each
// hooked call, to help locate the calling code in Ghidra by RVA.
#include "pause_hook.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

void PauseHook_Bind(const TsmHost* host)
{
    TsmBind(host);
}

typedef void (*TimerVoidFn)(void* thisPtr);
typedef void (*TimerMultiplyFPSFn)(void* thisPtr, float mult);

static TimerVoidFn        g_origStart = nullptr;
static TimerVoidFn        g_origStop = nullptr;
static TimerMultiplyFPSFn g_origMultiplyFPS = nullptr;

static volatile long long g_startCount = 0;
static volatile long long g_stopCount = 0;
static volatile long long g_multiplyFpsCount = 0;

static unsigned long long CallerRva(void* returnAddress)
{
    return (unsigned long long)((BYTE*)returnAddress - g_exeBase);
}

static void HookedTimerStart(void* thisPtr)
{
    void* caller = _ReturnAddress();
    g_startCount++;
    Logf("republicmp  PAUSE: C3D_TIMER::Start this=%p callerRVA=0x%llX (call #%lld)",
         thisPtr, CallerRva(caller), g_startCount);
    g_origStart(thisPtr);
}

static void HookedTimerStop(void* thisPtr)
{
    void* caller = _ReturnAddress();
    g_stopCount++;
    Logf("republicmp  PAUSE: C3D_TIMER::Stop this=%p callerRVA=0x%llX (call #%lld)",
         thisPtr, CallerRva(caller), g_stopCount);
    g_origStop(thisPtr);
}

static void HookedTimerMultiplyFPS(void* thisPtr, float mult)
{
    void* caller = _ReturnAddress();
    g_multiplyFpsCount++;
    Logf("republicmp  PAUSE: C3D_TIMER::MultiplyFPS this=%p mult=%.3f callerRVA=0x%llX (call #%lld)",
         thisPtr, mult, CallerRva(caller), g_multiplyFpsCount);
    g_origMultiplyFPS(thisPtr, mult);
}

bool InstallPauseHooks()
{
    bool okStart = PatchIat(g_exe, DLL_ENGINE, "?Start@C3D_TIMER@@QEAAXXZ",
                             (void*)HookedTimerStart, (void**)&g_origStart, "C3D_TIMER::Start");
    Logf("republicmp  PAUSE: hook Start installed: %d", okStart);

    bool okStop = PatchIat(g_exe, DLL_ENGINE, "?Stop@C3D_TIMER@@QEAAXXZ",
                            (void*)HookedTimerStop, (void**)&g_origStop, "C3D_TIMER::Stop");
    Logf("republicmp  PAUSE: hook Stop installed: %d", okStop);

    bool okMultiplyFPS = PatchIat(g_exe, DLL_ENGINE, "?MultiplyFPS@C3D_TIMER@@QEAAXM@Z",
                                   (void*)HookedTimerMultiplyFPS, (void**)&g_origMultiplyFPS,
                                   "C3D_TIMER::MultiplyFPS");
    Logf("republicmp  PAUSE: hook MultiplyFPS installed: %d", okMultiplyFPS);

    return okStart && okStop && okMultiplyFPS;
}
