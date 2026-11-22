// SPDX-License-Identifier: GPL-3.0-only
// See network.h.
#include "network.h"
#include "steam_p2p.h"
#include "lan_discovery.h"
#include <cstdio>
#include <mstcpip.h> // SIO_KEEPALIVE_VALS
#include <algorithm> // std::min - NOMINMAX is defined project-wide, so windows.h's min/max macros are gone
#include <atomic>
#include <filesystem>
#include <string>

#pragma comment(lib, "ws2_32.lib")

static std::atomic<bool> g_networkLoopbackOnlyForTest{false};

void Network_SetLoopbackOnlyForTest(bool enabled)
{
    g_networkLoopbackOnlyForTest.store(enabled);
}

void Network_Bind(const TsmHost* host)
{
    TsmBind(host);
    SteamP2P_Bind(host);
    LanDiscovery_Bind(host);
}

bool NetworkInit()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        Logf("republicmp  NETWORK: WSAStartup failed, code=%d", result);
        return false;
    }
    Logf("republicmp  NETWORK: WinSock initialized (%d.%d), SteamP2P=lazy-init",
         LOBYTE(wsaData.wVersion), HIBYTE(wsaData.wVersion));
    return true;
}

// Windows SO_RCVTIMEO/SO_SNDTIMEO take a DWORD (ms), not a struct timeval.
static bool NetworkSetSocketTimeouts(SOCKET sock, DWORD timeoutMs)
{
    if (sock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: NetworkSetSocketTimeouts() got INVALID_SOCKET");
        return false;
    }
    if (SteamP2P_IsSteamSocket(sock)) return true;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: setsockopt(SO_RCVTIMEO) failed, WSAGetLastError=%d", WSAGetLastError());
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: setsockopt(SO_SNDTIMEO) failed, WSAGetLastError=%d", WSAGetLastError());
        return false;
    }
    return true;
}

// TCP keepalive: the join connection sits in a blocking recv for minutes with
// nothing to read (host saving, client loading), so a silently dead link (VPN
// drop, crashed peer machine) would never be noticed. Probes after 5s idle,
// every 1s; Windows gives up after 10 unanswered ones -> ~15s to an error.
static void NetworkEnableKeepalive(SOCKET sock)
{
    if (SteamP2P_IsSteamSocket(sock)) return;
    tcp_keepalive ka = {};
    ka.onoff = 1;
    ka.keepalivetime = 5000;
    ka.keepaliveinterval = 1000;
    DWORD returned = 0;
    if (WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR)
        Logf("republicmp  NETWORK: SIO_KEEPALIVE_VALS failed, WSAGetLastError=%d", WSAGetLastError());
}

