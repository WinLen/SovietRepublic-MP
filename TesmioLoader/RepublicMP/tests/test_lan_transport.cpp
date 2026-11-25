// SPDX-License-Identifier: GPL-3.0-only
// LAN discovery, Steam-free session & ModSync, LAN UI, and LAN E2E test suites (#56-#59).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "test_harness.h"

#include "lan_m1_tests.cpp"
#include "lan_m2_tests.cpp"
#include "lan_m3_tests.cpp"
#define REPUBLICMP_E2E_EXTERNAL_INCLUDES 1
#include "lan_e2e_tests.cpp"

bool Test_LanDiscoveryDirectSmoke_M1()
{
    if (!H) { TsmBind(&g_mockHost); }
    LanDiscovery_Bind(&g_mockHost);
    LanDiscovery_Init();
    (void)LanDiscovery_GetLocalAdapters(false);
    (void)LanDiscovery_GetLocalDisplayName();
    (void)LanDiscovery_ClassifyAdapter(6, "Ethernet", "Realtek", "192.168.1.10", nullptr);
    char m[16] = {};
    char b[16] = {};
    TEST_CHECK(LanDiscovery_ComputeBroadcast("192.168.1.10", 24, m, sizeof(m), b, sizeof(b)));
    LanProbePacket probe = {};
    probe.protocol = REPUBLICMP_PROTOCOL;
    uint8_t pbuf[32] = {};
    size_t plen = LanDiscovery_SerializeProbe(probe, pbuf, sizeof(pbuf));
    TEST_CHECK(plen == sizeof(LanProbePacket));
    TEST_CHECK(LanDiscovery_ParseProbe(pbuf, plen, &probe));
    LanAnnouncePacket ann = {};
    ann.protocol = REPUBLICMP_PROTOCOL;
    ann.tcpPort = 5556u;
    ann.serverGuid = 1ULL;
    ann.open = 1u;
    uint8_t abuf[256] = {};
    size_t alen = LanDiscovery_SerializeAnnounce(ann, abuf, sizeof(abuf));
    TEST_CHECK(alen == sizeof(LanAnnouncePacket));
    TEST_CHECK(LanDiscovery_ParseAnnounce(abuf, alen, &ann));
    std::vector<LanHostEntry> hosts;
    TEST_CHECK(LanDiscovery_UpsertHost(hosts, ann, "192.168.1.10", 1000ULL));
    LanDiscovery_PruneExpired(hosts, 6000ULL, LAN_DISCOVERY_TTL_MS);
    TEST_CHECK(hosts.empty());
    LanDiscovery_SetPortForTest(0);
    LanDiscovery_SetSelfPidOverrideForTest(0, false);
    LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
    LanDiscovery_UpdateHostingMode(0);
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();
    (void)LanDiscovery_GetHosts();
    LanDiscovery_StopScan();
    LanDiscovery_Shutdown();
    char norm[64] = {};
    uint16_t p = 0;
    TEST_CHECK(NetworkNormalizeTargetAddress(" 127.0.0.1:5556 ", norm, sizeof(norm), &p));
    return Test_LanDiscoveryAndAddressNormalization_M1();
}

bool Test_SteamFreeSessionAndModSyncDirectSmoke_M2()
{
    TEST_CHECK(!SteamP2P_IsLoggedOn());
    TEST_CHECK(!SteamP2P_IsAvailable());
    TEST_CHECK(SteamP2P_GetLocalPersonaNameCopy().empty());
    TEST_CHECK(SteamP2P_ParseTargetSteamId("steam:76561197960265728") == 76561197960265728ULL);
    TEST_CHECK(SaveSync_DetectDualTestbedInputs("HOST", nullptr, nullptr));
    char disp[64] = {};
    bool isStm = false;
    SaveSync_FormatDisplayTarget("127.0.0.1", 5556u, disp, sizeof(disp), &isStm);
    TEST_CHECK(!isStm && strcmp(disp, "127.0.0.1:5556") == 0);
    TEST_CHECK(DiagLog_IsSafeRemoteLogFileName("tesmioloader.log"));
    TEST_CHECK(!ModSync_IsSteamUgcOnline());
    ModSync_TickMainThread();
    char modBuf[MAX_PATH] = {};
    TEST_CHECK(!ModSync_FindLocalModFolder(0, modBuf, sizeof(modBuf)));
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(0, nullptr, false, modBuf, sizeof(modBuf)));
    (void)ModSync_GetLastOfflineWarning();
    SaveSync_SetTcpPortForTest(0);
    LockstepNet_SetPortForTest(0);
    return Test_SteamFreeSessionAndModSync_M2();
}

