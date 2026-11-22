// SPDX-License-Identifier: GPL-3.0-only
// LAN discovery (UDP broadcast + directed per-NIC + 127.0.0.1) and multi-NIC /
// virtual VPN adapter enumeration for RepublicMP.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mswsock.h> // SIO_UDP_CONNRESET
#include <windows.h>

#include "lan_discovery.h"
#include "steam_p2p.h"
#include "../core/version.h"
#include "../core/safe_mem.h"
#include "../core/diag_log.h"
#include "../session/save_sync.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {

static constexpr char kProbeMagic[8]    = { 'R', 'P', 'M', 'P', 'P', 'R', 'B', '1' };
static constexpr char kAnnounceMagic[8] = { 'R', 'P', 'M', 'P', 'A', 'N', 'N', '1' };
static constexpr uint64_t kLanBeaconIntervalMs   = 1500ULL;
static constexpr uint64_t kLanAdapterCacheMs     = 5000ULL;
static constexpr uint64_t kLanPathSilentSwitchMs = 3000ULL;
static constexpr size_t   kMaxRecentProbePeers   = 32;
static constexpr int      kMaxDatagramsPerWake   = 64;

struct LanThreadContext {
    std::atomic<bool> stop{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> probeNow{false};
    HANDLE wakeEvent = nullptr;
    uint16_t bindPort = LAN_DISCOVERY_PORT;

    ~LanThreadContext()
    {
        if (wakeEvent) {
            CloseHandle(wakeEvent);
            wakeEvent = nullptr;
        }
    }
};

// Lock order: g_lifecycleCs (outer, lifecycle calls only) -> g_lanCs (inner, short data guards).
// Worker threads never acquire g_lifecycleCs.
static INIT_ONCE        g_lifecycleOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lifecycleCs;

static INIT_ONCE        g_lanOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lanCs;

static bool       g_initialized       = false;
static bool       g_wsaStarted        = false;
static uint64_t   g_localServerGuid   = 0;
static uint16_t   g_testPortOverride  = 0;
static bool       g_selfPidOverrideOn = false;
static uint32_t   g_selfPidOverride   = 0;
static std::atomic<bool> g_loopbackOnlyForTest{false};
static std::atomic<bool> g_processShuttingDown{false};

static bool       g_hostingOpen       = false;
static uint16_t   g_hostingTcpPort    = SAVE_SYNC_PORT;
static uint8_t    g_hostingMode       = 0;
static char       g_hostingName[48]   = {};
static char       g_hostingMap[48]    = {};

static HANDLE                            g_hostThread = nullptr;
static std::shared_ptr<LanThreadContext> g_hostCtx;

static HANDLE                            g_scanThread = nullptr;
static std::shared_ptr<LanThreadContext> g_scanCtx;

static std::vector<LanHostEntry>   g_hosts;
static std::vector<LanAdapterInfo> g_cachedAdapters;
static bool                        g_cachedAdaptersValid = false;
static uint64_t                    g_cachedAdaptersAtMs = 0;
static std::string                 g_lastLoggedAdaptersSummary;
static std::vector<sockaddr_in>    g_recentProbePeers;

static std::string FormatAdaptersSummary(const std::vector<LanAdapterInfo>& adapters)
{
    if (adapters.empty()) {
        return "enumerated 0 non-loopback IPv4 adapters";
    }
    std::string out = "enumerated " + std::to_string(adapters.size()) + " non-loopback IPv4 adapter(s): ";
    for (size_t i = 0; i < adapters.size(); ++i) {
        if (i > 0) out += ", ";
        char item[96] = {};
        _snprintf_s(item, sizeof(item), _TRUNCATE, "%s[%s]",
                    adapters[i].ip, adapters[i].name[0] ? adapters[i].name : "NIC");
        if (out.size() + strlen(item) + 8u > 420u) {
            out += "...";
            break;
        }
        out += item;
    }
    return out;
}

static void LanLogf(const char* fmt, ...)
{
    char buf[512] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (H && H->log) {
        H->log("%s", buf);
    }
    DiagLog_Printf("LAN", "%s", buf);
}

static std::string AsciiLowerString(const char* s)
{
    std::string out;
    if (!s) return out;
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        out.push_back((char)std::tolower(*p));
    }
    return out;
}

