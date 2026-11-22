// SPDX-License-Identifier: GPL-3.0-only
// See steam_p2p.h.
#include "steam_p2p.h"
#include "../session/save_sync.h"
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
static bool s_loggedNotLoggedOn = false;
static bool s_loggedWaitingForSteam = false;
static volatile bool s_shuttingDown = false;

void SteamP2P_UnregisterCallbacks();

struct SteamExitGuard {
    ~SteamExitGuard() {
        s_shuttingDown = true;
        s_available = false;
    }
};
static SteamExitGuard s_steamExitGuard;

static bool IsDualTestbedEnvActive()
{
    return SaveSync_IsDualTestbed();
}

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
static bool s_callbacksRegistered = false;

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

static void ResetSteamBindingsLocked()
{
    if (!s_shuttingDown && s_callbacksRegistered && s_hSteamDll != nullptr &&
        GetModuleHandleA("steam_api64.dll") == s_hSteamDll &&
        pSteamAPI_UnregisterCallback != nullptr) {
        pSteamAPI_UnregisterCallback(&s_cbP2PRequest);
        pSteamAPI_UnregisterCallback(&s_cbP2PFail);
        pSteamAPI_UnregisterCallback(&s_cbRichPresenceJoin);
    }

    s_hSteamDll = nullptr;
    s_available = false;
    s_callbacksRegistered = false;
    s_loggedNotLoggedOn = false;
    s_loggedWaitingForSteam = false;
    s_pSteamUser = nullptr;
    s_pSteamFriends = nullptr;
    s_pSteamNetworking = nullptr;

    pSteamAPI_IsSteamRunning = nullptr;
    pSteamAPI_RunCallbacks = nullptr;
    pSteamAPI_GetHSteamUser = nullptr;
    pSteamInternal_FindOrCreateUserInterface = nullptr;
    pSteamAPI_RegisterCallback = nullptr;
    pSteamAPI_UnregisterCallback = nullptr;

    pISteamUser_GetSteamID = nullptr;
    pISteamUser_BLoggedOn = nullptr;

    pISteamFriends_GetPersonaName = nullptr;
    pISteamFriends_GetFriendCount = nullptr;
    pISteamFriends_GetFriendByIndex = nullptr;
    pISteamFriends_GetFriendPersonaName = nullptr;
    pISteamFriends_GetFriendPersonaState = nullptr;
    pISteamFriends_GetFriendGamePlayed = nullptr;
    pISteamFriends_SetRichPresence = nullptr;
    pISteamFriends_ClearRichPresence = nullptr;
    pISteamFriends_GetFriendRichPresence = nullptr;
    pISteamFriends_RequestFriendRichPresence = nullptr;
    pISteamFriends_InviteUserToGame = nullptr;

    pISteamNetworking_SendP2PPacket = nullptr;
    pISteamNetworking_IsP2PPacketAvailable = nullptr;
    pISteamNetworking_ReadP2PPacket = nullptr;
    pISteamNetworking_AcceptP2PSessionWithUser = nullptr;
    pISteamNetworking_CloseP2PSessionWithUser = nullptr;
    pISteamNetworking_CloseP2PChannelWithUser = nullptr;
    pISteamNetworking_GetP2PSessionState = nullptr;
    pISteamNetworking_AllowP2PPacketRelay = nullptr;
}

static INIT_ONCE s_lockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_lock;
static char s_pendingJoinTarget[64] = "";

static bool VerifyActiveSteamModuleLocked();
static bool IsSteamP2PReadyLocked();

static void EnsureLock()
{
    InitOnceExecuteOnce(&s_lockOnce, [](PINIT_ONCE, PVOID param, PVOID*) -> BOOL {
        InitializeCriticalSection((CRITICAL_SECTION*)param);
        return TRUE;
    }, &s_lock, nullptr);
}

static void OnP2PSessionRequest(void* pvParam)
{
    P2PSessionRequest_t* req = (P2PSessionRequest_t*)pvParam;
    if (!req) return;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    bool accepted = false;
    uint64_t remoteId = req->m_steamIDRemote;
    if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
        pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, remoteId);
        accepted = true;
    }
    LeaveCriticalSection(&s_lock);
    if (accepted) {
        Logf("republicmp  STEAM_P2P: accepted incoming P2P session request from SteamID %llu",
             (unsigned long long)remoteId);
    }
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
        char logTarget[64] = {};
        EnsureLock();
        EnterCriticalSection(&s_lock);
        _snprintf_s(s_pendingJoinTarget, sizeof(s_pendingJoinTarget), _TRUNCATE,
                    "steam:%llu", (unsigned long long)targetId);
        strncpy_s(logTarget, sizeof(logTarget), s_pendingJoinTarget, _TRUNCATE);
        LeaveCriticalSection(&s_lock);
        Logf("republicmp  STEAM_P2P: Join Game requested via Steam Friends -> %s", logTarget);
    }
}

