// SPDX-License-Identifier: GPL-3.0-only
// See lockstep_net.h.
#include "lockstep_net.h"
#include "commands.h"
#include "../core/diag_log.h"
#include "../session/two_states.h"
#include "../determinism/determinism.h"
#include "../world_sync/vehicles/vehicle_soft_sync.h"
#include "../transport/network.h"
#include <deque>
#include <vector>
#include <string.h>
#include <stdio.h>

static void Locks();

void LockstepNet_Bind(const TsmHost* host)
{
    TsmBind(host);
    Locks();
}

enum LockstepMsgType {
    LS_GRANT = 1, // host -> player: LsGrant
    LS_CMD = 2,   // player -> host: LsCommandHead + payload
    LS_CMDS = 3,  // host -> player: LsCommandHead + payload
    LS_ACK = 4,   // player -> host: unsigned long long completed
    LS_HASH = 5,  // player -> host: LsHash
    LS_RESYNC = 6, // host -> player: the world is sent again (no payload)
    LS_PING = 11,  // either way: still here (LsPing; a request is answered with the same time)
    LS_DESYNC = 12, // host -> player: the first state difference (text)
    LS_CURSOR = 13, // either way: partner 3D cursor + optional map ping (LsCursor)
    LS_CLIENT_LOGS = 14, // player -> host: compressed client diagnostic log bundle
    LS_PAUSE = 7, // player -> host: a pause toggle
    LS_SPEED = 8, // player -> host: int speed
    LS_STATE = 9, // host -> player: LsState
    LS_BYE = 10,  // either way: leaving
};

#pragma pack(push, 1)
struct LsHeader { unsigned type; unsigned size; };
struct LsGrant { unsigned long long tick; int speed; };
struct LsCommandHead { unsigned long long afterTick; int kind; };
struct LsHash { unsigned long long tick; StateSummary s; };
struct LsState { int paused; int speed; };
struct LsPing { unsigned long long sentMs; int reply; };
struct LsCursor { float x, y, z; float camX, camZ; unsigned char isPing; };
#pragma pack(pop)

#define LS_MAX_PAYLOAD (1u << 20)
#define LS_HASH_KEEP 32

struct LsMessage {
    unsigned type;
    std::vector<BYTE> data;
};

static SOCKET g_sock = INVALID_SOCKET;
static volatile bool g_connected = false;
static bool g_isHost = false;
static HANDLE g_recvThread = nullptr;
static CRITICAL_SECTION g_inLock, g_sendLock;
static std::deque<LsMessage> g_inbox;
static PartnerCursorInfo g_partnerCursor = {};

// Host side.
static volatile unsigned long long g_playerCompleted = 0;
static int g_pauseToggles = 0;
static int g_speedRequest = 0;
struct OwnHash { unsigned long long tick; StateSummary s; bool valid; };
static OwnHash g_ownHashes[LS_HASH_KEEP];
static LsHash g_playerHashes[LS_HASH_KEEP];
static int g_playerHashCount = 0;
static char g_desync[256] = "";
static volatile unsigned long long g_lastMatched = 0; // the last tick both summaries matched
static int g_hardMismatchCount = 0;
static unsigned long long g_lastSoftDriftLogTick = 0;

// Player side.
static std::deque<LsGrant> g_grants;
static volatile bool g_resyncReceived = false; // set by the receive thread
static volatile ULONGLONG g_lastReceived = 0;    // GetTickCount64 of the last message
static bool g_hostStateKnown = false;
static LsState g_hostState = { 1, 1 };

// Both sides: the round trip of the last answered ping, the host's waits for the
// player (the admission window full) and the ticks granted so far.
static volatile ULONGLONG g_rttMs = 0;
static volatile bool g_rttKnown = false;
static volatile ULONGLONG g_lastStallMs = 0;
static volatile LONG g_stalls = 0;
static volatile unsigned long long g_lastGranted = 0;
static volatile LONG g_grantBacklog = 0; // the player's grants not run yet (for the interface)

static INIT_ONCE g_inLockOnce = INIT_ONCE_STATIC_INIT;
static INIT_ONCE g_sendLockOnce = INIT_ONCE_STATIC_INIT;

