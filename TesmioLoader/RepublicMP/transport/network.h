// SPDX-License-Identifier: GPL-3.0-only
// Minimal blocking TCP transport shared by the sync modules: accept one
// client, connect to a host, exchange fixed-size structs or whole files
// (8-byte little-endian size, then the bytes). Only ever called from worker
// threads or for small sends with a send timeout.
#pragma once

#include "tesmio_plugin.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void Network_Bind(const TsmHost* host);
bool NetworkInit();
void Network_SetLoopbackOnlyForTest(bool enabled);
// Disables Nagle's algorithm (TCP_NODELAY = 1) on real Winsock TCP sockets;
// safe no-op on INVALID_SOCKET and Steam P2P virtual sockets.
void NetworkEnableNoDelay(SOCKET sock);

// Normalizes a target address entered by the user or from a host list:
// trims leading/trailing ASCII whitespace, validates and preserves steam:<id>
// or raw >=15-digit SteamID64 (without modifying *outPort), or validates IPv4
// with optional :port (1..65535). Returns false on null/empty/malformed input
// or insufficient hostCap (clears outHost on failure).
bool NetworkNormalizeTargetAddress(const char* rawInput, char* outHost, size_t hostCap,
                                   uint16_t* outPort = nullptr);

// Listens on port and accepts one client. acceptTimeoutMs bounds the wait
// (0 = no limit); ioTimeoutMs is the send/recv timeout of the connection.
// The wait is polled in ~200ms steps and returns INVALID_SOCKET early when
// *cancelFlag becomes true.
SOCKET NetworkHostAcceptOne(unsigned short port,
                            DWORD acceptTimeoutMs = 60000,
                            DWORD ioTimeoutMs = 30000,
                            volatile bool* cancelFlag = nullptr);

// Connects to hostIp:port, same timeout and cancel semantics.
SOCKET NetworkConnect(const char* hostIp, unsigned short port,
                      DWORD connectTimeoutMs = 15000,
                      DWORD ioTimeoutMs = 30000,
                      volatile bool* cancelFlag = nullptr);

typedef void (*NetworkProgressFn)(unsigned long long done, unsigned long long total, void* userData);

bool NetworkSendFile(SOCKET sock, const char* filePath,
                     NetworkProgressFn onProgress = nullptr, void* userData = nullptr);
bool NetworkReceiveFile(SOCKET sock, const char* outFilePath,
                        NetworkProgressFn onProgress = nullptr, void* userData = nullptr);

// Application-ACK-paced file transfer for accurate sender progress/speed reporting
// over buffered Steam P2P or OS TCP buffers. Receiver confirms written bytes via uint64 ACK.
bool NetworkSendFileAckPaced(SOCKET sock, const char* filePath,
                             NetworkProgressFn onProgress = nullptr, void* userData = nullptr,
                             volatile bool* cancelFlag = nullptr,
                             size_t maxWindowBytes = (1024u * 1024u),
                             DWORD ackTimeoutMs = 60000u);
bool NetworkReceiveFileAckPaced(SOCKET sock, const char* outFilePath,
                                NetworkProgressFn onProgress = nullptr, void* userData = nullptr,
                                volatile bool* cancelFlag = nullptr);

// Sends/receives exactly `size` bytes; false on error or timeout.
bool NetworkSendExact(SOCKET sock, const void* data, size_t size);
bool NetworkRecvExact(SOCKET sock, void* data, size_t size);

// Non-blocking: true when the connection has been closed or has failed
// (including a keepalive timeout). Pending data counts as alive and is left
// unread.
bool NetworkPeerGone(SOCKET sock);

// Non-blocking / bounded poll for incoming readability (1 = readable or EOF/error,
// 0 = timeout, -1 = error or INVALID_SOCKET). SteamP2P sockets return 1 immediately
// because SteamP2P_RecvExact polls internally and exits on localClosed/remoteFin.
int NetworkPollReadable(SOCKET sock, DWORD timeoutMs);
// Bounded poll waiting for at least `minBytes` readable bytes (or EOF/error -> -1).
int NetworkPollReadableBytes(SOCKET sock, DWORD timeoutMs, size_t minBytes = 1);
void NetworkDrainReadable(SOCKET sock, size_t maxBytes = 65536u);
void NetworkShutdownSocket(SOCKET sock, int how = SD_BOTH);
void NetworkCloseSocket(SOCKET sock);
void NetworkGetPeerName(SOCKET sock, char* out, size_t outSize);

// Shared channel worker & socket activation helpers for BuildSync / RoadSync / TerrainSync.
inline void NetworkActivateChannelSocket(SOCKET* dstSock, SOCKET sock,
                                         volatile bool* connected, LPTHREAD_START_ROUTINE recvProc,
                                         HANDLE* outThread = nullptr)
{
    *dstSock = sock;
    DWORD sendTimeoutMs = 5000;
    DWORD recvTimeoutMs = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeoutMs, sizeof(sendTimeoutMs));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));
    *connected = true;
    HANDLE h = CreateThread(nullptr, 0, recvProc, nullptr, 0, nullptr);
    if (outThread) *outThread = h;
    else if (h) CloseHandle(h);
}

inline void NetworkLaunchChannelWorker(bool isHostRole, const char* hostIp,
                                       char* targetIpBuf, size_t targetIpCap,
                                       volatile bool* connected, volatile bool* connecting,
                                       volatile bool* isHostOut, volatile bool* cancelConnect,
                                       LPTHREAD_START_ROUTINE workerProc,
                                       HANDLE* outThread = nullptr)
{
    if (*connected || *connecting) return;
    if (!isHostRole && hostIp && hostIp[0] && targetIpBuf && targetIpCap > 0) {
        strncpy_s(targetIpBuf, targetIpCap, hostIp, _TRUNCATE);
    }
    *isHostOut = isHostRole;
    *cancelConnect = false;
    *connecting = true;
    HANDLE h = CreateThread(nullptr, 0, workerProc, nullptr, 0, nullptr);
    if (!h) {
        *connecting = false;
        if (outThread) *outThread = nullptr;
        return;
    }
    if (outThread) *outThread = h;
    else CloseHandle(h);
}

inline void NetworkJoinAndCloseThread(HANDLE* hThread, DWORD timeoutMs = 2000)
{
    if (!hThread || !*hThread) return;
    HANDLE h = *hThread;
    *hThread = nullptr;
    if (h != GetCurrentThread()) {
        WaitForSingleObject(h, timeoutMs);
    }
    CloseHandle(h);
}
