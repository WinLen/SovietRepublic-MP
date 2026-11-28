// SPDX-License-Identifier: GPL-3.0-only
// Shared Mock Engine Harness, assertion macros, cross-suite mocks, and suite declarations.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <locale.h>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>

#include "../../src/tesmio_plugin.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "../core/crash_upload.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include "../lockstep/world_refs.h"
#include "../lockstep/commands.h"
#include "../lockstep/game_calls.h"
#include "../lockstep/building_switches.h"
#include "../lockstep/vehicle_switches.h"
#include "../determinism/determinism.h"
#include "../determinism/state_hash.h"
#include "../determinism/held_frame.h"
#include "../session/game_speed.h"
#include "../session/two_states.h"
#include "../session/save_sync.h"
#include "../session/mod_sync.h"
#include "../save/save_zip.h"
#include "../lockstep/purchase.h"
#include "../world_sync/vehicles/vehicle_soft_sync.h"
#include "../ui/sync_block.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/local_builds.h"
#include "../game/session_patches.h"
#include "../game/input_block.h"
#include "../game/imgui_hook.h"
#include "../ui/native_menu.h"
#include "../ui/ui_common.h"
#include "../ui/lang.h"
#include "imgui.h"
#include "../lockstep/rename.h"
#include "../world_sync/buildings/build_mode.h"
#include "../world_sync/buildings/build_sync.h"
#include "../determinism/sim_patches.h"
#include "../game/present_hook.h"
#include "../lockstep/brush_tools.h"
#include "../transport/steam_p2p.h"
#include "../transport/network.h"
#include "../transport/lan_discovery.h"
#include "../session/settings_sync.h"
#include "../lockstep/source_slots.h"
#include "../lockstep/switch_common.h"
#include "../lockstep/route_tools.h"
#include "../lockstep/junction_accept.h"
#include "../lockstep/cargo_settings.h"
#include "../lockstep/line_accept.h"
#include "../lockstep/lockstep_net.h"
#include "../world_sync/citizens/citizen_destinations.h"

// ---------------------------------------------------------------------------
// Production Test Exports (not exposed in public headers)
// ---------------------------------------------------------------------------

extern "C" int TsmPluginInit(const TsmHost* origHost, TsmPluginInfo* info);

struct CamVec4 {
    float x;
    float y;
    float w;
};
extern CamVec4 ProjectPlacementPoint(float wx, float wy, float wz);
extern void CleanBuildingName(const char* raw, char* out, size_t maxLen);
extern void SafeCopyPlacementName(const char* src, char* dst, size_t maxLen);
extern bool SimPatches_GetActivePlacementInfo(char* outName, size_t maxLen, float* outX, float* outY, float* outZ);
extern void SimPatches_ClearPlacementActive();
extern void SimPatches_RecordPlacementForTest(const char* name, float x, float y, float z);
extern bool SaveSync_CheckRoadDlcAllowed(int category, int typeId, const char* toolName = nullptr);

// ---------------------------------------------------------------------------
// Minimal Test Framework Globals & Macros
// ---------------------------------------------------------------------------

extern int g_totalTests;
extern int g_passedTests;
extern int g_failedAssertions;

