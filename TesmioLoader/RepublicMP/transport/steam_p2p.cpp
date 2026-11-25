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

static INIT_ONCE s_lockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_dataCv = CONDITION_VARIABLE_INIT;
static char s_pendingJoinTarget[64] = "";

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

    WakeAllConditionVariable(&s_dataCv);
}

static bool VerifyActiveSteamModuleLocked();
static bool IsSteamP2PReadyLocked();

static void EnsureLock()
{
    InitOnceExecuteOnce(&s_lockOnce, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        InitializeCriticalSection(&s_lock);
        InitializeConditionVariable(&s_dataCv);
        return TRUE;
    }, nullptr, nullptr);
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
    WakeAllConditionVariable(&s_dataCv);
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
static volatile bool s_hostingPresenceDirty = false;

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

void SteamP2P_ResetHostProbes()
{
    EnsureLock();
    EnterCriticalSection(&s_lock);
    s_hostProbes.clear();
    s_hostProbeDirty = true;
    LeaveCriticalSection(&s_lock);
}

bool SteamP2P_ResolveFriendHostingState(bool rpHosting, bool hasPong, bool pongIsOpen,
                                        ULONGLONG pongAgeMs, ULONGLONG pingSentTick,
                                        ULONGLONG pongRecvTick)
{
    if (hasPong && pongIsOpen && pongAgeMs <= 8000ULL) {
        return true;
    }
    if (rpHosting) {
        if (hasPong && !pongIsOpen && pongAgeMs <= 4000ULL &&
            pingSentTick > 0ULL && pongRecvTick >= pingSentTick) {
            return false;
        }
        return true;
    }
    return false;
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

    int inSovietCount = 0;
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
        ++inSovietCount;

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
            const ULONGLONG pongAgeMs = (fp->hasPong && now >= fp->lastPongRecvTick)
                                        ? (now - fp->lastPongRecvTick)
                                        : ULLONG_MAX;
            isHosting = SteamP2P_ResolveFriendHostingState(
                rpHosting, fp->hasPong, fp->isOpen, pongAgeMs,
                fp->lastPingSentTick, fp->lastPongRecvTick);
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

    static int s_lastLoggedInSoviet = -1;
    static int s_lastLoggedHosting = -1;
    const int hostingCount = static_cast<int>(outFriends.size());
    if (inSovietCount != s_lastLoggedInSoviet || hostingCount != s_lastLoggedHosting) {
        s_lastLoggedInSoviet = inSovietCount;
        s_lastLoggedHosting = hostingCount;
        Logf("republicmp  STEAM_P2P: friends scan -> total=%d, inSoviet=%d, hosting=%d",
             count, inSovietCount, hostingCount);
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

static void SyncHostingPresenceLocked(bool hosting)
{
    if (!VerifyActiveSteamModuleLocked() || !IsSteamP2PReadyLocked()) return;
    if (hosting) {
        uint64_t myId = pISteamUser_GetSteamID(s_pSteamUser);
        char connectCmd[64] = {};
        _snprintf_s(connectCmd, sizeof(connectCmd), _TRUNCATE, "+connect_steam %llu", (unsigned long long)myId);
        pISteamFriends_SetRichPresence(s_pSteamFriends, "connect", connectCmd);
        pISteamFriends_SetRichPresence(s_pSteamFriends, "republicmp_hosting", "1");
        pISteamFriends_SetRichPresence(s_pSteamFriends, "status", "RepublicMP Co-op (Open)");
    } else {
        pISteamFriends_ClearRichPresence(s_pSteamFriends);
    }

    const uint8_t openByte = hosting ? 1u : 0u;
    const int k_EFriendFlagImmediate = 0x04;
    int count = pISteamFriends_GetFriendCount(s_pSteamFriends, k_EFriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        uint64_t fid = pISteamFriends_GetFriendByIndex(s_pSteamFriends, i, k_EFriendFlagImmediate);
        if (!fid) continue;
        SteamFriendGameInfo_t gameInfo = {};
        bool inGame = pISteamFriends_GetFriendGamePlayed(s_pSteamFriends, fid, &gameInfo);
        if (inGame && (uint32_t)(gameInfo.m_gameID & 0xFFFFFFULL) == 784150) {
            pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, fid);
            SendUnreliableFrameLocked(fid, 5555, 6 /* P2P_FRAME_HOST_PONG */, 0, &openByte, 1);
            if (!IsSteamP2PReadyLocked()) break;
        }
    }
}

void SteamP2P_SetHostingPresence(bool hosting)
{
    s_isHostingOpen = hosting;
    s_hostingPresenceDirty = true;
    if (!SteamP2P_IsAvailable()) return;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
        SyncHostingPresenceLocked(hosting);
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

static constexpr size_t kMaxPendingSyns = 32;
static constexpr size_t kMaxSlotRecvBufBytes = 16u * 1024u * 1024u;
static constexpr size_t kRingInitialCap = 4096u;
static constexpr int kMaxPumpFramesPerCall = 64;
static constexpr uint32_t kMaxDiscardScratchBytes = 65536u;

struct ByteRingBuffer {
    std::vector<uint8_t> buf;
    size_t cap = 0;
    size_t head = 0;
    size_t count = 0;

    bool Empty() const { return count == 0; }
    size_t Size() const { return count; }

    void Clear()
    {
        head = 0;
        count = 0;
    }

    void Release()
    {
        std::vector<uint8_t>().swap(buf);
        cap = 0;
        head = 0;
        count = 0;
    }

    bool EnsureCapacity(size_t needed)
    {
        if (needed > kMaxSlotRecvBufBytes) return false;
        if (needed <= cap && buf.size() >= cap && cap > 0) return true;
        size_t newCap = (cap >= kRingInitialCap) ? cap : kRingInitialCap;
        while (newCap < needed) {
            if (newCap >= kMaxSlotRecvBufBytes) {
                newCap = kMaxSlotRecvBufBytes;
                break;
            }
            newCap <<= 1;
        }
        if (newCap < needed) return false;
        std::vector<uint8_t> next(newCap);
        if (count > 0 && cap > 0 && buf.size() >= cap && head < cap && count <= cap) {
            const size_t first = std::min(count, cap - head);
            memcpy(next.data(), buf.data() + head, first);
            if (count > first) {
                memcpy(next.data() + first, buf.data(), count - first);
            }
        } else {
            count = 0;
        }
        buf.swap(next);
        cap = newCap;
        head = 0;
        return true;
    }

    bool Push(const uint8_t* src, size_t len)
    {
        if (len == 0) return true;
        if (!src) return false;
        if (len > kMaxSlotRecvBufBytes || count > kMaxSlotRecvBufBytes - len) {
            return false;
        }
        if (!EnsureCapacity(count + len)) return false;
        if (cap == 0 || buf.size() < cap) return false;
        const size_t mask = cap - 1u;
        const size_t tail = (head + count) & mask;
        const size_t first = std::min(len, cap - tail);
        memcpy(buf.data() + tail, src, first);
        if (len > first) {
            memcpy(buf.data(), src + first, len - first);
        }
        count += len;
        return true;
    }

    size_t Peek(uint8_t* dst, size_t maxLen) const
    {
        if (!dst || maxLen == 0 || count == 0 || cap == 0 || buf.size() < cap || head >= cap || count > cap) {
            return 0;
        }
        const size_t take = std::min(maxLen, count);
        const size_t first = std::min(take, cap - head);
        memcpy(dst, buf.data() + head, first);
        if (take > first) {
            memcpy(dst + first, buf.data(), take - first);
        }
        return take;
    }

    void Drain(size_t n)
    {
        if (n == 0 || count == 0) return;
        const size_t drop = std::min(n, count);
        count -= drop;
        if (count == 0 || cap == 0) {
            head = 0;
        } else {
            head = (head + drop) & (cap - 1u);
        }
    }

    size_t Pop(uint8_t* dst, size_t maxLen)
    {
        const size_t take = Peek(dst, maxLen);
        Drain(take);
        return take;
    }
};

struct SteamSocketSlot {
    bool inUse;
    bool connected;
    bool remoteFin;
    bool localClosed;
    uint32_t gen;
    uint64_t remoteSteamId;
    unsigned short port;
    uint32_t connId;
    DWORD ioTimeoutMs;
    ULONGLONG lastActivityTick;
    ByteRingBuffer recvBuf;
};

static SteamSocketSlot s_slots[MAX_STEAM_SOCKETS] = {};
static std::deque<PendingSyn> s_pendingSyns;
static std::vector<uint8_t> s_sendScratch;
static std::vector<uint8_t> s_recvScratch;
static const int s_activeChannels[] = { 5555, 5556, 5560 };

bool SteamP2P_IsSteamSocket(SOCKET s)
{
    if (s == INVALID_SOCKET) return false;
    return (((uint64_t)s) & STEAM_SOCKET_MASK) == STEAM_SOCKET_TAG;
}

static bool DecodeSteamSocket(SOCKET s, int* outIdx, uint32_t* outGen)
{
    if (!SteamP2P_IsSteamSocket(s)) return false;
    const uint64_t raw = static_cast<uint64_t>(s);
    const int idx = static_cast<int>(raw & 0xFFFFULL);
    const uint32_t gen = static_cast<uint32_t>((raw >> 16) & 0xFFFFFFULL);
    if (idx < 0 || idx >= MAX_STEAM_SOCKETS || gen == 0u) return false;
    if (outIdx) *outIdx = idx;
    if (outGen) *outGen = gen;
    return true;
}

static SOCKET MakeSteamSocket(int idx, uint32_t gen)
{
    return static_cast<SOCKET>(
        STEAM_SOCKET_TAG |
        (static_cast<uint64_t>(gen & 0xFFFFFFu) << 16) |
        static_cast<uint64_t>(idx & 0xFFFF));
}

static SteamSocketSlot* ResolveSlotLocked(SOCKET s)
{
    int idx = -1;
    uint32_t expectedGen = 0u;
    if (!DecodeSteamSocket(s, &idx, &expectedGen)) return nullptr;
    SteamSocketSlot& slot = s_slots[idx];
    if (!slot.inUse || slot.gen != expectedGen) return nullptr;
    return &slot;
}

static int AllocateSteamSlotLocked(uint64_t remoteSteamId, unsigned short port,
                                   uint32_t connId, DWORD ioTimeoutMs,
                                   bool connected, bool releaseRecvBuf)
{
    for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
        if (!s_slots[i].inUse) {
            SteamSocketSlot& slot = s_slots[i];
            uint32_t nextGen = (slot.gen + 1u) & 0xFFFFFFu;
            if (nextGen == 0u) nextGen = 1u;
            slot.gen = nextGen;
            slot.inUse = true;
            slot.connected = connected;
            slot.remoteFin = false;
            slot.localClosed = false;
            slot.remoteSteamId = remoteSteamId;
            slot.port = port;
            slot.connId = connId;
            slot.ioTimeoutMs = ioTimeoutMs;
            slot.lastActivityTick = GetTickCount64();
            if (releaseRecvBuf) {
                slot.recvBuf.Release();
            } else {
                slot.recvBuf.Clear();
            }
            return i;
        }
    }
    return -1;
}

static bool BuildFrameIntoSendScratchLocked(unsigned short port, uint8_t type,
                                            uint32_t connId, const void* payload,
                                            uint32_t payloadLen, size_t* outTotalLen)
{
    const size_t totalLen = sizeof(P2PFrameHeader) + static_cast<size_t>(payloadLen);
    if (totalLen > static_cast<size_t>(UINT32_MAX)) return false;
    if (payloadLen > 0 && !payload) return false;
    if (s_sendScratch.size() < totalLen) {
        s_sendScratch.resize(totalLen);
    }
    P2PFrameHeader* hdr = reinterpret_cast<P2PFrameHeader*>(s_sendScratch.data());
    hdr->magic = P2P_FRAME_MAGIC;
    hdr->type = type;
    hdr->port = port;
    hdr->connId = connId;
    hdr->payloadLen = payloadLen;
    if (payloadLen > 0) {
        memcpy(s_sendScratch.data() + sizeof(P2PFrameHeader), payload, payloadLen);
    }
    if (outTotalLen) *outTotalLen = totalLen;
    return true;
}

static bool SendFrameWithKindLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                                    uint32_t connId, const void* payload, uint32_t payloadLen, int sendType)
{
    if (s_shuttingDown || !s_available || !s_pSteamNetworking) return false;
    if (!VerifyActiveSteamModuleLocked() || !pISteamNetworking_SendP2PPacket) {
        return false;
    }
    size_t totalLen = 0;
    if (!BuildFrameIntoSendScratchLocked(port, type, connId, payload, payloadLen, &totalLen)) {
        return false;
    }
    return pISteamNetworking_SendP2PPacket(
        s_pSteamNetworking,
        remoteSteamId,
        s_sendScratch.data(),
        static_cast<uint32_t>(totalLen),
        sendType,
        static_cast<int>(port));
}

