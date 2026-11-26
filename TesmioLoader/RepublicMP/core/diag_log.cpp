// SPDX-License-Identifier: GPL-3.0-only
// See diag_log.h.
#include "diag_log.h"
#include "safe_mem.h"
#include "version.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../session/mod_sync.h"
#include "../session/updater.h"
#include "../determinism/determinism.h"
#include "../determinism/held_frame.h"
#include "../determinism/state_hash.h"
#include "../lockstep/lockstep_net.h"
#include "../transport/lan_discovery.h"
#include "../transport/steam_p2p.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../world_sync/vehicles/vehicle_soft_sync.h"
#include "../third_party/miniz/miniz.h"
#include "../ui/lang.h"
#include "../game/window_focus.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

static INIT_ONCE g_diagLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_diagLock;
static HANDLE g_hDiagFile = INVALID_HANDLE_VALUE;
static char g_diagFilePath[MAX_PATH] = "";
static char g_gameDir[MAX_PATH] = "";
static const TsmHost* g_origHost = nullptr;
static TsmHost g_wrappedHost = {};
static PVOID g_vehHandle = nullptr;
static thread_local int t_safeSehDepth = 0;

void DiagLog_BeginSafeSeh(void)
{
    ++t_safeSehDepth;
}

void DiagLog_EndSafeSeh(void)
{
    if (t_safeSehDepth > 0) --t_safeSehDepth;
}

int DiagLog_GetSafeSehDepthForTest(void)
{
    return t_safeSehDepth;
}

extern "C" __declspec(dllexport) int DiagLog_IsInSafeSeh(void)
{
    return t_safeSehDepth > 0 ? 1 : 0;
}

static void EnsureDiagLock()
{
    TsmEnsureCriticalSection(&g_diagLockOnce, &g_diagLock);
}

struct DiagPerfBucketCounters {
    volatile LONG64 totalTicks;
    volatile LONG64 count;
    volatile LONG64 maxTicks;
};

static DiagPerfBucketCounters g_perfBuckets[DIAG_PERF_BUCKET_COUNT] = {};
static volatile LONG64 g_perfMallocCalls = 0;
static volatile LONG64 g_perfMallocBytes = 0;
static LARGE_INTEGER g_perfFreq = {};

static double GetPerfFreqHz()
{
    if (g_perfFreq.QuadPart <= 0) {
        QueryPerformanceFrequency(&g_perfFreq);
    }
    return g_perfFreq.QuadPart > 0 ? (double)g_perfFreq.QuadPart : 10000000.0;
}

static void AtomicMax64(volatile LONG64* target, LONG64 val)
{
    LONG64 cur = *target;
    while (val > cur) {
        LONG64 prev = InterlockedCompareExchange64(target, val, cur);
        if (prev == cur) break;
        cur = prev;
    }
}

int64_t DiagPerf_Now(void)
{
    LARGE_INTEGER li = {};
    QueryPerformanceCounter(&li);
    return (int64_t)li.QuadPart;
}

void DiagPerf_RecordBatch(DiagPerfBucket bucket, int64_t count, int64_t totalQpcTicks, int64_t maxQpcTicks)
{
    const int idx = (int)bucket;
    if (idx < 0 || idx >= DIAG_PERF_BUCKET_COUNT || count <= 0) return;
    if (totalQpcTicks < 0) totalQpcTicks = 0;
    if (maxQpcTicks < 0) maxQpcTicks = 0;
    DiagPerfBucketCounters& b = g_perfBuckets[idx];
    InterlockedAdd64(&b.totalTicks, (LONG64)totalQpcTicks);
    InterlockedAdd64(&b.count, (LONG64)count);
    AtomicMax64(&b.maxTicks, (LONG64)maxQpcTicks);
}

void DiagPerf_Record(DiagPerfBucket bucket, int64_t qpcDelta)
{
    DiagPerf_RecordBatch(bucket, 1, qpcDelta, qpcDelta);
}

void DiagPerf_RecordVirtualQuery(int64_t qpcDelta)
{
    DiagPerf_Record(DIAG_PERF_VIRTUAL_QUERY, qpcDelta);
}

void DiagPerf_RecordMallocBatch(int64_t calls, int64_t bytes)
{
    if (calls > 0) InterlockedAdd64(&g_perfMallocCalls, (LONG64)calls);
    if (bytes > 0) InterlockedAdd64(&g_perfMallocBytes, (LONG64)bytes);
}

void DiagPerf_RecordMalloc(size_t bytes)
{
    DiagPerf_RecordMallocBatch(1, (int64_t)bytes);
}

double DiagLog_QueryTimerResolutionMs(void)
{
    typedef LONG (NTAPI* NtQueryTimerResolutionFn)(ULONG*, ULONG*, ULONG*);
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) return 0.0;
    auto pQuery = reinterpret_cast<NtQueryTimerResolutionFn>(
        reinterpret_cast<void*>(GetProcAddress(hNtdll, "NtQueryTimerResolution")));
    if (!pQuery) return 0.0;
    ULONG minRes = 0, maxRes = 0, curRes = 0;
    if (pQuery(&minRes, &maxRes, &curRes) != 0) return 0.0;
    return (double)curRes / 10000.0;
}

void DiagPerf_TakeSnapshot(DiagPerfSnapshot* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const double freq = GetPerfFreqHz();
    for (int i = 0; i < DIAG_PERF_BUCKET_COUNT; ++i) {
        LONG64 cnt = InterlockedExchange64(&g_perfBuckets[i].count, 0);
        LONG64 tot = InterlockedExchange64(&g_perfBuckets[i].totalTicks, 0);
        LONG64 mx  = InterlockedExchange64(&g_perfBuckets[i].maxTicks, 0);
        out->buckets[i].count = (int64_t)cnt;
        out->buckets[i].totalMs = (double)tot * 1000.0 / freq;
        out->buckets[i].avgMs = (cnt > 0) ? (out->buckets[i].totalMs / (double)cnt) : 0.0;
        out->buckets[i].maxMs = (double)mx * 1000.0 / freq;
    }
    out->mallocCalls = (int64_t)InterlockedExchange64(&g_perfMallocCalls, 0);
    out->mallocBytes = (int64_t)InterlockedExchange64(&g_perfMallocBytes, 0);
    out->winTimerResMs = DiagLog_QueryTimerResolutionMs();
    out->windowFocused = IsGameWindowFocused();
}