static bool IsAsciiAlnum(char c)
{
    const unsigned char uc = (unsigned char)c;
    return (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9');
}

static bool IsAsciiDigit(char c)
{
    const unsigned char uc = (unsigned char)c;
    return uc >= '0' && uc <= '9';
}

static bool ContainsAdapterToken(const std::string& haystack, const char* token)
{
    if (haystack.empty() || !token || !token[0]) return false;
    const size_t tokenLen = strlen(token);
    size_t pos = 0;
    while ((pos = haystack.find(token, pos)) != std::string::npos) {
        const bool leftOk = (pos == 0) || !IsAsciiAlnum(haystack[pos - 1]);
        const size_t afterPos = pos + tokenLen;
        const bool rightOk = (afterPos == haystack.size()) ||
                             IsAsciiDigit(haystack[afterPos]) ||
                             !IsAsciiAlnum(haystack[afterPos]);
        if (leftOk && rightOk) {
            return true;
        }
        pos += 1;
    }
    return false;
}

static bool ContainsAnyKeyword(const std::string& haystack, const char* const* keywords, size_t count)
{
    if (haystack.empty()) return false;
    for (size_t i = 0; i < count; ++i) {
        if (haystack.find(keywords[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static int PriorityForAdapterKind(LanAdapterKind kind)
{
    switch (kind) {
        case LanAdapterKind::Ethernet:     return 400;
        case LanAdapterKind::WiFi:         return 300;
        case LanAdapterKind::VirtualVpn:   return 200;
        case LanAdapterKind::Unknown:      return 100;
        case LanAdapterKind::VirtualLocal: return 50;
    }
    return 100;
}

static bool ParseIpv4HostOrder(const char* ipStr, uint32_t* outHostOrder)
{
    if (!ipStr || !ipStr[0]) return false;
    in_addr addr = {};
    if (inet_pton(AF_INET, ipStr, &addr) != 1) return false;
    if (outHostOrder) *outHostOrder = ntohl(addr.s_addr);
    return true;
}

static bool FormatIpv4HostOrder(uint32_t ipHostOrder, char* outBuf, size_t outCap)
{
    if (!outBuf || outCap < 16) return false;
    in_addr addr = {};
    addr.s_addr = htonl(ipHostOrder);
    return inet_ntop(AF_INET, &addr, outBuf, (socklen_t)outCap) != nullptr;
}

static size_t TrimTrailingIncompleteUtf8(const char* buf, size_t len)
{
    while (len > 0) {
        unsigned char last = (unsigned char)buf[len - 1];
        if (last < 0x80u) break;
        size_t contCount = 0;
        while (contCount < len && contCount < 3u &&
               (((unsigned char)buf[len - 1 - contCount]) & 0xC0u) == 0x80u) {
            ++contCount;
        }
        if (contCount == len) {
            len = 0;
            break;
        }
        size_t leadPos = len - 1 - contCount;
        unsigned char lead = (unsigned char)buf[leadPos];
        size_t expected = 0;
        if (lead >= 0xC2u && lead <= 0xDFu) expected = 2;
        else if (lead >= 0xE0u && lead <= 0xEFu) expected = 3;
        else if (lead >= 0xF0u && lead <= 0xF4u) expected = 4;

        size_t have = len - leadPos;
        if (expected == 0 || have < expected) {
            len = leadPos;
            continue;
        }
        if (have > expected) {
            len = leadPos + expected;
        }
        break;
    }
    return len;
}

static void SanitizeWireString(const char* src, size_t srcCap, char* dst, size_t dstCap)
{
    if (!dst || dstCap == 0) return;
    memset(dst, 0, dstCap);
    if (!src || srcCap == 0 || dstCap == 1) return;

    size_t maxCopy = (srcCap < (dstCap - 1)) ? srcCap : (dstCap - 1);
    size_t outIdx = 0;
    for (size_t i = 0; i < maxCopy; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0) break;
        dst[outIdx++] = (c < 0x20u || c == 0x7Fu) ? ' ' : (char)c;
    }
    while (outIdx > 0 && outIdx <= maxCopy && dst[outIdx - 1] == ' ') {
        dst[--outIdx] = '\0';
    }
    size_t safeIdx = TrimTrailingIncompleteUtf8(dst, outIdx);
    while (outIdx > safeIdx) {
        dst[--outIdx] = '\0';
    }
    while (outIdx > 0 && dst[outIdx - 1] == ' ') {
        dst[--outIdx] = '\0';
    }
}

static std::string WideToUtf8(const wchar_t* ws)
{
    if (!ws || !ws[0]) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return std::string();
    std::string out((size_t)(needed - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), needed, nullptr, nullptr) <= 0) {
        return std::string();
    }
    return out;
}

static bool IsVpnIpRange(uint32_t ipHostOrder)
{
    uint32_t b0 = (ipHostOrder >> 24) & 0xFFu;
    uint32_t b1 = (ipHostOrder >> 16) & 0xFFu;
    if (b0 == 26u || b0 == 25u) return true; // Radmin VPN (26.0.0.0/8), Hamachi (25.0.0.0/8)
    if (b0 == 100u && (b1 & 0xC0u) == 64u) return true; // Tailscale / CGNAT (100.64.0.0/10)
    return false;
}

static uint64_t GenerateServerGuid()
{
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    uint64_t x = (uint64_t)GetTickCount64() ^ ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)qpc.QuadPart;
    x ^= (x >> 30);
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= (x >> 27);
    x *= 0x94d049bb133111ebULL;
    x ^= (x >> 31);
    return (x != 0) ? x : 0x52504D504C414E31ULL;
}

static uint16_t EffectiveDiscoveryPortLocked()
{
    return (g_testPortOverride != 0) ? g_testPortOverride : LAN_DISCOVERY_PORT;
}

static bool EnsureLanInitializedLocked()
{
    if (g_processShuttingDown.load()) return false;
    if (!g_initialized) {
        WSADATA wsa = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        g_wsaStarted = true;
        if (g_localServerGuid == 0) {
            g_localServerGuid = GenerateServerGuid();
        }
        g_initialized = true;
    }
    return true;
}

static bool IsLoopbackIpv4HostOrder(uint32_t ipHostOrder)
{
    return ((ipHostOrder >> 24) & 0xFFu) == 127u;
}

static SOCKET CreateLanUdpSocket(uint16_t bindPort, bool reuseAddr)
{
    const bool loopbackOnly = g_loopbackOnlyForTest.load();
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    if (reuseAddr) {
        BOOL reuse = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    }
    if (!loopbackOnly) {
        BOOL broadcast = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));
    }

    BOOL newBehavior = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior),
             nullptr, 0, &bytesReturned, nullptr, nullptr);

    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(bindPort);
    bindAddr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(sock, (const sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        const int bindErr = WSAGetLastError();
        closesocket(sock);
        WSASetLastError(bindErr);
        return INVALID_SOCKET;
    }
    return sock;
}

static void SendUdpToAddress(SOCKET sock, const void* data, int len, const char* ipStr, uint16_t destPort)
{
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(destPort);
    if (inet_pton(AF_INET, ipStr, &dst.sin_addr) != 1) return;
    sendto(sock, (const char*)data, len, 0, (const sockaddr*)&dst, sizeof(dst));
}

static void SendUdpToDiscoveryTargets(SOCKET sock, const void* data, int len, uint16_t destPort,
                                      const std::vector<LanAdapterInfo>& adapters,
                                      const std::vector<sockaddr_in>* extraPeers)
{
    if (sock == INVALID_SOCKET || !data || len <= 0 || destPort == 0) return;

    const bool loopbackOnly = g_loopbackOnlyForTest.load();
    const std::vector<LanDiscoveryTarget> targets =
        LanDiscovery_PlanSendTargets(adapters, destPort, {}, 0, loopbackOnly);

    DWORD currentIfNet = 0xFFFFFFFFu;
    for (const auto& target : targets) {
        if (!loopbackOnly) {
            const DWORD desiredIfNet = (target.ifIndex != 0) ? htonl(target.ifIndex) : 0u;
            if (desiredIfNet != currentIfNet) {
                if (setsockopt(sock, IPPROTO_IP, IP_UNICAST_IF, (const char*)&desiredIfNet, sizeof(desiredIfNet)) == 0) {
                    currentIfNet = desiredIfNet;
                }
            }
        }
        SendUdpToAddress(sock, data, len, target.ip, target.port);
    }

    if (!loopbackOnly && currentIfNet != 0u) {
        DWORD resetIf = 0;
        setsockopt(sock, IPPROTO_IP, IP_UNICAST_IF, (const char*)&resetIf, sizeof(resetIf));
    }

    if (extraPeers) {
        for (const auto& peer : *extraPeers) {
            if (peer.sin_family != AF_INET || peer.sin_port == 0) continue;
            if (loopbackOnly && !IsLoopbackIpv4HostOrder(ntohl(peer.sin_addr.s_addr))) continue;
            sendto(sock, (const char*)data, len, 0, (const sockaddr*)&peer, sizeof(peer));
        }
    }
}

