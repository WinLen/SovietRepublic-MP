// SPDX-License-Identifier: GPL-3.0-only
// See two_states.h.
#include "two_states.h"
#include "save_sync.h"
#include "../game/save_load.h"
#include "../game/imgui_hook.h"
#include "../game/window_focus.h"
#include "../lockstep/commands.h"
#include "../lockstep/lockstep_net.h"
#include "../determinism/determinism.h"
#include "../ui/lang.h"
#include "imgui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <vector>
#include <deque>

#define WORLD_RVA 0x9D4F10
#define WORLD_USD_OFFSET 0x580
#define WORLD_RUB_OFFSET 0x588
#define CURSOR_X_OFFSET 0xF6C
#define CURSOR_Y_OFFSET 0xF70
#define CURSOR_Z_OFFSET 0xF74
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define VEC_VEHICLES_BEGIN_RVA (0x9D4F10 + 0x12810)
#define VEC_VEHICLES_END_RVA   (0x9D4F10 + 0x12818)
#define OBJ_POS_OFFSET 0x350
#define BLD_PROGRESS_OFFSET 0x604
#define BLD_PAY_RUB_OFFSET 0x608
#define BLD_PAY_USD_OFFSET 0x609
#define VEH_ID_OFFSET 0x04
#define VEH_HOME_OFFSET 0x4F8
#define VEH_DEPOT_OFFSET 0x600
#define VEH_POS_OFFSET 0xDE0

#define ENGINE_RVA 0x9941F0
#define ENGINE_TERRAIN_OFFSET 0xED8
#define CAMERA_RVA 0x9E9C90
#define CAMERA_POS_OFFSET 0x80
#define CAMERA_LOOKAT_OFFSET 0x98
#define CAMERA_VIEWPROJ_OFFSET 0x1F0

#define DLL_C3D "C3DDLL64.dll"

void TwoStates_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile int g_chosenMode = COOP_MODE_ONE_STATE;
static volatile int g_sessionMode = COOP_MODE_ONE_STATE;
static volatile bool g_inSession = false;
static volatile bool g_isHost = true;
static float g_borderHalfWidth = 250.0f; // [-250m .. +250m] = 500m shared border zone
static bool g_fullStartingCapital = true;
static volatile bool g_expandBorders = true; // unlock 2km restricted edge buffer by default
static bool g_show3DBorder = true;
static bool g_showMiniHud = true;
static bool g_miniHudHovered = false;

static double g_stateRub[2] = { 0.0, 0.0 };
static double g_stateUsd[2] = { 0.0, 0.0 };
static double g_netTradeRub[2] = { 0.0, 0.0 };
static double g_netTradeUsd[2] = { 0.0, 0.0 };
static bool g_treasuryInitialized = false;

static CRITICAL_SECTION g_ownerLock;
static bool g_ownerLockReady = false;
static std::unordered_map<int, int> g_buildingOwners; // buildingId -> 1 (West) or 2 (East)
static std::unordered_map<int, int> g_vehicleOwners;  // vehicleId  -> 1 (West) or 2 (East)

struct ToastMessage {
    char text[256];
    int colorKind; // 0=info(blue), 1=success(green), 2=warning(yellow), 3=error(red)
    ULONGLONG createdMs;
    ULONGLONG durationMs;
};
static std::deque<ToastMessage> g_toasts;

static void EnsureOwnerLock()
{
    if (!g_ownerLockReady) {
        InitializeCriticalSection(&g_ownerLock);
        g_ownerLockReady = true;
    }
}

void TwoStates_AddToast(const char* text, int colorKind)
{
    if (!text || !text[0]) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    ToastMessage tm = {};
    strncpy_s(tm.text, sizeof(tm.text), text, _TRUNCATE);
    tm.colorKind = colorKind;
    tm.createdMs = GetTickCount64();
    tm.durationMs = (colorKind >= 2) ? 5500 : 4500;
    g_toasts.push_back(tm);
    while (g_toasts.size() > 5) g_toasts.pop_front();
    LeaveCriticalSection(&g_ownerLock);
}

static double* WorldRubles()  { return (double*)(g_exeBase + WORLD_RVA + WORLD_RUB_OFFSET); }
static double* WorldDollars() { return (double*)(g_exeBase + WORLD_RVA + WORLD_USD_OFFSET); }

// ---------------------------------------------------------------------------
// C3D_TERRAIN boundary IAT hooks + Fair Economy Simulation Inline Hooks
// ---------------------------------------------------------------------------

typedef float (__fastcall* TerrainBoundaryFn)(BYTE* terrain);
typedef float (__fastcall* SampleHeightmapFn)(BYTE* terrain, float x, float z);
static TerrainBoundaryFn o_getBoundaryXMin = nullptr;
static TerrainBoundaryFn o_getBoundaryXMax = nullptr;
static TerrainBoundaryFn o_getBoundaryZMin = nullptr;
static TerrainBoundaryFn o_getBoundaryZMax = nullptr;
static SampleHeightmapFn g_sampleHeightmap = nullptr;

#define EXPANDED_BOUNDARY_MARGIN 150.0f

static void ApplyRuntimeTerrainBounds(BYTE* terrain)
{
    if (!terrain) return;
    __try {
        if (g_expandBorders) {
            if (*(float*)(terrain + 0x8DC) > EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8DC) = EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E0) > EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E0) = EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E4) < -EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E4) = -EXPANDED_BOUNDARY_MARGIN;
            if (*(float*)(terrain + 0x8E8) < -EXPANDED_BOUNDARY_MARGIN)
                *(float*)(terrain + 0x8E8) = -EXPANDED_BOUNDARY_MARGIN;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static float __fastcall H_GetTerrainBoundaryXMin(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryXMin ? o_getBoundaryXMin(terrain) : EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryXMax(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryXMax ? o_getBoundaryXMax(terrain) : -EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryZMin(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryZMin ? o_getBoundaryZMin(terrain) : EXPANDED_BOUNDARY_MARGIN;
}

static float __fastcall H_GetTerrainBoundaryZMax(BYTE* terrain)
{
    ApplyRuntimeTerrainBounds(terrain);
    return o_getBoundaryZMax ? o_getBoundaryZMax(terrain) : -EXPANDED_BOUNDARY_MARGIN;
}

// --- Fair Customs & Building Economy Attribution Hooks (SOVIET64.exe PE 6A3EB6AD) ---
// 1. 0x173320(world, building): building auto-purchase of resources & utility border trade
// 2. 0x1B7D50(world, building): customs / border trade (category 0x6B)
// 3. 0x1B9A40(world, building): customs / port vehicle loading & unloading export/import
// 4. 0x159070(world, building, float dt, unsigned char flag): construction resource/groundwork payment
// 5. 0x3E0730(world, vehicle, unsigned char a3, unsigned char a4): vehicle sale / border delivery

#define RVA_BLD_ECON        0x173320
#define RVA_CUSTOMS_BORDER  0x1B7D50
#define RVA_CUSTOMS_VEHICLE 0x1B9A40
#define RVA_CONSTRUCT_STEP  0x159070
#define RVA_VEHICLE_SALE    0x3E0730

static const BYTE kBldEconExpect[19] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48,
    0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kCustomsBorderExpect[18] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x48,
    0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kCustomsVehExpect[19] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48,
    0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const BYTE kConstructStepExpect[20] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0xF3, 0x0F, 0x11,
    0x50, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08
};
static const BYTE kVehicleSaleExpect[19] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44, 0x88, 0x40,
    0x18, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57
};