static void Locks()
{
    InitOnceExecuteOnce(&g_inLockOnce, [](PINIT_ONCE, PVOID p, PVOID*) -> BOOL {
        InitializeCriticalSection((CRITICAL_SECTION*)p);
        return TRUE;
    }, &g_inLock, nullptr);
    InitOnceExecuteOnce(&g_sendLockOnce, [](PINIT_ONCE, PVOID p, PVOID*) -> BOOL {
        InitializeCriticalSection((CRITICAL_SECTION*)p);
        return TRUE;
    }, &g_sendLock, nullptr);
}

static bool Send(unsigned type, const void* a, size_t na, const void* b = nullptr, size_t nb = 0)
{
    if (!g_connected) return false;
    LsHeader h = { type, (unsigned)(na + nb) };
    EnterCriticalSection(&g_sendLock);
    static std::vector<unsigned char> s_sendBuf;
    s_sendBuf.resize(sizeof(h) + na + nb);
    memcpy(s_sendBuf.data(), &h, sizeof(h));
    if (na) memcpy(s_sendBuf.data() + sizeof(h), a, na);
    if (nb) memcpy(s_sendBuf.data() + sizeof(h) + na, b, nb);
    bool ok = NetworkSendExact(g_sock, s_sendBuf.data(), s_sendBuf.size());
    LeaveCriticalSection(&g_sendLock);
    if (!ok && g_connected) {
        g_connected = false;
        Logf("republicmp  LOCKSTEP: send failed - the connection is lost");
    }
    return ok;
}

void LockstepNet_SendClientLogs(const char* reason)
{
    if (!g_connected || g_isHost || !TwoStates_GetSendClientLogs()) return;
    static ULONGLONG s_lastLogSendMs = 0;
    ULONGLONG now = GetTickCount64();
    bool isCrash = (reason && strcmp(reason, "CRASH") == 0);
    if (!isCrash && s_lastLogSendMs != 0 && (now - s_lastLogSendMs) < 5000ULL) return;
    s_lastLogSendMs = now;

    std::vector<unsigned char> bundle;
    if (DiagLog_BuildClientLogBundle(bundle, reason)) {
        Send(LS_CLIENT_LOGS, bundle.data(), bundle.size());
    }
}

static DWORD WINAPI RecvThread(LPVOID)
{
    while (g_connected) {
        LsHeader h;
        if (!NetworkRecvExact(g_sock, &h, sizeof(h)) || h.size > LS_MAX_PAYLOAD) break;
        LsMessage m;
        m.type = h.type;
        m.data.resize(h.size);
        if (h.size && !NetworkRecvExact(g_sock, m.data.data(), h.size)) break;
        g_lastReceived = GetTickCount64();
        if (h.type == LS_PING) {
            if (m.data.size() == sizeof(LsPing)) {
                LsPing p = *(const LsPing*)m.data.data();
                if (p.reply) {
                    ULONGLONG now = GetTickCount64();
                    g_rttMs = now >= p.sentMs ? now - p.sentMs : 0;
                    g_rttKnown = true;
                } else {
                    p.reply = 1;
                    Send(LS_PING, &p, sizeof(p));
                }
            }
            continue;
        }
        if (h.type == LS_CURSOR) {
            if (m.data.size() == sizeof(LsCursor)) {
                LsCursor c = *(const LsCursor*)m.data.data();
                ULONGLONG now = GetTickCount64();
                EnterCriticalSection(&g_inLock);
                g_partnerCursor.valid = true;
                g_partnerCursor.x = c.x;
                g_partnerCursor.y = c.y;
                g_partnerCursor.z = c.z;
                g_partnerCursor.camX = c.camX;
                g_partnerCursor.camZ = c.camZ;
                g_partnerCursor.lastUpdateMs = now;
                if (c.isPing) {
                    g_partnerCursor.hasPing = true;
                    g_partnerCursor.pingX = c.x;
                    g_partnerCursor.pingY = c.y;
                    g_partnerCursor.pingZ = c.z;
                    g_partnerCursor.pingReceivedMs = now;
                    g_partnerCursor.pingSeq++;
                }
                LeaveCriticalSection(&g_inLock);
            }
            continue;
        }
        if (h.type == LS_CLIENT_LOGS) {
            if (g_isHost && !m.data.empty()) {
                DiagLog_SaveRemoteClientLogBundle(m.data.data(), m.data.size());
            }
            continue;
        }
        if (h.type == LS_DESYNC) {
            size_t n = m.data.size() < sizeof(g_desync) - 1 ? m.data.size() : sizeof(g_desync) - 1;
            memcpy(g_desync, m.data.data(), n);
            g_desync[n] = 0;
            Logf("republicmp  LOCKSTEP: the host reports - %s", g_desync);
            unsigned long long tick = 0;
            if (sscanf_s(g_desync, "the games differ after tick %llu", &tick) == 1) Determinism_DumpMoneyTrace(tick);
            LockstepNet_SendClientLogs("DESYNC");
            continue;
        }
        if (h.type == LS_BYE) {
            Logf("republicmp  LOCKSTEP: the other player left");
            break;
        }
        // Noted here, not in the game thread's poll: the host closes the
        // connection right after it.
        if (h.type == LS_RESYNC) {
            g_resyncReceived = true;
            continue;
        }
        EnterCriticalSection(&g_inLock);
        g_inbox.push_back(std::move(m));
        LeaveCriticalSection(&g_inLock);
    }
    if (g_connected) Logf("republicmp  LOCKSTEP: the connection is closed");
    g_connected = false;
    return 0;
}

