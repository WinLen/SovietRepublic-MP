// SPDX-License-Identifier: GPL-3.0-only
// See diag_log.h.
#include "diag_log.h"
#include "version.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../session/mod_sync.h"
#include "../session/updater.h"
#include "../determinism/determinism.h"
#include "../determinism/state_hash.h"
#include "../lockstep/lockstep_net.h"
#include "../transport/steam_p2p.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"

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
        code != EXCEPTION_PRIV_INSTRUCTION) {
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