static LanAnnouncePacket BuildLocalAnnouncePacketLocked(bool open)
{
    LanAnnouncePacket pkt = {};
    memcpy(pkt.magic, kAnnounceMagic, sizeof(kAnnounceMagic));
    pkt.protocol = (uint16_t)REPUBLICMP_PROTOCOL;
    pkt.tcpPort = g_hostingTcpPort;
    pkt.serverGuid = g_localServerGuid;
    pkt.hostPid = GetCurrentProcessId();
    pkt.open = open ? 1u : 0u;
    pkt.twoStatesMode = g_hostingMode;
    pkt.hasPassword = 0;
    pkt.steamOnline = SteamP2P_IsAvailable() ? 1u : 0u;
    pkt.dllCrc = (uint32_t)SaveSync_GetLocalDllCrc();
    SanitizeWireString(REPUBLICMP_VERSION, sizeof(REPUBLICMP_VERSION), pkt.version, sizeof(pkt.version));
    SanitizeWireString(g_hostingName, sizeof(g_hostingName), pkt.hostName, sizeof(pkt.hostName));
    SanitizeWireString(g_hostingMap, sizeof(g_hostingMap), pkt.mapName, sizeof(pkt.mapName));
    return pkt;
}

static void RecordProbePeer(std::vector<sockaddr_in>& peers, const sockaddr_in& peer)
{
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        if (it->sin_addr.s_addr == peer.sin_addr.s_addr && it->sin_port == peer.sin_port) {
            peers.erase(it);
            peers.push_back(peer);
            return;
        }
    }
    if (peers.size() >= kMaxRecentProbePeers) {
        peers.erase(peers.begin());
    }
    peers.push_back(peer);
}

static int RankForHostPath(const char* connectIp, LanAdapterKind kind)
{
    if (connectIp && strcmp(connectIp, "127.0.0.1") == 0) {
        return 1000;
    }
    if (kind != LanAdapterKind::Unknown) {
        return PriorityForAdapterKind(kind);
    }
    uint32_t ipHost = 0;
    if (ParseIpv4HostOrder(connectIp, &ipHost) && IsVpnIpRange(ipHost)) {
        return PriorityForAdapterKind(LanAdapterKind::VirtualVpn);
    }
    return PriorityForAdapterKind(LanAdapterKind::Unknown);
}

static bool IsSameMachineSourceIp(const char* peerIp, const std::vector<LanAdapterInfo>& adapters)
{
    if (!peerIp || !peerIp[0]) return false;
    if (strcmp(peerIp, "127.0.0.1") == 0) return true;
    for (const auto& adapter : adapters) {
        if (strcmp(peerIp, adapter.ip) == 0) return true;
    }
    return false;
}

static LanAdapterKind ResolvePathKindFromAdapters(const char* peerIp, const std::vector<LanAdapterInfo>& adapters)
{
    uint32_t peerHost = 0;
    if (!ParseIpv4HostOrder(peerIp, &peerHost)) return LanAdapterKind::Unknown;
    for (const auto& adapter : adapters) {
        uint32_t adIp = 0;
        uint32_t adMask = 0;
        if (ParseIpv4HostOrder(adapter.ip, &adIp) && ParseIpv4HostOrder(adapter.mask, &adMask)) {
            if ((peerHost & adMask) == (adIp & adMask)) {
                return adapter.kind;
            }
        }
    }
    if (IsVpnIpRange(peerHost)) return LanAdapterKind::VirtualVpn;
    return LanAdapterKind::Unknown;
}

static bool PollUdpReadable(SOCKET sock, DWORD waitMs)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval tv = {};
    tv.tv_sec = (long)(waitMs / 1000u);
    tv.tv_usec = (long)((waitMs % 1000u) * 1000u);
    return select(0, &readSet, nullptr, nullptr, &tv) > 0;
}

static bool StopManagedThread(HANDLE& hThread, std::shared_ptr<LanThreadContext>& ctx)
{
    if (ctx) {
        ctx->stop.store(true);
        if (ctx->wakeEvent) {
            SetEvent(ctx->wakeEvent);
        }
    }
    HANDLE threadCopy = hThread;
    hThread = nullptr;
    bool stopped = true;
    if (threadCopy) {
        DWORD waitRes = WaitForSingleObject(threadCopy, 2500);
        stopped = (waitRes == WAIT_OBJECT_0);
        CloseHandle(threadCopy);
    }
    ctx.reset();
    return stopped;
}

static bool StartManagedThread(HANDLE& hThread, std::shared_ptr<LanThreadContext>& ctx,
                               LPTHREAD_START_ROUTINE proc, uint16_t bindPort, bool initialProbe)
{
    if (hThread && ctx && ctx->exited.load()) {
        StopManagedThread(hThread, ctx);
    }
    if (hThread && ctx) {
        if (initialProbe) ctx->probeNow.store(true);
        if (ctx->wakeEvent) SetEvent(ctx->wakeEvent);
        return true;
    }

    std::shared_ptr<LanThreadContext> newCtx = std::make_shared<LanThreadContext>();
    newCtx->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!newCtx->wakeEvent) return false;
    newCtx->bindPort = bindPort;
    newCtx->probeNow.store(initialProbe);

    auto* heapCtx = new std::shared_ptr<LanThreadContext>(newCtx);
    HANDLE h = CreateThread(nullptr, 0, proc, heapCtx, 0, nullptr);
    if (!h) {
        delete heapCtx;
        return false;
    }
    ctx = std::move(newCtx);
    hThread = h;
    if (ctx->wakeEvent) SetEvent(ctx->wakeEvent);
    return true;
}

static void HandleResponderReadable(SOCKET sock, const LanThreadContext* ctx)
{
    for (int drained = 0; drained < kMaxDatagramsPerWake && (!ctx || !ctx->stop.load()); ++drained) {
        uint8_t buf[256] = {};
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int rx = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&fromAddr, &fromLen);
        if (rx == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEMSGSIZE) continue;
            break;
        }
        if (rx == 0) continue;

        LanProbePacket probe = {};
        if (!LanDiscovery_ParseProbe(buf, (size_t)rx, &probe)) continue;

        LanAnnouncePacket reply = {};
        bool openNow = false;
        {
            TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
            openNow = g_hostingOpen;
            if (openNow) {
                RecordProbePeer(g_recentProbePeers, fromAddr);
                reply = BuildLocalAnnouncePacketLocked(true);
            }
        }
        if (openNow) {
            const bool loopbackOnly = g_loopbackOnlyForTest.load();
            if (!loopbackOnly || IsLoopbackIpv4HostOrder(ntohl(fromAddr.sin_addr.s_addr))) {
                uint8_t wire[sizeof(LanAnnouncePacket)] = {};
                size_t wireLen = LanDiscovery_SerializeAnnounce(reply, wire, sizeof(wire));
                if (wireLen > 0) {
                    sendto(sock, (const char*)wire, (int)wireLen, 0, (const sockaddr*)&fromAddr, fromLen);
                }
            }
        }
    }
}