bool Test_LanUiBrowserHostIpAndToastsDirectSmoke_M3()
{
    TEST_CHECK(!UiCommon_CopyTextToClipboard(""));
    std::string clipTmp;
    (void)clipTmp;
    char normBuf[64] = {};
    TEST_CHECK(UiCommon_NormalizeClipboardJoinAddress(" 127.0.0.1 ", normBuf, sizeof(normBuf)));
    TEST_CHECK(UiCommon_NormalizeToastColorKind(99) == 5);
    std::vector<LanAdapterInfo> ads;
    TEST_CHECK(UiCommon_SelectPrimaryAdapter(ads) == nullptr);
    UiCommon_RefreshLocalAdaptersCache();
    (void)UiCommon_GetCachedLocalAdapters(false);
    (void)UiCommon_AdapterKindTag(LanAdapterKind::Ethernet, "Ethernet", "192.168.1.10", false);
    (void)UiCommon_FormatHostEndpoint("127.0.0.1", 5556);
    (void)UiCommon_FormatCopyableHostIp("127.0.0.1", 5556);
    LanAdapterInfo dummyAd = {};
    strncpy_s(dummyAd.ip, "192.168.1.10", _TRUNCATE);
    dummyAd.kind = LanAdapterKind::Ethernet;
    (void)UiCommon_FormatAdapterLine(dummyAd, 5556);
    const UiHostAddressView hv = UiCommon_BuildHostAddressView(ads, 5556);
    (void)UiCommon_FormatHostTransportSummary(hv.primaryEndpoint.c_str(), false, 0, false);
    TwoStates_AddToastAtTickForTest("smoke", 0, GetTickCount64());
    TwoStates_ClearToastsForTest();
    TEST_CHECK(TwoStates_GetToastCountForTest() == 0);
    (void)Lang_CopyIpLabel();
    (void)Lang_CopyIpLabelW();
    (void)Lang_CopiedLabel();
    (void)Lang_CopiedLabelW();
    (void)Lang_RefreshIpLabel();
    (void)Lang_SteamOfflineLabel();
    (void)Lang_SteamOfflineLabelW();
    (void)Lang_CopySteamCodeLabel();
    (void)Lang_CopySteamCodeLabelW();
    (void)NativeMenu_Utf8ToWideSafe("Host");
    (void)NativeMenu_TruncateLabel(L"Host", NATIVE_JOIN_MAX_LABEL_CHARS);
    LanHostEntry dummyHost = {};
    strncpy_s(dummyHost.connectIp, "127.0.0.1", _TRUNCATE);
    dummyHost.protocolMatch = true;
    (void)NativeMenu_FormatLanHostLabel(dummyHost);
    TEST_CHECK(NativeMenu_FormatConnectTarget(dummyHost, normBuf, sizeof(normBuf)));
    TEST_CHECK(NativeMenu_IsLanHostVersionCompatible(dummyHost, REPUBLICMP_VERSION, 0u));
    (void)NativeMenu_SelectSidebarExtraAdapters(ads, 2);
    std::vector<LanHostEntry> lh;
    std::vector<NativeMenuFriendInfo> sf;
    const auto planned = NativeMenu_PlanJoinEntries(false, lh, false, sf, "127.0.0.1", true);
    JoinPlanEntry sel = {};
    TEST_CHECK(NativeMenu_ResolveJoinClick(planned, 0, &sel));
    return Test_LanUiBrowserHostIpAndToasts_M3();
}

