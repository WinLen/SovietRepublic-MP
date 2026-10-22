// SPDX-License-Identifier: GPL-3.0-only
// See build_sync.h for the overall design.
#include "build_sync.h"
#include "../../transport/network.h"
#include "../local_builds.h" // g_origUniversal, LocalBuilds_Begin/EndBuildingPlacement
#include "../terrain/terrain_sync.h"            // TerrainSync_Begin/EndForeignReplay, fence
#include "demolish_hook.h" // DemolishHook_CallOriginal
#include "../../session/game_speed.h"    // GameSpeed_OnRemote
#include "../../session/save_sync.h"               // SaveSync_OnResyncAnnounced
#include "../../session/money.h"         // Money_OnRemote
#include "../../session/settings_sync.h"           // SettingsSync_ClickedRecently
#include "../view_list_scope.h"
#include "../replay_menus.h"
#include "../../ui/debug_log.h"
#include "../../ui/sync_block.h"
#include <vector>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------------------------
// Engine addresses and offsets
// ---------------------------------------------------------------------------

#define UNIVERSAL_THIS_RVA 0x9D4F10      // placement god-object (thisPtr)
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18 // city buildings vector
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define VEC_CONSTRUCTIONS_BEGIN_RVA 0x9E6B38 // unfinished / demolition sites vector (thisPtr+0x11c28)
#define VEC_CONSTRUCTIONS_END_RVA 0x9E6B40
#define OBJ_TYPE_OFFSET 0x318            // building type record
#define OBJ_PAY_RUB_OFFSET 0x608         // byte: finish construction with rubles
#define OBJ_PAY_USD_OFFSET 0x609         // byte: finish construction with dollars
#define SET_PAY_RVA 0x160550             // FUN_140160550(this, building, rub, usd): sets both, then RESUME
#define OBJ_PROGRESS_OFFSET 0x604        // float: construction progress, 0..1
#define OBJ_STOPPED_OFFSET 0x879         // byte: construction stopped by the player
#define STOP_RVA 0x160570                // FUN_140160570(this, building, 1): the window's "stop" - sets
                                         // +0x879 and, unfinished and unpaid, cancels deliveries and workers
#define RESUME_RVA 0x161440              // FUN_140161440(this, building): the window's "continue" - clears
                                         // +0x879 (unless FUN_1401a9d60 says it must stay)
#define PAY_SCAN_INTERVAL_MS 250
#define TYPE_CATEGORY_OFFSET 0x360       // int category in the type record

// thisPtr placement inputs.
#define CURSOR_X_OFFSET 0xf6c
#define CURSOR_Y_OFFSET 0xf70
#define CURSOR_Z_OFFSET 0xf74
#define CURSOR_COPY_OFFSET 0xf78         // mirrors the cursor x/y/z
#define CURSOR_YAW_OFFSET 0xf94          // yaw in radians
#define MIRROR_TOGGLE_OFFSET 0x11db0     // byte: player's mirror toggle
#define PLACEMENT_RESULT_OFFSET 0x11a10  // 3 ints: validation result of the last call
#define TOOL_GUARD1_OFFSET 0xfbc         // any nonzero -> placement function returns at once
#define TOOL_GUARD2_OFFSET 0xfbe
#define TOOL_GUARD3_OFFSET 0xfc3
// Pointer to the menu entry the local player has selected (name string
// first, e.g. "road_onelane"; +0x2b5 a flag, +0x48 the type record), 0 when
// nothing is selected. FUN_1403c4850 (the road routing a building's access
// road goes through) reads +0x2b5 without a null check.
#define SELECTED_ENTRY_OFFSET 0xd428
// The open windows: a vector of window pointers at this+0xCAC0 (begin) /
// +0xCAC8 (end), walked by the window dispatcher FUN_1406e9b80. A building
// window's building is window+0x240 - FUN_1406f8cc0 (the building window)
// passes it to the stop / continue / pay calls.
#define OPEN_WINDOWS_BEGIN_OFFSET 0xCAC0
#define OPEN_WINDOWS_END_OFFSET 0xCAC8
#define WINDOW_BUILDING_OFFSET 0x240
#define OPEN_WINDOWS_MAX 64
#define PLACE_SCOPE_MARGIN 150.0f // view-list scope around a placement
#define REPLAY_ENTRY_SIZE 0x1000         // covers every field read off an entry

#define RETRY_DELAY_MS 500

// Building object: yaw as three consistent sin/cos pairs, and position.
#define OBJ_ROT_SIN_OFFSET 0x340
#define OBJ_ROT_COS_OFFSET 0x348
#define OBJ_ROT2_COS_OFFSET 0x360        // second pair: cos, sin
#define OBJ_ROT2_SIN_OFFSET 0x368
#define OBJ_ROT3_NSIN_OFFSET 0x380       // third pair: -sin, cos
#define OBJ_ROT3_COS_OFFSET 0x388
#define OBJ_POS_OFFSET 0x350

// ---------------------------------------------------------------------------
// Module state and helpers
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_queueLock;
static char g_replayEntry[REPLAY_ENTRY_SIZE]; // stand-in selected menu entry (game thread only)

void BuildSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_queueLock);
}

// Logs to the file log and to the Debug window.
static void BuildSyncStatus(const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    Logf("republicmp  BUILD_SYNC: %s", body);
    DebugLog_Add(body);
}

static __int64 GetPlacementManagerThisPtr()
{
    return (__int64)((uintptr_t)g_exe + UNIVERSAL_THIS_RVA);
}

