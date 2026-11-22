// SPDX-License-Identifier: GPL-3.0-only
// Milestone M1 Unit, Socket Loopback & Integration Tests (Features F1-F5):
//   - F1: Multi-NIC & Virtual VPN Adapter Classification, Broadcast Math, Live Enumeration
//   - F2: Packed Wire Protocol Codec, Unterminated Strings, Dedup Store & Wrap-Safe TTL Pruning
//   - F3/F4: Real Winsock2 UDP Loopback Responder/Scanner, Self-PID Filter, Bye Burst & Fuzz Stress
//   - F5: NetworkNormalizeTargetAddress & NetworkConnect Integration

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../transport/lan_discovery.h"
#include "../transport/network.h"
#include "../core/version.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct LanM1WsaScope {
    bool ok = false;
    LanM1WsaScope()
    {
        WSADATA wsa = {};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~LanM1WsaScope()
    {
        if (ok) WSACleanup();
    }
    LanM1WsaScope(const LanM1WsaScope&) = delete;
    LanM1WsaScope& operator=(const LanM1WsaScope&) = delete;
};

struct UdpSockScope {
    SOCKET sock = INVALID_SOCKET;
    explicit UdpSockScope(SOCKET s = INVALID_SOCKET) : sock(s) {}
    ~UdpSockScope()
    {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }
    void reset(SOCKET s = INVALID_SOCKET)
    {
        if (sock != INVALID_SOCKET) closesocket(sock);
        sock = s;
    }
    UdpSockScope(const UdpSockScope&) = delete;
    UdpSockScope& operator=(const UdpSockScope&) = delete;
};

static SOCKET BindLoopbackUdpEphemeral(uint16_t* outPort)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    sockaddr_in bound = {};
    int boundLen = sizeof(bound);
    if (getsockname(s, (sockaddr*)&bound, &boundLen) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (outPort) *outPort = ntohs(bound.sin_port);
    return s;
}

