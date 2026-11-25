// SPDX-License-Identifier: GPL-3.0-only
// See save_sync.h.
#include "save_sync.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "game_speed.h"
#include "sync_check.h"
#include "two_states.h"
#include "mod_sync.h"
#include "../game/save_load.h"
#include "../save/save_zip.h"
#include "../ui/ui_log.h"
#include "../ui/sync_block.h"
#include "../ui/native_menu.h"
#include "../ui/lang.h"
#include "../third_party/miniz/miniz.h" // mz_crc32 for the build check
#include "../transport/network.h"
#include "../transport/lan_discovery.h"
#include "../transport/steam_p2p.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/buildings/build_mode.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../determinism/determinism.h"
#include "../lockstep/lockstep_net.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <atomic>

#pragma comment(lib, "User32.lib")

static std::atomic<bool> g_inSaveTransfer{false};
static std::atomic<bool> g_testWorldLoadInProgress{false};

struct SaveTransferGuard {
    SaveTransferGuard() { g_inSaveTransfer.store(true, std::memory_order_relaxed); }
    ~SaveTransferGuard() { g_inSaveTransfer.store(false, std::memory_order_relaxed); }
};

// Fixed local name for the downloaded zip and its extraction folder on the client.
#define SAVE_SYNC_CLIENT_STAGING_NAME "mp_syncsave_incoming"

// How long the live sync channels get to connect after the world is loaded.
#define CHANNELS_CONNECT_TIMEOUT_MS 30000
#define CHANNELS_RETRY_MS 1000
// A single dropped channel (network hiccup, Debug window Disconnect) gets
// this long to reconnect before the session is given up.
#define CHANNEL_RECONNECT_TIMEOUT_MS 20000

#define JOIN_STEPS 7

bool SaveSync_InstallHooks();

void SaveSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    SaveSync_InstallHooks();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile SessionState g_state = SESSION_IDLE;
static volatile bool g_isHost = false;
static volatile bool g_hadPartnerInSession = false;
static INIT_ONCE g_saveSyncStrLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_saveSyncStrLock;
static char g_targetIp[64] = SAVE_SYNC_TEST_HOST_IP; // client target - overridable from the UI
static char g_peerIp[64] = "";
static char g_lastPartnerPeer[64] = "";
static char g_lastError[256] = "";
static ClientHostLostInfo g_clientHostLost = {};
static bool g_hostLostReconnectPending = false;
static ClientHostLostInfo g_hostLostReconnectSnapshot = {};

// Set by SaveSync_RequestDisconnect; reset at the start of every session.
static volatile bool g_disconnectRequested = false;
static std::atomic<bool> g_processShuttingDown{false};

// The join connection in use, so Disconnect can break a blocking recv.
static INIT_ONCE g_activeSockLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_activeSockLock;
static SOCKET g_activeSock = INVALID_SOCKET;

static INIT_ONCE g_sessionThreadLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_sessionThreadLock;
static HANDLE g_sessionThread = nullptr;

// GetTickCount64 when the current session was started (Host / Connect).
static volatile ULONGLONG g_sessionStartTick = 0;

// Re-sync (see save_sync.h).
#define RESYNC_ANNOUNCE_TIMEOUT_MS 3000 // host: the game thread sends the announcement within this
#define RESYNC_ANNOUNCE_FLUSH_MS 500    // host: lets the announcement leave before the channels close
#define RESYNC_REJOIN_TIMEOUT_MS 30000  // host: how long the player has to come back; client: how long it retries
#define RESYNC_RETRY_MS 500
static volatile bool g_resyncRequested = false; // host: button or timer
static volatile bool g_resyncSendNow = false;   // host: the game thread sends the announcement
static volatile bool g_resyncSent = false;
static volatile bool g_resyncAnnounced = false; // client: the host announced one
static volatile bool g_resyncing = false;
static volatile int g_autoResyncMinutes = 0;
static volatile int g_desyncResyncSeconds = 60;    // lockstep: re-sync this long after a desync (-1 off)
static volatile ULONGLONG g_desyncSince = 0;       // GetTickCount64 of the desync this session saw (0 none)
static volatile ULONGLONG g_lastWorldSentTick = 0; // host: when the player last got the world
// Lockstep: both PCs load the sent save with the determinism layer on and run
// the same ticks (lockstep/lockstep_net) instead of the live sync channels.
// The host chooses; the player follows the host's hello.
static volatile bool g_lockstepChosen = true;
static volatile bool g_dlcCheck = true; // refuse a join when the DLCs differ (both players' choice)
static volatile bool g_lockstep = true; // the current session's mode (lockstep is permanent)
#define LOCKSTEP_CONNECT_TIMEOUT_MS 180000 // the host loads the world again before it listens
#define LOCKSTEP_SILENCE_MS 8000           // a game that sent nothing (not even a keepalive) is gone
static const char* const kResyncReason = "re-sync";
static std::atomic<uint16_t> g_testTcpPortOverride{0};
static HANDLE g_localHostOpenEvent = nullptr;

static uint16_t EffectiveSaveSyncPort()
{
    const uint16_t p = g_testTcpPortOverride.load(std::memory_order_acquire);
    return (p != 0) ? p : static_cast<uint16_t>(SAVE_SYNC_PORT);
}

static void FormatLocalHostOpenEventName(uint16_t port, char* outBuf, size_t outCap)
{
    _snprintf_s(outBuf, outCap, _TRUNCATE, "Local\\RepublicMP_LocalHostOpen_%u", static_cast<unsigned>(port));
}

void SaveSync_SetTcpPortForTest(uint16_t port)
{
    g_testTcpPortOverride.store(port, std::memory_order_release);
}

static void SetState(SessionState s)
{
    if (s == SESSION_HOST_LISTENING) {
        g_state = s;
        if (!g_localHostOpenEvent) {
            char evName[64] = {};
            FormatLocalHostOpenEventName(EffectiveSaveSyncPort(), evName, sizeof(evName));
            g_localHostOpenEvent = CreateEventA(nullptr, TRUE, TRUE, evName);
        } else {
            SetEvent(g_localHostOpenEvent);
        }
    } else {
        if (g_localHostOpenEvent) {
            ResetEvent(g_localHostOpenEvent);
            CloseHandle(g_localHostOpenEvent);
            g_localHostOpenEvent = nullptr;
        }
        g_state = s;
    }
}

bool SaveSync_IsLocalPcHostWaiting()
{
    if (g_state != SESSION_IDLE) return false;
    char evName[64] = {};
    FormatLocalHostOpenEventName(EffectiveSaveSyncPort(), evName, sizeof(evName));
    HANDLE h = OpenEventA(SYNCHRONIZE, FALSE, evName);
    if (!h) return false;
    bool open = (WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
    CloseHandle(h);
    return open;
}

static void WriteProtectedString(char* dst, size_t dstCap, const char* src)
{
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    strncpy_s(dst, dstCap, src ? src : "", _TRUNCATE);
    LeaveCriticalSection(&g_saveSyncStrLock);
}

static void ReadProtectedString(const char* src, char* outBuf, size_t outCap)
{
    if (!outBuf || outCap == 0) return;
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    strncpy_s(outBuf, outCap, src ? src : "", _TRUNCATE);
    LeaveCriticalSection(&g_saveSyncStrLock);
}

static void SetActiveSock(SOCKET s)
{
    TsmEnsureCriticalSection(&g_activeSockLockOnce, &g_activeSockLock);
    EnterCriticalSection(&g_activeSockLock);
    g_activeSock = s;
    if (g_disconnectRequested && s != INVALID_SOCKET) {
        NetworkShutdownSocket(s);
    }
    LeaveCriticalSection(&g_activeSockLock);
}

static void ClearAndCloseActiveSock(SOCKET s)
{
    TsmEnsureCriticalSection(&g_activeSockLockOnce, &g_activeSockLock);
    EnterCriticalSection(&g_activeSockLock);
    if (g_activeSock == s) {
        g_activeSock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&g_activeSockLock);
    if (s != INVALID_SOCKET) {
        NetworkCloseSocket(s);
    }
}

static void JoinAndCloseSessionThread(DWORD waitMs)
{
    TsmEnsureCriticalSection(&g_sessionThreadLockOnce, &g_sessionThreadLock);
    EnterCriticalSection(&g_sessionThreadLock);
    HANDLE h = g_sessionThread;
    g_sessionThread = nullptr;
    LeaveCriticalSection(&g_sessionThreadLock);
    if (h) {
        if (GetThreadId(h) != GetCurrentThreadId()) {
            WaitForSingleObject(h, waitMs);
        }
        CloseHandle(h);
    }
}

static void SetPeerIp(const char* ip)
{
    WriteProtectedString(g_peerIp, sizeof(g_peerIp), ip);
}

static void SetLastPartnerPeer(const char* peer)
{
    WriteProtectedString(g_lastPartnerPeer, sizeof(g_lastPartnerPeer), peer);
}

static void ClearClientHostLost()
{
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    memset(&g_clientHostLost, 0, sizeof(g_clientHostLost));
    LeaveCriticalSection(&g_saveSyncStrLock);
}

static void ClearHostLostReconnectState()
{
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    g_hostLostReconnectPending = false;
    memset(&g_hostLostReconnectSnapshot, 0, sizeof(g_hostLostReconnectSnapshot));
    LeaveCriticalSection(&g_saveSyncStrLock);
}

static void SetHostLostReconnectPending(const ClientHostLostInfo& snap)
{
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    g_clientHostLost.active = false;
    g_hostLostReconnectPending = true;
    g_hostLostReconnectSnapshot = snap;
    g_hostLostReconnectSnapshot.active = true;
    LeaveCriticalSection(&g_saveSyncStrLock);
}

static bool ConsumeHostLostReconnectSnapshot(ClientHostLostInfo* out)
{
    if (!out) return false;
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    const bool pending = g_hostLostReconnectPending;
    if (pending) {
        *out = g_hostLostReconnectSnapshot;
    } else {
        memset(out, 0, sizeof(*out));
    }
    g_hostLostReconnectPending = false;
    memset(&g_hostLostReconnectSnapshot, 0, sizeof(g_hostLostReconnectSnapshot));
    LeaveCriticalSection(&g_saveSyncStrLock);
    return pending;
}

static void RecordClientHostLost(const char* addr, const char* label, const char* reason)
{
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    g_clientHostLost.active = true;
    strncpy_s(g_clientHostLost.hostAddress, sizeof(g_clientHostLost.hostAddress), addr ? addr : "", _TRUNCATE);
    strncpy_s(g_clientHostLost.hostLabel, sizeof(g_clientHostLost.hostLabel),
              (label && label[0]) ? label : (addr ? addr : ""), _TRUNCATE);
    strncpy_s(g_clientHostLost.reason, sizeof(g_clientHostLost.reason), reason ? reason : "", _TRUNCATE);
    g_clientHostLost.lostTick = GetTickCount64();
    LeaveCriticalSection(&g_saveSyncStrLock);
}

static void RestoreHostLostAfterFailedReconnect(const ClientHostLostInfo& prior, const char* fallbackTarget,
                                                const char* reasonOverride)
{
    const char* addr = prior.hostAddress[0] ? prior.hostAddress : (fallbackTarget ? fallbackTarget : "");
    const char* label = prior.hostLabel[0] ? prior.hostLabel : addr;
    const char* lastErr = SaveSync_GetLastError();
    const char* reason = (reasonOverride && reasonOverride[0]) ? reasonOverride
                         : (lastErr && lastErr[0]) ? lastErr
                         : (prior.reason[0] ? prior.reason
                                            : Tr("Хост ещё не готов к подключению", "Host server is not ready yet"));
    RecordClientHostLost(addr, label, reason);
    SaveSync_EnforceWaitingLobbyHold();
}

static void SetSessionError(const char* fmt, ...)
{
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    WriteProtectedString(g_lastError, sizeof(g_lastError), body);
    if (body[0]) {
        TwoStates_AddToast(body, 3);
    }
}

// Writes to tesmioloader.log and the in-game UI log.
void SaveStatus(const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    Logf("republicmp  STATUS: %s", body);
    UiLog_Add(body);
}

// ---------------------------------------------------------------------------
// Build identity
// ---------------------------------------------------------------------------

static INIT_ONCE g_dllCrcOnce = INIT_ONCE_STATIC_INIT;
static unsigned int g_dllCrc = 0;

static BOOL CALLBACK ComputeLocalDllCrcOnce(PINIT_ONCE, PVOID, PVOID*)
{
    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&SaveSync_GetLocalDllCrc, &self) ||
        !GetModuleFileNameA(self, path, MAX_PATH)) {
        Logf("republicmp  SESSION: couldn't locate RepublicMP.dll for the build check");
        g_dllCrc = 0;
        return TRUE;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Logf("republicmp  SESSION: couldn't read [%s] for the build check", path);
        g_dllCrc = 0;
        return TRUE;
    }
    std::vector<unsigned char> buf(64 * 1024);
    mz_ulong crc = MZ_CRC32_INIT;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
        crc = mz_crc32(crc, buf.data(), n);
    fclose(f);
    g_dllCrc = (unsigned int)crc;
    Logf("republicmp  SESSION: this build: version %s, protocol %d, dll crc %08X",
         REPUBLICMP_VERSION, REPUBLICMP_PROTOCOL, g_dllCrc);
    return TRUE;
}

// CRC32 of this RepublicMP.dll file on disk: two players with the same file
// have the same build, whatever its version string says.
unsigned int SaveSync_GetLocalDllCrc()
{
    InitOnceExecuteOnce(&g_dllCrcOnce, &ComputeLocalDllCrcOnce, nullptr, nullptr);
    return g_dllCrc;
}

const char* SaveSync_GetLocalVersion() { return REPUBLICMP_VERSION; }

// ---------------------------------------------------------------------------
// Wire messages
// ---------------------------------------------------------------------------

struct SessionHello {
    char magic[8];     // "RPMPSES"
    int protocol;      // REPUBLICMP_PROTOCOL
    char version[16];  // REPUBLICMP_VERSION
    unsigned int dllCrc;
    int lockstep;      // host: the session runs in lockstep
    int twoStatesMode; // 0 = COOP_MODE_ONE_STATE, 1 = COOP_MODE_TWO_STATES
    int expandBorders; // 1 = unlock 2km map border buffer
    float borderHalfWidth;
    double stateRub[2];
    double stateUsd[2];
};

// Step reports in both directions after the hello.
enum SessionMsgCode {
    MSG_STEP = 0,        // step/percent/text update
    MSG_FILE_FOLLOWS = 1, // host -> client: the save file comes next
    MSG_READY = 2,       // client -> host: world loaded
    MSG_ABORT = 3,       // either way: giving up, text says why
    MSG_MOD_SYNC = 4,    // host -> client: ModSync handshake begins now
    MSG_CLIENT_LOGS = 5, // client -> host: compressed diagnostic log bundle follows (step = byte count)
};
struct SessionMsg {
    int code;
    int step;
    int percent;
    char text[64];
};

static bool SendMsg(SOCKET sock, int code, int step, int percent, const char* text)
{
    SessionMsg m = {};
    m.code = code;
    m.step = step;
    m.percent = percent;
    strncpy_s(m.text, text ? text : "", _TRUNCATE);
    return NetworkSendExact(sock, &m, sizeof(m));
}

