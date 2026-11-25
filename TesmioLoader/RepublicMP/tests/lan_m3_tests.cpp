// SPDX-License-Identifier: GPL-3.0-only
// Milestone M3 Unit, Headless ImGui & Loopback Integration Tests (Features F9-F11):
//   - F9: Shared Win32 Clipboard (CF_UNICODETEXT + CF_TEXT), Toast Color Normalization,
//         SESSION_IDLE Overlay Rendering, TwoStates_OnSessionEnded Toast Preservation,
//         and Idempotent LanDiscovery_Shutdown.
//   - F10: Multi-NIC Host IPv4 Selection, VPN Provider Tagging (Radmin/Hamachi/ZeroTier/Tailscale),
//          Copyable Host IP Formatting, Steam-Offline No-SteamID Summary, RU/EN Lang Strings.
//   - F11: NATIVE_PAGE_JOIN Slot Planner (<= 28 slots, <= 21 char labels, << Back invariant,
//          127.0.0.1 dedup, [!ver] protocol mismatch, offline Refresh Hosts, UTF-8 truncation),
//          Live Loopback Discovery -> Join Plan, and Manual Address Normalization Matrix.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../core/engine_rva.h"
#include "../core/version.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../transport/lan_discovery.h"
#include "../transport/network.h"
#include "../transport/steam_p2p.h"
#include "../ui/lang.h"
#include "../ui/native_menu.h"
#include "../ui/sync_block.h"
#include "../ui/ui_common.h"
#include "imgui.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct M3TestEnvGuard {
    int prevLang;
    std::filesystem::path settingsIniPath;
    bool hadSettingsIni = false;
    std::string prevSettingsIniBytes;

    M3TestEnvGuard() : prevLang(Lang_Get())
    {
        LanDiscovery_SetLoopbackOnlyForTest(true);
        Network_SetLoopbackOnlyForTest(true);
        TwoStates_ClearToastsForTest();

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

    ~M3TestEnvGuard()
    {
        if (SaveSync_GetState() != SESSION_IDLE) {
            SaveSync_RequestDisconnect();
            for (int i = 0; i < 60 && SaveSync_GetState() != SESSION_IDLE; ++i) {
                Sleep(50);
            }
        }
        SyncBlock_SetActive(false, nullptr);
        SyncBlock_SetMapInspectMode(false);
        LanDiscovery_StopScan();
        LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
        LanDiscovery_Shutdown();
        LanDiscovery_SetPortForTest(0);
        LanDiscovery_SetSelfPidOverrideForTest(0, false);
        SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
        if (g_mockExeBuffer) {
            *(int64_t*)(g_mockExeBuffer + 0xA2E758) = 0;
            *(volatile char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
        }
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
    }

    M3TestEnvGuard(const M3TestEnvGuard&) = delete;
    M3TestEnvGuard& operator=(const M3TestEnvGuard&) = delete;
};

static bool M3IsClipboardOpenable()
{
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(nullptr)) {
            CloseClipboard();
            return true;
        }
        Sleep(15);
    }
    return false;
}

static bool M3ReadCfText(std::string& outText)
{
    outText.clear();
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(nullptr)) {
            HANDLE hData = GetClipboardData(CF_TEXT);
            if (hData) {
                const char* p = static_cast<const char*>(GlobalLock(hData));
                if (p) {
                    outText.assign(p);
                    GlobalUnlock(hData);
                }
            }
            CloseClipboard();
            return !outText.empty();
        }
        Sleep(15);
    }
    return false;
}