SOCKET NetworkHostAcceptOne(unsigned short port, DWORD acceptTimeoutMs, DWORD ioTimeoutMs, volatile bool* cancelFlag)
{
    SteamP2P_PrepareAccept(port);

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock != INVALID_SOCKET) {
        BOOL reuse = TRUE;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(g_networkLoopbackOnlyForTest.load() ? INADDR_LOOPBACK : INADDR_ANY);
        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
            listen(listenSock, 1) == SOCKET_ERROR) {
            Logf("republicmp  NETWORK: TCP bind/listen(:%u) failed, WSAGetLastError=%d", port, WSAGetLastError());
            closesocket(listenSock);
            listenSock = INVALID_SOCKET;
        }
    }

    if (listenSock == INVALID_SOCKET && !SteamP2P_IsAvailable()) {
        return INVALID_SOCKET;
    }

    if (acceptTimeoutMs == 0)
        Logf("republicmp  NETWORK: listening on :%u (TCP + Steam P2P), waiting for a client (no timeout)...", port);
    else
        Logf("republicmp  NETWORK: listening on :%u (TCP + Steam P2P), waiting for a client (timeout %u ms)...", port, acceptTimeoutMs);

    // accept() ignores SO_RCVTIMEO: wait with select() in short steps so the
    // cancel flag and Steam P2P incoming queue are checked between them.
    const DWORD pollStepMs = 100;
    DWORD waited = 0;
    for (;;) {
        if (cancelFlag && *cancelFlag) {
            Logf("republicmp  NETWORK: accept on :%u cancelled (no client yet)", port);
            if (listenSock != INVALID_SOCKET) closesocket(listenSock);
            return INVALID_SOCKET;
        }

        SOCKET steamSock = SteamP2P_TryAcceptOne(port, ioTimeoutMs);
        if (steamSock != INVALID_SOCKET) {
            if (listenSock != INVALID_SOCKET) closesocket(listenSock);
            return steamSock;
        }

        if (listenSock != INVALID_SOCKET) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSock, &readSet);
            timeval stepTimeout;
            stepTimeout.tv_sec = pollStepMs / 1000;
            stepTimeout.tv_usec = (pollStepMs % 1000) * 1000;

            int selectResult = select(0, &readSet, nullptr, nullptr, &stepTimeout);
            if (selectResult == SOCKET_ERROR) {
                Logf("republicmp  NETWORK: select() failed, WSAGetLastError=%d", WSAGetLastError());
                closesocket(listenSock);
                listenSock = INVALID_SOCKET;
                if (!SteamP2P_IsAvailable()) {
                    return INVALID_SOCKET;
                }
            } else if (selectResult > 0) {
                break; // a TCP client is waiting - fall through to accept() below
            }
        } else {
            if (!SteamP2P_IsAvailable()) {
                return INVALID_SOCKET;
            }
            Sleep(pollStepMs);
        }

        if (acceptTimeoutMs != 0) {
            waited += pollStepMs;
            if (waited >= acceptTimeoutMs) {
                Logf("republicmp  NETWORK: TIMEOUT - no client after %u ms", acceptTimeoutMs);
                if (listenSock != INVALID_SOCKET) closesocket(listenSock);
                return INVALID_SOCKET;
            }
        }
    }

    sockaddr_in clientAddr = {};
    int clientAddrLen = sizeof(clientAddr);
    SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientAddrLen);

    closesocket(listenSock);

    if (clientSock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: accept() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    char clientIp[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
    Logf("republicmp  NETWORK: client connected, %s:%u", clientIp, ntohs(clientAddr.sin_port));

    NetworkSetSocketTimeouts(clientSock, ioTimeoutMs);
    NetworkEnableKeepalive(clientSock);
    return clientSock;
}

static bool IsAsciiWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static std::string TrimTargetAddress(const char* raw)
{
    if (!raw) return std::string();
    const char* start = raw;
    while (*start != '\0' && IsAsciiWhitespace(*start)) {
        ++start;
    }
    const char* end = start + strlen(start);
    while (end > start && IsAsciiWhitespace(end[-1])) {
        --end;
    }
    return std::string(start, (size_t)(end - start));
}

static bool ParseStrictPort1To65535(const std::string& portStr, uint16_t* outPort)
{
    if (portStr.empty()) return false;
    for (char c : portStr) {
        if (c < '0' || c > '9') return false;
    }
    size_t firstNonZero = 0;
    while (firstNonZero < portStr.size() && portStr[firstNonZero] == '0') {
        ++firstNonZero;
    }
    if (firstNonZero == portStr.size()) return false;
    if ((portStr.size() - firstNonZero) > 5u) return false;
    unsigned long val = 0;
    for (size_t i = firstNonZero; i < portStr.size(); ++i) {
        val = val * 10ul + (unsigned long)(portStr[i] - '0');
    }
    if (val < 1ul || val > 65535ul) return false;
    if (outPort) *outPort = (uint16_t)val;
    return true;
}

static bool TryCopyCheckedHost(const char* src, char* outHost, size_t hostCap)
{
    if (!src || !outHost || hostCap == 0) return false;
    size_t len = strlen(src);
    if (len == 0 || len >= hostCap) return false;
    memcpy(outHost, src, len + 1);
    return true;
}

static bool TryParseStrictUint64Digits(const char* s, uint64_t* outVal)
{
    if (!s || s[0] < '0' || s[0] > '9') return false;
    uint64_t val = 0;
    size_t count = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        ++count;
        if (count > 20u) return false;
        uint64_t d = (uint64_t)(*p - '0');
        if (val > (UINT64_MAX - d) / 10ULL) {
            return false;
        }
        val = val * 10ULL + d;
    }
    if (count < 15u) return false;
    if (outVal) *outVal = val;
    return true;
}