static bool IsWorldLoaded()
{
    void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
    void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
    __try {
        return (*vecBeginPtr != nullptr) && (*vecEndPtr != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

static volatile bool g_connected = false;
static volatile bool g_connecting = false;
// Set by Reset to abandon a pending accept/connect (polled every ~200ms).
static volatile bool g_cancelConnect = false;
static volatile bool g_isHost = false;
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = BUILD_SYNC_TEST_HOST_IP;
// One message plus, for BUILD_SYNC_KIND_ACCESS_ROAD, the segment after it.
struct BuildWireItem {
    BuildSyncMsg msg;
    std::shared_ptr<RoadSyncSegment> road;
};
static std::vector<BuildWireItem> g_incoming; // receive thread -> game thread

static void CloseConnection()
{
    g_connected = false;
    NetworkCloseSocket(g_sock);
    g_sock = INVALID_SOCKET;
}

static DWORD WINAPI ReceiveLoopThread(LPVOID)
{
    while (g_connected) {
        BuildSyncMsg msg = {};
        if (!NetworkRecvExact(g_sock, &msg, sizeof(msg))) {
            BuildSyncStatus("[BUILD_SYNC] connection lost");
            CloseConnection();
            break;
        }
        msg.typeName[sizeof(msg.typeName) - 1] = 0;

        // Noted right here, before this thread can see the connection close
        // that follows it, so the session never mistakes it for a drop.
        if (msg.kind == BUILD_SYNC_KIND_RESYNC) {
            Logf("republicmp  BUILD_SYNC: the host announced a re-sync");
            SaveSync_OnResyncAnnounced();
            continue;
        }

        BuildWireItem item;
        item.msg = msg;
        if (msg.kind == BUILD_SYNC_KIND_ACCESS_ROAD) {
            // Header, then exactly pointCount points (see road_sync.h).
            item.road = std::make_shared<RoadSyncSegment>();
            RoadSyncSegment* road = item.road.get();
            if (!NetworkRecvExact(g_sock, road, ROAD_SYNC_SEGMENT_HEADER_BYTES)) {
                BuildSyncStatus("[BUILD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            if (!RoadSync_PointCountValid(road->pointCount)) {
                // Where the next message starts depends on the count - the
                // stream cannot go on.
                Logf("republicmp  BUILD_SYNC: access road with bad pointCount=%d - dropping the connection",
                     road->pointCount);
                BuildSyncStatus("[BUILD_SYNC] bad access road on the wire, dropping connection");
                CloseConnection();
                break;
            }
            if (road->pointCount > 0 &&
                !NetworkRecvExact(g_sock, road->points, RoadSync_PointBytes(road->pointCount))) {
                BuildSyncStatus("[BUILD_SYNC] connection lost");
                CloseConnection();
                break;
            }
        }

        EnterCriticalSection(&g_queueLock);
        g_incoming.push_back(item);
        LeaveCriticalSection(&g_queueLock);

        if (msg.kind == BUILD_SYNC_KIND_PAY)
            Logf("republicmp  BUILD_SYNC: received pay at (%.2f,%.2f,%.2f) rub=%d usd=%d", msg.x, msg.y, msg.z,
                 msg.speed, msg.speedSaved);
        if (msg.kind == BUILD_SYNC_KIND_PLACE || msg.kind == BUILD_SYNC_KIND_DEMOLISH)
            Logf("republicmp  BUILD_SYNC: received %s type=[%s] x=%.2f y=%.2f z=%.2f fence=%d",
                 msg.kind == BUILD_SYNC_KIND_DEMOLISH ? "demolish" : "placement",
                 msg.typeName, msg.x, msg.y, msg.z, msg.terrainFence);
    }
    return 0;
}

static void StartConnection(SOCKET sock)
{
    g_sock = sock;

    // Sends time out so a stalled peer cannot freeze the game thread;
    // receives block on the dedicated thread.
    DWORD sendTimeoutMs = 5000;
    DWORD recvTimeoutMs = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeoutMs, sizeof(sendTimeoutMs));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));

    g_connected = true;
    CreateThread(nullptr, 0, ReceiveLoopThread, nullptr, 0, nullptr);
    BuildSyncStatus("[BUILD_SYNC] connection established - build away");
}

static DWORD WINAPI HostAcceptWorkerThread(LPVOID)
{
    BuildSyncStatus("[BUILD_SYNC] HOST, waiting for a client on port %d...", BUILD_SYNC_PORT);
    if (!NetworkInit()) {
        BuildSyncStatus("[BUILD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkHostAcceptOne(BUILD_SYNC_PORT, 300000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        BuildSyncStatus("[BUILD_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
    if (sock == INVALID_SOCKET) {
        BuildSyncStatus("[BUILD_SYNC] no client connected within 5 min, aborting - press Host##bs again to retry");
        g_connecting = false;
        return 0;
    }
    BuildSyncStatus("[BUILD_SYNC] client connected");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

static DWORD WINAPI ClientConnectWorkerThread(LPVOID)
{
    BuildSyncStatus("[BUILD_SYNC] CLIENT, connecting to host %s:%d...", g_targetIp, BUILD_SYNC_PORT);
    if (!NetworkInit()) {
        BuildSyncStatus("[BUILD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkConnect(g_targetIp, BUILD_SYNC_PORT, 15000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        BuildSyncStatus("[BUILD_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
    if (sock == INVALID_SOCKET) {
        BuildSyncStatus("[BUILD_SYNC] couldn't connect - is the host listening (Host##bs)? Is the IP right? - press Connect##bs again to retry");
        g_connecting = false;
        return 0;
    }
    BuildSyncStatus("[BUILD_SYNC] connected to host");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

void BuildSync_StartHost()
{
    if (g_connected || g_connecting) return;
    g_isHost = true;
    g_cancelConnect = false;
    g_connecting = true;
    CreateThread(nullptr, 0, HostAcceptWorkerThread, nullptr, 0, nullptr);
}

void BuildSync_StartClient(const char* hostIp)
{
    if (g_connected || g_connecting) return;
    if (hostIp && hostIp[0]) {
        strncpy_s(g_targetIp, hostIp, _TRUNCATE);
    }
    g_isHost = false;
    g_cancelConnect = false;
    g_connecting = true;
    CreateThread(nullptr, 0, ClientConnectWorkerThread, nullptr, 0, nullptr);
}

void BuildSync_Reset()
{
    if (g_connecting) g_cancelConnect = true;
    if (g_connected) {
        BuildSyncStatus("[BUILD_SYNC] connection reset by user");
        CloseConnection();
    }
}

bool BuildSync_IsConnected()  { return g_connected; }
bool BuildSync_IsConnecting() { return g_connecting; }
bool BuildSync_IsHostRole()   { return g_isHost; }
const char* BuildSync_GetTargetIp() { return g_targetIp; }

// ---------------------------------------------------------------------------
// Sending (game thread only - the only thread that writes to the socket)
// ---------------------------------------------------------------------------

static std::vector<BuildWireItem> g_outbound; // placements waiting for a connection

static volatile LONG g_applyingRemote = 0;

bool BuildSync_IsApplyingRemote()
{
    return g_applyingRemote != 0;
}

static void SendNow(const BuildWireItem& item)
{
    const BuildSyncMsg& msg = item.msg;
    if (msg.kind == BUILD_SYNC_KIND_ACCESS_ROAD) {
        const RoadSyncSegment* road = item.road.get();
        if (!road || !RoadSync_PointCountValid(road->pointCount) || !NetworkSendExact(g_sock, &msg, sizeof(msg)) ||
            !NetworkSendExact(g_sock, road, ROAD_SYNC_SEGMENT_HEADER_BYTES + RoadSync_PointBytes(road->pointCount))) {
            BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
            CloseConnection();
        }
        return;
    }
    if (msg.kind == BUILD_SYNC_KIND_SPEED_SET || msg.kind == BUILD_SYNC_KIND_SPEED_REQUEST ||
        msg.kind == BUILD_SYNC_KIND_SPEED_BOOST || msg.kind == BUILD_SYNC_KIND_TIME ||
        msg.kind == BUILD_SYNC_KIND_MONEY || msg.kind == BUILD_SYNC_KIND_RESYNC ||
        msg.kind == BUILD_SYNC_KIND_SETTING || msg.kind == BUILD_SYNC_KIND_WEATHER ||
        msg.kind == BUILD_SYNC_KIND_STATE) {
        if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
            BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
            CloseConnection();
        }
        return;
    }
    if (msg.kind == BUILD_SYNC_KIND_DEMOLISH) {
        Logf("republicmp  BUILD_SYNC: sending demolish at (%.2f,%.2f,%.2f) category=%d fence=%d",
             msg.x, msg.y, msg.z, msg.demolishCategory, msg.terrainFence);
        if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
            BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
            CloseConnection();
        }
        return;
    }
    char mode[96];
    Logf("republicmp  BUILD_SYNC: sending type=[%s] obj=(%.2f,%.2f,%.2f) cursor=(%.2f,%.2f,%.2f) cursorRot=%.4f p5=0x%llx pay=%s",
         msg.typeName, msg.x, msg.y, msg.z, msg.cursorX, msg.cursorY, msg.cursorZ, msg.cursorRot, msg.param5,
         BuildMode_Describe(msg.mode, mode, sizeof(mode)));
    if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
        BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
        CloseConnection();
    }
}

static void SendOrQueue(const BuildSyncMsg& msg, std::shared_ptr<RoadSyncSegment> road = nullptr)
{
    BuildWireItem item;
    item.msg = msg;
    item.road = road;
    if (!g_connected || !g_outbound.empty()) {
        g_outbound.push_back(item);
        Logf("republicmp  BUILD_SYNC: queued %s [%s] (%zu waiting for connection)",
             msg.kind == BUILD_SYNC_KIND_DEMOLISH ? "demolish" : "placement", msg.typeName, g_outbound.size());
        return;
    }
    SendNow(item);
}

void BuildSync_SendPlacement(const BuildSyncMsg& in)
{
    BuildSyncMsg msg = in;
    msg.kind = BUILD_SYNC_KIND_PLACE;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    msg.mode = BuildMode_Read(); // same frame as the placement
    SendOrQueue(msg);
}

void BuildSync_SendSpeed(int kind, int speed, int speedSaved, int turbo)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = kind;
    strcpy_s(msg.typeName, "<speed>");
    msg.speed = speed;
    msg.speedSaved = speedSaved;
    msg.speedTurbo = turbo;
    // Behind queued placements, so order on the connection is kept.
    SendOrQueue(msg);
}

void BuildSync_SendTime(int day, int year, float progress)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_TIME;
    strcpy_s(msg.typeName, "<time>");
    msg.speed = day;
    msg.speedSaved = year;
    msg.timeProgress = progress;
    SendOrQueue(msg);
}
  
void BuildSync_SendWeather(const WeatherWire& w)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_WEATHER;
    strcpy_s(msg.typeName, "<weather>");
    msg.weather = w;
    SendOrQueue(msg);
}
  
void BuildSync_SendMoney(double rub, double usd)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_MONEY;
    strcpy_s(msg.typeName, "<money>");
    msg.moneyRub = rub;
    msg.moneyUsd = usd;
    SendOrQueue(msg);
}

void BuildSync_SendResync()
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_RESYNC;
    strcpy_s(msg.typeName, "<resync>");
    SendOrQueue(msg);
}

void BuildSync_SendSetting(const SettingWire& w)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_SETTING;
    strcpy_s(msg.typeName, "<setting>");
    msg.setting = w;
    SendOrQueue(msg);
}

