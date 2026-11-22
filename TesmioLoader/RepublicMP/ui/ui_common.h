// SPDX-License-Identifier: GPL-3.0-only
// Shared UI utilities: Win32 clipboard (UTF-16 + ANSI), toast normalization,
// and host IPv4 / multi-adapter formatting helpers.
// Pure C++/POD header: intentionally avoids <windows.h>, <winsock2.h>, and steam_p2p.h.
#pragma once

#include "../transport/lan_discovery.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

struct UiHostAddressView {
    std::string primaryIp;
    std::string primaryEndpoint;
    std::string primaryCopyText;
    std::string primaryTag;
    bool hasLanAdapter = false;
    std::vector<LanAdapterInfo> displayAdapters;
};

// Shared Win32 clipboard helpers (non-inline; external linkage in ui_common.cpp).
bool UiCommon_CopyTextToClipboard(const char* text);
bool UiCommon_ReadTextFromClipboard(std::string& outText);

// Non-blocking UI-side adapter cache (queries LanDiscovery_GetLocalAdapters at most
// once every 5000 ms unless forceRefresh is true).
std::vector<LanAdapterInfo> UiCommon_GetCachedLocalAdapters(bool forceRefresh = false);
void UiCommon_RefreshLocalAdaptersCache();

// Pure validation & normalization helpers.
int UiCommon_NormalizeToastColorKind(int colorKind);
bool UiCommon_NormalizeClipboardJoinAddress(const char* raw, char* outBuf, size_t outCap);

// Pure host IPv4 & multi-adapter display/copy formatting helpers.
const char* UiCommon_AdapterKindTag(LanAdapterKind kind, const char* adapterName, const char* ip, bool russian);
const LanAdapterInfo* UiCommon_SelectPrimaryAdapter(const std::vector<LanAdapterInfo>& adapters);
std::string UiCommon_FormatHostEndpoint(const char* ip, uint16_t tcpPort);
std::string UiCommon_FormatCopyableHostIp(const char* ip, uint16_t tcpPort, uint16_t defaultPort = 5556);
std::string UiCommon_FormatAdapterLine(const LanAdapterInfo& adapter, uint16_t tcpPort,
                                       bool includeName = true, bool russian = false);
UiHostAddressView UiCommon_BuildHostAddressView(const std::vector<LanAdapterInfo>& adapters,
                                                uint16_t tcpPort, uint16_t defaultPort = 5556,
                                                bool russian = false);
std::string UiCommon_FormatHostTransportSummary(const char* primaryEndpoint, bool steamAvailable,
                                                uint64_t steamId, bool russian);
