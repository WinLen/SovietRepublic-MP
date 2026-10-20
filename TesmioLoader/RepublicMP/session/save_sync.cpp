// SPDX-License-Identifier: GPL-3.0-only
// See save_sync.h.
#include "save_sync.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "game_speed.h"
#include "sync_check.h"
#include "two_states.h"
#include "mod_sync.h"
#include "../game/save_load.h"
#include "../save/save_zip.h"
#include "../ui/ui_log.h"
#include "../ui/sync_block.h"
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

#pragma comment(lib, "User32.lib")

// Fixed local name for the downloaded zip and its extraction folder on the client.
#define SAVE_SYNC_CLIENT_STAGING_NAME "mp_syncsave_incoming"

// How long the live sync channels get to connect after the world is loaded.
#define CHANNELS_CONNECT_TIMEOUT_MS 30000
#define CHANNELS_RETRY_MS 1000
// A single dropped channel (network hiccup, Debug window Disconnect) gets
// this long to reconnect before the session is given up.
#define CHANNEL_RECONNECT_TIMEOUT_MS 20000

#define JOIN_STEPS 7

void SaveSync_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile SessionState g_state = SESSION_IDLE;
static volatile bool g_isHost = false;
static char g_targetIp[64] = SAVE_SYNC_TEST_HOST_IP; // client target - overridable from the UI
static char g_peerIp[64] = "";
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

static void SetState(SessionState s) { g_state = s; }