static void WriteRawBlock(const char* data, size_t len, bool flush)
{
    if (!data || len == 0) return;
    EnsureDiagLock();
    EnterCriticalSection(&g_diagLock);
    if (g_hDiagFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_hDiagFile, data, (DWORD)len, &written, nullptr);
        if (flush) FlushFileBuffers(g_hDiagFile);
    }
    LeaveCriticalSection(&g_diagLock);
}

static void FormatDiagLinePrefixBody(char* out, size_t outSize, const char* category, const char* body)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();
    _snprintf_s(out, outSize, _TRUNCATE,
                "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [TID:%05lu] [%-7s] %s\r\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)tid, category ? category : "INFO", body ? body : "");
}

static void WriteRawLine(const char* category, const char* body)
{
    char line[4600];
    FormatDiagLinePrefixBody(line, sizeof(line), category, body);
    WriteRawBlock(line, strlen(line), true);
}

static void AppendDiagLine(std::string& batch, const char* category, const char* fmt, ...)
{
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt ? fmt : "", ap);
    va_end(ap);

    char line[4600];
    FormatDiagLinePrefixBody(line, sizeof(line), category, body);
    batch.append(line);
}

void DiagLog_Printf(const char* category, const char* fmt, ...)
{
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    WriteRawLine(category, body);
}

static void HookedHostLogf(const char* fmt, ...)
{
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    WriteRawLine("PLUGIN", body);
    if (g_origHost && g_origHost->log) {
        g_origHost->log("%s", body);
    }
}

// Formats an address as "ModuleName+0xRVA (0xAddress)"
static void FormatAddressModule(uintptr_t addr, char* out, size_t outSize)
{
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hMod) && hMod) {
        char modPath[MAX_PATH] = {};
        GetModuleFileNameA(hMod, modPath, MAX_PATH);
        const char* s1 = strrchr(modPath, '\\');
        const char* s2 = strrchr(modPath, '/');
        const char* lastSep = (!s1) ? s2 : (!s2 ? s1 : (s1 > s2 ? s1 : s2));
        const char* baseName = lastSep ? (lastSep + 1) : modPath;
        uintptr_t rva = addr - (uintptr_t)hMod;
        _snprintf_s(out, outSize, _TRUNCATE, "%s+0x%llX (0x%p)",
                    baseName, (unsigned long long)rva, (void*)addr);
    } else {
        _snprintf_s(out, outSize, _TRUNCATE, "0x%p", (void*)addr);
    }
}

static const char* FramePhaseName(int phase)
{
    switch (phase) {
    case 0: return "OUTSIDE";
    case 1: return "PREPARE";
    case 2: return "STEP_EARLY";
    case 3: return "HELD_EARLY";
    case 4: return "AFTER_EARLY";
    case 5: return "LATE";
    case 6: return "AFTER_LATE";
    default: return "UNKNOWN";
    }
}

