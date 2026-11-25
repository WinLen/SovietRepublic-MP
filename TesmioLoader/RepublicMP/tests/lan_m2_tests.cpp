// SPDX-License-Identifier: GPL-3.0-only
// Milestone M2 Unit, Socket Loopback & Integration Tests (Features F6-F8):
//   - F6: Steam Offline & BLoggedOn() Invariants, Dual-Testbed Detection Parity,
//         SteamP2P_ParseTargetSteamId Overflow/Edge Cases, ConsumePendingJoin Bounds
//   - F7: Steam-Optional ModSync (ModSync_IsSteamUgcOnline, 3-Tier Disk Fallback,
//         Non-Blocking Offline Download/WorldLoad Guards, Offline Loopback Handshake)
//   - F8: Steam-Free Session Lifecycle (LanDiscovery Init/Shutdown/SetHosting),
//         LocalHostOpen Event, Lockstep IP:port Normalization, Offline DiagLog Bundle

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/version.h"
#include "../session/mod_sync.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../transport/lan_discovery.h"
#include "../transport/network.h"
#include "../transport/steam_p2p.h"
#include "../ui/lang.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct M2LoopbackEnvGuard {
    std::vector<std::filesystem::path> tempDirs;
    std::filesystem::path settingsIniPath;
    bool hadSettingsIni = false;
    std::string prevSettingsIniBytes;
    int prevLang = MP_LANG_RU;

    M2LoopbackEnvGuard() : prevLang(Lang_Get())
    {
        LanDiscovery_SetLoopbackOnlyForTest(true);
        Network_SetLoopbackOnlyForTest(true);

        settingsIniPath = std::filesystem::path(Lang_GetConfigPath());
        std::error_code ec;
        if (std::filesystem::exists(settingsIniPath, ec)) {
            hadSettingsIni = true;
            FILE* f = nullptr;
            if (fopen_s(&f, settingsIniPath.string().c_str(), "rb") == 0 && f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    prevSettingsIniBytes.resize((size_t)sz);
                    fread(prevSettingsIniBytes.data(), 1, (size_t)sz, f);
                }
                fclose(f);
            }
        }
    }

    ~M2LoopbackEnvGuard()
    {
        if (SaveSync_GetState() != SESSION_IDLE) {
            SaveSync_RequestDisconnect();
            for (int i = 0; i < 60 && SaveSync_GetState() != SESSION_IDLE; ++i) {
                Sleep(50);
            }
        }
        LanDiscovery_StopScan();
        LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
        LanDiscovery_Shutdown();
        LanDiscovery_SetPortForTest(0);
        LanDiscovery_SetSelfPidOverrideForTest(0, false);
        SaveSync_SetTcpPortForTest(0);
        SteamP2P_SetPendingJoinForTest(nullptr);
        if (g_mockExeBuffer) {
            *(int64_t*)(g_mockExeBuffer + 0xA2E760) = 0;
            *(volatile char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
            memset(g_mockExeBuffer + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET, 0, SAVE_NAME_FIELD_MAX_LEN);
            *(BYTE**)(g_mockExeBuffer + WORKSHOP_MGR_PTR_RVA) = nullptr;
        }
        ModSync_EndSession();
        ModSync_TickMainThread();
        ModSync_CancelDownloadModal();
        TwoStates_ClearToastsForTest();
        Lang_Set(prevLang);

        std::error_code ec;
        if (hadSettingsIni) {
            FILE* f = nullptr;
            if (fopen_s(&f, settingsIniPath.string().c_str(), "wb") == 0 && f) {
                if (!prevSettingsIniBytes.empty()) {
                    fwrite(prevSettingsIniBytes.data(), 1, prevSettingsIniBytes.size(), f);
                }
                fclose(f);
            }
        } else {
            std::filesystem::remove(settingsIniPath, ec);
        }
        for (const auto& d : tempDirs) {
            std::filesystem::remove_all(d, ec);
        }
    }

    M2LoopbackEnvGuard(const M2LoopbackEnvGuard&) = delete;
    M2LoopbackEnvGuard& operator=(const M2LoopbackEnvGuard&) = delete;
};

static bool WriteTextFile(const std::filesystem::path& p, const char* content)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    FILE* f = nullptr;
    if (fopen_s(&f, p.string().c_str(), "wb") != 0 || !f) return false;
    size_t len = content ? strlen(content) : 0;
    if (len > 0) fwrite(content, 1, len, f);
    fclose(f);
    return true;
}

static bool WriteMockHeaderBinWithMod(const std::filesystem::path& saveDir, uint64_t modId1, uint64_t modId2)
{
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    std::vector<BYTE> buf(HEADER_BIN_DATA_OFFSET + 2 * HEADER_BIN_RECORD_SIZE, 0);
    auto writeRec = [&](size_t idx, uint64_t id) {
        if (id == 0) return;
        char* rec = (char*)(buf.data() + HEADER_BIN_DATA_OFFSET + idx * HEADER_BIN_RECORD_SIZE);
        _snprintf_s(rec, 100, _TRUNCATE, "media_soviet/workshop_wip/%llu/building.ini", (unsigned long long)id);
    };
    writeRec(0, modId1);
    writeRec(1, modId2);

    FILE* f = nullptr;
    std::filesystem::path hdrPath = saveDir / "header.bin";
    if (fopen_s(&f, hdrPath.string().c_str(), "wb") != 0 || !f) return false;
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return true;
}