static bool TryBindLoopbackPortM1(int sockType, int proto, uint16_t port, bool reuseAddr)
{
    SOCKET s = socket(AF_INET, sockType, proto);
    if (s == INVALID_SOCKET) return false;
    if (reuseAddr) {
        BOOL reuse = TRUE;
        (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    closesocket(s);
    return ok;
}

static uint16_t ReserveFreeTestPortM1(int sockType, int proto, uint16_t baseStart)
{
    static std::atomic<uint32_t> s_seq{0};
    const uint16_t minPort = 41000u;
    const uint16_t span = 7999u; // 41000..48998
    const uint32_t bias = ((GetCurrentProcessId() % 200u) * 29u) + (s_seq.fetch_add(1u) * 13u);
    const uint16_t startPort = static_cast<uint16_t>(minPort + (((uint32_t)(baseStart - minPort) + bias) % span));
    for (uint16_t offset = 0; offset < span; ++offset) {
        const uint16_t candidate = static_cast<uint16_t>(minPort + (((uint32_t)(startPort - minPort) + offset) % span));
        if (TryBindLoopbackPortM1(sockType, proto, candidate, false) &&
            TryBindLoopbackPortM1(sockType, proto, candidate, true)) {
            return candidate;
        }
    }
    return 0;
}

static uint16_t ReserveFreeTestUdpPort(uint16_t startPort = 41000)
{
    return ReserveFreeTestPortM1(SOCK_DGRAM, IPPROTO_UDP, startPort);
}

static uint16_t ReserveFreeTestTcpPort(uint16_t startPort = 43000)
{
    return ReserveFreeTestPortM1(SOCK_STREAM, IPPROTO_TCP, startPort);
}

static LanAnnouncePacket MakeSampleAnnounce(uint64_t guid, uint32_t pid, uint16_t proto,
                                              uint16_t tcpPort, uint8_t open,
                                              const char* hostName, const char* mapName)
{
    LanAnnouncePacket pkt = {};
    memcpy(pkt.magic, "RPMPANN1", 8);
    pkt.protocol = proto;
    pkt.tcpPort = tcpPort;
    pkt.serverGuid = guid;
    pkt.hostPid = pid;
    pkt.open = open;
    pkt.twoStatesMode = 1;
    pkt.hasPassword = 0;
    pkt.steamOnline = 0;
    pkt.dllCrc = 0x1234ABCDu;
    strncpy_s(pkt.version, sizeof(pkt.version), REPUBLICMP_VERSION, _TRUNCATE);
    strncpy_s(pkt.hostName, sizeof(pkt.hostName), hostName ? hostName : "", _TRUNCATE);
    strncpy_s(pkt.mapName, sizeof(pkt.mapName), mapName ? mapName : "", _TRUNCATE);
    return pkt;
}

} // namespace

static bool SubTest_LanM1_AdapterEnumAndBroadcast()
{
    int prio = 0;

    // Hypervisor/container adapters report IF_TYPE_ETHERNET_CSMACD (6) on Windows; must be VirtualLocal.
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "vEthernet (WSL)", "Hyper-V Virtual Ethernet Adapter", "172.22.16.1", &prio) == LanAdapterKind::VirtualLocal);
    TEST_CHECK(prio == 50);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "VMware Network Adapter VMnet8", "VMware Virtual Ethernet Adapter", "192.168.184.1", &prio) == LanAdapterKind::VirtualLocal);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "VirtualBox Host-Only Network", "VirtualBox Host-Only Ethernet Adapter", "192.168.56.1", &prio) == LanAdapterKind::VirtualLocal);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "DockerNAT", "Docker Desktop Network", "10.0.75.1", &prio) == LanAdapterKind::VirtualLocal);

    // Virtual VPN adapters: Radmin (26.x), Hamachi (25.x), ZeroTier, Tailscale (100.64.0.0/10), WireGuard, OpenVPN.
    int vpnPrio = 0;
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "Radmin VPN", "Famatech Radmin VPN Ethernet Adapter", "26.10.20.30", &vpnPrio) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(vpnPrio == 200);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "Adapter", "Generic Adapter", "26.55.12.9", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "Hamachi", "LogMeIn Hamachi Virtual Ethernet Adapter", "25.10.20.30", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "ZeroTier One [abc]", "ZeroTier Virtual Port", "10.147.17.5", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(53, "Tailscale", "Tailscale Tunnel", "100.64.5.10", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(53, "wg0", "WireGuard Tunnel", "10.8.0.2", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "OpenVPN", "TAP-Windows Adapter V9", "10.9.0.6", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(131, "Tunnel", "Generic Tunnel", "10.20.30.40", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "tun2", "Secondary TUN Adapter", "10.8.0.5", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "tap1", "Secondary TAP Adapter", "10.8.0.6", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "utun2", "Userspace TUN", "10.8.0.7", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "eth-tun2", "Bridge TUN", "10.8.0.8", nullptr) == LanAdapterKind::VirtualVpn);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "my_tap1", "TAP-Win Interface", "10.8.0.9", nullptr) == LanAdapterKind::VirtualVpn);

    // Physical Wi-Fi & Ethernet + Unknown (including false-positive substring checks like Neptune/Saturn/Fortune).
    int wifiPrio = 0;
    int ethPrio = 0;
    int unkPrio = 0;
    TEST_CHECK(LanDiscovery_ClassifyAdapter(71, "Wi-Fi", "Intel(R) Wi-Fi 6 AX200", "192.168.1.42", &wifiPrio) == LanAdapterKind::WiFi);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "Ethernet", "Realtek PCIe GbE Family Controller", "192.168.1.10", &ethPrio) == LanAdapterKind::Ethernet);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "Neptune Ethernet", "Neptune Gigabit Adapter", "192.168.1.44", nullptr) == LanAdapterKind::Ethernet);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(6, "Saturn Adapter", "Fortune Gigabit Adapter", "192.168.1.45", nullptr) == LanAdapterKind::Ethernet);
    TEST_CHECK(LanDiscovery_ClassifyAdapter(0, "Custom", "Unknown Interface", "192.168.1.99", &unkPrio) == LanAdapterKind::Unknown);
    TEST_CHECK(ethPrio > wifiPrio && wifiPrio > vpnPrio && vpnPrio > unkPrio && unkPrio > prio);

    // Broadcast math boundaries (no << 32 UB on prefix 0 or > 32).
    char mask[16] = {};
    char bcast[16] = {};
    TEST_CHECK(LanDiscovery_ComputeBroadcast("192.168.1.50", 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(strcmp(mask, "255.255.255.0") == 0 && strcmp(bcast, "192.168.1.255") == 0);

    TEST_CHECK(LanDiscovery_ComputeBroadcast("26.10.20.30", 8, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(strcmp(mask, "255.0.0.0") == 0 && strcmp(bcast, "26.255.255.255") == 0);

    TEST_CHECK(LanDiscovery_ComputeBroadcast("172.16.5.9", 20, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(strcmp(mask, "255.255.240.0") == 0 && strcmp(bcast, "172.16.15.255") == 0);

    TEST_CHECK(LanDiscovery_ComputeBroadcast("100.64.0.5", 32, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(strcmp(mask, "255.255.255.255") == 0 && strcmp(bcast, "100.64.0.5") == 0);

    TEST_CHECK(LanDiscovery_ComputeBroadcast("192.168.1.50", 0, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(strcmp(mask, "255.255.255.0") == 0 && strcmp(bcast, "192.168.1.255") == 0);

    TEST_CHECK(LanDiscovery_ComputeBroadcast("192.168.1.50", 255, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(strcmp(mask, "255.255.255.0") == 0 && strcmp(bcast, "192.168.1.255") == 0);

    TEST_CHECK(!LanDiscovery_ComputeBroadcast(nullptr, 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(!LanDiscovery_ComputeBroadcast("999.1.1.1", 24, mask, sizeof(mask), bcast, sizeof(bcast)));
    TEST_CHECK(!LanDiscovery_ComputeBroadcast("192.168.1.50", 24, mask, 8, bcast, sizeof(bcast)));

    // Live local adapter enumeration & display name fallback.
    std::vector<LanAdapterInfo> adapters = LanDiscovery_GetLocalAdapters(true);
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& a = adapters[i];
        TEST_CHECK(strncmp(a.ip, "127.", 4) != 0);
        TEST_CHECK(strncmp(a.ip, "169.254.", 8) != 0);
        TEST_CHECK(strcmp(a.ip, "0.0.0.0") != 0);
        if (i > 0) {
            TEST_CHECK(adapters[i - 1].priority >= a.priority);
        }
    }
    std::string dispName = LanDiscovery_GetLocalDisplayName();
    TEST_CHECK(!dispName.empty() && dispName.size() <= 47);

    // Pure multi-NIC/VPN broadcast target fan-out planning (prod vs loopback-only test mode).
    std::vector<LanAdapterInfo> mockAdapters(4);
    strncpy_s(mockAdapters[0].ip, sizeof(mockAdapters[0].ip), "192.168.1.10", _TRUNCATE);
    strncpy_s(mockAdapters[0].broadcast, sizeof(mockAdapters[0].broadcast), "192.168.1.255", _TRUNCATE);
    mockAdapters[0].kind = LanAdapterKind::Ethernet;
    mockAdapters[0].ifIndex = 11u;

    strncpy_s(mockAdapters[1].ip, sizeof(mockAdapters[1].ip), "26.10.20.30", _TRUNCATE);
    strncpy_s(mockAdapters[1].broadcast, sizeof(mockAdapters[1].broadcast), "26.255.255.255", _TRUNCATE);
    mockAdapters[1].kind = LanAdapterKind::VirtualVpn;
    mockAdapters[1].ifIndex = 22u;

    strncpy_s(mockAdapters[2].ip, sizeof(mockAdapters[2].ip), "25.10.20.30", _TRUNCATE);
    strncpy_s(mockAdapters[2].broadcast, sizeof(mockAdapters[2].broadcast), "25.255.255.255", _TRUNCATE);
    mockAdapters[2].kind = LanAdapterKind::VirtualVpn;
    mockAdapters[2].ifIndex = 33u;

    strncpy_s(mockAdapters[3].ip, sizeof(mockAdapters[3].ip), "10.147.17.5", _TRUNCATE);
    strncpy_s(mockAdapters[3].broadcast, sizeof(mockAdapters[3].broadcast), "10.147.255.255", _TRUNCATE);
    mockAdapters[3].kind = LanAdapterKind::VirtualVpn;
    mockAdapters[3].ifIndex = 44u;

    auto hasTarget = [](const std::vector<LanDiscoveryTarget>& list, const char* ip, uint16_t port, uint32_t ifIdx) {
        for (const auto& t : list) {
            if (strcmp(t.ip, ip) == 0 && t.port == port && t.ifIndex == ifIdx) return true;
        }
        return false;
    };

    const std::vector<std::string> extraPeers = { "127.0.0.1", "192.168.1.99" };
    const std::vector<LanDiscoveryTarget> prodTargets =
        LanDiscovery_PlanSendTargets(mockAdapters, 5555u, extraPeers, 55100u, false);
    TEST_CHECK(hasTarget(prodTargets, "127.0.0.1", 5555u, 0u));
    TEST_CHECK(hasTarget(prodTargets, "192.168.1.255", 5555u, 11u));
    TEST_CHECK(hasTarget(prodTargets, "26.255.255.255", 5555u, 22u));
    TEST_CHECK(hasTarget(prodTargets, "25.255.255.255", 5555u, 33u));
    TEST_CHECK(hasTarget(prodTargets, "10.147.255.255", 5555u, 44u));
    TEST_CHECK(hasTarget(prodTargets, "255.255.255.255", 5555u, 22u));
    TEST_CHECK(hasTarget(prodTargets, "255.255.255.255", 5555u, 0u));
    TEST_CHECK(hasTarget(prodTargets, "127.0.0.1", 55100u, 0u));
    TEST_CHECK(hasTarget(prodTargets, "192.168.1.99", 55100u, 0u));

    const std::vector<LanDiscoveryTarget> loopbackTargets =
        LanDiscovery_PlanSendTargets(mockAdapters, 5555u, extraPeers, 55100u, true);
    TEST_CHECK(loopbackTargets.size() == 2u);
    TEST_CHECK(hasTarget(loopbackTargets, "127.0.0.1", 5555u, 0u));
    TEST_CHECK(hasTarget(loopbackTargets, "127.0.0.1", 55100u, 0u));
    for (const auto& t : loopbackTargets) {
        TEST_CHECK(t.isLoopback && strncmp(t.ip, "127.", 4) == 0);
    }

    // Deduplication: duplicate adapters + "127.0.0.1" in extraPeers with extraPeerPort=0 must not duplicate.
    std::vector<LanAdapterInfo> dupAdapters = { mockAdapters[0], mockAdapters[0] };
    const std::vector<std::string> dupPeers = { "127.0.0.1", "127.0.0.1", "192.168.1.99", "192.168.1.99" };
    const std::vector<LanDiscoveryTarget> dedupTargets =
        LanDiscovery_PlanSendTargets(dupAdapters, 5555u, dupPeers, 0u, false);
    int loopCount = 0;
    int bcast11Count = 0;
    int peer99Count = 0;
    for (const auto& t : dedupTargets) {
        if (strcmp(t.ip, "127.0.0.1") == 0 && t.port == 5555u && t.ifIndex == 0u) ++loopCount;
        if (strcmp(t.ip, "192.168.1.255") == 0 && t.port == 5555u && t.ifIndex == 11u) ++bcast11Count;
        if (strcmp(t.ip, "192.168.1.99") == 0 && t.port == 5555u && t.ifIndex == 0u) ++peer99Count;
    }
    TEST_CHECK(loopCount == 1 && bcast11Count == 1 && peer99Count == 1);
    return true;
}

static bool SubTest_LanM1_ProtocolCodecAndPureStore()
{
    // Probe roundtrip + rejection.
    LanProbePacket probeIn = {};
    probeIn.protocol = REPUBLICMP_PROTOCOL;
    probeIn.clientNonce = 0xCAFEBABEu;
    probeIn.clientPid = 4242u;
    uint8_t probeWire[64] = {};
    size_t probeLen = LanDiscovery_SerializeProbe(probeIn, probeWire, sizeof(probeWire));
    TEST_CHECK(probeLen == sizeof(LanProbePacket));
    TEST_CHECK(memcmp(probeWire, "RPMPPRB1", 8) == 0);

    LanProbePacket probeOut = {};
    TEST_CHECK(LanDiscovery_ParseProbe(probeWire, probeLen, &probeOut));
    TEST_CHECK(probeOut.protocol == REPUBLICMP_PROTOCOL);
    TEST_CHECK(probeOut.clientNonce == 0xCAFEBABEu);
    TEST_CHECK(probeOut.clientPid == 4242u);

    TEST_CHECK(LanDiscovery_SerializeProbe(probeIn, probeWire, 8) == 0);
    TEST_CHECK(!LanDiscovery_ParseProbe(nullptr, probeLen, &probeOut));
    TEST_CHECK(!LanDiscovery_ParseProbe(probeWire, 0, &probeOut));
    TEST_CHECK(!LanDiscovery_ParseProbe(probeWire, probeLen - 1, &probeOut));
    TEST_CHECK(!LanDiscovery_ParseProbe(probeWire, probeLen + 1, &probeOut));
    probeWire[0] = 'X';
    TEST_CHECK(!LanDiscovery_ParseProbe(probeWire, probeLen, &probeOut));

    // Announce roundtrip + post-NUL tail zeroing + adversarial unterminated wire strings.
    LanAnnouncePacket annIn = MakeSampleAnnounce(0x1122334455667788ULL, 777u, REPUBLICMP_PROTOCOL,
                                                   5556u, 1u, "ComradeHost", "Siberia");
    memset(annIn.version + strlen(annIn.version) + 1, 'Z', sizeof(annIn.version) - strlen(annIn.version) - 1);
    memset(annIn.hostName + strlen(annIn.hostName) + 1, 'Z', sizeof(annIn.hostName) - strlen(annIn.hostName) - 1);
    memset(annIn.mapName + strlen(annIn.mapName) + 1, 'Z', sizeof(annIn.mapName) - strlen(annIn.mapName) - 1);
    uint8_t annWire[256] = {};
    size_t annLen = LanDiscovery_SerializeAnnounce(annIn, annWire, sizeof(annWire));
    TEST_CHECK(annLen == sizeof(LanAnnouncePacket));
    TEST_CHECK(memcmp(annWire, "RPMPANN1", 8) == 0);
    {
        LanAnnouncePacket serialized = {};
        memcpy(&serialized, annWire, sizeof(serialized));
        for (size_t i = strlen(serialized.version) + 1; i < sizeof(serialized.version); ++i) {
            TEST_CHECK(serialized.version[i] == '\0');
        }
        for (size_t i = strlen(serialized.hostName) + 1; i < sizeof(serialized.hostName); ++i) {
            TEST_CHECK(serialized.hostName[i] == '\0');
        }
        for (size_t i = strlen(serialized.mapName) + 1; i < sizeof(serialized.mapName); ++i) {
            TEST_CHECK(serialized.mapName[i] == '\0');
        }
    }

    LanAnnouncePacket annOut = {};
    TEST_CHECK(LanDiscovery_ParseAnnounce(annWire, annLen, &annOut));
    TEST_CHECK(annOut.serverGuid == 0x1122334455667788ULL);
    TEST_CHECK(annOut.tcpPort == 5556u);
    TEST_CHECK(strcmp(annOut.hostName, "ComradeHost") == 0);
    TEST_CHECK(strcmp(annOut.mapName, "Siberia") == 0);

    // Fill version/hostName/mapName completely with non-NUL bytes & control chars.
    LanAnnouncePacket adv = {};
    memcpy(adv.magic, "RPMPANN1", 8);
    adv.protocol = REPUBLICMP_PROTOCOL;
    adv.tcpPort = 5556u;
    adv.serverGuid = 0xAABBCCDDEEFF0011ULL;
    adv.hostPid = 888u;
    adv.open = 1u;
    memset(adv.version, 'V', sizeof(adv.version));
    memset(adv.hostName, 'H', sizeof(adv.hostName));
    memset(adv.mapName, 'M', sizeof(adv.mapName));
    adv.hostName[0] = '\x01';
    adv.hostName[1] = '\n';
    TEST_CHECK(LanDiscovery_ParseAnnounce(&adv, sizeof(adv), &annOut));
    TEST_CHECK(annOut.version[15] == '\0');
    TEST_CHECK(annOut.hostName[47] == '\0');
    TEST_CHECK(annOut.mapName[47] == '\0');
    TEST_CHECK(strlen(annOut.version) == 15);
    TEST_CHECK(annOut.hostName[0] == ' ' && annOut.hostName[1] == ' ');

    // Truncated / bad magic / open with port=0 rejected; bye with port=0 accepted.
    TEST_CHECK(!LanDiscovery_ParseAnnounce(nullptr, sizeof(adv), &annOut));
    TEST_CHECK(!LanDiscovery_ParseAnnounce(&adv, sizeof(adv) - 1, &annOut));
    TEST_CHECK(!LanDiscovery_ParseAnnounce(probeWire, sizeof(LanProbePacket), &annOut));
    LanAnnouncePacket badPort = adv;
    badPort.tcpPort = 0;
    TEST_CHECK(!LanDiscovery_ParseAnnounce(&badPort, sizeof(badPort), &annOut));
    badPort.open = 0;
    TEST_CHECK(LanDiscovery_ParseAnnounce(&badPort, sizeof(badPort), &annOut));

    // Upsert rejection of invalid serverGuid / protocol / open-with-port0 / sourceIp.
    std::vector<LanHostEntry> rejectList;
    LanAnnouncePacket badGuid = MakeSampleAnnounce(0ULL, 500u, REPUBLICMP_PROTOCOL, 5556u, 1u, "NoGuid", "M");
    TEST_CHECK(!LanDiscovery_UpsertHost(rejectList, badGuid, "192.168.1.10", 1000ULL));
    LanAnnouncePacket badProtoZero = MakeSampleAnnounce(0x9001ULL, 500u, 0u, 5556u, 1u, "NoProto", "M");
    TEST_CHECK(!LanDiscovery_UpsertHost(rejectList, badProtoZero, "192.168.1.10", 1000ULL));
    LanAnnouncePacket openZeroPort = MakeSampleAnnounce(0x9003ULL, 500u, REPUBLICMP_PROTOCOL, 0u, 1u, "ZeroPort", "M");
    TEST_CHECK(!LanDiscovery_UpsertHost(rejectList, openZeroPort, "192.168.1.10", 1000ULL));
    LanAnnouncePacket validPkt = MakeSampleAnnounce(0x9002ULL, 500u, REPUBLICMP_PROTOCOL, 5556u, 1u, "Valid", "M");
    TEST_CHECK(!LanDiscovery_UpsertHost(rejectList, validPkt, nullptr, 1000ULL));
    TEST_CHECK(!LanDiscovery_UpsertHost(rejectList, validPkt, "", 1000ULL));
    TEST_CHECK(!LanDiscovery_UpsertHost(rejectList, validPkt, "999.1.1.1", 1000ULL));
    TEST_CHECK(rejectList.empty());

    // Failover when first packet arrived at nowMs == 0 and backup beacons arrive at 1500, 3000, 4500ms (F2-nowMs0-starvation).
    std::vector<LanHostEntry> zeroTickList;
    TEST_CHECK(LanDiscovery_UpsertHost(zeroTickList, validPkt, "127.0.0.1", 0ULL, LanAdapterKind::Unknown));
    TEST_CHECK(zeroTickList.size() == 1 && strcmp(zeroTickList[0].connectIp, "127.0.0.1") == 0);
    TEST_CHECK(zeroTickList[0].hasPathLastSeen);
    TEST_CHECK(LanDiscovery_UpsertHost(zeroTickList, validPkt, "26.10.20.30", 1500ULL, LanAdapterKind::VirtualVpn));
    TEST_CHECK(strcmp(zeroTickList[0].connectIp, "127.0.0.1") == 0);
    TEST_CHECK(LanDiscovery_UpsertHost(zeroTickList, validPkt, "26.10.20.30", 3000ULL, LanAdapterKind::VirtualVpn));
    TEST_CHECK(strcmp(zeroTickList[0].connectIp, "127.0.0.1") == 0);
    TEST_CHECK(LanDiscovery_UpsertHost(zeroTickList, validPkt, "26.10.20.30", 4500ULL, LanAdapterKind::VirtualVpn));
    TEST_CHECK(strcmp(zeroTickList[0].connectIp, "26.10.20.30") == 0);

    // Failover across 64-bit tick wraparound (UINT64_MAX - 100 -> 4000).
    std::vector<LanHostEntry> wrapFailoverList;
    TEST_CHECK(LanDiscovery_UpsertHost(wrapFailoverList, validPkt, "127.0.0.1", UINT64_MAX - 100ULL, LanAdapterKind::Unknown));
    TEST_CHECK(LanDiscovery_UpsertHost(wrapFailoverList, validPkt, "26.10.20.30", 4000ULL, LanAdapterKind::VirtualVpn));
    TEST_CHECK(strcmp(wrapFailoverList[0].connectIp, "26.10.20.30") == 0);

    // Upsert dedup, path upgrade, non-downgrade, silent fallback, bye removal.
    std::vector<LanHostEntry> hosts;
    LanAnnouncePacket h1 = MakeSampleAnnounce(0x1001ULL, 500u, REPUBLICMP_PROTOCOL, 5556u, 1u, "HostA", "Map1");
    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h1, "172.22.0.1", 1000ULL, LanAdapterKind::VirtualLocal));
    TEST_CHECK(hosts.size() == 1 && strcmp(hosts[0].connectIp, "172.22.0.1") == 0 && !hosts[0].sameMachine);

    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h1, "26.10.20.30", 1200ULL, LanAdapterKind::VirtualVpn));
    TEST_CHECK(hosts.size() == 1 && strcmp(hosts[0].connectIp, "26.10.20.30") == 0);

    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h1, "192.168.1.25", 1400ULL, LanAdapterKind::WiFi));
    TEST_CHECK(hosts.size() == 1 && strcmp(hosts[0].connectIp, "192.168.1.25") == 0);

    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h1, "192.168.1.10", 1600ULL, LanAdapterKind::Ethernet));
    TEST_CHECK(hosts.size() == 1 && strcmp(hosts[0].connectIp, "192.168.1.10") == 0);

    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h1, "127.0.0.1", 1800ULL, LanAdapterKind::Unknown));
    TEST_CHECK(hosts.size() == 1 && strcmp(hosts[0].connectIp, "127.0.0.1") == 0 && hosts[0].sameMachine);

    // Lower-rank path within 3000ms refreshes lastSeenMs & metadata without downgrading connectIp.
    LanAnnouncePacket h1Updated = MakeSampleAnnounce(0x1001ULL, 500u, REPUBLICMP_PROTOCOL, 5556u, 1u, "HostA_v2", "Map2");
    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h1Updated, "172.22.0.1", 2500ULL, LanAdapterKind::VirtualLocal));
    TEST_CHECK(strcmp(hosts[0].connectIp, "127.0.0.1") == 0);
    TEST_CHECK(hosts[0].lastSeenMs == 2500ULL);
    TEST_CHECK(strcmp(hosts[0].hostName, "HostA_v2") == 0);
    TEST_CHECK(hosts[0].protocolMatch);

    // Higher-rank path silent > 3000ms allows live fallback path to take over.
    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h1Updated, "26.10.20.30", 5000ULL, LanAdapterKind::VirtualVpn));
    TEST_CHECK(strcmp(hosts[0].connectIp, "26.10.20.30") == 0);

    // Protocol mismatch host coexists cleanly.
    LanAnnouncePacket h2 = MakeSampleAnnounce(0x2002ULL, 501u, (uint16_t)(REPUBLICMP_PROTOCOL + 1), 5556u, 1u, "OldVer", "MapX");
    TEST_CHECK(LanDiscovery_UpsertHost(hosts, h2, "192.168.1.77", 5100ULL, LanAdapterKind::Ethernet));
    TEST_CHECK(hosts.size() == 2 && !hosts[1].protocolMatch);

    // Bye packet removes h1 but keeps h2.
    LanAnnouncePacket bye1 = MakeSampleAnnounce(0x1001ULL, 500u, REPUBLICMP_PROTOCOL, 0u, 0u, "", "");
    TEST_CHECK(LanDiscovery_UpsertHost(hosts, bye1, "26.10.20.30", 5200ULL));
    TEST_CHECK(hosts.size() == 1 && hosts[0].serverGuid == 0x2002ULL);

    // PruneExpired exact boundary, future clock skew, and 64-bit wraparound.
    std::vector<LanHostEntry> pruneList;
    LanAnnouncePacket pA = MakeSampleAnnounce(1ULL, 10u, REPUBLICMP_PROTOCOL, 5556u, 1u, "A", "M");
    LanAnnouncePacket pB = MakeSampleAnnounce(2ULL, 11u, REPUBLICMP_PROTOCOL, 5556u, 1u, "B", "M");
    LanAnnouncePacket pC = MakeSampleAnnounce(3ULL, 12u, REPUBLICMP_PROTOCOL, 5556u, 1u, "C", "M");
    TEST_CHECK(LanDiscovery_UpsertHost(pruneList, pA, "192.168.1.2", 1000ULL));
    TEST_CHECK(LanDiscovery_UpsertHost(pruneList, pB, "192.168.1.3", 1001ULL));
    TEST_CHECK(LanDiscovery_UpsertHost(pruneList, pC, "192.168.1.4", 10000ULL)); // future clock skew
    LanDiscovery_PruneExpired(pruneList, 5501ULL, LAN_DISCOVERY_TTL_MS); // 5501-1000=4501 (prune), 5501-1001=4500 (keep)
    TEST_CHECK(pruneList.size() == 2 && pruneList[0].serverGuid == 2ULL && pruneList[1].serverGuid == 3ULL);

    // Wraparound test near UINT64_MAX.
    std::vector<LanHostEntry> wrapList;
    TEST_CHECK(LanDiscovery_UpsertHost(wrapList, pA, "192.168.1.2", UINT64_MAX - 100ULL));
    LanDiscovery_PruneExpired(wrapList, 200ULL, LAN_DISCOVERY_TTL_MS); // elapsed = 301 <= 4500 -> keep
    TEST_CHECK(wrapList.size() == 1);
    LanDiscovery_PruneExpired(wrapList, 5000ULL, LAN_DISCOVERY_TTL_MS); // elapsed = 5101 > 4500 -> prune
    TEST_CHECK(wrapList.empty());

    // Stress 256 remote hosts: capped at LAN_DISCOVERY_MAX_HOSTS (64), keeping newest 64 (192..255).
    std::vector<LanHostEntry> stressHosts;
    for (uint64_t i = 0; i < 256; ++i) {
        LanAnnouncePacket sp = MakeSampleAnnounce(0x10000ULL + i, (uint32_t)(1000u + i),
                                                    REPUBLICMP_PROTOCOL, 5556u, 1u, "StressHost", "Map");
        TEST_CHECK(LanDiscovery_UpsertHost(stressHosts, sp, "192.168.1.20", 1000ULL + i));
    }
    TEST_CHECK(stressHosts.size() == LAN_DISCOVERY_MAX_HOSTS);
    LanDiscovery_PruneExpired(stressHosts, 5600ULL, 4500ULL); // newest 64 have lastSeen >= 1192 >= 1100
    TEST_CHECK(stressHosts.size() == LAN_DISCOVERY_MAX_HOSTS);

    // sameMachine host must be preserved against an external 80-host flood.
    std::vector<LanHostEntry> mixedHosts;
    LanAnnouncePacket localPkt = MakeSampleAnnounce(0x7777ULL, 50u, REPUBLICMP_PROTOCOL, 5556u, 1u, "LocalPC", "M");
    TEST_CHECK(LanDiscovery_UpsertHost(mixedHosts, localPkt, "127.0.0.1", 100ULL));
    for (uint64_t i = 0; i < 80; ++i) {
        LanAnnouncePacket rem = MakeSampleAnnounce(0x20000ULL + i, (uint32_t)(2000u + i),
                                                   REPUBLICMP_PROTOCOL, 5556u, 1u, "Remote", "M");
        TEST_CHECK(LanDiscovery_UpsertHost(mixedHosts, rem, "192.168.1.50", 200ULL + i));
    }
    TEST_CHECK(mixedHosts.size() == LAN_DISCOVERY_MAX_HOSTS);
    bool foundLocal = false;
    for (const auto& h : mixedHosts) {
        if (h.serverGuid == 0x7777ULL && h.sameMachine) foundLocal = true;
    }
    TEST_CHECK(foundLocal);

    // UTF-8 boundary preservation in SanitizeWireString (2-byte Cyrillic & 4-byte emoji cut across byte 47).
    {
        LanAnnouncePacket utfPkt = MakeSampleAnnounce(0x30001ULL, 99u, REPUBLICMP_PROTOCOL, 5556u, 1u, "", "");
        // 24 x "Ж" (0xD0 0x96) = 48 bytes -> cut at 47 must drop trailing orphaned 0xD0 -> 46 bytes (23 chars)
        for (size_t i = 0; i < 48; i += 2) {
            utfPkt.hostName[i] = (char)0xD0;
            utfPkt.hostName[i + 1] = (char)0x96;
        }
        // 12 x fire emoji (4 bytes each) = 48 bytes -> cut at 47 must drop trailing 3 bytes -> 44 bytes (11 emojis)
        for (size_t i = 0; i < 48; i += 4) {
            utfPkt.mapName[i] = (char)0xF0;
            utfPkt.mapName[i + 1] = (char)0x9F;
            utfPkt.mapName[i + 2] = (char)0x94;
            utfPkt.mapName[i + 3] = (char)0xA5;
        }
        uint8_t wire[256] = {};
        size_t wLen = LanDiscovery_SerializeAnnounce(utfPkt, wire, sizeof(wire));
        TEST_CHECK(wLen == sizeof(LanAnnouncePacket));
        LanAnnouncePacket parsedUtf = {};
        TEST_CHECK(LanDiscovery_ParseAnnounce(wire, wLen, &parsedUtf));
        TEST_CHECK(strlen(parsedUtf.hostName) == 46u);
        TEST_CHECK(strlen(parsedUtf.mapName) == 44u);
        TEST_CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, parsedUtf.hostName, -1, nullptr, 0) > 0);
        TEST_CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, parsedUtf.mapName, -1, nullptr, 0) > 0);
    }
    return true;
}

