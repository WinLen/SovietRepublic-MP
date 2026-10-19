// SPDX-License-Identifier: GPL-3.0-only
// See main_menu.h.
#include "main_menu.h"
#include "debug_window.h"
#include "ui_log.h"
#include "sync_block.h"
#include "../game/imgui_hook.h" // g_imguiVisible - "Hide menu" button
#include "../session/save_sync.h"
#include "../lockstep/lockstep_net.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../session/sync_check.h"
#include "../session/two_states.h"
#include "../session/updater.h"
#include "../determinism/determinism.h"
#include "../transport/steam_p2p.h"

#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText(const char*, std::string*)

#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>

MainMenu g_mainMenu;

static const ImVec4 kGrey   = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
static const ImVec4 kBlue   = ImVec4(0.4f, 0.75f, 0.95f, 1.0f);
static const ImVec4 kYellow = ImVec4(0.95f, 0.8f, 0.25f, 1.0f);
static const ImVec4 kGreen  = ImVec4(0.35f, 0.9f, 0.35f, 1.0f);
static const ImVec4 kRed    = ImVec4(0.95f, 0.4f, 0.35f, 1.0f);

// Text in a colour that wraps at the window's edge.
static void Wrapped(const ImVec4& color, const char* fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A coloured dot followed by text on the same line.
static void StatusDot(const ImVec4& color)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.3f,
                                                ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(h * 0.9f, h));
    ImGui::SameLine();
}

// Replay mode: "Buildings  Roads  Terrain" with a green/red dot each (lockstep uses none of them).
static void ChannelIndicators()
{
    struct { const char* name; bool up; } ch[] = {
        { "Buildings", BuildSync_IsConnected() },
        { "Roads", RoadSync_IsConnected() },
        { "Terrain", TerrainSync_IsConnected() },
    };
    ImGui::TextColored(kGrey, "Live sync:");
    for (auto& c : ch) {
        ImGui::SameLine(0, 12);
        StatusDot(c.up ? kGreen : kRed);
        ImGui::TextUnformatted(c.name);
    }
}

// The current join step (from the sync screen), e.g. "Step 4/7 - Sending...  62%".
static void JoinStepLine()
{
    int step, total, percent;
    char text[160];
    if (!SyncBlock_GetStep(&step, &total, text, sizeof(text), &percent)) {
        ImGui::TextColored(kGrey, "Preparing...");
        return;
    }
    if (total > 0) Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Step %d/%d - %s", step, total, text);
    else Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", text);
    if (percent >= 0) {
        char overlay[16];
        _snprintf_s(overlay, sizeof(overlay), _TRUNCATE, "%d%%", percent);
        ImGui::ProgressBar(percent / 100.0f, ImVec2(-1, 0), overlay);
    }
}

static void MinutesSeconds(char* out, size_t n, unsigned long long ms)
{
    _snprintf_s(out, n, _TRUNCATE, "%llu:%02llu", ms / 60000, (ms / 1000) % 60);
}

