// SPDX-License-Identifier: GPL-3.0-only
#include "ui_common.h"
#include "../transport/network.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

static HGLOBAL AllocClipboardBlock(const void* data, size_t bytes)
{
    if (!data || bytes == 0) return nullptr;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) return nullptr;
    void* dst = GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        return nullptr;
    }
    memcpy(dst, data, bytes);
    GlobalUnlock(hMem);
    return hMem;
}

static bool ContainsAsciiInsensitive(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !needle[0]) return false;
    const size_t nLen = strlen(needle);
    for (const char* p = haystack; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < nLen && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) ==
                   std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nLen) return true;
    }
    return false;
}

static bool StartsWithPrefix(const char* str, const char* prefix)
{
    if (!str || !prefix) return false;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static bool IsTailscaleCgnatIp(const char* ip)
{
    if (!ip || !StartsWithPrefix(ip, "100.")) return false;
    int a = 0, b = 0, c = 0, d = 0;
    if (sscanf_s(ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    return a == 100 && b >= 64 && b <= 127;
}

static bool TryOpenClipboardWithRetry(int maxAttempts = 6, DWORD delayMs = 4)
{
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        if (attempt + 1 < maxAttempts && delayMs > 0) {
            Sleep(delayMs);
        }
    }
    return false;
}

bool UiCommon_CopyTextToClipboard(const char* text)
{
    if (!text || !text[0]) return false;
    const size_t utf8Len = strlen(text);
    if (utf8Len > (1u << 20)) return false;

    std::vector<wchar_t> wideBuf;
    const int wideChars = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wideChars > 0) {
        wideBuf.resize(static_cast<size_t>(wideChars));
        if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wideBuf.data(), wideChars) <= 0) {
            wideBuf.clear();
        }
    }

    HGLOBAL hWide = !wideBuf.empty()
        ? AllocClipboardBlock(wideBuf.data(), wideBuf.size() * sizeof(wchar_t))
        : nullptr;
    HGLOBAL hAnsi = AllocClipboardBlock(text, utf8Len + 1);
    if (!hWide && !hAnsi) return false;

    if (!TryOpenClipboardWithRetry()) {
        if (hWide) GlobalFree(hWide);
        if (hAnsi) GlobalFree(hAnsi);
        return false;
    }

    EmptyClipboard();
    bool anySet = false;
    if (hWide) {
        if (SetClipboardData(CF_UNICODETEXT, hWide) != nullptr) {
            anySet = true;
        } else {
            GlobalFree(hWide);
        }
    }
    if (hAnsi) {
        if (SetClipboardData(CF_TEXT, hAnsi) != nullptr) {
            anySet = true;
        } else {
            GlobalFree(hAnsi);
        }
    }
    CloseClipboard();
    return anySet;
}

bool UiCommon_ReadTextFromClipboard(std::string& outText)
{
    outText.clear();
    if (!TryOpenClipboardWithRetry()) return false;

    HANDLE hUnicode = GetClipboardData(CF_UNICODETEXT);
    if (hUnicode) {
        const wchar_t* wstr = static_cast<const wchar_t*>(GlobalLock(hUnicode));
        if (wstr) {
            const SIZE_T rawBytes = GlobalSize(hUnicode);
            const size_t maxWchars = std::min<size_t>(rawBytes / sizeof(wchar_t), (1u << 20));
            const size_t wcharLen = wcsnlen(wstr, maxWchars);
            if (wcharLen > 0) {
                const int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(wcharLen),
                                                          nullptr, 0, nullptr, nullptr);
                if (utf8Bytes > 0 && utf8Bytes <= (1 << 20)) {
                    std::string utf8(static_cast<size_t>(utf8Bytes), '\0');
                    if (WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(wcharLen),
                                            &utf8[0], utf8Bytes, nullptr, nullptr) > 0) {
                        outText = utf8;
                    }
                }
            }
            GlobalUnlock(hUnicode);
        }
    }

    if (outText.empty()) {
        HANDLE hAnsi = GetClipboardData(CF_TEXT);
        if (hAnsi) {
            const char* astr = static_cast<const char*>(GlobalLock(hAnsi));
            if (astr) {
                const size_t len = strnlen(astr, (1u << 20));
                if (len > 0) {
                    outText.assign(astr, len);
                }
                GlobalUnlock(hAnsi);
            }
        }
    }

    CloseClipboard();
    return !outText.empty();
}

std::vector<LanAdapterInfo> UiCommon_GetCachedLocalAdapters(bool forceRefresh)
{
    static std::mutex s_cacheMutex;
    static std::vector<LanAdapterInfo> s_adapters;
    static ULONGLONG s_lastRefreshMs = 0;
    static bool s_initialized = false;

    std::lock_guard<std::mutex> lock(s_cacheMutex);
    const ULONGLONG now = GetTickCount64();
    if (forceRefresh || !s_initialized || (now - s_lastRefreshMs) >= 5000ULL) {
        s_adapters = LanDiscovery_GetLocalAdapters(forceRefresh);
        s_lastRefreshMs = now;
        s_initialized = true;
    }
    return s_adapters;
}