static bool SendFrameLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                            uint32_t connId, const void* payload, uint32_t payloadLen)
{
    return SendFrameWithKindLocked(remoteSteamId, port, type, connId, payload, payloadLen, k_EP2PSendReliable);
}

static bool SendUnreliableFrameLocked(uint64_t remoteSteamId, unsigned short port, uint8_t type,
                                      uint32_t connId, const void* payload, uint32_t payloadLen)
{
    return SendFrameWithKindLocked(remoteSteamId, port, type, connId, payload, payloadLen, k_EP2PSendUnreliable);
}

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
        bool shouldWake = false;
        if (hdr->type == P2P_FRAME_SYN_ACK) {
            slot.connected = true;
            shouldWake = true;
        } else if (hdr->type == P2P_FRAME_DATA) {
            if (hdr->payloadLen > 0) {
                if (!slot.recvBuf.Push(payload, hdr->payloadLen)) {
                    Logf("republicmp  STEAM_P2P: recvBuf overflow (>%zu bytes) on channel %u, closing slot",
                         kMaxSlotRecvBufBytes, (unsigned)slot.port);
                    slot.remoteFin = true;
                }
                shouldWake = true;
            }
        } else if (hdr->type == P2P_FRAME_FIN) {
            slot.remoteFin = true;
            shouldWake = true;
        }
        if (shouldWake) {
            WakeAllConditionVariable(&s_dataCv);
        }
        break;
    }
}

