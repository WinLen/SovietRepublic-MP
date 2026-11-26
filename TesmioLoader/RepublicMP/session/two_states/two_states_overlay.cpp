// SPDX-License-Identifier: GPL-3.0-only
// Minimap, 3D border, camera, cursor, ping, and toast overlay implementation.
#include "two_states_internal.h"

void TwoStates_BindOverlay(const TsmHost* host) { if (host) TsmBind(host); }

// ---------------------------------------------------------------------------
// Cross-Module Shared State (owned by two_states_overlay.cpp)
// ---------------------------------------------------------------------------

bool g_show3DBorder = true;
bool g_showMinimapBorders = true;

bool  g_hasSavedCam1 = false;
float g_savedCam1Pos[3] = { 0.0f, 0.0f, 0.0f };
float g_savedCam1Look[3] = { 0.0f, 0.0f, 0.0f };

bool  g_hasSavedCam2 = false;
float g_savedCam2Pos[3] = { 0.0f, 0.0f, 0.0f };
float g_savedCam2Look[3] = { 0.0f, 0.0f, 0.0f };

PendingCameraRestore g_pendingCamRestore = {};

bool  g_hasResyncCam = false;
float g_resyncCamPos[3] = { 0.0f, 0.0f, 0.0f };
float g_resyncCamLook[3] = { 0.0f, 0.0f, 0.0f };

bool g_localPingActive = false;
CollisionTerrainHeightFn g_collisionTerrainHeight = nullptr;

// ---------------------------------------------------------------------------
// Overlay Internal State & Toast Lock
// ---------------------------------------------------------------------------

static constexpr ULONGLONG PING_LIFETIME_MS = 20000;
static float     g_localPingX      = 0.0f;
static float     g_localPingY      = 0.0f;
static float     g_localPingZ      = 0.0f;
static ULONGLONG g_localPingSentMs = 0;

struct ToastMessage {
    char text[256];
    int colorKind; // 0=info(blue), 1=success(green), 2=warning(yellow), 3=error(red), 4=gold, 5=workshop(cyan)
    ULONGLONG createdMs;
    ULONGLONG durationMs;
};

static INIT_ONCE g_toastLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_toastLock;
static std::deque<ToastMessage> g_toasts;

static void EnsureToastLock()
{
    TsmEnsureCriticalSection(&g_toastLockOnce, &g_toastLock);
}

// ---------------------------------------------------------------------------
// Minimap Hook Constants & Trampolines
// ---------------------------------------------------------------------------

static const BYTE kMinimapDrawContentExpect[16] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0xA8, 0x28, 0xFF, 0xFF, 0xFF };
static const BYTE kMinimapDrawButtonsExpect[15] = { 0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00 };

typedef void (__fastcall* MinimapDrawFn)(BYTE* mm);
static MinimapDrawFn o_minimapDrawContent = nullptr;
static MinimapDrawFn o_minimapDrawButtons = nullptr;

// ---------------------------------------------------------------------------
// Toast Notifications
// ---------------------------------------------------------------------------

void TwoStates_AddToastAtTickForTest(const char* text, int colorKind, unsigned long long createdMs)
{
    if (!text || !text[0]) return;
    const int kind = UiCommon_NormalizeToastColorKind(colorKind);
    EnsureToastLock();
    EnterCriticalSection(&g_toastLock);
    ToastMessage tm = {};
    strncpy_s(tm.text, sizeof(tm.text), text, _TRUNCATE);
    tm.colorKind = kind;
    tm.createdMs = createdMs;
    tm.durationMs = (kind >= 2) ? 5500 : 4500;
    g_toasts.push_back(tm);
    while (g_toasts.size() > 5) g_toasts.pop_front();
    LeaveCriticalSection(&g_toastLock);
}

void TwoStates_AddToast(const char* text, int colorKind)
{
    TwoStates_AddToastAtTickForTest(text, colorKind, GetTickCount64());
}

void TwoStates_ClearToastsForTest()
{
    EnsureToastLock();
    EnterCriticalSection(&g_toastLock);
    g_toasts.clear();
    LeaveCriticalSection(&g_toastLock);
}

int TwoStates_GetToastCountForTest()
{
    EnsureToastLock();
    EnterCriticalSection(&g_toastLock);
    int count = static_cast<int>(g_toasts.size());
    LeaveCriticalSection(&g_toastLock);
    return count;
}

bool TwoStates_GetToastForTest(int index, char* outText, size_t outCap,
                               int* outColorKind, unsigned long long* outDurationMs)
{
    EnsureToastLock();
    EnterCriticalSection(&g_toastLock);
    if (index < 0 || static_cast<size_t>(index) >= g_toasts.size()) {
        LeaveCriticalSection(&g_toastLock);
        return false;
    }
    const ToastMessage& tm = g_toasts[static_cast<size_t>(index)];
    if (outText && outCap > 0) strncpy_s(outText, outCap, tm.text, _TRUNCATE);
    if (outColorKind) *outColorKind = tm.colorKind;
    if (outDurationMs) *outDurationMs = tm.durationMs;
    LeaveCriticalSection(&g_toastLock);
    return true;
}

// ---------------------------------------------------------------------------
// Camera, Cursor & Minimap Geometry
// ---------------------------------------------------------------------------

void ResetSavedCameraState()
{
    g_hasSavedCam1 = false;
    memset(g_savedCam1Pos, 0, sizeof(g_savedCam1Pos));
    memset(g_savedCam1Look, 0, sizeof(g_savedCam1Look));
    g_hasSavedCam2 = false;
    memset(g_savedCam2Pos, 0, sizeof(g_savedCam2Pos));
    memset(g_savedCam2Look, 0, sizeof(g_savedCam2Look));
    g_pendingCamRestore.active = false;
    MemoryBarrier();
    g_pendingCamRestore = {};
}

bool IsValidCameraPair(const float pos[3], const float look[3])
{
    if (!pos || !look) return false;
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2]) ||
        !isfinite(look[0]) || !isfinite(look[1]) || !isfinite(look[2])) {
        return false;
    }
    float dx = look[0] - pos[0];
    float dy = look[1] - pos[1];
    float dz = look[2] - pos[2];
    return (dx * dx + dy * dy + dz * dz) > 0.01f;
}

bool TwoStates_GetCursorWorldPos(float* outX, float* outY, float* outZ)
{
    if (!g_exeBase) return false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* world = g_exeBase + WORLD_RVA;
        if (outX) *outX = *(float*)(world + CURSOR_X_OFFSET);
        if (outY) *outY = *(float*)(world + CURSOR_Y_OFFSET);
        if (outZ) *outZ = *(float*)(world + CURSOR_Z_OFFSET);
        DiagLog_EndSafeSeh();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DiagLog_EndSafeSeh();
        return false;
    }
}

static bool GetCameraVectorByIndex(int idx, float* outX, float* outY, float* outZ)
{
    float v[2][3] = {};
    if (!TwoStates_GetCameraView(v[0], v[1])) return false;
    if (outX) *outX = v[idx][0];
    if (outY) *outY = v[idx][1];
    if (outZ) *outZ = v[idx][2];
    return true;
}

bool TwoStates_GetCameraPos(float* outX, float* outY, float* outZ)
{
    return GetCameraVectorByIndex(0, outX, outY, outZ);
}

bool TwoStates_GetCameraLookAt(float* outX, float* outY, float* outZ)
{
    return GetCameraVectorByIndex(1, outX, outY, outZ);
}

bool TwoStates_GetCameraView(float pos[3], float lookAt[3])
{
    if (!g_exeBase) return false;
    DiagLog_BeginSafeSeh();
    __try {
        const float* p = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_POS_OFFSET);
        const float* l = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_LOOKAT_OFFSET);
        if (pos) { pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2]; }
        if (lookAt) { lookAt[0] = l[0]; lookAt[1] = l[1]; lookAt[2] = l[2]; }
        DiagLog_EndSafeSeh();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DiagLog_EndSafeSeh();
        return false;
    }
}

static float SampleTerrainHeight(float wx, float wz)
{
    if (!g_collisionTerrainHeight || !SaveSync_IsWorldLoaded()) return 20.0f;
    float h = 20.0f;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE* engine = *(BYTE**)(g_exeBase + ENGINE_RVA);
        BYTE* terrain = engine ? *(BYTE**)(engine + ENGINE_TERRAIN_OFFSET) : nullptr;
        if (terrain && *(BYTE**)(terrain + 0x840) != nullptr) {
            float pos3[3] = { wx, 0.0f, wz };
            float sampled = g_collisionTerrainHeight(terrain, pos3);
            if (isfinite(sampled)) h = sampled;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 20.0f;
    }
    DiagLog_EndSafeSeh();
    return h;
}

float MinimapCameraYaw(float posX, float posZ, float lookX, float lookZ)
{
    float dx = lookX - posX;
    float dz = lookZ - posZ;
    if (!isfinite(dx) || !isfinite(dz)) return 0.0f;
    if (dx * dx + dz * dz < 0.0001f) return 0.0f;
    return atan2f(dx, dz);
}

float MinimapCameraYaw(const float pos[3], const float look[3])
{
    if (!pos || !look) return 0.0f;
    return MinimapCameraYaw(pos[0], pos[2], look[0], look[2]);
}