static LONG WINAPI DiagVectoredExceptionHandler(PEXCEPTION_POINTERS ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord || t_safeSehDepth > 0)
        return EXCEPTION_CONTINUE_SEARCH;

    static thread_local bool t_inVeh = false;
    if (t_inVeh)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Ignore benign / C++ / debugger / thread-naming exceptions unless fatal
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        code != 0xC0000374UL /* STATUS_HEAP_CORRUPTION */ &&
        code != 0xC0000409UL /* STATUS_STACK_BUFFER_OVERRUN */) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    struct VehGuard {
        bool& flag;
        ~VehGuard() { flag = false; }
    };
    t_inVeh = true;
    VehGuard vehGuard{ t_inVeh };

    static DWORD s_vehLogCount = 0;
    if (InterlockedIncrement(&s_vehLogCount) > 20) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char faultSym[256] = {};
    FormatAddressModule((uintptr_t)ep->ExceptionRecord->ExceptionAddress, faultSym, sizeof(faultSym));

    CONTEXT* c = ep->ContextRecord;
    DiagLog_Printf("CRASH", "=================================================================");
    DiagLog_Printf("CRASH", "EXCEPTION 0x%08lX at %s", (unsigned long)code, faultSym);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const char* op = (ep->ExceptionRecord->ExceptionInformation[0] == 0) ? "READ" :
                         (ep->ExceptionRecord->ExceptionInformation[0] == 1) ? "WRITE" : "EXEC";
        DiagLog_Printf("CRASH", "Access Violation: %s at address 0x%p",
                       op, (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    DeterminismDiagInfo dinfo = {};
    Determinism_GetDiagInfo(&dinfo);
    HeldFrameDiagStats hf = {};
    HeldFrame_GetDiagStats(&hf);
    VehicleSoftSyncDiagStats vss = {};
    VehicleSoftSync_GetDiagStats(&vss);
    DiagLog_Printf("CRASH", "FrameContext: SimTick=%llu | Beats=%llu | Phase=%d(%s) | StepFrame=%d | TickDebt=%.3f | SubTickAlpha=%.3f",
                   Determinism_StepCount(), dinfo.frameBeats, dinfo.framePhase, FramePhaseName(dinfo.framePhase),
                   (int)dinfo.isStepFrame, dinfo.tickDebt, dinfo.subTickAlpha);
    DiagLog_Printf("CRASH", "HeldFrameCtx: InHeldDraw=%d | Drawn=%llu | TrackedVeh=%u | LastTargets=%u | LastInterp=%u | LastAlpha=%.3f | Fallbacks=%llu | RestoreRefusals=%llu",
                   (int)hf.inHeldDraw, hf.heldFramesDrawn, hf.trackedVehicles, hf.lastTargetsCount,
                   hf.lastInterpolatedCount, hf.lastAlpha, hf.guardFallbacks, hf.restoreRefusals);
    DiagLog_Printf("CRASH", "SoftSyncCtx : Sent=%llu | Applied=%llu | ImmSweeps=%llu | VehSynced=%llu | Blended=%llu | Snapped=%llu | CargoFixed=%llu | BldStorageFixed=%llu | MaxDrift=%.2fm/%.2fm",
                   vss.batchesSent, vss.batchesApplied, vss.immediateSweeps, vss.vehiclesSynced,
                   vss.posBlended, vss.posSnapped, vss.cargoUpdated, vss.bldStorageUpdated, vss.lastMaxDriftM, vss.allTimeMaxDriftM);
    DiagLog_Printf("CRASH", "RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
                   c->Rax, c->Rbx, c->Rcx, c->Rdx);
    DiagLog_Printf("CRASH", "RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX",
                   c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    DiagLog_Printf("CRASH", "R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX",
                   c->R8, c->R9, c->R10, c->R11);
    DiagLog_Printf("CRASH", "R12=%016llX R13=%016llX R14=%016llX R15=%016llX",
                   c->R12, c->R13, c->R14, c->R15);

    // Walk stack pointers cleanly to find code return addresses
    static constexpr DWORD kReadableProtMask =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    uintptr_t* sp = (uintptr_t*)c->Rsp;
    int printed = 0;
    for (int i = 0; i < 64 && printed < 16; ++i) {
        const uintptr_t addr = (uintptr_t)(sp + i);
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
            (mbi.Protect & kReadableProtMask) == 0) {
            break;
        }
        const uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
        const uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (addr < regionStart || addr + sizeof(uintptr_t) > regionEnd) break;
        uintptr_t val = 0;
        if (!SafeMemcpyPod(&val, (const void*)addr, sizeof(val))) break;
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)val, &hMod) && hMod) {
            char sym[256] = {};
            FormatAddressModule(val, sym, sizeof(sym));
            DiagLog_Printf("CRASH", "  Stack[%02d] RSP+0x%03X -> %s", printed, i * 8, sym);
            printed++;
        }
    }
    DiagLog_Printf("CRASH", "=================================================================");
    if (g_gameDir[0] && g_diagFilePath[0]) {
        if (g_hDiagFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_hDiagFile);
        }
        char crashLogPath[MAX_PATH] = {};
        _snprintf_s(crashLogPath, sizeof(crashLogPath), _TRUNCATE, "%s\\RepublicMP_LastCrash.log", g_gameDir);
        CopyFileA(g_diagFilePath, crashLogPath, FALSE);
    }
    const bool unsafeForHeapOrNet = (code == 0xC0000374UL ||
                                     code == EXCEPTION_STACK_OVERFLOW ||
                                     code == 0xC0000409UL);
    if (!unsafeForHeapOrNet && !SaveSync_IsHostRole() && LockstepNet_IsConnected() && TwoStates_GetSendClientLogs()) {
        LockstepNet_SendClientLogs("CRASH");
        for (int i = 0; i < 16; ++i) {
            SteamP2P_FrameTick();
            Sleep(25);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

const TsmHost* DiagLog_InitAndWrapHost(const TsmHost* origHost)
{
    EnsureDiagLock();
    g_origHost = origHost;
    memset(&g_wrappedHost, 0, sizeof(g_wrappedHost));
    if (origHost) {
        g_wrappedHost = *origHost;
    }
    g_wrappedHost.log = &HookedHostLogf;

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    strncpy_s(g_gameDir, exePath, _TRUNCATE);
    if (char* s = strrchr(g_gameDir, '\\')) *s = '\0';

    _snprintf_s(g_diagFilePath, sizeof(g_diagFilePath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.log", g_gameDir);
    char prevPath[MAX_PATH] = {};
    _snprintf_s(prevPath, sizeof(prevPath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.prev.log", g_gameDir);

    // If dinput8.dll did not already rotate the log in this process, rotate it only if it is
    // both older than 3 seconds AND larger than 4 KB (so dinput8.dll's 220-byte [BOOT] stub
    // written a few seconds earlier never overwrites the previous session's .prev.log).
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExA(g_diagFilePath, GetFileExInfoStandard, &fad)) {
        FILETIME nowFt = {};
        GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER uNow, uFile;
        uNow.LowPart = nowFt.dwLowDateTime; uNow.HighPart = nowFt.dwHighDateTime;
        uFile.LowPart = fad.ftLastWriteTime.dwLowDateTime; uFile.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        if (uNow.QuadPart > uFile.QuadPart && (uNow.QuadPart - uFile.QuadPart) > 30000000ULL &&
            (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 4096)) {
            CopyFileA(g_diagFilePath, prevPath, FALSE);
            DeleteFileA(g_diagFilePath);
        }
    }

    g_hDiagFile = CreateFileA(g_diagFilePath, FILE_APPEND_DATA | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_hDiagFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_hDiagFile, 0, nullptr, FILE_END);
    }

    if (!g_vehHandle) {
        g_vehHandle = AddVectoredExceptionHandler(1, DiagVectoredExceptionHandler);
    }

    DiagLog_Printf("INIT", "=================================================================");
    DiagLog_Printf("INIT", "RepublicMP Diagnostic Log Initialized");
    DiagLog_Printf("INIT", "Version: v%s (Protocol %d) | GitHub Repo: %s",
                   REPUBLICMP_VERSION, REPUBLICMP_PROTOCOL, REPUBLICMP_GITHUB_REPO);
    DiagLog_Printf("INIT", "Process PID: %lu | Game EXE: %s",
                   (unsigned long)GetCurrentProcessId(), exePath);
    DiagLog_Printf("INIT", "Loader baseDir: %s", origHost && origHost->baseDir ? origHost->baseDir : "(null)");
    return &g_wrappedHost;
}

static const char* SessionStateName(SessionState st)
{
    switch (st) {
    case SESSION_IDLE:           return "IDLE";
    case SESSION_HOST_LISTENING: return "HOST_LISTENING";
    case SESSION_HOST_JOINING:   return "HOST_JOINING";
    case SESSION_HOST_IN_GAME:   return "HOST_IN_GAME";
    case SESSION_CLIENT_JOINING: return "CLIENT_JOINING";
    case SESSION_CLIENT_IN_GAME: return "CLIENT_IN_GAME";
    default:                     return "UNKNOWN";
    }
}

static void LogGameConfigSummary(void)
{
    if (!g_gameDir[0]) return;
    char cfgPath[MAX_PATH] = {};
    _snprintf_s(cfgPath, sizeof(cfgPath), _TRUNCATE, "%s\\media_soviet\\config.ini", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, cfgPath, "r") != 0 || !f) {
        DiagLog_Printf("CONFIG", "config.ini not found (%s) | WinTimer=%.2fms",
                       cfgPath, DiagLog_QueryTimerResolutionMs());
        return;
    }
    int resX = -1, resY = -1, windowed = -1, vsync = -1, drawDist = -1, citizens = -1;
    char line[256] = {};
    while (fgets(line, sizeof(line), f)) {
        int val = 0;
        if (sscanf_s(line, "$INT RESOLUTIONX %d", &val) == 1) resX = val;
        else if (sscanf_s(line, "$INT RESOLUTIONY %d", &val) == 1) resY = val;
        else if (sscanf_s(line, "$INT WINDOWED %d", &val) == 1) windowed = val;
        else if (sscanf_s(line, "$INT GRAPHIC_VSYNC %d", &val) == 1) vsync = val;
        else if (sscanf_s(line, "$INT GRAPHIC_DRAWDISTANCE %d", &val) == 1) drawDist = val;
        else if (sscanf_s(line, "$INT GRAPHIC_CITIZENS %d", &val) == 1) citizens = val;
    }
    fclose(f);
    DiagLog_Printf("CONFIG", "Res=%dx%d | Windowed=%d | VSync=%d | DrawDist=%d | Citizens=%d | WinTimer=%.2fms",
                   resX, resY, windowed, vsync, drawDist, citizens, DiagLog_QueryTimerResolutionMs());
}

void DiagLog_LogStartupSummary(void)
{
    HMODULE hExe = GetModuleHandleA(nullptr);
    HMODULE hEng = GetModuleHandleA("C3DDLL64.dll");
    HMODULE hStm = GetModuleHandleA("steam_api64.dll");
    HMODULE hDin = GetModuleHandleA("dinput8.dll");
    HMODULE hLdr = GetModuleHandleA("tesmioloader.dll");
    HMODULE hRmp = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&DiagLog_LogStartupSummary, &hRmp);

    unsigned peStamp = 0;
    if (hExe) {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hExe;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hExe + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                peStamp = nt->FileHeader.TimeDateStamp;
            }
        }
    }

    char rmpPath[MAX_PATH] = {};
    if (hRmp) GetModuleFileNameA(hRmp, rmpPath, MAX_PATH);

    DiagLog_Printf("MODULES", "SOVIET64.exe=%p (PE TimeDateStamp=0x%08X) | C3DDLL64.dll=%p | steam_api64.dll=%p",
                   (void*)hExe, peStamp, (void*)hEng, (void*)hStm);
    DiagLog_Printf("MODULES", "dinput8.dll=%p | tesmioloader.dll=%p | RepublicMP.dll=%p (%s)",
                   (void*)hDin, (void*)hLdr, (void*)hRmp, rmpPath);
    DiagLog_Printf("MODULES", "RepublicMP CRC32=%08X | LocalVersion=%s",
                   SaveSync_GetLocalDllCrc(), SaveSync_GetLocalVersion());
    LogGameConfigSummary();
}