static uint8_t* AcquireRecvScratchLocked(uint32_t needed)
{
    if (needed == 0) return nullptr;
    if (s_recvScratch.size() < needed) {
        s_recvScratch.resize(needed);
    }
    return s_recvScratch.data();
}

static bool DispatchDecodedFrameLocked(uint64_t sender, const uint8_t* buf, uint32_t readBytes, ULONGLONG now)
{
    if (!buf || readBytes < sizeof(P2PFrameHeader)) return true;
    const P2PFrameHeader* hdr = reinterpret_cast<const P2PFrameHeader*>(buf);
    if (hdr->magic != P2P_FRAME_MAGIC) return true;
    if (static_cast<size_t>(sizeof(P2PFrameHeader)) + static_cast<size_t>(hdr->payloadLen) > static_cast<size_t>(readBytes)) {
        return true;
    }

    const uint8_t* payload = buf + sizeof(P2PFrameHeader);

    if (hdr->type == P2P_FRAME_HOST_PING) {
        uint8_t openByte = s_isHostingOpen ? 1 : 0;
        SendUnreliableFrameLocked(sender, hdr->port, P2P_FRAME_HOST_PONG, 0, &openByte, 1);
        return s_available;
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
        return true;
    }

    if (hdr->type == P2P_FRAME_SYN) {
        HandleSynFrameLocked(sender, hdr, now);
        return s_available;
    }

    HandleSlotFrameLocked(sender, hdr, payload, now);
    return true;
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
            uint8_t* discard = AcquireRecvScratchLocked(discardCap);
            uint32_t readBytes = 0;
            uint64_t sender = 0;
            if (discard) {
                pISteamNetworking_ReadP2PPacket(s_pSteamNetworking, discard, discardCap, &readBytes, &sender, channel);
            }
            break;
        }
        uint8_t* buf = AcquireRecvScratchLocked(msgSize);
        if (!buf) break;
        uint32_t readBytes = 0;
        uint64_t sender = 0;
        if (!pISteamNetworking_ReadP2PPacket(s_pSteamNetworking, buf, msgSize, &readBytes, &sender, channel)) {
            break;
        }
        if (readBytes > msgSize) continue;
        if (!DispatchDecodedFrameLocked(sender, buf, readBytes, GetTickCount64())) {
            break;
        }
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

