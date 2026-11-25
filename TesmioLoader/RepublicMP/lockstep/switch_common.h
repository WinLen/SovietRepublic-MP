// SPDX-License-Identifier: GPL-3.0-only
// Shared helpers for lockstep switch & slot modules (in-place compare, SEH span batch, adaptive idle backoff).
#pragma once

#include "world_refs.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#define SWITCH_DOWN_RVA 0xA54E90u
#define SWITCH_PRESS_RVA 0xA54E91u
#define SWITCH_ENTER_RVA 0xA54CACu
#define SWITCH_KEYPAD_ENTER_RVA 0xA54D2Cu
#define SWITCH_WINDOW_OBJECT_OFF 0x240u
#define SWITCH_WINDOW_KIND_OFF 0x248u
#define SWITCH_MAX_WINDOWS 4096u
#define SWITCH_META_CHUNK 64u
#define SWITCH_SAMPLE_EVERY_BASE 16u
#define SWITCH_SAMPLE_EVERY_IDLE 128u
#define SWITCH_IDLE_BACKOFF_SAMPLES 32u

struct SwitchWindowFingerprint {
    U64 begin;
    U64 end;
    U64 hash;
    bool valid;

    bool operator==(const SwitchWindowFingerprint& o) const
    {
        return valid == o.valid && begin == o.begin && end == o.end && hash == o.hash;
    }
    bool operator!=(const SwitchWindowFingerprint& o) const { return !(*this == o); }
};

struct SwitchBackoffState {
    unsigned framesSinceSample;
    unsigned sampleEvery;
    unsigned cleanSamples;
    SwitchWindowFingerprint lastWinFp;
};

struct SwitchSpan {
    const void* src;
    size_t dstOffset;
    size_t bytes;
};

struct SwitchPerfStatsForTest {
    unsigned sampleEvery;
    unsigned cleanSamples;
    unsigned framesSinceSample;
    unsigned long long snapshotCount;
    unsigned long long mismatchCopyCount;
    size_t listScratchCapacity;
    size_t cmpScratchCapacity;
};

static inline bool SwitchCommon_Clicked(const uint8_t* exeBase)
{
    if (!exeBase) return false;
    float wheel = 0.0f;
    SafeReadPod((const float*)(exeBase + MOUSE_WHEEL_DELTA_RVA), &wheel);
    return exeBase[SWITCH_DOWN_RVA] || exeBase[SWITCH_PRESS_RVA] ||
           exeBase[SWITCH_ENTER_RVA] || exeBase[SWITCH_KEYPAD_ENTER_RVA] ||
           (isfinite(wheel) && wheel != 0.0f);
}