void DiagLog_FormatPerfMainLine(const DiagPerfSnapshot& ps, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    const auto& wr = ps.buckets[DIAG_PERF_WAIT_RENDER];
    const auto& ea = ps.buckets[DIAG_PERF_EARLY];
    const auto& la = ps.buckets[DIAG_PERF_LATE];
    const auto& he = ps.buckets[DIAG_PERF_HELD];
    const auto& ha = ps.buckets[DIAG_PERF_STATE_HASH];
    const auto& ts = ps.buckets[DIAG_PERF_TS_STEP];
    const auto& th = ps.buckets[DIAG_PERF_TS_ECON_HOOK];
    const double thAvgUs = (th.count > 0) ? (th.totalMs * 1000.0 / (double)th.count) : 0.0;
    _snprintf_s(out, outSize, _TRUNCATE,
                "PerfMain: waitRnd=%.2f/%.2f(n=%lld) early=%.2f/%.2f(n=%lld) late=%.2f/%.2f(n=%lld) held=%.2f/%.2f(n=%lld) hash=%.2f/%.2f(n=%lld) tsStep=%.2f/%.2f(n=%lld) tsHook=%.1fms/%.2fus(n=%lld)",
                wr.avgMs, wr.maxMs, (long long)wr.count,
                ea.avgMs, ea.maxMs, (long long)ea.count,
                la.avgMs, la.maxMs, (long long)la.count,
                he.avgMs, he.maxMs, (long long)he.count,
                ha.avgMs, ha.maxMs, (long long)ha.count,
                ts.avgMs, ts.maxMs, (long long)ts.count,
                th.totalMs, thAvgUs, (long long)th.count);
}

void DiagLog_FormatPerfRndLine(const DiagPerfSnapshot& ps, bool p2pConn, bool p2pRelay, int32_t p2pSendQ,
                               char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    const auto& ui  = ps.buckets[DIAG_PERF_UI_ROUTINE];
    const auto& tu  = ps.buckets[DIAG_PERF_TS_UI];
    const auto& ph  = ps.buckets[DIAG_PERF_PRESENT_HOOK];
    const auto& pw  = ps.buckets[DIAG_PERF_PRESENT_SWAP];
    const auto& vq  = ps.buckets[DIAG_PERF_VIRTUAL_QUERY];
    const auto& gap = ps.buckets[DIAG_PERF_INTERFRAME_GAP];
    const auto& pn  = ps.buckets[DIAG_PERF_PREPARE_NATIVE];
    const auto& pm  = ps.buckets[DIAG_PERF_PREPARE_MOD];
    const auto& ba  = ps.buckets[DIAG_PERF_BEFORE_AFTER_EARLY];
    const auto& pl  = ps.buckets[DIAG_PERF_PRIVATE_LATE];
    const auto& usw = ps.buckets[DIAG_PERF_UI_SWITCHES];
    const auto& mz  = ps.buckets[DIAG_PERF_MALLOC_ZERO];
    const double mallocMb = (double)ps.mallocBytes / (1024.0 * 1024.0);
    const double mzAvgUs = (mz.count > 0) ? (mz.totalMs * 1000.0 / (double)mz.count) : 0.0;
    _snprintf_s(out, outSize, _TRUNCATE,
                "PerfRnd: fg=%d ui=%.2f/%.2f(n=%lld) tsUi=%.2f/%.2f(n=%lld) uiSw=%.2f/%.2f(n=%lld) presHook=%.2f/%.2f(n=%lld) presSwap=%.2f/%.2f(n=%lld) | gap=%.2f/%.2f(n=%lld) prepNat=%.2f/%.2f(n=%lld) prepMod=%.2f/%.2f(n=%lld) baEarly=%.2f/%.2f(n=%lld) privLate=%.2f/%.2f(n=%lld) | VQ=%lld(%.2fms max=%.3f) Malloc=%lld(%.1fMB) mzEst=%.1fms/%.2fus(max=%.3f n=%lld) | WinTimer=%.2fms | P2P(conn=%d relay=%d sendQ=%dB)",
                ps.windowFocused ? 1 : 0,
                ui.avgMs, ui.maxMs, (long long)ui.count,
                tu.avgMs, tu.maxMs, (long long)tu.count,
                usw.avgMs, usw.maxMs, (long long)usw.count,
                ph.avgMs, ph.maxMs, (long long)ph.count,
                pw.avgMs, pw.maxMs, (long long)pw.count,
                gap.avgMs, gap.maxMs, (long long)gap.count,
                pn.avgMs, pn.maxMs, (long long)pn.count,
                pm.avgMs, pm.maxMs, (long long)pm.count,
                ba.avgMs, ba.maxMs, (long long)ba.count,
                pl.avgMs, pl.maxMs, (long long)pl.count,
                (long long)vq.count, vq.totalMs, vq.maxMs,
                (long long)ps.mallocCalls, mallocMb,
                mz.totalMs, mzAvgUs, mz.maxMs, (long long)mz.count,
                ps.winTimerResMs, (int)p2pConn, (int)p2pRelay, (int)p2pSendQ);
}