bool Test_LanE2eSuite_M4()
{
    if (!H) { TsmBind(&g_mockHost); }
    char why[128] = {};
    (void)SaveSync_ExchangeCustomHelloOverSocketForTest(
        static_cast<uintptr_t>(INVALID_SOCKET), false, "RPMPSES", REPUBLICMP_PROTOCOL,
        REPUBLICMP_VERSION, 0u, why, sizeof(why));
    (void)SaveSync_ExchangeHandshakeOverSocketForTest(
        static_cast<uintptr_t>(INVALID_SOCKET), false, "", true, "", why, sizeof(why));
    SaveSync_SetTcpPortForTest(0);
    LockstepNet_SetPortForTest(0);
    return RunLanE2eTestSuite() == 0;
}

namespace {

struct ScopedWinSocket {
    SOCKET s = INVALID_SOCKET;
    ~ScopedWinSocket()
    {
        if (s != INVALID_SOCKET) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
};

struct ScopedSteamTestSocket {
    SOCKET s = INVALID_SOCKET;
    ~ScopedSteamTestSocket()
    {
        if (s != INVALID_SOCKET) {
            SteamP2P_CloseSocket(s);
            s = INVALID_SOCKET;
        }
    }
};

static bool QueryTcpNoDelayEnabled(SOCKET sock)
{
    if (sock == INVALID_SOCKET) return false;
    BOOL val = FALSE;
    int len = static_cast<int>(sizeof(val));
    if (getsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&val), &len) == SOCKET_ERROR) {
        return false;
    }
    return val != 0;
}