void UiCommon_RefreshLocalAdaptersCache()
{
    (void)UiCommon_GetCachedLocalAdapters(true);
}

int UiCommon_NormalizeToastColorKind(int colorKind)
{
    if (colorKind < 0) return 0;
    if (colorKind > 5) return 5;
    return colorKind;
}

bool UiCommon_NormalizeClipboardJoinAddress(const char* raw, char* outBuf, size_t outCap)
{
    if (outBuf && outCap > 0) outBuf[0] = '\0';
    if (!raw || !raw[0] || !outBuf || outCap == 0) return false;

    const char* start = raw;
    while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start)) != 0) {
        ++start;
    }
    size_t len = strlen(start);
    while (len > 0 && std::isspace(static_cast<unsigned char>(start[len - 1])) != 0) {
        --len;
    }
    if (len == 0) return false;

    const std::string trimmed(start, len);
    bool allDigits = true;
    for (char ch : trimmed) {
        if (ch < '0' || ch > '9') {
            allDigits = false;
            break;
        }
    }

    std::string candidate = trimmed;
    if (allDigits) {
        if (!StartsWithPrefix(trimmed.c_str(), "7656") ||
            trimmed.size() < 17u || trimmed.size() > 20u) {
            return false;
        }
        errno = 0;
        char* endPtr = nullptr;
        const unsigned long long sid = strtoull(trimmed.c_str(), &endPtr, 10);
        if (errno != 0 || !endPtr || *endPtr != '\0' || sid < 76561197960265728ULL) {
            return false;
        }
        candidate = "steam:" + trimmed;
    }

    char normHost[64] = {};
    uint16_t parsedPort = 0;
    if (!NetworkNormalizeTargetAddress(candidate.c_str(), normHost, sizeof(normHost), &parsedPort)) {
        return false;
    }

    std::string finalStr(normHost);
    if (parsedPort != 0 && _strnicmp(normHost, "steam:", 6) != 0) {
        finalStr += ":" + std::to_string(static_cast<unsigned int>(parsedPort));
    }

    if (finalStr.empty() || finalStr.size() + 1u > outCap) {
        outBuf[0] = '\0';
        return false;
    }
    memcpy(outBuf, finalStr.c_str(), finalStr.size() + 1u);
    return true;
}

const char* UiCommon_AdapterKindTag(LanAdapterKind kind, const char* adapterName, const char* ip, bool russian)
{
    if (kind == LanAdapterKind::Ethernet) return "Ethernet";
    if (kind == LanAdapterKind::WiFi) return "Wi-Fi";
    if (kind == LanAdapterKind::VirtualLocal) {
        return russian ? "Вирт. локал." : "Virtual Local";
    }
    if (kind == LanAdapterKind::VirtualVpn) {
        if (ContainsAsciiInsensitive(adapterName, "radmin") || StartsWithPrefix(ip, "26.")) return "Radmin VPN";
        if (ContainsAsciiInsensitive(adapterName, "hamachi") || StartsWithPrefix(ip, "25.")) return "Hamachi";
        if (ContainsAsciiInsensitive(adapterName, "zerotier") || StartsWithPrefix(ip, "10.147.")) return "ZeroTier";
        if (ContainsAsciiInsensitive(adapterName, "tailscale") || IsTailscaleCgnatIp(ip)) return "Tailscale";
        if (ContainsAsciiInsensitive(adapterName, "wireguard")) return "WireGuard";
        if (ContainsAsciiInsensitive(adapterName, "openvpn")) return "OpenVPN";
        return "VPN";
    }
    return russian ? "Сеть" : "Network";
}

static bool IsAdapterBetter(const LanAdapterInfo& a, const LanAdapterInfo& b, bool hasNonVirtualLocal)
{
    if (hasNonVirtualLocal) {
        const bool aLocal = (a.kind == LanAdapterKind::VirtualLocal);
        const bool bLocal = (b.kind == LanAdapterKind::VirtualLocal);
        if (aLocal != bLocal) return !aLocal;
    }
    if (a.priority != b.priority) return a.priority > b.priority;
    if (a.ifIndex != b.ifIndex) return a.ifIndex < b.ifIndex;
    return strcmp(a.ip, b.ip) < 0;
}

const LanAdapterInfo* UiCommon_SelectPrimaryAdapter(const std::vector<LanAdapterInfo>& adapters)
{
    bool hasNonVirtualLocal = false;
    for (const auto& ad : adapters) {
        if (ad.ip[0] != '\0' && ad.kind != LanAdapterKind::VirtualLocal) {
            hasNonVirtualLocal = true;
            break;
        }
    }

    const LanAdapterInfo* best = nullptr;
    for (const auto& ad : adapters) {
        if (ad.ip[0] == '\0') continue;
        if (!best || IsAdapterBetter(ad, *best, hasNonVirtualLocal)) {
            best = &ad;
        }
    }
    return best;
}