static void AppendPerfSnapshotLines(std::string& batch, const char* tag)
{
    DiagPerfSnapshot ps = {};
    DiagPerf_TakeSnapshot(&ps);
    bool p2pConn = false, p2pRelay = false;
    int32_t p2pSendQ = 0;
    SteamP2P_GetPerfStats(&p2pConn, &p2pRelay, &p2pSendQ);

    char lineBuf[2048];
    DiagLog_FormatPerfMainLine(ps, lineBuf, sizeof(lineBuf));
    AppendDiagLine(batch, tag, "%s", lineBuf);
    DiagLog_FormatPerfRndLine(ps, p2pConn, p2pRelay, p2pSendQ, lineBuf, sizeof(lineBuf));
    AppendDiagLine(batch, tag, "%s", lineBuf);
}

static void DumpLiveSnapshot(const char* tag)
{
    SessionState st = SaveSync_GetState();
    bool world = SaveSync_IsWorldLoaded();
    bool host = SaveSync_IsHostRole();
    bool lockstep = SaveSync_IsLockstepSession();
    const char* peer = SaveSync_GetPeerIp();
    const char* lastErr = SaveSync_GetLastError();

    const bool isHeartbeat = (tag && strcmp(tag, "HEARTBT") == 0);
    std::string batch;
    batch.reserve(4096);

    AppendDiagLine(batch, tag, "State=%s | Role=%s | WorldLoaded=%d | Lockstep=%d | Peer='%s' | LastErr='%s'",
                   SessionStateName(st), host ? "HOST" : "CLIENT", (int)world, (int)lockstep,
                   peer ? peer : "", lastErr ? lastErr : "");

    // Steam P2P & local LAN display identity
    bool stmAvail = SteamP2P_IsAvailable();
    uint64_t mySteamId = SteamP2P_GetLocalSteamId();
    std::string myName = SteamP2P_GetLocalPersonaNameCopy();
    std::string localDisplay = LanDiscovery_GetLocalDisplayName();
    AppendDiagLine(batch, tag, "SteamP2P: Available=%d | LocalSteamID=%llu ('%s') | DisplayName='%s'",
                   (int)stmAvail, (unsigned long long)mySteamId, myName.c_str(),
                   localDisplay.c_str());

    // Two States & Map Expansion status
    double westDebtRub = 0.0, westDebtUsd = 0.0, eastDebtRub = 0.0, eastDebtUsd = 0.0;
    int westLoans = 0, eastLoans = 0;
    TwoStates_GetLoanDebt(1, &westDebtRub, &westDebtUsd, &westLoans);
    TwoStates_GetLoanDebt(2, &eastDebtRub, &eastDebtUsd, &eastLoans);
    AppendDiagLine(batch, tag, "TwoStates: Mode=%d | LocalState=%d | BorderHalf=%.0fm | ExpandBorders=%d | West(RUB=%.0f, USD=%.0f, Loans=%d:%.0fR/%.0f$) | East(RUB=%.0f, USD=%.0f, Loans=%d:%.0fR/%.0f$)",
                   (int)TwoStates_GetMode(), TwoStates_LocalState(),
                   TwoStates_GetBorderHalfWidth(), (int)TwoStates_GetExpandBorders(),
                   TwoStates_GetRubles(1), TwoStates_GetDollars(1), westLoans, westDebtRub, westDebtUsd,
                   TwoStates_GetRubles(2), TwoStates_GetDollars(2), eastLoans, eastDebtRub, eastDebtUsd);

    // ModSync status
    AppendDiagLine(batch, tag, "ModSync: PackMode=%d | CatalogMods=%d | ActiveServerMods=%d",
                   (int)ModSync_GetPackMode(), ModSync_GetModCount(),
                   ModSync_GetActiveServerModCount());

    // DLC status
    uint32_t hostMask = 0, clientMask = 0, commonMask = 0;
    SaveSync_GetDlcMasks(&hostMask, &clientMask, &commonMask);
    char commonNames[128] = {};
    SaveSync_FormatDlcNames(commonMask, commonNames, sizeof(commonNames));
    uint32_t disabledMask = (hostMask | clientMask) & ~commonMask;
    AppendDiagLine(batch, tag, "DlcSync: Host=0x%04X | Client=0x%04X | Common=0x%04X | Disabled=0x%04X | Active='%s'",
                   hostMask, clientMask, commonMask, disabledMask, commonNames[0] ? commonNames : "None");

    // Vehicle Sync telemetry
    VehicleSyncDiagStats vs = {};
    Purchase_GetDiagStats(&vs);
    AppendDiagLine(batch, tag, "VehSync: Attempt=%llu | Made=%llu | BlockDlc=%llu | BlockMod=%llu | Remapped=%llu | Fails=%llu | Sold=%llu | [Road=%u Rail=%u Water=%u Air=%u Spec=%u]",
                   (unsigned long long)vs.purchasesAttempted, (unsigned long long)vs.purchasesMade,
                   (unsigned long long)vs.purchasesBlockedDlc, (unsigned long long)vs.purchasesBlockedMod,
                   (unsigned long long)vs.scriptNameRemapped, (unsigned long long)vs.scriptNameResolveFails,
                   (unsigned long long)vs.vehiclesSold,
                   vs.countRoad, vs.countRail, vs.countWater, vs.countAir, vs.countSpecial);

    // Lockstep metrics if active
    if (lockstep || st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME) {
        bool paused = true;
        int speed = 1;
        Determinism_SessionSpeed(&paused, &speed);
        unsigned long long tick = Determinism_StepCount();
        unsigned long long rtt = 0;
        bool hasRtt = LockstepNet_RoundTrip(&rtt);
        LockstepFlow fl = LockstepNet_Flow();
        const char* desync = LockstepNet_Desync();
        unsigned long long lastMatched = LockstepNet_LastMatchedTick();
        AppendDiagLine(batch, tag, "Lockstep: Tick=%llu | MatchedTick=%llu | Paused=%d | Speed=%d | Ping=%llums(valid=%d) | Behind=%llu | Stalls=%ds | msSinceStall=%llu | Desync='%s'",
                       tick, lastMatched, (int)paused, speed, rtt, (int)hasRtt,
                       fl.behind, fl.stalls, fl.msSinceStall, desync ? desync : "");

        DeterminismDiagInfo dinfo = {};
        Determinism_GetDiagInfo(&dinfo);
        HeldFrameDiagStats hf = {};
        HeldFrame_GetDiagStats(&hf);
        VehicleSoftSyncDiagStats vss = {};
        VehicleSoftSync_GetDiagStats(&vss);
        AppendDiagLine(batch, tag, "HeldFrame: Beats=%llu | Phase=%d(%s) | TickDebt=%.3f | SubTickAlpha=%.3f | Drawn=%llu | TrackedVeh=%u | LastTargets=%u | LastInterp=%u | Fallbacks=%llu | RestoreRefusals=%llu",
                       dinfo.frameBeats, dinfo.framePhase, FramePhaseName(dinfo.framePhase), dinfo.tickDebt, dinfo.subTickAlpha,
                       hf.heldFramesDrawn, hf.trackedVehicles, hf.lastTargetsCount, hf.lastInterpolatedCount,
                       hf.guardFallbacks, hf.restoreRefusals);
        AppendDiagLine(batch, tag, "SoftSync: Sent=%llu | Applied=%llu | ImmSweeps=%llu | VehSynced=%llu | Blended=%llu | Snapped=%llu | CargoFixed=%llu | BldStorageFixed=%llu | MaxDrift=%.2fm (AllTime=%.2fm)",
                       vss.batchesSent, vss.batchesApplied, vss.immediateSweeps, vss.vehiclesSynced,
                       vss.posBlended, vss.posSnapped, vss.cargoUpdated, vss.bldStorageUpdated, vss.lastMaxDriftM, vss.allTimeMaxDriftM);
    }

    if (isHeartbeat) {
        AppendPerfSnapshotLines(batch, tag);
    }

    WriteRawBlock(batch.data(), batch.size(), !isHeartbeat);
}