static DWORD WINAPI ResponderThreadProc(LPVOID lpParam)
{
    std::unique_ptr<std::shared_ptr<LanThreadContext>> paramPtr(
        static_cast<std::shared_ptr<LanThreadContext>*>(lpParam));
    if (!paramPtr || !*paramPtr) return 0;
    std::shared_ptr<LanThreadContext> ctx = *paramPtr;
    paramPtr.reset();

    struct ExitFlagGuard {
        LanThreadContext* c;
        ~ExitFlagGuard() { if (c) c->exited.store(true); }
    } exitGuard{ ctx.get() };

    SOCKET sock = CreateLanUdpSocket(ctx->bindPort, true);
    if (sock == INVALID_SOCKET) {
        LanLogf("republicmp  LAN: responder failed to bind UDP :%u (err=%d)", ctx->bindPort, WSAGetLastError());
        return 0;
    }
    {
        const bool loopbackOnly = g_loopbackOnlyForTest.load();
        LanLogf("republicmp  LAN: responder listening on UDP :%u (bind=%s:%u loopbackOnly=%s)",
                ctx->bindPort, loopbackOnly ? "127.0.0.1" : "0.0.0.0",
                ctx->bindPort, loopbackOnly ? "true" : "false");
    }

    uint64_t lastBeaconMs = 0;
    while (!ctx->stop.load()) {
        const bool wakeSignaled = ctx->wakeEvent && (WaitForSingleObject(ctx->wakeEvent, 0) == WAIT_OBJECT_0);
        const DWORD waitMs = (wakeSignaled || ctx->stop.load()) ? 0u : 40u;
        if (PollUdpReadable(sock, waitMs)) {
            HandleResponderReadable(sock, ctx.get());
        }
        if (ctx->stop.load()) break;

        uint64_t nowMs = GetTickCount64();
        if (wakeSignaled || (nowMs - lastBeaconMs) >= kLanBeaconIntervalMs) {
            lastBeaconMs = nowMs;
            bool shouldSend = false;
            LanAnnouncePacket pkt = {};
            std::vector<sockaddr_in> peersCopy;
            uint16_t destPort = LAN_DISCOVERY_PORT;
            {
                TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
                shouldSend = g_hostingOpen;
                if (shouldSend) {
                    pkt = BuildLocalAnnouncePacketLocked(true);
                    peersCopy = g_recentProbePeers;
                    destPort = EffectiveDiscoveryPortLocked();
                }
            }
            if (shouldSend) {
                std::vector<LanAdapterInfo> adapters = g_loopbackOnlyForTest.load()
                    ? std::vector<LanAdapterInfo>{}
                    : LanDiscovery_GetLocalAdapters(false);
                uint8_t wire[sizeof(LanAnnouncePacket)] = {};
                size_t wireLen = LanDiscovery_SerializeAnnounce(pkt, wire, sizeof(wire));
                if (wireLen > 0) {
                    SendUdpToDiscoveryTargets(sock, wire, (int)wireLen, destPort, adapters, &peersCopy);
                }
            }
        }
    }

    closesocket(sock);
    return 0;
}

static void HandleScannerReadable(SOCKET sock, const std::vector<LanAdapterInfo>& adapters,
                                  const LanThreadContext* ctx)
{
    for (int drained = 0; drained < kMaxDatagramsPerWake && (!ctx || !ctx->stop.load()); ++drained) {
        uint8_t buf[512] = {};
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int rx = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&fromAddr, &fromLen);
        if (rx == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEMSGSIZE) continue;
            break;
        }
        if (rx == 0) continue;

        LanAnnouncePacket pkt = {};
        if (!LanDiscovery_ParseAnnounce(buf, (size_t)rx, &pkt)) continue;

        char peerIp[16] = {};
        if (!inet_ntop(AF_INET, &fromAddr.sin_addr, peerIp, sizeof(peerIp))) continue;

        LanAdapterKind pathKind = ResolvePathKindFromAdapters(peerIp, adapters);
        bool sameMachine = IsSameMachineSourceIp(peerIp, adapters);
        uint64_t nowMs = GetTickCount64();

        bool logDiscovered = false;
        bool logClosed = false;
        {
            TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
            bool isSelf = g_selfPidOverrideOn
                ? (pkt.hostPid == g_selfPidOverride)
                : ((g_localServerGuid != 0 && pkt.serverGuid == g_localServerGuid) ||
                   (pkt.hostPid == GetCurrentProcessId()));
            if (isSelf) continue;

            bool wasPresent = false;
            for (const auto& existing : g_hosts) {
                if (existing.serverGuid == pkt.serverGuid) {
                    wasPresent = true;
                    break;
                }
            }
            if (LanDiscovery_UpsertHost(g_hosts, pkt, peerIp, nowMs, pathKind)) {
                if (pkt.open != 0 && sameMachine) {
                    for (auto& h : g_hosts) {
                        if (h.serverGuid == pkt.serverGuid) {
                            h.sameMachine = true;
                            break;
                        }
                    }
                }
                logDiscovered = (!wasPresent && pkt.open != 0);
                logClosed = (wasPresent && pkt.open == 0);
            }
        }
        if (logDiscovered) {
            LanLogf("republicmp  LAN: discovered host [%s] at %s:%u (proto=%u match=%d sameMachine=%d)",
                    pkt.hostName, peerIp, static_cast<unsigned>(pkt.tcpPort),
                    static_cast<unsigned>(pkt.protocol),
                    (pkt.protocol == (uint16_t)REPUBLICMP_PROTOCOL) ? 1 : 0,
                    sameMachine ? 1 : 0);
        } else if (logClosed) {
            LanLogf("republicmp  LAN: host closed [%s] at %s:%u",
                    pkt.hostName, peerIp, static_cast<unsigned>(pkt.tcpPort));
        }
    }
}

