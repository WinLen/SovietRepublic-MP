// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP - TesmioLoader plugin entry points.
#include "tesmio_plugin.h"
#include "../world_sync/local_builds.h"
#include "../game/present_hook.h"
#include "../session/game_speed.h"
#include "../session/money.h"
#include "../game/save_load.h"
#include "../game/imgui_hook.h"
#include "../world_sync/terrain/terrain_hook.h"
#include "../world_sync/buildings/demolish_hook.h"
#include "../world_sync/buildings/placement_collision.h"
#include "../game/session_patches.h"
#include "../world_sync/view_list_scope.h"
#include "../game/input_block.h"
#include "../determinism/state_hash.h"
#include "../determinism/determinism.h"
#include "../determinism/sim_patches.h"
#include "../determinism/probe_trace.h"
#include "../determinism/probe_watch.h"
#include "../save/save_zip.h"
#include "../transport/network.h"
#include "../session/save_sync.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../session/sync_check.h"
#include "../session/settings_sync.h"
#include "../session/weather.h"
#include "../session/two_states.h"
#include "../session/updater.h"


#include "version.h"

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

// Binds every module to the loader's host API.
extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    LocalBuilds_Bind(host);
    PresentHook_Bind(host);
    GameSpeed_Bind(host);
    Money_Bind(host);
    TwoStates_Bind(host);
    SaveLoad_Bind(host);
    SaveZip_Bind(host);
    Network_Bind(host);
    SaveSync_Bind(host);
    BuildSync_Bind(host);
    RoadSync_Bind(host);
    ImGuiHook_Bind(host);
    TerrainHook_Bind(host);
    TerrainSync_Bind(host);
    DemolishHook_Bind(host);
    PlacementCollision_Bind(host);
    SessionPatches_Bind(host);
    ViewListScope_Bind(host);
    InputBlock_Bind(host);
    StateHash_Bind(host);
    Determinism_Bind(host);
    SimPatches_Bind(host);
    ProbeTrace_Bind(host);
    ProbeWatch_Bind(host);
    SyncCheck_Bind(host);
    Weather_Bind(host);
    SettingsSync_Bind(host);
    Updater_Bind(host);
    info->name    = "republicmp";
    info->version = REPUBLICMP_VERSION;
    return 0;
}

// Installs the engine hooks and starts the background threads.
extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    Logf("republicmp  version=%s", REPUBLICMP_VERSION);

    bool okUniversal = InstallInlineHook(g_exeBase + HOOK_UNIVERSAL_RVA, (void*)HookedUniversal,
                                          (void**)&g_origUniversal, kUniversalExpect,
                                          sizeof(kUniversalExpect), "republicmp universal");
    Logf("republicmp  universal hook: %d", okUniversal);

    bool okRoadCommit = InstallInlineHook(g_exeBase + HOOK_ROAD_COMMIT_RVA, (void*)HookedRoadCommit,
                                           (void**)&g_origRoadCommit, kRoadCommitExpect,
                                           sizeof(kRoadCommitExpect), "republicmp road commit");
    Logf("republicmp  road commit hook: %d%s", okRoadCommit,
         okRoadCommit ? "" : " - infrastructure will NOT be synced");

    // The sync screen relies on these to actually stop the game's input.
    bool okInput = InstallInputBlockHooks();
    Logf("republicmp  input block hooks: %d%s", okInput, okInput ? "" : " - the join screen will NOT fully lock input");

    bool okDemolish = InstallDemolishHook();
    Logf("republicmp  demolish hook: %d%s", okDemolish, okDemolish ? "" : " - demolitions will NOT be synced");

    bool okPlanStart = RoadSync_InstallPlanStartHooks();
    Logf("republicmp  plan start hooks: %d%s", okPlanStart,
         okPlanStart ? "" : " - starting planned roads/rails will NOT be synced");

    bool okGroupTool = RoadSync_InstallGroupToolHook();
    Logf("republicmp  group tool hook: %d%s", okGroupTool,
         okGroupTool ? "" : " - the group construction tools will NOT be synced");

    bool okCollision = InstallPlacementCollisionHook();
    Logf("republicmp  placement collision hook: %d%s", okCollision,
         okCollision ? "" : " - access roads may take a different route on each PC");

    bool okTerrain = InstallTerrainHooks();
    Logf("republicmp  terrain hooks: %d%s", okTerrain, okTerrain ? "" : " - terrain will NOT be fully synced");

    // Fixed timer, call-site random numbers, pinned math - inactive until used.
    bool okDeterminism = Determinism_Install();
    Logf("republicmp  determinism layer: %d%s", okDeterminism, okDeterminism ? "" : " - the determinism probe is unavailable");
    SyncCheck_Install();
    Weather_Install();
    TwoStates_Install();
    NetworkInit();
    Updater_Init();

    HANDLE presentHook = CreateThread(nullptr, 0, PresentHookThread, nullptr, 0, nullptr);
    Logf("republicmp  present hook thread started: %d", presentHook != nullptr);

    if (!okUniversal) return 1;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