bool MinimapWorldToPx(float worldX, float worldZ, float cx, float cy, float mapSize,
                      float* outPx, float* outPy, float margin)
{
    if (!outPx || !outPy) return false;
    if (!isfinite(worldX) || !isfinite(worldZ) || !isfinite(cx) || !isfinite(cy) ||
        !isfinite(mapSize) || !isfinite(margin) || mapSize <= 0.0f || margin < 0.0f) {
        return false;
    }
    float half = mapSize * 0.5f;
    if (margin >= half) return false;
    float px = cx + (worldX / 20000.0f) * mapSize;
    float py = cy - (worldZ / 20000.0f) * mapSize;
    if (!isfinite(px) || !isfinite(py)) return false;
    if (px < cx - half + margin || px > cx + half - margin ||
        py < cy - half + margin || py > cy + half - margin) {
        return false;
    }
    *outPx = px;
    *outPy = py;
    return true;
}

bool TwoStates_ApplyCameraView(const float pos[3], const float lookAt[3])
{
    if (!g_exeBase || !IsValidCameraPair(pos, lookAt)) return false;

    float dx = lookAt[0] - pos[0];
    float dy = lookAt[1] - pos[1];
    float dz = lookAt[2] - pos[2];
    float distXZ = sqrtf(dx * dx + dz * dz);
    float yaw = MinimapCameraYaw(pos, lookAt);
    float pitch = (distXZ > 0.001f) ? atan2f(-dy, distXZ) : 0.0f;
    float roll = 0.0f;

    DiagLog_BeginSafeSeh();
    __try {
        // 1. C3D_CAMERA at CAMERA_RVA
        BYTE* cam = g_exeBase + CAMERA_RVA;
        float* cPos    = (float*)(cam + CAMERA_POS_OFFSET);
        float* cPitch  = (float*)(cam + 0x8C);
        float* cYaw    = (float*)(cam + 0x90);
        float* cRoll   = (float*)(cam + 0x94);
        float* cLookAt = (float*)(cam + CAMERA_LOOKAT_OFFSET);

        cPos[0] = pos[0];
        cPos[1] = pos[1];
        cPos[2] = pos[2];

        *cPitch = pitch;
        *cYaw   = yaw;
        *cRoll  = roll;

        cLookAt[0] = lookAt[0];
        cLookAt[1] = lookAt[1];
        cLookAt[2] = lookAt[2];

        // 2. Camera Manager at world + WORLD_CAMERA_MGR_PTR_OFF
        BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
        BYTE* camMgr = (world && IsPointerLikeX64(reinterpret_cast<uint64_t>(world))) ? *(BYTE**)(world + WORLD_CAMERA_MGR_PTR_OFF) : nullptr;
        if (camMgr && IsPointerLikeX64(reinterpret_cast<uint64_t>(camMgr))) {
            float* mPos    = (float*)(camMgr + CAM_MGR_POS_OFFSET);
            float* mRot    = (float*)(camMgr + CAM_MGR_ANGLES_OFFSET);
            float* mCurPos = (float*)(camMgr + CAM_MGR_CUR_POS_OFFSET);
            float* mCurRot = (float*)(camMgr + CAM_MGR_CUR_ANGLES_OFFSET);

            mPos[0] = pos[0];    mPos[1] = pos[1];    mPos[2] = pos[2];
            mRot[0] = pitch;     mRot[1] = yaw;       mRot[2] = roll;
            mCurPos[0] = pos[0]; mCurPos[1] = pos[1]; mCurPos[2] = pos[2];
            mCurRot[0] = pitch;  mCurRot[1] = yaw;    mCurRot[2] = roll;

            // Clear snapback triggers
            *(uint8_t*)(camMgr + CAM_MGR_FOLLOW_ACTIVE_OFF) = 0;
            *(void**)(camMgr + CAM_MGR_RIDE_VEH_PTR_OFF) = nullptr;
            *(void**)(camMgr + CAM_MGR_FOLLOW_WIN_PTR_OFF) = nullptr;

            float* mStep = (float*)(camMgr + CAM_MGR_TRANS_STEP_OFFSET);
            mStep[0] = 0.0f; mStep[1] = 0.0f; mStep[2] = 0.0f;
            *(float*)(camMgr + CAM_MGR_TRANS_TIMER_OFFSET) = 0.0f;
            *(float*)(camMgr + CAM_MGR_TRANS_DUR_OFFSET) = 0.0f;
        }

        // 3. Engine view matrix updates (0x8B1538 / 0x8B1528)
        typedef void (__fastcall* CalcViewFn)(void* cam, void* m1, void* m2);
        typedef void (__fastcall* CalcFinalFn)(void* cam);

        CalcViewFn fnView = nullptr;
        CalcFinalFn fnFinal = nullptr;
        if (g_engine) {
            fnView = (CalcViewFn)GetProcAddress(g_engine, "?CalculateView@C3D_CAMERA@@QEAAXPEAVC3DMATRIX@@0@Z");
            fnFinal = (CalcFinalFn)GetProcAddress(g_engine, "?CalculateFinal@C3D_CAMERA@@QEAAXXZ");
        }
        if (!fnView && g_exeBase) {
            fnView = *(CalcViewFn*)(g_exeBase + 0x8B1538);
        }
        if (!fnFinal && g_exeBase) {
            fnFinal = *(CalcFinalFn*)(g_exeBase + 0x8B1528);
        }
        if (fnView && IsPointerLikeX64(reinterpret_cast<uint64_t>(fnView))) {
            fnView(cam, nullptr, nullptr);
        }
        if (fnFinal && IsPointerLikeX64(reinterpret_cast<uint64_t>(fnFinal))) {
            fnFinal(cam);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DiagLog_EndSafeSeh();
        return false;
    }
    DiagLog_EndSafeSeh();
    return true;
}

static bool ApplyInitialCoopCamera(float posX, float posZ, float lookX, float lookZ,
                                   float outPos[3], float outLook[3])
{
    float groundPos = SampleTerrainHeight(posX, posZ);
    float groundLook = SampleTerrainHeight(lookX, lookZ);
    if (!isfinite(groundPos) || groundPos < 1.0f) groundPos = 20.0f;
    if (!isfinite(groundLook) || groundLook < 1.0f) groundLook = 20.0f;

    float pos[3] = { posX, groundPos + 160.0f, posZ };
    float lookAt[3] = { lookX, groundLook + 10.0f, lookZ };
    if (outPos) memcpy(outPos, pos, sizeof(pos));
    if (outLook) memcpy(outLook, lookAt, sizeof(lookAt));
    return TwoStates_ApplyCameraView(pos, lookAt);
}

void TwoStates_ApplyInitialFraming(bool isHost, bool isTwoStates,
                                   float outPos[3], float outLook[3])
{
    if (isTwoStates) {
        float sign = isHost ? -1.0f : 1.0f;
        ApplyInitialCoopCamera(sign * 80.0f, 0.0f, sign * 450.0f, 0.0f, outPos, outLook);
    } else {
        ApplyInitialCoopCamera(0.0f, 370.0f, 0.0f, 0.0f, outPos, outLook);
    }
}

void TwoStates_CapturePreResyncCamera()
{
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float look[3] = { 0.0f, 0.0f, 0.0f };
    if (TwoStates_GetCameraView(pos, look) && IsValidCameraPair(pos, look)) {
        g_resyncCamPos[0] = pos[0]; g_resyncCamPos[1] = pos[1]; g_resyncCamPos[2] = pos[2];
        g_resyncCamLook[0] = look[0]; g_resyncCamLook[1] = look[1]; g_resyncCamLook[2] = look[2];
        g_hasResyncCam = true;
        Logf("republicmp  CAMERA: captured pre-resync camera (pos=[%.1f, %.1f, %.1f], look=[%.1f, %.1f, %.1f])",
             pos[0], pos[1], pos[2], look[0], look[1], look[2]);
    }
}

void TwoStates_TickPendingCameraRestore()
{
    if (!g_pendingCamRestore.active) return;
    if (!SaveSync_IsWorldLoaded() || Determinism_WorldLoading()) return;

    if (g_pendingCamRestore.framesRemaining > 0) {
        TwoStates_ApplyCameraView(g_pendingCamRestore.pos, g_pendingCamRestore.look);
        --g_pendingCamRestore.framesRemaining;
    }

    ++g_pendingCamRestore.checkFrameCounter;
    if (g_pendingCamRestore.checkFrameCounter >= 60) {
        float curPos[3] = {};
        float curLook[3] = {};
        if (TwoStates_GetCameraView(curPos, curLook)) {
            float dx = curPos[0] - g_pendingCamRestore.pos[0];
            float dy = curPos[1] - g_pendingCamRestore.pos[1];
            float dz = curPos[2] - g_pendingCamRestore.pos[2];
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            Logf("republicmp  CAMERA: post-load check frame 60 (curPos=[%.1f, %.1f, %.1f], targetPos=[%.1f, %.1f, %.1f], diff=%.2f)",
                 curPos[0], curPos[1], curPos[2],
                 g_pendingCamRestore.pos[0], g_pendingCamRestore.pos[1], g_pendingCamRestore.pos[2],
                 dist);
        }
        g_pendingCamRestore.active = false;
    }
}

bool TwoStates_GetPendingCameraRestoreForTest(float outPos[3], float outLook[3], int* outRemaining, int* outFrameCount)
{
    if (outPos) memcpy(outPos, g_pendingCamRestore.pos, sizeof(g_pendingCamRestore.pos));
    if (outLook) memcpy(outLook, g_pendingCamRestore.look, sizeof(g_pendingCamRestore.look));
    if (outRemaining) *outRemaining = g_pendingCamRestore.framesRemaining;
    if (outFrameCount) *outFrameCount = g_pendingCamRestore.checkFrameCounter;
    return g_pendingCamRestore.active;
}

bool TwoStates_HasCapturedResyncCamera()
{
    return g_hasResyncCam;
}

static bool IsInOrbitView()
{
    if (!g_exeBase) return false;
    int32_t activeOrbit = -1;
    return SafeReadPod((const int32_t*)(g_exeBase + WORLD_RVA + WORLD_ACTIVE_ORBIT_VIEW_OFFSET), &activeOrbit) &&
           activeOrbit >= 0;
}

bool TwoStates_JumpCameraTo(float wx, float wz)
{
    if (IsInOrbitView() || !SaveSync_IsWorldLoaded() || !isfinite(wx) || !isfinite(wz)) return false;
    float curPos[3] = {};
    float curLook[3] = {};
    if (!TwoStates_GetCameraView(curPos, curLook)) return false;

    float dx = wx - curLook[0];
    float dz = wz - curLook[2];
    float heightOffset = curPos[1] - curLook[1];
    if (heightOffset < 35.0f || heightOffset > 1500.0f || !isfinite(heightOffset))
        heightOffset = 140.0f;
    float groundY = SampleTerrainHeight(wx, wz);

    float newLook[3] = { wx, groundY, wz };
    float newPos[3] = { curPos[0] + dx, groundY + heightOffset, curPos[2] + dz };
    return TwoStates_ApplyCameraView(newPos, newLook);
}

bool TwoStates_JumpCameraToPartner()
{
    if (IsInOrbitView()) return false;
    PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
    if (!pc.valid) {
        TwoStates_AddToast(Tr("Партнёр ещё не подключён в режиме Lockstep",
                              "Partner is not connected in Lockstep mode yet"), 2);
        return false;
    }
    ULONGLONG now = GetTickCount64();
    float tx = pc.x, tz = pc.z;
    if (pc.hasPing && now - pc.pingReceivedMs < PING_LIFETIME_MS) {
        tx = pc.pingX;
        tz = pc.pingZ;
    } else if (fabsf(tx) < 0.01f && fabsf(tz) < 0.01f) {
        tx = pc.camX;
        tz = pc.camZ;
    }
    if (TwoStates_JumpCameraTo(tx, tz)) {
        char msg[160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    Tr("[КАМЕРА] Перемещена к партнёру (X=%.0f м, Z=%.0f м)",
                       "[CAMERA] Jumped to partner (X=%.0f m, Z=%.0f m)"),
                    tx, tz);
        TwoStates_AddToast(msg, 0);
        return true;
    }
    return false;
}

void TwoStates_SetShow3DBorder(bool on)       { g_show3DBorder = on; }
bool TwoStates_GetShow3DBorder()              { return g_show3DBorder; }
void TwoStates_SetShowMinimapBorders(bool on) { g_showMinimapBorders = on; }
bool TwoStates_GetShowMinimapBorders()        { return g_showMinimapBorders; }

void TwoStates_PingAtCursor()
{
    if (IsInOrbitView()) return;
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    if (!TwoStates_GetCursorWorldPos(&cx, &cy, &cz)) return;
    TwoStates_GetCameraLookAt(&lx, &ly, &lz);
    if (fabsf(cx) < 0.01f && fabsf(cz) < 0.01f) {
        cx = lx; cy = ly; cz = lz;
    }
    g_localPingActive = true;
    g_localPingX      = cx;
    g_localPingY      = cy;
    g_localPingZ      = cz;
    g_localPingSentMs = GetTickCount64();

    if (LockstepNet_IsConnected()) {
        float cpos[3] = { 0.0f, 0.0f, 0.0f };
        float clook[3] = { lx, ly, lz };
        TwoStates_GetCameraPos(&cpos[0], &cpos[1], &cpos[2]);
        LockstepNet_SendCursor(cx, cy, cz, cpos, clook, true);
    }
    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                Tr("[СИГНАЛ] Отправлен на карту на 20 сек (X=%.0f м, Z=%.0f м)",
                   "[PING] Sent on map for 20s (X=%.0f m, Z=%.0f m)"),
                cx, cz);
    TwoStates_AddToast(msg, 1);
    SaveStatus(Tr("[КАРТА] Отправлен сигнал партнёру: X=%.0f м, Z=%.0f м",
                  "[MAP] Ping sent to partner: X=%.0f m, Z=%.0f m"), cx, cz);
}