static bool RecvMsg(SOCKET sock, SessionMsg* m)
{
    if (!NetworkRecvExact(sock, m, sizeof(*m))) return false;
    m->text[sizeof(m->text) - 1] = 0;
    return true;
}

static void MakeHello(SessionHello* h)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "RPMPSES", 8);
    h->protocol = REPUBLICMP_PROTOCOL;
    strncpy_s(h->version, REPUBLICMP_VERSION, _TRUNCATE);
    h->dllCrc = SaveSync_GetLocalDllCrc();
    h->lockstep = 1;
    h->twoStatesMode = TwoStates_GetMode();
    h->expandBorders = TwoStates_GetExpandBorders() ? 1 : 0;
    h->borderHalfWidth = TwoStates_GetBorderHalfWidth();
}

// Compares the peer's hello with ours; fills why on a mismatch.
static bool HelloMatches(const SessionHello& peer, char* why, size_t whySize)
{
    if (memcmp(peer.magic, "RPMPSES", 8) != 0) {
        _snprintf_s(why, whySize, _TRUNCATE, "%s",
                    Tr("несовместимая версия RepublicMP у второго игрока",
                       "the other side is not a compatible RepublicMP (old build?)"));
        return false;
    }
    char peerVersion[17] = {};
    memcpy(peerVersion, peer.version, 16);
    if (peer.protocol != REPUBLICMP_PROTOCOL || strcmp(peerVersion, REPUBLICMP_VERSION) != 0 ||
        peer.dllCrc != SaveSync_GetLocalDllCrc()) {
        _snprintf_s(why, whySize, _TRUNCATE,
                    Tr("разные сборки RepublicMP: у вас %s (протокол %d, crc %08X), у партнёра %s (протокол %d, crc %08X). "
                       "Обновите RepublicMP.dll до одной версии",
                       "different RepublicMP builds - yours %s (protocol %d, crc %08X), theirs %s (protocol %d, crc %08X). "
                       "Both players need the same RepublicMP.dll"),
                    REPUBLICMP_VERSION, REPUBLICMP_PROTOCOL, SaveSync_GetLocalDllCrc(),
                    peerVersion, peer.protocol, peer.dllCrc);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Live sync channels
// ---------------------------------------------------------------------------

// Closes every channel, connected or still waiting for its peer.
static void ResetChannels()
{
    BuildSync_Reset();
    RoadSync_Reset();
    TerrainSync_Reset();
    LockstepNet_Close();
    Determinism_EndSession();
}

// Blocks while the session is live. Returns why it ended (nullptr = the
// local player stopped it, kResyncReason = a re-sync).
//
// All three channels gone at once = the other game closed the session (or
// is gone). Only some gone = a hiccup or a Debug-window Disconnect: those
// channels are reconnected (host listens again, client retries) and the
// session ends only if they are not back within CHANNEL_RECONNECT_TIMEOUT_MS.
// Messages that were in flight on a dropped channel are lost.
// Lockstep: the one channel. The host re-syncs on request, on the timer, or
// on a state difference while "Re-sync on drift" is set (any limit: in
// lockstep the games either match or they do not); the announcement goes
// over the lockstep channel and both sides take the usual re-sync path.
static const char* MonitorLockstep(bool client)
{
    bool reported = false;
    ULONGLONG lastPing = 0;
    g_desyncSince = 0;
    while (LockstepNet_IsConnected()) {
        if (g_disconnectRequested) return nullptr;
        ULONGLONG now = GetTickCount64();
        if (now - lastPing >= 1000) {
            LockstepNet_Ping();
            lastPing = now;
        }
        // A game that froze or crashed keeps its socket open while Windows
        // reports it: it is gone once it stays silent.
        if (LockstepNet_MsSinceReceived() > LOCKSTEP_SILENCE_MS) {
            return client ? Tr("хост не отвечает (потеряна связь или закрыта игра)",
                               "host stopped responding (connection lost or closed)")
                          : Tr("второй игрок перестал отвечать", "the other game stopped answering");
        }
        if (client && LockstepNet_TakeResync()) {
            SaveSync_OnResyncAnnounced();
            return kResyncReason;
        }
        const char* desync = LockstepNet_Desync();
        if (desync[0] && !reported) {
            reported = true;
            g_desyncSince = now;
            SaveStatus("[LOCKSTEP] %s", desync);
        }
        // In lockstep the games match or not: a re-sync comes on request, or a set time after a desync.
        int delay = g_desyncResyncSeconds;
        bool onDesync = desync[0] && delay >= 0 && now - g_desyncSince >= (ULONGLONG)delay * 1000;
        if (!client && (g_resyncRequested || onDesync)) {
            g_resyncRequested = false;
            SaveStatus(onDesync ? "[HOST] the games differ - re-syncing" : "[HOST] re-syncing");
            LockstepNet_SendResync();
            Sleep(RESYNC_ANNOUNCE_FLUSH_MS);
            g_resyncing = true;
            return kResyncReason;
        }
        Sleep(200);
    }
    // The announcement may arrive right before the host closes the channel.
    if (client && LockstepNet_TakeResync()) {
        SaveSync_OnResyncAnnounced();
        return kResyncReason;
    }
    if (g_disconnectRequested) return nullptr;
    return client ? Tr("хост закрыл сервер или разорвал соединение", "host closed the server or dropped connection")
                  : Tr("Lockstep-соединение закрыто", "the lockstep connection closed");
}

static const char* MonitorLiveSession(bool client)
{
    return MonitorLockstep(client);
}

// ---------------------------------------------------------------------------
// Telemetry & SteamP2P_FlushSend Progress (R7)
// ---------------------------------------------------------------------------

static uint64_t g_transferTotalBytes = 0;
static SaveTransferSpeedMeter g_transferMeter = {};
static std::atomic<bool> g_ackPacedSaveTransfer{false};

struct AckPacedTransferGuard {
    AckPacedTransferGuard() { g_ackPacedSaveTransfer.store(true, std::memory_order_release); }
    ~AckPacedTransferGuard() { g_ackPacedSaveTransfer.store(false, std::memory_order_release); }
};

void SaveSync_SpeedMeterReset(SaveTransferSpeedMeter* meter, ULONGLONG nowMs)
{
    if (!meter) return;
    memset(meter, 0, sizeof(*meter));
    if (nowMs > 0) {
        meter->samples[0].tickMs = nowMs;
        meter->samples[0].bytesDone = 0;
        meter->count = 1;
        meter->lastUpdateTick = nowMs;
    }
}

float SaveSync_SpeedMeterUpdate(SaveTransferSpeedMeter* meter, uint64_t doneBytes, ULONGLONG nowMs)
{
    if (!meter) return 0.0f;
    if (doneBytes > meter->maxBytesSeen) {
        meter->maxBytesSeen = doneBytes;
    }
    const uint64_t monotonicDone = meter->maxBytesSeen;
    if (meter->count == 0) {
        meter->head = 0;
        meter->count = 1;
        meter->samples[0] = { nowMs, monotonicDone };
        meter->lastUpdateTick = nowMs;
        return 0.0f;
    }
    const ULONGLONG dtLast = (nowMs >= meter->lastUpdateTick) ? (nowMs - meter->lastUpdateTick) : 0ULL;
    if (dtLast < SaveTransferSpeedMeter::kMinSampleIntervalMs) {
        return meter->speedEmaMbSec;
    }

    int insertPos = (meter->head + meter->count) % SaveTransferSpeedMeter::kCapacity;
    if (meter->count == SaveTransferSpeedMeter::kCapacity) {
        insertPos = meter->head;
        meter->head = (meter->head + 1) % SaveTransferSpeedMeter::kCapacity;
    } else {
        ++meter->count;
    }
    meter->samples[insertPos] = { nowMs, monotonicDone };

    int baselinePos = (meter->head + meter->count - 2) % SaveTransferSpeedMeter::kCapacity;
    for (int i = 0; i < meter->count - 1; ++i) {
        const int pos = (meter->head + i) % SaveTransferSpeedMeter::kCapacity;
        const ULONGLONG age = (nowMs >= meter->samples[pos].tickMs) ? (nowMs - meter->samples[pos].tickMs) : 0ULL;
        if (age <= SaveTransferSpeedMeter::kWindowMs && age >= SaveTransferSpeedMeter::kMinSampleIntervalMs) {
            baselinePos = pos;
            break;
        }
    }

    const SaveTransferSpeedSample& base = meter->samples[baselinePos];
    const ULONGLONG winDt = (nowMs > base.tickMs) ? (nowMs - base.tickMs) : 0ULL;
    if (winDt >= SaveTransferSpeedMeter::kMinSampleIntervalMs) {
        const uint64_t dBytes = (monotonicDone > base.bytesDone) ? (monotonicDone - base.bytesDone) : 0ULL;
        const double instMbSec = (static_cast<double>(dBytes) / (1024.0 * 1024.0)) /
                                 (static_cast<double>(winDt) / 1000.0);
        if (meter->speedEmaMbSec <= 0.001f) {
            meter->speedEmaMbSec = static_cast<float>(instMbSec);
        } else {
            meter->speedEmaMbSec = 0.7f * meter->speedEmaMbSec + 0.3f * static_cast<float>(instMbSec);
        }
    }
    meter->lastUpdateTick = nowMs;
    return meter->speedEmaMbSec;
}

static uint64_t GetFileSize64(const char* path)
{
    if (!path || !path[0]) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER size;
        size.LowPart = fad.nFileSizeLow;
        size.HighPart = (LONG)fad.nFileSizeHigh;
        return (uint64_t)size.QuadPart;
    }
    return 0;
}

static void UpdateTransferSpeed(uint64_t done)
{
    (void)SaveSync_SpeedMeterUpdate(&g_transferMeter, done, GetTickCount64());
}

void SaveSync_OnP2PFlushProgress(int queuedBytes, bool finished)
{
    if (g_ackPacedSaveTransfer.load(std::memory_order_acquire)) return;
    if (!g_inSaveTransfer.load(std::memory_order_relaxed) || g_transferTotalBytes == 0) return;
    if (finished || queuedBytes <= 0) {
        UpdateTransferSpeed(g_transferTotalBytes);
        SyncBlock_SetPercentEx(100, 0LL, (int64_t)g_transferTotalBytes, g_transferMeter.speedEmaMbSec);
        return;
    }
    uint64_t done = (g_transferTotalBytes > (uint64_t)queuedBytes)
        ? (g_transferTotalBytes - (uint64_t)queuedBytes)
        : 0;
    int percent = (int)((done * 100) / g_transferTotalBytes);
    if (percent > 99) percent = 99;
    UpdateTransferSpeed(done);
    SyncBlock_SetPercentEx(percent, (int64_t)queuedBytes, (int64_t)g_transferTotalBytes, g_transferMeter.speedEmaMbSec);
}

bool SaveSync_InstallHooks()
{
    return true;
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

// Shared by the progress callbacks: the step's bar, plus (optionally) a
// relay of the percentage to the other side every 10%.
struct ProgressCtx {
    SOCKET relaySock; // INVALID_SOCKET = do not relay
    int step;
    const char* text;
    int lastRelayed;
};

static void OnProgress(ProgressCtx* ctx, unsigned long long done, unsigned long long total)
{
    if (total == 0) return;
    int percent = (int)((done * 100) / total);
    if (percent > 100) percent = 100;

    UpdateTransferSpeed(done);
    int64_t queued = (int64_t)(total > done ? total - done : 0);
    SyncBlock_SetPercentEx(percent, queued, (int64_t)total, g_transferMeter.speedEmaMbSec);

    if (ctx && ctx->relaySock != INVALID_SOCKET && percent / 10 != ctx->lastRelayed / 10) {
        ctx->lastRelayed = percent;
        SendMsg(ctx->relaySock, MSG_STEP, ctx->step, percent, ctx->text);
    }
}

static void NetworkProgressCb(unsigned long long done, unsigned long long total, void* userData)
{
    OnProgress((ProgressCtx*)userData, done, total);
}

static void ZipProgressCb(size_t done, size_t total, void* userData)
{
    OnProgress((ProgressCtx*)userData, (unsigned long long)done, (unsigned long long)total);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool SaveSync_ResolveGameSaveDir(HMODULE exeMod, const char* fallbackBaseDir,
                                 char* outPath, size_t outSize, const char* saveName)
{
    if (!outPath || outSize == 0 || !saveName) return false;
    outPath[0] = '\0';
    if (_strnicmp(saveName, "save\\", 5) == 0 || _strnicmp(saveName, "save/", 5) == 0) {
        saveName += 5;
    }

    char rootDir[MAX_PATH] = {};
    auto tryModuleDir = [&](HMODULE mod) -> bool {
        char exePath[MAX_PATH] = {};
        DWORD len = GetModuleFileNameA(mod, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return false;
        char* lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) return false;
        *lastSlash = '\0';
        strncpy_s(rootDir, sizeof(rootDir), exePath, _TRUNCATE);
        return rootDir[0] != '\0';
    };

    bool hasRoot = (exeMod != nullptr && tryModuleDir(exeMod));
    if (!hasRoot && fallbackBaseDir && fallbackBaseDir[0] != '\0') {
        strncpy_s(rootDir, sizeof(rootDir), fallbackBaseDir, _TRUNCATE);
        hasRoot = (rootDir[0] != '\0');
    }
    if (!hasRoot) {
        hasRoot = tryModuleDir(nullptr);
    }
    if (!hasRoot) return false;

    return _snprintf_s(outPath, outSize, _TRUNCATE, "%s\\media_soviet\\save\\%s", rootDir, saveName) >= 0;
}

static bool GetGameSaveDir(char* outPath, size_t outSize, const char* saveName)
{
    return SaveSync_ResolveGameSaveDir(g_exe, g_baseDir, outPath, outSize, saveName);
}

bool SaveSync_GetGameSaveDirForTest(char* outPath, size_t outSize, const char* saveName)
{
    return GetGameSaveDir(outPath, outSize, saveName);
}

// ---------------------------------------------------------------------------
// DLCs
// ---------------------------------------------------------------------------
//
// The game's DLCs are folders media_soviet\dlcN (buildings, vehicles,
// terrains). A player without one cannot draw the other's DLC map, buildings
// and vehicles, and in lockstep the vehicle type table (commands name types by
// their index in it) differs. After the version check both sides send their
// list and their checker choice; the join is refused when the lists differ
// and both players keep the checker on.

struct SessionDlcs {
    char magic[8];  // "RPMPDLC"
    int check;      // this player's checker is on
    char list[248]; // the dlcN folder names, sorted, comma-separated
};

static void ListDlcs(char* out, size_t outSize)
{
    out[0] = 0;
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return;
    *lastSlash = '\0';
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::string(exePath) + "\\media_soviet", ec)) {
        std::string name = e.path().filename().string();
        if (_strnicmp(name.c_str(), "dlc", 3) != 0 || !e.is_directory(ec)) continue;
        if (std::filesystem::directory_iterator(e.path(), ec) == std::filesystem::directory_iterator()) continue; // empty
        for (char& c : name) c = (char)tolower((unsigned char)c);
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        if (out[0]) strncat_s(out, outSize, ",", _TRUNCATE);
        strncat_s(out, outSize, n.c_str(), _TRUNCATE);
    }
}

static void MakeDlcs(SessionDlcs* d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->magic, "RPMPDLC", 8);
    d->check = g_dlcCheck ? 1 : 0;
    ListDlcs(d->list, sizeof(d->list));
}

