// SPDX-License-Identifier: GPL-3.0-only
// See terrain_sync.h for the overall design.
#include "terrain_sync.h"
#include "../../transport/network.h"
#include "../../ui/debug_log.h"
#include "../../third_party/miniz/miniz.h"
#include <vector>
#include <deque>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#define WORLD_THIS_RVA 0x9D4F10
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18 // city buildings vector: non-null once a world is loaded
#define VEC_BUILDINGS_END_RVA 0x9E6A20

// Mask-paint queue: std::vector of 28-byte records at thisPtr+0x130d0,
// filled by FUN_14040eaa0 and drained every frame by FUN_14040ece0 (which
// empties it by setting end = begin). Records are plain data.
#define MASK_QUEUE_BEGIN_OFFSET 0x130d0
#define MASK_QUEUE_END_OFFSET 0x130d8
#define MASK_QUEUE_RECORD_SIZE 0x1c

// Payloads at least this big are compressed.
#define TERRAIN_SYNC_COMPRESS_MIN 512

// Heights further apart than this after the same batch count as drift.
#define TERRAIN_SYNC_DRIFT_EPS 0.001f
#define TERRAIN_SYNC_STATS_INTERVAL_MS 5000

// ---------------------------------------------------------------------------
// Wire layout of the synced methods
// ---------------------------------------------------------------------------

struct WireLayout {
    bool synced;
    bool hasPos2;
    unsigned char nf; // floats, TerrainArgs::f[0..nf)
    unsigned char ni; // ints, TerrainArgs::i[0..ni)
    bool editsMask;   // changes the mask, not heights (no height check)
    bool needsHeight; // texture locks the receiver must hold, as the game does:
    bool needsMask;   // C3DDLL64 reads terrain+0x210 (heightmap) / +0x158 (mask)
};

static WireLayout LayoutFor(int method)
{
    switch (method) {
    // EditRaiseLowerAngleLimit is the one height edit that also reads the
    // mask texture (terrain+0x158), so it gets both locks, like the terrain
    // tool (FUN_1403a8a70) holds them.
    case TM_RAISE_LOWER:       return { true, false, 4, 0, false, true,  false };
    case TM_RAISE_LOWER_ANGLE: return { true, false, 5, 0, false, true,  true  };
    case TM_SMOOTH:            return { true, false, 3, 0, false, true,  false };
    case TM_LEVEL:             return { true, false, 3, 0, false, true,  false };
    case TM_TO_HEIGHT:         return { true, true,  3, 0, false, true,  false };
    case TM_SMOOTH_LIMITS:     return { true, false, 9, 0, false, true,  false };
    case TM_MASK:              return { true, false, 2, 3, true,  false, true  };
    default:                   return { false, false, 0, 0, false, false, false };
    }
}

static size_t EncodedSize(const WireLayout& l)
{
    return 2 + 12 + (l.hasPos2 ? 12 : 0) + l.nf * 4 + l.ni * 4;
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

// g_lock (tesmio_plugin.h, initialized by TsmBind) guards the send queue and incoming.
static HANDLE g_sendEvent = nullptr;

static volatile bool g_connected = false;
static volatile bool g_connecting = false;
// Set by Reset to abandon a pending accept/connect (polled every ~200ms).
static volatile bool g_cancelConnect = false;
static volatile bool g_isHost = false;
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = REPUBLICMP_HOST_IP;

struct ReceivedBatch {
    TerrainSyncBatchHeader header;
    std::vector<unsigned char> payload;
};
static std::deque<std::vector<unsigned char>> g_sendQueue; // game thread -> send thread
static std::deque<ReceivedBatch> g_incoming;              // receive thread -> game thread

// Statistics, game thread only (except the counters the threads bump).
static volatile long g_statSentBatches = 0, g_statSentEdits = 0, g_statSentBytes = 0;
static long g_statRecvBatches = 0, g_statRecvEdits = 0, g_statDriftBatches = 0;
static float g_statMaxDrift = 0.0f;
static long g_statSuppressed = 0, g_statQueueCut = 0;
static ULONGLONG g_statLastLogMs = 0;

void TerrainSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    g_sendEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
}