static bool SubTest_LanM1_RawUdpLoopbackAndStress()
{
    LanM1WsaScope wsa;
    TEST_CHECK(wsa.ok);

    uint16_t respPort = 0;
    uint16_t cliPort = 0;
    UdpSockScope respSock(BindLoopbackUdpEphemeral(&respPort));
    UdpSockScope cliSock(BindLoopbackUdpEphemeral(&cliPort));
    TEST_CHECK(respSock.sock != INVALID_SOCKET && cliSock.sock != INVALID_SOCKET);

    DWORD rcvTimeoutMs = 300;
    setsockopt(respSock.sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeoutMs, sizeof(rcvTimeoutMs));
    setsockopt(cliSock.sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeoutMs, sizeof(rcvTimeoutMs));

    sockaddr_in respAddr = {};
    respAddr.sin_family = AF_INET;
    respAddr.sin_port = htons(respPort);
    inet_pton(AF_INET, "127.0.0.1", &respAddr.sin_addr);

    // 1. Client sends probe, responder replies unicast to recvfrom address.
    LanProbePacket probe = {};
    probe.protocol = REPUBLICMP_PROTOCOL;
    probe.clientNonce = 99u;
    probe.clientPid = 111u;
    uint8_t probeWire[sizeof(LanProbePacket)] = {};
    size_t pLen = LanDiscovery_SerializeProbe(probe, probeWire, sizeof(probeWire));
    TEST_CHECK(sendto(cliSock.sock, (const char*)probeWire, (int)pLen, 0,
                      (const sockaddr*)&respAddr, sizeof(respAddr)) == (int)pLen);

    uint8_t rxBuf[512] = {};
    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);
    int rx = recvfrom(respSock.sock, (char*)rxBuf, sizeof(rxBuf), 0, (sockaddr*)&fromAddr, &fromLen);
    TEST_CHECK(rx == (int)sizeof(LanProbePacket));
    LanProbePacket parsedProbe = {};
    TEST_CHECK(LanDiscovery_ParseProbe(rxBuf, (size_t)rx, &parsedProbe));
    TEST_CHECK(parsedProbe.clientNonce == 99u);

    LanAnnouncePacket ann = MakeSampleAnnounce(0x9988776655443322ULL, GetCurrentProcessId() ^ 0x55u,
                                               REPUBLICMP_PROTOCOL, 5556u, 1u, "LoopHost", "LoopMap");
    uint8_t annWire[sizeof(LanAnnouncePacket)] = {};
    size_t aLen = LanDiscovery_SerializeAnnounce(ann, annWire, sizeof(annWire));
    TEST_CHECK(sendto(respSock.sock, (const char*)annWire, (int)aLen, 0,
                      (const sockaddr*)&fromAddr, fromLen) == (int)aLen);

    fromLen = sizeof(fromAddr);
    rx = recvfrom(cliSock.sock, (char*)rxBuf, sizeof(rxBuf), 0, (sockaddr*)&fromAddr, &fromLen);
    TEST_CHECK(rx == (int)sizeof(LanAnnouncePacket));

    char peerIp[16] = {};
    TEST_CHECK(inet_ntop(AF_INET, &fromAddr.sin_addr, peerIp, sizeof(peerIp)) != nullptr);
    TEST_CHECK(strcmp(peerIp, "127.0.0.1") == 0);

    LanAnnouncePacket parsedAnn = {};
    TEST_CHECK(LanDiscovery_ParseAnnounce(rxBuf, (size_t)rx, &parsedAnn));
    std::vector<LanHostEntry> hosts;
    TEST_CHECK(LanDiscovery_UpsertHost(hosts, parsedAnn, peerIp, 1000ULL));
    TEST_CHECK(hosts.size() == 1 && hosts[0].protocolMatch && hosts[0].tcpPort == 5556u);

    // 2. Malformed/fuzz datagrams + subsequent valid burst stress (150 valid + 50 garbage).
    sockaddr_in cliAddr = {};
    cliAddr.sin_family = AF_INET;
    cliAddr.sin_port = htons(cliPort);
    inet_pton(AF_INET, "127.0.0.1", &cliAddr.sin_addr);

    const int junkSizes[5] = { 1, 7, 19, 139, 512 };
    int validUpserts = 0;
    for (int i = 0; i < 200; ++i) {
        if ((i % 4) == 0) {
            uint8_t junk[512] = {};
            int sz = junkSizes[(i / 4) % 5];
            memset(junk, (uint8_t)(0x41 + (i % 26)), (size_t)sz);
            sendto(respSock.sock, (const char*)junk, sz, 0, (const sockaddr*)&cliAddr, sizeof(cliAddr));
        } else {
            LanAnnouncePacket burst = MakeSampleAnnounce(0xABC000ULL + (uint64_t)i, 2000u + (uint32_t)i,
                                                         REPUBLICMP_PROTOCOL, 5556u, 1u, "Burst", "Map");
            size_t bLen = LanDiscovery_SerializeAnnounce(burst, annWire, sizeof(annWire));
            sendto(respSock.sock, (const char*)annWire, (int)bLen, 0, (const sockaddr*)&cliAddr, sizeof(cliAddr));
        }

        fromLen = sizeof(fromAddr);
        rx = recvfrom(cliSock.sock, (char*)rxBuf, sizeof(rxBuf), 0, (sockaddr*)&fromAddr, &fromLen);
        TEST_CHECK(rx > 0);
        LanAnnouncePacket rxPkt = {};
        if (LanDiscovery_ParseAnnounce(rxBuf, (size_t)rx, &rxPkt)) {
            if (LanDiscovery_UpsertHost(hosts, rxPkt, "127.0.0.1", 2000ULL + (uint64_t)i)) {
                ++validUpserts;
            }
        }
    }
    TEST_CHECK(validUpserts == 150);
    return true;
}