static char g_sessionHostDlcs[64] = {};
static char g_sessionPlayerDlcs[64] = {};
static volatile LONG g_sessionDlcsValid = 0;

bool SaveSync_SafeReadAscii(const char* src, char* outBuf, size_t outBufSize)
{
    if (!src || !outBuf || outBufSize == 0) return false;
    outBuf[0] = '\0';
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        size_t i = 0;
        for (; i < outBufSize - 1; ++i) {
            char c = src[i];
            outBuf[i] = c;
            if (c == '\0') break;
        }
        outBuf[i] = '\0';
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outBuf[0] = '\0';
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

bool SaveSync_ExtractDlcToken(const char* typeName, char* outToken, size_t outSize)
{
    if (outToken && outSize > 0) outToken[0] = '\0';
    if (!typeName || !outToken || outSize < 6) return false;

    const char* p = typeName;
    if (_strnicmp(p, "media_soviet/", 13) == 0 || _strnicmp(p, "media_soviet\\", 13) == 0) {
        p += 13;
    }

    if (_strnicmp(p, "dlc", 3) != 0) return false;
    size_t i = 3;
    while (p[i] >= '0' && p[i] <= '9') ++i;
    if (i == 3 || i >= outSize) return false;

    char sep = p[i];
    if (sep != '_' && sep != '/' && sep != '\\') return false;

    for (size_t k = 0; k < i; ++k) {
        outToken[k] = (char)tolower((unsigned char)p[k]);
    }
    outToken[i] = '\0';
    return true;
}

uint32_t SaveSync_GetDlcMask(const char* dlcList)
{
    if (!dlcList || !dlcList[0]) return 0;
    uint32_t mask = 0;
    const char* p = dlcList;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (_strnicmp(p, "dlc", 3) == 0) {
            const char* num = p + 3;
            int dlcNum = 0;
            while (*num >= '0' && *num <= '9') {
                dlcNum = dlcNum * 10 + (*num - '0');
                ++num;
            }
            if (dlcNum >= 1 && dlcNum <= 31) {
                mask |= (1u << (dlcNum - 1));
            }
            p = num;
        }
        while (*p && *p != ',' && *p != ' ' && *p != '\t') ++p;
    }
    return mask;
}

void SaveSync_FormatDlcNames(uint32_t mask, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (mask == 0) {
        strncpy_s(out, outSize, "none", _TRUNCATE);
        return;
    }
    struct DlcInfo {
        uint32_t bit;
        const char* name;
        const char* tag;
    };
    static const DlcInfo kDlcs[] = {
        { 1u << 0, "Help for Ukraine", "dlc1" },
        { 1u << 1, "Biomes", "dlc2" },
        { 1u << 2, "Early Start", "dlc3" },
        { 1u << 3, "World Maps", "dlc4" },
        { 1u << 4, "Space Program", "dlc5" }
    };
    bool first = true;
    for (size_t i = 0; i < sizeof(kDlcs) / sizeof(kDlcs[0]); ++i) {
        if (mask & kDlcs[i].bit) {
            char item[64];
            _snprintf_s(item, sizeof(item), _TRUNCATE, "%s%s (%s)", first ? "" : ", ", kDlcs[i].name, kDlcs[i].tag);
            strncat_s(out, outSize, item, _TRUNCATE);
            first = false;
        }
    }
    for (int d = 6; d <= 16; ++d) {
        uint32_t b = (1u << (d - 1));
        if (mask & b) {
            char item[32];
            _snprintf_s(item, sizeof(item), _TRUNCATE, "%sDLC %d (dlc%d)", first ? "" : ", ", d, d);
            strncat_s(out, outSize, item, _TRUNCATE);
            first = false;
        }
    }
}

void SaveSync_GetDlcMasks(uint32_t* outHostMask, uint32_t* outClientMask, uint32_t* outCommonMask)
{
    char host[sizeof(g_sessionHostDlcs)] = {};
    char client[sizeof(g_sessionPlayerDlcs)] = {};
    EnterCriticalSection(&g_lock);
    strncpy_s(host, g_sessionHostDlcs, _TRUNCATE);
    strncpy_s(client, g_sessionPlayerDlcs, _TRUNCATE);
    LeaveCriticalSection(&g_lock);

    uint32_t hMask = SaveSync_GetDlcMask(host);
    uint32_t cMask = SaveSync_GetDlcMask(client);
    if (outHostMask) *outHostMask = hMask;
    if (outClientMask) *outClientMask = cMask;
    if (outCommonMask) *outCommonMask = hMask & cMask;
}

bool SaveSync_IsDlcTokenInList(const char* list, const char* dlcToken)
{
    if (!list || !list[0] || !dlcToken || !dlcToken[0]) return false;
    size_t tokLen = strlen(dlcToken);
    const char* p = list;
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ',' && *p != ' ') ++p;
        size_t len = (size_t)(p - start);
        if (len == tokLen && _strnicmp(start, dlcToken, tokLen) == 0) return true;
    }
    return false;
}

bool SaveSync_CheckBuildingDlcAllowed(const char* typeName)
{
    SessionState st = SaveSync_GetState();
    if (st != SESSION_HOST_IN_GAME && st != SESSION_CLIENT_IN_GAME) return true;
    if (InterlockedCompareExchange(&g_sessionDlcsValid, 0, 0) == 0 && !IsDualTestbed()) return true;

    char dlcToken[16] = {};
    if (!SaveSync_ExtractDlcToken(typeName, dlcToken, sizeof(dlcToken))) return true;

    EnterCriticalSection(&g_lock);
    bool hostHas = SaveSync_IsDlcTokenInList(g_sessionHostDlcs, dlcToken);
    bool playerHas = SaveSync_IsDlcTokenInList(g_sessionPlayerDlcs, dlcToken);
    LeaveCriticalSection(&g_lock);

    if (hostHas && playerHas) return true;

    static ULONGLONG s_lastDlcBlockToastMs = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastDlcBlockToastMs >= 1500) {
        s_lastDlcBlockToastMs = now;
        char upperDlc[16] = {};
        for (size_t k = 0; dlcToken[k] && k + 1 < sizeof(upperDlc); ++k)
            upperDlc[k] = (char)toupper((unsigned char)dlcToken[k]);
        SaveStatus(Tr("[КООП] Здание [%s] заблокировано: у партнёра не установлено %s (вызовет рассинхрон)!",
                      "[CO-OP] Building [%s] blocked: partner does not have %s installed (would cause desync)!"),
                   typeName, upperDlc);
        char toast[220];
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("⛔ Здание из %s недоступно: у партнёра нет этого DLC!",
                       "⛔ Building from %s blocked: partner lacks this DLC!"),
                    upperDlc);
        TwoStates_AddToast(toast, 3);
    }
    return false;
}

static void SaveSync_NotifyVehicleDlcBlocked(const char* typeName, const char* dlcToken)
{
    static ULONGLONG s_lastVehDlcBlockToastMs = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastVehDlcBlockToastMs >= 1500) {
        s_lastVehDlcBlockToastMs = now;
        char upperDlc[16] = {};
        for (size_t k = 0; dlcToken[k] && k + 1 < sizeof(upperDlc); ++k)
            upperDlc[k] = (char)toupper((unsigned char)dlcToken[k]);

        Logf("republicmp  PURCHASE_DLC: Vehicle [%s] blocked: unshared %s (host=[%s], player=[%s])",
             typeName, upperDlc, g_sessionHostDlcs, g_sessionPlayerDlcs);
        SaveStatus(Tr("[КООП] Техника [%s] заблокирована: у партнёра не установлено %s (вызовет рассинхрон)!",
                      "[CO-OP] Vehicle [%s] blocked: partner does not have %s installed (would cause desync)!"),
                   typeName, upperDlc);

        TwoStates_AddToast(Tr("★ ТРАНСПОРТ ИЗ НЕУСТАНОВЛЕННОГО DLC ЗАБЛОКИРОВАН В КООПЕРАТИВЕ ★",
                              "★ VEHICLE FROM UNINSTALLED DLC BLOCKED IN CO-OP ★"), 3);
    }
}

bool SaveSync_CheckVehicleDlcAllowed(const void* vehicleTypePtr)
{
    // Single-player mode isolation: unrestricted in single player!
    if (SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning() && !IsDualTestbed()) {
        return true;
    }
    if (InterlockedCompareExchange(&g_sessionDlcsValid, 0, 0) == 0 && !IsDualTestbed()) {
        return true;
    }
    if (!vehicleTypePtr) return true;

    // 1. Check lead vehicle
    char typeName[64] = {};
    if (SafeReadAsciiDirect((const char*)vehicleTypePtr + 0x200, typeName, sizeof(typeName)) && typeName[0]) {
        char dlcToken[16] = {};
        if (SaveSync_ExtractDlcToken(typeName, dlcToken, sizeof(dlcToken))) {
            EnterCriticalSection(&g_lock);
            bool hostHas = SaveSync_IsDlcTokenInList(g_sessionHostDlcs, dlcToken);
            bool playerHas = SaveSync_IsDlcTokenInList(g_sessionPlayerDlcs, dlcToken);
            LeaveCriticalSection(&g_lock);

            if (!hostHas || !playerHas) {
                SaveSync_NotifyVehicleDlcBlocked(typeName, dlcToken);
                return false;
            }
        }
    }

    // 2. Check coupled trailers / wagons in type + 0x7C58
    BYTE* v = nullptr;
    BYTE* e = nullptr;
    if (SafeReadPod((const char*)vehicleTypePtr + 0x7C58, &v) &&
        SafeReadPod((const char*)vehicleTypePtr + 0x7C60, &e) &&
        v && e > v && (e - v) % 0x110 == 0)
    {
        constexpr unsigned kMaxTrailersChecked = 64;
        size_t count = (size_t)(e - v) / 0x110;
        if (count > kMaxTrailersChecked) count = kMaxTrailersChecked;
        unsigned n = (unsigned)count;
        for (unsigned i = 0; i < n; ++i) {
            BYTE* trType = nullptr;
            if (SafeReadPod((const char*)(v + i * 0x110 + 0x100), &trType) && trType) {
                char trTypeName[64] = {};
                if (SafeReadAsciiDirect((const char*)trType + 0x200, trTypeName, sizeof(trTypeName)) && trTypeName[0]) {
                    char trDlcTok[16] = {};
                    if (SaveSync_ExtractDlcToken(trTypeName, trDlcTok, sizeof(trDlcTok))) {
                        EnterCriticalSection(&g_lock);
                        bool hostHas = SaveSync_IsDlcTokenInList(g_sessionHostDlcs, trDlcTok);
                        bool playerHas = SaveSync_IsDlcTokenInList(g_sessionPlayerDlcs, trDlcTok);
                        LeaveCriticalSection(&g_lock);

                        if (!hostHas || !playerHas) {
                            SaveSync_NotifyVehicleDlcBlocked(trTypeName, trDlcTok);
                            return false;
                        }
                    }
                }
            }
        }
    }

    return true;
}

void SaveSync_SetTestDlcLists(const char* hostList, const char* clientList)
{
    EnterCriticalSection(&g_lock);
    strncpy_s(g_sessionHostDlcs, hostList ? hostList : "", _TRUNCATE);
    strncpy_s(g_sessionPlayerDlcs, clientList ? clientList : "", _TRUNCATE);
    InterlockedExchange(&g_sessionDlcsValid, (hostList || clientList) ? 1 : 0);
    LeaveCriticalSection(&g_lock);
}

// Both sides decide alike: false refuses the join (why says what differs).
static bool DlcsMatch(const SessionDlcs& mine, const SessionDlcs& theirs, bool host, char* why, size_t whySize)
{
    char peer[sizeof(theirs.list) + 1] = {};
    memcpy(peer, theirs.list, sizeof(theirs.list));
    const char* hostList = host ? mine.list : peer;
    const char* playerList = host ? peer : mine.list;
    EnterCriticalSection(&g_lock);
    strncpy_s(g_sessionHostDlcs, hostList, _TRUNCATE);
    strncpy_s(g_sessionPlayerDlcs, playerList, _TRUNCATE);
    InterlockedExchange(&g_sessionDlcsValid, 1);
    LeaveCriticalSection(&g_lock);
    Logf("republicmp  SESSION: DLCs - host [%s], player [%s]", hostList[0] ? hostList : "none", playerList[0] ? playerList : "none");

    uint32_t hostMask = SaveSync_GetDlcMask(hostList);
    uint32_t playerMask = SaveSync_GetDlcMask(playerList);
    uint32_t commonMask = hostMask & playerMask;
    uint32_t disabledMask = (hostMask | playerMask) & ~commonMask;
    char hostNames[128] = {}, playerNames[128] = {}, commonNames[128] = {}, disabledNames[128] = {};
    SaveSync_FormatDlcNames(hostMask, hostNames, sizeof(hostNames));
    SaveSync_FormatDlcNames(playerMask, playerNames, sizeof(playerNames));
    SaveSync_FormatDlcNames(commonMask, commonNames, sizeof(commonNames));
    SaveSync_FormatDlcNames(disabledMask, disabledNames, sizeof(disabledNames));

    DiagLog_Printf("DLC", "=================================================================");
    DiagLog_Printf("DLC", "DLC Handshake Negotiated:");
    DiagLog_Printf("DLC", "  Host Installed DLCs   : 0x%04X [%s]", hostMask, hostNames);
    DiagLog_Printf("DLC", "  Client Installed DLCs : 0x%04X [%s]", playerMask, playerNames);
    DiagLog_Printf("DLC", "  Common Negotiated Mask: 0x%04X [%s]", commonMask, commonNames);
    DiagLog_Printf("DLC", "  Disabled Unshared DLCs: 0x%04X [%s] -> BLOCKED in co-op", disabledMask, disabledNames);
    DiagLog_Printf("DLC", "=================================================================");

    if (strcmp(mine.list, peer) == 0) return true;
    bool refuse = mine.check && theirs.check;
    _snprintf_s(why, whySize, _TRUNCATE,
                Tr("разный набор DLC: хост [%s], клиент [%s]%s",
                   "the DLCs differ - host [%s], player [%s]. DLC maps, buildings and vehicles cannot be shown without the DLC%s"),
                hostList[0] ? hostList : Tr("нет", "none"), playerList[0] ? playerList : Tr("нет", "none"),
                refuse ? Tr(". Установите одинаковые DLC или отключите «Проверка DLC» в Настройках коопа",
                            "; install the same DLCs, or untick \"Check DLCs\" on either PC to play a map that needs none")
                       : Tr(" (проверка DLC выключена — продолжаем)",
                            " (the DLC check is off - playing anyway)"));
    if (!refuse) {
        Logf("republicmp  SESSION: %s", why);
        SaveStatus("%s %s", host ? "[HOST]" : "[CLIENT]", why);
    }
    return !refuse;
}

// A save name that will not collide with the player's own saves.
static void GenerateRandomSaveName(char* out, size_t outSize)
{
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(nullptr));
        seeded = true;
    }
    unsigned tag = (unsigned)GetTickCount() ^ ((unsigned)rand() << 16) ^ (unsigned)rand();
    _snprintf_s(out, outSize, _TRUNCATE, "mp_syncsave_%08x", tag);
}