static bool TryNormalizeSteamTarget(const std::string& trimmed, char* outHost, size_t hostCap, bool* outHandled)
{
    *outHandled = false;
    bool hasSteamPrefix = (_strnicmp(trimmed.c_str(), "steam:", 6) == 0);
    bool allDigits = !trimmed.empty();
    for (char c : trimmed) {
        if (c < '0' || c > '9') {
            allDigits = false;
            break;
        }
    }

    if (!hasSteamPrefix && !(allDigits && trimmed.size() >= 15u)) {
        return true;
    }

    *outHandled = true;
    const char* digitSeq = hasSteamPrefix ? (trimmed.c_str() + 6) : trimmed.c_str();
    uint64_t parsedStrict = 0;
    if (!TryParseStrictUint64Digits(digitSeq, &parsedStrict)) {
        return false;
    }
    uint64_t sid = SteamP2P_ParseTargetSteamId(trimmed.c_str());
    if (sid == 0 || sid != parsedStrict) return false;

    if (hasSteamPrefix) {
        char canon[64] = {};
        _snprintf_s(canon, sizeof(canon), _TRUNCATE, "steam:%llu", (unsigned long long)sid);
        return TryCopyCheckedHost(canon, outHost, hostCap);
    }
    return TryCopyCheckedHost(trimmed.c_str(), outHost, hostCap);
}

bool NetworkNormalizeTargetAddress(const char* rawInput, char* outHost, size_t hostCap, uint16_t* outPort)
{
    if (outHost && hostCap > 0) outHost[0] = '\0';
    if (!rawInput || !outHost || hostCap == 0) return false;

    std::string trimmed = TrimTargetAddress(rawInput);
    if (trimmed.empty()) return false;

    for (char c : trimmed) {
        if (c == '[' || c == ']' || IsAsciiWhitespace(c)) {
            return false;
        }
    }

    bool handledSteam = false;
    if (!TryNormalizeSteamTarget(trimmed, outHost, hostCap, &handledSteam)) {
        return false;
    }
    if (handledSteam) {
        return true;
    }

    size_t colonPos = std::string::npos;
    int colonCount = 0;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] == ':') {
            colonPos = i;
            ++colonCount;
        }
    }
    if (colonCount > 1) return false;

    std::string hostPart = trimmed;
    uint16_t parsedPort = 0;
    if (colonCount == 1) {
        hostPart = trimmed.substr(0, colonPos);
        std::string portPart = trimmed.substr(colonPos + 1);
        if (hostPart.empty() || !ParseStrictPort1To65535(portPart, &parsedPort)) {
            return false;
        }
    }

    in_addr ipv4 = {};
    if (inet_pton(AF_INET, hostPart.c_str(), &ipv4) != 1) {
        return false;
    }
    char canonIp[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &ipv4, canonIp, sizeof(canonIp))) {
        return false;
    }
    if (!TryCopyCheckedHost(canonIp, outHost, hostCap)) {
        return false;
    }
    if (colonCount == 1 && outPort) {
        *outPort = parsedPort;
    }
    return true;
}

