// SPDX-License-Identifier: GPL-3.0-only
// Per-state economy, loans, stat ledger, UI swaps, denomination, and building/vehicle econ hooks.
#include "two_states_internal.h"
#include <memory>

void TwoStates_BindEcon(const TsmHost* host) { if (host) TsmBind(host); }

#ifndef RVA_BLD_ECON
#define RVA_BLD_ECON        0x173340
#endif
#ifndef RVA_CONSTRUCT_STEP
#define RVA_CONSTRUCT_STEP  0x159070
#endif
#ifndef RVA_VEHICLE_SALE
#define RVA_VEHICLE_SALE    0x3E0670
#endif

// ---------------------------------------------------------------------------
// Economy, UI Swap & Attribution State
// ---------------------------------------------------------------------------

static INIT_ONCE g_moneySwapLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_moneySwapLock;
static bool g_uiMoneySwapped = false;
static double g_uiSavedSimRub = 0.0;
static double g_uiSavedSimUsd = 0.0;
static double g_uiShownRub = 0.0;
static double g_uiShownUsd = 0.0;
static bool g_uiLoansSwapped = false;
static bool g_uiLoansFilterByState = false;
static int  g_uiLoansOwnerSubType = 0;
static int  g_uiSavedGlobalSelectedIdx = -1;
static int  g_uiLocalSelectedIdxBefore = -1;
static float g_uiSavedLoanStats[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static std::vector<SovietLoan> g_uiFullLoansBackup;
static std::vector<SovietLoan> g_uiFilteredLoansBefore;
static std::vector<int>        g_uiFilteredToGlobalIdx;


StateEconPeriodStats              g_stateCurEcon[2];
std::vector<StateEconPeriodStats> g_stateHistEcon[2];
uint64_t                          g_stateHistSwapGen = 1;
bool                              g_stateEconInitialized = false;
StateTrackerSlice                 g_stateTrackerCur[2] = {};
StateTrackerSlice                 g_stateTrackerOld[2] = {};
bool                              g_stateTrackerInitialized = false;
static bool                              g_uiTrackerSwapped = false;
static BYTE                              g_uiSavedTrackerBlock[WORLD_TRACKER_BLOCK_SIZE] = {};
static bool                              g_uiEconSwapped = false;
static int                               g_uiEconSwappedState = 0;
static StatRecordUiBackup                g_uiSavedCurStat = {};
static std::vector<StatRecordUiBackup>   g_uiSavedHistStats;
static std::vector<StatRecordUiBackup>   g_uiPreparedHistSwap;
static std::vector<uint8_t>              g_uiHistDirtyMask;
static BYTE*                             g_uiSavedHistBegin = nullptr;
static int                               g_uiSavedHistCount = 0;
static int                               g_uiSavedHistState = 0;
static int                               g_uiSavedHistSwapCount = 0;
static uint64_t                          g_uiSavedHistGen = 0;
static bool                              g_uiHistBackupValid = false;
static int                               g_uiPreparedHistSwapState = 0;
static int                               g_uiPreparedHistSwapCount = 0;
static uint64_t                          g_uiPreparedHistSwapGen = 0;
static std::vector<SovietLoan>           g_uiAfterLoansScratch;


static bool                              g_uiCitVecSwapped = false;
static uintptr_t                         g_uiSavedCitVecHdr[3] = {};
static std::vector<BYTE*>                g_uiLocalCitizens;
static std::vector<SavedCitizenLifespan> g_uiSavedCitizenLifespans;
static std::vector<SavedCitizenLifespan> g_uiDirtyCitizenLifespans;
static std::unordered_set<BYTE*>         g_uiDirtyCitizenSet;
static bool                              g_uiHudPopSwapped = false;
static BYTE                              g_uiSavedHudPopBlock[WORLD_HUD_POP_BLOCK_SIZE] = {};
static BYTE                              g_stateHudPopBlock[2][WORLD_HUD_POP_BLOCK_SIZE] = {};
static bool                              g_stateHudPopInitialized[2] = { false, false };
static int                               g_stateLastLocalPop[2] = { -1, -1 };
static int32_t                           g_stateLastEduBasic[2] = { -1, -1 };
static int32_t                           g_stateLastEduUniv[2]  = { -1, -1 };
static StateEconScalars                  g_uiSavedPopWinScalars = {};
static int                               g_lastUiSwappedState = 0;
static size_t                            g_lastUiSwappedHistLen[2] = { 0, 0 };
static std::vector<BYTE*>                g_uiLocalBuildings;
static std::vector<BYTE*>                g_uiLocalVehicles;
static int                               g_testStoredResBldCount = 0;
static int                               g_testStoredResVehCount = 0;

struct UiCitizenCache {
    bool               valid = false;
    const BYTE*        world = nullptr;
    unsigned long long stepCount = 0;
    unsigned long long applyEpoch = 0;
    int                localState = 0;
    const BYTE*        citVecBegin = nullptr;
    ptrdiff_t          citVecCount = 0;
    uint64_t           ownerMapGen = 0;
    uint64_t           vehOwnerMapGen = 0;
    int                localPop = 0;
    StateEconScalars   demoScalars = {};
};
static UiCitizenCache g_uiCitizenCache = {};
static uint64_t       g_uiCitizenCacheBuildCount = 0;
static bool           g_histBackfillDone[2] = { false, false };
static size_t         g_histBackfillCheckedLen[2] = { 0, 0 };
static uint64_t       g_histBackfillScanCount = 0;


static void EnsureMoneySwapLock()
{
    TsmEnsureCriticalSection(&g_moneySwapLockOnce, &g_moneySwapLock);
}

// ---------------------------------------------------------------------------
// Bank / Loan POD & SEH Helpers (WORLD + 0x10BD0 / 0x10BE8 / 0x11718)
// ---------------------------------------------------------------------------

static const BYTE kVecLoanPushBackExpect[8] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83
};
typedef void (__fastcall* VecLoanPushBackFn)(BYTE* vecPtr, const SovietLoan* loan);

static bool ParseLoanVectorHeaderRaw(const BYTE* world, SovietLoan** outBegin, int* outCount, int* outCapCount)
{
    if (!world || !outBegin || !outCount || !outCapCount) return false;
    *outBegin = nullptr;
    *outCount = 0;
    *outCapCount = 0;
    const BYTE* vec = world + WORLD_LOANS_OFFSET;
    SovietLoan* b = *(SovietLoan* const*)(vec + 0);
    SovietLoan* e = *(SovietLoan* const*)(vec + 8);
    SovietLoan* c = *(SovietLoan* const*)(vec + 16);
    if (!b || e < b || c < e) {
        *outBegin = b;
        return b == nullptr && e == nullptr;
    }
    ptrdiff_t byteLen = (const BYTE*)e - (const BYTE*)b;
    ptrdiff_t capBytes = (const BYTE*)c - (const BYTE*)b;
    if (byteLen < 0 || byteLen % (ptrdiff_t)sizeof(SovietLoan) != 0 ||
        byteLen > 4096 * (ptrdiff_t)sizeof(SovietLoan)) {
        return false;
    }
    *outBegin = b;
    *outCount = (int)(byteLen / (ptrdiff_t)sizeof(SovietLoan));
    *outCapCount = (int)(capBytes / (ptrdiff_t)sizeof(SovietLoan));
    return true;
}

