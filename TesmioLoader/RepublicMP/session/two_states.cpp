// SPDX-License-Identifier: GPL-3.0-only
// See two_states.h.
#include "two_states.h"
#include "save_sync.h"
#include "mod_sync.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "../game/save_load.h"
#include "../game/window_focus.h"
#include "../lockstep/commands.h"
#include "../lockstep/lockstep_net.h"
#include "../determinism/determinism.h"
#include "../ui/lang.h"
#include "../ui/native_menu.h"
#include "imgui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unordered_map>
#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <filesystem>
#include <system_error>
#include <algorithm>

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
static volatile bool g_expandBorders = false; // default: standard 20x20 km map with borders (16x16 km playable zone)
static bool g_show3DBorder = true;
static volatile bool g_sendClientLogs = true;

static double g_stateRub[2] = { 0.0, 0.0 };
static double g_stateUsd[2] = { 0.0, 0.0 };
static double g_netTradeRub[2] = { 0.0, 0.0 };
static double g_netTradeUsd[2] = { 0.0, 0.0 };
static bool g_treasuryInitialized = false;
static bool g_loadedFromCoopSave = false;
static volatile bool g_importingSpSave = false;
static char g_savedJoinAddress[64] = "";

static INIT_ONCE g_ownerLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_ownerLock;
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
    TsmEnsureCriticalSection(&g_ownerLockOnce, &g_ownerLock);
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
static bool IsUnlimitedMoney()
{
    if (!g_exeBase) return false;
    __try {
        return *(const int*)(g_exeBase + WORLD_RVA + WORLD_UNLIMITED_MONEY_OFFSET) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// C3D_TERRAIN boundary IAT hooks + Fair Economy Simulation Inline Hooks
// ---------------------------------------------------------------------------

typedef float (__fastcall* TerrainBoundaryFn)(BYTE* terrain);
typedef float (__fastcall* CollisionTerrainHeightFn)(BYTE* terrain, const float pos3[3]);
static TerrainBoundaryFn o_getBoundaryXMin = nullptr;
static TerrainBoundaryFn o_getBoundaryXMax = nullptr;
static TerrainBoundaryFn o_getBoundaryZMin = nullptr;
static TerrainBoundaryFn o_getBoundaryZMax = nullptr;
static CollisionTerrainHeightFn g_collisionTerrainHeight = nullptr;

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
#define RVA_MASTER_SAVE     0x42CC70
#define RVA_MASTER_SAVE_AUTOSAVE_JE 0x42CCD9

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
static const BYTE kMasterSaveExpect[19] = {
    0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x0A, 0x00, 0x00
};

typedef __int64 (__fastcall* BldEconFn)(BYTE* world, BYTE* building);
typedef __int64 (__fastcall* ConstructStepFn)(BYTE* world, BYTE* building, float dt, unsigned char flag);
typedef __int64 (__fastcall* VehicleSaleFn)(BYTE* world, BYTE* vehicle, unsigned char a3, unsigned char a4);
typedef void (__fastcall* MasterSaveFn)(BYTE* world);

static BldEconFn o_bldEcon = nullptr;
static BldEconFn o_customsBorder = nullptr;
static BldEconFn o_customsVehicle = nullptr;
static ConstructStepFn o_constructStep = nullptr;
static VehicleSaleFn o_vehicleSale = nullptr;
static MasterSaveFn o_masterSave = nullptr;

static void WriteCoopSaveMetaInternal(const char* fullSaveDir);
static bool BuildFullSaveDir(const char* saveRel, char* outFullDir, size_t outSize);

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
    if (IsUnlimitedMoney()) return false;
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

struct VehSaveSnapshot {
    BYTE* veh;
    unsigned long long f17E8;
    unsigned long long f1A54;
    unsigned long long f1A7C;
};
static std::vector<VehSaveSnapshot> g_vehSaveSnaps;

static void SnapshotOneVehicleBeforeSave(BYTE* v)
{
    if (!v) return;
    VehSaveSnapshot s;
    s.veh   = v;
    s.f17E8 = *(unsigned long long*)(v + 0x17E8);
    s.f1A54 = *(unsigned long long*)(v + 0x1A54);
    s.f1A7C = *(unsigned long long*)(v + 0x1A7C);
    g_vehSaveSnaps.push_back(s);
}

static void SnapshotVehiclesBeforeSave()
{
    g_vehSaveSnaps.clear();
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** vb = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
        BYTE** ve = *(BYTE***)(g_exeBase + VEC_VEHICLES_END_RVA);
        if (vb && ve > vb && (ve - vb) <= 65536) {
            g_vehSaveSnaps.reserve((size_t)(ve - vb));
            for (BYTE** it = vb; it < ve; ++it) {
                BYTE* v = *it;
                if (!v) continue;
                SnapshotOneVehicleBeforeSave(v);
                BYTE** cBegin = *(BYTE***)(v + 0x3A0);
                BYTE** cEnd = *(BYTE***)(v + 0x3A8);
                if (cBegin && cEnd > cBegin && (cEnd - cBegin) <= 1024) {
                    for (BYTE** cit = cBegin; cit < cEnd; ++cit) {
                        if (*cit) SnapshotOneVehicleBeforeSave(*cit);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void RestoreVehiclesAfterSave()
{
    DiagLog_BeginSafeSeh();
    __try {
        for (size_t i = 0; i < g_vehSaveSnaps.size(); ++i) {
            const VehSaveSnapshot& s = g_vehSaveSnaps[i];
            *(unsigned long long*)(s.veh + 0x17E8) = s.f17E8;
            *(unsigned long long*)(s.veh + 0x1A54) = s.f1A54;
            *(unsigned long long*)(s.veh + 0x1A7C) = s.f1A7C;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    g_vehSaveSnaps.clear();
}

static void __fastcall H_MasterSave(BYTE* world)
{
    bool isCoopSave = g_inSession || NativeMenu_IsHostOnNextLoad() || TwoStates_IsCoopBankMounted();
    bool twoStates  = TwoStates_IsActive() ||
                      (isCoopSave && g_treasuryInitialized &&
                       (g_inSession ? g_sessionMode : g_chosenMode) == COOP_MODE_TWO_STATES);

    char saveRel[128] = {};
    char fullSaveDir[MAX_PATH] = {};
    double prevRub = 0.0, prevUsd = 0.0;
    bool moneySwapped = false;
    BYTE prevFc90 = 0;
    bool fc90Patched = false;
    BYTE* autosaveJeAddr = g_exeBase ? (g_exeBase + RVA_MASTER_SAVE_AUTOSAVE_JE) : nullptr;
    BYTE prevAutosaveJe = 0x74;
    DWORD oldProt = 0;
    bool autosaveJePatched = false;

    BYTE searchPoolBackup[72] = {};
    bool searchPoolSaved = false;

    SnapshotVehiclesBeforeSave();

    DiagLog_BeginSafeSeh();
    __try {
        if (world) {
            memcpy(searchPoolBackup, world + 0x14478, sizeof(searchPoolBackup));
            searchPoolSaved = true;
            strncpy_s(saveRel, (const char*)(world + SAVE_NAME_FIELD_OFFSET), _TRUNCATE);
            if (isCoopSave && _stricmp(saveRel, "saved_last") == 0) {
                // Redirect engine's automatic save-on-exit ("saved_last") into the Co-op save bank
                // so exiting a Co-op session never overwrites Singleplayer's saved_last and always
                // appears in "Continue Co-op" / "Co-op Saves".
                strncpy_s((char*)(world + SAVE_NAME_FIELD_OFFSET), SAVE_NAME_FIELD_MAX_LEN, "save/coop_last_session", _TRUNCATE);
                strncpy_s(saveRel, "save/coop_last_session", _TRUNCATE);
                prevFc90 = *(BYTE*)(world + 0xFC90);
                *(BYTE*)(world + 0xFC90) = 1;
                fc90Patched = true;
            }
            if (saveRel[0] && BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir))) {
                CreateDirectoryA(fullSaveDir, nullptr);
                char scriptPath[MAX_PATH] = {};
                _snprintf_s(scriptPath, sizeof(scriptPath), _TRUNCATE, "%s\\script.ini", fullSaveDir);
                if (!fc90Patched && GetFileAttributesA(scriptPath) == INVALID_FILE_ATTRIBUTES) {
                    prevFc90 = *(BYTE*)(world + 0xFC90);
                    *(BYTE*)(world + 0xFC90) = 1; // force C3D_TERRAIN::Save to write script.ini & terrain files
                    fc90Patched = true;
                }
            }
            if (isCoopSave) {
                *(BYTE*)(world + SAVE_ZIP_AFTER_FLAG_OFFSET) = 0;
                if (autosaveJeAddr && *autosaveJeAddr == 0x74) {
                    if (VirtualProtect(autosaveJeAddr, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                        prevAutosaveJe = *autosaveJeAddr;
                        *autosaveJeAddr = 0xEB; // always skip AUTOSAVE_TO_ZIP so Co-op saves remain folders
                        autosaveJePatched = true;
                    }
                }
            }
            if (twoStates && !IsUnlimitedMoney()) {
                prevRub = *WorldRubles();
                prevUsd = *WorldDollars();
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
                moneySwapped = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();

    if (o_masterSave) {
        Determinism_ToolScopeBegin();
        o_masterSave(world);
        Determinism_ToolScopeEnd();
    }

    RestoreVehiclesAfterSave();

    DiagLog_BeginSafeSeh();
    __try {
        if (searchPoolSaved && world) {
            memcpy(world + 0x14478, searchPoolBackup, sizeof(searchPoolBackup));
        }
        if (autosaveJePatched && autosaveJeAddr) {
            *autosaveJeAddr = prevAutosaveJe;
            VirtualProtect(autosaveJeAddr, 2, oldProt, &oldProt);
        }
        if (fc90Patched && world) {
            *(BYTE*)(world + 0xFC90) = prevFc90;
        }
        if (moneySwapped) {
            *WorldRubles()  = prevRub;
            *WorldDollars() = prevUsd;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();

    if (isCoopSave && fullSaveDir[0]) {
        if (g_expandBorders) {
            TwoStates_PatchSaveScriptIni(fullSaveDir, 0.0f);
        }
        WriteCoopSaveMetaInternal(fullSaveDir);
        if (strstr(saveRel, "mp_syncsave_") == nullptr) {
            int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
            if (mode == COOP_MODE_TWO_STATES) {
                char msg[240];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                            Tr("Кооп-сохранение записано (Запад: %.0f РУБ / %.0f $, Восток: %.0f РУБ / %.0f $)",
                               "Co-op save written (West: %.0f RUB / %.0f $, East: %.0f RUB / %.0f $)"),
                            g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
                TwoStates_AddToast(msg, 1);
            } else {
                TwoStates_AddToast(Tr("Кооп-сохранение успешно записано", "Co-op save written"), 1);
            }
        }
    }
}

bool TwoStates_Install()
{
    EnsureOwnerLock();
    TwoStates_LoadSettings();
    TwoStates_MountSaveBank(false);

    if (g_engine) {
        g_collisionTerrainHeight = (CollisionTerrainHeightFn)GetProcAddress(
            g_engine, "?CollisionTerrainHeightOverPoint@C3D_TERRAIN@@QEAAMVC3DVECTOR3@@@Z");
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
    int saveOk = InstallInlineHook(g_exeBase + RVA_MASTER_SAVE, (void*)&H_MasterSave, (void**)&o_masterSave,
                                   kMasterSaveExpect, sizeof(kMasterSaveExpect), "republicmp two_states master_save");

    Logf("republicmp  TWO_STATES: terrain boundary hooks (%d/4), economy attribution hooks (%d/5), master_save=%d, heightFn=%p",
         ok, econOk, saveOk, (void*)g_collisionTerrainHeight);
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
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = 100000000.0;
                g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = 100000000.0;
                g_stateUsd[1] = 100000000.0;
                *WorldRubles()  = 100000000.0;
                *WorldDollars() = 100000000.0;
                g_treasuryInitialized = true;
            } else if (!isResync && !g_treasuryInitialized) {
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
                // Already initialized (either from republicmp_coop.ini or while hosting/re-syncing):
                // attribute any local host delta to State 1.
                double diffRub = worldRub - (g_stateRub[0] + g_stateRub[1]);
                double diffUsd = worldUsd - (g_stateUsd[0] + g_stateUsd[1]);
                if (!Determinism_IsActive() && !g_loadedFromCoopSave) {
                    g_stateRub[0] += diffRub;
                    g_stateUsd[0] += diffUsd;
                }
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
    if (!g_loadedFromCoopSave) {
        EnterCriticalSection(&g_ownerLock);
        g_buildingOwners.clear();
        g_vehicleOwners.clear();
        LeaveCriticalSection(&g_ownerLock);
        g_netTradeRub[0] = g_netTradeRub[1] = 0.0;
        g_netTradeUsd[0] = g_netTradeUsd[1] = 0.0;
    }

    g_isHost = isHost;
    g_sessionMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (borderHalfWidth >= 50.0f && borderHalfWidth <= 2000.0f)
        g_borderHalfWidth = borderHalfWidth;
    g_expandBorders = expandBorders;
    g_stateRub[0] = stateRub[0];
    g_stateRub[1] = stateRub[1];
    g_stateUsd[0] = stateUsd[0];
    g_stateUsd[1] = stateUsd[1];
    g_treasuryInitialized = true;
    g_inSession = true;
    g_loadedFromCoopSave = false;

    if (g_sessionMode == COOP_MODE_TWO_STATES) {
        __try {
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
                *WorldRubles()  = 100000000.0;
                *WorldDollars() = 100000000.0;
            } else {
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
            }
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
        if (IsUnlimitedMoney()) {
            g_stateRub[0] = g_stateRub[1] = 100000000.0;
            g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
            *WorldRubles()  = 100000000.0;
            *WorldDollars() = 100000000.0;
        } else {
            *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
            *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
        }
        Commands_RecordMoney();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TwoStates_OnSessionEnded()
{
    g_inSession = false;
    g_inSimStep = false;
    g_treasuryInitialized = false;
    ModSync_EndSession();
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
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) {
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
    if (!g_inCommand || !TwoStates_IsActive() || IsUnlimitedMoney()) {
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

static int g_stepBldCount[2] = { 0, 0 };

void TwoStates_BeforeStep(BYTE* world)
{
    if (!TwoStates_IsActive() || !world) return;
    if (IsUnlimitedMoney()) {
        g_inSimStep = false;
        g_stateRub[0] = g_stateRub[1] = 100000000.0;
        g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
        return;
    }
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
                if (b[BLD_PAY_RUB_OFFSET] && g_stateRub[idx] <= 0.0) b[BLD_PAY_RUB_OFFSET] = 0;
                if (b[BLD_PAY_USD_OFFSET] && g_stateUsd[idx] <= 0.0) b[BLD_PAY_USD_OFFSET] = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

double TwoStates_ComputeStepWeightState1(int countState1, int countState2)
{
    if (countState1 < 0) countState1 = 0;
    if (countState2 < 0) countState2 = 0;
    int total = countState1 + countState2;
    if (total <= 0) return 0.5;
    return (double)countState1 / (double)total;
}

void TwoStates_AfterStep(BYTE* world)
{
    if (!TwoStates_IsActive() || !world || IsUnlimitedMoney()) {
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

        double w0 = TwoStates_ComputeStepWeightState1(g_stepBldCount[0], g_stepBldCount[1]);

        if (fabs(dRub) > 1e-6) {
            double d0 = dRub * w0;
            g_stateRub[0] += d0;
            g_stateRub[1] += (dRub - d0);
            g_netTradeRub[0] += d0;
            g_netTradeRub[1] += (dRub - d0);
        }
        if (fabs(dUsd) > 1e-6) {
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
    if (!TwoStates_IsActive() || g_inSimStep || IsUnlimitedMoney()) {
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
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
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
    if (!TwoStates_IsActive() || IsUnlimitedMoney()) return;
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

bool TwoStates_CheckBuildingControlAllowed(const BYTE* buildingPtr, const char* actionDesc)
{
    if (!TwoStates_IsActive() || !buildingPtr) return true;
    int id = -1;
    float x = 0.0f, z = 0.0f;
    __try {
        id = *(const int*)buildingPtr;
        x  = *(const float*)(buildingPtr + 0x350);
        z  = *(const float*)(buildingPtr + 0x358);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return TwoStates_CheckBuildingControlAllowed(id, x, z,
                                                 actionDesc ? actionDesc : Tr("настройка чужого здания", "configuring foreign building"));
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

static constexpr ULONGLONG PING_LIFETIME_MS = 20000;
static bool      g_localPingActive = false;
static float     g_localPingX      = 0.0f;
static float     g_localPingY      = 0.0f;
static float     g_localPingZ      = 0.0f;
static ULONGLONG g_localPingSentMs = 0;

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

void TwoStates_SetShow3DBorder(bool on) { g_show3DBorder = on; }
bool TwoStates_GetShow3DBorder()         { return g_show3DBorder; }

void TwoStates_PingAtCursor()
{
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
        LockstepNet_SendCursor(cx, cy, cz, lx, lz, true);
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

// Native Minimap pointer at world + 0xD400 (RVA 0x9E2310), HUD hidden flag at world + 0xFBA (RVA 0x9D5ECA).
static bool GetNativeMinimapRect(float* outCenterX, float* outCenterY, float* outMapSize)
{
    if (!g_exeBase) return false;
    __try {
        BYTE hideHud = *(BYTE*)(g_exeBase + WORLD_RVA + 0xFBA);
        if (hideHud != 0) return false;
        BYTE* mm = *(BYTE**)(g_exeBase + WORLD_RVA + 0xD400);
        if (!mm) return false;
        if (mm[0] == 0) return false; // minimap panel collapsed
        float rightX  = *(float*)(mm + 0x48);
        float bottomY = *(float*)(mm + 0x4C);
        float mapSize = *(float*)(mm + 0x50);
        float panelW  = *(float*)(mm + 0x54);
        float panelH  = *(float*)(mm + 0x58);
        if (!isfinite(mapSize) || mapSize < 40.0f || mapSize > 2500.0f) return false;
        if (!isfinite(rightX)  || !isfinite(bottomY) || !isfinite(panelW) || !isfinite(panelH)) return false;
        *outCenterX = (rightX - panelW) + panelH * 0.5f;
        *outCenterY = bottomY - panelH * 0.5f;
        *outMapSize = mapSize;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void DrawMinimapOverlay(ImDrawList* dl)
{
    float cx = 0.0f, cy = 0.0f, mapSize = 0.0f;
    if (!GetNativeMinimapRect(&cx, &cy, &mapSize)) return;

    float half = mapSize * 0.5f;
    ImVec2 mapMin(cx - half, cy - half);
    ImVec2 mapMax(cx + half, cy + half);

    dl->PushClipRect(mapMin, mapMax, true);

    bool twoStates = (TwoStates_IsActive() || TwoStates_GetMode() == COOP_MODE_TWO_STATES);
    if (twoStates) {
        int myState = TwoStates_LocalState(); // 1 = West (X < 0), 2 = East (X > 0)
        float borderZoneHalfPx = (g_borderHalfWidth / 20000.0f) * mapSize;
        if (borderZoneHalfPx < 1.5f) borderZoneHalfPx = 1.5f;

        ImU32 ownTint     = IM_COL32(45, 210, 95, 36);
        ImU32 partnerTint = IM_COL32(235, 65, 55, 36);
        ImU32 westTint    = (myState == 1) ? ownTint : partnerTint;
        ImU32 eastTint    = (myState == 2) ? ownTint : partnerTint;

        // Subtle territory tint on West (X < 0) and East (X > 0) halves
        dl->AddRectFilled(mapMin, ImVec2(cx - borderZoneHalfPx, mapMax.y), westTint);
        dl->AddRectFilled(ImVec2(cx + borderZoneHalfPx, mapMin.y), mapMax, eastTint);

        // Shared border zone band [-g_borderHalfWidth .. +g_borderHalfWidth]
        dl->AddRectFilled(ImVec2(cx - borderZoneHalfPx, mapMin.y),
                          ImVec2(cx + borderZoneHalfPx, mapMax.y),
                          IM_COL32(255, 215, 60, 42));

        // Dashed/highlighted border zone edges
        dl->AddLine(ImVec2(cx - borderZoneHalfPx, mapMin.y),
                    ImVec2(cx - borderZoneHalfPx, mapMax.y),
                    IM_COL32(90, 200, 255, 175), 1.2f);
        dl->AddLine(ImVec2(cx + borderZoneHalfPx, mapMin.y),
                    ImVec2(cx + borderZoneHalfPx, mapMax.y),
                    IM_COL32(255, 175, 70, 175), 1.2f);

        // Center state border line X = 0
        dl->AddLine(ImVec2(cx, mapMin.y), ImVec2(cx, mapMax.y), IM_COL32(255, 65, 50, 235), 2.5f);
        dl->AddLine(ImVec2(cx, mapMin.y), ImVec2(cx, mapMax.y), IM_COL32(255, 230, 95, 200), 1.2f);

        // Highlight border around the player's own half of the minimap
        ImVec2 ownMin = (myState == 1) ? mapMin : ImVec2(cx, mapMin.y);
        ImVec2 ownMax = (myState == 1) ? ImVec2(cx, mapMax.y) : mapMax;
        dl->AddRect(ownMin, ownMax, IM_COL32(75, 240, 120, 200), 0.0f, 0, 2.0f);

        // Top badges inside West & East halves showing which side is YOURS vs PARTNER'S
        const char* westLabel = (myState == 1) ? Tr("ЗАПАД (ВЫ)", "WEST (YOU)") : Tr("ЗАПАД (ПАРТНЁР)", "WEST (PARTNER)");
        const char* eastLabel = (myState == 2) ? Tr("ВОСТОК (ВЫ)", "EAST (YOU)") : Tr("ВОСТОК (ПАРТНЁР)", "EAST (PARTNER)");

        ImU32 ownBg       = IM_COL32(16, 48, 24, 225);
        ImU32 ownBrd      = IM_COL32(80, 240, 125, 245);
        ImU32 ownTxt      = IM_COL32(185, 255, 200, 255);
        ImU32 partnerBg   = IM_COL32(52, 20, 20, 220);
        ImU32 partnerBrd  = IM_COL32(255, 110, 95, 235);
        ImU32 partnerTxt  = IM_COL32(255, 205, 195, 255);

        float badgeY = mapMin.y + 13.0f;
        float westCx = mapMin.x + half * 0.5f;
        float eastCx = cx + half * 0.5f;

        Draw3DTextBadge(dl, ImVec2(westCx, badgeY), westLabel,
                        (myState == 1) ? ownBg : partnerBg,
                        (myState == 1) ? ownBrd : partnerBrd,
                        (myState == 1) ? ownTxt : partnerTxt);
        Draw3DTextBadge(dl, ImVec2(eastCx, badgeY), eastLabel,
                        (myState == 2) ? ownBg : partnerBg,
                        (myState == 2) ? ownBrd : partnerBrd,
                        (myState == 2) ? ownTxt : partnerTxt);
    }

    ULONGLONG now = GetTickCount64();

    // Draw local ping marker on minimap (20s)
    if (g_localPingActive && now - g_localPingSentMs < PING_LIFETIME_MS) {
        float ageSec = (float)(now - g_localPingSentMs) * 0.001f;
        float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
        float px = cx + (g_localPingX / 20000.0f) * mapSize;
        float py = cy - (g_localPingZ / 20000.0f) * mapSize;
        dl->AddCircle(ImVec2(px, py), 5.0f + pulse * 7.0f, IM_COL32(110, 245, 130, 235), 20, 2.0f);
        dl->AddCircleFilled(ImVec2(px, py), 3.5f, IM_COL32(80, 255, 120, 255));
    }

    // Draw partner cursor & partner ping marker on minimap
    if (LockstepNet_IsConnected()) {
        PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
        if (pc.valid) {
            if (now - pc.lastUpdateMs < 10000 && (fabsf(pc.x) > 0.01f || fabsf(pc.z) > 0.01f)) {
                float px = cx + (pc.x / 20000.0f) * mapSize;
                float py = cy - (pc.z / 20000.0f) * mapSize;
                dl->AddCircleFilled(ImVec2(px, py), 3.5f, IM_COL32(60, 215, 255, 240));
                dl->AddCircle(ImVec2(px, py), 6.5f, IM_COL32(255, 255, 255, 200), 16, 1.5f);
            }
            if (pc.hasPing && now - pc.pingReceivedMs < PING_LIFETIME_MS) {
                float ageSec = (float)(now - pc.pingReceivedMs) * 0.001f;
                float pulse = 0.5f + 0.5f * sinf(ageSec * 8.0f);
                float px = cx + (pc.pingX / 20000.0f) * mapSize;
                float py = cy - (pc.pingZ / 20000.0f) * mapSize;
                dl->AddCircle(ImVec2(px, py), 6.0f + pulse * 9.0f, IM_COL32(255, 215, 50, 245), 24, 2.2f);
                dl->AddCircleFilled(ImVec2(px, py), 4.0f, IM_COL32(255, 85, 40, 255));
            }
        }
    }

    dl->PopClipRect();
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

void TwoStates_DrawOverlay()
{
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
        TwoStates_SaveSettings();
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

    // Draw 3D partner cursor and map ping beacon (local + partner, 20s lifetime)
    if (worldLoaded) {
        DrawPartnerCursorAndPing(bg, displaySize);
        DrawMinimapOverlay(bg);
    }

    // While Host is waiting for Player 2 (before Lockstep starts), keep State 1 treasury synced with world balance
    SessionState st = SaveSync_GetState();
    if (st == SESSION_HOST_LISTENING && TwoStates_IsActive() && !g_inSimStep && !g_inCommand) {
        __try {
            double curRub = *WorldRubles();
            double curUsd = *WorldDollars();
            double sumRub = g_stateRub[0] + g_stateRub[1];
            double sumUsd = g_stateUsd[0] + g_stateUsd[1];
            if (fabs(curRub - sumRub) > 0.01) g_stateRub[0] += (curRub - sumRub);
            if (fabs(curUsd - sumUsd) > 0.01) g_stateUsd[0] += (curUsd - sumUsd);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Draw Toast notifications (top-center below the game's resource bar)
    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    while (!g_toasts.empty() && now - g_toasts.front().createdMs > g_toasts.front().durationMs) {
        g_toasts.pop_front();
    }
    float toastY = 58.0f;
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

// Invariant #4 (AGENTS.md): Base terrain maps (media_soviet\terrains, dlc*\terrains) are inviolable.
// TwoStates_PatchSaveScriptIni may ONLY modify script.ini inside save directories (save, save_coop, mp_syncsave_*).
bool TwoStates_IsSafeSaveScriptPath(const char* saveDir)
{
    if (!saveDir || !saveDir[0]) return false;
    std::string lower(saveDir);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '/') c = '\\';
    }
    if (lower.find("\\terrains\\") != std::string::npos ||
        lower.find("\\terrains") == (lower.size() >= 9 ? lower.size() - 9 : std::string::npos) ||
        lower.rfind("terrains\\", 0) == 0) {
        return false;
    }
    if (lower.find("\\save\\") != std::string::npos ||
        lower.find("\\save_coop\\") != std::string::npos ||
        lower.find("mp_syncsave_") != std::string::npos ||
        lower.rfind("save\\", 0) == 0) {
        return true;
    }
    return false;
}

bool TwoStates_PatchSaveScriptIni(const char* saveDir, float customWorldSizeMeters)
{
    if (!TwoStates_IsSafeSaveScriptPath(saveDir)) return false;
    if (!g_expandBorders && customWorldSizeMeters <= 0.0f) return false;
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

// ---------------------------------------------------------------------------
// Persistent Global Co-op Settings (republicmp_settings.ini)
// ---------------------------------------------------------------------------

static bool GetExeDir(char* outDir, size_t outSize)
{
    if (!outDir || outSize == 0) return false;
    outDir[0] = '\0';
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return false;
    *lastSlash = '\0';
    strncpy_s(outDir, outSize, exePath, _TRUNCATE);
    return true;
}

static std::string GetSettingsIniPath()
{
    char path[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&TwoStates_LoadSettings, &hMod)) {
        GetModuleFileNameA(hMod, path, MAX_PATH);
    }
    std::string dll(path);
    size_t pos = dll.find_last_of("\\/");
    if (pos == std::string::npos) return "republicmp_settings.ini";
    return dll.substr(0, pos + 1) + "republicmp_settings.ini";
}

void TwoStates_LoadSettings()
{
    std::string ini = GetSettingsIniPath();
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    int mode = GetPrivateProfileIntA("coop", "mode", (int)g_chosenMode, ini.c_str());
    g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (!g_inSession) g_sessionMode = g_chosenMode;

    int fullCap = GetPrivateProfileIntA("coop", "full_starting_capital", g_fullStartingCapital ? 1 : 0, ini.c_str());
    g_fullStartingCapital = (fullCap != 0);

    int bw = GetPrivateProfileIntA("coop", "border_half_width_m", (int)lroundf(g_borderHalfWidth), ini.c_str());
    if (bw >= 50 && bw <= 2000) g_borderHalfWidth = (float)bw;

    int expB = GetPrivateProfileIntA("coop", "expand_borders", g_expandBorders ? 1 : 0, ini.c_str());
    g_expandBorders = (expB != 0);

    int show3d = GetPrivateProfileIntA("coop", "show_3d_border", g_show3DBorder ? 1 : 0, ini.c_str());
    g_show3DBorder = (show3d != 0);

    int dlc = GetPrivateProfileIntA("coop", "dlc_check", SaveSync_GetDlcCheck() ? 1 : 0, ini.c_str());
    SaveSync_SetDlcCheck(dlc != 0);

    int sendLogs = GetPrivateProfileIntA("coop", "send_client_logs", g_sendClientLogs ? 1 : 0, ini.c_str());
    g_sendClientLogs = (sendLogs != 0);

    char addr[64] = {};
    GetPrivateProfileStringA("coop", "last_join_address", "", addr, sizeof(addr), ini.c_str());
    if (addr[0]) {
        strncpy_s(g_savedJoinAddress, addr, _TRUNCATE);
    }

    ModSync_LoadSettingsFromIni(ini.c_str());

    Logf("republicmp  SETTINGS: loaded from [%s] (mode=%d, fullCap=%d, border=%dm, expand=%d, show3d=%d, dlc=%d, sendLogs=%d, addr='%s')",
         ini.c_str(), (int)g_chosenMode, g_fullStartingCapital ? 1 : 0, (int)g_borderHalfWidth,
         g_expandBorders ? 1 : 0, g_show3DBorder ? 1 : 0, SaveSync_GetDlcCheck() ? 1 : 0,
         g_sendClientLogs ? 1 : 0, g_savedJoinAddress);
}

void TwoStates_SaveSettings()
{
    std::string ini = GetSettingsIniPath();
    char buf[32] = {};

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", (int)g_chosenMode);
    WritePrivateProfileStringA("coop", "mode", buf, ini.c_str());

    WritePrivateProfileStringA("coop", "full_starting_capital", g_fullStartingCapital ? "1" : "0", ini.c_str());

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", (int)lroundf(g_borderHalfWidth));
    WritePrivateProfileStringA("coop", "border_half_width_m", buf, ini.c_str());

    WritePrivateProfileStringA("coop", "expand_borders", g_expandBorders ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "show_3d_border", g_show3DBorder ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "dlc_check", SaveSync_GetDlcCheck() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "send_client_logs", g_sendClientLogs ? "1" : "0", ini.c_str());

    if (g_savedJoinAddress[0]) {
        WritePrivateProfileStringA("coop", "last_join_address", g_savedJoinAddress, ini.c_str());
    }
    ModSync_SaveSettingsToIni(ini.c_str());
}

const char* TwoStates_GetSavedJoinAddress()
{
    return g_savedJoinAddress;
}

void TwoStates_SetSavedJoinAddress(const char* addr)
{
    if (!addr || !addr[0]) return;
    strncpy_s(g_savedJoinAddress, addr, _TRUNCATE);
}

bool TwoStates_GetSendClientLogs()
{
    return g_sendClientLogs;
}

void TwoStates_SetSendClientLogs(bool send)
{
    g_sendClientLogs = send;
}

// ---------------------------------------------------------------------------
// Separate Singleplayer vs Co-op Save Banks (media_soviet\save <-> save_sp / save_coop)
// ---------------------------------------------------------------------------

#define COOP_BANK_MARKER_NAME ".republicmp_coop_bank"
#define COOP_SAVE_META_NAME   "republicmp_coop.ini"

static bool BuildFullSaveDir(const char* saveRel, char* outFullDir, size_t outSize)
{
    if (!saveRel || !saveRel[0] || !outFullDir || outSize == 0) return false;
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;
    char normRel[160] = {};
    strncpy_s(normRel, saveRel, _TRUNCATE);
    for (char* p = normRel; *p; ++p) {
        if (*p == '/') *p = '\\';
    }
    return _snprintf_s(outFullDir, outSize, _TRUNCATE, "%s\\media_soviet\\%s", exeDir, normRel) >= 0;
}

bool TwoStates_IsCoopBankMounted()
{
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;
    char markerPath[MAX_PATH] = {};
    _snprintf_s(markerPath, sizeof(markerPath), _TRUNCATE, "%s\\media_soviet\\save\\%s", exeDir, COOP_BANK_MARKER_NAME);
    return GetFileAttributesA(markerPath) != INVALID_FILE_ATTRIBUTES;
}

static void CleanStagingFilesInDir(const std::filesystem::path& dirPath)
{
    if (SaveSync_GetState() != SESSION_IDLE || SaveSync_IsWorldLoaded()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dirPath, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
        std::string name = entry.path().filename().string();
        if (_strnicmp(name.c_str(), "mp_syncsave_", 12) == 0 ||
            (name.size() > 15 && _stricmp(name.c_str() + name.size() - 15, ".republicmp.zip") == 0)) {
            std::error_code rmEc;
            std::filesystem::remove_all(entry.path(), rmEc);
        }
    }
}

static bool MoveOrMergeDir(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return true;
    if (!std::filesystem::exists(dst, ec)) {
        std::filesystem::rename(src, dst, ec);
        if (!ec) return true;
    }
    std::filesystem::create_directories(dst, ec);
    for (const auto& entry : std::filesystem::directory_iterator(src, ec)) {
        std::filesystem::path target = dst / entry.path().filename();
        std::error_code rmEc;
        if (std::filesystem::exists(target, rmEc)) {
            std::filesystem::remove_all(target, rmEc);
        }
        std::filesystem::rename(entry.path(), target, rmEc);
        if (rmEc) {
            std::filesystem::copy(entry.path(), target,
                                  std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                  rmEc);
            std::filesystem::remove_all(entry.path(), rmEc);
        }
    }
    std::filesystem::remove_all(src, ec);
    return true;
}

bool TwoStates_MountSaveBank(bool wantCoop)
{
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;

    std::filesystem::path mediaDir = std::filesystem::path(exeDir) / "media_soviet";
    std::filesystem::path saveDir  = mediaDir / "save";
    std::filesystem::path spDir    = mediaDir / "save_sp";
    std::filesystem::path coopDir  = mediaDir / "save_coop";
    std::filesystem::path marker   = saveDir / COOP_BANK_MARKER_NAME;

    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);

    bool curCoop = std::filesystem::exists(marker, ec);
    if (curCoop == wantCoop) {
        CleanStagingFilesInDir(saveDir);
        return true;
    }

    if (wantCoop) {
        // Switching from Singleplayer bank (currently in `save`) -> Co-op bank (`save_coop` -> `save`)
        CleanStagingFilesInDir(saveDir);
        MoveOrMergeDir(saveDir, spDir);
        if (std::filesystem::exists(coopDir, ec)) {
            MoveOrMergeDir(coopDir, saveDir);
        } else {
            std::filesystem::create_directories(saveDir, ec);
        }
        FILE* f = nullptr;
        if (fopen_s(&f, marker.string().c_str(), "wb") == 0 && f) {
            const char* txt = "REPUBLICMP_COOP_SAVE_BANK\n";
            fwrite(txt, 1, strlen(txt), f);
            fclose(f);
        }
        // Automatically migrate any pre-existing Co-op saves (folders with republicmp_coop.ini or coop_ prefix) from save_sp -> save
        if (std::filesystem::exists(spDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(spDir, ec)) {
                if (!entry.is_directory(ec)) continue;
                std::string folderName = entry.path().filename().string();
                bool isCoopFolder = (_strnicmp(folderName.c_str(), "coop_", 5) == 0) ||
                                    std::filesystem::exists(entry.path() / COOP_SAVE_META_NAME, ec);
                if (isCoopFolder) {
                    std::filesystem::path dst = saveDir / folderName;
                    if (!std::filesystem::exists(dst, ec)) {
                        std::filesystem::rename(entry.path(), dst, ec);
                    }
                }
            }
        }
        CleanStagingFilesInDir(saveDir);
        Logf("republicmp  SAVE_BANK: mounted CO-OP save bank at [%s]", saveDir.string().c_str());
    } else {
        // Switching from Co-op bank (currently in `save`) -> Singleplayer bank (`save_sp` -> `save`)
        ModSync_EndSession();
        CleanStagingFilesInDir(saveDir);
        MoveOrMergeDir(saveDir, coopDir);
        if (std::filesystem::exists(spDir, ec)) {
            MoveOrMergeDir(spDir, saveDir);
        } else {
            std::filesystem::create_directories(saveDir, ec);
        }
        if (std::filesystem::exists(marker, ec)) {
            std::filesystem::remove(marker, ec);
        }
        CleanStagingFilesInDir(saveDir);
        Logf("republicmp  SAVE_BANK: mounted SINGLEPLAYER save bank at [%s]", saveDir.string().c_str());
    }
    return true;
}

bool TwoStates_GetLatestCoopSaveName(char* outName, size_t outSize)
{
    if (!outName || outSize == 0) return false;
    outName[0] = '\0';

    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;

    std::filesystem::path mediaDir = std::filesystem::path(exeDir) / "media_soviet";
    std::filesystem::path coopBank = TwoStates_IsCoopBankMounted() ? (mediaDir / "save") : (mediaDir / "save_coop");

    std::error_code ec;
    if (!std::filesystem::exists(coopBank, ec)) return false;

    std::string bestName;
    std::filesystem::file_time_type bestTime{};
    bool found = false;

    for (const auto& entry : std::filesystem::directory_iterator(coopBank, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (_strnicmp(name.c_str(), "mp_syncsave_", 12) == 0) continue;
        std::filesystem::path headerBin = entry.path() / "header.bin";
        std::filesystem::path statsIni  = entry.path() / "stats.ini";
        if (!std::filesystem::exists(headerBin, ec) || !std::filesystem::exists(statsIni, ec)) continue;
        auto mtime = std::filesystem::last_write_time(headerBin, ec);
        if (!found || mtime > bestTime) {
            bestTime = mtime;
            bestName = name;
            found = true;
        }
    }

    if (!found) return false;
    strncpy_s(outName, outSize, bestName.c_str(), _TRUNCATE);
    return true;
}

void TwoStates_SetImportingSpSave(bool importing)
{
    g_importingSpSave = importing;
}

bool TwoStates_IsImportingSpSave()
{
    return g_importingSpSave;
}

// ---------------------------------------------------------------------------
// Per-Save Co-op Metadata (republicmp_coop.ini) & World Load/Save Hooks
// ---------------------------------------------------------------------------

static bool SafeReadWorldMoney(double* outRub, double* outUsd)
{
    __try {
        *outRub = *WorldRubles();
        *outUsd = *WorldDollars();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SafeWriteWorldMoney(double rub, double usd)
{
    __try {
        *WorldRubles()  = rub;
        *WorldDollars() = usd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool SafeReadPendingLoadInfo(BYTE* outTerrainFlag, char* outSaveRel, size_t outSaveRelSize)
{
    if (!g_exeBase) return false;
    __try {
        *outTerrainFlag = *(BYTE*)(g_exeBase + TERRAIN_FLAG_DAT_RVA);
        strncpy_s(outSaveRel, outSaveRelSize, (const char*)(g_exeBase + SAVE_NAME_DAT_RVA), _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void WriteCoopSaveMetaInternal(const char* fullSaveDir)
{
    if (!fullSaveDir || !fullSaveDir[0]) return;
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);

    int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
    double rub0 = g_stateRub[0];
    double rub1 = g_stateRub[1];
    double usd0 = g_stateUsd[0];
    double usd1 = g_stateUsd[1];

    if (!g_treasuryInitialized) {
        double wRub = 0.0, wUsd = 0.0;
        if (SafeReadWorldMoney(&wRub, &wUsd)) {
            if (mode == COOP_MODE_TWO_STATES) {
                if (g_fullStartingCapital) {
                    rub0 = wRub; rub1 = wRub;
                    usd0 = wUsd; usd1 = wUsd;
                } else {
                    rub0 = wRub * 0.5; rub1 = wRub - rub0;
                    usd0 = wUsd * 0.5; usd1 = wUsd - usd0;
                }
            } else {
                rub0 = wRub; rub1 = 0.0;
                usd0 = wUsd; usd1 = 0.0;
            }
        }
    }

    std::ostringstream out;
    out << "[republicmp_coop]\r\n";
    out << "version=1\r\n";
    out << "coop_mode=" << mode << "\r\n";
    out << "border_half_width_m=" << (int)lroundf(g_borderHalfWidth) << "\r\n";
    out << "expand_borders=" << (g_expandBorders ? 1 : 0) << "\r\n";
    out << "state1_rub_cents=" << (long long)llround(rub0 * 100.0) << "\r\n";
    out << "state2_rub_cents=" << (long long)llround(rub1 * 100.0) << "\r\n";
    out << "state1_usd_cents=" << (long long)llround(usd0 * 100.0) << "\r\n";
    out << "state2_usd_cents=" << (long long)llround(usd1 * 100.0) << "\r\n";
    out << "net_trade1_rub_cents=" << (long long)llround(g_netTradeRub[0] * 100.0) << "\r\n";
    out << "net_trade2_rub_cents=" << (long long)llround(g_netTradeRub[1] * 100.0) << "\r\n";
    out << "net_trade1_usd_cents=" << (long long)llround(g_netTradeUsd[0] * 100.0) << "\r\n";
    out << "net_trade2_usd_cents=" << (long long)llround(g_netTradeUsd[1] * 100.0) << "\r\n";

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    out << "building_owners=";
    bool first = true;
    for (const auto& kv : g_buildingOwners) {
        if (!first) out << ",";
        out << kv.first << ":" << kv.second;
        first = false;
    }
    out << "\r\n";

    out << "vehicle_owners=";
    first = true;
    for (const auto& kv : g_vehicleOwners) {
        if (!first) out << ",";
        out << kv.first << ":" << kv.second;
        first = false;
    }
    out << "\r\n";
    LeaveCriticalSection(&g_ownerLock);

    std::string data = out.str();
    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "wb") == 0 && f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        Logf("republicmp  COOP_META: saved [%s] (mode=%d, rub=[%.0f, %.0f], usd=[%.0f, %.0f], blds=%zu, vehs=%zu)",
             metaPath, mode, rub0, rub1, usd0, usd1, g_buildingOwners.size(), g_vehicleOwners.size());
    }
}

static void ParseOwnersList(const std::string& val, std::unordered_map<int, int>& outMap)
{
    outMap.clear();
    size_t pos = 0;
    while (pos < val.size()) {
        size_t comma = val.find(',', pos);
        if (comma == std::string::npos) comma = val.size();
        std::string token = val.substr(pos, comma - pos);
        size_t colon = token.find(':');
        if (colon != std::string::npos) {
            int id = atoi(token.substr(0, colon).c_str());
            int owner = atoi(token.substr(colon + 1).c_str());
            if (id >= 0 && (owner == 1 || owner == 2)) {
                outMap[id] = owner;
            }
        }
        pos = comma + 1;
    }
}

static bool ReadCoopSaveMetaInternal(const char* fullSaveDir, bool updateBudgetsAndMode)
{
    if (!fullSaveDir || !fullSaveDir[0]) return false;
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);

    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    int mode = (int)g_chosenMode;
    int bw = (int)lroundf(g_borderHalfWidth);
    int expB = g_expandBorders ? 1 : 0;
    long long rubCents[2] = { 0, 0 };
    long long usdCents[2] = { 0, 0 };
    long long tradeRubCents[2] = { 0, 0 };
    long long tradeUsdCents[2] = { 0, 0 };
    bool haveRub = false, haveUsd = false;

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_buildingOwners.clear();
    g_vehicleOwners.clear();

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '[' || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "coop_mode") mode = atoi(val.c_str());
        else if (key == "border_half_width_m") bw = atoi(val.c_str());
        else if (key == "expand_borders") expB = atoi(val.c_str());
        else if (key == "state1_rub_cents") { rubCents[0] = _strtoi64(val.c_str(), nullptr, 10); haveRub = true; }
        else if (key == "state2_rub_cents") { rubCents[1] = _strtoi64(val.c_str(), nullptr, 10); haveRub = true; }
        else if (key == "state1_usd_cents") { usdCents[0] = _strtoi64(val.c_str(), nullptr, 10); haveUsd = true; }
        else if (key == "state2_usd_cents") { usdCents[1] = _strtoi64(val.c_str(), nullptr, 10); haveUsd = true; }
        else if (key == "net_trade1_rub_cents") tradeRubCents[0] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade2_rub_cents") tradeRubCents[1] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade1_usd_cents") tradeUsdCents[0] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade2_usd_cents") tradeUsdCents[1] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "building_owners") ParseOwnersList(val, g_buildingOwners);
        else if (key == "vehicle_owners")  ParseOwnersList(val, g_vehicleOwners);
    }
    size_t bldCount = g_buildingOwners.size();
    size_t vehCount = g_vehicleOwners.size();
    LeaveCriticalSection(&g_ownerLock);

    g_netTradeRub[0] = (double)tradeRubCents[0] / 100.0;
    g_netTradeRub[1] = (double)tradeRubCents[1] / 100.0;
    g_netTradeUsd[0] = (double)tradeUsdCents[0] / 100.0;
    g_netTradeUsd[1] = (double)tradeUsdCents[1] / 100.0;

    if (updateBudgetsAndMode) {
        g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
        g_sessionMode = g_chosenMode;
        if (bw >= 50 && bw <= 2000) g_borderHalfWidth = (float)bw;
        g_expandBorders = (expB != 0);
        if (haveRub) {
            g_stateRub[0] = (double)rubCents[0] / 100.0;
            g_stateRub[1] = (double)rubCents[1] / 100.0;
        }
        if (haveUsd) {
            g_stateUsd[0] = (double)usdCents[0] / 100.0;
            g_stateUsd[1] = (double)usdCents[1] / 100.0;
        }
        g_treasuryInitialized = true;
        g_loadedFromCoopSave = true;

        if (g_chosenMode == COOP_MODE_TWO_STATES && (haveRub || haveUsd)) {
            if (IsUnlimitedMoney()) {
                g_stateRub[0] = g_stateRub[1] = 100000000.0;
                g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
                SafeWriteWorldMoney(100000000.0, 100000000.0);
            } else {
                SafeWriteWorldMoney(g_stateRub[0] + g_stateRub[1], g_stateUsd[0] + g_stateUsd[1]);
            }
        }
    }

    Logf("republicmp  COOP_META: loaded [%s] (updateBudgets=%d, mode=%d, rub=[%.0f, %.0f], usd=[%.0f, %.0f], blds=%zu, vehs=%zu)",
         metaPath, updateBudgetsAndMode ? 1 : 0, g_inSession ? (int)g_sessionMode : (int)g_chosenMode,
         g_stateRub[0], g_stateRub[1], g_stateUsd[0], g_stateUsd[1], bldCount, vehCount);
    return true;
}

bool TwoStates_WriteSaveMetaToDir(const char* fullSaveDir)
{
    if (!fullSaveDir || !fullSaveDir[0]) return false;
    WriteCoopSaveMetaInternal(fullSaveDir);
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);
    return GetFileAttributesA(metaPath) != INVALID_FILE_ATTRIBUTES;
}

bool TwoStates_ReadSaveMetaFromDir(const char* fullSaveDir, bool updateBudgetsAndMode)
{
    return ReadCoopSaveMetaInternal(fullSaveDir, updateBudgetsAndMode);
}

void TwoStates_BeforeAnyWorldLoad()
{
    if (!g_exeBase) return;
    bool wantCoopHost = NativeMenu_IsHostOnNextLoad() || g_importingSpSave;

    BYTE terrainFlag = 0;
    char saveRel[128] = {};
    if (!SafeReadPendingLoadInfo(&terrainFlag, saveRel, sizeof(saveRel))) {
        return;
    }

    if (!wantCoopHost) {
        ModSync_BeforeWorldLoad(false, nullptr);
        return;
    }

    if (!TwoStates_IsCoopBankMounted()) {
        // User is importing a Singleplayer save into Co-op!
        bool isSaveFolder = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
        if (terrainFlag == 0 && isSaveFolder) {
            char spFullDir[MAX_PATH] = {};
            char exeDir[MAX_PATH] = {};
            if (BuildFullSaveDir(saveRel, spFullDir, sizeof(spFullDir)) && GetExeDir(exeDir, sizeof(exeDir))) {
                std::error_code ec;
                std::filesystem::path srcPath(spFullDir);
                std::string folderName = srcPath.filename().string();
                std::filesystem::path coopCopy = std::filesystem::path(exeDir) / "media_soviet" / "save_coop" / folderName;
                if (std::filesystem::exists(srcPath, ec) && std::filesystem::is_directory(srcPath, ec)) {
                    std::filesystem::create_directories(coopCopy.parent_path(), ec);
                    if (std::filesystem::exists(coopCopy, ec)) {
                        std::filesystem::remove_all(coopCopy, ec);
                    }
                    std::filesystem::copy(srcPath, coopCopy,
                                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                          ec);
                    // Remove any old republicmp_coop.ini on a fresh SP import so menu capital settings apply cleanly
                    std::filesystem::remove(coopCopy / COOP_SAVE_META_NAME, ec);
                    Logf("republicmp  SAVE_BANK: imported SP save [%s] -> [%s]",
                         srcPath.string().c_str(), coopCopy.string().c_str());
                } else {
                    // Check if it was a .zip save in Singleplayer
                    std::filesystem::path srcZip( std::string(spFullDir) + ".zip" );
                    std::filesystem::path coopZip = std::filesystem::path(exeDir) / "media_soviet" / "save_coop" / (folderName + ".zip");
                    if (std::filesystem::exists(srcZip, ec)) {
                        std::filesystem::create_directories(coopZip.parent_path(), ec);
                        std::filesystem::copy_file(srcZip, coopZip, std::filesystem::copy_options::overwrite_existing, ec);
                    }
                }
            }
        }
        TwoStates_MountSaveBank(true);
    }

    g_importingSpSave = false;

    char fullDir[MAX_PATH] = {};
    bool isSaveRel = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
    bool hasFullDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullDir, sizeof(fullDir)));
    if (hasFullDir && g_expandBorders) {
        TwoStates_PatchSaveScriptIni(fullDir, 0.0f);
    }

    ModSync_BeforeWorldLoad(true, hasFullDir ? fullDir : nullptr);
}

void TwoStates_OnAnyWorldLoaded(bool isLockstepSessionLoad)
{
    if (!g_exeBase) return;
    BYTE terrainFlag = 0;
    char saveRel[128] = {};
    if (!SafeReadPendingLoadInfo(&terrainFlag, saveRel, sizeof(saveRel))) {
        return;
    }

    char fullSaveDir[MAX_PATH] = {};
    bool isSaveRel = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
    bool hasSaveDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir)));

    if (isLockstepSessionLoad) {
        if (hasSaveDir) {
            ReadCoopSaveMetaInternal(fullSaveDir, false);
        }
        TwoStates_OnWorldLoaded();
        return;
    }

    if (hasSaveDir && ReadCoopSaveMetaInternal(fullSaveDir, true)) {
        if (g_chosenMode == COOP_MODE_TWO_STATES) {
            char msg[240];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        Tr("Загружен кооп-сейв «2 государства» (Запад: %.0f РУБ / %.0f $, Восток: %.0f РУБ / %.0f $)",
                           "Loaded 'Two States' Co-op save (West: %.0f RUB / %.0f $, East: %.0f RUB / %.0f $)"),
                        g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
            TwoStates_AddToast(msg, 1);
        } else {
            TwoStates_AddToast(Tr("Загружен кооп-сейв «Одно государство»",
                                  "Loaded 'One State' Co-op save"), 1);
        }
    } else {
        g_loadedFromCoopSave = false;
        g_treasuryInitialized = false;
        EnsureOwnerLock();
        EnterCriticalSection(&g_ownerLock);
        g_buildingOwners.clear();
        g_vehicleOwners.clear();
        LeaveCriticalSection(&g_ownerLock);
        g_netTradeRub[0] = g_netTradeRub[1] = 0.0;
        g_netTradeUsd[0] = g_netTradeUsd[1] = 0.0;
    }
}

bool TwoStates_TriggerQuickCoopSave()
{
    if (!SaveSync_IsWorldLoaded()) return false;
    TwoStates_MountSaveBank(true);

    SYSTEMTIME st;
    GetLocalTime(&st);
    int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
    char saveName[96] = {};
    _snprintf_s(saveName, sizeof(saveName), _TRUNCATE,
                "coop_%s_%04d-%02d-%02d_%02d-%02d-%02d",
                (mode == COOP_MODE_TWO_STATES) ? "2states" : "1state",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char fullSaveDir[MAX_PATH] = {};
    char saveRel[128] = {};
    _snprintf_s(saveRel, sizeof(saveRel), _TRUNCATE, "save\\%s", saveName);
    if (BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir))) {
        CreateDirectoryA(fullSaveDir, nullptr);
    }

    return TriggerSaveGame(saveName);
}

