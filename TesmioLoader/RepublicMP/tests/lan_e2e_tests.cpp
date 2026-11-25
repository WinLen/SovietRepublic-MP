// SPDX-License-Identifier: GPL-3.0-only
// 4-Tier Opaque-Box E2E Test Suite for RepublicMP LAN & Steam-Free Support (F12).
// Dual-mode: compiles standalone (C++20 TU exposing int RunLanE2eTestSuite())
// and via #include "lan_e2e_tests.cpp" inside tests/test_main.cpp.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef REPUBLICMP_E2E_EXTERNAL_INCLUDES
#include "../../src/tesmio_plugin.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../transport/network.h"
#include "../transport/steam_p2p.h"
#include "../transport/lan_discovery.h"
#include "../session/mod_sync.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../lockstep/lockstep_net.h"
#include "../ui/native_menu.h"
#include "../ui/sync_block.h"
#include "../ui/ui_common.h"
#include "../ui/lang.h"
#include "imgui.h"
#endif

// Forward-declared plugin init entry point for standalone mode.
extern "C" int TsmPluginInit(const TsmHost* origHost, TsmPluginInfo* info);

namespace lan_e2e {

static int g_failedAssertions = 0;

#define LAN_CHECK(expr)                                                               \
    do {                                                                              \
        if (!(expr)) {                                                                \
            std::printf("  [FAIL] %s:%d: assertion failed: %s\n",                     \
                        __FILE__, __LINE__, #expr);                                   \
            ++lan_e2e::g_failedAssertions;                                            \
            return false;                                                             \
        }                                                                             \
    } while (0)

#define LAN_CHECK_NEAR(a, b, eps)                                                     \
    do {                                                                              \
        const double _va = static_cast<double>(a);                                    \
        const double _vb = static_cast<double>(b);                                    \
        const double _ve = static_cast<double>(eps);                                  \
        if (std::fabs(_va - _vb) > _ve) {                                             \
            std::printf("  [FAIL] %s:%d: %.6f != %.6f (eps=%g) [%s vs %s]\n",         \
                        __FILE__, __LINE__, _va, _vb, _ve, #a, #b);                   \
            ++lan_e2e::g_failedAssertions;                                            \
            return false;                                                             \
        }                                                                             \
    } while (0)

// Win32 MIB_IF_TYPE numeric constants (avoids pulling <ipifcons.h>).
static constexpr uint32_t kIfEthernet = 6u;
static constexpr uint32_t kIfLoopback = 24u;
static constexpr uint32_t kIfPropVirt = 53u;
static constexpr uint32_t kIfWifi     = 71u;
static constexpr uint32_t kIfTunnel   = 131u;

static constexpr size_t kStandaloneExeSize = 0x1000000u;
static constexpr uintptr_t kWorldRoadNetRva = 0xA2E758u;

// ---------------------------------------------------------------------------
// RAII Guards & Standalone Harness
// ---------------------------------------------------------------------------

struct ScopedWsa {
    bool ok = false;
    ScopedWsa() {
        WSADATA wsa = {};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~ScopedWsa() {
        if (ok) {
            WSACleanup();
        }
    }
    ScopedWsa(const ScopedWsa&) = delete;
    ScopedWsa& operator=(const ScopedWsa&) = delete;
};

struct ScopedSocket {
    SOCKET sock = INVALID_SOCKET;
    explicit ScopedSocket(SOCKET s = INVALID_SOCKET) : sock(s) {}
    ~ScopedSocket() {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }
    void Reset(SOCKET s = INVALID_SOCKET) {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
        }
        sock = s;
    }
    SOCKET Release() {
        SOCKET out = sock;
        sock = INVALID_SOCKET;
        return out;
    }
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;
};

struct ScopedTempDir {
    std::filesystem::path dir;
    explicit ScopedTempDir(const char* tag) {
        static std::atomic<uint32_t> s_seq{ 0 };
        const uint32_t seq = ++s_seq;
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec || base.empty()) {
            base = ".";
        }
        char name[96] = {};
        _snprintf_s(name, sizeof(name), _TRUNCATE, "RepMP_E2E_%lu_%u_%s",
                    static_cast<unsigned long>(GetCurrentProcessId()),
                    static_cast<unsigned int>(seq),
                    tag ? tag : "tmp");
        dir = base / name;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    ~ScopedTempDir() {
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
};

struct ScopedDiscoveryGuard {
    ScopedDiscoveryGuard() {
        LanDiscovery_SetLoopbackOnlyForTest(true);
        Network_SetLoopbackOnlyForTest(true);
    }
    ~ScopedDiscoveryGuard() {
        LanDiscovery_SetHosting(false, SAVE_SYNC_PORT, 0, "", "");
        LanDiscovery_StopScan();
        LanDiscovery_Shutdown();
        LanDiscovery_SetSelfPidOverrideForTest(0, false);
        LanDiscovery_SetPortForTest(LAN_DISCOVERY_PORT);
        SaveSync_SetTcpPortForTest(0);
        LockstepNet_SetPortForTest(0);
        LanDiscovery_SetLoopbackOnlyForTest(true);
        Network_SetLoopbackOnlyForTest(true);
    }
    ScopedDiscoveryGuard(const ScopedDiscoveryGuard&) = delete;
    ScopedDiscoveryGuard& operator=(const ScopedDiscoveryGuard&) = delete;
};

struct ScopedLoopbackTestMode {
    ScopedLoopbackTestMode() {
        LanDiscovery_SetLoopbackOnlyForTest(true);
        Network_SetLoopbackOnlyForTest(true);
    }
    ~ScopedLoopbackTestMode() {
        // Keep loopback-only mode active for any subsequent tests in the same runner.
        Network_SetLoopbackOnlyForTest(true);
        LanDiscovery_SetLoopbackOnlyForTest(true);
    }
    ScopedLoopbackTestMode(const ScopedLoopbackTestMode&) = delete;
    ScopedLoopbackTestMode& operator=(const ScopedLoopbackTestMode&) = delete;
};

struct ScopedModSyncGuard {
    ScopedModSyncGuard() = default;
    ~ScopedModSyncGuard() {
        ModSync_CancelDownloadModal();
        ModSync_EndSession();
    }
    ScopedModSyncGuard(const ScopedModSyncGuard&) = delete;
    ScopedModSyncGuard& operator=(const ScopedModSyncGuard&) = delete;
};

struct ScopedHostOnNextLoadGuard {
    explicit ScopedHostOnNextLoadGuard(bool enable) {
        NativeMenu_SetHostOnNextLoadForTest(enable);
    }
    ~ScopedHostOnNextLoadGuard() {
        NativeMenu_ClearHostOnNextLoad();
    }
    ScopedHostOnNextLoadGuard(const ScopedHostOnNextLoadGuard&) = delete;
    ScopedHostOnNextLoadGuard& operator=(const ScopedHostOnNextLoadGuard&) = delete;
};

struct ScopedSessionIdleReset {
    ScopedSessionIdleReset() = default;
    ~ScopedSessionIdleReset() {
        SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
        TwoStates_OnSessionEnded();
    }
    ScopedSessionIdleReset(const ScopedSessionIdleReset&) = delete;
    ScopedSessionIdleReset& operator=(const ScopedSessionIdleReset&) = delete;
};

struct ScopedModPackMode {
    ModPackMode prev = MOD_PACK_HOST_ONLY;
    explicit ScopedModPackMode(ModPackMode mode) {
        prev = ModSync_GetPackMode();
        ModSync_SetPackMode(mode);
    }
    ~ScopedModPackMode() {
        ModSync_SetPackMode(prev);
    }
    ScopedModPackMode(const ScopedModPackMode&) = delete;
    ScopedModPackMode& operator=(const ScopedModPackMode&) = delete;
};

struct ScopedWorldLoadedFlag {
    bool active = false;
    int64_t prevRoadNet = 0;
    unsigned char prevTrigger = 0;
    ScopedWorldLoadedFlag() {
        if (g_exeBase != nullptr && g_exeSize > kWorldRoadNetRva + sizeof(int64_t)) {
            int64_t* roadPtr = reinterpret_cast<int64_t*>(g_exeBase + kWorldRoadNetRva);
            unsigned char* trigPtr = g_exeBase + LOAD_TRIGGER_DAT_RVA;
            prevRoadNet = *roadPtr;
            prevTrigger = *trigPtr;
            *roadPtr = 1;
            *trigPtr = 0;
            active = true;
        }
    }
    ~ScopedWorldLoadedFlag() {
        if (active && g_exeBase != nullptr) {
            *reinterpret_cast<int64_t*>(g_exeBase + kWorldRoadNetRva) = prevRoadNet;
            *(g_exeBase + LOAD_TRIGGER_DAT_RVA) = prevTrigger;
        }
    }
    ScopedWorldLoadedFlag(const ScopedWorldLoadedFlag&) = delete;
    ScopedWorldLoadedFlag& operator=(const ScopedWorldLoadedFlag&) = delete;
};

struct ScopedImGuiFrame {
    ImGuiContext* ctx = nullptr;
    bool inFrame = false;
    ScopedImGuiFrame() {
        ctx = ImGui::CreateContext();
        if (ctx != nullptr) {
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1920.0f, 1080.0f);
            unsigned char* pixels = nullptr;
            int w = 0;
            int h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            ImGui::NewFrame();
            inFrame = true;
        }
    }
    int RenderAndCountVertices() {
        if (!inFrame) return -1;
        ImGui::Render();
        inFrame = false;
        ImDrawData* dd = ImGui::GetDrawData();
        return (dd != nullptr) ? dd->TotalVtxCount : 0;
    }
    bool NextFrame() {
        if (ctx == nullptr) return false;
        if (inFrame) {
            ImGui::Render();
            inFrame = false;
        }
        ImGui::NewFrame();
        inFrame = true;
        return true;
    }
    ~ScopedImGuiFrame() {
        if (inFrame) {
            ImGui::EndFrame();
            inFrame = false;
        }
        if (ctx != nullptr) {
            ImGui::DestroyContext(ctx);
        }
    }
    ScopedImGuiFrame(const ScopedImGuiFrame&) = delete;
    ScopedImGuiFrame& operator=(const ScopedImGuiFrame&) = delete;
};

template <typename T>
struct ScopedExePodRestore {
    T* ptr = nullptr;
    T prev{};
    ScopedExePodRestore(size_t rva, T tempValue) {
        if (g_exeBase != nullptr && g_exeSize >= rva + sizeof(T)) {
            ptr = reinterpret_cast<T*>(g_exeBase + rva);
            prev = *ptr;
            *ptr = tempValue;
        }
    }
    ~ScopedExePodRestore() {
        if (ptr != nullptr) {
            *ptr = prev;
        }
    }
    ScopedExePodRestore(const ScopedExePodRestore&) = delete;
    ScopedExePodRestore& operator=(const ScopedExePodRestore&) = delete;
};

struct ScopedSettingsIniGuard {
    struct Entry {
        std::filesystem::path path;
        bool existed = false;
        std::string content;
    };
    std::vector<Entry> entries;
    std::string prevJoinAddr;

    static void CapturePath(std::vector<Entry>& out, const std::filesystem::path& p) {
        if (p.empty()) return;
        for (const auto& e : out) {
            std::error_code ec;
            if (std::filesystem::equivalent(e.path, p, ec) || e.path == p) return;
        }
        Entry e;
        e.path = p;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            e.existed = true;
            FILE* f = nullptr;
            if (fopen_s(&f, p.string().c_str(), "rb") == 0 && f != nullptr) {
                char buf[4096];
                size_t n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                    e.content.append(buf, n);
                }
                std::fclose(f);
            }
        }
        out.push_back(std::move(e));
    }

    ScopedSettingsIniGuard() {
        const char* savedAddr = TwoStates_GetSavedJoinAddress();
        prevJoinAddr = (savedAddr != nullptr && savedAddr[0] != '\0') ? savedAddr : "127.0.0.1";
        const std::string cfgIni = Lang_GetConfigPath();
        if (!cfgIni.empty()) {
            CapturePath(entries, std::filesystem::path(cfgIni));
        }
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&CapturePath), &hMod) && hMod != nullptr) {
            char modPath[MAX_PATH] = {};
            if (GetModuleFileNameA(hMod, modPath, MAX_PATH) > 0) {
                CapturePath(entries, std::filesystem::path(modPath).parent_path() / "republicmp_settings.ini");
            }
        }
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
            CapturePath(entries, std::filesystem::path(exePath).parent_path() / "republicmp_settings.ini");
        }
        std::error_code ec;
        CapturePath(entries, std::filesystem::current_path(ec) / "republicmp_settings.ini");
    }

    ~ScopedSettingsIniGuard() {
        TwoStates_SetSavedJoinAddress(prevJoinAddr.c_str());
        for (const auto& e : entries) {
            std::error_code ec;
            if (!e.existed) {
                std::filesystem::remove(e.path, ec);
            } else {
                FILE* f = nullptr;
                if (fopen_s(&f, e.path.string().c_str(), "wb") == 0 && f != nullptr) {
                    if (!e.content.empty()) {
                        (void)std::fwrite(e.content.data(), 1, e.content.size(), f);
                    }
                    std::fclose(f);
                }
            }
        }
    }
    ScopedSettingsIniGuard(const ScopedSettingsIniGuard&) = delete;
    ScopedSettingsIniGuard& operator=(const ScopedSettingsIniGuard&) = delete;
};

static constexpr const char* kLocalHostOpenEventName = "Local\\RepublicMP_LocalHostOpen_5556";
static constexpr const char* kLocalHostTestMutexName = "Local\\RepublicMP_Test_LocalHostEvent";

static bool IsLocalHostOpenEventSignaled(uint16_t port = SAVE_SYNC_PORT) {
    char evName[64] = {};
    _snprintf_s(evName, sizeof(evName), _TRUNCATE, "Local\\RepublicMP_LocalHostOpen_%u",
                static_cast<unsigned int>(port));
    HANDLE h = OpenEventA(SYNCHRONIZE, FALSE, evName);
    if (h == nullptr) {
        return false;
    }
    const bool signaled = (WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
    (void)CloseHandle(h);
    return signaled;
}

struct ScopedLocalHostEventMutex {
    HANDLE mtx = nullptr;
    bool locked = false;
    ScopedLocalHostEventMutex() {
        mtx = CreateMutexA(nullptr, FALSE, kLocalHostTestMutexName);
        if (mtx != nullptr) {
            const DWORD waitRc = WaitForSingleObject(mtx, 10000);
            locked = (waitRc == WAIT_OBJECT_0 || waitRc == WAIT_ABANDONED);
        }
    }
    ~ScopedLocalHostEventMutex() {
        if (mtx != nullptr) {
            if (locked) {
                (void)ReleaseMutex(mtx);
            }
            (void)CloseHandle(mtx);
        }
    }
    ScopedLocalHostEventMutex(const ScopedLocalHostEventMutex&) = delete;
    ScopedLocalHostEventMutex& operator=(const ScopedLocalHostEventMutex&) = delete;
};

struct ScopedLocalHostEvent {
    ScopedLocalHostEventMutex mutexGuard;
    HANDLE ev = nullptr;
    bool resetOnExit = false;
    explicit ScopedLocalHostEvent(bool createSignaled) {
        if (createSignaled) {
            ev = CreateEventA(nullptr, TRUE, FALSE, kLocalHostOpenEventName);
            if (ev != nullptr) {
                const bool alreadySignaled = (WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
                resetOnExit = !alreadySignaled;
                (void)SetEvent(ev);
            }
        }
    }
    ~ScopedLocalHostEvent() {
        if (ev != nullptr) {
            if (resetOnExit) {
                (void)ResetEvent(ev);
            }
            (void)CloseHandle(ev);
        }
    }
    ScopedLocalHostEvent(const ScopedLocalHostEvent&) = delete;
    ScopedLocalHostEvent& operator=(const ScopedLocalHostEvent&) = delete;
};

static bool CheckLocalPcHostWaitingIsolated() {
    ScopedLocalHostEventMutex mutexGuard;
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    LAN_CHECK(!SaveSync_IsBusy());
    LAN_CHECK(!SaveSync_IsHostAwaitingClient());

    bool stableMatch = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const bool before = IsLocalHostOpenEventSignaled();
        const bool waiting = SaveSync_IsLocalPcHostWaiting();
        const bool after = IsLocalHostOpenEventSignaled();
        if (before == after && waiting == before) {
            stableMatch = true;
            break;
        }
        Sleep(1);
    }
    LAN_CHECK(stableMatch);

    bool waitingWhileHost = true;
    {
        ScopedLocalHostEvent hostEventGuard(true);
        SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
        waitingWhileHost = SaveSync_IsLocalPcHostWaiting();
    }
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    LAN_CHECK(!waitingWhileHost);
    return true;
}

static void StandaloneLog(const char* fmt, ...) {
    (void)fmt;
}
static void** StandaloneFindIat(void*, const char*, const char*) { return nullptr; }
static int StandalonePatchIat(void*, const char*, const char*, void*, void**, const char*) { return 0; }
static int StandaloneInstallHook(void*, void*, void**, const unsigned char*, size_t, const char*) { return 0; }
static unsigned char* StandaloneAllocNear(unsigned char*, size_t) { return nullptr; }
static int StandaloneReadable(const void* p, size_t n) {
    if (p == nullptr || n == 0) return 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return 0;
    const uintptr_t endRegion = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (reinterpret_cast<uintptr_t>(p) + n <= endRegion) ? 1 : 0;
}
static long StandaloneFault(const char*, void*) { return EXCEPTION_EXECUTE_HANDLER; }
static const void* StandaloneConsume(const char*, unsigned) { return nullptr; }

struct ScopedStandaloneHost {
    bool initialized = false;
    BYTE* exeBuf = nullptr;
    char baseDir[MAX_PATH] = {};
    TsmHost host = {};

    ScopedStandaloneHost() {
        if (H != nullptr) {
            return;
        }
        exeBuf = static_cast<BYTE*>(VirtualAlloc(nullptr, kStandaloneExeSize,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (exeBuf == nullptr) {
            return;
        }
        std::memset(exeBuf, 0, kStandaloneExeSize);
        char tmp[MAX_PATH] = {};
        if (GetTempPathA(sizeof(tmp), tmp) == 0) {
            std::strncpy(tmp, ".", sizeof(tmp) - 1);
        }
        _snprintf_s(baseDir, sizeof(baseDir), _TRUNCATE, "%sRepMP_LanE2E_%lu",
                    tmp, static_cast<unsigned long>(GetCurrentProcessId()));
        CreateDirectoryA(baseDir, nullptr);

        host.structSize = sizeof(TsmHost);
        host.apiVersion = TSM_API_VERSION;
        host.exeModule = exeBuf;
        host.exeBase = exeBuf;
        host.exeSize = kStandaloneExeSize;
        host.engineModule = nullptr;
        host.baseDir = baseDir;
        host.log = &StandaloneLog;
        host.findIatSlot = &StandaloneFindIat;
        host.patchIat = &StandalonePatchIat;
        host.installInlineHook = &StandaloneInstallHook;
        host.allocNear = &StandaloneAllocNear;
        host.readablePtr = &StandaloneReadable;
        host.faultFilter = &StandaloneFault;
        host.consume = &StandaloneConsume;
        host.vfsRoot = baseDir;

        TsmPluginInfo info = {};
        TsmPluginInit(&host, &info);
        TsmBind(&host);
        initialized = true;
    }

    ~ScopedStandaloneHost() {
        if (initialized) {
            H = nullptr;
            g_exe = nullptr;
            g_exeBase = nullptr;
            g_exeSize = 0;
            g_engine = nullptr;
            g_baseDir = nullptr;
            g_vfsRoot = nullptr;
            DeleteCriticalSection(&g_lock);
            if (baseDir[0] != '\0') {
                for (int attempt = 0; attempt < 3; ++attempt) {
                    std::error_code ec;
                    std::filesystem::remove_all(baseDir, ec);
                    if (!std::filesystem::exists(baseDir, ec)) break;
                    Sleep(20);
                }
            }
            if (exeBuf != nullptr) {
                VirtualFree(exeBuf, 0, MEM_RELEASE);
                exeBuf = nullptr;
            }
        }
    }
    ScopedStandaloneHost(const ScopedStandaloneHost&) = delete;
    ScopedStandaloneHost& operator=(const ScopedStandaloneHost&) = delete;
};

// ---------------------------------------------------------------------------
// Shared Pure, Socket, Filesystem & Clipboard Helpers
// ---------------------------------------------------------------------------

static std::string UniqueTag(const char* prefix) {
    static std::atomic<uint32_t> s_id{ 0 };
    const uint32_t seq = ++s_id;
    char buf[48] = {};
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s_%lu_%u",
                prefix ? prefix : "E2E",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned int>(seq));
    return std::string(buf);
}

static bool TryBindLoopbackReuseProbe(int sockType, int proto, uint16_t port) {
    if (port == 0) return false;
    ScopedSocket s(socket(AF_INET, sockType, proto));
    if (s.sock == INVALID_SOCKET) return false;
    BOOL reuse = TRUE;
    (void)setsockopt(s.sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return bind(s.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

static bool WaitPortReuseBindable(int sockType, int proto, uint16_t port, int timeoutMs = 500) {
    if (port == 0) return false;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 1);
    while (GetTickCount64() < deadline) {
        if (TryBindLoopbackReuseProbe(sockType, proto, port)) {
            return true;
        }
        Sleep(5);
    }
    return TryBindLoopbackReuseProbe(sockType, proto, port);
}

static uint16_t AllocateEphemeralPort(int sockType, int proto) {
    static std::atomic<uint32_t> s_nextSeq{ 0u };
    const uint32_t pidBias = (static_cast<uint32_t>(GetCurrentProcessId()) * 131u) % 7000u;
    const uint32_t baseOffset = (pidBias + s_nextSeq.fetch_add(1u) * 17u) % 7000u;
    for (uint32_t offset = 0; offset < 4000u; ++offset) {
        const uint16_t candidate = static_cast<uint16_t>(42000u + ((baseOffset + offset) % 7000u));
        {
            ScopedSocket s(socket(AF_INET, sockType, proto));
            if (s.sock == INVALID_SOCKET) continue;
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(candidate);
            if (bind(s.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
                continue;
            }
        }
        if (TryBindLoopbackReuseProbe(sockType, proto, candidate)) {
            return candidate;
        }
    }
    uint16_t assignedPort = 0;
    {
        ScopedSocket s(socket(AF_INET, sockType, proto));
        if (s.sock == INVALID_SOCKET) return 0;
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(s.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            return 0;
        }
        int len = static_cast<int>(sizeof(addr));
        if (getsockname(s.sock, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
            return 0;
        }
        assignedPort = ntohs(addr.sin_port);
    }
    return WaitPortReuseBindable(sockType, proto, assignedPort, 200) ? assignedPort : 0;
}

static SOCKET OpenLoopbackTcpListener(uint16_t* outPort) {
    if (outPort != nullptr) *outPort = 0;
    ScopedSocket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s.sock == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        return INVALID_SOCKET;
    }
    if (listen(s.sock, 2) == SOCKET_ERROR) {
        return INVALID_SOCKET;
    }
    int len = static_cast<int>(sizeof(addr));
    if (getsockname(s.sock, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        return INVALID_SOCKET;
    }
    if (outPort != nullptr) *outPort = ntohs(addr.sin_port);
    return s.Release();
}

static SOCKET AcceptLoopbackOne(SOCKET listenSock, int timeoutMs) {
    if (listenSock == INVALID_SOCKET) return INVALID_SOCKET;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 1);
    while (GetTickCount64() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenSock, &rfds);
        const ULONGLONG rem = deadline - GetTickCount64();
        const long sliceMs = (rem > 50ULL) ? 50L : static_cast<long>(rem);
        timeval tv = {};
        tv.tv_sec = sliceMs / 1000L;
        tv.tv_usec = (sliceMs % 1000L) * 1000L;
        const int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(listenSock, &rfds)) {
            sockaddr_in peer = {};
            int peerLen = static_cast<int>(sizeof(peer));
            return accept(listenSock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        }
        if (sel == SOCKET_ERROR) {
            return INVALID_SOCKET;
        }
    }
    return INVALID_SOCKET;
}

static bool ConnectLoopbackWithAccept(ScopedSocket& listener, const char* host, uint16_t port,
                                      int timeoutMs, SOCKET* outServer, SOCKET* outClient) {
    if (outServer != nullptr) *outServer = INVALID_SOCKET;
    if (outClient != nullptr) *outClient = INVALID_SOCKET;
    if (listener.sock == INVALID_SOCKET || host == nullptr || port == 0) return false;

    const SOCKET listenFd = listener.sock;
    std::atomic<SOCKET> accepted{ INVALID_SOCKET };
    std::thread acceptThread([listenFd, timeoutMs, &accepted]() {
        accepted.store(AcceptLoopbackOne(listenFd, timeoutMs), std::memory_order_release);
    });

    SOCKET client = NetworkConnect(host, port, timeoutMs, timeoutMs, nullptr);
    if (client == INVALID_SOCKET) {
        listener.Reset();
    }
    acceptThread.join();
    SOCKET server = accepted.load(std::memory_order_acquire);
    if (client == INVALID_SOCKET || server == INVALID_SOCKET) {
        if (client != INVALID_SOCKET) NetworkCloseSocket(client);
        if (server != INVALID_SOCKET) closesocket(server);
        return false;
    }
    DWORD ioMs = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 3000);
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ioMs), sizeof(ioMs));
    setsockopt(server, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ioMs), sizeof(ioMs));
    if (outServer != nullptr) *outServer = server;
    else closesocket(server);
    if (outClient != nullptr) *outClient = client;
    else NetworkCloseSocket(client);
    return true;
}

static bool CreateLoopbackTcpPair(SOCKET* outServer, SOCKET* outClient) {
    *outServer = INVALID_SOCKET;
    *outClient = INVALID_SOCKET;
    uint16_t port = 0;
    ScopedSocket listener(OpenLoopbackTcpListener(&port));
    if (listener.sock == INVALID_SOCKET || port == 0) return false;
    return ConnectLoopbackWithAccept(listener, "127.0.0.1", port, 3000, outServer, outClient);
}

static LanProbePacket MakeProbePacket(uint16_t proto, uint32_t nonce, uint32_t pid) {
    LanProbePacket p = {};
    std::memcpy(p.magic, "RPMPPRB1", 8);
    p.protocol = proto;
    p.reserved = 0;
    p.clientNonce = nonce;
    p.clientPid = pid;
    return p;
}

static LanAnnouncePacket MakeAnnouncePacket(uint64_t guid, uint32_t pid, uint16_t tcpPort,
                                            uint8_t open, const char* hostName, const char* mapName,
                                            uint16_t proto = REPUBLICMP_PROTOCOL, uint8_t mode = 1,
                                            uint8_t steamOn = 0) {
    LanAnnouncePacket a = {};
    std::memcpy(a.magic, "RPMPANN1", 8);
    a.protocol = proto;
    a.tcpPort = tcpPort;
    a.serverGuid = guid;
    a.hostPid = pid;
    a.open = open;
    a.twoStatesMode = mode;
    a.hasPassword = 0;
    a.steamOnline = steamOn;
    a.dllCrc = SaveSync_GetLocalDllCrc();
    std::strncpy(a.version, REPUBLICMP_VERSION, sizeof(a.version) - 1);
    if (hostName != nullptr) std::strncpy(a.hostName, hostName, sizeof(a.hostName) - 1);
    if (mapName != nullptr) std::strncpy(a.mapName, mapName, sizeof(a.mapName) - 1);
    return a;
}

static SOCKET OpenBoundUdpLoopback(uint16_t port = 0, DWORD rcvTimeoutMs = 400) {
    ScopedSocket s(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s.sock == INVALID_SOCKET) return INVALID_SOCKET;
    BOOL reuse = TRUE;
    (void)setsockopt(s.sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    (void)setsockopt(s.sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeoutMs), sizeof(rcvTimeoutMs));
    BOOL newBehavior = FALSE;
    DWORD bytesReturned = 0;
    (void)WSAIoctl(s.sock, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior),
                   nullptr, 0, &bytesReturned, nullptr, nullptr);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(s.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        return INVALID_SOCKET;
    }
    return s.Release();
}

