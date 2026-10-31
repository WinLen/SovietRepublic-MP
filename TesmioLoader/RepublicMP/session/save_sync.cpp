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
#include "../transport/steam_p2p.h"
#include "../world_sync/buildings/build_sync.h"
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
static char g_targetIp[64] = SAVE_SYNC_TEST_HOST_IP; // client target - overridable from the UI
static char g_peerIp[64] = "";
static char g_lastPartnerPeer[64] = "";
static char g_lastError[256] = "";

// Set by SaveSync_RequestDisconnect; reset at the start of every session.
static volatile bool g_disconnectRequested = false;

// The join connection in use, so Disconnect can break a blocking recv.
static volatile SOCKET g_activeSock = INVALID_SOCKET;

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
static volatile bool g_lockstep = false; // the current session's mode
#define LOCKSTEP_CONNECT_TIMEOUT_MS 180000 // the host loads the world again before it listens
#define LOCKSTEP_SILENCE_MS 20000          // a game that sent nothing (not even a keepalive) is gone
static const char* const kResyncReason = "re-sync";
static const char* const kLocalHostOpenEventName = "Local\\RepublicMP_LocalHostOpen_5556";
static HANDLE g_localHostOpenEvent = nullptr;

static void SetState(SessionState s)
{
    g_state = s;
    if (s == SESSION_HOST_LISTENING) {
        if (!g_localHostOpenEvent) {
            g_localHostOpenEvent = CreateEventA(nullptr, TRUE, TRUE, kLocalHostOpenEventName);
        } else {
            SetEvent(g_localHostOpenEvent);
        }
    } else if (g_localHostOpenEvent) {
        ResetEvent(g_localHostOpenEvent);
        CloseHandle(g_localHostOpenEvent);
        g_localHostOpenEvent = nullptr;
    }
}