// Lockstep in the game: the simulation, the connection, how the ticks flow and the comparison.
static void LockstepPanel(bool host)
{
    bool paused = true;
    int speed = 1;
    Determinism_SessionSpeed(&paused, &speed);
    unsigned long long tick = Determinism_StepCount();
    if (paused) Wrapped(kYellow, "Simulation: paused at tick %llu", tick);
    else {
        static const char* const kSpeeds[] = { "", "normal", "fast", "Hyper", "Plaid" };
        Wrapped(kGreen, "Simulation: running, tick %llu (%s)", tick, kSpeeds[speed < 1 ? 1 : speed > 4 ? 4 : speed]);
    }

    unsigned long long rtt = 0;
    if (LockstepNet_RoundTrip(&rtt)) {
        ImVec4 col = rtt < 80 ? kGreen : rtt < 250 ? kYellow : kRed;
        Wrapped(col, "Ping: %llu ms", rtt);
    } else {
        Wrapped(kGrey, "Ping: measuring...");
    }

    LockstepFlow f = LockstepNet_Flow();
    if (host) {
        if (f.msSinceStall < 5000)
            Wrapped(kYellow, "Flow: waiting for the player (%llu ticks behind) - the slower PC sets the pace", f.behind);
        else
            Wrapped(kGreen, "Flow: steady (player %llu tick(s) behind)", f.behind);
    } else {
        if (f.behind > LOCKSTEP_WINDOW / 2)
            Wrapped(kYellow, "Flow: catching up, %llu tick(s) waiting", f.behind);
        else
            Wrapped(kGreen, "Flow: steady");
    }

    const char* desync = LockstepNet_Desync();
    unsigned long long matched = LockstepNet_LastMatchedTick();
    if (desync[0]) {
        Wrapped(kRed, "Sync: %s", desync);
        ULONGLONG left = host ? SaveSync_GetMsUntilDesyncResync() : 0;
        if (left) {
            char t[16];
            MinutesSeconds(t, sizeof(t), left);
            Wrapped(kYellow, "The game goes on; re-sync in %s.", t);
        } else {
            Wrapped(kGrey, host ? "The game goes on; re-sync when you choose." : "The game goes on until the host re-syncs.");
        }
    } else if (host && matched) {
        Wrapped(kGreen, "Sync: the games match (checked after tick %llu)", matched);
    } else {
        Wrapped(kGrey, "Sync: compared every %d ticks%s", LOCKSTEP_HASH_TICKS, host ? "" : " by the host");
    }

    if (!host) return;
    ImGui::Spacing();
    if (ImGui::Button("Re-sync now")) SaveSync_RequestResync();
    ImGui::SameLine();
    static const int kSeconds[] = { -1, 0, 60, 120, 300 };
    static const char* const kLabels[] = { "Off", "immediately", "after 1 min", "after 2 min", "after 5 min" };
    int current = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kSeconds); i++)
        if (kSeconds[i] == SaveSync_GetDesyncResyncSeconds()) current = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("After a desync:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##desyncResync", &current, kLabels, IM_ARRAYSIZE(kLabels))) SaveSync_SetDesyncResyncSeconds(kSeconds[current]);
    Wrapped(kGrey, "Re-sync sends your world to the player again; both games then go on from it.");
}

// Replay mode, host with a player in the game: re-sync now, the interval and the drift.
static void ReplayResyncControls()
{
    static const int kMinutes[] = { 0, 5, 10, 15, 30, 60 };
    static const char* const kLabels[] = { "Off", "every 5 min", "every 10 min", "every 15 min",
                                           "every 30 min", "every 60 min" };
    int current = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kMinutes); i++)
        if (kMinutes[i] == SaveSync_GetAutoResyncMinutes()) current = i;

    if (ImGui::Button("Re-sync now"))
        SaveSync_RequestResync();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##autoResync", &current, kLabels, IM_ARRAYSIZE(kLabels)))
        SaveSync_SetAutoResyncMinutes(kMinutes[current]);
    ULONGLONG left = SaveSync_GetMsUntilAutoResync();
    if (left) {
        char t[16];
        MinutesSeconds(t, sizeof(t), left);
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "next in %s", t);
    }
    Wrapped(kGrey, "Re-sync sends your world to the player again (vehicles, citizens, production).");

    SyncDrift d = SyncCheck_GetDrift();
    if (d.valid) {
        ImVec4 col = d.total < 2.0f ? kGreen : d.total < 10.0f ? kYellow : kRed;
        Wrapped(col, "Drift %.1f%% (day %d %02d:00 - citizens %d/%d, vehicles %d/%d, storage %.1f%%)", d.total, d.day, d.hour,
                d.hostCitizens, d.clientCitizens, d.hostVehicles, d.clientVehicles, d.storage);
    } else {
        Wrapped(kGrey, "Drift: measured every game hour");
    }
    static const int kLimits[] = { 0, 2, 5, 10, 20 };
    static const char* const kLimitLabels[] = { "Off", "above 2%", "above 5%", "above 10%", "above 20%" };
    int limit = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kLimits); i++)
        if (kLimits[i] == SyncCheck_GetAutoResyncPercent()) limit = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Re-sync on drift:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("##driftResync", &limit, kLimitLabels, IM_ARRAYSIZE(kLimitLabels)))
        SyncCheck_SetAutoResyncPercent(kLimits[limit]);
    // Why an automatic re-sync has not happened yet (see session/sync_check.h).
    if (kLimits[limit] > 0 && d.valid && d.overLimitChecks > 0) {
        unsigned long long wait = SyncCheck_GetResyncCooldownMs();
        if (d.overLimitChecks < SYNC_CHECK_CONFIRM_CHECKS)
            Wrapped(kYellow, "above for %d/%d hours", d.overLimitChecks, SYNC_CHECK_CONFIRM_CHECKS);
        else if (wait) {
            char t[16];
            MinutesSeconds(t, sizeof(t), wait);
            Wrapped(kYellow, "waiting %s after the last re-sync", t);
        }
    }
}