static bool SendUdpLoopback(SOCKET s, uint16_t port, const void* data, size_t len) {
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(port);
    const int sent = sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                            reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    return sent == static_cast<int>(len);
}

static void DrainUdpSocket(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    std::vector<uint8_t> discard(512, 0);
    for (int i = 0; i < 64; ++i) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv = { 0, 0 };
        if (select(0, &rfds, nullptr, nullptr, &tv) <= 0 || !FD_ISSET(s, &rfds)) {
            break;
        }
        sockaddr_in from = {};
        int fromLen = static_cast<int>(sizeof(from));
        if (recvfrom(s, reinterpret_cast<char*>(discard.data()),
                     static_cast<int>(discard.size()), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLen) <= 0) {
            break;
        }
    }
}

static bool SendProbeAndAwaitAnnounce(SOCKET udpSock, uint16_t targetPort, const LanProbePacket& probe,
                                      const char* expectedHostName, LanAnnouncePacket* outPkt,
                                      uint8_t expectOpen = 1, int maxAttempts = 12) {
    std::vector<uint8_t> wire(sizeof(LanProbePacket));
    const size_t n = LanDiscovery_SerializeProbe(probe, wire.data(), wire.size());
    if (n != sizeof(LanProbePacket)) return false;

    const int totalMs = (maxAttempts <= 3) ? (maxAttempts * 80) : (maxAttempts * 125);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(totalMs > 0 ? totalMs : 80);
    while (GetTickCount64() < deadline) {
        if (!SendUdpLoopback(udpSock, targetPort, wire.data(), n)) return false;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const ULONGLONG rem = deadline - now;
        const long sliceMs = (rem > 80ULL) ? 80L : static_cast<long>(rem);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udpSock, &rfds);
        timeval tv = {};
        tv.tv_sec = sliceMs / 1000L;
        tv.tv_usec = (sliceMs % 1000L) * 1000L;
        const int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(udpSock, &rfds)) {
            for (int reads = 0; reads < 8; ++reads) {
                fd_set innerRfds;
                FD_ZERO(&innerRfds);
                FD_SET(udpSock, &innerRfds);
                timeval zeroTv = { 0, 0 };
                if (select(0, &innerRfds, nullptr, nullptr, &zeroTv) <= 0 || !FD_ISSET(udpSock, &innerRfds)) {
                    break;
                }
                std::vector<uint8_t> buf(512, 0);
                sockaddr_in from = {};
                int fromLen = static_cast<int>(sizeof(from));
                const int r = recvfrom(udpSock, reinterpret_cast<char*>(buf.data()),
                                       static_cast<int>(buf.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (r <= 0) break;
                LanAnnouncePacket ann = {};
                if (LanDiscovery_ParseAnnounce(buf.data(), static_cast<size_t>(r), &ann) &&
                    ann.open == expectOpen) {
                    if (expectedHostName == nullptr || expectedHostName[0] == '\0' ||
                        std::strcmp(ann.hostName, expectedHostName) == 0) {
                        if (outPkt != nullptr) *outPkt = ann;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool FindHostByName(const std::vector<LanHostEntry>& hosts, const char* name, LanHostEntry* outEntry) {
    for (const auto& h : hosts) {
        if (std::strcmp(h.hostName, name) == 0) {
            if (outEntry != nullptr) *outEntry = h;
            return true;
        }
    }
    return false;
}

static bool FindHostByGuid(const std::vector<LanHostEntry>& hosts, uint64_t guid, LanHostEntry* outEntry) {
    for (const auto& h : hosts) {
        if (h.serverGuid == guid) {
            if (outEntry != nullptr) *outEntry = h;
            return true;
        }
    }
    return false;
}

static bool PollForHostByName(const char* name, int timeoutMs, LanHostEntry* outEntry, bool requireLoopback = false) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 1);
    ULONGLONG lastProbeMs = 0;
    while (GetTickCount64() < deadline) {
        const ULONGLONG now = GetTickCount64();
        if (lastProbeMs == 0 || (now - lastProbeMs) >= 80ULL) {
            LanDiscovery_ProbeNow();
            lastProbeMs = now;
        }
        const std::vector<LanHostEntry> hosts = LanDiscovery_GetHosts();
        LanHostEntry tmp = {};
        if (FindHostByName(hosts, name, &tmp)) {
            if (!requireLoopback || std::strcmp(tmp.connectIp, "127.0.0.1") == 0) {
                if (outEntry != nullptr) *outEntry = tmp;
                return true;
            }
        }
        Sleep(25);
    }
    return false;
}

static bool PollForHostGone(uint64_t serverGuid, int timeoutMs = 2500) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 1);
    while (GetTickCount64() < deadline) {
        if (!FindHostByGuid(LanDiscovery_GetHosts(), serverGuid, nullptr)) {
            return true;
        }
        Sleep(20);
    }
    return !FindHostByGuid(LanDiscovery_GetHosts(), serverGuid, nullptr);
}

static bool WaitForSessionListening(int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 1);
    while (GetTickCount64() < deadline) {
        if (SaveSync_GetState() == SESSION_HOST_LISTENING) return true;
        Sleep(10);
    }
    return SaveSync_GetState() == SESSION_HOST_LISTENING;
}

static bool WaitForSessionIdle(int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (GetTickCount64() < deadline) {
        if (SaveSync_GetState() == SESSION_IDLE) return true;
        Sleep(25);
    }
    return SaveSync_GetState() == SESSION_IDLE;
}

struct ScopedSessionDisconnect {
    bool restoreExternalEvent = false;
    explicit ScopedSessionDisconnect(bool restoreEvent = false)
        : restoreExternalEvent(restoreEvent) {}

    void RestoreExternalEventNow() {
        if (!restoreExternalEvent) return;
        restoreExternalEvent = false;
        for (int i = 0; i < 50 && IsLocalHostOpenEventSignaled(); ++i) {
            Sleep(2);
        }
        HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, kLocalHostOpenEventName);
        if (h != nullptr) {
            (void)SetEvent(h);
            (void)CloseHandle(h);
        }
    }

    ~ScopedSessionDisconnect() {
        if (SaveSync_GetState() != SESSION_IDLE) {
            SaveSync_RequestDisconnect();
            (void)WaitForSessionIdle(1500);
        }
        TwoStates_OnSessionEnded();
        RestoreExternalEventNow();
    }

    ScopedSessionDisconnect(const ScopedSessionDisconnect&) = delete;
    ScopedSessionDisconnect& operator=(const ScopedSessionDisconnect&) = delete;
};

static bool CreateWorkshopDir(const std::filesystem::path& dir, bool createIni) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    if (!createIni) return true;
    FILE* f = nullptr;
    const std::filesystem::path ini = dir / "workshopconfig.ini";
    if (fopen_s(&f, ini.string().c_str(), "wb") != 0 || f == nullptr) return false;
    const char body[] = "$OBJECT_BUILDING workshop_test\r\n";
    const size_t wrote = std::fwrite(body, 1, sizeof(body) - 1, f);
    std::fclose(f);
    return wrote == sizeof(body) - 1;
}

static bool WriteHeaderBinWithMods(const std::filesystem::path& saveDir, const std::vector<uint64_t>& ids) {
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    if (ec) return false;

    const size_t totalBytes = static_cast<size_t>(HEADER_BIN_DATA_OFFSET) +
                              ids.size() * static_cast<size_t>(HEADER_BIN_RECORD_SIZE);
    std::vector<uint8_t> buf(totalBytes, 0);
    for (size_t i = 0; i < ids.size(); ++i) {
        char* rec = reinterpret_cast<char*>(buf.data() + HEADER_BIN_DATA_OFFSET + i * HEADER_BIN_RECORD_SIZE);
        _snprintf_s(rec, HEADER_BIN_RECORD_SIZE, _TRUNCATE, "workshop/%llu/building.ini",
                    static_cast<unsigned long long>(ids[i]));
    }

    if (!ids.empty()) {
        std::vector<uint64_t> parsed(ids.size(), 0);
        int count = 0;
        ModSync_ExtractHeaderBinModIdsFromBuffer(buf.data(), buf.size(), parsed.data(),
                                                 static_cast<int>(parsed.size()), &count);
        if (count != static_cast<int>(ids.size())) {
            return false;
        }
    }

    FILE* f = nullptr;
    const std::filesystem::path hdr = saveDir / "header.bin";
    if (fopen_s(&f, hdr.string().c_str(), "wb") != 0 || f == nullptr) return false;
    const size_t wrote = std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return wrote == buf.size();
}

static bool IsClipboardOpenable() {
    for (int i = 0; i < 25; ++i) {
        if (OpenClipboard(nullptr)) {
            CloseClipboard();
            return true;
        }
        Sleep(20);
    }
    return false;
}

static bool RetryCopyToClipboard(const char* text) {
    for (int i = 0; i < 25; ++i) {
        if (UiCommon_CopyTextToClipboard(text)) return true;
        Sleep(20);
    }
    return false;
}

static bool ReadClipboardFormats(std::string& outUniUtf8, std::string& outAnsiText) {
    outUniUtf8.clear();
    outAnsiText.clear();
    for (int i = 0; i < 25; ++i) {
        if (OpenClipboard(nullptr)) {
            bool hasUni = false;
            HANDLE hUni = GetClipboardData(CF_UNICODETEXT);
            if (hUni != nullptr) {
                const SIZE_T maxWchars = GlobalSize(hUni) / sizeof(wchar_t);
                const wchar_t* wp = static_cast<const wchar_t*>(GlobalLock(hUni));
                if (wp != nullptr && maxWchars > 0) {
                    const size_t wlen = wcsnlen(wp, maxWchars);
                    if (wlen > 0) {
                        const int need = WideCharToMultiByte(CP_UTF8, 0, wp, static_cast<int>(wlen),
                                                             nullptr, 0, nullptr, nullptr);
                        if (need > 0) {
                            outUniUtf8.resize(static_cast<size_t>(need));
                            WideCharToMultiByte(CP_UTF8, 0, wp, static_cast<int>(wlen),
                                                outUniUtf8.data(), need, nullptr, nullptr);
                            hasUni = true;
                        }
                    }
                    GlobalUnlock(hUni);
                }
            }

            bool hasAnsi = false;
            HANDLE hAnsi = GetClipboardData(CF_TEXT);
            if (hAnsi != nullptr) {
                const SIZE_T maxBytes = GlobalSize(hAnsi);
                const char* ap = static_cast<const char*>(GlobalLock(hAnsi));
                if (ap != nullptr && maxBytes > 0) {
                    const size_t alen = strnlen(ap, maxBytes);
                    outAnsiText.assign(ap, alen);
                    GlobalUnlock(hAnsi);
                    hasAnsi = true;
                }
            }
            CloseClipboard();
            if (hasUni && hasAnsi) return true;
        }
        Sleep(20);
    }
    return false;
}

static bool ReadClipboardText(std::string& outText) {
    outText.clear();
    std::string clipUni;
    std::string clipAnsi;
    if (!ReadClipboardFormats(clipUni, clipAnsi) || clipUni != clipAnsi) {
        return false;
    }
    outText = clipUni;
    return true;
}

static bool VerifyClipboardRoundTrip(const char* expectedText) {
    if (expectedText == nullptr) return false;
    if (!IsClipboardOpenable()) return true;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (RetryCopyToClipboard(expectedText)) {
            std::string clipUni;
            std::string clipAnsi;
            if (ReadClipboardFormats(clipUni, clipAnsi) &&
                clipUni == expectedText &&
                clipAnsi == expectedText) {
                return true;
            }
        }
        Sleep(20);
    }
    return false;
}

static bool VerifyLoopbackUdpSocketBound(uint16_t port) {
    if (port == 0) return false;
    ULONG sz = 0;
    const DWORD pre = GetExtendedUdpTable(nullptr, &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (pre != ERROR_INSUFFICIENT_BUFFER || sz < sizeof(MIB_UDPTABLE_OWNER_PID)) {
        return false;
    }
    std::vector<uint8_t> buf(sz, 0);
    if (GetExtendedUdpTable(buf.data(), &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) {
        return false;
    }
    const auto* tbl = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buf.data());
    const DWORD selfPid = GetCurrentProcessId();
    const DWORD loopbackNet = htonl(INADDR_LOOPBACK);
    int matched = 0;
    for (DWORD i = 0; i < tbl->dwNumEntries; ++i) {
        const MIB_UDPROW_OWNER_PID& row = tbl->table[i];
        if (row.dwOwningPid == selfPid &&
            ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
            if (row.dwLocalAddr != loopbackNet) {
                return false;
            }
            ++matched;
        }
    }
    return matched > 0;
}

static int MeasureIdleOverlayVertexDelta() {
    ScopedImGuiFrame frame;
    if (!frame.inFrame) return -1;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (bg == nullptr || fg == nullptr) return -1;
    const int before = bg->VtxBuffer.Size + fg->VtxBuffer.Size;
    TwoStates_DrawOverlay();
    const int after = bg->VtxBuffer.Size + fg->VtxBuffer.Size;
    return after - before;
}

// ---------------------------------------------------------------------------
// Parameterized Behavioral Helpers (Dryer / Single-Responsibility)
// ---------------------------------------------------------------------------

static bool CheckAdapter(uint32_t ifType, const char* name, const char* desc, const char* ip,
                         uint8_t prefixLen, LanAdapterKind expectedKind, int minPrio,
                         const char* expectedMask, const char* expectedBcast, int* outPrio = nullptr) {
    int prio = 0;
    const LanAdapterKind kind = LanDiscovery_ClassifyAdapter(ifType, name, desc, ip, &prio);
    LAN_CHECK(kind == expectedKind);
    LAN_CHECK(prio > minPrio);
    char mask[16] = {};
    char bcast[16] = {};
    LAN_CHECK(LanDiscovery_ComputeBroadcast(ip, prefixLen, mask, sizeof(mask), bcast, sizeof(bcast)));
    if (expectedMask != nullptr) {
        LAN_CHECK(std::strcmp(mask, expectedMask) == 0);
    }
    if (expectedBcast != nullptr) {
        LAN_CHECK(std::strcmp(bcast, expectedBcast) == 0);
    }
    if (outPrio != nullptr) {
        *outPrio = prio;
    }
    return true;
}

static bool StartResponder(uint16_t* outPort, uint8_t mode, const char* hostName,
                           const char* mapName, uint16_t tcpPort = SAVE_SYNC_PORT) {
    if (outPort == nullptr) return false;
    const uint16_t port = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(port != 0);
    *outPort = port;
    LanDiscovery_SetPortForTest(port);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, tcpPort, mode, hostName, mapName);
    return true;
}

static bool ProbeHost(uint16_t port, uint32_t nonce, uint32_t pid,
                      const char* expectedName, LanAnnouncePacket* outReply) {
    ScopedSocket cli(OpenBoundUdpLoopback(0, 400));
    LAN_CHECK(cli.sock != INVALID_SOCKET);
    return SendProbeAndAwaitAnnounce(cli.sock, port,
                                     MakeProbePacket(REPUBLICMP_PROTOCOL, nonce, pid),
                                     expectedName, outReply, 1u);
}

static bool StartHostScan(uint16_t tcpPort, uint32_t pidXorMask, const char* hostName,
                          const char* mapName, uint16_t* outUdpPort = nullptr) {
    const uint16_t port = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(port != 0);
    if (outUdpPort != nullptr) {
        *outUdpPort = port;
    }
    LanDiscovery_SetPortForTest(port);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ pidXorMask, true);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, tcpPort, 1u, hostName, mapName);
    LanDiscovery_StartScan();
    return true;
}

static bool CheckDedup(uint64_t guid, uint32_t pid, const char* name,
                       const char* const* ips, const LanAdapterKind* kinds,
                       size_t count, const char* winnerIp, bool sameMachine) {
    if (ips == nullptr || kinds == nullptr || winnerIp == nullptr || count == 0) return false;
    const LanAnnouncePacket pkt = MakeAnnouncePacket(guid, pid, SAVE_SYNC_PORT, 1u, name, "M");
    std::vector<LanHostEntry> hosts;
    for (size_t i = 0; i < count; ++i) {
        LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, ips[i], 1000ULL + static_cast<uint64_t>(i) * 10ULL, kinds[i]));
    }
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(std::strcmp(hosts[0].connectIp, winnerIp) == 0);
    LAN_CHECK(hosts[0].sameMachine == sameMachine);
    return true;
}

static bool CheckNorm(const char* raw, uint16_t initialPort, const char* expectedHost, uint16_t expectedPort) {
    char host[64] = {};
    uint16_t port = initialPort;
    LAN_CHECK(NetworkNormalizeTargetAddress(raw, host, sizeof(host), &port));
    LAN_CHECK(std::strcmp(host, expectedHost) == 0);
    LAN_CHECK(port == expectedPort);
    return true;
}

static bool CheckSteamNorm(const char* raw, uint64_t expectedSteamId) {
    char host[64] = {};
    uint16_t port = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress(raw, host, sizeof(host), &port));
    LAN_CHECK(SteamP2P_ParseTargetSteamId(host) == expectedSteamId);
    return true;
}

static bool CheckNormBad(const char* const* inputs, size_t count) {
    if (inputs == nullptr) return false;
    char host[64] = {};
    uint16_t port = 0;
    for (size_t i = 0; i < count; ++i) {
        LAN_CHECK(!NetworkNormalizeTargetAddress(inputs[i], host, sizeof(host), &port));
    }
    return true;
}

static bool ConnectNorm(const char* rawFmt, SOCKET* outSrv, SOCKET* outCli) {
    uint16_t port = 0;
    ScopedSocket listener(OpenLoopbackTcpListener(&port));
    LAN_CHECK(listener.sock != INVALID_SOCKET && port != 0);

    char raw[64] = {};
    _snprintf_s(raw, sizeof(raw), _TRUNCATE, rawFmt, static_cast<unsigned int>(port));
    char host[64] = {};
    uint16_t outPort = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress(raw, host, sizeof(host), &outPort));
    LAN_CHECK(std::strcmp(host, "127.0.0.1") == 0);
    LAN_CHECK(outPort == port);

    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(ConnectLoopbackWithAccept(listener, host, outPort, 2000, &srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);
    char peerIp[64] = {};
    NetworkGetPeerName(cliGuard.sock, peerIp, sizeof(peerIp));
    LAN_CHECK(std::strcmp(peerIp, "127.0.0.1") == 0);
    if (outSrv != nullptr) *outSrv = srvGuard.Release();
    if (outCli != nullptr) *outCli = cliGuard.Release();
    return true;
}

static bool CheckModDir(const std::filesystem::path& exeDir, const std::filesystem::path& modDir,
                        uint64_t modId, bool createIni) {
    std::error_code ec;
    std::filesystem::create_directories(exeDir, ec);
    LAN_CHECK(CreateWorkshopDir(modDir, createIni));
    char out[MAX_PATH] = {};
    const bool found = ModSync_FindLocalModFolderAtRoot(modId, exeDir.string().c_str(), false, out, sizeof(out));
    if (createIni) {
        char idStr[32] = {};
        _snprintf_s(idStr, sizeof(idStr), _TRUNCATE, "%llu", static_cast<unsigned long long>(modId));
        LAN_CHECK(found);
        LAN_CHECK(std::strstr(out, idStr) != nullptr);
    } else {
        LAN_CHECK(!found);
    }
    return true;
}

static bool RunModSync(const std::filesystem::path& saveDir, ModSyncHostResult* outHostRes,
                       ModSyncClientResult* outCliRes, char* cliErrOut, size_t cliErrCap) {
    if (outHostRes == nullptr || outCliRes == nullptr) return false;
    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);

    ModSyncHostResult hRes = MOD_SYNC_HOST_ERROR;
    char hostErr[256] = {};
    std::thread hThread([&]() {
        hRes = ModSync_HostHandshake(srvGuard.sock, saveDir.string().c_str(), hostErr, sizeof(hostErr));
    });
    char localCliErr[256] = {};
    const ModSyncClientResult cRes = ModSync_ClientHandshake(cliGuard.sock, localCliErr, sizeof(localCliErr));
    hThread.join();

    if (cliErrOut != nullptr && cliErrCap > 0) {
        strncpy_s(cliErrOut, cliErrCap, localCliErr, _TRUNCATE);
    }
    *outHostRes = hRes;
    *outCliRes = cRes;
    return true;
}

static bool CancelSilentCli(int settleMs, int idleWaitMs, ULONGLONG* outElapsedMs = nullptr) {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    uint16_t port = 0;
    ScopedSocket silentSrv(OpenLoopbackTcpListener(&port));
    LAN_CHECK(silentSrv.sock != INVALID_SOCKET && port != 0);

    char addr[64] = {};
    _snprintf_s(addr, sizeof(addr), _TRUNCATE, "127.0.0.1:%u", static_cast<unsigned int>(port));
    SaveSync_StartClient(addr);
    Sleep(static_cast<DWORD>(settleMs));
    const ULONGLONG t0 = GetTickCount64();
    SaveSync_RequestDisconnect();
    LAN_CHECK(WaitForSessionIdle(idleWaitMs));
    if (outElapsedMs != nullptr) {
        *outElapsedMs = GetTickCount64() - t0;
    }
    TwoStates_OnSessionEnded();
    return true;
}

static bool CheckSyncBlockTwoFrames(bool mapInspectMode) {
    ScopedWorldLoadedFlag worldGuard;
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    SyncBlock_SetActive(false, nullptr);
    SyncBlock_SetMapInspectMode(mapInspectMode);
    if (mapInspectMode) {
        LAN_CHECK(SyncBlock_IsMapInspectMode());
        LAN_CHECK(!SyncBlock_BlocksWorldInput());
        LAN_CHECK(SyncBlock_UpdateMapInspectState(static_cast<int>(SESSION_HOST_LISTENING), false));
    }
    {
        ScopedImGuiFrame frame;
        LAN_CHECK(frame.inFrame);
        SyncBlock_Draw();
        LAN_CHECK(frame.NextFrame());
        SyncBlock_Draw();
        LAN_CHECK(frame.RenderAndCountVertices() > 0);
    }
    SyncBlock_SetMapInspectMode(false);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    return true;
}

// ---------------------------------------------------------------------------
// Tier 1: Feature Coverage (55 Test Cases: 5 per Feature F1..F11)
// ---------------------------------------------------------------------------

// --- F1: Multi-NIC & VPN Adapter Enumeration ---

static bool T1_F1_01_ClassifyEthernetAndBroadcast() {
    return CheckAdapter(kIfEthernet, "Ethernet", "Intel(R) Ethernet Connection", "192.168.1.42", 24,
                        LanAdapterKind::Ethernet, 0, "255.255.255.0", "192.168.1.255");
}

static bool T1_F1_02_ClassifyWifiAndSlash16Broadcast() {
    int prioEth = 0;
    int prioWifi = 0;
    (void)LanDiscovery_ClassifyAdapter(kIfEthernet, "Ethernet", "Intel NIC", "192.168.1.10", &prioEth);
    LAN_CHECK(CheckAdapter(kIfWifi, "Wi-Fi", "Intel(R) Wi-Fi 6", "10.0.5.17", 16,
                           LanAdapterKind::WiFi, 0, "255.255.0.0", "10.0.255.255", &prioWifi));
    LAN_CHECK(prioEth > prioWifi);
    return true;
}

static bool T1_F1_03_ClassifyRadminVpn() {
    return CheckAdapter(kIfPropVirt, "Radmin VPN", "Famatech Radmin VPN Ethernet Adapter", "26.144.82.10", 8,
                        LanAdapterKind::VirtualVpn, 50, "255.0.0.0", "26.255.255.255");
}

static bool T1_F1_04_ClassifyHamachiAndZeroTier() {
    LAN_CHECK(CheckAdapter(kIfPropVirt, "Hamachi", "LogMeIn Hamachi Virtual Ethernet", "25.41.12.9", 8,
                           LanAdapterKind::VirtualVpn, 50, "255.0.0.0", "25.255.255.255"));
    return CheckAdapter(kIfEthernet, "ZeroTier One [abc]", "ZeroTier Virtual Port", "172.24.10.5", 16,
                        LanAdapterKind::VirtualVpn, 50, "255.255.0.0", "172.24.255.255");
}

static bool T1_F1_05_GetLocalAdaptersConsistency() {
    const std::vector<LanAdapterInfo> a = LanDiscovery_GetLocalAdapters(true);
    const std::vector<LanAdapterInfo> b = LanDiscovery_GetLocalAdapters(false);
    LAN_CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        in_addr parsed = {};
        LAN_CHECK(inet_pton(AF_INET, a[i].ip, &parsed) == 1);
        LAN_CHECK(std::strncmp(a[i].ip, "127.", 4) != 0);
        LAN_CHECK(std::strncmp(a[i].ip, "169.254.", 8) != 0);
        LAN_CHECK(std::strcmp(a[i].ip, "0.0.0.0") != 0);
        if (i + 1 < a.size()) {
            LAN_CHECK(a[i].priority >= a[i + 1].priority);
        }
    }
    return true;
}

// --- F2: LAN Discovery Wire Protocol & Pure Store ---

static bool T1_F2_01_ProbeSerializeParseRoundTrip() {
    const LanProbePacket in = MakeProbePacket(REPUBLICMP_PROTOCOL, 0x1234ABCDu, 4242u);
    std::vector<uint8_t> buf(64, 0);
    const size_t n = LanDiscovery_SerializeProbe(in, buf.data(), buf.size());
    LAN_CHECK(n == sizeof(LanProbePacket));
    LanProbePacket out = {};
    LAN_CHECK(LanDiscovery_ParseProbe(buf.data(), n, &out));
    LAN_CHECK(std::memcmp(out.magic, "RPMPPRB1", 8) == 0);
    LAN_CHECK(out.protocol == REPUBLICMP_PROTOCOL);
    LAN_CHECK(out.clientNonce == 0x1234ABCDu);
    LAN_CHECK(out.clientPid == 4242u);
    return true;
}

static bool T1_F2_02_AnnounceSerializeParseRoundTrip() {
    const LanAnnouncePacket in = MakeAnnouncePacket(0xCAFEBABE12345678ULL, 9001u, 5556u, 1u, "HostPC", "Siberia");
    std::vector<uint8_t> buf(256, 0);
    const size_t n = LanDiscovery_SerializeAnnounce(in, buf.data(), buf.size());
    LAN_CHECK(n == sizeof(LanAnnouncePacket));
    LanAnnouncePacket out = {};
    LAN_CHECK(LanDiscovery_ParseAnnounce(buf.data(), n, &out));
    LAN_CHECK(std::memcmp(out.magic, "RPMPANN1", 8) == 0);
    LAN_CHECK(out.open == 1u);
    LAN_CHECK(out.tcpPort == 5556u);
    LAN_CHECK(out.serverGuid == 0xCAFEBABE12345678ULL);
    LAN_CHECK(out.hostPid == 9001u);
    LAN_CHECK(std::strcmp(out.hostName, "HostPC") == 0);
    LAN_CHECK(std::strcmp(out.mapName, "Siberia") == 0);
    return true;
}

static bool T1_F2_03_UpsertHostInsertsEntry() {
    std::vector<LanHostEntry> hosts;
    const LanAnnouncePacket pkt = MakeAnnouncePacket(0x1111ULL, 9002u, 5556u, 1u, "FactoryHost", "Volga");
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "192.168.1.55", 1000ULL, LanAdapterKind::Ethernet));
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(std::strcmp(hosts[0].connectIp, "192.168.1.55") == 0);
    LAN_CHECK(hosts[0].protocolMatch);
    LAN_CHECK(!hosts[0].sameMachine);
    LAN_CHECK(hosts[0].lastSeenMs == 1000ULL);
    return true;
}