static bool SafeReadLoanVectorHeader(const BYTE* world, SovietLoan** outBegin, int* outCount, int* outCapCount)
{
    if (!world || !outBegin || !outCount || !outCapCount) return false;
    *outBegin = nullptr;
    *outCount = 0;
    *outCapCount = 0;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        ok = ParseLoanVectorHeaderRaw(world, outBegin, outCount, outCapCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void SafeReadLoanStats(const BYTE* world, float outStats[4])
{
    if (!outStats) return;
    memset(outStats, 0, 4 * sizeof(float));
    if (world) SafeMemcpyPod(outStats, world + WORLD_LOAN_STATS_OFFSET, 4 * sizeof(float));
}

static void SafeWriteLoanStats(BYTE* world, const float stats[4])
{
    if (world && stats) SafeMemcpyPod(world + WORLD_LOAN_STATS_OFFSET, stats, 4 * sizeof(float));
}

static void SafeAddLoanStatsDelta(BYTE* world, const float dStats[4])
{
    if (!world || !dStats) return;
    float cur[4] = {};
    SafeReadLoanStats(world, cur);
    for (int i = 0; i < 4; ++i) {
        if (isfinite(dStats[i])) cur[i] += dStats[i];
    }
    SafeWriteLoanStats(world, cur);
}

static bool SafePushBackLoan(BYTE* world, const SovietLoan& loan)
{
    if (!world) return false;
    if (g_exeBase && SafeBytesMatch(g_exeBase + RVA_VEC_LOAN_PUSH_BACK, kVecLoanPushBackExpect, sizeof(kVecLoanPushBackExpect))) {
        DiagLog_BeginSafeSeh();
        __try {
            VecLoanPushBackFn pushFn = (VecLoanPushBackFn)(g_exeBase + RVA_VEC_LOAN_PUSH_BACK);
            pushFn(world + WORLD_LOANS_OFFSET, &loan);
            DiagLog_EndSafeSeh();
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            DiagLog_EndSafeSeh();
            return false;
        }
    }
    SovietLoan* b = nullptr;
    int curCount = 0, capCount = 0;
    if (!SafeReadLoanVectorHeader(world, &b, &curCount, &capCount) || !b || curCount >= capCount) {
        return false;
    }
    DiagLog_BeginSafeSeh();
    __try {
        b[curCount] = loan;
        *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b + (curCount + 1);
        DiagLog_EndSafeSeh();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DiagLog_EndSafeSeh();
        return false;
    }
}

static bool SafeEraseLoanAt(BYTE* world, int globalIdx)
{
    SovietLoan* b = nullptr;
    int curCount = 0, capCount = 0;
    if (!SafeReadLoanVectorHeader(world, &b, &curCount, &capCount) || !b) return false;
    if (globalIdx < 0 || globalIdx >= curCount) return false;
    DiagLog_BeginSafeSeh();
    __try {
        if (globalIdx + 1 < curCount) {
            memmove(&b[globalIdx], &b[globalIdx + 1], (size_t)(curCount - globalIdx - 1) * sizeof(SovietLoan));
        }
        *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b + (curCount - 1);
        int32_t sel = *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET);
        if (sel == globalIdx) {
            *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = -1;
        } else if (sel > globalIdx) {
            *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = sel - 1;
        }
        DiagLog_EndSafeSeh();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DiagLog_EndSafeSeh();
        return false;
    }
}

static bool DetectUiLoanOperation(const std::vector<SovietLoan>& afterLoans, LsLoanCommand* outCmd)
{
    if (!outCmd) return false;
    memset(outCmd, 0, sizeof(*outCmd));
    size_t beforeN = g_uiFilteredLoansBefore.size();
    size_t afterN  = afterLoans.size();

    if (afterN == beforeN + 1) {
        outCmd->op = LOAN_OP_BORROW;
        outCmd->ownerSubType = g_uiLoansOwnerSubType;
        outCmd->ownerLoanOrd = (int32_t)beforeN;
        outCmd->loan = afterLoans.back();
        outCmd->loan.loanSubType = g_uiLoansOwnerSubType;
        return true;
    }
    if (afterN + 1 == beforeN) {
        int ord = g_uiLocalSelectedIdxBefore;
        if (ord < 0 || (size_t)ord >= beforeN) {
            ord = (int)afterN;
            for (size_t i = 0; i < afterN; ++i) {
                if (fabsf(afterLoans[i].initialPrincipal - g_uiFilteredLoansBefore[i].initialPrincipal) > 0.5f ||
                    afterLoans[i].loanType != g_uiFilteredLoansBefore[i].loanType ||
                    afterLoans[i].daysRemaining != g_uiFilteredLoansBefore[i].daysRemaining) {
                    ord = (int)i;
                    break;
                }
            }
        }
        outCmd->op = LOAN_OP_REPAY_FULL;
        outCmd->ownerSubType = g_uiLoansOwnerSubType;
        outCmd->ownerLoanOrd = (int32_t)ord;
        outCmd->loan = g_uiFilteredLoansBefore[(size_t)ord];
        outCmd->loan.loanSubType = g_uiLoansOwnerSubType;
        return true;
    }
    if (afterN == beforeN) {
        for (size_t i = 0; i < afterN; ++i) {
            if (fabsf(afterLoans[i].remainingPrincipal - g_uiFilteredLoansBefore[i].remainingPrincipal) > 0.005f ||
                fabsf(afterLoans[i].totalPaidSoFar - g_uiFilteredLoansBefore[i].totalPaidSoFar) > 0.005f) {
                outCmd->op = LOAN_OP_REPAY_PARTIAL;
                outCmd->ownerSubType = g_uiLoansOwnerSubType;
                outCmd->ownerLoanOrd = (int32_t)i;
                outCmd->loan = afterLoans[i];
                outCmd->loan.loanSubType = g_uiLoansOwnerSubType;
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-State Economic Statistics POD & SEH Helpers (WORLD + 0x11328 / 0x11728)
// ---------------------------------------------------------------------------


void TwoStates_BumpHistSwapGen()
{
    g_stateHistSwapGen++;
    g_uiHistBackupValid = false;
}

uint64_t TwoStates_GetHistSwapGenForTest()
{
    return g_stateHistSwapGen;
}

void TwoStates_InvalidateUiCitizenCache()
{
    g_uiCitizenCache.valid = false;
}

void TwoStates_InvalidateHistBackfillCache()
{
    g_histBackfillDone[0] = g_histBackfillDone[1] = false;
    g_histBackfillCheckedLen[0] = g_histBackfillCheckedLen[1] = 0;
}

void TwoStates_InvalidateUiHistBackupCache()
{
    g_uiHistBackupValid = false;
    g_uiPreparedHistSwapState = 0;
    g_uiPreparedHistSwapCount = 0;
    g_uiPreparedHistSwapGen = 0;
}

uint64_t TwoStates_GetUiCitizenCacheBuildCountForTest()
{
    return g_uiCitizenCacheBuildCount;
}

uint64_t TwoStates_GetHistBackfillScanCountForTest()
{
    return g_histBackfillScanCount;
}

void ResetStateHudPopCache()
{
    memset(g_stateHudPopBlock, 0, sizeof(g_stateHudPopBlock));
    g_stateHudPopInitialized[0] = g_stateHudPopInitialized[1] = false;
    g_stateLastLocalPop[0] = g_stateLastLocalPop[1] = -1;
    g_stateLastEduBasic[0] = g_stateLastEduBasic[1] = -1;
    g_stateLastEduUniv[0]  = g_stateLastEduUniv[1]  = -1;
    g_lastUiSwappedState = 0;
    g_lastUiSwappedHistLen[0] = g_lastUiSwappedHistLen[1] = 0;
    TwoStates_InvalidateUiCitizenCache();
    TwoStates_InvalidateHistBackfillCache();
    TwoStates_InvalidateUiHistBackupCache();
}

#ifdef __clang__
#define RMP_OFFSETOF(s, m) __builtin_offsetof(s, m)
#else
#define RMP_OFFSETOF(s, m) offsetof(s, m)
#endif

// Compile-time layout invariants for contiguous 3-block scalar copy & 5-memcmp attribution
static_assert(RMP_OFFSETOF(StateEconScalars, citizenCounts) == RMP_OFFSETOF(StateEconScalars, immigrantSpend) + sizeof(float[2]),
              "StateEconScalars Slice 2 must be contiguous");
static_assert(STAT_CITIZEN_COUNTS_OFF == STAT_IMMIGRANT_SPEND_OFF + 8u,
              "Engine Slice 2 layout must be contiguous");
static_assert(RMP_OFFSETOF(StateEconScalars, satisfaction) == RMP_OFFSETOF(StateEconScalars, citizenCounts) + sizeof(int32_t[5]),
              "StateEconScalars Block B must be contiguous");
static_assert(STAT_CITIZEN_SATISFACTION_OFF == STAT_CITIZEN_COUNTS_OFF + 20u,
              "Engine Block B layout must be contiguous");
static_assert(RMP_OFFSETOF(StateEconScalars, demographics) == RMP_OFFSETOF(StateEconScalars, citizenAverages) + sizeof(float[3]),
              "StateEconScalars Block C1 must be contiguous");
static_assert(RMP_OFFSETOF(StateEconScalars, citizenBrackets) == RMP_OFFSETOF(StateEconScalars, demographics) + sizeof(int32_t[4]),
              "StateEconScalars Block C2 must be contiguous");
static_assert(RMP_OFFSETOF(StateEconScalars, extraCounts) == RMP_OFFSETOF(StateEconScalars, citizenBrackets) + sizeof(int32_t[9]),
              "StateEconScalars Block C3 must be contiguous");
static_assert(RMP_OFFSETOF(StateEconScalars, tourism) == RMP_OFFSETOF(StateEconScalars, extraCounts) + sizeof(int32_t[21]),
              "StateEconScalars Slice 3a must be contiguous");
static_assert(RMP_OFFSETOF(StateEconScalars, loans) == RMP_OFFSETOF(StateEconScalars, tourism) + sizeof(float[44]),
              "StateEconScalars Slice 3b must be contiguous");
static_assert(STAT_CITIZEN_DEMOGRAPHICS_OFF == STAT_CITIZEN_AVERAGES_OFF + 12u &&
              STAT_CITIZEN_BRACKETS_OFF == STAT_CITIZEN_DEMOGRAPHICS_OFF + 16u &&
              STAT_EXTRA_COUNTS_OFF == STAT_CITIZEN_BRACKETS_OFF + 36u &&
              STAT_TOURISM_OFF == STAT_EXTRA_COUNTS_OFF + 84u &&
              STAT_LOANS_OFF == STAT_TOURISM_OFF + 176u &&
              STAT_LOANS_OFF + 16u == STAT_RECORD_SIZE,
              "Engine Block C / Slice 3 layout must be contiguous");
static_assert(RMP_OFFSETOF(StateTrackerSlice, healthCnt) == RMP_OFFSETOF(StateTrackerSlice, healthSum) + sizeof(float[TRACKER_HEALTH_CAUSE_COUNT]),
              "StateTrackerSlice health block must be contiguous");
static_assert(WORLD_TRACKER_HEALTH_CUR_CNT_OFF == WORLD_TRACKER_HEALTH_CUR_SUM_OFF + 48u &&
              WORLD_TRACKER_HEALTH_OLD_CNT_OFF == WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 48u,
              "Engine tracker health layout must be contiguous");
static_assert(RMP_OFFSETOF(StateTrackerSlice, happyCnt) == RMP_OFFSETOF(StateTrackerSlice, happySum) + sizeof(float[TRACKER_HAPPY_CAUSE_COUNT]),
              "StateTrackerSlice happy block must be contiguous");
static_assert(WORLD_TRACKER_HAPPY_CUR_CNT_OFF == WORLD_TRACKER_HAPPY_CUR_SUM_OFF + 92u &&
              WORLD_TRACKER_HAPPY_OLD_CNT_OFF == WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 92u,
              "Engine tracker happy layout must be contiguous");

static_assert(STAT_RES_VEC_BASE_OFF + STAT_RES_VEC_COUNT * STAT_RES_VEC_STRIDE == STAT_VEHICLES_OFF,
              "Engine resVecHeaders (0x008, 384B) and vehicles (0x188, 16B) must be contiguous 400B block");
static_assert(RMP_OFFSETOF(StatRecordUiBackup, scalars) + RMP_OFFSETOF(StateEconScalars, vehicles) ==
              RMP_OFFSETOF(StatRecordUiBackup, resVecHeaders) + sizeof(StatRecordUiBackup::resVecHeaders),
              "StatRecordUiBackup resVecHeaders and vehicles must be contiguous");

struct StatScalarBlockDesc {
    size_t recOff;
    size_t structOff;
    size_t bytes;
};

static constexpr StatScalarBlockDesc kStatScalarBlocks[3] = {
    { STAT_VEHICLES_OFF,         RMP_OFFSETOF(StateEconScalars, vehicles),        sizeof(StateEconScalars::vehicles) },
    { STAT_IMMIGRANT_SPEND_OFF,  RMP_OFFSETOF(StateEconScalars, immigrantSpend),  sizeof(StateEconScalars::immigrantSpend) + sizeof(StateEconScalars::citizenCounts) + sizeof(StateEconScalars::satisfaction) },
    { STAT_CITIZEN_AVERAGES_OFF, RMP_OFFSETOF(StateEconScalars, citizenAverages), sizeof(StateEconScalars::citizenAverages) + sizeof(StateEconScalars::demographics) + sizeof(StateEconScalars::citizenBrackets) + sizeof(StateEconScalars::extraCounts) + sizeof(StateEconScalars::tourism) + sizeof(StateEconScalars::loans) }
};

static constexpr StatScalarBlockDesc kUiHistBlocks[3] = {
    { STAT_RES_VEC_BASE_OFF,     RMP_OFFSETOF(StatRecordUiBackup, resVecHeaders),
      sizeof(StatRecordUiBackup::resVecHeaders) + sizeof(StateEconScalars::vehicles) },
    { STAT_IMMIGRANT_SPEND_OFF,  RMP_OFFSETOF(StatRecordUiBackup, scalars) + RMP_OFFSETOF(StateEconScalars, immigrantSpend),
      sizeof(StateEconScalars::immigrantSpend) + sizeof(StateEconScalars::citizenCounts) + sizeof(StateEconScalars::satisfaction) },
    { STAT_CITIZEN_AVERAGES_OFF, RMP_OFFSETOF(StatRecordUiBackup, scalars) + RMP_OFFSETOF(StateEconScalars, citizenAverages),
      sizeof(StateEconScalars::citizenAverages) + sizeof(StateEconScalars::demographics) + sizeof(StateEconScalars::citizenBrackets) + sizeof(StateEconScalars::extraCounts) + sizeof(StateEconScalars::tourism) + sizeof(StateEconScalars::loans) }
};

static inline void CopyStatBlocksFromRecRaw(const BYTE* rec, void* outStruct, const StatScalarBlockDesc* blocks)
{
    BYTE* dstBase = reinterpret_cast<BYTE*>(outStruct);
    for (int i = 0; i < 3; ++i) {
        memcpy(dstBase + blocks[i].structOff,
               rec + blocks[i].recOff,
               blocks[i].bytes);
    }
}

static inline uint8_t ComputeStatRecordDirtyMask(const StatRecordUiBackup& a, const StatRecordUiBackup& b)
{
    const BYTE* baseA = reinterpret_cast<const BYTE*>(&a);
    const BYTE* baseB = reinterpret_cast<const BYTE*>(&b);
    uint8_t mask = 0;
    for (int i = 0; i < 3; ++i) {
        if (memcmp(baseA + kUiHistBlocks[i].structOff,
                   baseB + kUiHistBlocks[i].structOff,
                   kUiHistBlocks[i].bytes) != 0) {
            mask |= (uint8_t)(1u << i);
        }
    }
    return mask;
}

static inline void WriteStatRecordDirtyBlocksRaw(BYTE* rec, const StatRecordUiBackup& src, uint8_t dirtyMask)
{
    const BYTE* srcBase = reinterpret_cast<const BYTE*>(&src);
    for (int i = 0; i < 3; ++i) {
        if (dirtyMask & (uint8_t)(1u << i)) {
            memcpy(rec + kUiHistBlocks[i].recOff,
                   srcBase + kUiHistBlocks[i].structOff,
                   kUiHistBlocks[i].bytes);
        }
    }
}

static inline void WriteStatScalarsToRecRaw(BYTE* rec, const StateEconScalars& src)
{
    const BYTE* srcBase = reinterpret_cast<const BYTE*>(&src);
    for (int i = 0; i < 3; ++i) {
        memcpy(rec + kStatScalarBlocks[i].recOff,
               srcBase + kStatScalarBlocks[i].structOff,
               kStatScalarBlocks[i].bytes);
    }
}

static bool SafeSehReadStruct(const void* src, void* dst, size_t dstBytes, bool flag,
                              void (*copyFn)(const void*, void*, bool))
{
    if (!dst) return false;
    if (!src || !copyFn) {
        memset(dst, 0, dstBytes);
        return false;
    }
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        copyFn(src, dst, flag);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(dst, 0, dstBytes);
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void CopyStatScalarsCallback(const void* src, void* dst, bool /*unused*/)
{
    CopyStatBlocksFromRecRaw(static_cast<const BYTE*>(src), dst, kStatScalarBlocks);
}

void SafeReadStatScalars(const BYTE* rec, StateEconScalars* out)
{
    SafeSehReadStruct(rec, out, sizeof(*out), false, &CopyStatScalarsCallback);
}

void SafeWriteStatScalars(BYTE* rec, const StateEconScalars& src)
{
    if (!rec) return;
    DiagLog_BeginSafeSeh();
    __try {
        WriteStatScalarsToRecRaw(rec, src);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void AddFloatSliceDelta(float* dst, const float* before, const float* after, int count, float scale)
{
    for (int i = 0; i < count; ++i) {
        float d = (after[i] - before[i]) * scale;
        if (isfinite(d) && fabsf(d) <= 1.0e9f) dst[i] += d;
    }
}

static void AddIntSliceDelta(int32_t* dst, const int32_t* before, const int32_t* after, int count, float scale)
{
    for (int i = 0; i < count; ++i) {
        int32_t d = after[i] - before[i];
        if (d != 0) dst[i] += (int32_t)lroundf((float)d * scale);
    }
}

static void AddExtraCountsSliceDelta(int32_t* dst, const int32_t* before, const int32_t* after, float scale)
{
    for (int i = 0; i < 21; ++i) {
        // Indices 10 and 11 (rec + 0x314 / 0x318, $Tourism_ToursitScoreUSD and $Tourism_ToursitScoreRUB)
        // are accumulated in SOVIET64.exe (0x836420 / 0x836455) as IEEE-754 float rating sums, not int32_t!
        if (i == 10 || i == 11) {
            float bf = 0.0f, af = 0.0f, df = 0.0f;
            memcpy(&bf, &before[i], sizeof(float));
            memcpy(&af, &after[i], sizeof(float));
            memcpy(&df, &dst[i], sizeof(float));
            if (!isfinite(bf)) bf = 0.0f;
            if (!isfinite(af)) af = 0.0f;
            if (!isfinite(df)) df = 0.0f;
            float fScale = scale;
            if (scale > 0.0f && scale < 1.0f) {
                int cntIdx = (i == 10) ? 6 : 7;
                int32_t dCnt = after[cntIdx] - before[cntIdx];
                if (dCnt != 0) {
                    int32_t sCnt = (int32_t)lroundf((float)dCnt * scale);
                    fScale = (float)sCnt / (float)dCnt;
                }
            }
            float d = (af - bf) * fScale;
            if (isfinite(d) && fabsf(d) <= 1.0e9f) {
                df += d;
                memcpy(&dst[i], &df, sizeof(float));
            }
        } else {
            int32_t d = after[i] - before[i];
            if (d != 0) dst[i] += (int32_t)lroundf((float)d * scale);
        }
    }
}

void AddStatScalarsDelta(StateEconScalars* dst, const StateEconScalars& before,
                         const StateEconScalars& after, float scale)
{
    if (!dst) return;
    AddFloatSliceDelta(dst->vehicles,       before.vehicles,       after.vehicles,       4,  scale);
    AddFloatSliceDelta(dst->immigrantSpend, before.immigrantSpend, after.immigrantSpend, 2,  scale);
    AddIntSliceDelta(dst->citizenCounts,    before.citizenCounts,  after.citizenCounts,  5,  scale);
    AddExtraCountsSliceDelta(dst->extraCounts, before.extraCounts, after.extraCounts,    scale);
    AddFloatSliceDelta(dst->tourism,        before.tourism,        after.tourism,        44, scale);
    AddFloatSliceDelta(dst->loans,          before.loans,          after.loans,          4,  scale);
}

static inline void CopyTrackerSliceRaw(const BYTE* world, bool readOld, StateTrackerSlice* out)
{
    const size_t hSumOff = readOld ? WORLD_TRACKER_HEALTH_OLD_SUM_OFF : WORLD_TRACKER_HEALTH_CUR_SUM_OFF;
    const size_t hCntOff = readOld ? WORLD_TRACKER_HEALTH_OLD_CNT_OFF : WORLD_TRACKER_HEALTH_CUR_CNT_OFF;
    const size_t pSumOff = readOld ? WORLD_TRACKER_HAPPY_OLD_SUM_OFF  : WORLD_TRACKER_HAPPY_CUR_SUM_OFF;
    const size_t pCntOff = readOld ? WORLD_TRACKER_HAPPY_OLD_CNT_OFF  : WORLD_TRACKER_HAPPY_CUR_CNT_OFF;
    memcpy(out->healthSum, world + hSumOff, sizeof(out->healthSum));
    memcpy(out->healthCnt, world + hCntOff, sizeof(out->healthCnt));
    memcpy(out->happySum,  world + pSumOff, sizeof(out->happySum));
    memcpy(out->happyCnt,  world + pCntOff, sizeof(out->happyCnt));
}

static inline void WriteTrackerSliceRaw(BYTE* world, bool writeOld, const StateTrackerSlice& src)
{
    const size_t hSumOff = writeOld ? WORLD_TRACKER_HEALTH_OLD_SUM_OFF : WORLD_TRACKER_HEALTH_CUR_SUM_OFF;
    const size_t hCntOff = writeOld ? WORLD_TRACKER_HEALTH_OLD_CNT_OFF : WORLD_TRACKER_HEALTH_CUR_CNT_OFF;
    const size_t pSumOff = writeOld ? WORLD_TRACKER_HAPPY_OLD_SUM_OFF  : WORLD_TRACKER_HAPPY_CUR_SUM_OFF;
    const size_t pCntOff = writeOld ? WORLD_TRACKER_HAPPY_OLD_CNT_OFF  : WORLD_TRACKER_HAPPY_CUR_CNT_OFF;
    memcpy(world + hSumOff, src.healthSum, sizeof(src.healthSum));
    memcpy(world + hCntOff, src.healthCnt, sizeof(src.healthCnt));
    memcpy(world + pSumOff, src.happySum,  sizeof(src.happySum));
    memcpy(world + pCntOff, src.happyCnt,  sizeof(src.happyCnt));
}

static void CopyTrackerSliceCallback(const void* src, void* dst, bool readOld)
{
    CopyTrackerSliceRaw(static_cast<const BYTE*>(src), readOld, static_cast<StateTrackerSlice*>(dst));
}

void SafeReadTrackerSlice(const BYTE* world, bool readOld, StateTrackerSlice* out)
{
    SafeSehReadStruct(world, out, sizeof(*out), readOld, &CopyTrackerSliceCallback);
}

static void SafeWriteTrackerSlice(BYTE* world, bool writeOld, const StateTrackerSlice& src)
{
    if (!world) return;
    DiagLog_BeginSafeSeh();
    __try {
        WriteTrackerSliceRaw(world, writeOld, src);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void AddTrackerSliceDelta(StateTrackerSlice* dst, const StateTrackerSlice& before,
                          const StateTrackerSlice& after, float scale)
{
    if (!dst || !isfinite(scale) || scale <= 0.0f) return;
    for (int i = 0; i < TRACKER_HEALTH_CAUSE_COUNT; ++i) {
        float ds = after.healthSum[i] - before.healthSum[i];
        if (isfinite(ds) && ds != 0.0f) dst->healthSum[i] += ds * scale;
        if (dst->healthSum[i] < 0.0f) dst->healthSum[i] = 0.0f;
        int32_t dc = after.healthCnt[i] - before.healthCnt[i];
        if (dc != 0) dst->healthCnt[i] += (int32_t)lroundf((float)dc * scale);
        if (dst->healthCnt[i] < 0) dst->healthCnt[i] = 0;
    }
    for (int i = 0; i < TRACKER_HAPPY_CAUSE_COUNT; ++i) {
        float ds = after.happySum[i] - before.happySum[i];
        if (isfinite(ds) && ds != 0.0f) dst->happySum[i] += ds * scale;
        if (dst->happySum[i] < 0.0f) dst->happySum[i] = 0.0f;
        int32_t dc = after.happyCnt[i] - before.happyCnt[i];
        if (dc != 0) dst->happyCnt[i] += (int32_t)lroundf((float)dc * scale);
        if (dst->happyCnt[i] < 0) dst->happyCnt[i] = 0;
    }
}

void ClearStateEconPeriod(StateEconPeriodStats* st, int32_t day, int32_t year)
{
    if (!st) return;
    st->day = day;
    st->year = year;
    for (int v = 0; v < STAT_RES_VEC_COUNT; ++v) {
        st->resVecs[v].clear();
    }
    memset(&st->scalars, 0, sizeof(st->scalars));
}

static bool SafeReadResVecEntries(const BYTE* vecAddr, std::vector<SovietStatResEntry>& outEntries)
{
    outEntries.clear();
    if (!vecAddr) return false;
    RawStatResVecHeader hdr = {};
    if (!SafeReadPod((const RawStatResVecHeader*)vecAddr, &hdr)) return false;
    if (!hdr.begin || hdr.end < hdr.begin || hdr.cap < hdr.end) {
        return hdr.begin == nullptr && hdr.end == nullptr;
    }
    ptrdiff_t byteLen = (const BYTE*)hdr.end - (const BYTE*)hdr.begin;
    if (byteLen < 0 || byteLen % (ptrdiff_t)sizeof(SovietStatResEntry) != 0 ||
        byteLen > 512 * (ptrdiff_t)sizeof(SovietStatResEntry)) {
        return false;
    }
    int count = (int)(byteLen / (ptrdiff_t)sizeof(SovietStatResEntry));
    if (count <= 0) return true;
    outEntries.resize((size_t)count);
    if (!SafeMemcpyPod(outEntries.data(), hdr.begin, (size_t)count * sizeof(SovietStatResEntry))) {
        outEntries.clear();
        return false;
    }
    return true;
}

void AddEntryDeltaToStateResVec(std::vector<SovietStatResEntry>& vec, uintptr_t resPtr,
                                       bool isNegativeRow, float dTons, float dVal)
{
    if (!isfinite(dTons)) dTons = 0.0f;
    if (!isfinite(dVal))  dVal  = 0.0f;
    if (fabsf(dTons) <= 1e-7f && fabsf(dVal) <= 1e-7f) return;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i].resourcePtr == resPtr) {
            bool curNeg = (vec[i].amountTons < 0.0f);
            if (curNeg == isNegativeRow || fabsf(vec[i].amountTons) <= 1e-7f) {
                vec[i].amountTons += dTons;
                vec[i].moneyValue += dVal;
                return;
            }
        }
    }
    SovietStatResEntry entry = { resPtr, dTons, dVal };
    vec.push_back(entry);
}

bool SafeReadStatHistHeader(const BYTE* world, BYTE** outBegin, int* outCount)
{
    if (!world || !outBegin || !outCount) return false;
    *outBegin = nullptr;
    *outCount = 0;
    uintptr_t ptrs[3] = {};
    if (!SafeMemcpyPod(ptrs, world + WORLD_STAT_HISTORY_OFFSET, sizeof(ptrs))) return false;
    BYTE* b = (BYTE*)ptrs[0];
    BYTE* e = (BYTE*)ptrs[1];
    BYTE* c = (BYTE*)ptrs[2];
    if (!b || e < b || c < e) {
        return b == nullptr && e == nullptr;
    }
    ptrdiff_t byteLen = e - b;
    if (byteLen < 0 || (byteLen % STAT_RECORD_SIZE) != 0 || byteLen > 4096 * STAT_RECORD_SIZE) {
        return false;
    }
    *outBegin = b;
    *outCount = (int)(byteLen / STAT_RECORD_SIZE);
    return true;
}

static void GetFullCitizenVectorRange(const BYTE* world, const BYTE* const** outBegin, const BYTE* const** outEnd)
{
    if (!world || !outBegin || !outEnd) return;
    if (g_uiCitVecSwapped && g_exeBase && world == g_exeBase + WORLD_RVA) {
        *outBegin = (const BYTE* const*)g_uiSavedCitVecHdr[0];
        *outEnd   = (const BYTE* const*)g_uiSavedCitVecHdr[1];
    } else {
        *outBegin = *(const BYTE* const* const*)(world + WORLD_CITIZENS_OFFSET);
        *outEnd   = *(const BYTE* const* const*)(world + WORLD_CITIZENS_OFFSET + 8);
    }
}

struct DemoStateAccum {
    float satSum[10];
    float prodSum;
    float lifespanSum;
    float ageSum;
    int   totalCounted;
};

static inline void ResetDemoOutputScalars(StateEconScalars* s)
{
    if (!s) return;
    memset(s->satisfaction, 0, sizeof(s->satisfaction));
    memset(s->citizenAverages, 0, sizeof(s->citizenAverages));
    memset(s->demographics, 0, sizeof(s->demographics));
    memset(s->citizenBrackets, 0, sizeof(s->citizenBrackets));
}

static float ComputeCitizenProductivity(const BYTE* cit, const BYTE* home)
{
    float d8 = *(const float*)(cit + 0xD8);
    float dc = *(const float*)(cit + 0xDC);
    float e0 = *(const float*)(cit + 0xE0);
    float e4 = *(const float*)(cit + 0xE4);
    if (!isfinite(d8)) d8 = 0.0f;
    if (!isfinite(dc)) dc = 0.0f;
    if (!isfinite(e0)) e0 = 0.0f;
    if (!isfinite(e4)) e4 = 0.0f;

    const BYTE* bldType = home ? *(const BYTE* const*)(home + 0x318) : nullptr;
    float prod = 0.0f;
    if (bldType && *(const int32_t*)(bldType + 0x360) == 0x4C) {
        prod = d8 * 0.4f + 0.6f;
    } else {
        float term1 = dc * 0.3f + 0.1f + e0 * 0.15f + (d8 - 0.55f) * 1.5f;
        float term2 = dc * 0.3f + 0.05f + e0 * 0.35f + d8 * 0.3f;
        float factor = (e4 - 0.5f) * 2.0f * 0.35f + 1.0f;
        prod = (term1 + term2) * 0.5f * factor;
    }
    return (prod < 0.3f) ? 0.3f : prod;
}

static void AccumulateAdultCitizenBrackets(const BYTE* cit, const BYTE* home, int32_t ioBrackets[9], float* ioProdSum)
{
    *ioProdSum += ComputeCitizenProductivity(cit, home);
    if (*(const BYTE*)(cit + 0x2D) != 0) {
        const BYTE* bldType = home ? *(const BYTE* const*)(home + 0x318) : nullptr;
        if (!bldType || (*(const int32_t*)(bldType + 0x360) == 2 && *(const int32_t*)(bldType + 0x364) != 1)) {
            ioBrackets[0]++;
        }
    }
    float elec = *(const float*)(cit + 0xFC);
    if (!isfinite(elec) || elec < 1.0f) {
        ioBrackets[4]++;
    } else {
        ioBrackets[5]++;
        if (elec >= 2.0f) ioBrackets[6]++;
        if (elec >= 3.0f) ioBrackets[7]++;
    }
}

static void AccumulateAllCitizenBrackets(const BYTE* cit, int32_t ioBrackets[9])
{
    float edu = *(const float*)(cit + 0xA8);
    if (!isfinite(edu) || edu < 1.0f) ioBrackets[1]++;
    else if (edu < 2.0f) ioBrackets[2]++;
    else ioBrackets[3]++;

    if (*(const uintptr_t*)(cit + 0x688) != 0) {
        ioBrackets[8]++;
    }
}

static inline void AccumulateCitizenIntoDemoState(const BYTE* cit, const BYTE* home, const BYTE* parent,
                                                  StateEconScalars* dst, DemoStateAccum* acc)
{
    acc->totalCounted++;
    for (int i = 0; i < 10; ++i) {
        float v = *(const float*)(cit + 0xD8 + (size_t)i * sizeof(float));
        if (isfinite(v)) acc->satSum[i] += v;
    }
    float lifespan = *(const float*)(cit + 0x18);
    if (!isfinite(lifespan) || lifespan <= 0.0f) lifespan = 70.0f;
    acc->lifespanSum += lifespan;

    float age = *(const float*)(cit + CIT_AGE_OFFSET);
    if (!isfinite(age) || age < 0.0f) age = 0.0f;
    acc->ageSum += age;

    if ((age > 0.0f && age < 6.0f) || (parent && age < 6.0f)) {
        dst->demographics[0]++;
    } else if ((age > 0.0f && age < 21.0f) || (parent && age < 21.0f)) {
        dst->demographics[1]++;
    } else if (parent) {
        dst->demographics[2]++;
    } else {
        dst->demographics[3]++;
        AccumulateAdultCitizenBrackets(cit, home, dst->citizenBrackets, &acc->prodSum);
    }
    AccumulateAllCitizenBrackets(cit, dst->citizenBrackets);
}

static inline void FinalizeDemoOutputScalars(StateEconScalars* dst, const DemoStateAccum& acc)
{
    if (!dst || acc.totalCounted <= 0) return;
    const float invTotal = 1.0f / (float)acc.totalCounted;
    for (int i = 0; i < 10; ++i) {
        dst->satisfaction[i] = acc.satSum[i] * invTotal;
    }
    const int32_t adults = dst->demographics[3];
    dst->citizenAverages[0] = (adults > 0) ? (acc.prodSum / (float)adults) : 0.0f;
    dst->citizenAverages[1] = acc.lifespanSum * invTotal;
    dst->citizenAverages[2] = acc.ageSum * invTotal;
}

static inline void CopyDemoFieldsToScalars(StateEconScalars* dst, const StateEconScalars& src)
{
    memcpy(dst->satisfaction,    src.satisfaction,    sizeof(src.satisfaction));
    memcpy(dst->citizenAverages, src.citizenAverages, sizeof(src.citizenAverages));
    memcpy(dst->demographics,    src.demographics,    sizeof(src.demographics));
    memcpy(dst->citizenBrackets, src.citizenBrackets, sizeof(src.citizenBrackets));
}

static void PrepareStatRecordUiPayload(StateEconPeriodStats& st, StatRecordUiBackup* outPayload)
{
    if (!outPayload) return;
    for (int v = 0; v < STAT_RES_VEC_COUNT; ++v) {
        std::vector<SovietStatResEntry>& sv = st.resVecs[v];
        SovietStatResEntry* p = sv.empty() ? nullptr : sv.data();
        outPayload->resVecHeaders[v].begin = p;
        outPayload->resVecHeaders[v].end   = p ? (p + sv.size()) : nullptr;
        outPayload->resVecHeaders[v].cap   = outPayload->resVecHeaders[v].end;
    }
    outPayload->scalars = st.scalars;
}

static void PopulateHudPopBlockFromScalars(BYTE* outBlock, const StateEconScalars& sc, int localPop)
{
    if (!outBlock) return;
    memset(outBlock, 0, WORLD_HUD_POP_BLOCK_SIZE);
    float fPop = (float)localPop;
    int32_t adults = sc.demographics[3];
    int32_t dependents = sc.demographics[0] + sc.demographics[1] + sc.demographics[2];
    *(float*)(outBlock + 0x00)   = fPop;
    *(float*)(outBlock + 0x04)   = (adults > 0) ? ((float)sc.citizenBrackets[0] / (float)adults) : 0.0f;
    *(float*)(outBlock + 0x08)   = sc.satisfaction[2];
    *(float*)(outBlock + 0x0C)   = sc.satisfaction[0];
    *(float*)(outBlock + 0x10)   = sc.satisfaction[1];
    *(float*)(outBlock + 0x14)   = 0.0f;
    *(int32_t*)(outBlock + 0x18) = (int32_t)localPop;
    *(int32_t*)(outBlock + 0x24) = adults;
    *(int32_t*)(outBlock + 0x28) = sc.citizenBrackets[1];
    *(int32_t*)(outBlock + 0x2C) = sc.citizenBrackets[2];
    *(int32_t*)(outBlock + 0x30) = sc.citizenBrackets[3];
    *(int32_t*)(outBlock + 0x34) = dependents;
    *(int32_t*)(outBlock + 0x38) = sc.demographics[0];
    *(int32_t*)(outBlock + 0x3C) = sc.demographics[1];
    *(int32_t*)(outBlock + 0x44) = sc.demographics[2];
    *(int32_t*)(outBlock + 0x58) = (int32_t)localPop;
    memcpy(outBlock + 0x5C, sc.citizenBrackets, sizeof(sc.citizenBrackets));
    memcpy(outBlock + 0x80, sc.demographics, sizeof(sc.demographics));
    for (int i = 0; i < 8; ++i) {
        *(float*)(outBlock + 0x90 + (size_t)i * sizeof(float)) = sc.satisfaction[i + 1] * fPop;
    }
    *(float*)(outBlock + 0xB0)   = (float)sc.citizenBrackets[6];
    *(float*)(outBlock + 0xB4)   = (float)sc.citizenBrackets[5];
    *(float*)(outBlock + 0xB8)   = sc.citizenAverages[1] * fPop;
    *(float*)(outBlock + 0xBC)   = sc.satisfaction[0] * fPop;
}

static bool SaveUiCitizenLifespansSeh(const BYTE* const* cits, size_t count,
                                      SavedCitizenLifespan* outSaved, int* outCount)
{
    *outCount = 0;
    if (!cits || count == 0 || !outSaved) return true;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        int n = 0;
        for (size_t i = 0; i < count; ++i) {
            BYTE* c = const_cast<BYTE*>(cits[i]);
            if (c) {
                outSaved[n].cit = c;
                outSaved[n].lifespan = *(const float*)(c + 0x18);
                n++;
            }
        }
        *outCount = n;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCount = 0;
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void SaveUiCitizenLifespans(const std::vector<BYTE*>& localCits, std::vector<SavedCitizenLifespan>& outSaved)
{
    if (localCits.empty()) {
        outSaved.clear();
        return;
    }
    outSaved.resize(localCits.size());
    int savedCount = 0;
    if (SaveUiCitizenLifespansSeh(localCits.data(), localCits.size(), outSaved.data(), &savedCount)) {
        outSaved.resize((size_t)savedCount);
    } else {
        outSaved.clear();
    }
}

static void RestoreUiCitizenLifespanListSeh(std::vector<SavedCitizenLifespan>& saved)
{
    if (saved.empty()) return;
    const SavedCitizenLifespan* b = saved.data();
    const SavedCitizenLifespan* e = b + saved.size();
    DiagLog_BeginSafeSeh();
    __try {
        for (const SavedCitizenLifespan* it = b; it < e; ++it) {
            if (it->cit) {
                float cur = *(const float*)(it->cit + 0x18);
                if (memcmp(&cur, &it->lifespan, sizeof(float)) != 0) {
                    *(float*)(it->cit + 0x18) = it->lifespan;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    saved.clear();
}

static inline bool IsCitizenDormStudentRaw(const BYTE* holder, const BYTE* holderHome)
{
    if (*(const BYTE*)(holder + 0x2D) != 1) return false;
    const BYTE* bldType = holderHome ? *(const BYTE* const*)(holderHome + 0x318) : nullptr;
    return bldType && *(const BYTE*)(bldType + 0x8E8) == 1;
}

static inline bool CitOwnerMatchesHolderHome(int primeLocalState, const BYTE* home,
                                             const BYTE* parent, const BYTE* holderHome)
{
    if (primeLocalState == 0 || !holderHome) return false;
    if (home && IsPointerLikeX64((uintptr_t)home)) return holderHome == home;
    return parent != nullptr;
}

static bool ComputeCitizenDemographicsCoreSeh(
    const BYTE* const* citBegin, const BYTE* const* citEnd,
    int primeLocalState, BYTE** outLocalCits, SavedCitizenLifespan* outLifespans,
    int* outLocalCitCount, int* outLocalPop, int* outPop1, int* outPop2,
    StateEconScalars* outS0, StateEconScalars* outS1, DemoStateAccum acc[2])
{
    if (outLocalCitCount) *outLocalCitCount = 0;
    if (outLocalPop) *outLocalPop = 0;
    if (outPop1) *outPop1 = 0;
    if (outPop2) *outPop2 = 0;
    if (!citBegin || citEnd <= citBegin) return true;

    const BldPtrOwnerSlot* slots = nullptr;
    size_t mask = 0;
    TwoStates_GetActiveBldPtrCacheView(&slots, &mask);

    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        StateEconScalars* outs[2] = { outS0, outS1 };
        int localCount = 0;
        int localPop = 0;
        int pop1 = 0;
        int pop2 = 0;
        for (const BYTE* const* it = citBegin; it < citEnd; ++it) {
            BYTE* cit = const_cast<BYTE*>(*it);
            if (!cit) continue;
            const BYTE* home   = *(const BYTE* const*)(cit + CIT_HOME_BUILDING_OFFSET);
            const BYTE* parent = *(const BYTE* const*)(cit + CIT_PARENT_OFFSET);

            int citOwner = 0;
            if (primeLocalState != 0) {
                citOwner = ResolveCitizenOwnerRawUnsafe(cit, home, parent, slots, mask);
                if (citOwner == primeLocalState) {
                    if (outLocalCits) outLocalCits[localCount] = cit;
                    if (outLifespans) {
                        outLifespans[localCount].cit = cit;
                        outLifespans[localCount].lifespan = *(const float*)(cit + 0x18);
                    }
                    localCount++;
                }
            }

            if (*(const int32_t*)(cit + CIT_INFANT_FLAG_OFFSET) != 0) continue;
            if (*(const int32_t*)(cit + CIT_UNBORN_FLAG_OFFSET) != 0) continue;

            const BYTE* holder = parent ? parent : cit;
            const BYTE* holderHome = (holder == cit)
                ? home
                : *(const BYTE* const*)(holder + CIT_HOME_BUILDING_OFFSET);

            int holderOwner = 0;
            if (holderHome && IsPointerLikeX64((uintptr_t)holderHome)) {
                holderOwner = CitOwnerMatchesHolderHome(primeLocalState, home, parent, holderHome)
                    ? citOwner
                    : LookupBldPtrOwnerInSlots(slots, mask, holderHome);
                if (holderOwner == 1) pop1++;
                else if (holderOwner == 2) pop2++;
            }

            if (IsCitizenDormStudentRaw(holder, holderHome)) continue;

            if (primeLocalState != 0 && citOwner == primeLocalState) {
                localPop++;
            }
            if ((holderOwner == 1 || holderOwner == 2) && outs[holderOwner - 1]) {
                AccumulateCitizenIntoDemoState(cit, holderHome, parent, outs[holderOwner - 1], &acc[holderOwner - 1]);
            }
        }
        if (outLocalCitCount) *outLocalCitCount = localCount;
        if (outLocalPop) *outLocalPop = localPop;
        if (outPop1) *outPop1 = pop1;
        if (outPop2) *outPop2 = pop2;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outLocalCitCount) *outLocalCitCount = 0;
        if (outLocalPop) *outLocalPop = 0;
        if (outPop1) *outPop1 = 0;
        if (outPop2) *outPop2 = 0;
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool PrepareUiCitizensAndDemographicsFromHdr(BYTE* world, int localState, const uintptr_t curHdr[3],
                                                    int* outLocalPop, bool skipLifespanSave,
                                                    uintptr_t outNewCitHdr[3])
{
    *outLocalPop = 0;
    outNewCitHdr[0] = outNewCitHdr[1] = outNewCitHdr[2] = 0;
    if (!world || (localState != 1 && localState != 2)) return false;

    const BYTE* const* b = (const BYTE* const*)curHdr[0];
    const BYTE* const* e = (const BYTE* const*)curHdr[1];
    const BYTE* const* c = (const BYTE* const*)curHdr[2];
    if (b && (e < b || c < e || (e - b) > 2000000)) {
        g_uiCitVecSwapped = false;
        g_uiSavedCitizenLifespans.clear();
        return false;
    }

    const int sIdx = localState - 1;
    const ptrdiff_t curCount = b ? (e - b) : 0;
    const bool sessionActive = Determinism_IsActive() && Determinism_SessionRunning();
    const unsigned long long stepCount  = sessionActive ? Determinism_StepCount() : 0;
    const unsigned long long applyEpoch = sessionActive ? Commands_ApplyEpoch() : 0;
    const uint64_t ownerGen = g_ownerMapGen.load(std::memory_order_acquire);
    const uint64_t vehOwnerGen = g_vehOwnerMapGen.load(std::memory_order_acquire);

    const bool cacheHit = sessionActive && g_uiCitizenCache.valid &&
        g_uiCitizenCache.world == world &&
        g_uiCitizenCache.stepCount == stepCount &&
        g_uiCitizenCache.applyEpoch == applyEpoch &&
        g_uiCitizenCache.localState == localState &&
        g_uiCitizenCache.citVecBegin == (const BYTE*)b &&
        g_uiCitizenCache.citVecCount == curCount &&
        g_uiCitizenCache.ownerMapGen == ownerGen &&
        g_uiCitizenCache.vehOwnerMapGen == vehOwnerGen;

    bool scanOk = true;
    if (cacheHit) {
        CopyDemoFieldsToScalars(&g_stateCurEcon[sIdx].scalars, g_uiCitizenCache.demoScalars);
        *outLocalPop = g_uiCitizenCache.localPop;
        memcpy(g_uiSavedCitVecHdr, curHdr, sizeof(g_uiSavedCitVecHdr));
        if (skipLifespanSave) {
            g_uiSavedCitizenLifespans.clear();
        } else {
            SaveUiCitizenLifespans(g_uiLocalCitizens, g_uiSavedCitizenLifespans);
        }
    } else {
        EnsureBldPtrOwnerCache(world);
        memcpy(g_uiSavedCitVecHdr, curHdr, sizeof(g_uiSavedCitVecHdr));
        g_uiLocalCitizens.resize((size_t)curCount);
        if (skipLifespanSave) {
            g_uiSavedCitizenLifespans.clear();
        } else {
            g_uiSavedCitizenLifespans.resize((size_t)curCount);
        }
        ResetDemoOutputScalars(&g_stateCurEcon[sIdx].scalars);
        DemoStateAccum acc[2] = {};
        int localCount = 0;
        int localPop = 0;
        scanOk = ComputeCitizenDemographicsCoreSeh(
            b, e, localState,
            g_uiLocalCitizens.empty() ? nullptr : g_uiLocalCitizens.data(),
            (skipLifespanSave || g_uiSavedCitizenLifespans.empty()) ? nullptr : g_uiSavedCitizenLifespans.data(),
            &localCount, &localPop, nullptr, nullptr,
            (sIdx == 0) ? &g_stateCurEcon[0].scalars : nullptr,
            (sIdx == 1) ? &g_stateCurEcon[1].scalars : nullptr,
            acc);
        if (!scanOk) {
            ResetDemoOutputScalars(&g_stateCurEcon[sIdx].scalars);
            g_uiLocalCitizens.clear();
            g_uiSavedCitizenLifespans.clear();
            localPop = 0;
        } else {
            g_uiLocalCitizens.resize((size_t)localCount);
            if (!skipLifespanSave) {
                g_uiSavedCitizenLifespans.resize((size_t)localCount);
            }
            FinalizeDemoOutputScalars(&g_stateCurEcon[sIdx].scalars, acc[sIdx]);
        }
        *outLocalPop = localPop;
        g_uiCitizenCacheBuildCount++;
    }

    BYTE** fBegin = g_uiLocalCitizens.empty() ? nullptr : g_uiLocalCitizens.data();
    outNewCitHdr[0] = (uintptr_t)fBegin;
    outNewCitHdr[1] = (uintptr_t)(fBegin ? (fBegin + g_uiLocalCitizens.size()) : nullptr);
    outNewCitHdr[2] = (uintptr_t)(fBegin ? (fBegin + g_uiLocalCitizens.size()) : nullptr);

    if (!cacheHit) {
        if (sessionActive && scanOk) {
            g_uiCitizenCache.valid = true;
            g_uiCitizenCache.world = world;
            g_uiCitizenCache.stepCount = stepCount;
            g_uiCitizenCache.applyEpoch = applyEpoch;
            g_uiCitizenCache.localState = localState;
            g_uiCitizenCache.citVecBegin = (const BYTE*)b;
            g_uiCitizenCache.citVecCount = curCount;
            g_uiCitizenCache.ownerMapGen = ownerGen;
            g_uiCitizenCache.vehOwnerMapGen = vehOwnerGen;
            g_uiCitizenCache.localPop = *outLocalPop;
            CopyDemoFieldsToScalars(&g_uiCitizenCache.demoScalars, g_stateCurEcon[sIdx].scalars);
        } else {
            g_uiCitizenCache.valid = false;
        }
    }
    return true;
}

struct UiEndReadState {
    bool ok;
    int localSelectedAfter;
    SovietLoan* loanBegin;
    int loanCount;
    int loanCapCount;
    double curRub;
    double curUsd;
    bool haveMoney;
};

static void UiEndReadWorldStateSeh(const BYTE* world, bool readLoans, bool readMoney, UiEndReadState* out)
{
    memset(out, 0, sizeof(*out));
    out->localSelectedAfter = -1;
    if (!world) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (readLoans) {
            out->localSelectedAfter = (int)(*(const int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET));
            ParseLoanVectorHeaderRaw(world, &out->loanBegin, &out->loanCount, &out->loanCapCount);
        }
        if (readMoney) {
            out->curRub = *WorldRubles();
            out->curUsd = *WorldDollars();
            out->haveMoney = true;
        }
        out->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->ok = false;
    }
    DiagLog_EndSafeSeh();
}

struct UiEndRestoreArgs {
    bool restoreEcon;
    int swappedState;
    bool wasTrackerSwapped;
    bool wasHudPopSwapped;
    bool wasCitVecSwapped;
    const StatRecordUiBackup* savedCurStat;
    BYTE* expectedHistBegin;
    int savedHistCount;
    const StatRecordUiBackup* savedHistStats;
    int savedHistLen;
    const uint8_t* histDirtyMask;
    bool restoreLoans;
    bool writeLoans;
    const SovietLoan* fullLoans;
    int fullLoanCount;
    bool writeLoanStats;
    const float* savedLoanStats;
    bool writeLoanSelected;
    int restoredSelectedIdx;
    bool writeMoney;
    double restoreRub;
    double restoreUsd;
    bool* outHistHeaderMismatch;
};

static void UiEndRestoreLoansRaw(BYTE* world, const UiEndRestoreArgs* a)
{
    if (a->writeLoans) {
        SovietLoan* b = nullptr;
        int curCount = 0, capCount = 0;
        if (ParseLoanVectorHeaderRaw(world, &b, &curCount, &capCount) &&
            a->fullLoanCount >= 0 && a->fullLoanCount <= capCount &&
            (a->fullLoanCount == 0 || (b && a->fullLoans))) {
            if (a->fullLoanCount > 0 && b != a->fullLoans) {
                memcpy(b, a->fullLoans, (size_t)a->fullLoanCount * sizeof(SovietLoan));
            }
            if (b) {
                *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b + a->fullLoanCount;
            }
        }
    }
    if (a->writeLoanStats && a->savedLoanStats) {
        memcpy(world + WORLD_LOAN_STATS_OFFSET, a->savedLoanStats, 4 * sizeof(float));
    }
    if (a->writeLoanSelected) {
        *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = (int32_t)a->restoredSelectedIdx;
    }
}

static void UiEndRestoreWorldSwapSeh(BYTE* world, const UiEndRestoreArgs* a)
{
    if (!world || !a) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (a->restoreEcon) {
            if (a->wasTrackerSwapped) {
                memcpy(world + WORLD_TRACKER_BLOCK_OFFSET, g_uiSavedTrackerBlock, WORLD_TRACKER_BLOCK_SIZE);
            }
            if (a->wasHudPopSwapped) {
                if (a->swappedState == 1 || a->swappedState == 2) {
                    int sIdx = a->swappedState - 1;
                    memcpy(g_stateHudPopBlock[sIdx], world + WORLD_HUD_POP_BLOCK_OFFSET, WORLD_HUD_POP_BLOCK_SIZE);
                    if (*(const float*)(world + WORLD_HUD_POP_TIMER_OFFSET) > 0.0f) {
                        g_stateHudPopInitialized[sIdx] = true;
                    }
                }
                memcpy(world + WORLD_HUD_POP_BLOCK_OFFSET, g_uiSavedHudPopBlock, WORLD_HUD_POP_BLOCK_SIZE);
                WriteStatScalarsToRecRaw(world + WORLD_POP_STAT_CURRENT_OFF, g_uiSavedPopWinScalars);
            }
            if (a->wasCitVecSwapped) {
                memcpy(world + WORLD_CITIZENS_OFFSET, g_uiSavedCitVecHdr, sizeof(g_uiSavedCitVecHdr));
            }
            if (a->savedCurStat) {
                WriteStatRecordDirtyBlocksRaw(world + WORLD_STAT_CURRENT_OFFSET, *a->savedCurStat, 0x7u);
            }
            if (a->expectedHistBegin && a->savedHistCount > 0 && a->savedHistStats) {
                BYTE* b = *(BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET);
                BYTE* e = *(BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET + 8);
                BYTE* c = *(BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET + 16);
                ptrdiff_t byteLen = (b && e >= b && c >= e) ? (e - b) : -1;
                int curHistCount = (byteLen >= 0 && (byteLen % STAT_RECORD_SIZE) == 0 && byteLen <= 4096 * STAT_RECORD_SIZE)
                    ? (int)(byteLen / STAT_RECORD_SIZE)
                    : 0;
                if (b == a->expectedHistBegin && curHistCount >= a->savedHistCount) {
                    int limit = (a->savedHistCount < a->savedHistLen) ? a->savedHistCount : a->savedHistLen;
                    for (int i = 0; i < limit; ++i) {
                        uint8_t mask = a->histDirtyMask ? a->histDirtyMask[(size_t)i] : 0x7u;
                        if (mask) {
                            WriteStatRecordDirtyBlocksRaw(b + (size_t)i * STAT_RECORD_SIZE, a->savedHistStats[(size_t)i], mask);
                        }
                    }
                } else if (a->outHistHeaderMismatch) {
                    *a->outHistHeaderMismatch = true;
                }
            }
        }
        if (a->restoreLoans) {
            UiEndRestoreLoansRaw(world, a);
        }
        if (a->writeMoney) {
            *WorldRubles()  = a->restoreRub;
            *WorldDollars() = a->restoreUsd;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (a->outHistHeaderMismatch) *a->outHistHeaderMismatch = true;
    }
    DiagLog_EndSafeSeh();
}

static int ResolveRestoredLoanSelection(int localSelectedAfter, bool keepLocalSelection)
{
    if (!keepLocalSelection) return g_uiSavedGlobalSelectedIdx;
    if (!g_uiLoansFilterByState) return localSelectedAfter;
    if (localSelectedAfter >= 0 && (size_t)localSelectedAfter < g_uiFilteredToGlobalIdx.size()) {
        return g_uiFilteredToGlobalIdx[(size_t)localSelectedAfter];
    }
    return -1;
}

static void RestoreUiCitizenLifespansForEnd(bool hadEcon)
{
    if (!hadEcon) {
        g_uiDirtyCitizenLifespans.clear();
        g_uiDirtyCitizenSet.clear();
        g_uiSavedCitizenLifespans.clear();
        return;
    }
    if (!g_uiDirtyCitizenLifespans.empty()) {
        RestoreUiCitizenLifespanListSeh(g_uiDirtyCitizenLifespans);
        g_uiDirtyCitizenSet.clear();
        g_uiSavedCitizenLifespans.clear();
    } else {
        RestoreUiCitizenLifespanListSeh(g_uiSavedCitizenLifespans);
    }
}

struct UiEndLoanEval {
    bool detected = false;
    bool blockedInLobby = false;
    bool submitCmd = false;
    bool applyOfflineCmd = false;
    LsLoanCommand cmd = {};
};

static UiEndLoanEval EvaluateUiLoanRestore(BYTE* world, const UiEndReadState& endRead)
{
    UiEndLoanEval ev = {};
    bool loansOk = true;
    if (endRead.loanCount > 0 && endRead.loanBegin) {
        g_uiAfterLoansScratch.resize((size_t)endRead.loanCount);
        loansOk = SafeMemcpyPod(g_uiAfterLoansScratch.data(), endRead.loanBegin,
                                (size_t)endRead.loanCount * sizeof(SovietLoan));
    } else {
        g_uiAfterLoansScratch.clear();
    }
    if (!loansOk || (g_uiAfterLoansScratch.empty() && g_uiFilteredLoansBefore.empty()) ||
        !DetectUiLoanOperation(g_uiAfterLoansScratch, &ev.cmd)) {
        return ev;
    }
    ev.detected = true;
    ev.cmd.dRub = (endRead.haveMoney ? endRead.curRub : g_uiShownRub) - g_uiShownRub;
    ev.cmd.dUsd = (endRead.haveMoney ? endRead.curUsd : g_uiShownUsd) - g_uiShownUsd;
    float afterStats[4] = {};
    SafeReadLoanStats(world, afterStats);
    for (int i = 0; i < 4; ++i) {
        ev.cmd.dStats[i] = afterStats[i] - g_uiSavedLoanStats[i];
    }
    ev.blockedInLobby = SaveSync_IsWaitingLobbyActive();
    if (!ev.blockedInLobby) {
        if (Commands_Capturing()) ev.submitCmd = true;
        else ev.applyOfflineCmd = true;
    }
    return ev;
}

static void ConfigureUiLoanRestoreArgs(const UiEndReadState& endRead, const UiEndLoanEval& loanEval, UiEndRestoreArgs* ra)
{
    if (loanEval.detected) {
        ra->restoreLoans = true;
        ra->writeLoans = true;
        ra->fullLoans = g_uiFullLoansBackup.empty() ? nullptr : g_uiFullLoansBackup.data();
        ra->fullLoanCount = (int)g_uiFullLoansBackup.size();
        ra->writeLoanStats = true;
        ra->savedLoanStats = g_uiSavedLoanStats;
        ra->writeLoanSelected = true;
        ra->restoredSelectedIdx = ResolveRestoredLoanSelection(endRead.localSelectedAfter, !loanEval.blockedInLobby);
        return;
    }
    if (!g_uiLoansFilterByState) return;
    int restoredSel = ResolveRestoredLoanSelection(endRead.localSelectedAfter, true);
    bool needVecWrite = (!g_uiFullLoansBackup.empty() || endRead.loanCount != 0);
    bool needSelWrite = (restoredSel != endRead.localSelectedAfter);
    if (!needVecWrite && !needSelWrite) return;
    ra->restoreLoans = true;
    ra->writeLoans = needVecWrite;
    ra->fullLoans = g_uiFullLoansBackup.empty() ? nullptr : g_uiFullLoansBackup.data();
    ra->fullLoanCount = (int)g_uiFullLoansBackup.size();
    ra->writeLoanSelected = needSelWrite;
    ra->restoredSelectedIdx = restoredSel;
}

void TwoStates_RestoreUiMoneySwap()
{
    EnsureMoneySwapLock();
    EnterCriticalSection(&g_moneySwapLock);
    const bool hadLoans = g_uiLoansSwapped;
    const bool hadMoney = g_uiMoneySwapped;
    const bool hadEcon  = g_uiEconSwapped;
    if (!hadLoans && !hadMoney && !hadEcon) {
        LeaveCriticalSection(&g_moneySwapLock);
        return;
    }
    g_uiLoansSwapped = false;
    g_uiMoneySwapped = false;
    g_uiEconSwapped  = false;

    RestoreUiCitizenLifespansForEnd(hadEcon);

    const int swappedState = g_uiEconSwappedState;
    const bool wasTracker  = g_uiTrackerSwapped;
    const bool wasHudPop   = g_uiHudPopSwapped;
    const bool wasCitVec   = g_uiCitVecSwapped;
    g_uiEconSwappedState = 0;
    g_uiTrackerSwapped   = false;
    g_uiHudPopSwapped    = false;
    g_uiCitVecSwapped    = false;

    BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
    UiEndReadState endRead = {};
    if (world && (hadLoans || hadMoney)) {
        UiEndReadWorldStateSeh(world, hadLoans, hadMoney, &endRead);
    }

    UiEndLoanEval loanEval = (hadLoans && world && endRead.ok)
        ? EvaluateUiLoanRestore(world, endRead)
        : UiEndLoanEval{};

    if (world && (hadEcon || hadLoans || (hadMoney && endRead.haveMoney))) {
        UiEndRestoreArgs ra = {};
        ra.restoreEcon = hadEcon;
        ra.swappedState = swappedState;
        ra.wasTrackerSwapped = wasTracker;
        ra.wasHudPopSwapped = wasHudPop;
        ra.wasCitVecSwapped = wasCitVec;
        ra.savedCurStat = &g_uiSavedCurStat;
        ra.expectedHistBegin = g_uiSavedHistBegin;
        ra.savedHistCount = g_uiSavedHistCount;
        ra.savedHistStats = g_uiSavedHistStats.empty() ? nullptr : g_uiSavedHistStats.data();
        ra.savedHistLen = (int)g_uiSavedHistStats.size();
        ra.histDirtyMask = (g_uiHistDirtyMask.size() >= g_uiSavedHistStats.size() && !g_uiHistDirtyMask.empty())
            ? g_uiHistDirtyMask.data() : nullptr;

        if (hadLoans) ConfigureUiLoanRestoreArgs(endRead, loanEval, &ra);

        if (hadMoney && (loanEval.detected || endRead.haveMoney)) {
            ra.writeMoney = true;
            ra.restoreRub = loanEval.detected ? g_uiSavedSimRub : (g_uiSavedSimRub + (endRead.curRub - g_uiShownRub));
            ra.restoreUsd = loanEval.detected ? g_uiSavedSimUsd : (g_uiSavedSimUsd + (endRead.curUsd - g_uiShownUsd));
        }

        bool histMismatch = false;
        ra.outHistHeaderMismatch = &histMismatch;
        UiEndRestoreWorldSwapSeh(world, &ra);
        if (histMismatch) {
            g_uiHistBackupValid = false;
        }
    }
    LeaveCriticalSection(&g_moneySwapLock);

    if (loanEval.submitCmd) {
        Commands_Submit(LS_CMD_LOAN, &loanEval.cmd, sizeof(loanEval.cmd));
    } else if (loanEval.applyOfflineCmd) {
        int owner = (loanEval.cmd.ownerSubType == 1) ? 2 : 1;
        TwoStates_BeginCommand(owner);
        TwoStates_ApplyLoanCommand(loanEval.cmd);
        TwoStates_EndCommand(owner);
    }
}

// ---------------------------------------------------------------------------
// Economy Hook Expected Bytes, Typedefs & Trampolines
// ---------------------------------------------------------------------------

static const BYTE kBldEconExpect[19] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48,
    0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kCustomsBorderExpect[18] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x48,
    0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kCustomsVehExpect[19] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48,
    0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kConstructStepExpect[20] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0xF3, 0x0F, 0x11,
    0x50, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08
};
static const BYTE kConstructGroundworkExpect[14] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57
};
static const BYTE kBldTourismStepExpect[16] = {
    0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48,
    0x81, 0xEC, 0x10, 0x01, 0x00, 0x00
};
static const BYTE kBldStepPrePostExpect[17] = {
    0x40, 0x53, 0x57, 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00,
    0x83, 0xB9, 0xB0, 0x05, 0x00, 0x00, 0x00
};
static const BYTE kBldStepUtilityExpect[14] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57
};
static const BYTE kBldStepProductionExpect[15] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56,
    0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kBldConstructWorkersExpect[15] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48,
    0x08, 0x55, 0x53, 0x56, 0x57
};
static const BYTE kBldWorkerStepExpect[19] = {
    0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00
};
static const BYTE kBldImportStepExpect[14] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x57, 0x41, 0x55, 0x41, 0x56,
    0x48, 0x8D, 0x68, 0x88
};
static const BYTE kBldProductivityStepExpect[15] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x70,
    0x20, 0x55, 0x57, 0x41, 0x54
};
static const BYTE kBldExportStepExpect[15] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41,
    0x57, 0x48, 0x8D, 0x68, 0xB8
};
static const BYTE kBldConsumeResExpect[16] = {
    0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50,
    0x10, 0x55, 0x56, 0x57, 0x41, 0x54
};
static const BYTE kBldUtilityBorderStepExpect[19] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56,
    0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};
static const BYTE kForeignLaborStepExpect[16] = {
    0x40, 0x55, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x45,
    0x0F, 0xB6, 0xF8, 0x48, 0x8B, 0xFA
};

static const BYTE kCalcStoredResourcesExpect[16] = {
    0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50,
    0x10, 0x48, 0x89, 0x48, 0x08, 0x55
};
static const BYTE kVehicleSaleExpect[19] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44, 0x88, 0x40,
    0x18, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57
};
static const BYTE kVehicleBorderExportExpect[17] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x68, 0xA1,
    0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00
};
static const BYTE kVehicleCargoTradeExpect[19] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x4C, 0x89, 0x40,
    0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08
};
static const BYTE kVehicleRefuelExpect[16] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xB8
};
static const BYTE kForeignWorkerBoardExpect[17] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10,
    0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kLedgerAddExpect[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
    0x57, 0x48, 0x83, 0xEC, 0x30
};
static const BYTE kStatRecordClearExpect[19] = {
    0x33, 0xC9, 0x48, 0x89, 0x0A, 0x48, 0x89, 0x8A, 0x88, 0x01,
    0x00, 0x00, 0x48, 0x89, 0x8A, 0x90, 0x01, 0x00, 0x00
};

