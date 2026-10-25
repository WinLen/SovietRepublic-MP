// SPDX-License-Identifier: GPL-3.0-only
// Thread-safe synchronization primitives and POD-only SEH memory helpers.
// Strictly follows AGENTS.md Rule #1 (MSVC C2712: no C++ destructors inside __try functions).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "diag_log.h"

// ---------------------------------------------------------------------------
// Thread-Safe Lazy CRITICAL_SECTION Initialization via Windows INIT_ONCE
// ---------------------------------------------------------------------------

inline BOOL CALLBACK TsmInitCsOnceCallback(PINIT_ONCE, PVOID param, PVOID*)
{
    InitializeCriticalSection((CRITICAL_SECTION*)param);
    return TRUE;
}

inline void TsmEnsureCriticalSection(INIT_ONCE* once, CRITICAL_SECTION* cs)
{
    InitOnceExecuteOnce(once, TsmInitCsOnceCallback, cs, nullptr);
}

// RAII Critical Section guard for use in non-SEH functions.
struct TsmScopedCsLock {
    CRITICAL_SECTION* cs;
    TsmScopedCsLock(INIT_ONCE* once, CRITICAL_SECTION* targetCs) : cs(targetCs)
    {
        TsmEnsureCriticalSection(once, cs);
        EnterCriticalSection(cs);
    }
    ~TsmScopedCsLock()
    {
        LeaveCriticalSection(cs);
    }
    TsmScopedCsLock(const TsmScopedCsLock&) = delete;
    TsmScopedCsLock& operator=(const TsmScopedCsLock&) = delete;
};

// ---------------------------------------------------------------------------
// POD-only SEH Memory Access Helpers (MSVC C2712 compliant)
// ---------------------------------------------------------------------------

inline bool SafeMemcpyPod(void* dst, const void* src, size_t bytes)
{
    if (!dst || !src || bytes == 0) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        memcpy(dst, src, bytes);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

template <typename T>
inline bool SafeReadPod(const void* src, T* outVal)
{
    return SafeMemcpyPod(outVal, src, sizeof(T));
}

template <typename T>
inline bool SafeWritePod(void* dst, const T& val)
{
    return SafeMemcpyPod(dst, &val, sizeof(T));
}

// Safely counts elements in an MSVC x64 std::vector header { begin, end, cap }.
inline long long SafeCountStdVector(const void* vecHeader, size_t elemSize, long long maxElements = 2000000LL)
{
    if (!vecHeader || elemSize == 0) return 0;
    uintptr_t ptrs[2] = { 0, 0 };
    if (!SafeMemcpyPod(ptrs, vecHeader, sizeof(ptrs))) return 0;
    if (ptrs[0] == 0 || ptrs[1] < ptrs[0]) return 0;
    uintptr_t byteDiff = ptrs[1] - ptrs[0];
    if ((byteDiff % elemSize) != 0) return 0;
    long long n = (long long)(byteDiff / elemSize);
    if (n < 0 || n > maxElements) return 0;
    return n;
}

// Invariant #12 (AGENTS.md): Detects x64 heap/module pointers in a 64-bit word,
// including low Windows x64 segment heap addresses (high >= 1 && high < 0x100).
inline bool IsPointerLikeX64(uint64_t v)
{
    unsigned high = (unsigned)(v >> 32);
    if (high >= 0x100u && high <= 0x7FFFu) return true;
    if (high >= 1u && high < 0x100u && (v & 3u) == 0u) {
        uint64_t probe = 0;
        if (SafeMemcpyPod(&probe, (const void*)(uintptr_t)v, sizeof(probe))) {
            return true;
        }
    }
    return false;
}