static DWORD WINAPI ScannerThreadProc(LPVOID lpParam)
{
    std::unique_ptr<std::shared_ptr<LanThreadContext>> paramPtr(
        static_cast<std::shared_ptr<LanThreadContext>*>(lpParam));
    if (!paramPtr || !*paramPtr) return 0;
    std::shared_ptr<LanThreadContext> ctx = *paramPtr;
    paramPtr.reset();

    struct ExitFlagGuard {
        LanThreadContext* c;
        ~ExitFlagGuard() { if (c) c->exited.store(true); }
    } exitGuard{ ctx.get() };

    SOCKET sock = CreateLanUdpSocket(0, false);
    if (sock == INVALID_SOCKET) {
        LanLogf("republicmp  LAN: scanner failed to create UDP socket (err=%d)", WSAGetLastError());
        return 0;
    }
    {
        const bool loopbackOnly = g_loopbackOnlyForTest.load();
        LanLogf("republicmp  LAN: scanner started (bind=%s:0 loopbackOnly=%s)",
                loopbackOnly ? "127.0.0.1" : "0.0.0.0", loopbackOnly ? "true" : "false");
    }

    uint64_t lastProbeMs = 0;
    uint32_t nonce = (uint32_t)(GetTickCount64() ^ GetCurrentProcessId());
    while (!ctx->stop.load()) {
        const bool wakeSignaled = ctx->wakeEvent && (WaitForSingleObject(ctx->wakeEvent, 0) == WAIT_OBJECT_0);
        const bool forceProbe = ctx->probeNow.exchange(false) || wakeSignaled;
        const DWORD waitMs = (forceProbe || ctx->stop.load()) ? 0u : 40u;
        const std::vector<LanAdapterInfo> adapters = g_loopbackOnlyForTest.load()
            ? std::vector<LanAdapterInfo>{}
            : LanDiscovery_GetLocalAdapters(false);
        if (PollUdpReadable(sock, waitMs)) {
            HandleScannerReadable(sock, adapters, ctx.get());
        }
        if (ctx->stop.load()) break;

        uint64_t nowMs = GetTickCount64();
        if (forceProbe || (nowMs - lastProbeMs) >= kLanBeaconIntervalMs) {
            lastProbeMs = nowMs;
            uint16_t destPort = LAN_DISCOVERY_PORT;
            {
                TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
                destPort = EffectiveDiscoveryPortLocked();
                LanDiscovery_PruneExpired(g_hosts, nowMs, LAN_DISCOVERY_TTL_MS);
            }

            LanProbePacket probe = {};
            probe.protocol = (uint16_t)REPUBLICMP_PROTOCOL;
            probe.clientNonce = ++nonce;
            probe.clientPid = GetCurrentProcessId();
            uint8_t wire[sizeof(LanProbePacket)] = {};
            size_t wireLen = LanDiscovery_SerializeProbe(probe, wire, sizeof(wire));
            if (wireLen > 0) {
                SendUdpToDiscoveryTargets(sock, wire, (int)wireLen, destPort, adapters, nullptr);
            }
        }
    }

    closesocket(sock);
    return 0;
}

static bool StopHostingUnderLifecycleLock()
{
    bool wasOpen = false;
    LanAnnouncePacket byePkt = {};
    std::vector<sockaddr_in> peersCopy;
    uint16_t destPort = LAN_DISCOVERY_PORT;
    {
        TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
        wasOpen = g_hostingOpen;
        if (wasOpen) {
            byePkt = BuildLocalAnnouncePacketLocked(false);
            peersCopy = g_recentProbePeers;
            destPort = EffectiveDiscoveryPortLocked();
        }
        g_hostingOpen = false;
        g_recentProbePeers.clear();
    }

    const bool hostStopped = StopManagedThread(g_hostThread, g_hostCtx);

    if (wasOpen) {
        const std::vector<LanAdapterInfo> adapters = g_loopbackOnlyForTest.load()
            ? std::vector<LanAdapterInfo>{}
            : LanDiscovery_GetLocalAdapters(false);
        SOCKET byeSock = CreateLanUdpSocket(0, false);
        if (byeSock != INVALID_SOCKET) {
            uint8_t wire[sizeof(LanAnnouncePacket)] = {};
            size_t wireLen = LanDiscovery_SerializeAnnounce(byePkt, wire, sizeof(wire));
            if (wireLen > 0) {
                for (int b = 0; b < 3; ++b) {
                    if (b > 0) Sleep(15);
                    SendUdpToDiscoveryTargets(byeSock, wire, (int)wireLen, destPort, adapters, &peersCopy);
                }
            }
            closesocket(byeSock);
        }
    }
    return hostStopped;
}

} // namespace

void LanDiscovery_Bind(const TsmHost* host)
{
    if (host) {
        TsmBind(host);
    }
}

void LanDiscovery_Init()
{
    TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
    TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
    EnsureLanInitializedLocked();
}

void LanDiscovery_BeginProcessShutdown()
{
    g_processShuttingDown.store(true);
}

void LanDiscovery_Shutdown()
{
    TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
    const bool hostStopped = StopHostingUnderLifecycleLock();
    const bool scanStopped = StopManagedThread(g_scanThread, g_scanCtx);

    TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
    g_hosts.clear();
    g_cachedAdapters.clear();
    g_cachedAdaptersValid = false;
    g_cachedAdaptersAtMs = 0;
    g_lastLoggedAdaptersSummary.clear();
    g_recentProbePeers.clear();
    g_hostingName[0] = '\0';
    g_hostingMap[0] = '\0';
    g_localServerGuid = 0;

    if (g_wsaStarted && hostStopped && scanStopped) {
        WSACleanup();
        g_wsaStarted = false;
    }
    g_initialized = false;
}

void LanDiscovery_SetHosting(bool open, uint16_t tcpPort, uint8_t twoStatesMode,
                             const char* hostName, const char* mapName)
{
    if (open) {
        if (g_processShuttingDown.load()) return;
        std::string fallbackName;
        const char* effectiveName = hostName;
        if (!effectiveName || !effectiveName[0]) {
            fallbackName = LanDiscovery_GetLocalDisplayName();
            effectiveName = fallbackName.c_str();
        }

        TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
        if (g_processShuttingDown.load()) return;
        uint16_t bindPort = LAN_DISCOVERY_PORT;
        {
            TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
            if (!EnsureLanInitializedLocked()) return;
            g_hostingOpen = true;
            g_hostingTcpPort = (tcpPort != 0) ? tcpPort : SAVE_SYNC_PORT;
            g_hostingMode = twoStatesMode;
            SanitizeWireString(effectiveName, effectiveName ? strlen(effectiveName) : 0,
                               g_hostingName, sizeof(g_hostingName));
            SanitizeWireString(mapName ? mapName : "", mapName ? strlen(mapName) : 0,
                               g_hostingMap, sizeof(g_hostingMap));
            bindPort = EffectiveDiscoveryPortLocked();
        }
        StartManagedThread(g_hostThread, g_hostCtx, ResponderThreadProc, bindPort, false);
        return;
    }

    TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
    StopHostingUnderLifecycleLock();
}

void LanDiscovery_UpdateHostingMode(uint8_t twoStatesMode)
{
    if (g_processShuttingDown.load()) return;
    TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
    bool shouldWake = false;
    {
        TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
        if (g_hostingOpen) {
            g_hostingMode = twoStatesMode;
            shouldWake = true;
        }
    }
    if (shouldWake && g_hostCtx && g_hostCtx->wakeEvent) {
        SetEvent(g_hostCtx->wakeEvent);
    }
}

void LanDiscovery_StartScan()
{
    if (g_processShuttingDown.load()) return;
    TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
    {
        TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
        if (!EnsureLanInitializedLocked()) return;
    }
    StartManagedThread(g_scanThread, g_scanCtx, ScannerThreadProc, 0, true);
}

void LanDiscovery_StopScan()
{
    TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
    StopManagedThread(g_scanThread, g_scanCtx);
}

void LanDiscovery_ProbeNow()
{
    TsmScopedCsLock lifecycleLock(&g_lifecycleOnce, &g_lifecycleCs);
    if (g_scanCtx) {
        g_scanCtx->probeNow.store(true);
        if (g_scanCtx->wakeEvent) {
            SetEvent(g_scanCtx->wakeEvent);
        }
    }
}