static bool SubTest_LanM1_LiveModuleLifecycle()
{
    LanM1WsaScope wsa;
    TEST_CHECK(wsa.ok);

    uint16_t testPort = ReserveFreeTestUdpPort(41000);
    TEST_CHECK(testPort != 0 && testPort < 49152);

    LanDiscovery_SetPortForTest(testPort);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 1u, true);

    // Idempotent init -> host -> scan -> discover -> bye -> self-filter -> double shutdown.
    LanDiscovery_Init();
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, "LiveM1Host", "LiveM1Map");
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();

    bool foundHost = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::vector<LanHostEntry> list = LanDiscovery_GetHosts();
        for (const auto& h : list) {
            if (strcmp(h.hostName, "LiveM1Host") == 0 && strcmp(h.connectIp, "127.0.0.1") == 0) {
                TEST_CHECK(h.tcpPort == 5556u);
                TEST_CHECK(h.twoStatesMode == 1u);
                TEST_CHECK(h.protocolMatch);
                TEST_CHECK(h.sameMachine);
                foundHost = true;
                break;
            }
        }
        if (foundHost) break;
        if ((attempt % 5) == 4) {
            LanDiscovery_ProbeNow();
        }
        Sleep(50);
    }
    TEST_CHECK(foundHost);

    // LanDiscovery_UpdateHostingMode updates twoStatesMode without wiping mapName.
    LanDiscovery_UpdateHostingMode(0u);
    LanDiscovery_ProbeNow();
    bool modeUpdated = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        for (const auto& h : LanDiscovery_GetHosts()) {
            if (strcmp(h.hostName, "LiveM1Host") == 0 && h.twoStatesMode == 0u &&
                strcmp(h.mapName, "LiveM1Map") == 0) {
                modeUpdated = true;
                break;
            }
        }
        if (modeUpdated) break;
        if ((attempt % 5) == 4) LanDiscovery_ProbeNow();
        Sleep(30);
    }
    TEST_CHECK(modeUpdated);

    // Stopping host sends bye burst and removes host from scanner list.
    LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
    bool clearedAfterBye = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (LanDiscovery_GetHosts().empty()) {
            clearedAfterBye = true;
            break;
        }
        Sleep(30);
    }
    TEST_CHECK(clearedAfterBye);

    // Self-PID filtering when override equals current process PID.
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId(), true);
    LanDiscovery_SetHosting(true, 5556u, 0, "SelfFilteredHost", "Map");
    LanDiscovery_ProbeNow();
    Sleep(200);
    TEST_CHECK(LanDiscovery_GetHosts().empty());

    LanDiscovery_StopScan();
    LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
    LanDiscovery_Shutdown();
    LanDiscovery_Shutdown();
    LanDiscovery_SetPortForTest(0);
    LanDiscovery_SetSelfPidOverrideForTest(0, false);
    return true;
}