static void TickHostingPresenceSyncLocked(ULONGLONG now)
{
    static ULONGLONG s_lastPresenceSyncTick = 0;
    if (s_hostingPresenceDirty ||
        (s_isHostingOpen && (s_lastPresenceSyncTick == 0 || (now - s_lastPresenceSyncTick) >= 3000ULL))) {
        s_hostingPresenceDirty = false;
        s_lastPresenceSyncTick = now;
        SyncHostingPresenceLocked(s_isHostingOpen);
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
    const ULONGLONG now = GetTickCount64();
    bool activeSocket = HasActiveSteamSocketLocked();
    if (!activeSocket) {
        static ULONGLONG s_lastIdlePumpTick = 0;
        if (s_lastIdlePumpTick != 0 && (now - s_lastIdlePumpTick) < 50) {
            PruneStalePendingSynsLocked(now, 0);
            LeaveCriticalSection(&s_lock);
            return;
        }
        s_lastIdlePumpTick = now;
        pSteamAPI_RunCallbacks();
        TickHostingPresenceSyncLocked(now);
        PumpChannelLocked(5555);
        if (s_isHostingOpen) {
            PumpChannelLocked(5556);
        }
        PruneStalePendingSynsLocked(now, 0);
        LeaveCriticalSection(&s_lock);
        return;
    }
    pSteamAPI_RunCallbacks();
    TickHostingPresenceSyncLocked(now);
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

        const int slotIdx = AllocateSteamSlotLocked(
            syn.remoteSteamId, port, syn.connId, ioTimeoutMs, true, false);
        if (slotIdx < 0) break;

        pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, syn.remoteSteamId);
        SendFrameLocked(syn.remoteSteamId, port, P2P_FRAME_SYN_ACK, syn.connId, nullptr, 0);

        result = MakeSteamSocket(slotIdx, s_slots[slotIdx].gen);
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
    const int slotIdx = AllocateSteamSlotLocked(remoteSteamId, port, 0u, ioTimeoutMs, false, false);
    if (slotIdx < 0) {
        LeaveCriticalSection(&s_lock);
        Logf("republicmp  STEAM_P2P: no free virtual socket slots");
        return INVALID_SOCKET;
    }

    uint32_t connId = (uint32_t)(GetTickCount64() ^ ((uint64_t)port << 16) ^ (uint64_t)(slotIdx + 1) * 0x9E3779B9U);
    if (connId == 0) connId = 1;

    SteamSocketSlot& slot = s_slots[slotIdx];
    slot.connId = connId;
    const SOCKET sock = MakeSteamSocket(slotIdx, slot.gen);

    pISteamNetworking_AcceptP2PSessionWithUser(s_pSteamNetworking, remoteSteamId);
    SendFrameLocked(remoteSteamId, port, P2P_FRAME_SYN, connId, nullptr, 0);
    LeaveCriticalSection(&s_lock);

    Logf("republicmp  STEAM_P2P: connecting to SteamID %llu on channel %u (connId=%08X, timeout=%u ms)...",
         (unsigned long long)remoteSteamId, (unsigned)port, connId, connectTimeoutMs);

    ULONGLONG startTick = GetTickCount64();
    ULONGLONG lastSynSent = startTick;

    for (;;) {
        if (cancelFlag && *cancelFlag) {
            SteamP2P_CloseSocket(sock);
            return INVALID_SOCKET;
        }

        EnterCriticalSection(&s_lock);
        PumpChannelLocked((int)port);
        SteamSocketSlot* activeSlot = ResolveSlotLocked(sock);
        bool connected = activeSlot ? activeSlot->connected : false;
        bool closed = !activeSlot || activeSlot->localClosed || activeSlot->remoteFin;
        ULONGLONG now = GetTickCount64();
        if (!connected && !closed && now - lastSynSent >= 400) {
            SendFrameLocked(remoteSteamId, port, P2P_FRAME_SYN, connId, nullptr, 0);
            lastSynSent = now;
        }
        LeaveCriticalSection(&s_lock);

        if (connected) {
            Logf("republicmp  STEAM_P2P: connected to SteamID %llu on channel %u",
                 (unsigned long long)remoteSteamId, (unsigned)port);
            return sock;
        }
        if (closed) {
            SteamP2P_CloseSocket(sock);
            return INVALID_SOCKET;
        }
        if (connectTimeoutMs != 0 && (GetTickCount64() - startTick) >= connectTimeoutMs) {
            Logf("republicmp  STEAM_P2P: TIMEOUT connecting to SteamID %llu on channel %u",
                 (unsigned long long)remoteSteamId, (unsigned)port);
            SteamP2P_CloseSocket(sock);
            return INVALID_SOCKET;
        }
        Sleep(20);
    }
}