void BuildSync_SendState(const SyncCheckWire& w)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_STATE;
    strcpy_s(msg.typeName, "<state>");
    msg.state = w;
    SendOrQueue(msg);
}

void BuildSync_SendAccessRoad(const RoadSyncSegment& seg)
{
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_ACCESS_ROAD;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    msg.mode = BuildMode_Read();
    strcpy_s(msg.typeName, "<access road>");
    msg.x = seg.startNode[0]; msg.y = seg.startNode[1]; msg.z = seg.startNode[2];
    SendOrQueue(msg, std::make_shared<RoadSyncSegment>(seg));
}

void BuildSync_SendDemolish(const BuildSyncMsg& in)
{
    BuildSyncMsg msg = in;
    msg.kind = BUILD_SYNC_KIND_DEMOLISH;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    strcpy_s(msg.typeName, "<demolish>");
    SendOrQueue(msg);
}

static void FlushOutbound()
{
    while (g_connected && !g_outbound.empty()) {
        SendNow(g_outbound.front());
        if (!g_connected) break; // send failed - keep it for the next connection
        g_outbound.erase(g_outbound.begin());
    }
}

// ---------------------------------------------------------------------------
// Applying received placements (game thread only)
// ---------------------------------------------------------------------------

// The sender's main object and ours may differ by float noise only; one
// farther away is some other object and must not get the sender's rotation.
#define MAIN_OBJECT_MAX_DELTA 1.0f

// Buildings the replayed placement already connected to the road network
// itself (it lays its own access road, as on the sender). Their access road
// message is then not built again: when the local network differs a little
// (a road a few percent less built), the game's own road takes another
// route, the sender's one no longer matches it, and both were built - a
// duplicate road next to the building.
#define SELF_CONNECTED_KEEP 16
static float g_selfConnectedAt[SELF_CONNECTED_KEEP][3];
static int g_selfConnectedRoads[SELF_CONNECTED_KEEP];
static int g_selfConnectedCount = 0;

static void RememberSelfConnected(const float pos[3], int roadsAdded)
{
    int k = g_selfConnectedCount++ % SELF_CONNECTED_KEEP;
    memcpy(g_selfConnectedAt[k], pos, sizeof(g_selfConnectedAt[k]));
    g_selfConnectedRoads[k] = roadsAdded;
}

// Road segments the latest placement of the building at the port added
// here, or 0.
static int SelfConnectedRoads(const RoadSyncBuildingPort& port)
{
    if (!port.valid) return 0;
    int kept = g_selfConnectedCount < SELF_CONNECTED_KEEP ? g_selfConnectedCount : SELF_CONNECTED_KEEP;
    for (int n = 1; n <= kept; n++) { // newest first: a rebuilt spot uses its latest placement
        int i = (g_selfConnectedCount - n) % SELF_CONNECTED_KEEP;
        if (fabsf(g_selfConnectedAt[i][0] - port.bldPos[0]) < 0.5f && fabsf(g_selfConnectedAt[i][2] - port.bldPos[2]) < 0.5f &&
            fabsf(g_selfConnectedAt[i][1] - port.bldPos[1]) < 1.0f)
            return g_selfConnectedRoads[i];
    }
    return 0;
}

// The buildings before a replayed placement, sorted, so the new ones are
// found by identity - not as "the vector's tail", which a placement that
// also removes or reorders objects breaks (issue #16, a pub 22.5 m off).
static std::vector<void*> g_placeBefore;

