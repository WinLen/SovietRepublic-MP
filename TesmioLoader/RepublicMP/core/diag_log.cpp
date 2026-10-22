// SPDX-License-Identifier: GPL-3.0-only
// See diag_log.h.
#include "diag_log.h"
#include "version.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../session/mod_sync.h"
#include "../session/updater.h"
#include "../determinism/determinism.h"
#include "../determinism/held_frame.h"
#include "../determinism/state_hash.h"
#include "../lockstep/lockstep_net.h"
#include "../transport/steam_p2p.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../world_sync/vehicles/vehicle_soft_sync.h"
#include "../third_party/miniz/miniz.h"
#include "../ui/lang.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

static CRITICAL_SECTION g_diagLock;
static bool g_diagLockInit = false;
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

static void EnsureDiagLock()
{
    if (!g_diagLockInit) {
        InitializeCriticalSection(&g_diagLock);
        g_diagLockInit = true;
    }
}

static void WriteRawLine(const char* category, const char* body)
{
    EnsureDiagLock();
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();

    char line[4600];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [TID:%05lu] [%-7s] %s\r\n",
                        st.wYear, st.wMonth, st.wDay,
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        (unsigned long)tid, category ? category : "INFO", body ? body : "");
    if (n <= 0) return;

    EnterCriticalSection(&g_diagLock);
    if (g_hDiagFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_hDiagFile, line, (DWORD)n, &written, nullptr);
        FlushFileBuffers(g_hDiagFile);
    }
    LeaveCriticalSection(&g_diagLock);
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
        const char* baseName = strrchr(modPath, '\\');
        baseName = baseName ? (baseName + 1) : modPath;
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
    DiagLog_Printf("CRASH", "SoftSyncCtx : Sent=%llu | Applied=%llu | ImmSweeps=%llu | VehSynced=%llu | Blended=%llu | Snapped=%llu | CargoFixed=%llu | MaxDrift=%.2fm/%.2fm",
                   vss.batchesSent, vss.batchesApplied, vss.immediateSweeps, vss.vehiclesSynced,
                   vss.posBlended, vss.posSnapped, vss.cargoUpdated, vss.lastMaxDriftM, vss.allTimeMaxDriftM);
    DiagLog_Printf("CRASH", "RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
                   c->Rax, c->Rbx, c->Rcx, c->Rdx);
    DiagLog_Printf("CRASH", "RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX",
                   c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    DiagLog_Printf("CRASH", "R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX",
                   c->R8, c->R9, c->R10, c->R11);
    DiagLog_Printf("CRASH", "R12=%016llX R13=%016llX R14=%016llX R15=%016llX",
                   c->R12, c->R13, c->R14, c->R15);

    // Walk stack pointers cleanly to find code return addresses
    uintptr_t* sp = (uintptr_t*)c->Rsp;
    int printed = 0;
    for (int i = 0; i < 64 && printed < 16; ++i) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        uintptr_t val = sp[i];
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
    if (!SaveSync_IsHostRole() && LockstepNet_IsConnected() && TwoStates_GetSendClientLogs()) {
        LockstepNet_SendClientLogs("CRASH");
        Sleep(150);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

const TsmHost* DiagLog_InitAndWrapHost(const TsmHost* origHost)
{
    EnsureDiagLock();
    g_origHost = origHost;
    g_wrappedHost = *origHost;
    g_wrappedHost.log = &HookedHostLogf;

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    strncpy_s(g_gameDir, exePath, _TRUNCATE);
    if (char* s = strrchr(g_gameDir, '\\')) *s = '\0';

    _snprintf_s(g_diagFilePath, sizeof(g_diagFilePath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.log", g_gameDir);
    char prevPath[MAX_PATH] = {};
    _snprintf_s(prevPath, sizeof(prevPath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.prev.log", g_gameDir);

    // If dinput8.dll did not already rotate the log in this process, rotate it if older than 5 seconds
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExA(g_diagFilePath, GetFileExInfoStandard, &fad)) {
        FILETIME nowFt = {};
        GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER uNow, uFile;
        uNow.LowPart = nowFt.dwLowDateTime; uNow.HighPart = nowFt.dwHighDateTime;
        uFile.LowPart = fad.ftLastWriteTime.dwLowDateTime; uFile.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        // If existing file was written more than 3 seconds ago, it's from a previous run
        if (uNow.QuadPart > uFile.QuadPart && (uNow.QuadPart - uFile.QuadPart) > 30000000ULL) {
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
}

static void DumpLiveSnapshot(const char* tag)
{
    SessionState st = SaveSync_GetState();
    bool world = SaveSync_IsWorldLoaded();
    bool host = SaveSync_IsHostRole();
    bool lockstep = SaveSync_IsLockstepSession();
    const char* peer = SaveSync_GetPeerIp();
    const char* lastErr = SaveSync_GetLastError();

    DiagLog_Printf(tag, "State=%s | Role=%s | WorldLoaded=%d | Lockstep=%d | Peer='%s' | LastErr='%s'",
                   SessionStateName(st), host ? "HOST" : "CLIENT", (int)world, (int)lockstep,
                   peer ? peer : "", lastErr ? lastErr : "");

    // Steam P2P status
    bool stmAvail = SteamP2P_IsAvailable();
    uint64_t mySteamId = SteamP2P_GetLocalSteamId();
    const char* myName = SteamP2P_GetLocalPersonaName();
    DiagLog_Printf(tag, "SteamP2P: Available=%d | LocalSteamID=%llu ('%s')",
                   (int)stmAvail, (unsigned long long)mySteamId, myName ? myName : "");

    // Two States & Map Expansion status
    DiagLog_Printf(tag, "TwoStates: Mode=%d | LocalState=%d | BorderHalf=%.0fm | ExpandBorders=%d | West(RUB=%.0f, USD=%.0f) | East(RUB=%.0f, USD=%.0f)",
                   (int)TwoStates_GetMode(), TwoStates_LocalState(),
                   TwoStates_GetBorderHalfWidth(), (int)TwoStates_GetExpandBorders(),
                   TwoStates_GetRubles(1), TwoStates_GetDollars(1),
                   TwoStates_GetRubles(2), TwoStates_GetDollars(2));

    // ModSync status
    DiagLog_Printf(tag, "ModSync: PackMode=%d | CatalogMods=%d | ActiveServerMods=%d",
                   (int)ModSync_GetPackMode(), ModSync_GetModCount(),
                   ModSync_GetActiveServerModCount());

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
        DiagLog_Printf(tag, "Lockstep: Tick=%llu | MatchedTick=%llu | Paused=%d | Speed=%d | Ping=%llums(valid=%d) | Behind=%llu | Stalls=%ds | msSinceStall=%llu | Desync='%s'",
                       tick, lastMatched, (int)paused, speed, rtt, (int)hasRtt,
                       fl.behind, fl.stalls, fl.msSinceStall, desync ? desync : "");

        DeterminismDiagInfo dinfo = {};
        Determinism_GetDiagInfo(&dinfo);
        HeldFrameDiagStats hf = {};
        HeldFrame_GetDiagStats(&hf);
        VehicleSoftSyncDiagStats vss = {};
        VehicleSoftSync_GetDiagStats(&vss);
        DiagLog_Printf(tag, "HeldFrame: Beats=%llu | Phase=%d(%s) | TickDebt=%.3f | SubTickAlpha=%.3f | Drawn=%llu | TrackedVeh=%u | LastTargets=%u | LastInterp=%u | Fallbacks=%llu | RestoreRefusals=%llu",
                       dinfo.frameBeats, dinfo.framePhase, FramePhaseName(dinfo.framePhase), dinfo.tickDebt, dinfo.subTickAlpha,
                       hf.heldFramesDrawn, hf.trackedVehicles, hf.lastTargetsCount, hf.lastInterpolatedCount,
                       hf.guardFallbacks, hf.restoreRefusals);
        DiagLog_Printf(tag, "SoftSync: Sent=%llu | Applied=%llu | ImmSweeps=%llu | VehSynced=%llu | Blended=%llu | Snapped=%llu | CargoFixed=%llu | MaxDrift=%.2fm (AllTime=%.2fm)",
                       vss.batchesSent, vss.batchesApplied, vss.immediateSweeps, vss.vehiclesSynced,
                       vss.posBlended, vss.posSnapped, vss.cargoUpdated, vss.lastMaxDriftM, vss.allTimeMaxDriftM);
    }
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

static void ReadLogFileTail(const char* fullPath, size_t maxBytes, std::vector<unsigned char>& outBuf)
{
    outBuf.clear();
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

bool DiagLog_HasPrevCrashOrDesyncLog(void)
{
    static bool s_checkedAndSent = false;
    if (s_checkedAndSent || !g_gameDir[0]) return false;

    char prevPath[MAX_PATH] = {};
    _snprintf_s(prevPath, sizeof(prevPath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.prev.log", g_gameDir);
    std::vector<unsigned char> tail;
    ReadLogFileTail(prevPath, 128 * 1024, tail);
    if (tail.empty()) return false;
    tail.push_back(0);
    const char* text = (const char*)tail.data();
    bool hadCrashInLoader = false;
    if (g_origHost && g_origHost->baseDir && g_origHost->baseDir[0]) {
        char loaderLogPath[MAX_PATH] = {};
        _snprintf_s(loaderLogPath, sizeof(loaderLogPath), _TRUNCATE, "%s\\tesmioloader.log", g_origHost->baseDir);
        std::vector<unsigned char> loaderTail;
        ReadLogFileTail(loaderLogPath, 64 * 1024, loaderTail);
        if (!loaderTail.empty()) {
            loaderTail.push_back(0);
            if (strstr((const char*)loaderTail.data(), "=== CRASH:") != nullptr)
                hadCrashInLoader = true;
        }
    }
    bool abnormalInGameExit = (strstr(text, "State=CLIENT_IN_GAME") != nullptr &&
                               strstr(text, "=== RepublicMP Diagnostics Shutting Down ===") == nullptr);
    if (strstr(text, "[CRASH  ]") != nullptr || strstr(text, "DESYNC - ") != nullptr ||
        hadCrashInLoader || abnormalInGameExit) {
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

    const char* kFiles[3] = {
        "tesmioloader.log",
        "RepublicMP_Diagnostics.log",
        "RepublicMP_Diagnostics.prev.log"
    };
    const size_t kMaxTailPerFile = 256 * 1024;

    std::vector<unsigned char> rawEntries;
    rawEntries.reserve(512 * 1024);

    for (int i = 0; i < 3; ++i) {
        char path[MAX_PATH] = {};
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_gameDir, kFiles[i]);
        std::vector<unsigned char> content;
        ReadLogFileTail(path, kMaxTailPerFile, content);
        if (content.empty() && i == 0 && g_origHost && g_origHost->baseDir && g_origHost->baseDir[0]) {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_origHost->baseDir, kFiles[i]);
            ReadLogFileTail(path, kMaxTailPerFile, content);
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
    const char* stmName = SteamP2P_GetLocalPersonaName();
    uint64_t stmId = SteamP2P_GetLocalSteamId();
    if (stmName && stmName[0]) {
        _snprintf_s(hdr.peerName, sizeof(hdr.peerName), _TRUNCATE, "%s (%llu)", stmName, (unsigned long long)stmId);
    } else {
        strncpy_s(hdr.peerName, "Client", _TRUNCATE);
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

        // Sanitize filename (strip any path separators)
        const char* baseName = strrchr(entry.fileName, '\\');
        if (!baseName) baseName = strrchr(entry.fileName, '/');
        baseName = baseName ? (baseName + 1) : entry.fileName;
        if (baseName[0] && strstr(baseName, "..") == nullptr) {
            char outPath[MAX_PATH] = {};
            _snprintf_s(outPath, sizeof(outPath), _TRUNCATE, "%s\\%s", sessionDir, baseName);
            FILE* fOut = nullptr;
            if (fopen_s(&fOut, outPath, "wb") == 0 && fOut) {
                fwrite(rawBuf.data() + pos, 1, entry.size, fOut);
                fclose(fOut);
                filesSaved++;
            }
        }
        pos += entry.size;
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