static bool CreateLoopbackTcpPair(SOCKET* outServerAccepted, SOCKET* outClientConnected)
{
    *outServerAccepted = INVALID_SOCKET;
    *outClientConnected = INVALID_SOCKET;
    if (!NetworkInit()) return false;

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return false;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (const sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        closesocket(listener);
        return false;
    }

    sockaddr_in bound = {};
    int boundLen = sizeof(bound);
    if (getsockname(listener, (sockaddr*)&bound, &boundLen) != 0) {
        closesocket(listener);
        return false;
    }
    unsigned short port = ntohs(bound.sin_port);

    SOCKET cliSock = INVALID_SOCKET;
    std::thread cliThread([&]() {
        cliSock = NetworkConnect("127.0.0.1", port, 3000, 3000, nullptr);
    });

    sockaddr_in peerAddr = {};
    int peerLen = sizeof(peerAddr);
    SOCKET srvSock = accept(listener, (sockaddr*)&peerAddr, &peerLen);
    closesocket(listener);
    cliThread.join();

    if (srvSock == INVALID_SOCKET || cliSock == INVALID_SOCKET) {
        if (srvSock != INVALID_SOCKET) NetworkCloseSocket(srvSock);
        if (cliSock != INVALID_SOCKET) NetworkCloseSocket(cliSock);
        return false;
    }

    DWORD timeoutMs = 4000;
    setsockopt(srvSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    setsockopt(srvSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    *outServerAccepted = srvSock;
    *outClientConnected = cliSock;
    return true;
}

static bool TryBindLoopbackPortM2(int sockType, int proto, uint16_t port, bool reuseAddr)
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
    bool ok = (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    closesocket(s);
    return ok;
}

static uint16_t ReserveFreeLoopbackPortM2(int sockType, int proto, uint16_t baseOffset)
{
    (void)NetworkInit();
    static std::atomic<uint32_t> s_seq{0};
    const uint32_t bias = ((GetCurrentProcessId() % 200u) * 20u) + (s_seq.fetch_add(1u) * 17u) + baseOffset;
    const uint16_t span = 7000u; // 42000..48999
    for (uint16_t offset = 0; offset < span; ++offset) {
        uint16_t candidate = (uint16_t)(42000u + ((bias + offset) % span));
        if (TryBindLoopbackPortM2(sockType, proto, candidate, false) &&
            TryBindLoopbackPortM2(sockType, proto, candidate, true)) {
            return candidate;
        }
    }
    return 0;
}

static uint16_t ReserveFreeLoopbackUdpPortM2()
{
    return ReserveFreeLoopbackPortM2(SOCK_DGRAM, IPPROTO_UDP, 0u);
}

static uint16_t ReserveFreeLoopbackTcpPortM2()
{
    return ReserveFreeLoopbackPortM2(SOCK_STREAM, IPPROTO_TCP, 1500u);
}

static bool SubTest_M2_F6_SteamOfflineAndParseHardening()
{
    // 1. Steam offline invariants (steam_api64.dll absent in unit runner)
    TEST_CHECK(!SteamP2P_IsLoggedOn());
    TEST_CHECK(!SteamP2P_IsAvailable());
    TEST_CHECK(!SteamP2P_Init());
    TEST_CHECK(!SteamP2P_IsAvailable());
    TEST_CHECK(!SteamP2P_IsAvailable() || SteamP2P_IsLoggedOn());
    TEST_CHECK(SteamP2P_GetLocalSteamId() == 0);
    const char* persona = SteamP2P_GetLocalPersonaName();
    TEST_CHECK(persona != nullptr && persona[0] == '\0');
    TEST_CHECK(SteamP2P_GetLocalPersonaNameCopy().empty());

    // Safe no-ops when Steam is offline
    std::vector<SteamFriendEntry> friends(4);
    SteamP2P_GetOnlineFriends(friends);
    TEST_CHECK(friends.empty());
    SteamP2P_SetHostingPresence(true);
    SteamP2P_SetHostingPresence(false);
    TEST_CHECK(!SteamP2P_InviteFriend(76561198000000001ULL));
    SteamP2P_FrameTick();
    SteamP2P_PrepareAccept(5556);
    TEST_CHECK(SteamP2P_TryAcceptOne(5556, 100) == INVALID_SOCKET);
    TEST_CHECK(SteamP2P_Connect(76561198000000001ULL, 5556, 50, 50, nullptr) == INVALID_SOCKET);
    SteamP2P_UnregisterCallbacks();
    SteamP2P_UnregisterCallbacks();

    char peerBuf[64] = "dirty";
    SteamP2P_GetPeerName(INVALID_SOCKET, peerBuf, sizeof(peerBuf));
    TEST_CHECK(peerBuf[0] == '\0');
    SteamP2P_GetPeerName(static_cast<SOCKET>(0x53544D0000000000ULL), peerBuf, sizeof(peerBuf));
    TEST_CHECK(peerBuf[0] == '\0');
    SteamP2P_GetPeerName(static_cast<SOCKET>(0x53544D0000000063ULL), peerBuf, sizeof(peerBuf));
    TEST_CHECK(peerBuf[0] == '\0');
    SteamP2P_GetPeerName(INVALID_SOCKET, nullptr, 0);

    // ConsumePendingJoin bounds safety & small-buffer preservation
    char joinBuf[16] = "dirty";
    TEST_CHECK(!SteamP2P_ConsumePendingJoin(nullptr, 0));
    TEST_CHECK(!SteamP2P_ConsumePendingJoin(joinBuf, 0));
    TEST_CHECK(strcmp(joinBuf, "dirty") == 0);
    TEST_CHECK(!SteamP2P_ConsumePendingJoin(joinBuf, sizeof(joinBuf)));
    TEST_CHECK(joinBuf[0] == '\0');

    SteamP2P_SetPendingJoinForTest("steam:76561198012345678");
    char tinyJoin[10] = "dirty";
    TEST_CHECK(!SteamP2P_ConsumePendingJoin(tinyJoin, sizeof(tinyJoin)));
    TEST_CHECK(tinyJoin[0] == '\0');
    char fullJoin[64] = {};
    TEST_CHECK(SteamP2P_ConsumePendingJoin(fullJoin, sizeof(fullJoin)));
    TEST_CHECK(strcmp(fullJoin, "steam:76561198012345678") == 0);
    TEST_CHECK(!SteamP2P_ConsumePendingJoin(fullJoin, sizeof(fullJoin)));

    // 2. SteamP2P_ParseTargetSteamId boundary, overflow, and whitespace hardening
    TEST_CHECK(SteamP2P_ParseTargetSteamId(nullptr) == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("steam:") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("steam:abc") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("12345") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("192.168.1.1") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("127.0.0.1:5556") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("76561197960265727") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("76561197960265728") == 76561197960265728ULL);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("  \tSTEAM:76561198012345678\r\n ") == 76561198012345678ULL);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("steam:18446744073709551615") == UINT64_MAX);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("steam:18446744073709551616") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("99999999999999999999999999") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("steam:000076561198000000000") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("+76561198000000000") == 0);
    TEST_CHECK(SteamP2P_ParseTargetSteamId("-76561198000000000") == 0);

    // 3. SaveSync_DetectDualTestbedInputs parity & negative traps
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs(nullptr, nullptr, nullptr));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs("", "-windowed", "C:\\Steam\\steamapps\\common\\SovietRepublic\\SOVIET64.exe"));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs("other", nullptr, nullptr));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs("0", nullptr, nullptr));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs("false", nullptr, nullptr));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs(nullptr, "SOVIET64.exe --mp-hostile", nullptr));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs(nullptr, "SOVIET64.exe --mp-client-foo", nullptr));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs(nullptr, "SOVIET64.exe foo--mp-host", nullptr));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs(nullptr, nullptr, "C:\\Users\\contest-host\\SovietRepublic\\SOVIET64.exe"));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs(nullptr, nullptr, "C:\\Games\\protest-client\\SOVIET64.exe"));
    TEST_CHECK(!SaveSync_DetectDualTestbedInputs(nullptr, nullptr, "C:\\test-host-v2\\SOVIET64.exe"));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs("HOST", nullptr, nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs("host", nullptr, nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs("CLIENT", nullptr, nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs("client", nullptr, nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, "--mp-host", nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, "SOVIET64.exe --mp-host", nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, "SOVIET64.exe --mp-client=1", nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, "\"--mp-left\"", nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, "SOVIET64.exe --mp-right", nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, "SOVIET64.exe --MP-HOST", nullptr));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, nullptr, "C:\\test-host\\SOVIET64.exe"));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, nullptr, "D:\\SovietRepublic MP\\local\\test-host\\SOVIET64.exe"));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, nullptr, "D:\\SovietRepublic MP\\local\\TEST-CLIENT\\SOVIET64.exe"));
    TEST_CHECK(SaveSync_DetectDualTestbedInputs(nullptr, nullptr, "D:/local/test-client/SOVIET64.exe"));
    if (SaveSync_IsDualTestbed()) {
        TEST_CHECK(!SteamP2P_IsAvailable());
        TEST_CHECK(!SteamP2P_IsLoggedOn());
    }
    return true;
}