static void StatusPanel()
{
    SessionState st = SaveSync_GetState();
    const char* peer = SaveSync_GetPeerIp();
    const char* lastError = SaveSync_GetLastError();
    bool lockstep = SaveSync_IsLockstepSession();
    const char* mode = lockstep ? "lockstep" : "replay mode";

    switch (st) {
    case SESSION_IDLE:
        StatusDot(kGrey);
        ImGui::TextColored(kGrey, "Not in a multiplayer session");
        if (lastError[0]) Wrapped(kRed, "Last session: %s", lastError);
        break;

    case SESSION_HOST_LISTENING:
        if (SaveSync_IsResyncing()) {
            StatusDot(kYellow);
            Wrapped(kYellow, "Re-syncing - waiting for %s to reconnect", peer[0] ? peer : "the player");
            break;
        }
        StatusDot(kBlue);
        Wrapped(kBlue, "Hosting (%s) - waiting for a player", SaveSync_GetLockstep() ? "lockstep" : "replay mode");
        Wrapped(kGrey, "Your game keeps running. Players connect to your IP (port %d).", SAVE_SYNC_PORT);
        if (lastError[0]) Wrapped(kYellow, "%s", lastError);
        break;

    case SESSION_HOST_JOINING:
        StatusDot(kYellow);
        Wrapped(kYellow, SaveSync_IsResyncing() ? "Re-syncing - sending the world to %s" : "Hosting - %s is joining",
                peer[0] ? peer : "a player");
        JoinStepLine();
        break;

    case SESSION_HOST_IN_GAME:
        StatusDot(kGreen);
        Wrapped(kGreen, "Hosting (%s) - %s is in the game", mode, peer[0] ? peer : "a player");
        if (lockstep) LockstepPanel(true);
        else {
            ChannelIndicators();
            ReplayResyncControls();
        }
        break;

    case SESSION_CLIENT_JOINING:
        StatusDot(kYellow);
        Wrapped(kYellow, SaveSync_IsResyncing() ? "Re-syncing with %s" : "Joining %s", peer[0] ? peer : SaveSync_GetTargetIp());
        JoinStepLine();
        break;

    case SESSION_CLIENT_IN_GAME:
        StatusDot(kGreen);
        Wrapped(kGreen, "Connected to %s (%s) - in the game", peer[0] ? peer : SaveSync_GetTargetIp(), mode);
        if (lockstep) LockstepPanel(false);
        else ChannelIndicators();
        break;
    }
}

static void CoopComfortPanel()
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kYellow, "Удобство и навигация в кооперативе:");

    bool showMiniHud = TwoStates_GetShowMiniHud();
    if (ImGui::Checkbox("Верхняя мини-панель (Mini-HUD поверх игры)", &showMiniHud))
        TwoStates_SetShowMiniHud(showMiniHud);

    if (TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
        bool showBorder = TwoStates_GetShow3DBorder();
        if (ImGui::Checkbox("3D-граница государств и погранзона на земле [F7]", &showBorder))
            TwoStates_SetShow3DBorder(showBorder);
    }

    if (ImGui::Button("Камера к партнёру [F6]")) {
        TwoStates_JumpCameraToPartner();
    }
    ImGui::SameLine();
    if (ImGui::Button("Подать сигнал на карте [Alt+ЛКМ]")) {
        TwoStates_PingAtCursor();
    }
    Wrapped(kGrey, "Нажмите Alt + ЛКМ в любом месте карты, чтобы поставить 3D-маяк для партнёра. Нажмите F6, чтобы переместить камеру к курсору или маяку друга.");
}

