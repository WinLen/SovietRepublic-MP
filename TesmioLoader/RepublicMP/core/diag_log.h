// SPDX-License-Identifier: GPL-3.0-only
// Comprehensive diagnostic logger for RepublicMP.
// Writes <GameDir>\RepublicMP_Diagnostics.log (and backs up the previous run to
// <GameDir>\RepublicMP_Diagnostics.prev.log), captures all Logf/UiLog/DebugLog
// output, periodic session/lockstep/SteamP2P telemetry, and crash stack traces.
#pragma once

#include "tesmio_plugin.h"

// Wraps TsmHost so every H->log (Logf) call across all modules is also written
// to RepublicMP_Diagnostics.log with millisecond timestamps and Thread IDs.
const TsmHost* DiagLog_InitAndWrapHost(const TsmHost* origHost);

// Logs system, module bases, PE timestamp, version, and CRC32 after hooks install.
void DiagLog_LogStartupSummary(void);

// Writes a formatted line to RepublicMP_Diagnostics.log.
void DiagLog_Printf(const char* category, const char* fmt, ...);

// Called every frame from SaveSync_Tick() to record state transitions and
// periodic 5-second session/lockstep/SteamP2P telemetry snapshots.
void DiagLog_FrameTick(void);

// Suppresses VectoredExceptionHandler crash logging inside guarded __try/__except probes.
void DiagLog_BeginSafeSeh(void);
void DiagLog_EndSafeSeh(void);
int  DiagLog_GetSafeSehDepthForTest(void);
extern "C" __declspec(dllexport) int DiagLog_IsInSafeSeh(void);

// Remote client log upload to Host (<GameDir>\RemoteLogs\<Reason>_<YYYYMMDD_HHMMSS>\)
#include <vector>
#include <stdint.h>
bool DiagLog_BuildClientLogBundle(std::vector<unsigned char>& outPayload, const char* reason);
bool DiagLog_SaveRemoteClientLogBundle(const void* data, size_t size);
bool DiagLog_IsSafeRemoteLogFileName(const char* name);
bool DiagLog_HasPrevCrashOrDesyncLog(void);
bool DiagLog_DetectPrevCrashOrDesync(void);
const char* DiagLog_GetGameDir(void);
const char* DiagLog_GetLoaderBaseDir(void);
void DiagLog_ReadFileTail(const char* fullPath, size_t maxBytes, std::vector<unsigned char>& outBuf);

// Vehicle synchronization & DLC protection telemetry
struct VehicleSyncDiagStats {
    uint64_t purchasesAttempted;
    uint64_t purchasesMade;
    uint64_t purchasesBlockedDlc;
    uint64_t purchasesBlockedMod;
    uint64_t purchasesBlockedMoneyOrDepot;
    uint64_t scriptNameRemapped;
    uint64_t scriptNameResolveFails;
    uint64_t vehiclesSold;
    uint32_t countRoad;
    uint32_t countRail;
    uint32_t countWater;
    uint32_t countAir;
    uint32_t countSpecial;
};

void Purchase_GetDiagStats(VehicleSyncDiagStats* outStats);
void Purchase_RecordDiagSale(unsigned cat);
void Purchase_ResetDiagStatsForTest();

// Lightweight lock-free QPC performance telemetry aggregated per HEARTBT window.
enum DiagPerfBucket {
    DIAG_PERF_WAIT_RENDER = 0,
    DIAG_PERF_EARLY,
    DIAG_PERF_LATE,
    DIAG_PERF_HELD,
    DIAG_PERF_STATE_HASH,
    DIAG_PERF_TS_STEP,
    DIAG_PERF_TS_ECON_HOOK,
    DIAG_PERF_UI_ROUTINE,
    DIAG_PERF_TS_UI,
    DIAG_PERF_PRESENT_HOOK,
    DIAG_PERF_PRESENT_SWAP,
    DIAG_PERF_VIRTUAL_QUERY,
    DIAG_PERF_INTERFRAME_GAP,
    DIAG_PERF_PREPARE_NATIVE,
    DIAG_PERF_PREPARE_MOD,
    DIAG_PERF_BEFORE_AFTER_EARLY,
    DIAG_PERF_PRIVATE_LATE,
    DIAG_PERF_UI_SWITCHES,
    DIAG_PERF_MALLOC_ZERO,
    DIAG_PERF_BUCKET_COUNT
};

struct DiagPerfBucketReport {
    int64_t count;
    double totalMs;
    double avgMs;
    double maxMs;
};

struct DiagPerfSnapshot {
    DiagPerfBucketReport buckets[DIAG_PERF_BUCKET_COUNT];
    int64_t mallocCalls;
    int64_t mallocBytes;
    double winTimerResMs;
    bool windowFocused;
};

int64_t DiagPerf_Now(void);
void DiagPerf_Record(DiagPerfBucket bucket, int64_t qpcDelta);
void DiagPerf_RecordBatch(DiagPerfBucket bucket, int64_t count, int64_t totalQpcTicks, int64_t maxQpcTicks);
void DiagPerf_RecordVirtualQuery(int64_t qpcDelta);
void DiagPerf_RecordMalloc(size_t bytes);
void DiagPerf_RecordMallocBatch(int64_t calls, int64_t bytes);
void DiagPerf_TakeSnapshot(DiagPerfSnapshot* out);
void DiagLog_FormatPerfMainLine(const DiagPerfSnapshot& ps, char* out, size_t outSize);
void DiagLog_FormatPerfRndLine(const DiagPerfSnapshot& ps, bool p2pConn, bool p2pRelay, int32_t p2pSendQ,
                               char* out, size_t outSize);
double DiagLog_QueryTimerResolutionMs(void);