static bool SubTest_M2_F7_DiskFallbackAndCatalog(M2LoopbackEnvGuard& guard)
{
    TEST_CHECK(!ModSync_IsSteamUgcOnline());

    std::filesystem::path fakeRoot = std::filesystem::path(g_mockBaseDir) / "f7_disk_test";
    guard.tempDirs.push_back(fakeRoot);
    std::filesystem::path steamApps = fakeRoot / "steamapps";
    std::filesystem::path exeDir = steamApps / "common" / "SovietRepublic";
    std::filesystem::path dir784150 = steamApps / "workshop" / "content" / "784150";

    const uint64_t kModWipOnly = 990001ULL;
    const uint64_t kModLocalWs = 1200000001ULL;
    const uint64_t kModSteam784150 = 3823543010ULL;
    const uint64_t kModPriority = 1300000003ULL;
    const uint64_t kModDirIni = 1400000004ULL;

    TEST_CHECK(WriteTextFile(exeDir / "media_soviet" / "workshop_wip" / "990001" / "workshopconfig.ini",
                             "$ITEM_TYPE WORKSHOP_ITEMTYPE_BUILDING\n$ITEM_NAME \"WIP Mod\"\n"));
    TEST_CHECK(WriteTextFile(exeDir / "media_soviet" / "workshop" / "1200000001" / "workshopconfig.ini",
                             "$ITEM_TYPE WORKSHOP_ITEMTYPE_VEHICLE\n$ITEM_NAME \"Local WS Mod\"\n"));
    TEST_CHECK(WriteTextFile(dir784150 / "3823543010" / "workshopconfig.ini",
                             "$ITEM_TYPE WORKSHOP_ITEMTYPE_SCRIPT\n$ITEM_NAME \"784150 Mod\"\n"));

    // Priority test: same ID in all 3 locations -> workshop_wip wins over workshop > 784150
    TEST_CHECK(WriteTextFile(exeDir / "media_soviet" / "workshop_wip" / "1300000003" / "workshopconfig.ini",
                             "$ITEM_NAME \"Prio WIP\"\n"));
    TEST_CHECK(WriteTextFile(exeDir / "media_soviet" / "workshop" / "1300000003" / "workshopconfig.ini",
                             "$ITEM_NAME \"Prio WS\"\n"));
    TEST_CHECK(WriteTextFile(dir784150 / "1300000003" / "workshopconfig.ini",
                             "$ITEM_NAME \"Prio 784150\"\n"));

    // Directory named workshopconfig.ini must be rejected
    std::error_code ec;
    std::filesystem::create_directories(exeDir / "media_soviet" / "workshop" / "1400000004" / "workshopconfig.ini", ec);

    std::string exeStr = exeDir.string();
    std::string exeTrailingSlash = exeStr + "\\";
    std::string exeTrailingFwd = exeStr + "/";
    char found[MAX_PATH] = {};

    TEST_CHECK(ModSync_FindLocalModFolderAtRoot(kModWipOnly, exeStr.c_str(), false, found, sizeof(found)));
    TEST_CHECK(strstr(found, "workshop_wip\\990001") != nullptr);

    TEST_CHECK(ModSync_FindLocalModFolderAtRoot(kModLocalWs, exeTrailingSlash.c_str(), false, found, sizeof(found)));
    TEST_CHECK(strstr(found, "media_soviet\\workshop\\1200000001") != nullptr);

    TEST_CHECK(ModSync_FindLocalModFolderAtRoot(kModSteam784150, exeTrailingFwd.c_str(), true, found, sizeof(found)));
    TEST_CHECK(strstr(found, "784150\\3823543010") != nullptr);

    TEST_CHECK(ModSync_FindLocalModFolderAtRoot(kModPriority, exeStr.c_str(), false, found, sizeof(found)));
    TEST_CHECK(strstr(found, "workshop_wip\\1300000003") != nullptr);

    // Negative & boundary cases
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(kModDirIni, exeStr.c_str(), false, found, sizeof(found)));
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(0, exeStr.c_str(), false, found, sizeof(found)));
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(kModWipOnly, nullptr, false, found, sizeof(found)));
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(kModWipOnly, "", false, found, sizeof(found)));
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(kModWipOnly, exeStr.c_str(), false, nullptr, 0));
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(kModWipOnly, exeStr.c_str(), false, found, 0));

    // Exact buffer too small (no silent truncation)
    TEST_CHECK(ModSync_FindLocalModFolderAtRoot(kModWipOnly, exeStr.c_str(), false, found, sizeof(found)));
    size_t exactLen = strlen(found);
    TEST_CHECK(exactLen > 8);
    char exactBuf[MAX_PATH] = {};
    TEST_CHECK(!ModSync_FindLocalModFolderAtRoot(kModWipOnly, exeStr.c_str(), false, exactBuf, exactLen));
    TEST_CHECK(exactBuf[0] == '\0');
    TEST_CHECK(ModSync_FindLocalModFolderAtRoot(kModWipOnly, exeStr.c_str(), false, exactBuf, exactLen + 1));

    // ModSync_FindLocalModFolder resolves via mock g_baseDir & ignores non-numeric folder suffixes
    std::filesystem::path mockBaseWip = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "workshop_wip" / "1500000005";
    guard.tempDirs.push_back(std::filesystem::path(g_mockBaseDir) / "media_soviet");
    TEST_CHECK(WriteTextFile(mockBaseWip / "workshopconfig.ini", "$ITEM_NAME \"Mock Base Mod\"\n"));
    TEST_CHECK(ModSync_FindLocalModFolder(1500000005ULL, found, sizeof(found)));
    ModSync_RefreshCatalog();
    int baseCatalogCount = ModSync_GetModCount();
    TEST_CHECK(baseCatalogCount >= 1);

    std::filesystem::path backupDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "workshop" / "12345_backup";
    TEST_CHECK(WriteTextFile(backupDir / "workshopconfig.ini", "$ITEM_NAME \"Backup NonNumeric\"\n"));
    ModSync_RefreshCatalog();
    TEST_CHECK(ModSync_GetModCount() == baseCatalogCount);

    std::filesystem::path validWsDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "workshop" / "1500000099";
    TEST_CHECK(WriteTextFile(validWsDir / "workshopconfig.ini", "$ITEM_NAME \"Valid Numeric\"\n"));
    ModSync_RefreshCatalog();
    TEST_CHECK(ModSync_GetModCount() == baseCatalogCount + 1);
    return true;
}