static void TwoStatesLivePanel()
{
    if (!TwoStates_IsActive()) return;

    ImGui::Spacing();
    ImGui::Separator();
    int myState = TwoStates_LocalState();
    Wrapped(kBlue, "Режим: ДВА ГОСУДАРСТВА  |  Вы: %s", TwoStates_StateName(myState));

    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    if (TwoStates_GetCursorWorldPos(&cx, &cy, &cz)) {
        int zone = TwoStates_GetZoneAt(cx, cz);
        if (zone == 0) {
            Wrapped(kYellow, "Курсор (X=%.0f м, Z=%.0f м): Общая пограничная зона (±%.0f м) — можно стыковать сети",
                    cx, cz, TwoStates_GetBorderHalfWidth());
        } else if (zone == myState) {
            Wrapped(kGreen, "Курсор (X=%.0f м, Z=%.0f м): Ваша территория (%s)",
                    cx, cz, TwoStates_StateName(zone));
        } else {
            Wrapped(kRed, "Курсор (X=%.0f м, Z=%.0f м): Чужая территория (%s) — строительство запрещено",
                    cx, cz, TwoStates_StateName(zone));
        }
    }

    ImGui::Spacing();
    ImGui::TextColored(myState == 1 ? kGreen : kGrey,
                       "Западная ССР (Хост, X < 0):   %.0f ₽ (торг: %+.0f ₽)  |  %.0f $ (торг: %+.0f $)",
                       TwoStates_GetRubles(1), TwoStates_GetNetTradeRub(1),
                       TwoStates_GetDollars(1), TwoStates_GetNetTradeUsd(1));
    ImGui::TextColored(myState == 2 ? kGreen : kGrey,
                       "Восточная ССР (Игрок 2, X >= 0): %.0f ₽ (торг: %+.0f ₽)  |  %.0f $ (торг: %+.0f $)",
                       TwoStates_GetRubles(2), TwoStates_GetNetTradeRub(2),
                       TwoStates_GetDollars(2), TwoStates_GetNetTradeUsd(2));

    ImGui::Spacing();
    ImGui::TextColored(kYellow, "Перевод средств в %s:", TwoStates_StateName(myState == 1 ? 2 : 1));
    static double s_sendRub = 100000.0;
    static double s_sendUsd = 0.0;
    ImGui::SetNextItemWidth(135);
    ImGui::InputDouble("Рубли (₽)##sendRub", &s_sendRub, 50000.0, 500000.0, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(135);
    ImGui::InputDouble("Доллары ($)##sendUsd", &s_sendUsd, 50000.0, 500000.0, "%.0f");
    if (s_sendRub < 0.0) s_sendRub = 0.0;
    if (s_sendUsd < 0.0) s_sendUsd = 0.0;

    if (ImGui::Button("Отправить перевод")) {
        TwoStates_RequestTransfer(s_sendRub, s_sendUsd);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+100 тыс. ₽")) TwoStates_RequestTransfer(100000.0, 0.0);
    ImGui::SameLine();
    if (ImGui::SmallButton("+1 млн ₽")) TwoStates_RequestTransfer(1000000.0, 0.0);
    ImGui::SameLine();
    if (ImGui::SmallButton("+100 тыс. $")) TwoStates_RequestTransfer(0.0, 100000.0);
}

static void SteamFriendsPanel(std::string& hostIp)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!SteamP2P_IsAvailable()) {
        Wrapped(kYellow, "Steam P2P: запустите клиент Steam для игры через друзей/Steam ID без VPN. Подключение по IP (127.0.0.1 / LAN / Radmin) работает и без Steam.");
        return;
    }

    uint64_t mySteamId = SteamP2P_GetLocalSteamId();
    const char* myName = SteamP2P_GetLocalPersonaName();
    ImGui::TextColored(kGreen, "Steam P2P готов (без VPN и проброса портов)");
    ImGui::Text("Вы: %s  |  Steam ID: %llu", myName[0] ? myName : "Игрок", (unsigned long long)mySteamId);
    ImGui::SameLine();
    if (ImGui::SmallButton("Скопировать ID")) {
        char idStr[32] = {};
        _snprintf_s(idStr, sizeof(idStr), _TRUNCATE, "%llu", (unsigned long long)mySteamId);
        ImGui::SetClipboardText(idStr);
    }

    static std::vector<SteamFriendEntry> s_friends;
    static ULONGLONG s_lastRefresh = 0;
    ULONGLONG now = GetTickCount64();
    bool refresh = (now - s_lastRefresh > 2500);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kYellow, "Друзья в Steam онлайн (%zu):", s_friends.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Обновить список")) refresh = true;

    if (refresh) {
        SteamP2P_GetOnlineFriends(s_friends);
        s_lastRefresh = now;
    }

    SessionState st = SaveSync_GetState();
    bool world = SaveSync_IsWorldLoaded();

    if (s_friends.empty()) {
        ImGui::TextColored(kGrey, "Нет друзей в сети (или профиль скрыт). Вы можете подключиться по Steam ID ниже.");
        return;
    }

    float listHeight = s_friends.size() > 4 ? 115.0f : (float)s_friends.size() * 28.0f + 12.0f;
    if (ImGui::BeginChild("steam_friends", ImVec2(-1, listHeight), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < s_friends.size(); ++i) {
            const SteamFriendEntry& f = s_friends[i];
            ImGui::PushID((int)i);
            StatusDot(f.inThisGame ? kGreen : kBlue);
            ImGui::Text("%s %s", f.name, f.inThisGame ? "[В игре W&R]" : "[В сети]");

            if (st == SESSION_IDLE) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Подключиться")) {
                    char target[48] = {};
                    _snprintf_s(target, sizeof(target), _TRUNCATE, "steam:%llu", (unsigned long long)f.steamId);
                    hostIp = target;
                    SaveSync_StartClient(hostIp.c_str());
                }
                if (world) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Пригласить (Host)")) {
                        SaveSync_StartHost();
                        SteamP2P_InviteFriend(f.steamId);
                    }
                }
            } else if (st == SESSION_HOST_LISTENING) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Пригласить в игру")) {
                    SteamP2P_InviteFriend(f.steamId);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// Host/Connect while idle; Stop hosting / Disconnect during a session.