std::vector<LanHostEntry> LanDiscovery_GetHosts()
{
    TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
    uint64_t nowMs = GetTickCount64();
    LanDiscovery_PruneExpired(g_hosts, nowMs, LAN_DISCOVERY_TTL_MS);
    std::vector<LanHostEntry> result = g_hosts;
    std::sort(result.begin(), result.end(), [](const LanHostEntry& a, const LanHostEntry& b) {
        if (a.sameMachine != b.sameMachine) return a.sameMachine > b.sameMachine;
        if (a.protocolMatch != b.protocolMatch) return a.protocolMatch > b.protocolMatch;
        if (a.lastSeenMs != b.lastSeenMs) return a.lastSeenMs > b.lastSeenMs;
        return a.serverGuid < b.serverGuid;
    });
    return result;
}

std::vector<LanAdapterInfo> LanDiscovery_GetLocalAdapters(bool forceRefresh)
{
    uint64_t nowMs = GetTickCount64();
    {
        TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
        if (!forceRefresh && g_cachedAdaptersValid &&
            (nowMs - g_cachedAdaptersAtMs) < kLanAdapterCacheMs) {
            return g_cachedAdapters;
        }
    }

    std::vector<LanAdapterInfo> out;
    ULONG bufLen = 16384;
    std::vector<uint8_t> buffer(bufLen);
    ULONG ret = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                                   (PIP_ADAPTER_ADDRESSES)buffer.data(), &bufLen);
        if (ret != ERROR_BUFFER_OVERFLOW) break;
        buffer.resize(bufLen);
    }

    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES aa = (PIP_ADAPTER_ADDRESSES)buffer.data(); aa != nullptr; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp) continue;
            if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            std::string friendly = WideToUtf8(aa->FriendlyName);
            std::string desc = WideToUtf8(aa->Description);

            for (PIP_ADAPTER_UNICAST_ADDRESS ua = aa->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                if (ua->Address.iSockaddrLength < (int)sizeof(sockaddr_in)) continue;
                if (ua->DadState == IpDadStateInvalid || ua->DadState == IpDadStateDuplicate) continue;

                sockaddr_in sin = {};
                memcpy(&sin, ua->Address.lpSockaddr, sizeof(sin));
                uint32_t ipHost = ntohl(sin.sin_addr.s_addr);
                uint32_t firstOctet = (ipHost >> 24) & 0xFFu;
                uint32_t topTwo = (ipHost >> 16) & 0xFFFFu;
                if (firstOctet == 0u || firstOctet == 127u || topTwo == 0xA9FEu || firstOctet >= 224u) {
                    continue;
                }

                LanAdapterInfo info = {};
                if (!FormatIpv4HostOrder(ipHost, info.ip, sizeof(info.ip))) continue;
                if (!LanDiscovery_ComputeBroadcast(info.ip, ua->OnLinkPrefixLength,
                                                   info.mask, sizeof(info.mask),
                                                   info.broadcast, sizeof(info.broadcast))) {
                    continue;
                }

                SanitizeWireString(friendly.c_str(), friendly.size(), info.name, sizeof(info.name));
                info.kind = LanDiscovery_ClassifyAdapter((uint32_t)aa->IfType, friendly.c_str(),
                                                         desc.c_str(), info.ip, &info.priority);
                info.ifIndex = (uint32_t)aa->IfIndex;
                out.push_back(info);
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const LanAdapterInfo& a, const LanAdapterInfo& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.ifIndex != b.ifIndex) return a.ifIndex < b.ifIndex;
        return strcmp(a.ip, b.ip) < 0;
    });

    const std::string summary = FormatAdaptersSummary(out);
    bool logSummary = false;
    {
        TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
        g_cachedAdapters = out;
        g_cachedAdaptersValid = (ret == NO_ERROR || ret == ERROR_NO_DATA);
        g_cachedAdaptersAtMs = GetTickCount64();
        if (g_cachedAdaptersValid && summary != g_lastLoggedAdaptersSummary) {
            g_lastLoggedAdaptersSummary = summary;
            logSummary = true;
        }
    }
    if (logSummary) {
        LanLogf("republicmp  LAN: %s", summary.c_str());
    }
    return out;
}

std::string LanDiscovery_GetLocalDisplayName()
{
    const std::string steamName = SteamP2P_GetLocalPersonaNameCopy();
    if (!steamName.empty()) {
        char clean[48] = {};
        SanitizeWireString(steamName.c_str(), steamName.size(), clean, sizeof(clean));
        if (clean[0]) return std::string(clean);
    }

    wchar_t compW[64] = {};
    DWORD compLen = (DWORD)(sizeof(compW) / sizeof(compW[0]));
    if (GetComputerNameW(compW, &compLen) && compLen > 0) {
        std::string utf8 = WideToUtf8(compW);
        char clean[48] = {};
        SanitizeWireString(utf8.c_str(), utf8.size(), clean, sizeof(clean));
        if (clean[0]) return std::string(clean);
    }

    return std::string("Host");
}

LanAdapterKind LanDiscovery_ClassifyAdapter(uint32_t ifType, const char* nameUtf8,
                                            const char* descUtf8, const char* ipStr,
                                            int* outPriority)
{
    static const char* const kVirtualLocalKeywords[] = {
        "vethernet", "wsl", "hyper-v", "docker", "vmware",
        "vmnet", "virtualbox", "vbox", "loopback", "npcap", "br-", "veth"
    };
    static const char* const kVirtualVpnKeywords[] = {
        "radmin", "hamachi", "zerotier", "tailscale", "wireguard",
        "wintun", "openvpn", "tap-windows", "tunnel", "utun",
        "vpn", "cscotun", "fortinet", "nordlynx",
        "protonvpn", "softether"
    };
    static const char* const kWifiKeywords[] = {
        "wi-fi", "wifi", "wireless", "wlan", "802.11"
    };
    static const char* const kEthKeywords[] = {
        "ethernet", "gigabit", "realtek", "intel(r)"
    };

    std::string combined = AsciiLowerString(nameUtf8);
    combined.push_back(' ');
    combined.append(AsciiLowerString(descUtf8));

    uint32_t ipHost = 0;
    bool hasIp = ParseIpv4HostOrder(ipStr, &ipHost);

    LanAdapterKind kind = LanAdapterKind::Unknown;
    if (ifType == IF_TYPE_SOFTWARE_LOOPBACK ||
        ContainsAnyKeyword(combined, kVirtualLocalKeywords, sizeof(kVirtualLocalKeywords) / sizeof(kVirtualLocalKeywords[0]))) {
        kind = LanAdapterKind::VirtualLocal;
    } else if (ContainsAnyKeyword(combined, kVirtualVpnKeywords, sizeof(kVirtualVpnKeywords) / sizeof(kVirtualVpnKeywords[0])) ||
               ContainsAdapterToken(combined, "tun") ||
               ContainsAdapterToken(combined, "tap") ||
               (hasIp && IsVpnIpRange(ipHost)) ||
               ifType == IF_TYPE_TUNNEL || ifType == IF_TYPE_PROP_VIRTUAL) {
        kind = LanAdapterKind::VirtualVpn;
    } else if (ifType == IF_TYPE_IEEE80211 ||
               ContainsAnyKeyword(combined, kWifiKeywords, sizeof(kWifiKeywords) / sizeof(kWifiKeywords[0]))) {
        kind = LanAdapterKind::WiFi;
    } else if (ifType == IF_TYPE_ETHERNET_CSMACD ||
               ContainsAnyKeyword(combined, kEthKeywords, sizeof(kEthKeywords) / sizeof(kEthKeywords[0]))) {
        kind = LanAdapterKind::Ethernet;
    }

    if (outPriority) {
        *outPriority = PriorityForAdapterKind(kind);
    }
    return kind;
}