static bool VerifyRejectedPluginListOverLoopback(const char* const* pluginIds, size_t pluginCount, const std::filesystem::path& sessionIniDir)
{
    SOCKET hostSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;
    TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));

    ModSyncClientResult cliRes = MOD_SYNC_CLIENT_OK;
    char cliErr[256] = {};
    bool sentEvil = false;
    std::thread evilHost([&]() {
#pragma pack(push, 1)
        struct RawHdr {
            char magic[8];
            uint32_t packMode;
            uint32_t modCount;
            uint32_t pluginCount;
            char csv[256];
        } hdr = {};
        struct RawPlug {
            char id[64];
            uint32_t byteSize;
        } plug = {};
#pragma pack(pop)
        memcpy(hdr.magic, "RPMPMOD3", 8);
        hdr.pluginCount = (uint32_t)pluginCount;
        sentEvil = NetworkSendExact(hostSock, &hdr, sizeof(hdr));
        for (size_t i = 0; sentEvil && i < pluginCount; ++i) {
            memset(&plug, 0, sizeof(plug));
            strncpy_s(plug.id, sizeof(plug.id), pluginIds[i], _TRUNCATE);
            plug.byteSize = 7;
            sentEvil = NetworkSendExact(hostSock, &plug, sizeof(plug)) &&
                       NetworkSendExact(hostSock, "pwned=1", 7);
        }
    });
    std::thread evilCli([&]() {
        cliRes = ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
    });
    evilHost.join();
    evilCli.join();
    NetworkCloseSocket(hostSock);
    NetworkCloseSocket(clientSock);
    TEST_CHECK(sentEvil);
    TEST_CHECK(cliRes == MOD_SYNC_CLIENT_ERROR);
    TEST_CHECK(strstr(cliErr, "invalid plugin") != nullptr);
    std::error_code ec;
    TEST_CHECK(!std::filesystem::exists(sessionIniDir, ec) || std::filesystem::is_empty(sessionIniDir, ec));
    return true;
}