// The host's newest join save. Each join (and so each re-sync) writes a new
// one; every older one is deleted, so they do not pile up - also those left
// by earlier runs of the game (one per run had stayed, ~150 MB each).
static bool IsHostJoinSaveName(const std::string& name)
{
    const char* prefix = "mp_syncsave_";
    if (name.size() != strlen(prefix) + 8 || name.compare(0, strlen(prefix), prefix) != 0) return false;
    for (size_t i = strlen(prefix); i < name.size(); i++)
        if (!isxdigit((unsigned char)name[i])) return false;
    return true;
}

static void KeepOnlyNewestHostSave(const char* newSaveDir)
{
    std::error_code ec;
    std::filesystem::path keep(newSaveDir);
    std::filesystem::directory_iterator it(keep.parent_path(), ec), end;
    int deleted = 0;
    for (; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string name = it->path().filename().string();
        if (!IsHostJoinSaveName(name) || _stricmp(name.c_str(), keep.filename().string().c_str()) == 0) continue;
        std::error_code rm;
        std::filesystem::remove_all(it->path(), rm);
        if (rm) Logf("republicmp  SESSION: couldn't delete the old join save [%s]: %s", name.c_str(), rm.message().c_str());
        else deleted++;
    }
    if (deleted) Logf("republicmp  SESSION: deleted %d old join save(s)", deleted);
}

