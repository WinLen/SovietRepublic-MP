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
    if (bytes == 0) return true;
    if (!dst || !src) return false;
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

inline bool SafeBytesMatch(const void* addr, const void* expected, size_t bytes)
{
    if (bytes == 0) return true;
    if (!addr || !expected) return false;
    bool match = false;
    DiagLog_BeginSafeSeh();
    __try {
        match = (memcmp(addr, expected, bytes) == 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        match = false;
    }
    DiagLog_EndSafeSeh();
    return match;
}

struct TsmRoutineSig {
    unsigned rva;
    unsigned char entry[16];
    unsigned size; // 0 means 16
};

inline bool TsmCheckRoutineTable(const uint8_t* exeBase, int* readyState,
                                 const TsmRoutineSig* table, size_t count,
                                 unsigned* outFailedRva = nullptr)
{
    if (!exeBase || !readyState || !table) return false;
    if (*readyState < 0) {
        *readyState = 1;
        for (size_t i = 0; i < count; ++i) {
            size_t len = table[i].size ? table[i].size : 16u;
            if (!SafeBytesMatch(exeBase + table[i].rva, table[i].entry, len)) {
                if (outFailedRva) *outFailedRva = table[i].rva;
                *readyState = 0;
                break;
            }
        }
    }
    return *readyState == 1;
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

inline bool SafeIsWorldLoaded(const void* exeBase)
{
    if (!exeBase) return false;
    const uint8_t* base = (const uint8_t*)exeBase;
    uintptr_t b = 0, e = 0;
    return SafeReadPod(base + 0x9E6A18, &b) && SafeReadPod(base + 0x9E6A20, &e) && b != 0 && e != 0;
}

inline bool SafeIsToolGuardBusy(uintptr_t worldThisPtr)
{
    if (!worldThisPtr) return false;
    const uint8_t* w = (const uint8_t*)worldThisPtr;
    uint8_t g1 = 0, g2 = 0, g3 = 0;
    if (!SafeReadPod(w + 0xFBC, &g1) || !SafeReadPod(w + 0xFBE, &g2) || !SafeReadPod(w + 0xFC3, &g3))
        return false;
    return (g1 != 0) || (g2 != 0) || (g3 != 0);
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

// Validates a C3D_MATERIAL pointer before SaveOriginalValues/ChangeValuesTo/RestoreOriginalValues.
inline bool SafeIsValidMaterial(const void* mat)
{
    if (!mat) return false;
    if (((uintptr_t)mat & 7u) != 0u) return false;
    uintptr_t addr = (uintptr_t)mat;
    if (!IsPointerLikeX64((uint64_t)addr)) return false;

    // std::vector<C3D_SUBMATERIAL> header at offset 0: { begin, end, cap }
    uintptr_t ptrs[2] = { 0, 0 };
    if (!SafeMemcpyPod(ptrs, mat, sizeof(ptrs))) return false;
    uintptr_t b = ptrs[0];
    uintptr_t e = ptrs[1];

    if (b == 0 && e == 0) return true;
    if (b == e) return IsPointerLikeX64((uint64_t)b);
    if (b > e) return false;

    uintptr_t diff = e - b;
    // sizeof(C3D_SUBMATERIAL) is 552 (0x228) bytes
    if ((diff % 552u) != 0) return false;
    uintptr_t count = diff / 552u;
    if (count > 1024u) return false;

    if (!IsPointerLikeX64((uint64_t)b) || !IsPointerLikeX64((uint64_t)e)) return false;

    // Probe first submaterial structure to verify readable memory
    uint8_t probe[16];
    return SafeMemcpyPod(probe, (const void*)b, sizeof(probe));
}
