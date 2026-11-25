// SPDX-License-Identifier: GPL-3.0-only
// Hotfix v1.7.1 regression tests:
//   1. Crash A (0x41E9F0 corrupt signal block pointer guard)
//   2. Crash B (Filling worker UAF on vehicle sale + UnloadWorld queue clear)
//   3. Client Host-Lost modal truth table, input/pause lock, ExitToMainMenu
//   4. Steam Friends probe resolution (stale PONG vs rpHosting) & Join Menu formatting
//   5. Sliding-window speed meter + 127.0.0.1 ACK-paced file transfer (0-byte, throttled, cancel)

#include <cmath>
#include <mutex>

static bool V171_MakeLoopbackSocketPair(SOCKET* outSrv, SOCKET* outCli)
{
    *outSrv = INVALID_SOCKET;
    *outCli = INVALID_SOCKET;
    if (!NetworkInit()) return false;

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) return false;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listenSock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listenSock, 1) != 0) {
        closesocket(listenSock);
        return false;
    }

    int addrLen = sizeof(addr);
    if (getsockname(listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        closesocket(listenSock);
        return false;
    }

    SOCKET cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (cli == INVALID_SOCKET) {
        closesocket(listenSock);
        return false;
    }
    if (connect(cli, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(cli);
        closesocket(listenSock);
        return false;
    }

    SOCKET srv = accept(listenSock, nullptr, nullptr);
    closesocket(listenSock);
    if (srv == INVALID_SOCKET) {
        closesocket(cli);
        return false;
    }
    DWORD ioTimeoutMs = 15000;
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ioTimeoutMs), sizeof(ioTimeoutMs));
    setsockopt(srv, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ioTimeoutMs), sizeof(ioTimeoutMs));
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ioTimeoutMs), sizeof(ioTimeoutMs));
    setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ioTimeoutMs), sizeof(ioTimeoutMs));
    *outSrv = srv;
    *outCli = cli;
    return true;
}

static bool SubTest_V171_CrashASignalSegmentGuard()
{
    TEST_CHECK(!SimPatches_IsSignalSegmentSafe(nullptr));
    TEST_CHECK(!SimPatches_IsSignalSegmentSafe(reinterpret_cast<const void*>(0x90ULL)));
    TEST_CHECK(!SimPatches_TestHookedUpdateSignalBlock(nullptr, reinterpret_cast<void*>(0x90ULL)));

    std::vector<BYTE> seg(0x100, 0);
    // Case 1: exact Crash A signature — [seg + 0xB8] == 0x90
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_SIGNAL_BLOCK_PTR_OFFSET) = 0x90ULL;
    TEST_CHECK(!SimPatches_IsSignalSegmentSafe(seg.data()));
    TEST_CHECK(!SimPatches_TestHookedUpdateSignalBlock(nullptr, seg.data()));

    // Case 2: sigBlock valid pointer, kind != 0x0A, but [sigBlock + 0x08] node ptr is invalid (0x41EB5A guard)
    std::vector<BYTE> sigBlock(0x40, 0);
    std::vector<BYTE> nodeObj(0x40, 0);
    *reinterpret_cast<uint64_t*>(sigBlock.data() + SIGNAL_BLOCK_NODE_PTR_OFFSET) = 0x90ULL;
    *reinterpret_cast<uint32_t*>(sigBlock.data() + SIGNAL_BLOCK_KIND_OFFSET) = 1u;
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_SIGNAL_BLOCK_PTR_OFFSET) =
        reinterpret_cast<uint64_t>(sigBlock.data());
    TEST_CHECK(!SimPatches_IsSignalSegmentSafe(seg.data()));

    // Case 3: sigBlock valid with valid node ptr and kind != 0x0A -> safe
    *reinterpret_cast<uint64_t*>(sigBlock.data() + SIGNAL_BLOCK_NODE_PTR_OFFSET) =
        reinterpret_cast<uint64_t>(nodeObj.data());
    TEST_CHECK(SimPatches_IsSignalSegmentSafe(seg.data()));
    TEST_CHECK(SimPatches_TestHookedUpdateSignalBlock(nullptr, seg.data()));

    // Case 4: kind == 0x0A falls through to connected-node vector (+0x10/+0x18); bad vector rejected
    *reinterpret_cast<uint32_t*>(sigBlock.data() + SIGNAL_BLOCK_KIND_OFFSET) = SIGNAL_BLOCK_KIND_SKIP;
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_CONNECTED_NODES_BEGIN_OFF) = 0x90ULL;
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_CONNECTED_NODES_END_OFF) = 0xA0ULL;
    TEST_CHECK(!SimPatches_IsSignalSegmentSafe(seg.data()));

    // Case 5: empty node vector or valid node vector with kind == 0x0A (or null sigBlock) -> safe
    std::vector<BYTE*> nodes = { nodeObj.data() };
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_CONNECTED_NODES_BEGIN_OFF) =
        reinterpret_cast<uint64_t>(nodes.data());
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_CONNECTED_NODES_END_OFF) =
        reinterpret_cast<uint64_t>(nodes.data() + nodes.size());
    TEST_CHECK(SimPatches_IsSignalSegmentSafe(seg.data()));

    *reinterpret_cast<uint64_t*>(seg.data() + SEG_SIGNAL_BLOCK_PTR_OFFSET) = 0ULL;
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_CONNECTED_NODES_BEGIN_OFF) = 0ULL;
    *reinterpret_cast<uint64_t*>(seg.data() + SEG_CONNECTED_NODES_END_OFF) = 0ULL;
    TEST_CHECK(SimPatches_IsSignalSegmentSafe(seg.data()));

    return true;
}