void SteamP2P_UnregisterCallbacks()
{
    EnsureLock();
    EnterCriticalSection(&s_lock);
    bool unregistered = false;
    if (s_callbacksRegistered) {
        HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
        if (s_hSteamDll != nullptr && hSteam == s_hSteamDll && pSteamAPI_UnregisterCallback != nullptr) {
            pSteamAPI_UnregisterCallback(&s_cbP2PRequest);
            pSteamAPI_UnregisterCallback(&s_cbP2PFail);
            pSteamAPI_UnregisterCallback(&s_cbRichPresenceJoin);
            s_callbacksRegistered = false;
            unregistered = true;
        } else {
            ResetSteamBindingsLocked();
        }
    }
    LeaveCriticalSection(&s_lock);
    if (unregistered) {
        Logf("republicmp  STEAM_P2P: unmounted callbacks 1202, 1203, 337");
    }
}

static void EnsureCallbacksRegistered()
{
    if (!s_callbacksRegistered && pSteamAPI_RegisterCallback) {
        s_cbP2PRequest = { s_callbackVtable, 0, {0,0,0}, 1202, (int)sizeof(P2PSessionRequest_t), &OnP2PSessionRequest };
        s_cbP2PFail = { s_callbackVtable, 0, {0,0,0}, 1203, (int)sizeof(P2PSessionConnectFail_t), &OnP2PSessionConnectFail };
        s_cbRichPresenceJoin = { s_callbackVtable, 0, {0,0,0}, 337, (int)sizeof(GameRichPresenceJoinRequested_t), &OnRichPresenceJoinRequested };
        pSteamAPI_RegisterCallback(&s_cbP2PRequest, 1202);
        pSteamAPI_RegisterCallback(&s_cbP2PFail, 1203);
        pSteamAPI_RegisterCallback(&s_cbRichPresenceJoin, 337);
        s_callbacksRegistered = true;
        Logf("republicmp  STEAM_P2P: mounted callbacks 1202, 1203, 337");
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
    if (s_shuttingDown) return false;
    if (IsDualTestbedEnvActive()) {
        static bool s_loggedTestbedSkip = false;
        if (!s_loggedTestbedSkip) {
            Logf("republicmp  STEAM_P2P: disabled in local dual-window testbed mode (using localhost TCP)");
            s_loggedTestbedSkip = true;
        }
        return false;
    }
    EnsureLock();
    EnterCriticalSection(&s_lock);
    HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
    if (!hSteam) {
        ResetSteamBindingsLocked();
        LeaveCriticalSection(&s_lock);
        return false;
    }
    if (s_hSteamDll && s_hSteamDll != hSteam) {
        ResetSteamBindingsLocked();
    }
    if (s_available) {
        if (pSteamAPI_GetHSteamUser && pSteamAPI_GetHSteamUser() &&
            pSteamAPI_IsSteamRunning && pSteamAPI_IsSteamRunning() &&
            s_pSteamUser && pISteamUser_BLoggedOn && pISteamUser_BLoggedOn(s_pSteamUser)) {
            LeaveCriticalSection(&s_lock);
            return true;
        }
        ResetSteamBindingsLocked();
    }

    static ULONGLONG s_lastAttemptTick = 0;
    ULONGLONG now = GetTickCount64();
    if (s_initAttempted && (now - s_lastAttemptTick < 3000)) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    s_initAttempted = true;
    s_lastAttemptTick = now;

    s_hSteamDll = hSteam;

#define RESOLVE_FN(var, type, name) \
    var = (type)GetProcAddress(s_hSteamDll, name); \
    if (!var) { ResetSteamBindingsLocked(); LeaveCriticalSection(&s_lock); Logf("republicmp  STEAM_P2P: missing export %s", name); return false; }

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
        LeaveCriticalSection(&s_lock);
        return false;
    }

    HSteamUser hUser = pSteamAPI_GetHSteamUser();
    if (!hUser) {
        LeaveCriticalSection(&s_lock);
        return false; // Wait until SOVIET64.exe has called SteamAPI_Init() on the main thread
    }

    // SOVIET64.exe + C3DDLL64.dll + steam_api64.dll flat thunks are compiled against:
    // SteamUser020, SteamFriends017, SteamNetworking005
    static const char* const kUserVersions[] = { "SteamUser020", "SteamUser021", "SteamUser019" };
    static const char* const kFriendsVersions[] = { "SteamFriends017", "SteamFriends016", "SteamFriends015" };
    static const char* const kNetVersions[] = { "SteamNetworking005", "SteamNetworking006", "SteamNetworking004" };

    s_pSteamUser = FindInterface(hUser, kUserVersions, 3);
    s_pSteamFriends = FindInterface(hUser, kFriendsVersions, 3);
    s_pSteamNetworking = FindInterface(hUser, kNetVersions, 3);

    if (!s_pSteamUser || !s_pSteamFriends || !s_pSteamNetworking) {
        LeaveCriticalSection(&s_lock);
        return false;
    }

    if (!pISteamUser_BLoggedOn(s_pSteamUser)) {
        if (!s_loggedNotLoggedOn) {
            Logf("republicmp  STEAM_P2P: Steam user is not logged on (offline mode) - Steam P2P disabled");
            s_loggedNotLoggedOn = true;
        }
        LeaveCriticalSection(&s_lock);
        return false;
    }

    pISteamNetworking_AllowP2PPacketRelay(s_pSteamNetworking, true);

    EnsureCallbacksRegistered();

    s_available = true;
    if (pISteamFriends_ClearRichPresence) {
        pISteamFriends_ClearRichPresence(s_pSteamFriends);
    }

    static bool s_checkedCmdLine = false;
    if (!s_checkedCmdLine) {
        s_checkedCmdLine = true;
        const char* cmdLine = GetCommandLineA();
        const char* p = cmdLine ? strstr(cmdLine, "+connect_steam ") : nullptr;
        if (p) {
            uint64_t parsed = _strtoui64(p + 15, nullptr, 10);
            if (parsed >= 76561197960265728ULL) {
                _snprintf_s(s_pendingJoinTarget, sizeof(s_pendingJoinTarget), _TRUNCATE,
                            "steam:%llu", (unsigned long long)parsed);
                Logf("republicmp  STEAM_P2P: +connect_steam from command line -> %s", s_pendingJoinTarget);
            }
        }
    }

    uint64_t myId = pISteamUser_GetSteamID(s_pSteamUser);
    const char* myName = pISteamFriends_GetPersonaName(s_pSteamFriends);
    LeaveCriticalSection(&s_lock);
    Logf("republicmp  STEAM_P2P: initialized (SteamID=%llu, name='%s', relay=enabled)",
         (unsigned long long)myId, myName ? myName : "");
    return true;
}