static const BYTE kGetMaxBorrowExpect[17] = {
    0x4C, 0x8B, 0x81, 0xB0, 0x26, 0x01, 0x00,
    0x45, 0x33, 0xC9,
    0x4C, 0x2B, 0x81, 0xA8, 0x26, 0x01, 0x00
};
static const BYTE kStepOneLoanDayExpect[20] = {
    0x48, 0x83, 0xEC, 0x18,
    0x8B, 0x42, 0x10,
    0x0F, 0x29, 0x34, 0x24,
    0x85, 0xC0,
    0x7E, 0x05,
    0xFF, 0xC8,
    0x89, 0x42, 0x10
};
static const BYTE kCitComputeLifespanExpect[18] = {
    0x48, 0x83, 0xEC, 0x38,
    0x48, 0x85, 0xD2,
    0x0F, 0x84, 0x59, 0x01, 0x00, 0x00,
    0xF3, 0x0F, 0x10, 0x6A, 0x18
};

typedef float (__fastcall* BldProductivityStepFn)(BYTE* world, BYTE* building, float a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* BldConsumeResFn)(BYTE* world, BYTE* building, BYTE* resource, float amount, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* ForeignLaborStepFn)(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4, uintptr_t a5, uintptr_t a6);
typedef __int64 (__fastcall* ConstructStepFn)(BYTE* world, BYTE* building, float dt, unsigned char flag, uintptr_t a5, uintptr_t a6);
typedef __int64 (__fastcall* VehicleSaleFn)(BYTE* world, BYTE* vehicle, unsigned char a3, unsigned char a4,
                                            uintptr_t a5, uintptr_t a6, float a7);
