// SPDX-License-Identifier: GPL-3.0-only
// See steam_p2p.h.
#include "steam_p2p.h"
#include <deque>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void SteamP2P_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// Dynamic bindings to steam_api64.dll (Steamworks SDK v1.44-v1.46 ABI)
// ---------------------------------------------------------------------------

typedef int32_t HSteamUser;
typedef bool (*Fn_SteamAPI_IsSteamRunning)();
typedef void (*Fn_SteamAPI_RunCallbacks)();
typedef HSteamUser (*Fn_SteamAPI_GetHSteamUser)();
typedef void* (*Fn_SteamInternal_FindOrCreateUserInterface)(HSteamUser hSteamUser, const char* pszVersion);
typedef void (*Fn_SteamAPI_RegisterCallback)(void* pCallback, int iCallback);
typedef void (*Fn_SteamAPI_UnregisterCallback)(void* pCallback);

// ISteamUser
typedef uint64_t (*Fn_ISteamUser_GetSteamID)(void* self);
typedef bool (*Fn_ISteamUser_BLoggedOn)(void* self);

// ISteamFriends
#pragma pack(push, 8)
struct SteamFriendGameInfo_t {
    uint64_t m_gameID;
    uint32_t m_unGameIP;
    uint16_t m_usGamePort;
    uint16_t m_usQueryPort;
    uint64_t m_steamIDLobby;
};
#pragma pack(pop)

typedef const char* (*Fn_ISteamFriends_GetPersonaName)(void* self);
typedef int (*Fn_ISteamFriends_GetFriendCount)(void* self, int iFriendFlags);
typedef uint64_t (*Fn_ISteamFriends_GetFriendByIndex)(void* self, int iFriend, int iFriendFlags);
typedef const char* (*Fn_ISteamFriends_GetFriendPersonaName)(void* self, uint64_t steamIDFriend);
typedef int (*Fn_ISteamFriends_GetFriendPersonaState)(void* self, uint64_t steamIDFriend);
typedef bool (*Fn_ISteamFriends_GetFriendGamePlayed)(void* self, uint64_t steamIDFriend, SteamFriendGameInfo_t* pFriendGameInfo);
typedef bool (*Fn_ISteamFriends_SetRichPresence)(void* self, const char* pchKey, const char* pchValue);
typedef void (*Fn_ISteamFriends_ClearRichPresence)(void* self);
typedef const char* (*Fn_ISteamFriends_GetFriendRichPresence)(void* self, uint64_t steamIDFriend, const char* pchKey);
typedef bool (*Fn_ISteamFriends_RequestFriendRichPresence)(void* self, uint64_t steamIDFriend);
typedef bool (*Fn_ISteamFriends_InviteUserToGame)(void* self, uint64_t steamIDFriend, const char* pchConnectString);

// ISteamNetworking
enum EP2PSend {
    k_EP2PSendUnreliable = 0,
    k_EP2PSendUnreliableNoDelay = 1,
    k_EP2PSendReliable = 2,
    k_EP2PSendReliableWithBuffering = 3,
};

#pragma pack(push, 8)
struct P2PSessionState_t {
    uint8_t m_bConnectionActive;
    uint8_t m_bConnecting;
    uint8_t m_eP2PSessionError;
    uint8_t m_bUsingRelay;
    int32_t m_nBytesQueuedForSend;
    int32_t m_nPacketsQueuedForSend;
    uint32_t m_nRemoteIP;
    uint16_t m_nRemotePort;
};
#pragma pack(pop)

typedef bool (*Fn_ISteamNetworking_SendP2PPacket)(void* self, uint64_t steamIDRemote, const void* pubData, uint32_t cubData, int eP2PSendType, int nChannel);
typedef bool (*Fn_ISteamNetworking_IsP2PPacketAvailable)(void* self, uint32_t* pcubMsgSize, int nChannel);
typedef bool (*Fn_ISteamNetworking_ReadP2PPacket)(void* self, void* pubDest, uint32_t cubDest, uint32_t* pcubMsgSize, uint64_t* psteamIDRemote, int nChannel);
typedef bool (*Fn_ISteamNetworking_AcceptP2PSessionWithUser)(void* self, uint64_t steamIDRemote);
typedef bool (*Fn_ISteamNetworking_CloseP2PSessionWithUser)(void* self, uint64_t steamIDRemote);
typedef bool (*Fn_ISteamNetworking_CloseP2PChannelWithUser)(void* self, uint64_t steamIDRemote, int nChannel);
typedef bool (*Fn_ISteamNetworking_GetP2PSessionState)(void* self, uint64_t steamIDRemote, P2PSessionState_t* pConnectionState);
typedef bool (*Fn_ISteamNetworking_AllowP2PPacketRelay)(void* self, bool bAllow);

static HMODULE s_hSteamDll = nullptr;
static bool s_initAttempted = false;
static bool s_available = false;
static volatile bool s_shuttingDown = false;

struct SteamExitGuard {
    ~SteamExitGuard() {
        s_shuttingDown = true;
        s_available = false;
    }
};
static SteamExitGuard s_steamExitGuard;

static Fn_SteamAPI_IsSteamRunning pSteamAPI_IsSteamRunning = nullptr;
static Fn_SteamAPI_RunCallbacks pSteamAPI_RunCallbacks = nullptr;
static Fn_SteamAPI_GetHSteamUser pSteamAPI_GetHSteamUser = nullptr;
static Fn_SteamInternal_FindOrCreateUserInterface pSteamInternal_FindOrCreateUserInterface = nullptr;
static Fn_SteamAPI_RegisterCallback pSteamAPI_RegisterCallback = nullptr;
static Fn_SteamAPI_UnregisterCallback pSteamAPI_UnregisterCallback = nullptr;