static bool SubTest_LanM1_AddressNormalizationAndConnect()
{
    char outHost[128] = {};
    uint16_t port = 5556u;

    // Plain IPv4 preserves caller's port untouched.
    TEST_CHECK(NetworkNormalizeTargetAddress("192.168.1.50", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "192.168.1.50") == 0 && port == 5556u);
    TEST_CHECK(NetworkNormalizeTargetAddress("  127.0.0.1\r\n", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "127.0.0.1") == 0 && port == 5556u);
    TEST_CHECK(NetworkNormalizeTargetAddress("\t26.10.20.30  ", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "26.10.20.30") == 0 && port == 5556u);

    // Optional :port bounds (1..65535), including leading zeros.
    TEST_CHECK(NetworkNormalizeTargetAddress("  26.10.20.30:7777  ", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "26.10.20.30") == 0 && port == 7777u);
    TEST_CHECK(NetworkNormalizeTargetAddress("192.168.1.50:005556", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "192.168.1.50") == 0 && port == 5556u);
    TEST_CHECK(NetworkNormalizeTargetAddress("100.64.0.1:1", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "100.64.0.1") == 0 && port == 1u);
    TEST_CHECK(NetworkNormalizeTargetAddress("192.168.0.2:65535", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "192.168.0.2") == 0 && port == 65535u);
    TEST_CHECK(NetworkNormalizeTargetAddress("192.168.1.50:5556", outHost, sizeof(outHost), nullptr));

    // Steam targets preserve *outPort untouched.
    port = 5556u;
    TEST_CHECK(NetworkNormalizeTargetAddress("steam:76561198000000000", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "steam:76561198000000000") == 0 && port == 5556u);
    TEST_CHECK(NetworkNormalizeTargetAddress("  STEAM:76561198012345678  ", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "steam:76561198012345678") == 0 && port == 5556u);
    TEST_CHECK(NetworkNormalizeTargetAddress("76561198012345678", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "76561198012345678") == 0 && port == 5556u);
    TEST_CHECK(NetworkNormalizeTargetAddress("steam:18446744073709551615", outHost, sizeof(outHost), &port));
    TEST_CHECK(strcmp(outHost, "steam:18446744073709551615") == 0 && port == 5556u);

    // Invalid inputs & truncation rejections (preserve *outPort on hostCap failure).
    const char* const kInvalidTargets[] = {
        nullptr, "", "   \t\r\n ", "256.1.1.1", "192.168.1", "192.168.1.1.1",
        "abc.def.ghi.jkl", "192.168.1.1 extra", "192.168.1.1:0", "192.168.1.1:0000",
        "192.168.1.1:65536", "192.168.1.1:99999999999", "192.168.1.1:-1", "192.168.1.1:abc",
        "192.168.1.1:", ":5556", "192.168.1.1:5556:7777", "[127.0.0.1]",
        "[::1]", "steam:", "steam:notanumber", "steam:12345",
        "steam:99999999999999999999999999", "99999999999999999999999999",
        "steam:18446744073709551616", "steam:000076561198000000000"
    };
    for (const char* bad : kInvalidTargets) {
        TEST_CHECK(!NetworkNormalizeTargetAddress(bad, outHost, sizeof(outHost), &port));
    }
    TEST_CHECK(!NetworkNormalizeTargetAddress("192.168.1.50", nullptr, sizeof(outHost), &port));
    TEST_CHECK(!NetworkNormalizeTargetAddress("192.168.1.50", outHost, 0, &port));
    port = 5556u;
    TEST_CHECK(!NetworkNormalizeTargetAddress("192.168.1.50:7777", outHost, 8, &port));
    TEST_CHECK(outHost[0] == '\0' && port == 5556u);

    // Real TCP NetworkConnect integration with normalized "  127.0.0.1:<port>  ".
    LanM1WsaScope wsa;
    TEST_CHECK(wsa.ok);
    TEST_CHECK(NetworkConnect(nullptr, 5556, 200, 200) == INVALID_SOCKET);
    TEST_CHECK(NetworkConnect("192.168.1.1:99999", 5556, 200, 200) == INVALID_SOCKET);

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_CHECK(listenSock != INVALID_SOCKET);
    sockaddr_in sin = {};
    sin.sin_family = AF_INET;
    sin.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    TEST_CHECK(bind(listenSock, (const sockaddr*)&sin, sizeof(sin)) == 0);
    TEST_CHECK(listen(listenSock, 1) == 0);
    int sinLen = sizeof(sin);
    TEST_CHECK(getsockname(listenSock, (sockaddr*)&sin, &sinLen) == 0);
    uint16_t tcpPort = ntohs(sin.sin_port);

    std::atomic<bool> acceptedOk(false);
    std::thread acceptThread([listenSock, &acceptedOk]() {
        sockaddr_in peer = {};
        int peerLen = sizeof(peer);
        SOCKET c = accept(listenSock, (sockaddr*)&peer, &peerLen);
        if (c != INVALID_SOCKET) {
            uint32_t magic = 0;
            if (NetworkRecvExact(c, &magic, sizeof(magic)) && magic == 0x504D5031u) {
                acceptedOk.store(true);
            }
            NetworkCloseSocket(c);
        }
    });

    char targetWithPort[64] = {};
    _snprintf_s(targetWithPort, sizeof(targetWithPort), _TRUNCATE, "  127.0.0.1:%u  ", (unsigned)tcpPort);
    SOCKET clientSock = NetworkConnect(targetWithPort, 9999, 2000, 2000);
    TEST_CHECK(clientSock != INVALID_SOCKET);
    uint32_t magic = 0x504D5031u;
    TEST_CHECK(NetworkSendExact(clientSock, &magic, sizeof(magic)));
    NetworkCloseSocket(clientSock);
    acceptThread.join();
    closesocket(listenSock);
    TEST_CHECK(acceptedOk.load());

    // Loopback-only NetworkHostAcceptOne + NetworkConnect integration (127.0.0.1 bind).
    uint16_t acceptPort = ReserveFreeTestTcpPort(43000);
    TEST_CHECK(acceptPort != 0 && acceptPort < 49152);
    volatile bool cancelAccept = false;
    std::atomic<bool> hostAcceptedMagic(false);
    std::thread hostAcceptThread([acceptPort, &cancelAccept, &hostAcceptedMagic]() {
        SOCKET accepted = NetworkHostAcceptOne(acceptPort, 2000, 2000, &cancelAccept);
        if (accepted != INVALID_SOCKET) {
            uint32_t m = 0;
            if (NetworkRecvExact(accepted, &m, sizeof(m)) && m == 0x4C4F4F50u) {
                hostAcceptedMagic.store(true);
            }
            NetworkCloseSocket(accepted);
        }
    });

    SOCKET loopClient = INVALID_SOCKET;
    for (int attempt = 0; attempt < 25; ++attempt) {
        loopClient = NetworkConnect("127.0.0.1", acceptPort, 300, 2000);
        if (loopClient != INVALID_SOCKET) break;
        Sleep(20);
    }
    if (loopClient == INVALID_SOCKET) {
        cancelAccept = true;
    }
    TEST_CHECK(loopClient != INVALID_SOCKET);
    uint32_t loopMagic = 0x4C4F4F50u;
    TEST_CHECK(NetworkSendExact(loopClient, &loopMagic, sizeof(loopMagic)));
    NetworkCloseSocket(loopClient);
    hostAcceptThread.join();
    TEST_CHECK(hostAcceptedMagic.load());

    // NetworkConnect(timeout=0) unbounded wait succeeds against live loopback listener (no instant timeout).
    {
        uint16_t zeroTimeoutPort = ReserveFreeTestTcpPort(43100);
        TEST_CHECK(zeroTimeoutPort != 0);
        volatile bool cancelZero = false;
        std::atomic<bool> acceptedZero(false);
        std::thread hostZero([zeroTimeoutPort, &cancelZero, &acceptedZero]() {
            SOCKET s = NetworkHostAcceptOne(zeroTimeoutPort, 2000, 1000, &cancelZero);
            if (s != INVALID_SOCKET) {
                acceptedZero.store(true);
                NetworkCloseSocket(s);
            }
        });
        Sleep(30);
        SOCKET cliZero = NetworkConnect("127.0.0.1", zeroTimeoutPort, 0, 1000, &cancelZero);
        if (cliZero == INVALID_SOCKET) cancelZero = true;
        TEST_CHECK(cliZero != INVALID_SOCKET);
        NetworkCloseSocket(cliZero);
        hostZero.join();
        TEST_CHECK(acceptedZero.load());
    }

    // NetworkHostAcceptOne timeout & *cancelFlag abort, plus offline steam: fast-fail.
    {
        uint16_t timeoutPort = ReserveFreeTestTcpPort(43200);
        TEST_CHECK(timeoutPort != 0);
        volatile bool cancelFalse = false;
        TEST_CHECK(NetworkHostAcceptOne(timeoutPort, 150, 500, &cancelFalse) == INVALID_SOCKET);

        volatile bool cancelSoon = false;
        std::thread canceler([&cancelSoon]() {
            Sleep(60);
            cancelSoon = true;
        });
        const ULONGLONG t0 = GetTickCount64();
        TEST_CHECK(NetworkHostAcceptOne(timeoutPort, 5000, 500, &cancelSoon) == INVALID_SOCKET);
        const ULONGLONG dt = GetTickCount64() - t0;
        canceler.join();
        TEST_CHECK(dt < 1500ULL);

        const ULONGLONG st0 = GetTickCount64();
        TEST_CHECK(NetworkConnect("steam:76561198000000000", 5556, 5000, 500) == INVALID_SOCKET);
        TEST_CHECK((GetTickCount64() - st0) < 1000ULL);
    }

    // NetworkSendFile / NetworkReceiveFile error bounds, partial cleanup, and roundtrip.
    {
        char tempDir[MAX_PATH] = {};
        TEST_CHECK(GetTempPathA(MAX_PATH, tempDir) > 0);
        const DWORD pid = GetCurrentProcessId();
        char srcPath[MAX_PATH] = {};
        char dstPath[MAX_PATH] = {};
        char partialPath[MAX_PATH] = {};
        _snprintf_s(srcPath, sizeof(srcPath), _TRUNCATE, "%srmp_m1_send_%lu.bin", tempDir, (unsigned long)pid);
        _snprintf_s(dstPath, sizeof(dstPath), _TRUNCATE, "%srmp_m1_recv_%lu.bin", tempDir, (unsigned long)pid);
        _snprintf_s(partialPath, sizeof(partialPath), _TRUNCATE, "%srmp_m1_part_%lu.bin", tempDir, (unsigned long)pid);
        DeleteFileA(srcPath);
        DeleteFileA(dstPath);
        DeleteFileA(partialPath);

        auto createTcpPair = [](SOCKET* outHost, SOCKET* outClient) -> bool {
            *outHost = INVALID_SOCKET;
            *outClient = INVALID_SOCKET;
            SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (ls == INVALID_SOCKET) return false;
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            if (bind(ls, (const sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
                closesocket(ls);
                return false;
            }
            int len = sizeof(a);
            if (getsockname(ls, (sockaddr*)&a, &len) != 0) {
                closesocket(ls);
                return false;
            }
            SOCKET c = NetworkConnect("127.0.0.1", ntohs(a.sin_port), 1000, 1000);
            SOCKET h = accept(ls, nullptr, nullptr);
            closesocket(ls);
            if (c == INVALID_SOCKET || h == INVALID_SOCKET) {
                if (c != INVALID_SOCKET) NetworkCloseSocket(c);
                if (h != INVALID_SOCKET) NetworkCloseSocket(h);
                return false;
            }
            *outHost = h;
            *outClient = c;
            return true;
        };

        // Missing source file must fail cleanly without sending UINT64_MAX.
        SOCKET sH = INVALID_SOCKET, sC = INVALID_SOCKET;
        TEST_CHECK(createTcpPair(&sH, &sC));
        TEST_CHECK(!NetworkSendFile(sH, nullptr));
        TEST_CHECK(!NetworkSendFile(sH, "Z:\\nonexistent_rmp_file_999.bin"));
        NetworkCloseSocket(sH);
        NetworkCloseSocket(sC);

        // Oversized header (> 2 GB) must be rejected without creating destination file.
        TEST_CHECK(createTcpPair(&sH, &sC));
        const unsigned long long tooBig = 3ULL * 1024ULL * 1024ULL * 1024ULL;
        TEST_CHECK(NetworkSendExact(sH, &tooBig, sizeof(tooBig)));
        TEST_CHECK(!NetworkReceiveFile(sC, partialPath));
        TEST_CHECK(GetFileAttributesA(partialPath) == INVALID_FILE_ATTRIBUTES);
        NetworkCloseSocket(sH);
        NetworkCloseSocket(sC);

        // Truncated stream mid-transfer must delete partial file.
        TEST_CHECK(createTcpPair(&sH, &sC));
        std::thread truncSender([sH]() {
            const unsigned long long declSz = 1024ULL;
            const char chunk[128] = {};
            NetworkSendExact(sH, &declSz, sizeof(declSz));
            NetworkSendExact(sH, chunk, sizeof(chunk));
            NetworkCloseSocket(sH);
        });
        TEST_CHECK(!NetworkReceiveFile(sC, partialPath));
        truncSender.join();
        NetworkCloseSocket(sC);
        TEST_CHECK(GetFileAttributesA(partialPath) == INVALID_FILE_ATTRIBUTES);

        // Valid loopback file roundtrip.
        FILE* fSrc = nullptr;
        TEST_CHECK(fopen_s(&fSrc, srcPath, "wb") == 0 && fSrc != nullptr);
        const char kPayload[] = "RepublicMP-M1-File-Roundtrip-0123456789";
        TEST_CHECK(fwrite(kPayload, 1, sizeof(kPayload) - 1, fSrc) == sizeof(kPayload) - 1);
        fclose(fSrc);

        TEST_CHECK(createTcpPair(&sH, &sC));
        bool sendOk = false;
        std::thread okSender([&]() {
            sendOk = NetworkSendFile(sH, srcPath);
            NetworkCloseSocket(sH);
        });
        TEST_CHECK(NetworkReceiveFile(sC, dstPath));
        okSender.join();
        NetworkCloseSocket(sC);
        TEST_CHECK(sendOk);

        FILE* fDst = nullptr;
        TEST_CHECK(fopen_s(&fDst, dstPath, "rb") == 0 && fDst != nullptr);
        char readBack[128] = {};
        const size_t gotBytes = fread(readBack, 1, sizeof(readBack), fDst);
        fclose(fDst);
        TEST_CHECK(gotBytes == sizeof(kPayload) - 1);
        TEST_CHECK(memcmp(readBack, kPayload, sizeof(kPayload) - 1) == 0);

        DeleteFileA(srcPath);
        DeleteFileA(dstPath);
        DeleteFileA(partialPath);
    }
    return true;
}

static bool SubTest_LanM1_ConcurrentLifecycleAndShutdownStress()
{
    LanM1WsaScope wsa;
    TEST_CHECK(wsa.ok);

    uint16_t testPort = ReserveFreeTestUdpPort(42000);
    TEST_CHECK(testPort != 0 && testPort < 49152);

    LanDiscovery_SetPortForTest(testPort);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 1u, true);

    // Warm up Winsock / internal OS handle baseline once before measuring handle stability.
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 0u, "WarmupHost", "WarmupMap");
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();
    Sleep(20);
    LanDiscovery_Shutdown();

    DWORD handlesAfterWarmup = 0;
    TEST_CHECK(GetProcessHandleCount(GetCurrentProcess(), &handlesAfterWarmup) != 0);

    // Phase A: Concurrent SetHosting / StartScan / StopScan / ProbeNow / GetHosts generation stress.
    std::atomic<uint64_t> maxStopDurationMs(0);
    auto recordDuration = [&maxStopDurationMs](uint64_t dt) {
        uint64_t prev = maxStopDurationMs.load();
        while (dt > prev && !maxStopDurationMs.compare_exchange_weak(prev, dt)) {}
    };

    std::vector<std::thread> phaseAThreads;
    for (int t = 0; t < 4; ++t) {
        phaseAThreads.emplace_back([t, &recordDuration]() {
            for (int i = 0; i < 30; ++i) {
                if ((t & 1) == 0) {
                    LanDiscovery_SetHosting(true, 5556u, (uint8_t)(i & 1), "StressHost", "StressMap");
                    Sleep(1);
                    uint64_t t0 = GetTickCount64();
                    LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
                    recordDuration(GetTickCount64() - t0);
                } else {
                    LanDiscovery_StartScan();
                    LanDiscovery_ProbeNow();
                    (void)LanDiscovery_GetHosts();
                    Sleep(1);
                    uint64_t t0 = GetTickCount64();
                    LanDiscovery_StopScan();
                    recordDuration(GetTickCount64() - t0);
                }
            }
        });
    }
    for (auto& th : phaseAThreads) {
        th.join();
    }
    TEST_CHECK(maxStopDurationMs.load() < 1500ULL);
    LanDiscovery_Shutdown();

    // Phase B: Concurrent Shutdown() racing against SetHosting(true) / StartScan() / ProbeNow().
    std::atomic<bool> stopHammer(false);
    std::thread hostHammer([&stopHammer]() {
        for (int i = 0; i < 40 && !stopHammer.load(); ++i) {
            LanDiscovery_SetHosting(true, 5556u, 0u, "RaceHost", "RaceMap");
            Sleep(1);
        }
    });
    std::thread scanHammer([&stopHammer]() {
        for (int i = 0; i < 40 && !stopHammer.load(); ++i) {
            LanDiscovery_StartScan();
            LanDiscovery_ProbeNow();
            Sleep(1);
        }
    });
    for (int i = 0; i < 20; ++i) {
        LanDiscovery_Shutdown();
        Sleep(2);
    }
    stopHammer.store(true);
    hostHammer.join();
    scanHammer.join();
    LanDiscovery_Shutdown();

    DWORD handlesAfterShutdown = 0;
    TEST_CHECK(GetProcessHandleCount(GetCurrentProcess(), &handlesAfterShutdown) != 0);
    TEST_CHECK(handlesAfterShutdown <= handlesAfterWarmup + 4);

    // Verify no zombie responder remains bound to testPort after Shutdown().
    {
        UdpSockScope checkPort(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        TEST_CHECK(checkPort.sock != INVALID_SOCKET);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(testPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        TEST_CHECK(bind(checkPort.sock, (const sockaddr*)&addr, sizeof(addr)) == 0);
    }

    // Verify clean restart after concurrent shutdown stress.
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, "RestartedHost", "RestartedMap");
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();
    bool rediscovered = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        for (const auto& h : LanDiscovery_GetHosts()) {
            if (strcmp(h.hostName, "RestartedHost") == 0 && strcmp(h.connectIp, "127.0.0.1") == 0) {
                rediscovered = true;
                break;
            }
        }
        if (rediscovered) break;
        if ((attempt % 5) == 4) {
            LanDiscovery_ProbeNow();
        }
        Sleep(30);
    }
    TEST_CHECK(rediscovered);

    LanDiscovery_StopScan();
    LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
    LanDiscovery_Shutdown();
    LanDiscovery_SetPortForTest(0);
    LanDiscovery_SetSelfPidOverrideForTest(0, false);
    return true;
}