void DiagLog_FrameTick(void)
{
    static SessionState s_lastState = SESSION_IDLE;
    static bool s_lastWorld = false;
    static std::string s_lastErr;
    static ULONGLONG s_lastHeartbeat = 0;

    SessionState st = SaveSync_GetState();
    bool world = SaveSync_IsWorldLoaded();
    const char* err = SaveSync_GetLastError();

    bool stateChanged = (st != s_lastState) || (world != s_lastWorld);
    bool errChanged = (err && err[0] && s_lastErr != err);

    if (stateChanged || errChanged) {
        s_lastState = st;
        s_lastWorld = world;
        s_lastErr = err ? err : "";
        DumpLiveSnapshot("STATE");
    }

    ULONGLONG now = GetTickCount64();
    // Periodic heartbeat every 5 seconds while in a session (or every 30s when idle)
    ULONGLONG interval = (st != SESSION_IDLE) ? 5000ULL : 30000ULL;
    if (now - s_lastHeartbeat >= interval) {
        s_lastHeartbeat = now;
        DumpLiveSnapshot("HEARTBT");
    }
}

// ---------------------------------------------------------------------------
// Remote Client Log Bundle Packaging & Saving
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct RemoteLogBundleHeader {
    char magic[8];        // "RPMPLOG1"
    char reason[32];      // e.g. "DESYNC", "DISCONNECT", "CRASH", "PREV_SESSION"
    char peerName[64];    // Steam persona name or IP
    unsigned int rawSize; // uncompressed size of entries
    unsigned int compSize; // compressed size (0 = uncompressed)
};
struct RemoteLogFileEntry {
    char fileName[48];
    unsigned int size;
};
#pragma pack(pop)