static void ResetState()
{
    EnterCriticalSection(&g_inLock);
    g_inbox.clear();
    g_grants.clear();
    memset(&g_partnerCursor, 0, sizeof(g_partnerCursor));
    g_playerCompleted = 0;
    g_pauseToggles = 0;
    g_speedRequest = 0;
    memset(g_ownHashes, 0, sizeof(g_ownHashes));
    g_playerHashCount = 0;
    g_desync[0] = 0;
    g_lastMatched = 0;
    g_hardMismatchCount = 0;
    g_lastSoftDriftLogTick = 0;
    g_hostStateKnown = false;
    g_hostState.paused = 1;
    g_hostState.speed = 1;
    g_resyncReceived = false;
    g_lastReceived = GetTickCount64();
    g_rttMs = 0;
    g_rttKnown = false;
    g_lastStallMs = 0;
    g_stalls = 0;
    g_lastGranted = 0;
    g_grantBacklog = 0;
    LeaveCriticalSection(&g_inLock);
}

static bool Started(SOCKET s, bool host)
{
    if (s == INVALID_SOCKET) return false;
    ResetState();
    g_sock = s;
    g_isHost = host;
    g_connected = true;
    g_recvThread = CreateThread(nullptr, 0, RecvThread, nullptr, 0, nullptr);
    Logf("republicmp  LOCKSTEP: connected as %s", host ? "host" : "player");
    return true;
}

bool LockstepNet_Host(DWORD timeoutMs, volatile bool* cancel)
{
    Locks();
    LockstepNet_Close();
    if (!NetworkInit()) return false;
    return Started(NetworkHostAcceptOne(LOCKSTEP_PORT, timeoutMs, 0, cancel), true);
}

bool LockstepNet_Join(const char* hostIp, DWORD timeoutMs, volatile bool* cancel)
{
    Locks();
    LockstepNet_Close();
    if (!NetworkInit()) return false;
    ULONGLONG start = GetTickCount64();
    for (;;) {
        SOCKET s = NetworkConnect(hostIp, LOCKSTEP_PORT, 5000, 0, cancel);
        if (s != INVALID_SOCKET) return Started(s, false);
        if ((cancel && *cancel) || GetTickCount64() - start > timeoutMs) return false;
        Sleep(500); // the host listens once its own load is done
    }
}