static void SetPeerIp(const char* ip)
{
    EnterCriticalSection(&g_lock);
    strncpy_s(g_peerIp, ip ? ip : "", _TRUNCATE);
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
        _snprintf_s(why, whySize, _TRUNCATE, "the other side is not a compatible RepublicMP (old build?)");
        return false;
    }
    char peerVersion[17] = {};
    memcpy(peerVersion, peer.version, 16);
    if (peer.protocol != REPUBLICMP_PROTOCOL || strcmp(peerVersion, REPUBLICMP_VERSION) != 0 ||
        peer.dllCrc != SaveSync_GetLocalDllCrc()) {
        _snprintf_s(why, whySize, _TRUNCATE,
                    "different RepublicMP builds - yours %s (protocol %d, crc %08X), theirs %s (protocol %d, crc %08X). "
                    "Both players need the same RepublicMP.dll",
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
    if (!BuildSync_IsConnected()) strcat_s(out, outSize, "buildings ");
    if (!RoadSync_IsConnected()) strcat_s(out, outSize, "roads ");
    if (!TerrainSync_IsConnected()) strcat_s(out, outSize, "terrain ");
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
        if (LockstepNet_MsSinceReceived() > LOCKSTEP_SILENCE_MS) return "the other game stopped answering";
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
    return g_disconnectRequested ? nullptr : "the lockstep connection closed";
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
    SyncBlock_SetPercent(percent);
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

// Both sides decide alike: false refuses the join (why says what differs).
static bool DlcsMatch(const SessionDlcs& mine, const SessionDlcs& theirs, bool host, char* why, size_t whySize)
{
    char peer[sizeof(theirs.list) + 1] = {};
    memcpy(peer, theirs.list, sizeof(theirs.list));
    const char* hostList = host ? mine.list : peer;
    const char* playerList = host ? peer : mine.list;
    Logf("republicmp  SESSION: DLCs - host [%s], player [%s]", hostList[0] ? hostList : "none", playerList[0] ? playerList : "none");
    if (strcmp(mine.list, peer) == 0) return true;
    bool refuse = mine.check && theirs.check;
    _snprintf_s(why, whySize, _TRUNCATE,
                "the DLCs differ - host [%s], player [%s]. DLC maps, buildings and vehicles cannot be shown without the DLC%s",
                hostList[0] ? hostList : "none", playerList[0] ? playerList : "none",
                refuse ? "; install the same DLCs, or untick \"Check DLCs\" on either PC to play a map that needs none"
                       : " (the DLC check is off - playing anyway)");
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

// One join, from the accepted connection to the lifted sync screen. The
// caller's session loop decides what happens next. Returns true when the
// player is in the game with all channels up.
static bool RunHostJoin(SOCKET sock)
{
    g_activeSock = sock;
    SyncBlock_SetActive(true, g_resyncing ? "Re-syncing the world" : "A player is connecting");
    EnsureGamePaused();
    bool joined = false;
    char madeSaveDir[MAX_PATH] = {}; // cleaned up below
    char madeZip[MAX_PATH] = {};

    do {
        // 1. Versions.
        HostStep(sock, 1, "A player is connecting - checking versions");
        SessionHello mine, theirs;
        MakeHello(&mine);
        TwoStates_PrepareHostSave(g_resyncing, mine.stateRub, mine.stateUsd);
        if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
            SetSessionError("the player dropped during the version check");
            break;
        }
        NetworkSendExact(sock, &mine, sizeof(mine)); // always, so the client can show what differs
        char why[256];
        if (!HelloMatches(theirs, why, sizeof(why))) {
            SetSessionError("join refused: %s", why);
            SaveStatus("[HOST] join refused: %s", why);
            break;
        }
        SessionDlcs myDlcs, theirDlcs;
        MakeDlcs(&myDlcs);
        if (!NetworkSendExact(sock, &myDlcs, sizeof(myDlcs)) || !NetworkRecvExact(sock, &theirDlcs, sizeof(theirDlcs)) ||
            memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
            SetSessionError("the player dropped during the DLC check");
            break;
        }
        char dlcWhy[512];
        if (!DlcsMatch(myDlcs, theirDlcs, true, dlcWhy, sizeof(dlcWhy))) {
            SetSessionError("join refused: %s", dlcWhy);
            SaveStatus("[HOST] join refused: %s", dlcWhy);
            break;
        }

        // 2. Save.
        char liveSaveName[64];
        GenerateRandomSaveName(liveSaveName, sizeof(liveSaveName));
        char saveDir[MAX_PATH] = {};
        if (!GetGameSaveDir(saveDir, sizeof(saveDir), liveSaveName)) {
            SetSessionError("couldn't build the save path");
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't save the world");
            break;
        }
        HostStep(sock, 2, "Saving the world");
        SendMsg(sock, MSG_STEP, 2, -1, "saving the world");
        // The engine's save writer does not create the destination folder.
        if (!CreateDirectoryA(saveDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            SetSessionError("couldn't create the save folder (error %lu)", GetLastError());
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

        // 2b. Synchronize Workshop/local mods and TesmioLoader gameplay plugins.
        HostStep(sock, 2, "Synchronizing mods & plugins", 0);
        SendMsg(sock, MSG_MOD_SYNC, 2, 0, "synchronizing mods & plugins");
        char modWhy[256] = {};
        if (!ModSync_HostHandshake(sock, saveDir, modWhy, sizeof(modWhy))) {
            SetSessionError("mod sync failed: %s", modWhy);
            SaveStatus("[HOST] mod sync failed: %s", modWhy);
            break;
        }

        // 3. Pack.
        char zipPath[MAX_PATH] = {};
        _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
        HostStep(sock, 3, "Packing the save", 0);
        SendMsg(sock, MSG_STEP, 3, 0, "packing the save");
        ProgressCtx packCtx = { sock, 3, "packing the save", -100 };
        strncpy_s(madeZip, zipPath, _TRUNCATE);
        if (!ZipSaveDirectory(saveDir, zipPath, ZipProgressCb, &packCtx)) {
            SetSessionError("packing the save failed");
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't pack the save");
            break;
        }
        if (g_disconnectRequested) {
            SendMsg(sock, MSG_ABORT, 0, -1, "host stopped hosting");
            break;
        }

        // 4. Send.
        HostStep(sock, 4, "Sending the world to the player", 0);
        SendMsg(sock, MSG_FILE_FOLLOWS, 4, -1, "");
        ProgressCtx sendCtx = { INVALID_SOCKET, 4, nullptr, 0 };
        if (!NetworkSendFile(sock, zipPath, NetworkProgressCb, &sendCtx)) {
            SetSessionError("sending the save failed - connection lost");
            break;
        }

        // Lockstep: this PC loads the same save while the player unpacks and
        // loads its copy; the player's step messages wait in the socket.
        if (g_lockstep) {
            HostStep(sock, 5, "Loading the shared world while the player loads it");
            if (!Determinism_BeginSession(true)) {
                SetSessionError("lockstep is not available - see the log (determinism layer)");
                break;
            }
            TwoStates_OnSessionJoined(true, mine.twoStatesMode, mine.borderHalfWidth, mine.expandBorders != 0,
                                      mine.stateRub, mine.stateUsd);
            TriggerLoadSave(liveSaveName);
            if (!WaitForLoadToFinish(0, &g_disconnectRequested) || !Determinism_SessionRunning()) {
                if (!g_disconnectRequested) SetSessionError("loading the shared world failed");
                TwoStates_OnSessionEnded();
                ResetChannels();
                break;
            }
        }

        // 5-6. The player unpacks and loads; its steps arrive as messages.
        HostStep(sock, 5, "Waiting for the player - unpacking", 0);
        bool ready = false;
        for (;;) {
            SessionMsg m;
            if (!RecvMsg(sock, &m)) {
                SetSessionError("the player dropped while loading");
                break;
            }
            if (m.code == MSG_READY) { ready = true; break; }
            if (m.code == MSG_ABORT) {
                SetSessionError("the player gave up: %s", m.text);
                SaveStatus("[HOST] the player gave up: %s", m.text);
                break;
            }
            if (m.code == MSG_STEP) {
                char text[96];
                _snprintf_s(text, sizeof(text), _TRUNCATE, "Waiting for the player - %s", m.text);
                if (m.step == 5) SyncBlock_SetStep(5, JOIN_STEPS, text, m.percent);
                else HostStep(sock, m.step, text, m.percent);
            }
        }
        if (!ready) {
            if (g_lockstep) {
                TwoStates_OnSessionEnded();
                ResetChannels();
            }
            break;
        }

        // 7. Lockstep: both loaded the same save; the channel connects.
        if (g_lockstep) {
            HostStep(sock, 7, "Waiting for the player's lockstep connection");
            if (!LockstepNet_Host(LOCKSTEP_CONNECT_TIMEOUT_MS, &g_disconnectRequested)) {
                if (!g_disconnectRequested) SetSessionError("the player's lockstep connection didn't arrive - TCP port %d must reach the host", LOCKSTEP_PORT);
                TwoStates_OnSessionEnded();
                ResetChannels();
                break;
            }
            joined = true;
            break;
        }

        // 7. Live sync channels.
        HostStep(sock, 7, "Starting live sync");
        StartChannelsHost();
        if (!WaitForChannels(false)) {
            if (!g_disconnectRequested) SetSessionError("the live sync channels didn't connect - TCP ports %d-%d must reach the host too (forward them like port %d)", BUILD_SYNC_PORT, TERRAIN_SYNC_PORT, SAVE_SYNC_PORT);
            ResetChannels();
            break;
        }
        joined = true;
        TwoStates_OnSessionJoined(true, mine.twoStatesMode, mine.borderHalfWidth, mine.expandBorders != 0,
                                  mine.stateRub, mine.stateUsd);
    } while (false);

    // The zip only existed to be sent.
    if (madeZip[0]) DeleteFileA(madeZip);
    if (madeSaveDir[0]) KeepOnlyNewestHostSave(madeSaveDir);

    g_activeSock = INVALID_SOCKET;
    NetworkCloseSocket(sock);
    SyncBlock_SetActive(false, nullptr);
    // A joined player starts paused on both sides - either player resumes
    // when both are ready (issue #18); the speed sync carries it over.
    if (!joined) EnsureGameUnpaused();
    return joined;
}

static void RunHostSession()
{
    if (!NetworkInit()) {
        SetSessionError("network init failed");
        return;
    }

    SteamP2P_SetHostingPresence(true);
    bool rejoin = false; // a re-sync: the player is expected back right away
    while (!g_disconnectRequested) {
        SetState(SESSION_HOST_LISTENING);
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
                SetSessionError("the player didn't come back for the re-sync");
                SaveStatus("[HOST] the player didn't come back for the re-sync - waiting for a player again");
                continue;
            }
            SetSessionError("couldn't listen on port %d (already in use?)", SAVE_SYNC_PORT);
            break;
        }

        char peer[64];
        PeerIpOf(sock, peer, sizeof(peer));
        SetPeerIp(peer);
        SetState(SESSION_HOST_JOINING);
        SetSessionError("");
        SaveStatus(rejoin ? "[HOST] %s is back - re-syncing, both games are paused until it is loaded"
                          : "[HOST] %s is joining - both games are paused until it is loaded", peer);

        g_lockstep = g_lockstepChosen;
        bool joined = RunHostJoin(sock);
        rejoin = false;
        g_resyncing = false;
        if (!joined) {
            if (g_disconnectRequested) break;
            SaveStatus("[HOST] the join failed (%s) - waiting for a player again", SaveSync_GetLastError());
            continue;
        }
        g_lastWorldSentTick = GetTickCount64();
        g_resyncRequested = false; // one asked for during the join is already done

        SetState(SESSION_HOST_IN_GAME);
        SaveStatus("[HOST] %s is in the game - live sync running, paused until either player resumes", peer);
        const char* why = MonitorLiveSession(false);
        ResetChannels();
        if (!why) break; // Stop hosting
        if (why == kResyncReason) {
            rejoin = true;
            SaveStatus("[HOST] re-syncing - waiting for %s to reconnect", peer);
            continue;
        }
        TwoStates_OnSessionEnded();
        SetSessionError("%s left: %s", peer, why);
        SaveStatus("[HOST] %s left (%s) - waiting for a player again", peer, why);
    }

    SteamP2P_SetHostingPresence(false);
    TwoStates_OnSessionEnded();
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

// Returns true when the player is in the host's game with all channels up.
// rejoin: coming back for a re-sync - the host may need a moment to listen
// again, so refused connects are retried.
static bool RunClientJoin(bool rejoin)
{
    g_resyncAnnounced = false;
    SyncBlock_SetActive(true, rejoin ? "Re-syncing with the host" : "Connecting to the host");
    EnsureGamePaused();

    SOCKET sock = INVALID_SOCKET;
    bool joined = false;

    do {
        char saveDir[MAX_PATH] = {};
        if (!GetGameSaveDir(saveDir, sizeof(saveDir), SAVE_SYNC_CLIENT_STAGING_NAME)) {
            SetSessionError("couldn't build the save path");
            break;
        }
        char zipPath[MAX_PATH] = {};
        _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
        char destDir[MAX_PATH] = {};
        _snprintf_s(destDir, sizeof(destDir), _TRUNCATE, "%s%s", saveDir, SAVE_SYNC_RECEIVED_SUFFIX);

        // 1. Connect + versions.
        ClientStep(1, "Connecting to the host - checking versions");
        if (!NetworkInit()) {
            SetSessionError("network init failed");
            break;
        }
        ULONGLONG connectStart = GetTickCount64();
        for (;;) {
            sock = NetworkConnect(g_targetIp, SAVE_SYNC_PORT, 15000, 0, &g_disconnectRequested);
            if (sock != INVALID_SOCKET || !rejoin || g_disconnectRequested) break;
            if (GetTickCount64() - connectStart > RESYNC_REJOIN_TIMEOUT_MS) break;
            Sleep(RESYNC_RETRY_MS);
        }
        if (sock == INVALID_SOCKET) {
            if (!g_disconnectRequested)
                SetSessionError("couldn't reach %s:%d - is the host hosting (and not already playing with someone)? is the IP right?", g_targetIp, SAVE_SYNC_PORT);
            break;
        }
        g_activeSock = sock;
        // A Disconnect that came while connecting found no socket to shut
        // down; without this the whole world would still be downloaded.
        if (g_disconnectRequested) break;
        SessionHello mine, theirs;
        MakeHello(&mine);
        if (!NetworkSendExact(sock, &mine, sizeof(mine)) || !NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
            SetSessionError("the host dropped during the version check (old build, or already has a player?)");
            break;
        }
        char why[256];
        if (!HelloMatches(theirs, why, sizeof(why))) {
            SetSessionError("join refused: %s", why);
            SaveStatus("[CLIENT] join refused: %s", why);
            break;
        }
        SessionDlcs myDlcs, theirDlcs;
        MakeDlcs(&myDlcs);
        if (!NetworkSendExact(sock, &myDlcs, sizeof(myDlcs)) || !NetworkRecvExact(sock, &theirDlcs, sizeof(theirDlcs)) ||
            memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
            SetSessionError("the host dropped during the DLC check");
            break;
        }
        char dlcWhy[512];
        if (!DlcsMatch(myDlcs, theirDlcs, false, dlcWhy, sizeof(dlcWhy))) {
            SetSessionError("join refused: %s", dlcWhy);
            SaveStatus("[CLIENT] join refused: %s", dlcWhy);
            break;
        }
        g_lockstep = theirs.lockstep != 0;
        if (g_lockstep) SaveStatus("[CLIENT] the host runs this session in lockstep");

        // 2-3. The host saves and packs; its steps arrive as messages.
        ClientStep(2, "Waiting for the host - saving the world");
        bool fileFollows = false;
        for (;;) {
            SessionMsg m;
            if (!RecvMsg(sock, &m)) {
                SetSessionError("the host dropped while preparing the world");
                break;
            }
            if (m.code == MSG_FILE_FOLLOWS) { fileFollows = true; break; }
            if (m.code == MSG_ABORT) {
                SetSessionError("the host gave up: %s", m.text);
                break;
            }
            if (m.code == MSG_MOD_SYNC) {
                ClientStep(2, "Synchronizing mods & plugins", 0);
                char modWhy[256] = {};
                if (!ModSync_ClientHandshake(sock, modWhy, sizeof(modWhy))) {
                    SetSessionError("mod sync failed: %s", modWhy);
                    SaveStatus("[CLIENT] mod sync failed: %s", modWhy);
                    break;
                }
                continue;
            }
            if (m.code == MSG_STEP) {
                char text[96];
                _snprintf_s(text, sizeof(text), _TRUNCATE, "Waiting for the host - %s", m.text);
                SyncBlock_SetStep(m.step, JOIN_STEPS, text, m.percent);
            }
        }
        if (!fileFollows) break;

        // 4. Receive.
        ClientStep(4, "Receiving the world", 0);
        ProgressCtx recvCtx = { INVALID_SOCKET, 4, nullptr, 0 };
        if (!NetworkReceiveFile(sock, zipPath, NetworkProgressCb, &recvCtx)) {
            SetSessionError("receiving the world failed - connection lost");
            break;
        }

        // 5. Unpack (relayed to the host).
        ClientStep(5, "Unpacking", 0);
        SendMsg(sock, MSG_STEP, 5, 0, "unpacking");
        ProgressCtx unpackCtx = { sock, 5, "unpacking", -100 };
        if (!UnzipToDirectory(zipPath, destDir, ZipProgressCb, &unpackCtx)) {
            SetSessionError("unpacking the world failed");
            SendMsg(sock, MSG_ABORT, 0, -1, "unpacking failed");
            break;
        }
        if (g_disconnectRequested) {
            SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled");
            break;
        }

        // 6. Load.
        ClientStep(6, "Loading the world");
        SendMsg(sock, MSG_STEP, 6, -1, "loading the world");
        char receivedSaveName[MAX_PATH] = {};
        _snprintf_s(receivedSaveName, sizeof(receivedSaveName), _TRUNCATE, "%s%s",
                    SAVE_SYNC_CLIENT_STAGING_NAME, SAVE_SYNC_RECEIVED_SUFFIX);
        if (g_lockstep) {
            if (!Determinism_BeginSession(false)) {
                SetSessionError("lockstep is not available - see the log (determinism layer)");
                SendMsg(sock, MSG_ABORT, 0, -1, "lockstep is not available on the player");
                break;
            }
            TwoStates_OnSessionJoined(false, theirs.twoStatesMode, theirs.borderHalfWidth, theirs.expandBorders != 0,
                                      theirs.stateRub, theirs.stateUsd);
        }
        TriggerLoadSave(receivedSaveName);
        if (!WaitForLoadToFinish(0, &g_disconnectRequested, JoinHostGone, &sock)) {
            if (g_disconnectRequested) SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled while loading");
            else SetSessionError("the host dropped while the world was loading");
            if (g_lockstep) TwoStates_OnSessionEnded();
            break;
        }

        // 7. Ready -> the host starts listening on the channels; connect them.
        if (g_lockstep) {
            ClientStep(7, "Waiting for the host to load the shared world");
            SendMsg(sock, MSG_READY, 7, -1, "");
            if (!Determinism_SessionRunning()) {
                SetSessionError("loading the shared world failed");
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
                if (hostGone) SetSessionError("the host dropped while loading the shared world");
                else if (!g_disconnectRequested) SetSessionError("couldn't open the lockstep connection to %s:%d", g_targetIp, LOCKSTEP_PORT);
                TwoStates_OnSessionEnded();
                break;
            }
            joined = true;
            break;
        }
        ClientStep(7, "Starting live sync");
        SendMsg(sock, MSG_READY, 7, -1, "");
        if (!WaitForChannels(true)) {
            if (!g_disconnectRequested) SetSessionError("the live sync channels didn't connect - TCP ports %d-%d of the host are not reachable (only port %d is forwarded?)", BUILD_SYNC_PORT, TERRAIN_SYNC_PORT, SAVE_SYNC_PORT);
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
            SetSessionError("disconnected from the host: %s", why);
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
    SetState(SESSION_IDLE);
    return 0;
}

static DWORD WINAPI ClientSessionThread(LPVOID)
{
    RunClientSession();
    SetPeerIp("");
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
    __try {
        return *(__int64*)((uintptr_t)g_exe + WORLD_ROAD_NETWORK_RVA) != 0 &&
               *(volatile char*)((uintptr_t)g_exe + LOAD_TRIGGER_DAT_RVA) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeGetCurrentWorldSaveRel(char* outRel, size_t outSize)
{
    __try {
        const char* curRel = (const char*)((uintptr_t)g_exe + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET);
        if (curRel && _strnicmp(curRel, "save/", 5) == 0 && curRel[5]) {
            strncpy_s(outRel, outSize, curRel, _TRUNCATE);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void SaveSync_StartHost()
{
    if (g_state != SESSION_IDLE) return;
    if (!SaveSync_IsWorldLoaded()) {
        // A joining player gets the host's world: there has to be one.
        SaveStatus("[HOST] no world loaded - load a city first, then host");
        SetSessionError("load a city before hosting");
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
    g_resyncRequested = false;
    g_resyncing = false;
    g_sessionStartTick = GetTickCount64();
    SetSessionError("");
    SetState(SESSION_HOST_LISTENING);
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
    SetSessionError(host ? "hosting stopped: you left the world" : "disconnected: you left the world");
}

void SaveSync_Tick()
{
    SteamP2P_FrameTick();
    DiagLog_FrameTick();
    if (g_state == SESSION_IDLE) {
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

// The returned buffers are only ever rewritten whole by the session thread;
// the UI reads them once per frame.
const char* SaveSync_GetPeerIp()    { return g_peerIp; }
const char* SaveSync_GetLastError() { return g_lastError; }