bool SteamP2P_SendExact(SOCKET s, const void* data, size_t size)
{
    if (!data && size > 0) return false;
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return false;

    const uint8_t* bytes = (const uint8_t*)data;
    size_t sent = 0;
    EnsureLock();
    while (sent < size) {
        uint32_t chunk = (uint32_t)std::min(size - sent, (size_t)P2P_MAX_CHUNK);
        ULONGLONG stallStart = GetTickCount64();
        int32_t minQueuedSeen = INT32_MAX;

        for (;;) {
            EnterCriticalSection(&s_lock);
            SteamSocketSlot* slot = ResolveSlotLocked(s);
            if (!slot || slot->localClosed || slot->remoteFin) {
                LeaveCriticalSection(&s_lock);
                return false;
            }
            unsigned short port = slot->port;
            PumpChannelLocked((int)port);
            slot = ResolveSlotLocked(s);
            if (!slot || slot->localClosed || slot->remoteFin) {
                LeaveCriticalSection(&s_lock);
                return false;
            }

            int32_t queuedBytes = 0;
            uint8_t sessionErr = 0;
            if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
                P2PSessionState_t state = {};
                if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot->remoteSteamId, &state)) {
                    queuedBytes = state.m_nBytesQueuedForSend;
                    sessionErr = state.m_eP2PSessionError;
                }
            }
            if (sessionErr != 0) {
                LeaveCriticalSection(&s_lock);
                Logf("republicmp  STEAM_P2P: SendExact aborted, P2P session error=%u on channel %u",
                     (unsigned)sessionErr, (unsigned)port);
                return false;
            }
            if (queuedBytes <= P2P_SEND_QUEUE_CAP) {
                bool ok = SendFrameLocked(slot->remoteSteamId, port, P2P_FRAME_DATA, slot->connId, bytes + sent, chunk);
                LeaveCriticalSection(&s_lock);
                if (!ok) {
                    Logf("republicmp  STEAM_P2P: SendP2PPacket failed on channel %u (queued=%d, sent=%zu/%zu)",
                         (unsigned)port, (int)queuedBytes, sent, size);
                    return false;
                }
                break;
            }
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
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return false;

    ULONGLONG stallStart = GetTickCount64();
    int32_t minQueuedSeen = INT32_MAX;
    EnsureLock();

    for (;;) {
        EnterCriticalSection(&s_lock);
        SteamSocketSlot* slot = ResolveSlotLocked(s);
        if (!slot || slot->localClosed || slot->remoteFin) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        unsigned short port = slot->port;
        PumpChannelLocked((int)port);
        slot = ResolveSlotLocked(s);
        if (!slot || slot->localClosed || slot->remoteFin) {
            LeaveCriticalSection(&s_lock);
            return false;
        }

        int32_t queuedBytes = 0;
        uint8_t sessionErr = 0;
        if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
            P2PSessionState_t state = {};
            if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot->remoteSteamId, &state)) {
                queuedBytes = state.m_nBytesQueuedForSend;
                sessionErr = state.m_eP2PSessionError;
            }
        }
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