static unsigned short ReserveEphemeralLoopbackPort()
{
    ScopedWinSocket probe;
    probe.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe.s == INVALID_SOCKET) return 0;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(probe.s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        return 0;
    }
    int addrLen = static_cast<int>(sizeof(addr));
    if (getsockname(probe.s, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

static bool TestTcpNoDelayOnLoopback()
{
    TEST_CHECK(NetworkInit());

    // Guard checks: INVALID_SOCKET and Steam virtual sockets must no-op cleanly.
    NetworkEnableNoDelay(INVALID_SOCKET);
    NetworkEnableNoDelay(static_cast<SOCKET>(0x53544D0000000000ULL));

    // Part 1: Direct OS ephemeral listener + NetworkConnect -> TCP_NODELAY enabled.
    ScopedWinSocket ephemListen;
    ephemListen.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_CHECK(ephemListen.s != INVALID_SOCKET);
    sockaddr_in ephemAddr = {};
    ephemAddr.sin_family = AF_INET;
    ephemAddr.sin_port = 0;
    ephemAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_CHECK(bind(ephemListen.s, reinterpret_cast<const sockaddr*>(&ephemAddr), sizeof(ephemAddr)) != SOCKET_ERROR);
    TEST_CHECK(listen(ephemListen.s, 1) != SOCKET_ERROR);
    int ephemLen = static_cast<int>(sizeof(ephemAddr));
    TEST_CHECK(getsockname(ephemListen.s, reinterpret_cast<sockaddr*>(&ephemAddr), &ephemLen) != SOCKET_ERROR);
    const unsigned short ephemPort = ntohs(ephemAddr.sin_port);
    TEST_CHECK(ephemPort != 0);

    ScopedWinSocket directConn;
    directConn.s = NetworkConnect("127.0.0.1", ephemPort, 2000, 2000);
    TEST_CHECK(directConn.s != INVALID_SOCKET);
    TEST_CHECK(QueryTcpNoDelayEnabled(directConn.s));

    ScopedWinSocket directAccepted;
    directAccepted.s = accept(ephemListen.s, nullptr, nullptr);
    TEST_CHECK(directAccepted.s != INVALID_SOCKET);

    // Part 2: NetworkHostAcceptOne + NetworkConnect -> TCP_NODELAY on BOTH sockets.
    const unsigned short hostPort = ReserveEphemeralLoopbackPort();
    TEST_CHECK(hostPort != 0);

    volatile bool cancelAccept = false;
    ScopedWinSocket acceptedSock;
    std::thread acceptThread([&]() {
        acceptedSock.s = NetworkHostAcceptOne(hostPort, 3000, 2000, &cancelAccept);
    });

    ScopedWinSocket clientSock;
    const ULONGLONG deadline = GetTickCount64() + 2500ULL;
    while (clientSock.s == INVALID_SOCKET && GetTickCount64() < deadline) {
        clientSock.s = NetworkConnect("127.0.0.1", hostPort, 250, 2000);
        if (clientSock.s == INVALID_SOCKET) {
            Sleep(15);
        }
    }
    if (clientSock.s == INVALID_SOCKET) {
        cancelAccept = true;
    }
    acceptThread.join();

    TEST_CHECK(clientSock.s != INVALID_SOCKET);
    TEST_CHECK(acceptedSock.s != INVALID_SOCKET);
    TEST_CHECK(QueryTcpNoDelayEnabled(clientSock.s));
    TEST_CHECK(QueryTcpNoDelayEnabled(acceptedSock.s));
    return true;
}

static void FillPattern(std::vector<uint8_t>& out, size_t len, uint32_t seed)
{
    out.resize(len);
    for (size_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>(((i + seed) * 37u) ^ 0x5Au);
    }
}

static bool TestSteamP2PRingBufferWrapGrowAndCap()
{
    static constexpr uint64_t kPeerId = 76561198000000101ULL;
    static constexpr unsigned short kPort = 5560;
    static constexpr uint32_t kConnId = 0x11223344U;
    static constexpr uint8_t kFrameData = 3;

    ScopedSteamTestSocket sock;
    sock.s = SteamP2P_CreateTestSocketForTest(kPeerId, kPort, kConnId, 2000, true);
    TEST_CHECK(sock.s != INVALID_SOCKET);

    size_t sz = 0, cap = 0, head = 0;
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 0 && cap == 0 && head == 0);

    // 1. Initial push (3000 B -> 4096 B initial power-of-2 capacity).
    std::vector<uint8_t> chunk1;
    FillPattern(chunk1, 3000, 1);
    SteamP2P_InjectTestFrameForTest(kPeerId, kFrameData, kPort, kConnId, chunk1.data(), 3000u);
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 3000u && cap == 4096u && head == 0u);

    // 2. Pop 2000 B -> advances head to 2000 within 4096 B ring.
    std::vector<uint8_t> out2000(2000);
    TEST_CHECK(SteamP2P_RecvExact(sock.s, out2000.data(), 2000u));
    TEST_CHECK(memcmp(out2000.data(), chunk1.data(), 2000u) == 0);
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 1000u && cap == 4096u && head == 2000u);

    // 3. Push 2500 B -> wraps around 4096 B end-to-start boundary while staying at cap=4096.
    std::vector<uint8_t> chunk2;
    FillPattern(chunk2, 2500, 3001);
    SteamP2P_InjectTestFrameForTest(kPeerId, kFrameData, kPort, kConnId, chunk2.data(), 2500u);
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 3500u && cap == 4096u && head == 2000u);

    // 4. Push 2000 B while wrapped -> must double to 8192 B and linearize (head == 0).
    std::vector<uint8_t> chunk3;
    FillPattern(chunk3, 2000, 7001);
    SteamP2P_InjectTestFrameForTest(kPeerId, kFrameData, kPort, kConnId, chunk3.data(), 2000u);
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 5500u && cap == 8192u && head == 0u);

    std::vector<uint8_t> drained(5500);
    TEST_CHECK(SteamP2P_RecvExact(sock.s, drained.data(), 5500u));
    TEST_CHECK(memcmp(drained.data(), chunk1.data() + 2000u, 1000u) == 0);
    TEST_CHECK(memcmp(drained.data() + 1000u, chunk2.data(), 2500u) == 0);
    TEST_CHECK(memcmp(drained.data() + 3500u, chunk3.data(), 2000u) == 0);
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 0u && head == 0u);

    // 5. Fill to 16 MB cap and confirm overflow rejection + FIN handling.
    std::vector<uint8_t> oneMb;
    FillPattern(oneMb, 1024u * 1024u, 99);
    for (int i = 0; i < 16; ++i) {
        SteamP2P_InjectTestFrameForTest(kPeerId, kFrameData, kPort, kConnId,
                                        oneMb.data(), static_cast<uint32_t>(oneMb.size()));
    }
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 16u * 1024u * 1024u && cap == 16u * 1024u * 1024u);
    TEST_CHECK(!SteamP2P_PeerGone(sock.s));

    const uint8_t overflowByte = 0x7Fu;
    SteamP2P_InjectTestFrameForTest(kPeerId, kFrameData, kPort, kConnId, &overflowByte, 1u);
    TEST_CHECK(SteamP2P_GetSlotBufferStatsForTest(sock.s, &sz, &cap, &head));
    TEST_CHECK(sz == 16u * 1024u * 1024u);

    std::vector<uint8_t> recvOneMb(1024u * 1024u);
    for (int i = 0; i < 16; ++i) {
        TEST_CHECK(SteamP2P_RecvExact(sock.s, recvOneMb.data(), recvOneMb.size()));
        TEST_CHECK(memcmp(recvOneMb.data(), oneMb.data(), oneMb.size()) == 0);
    }
    TEST_CHECK(SteamP2P_PeerGone(sock.s));
    TEST_CHECK(SteamP2P_PollReadable(sock.s, 0, 1) == -1);

    // 6. CloseSocket frees backing buffer and marks slot closed.
    const SOCKET rawSock = sock.s;
    SteamP2P_CloseSocket(rawSock);
    sock.s = INVALID_SOCKET;
    TEST_CHECK(!SteamP2P_GetSlotBufferStatsForTest(rawSock, &sz, &cap, &head));
    TEST_CHECK(sz == 0u && cap == 0u && head == 0u);
    return true;
}