static Fn_ISteamUser_GetSteamID pISteamUser_GetSteamID = nullptr;
static Fn_ISteamUser_BLoggedOn pISteamUser_BLoggedOn = nullptr;

static Fn_ISteamFriends_GetPersonaName pISteamFriends_GetPersonaName = nullptr;
static Fn_ISteamFriends_GetFriendCount pISteamFriends_GetFriendCount = nullptr;
static Fn_ISteamFriends_GetFriendByIndex pISteamFriends_GetFriendByIndex = nullptr;
static Fn_ISteamFriends_GetFriendPersonaName pISteamFriends_GetFriendPersonaName = nullptr;
static Fn_ISteamFriends_GetFriendPersonaState pISteamFriends_GetFriendPersonaState = nullptr;
static Fn_ISteamFriends_GetFriendGamePlayed pISteamFriends_GetFriendGamePlayed = nullptr;
static Fn_ISteamFriends_SetRichPresence pISteamFriends_SetRichPresence = nullptr;
static Fn_ISteamFriends_ClearRichPresence pISteamFriends_ClearRichPresence = nullptr;
static Fn_ISteamFriends_GetFriendRichPresence pISteamFriends_GetFriendRichPresence = nullptr;
static Fn_ISteamFriends_RequestFriendRichPresence pISteamFriends_RequestFriendRichPresence = nullptr;
static Fn_ISteamFriends_InviteUserToGame pISteamFriends_InviteUserToGame = nullptr;

static Fn_ISteamNetworking_SendP2PPacket pISteamNetworking_SendP2PPacket = nullptr;
static Fn_ISteamNetworking_IsP2PPacketAvailable pISteamNetworking_IsP2PPacketAvailable = nullptr;
static Fn_ISteamNetworking_ReadP2PPacket pISteamNetworking_ReadP2PPacket = nullptr;
static Fn_ISteamNetworking_AcceptP2PSessionWithUser pISteamNetworking_AcceptP2PSessionWithUser = nullptr;
static Fn_ISteamNetworking_CloseP2PSessionWithUser pISteamNetworking_CloseP2PSessionWithUser = nullptr;
static Fn_ISteamNetworking_CloseP2PChannelWithUser pISteamNetworking_CloseP2PChannelWithUser = nullptr;
static Fn_ISteamNetworking_GetP2PSessionState pISteamNetworking_GetP2PSessionState = nullptr;
static Fn_ISteamNetworking_AllowP2PPacketRelay pISteamNetworking_AllowP2PPacketRelay = nullptr;

static void* s_pSteamUser = nullptr;
static void* s_pSteamFriends = nullptr;
static void* s_pSteamNetworking = nullptr;

// ---------------------------------------------------------------------------
// Steam Callbacks (P2PSessionRequest_t = 1202, P2PSessionConnectFail_t = 1203,
// GameRichPresenceJoinRequested_t = 337)
// ---------------------------------------------------------------------------

#pragma pack(push, 8)
struct P2PSessionRequest_t {
    uint64_t m_steamIDRemote;
};
struct P2PSessionConnectFail_t {
    uint64_t m_steamIDRemote;
    uint8_t m_eP2PSessionError;
};
struct GameRichPresenceJoinRequested_t {
    uint64_t m_steamIDFriend;
    char m_rgchConnect[256];
};
#pragma pack(pop)

struct RawSteamCallback {
    void* vtable;
    uint8_t flags;
    uint8_t pad[3];
    int callbackId;
    int paramSize;
    void (*handler)(void* pvParam);
};

static void CallbackThunk_Run(RawSteamCallback* self, void* pvParam)
{
    if (self && self->handler && pvParam) self->handler(pvParam);
}

static int CallbackThunk_GetSize(RawSteamCallback* self)
{
    return self ? self->paramSize : 0;
}

static void* s_callbackVtable[3] = {
    (void*)&CallbackThunk_Run,
    (void*)&CallbackThunk_Run,
    (void*)&CallbackThunk_GetSize,
};

static RawSteamCallback s_cbP2PRequest = {};
static RawSteamCallback s_cbP2PFail = {};
static RawSteamCallback s_cbRichPresenceJoin = {};
static bool s_callbacksRegistered = false;

static CRITICAL_SECTION s_lock;
static bool s_lockReady = false;
static char s_pendingJoinTarget[64] = "";

static void EnsureLock()
{
    if (!s_lockReady) {
        InitializeCriticalSection(&s_lock);
        s_lockReady = true;
    }
}

static void OnP2PSessionRequest(void* pvParam)
{
    P2PSessionRequest_t* req = (P2PSessionRequest_t*)pvParam;
    if (!req || !s_pSteamNetworking || !pISteamNetworking_AcceptP2PSessionWithUser) return;
    pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, req->m_steamIDRemote);
    Logf("republicmp  STEAM_P2P: accepted incoming P2P session request from SteamID %llu",
         (unsigned long long)req->m_steamIDRemote);
}

static void OnP2PSessionConnectFail(void* pvParam)
{
    P2PSessionConnectFail_t* fail = (P2PSessionConnectFail_t*)pvParam;
    if (!fail) return;
    Logf("republicmp  STEAM_P2P: P2P session connect fail with SteamID %llu, error=%u",
         (unsigned long long)fail->m_steamIDRemote, (unsigned)fail->m_eP2PSessionError);
}