static bool M3TryBindLoopbackUdp(uint16_t port, bool reuseAddr)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    if (reuseAddr) {
        BOOL reuse = TRUE;
        (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    const bool ok = (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    closesocket(s);
    return ok;
}

static uint16_t M3AllocLoopbackUdpPort()
{
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    static std::atomic<uint32_t> s_seq{0};
    const uint32_t bias = ((GetCurrentProcessId() % 200u) * 19u) + (s_seq.fetch_add(1u) * 11u);
    const uint16_t minPort = 44000u;
    const uint16_t span = 4999u; // 44000..48998
    uint16_t foundPort = 0;
    for (uint16_t offset = 0; offset < span; ++offset) {
        const uint16_t cand = static_cast<uint16_t>(minPort + ((bias + offset) % span));
        if (M3TryBindLoopbackUdp(cand, false) && M3TryBindLoopbackUdp(cand, true)) {
            foundPort = cand;
            break;
        }
    }
    WSACleanup();
    return foundPort;
}

static LanAdapterInfo MakeAdapter(LanAdapterKind kind, int priority, uint32_t ifIndex,
                                  const char* ip, const char* name)
{
    LanAdapterInfo ad = {};
    ad.kind = kind;
    ad.priority = priority;
    ad.ifIndex = ifIndex;
    strncpy_s(ad.ip, ip ? ip : "", _TRUNCATE);
    strncpy_s(ad.mask, "255.255.255.0", _TRUNCATE);
    strncpy_s(ad.broadcast, "255.255.255.255", _TRUNCATE);
    strncpy_s(ad.name, name ? name : "", _TRUNCATE);
    return ad;
}

static LanHostEntry MakeHost(uint64_t guid, const char* ip, uint16_t tcpPort,
                             const char* name, bool sameMachine, bool protoMatch,
                             LanAdapterKind pathKind = LanAdapterKind::Ethernet,
                             uint64_t lastSeenMs = 1000ULL)
{
    LanHostEntry h = {};
    h.serverGuid = guid;
    h.hostPid = static_cast<uint32_t>(1000ULL + guid);
    h.protocol = protoMatch ? REPUBLICMP_PROTOCOL : static_cast<uint16_t>(REPUBLICMP_PROTOCOL - 1);
    h.tcpPort = tcpPort;
    h.sameMachine = sameMachine;
    h.protocolMatch = protoMatch;
    h.pathKind = pathKind;
    h.lastSeenMs = lastSeenMs;
    strncpy_s(h.connectIp, ip ? ip : "", _TRUNCATE);
    strncpy_s(h.hostName, name ? name : "", _TRUNCATE);
    strncpy_s(h.mapName, "TestMap", _TRUNCATE);
    strncpy_s(h.version, REPUBLICMP_VERSION, _TRUNCATE);
    return h;
}

static bool IsValidUtf8Strict(const std::string& s)
{
    if (s.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               s.data(), static_cast<int>(s.size()),
                               nullptr, 0) > 0;
}

static bool HasNoUnpairedUtf16Surrogates(const std::wstring& ws)
{
    for (size_t i = 0; i < ws.size(); ++i) {
        const wchar_t ch = ws[i];
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (i + 1 >= ws.size() || ws[i + 1] < 0xDC00 || ws[i + 1] > 0xDFFF) {
                return false;
            }
            ++i;
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

static bool SubTest_M3_F9_ClipboardSharedAndUtf8()
{
    TEST_CHECK(!UiCommon_CopyTextToClipboard(nullptr));
    TEST_CHECK(!UiCommon_CopyTextToClipboard(""));

    char norm[64] = {};
    TEST_CHECK(!UiCommon_NormalizeClipboardJoinAddress(nullptr, norm, sizeof(norm)));
    TEST_CHECK(!UiCommon_NormalizeClipboardJoinAddress("   \t\r\n ", norm, sizeof(norm)));
    TEST_CHECK(UiCommon_NormalizeClipboardJoinAddress("  192.168.1.50:5556 \r\n", norm, sizeof(norm)));
    TEST_CHECK(strcmp(norm, "192.168.1.50:5556") == 0);
    TEST_CHECK(UiCommon_NormalizeClipboardJoinAddress(" 76561198012345678 ", norm, sizeof(norm)));
    TEST_CHECK(strcmp(norm, "steam:76561198012345678") == 0);
    TEST_CHECK(UiCommon_NormalizeClipboardJoinAddress("steam:76561198000000001", norm, sizeof(norm)));
    TEST_CHECK(strcmp(norm, "steam:76561198000000001") == 0);
    TEST_CHECK(UiCommon_NormalizeClipboardJoinAddress("192.168.1.50:05556", norm, sizeof(norm)));
    TEST_CHECK(strcmp(norm, "192.168.1.50:5556") == 0);

    // Regression: reject garbage, URLs, invalid octets/ports, invalid SteamID lengths, non-7656 raw digits, and small buffers.
    const char* const invalidClipboardInputs[] = {
        "garbage_text_in_clipboard",
        "https://example.com/some/url",
        "999.999.999.999",
        "192.168.1.10:0",
        "192.168.1.10:70000",
        "76561198012345",          // 14 digits
        "765611980123456",         // 15 digits
        "76561197960265727",       // 17 digits < min SteamID64
        "12345678901234567",       // 17 digits not SteamID64
        "80000000000000000",       // 17 digits not starting with 7656
        "18446744073709551615",    // 20 digits not starting with 7656
        "76561198012345678901",    // 20 digits (> UINT64_MAX)
        "765611980123456789012"    // 21 digits
    };
    for (const char* bad : invalidClipboardInputs) {
        memset(norm, 'X', sizeof(norm));
        norm[sizeof(norm) - 1] = '\0';
        TEST_CHECK(!UiCommon_NormalizeClipboardJoinAddress(bad, norm, sizeof(norm)));
        TEST_CHECK(norm[0] == '\0');
    }

    char smallBuf[10] = "abcdefghi";
    TEST_CHECK(!UiCommon_NormalizeClipboardJoinAddress("192.168.1.50", smallBuf, sizeof(smallBuf)));
    TEST_CHECK(smallBuf[0] == '\0');
    char exactMinusOne[12] = "abcdefghijk";
    TEST_CHECK(!UiCommon_NormalizeClipboardJoinAddress("192.168.1.50", exactMinusOne, sizeof(exactMinusOne)));
    TEST_CHECK(exactMinusOne[0] == '\0');
    char exactFit[13] = {};
    TEST_CHECK(UiCommon_NormalizeClipboardJoinAddress("192.168.1.50", exactFit, sizeof(exactFit)));
    TEST_CHECK(strcmp(exactFit, "192.168.1.50") == 0);

    if (M3IsClipboardOpenable()) {
        TEST_CHECK(UiCommon_CopyTextToClipboard("192.168.1.50:5556"));
        std::string readBack;
        TEST_CHECK(UiCommon_ReadTextFromClipboard(readBack));
        TEST_CHECK(readBack == "192.168.1.50:5556");

        const std::string utf8 = "Сервер-ЛВС-192.168.1.10:5556";
        TEST_CHECK(UiCommon_CopyTextToClipboard(utf8.c_str()));
        TEST_CHECK(UiCommon_ReadTextFromClipboard(readBack));
        TEST_CHECK(readBack == utf8);
        std::string cfText;
        TEST_CHECK(M3ReadCfText(cfText));
        TEST_CHECK(cfText == utf8);
    }
    return true;
}

static int RenderIdleTwoStatesOverlayAndCountVertices()
{
    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* texPixels = nullptr;
    int texW = 0, texH = 0;
    io.Fonts->GetTexDataAsRGBA32(&texPixels, &texW, &texH);

    ImGui::NewFrame();
    TwoStates_DrawOverlay();
    ImGui::Render();

    int totalVtx = 0;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (bg) totalVtx += bg->VtxBuffer.Size;
    if (fg) totalVtx += fg->VtxBuffer.Size;

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prevCtx);
    return totalVtx;
}

static bool SubTest_M3_F9_ToastNormalizeSessionEndedAndIdleRender()
{
    TEST_CHECK(UiCommon_NormalizeToastColorKind(-10) == 0);
    for (int k = 0; k <= 5; ++k) {
        TEST_CHECK(UiCommon_NormalizeToastColorKind(k) == k);
    }
    TEST_CHECK(UiCommon_NormalizeToastColorKind(6) == 5);
    TEST_CHECK(UiCommon_NormalizeToastColorKind(99) == 5);

    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TwoStates_ClearToastsForTest();
    TEST_CHECK(TwoStates_GetToastCountForTest() == 0);
    TEST_CHECK(RenderIdleTwoStatesOverlayAndCountVertices() == 0);

    // Null & empty ignored; out-of-range color clamped; queue bounded at 5.
    TwoStates_AddToast(nullptr, 0);
    TwoStates_AddToast("", 3);
    TEST_CHECK(TwoStates_GetToastCountForTest() == 0);

    TwoStates_AddToast("Info toast", 0);
    TwoStates_AddToast("Success toast", 1);
    TwoStates_AddToast("Warn toast", 2);
    TwoStates_AddToast("Error toast", 3);
    TwoStates_AddToast("Gold toast", 4);
    TwoStates_AddToast("Workshop cyan toast", 99); // pops oldest, clamps 99 -> 5
    TEST_CHECK(TwoStates_GetToastCountForTest() == 5);

    char txt[128] = {};
    int ck = -1;
    unsigned long long dur = 0;
    TEST_CHECK(TwoStates_GetToastForTest(4, txt, sizeof(txt), &ck, &dur));
    TEST_CHECK(strcmp(txt, "Workshop cyan toast") == 0);
    TEST_CHECK(ck == 5);
    TEST_CHECK(dur == 5500ULL);

    // TwoStates_OnSessionEnded must preserve toasts across disconnect/join-failure teardown.
    TwoStates_OnSessionEnded();
    TwoStates_OnSessionEnded();
    TEST_CHECK(TwoStates_GetToastCountForTest() == 5);

    const int vtx = RenderIdleTwoStatesOverlayAndCountVertices();
    TEST_CHECK(vtx > 0);

    // Future createdMs > now must NOT underflow ULONGLONG and purge the toast!
    TwoStates_ClearToastsForTest();
    TwoStates_AddToastAtTickForTest("Future tick toast", 2, GetTickCount64() + 5000ULL);
    TEST_CHECK(TwoStates_GetToastCountForTest() == 1);
    TEST_CHECK(RenderIdleTwoStatesOverlayAndCountVertices() > 0);
    TEST_CHECK(TwoStates_GetToastCountForTest() == 1);

    TwoStates_ClearToastsForTest();
    TEST_CHECK(TwoStates_GetToastCountForTest() == 0);
    TEST_CHECK(RenderIdleTwoStatesOverlayAndCountVertices() == 0);

    // Idempotent shutdown safety (also invoked on WM_DESTROY).
    LanDiscovery_Shutdown();
    LanDiscovery_Shutdown();
    return true;
}

static bool SubTest_M3_F10_MultiAdapterHostViewAndTags()
{
    // Empty adapter list -> 127.0.0.1 fallback.
    std::vector<LanAdapterInfo> emptyAds;
    const UiHostAddressView emptyViewEn = UiCommon_BuildHostAddressView(emptyAds, 5556, 5556, false);
    TEST_CHECK(!emptyViewEn.hasLanAdapter);
    TEST_CHECK(emptyViewEn.primaryIp == "127.0.0.1");
    TEST_CHECK(emptyViewEn.primaryEndpoint == "127.0.0.1:5556");
    TEST_CHECK(emptyViewEn.primaryCopyText == "127.0.0.1");
    TEST_CHECK(emptyViewEn.primaryTag == "Local PC");

    const UiHostAddressView emptyViewRu = UiCommon_BuildHostAddressView(emptyAds, 5577, 5556, true);
    TEST_CHECK(emptyViewRu.primaryEndpoint == "127.0.0.1:5577");
    TEST_CHECK(emptyViewRu.primaryCopyText == "127.0.0.1:5577");
    TEST_CHECK(!emptyViewRu.primaryTag.empty());

    // Multi-adapter unsorted + duplicate IP + Hyper-V VirtualLocal + VPNs + Wi-Fi + Ethernet.
    std::vector<LanAdapterInfo> ads = {
        MakeAdapter(LanAdapterKind::VirtualLocal, 50, 9, "172.29.0.1", "vEthernet (Default Switch)"),
        MakeAdapter(LanAdapterKind::VirtualVpn, 200, 7, "172.24.1.2", "ZeroTier One"),
        MakeAdapter(LanAdapterKind::VirtualVpn, 200, 6, "25.10.20.30", "LogMeIn Hamachi"),
        MakeAdapter(LanAdapterKind::VirtualVpn, 200, 5, "26.15.25.35", "Radmin VPN"),
        MakeAdapter(LanAdapterKind::VirtualVpn, 200, 8, "100.64.1.2", "Tailscale"),
        MakeAdapter(LanAdapterKind::WiFi, 300, 3, "10.0.0.5", "Wi-Fi"),
        MakeAdapter(LanAdapterKind::Ethernet, 400, 2, "192.168.1.10", "Ethernet"),
        MakeAdapter(LanAdapterKind::Ethernet, 400, 4, "192.168.1.10", "Ethernet Duplicate")
    };
    const UiHostAddressView view = UiCommon_BuildHostAddressView(ads, 5556, 5556, false);
    TEST_CHECK(view.hasLanAdapter);
    TEST_CHECK(view.primaryIp == "192.168.1.10");
    TEST_CHECK(view.primaryEndpoint == "192.168.1.10:5556");
    TEST_CHECK(view.primaryCopyText == "192.168.1.10");
    TEST_CHECK(view.primaryTag == "Ethernet");
    TEST_CHECK(view.displayAdapters.size() == 7u);
    TEST_CHECK(view.displayAdapters.back().kind == LanAdapterKind::VirtualLocal);

    TEST_CHECK(strcmp(UiCommon_AdapterKindTag(LanAdapterKind::VirtualVpn, "", "26.1.2.3", false), "Radmin VPN") == 0);
    TEST_CHECK(strcmp(UiCommon_AdapterKindTag(LanAdapterKind::VirtualVpn, "", "25.1.2.3", false), "Hamachi") == 0);
    TEST_CHECK(strcmp(UiCommon_AdapterKindTag(LanAdapterKind::VirtualVpn, "ZeroTier", "10.1.2.3", false), "ZeroTier") == 0);
    TEST_CHECK(strcmp(UiCommon_AdapterKindTag(LanAdapterKind::VirtualVpn, "", "10.147.20.5", false), "ZeroTier") == 0);
    TEST_CHECK(strcmp(UiCommon_AdapterKindTag(LanAdapterKind::VirtualVpn, "", "100.100.1.2", false), "Tailscale") == 0);
    TEST_CHECK(strcmp(UiCommon_AdapterKindTag(LanAdapterKind::VirtualVpn, "WireGuard Tunnel", "10.8.0.2", false), "WireGuard") == 0);
    TEST_CHECK(strcmp(UiCommon_AdapterKindTag(LanAdapterKind::VirtualVpn, "OpenVPN TAP", "10.9.0.2", false), "OpenVPN") == 0);

    // Regression: duplicate-IP priority sorting must happen BEFORE deduplication.
    {
        std::vector<LanAdapterInfo> dupSameIp = {
            MakeAdapter(LanAdapterKind::VirtualLocal, 50, 1, "192.168.1.10", "vEthernet (WSL)"),
            MakeAdapter(LanAdapterKind::Ethernet, 400, 2, "192.168.1.10", "Ethernet")
        };
        const UiHostAddressView dupView = UiCommon_BuildHostAddressView(dupSameIp, 5556, 5556, false);
        TEST_CHECK(dupView.hasLanAdapter);
        TEST_CHECK(dupView.primaryTag == "Ethernet");
        TEST_CHECK(dupView.displayAdapters.size() == 1u);
        TEST_CHECK(dupView.displayAdapters[0].kind == LanAdapterKind::Ethernet);
    }

    // Regression: UTF-8 Cyrillic & 4-byte emoji adapter name truncation must remain valid UTF-8.
    {
        const LanAdapterInfo ruAdapter = MakeAdapter(
            LanAdapterKind::Ethernet, 400, 1, "192.168.1.20",
            "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 "
            "\xD0\xBF\xD0\xBE \xD0\xBB\xD0\xBE\xD0\xBA\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xBE\xD0\xB9 "
            "\xD1\x81\xD0\xB5\xD1\x82\xD0\xB8");
        const std::string ruLine = UiCommon_FormatAdapterLine(ruAdapter, 5556, true, true);
        TEST_CHECK(!ruLine.empty());
        TEST_CHECK(IsValidUtf8Strict(ruLine));
        TEST_CHECK(ruLine.find("..") != std::string::npos);

        const LanAdapterInfo emojiAdapter = MakeAdapter(
            LanAdapterKind::WiFi, 300, 2, "10.0.0.8",
            "Net\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5"
            "\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5");
        const std::string emojiLine = UiCommon_FormatAdapterLine(emojiAdapter, 5556, true, false);
        TEST_CHECK(IsValidUtf8Strict(emojiLine));
    }

    const std::string radminLine = UiCommon_FormatAdapterLine(ads[3], 5556, true, false);
    TEST_CHECK(radminLine.find("[Radmin VPN]") != std::string::npos);
    TEST_CHECK(radminLine.find("26.15.25.35:5556") != std::string::npos);

    const std::string onlineSum = UiCommon_FormatHostTransportSummary("192.168.1.10:5556", true, 76561198000000123ULL, false);
    TEST_CHECK(onlineSum.find("SteamID: 76561198000000123") != std::string::npos);
    const std::string offEn = UiCommon_FormatHostTransportSummary("192.168.1.10:5556", false, 0ULL, false);
    const std::string offRu = UiCommon_FormatHostTransportSummary("192.168.1.10:5556", false, 0ULL, true);
    TEST_CHECK(offEn.find("SteamID: 0") == std::string::npos);
    TEST_CHECK(offRu.find("SteamID: 0") == std::string::npos);
    TEST_CHECK(offEn.find("Offline") != std::string::npos);

    Lang_Set(MP_LANG_RU);
    TEST_CHECK(strlen(Lang_CopyIpLabel()) > 0);
    TEST_CHECK(strlen(Lang_CopiedLabel()) > 0);
    TEST_CHECK(strlen(Lang_SteamOfflineLabel()) > 0);
    Lang_Set(MP_LANG_EN);
    TEST_CHECK(strcmp(Lang_CopyIpLabel(), "Copy IP") == 0);
    TEST_CHECK(strcmp(Lang_CopiedLabel(), "Copied!") == 0);

    // VirtualLocal-only fallback in UiCommon_SelectPrimaryAdapter (must return VirtualLocal, not nullptr).
    std::vector<LanAdapterInfo> onlyVirtualLocal = {
        MakeAdapter(LanAdapterKind::VirtualLocal, 50, 9, "172.29.0.1", "vEthernet (WSL)"),
    };
    const LanAdapterInfo* vlocPrimary = UiCommon_SelectPrimaryAdapter(onlyVirtualLocal);
    TEST_CHECK(vlocPrimary != nullptr && strcmp(vlocPrimary->ip, "172.29.0.1") == 0);

    // NativeMenu_SelectSidebarExtraAdapters surfaces VirtualVpn at index 2+ above 2nd Ethernet/Wi-Fi.
    std::vector<LanAdapterInfo> threeAds = {
        MakeAdapter(LanAdapterKind::Ethernet, 400, 1, "192.168.1.10", "Ethernet"),
        MakeAdapter(LanAdapterKind::WiFi, 300, 2, "192.168.1.11", "Wi-Fi"),
        MakeAdapter(LanAdapterKind::Ethernet, 400, 3, "192.168.2.10", "Ethernet 2"),
        MakeAdapter(LanAdapterKind::VirtualVpn, 200, 4, "26.10.20.30", "Radmin VPN"),
    };
    const auto extras = NativeMenu_SelectSidebarExtraAdapters(threeAds, 2);
    TEST_CHECK(extras.size() == 2);
    TEST_CHECK(strcmp(extras[0].ip, "192.168.1.11") == 0);
    TEST_CHECK(strcmp(extras[1].ip, "26.10.20.30") == 0);
    return true;
}

static bool AssertJoinPlanInvariants(const std::vector<JoinPlanEntry>& entries, int maxSlots)
{
    TEST_CHECK(!entries.empty());
    TEST_CHECK(static_cast<int>(entries.size()) <= maxSlots);
    TEST_CHECK(entries.back().kind == JOIN_ACT_BACK);
    bool hasRefresh = false;
    bool hasManual = false;
    bool hasPaste = false;
    bool hasConnect = false;
    for (const auto& e : entries) {
        TEST_CHECK(!e.label.empty());
        TEST_CHECK(e.label.size() <= NATIVE_JOIN_MAX_LABEL_CHARS);
        if (e.kind == JOIN_ACT_REFRESH_HOSTS) hasRefresh = true;
        if (e.kind == JOIN_ACT_TYPE_MANUAL) hasManual = true;
        if (e.kind == JOIN_ACT_PASTE_CLIPBOARD) hasPaste = true;
        if (e.kind == JOIN_ACT_CONNECT_ADDR) hasConnect = true;
    }
    if (maxSlots >= 5) {
        TEST_CHECK(hasConnect && hasManual && hasPaste && hasRefresh);
    }
    return true;
}

static bool SubTest_M3_F11_JoinPlannerSlotsDedupAndErrors()
{
    // 1. 0 hosts + Steam offline (RU & EN): Refresh Hosts present, Steam Offline row present, <= 21 chars.
    const std::vector<LanHostEntry> noHosts;
    const std::vector<NativeMenuFriendInfo> noFriends;
    for (int en = 0; en <= 1; ++en) {
        const auto plan = NativeMenu_PlanJoinEntries(false, noHosts, false, noFriends, "127.0.0.1", en != 0);
        TEST_CHECK(AssertJoinPlanInvariants(plan, NATIVE_JOIN_MAX_SLOTS));
        bool foundOffline = false;
        bool foundScan = false;
        for (const auto& e : plan) {
            if (e.kind == JOIN_ACT_INFO_STEAM_OFFLINE) foundOffline = true;
            if (e.kind == JOIN_ACT_INFO_NO_HOSTS) foundScan = true;
        }
        TEST_CHECK(foundOffline && foundScan);
    }

    // 2. localPcHostWaiting == true before UDP probe -> 1 JOIN_ACT_LOCAL_PC_HOST.
    {
        const auto plan = NativeMenu_PlanJoinEntries(true, noHosts, false, noFriends, "127.0.0.1", true);
        TEST_CHECK(AssertJoinPlanInvariants(plan, NATIVE_JOIN_MAX_SLOTS));
        int pcCount = 0;
        for (const auto& e : plan) {
            if (e.kind == JOIN_ACT_LOCAL_PC_HOST) {
                ++pcCount;
                TEST_CHECK(strcmp(e.connectTarget, "127.0.0.1") == 0);
            }
        }
        TEST_CHECK(pcCount == 1);
    }

    // 3. localPcHostWaiting == true WITH sameMachine LanHostEntry -> deduplicated to 1 entry.
    {
        std::vector<LanHostEntry> hosts = {
            MakeHost(11ULL, "192.168.1.10", 5556u, "MyLocalPc", true, true),
            MakeHost(12ULL, "127.0.0.1", 5556u, "LoopbackDup", true, true),
            MakeHost(13ULL, "192.168.1.77", 5577u, "MismatchSrv", false, false, LanAdapterKind::VirtualVpn),
        };
        const auto plan = NativeMenu_PlanJoinEntries(true, hosts, false, noFriends, "127.0.0.1", true);
        TEST_CHECK(AssertJoinPlanInvariants(plan, NATIVE_JOIN_MAX_SLOTS));
        int samePcCount = 0;
        int mismatchCount = 0;
        for (const auto& e : plan) {
            if (e.kind == JOIN_ACT_LOCAL_PC_HOST) ++samePcCount;
            if (e.kind == JOIN_ACT_LAN_HOST && strcmp(e.connectTarget, "127.0.0.1") == 0) ++samePcCount;
            if (e.kind == JOIN_ACT_LAN_HOST && !e.lanHost.protocolMatch) {
                ++mismatchCount;
                TEST_CHECK(e.label.rfind(L"[!ver] ", 0) == 0);
                TEST_CHECK(strcmp(e.connectTarget, "192.168.1.77:5577") == 0);
            }
        }
        TEST_CHECK(samePcCount == 1);
        TEST_CHECK(mismatchCount == 1);
    }

    // 4. UTF-8 Cyrillic, UTF-16 surrogate pair & control character label sanitization safety.
    {
        const LanHostEntry longRu = MakeHost(22ULL, "26.1.2.3", 5556u,
                                             "ОченьДлинноеНазваниеСервераСоветскойРеспублики",
                                             false, true, LanAdapterKind::VirtualVpn);
        const std::wstring lbl = NativeMenu_FormatLanHostLabel(longRu);
        TEST_CHECK(lbl.size() == NATIVE_JOIN_MAX_LABEL_CHARS);
        TEST_CHECK(lbl.rfind(L"[VPN] ", 0) == 0);
        TEST_CHECK(lbl.substr(lbl.size() - 2) == L"..");
        TEST_CHECK(HasNoUnpairedUtf16Surrogates(lbl));

        const char* tenFires =
            "\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5"
            "\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5";
        const std::wstring wideFires = NativeMenu_Utf8ToWideSafe(tenFires);
        TEST_CHECK(wideFires.size() == 20u);
        for (size_t maxLen = 1; maxLen <= 22; ++maxLen) {
            const std::wstring trunc = NativeMenu_TruncateLabel(wideFires, maxLen);
            TEST_CHECK(trunc.size() <= maxLen);
            TEST_CHECK(HasNoUnpairedUtf16Surrogates(trunc));
        }

        const LanHostEntry emojiHost = MakeHost(23ULL, "192.168.1.90", 5556u, tenFires, false, true);
        const std::wstring emojiLbl = NativeMenu_FormatLanHostLabel(emojiHost);
        TEST_CHECK(emojiLbl.size() <= NATIVE_JOIN_MAX_LABEL_CHARS);
        TEST_CHECK(HasNoUnpairedUtf16Surrogates(emojiLbl));

        const LanHostEntry ctrlHost = MakeHost(24ULL, "192.168.1.91", 5556u,
                                               "Bad\r\nHost\tName\x01", false, true);
        const std::wstring ctrlLbl = NativeMenu_FormatLanHostLabel(ctrlHost);
        TEST_CHECK(ctrlLbl.rfind(L"[LAN] ", 0) == 0);
        for (wchar_t ch : ctrlLbl) {
            TEST_CHECK(ch >= 0x20 && ch != 0x7F);
        }
        TEST_CHECK(ctrlLbl.find(L"Bad Host Name") != std::wstring::npos);
    }

    // 5. Overflow: 30 LAN hosts + 6 Steam friends -> clamped to 28 slots with all 5 fixed actions preserved.
    {
        std::vector<LanHostEntry> manyHosts;
        for (uint64_t i = 1; i <= 30; ++i) {
            char ip[16] = {};
            char nm[48] = {};
            _snprintf_s(ip, sizeof(ip), _TRUNCATE, "192.168.1.%u", static_cast<unsigned int>(10 + i));
            _snprintf_s(nm, sizeof(nm), _TRUNCATE, "Host_%02u", static_cast<unsigned int>(i));
            manyHosts.push_back(MakeHost(100ULL + i, ip, 5556u, nm, false, true));
        }
        std::vector<NativeMenuFriendInfo> friends;
        for (uint64_t i = 1; i <= 6; ++i) {
            NativeMenuFriendInfo f = {};
            f.steamId = 76561198000000000ULL + i;
            _snprintf_s(f.name, sizeof(f.name), _TRUNCATE, "Friend%llu", static_cast<unsigned long long>(i));
            f.inThisGame = true;
            f.isHosting = true;
            friends.push_back(f);
        }
        const auto plan = NativeMenu_PlanJoinEntries(false, manyHosts, true, friends, "192.168.1.50", true,
                                                     NATIVE_JOIN_MAX_SLOTS);
        TEST_CHECK(static_cast<int>(plan.size()) == NATIVE_JOIN_MAX_SLOTS);
        TEST_CHECK(AssertJoinPlanInvariants(plan, NATIVE_JOIN_MAX_SLOTS));
        int lanCount = 0;
        for (const auto& e : plan) {
            if (e.kind == JOIN_ACT_LAN_HOST) ++lanCount;
        }
        TEST_CHECK(lanCount == NATIVE_JOIN_MAX_SLOTS - 5);

        const auto tiny = NativeMenu_PlanJoinEntries(false, manyHosts, false, noFriends, "127.0.0.1", true, 3);
        TEST_CHECK(tiny.size() == 3u && tiny.back().kind == JOIN_ACT_BACK);
    }

    // 6. NativeMenu_ResolveJoinClick: snapshot prevents +1 shift and tail shrink bugs.
    {
        const std::vector<LanHostEntry> frameNHosts = {
            MakeHost(101ULL, "192.168.1.10", 5556u, "HostA", false, true),
        };
        const auto snapFrameN = NativeMenu_PlanJoinEntries(false, frameNHosts, false, noFriends, "127.0.0.1", true);
        const int backIdx = static_cast<int>(snapFrameN.size()) - 1;
        TEST_CHECK(snapFrameN[backIdx].kind == JOIN_ACT_BACK);

        // Host B joins asynchronously before Frame N+1 -> new list grows by +1, but click resolves against Frame N snapshot!
        JoinPlanEntry resolved = {};
        TEST_CHECK(NativeMenu_ResolveJoinClick(snapFrameN, backIdx, &resolved));
        TEST_CHECK(resolved.kind == JOIN_ACT_BACK);
        TEST_CHECK(NativeMenu_ResolveJoinClick(snapFrameN, 0, &resolved));
        TEST_CHECK(resolved.kind == JOIN_ACT_LAN_HOST && resolved.lanHost.serverGuid == 101ULL);
        TEST_CHECK(!NativeMenu_ResolveJoinClick(snapFrameN, -1, &resolved));
        TEST_CHECK(!NativeMenu_ResolveJoinClick(snapFrameN, static_cast<int>(snapFrameN.size()), &resolved));
    }

    // 7. NativeMenu_IsLanHostVersionCompatible: protocol, version & non-zero dllCrc checks.
    {
        LanHostEntry compat = MakeHost(201ULL, "192.168.1.20", 5556u, "Compat", false, true);
        strncpy_s(compat.version, sizeof(compat.version), REPUBLICMP_VERSION, _TRUNCATE);
        compat.dllCrc = 0xAABBCCDDu;
        TEST_CHECK(NativeMenu_IsLanHostVersionCompatible(compat, REPUBLICMP_VERSION, 0xAABBCCDDu));
        TEST_CHECK(NativeMenu_IsLanHostVersionCompatible(compat, REPUBLICMP_VERSION, 0u));

        LanHostEntry badProto = compat;
        badProto.protocolMatch = false;
        TEST_CHECK(!NativeMenu_IsLanHostVersionCompatible(badProto, REPUBLICMP_VERSION, 0xAABBCCDDu));

        LanHostEntry badVer = compat;
        strncpy_s(badVer.version, sizeof(badVer.version), "0.0.1", _TRUNCATE);
        TEST_CHECK(!NativeMenu_IsLanHostVersionCompatible(badVer, REPUBLICMP_VERSION, 0xAABBCCDDu));

        LanHostEntry badCrc = compat;
        badCrc.dllCrc = 0x11223344u;
        TEST_CHECK(!NativeMenu_IsLanHostVersionCompatible(badCrc, REPUBLICMP_VERSION, 0xAABBCCDDu));
    }

    // 8. PlanJoinEntries gap coverage: JOIN_ACT_STEAM_FRIEND, IsDuplicateRemoteHost, SelectSamePcHostIndex ties,
    //    sameMachine tcpPort != 5556, and steam: tail label.
    {
        const std::vector<LanHostEntry> fewHosts = {
            MakeHost(301ULL, "192.168.1.30", 5556u, "Remote1", false, true, LanAdapterKind::Ethernet, 1000ULL),
            MakeHost(301ULL, "192.168.1.30", 5556u, "Remote1Dup", false, true, LanAdapterKind::Ethernet, 1200ULL),
            MakeHost(303ULL, "127.0.0.1", 5556u, "LocalBadProto", true, false, LanAdapterKind::Ethernet, 9000ULL),
            MakeHost(304ULL, "127.0.0.1", 5556u, "LocalGoodProto", true, true, LanAdapterKind::Ethernet, 2000ULL),
        };
        NativeMenuFriendInfo fr = {};
        fr.steamId = 76561198000000077ULL;
        strncpy_s(fr.name, sizeof(fr.name), "ComradeSteam", _TRUNCATE);
        fr.inThisGame = true;
        fr.isHosting = true;
        const std::vector<NativeMenuFriendInfo> oneFriend = { fr };

        const auto plan = NativeMenu_PlanJoinEntries(true, fewHosts, true, oneFriend,
                                                     "steam:76561198000000077", true, NATIVE_JOIN_MAX_SLOTS);
        TEST_CHECK(AssertJoinPlanInvariants(plan, NATIVE_JOIN_MAX_SLOTS));
        bool sawSteamFriend = false;
        bool sawSteamTailLabel = false;
        bool sawLocalGoodProto = false;
        int samePcCount = 0;
        int remoteCount = 0;
        for (const auto& e : plan) {
            if (e.kind == JOIN_ACT_STEAM_FRIEND && e.steamId == fr.steamId) {
                sawSteamFriend = true;
            }
            if (e.kind == JOIN_ACT_CONNECT_ADDR && e.label == L"Join via SteamID") {
                sawSteamTailLabel = true;
            }
            if (e.kind == JOIN_ACT_LOCAL_PC_HOST ||
                (e.kind == JOIN_ACT_LAN_HOST && e.lanHost.sameMachine)) {
                ++samePcCount;
                if (e.kind == JOIN_ACT_LAN_HOST && e.lanHost.serverGuid == 304ULL) {
                    sawLocalGoodProto = true;
                }
            } else if (e.kind == JOIN_ACT_LAN_HOST && e.lanHost.serverGuid == 301ULL) {
                ++remoteCount;
            }
        }
        TEST_CHECK(sawSteamFriend && sawSteamTailLabel && sawLocalGoodProto);
        TEST_CHECK(samePcCount == 1);
        TEST_CHECK(remoteCount == 1); // duplicate serverGuid/ip collapsed by IsDuplicateRemoteHost

        // Same-PC host on custom tcpPort != 5556 maps its non-loopback LAN IP to 127.0.0.1:<port>
        const std::vector<LanHostEntry> port5577Only = {
            MakeHost(302ULL, "192.168.1.31", 5577u, "LocalPort5577", true, true, LanAdapterKind::Ethernet, 1500ULL),
        };
        const auto plan5577 = NativeMenu_PlanJoinEntries(false, port5577Only, false, {},
                                                         "127.0.0.1", true, NATIVE_JOIN_MAX_SLOTS);
        TEST_CHECK(AssertJoinPlanInvariants(plan5577, NATIVE_JOIN_MAX_SLOTS));
        bool sawPort5577Loopback = false;
        for (const auto& e : plan5577) {
            if (e.kind == JOIN_ACT_LAN_HOST && e.lanHost.serverGuid == 302ULL &&
                strcmp(e.connectTarget, "127.0.0.1:5577") == 0) {
                sawPort5577Loopback = true;
            }
        }
        TEST_CHECK(sawPort5577Loopback);
    }

    return true;
}

static bool SubTest_M3_F11_LiveLoopbackDiscoveryToJoinPlanAndManualAddr()
{
    const uint16_t udpPort = M3AllocLoopbackUdpPort();
    TEST_CHECK(udpPort != 0);
    LanDiscovery_SetPortForTest(udpPort);
    LanDiscovery_SetSelfPidOverrideForTest(GetCurrentProcessId() ^ 0xA301u, true);
    LanDiscovery_Init();
    LanDiscovery_SetHosting(true, 5556u, 1u, "M3LiveHost", "M3Map");
    LanDiscovery_StartScan();
    LanDiscovery_ProbeNow();

    bool found = false;
    std::vector<LanHostEntry> hosts;
    const ULONGLONG deadline = GetTickCount64() + 3500ULL;
    ULONGLONG lastProbeMs = GetTickCount64();
    while (GetTickCount64() < deadline) {
        hosts = LanDiscovery_GetHosts();
        for (const auto& h : hosts) {
            if (strcmp(h.hostName, "M3LiveHost") == 0) {
                found = true;
                break;
            }
        }
        if (found) break;
        const ULONGLONG nowMs = GetTickCount64();
        if ((nowMs - lastProbeMs) >= 250ULL) {
            LanDiscovery_ProbeNow();
            lastProbeMs = nowMs;
        }
        Sleep(25);
    }
    TEST_CHECK(found);

    const std::vector<NativeMenuFriendInfo> noFriends;
    const auto plan = NativeMenu_PlanJoinEntries(true, hosts, false, noFriends, "127.0.0.1", true,
                                                 NATIVE_JOIN_MAX_SLOTS);
    TEST_CHECK(AssertJoinPlanInvariants(plan, NATIVE_JOIN_MAX_SLOTS));
    int liveRows = 0;
    for (const auto& e : plan) {
        if (e.kind == JOIN_ACT_LAN_HOST && strcmp(e.lanHost.hostName, "M3LiveHost") == 0) {
            ++liveRows;
            TEST_CHECK(strcmp(e.connectTarget, "127.0.0.1") == 0);
        }
    }
    TEST_CHECK(liveRows == 1);

    LanDiscovery_StopScan();
    LanDiscovery_SetHosting(false, 0, 0, nullptr, nullptr);
    LanDiscovery_Shutdown();

    // Manual join address normalization matrix.
    char outHost[64] = {};
    uint16_t outPort = 0;
    TEST_CHECK(NetworkNormalizeTargetAddress("127.0.0.1", outHost, sizeof(outHost), &outPort));
    TEST_CHECK(strcmp(outHost, "127.0.0.1") == 0 && outPort == 0);
    TEST_CHECK(NetworkNormalizeTargetAddress(" 192.168.1.50:5556 ", outHost, sizeof(outHost), &outPort));
    TEST_CHECK(strcmp(outHost, "192.168.1.50") == 0 && outPort == 5556u);
    TEST_CHECK(NetworkNormalizeTargetAddress("steam:76561198012345678", outHost, sizeof(outHost), &outPort));
    TEST_CHECK(strcmp(outHost, "steam:76561198012345678") == 0);
    TEST_CHECK(!NetworkNormalizeTargetAddress("", outHost, sizeof(outHost), &outPort));
    TEST_CHECK(!NetworkNormalizeTargetAddress("999.1.1.1", outHost, sizeof(outHost), &outPort));
    TEST_CHECK(!NetworkNormalizeTargetAddress("192.168.1.1:0", outHost, sizeof(outHost), &outPort));
    TEST_CHECK(!NetworkNormalizeTargetAddress("invalid_garbage", outHost, sizeof(outHost), &outPort));
    return true;
}

static int RenderSyncBlockAtSizeAndCountVerts(float w, float h)
{
    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* texPixels = nullptr;
    int texW = 0, texH = 0;
    io.Fonts->GetTexDataAsRGBA32(&texPixels, &texW, &texH);

    // Run 2 frames so ImGuiWindowFlags_AlwaysAutoResize windows populate beyond Frame 1 auto-fit skip
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        SyncBlock_Draw();
        ImGui::Render();
    }

    int totalVtx = 0;
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd) totalVtx = dd->TotalVtxCount;
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prevCtx);
    return totalVtx;
}