typedef void (__fastcall* VehicleExportFn)(BYTE* world, BYTE* vehicle, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef unsigned char (__fastcall* VehicleCargoTradeFn)(BYTE* vehicle, uintptr_t a2, uintptr_t a3, unsigned char a4,
                                                        uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8,
                                                        uintptr_t a9, uintptr_t a10);
typedef float (__fastcall* VehicleRefuelFn)(BYTE* vehicle, float a2, unsigned char a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef unsigned char (__fastcall* ForeignWorkerBoardFn)(BYTE* world, BYTE* worker, BYTE* vehicle, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* LedgerAddFn)(BYTE* world, const SovietStatResEntry* row, BYTE* ledgerVec,
                                       float a4, unsigned char a5, float a6, unsigned char a7);
typedef void (__fastcall* StatRecordClearFn)(BYTE* world, BYTE* rec);
typedef void (__fastcall* MasterSaveFn)(BYTE* world);
typedef float (__fastcall* GetMaxAvailableBorrowFn)(BYTE* world, int32_t loanType);
typedef void (__fastcall* StepOneLoanDayFn)(BYTE* world, SovietLoan* loan);
typedef void (__fastcall* CitComputeLifespanFn)(BYTE* world, BYTE* cit);

typedef void (__fastcall* CalcStoredResourcesFn)(BYTE* world, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                                                 uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8);

static BldEconFn o_bldEcon = nullptr;
static BldEconFn o_customsBorder = nullptr;
static BldEconFn o_customsVehicle = nullptr;
static ConstructStepFn o_constructStep = nullptr;
static BldEconFn o_constructGroundwork = nullptr;
static BldEconFn o_bldTourismStep = nullptr;
static BldEconFn o_bldStepPrePost = nullptr;
static BldEconFn o_bldStepUtility = nullptr;
static BldEconFn o_bldStepProduction = nullptr;
static BldEconFn o_bldConstructWorkers = nullptr;
static BldEconFn o_bldWorkerStep = nullptr;
static BldEconFn o_bldImportStep = nullptr;
static BldProductivityStepFn o_bldProductivityStep = nullptr;
static BldEconFn o_bldExportStep = nullptr;
static BldConsumeResFn o_bldConsumeRes = nullptr;
static BldEconFn o_bldUtilityBorderStep = nullptr;
static ForeignLaborStepFn o_foreignLaborStep = nullptr;

static CalcStoredResourcesFn o_calcStoredResources = nullptr;
static VehicleSaleFn o_vehicleSale = nullptr;
static VehicleExportFn o_vehicleBorderExport = nullptr;
static VehicleCargoTradeFn o_vehicleCargoTrade = nullptr;
static VehicleRefuelFn o_vehicleRefuel = nullptr;
static ForeignWorkerBoardFn o_foreignWorkerBoard = nullptr;
static LedgerAddFn o_ledgerAdd = nullptr;
static StatRecordClearFn o_statRecordClear = nullptr;

static GetMaxAvailableBorrowFn o_getMaxAvailableBorrow = nullptr;
static StepOneLoanDayFn o_stepOneLoanDay = nullptr;
static CitComputeLifespanFn o_citComputeLifespan = nullptr;

static void RecordDirtyCitizenLifespan(BYTE* cit)
{
    if (!cit) return;
    if (g_uiDirtyCitizenSet.empty()) {
        for (size_t i = 0; i < g_uiDirtyCitizenLifespans.size(); ++i) {
            if (g_uiDirtyCitizenLifespans[i].cit == cit) return;
        }
        if (g_uiDirtyCitizenLifespans.size() >= 32) {
            for (size_t i = 0; i < g_uiDirtyCitizenLifespans.size(); ++i) {
                g_uiDirtyCitizenSet.insert(g_uiDirtyCitizenLifespans[i].cit);
            }
            if (!g_uiDirtyCitizenSet.insert(cit).second) return;
        }
    } else if (!g_uiDirtyCitizenSet.insert(cit).second) {
        return;
    }
    float orig = 0.0f;
    if (SafeReadPod((const float*)(cit + 0x18), &orig)) {
        g_uiDirtyCitizenLifespans.push_back({ cit, orig });
    }
}

static void __fastcall H_CitComputeLifespan(BYTE* world, BYTE* cit)
{
    // CRITICAL: stolen prologue je +0x159 at +7 is not relocated, so null cit must return early.
    if (!cit) return;
    if (g_uiCitVecSwapped && !g_inSimStep && !g_inCommand) {
        RecordDirtyCitizenLifespan(cit);
    }
    if (o_citComputeLifespan) {
        o_citComputeLifespan(world, cit);
    }
}

static bool g_inStatClearTest = false;
static thread_local bool t_inEconHook = false;
static thread_local int  t_econOwner = 1;
static thread_local bool t_suppressCurStatLedgerHook = false;

struct EconHookPerfAccum {
    int64_t count;
    int64_t totalTicks;
    int64_t maxTicks;
};
static constexpr int64_t kEconHookPerfBatchLimit = 4096;
static thread_local EconHookPerfAccum t_econHookPerf = {};

void TwoStates_FlushEconHookPerf()
{
    if (t_econHookPerf.count <= 0) return;
    const int64_t count = t_econHookPerf.count;
    const int64_t totalTicks = t_econHookPerf.totalTicks;
    const int64_t maxTicks = t_econHookPerf.maxTicks;
    t_econHookPerf.count = 0;
    t_econHookPerf.totalTicks = 0;
    t_econHookPerf.maxTicks = 0;
    DiagPerf_RecordBatch(DIAG_PERF_TS_ECON_HOOK, count, totalTicks, maxTicks);
}

static inline void RecordEconHookPerf(int64_t qpcDelta)
{
    if (qpcDelta < 0) qpcDelta = 0;
    t_econHookPerf.count += 1;
    t_econHookPerf.totalTicks += qpcDelta;
    if (qpcDelta > t_econHookPerf.maxTicks) {
        t_econHookPerf.maxTicks = qpcDelta;
    }
    if (!g_inSimStep || GetCurrentThreadId() != g_simStepThread ||
        t_econHookPerf.count >= kEconHookPerfBatchLimit) {
        TwoStates_FlushEconHookPerf();
    }
}
double g_lastWorldRub = 0.0;
double g_lastWorldUsd = 0.0;
double g_unattributedRub = 0.0;
double g_unattributedUsd = 0.0;
StateEconScalars g_lastWorldScalars = {};
StateEconScalars g_unattributedScalars = {};
static StateEconScalars g_econBeforeScalars = {};
StateEconScalars g_cmdBeforeScalars = {};
StateTrackerSlice g_lastWorldTrackerCur = {};
StateTrackerSlice g_unattributedTrackerCur = {};
static StateTrackerSlice g_econBeforeTrackerCur = {};
StateTrackerSlice g_cmdBeforeTrackerCur = {};
static bool g_econBeforeMatchesLast = false;

bool TwoStates_IsInEconHook() { return t_inEconHook; }
int TwoStates_GetEconOwner() { return t_econOwner; }
bool TwoStates_IsCurStatLedgerSuppressed() { return t_suppressCurStatLedgerHook; }
void TwoStates_SetCurStatLedgerSuppressed(bool suppressed) { t_suppressCurStatLedgerHook = suppressed; }

void TwoStates_ResetEconBeforeCursor()
{
    g_econBeforeMatchesLast = false;
}

void ResetStateTrackerSlices()
{
    memset(g_stateTrackerCur, 0, sizeof(g_stateTrackerCur));
    memset(g_stateTrackerOld, 0, sizeof(g_stateTrackerOld));
    memset(&g_lastWorldTrackerCur, 0, sizeof(g_lastWorldTrackerCur));
    memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
    memset(&g_econBeforeTrackerCur, 0, sizeof(g_econBeforeTrackerCur));
    memset(&g_cmdBeforeTrackerCur, 0, sizeof(g_cmdBeforeTrackerCur));
    g_econBeforeMatchesLast = false;
    g_stateTrackerInitialized = false;
}

static inline bool WorldTrackedEconSlicesEqual(const BYTE* rec, const BYTE* world,
                                               const StateEconScalars& refSc, const StateTrackerSlice& refTr)
{
    const BYTE* scBytes = reinterpret_cast<const BYTE*>(&refSc);
    const BYTE* trBytes = reinterpret_cast<const BYTE*>(&refTr);
    return memcmp(rec + STAT_VEHICLES_OFF,
                  scBytes + offsetof(StateEconScalars, vehicles),
                  sizeof(refSc.vehicles)) == 0 &&
           memcmp(rec + STAT_IMMIGRANT_SPEND_OFF,
                  scBytes + offsetof(StateEconScalars, immigrantSpend),
                  sizeof(refSc.immigrantSpend) + sizeof(refSc.citizenCounts)) == 0 &&
           memcmp(rec + STAT_EXTRA_COUNTS_OFF,
                  scBytes + offsetof(StateEconScalars, extraCounts),
                  sizeof(refSc.extraCounts) + sizeof(refSc.tourism) + sizeof(refSc.loans)) == 0 &&
           memcmp(world + WORLD_TRACKER_HEALTH_CUR_SUM_OFF,
                  trBytes + offsetof(StateTrackerSlice, healthSum),
                  sizeof(refTr.healthSum) + sizeof(refTr.healthCnt)) == 0 &&
           memcmp(world + WORLD_TRACKER_HAPPY_CUR_SUM_OFF,
                  trBytes + offsetof(StateTrackerSlice, happySum),
                  sizeof(refTr.happySum) + sizeof(refTr.happyCnt)) == 0;
}

static inline void ReadTrackedEconSlicesInPlace(const BYTE* rec, const BYTE* world,
                                                StateEconScalars* outSc, StateTrackerSlice* outTr)
{
    BYTE* scBytes = reinterpret_cast<BYTE*>(outSc);
    BYTE* trBytes = reinterpret_cast<BYTE*>(outTr);
    memcpy(scBytes + offsetof(StateEconScalars, vehicles),
           rec + STAT_VEHICLES_OFF,
           sizeof(outSc->vehicles));
    memcpy(scBytes + offsetof(StateEconScalars, immigrantSpend),
           rec + STAT_IMMIGRANT_SPEND_OFF,
           sizeof(outSc->immigrantSpend) + sizeof(outSc->citizenCounts));
    memcpy(scBytes + offsetof(StateEconScalars, extraCounts),
           rec + STAT_EXTRA_COUNTS_OFF,
           sizeof(outSc->extraCounts) + sizeof(outSc->tourism) + sizeof(outSc->loans));
    memcpy(trBytes + offsetof(StateTrackerSlice, healthSum),
           world + WORLD_TRACKER_HEALTH_CUR_SUM_OFF,
           sizeof(outTr->healthSum) + sizeof(outTr->healthCnt));
    memcpy(trBytes + offsetof(StateTrackerSlice, happySum),
           world + WORLD_TRACKER_HAPPY_CUR_SUM_OFF,
           sizeof(outTr->happySum) + sizeof(outTr->happyCnt));
}

static inline void CopyTrackedEconSlices(StateEconScalars* dstSc, StateTrackerSlice* dstTr,
                                         const StateEconScalars& srcSc, const StateTrackerSlice& srcTr)
{
    BYTE* dstScBytes = reinterpret_cast<BYTE*>(dstSc);
    BYTE* dstTrBytes = reinterpret_cast<BYTE*>(dstTr);
    const BYTE* srcScBytes = reinterpret_cast<const BYTE*>(&srcSc);
    const BYTE* srcTrBytes = reinterpret_cast<const BYTE*>(&srcTr);
    memcpy(dstScBytes + offsetof(StateEconScalars, vehicles),
           srcScBytes + offsetof(StateEconScalars, vehicles),
           sizeof(srcSc.vehicles));
    memcpy(dstScBytes + offsetof(StateEconScalars, immigrantSpend),
           srcScBytes + offsetof(StateEconScalars, immigrantSpend),
           sizeof(srcSc.immigrantSpend) + sizeof(srcSc.citizenCounts));
    memcpy(dstScBytes + offsetof(StateEconScalars, extraCounts),
           srcScBytes + offsetof(StateEconScalars, extraCounts),
           sizeof(srcSc.extraCounts) + sizeof(srcSc.tourism) + sizeof(srcSc.loans));
    memcpy(dstTrBytes + offsetof(StateTrackerSlice, healthSum),
           srcTrBytes + offsetof(StateTrackerSlice, healthSum),
           sizeof(srcTr.healthSum) + sizeof(srcTr.healthCnt));
    memcpy(dstTrBytes + offsetof(StateTrackerSlice, happySum),
           srcTrBytes + offsetof(StateTrackerSlice, happySum),
           sizeof(srcTr.happySum) + sizeof(srcTr.happyCnt));
}

__declspec(noinline) static void FlushWorldTrackedEconDeltaSlow(
    const BYTE* world, const StateEconScalars& refSc, const StateTrackerSlice& refTr,
    StateEconScalars* dstSc, StateTrackerSlice* dstTr)
{
    StateEconScalars curSc;
    StateTrackerSlice curTr;
    ReadTrackedEconSlicesInPlace(world + WORLD_STAT_CURRENT_OFFSET, world, &curSc, &curTr);
    AddStatScalarsDelta(dstSc, refSc, curSc, 1.0f);
    AddTrackerSliceDelta(dstTr, refTr, curTr, 1.0f);
    CopyTrackedEconSlices(&g_lastWorldScalars, &g_lastWorldTrackerCur, curSc, curTr);
    g_econBeforeMatchesLast = true;
}

static inline void FlushWorldTrackedEconDelta(
    const BYTE* world, StateEconScalars* dstSc, StateTrackerSlice* dstTr, bool fromLastOnly)
{
    const bool useLast = fromLastOnly || g_econBeforeMatchesLast;
    const StateEconScalars& refSc = useLast ? g_lastWorldScalars    : g_econBeforeScalars;
    const StateTrackerSlice& refTr = useLast ? g_lastWorldTrackerCur : g_econBeforeTrackerCur;
    if (WorldTrackedEconSlicesEqual(world + WORLD_STAT_CURRENT_OFFSET, world, refSc, refTr)) {
        if (!useLast) {
            CopyTrackedEconSlices(&g_lastWorldScalars, &g_lastWorldTrackerCur, refSc, refTr);
        }
        g_econBeforeMatchesLast = true;
        return;
    }
    FlushWorldTrackedEconDeltaSlow(world, refSc, refTr, dstSc, dstTr);
}

void TwoStates_AttributeHealthCauseDirect(BYTE* world, int owner, int causeIdx, float delta)
{
    if (!world || causeIdx < 0 || causeIdx >= TRACKER_HEALTH_CAUSE_COUNT || delta == 0.0f) return;
    const bool trackStates = (owner == 1 || owner == 2);
    if (trackStates && (!g_stateEconInitialized || !g_stateTrackerInitialized)) {
        EnsureStateEconInitialized(world);
    }
    DiagLog_BeginSafeSeh();
    __try {
        *(float*)(world + WORLD_TRACKER_HEALTH_CUR_SUM_OFF + (size_t)causeIdx * sizeof(float)) += delta;
        *(int32_t*)(world + WORLD_TRACKER_HEALTH_CUR_CNT_OFF + (size_t)causeIdx * sizeof(int32_t)) += 1;
        if (trackStates) {
            int idx = owner - 1;
            if (isfinite(delta)) {
                g_stateTrackerCur[idx].healthSum[causeIdx] += delta;
                if (g_stateTrackerCur[idx].healthSum[causeIdx] < 0.0f) {
                    g_stateTrackerCur[idx].healthSum[causeIdx] = 0.0f;
                }
            }
            g_stateTrackerCur[idx].healthCnt[causeIdx] += 1;
            g_lastWorldTrackerCur.healthSum[causeIdx] += delta;
            g_lastWorldTrackerCur.healthCnt[causeIdx] += 1;
            if (t_inEconHook && !g_econBeforeMatchesLast) {
                g_econBeforeTrackerCur.healthSum[causeIdx] += delta;
                g_econBeforeTrackerCur.healthCnt[causeIdx] += 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

bool BeginEconAttribution(BYTE* world, int owner)
{
    if (!g_inSimStep || g_inCommand || t_inEconHook || !TwoStates_IsActive()) return false;
    if (Determinism_IsActive() && !Determinism_InStepFrame()) return false;
    if (world != g_exeBase + WORLD_RVA || GetCurrentThreadId() != g_simStepThread) return false;
    if (owner != 1 && owner != 2) return false;

    const int64_t hookT0 = DiagPerf_Now();
    const bool unlim = g_stepUnlimitedMoney;
    if (!g_stateEconInitialized || !g_stateTrackerInitialized) {
        EnsureStateEconInitialized(world);
    }
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        if (!unlim) {
            double curRub = *WorldRubles();
            double curUsd = *WorldDollars();
            g_unattributedRub += (curRub - g_lastWorldRub);
            g_unattributedUsd += (curUsd - g_lastWorldUsd);
            *WorldRubles()  = g_stateRub[owner - 1];
            *WorldDollars() = g_stateUsd[owner - 1];
        }
        FlushWorldTrackedEconDelta(world, &g_unattributedScalars, &g_unattributedTrackerCur, true);
        t_econOwner  = owner;
        t_inEconHook = true;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_inEconHook = false;
        ok = false;
    }
    DiagLog_EndSafeSeh();
    RecordEconHookPerf(DiagPerf_Now() - hookT0);
    return ok;
}

double SanitizeMoneyStep(double oldBal, double newBal)
{
    if (!isfinite(newBal) || fabs(newBal - oldBal) > 1.0e9) return oldBal;
    return newBal;
}

void EndEconAttribution(int owner, bool countInNetTrade)
{
    if (!t_inEconHook) return;
    t_inEconHook = false;
    const int64_t hookT0 = DiagPerf_Now();
    const bool unlim = g_stepUnlimitedMoney;
    DiagLog_BeginSafeSeh();
    __try {
        int idx = owner - 1;
        if (!unlim) {
            double newRub = SanitizeMoneyStep(g_stateRub[idx], *WorldRubles());
            double newUsd = SanitizeMoneyStep(g_stateUsd[idx], *WorldDollars());
            double dRub = newRub - g_stateRub[idx];
            double dUsd = newUsd - g_stateUsd[idx];
            g_stateRub[idx] = newRub;
            g_stateUsd[idx] = newUsd;
            if (countInNetTrade) {
                g_netTradeRub[idx] += dRub;
                g_netTradeUsd[idx] += dUsd;
            }
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1] + g_unattributedRub;
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1] + g_unattributedUsd;
            g_lastWorldRub = *WorldRubles();
            g_lastWorldUsd = *WorldDollars();
        }
        if (g_exeBase) {
            BYTE* world = g_exeBase + WORLD_RVA;
            FlushWorldTrackedEconDelta(world, &g_stateCurEcon[idx].scalars, &g_stateTrackerCur[idx], false);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    RecordEconHookPerf(DiagPerf_Now() - hookT0);
}

void SanitizeBuildingEffectiveCapFloat(BYTE* b)
{
    if (!b) return;
    DiagLog_BeginSafeSeh();
    __try {
        float cap = *(const float*)(b + BLD_EFFECTIVE_CAP_FLOAT_OFFSET);
        if (!isfinite(cap) || cap < 0.0f || cap > 1.0e6f) {
            *(float*)(b + BLD_EFFECTIVE_CAP_FLOAT_OFFSET) = 0.0f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

void SanitizeAllWorldBuildingsEffectiveCap(const BYTE* world)
{
    if (!world) return;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* const* b = *(BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET);
        BYTE* const* e = *(BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET + 8);
        if (b && e > b && (e - b) <= 200000) {
            for (BYTE* const* it = b; it < e; ++it) {
                BYTE* bld = *it;
                if (!bld) continue;
                float cap = *(const float*)(bld + BLD_EFFECTIVE_CAP_FLOAT_OFFSET);
                if (!isfinite(cap) || cap < 0.0f || cap > 1.0e6f) {
                    *(float*)(bld + BLD_EFFECTIVE_CAP_FLOAT_OFFSET) = 0.0f;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

__int64 CallBuildingEconHook(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                    uintptr_t a5, uintptr_t a6, BldEconFn orig)
{
    int owner = (g_inSimStep && !t_inEconHook) ? LookupBuildingOwnerCached(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = orig ? orig(world, building, a3, a4, a5, a6) : 0;
    if (building) SanitizeBuildingEffectiveCapFloat(building);
    if (active) EndEconAttribution(owner);
    return r;
}

static __int64 __fastcall H_BldEcon(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                    uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldEcon);
}

static __int64 __fastcall H_CustomsBorder(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                          uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_customsBorder);
}

static __int64 __fastcall H_CustomsVehicle(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                           uintptr_t a5, uintptr_t a6)
{
    // 0x1B9A40(world, building) is the Foreign Power Connection (category 0x1F) electricity
    // import (+0x9E8) and export (+0x9EC) step, called at 0x13A4E4 and 0x13E5EA outside
    // RVA_BLD_SIM_LOOP_HEAD..TAIL (where g_simLoopCurrentBuilding is nullptr). Wrapping it
    // in CallBuildingEconHook attributes 100% of electricity import/export RUB/USD and
    // R0..R3 ledger entries to the Foreign Power Connection's owner state.
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_customsVehicle);
}

static void AttributeLedgerVecDiffToStates(const BYTE* world, int vecIdx, int owner,
                                           const std::vector<SovietStatResEntry>& beforeEntries,
                                           const std::vector<SovietStatResEntry>& afterEntries);

ConstructResSnapshot g_cmdResSnapshot;

void BeginConstructResSnapshot(const BYTE* world, int owner, ConstructResSnapshot* snap)
{
    if (!snap) return;
    snap->active = false;
    if (!owner || !TwoStates_IsActive() || !world || (g_exeBase && world != g_exeBase + WORLD_RVA)) return;
    EnsureStateEconInitialized(world);
    const BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;
    for (int vIdx = 0; vIdx < STAT_RES_VEC_COUNT; ++vIdx) {
        SafeReadResVecEntries(curStat + STAT_RES_VEC_BASE_OFF + (size_t)vIdx * STAT_RES_VEC_STRIDE, snap->before[vIdx]);
    }
    snap->prevSuppress = t_suppressCurStatLedgerHook;
    t_suppressCurStatLedgerHook = true;
    snap->active = true;
}

void EndConstructResSnapshot(const BYTE* world, int owner, const ConstructResSnapshot& snap)
{
    if (!snap.active) return;
    t_suppressCurStatLedgerHook = snap.prevSuppress;
    if (!world) return;
    const BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;
    static thread_local std::vector<SovietStatResEntry> t_endConstructAfter;
    for (int vIdx = 0; vIdx < STAT_RES_VEC_COUNT; ++vIdx) {
        t_endConstructAfter.clear();
        if (SafeReadResVecEntries(curStat + STAT_RES_VEC_BASE_OFF + (size_t)vIdx * STAT_RES_VEC_STRIDE, t_endConstructAfter)) {
            AttributeLedgerVecDiffToStates(world, vIdx, owner, snap.before[vIdx], t_endConstructAfter);
        }
    }
}

static thread_local ConstructResSnapshot t_constructResSnapshot;

struct ConstructEconScope {
    BYTE* world = nullptr;
    int owner = 0;
    bool active = false;
    bool ownsSnap = false;

    ConstructEconScope(BYTE* w, BYTE* building) : world(w)
    {
        BYTE* canonicalWorld = (g_exeBase && w == g_exeBase + WORLD_RVA) ? (g_exeBase + WORLD_RVA) : w;
        owner = (g_inSimStep && !t_inEconHook) ? LookupBuildingOwnerCached(building) : 0;
        active = owner ? BeginEconAttribution(canonicalWorld, owner) : false;
        if (active && !t_constructResSnapshot.active) {
            BeginConstructResSnapshot(canonicalWorld, owner, &t_constructResSnapshot);
            ownsSnap = t_constructResSnapshot.active;
        }
    }

    ~ConstructEconScope()
    {
        if (active) {
            BYTE* canonicalWorld = g_exeBase ? (g_exeBase + WORLD_RVA) : world;
            if (ownsSnap) {
                EndConstructResSnapshot(canonicalWorld, owner, t_constructResSnapshot);
                t_constructResSnapshot.active = false;
            }
            EndEconAttribution(owner);
        }
    }
};

static __int64 __fastcall H_ConstructStep(BYTE* world, BYTE* building, float dt, unsigned char flag,
                                          uintptr_t a5, uintptr_t a6)
{
    ConstructEconScope scope(world, building);
    return o_constructStep ? o_constructStep(world, building, dt, flag, a5, a6) : 0;
}

static __int64 __fastcall H_ConstructGroundwork(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                                uintptr_t a5, uintptr_t a6)
{
    ConstructEconScope scope(world, building);
    return o_constructGroundwork ? o_constructGroundwork(world, building, a3, a4, a5, a6) : 0;
}

static __int64 __fastcall H_BldTourismStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                           uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldTourismStep);
}

static __int64 __fastcall H_BldStepPrePost(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                           uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldStepPrePost);
}

static __int64 __fastcall H_BldStepUtility(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                           uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldStepUtility);
}

static void RegisterNewlySpawnedVehicles(BYTE* building, int vehBefore)
{
    int vehAfter = GetWorldVehicleCount();
    if (vehAfter <= vehBefore || !building || !g_exeBase) return;
    int owner = LookupBuildingOwnerCached(building);
    if (owner != 1 && owner != 2) return;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** v = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
        for (int i = vehBefore; v && i < vehAfter; ++i) {
            if (v[i]) {
                int vehId = *(const int*)(v[i] + VEH_ID_OFFSET);
                TwoStates_RegisterVehicleOwner(vehId, owner);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DiagLog_EndSafeSeh();
}

static __int64 CallBuildingEconAndRegisterVehicles(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                                   uintptr_t a5, uintptr_t a6, BldEconFn orig)
{
    int vehBefore = GetWorldVehicleCount();
    __int64 r = CallBuildingEconHook(world, building, a3, a4, a5, a6, orig);
    RegisterNewlySpawnedVehicles(building, vehBefore);
    return r;
}

static __int64 __fastcall H_BldStepProduction(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                              uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconAndRegisterVehicles(world, building, a3, a4, a5, a6, o_bldStepProduction);
}

static __int64 __fastcall H_BldConstructWorkers(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                                uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconAndRegisterVehicles(world, building, a3, a4, a5, a6, o_bldConstructWorkers);
}

static __int64 __fastcall H_BldWorkerStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                          uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldWorkerStep);
}

static __int64 __fastcall H_BldImportStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                          uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldImportStep);
}

static float __fastcall H_BldProductivityStep(BYTE* world, BYTE* building, float a3, uintptr_t a4,
                                              uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !t_inEconHook) ? LookupBuildingOwnerCached(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    float r = o_bldProductivityStep ? o_bldProductivityStep(world, building, a3, a4, a5, a6) : 0.0f;
    if (active) EndEconAttribution(owner);
    return r;
}

static __int64 __fastcall H_BldExportStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                          uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldExportStep);
}

static void __fastcall H_BldConsumeResource(BYTE* world, BYTE* building, BYTE* resource, float amount,
                                            uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !t_inEconHook && building) ? LookupBuildingOwnerCached(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    if (o_bldConsumeRes) o_bldConsumeRes(world, building, resource, amount, a5, a6);
    if (active) EndEconAttribution(owner);
}

static __int64 __fastcall H_BldUtilityBorderStep(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                                 uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldUtilityBorderStep);
}

static int ResolveForeignLaborOwnerFast(const BYTE* citizen)
{
    if (g_simLoopCurrentBuilding) return LookupBuildingOwnerCached(g_simLoopCurrentBuilding);
    return ResolveCitizenOwnerCached(citizen);
}

static void __fastcall H_ForeignLaborStep(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4,
                                          uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveForeignLaborOwnerFast(citizen) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    if (o_foreignLaborStep) o_foreignLaborStep(world, citizen, a3, a4, a5, a6);
    if (active) EndEconAttribution(owner);
}

static void __fastcall H_CalcStoredResources(BYTE* world, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                                             uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8)
{
    bool filter = TwoStates_IsActive() && !g_inSimStep && !g_inCommand && !TwoStates_IsSaveInProgress() &&
                  world && (!g_exeBase || world == g_exeBase + WORLD_RVA);
    uintptr_t savedBldHdr[3] = {};
    uintptr_t savedVehHdr[3] = {};
    bool bldSwapped = false;
    bool vehSwapped = false;
    if (filter) {
        EnsureBldPtrOwnerCache(world);
        int localState = TwoStates_LocalState();
        bldSwapped = SwapWorldPtrVecForState(world, WORLD_BUILDINGS_OFFSET, localState,
                                             &LookupBuildingOwnerCached, 200000,
                                             savedBldHdr, g_uiLocalBuildings);
        vehSwapped = SwapWorldPtrVecForState(world, WORLD_VEHICLES_OFFSET, localState,
                                             &ResolveVehicleOwnerFast, 200000,
                                             savedVehHdr, g_uiLocalVehicles);
    }
    if (o_calcStoredResources) {
        o_calcStoredResources(world, a2, a3, a4, a5, a6, a7, a8);
    } else if (world) {
        g_testStoredResBldCount = (int)SafeCountStdVector(world + WORLD_BUILDINGS_OFFSET, sizeof(void*));
        g_testStoredResVehCount = (int)SafeCountStdVector(world + WORLD_VEHICLES_OFFSET, sizeof(void*));
    }
    if (vehSwapped) SafeMemcpyPod(world + WORLD_VEHICLES_OFFSET, savedVehHdr, sizeof(savedVehHdr));
    if (bldSwapped) SafeMemcpyPod(world + WORLD_BUILDINGS_OFFSET, savedBldHdr, sizeof(savedBldHdr));
}

void TwoStates_CalcStoredResourcesForTest(BYTE* world, int* outBuildingCount, int* outVehicleCount)
{
    g_testStoredResBldCount = 0;
    g_testStoredResVehCount = 0;
    H_CalcStoredResources(world, 0, 0, 0, 0, 0, 0, 0);
    if (outBuildingCount) *outBuildingCount = g_testStoredResBldCount;
    if (outVehicleCount)  *outVehicleCount  = g_testStoredResVehCount;
}

class VehicleEconOverrideScope {
public:
    VehicleEconOverrideScope(BYTE* world, const BYTE* vehicle)
    {
        if (!g_inSimStep || g_inCommand || !vehicle || !TwoStates_IsActive()) return;
        if (Determinism_IsActive() && !Determinism_InStepFrame()) return;
        if (!world || world != (g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr)) return;
        if (GetCurrentThreadId() != g_simStepThread) return;

        int vehOwner = ResolveVehicleOwnerFast(vehicle);
        if (vehOwner != 1 && vehOwner != 2) return;

        m_world = world;
        m_vehOwner = vehOwner;
        const bool unlim = g_stepUnlimitedMoney;

        if (t_inEconHook) {
            m_prevOwner = t_econOwner;
            m_prevInHook = true;
            if (m_prevOwner == vehOwner) {
                m_active = true;
                m_nested = false;
                return;
            }
            DiagLog_BeginSafeSeh();
            __try {
                int prevIdx = m_prevOwner - 1;
                if (prevIdx >= 0 && prevIdx < 2) {
                    if (!unlim) {
                        g_stateRub[prevIdx] = *WorldRubles();
                        g_stateUsd[prevIdx] = *WorldDollars();
                    }
                    FlushWorldTrackedEconDelta(m_world, &g_stateCurEcon[prevIdx].scalars,
                                               &g_stateTrackerCur[prevIdx], false);
                }
                if (!unlim) {
                    *WorldRubles()  = g_stateRub[vehOwner - 1];
                    *WorldDollars() = g_stateUsd[vehOwner - 1];
                }
                t_econOwner = vehOwner;
                m_active = true;
                m_nested = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                m_active = false;
            }
            DiagLog_EndSafeSeh();
        } else {
            if (BeginEconAttribution(world, vehOwner)) {
                m_active = true;
                m_nested = false;
            }
        }
    }

    ~VehicleEconOverrideScope()
    {
        if (!m_active) return;
        if (m_nested) {
            const bool unlim = g_stepUnlimitedMoney;
            DiagLog_BeginSafeSeh();
            __try {
                int vehIdx = m_vehOwner - 1;
                if (!unlim) {
                    double newRub = SanitizeMoneyStep(g_stateRub[vehIdx], *WorldRubles());
                    double newUsd = SanitizeMoneyStep(g_stateUsd[vehIdx], *WorldDollars());
                    double dRub = newRub - g_stateRub[vehIdx];
                    double dUsd = newUsd - g_stateUsd[vehIdx];
                    g_stateRub[vehIdx] = newRub;
                    g_stateUsd[vehIdx] = newUsd;
                    g_netTradeRub[vehIdx] += dRub;
                    g_netTradeUsd[vehIdx] += dUsd;
                }

                if (m_world) {
                    FlushWorldTrackedEconDelta(m_world, &g_stateCurEcon[vehIdx].scalars,
                                               &g_stateTrackerCur[vehIdx], false);
                }

                int prevIdx = m_prevOwner - 1;
                if (!unlim && prevIdx >= 0 && prevIdx < 2) {
                    *WorldRubles()  = g_stateRub[prevIdx];
                    *WorldDollars() = g_stateUsd[prevIdx];
                }
                t_econOwner  = m_prevOwner;
                t_inEconHook = m_prevInHook;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            DiagLog_EndSafeSeh();
        } else if (!m_prevInHook) {
            EndEconAttribution(m_vehOwner);
        }
    }

    VehicleEconOverrideScope(const VehicleEconOverrideScope&) = delete;
    VehicleEconOverrideScope& operator=(const VehicleEconOverrideScope&) = delete;

private:
    BYTE* m_world = nullptr;
    int m_vehOwner = 0;
    int m_prevOwner = 0;
    bool m_prevInHook = false;
    bool m_active = false;
    bool m_nested = false;
};

void TwoStates_NestedVehicleOverrideForTest(BYTE* world, BYTE* outerBld, BYTE* vehMid,
                                            BYTE* vehInner, BYTE* vehSameOwner)
{
    if (!world || !outerBld) return;
    int outerOwner = LookupBuildingOwnerCached(outerBld);
    if (!BeginEconAttribution(world, outerOwner)) return;
    *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_TOURISM_OFF) += 10.0f;
    *(double*)(world + WORLD_RUB_OFFSET) -= 100.0;
    {
        VehicleEconOverrideScope midScope(world, vehMid);
        *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_OFF) += 50.0f;
        *(double*)(world + WORLD_RUB_OFFSET) -= 200.0;
        {
            VehicleEconOverrideScope innerScope(world, vehInner);
            *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_TOURISM_OFF + 4) += 25.0f;
            *(double*)(world + WORLD_RUB_OFFSET) -= 50.0;
            {
                VehicleEconOverrideScope sameScope(world, vehSameOwner);
                *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_OFF + 4) += 30.0f;
            }
        }
        *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_OFF + 8) += 15.0f;
    }
    *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_TOURISM_OFF + 8) += 5.0f;
    EndEconAttribution(outerOwner);
}