static bool SubTest_V171_CrashBFillingWorkerAndSale()
{
    ResetMockWorldMemory();

    std::vector<BYTE> v1(0x1800, 0), v2(0x1800, 0), v3(0x1800, 0);
    *(int*)(v1.data() + VEH_ID_OFFSET) = 101;
    *(int*)(v2.data() + VEH_ID_OFFSET) = 102;
    *(int*)(v3.data() + VEH_ID_OFFSET) = 103;
    std::vector<BYTE*> activeVehs = { v1.data(), v2.data(), v3.data() };
    std::vector<BYTE*> fillingVehs = { v1.data(), v2.data(), v2.data(), v3.data() };

    WriteMockVectorHeader(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA, activeVehs);
    WriteMockVectorHeader(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, fillingVehs);

    TEST_CHECK(TwoStates_PtrInStdVector(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA, v2.data()));
    TEST_CHECK(!TwoStates_PtrInStdVector(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA,
                                          reinterpret_cast<const void*>(0x123456ULL)));

    // Selling v2 must purge all copies of v2 from VEC_FILLING_VEHICLES_BEGIN_RVA while keeping v1, v3
    TwoStates_VehicleSaleForTest(g_mockExeBuffer + WORLD_RVA, v2.data(), 500.0f, 500.0f);
    TEST_CHECK(!TwoStates_PtrInStdVector(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, v2.data()));
    TEST_CHECK(TwoStates_PtrInStdVector(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, v1.data()));
    TEST_CHECK(TwoStates_PtrInStdVector(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, v3.data()));
    TEST_CHECK(SafeCountStdVector(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, sizeof(void*)) == 2);

    // Corrupt header bounds fail safely without crashing
    uint64_t badHdr[2] = { 0x90ULL, 0xB0ULL };
    TEST_CHECK(!TwoStates_PtrInStdVector(badHdr, v1.data()));
    TEST_CHECK(TwoStates_ErasePtrFromStdVector(badHdr, v1.data()) == 0);

    // ClearFillingWorkQueues empties all 3 Filling queues (0xA317F8, [0xA317D0]+0x60, [0xA2C3F8]+0x40)
    std::vector<BYTE> queueObj(0x100, 0), pathsObj(0x100, 0);
    std::vector<BYTE*> qEntries = { v1.data(), v3.data() };
    std::vector<BYTE*> pEntries = { v1.data() };
    WriteMockVectorHeader(queueObj.data() + 0x60u, qEntries);
    WriteMockVectorHeader(pathsObj.data() + 0x40u, pEntries);
    *reinterpret_cast<BYTE**>(g_mockExeBuffer + FILLING_QUEUE_PTR_RVA) = queueObj.data();
    *reinterpret_cast<BYTE**>(g_mockExeBuffer + FILLING_PATHS_PTR_RVA) = pathsObj.data();

    Determinism_ClearFillingWorkQueues(g_mockExeBuffer);
    TEST_CHECK(SafeCountStdVector(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, sizeof(void*)) == 0);
    TEST_CHECK(SafeCountStdVector(queueObj.data() + 0x60u, sizeof(void*)) == 0);
    TEST_CHECK(SafeCountStdVector(pathsObj.data() + 0x40u, sizeof(void*)) == 0);

    // Null queue/paths pointers do not crash
    *reinterpret_cast<BYTE**>(g_mockExeBuffer + FILLING_QUEUE_PTR_RVA) = nullptr;
    *reinterpret_cast<BYTE**>(g_mockExeBuffer + FILLING_PATHS_PTR_RVA) = nullptr;
    Determinism_ClearFillingWorkQueues(g_mockExeBuffer);

    return true;
}