static inline bool SwitchCommon_GuardedRef(bool isVehicle, U64 obj, RouteRef& out)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        ok = isVehicle ? Ref_Vehicle(obj, out) : Ref_Building(obj, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static inline bool SwitchCommon_ReadPtrVector(const void* vecHeaderAddr, size_t maxCount,
                                              std::vector<U64>& out, bool* outWasNonEmpty = nullptr)
{
    out.clear();
    if (outWasNonEmpty) *outWasNonEmpty = false;
    U64 v[2] = {};
    if (!vecHeaderAddr || !SafeMemcpyPod(v, vecHeaderAddr, sizeof(v))) return false;
    if (!v[0] && !v[1]) return true;
    if (!v[0] || v[1] < v[0] || ((v[1] - v[0]) & 7u) != 0u) return false;
    const size_t n = (size_t)((v[1] - v[0]) / 8u);
    if (n > maxCount) return false;
    if (outWasNonEmpty) *outWasNonEmpty = (n > 0);
    if (n == 0) return true;
    out.resize(n);
    if (!SafeMemcpyPod(out.data(), (const void*)v[0], n * 8u)) {
        out.clear();
        return false;
    }
    return true;
}

static inline void SwitchCommon_CopySpanBatchPod(SwitchSpan* spans, size_t count, BYTE* dstBase)
{
    if (!spans || !dstBase || count == 0) return;
    DiagLog_BeginSafeSeh();
    size_t idx = 0;
    while (idx < count) {
        volatile size_t cur = idx;
        __try {
            for (; idx < count; ++idx) {
                cur = idx;
                if (spans[idx].bytes == 0 || !spans[idx].src) continue;
                memcpy(dstBase + spans[idx].dstOffset, spans[idx].src, spans[idx].bytes);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            const size_t failed = cur;
            if (failed < count) {
                spans[failed].bytes = 0;
            }
            idx = failed + 1u;
        }
    }
    DiagLog_EndSafeSeh();
}

static inline bool SwitchCommon_CopyMismatchToScratch(const void* livePtr, size_t bytes, std::vector<BYTE>& scratch)
{
    if (!livePtr || bytes == 0) return false;
    scratch.resize(bytes);
    return SafeMemcpyPod(scratch.data(), livePtr, bytes);
}

static inline U64 SwitchCommon_MixFnv64(U64 h, U64 val)
{
    h ^= val;
    h *= 1099511628211ull;
    return h;
}

static inline bool SwitchCommon_ReadWindowHeaderPod(
    const uint8_t* exeBase, U64* outBegin, U64* outEnd, size_t* outCount)
{
    *outBegin = 0;
    *outEnd = 0;
    *outCount = 0;
    if (!exeBase) return false;
    bool valid = false;
    DiagLog_BeginSafeSeh();
    __try {
        const U64* vec = (const U64*)(exeBase + WORLD_OPEN_WINDOWS_BEGIN_RVA);
        const U64 beg = vec[0];
        const U64 end = vec[1];
        if (!beg && !end) {
            valid = true;
        } else if (beg && end >= beg && ((end - beg) & 7u) == 0u) {
            const size_t count = (size_t)((end - beg) / 8u);
            if (count <= SWITCH_MAX_WINDOWS) {
                *outBegin = beg;
                *outEnd = end;
                *outCount = count;
                valid = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        valid = false;
    }
    DiagLog_EndSafeSeh();
    return valid;
}

static inline bool SwitchCommon_ReadWindowPtrChunkPod(
    const U64* ptrs, size_t count, U64* outChunk, U64* ioHash)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        U64 h = *ioHash;
        for (size_t i = 0; i < count; ++i) {
            const U64 w = ptrs[i];
            outChunk[i] = w;
            h = SwitchCommon_MixFnv64(h, w);
        }
        *ioHash = h;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static inline bool SwitchCommon_TryReadWindowMetaPod(U64 win, U64* outObj, unsigned* outKind)
{
    *outObj = 0;
    *outKind = 0;
    if (!IsPointerLikeX64(win)) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        *outObj = *(const U64*)(win + SWITCH_WINDOW_OBJECT_OFF);
        *outKind = *(const unsigned*)(win + SWITCH_WINDOW_KIND_OFF);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static inline SwitchWindowFingerprint SwitchCommon_ReadWindowFingerprintPod(const uint8_t* exeBase)
{
    SwitchWindowFingerprint fp = { 0, 0, 0, false };
    size_t count = 0;
    if (!SwitchCommon_ReadWindowHeaderPod(exeBase, &fp.begin, &fp.end, &count)) {
        return fp;
    }
    if (!fp.begin && !fp.end) {
        fp.hash = SwitchCommon_MixFnv64(14695981039346656037ull, 0ull);
        fp.valid = true;
        return fp;
    }
    U64 h = 14695981039346656037ull;
    h = SwitchCommon_MixFnv64(h, fp.begin);
    h = SwitchCommon_MixFnv64(h, fp.end);
    const U64* ptrs = (const U64*)(uintptr_t)fp.begin;
    U64 chunk[SWITCH_META_CHUNK] = {};
    for (size_t base = 0; base < count; base += SWITCH_META_CHUNK) {
        const size_t n = (count - base < SWITCH_META_CHUNK) ? (count - base) : SWITCH_META_CHUNK;
        if (!SwitchCommon_ReadWindowPtrChunkPod(ptrs + base, n, chunk, &h)) {
            return { 0, 0, 0, false };
        }
        for (size_t i = 0; i < n; ++i) {
            U64 obj = 0;
            unsigned kind = 0;
            if (chunk[i] && SwitchCommon_TryReadWindowMetaPod(chunk[i], &obj, &kind)) {
                h = SwitchCommon_MixFnv64(h, obj);
                h = SwitchCommon_MixFnv64(h, (U64)kind);
            }
        }
    }
    fp.hash = h;
    fp.valid = true;
    return fp;
}

static inline void SwitchBackoff_Reset(SwitchBackoffState& st)
{
    st.framesSinceSample = 0;
    st.sampleEvery = SWITCH_SAMPLE_EVERY_BASE;
    st.cleanSamples = 0;
    st.lastWinFp = { 0, 0, 0, false };
}

static inline bool SwitchBackoff_ShouldSampleBefore(SwitchBackoffState& st, const uint8_t* exeBase, bool click)
{
    const SwitchWindowFingerprint curWin = SwitchCommon_ReadWindowFingerprintPod(exeBase);
    if (click) {
        st.sampleEvery = SWITCH_SAMPLE_EVERY_BASE;
        st.cleanSamples = 0;
        st.framesSinceSample = 0;
        st.lastWinFp = curWin;
        return true;
    }
    if (!curWin.valid || !st.lastWinFp.valid || curWin != st.lastWinFp) {
        st.sampleEvery = SWITCH_SAMPLE_EVERY_BASE;
        st.cleanSamples = 0;
        st.lastWinFp = curWin;
    }
    if (++st.framesSinceSample >= st.sampleEvery) {
        st.framesSinceSample = 0;
        return true;
    }
    return false;
}

static inline void SwitchBackoff_RecordAfter(SwitchBackoffState& st, const uint8_t* exeBase, bool click, bool anyChange)
{
    const SwitchWindowFingerprint curWin = SwitchCommon_ReadWindowFingerprintPod(exeBase);
    const bool winInvalidOrChanged = !curWin.valid || !st.lastWinFp.valid || curWin != st.lastWinFp;
    if (click || anyChange || winInvalidOrChanged) {
        st.sampleEvery = SWITCH_SAMPLE_EVERY_BASE;
        st.cleanSamples = 0;
        st.lastWinFp = curWin;
        return;
    }
    if (st.cleanSamples < SWITCH_IDLE_BACKOFF_SAMPLES) {
        ++st.cleanSamples;
    }
    if (st.cleanSamples >= SWITCH_IDLE_BACKOFF_SAMPLES) {
        st.sampleEvery = SWITCH_SAMPLE_EVERY_IDLE;
    }
}

static inline void SwitchCommon_FillStats(
    SwitchPerfStatsForTest* out, const SwitchBackoffState& backoff,
    unsigned long long snapshotCount, unsigned long long mismatchCopyCount,
    size_t listCap, size_t cmpCap)
{
    if (!out) return;
    out->sampleEvery = backoff.sampleEvery;
    out->cleanSamples = backoff.cleanSamples;
    out->framesSinceSample = backoff.framesSinceSample;
    out->snapshotCount = snapshotCount;
    out->mismatchCopyCount = mismatchCopyCount;
    out->listScratchCapacity = listCap;
    out->cmpScratchCapacity = cmpCap;
}