static bool TestSteamP2PScratchBufferReuse()
{
    static constexpr uint64_t kPeerId = 76561198000000202ULL;
    static constexpr unsigned short kPort = 5556;
    static constexpr uint32_t kConnId = 0x55667788U;

    ScopedSteamTestSocket sock;
    sock.s = SteamP2P_CreateTestSocketForTest(kPeerId, kPort, kConnId, 2000, true);
    TEST_CHECK(sock.s != INVALID_SOCKET);

    std::vector<uint8_t> largePayload;
    FillPattern(largePayload, 65536u, 17);
    const void* sendPtr1 = nullptr;
    const void* recvPtr1 = nullptr;
    size_t sendCap1 = 0;
    size_t recvCap1 = 0;
    TEST_CHECK(SteamP2P_TestScratchAndDispatchForTest(
        sock.s, largePayload.data(), static_cast<uint32_t>(largePayload.size()),
        2000000u, &sendPtr1, &sendCap1, &recvPtr1, &recvCap1));
    TEST_CHECK(sendPtr1 != nullptr && recvPtr1 != nullptr);
    TEST_CHECK(sendCap1 >= largePayload.size() && recvCap1 >= largePayload.size());

    std::vector<uint8_t> recvLarge(65536u);
    TEST_CHECK(SteamP2P_RecvExact(sock.s, recvLarge.data(), recvLarge.size()));
    TEST_CHECK(memcmp(recvLarge.data(), largePayload.data(), largePayload.size()) == 0);

    std::vector<uint8_t> smallPayload;
    FillPattern(smallPayload, 128u, 33);
    const void* sendPtr2 = nullptr;
    const void* recvPtr2 = nullptr;
    size_t sendCap2 = 0;
    size_t recvCap2 = 0;
    TEST_CHECK(SteamP2P_TestScratchAndDispatchForTest(
        sock.s, smallPayload.data(), static_cast<uint32_t>(smallPayload.size()),
        0u, &sendPtr2, &sendCap2, &recvPtr2, &recvCap2));
    TEST_CHECK(sendPtr2 == sendPtr1 && sendCap2 == sendCap1);
    TEST_CHECK(recvPtr2 == recvPtr1 && recvCap2 == recvCap1);

    std::vector<uint8_t> recvSmall(128u);
    TEST_CHECK(SteamP2P_RecvExact(sock.s, recvSmall.data(), recvSmall.size()));
    TEST_CHECK(memcmp(recvSmall.data(), smallPayload.data(), smallPayload.size()) == 0);
    return true;
}