bool LanDiscovery_ComputeBroadcast(const char* ipStr, uint8_t prefixLen,
                                   char* outMask, size_t maskCap,
                                   char* outBroadcast, size_t bcastCap)
{
    if (!outMask || maskCap < 16 || !outBroadcast || bcastCap < 16) return false;
    outMask[0] = '\0';
    outBroadcast[0] = '\0';

    uint32_t ipHost = 0;
    if (!ParseIpv4HostOrder(ipStr, &ipHost)) return false;

    uint8_t effectivePrefix = (prefixLen == 0 || prefixLen > 32) ? 24u : prefixLen;
    uint32_t maskHost = (effectivePrefix == 32u) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32u - effectivePrefix));
    uint32_t bcastHost = (ipHost & maskHost) | (~maskHost);

    if (!FormatIpv4HostOrder(maskHost, outMask, maskCap) ||
        !FormatIpv4HostOrder(bcastHost, outBroadcast, bcastCap)) {
        outMask[0] = '\0';
        outBroadcast[0] = '\0';
        return false;
    }
    return true;
}

size_t LanDiscovery_SerializeProbe(const LanProbePacket& pkt, void* outBuf, size_t outCap)
{
    if (!outBuf || outCap < sizeof(LanProbePacket)) return 0;
    LanProbePacket wire = pkt;
    memcpy(wire.magic, kProbeMagic, sizeof(kProbeMagic));
    memcpy(outBuf, &wire, sizeof(wire));
    return sizeof(wire);
}

bool LanDiscovery_ParseProbe(const void* data, size_t len, LanProbePacket* outPkt)
{
    if (!data || !outPkt || len != sizeof(LanProbePacket)) return false;
    LanProbePacket tmp = {};
    memcpy(&tmp, data, sizeof(tmp));
    if (memcmp(tmp.magic, kProbeMagic, sizeof(kProbeMagic)) != 0) return false;
    if (tmp.protocol == 0) return false;
    *outPkt = tmp;
    return true;
}

size_t LanDiscovery_SerializeAnnounce(const LanAnnouncePacket& pkt, void* outBuf, size_t outCap)
{
    if (!outBuf || outCap < sizeof(LanAnnouncePacket)) return 0;
    LanAnnouncePacket wire = pkt;
    memcpy(wire.magic, kAnnounceMagic, sizeof(kAnnounceMagic));
    SanitizeWireString(pkt.version, sizeof(pkt.version), wire.version, sizeof(wire.version));
    SanitizeWireString(pkt.hostName, sizeof(pkt.hostName), wire.hostName, sizeof(wire.hostName));
    SanitizeWireString(pkt.mapName, sizeof(pkt.mapName), wire.mapName, sizeof(wire.mapName));
    memcpy(outBuf, &wire, sizeof(wire));
    return sizeof(wire);
}

bool LanDiscovery_ParseAnnounce(const void* data, size_t len, LanAnnouncePacket* outPkt)
{
    if (!data || !outPkt || len != sizeof(LanAnnouncePacket)) return false;
    LanAnnouncePacket raw = {};
    memcpy(&raw, data, sizeof(raw));
    if (memcmp(raw.magic, kAnnounceMagic, sizeof(kAnnounceMagic)) != 0) return false;
    if (raw.protocol == 0) return false;
    if (raw.open != 0 && raw.tcpPort == 0) return false;

    *outPkt = raw;
    SanitizeWireString(raw.version, sizeof(raw.version), outPkt->version, sizeof(outPkt->version));
    SanitizeWireString(raw.hostName, sizeof(raw.hostName), outPkt->hostName, sizeof(outPkt->hostName));
    SanitizeWireString(raw.mapName, sizeof(raw.mapName), outPkt->mapName, sizeof(outPkt->mapName));
    return true;
}

static bool IsElapsedGreaterThan(uint64_t nowMs, uint64_t thenMs, uint64_t thresholdMs)
{
    if (nowMs >= thenMs) {
        return (nowMs - thenMs) > thresholdMs;
    }
    uint64_t ahead = thenMs - nowMs;
    if (ahead > (UINT64_MAX / 2ULL)) {
        uint64_t wrappedAge = (uint64_t)(nowMs - thenMs);
        return wrappedAge > thresholdMs;
    }
    return false;
}

static bool IsBetterEvictionVictim(const LanHostEntry& cand, const LanHostEntry& current, uint64_t nowMs)
{
    if (cand.sameMachine != current.sameMachine) return !cand.sameMachine;
    if (cand.protocolMatch != current.protocolMatch) return !cand.protocolMatch;
    const uint64_t candAge = nowMs - cand.lastSeenMs;
    const uint64_t curAge = nowMs - current.lastSeenMs;
    if (candAge != curAge) return candAge > curAge;
    const int candRank = RankForHostPath(cand.connectIp, cand.pathKind);
    const int curRank = RankForHostPath(current.connectIp, current.pathKind);
    if (candRank != curRank) return candRank < curRank;
    return cand.serverGuid < current.serverGuid;
}

static size_t FindHostEvictionVictimIndex(const std::vector<LanHostEntry>& hosts, uint64_t nowMs)
{
    size_t victimIdx = 0;
    for (size_t i = 1; i < hosts.size(); ++i) {
        if (IsBetterEvictionVictim(hosts[i], hosts[victimIdx], nowMs)) {
            victimIdx = i;
        }
    }
    return victimIdx;
}