static bool T1_F2_04_PruneExpiredRemovesOnlyOld() {
    std::vector<LanHostEntry> hosts;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, MakeAnnouncePacket(1ULL, 100u, 5556u, 1u, "Old", "M1"),
                                      "192.168.1.10", 1000ULL));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, MakeAnnouncePacket(2ULL, 101u, 5556u, 1u, "Fresh", "M2"),
                                      "192.168.1.11", 4000ULL));
    LanDiscovery_PruneExpired(hosts, 6000ULL, LAN_DISCOVERY_TTL_MS);
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(hosts[0].serverGuid == 2ULL);
    return true;
}

static bool T1_F2_05_ByeAnnounceRemovesImmediately() {
    std::vector<LanHostEntry> hosts;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, MakeAnnouncePacket(777ULL, 102u, 5556u, 1u, "Alive", "M1"),
                                      "192.168.1.20", 1000ULL));
    LAN_CHECK(hosts.size() == 1u);
    const LanAnnouncePacket bye = MakeAnnouncePacket(777ULL, 102u, 5556u, 0u, "Alive", "M1");
    (void)LanDiscovery_UpsertHost(hosts, bye, "192.168.1.20", 1200ULL);
    LAN_CHECK(hosts.empty());
    return true;
}

// --- F3: Host UDP Discovery Responder ---

static bool T1_F3_01_HostRepliesUnicastToProbe() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string hName = UniqueTag("T1F301");
    LAN_CHECK(StartResponder(&port, 1u, hName.c_str(), "MapA"));
    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(port, 0x101u, 999999u, hName.c_str(), &reply));
    LAN_CHECK(reply.protocol == REPUBLICMP_PROTOCOL);
    LAN_CHECK(reply.open == 1u);
    LAN_CHECK(reply.serverGuid != 0ULL);
    LAN_CHECK(reply.tcpPort == 5556u);
    LAN_CHECK(std::strcmp(reply.hostName, hName.c_str()) == 0);
    LAN_CHECK(std::strcmp(reply.mapName, "MapA") == 0);
    return true;
}

static bool T1_F3_02_HostSendsPeriodicBeacon() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const std::string hName = UniqueTag("T1F302");

    // Bind passive UDP listener directly on loopback port 0 without sending any LanProbePacket.
    ScopedSocket rx(OpenBoundUdpLoopback(0, 300));
    LAN_CHECK(rx.sock != INVALID_SOCKET);
    sockaddr_in boundAddr = {};
    int boundLen = static_cast<int>(sizeof(boundAddr));
    LAN_CHECK(getsockname(rx.sock, reinterpret_cast<sockaddr*>(&boundAddr), &boundLen) == 0);
    const uint16_t port = ntohs(boundAddr.sin_port);
    LAN_CHECK(port != 0);

    LanDiscovery_SetPortForTest(port);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "BeaconMap");

    int beaconCount = 0;
    const ULONGLONG deadline = GetTickCount64() + 4000ULL;
    while (GetTickCount64() < deadline && beaconCount < 2) {
        std::vector<uint8_t> buf(512, 0);
        sockaddr_in from = {};
        int fromLen = static_cast<int>(sizeof(from));
        const int r = recvfrom(rx.sock, reinterpret_cast<char*>(buf.data()),
                               static_cast<int>(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (r <= 0) continue;
        LanAnnouncePacket pkt = {};
        if (LanDiscovery_ParseAnnounce(buf.data(), static_cast<size_t>(r), &pkt) &&
            pkt.open == 1u &&
            std::strcmp(pkt.hostName, hName.c_str()) == 0) {
            LAN_CHECK(std::strcmp(pkt.mapName, "BeaconMap") == 0);
            LAN_CHECK(pkt.tcpPort == 5556u);
            ++beaconCount;
        }
    }
    LAN_CHECK(beaconCount >= 2);
    return true;
}

static bool T1_F3_03_SetHostingUpdatesMetadata() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string hName1 = UniqueTag("T1F303A");
    const std::string hName2 = UniqueTag("T1F303B");
    LAN_CHECK(StartResponder(&port, 1u, hName1.c_str(), "MapA"));
    LanAnnouncePacket before = {};
    LAN_CHECK(ProbeHost(port, 0x302u, 999997u, hName1.c_str(), &before));
    LAN_CHECK(before.twoStatesMode == 1u);

    LanDiscovery_SetHosting(true, 5577u, 0u, hName2.c_str(), "MapB");
    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(port, 0x303u, 999997u, hName2.c_str(), &reply));
    LAN_CHECK(reply.twoStatesMode == 0u);
    LAN_CHECK(reply.tcpPort == 5577u);
    LAN_CHECK(std::strcmp(reply.mapName, "MapB") == 0);
    return true;
}

static bool T1_F3_04_SetHostingFalseStopsOpenReplies() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string hName = UniqueTag("T1F304");
    LAN_CHECK(StartResponder(&port, 1u, hName.c_str(), "MapStop"));

    ScopedSocket cli(OpenBoundUdpLoopback(0, 250));
    LAN_CHECK(cli.sock != INVALID_SOCKET);
    LanAnnouncePacket first = {};
    LAN_CHECK(SendProbeAndAwaitAnnounce(cli.sock, port,
                                          MakeProbePacket(REPUBLICMP_PROTOCOL, 0x404u, 999996u),
                                          hName.c_str(), &first, 1u));
    LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    DrainUdpSocket(cli.sock);

    ScopedSocket cli2(OpenBoundUdpLoopback(0, 250));
    LAN_CHECK(cli2.sock != INVALID_SOCKET);
    LanAnnouncePacket after = {};
    const bool gotOpen = SendProbeAndAwaitAnnounce(cli2.sock, port,
                                                   MakeProbePacket(REPUBLICMP_PROTOCOL, 0x405u, 999996u),
                                                   hName.c_str(), &after, 1u, 3);
    LAN_CHECK(!gotOpen);
    return true;
}

static bool T1_F3_05_ShutdownAndReinitResumes() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string hName = UniqueTag("T1F305");
    LAN_CHECK(StartResponder(&port, 1u, hName.c_str(), "MapR1"));
    LanAnnouncePacket r1 = {};
    LAN_CHECK(ProbeHost(port, 0x504u, 999995u, hName.c_str(), &r1));
    LAN_CHECK(std::strcmp(r1.mapName, "MapR1") == 0);

    LanDiscovery_Shutdown();
    LAN_CHECK(LanDiscovery_GetHosts().empty());
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "MapR2");
    LanAnnouncePacket r2 = {};
    LAN_CHECK(ProbeHost(port, 0x505u, 999995u, hName.c_str(), &r2));
    LAN_CHECK(std::strcmp(r2.mapName, "MapR2") == 0);
    LAN_CHECK(r2.serverGuid != r1.serverGuid);
    return true;
}

// --- F4: Client UDP Discovery Scanner ---

static bool T1_F4_01_ScannerDiscoversHostViaAuthoritativeIp() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const std::string hName = UniqueTag("T1F401");
    LAN_CHECK(StartHostScan(5556u, 0x4444u, hName.c_str(), "ScanMap"));
    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &found, true));
    LAN_CHECK(std::strcmp(found.connectIp, "127.0.0.1") == 0);
    return true;
}

static bool T1_F4_02_ScannerPreservesMetadataAndPort() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const std::string hName = UniqueTag("T1F402");
    LAN_CHECK(StartHostScan(5577u, 0x5555u, hName.c_str(), "Donbas"));
    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &found));
    LAN_CHECK(found.tcpPort == 5577u);
    LAN_CHECK(std::strcmp(found.mapName, "Donbas") == 0);
    LAN_CHECK(found.protocolMatch);
    return true;
}

static bool T1_F4_03_HostAndScannerCoexistSameProcess() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const uint16_t port = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(port != 0);
    const std::string hName = UniqueTag("T1F403");
    LanDiscovery_SetPortForTest(port);
    LanDiscovery_SetSelfPidOverrideForTest(0, false);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "Coexist");
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();

    // Probe external responder first so ResponderThreadProc is proven live before asserting self-PID filtering.
    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(port, 0x4301u, 999990u, hName.c_str(), &reply));
    LAN_CHECK(std::strcmp(reply.mapName, "Coexist") == 0);
    LAN_CHECK(!FindHostByName(LanDiscovery_GetHosts(), hName.c_str(), nullptr));

    LanDiscovery_StopScan();
    LAN_CHECK(LanDiscovery_GetHosts().empty());
    LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    return true;
}

static bool T1_F4_04_StopScanIdempotentRestart() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const std::string hName = UniqueTag("T1F404");
    LAN_CHECK(StartHostScan(5556u, 0x4404u, hName.c_str(), "RestartScan"));

    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &found, true));

    LanDiscovery_StopScan();
    LanDiscovery_StopScan();
    (void)LanDiscovery_GetHosts();
    LanDiscovery_Shutdown();
    LAN_CHECK(LanDiscovery_GetHosts().empty());

    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "RestartScan");
    LanDiscovery_StartScan();
    LanHostEntry rediscovered = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &rediscovered, true));
    LAN_CHECK(std::strcmp(rediscovered.mapName, "RestartScan") == 0);

    LanDiscovery_StopScan();
    LanDiscovery_Shutdown();
    LAN_CHECK(LanDiscovery_GetHosts().empty());
    return true;
}

static bool T1_F4_05_ProtocolMatchFlagTrueForCurrentProtocol() {
    std::vector<LanHostEntry> hosts;
    const LanAnnouncePacket pkt = MakeAnnouncePacket(0x5050ULL, 8181u, 5556u, 1u, "ProtoHost", "M", REPUBLICMP_PROTOCOL);
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "192.168.1.33", 2000ULL));
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(hosts[0].protocol == REPUBLICMP_PROTOCOL);
    LAN_CHECK(hosts[0].protocolMatch);
    return true;
}

// --- F5: Target Address Normalization ---

static bool T1_F5_01_NormalizePlainIpv4() {
    LAN_CHECK(CheckNorm("192.168.1.25", SAVE_SYNC_PORT, "192.168.1.25", SAVE_SYNC_PORT));
    return CheckNorm("192.168.1.25", 0, "192.168.1.25", 0);
}

static bool T1_F5_02_NormalizeIpv4WithPort() {
    return CheckNorm("26.10.20.30:6000", 0, "26.10.20.30", 6000u);
}

static bool T1_F5_03_NormalizeLocalhostVariants() {
    LAN_CHECK(CheckNorm("127.0.0.1", SAVE_SYNC_PORT, "127.0.0.1", 5556u));
    return CheckNorm("127.0.0.1:5556", 0, "127.0.0.1", 5556u);
}

static bool T1_F5_04_NormalizeSteamTargets() {
    LAN_CHECK(CheckSteamNorm("steam:76561198012345678", 76561198012345678ULL));
    return CheckSteamNorm("76561198012345678", 76561198012345678ULL);
}

static bool T1_F5_05_NormalizeTrimAndLoopbackConnect() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    return ConnectNorm("  127.0.0.1:%u  ", nullptr, nullptr);
}

// --- F6: Steam Offline & Dual-Testbed Detection ---

static bool T1_F6_01_SteamAbsentReturnsFalseCleanly() {
    if (GetModuleHandleA("steam_api64.dll") == nullptr) {
        LAN_CHECK(!SteamP2P_Init());
        LAN_CHECK(!SteamP2P_IsAvailable());
        LAN_CHECK(!SteamP2P_IsLoggedOn());
        LAN_CHECK(SteamP2P_GetLocalSteamId() == 0ULL);
        const char* name = SteamP2P_GetLocalPersonaName();
        LAN_CHECK(name != nullptr && name[0] == '\0');
    } else {
        // If Steam is loaded in test environment, verify pointer & predicate coherence.
        LAN_CHECK(SteamP2P_GetLocalPersonaName() != nullptr);
    }
    return true;
}

static bool T1_F6_02_SteamOfflineNoOpsSafe() {
    SteamP2P_SetHostingPresence(true);
    SteamP2P_SetHostingPresence(false);
    std::vector<SteamFriendEntry> friends;
    SteamP2P_GetOnlineFriends(friends);
    if (!SteamP2P_IsAvailable()) {
        LAN_CHECK(friends.empty());
    }
    return true;
}

static bool T1_F6_03_DualTestbedConsistentInNormalEnv() {
    char envBuf[32] = {};
    const DWORD envLen = GetEnvironmentVariableA("REPUBLICMP_ROLE", envBuf, sizeof(envBuf));
    if (envLen == 0) {
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0 &&
            std::strstr(exePath, "test-host") == nullptr &&
            std::strstr(exePath, "test-client") == nullptr) {
            LAN_CHECK(!SaveSync_IsDualTestbed());
        }
    }
    LAN_CHECK(SaveSync_IsDualTestbed() == SaveSync_IsDualTestbed());
    return true;
}

static bool T1_F6_04_DualTestbedAndSteamParity() {
    const bool dual = SaveSync_IsDualTestbed();
    if (dual) {
        LAN_CHECK(!SteamP2P_Init());
        LAN_CHECK(!SteamP2P_IsAvailable());
    }
    if (GetModuleHandleA("steam_api64.dll") == nullptr) {
        LAN_CHECK(!SteamP2P_Init());
        LAN_CHECK(!SteamP2P_IsAvailable());
    }
    return true;
}

static bool T1_F6_05_LoggedOnAndAvailableInvariant() {
    const bool logged = SteamP2P_IsLoggedOn();
    const bool avail = SteamP2P_IsAvailable();
    LAN_CHECK(!avail || logged);
    LAN_CHECK(SteamP2P_IsLoggedOn() == logged);
    if (!logged) {
        LAN_CHECK(!avail);
        LAN_CHECK(SteamP2P_GetLocalSteamId() == 0ULL);
    } else {
        LAN_CHECK(SteamP2P_GetLocalPersonaName() != nullptr);
    }
    return true;
}

// --- F7: Steam-Optional ModSync & Disk Fallback ---

static bool T1_F7_01_FindLocalModInWorkshopWip() {
    ScopedTempDir tmp("f701");
    return CheckModDir(tmp.dir, tmp.dir / "media_soviet" / "workshop_wip" / "990001", 990001ULL, true);
}

static bool T1_F7_02_FindLocalModInSteamWorkshop784150() {
    ScopedTempDir tmp("f702");
    return CheckModDir(tmp.dir / "common" / "SovietRepublic",
                       tmp.dir / "workshop" / "content" / "784150" / "990002", 990002ULL, true);
}

static bool T1_F7_03_FindLocalModInMediaSovietWorkshop() {
    ScopedTempDir tmp("f703");
    return CheckModDir(tmp.dir, tmp.dir / "media_soviet" / "workshop" / "990003", 990003ULL, true);
}

static bool T1_F7_04_IsSteamUgcOnlineNonBlocking() {
    if (GetModuleHandleA("steam_api64.dll") == nullptr) {
        LAN_CHECK(!ModSync_IsSteamUgcOnline());
    } else {
        (void)ModSync_IsSteamUgcOnline();
    }
    return true;
}

static bool T1_F7_05_LoopbackModSyncEmptyManifestOffline() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedTempDir tmp("f705");
    ScopedModPackMode packGuard(MOD_PACK_SAVE_ONLY);
    ScopedModSyncGuard modGuard;
    ModSyncHostResult hostRes = MOD_SYNC_HOST_ERROR;
    ModSyncClientResult cliRes = MOD_SYNC_CLIENT_ERROR;
    LAN_CHECK(RunModSync(tmp.dir, &hostRes, &cliRes, nullptr, 0));
    LAN_CHECK(hostRes == MOD_SYNC_HOST_OK);
    LAN_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
    LAN_CHECK(!ModSync_IsDownloadActive());
    return true;
}

// --- F8: Steam-Free Session Lifecycle & Identity ---

static bool T1_F8_01_LocalDisplayNameFallbackNonEmpty() {
    const std::string name = LanDiscovery_GetLocalDisplayName();
    LAN_CHECK(!name.empty());
    LAN_CHECK(name.size() < 48u);
    return true;
}

static bool T1_F8_02_HostSessionStartAndDisconnect() {
    ScopedWorldLoadedFlag worldGuard;
    ScopedDiscoveryGuard discGuard;
    const uint16_t udpPort = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    const uint16_t tcpPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(udpPort != 0 && tcpPort != 0);
    LanDiscovery_SetPortForTest(udpPort);
    SaveSync_SetTcpPortForTest(tcpPort);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    ScopedSessionDisconnect disconnectGuard(false);
    SaveSync_StartHost();
    LAN_CHECK(WaitForSessionListening(1000));
    LAN_CHECK(SaveSync_GetState() == SESSION_HOST_LISTENING);
    LAN_CHECK(SaveSync_IsHostRole());
    LAN_CHECK(SaveSync_IsHostAwaitingClient());
    LAN_CHECK(IsLocalHostOpenEventSignaled(tcpPort));
    SaveSync_RequestDisconnect();
    const bool idleOk = WaitForSessionIdle(5000);
    LAN_CHECK(idleOk);
    for (int i = 0; i < 50 && IsLocalHostOpenEventSignaled(tcpPort); ++i) {
        Sleep(2);
    }
    LAN_CHECK(!IsLocalHostOpenEventSignaled(tcpPort));
    LAN_CHECK(!SaveSync_IsLocalPcHostWaiting());
    TwoStates_OnSessionEnded();
    return true;
}

static bool T1_F8_03_ClientSessionCancelTransitionsToIdle() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    return CancelSilentCli(80, 3000);
}

static bool T1_F8_04_LocalIdentityVersionAndTargetIp() {
    LAN_CHECK(std::strcmp(SaveSync_GetLocalVersion(), REPUBLICMP_VERSION) == 0);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    ScopedSessionDisconnect disconnectGuard;
    uint16_t closedPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    if (closedPort == 0) closedPort = 59998u;
    char addr[64] = {};
    _snprintf_s(addr, sizeof(addr), _TRUNCATE, "127.0.0.1:%u", static_cast<unsigned int>(closedPort));
    SaveSync_StartClient(addr);
    LAN_CHECK(WaitForSessionIdle(5000));
    LAN_CHECK(std::strstr(SaveSync_GetTargetIp(), "127.0.0.1") != nullptr);
    TwoStates_OnSessionEnded();
    return true;
}

static bool T1_F8_05_TestSessionStateBusyAndAwaitingPredicates() {
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    LAN_CHECK(SaveSync_IsHostAwaitingClient());
    LAN_CHECK(SaveSync_IsBusy());
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    LAN_CHECK(!SaveSync_IsHostAwaitingClient());
    LAN_CHECK(!SaveSync_IsBusy());
    return true;
}

// --- F9: Shared Clipboard & Main Menu Toast Fix ---

static bool T1_F9_01_CopyTextToClipboardRoundTrip() {
    LAN_CHECK(VerifyClipboardRoundTrip("192.168.1.50:5556"));
    return true;
}

static bool T1_F9_02_ToastRendersInSessionIdle() {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    for (int i = 0; i < 5; ++i) {
        TwoStates_AddToast(".", 0);
    }
    TwoStates_OnSessionEnded();
    const int baseDelta = MeasureIdleOverlayVertexDelta();

    TwoStates_AddToast("LAN host address copied: 192.168.1.50:5556", 1);
    const int toastDelta = MeasureIdleOverlayVertexDelta();
    LAN_CHECK(toastDelta > 0);
    LAN_CHECK(toastDelta > baseDelta);
    TwoStates_OnSessionEnded();
    return true;
}

static bool T1_F9_03_ErrorToastSurvivesOnSessionEnded() {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    TwoStates_AddToast("Join failed: connection refused", 3);
    TwoStates_OnSessionEnded();
    const int delta = MeasureIdleOverlayVertexDelta();
    LAN_CHECK(delta > 0);
    return true;
}

static bool T1_F9_04_AllToastColorKindsRenderSafely() {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    for (int kind = 0; kind <= 3; ++kind) {
        char msg[48] = {};
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Toast colorKind %d", kind);
        TwoStates_AddToast(msg, kind);
    }
    const int delta = MeasureIdleOverlayVertexDelta();
    LAN_CHECK(delta > 0);
    TwoStates_OnSessionEnded();
    return true;
}

static bool T1_F9_05_ConsecutiveClipboardWritesOverwriteCleanly() {
    if (IsClipboardOpenable()) {
        LAN_CHECK(RetryCopyToClipboard("10.0.0.1"));
        LAN_CHECK(VerifyClipboardRoundTrip("26.1.2.3"));
    }
    return true;
}

// --- F10: Host Local IPv4 Display & Copy IP UI ---

static bool T1_F10_01_PrimaryHostIpCopyAndPort() {
    LAN_CHECK(CheckNorm("192.168.1.10", SAVE_SYNC_PORT, "192.168.1.10", SAVE_SYNC_PORT));
    LAN_CHECK(VerifyClipboardRoundTrip("192.168.1.10"));
    return CheckSyncBlockTwoFrames(false);
}

static bool T1_F10_02_MultiAdapterLanAndVpnRanking() {
    int pEth = 0;
    int pRad = 0;
    int pHam = 0;
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfEthernet, "Ethernet", "Realtek", "192.168.1.10", &pEth) == LanAdapterKind::Ethernet);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfPropVirt, "Radmin VPN", "Radmin", "26.40.50.60", &pRad) == LanAdapterKind::VirtualVpn);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfPropVirt, "Hamachi", "Hamachi", "25.10.20.30", &pHam) == LanAdapterKind::VirtualVpn);
    LAN_CHECK(pEth > pRad && pEth > pHam);
    return CheckSyncBlockTwoFrames(false);
}

static bool T1_F10_03_CopyHostIpWithoutSteamId() {
    ScopedSessionIdleReset idleReset;
    if (!SteamP2P_IsAvailable()) {
        LAN_CHECK(CheckSyncBlockTwoFrames(false));
        LAN_CHECK(VerifyClipboardRoundTrip("192.168.1.10"));
        for (int i = 0; i < 5; ++i) {
            TwoStates_AddToast(".", 0);
        }
        SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
        TwoStates_OnSessionEnded();
        const int baseIdleDelta = MeasureIdleOverlayVertexDelta();
        TwoStates_AddToast("LAN IP copied: 192.168.1.10", 1);
        const int postToastDelta = MeasureIdleOverlayVertexDelta();
        LAN_CHECK(postToastDelta > 0);
        LAN_CHECK(postToastDelta > baseIdleDelta);
    }
    return true;
}

static bool T1_F10_04_MapInspectModeAllowsCameraUnblocked() {
    return CheckSyncBlockTwoFrames(true);
}

static bool T1_F10_05_CoexistenceWithSteamIdentity() {
    const std::vector<LanAdapterInfo> adapters = LanDiscovery_GetLocalAdapters(false);
    (void)SteamP2P_GetLocalSteamId();
    for (const auto& ad : adapters) {
        LAN_CHECK(ad.ip[0] != '\0');
    }
    LAN_CHECK(VerifyClipboardRoundTrip("192.168.1.10"));
    return true;
}

// --- F11: LAN Server Browser in Join Menu & Modal ---

static bool T1_F11_01_DiscoveredLanHostConnectFlow() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t tcpPort = 0;
    ScopedSocket tcpSrv(OpenLoopbackTcpListener(&tcpPort));
    LAN_CHECK(tcpSrv.sock != INVALID_SOCKET && tcpPort != 0);

    const std::string hName = UniqueTag("SrvA");
    LAN_CHECK(StartHostScan(tcpPort, 0x1111u, hName.c_str(), "MapJ"));

    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &found, true));
    LAN_CHECK(found.protocolMatch);

    SOCKET acceptedSock = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(ConnectLoopbackWithAccept(tcpSrv, found.connectIp, found.tcpPort, 2000, &acceptedSock, &cli));
    ScopedSocket acceptedGuard(acceptedSock);
    ScopedSocket cliGuard(cli);
    return true;
}

static bool T1_F11_02_JoinMenuSafeWhenSteamOfflineAndNoHosts() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const uint16_t udpPort = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(udpPort != 0);
    LanDiscovery_SetPortForTest(udpPort);
    LanDiscovery_Init();
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();
    const std::vector<LanHostEntry> hosts = LanDiscovery_GetHosts();
    LAN_CHECK(hosts.empty());
    LAN_CHECK(CheckLocalPcHostWaitingIsolated());

    ScopedExePodRestore<int> screenGuard(MENU_OBJ_RVA + MENU_SCREEN_ID_OFF, 1);
    if (screenGuard.ptr != nullptr) {
        LAN_CHECK(NativeMenu_GetActiveScreenId() == 1);
        LAN_CHECK(!NativeMenu_WantsInput());
        {
            ScopedImGuiFrame frame;
            LAN_CHECK(frame.inFrame);
            NativeMenu_DrawOverlay();
            (void)frame.RenderAndCountVertices();
        }
        *screenGuard.ptr = 2;
        ScopedHostOnNextLoadGuard hostNextGuard(true);
        {
            ScopedImGuiFrame frame2;
            LAN_CHECK(frame2.inFrame);
            NativeMenu_DrawOverlay();
            LAN_CHECK(frame2.NextFrame());
            NativeMenu_DrawOverlay();
            LAN_CHECK(frame2.RenderAndCountVertices() > 0);
        }
    }
    return true;
}

static bool T1_F11_03_SamePcHostDeduplicationFlag() {
    ScopedLocalHostEvent localEv(true);
    LAN_CHECK(localEv.ev != nullptr);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    LAN_CHECK(SaveSync_IsLocalPcHostWaiting());

    std::vector<LanHostEntry> hosts;
    const LanAnnouncePacket pkt = MakeAnnouncePacket(0xA1A1ULL, 5151u, 5556u, 1u, "LocalPC", "Map");
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "127.0.0.1", 1000ULL));
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(hosts[0].sameMachine);
    return true;
}

static bool T1_F11_04_ProtocolMismatchMarkedInHostEntry() {
    std::vector<LanHostEntry> hosts;
    const LanAnnouncePacket pkt = MakeAnnouncePacket(
        0xB2B2ULL, 5252u, 5556u, 1u, "OldServer", "Map", static_cast<uint16_t>(REPUBLICMP_PROTOCOL + 1));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "192.168.1.90", 1000ULL));
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(!hosts[0].protocolMatch);
    LAN_CHECK(hosts[0].protocol == static_cast<uint16_t>(REPUBLICMP_PROTOCOL + 1));
    return true;
}

static bool T1_F11_05_RefreshHostsOfflineProbeAndToast() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const uint16_t udpPort = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(udpPort != 0);
    LanDiscovery_SetPortForTest(udpPort);
    LanDiscovery_Init();
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_AddToast("LAN host list refreshed", 1);
    LAN_CHECK(MeasureIdleOverlayVertexDelta() > 0);
    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 2: Boundary & Corner Cases (55 Test Cases: 5 per Feature F1..F11)
// ---------------------------------------------------------------------------

// --- F1 Boundaries ---