static bool SubTest_M2_F7_OfflineGuardsAndLoopbackHandshake(M2LoopbackEnvGuard& guard)
{
    std::filesystem::path mockRoot(g_mockBaseDir);
    std::filesystem::path presentModDir = mockRoot / "media_soviet" / "workshop_wip" / "1600000001";
    std::filesystem::path saveDir = mockRoot / "media_soviet" / "save_coop" / "m2_test_save";
    guard.tempDirs.push_back(mockRoot / "media_soviet");

    const uint64_t kPresentModId = 1600000001ULL;
    const uint64_t kMissingModId = 1600000002ULL;
    TEST_CHECK(WriteTextFile(presentModDir / "workshopconfig.ini", "$ITEM_TYPE WORKSHOP_ITEMTYPE_BUILDING\n$ITEM_NAME \"Present Mod\"\n"));
    TEST_CHECK(WriteMockHeaderBinWithMod(saveDir, kPresentModId, kMissingModId));

    // 1. Offline ModSync_BeforeWorldLoad must never hang download modal
    ModSync_BeforeWorldLoad(true, saveDir.string().c_str());
    TEST_CHECK(!ModSync_IsDownloadActive());
    TEST_CHECK(ModSync_GetLastOfflineWarning()[0] != '\0');
    ModSync_TickDownloadProgress();
    TEST_CHECK(!ModSync_IsDownloadActive());
    ModSync_EndSession();
    TEST_CHECK(ModSync_GetLastOfflineWarning()[0] == '\0');

    // 2. Offline loopback handshake with missing mod -> proceeds non-blocking (OK + warning + filtered)
    SOCKET hostSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;
    TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));

    ModSyncHostResult hostRes = MOD_SYNC_HOST_ERROR;
    ModSyncClientResult cliRes = MOD_SYNC_CLIENT_ERROR;
    char hostErr[256] = {};
    char cliErr[256] = {};

    std::thread hostThread([&]() {
        hostRes = ModSync_HostHandshake(hostSock, saveDir.string().c_str(), hostErr, sizeof(hostErr));
    });
    std::thread clientThread([&]() {
        cliRes = ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
    });
    hostThread.join();
    clientThread.join();

    NetworkCloseSocket(hostSock);
    NetworkCloseSocket(clientSock);

    TEST_CHECK(hostRes == MOD_SYNC_HOST_OK);
    TEST_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
    TEST_CHECK(!ModSync_IsDownloadActive());
    TEST_CHECK(ModSync_GetLastOfflineWarning()[0] != '\0');
    ModSync_EndSession();

    // 2b. Verify ClientHandshake offline filter & WireModSyncResponse directly against a mock Host manifest
    //     with SOVIET_WORKSHOP_MANAGER populated (avoids hostThread overwriting process-global filter).
    auto makeMockItem = [](uint64_t id, int itemType) -> std::vector<BYTE> {
        std::vector<BYTE> item(0x4900, 0);
        *(WORD*)(item.data() + ITEM_IS_SUBSCRIBED_OFF) = 1;
        *(int*)(item.data() + ITEM_TYPE_OFF) = itemType;
        *(uint64_t*)(item.data() + ITEM_ID_QWORD_OFF) = id;
        _snprintf_s((char*)(item.data() + ITEM_ID_STR_OFF), 0x400, _TRUNCATE, "%llu", (unsigned long long)id);
        _snprintf_s((char*)(item.data() + ITEM_NAME_OFF), 0x400, _TRUNCATE, "M2_%llu", (unsigned long long)id);
        return item;
    };
    auto itemPresent = makeMockItem(kPresentModId, 1);
    auto itemMissing = makeMockItem(kMissingModId, 1);
    auto itemLocText = makeMockItem(4000000004ULL, 4);
    std::vector<BYTE*> mgrPtrs = { itemPresent.data(), itemMissing.data(), itemLocText.data() };
    BYTE mgrHdr[24] = {};
    WriteMockVectorHeader(mgrHdr, mgrPtrs);
    *(BYTE**)(g_mockExeBuffer + WORKSHOP_MGR_PTR_RVA) = mgrHdr;

    TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
    uint32_t respStatus = 99;
    uint32_t respMissing = 0;
    char respDetails[64] = {};
    bool mockHostOk = false;
    std::thread mockHost([&]() {
#pragma pack(push, 1)
        struct RawHdr {
            char magic[8];
            uint32_t packMode;
            uint32_t modCount;
            uint32_t pluginCount;
            char csv[256];
        } hdr = {};
        struct RawMod {
            uint64_t id;
            uint8_t isWip;
            uint8_t itemType;
            char title[64];
        } mods[2] = {};
        struct RawResp {
            char magic[8];
            uint32_t status;
            uint32_t missingCount;
            char details[64];
        } resp = {};
#pragma pack(pop)
        memcpy(hdr.magic, "RPMPMOD3", 8);
        hdr.modCount = 2;
        mods[0].id = kPresentModId;
        mods[0].isWip = 1;
        mods[0].itemType = 1;
        mods[1].id = kMissingModId;
        mods[1].isWip = 0;
        mods[1].itemType = 1;
        if (NetworkSendExact(hostSock, &hdr, sizeof(hdr)) &&
            NetworkSendExact(hostSock, mods, sizeof(mods)) &&
            NetworkRecvExact(hostSock, &resp, sizeof(resp)) &&
            memcmp(resp.magic, "RPMPRESP", 8) == 0) {
            respStatus = resp.status;
            respMissing = resp.missingCount;
            memcpy(respDetails, resp.details, sizeof(respDetails));
            mockHostOk = true;
        }
    });
    cliRes = MOD_SYNC_CLIENT_ERROR;
    std::thread realCli([&]() {
        cliRes = ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
    });
    mockHost.join();
    realCli.join();
    NetworkCloseSocket(hostSock);
    NetworkCloseSocket(clientSock);

    TEST_CHECK(mockHostOk);
    TEST_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
    TEST_CHECK(respStatus == 0 /* MOD_SYNC_STATUS_OK */);
    TEST_CHECK(respMissing == 1);
    TEST_CHECK(strcmp(respDetails, "offline-skip") == 0);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(kPresentModId));
    TEST_CHECK(!ModSync_IsModIdAllowedInSession(kMissingModId));
    TEST_CHECK(!ModSync_IsModIdAllowedInSession(9999999998ULL));
    BYTE** mgrBegin = *(BYTE***)(mgrHdr + 0x00);
    BYTE** mgrEnd = *(BYTE***)(mgrHdr + 0x08);
    // Worker thread MUST NOT mutate SOVIET_WORKSHOP_MANAGER directly; deferred until main-thread tick!
    TEST_CHECK((mgrEnd - mgrBegin) == 3);
    ModSync_TickMainThread();
    mgrEnd = *(BYTE***)(mgrHdr + 0x08);
    TEST_CHECK((mgrEnd - mgrBegin) == 2); // kPresentModId + itemType==4 text localization
    ModSync_EndSession();
    mgrEnd = *(BYTE***)(mgrHdr + 0x08);
    TEST_CHECK((mgrEnd - mgrBegin) == 2); // still hidden until main-thread restore
    ModSync_TickMainThread();
    mgrEnd = *(BYTE***)(mgrHdr + 0x08);
    TEST_CHECK((mgrEnd - mgrBegin) == 3); // restored on main-thread tick

    // Also verify ModSync_BeforeWorldLoad(false, nullptr) restores hidden items if EndSession ran on worker.
    TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
    std::thread mockHost2([&]() {
        uint8_t hdrBuf[8 + 12 + 256] = {};
        memcpy(hdrBuf, "RPMPMOD3", 8);
        uint32_t oneMod = 1;
        memcpy(hdrBuf + 12, &oneMod, 4);
#pragma pack(push, 1)
        struct RawMod { uint64_t id; uint8_t isWip; uint8_t itemType; char title[64]; } m = {};
        struct RawResp { char magic[8]; uint32_t status; uint32_t missingCount; char details[64]; } r = {};
#pragma pack(pop)
        m.id = kPresentModId; m.isWip = 1; m.itemType = 1;
        NetworkSendExact(hostSock, hdrBuf, sizeof(hdrBuf));
        NetworkSendExact(hostSock, &m, sizeof(m));
        NetworkRecvExact(hostSock, &r, sizeof(r));
    });
    std::thread realCli2([&]() {
        (void)ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
    });
    mockHost2.join();
    realCli2.join();
    NetworkCloseSocket(hostSock);
    NetworkCloseSocket(clientSock);
    ModSync_TickMainThread();
    TEST_CHECK((*(BYTE***)(mgrHdr + 0x08) - *(BYTE***)(mgrHdr + 0x00)) == 2);
    ModSync_EndSession();
    ModSync_BeforeWorldLoad(false, nullptr);
    TEST_CHECK((*(BYTE***)(mgrHdr + 0x08) - *(BYTE***)(mgrHdr + 0x00)) == 3);
    *(BYTE**)(g_mockExeBuffer + WORKSHOP_MGR_PTR_RVA) = nullptr;

    // 2c. ModSync_HostHandshake receiving MOD_SYNC_STATUS_OK with missingCount > 0 ("offline-skip")
    //     must warn Host via ModSync_GetLastOfflineWarning() & warning toast (colorKind == 2).
    {
        TwoStates_ClearToastsForTest();
        TEST_CHECK(WriteMockHeaderBinWithMod(saveDir, kPresentModId, 0));
        TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
        hostRes = MOD_SYNC_HOST_ERROR;
        std::thread hostWarnThread([&]() {
            hostRes = ModSync_HostHandshake(hostSock, saveDir.string().c_str(), hostErr, sizeof(hostErr));
        });
        std::thread mockCliWarn([&]() {
#pragma pack(push, 1)
            struct RawHdr { char magic[8]; uint32_t packMode; uint32_t modCount; uint32_t pluginCount; char csv[256]; } hdr = {};
            struct RawMod { uint64_t id; uint8_t isWip; uint8_t itemType; char title[64]; };
            struct RawResp { char magic[8]; uint32_t status; uint32_t missingCount; char details[64]; } resp = {};
#pragma pack(pop)
            if (NetworkRecvExact(clientSock, &hdr, sizeof(hdr))) {
                if (hdr.modCount > 0) {
                    std::vector<RawMod> ms(hdr.modCount);
                    NetworkRecvExact(clientSock, ms.data(), ms.size() * sizeof(RawMod));
                }
                memcpy(resp.magic, "RPMPRESP", 8);
                resp.status = 0; // MOD_SYNC_STATUS_OK
                resp.missingCount = 2;
                strncpy_s(resp.details, sizeof(resp.details), "offline-skip", _TRUNCATE);
                NetworkSendExact(clientSock, &resp, sizeof(resp));
            }
        });
        hostWarnThread.join();
        mockCliWarn.join();
        NetworkCloseSocket(hostSock);
        NetworkCloseSocket(clientSock);
        TEST_CHECK(hostRes == MOD_SYNC_HOST_OK);
        TEST_CHECK(ModSync_GetLastOfflineWarning()[0] != '\0');
        TEST_CHECK(TwoStates_GetToastCountForTest() >= 1);
        char toastTxt[128] = {};
        int toastKind = -1;
        TEST_CHECK(TwoStates_GetToastForTest(TwoStates_GetToastCountForTest() - 1, toastTxt, sizeof(toastTxt), &toastKind, nullptr));
        TEST_CHECK(toastKind == 2);
        TwoStates_ClearToastsForTest();
        ModSync_EndSession();
        ModSync_TickMainThread();
    }

    // 2d. Vanilla save (existing header.bin with 0 mods) in MOD_PACK_SAVE_ONLY must NOT reuse g_lastSaveUsedIds.
    {
        std::filesystem::path vanillaDir = mockRoot / "vanilla_save";
        guard.tempDirs.push_back(vanillaDir);
        TEST_CHECK(WriteMockHeaderBinWithMod(vanillaDir, 0, 0));
        ModPackMode prevPack = ModSync_GetPackMode();
        ModSync_SetPackMode(MOD_PACK_SAVE_ONLY);

        TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
        uint32_t seenModCount = 999;
        hostRes = MOD_SYNC_HOST_ERROR;
        std::thread hostVan([&]() {
            hostRes = ModSync_HostHandshake(hostSock, vanillaDir.string().c_str(), hostErr, sizeof(hostErr));
        });
        std::thread cliVan([&]() {
#pragma pack(push, 1)
            struct RawHdr { char magic[8]; uint32_t packMode; uint32_t modCount; uint32_t pluginCount; char csv[256]; } hdr = {};
            struct RawMod { uint64_t id; uint8_t isWip; uint8_t itemType; char title[64]; };
            struct RawResp { char magic[8]; uint32_t status; uint32_t missingCount; char details[64]; } resp = {};
#pragma pack(pop)
            if (NetworkRecvExact(clientSock, &hdr, sizeof(hdr))) {
                seenModCount = hdr.modCount;
                if (hdr.modCount > 0) {
                    std::vector<RawMod> ms(hdr.modCount);
                    NetworkRecvExact(clientSock, ms.data(), ms.size() * sizeof(RawMod));
                }
                memcpy(resp.magic, "RPMPRESP", 8);
                resp.status = 0;
                NetworkSendExact(clientSock, &resp, sizeof(resp));
            }
        });
        hostVan.join();
        cliVan.join();
        NetworkCloseSocket(hostSock);
        NetworkCloseSocket(clientSock);
        ModSync_SetPackMode(prevPack);
        ModSync_EndSession();
        ModSync_TickMainThread();
        TEST_CHECK(hostRes == MOD_SYNC_HOST_OK);
        TEST_CHECK(seenModCount == 0);
    }

    // 3. Offline loopback handshake when all save mods are present on disk
    TEST_CHECK(WriteMockHeaderBinWithMod(saveDir, kPresentModId, 0));
    TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
    hostRes = MOD_SYNC_HOST_ERROR;
    cliRes = MOD_SYNC_CLIENT_ERROR;
    std::thread hostOkThread([&]() {
        hostRes = ModSync_HostHandshake(hostSock, saveDir.string().c_str(), hostErr, sizeof(hostErr));
    });
    std::thread cliOkThread([&]() {
        cliRes = ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
    });
    hostOkThread.join();
    cliOkThread.join();
    NetworkCloseSocket(hostSock);
    NetworkCloseSocket(clientSock);
    TEST_CHECK(hostRes == MOD_SYNC_HOST_OK);
    TEST_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
    TEST_CHECK(ModSync_GetLastOfflineWarning()[0] == '\0');
    ModSync_EndSession();

    // 4. Reject path-traversal, drive-letter, ADS, dot, and reserved device plugin IDs over loopback TCP
    std::filesystem::path sessionIniDir = mockRoot / "plugins" / ".mp_session_ini";
    guard.tempDirs.push_back(mockRoot / "plugins");
    static const char* const kBadPluginIds[] = {
        "../evil_plugin",
        "C:x",
        "D:x",
        "C:evil",
        "foo:bar",
        ".",
        "CON",
        "con.txt",
        "COM1",
        "a..b",
        ".hidden",
        "trailing."
    };
    for (const char* badId : kBadPluginIds) {
        if (!VerifyRejectedPluginListOverLoopback(&badId, 1, sessionIniDir)) return false;
    }
    static const char* const kMultiPluginBad[] = { "valid_plugin", "C:evil" };
    if (!VerifyRejectedPluginListOverLoopback(kMultiPluginBad, 2, sessionIniDir)) return false;
    std::error_code cwdEc;
    TEST_CHECK(!std::filesystem::exists(sessionIniDir / "valid_plugin.ini", cwdEc));
    TEST_CHECK(!std::filesystem::exists(std::filesystem::current_path(cwdEc) / "x.ini", cwdEc));
    TEST_CHECK(!std::filesystem::exists(std::filesystem::current_path(cwdEc) / "evil.ini", cwdEc));

    // 5. Reject pluginCount > 64, invalid enabledPluginsCsv tokens, and verify valid .ini sync roundtrip
    {
        // pluginCount > 64 rejection
        TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
        std::thread hostTooMany([&]() {
#pragma pack(push, 1)
            struct RawHdr { char magic[8]; uint32_t packMode; uint32_t modCount; uint32_t pluginCount; char csv[256]; } hdr = {};
#pragma pack(pop)
            memcpy(hdr.magic, "RPMPMOD3", 8);
            hdr.pluginCount = 65;
            NetworkSendExact(hostSock, &hdr, sizeof(hdr));
        });
        cliRes = ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
        hostTooMany.join();
        NetworkCloseSocket(hostSock);
        NetworkCloseSocket(clientSock);
        TEST_CHECK(cliRes == MOD_SYNC_CLIENT_ERROR);

        // Invalid enabledPluginsCsv rejection ("valid_plugin,../evil" and "valid_plugin,CON")
        static const char* const kBadCsvs[] = { "valid_plugin,../evil", "valid_plugin,CON", "valid_plugin," };
        for (const char* badCsv : kBadCsvs) {
            TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
            std::thread hostBadCsv([&]() {
#pragma pack(push, 1)
                struct RawHdr { char magic[8]; uint32_t packMode; uint32_t modCount; uint32_t pluginCount; char csv[256]; } hdr = {};
#pragma pack(pop)
                memcpy(hdr.magic, "RPMPMOD3", 8);
                strncpy_s(hdr.csv, sizeof(hdr.csv), badCsv, _TRUNCATE);
                NetworkSendExact(hostSock, &hdr, sizeof(hdr));
            });
            cliRes = ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
            hostBadCsv.join();
            NetworkCloseSocket(hostSock);
            NetworkCloseSocket(clientSock);
            TEST_CHECK(cliRes == MOD_SYNC_CLIENT_ERROR);
        }

        // Valid plugin .ini sync roundtrip into <g_baseDir>/plugins/.mp_session_ini
        TEST_CHECK(CreateLoopbackTcpPair(&hostSock, &clientSock));
        std::thread hostValidIni([&]() {
#pragma pack(push, 1)
            struct RawHdr { char magic[8]; uint32_t packMode; uint32_t modCount; uint32_t pluginCount; char csv[256]; } hdr = {};
            struct RawIniHdr { char id[64]; uint32_t byteSize; } ih = {};
            struct RawResp { char magic[8]; uint32_t status; uint32_t missingCount; char details[64]; } resp = {};
#pragma pack(pop)
            const char kIniBody[] = "[general]\nenabled=1\n";
            memcpy(hdr.magic, "RPMPMOD3", 8);
            hdr.pluginCount = 1;
            strncpy_s(hdr.csv, sizeof(hdr.csv), "good_plugin", _TRUNCATE);
            strncpy_s(ih.id, sizeof(ih.id), "good_plugin", _TRUNCATE);
            ih.byteSize = (uint32_t)(sizeof(kIniBody) - 1);
            if (NetworkSendExact(hostSock, &hdr, sizeof(hdr)) &&
                NetworkSendExact(hostSock, &ih, sizeof(ih)) &&
                NetworkSendExact(hostSock, kIniBody, ih.byteSize)) {
                NetworkRecvExact(hostSock, &resp, sizeof(resp));
            }
        });
        cliRes = ModSync_ClientHandshake(clientSock, cliErr, sizeof(cliErr));
        hostValidIni.join();
        NetworkCloseSocket(hostSock);
        NetworkCloseSocket(clientSock);
        TEST_CHECK(cliRes == MOD_SYNC_CLIENT_OK);
        TEST_CHECK(std::filesystem::is_regular_file(sessionIniDir / "good_plugin.ini", cwdEc));
        ModSync_EndSession();
        ModSync_TickMainThread();
    }
    return true;
}