bool TwoStates_PtrInStdVector(const void* vecHeader, const void* targetPtr)
{
    if (!vecHeader || !targetPtr) return false;
    uint64_t bounds[2] = { 0, 0 };
    if (!SafeReadPod(vecHeader, &bounds)) return false;
    const uint64_t beginAddr = bounds[0];
    const uint64_t endAddr = bounds[1];
    if (beginAddr == 0 || endAddr <= beginAddr) return false;
    if ((beginAddr & 7u) != 0 || (endAddr & 7u) != 0) return false;
    const uint64_t count = (endAddr - beginAddr) / sizeof(uint64_t);
    if (count > 1000000ULL || !IsPointerLikeX64(beginAddr)) return false;
    const uint64_t targetAddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(targetPtr));
    const BYTE* basePtr = reinterpret_cast<const BYTE*>(static_cast<uintptr_t>(beginAddr));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t elem = 0;
        if (!SafeReadPod(basePtr + i * sizeof(uint64_t), &elem)) return false;
        if (elem == targetAddr) return true;
    }
    return false;
}

size_t TwoStates_ErasePtrFromStdVector(void* vecHeader, const void* targetPtr)
{
    if (!vecHeader || !targetPtr) return 0;
    uint64_t bounds[2] = { 0, 0 };
    if (!SafeReadPod(vecHeader, &bounds)) return 0;
    const uint64_t beginAddr = bounds[0];
    const uint64_t endAddr = bounds[1];
    if (beginAddr == 0 || endAddr <= beginAddr) return 0;
    if ((beginAddr & 7u) != 0 || (endAddr & 7u) != 0) return 0;
    const uint64_t count = (endAddr - beginAddr) / sizeof(uint64_t);
    if (count > 1000000ULL || !IsPointerLikeX64(beginAddr)) return 0;
    const uint64_t targetAddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(targetPtr));
    BYTE* basePtr = reinterpret_cast<BYTE*>(static_cast<uintptr_t>(beginAddr));
    uint64_t writeIdx = 0;
    size_t removed = 0;
    for (uint64_t readIdx = 0; readIdx < count; ++readIdx) {
        uint64_t elem = 0;
        if (!SafeReadPod(basePtr + readIdx * sizeof(uint64_t), &elem)) return 0;
        if (elem == targetAddr) {
            ++removed;
            continue;
        }
        if (writeIdx != readIdx) {
            if (!SafeWritePod(basePtr + writeIdx * sizeof(uint64_t), elem)) return removed;
        }
        ++writeIdx;
    }
    if (removed > 0) {
        const uint64_t newEnd = beginAddr + writeIdx * sizeof(uint64_t);
        (void)SafeWritePod(static_cast<BYTE*>(vecHeader) + sizeof(uint64_t), newEnd);
    }
    return removed;
}

static __int64 __fastcall H_VehicleSale(BYTE* world, BYTE* vehicle, unsigned char a3, unsigned char a4,
                                        uintptr_t a5, uintptr_t a6, float a7)
{
    if (!isfinite(a7) || fabsf(a7) > 1.0e9f) a7 = 0.0f;
    int vehId = vehicle ? *(const int*)(vehicle + VEH_ID_OFFSET) : -1;
    VehicleEconOverrideScope econ(world, vehicle);
    __int64 r = o_vehicleSale ? o_vehicleSale(world, vehicle, a3, a4, a5, a6, a7) : 0;
    // Crash B: native 0x3E0670 can leave rax == 0 (it has no explicit `mov al, 1` in its epilogue)
    // while removing `vehicle` from VEC_VEHICLES, or leave a dangling pointer in VEC_FILLING_VEHICLES.
    const bool sold = (r != 0) ||
                      (vehicle != nullptr && g_exeBase != nullptr &&
                       !TwoStates_PtrInStdVector(g_exeBase + VEC_VEHICLES_BEGIN_RVA, vehicle));
    if (sold) {
        if (vehicle && g_exeBase) {
            const size_t purged = TwoStates_ErasePtrFromStdVector(
                g_exeBase + VEC_FILLING_VEHICLES_BEGIN_RVA, vehicle);
            if (purged > 0) {
                Logf("republicmp  CRASH_FIX_B: pruned sold vehicle %p (id=%d, count=%zu) from Filling vector",
                     vehicle, vehId, purged);
            }
        }
        if (vehId >= 0) {
            EnsureOwnerLock();
            EnterCriticalSection(&g_ownerLock);
            if (g_vehicleOwners.erase(vehId) > 0) TwoStates_BumpVehOwnerMapGen();
            LeaveCriticalSection(&g_ownerLock);
        }
    }
    return r;
}

static float g_mockVehicleSaleDefaultPrice = 0.0f;
static __int64 __fastcall MockEngineVehicleSale(BYTE* world, BYTE* /*vehicle*/, unsigned char a3, unsigned char /*a4*/,
                                                uintptr_t /*a5*/, uintptr_t /*a6*/, float a7)
{
    if (!world || a3 == 0) return 0;
    float saleRub = (a7 != 0.0f) ? a7 : g_mockVehicleSaleDefaultPrice;
    *(double*)(world + WORLD_RUB_OFFSET) += (double)saleRub;
    *(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_VEHICLES_OFF + 12) += saleRub;
    return 1;
}

void TwoStates_VehicleSaleForTest(BYTE* world, BYTE* vehicle, float customSalePrice, float defaultDepreciatedPrice)
{
    VehicleSaleFn prev = o_vehicleSale;
    o_vehicleSale = &MockEngineVehicleSale;
    g_mockVehicleSaleDefaultPrice = defaultDepreciatedPrice;
    H_VehicleSale(world, vehicle, 1, 0, 1, 0, customSalePrice);
    o_vehicleSale = prev;
}

static void __fastcall H_VehicleBorderExport(BYTE* world, BYTE* vehicle, uintptr_t a3, uintptr_t a4,
                                             uintptr_t a5, uintptr_t a6)
{
    VehicleEconOverrideScope econ(world, vehicle);
    if (o_vehicleBorderExport) o_vehicleBorderExport(world, vehicle, a3, a4, a5, a6);
}

static unsigned char __fastcall H_VehicleCargoTrade(BYTE* vehicle, uintptr_t a2, uintptr_t a3, unsigned char a4,
                                                    uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8,
                                                    uintptr_t a9, uintptr_t a10)
{
    BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
    VehicleEconOverrideScope econ(world, vehicle);
    return o_vehicleCargoTrade ? o_vehicleCargoTrade(vehicle, a2, a3, a4, a5, a6, a7, a8, a9, a10) : 0;
}

static float __fastcall H_VehicleRefuel(BYTE* vehicle, float a2, unsigned char a3,
                                        uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
    VehicleEconOverrideScope econ(world, vehicle);
    return o_vehicleRefuel ? o_vehicleRefuel(vehicle, a2, a3, a4, a5, a6) : 1.0f;
}

static unsigned char __fastcall H_ForeignWorkerBoard(BYTE* world, BYTE* worker, BYTE* vehicle,
                                                     uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    VehicleEconOverrideScope econ(world, vehicle);
    return o_foreignWorkerBoard ? o_foreignWorkerBoard(world, worker, vehicle, a4, a5, a6) : 0;
}