static bool T2_F1_01_LoopbackApipaZeroRejected() {
    char mask[16] = {};
    char bcast[16] = {};
    LAN_CHECK(LanDiscovery_ComputeBroadcast("0.0.0.0", 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    LAN_CHECK(std::strcmp(mask, "255.255.255.0") == 0);
    LAN_CHECK(std::strcmp(bcast, "0.0.0.255") == 0);
    const LanAdapterKind lb = LanDiscovery_ClassifyAdapter(kIfLoopback, "Loopback", "Loopback", "127.0.0.1", nullptr);
    LAN_CHECK(lb == LanAdapterKind::VirtualLocal);
    const std::vector<LanAdapterInfo> list = LanDiscovery_GetLocalAdapters(true);
    for (const auto& a : list) {
        LAN_CHECK(std::strncmp(a.ip, "127.", 4) != 0);
        LAN_CHECK(std::strncmp(a.ip, "169.254.", 8) != 0);
        LAN_CHECK(std::strcmp(a.ip, "0.0.0.0") != 0);
    }
    return true;
}

static bool T2_F1_02_PrefixLengthsEdgeAndInvalid() {
    char mask[16] = {};
    char bcast[16] = {};
    LAN_CHECK(LanDiscovery_ComputeBroadcast("192.168.1.10", 32, mask, sizeof(mask), bcast, sizeof(bcast)));
    LAN_CHECK(std::strcmp(mask, "255.255.255.255") == 0);
    LAN_CHECK(std::strcmp(bcast, "192.168.1.10") == 0);

    LAN_CHECK(LanDiscovery_ComputeBroadcast("192.168.1.10", 31, mask, sizeof(mask), bcast, sizeof(bcast)));
    LAN_CHECK(std::strcmp(mask, "255.255.255.254") == 0);
    LAN_CHECK(std::strcmp(bcast, "192.168.1.11") == 0);

    // Prefix 0 and > 32 safely fall back to /24 (or return false) with zero UB shift.
    const bool ok0 = LanDiscovery_ComputeBroadcast("192.168.1.10", 0, mask, sizeof(mask), bcast, sizeof(bcast));
    if (ok0) LAN_CHECK(std::strcmp(mask, "255.255.255.0") == 0);
    const bool ok33 = LanDiscovery_ComputeBroadcast("192.168.1.10", 33, mask, sizeof(mask), bcast, sizeof(bcast));
    if (ok33) LAN_CHECK(std::strcmp(mask, "255.255.255.0") == 0);
    const bool ok255 = LanDiscovery_ComputeBroadcast("192.168.1.10", 255, mask, sizeof(mask), bcast, sizeof(bcast));
    if (ok255) LAN_CHECK(std::strcmp(mask, "255.255.255.0") == 0);
    return true;
}

static bool T2_F1_03_MalformedIpsAndNullBuffers() {
    char mask[16] = {};
    char bcast[16] = {};
    LAN_CHECK(!LanDiscovery_ComputeBroadcast("999.999.1.1", 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    LAN_CHECK(!LanDiscovery_ComputeBroadcast(nullptr, 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    LAN_CHECK(!LanDiscovery_ComputeBroadcast("", 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    LAN_CHECK(!LanDiscovery_ComputeBroadcast("192.168.1.10", 24, mask, sizeof(mask), nullptr, 0));
    LAN_CHECK(!LanDiscovery_ComputeBroadcast("192.168.1.10", 24, mask, 4, bcast, 4));
    return true;
}

static bool T2_F1_04_VpnOutranksVirtualLocal() {
    int pHyperV = 0;
    int pWsl = 0;
    int pDocker = 0;
    int pRadmin = 0;
    int pTailscale = 0;
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfEthernet, "vEthernet (Default Switch)", "Hyper-V Virtual Ethernet",
                                           "172.29.0.1", &pHyperV) == LanAdapterKind::VirtualLocal);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfEthernet, "vEthernet (WSL)", "Hyper-V Virtual Ethernet",
                                           "172.25.0.1", &pWsl) == LanAdapterKind::VirtualLocal);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfEthernet, "DockerNAT", "Docker Virtual",
                                           "172.17.0.1", &pDocker) == LanAdapterKind::VirtualLocal);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfPropVirt, "Radmin VPN", "Radmin", "26.1.2.3", &pRadmin) == LanAdapterKind::VirtualVpn);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfTunnel, "Tailscale", "Tailscale Tunnel", "100.64.0.5", &pTailscale) == LanAdapterKind::VirtualVpn);
    LAN_CHECK(pRadmin > pHyperV && pRadmin > pWsl && pRadmin > pDocker);
    LAN_CHECK(pTailscale > pHyperV);
    return true;
}

static bool T2_F1_05_TunnelWithEmptyNameClassifiedByIp() {
    const LanAdapterKind rad = LanDiscovery_ClassifyAdapter(kIfTunnel, "", "", "26.55.66.77", nullptr);
    const LanAdapterKind ham = LanDiscovery_ClassifyAdapter(kIfTunnel, "", "", "25.12.34.56", nullptr);
    LAN_CHECK(rad == LanAdapterKind::VirtualVpn);
    LAN_CHECK(ham == LanAdapterKind::VirtualVpn);
    return true;
}

// --- F2 Boundaries ---

static bool T2_F2_01_TruncatedAndOversizedPacketsRejected() {
    LanProbePacket p = {};
    LanAnnouncePacket a = {};
    const uint8_t tiny[4] = { 'R', 'P', 'M', 'P' };
    LAN_CHECK(!LanDiscovery_ParseProbe(nullptr, 0, &p));
    LAN_CHECK(!LanDiscovery_ParseProbe(tiny, sizeof(tiny), &p));
    LAN_CHECK(!LanDiscovery_ParseAnnounce(tiny, sizeof(tiny), &a));
    std::vector<uint8_t> almost(sizeof(LanAnnouncePacket) - 1u, 0);
    std::memcpy(almost.data(), "RPMPANN1", 8);
    LAN_CHECK(!LanDiscovery_ParseAnnounce(almost.data(), almost.size(), &a));
    std::vector<uint8_t> big(512, 0);
    std::memcpy(big.data(), "RPMPANN1", 8);
    LAN_CHECK(!LanDiscovery_ParseAnnounce(big.data(), big.size(), &a));
    return true;
}

static bool T2_F2_02_BadMagicAndZeroPortRejected() {
    LanProbePacket p = MakeProbePacket(REPUBLICMP_PROTOCOL, 1u, 2u);
    std::memcpy(p.magic, "BADMAGIC", 8);
    LanProbePacket outP = {};
    LAN_CHECK(!LanDiscovery_ParseProbe(&p, sizeof(p), &outP));

    LanAnnouncePacket a = MakeAnnouncePacket(1ULL, 10u, 5556u, 1u, "H", "M");
    std::memcpy(a.magic, "RPMPANN0", 8);
    LanAnnouncePacket outA = {};
    LAN_CHECK(!LanDiscovery_ParseAnnounce(&a, sizeof(a), &outA));

    LanAnnouncePacket zeroPort = MakeAnnouncePacket(2ULL, 10u, 0u, 1u, "H", "M");
    LAN_CHECK(!LanDiscovery_ParseAnnounce(&zeroPort, sizeof(zeroPort), &outA));
    return true;
}

static bool T2_F2_03_UnterminatedStringsBoundedSafely() {
    LanAnnouncePacket raw = {};
    std::memcpy(raw.magic, "RPMPANN1", 8);
    raw.protocol = REPUBLICMP_PROTOCOL;
    raw.tcpPort = 5556u;
    raw.serverGuid = 0xDEADULL;
    raw.hostPid = 777u;
    raw.open = 1u;
    std::memset(raw.version, 'V', sizeof(raw.version));
    std::memset(raw.hostName, 'H', sizeof(raw.hostName));
    std::memset(raw.mapName, 'M', sizeof(raw.mapName));

    LanAnnouncePacket parsed = {};
    LAN_CHECK(LanDiscovery_ParseAnnounce(&raw, sizeof(raw), &parsed));
    LAN_CHECK(parsed.version[sizeof(parsed.version) - 1u] == '\0');
    LAN_CHECK(parsed.hostName[sizeof(parsed.hostName) - 1u] == '\0');
    LAN_CHECK(parsed.mapName[sizeof(parsed.mapName) - 1u] == '\0');

    std::vector<LanHostEntry> hosts;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, parsed, "192.168.1.40", 1000ULL));
    LAN_CHECK(hosts[0].hostName[sizeof(hosts[0].hostName) - 1u] == '\0');
    return true;
}

static bool T2_F2_04_ProtocolMismatchValues() {
    std::vector<LanHostEntry> hosts;
    const LanAnnouncePacket zeroProto = MakeAnnouncePacket(10ULL, 9u, 5556u, 1u, "ZeroProto", "M", 0u);
    LAN_CHECK(!LanDiscovery_UpsertHost(hosts, zeroProto, "192.168.1.1", 1000ULL));

    const LanAnnouncePacket a = MakeAnnouncePacket(11ULL, 10u, 5556u, 1u, "Mismatch", "M", 1u);
    const LanAnnouncePacket b = MakeAnnouncePacket(12ULL, 11u, 5556u, 1u, "Future", "M",
                                                   static_cast<uint16_t>(REPUBLICMP_PROTOCOL + 1u));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, a, "192.168.1.2", 1000ULL));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, b, "192.168.1.3", 1000ULL));
    LAN_CHECK(hosts.size() == 2u);
    LAN_CHECK(!hosts[0].protocolMatch);
    LAN_CHECK(!hosts[1].protocolMatch);
    return true;
}

static bool T2_F2_05_ClockWraparoundAndExactTtlBoundary() {
    std::vector<LanHostEntry> hosts;
    const uint64_t lastSeen = UINT64_MAX - 1000ULL;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, MakeAnnouncePacket(90ULL, 10u, 5556u, 1u, "Wrap", "M"),
                                      "192.168.1.5", lastSeen));
    LanDiscovery_PruneExpired(hosts, 3499ULL, LAN_DISCOVERY_TTL_MS); // age == 4500 <= 4500
    LAN_CHECK(hosts.size() == 1u);
    LanDiscovery_PruneExpired(hosts, 3500ULL, LAN_DISCOVERY_TTL_MS); // age == 4501 > 4500
    LAN_CHECK(hosts.empty());
    return true;
}

// --- F3 Boundaries ---

static bool T2_F3_01_ClosedHostIgnoresProbes() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const uint16_t port = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(port != 0);
    LanDiscovery_SetPortForTest(port);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(false, 5556u, 1u, "Closed", "M");
    ScopedSocket cli(OpenBoundUdpLoopback(0, 150));
    LAN_CHECK(cli.sock != INVALID_SOCKET);
    LanAnnouncePacket reply = {};
    LAN_CHECK(!SendProbeAndAwaitAnnounce(cli.sock, port, MakeProbePacket(REPUBLICMP_PROTOCOL, 1u, 999991u),
                                           nullptr, &reply, 1u, 3));
    return true;
}

static bool T2_F3_02_GarbageFloodResilience() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string hName = UniqueTag("T2F302");
    LAN_CHECK(StartResponder(&port, 1u, hName.c_str(), "FloodMap"));

    ScopedSocket cli(OpenBoundUdpLoopback(0, 400));
    LAN_CHECK(cli.sock != INVALID_SOCKET);
    LanAnnouncePacket preFlood = {};
    LAN_CHECK(SendProbeAndAwaitAnnounce(cli.sock, port, MakeProbePacket(REPUBLICMP_PROTOCOL, 1u, 999992u),
                                          hName.c_str(), &preFlood, 1u));

    std::vector<uint8_t> junk(1400, 0xAB);
    for (int i = 0; i < 100; ++i) {
        const size_t len = static_cast<size_t>((i * 17) % 1399 + 1);
        junk[0] = static_cast<uint8_t>(i);
        (void)SendUdpLoopback(cli.sock, port, junk.data(), len);
    }
    LanAnnouncePacket reply = {};
    LAN_CHECK(SendProbeAndAwaitAnnounce(cli.sock, port, MakeProbePacket(REPUBLICMP_PROTOCOL, 2u, 999992u),
                                          hName.c_str(), &reply, 1u));
    return true;
}

static bool T2_F3_03_LongUtf8HostNameAndNullMapName() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string longName(120, 'X');
    LAN_CHECK(StartResponder(&port, 1u, longName.c_str(), nullptr));
    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(port, 3u, 999993u, nullptr, &reply));
    LAN_CHECK(reply.hostName[sizeof(reply.hostName) - 1u] == '\0');
    LAN_CHECK(reply.mapName[0] == '\0');
    return true;
}

static bool T2_F3_04_ExclusivePortConflictDegradesGracefully() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    ScopedSocket blocker(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    LAN_CHECK(blocker.sock != INVALID_SOCKET);
    BOOL excl = TRUE;
    (void)setsockopt(blocker.sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    LAN_CHECK(bind(blocker.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    int len = static_cast<int>(sizeof(addr));
    LAN_CHECK(getsockname(blocker.sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const uint16_t port = ntohs(addr.sin_port);

    LanDiscovery_SetPortForTest(port);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, "BlockedHost", "M");
    LAN_CHECK(!LanDiscovery_GetLocalDisplayName().empty());
    LAN_CHECK(LanDiscovery_GetHosts().empty());
    LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    LanDiscovery_Shutdown();

    // Release exclusive blocker, wait until AFD allows SO_REUSEADDR bind, and verify recovery.
    blocker.Reset();
    LAN_CHECK(WaitPortReuseBindable(SOCK_DGRAM, IPPROTO_UDP, port, 500));
    const std::string hName = UniqueTag("T2F304");
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "Recovered");
    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(port, 0x3401u, 999989u, hName.c_str(), &reply));
    LAN_CHECK(std::strcmp(reply.mapName, "Recovered") == 0);
    return true;
}

static bool T2_F3_05_RapidHostingToggleDeterminism() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string hName = UniqueTag("T2F305");
    LAN_CHECK(StartResponder(&port, 1u, hName.c_str(), "Toggle"));
    for (int i = 0; i < 20; ++i) {
        LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "Toggle");
        LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    }
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "Final");
    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(port, 5u, 999994u, hName.c_str(), &reply));
    LAN_CHECK(std::strcmp(reply.mapName, "Final") == 0);
    return true;
}

// --- F4 Boundaries ---

static bool T2_F4_01_DedupOrderLoopbackOverLanOverVpn() {
    const char* const revIps[3] = { "26.5.5.5", "192.168.1.5", "127.0.0.1" };
    const LanAdapterKind revKinds[3] = {
        LanAdapterKind::VirtualVpn, LanAdapterKind::Ethernet, LanAdapterKind::Ethernet
    };
    LAN_CHECK(CheckDedup(0x999ULL, 3000u, "MultiNic", revIps, revKinds, 3u, "127.0.0.1", true));

    const char* const fwdIps[3] = { "127.0.0.1", "192.168.1.5", "26.5.5.5" };
    const LanAdapterKind fwdKinds[3] = {
        LanAdapterKind::Ethernet, LanAdapterKind::Ethernet, LanAdapterKind::VirtualVpn
    };
    return CheckDedup(0x999ULL, 3000u, "MultiNic", fwdIps, fwdKinds, 3u, "127.0.0.1", true);
}

static bool T2_F4_02_DedupOrderLanOverVpn() {
    const char* const aIps[2] = { "26.5.5.5", "192.168.1.5" };
    const LanAdapterKind aKinds[2] = { LanAdapterKind::VirtualVpn, LanAdapterKind::Ethernet };
    LAN_CHECK(CheckDedup(0x888ULL, 3001u, "LanVpn", aIps, aKinds, 2u, "192.168.1.5", false));

    const char* const bIps[2] = { "192.168.1.5", "26.5.5.5" };
    const LanAdapterKind bKinds[2] = { LanAdapterKind::Ethernet, LanAdapterKind::VirtualVpn };
    return CheckDedup(0x888ULL, 3001u, "LanVpn", bIps, bKinds, 2u, "192.168.1.5", false);
}

static bool T2_F4_03_ScannerIgnoresSelfPid() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const uint16_t port = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(port != 0);
    const std::string hName = UniqueTag("T2F403");
    LanDiscovery_SetPortForTest(port);
    LanDiscovery_SetSelfPidOverrideForTest(0, false);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "SelfMap");
    LanDiscovery_StartScan();

    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(port, 0x4302u, 999990u, hName.c_str(), &reply));
    LAN_CHECK(std::strcmp(reply.mapName, "SelfMap") == 0);

    for (int i = 0; i < 8; ++i) {
        LanDiscovery_ProbeNow();
        Sleep(40);
    }
    const std::vector<LanHostEntry> hosts = LanDiscovery_GetHosts();
    LAN_CHECK(!FindHostByName(hosts, hName.c_str(), nullptr));
    return true;
}

static bool T2_F4_04_AuthoritativeRecvfromIpOverridesPayload() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;

    // Bind a raw loopback UDP responder sending a spoofed hostName "203.0.113.55:9999".
    ScopedSocket rogueSrv(OpenBoundUdpLoopback(0, 400));
    LAN_CHECK(rogueSrv.sock != INVALID_SOCKET);
    sockaddr_in srvAddr = {};
    int srvLen = static_cast<int>(sizeof(srvAddr));
    LAN_CHECK(getsockname(rogueSrv.sock, reinterpret_cast<sockaddr*>(&srvAddr), &srvLen) == 0);
    const uint16_t roguePort = ntohs(srvAddr.sin_port);
    LAN_CHECK(roguePort != 0);

    LanDiscovery_SetPortForTest(roguePort);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 0x7777u, true);
    LanDiscovery_Init();
    LanDiscovery_StartScan();

    bool replied = false;
    const ULONGLONG deadline = GetTickCount64() + 3000ULL;
    while (GetTickCount64() < deadline && !replied) {
        LanDiscovery_ProbeNow();
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(rogueSrv.sock, &rfds);
        timeval tv = { 0, 80000 };
        if (select(0, &rfds, nullptr, nullptr, &tv) > 0 && FD_ISSET(rogueSrv.sock, &rfds)) {
            std::vector<uint8_t> rx(256, 0);
            sockaddr_in from = {};
            int fromLen = static_cast<int>(sizeof(from));
            const int r = recvfrom(rogueSrv.sock, reinterpret_cast<char*>(rx.data()),
                                   static_cast<int>(rx.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            LanProbePacket prb = {};
            if (r > 0 && LanDiscovery_ParseProbe(rx.data(), static_cast<size_t>(r), &prb)) {
                const LanAnnouncePacket spoof = MakeAnnouncePacket(
                    0x2404ULL, GetCurrentProcessId() ^ 0x8888u, 5556u, 1u, "203.0.113.55:9999", "AuthMap");
                std::vector<uint8_t> wire(sizeof(LanAnnouncePacket), 0);
                LAN_CHECK(LanDiscovery_SerializeAnnounce(spoof, wire.data(), wire.size()) == sizeof(LanAnnouncePacket));
                const int sent = sendto(rogueSrv.sock, reinterpret_cast<const char*>(wire.data()),
                                        static_cast<int>(wire.size()), 0,
                                        reinterpret_cast<const sockaddr*>(&from), fromLen);
                LAN_CHECK(sent == static_cast<int>(wire.size()));
                replied = true;
            }
        }
    }
    LAN_CHECK(replied);

    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName("203.0.113.55:9999", 3000, &found, true));
    LAN_CHECK(std::strcmp(found.connectIp, "127.0.0.1") == 0);
    LAN_CHECK(found.sameMachine);
    return true;
}

static bool T2_F4_05_SixtyFourHostsUpsertAndExpire() {
    std::vector<LanHostEntry> hosts;
    for (uint64_t i = 1ULL; i <= 64ULL; ++i) {
        char ip[24] = {};
        _snprintf_s(ip, sizeof(ip), _TRUNCATE, "192.168.1.%u", static_cast<unsigned int>(i + 10ULL));
        const LanAnnouncePacket pkt = MakeAnnouncePacket(i, static_cast<uint32_t>(i + 1000ULL), 5556u, 1u, "Bulk", "M");
        LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, ip, 1000ULL + i));
    }
    LAN_CHECK(hosts.size() == 64u);
    LanDiscovery_PruneExpired(hosts, 7000ULL, LAN_DISCOVERY_TTL_MS);
    LAN_CHECK(hosts.empty());
    return true;
}

// --- F5 Boundaries ---

static bool T2_F5_01_NullEmptyWhitespaceRejected() {
    char host[64] = {};
    uint16_t port = 0;
    LAN_CHECK(!NetworkNormalizeTargetAddress(nullptr, host, sizeof(host), &port));
    LAN_CHECK(!NetworkNormalizeTargetAddress("", host, sizeof(host), &port));
    LAN_CHECK(!NetworkNormalizeTargetAddress("   \t\r\n  ", host, sizeof(host), &port));
    return true;
}

static bool T2_F5_02_InvalidPortsRejected() {
    LAN_CHECK(CheckNorm("192.168.1.1:1", 0, "192.168.1.1", 1u));
    LAN_CHECK(CheckNorm("192.168.1.1:65535", 0, "192.168.1.1", 65535u));
    const char* const bad[] = {
        "192.168.1.1:",
        "192.168.1.1:0",
        "192.168.1.1:65536",
        "192.168.1.1:99999",
        "192.168.1.1:abc",
        ":5556"
    };
    return CheckNormBad(bad, sizeof(bad) / sizeof(bad[0]));
}

static bool T2_F5_03_MalformedIpv4Rejected() {
    const char* const bad[] = {
        "999.1.1.1",
        "192.168.1",
        "192.168.1.1.1",
        "192.168.1.1 foo",
        "256.0.0.1:5556"
    };
    LAN_CHECK(CheckNormBad(bad, sizeof(bad) / sizeof(bad[0])));
    char outHost[64] = "dirty";
    uint16_t outPort = 7777u;
    LAN_CHECK(!NetworkNormalizeTargetAddress("999.1.1.1", outHost, sizeof(outHost), &outPort));
    LAN_CHECK(outHost[0] == '\0');
    LAN_CHECK(outPort == 7777u);
    return true;
}

static bool T2_F5_04_TrimmedPortAndSteamIdAccepted() {
    LAN_CHECK(CheckNorm("  \t10.20.30.40:5566\r\n ", 0, "10.20.30.40", 5566u));
    return CheckSteamNorm("  steam:76561198000000001  ", 76561198000000001ULL);
}

static bool T2_F5_05_SmallBufferCapacityRejected() {
    char tiny[8] = {};
    uint16_t port = 0;
    LAN_CHECK(!NetworkNormalizeTargetAddress("192.168.100.200", tiny, sizeof(tiny), &port));
    LAN_CHECK(!NetworkNormalizeTargetAddress("192.168.1.1", nullptr, 0, &port));
    return true;
}

// --- F6 Boundaries ---

static bool T2_F6_01_OfflineModeNotAvailable() {
    if (!SteamP2P_IsLoggedOn()) {
        LAN_CHECK(!SteamP2P_IsAvailable());
        LAN_CHECK(SteamP2P_GetLocalSteamId() == 0ULL);
        LAN_CHECK(SteamP2P_GetLocalPersonaName()[0] == '\0');
    }
    return true;
}

static bool T2_F6_02_SteamInitLoopIdempotentFast() {
    if (GetModuleHandleA("steam_api64.dll") == nullptr) {
        const ULONGLONG t0 = GetTickCount64();
        for (int i = 0; i < 10; ++i) {
            LAN_CHECK(!SteamP2P_Init());
        }
        LAN_CHECK(GetTickCount64() - t0 < 2000ULL);
    }
    return true;
}

static bool T2_F6_03_DualTestbedEnvConsistency() {
    char buf[32] = {};
    const DWORD len = GetEnvironmentVariableA("REPUBLICMP_ROLE", buf, sizeof(buf));
    if (len > 0 && buf[0] != '\0') {
        LAN_CHECK(SaveSync_IsDualTestbed());
    }
    LAN_CHECK(SaveSync_IsDualTestbed() == SaveSync_IsDualTestbed());
    return true;
}

static bool T2_F6_04_SteamBoundaryNullAndInvalidIds() {
    LAN_CHECK(!SteamP2P_ConsumePendingJoin(nullptr, 0));
    char smallBuf[4] = {};
    LAN_CHECK(!SteamP2P_ConsumePendingJoin(smallBuf, 0));
    LAN_CHECK(SteamP2P_ParseTargetSteamId(nullptr) == 0ULL);
    LAN_CHECK(SteamP2P_ParseTargetSteamId("") == 0ULL);
    LAN_CHECK(SteamP2P_ParseTargetSteamId("steam:") == 0ULL);
    LAN_CHECK(SteamP2P_ParseTargetSteamId("steam:abc") == 0ULL);
    LAN_CHECK(SteamP2P_ParseTargetSteamId("12345") == 0ULL);
    return true;
}

static bool T2_F6_05_InvalidSocketChecksSafe() {
    LAN_CHECK(!SteamP2P_IsSteamSocket(INVALID_SOCKET));
    SteamP2P_PeerGone(INVALID_SOCKET);
    SteamP2P_CloseSocket(INVALID_SOCKET);
    return true;
}

// --- F7 Boundaries ---

static bool T2_F7_01_FindLocalModInvalidArgs() {
    char out[MAX_PATH] = {};
    LAN_CHECK(!ModSync_FindLocalModFolderAtRoot(0ULL, "C:\\NonExistent", false, out, sizeof(out)));
    LAN_CHECK(!ModSync_FindLocalModFolderAtRoot(123456ULL, nullptr, false, out, sizeof(out)));
    LAN_CHECK(!ModSync_FindLocalModFolderAtRoot(123456ULL, "", false, out, sizeof(out)));
    LAN_CHECK(!ModSync_FindLocalModFolderAtRoot(123456ULL, "C:\\NonExistent", false, nullptr, 0));
    ScopedTempDir tmp("f7b1");
    LAN_CHECK(CreateWorkshopDir(tmp.dir / "media_soviet" / "workshop_wip" / "123456", true));
    char tiny[4] = {};
    LAN_CHECK(!ModSync_FindLocalModFolderAtRoot(123456ULL, tmp.dir.string().c_str(), false, tiny, sizeof(tiny)));
    return true;
}

static bool T2_F7_02_ModDirWithoutConfigIniIgnored() {
    ScopedTempDir tmp("f7b2");
    const std::filesystem::path exeDir = tmp.dir / "common" / "SovietRepublic";
    std::error_code ec;
    std::filesystem::create_directories(exeDir, ec);
    const std::filesystem::path modDir = tmp.dir / "workshop" / "content" / "784150" / "888001";
    LAN_CHECK(CreateWorkshopDir(modDir, false));
    char out[MAX_PATH] = {};
    LAN_CHECK(!ModSync_FindLocalModFolderAtRoot(888001ULL, exeDir.string().c_str(), false, out, sizeof(out)));
    return true;
}

static bool T2_F7_03_MissingModOfflineDoesNotHangDownloadModal() {
    if (ModSync_IsSteamUgcOnline()) return true;
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedTempDir tmp("f7b3");
    ScopedModPackMode packGuard(MOD_PACK_SAVE_ONLY);
    ScopedModSyncGuard modGuard;
    LAN_CHECK(WriteHeaderBinWithMods(tmp.dir, { 777001999ULL }));

    ModSyncHostResult hostRes = MOD_SYNC_HOST_ERROR;
    ModSyncClientResult cliRes = MOD_SYNC_CLIENT_ERROR;
    LAN_CHECK(RunModSync(tmp.dir, &hostRes, &cliRes, nullptr, 0));
    LAN_CHECK(!ModSync_IsDownloadActive());
    LAN_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
    LAN_CHECK(hostRes == MOD_SYNC_HOST_OK);
    LAN_CHECK(ModSync_GetLastOfflineWarning()[0] != '\0');
    LAN_CHECK(!ModSync_IsModIdAllowedInSession(9999999998ULL));
    return true;
}

static bool T2_F7_04_BeforeWorldLoadOfflineMissingModNoStuckModal() {
    if (ModSync_IsSteamUgcOnline()) return true;
    ScopedTempDir tmp("f7b4");
    ScopedModSyncGuard modGuard;
    LAN_CHECK(WriteHeaderBinWithMods(tmp.dir, { 777002999ULL }));
    ModSync_BeforeWorldLoad(true, tmp.dir.string().c_str());
    LAN_CHECK(!ModSync_IsDownloadActive());
    return true;
}

static bool T2_F7_05_CorruptedModManifestHeaderRejected() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);

    std::thread sender([&]() {
        // 276-byte WireModManifestHeader with modCount = 99999 (> 4096 cap).
        std::vector<uint8_t> hdr(276, 0);
        std::memcpy(hdr.data(), "RPMPMOD3", 8);
        const uint32_t badCount = 99999u;
        std::memcpy(hdr.data() + 12, &badCount, sizeof(badCount));
        (void)NetworkSendExact(srvGuard.sock, hdr.data(), hdr.size());
    });

    char err[256] = {};
    const ModSyncClientResult res = ModSync_ClientHandshake(cliGuard.sock, err, sizeof(err));
    sender.join();
    LAN_CHECK(res == MOD_SYNC_CLIENT_ERROR);
    LAN_CHECK(err[0] != '\0');
    return true;
}