static bool SubTest_M3_SyncBlockHeadlessDrawAndStateTransitions()
{
    if (g_mockExeBuffer) {
        *(int64_t*)(g_mockExeBuffer + 0xA2E758) = 0x1000LL;
        *(volatile char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    }

    // Atomic SyncBlock state transitions
    SyncBlock_SetActive(false, nullptr);
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    SyncBlock_SetMapInspectMode(true);
    TEST_CHECK(SyncBlock_IsMapInspectMode());
    TEST_CHECK(!SyncBlock_BlocksWorldInput());

    SyncBlock_SetActive(true, "Syncing test");
    TEST_CHECK(SyncBlock_IsActive());
    TEST_CHECK(!SyncBlock_IsMapInspectMode());
    TEST_CHECK(SyncBlock_BlocksWorldInput());

    TEST_CHECK(!SyncBlock_UpdateMapInspectState(SESSION_IDLE, false));
    SyncBlock_SetActive(false, nullptr);

    const float widths[2] = { 1280.0f, 2560.0f };
    const float heights[2] = { 720.0f, 1440.0f };
    for (int langIdx = 0; langIdx <= 1; ++langIdx) {
        Lang_Set(langIdx == 0 ? MP_LANG_RU : MP_LANG_EN);
        for (int res = 0; res < 2; ++res) {
            // 1. SESSION_HOST_LISTENING (full waiting lobby)
            SyncBlock_SetActive(false, nullptr);
            SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
            SyncBlock_SetMapInspectMode(false);
            TEST_CHECK(RenderSyncBlockAtSizeAndCountVerts(widths[res], heights[res]) > 0);

            // 2. SESSION_HOST_LISTENING + isReconnect + MapInspect banner
            SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, true, "192.168.1.99");
            SyncBlock_SetMapInspectMode(true);
            TEST_CHECK(RenderSyncBlockAtSizeAndCountVerts(widths[res], heights[res]) > 0);
            SyncBlock_SetMapInspectMode(false);

            // 3. SESSION_HOST_JOINING
            SaveSync_SetTestSessionState(SESSION_HOST_JOINING, false, "");
            SyncBlock_SetActive(true, "Saving world...");
            SyncBlock_SetStep(2, 7, "Saving world...", 40);
            TEST_CHECK(RenderSyncBlockAtSizeAndCountVerts(widths[res], heights[res]) > 0);

            // 4. SESSION_CLIENT_JOINING
            SaveSync_SetTestSessionState(SESSION_CLIENT_JOINING, false, "");
            SyncBlock_SetStep(4, 7, "Receiving world...", 75);
            TEST_CHECK(RenderSyncBlockAtSizeAndCountVerts(widths[res], heights[res]) > 0);
        }
    }

    SyncBlock_SetActive(false, nullptr);
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    if (g_mockExeBuffer) {
        *(int64_t*)(g_mockExeBuffer + 0xA2E758) = 0;
        *(volatile char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    }
    return true;
}

} // namespace