// The buildings vector's bounds; false if unreadable.
static bool ReadBuildingsVector(void*** begin, void*** end)
{
    __try {
        *begin = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        *end = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        return *begin && *end >= *begin;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SnapshotBeforePlacement()
{
    g_placeBefore.clear();
    void** begin;
    void** end;
    if (ReadBuildingsVector(&begin, &end)) g_placeBefore.assign(begin, end);
    std::sort(g_placeBefore.begin(), g_placeBefore.end());
}

// Plain binary search: no STL iterators, which can't sit in a __try.
static bool WasBeforePlacement(void* obj)
{
    void* const* a = g_placeBefore.data();
    size_t lo = 0, hi = g_placeBefore.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a[mid] < obj) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_placeBefore.size() && a[lo] == obj;
}

// The new object (not in the snapshot) nearest the sender's main object at
// (x, z); *newCount = how many new objects there are, *dist = its distance.
static void* PickMainNewObject(float x, float z, int* newCount, float* dist)
{
    void* best = nullptr;
    float bestD2 = 0.0f;
    *newCount = 0;
    __try {
        void** it = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void** end = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it) {
            void* obj = *it;
            if (!obj || WasBeforePlacement(obj)) continue;
            ++*newCount;
            float dx = *(float*)((char*)obj + OBJ_POS_OFFSET) - x;
            float dz = *(float*)((char*)obj + OBJ_POS_OFFSET + 8) - z;
            float d2 = dx * dx + dz * dz;
            if (!best || d2 < bestD2) { best = obj; bestD2 = d2; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        best = nullptr;
    }
    *dist = sqrtf(bestD2);
    return best;
}

// For the log: the new objects of a placement - category (-1: no type, as a
// construction site's), offset from the cursor and construction progress - so
// the two PCs' logs show which object one of them made and the other did not.
static void DescribeNewObjects(float x, float z, char* out, size_t outSize)
{
    out[0] = 0;
    size_t used = 0;
    int shown = 0;
    __try {
        void** it = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void** end = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end && shown < 6; ++it) {
            char* obj = (char*)*it;
            if (!obj || WasBeforePlacement(obj)) continue;
            __int64 type = *(__int64*)(obj + OBJ_TYPE_OFFSET);
            int cat = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
            float dx = *(float*)(obj + OBJ_POS_OFFSET) - x;
            float dz = *(float*)(obj + OBJ_POS_OFFSET + 8) - z;
            int n = _snprintf_s(out + used, outSize - used, _TRUNCATE, "%s[cat %d at %+.1f,%+.1f progress %.3f]",
                                shown ? " " : "", cat, dx, dz, *(float*)(obj + 0x604));
            if (n < 0) break;
            used += (size_t)n;
            shown++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// True while the placement function would return without doing anything.
static bool PlacementGuardBusy(__int64 thisPtr)
{
    __try {
        return *(unsigned char*)(thisPtr + TOOL_GUARD1_OFFSET) ||
               *(unsigned char*)(thisPtr + TOOL_GUARD2_OFFSET) ||
               *(unsigned char*)(thisPtr + TOOL_GUARD3_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Replays one placement through the engine's placement function with the
// sender's inputs, then applies the sender's exact rotation to the main new
// object. The local player's cursor/yaw/mirror are restored even if the call
// throws. Returns true if a building was created.
static bool ApplyOneRemotePlacement(__int64 thisPtr, const BuildSyncMsg& msg, int attempt)
{
    bool placed = false;
    __try {
        float* cursor = (float*)(thisPtr + CURSOR_X_OFFSET);
        float* cursorCopy = (float*)(thisPtr + CURSOR_COPY_OFFSET);
        float* yaw = (float*)(thisPtr + CURSOR_YAW_OFFSET);
        unsigned char* mirror = (unsigned char*)(thisPtr + MIRROR_TOGGLE_OFFSET);
        float savedCursor[3] = { cursor[0], cursor[1], cursor[2] };
        float savedCopy[3] = { cursorCopy[0], cursorCopy[1], cursorCopy[2] };
        float savedYaw = *yaw;
        unsigned char savedMirror = *mirror;

        __try {
            cursor[0] = cursorCopy[0] = msg.cursorX;
            cursor[1] = cursorCopy[1] = msg.cursorY;
            cursor[2] = cursorCopy[2] = msg.cursorZ;
            *yaw = msg.cursorRot;
            *mirror = msg.mirrored ? 1 : 0;

            char typeName[64];
            strncpy_s(typeName, msg.typeName, _TRUNCATE);

            SnapshotBeforePlacement();
            int roadsBefore = RoadSync_SegmentCount(thisPtr, ROAD_SYNC_CATEGORY_ROAD);
            int pathsBefore = RoadSync_SegmentCount(thisPtr, ROAD_SYNC_CATEGORY_PEDESTRIAN);

            BuildMode mine = BuildMode_Read();
            char theirsText[96], mineText[96];
            Logf("republicmp  BUILD_SYNC: applying type=[%s] cursor=(%.2f,%.2f,%.2f) cursorRot=%.4f mirrored=%d attempt %d/%d pay=%s (mine: %s)",
                 typeName, msg.cursorX, msg.cursorY, msg.cursorZ, msg.cursorRot, msg.mirrored,
                 attempt, BUILD_SYNC_MAX_APPLY_ATTEMPTS,
                 BuildMode_Describe(msg.mode, theirsText, sizeof(theirsText)),
                 BuildMode_Describe(mine, mineText, sizeof(mineText)));

            // The original (not the hook), so the placement is not sent back.
            // param_4/param_8 = 1 confirm the placement without a mouse click.
            // Terrain levelling under the building arrives over terrain sync.
            LocalBuilds_BeginBuildingPlacement();
            TerrainSync_BeginForeignReplay();
            InterlockedIncrement(&g_applyingRemote);
            // Built the way the sender chose; the local choice goes back after.
            BuildMode_Write(msg.mode);
            // The local player's selected menu entry must not leak into the
            // replay - and the access-road routing reads it unchecked, so a
            // player with nothing selected crashed it. A neutral entry named
            // after the building stands in while the replay runs.
            __int64* selected = (__int64*)(thisPtr + SELECTED_ENTRY_OFFSET);
            __int64 savedSelected = *selected;
            // Not strncpy_s: the debug CRT fills the rest of the buffer with
            // 0xFE, and the game follows +0x48 as a pointer when it is nonzero.
            memset(g_replayEntry, 0, sizeof(g_replayEntry));
            memcpy(g_replayEntry, typeName, strnlen(typeName, 63));
            *selected = (__int64)g_replayEntry;
            // The same "camera-visible" edges as the sender's click (view_list_scope.h).
            bool scoped = ViewListScope_Begin(msg.cursorX - PLACE_SCOPE_MARGIN, msg.cursorZ - PLACE_SCOPE_MARGIN,
                                              msg.cursorX + PLACE_SCOPE_MARGIN, msg.cursorZ + PLACE_SCOPE_MARGIN);
            __try {
                g_origUniversal(thisPtr, typeName, 0, 1, msg.param5, 0xffffffff, 0, 1);
            } __finally {
                if (scoped) ViewListScope_End();
                *selected = savedSelected;
                BuildMode_Write(mine);
                InterlockedDecrement(&g_applyingRemote);
                TerrainSync_EndForeignReplay();
                LocalBuilds_EndBuildingPlacement();
            }

            int newCount = 0;
            float delta = 0.0f;
            void* obj = PickMainNewObject(msg.x, msg.z, &newCount, &delta);
            if (!obj) {
                const int* result = (const int*)(thisPtr + PLACEMENT_RESULT_OFFSET);
                Logf("republicmp  BUILD_SYNC: placement refused by the engine type=[%s] result=(%d,%d,%d)",
                     typeName, result[0], result[1], result[2]);
            } else if (delta > MAIN_OBJECT_MAX_DELTA) {
                // Something was built, but not where the sender's building
                // is: no rotation for it, and no retry - a retry would build
                // the helper objects again.
                placed = true;
                Logf("republicmp  BUILD_SYNC: placed type=[%s] objects=%d, but the nearest new object is %.3fm from the sender's - NOT verified, rotation not written",
                     typeName, newCount, delta);
            } else {
                placed = true;
                *(float*)((char*)obj + OBJ_ROT_SIN_OFFSET) = msg.rotSin;
                *(float*)((char*)obj + OBJ_ROT_COS_OFFSET) = msg.rotCos;
                *(float*)((char*)obj + OBJ_ROT2_COS_OFFSET) = msg.rotCos;
                *(float*)((char*)obj + OBJ_ROT2_SIN_OFFSET) = msg.rotSin;
                *(float*)((char*)obj + OBJ_ROT3_NSIN_OFFSET) = -msg.rotSin;
                *(float*)((char*)obj + OBJ_ROT3_COS_OFFSET) = msg.rotCos;
                int roadsAdded = RoadSync_SegmentCount(thisPtr, ROAD_SYNC_CATEGORY_ROAD) - roadsBefore;
                float pos[3] = { msg.x, msg.y, msg.z };
                RememberSelfConnected(pos, roadsAdded);
                Logf("republicmp  BUILD_SYNC: placed type=[%s] objects=%d, main object delta to sender %.3fm, road segments %+d, footpath segments %+d",
                     typeName, newCount, delta, roadsAdded,
                     RoadSync_SegmentCount(thisPtr, ROAD_SYNC_CATEGORY_PEDESTRIAN) - pathsBefore);
            }
        } __finally {
            cursor[0] = savedCursor[0]; cursor[1] = savedCursor[1]; cursor[2] = savedCursor[2];
            cursorCopy[0] = savedCopy[0]; cursorCopy[1] = savedCopy[1]; cursorCopy[2] = savedCopy[2];
            *yaw = savedYaw;
            *mirror = savedMirror;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  BUILD_SYNC: <exception applying remote placement>");
    }
    return placed;
}

// The local building at `pos` (0.5 m in x/z, 1 m in y) with this type
// category (-1 = any), nearest first; 0 if none.
static __int64 FindBuildingAt(float x, float y, float z, int category)
{
    __int64 found = 0;
    float bestD = 1e30f;
    __try {
        const unsigned int kBeginRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
        const unsigned int kEndRvas[2]   = { VEC_BUILDINGS_END_RVA,   VEC_CONSTRUCTIONS_END_RVA };
        for (int v = 0; v < 2; ++v) {
            __int64* it = *(__int64**)((uintptr_t)g_exeBase + kBeginRvas[v]);
            __int64* end = *(__int64**)((uintptr_t)g_exeBase + kEndRvas[v]);
            for (; it && it < end; ++it) {
                __int64 b = *it;
                if (!b) continue;
                const float* p = (const float*)(b + OBJ_POS_OFFSET);
                float dx = p[0] - x, dz = p[2] - z;
                if (fabsf(dx) >= 0.5f || fabsf(dz) >= 0.5f || fabsf(p[1] - y) >= 1.0f) continue;
                __int64 type = *(__int64*)(b + OBJ_TYPE_OFFSET);
                int cat = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
                if (category >= 0 && cat != category) continue;
                float d = dx * dx + dz * dz;
                if (d < bestD) { bestD = d; found = b; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = 0;
    }
    return found;
}

// ---------------------------------------------------------------------------
// Pay-to-finish flags (see build_sync.h)
// ---------------------------------------------------------------------------

static std::unordered_map<__int64, unsigned char> g_payFlags; // building -> rub | usd << 1, as last seen
static ULONGLONG g_lastPayScanMs = 0;

// The construction controls of a building's window, as flags:
#define PAY_RUB 1      // finish with rubles (+0x608)
#define PAY_USD 2      // finish with dollars (+0x609)
#define PAY_STOPPED 4  // construction stopped (+0x879)

// The flags of a building, or -1 if unreadable.
static int ReadPayFlags(__int64 b)
{
    __try {
        return (*(unsigned char*)(b + OBJ_PAY_RUB_OFFSET) ? PAY_RUB : 0) |
               (*(unsigned char*)(b + OBJ_PAY_USD_OFFSET) ? PAY_USD : 0) |
               (*(unsigned char*)(b + OBJ_STOPPED_OFFSET) ? PAY_STOPPED : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static bool ReadBuildingInfo(__int64 b, float pos[3], int* category)
{
    __try {
        const float* p = (const float*)(b + OBJ_POS_OFFSET);
        pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
        __int64 type = *(__int64*)(b + OBJ_TYPE_OFFSET);
        *category = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int SnapshotBuildings(__int64* out, int max)
{
    int n = 0;
    __try {
        const unsigned int kBeginRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
        const unsigned int kEndRvas[2]   = { VEC_BUILDINGS_END_RVA,   VEC_CONSTRUCTIONS_END_RVA };
        for (int v = 0; v < 2; ++v) {
            __int64* it = *(__int64**)((uintptr_t)g_exeBase + kBeginRvas[v]);
            __int64* end = *(__int64**)((uintptr_t)g_exeBase + kEndRvas[v]);
            for (; it && it < end && n < max; ++it)
                if (*it) out[n++] = *it;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static void SendPay(__int64 b, int flags);
static const char* PayName(int flags, char* out, size_t outSize);

// The buildings of the open windows (window+0x240 of every open window - for
// a window that is not a building's it is some other pointer, which never
// matches a building). Returns how many were written to out.
static int ReadOpenWindowBuildings(__int64* out, int max)
{
    int n = 0;
    __try {
        __int64 world = GetPlacementManagerThisPtr();
        __int64* it = *(__int64**)(world + OPEN_WINDOWS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(world + OPEN_WINDOWS_END_OFFSET);
        for (; it && it < end && n < max; ++it) {
            __int64 window = *it;
            if (!window) continue;
            __int64 b = *(__int64*)(window + WINDOW_BUILDING_OFFSET);
            if (b) out[n++] = b;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// True for a finished building (progress >= 1).
static bool IsConstructionDone(__int64 b)
{
    __try {
        return *(float*)(b + OBJ_PROGRESS_OFFSET) >= 1.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Sends every building whose construction controls changed since the last
// scan - switched on AND off: cancelling the money finish or stopping the
// construction used to stay local, so the other side kept building. A
// building seen for the first time is only recorded (a placement with money
// is already covered by the placement message). Only a change right after a
// local click in THIS building's window is sent: the game changes these
// itself too - a finished site clears its money flags, and a new site drawn
// with the road tool can come up stopped - and those used to go out as the
// player's stop / cancel (issue #17), which left the other side's copy
// stuck unfinished. A window's building is remembered for the click window,
// so closing the window right after the click still sends.
static void ScanPayFlags()
{
    static std::unordered_map<__int64, ULONGLONG> s_windowSeenMs; // building -> last seen in an open window
    ULONGLONG nowMs = GetTickCount64();
    __int64 open[OPEN_WINDOWS_MAX];
    int openCount = ReadOpenWindowBuildings(open, OPEN_WINDOWS_MAX);
    for (int i = 0; i < openCount; i++) s_windowSeenMs[open[i]] = nowMs;
    for (auto it = s_windowSeenMs.begin(); it != s_windowSeenMs.end();)
        it = nowMs - it->second > SETTINGS_CLICK_WINDOW_MS ? s_windowSeenMs.erase(it) : std::next(it);

    bool byPlayer = SettingsSync_ClickedRecently();
    static std::vector<__int64> s_list;
    s_list.resize(65536);
    int n = SnapshotBuildings(s_list.data(), (int)s_list.size());

    std::unordered_map<__int64, unsigned char> next;
    next.reserve(n);
    for (int i = 0; i < n; i++) {
        __int64 b = s_list[i];
        int flags = ReadPayFlags(b);
        if (flags < 0) continue;
        auto old = g_payFlags.find(b);
        if (old != g_payFlags.end() && flags != old->second && byPlayer) {
            char was[48], now[48];
            if (!s_windowSeenMs.count(b))
                Logf("republicmp  BUILD_SYNC: pay flags %s -> %s of a building whose window is not open - not sent",
                     PayName(old->second, was, sizeof(was)), PayName(flags, now, sizeof(now)));
            else if (IsConstructionDone(b) && (flags & ~old->second) == 0)
                Logf("republicmp  BUILD_SYNC: pay flags %s -> %s of a finished building (the game's cleanup) - not sent",
                     PayName(old->second, was, sizeof(was)), PayName(flags, now, sizeof(now)));
            else
                SendPay(b, flags);
        }
        next[b] = (unsigned char)flags;
    }
    g_payFlags.swap(next);
}

static bool CallSetPay(__int64 thisPtr, __int64 b, int flags)
{
    typedef void (*SetPayFn)(__int64 thisPtr, __int64 building, unsigned char rub, unsigned char usd);
    __try {
        ((SetPayFn)((uintptr_t)g_exeBase + SET_PAY_RVA))(thisPtr, b, (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallStop(__int64 thisPtr, __int64 b, bool stop)
{
    typedef void (*StopFn)(__int64 thisPtr, __int64 building, unsigned char cancel);
    typedef void (*ResumeFn)(__int64 thisPtr, __int64 building);
    __try {
        if (stop) ((StopFn)((uintptr_t)g_exeBase + STOP_RVA))(thisPtr, b, 1);
        else ((ResumeFn)((uintptr_t)g_exeBase + RESUME_RVA))(thisPtr, b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// "rubles", "dollars, stopped", "none", ...
static const char* PayName(int flags, char* out, size_t outSize)
{
    const char* pay = (flags & 3) == 3 ? "rubles+dollars"
                    : (flags & PAY_RUB) ? "rubles"
                    : (flags & PAY_USD) ? "dollars"
                                        : "none";
    _snprintf_s(out, outSize, _TRUNCATE, "%s%s", pay, (flags & PAY_STOPPED) ? ", stopped" : "");
    return out;
}

static void SendPay(__int64 b, int flags)
{
    float pos[3];
    int category = -1;
    if (!ReadBuildingInfo(b, pos, &category)) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_PAY;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    strcpy_s(msg.typeName, "<pay>");
    msg.x = pos[0]; msg.y = pos[1]; msg.z = pos[2];
    msg.demolishCategory = category;
    msg.speed = (flags & PAY_RUB) ? 1 : 0;
    msg.speedSaved = (flags & PAY_USD) ? 1 : 0;
    msg.speedTurbo = (flags & PAY_STOPPED) ? 1 : 0;
    char name[48];
    Logf("republicmp  BUILD_SYNC: sending pay at (%.2f,%.2f,%.2f) category=%d - finish with %s",
         pos[0], pos[1], pos[2], category, PayName(flags, name, sizeof(name)));
    SendOrQueue(msg);
}

// Diagnostics for "no such building": the nearest local objects of that
// category within NEAREST_LOG_RADIUS, so one test shows where the receiver's
// object really is (path construction sites - category -1 - did not match).
#define NEAREST_LOG_RADIUS 300.0f
#define NEAREST_LOG_COUNT 3

static void LogNearestOfCategory(const float want[3], int category)
{
    static std::vector<__int64> s_list;
    s_list.resize(65536);
    int n = SnapshotBuildings(s_list.data(), (int)s_list.size());
    float bestD[NEAREST_LOG_COUNT];
    float bestP[NEAREST_LOG_COUNT][3];
    int found = 0;
    for (int i = 0; i < n; i++) {
        float pos[3];
        int cat;
        if (!ReadBuildingInfo(s_list[i], pos, &cat) || cat != category) continue;
        float dx = pos[0] - want[0], dz = pos[2] - want[2];
        float d = sqrtf(dx * dx + dz * dz);
        if (d > NEAREST_LOG_RADIUS) continue;
        int k;
        if (found < NEAREST_LOG_COUNT) {
            k = found++; // a free slot
        } else {
            if (d >= bestD[NEAREST_LOG_COUNT - 1]) continue; // not nearer than the farthest kept
            k = NEAREST_LOG_COUNT - 1;                       // replaces the farthest
        }
        // insertion into the list sorted by distance
        while (k > 0 && bestD[k - 1] > d) {
            bestD[k] = bestD[k - 1];
            memcpy(bestP[k], bestP[k - 1], sizeof(bestP[k]));
            k--;
        }
        bestD[k] = d;
        memcpy(bestP[k], pos, sizeof(bestP[k]));
    }
    if (!found) {
        Logf("republicmp  BUILD_SYNC:   no object of category %d within %.0fm here", category, NEAREST_LOG_RADIUS);
        return;
    }
    for (int i = 0; i < found; i++)
        Logf("republicmp  BUILD_SYNC:   nearest category %d here: (%.2f,%.2f,%.2f), %.1fm away", category,
             bestP[i][0], bestP[i][1], bestP[i][2], bestD[i]);
}

static void ApplyOneRemotePay(__int64 thisPtr, const BuildSyncMsg& msg)
{
    int flags = (msg.speed ? PAY_RUB : 0) | (msg.speedSaved ? PAY_USD : 0) | (msg.speedTurbo ? PAY_STOPPED : 0);
    __int64 b = FindBuildingAt(msg.x, msg.y, msg.z, msg.demolishCategory);
    if (!b) {
        Logf("republicmp  BUILD_SYNC: pay at (%.2f,%.2f,%.2f) category=%d - no such building here, skipped",
             msg.x, msg.y, msg.z, msg.demolishCategory);
        float want[3] = { msg.x, msg.y, msg.z };
        LogNearestOfCategory(want, msg.demolishCategory);
        return;
    }
    int before = ReadPayFlags(b);
    bool ok = true;
    if (before != flags) {
        // The same calls the window makes: the money buttons (which also
        // resume a stopped site), then stop or continue.
        TerrainSync_BeginForeignReplay();
        InterlockedIncrement(&g_applyingRemote);
        if (before < 0 || (before & 3) != (flags & 3)) ok = CallSetPay(thisPtr, b, flags);
        int now = ReadPayFlags(b);
        if (ok && now >= 0 && (now & PAY_STOPPED) != (flags & PAY_STOPPED))
            ok = CallStop(thisPtr, b, (flags & PAY_STOPPED) != 0);
        InterlockedDecrement(&g_applyingRemote);
        TerrainSync_EndForeignReplay();
    }
    int after = ReadPayFlags(b);
    if (after >= 0) g_payFlags[b] = (unsigned char)after; // not a local change - do not send it back
    char want[48], was[48], got[48];
    Logf("republicmp  BUILD_SYNC: pay at (%.2f,%.2f,%.2f) category=%d - finish with %s: %s (flags %s -> %s)",
         msg.x, msg.y, msg.z, msg.demolishCategory, PayName(flags, want, sizeof(want)),
         !ok ? "THREW" : before == flags ? "already set" : after == flags ? "applied" : "applied, differs",
         before < 0 ? "?" : PayName(before, was, sizeof(was)), after < 0 ? "?" : PayName(after, got, sizeof(got)));
}

// Replays a remote demolition on the local building at the sender's
// position. Returns false only if the replay threw; a building that is not
// there (already gone) counts as done.
static bool ApplyOneRemoteDemolish(__int64 thisPtr, const BuildSyncMsg& msg)
{
    __int64 found = FindBuildingAt(msg.x, msg.y, msg.z, msg.demolishCategory);
    if (!found) {
        Logf("republicmp  BUILD_SYNC: demolish at (%.2f,%.2f,%.2f) category=%d - no such building here (already gone), skipped",
             msg.x, msg.y, msg.z, msg.demolishCategory);
        return true;
    }

    TerrainSync_BeginForeignReplay();
    InterlockedIncrement(&g_applyingRemote);
    bool ok = DemolishHook_CallOriginal(thisPtr, found, msg);
    InterlockedDecrement(&g_applyingRemote);
    TerrainSync_EndForeignReplay();

    // Removed at once, or (realistic mode) turned into a demolition site
    // that stays in the list until workers finish.
    bool stillListed = false;
    __try {
        const unsigned int kBeginRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
        const unsigned int kEndRvas[2]   = { VEC_BUILDINGS_END_RVA,   VEC_CONSTRUCTIONS_END_RVA };
        for (int v = 0; v < 2 && !stillListed; ++v) {
            __int64* it = *(__int64**)((uintptr_t)g_exeBase + kBeginRvas[v]);
            __int64* end = *(__int64**)((uintptr_t)g_exeBase + kEndRvas[v]);
            for (; it && it < end; ++it)
                if (*it == found) { stillListed = true; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Logf("republicmp  BUILD_SYNC: demolish at (%.2f,%.2f,%.2f) category=%d args=[%llx,%llx,%llx,%llx,%llx] eb1=%u shift=%u ctrl=%u %s",
         msg.x, msg.y, msg.z, msg.demolishCategory,
         msg.demolishArgs[0] & 0xff, msg.demolishArgs[1] & 0xff, msg.demolishArgs[2] & 0xff,
         msg.demolishArgs[3] & 0xff, msg.demolishArgs[4] & 0xff,
         msg.demolishEb1, msg.demolishShift, msg.demolishCtrl,
         !ok ? "THREW" : stillListed ? "applied - building STILL in the list (demolition site?)" : "applied - building removed");
    return ok;
}

// ---------------------------------------------------------------------------
// Lockstep commands (see build_sync.h)
// ---------------------------------------------------------------------------
//
// The placement is the building tool's own click frame: 2bade0 as the tool
// calls it (param_4 = 0, param_8 = 0) with the button pressed this frame and
// the clicking PC's cursor, yaw and mirror. param_4 = 0 is what lays the
// building's access roads (the loop over the type's connection points at
// type+0x2f8, 2bb563), so each PC's engine routes the same roads itself;
// param_4 = 1 (the replay above) skips them. The no-click call with param_4 =
// 0, param_8 = 1 (4e4cb0's second branch) returned before validating
// anything (result -1,-1,0), as the click path reads the mouse. When the
// click frame builds nothing, the replay's call places the building without
// its roads - the same outcome on every PC either way. No rotation is written
// afterwards: every PC smooths the same yaw input alike.

#define LMB_HELD_RVA 0xA54E90         // byte: left mouse button held
#define LMB_JUST_PRESSED_RVA 0xA54E91 // byte: pressed this frame

bool BuildSync_ApplyPlacementCommand(const BuildSyncMsg& msg)
{
    __int64 thisPtr = GetPlacementManagerThisPtr();
    bool placed = false;
    __try {
        float* cursor = (float*)(thisPtr + CURSOR_X_OFFSET);
        float* cursorCopy = (float*)(thisPtr + CURSOR_COPY_OFFSET);
        float* yaw = (float*)(thisPtr + CURSOR_YAW_OFFSET);
        unsigned char* mirror = (unsigned char*)(thisPtr + MIRROR_TOGGLE_OFFSET);
        unsigned char* held = (unsigned char*)((uintptr_t)g_exe + LMB_HELD_RVA);
        unsigned char* pressed = (unsigned char*)((uintptr_t)g_exe + LMB_JUST_PRESSED_RVA);
        __int64* selected = (__int64*)(thisPtr + SELECTED_ENTRY_OFFSET);
        float savedCursor[3] = { cursor[0], cursor[1], cursor[2] };
        float savedCopy[3] = { cursorCopy[0], cursorCopy[1], cursorCopy[2] };
        float savedYaw = *yaw;
        unsigned char savedMirror = *mirror, savedHeld = *held, savedPressed = *pressed;
        __int64 savedSelected = *selected;
        // The clicking PC's pick modes (see BuildSyncMsg::pickModes).
        unsigned char* pickModes[2] = { (unsigned char*)(thisPtr + 0xF600), (unsigned char*)(thisPtr + 0xF698) };
        unsigned char savedPickModes[2] = { *pickModes[0], *pickModes[1] };
        // This PC's open menus would make the placement return at once
        // (replay_menus.h); they read closed for the call.
        unsigned char* menus = (unsigned char*)thisPtr;
        unsigned char savedMenus[REPLAY_UI_FLAG_COUNT];
        for (int i = 0; i < REPLAY_UI_FLAG_COUNT; i++) savedMenus[i] = menus[kReplayUiFlagOffsets[i]];
        BuildMode mine = BuildMode_Read();
        char typeName[64];
        strncpy_s(typeName, msg.typeName, _TRUNCATE);
        __try {
            cursor[0] = cursorCopy[0] = msg.cursorX;
            cursor[1] = cursorCopy[1] = msg.cursorY;
            cursor[2] = cursorCopy[2] = msg.cursorZ;
            *yaw = msg.cursorRot;
            *mirror = msg.mirrored ? 1 : 0;
            *held = 0;
            *pressed = 0;
            *pickModes[0] = (msg.pickModes & 1) ? 1 : 0;
            *pickModes[1] = (msg.pickModes & 2) ? 1 : 0;
            BuildMode_Write(msg.mode);
            memset(g_replayEntry, 0, sizeof(g_replayEntry));
            memcpy(g_replayEntry, typeName, strnlen(typeName, 63));
            *selected = (__int64)g_replayEntry;
            for (int i = 0; i < REPLAY_UI_FLAG_COUNT; i++) menus[kReplayUiFlagOffsets[i]] = 0;
            SnapshotBeforePlacement();
            // The access road's planner reads what this PC's mouse hovers (the clicking PC's road under
            // the cursor, the other PC's whatever it points at): one PC laid two pieces, the other one.
            RoadSync_ClearHover();
            bool scoped = ViewListScope_Begin(msg.cursorX - PLACE_SCOPE_MARGIN, msg.cursorZ - PLACE_SCOPE_MARGIN,
                                              msg.cursorX + PLACE_SCOPE_MARGIN, msg.cursorZ + PLACE_SCOPE_MARGIN);
            LocalBuilds_BeginBuildingPlacement();
            InterlockedIncrement(&g_applyingRemote);
            const int* result = (const int*)(thisPtr + PLACEMENT_RESULT_OFFSET);
            int clickResult[3] = { 0, 0, 0 };
            int newCount = 0;
            float delta = 0.0f;
            bool withRoads = false;
            __try {
                *pressed = 1;
                g_origUniversal(thisPtr, typeName, 0, 0, msg.param5, 0xffffffff, 0, 0);
                *pressed = 0;
                memcpy(clickResult, result, sizeof(clickResult));
                placed = withRoads = PickMainNewObject(msg.cursorX, msg.cursorZ, &newCount, &delta) != nullptr;
                if (!placed) {
                    SnapshotBeforePlacement();
                    g_origUniversal(thisPtr, typeName, 0, 1, msg.param5, 0xffffffff, 0, 1);
                    placed = PickMainNewObject(msg.cursorX, msg.cursorZ, &newCount, &delta) != nullptr;
                }
            } __finally {
                *pressed = 0;
                InterlockedDecrement(&g_applyingRemote);
                LocalBuilds_EndBuildingPlacement();
                if (scoped) ViewListScope_End();
            }
            char modeText[96];
            char made[320];
            DescribeNewObjects(msg.cursorX, msg.cursorZ, made, sizeof(made));
            Logf("republicmp  BUILD_SYNC: command placement type=[%s] pay=%s cursor=(%.2f,%.2f,%.2f) - %s (objects %d, click result %d,%d,%d, result %d,%d,%d) %s",
                 typeName, BuildMode_Describe(msg.mode, modeText, sizeof(modeText)), msg.cursorX, msg.cursorY, msg.cursorZ,
                 withRoads ? "placed by the click frame" : placed ? "placed by the replay call, without access roads" : "refused by the engine",
                 newCount, clickResult[0], clickResult[1], clickResult[2], result[0], result[1], result[2], made);
        } __finally {
            cursor[0] = savedCursor[0]; cursor[1] = savedCursor[1]; cursor[2] = savedCursor[2];
            cursorCopy[0] = savedCopy[0]; cursorCopy[1] = savedCopy[1]; cursorCopy[2] = savedCopy[2];
            *yaw = savedYaw;
            *mirror = savedMirror;
            *held = savedHeld;
            *pressed = savedPressed;
            *selected = savedSelected;
            *pickModes[0] = savedPickModes[0];
            *pickModes[1] = savedPickModes[1];
            for (int i = 0; i < REPLAY_UI_FLAG_COUNT; i++) menus[kReplayUiFlagOffsets[i]] = savedMenus[i];
            BuildMode_Write(mine);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  BUILD_SYNC: <exception making a placement command>");
    }
    return placed;
}

bool BuildSync_ApplyDemolishCommand(const BuildSyncMsg& msg)
{
    return ApplyOneRemoteDemolish(GetPlacementManagerThisPtr(), msg);
}

// A received placement/demolition waiting to be applied.
struct PendingPlacement {
    BuildSyncMsg msg;
    std::shared_ptr<RoadSyncSegment> road; // BUILD_SYNC_KIND_ACCESS_ROAD
    int attempts;               // failed attempts so far
    unsigned long long notBeforeMs; // GetTickCount64() of the next try
    unsigned long long fenceWaitStartMs; // 0 = not waiting for the terrain fence yet
};
static std::vector<PendingPlacement> g_pending;

// Buildings whose placement was given up (sender's main object position).
// Their access road comes right after them; built alone it is a path to
// nowhere that the sender doesn't have.
#define GAVE_UP_KEEP 16
static float g_gaveUpAt[GAVE_UP_KEEP][3];
static int g_gaveUpCount = 0;

static bool PortOnGivenUpBuilding(const RoadSyncBuildingPort& port)
{
    if (!port.valid) return false;
    for (int i = 0; i < g_gaveUpCount && i < GAVE_UP_KEEP; i++)
        if (fabsf(g_gaveUpAt[i][0] - port.bldPos[0]) < 0.5f && fabsf(g_gaveUpAt[i][2] - port.bldPos[2]) < 0.5f &&
            fabsf(g_gaveUpAt[i][1] - port.bldPos[1]) < 1.0f)
            return true;
    return false;
}
static bool g_warnedWorldNotLoaded = false;

void BuildSync_Tick()
{
    EnterCriticalSection(&g_queueLock);
    std::vector<BuildSyncMsg> speedMsgs;
    for (const BuildWireItem& in : g_incoming) {
        const BuildSyncMsg& msg = in.msg;
        // Speed is not world state - applied at once, never behind a
        // placement waiting for its terrain or a retry.
        if (msg.kind == BUILD_SYNC_KIND_SPEED_SET || msg.kind == BUILD_SYNC_KIND_SPEED_REQUEST ||
            msg.kind == BUILD_SYNC_KIND_SPEED_BOOST || msg.kind == BUILD_SYNC_KIND_TIME ||
            msg.kind == BUILD_SYNC_KIND_MONEY || msg.kind == BUILD_SYNC_KIND_SETTING ||
            msg.kind == BUILD_SYNC_KIND_WEATHER || msg.kind == BUILD_SYNC_KIND_STATE) {
            speedMsgs.push_back(msg);
            continue;
        }
        PendingPlacement p = { msg, in.road, 0, 0, 0 };
        g_pending.push_back(p);
    }
    g_incoming.clear();
    LeaveCriticalSection(&g_queueLock);

    for (const BuildSyncMsg& m : speedMsgs) {
        if (m.kind == BUILD_SYNC_KIND_TIME) GameSpeed_OnRemoteTime(m.speed, m.speedSaved, m.timeProgress);
        else if (m.kind == BUILD_SYNC_KIND_STATE) SyncCheck_OnRemote(m.state);
        else if (m.kind == BUILD_SYNC_KIND_WEATHER) Weather_OnRemote(m.weather);
        else if (m.kind == BUILD_SYNC_KIND_MONEY) Money_OnRemote(m.moneyRub, m.moneyUsd);
        else if (m.kind == BUILD_SYNC_KIND_SETTING) SettingsSync_OnRemote(m.setting);
        else GameSpeed_OnRemote(m.kind, m.speed, m.speedSaved, m.speedTurbo);
    }

    FlushOutbound();

    // Pay flags: only while connected and in the world (the sync screen and
    // a fresh load would otherwise look like changes).
    if (g_connected && !SyncBlock_IsActive() && IsWorldLoaded()) {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - g_lastPayScanMs >= PAY_SCAN_INTERVAL_MS) {
            g_lastPayScanMs = nowMs;
            ScanPayFlags();
        }
    } else {
        g_payFlags.clear();
    }

    if (g_pending.empty()) return;

    if (!IsWorldLoaded()) {
        if (!g_warnedWorldNotLoaded) {
            Logf("republicmp  BUILD_SYNC: %zu remote placement(s) held until the world is loaded", g_pending.size());
            g_warnedWorldNotLoaded = true;
        }
        return;
    }
    g_warnedWorldNotLoaded = false;

    __int64 thisPtr = GetPlacementManagerThisPtr();

    // Applied in arrival order; a refused placement is retried after
    // RETRY_DELAY_MS, and later ones wait behind it so order is kept.
    unsigned long long now = GetTickCount64();
    // A window or menu open on this PC no longer holds the other player's
    // buildings (replay_menus.h); the join screen still does.
    if (SyncBlock_IsActive()) return;
    ReplayUiScope replayUi;
    static bool s_loggedWindow = false;
    if (replayUi.WasBlocked() != s_loggedWindow) {
        s_loggedWindow = replayUi.WasBlocked();
        if (s_loggedWindow) Logf("republicmp  BUILD_SYNC: a window is open here - replaying with menus closed");
    }
    while (!g_pending.empty()) {
        PendingPlacement& p = g_pending.front();
        if (now < p.notBeforeMs) break;
        if (!TerrainSync_FenceReached(p.msg.terrainFence)) {
            if (!p.fenceWaitStartMs) p.fenceWaitStartMs = now;
            if (now - p.fenceWaitStartMs < BUILD_SYNC_FENCE_TIMEOUT_MS) break;
            Logf("republicmp  BUILD_SYNC: terrain fence %d not reached after %dms - applying anyway",
                 p.msg.terrainFence, BUILD_SYNC_FENCE_TIMEOUT_MS);
        }
        // Construction controls do not go through the placement function, so
        // the game's UI blocking it (any open window - the construction
        // site's own included, which is exactly where a player watches
        // them) must not hold them: that delayed "stop" / "finish with
        // money" until the receiver closed the window.
        if (p.msg.kind == BUILD_SYNC_KIND_PAY) {
            ApplyOneRemotePay(thisPtr, p.msg);
            g_pending.erase(g_pending.begin());
            continue;
        }
        // Everything else stays queued (in order) while the placement
        // function is blocked.
        if (PlacementGuardBusy(thisPtr)) break;
        if (p.msg.kind == BUILD_SYNC_KIND_DEMOLISH) {
            ApplyOneRemoteDemolish(thisPtr, p.msg);
            g_pending.erase(g_pending.begin());
            continue;
        }
        if (p.msg.kind == BUILD_SYNC_KIND_ACCESS_ROAD) {
            // Not into a manager the local player is dragging with.
            if (LocalBuilds_IsToolDragActive()) break;
            if (p.road && (PortOnGivenUpBuilding(p.road->startPort) || PortOnGivenUpBuilding(p.road->endPort))) {
                Logf("republicmp  BUILD_SYNC: access road at (%.2f,%.2f,%.2f) skipped - its building was not placed here",
                     p.msg.x, p.msg.y, p.msg.z);
                g_pending.erase(g_pending.begin());
                continue;
            }
            int selfRoads = p.road ? SelfConnectedRoads(p.road->startPort) + SelfConnectedRoads(p.road->endPort) : 0;
            if (selfRoads > 0 && p.road->category == ROAD_SYNC_CATEGORY_ROAD) {
                Logf("republicmp  BUILD_SYNC: access road at (%.2f,%.2f,%.2f) skipped - the placement already laid %d road segment(s) for its building here%s",
                     p.msg.x, p.msg.y, p.msg.z, selfRoads,
                     RoadSync_SegmentExists(*p.road) ? " (same as the sender's)" : " (DIFFERENT route from the sender's)");
                g_pending.erase(g_pending.begin());
                continue;
            }
            BuildMode mine = BuildMode_Read();
            BuildMode_Write(p.msg.mode);
            int pathsBefore = RoadSync_SegmentCount(thisPtr, ROAD_SYNC_CATEGORY_PEDESTRIAN);
            bool ok = p.road && RoadSync_ApplySegmentForBuild(*p.road);
            int pathsAdded = RoadSync_SegmentCount(thisPtr, ROAD_SYNC_CATEGORY_PEDESTRIAN) - pathsBefore;
            BuildMode_Write(mine);
            char theirsText[96], mineText[96];
            Logf("republicmp  BUILD_SYNC: access road %s at (%.2f,%.2f,%.2f) %s, footpath segments %+d, pay=%s (mine: %s)",
                 p.road ? (p.road->category == ROAD_SYNC_CATEGORY_PEDESTRIAN ? "FOOTPATH" : "ROAD") : "?",
                 p.msg.x, p.msg.y, p.msg.z, ok ? "built" : "REJECTED by the engine", pathsAdded,
                 BuildMode_Describe(p.msg.mode, theirsText, sizeof(theirsText)),
                 BuildMode_Describe(mine, mineText, sizeof(mineText)));
            g_pending.erase(g_pending.begin());
            continue;
        }
        if (ApplyOneRemotePlacement(thisPtr, p.msg, p.attempts + 1)) {
            g_pending.erase(g_pending.begin());
            continue;
        }
        if (++p.attempts >= BUILD_SYNC_MAX_APPLY_ATTEMPTS) {
            BuildSyncStatus("[BUILD_SYNC] gave up on [%s] at (%.1f, %.1f) after %d attempts",
                            p.msg.typeName, p.msg.cursorX, p.msg.cursorZ, p.attempts);
            float* at = g_gaveUpAt[g_gaveUpCount++ % GAVE_UP_KEEP];
            at[0] = p.msg.x; at[1] = p.msg.y; at[2] = p.msg.z;
            g_pending.erase(g_pending.begin());
            continue;
        }
        p.notBeforeMs = now + RETRY_DELAY_MS;
        break;
    }
}