static bool VerifyActiveSteamModuleLocked()
{
    HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
    if (!hSteam || hSteam != s_hSteamDll || !pSteamAPI_GetHSteamUser || !pSteamAPI_GetHSteamUser()) {
        ResetSteamBindingsLocked();
        return false;
    }
    return true;
}

static bool IsSteamP2PReadyLocked()
{
    if (s_shuttingDown || !s_available || !s_hSteamDll) return false;
    if (!s_pSteamUser || !s_pSteamFriends || !s_pSteamNetworking) return false;
    if (!pSteamAPI_IsSteamRunning || !pSteamAPI_RunCallbacks || !pSteamAPI_GetHSteamUser) return false;
    if (!pISteamUser_GetSteamID || !pISteamUser_BLoggedOn) return false;
    if (!pISteamFriends_GetPersonaName || !pISteamFriends_GetFriendCount ||
        !pISteamFriends_GetFriendByIndex || !pISteamFriends_GetFriendPersonaName ||
        !pISteamFriends_GetFriendPersonaState || !pISteamFriends_GetFriendGamePlayed ||
        !pISteamFriends_SetRichPresence || !pISteamFriends_ClearRichPresence ||
        !pISteamFriends_GetFriendRichPresence || !pISteamFriends_RequestFriendRichPresence ||
        !pISteamFriends_InviteUserToGame) {
        return false;
    }
    if (!pISteamNetworking_SendP2PPacket || !pISteamNetworking_IsP2PPacketAvailable ||
        !pISteamNetworking_ReadP2PPacket || !pISteamNetworking_AcceptP2PSessionWithUser ||
        !pISteamNetworking_GetP2PSessionState) {
        return false;
    }
    return true;
}

static bool IsSteamUserLoggedOnLocked(HMODULE hSteam)
{
    if (!hSteam) {
        ResetSteamBindingsLocked();
        return false;
    }
    if (s_hSteamDll != hSteam) {
        ResetSteamBindingsLocked();
    }

    Fn_SteamAPI_IsSteamRunning fnIsRunning = pSteamAPI_IsSteamRunning
        ? pSteamAPI_IsSteamRunning
        : (Fn_SteamAPI_IsSteamRunning)GetProcAddress(hSteam, "SteamAPI_IsSteamRunning");
    Fn_SteamAPI_GetHSteamUser fnGetUser = pSteamAPI_GetHSteamUser
        ? pSteamAPI_GetHSteamUser
        : (Fn_SteamAPI_GetHSteamUser)GetProcAddress(hSteam, "SteamAPI_GetHSteamUser");
    Fn_SteamInternal_FindOrCreateUserInterface fnFindIface = pSteamInternal_FindOrCreateUserInterface
        ? pSteamInternal_FindOrCreateUserInterface
        : (Fn_SteamInternal_FindOrCreateUserInterface)GetProcAddress(hSteam, "SteamInternal_FindOrCreateUserInterface");
    Fn_ISteamUser_BLoggedOn fnLoggedOn = pISteamUser_BLoggedOn
        ? pISteamUser_BLoggedOn
        : (Fn_ISteamUser_BLoggedOn)GetProcAddress(hSteam, "SteamAPI_ISteamUser_BLoggedOn");

    if (!fnIsRunning || !fnGetUser || !fnFindIface || !fnLoggedOn) return false;
    if (!fnIsRunning()) {
        ResetSteamBindingsLocked();
        return false;
    }

    HSteamUser hUser = fnGetUser();
    if (!hUser) {
        ResetSteamBindingsLocked();
        return false;
    }

    void* userIface = (s_available && s_hSteamDll == hSteam) ? s_pSteamUser : nullptr;
    if (!userIface) {
        static const char* const kUserVersions[] = { "SteamUser020", "SteamUser021", "SteamUser019" };
        for (int i = 0; i < 3 && !userIface; ++i) {
            userIface = fnFindIface(hUser, kUserVersions[i]);
        }
    }
    return (userIface != nullptr) && fnLoggedOn(userIface);
}