typedef __int64 (__fastcall* BldEconFn)(BYTE* world, BYTE* building);
typedef __int64 (__fastcall* ConstructStepFn)(BYTE* world, BYTE* building, float dt, unsigned char flag);
typedef __int64 (__fastcall* VehicleSaleFn)(BYTE* world, BYTE* vehicle, unsigned char a3, unsigned char a4);

static BldEconFn o_bldEcon = nullptr;
static BldEconFn o_customsBorder = nullptr;
static BldEconFn o_customsVehicle = nullptr;
static ConstructStepFn o_constructStep = nullptr;
static VehicleSaleFn o_vehicleSale = nullptr;

static volatile bool g_inSimStep = false;
static volatile DWORD g_simStepThread = 0;
static bool g_inCommand = false;
static thread_local bool t_inEconHook = false;
static double g_lastWorldRub = 0.0;
static double g_lastWorldUsd = 0.0;
static double g_unattributedRub = 0.0;
static double g_unattributedUsd = 0.0;

static bool BeginEconAttribution(BYTE* world, int owner)
{
    if (!g_inSimStep || g_inCommand || t_inEconHook || !TwoStates_IsActive()) return false;
    if (!Determinism_InStepFrame()) return false;
    if (world != g_exeBase + WORLD_RVA || GetCurrentThreadId() != g_simStepThread) return false;
    if (owner != 1 && owner != 2) return false;

    __try {
        double curRub = *WorldRubles();
        double curUsd = *WorldDollars();
        g_unattributedRub += (curRub - g_lastWorldRub);
        g_unattributedUsd += (curUsd - g_lastWorldUsd);
        *WorldRubles()  = g_stateRub[owner - 1];
        *WorldDollars() = g_stateUsd[owner - 1];
        t_inEconHook = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_inEconHook = false;
        return false;
    }
}

