// SPDX-License-Identifier: GPL-3.0-only
// See two_states.h.
#include "two_states.h"
#include "save_sync.h"
#include "mod_sync.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "../game/save_load.h"
#include "../game/window_focus.h"
#include "../lockstep/commands.h"
#include "../lockstep/lockstep_net.h"
#include "../determinism/determinism.h"
#include "../ui/lang.h"
#include "../ui/native_menu.h"
#include "../ui/sync_block.h"
#include "imgui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <filesystem>
#include <system_error>
#include <algorithm>

#define DLL_C3D "C3DDLL64.dll"

void TwoStates_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile int g_chosenMode = COOP_MODE_ONE_STATE;
static volatile int g_sessionMode = COOP_MODE_ONE_STATE;
static volatile bool g_inSession = false;
static volatile bool g_isHost = true;
static float g_borderHalfWidth = 250.0f; // [-250m .. +250m] = 500m shared border zone
static bool g_fullStartingCapital = true;
static volatile bool g_expandBorders = false; // default: standard 20x20 km map with borders (16x16 km playable zone)
static bool g_show3DBorder = true;
static volatile bool g_sendClientLogs = true;

static double g_stateRub[2] = { 0.0, 0.0 };
static double g_stateUsd[2] = { 0.0, 0.0 };
static double g_netTradeRub[2] = { 0.0, 0.0 };
static double g_netTradeUsd[2] = { 0.0, 0.0 };
static double g_lobbyBaseWorldRub = 0.0;
static double g_lobbyBaseWorldUsd = 0.0;
static bool g_haveLobbyBaseMoney = false;
static bool g_treasuryInitialized = false;
static bool g_loadedFromCoopSave = false;
static volatile bool g_importingSpSave = false;
static char g_savedJoinAddress[64] = "";

static constexpr ULONGLONG PING_LIFETIME_MS = 20000;
static bool      g_localPingActive = false;
static float     g_localPingX      = 0.0f;
static float     g_localPingY      = 0.0f;
static float     g_localPingZ      = 0.0f;
static ULONGLONG g_localPingSentMs = 0;

static INIT_ONCE g_ownerLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_ownerLock;
static std::unordered_map<int, int> g_buildingOwners; // buildingId -> 1 (West) or 2 (East)
static std::unordered_map<int, int> g_vehicleOwners;  // vehicleId  -> 1 (West) or 2 (East)

struct ToastMessage {
    char text[256];
    int colorKind; // 0=info(blue), 1=success(green), 2=warning(yellow), 3=error(red)
    ULONGLONG createdMs;
    ULONGLONG durationMs;
};
static std::deque<ToastMessage> g_toasts;

static void EnsureOwnerLock()
{
    TsmEnsureCriticalSection(&g_ownerLockOnce, &g_ownerLock);
}

void TwoStates_AddToast(const char* text, int colorKind)
{
    if (!text || !text[0]) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    ToastMessage tm = {};
    strncpy_s(tm.text, sizeof(tm.text), text, _TRUNCATE);
    tm.colorKind = colorKind;
    tm.createdMs = GetTickCount64();
    tm.durationMs = (colorKind >= 2) ? 5500 : 4500;
    g_toasts.push_back(tm);
    while (g_toasts.size() > 5) g_toasts.pop_front();
    LeaveCriticalSection(&g_ownerLock);
}

static double* WorldRubles()  { return (double*)(g_exeBase + WORLD_RVA + WORLD_RUB_OFFSET); }
static double* WorldDollars() { return (double*)(g_exeBase + WORLD_RVA + WORLD_USD_OFFSET); }
static bool IsUnlimitedMoney()
{
    if (!g_exeBase) return false;
    __try {
        return *(const int*)(g_exeBase + WORLD_RVA + WORLD_UNLIMITED_MONEY_OFFSET) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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

struct StateEconScalars {
    float   vehicles[4];         // +0x188..+0x197 ($Vehicles_ImportUSD/RUB, $Vehicles_ExportUSD/RUB)
    float   immigrantSpend[2];   // +0x240..+0x247 ($Economy_ImigrantCitySpendUSD/RUB)
    int32_t citizenCounts[5];    // +0x248..+0x25B ($Citizens_Born, Dead, Escaped, ImigrantSoviet, ImigrantAfrica)
    float   satisfaction[10];    // +0x25C..+0x283 (Health, Food, Clothes, Alcohol, Culture, Sport, Religion, Loyalty, Happiness, Electronics)
    float   citizenAverages[3];  // +0x2AC..+0x2B7 (Productivity, LifeExpectancy, AverageAge)
    int32_t demographics[4];     // +0x2B8..+0x2C7 ($Citizens_SmallChilds, MediumChilds, AdultsParent, Adults at +0x2C4)
    int32_t citizenBrackets[9];  // +0x2C8..+0x2EB (AdultsWithParents, EduNone, EduBasic, EduUniv, ElecNone, Radio, TV, Comp, Cars)
    int32_t extraCounts[21];     // +0x2EC..+0x33F (foreign worker border hires, tourist arrivals/departures/deaths, graduations)
    float   tourism[44];         // +0x340..+0x3EF ($Tourism_SpendUSD/RUB[16], Shops/Cinemas/Pubs/Sport/Hotels/Rides)
    float   loans[4];            // +0x3F0..+0x3FF ($Loan_Ballance_USD, $Loan_Interest_USD, $Loan_Ballance_RUB, $Loan_Interest_RUB)
};

struct StateEconPeriodStats {
    int32_t day = 0;
    int32_t year = 0;
    std::vector<SovietStatResEntry> resVecs[STAT_RES_VEC_COUNT];
    StateEconScalars scalars = {};
};

struct RawStatResVecHeader {
    SovietStatResEntry* begin;
    SovietStatResEntry* end;
    SovietStatResEntry* cap;
};
static_assert(sizeof(RawStatResVecHeader) == 24, "RawStatResVecHeader must be 24 bytes");

struct StatRecordUiBackup {
    RawStatResVecHeader resVecHeaders[STAT_RES_VEC_COUNT];
    StateEconScalars    scalars;
};

static StateEconPeriodStats              g_stateCurEcon[2];
static std::vector<StateEconPeriodStats> g_stateHistEcon[2];
static bool                              g_stateEconInitialized = false;
static bool                              g_uiEconSwapped = false;
static int                               g_uiEconSwappedState = 0;
static StatRecordUiBackup                g_uiSavedCurStat = {};
static std::vector<StatRecordUiBackup>   g_uiSavedHistStats;
static BYTE*                             g_uiSavedHistBegin = nullptr;
static int                               g_uiSavedHistCount = 0;

struct SavedCitizenLifespan {
    BYTE* cit;
    float lifespan;
};

static bool                              g_uiCitVecSwapped = false;
static uintptr_t                         g_uiSavedCitVecHdr[3] = {};
static std::vector<BYTE*>                g_uiLocalCitizens;
static std::vector<SavedCitizenLifespan> g_uiSavedCitizenLifespans;
static bool                              g_uiHudPopSwapped = false;
static BYTE                              g_uiSavedHudPopBlock[WORLD_HUD_POP_BLOCK_SIZE] = {};
static BYTE                              g_stateHudPopBlock[2][WORLD_HUD_POP_BLOCK_SIZE] = {};
static bool                              g_stateHudPopInitialized[2] = { false, false };
static int                               g_stateLastLocalPop[2] = { -1, -1 };
static StateEconScalars                  g_uiSavedPopWinScalars = {};
static int                               g_lastUiSwappedState = 0;
static size_t                            g_lastUiSwappedHistLen[2] = { 0, 0 };
static std::vector<BYTE*>                g_uiLocalBuildings;
static std::vector<BYTE*>                g_uiLocalVehicles;
static int                               g_testStoredResBldCount = 0;
static int                               g_testStoredResVehCount = 0;

static volatile LONG g_saveInProgress = 0;
static volatile LONGLONG g_completedSaveCount = 0;

static void EnsureMoneySwapLock()
{
    TsmEnsureCriticalSection(&g_moneySwapLockOnce, &g_moneySwapLock);
}

bool TwoStates_IsSaveInProgress()
{
    return InterlockedCompareExchange(&g_saveInProgress, 0, 0) > 0;
}

unsigned long long TwoStates_GetCompletedSaveCount()
{
    return (unsigned long long)InterlockedCompareExchange64(&g_completedSaveCount, 0, 0);
}

// ---------------------------------------------------------------------------
// Bank / Loan POD & SEH Helpers (WORLD + 0x10BD0 / 0x10BE8 / 0x11718)
// ---------------------------------------------------------------------------

static const BYTE kVecLoanPushBackExpect[8] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83
};
typedef void (__fastcall* VecLoanPushBackFn)(BYTE* vecPtr, const SovietLoan* loan);

static bool SafeReadLoanVectorHeader(const BYTE* world, SovietLoan** outBegin, int* outCount, int* outCapCount)
{
    if (!world || !outBegin || !outCount || !outCapCount) return false;
    *outBegin = nullptr;
    *outCount = 0;
    *outCapCount = 0;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* vec = world + WORLD_LOANS_OFFSET;
        SovietLoan* b = *(SovietLoan* const*)(vec + 0);
        SovietLoan* e = *(SovietLoan* const*)(vec + 8);
        SovietLoan* c = *(SovietLoan* const*)(vec + 16);
        if (!b || e < b || c < e) {
            *outBegin = b;
            DiagLog_EndSafeSeh();
            return b == nullptr && e == nullptr;
        }
        ptrdiff_t byteLen = (const BYTE*)e - (const BYTE*)b;
        ptrdiff_t capBytes = (const BYTE*)c - (const BYTE*)b;
        if (byteLen < 0 || byteLen % (ptrdiff_t)sizeof(SovietLoan) != 0 ||
            byteLen > 4096 * (ptrdiff_t)sizeof(SovietLoan)) {
            DiagLog_EndSafeSeh();
            return false;
        }
        *outBegin = b;
        *outCount = (int)(byteLen / (ptrdiff_t)sizeof(SovietLoan));
        *outCapCount = (int)(capBytes / (ptrdiff_t)sizeof(SovietLoan));
        DiagLog_EndSafeSeh();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DiagLog_EndSafeSeh();
        return false;
    }
}

static bool SafeWriteLoanVectorInPlace(BYTE* world, const SovietLoan* src, int count)
{
    SovietLoan* b = nullptr;
    int curCount = 0, capCount = 0;
    if (!SafeReadLoanVectorHeader(world, &b, &curCount, &capCount)) return false;
    if (count < 0 || count > capCount) return false;
    if (count > 0 && (!b || !src)) return false;
    DiagLog_BeginSafeSeh();
    __try {
        if (count > 0 && b != src) {
            memcpy(b, src, (size_t)count * sizeof(SovietLoan));
        }
        if (b) {
            *(SovietLoan**)(world + WORLD_LOANS_OFFSET + 8) = b + count;
        }
        DiagLog_EndSafeSeh();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DiagLog_EndSafeSeh();
        return false;
    }
}

static int SafeReadSelectedLoanIdx(const BYTE* world)
{
    int32_t idx = -1;
    if (world) SafeReadPod((const int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET), &idx);
    return (int)idx;
}