bool LanDiscovery_UpsertHost(std::vector<LanHostEntry>& hosts, const LanAnnouncePacket& pkt,
                             const char* sourceIp, uint64_t nowMs, LanAdapterKind pathKind)
{
    if (pkt.serverGuid == 0 || pkt.protocol == 0) return false;
    if (pkt.open != 0 && pkt.tcpPort == 0) return false;

    if (pkt.open == 0) {
        hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [&pkt](const LanHostEntry& e) {
            return e.serverGuid == pkt.serverGuid;
        }), hosts.end());
        return true;
    }

    uint32_t ipHost = 0;
    if (!ParseIpv4HostOrder(sourceIp, &ipHost)) return false;

    char canonIp[16] = {};
    if (!FormatIpv4HostOrder(ipHost, canonIp, sizeof(canonIp))) return false;

    bool incomingSameMachine = (strcmp(canonIp, "127.0.0.1") == 0);
    int incomingRank = RankForHostPath(canonIp, pathKind);

    for (auto& existing : hosts) {
        if (existing.serverGuid != pkt.serverGuid) continue;

        const uint64_t prevLastSeenMs = existing.lastSeenMs;
        existing.hostPid = pkt.hostPid;
        existing.protocol = pkt.protocol;
        existing.tcpPort = pkt.tcpPort;
        existing.dllCrc = pkt.dllCrc;
        existing.twoStatesMode = pkt.twoStatesMode;
        existing.steamOnline = pkt.steamOnline;
        existing.protocolMatch = (pkt.protocol == (uint16_t)REPUBLICMP_PROTOCOL);
        SanitizeWireString(pkt.hostName, sizeof(pkt.hostName), existing.hostName, sizeof(existing.hostName));
        SanitizeWireString(pkt.mapName, sizeof(pkt.mapName), existing.mapName, sizeof(existing.mapName));
        SanitizeWireString(pkt.version, sizeof(pkt.version), existing.version, sizeof(existing.version));
        existing.lastSeenMs = nowMs;

        int existingRank = RankForHostPath(existing.connectIp, existing.pathKind);
        uint64_t refPathMs = existing.hasPathLastSeen ? existing.pathLastSeenMs : prevLastSeenMs;
        bool primarySilent = IsElapsedGreaterThan(nowMs, refPathMs, kLanPathSilentSwitchMs);
        bool samePath = (strcmp(existing.connectIp, canonIp) == 0);

        if (samePath || incomingRank > existingRank || primarySilent) {
            strncpy_s(existing.connectIp, sizeof(existing.connectIp), canonIp, _TRUNCATE);
            existing.sameMachine = existing.sameMachine || incomingSameMachine;
            existing.pathKind = pathKind;
            existing.pathLastSeenMs = nowMs;
            existing.hasPathLastSeen = true;
        }
        return true;
    }

    LanHostEntry entry = {};
    entry.serverGuid = pkt.serverGuid;
    entry.hostPid = pkt.hostPid;
    entry.protocol = pkt.protocol;
    entry.tcpPort = pkt.tcpPort;
    entry.dllCrc = pkt.dllCrc;
    entry.twoStatesMode = pkt.twoStatesMode;
    entry.steamOnline = pkt.steamOnline;
    entry.sameMachine = incomingSameMachine;
    entry.protocolMatch = (pkt.protocol == (uint16_t)REPUBLICMP_PROTOCOL);
    strncpy_s(entry.connectIp, sizeof(entry.connectIp), canonIp, _TRUNCATE);
    SanitizeWireString(pkt.hostName, sizeof(pkt.hostName), entry.hostName, sizeof(entry.hostName));
    SanitizeWireString(pkt.mapName, sizeof(pkt.mapName), entry.mapName, sizeof(entry.mapName));
    SanitizeWireString(pkt.version, sizeof(pkt.version), entry.version, sizeof(entry.version));
    entry.lastSeenMs = nowMs;
    entry.pathKind = pathKind;
    entry.pathLastSeenMs = nowMs;
    entry.hasPathLastSeen = true;

    if (hosts.size() >= LAN_DISCOVERY_MAX_HOSTS) {
        size_t victimIdx = FindHostEvictionVictimIndex(hosts, nowMs);
        if (!incomingSameMachine && hosts[victimIdx].sameMachine) {
            return false;
        }
        hosts[victimIdx] = entry;
        return true;
    }

    hosts.push_back(entry);
    return true;
}

void LanDiscovery_PruneExpired(std::vector<LanHostEntry>& hosts, uint64_t nowMs, uint64_t ttlMs)
{
    hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [nowMs, ttlMs](const LanHostEntry& entry) {
        return IsElapsedGreaterThan(nowMs, entry.lastSeenMs, ttlMs);
    }), hosts.end());
}

std::vector<LanDiscoveryTarget> LanDiscovery_PlanSendTargets(
    const std::vector<LanAdapterInfo>& adapters,
    uint16_t destPort,
    const std::vector<std::string>& extraPeerIps,
    uint16_t extraPeerPort,
    bool loopbackOnly)
{
    std::vector<LanDiscoveryTarget> targets;
    if (destPort == 0) return targets;

    auto pushTarget = [&targets](const char* ipStr, uint16_t port, uint32_t ifIndex, bool isLoopback) {
        for (const auto& existing : targets) {
            if (existing.port == port && existing.ifIndex == ifIndex &&
                existing.isLoopback == isLoopback && strcmp(existing.ip, ipStr) == 0) {
                return;
            }
        }
        LanDiscoveryTarget t = {};
        strncpy_s(t.ip, sizeof(t.ip), ipStr, _TRUNCATE);
        t.port = port;
        t.ifIndex = ifIndex;
        t.isLoopback = isLoopback;
        targets.push_back(t);
    };

    pushTarget("127.0.0.1", destPort, 0u, true);

    if (!loopbackOnly) {
        for (const auto& adapter : adapters) {
            if (adapter.broadcast[0] != '\0' && strcmp(adapter.broadcast, "255.255.255.255") != 0) {
                pushTarget(adapter.broadcast, destPort, adapter.ifIndex, false);
            }
            if (adapter.ifIndex != 0) {
                pushTarget("255.255.255.255", destPort, adapter.ifIndex, false);
            }
        }
        pushTarget("255.255.255.255", destPort, 0u, false);
    }

    const uint16_t peerPort = (extraPeerPort != 0) ? extraPeerPort : destPort;
    for (const auto& peerIp : extraPeerIps) {
        uint32_t peerHost = 0;
        if (!ParseIpv4HostOrder(peerIp.c_str(), &peerHost)) continue;
        const bool isLoopback = IsLoopbackIpv4HostOrder(peerHost);
        if (loopbackOnly && !isLoopback) continue;
        char canonIp[16] = {};
        if (FormatIpv4HostOrder(peerHost, canonIp, sizeof(canonIp))) {
            pushTarget(canonIp, peerPort, 0u, isLoopback);
        }
    }
    return targets;
}

void LanDiscovery_SetPortForTest(uint16_t port)
{
    TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
    g_testPortOverride = port;
}

void LanDiscovery_SetSelfPidOverrideForTest(uint32_t pidOverride, bool enableOverride)
{
    TsmScopedCsLock lock(&g_lanOnce, &g_lanCs);
    g_selfPidOverrideOn = enableOverride;
    g_selfPidOverride = pidOverride;
}

void LanDiscovery_SetLoopbackOnlyForTest(bool enabled)
{
    g_loopbackOnlyForTest.store(enabled);
}