static bool Test_LanDiscoveryAndAddressNormalization_M1()
{
    LanDiscovery_SetLoopbackOnlyForTest(true);
    Network_SetLoopbackOnlyForTest(true);

    printf("    [SubTest 1: F1 Multi-NIC Adapter Classification & Broadcast] ...\n"); fflush(stdout);
    if (!SubTest_LanM1_AdapterEnumAndBroadcast()) return false;

    printf("    [SubTest 2: F2 Wire Codec, Dedup Store & Wrap-Safe TTL Prune] ...\n"); fflush(stdout);
    if (!SubTest_LanM1_ProtocolCodecAndPureStore()) return false;

    printf("    [SubTest 3: F3/F4 UDP Loopback Responder/Scanner & Live Lifecycle] ...\n"); fflush(stdout);
    if (!SubTest_LanM1_LiveModuleLifecycle()) return false;
    if (!SubTest_LanM1_RawUdpLoopbackAndStress()) return false;

    printf("    [SubTest 4: F3/F4 Concurrent Lifecycle & Shutdown Stress] ...\n"); fflush(stdout);
    if (!SubTest_LanM1_ConcurrentLifecycleAndShutdownStress()) return false;

    printf("    [SubTest 5: F5 NetworkNormalizeTargetAddress & NetworkConnect] ...\n"); fflush(stdout);
    if (!SubTest_LanM1_AddressNormalizationAndConnect()) return false;

    return true;
}