void DiagLog_ReadFileTail(const char* fullPath, size_t maxBytes, std::vector<unsigned char>& outBuf)
{
    outBuf.clear();
    if (!fullPath || !fullPath[0] || maxBytes == 0) return;
    HANDLE h = CreateFileA(fullPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER fileSize = {};
    if (GetFileSizeEx(h, &fileSize) && fileSize.QuadPart > 0) {
        LONGLONG toRead = fileSize.QuadPart;
        if (toRead > (LONGLONG)maxBytes) {
            LARGE_INTEGER seekPos = {};
            seekPos.QuadPart = fileSize.QuadPart - (LONGLONG)maxBytes;
            SetFilePointerEx(h, seekPos, nullptr, FILE_BEGIN);
            toRead = (LONGLONG)maxBytes;
        }
        outBuf.resize((size_t)toRead);
        DWORD bytesRead = 0;
        if (ReadFile(h, outBuf.data(), (DWORD)toRead, &bytesRead, nullptr)) {
            outBuf.resize(bytesRead);
        } else {
            outBuf.clear();
        }
    }
    CloseHandle(h);
}

const char* DiagLog_GetGameDir(void)
{
    return g_gameDir;
}

const char* DiagLog_GetLoaderBaseDir(void)
{
    return (g_origHost && g_origHost->baseDir) ? g_origHost->baseDir : "";
}

bool DiagLog_DetectPrevCrashOrDesync(void)
{
    if (!g_gameDir[0]) return false;

    bool hadCrashInLoader = false;
    if (g_origHost && g_origHost->baseDir && g_origHost->baseDir[0]) {
        const char* kLoaderLogs[2] = { "tesmioloader.prev.log", "tesmioloader.log" };
        for (int i = 0; i < 2 && !hadCrashInLoader; ++i) {
            char loaderLogPath[MAX_PATH] = {};
            _snprintf_s(loaderLogPath, sizeof(loaderLogPath), _TRUNCATE, "%s\\%s", g_origHost->baseDir, kLoaderLogs[i]);
            std::vector<unsigned char> loaderTail;
            DiagLog_ReadFileTail(loaderLogPath, 64 * 1024, loaderTail);
            if (!loaderTail.empty()) {
                loaderTail.push_back(0);
                if (strstr((const char*)loaderTail.data(), "=== CRASH:") != nullptr ||
                    strstr((const char*)loaderTail.data(), "FAULT ") != nullptr)
                    hadCrashInLoader = true;
            }
        }
    }

    char crashLogPath[MAX_PATH] = {};
    _snprintf_s(crashLogPath, sizeof(crashLogPath), _TRUNCATE, "%s\\RepublicMP_LastCrash.log", g_gameDir);
    bool hasLastCrashFile = (GetFileAttributesA(crashLogPath) != INVALID_FILE_ATTRIBUTES);

    char prevPath[MAX_PATH] = {};
    _snprintf_s(prevPath, sizeof(prevPath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.prev.log", g_gameDir);
    std::vector<unsigned char> tail;
    DiagLog_ReadFileTail(prevPath, 128 * 1024, tail);
    bool hadCrashInDiagPrev = false;
    bool abnormalInGameExit = false;
    if (!tail.empty()) {
        tail.push_back(0);
        const char* text = (const char*)tail.data();
        hadCrashInDiagPrev = (strstr(text, "[CRASH  ]") != nullptr || strstr(text, "DESYNC - ") != nullptr);
        abnormalInGameExit = ((strstr(text, "State=CLIENT_IN_GAME") != nullptr ||
                               strstr(text, "State=HOST_IN_GAME") != nullptr) &&
                              strstr(text, "=== RepublicMP Diagnostics Shutting Down ===") == nullptr);
    }

    return hadCrashInDiagPrev || hadCrashInLoader || abnormalInGameExit || hasLastCrashFile;
}

bool DiagLog_HasPrevCrashOrDesyncLog(void)
{
    static bool s_checkedAndSent = false;
    if (s_checkedAndSent || !g_gameDir[0]) return false;
    if (DiagLog_DetectPrevCrashOrDesync()) {
        s_checkedAndSent = true;
        return true;
    }
    return false;
}

bool DiagLog_BuildClientLogBundle(std::vector<unsigned char>& outPayload, const char* reason)
{
    outPayload.clear();
    if (!TwoStates_GetSendClientLogs() || !g_gameDir[0]) return false;

    DumpLiveSnapshot("LOGSEND");

    const char* kFiles[5] = {
        "tesmioloader.log",
        "tesmioloader.prev.log",
        "RepublicMP_Diagnostics.log",
        "RepublicMP_Diagnostics.prev.log",
        "RepublicMP_LastCrash.log"
    };
    const size_t kMaxTailPerFile = 256 * 1024;

    std::vector<unsigned char> rawEntries;
    rawEntries.reserve(512 * 1024);

    for (int i = 0; i < 5; ++i) {
        char path[MAX_PATH] = {};
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_gameDir, kFiles[i]);
        std::vector<unsigned char> content;
        DiagLog_ReadFileTail(path, kMaxTailPerFile, content);
        if (content.empty() && i <= 1 && g_origHost && g_origHost->baseDir && g_origHost->baseDir[0]) {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_origHost->baseDir, kFiles[i]);
            DiagLog_ReadFileTail(path, kMaxTailPerFile, content);
        }
        if (content.empty()) continue;

        RemoteLogFileEntry entry = {};
        strncpy_s(entry.fileName, kFiles[i], _TRUNCATE);
        entry.size = (unsigned int)content.size();
        size_t off = rawEntries.size();
        rawEntries.resize(off + sizeof(entry) + content.size());
        memcpy(rawEntries.data() + off, &entry, sizeof(entry));
        memcpy(rawEntries.data() + off + sizeof(entry), content.data(), content.size());
    }

    if (rawEntries.empty()) return false;

    RemoteLogBundleHeader hdr = {};
    memcpy(hdr.magic, "RPMPLOG1", 8);
    strncpy_s(hdr.reason, reason && reason[0] ? reason : "SESSION", _TRUNCATE);
    std::string stmName = SteamP2P_GetLocalPersonaNameCopy();
    uint64_t stmId = SteamP2P_GetLocalSteamId();
    if (!stmName.empty() && stmId != 0) {
        _snprintf_s(hdr.peerName, sizeof(hdr.peerName), _TRUNCATE, "%s (%llu)", stmName.c_str(), (unsigned long long)stmId);
    } else {
        std::string localName = LanDiscovery_GetLocalDisplayName();
        if (!localName.empty() && localName != "Host") {
            _snprintf_s(hdr.peerName, sizeof(hdr.peerName), _TRUNCATE, "%s (LAN)", localName.c_str());
        } else {
            strncpy_s(hdr.peerName, "Client", _TRUNCATE);
        }
    }
    hdr.rawSize = (unsigned int)rawEntries.size();

    mz_ulong compBound = mz_compressBound((mz_ulong)rawEntries.size());
    std::vector<unsigned char> compBuf(compBound);
    mz_ulong compLen = compBound;
    if (mz_compress2(compBuf.data(), &compLen, rawEntries.data(), (mz_ulong)rawEntries.size(), MZ_DEFAULT_COMPRESSION) == MZ_OK &&
        compLen < (mz_ulong)rawEntries.size() && sizeof(hdr) + compLen <= (900 * 1024)) {
        hdr.compSize = (unsigned int)compLen;
        outPayload.resize(sizeof(hdr) + compLen);
        memcpy(outPayload.data(), &hdr, sizeof(hdr));
        memcpy(outPayload.data() + sizeof(hdr), compBuf.data(), compLen);
    } else if (sizeof(hdr) + rawEntries.size() <= (900 * 1024)) {
        hdr.compSize = 0;
        outPayload.resize(sizeof(hdr) + rawEntries.size());
        memcpy(outPayload.data(), &hdr, sizeof(hdr));
        memcpy(outPayload.data() + sizeof(hdr), rawEntries.data(), rawEntries.size());
    } else {
        return false;
    }

    DiagLog_Printf("REMLOG", "Packaged client log bundle (reason='%s', raw=%u B, wire=%u B)",
                   hdr.reason, hdr.rawSize, (unsigned int)outPayload.size());
    return true;
}

bool DiagLog_IsSafeRemoteLogFileName(const char* name)
{
    if (!name || !name[0]) return false;
    const size_t len = strnlen(name, 48);
    if (len == 0 || len >= 48) return false;
    if (name[0] == '.' || name[0] == ' ' || name[len - 1] == '.' || name[len - 1] == ' ') {
        return false;
    }
    if (strstr(name, "..") != nullptr) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c == 0x7Fu) return false;
        switch (c) {
            case '<': case '>': case ':': case '"':
            case '/': case '\\': case '|': case '?': case '*':
                return false;
            default:
                break;
        }
    }
    char stem[48] = {};
    size_t stemLen = 0;
    while (stemLen < len && name[stemLen] != '.') {
        stem[stemLen] = name[stemLen];
        ++stemLen;
    }
    stem[stemLen] = '\0';
    static const char* const kReservedNames[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    for (const char* reserved : kReservedNames) {
        if (_stricmp(stem, reserved) == 0) return false;
    }
    return true;
}