static double QpcElapsedMs(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq)
{
    return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) /
           static_cast<double>(freq.QuadPart);
}

static bool TestSteamP2PCondVarWakeup()
{
    static constexpr uint64_t kPeerId = 76561198000000303ULL;
    static constexpr unsigned short kPort = 5560;
    static constexpr uint32_t kConnId = 0x99AABBCCU;
    static constexpr uint8_t kFrameData = 3;
    static constexpr uint8_t kFrameFin = 4;

    LARGE_INTEGER freq = {};
    TEST_CHECK(QueryPerformanceFrequency(&freq) != 0 && freq.QuadPart > 0);

    // Case A: PollReadable(timeoutMs=1500, minBytes=8) wakes early when DATA arrives.
    ScopedSteamTestSocket sock;
    sock.s = SteamP2P_CreateTestSocketForTest(kPeerId, kPort, kConnId, 2000, true);
    TEST_CHECK(sock.s != INVALID_SOCKET);

    std::atomic<bool> waiterReady{false};
    LARGE_INTEGER injectQpc = {};
    const uint8_t msg8[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    std::thread prodPoll([&]() {
        while (!waiterReady.load()) {
            Sleep(1);
        }
        Sleep(20);
        QueryPerformanceCounter(&injectQpc);
        SteamP2P_InjectTestFrameForTest(kPeerId, kFrameData, kPort, kConnId, msg8, 8u);
    });

    LARGE_INTEGER startPoll = {};
    QueryPerformanceCounter(&startPoll);
    waiterReady.store(true);
    const int pollRes = SteamP2P_PollReadable(sock.s, 1500u, 8u);
    LARGE_INTEGER endPoll = {};
    QueryPerformanceCounter(&endPoll);
    prodPoll.join();

    TEST_CHECK(pollRes == 1);
    TEST_CHECK(QpcElapsedMs(startPoll, endPoll, freq) < 500.0);
    TEST_CHECK(QpcElapsedMs(injectQpc, endPoll, freq) < 50.0);

    uint8_t out8[8] = {};
    TEST_CHECK(SteamP2P_RecvExact(sock.s, out8, 8u));
    TEST_CHECK(memcmp(out8, msg8, 8u) == 0);

    // Case B: RecvExact wakes early on DATA and terminates promptly on FIN.
    std::atomic<bool> recvReady{false};
    const uint8_t msg16[16] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
    };
    std::thread prodRecv([&]() {
        while (!recvReady.load()) {
            Sleep(1);
        }
        Sleep(20);
        SteamP2P_InjectTestFrameForTest(kPeerId, kFrameData, kPort, kConnId, msg16, 16u);
        Sleep(20);
        SteamP2P_InjectTestFrameForTest(kPeerId, kFrameFin, kPort, kConnId, nullptr, 0u);
    });

    LARGE_INTEGER startRecv = {};
    QueryPerformanceCounter(&startRecv);
    recvReady.store(true);
    uint8_t out16[16] = {};
    const bool ok16 = SteamP2P_RecvExact(sock.s, out16, 16u);
    uint8_t extra = 0;
    const bool okEof = SteamP2P_RecvExact(sock.s, &extra, 1u);
    LARGE_INTEGER endRecv = {};
    QueryPerformanceCounter(&endRecv);
    prodRecv.join();

    TEST_CHECK(ok16);
    TEST_CHECK(memcmp(out16, msg16, 16u) == 0);
    TEST_CHECK(!okEof);
    TEST_CHECK(QpcElapsedMs(startRecv, endRecv, freq) < 500.0);

    // Case C: ShutdownSocket wakes blocked PollReadable immediately with -1.
    ScopedSteamTestSocket sockShut;
    sockShut.s = SteamP2P_CreateTestSocketForTest(kPeerId + 1ULL, kPort, kConnId + 1U, 2000, true);
    TEST_CHECK(sockShut.s != INVALID_SOCKET);
    std::atomic<bool> shutReady{false};
    std::thread prodShut([&]() {
        while (!shutReady.load()) {
            Sleep(1);
        }
        Sleep(20);
        SteamP2P_ShutdownSocket(sockShut.s);
    });
    LARGE_INTEGER startShut = {};
    QueryPerformanceCounter(&startShut);
    shutReady.store(true);
    const int shutRes = SteamP2P_PollReadable(sockShut.s, 1500u, 1u);
    LARGE_INTEGER endShut = {};
    QueryPerformanceCounter(&endShut);
    prodShut.join();

    TEST_CHECK(shutRes == -1);
    TEST_CHECK(QpcElapsedMs(startShut, endShut, freq) < 500.0);
    return true;
}