void LockstepNet_Close()
{
    Locks();
    if (g_sock != INVALID_SOCKET && g_connected) {
        if (!g_isHost) LockstepNet_SendClientLogs("DISCONNECT");
        Send(LS_BYE, nullptr, 0);
    }
    SOCKET s = (SOCKET)InterlockedExchange64((volatile LONG64*)&g_sock, (LONG64)INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        g_connected = false;
        NetworkCloseSocket(s);
    }
    HANDLE h = InterlockedExchangePointer(&g_recvThread, nullptr);
    if (h) {
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
}

bool LockstepNet_IsConnected() { return g_connected; }
bool LockstepNet_IsHost() { return g_isHost; }
const char* LockstepNet_Desync() { return g_desync; }
unsigned long long LockstepNet_LastMatchedTick() { return g_lastMatched; }

// ---------------------------------------------------------------------------
// State comparison (host)
// ---------------------------------------------------------------------------

static void Compare(const LsHash& theirs)
{
    if (g_desync[0]) return;
    const OwnHash* mine = nullptr;
    for (const OwnHash& h : g_ownHashes)
        if (h.valid && h.tick == theirs.tick) mine = &h;
    if (!mine) {
        if (g_playerHashCount < LS_HASH_KEEP) g_playerHashes[g_playerHashCount++] = theirs;
        return;
    }
    const StateSummary& a = mine->s;
    const StateSummary& b = theirs.s;

    bool vehLogicalDiff = (a.hashVehicles != b.hashVehicles);
    bool vehPosDiff = (a.hashVehiclesPos != b.hashVehiclesPos);
    bool citDiff = (a.hashCitizens != b.hashCitizens);
    if (vehLogicalDiff || vehPosDiff || citDiff) {
        VehicleSoftSync_RequestImmediate();
        if (theirs.tick >= g_lastSoftDriftLogTick + 300) {
            g_lastSoftDriftLogTick = theirs.tick;
            Logf("republicmp  SOFT_SYNC: tick %llu in-transit drift (vehLogic=%d vehPos=%d cit=%d, vehicles %d/%d cargo %.2f/%.2f citizens %d/%d age %.1f/%.1f) - healing via LS_CMD_VEHICLE_SOFT_SYNC",
                 theirs.tick, (int)vehLogicalDiff, (int)vehPosDiff, (int)citDiff,
                 a.vehicles, b.vehicles, a.cargoTotal, b.cargoTotal,
                 a.citizens, b.citizens, a.ageTotal, b.ageTotal);
        }
    }

    char what[160] = "";
    if (a.hashClock != b.hashClock) strcat_s(what, "clock/money ");
    if (a.hashBuildings != b.hashBuildings) strcat_s(what, "buildings ");
    if (!what[0]) {
        g_hardMismatchCount = 0;
        g_lastMatched = theirs.tick;
        return;
    }

    VehicleSoftSync_RequestImmediate();
    if (++g_hardMismatchCount < 3) {
        Logf("republicmp  LOCKSTEP: transient %sdifference after tick %llu (%d/3) - awaiting soft sync convergence (prod %.2f/%.2f storage %.2f/%.2f cargo %.2f/%.2f rub %.2f/%.2f usd %.2f/%.2f)",
             what, theirs.tick, g_hardMismatchCount,
             a.productionTotal, b.productionTotal, a.storageTotal, b.storageTotal,
             a.cargoTotal, b.cargoTotal, a.rub, b.rub, a.usd, b.usd);
        return;
    }

    _snprintf_s(g_desync, sizeof(g_desync), _TRUNCATE, "the games differ after tick %llu: %s", theirs.tick, what);
    Logf("republicmp  LOCKSTEP: DESYNC - %s (host: buildings %d vehicles %d citizens %d, player: %d %d %d)", g_desync,
         a.buildings, a.vehicles, a.citizens, b.buildings, b.vehicles, b.citizens);
    Logf("republicmp  LOCKSTEP:   host day %d %.4f rubles %.4f dollars %.4f, player day %d %.4f rubles %.4f dollars %.4f", a.day,
         a.dayProgress, a.rub, a.usd, b.day, b.dayProgress, b.rub, b.usd);
    Logf("republicmp  LOCKSTEP:   totals host(prod=%.3f stor=%.3f cargo=%.3f age=%.1f) vs player(prod=%.3f stor=%.3f cargo=%.3f age=%.1f)",
         a.productionTotal, a.storageTotal, a.cargoTotal, a.ageTotal,
         b.productionTotal, b.storageTotal, b.cargoTotal, b.ageTotal);
    Logf("republicmp  LOCKSTEP:   hashes host(clk=%016llX bld=%016llX veh=%016llX vpos=%016llX cit=%016llX) vs player(clk=%016llX bld=%016llX veh=%016llX vpos=%016llX cit=%016llX)",
         a.hashClock, a.hashBuildings, a.hashVehicles, a.hashVehiclesPos, a.hashCitizens,
         b.hashClock, b.hashBuildings, b.hashVehicles, b.hashVehiclesPos, b.hashCitizens);
    Send(LS_DESYNC, g_desync, strlen(g_desync)); // the player shows it too
    Determinism_DumpMoneyTrace(theirs.tick);
}

void LockstepNet_StateAfter(unsigned long long tick, const StateSummary& s)
{
    if (!g_connected) return;
    if (!g_isHost) {
        LsHash h = { tick, s };
        Send(LS_HASH, &h, sizeof(h));
        return;
    }
    OwnHash& slot = g_ownHashes[(tick / LOCKSTEP_HASH_TICKS) % LS_HASH_KEEP];
    slot.tick = tick;
    slot.s = s;
    slot.valid = true;
    // A player summary that came first (it should not, the player is behind).
    for (int i = 0; i < g_playerHashCount; i++) {
        if (g_playerHashes[i].tick != tick) continue;
        LsHash h = g_playerHashes[i];
        g_playerHashes[i] = g_playerHashes[--g_playerHashCount];
        Compare(h);
        break;
    }
}

// ---------------------------------------------------------------------------
// Game thread
// ---------------------------------------------------------------------------

void LockstepNet_Poll()
{
    Locks();
    std::deque<LsMessage> in;
    EnterCriticalSection(&g_inLock);
    in.swap(g_inbox);
    LeaveCriticalSection(&g_inLock);
    for (LsMessage& m : in) {
        const BYTE* p = m.data.data();
        size_t n = m.data.size();
        switch (m.type) {
        case LS_GRANT:
            if (n == sizeof(LsGrant)) {
                EnterCriticalSection(&g_inLock);
                g_grants.push_back(*(const LsGrant*)p);
                g_grantBacklog = (LONG)g_grants.size();
                LeaveCriticalSection(&g_inLock);
            }
            break;
        case LS_CMD:
            if (n >= sizeof(LsCommandHead))
                Commands_SubmitRemote(((const LsCommandHead*)p)->kind, p + sizeof(LsCommandHead), n - sizeof(LsCommandHead));
            break;
        case LS_CMDS:
            if (n >= sizeof(LsCommandHead))
                Commands_Receive(((const LsCommandHead*)p)->kind, ((const LsCommandHead*)p)->afterTick,
                                 p + sizeof(LsCommandHead), n - sizeof(LsCommandHead));
            break;
        case LS_ACK:
            if (n == sizeof(unsigned long long) && *(const unsigned long long*)p > g_playerCompleted)
                g_playerCompleted = *(const unsigned long long*)p;
            break;
        case LS_HASH:
            if (n == sizeof(LsHash)) Compare(*(const LsHash*)p);
            break;
        case LS_PAUSE:
            g_pauseToggles++;
            break;
        case LS_SPEED:
            if (n == sizeof(int)) g_speedRequest = *(const int*)p;
            break;
        case LS_STATE:
            if (n == sizeof(LsState)) {
                g_hostState = *(const LsState*)p;
                g_hostStateKnown = true;
            }
            break;
        }
    }
}

bool LockstepNet_MayRun(unsigned long long tick)
{
    bool may = g_connected && g_playerCompleted + LOCKSTEP_WINDOW >= tick;
    if (!may && g_connected) {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastStallMs > 1000) {
            InterlockedIncrement(&g_stalls); // one per second of waiting
            g_lastStallMs = now;
        }
    }
    return may;
}