bool SteamP2P_IsLoggedOn()
{
    if (s_shuttingDown || IsDualTestbedEnvActive()) return false;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
    bool loggedOn = IsSteamUserLoggedOnLocked(hSteam);
    LeaveCriticalSection(&s_lock);
    return loggedOn;
}

bool SteamP2P_IsAvailable()
{
    if (s_shuttingDown || IsDualTestbedEnvActive()) return false;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (s_available) {
        if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
            ResetSteamBindingsLocked();
            LeaveCriticalSection(&s_lock);
            return false;
        }
        if (!pSteamAPI_IsSteamRunning() || !pISteamUser_BLoggedOn(s_pSteamUser)) {
            ResetSteamBindingsLocked();
            LeaveCriticalSection(&s_lock);
            return false;
        }
        LeaveCriticalSection(&s_lock);
        return true;
    }
    LeaveCriticalSection(&s_lock);
    return SteamP2P_Init();
}

uint64_t SteamP2P_GetLocalSteamId()
{
    if (!SteamP2P_IsAvailable()) return 0;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    uint64_t id = (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked())
        ? pISteamUser_GetSteamID(s_pSteamUser)
        : 0;
    LeaveCriticalSection(&s_lock);
    return id;
}

std::string SteamP2P_GetLocalPersonaNameCopy()
{
    if (!SteamP2P_IsAvailable()) return std::string();
    EnsureLock();
    EnterCriticalSection(&s_lock);
    const char* name = (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked())
        ? pISteamFriends_GetPersonaName(s_pSteamFriends)
        : nullptr;
    std::string copy = (name != nullptr) ? std::string(name) : std::string();
    LeaveCriticalSection(&s_lock);
    return copy;
}

const char* SteamP2P_GetLocalPersonaName()
{
    static thread_local char tlsPersona[256] = {};
    const std::string copy = SteamP2P_GetLocalPersonaNameCopy();
    strncpy_s(tlsPersona, sizeof(tlsPersona), copy.c_str(), _TRUNCATE);
    return tlsPersona;
}

static volatile bool s_isHostingOpen = false;
static volatile bool s_hostProbeDirty = false;

struct FriendHostProbe {
    uint64_t steamId;
    bool hasPong;
    bool isOpen;
    ULONGLONG lastPingSentTick;
    ULONGLONG lastPongRecvTick;
};

static std::vector<FriendHostProbe> s_hostProbes;

static FriendHostProbe* FindOrCreateHostProbeLocked(uint64_t steamId)
{
    for (auto& fp : s_hostProbes) {
        if (fp.steamId == steamId) return &fp;
    }
    if (s_hostProbes.size() >= 256) {
        s_hostProbes.erase(s_hostProbes.begin());
    }
    FriendHostProbe fp = {};
    fp.steamId = steamId;
    s_hostProbes.push_back(fp);
    return &s_hostProbes.back();
}

static void PumpChannelLocked(int channel);
static bool SendUnreliableFrameLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                                      uint32_t connId, const void* payload, uint32_t payloadLen);

bool SteamP2P_ConsumeHostProbeDirty()
{
    EnsureLock();
    bool dirty = false;
    EnterCriticalSection(&s_lock);
    dirty = s_hostProbeDirty;
    s_hostProbeDirty = false;
    LeaveCriticalSection(&s_lock);
    return dirty;
}