// --- F8 Boundaries ---

static bool T2_F8_01_ClientCancelReturnsToIdleQuickly() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ULONGLONG elapsedMs = 0;
    LAN_CHECK(CancelSilentCli(150, 2500, &elapsedMs));
    LAN_CHECK(elapsedMs <= 2500ULL);
    return true;
}

static bool T2_F8_02_StartHostRefusedWhenNoWorldLoaded() {
    ScopedExePodRestore<int64_t> roadGuard(kWorldRoadNetRva, 0);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_StartHost();
    LAN_CHECK(SaveSync_GetState() == SESSION_IDLE);
    LAN_CHECK(SaveSync_GetLastError()[0] != '\0');
    return true;
}

static bool T2_F8_03_VersionAndProtocolMismatchCheck() {
    // Verify local identity & mismatch detection via wire protocol + version checks.
    LAN_CHECK(std::strlen(SaveSync_GetLocalVersion()) > 0);
    LAN_CHECK(SaveSync_GetLocalDllCrc() != 0u);
    const LanAnnouncePacket badProto = MakeAnnouncePacket(
        0x3030ULL, 404u, 5556u, 1u, "MismatchSrv", "Map", static_cast<uint16_t>(REPUBLICMP_PROTOCOL - 1));
    std::vector<LanHostEntry> hosts;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, badProto, "127.0.0.1", 1000ULL));
    LAN_CHECK(!hosts[0].protocolMatch);
    return true;
}

static bool T2_F8_04_StartClientIgnoredWhileBusy() {
    SaveSync_SetTestSessionState(SESSION_CLIENT_JOINING, false, "");
    const std::string beforeIp = SaveSync_GetTargetIp();
    SaveSync_StartClient("192.168.1.99");
    LAN_CHECK(SaveSync_IsBusy());
    LAN_CHECK(std::string(SaveSync_GetTargetIp()) == beforeIp);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    return true;
}

static bool T2_F8_05_DisplayNameBoundsValid() {
    const std::string name = LanDiscovery_GetLocalDisplayName();
    LAN_CHECK(!name.empty());
    LAN_CHECK(name.size() <= 47u);
    return true;
}

// --- F9 Boundaries ---

static bool T2_F9_01_ClipboardRejectsNullAndEmpty() {
    LAN_CHECK(!UiCommon_CopyTextToClipboard(nullptr));
    LAN_CHECK(!UiCommon_CopyTextToClipboard(""));
    return true;
}

static bool T2_F9_02_ClipboardHandlesLargeAndUtf8Text() {
    const std::string utf8 = "Сервер-ЛВС-192.168.1.10:" + std::string(480, 'A');
    LAN_CHECK(VerifyClipboardRoundTrip(utf8.c_str()));
    return true;
}

static bool T2_F9_03_RapidToastBurstBoundedInIdle() {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    for (int i = 0; i < 20; ++i) {
        char msg[48] = {};
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Burst toast %d", i);
        TwoStates_AddToast(msg, i % 4);
    }
    LAN_CHECK(MeasureIdleOverlayVertexDelta() > 0);
    TwoStates_OnSessionEnded();
    return true;
}

static bool T2_F9_04_DoubleOnSessionEndedIdempotent() {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    TwoStates_AddToast("Connection timed out", 3);
    TwoStates_OnSessionEnded();
    TwoStates_OnSessionEnded();
    LAN_CHECK(MeasureIdleOverlayVertexDelta() > 0);
    return true;
}

static bool T2_F9_05_AddToastNullEmptyAndOutOfRangeColor() {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    TwoStates_AddToast(nullptr, 0);
    TwoStates_AddToast("", 99);
    TwoStates_AddToast("Clamped color toast", 99);
    LAN_CHECK(MeasureIdleOverlayVertexDelta() > 0);
    TwoStates_OnSessionEnded();
    return true;
}

// --- F10 Boundaries ---

static bool T2_F10_01_EmptyAdapterFallbackNeverCopiesEmpty() {
    LAN_CHECK(!UiCommon_CopyTextToClipboard(""));
    LAN_CHECK(VerifyClipboardRoundTrip("127.0.0.1"));
    return true;
}

static bool T2_F10_02_SixAdaptersPrioritizedAboveVirtualLocal() {
    struct Spec { uint32_t ift; const char* n; const char* d; const char* ip; };
    const Spec specs[6] = {
        { kIfEthernet, "vEthernet (Default Switch)", "Hyper-V", "172.29.0.1" },
        { kIfPropVirt, "ZeroTier One", "ZeroTier", "172.24.1.2" },
        { kIfPropVirt, "Hamachi", "Hamachi", "25.1.2.3" },
        { kIfPropVirt, "Radmin VPN", "Radmin", "26.1.2.3" },
        { kIfWifi, "Wi-Fi", "Intel Wi-Fi", "10.0.0.5" },
        { kIfEthernet, "Ethernet", "Intel Ethernet", "192.168.1.10" }
    };
    int prios[6] = {};
    for (int i = 0; i < 6; ++i) {
        (void)LanDiscovery_ClassifyAdapter(specs[i].ift, specs[i].n, specs[i].d, specs[i].ip, &prios[i]);
    }
    LAN_CHECK(prios[5] > prios[4]); // Ethernet > WiFi
    LAN_CHECK(prios[4] > prios[3]); // WiFi > VirtualVpn
    for (int i = 1; i <= 3; ++i) {
        LAN_CHECK(prios[i] > prios[0]); // All VPNs > Hyper-V VirtualLocal
    }
    return true;
}

static bool T2_F10_03_MapInspectExitsOnHostJoining() {
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    SyncBlock_SetActive(false, nullptr);
    SyncBlock_SetMapInspectMode(true);
    LAN_CHECK(SyncBlock_IsMapInspectMode());
    LAN_CHECK(!SyncBlock_UpdateMapInspectState(static_cast<int>(SESSION_HOST_JOINING), true));
    LAN_CHECK(!SyncBlock_IsMapInspectMode());
    LAN_CHECK(!SyncBlock_UpdateMapInspectState(static_cast<int>(SESSION_HOST_IN_GAME), false));
    LAN_CHECK(!SyncBlock_IsMapInspectMode());
    SyncBlock_SetMapInspectMode(false);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    return true;
}

static bool T2_F10_04_LongAdapterNameAndMaxIpv4Safe() {
    const std::string longAdName(63, 'N');
    int prio = 0;
    const LanAdapterKind k = LanDiscovery_ClassifyAdapter(kIfEthernet, longAdName.c_str(), longAdName.c_str(),
                                                          "255.255.255.255", &prio);
    LAN_CHECK(k == LanAdapterKind::Ethernet);
    char mask[16] = {};
    char bcast[16] = {};
    LAN_CHECK(LanDiscovery_ComputeBroadcast("255.255.255.255", 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    LAN_CHECK(VerifyClipboardRoundTrip("255.255.255.255"));
    return true;
}

static bool T2_F10_05_CopyToastSurvivesHostDisconnectToIdle() {
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    LAN_CHECK(VerifyClipboardRoundTrip("192.168.1.10"));
    TwoStates_AddToast("IP copied", 1);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    LAN_CHECK(MeasureIdleOverlayVertexDelta() > 0);
    return true;
}

// --- F11 Boundaries ---

static bool T2_F11_01_JoinMenuSlotBudgetPreservesStaticControls() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    {
        ScopedLocalHostEvent localEv(true);
        LAN_CHECK(localEv.ev != nullptr);
        SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
        LAN_CHECK(SaveSync_IsLocalPcHostWaiting());
    }

    const uint16_t udpPort = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(udpPort != 0);
    const std::string liveName = UniqueTag("BrowserSrv");
    LanDiscovery_SetPortForTest(udpPort);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 0xB101u, true);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, liveName.c_str(), "BrowserMap");
    LanDiscovery_StartScan();

    LanHostEntry liveFound = {};
    LAN_CHECK(PollForHostByName(liveName.c_str(), 3000, &liveFound, true));
    const std::vector<LanHostEntry> browserHosts = LanDiscovery_GetHosts();
    LAN_CHECK(FindHostByName(browserHosts, liveName.c_str(), nullptr));

    std::vector<LanHostEntry> hosts;
    for (uint64_t i = 1ULL; i <= 30ULL; ++i) {
        char name[48] = {};
        char ip[16] = {};
        _snprintf_s(name, sizeof(name), _TRUNCATE, "LanSrv%02u", static_cast<unsigned int>(i));
        _snprintf_s(ip, sizeof(ip), _TRUNCATE, "192.168.1.%u", static_cast<unsigned int>(50u + i));
        LAN_CHECK(LanDiscovery_UpsertHost(hosts, MakeAnnouncePacket(i, 4000u + static_cast<uint32_t>(i),
                                                                    5556u, 1u, name, "M"),
                                          ip, 1000ULL + i));
    }
    LAN_CHECK(hosts.size() == 30u);
    for (const auto& h : hosts) {
        LAN_CHECK(h.hostName[0] != '\0');
        LAN_CHECK(h.connectIp[0] != '\0');
        LAN_CHECK(h.tcpPort == 5556u);
    }
    const auto plan30 = NativeMenu_PlanJoinEntries(false, hosts, false, {}, "127.0.0.1", true, NATIVE_JOIN_MAX_SLOTS);
    LAN_CHECK(plan30.size() == static_cast<size_t>(NATIVE_JOIN_MAX_SLOTS));
    size_t lanSlots = 0;
    for (const auto& e : plan30) {
        LAN_CHECK(e.label.size() <= NATIVE_JOIN_MAX_LABEL_CHARS);
        if (e.kind == JOIN_ACT_LAN_HOST) ++lanSlots;
    }
    LAN_CHECK(lanSlots == 23u);
    LAN_CHECK(plan30[23].kind == JOIN_ACT_CONNECT_ADDR);
    LAN_CHECK(plan30[24].kind == JOIN_ACT_TYPE_MANUAL);
    LAN_CHECK(plan30[25].kind == JOIN_ACT_PASTE_CLIPBOARD);
    LAN_CHECK(plan30[26].kind == JOIN_ACT_REFRESH_HOSTS);
    LAN_CHECK(plan30[27].kind == JOIN_ACT_BACK);
    JoinPlanEntry backSel = {};
    LAN_CHECK(NativeMenu_ResolveJoinClick(plan30, 27, &backSel) && backSel.kind == JOIN_ACT_BACK);

    const auto browserPlan = NativeMenu_PlanJoinEntries(true, browserHosts, false, {}, "127.0.0.1", true, NATIVE_JOIN_MAX_SLOTS);
    bool sawLiveInPlan = false;
    for (const auto& e : browserPlan) {
        if (e.kind == JOIN_ACT_LAN_HOST && std::strcmp(e.lanHost.hostName, liveName.c_str()) == 0) {
            sawLiveInPlan = true;
            break;
        }
    }
    LAN_CHECK(sawLiveInPlan);

    std::vector<SteamFriendEntry> friends;
    SteamP2P_GetOnlineFriends(friends);

    ScopedExePodRestore<int> screenGuard(MENU_OBJ_RVA + MENU_SCREEN_ID_OFF, 1);
    if (screenGuard.ptr != nullptr) {
        int baselineVtx = 0;
        {
            ScopedImGuiFrame baseFrame;
            LAN_CHECK(baseFrame.inFrame);
            NativeMenu_DrawOverlay();
            baselineVtx = baseFrame.RenderAndCountVertices();
            LAN_CHECK(baselineVtx >= 0);
        }
        *screenGuard.ptr = 2;
        ScopedHostOnNextLoadGuard hostNextGuard(true);
        {
            ScopedImGuiFrame activeFrame;
            LAN_CHECK(activeFrame.inFrame);
            NativeMenu_DrawOverlay();
            LAN_CHECK(activeFrame.NextFrame());
            NativeMenu_DrawOverlay();
            const int activeVtx = activeFrame.RenderAndCountVertices();
            LAN_CHECK(activeVtx > 0);
            LAN_CHECK(activeVtx > baselineVtx);
        }
    }
    return true;
}

static bool T2_F11_02_FortySevenCharHostNameNullTerminated() {
    const char* name47 = "VeryLongSovietRepublicServerNameWithLotsOfText!";
    LAN_CHECK(std::strlen(name47) == 47u);
    const LanAnnouncePacket pkt = MakeAnnouncePacket(0x4747ULL, 5000u, 5556u, 1u, name47, "M");
    std::vector<uint8_t> wire(sizeof(LanAnnouncePacket), 0);
    LAN_CHECK(LanDiscovery_SerializeAnnounce(pkt, wire.data(), wire.size()) == sizeof(LanAnnouncePacket));
    LanAnnouncePacket parsed = {};
    LAN_CHECK(LanDiscovery_ParseAnnounce(wire.data(), wire.size(), &parsed));
    std::vector<LanHostEntry> hosts;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, parsed, "192.168.1.77", 1000ULL));
    LAN_CHECK(std::strlen(hosts[0].hostName) == 47u);
    LAN_CHECK(hosts[0].hostName[47] == '\0');
    return true;
}

static bool T2_F11_03_ClipboardPasteValidAndGarbageAddresses() {
    TwoStates_SetSavedJoinAddress("127.0.0.1");
    if (IsClipboardOpenable()) {
        LAN_CHECK(VerifyClipboardRoundTrip("  192.168.1.44:5556 \r\n"));
        std::string clip;
        LAN_CHECK(ReadClipboardText(clip));
        char host[64] = {};
        uint16_t port = 0;
        LAN_CHECK(NetworkNormalizeTargetAddress(clip.c_str(), host, sizeof(host), &port));
        LAN_CHECK(std::strcmp(host, "192.168.1.44") == 0 && port == 5556u);
        TwoStates_SetSavedJoinAddress(host);
        LAN_CHECK(std::strcmp(TwoStates_GetSavedJoinAddress(), "192.168.1.44") == 0);

        LAN_CHECK(VerifyClipboardRoundTrip("invalid_garbage_text"));
        clip.clear();
        LAN_CHECK(ReadClipboardText(clip));
        LAN_CHECK(!NetworkNormalizeTargetAddress(clip.c_str(), host, sizeof(host), &port));
    }
    return true;
}

static bool T2_F11_04_ManualJoinCustomPortPreserved() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    uint16_t customPort = 0;
    ScopedSocket listener(OpenLoopbackTcpListener(&customPort));
    LAN_CHECK(listener.sock != INVALID_SOCKET && customPort != 0);

    char input[64] = {};
    _snprintf_s(input, sizeof(input), _TRUNCATE, "127.0.0.1:%u", static_cast<unsigned int>(customPort));
    char host[64] = {};
    uint16_t outPort = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress(input, host, sizeof(host), &outPort));
    LAN_CHECK(outPort == customPort);
    TwoStates_SetSavedJoinAddress(input);
    LAN_CHECK(std::strcmp(TwoStates_GetSavedJoinAddress(), input) == 0);

    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(ConnectLoopbackWithAccept(listener, host, outPort, 2000, &srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);
    return true;
}

static bool T2_F11_05_StoppedHostVanishesFromJoinList() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const std::string hName = UniqueTag("T2F1105");
    LAN_CHECK(StartHostScan(5556u, 0x8888u, hName.c_str(), "ByeMap"));

    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &found));
    LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    LAN_CHECK(PollForHostGone(found.serverGuid, 2500));
    LAN_CHECK(CheckLocalPcHostWaitingIsolated());
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3: Cross-Feature Pairwise Combinations (13 Test Cases)
// ---------------------------------------------------------------------------

static bool T3_01_MultiNicClassificationPlusClientDedup() {
    const LanAdapterKind vpnKind = LanDiscovery_ClassifyAdapter(kIfPropVirt, "Radmin VPN", "Radmin", "26.5.5.5", nullptr);
    const LanAdapterKind lanKind = LanDiscovery_ClassifyAdapter(kIfEthernet, "Ethernet", "Realtek", "192.168.1.5", nullptr);
    const char* const ips[3] = { "26.5.5.5", "192.168.1.5", "127.0.0.1" };
    const LanAdapterKind kinds[3] = { vpnKind, lanKind, LanAdapterKind::Ethernet };
    return CheckDedup(0x3101ULL, 4100u, "MultiHost", ips, kinds, 3u, "127.0.0.1", true);
}

static bool T3_02_HostResponderPlusClientScannerByeBurst() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t port = 0;
    const std::string hName = UniqueTag("T302");
    LAN_CHECK(StartHostScan(5556u, 0x3232u, hName.c_str(), "Pair32", &port));

    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &found));
    LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    LAN_CHECK(PollForHostGone(found.serverGuid, 2500));

    // Confirm external probe sees no open=1 reply after stop, and wire bye removes synthetic entry immediately.
    ScopedSocket extCli(OpenBoundUdpLoopback(0, 200));
    LAN_CHECK(extCli.sock != INVALID_SOCKET);
    LanAnnouncePacket afterStop = {};
    LAN_CHECK(!SendProbeAndAwaitAnnounce(extCli.sock, port,
                                           MakeProbePacket(REPUBLICMP_PROTOCOL, 0x3202u, 999982u),
                                           hName.c_str(), &afterStop, 1u, 3));

    std::vector<LanHostEntry> wireStore;
    LAN_CHECK(LanDiscovery_UpsertHost(wireStore, MakeAnnouncePacket(0x3209ULL, 5152u, 5556u, 1u, "ByeWire", "M"),
                                      "192.168.1.44", 1000ULL));
    (void)LanDiscovery_UpsertHost(wireStore, MakeAnnouncePacket(0x3209ULL, 5152u, 5556u, 0u, "ByeWire", "M"),
                                  "192.168.1.44", 1050ULL);
    LAN_CHECK(wireStore.empty());
    return true;
}

static bool T3_03_MultiAdapterPlusClipboardCopyToast() {
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfPropVirt, "Radmin VPN", "Radmin", "26.200.1.2", nullptr) == LanAdapterKind::VirtualVpn);
    LAN_CHECK(VerifyClipboardRoundTrip("26.200.1.2"));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_AddToast("Copied Radmin VPN IP 26.200.1.2", 1);
    LAN_CHECK(MeasureIdleOverlayVertexDelta() > 0);
    TwoStates_OnSessionEnded();
    return true;
}

static bool T3_04_LocalPcWaitingPlusRemoteLanHostCoexist() {
    ScopedLocalHostEvent localEv(true);
    LAN_CHECK(localEv.ev != nullptr);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    LAN_CHECK(SaveSync_IsLocalPcHostWaiting());

    std::vector<LanHostEntry> hosts;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, MakeAnnouncePacket(1ULL, 111u, 5556u, 1u, "SamePC", "M1"),
                                      "127.0.0.1", 1000ULL));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, MakeAnnouncePacket(2ULL, 222u, 5556u, 1u, "RemoteLAN", "M2"),
                                      "192.168.1.50", 1000ULL));
    LAN_CHECK(hosts.size() == 2u);
    LAN_CHECK(hosts[0].sameMachine || hosts[1].sameMachine);
    LAN_CHECK(!hosts[0].sameMachine || !hosts[1].sameMachine);
    return true;
}

static bool T3_05_NormalizedManualLoopbackConnectSteamFree() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    if (GetModuleHandleA("steam_api64.dll") == nullptr) {
        LAN_CHECK(!SteamP2P_IsAvailable());
    }
    TwoStates_SetSavedJoinAddress("127.0.0.1");
    LAN_CHECK(std::strcmp(TwoStates_GetSavedJoinAddress(), "127.0.0.1") == 0);

    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(ConnectNorm(" \t127.0.0.1:%u\r\n ", &srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);

    const char kPing[12] = "T305_PINGOK";
    char rx[12] = {};
    LAN_CHECK(NetworkSendExact(cliGuard.sock, kPing, sizeof(kPing)));
    LAN_CHECK(NetworkRecvExact(srvGuard.sock, rx, sizeof(rx)));
    LAN_CHECK(std::memcmp(kPing, rx, sizeof(kPing)) == 0);
    return true;
}

static bool T3_06_SteamOfflinePlusDiskWorkshopModFallback() {
    if (!SteamP2P_IsLoggedOn()) {
        LAN_CHECK(!SteamP2P_IsAvailable());
        LAN_CHECK(!ModSync_IsSteamUgcOnline());
    }
    ScopedTempDir tmp("t306");
    return CheckModDir(tmp.dir / "common" / "SovietRepublic",
                       tmp.dir / "workshop" / "content" / "784150" / "555123", 555123ULL, true);
}

static bool T3_07_FullLoopbackModSyncHandshakeSteamFree() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    if (!SteamP2P_IsLoggedOn()) {
        LAN_CHECK(!SteamP2P_IsAvailable());
        LAN_CHECK(!ModSync_IsSteamUgcOnline());
    }
    ScopedTempDir tmp("t307");
    ScopedModPackMode packGuard(MOD_PACK_SAVE_ONLY);
    ScopedModSyncGuard modGuard;

    const uint64_t kModId = 998877665ULL;
    LAN_CHECK(CheckModDir(tmp.dir / "common" / "SovietRepublic",
                          tmp.dir / "workshop" / "content" / "784150" / "998877665", kModId, true));
    const std::filesystem::path saveWithMod = tmp.dir / "save_with_mod";
    LAN_CHECK(WriteHeaderBinWithMods(saveWithMod, { kModId }));

    ModSyncHostResult hostRes = MOD_SYNC_HOST_ERROR;
    ModSyncClientResult cliRes = MOD_SYNC_CLIENT_ERROR;
    LAN_CHECK(RunModSync(saveWithMod, &hostRes, &cliRes, nullptr, 0));
    LAN_CHECK(hostRes == MOD_SYNC_HOST_OK);
    LAN_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
    LAN_CHECK(!ModSync_IsDownloadActive());

    ModSync_SetTestSessionFilter(true, &kModId, 1);
    LAN_CHECK(ModSync_IsModIdAllowedInSession(kModId));
    LAN_CHECK(!ModSync_IsModIdAllowedInSession(111222333ULL));
    ModSync_EndSession();
    LAN_CHECK(ModSync_IsModIdAllowedInSession(111222333ULL));
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    return true;
}

static bool VerifyFailedJoinEmittedErrorToastInIdle(const char* targetAddr) {
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_OnSessionEnded();
    TwoStates_ClearToastsForTest();
    LAN_CHECK(TwoStates_GetToastCountForTest() == 0);
    ScopedSessionDisconnect disconnectGuard;
    SaveSync_StartClient(targetAddr);
    LAN_CHECK(WaitForSessionIdle(5000));
    LAN_CHECK(SaveSync_GetLastError()[0] != '\0');
    const int toastCount = TwoStates_GetToastCountForTest();
    LAN_CHECK(toastCount >= 1);
    char toastTxt[256] = {};
    int toastKind = -1;
    LAN_CHECK(TwoStates_GetToastForTest(toastCount - 1, toastTxt, sizeof(toastTxt), &toastKind, nullptr));
    LAN_CHECK(toastTxt[0] != '\0' && toastKind == 3);
    TwoStates_OnSessionEnded();
    LAN_CHECK(TwoStates_GetToastCountForTest() == toastCount);
    LAN_CHECK(MeasureIdleOverlayVertexDelta() > 0);
    TwoStates_ClearToastsForTest();
    return true;
}

static bool T3_08_FailedJoinErrorToastPreservedInIdle() {
    const uint16_t closedPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    char addr[64] = {};
    _snprintf_s(addr, sizeof(addr), _TRUNCATE, "127.0.0.1:%u",
                static_cast<unsigned int>(closedPort ? closedPort : 59997u));
    return VerifyFailedJoinEmittedErrorToastInIdle(addr);
}

static bool T3_09_IncompatibleProtocolWarningFlaggedInBrowser() {
    const LanAnnouncePacket raw = MakeAnnouncePacket(
        0x3900ULL, 6001u, 5556u, 1u, "OutdatedHost", "M", static_cast<uint16_t>(REPUBLICMP_PROTOCOL - 1));
    LanAnnouncePacket parsed = {};
    LAN_CHECK(LanDiscovery_ParseAnnounce(&raw, sizeof(raw), &parsed));
    std::vector<LanHostEntry> hosts;
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, parsed, "192.168.1.91", 1000ULL));
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(!hosts[0].protocolMatch);
    return true;
}

static bool T3_10_LocalHostEventPlusLanResponderConcurrent() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    ScopedLocalHostEvent localEv(true);
    LAN_CHECK(localEv.ev != nullptr);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    LAN_CHECK(SaveSync_IsLocalPcHostWaiting());
    uint16_t udpPort = 0;
    const std::string hName = UniqueTag("T310");
    LAN_CHECK(StartResponder(&udpPort, 1u, hName.c_str(), "DualMap"));

    if (localEv.resetOnExit) {
        LAN_CHECK(SaveSync_IsLocalPcHostWaiting());
    }
    LanAnnouncePacket reply = {};
    LAN_CHECK(ProbeHost(udpPort, 10u, 999980u, hName.c_str(), &reply));
    return true;
}

static bool T3_11_MapInspectModeAndHostStopTeardown() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    uint16_t udpPort = 0;
    LAN_CHECK(StartResponder(&udpPort, 1u, UniqueTag("T311").c_str(), "InspectMap"));

    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    SyncBlock_SetActive(false, nullptr);
    SyncBlock_SetMapInspectMode(true);
    LAN_CHECK(SyncBlock_IsMapInspectMode());
    LAN_CHECK(VerifyClipboardRoundTrip("192.168.1.10"));

    LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    LAN_CHECK(!SyncBlock_UpdateMapInspectState(static_cast<int>(SESSION_IDLE), false));
    LAN_CHECK(!SyncBlock_IsMapInspectMode());
    return true;
}

static bool T3_12_ManualJoinModalSteamVsIpv4Routing() {
    LAN_CHECK(CheckSteamNorm("steam:76561198099999999", 76561198099999999ULL));
    LAN_CHECK(CheckNorm(" 192.168.1.99:5556 ", 0, "192.168.1.99", 5556u));
    LAN_CHECK(SteamP2P_ParseTargetSteamId("192.168.1.99") == 0ULL);
    TwoStates_SetSavedJoinAddress("192.168.1.99:5556");
    LAN_CHECK(std::strcmp(TwoStates_GetSavedJoinAddress(), "192.168.1.99:5556") == 0);
    return true;
}