SOCKET NetworkConnect(const char* hostIp, unsigned short port, DWORD connectTimeoutMs, DWORD ioTimeoutMs, volatile bool* cancelFlag)
{
    char normHost[128] = {};
    uint16_t parsedPort = 0;
    if (!NetworkNormalizeTargetAddress(hostIp, normHost, sizeof(normHost), &parsedPort)) {
        Logf("republicmp  NETWORK: invalid target address [%s]", hostIp ? hostIp : "(null)");
        return INVALID_SOCKET;
    }

    unsigned short effectivePort = (parsedPort != 0) ? parsedPort : port;
    if (effectivePort == 0) {
        Logf("republicmp  NETWORK: invalid port 0 for [%s]", normHost);
        return INVALID_SOCKET;
    }

    const char* connectHost = normHost;
    uint64_t remoteSteamId = SteamP2P_ParseTargetSteamId(normHost);
    if (remoteSteamId != 0) {
        uint64_t mySteamId = SteamP2P_GetLocalSteamId();
        if (mySteamId != 0 && remoteSteamId == mySteamId) {
            Logf("republicmp  NETWORK: target SteamID %llu is local Steam account -> routing via TCP 127.0.0.1:%u",
                 (unsigned long long)remoteSteamId, effectivePort);
            connectHost = "127.0.0.1";
        } else {
            return SteamP2P_Connect(remoteSteamId, effectivePort, connectTimeoutMs, ioTimeoutMs, cancelFlag);
        }
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: socket() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(effectivePort);
    if (inet_pton(AF_INET, connectHost, &addr.sin_addr) != 1) {
        Logf("republicmp  NETWORK: couldn't parse IP [%s]", connectHost);
        closesocket(sock);
        return INVALID_SOCKET;
    }

    Logf("republicmp  NETWORK: connecting to %s:%u (timeout %u ms)...", connectHost, effectivePort, connectTimeoutMs);

    // Standard non-blocking connect-with-timeout pattern.
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    int connectResult = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (connectResult == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            Logf("republicmp  NETWORK: connect() failed, WSAGetLastError=%d", err);
            closesocket(sock);
            return INVALID_SOCKET;
        }

        // Polled in short steps so the cancel flag is checked; the result is read
        // from the same select() call that reported completion.
        const DWORD pollStepMs = 200;
        DWORD waited = 0;
        fd_set writeSet;
        fd_set exceptSet;
        for (;;) {
            if (cancelFlag && *cancelFlag) {
                Logf("republicmp  NETWORK: connect to %s:%u cancelled", connectHost, effectivePort);
                closesocket(sock);
                return INVALID_SOCKET;
            }

            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            FD_ZERO(&exceptSet);
            FD_SET(sock, &exceptSet);

            DWORD stepMs = (connectTimeoutMs == 0)
                ? pollStepMs
                : std::min(pollStepMs, connectTimeoutMs - waited);
            timeval stepTimeout;
            stepTimeout.tv_sec = stepMs / 1000;
            stepTimeout.tv_usec = (stepMs % 1000) * 1000;

            int selectResult = select(0, nullptr, &writeSet, &exceptSet, &stepTimeout);
            if (selectResult == SOCKET_ERROR) {
                Logf("republicmp  NETWORK: select() failed, WSAGetLastError=%d", WSAGetLastError());
                closesocket(sock);
                return INVALID_SOCKET;
            }
            if (selectResult > 0) {
                break; // writeSet/exceptSet now hold this call's real result
            }

            if (connectTimeoutMs != 0) {
                waited += stepMs;
                if (waited >= connectTimeoutMs) {
                    Logf("republicmp  NETWORK: TIMEOUT - couldn't connect to %s:%u within %u ms", connectHost, effectivePort, connectTimeoutMs);
                    closesocket(sock);
                    return INVALID_SOCKET;
                }
            }
        }

        if (FD_ISSET(sock, &exceptSet)) {
            // select() succeeded; the connect error itself is in SO_ERROR (e.g.
            // WSAECONNREFUSED when nothing is listening).
            int soError = 0;
            int soErrorLen = sizeof(soError);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen);
            Logf("republicmp  NETWORK: connect() failed (SO_ERROR=%d)", soError);
            closesocket(sock);
            return INVALID_SOCKET;
        }

        int soError = 0;
        int soErrorLen = sizeof(soError);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen);
        if (soError != 0) {
            Logf("republicmp  NETWORK: connect() failed (SO_ERROR=%d)", soError);
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }

    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);

    Logf("republicmp  NETWORK: connected to %s:%u", connectHost, effectivePort);
    NetworkSetSocketTimeouts(sock, ioTimeoutMs);
    NetworkEnableKeepalive(sock);
    return sock;
}

bool NetworkSendExact(SOCKET sock, const void* data, size_t size)
{
    if (SteamP2P_IsSteamSocket(sock)) {
        return SteamP2P_SendExact(sock, data, size);
    }
    const char* bytes = (const char*)data;
    size_t sent = 0;
    while (sent < size) {
        int chunk = send(sock, bytes + sent, (int)std::min(size - sent, (size_t)INT_MAX), 0);
        if (chunk == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                Logf("republicmp  NETWORK: TIMEOUT - send() stalled (sent %zu of %zu bytes)", sent, size);
            } else {
                Logf("republicmp  NETWORK: send() failed, WSAGetLastError=%d", err);
            }
            return false;
        }
        if (chunk == 0) {
            Logf("republicmp  NETWORK: send() returned 0 (connection closed?)");
            return false;
        }
        sent += (size_t)chunk;
    }
    return true;
}