void SteamP2P_GetOnlineFriends(std::vector<SteamFriendEntry>& outFriends)
{
    outFriends.clear();
    if (!SteamP2P_IsAvailable()) return;

    ULONGLONG now = GetTickCount64();
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    PumpChannelLocked(5555);
    if (!IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return;
    }

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

        // Only consider friends who are currently playing Workers & Resources: Soviet Republic (AppID 784150)
        if (!inSoviet) continue;

        pISteamFriends_RequestFriendRichPresence(s_pSteamFriends, fid);

        const char* conn = pISteamFriends_GetFriendRichPresence(s_pSteamFriends, fid, "connect");
        const char* hostFlag = pISteamFriends_GetFriendRichPresence(s_pSteamFriends, fid, "republicmp_hosting");
        bool rpHosting = (conn && strstr(conn, "+connect_steam") != nullptr) ||
                         (hostFlag && strcmp(hostFlag, "1") == 0);

        bool isHosting = rpHosting;
        FriendHostProbe* fp = FindOrCreateHostProbeLocked(fid);
        if (fp) {
            if (fp->lastPingSentTick == 0 || (now - fp->lastPingSentTick) >= 1200) {
                fp->lastPingSentTick = now;
                pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, fid);
                SendUnreliableFrameLocked(fid, 5555, 5 /* P2P_FRAME_HOST_PING */, 0, nullptr, 0);
                if (!IsSteamP2PReadyLocked()) break;
            }
            if (fp->hasPong && (now - fp->lastPongRecvTick) <= 6000) {
                isHosting = fp->isOpen;
            }
        }

        // Only display friends who are ALREADY actively hosting an open Co-op server
        if (!isHosting) continue;

        SteamFriendEntry entry = {};
        entry.steamId = fid;
        const char* fname = pISteamFriends_GetFriendPersonaName(s_pSteamFriends, fid);
        strncpy_s(entry.name, fname ? fname : "Steam Friend", _TRUNCATE);
        entry.inThisGame = true;
        entry.isHosting = true;
        entry.personaState = state;
        if (conn && conn[0]) {
            strncpy_s(entry.connectString, conn, _TRUNCATE);
        } else {
            _snprintf_s(entry.connectString, sizeof(entry.connectString), _TRUNCATE,
                        "+connect_steam %llu", (unsigned long long)fid);
        }
        outFriends.push_back(entry);
    }
    LeaveCriticalSection(&s_lock);

    std::sort(outFriends.begin(), outFriends.end(), [](const SteamFriendEntry& a, const SteamFriendEntry& b) {
        return _stricmp(a.name, b.name) < 0;
    });
}

bool SteamP2P_InviteFriend(uint64_t friendSteamId)
{
    if (!SteamP2P_IsAvailable() || !friendSteamId) return false;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    uint64_t myId = pISteamUser_GetSteamID(s_pSteamUser);
    char connectCmd[64] = {};
    _snprintf_s(connectCmd, sizeof(connectCmd), _TRUNCATE, "+connect_steam %llu", (unsigned long long)myId);
    pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, friendSteamId);
    bool ok = pISteamFriends_InviteUserToGame(s_pSteamFriends, friendSteamId, connectCmd);
    LeaveCriticalSection(&s_lock);
    Logf("republicmp  STEAM_P2P: sent game invite to %llu (%s)",
         (unsigned long long)friendSteamId, ok ? "ok" : "failed");
    return ok;
}

void SteamP2P_SetHostingPresence(bool hosting)
{
    s_isHostingOpen = hosting;
    if (!SteamP2P_IsAvailable()) return;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    if (hosting) {
        uint64_t myId = pISteamUser_GetSteamID(s_pSteamUser);
        char connectCmd[64] = {};
        _snprintf_s(connectCmd, sizeof(connectCmd), _TRUNCATE, "+connect_steam %llu", (unsigned long long)myId);
        pISteamFriends_SetRichPresence(s_pSteamFriends, "connect", connectCmd);
        pISteamFriends_SetRichPresence(s_pSteamFriends, "republicmp_hosting", "1");
        pISteamFriends_SetRichPresence(s_pSteamFriends, "status", "RepublicMP Co-op (Open)");

        const int k_EFriendFlagImmediate = 0x04;
        int count = pISteamFriends_GetFriendCount(s_pSteamFriends, k_EFriendFlagImmediate);
        for (int i = 0; i < count; ++i) {
            uint64_t fid = pISteamFriends_GetFriendByIndex(s_pSteamFriends, i, k_EFriendFlagImmediate);
            if (!fid) continue;
            SteamFriendGameInfo_t gameInfo = {};
            bool inGame = pISteamFriends_GetFriendGamePlayed(s_pSteamFriends, fid, &gameInfo);
            if (inGame && (uint32_t)(gameInfo.m_gameID & 0xFFFFFFULL) == 784150) {
                pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, fid);
            }
        }
    } else {
        pISteamFriends_ClearRichPresence(s_pSteamFriends);
    }
    LeaveCriticalSection(&s_lock);
}

bool SteamP2P_ConsumePendingJoin(char* outTarget, size_t outSize)
{
    if (!outTarget || outSize == 0) return false;
    outTarget[0] = '\0';
    EnsureLock();
    bool found = false;
    EnterCriticalSection(&s_lock);
    size_t len = strlen(s_pendingJoinTarget);
    if (len > 0 && len < outSize) {
        memcpy(outTarget, s_pendingJoinTarget, len + 1);
        s_pendingJoinTarget[0] = 0;
        found = true;
    }
    LeaveCriticalSection(&s_lock);
    return found;
}

void SteamP2P_SetPendingJoinForTest(const char* target)
{
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (target && target[0]) {
        strncpy_s(s_pendingJoinTarget, sizeof(s_pendingJoinTarget), target, _TRUNCATE);
    } else {
        s_pendingJoinTarget[0] = '\0';
    }
    LeaveCriticalSection(&s_lock);
}