static bool T3_13_MultiNicRadminHamachiZeroTierTargetPlanningPure() {
    struct NicSpec {
        uint32_t ifType;
        const char* name;
        const char* desc;
        const char* ip;
        uint8_t prefix;
        LanAdapterKind expectedKind;
        const char* expectedMask;
        const char* expectedBroadcast;
    };
    const NicSpec specs[7] = {
        { kIfEthernet, "Ethernet", "Intel Ethernet", "192.168.10.25", 24, LanAdapterKind::Ethernet, "255.255.255.0", "192.168.10.255" },
        { kIfWifi, "Wi-Fi", "Intel Wi-Fi 6", "10.20.0.14", 16, LanAdapterKind::WiFi, "255.255.0.0", "10.20.255.255" },
        { kIfPropVirt, "Radmin VPN", "Famatech Radmin VPN", "26.144.82.10", 8, LanAdapterKind::VirtualVpn, "255.0.0.0", "26.255.255.255" },
        { kIfPropVirt, "Hamachi", "LogMeIn Hamachi", "25.41.12.9", 8, LanAdapterKind::VirtualVpn, "255.0.0.0", "25.255.255.255" },
        { kIfEthernet, "ZeroTier One", "ZeroTier Virtual Port", "172.24.10.5", 16, LanAdapterKind::VirtualVpn, "255.255.0.0", "172.24.255.255" },
        { kIfTunnel, "Tailscale", "Tailscale Tunnel", "100.64.0.5", 10, LanAdapterKind::VirtualVpn, "255.192.0.0", "100.127.255.255" },
        { kIfEthernet, "vEthernet (Default Switch)", "Hyper-V Virtual Ethernet", "172.29.0.1", 16, LanAdapterKind::VirtualLocal, "255.255.0.0", "172.29.255.255" }
    };

    int prios[7] = {};
    std::vector<LanAdapterInfo> simAdapters;
    for (int i = 0; i < 7; ++i) {
        const LanAdapterKind k = LanDiscovery_ClassifyAdapter(
            specs[i].ifType, specs[i].name, specs[i].desc, specs[i].ip, &prios[i]);
        LAN_CHECK(k == specs[i].expectedKind);
        char mask[16] = {};
        char bcast[16] = {};
        LAN_CHECK(LanDiscovery_ComputeBroadcast(specs[i].ip, specs[i].prefix, mask, sizeof(mask), bcast, sizeof(bcast)));
        LAN_CHECK(std::strcmp(mask, specs[i].expectedMask) == 0);
        LAN_CHECK(std::strcmp(bcast, specs[i].expectedBroadcast) == 0);

        LanAdapterInfo ad = {};
        std::strncpy(ad.ip, specs[i].ip, sizeof(ad.ip) - 1);
        std::strncpy(ad.mask, mask, sizeof(ad.mask) - 1);
        std::strncpy(ad.broadcast, bcast, sizeof(ad.broadcast) - 1);
        std::strncpy(ad.name, specs[i].name, sizeof(ad.name) - 1);
        ad.kind = k;
        ad.priority = prios[i];
        ad.ifIndex = static_cast<uint32_t>(10 + i);
        simAdapters.push_back(ad);
    }

    LAN_CHECK(prios[0] > prios[1]); // Ethernet > WiFi
    LAN_CHECK(prios[1] > prios[2]); // WiFi > VirtualVpn
    for (int i = 2; i <= 5; ++i) {
        LAN_CHECK(prios[i] > prios[6]); // Radmin/Hamachi/ZeroTier/Tailscale > Hyper-V VirtualLocal
    }

    // Pure target fan-out planning (zero OS network transmission).
    const std::vector<std::string> extraPeers = { "127.0.0.1", "26.144.82.99", "192.168.10.99" };
    const std::vector<LanDiscoveryTarget> prodTargets =
        LanDiscovery_PlanSendTargets(simAdapters, LAN_DISCOVERY_PORT, extraPeers, 44444u, false);
    auto hasTargetIp = [](const std::vector<LanDiscoveryTarget>& list, const char* ip) -> bool {
        for (const auto& t : list) {
            if (std::strcmp(t.ip, ip) == 0) return true;
        }
        return false;
    };
    auto countIpPort = [](const std::vector<LanDiscoveryTarget>& list, const char* ip, uint16_t port) -> size_t {
        size_t n = 0;
        for (const auto& t : list) {
            if (std::strcmp(t.ip, ip) == 0 && t.port == port) ++n;
        }
        return n;
    };
    auto allTargetsUnique = [](const std::vector<LanDiscoveryTarget>& list) -> bool {
        for (size_t i = 0; i < list.size(); ++i) {
            for (size_t j = i + 1; j < list.size(); ++j) {
                if (std::strcmp(list[i].ip, list[j].ip) == 0 &&
                    list[i].port == list[j].port &&
                    list[i].ifIndex == list[j].ifIndex) {
                    return false;
                }
            }
        }
        return true;
    };
    LAN_CHECK(allTargetsUnique(prodTargets));
    LAN_CHECK(countIpPort(prodTargets, "127.0.0.1", LAN_DISCOVERY_PORT) == 1u);
    LAN_CHECK(countIpPort(prodTargets, "127.0.0.1", 44444u) == 1u);
    LAN_CHECK(hasTargetIp(prodTargets, "127.0.0.1"));
    LAN_CHECK(hasTargetIp(prodTargets, "192.168.10.255"));
    LAN_CHECK(hasTargetIp(prodTargets, "26.255.255.255"));
    LAN_CHECK(hasTargetIp(prodTargets, "25.255.255.255"));
    LAN_CHECK(hasTargetIp(prodTargets, "172.24.255.255"));
    LAN_CHECK(hasTargetIp(prodTargets, "255.255.255.255"));
    LAN_CHECK(hasTargetIp(prodTargets, "26.144.82.99"));

    const std::vector<LanDiscoveryTarget> loopTargets =
        LanDiscovery_PlanSendTargets(simAdapters, LAN_DISCOVERY_PORT, extraPeers, 44444u, true);
    LAN_CHECK(allTargetsUnique(loopTargets));
    LAN_CHECK(loopTargets.size() == 2u);
    LAN_CHECK(countIpPort(loopTargets, "127.0.0.1", LAN_DISCOVERY_PORT) == 1u);
    LAN_CHECK(countIpPort(loopTargets, "127.0.0.1", 44444u) == 1u);
    for (const auto& t : loopTargets) {
        LAN_CHECK(t.isLoopback);
        LAN_CHECK(std::strcmp(t.ip, "127.0.0.1") == 0);
    }

    const std::vector<LanDiscoveryTarget> loopOnlyNoPeers =
        LanDiscovery_PlanSendTargets(simAdapters, LAN_DISCOVERY_PORT, {}, 0, true);
    LAN_CHECK(loopOnlyNoPeers.size() == 1u);
    LAN_CHECK(loopOnlyNoPeers[0].isLoopback);
    LAN_CHECK(std::strcmp(loopOnlyNoPeers[0].ip, "127.0.0.1") == 0);
    LAN_CHECK(loopOnlyNoPeers[0].port == LAN_DISCOVERY_PORT);

    std::vector<LanHostEntry> hosts;
    const LanAnnouncePacket pkt = MakeAnnouncePacket(0x3130ULL, 6100u, 5556u, 1u, "TargetPlanHost", "PlanMap");
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "26.144.82.10", 1000ULL, LanAdapterKind::VirtualVpn));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "25.41.12.9", 1005ULL, LanAdapterKind::VirtualVpn));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "172.24.10.5", 1010ULL, LanAdapterKind::VirtualVpn));
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "192.168.10.25", 1015ULL, LanAdapterKind::Ethernet));
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(std::strcmp(hosts[0].connectIp, "192.168.10.25") == 0);
    LAN_CHECK(LanDiscovery_UpsertHost(hosts, pkt, "127.0.0.1", 1020ULL, LanAdapterKind::Ethernet));
    LAN_CHECK(hosts.size() == 1u);
    LAN_CHECK(std::strcmp(hosts[0].connectIp, "127.0.0.1") == 0);
    LAN_CHECK(hosts[0].sameMachine);
    return true;
}

// ---------------------------------------------------------------------------
// Tier 4: Real-World Application Scenarios (8 End-to-End Scenarios)
// ---------------------------------------------------------------------------

static bool T4_01_FullSteamFreeUdpDiscoveryAndTcpHandshake() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    ScopedTempDir tmp("t401");
    ScopedModPackMode packGuard(MOD_PACK_SAVE_ONLY);
    ScopedModSyncGuard modGuard;

    if (GetModuleHandleA("steam_api64.dll") == nullptr) {
        LAN_CHECK(!SteamP2P_IsAvailable());
        LAN_CHECK(!ModSync_IsSteamUgcOnline());
    }

    uint16_t tcpPort = 0;
    ScopedSocket tcpSrv(OpenLoopbackTcpListener(&tcpPort));
    LAN_CHECK(tcpSrv.sock != INVALID_SOCKET && tcpPort != 0);

    const std::string hName = UniqueTag("T401Host");
    LAN_CHECK(StartHostScan(tcpPort, 0xA001u, hName.c_str(), "RepublicMap"));

    LanHostEntry discovered = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &discovered, true));

    char targetBuf[64] = {};
    _snprintf_s(targetBuf, sizeof(targetBuf), _TRUNCATE, "%s:%u",
                discovered.connectIp, static_cast<unsigned int>(discovered.tcpPort));
    char normHost[64] = {};
    uint16_t normPort = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress(targetBuf, normHost, sizeof(normHost), &normPort));

    SOCKET srvRaw = INVALID_SOCKET;
    SOCKET cliSock = INVALID_SOCKET;
    LAN_CHECK(ConnectLoopbackWithAccept(tcpSrv, normHost, normPort, 3000, &srvRaw, &cliSock));
    ScopedSocket srvSock(srvRaw);
    ScopedSocket cliGuard(cliSock);

    ModSyncHostResult hostRes = MOD_SYNC_HOST_ERROR;
    char hostErr[256] = {};
    std::thread hostTh([&]() {
        hostRes = ModSync_HostHandshake(srvSock.sock, tmp.dir.string().c_str(), hostErr, sizeof(hostErr));
    });
    char cliErr[256] = {};
    const ModSyncClientResult cliRes = ModSync_ClientHandshake(cliGuard.sock, cliErr, sizeof(cliErr));
    hostTh.join();

    LAN_CHECK(hostRes == MOD_SYNC_HOST_OK);
    LAN_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
    return true;
}

static bool T4_02_MultiAdapterVirtualLanAndClipboardWorkflow() {
    int pEth = 0;
    int pRad = 0;
    int pHam = 0;
    int pHv = 0;
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfEthernet, "Ethernet", "Intel", "192.168.1.10", &pEth) == LanAdapterKind::Ethernet);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfPropVirt, "Radmin VPN", "Radmin", "26.10.20.30", &pRad) == LanAdapterKind::VirtualVpn);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfPropVirt, "Hamachi", "Hamachi", "25.10.20.30", &pHam) == LanAdapterKind::VirtualVpn);
    LAN_CHECK(LanDiscovery_ClassifyAdapter(kIfEthernet, "vEthernet (Default Switch)", "Hyper-V", "172.29.0.1", &pHv) == LanAdapterKind::VirtualLocal);
    LAN_CHECK(pEth > pRad && pRad > pHv && pHam > pHv);

    const char* const ips[4] = { "26.10.20.30", "25.10.20.30", "192.168.1.10", "127.0.0.1" };
    const LanAdapterKind kinds[4] = {
        LanAdapterKind::VirtualVpn, LanAdapterKind::VirtualVpn, LanAdapterKind::Ethernet, LanAdapterKind::Ethernet
    };
    LAN_CHECK(CheckDedup(0x4200ULL, 7100u, "VpnHost", ips, kinds, 4u, "127.0.0.1", true));
    LAN_CHECK(VerifyClipboardRoundTrip("26.10.20.30"));
    return CheckNorm("26.10.20.30:5556", 0, "26.10.20.30", 5556u);
}

static bool T4_03_HostStopRestartAndSilentTtlRecovery() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const std::string hName1 = UniqueTag("T403A");
    const std::string hName2 = UniqueTag("T403B");
    LAN_CHECK(StartHostScan(5556u, 0x4300u, hName1.c_str(), "Phase1"));

    LanHostEntry g1 = {};
    LAN_CHECK(PollForHostByName(hName1.c_str(), 3000, &g1));

    // Step 1: graceful stop -> bye burst purges G1.
    LanDiscovery_SetHosting(false, 5556u, 1u, "", "");
    LAN_CHECK(PollForHostGone(g1.serverGuid, 2500));

    // Step 2: restart with new serverGuid -> discovered cleanly.
    LanDiscovery_SetHosting(true, 5556u, 1u, hName2.c_str(), "Phase2");
    LanHostEntry g2 = {};
    LAN_CHECK(PollForHostByName(hName2.c_str(), 3000, &g2));

    // Step 3: silent-drop TTL expiration on pure store.
    std::vector<LanHostEntry> silentStore;
    LAN_CHECK(LanDiscovery_UpsertHost(silentStore, MakeAnnouncePacket(0x9999ULL, 888u, 5556u, 1u, "Ghost", "M"),
                                      "192.168.1.99", 10000ULL));
    LanDiscovery_PruneExpired(silentStore, 10000ULL + LAN_DISCOVERY_TTL_MS + 1ULL, LAN_DISCOVERY_TTL_MS);
    LAN_CHECK(silentStore.empty());
    return true;
}

static bool T4_04_CorruptedUdpStormWithValidHostDiscovery() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    const uint16_t port = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(port != 0);
    const std::string hName = UniqueTag("T404");

    LanDiscovery_SetPortForTest(port);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 0x4400u, true);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, hName.c_str(), "StormMap");
    LanDiscovery_StartScan();

    std::atomic<bool> stopStorm{ false };
    std::thread stormTh([&]() {
        ScopedSocket s(OpenBoundUdpLoopback(0, 200));
        if (s.sock == INVALID_SOCKET) return;
        std::vector<uint8_t> pkt(512, 0xCC);
        for (int i = 0; i < 500 && !stopStorm.load(std::memory_order_relaxed); ++i) {
            const size_t len = static_cast<size_t>((i * 29) % 511 + 1);
            pkt[0] = static_cast<uint8_t>(i);
            (void)SendUdpLoopback(s.sock, port, pkt.data(), len);
            if ((i % 50) == 0) Sleep(2);
        }
    });

    LanHostEntry valid = {};
    const bool found = PollForHostByName(hName.c_str(), 3000, &valid);
    stopStorm.store(true, std::memory_order_relaxed);
    stormTh.join();
    LAN_CHECK(found);
    LAN_CHECK(std::strcmp(valid.mapName, "StormMap") == 0);
    return true;
}

static bool T4_05_OfflineWorkshopContent784150AndWipJoinCycle() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedTempDir tmp("t405");
    ScopedModPackMode packGuard(MOD_PACK_SAVE_ONLY);
    ScopedModSyncGuard modGuard;
    const std::filesystem::path exeDir = tmp.dir / "common" / "SovietRepublic";
    const std::filesystem::path wsMod = tmp.dir / "workshop" / "content" / "784150" / "3823543010";
    const std::filesystem::path wipMod = exeDir / "media_soviet" / "workshop_wip" / "999888";
    LAN_CHECK(CreateWorkshopDir(wsMod, true));
    LAN_CHECK(CreateWorkshopDir(wipMod, true));

    char resolvedWs[MAX_PATH] = {};
    char resolvedWip[MAX_PATH] = {};
    LAN_CHECK(ModSync_FindLocalModFolderAtRoot(3823543010ULL, exeDir.string().c_str(), false,
                                               resolvedWs, sizeof(resolvedWs)));
    LAN_CHECK(ModSync_FindLocalModFolderAtRoot(999888ULL, exeDir.string().c_str(), false,
                                               resolvedWip, sizeof(resolvedWip)));

    // Full real loopback TCP ModSync handshake cycle offline (MOD_SYNC_CLIENT_OK).
    {
        const std::filesystem::path saveOk = tmp.dir / "save_ok";
        std::error_code ec;
        std::filesystem::create_directories(saveOk, ec);
        ModSyncHostResult hostRes = MOD_SYNC_HOST_ERROR;
        ModSyncClientResult cliRes = MOD_SYNC_CLIENT_ERROR;
        LAN_CHECK(RunModSync(saveOk, &hostRes, &cliRes, nullptr, 0));
        ModSync_EndSession();

        LAN_CHECK(hostRes == MOD_SYNC_HOST_OK);
        LAN_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
        LAN_CHECK(!ModSync_IsDownloadActive());
    }

    // Missing offline mod must never block in "Waiting for Steam...".
    if (!ModSync_IsSteamUgcOnline()) {
        const std::filesystem::path saveDir = tmp.dir / "save_missing";
        LAN_CHECK(WriteHeaderBinWithMods(saveDir, { 987654321ULL }));
        ModSync_BeforeWorldLoad(true, saveDir.string().c_str());
        LAN_CHECK(!ModSync_IsDownloadActive());
    }
    return true;
}

static bool T4_06_BackwardCompatLoopbackAndSteamTarget() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    (void)SaveSync_IsDualTestbed();
    {
        ScopedLocalHostEvent ev(true);
        LAN_CHECK(ev.ev != nullptr);
        SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
        LAN_CHECK(SaveSync_IsLocalPcHostWaiting());
    }

    char hSteam[64] = {};
    uint16_t pSteam = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress("steam:76561198012345678", hSteam, sizeof(hSteam), &pSteam));
    LAN_CHECK(SteamP2P_ParseTargetSteamId(hSteam) == 76561198012345678ULL);

    uint16_t port = 0;
    ScopedSocket listener(OpenLoopbackTcpListener(&port));
    LAN_CHECK(listener.sock != INVALID_SOCKET && port != 0);
    char hIp[64] = {};
    uint16_t pIp = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress("127.0.0.1", hIp, sizeof(hIp), &pIp));
    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(ConnectLoopbackWithAccept(listener, hIp, port, 2000, &srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);
    return true;
}

static bool T4_07_JoinFailureIdleToastAndManualRetryRecovery() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    const uint16_t closedPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    char closedAddr[64] = {};
    _snprintf_s(closedAddr, sizeof(closedAddr), _TRUNCATE, "127.0.0.1:%u",
                static_cast<unsigned int>(closedPort ? closedPort : 59999u));
    LAN_CHECK(VerifyFailedJoinEmittedErrorToastInIdle(closedAddr));

    // Retry against an open loopback TCP listener.
    uint16_t openPort = 0;
    ScopedSocket listener(OpenLoopbackTcpListener(&openPort));
    LAN_CHECK(listener.sock != INVALID_SOCKET && openPort != 0);
    char openAddr[64] = {};
    _snprintf_s(openAddr, sizeof(openAddr), _TRUNCATE, "127.0.0.1:%u", static_cast<unsigned int>(openPort));
    char normHost[64] = {};
    uint16_t normPort = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress(openAddr, normHost, sizeof(normHost), &normPort));

    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(ConnectLoopbackWithAccept(listener, normHost, normPort, 2000, &srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);
    SaveSync_RequestDisconnect();
    LAN_CHECK(WaitForSessionIdle(1500));
    return true;
}

static bool T4_08_LoopbackOnlyModeUdpDiscoveryAndTcpAcceptHandshake() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;
    LanDiscovery_SetLoopbackOnlyForTest(true);
    Network_SetLoopbackOnlyForTest(true);

    const uint16_t udpPort = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    const uint16_t tcpPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(udpPort != 0 && tcpPort != 0);

    // Verify Shutdown/Init cycles preserve loopback-only UDP discovery on 127.0.0.1.
    const std::string hName = UniqueTag("T408Loop");
    LanDiscovery_SetPortForTest(udpPort);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 0x4808u, true);
    LanDiscovery_Init();
    LanDiscovery_Shutdown();
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, tcpPort, 1u, hName.c_str(), "LoopbackMap");
    LanDiscovery_StartScan();

    LanHostEntry found = {};
    LAN_CHECK(PollForHostByName(hName.c_str(), 3000, &found, true));
    LAN_CHECK(VerifyLoopbackUdpSocketBound(udpPort));
    LAN_CHECK(std::strcmp(found.connectIp, "127.0.0.1") == 0);
    LAN_CHECK(found.sameMachine);
    LAN_CHECK(found.tcpPort == tcpPort);

    char rawTarget[64] = {};
    _snprintf_s(rawTarget, sizeof(rawTarget), _TRUNCATE, "%s:%u",
                found.connectIp, static_cast<unsigned int>(found.tcpPort));
    char normHost[64] = {};
    uint16_t normPort = 0;
    LAN_CHECK(NetworkNormalizeTargetAddress(rawTarget, normHost, sizeof(normHost), &normPort));
    LAN_CHECK(std::strcmp(normHost, "127.0.0.1") == 0 && normPort == tcpPort);

    volatile bool cancelAccept = false;
    std::atomic<SOCKET> srvSock{ INVALID_SOCKET };
    std::thread acceptTh([normPort, &cancelAccept, &srvSock]() {
        srvSock.store(NetworkHostAcceptOne(normPort, 3000, 3000, &cancelAccept), std::memory_order_release);
    });

    SOCKET cliSockRaw = INVALID_SOCKET;
    const ULONGLONG connectDeadline = GetTickCount64() + 2000ULL;
    while (cliSockRaw == INVALID_SOCKET && GetTickCount64() < connectDeadline) {
        Sleep(25);
        cliSockRaw = NetworkConnect(normHost, normPort, 300, 3000, nullptr);
    }
    if (cliSockRaw == INVALID_SOCKET) {
        cancelAccept = true;
    }
    acceptTh.join();

    ScopedSocket acceptedGuard(srvSock.load(std::memory_order_acquire));
    ScopedSocket cliGuard(cliSockRaw);
    LAN_CHECK(acceptedGuard.sock != INVALID_SOCKET && cliGuard.sock != INVALID_SOCKET);

    char srvPeer[64] = {};
    char cliPeer[64] = {};
    NetworkGetPeerName(acceptedGuard.sock, srvPeer, sizeof(srvPeer));
    NetworkGetPeerName(cliGuard.sock, cliPeer, sizeof(cliPeer));
    const char kPayload[16] = "LOOPBACK_OK_127";
    char rxBuf[16] = {};
    const bool txOk = NetworkSendExact(cliGuard.sock, kPayload, sizeof(kPayload));
    const bool rxOk = txOk && NetworkRecvExact(acceptedGuard.sock, rxBuf, sizeof(rxBuf));

    LAN_CHECK(std::strcmp(srvPeer, "127.0.0.1") == 0);
    LAN_CHECK(std::strcmp(cliPeer, "127.0.0.1") == 0);
    LAN_CHECK(txOk);
    LAN_CHECK(rxOk);
    LAN_CHECK(std::memcmp(kPayload, rxBuf, sizeof(kPayload)) == 0);
    return true;
}

// ---------------------------------------------------------------------------
// Tier 5: Adversarial & Regression Tests (14 Test Cases: T5-01 .. T5-14)
// ---------------------------------------------------------------------------

static bool CheckCustomHelloRejection(const char* magic, int protocol, const char* version, unsigned int dllCrc) {
    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);

    bool hostOk = true;
    char hostWhy[256] = {};
    char cliWhy[256] = {};
    std::thread hostTh([&]() {
        hostOk = SaveSync_ExchangeHelloOverSocketForTest(static_cast<uintptr_t>(srvGuard.sock),
                                                         true, hostWhy, sizeof(hostWhy));
    });
    const bool cliOk = SaveSync_ExchangeCustomHelloOverSocketForTest(
        static_cast<uintptr_t>(cliGuard.sock), false, magic, protocol, version, dllCrc,
        cliWhy, sizeof(cliWhy));
    hostTh.join();
    LAN_CHECK(!hostOk && hostWhy[0] != '\0');
    LAN_CHECK(cliOk);
    return true;
}

static bool T5_01_SaveSyncCustomHelloRejections() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    const unsigned int crc = SaveSync_GetLocalDllCrc();
    LAN_CHECK(CheckCustomHelloRejection("BADMAGC", REPUBLICMP_PROTOCOL, REPUBLICMP_VERSION, crc));
    LAN_CHECK(CheckCustomHelloRejection("RPMPSES", REPUBLICMP_PROTOCOL - 1, REPUBLICMP_VERSION, crc));
    LAN_CHECK(CheckCustomHelloRejection("RPMPSES", REPUBLICMP_PROTOCOL, "0.0.1-bad", crc));
    LAN_CHECK(CheckCustomHelloRejection("RPMPSES", REPUBLICMP_PROTOCOL, REPUBLICMP_VERSION, crc ^ 1u));

    // Also verify Client side rejects mismatch when Host sends bad hello
    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);
    bool hostOk = false;
    char hostWhy[256] = {};
    char cliWhy[256] = {};
    std::thread hostTh([&]() {
        hostOk = SaveSync_ExchangeCustomHelloOverSocketForTest(
            static_cast<uintptr_t>(srvGuard.sock), true, "BADMAGC", REPUBLICMP_PROTOCOL - 1,
            "0.0.1-bad", crc ^ 1u, hostWhy, sizeof(hostWhy));
    });
    const bool cliOk = SaveSync_ExchangeHelloOverSocketForTest(
        static_cast<uintptr_t>(cliGuard.sock), false, cliWhy, sizeof(cliWhy));
    hostTh.join();
    LAN_CHECK(hostOk);
    LAN_CHECK(!cliOk && cliWhy[0] != '\0');
    return true;
}

static bool RunFullHandshakePair(const char* hostDlc, bool hostCheck,
                                 const char* cliDlc, bool cliCheck,
                                 const char* saveDir,
                                 bool expectHostOk, bool expectCliOk) {
    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);

    bool hostOk = false;
    char hostWhy[256] = {};
    char cliWhy[256] = {};
    std::thread hostTh([&]() {
        hostOk = SaveSync_ExchangeHandshakeOverSocketForTest(
            static_cast<uintptr_t>(srvGuard.sock), true, hostDlc, hostCheck, saveDir, hostWhy, sizeof(hostWhy));
    });
    const bool cliOk = SaveSync_ExchangeHandshakeOverSocketForTest(
        static_cast<uintptr_t>(cliGuard.sock), false, cliDlc, cliCheck, "", cliWhy, sizeof(cliWhy));
    hostTh.join();
    LAN_CHECK(hostOk == expectHostOk);
    LAN_CHECK(cliOk == expectCliOk);
    if (!expectHostOk) LAN_CHECK(hostWhy[0] != '\0');
    if (!expectCliOk) LAN_CHECK(cliWhy[0] != '\0');
    return true;
}

static bool T5_02_SaveSyncFullHandshakeAndDlcNegotiation() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedTempDir tmp("t502");
    ScopedModPackMode packGuard(MOD_PACK_SAVE_ONLY);
    ScopedModSyncGuard modGuard;

    LAN_CHECK(RunFullHandshakePair("dlc2,dlc3", true, "dlc2", true, tmp.dir.string().c_str(), false, false));
    LAN_CHECK(RunFullHandshakePair("dlc2,dlc3", true, "dlc2", false, tmp.dir.string().c_str(), true, true));
    LAN_CHECK(RunFullHandshakePair("", true, "", true, tmp.dir.string().c_str(), true, true));
    return true;
}

static bool T5_03_NetworkConnectCancelTimeoutAndInvalidPort() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    const uint16_t closedPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(closedPort != 0);

    // port == 0 -> INVALID_SOCKET immediately
    LAN_CHECK(NetworkConnect("127.0.0.1", 0, 200, 200) == INVALID_SOCKET);

    // Short connectTimeoutMs expiration
    const ULONGLONG tTimeoutStart = GetTickCount64();
    LAN_CHECK(NetworkConnect("127.0.0.1", closedPort, 100, 500, nullptr) == INVALID_SOCKET);
    LAN_CHECK((GetTickCount64() - tTimeoutStart) < 1500ULL);

    // Pre-set cancelFlag abort on NetworkConnect
    volatile bool preCancel = true;
    LAN_CHECK(NetworkConnect("127.0.0.1", closedPort, 5000, 500, &preCancel) == INVALID_SOCKET);

    // Mid-wait cancelFlag abort at ~40ms on NetworkHostAcceptOne
    const uint16_t listenPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(listenPort != 0);
    volatile bool midCancel = false;
    std::thread cancelTh([&midCancel]() {
        Sleep(40);
        midCancel = true;
    });
    const ULONGLONG tAcceptStart = GetTickCount64();
    const SOCKET acc = NetworkHostAcceptOne(listenPort, 5000, 500, &midCancel);
    const ULONGLONG elapsed = GetTickCount64() - tAcceptStart;
    cancelTh.join();
    LAN_CHECK(acc == INVALID_SOCKET);
    LAN_CHECK(elapsed < 1500ULL);
    return true;
}