static bool SubTest_M2_F8_SessionLifecycleAndIdentity(M2LoopbackEnvGuard& guard)
{
    const uint16_t udpPort = ReserveFreeLoopbackUdpPortM2();
    const uint16_t tcpPort = ReserveFreeLoopbackTcpPortM2();
    TEST_CHECK(udpPort != 0 && tcpPort != 0);
    LanDiscovery_SetPortForTest(udpPort);
    LanDiscovery_SetSelfPidOverrideForTest(0x778899u, true);
    SaveSync_SetTcpPortForTest(tcpPort);

    // Idempotent LanDiscovery_Init
    LanDiscovery_Init();
    LanDiscovery_Init();
    std::string dispName = LanDiscovery_GetLocalDisplayName();
    TEST_CHECK(!dispName.empty());

    // Put mock world into loaded state with a save map name
    *(int64_t*)(g_mockExeBuffer + 0xA2E760) = 0x1000LL;
    *(volatile char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    strncpy_s((char*)(g_mockExeBuffer + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET), 64, "save/DonbasCoop", _TRUNCATE);
    TEST_CHECK(SaveSync_IsWorldLoaded());

    // Always run full host/client lifecycle on isolated ephemeral UDP + TCP ports.
    char localOpenEvName[64] = {};
    _snprintf_s(localOpenEvName, sizeof(localOpenEvName), _TRUNCATE,
                "Local\\RepublicMP_LocalHostOpen_%u", static_cast<unsigned>(tcpPort));

    SaveSync_StartHost();
    TEST_CHECK(SaveSync_GetState() == SESSION_HOST_LISTENING);

    // LocalHostOpen_<tcpPort> event must be signaled while listening
    HANDLE hOpenEv = OpenEventA(SYNCHRONIZE, FALSE, localOpenEvName);
    TEST_CHECK(hOpenEv != nullptr);
    TEST_CHECK(WaitForSingleObject(hOpenEv, 0) == WAIT_OBJECT_0);
    CloseHandle(hOpenEv);

    // Scanner on loopback must discover our open LAN host with Steam offline and mapName == "DonbasCoop"
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();
    bool discovered = false;
    for (int i = 0; i < 80; ++i) {
        auto hosts = LanDiscovery_GetHosts();
        for (const auto& h : hosts) {
            if (h.tcpPort == tcpPort && strcmp(h.mapName, "DonbasCoop") == 0) {
                TEST_CHECK(h.steamOnline == 0);
                TEST_CHECK(h.protocol == REPUBLICMP_PROTOCOL);
                TEST_CHECK(h.protocolMatch);
                TEST_CHECK(h.hostName[0] != '\0');
                discovered = true;
                break;
            }
        }
        if (discovered) break;
        if ((i % 6) == 5) LanDiscovery_ProbeNow();
        Sleep(35);
    }
    TEST_CHECK(discovered);

    // Stop host -> returns to SESSION_IDLE, clears LocalHostOpen event, sends bye burst
    SaveSync_RequestDisconnect();
    for (int i = 0; i < 60 && SaveSync_GetState() != SESSION_IDLE; ++i) {
        Sleep(50);
    }
    TEST_CHECK(SaveSync_GetState() == SESSION_IDLE);
    TEST_CHECK(!SaveSync_IsLocalPcHostWaiting());

    // Verify map name fallback to "Co-op World" when save field is mp_syncsave_*
    strncpy_s((char*)(g_mockExeBuffer + WORLD_THIS_RVA + SAVE_NAME_FIELD_OFFSET), 64, "save/mp_syncsave_deadbeef", _TRUNCATE);
    SaveSync_StartHost();
    TEST_CHECK(SaveSync_GetState() == SESSION_HOST_LISTENING);
    LanDiscovery_ProbeNow();
    bool fallbackSeen = false;
    for (int i = 0; i < 80; ++i) {
        for (const auto& h : LanDiscovery_GetHosts()) {
            if (h.tcpPort == tcpPort && strcmp(h.mapName, "Co-op World") == 0) {
                fallbackSeen = true;
                break;
            }
        }
        if (fallbackSeen) break;
        if ((i % 6) == 5) LanDiscovery_ProbeNow();
        Sleep(35);
    }
    TEST_CHECK(fallbackSeen);
    SaveSync_RequestDisconnect();
    for (int i = 0; i < 60 && SaveSync_GetState() != SESSION_IDLE; ++i) {
        Sleep(50);
    }
    TEST_CHECK(SaveSync_GetState() == SESSION_IDLE);

    // Verify IP:port normalization for client target & Lockstep host extraction
    char clientTarget[64] = {};
    _snprintf_s(clientTarget, sizeof(clientTarget), _TRUNCATE, " 127.0.0.1:%u ", static_cast<unsigned>(tcpPort));
    char normHost[64] = {};
    uint16_t parsedPort = 0;
    TEST_CHECK(NetworkNormalizeTargetAddress(clientTarget, normHost, sizeof(normHost), &parsedPort));
    TEST_CHECK(strcmp(normHost, "127.0.0.1") == 0);
    TEST_CHECK(parsedPort == tcpPort);
    SaveSync_StartClient(clientTarget);
    SaveSync_RequestDisconnect();
    for (int i = 0; i < 80 && SaveSync_GetState() != SESSION_IDLE; ++i) {
        Sleep(50);
    }
    TEST_CHECK(SaveSync_GetState() == SESSION_IDLE);
    SaveSync_SetTcpPortForTest(0);

    // SaveSync SessionHello / SessionDlcs verification + loopback hello exchange
    {
        char why[512] = {};
        const unsigned int crc = SaveSync_GetLocalDllCrc();
        TEST_CHECK(SaveSync_VerifyPeerHelloForTest("RPMPSES", REPUBLICMP_PROTOCOL, REPUBLICMP_VERSION, crc, why, sizeof(why)));
        TEST_CHECK(!SaveSync_VerifyPeerHelloForTest("BADMAGC", REPUBLICMP_PROTOCOL, REPUBLICMP_VERSION, crc, why, sizeof(why)));
        TEST_CHECK(!SaveSync_VerifyPeerHelloForTest("RPMPSES", REPUBLICMP_PROTOCOL + 1, REPUBLICMP_VERSION, crc, why, sizeof(why)));
        TEST_CHECK(!SaveSync_VerifyPeerHelloForTest("RPMPSES", REPUBLICMP_PROTOCOL, "0.0.0", crc, why, sizeof(why)));
        TEST_CHECK(!SaveSync_VerifyPeerHelloForTest("RPMPSES", REPUBLICMP_PROTOCOL, REPUBLICMP_VERSION, crc ^ 0x1234u, why, sizeof(why)));

        TEST_CHECK(SaveSync_VerifyDlcListsForTest("dlc1,dlc2", true, "dlc1,dlc2", true, why, sizeof(why)));
        TEST_CHECK(!SaveSync_VerifyDlcListsForTest("dlc1,dlc2", true, "dlc1", true, why, sizeof(why)));
        TEST_CHECK(SaveSync_VerifyDlcListsForTest("dlc1,dlc2", false, "dlc1", true, why, sizeof(why)));
        TEST_CHECK(SaveSync_VerifyDlcListsForTest("dlc1,dlc2", true, "dlc1", false, why, sizeof(why)));

        SOCKET sH = INVALID_SOCKET, sC = INVALID_SOCKET;
        TEST_CHECK(CreateLoopbackTcpPair(&sH, &sC));
        bool hostHelloOk = false;
        char hWhy[256] = {}, cWhy[256] = {};
        std::thread hThr([&]() {
            hostHelloOk = SaveSync_ExchangeHelloOverSocketForTest((uintptr_t)sH, true, hWhy, sizeof(hWhy));
        });
        const bool cliHelloOk = SaveSync_ExchangeHelloOverSocketForTest((uintptr_t)sC, false, cWhy, sizeof(cWhy));
        hThr.join();
        NetworkCloseSocket(sH);
        NetworkCloseSocket(sC);
        TEST_CHECK(hostHelloOk && cliHelloOk);
    }

    // SaveSync_FormatDisplayTarget (no double-port or :5556 on steam:)
    {
        char disp[96] = {};
        bool isSteam = false;
        SaveSync_FormatDisplayTarget("192.168.1.50", 5556u, disp, sizeof(disp), &isSteam);
        TEST_CHECK(!isSteam && strcmp(disp, "192.168.1.50:5556") == 0);
        SaveSync_FormatDisplayTarget(" 192.168.1.50:5577 ", 5556u, disp, sizeof(disp), &isSteam);
        TEST_CHECK(!isSteam && strcmp(disp, "192.168.1.50:5577") == 0);
        SaveSync_FormatDisplayTarget("steam:76561198000000000", 5556u, disp, sizeof(disp), &isSteam);
        TEST_CHECK(isSteam && strcmp(disp, "steam:76561198000000000") == 0);
        SaveSync_FormatDisplayTarget("76561198000000000", 5556u, disp, sizeof(disp), &isSteam);
        TEST_CHECK(isSteam && strcmp(disp, "steam:76561198000000000") == 0);
    }

    // Concurrent SaveSync string getters + Lang_Get / Lang_Set / Tr thread safety
    {
        std::atomic<bool> stopStress(false);
        std::atomic<bool> workerOk(true);
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([t, &stopStress, &workerOk]() {
                for (int i = 0; i < 200 && !stopStress.load(); ++i) {
                    if (t == 0) {
                        SaveSync_SetTestSessionState(SESSION_IDLE, (i & 1) != 0,
                                                     (i & 1) ? "192.168.1.10" : "127.0.0.1");
                        Lang_Set((i & 1) ? MP_LANG_EN : MP_LANG_RU);
                    } else {
                        const char* p1 = SaveSync_GetPeerIp();
                        const char* p2 = SaveSync_GetLastError();
                        const char* p3 = SaveSync_GetLastPartnerPeer();
                        const char* p4 = SaveSync_GetTargetIp();
                        const char* tr = Tr("ru", "en");
                        if (!p1 || !p2 || !p3 || !p4 || !tr) {
                            workerOk.store(false);
                        }
                        (void)Lang_Get();
                        Lang_AutoDetectFromGame(MP_LANG_RU);
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
        stopStress.store(true);
        TEST_CHECK(workerOk.load());
    }

    // DiagLog_IsSafeRemoteLogFileName, DiagLog_SaveRemoteClientLogBundle & InitAndWrapHost(nullptr)
    {
        TEST_CHECK(DiagLog_IsSafeRemoteLogFileName("RepublicMP_Diagnostics.log"));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName(nullptr));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName(""));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName("CON"));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName("NUL.log"));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName("COM1.txt"));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName("a.log:ads"));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName("../x.log"));
        TEST_CHECK(!DiagLog_IsSafeRemoteLogFileName(".."));

        // Verify DiagLog_SaveRemoteClientLogBundle rejects a bundle containing only unsafe filenames