void LockstepNet_Grant(unsigned long long tick, int speed)
{
    g_lastGranted = tick;
    LsGrant g = { tick, speed };
    Send(LS_GRANT, &g, sizeof(g));
}

int LockstepNet_TakePauseToggles()
{
    int n = g_pauseToggles;
    g_pauseToggles = 0;
    return n;
}

int LockstepNet_TakeSpeedRequest()
{
    int s = g_speedRequest;
    g_speedRequest = 0;
    return s;
}

void LockstepNet_SendState(bool paused, int speed)
{
    LsState s = { paused ? 1 : 0, speed };
    Send(LS_STATE, &s, sizeof(s));
}

int LockstepNet_GrantBacklog(unsigned long long nextTick)
{
    Locks();
    EnterCriticalSection(&g_inLock);
    while (!g_grants.empty() && g_grants.front().tick < nextTick) g_grants.pop_front();
    g_grantBacklog = (LONG)g_grants.size();
    int count = (g_grants.empty() || g_grants.front().tick != nextTick) ? 0 : (int)g_grants.size();
    LeaveCriticalSection(&g_inLock);
    return count;
}

bool LockstepNet_TakeGrant(unsigned long long tick, int* speed)
{
    Locks();
    EnterCriticalSection(&g_inLock);
    while (!g_grants.empty() && g_grants.front().tick < tick) g_grants.pop_front();
    if (g_grants.empty() || g_grants.front().tick != tick) {
        LeaveCriticalSection(&g_inLock);
        return false;
    }
    *speed = g_grants.front().speed;
    g_grants.pop_front();
    g_grantBacklog = (LONG)g_grants.size();
    LeaveCriticalSection(&g_inLock);
    return true;
}