static void PeerIpOf(SOCKET sock, char* out, size_t outSize)
{
    NetworkGetPeerName(sock, out, outSize);
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

static void HostStep(SOCKET sock, int step, const char* text, int percent = -1)
{
    SyncBlock_SetStep(step, JOIN_STEPS, text, percent);
    SaveStatus("[HOST] step %d/%d: %s", step, JOIN_STEPS, text);
    (void)sock;
}

static void ExtractMapBasename(const char* raw, char* outMap, size_t outSize)
{
    if (!raw || !raw[0] || strstr(raw, "mp_syncsave_") != nullptr) return;
    const char* p = raw;
    if (_strnicmp(p, "save_coop/", 10) == 0 || _strnicmp(p, "save_coop\\", 10) == 0) {
        p += 10;
    } else if (_strnicmp(p, "save/", 5) == 0 || _strnicmp(p, "save\\", 5) == 0) {
        p += 5;
    }
    const char* s1 = strrchr(p, '/');
    const char* s2 = strrchr(p, '\\');
    const char* sep = (!s1) ? s2 : (!s2 ? s1 : (s1 > s2 ? s1 : s2));
    const char* base = sep ? (sep + 1) : p;
    if (base[0] && strstr(base, "mp_syncsave_") == nullptr) {
        strncpy_s(outMap, outSize, base, _TRUNCATE);
    }
}

static void GetHostDisplayMapName(char* outMap, size_t outSize)
{
    if (!outMap || outSize == 0) return;
    outMap[0] = '\0';
    char raw[128] = {};
    if (g_exe) {
        if (SaveSync_SafeReadAscii((const char*)((uintptr_t)g_exe + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET),
                                   raw, sizeof(raw))) {
            ExtractMapBasename(raw, outMap, outSize);
        }
        if (!outMap[0] &&
            SaveSync_SafeReadAscii((const char*)((uintptr_t)g_exe + SAVE_NAME_DAT_RVA), raw, sizeof(raw))) {
            ExtractMapBasename(raw, outMap, outSize);
        }
    }
    if (!outMap[0]) {
        strncpy_s(outMap, outSize, "Co-op World", _TRUNCATE);
    }
}

static void SafeSaveHostWorldSaveNames(char* outField, size_t fieldSize, char* outDat, size_t datSize)
{
    outField[0] = '\0';
    outDat[0] = '\0';
    if (!g_exe) return;
    DiagLog_BeginSafeSeh();
    __try {
        const char* curField = (const char*)((uintptr_t)g_exe + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET);
        if (curField && curField[0] && strstr(curField, "mp_syncsave_") == nullptr) {
            strncpy_s(outField, fieldSize, curField, _TRUNCATE);
        } else {
            strncpy_s(outField, fieldSize, "save/coop_last_session", _TRUNCATE);
        }
        const char* curDat = (const char*)((uintptr_t)g_exe + SAVE_NAME_DAT_RVA);
        if (curDat && curDat[0] && strstr(curDat, "mp_syncsave_") == nullptr) {
            strncpy_s(outDat, datSize, curDat, _TRUNCATE);
        } else {
            strncpy_s(outDat, datSize, "save\\coop_last_session", _TRUNCATE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void SafeRestoreHostWorldSaveNames(const char* origField, const char* origDat)
{
    if (!g_exe) return;
    DiagLog_BeginSafeSeh();
    __try {
        if (origField && origField[0]) {
            char* dstField = (char*)((uintptr_t)g_exe + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET);
            strncpy_s(dstField, SAVE_NAME_FIELD_MAX_LEN, origField, _TRUNCATE);
        }
        if (origDat && origDat[0]) {
            char* dstDat = (char*)((uintptr_t)g_exe + SAVE_NAME_DAT_RVA);
            strncpy_s(dstDat, SAVE_NAME_MAX_LEN, origDat, _TRUNCATE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static bool TryRecvJoinLogBundle(SOCKET sock, int byteSize)
{
    if (byteSize <= 0 || byteSize > (2 << 20)) return true;
    std::vector<unsigned char> logBuf((size_t)byteSize);
    if (!NetworkRecvExact(sock, logBuf.data(), logBuf.size())) return false;
    DiagLog_SaveRemoteClientLogBundle(logBuf.data(), logBuf.size());
    return true;
}

static void SendJoinDiagLogBundleIfEnabled(SOCKET sock, const char* reason)
{
    if (!TwoStates_GetSendClientLogs()) return;
    std::vector<unsigned char> logBundle;
    if (DiagLog_BuildClientLogBundle(logBundle, reason)) {
        if (SendMsg(sock, MSG_CLIENT_LOGS, (int)logBundle.size(), -1, "")) {
            NetworkSendExact(sock, logBundle.data(), logBundle.size());
        }
    }
}

static bool WaitForJoinControlMsg(SOCKET sock, int targetCode, bool isHost,
                                  const char* disconnectErr, const char* abortFmt, const char* stepFmt)
{
    for (;;) {
        SessionMsg m;
        if (!RecvMsg(sock, &m)) {
            SetSessionError("%s", disconnectErr);
            return false;
        }
        if (m.code == targetCode) return true;
        if (m.code == MSG_ABORT) {
            SetSessionError(abortFmt, m.text);
            if (isHost) SaveStatus("[HOST] the player gave up: %s", m.text);
            return false;
        }
        if (m.code == MSG_CLIENT_LOGS) {
            if (!TryRecvJoinLogBundle(sock, m.step)) return false;
            continue;
        }
        if (m.code == MSG_STEP) {
            char text[96];
            _snprintf_s(text, sizeof(text), _TRUNCATE, stepFmt, m.text);
            if (isHost && m.step != 5) HostStep(sock, m.step, text, m.percent);
            else SyncBlock_SetStep(isHost ? 5 : m.step, JOIN_STEPS, text, m.percent);
        }
    }
}

static bool HostPerformHandshake(SOCKET sock, const char* activeName, SessionHello* outMine)
{
    HostStep(sock, 1, Tr("Подключение игрока — проверка версий", "A player is connecting - checking versions"));
    SessionHello mine = {}, theirs = {};
    MakeHello(&mine);
    TwoStates_PrepareHostSave(g_resyncing, mine.stateRub, mine.stateUsd);
    if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
        SetSessionError(Tr("Игрок отключился во время проверки версий", "the player dropped during the version check"));
        return false;
    }
    NetworkSendExact(sock, &mine, sizeof(mine)); // always, so the client can show what differs
    char why[256];
    if (!HelloMatches(theirs, why, sizeof(why))) {
        SetSessionError(Tr("Вход отклонён: %s", "join refused: %s"), why);
        SaveStatus("[HOST] join refused: %s", why);
        return false;
    }
    SessionDlcs myDlcs, theirDlcs;
    MakeDlcs(&myDlcs);
    if (!NetworkSendExact(sock, &myDlcs, sizeof(myDlcs)) || !NetworkRecvExact(sock, &theirDlcs, sizeof(theirDlcs)) ||
        memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
        SetSessionError(Tr("Игрок отключился во время проверки DLC", "the player dropped during the DLC check"));
        return false;
    }
    char dlcWhy[512];
    if (!DlcsMatch(myDlcs, theirDlcs, true, dlcWhy, sizeof(dlcWhy))) {
        SetSessionError(Tr("Вход отклонён: %s", "join refused: %s"), dlcWhy);
        SaveStatus("[HOST] join refused: %s", dlcWhy);
        return false;
    }

    HostStep(sock, 1, Tr("Синхронизация модов Мастерской Steam", "Synchronizing Steam Workshop mods"), 0);
    SendMsg(sock, MSG_MOD_SYNC, 1, 0, Tr("проверка модов", "checking mods"));
    char activeSaveDir[MAX_PATH] = {};
    if (activeName && activeName[0]) {
        GetGameSaveDir(activeSaveDir, sizeof(activeSaveDir), activeName);
    }
    char modWhy[256] = {};
    ModSyncHostResult modRes = ModSync_HostHandshake(sock, activeSaveDir, modWhy, sizeof(modWhy));
    if (modRes == MOD_SYNC_HOST_CLIENT_MISSING) {
        char missingToast[256] = {};
        _snprintf_s(missingToast, sizeof(missingToast), _TRUNCATE,
                    Tr("Клиент скачивает %s недостающих модов из Steam Workshop...",
                       "Client is downloading %s missing mods from Steam Workshop..."),
                    modWhy[0] ? modWhy : "N");
        SaveStatus("[HOST] %s", missingToast);
        TwoStates_AddToast(missingToast, 5);
        return false;
    }
    if (modRes != MOD_SYNC_HOST_OK) {
        SetSessionError(Tr("Ошибка синхронизации модов: %s", "mod sync failed: %s"), modWhy);
        SaveStatus("[HOST] mod sync failed: %s", modWhy);
        return false;
    }
    *outMine = mine;
    return true;
}

static bool HostSaveAndPackWorld(SOCKET sock, const char* liveSaveName,
                                 char* outSaveDir, size_t saveDirCap,
                                 char* outZipPath, size_t zipPathCap)
{
    char saveDir[MAX_PATH] = {};
    if (!GetGameSaveDir(saveDir, sizeof(saveDir), liveSaveName)) {
        SetSessionError(Tr("Не удалось сформировать путь сохранения", "couldn't build the save path"));
        SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't save the world");
        return false;
    }
    HostStep(sock, 2, Tr("Сохранение мира для передачи", "Saving the world"));
    SendMsg(sock, MSG_STEP, 2, -1, Tr("сохранение мира", "saving the world"));
    std::error_code parentEc;
    std::filesystem::create_directories(std::filesystem::path(saveDir).parent_path(), parentEc);
    if (!CreateDirectoryA(saveDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        DWORD err = GetLastError();
        Logf("republicmp  SESSION: CreateDirectoryA failed for [%s] (err=%lu)", saveDir, (unsigned long)err);
        SetSessionError(Tr("Не удалось создать папку сохранения (код %lu)", "couldn't create the save folder (error %lu)"), err);
        SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't save the world");
        return false;
    }
    strncpy_s(outSaveDir, saveDirCap, saveDir, _TRUNCATE);
    TriggerSaveGame(liveSaveName);
    if (!WaitForSaveToFinish(0, &g_disconnectRequested)) {
        SendMsg(sock, MSG_ABORT, 0, -1, "host stopped hosting");
        return false;
    }
    if (TwoStates_GetExpandBorders()) {
        TwoStates_PatchSaveScriptIni(saveDir, 0.0f);
    }

    const char* hostLogReason = DiagLog_HasPrevCrashOrDesyncLog() ? "HOST_PREV_CRASH" : "HOST_JOIN";
    SendJoinDiagLogBundleIfEnabled(sock, hostLogReason);

    char zipPath[MAX_PATH] = {};
    _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
    HostStep(sock, 3, Tr("Упаковка сохранения", "Packing the save"), 0);
    SendMsg(sock, MSG_STEP, 3, 0, Tr("упаковка сохранения", "packing the save"));
    ProgressCtx packCtx = { sock, 3, Tr("упаковка сохранения", "packing the save"), -100 };
    strncpy_s(outZipPath, zipPathCap, zipPath, _TRUNCATE);
    if (!ZipSaveDirectory(saveDir, zipPath, ZipProgressCb, &packCtx)) {
        SetSessionError(Tr("Ошибка упаковки сохранения", "packing the save failed"));
        SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't pack the save");
        return false;
    }
    if (g_disconnectRequested) {
        SendMsg(sock, MSG_ABORT, 0, -1, "host stopped hosting");
        return false;
    }
    return true;
}

static bool HostSendWorldZip(SOCKET sock, const char* zipPath)
{
    HostStep(sock, 4, Tr("Передача мира второму игроку", "Sending the world to the player"), 0);
    SendMsg(sock, MSG_FILE_FOLLOWS, 4, -1, "");
    ProgressCtx sendCtx = { INVALID_SOCKET, 4, nullptr, 0 };
    const uint64_t totalSize = GetFileSize64(zipPath);
    g_transferTotalBytes = totalSize;
    const ULONGLONG startTick = GetTickCount64();
    SaveSync_SpeedMeterReset(&g_transferMeter, startTick);
    bool sendOk = false;
    {
        SaveTransferGuard transferGuard;
        AckPacedTransferGuard ackGuard;
        sendOk = NetworkSendFileAckPaced(sock, zipPath, NetworkProgressCb, &sendCtx, &g_disconnectRequested);
    }
    const ULONGLONG elapsedMs = GetTickCount64() - startTick;
    if (sendOk && totalSize > 0 && elapsedMs > 0) {
        const double avgMbSec = (static_cast<double>(totalSize) / (1024.0 * 1024.0)) /
                                (static_cast<double>(elapsedMs) / 1000.0);
        SaveStatus("[HOST] save transfer complete (%llu bytes in %llu ms, avg %.2f MB/s)",
                   static_cast<unsigned long long>(totalSize), elapsedMs, avgMbSec);
    }
    if (!sendOk) {
        SetSessionError(Tr("Ошибка передачи сохранения — соединение потеряно", "sending the save failed - connection lost"));
    }
    return sendOk;
}

static bool HostLoadSharedWorld(const SessionHello& mine, const char* liveSaveName, bool rejoin)
{
    if (!g_lockstep) return true;
    HostStep(INVALID_SOCKET, 5, Tr("Загрузка общего мира...", "Loading the shared world while the player loads it"));
    if (!Determinism_BeginSession(true)) {
        SetSessionError(Tr("Слой детерминизма недоступен (см. лог)", "lockstep is not available - see the log (determinism layer)"));
        TwoStates_OnSessionEnded();
        ResetChannels();
        return false;
    }
    TwoStates_OnSessionJoined(true, mine.twoStatesMode, mine.borderHalfWidth, mine.expandBorders != 0,
                              mine.stateRub, mine.stateUsd);
    if (rejoin) {
        TwoStates_CapturePreResyncCamera();
    }
    TriggerLoadSave(liveSaveName);
    if (!WaitForLoadToFinish(0, &g_disconnectRequested) || !Determinism_SessionRunning()) {
        if (!g_disconnectRequested) {
            SetSessionError(Tr("Не удалось загрузить общий мир", "loading the shared world failed"));
        }
        TwoStates_OnSessionEnded();
        ResetChannels();
        return false;
    }
    return true;
}

static void CleanupHostStaging(const char* madeZip, const char* madeSaveDir,
                               const char* origSaveField, const char* origSaveDat)
{
    if (madeZip[0]) DeleteFileA(madeZip);
    if (madeSaveDir[0]) {
        KeepOnlyNewestHostSave(madeSaveDir);
        std::error_code rmEc;
        std::filesystem::remove_all(madeSaveDir, rmEc);
    }
    SafeRestoreHostWorldSaveNames(origSaveField, origSaveDat);
}

// One join, from the accepted connection to the lifted sync screen. The
// caller's session loop decides what happens next. Returns true when the
// player is in the game with all channels up.
static bool RunHostJoin(SOCKET sock, bool rejoin = false, bool* outFatalHostError = nullptr)
{
    if (outFatalHostError) *outFatalHostError = false;
    SetActiveSock(sock);
    SyncBlock_SetActive(true, g_resyncing
        ? Tr("Повторная синхронизация мира...", "Re-syncing the world")
        : Tr("Подключается второй игрок...", "A player is connecting"));
    EnsureGamePaused();
    bool joined = false;
    bool modHandshakeOk = false;
    bool fatalHostError = false;
    char madeSaveDir[MAX_PATH] = {};
    char madeZip[MAX_PATH] = {};
    char origSaveField[128] = {};
    char origSaveDat[128] = {};
    SafeSaveHostWorldSaveNames(origSaveField, sizeof(origSaveField), origSaveDat, sizeof(origSaveDat));

    do {
        SessionHello mine = {};
        const char* activeName = origSaveField[0] ? origSaveField : (origSaveDat[0] ? origSaveDat : nullptr);
        if (!HostPerformHandshake(sock, activeName, &mine)) break;
        modHandshakeOk = true;

        char liveSaveName[64];
        GenerateRandomSaveName(liveSaveName, sizeof(liveSaveName));
        if (!HostSaveAndPackWorld(sock, liveSaveName, madeSaveDir, sizeof(madeSaveDir), madeZip, sizeof(madeZip))) break;
        if (!HostSendWorldZip(sock, madeZip)) break;
        if (!HostLoadSharedWorld(mine, liveSaveName, rejoin)) {
            fatalHostError = true;
            break;
        }

        HostStep(sock, 5, Tr("Ожидание второго игрока — распаковка", "Waiting for the player - unpacking"), 0);
        bool ready = WaitForJoinControlMsg(
            sock, MSG_READY, true,
            Tr("Второй игрок отключился во время загрузки мира", "the player dropped while loading"),
            Tr("Второй игрок отменил подключение: %s", "the player gave up: %s"),
            Tr("Ожидание игрока — %s", "Waiting for the player - %s"));
        if (!ready) {
            if (!g_disconnectRequested) {
                SetState(SESSION_HOST_LISTENING);
                EnsureGamePaused();
            }
            if (g_lockstep) {
                ResetChannels();
            }
            break;
        }

        HostStep(sock, 7, Tr("Запуск синхронной симуляции Lockstep...", "Waiting for the player's lockstep connection"));
        if (!LockstepNet_Host(LOCKSTEP_CONNECT_TIMEOUT_MS, &g_disconnectRequested)) {
            if (!g_disconnectRequested) {
                SetSessionError(Tr("Соединение Lockstep от игрока не пришло — порт TCP %d должен быть открыт у хоста",
                                   "the player's lockstep connection didn't arrive - TCP port %d must reach the host"),
                                LOCKSTEP_PORT);
                SetState(SESSION_HOST_LISTENING);
                EnsureGamePaused();
            }
            ResetChannels();
            break;
        }
        joined = true;
    } while (false);

    if (!joined && modHandshakeOk) {
        ModSync_EndSession();
    }

    CleanupHostStaging(madeZip, madeSaveDir, origSaveField, origSaveDat);
    ClearAndCloseActiveSock(sock);
    if (!joined && !g_disconnectRequested && !fatalHostError) {
        SetState(SESSION_HOST_LISTENING);
        EnsureGamePaused();
    }
    if (outFatalHostError) *outFatalHostError = fatalHostError;
    SyncBlock_SetActive(false, nullptr);
    if (!joined && g_disconnectRequested) EnsureGameUnpaused();
    return joined;
}

static void RunHostSession()
{
    if (!NetworkInit()) {
        SetSessionError(Tr("Ошибка инициализации сети (Winsock)", "network init failed"));
        return;
    }

    bool rejoin = false; // a re-sync: the player is expected back right away
    while (!g_disconnectRequested && !g_processShuttingDown.load()) {
        const uint16_t listenPort = EffectiveSaveSyncPort();
        SetState(SESSION_HOST_LISTENING);
        EnsureGamePaused();
        if (g_disconnectRequested || g_processShuttingDown.load()) break;
        SteamP2P_SetHostingPresence(!rejoin);
        if (!rejoin) {
            char mapName[48] = {};
            GetHostDisplayMapName(mapName, sizeof(mapName));
            std::string hostName = LanDiscovery_GetLocalDisplayName();
            if (g_disconnectRequested || g_processShuttingDown.load()) break;
            LanDiscovery_SetHosting(true, listenPort, (uint8_t)TwoStates_GetMode(),
                                    hostName.c_str(), mapName);
            SetPeerIp("");
            SaveStatus("[HOST] hosting on port %u (%s) - waiting for a player",
                       static_cast<unsigned>(listenPort), SteamP2P_IsAvailable() ? "TCP + LAN + Steam P2P" : "TCP + LAN");
        } else {
            LanDiscovery_SetHosting(false, listenPort, 0, nullptr, nullptr);
        }

        // No accept timeout (cancellable) unless the player is coming back
        // from a re-sync; no I/O timeout - the player may take a while to
        // unpack and load.
        SOCKET sock = NetworkHostAcceptOne(listenPort, rejoin ? RESYNC_REJOIN_TIMEOUT_MS : 0, 0,
                                           &g_disconnectRequested);
        if (sock == INVALID_SOCKET) {
            if (g_disconnectRequested || g_processShuttingDown.load()) break;
            if (rejoin) {
                rejoin = false;
                g_resyncing = false;
                SetSessionError(Tr("Второй игрок не вернулся для пересинхронизации", "the player didn't come back for the re-sync"));
                SaveStatus("[HOST] the player didn't come back for the re-sync - waiting for a player again");
                continue;
            }
            SetSessionError(Tr("Не удалось открыть порт %u (возможно, уже занят)", "couldn't listen on port %u (already in use?)"),
                            static_cast<unsigned>(listenPort));
            break;
        }

        SteamP2P_SetHostingPresence(false);
        LanDiscovery_SetHosting(false, listenPort, 0, nullptr, nullptr);
        char peer[64];
        PeerIpOf(sock, peer, sizeof(peer));
        SetPeerIp(peer);
        SetState(SESSION_HOST_JOINING);
        SetSessionError("");
        SaveStatus(rejoin ? "[HOST] %s is back - re-syncing, both games are paused until it is loaded"
                          : "[HOST] %s is joining - both games are paused until it is loaded", peer);

        g_lockstep = g_lockstepChosen;
        bool fatalHostError = false;
        bool joined = RunHostJoin(sock, rejoin, &fatalHostError);
        rejoin = false;
        g_resyncing = false;
        if (!joined) {
            if (g_disconnectRequested || g_processShuttingDown.load() || fatalHostError) break;
            SaveStatus("[HOST] the join failed (%s) - waiting for a player again", SaveSync_GetLastError());
            continue;
        }
        g_lastWorldSentTick = GetTickCount64();
        g_resyncRequested = false; // one asked for during the join is already done

        g_hadPartnerInSession = true;
        SetLastPartnerPeer(peer);
        SetState(SESSION_HOST_IN_GAME);
        SaveStatus("[HOST] %s is in the game - live sync running, paused until either player resumes", peer);
        const char* why = MonitorLiveSession(false);
        if (!why || g_disconnectRequested) {
            ResetChannels();
            break; // Stop hosting
        }
        if (why == kResyncReason) {
            SetState(SESSION_HOST_LISTENING);
            EnsureGamePaused();
            ResetChannels();
            rejoin = true;
            SaveStatus("[HOST] re-syncing - waiting for %s to reconnect", peer);
            continue;
        }
        // Partner disconnected/crashed: transition straight to SESSION_HOST_LISTENING and pause
        // BEFORE tearing down Determinism so no unpaused simulation tick can occur.
        SetState(SESSION_HOST_LISTENING);
        EnsureGamePaused();
        ResetChannels();
        ModSync_EndSession();
        SetSessionError(Tr("Связь с %s прервана (%s) — игра на паузе, ждём переподключения",
                           "%s disconnected (%s) - game paused, waiting for reconnect"), peer, why);
        SaveStatus("[HOST] %s left (%s) - waiting for a player again", peer, why);
    }

    SteamP2P_SetHostingPresence(false);
    LanDiscovery_SetHosting(false, EffectiveSaveSyncPort(), 0, nullptr, nullptr);
    TwoStates_OnSessionEnded();
    g_hadPartnerInSession = false;
    SetLastPartnerPeer("");
    ResetChannels();
    SetPeerIp("");
    SaveStatus("[HOST] stopped hosting");
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

static void ClientStep(int step, const char* text, int percent = -1)
{
    SyncBlock_SetStep(step, JOIN_STEPS, text, percent);
    SaveStatus("[CLIENT] step %d/%d: %s", step, JOIN_STEPS, text);
}

// WaitForLoadToFinish abort check: the host sends nothing while the client
// loads, so anything readable here is the connection closing or failing.
static bool JoinHostGone(void* sockPtr)
{
    return NetworkPeerGone(*(SOCKET*)sockPtr);
}

static bool ClientPerformHandshake(SOCKET sock, SessionHello* outHostHello)
{
    if (!outHostHello) return false;
    SessionHello mine = {};
    MakeHello(&mine);
    if (!NetworkSendExact(sock, &mine, sizeof(mine)) || !NetworkRecvExact(sock, outHostHello, sizeof(*outHostHello))) {
        SetSessionError(Tr("Соединение с хостом прервалось при проверке версий", "the host dropped during the version check"));
        return false;
    }
    char why[256];
    if (!HelloMatches(*outHostHello, why, sizeof(why))) {
        SetSessionError(Tr("Вход отклонён: %s", "join refused: %s"), why);
        SaveStatus("[CLIENT] join refused: %s", why);
        return false;
    }
    SessionDlcs myDlcs, theirDlcs;
    MakeDlcs(&myDlcs);
    if (!NetworkSendExact(sock, &myDlcs, sizeof(myDlcs)) || !NetworkRecvExact(sock, &theirDlcs, sizeof(theirDlcs)) ||
        memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
        SetSessionError(Tr("Соединение с хостом прервалось при проверке DLC", "the host dropped during the DLC check"));
        return false;
    }
    char dlcWhy[512];
    if (!DlcsMatch(myDlcs, theirDlcs, false, dlcWhy, sizeof(dlcWhy))) {
        SetSessionError(Tr("Вход отклонён: %s", "join refused: %s"), dlcWhy);
        SaveStatus("[CLIENT] join refused: %s", dlcWhy);
        return false;
    }
    g_lockstep = outHostHello->lockstep != 0;
    if (g_lockstep) SaveStatus("[CLIENT] the host runs this session in lockstep");

    ClientStep(1, Tr("Синхронизация модов Мастерской Steam", "Synchronizing Steam Workshop mods"), 0);
    SessionMsg modMsg = {};
    if (!RecvMsg(sock, &modMsg) || modMsg.code != MSG_MOD_SYNC) {
        SetSessionError(Tr("Соединение с хостом прервалось при проверке модов", "the host dropped during the mod check"));
        return false;
    }
    char modWhy[256] = {};
    ModSyncClientResult clientRes = ModSync_ClientHandshake(sock, modWhy, sizeof(modWhy));
    if (clientRes == MOD_SYNC_CLIENT_MISSING) {
        char missingMsg[256] = {};
        _snprintf_s(missingMsg, sizeof(missingMsg), _TRUNCATE,
                    Tr("Требуется загрузка %s модов из Steam Workshop (см. окно загрузки)",
                       "Need to download %s missing mods from Steam Workshop (see download modal)"),
                    modWhy[0] ? modWhy : "N");
        SetSessionError("%s", missingMsg);
        SaveStatus("[CLIENT] %s", missingMsg);
        return false;
    }
    if (clientRes != MOD_SYNC_CLIENT_OK) {
        SetSessionError(Tr("Ошибка синхронизации модов: %s", "mod sync failed: %s"), modWhy);
        SaveStatus("[CLIENT] mod sync failed: %s", modWhy);
        return false;
    }
    return true;
}

static void WaitForWorldLoadToClear(DWORD timeoutMs = 30000)
{
    ULONGLONG start = GetTickCount64();
    while (SaveSync_IsWorldLoadInProgress() &&
           !g_processShuttingDown.load() &&
           (GetTickCount64() - start) < timeoutMs) {
        Sleep(50);
    }
}

// Feature 34: Client settings isolation
static void SaveSync_IsolateClientSettings(const char* destDir, bool rejoin)
{
    if (!destDir || !destDir[0]) return;
    std::error_code ec;
    std::filesystem::path dir(destDir);

    // 1. Always delete Host's windows.bin on Client so Host's open windows never appear.
    std::filesystem::path winBin = dir / "windows.bin";
    if (std::filesystem::exists(winBin, ec)) {
        std::filesystem::remove(winBin, ec);
        if (H && H->log) {
            Logf("republicmp  CLIENT_SETTINGS: removed host windows.bin from incoming save (rejoin=%d)", rejoin ? 1 : 0);
        }
    }

    // 2. Manage toolco.bin (Construction Office quick groups Ctrl+1..4)
    std::filesystem::path toolcoBin = dir / "toolco.bin";
    if (!rejoin) {
        // Initial join: delete host toolco.bin so client starts with clean empty presets
        if (std::filesystem::exists(toolcoBin, ec)) {
            std::filesystem::remove(toolcoBin, ec);
            if (H && H->log) {
                Logf("republicmp  CLIENT_SETTINGS: removed host toolco.bin from incoming initial save");
            }
        }
    } else {
        // Re-sync: restore client's existing toolco.bin if backed up
        char backupDir[MAX_PATH] = {};
        if (GetGameSaveDir(backupDir, sizeof(backupDir), "coop_client_backup")) {
            std::filesystem::path clientToolco = std::filesystem::path(backupDir) / "toolco.bin";
            if (std::filesystem::exists(clientToolco, ec)) {
                std::filesystem::copy_file(clientToolco, toolcoBin, std::filesystem::copy_options::overwrite_existing, ec);
                if (H && H->log) {
                    Logf("republicmp  CLIENT_SETTINGS: restored client toolco.bin from backup on resync");
                }
            } else if (std::filesystem::exists(toolcoBin, ec)) {
                std::filesystem::remove(toolcoBin, ec);
            }
        }
    }
}

// Feature 32: Retain client backup
static bool SaveSync_PreserveClientBackup(const char* destDir)
{
    if (!destDir || !destDir[0]) return false;
    char backupDir[MAX_PATH] = {};
    if (!GetGameSaveDir(backupDir, sizeof(backupDir), "coop_client_backup")) return false;

    std::error_code ec;
    std::filesystem::path src(destDir);
    std::filesystem::path dst(backupDir);
    std::filesystem::create_directories(dst, ec);
    std::filesystem::copy(src, dst,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (!ec) {
        if (H && H->log) {
            Logf("republicmp  SAVE_SYNC: client backup copy preserved at [%s]", backupDir);
        }
        return true;
    }
    if (H && H->log) {
        Logf("republicmp  SAVE_SYNC: warning: failed to preserve client backup copy: %s", ec.message().c_str());
    }
    return false;
}

void SaveSync_IsolateClientSettingsForTest(const char* destDir, bool rejoin)
{
    SaveSync_IsolateClientSettings(destDir, rejoin);
}

bool SaveSync_PreserveClientBackupForTest(const char* destDir)
{
    return SaveSync_PreserveClientBackup(destDir);
}

void SaveSync_FormatDisplayTarget(const char* rawTarget, unsigned short defaultPort,
                                  char* outDisplay, size_t outCap, bool* outIsSteam)
{
    if (outIsSteam) *outIsSteam = false;
    if (!outDisplay || outCap == 0) return;
    outDisplay[0] = '\0';

    char normHost[128] = {};
    uint16_t parsedPort = 0;
    if (NetworkNormalizeTargetAddress(rawTarget, normHost, sizeof(normHost), &parsedPort) && normHost[0]) {
        const uint64_t sid = SteamP2P_ParseTargetSteamId(normHost);
        if (sid != 0) {
            if (outIsSteam) *outIsSteam = true;
            _snprintf_s(outDisplay, outCap, _TRUNCATE, "steam:%llu", (unsigned long long)sid);
            return;
        }
        const unsigned short effPort = (parsedPort != 0) ? parsedPort : defaultPort;
        _snprintf_s(outDisplay, outCap, _TRUNCATE, "%s:%u", normHost, (unsigned)effPort);
        return;
    }

    strncpy_s(outDisplay, outCap, rawTarget ? rawTarget : "", _TRUNCATE);
}

static SOCKET ClientConnectJoinSocket(bool rejoin)
{
    ClientStep(1, Tr("Подключение к серверу — проверка версий", "Connecting to the host - checking versions"));
    if (!NetworkInit()) {
        SetSessionError(Tr("Ошибка инициализации сети", "network init failed"));
        return INVALID_SOCKET;
    }
    char targetCopy[64] = {};
    ReadProtectedString(g_targetIp, targetCopy, sizeof(targetCopy));

    const uint16_t defaultPort = EffectiveSaveSyncPort();
    ULONGLONG connectStart = GetTickCount64();
    SOCKET sock = INVALID_SOCKET;
    for (;;) {
        sock = NetworkConnect(targetCopy, defaultPort, 30000, 0, &g_disconnectRequested);
        if (sock != INVALID_SOCKET || !rejoin || g_disconnectRequested) break;
        if (GetTickCount64() - connectStart > RESYNC_REJOIN_TIMEOUT_MS) break;
        Sleep(RESYNC_RETRY_MS);
    }
    if (sock == INVALID_SOCKET && !g_disconnectRequested) {
        char dispTarget[96] = {};
        bool isSteamTarget = false;
        SaveSync_FormatDisplayTarget(targetCopy, defaultPort, dispTarget, sizeof(dispTarget), &isSteamTarget);
        if (isSteamTarget && !SteamP2P_IsAvailable()) {
            SetSessionError(Tr("Steam не в сети — не удалось подключиться к %s. Используйте LAN IP хоста",
                               "Steam is offline — couldn't reach %s. Use the host's LAN IP instead"),
                            dispTarget);
        } else {
            SetSessionError(Tr("Не удалось подключиться к %s — проверьте, что хост уже открыл сервер",
                               "couldn't reach %s - is the host hosting (and not already playing with someone)?"),
                            dispTarget);
        }
    }
    return sock;
}

static bool ClientReceiveAndUnpackWorld(SOCKET sock, const char* zipPath, const char* destDir)
{
    {
        SaveTransferGuard transferGuard;
        AckPacedTransferGuard ackGuard;
        ClientStep(4, Tr("Приём мира от хоста", "Receiving the world"), 0);
        // Do NOT send MSG_STEP control frames while the socket carries raw payload + uint64 ACKs.
        ProgressCtx recvCtx = { INVALID_SOCKET, 4, Tr("приём мира", "receiving the world"), 0 };
        g_transferTotalBytes = 0;
        SaveSync_SpeedMeterReset(&g_transferMeter, GetTickCount64());
        if (!NetworkReceiveFileAckPaced(sock, zipPath, NetworkProgressCb, &recvCtx, &g_disconnectRequested)) {
            SetSessionError(Tr("Ошибка приёма мира — соединение потеряно", "receiving the world failed - connection lost"));
            return false;
        }
    }

    ClientStep(5, Tr("Распаковка мира", "Unpacking"), 0);
    SendMsg(sock, MSG_STEP, 5, 0, Tr("распаковка мира", "unpacking"));
    {
        std::error_code rmEc;
        std::filesystem::remove_all(destDir, rmEc);
    }
    ProgressCtx unpackCtx = { sock, 5, Tr("распаковка мира", "unpacking"), -100 };
    bool unzippedOk = UnzipToDirectory(zipPath, destDir, ZipProgressCb, &unpackCtx);
    DeleteFileA(zipPath);
    if (!unzippedOk) {
        SetSessionError(Tr("Ошибка распаковки мира", "unpacking the world failed"));
        SendMsg(sock, MSG_ABORT, 0, -1, "unpacking failed");
        return false;
    }
    if (g_disconnectRequested) {
        SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled");
        return false;
    }
    return true;
}

static bool ClientPrepareAndLoadWorld(SOCKET sock, const SessionHello& theirs, const char* destDir, bool rejoin)
{
    SaveSync_IsolateClientSettings(destDir, rejoin);
    RoadSync_ClearPlannedSegments();

    BuildMode preResyncBuildMode = {};
    BYTE preResyncAutoPopulate = 0;
    if (rejoin) {
        TwoStates_CapturePreResyncCamera();
        preResyncBuildMode = BuildMode_Read();
        BYTE* w = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
        if (w) preResyncAutoPopulate = *(w + 0xB30);
    }

    const char* logReason = rejoin ? "RESYNC" : (DiagLog_HasPrevCrashOrDesyncLog() ? "PREV_CRASH" : "CLIENT_JOIN");
    SendJoinDiagLogBundleIfEnabled(sock, logReason);

    ClientStep(6, Tr("Загрузка мира...", "Loading the world"));
    SendMsg(sock, MSG_STEP, 6, -1, Tr("загрузка мира", "loading the world"));
    char receivedSaveName[MAX_PATH] = {};
    _snprintf_s(receivedSaveName, sizeof(receivedSaveName), _TRUNCATE, "%s%s",
                SAVE_SYNC_CLIENT_STAGING_NAME, SAVE_SYNC_RECEIVED_SUFFIX);
    if (g_lockstep) {
        if (!Determinism_BeginSession(false)) {
            SetSessionError(Tr("Слой детерминизма недоступен (см. лог)", "lockstep is not available - see the log (determinism layer)"));
            SendMsg(sock, MSG_ABORT, 0, -1, "lockstep is not available on the player");
            return false;
        }
        TwoStates_OnSessionJoined(false, theirs.twoStatesMode, theirs.borderHalfWidth, theirs.expandBorders != 0,
                                  theirs.stateRub, theirs.stateUsd);
    }
    TriggerLoadSave(receivedSaveName);
    bool loadFinished = WaitForLoadToFinish(0, &g_disconnectRequested, JoinHostGone, &sock);
    WaitForWorldLoadToClear();

    if (loadFinished) {
        BYTE* w = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
        if (rejoin) {
            BuildMode_Write(preResyncBuildMode);
            if (w) *(w + 0xB30) = preResyncAutoPopulate;
            Logf("republicmp  CLIENT_SETTINGS: restored client BuildMode & autoPopulate after resync");
        } else {
            BuildMode defMode = { 0, 0, 0, 0 };
            BuildMode_Write(defMode);
        }
    }

    SaveSync_PreserveClientBackup(destDir);
    {
        std::error_code rmEc;
        std::filesystem::remove_all(destDir, rmEc);
    }
    if (!loadFinished) {
        if (g_disconnectRequested) SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled while loading");
        else SetSessionError(Tr("Хост отключился во время загрузки мира", "the host dropped while the world was loading"));
        if (g_lockstep) TwoStates_OnSessionEnded();
        return false;
    }
    return true;
}

static bool ClientConnectLockstep(SOCKET sock)
{
    ClientStep(7, Tr("Ожидание загрузки мира у хоста...", "Waiting for the host to load the shared world"));
    SendMsg(sock, MSG_READY, 7, -1, "");
    if (!Determinism_SessionRunning()) {
        SetSessionError(Tr("Ошибка загрузки общего мира", "loading the shared world failed"));
        TwoStates_OnSessionEnded();
        return false;
    }

    char targetCopy[64] = {};
    ReadProtectedString(g_targetIp, targetCopy, sizeof(targetCopy));
    char lockstepHost[64] = {};
    if (!NetworkNormalizeTargetAddress(targetCopy, lockstepHost, sizeof(lockstepHost), nullptr) || !lockstepHost[0]) {
        strncpy_s(lockstepHost, sizeof(lockstepHost), targetCopy, _TRUNCATE);
    }

    ULONGLONG joinStart = GetTickCount64();
    bool lockstepUp = false, hostGone = false;
    while (!g_disconnectRequested && GetTickCount64() - joinStart < LOCKSTEP_CONNECT_TIMEOUT_MS) {
        if (LockstepNet_Join(lockstepHost, 6000, &g_disconnectRequested)) {
            lockstepUp = true;
            break;
        }
        if (JoinHostGone(&sock)) {
            hostGone = true;
            break;
        }
    }
    if (!lockstepUp) {
        if (hostGone) SetSessionError(Tr("Хост отключился во время загрузки общего мира", "the host dropped while loading the shared world"));
        else if (!g_disconnectRequested) SetSessionError(Tr("Не удалось открыть Lockstep-канал к %s:%d", "couldn't open the lockstep connection to %s:%d"), lockstepHost, LOCKSTEP_PORT);
        TwoStates_OnSessionEnded();
        return false;
    }
    return true;
}

// Returns true when the player is in the host's game with all channels up.
// rejoin: coming back for a re-sync - the host may need a moment to listen
// again, so refused connects are retried.
static bool RunClientJoin(bool rejoin, bool holdPauseOnFailure = false)
{
    g_resyncAnnounced = false;
    SyncBlock_SetActive(true, rejoin
        ? Tr("Повторная синхронизация с хостом...", "Re-syncing with the host")
        : Tr("Подключение к хосту...", "Connecting to the host"));
    EnsureGamePaused();

    SOCKET sock = INVALID_SOCKET;
    bool joined = false;

    do {
        char saveDir[MAX_PATH] = {};
        if (!GetGameSaveDir(saveDir, sizeof(saveDir), SAVE_SYNC_CLIENT_STAGING_NAME)) {
            SetSessionError(Tr("Не удалось сформировать путь сохранения", "couldn't build the save path"));
            break;
        }
        char zipPath[MAX_PATH] = {};
        _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
        char destDir[MAX_PATH] = {};
        _snprintf_s(destDir, sizeof(destDir), _TRUNCATE, "%s%s", saveDir, SAVE_SYNC_RECEIVED_SUFFIX);
        std::error_code parentEc;
        std::filesystem::create_directories(std::filesystem::path(zipPath).parent_path(), parentEc);

        sock = ClientConnectJoinSocket(rejoin);
        if (sock == INVALID_SOCKET) break;
        SetActiveSock(sock);
        if (g_disconnectRequested) break;

        SessionHello theirs = {};
        if (!ClientPerformHandshake(sock, &theirs)) break;

        ClientStep(2, Tr("Ожидание хоста — сохранение мира", "Waiting for the host - saving the world"));
        if (!WaitForJoinControlMsg(
                sock, MSG_FILE_FOLLOWS, false,
                Tr("Хост отключился во время подготовки мира", "the host dropped while preparing the world"),
                Tr("Хост прервал подключение: %s", "the host gave up: %s"),
                Tr("Ожидание хоста — %s", "Waiting for the host - %s"))) {
            break;
        }

        if (!ClientReceiveAndUnpackWorld(sock, zipPath, destDir)) break;
        if (!ClientPrepareAndLoadWorld(sock, theirs, destDir, rejoin)) break;
        if (!ClientConnectLockstep(sock)) break;
        joined = true;
    } while (false);

    ClearAndCloseActiveSock(sock);
    SyncBlock_SetActive(false, nullptr);
    if (!joined && !holdPauseOnFailure) EnsureGameUnpaused();
    return joined;
}

static void RunClientSession()
{
    ClientHostLostInfo priorHostLost = {};
    bool fromHostLost = ConsumeHostLostReconnectSnapshot(&priorHostLost);
    char targetCopy[64] = {};
    ReadProtectedString(g_targetIp, targetCopy, sizeof(targetCopy));
    SetPeerIp(targetCopy);
    bool rejoin = false;
    for (;;) {
        SetState(SESSION_CLIENT_JOINING);
        const bool holdPauseOnFail = SaveSync_ShouldRestoreClientHostLostOnJoinFailure(
            fromHostLost, SaveSync_IsWorldLoaded(), g_processShuttingDown.load());
        bool joined = RunClientJoin(rejoin, holdPauseOnFail);
        g_resyncing = false;
        if (!joined) {
            TwoStates_OnSessionEnded();
            ResetChannels();
            if (g_disconnectRequested) SaveStatus("[CLIENT] cancelled");
            else SaveStatus("[CLIENT] couldn't join: %s", SaveSync_GetLastError());
            if (SaveSync_ShouldRestoreClientHostLostOnJoinFailure(
                    fromHostLost, SaveSync_IsWorldLoaded(), g_processShuttingDown.load())) {
                const char* overrideReason = g_disconnectRequested
                    ? Tr("Повторное подключение отменено — сервер ещё не готов",
                         "Reconnect cancelled — host server is not ready yet")
                    : nullptr;
                RestoreHostLostAfterFailedReconnect(priorHostLost, targetCopy, overrideReason);
                SaveStatus("[CLIENT] reconnect failed - keeping game paused & showing Host-Lost recovery prompt");
            }
            return;
        }
        fromHostLost = false;

        SetState(SESSION_CLIENT_IN_GAME);
        SaveStatus("[CLIENT] in the game at %s - live sync running, paused until either player resumes", targetCopy);
        const char* why = MonitorLiveSession(true);
        char hostLabel[128] = {};
        ReadProtectedString(g_lastPartnerPeer, hostLabel, sizeof(hostLabel));
        if (!hostLabel[0]) {
            SaveSync_FormatDisplayTarget(targetCopy, EffectiveSaveSyncPort(), hostLabel, sizeof(hostLabel), nullptr);
        }
        ResetChannels();
        if (why == kResyncReason) {
            rejoin = true;
            SaveStatus("[CLIENT] the host is re-syncing the world - reconnecting");
            continue;
        }
        const bool unexpectedHostLoss = (why != nullptr && !g_disconnectRequested && SaveSync_IsWorldLoaded());
        TwoStates_OnSessionEnded();
        if (why) {
            SetSessionError(Tr("Отключено от хоста: %s", "disconnected from the host: %s"), why);
            SaveStatus("[CLIENT] disconnected from the host (%s)", why);
        } else {
            SaveStatus("[CLIENT] left the session");
        }
        if (unexpectedHostLoss) {
            RecordClientHostLost(targetCopy, hostLabel, why);
            SaveSync_EnforceWaitingLobbyHold();
            SaveStatus("[CLIENT] connection lost - paused game & showing recovery prompt");
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

static DWORD WINAPI HostSessionThread(LPVOID)
{
    RunHostSession();
    WaitForWorldLoadToClear();
    SetState(SESSION_IDLE);
    return 0;
}

static DWORD WINAPI ClientSessionThread(LPVOID)
{
    RunClientSession();
    SetPeerIp("");
    WaitForWorldLoadToClear();
    SetState(SESSION_IDLE);
    return 0;
}

// The world's road network (world+0xf7a0): made with every world, an empty
// map too; deleted and zeroed when the world is torn down (0x9952C,
// 0x29564E) - back in the main menu, or while another save loads.
#define WORLD_ROAD_NETWORK_RVA 0xA2E758
// Out of the world this long in a session ends it (a load's teardown is
// shorter, and joins/re-syncs are not checked at all).
#define OUT_OF_WORLD_GRACE_MS 1500

bool SaveSync_IsWorldLoaded()
{
    if (Determinism_WorldLoading()) return false;
    bool loaded = false;
    DiagLog_BeginSafeSeh();
    __try {
        loaded = *(__int64*)((uintptr_t)g_exe + WORLD_ROAD_NETWORK_RVA) != 0 &&
                 *(volatile char*)((uintptr_t)g_exe + LOAD_TRIGGER_DAT_RVA) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        loaded = false;
    }
    DiagLog_EndSafeSeh();
    return loaded;
}

bool SaveSync_IsWorldLoadInProgress()
{
    if (g_testWorldLoadInProgress.load(std::memory_order_relaxed)) return true;
    char trigger = 0;
    bool triggerPending = false;
    if (g_exe && SafeReadPod((const unsigned char*)g_exe + LOAD_TRIGGER_DAT_RVA, &trigger) && trigger != 0) {
        triggerPending = true;
    }
    return triggerPending || Determinism_WorldLoading() || g_inSaveTransfer.load(std::memory_order_relaxed);
}

void SaveSync_SetTestWorldLoadInProgress(bool inProgress)
{
    g_testWorldLoadInProgress.store(inProgress, std::memory_order_relaxed);
}

static bool SafeGetCurrentWorldSaveRel(char* outRel, size_t outSize)
{
    if (!g_exe || !outRel || outSize == 0) return false;
    outRel[0] = '\0';
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        const char* curRel = (const char*)((uintptr_t)g_exe + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET);
        if (curRel && (_strnicmp(curRel, "save/", 5) == 0 || _strnicmp(curRel, "save\\", 5) == 0) && curRel[5]) {
            strncpy_s(outRel, outSize, curRel, _TRUNCATE);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void SaveSync_StartHost()
{
    if (g_processShuttingDown.load() || g_state != SESSION_IDLE) return;
    if (!SaveSync_IsWorldLoaded()) {
        // A joining player gets the host's world: there has to be one.
        SaveStatus("[HOST] no world loaded - load a city first, then host");
        SetSessionError(Tr("Сначала загрузите город или карту перед созданием сервера", "load a city before hosting"));
        return;
    }
    if (!TwoStates_IsCoopBankMounted()) {
        // Started server from an in-game Singleplayer session: copy current save folder (if any) to Co-op bank first
        char curRel[128] = {};
        if (SafeGetCurrentWorldSaveRel(curRel, sizeof(curRel))) {
            char spDir[MAX_PATH] = {};
            if (GetGameSaveDir(spDir, sizeof(spDir), curRel + 5)) {
                std::error_code ec;
                std::filesystem::path src(spDir);
                std::filesystem::path dst = src.parent_path().parent_path() / "save_coop" / (curRel + 5);
                if (std::filesystem::exists(src, ec) && std::filesystem::is_directory(src, ec)) {
                    std::filesystem::create_directories(dst.parent_path(), ec);
                    std::filesystem::copy(src, dst,
                                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                          ec);
                }
            }
        }
        TwoStates_MountSaveBank(true);
    }
    if (TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
        g_lockstepChosen = true;
    }
    ClearClientHostLost();
    ClearHostLostReconnectState();
    g_disconnectRequested = false;
    g_isHost = true;
    g_hadPartnerInSession = false;
    SetLastPartnerPeer("");
    g_resyncRequested = false;
    g_resyncing = false;
    g_sessionStartTick = GetTickCount64();
    SetSessionError("");
    SetState(SESSION_HOST_LISTENING);
    SaveSync_EnforceWaitingLobbyHold();
    TwoStates_CaptureLobbyBaseMoney();
    double initRub[2] = {}, initUsd[2] = {};
    TwoStates_PrepareHostSave(false, initRub, initUsd);
    TwoStates_OnSessionJoined(true, TwoStates_GetMode(), TwoStates_GetBorderHalfWidth(), TwoStates_GetExpandBorders(),
                              initRub, initUsd);
    JoinAndCloseSessionThread(0);
    HANDLE hThread = CreateThread(nullptr, 0, HostSessionThread, nullptr, 0, nullptr);
    if (hThread) {
        TsmEnsureCriticalSection(&g_sessionThreadLockOnce, &g_sessionThreadLock);
        EnterCriticalSection(&g_sessionThreadLock);
        g_sessionThread = hThread;
        LeaveCriticalSection(&g_sessionThreadLock);
    } else {
        TwoStates_OnSessionEnded();
        SetState(SESSION_IDLE);
        SetSessionError(Tr("Не удалось запустить поток сервера", "failed to start host session thread"));
    }
}

static bool StartClientInternal(const char* hostIp, bool fromHostLost, const ClientHostLostInfo* priorHostLost)
{
    if (g_processShuttingDown.load() || g_state != SESSION_IDLE) return false;
    if (fromHostLost && priorHostLost) {
        SetHostLostReconnectPending(*priorHostLost);
    } else {
        ClearClientHostLost();
        ClearHostLostReconnectState();
    }
    TwoStates_MountSaveBank(true);
    g_disconnectRequested = false;
    if (hostIp && hostIp[0]) {
        WriteProtectedString(g_targetIp, sizeof(g_targetIp), hostIp);
        char targetCopy[64] = {};
        ReadProtectedString(g_targetIp, targetCopy, sizeof(targetCopy));
        TwoStates_SetSavedJoinAddress(targetCopy);
        TwoStates_SaveSettings();
    }
    g_isHost = false;
    g_hadPartnerInSession = false;
    SetLastPartnerPeer("");
    g_resyncing = false;
    g_sessionStartTick = GetTickCount64();
    SetSessionError("");
    SetState(SESSION_CLIENT_JOINING);
    JoinAndCloseSessionThread(0);
    HANDLE hThread = CreateThread(nullptr, 0, ClientSessionThread, nullptr, 0, nullptr);
    if (hThread) {
        TsmEnsureCriticalSection(&g_sessionThreadLockOnce, &g_sessionThreadLock);
        EnterCriticalSection(&g_sessionThreadLock);
        g_sessionThread = hThread;
        LeaveCriticalSection(&g_sessionThreadLock);
        return true;
    }
    TwoStates_OnSessionEnded();
    SetSessionError(Tr("Не удалось запустить поток подключения", "failed to start client session thread"));
    if (SaveSync_ShouldRestoreClientHostLostOnJoinFailure(
            fromHostLost, SaveSync_IsWorldLoaded(), g_processShuttingDown.load()) && priorHostLost) {
        ClearHostLostReconnectState();
        RestoreHostLostAfterFailedReconnect(*priorHostLost, hostIp, nullptr);
    }
    SetState(SESSION_IDLE);
    return false;
}

void SaveSync_StartClient(const char* hostIp)
{
    (void)StartClientInternal(hostIp, false, nullptr);
}

void SaveSync_RequestDisconnect()
{
    ClearClientHostLost();
    if (g_state == SESSION_IDLE) {
        // Channels opened by hand from the Debug window.
        ResetChannels();
        return;
    }
    SaveStatus(g_isHost ? "[HOST] stopping..." : "[CLIENT] disconnecting...");
    TsmEnsureCriticalSection(&g_activeSockLockOnce, &g_activeSockLock);
    EnterCriticalSection(&g_activeSockLock);
    g_disconnectRequested = true;
    if (g_activeSock != INVALID_SOCKET) {
        NetworkShutdownSocket(g_activeSock);
    }
    LeaveCriticalSection(&g_activeSockLock);
    ResetChannels();
}

void SaveSync_Shutdown(DWORD waitMs)
{
    ClearClientHostLost();
    ClearHostLostReconnectState();
    g_processShuttingDown.store(true);
    SaveSync_RequestDisconnect();
    JoinAndCloseSessionThread(waitMs);
}

void SaveSync_RequestResync()
{
    if (g_isHost && g_state == SESSION_HOST_IN_GAME) g_resyncRequested = true;
}

void SaveSync_SetDesyncResyncSeconds(int seconds) { g_desyncResyncSeconds = seconds < 0 ? -1 : seconds; }
int SaveSync_GetDesyncResyncSeconds() { return g_desyncResyncSeconds; }

ULONGLONG SaveSync_GetMsUntilDesyncResync()
{
    int delay = g_desyncResyncSeconds;
    ULONGLONG since = g_desyncSince;
    if (!g_isHost || !since || delay < 0 || !LockstepNet_Desync()[0]) return 0;
    ULONGLONG elapsed = GetTickCount64() - since, wait = (ULONGLONG)delay * 1000;
    return elapsed >= wait ? 1 : wait - elapsed;
}

void SaveSync_SetAutoResyncMinutes(int minutes)
{
    g_autoResyncMinutes = minutes > 0 ? minutes : 0;
}

int SaveSync_GetAutoResyncMinutes() { return g_autoResyncMinutes; }

ULONGLONG SaveSync_GetMsUntilAutoResync()
{
    int minutes = g_autoResyncMinutes;
    if (!g_isHost || g_state != SESSION_HOST_IN_GAME || minutes <= 0) return 0;
    ULONGLONG interval = (ULONGLONG)minutes * 60000;
    ULONGLONG elapsed = GetTickCount64() - g_lastWorldSentTick;
    return elapsed >= interval ? 1 : interval - elapsed;
}

ULONGLONG SaveSync_GetMsSinceWorldSent()
{
    if (!g_isHost || g_state != SESSION_HOST_IN_GAME) return 0;
    return GetTickCount64() - g_lastWorldSentTick;
}

bool SaveSync_IsResyncing() { return g_resyncing; }

void SaveSync_OnResyncAnnounced()
{
    g_resyncing = true;
    g_resyncAnnounced = true;
}

// Leaving the world (main menu, another save) ends the session: the other
// player would otherwise stay connected to a game that is gone. Joins and
// re-syncs load a world themselves and are not checked.
static void CheckStillInWorld()
{
    static ULONGLONG s_outSince = 0;
    SessionState st = g_state;
    bool watched = st == SESSION_HOST_LISTENING || st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME;
    if (!watched || g_disconnectRequested || SaveSync_IsWorldLoaded()) {
        s_outSince = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!s_outSince) {
        s_outSince = now;
        return;
    }
    if (now - s_outSince < OUT_OF_WORLD_GRACE_MS) return;
    s_outSince = 0;
    bool host = g_isHost;
    SaveStatus(host ? "[HOST] left the world - stopping hosting" : "[CLIENT] left the world - disconnecting");
    SaveSync_RequestDisconnect();
    SetSessionError(host ? Tr("Сервер остановлен: выход из мира", "hosting stopped: you left the world")
                         : Tr("Отключено: выход из мира", "disconnected: you left the world"));
}

bool SaveSync_IsClientHostLost()
{
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    const bool active = g_clientHostLost.active;
    LeaveCriticalSection(&g_saveSyncStrLock);
    return active;
}

bool SaveSync_GetClientHostLostInfo(ClientHostLostInfo* out)
{
    if (!out) return false;
    TsmEnsureCriticalSection(&g_saveSyncStrLockOnce, &g_saveSyncStrLock);
    EnterCriticalSection(&g_saveSyncStrLock);
    *out = g_clientHostLost;
    const bool active = g_clientHostLost.active;
    LeaveCriticalSection(&g_saveSyncStrLock);
    return active;
}

void SaveSync_DismissClientHostLost()
{
    ClearClientHostLost();
    ClearHostLostReconnectState();
}

bool SaveSync_ShouldRestoreClientHostLostOnJoinFailure(bool fromHostLost, bool worldLoaded, bool shuttingDown)
{
    return fromHostLost && worldLoaded && !shuttingDown;
}

bool SaveSync_ReconnectAfterHostLost()
{
    if (g_processShuttingDown.load() || g_state != SESSION_IDLE) return false;
    ClientHostLostInfo snap = {};
    if (!SaveSync_GetClientHostLostInfo(&snap) || !snap.active) return false;
    return StartClientInternal(snap.hostAddress[0] ? snap.hostAddress : nullptr, true, &snap);
}

bool SaveSync_ShouldShowClientHostLostModal(int sessionState, bool worldLoaded, bool hostLostFlag)
{
    return sessionState == static_cast<int>(SESSION_IDLE) && worldLoaded && hostLostFlag;
}

bool SaveSync_ExitToMainMenu(unsigned char* exeBase)
{
    ClearClientHostLost();
    ClearHostLostReconnectState();
    if (g_state != SESSION_IDLE) {
        SaveSync_RequestDisconnect();
    }
    BYTE* base = exeBase ? exeBase : reinterpret_cast<BYTE*>(g_exe);
    if (!base) return false;
    Determinism_ClearFillingWorkQueues(base);
    const uint8_t zero = 0;
    const uint8_t one = 1;
    (void)SafeWritePod(base + WORLD_RVA + WORLD_EXIT_DESKTOP_FLAG_OFF, zero);
    if (!SafeWritePod(base + EXIT_TO_MENU_FLAG_RVA, one)) {
        return false;
    }
    SaveStatus("[CLIENT] returning to main menu");
    return true;
}

void SaveSync_SetClientHostLostForTest(bool active, const char* hostAddr,
                                       const char* hostLabel, const char* reason)
{
    if (!active) {
        ClearClientHostLost();
        return;
    }
    RecordClientHostLost(hostAddr, hostLabel, reason);
}

void SaveSync_Tick()
{
    SteamP2P_FrameTick();
    DiagLog_FrameTick();
    ModSync_TickMainThread();
    const bool worldLoaded = SaveSync_IsWorldLoaded();
    if (!worldLoaded && SaveSync_IsClientHostLost()) {
        ClearClientHostLost();
    }
    if (SaveSync_IsWaitingLobbyActive() ||
        (NativeMenu_IsHostOnNextLoad() && worldLoaded) ||
        SaveSync_ShouldShowClientHostLostModal(static_cast<int>(g_state), worldLoaded, SaveSync_IsClientHostLost())) {
        SaveSync_EnforceWaitingLobbyHold();
    }
    if (g_state == SESSION_IDLE && (NativeMenu_GetActiveScreenId() != 0 || SaveSync_IsWorldLoaded())) {
        char steamJoinTarget[64] = {};
        if (SteamP2P_ConsumePendingJoin(steamJoinTarget, sizeof(steamJoinTarget))) {
            SaveSync_StartClient(steamJoinTarget);
        }
    }
    CheckStillInWorld();
    if (!g_resyncSendNow || g_resyncSent) return;
    if (!BuildSync_IsConnected()) return; // AnnounceResync gives up after its timeout
    BuildSync_SendResync();
    if (BuildSync_IsConnected()) g_resyncSent = true;
}

SessionState SaveSync_GetState() { return g_state; }
void SaveSync_SetLockstep(bool on) { (void)on; g_lockstepChosen = true; g_lockstep = true; }
void SaveSync_SetDlcCheck(bool on) { g_dlcCheck = on; }
bool SaveSync_GetDlcCheck() { return g_dlcCheck; }
bool SaveSync_GetLockstep() { return true; }
bool SaveSync_IsLockstepSession() { return g_state != SESSION_IDLE; }
bool SaveSync_IsBusy()           { return g_state != SESSION_IDLE; }
bool SaveSync_IsHostRole()       { return g_isHost; }
ULONGLONG SaveSync_GetSessionAgeMs() { return g_state == SESSION_IDLE ? 0 : GetTickCount64() - g_sessionStartTick; }
bool SaveSync_IsHostAwaitingClient() { return g_state == SESSION_HOST_LISTENING; }
const char* SaveSync_GetTargetIp()
{
    static thread_local char tlsTargetIp[64] = {};
    ReadProtectedString(g_targetIp, tlsTargetIp, sizeof(tlsTargetIp));
    return tlsTargetIp;
}

bool SaveSync_ShouldBlockWorldEdits(int sessionState, bool inGameplay, bool syncBlockActive)
{
    if (!inGameplay) return false;
    if (syncBlockActive) return true;
    return sessionState == SESSION_HOST_LISTENING ||
           sessionState == SESSION_HOST_JOINING ||
           sessionState == SESSION_CLIENT_JOINING;
}

bool SaveSync_IsWaitingLobbyActive()
{
    return SaveSync_ShouldBlockWorldEdits((int)g_state, SaveSync_IsWorldLoaded(), SyncBlock_IsActive());
}

bool SaveSync_IsWaitingForReconnect()
{
    return g_state == SESSION_HOST_LISTENING && g_hadPartnerInSession;
}

bool SaveSync_HadPartnerInSession()
{
    return g_hadPartnerInSession;
}

const char* SaveSync_GetLastPartnerPeer()
{
    static thread_local char tlsPartner[64] = {};
    ReadProtectedString(g_lastPartnerPeer, tlsPartner, sizeof(tlsPartner));
    return tlsPartner;
}

void SaveSync_EnforceWaitingLobbyHold(unsigned char* exeBase)
{
    unsigned char* base = exeBase ? exeBase : (unsigned char*)g_exe;
    if (!base) return;

    // 1. Force game speed = 0 (paused), preserving any positive speed in GAME_SPEED_SAVED_OFFSET (+0x5A8),
    //    and force turbo = 0, hold = 0, timer paused byte (+12) = 1, and raw LMB bytes (0xA54E90/0xA54E91) = 0.
    int curSpeed = 0;
    if (SafeReadPod<int>(base + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET, &curSpeed) && curSpeed != 0) {
        if (curSpeed > 0) {
            SafeWritePod<int>(base + GAME_SPEED_DAT_RVA + GAME_SPEED_SAVED_OFFSET, curSpeed);
        }
        SafeWritePod<int>(base + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET, 0);
    }
    SafeWritePod<int>(base + GAME_SPEED_DAT_RVA + GAME_SPEED_TURBO_OFFSET, 0);
    SafeWritePod<unsigned char>(base + GAME_SPEED_DAT_RVA + GAME_SPEED_HOLD_OFFSET, 0);
    SafeWritePod<unsigned char>(base + 0xA1EEF0 + 12, 1);
    SafeWritePod<unsigned char>(base + 0xA9EF50, 0);
    SafeWritePod<unsigned char>(base + 0xA9EF51, 0);

    // 2. Deselect any active building/terrain/demolish/road tool and cancel active drag
    uintptr_t activeTool = 0;
    unsigned char dragActive = 0;
    SafeReadPod<uintptr_t>(base + WORLD_RVA + WORLD_SELECTED_TOOL_PTR_OFF, &activeTool);
    SafeReadPod<unsigned char>(base + WORLD_RVA + TOOL_DRAG_ACTIVE_OFFSET, &dragActive);
    if (activeTool != 0 || dragActive != 0) {
        SafeWritePod<uintptr_t>(base + WORLD_RVA + WORLD_SELECTED_TOOL_PTR_OFF, 0);
        SafeWritePod<unsigned char>(base + WORLD_RVA + TOOL_DRAG_ACTIVE_OFFSET, 0);
        if (base == (unsigned char*)g_exe) {
            RoadSync_CancelLocalDrag();
            for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; ++c) {
                RoadSync_ClearCategoryPreview(c);
            }
        }
    }
}

void SaveSync_NotifyWaitingLobbyActionBlocked()
{
    static ULONGLONG s_lastNotifyMs = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastNotifyMs < 1800) return;
    s_lastNotifyMs = now;

    if (SaveSync_IsWaitingForReconnect()) {
        TwoStates_AddToast(Tr("⏸ Связь с партнёром потеряна — строительство и рытьё на паузе до переподключения",
                              "⏸ Partner disconnected — building and terrain tools are locked until reconnect"), 2);
    } else {
        TwoStates_AddToast(Tr("⏸ Ожидание второго игрока — строительство, снос и рытьё временно заблокированы",
                              "⏸ Waiting for Player 2 — building, demolition and terrain tools are locked"), 2);
    }
}

void SaveSync_SetTestSessionState(SessionState state, bool hadPartnerInSession, const char* lastPartnerPeer)
{
    g_state = state;
    g_hadPartnerInSession = hadPartnerInSession;
    SetLastPartnerPeer(lastPartnerPeer ? lastPartnerPeer : "");
}

// Thread-safe snapshots into thread-local buffers so UI/logger readers never observe
// torn writes while session threads update these strings.
const char* SaveSync_GetPeerIp()
{
    static thread_local char tlsPeerIp[64] = {};
    ReadProtectedString(g_peerIp, tlsPeerIp, sizeof(tlsPeerIp));
    return tlsPeerIp;
}
const char* SaveSync_GetLastError()
{
    static thread_local char tlsLastError[256] = {};
    ReadProtectedString(g_lastError, tlsLastError, sizeof(tlsLastError));
    return tlsLastError;
}

static bool IsDualCmdBoundaryBefore(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"';
}

static bool IsDualCmdBoundaryAfter(char c)
{
    return c == '\0' || IsDualCmdBoundaryBefore(c) || c == '=';
}

static bool HasCmdLineTokenIgnoreCase(const char* cmdLine, const char* token)
{
    if (!cmdLine || !token || !token[0]) return false;
    size_t tLen = strlen(token);
    for (const char* p = cmdLine; *p; ++p) {
        if (p != cmdLine && !IsDualCmdBoundaryBefore(*(p - 1))) continue;
        if (_strnicmp(p, token, tLen) == 0 && IsDualCmdBoundaryAfter(p[tLen])) {
            return true;
        }
    }
    return false;
}

static bool IsPathSepChar(char c)
{
    return c == '\\' || c == '/';
}

static bool HasPathSegmentIgnoreCase(const char* path, const char* segment)
{
    if (!path || !segment || !segment[0]) return false;
    size_t sLen = strlen(segment);
    for (const char* p = path; *p; ++p) {
        if (p != path && !IsPathSepChar(*(p - 1))) continue;
        if (_strnicmp(p, segment, sLen) == 0 && (p[sLen] == '\0' || IsPathSepChar(p[sLen]))) {
            return true;
        }
    }
    return false;
}

bool SaveSync_DetectDualTestbedInputs(const char* roleEnv, const char* cmdLine, const char* exePath)
{
    if (roleEnv && (_stricmp(roleEnv, "HOST") == 0 || _stricmp(roleEnv, "CLIENT") == 0)) {
        return true;
    }
    if (cmdLine && (HasCmdLineTokenIgnoreCase(cmdLine, "--mp-host") ||
                    HasCmdLineTokenIgnoreCase(cmdLine, "--mp-left") ||
                    HasCmdLineTokenIgnoreCase(cmdLine, "--mp-client") ||
                    HasCmdLineTokenIgnoreCase(cmdLine, "--mp-right"))) {
        return true;
    }
    if (exePath && (HasPathSegmentIgnoreCase(exePath, "test-host") ||
                    HasPathSegmentIgnoreCase(exePath, "test-client"))) {
        return true;
    }
    return false;
}

bool SaveSync_IsDualTestbed()
{
    static std::atomic<int> s_cached{-1};
    int cached = s_cached.load(std::memory_order_acquire);
    if (cached < 0) {
        char envBuf[32] = {};
        if (GetEnvironmentVariableA("REPUBLICMP_ROLE", envBuf, sizeof(envBuf)) == 0) {
            envBuf[0] = '\0';
        }
        const char* cmdLine = GetCommandLineA();
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
            exePath[0] = '\0';
        }
        cached = SaveSync_DetectDualTestbedInputs(envBuf, cmdLine, exePath) ? 1 : 0;
        s_cached.store(cached, std::memory_order_release);
    }
    return cached == 1;
}

bool SaveSync_VerifyPeerHelloForTest(const char* magic, int protocol, const char* version,
                                     unsigned int dllCrc, char* whyOut, size_t whyCap)
{
    SessionHello peer = {};
    if (magic) memcpy(peer.magic, magic, std::min<size_t>(strlen(magic), sizeof(peer.magic)));
    peer.protocol = protocol;
    if (version) strncpy_s(peer.version, sizeof(peer.version), version, _TRUNCATE);
    peer.dllCrc = dllCrc;
    char localWhy[256] = {};
    bool ok = HelloMatches(peer, localWhy, sizeof(localWhy));
    if (whyOut && whyCap > 0) strncpy_s(whyOut, whyCap, localWhy, _TRUNCATE);
    return ok;
}

bool SaveSync_VerifyDlcListsForTest(const char* mineList, bool mineCheck,
                                    const char* peerList, bool peerCheck,
                                    char* whyOut, size_t whyCap)
{
    SessionDlcs mine = {};
    memcpy(mine.magic, "RPMPDLC", 8);
    mine.check = mineCheck ? 1 : 0;
    strncpy_s(mine.list, sizeof(mine.list), mineList ? mineList : "", _TRUNCATE);

    SessionDlcs theirs = {};
    memcpy(theirs.magic, "RPMPDLC", 8);
    theirs.check = peerCheck ? 1 : 0;
    strncpy_s(theirs.list, sizeof(theirs.list), peerList ? peerList : "", _TRUNCATE);

    char localWhy[512] = {};
    bool ok = DlcsMatch(mine, theirs, true, localWhy, sizeof(localWhy));
    if (whyOut && whyCap > 0) strncpy_s(whyOut, whyCap, localWhy, _TRUNCATE);
    return ok;
}

bool SaveSync_ExchangeCustomHelloOverSocketForTest(uintptr_t sockHandle, bool isHost,
                                                   const char* customMagic, int customProtocol,
                                                   const char* customVersion, unsigned int customDllCrc,
                                                   char* whyOut, size_t whyCap)
{
    if (whyOut && whyCap > 0) whyOut[0] = '\0';
    const SOCKET sock = (SOCKET)sockHandle;
    SessionHello mine = {};
    MakeHello(&mine);
    if (customMagic) {
        memset(mine.magic, 0, sizeof(mine.magic));
        memcpy(mine.magic, customMagic, std::min<size_t>(strlen(customMagic), sizeof(mine.magic)));
    }
    mine.protocol = customProtocol;
    if (customVersion) {
        memset(mine.version, 0, sizeof(mine.version));
        strncpy_s(mine.version, sizeof(mine.version), customVersion, _TRUNCATE);
    }
    mine.dllCrc = customDllCrc;

    SessionHello theirs = {};
    if (isHost) {
        if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) return false;
        if (!NetworkSendExact(sock, &mine, sizeof(mine))) return false;
    } else {
        if (!NetworkSendExact(sock, &mine, sizeof(mine))) return false;
        if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) return false;
    }
    char localWhy[256] = {};
    bool ok = HelloMatches(theirs, localWhy, sizeof(localWhy));
    if (whyOut && whyCap > 0) strncpy_s(whyOut, whyCap, localWhy, _TRUNCATE);
    return ok;
}

bool SaveSync_ExchangeHelloOverSocketForTest(uintptr_t sockHandle, bool isHost, char* whyOut, size_t whyCap)
{
    return SaveSync_ExchangeCustomHelloOverSocketForTest(
        sockHandle, isHost, "RPMPSES", REPUBLICMP_PROTOCOL, REPUBLICMP_VERSION,
        SaveSync_GetLocalDllCrc(), whyOut, whyCap);
}

bool SaveSync_ExchangeHandshakeOverSocketForTest(uintptr_t sockHandle, bool isHost,
                                               const char* dlcList, bool dlcCheck,
                                               const char* activeSaveDir,
                                               char* whyOut, size_t whyCap)
{
    if (!SaveSync_ExchangeHelloOverSocketForTest(sockHandle, isHost, whyOut, whyCap)) {
        return false;
    }
    const SOCKET sock = (SOCKET)sockHandle;
    SessionDlcs myDlcs = {};
    memcpy(myDlcs.magic, "RPMPDLC", 8);
    myDlcs.check = dlcCheck ? 1 : 0;
    strncpy_s(myDlcs.list, sizeof(myDlcs.list), dlcList ? dlcList : "", _TRUNCATE);

    SessionDlcs theirDlcs = {};
    if (!NetworkSendExact(sock, &myDlcs, sizeof(myDlcs)) ||
        !NetworkRecvExact(sock, &theirDlcs, sizeof(theirDlcs)) ||
        memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
        if (whyOut && whyCap > 0) strncpy_s(whyOut, whyCap, "dlc exchange failed", _TRUNCATE);
        return false;
    }
    char dlcWhy[512] = {};
    if (!DlcsMatch(myDlcs, theirDlcs, isHost, dlcWhy, sizeof(dlcWhy))) {
        if (whyOut && whyCap > 0) strncpy_s(whyOut, whyCap, dlcWhy, _TRUNCATE);
        return false;
    }
    char modWhy[256] = {};
    if (isHost) {
        ModSyncHostResult hr = ModSync_HostHandshake(sock, activeSaveDir ? activeSaveDir : "", modWhy, sizeof(modWhy));
        if (hr != MOD_SYNC_HOST_OK) {
            if (whyOut && whyCap > 0) strncpy_s(whyOut, whyCap, modWhy[0] ? modWhy : "mod host failed", _TRUNCATE);
            return false;
        }
    } else {
        ModSyncClientResult cr = ModSync_ClientHandshake(sock, modWhy, sizeof(modWhy));
        if (cr != MOD_SYNC_CLIENT_OK) {
            if (whyOut && whyCap > 0) strncpy_s(whyOut, whyCap, modWhy[0] ? modWhy : "mod client failed", _TRUNCATE);
            return false;
        }
    }
    return true;
}
