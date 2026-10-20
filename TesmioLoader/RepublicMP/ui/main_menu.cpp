// SPDX-License-Identifier: GPL-3.0-only
#include "main_menu.h"
#include "lang.h"
#include "ui_log.h"
#include "sync_block.h"
#include "debug_window.h"
#include "../session/save_sync.h"
#include "../session/sync_check.h"
#include "../session/two_states.h"
#include "../session/updater.h"
#include "../transport/steam_p2p.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../determinism/determinism.h"
#include "../lockstep/lockstep_net.h"
#include "../game/imgui_hook.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static const ImVec4 kRed    = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
static const ImVec4 kYellow = ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
static const ImVec4 kGreen  = ImVec4(0.35f, 0.90f, 0.45f, 1.0f);
static const ImVec4 kGrey   = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
static const ImVec4 kCyan   = ImVec4(0.40f, 0.85f, 1.00f, 1.0f);
static const ImVec4 kGold   = ImVec4(1.00f, 0.78f, 0.35f, 1.0f);

static void Dot(const ImVec4& col)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    dl->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.32f,
                        ImGui::ColorConvertFloat4ToU32(col));
    ImGui::Dummy(ImVec2(h * 0.9f, h));
    ImGui::SameLine();
}

static void StatusPanel(SessionState st)
{
    const char* peer = SaveSync_GetPeerIp();
    ImVec4 col = kGrey;
    char headline[128] = {};
    const char* hint = nullptr;

    switch (st) {
    case SESSION_IDLE:
        col = kGrey;
        _snprintf_s(headline, sizeof(headline), _TRUNCATE, "%s",
                    Tr("Не подключено", "Not connected"));
        hint = SaveSync_IsWorldLoaded()
            ? Tr("Выберите режим игры и нажмите «Создать сервер», либо подключитесь к другу.",
                 "Select a game mode and click 'Start Server', or connect to a friend.")
            : Tr("Выберите сохранение прямо здесь для запуска сервера, либо подключитесь к другу.",
                 "Pick a save right here to start hosting, or connect to a friend.");
        break;
    case SESSION_HOST_LISTENING:
        col = kYellow;
        _snprintf_s(headline, sizeof(headline), _TRUNCATE,
                    Tr("Ожидание второго игрока (порт %d + Steam P2P)", "Waiting for player 2 (port %d + Steam P2P)"),
                    SAVE_SYNC_PORT);
        hint = Tr("Друг может войти в 1 клик через список друзей Steam ниже или по вашему IP.",
                  "Your friend can join in 1 click via the Steam Friends list below or by IP.");
        break;
    case SESSION_HOST_JOINING:
        col = kYellow;
        _snprintf_s(headline, sizeof(headline), _TRUNCATE,
                    SaveSync_IsResyncing()
                        ? Tr("Пересинхронизация мира с %s...", "Re-syncing the world with %s...")
                        : Tr("Игрок %s подключается — передача сохранения...", "Player %s is joining — sending save..."),
                    peer[0] ? peer : Tr("игроком", "player"));
        hint = Tr("Обе игры поставлены на паузу до завершения загрузки мира.",
                  "Both games are paused until the world finishes loading.");
        break;
    case SESSION_HOST_IN_GAME:
        col = kGreen;
        _snprintf_s(headline, sizeof(headline), _TRUNCATE,
                    Tr("В игре (Хост) — партнёр: %s", "In game (Host) — partner: %s"),
                    peer[0] ? peer : Tr("игрок", "player"));
        break;
    case SESSION_CLIENT_JOINING:
        col = kYellow;
        _snprintf_s(headline, sizeof(headline), _TRUNCATE,
                    SaveSync_IsResyncing()
                        ? Tr("Пересинхронизация с хостом %s...", "Re-syncing with host %s...")
                        : Tr("Подключение к %s:%d...", "Connecting to %s:%d..."),
                    peer[0] ? peer : "?", SAVE_SYNC_PORT);
        hint = Tr("Скачивание и загрузка мира хоста — игра временно на паузе.",
                  "Downloading and loading the host's world — game is paused.");
        break;
    case SESSION_CLIENT_IN_GAME:
        col = kGreen;
        _snprintf_s(headline, sizeof(headline), _TRUNCATE,
                    Tr("В игре (Клиент) — сервер: %s", "In game (Client) — server: %s"),
                    peer[0] ? peer : Tr("хост", "host"));
        break;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(col.x * 0.18f, col.y * 0.18f, col.z * 0.18f, 0.85f));
    ImGui::BeginChild("##status", ImVec2(0, hint ? 52.0f : 32.0f), true, ImGuiWindowFlags_NoScrollbar);
    Dot(col);
    ImGui::TextColored(col, "%s", headline);
    if (hint) {
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    const char* err = SaveSync_GetLastError();
    if (err[0]) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.10f, 0.10f, 0.90f));
        ImGui::BeginChild("##last_err", ImVec2(0, 44.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(kRed, "%s %s", Tr("Последняя ошибка:", "Last error:"), err);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

static void LockstepPanel(SessionState st)
{
    if (st != SESSION_HOST_IN_GAME && st != SESSION_CLIENT_IN_GAME) return;
    if (!SaveSync_IsLockstepSession()) return;

    const char* desync = LockstepNet_Desync();
    bool ok = !desync[0];
    ImGui::TextDisabled("%s", Tr("Синхронизация симуляции:", "Simulation sync:"));
    ImGui::SameLine();
    Dot(ok ? kGreen : kRed);
    if (ok) {
        ImGui::TextColored(kGreen, Tr("Lockstep (шаг %u)", "Lockstep (step %u)"), Determinism_StepCount());
    } else {
        ImGui::TextColored(kRed, "%s", desync);
    }
    unsigned long long rtt = 0;
    if (LockstepNet_RoundTrip(&rtt)) {
        ImGui::SameLine();
        ImGui::TextDisabled(Tr("  Пинг: %llu мс", "  Ping: %llu ms"), rtt);
    }

    if (st != SESSION_HOST_IN_GAME) return;

    static const int kWaitSeconds[] = { -1, 0, 30, 60, 120, 300 };
    static const char* const kWaitLabelsRu[] = { "Выкл", "Сразу", "Через 30 сек", "Через 1 мин", "Через 2 мин", "Через 5 мин" };
    static const char* const kWaitLabelsEn[] = { "Off", "Immediately", "After 30 sec", "After 1 min", "After 2 min", "After 5 min" };
    int cur = SaveSync_GetDesyncResyncSeconds(), idx = 3;
    for (int i = 0; i < 6; i++)
        if (kWaitSeconds[i] == cur) idx = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Tr("Авто-ресинк при рассинхроне:", "Auto re-sync on desync:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    const char* const* labels = (Lang_Get() == MP_LANG_EN) ? kWaitLabelsEn : kWaitLabelsRu;
    if (ImGui::Combo("##lsresync", &idx, labels, 6))
        SaveSync_SetDesyncResyncSeconds(kWaitSeconds[idx]);
    ULONGLONG left = SaveSync_GetMsUntilDesyncResync();
    if (left) {
        ImGui::SameLine();
        ImGui::TextColored(kYellow, Tr("через %llus", "in %llus"), (left + 999) / 1000);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Пересинхронизировать мир", "Re-sync World Now")))
        SaveSync_RequestResync();
}

static void ReplayResyncControls(SessionState st)
{
    if (st != SESSION_HOST_IN_GAME) return;

    ImGui::Spacing();
    if (ImGui::Button(Tr("Пересинхронизировать мир сейчас", "Re-sync World Now")))
        SaveSync_RequestResync();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr("Сохранить ваш мир и заново отправить его второму игроку.",
                                   "Save your world and send it to the second player again."));

    static const int kIntervals[] = { 0, 5, 10, 15, 30 };
    static const char* const kLabelsRu[] = { "Выкл", "Каждые 5 мин", "Каждые 10 мин", "Каждые 15 мин", "Каждые 30 мин" };
    static const char* const kLabelsEn[] = { "Off", "Every 5 min", "Every 10 min", "Every 15 min", "Every 30 min" };
    int cur = SaveSync_GetAutoResyncMinutes();
    int idx = 0;
    for (int i = 0; i < 5; i++)
        if (kIntervals[i] == cur) idx = i;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("По таймеру:", "Timer:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(135.0f);
    const char* const* labels = (Lang_Get() == MP_LANG_EN) ? kLabelsEn : kLabelsRu;
    if (ImGui::Combo("##autoresync", &idx, labels, 5))
        SaveSync_SetAutoResyncMinutes(kIntervals[idx]);
    ULONGLONG leftMs = SaveSync_GetMsUntilAutoResync();
    if (leftMs) {
        unsigned secs = (unsigned)(leftMs / 1000);
        ImGui::SameLine();
        ImGui::TextDisabled("(%u:%02u)", secs / 60, secs % 60);
    }
}

static void ChannelIndicators(SessionState st)
{
    if (st != SESSION_HOST_IN_GAME && st != SESSION_CLIENT_IN_GAME) return;
    if (SaveSync_IsLockstepSession()) {
        LockstepPanel(st);
        return;
    }

    struct Chan { const char* name; bool up; } chans[] = {
        { Tr("Постройки", "Buildings"), BuildSync_IsConnected() },
        { Tr("Дороги и сети", "Roads & Networks"), RoadSync_IsConnected() },
        { Tr("Терраформинг", "Terraforming"), TerrainSync_IsConnected() },
    };
    ImGui::TextDisabled("%s", Tr("Каналы синхронизации:", "Sync channels:"));
    for (const Chan& c : chans) {
        ImGui::SameLine(0.0f, 14.0f);
        Dot(c.up ? kGreen : kRed);
        ImGui::TextColored(c.up ? kGreen : kRed, "%s", c.name);
    }
    ReplayResyncControls(st);
}

static void JoinStepLine(SessionState st)
{
    if (st != SESSION_HOST_JOINING && st != SESSION_CLIENT_JOINING) return;
    int step = 0, total = 0, pct = -1;
    char text[160] = {};
    if (!SyncBlock_GetStep(&step, &total, text, sizeof(text), &pct)) return;
    if (total > 0 && pct >= 0)
        ImGui::TextColored(kYellow, Tr("Шаг %d/%d: %s (%d%%)", "Step %d/%d: %s (%d%%)"), step, total, text, pct);
    else if (total > 0)
        ImGui::TextColored(kYellow, Tr("Шаг %d/%d: %s", "Step %d/%d: %s"), step, total, text);
    else
        ImGui::TextColored(kYellow, "%s", text);
}

static void CoopComfortPanel(SessionState st)
{
    bool inGame = (st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME);
    if (!inGame && !SaveSync_IsWorldLoaded()) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.14f, 0.19f, 0.88f));
    ImGui::BeginChild("##coop_comfort_bar", ImVec2(0, 62.0f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(kCyan, "%s", Tr("Удобство и навигация в коопе:", "Co-op Comfort & Navigation:"));
    ImGui::SameLine();
    bool miniHud = TwoStates_GetShowMiniHud();
    if (ImGui::Checkbox(Tr("Верхняя мини-панель (Mini-HUD)", "Top Mini-HUD Bar"), &miniHud))
        TwoStates_SetShowMiniHud(miniHud);
    if (TwoStates_GetMode() == COOP_MODE_TWO_STATES || TwoStates_IsActive()) {
        ImGui::SameLine();
        bool showBorder = TwoStates_GetShow3DBorder();
        if (ImGui::Checkbox(Tr("3D-граница на земле [F7]", "3D Ground Border [F7]"), &showBorder))
            TwoStates_SetShow3DBorder(showBorder);
    }

    if (ImGui::Button(Tr("К камере / сигналу друга [F6]", "Jump to Partner / Ping [F6]")))
        TwoStates_JumpCameraToPartner();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr("Мгновенно переместить вашу камеру к курсору партнёра или к его последнему сигналу на карте.",
                                   "Instantly move your camera to your partner's cursor or latest map ping."));
    ImGui::SameLine();
    if (ImGui::Button(Tr("Подать сигнал на карте [Alt+ЛКМ]", "Send Map Ping [Alt+LMB]")))
        TwoStates_PingAtCursor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr("Поставить яркий 3D-маяк в точке курсора (или в центре экрана), который увидит ваш друг.",
                                   "Place a bright 3D beacon at your cursor (or screen center) visible to your friend."));
    if (TwoStates_GetMode() == COOP_MODE_TWO_STATES || TwoStates_IsActive()) {
        ImGui::SameLine();
        if (ImGui::Button(Tr("Камера на границу (X=0)", "Camera to Border (X=0)"))) {
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            TwoStates_GetCameraLookAt(&lx, &ly, &lz);
            TwoStates_JumpCameraTo(0.0f, lz);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void TwoStatesLivePanel(SessionState st)
{
    if ((st != SESSION_HOST_IN_GAME && st != SESSION_CLIENT_IN_GAME) || !TwoStates_IsActive())
        return;

    int me = TwoStates_LocalState();
    float borderW = TwoStates_GetBorderHalfWidth();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.18f, 0.24f, 0.92f));
    ImGui::BeginChild("##twostates_live", ImVec2(0, 178.0f), true, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(kGold, Tr("Режим «Два государства» — Вы управляете: %s",
                                 "Two States Mode — You control: %s"),
                       TwoStates_StateName(me));
    ImGui::SameLine();
    ImGui::TextDisabled(Tr("(Погранзона: X от -%.0f м до +%.0f м)",
                           "(Border Zone: X from -%.0f m to +%.0f m)"),
                        borderW, borderW);

    // Cursor territory indicator
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    if (TwoStates_GetCursorWorldPos(&cx, &cy, &cz)) {
        int zone = TwoStates_GetZoneAt(cx, cz);
        ImGui::TextDisabled(Tr("Курсор (X=%.0f, Z=%.0f):", "Cursor (X=%.0f, Z=%.0f):"), cx, cz);
        ImGui::SameLine();
        if (zone == 0) {
            ImGui::TextColored(kYellow, "%s",
                               Tr("Общая пограничная зона (строить могут оба государства)",
                                  "Shared Border Zone (both states can build here)"));
        } else if (zone == me) {
            ImGui::TextColored(kGreen, Tr("Ваша суверенная территория (%s)", "Your sovereign territory (%s)"),
                               TwoStates_StateName(zone));
        } else {
            ImGui::TextColored(kRed, Tr("Чужая территория (%s — строительство запрещено)",
                                        "Partner's territory (%s — building forbidden)"),
                               TwoStates_StateName(zone));
        }
    }

    ImGui::Separator();

    // Dual Treasury Table
    ImGui::TextColored(me == 1 ? kCyan : kGrey,
                       Tr("Западная ССР (Хост):    %12.0f ₽  |  %12.0f $   (торговля/таможня: %+.0f ₽ / %+.0f $)",
                          "West SSR (Host):        %12.0f RUB | %12.0f USD  (trade/customs: %+.0f RUB / %+.0f USD)"),
                       TwoStates_GetRubles(1), TwoStates_GetDollars(1),
                       TwoStates_GetNetTradeRub(1), TwoStates_GetNetTradeUsd(1));
    ImGui::TextColored(me == 2 ? kCyan : kGrey,
                       Tr("Восточная ССР (Игрок 2): %12.0f ₽  |  %12.0f $   (торговля/таможня: %+.0f ₽ / %+.0f $)",
                          "East SSR (Player 2):    %12.0f RUB | %12.0f USD  (trade/customs: %+.0f RUB / %+.0f USD)"),
                       TwoStates_GetRubles(2), TwoStates_GetDollars(2),
                       TwoStates_GetNetTradeRub(2), TwoStates_GetNetTradeUsd(2));

    ImGui::Spacing();
    static float s_sendRub = 50000.0f;
    static float s_sendUsd = 0.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Tr("Межгосударственный перевод:", "Inter-state transfer:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115.0f);
    ImGui::InputFloat("₽##tr_rub", &s_sendRub, 10000.0f, 100000.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115.0f);
    ImGui::InputFloat("$##tr_usd", &s_sendUsd, 10000.0f, 100000.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::Button(Tr("Отправить партнёру", "Send to Partner"))) {
        if (s_sendRub > 0.0f || s_sendUsd > 0.0f) {
            TwoStates_RequestTransfer((double)s_sendRub, (double)s_sendUsd);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          Tr("Мгновенный перевод рублей и/или долларов из вашей казны во второе государство (засчитывается как оплата за ресурсы/электроэнергию/помощь).",
                             "Instant transfer of Rubles and/or Dollars from your treasury to the partner state (for resources, power, or aid)."));
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void SteamFriendsPanel(bool canConnect, char* ipBuf, size_t ipBufSize)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.16f, 0.22f, 0.90f));
    ImGui::BeginChild("##steam_p2p_panel", ImVec2(0, 128.0f), true);

    if (!SteamP2P_IsAvailable()) {
        ImGui::TextColored(kYellow, "%s",
                           Tr("Steam P2P: клиент Steam не обнаружен (доступно прямое подключение по IP / Radmin / ZeroTier)",
                              "Steam P2P: Steam client not detected (direct connection via IP / Radmin / ZeroTier is available)"));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::TextColored(kCyan, Tr("Steam P2P активен: %s (SteamID: %llu)",
                                 "Steam P2P Active: %s (SteamID: %llu)"),
                       SteamP2P_GetLocalPersonaName(), (unsigned long long)SteamP2P_GetLocalSteamId());
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Копировать мой код", "Copy My Code"))) {
        char code[64];
        _snprintf_s(code, sizeof(code), _TRUNCATE, "steam:%llu", (unsigned long long)SteamP2P_GetLocalSteamId());
        ImGui::SetClipboardText(code);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr("Скопировать ваш адрес steam:<SteamID> в буфер обмена (можно также приглашать через Shift+Tab в Steam)",
                                   "Copy your steam:<SteamID> address to clipboard (you can also invite via Shift+Tab Steam Overlay)"));

    static std::vector<SteamFriendEntry> s_friends;
    static ULONGLONG s_lastRefresh = 0;
    ULONGLONG now = GetTickCount64();
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Обновить список друзей", "Refresh Friends")) || now - s_lastRefresh > 3000) {
        s_lastRefresh = now;
        SteamP2P_GetOnlineFriends(s_friends);
    }

    if (s_friends.empty()) {
        ImGui::TextDisabled("%s",
                            Tr("Нет друзей онлайн в списке Steam (вы можете ввести steam:<SteamID> вручную или пригласить через Shift+Tab).",
                               "No online friends found in Steam list (you can type steam:<SteamID> manually or invite via Shift+Tab)."));
    } else {
        ImGui::TextDisabled("%s", Tr("Друзья в Steam (нажмите «Подключиться» к другу, который создал сервер):",
                                     "Steam Friends (click 'Connect' next to a friend who is hosting):"));
        for (size_t i = 0; i < s_friends.size(); ++i) {
            const auto& f = s_friends[i];
            ImGui::PushID((int)i);
            bool hosting = (f.connectString[0] != 0);
            if (hosting) {
                Dot(kGreen);
                ImGui::TextColored(kGreen, Tr("%s  [Сервер открыт]", "%s  [Server Open]"), f.name);
            } else if (f.inThisGame) {
                Dot(kYellow);
                ImGui::TextColored(kYellow, Tr("%s  [В игре Soviet Republic]", "%s  [In Soviet Republic]"), f.name);
            } else {
                Dot(kGrey);
                ImGui::TextDisabled("%s", f.name);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!canConnect);
            if (ImGui::SmallButton(Tr("Подключиться по Steam", "Connect via Steam"))) {
                _snprintf_s(ipBuf, ipBufSize, _TRUNCATE, "steam:%llu", (unsigned long long)f.steamId);
                SaveSync_StartClient(ipBuf);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("Пригласить", "Invite"))) {
                SteamP2P_InviteFriend(f.steamId);
            }
            ImGui::PopID();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void SessionButtons(SessionState st, char* ipBuf, size_t ipBufSize)
{
    bool busy = SaveSync_IsBusy();
    bool worldLoaded = SaveSync_IsWorldLoaded();
    float btnH = ImGui::GetFrameHeight() * 1.35f;

    if (!busy) {
        // --- Game Mode Selection Box ---
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.15f, 0.20f, 0.90f));
        ImGui::BeginChild("##coop_mode_select", ImVec2(0, 172.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(kGold, "%s", Tr("Режим кооперативной игры (выбирает Хост):",
                                           "Co-op Game Mode (selected by Host):"));

        int mode = TwoStates_GetMode();
        if (ImGui::RadioButton(Tr("Одно государство (общая казна и единое управление всей картой)",
                                  "One State (shared treasury & joint control of the whole map)"),
                               mode == COOP_MODE_ONE_STATE)) {
            TwoStates_SetMode(COOP_MODE_ONE_STATE);
            mode = COOP_MODE_ONE_STATE;
        }
        if (ImGui::RadioButton(Tr("Два государства (Западная ССР vs Восточная ССР — раздельные бюджеты и границы)",
                                  "Two States (West SSR vs East SSR — separate budgets & borders)"),
                               mode == COOP_MODE_TWO_STATES)) {
            TwoStates_SetMode(COOP_MODE_TWO_STATES);
            SaveSync_SetLockstep(true);
            mode = COOP_MODE_TWO_STATES;
        }

        if (mode == COOP_MODE_TWO_STATES) {
            float halfW = TwoStates_GetBorderHalfWidth();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderFloat(Tr("Пограничная зона (±м от X=0)", "Border Zone (±m from X=0)"),
                                   &halfW, 100.0f, 1500.0f, "%.0f m")) {
                TwoStates_SetBorderHalfWidth(halfW);
            }
            ImGui::SameLine();
            bool fullCap = TwoStates_GetFullStartingCapital();
            if (ImGui::Checkbox(Tr("100% стартового капитала каждому", "100% starting capital for each"), &fullCap)) {
                TwoStates_SetFullStartingCapital(fullCap);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                                  Tr("Вкл: каждое государство получает полный стартовый бюджет карты.\nВыкл: текущий бюджет карты делится 50/50.",
                                     "On: each state receives 100% of the map's starting budget.\nOff: the map's budget is split 50/50."));
            }
        } else {
            ImGui::TextDisabled("%s",
                                Tr("Оба игрока строят и управляют одной общей республикой без территориальных ограничений.",
                                   "Both players build and manage a single shared republic without territory restrictions."));
        }

        // Map Expansion Options
        bool expandBorders = TwoStates_GetExpandBorders();
        if (ImGui::Checkbox(Tr("Снять 2-км буфер у краёв карты (доступна вся площадь 20×20 км = 388 км²)",
                               "Unlock 2-km map edge buffer (full 20×20 km = 388 km² buildable area)"),
                            &expandBorders)) {
            TwoStates_SetExpandBorders(expandBorders);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              Tr("По умолчанию игра запрещает строить ближе 2 км от края карты (оставляя лишь 16×16 км).\nЭта опция открывает строительство вплоть до самых таможен на обоих компьютерах!",
                                 "By default the game blocks building within 2 km of map edges (leaving only 16×16 km).\nThis option unlocks building right up to the customs houses on both PCs!"));
        }
        ImGui::SameLine();
        bool creatingMap = TwoStates_IsCreatingExpandedMap();
        ImGui::BeginDisabled(!worldLoaded || creatingMap);
        if (ImGui::SmallButton(creatingMap
                               ? Tr("Создание карты 30×30 км...", "Creating 30×30 km map...")
                               : Tr("Создать карту 30×30 км (900 км²)", "Create 30×30 km Map (900 km²)"))) {
            TwoStates_RequestCreateExpandedMap(30);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s",
                              Tr("Автоматически сохраняет текущую карту как «TwoStates_30x30km», расширяет её масштаб до 30×30 км (900 км², в 3.5 раза больше стандартной!) и загружает её.",
                                 "Saves the current map as 'TwoStates_30x30km', expands its world scale to 30×30 km (900 km², 3.5x larger than standard!) and loads it."));
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        bool lockstep = SaveSync_GetLockstep();
        if (mode == COOP_MODE_TWO_STATES) {
            lockstep = true;
            SaveSync_SetLockstep(true);
        }
        ImGui::BeginDisabled(mode == COOP_MODE_TWO_STATES);
        if (ImGui::Checkbox(Tr("Полная синхронизация симуляции (Lockstep — транспорт, жители, экономика)",
                               "Full simulation sync (Lockstep — vehicles, citizens, economy)"),
                            &lockstep))
            SaveSync_SetLockstep(lockstep);
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 18.0f);
        bool dlcCheck = SaveSync_GetDlcCheck();
        if (ImGui::Checkbox(Tr("Проверять совпадение DLC", "Verify matching DLCs"), &dlcCheck))
            SaveSync_SetDlcCheck(dlcCheck);
    }

    // Host row (works both in-game AND directly from the Main Menu by picking a save)
    bool canHostNow = !busy && worldLoaded && !SaveSync_IsPendingHostAfterLoad();
    if (worldLoaded) {
        ImGui::BeginDisabled(!canHostNow);
        if (ImGui::Button(Tr("Создать сервер (Хост текущего мира)", "Start Server (Host Current World)"), ImVec2(290.0f, btnH)))
            SaveSync_StartHost();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Tr("Мир загружен — можно открывать сервер", "World loaded — ready to host"));
    } else {
        static std::vector<std::string> s_saves;
        static int s_selectedSave = 0;
        static ULONGLONG s_lastSaveScan = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastSaveScan > 3000 || s_saves.empty()) {
            s_lastSaveScan = now;
            SaveSync_ListLocalSaves(s_saves);
            if (s_selectedSave >= (int)s_saves.size()) s_selectedSave = 0;
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.19f, 0.14f, 0.90f));
        ImGui::BeginChild("##quick_host_menu", ImVec2(0, 68.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(kGreen, "%s",
                           Tr("Быстрый запуск сервера прямо из главного меню:",
                              "Quick Server Launch directly from the Main Menu:"));
        if (s_saves.empty()) {
            ImGui::TextDisabled("%s",
                                Tr("Сохранения не найдены — начните новую игру в меню слева, затем нажмите «Создать сервер».",
                                   "No saves found — start a new game from the left menu, then click 'Start Server'."));
        } else {
            ImGui::SetNextItemWidth(260.0f);
            const char* preview = s_saves[s_selectedSave].c_str();
            if (ImGui::BeginCombo("##save_picker", preview)) {
                for (int i = 0; i < (int)s_saves.size(); ++i) {
                    bool sel = (i == s_selectedSave);
                    if (ImGui::Selectable(s_saves[i].c_str(), sel))
                        s_selectedSave = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            bool pending = SaveSync_IsPendingHostAfterLoad();
            ImGui::BeginDisabled(busy || pending);
            if (ImGui::Button(pending
                              ? Tr("Загрузка мира и запуск...", "Loading world & starting...")
                              : Tr("Загрузить мир и создать сервер", "Load Save & Start Server"))) {
                SaveSync_LoadAndStartHost(s_saves[s_selectedSave].c_str());
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("Обновить", "Refresh"))) {
                SaveSync_ListLocalSaves(s_saves);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    // Steam P2P Friends & 1-click Join Panel
    SteamFriendsPanel(!busy, ipBuf, ipBufSize);

    // Direct IP / SteamID row
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(Tr("Подключиться по адресу", "Connect to Address"), ImVec2(220.0f, btnH)))
        SaveSync_StartClient(ipBuf);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr("IP или steam:<ID>:", "IP or steam:<ID>:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##hostip", ipBuf, ipBufSize);
    ImGui::EndDisabled();

    // Stop / Disconnect row
    if (busy) {
        ImGui::Spacing();
        const char* stopLabel = Tr("Отключиться", "Disconnect");
        if (st == SESSION_HOST_LISTENING) stopLabel = Tr("Остановить сервер", "Stop Server");
        else if (st == SESSION_HOST_JOINING || st == SESSION_HOST_IN_GAME) stopLabel = Tr("Остановить сервер (отключить игрока)", "Stop Server (Disconnect Player)");
        else if (st == SESSION_CLIENT_JOINING) stopLabel = Tr("Отменить подключение", "Cancel Connection");

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
        if (ImGui::Button(stopLabel, ImVec2(290.0f, btnH)))
            SaveSync_RequestDisconnect();
        ImGui::PopStyleColor(2);
    }
}

void MainMenu::Draw()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.05f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(760.0f, 680.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 380.0f), ImVec2(vp->Size.x * 0.95f, vp->Size.y * 0.95f));

    if (!ImGui::Begin(Tr("Кооператив — Workers & Resources: Soviet Republic (RepublicMP)###RepublicMPMain",
                         "Co-op Multiplayer — Workers & Resources: Soviet Republic (RepublicMP)###RepublicMPMain"))) {
        ImGui::End();
        return;
    }

    // Header row: version + language selector [RU | EN] + Hide button
    ImGui::TextDisabled(Tr("Версия %s (протокол %d, CRC %08X)  |  Клавиша INSERT — скрыть/показать окно",
                           "Version %s (protocol %d, CRC %08X)  |  INSERT key — hide/show window"),
                        SaveSync_GetLocalVersion(), REPUBLICMP_PROTOCOL, SaveSync_GetLocalDllCrc());

    ImGui::SameLine(ImGui::GetWindowWidth() - 235.0f);
    bool isRu = (Lang_Get() == MP_LANG_RU);
    if (isRu) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.80f, 1.0f));
    if (ImGui::SmallButton("RU")) Lang_Set(MP_LANG_RU);
    if (isRu) ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 4.0f);
    if (!isRu) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.80f, 1.0f));
    if (ImGui::SmallButton("EN")) Lang_Set(MP_LANG_EN);
    if (!isRu) ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 10.0f);
    if (ImGui::SmallButton(Tr("Свернуть окно [Ins]", "Minimize [Ins]"))) {
        g_imguiVisible = false;
    }

    // Auto-updater bar (Steam Workshop & GitHub Releases)
    UpdateStatus updSt = Updater_GetStatus();
    if (updSt == UPDATE_AVAILABLE_GITHUB || updSt == UPDATE_AVAILABLE_WORKSHOP || updSt == UPDATE_APPLIED_RESTART_NEEDED) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.26f, 0.14f, 0.92f));
        ImGui::BeginChild("##updater_banner", ImVec2(0, 36.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(kGreen, "%s", Updater_GetStatusMessage());
        if (updSt != UPDATE_APPLIED_RESTART_NEEDED) {
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("Скачать и установить в 1 клик", "Download & Install in 1 Click"))) {
                Updater_DownloadAndApply();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    static char s_ip[64] = "";
    if (s_ip[0] == '\0')
        strncpy_s(s_ip, SaveSync_GetTargetIp(), _TRUNCATE);

    SessionState st = SaveSync_GetState();
    StatusPanel(st);
    JoinStepLine(st);
    ChannelIndicators(st);
    CoopComfortPanel(st);
    TwoStatesLivePanel(st);

    ImGui::Spacing();
    SessionButtons(st, s_ip, sizeof(s_ip));

    ImGui::Spacing();
    ImGui::Separator();

    // Event log header with Clear + Copy + Export Diagnostic Report buttons
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr("Журнал событий", "Event Log"));
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Очистить", "Clear")))
        UiLog_Clear();
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Скопировать диагностику в буфер", "Copy Diagnostics to Clipboard"))) {
        std::string rep = DiagLog_GetClipboardReport(); ImGui::SetClipboardText(rep.c_str());
        TwoStates_AddToast(Tr("Полный диагностический лог скопирован в буфер обмена (Ctrl+V)",
                              "Full diagnostic log copied to clipboard (Ctrl+V)"), 1);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr("Сохранить отчёт на Рабочий стол", "Save Report to Desktop"))) {
        std::string savedPath;
        if (DiagLog_ExportToDesktop(&savedPath)) {
            char msg[320];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        Tr("Отчёт сохранён: %s", "Report saved: %s"), savedPath.c_str());
            TwoStates_AddToast(msg, 1);
        }
    }

    float footerH = ImGui::GetFrameHeightWithSpacing() * 1.4f;
    ImGui::BeginChild("##uilog", ImVec2(0, -footerH), true, ImGuiWindowFlags_HorizontalScrollbar);
    std::vector<std::string> lines = UiLog_Snapshot();
    for (const std::string& line : lines)
        ImGui::TextWrapped("%s", line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (ImGui::CollapsingHeader(Tr("Обновления и диагностика (для разработчиков)",
                                   "Updates & Diagnostics (Developer Tools)"))) {
        static char s_repoBuf[128] = "";
        static bool s_repoInit = false;
        if (!s_repoInit) {
            strncpy_s(s_repoBuf, Updater_GetGithubRepo(), _TRUNCATE);
            s_repoInit = true;
        }
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText(Tr("GitHub репозиторий (owner/repo)", "GitHub Repository (owner/repo)"), s_repoBuf, sizeof(s_repoBuf))) {
            Updater_SetGithubRepo(s_repoBuf);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr("Проверить обновления", "Check for Updates"))) {
            Updater_CheckNow();
        }
        ImGui::TextDisabled(Tr("Статус обновлений: %s", "Update status: %s"), Updater_GetStatusMessage());
        ImGui::TextDisabled(Tr("Файл полной диагностики: %s", "Full diagnostics file: %s"), DiagLog_GetLogFilePath());
        if (ImGui::SmallButton(Tr("Открыть техническое окно отладки", "Open Technical Debug Window"))) {
            g_debug_window.Open();
        }
    }

    ImGui::End();
}

MainMenu g_mainMenu;