static void EndEconAttribution(int owner)
{
    if (!t_inEconHook) return;
    t_inEconHook = false;
    __try {
        int idx = owner - 1;
        double newRub = *WorldRubles();
        double newUsd = *WorldDollars();
        double dRub = newRub - g_stateRub[idx];
        double dUsd = newUsd - g_stateUsd[idx];
        g_stateRub[idx] = newRub;
        g_stateUsd[idx] = newUsd;
        g_netTradeRub[idx] += dRub;
        g_netTradeUsd[idx] += dUsd;
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1] + g_unattributedRub;
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1] + g_unattributedUsd;
        g_lastWorldRub = *WorldRubles();
        g_lastWorldUsd = *WorldDollars();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static int ResolveBuildingOwnerFast(const BYTE* b)
{
    if (!b) return 1;
    __try {
        int id = *(const int*)b;
        float x = *(const float*)(b + OBJ_POS_OFFSET);
        float z = *(const float*)(b + OBJ_POS_OFFSET + 8);
        return TwoStates_GetBuildingOwner(id, x, z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int ResolveVehicleOwnerFast(const BYTE* v)
{
    if (!v) return 1;
    __try {
        int id = *(const int*)(v + VEH_ID_OFFSET);
        return TwoStates_GetVehicleOwner(id, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static __int64 __fastcall H_BldEcon(BYTE* world, BYTE* building)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveBuildingOwnerFast(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = o_bldEcon(world, building);
    if (active) EndEconAttribution(owner);
    return r;
}

static __int64 __fastcall H_CustomsBorder(BYTE* world, BYTE* building)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveBuildingOwnerFast(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = o_customsBorder(world, building);
    if (active) EndEconAttribution(owner);
    return r;
}

static __int64 __fastcall H_CustomsVehicle(BYTE* world, BYTE* building)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveBuildingOwnerFast(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = o_customsVehicle(world, building);
    if (active) EndEconAttribution(owner);
    return r;
}

static __int64 __fastcall H_ConstructStep(BYTE* world, BYTE* building, float dt, unsigned char flag)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveBuildingOwnerFast(building) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = o_constructStep(world, building, dt, flag);
    if (active) EndEconAttribution(owner);
    return r;
}

static __int64 __fastcall H_VehicleSale(BYTE* world, BYTE* vehicle, unsigned char a3, unsigned char a4)
{
    int owner = (g_inSimStep && !t_inEconHook) ? ResolveVehicleOwnerFast(vehicle) : 0;
    bool active = owner ? BeginEconAttribution(world, owner) : false;
    __int64 r = o_vehicleSale(world, vehicle, a3, a4);
    if (active) EndEconAttribution(owner);
    return r;
}

bool TwoStates_Install()
{
    EnsureOwnerLock();
    if (g_engine) {
        g_sampleHeightmap = (SampleHeightmapFn)GetProcAddress(g_engine, "?SampleHeightmap@C3D_TERRAIN@@QEAAMMM@Z");
    }
    int ok = 0;
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryXMin@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryXMin, (void**)&o_getBoundaryXMin, "terrain BoundaryXMin");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryXMax@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryXMax, (void**)&o_getBoundaryXMax, "terrain BoundaryXMax");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryZMin@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryZMin, (void**)&o_getBoundaryZMin, "terrain BoundaryZMin");
    ok += PatchIat(g_exe, DLL_C3D, "?GetTerrainBoundaryZMax@C3D_TERRAIN@@QEAAMXZ",
                   (void*)&H_GetTerrainBoundaryZMax, (void**)&o_getBoundaryZMax, "terrain BoundaryZMax");

    int econOk = 0;
    econOk += InstallInlineHook(g_exeBase + RVA_BLD_ECON, (void*)&H_BldEcon, (void**)&o_bldEcon,
                                kBldEconExpect, sizeof(kBldEconExpect), "republicmp two_states bld_econ");
    econOk += InstallInlineHook(g_exeBase + RVA_CUSTOMS_BORDER, (void*)&H_CustomsBorder, (void**)&o_customsBorder,
                                kCustomsBorderExpect, sizeof(kCustomsBorderExpect), "republicmp two_states customs_border");
    econOk += InstallInlineHook(g_exeBase + RVA_CUSTOMS_VEHICLE, (void*)&H_CustomsVehicle, (void**)&o_customsVehicle,
                                kCustomsVehExpect, sizeof(kCustomsVehExpect), "republicmp two_states customs_vehicle");
    econOk += InstallInlineHook(g_exeBase + RVA_CONSTRUCT_STEP, (void*)&H_ConstructStep, (void**)&o_constructStep,
                                kConstructStepExpect, sizeof(kConstructStepExpect), "republicmp two_states construct_step");
    econOk += InstallInlineHook(g_exeBase + RVA_VEHICLE_SALE, (void*)&H_VehicleSale, (void**)&o_vehicleSale,
                                kVehicleSaleExpect, sizeof(kVehicleSaleExpect), "republicmp two_states vehicle_sale");

    Logf("republicmp  TWO_STATES: terrain boundary hooks (%d/4), economy attribution hooks (%d/5), heightmap=%p",
         ok, econOk, (void*)g_sampleHeightmap);
    return ok == 4;
}

// ---------------------------------------------------------------------------
// Configuration & Lifecycle
// ---------------------------------------------------------------------------

void TwoStates_SetMode(int mode)
{
    g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (!g_inSession) g_sessionMode = g_chosenMode;
}

int TwoStates_GetMode()
{
    return g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
}

bool TwoStates_IsActive()
{
    return g_inSession && g_sessionMode == COOP_MODE_TWO_STATES;
}

void TwoStates_SetBorderHalfWidth(float meters)
{
    if (meters < 50.0f) meters = 50.0f;
    if (meters > 2000.0f) meters = 2000.0f;
    g_borderHalfWidth = meters;
}

float TwoStates_GetBorderHalfWidth()
{
    return g_borderHalfWidth;
}

void TwoStates_SetFullStartingCapital(bool full)
{
    g_fullStartingCapital = full;
}

bool TwoStates_GetFullStartingCapital()
{
    return g_fullStartingCapital;
}

int TwoStates_LocalState()
{
    if (!g_inSession) return 1;
    return g_isHost ? 1 : 2;
}

const char* TwoStates_StateName(int state)
{
    if (state == 1) return Tr("Западная ССР (Хост)", "West SSR (Host)");
    if (state == 2) return Tr("Восточная ССР (Игрок 2)", "East SSR (Player 2)");
    return Tr("Общая пограничная зона", "Shared Border Zone");
}

void TwoStates_PrepareHostSave(bool isResync, double* outRub, double* outUsd)
{
    __try {
        double worldRub = *WorldRubles();
        double worldUsd = *WorldDollars();
        if (g_chosenMode == COOP_MODE_TWO_STATES) {
            if (!isResync || !g_treasuryInitialized) {
                if (g_fullStartingCapital) {
                    g_stateRub[0] = worldRub;
                    g_stateRub[1] = worldRub;
                    g_stateUsd[0] = worldUsd;
                    g_stateUsd[1] = worldUsd;
                    *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                    *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
                } else {
                    g_stateRub[0] = worldRub * 0.5;
                    g_stateRub[1] = worldRub - g_stateRub[0];
                    g_stateUsd[0] = worldUsd * 0.5;
                    g_stateUsd[1] = worldUsd - g_stateUsd[0];
                }
                g_treasuryInitialized = true;
            } else {
                // Re-sync: ensure the world object carries the exact sum before saving.
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            }
        } else {
            g_stateRub[0] = worldRub;
            g_stateRub[1] = 0.0;
            g_stateUsd[0] = worldUsd;
            g_stateUsd[1] = 0.0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (outRub) { outRub[0] = g_stateRub[0]; outRub[1] = g_stateRub[1]; }
    if (outUsd) { outUsd[0] = g_stateUsd[0]; outUsd[1] = g_stateUsd[1]; }
}

void TwoStates_OnSessionJoined(bool isHost, int mode, float borderHalfWidth, bool expandBorders,
                               const double stateRub[2], const double stateUsd[2])
{
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_buildingOwners.clear();
    g_vehicleOwners.clear();
    LeaveCriticalSection(&g_ownerLock);

    g_isHost = isHost;
    g_sessionMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (borderHalfWidth >= 50.0f && borderHalfWidth <= 2000.0f)
        g_borderHalfWidth = borderHalfWidth;
    g_expandBorders = expandBorders;
    g_stateRub[0] = stateRub[0];
    g_stateRub[1] = stateRub[1];
    g_stateUsd[0] = stateUsd[0];
    g_stateUsd[1] = stateUsd[1];
    g_netTradeRub[0] = g_netTradeRub[1] = 0.0;
    g_netTradeUsd[0] = g_netTradeUsd[1] = 0.0;
    g_treasuryInitialized = true;
    g_inSession = true;

    if (g_sessionMode == COOP_MODE_TWO_STATES) {
        __try {
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            Commands_RecordMoney();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Режим активен! Вы: %s | Граница X=0 (общая зона ±%.0f м)",
                      "[TWO STATES] Mode active! You: %s | Border X=0 (shared zone ±%.0f m)"),
                   TwoStates_StateName(TwoStates_LocalState()), g_borderHalfWidth);
        SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Казна Запада: %.0f ₽ / %.0f $ | Казна Востока: %.0f ₽ / %.0f $",
                      "[TWO STATES] West Treasury: %.0f RUB / %.0f USD | East Treasury: %.0f RUB / %.0f USD"),
                   g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
        char toast[220];
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("Режим «Два государства»: Вы — %s (F7: 3D-граница, F6: к другу, Alt+ЛКМ: сигнал)",
                       "Two States Mode: You are %s (F7: 3D border, F6: jump to partner, Alt+LMB: ping)"),
                    TwoStates_StateName(TwoStates_LocalState()));
        TwoStates_AddToast(toast, 1);
    } else {
        SaveStatus(Tr("[ОДНО ГОСУДАРСТВО] Совместное управление единой республикой (общая казна)",
                      "[ONE STATE] Co-op management of a single republic (shared treasury)"));
        TwoStates_AddToast(Tr("Режим «Одно государство»: общая казна и территория (F6: к другу, Alt+ЛКМ: сигнал)",
                              "One State Mode: shared treasury & territory (F6: jump to partner, Alt+LMB: ping)"), 1);
    }
}

void TwoStates_OnWorldLoaded()
{
    if (!TwoStates_IsActive()) return;
    __try {
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        Commands_RecordMoney();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_OnSessionEnded()
{
    g_inSession = false;
    g_inSimStep = false;
    g_treasuryInitialized = false;
}

// ---------------------------------------------------------------------------
// Treasury & Deterministic Command / Step Hooks
// ---------------------------------------------------------------------------

double TwoStates_GetRubles(int state)
{
    if (!TwoStates_IsActive()) {
        __try { return *WorldRubles(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0; }
    }
    if (state == 1) return g_stateRub[0];
    if (state == 2) return g_stateRub[1];
    return g_stateRub[0] + g_stateRub[1];
}

double TwoStates_GetDollars(int state)
{
    if (!TwoStates_IsActive()) {
        __try { return *WorldDollars(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0; }
    }
    if (state == 1) return g_stateUsd[0];
    if (state == 2) return g_stateUsd[1];
    return g_stateUsd[0] + g_stateUsd[1];
}

double TwoStates_LocalRubles()
{
    return TwoStates_GetRubles(TwoStates_LocalState());
}

double TwoStates_LocalDollars()
{
    return TwoStates_GetDollars(TwoStates_LocalState());
}

double TwoStates_GetNetTradeRub(int state)
{
    if (state == 1) return g_netTradeRub[0];
    if (state == 2) return g_netTradeRub[1];
    return g_netTradeRub[0] + g_netTradeRub[1];
}

double TwoStates_GetNetTradeUsd(int state)
{
    if (state == 1) return g_netTradeUsd[0];
    if (state == 2) return g_netTradeUsd[1];
    return g_netTradeUsd[0] + g_netTradeUsd[1];
}

static int g_cmdOwner = 1;
static int g_cmdBuildingsBefore = 0;
static int g_cmdVehiclesBefore = 0;

static int GetWorldBuildingCount()
{
    __try {
        BYTE** b = *(BYTE***)(g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        BYTE** e = *(BYTE***)(g_exeBase + VEC_BUILDINGS_END_RVA);
        return (b && e >= b) ? (int)(e - b) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int GetWorldVehicleCount()
{
    __try {
        BYTE** b = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
        BYTE** e = *(BYTE***)(g_exeBase + VEC_VEHICLES_END_RVA);
        return (b && e >= b) ? (int)(e - b) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void TwoStates_BeginCommand(int ownerState)
{
    g_cmdOwner = (ownerState == 2) ? 2 : 1;
    g_cmdBuildingsBefore = GetWorldBuildingCount();
    g_cmdVehiclesBefore  = GetWorldVehicleCount();
    if (!TwoStates_IsActive()) {
        g_inCommand = false;
        return;
    }
    g_inCommand = true;
    __try {
        *WorldRubles()  = g_stateRub[g_cmdOwner - 1];
        *WorldDollars() = g_stateUsd[g_cmdOwner - 1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_EndCommand(int ownerState)
{
    int owner = (ownerState == 2) ? 2 : 1;
    // Register any new buildings placed during this command to `owner`.
    int bldAfter = GetWorldBuildingCount();
    if (bldAfter > g_cmdBuildingsBefore) {
        __try {
            BYTE** b = *(BYTE***)(g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
            for (int i = g_cmdBuildingsBefore; b && i < bldAfter; i++) {
                if (b[i]) TwoStates_RegisterBuildingOwner(*(int*)b[i], owner);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // Register any new vehicles bought/spawned during this command to `owner`.
    int vehAfter = GetWorldVehicleCount();
    if (vehAfter > g_cmdVehiclesBefore) {
        __try {
            BYTE** v = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
            for (int i = g_cmdVehiclesBefore; v && i < vehAfter; i++) {
                if (v[i]) TwoStates_RegisterVehicleOwner(*(int*)(v[i] + VEH_ID_OFFSET), owner);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!g_inCommand || !TwoStates_IsActive()) {
        g_inCommand = false;
        return;
    }
    g_inCommand = false;
    __try {
        g_stateRub[owner - 1] = *WorldRubles();
        g_stateUsd[owner - 1] = *WorldDollars();
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool TwoStates_ApplyTransferCommand(const LsStateTransfer& t)
{
    if (!TwoStates_IsActive()) return false;
    if ((t.fromState != 1 && t.fromState != 2) || (t.toState != 1 && t.toState != 2) || t.fromState == t.toState)
        return false;
    if (!isfinite(t.rubles) || !isfinite(t.dollars) || t.rubles < 0.0 || t.dollars < 0.0)
        return false;
    if (t.rubles == 0.0 && t.dollars == 0.0)
        return false;

    int src = t.fromState - 1;
    int dst = t.toState - 1;
    double sendRub = t.rubles <= g_stateRub[src] ? t.rubles : g_stateRub[src];
    double sendUsd = t.dollars <= g_stateUsd[src] ? t.dollars : g_stateUsd[src];
    if (sendRub <= 0.0 && sendUsd <= 0.0) return false;

    g_stateRub[src] -= sendRub;
    g_stateRub[dst] += sendRub;
    g_stateUsd[src] -= sendUsd;
    g_stateUsd[dst] += sendUsd;

    if (g_inCommand) {
        __try {
            *WorldRubles()  = g_stateRub[g_cmdOwner - 1];
            *WorldDollars() = g_stateUsd[g_cmdOwner - 1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Перевод из %s в %s: %.0f ₽, %.0f $",
                  "[TWO STATES] Transfer from %s to %s: %.0f RUB, %.0f USD"),
               TwoStates_StateName(t.fromState), TwoStates_StateName(t.toState), sendRub, sendUsd);
    char toast[200];
    _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                Tr("💸 Перевод из %s в %s: %.0f ₽ / %.0f $",
                   "💸 Transfer from %s to %s: %.0f RUB / %.0f USD"),
                TwoStates_StateName(t.fromState), TwoStates_StateName(t.toState), sendRub, sendUsd);
    TwoStates_AddToast(toast, 1);
    return true;
}

void TwoStates_RequestTransfer(double rubles, double dollars)
{
    if (!TwoStates_IsActive()) return;
    if (rubles < 0.0) rubles = 0.0;
    if (dollars < 0.0) dollars = 0.0;
    if (rubles == 0.0 && dollars == 0.0) return;

    int me = TwoStates_LocalState();
    int other = (me == 1) ? 2 : 1;
    LsStateTransfer t = { me, other, rubles, dollars };
    if (Commands_Capturing()) {
        Commands_Submit(26 /* LS_CMD_STATE_TRANSFER */, &t, sizeof(t));
    } else {
        TwoStates_ApplyTransferCommand(t);
    }
}

static double g_stepRubWeight[2] = { 0.0, 0.0 };
static double g_stepUsdWeight[2] = { 0.0, 0.0 };
static int g_stepBldCount[2] = { 0, 0 };

void TwoStates_BeforeStep(BYTE* world)
{
    if (!TwoStates_IsActive() || !world) return;
    g_stepRubWeight[0] = g_stepRubWeight[1] = 0.0;
    g_stepUsdWeight[0] = g_stepUsdWeight[1] = 0.0;
    g_stepBldCount[0] = g_stepBldCount[1] = 0;

    __try {
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        g_lastWorldRub = *WorldRubles();
        g_lastWorldUsd = *WorldDollars();
        g_unattributedRub = 0.0;
        g_unattributedUsd = 0.0;
        g_simStepThread = GetCurrentThreadId();
        g_inSimStep = true;

        BYTE** it = *(BYTE***)(g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        BYTE** end = *(BYTE***)(g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it) {
            BYTE* b = *it;
            if (!b) continue;
            float x = *(float*)(b + OBJ_POS_OFFSET);
            float z = *(float*)(b + OBJ_POS_OFFSET + 8);
            int owner = TwoStates_GetBuildingOwner(*(int*)b, x, z);
            int idx = owner - 1;
            g_stepBldCount[idx]++;
            float prog = *(float*)(b + BLD_PROGRESS_OFFSET);
            if (prog < 1.0f) {
                if (b[BLD_PAY_RUB_OFFSET]) {
                    if (g_stateRub[idx] <= 0.0) b[BLD_PAY_RUB_OFFSET] = 0;
                    else g_stepRubWeight[idx] += 1.0;
                }
                if (b[BLD_PAY_USD_OFFSET]) {
                    if (g_stateUsd[idx] <= 0.0) b[BLD_PAY_USD_OFFSET] = 0;
                    else g_stepUsdWeight[idx] += 1.0;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_AfterStep(BYTE* world)
{
    if (!TwoStates_IsActive() || !world) {
        g_inSimStep = false;
        return;
    }
    g_inSimStep = false;
    __try {
        double nowRub = *WorldRubles();
        double nowUsd = *WorldDollars();
        double dRub = g_unattributedRub + (nowRub - g_lastWorldRub);
        double dUsd = g_unattributedUsd + (nowUsd - g_lastWorldUsd);
        g_unattributedRub = 0.0;
        g_unattributedUsd = 0.0;

        if (fabs(dRub) > 1e-6) {
            double w0 = 0.5;
            double wSum = g_stepRubWeight[0] + g_stepRubWeight[1];
            if (wSum > 0.0) {
                w0 = g_stepRubWeight[0] / wSum;
            } else if (g_stepBldCount[0] + g_stepBldCount[1] > 0) {
                w0 = (double)g_stepBldCount[0] / (double)(g_stepBldCount[0] + g_stepBldCount[1]);
            }
            double d0 = dRub * w0;
            g_stateRub[0] += d0;
            g_stateRub[1] += (dRub - d0);
            g_netTradeRub[0] += d0;
            g_netTradeRub[1] += (dRub - d0);
        }
        if (fabs(dUsd) > 1e-6) {
            double w0 = 0.5;
            double wSum = g_stepUsdWeight[0] + g_stepUsdWeight[1];
            if (wSum > 0.0) {
                w0 = g_stepUsdWeight[0] / wSum;
            } else if (g_stepBldCount[0] + g_stepBldCount[1] > 0) {
                w0 = (double)g_stepBldCount[0] / (double)(g_stepBldCount[0] + g_stepBldCount[1]);
            }
            double d0 = dUsd * w0;
            g_stateUsd[0] += d0;
            g_stateUsd[1] += (dUsd - d0);
            g_netTradeUsd[0] += d0;
            g_netTradeUsd[1] += (dUsd - d0);
        }
        *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
        *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool g_uiMoneySwapped = false;
static double g_uiSavedSimRub = 0.0;
static double g_uiSavedSimUsd = 0.0;
static double g_uiShownRub = 0.0;
static double g_uiShownUsd = 0.0;

void TwoStates_UiBegin()
{
    if (!TwoStates_IsActive() || g_inSimStep) {
        g_uiMoneySwapped = false;
        return;
    }
    __try {
        g_uiSavedSimRub = *WorldRubles();
        g_uiSavedSimUsd = *WorldDollars();
        g_uiShownRub = TwoStates_LocalRubles();
        g_uiShownUsd = TwoStates_LocalDollars();
        *WorldRubles()  = g_uiShownRub;
        *WorldDollars() = g_uiShownUsd;
        g_uiMoneySwapped = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_uiMoneySwapped = false;
    }
}

void TwoStates_UiEnd()
{
    if (!g_uiMoneySwapped) return;
    g_uiMoneySwapped = false;
    __try {
        double dRub = *WorldRubles() - g_uiShownRub;
        double dUsd = *WorldDollars() - g_uiShownUsd;
        *WorldRubles()  = g_uiSavedSimRub + dRub;
        *WorldDollars() = g_uiSavedSimUsd + dUsd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_ToolMoneyBegin(double* outSavedSimRub, double* outSavedSimUsd)
{
    if (!TwoStates_IsActive()) return;
    __try {
        *outSavedSimRub = *WorldRubles();
        *outSavedSimUsd = *WorldDollars();
        *WorldRubles()  = TwoStates_LocalRubles();
        *WorldDollars() = TwoStates_LocalDollars();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_ToolMoneyEnd(double savedSimRub, double savedSimUsd)
{
    if (!TwoStates_IsActive()) return;
    __try {
        *WorldRubles()  = savedSimRub;
        *WorldDollars() = savedSimUsd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---------------------------------------------------------------------------
// Territory, Building & Vehicle Ownership
// ---------------------------------------------------------------------------

void TwoStates_RegisterBuildingOwner(int buildingId, int ownerState)
{
    if (buildingId < 0 || (ownerState != 1 && ownerState != 2)) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_buildingOwners[buildingId] = ownerState;
    LeaveCriticalSection(&g_ownerLock);
}

int TwoStates_GetBuildingOwner(int buildingId, float x, float z)
{
    (void)z;
    if (buildingId >= 0) {
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        auto it = g_buildingOwners.find(buildingId);
        int found = (it != g_buildingOwners.end()) ? it->second : 0;
        LeaveCriticalSection(&g_ownerLock);
        if (found == 1 || found == 2) return found;
    }
    return (x < 0.0f) ? 1 : 2;
}

void TwoStates_RegisterVehicleOwner(int vehicleId, int ownerState)
{
    if (vehicleId < 0 || (ownerState != 1 && ownerState != 2)) return;
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_vehicleOwners[vehicleId] = ownerState;
    LeaveCriticalSection(&g_ownerLock);
}

int TwoStates_GetVehicleOwner(int vehicleId, const BYTE* vehiclePtr)
{
    if (vehicleId >= 0) {
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        auto it = g_vehicleOwners.find(vehicleId);
        int found = (it != g_vehicleOwners.end()) ? it->second : 0;
        LeaveCriticalSection(&g_ownerLock);
        if (found == 1 || found == 2) return found;
    }
    if (vehiclePtr) {
        __try {
            const BYTE* home = *(const BYTE* const*)(vehiclePtr + VEH_HOME_OFFSET);
            if (!home) home = *(const BYTE* const*)(vehiclePtr + VEH_DEPOT_OFFSET);
            if (home) {
                int bldId = *(const int*)home;
                float bx = *(const float*)(home + OBJ_POS_OFFSET);
                float bz = *(const float*)(home + OBJ_POS_OFFSET + 8);
                return TwoStates_GetBuildingOwner(bldId, bx, bz);
            }
            float vx = *(const float*)(vehiclePtr + VEH_POS_OFFSET);
            return (vx < 0.0f) ? 1 : 2;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 1;
}

int TwoStates_GetZoneAt(float x, float z)
{
    (void)z;
    if (x < -g_borderHalfWidth) return 1; // West Republic
    if (x >  g_borderHalfWidth) return 2; // East Republic
    return 0;                             // Shared Border Zone
}

bool TwoStates_CanBuildAt(float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int zone = TwoStates_GetZoneAt(x, z);
    // Each state can build on its own side AND inside the shared border strip (zone == 0).
    return zone == 0 || zone == s;
}

bool TwoStates_CanDemolishAt(float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int zone = TwoStates_GetZoneAt(x, z);
    return zone == 0 || zone == s;
}

bool TwoStates_CanControlBuilding(int buildingId, float x, float z, int state)
{
    if (!TwoStates_IsActive()) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int owner = TwoStates_GetBuildingOwner(buildingId, x, z);
    return owner == s;
}

bool TwoStates_CanControlVehicle(const BYTE* vehiclePtr, int state)
{
    if (!TwoStates_IsActive() || !vehiclePtr) return true;
    int s = (state == 1 || state == 2) ? state : TwoStates_LocalState();
    int id = -1;
    __try { id = *(const int*)(vehiclePtr + VEH_ID_OFFSET); } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    int owner = TwoStates_GetVehicleOwner(id, vehiclePtr);
    return owner == s;
}

static ULONGLONG g_lastRefuseLogMs = 0;

static void NotifyTerritoryRefused(const char* actionDesc, float x)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastRefuseLogMs < 1500) return;
    g_lastRefuseLogMs = now;
    int other = (TwoStates_LocalState() == 1) ? 2 : 1;
    const char* act = actionDesc ? actionDesc : Tr("действие", "action");
    SaveStatus(Tr("[ДВА ГОСУДАРСТВА] Запрещено (%s на X=%.0f): это территория/собственность «%s»!",
                  "[TWO STATES] Forbidden (%s at X=%.0f): territory/property of '%s'!"),
               act, x, TwoStates_StateName(other));
    char toast[220];
    _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                Tr("⛔ Запрещено (%s): собственность «%s»!",
                   "⛔ Forbidden (%s): property of '%s'!"),
                act, TwoStates_StateName(other));
    TwoStates_AddToast(toast, 3);
}

bool TwoStates_CheckBuildAllowed(float x, float z, const char* actionDesc)
{
    if (TwoStates_CanBuildAt(x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckDemolishAllowed(float x, float z, const char* actionDesc)
{
    if (TwoStates_CanDemolishAt(x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckBuildingControlAllowed(int buildingId, float x, float z, const char* actionDesc)
{
    if (TwoStates_CanControlBuilding(buildingId, x, z)) return true;
    NotifyTerritoryRefused(actionDesc, x);
    return false;
}

bool TwoStates_CheckVehicleControlAllowed(const BYTE* vehiclePtr, const char* actionDesc)
{
    if (TwoStates_CanControlVehicle(vehiclePtr)) return true;
    float vx = 0.0f;
    __try { if (vehiclePtr) vx = *(const float*)(vehiclePtr + VEH_POS_OFFSET); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    NotifyTerritoryRefused(actionDesc, vx);
    return false;
}

bool TwoStates_GetCursorWorldPos(float* outX, float* outY, float* outZ)
{
    __try {
        BYTE* world = g_exeBase + WORLD_RVA;
        if (outX) *outX = *(float*)(world + CURSOR_X_OFFSET);
        if (outY) *outY = *(float*)(world + CURSOR_Y_OFFSET);
        if (outZ) *outZ = *(float*)(world + CURSOR_Z_OFFSET);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TwoStates_GetCameraLookAt(float* outX, float* outY, float* outZ)
{
    __try {
        const float* look = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_LOOKAT_OFFSET);
        if (outX) *outX = look[0];
        if (outY) *outY = look[1];
        if (outZ) *outZ = look[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static float SampleTerrainHeight(float wx, float wz)
{
    if (!g_sampleHeightmap) return 20.0f;
    __try {
        BYTE* engine = *(BYTE**)(g_exeBase + ENGINE_RVA);
        BYTE* terrain = engine ? *(BYTE**)(engine + ENGINE_TERRAIN_OFFSET) : nullptr;
        if (!terrain) return 20.0f;
        float h = g_sampleHeightmap(terrain, wx, wz);
        return isfinite(h) ? h : 20.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 20.0f;
    }
}

bool TwoStates_JumpCameraTo(float wx, float wz)
{
    if (!SaveSync_IsWorldLoaded() || !isfinite(wx) || !isfinite(wz)) return false;
    __try {
        float* camPos    = (float*)(g_exeBase + CAMERA_RVA + CAMERA_POS_OFFSET);
        float* camLookAt = (float*)(g_exeBase + CAMERA_RVA + CAMERA_LOOKAT_OFFSET);
        float dx = wx - camLookAt[0];
        float dz = wz - camLookAt[2];
        float heightOffset = camPos[1] - camLookAt[1];
        if (heightOffset < 35.0f || heightOffset > 1500.0f || !isfinite(heightOffset))
            heightOffset = 140.0f;
        float groundY = SampleTerrainHeight(wx, wz);
        camLookAt[0] = wx;
        camLookAt[1] = groundY;
        camLookAt[2] = wz;
        camPos[0] += dx;
        camPos[1]  = groundY + heightOffset;
        camPos[2] += dz;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TwoStates_JumpCameraToPartner()
{
    PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
    if (!pc.valid) {
        TwoStates_AddToast(Tr("Партнёр ещё не подключён в режиме Lockstep",
                              "Partner is not connected in Lockstep mode yet"), 2);
        return false;
    }
    ULONGLONG now = GetTickCount64();
    float tx = pc.x, tz = pc.z;
    if (pc.hasPing && now - pc.pingReceivedMs < 15000) {
        tx = pc.pingX;
        tz = pc.pingZ;
    } else if (fabsf(tx) < 0.01f && fabsf(tz) < 0.01f) {
        tx = pc.camX;
        tz = pc.camZ;
    }
    if (TwoStates_JumpCameraTo(tx, tz)) {
        char msg[160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    Tr("📷 Камера перемещена к партнёру (X=%.0f м, Z=%.0f м)",
                       "📷 Camera jumped to partner (X=%.0f m, Z=%.0f m)"),
                    tx, tz);
        TwoStates_AddToast(msg, 0);
        return true;
    }
    return false;
}

void TwoStates_SetShow3DBorder(bool on) { g_show3DBorder = on; }
bool TwoStates_GetShow3DBorder()         { return g_show3DBorder; }
void TwoStates_SetShowMiniHud(bool on)  { g_showMiniHud = on; }
bool TwoStates_GetShowMiniHud()          { return g_showMiniHud; }
bool TwoStates_MiniHudWantsMouse()       { return g_miniHudHovered; }

void TwoStates_PingAtCursor()
{
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    if (!TwoStates_GetCursorWorldPos(&cx, &cy, &cz)) return;
    TwoStates_GetCameraLookAt(&lx, &ly, &lz);
    if (fabsf(cx) < 0.01f && fabsf(cz) < 0.01f) {
        cx = lx; cy = ly; cz = lz;
    }
    if (LockstepNet_IsConnected()) {
        LockstepNet_SendCursor(cx, cy, cz, lx, lz, true);
    }
    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                Tr("📍 Сигнал отправлен на карту (X=%.0f м, Z=%.0f м)",
                   "📍 Ping sent on map (X=%.0f m, Z=%.0f m)"),
                cx, cz);
    TwoStates_AddToast(msg, 1);
    SaveStatus(Tr("[КАРТА] Отправлен сигнал партнёру: X=%.0f м, Z=%.0f м",
                  "[MAP] Ping sent to partner: X=%.0f m, Z=%.0f m"), cx, cz);
}

static bool WorldToScreen(float wx, float wy, float wz, const ImVec2& displaySize, ImVec2* outScreen)
{
    __try {
        const float* m = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_VIEWPROJ_OFFSET);
        float clipX = wx * m[0] + wy * m[4] + wz * m[8]  + m[12];
        float clipY = wx * m[1] + wy * m[5] + wz * m[9]  + m[13];
        float clipW = wx * m[3] + wy * m[7] + wz * m[11] + m[15];
        if (clipW <= 0.5f || !isfinite(clipW)) return false;
        float invW = 1.0f / clipW;
        float ndcX = clipX * invW;
        float ndcY = clipY * invW;
        if (ndcX < -1.6f || ndcX > 1.6f || ndcY < -1.6f || ndcY > 1.6f) return false;
        outScreen->x = (ndcX * 0.5f + 0.5f) * displaySize.x;
        outScreen->y = (0.5f - ndcY * 0.5f) * displaySize.y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

    // 3. Floating 3D labels near camera lookAt Z
    float labelY = SampleTerrainHeight(0.0f, lz) + 24.0f;
    ImVec2 centerScreen;
    if (WorldToScreen(0.0f, labelY, lz, displaySize, &centerScreen)) {
        Draw3DTextBadge(dl, centerScreen,
                        Tr("◄ ЗАПАДНАЯ ССР (Хост)  |  ГОСУДАРСТВЕННАЯ ГРАНИЦА (X=0)  |  ВОСТОЧНАЯ ССР (Игрок 2) ►",
                           "◄ WEST SSR (Host)  |  STATE BORDER (X=0)  |  EAST SSR (Player 2) ►"),
                        IM_COL32(20, 22, 28, 220), IM_COL32(255, 200, 60, 230), IM_COL32(255, 235, 140, 255));
    }
    ImVec2 westEdgeScreen, eastEdgeScreen;
    if (WorldToScreen(-g_borderHalfWidth, SampleTerrainHeight(-g_borderHalfWidth, lz) + 14.0f, lz, displaySize, &westEdgeScreen)) {
        char buf[96];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, Tr("Пограничная зона (-%.0f м)", "Border Zone (-%.0f m)"), g_borderHalfWidth);
        Draw3DTextBadge(dl, westEdgeScreen, buf, IM_COL32(18, 28, 40, 195), IM_COL32(80, 190, 255, 200), IM_COL32(160, 225, 255, 255));
    }
    if (WorldToScreen(g_borderHalfWidth, SampleTerrainHeight(g_borderHalfWidth, lz) + 14.0f, lz, displaySize, &eastEdgeScreen)) {
        char buf[96];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, Tr("Пограничная зона (+%.0f м)", "Border Zone (+%.0f m)"), g_borderHalfWidth);
        Draw3DTextBadge(dl, eastEdgeScreen, buf, IM_COL32(40, 26, 16, 195), IM_COL32(255, 170, 70, 200), IM_COL32(255, 215, 150, 255));
    }
}

static void DrawPartnerCursorAndPing(ImDrawList* dl, const ImVec2& displaySize)
{
    if (!LockstepNet_IsConnected()) return;
    PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
    if (!pc.valid) return;

    ULONGLONG now = GetTickCount64();
    static unsigned s_lastPingSeq = 0;
    if (pc.hasPing && pc.pingSeq != s_lastPingSeq) {
        s_lastPingSeq = pc.pingSeq;
        char toast[200];
        _snprintf_s(toast, sizeof(toast), _TRUNCATE,
                    Tr("📍 Сигнал от партнёра на карте (X=%.0f м, Z=%.0f м) — нажмите F6 для перехода!",
                       "📍 Map ping from partner (X=%.0f m, Z=%.0f m) — press F6 to jump!"),
                    pc.pingX, pc.pingZ);
        TwoStates_AddToast(toast, 2);
    }

    int partnerState = (TwoStates_LocalState() == 1) ? 2 : 1;
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

    // Draw active 3D Map Ping beacon (visible for 10 seconds)
    if (pc.hasPing && now - pc.pingReceivedMs < 10000) {
        float ageSec = (float)(now - pc.pingReceivedMs) * 0.001f;
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
                            Tr("📍 СИГНАЛ ПАРТНЁРА (X=%.0f, Z=%.0f) [F6]", "📍 PARTNER PING (X=%.0f, Z=%.0f) [F6]"),
                            pc.pingX, pc.pingZ);
                Draw3DTextBadge(dl, ImVec2(beamTopPt.x, beamTopPt.y - 14.0f), pingText,
                                IM_COL32(45, 30, 10, 235), IM_COL32(255, 210, 50, 255), IM_COL32(255, 240, 150, 255));
            }
        }
    }
}

void TwoStates_DrawOverlay()
{
    g_miniHudHovered = false;
    bool worldLoaded = SaveSync_IsWorldLoaded();
    bool focused = IsGameWindowFocused();

    // Hotkeys when game window is focused:
    //   F7: toggle 3D state border lines
    //   F6: jump camera to partner / active ping
    //   Alt + LMB: send map ping at cursor
    static bool s_f7WasDown = false;
    static bool s_f6WasDown = false;
    static bool s_altClickWasDown = false;

    bool f7Down = focused && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7Down && !s_f7WasDown) {
        g_show3DBorder = !g_show3DBorder;
        TwoStates_AddToast(g_show3DBorder
            ? Tr("3D-граница государств: ВКЛЮЧЕНА [F7]", "3D State Border: ON [F7]")
            : Tr("3D-граница государств: СКРЫТА [F7]", "3D State Border: OFF [F7]"), 0);
    }
    s_f7WasDown = f7Down;

    bool f6Down = focused && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6Down && !s_f6WasDown && worldLoaded) {
        TwoStates_JumpCameraToPartner();
    }
    s_f6WasDown = f6Down;

    bool altClickDown = focused && !ImGui::GetIO().WantCaptureMouse &&
                        (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
                        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (altClickDown && !s_altClickWasDown && worldLoaded) {
        TwoStates_PingAtCursor();
    }
    s_altClickWasDown = altClickDown;

    // Periodically send local 3D cursor & camera lookAt to partner (~10 Hz)
    static ULONGLONG s_lastCursorSendMs = 0;
    ULONGLONG now = GetTickCount64();
    if (worldLoaded && LockstepNet_IsConnected() && now - s_lastCursorSendMs >= 100) {
        s_lastCursorSendMs = now;
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        float lx = 0.0f, ly = 0.0f, lz = 0.0f;
        TwoStates_GetCursorWorldPos(&cx, &cy, &cz);
        TwoStates_GetCameraLookAt(&lx, &ly, &lz);
        LockstepNet_SendCursor(cx, cy, cz, lx, lz, false);
    }

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // Draw 3D border lines whenever world is loaded & Two States mode is selected/active & g_show3DBorder is true
    if (worldLoaded && g_show3DBorder && (TwoStates_IsActive() || TwoStates_GetMode() == COOP_MODE_TWO_STATES)) {
        Draw3DBorderLines(bg, displaySize);
    }

    // Draw 3D partner cursor and map ping beacon
    if (worldLoaded && LockstepNet_IsConnected()) {
        DrawPartnerCursorAndPing(bg, displaySize);
    }

    // Draw Compact Top-Center Mini-HUD when in session or when Two States mode is active
    SessionState st = SaveSync_GetState();
    bool inActiveGame = (st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME);
    if (g_showMiniHud && (inActiveGame || TwoStates_IsActive())) {
        ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, 8.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.10f, 0.13f, 0.88f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 5.0f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##RepublicMP_MiniHUD", nullptr, flags)) {
            if (TwoStates_IsActive()) {
                int me = TwoStates_LocalState();
                ImGui::TextColored(me == 1 ? ImVec4(0.4f, 0.85f, 1.0f, 1.0f) : ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                                   "%s", me == 1 ? Tr("★ Западная ССР", "★ West SSR") : Tr("★ Восточная ССР", "★ East SSR"));
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.45f, 1.0f),
                                   Tr("Ваша казна: %.0f ₽ / %.0f $", "Your Treasury: %.0f RUB / %.0f USD"),
                                   TwoStates_LocalRubles(), TwoStates_LocalDollars());
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                int other = (me == 1) ? 2 : 1;
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                                   Tr("%s: %.0f ₽ / %.0f $", "%s: %.0f RUB / %.0f USD"),
                                   other == 1 ? Tr("Запад", "West") : Tr("Восток", "East"),
                                   TwoStates_GetRubles(other), TwoStates_GetDollars(other));

                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                if (TwoStates_GetCursorWorldPos(&cx, &cy, &cz)) {
                    int zone = TwoStates_GetZoneAt(cx, cz);
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    if (zone == 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), Tr("Погранзона", "Border Zone"));
                    else if (zone == me)
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), Tr("Своя земля", "Own Land"));
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), Tr("Чужая земля", "Partner's Land"));
                }
            } else {
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.45f, 1.0f),
                                   Tr("★ Одно государство (Кооп)", "★ One State (Co-op)"));
            }

            unsigned long long rtt = 0;
            if (LockstepNet_RoundTrip(&rtt)) {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::TextColored(rtt < 80 ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   Tr("%llu мс", "%llu ms"), rtt);
            }

            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("К другу [F6]", "To Partner [F6]"))) TwoStates_JumpCameraToPartner();
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("Сигнал [Alt+ЛКМ]", "Ping [Alt+LMB]"))) TwoStates_PingAtCursor();
            if (TwoStates_IsActive() || TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
                ImGui::SameLine();
                if (ImGui::SmallButton(g_show3DBorder ? Tr("Граница: ВКЛ [F7]", "Border: ON [F7]") : Tr("Граница: ВЫКЛ [F7]", "Border: OFF [F7]")))
                    g_show3DBorder = !g_show3DBorder;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(g_imguiVisible ? Tr("Скрыть [Ins]", "Hide [Ins]") : Tr("Меню [Ins]", "Menu [Ins]")))
                g_imguiVisible = !g_imguiVisible;

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                g_miniHudHovered = true;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // Draw Toast notifications (top-center below Mini-HUD)
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    while (!g_toasts.empty() && now - g_toasts.front().createdMs > g_toasts.front().durationMs) {
        g_toasts.pop_front();
    }
    float toastY = 48.0f;
    for (size_t i = 0; i < g_toasts.size(); ++i) {
        const ToastMessage& tm = g_toasts[i];
        ImU32 borderCol = IM_COL32(90, 180, 255, 230);
        ImU32 textCol   = IM_COL32(230, 245, 255, 255);
        if (tm.colorKind == 1) { borderCol = IM_COL32(80, 220, 100, 235); textCol = IM_COL32(210, 255, 215, 255); }
        else if (tm.colorKind == 2) { borderCol = IM_COL32(255, 205, 60, 240); textCol = IM_COL32(255, 240, 180, 255); }
        else if (tm.colorKind == 3) { borderCol = IM_COL32(255, 80, 70, 245); textCol = IM_COL32(255, 205, 200, 255); }

        Draw3DTextBadge(bg, ImVec2(displaySize.x * 0.5f, toastY), tm.text,
                        IM_COL32(18, 22, 28, 235), borderCol, textCol);
        toastY += 28.0f;
    }
    LeaveCriticalSection(&g_ownerLock);
}

// ---------------------------------------------------------------------------
// Map Expansion: Border Unlocker (20x20 km) & 30x30 km Generator
// ---------------------------------------------------------------------------

void TwoStates_SetExpandBorders(bool on)
{
    g_expandBorders = on;
}

bool TwoStates_GetExpandBorders()
{
    return g_expandBorders;
}

bool TwoStates_PatchSaveScriptIni(const char* saveDir, float customWorldSizeMeters)
{
    if (!saveDir || !saveDir[0]) return false;
    char path[MAX_PATH] = {};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\script.ini", saveDir);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return false;
    }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    float worldSize = customWorldSizeMeters > 20000.0f ? customWorldSizeMeters : 20000.0f;
    float halfEdge = worldSize * 0.5f - EXPANDED_BOUNDARY_MARGIN; // 9850m for 20km, 14850m for 30km

    std::istringstream in(content);
    std::ostringstream out;
    std::string line;
    int polyVertsLeft = 0;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (polyVertsLeft > 0) {
            float vx = 0, vy = 0, vz = 0;
            int flag = 0;
            if (sscanf_s(line.c_str(), "%f %f %f %d", &vx, &vy, &vz, &flag) == 4) {
                vx = (vx < 0.0f) ? -halfEdge : halfEdge;
                vz = (vz < 0.0f) ? -halfEdge : halfEdge;
                char buf[128];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f %.1f %.1f %d", vx, vy, vz, flag);
                out << buf << "\r\n";
                polyVertsLeft--;
                continue;
            }
            polyVertsLeft = 0;
        }

        if (customWorldSizeMeters > 20000.0f && line.rfind("$WORLDSIZE", 0) == 0) {
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "$WORLDSIZE %.6f %.6f", customWorldSizeMeters, customWorldSizeMeters);
            out << buf << "\r\n";
            continue;
        }
        if (customWorldSizeMeters > 20000.0f && line.rfind("$DRAWDISTANCE", 0) == 0) {
            out << "$DRAWDISTANCE 75000.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_XMIN", 0) == 0) {
            out << "$BOUNDARY_XMIN 150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_ZMIN", 0) == 0) {
            out << "$BOUNDARY_ZMIN 150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_XMAX", 0) == 0) {
            out << "$BOUNDARY_XMAX -150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_ZMAX", 0) == 0) {
            out << "$BOUNDARY_ZMAX -150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BORDER_POLYGON", 0) == 0) {
            int count = 0;
            if (sscanf_s(line.c_str(), "$BORDER_POLYGON %d", &count) == 1 && count > 0 && count <= 64) {
                polyVertsLeft = count;
            }
            out << line << "\r\n";
            continue;
        }

        out << line << "\r\n";
    }

    std::string patched = out.str();
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(patched.data(), 1, patched.size(), f);
    fclose(f);

    Logf("republicmp  TWO_STATES: patched [%s] (worldSize=%.0fm, border=±%.0fm)", path, worldSize, halfEdge);
    return true;
}

static volatile bool g_creatingMap = false;
static int g_targetMapKm = 30;

static DWORD WINAPI CreateExpandedMapThread(LPVOID)
{
    int km = g_targetMapKm;
    char saveName[64] = {};
    _snprintf_s(saveName, sizeof(saveName), _TRUNCATE, "TwoStates_%dx%dkm", km, km);

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (slash) *slash = 0;

    char saveDir[MAX_PATH] = {};
    _snprintf_s(saveDir, sizeof(saveDir), _TRUNCATE, "%s\\media_soviet\\save\\%s", exePath, saveName);
    CreateDirectoryA(saveDir, nullptr);

    SaveStatus(Tr("[КАРТА %dx%d КМ] Шаг 1/3: Сохранение текущего мира в «%s»...",
                  "[MAP %dx%d KM] Step 1/3: Saving current world to '%s'..."), km, km, saveName);
    TriggerSaveGame(saveName);
    if (!WaitForSaveToFinish(120000)) {
        SaveStatus(Tr("[КАРТА %dx%d КМ] Ошибка: превышено время ожидания сохранения",
                      "[MAP %dx%d KM] Error: save timed out"), km, km);
        g_creatingMap = false;
        return 0;
    }

    SaveStatus(Tr("[КАРТА %dx%d КМ] Шаг 2/3: Расширение границ и масштаба карты до %dx%d км (%d км²)...",
                  "[MAP %dx%d KM] Step 2/3: Expanding borders and map scale to %dx%d km (%d km²)..."),
               km, km, km, km, km * km);
    if (!TwoStates_PatchSaveScriptIni(saveDir, (float)km * 1000.0f)) {
        SaveStatus(Tr("[КАРТА %dx%d КМ] Ошибка: не удалось обновить script.ini",
                      "[MAP %dx%d KM] Error: failed to patch script.ini"), km, km);
        g_creatingMap = false;
        return 0;
    }

    SaveStatus(Tr("[КАРТА %dx%d КМ] Шаг 3/3: Загрузка увеличенной карты «%s»...",
                  "[MAP %dx%d KM] Step 3/3: Loading expanded map '%s'..."), km, km, saveName);
    TriggerLoadSave(saveName);
    WaitForLoadToFinish(180000);
    SaveStatus(Tr("[КАРТА %dx%d КМ] Готово! Карта %dx%d км (%d км²) успешно загружена.",
                  "[MAP %dx%d KM] Done! Map %dx%d km (%d km²) loaded."), km, km, km, km, km * km);
    g_creatingMap = false;
    return 0;
}

void TwoStates_RequestCreateExpandedMap(int sizeKm)
{
    if (g_creatingMap || SaveSync_IsBusy() || !SaveSync_IsWorldLoaded()) return;
    if (sizeKm < 20) sizeKm = 20;
    if (sizeKm > 60) sizeKm = 60;
    g_targetMapKm = sizeKm;
    g_creatingMap = true;
    CreateThread(nullptr, 0, CreateExpandedMapThread, nullptr, 0, nullptr);
}

bool TwoStates_IsCreatingExpandedMap()
{
    return g_creatingMap;
}