static void SafeWriteSelectedLoanIdx(BYTE* world, int idx)
{
    int32_t val = (int32_t)idx;
    if (world) SafeWritePod((int32_t*)(world + WORLD_LOAN_SELECTED_OFFSET), val);
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

static bool ReadCurrentLoansToVector(const BYTE* world, std::vector<SovietLoan>& outLoans)
{
    outLoans.clear();
    SovietLoan* b = nullptr;
    int count = 0, capCount = 0;
    if (!SafeReadLoanVectorHeader(world, &b, &count, &capCount)) return false;
    if (count <= 0 || !b) return true;
    outLoans.resize((size_t)count);
    if (!SafeMemcpyPod(outLoans.data(), b, (size_t)count * sizeof(SovietLoan))) {
        outLoans.clear();
        return false;
    }
    return true;
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

static void RestoreUnchangedUiLoans(BYTE* world, int localSelectedAfter)
{
    if (!g_uiLoansFilterByState || !world) return;
    SafeWriteLoanVectorInPlace(world, g_uiFullLoansBackup.data(), (int)g_uiFullLoansBackup.size());
    int restoredSel = -1;
    if (localSelectedAfter >= 0 && (size_t)localSelectedAfter < g_uiFilteredToGlobalIdx.size()) {
        restoredSel = g_uiFilteredToGlobalIdx[(size_t)localSelectedAfter];
    }
    SafeWriteSelectedLoanIdx(world, restoredSel);
}

static void RevertUiLoansToPreUiState(BYTE* world, int localSelectedAfter, bool keepLocalSelection)
{
    if (!world) return;
    SafeWriteLoanVectorInPlace(world, g_uiFullLoansBackup.data(), (int)g_uiFullLoansBackup.size());
    SafeWriteLoanStats(world, g_uiSavedLoanStats);
    int restoredSel = g_uiSavedGlobalSelectedIdx;
    if (keepLocalSelection) {
        if (g_uiLoansFilterByState) {
            restoredSel = (localSelectedAfter >= 0 && (size_t)localSelectedAfter < g_uiFilteredToGlobalIdx.size())
                ? g_uiFilteredToGlobalIdx[(size_t)localSelectedAfter]
                : -1;
        } else {
            restoredSel = localSelectedAfter;
        }
    }
    SafeWriteSelectedLoanIdx(world, restoredSel);
}

// ---------------------------------------------------------------------------
// Per-State Economic Statistics POD & SEH Helpers (WORLD + 0x11328 / 0x11728)
// ---------------------------------------------------------------------------

static int  CountStateAdultHousedCitizens(const BYTE* world, int state);
static void ComputeStateCitizenDemographics(const BYTE* world, int state, StateEconScalars* ioScalars);
static int  ResolveCitizenOwnerFast(const BYTE* cit);

typedef int (*ResolveEntityOwnerFn)(const BYTE* entity);

static void ResetStateHudPopCache()
{
    memset(g_stateHudPopBlock, 0, sizeof(g_stateHudPopBlock));
    g_stateHudPopInitialized[0] = g_stateHudPopInitialized[1] = false;
    g_stateLastLocalPop[0] = g_stateLastLocalPop[1] = -1;
    g_lastUiSwappedState = 0;
    g_lastUiSwappedHistLen[0] = g_lastUiSwappedHistLen[1] = 0;
}

static bool SwapWorldPtrVecForState(BYTE* world, size_t vecOff, int targetState,
                                    ResolveEntityOwnerFn resolveFn, ptrdiff_t maxCount,
                                    uintptr_t outSavedHdr[3], std::vector<BYTE*>& outFiltered)
{
    outFiltered.clear();
    if (!world || !outSavedHdr || !resolveFn) return false;
    memset(outSavedHdr, 0, 3 * sizeof(uintptr_t));
    if (!SafeMemcpyPod(outSavedHdr, world + vecOff, 3 * sizeof(uintptr_t))) return false;
    BYTE* const* b = (BYTE* const*)outSavedHdr[0];
    BYTE* const* e = (BYTE* const*)outSavedHdr[1];
    BYTE* const* c = (BYTE* const*)outSavedHdr[2];
    if (b && (e < b || c < e || (e - b) > maxCount)) return false;

    if (b && e > b) {
        outFiltered.reserve((size_t)(e - b));
        DiagLog_BeginSafeSeh();
        __try {
            for (BYTE* const* it = b; it < e; ++it) {
                BYTE* elem = *it;
                if (elem && resolveFn(elem) == targetState) {
                    outFiltered.push_back(elem);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outFiltered.clear();
        }
        DiagLog_EndSafeSeh();
    }
    BYTE** fBegin = outFiltered.empty() ? nullptr : outFiltered.data();
    uintptr_t newHdr[3] = {
        (uintptr_t)fBegin,
        (uintptr_t)(fBegin ? (fBegin + outFiltered.size()) : nullptr),
        (uintptr_t)(fBegin ? (fBegin + outFiltered.size()) : nullptr)
    };
    return SafeMemcpyPod(world + vecOff, newHdr, sizeof(newHdr));
}

static int CountLocalHudPopulation(const BYTE* const* begin, const BYTE* const* end)
{
    if (!begin || end <= begin) return 0;
    int n = 0;
    DiagLog_BeginSafeSeh();
    __try {
        for (const BYTE* const* it = begin; it < end; ++it) {
            const BYTE* c = *it;
            if (!c || *(const int32_t*)(c + CIT_INFANT_FLAG_OFFSET) || *(const int32_t*)(c + CIT_UNBORN_FLAG_OFFSET)) continue;
            const BYTE* job = *(const BYTE* const*)(c + CIT_PARENT_OFFSET);
            const BYTE* holder = job ? job : c;
            const BYTE* building = *(const BYTE* const*)(holder + CIT_HOME_BUILDING_OFFSET);
            if (*(const BYTE*)(holder + 0x2D) == 1) {
                const BYTE* type = building ? *(const BYTE* const*)(building + 0x318) : nullptr;
                if (type && *(const BYTE*)(type + 0x8E8) == 1) continue;
            }
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    return n;
}

static void SafeReadStatScalars(const BYTE* rec, StateEconScalars* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!rec) return;
    SafeMemcpyPod(out->vehicles,        rec + STAT_VEHICLES_OFF,             sizeof(out->vehicles));
    SafeMemcpyPod(out->immigrantSpend,  rec + STAT_IMMIGRANT_SPEND_OFF,      sizeof(out->immigrantSpend));
    SafeMemcpyPod(out->citizenCounts,   rec + STAT_CITIZEN_COUNTS_OFF,       sizeof(out->citizenCounts));
    SafeMemcpyPod(out->satisfaction,    rec + STAT_CITIZEN_SATISFACTION_OFF, sizeof(out->satisfaction));
    SafeMemcpyPod(out->citizenAverages, rec + STAT_CITIZEN_AVERAGES_OFF,     sizeof(out->citizenAverages));
    SafeMemcpyPod(out->demographics,    rec + STAT_CITIZEN_DEMOGRAPHICS_OFF, sizeof(out->demographics));
    SafeMemcpyPod(out->citizenBrackets, rec + STAT_CITIZEN_BRACKETS_OFF,     sizeof(out->citizenBrackets));
    SafeMemcpyPod(out->extraCounts,     rec + STAT_EXTRA_COUNTS_OFF,         sizeof(out->extraCounts));
    SafeMemcpyPod(out->tourism,         rec + STAT_TOURISM_OFF,              sizeof(out->tourism));
    SafeMemcpyPod(out->loans,           rec + STAT_LOANS_OFF,                sizeof(out->loans));
}

static void SafeWriteStatScalars(BYTE* rec, const StateEconScalars& src)
{
    if (!rec) return;
    SafeMemcpyPod(rec + STAT_VEHICLES_OFF,             src.vehicles,        sizeof(src.vehicles));
    SafeMemcpyPod(rec + STAT_IMMIGRANT_SPEND_OFF,      src.immigrantSpend,  sizeof(src.immigrantSpend));
    SafeMemcpyPod(rec + STAT_CITIZEN_COUNTS_OFF,       src.citizenCounts,   sizeof(src.citizenCounts));
    SafeMemcpyPod(rec + STAT_CITIZEN_SATISFACTION_OFF, src.satisfaction,    sizeof(src.satisfaction));
    SafeMemcpyPod(rec + STAT_CITIZEN_AVERAGES_OFF,     src.citizenAverages, sizeof(src.citizenAverages));
    SafeMemcpyPod(rec + STAT_CITIZEN_DEMOGRAPHICS_OFF, src.demographics,    sizeof(src.demographics));
    SafeMemcpyPod(rec + STAT_CITIZEN_BRACKETS_OFF,     src.citizenBrackets, sizeof(src.citizenBrackets));
    SafeMemcpyPod(rec + STAT_EXTRA_COUNTS_OFF,         src.extraCounts,     sizeof(src.extraCounts));
    SafeMemcpyPod(rec + STAT_TOURISM_OFF,              src.tourism,         sizeof(src.tourism));
    SafeMemcpyPod(rec + STAT_LOANS_OFF,                src.loans,           sizeof(src.loans));
}

static void AddFloatSliceDelta(float* dst, const float* before, const float* after, int count, float scale)
{
    for (int i = 0; i < count; ++i) {
        float d = (after[i] - before[i]) * scale;
        if (isfinite(d)) dst[i] += d;
    }
}

static void AddIntSliceDelta(int32_t* dst, const int32_t* before, const int32_t* after, int count, float scale)
{
    for (int i = 0; i < count; ++i) {
        int32_t d = after[i] - before[i];
        if (d != 0) dst[i] += (int32_t)lroundf((float)d * scale);
    }
}

static void AddStatScalarsDelta(StateEconScalars* dst, const StateEconScalars& before,
                                const StateEconScalars& after, float scale = 1.0f)
{
    if (!dst) return;
    AddFloatSliceDelta(dst->vehicles,       before.vehicles,       after.vehicles,       4,  scale);
    AddFloatSliceDelta(dst->immigrantSpend, before.immigrantSpend, after.immigrantSpend, 2,  scale);
    AddIntSliceDelta(dst->citizenCounts,    before.citizenCounts,  after.citizenCounts,  5,  scale);
    AddIntSliceDelta(dst->extraCounts,      before.extraCounts,    after.extraCounts,    21, scale);
    AddFloatSliceDelta(dst->tourism,        before.tourism,        after.tourism,        44, scale);
    AddFloatSliceDelta(dst->loans,          before.loans,          after.loans,          4,  scale);
}

static void ClearStateEconPeriod(StateEconPeriodStats* st, int32_t day = 0, int32_t year = 0)
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

static void AddEntryDeltaToStateResVec(std::vector<SovietStatResEntry>& vec, uintptr_t resPtr,
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

static bool SafeReadStatHistHeader(const BYTE* world, BYTE** outBegin, int* outCount)
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

static void SaveRecordToUiBackup(const BYTE* rec, StatRecordUiBackup* outBackup)
{
    if (!rec || !outBackup) return;
    memset(outBackup, 0, sizeof(*outBackup));
    SafeMemcpyPod(outBackup->resVecHeaders, rec + STAT_RES_VEC_BASE_OFF, sizeof(outBackup->resVecHeaders));
    SafeReadStatScalars(rec, &outBackup->scalars);
}

static void WriteStatePeriodToEngineRecordForUi(BYTE* rec, StateEconPeriodStats& st)
{
    if (!rec) return;
    RawStatResVecHeader hdrs[STAT_RES_VEC_COUNT] = {};
    for (int v = 0; v < STAT_RES_VEC_COUNT; ++v) {
        std::vector<SovietStatResEntry>& sv = st.resVecs[v];
        SovietStatResEntry* p = sv.empty() ? nullptr : sv.data();
        hdrs[v].begin = p;
        hdrs[v].end   = p ? (p + sv.size()) : nullptr;
        hdrs[v].cap   = hdrs[v].end;
    }
    SafeMemcpyPod(rec + STAT_RES_VEC_BASE_OFF, hdrs, sizeof(hdrs));
    SafeWriteStatScalars(rec, st.scalars);
}

static void RestoreRecordFromUiBackup(BYTE* rec, const StatRecordUiBackup& backup)
{
    if (!rec) return;
    SafeMemcpyPod(rec + STAT_RES_VEC_BASE_OFF, backup.resVecHeaders, sizeof(backup.resVecHeaders));
    SafeWriteStatScalars(rec, backup.scalars);
}

static void PopulateHudPopBlockFromScalars(BYTE* outBlock, const StateEconScalars& sc, int localPop)
{
    if (!outBlock) return;
    memset(outBlock, 0, WORLD_HUD_POP_BLOCK_SIZE);
    float fPop = (float)localPop;
    int32_t adults = sc.demographics[3];
    *(float*)(outBlock + 0x00)   = fPop;
    *(float*)(outBlock + 0x04)   = (adults > 0) ? ((float)sc.citizenBrackets[0] / (float)adults) : 0.0f;
    *(float*)(outBlock + 0x08)   = sc.satisfaction[2];
    *(float*)(outBlock + 0x0C)   = sc.satisfaction[0];
    *(float*)(outBlock + 0x10)   = sc.satisfaction[1];
    *(float*)(outBlock + 0x14)   = 0.0f;
    *(int32_t*)(outBlock + 0x24) = adults;
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

static void SaveUiCitizenLifespans(const BYTE* const* begin, const BYTE* const* end, std::vector<SavedCitizenLifespan>& outSaved)
{
    outSaved.clear();
    if (!begin || end <= begin) return;
    size_t count = (size_t)(end - begin);
    if (count > 2000000) return;
    outSaved.reserve(count);
    DiagLog_BeginSafeSeh();
    __try {
        for (const BYTE* const* it = begin; it < end; ++it) {
            BYTE* c = const_cast<BYTE*>(*it);
            if (c) {
                SavedCitizenLifespan item = { c, *(const float*)(c + 0x18) };
                outSaved.push_back(item);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outSaved.clear();
    }
    DiagLog_EndSafeSeh();
}

static void RestoreUiCitizenLifespans(std::vector<SavedCitizenLifespan>& saved)
{
    if (saved.empty()) return;
    const SavedCitizenLifespan* b = saved.data();
    const SavedCitizenLifespan* e = b + saved.size();
    DiagLog_BeginSafeSeh();
    __try {
        for (const SavedCitizenLifespan* it = b; it < e; ++it) {
            if (it->cit) *(float*)(it->cit + 0x18) = it->lifespan;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    saved.clear();
}

static void SwapUiCitizenVecAndHudPop(BYTE* world, int localState)
{
    if (!world || (localState != 1 && localState != 2)) return;
    int sIdx = localState - 1;
    g_uiCitVecSwapped = SwapWorldPtrVecForState(world, WORLD_CITIZENS_OFFSET, localState,
                                                &ResolveCitizenOwnerFast, 2000000,
                                                g_uiSavedCitVecHdr, g_uiLocalCitizens);
    const BYTE* const* lb = (g_uiCitVecSwapped && !g_uiLocalCitizens.empty()) ? g_uiLocalCitizens.data() : nullptr;
    if (g_uiCitVecSwapped) {
        SaveUiCitizenLifespans(lb, lb ? (lb + g_uiLocalCitizens.size()) : nullptr, g_uiSavedCitizenLifespans);
    } else {
        g_uiSavedCitizenLifespans.clear();
    }

    if (SafeMemcpyPod(g_uiSavedHudPopBlock, world + WORLD_HUD_POP_BLOCK_OFFSET, WORLD_HUD_POP_BLOCK_SIZE)) {
        g_uiHudPopSwapped = true;
        int localPop = CountLocalHudPopulation(lb, lb ? (lb + g_uiLocalCitizens.size()) : nullptr);
        if (!g_stateHudPopInitialized[sIdx]) {
            PopulateHudPopBlockFromScalars(g_stateHudPopBlock[sIdx], g_stateCurEcon[sIdx].scalars, localPop);
        }
        SafeMemcpyPod(world + WORLD_HUD_POP_BLOCK_OFFSET, g_stateHudPopBlock[sIdx], WORLD_HUD_POP_BLOCK_SIZE);
        int32_t pop32 = (int32_t)localPop;
        SafeWritePod((int32_t*)(world + WORLD_HUD_POP_COUNT_OFFSET), pop32);
        if (!g_stateHudPopInitialized[sIdx] || g_stateLastLocalPop[sIdx] != localPop || g_lastUiSwappedState != localState) {
            float zeroTimer = 0.0f;
            SafeWritePod((float*)(world + WORLD_HUD_POP_TIMER_OFFSET), zeroTimer);
            SafeWritePod((float*)(world + WORLD_POP_STAT_TIMER_OFF), zeroTimer);
            g_stateLastLocalPop[sIdx] = localPop;
        }

        BYTE* popWinStat = world + WORLD_POP_STAT_CURRENT_OFF;
        SafeReadStatScalars(popWinStat, &g_uiSavedPopWinScalars);
        SafeWriteStatScalars(popWinStat, g_stateCurEcon[sIdx].scalars);
    }

    if (g_lastUiSwappedState != localState || g_lastUiSwappedHistLen[sIdx] != g_stateHistEcon[sIdx].size()) {
        BYTE dirty = 1;
        SafeWritePod(world + WORLD_ECON_GRAPH_DIRTY_OFF, dirty);
        SafeWritePod(world + WORLD_POP_GRAPH_DIRTY_OFF,  dirty);
        g_lastUiSwappedState = localState;
        g_lastUiSwappedHistLen[sIdx] = g_stateHistEcon[sIdx].size();
    }
}

static void RestoreUiCitizenVecAndHudPop(BYTE* world)
{
    RestoreUiCitizenLifespans(g_uiSavedCitizenLifespans);
    if (g_uiHudPopSwapped) {
        g_uiHudPopSwapped = false;
        if (world) {
            if (g_uiEconSwappedState == 1 || g_uiEconSwappedState == 2) {
                int sIdx = g_uiEconSwappedState - 1;
                SafeMemcpyPod(g_stateHudPopBlock[sIdx],
                              world + WORLD_HUD_POP_BLOCK_OFFSET, WORLD_HUD_POP_BLOCK_SIZE);
                float hudTimerAfter = 0.0f;
                if (SafeReadPod((const float*)(world + WORLD_HUD_POP_TIMER_OFFSET), &hudTimerAfter) &&
                    hudTimerAfter > 0.0f) {
                    g_stateHudPopInitialized[sIdx] = true;
                }
            }
            SafeMemcpyPod(world + WORLD_HUD_POP_BLOCK_OFFSET, g_uiSavedHudPopBlock, WORLD_HUD_POP_BLOCK_SIZE);
            SafeWriteStatScalars(world + WORLD_POP_STAT_CURRENT_OFF, g_uiSavedPopWinScalars);
        }
    }
    if (g_uiCitVecSwapped) {
        g_uiCitVecSwapped = false;
        if (world) {
            SafeMemcpyPod(world + WORLD_CITIZENS_OFFSET, g_uiSavedCitVecHdr, sizeof(g_uiSavedCitVecHdr));
        }
    }
}

static void RestoreUiEconSwap(BYTE* world)
{
    if (!g_uiEconSwapped) return;
    g_uiEconSwapped = false;
    RestoreUiCitizenVecAndHudPop(world);
    g_uiEconSwappedState = 0;
    if (!world) return;

    RestoreRecordFromUiBackup(world + WORLD_STAT_CURRENT_OFFSET, g_uiSavedCurStat);
    if (g_uiSavedHistBegin && g_uiSavedHistCount > 0) {
        BYTE* curHistBegin = nullptr;
        int curHistCount = 0;
        if (SafeReadStatHistHeader(world, &curHistBegin, &curHistCount) &&
            curHistBegin == g_uiSavedHistBegin && curHistCount >= g_uiSavedHistCount) {
            for (int i = 0; i < g_uiSavedHistCount && (size_t)i < g_uiSavedHistStats.size(); ++i) {
                RestoreRecordFromUiBackup(curHistBegin + (size_t)i * STAT_RECORD_SIZE, g_uiSavedHistStats[(size_t)i]);
            }
        }
    }
    g_uiSavedHistBegin = nullptr;
    g_uiSavedHistCount = 0;
}

void TwoStates_RestoreUiMoneySwap()
{
    EnsureMoneySwapLock();
    EnterCriticalSection(&g_moneySwapLock);
    bool hadLoans = g_uiLoansSwapped;
    bool hadMoney = g_uiMoneySwapped;
    bool hadEcon  = g_uiEconSwapped;
    g_uiLoansSwapped = false;
    g_uiMoneySwapped = false;

    bool submitLoanCmd = false;
    bool applyOfflineLoanCmd = false;
    LsLoanCommand loanCmd = {};

    if (hadLoans && g_exeBase) {
        BYTE* world = g_exeBase + WORLD_RVA;
        int localSelectedAfter = SafeReadSelectedLoanIdx(world);
        std::vector<SovietLoan> afterLoans;
        if (ReadCurrentLoansToVector(world, afterLoans) && DetectUiLoanOperation(afterLoans, &loanCmd)) {
            double curRub = g_uiShownRub, curUsd = g_uiShownUsd;
            SafeReadPod(WorldRubles(), &curRub);
            SafeReadPod(WorldDollars(), &curUsd);
            loanCmd.dRub = curRub - g_uiShownRub;
            loanCmd.dUsd = curUsd - g_uiShownUsd;
            float afterStats[4] = {};
            SafeReadLoanStats(world, afterStats);
            for (int i = 0; i < 4; ++i) {
                loanCmd.dStats[i] = afterStats[i] - g_uiSavedLoanStats[i];
            }

            bool blockedInLobby = SaveSync_IsWaitingLobbyActive();
            RevertUiLoansToPreUiState(world, localSelectedAfter, !blockedInLobby);
            if (hadMoney) {
                SafeWritePod(WorldRubles(), g_uiSavedSimRub);
                SafeWritePod(WorldDollars(), g_uiSavedSimUsd);
                hadMoney = false;
            }
            if (!blockedInLobby) {
                if (Commands_Capturing()) {
                    submitLoanCmd = true;
                } else {
                    applyOfflineLoanCmd = true;
                }
            }
        } else {
            RestoreUnchangedUiLoans(world, localSelectedAfter);
        }
    }

    if (hadEcon && g_exeBase) {
        RestoreUiEconSwap(g_exeBase + WORLD_RVA);
    }

    if (hadMoney) {
        double curRub = g_uiShownRub, curUsd = g_uiShownUsd;
        if (SafeReadPod(WorldRubles(), &curRub) && SafeReadPod(WorldDollars(), &curUsd)) {
            double dRub = curRub - g_uiShownRub;
            double dUsd = curUsd - g_uiShownUsd;
            SafeWritePod(WorldRubles(), g_uiSavedSimRub + dRub);
            SafeWritePod(WorldDollars(), g_uiSavedSimUsd + dUsd);
        }
    }
    LeaveCriticalSection(&g_moneySwapLock);

    if (submitLoanCmd) {
        Commands_Submit(LS_CMD_LOAN, &loanCmd, sizeof(loanCmd));
    } else if (applyOfflineLoanCmd) {
        int owner = (loanCmd.ownerSubType == 1) ? 2 : 1;
        TwoStates_BeginCommand(owner);
        TwoStates_ApplyLoanCommand(loanCmd);
        TwoStates_EndCommand(owner);
    }
}

// ---------------------------------------------------------------------------
// C3D_TERRAIN boundary IAT hooks + Fair Economy Simulation Inline Hooks
// ---------------------------------------------------------------------------

typedef float (__fastcall* TerrainBoundaryFn)(BYTE* terrain);
typedef float (__fastcall* CollisionTerrainHeightFn)(BYTE* terrain, const float pos3[3]);
static TerrainBoundaryFn o_getBoundaryXMin = nullptr;
static TerrainBoundaryFn o_getBoundaryXMax = nullptr;
static TerrainBoundaryFn o_getBoundaryZMin = nullptr;
static TerrainBoundaryFn o_getBoundaryZMax = nullptr;
static CollisionTerrainHeightFn g_collisionTerrainHeight = nullptr;

#define EXPANDED_BOUNDARY_MARGIN 150.0f

static void ApplyRuntimeTerrainBounds(BYTE* terrain)
{
    if (!terrain) return;
    __try {
        if (g_expandBorders) {
            if (*(float*)(terrain + 0x8DC) > EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8DC) = EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E0) > EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E0) = EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E4) < -EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E4) = -EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E8) < -EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E8) = -EXPANDED_BOUNDARY_MARGIN;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static float __fastcall H_GetTerrainBoundaryXMin(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryXMin ? o_getBoundaryXMin(terrain) : EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryXMax(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryXMax ? o_getBoundaryXMax(terrain) : -EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryZMin(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryZMin ? o_getBoundaryZMin(terrain) : EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryZMax(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryZMax ? o_getBoundaryZMax(terrain) : -EXPANDED_BOUNDARY_MARGIN;
}

// --- Fair Customs, Building, Vehicle & Statistics Attribution Hooks (SOVIET64.exe PE 6A3EB6AD) ---
// 1. 0x173320(world, building): building auto-purchase of resources & utility border trade
// 2. 0x1B7D50(world, building): customs / border trade (category 0x6B)
// 3. 0x1B9A40(world, building): customs / port vehicle loading & unloading export/import
// 4. 0x159070(world, building, float dt, unsigned char flag): construction resource/groundwork payment
// 5. 0x143710(world, building): groundwork / initial construction auto-purchase & crane/train step
// 6. 0x1856F0(world, building): tourist hotel/attraction/restaurant spending & shop consumption
// 7. 0x3E0730(world, vehicle, unsigned char a3, unsigned char a4): vehicle sale / border delivery
// 8. 0x3DF9B9(world, vehicle): vehicle border cargo export (after null-vehicle check at 0x3DF9B0)
// 9. 0x831120(world, worker, vehicle): foreign worker hiring onto vehicle at customs house
// 10. 0x2FDB00(world, row, ledgerVec, ...): resource statistics ledger accumulator
// 11. 0x2FC290(world, rec): period statistics record rollover / clear

#define RVA_BLD_ECON        0x173320
#define RVA_CUSTOMS_BORDER  0x1B7D50
#define RVA_CUSTOMS_VEHICLE 0x1B9A40
#define RVA_CONSTRUCT_STEP  0x159070
#define RVA_VEHICLE_SALE    0x3E0730
#define RVA_MASTER_SAVE     0x42CC70
#define RVA_MASTER_SAVE_AUTOSAVE_JE 0x42CCD9

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
static const BYTE kCitHomeStepExpect[18] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x49, 0x8B, 0x40, 0x20,
    0x48, 0x8B, 0xDA, 0x0F, 0x29, 0x74, 0x24, 0x50
};
static const BYTE kCitizenBirthExpect[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
    0x57, 0x48, 0x83, 0xEC, 0x50
};
static const BYTE kCitizenRemoveExpect[18] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
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
static const BYTE kBldSimLoopHeadExpect[16] = {
    0x48, 0x8B, 0x86, 0x08, 0x1B, 0x01, 0x00,
    0x48, 0x8B, 0x14, 0xC8,
    0x48, 0x89, 0x54, 0x24, 0x40
};
static const BYTE kBldSimLoopTailExpect[17] = {
    0x45, 0x03, 0xF7,
    0x48, 0x8B, 0x86, 0x10, 0x1B, 0x01, 0x00,
    0x48, 0x2B, 0x86, 0x08, 0x1B, 0x01, 0x00
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
static const BYTE kMasterSaveExpect[19] = {
    0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x0A, 0x00, 0x00
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

typedef __int64 (__fastcall* BldEconFn)(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef float (__fastcall* BldProductivityStepFn)(BYTE* world, BYTE* building, float a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* BldConsumeResFn)(BYTE* world, BYTE* building, BYTE* resource, float amount, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* ForeignLaborStepFn)(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4, uintptr_t a5, uintptr_t a6);
typedef __int64 (__fastcall* ConstructStepFn)(BYTE* world, BYTE* building, float dt, unsigned char flag, uintptr_t a5, uintptr_t a6);
typedef __int64 (__fastcall* VehicleSaleFn)(BYTE* world, BYTE* vehicle, unsigned char a3, unsigned char a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* VehicleExportFn)(BYTE* world, BYTE* vehicle, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef unsigned char (__fastcall* VehicleCargoTradeFn)(BYTE* vehicle, uintptr_t a2, uintptr_t a3, unsigned char a4,
                                                        uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8,
                                                        uintptr_t a9, uintptr_t a10);
typedef void (__fastcall* VehicleRefuelFn)(BYTE* vehicle, float a2, unsigned char a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef unsigned char (__fastcall* ForeignWorkerBoardFn)(BYTE* world, BYTE* worker, BYTE* vehicle, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* LedgerAddFn)(BYTE* world, const SovietStatResEntry* row, BYTE* ledgerVec,
                                       float a4, unsigned char a5, float a6, unsigned char a7);
typedef void (__fastcall* StatRecordClearFn)(BYTE* world, BYTE* rec);
typedef void (__fastcall* MasterSaveFn)(BYTE* world);
typedef float (__fastcall* GetMaxAvailableBorrowFn)(BYTE* world, int32_t loanType);
typedef void (__fastcall* StepOneLoanDayFn)(BYTE* world, SovietLoan* loan);
typedef void (__fastcall* CitHomeStepFn)(BYTE* world, BYTE* building, BYTE* citizen, float dt, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* CitizenBirthFn)(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
typedef void (__fastcall* CitizenRemoveFn)(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4, uintptr_t a5, uintptr_t a6);
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
static CitHomeStepFn o_citHomeStep = nullptr;
static CitizenBirthFn o_citizenBirth = nullptr;
static CitizenRemoveFn o_citizenRemove = nullptr;
static CalcStoredResourcesFn o_calcStoredResources = nullptr;
static VehicleSaleFn o_vehicleSale = nullptr;
static VehicleExportFn o_vehicleBorderExport = nullptr;
static VehicleCargoTradeFn o_vehicleCargoTrade = nullptr;
static VehicleRefuelFn o_vehicleRefuel = nullptr;
static ForeignWorkerBoardFn o_foreignWorkerBoard = nullptr;
static LedgerAddFn o_ledgerAdd = nullptr;
static StatRecordClearFn o_statRecordClear = nullptr;
static MasterSaveFn o_masterSave = nullptr;
static GetMaxAvailableBorrowFn o_getMaxAvailableBorrow = nullptr;
static StepOneLoanDayFn o_stepOneLoanDay = nullptr;

static void WriteCoopSaveMetaInternal(const char* fullSaveDir);
static bool BuildFullSaveDir(const char* saveRel, char* outFullDir, size_t outSize);
static void EnsureStateEconInitialized(const BYTE* world);

static volatile bool g_inSimStep = false;
static volatile DWORD g_simStepThread = 0;
static BYTE* volatile g_simLoopCurrentBuilding = nullptr;
static bool g_inCommand = false;
static int g_cmdOwner = 1;
static bool g_beforeLoadPrepared = false;
static bool g_inStatClearTest = false;
static thread_local bool t_inEconHook = false;
static thread_local int  t_econOwner = 1;
static thread_local bool t_suppressCurStatLedgerHook = false;
static double g_lastWorldRub = 0.0;
static double g_lastWorldUsd = 0.0;
static double g_unattributedRub = 0.0;
static double g_unattributedUsd = 0.0;
static StateEconScalars g_lastWorldScalars = {};
static StateEconScalars g_unattributedScalars = {};
static StateEconScalars g_econBeforeScalars = {};
static StateEconScalars g_cmdBeforeScalars = {};

static bool BeginEconAttribution(BYTE* world, int owner)
{
    if (!g_inSimStep || g_inCommand || t_inEconHook || !TwoStates_IsActive()) return false;
    if (IsUnlimitedMoney()) return false;
    if (Determinism_IsActive() && !Determinism_InStepFrame()) return false;
    if (world != g_exeBase + WORLD_RVA || GetCurrentThreadId() != g_simStepThread) return false;
    if (owner != 1 && owner != 2) return false;

    EnsureStateEconInitialized(world);
    __try {
        double curRub = *WorldRubles();
        double curUsd = *WorldDollars();
        g_unattributedRub += (curRub - g_lastWorldRub);
        g_unattributedUsd += (curUsd - g_lastWorldUsd);
        *WorldRubles()  = g_stateRub[owner - 1];
        *WorldDollars() = g_stateUsd[owner - 1];
        StateEconScalars curScalars = {};
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &curScalars);
        AddStatScalarsDelta(&g_unattributedScalars, g_lastWorldScalars, curScalars, 1.0f);
        g_econBeforeScalars = curScalars;
        g_lastWorldScalars  = curScalars;
        t_econOwner  = owner;
        t_inEconHook = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_inEconHook = false;
        return false;
    }
}

static void EndEconAttribution(int owner, bool countInNetTrade = true)
{
    if (!t_inEconHook) return;
    t_inEconHook = false;
    __try {
        int idx = owner - 1;
        double newRub = *WorldRubles();
        double newUsd = *WorldDollars();
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
        if (g_exeBase) {
            StateEconScalars afterScalars = {};
            SafeReadStatScalars(g_exeBase + WORLD_RVA + WORLD_STAT_CURRENT_OFFSET, &afterScalars);
            AddStatScalarsDelta(&g_stateCurEcon[idx].scalars, g_econBeforeScalars, afterScalars, 1.0f);
            g_lastWorldScalars = afterScalars;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static int ResolveBuildingOwnerFast(const BYTE* b)
{
    if (!b) return 1;
    int owner = 1;
    DiagLog_BeginSafeSeh();
    __try {
        float x = *(const float*)(b + OBJ_POS_OFFSET);
        float z = *(const float*)(b + OBJ_POS_OFFSET + 8);
        owner = TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(x, z), x, z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 1;
    }
    DiagLog_EndSafeSeh();
    return owner;
}

static int ResolveVehicleOwnerFast(const BYTE* v)
{
    if (!v) return 1;
    int owner = 1;
    DiagLog_BeginSafeSeh();
    __try {
        int id = *(const int*)(v + VEH_ID_OFFSET);
        owner = TwoStates_GetVehicleOwner(id, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 1;
    }
    DiagLog_EndSafeSeh();
    return owner;
}

static int ResolveCitizenOwnerFast(const BYTE* cit)
{
    if (!cit) return 1;
    int owner = 1;
    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* home = *(const BYTE* const*)(cit + CIT_HOME_BUILDING_OFFSET);
        if (home) {
            owner = ResolveBuildingOwnerFast(home);
        } else {
            const BYTE* parent = *(const BYTE* const*)(cit + CIT_PARENT_OFFSET);
            const BYTE* parentHome = parent ? *(const BYTE* const*)(parent + CIT_HOME_BUILDING_OFFSET) : nullptr;
            if (parentHome) {
                owner = ResolveBuildingOwnerFast(parentHome);
            } else {
                float x = *(const float*)(cit + CIT_POS_OFFSET);
                float z = *(const float*)(cit + CIT_POS_OFFSET + 8);
                if (isfinite(x) && isfinite(z)) owner = (x <= 0.0f) ? 1 : 2;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 1;
    }
    DiagLog_EndSafeSeh();
    return owner;
}

static __int64 CallBuildingEconHook(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                    uintptr_t a5, uintptr_t a6, BldEconFn orig)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveBuildingOwnerFast(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = orig ? orig(world, building, a3, a4, a5, a6) : 0;
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
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_customsVehicle);
}

static void EnsureStateEconInitialized(const BYTE* world);
static void AttributeLedgerVecDiffToStates(const BYTE* world, int vecIdx, int owner,
                                           const std::vector<SovietStatResEntry>& beforeEntries,
                                           const std::vector<SovietStatResEntry>& afterEntries);

struct ConstructResSnapshot {
    bool active = false;
    bool prevSuppress = false;
    std::vector<SovietStatResEntry> before[STAT_RES_VEC_COUNT];
};

static ConstructResSnapshot g_cmdResSnapshot;

static void BeginConstructResSnapshot(const BYTE* world, int owner, ConstructResSnapshot* snap)
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

static void EndConstructResSnapshot(const BYTE* world, int owner, const ConstructResSnapshot& snap)
{
    if (!snap.active) return;
    t_suppressCurStatLedgerHook = snap.prevSuppress;
    if (!world) return;
    const BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;
    std::vector<SovietStatResEntry> after;
    for (int vIdx = 0; vIdx < STAT_RES_VEC_COUNT; ++vIdx) {
        after.clear();
        if (SafeReadResVecEntries(curStat + STAT_RES_VEC_BASE_OFF + (size_t)vIdx * STAT_RES_VEC_STRIDE, after)) {
            AttributeLedgerVecDiffToStates(world, vIdx, owner, snap.before[vIdx], after);
        }
    }
}

struct ConstructEconScope {
    BYTE* world = nullptr;
    int owner = 0;
    bool active = false;
    ConstructResSnapshot snap;

    ConstructEconScope(BYTE* w, BYTE* building) : world(w)
    {
        BYTE* canonicalWorld = (g_exeBase && w == g_exeBase + WORLD_RVA) ? (g_exeBase + WORLD_RVA) : w;
        owner = (g_inSimStep && !t_inEconHook) ? ResolveBuildingOwnerFast(building) : 0;
        active = owner ? BeginEconAttribution(canonicalWorld, owner) : false;
        if (active) BeginConstructResSnapshot(canonicalWorld, owner, &snap);
    }

    ~ConstructEconScope()
    {
        if (active) {
            BYTE* canonicalWorld = g_exeBase ? (g_exeBase + WORLD_RVA) : world;
            EndConstructResSnapshot(canonicalWorld, owner, snap);
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

static __int64 __fastcall H_BldStepProduction(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                              uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldStepProduction);
}

static __int64 __fastcall H_BldConstructWorkers(BYTE* world, BYTE* building, uintptr_t a3, uintptr_t a4,
                                                uintptr_t a5, uintptr_t a6)
{
    return CallBuildingEconHook(world, building, a3, a4, a5, a6, o_bldConstructWorkers);
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
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveBuildingOwnerFast(building) : 0;
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
    int owner = (g_inSimStep && !t_inEconHook && building) ? ResolveBuildingOwnerFast(building) : 0;
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
    if (g_simLoopCurrentBuilding) return ResolveBuildingOwnerFast(g_simLoopCurrentBuilding);
    if (!citizen) return 1;
    static const size_t kCitBldOffsets[6] = { 0x38, 0x58, 0x78, 0x80, 0x88, CIT_HOME_BUILDING_OFFSET };
    for (size_t i = 0; i < 6; ++i) {
        const BYTE* b = nullptr;
        if (!SafeReadPod((const BYTE* const*)(citizen + kCitBldOffsets[i]), &b) || !IsPointerLikeX64((uintptr_t)b)) continue;
        float bx = 0.0f, bz = 0.0f;
        if (SafeReadPod((const float*)(b + OBJ_POS_OFFSET), &bx) &&
            SafeReadPod((const float*)(b + OBJ_POS_OFFSET + 8), &bz) &&
            isfinite(bx) && isfinite(bz) && fabsf(bx) < 50000.0f && fabsf(bz) < 50000.0f) {
            return TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(bx, bz), bx, bz);
        }
    }
    return ResolveCitizenOwnerFast(citizen);
}

static void __fastcall H_ForeignLaborStep(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4,
                                          uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveForeignLaborOwnerFast(citizen) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    if (o_foreignLaborStep) o_foreignLaborStep(world, citizen, a3, a4, a5, a6);
    if (active) EndEconAttribution(owner);
}

struct CitizenEconScope {
    int owner = 0;
    bool active = false;

    CitizenEconScope(BYTE* world, const BYTE* building, const BYTE* citizen)
    {
        if (g_inSimStep && !t_inEconHook) {
            owner = building ? ResolveBuildingOwnerFast(building) : ResolveCitizenOwnerFast(citizen);
            active = owner ? BeginEconAttribution(world, owner) : false;
        }
    }

    ~CitizenEconScope()
    {
        if (active) EndEconAttribution(owner);
    }
};

static void __fastcall H_CitHomeStep(BYTE* world, BYTE* building, BYTE* citizen, float dt,
                                     uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, building, citizen);
    if (o_citHomeStep) o_citHomeStep(world, building, citizen, dt, a5, a6);
}

static void __fastcall H_CitizenBirth(BYTE* world, BYTE* citizen, uintptr_t a3, uintptr_t a4,
                                      uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenBirth) o_citizenBirth(world, citizen, a3, a4, a5, a6);
}

static void __fastcall H_CitizenRemove(BYTE* world, BYTE* citizen, unsigned char a3, unsigned char a4,
                                       uintptr_t a5, uintptr_t a6)
{
    CitizenEconScope scope(world, nullptr, citizen);
    if (o_citizenRemove) o_citizenRemove(world, citizen, a3, a4, a5, a6);
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
        int localState = TwoStates_LocalState();
        bldSwapped = SwapWorldPtrVecForState(world, WORLD_BUILDINGS_OFFSET, localState,
                                             &ResolveBuildingOwnerFast, 200000,
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

static __int64 __fastcall H_VehicleSale(BYTE* world, BYTE* vehicle, unsigned char a3, unsigned char a4,
                                        uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveVehicleOwnerFast(vehicle) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = o_vehicleSale(world, vehicle, a3, a4, a5, a6);
    if (active) EndEconAttribution(owner);
    return r;
}

static void __fastcall H_VehicleBorderExport(BYTE* world, BYTE* vehicle, uintptr_t a3, uintptr_t a4,
                                             uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveVehicleOwnerFast(vehicle) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    o_vehicleBorderExport(world, vehicle, a3, a4, a5, a6);
    if (active) EndEconAttribution(owner);
}

static unsigned char __fastcall H_VehicleCargoTrade(BYTE* vehicle, uintptr_t a2, uintptr_t a3, unsigned char a4,
                                                    uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8,
                                                    uintptr_t a9, uintptr_t a10)
{
    BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
    int owner = (g_inSimStep && !t_inEconHook && world) ? ResolveVehicleOwnerFast(vehicle) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    unsigned char r = o_vehicleCargoTrade ? o_vehicleCargoTrade(vehicle, a2, a3, a4, a5, a6, a7, a8, a9, a10) : 0;
    if (active) EndEconAttribution(owner);
    return r;
}

static void __fastcall H_VehicleRefuel(BYTE* vehicle, float a2, unsigned char a3,
                                       uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
    int owner = (g_inSimStep && !t_inEconHook && world) ? ResolveVehicleOwnerFast(vehicle) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    if (o_vehicleRefuel) o_vehicleRefuel(vehicle, a2, a3, a4, a5, a6);
    if (active) EndEconAttribution(owner);
}

static unsigned char __fastcall H_ForeignWorkerBoard(BYTE* world, BYTE* worker, BYTE* vehicle,
                                                     uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveVehicleOwnerFast(vehicle) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    unsigned char r = o_foreignWorkerBoard(world, worker, vehicle, a4, a5, a6);
    if (active) EndEconAttribution(owner);
    return r;
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

static void ComputeStateCitizenDemographics(const BYTE* world, int state, StateEconScalars* ioScalars)
{
    if (!ioScalars) return;
    memset(ioScalars->satisfaction, 0, sizeof(ioScalars->satisfaction));
    memset(ioScalars->citizenAverages, 0, sizeof(ioScalars->citizenAverages));
    memset(ioScalars->demographics, 0, sizeof(ioScalars->demographics));
    memset(ioScalars->citizenBrackets, 0, sizeof(ioScalars->citizenBrackets));
    if (!world || (state != 1 && state != 2)) return;

    float satSum[10] = {};
    float prodSum = 0.0f;
    float lifespanSum = 0.0f;
    float ageSum = 0.0f;
    int totalCounted = 0;

    DiagLog_BeginSafeSeh();
    __try {
        const BYTE* const* b = nullptr;
        const BYTE* const* e = nullptr;
        GetFullCitizenVectorRange(world, &b, &e);
        if (b && e >= b && (e - b) <= 2000000) {
            for (const BYTE* const* it = b; it < e; ++it) {
                const BYTE* cit = *it;
                if (!cit) continue;
                if (*(const int32_t*)(cit + CIT_INFANT_FLAG_OFFSET) != 0) continue;
                if (*(const int32_t*)(cit + CIT_UNBORN_FLAG_OFFSET) != 0) continue;
                const BYTE* parent = *(const BYTE* const*)(cit + CIT_PARENT_OFFSET);
                const BYTE* holder = parent ? parent : cit;
                const BYTE* home = *(const BYTE* const*)(holder + CIT_HOME_BUILDING_OFFSET);
                if (!home || ResolveBuildingOwnerFast(home) != state) continue;
                if (*(const BYTE*)(holder + 0x2D) == 1) {
                    const BYTE* bldType = *(const BYTE* const*)(home + 0x318);
                    if (bldType && *(const BYTE*)(bldType + 0x8E8) == 1) continue;
                }

                totalCounted++;
                for (int i = 0; i < 10; ++i) {
                    float v = *(const float*)(cit + 0xD8 + (size_t)i * sizeof(float));
                    if (isfinite(v)) satSum[i] += v;
                }
                float lifespan = *(const float*)(cit + 0x18);
                if (!isfinite(lifespan) || lifespan <= 0.0f) lifespan = 70.0f;
                lifespanSum += lifespan;

                float age = *(const float*)(cit + CIT_AGE_OFFSET);
                if (!isfinite(age) || age < 0.0f) age = 0.0f;
                ageSum += age;

                if ((age > 0.0f && age < 6.0f) || (parent && age < 6.0f)) {
                    ioScalars->demographics[0]++;
                } else if ((age > 0.0f && age < 21.0f) || (parent && age < 21.0f)) {
                    ioScalars->demographics[1]++;
                } else if (parent) {
                    ioScalars->demographics[2]++;
                } else {
                    ioScalars->demographics[3]++;
                    AccumulateAdultCitizenBrackets(cit, home, ioScalars->citizenBrackets, &prodSum);
                }
                AccumulateAllCitizenBrackets(cit, ioScalars->citizenBrackets);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(ioScalars->demographics, 0, sizeof(ioScalars->demographics));
        memset(ioScalars->citizenBrackets, 0, sizeof(ioScalars->citizenBrackets));
        totalCounted = 0;
    }
    DiagLog_EndSafeSeh();

    if (totalCounted > 0) {
        float invTotal = 1.0f / (float)totalCounted;
        for (int i = 0; i < 10; ++i) {
            ioScalars->satisfaction[i] = satSum[i] * invTotal;
        }
        int32_t adults = ioScalars->demographics[3];
        ioScalars->citizenAverages[0] = (adults > 0) ? (prodSum / (float)adults) : 0.0f;
        ioScalars->citizenAverages[1] = lifespanSum * invTotal;
        ioScalars->citizenAverages[2] = ageSum * invTotal;
    }
}

static int CountStateAdultHousedCitizens(const BYTE* world, int state)
{
    StateEconScalars tmp = {};
    ComputeStateCitizenDemographics(world, state, &tmp);
    return tmp.demographics[0] + tmp.demographics[1] + tmp.demographics[2] + tmp.demographics[3];
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
    int activeState = t_inEconHook ? t_econOwner : (g_inCommand ? g_cmdOwner : TwoStates_LocalState());
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

static int g_stepBldCount[2] = { 0, 0 };

static void CountOwnedStructuresInVec(const BYTE* world, size_t vecOff, int* ioC1, int* ioC2)
{
    if (!world || !ioC1 || !ioC2) return;
    const BYTE* const* b = *(const BYTE* const* const*)(world + vecOff);
    const BYTE* const* e = *(const BYTE* const* const*)(world + vecOff + 8);
    if (b && e > b && (e - b) <= 200000) {
        for (const BYTE* const* it = b; it < e; ++it) {
            const BYTE* bld = *it;
            if (!bld) continue;
            if (ResolveBuildingOwnerFast(bld) == 2) (*ioC2)++;
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
        ComputeStateCitizenDemographics(world, 1, &outS0->scalars);
        ComputeStateCitizenDemographics(world, 2, &outS1->scalars);
    }
}

static void SyncStateEconHistoryLength(const BYTE* world, float w0)
{
    BYTE* histBegin = nullptr;
    int histCount = 0;
    if (!SafeReadStatHistHeader(world, &histBegin, &histCount) || !histBegin || histCount <= 0) return;

    size_t curLen = std::min(g_stateHistEcon[0].size(), g_stateHistEcon[1].size());
    if (curLen > (size_t)histCount) {
        g_stateHistEcon[0].resize((size_t)histCount);
        g_stateHistEcon[1].resize((size_t)histCount);
        return;
    }
    for (size_t i = curLen; i < (size_t)histCount; ++i) {
        StateEconPeriodStats s0, s1;
        SplitEngineStatRecordToStates(world, histBegin + i * STAT_RECORD_SIZE, w0, &s0, &s1);
        g_stateHistEcon[0].push_back(s0);
        g_stateHistEcon[1].push_back(s1);
    }
}

static void EnsureStateEconInitialized(const BYTE* world)
{
    if (!world) return;
    float w0 = ComputeWorldState1Weight(world);
    if (!g_stateEconInitialized) {
        SplitEngineStatRecordToStates(world, world + WORLD_STAT_CURRENT_OFFSET, w0,
                                      &g_stateCurEcon[0], &g_stateCurEcon[1]);
        g_stateHistEcon[0].clear();
        g_stateHistEcon[1].clear();
        SyncStateEconHistoryLength(world, w0);
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
        g_stateEconInitialized = true;
    } else {
        SyncStateEconHistoryLength(world, w0);
    }
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
        EnsureStateEconInitialized(world);
        track = true;
    }

    std::vector<SovietStatResEntry> beforeEntries;
    if (track) {
        SafeReadResVecEntries(ledgerVec, beforeEntries);
    }

    if (o_ledgerAdd) {
        o_ledgerAdd(world, row, ledgerVec, a4, a5, a6, a7);
    } else {
        LedgerAddFallback(world, row, ledgerVec, a6);
    }

    if (track) {
        std::vector<SovietStatResEntry> afterEntries;
        if (SafeReadResVecEntries(ledgerVec, afterEntries)) {
            int owner = t_inEconHook ? t_econOwner : (g_inCommand ? g_cmdOwner : 0);
            if (owner != 1 && owner != 2 && g_inSimStep && g_simLoopCurrentBuilding) {
                owner = ResolveBuildingOwnerFast(g_simLoopCurrentBuilding);
            }
            AttributeLedgerVecDiffToStates(world, vecIdx, owner, beforeEntries, afterEntries);
        }
    }

    if (wasUiSwapped && !g_inSimStep && !g_inCommand) {
        TwoStates_UiBegin();
    }
}

static void StatRecordClearFallback(BYTE* world, BYTE* rec)
{
    (void)world;
    if (!rec) return;
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

static void FlushUnattributedScalarsToStates(const BYTE* world)
{
    if (!world) return;
    StateEconScalars curScalars = {};
    SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &curScalars);
    AddStatScalarsDelta(&g_unattributedScalars, g_lastWorldScalars, curScalars, 1.0f);
    g_lastWorldScalars = curScalars;

    StateEconScalars zeroScalars = {};
    StateEconScalars share0 = {};
    float w0 = ComputeWorldState1Weight(world);
    AddStatScalarsDelta(&share0, zeroScalars, g_unattributedScalars, w0);
    AddStatScalarsDelta(&g_stateCurEcon[0].scalars, zeroScalars, share0, 1.0f);
    AddStatScalarsDelta(&g_stateCurEcon[1].scalars, share0, g_unattributedScalars, 1.0f);
    memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
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
        FlushUnattributedScalarsToStates(world);

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
            ComputeStateCitizenDemographics(world, s + 1, &g_stateCurEcon[s].scalars);
        }

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
    }

    if (o_statRecordClear) {
        o_statRecordClear(rcxWorld, rec);
    } else {
        StatRecordClearFallback(world, rec);
    }

    if (isCurStat) {
        SafeReadStatScalars(rec, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
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
    ResetStateHudPopCache();
    memset(g_testLedgerPool, 0, sizeof(g_testLedgerPool));
    memset(&g_lastWorldScalars, 0, sizeof(g_lastWorldScalars));
    memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
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

struct VehSaveSnapshot {
    BYTE* veh;
    unsigned long long f17E8;
    unsigned long long f1A54;
    unsigned long long f1A7C;
};
static std::vector<VehSaveSnapshot> g_vehSaveSnaps;

static void SnapshotOneVehicleBeforeSave(BYTE* v)
{
    if (!v) return;
    VehSaveSnapshot s;
    s.veh   = v;
    s.f17E8 = *(unsigned long long*)(v + 0x17E8);
    s.f1A54 = *(unsigned long long*)(v + 0x1A54);
    s.f1A7C = *(unsigned long long*)(v + 0x1A7C);
    g_vehSaveSnaps.push_back(s);
}

static void SnapshotVehiclesBeforeSave()
{
    g_vehSaveSnaps.clear();
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** vb = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
        BYTE** ve = *(BYTE***)(g_exeBase + VEC_VEHICLES_END_RVA);
        if (vb && ve > vb && (ve - vb) <= 65536) {
            g_vehSaveSnaps.reserve((size_t)(ve - vb));
            for (BYTE** it = vb; it < ve; ++it) {
                BYTE* v = *it;
                if (!v) continue;
                SnapshotOneVehicleBeforeSave(v);
                BYTE** cBegin = *(BYTE***)(v + 0x3A0);
                BYTE** cEnd = *(BYTE***)(v + 0x3A8);
                if (cBegin && cEnd > cBegin && (cEnd - cBegin) <= 1024) {
                    for (BYTE** cit = cBegin; cit < cEnd; ++cit) {
                        if (*cit) SnapshotOneVehicleBeforeSave(*cit);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void RestoreVehiclesAfterSave()
{
    DiagLog_BeginSafeSeh();
    __try {
        for (size_t i = 0; i < g_vehSaveSnaps.size(); ++i) {
            const VehSaveSnapshot& s = g_vehSaveSnaps[i];
            *(unsigned long long*)(s.veh + 0x17E8) = s.f17E8;
            *(unsigned long long*)(s.veh + 0x1A54) = s.f1A54;
            *(unsigned long long*)(s.veh + 0x1A7C) = s.f1A7C;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    g_vehSaveSnaps.clear();
}

static void __fastcall H_MasterSave(BYTE* world)
{
    InterlockedIncrement(&g_saveInProgress);
    TwoStates_RestoreUiMoneySwap();

    bool isCoopSave = g_inSession || NativeMenu_IsHostOnNextLoad() || TwoStates_IsCoopBankMounted();
    bool twoStates  = TwoStates_IsActive() ||
                      (isCoopSave && g_treasuryInitialized &&
                       (g_inSession ? g_sessionMode : g_chosenMode) == COOP_MODE_TWO_STATES);

    char saveRel[128] = {};
    char fullSaveDir[MAX_PATH] = {};
    double prevRub = 0.0, prevUsd = 0.0;
    bool moneySwapped = false;
    BYTE prevFc90 = 0;
    bool fc90Patched = false;
    BYTE* autosaveJeAddr = g_exeBase ? (g_exeBase + RVA_MASTER_SAVE_AUTOSAVE_JE) : nullptr;
    BYTE prevAutosaveJe = 0x74;
    DWORD oldProt = 0;
    bool autosaveJePatched = false;

    BYTE searchPoolBackup[72] = {};
    bool searchPoolSaved = false;

    SnapshotVehiclesBeforeSave();

    DiagLog_BeginSafeSeh();
    __try {
        if (world) {
            memcpy(searchPoolBackup, world + 0x14478, sizeof(searchPoolBackup));
            searchPoolSaved = true;
            strncpy_s(saveRel, (const char*)(world + SAVE_NAME_FIELD_OFFSET), _TRUNCATE);
            if (isCoopSave && _stricmp(saveRel, "saved_last") == 0) {
                // Redirect engine's automatic save-on-exit ("saved_last") into the Co-op save bank
                // so exiting a Co-op session never overwrites Singleplayer's saved_last and always
                // appears in "Continue Co-op" / "Co-op Saves".
                strncpy_s((char*)(world + SAVE_NAME_FIELD_OFFSET), SAVE_NAME_FIELD_MAX_LEN, "save/coop_last_session", _TRUNCATE);
                strncpy_s(saveRel, "save/coop_last_session", _TRUNCATE);
                prevFc90 = *(BYTE*)(world + 0xFC90);
                *(BYTE*)(world + 0xFC90) = 1;
                fc90Patched = true;
            }
            if (saveRel[0] && BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir))) {
                CreateDirectoryA(fullSaveDir, nullptr);
                char scriptPath[MAX_PATH] = {};
                _snprintf_s(scriptPath, sizeof(scriptPath), _TRUNCATE, "%s\\script.ini", fullSaveDir);
                if (!fc90Patched && GetFileAttributesA(scriptPath) == INVALID_FILE_ATTRIBUTES) {
                    prevFc90 = *(BYTE*)(world + 0xFC90);
                    *(BYTE*)(world + 0xFC90) = 1; // force C3D_TERRAIN::Save to write script.ini & terrain files
                    fc90Patched = true;
                }
            }
            if (isCoopSave) {
                *(BYTE*)(world + SAVE_ZIP_AFTER_FLAG_OFFSET) = 0;
                if (autosaveJeAddr && *autosaveJeAddr == 0x74) {
                    if (VirtualProtect(autosaveJeAddr, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                        prevAutosaveJe = *autosaveJeAddr;
                        *autosaveJeAddr = 0xEB; // always skip AUTOSAVE_TO_ZIP so Co-op saves remain folders
                        autosaveJePatched = true;
                    }
                }
            }
            if (twoStates && !IsUnlimitedMoney()) {
                prevRub = *WorldRubles();
                prevUsd = *WorldDollars();
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
                moneySwapped = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();

    if (o_masterSave) {
        Determinism_ToolScopeBegin();
        o_masterSave(world);
        Determinism_ToolScopeEnd();
    }

    RestoreVehiclesAfterSave();

    DiagLog_BeginSafeSeh();
    __try {
        if (searchPoolSaved && world) {
            memcpy(world + 0x14478, searchPoolBackup, sizeof(searchPoolBackup));
        }
        if (autosaveJePatched && autosaveJeAddr) {
            *autosaveJeAddr = prevAutosaveJe;
            VirtualProtect(autosaveJeAddr, 2, oldProt, &oldProt);
        }
        if (fc90Patched && world) {
            *(BYTE*)(world + 0xFC90) = prevFc90;
        }
        if (moneySwapped) {
            *WorldRubles()  = prevRub;
            *WorldDollars() = prevUsd;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();

    if (isCoopSave && fullSaveDir[0]) {
        if (twoStates && world) {
            EnsureStateEconInitialized(world);
        }
        if (g_expandBorders) {
            TwoStates_PatchSaveScriptIni(fullSaveDir, 0.0f);
        }
        WriteCoopSaveMetaInternal(fullSaveDir);
        if (strstr(saveRel, "mp_syncsave_") == nullptr) {
            int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
            if (mode == COOP_MODE_TWO_STATES) {
                char msg[240];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                            Tr("Кооп-сохранение записано (Запад: %.0f РУБ / %.0f $, Восток: %.0f РУБ / %.0f $)",
                               "Co-op save written (West: %.0f RUB / %.0f $, East: %.0f RUB / %.0f $)"),
                            g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
                TwoStates_AddToast(msg, 1);
            } else {
                TwoStates_AddToast(Tr("Кооп-сохранение успешно записано", "Co-op save written"), 1);
            }
        }
    }

    InterlockedIncrement64(&g_completedSaveCount);
    InterlockedDecrement(&g_saveInProgress);
}

static BYTE*  g_econRelayStubPage = nullptr;
static size_t g_econRelayStubUsed = 0;

void* TwoStates_BuildRegPreservingRelayStub(void* targetCppFn, bool keepXmm0Result, bool zeroRcxOnExit)
{
    if (!targetCppFn) return nullptr;
    if (!g_econRelayStubPage) {
        if (g_exeBase) {
            g_econRelayStubPage = AllocNear(g_exeBase, 4096);
        }
        if (!g_econRelayStubPage) {
            g_econRelayStubPage = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        }
        g_econRelayStubUsed = 0;
    }
    if (!g_econRelayStubPage || g_econRelayStubUsed + 160 > 4096) return targetCppFn;

    BYTE* stub = g_econRelayStubPage + g_econRelayStubUsed;
    BYTE* p = stub;
    auto emit = [&](std::initializer_list<BYTE> bytes) {
        for (BYTE b : bytes) *p++ = b;
    };

    // Leaf-function LTCG callers in SOVIET64.exe (e.g. 0x334CC9, 0x32F50D, 0x33381E calling 0x2FC290)
    // assume volatile registers (rdx, r8, r9, r10, r11, xmm0..xmm5) are preserved across the call.
    // On entry rsp & 0xF == 8; sub rsp, 0xB8 aligns rsp to 16 bytes for targetCppFn.
    emit({ 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00 });         // sub rsp, 0xB8
    emit({ 0x48, 0x89, 0x4C, 0x24, 0x20 });                     // mov [rsp+0x20], rcx
    emit({ 0x48, 0x89, 0x54, 0x24, 0x28 });                     // mov [rsp+0x28], rdx
    emit({ 0x4C, 0x89, 0x44, 0x24, 0x30 });                     // mov [rsp+0x30], r8
    emit({ 0x4C, 0x89, 0x4C, 0x24, 0x38 });                     // mov [rsp+0x38], r9
    emit({ 0x4C, 0x89, 0x54, 0x24, 0x40 });                     // mov [rsp+0x40], r10
    emit({ 0x4C, 0x89, 0x5C, 0x24, 0x48 });                     // mov [rsp+0x48], r11
    emit({ 0x0F, 0x11, 0x44, 0x24, 0x50 });                     // movups [rsp+0x50], xmm0
    emit({ 0x0F, 0x11, 0x4C, 0x24, 0x60 });                     // movups [rsp+0x60], xmm1
    emit({ 0x0F, 0x11, 0x54, 0x24, 0x70 });                     // movups [rsp+0x70], xmm2
    emit({ 0x0F, 0x11, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00 });   // movups [rsp+0x80], xmm3
    emit({ 0x0F, 0x11, 0xA4, 0x24, 0x90, 0x00, 0x00, 0x00 });   // movups [rsp+0x90], xmm4
    emit({ 0x0F, 0x11, 0xAC, 0x24, 0xA0, 0x00, 0x00, 0x00 });   // movups [rsp+0xA0], xmm5

    emit({ 0x48, 0xB8 });                                       // mov rax, imm64 (targetCppFn)
    memcpy(p, &targetCppFn, sizeof(void*));
    p += sizeof(void*);
    emit({ 0xFF, 0xD0 });                                       // call rax

    if (!keepXmm0Result) {
        emit({ 0x0F, 0x10, 0x44, 0x24, 0x50 });                 // movups xmm0, [rsp+0x50]
    }
    emit({ 0x0F, 0x10, 0x4C, 0x24, 0x60 });                     // movups xmm1, [rsp+0x60]
    emit({ 0x0F, 0x10, 0x54, 0x24, 0x70 });                     // movups xmm2, [rsp+0x70]
    emit({ 0x0F, 0x10, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00 });   // movups xmm3, [rsp+0x80]
    emit({ 0x0F, 0x10, 0xA4, 0x24, 0x90, 0x00, 0x00, 0x00 });   // movups xmm4, [rsp+0x90]
    emit({ 0x0F, 0x10, 0xAC, 0x24, 0xA0, 0x00, 0x00, 0x00 });   // movups xmm5, [rsp+0xA0]
    emit({ 0x4C, 0x8B, 0x5C, 0x24, 0x48 });                     // mov r11, [rsp+0x48]
    emit({ 0x4C, 0x8B, 0x54, 0x24, 0x40 });                     // mov r10, [rsp+0x40]
    emit({ 0x4C, 0x8B, 0x4C, 0x24, 0x38 });                     // mov r9,  [rsp+0x38]
    emit({ 0x4C, 0x8B, 0x44, 0x24, 0x30 });                     // mov r8,  [rsp+0x30]
    emit({ 0x48, 0x8B, 0x54, 0x24, 0x28 });                     // mov rdx, [rsp+0x28]
    if (zeroRcxOnExit) {
        emit({ 0x33, 0xC9 });                                   // xor ecx, ecx
    } else {
        emit({ 0x48, 0x8B, 0x4C, 0x24, 0x20 });                 // mov rcx, [rsp+0x20]
    }
    emit({ 0x48, 0x81, 0xC4, 0xB8, 0x00, 0x00, 0x00 });         // add rsp, 0xB8
    emit({ 0xC3 });                                             // ret

    size_t stubLen = (size_t)(p - stub);
    FlushInstructionCache(GetCurrentProcess(), stub, stubLen);
    g_econRelayStubUsed += ((stubLen + 15) & ~size_t(15));
    return stub;
}

static bool PatchJmp14WithNops(BYTE* site, size_t siteLen, const BYTE* expected, void* targetStub, const char* tag)
{
    if (!site || siteLen < 14 || !expected || !targetStub) return false;
    if (!SafeBytesMatch(site, expected, siteLen)) {
        Logf("republicmp  TWO_STATES: %s bytes mismatch at %p", tag, (void*)site);
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(site, siteLen, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    site[0] = 0xFF;
    site[1] = 0x25;
    *(uint32_t*)(site + 2) = 0;
    memcpy(site + 6, &targetStub, sizeof(void*));
    for (size_t i = 14; i < siteLen; ++i) site[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), site, siteLen);
    VirtualProtect(site, siteLen, oldProt, &oldProt);
    return true;
}

static int InstallBldSimLoopHooks()
{
    if (!g_exeBase || !g_econRelayStubPage || g_econRelayStubUsed + 160 > 4096) return 0;

    void* curBldPtrAddr = (void*)&g_simLoopCurrentBuilding;

    BYTE* headStub = g_econRelayStubPage + g_econRelayStubUsed;
    BYTE* p = headStub;
    memcpy(p, kBldSimLoopHeadExpect, sizeof(kBldSimLoopHeadExpect));
    p += sizeof(kBldSimLoopHeadExpect);
    *p++ = 0x48; *p++ = 0xB8;
    memcpy(p, &curBldPtrAddr, sizeof(void*));
    p += sizeof(void*);
    *p++ = 0x48; *p++ = 0x89; *p++ = 0x10;
    memcpy(p, kBldSimLoopHeadExpect, 7);
    p += 7;
    *p++ = 0xFF; *p++ = 0x25; *(uint32_t*)p = 0; p += 4;
    void* headRet = (void*)(g_exeBase + RVA_BLD_SIM_LOOP_HEAD + sizeof(kBldSimLoopHeadExpect));
    memcpy(p, &headRet, sizeof(void*));
    p += sizeof(void*);
    size_t headLen = (size_t)(p - headStub);
    FlushInstructionCache(GetCurrentProcess(), headStub, headLen);
    g_econRelayStubUsed += ((headLen + 15) & ~size_t(15));

    BYTE* tailStub = g_econRelayStubPage + g_econRelayStubUsed;
    p = tailStub;
    *p++ = 0x48; *p++ = 0xB8;
    memcpy(p, &curBldPtrAddr, sizeof(void*));
    p += sizeof(void*);
    *p++ = 0x48; *p++ = 0xC7; *p++ = 0x00; *(uint32_t*)p = 0; p += 4;
    memcpy(p, kBldSimLoopTailExpect, sizeof(kBldSimLoopTailExpect));
    p += sizeof(kBldSimLoopTailExpect);
    *p++ = 0xFF; *p++ = 0x25; *(uint32_t*)p = 0; p += 4;
    void* tailRet = (void*)(g_exeBase + RVA_BLD_SIM_LOOP_TAIL + sizeof(kBldSimLoopTailExpect));
    memcpy(p, &tailRet, sizeof(void*));
    p += sizeof(void*);
    size_t tailLen = (size_t)(p - tailStub);
    FlushInstructionCache(GetCurrentProcess(), tailStub, tailLen);
    g_econRelayStubUsed += ((tailLen + 15) & ~size_t(15));

    int patched = 0;
    if (PatchJmp14WithNops(g_exeBase + RVA_BLD_SIM_LOOP_HEAD, sizeof(kBldSimLoopHeadExpect),
                           kBldSimLoopHeadExpect, headStub, "bld_sim_loop_head")) {
        patched++;
    }
    if (PatchJmp14WithNops(g_exeBase + RVA_BLD_SIM_LOOP_TAIL, sizeof(kBldSimLoopTailExpect),
                           kBldSimLoopTailExpect, tailStub, "bld_sim_loop_tail")) {
        patched++;
    }
    return patched;
}

bool TwoStates_Install()
{
    EnsureOwnerLock();
    TwoStates_LoadSettings();
    TwoStates_MountSaveBank(false);

    if (g_engine) {
        g_collisionTerrainHeight = (CollisionTerrainHeightFn)GetProcAddress(
            g_engine, "?CollisionTerrainHeightOverPoint@C3D_TERRAIN@@QEAAMVC3DVECTOR3@@@Z");
    }
    int ok = 0;
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryXMin@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryXMin, (void**)&o_getBoundaryXMin, "terrain BoundaryXMin");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryXMax@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryXMax, (void**)&o_getBoundaryXMax, "terrain BoundaryXMax");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryZMin@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryZMin, (void**)&o_getBoundaryZMin, "terrain BoundaryZMin");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryZMax@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryZMax, (void**)&o_getBoundaryZMax, "terrain BoundaryZMax");

    void* stubStatClear = TwoStates_BuildRegPreservingRelayStub((void*)&H_StatRecordClear, false, true);
    void* stubMaxBorrow = TwoStates_BuildRegPreservingRelayStub((void*)&H_GetMaxAvailableBorrow, true, false);
    void* stubLoanDay   = TwoStates_BuildRegPreservingRelayStub((void*)&H_StepOneLoanDay, false, false);

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
    econOk += InstallInlineHook(g_exeBase + RVA_CIT_HOME_STEP, (void*)&H_CitHomeStep,
                                (void**)&o_citHomeStep, kCitHomeStepExpect,
                                sizeof(kCitHomeStepExpect), "republicmp two_states cit_home_step");
    econOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_BIRTH, (void*)&H_CitizenBirth,
                                (void**)&o_citizenBirth, kCitizenBirthExpect,
                                sizeof(kCitizenBirthExpect), "republicmp two_states citizen_birth");
    econOk += InstallInlineHook(g_exeBase + RVA_CITIZEN_REMOVE, (void*)&H_CitizenRemove,
                                (void**)&o_citizenRemove, kCitizenRemoveExpect,
                                sizeof(kCitizenRemoveExpect), "republicmp two_states citizen_remove");
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
    econOk += InstallBldSimLoopHooks();
    int saveOk = InstallInlineHook(g_exeBase + RVA_MASTER_SAVE, (void*)&H_MasterSave, (void**)&o_masterSave,
                                   kMasterSaveExpect, sizeof(kMasterSaveExpect), "republicmp two_states master_save");

    Logf("republicmp  TWO_STATES: terrain boundary hooks (%d/4), economy/loan/stats attribution hooks (%d/32), master_save=%d, heightFn=%p",
         ok, econOk, saveOk, (void*)g_collisionTerrainHeight);
    return ok == 4;
}

// ---------------------------------------------------------------------------
// Configuration & Lifecycle
// ---------------------------------------------------------------------------

void TwoStates_SetMode(int mode)
{
    g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (!g_inSession) g_sessionMode = g_chosenMode;
}

int TwoStates_GetMode()
{
    return g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
}

bool TwoStates_IsActive()
{
    return g_inSession && g_sessionMode == COOP_MODE_TWO_STATES;
}

void TwoStates_SetBorderHalfWidth(float meters)
{
    if (meters < 50.0f) meters = 50.0f;
    if (meters > 2000.0f) meters = 2000.0f;
    g_borderHalfWidth = meters;
}

float TwoStates_GetBorderHalfWidth()
{
    return g_borderHalfWidth;
}

void TwoStates_SetFullStartingCapital(bool full)
{
    g_fullStartingCapital = full;
}

bool TwoStates_GetFullStartingCapital()
{
    return g_fullStartingCapital;
}

int TwoStates_LocalState()
{
    if (!g_inSession) return 1;
    return g_isHost ? 1 : 2;
}

const char* TwoStates_StateName(int state)
{
    if (state == 1) return Tr("Западная ССР (Хост)", "West SSR (Host)");
    if (state == 2) return Tr("Восточная ССР (Игрок 2)", "East SSR (Player 2)");
    return Tr("Общая пограничная зона", "Shared Border Zone");
}

void TwoStates_PrepareHostSave(bool isResync, double* outRub, double* outUsd)
{
    TwoStates_RestoreUiMoneySwap();
    DiagLog_BeginSafeSeh();
    __try {
        double worldRub = *WorldRubles();
        double worldUsd = *WorldDollars();
        if (!isResync && !g_loadedFromCoopSave && !g_haveLobbyBaseMoney) {
            g_lobbyBaseWorldRub = worldRub;
            g_lobbyBaseWorldUsd = worldUsd;
            g_haveLobbyBaseMoney = true;
        }
        if (TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = 100000000.0;
                g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = 100000000.0;
                g_stateUsd[1] = 100000000.0;
                *WorldRubles()  = 100000000.0;
                *WorldDollars() = 100000000.0;
                g_treasuryInitialized = true;
            } else if (!isResync && !g_treasuryInitialized) {
                double baseRub = g_haveLobbyBaseMoney ? g_lobbyBaseWorldRub : worldRub;
                double baseUsd = g_haveLobbyBaseMoney ? g_lobbyBaseWorldUsd : worldUsd;
                if (g_fullStartingCapital) {
                    g_stateRub[0] = baseRub;
                    g_stateRub[1] = baseRub;
                    g_stateUsd[0] = baseUsd;
                    g_stateUsd[1] = baseUsd;
                    *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                    *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
                } else {
                    g_stateRub[0] = baseRub * 0.5;
                    g_stateRub[1] = baseRub - g_stateRub[0];
                    g_stateUsd[0] = baseUsd * 0.5;
                    g_stateUsd[1] = baseUsd - g_stateUsd[0];
                    *WorldRubles()  = baseRub;
                    *WorldDollars() = baseUsd;
                }
                g_treasuryInitialized = true;
            } else {
                // Already initialized (either from republicmp_coop.ini or while hosting/re-syncing):
                // attribute any pre-session singleplayer delta on Host to State 1.
                double diffRub = worldRub - (g_stateRub[0] + g_stateRub[1]);
                double diffUsd = worldUsd - (g_stateUsd[0] + g_stateUsd[1]);
                if (!Determinism_IsActive() && !g_loadedFromCoopSave && !g_inSession) {
                    g_stateRub[0] += diffRub;
                    g_stateUsd[0] += diffUsd;
                }
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            }
        } else {
            double baseRub = (!isResync && !g_loadedFromCoopSave && g_haveLobbyBaseMoney) ? g_lobbyBaseWorldRub : worldRub;
            double baseUsd = (!isResync && !g_loadedFromCoopSave && g_haveLobbyBaseMoney) ? g_lobbyBaseWorldUsd : worldUsd;
            g_stateRub[0] = baseRub;
            g_stateRub[1] = 0.0;
            g_stateUsd[0] = baseUsd;
            g_stateUsd[1] = 0.0;
            if (!IsUnlimitedMoney() && !isResync && !g_loadedFromCoopSave && g_haveLobbyBaseMoney) {
                *WorldRubles()  = baseRub;
                *WorldDollars() = baseUsd;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    if (outRub) { outRub[0] = g_stateRub[0]; outRub[1] = g_stateRub[1]; }
    if (outUsd) { outUsd[0] = g_stateUsd[0]; outUsd[1] = g_stateUsd[1]; }
}

bool TwoStates_IsLoadedFromCoopSave()
{
    return g_loadedFromCoopSave;
}

void TwoStates_CaptureLobbyBaseMoney()
{
    TwoStates_RestoreUiMoneySwap();
    if (g_loadedFromCoopSave || g_inSession || SaveSync_IsWaitingForReconnect()) return;
    DiagLog_BeginSafeSeh();
    __try {
        g_lobbyBaseWorldRub = *WorldRubles();
        g_lobbyBaseWorldUsd = *WorldDollars();
        g_haveLobbyBaseMoney = true;
        g_treasuryInitialized = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_haveLobbyBaseMoney = false;
    }
    DiagLog_EndSafeSeh();
}

void TwoStates_ReapplyWaitingLobbySettings()
{
    TwoStates_RestoreUiMoneySwap();
    g_sessionMode = g_chosenMode;
    if (g_loadedFromCoopSave || SaveSync_IsWaitingForReconnect()) return;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        if (!g_haveLobbyBaseMoney) {
            g_lobbyBaseWorldRub = *WorldRubles();
            g_lobbyBaseWorldUsd = *WorldDollars();
            g_haveLobbyBaseMoney = true;
        }
        g_treasuryInitialized = false;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    if (ok) {
        TwoStates_PrepareHostSave(false, nullptr, nullptr);
    }
}

void TwoStates_OnSessionJoined(bool isHost, int mode, float borderHalfWidth, bool expandBorders,
                               const double stateRub[2], const double stateUsd[2])
{
    EnsureOwnerLock();
    TwoStates_RestoreUiMoneySwap();

    g_isHost = isHost;
    g_sessionMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (borderHalfWidth >= 50.0f && borderHalfWidth <= 2000.0f)
        g_borderHalfWidth = borderHalfWidth;
    g_expandBorders = expandBorders;
    g_stateRub[0] = stateRub[0];
    g_stateRub[1] = stateRub[1];
    g_stateUsd[0] = stateUsd[0];
    g_stateUsd[1] = stateUsd[1];
    g_treasuryInitialized = (g_sessionMode == COOP_MODE_TWO_STATES);
    g_inSession = true;
    if (!isHost) {
        g_loadedFromCoopSave = false;
    }

    if (g_sessionMode == COOP_MODE_TWO_STATES) {
        DiagLog_BeginSafeSeh();
        __try {
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
                *WorldRubles()  = 100000000.0;
                *WorldDollars() = 100000000.0;
            } else {
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            }
            if (g_exeBase && !g_beforeLoadPrepared && !Determinism_WorldLoading() &&
                (!o_statRecordClear || SaveSync_IsWorldLoaded())) {
                EnsureStateEconInitialized(g_exeBase + WORLD_RVA);
            }
            Commands_RecordMoney();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DiagLog_EndSafeSeh();
        SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Режим активен! Вы: %s | Граница X=0 (общая зона ±%.0f м)",
                      "[TWO STATES] Mode active! You: %s | Border X=0 (shared zone ±%.0f m)"),
                   TwoStates_StateName(TwoStates_LocalState()), g_borderHalfWidth);
        SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Казна Запада: %.0f ₽ / %.0f $ | Казна Востока: %.0f ₽ / %.0f $",
                      "[TWO STATES] West Treasury: %.0f RUB / %.0f USD | East Treasury: %.0f RUB / %.0f USD"),
                   g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
        char toast[220];
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("Режим «Два государства»: Вы — %s (F7: 3D-граница, F6: к другу, Alt+ЛКМ: сигнал)",
                       "Two States Mode: You are %s (F7: 3D border, F6: jump to partner, Alt+LMB: ping)"),
                    TwoStates_StateName(TwoStates_LocalState()));
        TwoStates_AddToast(toast, 1);
    } else {
        SaveStatus(Tr("[ОДНО ГОСУДАРСТВО] Совместное управление единой республикой (общая казна)",
                      "[ONE STATE] Co-op management of a single republic (shared treasury)"));
        TwoStates_AddToast(Tr("Режим «Одно государство»: общая казна и территория (F6: к другу, Alt+ЛКМ: сигнал)",
                              "One State Mode: shared treasury & territory (F6: jump to partner, Alt+LMB: ping)"), 1);
    }
}

void TwoStates_OnWorldLoaded()
{
    TwoStates_RestoreUiMoneySwap();
    if (!TwoStates_IsActive()) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (IsUnlimitedMoney()) {
            g_stateRub[0] = g_stateRub[1] = 100000000.0;
            g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
            *WorldRubles()  = 100000000.0;
            *WorldDollars() = 100000000.0;
        } else {
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        }
        if (g_exeBase) {
            EnsureStateEconInitialized(g_exeBase + WORLD_RVA);
        }
        Commands_RecordMoney();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void SafeWriteWorldMoney(double rub, double usd);

void TwoStates_OnSessionEnded()
{
    TwoStates_RestoreUiMoneySwap();
    ResetStateHudPopCache();
    g_inSession = false;
    g_inSimStep = false;
    if (!SaveSync_IsWorldLoaded()) {
        g_treasuryInitialized = false;
        g_haveLobbyBaseMoney = false;
        g_stateRub[0] = g_stateRub[1] = 0.0;
        g_stateUsd[0] = g_stateUsd[1] = 0.0;
        g_loadedFromCoopSave = false;
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        g_buildingOwners.clear();
        g_vehicleOwners.clear();
        LeaveCriticalSection(&g_ownerLock);
    } else if (!g_loadedFromCoopSave && g_haveLobbyBaseMoney && !SaveSync_HadPartnerInSession()) {
        // Server was stopped in the Waiting Lobby before any partner ever joined:
        // restore the exact pre-lobby Singleplayer world balance so 100%-each never doubles SP money.
        if (!IsUnlimitedMoney()) {
            SafeWriteWorldMoney(g_lobbyBaseWorldRub, g_lobbyBaseWorldUsd);
        }
        g_treasuryInitialized = false;
        g_haveLobbyBaseMoney = false;
        g_stateRub[0] = g_stateRub[1] = 0.0;
        g_stateUsd[0] = g_stateUsd[1] = 0.0;
        g_loadedFromCoopSave = false;
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        g_buildingOwners.clear();
        g_vehicleOwners.clear();
        LeaveCriticalSection(&g_ownerLock);
    } else if (g_treasuryInitialized && TwoStates_GetMode() == COOP_MODE_TWO_STATES && !IsUnlimitedMoney()) {
        SafeWriteWorldMoney(g_stateRub[0] + g_stateRub[1], g_stateUsd[0] + g_stateUsd[1]);
    }

    g_localPingActive = false;

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_toasts.clear();
    LeaveCriticalSection(&g_ownerLock);

    TwoStates_MountSaveBank(false);
    NativeMenu_ClearHostOnNextLoad();
    ModSync_EndSession();
}

// ---------------------------------------------------------------------------
// Treasury & Deterministic Command / Step Hooks
// ---------------------------------------------------------------------------

static double GetStateTreasuryBalance(int state, const double balances[2], double* (*worldCurrencyPtr)())
{
    if (!TwoStates_IsActive() && !(g_treasuryInitialized && TwoStates_GetMode() == COOP_MODE_TWO_STATES)) {
        double v = 0.0;
        SafeReadPod(worldCurrencyPtr(), &v);
        return v;
    }
    if (state == 1) return balances[0];
    if (state == 2) return balances[1];
    return balances[0] + balances[1];
}

double TwoStates_GetRubles(int state)
{
    return GetStateTreasuryBalance(state, g_stateRub, WorldRubles);
}

double TwoStates_GetDollars(int state)
{
    return GetStateTreasuryBalance(state, g_stateUsd, WorldDollars);
}

double TwoStates_LocalRubles()
{
    return TwoStates_GetRubles(TwoStates_LocalState());
}

double TwoStates_LocalDollars()
{
    return TwoStates_GetDollars(TwoStates_LocalState());
}

void TwoStates_SyncBalancesFromHost(double r1, double r2, double u1, double u2)
{
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
    if (!isfinite(r1) || !isfinite(r2) || !isfinite(u1) || !isfinite(u2)) return;
    g_stateRub[0] = r1;
    g_stateRub[1] = r2;
    g_stateUsd[0] = u1;
    g_stateUsd[1] = u2;
    __try {
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static int g_cmdBuildingsBefore = 0;
static int g_cmdConstructionsBefore = 0;
static int g_cmdVehiclesBefore = 0;

static int GetWorldBuildingCount()
{
    return (int)SafeCountStdVector(g_exeBase + VEC_BUILDINGS_BEGIN_RVA, sizeof(void*));
}

static int GetWorldConstructionCount()
{
    return (int)SafeCountStdVector(g_exeBase + VEC_CONSTRUCTIONS_BEGIN_RVA, sizeof(void*));
}

static int GetWorldVehicleCount()
{
    return (int)SafeCountStdVector(g_exeBase + VEC_VEHICLES_BEGIN_RVA, sizeof(void*));
}

void TwoStates_BeginCommand(int ownerState)
{
    TwoStates_RestoreUiMoneySwap();
    g_cmdOwner = (ownerState == 2) ? 2 : 1;
    g_cmdBuildingsBefore     = GetWorldBuildingCount();
    g_cmdConstructionsBefore = GetWorldConstructionCount();
    g_cmdVehiclesBefore      = GetWorldVehicleCount();
    g_cmdResSnapshot.active  = false;
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) {
        g_inCommand = false;
        return;
    }
    g_inCommand = true;
    __try {
        if (g_exeBase) {
            BYTE* world = g_exeBase + WORLD_RVA;
            EnsureStateEconInitialized(world);
            SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_cmdBeforeScalars);
            BeginConstructResSnapshot(world, g_cmdOwner, &g_cmdResSnapshot);
        }
        *WorldRubles()  = g_stateRub[g_cmdOwner - 1];
        *WorldDollars() = g_stateUsd[g_cmdOwner - 1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void RegisterNewStructuresInVec(size_t beginRva, int countBefore, int countAfter, int owner)
{
    if (!g_exeBase || countAfter <= countBefore) return;
    __try {
        BYTE** arr = *(BYTE***)(g_exeBase + beginRva);
        for (int i = countBefore; arr && i < countAfter; i++) {
            if (arr[i]) {
                float x = *(const float*)(arr[i] + OBJ_POS_OFFSET);
                float z = *(const float*)(arr[i] + OBJ_POS_OFFSET + 8);
                TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(x, z), owner);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_EndCommand(int ownerState)
{
    int owner = (ownerState == 2) ? 2 : 1;
    // Register any new buildings and road/network construction sites placed during this command to `owner`
    // using their deterministic position key (since *(int*)b == 0 at creation time and is overwritten at 0x13CD0E).
    RegisterNewStructuresInVec(VEC_BUILDINGS_BEGIN_RVA,     g_cmdBuildingsBefore,     GetWorldBuildingCount(),     owner);
    RegisterNewStructuresInVec(VEC_CONSTRUCTIONS_BEGIN_RVA, g_cmdConstructionsBefore, GetWorldConstructionCount(), owner);

    // Register any new vehicles bought/spawned during this command to `owner`.
    int vehAfter = GetWorldVehicleCount();
    if (vehAfter > g_cmdVehiclesBefore) {
        __try {
            BYTE** v = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
            for (int i = g_cmdVehiclesBefore; v && i < vehAfter; i++) {
                if (v[i]) TwoStates_RegisterVehicleOwner(*(int*)(v[i] + VEH_ID_OFFSET), owner);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!g_inCommand || !TwoStates_IsActive() || IsUnlimitedMoney()) {
        if (g_cmdResSnapshot.active && g_exeBase) {
            EndConstructResSnapshot(g_exeBase + WORLD_RVA, owner, g_cmdResSnapshot);
            g_cmdResSnapshot.active = false;
        }
        g_inCommand = false;
        return;
    }
    g_inCommand = false;
    __try {
        if (g_exeBase) {
            BYTE* world = g_exeBase + WORLD_RVA;
            EndConstructResSnapshot(world, owner, g_cmdResSnapshot);
            g_cmdResSnapshot.active = false;
            StateEconScalars afterScalars = {};
            SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &afterScalars);
            AddStatScalarsDelta(&g_stateCurEcon[owner - 1].scalars, g_cmdBeforeScalars, afterScalars, 1.0f);
            g_lastWorldScalars = afterScalars;
        }
        double newRub = *WorldRubles();
        double newUsd = *WorldDollars();
        g_netTradeRub[owner - 1] += (newRub - g_stateRub[owner - 1]);
        g_netTradeUsd[owner - 1] += (newUsd - g_stateUsd[owner - 1]);
        g_stateRub[owner - 1] = newRub;
        g_stateUsd[owner - 1] = newUsd;
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_suppressCurStatLedgerHook = false;
        g_cmdResSnapshot.active = false;
    }
}

bool TwoStates_ApplyTransferCommand(const LsStateTransfer& t)
{
    if (!TwoStates_IsActive()) return false;
    if ((t.fromState != 1 && t.fromState != 2) || (t.toState != 1 && t.toState != 2) || t.fromState == t.toState)
        return false;
    if (!isfinite(t.rubles) || !isfinite(t.dollars) || t.rubles < 0.0 || t.dollars < 0.0)
        return false;
    if (t.rubles == 0.0 && t.dollars == 0.0)
        return false;

    int src = t.fromState - 1;
    int dst = t.toState - 1;
    double sendRub = t.rubles <= g_stateRub[src] ? t.rubles : g_stateRub[src];
    double sendUsd = t.dollars <= g_stateUsd[src] ? t.dollars : g_stateUsd[src];
    if (sendRub <= 0.0 && sendUsd <= 0.0) return false;

    g_stateRub[src] -= sendRub;
    g_stateRub[dst] += sendRub;
    g_stateUsd[src] -= sendUsd;
    g_stateUsd[dst] += sendUsd;

    if (g_inCommand) {
        __try {
            *WorldRubles()  = g_stateRub[g_cmdOwner - 1];
            *WorldDollars() = g_stateUsd[g_cmdOwner - 1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Перевод из %s в %s: %.0f ₽, %.0f $",
                  "[TWO STATES] Transfer from %s to %s: %.0f RUB, %.0f USD"),
               TwoStates_StateName(t.fromState), TwoStates_StateName(t.toState), sendRub, sendUsd);
    char toast[200];
    _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                Tr("★ Перевод из %s в %s: %.0f ₽ / %.0f $",
                   "★ Transfer from %s to %s: %.0f RUB / %.0f USD"),
                TwoStates_StateName(t.fromState), TwoStates_StateName(t.toState), sendRub, sendUsd);
    TwoStates_AddToast(toast, 1);
    return true;
}

void TwoStates_RequestTransfer(double rubles, double dollars)
{
    if (!TwoStates_IsActive()) return;
    if (rubles < 0.0) rubles = 0.0;
    if (dollars < 0.0) dollars = 0.0;
    if (rubles == 0.0 && dollars == 0.0) return;

    int me = TwoStates_LocalState();
    int other = (me == 1) ? 2 : 1;
    LsStateTransfer t = { me, other, rubles, dollars };
    if (Commands_Capturing()) {
        Commands_Submit(26 /* LS_CMD_STATE_TRANSFER */, &t, sizeof(t));
    } else {
        TwoStates_ApplyTransferCommand(t);
    }
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
    if (!TwoStates_IsActive()) return;
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

static void PrepareStructureVectorBeforeStep(size_t beginRva, size_t endRva)
{
    if (!g_exeBase) return;
    BYTE** it = *(BYTE***)(g_exeBase + beginRva);
    BYTE** end = *(BYTE***)(g_exeBase + endRva);
    if (!it || end <= it || (end - it) > 200000) return;
    for (; it < end; ++it) {
        BYTE* b = *it;
        if (!b) continue;
        float x = *(float*)(b + OBJ_POS_OFFSET);
        float z = *(float*)(b + OBJ_POS_OFFSET + 8);
        int owner = TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(x, z), x, z);
        int idx = owner - 1;
        g_stepBldCount[idx]++;
        float prog = *(float*)(b + BLD_PROGRESS_OFFSET);
        if (prog < 1.0f) {
            if (b[BLD_PAY_RUB_OFFSET] && g_stateRub[idx] <= 0.0) b[BLD_PAY_RUB_OFFSET] = 0;
            if (b[BLD_PAY_USD_OFFSET] && g_stateUsd[idx] <= 0.0) b[BLD_PAY_USD_OFFSET] = 0;
        }
    }
}

void TwoStates_BeforeStep(BYTE* world)
{
    TwoStates_RestoreUiMoneySwap();
    g_simLoopCurrentBuilding = nullptr;
    if (!TwoStates_IsActive() || !world) return;
    if (IsUnlimitedMoney()) {
        g_inSimStep = false;
        g_stateRub[0] = g_stateRub[1] = 100000000.0;
        g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
        return;
    }
    EnsureStateEconInitialized(world);
    g_stepBldCount[0] = g_stepBldCount[1] = 0;

    __try {
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        g_lastWorldRub = *WorldRubles();
        g_lastWorldUsd = *WorldDollars();
        g_unattributedRub = 0.0;
        g_unattributedUsd = 0.0;
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
        g_simStepThread = GetCurrentThreadId();
        g_inSimStep = true;

        PrepareStructureVectorBeforeStep(VEC_BUILDINGS_BEGIN_RVA, VEC_BUILDINGS_END_RVA);
        PrepareStructureVectorBeforeStep(VEC_CONSTRUCTIONS_BEGIN_RVA, VEC_CONSTRUCTIONS_END_RVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

double TwoStates_ComputeStepWeightState1(int countState1, int countState2)
{
    if (countState1 < 0) countState1 = 0;
    if (countState2 < 0) countState2 = 0;
    int total = countState1 + countState2;
    if (total <= 0) return 0.5;
    return (double)countState1 / (double)total;
}

void TwoStates_AfterStep(BYTE* world)
{
    g_simLoopCurrentBuilding = nullptr;
    if (!TwoStates_IsActive() || !world || IsUnlimitedMoney()) {
        g_inSimStep = false;
        return;
    }
    g_inSimStep = false;
    __try {
        FlushUnattributedScalarsToStates(world);
        ComputeStateCitizenDemographics(world, 1, &g_stateCurEcon[0].scalars);
        ComputeStateCitizenDemographics(world, 2, &g_stateCurEcon[1].scalars);

        double nowRub = *WorldRubles();
        double nowUsd = *WorldDollars();
        double dRub = g_unattributedRub + (nowRub - g_lastWorldRub);
        double dUsd = g_unattributedUsd + (nowUsd - g_lastWorldUsd);
        g_unattributedRub = 0.0;
        g_unattributedUsd = 0.0;

        double w0 = TwoStates_ComputeStepWeightState1(g_stepBldCount[0], g_stepBldCount[1]);

        if (fabs(dRub) > 1e-6) {
            double d0 = dRub * w0;
            g_stateRub[0] += d0;
            g_stateRub[1] += (dRub - d0);
            g_netTradeRub[0] += d0;
            g_netTradeRub[1] += (dRub - d0);
        }
        if (fabs(dUsd) > 1e-6) {
            double d0 = dUsd * w0;
            g_stateUsd[0] += d0;
            g_stateUsd[1] += (dUsd - d0);
            g_netTradeUsd[0] += d0;
            g_netTradeUsd[1] += (dUsd - d0);
        }
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void BackfillLegacyHistCitizenScalars(int sIdx)
{
    if (sIdx < 0 || sIdx > 1) return;
    const StateEconScalars& cur = g_stateCurEcon[sIdx].scalars;
    if (cur.citizenAverages[1] <= 0.0f) return;
    int curPop = cur.demographics[0] + cur.demographics[1] + cur.demographics[2] + cur.demographics[3];
    int curAdults = cur.demographics[3];
    std::vector<StateEconPeriodStats>& hist = g_stateHistEcon[sIdx];
    for (size_t i = 0; i < hist.size(); ++i) {
        StateEconScalars& hs = hist[i].scalars;
        int histPop = hs.demographics[0] + hs.demographics[1] + hs.demographics[2] + hs.demographics[3];
        if (histPop > 0 && hs.citizenAverages[1] <= 0.0f) {
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
}

static void BeginUiEconSwap(BYTE* world, int localState)
{
    if (!world || (localState != 1 && localState != 2)) return;
    EnsureStateEconInitialized(world);
    int sIdx = localState - 1;
    ComputeStateCitizenDemographics(world, localState, &g_stateCurEcon[sIdx].scalars);
    BackfillLegacyHistCitizenScalars(sIdx);

    SaveRecordToUiBackup(world + WORLD_STAT_CURRENT_OFFSET, &g_uiSavedCurStat);
    WriteStatePeriodToEngineRecordForUi(world + WORLD_STAT_CURRENT_OFFSET, g_stateCurEcon[sIdx]);

    BYTE* histBegin = nullptr;
    int histCount = 0;
    if (SafeReadStatHistHeader(world, &histBegin, &histCount) && histBegin && histCount > 0) {
        g_uiSavedHistBegin = histBegin;
        g_uiSavedHistCount = histCount;
        g_uiSavedHistStats.resize((size_t)histCount);
        int swapCount = (int)std::min((size_t)histCount, g_stateHistEcon[sIdx].size());
        for (int i = 0; i < histCount; ++i) {
            SaveRecordToUiBackup(histBegin + (size_t)i * STAT_RECORD_SIZE, &g_uiSavedHistStats[(size_t)i]);
            if (i < swapCount) {
                WriteStatePeriodToEngineRecordForUi(histBegin + (size_t)i * STAT_RECORD_SIZE,
                                                    g_stateHistEcon[sIdx][(size_t)i]);
            }
        }
    } else {
        g_uiSavedHistBegin = nullptr;
        g_uiSavedHistCount = 0;
        g_uiSavedHistStats.clear();
    }
    g_uiEconSwappedState = localState;
    SwapUiCitizenVecAndHudPop(world, localState);
    g_uiEconSwapped = true;
}

static void BeginUiLoanSwap(BYTE* world, bool filterByState)
{
    if (!world || !ReadCurrentLoansToVector(world, g_uiFullLoansBackup)) {
        g_uiLoansSwapped = false;
        return;
    }
    g_uiSavedGlobalSelectedIdx = SafeReadSelectedLoanIdx(world);
    SafeReadLoanStats(world, g_uiSavedLoanStats);
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
        SafeWriteLoanVectorInPlace(world, g_uiFilteredLoansBefore.data(), (int)g_uiFilteredLoansBefore.size());
        SafeWriteSelectedLoanIdx(world, g_uiLocalSelectedIdxBefore);
    }
    g_uiLoansSwapped = true;
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
        Determinism_GetFramePhase() != 0 || IsUnlimitedMoney()) {
        LeaveCriticalSection(&g_moneySwapLock);
        return;
    }

    bool twoStates = TwoStates_IsActive();
    bool captureLoans = twoStates || Commands_Capturing() || SaveSync_IsWaitingLobbyActive();
    if (!twoStates && !captureLoans) {
        LeaveCriticalSection(&g_moneySwapLock);
        return;
    }

    if (SafeReadPod(WorldRubles(), &g_uiSavedSimRub) && SafeReadPod(WorldDollars(), &g_uiSavedSimUsd)) {
        g_uiShownRub = twoStates ? TwoStates_LocalRubles() : g_uiSavedSimRub;
        g_uiShownUsd = twoStates ? TwoStates_LocalDollars() : g_uiSavedSimUsd;
        if (twoStates) {
            SafeWritePod(WorldRubles(), g_uiShownRub);
            SafeWritePod(WorldDollars(), g_uiShownUsd);
        }
        g_uiMoneySwapped = true;
    }

    if (twoStates) {
        BeginUiEconSwap(g_exeBase + WORLD_RVA, TwoStates_LocalState());
    }
    BeginUiLoanSwap(g_exeBase + WORLD_RVA, twoStates);
    LeaveCriticalSection(&g_moneySwapLock);
}

void TwoStates_UiEnd()
{
    TwoStates_RestoreUiMoneySwap();
}

void TwoStates_ToolMoneyBegin(double* outSavedSimRub, double* outSavedSimUsd)
{
    TwoStates_RestoreUiMoneySwap();
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
    __try {
        *outSavedSimRub = *WorldRubles();
        *outSavedSimUsd = *WorldDollars();
        *WorldRubles()  = TwoStates_LocalRubles();
        *WorldDollars() = TwoStates_LocalDollars();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_ToolMoneyEnd(double savedSimRub, double savedSimUsd)
{
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
    __try {
        *WorldRubles()  = savedSimRub;
        *WorldDollars() = savedSimUsd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---------------------------------------------------------------------------
// Territory, Building & Vehicle Ownership
// ---------------------------------------------------------------------------

int TwoStates_BuildingPosKey(float x, float z)
{
    if (!isfinite(x) || !isfinite(z)) return 0;
    uint32_t qx = (uint32_t)(int32_t)lroundf(x * 10.0f);
    uint32_t qz = (uint32_t)(int32_t)lroundf(z * 10.0f);
    uint32_t h = qx * 2654435761u ^ (qz * 2246822519u);
    h ^= (h >> 16);
    h *= 0x85EBCA6Bu;
    h ^= (h >> 13);
    return (int)(0x40000000u | (h & 0x3FFFFFFFu));
}

static void RegisterEntityOwner(std::unordered_map<int, int>& ownersMap, int entityId, int minId, int ownerState)
{
    if (entityId < minId || (ownerState != 1 && ownerState != 2)) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    ownersMap[entityId] = ownerState;
    LeaveCriticalSection(&g_ownerLock);
}

void TwoStates_RegisterBuildingOwner(int buildingId, int ownerState)
{
    RegisterEntityOwner(g_buildingOwners, buildingId, 1, ownerState);
}

int TwoStates_GetBuildingOwner(int buildingId, float x, float z)
{
    int posKey = TwoStates_BuildingPosKey(x, z);
    if (posKey > 0 || buildingId > 2) {
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        int found = 0;
        if (posKey > 0) {
            auto itPos = g_buildingOwners.find(posKey);
            if (itPos != g_buildingOwners.end()) found = itPos->second;
        }
        if (found != 1 && found != 2 && buildingId > 2 && buildingId != posKey) {
            auto itId = g_buildingOwners.find(buildingId);
            if (itId != g_buildingOwners.end()) found = itId->second;
        }
        LeaveCriticalSection(&g_ownerLock);
        if (found == 1 || found == 2) return found;
    }
    return (x < 0.0f) ? 1 : 2;
}

void TwoStates_RegisterVehicleOwner(int vehicleId, int ownerState)
{
    RegisterEntityOwner(g_vehicleOwners, vehicleId, 0, ownerState);
}

int TwoStates_GetVehicleOwner(int vehicleId, const BYTE* vehiclePtr)
{
    if (vehicleId >= 0) {
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        auto it = g_vehicleOwners.find(vehicleId);
        int found = (it != g_vehicleOwners.end()) ? it->second : 0;
        LeaveCriticalSection(&g_ownerLock);
        if (found == 1 || found == 2) return found;
    }
    if (vehiclePtr) {
        __try {
            const BYTE* home = *(const BYTE* const*)(vehiclePtr + VEH_HOME_OFFSET);
            if (!home) home = *(const BYTE* const*)(vehiclePtr + VEH_DEPOT_OFFSET);
            if (home) {
                float bx = *(const float*)(home + OBJ_POS_OFFSET);
                float bz = *(const float*)(home + OBJ_POS_OFFSET + 8);
                return TwoStates_GetBuildingOwner(TwoStates_BuildingPosKey(bx, bz), bx, bz);
            }
            float vx = *(const float*)(vehiclePtr + VEH_POS_OFFSET);
            return (vx < 0.0f) ? 1 : 2;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 0;
}

int TwoStates_GetZoneAt(float x, float z)
{
    (void)z;
    if (x < -g_borderHalfWidth) return 1; // West Republic
    if (x >  g_borderHalfWidth) return 2; // East Republic
    return 0;                             // Shared Border Zone
}

bool TwoStates_CanBuildAt(float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int zone = TwoStates_GetZoneAt(x, z);
    // Each state can build on its own side AND inside the shared border strip (zone == 0).
    return zone == 0 || zone == s;
}

bool TwoStates_CanDemolishAt(float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int zone = TwoStates_GetZoneAt(x, z);
    return zone == 0 || zone == s;
}

bool TwoStates_CanControlBuilding(int buildingId, float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int owner = TwoStates_GetBuildingOwner(buildingId, x, z);
    return owner == s;
}

bool TwoStates_CanControlVehicle(const BYTE* vehiclePtr, int state)
{
    if (!TwoStates_IsActive() || !vehiclePtr) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int id = -1;
    __try { id = *(const int*)(vehiclePtr + VEH_ID_OFFSET); } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    int owner = TwoStates_GetVehicleOwner(id, vehiclePtr);
    return owner == s;
}

static ULONGLONG g_lastRefuseLogMs = 0;

static void NotifyTerritoryRefused(const char* actionDesc, float x)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastRefuseLogMs < 1500) return;
    g_lastRefuseLogMs = now;
    int other = (TwoStates_LocalState() == 1) ? 2 : 1;
    const char* act = actionDesc ? actionDesc : Tr("действие", "action");
    SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Запрещено (%s на X=%.0f): это территория/собственность «%s»!",
                  "[TWO STATES] Forbidden (%s at X=%.0f): territory/property of '%s'!"),
               act, x, TwoStates_StateName(other));
    char toast[220];
    _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                Tr("⛔ Запрещено (%s): собственность «%s»!",
                   "⛔ Forbidden (%s): property of '%s'!"),
                act, TwoStates_StateName(other));
    TwoStates_AddToast(toast, 3);
}

bool TwoStates_CheckBuildAllowed(float x, float z, const char* actionDesc)
{
    if (TwoStates_CanBuildAt(x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckDemolishAllowed(float x, float z, const char* actionDesc)
{
    if (TwoStates_CanDemolishAt(x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckBuildingControlAllowed(int buildingId, float x, float z, const char* actionDesc)
{
    if (TwoStates_CanControlBuilding(buildingId, x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckBuildingControlAllowed(const BYTE* buildingPtr, const char* actionDesc)
{
    if (!TwoStates_IsActive() || !buildingPtr) return true;
    float x = 0.0f, z = 0.0f;
    __try {
        x = *(const float*)(buildingPtr + OBJ_POS_OFFSET);
        z = *(const float*)(buildingPtr + OBJ_POS_OFFSET + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return TwoStates_CheckBuildingControlAllowed(TwoStates_BuildingPosKey(x, z), x, z,
                                                 actionDesc ? actionDesc : Tr("настройка чужого здания", "configuring foreign building"));
}

bool TwoStates_CheckVehicleControlAllowed(const BYTE* vehiclePtr, const char* actionDesc)
{
    if (TwoStates_CanControlVehicle(vehiclePtr)) return true;
    float vx = 0.0f;
    __try { if (vehiclePtr) vx = *(const float*)(vehiclePtr + VEH_POS_OFFSET); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    NotifyTerritoryRefused(actionDesc, vx);
    return false;
}

bool TwoStates_GetCursorWorldPos(float* outX, float* outY, float* outZ)
{
    __try {
        BYTE* world = g_exeBase + WORLD_RVA;
        if (outX) *outX = *(float*)(world + CURSOR_X_OFFSET);
        if (outY) *outY = *(float*)(world + CURSOR_Y_OFFSET);
        if (outZ) *outZ = *(float*)(world + CURSOR_Z_OFFSET);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TwoStates_GetCameraLookAt(float* outX, float* outY, float* outZ)
{
    __try {
        const float* look = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_LOOKAT_OFFSET);
        if (outX) *outX = look[0];
        if (outY) *outY = look[1];
        if (outZ) *outZ = look[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static float SampleTerrainHeight(float wx, float wz)
{
    if (!g_collisionTerrainHeight || !SaveSync_IsWorldLoaded()) return 20.0f;
    float h = 20.0f;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* engine = *(BYTE**)(g_exeBase + ENGINE_RVA);
        BYTE* terrain = engine ? *(BYTE**)(engine + ENGINE_TERRAIN_OFFSET) : nullptr;
        if (terrain && *(BYTE**)(terrain + 0x840) != nullptr) {
            float pos3[3] = { wx, 0.0f, wz };
            float sampled = g_collisionTerrainHeight(terrain, pos3);
            if (isfinite(sampled)) h = sampled;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 20.0f;
    }
    DiagLog_EndSafeSeh();
    return h;
}

bool TwoStates_JumpCameraTo(float wx, float wz)
{
    if (!SaveSync_IsWorldLoaded() || !isfinite(wx) || !isfinite(wz)) return false;
    __try {
        float* camPos    = (float*)(g_exeBase + CAMERA_RVA + CAMERA_POS_OFFSET);
        float* camLookAt = (float*)(g_exeBase + CAMERA_RVA + CAMERA_LOOKAT_OFFSET);
        float dx = wx - camLookAt[0];
        float dz = wz - camLookAt[2];
        float heightOffset = camPos[1] - camLookAt[1];
        if (heightOffset < 35.0f || heightOffset > 1500.0f || !isfinite(heightOffset))
            heightOffset = 140.0f;
        float groundY = SampleTerrainHeight(wx, wz);
        camLookAt[0] = wx;
        camLookAt[1] = groundY;
        camLookAt[2] = wz;
        camPos[0] += dx;
        camPos[1]  = groundY + heightOffset;
        camPos[2] += dz;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


bool TwoStates_JumpCameraToPartner()
{
    PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
    if (!pc.valid) {
        TwoStates_AddToast(Tr("Партнёр ещё не подключён в режиме Lockstep",
                              "Partner is not connected in Lockstep mode yet"), 2);
        return false;
    }
    ULONGLONG now = GetTickCount64();
    float tx = pc.x, tz = pc.z;
    if (pc.hasPing && now - pc.pingReceivedMs < PING_LIFETIME_MS) {
        tx = pc.pingX;
        tz = pc.pingZ;
    } else if (fabsf(tx) < 0.01f && fabsf(tz) < 0.01f) {
        tx = pc.camX;
        tz = pc.camZ;
    }
    if (TwoStates_JumpCameraTo(tx, tz)) {
        char msg[160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    Tr("[КАМЕРА] Перемещена к партнёру (X=%.0f м, Z=%.0f м)",
                       "[CAMERA] Jumped to partner (X=%.0f m, Z=%.0f m)"),
                    tx, tz);
        TwoStates_AddToast(msg, 0);
        return true;
    }
    return false;
}

void TwoStates_SetShow3DBorder(bool on) { g_show3DBorder = on; }
bool TwoStates_GetShow3DBorder()         { return g_show3DBorder; }

void TwoStates_PingAtCursor()
{
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    if (!TwoStates_GetCursorWorldPos(&cx, &cy, &cz)) return;
    TwoStates_GetCameraLookAt(&lx, &ly, &lz);
    if (fabsf(cx) < 0.01f && fabsf(cz) < 0.01f) {
        cx = lx; cy = ly; cz = lz;
    }
    g_localPingActive = true;
    g_localPingX      = cx;
    g_localPingY      = cy;
    g_localPingZ      = cz;
    g_localPingSentMs = GetTickCount64();

    if (LockstepNet_IsConnected()) {
        LockstepNet_SendCursor(cx, cy, cz, lx, lz, true);
    }
    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                Tr("[СИГНАЛ] Отправлен на карту на 20 сек (X=%.0f м, Z=%.0f м)",
                   "[PING] Sent on map for 20s (X=%.0f m, Z=%.0f m)"),
                cx, cz);
    TwoStates_AddToast(msg, 1);
    SaveStatus(Tr("[КАРТА] Отправлен сигнал партнёру: X=%.0f м, Z=%.0f м",
                  "[MAP] Ping sent to partner: X=%.0f m, Z=%.0f m"), cx, cz);
}

static bool WorldToScreen(float wx, float wy, float wz, const ImVec2& displaySize, ImVec2* outScreen)
{
    __try {
        const float* m = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_VIEWPROJ_OFFSET);
        float clipX = wx * m[0] + wy * m[4] + wz * m[8]  + m[12];
        float clipY = wx * m[1] + wy * m[5] + wz * m[9]  + m[13];
        float clipW = wx * m[3] + wy * m[7] + wz * m[11] + m[15];
        if (clipW <= 0.5f || !isfinite(clipW)) return false;
        float invW = 1.0f / clipW;
        float ndcX = clipX * invW;
        float ndcY = clipY * invW;
        if (ndcX < -1.6f || ndcX > 1.6f || ndcY < -1.6f || ndcY > 1.6f) return false;
        outScreen->x = (ndcX * 0.5f + 0.5f) * displaySize.x;
        outScreen->y = (0.5f - ndcY * 0.5f) * displaySize.y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void Draw3DTextBadge(ImDrawList* dl, const ImVec2& pos, const char* text, ImU32 bgCol, ImU32 borderCol, ImU32 textCol)
{
    ImVec2 sz = ImGui::CalcTextSize(text);
    ImVec2 pMin(pos.x - sz.x * 0.5f - 7.0f, pos.y - sz.y * 0.5f - 4.0f);
    ImVec2 pMax(pos.x + sz.x * 0.5f + 7.0f, pos.y + sz.y * 0.5f + 4.0f);
    dl->AddRectFilled(pMin, pMax, bgCol, 5.0f);
    dl->AddRect(pMin, pMax, borderCol, 5.0f, 0, 1.5f);
    dl->AddText(ImVec2(pos.x - sz.x * 0.5f, pos.y - sz.y * 0.5f), textCol, text);
}

// Native Minimap pointer at world + 0xD400 (RVA 0x9E2310), HUD hidden flag at world + 0xFBA (RVA 0x9D5ECA).
static bool GetNativeMinimapRect(float* outCenterX, float* outCenterY, float* outMapSize)
{
    if (!g_exeBase) return false;
    __try {
        BYTE hideHud = *(BYTE*)(g_exeBase + WORLD_RVA + 0xFBA);
        if (hideHud != 0) return false;
        BYTE* mm = *(BYTE**)(g_exeBase + WORLD_RVA + 0xD400);
        if (!mm) return false;
        if (mm[0] == 0) return false; // minimap panel collapsed
        float rightX  = *(float*)(mm + 0x48);
        float bottomY = *(float*)(mm + 0x4C);
        float mapSize = *(float*)(mm + 0x50);
        float panelW  = *(float*)(mm + 0x54);
        float panelH  = *(float*)(mm + 0x58);
        if (!isfinite(mapSize) || mapSize < 40.0f || mapSize > 2500.0f) return false;
        if (!isfinite(rightX)  || !isfinite(bottomY) || !isfinite(panelW) || !isfinite(panelH)) return false;
        *outCenterX = (rightX - panelW) + panelH * 0.5f;
        *outCenterY = bottomY - panelH * 0.5f;
        *outMapSize = mapSize;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void DrawMinimapOverlay(ImDrawList* dl)
{
    const bool inCoop = g_inSession || SaveSync_IsBusy() || SaveSync_IsWaitingLobbyActive();
    if (!inCoop) return;

    float cx = 0.0f, cy = 0.0f, mapSize = 0.0f;
    if (!GetNativeMinimapRect(&cx, &cy, &mapSize)) return;

    float half = mapSize * 0.5f;
    ImVec2 mapMin(cx - half, cy - half);
    ImVec2 mapMax(cx + half, cy + half);

    dl->PushClipRect(mapMin, mapMax, true);

    bool twoStates = TwoStates_IsActive();
    if (twoStates) {
        int myState = TwoStates_LocalState(); // 1 = West (X < 0), 2 = East (X > 0)
        float borderZoneHalfPx = (g_borderHalfWidth / 20000.0f) * mapSize;
        if (borderZoneHalfPx < 1.5f) borderZoneHalfPx = 1.5f;

        ImU32 ownTint     = IM_COL32(45, 210, 95, 36);
        ImU32 partnerTint = IM_COL32(235, 65, 55, 36);
        ImU32 westTint    = (myState == 1) ? ownTint : partnerTint;
        ImU32 eastTint    = (myState == 2) ? ownTint : partnerTint;

        // Subtle territory tint on West (X < 0) and East (X > 0) halves
        dl->AddRectFilled(mapMin, ImVec2(cx - borderZoneHalfPx, mapMax.y), westTint);
        dl->AddRectFilled(ImVec2(cx + borderZoneHalfPx, mapMin.y), mapMax, eastTint);

        // Shared border zone band [-g_borderHalfWidth .. +g_borderHalfWidth]
        dl->AddRectFilled(ImVec2(cx - borderZoneHalfPx, mapMin.y),
                          ImVec2(cx + borderZoneHalfPx, mapMax.y),
                          IM_COL32(255, 215, 60, 42));

        // Dashed/highlighted border zone edges
        dl->AddLine(ImVec2(cx - borderZoneHalfPx, mapMin.y),
                    ImVec2(cx - borderZoneHalfPx, mapMax.y),
                    IM_COL32(90, 200, 255, 175), 1.2f);
        dl->AddLine(ImVec2(cx + borderZoneHalfPx, mapMin.y),
                    ImVec2(cx + borderZoneHalfPx, mapMax.y),
                    IM_COL32(255, 175, 70, 175), 1.2f);

        // Center state border line X = 0
        dl->AddLine(ImVec2(cx, mapMin.y), ImVec2(cx, mapMax.y), IM_COL32(255, 65, 50, 235), 2.5f);
        dl->AddLine(ImVec2(cx, mapMin.y), ImVec2(cx, mapMax.y), IM_COL32(255, 230, 95, 200), 1.2f);

        // Highlight border around the player's own half of the minimap
        ImVec2 ownMin = (myState == 1) ? mapMin : ImVec2(cx, mapMin.y);
        ImVec2 ownMax = (myState == 1) ? ImVec2(cx, mapMax.y) : mapMax;
        dl->AddRect(ownMin, ownMax, IM_COL32(75, 240, 120, 200), 0.0f, 0, 2.0f);

        // Top badges inside West & East halves showing which side is YOURS vs PARTNER'S
        const char* westLabel = (myState == 1) ? Tr("ЗАПАД (ВЫ)", "WEST (YOU)") : Tr("ЗАПАД (ПАРТНЁР)", "WEST (PARTNER)");
        const char* eastLabel = (myState == 2) ? Tr("ВОСТОК (ВЫ)", "EAST (YOU)") : Tr("ВОСТОК (ПАРТНЁР)", "EAST (PARTNER)");

        ImU32 ownBg       = IM_COL32(16, 48, 24, 225);
        ImU32 ownBrd      = IM_COL32(80, 240, 125, 245);
        ImU32 ownTxt      = IM_COL32(185, 255, 200, 255);
        ImU32 partnerBg   = IM_COL32(52, 20, 20, 220);
        ImU32 partnerBrd  = IM_COL32(255, 110, 95, 235);
        ImU32 partnerTxt  = IM_COL32(255, 205, 195, 255);

        float badgeY = mapMin.y + 13.0f;
        float westCx = mapMin.x + half * 0.5f;
        float eastCx = cx + half * 0.5f;

        Draw3DTextBadge(dl, ImVec2(westCx, badgeY), westLabel,
                        (myState == 1) ? ownBg : partnerBg,
                        (myState == 1) ? ownBrd : partnerBrd,
                        (myState == 1) ? ownTxt : partnerTxt);
        Draw3DTextBadge(dl, ImVec2(eastCx, badgeY), eastLabel,
                        (myState == 2) ? ownBg : partnerBg,
                        (myState == 2) ? ownBrd : partnerBrd,
                        (myState == 2) ? ownTxt : partnerTxt);
    }

    ULONGLONG now = GetTickCount64();

    // Draw local ping marker on minimap (20s)
    if (g_localPingActive && now - g_localPingSentMs < PING_LIFETIME_MS) {
        float ageSec = (float)(now - g_localPingSentMs) * 0.001f;
        float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
        float px = cx + (g_localPingX / 20000.0f) * mapSize;
        float py = cy - (g_localPingZ / 20000.0f) * mapSize;
        dl->AddCircle(ImVec2(px, py), 5.0f + pulse * 7.0f, IM_COL32(110, 245, 130, 235), 20, 2.0f);
        dl->AddCircleFilled(ImVec2(px, py), 3.5f, IM_COL32(80, 255, 120, 255));
    }

    // Draw partner cursor & partner ping marker on minimap
    if (LockstepNet_IsConnected()) {
        PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
        if (pc.valid) {
            if (now - pc.lastUpdateMs < 10000 && (fabsf(pc.x) > 0.01f || fabsf(pc.z) > 0.01f)) {
                float px = cx + (pc.x / 20000.0f) * mapSize;
                float py = cy - (pc.z / 20000.0f) * mapSize;
                dl->AddCircleFilled(ImVec2(px, py), 3.5f, IM_COL32(60, 215, 255, 240));
                dl->AddCircle(ImVec2(px, py), 6.5f, IM_COL32(255, 255, 255, 200), 16, 1.5f);
            }
            if (pc.hasPing && now - pc.pingReceivedMs < PING_LIFETIME_MS) {
                float ageSec = (float)(now - pc.pingReceivedMs) * 0.001f;
                float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
                float px = cx + (pc.pingX / 20000.0f) * mapSize;
                float py = cy - (pc.pingZ / 20000.0f) * mapSize;
                dl->AddCircle(ImVec2(px, py), 6.0f + pulse * 9.0f, IM_COL32(255, 215, 50, 245), 24, 2.2f);
                dl->AddCircleFilled(ImVec2(px, py), 4.0f, IM_COL32(255, 85, 40, 255));
            }
        }
    }

    dl->PopClipRect();
}

static void Draw3DBorderLines(ImDrawList* dl, const ImVec2& displaySize)
{
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    if (!TwoStates_GetCameraLookAt(&lx, &ly, &lz)) return;
    if (fabsf(lx) > (g_borderHalfWidth + 3800.0f)) return; // camera is far away from the X=0 border

    float zMin = lz - 2400.0f;
    float zMax = lz + 2400.0f;
    if (zMin < -14500.0f) zMin = -14500.0f;
    if (zMax >  14500.0f) zMax =  14500.0f;
    const float step = 60.0f;

    // 1. Center state border line X = 0.0f
    ImVec2 prevPt;
    bool havePrev = false;
    for (float z = zMin; z <= zMax; z += step) {
        float y = SampleTerrainHeight(0.0f, z) + 2.5f;
        ImVec2 pt;
        if (WorldToScreen(0.0f, y, z, displaySize, &pt)) {
            if (havePrev) {
                dl->AddLine(prevPt, pt, IM_COL32(255, 70, 50, 225), 3.5f);
                dl->AddLine(prevPt, pt, IM_COL32(255, 220, 90, 180), 1.5f);
            }
            // Draw vertical border post every 240m
            int zIdx = (int)roundf(z / 60.0f);
            if ((zIdx % 4) == 0) {
                ImVec2 topPt;
                if (WorldToScreen(0.0f, y + 18.0f, z, displaySize, &topPt)) {
                    dl->AddLine(pt, topPt, IM_COL32(255, 215, 70, 210), 2.2f);
                    dl->AddCircleFilled(topPt, 4.0f, IM_COL32(255, 65, 50, 240));
                }
            }
            prevPt = pt;
            havePrev = true;
        } else {
            havePrev = false;
        }
    }

    // 2. Shared border zone lines X = -g_borderHalfWidth and X = +g_borderHalfWidth
    float edges[2] = { -g_borderHalfWidth, g_borderHalfWidth };
    ImU32 edgeCols[2] = { IM_COL32(80, 190, 255, 175), IM_COL32(255, 170, 70, 175) };
    for (int side = 0; side < 2; ++side) {
        float bx = edges[side];
        havePrev = false;
        int seg = 0;
        for (float z = zMin; z <= zMax; z += step, ++seg) {
            float y = SampleTerrainHeight(bx, z) + 2.0f;
            ImVec2 pt;
            if (WorldToScreen(bx, y, z, displaySize, &pt)) {
                if (havePrev && (seg & 1) == 0) {
                    dl->AddLine(prevPt, pt, edgeCols[side], 2.0f);
                }
                prevPt = pt;
                havePrev = true;
            } else {
                havePrev = false;
            }
        }
    }

    // 3. Single clean 3D badge on the main border (no side badges that collide when zoomed out)
    float labelY = SampleTerrainHeight(0.0f, lz) + 24.0f;
    ImVec2 centerScreen;
    if (WorldToScreen(0.0f, labelY, lz, displaySize, &centerScreen)) {
        Draw3DTextBadge(dl, centerScreen,
                        Tr("<< ЗАПАДНАЯ ССР  |  ГРАНИЦА  |  ВОСТОЧНАЯ ССР >>",
                           "<< WEST SSR  |  BORDER  |  EAST SSR >>"),
                        IM_COL32(20, 22, 28, 220), IM_COL32(255, 200, 60, 230), IM_COL32(255, 235, 140, 255));
    }
}

static void DrawPartnerCursorAndPing(ImDrawList* dl, const ImVec2& displaySize)
{
    ULONGLONG now = GetTickCount64();

    // Draw local 3D Map Ping beacon (visible for 20 seconds)
    if (g_localPingActive && now - g_localPingSentMs < PING_LIFETIME_MS) {
        float ageSec = (float)(now - g_localPingSentMs) * 0.001f;
        int remSec = (int)ceilf((float)(PING_LIFETIME_MS - (now - g_localPingSentMs)) * 0.001f);
        float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
        float py = g_localPingY > 1.0f ? g_localPingY : SampleTerrainHeight(g_localPingX, g_localPingZ);
        ImVec2 basePt, beamTopPt;
        if (WorldToScreen(g_localPingX, py + 1.0f, g_localPingZ, displaySize, &basePt)) {
            dl->AddCircle(basePt, 12.0f + pulse * 14.0f, IM_COL32(95, 240, 125, 225), 32, 2.5f);
            dl->AddCircleFilled(basePt, 6.0f, IM_COL32(60, 225, 95, 245));
            if (WorldToScreen(g_localPingX, py + 55.0f, g_localPingZ, displaySize, &beamTopPt)) {
                dl->AddLine(basePt, beamTopPt, IM_COL32(95, 240, 125, 210), 3.0f);
                char pingText[128];
                _snprintf_s(pingText, sizeof(pingText), _TRUNCATE,
                            Tr("[ВАША МЕТКА] (X=%.0f, Z=%.0f) [%dс]", "[YOUR PING] (X=%.0f, Z=%.0f) [%ds]"),
                            g_localPingX, g_localPingZ, remSec);
                Draw3DTextBadge(dl, ImVec2(beamTopPt.x, beamTopPt.y - 14.0f), pingText,
                                IM_COL32(15, 42, 22, 230), IM_COL32(95, 240, 125, 250), IM_COL32(190, 255, 205, 255));
            }
        }
    }

    if (!LockstepNet_IsConnected()) return;
    PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
    if (!pc.valid) return;

    static unsigned s_lastPingSeq = 0;
    if (pc.hasPing && pc.pingSeq != s_lastPingSeq) {
        s_lastPingSeq = pc.pingSeq;
        char toast[200];
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("[СИГНАЛ] От партнёра на карте (X=%.0f м, Z=%.0f м) — нажмите F6 для перехода!",
                       "[PING] Map ping from partner (X=%.0f m, Z=%.0f m) — press F6 to jump!"),
                    pc.pingX, pc.pingZ);
        TwoStates_AddToast(toast, 2);
    }

    int partnerState = (TwoStates_LocalState() == 1) ? 2 : 1;
    const char* partnerLabel = TwoStates_IsActive()
        ? (partnerState == 1 ? Tr("Курсор: Западная ССР (Хост)", "Cursor: West SSR (Host)")
                             : Tr("Курсор: Восточная ССР (Игрок 2)", "Cursor: East SSR (Player 2)"))
        : (LockstepNet_IsHost() ? Tr("Курсор: Игрок 2", "Cursor: Player 2") : Tr("Курсор: Хост", "Cursor: Host"));

    // Draw partner 3D cursor
    if (now - pc.lastUpdateMs < 10000 && (fabsf(pc.x) > 0.01f || fabsf(pc.z) > 0.01f)) {
        float wy = pc.y > 1.0f ? pc.y : SampleTerrainHeight(pc.x, pc.z);
        ImVec2 curPt, topPt;
        if (WorldToScreen(pc.x, wy + 1.5f, pc.z, displaySize, &curPt)) {
            dl->AddCircleFilled(curPt, 6.0f, IM_COL32(60, 215, 255, 230));
            dl->AddCircle(curPt, 13.0f, IM_COL32(60, 215, 255, 180), 24, 2.0f);
            if (WorldToScreen(pc.x, wy + 16.0f, pc.z, displaySize, &topPt)) {
                dl->AddLine(curPt, topPt, IM_COL32(60, 215, 255, 200), 2.0f);
                Draw3DTextBadge(dl, ImVec2(topPt.x, topPt.y - 12.0f), partnerLabel,
                                IM_COL32(15, 28, 38, 215), IM_COL32(60, 215, 255, 230), IM_COL32(180, 240, 255, 255));
            }
        }
    }

    // Draw active 3D Map Ping beacon (visible for 20 seconds)
    if (pc.hasPing && now - pc.pingReceivedMs < PING_LIFETIME_MS) {
        float ageSec = (float)(now - pc.pingReceivedMs) * 0.001f;
        int remSec = (int)ceilf((float)(PING_LIFETIME_MS - (now - pc.pingReceivedMs)) * 0.001f);
        float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
        float py = pc.pingY > 1.0f ? pc.pingY : SampleTerrainHeight(pc.pingX, pc.pingZ);
        ImVec2 basePt, beamTopPt;
        if (WorldToScreen(pc.pingX, py + 1.0f, pc.pingZ, displaySize, &basePt)) {
            dl->AddCircle(basePt, 14.0f + pulse * 18.0f, IM_COL32(255, 215, 50, 230), 32, 3.0f);
            dl->AddCircleFilled(basePt, 7.0f, IM_COL32(255, 90, 40, 245));
            if (WorldToScreen(pc.pingX, py + 65.0f, pc.pingZ, displaySize, &beamTopPt)) {
                dl->AddLine(basePt, beamTopPt, IM_COL32(255, 220, 60, 220), 3.5f);
                char pingText[128];
                _snprintf_s(pingText, sizeof(pingText), _TRUNCATE,
                            Tr("[СИГНАЛ ПАРТНЁРА] (X=%.0f, Z=%.0f) [%dс | F6]",
                               "[PARTNER PING] (X=%.0f, Z=%.0f) [%ds | F6]"),
                            pc.pingX, pc.pingZ, remSec);
                Draw3DTextBadge(dl, ImVec2(beamTopPt.x, beamTopPt.y - 14.0f), pingText,
                                IM_COL32(45, 30, 10, 235), IM_COL32(255, 210, 50, 255), IM_COL32(255, 240, 150, 255));
            }
        }
    }
}

void TwoStates_DrawOverlay()
{
    const bool inCoop = g_inSession || SaveSync_IsBusy() || SaveSync_IsWaitingLobbyActive();
    if (!inCoop) return;

    bool worldLoaded = SaveSync_IsWorldLoaded();
    bool focused = IsGameWindowFocused();

    // Hotkeys when game window is focused:
    //   F7: toggle 3D state border lines (Two States mode only)
    //   F6: jump camera to partner / active ping
    //   Alt + LMB: send map ping at cursor
    static bool s_f7WasDown = false;
    static bool s_f6WasDown = false;
    static bool s_altClickWasDown = false;

    bool allowWorldHotkeys = focused && !SyncBlock_BlocksWorldInput();

    bool f7Down = allowWorldHotkeys && TwoStates_IsActive() && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7Down && !s_f7WasDown) {
        g_show3DBorder = !g_show3DBorder;
        TwoStates_SaveSettings();
        TwoStates_AddToast(g_show3DBorder
            ? Tr("3D-граница государств: ВКЛЮЧЕНА [F7]", "3D State Border: ON [F7]")
            : Tr("3D-граница государств: СКРЫТА [F7]", "3D State Border: OFF [F7]"), 0);
    }
    s_f7WasDown = f7Down;

    bool f6Down = allowWorldHotkeys && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6Down && !s_f6WasDown && worldLoaded) {
        TwoStates_JumpCameraToPartner();
    }
    s_f6WasDown = f6Down;

    bool altClickDown = allowWorldHotkeys && !ImGui::GetIO().WantCaptureMouse &&
                        (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
                        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (altClickDown && !s_altClickWasDown && worldLoaded) {
        TwoStates_PingAtCursor();
    }
    s_altClickWasDown = altClickDown;

    // Periodically send local 3D cursor & camera lookAt to partner (~10 Hz)
    static ULONGLONG s_lastCursorSendMs = 0;
    ULONGLONG now = GetTickCount64();
    if (worldLoaded && LockstepNet_IsConnected() && now - s_lastCursorSendMs >= 100) {
        s_lastCursorSendMs = now;
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        float lx = 0.0f, ly = 0.0f, lz = 0.0f;
        TwoStates_GetCursorWorldPos(&cx, &cy, &cz);
        TwoStates_GetCameraLookAt(&lx, &ly, &lz);
        LockstepNet_SendCursor(cx, cy, cz, lx, lz, false);
    }

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // Draw 3D border lines whenever world is loaded & Two States mode is active & g_show3DBorder is true
    if (worldLoaded && g_show3DBorder && TwoStates_IsActive()) {
        Draw3DBorderLines(bg, displaySize);
    }

    // Draw 3D partner cursor and map ping beacon (local + partner, 20s lifetime)
    if (worldLoaded) {
        DrawPartnerCursorAndPing(bg, displaySize);
        DrawMinimapOverlay(bg);
    }

    // Draw Toast notifications (top-center below the game's resource bar)
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    while (!g_toasts.empty() && now - g_toasts.front().createdMs > g_toasts.front().durationMs) {
        g_toasts.pop_front();
    }
    float toastY = 58.0f;
    for (size_t i = 0; i < g_toasts.size(); ++i) {
        const ToastMessage& tm = g_toasts[i];
        ImU32 borderCol = IM_COL32(90, 180, 255, 230);
        ImU32 textCol   = IM_COL32(230, 245, 255, 255);
        if (tm.colorKind == 1) { borderCol = IM_COL32(80, 220, 100, 235); textCol = IM_COL32(210, 255, 215, 255); }
        else if (tm.colorKind == 2) { borderCol = IM_COL32(255, 205, 60, 240); textCol = IM_COL32(255, 240, 180, 255); }
        else if (tm.colorKind == 3) { borderCol = IM_COL32(255, 80, 70, 245); textCol = IM_COL32(255, 205, 200, 255); }

        Draw3DTextBadge(bg, ImVec2(displaySize.x * 0.5f, toastY), tm.text,
                        IM_COL32(18, 22, 28, 235), borderCol, textCol);
        toastY += 28.0f;
    }
    LeaveCriticalSection(&g_ownerLock);
}

// ---------------------------------------------------------------------------
// Map Expansion: Border Unlocker (20x20 km) & 30x30 km Generator
// ---------------------------------------------------------------------------

void TwoStates_SetExpandBorders(bool on)
{
    g_expandBorders = on;
}

bool TwoStates_GetExpandBorders()
{
    return g_expandBorders;
}

// Invariant #4 (AGENTS.md): Base terrain maps (media_soviet\terrains, dlc*\terrains) are inviolable.
// TwoStates_PatchSaveScriptIni may ONLY modify script.ini inside save directories (save, save_coop, mp_syncsave_*).
bool TwoStates_IsSafeSaveScriptPath(const char* saveDir)
{
    if (!saveDir || !saveDir[0]) return false;
    std::string lower(saveDir);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '/') c = '\\';
    }
    if (lower.find("\\terrains\\") != std::string::npos ||
        lower.find("\\terrains") == (lower.size() >= 9 ? lower.size() - 9 : std::string::npos) ||
        lower.rfind("terrains\\", 0) == 0) {
        return false;
    }
    if (lower.find("\\save\\") != std::string::npos ||
        lower.find("\\save_coop\\") != std::string::npos ||
        lower.find("mp_syncsave_") != std::string::npos ||
        lower.rfind("save\\", 0) == 0) {
        return true;
    }
    return false;
}

bool TwoStates_PatchSaveScriptIni(const char* saveDir, float customWorldSizeMeters)
{
    if (!TwoStates_IsSafeSaveScriptPath(saveDir)) return false;
    if (!g_expandBorders && customWorldSizeMeters <= 0.0f) return false;
    char path[MAX_PATH] = {};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\script.ini", saveDir);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return false;
    }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    float worldSize = customWorldSizeMeters > 20000.0f ? customWorldSizeMeters : 20000.0f;
    float halfEdge = worldSize * 0.5f - EXPANDED_BOUNDARY_MARGIN; // 9850m for 20km, 14850m for 30km

    std::istringstream in(content);
    std::ostringstream out;
    std::string line;
    int polyVertsLeft = 0;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (polyVertsLeft > 0) {
            float vx = 0, vy = 0, vz = 0;
            int flag = 0;
            if (sscanf_s(line.c_str(), "%f %f %f %d", &vx, &vy, &vz, &flag) == 4) {
                vx = (vx < 0.0f) ? -halfEdge : halfEdge;
                vz = (vz < 0.0f) ? -halfEdge : halfEdge;
                char buf[128];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f %.1f %.1f %d", vx, vy, vz, flag);
                out << buf << "\r\n";
                polyVertsLeft--;
                continue;
            }
            polyVertsLeft = 0;
        }

        if (customWorldSizeMeters > 20000.0f && line.rfind("$WORLDSIZE", 0) == 0) {
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "$WORLDSIZE %.6f %.6f", customWorldSizeMeters, customWorldSizeMeters);
            out << buf << "\r\n";
            continue;
        }
        if (customWorldSizeMeters > 20000.0f && line.rfind("$DRAWDISTANCE", 0) == 0) {
            out << "$DRAWDISTANCE 75000.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_XMIN", 0) == 0) {
            out << "$BOUNDARY_XMIN 150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_ZMIN", 0) == 0) {
            out << "$BOUNDARY_ZMIN 150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_XMAX", 0) == 0) {
            out << "$BOUNDARY_XMAX -150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_ZMAX", 0) == 0) {
            out << "$BOUNDARY_ZMAX -150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BORDER_POLYGON", 0) == 0) {
            int count = 0;
            if (sscanf_s(line.c_str(), "$BORDER_POLYGON %d", &count) == 1 && count > 0 && count <= 64) {
                polyVertsLeft = count;
            }
            out << line << "\r\n";
            continue;
        }

        out << line << "\r\n";
    }

    std::string patched = out.str();
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(patched.data(), 1, patched.size(), f);
    fclose(f);

    Logf("republicmp  TWO_STATES: patched [%s] (worldSize=%.0fm, border=±%.0fm)", path, worldSize, halfEdge);
    return true;
}

// ---------------------------------------------------------------------------
// Persistent Global Co-op Settings (republicmp_settings.ini)
// ---------------------------------------------------------------------------

static bool GetExeDir(char* outDir, size_t outSize)
{
    if (!outDir || outSize == 0) return false;
    outDir[0] = '\0';
    if (g_baseDir && g_baseDir[0] != '\0') {
        strncpy_s(outDir, outSize, g_baseDir, _TRUNCATE);
        return true;
    }
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0) {
        len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    }
    if (len == 0 || len == MAX_PATH) return false;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return false;
    *lastSlash = '\0';
    strncpy_s(outDir, outSize, exePath, _TRUNCATE);
    return true;
}

void TwoStates_LoadSettings()
{
    std::string ini = Lang_GetConfigPath();
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    int mode = GetPrivateProfileIntA("coop", "mode", (int)g_chosenMode, ini.c_str());
    g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (!g_inSession) g_sessionMode = g_chosenMode;

    int fullCap = GetPrivateProfileIntA("coop", "full_starting_capital", g_fullStartingCapital ? 1 : 0, ini.c_str());
    g_fullStartingCapital = (fullCap != 0);

    int bw = GetPrivateProfileIntA("coop", "border_half_width_m", (int)lroundf(g_borderHalfWidth), ini.c_str());
    if (bw >= 50 && bw <= 2000) g_borderHalfWidth = (float)bw;

    int expB = GetPrivateProfileIntA("coop", "expand_borders", g_expandBorders ? 1 : 0, ini.c_str());
    g_expandBorders = (expB != 0);

    int show3d = GetPrivateProfileIntA("coop", "show_3d_border", g_show3DBorder ? 1 : 0, ini.c_str());
    g_show3DBorder = (show3d != 0);

    int dlc = GetPrivateProfileIntA("coop", "dlc_check", SaveSync_GetDlcCheck() ? 1 : 0, ini.c_str());
    SaveSync_SetDlcCheck(dlc != 0);

    int sendLogs = GetPrivateProfileIntA("coop", "send_client_logs", g_sendClientLogs ? 1 : 0, ini.c_str());
    g_sendClientLogs = (sendLogs != 0);

    char addr[64] = {};
    GetPrivateProfileStringA("coop", "last_join_address", "", addr, sizeof(addr), ini.c_str());
    if (addr[0]) {
        strncpy_s(g_savedJoinAddress, addr, _TRUNCATE);
    }

    ModSync_LoadSettingsFromIni(ini.c_str());

    Logf("republicmp  SETTINGS: loaded from [%s] (mode=%d, fullCap=%d, border=%dm, expand=%d, show3d=%d, dlc=%d, sendLogs=%d, addr='%s')",
         ini.c_str(), (int)g_chosenMode, g_fullStartingCapital ? 1 : 0, (int)g_borderHalfWidth,
         g_expandBorders ? 1 : 0, g_show3DBorder ? 1 : 0, SaveSync_GetDlcCheck() ? 1 : 0,
         g_sendClientLogs ? 1 : 0, g_savedJoinAddress);
}

void TwoStates_SaveSettings()
{
    std::string ini = Lang_GetConfigPath();
    char buf[32] = {};

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", (int)g_chosenMode);
    WritePrivateProfileStringA("coop", "mode", buf, ini.c_str());

    WritePrivateProfileStringA("coop", "full_starting_capital", g_fullStartingCapital ? "1" : "0", ini.c_str());

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", (int)lroundf(g_borderHalfWidth));
    WritePrivateProfileStringA("coop", "border_half_width_m", buf, ini.c_str());

    WritePrivateProfileStringA("coop", "expand_borders", g_expandBorders ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "show_3d_border", g_show3DBorder ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "dlc_check", SaveSync_GetDlcCheck() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "send_client_logs", g_sendClientLogs ? "1" : "0", ini.c_str());

    if (g_savedJoinAddress[0]) {
        WritePrivateProfileStringA("coop", "last_join_address", g_savedJoinAddress, ini.c_str());
    }
    ModSync_SaveSettingsToIni(ini.c_str());
}

const char* TwoStates_GetSavedJoinAddress()
{
    return g_savedJoinAddress;
}

void TwoStates_SetSavedJoinAddress(const char* addr)
{
    if (!addr || !addr[0]) return;
    strncpy_s(g_savedJoinAddress, addr, _TRUNCATE);
}

bool TwoStates_GetSendClientLogs()
{
    return g_sendClientLogs;
}

void TwoStates_SetSendClientLogs(bool send)
{
    g_sendClientLogs = send;
}

// ---------------------------------------------------------------------------
// Separate Singleplayer vs Co-op Save Banks (media_soviet\save <-> save_sp / save_coop)
// ---------------------------------------------------------------------------

#define COOP_BANK_MARKER_NAME ".republicmp_coop_bank"
#define COOP_SAVE_META_NAME   "republicmp_coop.ini"

static bool BuildFullSaveDir(const char* saveRel, char* outFullDir, size_t outSize)
{
    if (!saveRel || !saveRel[0] || !outFullDir || outSize == 0) return false;
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;
    char normRel[160] = {};
    strncpy_s(normRel, saveRel, _TRUNCATE);
    for (char* p = normRel; *p; ++p) {
        if (*p == '/') *p = '\\';
    }
    return _snprintf_s(outFullDir, outSize, _TRUNCATE, "%s\\media_soviet\\%s", exeDir, normRel) >= 0;
}

bool TwoStates_IsCoopBankMounted()
{
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;
    char markerPath[MAX_PATH] = {};
    _snprintf_s(markerPath, sizeof(markerPath), _TRUNCATE, "%s\\media_soviet\\save\\%s", exeDir, COOP_BANK_MARKER_NAME);
    return GetFileAttributesA(markerPath) != INVALID_FILE_ATTRIBUTES;
}

static void CleanStagingFilesInDir(const std::filesystem::path& dirPath)
{
    if (SaveSync_GetState() != SESSION_IDLE || SaveSync_IsWorldLoaded()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dirPath, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
        std::string name = entry.path().filename().string();
        if (_strnicmp(name.c_str(), "mp_syncsave_", 12) == 0 ||
            (name.size() > 15 && _stricmp(name.c_str() + name.size() - 15, ".republicmp.zip") == 0)) {
            std::error_code rmEc;
            std::filesystem::remove_all(entry.path(), rmEc);
        }
    }
}

static bool MoveOrMergeDir(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return true;
    if (!std::filesystem::exists(dst, ec)) {
        std::filesystem::rename(src, dst, ec);
        if (!ec) return true;
    }
    std::filesystem::create_directories(dst, ec);
    for (const auto& entry : std::filesystem::directory_iterator(src, ec)) {
        std::filesystem::path target = dst / entry.path().filename();
        std::error_code rmEc;
        if (std::filesystem::exists(target, rmEc)) {
            std::filesystem::remove_all(target, rmEc);
        }
        std::filesystem::rename(entry.path(), target, rmEc);
        if (rmEc) {
            std::filesystem::copy(entry.path(), target,
                                  std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                  rmEc);
            std::filesystem::remove_all(entry.path(), rmEc);
        }
    }
    std::filesystem::remove_all(src, ec);
    return true;
}

bool TwoStates_MountSaveBank(bool wantCoop)
{
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;

    std::filesystem::path mediaDir = std::filesystem::path(exeDir) / "media_soviet";
    std::filesystem::path saveDir  = mediaDir / "save";
    std::filesystem::path spDir    = mediaDir / "save_sp";
    std::filesystem::path coopDir  = mediaDir / "save_coop";
    std::filesystem::path marker   = saveDir / COOP_BANK_MARKER_NAME;

    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);

    bool curCoop = std::filesystem::exists(marker, ec);
    if (curCoop == wantCoop) {
        CleanStagingFilesInDir(saveDir);
        return true;
    }

    if (wantCoop) {
        // Switching from Singleplayer bank (currently in `save`) -> Co-op bank (`save_coop` -> `save`)
        CleanStagingFilesInDir(saveDir);
        MoveOrMergeDir(saveDir, spDir);
        if (std::filesystem::exists(coopDir, ec)) {
            MoveOrMergeDir(coopDir, saveDir);
        } else {
            std::filesystem::create_directories(saveDir, ec);
        }
        FILE* f = nullptr;
        if (fopen_s(&f, marker.string().c_str(), "wb") == 0 && f) {
            const char* txt = "REPUBLICMP_COOP_SAVE_BANK\n";
            fwrite(txt, 1, strlen(txt), f);
            fclose(f);
        }
        // Automatically migrate any pre-existing Co-op saves (folders with republicmp_coop.ini or coop_ prefix) from save_sp -> save
        if (std::filesystem::exists(spDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(spDir, ec)) {
                if (!entry.is_directory(ec)) continue;
                std::string folderName = entry.path().filename().string();
                bool isCoopFolder = (_strnicmp(folderName.c_str(), "coop_", 5) == 0) ||
                                    std::filesystem::exists(entry.path() / COOP_SAVE_META_NAME, ec);
                if (isCoopFolder) {
                    std::filesystem::path dst = saveDir / folderName;
                    if (!std::filesystem::exists(dst, ec)) {
                        std::filesystem::rename(entry.path(), dst, ec);
                    }
                }
            }
        }
        CleanStagingFilesInDir(saveDir);
        Logf("republicmp  SAVE_BANK: mounted CO-OP save bank at [%s]", saveDir.string().c_str());
    } else {
        // Switching from Co-op bank (currently in `save`) -> Singleplayer bank (`save_sp` -> `save`)
        ModSync_EndSession();
        CleanStagingFilesInDir(saveDir);
        MoveOrMergeDir(saveDir, coopDir);
        if (std::filesystem::exists(spDir, ec)) {
            MoveOrMergeDir(spDir, saveDir);
        } else {
            std::filesystem::create_directories(saveDir, ec);
        }
        if (std::filesystem::exists(marker, ec)) {
            std::filesystem::remove(marker, ec);
        }
        CleanStagingFilesInDir(saveDir);
        Logf("republicmp  SAVE_BANK: mounted SINGLEPLAYER save bank at [%s]", saveDir.string().c_str());
    }
    return true;
}

bool TwoStates_GetLatestCoopSaveName(char* outName, size_t outSize)
{
    if (!outName || outSize == 0) return false;
    outName[0] = '\0';

    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;

    std::filesystem::path mediaDir = std::filesystem::path(exeDir) / "media_soviet";
    std::filesystem::path coopBank = TwoStates_IsCoopBankMounted() ? (mediaDir / "save") : (mediaDir / "save_coop");

    std::error_code ec;
    if (!std::filesystem::exists(coopBank, ec)) return false;

    std::string bestName;
    std::filesystem::file_time_type bestTime{};
    bool found = false;

    for (const auto& entry : std::filesystem::directory_iterator(coopBank, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (_strnicmp(name.c_str(), "mp_syncsave_", 12) == 0) continue;
        std::filesystem::path headerBin = entry.path() / "header.bin";
        std::filesystem::path statsIni  = entry.path() / "stats.ini";
        if (!std::filesystem::exists(headerBin, ec) || !std::filesystem::exists(statsIni, ec)) continue;
        auto mtime = std::filesystem::last_write_time(headerBin, ec);
        if (!found || mtime > bestTime) {
            bestTime = mtime;
            bestName = name;
            found = true;
        }
    }

    if (!found) return false;
    strncpy_s(outName, outSize, bestName.c_str(), _TRUNCATE);
    return true;
}

void TwoStates_SetImportingSpSave(bool importing)
{
    g_importingSpSave = importing;
}

bool TwoStates_IsImportingSpSave()
{
    return g_importingSpSave;
}

// ---------------------------------------------------------------------------
// Per-Save Co-op Metadata (republicmp_coop.ini) & World Load/Save Hooks
// ---------------------------------------------------------------------------

static bool SafeReadWorldMoney(double* outRub, double* outUsd)
{
    __try {
        *outRub = *WorldRubles();
        *outUsd = *WorldDollars();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SafeWriteWorldMoney(double rub, double usd)
{
    __try {
        *WorldRubles()  = rub;
        *WorldDollars() = usd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool SafeReadPendingLoadInfo(BYTE* outTerrainFlag, char* outSaveRel, size_t outSaveRelSize)
{
    if (!g_exeBase) return false;
    __try {
        *outTerrainFlag = *(BYTE*)(g_exeBase + TERRAIN_FLAG_DAT_RVA);
        strncpy_s(outSaveRel, outSaveRelSize, (const char*)(g_exeBase + SAVE_NAME_DAT_RVA), _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::string ResourcePtrToToken(const BYTE* world, uintptr_t resPtr)
{
    if (!resPtr) return "I0";
    if (world) {
        uintptr_t resBegin = 0, resEnd = 0;
        if (SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET), &resBegin) &&
            SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET + 8), &resEnd) &&
            resBegin && resEnd > resBegin && resPtr >= resBegin && resPtr < resEnd &&
            ((resPtr - resBegin) % WORLD_RESOURCE_STRIDE) == 0) {
            char buf[32];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "R%llu",
                        (unsigned long long)((resPtr - resBegin) / WORLD_RESOURCE_STRIDE));
            return buf;
        }
        uintptr_t wBase = (uintptr_t)world;
        if (resPtr >= wBase && resPtr < wBase + 0x1000000ULL) {
            char buf[32];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "W%llu", (unsigned long long)(resPtr - wBase));
            return buf;
        }
        for (size_t off = WORLD_RES_PTR_SLOTS_BEGIN_OFF; off <= WORLD_RES_PTR_SLOTS_END_OFF; off += sizeof(uintptr_t)) {
            uintptr_t slotVal = 0;
            if (SafeReadPod((const uintptr_t*)(world + off), &slotVal) && slotVal == resPtr) {
                char buf[32];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "P%llu", (unsigned long long)off);
                return buf;
            }
        }
    }
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "I%llu", (unsigned long long)resPtr);
    return buf;
}

static uintptr_t ResourceTokenToPtr(const BYTE* world, const std::string& tok)
{
    if (tok.empty()) return 0;
    char prefix = tok[0];
    unsigned long long num = _strtoui64(tok.c_str() + 1, nullptr, 10);
    if (prefix == 'R' && world) {
        uintptr_t resBegin = 0, resEnd = 0;
        if (SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET), &resBegin) &&
            SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET + 8), &resEnd) &&
            resBegin && resEnd > resBegin) {
            uintptr_t ptr = resBegin + (uintptr_t)(num * WORLD_RESOURCE_STRIDE);
            if (ptr >= resBegin && ptr < resEnd) return ptr;
        }
    } else if (prefix == 'W' && world) {
        if (num < 0x1000000ULL) {
            return (uintptr_t)world + (uintptr_t)num;
        }
    } else if (prefix == 'P' && world) {
        if (num >= WORLD_RES_PTR_SLOTS_BEGIN_OFF && num <= WORLD_RES_PTR_SLOTS_END_OFF &&
            (num % sizeof(uintptr_t)) == 0) {
            uintptr_t slotVal = 0;
            if (SafeReadPod((const uintptr_t*)(world + (size_t)num), &slotVal)) {
                return slotVal;
            }
        }
    } else if (prefix == 'I' && world) {
        uintptr_t ptr = (uintptr_t)num;
        uintptr_t wBase = (uintptr_t)world;
        if (ptr >= wBase && ptr < wBase + 0x1000000ULL) return ptr;
        uintptr_t resBegin = 0, resEnd = 0;
        if (SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET), &resBegin) &&
            SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET + 8), &resEnd) &&
            resBegin && resEnd > resBegin && ptr >= resBegin && ptr < resEnd) {
            return ptr;
        }
    }
    return 0;
}

static long long FloatToCents(float v)
{
    return isfinite(v) ? (long long)llround((double)v * 100.0) : 0LL;
}

static long long FloatToMillitons(float v)
{
    return isfinite(v) ? (long long)llround((double)v * 1000.0) : 0LL;
}

static void AppendScaledFloatSlice(std::ostringstream& ss, const char* prefix, const float* vals, int count, double mult)
{
    ss << prefix;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ss << ",";
        long long scaled = isfinite(vals[i]) ? (long long)llround((double)vals[i] * mult) : 0LL;
        ss << scaled;
    }
}

static void AppendIntSlice(std::ostringstream& ss, const char* prefix, const int32_t* vals, int count)
{
    ss << prefix;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ss << ",";
        ss << vals[i];
    }
}

static std::string SerializeStateEconPeriod(const BYTE* world, const StateEconPeriodStats& st)
{
    std::ostringstream ss;
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "D:%d,%d", st.day, st.year);
    ss << buf;
    AppendScaledFloatSlice(ss, ";V:", st.scalars.vehicles, 4, 100.0);
    AppendScaledFloatSlice(ss, ";M:", st.scalars.immigrantSpend, 2, 100.0);
    AppendIntSlice(ss, ";C:", st.scalars.citizenCounts, 5);
    AppendScaledFloatSlice(ss, ";S:", st.scalars.satisfaction, 10, 1000.0);
    AppendScaledFloatSlice(ss, ";A:", st.scalars.citizenAverages, 3, 1000.0);
    AppendIntSlice(ss, ";P:", st.scalars.demographics, 4);
    AppendIntSlice(ss, ";B:", st.scalars.citizenBrackets, 9);
    AppendScaledFloatSlice(ss, ";L:", st.scalars.loans, 4, 100.0);

    bool hasTourism = false;
    for (int i = 0; i < 44; ++i) {
        long long cents = FloatToCents(st.scalars.tourism[i]);
        if (cents != 0) {
            ss << (hasTourism ? "," : ";T:");
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d:%lld", i, cents);
            ss << buf;
            hasTourism = true;
        }
    }

    bool hasExtra = false;
    for (int i = 0; i < 21; ++i) {
        if (st.scalars.extraCounts[i] != 0) {
            ss << (hasExtra ? "," : ";X:");
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d:%d", i, st.scalars.extraCounts[i]);
            ss << buf;
            hasExtra = true;
        }
    }

    for (int v = 0; v < STAT_RES_VEC_COUNT; ++v) {
        const std::vector<SovietStatResEntry>& vec = st.resVecs[v];
        if (vec.empty()) continue;
        ss << ";R" << v << ":";
        for (size_t i = 0; i < vec.size(); ++i) {
            if (i > 0) ss << ",";
            std::string rTok = ResourcePtrToToken(world, vec[i].resourcePtr);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s:%lld:%lld",
                        rTok.c_str(), FloatToMillitons(vec[i].amountTons), FloatToCents(vec[i].moneyValue));
            ss << buf;
        }
    }
    return ss.str();
}

static void ParseScaledFloatSlice(const std::string& body, float* outFloats, int maxCount, double divisor)
{
    size_t pos = 0;
    for (int i = 0; i < maxCount && pos < body.size(); ++i) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        long long scaled = _strtoi64(body.substr(pos, comma - pos).c_str(), nullptr, 10);
        outFloats[i] = (float)((double)scaled / divisor);
        pos = comma + 1;
    }
}

static void ParseIntSlice(const std::string& body, int32_t* outInts, int maxCount)
{
    size_t pos = 0;
    for (int i = 0; i < maxCount && pos < body.size(); ++i) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        outInts[i] = (int32_t)atoi(body.substr(pos, comma - pos).c_str());
        pos = comma + 1;
    }
}

static void ParseTourismTokenBody(const std::string& body, float outTourism[44])
{
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        std::string item = body.substr(pos, comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            int idx = atoi(item.substr(0, colon).c_str());
            long long cents = _strtoi64(item.substr(colon + 1).c_str(), nullptr, 10);
            if (idx >= 0 && idx < 44) {
                outTourism[idx] = (float)((double)cents / 100.0);
            }
        }
        pos = comma + 1;
    }
}

static void ParseExtraCountsTokenBody(const std::string& body, int32_t outExtra[21])
{
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        std::string item = body.substr(pos, comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            int idx = atoi(item.substr(0, colon).c_str());
            int32_t val = (int32_t)atoi(item.substr(colon + 1).c_str());
            if (idx >= 0 && idx < 21) {
                outExtra[idx] = val;
            }
        }
        pos = comma + 1;
    }
}

static void ParseResVecTokenBody(const BYTE* world, const std::string& body, std::vector<SovietStatResEntry>& outVec)
{
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        std::string item = body.substr(pos, comma - pos);
        size_t c1 = item.find(':');
        size_t c2 = (c1 != std::string::npos) ? item.find(':', c1 + 1) : std::string::npos;
        if (c1 != std::string::npos && c2 != std::string::npos) {
            uintptr_t resPtr = ResourceTokenToPtr(world, item.substr(0, c1));
            long long mtons = _strtoi64(item.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
            long long cents = _strtoi64(item.substr(c2 + 1).c_str(), nullptr, 10);
            if (resPtr != 0) {
                SovietStatResEntry e = { resPtr, (float)((double)mtons / 1000.0), (float)((double)cents / 100.0) };
                outVec.push_back(e);
            }
        }
        pos = comma + 1;
    }
}

static void ApplyEconPeriodSegment(const BYTE* world, const std::string& tag, const std::string& body, StateEconPeriodStats* out)
{
    if (tag == "D")      sscanf_s(body.c_str(), "%d,%d", &out->day, &out->year);
    else if (tag == "V") ParseScaledFloatSlice(body, out->scalars.vehicles, 4, 100.0);
    else if (tag == "M") ParseScaledFloatSlice(body, out->scalars.immigrantSpend, 2, 100.0);
    else if (tag == "C") ParseIntSlice(body, out->scalars.citizenCounts, 5);
    else if (tag == "S") ParseScaledFloatSlice(body, out->scalars.satisfaction, 10, 1000.0);
    else if (tag == "A") ParseScaledFloatSlice(body, out->scalars.citizenAverages, 3, 1000.0);
    else if (tag == "P") ParseIntSlice(body, out->scalars.demographics, 4);
    else if (tag == "B") ParseIntSlice(body, out->scalars.citizenBrackets, 9);
    else if (tag == "L") ParseScaledFloatSlice(body, out->scalars.loans, 4, 100.0);
    else if (tag == "T") ParseTourismTokenBody(body, out->scalars.tourism);
    else if (tag == "X") ParseExtraCountsTokenBody(body, out->scalars.extraCounts);
    else if (tag.size() >= 2 && tag[0] == 'R') {
        int vIdx = atoi(tag.c_str() + 1);
        if (vIdx >= 0 && vIdx < STAT_RES_VEC_COUNT) {
            ParseResVecTokenBody(world, body, out->resVecs[vIdx]);
        }
    }
}

static void DeserializeStateEconPeriod(const BYTE* world, const std::string& str, StateEconPeriodStats* out)
{
    if (!out) return;
    ClearStateEconPeriod(out, 0, 0);
    size_t pos = 0;
    while (pos < str.size()) {
        size_t semi = str.find(';', pos);
        if (semi == std::string::npos) semi = str.size();
        std::string seg = str.substr(pos, semi - pos);
        pos = semi + 1;
        size_t colon = seg.find(':');
        if (colon != std::string::npos) {
            ApplyEconPeriodSegment(world, seg.substr(0, colon), seg.substr(colon + 1), out);
        }
    }
}

static bool ParseCoopMetaEconKey(const BYTE* world, const std::string& key, const std::string& val, bool haveEconCur[2])
{
    if (key == "econ_cur1") {
        DeserializeStateEconPeriod(world, val, &g_stateCurEcon[0]);
        haveEconCur[0] = true;
        return true;
    }
    if (key == "econ_cur2") {
        DeserializeStateEconPeriod(world, val, &g_stateCurEcon[1]);
        haveEconCur[1] = true;
        return true;
    }
    if (key == "econ_hist_count") {
        int cnt = atoi(val.c_str());
        if (cnt > 0 && cnt <= 4096) {
            g_stateHistEcon[0].resize((size_t)cnt);
            g_stateHistEcon[1].resize((size_t)cnt);
        }
        return true;
    }
    if (_strnicmp(key.c_str(), "econ_hist1_", 11) == 0) {
        int idx = atoi(key.c_str() + 11);
        if (idx >= 0 && (size_t)idx < g_stateHistEcon[0].size()) {
            DeserializeStateEconPeriod(world, val, &g_stateHistEcon[0][(size_t)idx]);
        }
        return true;
    }
    if (_strnicmp(key.c_str(), "econ_hist2_", 11) == 0) {
        int idx = atoi(key.c_str() + 11);
        if (idx >= 0 && (size_t)idx < g_stateHistEcon[1].size()) {
            DeserializeStateEconPeriod(world, val, &g_stateHistEcon[1][(size_t)idx]);
        }
        return true;
    }
    return false;
}

static void WriteCoopSaveMetaInternal(const char* fullSaveDir)
{
    if (!fullSaveDir || !fullSaveDir[0]) return;
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);

    int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
    double rub0 = g_stateRub[0];
    double rub1 = g_stateRub[1];
    double usd0 = g_stateUsd[0];
    double usd1 = g_stateUsd[1];

    if (!g_treasuryInitialized) {
        double wRub = 0.0, wUsd = 0.0;
        if (SafeReadWorldMoney(&wRub, &wUsd)) {
            if (mode == COOP_MODE_TWO_STATES) {
                if (g_fullStartingCapital) {
                    rub0 = wRub; rub1 = wRub;
                    usd0 = wUsd; usd1 = wUsd;
                } else {
                    rub0 = wRub * 0.5; rub1 = wRub - rub0;
                    usd0 = wUsd * 0.5; usd1 = wUsd - usd0;
                }
            } else {
                rub0 = wRub; rub1 = 0.0;
                usd0 = wUsd; usd1 = 0.0;
            }
        }
    }

    std::ostringstream out;
    out << "[republicmp_coop]\r\n";
    out << "version=1\r\n";
    out << "coop_mode=" << mode << "\r\n";
    out << "border_half_width_m=" << (int)lroundf(g_borderHalfWidth) << "\r\n";
    out << "expand_borders=" << (g_expandBorders ? 1 : 0) << "\r\n";
    out << "state1_rub_cents=" << (long long)llround(rub0 * 100.0) << "\r\n";
    out << "state2_rub_cents=" << (long long)llround(rub1 * 100.0) << "\r\n";
    out << "state1_usd_cents=" << (long long)llround(usd0 * 100.0) << "\r\n";
    out << "state2_usd_cents=" << (long long)llround(usd1 * 100.0) << "\r\n";
    out << "net_trade1_rub_cents=" << (long long)llround(g_netTradeRub[0] * 100.0) << "\r\n";
    out << "net_trade2_rub_cents=" << (long long)llround(g_netTradeRub[1] * 100.0) << "\r\n";
    out << "net_trade1_usd_cents=" << (long long)llround(g_netTradeUsd[0] * 100.0) << "\r\n";
    out << "net_trade2_usd_cents=" << (long long)llround(g_netTradeUsd[1] * 100.0) << "\r\n";

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    out << "building_owners=";
    bool first = true;
    for (const auto& kv : g_buildingOwners) {
        if (!first) out << ",";
        out << kv.first << ":" << kv.second;
        first = false;
    }
    out << "\r\n";

    out << "vehicle_owners=";
    first = true;
    for (const auto& kv : g_vehicleOwners) {
        if (!first) out << ",";
        out << kv.first << ":" << kv.second;
        first = false;
    }
    out << "\r\n";
    LeaveCriticalSection(&g_ownerLock);

    const BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
    if (g_stateEconInitialized && world) {
        out << "econ_cur1=" << SerializeStateEconPeriod(world, g_stateCurEcon[0]) << "\r\n";
        out << "econ_cur2=" << SerializeStateEconPeriod(world, g_stateCurEcon[1]) << "\r\n";
        size_t histCount = std::min(g_stateHistEcon[0].size(), g_stateHistEcon[1].size());
        out << "econ_hist_count=" << histCount << "\r\n";
        for (size_t i = 0; i < histCount; ++i) {
            out << "econ_hist1_" << i << "=" << SerializeStateEconPeriod(world, g_stateHistEcon[0][i]) << "\r\n";
            out << "econ_hist2_" << i << "=" << SerializeStateEconPeriod(world, g_stateHistEcon[1][i]) << "\r\n";
        }
    }

    std::string data = out.str();
    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "wb") == 0 && f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        Logf("republicmp  COOP_META: saved [%s] (mode=%d, rub=[%.0f, %.0f], usd=[%.0f, %.0f], blds=%zu, vehs=%zu)",
             metaPath, mode, rub0, rub1, usd0, usd1, g_buildingOwners.size(), g_vehicleOwners.size());
    }
}

static void ParseOwnersList(const std::string& val, std::unordered_map<int, int>& outMap, int minId, bool* outHadPoisonedZero)
{
    outMap.clear();
    if (outHadPoisonedZero) *outHadPoisonedZero = false;
    size_t pos = 0;
    while (pos < val.size()) {
        size_t comma = val.find(',', pos);
        if (comma == std::string::npos) comma = val.size();
        std::string token = val.substr(pos, comma - pos);
        size_t colon = token.find(':');
        if (colon != std::string::npos) {
            int id = atoi(token.substr(0, colon).c_str());
            int owner = atoi(token.substr(colon + 1).c_str());
            if (owner == 1 || owner == 2) {
                if (id >= minId) {
                    outMap[id] = owner;
                } else if (outHadPoisonedZero && id == 0) {
                    *outHadPoisonedZero = true;
                }
            }
        }
        pos = comma + 1;
    }
}

static void MergePeriodTradeResVecsIntoState(StateEconPeriodStats* dst, StateEconPeriodStats* src)
{
    if (!dst || !src) return;
    for (int v = 0; v < 8; ++v) {
        const std::vector<SovietStatResEntry>& sv = src->resVecs[v];
        for (size_t i = 0; i < sv.size(); ++i) {
            bool isNeg = (sv[i].amountTons < 0.0f);
            AddEntryDeltaToStateResVec(dst->resVecs[v], sv[i].resourcePtr, isNeg,
                                       sv[i].amountTons, sv[i].moneyValue);
        }
        src->resVecs[v].clear();
    }
}

static void ClearPeriodTradeResVecs(StateEconPeriodStats* st)
{
    if (!st) return;
    for (int v = 0; v < 8; ++v) {
        st->resVecs[v].clear();
    }
}

static void HealLoadedCoopMetaTradeStats(bool hadPoisonedZero, const long long rubCents[2],
                                         const long long tradeRubCents[2], const long long tradeUsdCents[2])
{
    if (!g_stateEconInitialized) return;
    for (int s = 0; s < 2; ++s) {
        int other = 1 - s;
        bool sZeroTrade = (tradeRubCents[s] == 0 && tradeUsdCents[s] == 0);
        bool otherHasTrade = (tradeRubCents[other] != 0 || tradeUsdCents[other] != 0);
        if (sZeroTrade && otherHasTrade) {
            MergePeriodTradeResVecsIntoState(&g_stateCurEcon[other], &g_stateCurEcon[s]);
            size_t n = std::min(g_stateHistEcon[s].size(), g_stateHistEcon[other].size());
            for (size_t i = 0; i < n; ++i) {
                MergePeriodTradeResVecsIntoState(&g_stateHistEcon[other][i], &g_stateHistEcon[s][i]);
            }
            return;
        }
    }
    if (tradeRubCents[0] == 0 && tradeRubCents[1] == 0 && tradeUsdCents[0] == 0 && tradeUsdCents[1] == 0 &&
        (hadPoisonedZero || (rubCents[0] == 1000000000LL && rubCents[1] == 1000000000LL))) {
        for (int s = 0; s < 2; ++s) {
            ClearPeriodTradeResVecs(&g_stateCurEcon[s]);
            for (size_t i = 0; i < g_stateHistEcon[s].size(); ++i) {
                ClearPeriodTradeResVecs(&g_stateHistEcon[s][i]);
            }
        }
    }
}

static bool ReadCoopSaveMetaInternal(const char* fullSaveDir, bool updateBudgetsAndMode)
{
    if (!fullSaveDir || !fullSaveDir[0]) return false;
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);

    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    int mode = (int)g_chosenMode;
    int bw = (int)lroundf(g_borderHalfWidth);
    int expB = g_expandBorders ? 1 : 0;
    long long rubCents[2] = { 0, 0 };
    long long usdCents[2] = { 0, 0 };
    long long tradeRubCents[2] = { 0, 0 };
    long long tradeUsdCents[2] = { 0, 0 };
    bool haveRub = false, haveUsd = false;
    bool haveEconCur[2] = { false, false };
    bool hadPoisonedZero = false;
    const BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;

    ClearStateEconPeriod(&g_stateCurEcon[0], 0, 0);
    ClearStateEconPeriod(&g_stateCurEcon[1], 0, 0);
    g_stateHistEcon[0].clear();
    g_stateHistEcon[1].clear();

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_buildingOwners.clear();
    g_vehicleOwners.clear();

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '[' || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "coop_mode") mode = atoi(val.c_str());
        else if (key == "border_half_width_m") bw = atoi(val.c_str());
        else if (key == "expand_borders") expB = atoi(val.c_str());
        else if (key == "state1_rub_cents") { rubCents[0] = _strtoi64(val.c_str(), nullptr, 10); haveRub = true; }
        else if (key == "state2_rub_cents") { rubCents[1] = _strtoi64(val.c_str(), nullptr, 10); haveRub = true; }
        else if (key == "state1_usd_cents") { usdCents[0] = _strtoi64(val.c_str(), nullptr, 10); haveUsd = true; }
        else if (key == "state2_usd_cents") { usdCents[1] = _strtoi64(val.c_str(), nullptr, 10); haveUsd = true; }
        else if (key == "net_trade1_rub_cents") tradeRubCents[0] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade2_rub_cents") tradeRubCents[1] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade1_usd_cents") tradeUsdCents[0] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade2_usd_cents") tradeUsdCents[1] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "building_owners") ParseOwnersList(val, g_buildingOwners, 1, &hadPoisonedZero);
        else if (key == "vehicle_owners")  ParseOwnersList(val, g_vehicleOwners, 0, nullptr);
        else ParseCoopMetaEconKey(world, key, val, haveEconCur);
    }
    size_t bldCount = g_buildingOwners.size();
    size_t vehCount = g_vehicleOwners.size();
    LeaveCriticalSection(&g_ownerLock);

    g_stateEconInitialized = (haveEconCur[0] && haveEconCur[1]);
    HealLoadedCoopMetaTradeStats(hadPoisonedZero, rubCents, tradeRubCents, tradeUsdCents);
    if (g_stateEconInitialized && world) {
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
    }

    g_netTradeRub[0] = (double)tradeRubCents[0] / 100.0;
    g_netTradeRub[1] = (double)tradeRubCents[1] / 100.0;
    g_netTradeUsd[0] = (double)tradeUsdCents[0] / 100.0;
    g_netTradeUsd[1] = (double)tradeUsdCents[1] / 100.0;

    if (updateBudgetsAndMode) {
        g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
        g_sessionMode = g_chosenMode;
        if (bw >= 50 && bw <= 2000) g_borderHalfWidth = (float)bw;
        g_expandBorders = (expB != 0);
        if (haveRub) {
            g_stateRub[0] = (double)rubCents[0] / 100.0;
            g_stateRub[1] = (double)rubCents[1] / 100.0;
        }
        if (haveUsd) {
            g_stateUsd[0] = (double)usdCents[0] / 100.0;
            g_stateUsd[1] = (double)usdCents[1] / 100.0;
        }
        g_treasuryInitialized = (g_chosenMode == COOP_MODE_TWO_STATES && (haveRub || haveUsd));
        g_loadedFromCoopSave = true;

        if (g_chosenMode == COOP_MODE_TWO_STATES && (haveRub || haveUsd)) {
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
                SafeWriteWorldMoney(100000000.0, 100000000.0);
            } else {
                SafeWriteWorldMoney(g_stateRub[0] + g_stateRub[1], g_stateUsd[0] + g_stateUsd[1]);
            }
        }
    }

    Logf("republicmp  COOP_META: loaded [%s] (updateBudgets=%d, mode=%d, rub=[%.0f, %.0f], usd=[%.0f, %.0f], blds=%zu, vehs=%zu)",
         metaPath, updateBudgetsAndMode ? 1 : 0, g_inSession ? (int)g_sessionMode : (int)g_chosenMode,
         g_stateRub[0], g_stateRub[1], g_stateUsd[0], g_stateUsd[1], bldCount, vehCount);
    return true;
}

bool TwoStates_WriteSaveMetaToDir(const char* fullSaveDir)
{
    if (!fullSaveDir || !fullSaveDir[0]) return false;
    WriteCoopSaveMetaInternal(fullSaveDir);
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);
    return GetFileAttributesA(metaPath) != INVALID_FILE_ATTRIBUTES;
}

bool TwoStates_ReadSaveMetaFromDir(const char* fullSaveDir, bool updateBudgetsAndMode)
{
    return ReadCoopSaveMetaInternal(fullSaveDir, updateBudgetsAndMode);
}

void TwoStates_BeforeAnyWorldLoad()
{
    if (!g_exeBase) return;
    if (g_beforeLoadPrepared) return;
    g_beforeLoadPrepared = true;
    TwoStates_RestoreUiMoneySwap();
    ResetStateHudPopCache();
    g_stateEconInitialized = false;
    ClearStateEconPeriod(&g_stateCurEcon[0], 0, 0);
    ClearStateEconPeriod(&g_stateCurEcon[1], 0, 0);
    g_stateHistEcon[0].clear();
    g_stateHistEcon[1].clear();

    bool isSessionLoad = Determinism_WorldLoading() || SaveSync_GetState() != SESSION_IDLE;
    bool wantCoopHost = (NativeMenu_IsHostOnNextLoad() || g_importingSpSave) && !isSessionLoad;

    BYTE terrainFlag = 0;
    char saveRel[128] = {};
    if (!SafeReadPendingLoadInfo(&terrainFlag, saveRel, sizeof(saveRel))) {
        return;
    }

    char fullDir[MAX_PATH] = {};
    bool isSaveRel = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
    bool hasFullDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullDir, sizeof(fullDir)));

    if (!wantCoopHost) {
        if (isSessionLoad && hasFullDir && g_expandBorders) {
            TwoStates_PatchSaveScriptIni(fullDir, 0.0f);
        }
        ModSync_BeforeWorldLoad(false, nullptr);
        return;
    }

    if (!TwoStates_IsCoopBankMounted()) {
        // User is importing a Singleplayer save into Co-op!
        bool isSaveFolder = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
        if (terrainFlag == 0 && isSaveFolder) {
            char spFullDir[MAX_PATH] = {};
            char exeDir[MAX_PATH] = {};
            if (BuildFullSaveDir(saveRel, spFullDir, sizeof(spFullDir)) && GetExeDir(exeDir, sizeof(exeDir))) {
                std::error_code ec;
                std::filesystem::path srcPath(spFullDir);
                std::string folderName = srcPath.filename().string();
                std::filesystem::path coopCopy = std::filesystem::path(exeDir) / "media_soviet" / "save_coop" / folderName;
                if (std::filesystem::exists(srcPath, ec) && std::filesystem::is_directory(srcPath, ec)) {
                    std::filesystem::create_directories(coopCopy.parent_path(), ec);
                    if (std::filesystem::exists(coopCopy, ec)) {
                        std::filesystem::remove_all(coopCopy, ec);
                    }
                    std::filesystem::copy(srcPath, coopCopy,
                                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                          ec);
                    // Remove any old republicmp_coop.ini on a fresh SP import so menu capital settings apply cleanly
                    std::filesystem::remove(coopCopy / COOP_SAVE_META_NAME, ec);
                    Logf("republicmp  SAVE_BANK: imported SP save [%s] -> [%s]",
                         srcPath.string().c_str(), coopCopy.string().c_str());
                } else {
                    // Check if it was a .zip save in Singleplayer
                    std::filesystem::path srcZip( std::string(spFullDir) + ".zip" );
                    std::filesystem::path coopZip = std::filesystem::path(exeDir) / "media_soviet" / "save_coop" / (folderName + ".zip");
                    if (std::filesystem::exists(srcZip, ec)) {
                        std::filesystem::create_directories(coopZip.parent_path(), ec);
                        std::filesystem::copy_file(srcZip, coopZip, std::filesystem::copy_options::overwrite_existing, ec);
                    }
                }
            }
        }
        TwoStates_MountSaveBank(true);
    }

    g_importingSpSave = false;

    hasFullDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullDir, sizeof(fullDir)));
    if (hasFullDir && g_expandBorders) {
        TwoStates_PatchSaveScriptIni(fullDir, 0.0f);
    }

    ModSync_BeforeWorldLoad(true, hasFullDir ? fullDir : nullptr);
}

void TwoStates_OnAnyWorldLoaded(bool isLockstepSessionLoad)
{
    g_beforeLoadPrepared = false;
    if (!g_exeBase) return;
    BYTE terrainFlag = 0;
    char saveRel[128] = {};
    if (!SafeReadPendingLoadInfo(&terrainFlag, saveRel, sizeof(saveRel))) {
        return;
    }

    char fullSaveDir[MAX_PATH] = {};
    bool isSaveRel = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
    bool hasSaveDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir)));

    if (isLockstepSessionLoad) {
        if (hasSaveDir) {
            ReadCoopSaveMetaInternal(fullSaveDir, false);
        }
        TwoStates_OnWorldLoaded();
        return;
    }

    const bool inCoop = g_inSession || NativeMenu_IsHostOnNextLoad() || SaveSync_IsBusy();
    if (inCoop && hasSaveDir && ReadCoopSaveMetaInternal(fullSaveDir, true)) {
        g_haveLobbyBaseMoney = false;
        if (g_chosenMode == COOP_MODE_TWO_STATES) {
            EnsureStateEconInitialized(g_exeBase + WORLD_RVA);
            char msg[240];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        Tr("Загружен кооп-сейв «2 государства» (Запад: %.0f РУБ / %.0f $, Восток: %.0f РУБ / %.0f $)",
                           "Loaded 'Two States' Co-op save (West: %.0f RUB / %.0f $, East: %.0f RUB / %.0f $)"),
                        g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
            TwoStates_AddToast(msg, 1);
        } else {
            TwoStates_AddToast(Tr("Загружен кооп-сейв «Одно государство»",
                                  "Loaded 'One State' Co-op save"), 1);
        }
    } else {
        g_loadedFromCoopSave = false;
        g_treasuryInitialized = false;
        g_haveLobbyBaseMoney = false;
        DiagLog_BeginSafeSeh();
        __try {
            g_lobbyBaseWorldRub = *WorldRubles();
            g_lobbyBaseWorldUsd = *WorldDollars();
            g_haveLobbyBaseMoney = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_haveLobbyBaseMoney = false;
        }
        DiagLog_EndSafeSeh();
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        g_buildingOwners.clear();
        g_vehicleOwners.clear();
        LeaveCriticalSection(&g_ownerLock);
        g_netTradeRub[0] = g_netTradeRub[1] = 0.0;
        g_netTradeUsd[0] = g_netTradeUsd[1] = 0.0;
        ResetStateHudPopCache();
        g_stateEconInitialized = false;
        ClearStateEconPeriod(&g_stateCurEcon[0], 0, 0);
        ClearStateEconPeriod(&g_stateCurEcon[1], 0, 0);
        g_stateHistEcon[0].clear();
        g_stateHistEcon[1].clear();
    }
}

bool TwoStates_TriggerQuickCoopSave()
{
    if (!SaveSync_IsWorldLoaded()) return false;
    TwoStates_MountSaveBank(true);

    SYSTEMTIME st;
    GetLocalTime(&st);
    int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
    char saveName[96] = {};
    _snprintf_s(saveName, sizeof(saveName), _TRUNCATE,
                "coop_%s_%04d-%02d-%02d_%02d-%02d-%02d",
                (mode == COOP_MODE_TWO_STATES) ? "2states" : "1state",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char fullSaveDir[MAX_PATH] = {};
    char saveRel[128] = {};
    _snprintf_s(saveRel, sizeof(saveRel), _TRUNCATE, "save\\%s", saveName);
    if (BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir))) {
        CreateDirectoryA(fullSaveDir, nullptr);
    }

    return TriggerSaveGame(saveName);
}