static bool TestSteamP2PAbaSlotReuseRace()
{
    static constexpr uint64_t kPeer0 = 76561198000000401ULL;
    static constexpr uint64_t kPeerNew = 76561198000000402ULL;
    static constexpr unsigned short kPort = 5560;
    static constexpr uint32_t kConnBase0 = 0x10000000U;
    static constexpr uint32_t kConnBaseNew = 0x20000000U;
    static constexpr uint8_t kFrameData = 3;
    static const uint8_t kNewPayload[4] = { 'N', 'E', 'W', '!' };

    for (uint32_t iter = 0; iter < 50u; ++iter) {
        const SOCKET sock0 = SteamP2P_CreateTestSocketForTest(
            kPeer0, kPort, kConnBase0 + iter, 1500u, true);
        TEST_CHECK(sock0 != INVALID_SOCKET);

        std::atomic<int> readersReady{0};
        bool recvOk = true;
        uint8_t stolenBuf[4] = {};
        std::thread readerRecv([&]() {
            readersReady.fetch_add(1);
            recvOk = SteamP2P_RecvExact(sock0, stolenBuf, sizeof(stolenBuf));
        });

        int pollRes = 99;
        std::thread readerPoll([&]() {
            readersReady.fetch_add(1);
            pollRes = SteamP2P_PollReadable(sock0, 1500u, 4u);
        });

        while (readersReady.load() < 2) {
            Sleep(1);
        }
        Sleep(2);

        SteamP2P_CloseSocket(sock0);
        ScopedSteamTestSocket sockNew;
        sockNew.s = SteamP2P_CreateTestSocketForTest(
            kPeerNew, kPort, kConnBaseNew + iter, 1500u, true);
        TEST_CHECK(sockNew.s != INVALID_SOCKET);
        TEST_CHECK(sockNew.s != sock0);
        SteamP2P_InjectTestFrameForTest(
            kPeerNew, kFrameData, kPort, kConnBaseNew + iter,
            kNewPayload, static_cast<uint32_t>(sizeof(kNewPayload)));

        readerRecv.join();
        readerPoll.join();
        TEST_CHECK(!recvOk);
        TEST_CHECK(pollRes == -1);

        // Stale operations on sock0 must be strict no-ops against sockNew.
        SteamP2P_ShutdownSocket(sock0);
        SteamP2P_CloseSocket(sock0);
        TEST_CHECK(!SteamP2P_PeerGone(sockNew.s));
        size_t sz = 1, cap = 1, head = 1;
        TEST_CHECK(!SteamP2P_GetSlotBufferStatsForTest(sock0, &sz, &cap, &head));
        TEST_CHECK(sz == 0u && cap == 0u && head == 0u);

        TEST_CHECK(SteamP2P_PollReadable(sockNew.s, 0u, 4u) == 1);
        uint8_t newOut[4] = {};
        TEST_CHECK(SteamP2P_RecvExact(sockNew.s, newOut, sizeof(newOut)));
        TEST_CHECK(memcmp(newOut, kNewPayload, sizeof(kNewPayload)) == 0);
    }
    return true;
}

} // namespace

bool TestPerf_TcpNoDelayAndSteamP2PRingBuffer()
{
    if (!TestTcpNoDelayOnLoopback()) return false;
    if (!TestSteamP2PRingBufferWrapGrowAndCap()) return false;
    if (!TestSteamP2PScratchBufferReuse()) return false;
    if (!TestSteamP2PCondVarWakeup()) return false;
    if (!TestSteamP2PAbaSlotReuseRace()) return false;
    return true;
}