static bool SafeReadCitizenVectorRange(const BYTE* world, const BYTE* const** outBegin, const BYTE* const** outEnd)
{
    *outBegin = nullptr;
    *outEnd = nullptr;
    if (!world) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* const* b = nullptr;
        const BYTE* const* e = nullptr;
        GetFullCitizenVectorRange(world, &b, &e);
        if (b && e >= b && (e - b) <= 2000000) {
            *outBegin = b;
            *outEnd = e;
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outBegin = nullptr;
        *outEnd = nullptr;
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void ComputeBothStatesCitizenDemographics(const BYTE* world, StateEconScalars* outS0, StateEconScalars* outS1)
{
    ResetDemoOutputScalars(outS0);
    ResetDemoOutputScalars(outS1);
    if (!world || (!outS0 && !outS1)) return;

    EnsureBldPtrOwnerCache(world);
    const BYTE* const* b = nullptr;
    const BYTE* const* e = nullptr;
    if (!SafeReadCitizenVectorRange(world, &b, &e)) return;

    DemoStateAccum acc[2] = {};
    if (!ComputeCitizenDemographicsCoreSeh(b, e, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, outS0, outS1, acc)) {
        ResetDemoOutputScalars(outS0);
        ResetDemoOutputScalars(outS1);
        return;
    }
    FinalizeDemoOutputScalars(outS0, acc[0]);
    FinalizeDemoOutputScalars(outS1, acc[1]);
}

void TwoStates_ComputeStepDemographicsAndPrimeUiCache(const BYTE* world, StateEconScalars* outS0,
                                                       StateEconScalars* outS1, float* outWCit0)
{
    if (outWCit0) *outWCit0 = -1.0f;
    ResetDemoOutputScalars(outS0);
    ResetDemoOutputScalars(outS1);
    if (!world || (!outS0 && !outS1)) return;

    int localState = TwoStates_LocalState();
    if (localState != 1 && localState != 2) localState = 1;
    const int sIdx = localState - 1;

    EnsureBldPtrOwnerCache(world);
    const BYTE* const* b = nullptr;
    const BYTE* const* e = nullptr;
    if (!SafeReadCitizenVectorRange(world, &b, &e)) return;

    const ptrdiff_t curCount = b ? (e - b) : 0;
    static std::unique_ptr<BYTE*[]> s_stepLocalCitsBuf;
    static size_t s_stepLocalCitsCap = 0;
    if (curCount > 0 && (size_t)curCount > s_stepLocalCitsCap) {
        size_t newCap = ((size_t)curCount + 4095u) & ~size_t(4095u);
        s_stepLocalCitsBuf = std::make_unique_for_overwrite<BYTE*[]>(newCap);
        s_stepLocalCitsCap = newCap;
    }

    DemoStateAccum acc[2] = {};
    int localCount = 0;
    int localPop = 0;
    int pop1 = 0;
    int pop2 = 0;
    if (!ComputeCitizenDemographicsCoreSeh(
            b, e, localState,
            (curCount > 0) ? s_stepLocalCitsBuf.get() : nullptr,
            nullptr, &localCount, &localPop, &pop1, &pop2, outS0, outS1, acc)) {
        ResetDemoOutputScalars(outS0);
        ResetDemoOutputScalars(outS1);
        return;
    }
    if (outWCit0 && (pop1 + pop2) > 0) {
        *outWCit0 = (float)pop1 / (float)(pop1 + pop2);
    }
    FinalizeDemoOutputScalars(outS0, acc[0]);
    FinalizeDemoOutputScalars(outS1, acc[1]);

    if (Determinism_IsActive() && Determinism_SessionRunning()) {
        const unsigned long long stepCount  = Determinism_StepCount();
        const unsigned long long applyEpoch = Commands_ApplyEpoch();
        const uint64_t ownerGen = g_ownerMapGen.load(std::memory_order_acquire);
        const uint64_t vehOwnerGen = g_vehOwnerMapGen.load(std::memory_order_acquire);
        const StateEconScalars* localDemo = (sIdx == 0) ? outS0 : outS1;

        EnsureMoneySwapLock();
        EnterCriticalSection(&g_moneySwapLock);
        if (!g_uiEconSwapped && localDemo) {
            BYTE** srcBegin = (localCount > 0) ? s_stepLocalCitsBuf.get() : nullptr;
            g_uiLocalCitizens.assign(srcBegin, srcBegin + localCount);
            g_uiCitizenCache.valid = true;
            g_uiCitizenCache.world = world;
            g_uiCitizenCache.stepCount = stepCount;
            g_uiCitizenCache.applyEpoch = applyEpoch;
            g_uiCitizenCache.localState = localState;
            g_uiCitizenCache.citVecBegin = (const BYTE*)b;
            g_uiCitizenCache.citVecCount = curCount;
            g_uiCitizenCache.ownerMapGen = ownerGen;
            g_uiCitizenCache.vehOwnerMapGen = vehOwnerGen;
            g_uiCitizenCache.localPop = localPop;
            CopyDemoFieldsToScalars(&g_uiCitizenCache.demoScalars, *localDemo);
        }
        LeaveCriticalSection(&g_moneySwapLock);
    }
}

void ComputeStateCitizenDemographics(const BYTE* world, int state, StateEconScalars* ioScalars)
{
    if (!ioScalars) return;
    ComputeBothStatesCitizenDemographics(world,
                                           (state == 1) ? ioScalars : nullptr,
                                           (state == 2) ? ioScalars : nullptr);
}

void TwoStates_ComputeDemographicsParityForTest(const BYTE* world, int* outPopS1, int* outPopS2,
                                                float* outSatS1, float* outSatS2,
                                                bool* outExactMatchBoth)
{
    StateEconScalars both0 = {}, both1 = {}, ref0 = {}, ref1 = {};
    ComputeBothStatesCitizenDemographics(world, &both0, &both1);
    ComputeStateCitizenDemographics(world, 1, &ref0);
    ComputeStateCitizenDemographics(world, 2, &ref1);
    if (outPopS1) *outPopS1 = both0.demographics[0] + both0.demographics[1] + both0.demographics[2] + both0.demographics[3];
    if (outPopS2) *outPopS2 = both1.demographics[0] + both1.demographics[1] + both1.demographics[2] + both1.demographics[3];
    if (outSatS1) *outSatS1 = both0.satisfaction[0];
    if (outSatS2) *outSatS2 = both1.satisfaction[0];
    if (outExactMatchBoth) {
        *outExactMatchBoth = (memcmp(&both0, &ref0, sizeof(StateEconScalars)) == 0) &&
                             (memcmp(&both1, &ref1, sizeof(StateEconScalars)) == 0);
    }
}

static int CountStateAdultHousedCitizens(const BYTE* world, int state)
{
    StateEconScalars tmp = {};
    ComputeStateCitizenDemographics(world, state, &tmp);
    return tmp.demographics[0] + tmp.demographics[1] + tmp.demographics[2] + tmp.demographics[3];
}

static void AccumulateCivicBuilding(const BYTE* bld, int state, StateCivicStats* outStats)
{
    if (!bld || !outStats || LookupBuildingOwnerCached(bld) != state) return;
    const BYTE* bldType = *(const BYTE* const*)(bld + 0x318);
    if (!bldType) return;
    BYTE kind = *(const BYTE*)(bldType + 0x360);
    if (kind == 0x4B) {
        outStats->courthouses++;
        int cl = *(const int*)(bld + 0x1388);
        if (cl > 0) outStats->caseload += cl;
    } else if (kind == 0x4C) {
        outStats->prisons++;
        int pr = (int)SafeCountStdVector(bld + 0xBC0, sizeof(void*));
        if (pr > 0) outStats->prisoners += pr;
    } else if (kind == 0x4A) {
        outStats->orphanages++;
        int orph = (int)SafeCountStdVector(bld + 0xBD8, sizeof(void*));
        if (orph > 0) outStats->orphans += orph;
    }
}

static void AccumulateCivicTourism(int state, StateCivicStats* outStats)
{
    if (!outStats || (state != 1 && state != 2)) return;
    int idx = state - 1;
    const StateEconScalars& sc = g_stateCurEcon[idx].scalars;
    double sumRub = 0.0, sumUsd = 0.0;
    for (int i = 0; i < 44; ++i) {
        float val = sc.tourism[i];
        if (val > 0.0f) {
            if ((i % 2) == 0) sumUsd += (double)val;
            else sumRub += (double)val;
        }
    }
    outStats->tourismRub = sumRub;
    outStats->tourismUsd = sumUsd;
}

void TwoStates_GetStateCivicStatsEx(const BYTE* world, int state, StateCivicStats* outStats)
{
    if (!outStats) return;
    memset(outStats, 0, sizeof(*outStats));
    if (state != 1 && state != 2) return;

    if (world) {
        EnsureBldPtrOwnerCache(world);
        DiagLog_BeginSafeSeh();
        __try {
            const BYTE* const* b = *(const BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET);
            const BYTE* const* e = *(const BYTE* const* const*)(world + WORLD_BUILDINGS_OFFSET + 8);
            if (b && e > b && (e - b) <= 200000) {
                for (const BYTE* const* it = b; it < e; ++it) {
                    AccumulateCivicBuilding(*it, state, outStats);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();

        outStats->criminals = outStats->prisoners + outStats->caseload;
        int totalPop = CountStateAdultHousedCitizens(world, state);
        if (totalPop > 0) {
            outStats->crimeRate = ((float)outStats->criminals / (float)totalPop) * 100.0f;
            if (outStats->crimeRate > 100.0f) outStats->crimeRate = 100.0f;
        }
    }

    AccumulateCivicTourism(state, outStats);
}

static float ReadWorkResourceMultiplier(const BYTE* world, int loanType)
{
    if (!world) return 0.0f;
    float mult = 0.0f;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* workRes = *(const BYTE* const*)(world + WORLD_WORK_RESOURCE_PTR_OFF);
        if (workRes) {
            mult = (loanType == LOAN_CURRENCY_USD)
                ? *(const float*)(workRes + 0x58)
                : *(const float*)(workRes + 0x5C);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mult = 0.0f;
    }
    DiagLog_EndSafeSeh();
    return isfinite(mult) ? mult : 0.0f;
}

static float SumLoanSliceDebtForState(const SovietLoan* loans, int count, int loanType, int state)
{
    if (!loans || count <= 0) return 0.0f;
    int targetSub = (state == 2) ? 1 : 0;
    float sum = 0.0f;
    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            int sub = (loans[i].loanSubType == 1) ? 1 : 0;
            if (loans[i].loanType == loanType && sub == targetSub) {
                sum += loans[i].remainingPrincipal + loans[i].unpaidPenaltyBalance;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    return sum;
}

static float SumStateExistingLoanDebt(const BYTE* world, int loanType, int state)
{
    EnsureMoneySwapLock();
    EnterCriticalSection(&g_moneySwapLock);
    if (g_uiLoansSwapped && g_uiLoansFilterByState) {
        float sum = SumLoanSliceDebtForState(g_uiFullLoansBackup.data(), (int)g_uiFullLoansBackup.size(), loanType, state);
        LeaveCriticalSection(&g_moneySwapLock);
        return sum;
    }
    LeaveCriticalSection(&g_moneySwapLock);

    SovietLoan* b = nullptr;
    int count = 0, capCount = 0;
    if (!SafeReadLoanVectorHeader(world, &b, &count, &capCount) || !b || count <= 0) return 0.0f;
    return SumLoanSliceDebtForState(b, count, loanType, state);
}

float TwoStates_CalcMaxAvailableBorrow(BYTE* world, int loanType, int state)
{
    if (!world) return 0.0f;
    int targetState = (state == 2) ? 2 : 1;
    int adultCitizens = CountStateAdultHousedCitizens(world, targetState);
    float popRatio = (float)adultCitizens / 100000.0f;
    if (popRatio > 1.0f) popRatio = 1.0f;
    if (popRatio < 0.0f) popRatio = 0.0f;
    float mult = ReadWorkResourceMultiplier(world, loanType);
    float maxDebt = (200000.0f + 2000000.0f * popRatio) * mult;
    float existingDebt = SumStateExistingLoanDebt(world, loanType, targetState);
    return maxDebt - existingDebt;
}

static float __fastcall H_GetMaxAvailableBorrow(BYTE* world, int32_t loanType)
{
    if (!TwoStates_IsActive() || !world || world != g_exeBase + WORLD_RVA) {
        return o_getMaxAvailableBorrow ? o_getMaxAvailableBorrow(world, loanType) : 0.0f;
    }
    int activeState = t_inEconHook ? t_econOwner : ResolveActiveResearchWorldState();
    return TwoStates_CalcMaxAvailableBorrow(world, (int)loanType, activeState);
}

// Fallback matching 0x4B9490 (StepOneLoanDay) for unit tests in MockEngineHarness where o_stepOneLoanDay is null.
static void StepOneLoanDayFallback(BYTE* world, SovietLoan* loan)
{
    if (!world || !loan) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (loan->daysRemaining > 0) loan->daysRemaining--;
        const float kInv365 = 1.0f / 365.0f;
        float rateFrac = loan->yearInterestRate / 100.0f;
        float p0 = loan->remainingPrincipal;
        float p1 = p0 + p0 * kInv365 * rateFrac;
        float penInterest = loan->unpaidPenaltyBalance * kInv365 * rateFrac;
        float dailyInterest = p1 * kInv365 * rateFrac + penInterest;
        float newPrincipal = p1 + penInterest;
        loan->remainingPrincipal = newPrincipal;

        float principalShare = (loan->daysRemaining > 0)
            ? (newPrincipal / (float)(loan->daysRemaining + 1))
            : newPrincipal;
        float dueToday = principalShare + loan->unpaidPenaltyBalance;

        double* treasuryPtr = (loan->loanType == LOAN_CURRENCY_USD)
            ? (double*)(world + WORLD_USD_OFFSET)
            : (double*)(world + WORLD_RUB_OFFSET);
        if (*treasuryPtr < 0.0) *treasuryPtr = 0.0;
        double avail = *treasuryPtr;

        float actualPaid = dueToday;
        float principalStatDelta = dueToday - dailyInterest;
        if ((double)dueToday > avail) {
            actualPaid = (float)avail;
            if ((double)dailyInterest > avail) {
                dailyInterest = actualPaid;
                principalStatDelta = 0.0f;
            } else {
                principalStatDelta = (float)(avail - (double)dailyInterest);
            }
        }
        *treasuryPtr = avail - (double)actualPaid;

        float* stats = (float*)(world + WORLD_LOAN_STATS_OFFSET);
        int statBase = (loan->loanType == LOAN_CURRENCY_USD) ? 0 : 2;
        stats[statBase + 0] -= principalStatDelta;
        stats[statBase + 1] -= dailyInterest;

        loan->totalPaidSoFar += actualPaid;
        float remAfterPenalty = actualPaid;
        if (loan->unpaidPenaltyBalance > 0.0f) {
            float newPen = loan->unpaidPenaltyBalance - actualPaid;
            if (newPen >= 0.0f) {
                loan->unpaidPenaltyBalance = newPen;
                remAfterPenalty = 0.0f;
            } else {
                loan->unpaidPenaltyBalance = 0.0f;
                remAfterPenalty = -newPen;
            }
        }
        if (remAfterPenalty > 0.0f) {
            if (loan->remainingPrincipal > 0.0f) {
                loan->remainingPrincipal -= remAfterPenalty;
                if (principalShare > remAfterPenalty) {
                    float unpaidInstallment = principalShare - remAfterPenalty;
                    loan->remainingPrincipal -= unpaidInstallment;
                    loan->unpaidPenaltyBalance += unpaidInstallment;
                }
            }
        } else if (principalShare > 0.0f) {
            loan->remainingPrincipal -= principalShare;
            loan->unpaidPenaltyBalance += principalShare;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void __fastcall H_StepOneLoanDay(BYTE* world, SovietLoan* loan)
{
    int owner = 1;
    int32_t sub = 0;
    if (loan && SafeReadPod(&loan->loanSubType, &sub) && sub == 1) {
        owner = 2;
    }
    bool active = (g_inSimStep && !t_inEconHook) ? BeginEconAttribution(world, owner) : false;
    if (o_stepOneLoanDay) {
        o_stepOneLoanDay(world, loan);
    } else {
        StepOneLoanDayFallback(world, loan);
    }
    if (active) {
        EndEconAttribution(owner, false);
    }
}

void TwoStates_StepOneLoanDayForTest(BYTE* world, SovietLoan* loan)
{
    H_StepOneLoanDay(world, loan);
}

int g_stepBldCount[2] = { 0, 0 };

static void CountOwnedStructuresInVec(const BYTE* world, size_t vecOff, int* ioC1, int* ioC2)
{
    if (!world || !ioC1 || !ioC2) return;
    const BYTE* const* b = *(const BYTE* const* const*)(world + vecOff);
    const BYTE* const* e = *(const BYTE* const* const*)(world + vecOff + 8);
    if (b && e > b && (e - b) <= 200000) {
        for (const BYTE* const* it = b; it < e; ++it) {
            const BYTE* bld = *it;
            if (!bld) continue;
            if (LookupBuildingOwnerCached(bld) == 2) (*ioC2)++;
            else (*ioC1)++;
        }
    }
}

static float ComputeWorldState1Weight(const BYTE* world)
{
    if (!world) return 0.5f;
    if (g_stepBldCount[0] + g_stepBldCount[1] > 0) {
        return (float)TwoStates_ComputeStepWeightState1(g_stepBldCount[0], g_stepBldCount[1]);
    }
    EnsureBldPtrOwnerCache(world);
    int c1 = 0, c2 = 0;
    DiagLog_BeginSafeSeh();
    __try {
        CountOwnedStructuresInVec(world, WORLD_BUILDINGS_OFFSET, &c1, &c2);
        CountOwnedStructuresInVec(world, WORLD_CONSTRUCTIONS_OFFSET, &c1, &c2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    return (float)TwoStates_ComputeStepWeightState1(c1, c2);
}

static float ComputeWorldCitizenState1Weight(const BYTE* world)
{
    if (!world) return 0.5f;
    EnsureBldPtrOwnerCache(world);
    int pop1 = 0, pop2 = 0;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* const* citB = nullptr;
        const BYTE* const* citE = nullptr;
        GetFullCitizenVectorRange(world, &citB, &citE);
        if (citB && citE >= citB && (citE - citB) <= 2000000) {
            for (const BYTE* const* it = citB; it < citE; ++it) {
                const BYTE* cit = *it;
                if (!cit) continue;
                if (*(const int32_t*)(cit + CIT_INFANT_FLAG_OFFSET) != 0) continue;
                if (*(const int32_t*)(cit + CIT_UNBORN_FLAG_OFFSET) != 0) continue;
                const BYTE* parent = *(const BYTE* const*)(cit + CIT_PARENT_OFFSET);
                const BYTE* holder = parent ? parent : cit;
                const BYTE* home = *(const BYTE* const*)(holder + CIT_HOME_BUILDING_OFFSET);
                if (!home) continue;
                int owner = LookupBuildingOwnerCached(home);
                if (owner == 1) pop1++;
                else if (owner == 2) pop2++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();

    int totalPop = pop1 + pop2;
    if (totalPop > 0) {
        return (float)pop1 / (float)totalPop;
    }
    return ComputeWorldState1Weight(world);
}

static void SplitEngineStatRecordToStates(const BYTE* world, const BYTE* rec, float w0,
                                          StateEconPeriodStats* outS0, StateEconPeriodStats* outS1)
{
    if (!outS0 || !outS1) return;
    int32_t day = 0, year = 0;
    if (rec) {
        SafeReadPod((const int32_t*)(rec + 0), &day);
        SafeReadPod((const int32_t*)(rec + 4), &year);
    }
    ClearStateEconPeriod(outS0, day, year);
    ClearStateEconPeriod(outS1, day, year);
    if (!rec) return;

    if (w0 < 0.0f) w0 = 0.0f;
    if (w0 > 1.0f) w0 = 1.0f;

    std::vector<SovietStatResEntry> entries;
    for (int v = 0; v < STAT_RES_VEC_COUNT; ++v) {
        if (!SafeReadResVecEntries(rec + STAT_RES_VEC_BASE_OFF + (size_t)v * STAT_RES_VEC_STRIDE, entries)) continue;
        float vecW0 = w0;
        if (v < 8) {
            bool isUsdVec = (v == 0 || v == 1 || v == 4 || v == 5);
            double tr0 = isUsdVec ? fabs(g_netTradeUsd[0]) : fabs(g_netTradeRub[0]);
            double tr1 = isUsdVec ? fabs(g_netTradeUsd[1]) : fabs(g_netTradeRub[1]);
            if (tr0 < 0.005 && tr1 < 0.005) continue;
            if (tr0 < 0.005 && tr1 >= 0.005) vecW0 = 0.0f;
            else if (tr0 >= 0.005 && tr1 < 0.005) vecW0 = 1.0f;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            float t0 = entries[i].amountTons * vecW0;
            float v0 = entries[i].moneyValue * vecW0;
            bool isNeg = (entries[i].amountTons < 0.0f);
            AddEntryDeltaToStateResVec(outS0->resVecs[v], entries[i].resourcePtr, isNeg, t0, v0);
            AddEntryDeltaToStateResVec(outS1->resVecs[v], entries[i].resourcePtr, isNeg,
                                       entries[i].amountTons - t0, entries[i].moneyValue - v0);
        }
    }

    StateEconScalars fullScalars = {};
    StateEconScalars zeroScalars = {};
    SafeReadStatScalars(rec, &fullScalars);
    AddStatScalarsDelta(&outS0->scalars, zeroScalars, fullScalars, w0);
    AddStatScalarsDelta(&outS1->scalars, outS0->scalars, fullScalars, 1.0f);

    float wCit0 = ComputeWorldCitizenState1Weight(world);
    memset(outS0->scalars.citizenCounts, 0, sizeof(outS0->scalars.citizenCounts));
    memset(outS1->scalars.citizenCounts, 0, sizeof(outS1->scalars.citizenCounts));
    memset(outS0->scalars.extraCounts, 0, sizeof(outS0->scalars.extraCounts));
    memset(outS1->scalars.extraCounts, 0, sizeof(outS1->scalars.extraCounts));
    AddIntSliceDelta(outS0->scalars.citizenCounts, zeroScalars.citizenCounts, fullScalars.citizenCounts, 5, wCit0);
    AddIntSliceDelta(outS1->scalars.citizenCounts, outS0->scalars.citizenCounts, fullScalars.citizenCounts, 5, 1.0f);
    AddExtraCountsSliceDelta(outS0->scalars.extraCounts, zeroScalars.extraCounts, fullScalars.extraCounts, wCit0);
    AddExtraCountsSliceDelta(outS1->scalars.extraCounts, outS0->scalars.extraCounts, fullScalars.extraCounts, 1.0f);
    memcpy(outS0->scalars.satisfaction, fullScalars.satisfaction, sizeof(fullScalars.satisfaction));
    memcpy(outS1->scalars.satisfaction, fullScalars.satisfaction, sizeof(fullScalars.satisfaction));
    memcpy(outS0->scalars.citizenAverages, fullScalars.citizenAverages, sizeof(fullScalars.citizenAverages));
    memcpy(outS1->scalars.citizenAverages, fullScalars.citizenAverages, sizeof(fullScalars.citizenAverages));
    AddIntSliceDelta(outS0->scalars.demographics, zeroScalars.demographics, fullScalars.demographics, 4, w0);
    AddIntSliceDelta(outS1->scalars.demographics, outS0->scalars.demographics, fullScalars.demographics, 4, 1.0f);
    AddIntSliceDelta(outS0->scalars.citizenBrackets, zeroScalars.citizenBrackets, fullScalars.citizenBrackets, 9, w0);
    AddIntSliceDelta(outS1->scalars.citizenBrackets, outS0->scalars.citizenBrackets, fullScalars.citizenBrackets, 9, 1.0f);

    const BYTE* const* citB = nullptr;
    const BYTE* const* citE = nullptr;
    if (world) GetFullCitizenVectorRange(world, &citB, &citE);
    if (citB && citE >= citB && (!world || rec == world + WORLD_STAT_CURRENT_OFFSET)) {
        ComputeBothStatesCitizenDemographics(world, &outS0->scalars, &outS1->scalars);
    }
}

static void SyncStateEconHistoryLength(const BYTE* world, float w0IfKnown,
                                       BYTE* preHistBegin = nullptr, int preHistCount = -1)
{
    BYTE* histBegin = preHistBegin;
    int histCount = preHistCount;
    if (histCount < 0) {
        if (!SafeReadStatHistHeader(world, &histBegin, &histCount)) return;
    }
    if (!histBegin || histCount <= 0) return;

    size_t curLen = std::min(g_stateHistEcon[0].size(), g_stateHistEcon[1].size());
    if (curLen > (size_t)histCount) {
        g_stateHistEcon[0].resize((size_t)histCount);
        g_stateHistEcon[1].resize((size_t)histCount);
        TwoStates_BumpHistSwapGen();
        return;
    }
    if (curLen < (size_t)histCount) {
        float w0 = (w0IfKnown >= 0.0f) ? w0IfKnown : ComputeWorldState1Weight(world);
        for (size_t i = curLen; i < (size_t)histCount; ++i) {
            StateEconPeriodStats s0, s1;
            SplitEngineStatRecordToStates(world, histBegin + i * STAT_RECORD_SIZE, w0, &s0, &s1);
            g_stateHistEcon[0].push_back(s0);
            g_stateHistEcon[1].push_back(s1);
        }
        TwoStates_BumpHistSwapGen();
    }
}

static void EnsureStateEconInitializedFromHdr(const BYTE* world, BYTE* histBegin, int histCount)
{
    if (!world) return;
    if (!g_stateEconInitialized) {
        float w0 = ComputeWorldState1Weight(world);
        SplitEngineStatRecordToStates(world, world + WORLD_STAT_CURRENT_OFFSET, w0,
                                      &g_stateCurEcon[0], &g_stateCurEcon[1]);
        g_stateHistEcon[0].clear();
        g_stateHistEcon[1].clear();
        TwoStates_BumpHistSwapGen();
        SyncStateEconHistoryLength(world, w0, histBegin, histCount);
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
        g_stateEconInitialized = true;
    } else {
        SyncStateEconHistoryLength(world, -1.0f, histBegin, histCount);
    }
    if (!g_stateTrackerInitialized) {
        float wCit0 = ComputeWorldCitizenState1Weight(world);
        StateTrackerSlice fullCur = {}, fullOld = {}, zeroTracker = {};
        SafeReadTrackerSlice(world, false, &fullCur);
        SafeReadTrackerSlice(world, true,  &fullOld);
        memset(g_stateTrackerCur, 0, sizeof(g_stateTrackerCur));
        memset(g_stateTrackerOld, 0, sizeof(g_stateTrackerOld));
        AddTrackerSliceDelta(&g_stateTrackerCur[0], zeroTracker, fullCur, wCit0);
        AddTrackerSliceDelta(&g_stateTrackerCur[1], g_stateTrackerCur[0], fullCur, 1.0f);
        AddTrackerSliceDelta(&g_stateTrackerOld[0], zeroTracker, fullOld, wCit0);
        AddTrackerSliceDelta(&g_stateTrackerOld[1], g_stateTrackerOld[0], fullOld, 1.0f);
        g_lastWorldTrackerCur = fullCur;
        memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
        g_stateTrackerInitialized = true;
    }
}

void EnsureStateEconInitialized(const BYTE* world, BYTE** outHistBegin, int* outHistCount)
{
    if (!world) return;
    BYTE* histBegin = nullptr;
    int histCount = -1;
    if (outHistBegin || outHistCount || !g_stateEconInitialized) {
        if (!SafeReadStatHistHeader(world, &histBegin, &histCount)) {
            histBegin = nullptr;
            histCount = 0;
        }
        if (outHistBegin) *outHistBegin = histBegin;
        if (outHistCount) *outHistCount = histCount;
    }
    EnsureStateEconInitializedFromHdr(world, histBegin, histCount);
}

static bool ReadSubStepEconSnapshotSeh(const BYTE* world, BYTE** outHistBegin, int* outHistCount,
                                       StateEconScalars* outScalars, StateTrackerSlice* outTracker)
{
    *outHistBegin = nullptr;
    *outHistCount = 0;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* b = *(const BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET);
        const BYTE* e = *(const BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET + 8);
        const BYTE* c = *(const BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET + 16);
        if (b && e >= b && c >= e && (e - b) % STAT_RECORD_SIZE == 0) {
            ptrdiff_t cnt = (e - b) / STAT_RECORD_SIZE;
            if (cnt >= 0 && cnt <= 20000) {
                *outHistBegin = const_cast<BYTE*>(b);
                *outHistCount = (int)cnt;
            }
        }
        CopyStatBlocksFromRecRaw(world + WORLD_STAT_CURRENT_OFFSET, outScalars, kStatScalarBlocks);
        CopyTrackerSliceRaw(world, false, outTracker);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outHistBegin = nullptr;
        *outHistCount = 0;
        memset(outScalars, 0, sizeof(*outScalars));
        memset(outTracker, 0, sizeof(*outTracker));
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void TwoStates_BeginSubStepEconRead(const BYTE* world)
{
    if (!world) return;
    if (!g_stateEconInitialized || !g_stateTrackerInitialized) {
        EnsureStateEconInitialized(world);
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
        SafeReadTrackerSlice(world, false, &g_lastWorldTrackerCur);
        memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
        return;
    }
    BYTE* histBegin = nullptr;
    int histCount = 0;
    if (ReadSubStepEconSnapshotSeh(world, &histBegin, &histCount, &g_lastWorldScalars, &g_lastWorldTrackerCur)) {
        size_t curLen = std::min(g_stateHistEcon[0].size(), g_stateHistEcon[1].size());
        if (histBegin && histCount > 0 && (size_t)histCount != curLen) {
            SyncStateEconHistoryLength(world, -1.0f, histBegin, histCount);
        }
    }
    memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
    memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
}

// Fallback ledger accumulator for unit tests in MockEngineHarness when o_ledgerAdd is null.
static SovietStatResEntry g_testLedgerPool[STAT_RES_VEC_COUNT][64] = {};

static void LedgerAddFallback(BYTE* world, const SovietStatResEntry* row, BYTE* ledgerVec, float mult)
{
    if (!world || !row || !ledgerVec) return;
    SovietStatResEntry src = {};
    if (!SafeReadPod(row, &src)) return;
    float dTons = src.amountTons * mult;
    float dVal  = src.moneyValue * mult;
    bool isNeg  = (dTons < 0.0f);

    RawStatResVecHeader hdr = {};
    if (!SafeReadPod((const RawStatResVecHeader*)ledgerVec, &hdr)) return;
    if (!hdr.begin) {
        ptrdiff_t rel = ledgerVec - (world + WORLD_STAT_CURRENT_OFFSET + STAT_RES_VEC_BASE_OFF);
        int vecIdx = (rel >= 0 && rel < STAT_RES_VEC_COUNT * STAT_RES_VEC_STRIDE)
            ? (int)(rel / STAT_RES_VEC_STRIDE) : 0;
        hdr.begin = g_testLedgerPool[vecIdx];
        hdr.end   = hdr.begin;
        hdr.cap   = hdr.begin + 64;
        SafeWritePod((RawStatResVecHeader*)ledgerVec, hdr);
    }

    int count = (int)(hdr.end - hdr.begin);
    int cap   = (int)(hdr.cap - hdr.begin);
    if (count < 0 || cap < count || cap > 512) return;

    DiagLog_BeginSafeSeh();
    __try {
        for (int i = 0; i < count; ++i) {
            if (hdr.begin[i].resourcePtr == src.resourcePtr) {
                bool curNeg = (hdr.begin[i].amountTons < 0.0f);
                if (curNeg == isNeg || fabsf(hdr.begin[i].amountTons) <= 1e-7f) {
                    hdr.begin[i].amountTons += dTons;
                    hdr.begin[i].moneyValue += dVal;
                    DiagLog_EndSafeSeh();
                    return;
                }
            }
        }
        if (count < cap) {
            hdr.begin[count].resourcePtr = src.resourcePtr;
            hdr.begin[count].amountTons  = dTons;
            hdr.begin[count].moneyValue  = dVal;
            *(SovietStatResEntry**)(ledgerVec + 8) = hdr.begin + (count + 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void AttributeResEntryDelta(const BYTE* world, int vecIdx, int owner,
                                   uintptr_t resPtr, bool isNeg, float dTons, float dVal)
{
    if (fabsf(dTons) <= 1e-7f && fabsf(dVal) <= 1e-7f) return;
    if (owner == 1 || owner == 2) {
        AddEntryDeltaToStateResVec(g_stateCurEcon[owner - 1].resVecs[vecIdx], resPtr, isNeg, dTons, dVal);
    } else {
        float w0 = ComputeWorldState1Weight(world);
        float t0 = dTons * w0;
        float v0 = dVal * w0;
        AddEntryDeltaToStateResVec(g_stateCurEcon[0].resVecs[vecIdx], resPtr, isNeg, t0, v0);
        AddEntryDeltaToStateResVec(g_stateCurEcon[1].resVecs[vecIdx], resPtr, isNeg, dTons - t0, dVal - v0);
    }
}

static void AttributeLedgerVecDiffToStates(const BYTE* world, int vecIdx, int owner,
                                           const std::vector<SovietStatResEntry>& beforeEntries,
                                           const std::vector<SovietStatResEntry>& afterEntries)
{
    if (vecIdx < 0 || vecIdx >= STAT_RES_VEC_COUNT) return;
    if (afterEntries.size() < beforeEntries.size()) return;
    for (size_t i = 0; i < beforeEntries.size(); ++i) {
        if (afterEntries[i].resourcePtr != beforeEntries[i].resourcePtr) continue;
        float dt = afterEntries[i].amountTons - beforeEntries[i].amountTons;
        float dv = afterEntries[i].moneyValue - beforeEntries[i].moneyValue;
        if (fabsf(dt) > 1e-7f || fabsf(dv) > 1e-7f) {
            bool isNeg = (afterEntries[i].amountTons < 0.0f) ||
                         (fabsf(afterEntries[i].amountTons) <= 1e-7f && dt < 0.0f);
            AttributeResEntryDelta(world, vecIdx, owner, afterEntries[i].resourcePtr, isNeg, dt, dv);
        }
    }
    for (size_t i = beforeEntries.size(); i < afterEntries.size(); ++i) {
        const SovietStatResEntry& e = afterEntries[i];
        AttributeResEntryDelta(world, vecIdx, owner, e.resourcePtr, (e.amountTons < 0.0f), e.amountTons, e.moneyValue);
    }
}

static bool IsCurStatLedgerVec(const BYTE* world, const BYTE* ledgerVec, int* outVecIdx)
{
    if (!world || !ledgerVec || (g_exeBase && world != g_exeBase + WORLD_RVA)) return false;
    ptrdiff_t rel = ledgerVec - (world + WORLD_STAT_CURRENT_OFFSET + STAT_RES_VEC_BASE_OFF);
    if (rel >= 0 && rel < STAT_RES_VEC_COUNT * STAT_RES_VEC_STRIDE && (rel % STAT_RES_VEC_STRIDE) == 0) {
        if (outVecIdx) *outVecIdx = (int)(rel / STAT_RES_VEC_STRIDE);
        return true;
    }
    return false;
}

static void __fastcall H_LedgerAdd(BYTE* world, const SovietStatResEntry* row, BYTE* ledgerVec,
                                   float a4, unsigned char a5, float a6, unsigned char a7)
{
    int vecIdx = -1;
    bool isCurStatVec = IsCurStatLedgerVec(world, ledgerVec, &vecIdx);
    // Do NOT unswap g_uiEconSwapped when the Economy & Trade Window (0x361BB0) or building UI (0x5D6FC0)
    // aggregates rows into a temporary UI summary vector (e.g. world + 0xC7F0), otherwise curStat is
    // reverted to combined Host+Client stats mid-render while the UI is iterating over curStat!
    bool wasUiSwapped = isCurStatVec && g_uiEconSwapped;
    if (wasUiSwapped) {
        TwoStates_RestoreUiMoneySwap();
    }
    bool track = false;
    bool canTrackContext = (!g_exeBase || !Determinism_IsActive() ||
                            (g_inSimStep && Determinism_InStepFrame()) ||
                            g_inCommand || g_inStatClearTest);
    if (isCurStatVec && canTrackContext && !t_suppressCurStatLedgerHook && TwoStates_IsActive()) {
        if (!g_stateEconInitialized || !g_stateTrackerInitialized) {
            EnsureStateEconInitialized(world);
        }
        track = true;
    }

    static thread_local std::vector<SovietStatResEntry> t_ledgerBeforeEntries;
    static thread_local std::vector<SovietStatResEntry> t_ledgerAfterEntries;
    if (track) {
        SafeReadResVecEntries(ledgerVec, t_ledgerBeforeEntries);
    }

    if (o_ledgerAdd) {
        o_ledgerAdd(world, row, ledgerVec, a4, a5, a6, a7);
    } else {
        LedgerAddFallback(world, row, ledgerVec, a6);
    }

    if (track) {
        if (SafeReadResVecEntries(ledgerVec, t_ledgerAfterEntries)) {
            int owner = t_inEconHook ? t_econOwner : (g_inCommand ? g_cmdOwner : 0);
            if (owner != 1 && owner != 2 && g_inSimStep && g_simLoopCurrentBuilding) {
                owner = LookupBuildingOwnerCached(g_simLoopCurrentBuilding);
            }
            AttributeLedgerVecDiffToStates(world, vecIdx, owner, t_ledgerBeforeEntries, t_ledgerAfterEntries);
        }
    }

    if (wasUiSwapped && !g_inSimStep && !g_inCommand) {
        TwoStates_UiBegin();
    }
}

static void StatRecordClearFallback(BYTE* world, BYTE* rec)
{
    if (!rec) return;
    if (world && rec == world + WORLD_STAT_CURRENT_OFFSET) {
        StateTrackerSlice curTracker = {}, zeroTracker = {};
        SafeReadTrackerSlice(world, false, &curTracker);
        SafeWriteTrackerSlice(world, true, curTracker);
        SafeWriteTrackerSlice(world, false, zeroTracker);
    }
    StateEconScalars zeroScalars = {};
    SafeWriteStatScalars(rec, zeroScalars);
    int32_t zeroInt = 0;
    SafeWritePod((int32_t*)(rec + 0), zeroInt);
    SafeWritePod((int32_t*)(rec + 4), zeroInt);
    for (int v = 0; v < STAT_RES_VEC_COUNT; ++v) {
        BYTE* vecAddr = rec + STAT_RES_VEC_BASE_OFF + (size_t)v * STAT_RES_VEC_STRIDE;
        RawStatResVecHeader hdr = {};
        if (SafeReadPod((const RawStatResVecHeader*)vecAddr, &hdr) && hdr.begin) {
            hdr.end = hdr.begin;
            SafeWritePod((RawStatResVecHeader*)vecAddr, hdr);
        }
    }
}

static bool HasAttributedScalarsDiff(const StateEconScalars& a, const StateEconScalars& b)
{
    const BYTE* ab = reinterpret_cast<const BYTE*>(&a);
    const BYTE* bb = reinterpret_cast<const BYTE*>(&b);
    return memcmp(ab + offsetof(StateEconScalars, vehicles),
                  bb + offsetof(StateEconScalars, vehicles),
                  sizeof(a.vehicles)) != 0 ||
           memcmp(ab + offsetof(StateEconScalars, immigrantSpend),
                  bb + offsetof(StateEconScalars, immigrantSpend),
                  sizeof(a.immigrantSpend) + sizeof(a.citizenCounts)) != 0 ||
           memcmp(ab + offsetof(StateEconScalars, extraCounts),
                  bb + offsetof(StateEconScalars, extraCounts),
                  sizeof(a.extraCounts) + sizeof(a.tourism) + sizeof(a.loans)) != 0;
}

void FlushUnattributedScalarsToStates(const BYTE* world, bool trackerFromOld, float precomputedWCit0)
{
    if (!world) return;
    StateEconScalars curScalars = {};
    SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &curScalars);
    if (HasAttributedScalarsDiff(g_lastWorldScalars, curScalars)) {
        AddStatScalarsDelta(&g_unattributedScalars, g_lastWorldScalars, curScalars, 1.0f);
    }
    g_lastWorldScalars = curScalars;

    StateTrackerSlice curTracker = {};
    SafeReadTrackerSlice(world, trackerFromOld, &curTracker);
    if (memcmp(&curTracker, &g_lastWorldTrackerCur, sizeof(StateTrackerSlice)) != 0) {
        AddTrackerSliceDelta(&g_unattributedTrackerCur, g_lastWorldTrackerCur, curTracker, 1.0f);
        g_lastWorldTrackerCur = curTracker;
    }

    static const StateEconScalars kZeroScalars = {};
    static const StateTrackerSlice kZeroTracker = {};
    const bool hasBldScalars =
        memcmp(&g_unattributedScalars.vehicles,       &kZeroScalars.vehicles,       sizeof(kZeroScalars.vehicles)) != 0 ||
        memcmp(&g_unattributedScalars.immigrantSpend, &kZeroScalars.immigrantSpend, sizeof(kZeroScalars.immigrantSpend)) != 0 ||
        memcmp(&g_unattributedScalars.tourism,        &kZeroScalars.tourism,        sizeof(kZeroScalars.tourism)) != 0 ||
        memcmp(&g_unattributedScalars.loans,          &kZeroScalars.loans,          sizeof(kZeroScalars.loans)) != 0;
    const bool hasCitScalars =
        memcmp(&g_unattributedScalars.citizenCounts,  &kZeroScalars.citizenCounts,  sizeof(kZeroScalars.citizenCounts)) != 0 ||
        memcmp(&g_unattributedScalars.extraCounts,    &kZeroScalars.extraCounts,    sizeof(kZeroScalars.extraCounts)) != 0;
    const bool hasTracker =
        memcmp(&g_unattributedTrackerCur, &kZeroTracker, sizeof(StateTrackerSlice)) != 0;

    if (!hasBldScalars && !hasCitScalars && !hasTracker) {
        return;
    }

    auto ResolveWCit0 = [&]() -> float {
        return (precomputedWCit0 >= 0.0f) ? precomputedWCit0 : ComputeWorldCitizenState1Weight(world);
    };

    if (hasBldScalars || hasCitScalars) {
        const float w0 = hasBldScalars ? ComputeWorldState1Weight(world) : 0.5f;
        const float wCit0 = hasCitScalars ? ResolveWCit0() : 0.5f;
        StateEconScalars share0 = {};
        if (hasBldScalars) {
            AddStatScalarsDelta(&share0, kZeroScalars, g_unattributedScalars, w0);
        }
        memset(share0.citizenCounts, 0, sizeof(share0.citizenCounts));
        memset(share0.extraCounts, 0, sizeof(share0.extraCounts));
        if (hasCitScalars) {
            AddIntSliceDelta(share0.citizenCounts, kZeroScalars.citizenCounts, g_unattributedScalars.citizenCounts, 5, wCit0);
            AddExtraCountsSliceDelta(share0.extraCounts, kZeroScalars.extraCounts, g_unattributedScalars.extraCounts, wCit0);
        }
        AddStatScalarsDelta(&g_stateCurEcon[0].scalars, kZeroScalars, share0, 1.0f);
        AddStatScalarsDelta(&g_stateCurEcon[1].scalars, share0, g_unattributedScalars, 1.0f);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));

        if (hasTracker) {
            const float wt = hasCitScalars ? wCit0 : ResolveWCit0();
            StateTrackerSlice trackerShare0 = {};
            AddTrackerSliceDelta(&trackerShare0, kZeroTracker, g_unattributedTrackerCur, wt);
            AddTrackerSliceDelta(&g_stateTrackerCur[0], kZeroTracker, trackerShare0, 1.0f);
            AddTrackerSliceDelta(&g_stateTrackerCur[1], trackerShare0, g_unattributedTrackerCur, 1.0f);
            memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
        }
    } else if (hasTracker) {
        const float wCit0 = ResolveWCit0();
        StateTrackerSlice trackerShare0 = {};
        AddTrackerSliceDelta(&trackerShare0, kZeroTracker, g_unattributedTrackerCur, wCit0);
        AddTrackerSliceDelta(&g_stateTrackerCur[0], kZeroTracker, trackerShare0, 1.0f);
        AddTrackerSliceDelta(&g_stateTrackerCur[1], trackerShare0, g_unattributedTrackerCur, 1.0f);
        memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
    }
}

void TwoStates_FlushUnattributedScalarsForTest(const BYTE* world)
{
    FlushUnattributedScalarsToStates(world, false);
}

static void __fastcall H_StatRecordClear(BYTE* rcxWorld, BYTE* rec)
{
    bool wasUiSwapped = g_uiEconSwapped;
    if (wasUiSwapped) {
        TwoStates_RestoreUiMoneySwap();
    }
    BYTE* world = (rcxWorld && (!g_exeBase || rcxWorld == g_exeBase + WORLD_RVA))
        ? rcxWorld
        : (g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr);
    bool isCurStat = (TwoStates_IsActive() && world && rec == world + WORLD_STAT_CURRENT_OFFSET);
    bool isRollover = isCurStat && !g_beforeLoadPrepared && !Determinism_WorldLoading() &&
                      (g_inSimStep || g_inStatClearTest);
    if (isRollover) {
        EnsureStateEconInitialized(world);
        FlushUnattributedScalarsToStates(world, o_statRecordClear != nullptr);
        g_stateTrackerOld[0] = g_stateTrackerCur[0];
        g_stateTrackerOld[1] = g_stateTrackerCur[1];
        memset(g_stateTrackerCur, 0, sizeof(g_stateTrackerCur));

        BYTE* histBegin = nullptr;
        int histCount = 0;
        bool hasHist = SafeReadStatHistHeader(world, &histBegin, &histCount) && histBegin && histCount > 0;
        int32_t completedDay = 0, completedYear = 0;
        const BYTE* dateSrc = hasHist ? (histBegin + (size_t)(histCount - 1) * STAT_RECORD_SIZE) : rec;
        SafeReadPod((const int32_t*)(dateSrc + 0), &completedDay);
        SafeReadPod((const int32_t*)(dateSrc + 4), &completedYear);

        float w0 = ComputeWorldState1Weight(world);
        for (int s = 0; s < 2; ++s) {
            g_stateCurEcon[s].day = completedDay;
            g_stateCurEcon[s].year = completedYear;
        }
        ComputeBothStatesCitizenDemographics(world, &g_stateCurEcon[0].scalars, &g_stateCurEcon[1].scalars);
        TwoStates_InvalidateUiCitizenCache();

        if (hasHist) {
            while ((int)g_stateHistEcon[0].size() + 1 < histCount) {
                size_t idx = g_stateHistEcon[0].size();
                StateEconPeriodStats s0, s1;
                SplitEngineStatRecordToStates(world, histBegin + idx * STAT_RECORD_SIZE, w0, &s0, &s1);
                g_stateHistEcon[0].push_back(s0);
                g_stateHistEcon[1].push_back(s1);
            }
            for (int s = 0; s < 2; ++s) {
                if ((int)g_stateHistEcon[s].size() < histCount) {
                    g_stateHistEcon[s].push_back(g_stateCurEcon[s]);
                } else if (!g_stateHistEcon[s].empty()) {
                    g_stateHistEcon[s].back() = g_stateCurEcon[s];
                }
            }
        } else {
            g_stateHistEcon[0].push_back(g_stateCurEcon[0]);
            g_stateHistEcon[1].push_back(g_stateCurEcon[1]);
        }

        ClearStateEconPeriod(&g_stateCurEcon[0]);
        ClearStateEconPeriod(&g_stateCurEcon[1]);
        TwoStates_BumpHistSwapGen();
    }

    if (o_statRecordClear) {
        o_statRecordClear(rcxWorld, rec);
    } else {
        StatRecordClearFallback(world, rec);
    }

    if (isCurStat) {
        if (t_inEconHook && g_econBeforeMatchesLast) {
            g_econBeforeScalars    = g_lastWorldScalars;
            g_econBeforeTrackerCur = g_lastWorldTrackerCur;
            g_econBeforeMatchesLast = false;
        }
        SafeReadStatScalars(rec, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
        SafeReadTrackerSlice(world, false, &g_lastWorldTrackerCur);
        memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
    }

    if (wasUiSwapped && !g_inSimStep && !g_inCommand) {
        TwoStates_UiBegin();
    }
}

void TwoStates_ResetEconStatsForTest(BYTE* world)
{
    TwoStates_RestoreUiMoneySwap();
    ClearStateEconPeriod(&g_stateCurEcon[0]);
    ClearStateEconPeriod(&g_stateCurEcon[1]);
    g_stateHistEcon[0].clear();
    g_stateHistEcon[1].clear();
    TwoStates_BumpHistSwapGen();
    ResetStateHudPopCache();
    ResetStateTrackerSlices();
    memset(g_testLedgerPool, 0, sizeof(g_testLedgerPool));
    memset(&g_lastWorldScalars, 0, sizeof(g_lastWorldScalars));
    memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
    g_netTradeRub[0] = g_netTradeRub[1] = 0.0;
    g_netTradeUsd[0] = g_netTradeUsd[1] = 0.0;
    g_stateEconInitialized = false;
    if (world) {
        EnsureStateEconInitialized(world);
    }
}

void TwoStates_LedgerAddForTest(BYTE* world, const SovietStatResEntry* row, BYTE* ledgerVec, float mult)
{
    H_LedgerAdd(world, row, ledgerVec, 0.0f, 0, mult, 0);
}

void TwoStates_ConstructStepBatchForTest(BYTE* world, BYTE* building, const SovietStatResEntry* entries,
                                         const int* vecIdxs, int count)
{
    if (!world || !building || !entries || !vecIdxs || count <= 0) return;
    ConstructEconScope scope(world, building);
    BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;
    for (int i = 0; i < count; ++i) {
        int vIdx = vecIdxs[i];
        if (vIdx >= 0 && vIdx < STAT_RES_VEC_COUNT) {
            LedgerAddFallback(world, &entries[i], curStat + STAT_RES_VEC_BASE_OFF + (size_t)vIdx * STAT_RES_VEC_STRIDE, 1.0f);
        }
    }
}

struct ForeignPowerTradeMockCtx {
    uintptr_t resPtr = 0;
    float mwh = 0.0f;
    float costVal = 0.0f;
    bool isUsd = false;
    bool isExport = false;
};
static ForeignPowerTradeMockCtx g_foreignPowerMockCtx = {};

static __int64 __fastcall MockForeignPowerStep(BYTE* world, BYTE* /*building*/, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    double delta = g_foreignPowerMockCtx.isExport ? (double)g_foreignPowerMockCtx.costVal : -(double)g_foreignPowerMockCtx.costVal;
    if (g_foreignPowerMockCtx.isUsd) {
        *WorldDollars() += delta;
    } else {
        *WorldRubles() += delta;
    }
    int vecIdx = (g_foreignPowerMockCtx.isUsd ? 2 : 0) + (g_foreignPowerMockCtx.isExport ? 1 : 0);
    SovietStatResEntry row = { g_foreignPowerMockCtx.resPtr, g_foreignPowerMockCtx.mwh, g_foreignPowerMockCtx.costVal };
    BYTE* ledgerVec = world + WORLD_STAT_CURRENT_OFFSET + STAT_RES_VEC_BASE_OFF + (size_t)vecIdx * STAT_RES_VEC_STRIDE;
    H_LedgerAdd(world, &row, ledgerVec, 0.0f, 0, 1.0f, 0);
    return 0;
}

void TwoStates_ForeignPowerTradeForTest(BYTE* world, BYTE* building, uintptr_t elEnergyResPtr,
                                        float mwh, float costVal, bool isUsd, bool isExport)
{
    if (!world || !building) return;
    g_foreignPowerMockCtx = { elEnergyResPtr, mwh, costVal, isUsd, isExport };
    BldEconFn prev = o_customsVehicle;
    o_customsVehicle = &MockForeignPowerStep;
    H_CustomsVehicle(world, building, 0, 0, 0, 0);
    o_customsVehicle = prev;
}

void TwoStates_StatRecordClearForTest(BYTE* world, BYTE* rec)
{
    g_inStatClearTest = true;
    H_StatRecordClear(world, rec);
    g_inStatClearTest = false;
}

bool TwoStates_GetStateEconResEntry(int state, int vecIdx, uintptr_t resourcePtr, float* outTons, float* outValue)
{
    if (outTons)  *outTons  = 0.0f;
    if (outValue) *outValue = 0.0f;
    if ((state != 1 && state != 2) || vecIdx < 0 || vecIdx >= STAT_RES_VEC_COUNT) return false;
    const std::vector<SovietStatResEntry>& vec = g_stateCurEcon[state - 1].resVecs[vecIdx];
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i].resourcePtr == resourcePtr) {
            if (outTons)  *outTons  = vec[i].amountTons;
            if (outValue) *outValue = vec[i].moneyValue;
            return true;
        }
    }
    return false;
}

void TwoStates_GetStateEconFloat4ForTest(int state, bool loansSlice, float outVals[4])
{
    if (!outVals) return;
    memset(outVals, 0, 4 * sizeof(float));
    if (state == 1 || state == 2) {
        const float* src = loansSlice
            ? g_stateCurEcon[state - 1].scalars.loans
            : g_stateCurEcon[state - 1].scalars.vehicles;
        memcpy(outVals, src, 4 * sizeof(float));
    }
}

int TwoStates_GetStateEconHistoryCount(int state)
{
    if (state != 1 && state != 2) return 0;
    return (int)g_stateHistEcon[state - 1].size();
}

int32_t TwoStates_GetStateEconExtraCountForTest(int state, int idx)
{
    if ((state != 1 && state != 2) || idx < 0 || idx >= 21) return 0;
    return g_stateCurEcon[state - 1].scalars.extraCounts[idx];
}

void TwoStates_SetStateEconExtraCountForTest(int state, int idx, int32_t val)
{
    if ((state == 1 || state == 2) && idx >= 0 && idx < 21) {
        g_stateCurEcon[state - 1].scalars.extraCounts[idx] = val;
    }
}

float TwoStates_GetStateEconExtraFloatForTest(int state, int idx)
{
    if ((state != 1 && state != 2) || idx < 0 || idx >= 21) return 0.0f;
    float f = 0.0f;
    memcpy(&f, &g_stateCurEcon[state - 1].scalars.extraCounts[idx], sizeof(float));
    return f;
}

void TwoStates_SetStateEconExtraFloatForTest(int state, int idx, float val)
{
    if ((state == 1 || state == 2) && idx >= 0 && idx < 21) {
        memcpy(&g_stateCurEcon[state - 1].scalars.extraCounts[idx], &val, sizeof(float));
    }
}

float TwoStates_GetStateEconTourismFloatForTest(int state, int idx)
{
    if ((state != 1 && state != 2) || idx < 0 || idx >= 44) return 0.0f;
    return g_stateCurEcon[state - 1].scalars.tourism[idx];
}

void TwoStates_CloseDenominationPopups(BYTE* world)
{
    if (!world) return;
    BYTE* const* begin = nullptr;
    BYTE* const* end = nullptr;
    if (!SafeReadPod(world + WORLD_OPEN_WINDOWS_BEGIN_OFFSET, &begin) ||
        !SafeReadPod(world + WORLD_OPEN_WINDOWS_END_OFFSET, &end)) {
        return;
    }
    if (!begin || !end || end <= begin) return;
    const uintptr_t byteSpan = (uintptr_t)end - (uintptr_t)begin;
    if ((byteSpan % sizeof(BYTE*)) != 0 || byteSpan > 4096u * sizeof(BYTE*)) return;

    const size_t count = (size_t)(byteSpan / sizeof(BYTE*));
    for (size_t i = 0; i < count; ++i) {
        BYTE* win = nullptr;
        if (!SafeReadPod(begin + i, &win) || !IsPointerLikeX64((uint64_t)(uintptr_t)win)) continue;
        int32_t winType = 0;
        if (SafeReadPod(win + WINDOW_POPUP_TYPE_OFFSET, &winType) &&
            winType == WINDOW_POPUP_TYPE_DENOMINATION) {
            SafeWritePod(win, (BYTE)1);
        }
    }
}

static void DivideFloatSlice(float* arr, size_t count, float divisor)
{
    if (!arr || divisor == 0.0f) return;
    for (size_t i = 0; i < count; ++i) {
        if (isfinite(arr[i])) arr[i] /= divisor;
    }
}

static void DivideStateEconPeriodBy100(StateEconPeriodStats* st)
{
    if (!st) return;
    const float divF = (float)CURRENCY_DENOMINATION_DIVISOR;
    // Matches 0x2F9E50: divides moneyValue (+0x0C) in resVecs[0..7] only (leaves amountTons and resVecs[8..15] untouched).
    for (size_t v = 0; v < 8; ++v) {
        for (size_t i = 0; i < st->resVecs[v].size(); ++i) {
            if (isfinite(st->resVecs[v][i].moneyValue)) {
                st->resVecs[v][i].moneyValue /= divF;
            }
        }
    }
    DivideFloatSlice(st->scalars.vehicles, 4, divF);
    DivideFloatSlice(st->scalars.immigrantSpend, 2, divF);
    DivideFloatSlice(st->scalars.tourism, 44, divF);
    DivideFloatSlice(st->scalars.loans, 4, divF);
}

void TwoStates_ApplyDenomination(BYTE* world)
{
    TwoStates_CloseDenominationPopups(world);
    if (!TwoStates_IsActive() || !world || !g_exeBase) return;
    TwoStates_RestoreUiMoneySwap();

    const double divD = CURRENCY_DENOMINATION_DIVISOR;
    for (int i = 0; i < 2; ++i) {
        g_netTradeRub[i] /= divD;
        g_netTradeUsd[i] /= divD;
    }
    if (!IsUnlimitedMoney()) {
        for (int i = 0; i < 2; ++i) {
            g_stateRub[i] /= divD;
            g_stateUsd[i] /= divD;
        }
        const double totalRub = g_stateRub[0] + g_stateRub[1];
        const double totalUsd = g_stateUsd[0] + g_stateUsd[1];
        SafeWritePod(WorldRubles(), totalRub);
        SafeWritePod(WorldDollars(), totalUsd);
        g_lastWorldRub = totalRub;
        g_lastWorldUsd = totalUsd;
    } else {
        SafeReadPod(WorldRubles(), &g_lastWorldRub);
        SafeReadPod(WorldDollars(), &g_lastWorldUsd);
    }
    g_unattributedRub = 0.0;
    g_unattributedUsd = 0.0;

    if (g_stateEconInitialized) {
        DivideStateEconPeriodBy100(&g_stateCurEcon[0]);
        DivideStateEconPeriodBy100(&g_stateCurEcon[1]);
        for (int s = 0; s < 2; ++s) {
            for (size_t i = 0; i < g_stateHistEcon[s].size(); ++i) {
                DivideStateEconPeriodBy100(&g_stateHistEcon[s][i]);
            }
        }
        TwoStates_BumpHistSwapGen();
    } else {
        EnsureStateEconInitialized(world);
    }
    SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_lastWorldScalars);
    g_unattributedScalars = {};
    g_econBeforeMatchesLast = false;
    TwoStates_InvalidateUiCitizenCache();
}

void TwoStates_GetNetTradeForTest(int state, double* outRub, double* outUsd)
{
    if (outRub) *outRub = (state == 1 || state == 2) ? g_netTradeRub[state - 1] : 0.0;
    if (outUsd) *outUsd = (state == 1 || state == 2) ? g_netTradeUsd[state - 1] : 0.0;
}

static int FindOwnerLoanGlobalIdx(const SovietLoan* b, int count, bool filterByState, int targetSubType, const LsLoanCommand& cmd)
{
    if (!b || count <= 0) return -1;
    int ord = 0;
    for (int i = 0; i < count; ++i) {
        int sub = (b[i].loanSubType == 1) ? 1 : 0;
        if (!filterByState || sub == targetSubType) {
            if (ord == cmd.ownerLoanOrd) return i;
            ord++;
        }
    }
    for (int i = 0; i < count; ++i) {
        int sub = (b[i].loanSubType == 1) ? 1 : 0;
        if ((!filterByState || sub == targetSubType) &&
            b[i].loanType == cmd.loan.loanType &&
            fabsf(b[i].initialPrincipal - cmd.loan.initialPrincipal) < 1.0f &&
            b[i].initialDays == cmd.loan.initialDays) {
            return i;
        }
    }
    return -1;
}

static void NotifyLoanActionToast(int ownerState, const LsLoanCommand& cmd)
{
    if (!TwoStates_IsActive() || ownerState != TwoStates_LocalState()) return;
    bool isUsd = (cmd.loan.loanType == LOAN_CURRENCY_USD);
    double amount = isUsd ? fabs(cmd.dUsd) : fabs(cmd.dRub);
    if (amount < 1.0) amount = (double)cmd.loan.initialPrincipal;
    const char* curSym = isUsd ? "$" : Tr("₽", "RUB");
    char toast[220];
    if (cmd.op == LOAN_OP_BORROW) {
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("★ %s взяла кредит: +%.0f %s", "★ %s took a loan: +%.0f %s"),
                    TwoStates_StateName(ownerState), amount, curSym);
    } else if (cmd.op == LOAN_OP_REPAY_FULL) {
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("★ %s полностью погасила кредит: -%.0f %s", "★ %s repaid loan in full: -%.0f %s"),
                    TwoStates_StateName(ownerState), amount, curSym);
    } else {
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("★ %s погасила часть кредита: -%.0f %s", "★ %s partially repaid loan: -%.0f %s"),
                    TwoStates_StateName(ownerState), amount, curSym);
    }
    TwoStates_AddToast(toast, 1);
}

bool TwoStates_ApplyLoanCommand(const LsLoanCommand& cmd)
{
    if (!g_exeBase) return false;
    if (cmd.op != LOAN_OP_BORROW && cmd.op != LOAN_OP_REPAY_PARTIAL && cmd.op != LOAN_OP_REPAY_FULL)
        return false;
    if (!isfinite(cmd.dRub) || !isfinite(cmd.dUsd))
        return false;

    BYTE* world = g_exeBase + WORLD_RVA;
    int ownerState = (cmd.ownerSubType == 1) ? 2 : (g_inCommand ? g_cmdOwner : 1);
    bool filterByState = TwoStates_IsActive();
    int targetSubType = (filterByState && ownerState == 2) ? 1 : 0;

    if (cmd.op == LOAN_OP_BORROW) {
        SovietLoan newLoan = cmd.loan;
        newLoan.loanSubType = targetSubType;
        if (!SafePushBackLoan(world, newLoan)) return false;
    } else {
        SovietLoan* b = nullptr;
        int count = 0, capCount = 0;
        if (!SafeReadLoanVectorHeader(world, &b, &count, &capCount) || !b || count <= 0) return false;
        int globalIdx = FindOwnerLoanGlobalIdx(b, count, filterByState, targetSubType, cmd);
        if (globalIdx < 0) return false;

        if (cmd.op == LOAN_OP_REPAY_PARTIAL) {
            SovietLoan updated = cmd.loan;
            updated.loanSubType = targetSubType;
            if (!SafeWritePod(&b[globalIdx], updated)) return false;
        } else {
            if (!SafeEraseLoanAt(world, globalIdx)) return false;
        }
    }

    if (filterByState && !IsUnlimitedMoney()) {
        int idx = ownerState - 1;
        g_stateRub[idx] += cmd.dRub;
        g_stateUsd[idx] += cmd.dUsd;
        if (g_stateRub[idx] < 0.0) g_stateRub[idx] = 0.0;
        if (g_stateUsd[idx] < 0.0) g_stateUsd[idx] = 0.0;
        if (g_inCommand) {
            SafeWritePod(WorldRubles(), g_stateRub[g_cmdOwner - 1]);
            SafeWritePod(WorldDollars(), g_stateUsd[g_cmdOwner - 1]);
        } else {
            SafeWritePod(WorldRubles(), g_stateRub[0] + g_stateRub[1]);
            SafeWritePod(WorldDollars(), g_stateUsd[0] + g_stateUsd[1]);
        }
    } else {
        double curRub = 0.0, curUsd = 0.0;
        if (SafeReadPod(WorldRubles(), &curRub) && SafeReadPod(WorldDollars(), &curUsd)) {
            double nextRub = curRub + cmd.dRub;
            double nextUsd = curUsd + cmd.dUsd;
            if (nextRub < 0.0) nextRub = 0.0;
            if (nextUsd < 0.0) nextUsd = 0.0;
            SafeWritePod(WorldRubles(), nextRub);
            SafeWritePod(WorldDollars(), nextUsd);
        }
    }
    SafeAddLoanStatsDelta(world, cmd.dStats);
    if (filterByState && !g_inCommand) {
        EnsureStateEconInitialized(world);
        for (int i = 0; i < 4; ++i) {
            if (isfinite(cmd.dStats[i])) {
                g_stateCurEcon[ownerState - 1].scalars.loans[i] += cmd.dStats[i];
            }
        }
    }
    NotifyLoanActionToast(ownerState, cmd);
    return true;
}

static void AccumulateLoansForState(const SovietLoan* loans, int count, int state,
                                    double* outRubDebt, double* outUsdDebt, int* outLoanCount)
{
    double rub = 0.0, usd = 0.0;
    int matched = 0;
    if (loans && count > 0) {
        DiagLog_BeginSafeSeh();
        __try {
            for (int i = 0; i < count; ++i) {
                int loanState = (loans[i].loanSubType == 1) ? 2 : 1;
                if (state != 0 && loanState != state) continue;
                double d = (double)loans[i].remainingPrincipal + (double)loans[i].unpaidPenaltyBalance;
                if (loans[i].loanType == LOAN_CURRENCY_USD) usd += d;
                else rub += d;
                matched++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
    }
    if (outRubDebt) *outRubDebt = rub;
    if (outUsdDebt) *outUsdDebt = usd;
    if (outLoanCount) *outLoanCount = matched;
}

void TwoStates_GetLoanDebt(int state, double* outRubDebt, double* outUsdDebt, int* outLoanCount)
{
    if (outRubDebt) *outRubDebt = 0.0;
    if (outUsdDebt) *outUsdDebt = 0.0;
    if (outLoanCount) *outLoanCount = 0;
    if (!g_exeBase) return;

    EnsureMoneySwapLock();
    EnterCriticalSection(&g_moneySwapLock);
    if (g_uiLoansSwapped) {
        AccumulateLoansForState(g_uiFullLoansBackup.data(), (int)g_uiFullLoansBackup.size(),
                                state, outRubDebt, outUsdDebt, outLoanCount);
        LeaveCriticalSection(&g_moneySwapLock);
        return;
    }
    LeaveCriticalSection(&g_moneySwapLock);

    SovietLoan* b = nullptr;
    int count = 0, capCount = 0;
    if (SafeReadLoanVectorHeader(g_exeBase + WORLD_RVA, &b, &count, &capCount) && b && count > 0) {
        AccumulateLoansForState(b, count, state, outRubDebt, outUsdDebt, outLoanCount);
    }
}

static void BackfillLegacyHistCitizenScalars(int sIdx)
{
    if (sIdx < 0 || sIdx > 1) return;
    std::vector<StateEconPeriodStats>& hist = g_stateHistEcon[sIdx];
    if (g_histBackfillDone[sIdx] && g_histBackfillCheckedLen[sIdx] == hist.size()) return;
    const StateEconScalars& cur = g_stateCurEcon[sIdx].scalars;
    if (cur.citizenAverages[1] <= 0.0f) return;

    g_histBackfillScanCount++;
    int curPop = cur.demographics[0] + cur.demographics[1] + cur.demographics[2] + cur.demographics[3];
    int curAdults = cur.demographics[3];
    bool anyModified = false;
    for (size_t i = 0; i < hist.size(); ++i) {
        StateEconScalars& hs = hist[i].scalars;
        int histPop = hs.demographics[0] + hs.demographics[1] + hs.demographics[2] + hs.demographics[3];
        if (histPop > 0 && hs.citizenAverages[1] <= 0.0f) {
            anyModified = true;
            memcpy(hs.satisfaction, cur.satisfaction, sizeof(hs.satisfaction));
            memcpy(hs.citizenAverages, cur.citizenAverages, sizeof(hs.citizenAverages));
            if (hs.demographics[3] <= 0) hs.citizenAverages[0] = 0.0f;
            float popRatio = (curPop > 0) ? ((float)histPop / (float)curPop) : 1.0f;
            float adultRatio = (curAdults > 0) ? ((float)hs.demographics[3] / (float)curAdults) : popRatio;
            for (int b = 0; b < 9; ++b) {
                float r = (b == 0 || (b >= 4 && b <= 7)) ? adultRatio : popRatio;
                hs.citizenBrackets[b] = (int32_t)lroundf((float)cur.citizenBrackets[b] * r);
            }
        }
    }
    if (anyModified) {
        TwoStates_BumpHistSwapGen();
    }
    g_histBackfillDone[sIdx] = true;
    g_histBackfillCheckedLen[sIdx] = hist.size();
}

struct UiBeginReadHeader {
    bool ok;
    bool unlimitedMoney;
    double worldRub;
    double worldUsd;
    uintptr_t citHdr[3];
    BYTE* histBegin;
    int histCount;
    SovietLoan* loanBegin;
    int loanCount;
    int loanCapCount;
    int loanSelectedIdx;
    float loanStats[4];
};

static bool UiBeginReadWorldHeaderSeh(const BYTE* world, UiBeginReadHeader* out)
{
    memset(out, 0, sizeof(*out));
    out->loanSelectedIdx = -1;
    if (!world) return false;
    DiagLog_BeginSafeSeh();
    __try {
        out->unlimitedMoney = (*(const int*)(world + WORLD_UNLIMITED_MONEY_OFFSET) != 0);
        out->worldRub = *WorldRubles();
        out->worldUsd = *WorldDollars();
        memcpy(out->citHdr, world + WORLD_CITIZENS_OFFSET, sizeof(out->citHdr));

        BYTE* hb = *(BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET);
        BYTE* he = *(BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET + 8);
        BYTE* hc = *(BYTE* const*)(world + WORLD_STAT_HISTORY_OFFSET + 16);
        if (hb && he >= hb && hc >= he) {
            ptrdiff_t hBytes = he - hb;
            if (hBytes >= 0 && (hBytes % STAT_RECORD_SIZE) == 0 && hBytes <= 4096 * STAT_RECORD_SIZE) {
                out->histBegin = hb;
                out->histCount = (int)(hBytes / STAT_RECORD_SIZE);
            }
        }

        ParseLoanVectorHeaderRaw(world, &out->loanBegin, &out->loanCount, &out->loanCapCount);
        out->loanSelectedIdx = (int)(*(const int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET));
        memcpy(out->loanStats, world + WORLD_LOAN_STATS_OFFSET, 4 * sizeof(float));
        out->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->ok = false;
    }
    DiagLog_EndSafeSeh();
    return out->ok;
}

struct UiBeginCommitArgs {
    bool writeMoney;
    double shownRub;
    double shownUsd;
    bool swapEcon;
    bool writeCitHdr;
    uintptr_t newCitHdr[3];
    const StatRecordUiBackup* curPayload;
    StatRecordUiBackup* savedCurStat;
    BYTE* histBegin;
    int histCount;
    int swapCount;
    bool useHistBackupCache;
    StatRecordUiBackup* savedHistStats;
    const StatRecordUiBackup* preparedHistSwap;
    uint8_t* histDirtyMask;
    int sIdx;
    int localPop;
    const BYTE* hudPopBlock;
    bool resetTimers;
    const StateEconScalars* curScalars;
    const StateTrackerSlice* trackerCur;
    const StateTrackerSlice* trackerOld;
    bool markGraphsDirty;
    BYTE* savedHudPopBlock;
    StateEconScalars* savedPopWinScalars;
    BYTE* savedTrackerBlock;
    bool filterLoans;
    bool writeLoans;
    const SovietLoan* filteredLoans;
    int filteredLoanCount;
    bool writeLoanSelected;
    int localLoanSelectedIdx;
    bool* outHistBackupOk;
    bool* outCitSwapped;
    bool* outHudSwapped;
    bool* outTrackerSwapped;
    bool* outLoansSwapped;
};

static void UiBeginCommitEconRaw(BYTE* world, const UiBeginCommitArgs* a)
{
    if (a->writeCitHdr) {
        memcpy(world + WORLD_CITIZENS_OFFSET, a->newCitHdr, sizeof(a->newCitHdr));
        *a->outCitSwapped = true;
    }
    CopyStatBlocksFromRecRaw(world + WORLD_STAT_CURRENT_OFFSET, a->savedCurStat, kUiHistBlocks);
    WriteStatRecordDirtyBlocksRaw(world + WORLD_STAT_CURRENT_OFFSET, *a->curPayload, 0x7u);

    if (a->histBegin && a->histCount > 0) {
        if (!a->useHistBackupCache) {
            for (int i = 0; i < a->histCount; ++i) {
                BYTE* rec = a->histBegin + (size_t)i * STAT_RECORD_SIZE;
                CopyStatBlocksFromRecRaw(rec, &a->savedHistStats[(size_t)i], kUiHistBlocks);
                uint8_t mask = (i < a->swapCount && a->preparedHistSwap)
                    ? ComputeStatRecordDirtyMask(a->savedHistStats[(size_t)i], a->preparedHistSwap[(size_t)i])
                    : 0;
                a->histDirtyMask[(size_t)i] = mask;
                if (mask) {
                    WriteStatRecordDirtyBlocksRaw(rec, a->preparedHistSwap[(size_t)i], mask);
                }
            }
        } else if (a->preparedHistSwap && a->histDirtyMask) {
            for (int i = 0; i < a->swapCount; ++i) {
                uint8_t mask = a->histDirtyMask[(size_t)i];
                if (mask) {
                    WriteStatRecordDirtyBlocksRaw(a->histBegin + (size_t)i * STAT_RECORD_SIZE,
                                                  a->preparedHistSwap[(size_t)i], mask);
                }
            }
        }
        *a->outHistBackupOk = true;
    }

    memcpy(a->savedHudPopBlock, world + WORLD_HUD_POP_BLOCK_OFFSET, WORLD_HUD_POP_BLOCK_SIZE);
    *a->outHudSwapped = true;
    memcpy(world + WORLD_HUD_POP_BLOCK_OFFSET, a->hudPopBlock, WORLD_HUD_POP_BLOCK_SIZE);
    *(int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET) = (int32_t)a->localPop;
    if (a->resetTimers) {
        *(float*)(world + WORLD_HUD_POP_TIMER_OFFSET) = 0.0f;
        *(float*)(world + WORLD_POP_STAT_TIMER_OFF)   = 0.0f;
    }
    CopyStatBlocksFromRecRaw(world + WORLD_POP_STAT_CURRENT_OFF, a->savedPopWinScalars, kStatScalarBlocks);
    WriteStatScalarsToRecRaw(world + WORLD_POP_STAT_CURRENT_OFF, *a->curScalars);

    memcpy(a->savedTrackerBlock, world + WORLD_TRACKER_BLOCK_OFFSET, WORLD_TRACKER_BLOCK_SIZE);
    *a->outTrackerSwapped = true;
    WriteTrackerSliceRaw(world, false, *a->trackerCur);
    WriteTrackerSliceRaw(world, true,  *a->trackerOld);

    if (a->markGraphsDirty) {
        *(BYTE*)(world + WORLD_ECON_GRAPH_DIRTY_OFF) = 1;
        *(BYTE*)(world + WORLD_POP_GRAPH_DIRTY_OFF)  = 1;
    }
}

static void UiBeginCommitWorldSwapSeh(BYTE* world, const UiBeginCommitArgs* a)
{
    if (!world || !a) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (a->writeMoney) {
            *WorldRubles()  = a->shownRub;
            *WorldDollars() = a->shownUsd;
        }
        if (a->swapEcon) {
            UiBeginCommitEconRaw(world, a);
        }
        if (a->filterLoans) {
            if (a->writeLoans) {
                SovietLoan* b = nullptr;
                int curCount = 0, capCount = 0;
                if (ParseLoanVectorHeaderRaw(world, &b, &curCount, &capCount) &&
                    a->filteredLoanCount >= 0 && a->filteredLoanCount <= capCount &&
                    (a->filteredLoanCount == 0 || (b && a->filteredLoans))) {
                    if (a->filteredLoanCount > 0 && b != a->filteredLoans) {
                        memcpy(b, a->filteredLoans, (size_t)a->filteredLoanCount * sizeof(SovietLoan));
                    }
                    if (b) {
                        *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b + a->filteredLoanCount;
                    }
                }
            }
            if (a->writeLoanSelected) {
                *(int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET) = (int32_t)a->localLoanSelectedIdx;
            }
            *a->outLoansSwapped = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *a->outHistBackupOk = false;
    }
    DiagLog_EndSafeSeh();
}

static void PrepareUiHistSwapCache(int localState, int sIdx, BYTE* histBegin, int histCount,
                                   int* outSwapCount, bool* outUseBackupCache)
{
    *outSwapCount = 0;
    *outUseBackupCache = false;
    if (!histBegin || histCount <= 0) {
        g_uiSavedHistBegin = nullptr;
        g_uiSavedHistCount = 0;
        g_uiSavedHistStats.clear();
        g_uiHistDirtyMask.clear();
        g_uiHistBackupValid = false;
        return;
    }

    const int swapCount = (int)std::min((size_t)histCount, g_stateHistEcon[sIdx].size());
    *outSwapCount = swapCount;

    if (g_uiPreparedHistSwapState != localState ||
        g_uiPreparedHistSwapCount != swapCount ||
        g_uiPreparedHistSwapGen   != g_stateHistSwapGen) {
        g_uiPreparedHistSwap.resize((size_t)swapCount);
        for (int i = 0; i < swapCount; ++i) {
            PrepareStatRecordUiPayload(g_stateHistEcon[sIdx][(size_t)i], &g_uiPreparedHistSwap[(size_t)i]);
        }
        g_uiPreparedHistSwapState = localState;
        g_uiPreparedHistSwapCount = swapCount;
        g_uiPreparedHistSwapGen   = g_stateHistSwapGen;
    }

    const bool sessionRunning = Determinism_IsActive() && Determinism_SessionRunning();
    const bool backupHit = sessionRunning && g_uiHistBackupValid &&
        g_uiSavedHistState == localState &&
        g_uiSavedHistBegin == histBegin &&
        g_uiSavedHistCount == histCount &&
        g_uiSavedHistSwapCount == swapCount &&
        g_uiSavedHistGen == g_stateHistSwapGen &&
        g_uiSavedHistStats.size() == (size_t)histCount &&
        g_uiHistDirtyMask.size() == (size_t)histCount;

    g_uiSavedHistBegin = histBegin;
    g_uiSavedHistCount = histCount;
    if (!backupHit) {
        g_uiSavedHistStats.resize((size_t)histCount);
        g_uiHistDirtyMask.resize((size_t)histCount);
        g_uiHistBackupValid = false;
    }
    *outUseBackupCache = backupHit;
}

static void PrepareUiHudPopState(int localState, int sIdx, int localPop, bool* outResetTimers, bool* outMarkGraphsDirty)
{
    const StateEconScalars& curSc = g_stateCurEcon[sIdx].scalars;
    const bool eduChanged = (g_stateLastEduBasic[sIdx] != curSc.citizenBrackets[2] ||
                             g_stateLastEduUniv[sIdx]  != curSc.citizenBrackets[3]);
    if (!g_stateHudPopInitialized[sIdx]) {
        PopulateHudPopBlockFromScalars(g_stateHudPopBlock[sIdx], curSc, localPop);
    } else if (eduChanged) {
        *(int32_t*)(g_stateHudPopBlock[sIdx] + 0x24) = curSc.demographics[3];
        *(int32_t*)(g_stateHudPopBlock[sIdx] + 0x28) = curSc.citizenBrackets[1];
        *(int32_t*)(g_stateHudPopBlock[sIdx] + 0x2C) = curSc.citizenBrackets[2];
        *(int32_t*)(g_stateHudPopBlock[sIdx] + 0x30) = curSc.citizenBrackets[3];
        memcpy(g_stateHudPopBlock[sIdx] + 0x5C, curSc.citizenBrackets, sizeof(curSc.citizenBrackets));
    }
    if (localPop <= 0) {
        memset(&g_stateTrackerCur[sIdx], 0, sizeof(StateTrackerSlice));
        memset(&g_stateTrackerOld[sIdx], 0, sizeof(StateTrackerSlice));
    }
    *outResetTimers = !g_stateHudPopInitialized[sIdx] || g_stateLastLocalPop[sIdx] != localPop ||
                      eduChanged || g_lastUiSwappedState != localState;
    *outMarkGraphsDirty = (g_lastUiSwappedState != localState ||
                           g_lastUiSwappedHistLen[sIdx] != g_stateHistEcon[sIdx].size());
}

static bool PrepareUiLoanSwap(const UiBeginReadHeader& hdr, bool filterByState,
                              bool* outWriteLoans, bool* outWriteSelected)
{
    *outWriteLoans = false;
    *outWriteSelected = false;
    if (hdr.loanCount > 0 && hdr.loanBegin) {
        g_uiFullLoansBackup.resize((size_t)hdr.loanCount);
        if (!SafeMemcpyPod(g_uiFullLoansBackup.data(), hdr.loanBegin, (size_t)hdr.loanCount * sizeof(SovietLoan))) {
            g_uiFullLoansBackup.clear();
            return false;
        }
    } else {
        g_uiFullLoansBackup.clear();
    }

    g_uiSavedGlobalSelectedIdx = hdr.loanSelectedIdx;
    memcpy(g_uiSavedLoanStats, hdr.loanStats, sizeof(g_uiSavedLoanStats));
    g_uiLoansFilterByState = filterByState;
    g_uiLoansOwnerSubType = (filterByState && TwoStates_LocalState() == 2) ? 1 : 0;
    g_uiFilteredLoansBefore.clear();
    g_uiFilteredToGlobalIdx.clear();
    g_uiLocalSelectedIdxBefore = -1;

    for (size_t i = 0; i < g_uiFullLoansBackup.size(); ++i) {
        int sub = (g_uiFullLoansBackup[i].loanSubType == 1) ? 1 : 0;
        if (!filterByState || sub == g_uiLoansOwnerSubType) {
            int localOrd = (int)g_uiFilteredLoansBefore.size();
            if ((int)i == g_uiSavedGlobalSelectedIdx) {
                g_uiLocalSelectedIdxBefore = localOrd;
            }
            g_uiFilteredLoansBefore.push_back(g_uiFullLoansBackup[i]);
            g_uiFilteredToGlobalIdx.push_back((int)i);
        }
    }

    if (filterByState) {
        *outWriteLoans = (!g_uiFullLoansBackup.empty() || hdr.loanCount != 0);
        *outWriteSelected = (g_uiSavedGlobalSelectedIdx != g_uiLocalSelectedIdxBefore);
    }
    return true;
}

struct UiBeginPreparedMeta {
    int localState = 0;
    int sIdx = 0;
    int localPop = 0;
    int swapCount = 0;
    bool useHistBackupCache = false;
    bool resetTimers = false;
    bool markGraphsDirty = false;
};

static void PrepareUiEconCommitState(BYTE* world, const UiBeginReadHeader& hdr,
                                     StatRecordUiBackup* curPayload, UiBeginCommitArgs* ca,
                                     UiBeginPreparedMeta* meta)
{
    int localState = TwoStates_LocalState();
    if (localState != 1 && localState != 2) localState = 1;
    const int sIdx = localState - 1;
    meta->localState = localState;
    meta->sIdx = sIdx;

    EnsureStateEconInitializedFromHdr(world, hdr.histBegin, hdr.histCount);
    g_uiDirtyCitizenLifespans.clear();
    if (!g_uiDirtyCitizenSet.empty()) g_uiDirtyCitizenSet.clear();
    const bool skipLifespanSave = (o_citComputeLifespan != nullptr);
    uintptr_t newCitHdr[3] = {};
    ca->writeCitHdr = PrepareUiCitizensAndDemographicsFromHdr(
        world, localState, hdr.citHdr, &meta->localPop, skipLifespanSave, newCitHdr);
    if (ca->writeCitHdr) {
        memcpy(ca->newCitHdr, newCitHdr, sizeof(newCitHdr));
    }
    BackfillLegacyHistCitizenScalars(sIdx);
    PrepareStatRecordUiPayload(g_stateCurEcon[sIdx], curPayload);
    PrepareUiHistSwapCache(localState, sIdx, hdr.histBegin, hdr.histCount,
                           &meta->swapCount, &meta->useHistBackupCache);
    PrepareUiHudPopState(localState, sIdx, meta->localPop, &meta->resetTimers, &meta->markGraphsDirty);

    ca->swapEcon = true;
    ca->curPayload = curPayload;
    ca->savedCurStat = &g_uiSavedCurStat;
    ca->histBegin = hdr.histBegin;
    ca->histCount = hdr.histCount;
    ca->swapCount = meta->swapCount;
    ca->useHistBackupCache = meta->useHistBackupCache;
    ca->savedHistStats = g_uiSavedHistStats.empty() ? nullptr : g_uiSavedHistStats.data();
    ca->preparedHistSwap = g_uiPreparedHistSwap.empty() ? nullptr : g_uiPreparedHistSwap.data();
    ca->histDirtyMask = g_uiHistDirtyMask.empty() ? nullptr : g_uiHistDirtyMask.data();
    ca->sIdx = sIdx;
    ca->localPop = meta->localPop;
    ca->hudPopBlock = g_stateHudPopBlock[sIdx];
    ca->resetTimers = meta->resetTimers;
    ca->curScalars = &g_stateCurEcon[sIdx].scalars;
    ca->trackerCur = &g_stateTrackerCur[sIdx];
    ca->trackerOld = &g_stateTrackerOld[sIdx];
    ca->markGraphsDirty = meta->markGraphsDirty;
    ca->savedHudPopBlock = g_uiSavedHudPopBlock;
    ca->savedPopWinScalars = &g_uiSavedPopWinScalars;
    ca->savedTrackerBlock = g_uiSavedTrackerBlock;
}

static void RecordUiBeginCommittedState(const UiBeginPreparedMeta& meta, bool citSwapped,
                                        bool hudSwapped, bool trackerSwapped, bool histBackupOk)
{
    g_uiCitVecSwapped = citSwapped;
    if (!citSwapped) g_uiSavedCitizenLifespans.clear();
    g_uiHudPopSwapped = hudSwapped;
    g_uiTrackerSwapped = trackerSwapped;
    g_uiEconSwappedState = meta.localState;
    g_uiEconSwapped = true;

    const int sIdx = meta.sIdx;
    if (hudSwapped && meta.resetTimers) {
        const StateEconScalars& curSc = g_stateCurEcon[sIdx].scalars;
        g_stateLastLocalPop[sIdx] = meta.localPop;
        g_stateLastEduBasic[sIdx] = curSc.citizenBrackets[2];
        g_stateLastEduUniv[sIdx]  = curSc.citizenBrackets[3];
    }
    if (meta.markGraphsDirty) {
        g_lastUiSwappedState = meta.localState;
        g_lastUiSwappedHistLen[sIdx] = g_stateHistEcon[sIdx].size();
    }
    if (histBackupOk && !meta.useHistBackupCache && Determinism_IsActive() && Determinism_SessionRunning()) {
        g_uiHistBackupValid = true;
        g_uiSavedHistState = meta.localState;
        g_uiSavedHistSwapCount = meta.swapCount;
        g_uiSavedHistGen = g_stateHistSwapGen;
    }
}

void TwoStates_UiBegin()
{
    EnsureMoneySwapLock();
    EnterCriticalSection(&g_moneySwapLock);
    if (g_uiMoneySwapped || g_uiLoansSwapped || g_uiEconSwapped) {
        LeaveCriticalSection(&g_moneySwapLock);
        return;
    }
    if (!g_exeBase || g_inSimStep || g_inCommand || TwoStates_IsSaveInProgress() ||
        g_beforeLoadPrepared || Determinism_WorldLoading() ||
        (o_statRecordClear && !SaveSync_IsWorldLoaded()) ||
        Determinism_GetFramePhase() != 0) {
        LeaveCriticalSection(&g_moneySwapLock);
        return;
    }

    const bool twoStates = TwoStates_IsActive();
    const bool captureLoans = twoStates || Commands_Capturing() || SaveSync_IsWaitingLobbyActive();
    if (!twoStates && !captureLoans) {
        LeaveCriticalSection(&g_moneySwapLock);
        return;
    }

    const int64_t tsUiT0 = DiagPerf_Now();
    BYTE* world = g_exeBase + WORLD_RVA;
    UiBeginReadHeader hdr = {};
    if (!UiBeginReadWorldHeaderSeh(world, &hdr)) {
        LeaveCriticalSection(&g_moneySwapLock);
        DiagPerf_Record(DIAG_PERF_TS_UI, DiagPerf_Now() - tsUiT0);
        return;
    }

    UiBeginCommitArgs ca = {};
    if (!hdr.unlimitedMoney) {
        g_uiSavedSimRub = hdr.worldRub;
        g_uiSavedSimUsd = hdr.worldUsd;
        g_uiShownRub = twoStates ? TwoStates_LocalRubles() : g_uiSavedSimRub;
        g_uiShownUsd = twoStates ? TwoStates_LocalDollars() : g_uiSavedSimUsd;
        g_uiMoneySwapped = true;
        if (twoStates) {
            ca.writeMoney = true;
            ca.shownRub = g_uiShownRub;
            ca.shownUsd = g_uiShownUsd;
        }
    }

    StatRecordUiBackup curPayload = {};
    UiBeginPreparedMeta meta = {};
    if (twoStates) {
        PrepareUiEconCommitState(world, hdr, &curPayload, &ca, &meta);
    }

    bool writeLoans = false, writeLoanSelected = false;
    bool loansPrepared = PrepareUiLoanSwap(hdr, twoStates, &writeLoans, &writeLoanSelected);
    if (loansPrepared) {
        ca.filterLoans = twoStates;
        ca.writeLoans = writeLoans;
        ca.filteredLoans = g_uiFilteredLoansBefore.empty() ? nullptr : g_uiFilteredLoansBefore.data();
        ca.filteredLoanCount = (int)g_uiFilteredLoansBefore.size();
        ca.writeLoanSelected = writeLoanSelected;
        ca.localLoanSelectedIdx = g_uiLocalSelectedIdxBefore;
    }

    bool histBackupOk = false;
    bool citSwapped = false, hudSwapped = false, trackerSwapped = false, loansSwapped = false;
    ca.outHistBackupOk = &histBackupOk;
    ca.outCitSwapped = &citSwapped;
    ca.outHudSwapped = &hudSwapped;
    ca.outTrackerSwapped = &trackerSwapped;
    ca.outLoansSwapped = &loansSwapped;

    UiBeginCommitWorldSwapSeh(world, &ca);

    if (twoStates) {
        RecordUiBeginCommittedState(meta, citSwapped, hudSwapped, trackerSwapped, histBackupOk);
    }
    g_uiLoansSwapped = loansPrepared && (!twoStates || loansSwapped);

    LeaveCriticalSection(&g_moneySwapLock);
    DiagPerf_Record(DIAG_PERF_TS_UI, DiagPerf_Now() - tsUiT0);
}

void TwoStates_UiEnd()
{
    TwoStates_FlushEconHookPerf();
    if (!g_exeBase || (!TwoStates_IsActive() && !Commands_Capturing() && !SaveSync_IsWaitingLobbyActive())) {
        TwoStates_RestoreUiMoneySwap();
        return;
    }
    const int64_t tsUiT0 = DiagPerf_Now();
    TwoStates_RestoreUiMoneySwap();
    DiagPerf_Record(DIAG_PERF_TS_UI, DiagPerf_Now() - tsUiT0);
}

void TwoStates_SetCitComputeLifespanTestHook(TwoStatesCitComputeLifespanTestFn origFn)
{
    o_citComputeLifespan = (CitComputeLifespanFn)origFn;
}

void TwoStates_InvokeCitComputeLifespanHookForTest(BYTE* world, BYTE* cit)
{
    H_CitComputeLifespan(world, cit);
}

size_t TwoStates_GetUiSavedLifespanCountForTest()
{
    return g_uiSavedCitizenLifespans.size();
}

size_t TwoStates_GetUiDirtyLifespanCountForTest()
{
    return g_uiDirtyCitizenLifespans.size();
}

float TwoStates_InvokeVehicleRefuelHookForTest(BYTE* vehicle, TwoStatesVehicleRefuelTestFn origFn,
                                             float a2, unsigned char a3)
{
    VehicleRefuelFn saved = o_vehicleRefuel;
    o_vehicleRefuel = (VehicleRefuelFn)origFn;
    float res = H_VehicleRefuel(vehicle, a2, a3, 0, 0, 0);
    o_vehicleRefuel = saved;
    return res;
}

// ---------------------------------------------------------------------------
// Econ Hook Installation
// ---------------------------------------------------------------------------

int TwoStates_InstallEconHooks()
{
    void* stubStatClear   = TwoStates_BuildRegPreservingRelayStub((void*)&H_StatRecordClear, false, true);
    void* stubMaxBorrow   = TwoStates_BuildRegPreservingRelayStub((void*)&H_GetMaxAvailableBorrow, true, false);
    void* stubLoanDay     = TwoStates_BuildRegPreservingRelayStub((void*)&H_StepOneLoanDay, false, false);
    void* stubCitLifespan = TwoStates_BuildRegPreservingRelayStub((void*)&H_CitComputeLifespan, false, false);

    int econOk = 0;
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_ECON, (void*)&H_BldEcon, (void**)&o_bldEcon,
                                kBldEconExpect, sizeof(kBldEconExpect), "republicmp two_states bld_econ");
    econOk += InstallInlineHook(g_exeBase + RVA_CUSTOMS_BORDER, (void*)&H_CustomsBorder, (void**)&o_customsBorder,
                                kCustomsBorderExpect, sizeof(kCustomsBorderExpect), "republicmp two_states customs_border");
    econOk += InstallInlineHook(g_exeBase + RVA_CUSTOMS_VEHICLE, (void*)&H_CustomsVehicle, (void**)&o_customsVehicle,
                                kCustomsVehExpect, sizeof(kCustomsVehExpect), "republicmp two_states customs_vehicle");
    econOk += InstallInlineHook(g_exeBase + RVA_CONSTRUCT_STEP, (void*)&H_ConstructStep, (void**)&o_constructStep,
                                kConstructStepExpect, sizeof(kConstructStepExpect), "republicmp two_states construct_step");
    econOk += InstallInlineHook(g_exeBase + RVA_CONSTRUCT_GROUNDWORK, (void*)&H_ConstructGroundwork,
                                (void**)&o_constructGroundwork, kConstructGroundworkExpect,
                                sizeof(kConstructGroundworkExpect), "republicmp two_states construct_groundwork");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_TOURISM_STEP, (void*)&H_BldTourismStep,
                                (void**)&o_bldTourismStep, kBldTourismStepExpect,
                                sizeof(kBldTourismStepExpect), "republicmp two_states bld_tourism_step");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_STEP_PRE_POST, (void*)&H_BldStepPrePost,
                                (void**)&o_bldStepPrePost, kBldStepPrePostExpect,
                                sizeof(kBldStepPrePostExpect), "republicmp two_states bld_step_pre_post");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_STEP_UTILITY, (void*)&H_BldStepUtility,
                                (void**)&o_bldStepUtility, kBldStepUtilityExpect,
                                sizeof(kBldStepUtilityExpect), "republicmp two_states bld_step_utility");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_STEP_PRODUCTION, (void*)&H_BldStepProduction,
                                (void**)&o_bldStepProduction, kBldStepProductionExpect,
                                sizeof(kBldStepProductionExpect), "republicmp two_states bld_step_production");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_CONSTRUCT_WORKERS, (void*)&H_BldConstructWorkers,
                                (void**)&o_bldConstructWorkers, kBldConstructWorkersExpect,
                                sizeof(kBldConstructWorkersExpect), "republicmp two_states bld_construct_workers");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_WORKER_STEP, (void*)&H_BldWorkerStep,
                                (void**)&o_bldWorkerStep, kBldWorkerStepExpect,
                                sizeof(kBldWorkerStepExpect), "republicmp two_states bld_worker_step");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_IMPORT_STEP, (void*)&H_BldImportStep,
                                (void**)&o_bldImportStep, kBldImportStepExpect,
                                sizeof(kBldImportStepExpect), "republicmp two_states bld_import_step");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_PRODUCTIVITY_STEP, (void*)&H_BldProductivityStep,
                                (void**)&o_bldProductivityStep, kBldProductivityStepExpect,
                                sizeof(kBldProductivityStepExpect), "republicmp two_states bld_productivity_step");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_EXPORT_STEP, (void*)&H_BldExportStep,
                                (void**)&o_bldExportStep, kBldExportStepExpect,
                                sizeof(kBldExportStepExpect), "republicmp two_states bld_export_step");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_CONSUME_RESOURCE, (void*)&H_BldConsumeResource,
                                (void**)&o_bldConsumeRes, kBldConsumeResExpect,
                                sizeof(kBldConsumeResExpect), "republicmp two_states bld_consume_res");
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_UTILITY_BORDER_STEP, (void*)&H_BldUtilityBorderStep,
                                (void**)&o_bldUtilityBorderStep, kBldUtilityBorderStepExpect,
                                sizeof(kBldUtilityBorderStepExpect), "republicmp two_states bld_utility_border_step");
    econOk += InstallInlineHook(g_exeBase + RVA_FOREIGN_LABOR_STEP, (void*)&H_ForeignLaborStep,
                                (void**)&o_foreignLaborStep, kForeignLaborStepExpect,
                                sizeof(kForeignLaborStepExpect), "republicmp two_states foreign_labor_step");
    econOk += InstallInlineHook(g_exeBase + RVA_CALC_STORED_RESOURCES, (void*)&H_CalcStoredResources,
                                (void**)&o_calcStoredResources, kCalcStoredResourcesExpect,
                                sizeof(kCalcStoredResourcesExpect), "republicmp two_states calc_stored_resources");
    econOk += InstallInlineHook(g_exeBase + RVA_VEHICLE_SALE, (void*)&H_VehicleSale, (void**)&o_vehicleSale,
                                kVehicleSaleExpect, sizeof(kVehicleSaleExpect), "republicmp two_states vehicle_sale");
    econOk += InstallInlineHook(g_exeBase + RVA_VEHICLE_BORDER_EXPORT, (void*)&H_VehicleBorderExport,
                                (void**)&o_vehicleBorderExport, kVehicleBorderExportExpect,
                                sizeof(kVehicleBorderExportExpect), "republicmp two_states vehicle_border_export");
    econOk += InstallInlineHook(g_exeBase + RVA_VEHICLE_CARGO_TRADE, (void*)&H_VehicleCargoTrade,
                                (void**)&o_vehicleCargoTrade, kVehicleCargoTradeExpect,
                                sizeof(kVehicleCargoTradeExpect), "republicmp two_states vehicle_cargo_trade");
    econOk += InstallInlineHook(g_exeBase + RVA_VEHICLE_REFUEL, (void*)&H_VehicleRefuel,
                                (void**)&o_vehicleRefuel, kVehicleRefuelExpect,
                                sizeof(kVehicleRefuelExpect), "republicmp two_states vehicle_refuel");
    econOk += InstallInlineHook(g_exeBase + RVA_FOREIGN_WORKER_BOARD, (void*)&H_ForeignWorkerBoard,
                                (void**)&o_foreignWorkerBoard, kForeignWorkerBoardExpect,
                                sizeof(kForeignWorkerBoardExpect), "republicmp two_states foreign_worker_board");
    econOk += InstallInlineHook(g_exeBase + RVA_LEDGER_ADD, (void*)&H_LedgerAdd, (void**)&o_ledgerAdd,
                                kLedgerAddExpect, sizeof(kLedgerAddExpect), "republicmp two_states ledger_add");
    econOk += InstallInlineHook(g_exeBase + RVA_STAT_RECORD_CLEAR, stubStatClear,
                                (void**)&o_statRecordClear, kStatRecordClearExpect,
                                sizeof(kStatRecordClearExpect), "republicmp two_states stat_record_clear");
    econOk += InstallInlineHook(g_exeBase + RVA_GET_MAX_AVAILABLE_BORROW, stubMaxBorrow,
                                (void**)&o_getMaxAvailableBorrow, kGetMaxBorrowExpect, sizeof(kGetMaxBorrowExpect),
                                "republicmp two_states get_max_borrow");
    econOk += InstallInlineHook(g_exeBase + RVA_STEP_ONE_LOAN_DAY, stubLoanDay,
                                (void**)&o_stepOneLoanDay, kStepOneLoanDayExpect, sizeof(kStepOneLoanDayExpect),
                                "republicmp two_states step_one_loan_day");
    econOk += InstallInlineHook(g_exeBase + RVA_CIT_COMPUTE_LIFESPAN, stubCitLifespan,
                                (void**)&o_citComputeLifespan, kCitComputeLifespanExpect, sizeof(kCitComputeLifespanExpect),
                                "republicmp two_states cit_compute_lifespan");
    return econOk;
}

bool TwoStates_AreEconHooksInstalled()
{
    return o_statRecordClear != nullptr;
}
