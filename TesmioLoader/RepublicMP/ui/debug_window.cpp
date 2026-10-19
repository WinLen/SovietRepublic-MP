// SPDX-License-Identifier: GPL-3.0-only
// See debug_window.h.
#include "debug_window.h"
#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText(const char*, std::string*)
#include "../game/present_hook.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../world_sync/terrain/terrain_hook.h"
#include "../session/save_sync.h"
#include "../determinism/determinism.h"
#include "../session/settings_sync.h"
#include "debug_log.h"
#include "../lockstep/lockstep_net.h"
#include <vector>
#include <string>

DebugWindow g_debug_window;

void DebugWindow::Open()  { isOpen = true; }
void DebugWindow::Close() { isOpen = false; }

// Draws the Debug window: manual building/road sync connection controls and
// the verbose debug log.
void DebugWindow::Draw()
{
    if (!isOpen)
        return;

    static std::string s_buildSyncIp;
    static bool s_buildSyncIpInit = false;
    static std::string s_roadSyncIp;
    static bool s_roadSyncIpInit = false;
    static std::string s_terrainSyncIp;
    static bool s_terrainSyncIpInit = false;
    static bool s_autoScrollDebugLog = true;
    if (!s_buildSyncIpInit) {
        s_buildSyncIp = BuildSync_GetTargetIp();
        s_buildSyncIpInit = true;
    }
    if (!s_roadSyncIpInit) {
        s_roadSyncIp = RoadSync_GetTargetIp();
        s_roadSyncIpInit = true;
    }
    if (!s_terrainSyncIpInit) {
        s_terrainSyncIp = TerrainSync_GetTargetIp();
        s_terrainSyncIpInit = true;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug", &isOpen))
    {
        ImGui::Text("Present calls: %lld", g_presentCallCount);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Lockstep (lockstep/lockstep_net) ---
        if (SaveSync_IsLockstepSession() && LockstepNet_IsConnected()) {
            bool paused = true;
            int speed = 1;
            Determinism_SessionSpeed(&paused, &speed);
            unsigned long long rtt = 0;
            bool rttKnown = LockstepNet_RoundTrip(&rtt);
            LockstepFlow f = LockstepNet_Flow();
            ImGui::TextUnformatted("Lockstep:");
            ImGui::TextWrapped("%s, tick %llu, %s (speed %d)", LockstepNet_IsHost() ? "host" : "player", Determinism_StepCount(),
                               paused ? "paused" : "running", speed);
            ImGui::TextWrapped("Ping %s ms, silent %llu ms", rttKnown ? std::to_string(rtt).c_str() : "?", LockstepNet_MsSinceReceived());
            ImGui::TextWrapped("%s %llu tick(s); waited for the player %d s in all, last %s ago",
                               LockstepNet_IsHost() ? "Player behind by" : "Grants waiting:", f.behind, f.stalls,
                               f.msSinceStall == ~0ull ? "never" : (std::to_string(f.msSinceStall / 1000) + " s").c_str());
            ImGui::TextWrapped("Last match after tick %llu; %s", LockstepNet_LastMatchedTick(),
                               LockstepNet_Desync()[0] ? LockstepNet_Desync() : "no difference");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // --- Building sync (started automatically by the session; these
        // buttons are for manual reconnects) ---
        ImGui::TextUnformatted("Building sync:");

        bool bsBusy = BuildSync_IsConnected() || BuildSync_IsConnecting();
        bool bsIsHost = BuildSync_IsHostRole();

        ImGui::BeginDisabled(bsBusy);
        if (ImGui::Button("Host##bs"))
            BuildSync_StartHost();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##bsIp", &s_buildSyncIp);
        ImGui::SameLine();
        if (ImGui::Button("Connect##bs"))
            BuildSync_StartClient(s_buildSyncIp.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!bsBusy);
        if (ImGui::Button("Disconnect##bs"))
            BuildSync_Reset();
        ImGui::EndDisabled();

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (BuildSync_IsConnected())
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), bsIsHost ? "Hosting" : "Connected");
        else if (BuildSync_IsConnecting())
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), bsIsHost ? "Waiting for client..." : "Connecting...");
        else
            ImGui::TextDisabled("Idle");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Infrastructure sync (started automatically by the save-sync
        // flow; these buttons are for manual reconnects) ---
        ImGui::TextUnformatted("Infrastructure sync:");

        bool rsBusy = RoadSync_IsConnected() || RoadSync_IsConnecting();
        bool rsIsHost = RoadSync_IsHostRole();

        ImGui::BeginDisabled(rsBusy);
        if (ImGui::Button("Host##rs"))
            RoadSync_StartHost();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##rsIp", &s_roadSyncIp);
        ImGui::SameLine();
        if (ImGui::Button("Connect##rs"))
            RoadSync_StartClient(s_roadSyncIp.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!rsBusy);
        if (ImGui::Button("Disconnect##rs"))
            RoadSync_Reset();
        ImGui::EndDisabled();

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (RoadSync_IsConnected())
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), rsIsHost ? "Hosting" : "Connected");
        else if (RoadSync_IsConnecting())
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), rsIsHost ? "Waiting for client..." : "Connecting...");
        else
            ImGui::TextDisabled("Idle");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Terrain sync (started automatically by the save-sync flow;
        // these buttons are for manual reconnects) ---
        ImGui::TextUnformatted("Terrain sync:");

        bool tsBusy = TerrainSync_IsConnected() || TerrainSync_IsConnecting();
        bool tsIsHost = TerrainSync_IsHostRole();

        ImGui::BeginDisabled(tsBusy);
        if (ImGui::Button("Host##ts"))
            TerrainSync_StartHost();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##tsIp", &s_terrainSyncIp);
        ImGui::SameLine();
        if (ImGui::Button("Connect##ts"))
            TerrainSync_StartClient(s_terrainSyncIp.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!tsBusy);
        if (ImGui::Button("Disconnect##ts"))
            TerrainSync_Reset();
        ImGui::EndDisabled();

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (TerrainSync_IsConnected())
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), tsIsHost ? "Hosting" : "Connected");
        else if (TerrainSync_IsConnecting())
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), tsIsHost ? "Waiting for client..." : "Connecting...");
        else
            ImGui::TextDisabled("Idle");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Determinism probe (determinism/determinism) ---
        ImGui::TextUnformatted("Determinism probe:");
        static int s_probeLength = 1;
        static bool s_probeFuzz = false;
        static bool s_probeRecord = false;
        static const int kProbeSteps[] = { 600, 1800, 3600, 10800 };
        static const char* const kProbeLabels[] = { "600 ticks", "1800 ticks", "3600 ticks", "10800 ticks" };
        ImGui::BeginDisabled(Probe_IsRunning() || SaveSync_IsBusy());
        ImGui::SetNextItemWidth(170);
        ImGui::Combo("##probeLength", &s_probeLength, kProbeLabels, IM_ARRAYSIZE(kProbeLabels));
        ImGui::SameLine();
        if (ImGui::Button("Run probe"))
            Probe_Start(kProbeSteps[s_probeLength], 30, s_probeFuzz, s_probeRecord);
        ImGui::Checkbox("Fuzz held frames and speed", &s_probeFuzz);
        ImGui::Checkbox("Build during run 1 (lockstep commands)", &s_probeRecord);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Saves the game as 'mp_probe', then loads and simulates it twice and compares the "
                           "state after complete ticks. Fuzz holds 0-3 frames in run 2 and changes speed every 300 ticks. "
                           "Space may pause; avoid other actions. Not during a multiplayer session.");
        ImGui::TextWrapped("Result: %s", Probe_Status());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Terrain call log (world_sync/terrain/terrain_hook) ---
        static const char* const kTerrainLog[] = { "Off", "Per frame", "Per call" };
        int terrainLog = TerrainHook_GetLogMode();
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo("Terrain call log", &terrainLog, kTerrainLog, IM_ARRAYSIZE(kTerrainLog))) TerrainHook_SetLogMode(terrainLog);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Money trace (determinism) ---
        bool moneyTrace = Determinism_GetMoneyTrace();
        if (ImGui::Checkbox("Money trace (lockstep desyncs)", &moneyTrace)) Determinism_SetMoneyTrace(moneyTrace);
        ImGui::TextWrapped("Both players turn it on: on a desync each log gets the balances and construction sites "
                           "of the ticks before it, and every command that changed the money.");
        ImGui::Spacing();

        // --- Field diff (session/settings_sync) ---
        bool fieldDiff = SettingsSync_GetFieldDiff();
        if (ImGui::Checkbox("Log building fields changed by clicks (FIELD_DIFF)", &fieldDiff))
            SettingsSync_SetFieldDiff(fieldDiff);
        ImGui::TextWrapped("For finding window settings that are not synced yet: open the building's window, "
                           "leave it a few seconds, then click the setting. The changed bytes go to the log.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Verbose sync log (kept out of the main window's log) ---
        ImGui::TextUnformatted("Debug log:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##dbg"))
            DebugLog_Clear();
        ImGui::SameLine();
        ImGui::Checkbox("Autoscroll##dbg", &s_autoScrollDebugLog);

        if (ImGui::BeginChild("dbglog", { -1, -1 }, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoSavedSettings))
        {
            std::vector<std::string> lines = DebugLog_Snapshot();
            for (const std::string& line : lines)
                ImGui::TextWrapped("%s", line.c_str());

            if (s_autoScrollDebugLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