void LockstepNet_Ping()
{
    LsPing p = { GetTickCount64(), 0 };
    Send(LS_PING, &p, sizeof(p));
}

bool LockstepNet_RoundTrip(unsigned long long* ms)
{
    *ms = g_rttMs;
    return g_rttKnown;
}

LockstepFlow LockstepNet_Flow()
{
    LockstepFlow f = {};
    f.stalls = (int)g_stalls;
    ULONGLONG stallMs = g_lastStallMs;
    ULONGLONG now = GetTickCount64();
    f.msSinceStall = stallMs ? (now >= stallMs ? now - stallMs : 0) : ~0ull;
    if (g_isHost) {
        unsigned long long granted = g_lastGranted;
        unsigned long long completed = g_playerCompleted;
        f.behind = granted > completed ? granted - completed : 0;
    } else {
        f.behind = (unsigned long long)g_grantBacklog;
    }
    return f;
}

ULONGLONG LockstepNet_MsSinceReceived()
{
    ULONGLONG last = g_lastReceived;
    ULONGLONG now = GetTickCount64();
    return now >= last ? now - last : 0;
}

void LockstepNet_SendResync()
{
    Send(LS_RESYNC, nullptr, 0);
}

bool LockstepNet_TakeResync()
{
    if (!g_resyncReceived) return false;
    g_resyncReceived = false;
    return true;
}

void LockstepNet_Ack(unsigned long long completed)
{
    Send(LS_ACK, &completed, sizeof(completed));
}

void LockstepNet_SendPauseToggle()
{
    Send(LS_PAUSE, nullptr, 0);
}

void LockstepNet_SendSpeed(int speed)
{
    Send(LS_SPEED, &speed, sizeof(speed));
}

bool LockstepNet_HostState(bool* paused, int* speed)
{
    if (!g_hostStateKnown) return false;
    *paused = g_hostState.paused != 0;
    *speed = g_hostState.speed;
    return true;
}

void LockstepNet_SendCommand(int kind, unsigned long long afterTick, const void* data, size_t size)
{
    LsCommandHead h = { afterTick, kind };
    Send(g_isHost ? LS_CMDS : LS_CMD, &h, sizeof(h), data, size);
}

void LockstepNet_SendCursor(float x, float y, float z, float camX, float camZ, bool isPing)
{
    if (!g_connected) return;
    LsCursor c = { x, y, z, camX, camZ, (unsigned char)(isPing ? 1 : 0) };
    Send(LS_CURSOR, &c, sizeof(c));
}

PartnerCursorInfo LockstepNet_GetPartnerCursor()
{
    PartnerCursorInfo out = {};
    if (!g_connected) return out;
    Locks();
    EnterCriticalSection(&g_inLock);
    out = g_partnerCursor;
    LeaveCriticalSection(&g_inLock);
    return out;
}