static void OnRichPresenceJoinRequested(void* pvParam)
{
    GameRichPresenceJoinRequested_t* join = (GameRichPresenceJoinRequested_t*)pvParam;
    if (!join) return;
    uint64_t targetId = join->m_steamIDFriend;
    const char* p = strstr(join->m_rgchConnect, "+connect_steam ");
    if (p) {
        uint64_t parsed = _strtoui64(p + 15, nullptr, 10);
        if (parsed >= 76561197960265728ULL) targetId = parsed;
    }
    if (targetId >= 76561197960265728ULL) {
        EnsureLock();
        EnterCriticalSection(&s_lock);
        _snprintf_s(s_pendingJoinTarget, sizeof(s_pendingJoinTarget), _TRUNCATE,
                    "steam:%llu", (unsigned long long)targetId);
        LeaveCriticalSection(&s_lock);
        Logf("republicmp  STEAM_P2P: Join Game requested via Steam Friends -> %s", s_pendingJoinTarget);
    }
}

static void* FindInterface(HSteamUser hUser, const char* const* versions, int count)
{
    if (!pSteamInternal_FindOrCreateUserInterface) return nullptr;
    for (int i = 0; i < count; ++i) {
        void* iface = pSteamInternal_FindOrCreateUserInterface(hUser, versions[i]);
        if (iface) return iface;
    }
    return nullptr;
}