uint64_t SteamP2P_ParseTargetSteamId(const char* target)
{
    if (!target) return 0;
    auto isWs = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (isWs(*target)) ++target;
    if (_strnicmp(target, "steam:", 6) == 0) {
        target += 6;
        while (isWs(*target)) ++target;
    }
    // Reject empty, sign prefixes, non-digits, and leading zeros.
    if (*target < '1' || *target > '9') return 0;

    uint64_t val = 0;
    int digits = 0;
    while (*target >= '0' && *target <= '9') {
        uint64_t d = (uint64_t)(*target - '0');
        if (val > (UINT64_MAX - d) / 10ULL) return 0;
        val = val * 10ULL + d;
        ++digits;
        ++target;
    }
    while (isWs(*target)) ++target;
    if (*target != '\0' || digits < 15 || digits > 20) return 0;
    return (val >= 76561197960265728ULL) ? val : 0;
}

// ---------------------------------------------------------------------------
// Virtual SOCKET Stream Multiplexer over Steam P2P Channels
// ---------------------------------------------------------------------------

#define STEAM_SOCKET_TAG    0x53544D0000000000ULL
#define STEAM_SOCKET_MASK   0xFFFFFF0000000000ULL
#define MAX_STEAM_SOCKETS   32
#define P2P_FRAME_MAGIC     0x504D5052U // 'RPMP'
#define P2P_MAX_CHUNK       16384U      // 16 KB reliable chunks
#define P2P_SEND_QUEUE_CAP  (256 * 1024) // Keep at most 256 KB in flight in Steam's reliable buffer

enum P2PFrameType : uint8_t {
    P2P_FRAME_SYN       = 1,
    P2P_FRAME_SYN_ACK   = 2,
    P2P_FRAME_DATA      = 3,
    P2P_FRAME_FIN       = 4,
    P2P_FRAME_HOST_PING = 5,
    P2P_FRAME_HOST_PONG = 6,
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
static const int s_activeChannels[] = { 5555, 5556, 5560 };

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

static bool SendFrameWithKindLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                                    uint32_t connId, const void* payload, uint32_t payloadLen, int sendType)
{
    if (s_shuttingDown || !s_available || !s_pSteamNetworking) return false;
    if (!VerifyActiveSteamModuleLocked() || !pISteamNetworking_SendP2PPacket) {
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
        sendType,
        (int)port);
}

static bool SendFrameLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                            uint32_t connId, const void* payload, uint32_t payloadLen)
{
    return SendFrameWithKindLocked(remoteSteamId, port, type, connId, payload, payloadLen, k_EP2PSendReliable);
}

static bool SendUnreliableFrameLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                                      uint32_t connId, const void* payload, uint32_t payloadLen)
{
    return SendFrameWithKindLocked(remoteSteamId, port, type, connId, payload, payloadLen, k_EP2PSendUnreliableNoDelay);
}

static constexpr size_t kMaxPendingSyns = 32;
static constexpr size_t kMaxSlotRecvBufBytes = 16u * 1024u * 1024u;
static constexpr int kMaxPumpFramesPerCall = 64;
static constexpr uint32_t kMaxDiscardScratchBytes = 65536u;

static void PruneStalePendingSynsLocked(ULONGLONG now, unsigned short portFilter)
{
    for (auto it = s_pendingSyns.begin(); it != s_pendingSyns.end(); ) {
        if ((portFilter == 0 || it->port == portFilter) && (now - it->receivedTick) > 2000ULL) {
            it = s_pendingSyns.erase(it);
        } else {
            ++it;
        }
    }
}

static void HandleSynFrameLocked(uint64_t sender, const P2PFrameHeader* hdr, ULONGLONG now)
{
    for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
        if (s_slots[i].inUse && !s_slots[i].localClosed &&
            s_slots[i].remoteSteamId == sender &&
            s_slots[i].port == hdr->port &&
            s_slots[i].connId == hdr->connId) {
            SendFrameLocked(sender, hdr->port, P2P_FRAME_SYN_ACK, hdr->connId, nullptr, 0);
            return;
        }
    }
    for (auto& syn : s_pendingSyns) {
        if (syn.remoteSteamId == sender && syn.port == hdr->port && syn.connId == hdr->connId) {
            syn.receivedTick = now;
            return;
        }
    }
    if (s_pendingSyns.size() >= kMaxPendingSyns) {
        s_pendingSyns.pop_front();
    }
    PendingSyn syn = { sender, hdr->port, hdr->connId, now };
    s_pendingSyns.push_back(syn);
}

