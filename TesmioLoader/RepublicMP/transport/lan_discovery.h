// SPDX-License-Identifier: GPL-3.0-only
// LAN discovery (UDP broadcast + directed per-NIC + 127.0.0.1) and multi-NIC /
// virtual VPN adapter enumeration for RepublicMP.
// Pure standard C++/POD header: intentionally avoids <windows.h> and <winsock2.h>.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

struct TsmHost;

static constexpr uint16_t LAN_DISCOVERY_PORT      = 5555;
static constexpr uint64_t LAN_DISCOVERY_TTL_MS    = 4500;
static constexpr size_t   LAN_DISCOVERY_MAX_HOSTS = 64;

enum class LanAdapterKind : uint8_t {
    Unknown      = 0,
    Ethernet     = 1,
    WiFi         = 2,
    VirtualVpn   = 3,
    VirtualLocal = 4
};

// Higher priority value means preferred interface:
// Ethernet (400) > WiFi (300) > VirtualVpn (200) > Unknown (100) > VirtualLocal (50).
struct LanAdapterInfo {
    char           ip[16];
    char           mask[16];
    char           broadcast[16];
    char           name[64];
    LanAdapterKind kind;
    int            priority;
    uint32_t       ifIndex;
};

struct LanHostEntry {
    uint64_t       serverGuid;
    uint32_t       hostPid;
    uint16_t       protocol;
    uint16_t       tcpPort;
    uint32_t       dllCrc;
    uint8_t        twoStatesMode;
    uint8_t        steamOnline;
    bool           sameMachine;
    bool           protocolMatch;
    char           connectIp[16];
    char           hostName[48];
    char           mapName[48];
    char           version[16];
    uint64_t       lastSeenMs;
    LanAdapterKind pathKind = LanAdapterKind::Unknown;
    uint64_t       pathLastSeenMs = 0;
    bool           hasPathLastSeen = false;
};

// Wire structs use host little-endian byte order (uniform across Windows x64 peers).
#pragma pack(push, 1)
struct LanProbePacket {
    char     magic[8]; // "RPMPPRB1"
    uint16_t protocol;
    uint16_t reserved;
    uint32_t clientNonce;
    uint32_t clientPid;
};

struct LanAnnouncePacket {
    char     magic[8]; // "RPMPANN1"
    uint16_t protocol;
    uint16_t tcpPort;
    uint64_t serverGuid;
    uint32_t hostPid;
    uint8_t  open;
    uint8_t  twoStatesMode;
    uint8_t  hasPassword;
    uint8_t  steamOnline;
    uint32_t dllCrc;
    char     version[16];
    char     hostName[48];
    char     mapName[48];
};
#pragma pack(pop)

static_assert(sizeof(LanProbePacket) == 20, "LanProbePacket must be 20 bytes");
static_assert(sizeof(LanAnnouncePacket) == 144, "LanAnnouncePacket must be 144 bytes");

void LanDiscovery_Bind(const TsmHost* host);

// Lifecycle & runtime responder/scanner APIs (all thread-safe and idempotent).
void LanDiscovery_Init();
void LanDiscovery_BeginProcessShutdown();
void LanDiscovery_Shutdown();
void LanDiscovery_SetHosting(bool open, uint16_t tcpPort, uint8_t twoStatesMode,
                             const char* hostName, const char* mapName);
void LanDiscovery_UpdateHostingMode(uint8_t twoStatesMode);
void LanDiscovery_StartScan();
void LanDiscovery_StopScan();
void LanDiscovery_ProbeNow();
std::vector<LanHostEntry> LanDiscovery_GetHosts();
std::vector<LanAdapterInfo> LanDiscovery_GetLocalAdapters(bool forceRefresh = false);
std::string LanDiscovery_GetLocalDisplayName();

// Pure testable helpers (no OS network side effects).
LanAdapterKind LanDiscovery_ClassifyAdapter(uint32_t ifType, const char* nameUtf8,
                                            const char* descUtf8, const char* ipStr,
                                            int* outPriority = nullptr);
// Computes IPv4 subnet mask and directed broadcast from IPv4 string + prefix length.
// Prefix 0 or > 32 falls back safely to /24 (255.255.255.0) to avoid UB.
bool LanDiscovery_ComputeBroadcast(const char* ipStr, uint8_t prefixLen,
                                   char* outMask, size_t maskCap,
                                   char* outBroadcast, size_t bcastCap);
size_t LanDiscovery_SerializeProbe(const LanProbePacket& pkt, void* outBuf, size_t outCap);
bool LanDiscovery_ParseProbe(const void* data, size_t len, LanProbePacket* outPkt);
size_t LanDiscovery_SerializeAnnounce(const LanAnnouncePacket& pkt, void* outBuf, size_t outCap);
bool LanDiscovery_ParseAnnounce(const void* data, size_t len, LanAnnouncePacket* outPkt);
bool LanDiscovery_UpsertHost(std::vector<LanHostEntry>& hosts, const LanAnnouncePacket& pkt,
                             const char* sourceIp, uint64_t nowMs,
                             LanAdapterKind pathKind = LanAdapterKind::Unknown);
void LanDiscovery_PruneExpired(std::vector<LanHostEntry>& hosts, uint64_t nowMs,
                               uint64_t ttlMs = LAN_DISCOVERY_TTL_MS);

struct LanDiscoveryTarget {
    char     ip[16];
    uint16_t port;
    uint32_t ifIndex;
    bool     isLoopback;
};

// Pure target fan-out planner shared by SendUdpToDiscoveryTargets and unit tests.
std::vector<LanDiscoveryTarget> LanDiscovery_PlanSendTargets(
    const std::vector<LanAdapterInfo>& adapters,
    uint16_t destPort,
    const std::vector<std::string>& extraPeerIps = {},
    uint16_t extraPeerPort = 0,
    bool loopbackOnly = false);

// Isolated test hooks (default: port=5555, pidOverride=disabled, loopbackOnly=false).
void LanDiscovery_SetPortForTest(uint16_t port);
void LanDiscovery_SetSelfPidOverrideForTest(uint32_t pidOverride, bool enableOverride);
void LanDiscovery_SetLoopbackOnlyForTest(bool enabled);