static void SessionButtons(std::string& hostIp)
{
    SessionState st = SaveSync_GetState();
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;

    if (st == SESSION_IDLE) {
        // --- Gameplay mode selection (One State vs Two States) ---
        ImGui::TextColored(kYellow, "Режим совместной игры (выбор Хоста):");
        int coopMode = TwoStates_GetMode();
        if (ImGui::RadioButton("Одно государство (общая казна и карта)", coopMode == COOP_MODE_ONE_STATE)) {
            TwoStates_SetMode(COOP_MODE_ONE_STATE);
        }
        if (ImGui::RadioButton("Два государства на одной карте (Западная ССР / Восточная ССР)", coopMode == COOP_MODE_TWO_STATES)) {
            TwoStates_SetMode(COOP_MODE_TWO_STATES);
            SaveSync_SetLockstep(true);
        }
        if (TwoStates_GetMode() == COOP_MODE_TWO_STATES) {
            float borderHalf = TwoStates_GetBorderHalfWidth();
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("Пограничная зона X=0 (±м)", &borderHalf, 50.0f, 1000.0f, "%.0f м"))
                TwoStates_SetBorderHalfWidth(borderHalf);
            bool fullCap = TwoStates_GetFullStartingCapital();
            if (ImGui::Checkbox("100% стартового капитала каждому государству (иначе 50/50)", &fullCap))
                TwoStates_SetFullStartingCapital(fullCap);
            Wrapped(kGrey, "Западная ССР (Хост, X < 0) и Восточная ССР (Игрок 2, X >= 0): раздельные казны (₽ и $), защита территории и чужих зданий, стыковка дорог/ЖД/ЛЭП в пограничной полосе.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Map Expansion Options ---
        ImGui::TextColored(kYellow, "Увеличение размера карты:");
        bool expandBorders = TwoStates_GetExpandBorders();
        if (ImGui::Checkbox("Снять 2-км ограничение у краёв карты (20x20 км целиком, +56% площади)", &expandBorders))
            TwoStates_SetExpandBorders(expandBorders);
        bool world = SaveSync_IsWorldLoaded();
        bool creatingMap = TwoStates_IsCreatingExpandedMap();
        ImGui::BeginDisabled(!world || creatingMap);
        if (ImGui::Button(creatingMap ? "Создание карты 30x30 км..." : "Создать и загрузить карту 30x30 км (900 км²)", ImVec2(-1, 0))) {
            TwoStates_RequestCreateExpandedMap(30);
        }
        ImGui::EndDisabled();
        if (!world && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Сначала загрузите любой город или карту, чтобы создать на её основе расширенную карту 30x30 км.");

        SteamFriendsPanel(hostIp);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // The host picks the mode; the joining player follows it.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Синхронизация:");
        ImGui::SameLine();
        bool lockstep = SaveSync_GetLockstep();
        if (ImGui::RadioButton("Lockstep (рекомендуется)", lockstep)) SaveSync_SetLockstep(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Оба ПК симулируют одни и те же тики без рассинхрона.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Replay", !lockstep)) SaveSync_SetLockstep(false);
        bool dlcCheck = SaveSync_GetDlcCheck();
        if (ImGui::Checkbox("Проверять совпадение DLC (Check DLCs)", &dlcCheck)) SaveSync_SetDlcCheck(dlcCheck);
        ImGui::Spacing();

        ImGui::BeginDisabled(!world);
        if (ImGui::Button("Создать сервер (Host: Steam + IP)", ImVec2(half, 0)))
            SaveSync_StartHost();
        ImGui::EndDisabled();
        if (!world && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Сначала загрузите город/карту.");
        ImGui::SameLine();
        ImGui::BeginDisabled(hostIp.empty());
        if (ImGui::Button("Подключиться (Connect)", ImVec2(half, 0)))
            SaveSync_StartClient(hostIp.c_str());
        ImGui::EndDisabled();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Steam ID или IP Хоста:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Вставить")) {
            const char* clip = ImGui::GetClipboardText();
            if (clip && clip[0]) hostIp = clip;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##hostIp", &hostIp);
        if (!world) Wrapped(kGrey, "Загрузите карту или сохранение, чтобы создать сервер (Host).");
        return;
    }

    if (st == SESSION_HOST_LISTENING) {
        SteamFriendsPanel(hostIp);
    }
    CoopComfortPanel();
    TwoStatesLivePanel();
    ImGui::Spacing();

    bool host = SaveSync_IsHostRole();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
    ImGui::BeginDisabled(SaveSync_GetSessionAgeMs() < SAVE_SYNC_STOP_BUTTON_DELAY_MS);
    if (ImGui::Button(host ? "Остановить сервер (Stop hosting)" : "Отключиться (Disconnect)", ImVec2(-1, 0)))
        SaveSync_RequestDisconnect();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
}

void MainMenu::Draw()
{
    if (!hostIpInitialized) {
        hostIp = SaveSync_GetTargetIp(); // prefill with the default host IP
        hostIpInitialized = true;
    }

    ImGui::SetNextWindowSize(ImVec2(500, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("RepublicMP — Кооператив (Одно / Два государства)"))
    {
        StatusPanel();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        SessionButtons(hostIp);

        ImGui::Spacing();
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        if (ImGui::Button("Скрыть меню (INSERT)", ImVec2(half, 0)))
            g_imguiVisible = false; // INSERT brings it back (game/imgui_hook.cpp)
        ImGui::SameLine();
        if (ImGui::Button("Отладка (Debug)", ImVec2(half, 0)))
            g_debug_window.Open();

        ImGui::TextColored(kGrey, "RepublicMP v%s  build %08X", SaveSync_GetLocalVersion(), SaveSync_GetLocalDllCrc());
        ImGui::SameLine();
        if (ImGui::SmallButton("Проверить обновления"))
            Updater_CheckNow();

        UpdateStatus ust = Updater_GetStatus();
        const char* umsg = Updater_GetStatusMessage();
        if (ust == UPDATE_AVAILABLE_GITHUB || ust == UPDATE_AVAILABLE_WORKSHOP) {
            Wrapped(kGreen, "%s", umsg);
            if (ImGui::Button("Скачать и установить обновление сейчас", ImVec2(-1, 0)))
                Updater_DownloadAndApply();
        } else if (ust == UPDATE_APPLIED_RESTART_NEEDED) {
            Wrapped(kYellow, "%s", umsg);
        } else if (ust == UPDATE_CHECKING || ust == UPDATE_DOWNLOADING) {
            Wrapped(kBlue, "%s", umsg);
        } else if (ust == UPDATE_ERROR) {
            Wrapped(kRed, "%s", umsg);
        } else if (umsg && umsg[0]) {
            Wrapped(kGrey, "%s", umsg);
        }

        if (ImGui::TreeNode("Настройки автообновления (GitHub Releases / Steam Workshop)")) {
            static std::string repoBuf;
            static bool repoInit = false;
            if (!repoInit) {
                repoBuf = Updater_GetGithubRepo();
                repoInit = true;
            }
            ImGui::SetNextItemWidth(260);
            ImGui::InputText("GitHub (owner/repo)", &repoBuf);
            ImGui::SameLine();
            if (ImGui::SmallButton("Сохранить")) {
                Updater_SetGithubRepo(repoBuf.c_str());
                Updater_CheckNow();
            }
            ImGui::TreePop();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Everyday log; verbose sync logs are in the Debug window ---
        ImGui::TextUnformatted("Журнал событий (Log):");
        ImGui::SameLine();
        if (ImGui::SmallButton("Очистить"))
            UiLog_Clear();
        ImGui::SameLine();
        ImGui::Checkbox("Автопрокрутка", &autoScrollLog);

        if (ImGui::BeginChild("log", { -1, -1 }, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoSavedSettings))
        {
            std::vector<std::string> lines = UiLog_Snapshot();
            for (const std::string& line : lines)
                ImGui::TextWrapped("%s", line.c_str());

            if (autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