#pragma pack(push, 1)
        struct BadBundleHdr {
            char magic[8];
            char reason[32];
            char peerName[64];
            unsigned int rawSize;
            unsigned int compSize;
        };
        struct BadFileEntry {
            char fileName[48];
            unsigned int size;
        };
#pragma pack(pop)
        const char* kBadNames[4] = { "CON", "NUL.log", "a.log:ads", "../x.log" };
        for (const char* badName : kBadNames) {
            std::vector<unsigned char> raw(sizeof(BadBundleHdr) + sizeof(BadFileEntry) + 4, 0);
            auto* bh = (BadBundleHdr*)raw.data();
            memcpy(bh->magic, "RPMPLOG1", 8);
            strncpy_s(bh->reason, sizeof(bh->reason), "M2TEST", _TRUNCATE);
            strncpy_s(bh->peerName, sizeof(bh->peerName), "Tester", _TRUNCATE);
            bh->rawSize = sizeof(BadFileEntry) + 4;
            bh->compSize = 0;
            auto* fe = (BadFileEntry*)(raw.data() + sizeof(BadBundleHdr));
            strncpy_s(fe->fileName, sizeof(fe->fileName), badName, _TRUNCATE);
            fe->size = 4;
            memcpy(raw.data() + sizeof(BadBundleHdr) + sizeof(BadFileEntry), "test", 4);
            TEST_CHECK(!DiagLog_SaveRemoteClientLogBundle(raw.data(), raw.size()));
        }
        const TsmHost* wrappedNull = DiagLog_InitAndWrapHost(nullptr);
        TEST_CHECK(wrappedNull != nullptr && wrappedNull->log != nullptr);
    }

    // Verify DiagLog_BuildClientLogBundle offline identity uses "<name> (LAN)" instead of "Client"
    std::filesystem::path dummyLog = std::filesystem::path(g_mockBaseDir) / "RepublicMP_Diagnostics.log";
    guard.tempDirs.push_back(dummyLog);
    TEST_CHECK(WriteTextFile(dummyLog, "2026-11-21T15:00:00Z [INFO] M2 test log line\r\n"));
    bool prevSendLogs = TwoStates_GetSendClientLogs();
    TwoStates_SetSendClientLogs(true);
    std::vector<unsigned char> bundle;
    bool builtBundle = DiagLog_BuildClientLogBundle(bundle, "CLIENT_JOIN");
    TwoStates_SetSendClientLogs(prevSendLogs);
    TEST_CHECK(builtBundle);
    TEST_CHECK(bundle.size() >= 8 + 32 + 64 + 8);
    TEST_CHECK(memcmp(bundle.data(), "RPMPLOG1", 8) == 0);
    const char* peerName = (const char*)(bundle.data() + 8 + 32);
    TEST_CHECK(peerName[0] != '\0');
    TEST_CHECK(strcmp(peerName, "Client") != 0);
    if (dispName != "Host") {
        TEST_CHECK(strstr(peerName, "(LAN)") != nullptr);
    }

    // Protocol constant sanity check
    TEST_CHECK(REPUBLICMP_PROTOCOL >= 63);
    TEST_CHECK(strcmp(SaveSync_GetLocalVersion(), REPUBLICMP_VERSION) == 0);

    // Idempotent LanDiscovery_Shutdown
    LanDiscovery_StopScan();
    LanDiscovery_Shutdown();
    LanDiscovery_Shutdown();
    return true;
}

} // namespace

static bool Test_SteamFreeSessionAndModSync_M2()
{
    M2LoopbackEnvGuard guard;

    printf("    [SubTest 1: F6 Steam Offline, BLoggedOn, ParseTargetSteamId & Dual-Testbed] ...\n");
    fflush(stdout);
    if (!SubTest_M2_F6_SteamOfflineAndParseHardening()) return false;

    printf("    [SubTest 2: F7 Disk Fallback (workshop_wip / workshop / 784150) & Catalog] ...\n");
    fflush(stdout);
    if (!SubTest_M2_F7_DiskFallbackAndCatalog(guard)) return false;

    printf("    [SubTest 3: F7 Offline Download Guards & Loopback ModSync Handshake] ...\n");
    fflush(stdout);
    if (!SubTest_M2_F7_OfflineGuardsAndLoopbackHandshake(guard)) return false;

    printf("    [SubTest 4: F8 Steam-Free Session Lifecycle, LocalHostOpen & DiagLog Identity] ...\n");
    fflush(stdout);
    if (!SubTest_M2_F8_SessionLifecycleAndIdentity(guard)) return false;

    return true;
}
