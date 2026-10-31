// SPDX-License-Identifier: GPL-3.0-only
// Steamworks P2P transport & Friends/Rich-Presence integration for RepublicMP.
// Dynamically binds to the game's already-loaded steam_api64.dll so no external
// Steamworks SDK .lib is required at build time.
#pragma once

#include "tesmio_plugin.h"
#include <winsock2.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>

struct SteamFriendEntry {
    uint64_t steamId;
    char name[64];
    bool inThisGame;
    bool isHosting;
    int personaState; // 0=Offline, 1=Online, 2=Busy, 3=Away, 4=Snooze, ...
    char connectString[128];
};

void SteamP2P_Bind(const TsmHost* host);

// Initializes dynamic bindings to steam_api64.dll and registers P2P / Join callbacks.
bool SteamP2P_Init();

// True if steam_api64.dll is loaded and the user is logged into Steam.
bool SteamP2P_IsAvailable();

// Local player's 64-bit SteamID (e.g. 76561198000000000) and persona name.
uint64_t SteamP2P_GetLocalSteamId();
const char* SteamP2P_GetLocalPersonaName();

// Refreshes and returns the list of Steam friends currently playing W&R (AppID 784150)
// and actively hosting an open Co-op server.
void SteamP2P_GetOnlineFriends(std::vector<SteamFriendEntry>& outFriends);

// Returns true (and clears the flag) if a P2P host probe reply arrived since the last check.
bool SteamP2P_ConsumeHostProbeDirty();

// Sends a Steam game invite to a friend.
bool SteamP2P_InviteFriend(uint64_t friendSteamId);

// Updates Steam Rich Presence so friends can click "Join Game" in the Steam overlay.
void SteamP2P_SetHostingPresence(bool hosting);

// Checks if the user clicked "Join Game" in Steam Friends / accepted an invite.
// If true, writes "steam:<SteamID64>" into outTarget and clears the pending flag.
bool SteamP2P_ConsumePendingJoin(char* outTarget, size_t outSize);

// Parses a target string ("steam:7656119...", or raw 17-digit SteamID64 "7656119...")
// and returns the 64-bit SteamID, or 0 if the string is a regular IPv4 address.
uint64_t SteamP2P_ParseTargetSteamId(const char* target);

// Virtual SOCKET helpers used by transport/network.cpp.
bool SteamP2P_IsSteamSocket(SOCKET s);

// Polls once (non-blocking) for an incoming Steam P2P connection on `port`.
// Returns a virtual SOCKET on success, or INVALID_SOCKET if none is ready yet.
SOCKET SteamP2P_TryAcceptOne(unsigned short port, DWORD ioTimeoutMs);

// Pre-accepts P2P sessions from online friends and flushes stale packets on `port`.
void SteamP2P_PrepareAccept(unsigned short port);

// Connects to `remoteSteamId` on channel `port` over Steam P2P.
SOCKET SteamP2P_Connect(uint64_t remoteSteamId, unsigned short port,
                        DWORD connectTimeoutMs, DWORD ioTimeoutMs,
                        volatile bool* cancelFlag);

bool SteamP2P_SendExact(SOCKET s, const void* data, size_t size);
bool SteamP2P_FlushSend(SOCKET s, DWORD timeoutMs);
bool SteamP2P_RecvExact(SOCKET s, void* data, size_t size);
bool SteamP2P_PeerGone(SOCKET s);
void SteamP2P_ShutdownSocket(SOCKET s);
void SteamP2P_CloseSocket(SOCKET s);
void SteamP2P_GetPeerName(SOCKET s, char* out, size_t outSize);

// Called once per frame on the main thread to run Steam callbacks and pump packets.
void SteamP2P_FrameTick();

// Unregisters Steam P2P callbacks (1202, 1203, 337) during session teardown.
void SteamP2P_UnregisterCallbacks();