static bool Test_LanUiBrowserHostIpAndToasts_M3()
{
    M3TestEnvGuard guard;

    printf("    [M3-F9-1: Shared Clipboard UTF-8/ANSI & Normalization] ...\n"); fflush(stdout);
    if (!SubTest_M3_F9_ClipboardSharedAndUtf8()) return false;

    printf("    [M3-F9-2: Toast ColorKind 0..5, OnSessionEnded Preserve & SESSION_IDLE Render] ...\n"); fflush(stdout);
    if (!SubTest_M3_F9_ToastNormalizeSessionEndedAndIdleRender()) return false;

    printf("    [M3-F10: Multi-Adapter Host IPv4, VPN Tags & Steam-Offline Summary] ...\n"); fflush(stdout);
    if (!SubTest_M3_F10_MultiAdapterHostViewAndTags()) return false;

    printf("    [M3-F11-1: Join Browser Slot Budget, 127.0.0.1 Dedup & [!ver] Labels] ...\n"); fflush(stdout);
    if (!SubTest_M3_F11_JoinPlannerSlotsDedupAndErrors()) return false;

    printf("    [M3-F11-2: Live Loopback LanDiscovery -> Join Plan & Manual Addr Matrix] ...\n"); fflush(stdout);
    if (!SubTest_M3_F11_LiveLoopbackDiscoveryToJoinPlanAndManualAddr()) return false;

    printf("    [M3-SyncBlock: Headless Draw (1280x720 & 2560x1440 RU/EN) & Atomic State] ...\n"); fflush(stdout);
    if (!SubTest_M3_SyncBlockHeadlessDrawAndStateTransitions()) return false;

    return true;
}
