// SPDX-License-Identifier: GPL-3.0-only
// Multiplayer session: joining a player into the host's running game and
// keeping track of whether that player is still there.
//
// Host: Host starts hosting and returns - the game keeps running while
// nobody is connected. When a player connects, both games are paused and
// locked behind the sync screen (ui/sync_block) while the host saves, packs
// and sends the world and the player unpacks and loads it. Then the live sync
// channels (build_sync, road_sync, terrain_sync) connect, the screens lift
// and both games resume together. The host keeps hosting: if the player
// leaves or the connection drops, it goes back to waiting for a player, until
// Stop hosting.
//
// Client: Connect pauses and locks the game at once and walks the same
// steps from the other side; the host's steps (saving, packing) are relayed
// so the sync screen always says what is happening. The session ends when
// the connection drops or on Disconnect.
//
// Before anything else both sides exchange version, protocol number and a
// CRC32 of their RepublicMP.dll (version.h); any difference refuses the join.
//
// One session runs at a time: Host/Connect are refused while one is active.
//
// Re-sync: both games simulate on their own, so vehicles, citizens,
// production and storage drift apart. The host can replace the player's world
// with its own again, by hand or every few minutes: it announces the re-sync
// on the build_sync channel, closes the live channels and listens again; the
// player, seeing the announcement instead of a drop, connects straight back
// and the normal join runs (both games pause, save, send, load).
#pragma once

#include "tesmio_plugin.h"
#include "../core/host_config.h"

#define SAVE_SYNC_PORT 5556
#define SAVE_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP
#define SAVE_SYNC_RECEIVED_SUFFIX "_received"

enum SessionState {
    SESSION_IDLE = 0,
    SESSION_HOST_LISTENING,  // hosting, no player yet
    SESSION_HOST_JOINING,    // a player is joining (both games locked)
    SESSION_HOST_IN_GAME,    // the player is in the game, live sync running
    SESSION_CLIENT_JOINING,  // connecting / receiving / loading (locked)
    SESSION_CLIENT_IN_GAME,  // in the host's game, live sync running
};

void SaveSync_Bind(const TsmHost* host);

// Writes to tesmioloader.log and the in-game UI log.
void SaveStatus(const char* fmt, ...);

#include <vector>
#include <string>

// True while a world is loaded (not the main menu, not mid-load).
bool SaveSync_IsWorldLoaded();

// Refused (no-op) while a session is active. Hosting needs a loaded world;
// leaving the world ends the session (SaveSync_Tick).
void SaveSync_StartHost();
void SaveSync_StartClient(const char* hostIp = nullptr); // nullptr = keep last IP

// Ends the session: stops hosting / leaves the host, closes the live sync
// channels. Waits (accept, connect, save, load) react within ~200ms.
void SaveSync_RequestDisconnect();

// Host, player in the game: re-sync now (no-op otherwise).
void SaveSync_RequestResync();

// Host: re-sync automatically every `minutes` minutes of live play (0 = off).
// Lockstep: re-sync this many seconds after the games differ (-1: never by itself).
void SaveSync_SetDesyncResyncSeconds(int seconds);
int SaveSync_GetDesyncResyncSeconds();
// Host: ms until that re-sync (0: none pending).
ULONGLONG SaveSync_GetMsUntilDesyncResync();
void SaveSync_SetAutoResyncMinutes(int minutes);
int SaveSync_GetAutoResyncMinutes();

// Host, player in the game: ms until the next automatic re-sync (0 = off).
ULONGLONG SaveSync_GetMsUntilAutoResync();

// Host, player in the game: ms since the player last received the world (join
// or re-sync); 0 = no player in the game.
ULONGLONG SaveSync_GetMsSinceWorldSent();

// True while a re-sync is under way (from the announcement to the player
// being back in the game).
bool SaveSync_IsResyncing();

// Client: the host announced a re-sync (build_sync receive thread).
void SaveSync_OnResyncAnnounced();

// Per-frame (game thread, Present hook): sends a requested announcement -
// build_sync only sends from the game thread.
void SaveSync_Tick();

// Host: run the next session in lockstep (both PCs simulate the same ticks;
// lockstep/lockstep_net). The player follows the host's choice.
void SaveSync_SetLockstep(bool on);
bool SaveSync_GetLockstep();
// Refuse a join when the two games' DLCs differ (on by default; a join goes ahead when either player turns it off).
void SaveSync_SetDlcCheck(bool on);
bool SaveSync_GetDlcCheck();
// Extracts a lowercase DLC token (e.g. "dlc3") from building or vehicle type name (e.g. "dlc3_school", "media_soviet/dlc3/vehicles/bus", "dlc1\\vehicles\\car"); returns false if not a DLC token.
bool SaveSync_ExtractDlcToken(const char* typeName, char* outToken, size_t outSize);
inline bool SaveSync_ExtractBuildingDlcToken(const char* typeName, char* outToken, size_t outSize) {
    return SaveSync_ExtractDlcToken(typeName, outToken, outSize);
}