static bool T5_04_NetworkExactClosedPeerAndZeroByteFile() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    char dummy[8] = { 1, 2, 3, 4 };
    LAN_CHECK(!NetworkSendExact(INVALID_SOCKET, dummy, sizeof(dummy)));
    LAN_CHECK(!NetworkRecvExact(INVALID_SOCKET, dummy, sizeof(dummy)));

    SOCKET srv = INVALID_SOCKET;
    SOCKET cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    NetworkCloseSocket(cli);
    LAN_CHECK(!NetworkRecvExact(srvGuard.sock, dummy, sizeof(dummy)));

    // NetworkSendFile / NetworkReceiveFile null & empty paths
    LAN_CHECK(!NetworkSendFile(srvGuard.sock, nullptr));
    LAN_CHECK(!NetworkSendFile(srvGuard.sock, ""));
    LAN_CHECK(!NetworkReceiveFile(srvGuard.sock, nullptr));
    LAN_CHECK(!NetworkReceiveFile(srvGuard.sock, ""));

    // Genuine 0-byte file roundtrip over loopback TCP
    ScopedTempDir tmp("t504");
    const std::filesystem::path srcZero = tmp.dir / "empty_in.bin";
    const std::filesystem::path dstZero = tmp.dir / "empty_out.bin";
    {
        FILE* f = nullptr;
        LAN_CHECK(fopen_s(&f, srcZero.string().c_str(), "wb") == 0 && f != nullptr);
        std::fclose(f);
    }
    SOCKET fSrv = INVALID_SOCKET;
    SOCKET fCli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&fSrv, &fCli));
    ScopedSocket fSrvGuard(fSrv);
    ScopedSocket fCliGuard(fCli);

    bool rxFileOk = false;
    std::thread rxTh([&]() {
        rxFileOk = NetworkReceiveFile(fSrvGuard.sock, dstZero.string().c_str());
    });
    const bool txFileOk = NetworkSendFile(fCliGuard.sock, srcZero.string().c_str());
    rxTh.join();
    LAN_CHECK(txFileOk && rxFileOk);
    std::error_code ec;
    LAN_CHECK(std::filesystem::is_regular_file(dstZero, ec));
    LAN_CHECK(std::filesystem::file_size(dstZero, ec) == 0ULL);
    return true;
}

#pragma pack(push, 1)
struct TestLsHeader { unsigned type; unsigned size; };
struct TestLsGrant { unsigned long long tick; int speed; };
struct TestLsPing { ULONGLONG sentMs; unsigned char reply; };
#pragma pack(pop)

static bool VerifyPeerReceivesGracefulByeOnClose(SOCKET peerSock, bool* outSawClientLogs = nullptr);

static bool T5_05_LockstepNetHostAndJoinLoopbackLifecycle() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;

    const uint16_t lsPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(lsPort != 0);
    LockstepNet_SetPortForTest(lsPort);

    // Part A: LockstepNet_Host + raw TCP client peer
    {
        volatile bool cancelHost = false;
        bool hostRes = false;
        std::thread hostTh([&]() {
            hostRes = LockstepNet_Host(3000, &cancelHost);
        });

        SOCKET rawCli = INVALID_SOCKET;
        for (int i = 0; i < 60 && rawCli == INVALID_SOCKET; ++i) {
            Sleep(20);
            rawCli = NetworkConnect("127.0.0.1", lsPort, 300, 2000, nullptr);
        }
        if (rawCli == INVALID_SOCKET) cancelHost = true;
        hostTh.join();
        ScopedSocket rawCliGuard(rawCli);
        LAN_CHECK(hostRes && rawCliGuard.sock != INVALID_SOCKET);
        LAN_CHECK(LockstepNet_IsConnected() && LockstepNet_IsHost());

        // Ping -> reply -> RTT
        LockstepNet_Ping();
        TestLsHeader pingHdr = {};
        LAN_CHECK(NetworkRecvExact(rawCliGuard.sock, &pingHdr, sizeof(pingHdr)));
        LAN_CHECK(pingHdr.type == 11u && pingHdr.size == sizeof(TestLsPing));
        TestLsPing pingBody = {};
        LAN_CHECK(NetworkRecvExact(rawCliGuard.sock, &pingBody, sizeof(pingBody)));
        pingBody.reply = 1;
        LAN_CHECK(NetworkSendExact(rawCliGuard.sock, &pingHdr, sizeof(pingHdr)));
        LAN_CHECK(NetworkSendExact(rawCliGuard.sock, &pingBody, sizeof(pingBody)));
        ULONGLONG rttMs = 0;
        bool gotRtt = false;
        for (int i = 0; i < 40; ++i) {
            if (LockstepNet_RoundTrip(&rttMs)) { gotRtt = true; break; }
            Sleep(15);
        }
        LAN_CHECK(gotRtt);

        // Grant -> raw client reads -> sends Ack -> host Poll updates Flow
        LockstepNet_Grant(1ULL, 2);
        TestLsHeader grantHdr = {};
        TestLsGrant grantBody = {};
        LAN_CHECK(NetworkRecvExact(rawCliGuard.sock, &grantHdr, sizeof(grantHdr)));
        LAN_CHECK(grantHdr.type == 1u && grantHdr.size == sizeof(TestLsGrant));
        LAN_CHECK(NetworkRecvExact(rawCliGuard.sock, &grantBody, sizeof(grantBody)));
        LAN_CHECK(grantBody.tick == 1ULL && grantBody.speed == 2);

        const TestLsHeader ackHdr = { 4u, static_cast<unsigned>(sizeof(unsigned long long)) };
        const unsigned long long completedTick = 1ULL;
        LAN_CHECK(NetworkSendExact(rawCliGuard.sock, &ackHdr, sizeof(ackHdr)));
        LAN_CHECK(NetworkSendExact(rawCliGuard.sock, &completedTick, sizeof(completedTick)));
        for (int i = 0; i < 30; ++i) {
            LockstepNet_Poll();
            if (LockstepNet_Flow().behind == 0ULL) break;
            Sleep(10);
        }
        LAN_CHECK(LockstepNet_Flow().behind == 0ULL);

        // LS_BYE cleanly disconnects
        const TestLsHeader byeHdr = { 10u, 0u };
        LAN_CHECK(NetworkSendExact(rawCliGuard.sock, &byeHdr, sizeof(byeHdr)));
        for (int i = 0; i < 50 && LockstepNet_IsConnected(); ++i) Sleep(20);
        LAN_CHECK(!LockstepNet_IsConnected());
        LockstepNet_Close();
    }

    // Part B: Raw TCP listener + LockstepNet_Join
    {
        ScopedSocket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        LAN_CHECK(listener.sock != INVALID_SOCKET);
        BOOL reuse = TRUE;
        (void)setsockopt(listener.sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(lsPort);
        LAN_CHECK(bind(listener.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
        LAN_CHECK(listen(listener.sock, 1) == 0);

        std::atomic<SOCKET> srvAccepted{ INVALID_SOCKET };
        std::thread accTh([&]() {
            srvAccepted.store(AcceptLoopbackOne(listener.sock, 3000), std::memory_order_release);
        });
        volatile bool cancelJoin = false;
        const bool joined = LockstepNet_Join("127.0.0.1", 3000, &cancelJoin);
        accTh.join();
        ScopedSocket srvGuard(srvAccepted.load(std::memory_order_acquire));
        LAN_CHECK(joined && srvGuard.sock != INVALID_SOCKET);
        LAN_CHECK(LockstepNet_IsConnected() && !LockstepNet_IsHost());

        const TestLsHeader gHdr = { 1u, static_cast<unsigned>(sizeof(TestLsGrant)) };
        const TestLsGrant gBody = { 1ULL, 2 };
        LAN_CHECK(NetworkSendExact(srvGuard.sock, &gHdr, sizeof(gHdr)));
        LAN_CHECK(NetworkSendExact(srvGuard.sock, &gBody, sizeof(gBody)));
        int spd = 0;
        bool tookGrant = false;
        for (int i = 0; i < 40; ++i) {
            LockstepNet_Poll();
            if (LockstepNet_TakeGrant(1ULL, &spd)) { tookGrant = true; break; }
            Sleep(15);
        }
        LAN_CHECK(tookGrant && spd == 2);

        LockstepNet_Ack(1ULL);
        TestLsHeader ackHdr = {};
        unsigned long long ackTick = 0;
        LAN_CHECK(NetworkRecvExact(srvGuard.sock, &ackHdr, sizeof(ackHdr)));
        LAN_CHECK(ackHdr.type == 4u && ackHdr.size == sizeof(ackTick));
        LAN_CHECK(NetworkRecvExact(srvGuard.sock, &ackTick, sizeof(ackTick)));
        LAN_CHECK(ackTick == 1ULL);

        // Verify LockstepNet_Close() delivers optional LS_CLIENT_LOGS + LS_BYE cleanly without WSAECONNRESET
        LAN_CHECK(VerifyPeerReceivesGracefulByeOnClose(srvGuard.sock));
    }
    return true;
}

static bool T5_06_LockstepNetCancelTimeoutAndIpPortStrip() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;

    const uint16_t lsPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(lsPort != 0);
    LockstepNet_SetPortForTest(lsPort);

    // 1. Host timeout
    volatile bool cancel = false;
    const ULONGLONG t0 = GetTickCount64();
    LAN_CHECK(!LockstepNet_Host(120, &cancel));
    LAN_CHECK((GetTickCount64() - t0) < 1500ULL);

    // 2. Host *cancel early abort
    volatile bool cancelHost = false;
    std::thread hCancelTh([&cancelHost]() { Sleep(60); cancelHost = true; });
    const ULONGLONG t1 = GetTickCount64();
    LAN_CHECK(!LockstepNet_Host(4000, &cancelHost));
    const ULONGLONG elapsedHost = GetTickCount64() - t1;
    hCancelTh.join();
    LAN_CHECK(elapsedHost < 1500ULL);

    // 3. Join *cancel early abort
    volatile bool cancelJoin = false;
    std::thread jCancelTh([&cancelJoin]() { Sleep(60); cancelJoin = true; });
    const ULONGLONG t2 = GetTickCount64();
    LAN_CHECK(!LockstepNet_Join("127.0.0.1", 4000, &cancelJoin));
    const ULONGLONG elapsedJoin = GetTickCount64() - t2;
    jCancelTh.join();
    LAN_CHECK(elapsedJoin < 1500ULL);

    // 4. Strip :port note (SaveSync strips :5556 before LockstepNet_Join)
    char strippedHost[64] = {};
    LAN_CHECK(NetworkNormalizeTargetAddress("127.0.0.1:5556", strippedHost, sizeof(strippedHost), nullptr));
    LAN_CHECK(std::strcmp(strippedHost, "127.0.0.1") == 0);

    ScopedSocket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    LAN_CHECK(listener.sock != INVALID_SOCKET);
    BOOL reuse = TRUE;
    (void)setsockopt(listener.sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(lsPort);
    LAN_CHECK(bind(listener.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    LAN_CHECK(listen(listener.sock, 1) == 0);

    std::atomic<SOCKET> acc{ INVALID_SOCKET };
    std::thread accTh([&]() { acc.store(AcceptLoopbackOne(listener.sock, 2500), std::memory_order_release); });
    LAN_CHECK(LockstepNet_Join(strippedHost, 2500, &cancel));
    accTh.join();
    ScopedSocket accGuard(acc.load(std::memory_order_acquire));
    LAN_CHECK(accGuard.sock != INVALID_SOCKET && LockstepNet_IsConnected());
    LockstepNet_Close();
    return true;
}

static bool T5_07_LockstepNetMalformedWireAndTruncatedGrant() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;

    const uint16_t lsPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(lsPort != 0);
    LockstepNet_SetPortForTest(lsPort);

    ScopedSocket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    LAN_CHECK(listener.sock != INVALID_SOCKET);
    BOOL reuse = TRUE;
    (void)setsockopt(listener.sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(lsPort);
    LAN_CHECK(bind(listener.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    LAN_CHECK(listen(listener.sock, 1) == 0);

    std::atomic<SOCKET> acc{ INVALID_SOCKET };
    std::thread accTh([&]() { acc.store(AcceptLoopbackOne(listener.sock, 2500), std::memory_order_release); });
    volatile bool cancel = false;
    LAN_CHECK(LockstepNet_Join("127.0.0.1", 2500, &cancel));
    accTh.join();
    ScopedSocket srvGuard(acc.load(std::memory_order_acquire));
    LAN_CHECK(srvGuard.sock != INVALID_SOCKET);

    // 1. Truncated LsGrant (size = 2) -> Poll ignores without enqueueing corrupt grant
    const TestLsHeader truncHdr = { 1u, 2u };
    const uint16_t truncBody = 0u;
    LAN_CHECK(NetworkSendExact(srvGuard.sock, &truncHdr, sizeof(truncHdr)));
    LAN_CHECK(NetworkSendExact(srvGuard.sock, &truncBody, sizeof(truncBody)));
    Sleep(40);
    LockstepNet_Poll();
    int spd = 0;
    LAN_CHECK(!LockstepNet_TakeGrant(0ULL, &spd));
    LAN_CHECK(!LockstepNet_TakeGrant(1ULL, &spd));
    LAN_CHECK(LockstepNet_IsConnected());

    // 2. Oversized LsHeader (size = LS_MAX_PAYLOAD + 1u) -> RecvThread breaks cleanly
    const TestLsHeader badHdr = { 11u, LS_MAX_PAYLOAD + 1u };
    LAN_CHECK(NetworkSendExact(srvGuard.sock, &badHdr, sizeof(badHdr)));
    for (int i = 0; i < 50 && LockstepNet_IsConnected(); ++i) Sleep(20);
    LAN_CHECK(!LockstepNet_IsConnected());
    LockstepNet_Close();
    return true;
}

#pragma pack(push, 1)
struct TestWireModManifestHeader {
    char     magic[8];
    uint32_t packMode;
    uint32_t modCount;
    uint32_t pluginCount;
    char     enabledPluginsCsv[256];
};
struct TestWireModEntry {
    uint64_t id;
    uint8_t  isWip;
    uint8_t  itemType;
    char     title[64];
};
struct TestWirePluginIniHeader {
    char     id[64];
    uint32_t byteSize;
};
struct TestWireModSyncResponse {
    char     magic[8];
    uint32_t status;
    uint32_t missingCount;
    char     details[64];
};
#pragma pack(pop)

static bool T5_08_ModSyncTruncatedEntryAndOversizedPluginIni() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedModPackMode packGuard(MOD_PACK_HOST_ONLY);
    ScopedModSyncGuard modGuard;

    // 1. Truncated WireModEntry (modCount=2, send only 1 entry and close)
    {
        SOCKET srv = INVALID_SOCKET, cli = INVALID_SOCKET;
        LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
        ScopedSocket srvGuard(srv);
        ScopedSocket cliGuard(cli);

        std::thread txTh([&]() {
            TestWireModManifestHeader hdr = {};
            std::memcpy(hdr.magic, "RPMPMOD3", 8);
            hdr.packMode = 1u;
            hdr.modCount = 2u;
            TestWireModEntry entry = {};
            entry.id = 990011ULL;
            std::strncpy(entry.title, "HalfSentMod", sizeof(entry.title) - 1);
            (void)NetworkSendExact(srvGuard.sock, &hdr, sizeof(hdr));
            (void)NetworkSendExact(srvGuard.sock, &entry, sizeof(entry));
            srvGuard.Reset();
        });
        char errBuf[256] = {};
        const ModSyncClientResult res = ModSync_ClientHandshake(cliGuard.sock, errBuf, sizeof(errBuf));
        txTh.join();
        LAN_CHECK(res == MOD_SYNC_CLIENT_ERROR);
    }

    // 2. Oversized WirePluginIniHeader::byteSize > 256KB + cleanup of .mp_session_ini
    {
        SOCKET srv = INVALID_SOCKET, cli = INVALID_SOCKET;
        LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
        ScopedSocket srvGuard(srv);
        ScopedSocket cliGuard(cli);

        std::thread txTh([&]() {
            TestWireModManifestHeader hdr = {};
            std::memcpy(hdr.magic, "RPMPMOD3", 8);
            hdr.packMode = 1u;
            hdr.modCount = 0u;
            hdr.pluginCount = 1u;
            std::strncpy(hdr.enabledPluginsCsv, "bad_plugin", sizeof(hdr.enabledPluginsCsv) - 1);
            TestWirePluginIniHeader ph = {};
            std::strncpy(ph.id, "bad_plugin", sizeof(ph.id) - 1);
            ph.byteSize = 256u * 1024u + 1u;
            (void)NetworkSendExact(srvGuard.sock, &hdr, sizeof(hdr));
            (void)NetworkSendExact(srvGuard.sock, &ph, sizeof(ph));
            srvGuard.Reset();
        });
        char errBuf[256] = {};
        const ModSyncClientResult res = ModSync_ClientHandshake(cliGuard.sock, errBuf, sizeof(errBuf));
        txTh.join();
        LAN_CHECK(res == MOD_SYNC_CLIENT_ERROR);
        if (g_baseDir != nullptr && g_baseDir[0] != '\0') {
            std::error_code ec;
            const std::filesystem::path sessionIniDir = std::filesystem::path(g_baseDir) / "plugins" / ".mp_session_ini";
            LAN_CHECK(!std::filesystem::exists(sessionIniDir, ec));
        }
    }
    return true;
}

static bool RunModSyncHostWithRawResponse(const char* respMagic, uint32_t status, uint32_t missingCount,
                                          ModSyncHostResult expectedRes) {
    ScopedTempDir tmp("t509");
    SOCKET srv = INVALID_SOCKET, cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);

    std::thread cliTh([&]() {
        TestWireModManifestHeader hdr = {};
        if (!NetworkRecvExact(cliGuard.sock, &hdr, sizeof(hdr))) return;
        for (uint32_t i = 0; i < hdr.modCount; ++i) {
            TestWireModEntry e = {};
            if (!NetworkRecvExact(cliGuard.sock, &e, sizeof(e))) return;
        }
        for (uint32_t i = 0; i < hdr.pluginCount; ++i) {
            TestWirePluginIniHeader ph = {};
            if (!NetworkRecvExact(cliGuard.sock, &ph, sizeof(ph))) return;
            if (ph.byteSize > 0 && ph.byteSize <= 256u * 1024u) {
                std::vector<uint8_t> body(ph.byteSize);
                if (!NetworkRecvExact(cliGuard.sock, body.data(), body.size())) return;
            }
        }
        TestWireModSyncResponse resp = {};
        std::memcpy(resp.magic, respMagic, 8);
        resp.status = status;
        resp.missingCount = missingCount;
        std::strncpy(resp.details, "wire_test", sizeof(resp.details) - 1);
        (void)NetworkSendExact(cliGuard.sock, &resp, sizeof(resp));
    });

    char errBuf[256] = {};
    const ModSyncHostResult res = ModSync_HostHandshake(srvGuard.sock, tmp.dir.string().c_str(), errBuf, sizeof(errBuf));
    cliTh.join();
    LAN_CHECK(res == expectedRes);
    return true;
}

static bool T5_09_ModSyncHostHandshakeErrorResponses() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedModPackMode packGuard(MOD_PACK_SAVE_ONLY);
    ScopedModSyncGuard modGuard;

    LAN_CHECK(RunModSyncHostWithRawResponse("BADRESP1", 0u, 0u, MOD_SYNC_HOST_ERROR));
    LAN_CHECK(RunModSyncHostWithRawResponse("RPMPRESP", 1u, 2u, MOD_SYNC_HOST_CLIENT_MISSING));
    LAN_CHECK(RunModSyncHostWithRawResponse("RPMPRESP", 2u, 0u, MOD_SYNC_HOST_ERROR));
    return true;
}

static LanHostEntry MakePlanHost(uint64_t guid, const char* ip, uint16_t port, bool samePc,
                                 bool protoMatch, uint64_t seenMs, const char* name) {
    LanHostEntry h = {};
    h.serverGuid = guid;
    h.protocol = protoMatch ? REPUBLICMP_PROTOCOL : static_cast<uint16_t>(REPUBLICMP_PROTOCOL - 1);
    h.tcpPort = port;
    h.sameMachine = samePc;
    h.protocolMatch = protoMatch;
    h.lastSeenMs = seenMs;
    std::strncpy(h.connectIp, ip, sizeof(h.connectIp) - 1);
    std::strncpy(h.hostName, name, sizeof(h.hostName) - 1);
    std::strncpy(h.version, REPUBLICMP_VERSION, sizeof(h.version) - 1);
    return h;
}

static bool T5_10_NativeMenuPlanJoinAndDuplicateSplitEdges() {
    // 1. IsDuplicateRemoteHost split branches + SelectSamePcHostIndex secondary tie-breakers + Steam friends
    const std::vector<LanHostEntry> hosts = {
        // Same-PC candidates: all protocolMatch=true; 127.0.0.1 with higher lastSeenMs must win over LAN IP & older loopback
        MakePlanHost(10ULL, "192.168.1.20", 5556u, true, true, 9000ULL, "SamePcLan"),
        MakePlanHost(11ULL, "127.0.0.1",    5556u, true, true, 3000ULL, "SamePcOld"),
        MakePlanHost(12ULL, "127.0.0.1",    5556u, true, true, 5000ULL, "SamePcNew"),
        MakePlanHost(13ULL, "127.0.0.1",    5556u, true, true, 5000ULL, "SamePcTie"),
        // Remote duplicates split branches:
        MakePlanHost(20ULL, "192.168.1.30", 5556u, false, true, 1000ULL, "MultiNicA"),
        MakePlanHost(20ULL, "10.0.0.30",    5556u, false, true, 1000ULL, "MultiNicB"), // same guid -> collapses
        MakePlanHost(21ULL, "192.168.1.31", 5556u, false, true, 1000ULL, "SameIpPortA"),
        MakePlanHost(22ULL, "192.168.1.31", 5556u, false, true, 1000ULL, "SameIpPortB"), // diff guid, same ip:port -> collapses
        MakePlanHost(23ULL, "192.168.1.31", 5557u, false, true, 1000ULL, "SameIpDiffPort"), // diff port -> kept
        MakePlanHost(0ULL,  "192.168.1.32", 5556u, false, true, 1000ULL, "ZeroGuid1"),
        MakePlanHost(0ULL,  "192.168.1.33", 5556u, false, true, 1000ULL, "ZeroGuid2"), // guid=0, diff ip -> kept
    };

    NativeMenuFriendInfo fValid = { 76561198000000101ULL, "ValidFriend", true, true };
    NativeMenuFriendInfo fNotHosting = { 76561198000000102ULL, "IdleFriend", true, false };
    NativeMenuFriendInfo fZeroId = { 0ULL, "ZeroFriend", true, true };
    const std::vector<NativeMenuFriendInfo> friends = { fValid, fNotHosting, fZeroId };

    // Russian + STEAM: uppercase prefix -> "Войти по SteamID"
    const auto planRu = NativeMenu_PlanJoinEntries(false, hosts, true, friends, "STEAM:76561198000000101", false,
                                                   NATIVE_JOIN_MAX_SLOTS);
    int samePcCount = 0;
    uint64_t chosenSamePcGuid = 0;
    int remoteCount = 0;
    int steamFriendCount = 0;
    bool sawRuSteamTail = false;
    for (const auto& e : planRu) {
        if (e.kind == JOIN_ACT_LAN_HOST && e.lanHost.sameMachine) {
            ++samePcCount;
            chosenSamePcGuid = e.lanHost.serverGuid;
        } else if (e.kind == JOIN_ACT_LAN_HOST) {
            ++remoteCount;
        } else if (e.kind == JOIN_ACT_STEAM_FRIEND) {
            ++steamFriendCount;
            LAN_CHECK(e.steamId == fValid.steamId);
        } else if (e.kind == JOIN_ACT_CONNECT_ADDR && e.label == L"Войти по SteamID") {
            sawRuSteamTail = true;
        }
    }
    LAN_CHECK(samePcCount == 1 && chosenSamePcGuid == 12ULL);
    LAN_CHECK(remoteCount == 5); // MultiNicA, SameIpPortA, SameIpDiffPort, ZeroGuid1, ZeroGuid2
    LAN_CHECK(steamFriendCount == 1);
    LAN_CHECK(sawRuSteamTail);

    // 2. NativeMenu_FormatConnectTarget edge cases
    char tgt[64] = {};
    const LanHostEntry hPort0 = MakePlanHost(1ULL, "192.168.1.50", 0u, true, true, 100ULL, "H0");
    LAN_CHECK(NativeMenu_FormatConnectTarget(hPort0, tgt, sizeof(tgt)) && std::strcmp(tgt, "127.0.0.1") == 0);
    const LanHostEntry hPort5577 = MakePlanHost(2ULL, "192.168.1.50", 5577u, true, true, 100ULL, "H5577");
    LAN_CHECK(NativeMenu_FormatConnectTarget(hPort5577, tgt, sizeof(tgt)) && std::strcmp(tgt, "127.0.0.1:5577") == 0);
    const LanHostEntry hEmptyIp = MakePlanHost(3ULL, "", 5556u, false, true, 100ULL, "HEmpty");
    LAN_CHECK(!NativeMenu_FormatConnectTarget(hEmptyIp, tgt, sizeof(tgt)));
    LAN_CHECK(!NativeMenu_FormatConnectTarget(hPort0, tgt, 0));
    return true;
}

static bool T5_11_UiCommonAdaptersClipboardAndSyncBlockStates() {
    // 1. VirtualLocal-only fallback returns best VirtualLocal adapter, not nullptr
    LanAdapterInfo vl1 = {};
    std::strncpy(vl1.ip, "172.22.0.1", sizeof(vl1.ip) - 1);
    vl1.kind = LanAdapterKind::VirtualLocal;
    vl1.priority = 50;
    vl1.ifIndex = 12;
    LanAdapterInfo vl2 = vl1;
    std::strncpy(vl2.ip, "172.23.0.1", sizeof(vl2.ip) - 1);
    vl2.ifIndex = 5;
    const std::vector<LanAdapterInfo> vlOnly = { vl1, vl2 };
    const LanAdapterInfo* picked = UiCommon_SelectPrimaryAdapter(vlOnly);
    LAN_CHECK(picked != nullptr && std::strcmp(picked->ip, "172.23.0.1") == 0);

    // 2. UiCommon_NormalizeClipboardJoinAddress & leading-zero octet rejection
    char norm[64] = {};
    LAN_CHECK(UiCommon_NormalizeClipboardJoinAddress("STEAM:76561198012345678", norm, sizeof(norm)));
    LAN_CHECK(std::strcmp(norm, "steam:76561198012345678") == 0);
    LAN_CHECK(UiCommon_NormalizeClipboardJoinAddress("192.168.1.50:05556", norm, sizeof(norm)));
    LAN_CHECK(std::strcmp(norm, "192.168.1.50:5556") == 0);
    LAN_CHECK(!UiCommon_NormalizeClipboardJoinAddress("76561198012345678:5556", norm, sizeof(norm)));
    LAN_CHECK(!NetworkNormalizeTargetAddress("192.168.001.050", norm, sizeof(norm), nullptr));

    // 3. SyncBlock_Draw across reconnect (MapInspect=false), HOST_JOINING (SetPercentEx), CLIENT_JOINING (indeterminate)
    ScopedWorldLoadedFlag worldGuard;
    auto renderSyncState = [](SessionState st, bool hadPartner, bool active, int pct,
                              int64_t queued, int64_t total, float speed) -> bool {
        SaveSync_SetTestSessionState(st, hadPartner, hadPartner ? "192.168.1.99" : "");
        SyncBlock_SetMapInspectMode(false);
        SyncBlock_SetActive(active, active ? "Transferring" : nullptr);
        SyncBlock_SetStep(3, 7, "Transferring", pct);
        if (total > 0) {
            SyncBlock_SetPercentEx(pct, queued, total, speed);
        }
        ScopedImGuiFrame frame;
        LAN_CHECK(frame.inFrame);
        SyncBlock_Draw();
        LAN_CHECK(frame.NextFrame());
        SyncBlock_Draw();
        LAN_CHECK(frame.RenderAndCountVertices() > 0);
        return true;
    };
    LAN_CHECK(renderSyncState(SESSION_HOST_LISTENING, true, false, -1, 0, 0, 0.0f));
    LAN_CHECK(renderSyncState(SESSION_HOST_JOINING, false, true, 45, 512000, 1024000, 1.5f));
    LAN_CHECK(renderSyncState(SESSION_CLIENT_JOINING, false, true, -1, 0, 0, 0.0f));
    SyncBlock_SetActive(false, nullptr);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    return true;
}

static bool T5_12_LanDiscoveryAutoNameAndSortPriority() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;

    // 1. nullptr hostName/mapName fallback uses LanDiscovery_GetLocalDisplayName() and empty mapName
    const uint16_t udpPort1 = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(udpPort1 != 0);
    LanDiscovery_SetPortForTest(udpPort1);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 0x5120u, true);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, nullptr, nullptr);
    LanDiscovery_StartScan();

    const std::string expectedName = LanDiscovery_GetLocalDisplayName();
    LAN_CHECK(!expectedName.empty());
    LanHostEntry autoHost = {};
    LAN_CHECK(PollForHostByName(expectedName.c_str(), 3000, &autoHost, true));
    LAN_CHECK(autoHost.mapName[0] == '\0');

    LanDiscovery_StopScan();
    LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
    LanDiscovery_Shutdown();

    // 2. LanDiscovery_GetHosts() sort priority over real loopback UDP responder replying to scanner probe
    const uint16_t udpPort2 = AllocateEphemeralPort(SOCK_DGRAM, IPPROTO_UDP);
    LAN_CHECK(udpPort2 != 0);
    ScopedSocket mockResp(OpenBoundUdpLoopback(udpPort2, 100));
    LAN_CHECK(mockResp.sock != INVALID_SOCKET);

    const uint32_t remotePid = GetCurrentProcessId() ^ 0x5121u;
    const LanAnnouncePacket pMismatch = MakeAnnouncePacket(0xA01ULL, remotePid, 5556u, 1u,
                                                           "ProtoMismatch", "M",
                                                           static_cast<uint16_t>(REPUBLICMP_PROTOCOL - 1));
    const LanAnnouncePacket pOlder = MakeAnnouncePacket(0xA02ULL, remotePid, 5557u, 1u,
                                                        "OlderMatch", "M", REPUBLICMP_PROTOCOL);
    const LanAnnouncePacket pNewer = MakeAnnouncePacket(0xA03ULL, remotePid, 5558u, 1u,
                                                        "NewerMatch", "M", REPUBLICMP_PROTOCOL);

    std::atomic<bool> stopResp{ false };
    std::thread respTh([&]() {
        bool sentOlder = false;
        bool sentRest = false;
        while (!stopResp.load(std::memory_order_acquire)) {
            uint8_t buf[256] = {};
            sockaddr_in from = {};
            int fromLen = static_cast<int>(sizeof(from));
            const int rx = recvfrom(mockResp.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&from), &fromLen);
            LanProbePacket pr = {};
            if (rx > 0 && LanDiscovery_ParseProbe(buf, static_cast<size_t>(rx), &pr)) {
                auto sendAnn = [&](const LanAnnouncePacket& a) {
                    uint8_t wire[sizeof(LanAnnouncePacket)] = {};
                    const size_t n = LanDiscovery_SerializeAnnounce(a, wire, sizeof(wire));
                    if (n > 0) {
                        sendto(mockResp.sock, reinterpret_cast<const char*>(wire), static_cast<int>(n), 0,
                               reinterpret_cast<const sockaddr*>(&from), fromLen);
                    }
                };
                if (!sentOlder) {
                    sendAnn(pOlder);
                    sentOlder = true;
                } else if (!sentRest) {
                    Sleep(40);
                    sendAnn(pMismatch);
                    sendAnn(pNewer);
                    sentRest = true;
                }
            }
        }
    });

    LanDiscovery_SetPortForTest(udpPort2);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId(), true);
    LanDiscovery_Init();
    LanDiscovery_StartScan();
    std::vector<LanHostEntry> sortedHosts;
    const ULONGLONG deadline = GetTickCount64() + 4000ULL;
    while (GetTickCount64() < deadline) {
        LanDiscovery_ProbeNow();
        Sleep(40);
        sortedHosts = LanDiscovery_GetHosts();
        if (sortedHosts.size() >= 3u) break;
    }
    stopResp.store(true, std::memory_order_release);
    mockResp.Reset();
    respTh.join();

    LAN_CHECK(sortedHosts.size() >= 3u);
    LAN_CHECK(sortedHosts[0].serverGuid == 0xA03ULL); // newerMatch first
    LAN_CHECK(sortedHosts[1].serverGuid == 0xA02ULL); // olderMatch second
    LAN_CHECK(sortedHosts[2].serverGuid == 0xA01ULL); // protocolMismatch last
    return true;
}