bool NetworkRecvExact(SOCKET sock, void* data, size_t size)
{
    if (SteamP2P_IsSteamSocket(sock)) {
        return SteamP2P_RecvExact(sock, data, size);
    }
    char* bytes = (char*)data;
    size_t received = 0;
    while (received < size) {
        int chunk = recv(sock, bytes + received, (int)std::min(size - received, (size_t)INT_MAX), 0);
        if (chunk == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                Logf("republicmp  NETWORK: TIMEOUT - recv() stalled (received %zu of %zu bytes)", received, size);
            } else {
                Logf("republicmp  NETWORK: recv() failed, WSAGetLastError=%d", err);
            }
            return false;
        }
        if (chunk == 0) {
            Logf("republicmp  NETWORK: connection closed early (received %zu of %zu bytes)", received, size);
            return false;
        }
        received += (size_t)chunk;
    }
    return true;
}

bool NetworkSendFile(SOCKET sock, const char* filePath, NetworkProgressFn onProgress, void* userData)
{
    if (!filePath || !filePath[0]) {
        Logf("republicmp  NETWORK: NetworkSendFile() called with empty filePath");
        return false;
    }
    std::filesystem::path fp(filePath);
    FILE* f = nullptr;
    _wfopen_s(&f, fp.c_str(), L"rb");
    if (!f) {
        Logf("republicmp  NETWORK: couldn't open [%s] for reading", filePath);
        return false;
    }

    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long long rawSz = _ftelli64(f);
    if (rawSz < 0 || _fseeki64(f, 0, SEEK_SET) != 0) {
        Logf("republicmp  NETWORK: _ftelli64/_fseeki64 failed for [%s]", filePath);
        fclose(f);
        return false;
    }
    unsigned long long fileSize = (unsigned long long)rawSz;

    if (!NetworkSendExact(sock, (const char*)&fileSize, sizeof(fileSize))) {
        fclose(f);
        return false;
    }

    Logf("republicmp  NETWORK: sending [%s], %llu bytes...", filePath, fileSize);

    char buffer[8192];
    unsigned long long sentTotal = 0;
    while (sentTotal < fileSize) {
        size_t toRead = (size_t)std::min((unsigned long long)sizeof(buffer), fileSize - sentTotal);
        size_t bytesRead = fread(buffer, 1, toRead, f);
        if (bytesRead == 0) {
            Logf("republicmp  NETWORK: fread() returned 0 early (sent %llu of %llu bytes)", sentTotal, fileSize);
            fclose(f);
            return false;
        }
        if (!NetworkSendExact(sock, buffer, bytesRead)) {
            fclose(f);
            return false;
        }
        sentTotal += bytesRead;
        if (onProgress) onProgress(sentTotal, fileSize, userData);
    }

    fclose(f);
    if (SteamP2P_IsSteamSocket(sock)) {
        if (!SteamP2P_FlushSend(sock, 30000)) {
            Logf("republicmp  NETWORK: failed flushing SteamP2P send queue for [%s]", filePath);
            return false;
        }
    }
    Logf("republicmp  NETWORK: finished sending [%s], %llu bytes", filePath, sentTotal);
    return true;
}