static bool IsSlotEofLocked(const SteamSocketSlot& slot, ULONGLONG waitStart, bool checkIdleDisconnect)
{
    if (slot.remoteFin) return true;
    const ULONGLONG elapsed = GetTickCount64() - waitStart;
    if (elapsed > 2500ULL && VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
        P2PSessionState_t state = {};
        if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot.remoteSteamId, &state)) {
            if (state.m_eP2PSessionError != 0) {
                return true;
            }
            if (checkIdleDisconnect && !state.m_bConnectionActive && !state.m_bConnecting && elapsed > 10000ULL) {
                return true;
            }
        }
    }
    return false;
}

bool SteamP2P_RecvExact(SOCKET s, void* data, size_t size)
{
    if (!data && size > 0) return false;
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return false;

    uint8_t* out = (uint8_t*)data;
    size_t received = 0;
    ULONGLONG waitStart = GetTickCount64();
    EnsureLock();

    while (received < size) {
        EnterCriticalSection(&s_lock);
        SteamSocketSlot* slot = ResolveSlotLocked(s);
        if (!slot || slot->localClosed) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        PumpChannelLocked((int)slot->port);
        slot = ResolveSlotLocked(s);
        if (!slot || slot->localClosed) {
            LeaveCriticalSection(&s_lock);
            return false;
        }

        size_t avail = slot->recvBuf.Size();
        if (avail > 0) {
            size_t take = slot->recvBuf.Pop(out + received, size - received);
            received += take;
            waitStart = GetTickCount64();
            LeaveCriticalSection(&s_lock);
            continue;
        }

        bool eof = IsSlotEofLocked(*slot, waitStart, true);
        DWORD timeoutMs = slot->ioTimeoutMs;
        if (eof) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        if (timeoutMs != 0 && (GetTickCount64() - waitStart) >= timeoutMs) {
            LeaveCriticalSection(&s_lock);
            Logf("republicmp  STEAM_P2P: RecvExact timeout after %u ms", timeoutMs);
            return false;
        }
        SleepConditionVariableCS(&s_dataCv, &s_lock, 1);
        LeaveCriticalSection(&s_lock);
    }
    return true;
}