#define TEST_CHECK(expr)                                                                                  \
    do {                                                                                                  \
        if (!(expr)) {                                                                                    \
            printf("  [FAIL] %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr);                  \
            ++g_failedAssertions;                                                                         \
            return false;                                                                                 \
        }                                                                                                 \
    } while (0)

#define TEST_CHECK_NEAR(a, b, eps)                                                                        \
    do {                                                                                                  \
        double _va = (double)(a);                                                                         \
        double _vb = (double)(b);                                                                         \
        if (fabs(_va - _vb) > (eps)) {                                                                    \
            printf("  [FAIL] %s:%d: %.6f != %.6f (eps=%g) [%s vs %s]\n",                                  \
                   __FILE__, __LINE__, _va, _vb, (double)(eps), #a, #b);                                  \
            ++g_failedAssertions;                                                                         \
            return false;                                                                                 \
        }                                                                                                 \
    } while (0)

// ---------------------------------------------------------------------------
// Mock Engine Memory Harness (16 MB fake SOVIET64.exe image)
// ---------------------------------------------------------------------------

inline constexpr size_t MOCK_EXE_SIZE = 0x1000000; // 16 MB covers all RVAs in SOVIET64.exe
inline constexpr size_t VEHICLE_STRUCT_BYTES = 0x1C30; // sizeof(VEHICLE) in v1.2.0.9
extern BYTE* g_mockExeBuffer;
extern char g_mockBaseDir[MAX_PATH];
extern TsmHost g_mockHost;

void MockHost_Log(const char* fmt, ...);
void** MockHost_FindIatSlot(void*, const char*, const char*);
int MockHost_PatchIat(void*, const char*, const char*, void*, void**, const char*);
int MockHost_InstallInlineHook(void*, void*, void**, const unsigned char*, size_t, const char*);
unsigned char* MockHost_AllocNear(unsigned char*, size_t);
int MockHost_ReadablePtr(const void* p, size_t n);
long MockHost_FaultFilter(const char*, void*);
const void* MockHost_Consume(const char*, unsigned);

void InitMockHarness();
void ResetMockWorldMemory();
void WriteAbsJmp12(BYTE* dst, void* fn);

// Helper to write a fake MSVC std::vector<T> header { begin, end, cap } at a target address
template <typename T>
inline void WriteMockVectorHeader(BYTE* vecHeaderAddr, std::vector<T>& backingVec)
{
    if (!vecHeaderAddr) return;
    if (backingVec.empty() || !backingVec.data()) {
        memset(vecHeaderAddr, 0, 3 * sizeof(T*));
        return;
    }
    T* beginPtr = backingVec.data();
    T* endPtr   = beginPtr + backingVec.size();
    T* capPtr   = beginPtr + backingVec.capacity();
    *(T**)(vecHeaderAddr + 0x00) = beginPtr;
    *(T**)(vecHeaderAddr + 0x08) = endPtr;
    *(T**)(vecHeaderAddr + 0x10) = capPtr;
}

// ---------------------------------------------------------------------------
// Shared Command / Sound IAT Mock Exports
// ---------------------------------------------------------------------------

extern int g_mockPlaySound2DCalls;
extern int g_mockPlaySound3DCalls;
extern int g_mockCreatePopupCalls;
extern int g_mockRebuildNotifCalls;
extern char g_mockLastPlaySound2DName[128];
extern const char* g_mockLastPlaySound2DTag;
extern unsigned char g_mockLastPlaySound2DLoop;
extern float g_mockLastPlaySound2DVol;

bool __fastcall MockOrig_PlaySound2D(void* mgr, const char* name, unsigned char loop, float vol, const char* tag);
bool __fastcall MockOrig_PlaySound3D(void* mgr, const char* name, const float* pos, unsigned char loop, float param5);
long long __fastcall MockOrig_CreatePopupWindow(long long world, int kind, long long ptr, int subkind, long long p5,
                                               int popupType, int stringId, long long p8, long long p9);
void __fastcall MockOrig_RebuildNotificationPopups(long long world);
int MockHost_PatchIatForCommands(void*, const char*, const char* fn, void*, void** outOrig, const char*);
int MockHost_InstallInlineHookForCommands(void* target, void*, void** outTramp, const unsigned char*, size_t, const char*);

// ---------------------------------------------------------------------------
// Shared Cross-Suite Wire Structs
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct TestRenameMsg {
    unsigned magic, kind, index, check, chars;
    unsigned short text[64];
};

struct VehicleSoftSyncEntry_M4 {
    int place;
    unsigned vehicleId;
    int currentBldId;
    int scheduleIdx;
    float fuel;
    float speed;
    float wear[3];
    float pos[3];
    unsigned short cargoCount;
    unsigned short flags;
    unsigned cargoResHash[SOFT_SYNC_VEH_CARGO_MAX];
    float cargoAmount[SOFT_SYNC_VEH_CARGO_MAX];
    float passengers;
    float workers;
    float students;
    float tourists;
};

struct VehicleSoftSyncMsg_M4 {
    unsigned count;
    VehicleSoftSyncEntry_M4 entries[1];
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// All 74 Test Suite Entry Points (order matches kTests[74] in test_main.cpp)
// ---------------------------------------------------------------------------

bool Test_RvaAndSafeMem();
bool Test_WorldRefs();
bool Test_StateHash();
bool Test_TwoStates_ZonesAndEconomy();
bool Test_TwoStates_SaveMetaAndScriptIniSafety();
bool Test_ModSync_HeaderBinAndWorkshopFilter();
bool Test_BuildingSwitches_Commands_SaveZip();
bool Test_WaitingLobbyAndActionLock();
bool Test_TwoStates_Loans();
bool Test_TwoStates_EconomyStats();
bool Test_SingleplayerIsolationAndReset();
bool Test_VehicleDlcFilter_ScriptName_DiagLog();
bool Test_VehicleCargoAndScheduleSync();
bool Test_CameraPersistenceAndFraming();
bool Test_RemoteSoundAndWarningIsolation();
bool Test_E2E_Tier1_FeatureCoverage_R1_to_R6();
bool Test_E2E_Tier1_FeatureCoverage_R7_to_R12();
bool Test_E2E_Tier1_FeatureCoverage_R13_to_R18();
bool Test_E2E_Tier2_BoundaryCases_R1_to_R6();
bool Test_E2E_Tier2_BoundaryCases_R7_to_R12();
bool Test_E2E_Tier2_BoundaryCases_R13_to_R18();
bool Test_E2E_Tier3_CrossFeatureInteractions();
bool Test_E2E_Tier4_RealWorldScenarios();
bool Test_Stress_M1_Challenger_R1_R4_R5_R16();
bool Test_Stress_M4_Challenger_R3_R10_R11_R17();
bool Test_Stress_M4_Challenger2();
bool Test_Stress_M4_Challenger1_Iter2();
bool Test_Stress_M1_Challenger2();
bool Test_Stress_M1_Challenger1_Iter2();
bool Test_Stress_M3_Challenger2();
bool Test_Stress_M3_Challenger1_R9_R12();
bool Test_Stress_M5_Challenger2();
bool Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18();
bool Test_Stress_M6_Challenger1_R13();
bool Test_Stress_M6_Challenger2();
bool Test_SettingsSync_MediaPrisonOrphanageAlerts();
bool Test_Stress_Gen6_Challenger1();
bool Test_JoinHubDistOfficeCarrierAndStorageVisuals();
bool Test_CrashFixesAndRcoTrackBuilder();
bool Test_SystemUiInterceptionAndNetworkTools_M9_1();
bool Test_Stress_M9_1_Challenger1();
bool Test_MissingUiElementsAndBufferSafety_M9_2();
bool Test_Stress_M9_2_Challenger2();
bool Test_Stress_M9_2_Challenger1();
bool Test_SaveSyncAndEconStatePersistence_M9_3();
bool Test_Stress_M9_3_Challenger1();
bool Test_Stress_M9_3_Challenger2();
bool Test_StateHashAndTwoStates_M9_4();
bool Test_Stress_M9_4_Challenger1();
bool Test_Stress_M9_4_Challenger2();
bool Test_R2Bugs_DenomReplaceAutoReplaceLineSet();
bool Test_R3Bugs_PersonalCarAutoBuyFactoryQueue();
bool Test_R1SecondPass_WasteSetForBioCrossroadEditor();
bool Test_HookFixesAndRunAudit_M1();
bool Test_Audit7Fixes_M1();
bool Test_LanDiscoveryDirectSmoke_M1();
bool Test_SteamFreeSessionAndModSyncDirectSmoke_M2();
bool Test_LanUiBrowserHostIpAndToastsDirectSmoke_M3();
bool Test_LanE2eSuite_M4();
bool Test_HotfixV171_All();
bool Test_RcoRailConstructionFixes_All();
bool Test_TwoStatesResearchDesyncFixes();
bool Test_RcoRefuelHeldFrameAndSoftSyncFixes();
bool TestPerf_BldPtrOwnerCacheParityAndInvalidation();
bool TestPerf_SinglePassDemographicsAndUiCacheParity();
bool TestPerf_InPlaceEconAttributionAndNestedVehicleOverride();
bool TestPerf_ResearchAndVehLicenseMountCache();
bool TestPerf_SiteRandFlatTableAndSwitchInPlaceCompare();
bool TestPerf_TcpNoDelayAndSteamP2PRingBuffer();
bool TestPerf_M6TsUiAndHeldFrameOptimizations();
bool SubTest_M6_HeldFrameCoalescedGuardAndVisualParity();
bool Test_SpaceProgramPhase2_DeterminismOrbitsSoftSyncMixedDlc();
bool Test_SpaceProgramPhase3_MutatorsPadSwitchesAndCameraIsolation();
bool Test_SpaceProgramPhase4_TwoStatesSpaceRace();
bool Test_CrashUpload_ZipBundleDedupAndHttpPost();