std::string UiCommon_FormatHostEndpoint(const char* ip, uint16_t tcpPort)
{
    if (!ip || !ip[0]) return std::string();
    if (tcpPort == 0) return std::string(ip);
    char buf[48] = {};
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s:%u", ip, static_cast<unsigned int>(tcpPort));
    return std::string(buf);
}

std::string UiCommon_FormatCopyableHostIp(const char* ip, uint16_t tcpPort, uint16_t defaultPort)
{
    if (!ip || !ip[0]) return std::string();
    if (tcpPort == 0 || tcpPort == defaultPort) return std::string(ip);
    return UiCommon_FormatHostEndpoint(ip, tcpPort);
}

static std::string SanitizeAndTrimAdapterName(const char* rawName)
{
    if (!rawName || !rawName[0]) return std::string();
    std::string nm(rawName);
    for (char& ch : nm) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 0x20u || uch == 0x7Fu) {
            ch = ' ';
        }
    }
    while (!nm.empty() && nm.front() == ' ') nm.erase(nm.begin());
    while (!nm.empty() && nm.back() == ' ') nm.pop_back();
    if (nm.size() > 26u) {
        size_t cut = 24u;
        while (cut > 0 && (static_cast<unsigned char>(nm[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        while (cut > 0 && static_cast<unsigned char>(nm[cut - 1]) >= 0xC0u) {
            --cut;
        }
        nm.resize(cut);
        nm += "..";
    }
    return nm;
}

std::string UiCommon_FormatAdapterLine(const LanAdapterInfo& adapter, uint16_t tcpPort,
                                       bool includeName, bool russian)
{
    const char* tag = UiCommon_AdapterKindTag(adapter.kind, adapter.name, adapter.ip, russian);
    const std::string ep = UiCommon_FormatHostEndpoint(adapter.ip, tcpPort);
    if (ep.empty()) return std::string();

    if (includeName && adapter.name[0] != '\0') {
        const std::string nm = SanitizeAndTrimAdapterName(adapter.name);
        if (!nm.empty()) {
            return std::string("[") + tag + "] " + ep + " (" + nm + ")";
        }
    }
    return std::string("[") + tag + "] " + ep;
}

UiHostAddressView UiCommon_BuildHostAddressView(const std::vector<LanAdapterInfo>& adapters,
                                                uint16_t tcpPort, uint16_t defaultPort,
                                                bool russian)
{
    UiHostAddressView view;
    std::vector<LanAdapterInfo> sorted;
    sorted.reserve(adapters.size());
    bool hasNonVirtualLocal = false;
    for (const auto& ad : adapters) {
        if (ad.ip[0] == '\0') continue;
        if (ad.kind != LanAdapterKind::VirtualLocal) hasNonVirtualLocal = true;
        sorted.push_back(ad);
    }

    std::sort(sorted.begin(), sorted.end(),
              [hasNonVirtualLocal](const LanAdapterInfo& a, const LanAdapterInfo& b) {
                  return IsAdapterBetter(a, b, hasNonVirtualLocal);
              });

    for (const auto& ad : sorted) {
        bool duplicate = false;
        for (const auto& ex : view.displayAdapters) {
            if (strcmp(ex.ip, ad.ip) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            view.displayAdapters.push_back(ad);
        }
    }

    const LanAdapterInfo* primary = UiCommon_SelectPrimaryAdapter(view.displayAdapters);
    if (primary) {
        view.hasLanAdapter = true;
        view.primaryIp = primary->ip;
        view.primaryEndpoint = UiCommon_FormatHostEndpoint(primary->ip, tcpPort);
        view.primaryCopyText = UiCommon_FormatCopyableHostIp(primary->ip, tcpPort, defaultPort);
        view.primaryTag = UiCommon_AdapterKindTag(primary->kind, primary->name, primary->ip, russian);
    } else {
        view.hasLanAdapter = false;
        view.primaryIp = "127.0.0.1";
        view.primaryEndpoint = UiCommon_FormatHostEndpoint("127.0.0.1", tcpPort);
        view.primaryCopyText = UiCommon_FormatCopyableHostIp("127.0.0.1", tcpPort, defaultPort);
        view.primaryTag = russian ? "Локальный ПК" : "Local PC";
    }
    return view;
}

std::string UiCommon_FormatHostTransportSummary(const char* primaryEndpoint, bool steamAvailable,
                                                uint64_t steamId, bool russian)
{
    const char* ep = (primaryEndpoint && primaryEndpoint[0]) ? primaryEndpoint : "127.0.0.1:5556";
    if (steamAvailable && steamId != 0) {
        char buf[96] = {};
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "LAN: %s | SteamID: %llu",
                    ep, static_cast<unsigned long long>(steamId));
        return std::string(buf);
    }
    return std::string("LAN: ") + ep + (russian ? " | Steam: офлайн (режим LAN)" : " | Steam: Offline (LAN mode)");
}