static void TerrainSyncStatus(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    DebugLog_FormatStatus(Logf, "TERRAIN_SYNC", fmt, ap);
    va_end(ap);
}

static bool IsWorldLoaded()
{
    __try {
        void* b = *(void**)(g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void* e = *(void**)(g_exeBase + VEC_BUILDINGS_END_RVA);
        return b && e && TerrainHook_GetTerrain();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Foreign replays (road/building replays of the other machine's work)
// ---------------------------------------------------------------------------

static int g_foreignDepth = 0;           // game thread only
static bool g_foreignSuppress = false;   // decided once at the outermost Begin
static long long g_foreignQueueCount = -1;

static long long MaskQueueCount()
{
    __try {
        BYTE* thisPtr = g_exeBase + WORLD_THIS_RVA;
        BYTE* b = *(BYTE**)(thisPtr + MASK_QUEUE_BEGIN_OFFSET);
        BYTE* e = *(BYTE**)(thisPtr + MASK_QUEUE_END_OFFSET);
        if (!b || e < b) return 0;
        return (long long)(e - b) / MASK_QUEUE_RECORD_SIZE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Drops the records queued after `keep`, the same way FUN_14040ece0 empties
// the queue (moving `end` back). Returns how many were dropped.
static long long MaskQueueTruncate(long long keep)
{
    __try {
        BYTE* thisPtr = g_exeBase + WORLD_THIS_RVA;
        BYTE* b = *(BYTE**)(thisPtr + MASK_QUEUE_BEGIN_OFFSET);
        BYTE** endField = (BYTE**)(thisPtr + MASK_QUEUE_END_OFFSET);
        if (!b) return 0;
        long long now = (long long)(*endField - b) / MASK_QUEUE_RECORD_SIZE;
        if (keep < 0 || now <= keep) return 0;
        *endField = b + keep * MASK_QUEUE_RECORD_SIZE;
        return now - keep;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void TerrainSync_BeginForeignReplay()
{
    if (g_foreignDepth++ > 0) return;
    g_foreignSuppress = g_connected;
    g_foreignQueueCount = g_foreignSuppress ? MaskQueueCount() : -1;
}

void TerrainSync_EndForeignReplay()
{
    if (g_foreignDepth <= 0) return;
    if (--g_foreignDepth > 0) return;
    if (g_foreignSuppress) {
        long long cut = MaskQueueTruncate(g_foreignQueueCount);
        g_statQueueCut += (long)cut;
    }
    g_foreignSuppress = false;
    g_foreignQueueCount = -1;
}

bool TerrainSync_SuppressLocalEdits()
{
    if (g_foreignDepth > 0 && g_foreignSuppress) {
        g_statSuppressed++;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Capture (game thread)
// ---------------------------------------------------------------------------

static std::vector<unsigned char> g_outPayload;
static int g_outEdits = 0;
static float g_outCheckPos[TERRAIN_SYNC_MAX_CHECKS][3];
static int g_outHeightEdits = 0; // height edits this batch; last MAX_CHECKS positions kept
static int g_nextSeq = 0;
static volatile long g_lastAppliedSeq = 0; // highest remote batch seq applied (game thread writes)

static void Put(std::vector<unsigned char>& v, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    v.insert(v.end(), b, b + n);
}

// Queues the current batch for sending. withChecks: sample heights now
// (only at the end of a frame, after every edit of that frame ran).
static void FinishBatch(bool withChecks)
{
    if (g_outEdits == 0) return;

    TerrainSyncBatchHeader h = {};
    h.seq = ++g_nextSeq;
    h.editCount = g_outEdits;
    h.payloadBytes = (int)g_outPayload.size();
    if (withChecks) {
        int n = g_outHeightEdits < TERRAIN_SYNC_MAX_CHECKS ? g_outHeightEdits : TERRAIN_SYNC_MAX_CHECKS;
        for (int k = 0; k < n; k++) {
            TerrainSyncCheck& c = h.checks[h.checkCount];
            memcpy(c.pos, g_outCheckPos[k], sizeof(c.pos));
            if (TerrainHook_HeightAt(c.pos, &c.height)) h.checkCount++;
        }
    }

    // Level 1: a few hundred microseconds for a full 64KB batch.
    std::vector<unsigned char> packed;
    if (g_outPayload.size() >= TERRAIN_SYNC_COMPRESS_MIN) {
        mz_ulong packedLen = mz_compressBound((mz_ulong)g_outPayload.size());
        packed.resize(packedLen);
        if (mz_compress2(packed.data(), &packedLen, g_outPayload.data(), (mz_ulong)g_outPayload.size(), 1) == MZ_OK &&
            packedLen < g_outPayload.size()) {
            packed.resize(packedLen);
            h.compressedBytes = (int)packedLen;
        } else {
            packed.clear();
        }
    }
    const std::vector<unsigned char>& body = h.compressedBytes ? packed : g_outPayload;

    std::vector<unsigned char> msg;
    msg.reserve(sizeof(int) + sizeof(h) + body.size());
    int tag = TERRAIN_SYNC_WIRE_BATCH;
    Put(msg, &tag, sizeof(tag));
    Put(msg, &h, sizeof(h));
    Put(msg, body.data(), body.size());

    EnterCriticalSection(&g_lock);
    g_sendQueue.push_back(std::move(msg));
    LeaveCriticalSection(&g_lock);
    SetEvent(g_sendEvent);

    g_outPayload.clear();
    g_outEdits = 0;
    g_outHeightEdits = 0;
}

void TerrainSync_CaptureEdit(int method, const TerrainArgs& a)
{
    if (!g_connected) return;
    WireLayout l = LayoutFor(method);
    if (!l.synced) return;

    if (g_outPayload.size() + EncodedSize(l) > TERRAIN_SYNC_MAX_PAYLOAD) FinishBatch(false);

    unsigned char head[2] = { (unsigned char)method, (unsigned char)(a.b ? 1 : 0) };
    Put(g_outPayload, head, sizeof(head));
    Put(g_outPayload, a.pos, 12);
    if (l.hasPos2) Put(g_outPayload, a.pos2, 12);
    Put(g_outPayload, a.f, l.nf * 4);
    Put(g_outPayload, a.i, l.ni * 4);
    g_outEdits++;

    if (!l.editsMask) {
        memcpy(g_outCheckPos[g_outHeightEdits % TERRAIN_SYNC_MAX_CHECKS], a.pos, 12);
        g_outHeightEdits++;
    }
}

// ---------------------------------------------------------------------------
// Apply (game thread)
// ---------------------------------------------------------------------------

// Decodes one edit at payload[*off]; false on a malformed record.
static bool DecodeEdit(const std::vector<unsigned char>& p, size_t* off, int* method, TerrainArgs* a)
{
    if (*off + 2 > p.size()) return false;
    *method = p[*off];
    WireLayout l = LayoutFor(*method);
    if (!l.synced || *off + EncodedSize(l) > p.size()) return false;

    memset(a, 0, sizeof(*a));
    a->b = p[*off + 1];
    // p.data() + o, not &p[o]: a height edit has no ints, so at the end of
    // the payload o == p.size(), which the debug STL asserts on.
    const unsigned char* d = p.data();
    size_t o = *off + 2;
    memcpy(a->pos, d + o, 12); o += 12;
    if (l.hasPos2) { memcpy(a->pos2, d + o, 12); o += 12; }
    memcpy(a->f, d + o, l.nf * 4); o += l.nf * 4;
    memcpy(a->i, d + o, l.ni * 4); o += l.ni * 4;
    *off = o;
    return true;
}

// Applies one received batch; returns false if it was malformed.
static bool ApplyBatch(const ReceivedBatch& batch)
{
    void* terrain = TerrainHook_GetTerrain();
    if (!terrain) return false;

    // First pass: validate and find which textures the edits need.
    bool needHeight = false, needMask = false;
    {
        size_t off = 0;
        int method;
        TerrainArgs a;
        for (int k = 0; k < batch.header.editCount; k++) {
            if (!DecodeEdit(batch.payload, &off, &method, &a)) {
                Logf("republicmp  TERRAIN_SYNC: seq=%d malformed edit #%d - batch dropped", batch.header.seq, k);
                return false;
            }
            WireLayout l = LayoutFor(method);
            if (l.needsHeight) needHeight = true;
            if (l.needsMask) needMask = true;
        }
    }

    InterlockedIncrement(&g_terrainApplyingRemote);
    __try {
        if (needHeight) TerrainHook_HeightmapOpen(terrain);
        if (needMask) TerrainHook_MaskOpen(terrain);
        __try {
            size_t off = 0;
            int method;
            TerrainArgs a;
            for (int k = 0; k < batch.header.editCount; k++) {
                DecodeEdit(batch.payload, &off, &method, &a);
                TerrainHook_ApplyEdit(terrain, method, a);
            }
        } __finally {
            if (needMask) TerrainHook_MaskClose(terrain);
            if (needHeight) TerrainHook_HeightmapClose(terrain);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  TERRAIN_SYNC: seq=%d threw while applying", batch.header.seq);
    }
    InterlockedDecrement(&g_terrainApplyingRemote);

    // Drift check against the sender's heights after the same edits.
    float worst = 0.0f;
    int worstIdx = -1;
    for (int k = 0; k < batch.header.checkCount && k < TERRAIN_SYNC_MAX_CHECKS; k++) {
        const TerrainSyncCheck& c = batch.header.checks[k];
        float h;
        if (!TerrainHook_HeightAt(c.pos, &h)) continue;
        float d = fabsf(h - c.height);
        if (d > worst) { worst = d; worstIdx = k; }
    }
    if (worst > g_statMaxDrift) g_statMaxDrift = worst;
    if (worst > TERRAIN_SYNC_DRIFT_EPS) {
        g_statDriftBatches++;
        const TerrainSyncCheck& c = batch.header.checks[worstIdx];
        Logf("republicmp  TERRAIN_SYNC: seq=%d DRIFT %.4fm at (%.2f,%.2f,%.2f) sender=%.4f",
             batch.header.seq, worst, c.pos[0], c.pos[1], c.pos[2], c.height);
    }

    g_statRecvBatches++;
    g_statRecvEdits += batch.header.editCount;
    if (batch.header.seq > g_lastAppliedSeq) g_lastAppliedSeq = batch.header.seq;
    return true;
}

int TerrainSync_GetFenceSeq()
{
    if (!g_connected) return 0;
    return g_nextSeq + (g_outEdits > 0 ? 1 : 0);
}

bool TerrainSync_FenceReached(int fence)
{
    // Not connected: there is no terrain stream to wait for.
    if (fence <= 0 || !g_connected) return true;
    return g_lastAppliedSeq >= fence;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

static void CloseConnection()
{
    g_connected = false;
    NetworkCloseSocket(g_sock);
    g_sock = INVALID_SOCKET;
    if (g_sendEvent) SetEvent(g_sendEvent);
}

static DWORD WINAPI SendLoopThread(LPVOID)
{
    while (g_connected) {
        WaitForSingleObject(g_sendEvent, 200);
        for (;;) {
            std::vector<unsigned char> msg;
            EnterCriticalSection(&g_lock);
            if (!g_sendQueue.empty()) {
                msg = std::move(g_sendQueue.front());
                g_sendQueue.pop_front();
            }
            LeaveCriticalSection(&g_lock);
            if (msg.empty() || !g_connected) break;

            if (!NetworkSendExact(g_sock, msg.data(), msg.size())) {
                TerrainSyncStatus("[TERRAIN_SYNC] send failed, dropping connection");
                CloseConnection();
                break;
            }
            const TerrainSyncBatchHeader* h = (const TerrainSyncBatchHeader*)(msg.data() + sizeof(int));
            InterlockedIncrement(&g_statSentBatches);
            InterlockedExchangeAdd(&g_statSentEdits, h->editCount);
            InterlockedExchangeAdd(&g_statSentBytes, (long)msg.size());
        }
    }
    return 0;
}

static DWORD WINAPI ReceiveLoopThread(LPVOID)
{
    while (g_connected) {
        int tag = 0;
        if (!NetworkRecvExact(g_sock, &tag, sizeof(tag))) {
            TerrainSyncStatus("[TERRAIN_SYNC] connection lost");
            CloseConnection();
            break;
        }
        if (tag != TERRAIN_SYNC_WIRE_BATCH) {
            TerrainSyncStatus("[TERRAIN_SYNC] unknown wire tag %d, dropping connection (mismatched build?)", tag);
            CloseConnection();
            break;
        }

        ReceivedBatch batch;
        if (!NetworkRecvExact(g_sock, &batch.header, sizeof(batch.header))) {
            TerrainSyncStatus("[TERRAIN_SYNC] connection lost");
            CloseConnection();
            break;
        }
        const TerrainSyncBatchHeader& h = batch.header;
        if (h.payloadBytes < 0 || h.payloadBytes > TERRAIN_SYNC_MAX_PAYLOAD || h.editCount < 0 ||
            h.compressedBytes < 0 || h.compressedBytes > TERRAIN_SYNC_MAX_PAYLOAD ||
            h.checkCount < 0 || h.checkCount > TERRAIN_SYNC_MAX_CHECKS) {
            TerrainSyncStatus("[TERRAIN_SYNC] bad batch header (seq=%d bytes=%d edits=%d), dropping connection",
                              h.seq, h.payloadBytes, h.editCount);
            CloseConnection();
            break;
        }
        int wireBytes = h.compressedBytes ? h.compressedBytes : h.payloadBytes;
        std::vector<unsigned char> wire(wireBytes);
        if (wireBytes && !NetworkRecvExact(g_sock, wire.data(), wireBytes)) {
            TerrainSyncStatus("[TERRAIN_SYNC] connection lost");
            CloseConnection();
            break;
        }
        if (h.compressedBytes) {
            batch.payload.resize(h.payloadBytes);
            mz_ulong outLen = (mz_ulong)h.payloadBytes;
            if (mz_uncompress(batch.payload.data(), &outLen, wire.data(), (mz_ulong)wireBytes) != MZ_OK ||
                outLen != (mz_ulong)h.payloadBytes) {
                // The edits are lost, but the stream itself is still framed
                // correctly; the empty batch still advances the fence.
                Logf("republicmp  TERRAIN_SYNC: seq=%d failed to inflate - batch dropped", h.seq);
                batch.header.editCount = 0;
                batch.header.checkCount = 0;
                batch.payload.clear();
            }
        } else {
            batch.payload.swap(wire);
        }

        EnterCriticalSection(&g_lock);
        g_incoming.push_back(std::move(batch));
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

static void StartConnection(SOCKET sock)
{
    g_sock = sock;

    DWORD sendTimeoutMs = 5000;
    DWORD recvTimeoutMs = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeoutMs, sizeof(sendTimeoutMs));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));
    BOOL noDelay = TRUE; // one small batch per frame - do not let Nagle hold it back
    setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

    EnterCriticalSection(&g_lock);
    g_sendQueue.clear();
    g_incoming.clear();
    LeaveCriticalSection(&g_lock);
    g_lastAppliedSeq = 0;

    g_connected = true;
    CreateThread(nullptr, 0, ReceiveLoopThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, SendLoopThread, nullptr, 0, nullptr);
    TerrainSyncStatus("[TERRAIN_SYNC] connection established");
}

static DWORD WINAPI HostAcceptWorkerThread(LPVOID)
{
    TerrainSyncStatus("[TERRAIN_SYNC] HOST, waiting for a client on port %d...", TERRAIN_SYNC_PORT);
    if (!NetworkInit()) {
        TerrainSyncStatus("[TERRAIN_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkHostAcceptOne(TERRAIN_SYNC_PORT, 300000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        TerrainSyncStatus("[TERRAIN_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
    if (sock == INVALID_SOCKET) {
        TerrainSyncStatus("[TERRAIN_SYNC] no client connected within 5 min, aborting - press Host##ts again to retry");
        g_connecting = false;
        return 0;
    }
    TerrainSyncStatus("[TERRAIN_SYNC] client connected");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

static DWORD WINAPI ClientConnectWorkerThread(LPVOID)
{
    TerrainSyncStatus("[TERRAIN_SYNC] CLIENT, connecting to host %s:%d...", g_targetIp, TERRAIN_SYNC_PORT);
    if (!NetworkInit()) {
        TerrainSyncStatus("[TERRAIN_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkConnect(g_targetIp, TERRAIN_SYNC_PORT, 15000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        TerrainSyncStatus("[TERRAIN_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
    if (sock == INVALID_SOCKET) {
        TerrainSyncStatus("[TERRAIN_SYNC] couldn't connect - is the host listening (Host##ts)? Is the IP right? - press Connect##ts again to retry");
        g_connecting = false;
        return 0;
    }
    TerrainSyncStatus("[TERRAIN_SYNC] connected to host");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

void TerrainSync_StartHost()
{
    NetworkLaunchChannelWorker(true, nullptr, g_targetIp, sizeof(g_targetIp),
                               &g_connected, &g_connecting, &g_isHost, &g_cancelConnect,
                               HostAcceptWorkerThread);
}

void TerrainSync_StartClient(const char* hostIp)
{
    NetworkLaunchChannelWorker(false, hostIp, g_targetIp, sizeof(g_targetIp),
                               &g_connected, &g_connecting, &g_isHost, &g_cancelConnect,
                               ClientConnectWorkerThread);
}

void TerrainSync_Reset()
{
    if (g_connecting) g_cancelConnect = true;
    if (g_connected) {
        TerrainSyncStatus("[TERRAIN_SYNC] connection reset by user");
        CloseConnection();
    }
}

bool TerrainSync_IsConnected()  { return g_connected; }
bool TerrainSync_IsConnecting() { return g_connecting; }
bool TerrainSync_IsHostRole()   { return g_isHost; }
const char* TerrainSync_GetTargetIp() { return g_targetIp; }

// ---------------------------------------------------------------------------
// Tick (game thread)
// ---------------------------------------------------------------------------

static bool g_warnedWorldNotLoaded = false;

static void LogStats()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_statLastLogMs < TERRAIN_SYNC_STATS_INTERVAL_MS) return;
    g_statLastLogMs = now;

    long sentBatches = InterlockedExchange(&g_statSentBatches, 0);
    long sentEdits = InterlockedExchange(&g_statSentEdits, 0);
    long sentBytes = InterlockedExchange(&g_statSentBytes, 0);
    if (sentBatches || g_statRecvBatches || g_statSuppressed || g_statQueueCut) {
        Logf("republicmp  TERRAIN_SYNC: last %ds: sent %ld batches/%ld edits/%ld bytes, "
             "received %ld batches/%ld edits, drift>%.3fm in %ld batches (max %.4fm), "
             "swallowed %ld foreign edits, cut %ld foreign mask records",
             TERRAIN_SYNC_STATS_INTERVAL_MS / 1000, sentBatches, sentEdits, sentBytes,
             g_statRecvBatches, g_statRecvEdits, TERRAIN_SYNC_DRIFT_EPS, g_statDriftBatches,
             g_statMaxDrift, g_statSuppressed, g_statQueueCut);
    }
    g_statRecvBatches = g_statRecvEdits = g_statDriftBatches = 0;
    g_statSuppressed = g_statQueueCut = 0;
    g_statMaxDrift = 0.0f;
}

void TerrainSync_Tick()
{
    // A replay bracket can never span frames; recover if one was left open.
    if (g_foreignDepth != 0) {
        Logf("republicmp  TERRAIN_SYNC: foreign replay bracket left open (depth=%d) - closed", g_foreignDepth);
        g_foreignDepth = 1;
        TerrainSync_EndForeignReplay();
    }

    if (g_connected) FinishBatch(true);
    else {
        g_outPayload.clear();
        g_outEdits = 0;
        g_outHeightEdits = 0;
    }

    std::deque<ReceivedBatch> batches;
    EnterCriticalSection(&g_lock);
    if (!g_incoming.empty()) {
        if (!IsWorldLoaded()) {
            if (!g_warnedWorldNotLoaded) {
                Logf("republicmp  TERRAIN_SYNC: %zu remote batch(es) held until the world is loaded", g_incoming.size());
                g_warnedWorldNotLoaded = true;
            }
        } else {
            batches.swap(g_incoming);
            g_warnedWorldNotLoaded = false;
        }
    }
    LeaveCriticalSection(&g_lock);

    for (const ReceivedBatch& b : batches) {
        if (!ApplyBatch(b) && b.header.seq > g_lastAppliedSeq)
            g_lastAppliedSeq = b.header.seq; // dropped, but nothing waiting on it should hang
    }

    LogStats();
}