static void HandleSlotFrameLocked(uint64_t sender, const P2PFrameHeader* hdr,
                                  const uint8_t* payload, ULONGLONG now)
{
    if (hdr->type == P2P_FRAME_FIN) {
        for (auto it = s_pendingSyns.begin(); it != s_pendingSyns.end(); ) {
            if (it->remoteSteamId == sender && it->port == hdr->port && it->connId == hdr->connId) {
                it = s_pendingSyns.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
        SteamSocketSlot& slot = s_slots[i];
        if (!slot.inUse || slot.localClosed) continue;
        if (slot.remoteSteamId != sender || slot.port != hdr->port || slot.connId != hdr->connId) continue;

        slot.lastActivityTick = now;
        if (hdr->type == P2P_FRAME_SYN_ACK) {
            slot.connected = true;
        } else if (hdr->type == P2P_FRAME_DATA) {
            if (hdr->payloadLen > 0) {
                if (slot.recvBuf.size() + hdr->payloadLen > kMaxSlotRecvBufBytes) {
                    Logf("republicmp  STEAM_P2P: recvBuf overflow (>%zu bytes) on channel %u, closing slot",
                         kMaxSlotRecvBufBytes, (unsigned)slot.port);
                    slot.remoteFin = true;
                } else {
                    slot.recvBuf.insert(slot.recvBuf.end(), payload, payload + hdr->payloadLen);
                }
            }
        } else if (hdr->type == P2P_FRAME_FIN) {
            slot.remoteFin = true;
        }
        break;
    }
}

static void PumpChannelLocked(int channel)
{
    if (s_shuttingDown || !s_available || !s_pSteamNetworking) return;
    if (!VerifyActiveSteamModuleLocked() || !pISteamNetworking_IsP2PPacketAvailable || !pISteamNetworking_ReadP2PPacket) {
        return;
    }
    uint32_t msgSize = 0;
    for (int pumped = 0;
         pumped < kMaxPumpFramesPerCall && pISteamNetworking_IsP2PPacketAvailable(s_pSteamNetworking, &msgSize, channel);
         ++pumped) {
        if (msgSize == 0 || msgSize > (1024u * 1024u)) {
            uint32_t discardCap = (msgSize == 0) ? 1u : std::min<uint32_t>(msgSize, kMaxDiscardScratchBytes);
            std::vector<uint8_t> discard(discardCap);
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

        if (hdr->type == P2P_FRAME_HOST_PING) {
            uint8_t openByte = s_isHostingOpen ? 1 : 0;
            SendUnreliableFrameLocked(sender, hdr->port, P2P_FRAME_HOST_PONG, 0, &openByte, 1);
            if (!s_available) break;
            continue;
        }

        if (hdr->type == P2P_FRAME_HOST_PONG) {
            bool isOpen = (hdr->payloadLen >= 1 && payload[0] != 0);
            FriendHostProbe* fp = FindOrCreateHostProbeLocked(sender);
            if (fp) {
                if (!fp->hasPong || fp->isOpen != isOpen) {
                    s_hostProbeDirty = true;
                }
                fp->hasPong = true;
                fp->isOpen = isOpen;
                fp->lastPongRecvTick = now;
            }
            continue;
        }

        if (hdr->type == P2P_FRAME_SYN) {
            HandleSynFrameLocked(sender, hdr, now);
            if (!s_available) break;
            continue;
        }

        HandleSlotFrameLocked(sender, hdr, payload, now);
    }
    PruneStalePendingSynsLocked(GetTickCount64(), 0);
}

static bool HasActiveSteamSocketLocked()
{
    for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
        if (s_slots[i].inUse && !s_slots[i].localClosed) {
            return true;
        }
    }
    return false;
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
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    bool activeSocket = HasActiveSteamSocketLocked();
    if (!activeSocket) {
        static ULONGLONG s_lastIdlePumpTick = 0;
        ULONGLONG now = GetTickCount64();
        if (s_lastIdlePumpTick != 0 && (now - s_lastIdlePumpTick) < 50) {
            PruneStalePendingSynsLocked(now, 0);
            LeaveCriticalSection(&s_lock);
            return;
        }
        s_lastIdlePumpTick = now;
        pSteamAPI_RunCallbacks();
        PumpChannelLocked(5555);
        if (s_isHostingOpen) {
            PumpChannelLocked(5556);
        }
        PruneStalePendingSynsLocked(now, 0);
        LeaveCriticalSection(&s_lock);
        return;
    }
    pSteamAPI_RunCallbacks();
    PumpAllLocked();
    LeaveCriticalSection(&s_lock);
}

void SteamP2P_PrepareAccept(unsigned short port)
{
    if (!SteamP2P_IsAvailable()) return;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    pSteamAPI_RunCallbacks();
    PumpChannelLocked((int)port);
    // Prune stale SYNs older than 2s on this port (active clients re-send SYN every 400ms)
    PruneStalePendingSynsLocked(GetTickCount64(), port);
    LeaveCriticalSection(&s_lock);
}

SOCKET SteamP2P_TryAcceptOne(unsigned short port, DWORD ioTimeoutMs)
{
    if (!SteamP2P_IsAvailable()) return INVALID_SOCKET;
    SOCKET result = INVALID_SOCKET;

    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return INVALID_SOCKET;
    }
    PumpChannelLocked((int)port);
    if (!IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return INVALID_SOCKET;
    }

    PruneStalePendingSynsLocked(GetTickCount64(), port);

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

    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) {
        LeaveCriticalSection(&s_lock);
        return INVALID_SOCKET;
    }
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
    EnsureLock();
    while (sent < size) {
        uint32_t chunk = (uint32_t)std::min(size - sent, (size_t)P2P_MAX_CHUNK);
        ULONGLONG stallStart = GetTickCount64();
        int32_t minQueuedSeen = INT32_MAX;

        for (;;) {
            EnterCriticalSection(&s_lock);
            SteamSocketSlot& slot = s_slots[idx];
            if (!slot.inUse || slot.localClosed || slot.remoteFin) {
                LeaveCriticalSection(&s_lock);
                return false;
            }
            PumpChannelLocked((int)slot.port);

            int32_t queuedBytes = 0;
            uint8_t sessionErr = 0;
            if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
                P2PSessionState_t state = {};
                if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot.remoteSteamId, &state)) {
                    queuedBytes = state.m_nBytesQueuedForSend;
                    sessionErr = state.m_eP2PSessionError;
                }
            }
            if (sessionErr != 0) {
                unsigned short port = slot.port;
                LeaveCriticalSection(&s_lock);
                Logf("republicmp  STEAM_P2P: SendExact aborted, P2P session error=%u on channel %u",
                     (unsigned)sessionErr, (unsigned)port);
                return false;
            }
            if (queuedBytes <= P2P_SEND_QUEUE_CAP) {
                bool ok = SendFrameLocked(slot.remoteSteamId, slot.port, P2P_FRAME_DATA, slot.connId, bytes + sent, chunk);
                unsigned short port = slot.port;
                LeaveCriticalSection(&s_lock);
                if (!ok) {
                    Logf("republicmp  STEAM_P2P: SendP2PPacket failed on channel %u (queued=%d, sent=%zu/%zu)",
                         (unsigned)port, (int)queuedBytes, sent, size);
                    return false;
                }
                break;
            }
            unsigned short port = slot.port;
            LeaveCriticalSection(&s_lock);

            if (queuedBytes < minQueuedSeen) {
                minQueuedSeen = queuedBytes;
                stallStart = GetTickCount64();
            } else if (GetTickCount64() - stallStart > 30000) {
                Logf("republicmp  STEAM_P2P: SendExact backpressure timeout on channel %u (queued=%d, sent=%zu/%zu)",
                     (unsigned)port, (int)queuedBytes, sent, size);
                return false;
            }
            Sleep(2);
        }
        sent += chunk;
    }
    return true;
}