bool DiagLog_SaveRemoteClientLogBundle(const void* data, size_t size)
{
    if (!data || size < sizeof(RemoteLogBundleHeader) || !g_gameDir[0]) return false;
    const RemoteLogBundleHeader* hdr = (const RemoteLogBundleHeader*)data;
    if (memcmp(hdr->magic, "RPMPLOG1", 8) != 0) return false;
    if (hdr->rawSize == 0 || hdr->rawSize > (4u << 20)) return false;

    const unsigned char* body = (const unsigned char*)data + sizeof(RemoteLogBundleHeader);
    size_t bodySize = size - sizeof(RemoteLogBundleHeader);

    std::vector<unsigned char> rawBuf;
    if (hdr->compSize > 0) {
        if (bodySize < hdr->compSize) return false;
        rawBuf.resize(hdr->rawSize);
        mz_ulong destLen = (mz_ulong)hdr->rawSize;
        if (mz_uncompress(rawBuf.data(), &destLen, body, (mz_ulong)hdr->compSize) != MZ_OK || destLen != hdr->rawSize) {
            DiagLog_Printf("REMLOG", "Failed to decompress remote client log bundle");
            return false;
        }
    } else {
        if (bodySize < hdr->rawSize) return false;
        rawBuf.assign(body, body + hdr->rawSize);
    }

    char reasonClean[32] = {};
    char peerSafe[64] = {};
    memcpy(reasonClean, hdr->reason, sizeof(reasonClean) - 1);
    memcpy(peerSafe, hdr->peerName, sizeof(peerSafe) - 1);
    for (char* p = reasonClean; *p; ++p) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            *p = '_';
    }
    if (!reasonClean[0]) strncpy_s(reasonClean, "CLIENT", _TRUNCATE);

    char baseRemoteDir[MAX_PATH] = {};
    _snprintf_s(baseRemoteDir, sizeof(baseRemoteDir), _TRUNCATE, "%s\\RemoteLogs", g_gameDir);
    CreateDirectoryA(baseRemoteDir, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char sessionDir[MAX_PATH] = {};
    _snprintf_s(sessionDir, sizeof(sessionDir), _TRUNCATE,
                "%s\\%s_%04d%02d%02d_%02d%02d%02d",
                baseRemoteDir, reasonClean,
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA(sessionDir, nullptr);

    // Write summary info.txt
    char infoPath[MAX_PATH] = {};
    _snprintf_s(infoPath, sizeof(infoPath), _TRUNCATE, "%s\\info.txt", sessionDir);
    FILE* fInfo = nullptr;
    if (fopen_s(&fInfo, infoPath, "wb") == 0 && fInfo) {
        fprintf(fInfo, "Reason: %s\r\nPeer: %s\r\nReceived: %04d-%02d-%02d %02d:%02d:%02d.%03d\r\n",
                reasonClean, peerSafe,
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        fclose(fInfo);
    }

    size_t pos = 0;
    int filesSaved = 0;
    while (pos + sizeof(RemoteLogFileEntry) <= rawBuf.size()) {
        RemoteLogFileEntry entry = {};
        memcpy(&entry, rawBuf.data() + pos, sizeof(entry));
        entry.fileName[sizeof(entry.fileName) - 1] = '\0';
        pos += sizeof(RemoteLogFileEntry);
        if (pos + entry.size > rawBuf.size()) break;

        if (DiagLog_IsSafeRemoteLogFileName(entry.fileName)) {
            char outPath[MAX_PATH] = {};
            _snprintf_s(outPath, sizeof(outPath), _TRUNCATE, "%s\\%s", sessionDir, entry.fileName);
            FILE* fOut = nullptr;
            if (fopen_s(&fOut, outPath, "wb") == 0 && fOut) {
                fwrite(rawBuf.data() + pos, 1, entry.size, fOut);
                fclose(fOut);
                filesSaved++;
            }
        }
        pos += entry.size;
    }

    if (filesSaved == 0) {
        DeleteFileA(infoPath);
        RemoveDirectoryA(sessionDir);
        return false;
    }

    DiagLog_Printf("REMLOG", "Saved %d remote client log file(s) from '%s' (reason=%s) to %s",
                   filesSaved, peerSafe, reasonClean, sessionDir);
    if (filesSaved > 0) {
        char toast[256] = {};
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("Получены логи клиента (%s) -> RemoteLogs", "Received client logs (%s) -> RemoteLogs"),
                    reasonClean);
        TwoStates_AddToast(toast, 1);
    }
    return filesSaved > 0;
}