bool NetworkReceiveFile(SOCKET sock, const char* outFilePath, NetworkProgressFn onProgress, void* userData)
{
    if (!outFilePath || !outFilePath[0]) {
        Logf("republicmp  NETWORK: NetworkReceiveFile() called with empty outFilePath");
        return false;
    }

    unsigned long long fileSize = 0;
    if (!NetworkRecvExact(sock, (char*)&fileSize, sizeof(fileSize))) {
        return false;
    }

    static constexpr unsigned long long kMaxReceiveFileBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    if (fileSize > kMaxReceiveFileBytes) {
        Logf("republicmp  NETWORK: NetworkReceiveFile rejected oversized payload (%llu > %llu bytes)",
             fileSize, kMaxReceiveFileBytes);
        return false;
    }

    std::filesystem::path fp(outFilePath);
    FILE* f = nullptr;
    _wfopen_s(&f, fp.c_str(), L"wb");
    if (!f) {
        Logf("republicmp  NETWORK: couldn't open [%s] for writing", outFilePath);
        return false;
    }

    auto removePartialFile = [&fp]() {
        std::error_code ec;
        std::filesystem::remove(fp, ec);
    };

    Logf("republicmp  NETWORK: receiving [%s], expecting %llu bytes...", outFilePath, fileSize);

    char buffer[8192];
    unsigned long long receivedTotal = 0;
    unsigned long long nextLogMark = 4ULL * 1024ULL * 1024ULL;
    while (receivedTotal < fileSize) {
        size_t toRead = (size_t)std::min((unsigned long long)sizeof(buffer), fileSize - receivedTotal);
        if (!NetworkRecvExact(sock, buffer, toRead)) {
            Logf("republicmp  NETWORK: NetworkReceiveFile interrupted at %llu / %llu bytes", receivedTotal, fileSize);
            fclose(f);
            removePartialFile();
            return false;
        }
        if (fwrite(buffer, 1, toRead, f) != toRead) {
            Logf("republicmp  NETWORK: fwrite() didn't write everything (disk full?)");
            fclose(f);
            removePartialFile();
            return false;
        }
        receivedTotal += toRead;
        if (receivedTotal >= nextLogMark || receivedTotal == fileSize) {
            Logf("republicmp  NETWORK: received %llu / %llu bytes (%llu%%)",
                 receivedTotal, fileSize, fileSize ? (receivedTotal * 100ULL) / fileSize : 100ULL);
            nextLogMark = receivedTotal + (4ULL * 1024ULL * 1024ULL);
        }
        if (onProgress) onProgress(receivedTotal, fileSize, userData);
    }

    if (fclose(f) != 0) {
        Logf("republicmp  NETWORK: fclose() failed flushing [%s] (disk full?)", outFilePath);
        removePartialFile();
        return false;
    }
    Logf("republicmp  NETWORK: finished receiving [%s], %llu bytes", outFilePath, receivedTotal);
    return true;
}

bool NetworkPeerGone(SOCKET sock)
{
    if (sock == INVALID_SOCKET) return true;
    if (SteamP2P_IsSteamSocket(sock)) return SteamP2P_PeerGone(sock);
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval zero = { 0, 0 };
    int r = select(0, &readSet, nullptr, nullptr, &zero);
    if (r == SOCKET_ERROR) return true;
    if (r == 0) return false; // nothing pending - still up
    // Readable: data (peer alive), or EOF / reset / keepalive failure. The
    // socket is readable, so this peek does not block.
    char b;
    int n = recv(sock, &b, 1, MSG_PEEK);
    if (n > 0) return false;
    if (n == 0) Logf("republicmp  NETWORK: the peer closed the connection");
    else Logf("republicmp  NETWORK: connection lost, WSAGetLastError=%d", WSAGetLastError());
    return true;
}

int NetworkPollReadable(SOCKET sock, DWORD timeoutMs)
{
    if (sock == INVALID_SOCKET) return -1;
    if (SteamP2P_IsSteamSocket(sock)) return 1;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval tv = {};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000u);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000u) * 1000u);
    int r = select(0, &readSet, nullptr, nullptr, &tv);
    if (r == SOCKET_ERROR) return -1;
    return (r > 0 && FD_ISSET(sock, &readSet)) ? 1 : 0;
}

void NetworkDrainReadable(SOCKET sock, size_t maxBytes)
{
    if (sock == INVALID_SOCKET || SteamP2P_IsSteamSocket(sock)) return;
    char buf[1024];
    size_t drained = 0;
    while (drained < maxBytes) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval zero = { 0, 0 };
        int r = select(0, &readSet, nullptr, nullptr, &zero);
        if (r <= 0 || !FD_ISSET(sock, &readSet)) break;
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        drained += static_cast<size_t>(n);
    }
}

void NetworkShutdownSocket(SOCKET sock, int how)
{
    if (sock == INVALID_SOCKET) return;
    if (SteamP2P_IsSteamSocket(sock)) {
        SteamP2P_ShutdownSocket(sock);
        return;
    }
    shutdown(sock, how);
}

void NetworkCloseSocket(SOCKET sock)
{
    if (sock == INVALID_SOCKET) return;
    if (SteamP2P_IsSteamSocket(sock)) {
        SteamP2P_CloseSocket(sock);
        return;
    }
    closesocket(sock);
}

void NetworkGetPeerName(SOCKET sock, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (sock == INVALID_SOCKET) return;
    if (SteamP2P_IsSteamSocket(sock)) {
        SteamP2P_GetPeerName(sock, out, outSize);
        return;
    }
    sockaddr_in addr = {};
    int len = sizeof(addr);
    if (getpeername(sock, (sockaddr*)&addr, &len) == 0)
        inet_ntop(AF_INET, &addr.sin_addr, out, outSize);
}