bool SteamP2P_Init()
{
    EnsureLock();
    if (s_available) return true;

    static ULONGLONG s_lastAttemptTick = 0;
    static bool s_loggedWaitingForSteam = false;
    ULONGLONG now = GetTickCount64();
    if (s_initAttempted && (now - s_lastAttemptTick < 3000)) {
        return false;
    }
    s_initAttempted = true;
    s_lastAttemptTick = now;

    if (!s_hSteamDll) {
        s_hSteamDll = GetModuleHandleA("steam_api64.dll");
        if (!s_hSteamDll) return false;
    }

#define RESOLVE_FN(var, type, name) \
    var = (type)GetProcAddress(s_hSteamDll, name); \
    if (!var) { Logf("republicmp  STEAM_P2P: missing export %s", name); return false; }

    RESOLVE_FN(pSteamAPI_IsSteamRunning, Fn_SteamAPI_IsSteamRunning, "SteamAPI_IsSteamRunning");
    RESOLVE_FN(pSteamAPI_RunCallbacks, Fn_SteamAPI_RunCallbacks, "SteamAPI_RunCallbacks");
    RESOLVE_FN(pSteamAPI_GetHSteamUser, Fn_SteamAPI_GetHSteamUser, "SteamAPI_GetHSteamUser");
    RESOLVE_FN(pSteamInternal_FindOrCreateUserInterface, Fn_SteamInternal_FindOrCreateUserInterface, "SteamInternal_FindOrCreateUserInterface");
    RESOLVE_FN(pSteamAPI_RegisterCallback, Fn_SteamAPI_RegisterCallback, "SteamAPI_RegisterCallback");
    RESOLVE_FN(pSteamAPI_UnregisterCallback, Fn_SteamAPI_UnregisterCallback, "SteamAPI_UnregisterCallback");

    RESOLVE_FN(pISteamUser_GetSteamID, Fn_ISteamUser_GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    RESOLVE_FN(pISteamUser_BLoggedOn, Fn_ISteamUser_BLoggedOn, "SteamAPI_ISteamUser_BLoggedOn");

    RESOLVE_FN(pISteamFriends_GetPersonaName, Fn_ISteamFriends_GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    RESOLVE_FN(pISteamFriends_GetFriendCount, Fn_ISteamFriends_GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    RESOLVE_FN(pISteamFriends_GetFriendByIndex, Fn_ISteamFriends_GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    RESOLVE_FN(pISteamFriends_GetFriendPersonaName, Fn_ISteamFriends_GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    RESOLVE_FN(pISteamFriends_GetFriendPersonaState, Fn_ISteamFriends_GetFriendPersonaState, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    RESOLVE_FN(pISteamFriends_GetFriendGamePlayed, Fn_ISteamFriends_GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    RESOLVE_FN(pISteamFriends_SetRichPresence, Fn_ISteamFriends_SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    RESOLVE_FN(pISteamFriends_ClearRichPresence, Fn_ISteamFriends_ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence");
    RESOLVE_FN(pISteamFriends_GetFriendRichPresence, Fn_ISteamFriends_GetFriendRichPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence");
    RESOLVE_FN(pISteamFriends_RequestFriendRichPresence, Fn_ISteamFriends_RequestFriendRichPresence, "SteamAPI_ISteamFriends_RequestFriendRichPresence");
    RESOLVE_FN(pISteamFriends_InviteUserToGame, Fn_ISteamFriends_InviteUserToGame, "SteamAPI_ISteamFriends_InviteUserToGame");

    RESOLVE_FN(pISteamNetworking_SendP2PPacket, Fn_ISteamNetworking_SendP2PPacket, "SteamAPI_ISteamNetworking_SendP2PPacket");
    RESOLVE_FN(pISteamNetworking_IsP2PPacketAvailable, Fn_ISteamNetworking_IsP2PPacketAvailable, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    RESOLVE_FN(pISteamNetworking_ReadP2PPacket, Fn_ISteamNetworking_ReadP2PPacket, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    RESOLVE_FN(pISteamNetworking_AcceptP2PSessionWithUser, Fn_ISteamNetworking_AcceptP2PSessionWithUser, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    RESOLVE_FN(pISteamNetworking_CloseP2PSessionWithUser, Fn_ISteamNetworking_CloseP2PSessionWithUser, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
    RESOLVE_FN(pISteamNetworking_CloseP2PChannelWithUser, Fn_ISteamNetworking_CloseP2PChannelWithUser, "SteamAPI_ISteamNetworking_CloseP2PChannelWithUser");
    RESOLVE_FN(pISteamNetworking_GetP2PSessionState, Fn_ISteamNetworking_GetP2PSessionState, "SteamAPI_ISteamNetworking_GetP2PSessionState");
    RESOLVE_FN(pISteamNetworking_AllowP2PPacketRelay, Fn_ISteamNetworking_AllowP2PPacketRelay, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
#undef RESOLVE_FN

    SetEnvironmentVariableA("SteamAppId", "784150");
    SetEnvironmentVariableA("SteamGameId", "784150");

    if (!pSteamAPI_IsSteamRunning()) {
        if (!s_loggedWaitingForSteam) {
            Logf("republicmp  STEAM_P2P: Steam client is not running yet (TCP/LAN ready; Steam P2P will auto-enable when Steam starts)");
            s_loggedWaitingForSteam = true;
        }
        return false;
    }

    HSteamUser hUser = pSteamAPI_GetHSteamUser();
    if (!hUser) return false; // Wait until SOVIET64.exe has called SteamAPI_Init() on the main thread

    // SOVIET64.exe + C3DDLL64.dll + steam_api64.dll flat thunks are compiled against:
    // SteamUser020, SteamFriends017, SteamNetworking005
    static const char* const kUserVersions[] = { "SteamUser020", "SteamUser021", "SteamUser019" };
    static const char* const kFriendsVersions[] = { "SteamFriends017", "SteamFriends016", "SteamFriends015" };
    static const char* const kNetVersions[] = { "SteamNetworking005", "SteamNetworking006", "SteamNetworking004" };

    s_pSteamUser = FindInterface(hUser, kUserVersions, 3);
    s_pSteamFriends = FindInterface(hUser, kFriendsVersions, 3);
    s_pSteamNetworking = FindInterface(hUser, kNetVersions, 3);

    if (!s_pSteamUser || !s_pSteamFriends || !s_pSteamNetworking) {
        return false;
    }

    pISteamNetworking_AllowP2PPacketRelay(s_pSteamNetworking, true);

    if (!s_callbacksRegistered) {
        s_cbP2PRequest = { s_callbackVtable, 0, {0,0,0}, 1202, (int)sizeof(P2PSessionRequest_t), &OnP2PSessionRequest };
        s_cbP2PFail = { s_callbackVtable, 0, {0,0,0}, 1203, (int)sizeof(P2PSessionConnectFail_t), &OnP2PSessionConnectFail };
        s_cbRichPresenceJoin = { s_callbackVtable, 0, {0,0,0}, 337, (int)sizeof(GameRichPresenceJoinRequested_t), &OnRichPresenceJoinRequested };
        pSteamAPI_RegisterCallback(&s_cbP2PRequest, 1202);
        pSteamAPI_RegisterCallback(&s_cbP2PFail, 1203);
        pSteamAPI_RegisterCallback(&s_cbRichPresenceJoin, 337);
        s_callbacksRegistered = true;
    }

    s_available = true;
    uint64_t myId = pISteamUser_GetSteamID(s_pSteamUser);
    const char* myName = pISteamFriends_GetPersonaName(s_pSteamFriends);
    Logf("republicmp  STEAM_P2P: initialized (SteamID=%llu, name='%s', relay=enabled)",
         (unsigned long long)myId, myName ? myName : "");
    return true;
}

bool SteamP2P_IsAvailable()
{
    if (s_shuttingDown) return false;
    if (s_available) {
        if (!GetModuleHandleA("steam_api64.dll") || (pSteamAPI_GetHSteamUser && !pSteamAPI_GetHSteamUser())) {
            s_shuttingDown = true;
            s_available = false;
            return false;
        }
        return true;
    }
    SteamP2P_Init();
    return s_available;
}

uint64_t SteamP2P_GetLocalSteamId()
{
    if (!SteamP2P_IsAvailable()) return 0;
    return pISteamUser_GetSteamID(s_pSteamUser);
}

const char* SteamP2P_GetLocalPersonaName()
{
    if (!SteamP2P_IsAvailable()) return "";
    const char* name = pISteamFriends_GetPersonaName(s_pSteamFriends);
    return name ? name : "";
}

void SteamP2P_GetOnlineFriends(std::vector<SteamFriendEntry>& outFriends)
{
    outFriends.clear();
    if (!SteamP2P_IsAvailable()) return;

    const int k_EFriendFlagImmediate = 0x04;
    int count = pISteamFriends_GetFriendCount(s_pSteamFriends, k_EFriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        uint64_t fid = pISteamFriends_GetFriendByIndex(s_pSteamFriends, i, k_EFriendFlagImmediate);
        if (!fid) continue;
        int state = pISteamFriends_GetFriendPersonaState(s_pSteamFriends, fid);
        SteamFriendGameInfo_t gameInfo = {};
        bool inGame = pISteamFriends_GetFriendGamePlayed(s_pSteamFriends, fid, &gameInfo);
        uint32_t appId = (uint32_t)(gameInfo.m_gameID & 0xFFFFFFULL);
        bool inSoviet = inGame && (appId == 784150);

        if (state == 0 && !inSoviet) continue; // Offline

        SteamFriendEntry entry = {};
        entry.steamId = fid;
        const char* fname = pISteamFriends_GetFriendPersonaName(s_pSteamFriends, fid);
        strncpy_s(entry.name, fname ? fname : "Steam Friend", _TRUNCATE);
        entry.inThisGame = inSoviet;
        entry.personaState = state;
        const char* conn = pISteamFriends_GetFriendRichPresence(s_pSteamFriends, fid, "connect");
        if (conn) strncpy_s(entry.connectString, conn, _TRUNCATE);
        outFriends.push_back(entry);
    }

    std::sort(outFriends.begin(), outFriends.end(), [](const SteamFriendEntry& a, const SteamFriendEntry& b) {
        if (a.inThisGame != b.inThisGame) return a.inThisGame > b.inThisGame;
        return _stricmp(a.name, b.name) < 0;
    });
}

bool SteamP2P_InviteFriend(uint64_t friendSteamId)
{
    if (!SteamP2P_IsAvailable() || !friendSteamId) return false;
    uint64_t myId = SteamP2P_GetLocalSteamId();
    char connectCmd[64] = {};
    _snprintf_s(connectCmd, sizeof(connectCmd), _TRUNCATE, "+connect_steam %llu", (unsigned long long)myId);
    pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, friendSteamId);
    bool ok = pISteamFriends_InviteUserToGame(s_pSteamFriends, friendSteamId, connectCmd);
    Logf("republicmp  STEAM_P2P: sent game invite to %llu (%s)",
         (unsigned long long)friendSteamId, ok ? "ok" : "failed");
    return ok;
}

void SteamP2P_SetHostingPresence(bool hosting)
{
    if (!SteamP2P_IsAvailable()) return;
    if (hosting) {
        uint64_t myId = SteamP2P_GetLocalSteamId();
        char connectCmd[64] = {};
        _snprintf_s(connectCmd, sizeof(connectCmd), _TRUNCATE, "+connect_steam %llu", (unsigned long long)myId);
        pISteamFriends_SetRichPresence(s_pSteamFriends, "connect", connectCmd);
        pISteamFriends_SetRichPresence(s_pSteamFriends, "status", "RepublicMP Co-op");
    } else {
        pISteamFriends_ClearRichPresence(s_pSteamFriends);
    }
}

bool SteamP2P_ConsumePendingJoin(char* outTarget, size_t outSize)
{
    if (!s_lockReady) return false;
    bool found = false;
    EnterCriticalSection(&s_lock);
    if (s_pendingJoinTarget[0] != 0) {
        strncpy_s(outTarget, outSize, s_pendingJoinTarget, _TRUNCATE);
        s_pendingJoinTarget[0] = 0;
        found = true;
    }
    LeaveCriticalSection(&s_lock);
    return found;
}

uint64_t SteamP2P_ParseTargetSteamId(const char* target)
{
    if (!target) return 0;
    while (*target == ' ' || *target == '\t') ++target;
    if (_strnicmp(target, "steam:", 6) == 0) {
        target += 6;
        while (*target == ' ' || *target == '\t') ++target;
    }
    // Check if the remaining string is a 64-bit integer >= minimum individual SteamID64
    const char* p = target;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        ++digits;
        ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != 0 || digits < 15) return 0;
    uint64_t val = _strtoui64(target, nullptr, 10);
    return (val >= 76561197960265728ULL) ? val : 0;
}

// ---------------------------------------------------------------------------
// Virtual SOCKET Stream Multiplexer over Steam P2P Channels
// ---------------------------------------------------------------------------

#define STEAM_SOCKET_TAG    0x53544D0000000000ULL
#define STEAM_SOCKET_MASK   0xFFFFFF0000000000ULL
#define MAX_STEAM_SOCKETS   32
#define P2P_FRAME_MAGIC     0x504D5052U // 'RPMP'
#define P2P_MAX_CHUNK       32768U      // 32 KB reliable chunks

enum P2PFrameType : uint8_t {
    P2P_FRAME_SYN     = 1,
    P2P_FRAME_SYN_ACK = 2,
    P2P_FRAME_DATA    = 3,
    P2P_FRAME_FIN     = 4,
};

#pragma pack(push, 1)
struct P2PFrameHeader {
    uint32_t magic;
    uint8_t  type;
    uint16_t port;
    uint32_t connId;
    uint32_t payloadLen;
};
#pragma pack(pop)

struct PendingSyn {
    uint64_t remoteSteamId;
    unsigned short port;
    uint32_t connId;
    ULONGLONG receivedTick;
};

struct SteamSocketSlot {
    bool inUse;
    bool connected;
    bool remoteFin;
    bool localClosed;
    uint64_t remoteSteamId;
    unsigned short port;
    uint32_t connId;
    DWORD ioTimeoutMs;
    ULONGLONG lastActivityTick;
    std::deque<uint8_t> recvBuf;
};

static SteamSocketSlot s_slots[MAX_STEAM_SOCKETS] = {};
static std::deque<PendingSyn> s_pendingSyns;
static const int s_activeChannels[] = { 5555, 5556, 5557, 5558, 5560 };

bool SteamP2P_IsSteamSocket(SOCKET s)
{
    if (s == INVALID_SOCKET) return false;
    return (((uint64_t)s) & STEAM_SOCKET_MASK) == STEAM_SOCKET_TAG;
}

static int SlotIndexFromSocket(SOCKET s)
{
    if (!SteamP2P_IsSteamSocket(s)) return -1;
    int idx = (int)(((uint64_t)s) & 0xFFFFULL);
    if (idx < 0 || idx >= MAX_STEAM_SOCKETS) return -1;
    return idx;
}

static SOCKET MakeSteamSocket(int idx)
{
    return (SOCKET)(STEAM_SOCKET_TAG | (uint64_t)(idx & 0xFFFF));
}

static bool SendFrameLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                            uint32_t connId, const void* payload, uint32_t payloadLen)
{
    if (s_shuttingDown || !s_available || !s_pSteamNetworking) return false;
    if (!GetModuleHandleA("steam_api64.dll") || (pSteamAPI_GetHSteamUser && !pSteamAPI_GetHSteamUser())) {
        s_shuttingDown = true;
        s_available = false;
        return false;
    }
    std::vector<uint8_t> packet(sizeof(P2PFrameHeader) + payloadLen);
    P2PFrameHeader* hdr = (P2PFrameHeader*)packet.data();
    hdr->magic = P2P_FRAME_MAGIC;
    hdr->type = type;
    hdr->port = port;
    hdr->connId = connId;
    hdr->payloadLen = payloadLen;
    if (payloadLen > 0 && payload) {
        memcpy(packet.data() + sizeof(P2PFrameHeader), payload, payloadLen);
    }
    return pISteamNetworking_SendP2PPacket(
        s_pSteamNetworking,
        remoteSteamId,
        packet.data(),
        (uint32_t)packet.size(),
        k_EP2PSendReliable,
        (int)port);
}

static void PumpChannelLocked(int channel)
{
    if (s_shuttingDown || !s_available || !s_pSteamNetworking) return;
    if (!GetModuleHandleA("steam_api64.dll") || (pSteamAPI_GetHSteamUser && !pSteamAPI_GetHSteamUser())) {
        s_shuttingDown = true;
        s_available = false;
        return;
    }
    uint32_t msgSize = 0;
    while (pISteamNetworking_IsP2PPacketAvailable(s_pSteamNetworking, &msgSize, channel)) {
        if (msgSize == 0 || msgSize > (1024 * 1024)) {
            std::vector<uint8_t> discard(msgSize ? msgSize : 1);
            uint32_t readBytes = 0;
            uint64_t sender = 0;
            pISteamNetworking_ReadP2PPacket(s_pSteamNetworking, discard.data(), (uint32_t)discard.size(), &readBytes, &sender, channel);
            break;
        }
        std::vector<uint8_t> buf(msgSize);
        uint32_t readBytes = 0;
        uint64_t sender = 0;
        if (!pISteamNetworking_ReadP2PPacket(s_pSteamNetworking, buf.data(), msgSize, &readBytes, &sender, channel)) {
            break;
        }
        if (readBytes < sizeof(P2PFrameHeader)) continue;
        const P2PFrameHeader* hdr = (const P2PFrameHeader*)buf.data();
        if (hdr->magic != P2P_FRAME_MAGIC) continue;
        if (sizeof(P2PFrameHeader) + hdr->payloadLen > readBytes) continue;

        const uint8_t* payload = buf.data() + sizeof(P2PFrameHeader);
        ULONGLONG now = GetTickCount64();

        if (hdr->type == P2P_FRAME_SYN) {
            // If already accepted in an active slot, re-send SYN_ACK
            bool alreadyAccepted = false;
            for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
                if (s_slots[i].inUse && !s_slots[i].localClosed &&
                    s_slots[i].remoteSteamId == sender &&
                    s_slots[i].port == hdr->port &&
                    s_slots[i].connId == hdr->connId) {
                    SendFrameLocked(sender, hdr->port, P2P_FRAME_SYN_ACK, hdr->connId, nullptr, 0);
                    alreadyAccepted = true;
                    break;
                }
            }
            if (!alreadyAccepted) {
                bool queued = false;
                for (auto& syn : s_pendingSyns) {
                    if (syn.remoteSteamId == sender && syn.port == hdr->port && syn.connId == hdr->connId) {
                        syn.receivedTick = now;
                        queued = true;
                        break;
                    }
                }
                if (!queued) {
                    PendingSyn syn = { sender, hdr->port, hdr->connId, now };
                    s_pendingSyns.push_back(syn);
                }
            }
            continue;
        }

        if (hdr->type == P2P_FRAME_FIN) {
            for (auto it = s_pendingSyns.begin(); it != s_pendingSyns.end(); ) {
                if (it->remoteSteamId == sender && it->port == hdr->port && it->connId == hdr->connId) {
                    it = s_pendingSyns.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Match active slot
        for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
            SteamSocketSlot& slot = s_slots[i];
            if (!slot.inUse || slot.localClosed) continue;
            if (slot.remoteSteamId != sender || slot.port != hdr->port || slot.connId != hdr->connId) continue;

            slot.lastActivityTick = now;
            if (hdr->type == P2P_FRAME_SYN_ACK) {
                slot.connected = true;
            } else if (hdr->type == P2P_FRAME_DATA) {
                if (hdr->payloadLen > 0) {
                    slot.recvBuf.insert(slot.recvBuf.end(), payload, payload + hdr->payloadLen);
                }
            } else if (hdr->type == P2P_FRAME_FIN) {
                slot.remoteFin = true;
            }
            break;
        }
    }
}

static void PumpAllLocked()
{
    if (!s_available) return;
    for (int ch : s_activeChannels) {
        PumpChannelLocked(ch);
    }
}

void SteamP2P_FrameTick()
{
    if (!SteamP2P_IsAvailable()) return;
    EnterCriticalSection(&s_lock);
    if (pSteamAPI_RunCallbacks) pSteamAPI_RunCallbacks();
    PumpAllLocked();
    LeaveCriticalSection(&s_lock);
}

void SteamP2P_PrepareAccept(unsigned short port)
{
    if (!SteamP2P_IsAvailable()) return;
    EnterCriticalSection(&s_lock);
    // Pre-accept all online friends so their P2P packets arrive without waiting
    const int k_EFriendFlagImmediate = 0x04;
    int count = pISteamFriends_GetFriendCount(s_pSteamFriends, k_EFriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        uint64_t fid = pISteamFriends_GetFriendByIndex(s_pSteamFriends, i, k_EFriendFlagImmediate);
        if (fid && pISteamFriends_GetFriendPersonaState(s_pSteamFriends, fid) != 0) {
            pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, fid);
        }
    }
    PumpChannelLocked((int)port);
    // Prune stale SYNs older than 2s on this port (active clients re-send SYN every 400ms)
    ULONGLONG now = GetTickCount64();
    for (auto it = s_pendingSyns.begin(); it != s_pendingSyns.end(); ) {
        if (it->port == port && now - it->receivedTick > 2000) {
            it = s_pendingSyns.erase(it);
        } else {
            ++it;
        }
    }
    LeaveCriticalSection(&s_lock);
}

SOCKET SteamP2P_TryAcceptOne(unsigned short port, DWORD ioTimeoutMs)
{
    if (!SteamP2P_IsAvailable()) return INVALID_SOCKET;
    SOCKET result = INVALID_SOCKET;

    EnterCriticalSection(&s_lock);
    PumpChannelLocked((int)port);

    ULONGLONG now = GetTickCount64();
    for (auto it = s_pendingSyns.begin(); it != s_pendingSyns.end(); ) {
        if (it->port == port && now - it->receivedTick > 2000) {
            it = s_pendingSyns.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = s_pendingSyns.begin(); it != s_pendingSyns.end(); ++it) {
        if (it->port != port) continue;
        PendingSyn syn = *it;
        s_pendingSyns.erase(it);

        int slotIdx = -1;
        for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
            if (!s_slots[i].inUse) {
                slotIdx = i;
                break;
            }
        }
        if (slotIdx < 0) break;

        SteamSocketSlot& slot = s_slots[slotIdx];
        slot.inUse = true;
        slot.connected = true;
        slot.remoteFin = false;
        slot.localClosed = false;
        slot.remoteSteamId = syn.remoteSteamId;
        slot.port = port;
        slot.connId = syn.connId;
        slot.ioTimeoutMs = ioTimeoutMs;
        slot.lastActivityTick = GetTickCount64();
        slot.recvBuf.clear();

        pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, syn.remoteSteamId);
        SendFrameLocked(syn.remoteSteamId, port, P2P_FRAME_SYN_ACK, syn.connId, nullptr, 0);

        result = MakeSteamSocket(slotIdx);
        Logf("republicmp  STEAM_P2P: accepted client SteamID %llu on channel %u (connId=%08X)",
             (unsigned long long)syn.remoteSteamId, (unsigned)port, syn.connId);
        break;
    }

    LeaveCriticalSection(&s_lock);
    return result;
}

SOCKET SteamP2P_Connect(uint64_t remoteSteamId, unsigned short port,
                        DWORD connectTimeoutMs, DWORD ioTimeoutMs,
                        volatile bool* cancelFlag)
{
    if (!SteamP2P_IsAvailable() || !remoteSteamId) return INVALID_SOCKET;

    EnterCriticalSection(&s_lock);
    int slotIdx = -1;
    for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
        if (!s_slots[i].inUse) {
            slotIdx = i;
            break;
        }
    }
    if (slotIdx < 0) {
        LeaveCriticalSection(&s_lock);
        Logf("republicmp  STEAM_P2P: no free virtual socket slots");
        return INVALID_SOCKET;
    }

    uint32_t connId = (uint32_t)(GetTickCount64() ^ ((uint64_t)port << 16) ^ (uint64_t)(slotIdx + 1) * 0x9E3779B9U);
    if (connId == 0) connId = 1;

    SteamSocketSlot& slot = s_slots[slotIdx];
    slot.inUse = true;
    slot.connected = false;
    slot.remoteFin = false;
    slot.localClosed = false;
    slot.remoteSteamId = remoteSteamId;
    slot.port = port;
    slot.connId = connId;
    slot.ioTimeoutMs = ioTimeoutMs;
    slot.lastActivityTick = GetTickCount64();
    slot.recvBuf.clear();

    pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, remoteSteamId);
    SendFrameLocked(remoteSteamId, port, P2P_FRAME_SYN, connId, nullptr, 0);
    LeaveCriticalSection(&s_lock);

    Logf("republicmp  STEAM_P2P: connecting to SteamID %llu on channel %u (connId=%08X, timeout=%u ms)...",
         (unsigned long long)remoteSteamId, (unsigned)port, connId, connectTimeoutMs);

    ULONGLONG startTick = GetTickCount64();
    ULONGLONG lastSynSent = startTick;

    for (;;) {
        if (cancelFlag && *cancelFlag) {
            SteamP2P_CloseSocket(MakeSteamSocket(slotIdx));
            return INVALID_SOCKET;
        }

        EnterCriticalSection(&s_lock);
        PumpChannelLocked((int)port);
        bool connected = s_slots[slotIdx].connected;
        bool closed = s_slots[slotIdx].localClosed || s_slots[slotIdx].remoteFin;
        ULONGLONG now = GetTickCount64();
        if (!connected && !closed && now - lastSynSent >= 400) {
            SendFrameLocked(remoteSteamId, port, P2P_FRAME_SYN, connId, nullptr, 0);
            lastSynSent = now;
        }
        LeaveCriticalSection(&s_lock);

        if (connected) {
            Logf("republicmp  STEAM_P2P: connected to SteamID %llu on channel %u",
                 (unsigned long long)remoteSteamId, (unsigned)port);
            return MakeSteamSocket(slotIdx);
        }
        if (closed) {
            SteamP2P_CloseSocket(MakeSteamSocket(slotIdx));
            return INVALID_SOCKET;
        }
        if (connectTimeoutMs != 0 && (GetTickCount64() - startTick) >= connectTimeoutMs) {
            Logf("republicmp  STEAM_P2P: TIMEOUT connecting to SteamID %llu on channel %u",
                 (unsigned long long)remoteSteamId, (unsigned)port);
            SteamP2P_CloseSocket(MakeSteamSocket(slotIdx));
            return INVALID_SOCKET;
        }
        Sleep(20);
    }
}

bool SteamP2P_SendExact(SOCKET s, const void* data, size_t size)
{
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return false;

    const uint8_t* bytes = (const uint8_t*)data;
    size_t sent = 0;
    while (sent < size) {
        uint32_t chunk = (uint32_t)std::min(size - sent, (size_t)P2P_MAX_CHUNK);
        EnterCriticalSection(&s_lock);
        SteamSocketSlot& slot = s_slots[idx];
        if (!slot.inUse || slot.localClosed || slot.remoteFin) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        bool ok = SendFrameLocked(slot.remoteSteamId, slot.port, P2P_FRAME_DATA, slot.connId, bytes + sent, chunk);
        LeaveCriticalSection(&s_lock);
        if (!ok) {
            Logf("republicmp  STEAM_P2P: SendP2PPacket failed on channel %u", (unsigned)s_slots[idx].port);
            return false;
        }
        sent += chunk;
    }
    return true;
}

bool SteamP2P_RecvExact(SOCKET s, void* data, size_t size)
{
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return false;

    uint8_t* out = (uint8_t*)data;
    size_t received = 0;
    ULONGLONG waitStart = GetTickCount64();

    while (received < size) {
        EnterCriticalSection(&s_lock);
        SteamSocketSlot& slot = s_slots[idx];
        if (!slot.inUse || slot.localClosed) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        PumpChannelLocked((int)slot.port);

        size_t avail = slot.recvBuf.size();
        if (avail > 0) {
            size_t take = std::min(size - received, avail);
            for (size_t i = 0; i < take; ++i) {
                out[received + i] = slot.recvBuf.front();
                slot.recvBuf.pop_front();
            }
            received += take;
            waitStart = GetTickCount64();
            LeaveCriticalSection(&s_lock);
            continue;
        }

        bool eof = slot.remoteFin;
        if (!eof && (GetTickCount64() - waitStart) > 2500 &&
            s_pSteamNetworking && pISteamNetworking_GetP2PSessionState) {
            P2PSessionState_t state = {};
            if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot.remoteSteamId, &state)) {
                if (state.m_eP2PSessionError != 0) eof = true;
            }
        }
        DWORD timeoutMs = slot.ioTimeoutMs;
        LeaveCriticalSection(&s_lock);

        if (eof) return false;
        if (timeoutMs != 0 && (GetTickCount64() - waitStart) >= timeoutMs) {
            Logf("republicmp  STEAM_P2P: RecvExact timeout after %u ms", timeoutMs);
            return false;
        }
        Sleep(1);
    }
    return true;
}

bool SteamP2P_PeerGone(SOCKET s)
{
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return true;

    EnterCriticalSection(&s_lock);
    SteamSocketSlot& slot = s_slots[idx];
    if (!slot.inUse || slot.localClosed) {
        LeaveCriticalSection(&s_lock);
        return true;
    }
    PumpChannelLocked((int)slot.port);
    if (!slot.recvBuf.empty()) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    bool gone = slot.remoteFin;
    if (!gone && s_pSteamNetworking && pISteamNetworking_GetP2PSessionState) {
        P2PSessionState_t state = {};
        if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot.remoteSteamId, &state)) {
            if (state.m_eP2PSessionError != 0) gone = true;
        }
    }
    LeaveCriticalSection(&s_lock);
    return gone;
}

void SteamP2P_ShutdownSocket(SOCKET s)
{
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return;

    EnterCriticalSection(&s_lock);
    SteamSocketSlot& slot = s_slots[idx];
    if (slot.inUse && !slot.localClosed) {
        SendFrameLocked(slot.remoteSteamId, slot.port, P2P_FRAME_FIN, slot.connId, nullptr, 0);
        slot.localClosed = true;
    }
    LeaveCriticalSection(&s_lock);
}

void SteamP2P_CloseSocket(SOCKET s)
{
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return;

    EnterCriticalSection(&s_lock);
    SteamSocketSlot& slot = s_slots[idx];
    if (slot.inUse) {
        if (!slot.localClosed) {
            SendFrameLocked(slot.remoteSteamId, slot.port, P2P_FRAME_FIN, slot.connId, nullptr, 0);
        }
        slot.inUse = false;
        slot.connected = false;
        slot.localClosed = true;
        slot.remoteFin = true;
        slot.recvBuf.clear();
    }
    LeaveCriticalSection(&s_lock);
}

void SteamP2P_GetPeerName(SOCKET s, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = 0;
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return;

    uint64_t remoteId = 0;
    EnterCriticalSection(&s_lock);
    if (s_slots[idx].inUse) remoteId = s_slots[idx].remoteSteamId;
    LeaveCriticalSection(&s_lock);

    if (!remoteId) return;
    const char* persona = nullptr;
    if (s_available && s_pSteamFriends && pISteamFriends_GetFriendPersonaName) {
        persona = pISteamFriends_GetFriendPersonaName(s_pSteamFriends, remoteId);
    }
    if (persona && persona[0]) {
        _snprintf_s(out, outSize, _TRUNCATE, "Steam:%s (%llu)", persona, (unsigned long long)remoteId);
    } else {
        _snprintf_s(out, outSize, _TRUNCATE, "steam:%llu", (unsigned long long)remoteId);
    }
}