int SteamP2P_PollReadable(SOCKET s, DWORD timeoutMs, size_t minBytes)
{
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return -1;

    const size_t wantBytes = (minBytes > 0) ? minBytes : 1u;
    const ULONGLONG start = GetTickCount64();
    EnsureLock();

    for (;;) {
        EnterCriticalSection(&s_lock);
        SteamSocketSlot* slot = ResolveSlotLocked(s);
        if (!slot || slot->localClosed) {
            LeaveCriticalSection(&s_lock);
            return -1;
        }
        PumpChannelLocked(static_cast<int>(slot->port));
        slot = ResolveSlotLocked(s);
        if (!slot || slot->localClosed) {
            LeaveCriticalSection(&s_lock);
            return -1;
        }
        if (slot->recvBuf.Size() >= wantBytes) {
            LeaveCriticalSection(&s_lock);
            return 1;
        }
        if (IsSlotEofLocked(*slot, start, false)) {
            LeaveCriticalSection(&s_lock);
            return -1;
        }
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (timeoutMs == 0 || elapsed >= timeoutMs) {
            LeaveCriticalSection(&s_lock);
            return 0;
        }
        const DWORD rem = static_cast<DWORD>(timeoutMs - elapsed);
        const DWORD waitMs = (rem > 1u) ? 1u : rem;
        SleepConditionVariableCS(&s_dataCv, &s_lock, waitMs);
        LeaveCriticalSection(&s_lock);
    }
}

bool SteamP2P_PeerGone(SOCKET s)
{
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return true;

    EnsureLock();
    EnterCriticalSection(&s_lock);
    SteamSocketSlot* slot = ResolveSlotLocked(s);
    if (!slot || slot->localClosed) {
        LeaveCriticalSection(&s_lock);
        return true;
    }
    PumpChannelLocked((int)slot->port);
    slot = ResolveSlotLocked(s);
    if (!slot || slot->localClosed) {
        LeaveCriticalSection(&s_lock);
        return true;
    }
    if (!slot->recvBuf.Empty()) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    bool gone = slot->remoteFin;
    if (!gone && VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
        P2PSessionState_t state = {};
        if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, slot->remoteSteamId, &state)) {
            if (state.m_eP2PSessionError != 0) gone = true;
        }
    }
    LeaveCriticalSection(&s_lock);
    return gone;
}

void SteamP2P_ShutdownSocket(SOCKET s)
{
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return;

    EnsureLock();
    EnterCriticalSection(&s_lock);
    SteamSocketSlot* slot = ResolveSlotLocked(s);
    if (slot && !slot->localClosed) {
        SendFrameLocked(slot->remoteSteamId, slot->port, P2P_FRAME_FIN, slot->connId, nullptr, 0);
        slot->localClosed = true;
        WakeAllConditionVariable(&s_dataCv);
    }
    LeaveCriticalSection(&s_lock);
}

void SteamP2P_CloseSocket(SOCKET s)
{
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return;

    EnsureLock();
    EnterCriticalSection(&s_lock);
    SteamSocketSlot* slot = ResolveSlotLocked(s);
    if (slot) {
        if (!slot->localClosed) {
            SendFrameLocked(slot->remoteSteamId, slot->port, P2P_FRAME_FIN, slot->connId, nullptr, 0);
        }
        slot->inUse = false;
        slot->connected = false;
        slot->localClosed = true;
        slot->remoteFin = true;
        slot->recvBuf.Release();
        WakeAllConditionVariable(&s_dataCv);
    }
    LeaveCriticalSection(&s_lock);
}