bool SaveSync_IsLocalPcHostWaiting()
{
    if (g_state != SESSION_IDLE) return false;
    HANDLE h = OpenEventA(SYNCHRONIZE, FALSE, kLocalHostOpenEventName);
    if (!h) return false;
    bool open = (WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
    CloseHandle(h);
    return open;
}

static void SetPeerIp(const char* ip)
{
    EnterCriticalSection(&g_lock);
    strncpy_s(g_peerIp, ip ? ip : "", _TRUNCATE);
    LeaveCriticalSection(&g_lock);
}

static void SetLastPartnerPeer(const char* peer)
{
    EnterCriticalSection(&g_lock);
    strncpy_s(g_lastPartnerPeer, peer ? peer : "", _TRUNCATE);
    LeaveCriticalSection(&g_lock);
}

static void SetSessionError(const char* fmt, ...)
{
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    strncpy_s(g_lastError, body, _TRUNCATE);
    LeaveCriticalSection(&g_lock);
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

static unsigned int g_dllCrc = 0;
static bool g_dllCrcDone = false;

// CRC32 of this RepublicMP.dll file on disk: two players with the same file
// have the same build, whatever its version string says.
unsigned int SaveSync_GetLocalDllCrc()
{
    if (g_dllCrcDone) return g_dllCrc;
    g_dllCrcDone = true;

    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&SaveSync_GetLocalDllCrc, &self) ||
        !GetModuleFileNameA(self, path, MAX_PATH)) {
        Logf("republicmp  SESSION: couldn't locate RepublicMP.dll for the build check");
        return 0;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Logf("republicmp  SESSION: couldn't read [%s] for the build check", path);
        return 0;
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
    h->lockstep = g_lockstepChosen ? 1 : 0;
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

static bool AllChannelsConnected()
{
    return BuildSync_IsConnected() && RoadSync_IsConnected() && TerrainSync_IsConnected();
}

// Closes every channel, connected or still waiting for its peer.
static void ResetChannels()
{
    BuildSync_Reset();
    RoadSync_Reset();
    TerrainSync_Reset();
    LockstepNet_Close();
    Determinism_EndSession();
}

static void StartChannelsHost()
{
    BuildSync_StartHost();
    RoadSync_StartHost();
    TerrainSync_StartHost();
}

// Re-tries every channel that is neither connected nor connecting (the host
// may not be listening yet when the first attempt goes out).
static void StartChannelsClient()
{
    if (!BuildSync_IsConnected() && !BuildSync_IsConnecting()) BuildSync_StartClient(g_targetIp);
    if (!RoadSync_IsConnected() && !RoadSync_IsConnecting()) RoadSync_StartClient(g_targetIp);
    if (!TerrainSync_IsConnected() && !TerrainSync_IsConnecting()) TerrainSync_StartClient(g_targetIp);
}

// Waits until all channels are up. Client side keeps re-trying.
static bool WaitForChannels(bool client)
{
    ULONGLONG start = GetTickCount64();
    ULONGLONG lastTry = 0;
    while (!g_disconnectRequested) {
        if (AllChannelsConnected()) return true;
        ULONGLONG now = GetTickCount64();
        if (now - start > CHANNELS_CONNECT_TIMEOUT_MS) return false;
        if (client && now - lastTry >= CHANNELS_RETRY_MS) {
            StartChannelsClient();
            lastTry = now;
        }
        Sleep(100);
    }
    return false;
}

// Names of the channels that are down, e.g. "roads, terrain".
static void DownChannels(char* out, size_t outSize)
{
    out[0] = 0;
    if (!BuildSync_IsConnected()) strncat_s(out, outSize, "buildings ", _TRUNCATE);
    if (!RoadSync_IsConnected()) strncat_s(out, outSize, "roads ", _TRUNCATE);
    if (!TerrainSync_IsConnected()) strncat_s(out, outSize, "terrain ", _TRUNCATE);
    size_t n = strlen(out);
    if (n) out[n - 1] = 0;
}

static bool ResyncDue()
{
    if (g_resyncRequested) return true;
    int minutes = g_autoResyncMinutes;
    return minutes > 0 && GetTickCount64() - g_lastWorldSentTick >= (ULONGLONG)minutes * 60000;
}

// Host: has the game thread send the announcement and waits until it is out.
// False (re-sync dropped) if it could not be sent.
static bool AnnounceResync()
{
    g_resyncRequested = false;
    g_resyncSent = false;
    g_resyncSendNow = true;
    ULONGLONG start = GetTickCount64();
    while (!g_resyncSent && !g_disconnectRequested && GetTickCount64() - start < RESYNC_ANNOUNCE_TIMEOUT_MS)
        Sleep(50);
    g_resyncSendNow = false;
    if (!g_resyncSent) {
        SaveStatus("[HOST] couldn't announce the re-sync - skipped");
        g_lastWorldSentTick = GetTickCount64(); // the timer tries again a full interval later
        return false;
    }
    Sleep(RESYNC_ANNOUNCE_FLUSH_MS);
    g_resyncing = true;
    return true;
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
        if (LockstepNet_MsSinceReceived() > LOCKSTEP_SILENCE_MS) return Tr("второй игрок перестал отвечать", "the other game stopped answering");
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
    return g_disconnectRequested ? nullptr : Tr("Lockstep-соединение закрыто", "the lockstep connection closed");
}

static const char* MonitorLiveSession(bool client)
{
    if (g_lockstep) return MonitorLockstep(client);
    static char s_why[160];
    ULONGLONG downSince = 0;
    ULONGLONG lastTry = 0;
    for (;;) {
        if (g_disconnectRequested) return nullptr;
        if (client && g_resyncAnnounced) return kResyncReason;

        bool b = BuildSync_IsConnected(), r = RoadSync_IsConnected(), t = TerrainSync_IsConnected();
        if (b && r && t) {
            if (downSince) {
                SaveStatus("live sync restored after %llus", (GetTickCount64() - downSince) / 1000);
                downSince = 0;
            }
            if (!client && ResyncDue() && AnnounceResync()) return kResyncReason;
            Sleep(200);
            continue;
        }

        // The announcement is noted before its connection is seen closing.
        if (client && g_resyncAnnounced) return kResyncReason;

        char down[64];
        DownChannels(down, sizeof(down));
        if (!b && !r && !t) {
            // When the peer really leaves, the three drop within ~100 ms of
            // each other; the reconnect branch below covers that gap.
            _snprintf_s(s_why, sizeof(s_why), _TRUNCATE, "all live sync connections closed");
            return s_why;
        }

        ULONGLONG now = GetTickCount64();
        if (!downSince) {
            downSince = now;
            SaveStatus("live sync channel(s) dropped: %s - reconnecting", down);
        }
        if (now - downSince > CHANNEL_RECONNECT_TIMEOUT_MS) {
            _snprintf_s(s_why, sizeof(s_why), _TRUNCATE, "couldn't reconnect: %s", down);
            return s_why;
        }
        if (client) {
            if (now - lastTry >= CHANNELS_RETRY_MS) {
                StartChannelsClient();
                lastTry = now;
            }
        } else {
            StartChannelsHost(); // no-op for channels already connected or listening
        }
        Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// Telemetry & SteamP2P_FlushSend Hooks (R7)
// ---------------------------------------------------------------------------

extern void SyncBlock_SetPercentEx(int percent, int queuedBytes, int totalBytes, float speedMbSec);

typedef bool (*FnSteamP2P_FlushSend)(SOCKET s, DWORD timeoutMs);
static FnSteamP2P_FlushSend o_steamP2P_FlushSend = nullptr;

#pragma pack(push, 8)
struct P2PSessionState_t {
    uint8_t m_bConnectionActive;
    uint8_t m_bConnecting;
    uint8_t m_eP2PSessionError;
    uint8_t m_bUsingRelay;
    int32_t m_nBytesQueuedForSend;
    int32_t m_nPacketsQueuedForSend;
    uint32_t m_nRemoteIP;
    uint16_t m_nRemotePort;
};
#pragma pack(pop)

typedef bool (*FnGetP2PSessionState)(void* pSteamNetworking, uint64_t steamIDRemote, P2PSessionState_t* pConnectionState);
static FnGetP2PSessionState o_getP2PSessionState = nullptr;

static uint64_t g_transferTotalBytes = 0;
static uint64_t g_transferStartTime = 0;
static uint64_t g_transferLastTick = 0;
static uint64_t g_transferLastDone = 0;
static float g_transferSpeedEma = 0.0f;

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
    ULONGLONG now = GetTickCount64();
    if (g_transferLastTick == 0) g_transferLastTick = now;
    ULONGLONG dt = now - g_transferLastTick;
    if (dt >= 150) {
        double bytesDelta = (double)(done > g_transferLastDone ? done - g_transferLastDone : 0);
        double curSpeed = (bytesDelta / (1024.0 * 1024.0)) / ((double)dt / 1000.0);
        if (g_transferSpeedEma <= 0.001f) {
            g_transferSpeedEma = (float)curSpeed;
        } else {
            g_transferSpeedEma = 0.7f * g_transferSpeedEma + 0.3f * (float)curSpeed;
        }
        g_transferLastTick = now;
        g_transferLastDone = done;
    }
}

static bool Hook_GetP2PSessionState(void* pSteamNetworking, uint64_t steamIDRemote, P2PSessionState_t* pConnectionState)
{
    bool res = o_getP2PSessionState ? o_getP2PSessionState(pSteamNetworking, steamIDRemote, pConnectionState) : false;
    if (res && pConnectionState && g_inSaveTransfer.load() && g_transferTotalBytes > 0) {
        int queued = pConnectionState->m_nBytesQueuedForSend;
        if (queued < 0) queued = 0;
        uint64_t done = (g_transferTotalBytes > (uint64_t)queued) ? (g_transferTotalBytes - (uint64_t)queued) : 0;
        int percent = (int)((done * 100) / g_transferTotalBytes);
        if (percent > 100) percent = 100;
        UpdateTransferSpeed(done);
        SyncBlock_SetPercentEx(percent, queued, (int)g_transferTotalBytes, g_transferSpeedEma);
    }
    return res;
}

static bool Hook_SteamP2P_FlushSend(SOCKET s, DWORD timeoutMs)
{
    if (!o_steamP2P_FlushSend) return false;
    g_inSaveTransfer = true;
    bool ok = o_steamP2P_FlushSend(s, timeoutMs);
    g_inSaveTransfer = false;
    if (ok && g_transferTotalBytes > 0) {
        SyncBlock_SetPercentEx(100, 0, (int)g_transferTotalBytes, g_transferSpeedEma);
    }
    return ok;
}

bool SaveSync_InstallHooks()
{
    InstallInlineHook((void*)&SteamP2P_FlushSend, (void*)&Hook_SteamP2P_FlushSend,
                      (void**)&o_steamP2P_FlushSend, nullptr, 0, "republicmp flush_send");

    HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
    if (hSteam) {
        void* pFn = (void*)GetProcAddress(hSteam, "SteamAPI_ISteamNetworking_GetP2PSessionState");
        if (pFn) {
            InstallInlineHook(pFn, (void*)&Hook_GetP2PSessionState,
                              (void**)&o_getP2PSessionState, nullptr, 0, "republicmp p2p_session_state");
        }
    }
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
    int queued = (int)(total > done ? total - done : 0);
    SyncBlock_SetPercentEx(percent, queued, (int)total, g_transferSpeedEma);

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

static bool GetGameSaveDir(char* outPath, size_t outSize, const char* saveName)
{
    if (!saveName) return false;
    if (_strnicmp(saveName, "save\\", 5) == 0 || _strnicmp(saveName, "save/", 5) == 0) {
        saveName += 5;
    }
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return false;
    *lastSlash = '\0';
    return _snprintf_s(outPath, outSize, _TRUNCATE, "%s\\media_soviet\\save\\%s", exePath, saveName) >= 0;
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
        { 1u << 3, "World Maps", "dlc4" }
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
    for (int d = 5; d <= 16; ++d) {
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

    // 2. Check coupled trailers / wagons in type + 0x7C08
    BYTE* v = nullptr;
    BYTE* e = nullptr;
    if (SafeReadPod((const char*)vehicleTypePtr + 0x7C08, &v) &&
        SafeReadPod((const char*)vehicleTypePtr + 0x7C10, &e) &&
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

static void SafeSaveHostWorldSaveNames(char* outField, size_t fieldSize, char* outDat, size_t datSize)
{
    outField[0] = '\0';
    outDat[0] = '\0';
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
}

static void SafeRestoreHostWorldSaveNames(const char* origField, const char* origDat)
{
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
}

// One join, from the accepted connection to the lifted sync screen. The
// caller's session loop decides what happens next. Returns true when the
// player is in the game with all channels up.
static bool RunHostJoin(SOCKET sock, bool rejoin = false)
{
    g_activeSock = sock;
    SyncBlock_SetActive(true, g_resyncing
        ? Tr("Повторная синхронизация мира...", "Re-syncing the world")
        : Tr("Подключается второй игрок...", "A player is connecting"));
    EnsureGamePaused();
    bool joined = false;
    char madeSaveDir[MAX_PATH] = {}; // cleaned up below
    char madeZip[MAX_PATH] = {};
    char origSaveField[128] = {};
    char origSaveDat[128] = {};
    SafeSaveHostWorldSaveNames(origSaveField, sizeof(origSaveField), origSaveDat, sizeof(origSaveDat));

    do {
        // 1. Versions.
        HostStep(sock, 1, Tr("Подключение игрока — проверка версий", "A player is connecting - checking versions"));
        SessionHello mine, theirs;
        MakeHello(&mine);
        TwoStates_PrepareHostSave(g_resyncing, mine.stateRub, mine.stateUsd);
        if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
            SetSessionError(Tr("Игрок отключился во время проверки версий", "the player dropped during the version check"));
            break;
        }
        NetworkSendExact(sock, &mine, sizeof(mine)); // always, so the client can show what differs
        char why[256];
        if (!HelloMatches(theirs, why, sizeof(why))) {
            SetSessionError(Tr("Вход отклонён: %s", "join refused: %s"), why);
            SaveStatus("[HOST] join refused: %s", why);
            break;
        }
        SessionDlcs myDlcs, theirDlcs;
        MakeDlcs(&myDlcs);
        if (!NetworkSendExact(sock, &myDlcs, sizeof(myDlcs)) || !NetworkRecvExact(sock, &theirDlcs, sizeof(theirDlcs)) ||
            memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
            SetSessionError(Tr("Игрок отключился во время проверки DLC", "the player dropped during the DLC check"));
            break;
        }
        char dlcWhy[512];
        if (!DlcsMatch(myDlcs, theirDlcs, true, dlcWhy, sizeof(dlcWhy))) {
            SetSessionError(Tr("Вход отклонён: %s", "join refused: %s"), dlcWhy);
            SaveStatus("[HOST] join refused: %s", dlcWhy);
            break;
        }

        // 1b. Mod sync handshake (pre-save, host-authoritative)
        HostStep(sock, 1, Tr("Синхронизация модов Мастерской Steam", "Synchronizing Steam Workshop mods"), 0);
        SendMsg(sock, MSG_MOD_SYNC, 1, 0, Tr("проверка модов", "checking mods"));
        char activeSaveDir[MAX_PATH] = {};
        const char* activeName = origSaveField[0] ? origSaveField : (origSaveDat[0] ? origSaveDat : nullptr);
        if (activeName) {
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
            break;
        } else if (modRes != MOD_SYNC_HOST_OK) {
            SetSessionError(Tr("Ошибка синхронизации модов: %s", "mod sync failed: %s"), modWhy);
            SaveStatus("[HOST] mod sync failed: %s", modWhy);
            break;
        }

        // 2. Save.
        char liveSaveName[64];
        GenerateRandomSaveName(liveSaveName, sizeof(liveSaveName));
        char saveDir[MAX_PATH] = {};
        if (!GetGameSaveDir(saveDir, sizeof(saveDir), liveSaveName)) {
            SetSessionError(Tr("Не удалось сформировать путь сохранения", "couldn't build the save path"));
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't save the world");
            break;
        }
        HostStep(sock, 2, Tr("Сохранение мира для передачи", "Saving the world"));
        SendMsg(sock, MSG_STEP, 2, -1, Tr("сохранение мира", "saving the world"));
        // The engine's save writer does not create the destination folder.
        if (!CreateDirectoryA(saveDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            SetSessionError(Tr("Не удалось создать папку сохранения (код %lu)", "couldn't create the save folder (error %lu)"), GetLastError());
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't save the world");
            break;
        }
        strncpy_s(madeSaveDir, saveDir, _TRUNCATE);
        TriggerSaveGame(liveSaveName);
        if (!WaitForSaveToFinish(0, &g_disconnectRequested)) {
            SendMsg(sock, MSG_ABORT, 0, -1, "host stopped hosting");
            break;
        }
        if (TwoStates_GetExpandBorders()) {
            TwoStates_PatchSaveScriptIni(saveDir, 0.0f);
        }

        // 2b. If diagnostic log sharing is enabled, also exchange diagnostic logs before packing/sending the world.
        if (TwoStates_GetSendClientLogs()) {
            const char* hostLogReason = DiagLog_HasPrevCrashOrDesyncLog() ? "HOST_PREV_CRASH" : "HOST_JOIN";
            std::vector<unsigned char> hostLogBundle;
            if (DiagLog_BuildClientLogBundle(hostLogBundle, hostLogReason)) {
                if (SendMsg(sock, MSG_CLIENT_LOGS, (int)hostLogBundle.size(), -1, "")) {
                    NetworkSendExact(sock, hostLogBundle.data(), hostLogBundle.size());
                }
            }
        }

        // 3. Pack.
        char zipPath[MAX_PATH] = {};
        _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
        HostStep(sock, 3, Tr("Упаковка сохранения", "Packing the save"), 0);
        SendMsg(sock, MSG_STEP, 3, 0, Tr("упаковка сохранения", "packing the save"));
        ProgressCtx packCtx = { sock, 3, Tr("упаковка сохранения", "packing the save"), -100 };
        strncpy_s(madeZip, zipPath, _TRUNCATE);
        if (!ZipSaveDirectory(saveDir, zipPath, ZipProgressCb, &packCtx)) {
            SetSessionError(Tr("Ошибка упаковки сохранения", "packing the save failed"));
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't pack the save");
            break;
        }
        if (g_disconnectRequested) {
            SendMsg(sock, MSG_ABORT, 0, -1, "host stopped hosting");
            break;
        }

        // 4. Send.
        HostStep(sock, 4, Tr("Передача мира второму игроку", "Sending the world to the player"), 0);
        SendMsg(sock, MSG_FILE_FOLLOWS, 4, -1, "");
        ProgressCtx sendCtx = { INVALID_SOCKET, 4, nullptr, 0 };
        g_transferTotalBytes = GetFileSize64(zipPath);
        g_transferStartTime = GetTickCount64();
        g_transferLastTick = g_transferStartTime;
        g_transferLastDone = 0;
        g_transferSpeedEma = 0.0f;
        g_inSaveTransfer = true;
        bool sendOk = NetworkSendFile(sock, zipPath, NetworkProgressCb, &sendCtx);
        g_inSaveTransfer = false;
        if (!sendOk) {
            SetSessionError(Tr("Ошибка передачи сохранения — соединение потеряно", "sending the save failed - connection lost"));
            break;
        }

        // Lockstep: this PC loads the same save while the player unpacks and
        // loads its copy; the player's step messages wait in the socket.
        if (g_lockstep) {
            HostStep(sock, 5, Tr("Загрузка общего мира...", "Loading the shared world while the player loads it"));
            if (!Determinism_BeginSession(true)) {
                SetSessionError(Tr("Слой детерминизма недоступен (см. лог)", "lockstep is not available - see the log (determinism layer)"));
                break;
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
                    SetState(SESSION_HOST_LISTENING);
                    EnsureGamePaused();
                }
                TwoStates_OnSessionEnded();
                ResetChannels();
                break;
            }
        }

        // 5-6. The player unpacks and loads; its steps arrive as messages.
        HostStep(sock, 5, Tr("Ожидание второго игрока — распаковка", "Waiting for the player - unpacking"), 0);
        bool ready = false;
        for (;;) {
            SessionMsg m;
            if (!RecvMsg(sock, &m)) {
                SetSessionError(Tr("Второй игрок отключился во время загрузки мира", "the player dropped while loading"));
                break;
            }
            if (m.code == MSG_READY) { ready = true; break; }
            if (m.code == MSG_ABORT) {
                SetSessionError(Tr("Второй игрок отменил подключение: %s", "the player gave up: %s"), m.text);
                SaveStatus("[HOST] the player gave up: %s", m.text);
                break;
            }
            if (m.code == MSG_CLIENT_LOGS) {
                if (m.step > 0 && m.step <= (2 << 20)) {
                    std::vector<unsigned char> logBuf((size_t)m.step);
                    if (!NetworkRecvExact(sock, logBuf.data(), logBuf.size())) break;
                    DiagLog_SaveRemoteClientLogBundle(logBuf.data(), logBuf.size());
                }
                continue;
            }
            if (m.code == MSG_STEP) {
                char text[96];
                _snprintf_s(text, sizeof(text), _TRUNCATE, Tr("Ожидание игрока — %s", "Waiting for the player - %s"), m.text);
                if (m.step == 5) SyncBlock_SetStep(5, JOIN_STEPS, text, m.percent);
                else HostStep(sock, m.step, text, m.percent);
            }
        }
        if (!ready) {
            if (!g_disconnectRequested) {
                SetState(SESSION_HOST_LISTENING);
                EnsureGamePaused();
            }
            if (g_lockstep) {
                ResetChannels();
                ModSync_EndSession();
            }
            break;
        }

        // 7. Lockstep: both loaded the same save; the channel connects.
        if (g_lockstep) {
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
                ModSync_EndSession();
                break;
            }
            joined = true;
            break;
        }

        // 7. Live sync channels.
        HostStep(sock, 7, Tr("Запуск живой синхронизации...", "Starting live sync"));
        StartChannelsHost();
        if (!WaitForChannels(false)) {
            if (!g_disconnectRequested) {
                SetSessionError(Tr("Каналы синхронизации не подключились — порты TCP %d-%d также должны быть открыты у хоста",
                                   "the live sync channels didn't connect - TCP ports %d-%d must reach the host too (forward them like port %d)"),
                                BUILD_SYNC_PORT, TERRAIN_SYNC_PORT, SAVE_SYNC_PORT);
                SetState(SESSION_HOST_LISTENING);
                EnsureGamePaused();
            }
            ResetChannels();
            break;
        }
        joined = true;
        TwoStates_OnSessionJoined(true, mine.twoStatesMode, mine.borderHalfWidth, mine.expandBorders != 0,
                                  mine.stateRub, mine.stateUsd);
    } while (false);

    // The zip and temporary staging folder only existed to transfer and load the shared world.
    if (madeZip[0]) DeleteFileA(madeZip);
    if (madeSaveDir[0]) {
        KeepOnlyNewestHostSave(madeSaveDir);
        std::error_code rmEc;
        std::filesystem::remove_all(madeSaveDir, rmEc);
    }
    SafeRestoreHostWorldSaveNames(origSaveField, origSaveDat);

    g_activeSock = INVALID_SOCKET;
    NetworkCloseSocket(sock);
    if (!joined && !g_disconnectRequested) {
        SetState(SESSION_HOST_LISTENING);
        EnsureGamePaused();
    }
    SyncBlock_SetActive(false, nullptr);
    // Keep Host paused while waiting in the Co-op Lobby; only unpause if hosting was stopped.
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
    while (!g_disconnectRequested) {
        SetState(SESSION_HOST_LISTENING);
        EnsureGamePaused();
        SteamP2P_SetHostingPresence(!rejoin);
        if (!rejoin) {
            SetPeerIp("");
            SaveStatus("[HOST] hosting on port %d (TCP + Steam P2P) - waiting for a player", SAVE_SYNC_PORT);
        }

        // No accept timeout (cancellable) unless the player is coming back
        // from a re-sync; no I/O timeout - the player may take a while to
        // unpack and load.
        SOCKET sock = NetworkHostAcceptOne(SAVE_SYNC_PORT, rejoin ? RESYNC_REJOIN_TIMEOUT_MS : 0, 0,
                                           &g_disconnectRequested);
        if (sock == INVALID_SOCKET) {
            if (g_disconnectRequested) break;
            if (rejoin) {
                rejoin = false;
                g_resyncing = false;
                SetSessionError(Tr("Второй игрок не вернулся для пересинхронизации", "the player didn't come back for the re-sync"));
                SaveStatus("[HOST] the player didn't come back for the re-sync - waiting for a player again");
                continue;
            }
            SetSessionError(Tr("Не удалось открыть порт %d (возможно, уже занят)", "couldn't listen on port %d (already in use?)"), SAVE_SYNC_PORT);
            break;
        }

        SteamP2P_SetHostingPresence(false);
        char peer[64];
        PeerIpOf(sock, peer, sizeof(peer));
        SetPeerIp(peer);
        SetState(SESSION_HOST_JOINING);
        SetSessionError("");
        SaveStatus(rejoin ? "[HOST] %s is back - re-syncing, both games are paused until it is loaded"
                          : "[HOST] %s is joining - both games are paused until it is loaded", peer);

        g_lockstep = g_lockstepChosen;
        bool joined = RunHostJoin(sock, rejoin);
        rejoin = false;
        g_resyncing = false;
        if (!joined) {
            if (g_disconnectRequested) break;
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

// Returns true when the player is in the host's game with all channels up.
// rejoin: coming back for a re-sync - the host may need a moment to listen
// again, so refused connects are retried.
static bool RunClientJoin(bool rejoin)
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

        // 1. Connect + versions.
        ClientStep(1, Tr("Подключение к серверу — проверка версий", "Connecting to the host - checking versions"));
        if (!NetworkInit()) {
            SetSessionError(Tr("Ошибка инициализации сети", "network init failed"));
            break;
        }
        ULONGLONG connectStart = GetTickCount64();
        for (;;) {
            sock = NetworkConnect(g_targetIp, SAVE_SYNC_PORT, 30000, 0, &g_disconnectRequested);
            if (sock != INVALID_SOCKET || !rejoin || g_disconnectRequested) break;
            if (GetTickCount64() - connectStart > RESYNC_REJOIN_TIMEOUT_MS) break;
            Sleep(RESYNC_RETRY_MS);
        }
        if (sock == INVALID_SOCKET) {
            if (!g_disconnectRequested)
                SetSessionError(Tr("Не удалось подключиться к %s — проверьте, что хост уже открыл сервер",
                                   "couldn't reach %s:%d - is the host hosting (and not already playing with someone)?"),
                                g_targetIp, SAVE_SYNC_PORT);
            break;
        }
        g_activeSock = sock;
        // A Disconnect that came while connecting found no socket to shut
        // down; without this the whole world would still be downloaded.
        if (g_disconnectRequested) break;
        SessionHello theirs = {};
        if (!ClientPerformHandshake(sock, &theirs)) {
            break;
        }

        // 2-3. The host saves and packs; its steps arrive as messages.
        ClientStep(2, Tr("Ожидание хоста — сохранение мира", "Waiting for the host - saving the world"));
        bool fileFollows = false;
        for (;;) {
            SessionMsg m;
            if (!RecvMsg(sock, &m)) {
                SetSessionError(Tr("Хост отключился во время подготовки мира", "the host dropped while preparing the world"));
                break;
            }
            if (m.code == MSG_FILE_FOLLOWS) { fileFollows = true; break; }
            if (m.code == MSG_ABORT) {
                SetSessionError(Tr("Хост прервал подключение: %s", "the host gave up: %s"), m.text);
                break;
            }
            if (m.code == MSG_CLIENT_LOGS) {
                if (m.step > 0 && m.step <= (2 << 20)) {
                    std::vector<unsigned char> logBuf((size_t)m.step);
                    if (!NetworkRecvExact(sock, logBuf.data(), logBuf.size())) break;
                    DiagLog_SaveRemoteClientLogBundle(logBuf.data(), logBuf.size());
                }
                continue;
            }
            if (m.code == MSG_STEP) {
                char text[96];
                _snprintf_s(text, sizeof(text), _TRUNCATE, Tr("Ожидание хоста — %s", "Waiting for the host - %s"), m.text);
                SyncBlock_SetStep(m.step, JOIN_STEPS, text, m.percent);
            }
        }
        if (!fileFollows) break;

        // 4. Receive.
        SaveTransferGuard transferGuard;
        ClientStep(4, Tr("Приём мира от хоста", "Receiving the world"), 0);
        ProgressCtx recvCtx = { sock, 4, Tr("приём мира", "receiving the world"), 0 };
        g_transferStartTime = GetTickCount64();
        g_transferLastTick = g_transferStartTime;
        g_transferLastDone = 0;
        g_transferSpeedEma = 0.0f;
        if (!NetworkReceiveFile(sock, zipPath, NetworkProgressCb, &recvCtx)) {
            SetSessionError(Tr("Ошибка приёма мира — соединение потеряно", "receiving the world failed - connection lost"));
            break;
        }

        // 5. Unpack (relayed to the host).
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
            break;
        }
        if (g_disconnectRequested) {
            SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled");
            break;
        }

        // 5b. Upload client diagnostic logs (including any previous session crash/desync logs) to Host.
        if (TwoStates_GetSendClientLogs()) {
            const char* logReason = rejoin ? "RESYNC" : (DiagLog_HasPrevCrashOrDesyncLog() ? "PREV_CRASH" : "CLIENT_JOIN");
            std::vector<unsigned char> logBundle;
            if (DiagLog_BuildClientLogBundle(logBundle, logReason)) {
                if (SendMsg(sock, MSG_CLIENT_LOGS, (int)logBundle.size(), -1, "")) {
                    NetworkSendExact(sock, logBundle.data(), logBundle.size());
                }
            }
        }

        // 6. Load.
        ClientStep(6, Tr("Загрузка мира...", "Loading the world"));
        SendMsg(sock, MSG_STEP, 6, -1, Tr("загрузка мира", "loading the world"));
        char receivedSaveName[MAX_PATH] = {};
        _snprintf_s(receivedSaveName, sizeof(receivedSaveName), _TRUNCATE, "%s%s",
                    SAVE_SYNC_CLIENT_STAGING_NAME, SAVE_SYNC_RECEIVED_SUFFIX);
        if (g_lockstep) {
            if (!Determinism_BeginSession(false)) {
                SetSessionError(Tr("Слой детерминизма недоступен (см. лог)", "lockstep is not available - see the log (determinism layer)"));
                SendMsg(sock, MSG_ABORT, 0, -1, "lockstep is not available on the player");
                break;
            }
            TwoStates_OnSessionJoined(false, theirs.twoStatesMode, theirs.borderHalfWidth, theirs.expandBorders != 0,
                                      theirs.stateRub, theirs.stateUsd);
        }
        if (rejoin) {
            TwoStates_CapturePreResyncCamera();
        }
        TriggerLoadSave(receivedSaveName);
        bool loadFinished = WaitForLoadToFinish(0, &g_disconnectRequested, JoinHostGone, &sock);
        while (SaveSync_IsWorldLoadInProgress()) {
            Sleep(50);
        }
        {
            std::error_code rmEc;
            std::filesystem::remove_all(destDir, rmEc);
        }
        if (!loadFinished) {
            if (g_disconnectRequested) SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled while loading");
            else SetSessionError(Tr("Хост отключился во время загрузки мира", "the host dropped while the world was loading"));
            if (g_lockstep) TwoStates_OnSessionEnded();
            break;
        }

        // 7. Ready -> the host starts listening on the channels; connect them.
        if (g_lockstep) {
            ClientStep(7, Tr("Ожидание загрузки мира у хоста...", "Waiting for the host to load the shared world"));
            SendMsg(sock, MSG_READY, 7, -1, "");
            if (!Determinism_SessionRunning()) {
                SetSessionError(Tr("Ошибка загрузки общего мира", "loading the shared world failed"));
                TwoStates_OnSessionEnded();
                break;
            }
            // In short tries, watching the join connection: a host that
            // crashed while loading closes it.
            ULONGLONG joinStart = GetTickCount64();
            bool lockstepUp = false, hostGone = false;
            while (!g_disconnectRequested && GetTickCount64() - joinStart < LOCKSTEP_CONNECT_TIMEOUT_MS) {
                if (LockstepNet_Join(g_targetIp, 6000, &g_disconnectRequested)) {
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
                else if (!g_disconnectRequested) SetSessionError(Tr("Не удалось открыть Lockstep-канал к %s:%d", "couldn't open the lockstep connection to %s:%d"), g_targetIp, LOCKSTEP_PORT);
                TwoStates_OnSessionEnded();
                break;
            }
            joined = true;
            break;
        }
        ClientStep(7, Tr("Запуск каналов синхронизации...", "Starting live sync"));
        SendMsg(sock, MSG_READY, 7, -1, "");
        if (!WaitForChannels(true)) {
            if (!g_disconnectRequested) SetSessionError(Tr("Каналы синхронизации не подключились — TCP порты %d-%d хоста недоступны", "the live sync channels didn't connect - TCP ports %d-%d of the host are not reachable (only port %d is forwarded?)"), BUILD_SYNC_PORT, TERRAIN_SYNC_PORT, SAVE_SYNC_PORT);
            ResetChannels();
            break;
        }
        joined = true;
        TwoStates_OnSessionJoined(false, theirs.twoStatesMode, theirs.borderHalfWidth, theirs.expandBorders != 0,
                                  theirs.stateRub, theirs.stateUsd);
    } while (false);

    g_activeSock = INVALID_SOCKET;
    if (sock != INVALID_SOCKET) NetworkCloseSocket(sock);
    SyncBlock_SetActive(false, nullptr);
    // Joined: stays paused until either player resumes (see RunHostJoin).
    if (!joined) EnsureGameUnpaused();
    return joined;
}

static void RunClientSession()
{
    SetPeerIp(g_targetIp);
    bool rejoin = false;
    for (;;) {
        SetState(SESSION_CLIENT_JOINING);
        bool joined = RunClientJoin(rejoin);
        g_resyncing = false;
        if (!joined) {
            TwoStates_OnSessionEnded();
            ResetChannels();
            if (g_disconnectRequested) SaveStatus("[CLIENT] cancelled");
            else SaveStatus("[CLIENT] couldn't join: %s", SaveSync_GetLastError());
            return;
        }

        SetState(SESSION_CLIENT_IN_GAME);
        SaveStatus("[CLIENT] in the game at %s - live sync running, paused until either player resumes", g_targetIp);
        const char* why = MonitorLiveSession(true);
        ResetChannels();
        if (why == kResyncReason) {
            rejoin = true;
            SaveStatus("[CLIENT] the host is re-syncing the world - reconnecting");
            continue;
        }
        TwoStates_OnSessionEnded();
        if (why) {
            SetSessionError(Tr("Отключено от хоста: %s", "disconnected from the host: %s"), why);
            SaveStatus("[CLIENT] disconnected from the host (%s)", why);
        } else {
            SaveStatus("[CLIENT] left the session");
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
    while (SaveSync_IsWorldLoadInProgress()) {
        Sleep(50);
    }
    SetState(SESSION_IDLE);
    return 0;
}

static DWORD WINAPI ClientSessionThread(LPVOID)
{
    RunClientSession();
    SetPeerIp("");
    while (SaveSync_IsWorldLoadInProgress()) {
        Sleep(50);
    }
    SetState(SESSION_IDLE);
    return 0;
}

// The world's road network (world+0xf7a0): made with every world, an empty
// map too; deleted and zeroed when the world is torn down (0x9952C,
// 0x29564E) - back in the main menu, or while another save loads.
#define WORLD_ROAD_NETWORK_RVA 0x9E46B0
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
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        const char* curRel = (const char*)((uintptr_t)g_exe + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET);
        if (curRel && _strnicmp(curRel, "save/", 5) == 0 && curRel[5]) {
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
    if (g_state != SESSION_IDLE) return;
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
    CreateThread(nullptr, 0, HostSessionThread, nullptr, 0, nullptr);
}

void SaveSync_StartClient(const char* hostIp)
{
    if (g_state != SESSION_IDLE) return;
    TwoStates_MountSaveBank(true);
    g_disconnectRequested = false;
    if (hostIp && hostIp[0]) {
        strncpy_s(g_targetIp, hostIp, _TRUNCATE);
        TwoStates_SetSavedJoinAddress(g_targetIp);
        TwoStates_SaveSettings();
    }
    g_isHost = false;
    g_hadPartnerInSession = false;
    SetLastPartnerPeer("");
    g_resyncing = false;
    g_sessionStartTick = GetTickCount64();
    SetSessionError("");
    SetState(SESSION_CLIENT_JOINING);
    CreateThread(nullptr, 0, ClientSessionThread, nullptr, 0, nullptr);
}

void SaveSync_RequestDisconnect()
{
    if (g_state == SESSION_IDLE) {
        // Channels opened by hand from the Debug window.
        ResetChannels();
        return;
    }
    SaveStatus(g_isHost ? "[HOST] stopping..." : "[CLIENT] disconnecting...");
    g_disconnectRequested = true;
    // Wakes a blocking recv on the join connection (closed by its owner).
    SOCKET s = g_activeSock;
    if (s != INVALID_SOCKET) NetworkShutdownSocket(s);
    ResetChannels();
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

void SaveSync_Tick()
{
    SteamP2P_FrameTick();
    DiagLog_FrameTick();
    if (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded())) {
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
void SaveSync_SetLockstep(bool on) { g_lockstepChosen = on; }
void SaveSync_SetDlcCheck(bool on) { g_dlcCheck = on; }
bool SaveSync_GetDlcCheck() { return g_dlcCheck; }
bool SaveSync_GetLockstep() { return g_lockstepChosen; }
bool SaveSync_IsLockstepSession() { return g_lockstep && g_state != SESSION_IDLE; }
bool SaveSync_IsBusy()           { return g_state != SESSION_IDLE; }
bool SaveSync_IsHostRole()       { return g_isHost; }
ULONGLONG SaveSync_GetSessionAgeMs() { return g_state == SESSION_IDLE ? 0 : GetTickCount64() - g_sessionStartTick; }
bool SaveSync_IsHostAwaitingClient() { return g_state == SESSION_HOST_LISTENING; }
const char* SaveSync_GetTargetIp() { return g_targetIp; }

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
    return g_lastPartnerPeer;
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
    SafeWritePod<unsigned char>(base + 0x9D4EE0 + 12, 1);
    SafeWritePod<unsigned char>(base + 0xA54E90, 0);
    SafeWritePod<unsigned char>(base + 0xA54E91, 0);

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

// The returned buffers are only ever rewritten whole by the session thread;
// the UI reads them once per frame.
const char* SaveSync_GetPeerIp()    { return g_peerIp; }
const char* SaveSync_GetLastError() { return g_lastError; }

bool SaveSync_IsDualTestbed()
{
    static int s_cached = -1;
    if (s_cached < 0) {
        char envBuf[32] = {};
        bool dual = (GetEnvironmentVariableA("REPUBLICMP_ROLE", envBuf, sizeof(envBuf)) > 0);
        if (!dual) {
            const char* cmdLine = GetCommandLineA();
            if (cmdLine && (strstr(cmdLine, "--mp-host") || strstr(cmdLine, "--mp-left") ||
                            strstr(cmdLine, "--mp-client") || strstr(cmdLine, "--mp-right"))) {
                dual = true;
            }
        }
        if (!dual) {
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            dual = (strstr(exePath, "test-host") != nullptr || strstr(exePath, "test-client") != nullptr);
        }
        s_cached = dual ? 1 : 0;
    }
    return s_cached == 1;
}