static bool SubTest_V171_ClientHostLostModalAndExit()
{
    ResetMockWorldMemory();
    SaveSync_SetTestSessionState(SESSION_IDLE, false, nullptr);
    SaveSync_DismissClientHostLost();
    TEST_CHECK(!SaveSync_IsClientHostLost());

    // Truth table for SaveSync_ShouldShowClientHostLostModal
    TEST_CHECK(SaveSync_ShouldShowClientHostLostModal(SESSION_IDLE, true, true));
    TEST_CHECK(!SaveSync_ShouldShowClientHostLostModal(SESSION_IDLE, false, true));
    TEST_CHECK(!SaveSync_ShouldShowClientHostLostModal(SESSION_IDLE, true, false));
    TEST_CHECK(!SaveSync_ShouldShowClientHostLostModal(SESSION_CLIENT_IN_GAME, true, true));
    TEST_CHECK(!SaveSync_ShouldShowClientHostLostModal(SESSION_CLIENT_JOINING, true, true));
    TEST_CHECK(!SaveSync_ShouldShowClientHostLostModal(SESSION_HOST_IN_GAME, true, true));

    // When world is loaded and host-lost flag is active at SESSION_IDLE, input must lock
    std::vector<BYTE> dummyBld(0x2000, 0);
    std::vector<BYTE*> blds = { dummyBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, blds);
    *reinterpret_cast<uint64_t*>(g_mockExeBuffer + 0xA2E760u) = 0x1000ULL; // ROAD_NETWORK_RVA non-null
    *(volatile char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;

    TEST_CHECK(SaveSync_IsWorldLoaded());
    TEST_CHECK(!SyncBlock_BlocksWorldInput());

    SaveSync_SetClientHostLostForTest(true, "192.168.1.44", "Host: Comrade", "host stopped responding");
    TEST_CHECK(SaveSync_IsClientHostLost());
    TEST_CHECK(SyncBlock_BlocksWorldInput());

    ClientHostLostInfo info = {};
    TEST_CHECK(SaveSync_GetClientHostLostInfo(&info));
    TEST_CHECK(info.active);
    TEST_CHECK(strcmp(info.hostAddress, "192.168.1.44") == 0);
    TEST_CHECK(strcmp(info.hostLabel, "Host: Comrade") == 0);
    TEST_CHECK(strstr(info.reason, "host stopped responding") != nullptr);

    // Verify Host-Lost modal expands vertically with AutoResizeY (no collapsed child)
    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGuiContext* hostLostCtx = ImGui::CreateContext();
    TEST_CHECK(hostLostCtx != nullptr);
    float cardW = 0.0f, cardH = 0.0f, innerH = 0.0f, contentY = 0.0f;
    bool hasChild = false;
    if (hostLostCtx) {
        ImGui::SetCurrentContext(hostLostCtx);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* fontPx = nullptr;
        int fontW = 0, fontH = 0;
        io.Fonts->GetTexDataAsRGBA32(&fontPx, &fontW, &fontH);
        for (int frameIdx = 0; frameIdx < 3; ++frameIdx) {
            ImGui::NewFrame();
            SyncBlock_Draw();
            ImGui::Render();
        }
        if (ImGuiWindow* cardWin = ImGui::FindWindowByName("##republicmp_client_host_lost_card")) {
            cardW = cardWin->Size.x;
            cardH = cardWin->Size.y;
            if (cardWin->DC.ChildWindows.Size >= 1 && cardWin->DC.ChildWindows[0]) {
                ImGuiWindow* innerWin = cardWin->DC.ChildWindows[0];
                hasChild = true;
                innerH = innerWin->Size.y;
                contentY = innerWin->ContentSize.y;
            }
        }
        ImGui::DestroyContext(hostLostCtx);
        ImGui::SetCurrentContext(prevCtx);
    }
    printf("    [V171-HostLostModal] card=%.1fx%.1f innerH=%.1f contentY=%.1f\n",
           cardW, cardH, innerH, contentY);
    TEST_CHECK(cardW >= 760.0f && cardH >= 240.0f);
    TEST_CHECK(hasChild && innerH >= 160.0f);
    TEST_CHECK(contentY <= innerH + 4.0f);

    // Truth table for restoring Host-Lost modal on failed reconnect
    TEST_CHECK(SaveSync_ShouldRestoreClientHostLostOnJoinFailure(true, true, false));
    TEST_CHECK(!SaveSync_ShouldRestoreClientHostLostOnJoinFailure(false, true, false));
    TEST_CHECK(!SaveSync_ShouldRestoreClientHostLostOnJoinFailure(true, false, false));
    TEST_CHECK(!SaveSync_ShouldRestoreClientHostLostOnJoinFailure(true, true, true));

    // If Reconnect is clicked while host is not ready yet, failed connect restores the Host-Lost menu & pause
    TEST_CHECK(NetworkInit());
    SOCKET probeSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_CHECK(probeSock != INVALID_SOCKET);
    sockaddr_in probeAddr = {};
    probeAddr.sin_family = AF_INET;
    probeAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    probeAddr.sin_port = 0;
    TEST_CHECK(bind(probeSock, reinterpret_cast<const sockaddr*>(&probeAddr), sizeof(probeAddr)) == 0);
    int probeLen = sizeof(probeAddr);
    TEST_CHECK(getsockname(probeSock, reinterpret_cast<sockaddr*>(&probeAddr), &probeLen) == 0);
    const uint16_t closedPort = ntohs(probeAddr.sin_port);
    closesocket(probeSock);

    char closedTarget[64] = {};
    _snprintf_s(closedTarget, sizeof(closedTarget), _TRUNCATE, "127.0.0.1:%u", static_cast<unsigned>(closedPort));
    *reinterpret_cast<int*>(g_mockExeBuffer + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET) = 1;
    SaveSync_EnforceWaitingLobbyHold(g_mockExeBuffer);
    TEST_CHECK(*reinterpret_cast<const int*>(g_mockExeBuffer + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET) == 0);

    SaveSync_SetClientHostLostForTest(true, closedTarget, "Host: Comrade", "host stopped responding");
    TEST_CHECK(SaveSync_ReconnectAfterHostLost());
    for (int waitIdx = 0; waitIdx < 400 && SaveSync_GetState() != SESSION_IDLE; ++waitIdx) {
        Sleep(20);
    }
    TEST_CHECK(SaveSync_GetState() == SESSION_IDLE);
    TEST_CHECK(SaveSync_IsClientHostLost());
    TEST_CHECK(SaveSync_ShouldShowClientHostLostModal(SESSION_IDLE, true, true));
    TEST_CHECK(SyncBlock_BlocksWorldInput());
    ClientHostLostInfo reInfo = {};
    TEST_CHECK(SaveSync_GetClientHostLostInfo(&reInfo));
    TEST_CHECK(reInfo.active);
    TEST_CHECK(strcmp(reInfo.hostAddress, closedTarget) == 0);
    TEST_CHECK(strcmp(reInfo.hostLabel, "Host: Comrade") == 0);
    TEST_CHECK(reInfo.reason[0] != '\0');
    TEST_CHECK(strcmp(reInfo.reason, "host stopped responding") != 0);
    TEST_CHECK(*reinterpret_cast<const int*>(g_mockExeBuffer + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET) == 0);

    // Dismissing unblocks offline continuation and rejects host-lost reconnect without active prompt
    SaveSync_DismissClientHostLost();
    TEST_CHECK(!SaveSync_IsClientHostLost());
    TEST_CHECK(!SyncBlock_BlocksWorldInput());
    TEST_CHECK(!SaveSync_ReconnectAfterHostLost());

    // ExitToMainMenu clears host-lost + Filling queues + writes menu transition bytes
    std::vector<BYTE> v1(0x100, 0);
    std::vector<BYTE*> fVec = { v1.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, fVec);
    g_mockExeBuffer[WORLD_RVA + WORLD_EXIT_DESKTOP_FLAG_OFF] = 1;
    g_mockExeBuffer[EXIT_TO_MENU_FLAG_RVA] = 0;
    SaveSync_SetClientHostLostForTest(true, "steam:76561198000000001", "Steam:WinLen", "closed");

    TEST_CHECK(SaveSync_ExitToMainMenu(g_mockExeBuffer));
    TEST_CHECK(!SaveSync_IsClientHostLost());
    TEST_CHECK(SafeCountStdVector(g_mockExeBuffer + VEC_FILLING_VEHICLES_BEGIN_RVA, sizeof(void*)) == 0);
    TEST_CHECK(g_mockExeBuffer[WORLD_RVA + WORLD_EXIT_DESKTOP_FLAG_OFF] == 0);
    TEST_CHECK(g_mockExeBuffer[EXIT_TO_MENU_FLAG_RVA] == 1);

    return true;
}

static bool SubTest_V171_SteamFriendsAndJoinMenu()
{
    // ResolveFriendHostingState regression matrix
    TEST_CHECK(SteamP2P_ResolveFriendHostingState(false, true, true, 1500ULL, 100ULL, 200ULL));
    // Old positive PONG (>8s) without Rich Presence -> false
    TEST_CHECK(!SteamP2P_ResolveFriendHostingState(false, true, true, 9000ULL, 100ULL, 200ULL));
    // Stale negative PONG from before host started -> rpHosting wins (regression fix!)
    TEST_CHECK(SteamP2P_ResolveFriendHostingState(true, true, false, 1500ULL, 500ULL, 300ULL));
    TEST_CHECK(SteamP2P_ResolveFriendHostingState(true, true, false, 5000ULL, 100ULL, 200ULL));
    // Fresh negative PONG answering our most recent ping while <=4s -> overrides rpHosting to false
    TEST_CHECK(!SteamP2P_ResolveFriendHostingState(true, true, false, 1000ULL, 100ULL, 200ULL));
    // No PONG at all, rpHosting true -> true; false -> false
    TEST_CHECK(SteamP2P_ResolveFriendHostingState(true, false, false, 0ULL, 100ULL, 0ULL));
    TEST_CHECK(!SteamP2P_ResolveFriendHostingState(false, false, false, 0ULL, 100ULL, 0ULL));

    // Join menu formatting when Steam is online
    std::vector<LanHostEntry> emptyLan;
    std::vector<NativeMenuFriendInfo> friends;
    const auto noHostEntries = NativeMenu_PlanJoinEntries(false, emptyLan, true, friends, "127.0.0.1", true);
    bool foundNoHosts = false;
    for (const auto& e : noHostEntries) {
        TEST_CHECK(e.label.size() <= NATIVE_JOIN_MAX_LABEL_CHARS);
        if (e.kind == JOIN_ACT_INFO_NO_HOSTS) {
            foundNoHosts = true;
            TEST_CHECK(e.label == L"No LAN/Steam hosts");
        }
    }
    TEST_CHECK(foundNoHosts);

    const auto noHostRu = NativeMenu_PlanJoinEntries(false, emptyLan, true, friends, "127.0.0.1", false);
    bool foundNoHostsRu = false;
    for (const auto& e : noHostRu) {
        TEST_CHECK(e.label.size() <= NATIVE_JOIN_MAX_LABEL_CHARS);
        if (e.kind == JOIN_ACT_INFO_NO_HOSTS) {
            foundNoHostsRu = true;
            TEST_CHECK(e.label == L"Нет LAN/Steam хостов");
        }
    }
    TEST_CHECK(foundNoHostsRu);

    NativeMenuFriendInfo f = {};
    f.steamId = 76561198012345678ULL;
    f.inThisGame = true;
    f.isHosting = true;
    strncpy_s(f.name, sizeof(f.name), "WinLenComradeLongName", _TRUNCATE);
    friends.push_back(f);

    const auto withFriend = NativeMenu_PlanJoinEntries(false, emptyLan, true, friends, "127.0.0.1", true);
    bool foundFriend = false;
    for (const auto& e : withFriend) {
        TEST_CHECK(e.label.size() <= NATIVE_JOIN_MAX_LABEL_CHARS);
        if (e.kind == JOIN_ACT_STEAM_FRIEND) {
            foundFriend = true;
            TEST_CHECK(e.label.rfind(L"[Steam] ", 0) == 0);
            TEST_CHECK(strcmp(e.connectTarget, "steam:76561198012345678") == 0);
        }
    }
    TEST_CHECK(foundFriend);

    return true;
}

static bool SubTest_V171_SpeedMeterAndAckTransfer()
{
    // Part A: SaveTransferSpeedMeter unit tests
    SaveTransferSpeedMeter meter = {};
    SaveSync_SpeedMeterReset(&meter, 1000ULL);
    TEST_CHECK_NEAR(SaveSync_SpeedMeterUpdate(&meter, 1024ULL * 1024ULL, 1050ULL), 0.0f, 1e-4f); // < 150ms interval ignored

    // Steady 1 MB/s steps every 500ms across 6 seconds
    float lastSpeed = 0.0f;
    for (int i = 1; i <= 12; ++i) {
        const ULONGLONG t = 1000ULL + static_cast<ULONGLONG>(i) * 500ULL;
        const uint64_t bytes = static_cast<uint64_t>(i) * 524288ULL; // 0.5 MB per 0.5s == 1.0 MB/s
        lastSpeed = SaveSync_SpeedMeterUpdate(&meter, bytes, t);
    }
    TEST_CHECK(lastSpeed > 0.85f && lastSpeed < 1.15f);

    // Backwards doneBytes clamped monotonically without negative/NaN speed
    const float clamped = SaveSync_SpeedMeterUpdate(&meter, 100ULL, 8000ULL);
    TEST_CHECK(std::isfinite(clamped) && clamped >= 0.0f);

    // Part B: 0-byte ACK-paced transfer over 127.0.0.1
    std::filesystem::path tmpDir = std::filesystem::path(g_mockBaseDir) / "v171_ack_tests";
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const std::string zeroSrc = (tmpDir / "zero_src.bin").string();
    const std::string zeroDst = (tmpDir / "zero_dst.bin").string();
    {
        FILE* f = nullptr;
        fopen_s(&f, zeroSrc.c_str(), "wb");
        TEST_CHECK(f != nullptr);
        if (f) fclose(f);
    }
    {
        SOCKET srv = INVALID_SOCKET, cli = INVALID_SOCKET;
        TEST_CHECK(V171_MakeLoopbackSocketPair(&srv, &cli));
        bool recvOk = false;
        std::thread recvThread([&]() {
            recvOk = NetworkReceiveFileAckPaced(cli, zeroDst.c_str());
        });
        struct ProgSpy {
            int calls = 0;
            unsigned long long lastDone = 999;
        } spy;
        auto progCb = [](unsigned long long done, unsigned long long /*tot*/, void* ud) {
            ProgSpy* p = static_cast<ProgSpy*>(ud);
            ++p->calls;
            p->lastDone = done;
        };
        const bool sendOk = NetworkSendFileAckPaced(srv, zeroSrc.c_str(), progCb, &spy, nullptr, 65536u, 5000u);
        recvThread.join();
        closesocket(srv);
        closesocket(cli);
        TEST_CHECK(sendOk && recvOk);
        TEST_CHECK(spy.calls >= 1 && spy.lastDone == 0ULL);
        TEST_CHECK(std::filesystem::exists(zeroDst, ec) && std::filesystem::file_size(zeroDst, ec) == 0ULL);
    }

    // Part C: 1.5 MB transfer with small 128 KB window and slow receiver verifying ACK progress tracking
    const std::string bigSrc = (tmpDir / "big_src.bin").string();
    const std::string bigDst = (tmpDir / "big_dst.bin").string();
    constexpr size_t kBigBytes = 1536u * 1024u;
    std::vector<uint8_t> srcData(kBigBytes);
    for (size_t i = 0; i < kBigBytes; ++i) {
        srcData[i] = static_cast<uint8_t>((i * 131u + 17u) & 0xFFu);
    }
    {
        FILE* f = nullptr;
        fopen_s(&f, bigSrc.c_str(), "wb");
        TEST_CHECK(f != nullptr);
        if (f) {
            TEST_CHECK(fwrite(srcData.data(), 1, srcData.size(), f) == srcData.size());
            fclose(f);
        }
    }
    {
        SOCKET srv = INVALID_SOCKET, cli = INVALID_SOCKET;
        TEST_CHECK(V171_MakeLoopbackSocketPair(&srv, &cli));
        struct RecvState {
            std::atomic<unsigned long long> recvDone{0};
            bool ok{false};
        } rstate;
        struct SendState {
            std::mutex mtx;
            bool monotonic{true};
            unsigned long long lastReported{0};
            unsigned long long finalDone{0};
            bool withinConfirmLag{true};
            RecvState* rstate{nullptr};
        } sstate;
        sstate.rstate = &rstate;

        auto recvCb = [](unsigned long long done, unsigned long long /*tot*/, void* ud) {
            static_cast<RecvState*>(ud)->recvDone.store(done, std::memory_order_release);
        };
        auto sendCb = [](unsigned long long done, unsigned long long /*tot*/, void* ud) {
            SendState* st = static_cast<SendState*>(ud);
            std::lock_guard<std::mutex> lk(st->mtx);
            if (done < st->lastReported) st->monotonic = false;
            st->lastReported = done;
            st->finalDone = done;
            if (done > st->rstate->recvDone.load(std::memory_order_acquire)) {
                st->withinConfirmLag = false;
            }
        };

        std::thread recvThread([&]() {
            rstate.ok = NetworkReceiveFileAckPaced(cli, bigDst.c_str(), recvCb, &rstate);
        });
        const bool sendOk = NetworkSendFileAckPaced(srv, bigSrc.c_str(), sendCb, &sstate, nullptr, 131072u, 10000u);
        recvThread.join();
        closesocket(srv);
        closesocket(cli);

        TEST_CHECK(sendOk && rstate.ok);
        TEST_CHECK(sstate.monotonic && sstate.withinConfirmLag);
        TEST_CHECK(sstate.finalDone == kBigBytes);
        TEST_CHECK(std::filesystem::file_size(bigDst, ec) == kBigBytes);
        std::vector<uint8_t> dstData(kBigBytes, 0);
        FILE* df = nullptr;
        fopen_s(&df, bigDst.c_str(), "rb");
        TEST_CHECK(df != nullptr);
        if (df) {
            TEST_CHECK(fread(dstData.data(), 1, kBigBytes, df) == kBigBytes);
            fclose(df);
        }
        TEST_CHECK(dstData == srcData);
    }

    // Part D: Mid-transfer cancel flag removes partial file cleanly
    const std::string canDst = (tmpDir / "cancel_dst.bin").string();
    {
        SOCKET srv = INVALID_SOCKET, cli = INVALID_SOCKET;
        TEST_CHECK(V171_MakeLoopbackSocketPair(&srv, &cli));
        volatile bool cancelSend = false;
        volatile bool cancelRecv = false;
        struct CancelBox { volatile bool* flag; } cancelBox{ &cancelSend };
        bool recvOk = true;
        auto canSendCb = [](unsigned long long done, unsigned long long /*tot*/, void* ud) {
            if (done >= 262144ULL) *static_cast<CancelBox*>(ud)->flag = true;
        };
        std::thread recvThread([&]() {
            recvOk = NetworkReceiveFileAckPaced(cli, canDst.c_str(), nullptr, nullptr, &cancelRecv);
        });
        const bool sendOk = NetworkSendFileAckPaced(
            srv, bigSrc.c_str(), canSendCb, &cancelBox, &cancelSend, 131072u, 3000u);
        cancelRecv = true;
        shutdown(srv, SD_BOTH);
        shutdown(cli, SD_BOTH);
        recvThread.join();
        closesocket(srv);
        closesocket(cli);
        TEST_CHECK(!sendOk && !recvOk);
        TEST_CHECK(!std::filesystem::exists(canDst, ec));
    }

    std::filesystem::remove_all(tmpDir, ec);
    return true;
}

static bool Test_HotfixV171_All()
{
    TEST_CHECK(REPUBLICMP_PROTOCOL >= 64);

    printf("    [1/5] Crash A: Signal segment & 0x41E9F0 prologue/pointer guard ...\n"); fflush(stdout);
    if (!SubTest_V171_CrashASignalSegmentGuard()) return false;

    printf("    [2/5] Crash B: Filling worker UAF vehicle sale prune & queue clear ...\n"); fflush(stdout);
    if (!SubTest_V171_CrashBFillingWorkerAndSale()) return false;

    printf("    [3/5] Client Host-Lost modal, input block & ExitToMainMenu ...\n"); fflush(stdout);
    if (!SubTest_V171_ClientHostLostModalAndExit()) return false;

    printf("    [4/5] Steam Friends probe resolution & Join Menu planner ...\n"); fflush(stdout);
    if (!SubTest_V171_SteamFriendsAndJoinMenu()) return false;

    printf("    [5/5] SaveTransferSpeedMeter & 127.0.0.1 ACK-paced file transfer ...\n"); fflush(stdout);
    if (!SubTest_V171_SpeedMeterAndAckTransfer()) return false;

    return true;
}