static bool WorldToScreen(float wx, float wy, float wz, const ImVec2& displaySize, ImVec2* outScreen)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        const float* m = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_VIEWPROJ_OFFSET);
        float clipX = wx * m[0] + wy * m[4] + wz * m[8]  + m[12];
        float clipY = wx * m[1] + wy * m[5] + wz * m[9]  + m[13];
        float clipW = wx * m[3] + wy * m[7] + wz * m[11] + m[15];
        if (clipW > 0.5f && isfinite(clipW)) {
            float invW = 1.0f / clipW;
            float ndcX = clipX * invW;
            float ndcY = clipY * invW;
            if (ndcX >= -1.6f && ndcX <= 1.6f && ndcY >= -1.6f && ndcY <= 1.6f) {
                outScreen->x = (ndcX * 0.5f + 0.5f) * displaySize.x;
                outScreen->y = (0.5f - ndcY * 0.5f) * displaySize.y;
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void Draw3DTextBadge(ImDrawList* dl, const ImVec2& pos, const char* text, ImU32 bgCol, ImU32 borderCol, ImU32 textCol)
{
    ImVec2 sz = ImGui::CalcTextSize(text);
    ImVec2 pMin(pos.x - sz.x * 0.5f - 7.0f, pos.y - sz.y * 0.5f - 4.0f);
    ImVec2 pMax(pos.x + sz.x * 0.5f + 7.0f, pos.y + sz.y * 0.5f + 4.0f);
    dl->AddRectFilled(pMin, pMax, bgCol, 5.0f);
    dl->AddRect(pMin, pMax, borderCol, 5.0f, 0, 1.5f);
    dl->AddText(ImVec2(pos.x - sz.x * 0.5f, pos.y - sz.y * 0.5f), textCol, text);
}

// ---------------------------------------------------------------------------
// Native Minimap Territory Overlay & 6th Resource-Strip Toggle Button
// (Hooks on 0x4BDE80 Minimap_DrawMapContent & 0x4BFF40 Minimap_DrawLayerButtons)
// ---------------------------------------------------------------------------

typedef void (__fastcall* FnTexBind)(void* texPtr, uint32_t slot, void* rendererPtr);
typedef void (__fastcall* FnHudDrawQuad)(void* stage, float u0, float v0, float u1, float v1, float a6, uint8_t a7);
typedef float* (__fastcall* FnHudGetMousePos)(void* mouseState, float outPt[3]);
typedef uint8_t (__fastcall* FnHudHitTestQuad)(void* stage, const float mousePt[3], float padX, float padY);
typedef void (__cdecl* FnTextDrawW)(void* textMgr, void* font, float x, float y, uint32_t colorArgb, const wchar_t* fmt, ...);
typedef int* (__fastcall* FnHudGetTextRect)(void* font, int* outRect, float x, float y, const wchar_t* text);
typedef void (__fastcall* FnHudEndDraw)(void* stage);
typedef void (__fastcall* FnHudBeginDraw)(void* stage, void* arg2, uint8_t flag);
typedef void (__fastcall* FnTextPrintAll)(void* textMgr, const char* passName);
typedef void* (__fastcall* FnHudCreateManagedTex)(void* middlePoint, const char* relPath);
typedef int (__fastcall* FnHudTexLoadFile)(void* tex, const char* relPath, int a3, int a4, int a5, int a6);

static void*     g_minimapBordersIconTex       = nullptr;
static uintptr_t g_minimapBordersIconVtbl      = 0;
static bool      g_minimapBordersIconAttempted = false;

static bool BindHudTexturePtr(void* tex)
{
    if (!g_exeBase || !IsPointerLikeX64((uintptr_t)tex)) return false;
    void* renderer = nullptr;
    if (!SafeReadPod<void*>((const void*)(g_exeBase + HUD_RENDERER_PTR_RVA), &renderer) ||
        !IsPointerLikeX64((uintptr_t)renderer)) return false;
    uintptr_t vtbl = 0;
    if (!SafeReadPod<uintptr_t>((const void*)tex, &vtbl) ||
        !IsPointerLikeX64(vtbl)) return false;
    FnTexBind bindFn = nullptr;
    if (!SafeReadPod<FnTexBind>((const void*)(vtbl + HUD_TEX_BIND_VTABLE_OFF), &bindFn) ||
        !IsPointerLikeX64((uintptr_t)bindFn)) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        bindFn(tex, 0, renderer);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool BindHudTexture(uintptr_t texPtrRva)
{
    if (!g_exeBase) return false;
    void* tex = nullptr;
    if (!SafeReadPod<void*>((const void*)(g_exeBase + texPtrRva), &tex)) return false;
    return BindHudTexturePtr(tex);
}

static void* GetOrLoadMinimapBordersIconTex()
{
    if (!g_exeBase) return nullptr;
    if (g_minimapBordersIconAttempted && g_minimapBordersIconTex) {
        if (IsPointerLikeX64((uintptr_t)g_minimapBordersIconTex) &&
            g_minimapBordersIconVtbl != 0 &&
            *(uintptr_t*)g_minimapBordersIconTex == g_minimapBordersIconVtbl &&
            IsPointerLikeX64(g_minimapBordersIconVtbl)) {
            return g_minimapBordersIconTex;
        }
        g_minimapBordersIconTex       = nullptr;
        g_minimapBordersIconVtbl      = 0;
        g_minimapBordersIconAttempted = false;
    }
    if (g_minimapBordersIconAttempted) {
        return g_minimapBordersIconTex;
    }
    g_minimapBordersIconAttempted = true;
    FnHudCreateManagedTex createTex = *(FnHudCreateManagedTex*)(g_exeBase + RVA_IAT_CREATE_MANAGED_TEX);
    if (!IsPointerLikeX64((uintptr_t)createTex)) return nullptr;
    const char* relPath = "editor/mp_icon_mode.png";
    void* tex = createTex(g_exeBase + HUD_MIDDLEPOINT_OBJ_RVA, relPath);
    if (IsPointerLikeX64((uintptr_t)tex)) {
        uintptr_t vtbl = *(uintptr_t*)tex;
        if (IsPointerLikeX64(vtbl)) {
            FnHudTexLoadFile loadFn = *(FnHudTexLoadFile*)(vtbl + 2 * sizeof(void*));
            if (IsPointerLikeX64((uintptr_t)loadFn)) {
                g_minimapBordersIconVtbl = vtbl;
                loadFn(tex, relPath, 0, 0, 1, 0);
                g_minimapBordersIconTex = tex;
            }
        }
    }
    return g_minimapBordersIconTex;
}

static void SetHudQuadStage(float cx, float cy, float w, float h, const float rgba[4], float rotRad = 0.0f)
{
    *(float*)(g_exeBase + HUD_QUAD_SIZE_X_RVA) = w;
    *(float*)(g_exeBase + HUD_QUAD_SIZE_Y_RVA) = h;
    *(float*)(g_exeBase + HUD_QUAD_POS_X_RVA)  = cx;
    *(float*)(g_exeBase + HUD_QUAD_POS_Y_RVA)  = cy;
    *(uint32_t*)(g_exeBase + HUD_QUAD_Z_RVA)   = 0;
    *(float*)(g_exeBase + HUD_QUAD_ROT_RVA)    = rotRad;
    if (rgba) {
        memcpy(g_exeBase + HUD_QUAD_COLOR_RVA, rgba, 4 * sizeof(float));
    }
}

static void DrawHudRotatedQuad(float cx, float cy, float w, float h, float rotRad,
                               const float rgba[4], bool fullTexUv = false)
{
    if (!g_exeBase || !rgba || !isfinite(cx) || !isfinite(cy) || !isfinite(w) || !isfinite(h) ||
        !isfinite(rotRad) || w <= 0.0f || h <= 0.0f) return;
    SetHudQuadStage(cx, cy, w, h, rgba, rotRad);
    __try {
        FnHudDrawQuad drawFn = nullptr;
        if (SafeReadPod<FnHudDrawQuad>((const void*)(g_exeBase + RVA_IAT_HUD_DRAW_QUAD), &drawFn) &&
            IsPointerLikeX64((uintptr_t)drawFn)) {
            float u0 = fullTexUv ? 0.0f : 0.25f;
            float u1 = fullTexUv ? 1.0f : 0.75f;
            drawFn(g_exeBase + HUD_QUAD_STAGE_RVA, u0, u0, u1, u1, 0.0f, 1);
        }
    } __finally {
        *(float*)(g_exeBase + HUD_QUAD_ROT_RVA) = 0.0f;
    }
}

static void DrawHudSolidQuad(float cx, float cy, float w, float h, const float rgba[4], bool fullTexUv = false)
{
    DrawHudRotatedQuad(cx, cy, w, h, 0.0f, rgba, fullTexUv);
}

static void DrawHudDisk(float cx, float cy, float radius, const float rgba[4])
{
    if (!rgba || !isfinite(cx) || !isfinite(cy) || !isfinite(radius) || radius <= 0.0f) return;
    float sz = radius * 2.0f;
    for (int i = 0; i < 4; ++i) {
        float rotRad = (float)i * (3.14159265f / 8.0f);
        DrawHudRotatedQuad(cx, cy, sz, sz, rotRad, rgba, false);
    }
}

static void DrawHudRing(float cx, float cy, float radius, float thick, const float rgba[4])
{
    if (!rgba || !isfinite(cx) || !isfinite(cy) || !isfinite(radius) || !isfinite(thick) ||
        radius <= 0.0f || thick <= 0.0f) return;
    const int n = 16;
    float chord = 2.0f * 3.14159265f * radius / (float)n * 1.15f;
    for (int i = 0; i < n; ++i) {
        float ang = (float)i * (2.0f * 3.14159265f / (float)n);
        float sx = cx + radius * cosf(ang);
        float sy = cy + radius * sinf(ang);
        float rot = ang + (3.14159265f * 0.5f);
        DrawHudRotatedQuad(sx, sy, chord, thick, rot, rgba, false);
    }
}

static void DrawHudHollowRect(float cx, float cy, float w, float h, float thickness, const float rgba[4])
{
    if (!rgba || !isfinite(cx) || !isfinite(cy) || !isfinite(w) || !isfinite(h) || !isfinite(thickness) ||
        thickness <= 0.0f || w <= thickness * 2.0f || h <= thickness * 2.0f) return;
    float halfW = w * 0.5f;
    float halfH = h * 0.5f;
    float halfT = thickness * 0.5f;
    DrawHudSolidQuad(cx, cy - halfH + halfT, w, thickness, rgba);
    DrawHudSolidQuad(cx, cy + halfH - halfT, w, thickness, rgba);
    DrawHudSolidQuad(cx - halfW + halfT, cy, thickness, h - thickness * 2.0f, rgba);
    DrawHudSolidQuad(cx + halfW - halfT, cy, thickness, h - thickness * 2.0f, rgba);
}

static void FlushHudQueuedTexts()
{
    if (!g_exeBase) return;
    FnHudEndDraw   endDraw   = *(FnHudEndDraw*)(g_exeBase + RVA_IAT_HUD_END_DRAW);
    FnTextPrintAll printAll  = *(FnTextPrintAll*)(g_exeBase + RVA_IAT_TEXT_PRINT_ALL);
    FnHudBeginDraw beginDraw = *(FnHudBeginDraw*)(g_exeBase + RVA_IAT_HUD_BEGIN_DRAW);
    if (IsPointerLikeX64((uintptr_t)endDraw) &&
        IsPointerLikeX64((uintptr_t)printAll) &&
        IsPointerLikeX64((uintptr_t)beginDraw)) {
        endDraw(g_exeBase + HUD_QUAD_STAGE_RVA);
        printAll(g_exeBase + HUD_TEXT_MGR_RVA, (const char*)(g_exeBase + HUD_BASIC_PASS_STR_RVA));
        beginDraw(g_exeBase + HUD_QUAD_STAGE_RVA, nullptr, 0);
    }
}

static void DrawHudMinimapTextPill(float cx, float cy, float maxPillW, float uiScale,
                                   const wchar_t* label, const float bgCol[4], const float brdCol[4], uint32_t textArgb)
{
    if (!g_exeBase || !label || !label[0]) return;
    void* font = *(void**)(g_exeBase + HUD_FONT_MAIN_PTR_RVA);
    FnTextDrawW drawText = *(FnTextDrawW*)(g_exeBase + RVA_IAT_TEXT_DRAW_W);
    if (!IsPointerLikeX64((uintptr_t)font) || !IsPointerLikeX64((uintptr_t)drawText)) return;

    uintptr_t fontVtbl = *(uintptr_t*)font;
    if (!IsPointerLikeX64(fontVtbl)) return;
    FnHudGetTextRect getTextRect = *(FnHudGetTextRect*)(fontVtbl + 5 * sizeof(void*));
    int textRect[4] = { 0, 0, (int)(wcslen(label) * 8), 14 };
    if (IsPointerLikeX64((uintptr_t)getTextRect)) {
        getTextRect(font, textRect, 0.0f, 0.0f, label);
    }

    float textW = (float)(textRect[2] - textRect[0]);
    float textH = (float)(textRect[3] - textRect[1]);
    if (textW < 20.0f) textW = (float)wcslen(label) * 7.5f * uiScale;
    if (textH < 10.0f) textH = 14.0f * uiScale;

    float pillW = fminf(maxPillW, fmaxf(52.0f * uiScale, textW + 14.0f * uiScale));
    float pillH = fmaxf(18.0f * uiScale, textH + 6.0f * uiScale);

    DrawHudSolidQuad(cx, cy, pillW, pillH, bgCol);
    DrawHudHollowRect(cx, cy, pillW, pillH, fmaxf(1.0f, 1.3f * uiScale), brdCol);

    float tx = floorf(cx - 0.5f * (float)(textRect[0] + textRect[2]) + 0.5f);
    float ty = floorf(cy - 0.5f * (float)(textRect[1] + textRect[3]) + 0.5f);
    const wchar_t* fmt = (const wchar_t*)(g_exeBase + HUD_FMT_WSTR_RVA);
    drawText(g_exeBase + HUD_TEXT_MGR_RVA, font, tx, ty, textArgb, fmt, label);
    BindHudTexture(HUD_SOLID_TEX_PTR_RVA);
}

static void DrawNativeMinimapTwoStatesBorders(float cx, float cy, float mapSize, float uiScale)
{
    int myState = TwoStates_LocalState(); // 1 = West (X < 0), 2 = East (X > 0)
    float half = mapSize * 0.5f;
    float borderZoneHalfPx = (g_borderHalfWidth / 20000.0f) * mapSize;
    if (borderZoneHalfPx < 1.5f) borderZoneHalfPx = 1.5f;
    if (borderZoneHalfPx > half * 0.45f) borderZoneHalfPx = half * 0.45f;

    const float ownTint[4]     = { 0.18f, 0.82f, 0.37f, 0.10f };
    const float partnerTint[4] = { 0.92f, 0.25f, 0.22f, 0.10f };
    const float* westTint      = (myState == 1) ? ownTint : partnerTint;
    const float* eastTint      = (myState == 2) ? ownTint : partnerTint;

    const float zoneCol[4]     = { 1.00f, 0.84f, 0.24f, 0.15f };
    const float westEdgeCol[4] = { 0.35f, 0.78f, 1.00f, 0.72f };
    const float eastEdgeCol[4] = { 1.00f, 0.69f, 0.27f, 0.72f };
    const float brdRedCol[4]   = { 1.00f, 0.25f, 0.20f, 0.92f };
    const float brdGoldCol[4]  = { 1.00f, 0.90f, 0.37f, 0.85f };
    const float ownFrameCol[4] = { 0.30f, 0.94f, 0.47f, 0.80f };

    // Subtle territory tint on West (X < 0) and East (X > 0) halves (matching v1.4.1)
    float sideW = half - borderZoneHalfPx;
    if (sideW > 2.0f) {
        DrawHudSolidQuad(cx - borderZoneHalfPx - sideW * 0.5f, cy, sideW, mapSize, westTint);
        DrawHudSolidQuad(cx + borderZoneHalfPx + sideW * 0.5f, cy, sideW, mapSize, eastTint);
    }

    DrawHudSolidQuad(cx, cy, borderZoneHalfPx * 2.0f, mapSize, zoneCol);
    DrawHudSolidQuad(cx - borderZoneHalfPx, cy, fmaxf(1.0f, 1.2f * uiScale), mapSize, westEdgeCol);
    DrawHudSolidQuad(cx + borderZoneHalfPx, cy, fmaxf(1.0f, 1.2f * uiScale), mapSize, eastEdgeCol);
    DrawHudSolidQuad(cx, cy, fmaxf(2.0f, 2.4f * uiScale), mapSize, brdRedCol);
    DrawHudSolidQuad(cx, cy, fmaxf(1.0f, 1.1f * uiScale), mapSize, brdGoldCol);

    // Outer 3-edge highlight on player's own half (no duplicate vertical line on the X=0 center border)
    float frameT  = fmaxf(1.5f, 2.0f * uiScale);
    float halfT   = frameT * 0.5f;
    float ownCx   = (myState == 1) ? (cx - half * 0.5f) : (cx + half * 0.5f);
    float outerX  = (myState == 1) ? (cx - half + halfT) : (cx + half - halfT);
    DrawHudSolidQuad(ownCx, cy - half + halfT, half, frameT, ownFrameCol);
    DrawHudSolidQuad(ownCx, cy + half - halfT, half, frameT, ownFrameCol);
    DrawHudSolidQuad(outerX, cy, frameT, mapSize - frameT * 2.0f, ownFrameCol);

    const float ownBg[4]      = { 0.06f, 0.20f, 0.09f, 0.90f };
    const float ownBrd[4]     = { 0.31f, 0.94f, 0.49f, 0.96f };
    const float partnerBg[4]  = { 0.22f, 0.08f, 0.08f, 0.90f };
    const float partnerBrd[4] = { 1.00f, 0.43f, 0.37f, 0.94f };

    const wchar_t* westLbl = (myState == 1)
        ? TrW(L"ЗАПАД (ВЫ)", L"WEST (YOU)")
        : TrW(L"ЗАПАД", L"WEST");
    const wchar_t* eastLbl = (myState == 2)
        ? TrW(L"ВОСТОК (ВЫ)", L"EAST (YOU)")
        : TrW(L"ВОСТОК", L"EAST");

    float pillY    = (cy - half) + 14.0f * uiScale;
    float maxPillW = half - 10.0f * uiScale;
    if (maxPillW > 40.0f) {
        DrawHudMinimapTextPill(cx - half * 0.5f, pillY, maxPillW, uiScale, westLbl,
                               (myState == 1) ? ownBg : partnerBg,
                               (myState == 1) ? ownBrd : partnerBrd,
                               (myState == 1) ? 0xFFB9FFC8u : 0xFFFFCDC3u);
        DrawHudMinimapTextPill(cx + half * 0.5f, pillY, maxPillW, uiScale, eastLbl,
                               (myState == 2) ? ownBg : partnerBg,
                               (myState == 2) ? ownBrd : partnerBrd,
                               (myState == 2) ? 0xFFB9FFC8u : 0xFFFFCDC3u);
    }
}

static void DrawHudPulsingMarker(float px, float py, float cx, float cy, float half,
                                 float innerRadius, float outerRadius, float thickness,
                                 const float innerCol[4], const float outerCol[4])
{
    if (!innerCol || !outerCol || !isfinite(px) || !isfinite(py) || !isfinite(cx) || !isfinite(cy) || !isfinite(half)) return;
    if (px < cx - half + 2.0f || px > cx + half - 2.0f ||
        py < cy - half + 2.0f || py > cy + half - 2.0f) {
        return;
    }
    DrawHudRing(px, py, outerRadius, thickness, outerCol);
    DrawHudDisk(px, py, innerRadius, innerCol);
}

static void DrawNativeMinimapPartner(const PartnerCursorInfo& pc, float cx, float cy, float mapSize, float uiScale)
{
    if (!pc.valid) return;
    ULONGLONG now = GetTickCount64();
    if (now - pc.lastUpdateMs >= 10000) return;
    if (!isfinite(pc.camPos[0]) || !isfinite(pc.camPos[2])) return;

    float px = 0.0f, py = 0.0f;
    if (!MinimapWorldToPx(pc.camPos[0], pc.camPos[2], cx, cy, mapSize, &px, &py)) {
        return;
    }

    float yaw = MinimapCameraYaw(pc.camPos, pc.camLookAt);
    float coneSz = fminf(mapSize * MINIMAP_CAMERA_ICON_MAP_FRACTION, uiScale * MINIMAP_CAMERA_ICON_MAX_PX) * 0.85f;

    if (BindHudTexture(HUD_MAP_CAMERA_TEX_PTR_RVA)) {
        const float cyanConeCol[4] = { 0.24f, 0.84f, 1.00f, 0.92f };
        DrawHudRotatedQuad(px, py, coneSz, coneSz, yaw, cyanConeCol, true);
        BindHudTexture(HUD_SOLID_TEX_PTR_RVA);
    }

    const float whiteCol[4]    = { 1.00f, 1.00f, 1.00f, 0.95f };
    const float cyanDiskCol[4] = { 0.24f, 0.84f, 1.00f, 1.00f };
    DrawHudDisk(px, py, 5.5f * uiScale, whiteCol);
    DrawHudDisk(px, py, 3.8f * uiScale, cyanDiskCol);
}

static void DrawNativeMinimapMarkers(float cx, float cy, float mapSize, float uiScale)
{
    float half = mapSize * 0.5f;
    ULONGLONG now = GetTickCount64();

    if (g_localPingActive && now - g_localPingSentMs < PING_LIFETIME_MS) {
        float ageSec = (float)(now - g_localPingSentMs) * 0.001f;
        float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
        float px = 0.0f, py = 0.0f;
        if (MinimapWorldToPx(g_localPingX, g_localPingZ, cx, cy, mapSize, &px, &py)) {
            const float inCol[4]  = { 0.31f, 1.00f, 0.47f, 1.00f };
            const float outCol[4] = { 0.43f, 0.96f, 0.51f, 0.92f };
            DrawHudPulsingMarker(px, py, cx, cy, half, 3.8f * uiScale, (5.0f + pulse * 7.0f) * uiScale,
                                 1.8f * uiScale, inCol, outCol);
        }
    }

    if (LockstepNet_IsConnected()) {
        PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
        if (pc.valid) {
            DrawNativeMinimapPartner(pc, cx, cy, mapSize, uiScale);

            if (pc.hasPing && now - pc.pingReceivedMs < PING_LIFETIME_MS) {
                float ageSec = (float)(now - pc.pingReceivedMs) * 0.001f;
                float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
                float px = 0.0f, py = 0.0f;
                if (MinimapWorldToPx(pc.pingX, pc.pingZ, cx, cy, mapSize, &px, &py)) {
                    const float inCol[4]  = { 1.00f, 0.33f, 0.16f, 1.00f };
                    const float outCol[4] = { 1.00f, 0.84f, 0.20f, 0.96f };
                    DrawHudPulsingMarker(px, py, cx, cy, half, 4.2f * uiScale, (6.0f + pulse * 8.0f) * uiScale,
                                         2.0f * uiScale, inCol, outCol);
                }
            }
        }
    }
}

static void DrawNativeMinimapOverlay(BYTE* mm)
{
    if (!g_exeBase || !IsPointerLikeX64((uintptr_t)mm) || mm[0] == 0) return;
    float rightX  = *(float*)(mm + 0x48);
    float bottomY = *(float*)(mm + 0x4C);
    float mapSize = *(float*)(mm + 0x50);
    float panelW  = *(float*)(mm + 0x54);
    float panelH  = *(float*)(mm + 0x58);
    if (!isfinite(mapSize) || mapSize < 40.0f || mapSize > 2500.0f) return;
    if (!isfinite(rightX) || !isfinite(bottomY) || !isfinite(panelW) || !isfinite(panelH)) return;

    float cx = (rightX - panelW) + panelH * 0.5f;
    float cy = bottomY - panelH * 0.5f;
    float uiScale = *(float*)(g_exeBase + HUD_UI_SCALE_RVA);
    if (!isfinite(uiScale) || uiScale < 0.5f || uiScale > 3.0f) uiScale = 1.0f;

    if (BindHudTexture(HUD_SOLID_TEX_PTR_RVA)) {
        bool drewBorders = (TwoStates_IsActive() && g_showMinimapBorders);
        if (drewBorders) {
            DrawNativeMinimapTwoStatesBorders(cx, cy, mapSize, uiScale);
        }
        DrawNativeMinimapMarkers(cx, cy, mapSize, uiScale);
        if (drewBorders) {
            // Flush queued text pills inside the minimap pass (matching 0x4BEA85..0x4BEAB2 in 0x4BDE80)
            FlushHudQueuedTexts();
        }
    }

    // Restore C3D_HUD quad stage to full map square (cx, cy, mapSize, mapSize) as expected by 0x4BEB10 at 0x4BECFE
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    SetHudQuadStage(cx, cy, mapSize, mapSize, white);
}

bool TwoStates_TestMinimapRotResetForTest()
{
    if (!g_exeBase) return false;
    *(float*)(g_exeBase + HUD_QUAD_ROT_RVA) = 2.5f;
    const float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    DrawHudRotatedQuad(100.0f, 100.0f, 20.0f, 20.0f, 1.2f, col, true);
    float rot = *(float*)(g_exeBase + HUD_QUAD_ROT_RVA);
    return (rot == 0.0f);
}

static bool IsMinimapMouseInputAllowed(const BYTE* mm)
{
    if (!g_exeBase || !mm) return false;
    if (mm[0x5C] != 0 || mm[0x69] != 0) return false;
    if (*(BYTE*)(g_exeBase + 0xA1FF13) != 0) return false;
    const BYTE* pMgr = *(const BYTE* const*)(g_exeBase + 0xA2C408);
    if (IsPointerLikeX64((uintptr_t)pMgr) && pMgr[0x160] != 0) return false;
    if (*(BYTE*)(g_exeBase + 0xA1FF10) != 0 && *(BYTE*)(g_exeBase + 0xA1FF0A) == 0) return false;
    return true;
}

static void DrawNativeMinimapSplitIcon(float btnX, float btnY, float iconSz, float alpha)
{
    if (!BindHudTexture(HUD_SOLID_TEX_PTR_RVA)) return;
    int myState = TwoStates_LocalState();
    const float bgCol[4]      = { 0.08f, 0.10f, 0.13f, 0.90f * alpha };
    const float ownCol[4]     = { 0.22f, 0.86f, 0.42f, 0.85f * alpha };
    const float partnerCol[4] = { 0.92f, 0.28f, 0.24f, 0.85f * alpha };
    const float brdCol[4]     = { 1.00f, 0.88f, 0.32f, 0.98f * alpha };
    const float* westCol      = (myState == 1) ? ownCol : partnerCol;
    const float* eastCol      = (myState == 2) ? ownCol : partnerCol;

    float innerSz = iconSz * 0.78f;
    float halfW   = innerSz * 0.44f;
    DrawHudSolidQuad(btnX, btnY, innerSz, innerSz, bgCol);
    DrawHudSolidQuad(btnX - innerSz * 0.24f, btnY, halfW, innerSz * 0.86f, westCol);
    DrawHudSolidQuad(btnX + innerSz * 0.24f, btnY, halfW, innerSz * 0.86f, eastCol);
    DrawHudSolidQuad(btnX, btnY, fmaxf(2.0f, iconSz * 0.08f), innerSz * 0.92f, brdCol);
}

static void DrawNativeMinimapBordersButton(BYTE* mm, float btnX, float btnY, float btnStep, float hitScale)
{
    if (!g_exeBase || !IsPointerLikeX64((uintptr_t)mm) || mm[0] == 0) return;
    float iconSz = btnStep * 0.70f;

    const float halfWhite[4] = { 1.0f, 1.0f, 1.0f, 0.5f };
    const float fullWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    BindHudTexture(HUD_BTN_FRAME_TEX_PTR_RVA);
    SetHudQuadStage(btnX, btnY, btnStep, btnStep, halfWhite);

    bool hovered = false;
    if (IsMinimapMouseInputAllowed(mm)) {
        FnHudGetMousePos getMousePos = *(FnHudGetMousePos*)(g_exeBase + RVA_IAT_HUD_GET_MOUSE_POS);
        FnHudHitTestQuad hitTestQuad = *(FnHudHitTestQuad*)(g_exeBase + RVA_IAT_HUD_HIT_TEST_QUAD);
        if (IsPointerLikeX64((uintptr_t)getMousePos) && IsPointerLikeX64((uintptr_t)hitTestQuad)) {
            float mousePt[4] = {};
            float* mp = getMousePos(g_exeBase + HUD_MOUSE_STATE_RVA, mousePt);
            float hitPad = 5.0f * hitScale;
            hovered = (hitTestQuad(g_exeBase + HUD_QUAD_STAGE_RVA, mp, hitPad, hitPad) != 0);
        }
    }

    if (hovered) {
        if (*(BYTE*)(g_exeBase + HUD_MOUSE_LCLICK_EDGE_RVA) != 0) {
            g_showMinimapBorders = !g_showMinimapBorders;
            TwoStates_SaveSettings();
        }
        const wchar_t* tip = g_showMinimapBorders
            ? TrW(L"Границы государств на миникарте (ВКЛ)", L"State borders on minimap (ON)")
            : TrW(L"Границы государств на миникарте (ВЫКЛ)", L"State borders on minimap (OFF)");
        wcsncpy_s((wchar_t*)(g_exeBase + MINIMAP_TOOLTIP_WSTR_RVA), 2048, tip, _TRUNCATE);
    }

    const float* btnCol = hovered ? fullWhite : halfWhite;
    DrawHudSolidQuad(btnX, btnY, btnStep, btnStep, btnCol, true);

    void* iconTex = GetOrLoadMinimapBordersIconTex();
    if (iconTex && BindHudTexturePtr(iconTex)) {
        DrawHudSolidQuad(btnX, btnY, iconSz, iconSz, btnCol, true);
    } else {
        DrawNativeMinimapSplitIcon(btnX, btnY, iconSz, hovered ? 1.0f : 0.55f);
    }

    if (g_showMinimapBorders && BindHudTexture(HUD_PIN_ICON_TEX_PTR_RVA)) {
        float pinOff = btnStep * 0.20f;
        float pinSz  = btnStep * 0.50f;
        DrawHudSolidQuad(btnX + pinOff, btnY + pinOff, pinSz, pinSz, fullWhite, true);
    }
}

static void __fastcall H_MinimapDrawContent(BYTE* mm)
{
    if (o_minimapDrawContent) {
        o_minimapDrawContent(mm);
    }
    const bool inCoop = g_inSession || SaveSync_IsBusy() || SaveSync_IsWaitingLobbyActive();
    if (!inCoop || !SaveSync_IsWorldLoaded()) return;
    DiagLog_BeginSafeSeh();
    __try {
        DrawNativeMinimapOverlay(mm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_exeBase) {
            *(float*)(g_exeBase + HUD_QUAD_ROT_RVA) = 0.0f;
        }
    }
    DiagLog_EndSafeSeh();
}

static void __fastcall H_MinimapDrawButtons(BYTE* mm)
{
    const bool inCoop = g_inSession || SaveSync_IsBusy() || SaveSync_IsWaitingLobbyActive();
    const bool activeTwoStates = inCoop && TwoStates_IsActive() && SaveSync_IsWorldLoaded() &&
                                 g_exeBase && IsPointerLikeX64((uintptr_t)mm);
    if (!activeTwoStates) {
        if (o_minimapDrawButtons) {
            o_minimapDrawButtons(mm);
        }
        return;
    }

    DiagLog_BeginSafeSeh();
    __try {
        float uiScale = *(float*)(g_exeBase + HUD_UI_SCALE_RVA);
        if (!isfinite(uiScale) || uiScale < 0.5f || uiScale > 3.0f) uiScale = 1.0f;
        float rightX  = *(float*)(mm + 0x48);
        float bottomY = *(float*)(mm + 0x4C);
        float panelH  = *(float*)(mm + 0x58);

        if (isfinite(rightX) && isfinite(bottomY) && isfinite(panelH) && panelH > 180.0f * uiScale) {
            float availH    = panelH - 165.0f * uiScale;
            float btnFactor = fminf(1.0f, fmaxf(0.80f, availH / (6.0f * 55.0f * uiScale)));
            float effScale  = uiScale * btnFactor;
            float topY      = bottomY - panelH;
            float startOff  = (78.0f + 8.0f * (btnFactor - 0.80f) / 0.20f) * uiScale;

            *(float*)(g_exeBase + HUD_UI_SCALE_RVA) = effScale;
            *(float*)(mm + 0x48) = rightX - 27.5f * uiScale * (1.0f - btnFactor);
            *(float*)(mm + 0x58) = panelH + 86.0f * effScale - startOff;

            if (o_minimapDrawButtons) {
                o_minimapDrawButtons(mm);
            }

            *(float*)(g_exeBase + HUD_UI_SCALE_RVA) = uiScale;
            *(float*)(mm + 0x48) = rightX;
            *(float*)(mm + 0x58) = panelH;

            float btnStep = 55.0f * effScale;
            float btnX    = rightX - 27.5f * uiScale;
            float btnY    = topY + startOff + 5.0f * btnStep;
            DrawNativeMinimapBordersButton(mm, btnX, btnY, btnStep, effScale);
        } else if (o_minimapDrawButtons) {
            o_minimapDrawButtons(mm);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

// ---------------------------------------------------------------------------
// 3D World Borders, Partner Cursor, Map Ping & Toast ImGui Overlays
// ---------------------------------------------------------------------------

static void Draw3DBorderLines(ImDrawList* dl, const ImVec2& displaySize)
{
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    if (!TwoStates_GetCameraLookAt(&lx, &ly, &lz)) return;
    if (fabsf(lx) > (g_borderHalfWidth + 3800.0f)) return; // camera is far away from the X=0 border

    float zMin = lz - 2400.0f;
    float zMax = lz + 2400.0f;
    if (zMin < -14500.0f) zMin = -14500.0f;
    if (zMax >  14500.0f) zMax =  14500.0f;
    const float step = 60.0f;

    // 1. Center state border line X = 0.0f
    ImVec2 prevPt;
    bool havePrev = false;
    for (float z = zMin; z <= zMax; z += step) {
        float y = SampleTerrainHeight(0.0f, z) + 2.5f;
        ImVec2 pt;
        if (WorldToScreen(0.0f, y, z, displaySize, &pt)) {
            if (havePrev) {
                dl->AddLine(prevPt, pt, IM_COL32(255, 70, 50, 225), 3.5f);
                dl->AddLine(prevPt, pt, IM_COL32(255, 220, 90, 180), 1.5f);
            }
            // Draw vertical border post every 240m
            int zIdx = (int)roundf(z / 60.0f);
            if ((zIdx % 4) == 0) {
                ImVec2 topPt;
                if (WorldToScreen(0.0f, y + 18.0f, z, displaySize, &topPt)) {
                    dl->AddLine(pt, topPt, IM_COL32(255, 215, 70, 210), 2.2f);
                    dl->AddCircleFilled(topPt, 4.0f, IM_COL32(255, 65, 50, 240));
                }
            }
            prevPt = pt;
            havePrev = true;
        } else {
            havePrev = false;
        }
    }

    // 2. Shared border zone lines X = -g_borderHalfWidth and X = +g_borderHalfWidth
    float edges[2] = { -g_borderHalfWidth, g_borderHalfWidth };
    ImU32 edgeCols[2] = { IM_COL32(80, 190, 255, 175), IM_COL32(255, 170, 70, 175) };
    for (int side = 0; side < 2; ++side) {
        float bx = edges[side];
        havePrev = false;
        int seg = 0;
        for (float z = zMin; z <= zMax; z += step, ++seg) {
            float y = SampleTerrainHeight(bx, z) + 2.0f;
            ImVec2 pt;
            if (WorldToScreen(bx, y, z, displaySize, &pt)) {
                if (havePrev && (seg & 1) == 0) {
                    dl->AddLine(prevPt, pt, edgeCols[side], 2.0f);
                }
                prevPt = pt;
                havePrev = true;
            } else {
                havePrev = false;
            }
        }
    }

    // 3. Single clean 3D badge on the main border (no side badges that collide when zoomed out)
    float labelY = SampleTerrainHeight(0.0f, lz) + 24.0f;
    ImVec2 centerScreen;
    if (WorldToScreen(0.0f, labelY, lz, displaySize, &centerScreen)) {
        Draw3DTextBadge(dl, centerScreen,
                        Tr("<< ЗАПАДНАЯ ССР  |  ГРАНИЦА  |  ВОСТОЧНАЯ ССР >>",
                           "<< WEST SSR  |  BORDER  |  EAST SSR >>"),
                        IM_COL32(20, 22, 28, 220), IM_COL32(255, 200, 60, 230), IM_COL32(255, 235, 140, 255));
    }
}

static void DrawPartnerCursorAndPing(ImDrawList* dl, const ImVec2& displaySize)
{
    ULONGLONG now = GetTickCount64();

    // Draw local 3D Map Ping beacon (visible for 20 seconds)
    if (g_localPingActive && now - g_localPingSentMs < PING_LIFETIME_MS) {
        float ageSec = (float)(now - g_localPingSentMs) * 0.001f;
        int remSec = (int)ceilf((float)(PING_LIFETIME_MS - (now - g_localPingSentMs)) * 0.001f);
        float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
        float py = g_localPingY > 1.0f ? g_localPingY : SampleTerrainHeight(g_localPingX, g_localPingZ);
        ImVec2 basePt, beamTopPt;
        if (WorldToScreen(g_localPingX, py + 1.0f, g_localPingZ, displaySize, &basePt)) {
            dl->AddCircle(basePt, 12.0f + pulse * 14.0f, IM_COL32(95, 240, 125, 225), 32, 2.5f);
            dl->AddCircleFilled(basePt, 6.0f, IM_COL32(60, 225, 95, 245));
            if (WorldToScreen(g_localPingX, py + 55.0f, g_localPingZ, displaySize, &beamTopPt)) {
                dl->AddLine(basePt, beamTopPt, IM_COL32(95, 240, 125, 210), 3.0f);
                char pingText[128];
                _snprintf_s(pingText, sizeof(pingText), _TRUNCATE,
                            Tr("[ВАША МЕТКА] (X=%.0f, Z=%.0f) [%dс]", "[YOUR PING] (X=%.0f, Z=%.0f) [%ds]"),
                            g_localPingX, g_localPingZ, remSec);
                Draw3DTextBadge(dl, ImVec2(beamTopPt.x, beamTopPt.y - 14.0f), pingText,
                                IM_COL32(15, 42, 22, 230), IM_COL32(95, 240, 125, 250), IM_COL32(190, 255, 205, 255));
            }
        }
    }

    if (!LockstepNet_IsConnected()) return;
    PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
    if (!pc.valid) return;

    int partnerState = (TwoStates_LocalState() == 1) ? 2 : 1;
    TwoStates_SetSavedCameraForTest(partnerState, pc.camPos, pc.camLookAt);

    static unsigned s_lastPingSeq = 0;
    if (pc.hasPing && pc.pingSeq != s_lastPingSeq) {
        s_lastPingSeq = pc.pingSeq;
        char toast[200];
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("[СИГНАЛ] От партнёра на карте (X=%.0f м, Z=%.0f м) — нажмите F6 для перехода!",
                       "[PING] Map ping from partner (X=%.0f m, Z=%.0f m) — press F6 to jump!"),
                    pc.pingX, pc.pingZ);
        TwoStates_AddToast(toast, 2);
    }

    const char* partnerLabel = TwoStates_IsActive()
        ? (partnerState == 1 ? Tr("Курсор: Западная ССР (Хост)", "Cursor: West SSR (Host)")
                             : Tr("Курсор: Восточная ССР (Игрок 2)", "Cursor: East SSR (Player 2)"))
        : (LockstepNet_IsHost() ? Tr("Курсор: Игрок 2", "Cursor: Player 2") : Tr("Курсор: Хост", "Cursor: Host"));

    // Draw partner 3D cursor
    if (now - pc.lastUpdateMs < 10000 && (fabsf(pc.x) > 0.01f || fabsf(pc.z) > 0.01f)) {
        float wy = pc.y > 1.0f ? pc.y : SampleTerrainHeight(pc.x, pc.z);
        ImVec2 curPt, topPt;
        if (WorldToScreen(pc.x, wy + 1.5f, pc.z, displaySize, &curPt)) {
            dl->AddCircleFilled(curPt, 6.0f, IM_COL32(60, 215, 255, 230));
            dl->AddCircle(curPt, 13.0f, IM_COL32(60, 215, 255, 180), 24, 2.0f);
            if (WorldToScreen(pc.x, wy + 16.0f, pc.z, displaySize, &topPt)) {
                dl->AddLine(curPt, topPt, IM_COL32(60, 215, 255, 200), 2.0f);
                Draw3DTextBadge(dl, ImVec2(topPt.x, topPt.y - 12.0f), partnerLabel,
                                IM_COL32(15, 28, 38, 215), IM_COL32(60, 215, 255, 230), IM_COL32(180, 240, 255, 255));
            }
        }
    }

    // Draw active 3D Map Ping beacon (visible for 20 seconds)
    if (pc.hasPing && now - pc.pingReceivedMs < PING_LIFETIME_MS) {
        float ageSec = (float)(now - pc.pingReceivedMs) * 0.001f;
        int remSec = (int)ceilf((float)(PING_LIFETIME_MS - (now - pc.pingReceivedMs)) * 0.001f);
        float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
        float py = pc.pingY > 1.0f ? pc.pingY : SampleTerrainHeight(pc.pingX, pc.pingZ);
        ImVec2 basePt, beamTopPt;
        if (WorldToScreen(pc.pingX, py + 1.0f, pc.pingZ, displaySize, &basePt)) {
            dl->AddCircle(basePt, 14.0f + pulse * 18.0f, IM_COL32(255, 215, 50, 230), 32, 3.0f);
            dl->AddCircleFilled(basePt, 7.0f, IM_COL32(255, 90, 40, 245));
            if (WorldToScreen(pc.pingX, py + 65.0f, pc.pingZ, displaySize, &beamTopPt)) {
                dl->AddLine(basePt, beamTopPt, IM_COL32(255, 220, 60, 220), 3.5f);
                char pingText[128];
                _snprintf_s(pingText, sizeof(pingText), _TRUNCATE,
                            Tr("[СИГНАЛ ПАРТНЁРА] (X=%.0f, Z=%.0f) [%dс | F6]",
                               "[PARTNER PING] (X=%.0f, Z=%.0f) [%ds | F6]"),
                            pc.pingX, pc.pingZ, remSec);
                Draw3DTextBadge(dl, ImVec2(beamTopPt.x, beamTopPt.y - 14.0f), pingText,
                                IM_COL32(45, 30, 10, 235), IM_COL32(255, 210, 50, 255), IM_COL32(255, 240, 150, 255));
            }
        }
    }
}

static void GetToastColorsForKind(int colorKind, ImU32* outBorderCol, ImU32* outTextCol)
{
    switch (UiCommon_NormalizeToastColorKind(colorKind)) {
    case 1:
        *outBorderCol = IM_COL32(80, 220, 100, 235);
        *outTextCol   = IM_COL32(210, 255, 215, 255);
        break;
    case 2:
        *outBorderCol = IM_COL32(255, 205, 60, 240);
        *outTextCol   = IM_COL32(255, 240, 180, 255);
        break;
    case 3:
        *outBorderCol = IM_COL32(255, 80, 70, 245);
        *outTextCol   = IM_COL32(255, 205, 200, 255);
        break;
    case 4:
        *outBorderCol = IM_COL32(235, 190, 70, 240);
        *outTextCol   = IM_COL32(255, 242, 185, 255);
        break;
    case 5:
        *outBorderCol = IM_COL32(70, 215, 225, 240);
        *outTextCol   = IM_COL32(215, 250, 255, 255);
        break;
    default:
        *outBorderCol = IM_COL32(90, 180, 255, 230);
        *outTextCol   = IM_COL32(230, 245, 255, 255);
        break;
    }
}

static void DrawToastsOverlay(ImDrawList* dl, const ImVec2& displaySize)
{
    if (!dl) return;
    ToastMessage active[5] = {};
    int activeCount = 0;

    EnsureToastLock();
    EnterCriticalSection(&g_toastLock);
    const ULONGLONG now = GetTickCount64();
    while (!g_toasts.empty()) {
        const ToastMessage& front = g_toasts.front();
        if (now >= front.createdMs && (now - front.createdMs) > front.durationMs) {
            g_toasts.pop_front();
        } else {
            break;
        }
    }
    for (size_t i = 0; i < g_toasts.size() && activeCount < 5; ++i) {
        active[activeCount++] = g_toasts[i];
    }
    LeaveCriticalSection(&g_toastLock);

    float toastY = 58.0f;
    for (int i = 0; i < activeCount; ++i) {
        ImU32 borderCol = 0;
        ImU32 textCol = 0;
        GetToastColorsForKind(active[i].colorKind, &borderCol, &textCol);
        Draw3DTextBadge(dl, ImVec2(displaySize.x * 0.5f, toastY), active[i].text,
                        IM_COL32(18, 22, 28, 235), borderCol, textCol);
        toastY += 28.0f;
    }
}

void TwoStates_DrawOverlay()
{
    TwoStates_TickPendingCameraRestore();
    if (!ImGui::GetCurrentContext()) return;

    const bool inCoop = g_inSession || SaveSync_IsBusy() || SaveSync_IsWaitingLobbyActive();
    const ULONGLONG now = GetTickCount64();
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    if (inCoop) {
        bool worldLoaded = SaveSync_IsWorldLoaded();
        bool focused = IsGameWindowFocused();

        // Hotkeys when game window is focused:
        //   F7: toggle 3D state border lines (Two States mode only)
        //   F6: jump camera to partner / active ping
        //   Alt + LMB: send map ping at cursor
        static bool s_f7WasDown = false;
        static bool s_f6WasDown = false;
        static bool s_altClickWasDown = false;

        bool allowWorldHotkeys = focused && !SyncBlock_BlocksWorldInput();

        bool f7Down = allowWorldHotkeys && TwoStates_IsActive() && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7Down && !s_f7WasDown) {
            g_show3DBorder = !g_show3DBorder;
            TwoStates_SaveSettings();
            TwoStates_AddToast(g_show3DBorder
                ? Tr("3D-граница государств: ВКЛЮЧЕНА [F7]", "3D State Border: ON [F7]")
                : Tr("3D-граница государств: СКРЫТА [F7]", "3D State Border: OFF [F7]"), 0);
        }
        s_f7WasDown = f7Down;

        const bool inOrbitView = worldLoaded && IsInOrbitView();

        bool f6Down = allowWorldHotkeys && !inOrbitView && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6Down && !s_f6WasDown && worldLoaded) {
            TwoStates_JumpCameraToPartner();
        }
        s_f6WasDown = f6Down;

        bool altClickDown = allowWorldHotkeys && !inOrbitView && !ImGui::GetIO().WantCaptureMouse &&
                            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
                            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (altClickDown && !s_altClickWasDown && worldLoaded) {
            TwoStates_PingAtCursor();
        }
        s_altClickWasDown = altClickDown;

        // Periodically send local 3D cursor & camera lookAt to partner (~10 Hz)
        static ULONGLONG s_lastCursorSendMs = 0;
        if (worldLoaded && LockstepNet_IsConnected() && now - s_lastCursorSendMs >= 100) {
            s_lastCursorSendMs = now;
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            TwoStates_GetCursorWorldPos(&cx, &cy, &cz);
            TwoStates_GetCameraLookAt(&lx, &ly, &lz);
            float cpos[3] = { 0.0f, 0.0f, 0.0f };
            float clook[3] = { lx, ly, lz };
            TwoStates_GetCameraPos(&cpos[0], &cpos[1], &cpos[2]);
            LockstepNet_SendCursor(cx, cy, cz, cpos, clook, false);
        }

        ImDrawList* bg = ImGui::GetBackgroundDrawList();

        // Draw 3D border lines whenever world is loaded & Two States mode is active & g_show3DBorder is true
        if (worldLoaded && !inOrbitView && g_show3DBorder && TwoStates_IsActive()) {
            Draw3DBorderLines(bg, displaySize);
        }

        // Draw 3D partner cursor and map ping beacon (local + partner, 20s lifetime).
        // Minimap territory/border/ping overlay is rendered natively inside H_MinimapDrawContent (0x4BDE80)
        // and its toggle button inside H_MinimapDrawButtons (0x4BFF40), respecting HUD hide (G) and window Z-order.
        if (worldLoaded && !inOrbitView) {
            DrawPartnerCursorAndPing(bg, displaySize);
        }
    }

    // Draw Toast notifications even in SESSION_IDLE (Main Menu / Join Menu / post-disconnect).
    // Render to ForegroundDrawList so toasts remain visible above Lobby/Modal dimmers.
    DrawToastsOverlay(ImGui::GetForegroundDrawList(), displaySize);
}

// ---------------------------------------------------------------------------
// Overlay Hook Installer
// ---------------------------------------------------------------------------

int TwoStates_InstallOverlayHooks()
{
    if (g_engine) {
        g_collisionTerrainHeight = (CollisionTerrainHeightFn)GetProcAddress(
            g_engine, "?CollisionTerrainHeightOverPoint@C3D_TERRAIN@@QEAAMVC3DVECTOR3@@@Z");
    }
    int overlayOk = 0;
    overlayOk += InstallInlineHook(g_exeBase + RVA_MINIMAP_DRAW_CONTENT, (void*)&H_MinimapDrawContent,
                                   (void**)&o_minimapDrawContent, kMinimapDrawContentExpect,
                                   sizeof(kMinimapDrawContentExpect), "republicmp two_states minimap_draw_content");
    overlayOk += InstallInlineHook(g_exeBase + RVA_MINIMAP_DRAW_BUTTONS, (void*)&H_MinimapDrawButtons,
                                   (void**)&o_minimapDrawButtons, kMinimapDrawButtonsExpect,
                                   sizeof(kMinimapDrawButtonsExpect), "republicmp two_states minimap_draw_buttons");
    return overlayOk;
}