void SteamP2P_GetPeerName(SOCKET s, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return;

    EnsureLock();
    EnterCriticalSection(&s_lock);
    SteamSocketSlot* slot = ResolveSlotLocked(s);
    uint64_t remoteId = slot ? slot->remoteSteamId : 0;
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

void SteamP2P_GetPerfStats(bool* outConnected, bool* outUsingRelay, int32_t* outQueuedBytes)
{
    if (outConnected) *outConnected = false;
    if (outUsingRelay) *outUsingRelay = false;
    if (outQueuedBytes) *outQueuedBytes = 0;

    EnsureLock();
    EnterCriticalSection(&s_lock);
    if (VerifyActiveSteamModuleLocked() && IsSteamP2PReadyLocked()) {
        uint64_t chosenRemoteId = 0;
        for (int i = 0; i < MAX_STEAM_SOCKETS; ++i) {
            const SteamSocketSlot& slot = s_slots[i];
            if (!slot.inUse || slot.localClosed || slot.remoteFin || slot.remoteSteamId == 0) continue;
            chosenRemoteId = slot.remoteSteamId;
            if (slot.port == 5560) break;
        }
        if (chosenRemoteId != 0) {
            P2PSessionState_t state = {};
            if (pISteamNetworking_GetP2PSessionState(s_pSteamNetworking, chosenRemoteId, &state)) {
                if (outConnected) *outConnected = (state.m_bConnectionActive != 0);
                if (outUsingRelay) *outUsingRelay = (state.m_bUsingRelay != 0);
                if (outQueuedBytes) *outQueuedBytes = state.m_nBytesQueuedForSend;
            }
        }
    }
    LeaveCriticalSection(&s_lock);
}

SOCKET SteamP2P_CreateTestSocketForTest(uint64_t remoteSteamId, unsigned short port,
                                        uint32_t connId, DWORD ioTimeoutMs, bool connected)
{
    EnsureLock();
    EnterCriticalSection(&s_lock);
    const int slotIdx = AllocateSteamSlotLocked(
        remoteSteamId, port, connId, ioTimeoutMs, connected, true);
    if (slotIdx < 0) {
        LeaveCriticalSection(&s_lock);
        return INVALID_SOCKET;
    }
    SOCKET sock = MakeSteamSocket(slotIdx, s_slots[slotIdx].gen);
    LeaveCriticalSection(&s_lock);
    return sock;
}

void SteamP2P_InjectTestFrameForTest(uint64_t senderSteamId, uint8_t frameType,
                                     unsigned short port, uint32_t connId,
                                     const void* payload, uint32_t payloadLen)
{
    EnsureLock();
    EnterCriticalSection(&s_lock);
    P2PFrameHeader hdr = {};
    hdr.magic = P2P_FRAME_MAGIC;
    hdr.type = frameType;
    hdr.port = port;
    hdr.connId = connId;
    hdr.payloadLen = payloadLen;
    HandleSlotFrameLocked(senderSteamId, &hdr, static_cast<const uint8_t*>(payload), GetTickCount64());
    LeaveCriticalSection(&s_lock);
}

bool SteamP2P_GetSlotBufferStatsForTest(SOCKET s, size_t* outSize, size_t* outCap, size_t* outHead)
{
    if (outSize) *outSize = 0;
    if (outCap) *outCap = 0;
    if (outHead) *outHead = 0;
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return false;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    const SteamSocketSlot* slot = ResolveSlotLocked(s);
    if (!slot) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    if (outSize) *outSize = slot->recvBuf.Size();
    if (outCap) *outCap = slot->recvBuf.cap;
    if (outHead) *outHead = slot->recvBuf.head;
    LeaveCriticalSection(&s_lock);
    return true;
}

bool SteamP2P_TestScratchAndDispatchForTest(SOCKET s, const void* payload, uint32_t payloadLen,
                                            uint32_t oversizedMsgSize,
                                            const void** outSendPtr, size_t* outSendCap,
                                            const void** outRecvPtr, size_t* outRecvCap)
{
    if (!DecodeSteamSocket(s, nullptr, nullptr)) return false;
    EnsureLock();
    EnterCriticalSection(&s_lock);
    const SteamSocketSlot* slot = ResolveSlotLocked(s);
    if (!slot || slot->localClosed) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    const uint64_t sender = slot->remoteSteamId;
    const unsigned short port = slot->port;
    const uint32_t connId = slot->connId;

    if (oversizedMsgSize > 0) {
        const uint32_t discardCap = std::min<uint32_t>(oversizedMsgSize, kMaxDiscardScratchBytes);
        uint8_t* discard = AcquireRecvScratchLocked(discardCap);
        if (!discard) {
            LeaveCriticalSection(&s_lock);
            return false;
        }
        discard[0] = 0xAAu;
    }

    size_t totalLen = 0;
    if (!BuildFrameIntoSendScratchLocked(port, P2P_FRAME_DATA, connId, payload, payloadLen, &totalLen) ||
        totalLen > static_cast<size_t>(UINT32_MAX)) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    uint8_t* recvBufPtr = AcquireRecvScratchLocked(static_cast<uint32_t>(totalLen));
    if (!recvBufPtr) {
        LeaveCriticalSection(&s_lock);
        return false;
    }
    memcpy(recvBufPtr, s_sendScratch.data(), totalLen);
    DispatchDecodedFrameLocked(sender, recvBufPtr, static_cast<uint32_t>(totalLen), GetTickCount64());

    if (outSendPtr) *outSendPtr = s_sendScratch.data();
    if (outSendCap) *outSendCap = s_sendScratch.capacity();
    if (outRecvPtr) *outRecvPtr = s_recvScratch.data();
    if (outRecvCap) *outRecvCap = s_recvScratch.capacity();
    LeaveCriticalSection(&s_lock);
    return true;
}