bool SteamP2P_FlushSend(SOCKET s, DWORD timeoutMs)
{
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return false;

    ULONGLONG stallStart = GetTickCount64();
    int32_t minQueuedSeen = INT32_MAX;
    EnsureLock();

    for (;;) {
        EnterCriticalSection(&s_lock);
        SteamSocketSlot& slot = s_slots[idx];
        if (!slot.inUse || slot.localClosed || slot.remoteFin) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        PumpChannelLocked((int)slot.port);

        int32_t queuedBytes = 0;
        uint8_t sessionErr = 0;
        if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
            P2PSessionState_t state = {};
            if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot.remoteSteamId, &state)) {
                queuedBytes = state.m_nBytesQueuedForSend;
                sessionErr = state.m_eP2PSessionError;
            }
        }
        unsigned short port = slot.port;
        LeaveCriticalSection(&s_lock);

        if (sessionErr != 0) {
            Logf("republicmp  STEAM_P2P: FlushSend aborted, P2P session error=%u on channel %u",
                 (unsigned)sessionErr, (unsigned)port);
            return false;
        }
        SaveSync_OnP2PFlushProgress((int)queuedBytes, queuedBytes <= 0);
        if (queuedBytes <= 0) {
            return true;
        }
        if (queuedBytes < minQueuedSeen) {
            minQueuedSeen = queuedBytes;
            stallStart = GetTickCount64();
        } else if (timeoutMs != 0 && (GetTickCount64() - stallStart) >= timeoutMs) {
            Logf("republicmp  STEAM_P2P: FlushSend timeout on channel %u (queued=%d)",
                 (unsigned)port, (int)queuedBytes);
            return false;
        }
        Sleep(2);
    }
}

bool SteamP2P_RecvExact(SOCKET s, void* data, size_t size)
{
    int idx = SlotIndexFromSocket(s);
    if (idx < 0) return false;

    uint8_t* out = (uint8_t*)data;
    size_t received = 0;
    ULONGLONG waitStart = GetTickCount64();
    EnsureLock();

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
            VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
            P2PSessionState_t state = {};
            if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot.remoteSteamId, &state)) {
                if (state.m_eP2PSessionError != 0) {
                    eof = true;
                } else if (!state.m_bConnectionActive && !state.m_bConnecting && (GetTickCount64() - waitStart) > 10000) {
                    eof = true;
                }
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

    EnsureLock();
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
    if (!gone && VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
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

    EnsureLock();
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

    EnsureLock();
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

    EnsureLock();
    EnterCriticalSection(&s_lock);
    uint64_t remoteId = s_slots[idx].inUse ? s_slots[idx].remoteSteamId : 0;
    if (!remoteId) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    const char* persona = nullptr;
    if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
        persona = pISteamFriends_GetFriendPersonaName(s_pSteamFriends, remoteId);
    }
    if (persona && persona[0]) {
        _snprintf_s(out, outSize, _TRUNCATE, "Steam:%s (%llu)", persona, (unsigned long long)remoteId);
    } else {
        _snprintf_s(out, outSize, _TRUNCATE, "steam:%llu", (unsigned long long)remoteId);
    }
    LeaveCriticalSection(&s_lock);
}