static bool VerifyPeerReceivesGracefulByeOnClose(SOCKET peerSock, bool* outSawClientLogs) {
    if (outSawClientLogs != nullptr) *outSawClientLogs = false;
    if (peerSock == INVALID_SOCKET) return false;
    DWORD rcvTimeoutMs = 2000;
    (void)setsockopt(peerSock, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&rcvTimeoutMs), sizeof(rcvTimeoutMs));

    bool sawBye = false;
    bool sawLogs = false;
    int rxErr = 0;
    int eofRx = -1;
    std::atomic<bool> readerReady{ false };
    std::thread readerTh([&]() {
        readerReady.store(true, std::memory_order_release);
        for (int msgIdx = 0; msgIdx < 4; ++msgIdx) {
            TestLsHeader hdr = {};
            const int rx = recv(peerSock, reinterpret_cast<char*>(&hdr),
                                static_cast<int>(sizeof(hdr)), MSG_WAITALL);
            if (rx != static_cast<int>(sizeof(hdr))) {
                if (rx < 0) rxErr = WSAGetLastError();
                break;
            }
            if (hdr.size > 0u) {
                if (hdr.size > LS_MAX_PAYLOAD) {
                    rxErr = WSAEMSGSIZE;
                    break;
                }
                std::vector<char> body(hdr.size);
                const int bx = recv(peerSock, body.data(), static_cast<int>(hdr.size), MSG_WAITALL);
                if (bx != static_cast<int>(hdr.size)) {
                    if (bx < 0) rxErr = WSAGetLastError();
                    break;
                }
            }
            if (hdr.type == 14u /* LS_CLIENT_LOGS */) {
                sawLogs = true;
                continue;
            }
            if (hdr.type == 10u /* LS_BYE */ && hdr.size == 0u) {
                sawBye = true;
                char extra = 0;
                eofRx = recv(peerSock, &extra, 1, 0);
                if (eofRx < 0) rxErr = WSAGetLastError();
                break;
            }
            break;
        }
    });
    while (!readerReady.load(std::memory_order_acquire)) Sleep(1);
    Sleep(10); // let reader block inside recv() before LockstepNet_Close()

    const ULONGLONG t0 = GetTickCount64();
    LockstepNet_Close();
    const ULONGLONG closeMs = GetTickCount64() - t0;
    readerTh.join();

    if (outSawClientLogs != nullptr) *outSawClientLogs = sawLogs;
    LAN_CHECK(!LockstepNet_IsConnected());
    LAN_CHECK(closeMs < 1000ULL);
    LAN_CHECK(rxErr == 0);
    LAN_CHECK(sawBye);
    LAN_CHECK(eofRx == 0);
    return true;
}

static bool T5_13_LockstepNetCloseGracefulByeNoReset() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);
    ScopedDiscoveryGuard guard;

    const uint16_t lsPort = AllocateEphemeralPort(SOCK_STREAM, IPPROTO_TCP);
    LAN_CHECK(lsPort != 0);
    LockstepNet_SetPortForTest(lsPort);

    // 1. Client-side LockstepNet_Join() -> LockstepNet_Close() across 10 trials with peer actively blocked in recv()
    {
        ScopedSocket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        LAN_CHECK(listener.sock != INVALID_SOCKET);
        BOOL reuse = TRUE;
        (void)setsockopt(listener.sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(lsPort);
        LAN_CHECK(bind(listener.sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
        LAN_CHECK(listen(listener.sock, 4) == 0);

        for (int trial = 0; trial < 10; ++trial) {
            std::atomic<SOCKET> acc{ INVALID_SOCKET };
            std::thread accTh([&]() { acc.store(AcceptLoopbackOne(listener.sock, 2500), std::memory_order_release); });
            volatile bool cancel = false;
            const bool joined = LockstepNet_Join("127.0.0.1", 2500, &cancel);
            accTh.join();
            ScopedSocket srvGuard(acc.load(std::memory_order_acquire));
            LAN_CHECK(joined && srvGuard.sock != INVALID_SOCKET);
            LAN_CHECK(VerifyPeerReceivesGracefulByeOnClose(srvGuard.sock));
        }
    }

    // 2. Host-side LockstepNet_Host() -> LockstepNet_Close() across 5 trials with client peer actively blocked in recv()
    for (int trial = 0; trial < 5; ++trial) {
        volatile bool cancelHost = false;
        bool hostOk = false;
        std::thread hostTh([&]() { hostOk = LockstepNet_Host(3000, &cancelHost); });

        SOCKET rawCli = INVALID_SOCKET;
        for (int i = 0; i < 60 && rawCli == INVALID_SOCKET; ++i) {
            Sleep(15);
            rawCli = NetworkConnect("127.0.0.1", lsPort, 300, 2000, nullptr);
        }
        if (rawCli == INVALID_SOCKET) cancelHost = true;
        hostTh.join();
        ScopedSocket cliGuard(rawCli);
        LAN_CHECK(hostOk && cliGuard.sock != INVALID_SOCKET);
        LAN_CHECK(VerifyPeerReceivesGracefulByeOnClose(cliGuard.sock));
    }

    // Idempotent second close
    LockstepNet_Close();
    LAN_CHECK(!LockstepNet_IsConnected());
    return true;
}

static bool T5_14_SaveSyncDllCrcConcurrentHello() {
    ScopedWsa wsa;
    LAN_CHECK(wsa.ok);

    // 1. Concurrent SaveSync_GetLocalDllCrc() across 8 threads
    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };
    unsigned int crcs[8] = {};
    std::vector<std::thread> crcThreads;
    for (int t = 0; t < 8; ++t) {
        crcThreads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
                YieldProcessor();
            }
            crcs[t] = SaveSync_GetLocalDllCrc();
        });
    }
    while (ready.load(std::memory_order_acquire) < 8) Sleep(0);
    go.store(true, std::memory_order_release);
    for (auto& th : crcThreads) th.join();
    LAN_CHECK(crcs[0] != 0u);
    for (int t = 1; t < 8; ++t) {
        LAN_CHECK(crcs[t] == crcs[0]);
    }

    // 2. Concurrent Hello exchange between Host and Client loopback socket pairs
    SOCKET srv = INVALID_SOCKET, cli = INVALID_SOCKET;
    LAN_CHECK(CreateLoopbackTcpPair(&srv, &cli));
    ScopedSocket srvGuard(srv);
    ScopedSocket cliGuard(cli);
    bool hostOk = false;
    char hostWhy[256] = {};
    char cliWhy[256] = {};
    std::thread hostTh([&]() {
        hostOk = SaveSync_ExchangeHelloOverSocketForTest(static_cast<uintptr_t>(srvGuard.sock),
                                                         true, hostWhy, sizeof(hostWhy));
    });
    const bool cliOk = SaveSync_ExchangeHelloOverSocketForTest(static_cast<uintptr_t>(cliGuard.sock),
                                                              false, cliWhy, sizeof(cliWhy));
    hostTh.join();
    LAN_CHECK(hostOk && cliOk);
    return true;
}

struct LanE2eCase {
    int tier;
    const char* id;
    bool (*fn)();
};

static const LanE2eCase kLanE2eCases[] = {
    // Tier 1 (55)
    { 1, "T1-F1-01", &T1_F1_01_ClassifyEthernetAndBroadcast },
    { 1, "T1-F1-02", &T1_F1_02_ClassifyWifiAndSlash16Broadcast },
    { 1, "T1-F1-03", &T1_F1_03_ClassifyRadminVpn },
    { 1, "T1-F1-04", &T1_F1_04_ClassifyHamachiAndZeroTier },
    { 1, "T1-F1-05", &T1_F1_05_GetLocalAdaptersConsistency },
    { 1, "T1-F2-01", &T1_F2_01_ProbeSerializeParseRoundTrip },
    { 1, "T1-F2-02", &T1_F2_02_AnnounceSerializeParseRoundTrip },
    { 1, "T1-F2-03", &T1_F2_03_UpsertHostInsertsEntry },
    { 1, "T1-F2-04", &T1_F2_04_PruneExpiredRemovesOnlyOld },
    { 1, "T1-F2-05", &T1_F2_05_ByeAnnounceRemovesImmediately },
    { 1, "T1-F3-01", &T1_F3_01_HostRepliesUnicastToProbe },
    { 1, "T1-F3-02", &T1_F3_02_HostSendsPeriodicBeacon },
    { 1, "T1-F3-03", &T1_F3_03_SetHostingUpdatesMetadata },
    { 1, "T1-F3-04", &T1_F3_04_SetHostingFalseStopsOpenReplies },
    { 1, "T1-F3-05", &T1_F3_05_ShutdownAndReinitResumes },
    { 1, "T1-F4-01", &T1_F4_01_ScannerDiscoversHostViaAuthoritativeIp },
    { 1, "T1-F4-02", &T1_F4_02_ScannerPreservesMetadataAndPort },
    { 1, "T1-F4-03", &T1_F4_03_HostAndScannerCoexistSameProcess },
    { 1, "T1-F4-04", &T1_F4_04_StopScanIdempotentRestart },
    { 1, "T1-F4-05", &T1_F4_05_ProtocolMatchFlagTrueForCurrentProtocol },
    { 1, "T1-F5-01", &T1_F5_01_NormalizePlainIpv4 },
    { 1, "T1-F5-02", &T1_F5_02_NormalizeIpv4WithPort },
    { 1, "T1-F5-03", &T1_F5_03_NormalizeLocalhostVariants },
    { 1, "T1-F5-04", &T1_F5_04_NormalizeSteamTargets },
    { 1, "T1-F5-05", &T1_F5_05_NormalizeTrimAndLoopbackConnect },
    { 1, "T1-F6-01", &T1_F6_01_SteamAbsentReturnsFalseCleanly },
    { 1, "T1-F6-02", &T1_F6_02_SteamOfflineNoOpsSafe },
    { 1, "T1-F6-03", &T1_F6_03_DualTestbedConsistentInNormalEnv },
    { 1, "T1-F6-04", &T1_F6_04_DualTestbedAndSteamParity },
    { 1, "T1-F6-05", &T1_F6_05_LoggedOnAndAvailableInvariant },
    { 1, "T1-F7-01", &T1_F7_01_FindLocalModInWorkshopWip },
    { 1, "T1-F7-02", &T1_F7_02_FindLocalModInSteamWorkshop784150 },
    { 1, "T1-F7-03", &T1_F7_03_FindLocalModInMediaSovietWorkshop },
    { 1, "T1-F7-04", &T1_F7_04_IsSteamUgcOnlineNonBlocking },
    { 1, "T1-F7-05", &T1_F7_05_LoopbackModSyncEmptyManifestOffline },
    { 1, "T1-F8-01", &T1_F8_01_LocalDisplayNameFallbackNonEmpty },
    { 1, "T1-F8-02", &T1_F8_02_HostSessionStartAndDisconnect },
    { 1, "T1-F8-03", &T1_F8_03_ClientSessionCancelTransitionsToIdle },
    { 1, "T1-F8-04", &T1_F8_04_LocalIdentityVersionAndTargetIp },
    { 1, "T1-F8-05", &T1_F8_05_TestSessionStateBusyAndAwaitingPredicates },
    { 1, "T1-F9-01", &T1_F9_01_CopyTextToClipboardRoundTrip },
    { 1, "T1-F9-02", &T1_F9_02_ToastRendersInSessionIdle },
    { 1, "T1-F9-03", &T1_F9_03_ErrorToastSurvivesOnSessionEnded },
    { 1, "T1-F9-04", &T1_F9_04_AllToastColorKindsRenderSafely },
    { 1, "T1-F9-05", &T1_F9_05_ConsecutiveClipboardWritesOverwriteCleanly },
    { 1, "T1-F10-01", &T1_F10_01_PrimaryHostIpCopyAndPort },
    { 1, "T1-F10-02", &T1_F10_02_MultiAdapterLanAndVpnRanking },
    { 1, "T1-F10-03", &T1_F10_03_CopyHostIpWithoutSteamId },
    { 1, "T1-F10-04", &T1_F10_04_MapInspectModeAllowsCameraUnblocked },
    { 1, "T1-F10-05", &T1_F10_05_CoexistenceWithSteamIdentity },
    { 1, "T1-F11-01", &T1_F11_01_DiscoveredLanHostConnectFlow },
    { 1, "T1-F11-02", &T1_F11_02_JoinMenuSafeWhenSteamOfflineAndNoHosts },
    { 1, "T1-F11-03", &T1_F11_03_SamePcHostDeduplicationFlag },
    { 1, "T1-F11-04", &T1_F11_04_ProtocolMismatchMarkedInHostEntry },
    { 1, "T1-F11-05", &T1_F11_05_RefreshHostsOfflineProbeAndToast },

    // Tier 2 (55)
    { 2, "T2-F1-01", &T2_F1_01_LoopbackApipaZeroRejected },
    { 2, "T2-F1-02", &T2_F1_02_PrefixLengthsEdgeAndInvalid },
    { 2, "T2-F1-03", &T2_F1_03_MalformedIpsAndNullBuffers },
    { 2, "T2-F1-04", &T2_F1_04_VpnOutranksVirtualLocal },
    { 2, "T2-F1-05", &T2_F1_05_TunnelWithEmptyNameClassifiedByIp },
    { 2, "T2-F2-01", &T2_F2_01_TruncatedAndOversizedPacketsRejected },
    { 2, "T2-F2-02", &T2_F2_02_BadMagicAndZeroPortRejected },
    { 2, "T2-F2-03", &T2_F2_03_UnterminatedStringsBoundedSafely },
    { 2, "T2-F2-04", &T2_F2_04_ProtocolMismatchValues },
    { 2, "T2-F2-05", &T2_F2_05_ClockWraparoundAndExactTtlBoundary },
    { 2, "T2-F3-01", &T2_F3_01_ClosedHostIgnoresProbes },
    { 2, "T2-F3-02", &T2_F3_02_GarbageFloodResilience },
    { 2, "T2-F3-03", &T2_F3_03_LongUtf8HostNameAndNullMapName },
    { 2, "T2-F3-04", &T2_F3_04_ExclusivePortConflictDegradesGracefully },
    { 2, "T2-F3-05", &T2_F3_05_RapidHostingToggleDeterminism },
    { 2, "T2-F4-01", &T2_F4_01_DedupOrderLoopbackOverLanOverVpn },
    { 2, "T2-F4-02", &T2_F4_02_DedupOrderLanOverVpn },
    { 2, "T2-F4-03", &T2_F4_03_ScannerIgnoresSelfPid },
    { 2, "T2-F4-04", &T2_F4_04_AuthoritativeRecvfromIpOverridesPayload },
    { 2, "T2-F4-05", &T2_F4_05_SixtyFourHostsUpsertAndExpire },
    { 2, "T2-F5-01", &T2_F5_01_NullEmptyWhitespaceRejected },
    { 2, "T2-F5-02", &T2_F5_02_InvalidPortsRejected },
    { 2, "T2-F5-03", &T2_F5_03_MalformedIpv4Rejected },
    { 2, "T2-F5-04", &T2_F5_04_TrimmedPortAndSteamIdAccepted },
    { 2, "T2-F5-05", &T2_F5_05_SmallBufferCapacityRejected },
    { 2, "T2-F6-01", &T2_F6_01_OfflineModeNotAvailable },
    { 2, "T2-F6-02", &T2_F6_02_SteamInitLoopIdempotentFast },
    { 2, "T2-F6-03", &T2_F6_03_DualTestbedEnvConsistency },
    { 2, "T2-F6-04", &T2_F6_04_SteamBoundaryNullAndInvalidIds },
    { 2, "T2-F6-05", &T2_F6_05_InvalidSocketChecksSafe },
    { 2, "T2-F7-01", &T2_F7_01_FindLocalModInvalidArgs },
    { 2, "T2-F7-02", &T2_F7_02_ModDirWithoutConfigIniIgnored },
    { 2, "T2-F7-03", &T2_F7_03_MissingModOfflineDoesNotHangDownloadModal },
    { 2, "T2-F7-04", &T2_F7_04_BeforeWorldLoadOfflineMissingModNoStuckModal },
    { 2, "T2-F7-05", &T2_F7_05_CorruptedModManifestHeaderRejected },
    { 2, "T2-F8-01", &T2_F8_01_ClientCancelReturnsToIdleQuickly },
    { 2, "T2-F8-02", &T2_F8_02_StartHostRefusedWhenNoWorldLoaded },
    { 2, "T2-F8-03", &T2_F8_03_VersionAndProtocolMismatchCheck },
    { 2, "T2-F8-04", &T2_F8_04_StartClientIgnoredWhileBusy },
    { 2, "T2-F8-05", &T2_F8_05_DisplayNameBoundsValid },
    { 2, "T2-F9-01", &T2_F9_01_ClipboardRejectsNullAndEmpty },
    { 2, "T2-F9-02", &T2_F9_02_ClipboardHandlesLargeAndUtf8Text },
    { 2, "T2-F9-03", &T2_F9_03_RapidToastBurstBoundedInIdle },
    { 2, "T2-F9-04", &T2_F9_04_DoubleOnSessionEndedIdempotent },
    { 2, "T2-F9-05", &T2_F9_05_AddToastNullEmptyAndOutOfRangeColor },
    { 2, "T2-F10-01", &T2_F10_01_EmptyAdapterFallbackNeverCopiesEmpty },
    { 2, "T2-F10-02", &T2_F10_02_SixAdaptersPrioritizedAboveVirtualLocal },
    { 2, "T2-F10-03", &T2_F10_03_MapInspectExitsOnHostJoining },
    { 2, "T2-F10-04", &T2_F10_04_LongAdapterNameAndMaxIpv4Safe },
    { 2, "T2-F10-05", &T2_F10_05_CopyToastSurvivesHostDisconnectToIdle },
    { 2, "T2-F11-01", &T2_F11_01_JoinMenuSlotBudgetPreservesStaticControls },
    { 2, "T2-F11-02", &T2_F11_02_FortySevenCharHostNameNullTerminated },
    { 2, "T2-F11-03", &T2_F11_03_ClipboardPasteValidAndGarbageAddresses },
    { 2, "T2-F11-04", &T2_F11_04_ManualJoinCustomPortPreserved },
    { 2, "T2-F11-05", &T2_F11_05_StoppedHostVanishesFromJoinList },

    // Tier 3 (13)
    { 3, "T3-01", &T3_01_MultiNicClassificationPlusClientDedup },
    { 3, "T3-02", &T3_02_HostResponderPlusClientScannerByeBurst },
    { 3, "T3-03", &T3_03_MultiAdapterPlusClipboardCopyToast },
    { 3, "T3-04", &T3_04_LocalPcWaitingPlusRemoteLanHostCoexist },
    { 3, "T3-05", &T3_05_NormalizedManualLoopbackConnectSteamFree },
    { 3, "T3-06", &T3_06_SteamOfflinePlusDiskWorkshopModFallback },
    { 3, "T3-07", &T3_07_FullLoopbackModSyncHandshakeSteamFree },
    { 3, "T3-08", &T3_08_FailedJoinErrorToastPreservedInIdle },
    { 3, "T3-09", &T3_09_IncompatibleProtocolWarningFlaggedInBrowser },
    { 3, "T3-10", &T3_10_LocalHostEventPlusLanResponderConcurrent },
    { 3, "T3-11", &T3_11_MapInspectModeAndHostStopTeardown },
    { 3, "T3-12", &T3_12_ManualJoinModalSteamVsIpv4Routing },
    { 3, "T3-13", &T3_13_MultiNicRadminHamachiZeroTierTargetPlanningPure },

    // Tier 4 (8)
    { 4, "T4-01", &T4_01_FullSteamFreeUdpDiscoveryAndTcpHandshake },
    { 4, "T4-02", &T4_02_MultiAdapterVirtualLanAndClipboardWorkflow },
    { 4, "T4-03", &T4_03_HostStopRestartAndSilentTtlRecovery },
    { 4, "T4-04", &T4_04_CorruptedUdpStormWithValidHostDiscovery },
    { 4, "T4-05", &T4_05_OfflineWorkshopContent784150AndWipJoinCycle },
    { 4, "T4-06", &T4_06_BackwardCompatLoopbackAndSteamTarget },
    { 4, "T4-07", &T4_07_JoinFailureIdleToastAndManualRetryRecovery },
    { 4, "T4-08", &T4_08_LoopbackOnlyModeUdpDiscoveryAndTcpAcceptHandshake },

    // Tier 5 (14)
    { 5, "T5-01", &T5_01_SaveSyncCustomHelloRejections },
    { 5, "T5-02", &T5_02_SaveSyncFullHandshakeAndDlcNegotiation },
    { 5, "T5-03", &T5_03_NetworkConnectCancelTimeoutAndInvalidPort },
    { 5, "T5-04", &T5_04_NetworkExactClosedPeerAndZeroByteFile },
    { 5, "T5-05", &T5_05_LockstepNetHostAndJoinLoopbackLifecycle },
    { 5, "T5-06", &T5_06_LockstepNetCancelTimeoutAndIpPortStrip },
    { 5, "T5-07", &T5_07_LockstepNetMalformedWireAndTruncatedGrant },
    { 5, "T5-08", &T5_08_ModSyncTruncatedEntryAndOversizedPluginIni },
    { 5, "T5-09", &T5_09_ModSyncHostHandshakeErrorResponses },
    { 5, "T5-10", &T5_10_NativeMenuPlanJoinAndDuplicateSplitEdges },
    { 5, "T5-11", &T5_11_UiCommonAdaptersClipboardAndSyncBlockStates },
    { 5, "T5-12", &T5_12_LanDiscoveryAutoNameAndSortPriority },
    { 5, "T5-13", &T5_13_LockstepNetCloseGracefulByeNoReset },
    { 5, "T5-14", &T5_14_SaveSyncDllCrcConcurrentHello },
};

} // namespace lan_e2e

int RunLanE2eTestSuite() {
    lan_e2e::ScopedWsa wsaSuiteGuard;
    lan_e2e::ScopedSettingsIniGuard settingsIniGuard;
    lan_e2e::ScopedStandaloneHost standaloneHost;
    lan_e2e::ScopedLoopbackTestMode loopbackTestGuard;
    lan_e2e::g_failedAssertions = 0;

    int tierTotal[6] = {};
    int tierPass[6] = {};
    int totalRun = 0;
    int totalPass = 0;

    for (const auto& tc : lan_e2e::kLanE2eCases) {
        ++totalRun;
        if (tc.tier >= 1 && tc.tier <= 5) {
            ++tierTotal[tc.tier];
        }
        const bool ok = tc.fn();
        if (ok) {
            ++totalPass;
            if (tc.tier >= 1 && tc.tier <= 5) {
                ++tierPass[tc.tier];
            }
            std::printf("  [PASS] %s\n", tc.id);
        } else {
            std::printf("  [FAIL] %s\n", tc.id);
        }
    }

    SyncBlock_SetMapInspectMode(false);
    SyncBlock_SetActive(false, nullptr);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    NativeMenu_ClearHostOnNextLoad();

    std::printf("LAN E2E Summary: Tier1=%d/%d Tier2=%d/%d Tier3=%d/%d Tier4=%d/%d Tier5=%d/%d Total=%d/%d (failedAssertions=%d)\n",
                tierPass[1], tierTotal[1],
                tierPass[2], tierTotal[2],
                tierPass[3], tierTotal[3],
                tierPass[4], tierTotal[4],
                tierPass[5], tierTotal[5],
                totalPass, totalRun, lan_e2e::g_failedAssertions);

    return (totalPass == totalRun && lan_e2e::g_failedAssertions == 0) ? 0 : 1;
}