// Safely reads a null-terminated ASCII string under SEH protection without page faults.
bool SaveSync_SafeReadAscii(const char* src, char* outBuf, size_t outBufSize);
inline bool SafeReadAsciiDirect(const char* src, char* outBuf, size_t outBufSize) {
    return SaveSync_SafeReadAscii(src, outBuf, outBufSize);
}

// Parses a comma-separated DLC string (e.g. "dlc2,dlc3") into bitmask (1 << (dlcNum - 1)).
uint32_t SaveSync_GetDlcMask(const char* dlcList);

// Formats human-readable DLC titles from bitmask (e.g. "Biomes (dlc2), Early Start (dlc3)").
void SaveSync_FormatDlcNames(uint32_t mask, char* out, size_t outSize);

// Returns the current session's host, client, and negotiated common DLC bitmasks.
void SaveSync_GetDlcMasks(uint32_t* outHostMask, uint32_t* outClientMask, uint32_t* outCommonMask);

// True if `dlcToken` (e.g. "dlc3") is present in a comma-separated DLC list (e.g. "dlc2,dlc3").
bool SaveSync_IsDlcTokenInList(const char* list, const char* dlcToken);

// True if placing `typeName` is allowed in the current session (blocks DLC buildings when the peer lacks that DLC).
bool SaveSync_CheckBuildingDlcAllowed(const char* typeName);

// True if purchasing/creating `vehicleTypePtr` (and all coupled trailers) is allowed in the current session.
bool SaveSync_CheckVehicleDlcAllowed(const void* vehicleTypePtr);

// The current session runs in lockstep.
bool SaveSync_IsLockstepSession();

SessionState SaveSync_GetState();
bool SaveSync_IsBusy();     // any state but SESSION_IDLE
bool SaveSync_IsHostRole(); // role of the current/last session
// Milliseconds since Host / Connect started the current session (0 when idle).
// The UI keeps its stop buttons disabled for a moment after a start, so a
// double click on Connect does not land on Disconnect.
ULONGLONG SaveSync_GetSessionAgeMs();
#define SAVE_SYNC_STOP_BUTTON_DELAY_MS 2000

// The other player's IP (host: the connected player, client: the host), or "".
const char* SaveSync_GetPeerIp();

// Why the last session ended or failed ("" if it ended normally).
const char* SaveSync_GetLastError();

// This build's identity, as exchanged in the handshake.
const char* SaveSync_GetLocalVersion();
unsigned int SaveSync_GetLocalDllCrc();

const char* SaveSync_GetTargetIp();

// Kept for the debug UI; true while hosting with nobody connected.
bool SaveSync_IsHostAwaitingClient();

// True when another SOVIET64.exe instance on this same PC is currently hosting
// an open Co-op server in SESSION_HOST_LISTENING on 127.0.0.1:5556.
bool SaveSync_IsLocalPcHostWaiting();

// Pure predicate (used by runtime and unit tests): true when world edits (buildings,
// terrain digging/leveling, roads, demolition) and simulation ticks must be held.
bool SaveSync_ShouldBlockWorldEdits(int sessionState, bool inGameplay, bool syncBlockActive);

// True while the Co-op Waiting Lobby / Synchronization lock is active in gameplay.
bool SaveSync_IsWaitingLobbyActive();

// True when a partner had already joined this Host session earlier, dropped/disconnected,
// and the Host is now paused in SESSION_HOST_LISTENING waiting for them to reconnect.
bool SaveSync_IsWaitingForReconnect();

// True if at least one partner has connected to this Host session since SaveSync_StartHost().
bool SaveSync_HadPartnerInSession();

// Last partner address/name that was in the game before disconnecting (or "").
const char* SaveSync_GetLastPartnerPeer();

// Enforces speed = 0, timer paused byte = 1, and clears any selected build/dig/demolish tool
// and active drag on `exeBase` (or `g_exeBase` when `exeBase == nullptr`).
void SaveSync_EnforceWaitingLobbyHold(unsigned char* exeBase = nullptr);

// Rate-limited toast & log notification when a player attempts to build/dig/demolish while waiting.
void SaveSync_NotifyWaitingLobbyActionBlocked();

// Test-only state injection for unit tests in tests/test_main.cpp.
void SaveSync_SetTestSessionState(SessionState state, bool hadPartnerInSession = false,
                                  const char* lastPartnerPeer = nullptr);
void SaveSync_SetTestDlcLists(const char* hostList, const char* clientList);

// True when running in dual-window automation (REPUBLICMP_ROLE, CLI flags, or test-host/test-client).
bool SaveSync_IsDualTestbed();
inline bool IsDualTestbed() { return SaveSync_IsDualTestbed(); }

