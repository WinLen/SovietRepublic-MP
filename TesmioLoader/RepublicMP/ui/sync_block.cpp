// SPDX-License-Identifier: GPL-3.0-only
// See sync_block.h.
#include "sync_block.h"
#include "lang.h"
#include "../core/version.h"
#include "../session/game_speed.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../session/mod_sync.h"
#include "../transport/steam_p2p.h"
#include "imgui.h"
#include <windows.h>
#include <mutex>
#include <cstdio>
#include <cmath>

static std::mutex g_mutex;
static volatile bool g_active = false;
static volatile bool g_mapInspectMode = false;
static char g_text[160] = "Syncing...";
static int g_step = 0;
static int g_totalSteps = 0;
static int g_percent = -1;
static int g_queuedBytes = 0;
static int g_totalBytes = 0;
static float g_speedMbSec = 0.0f;

void SyncBlock_SetActive(bool active, const char* reason)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = active;
    if (active) {
        g_mapInspectMode = false;
        if (reason && reason[0]) {
            _snprintf_s(g_text, sizeof(g_text), _TRUNCATE, "%s", reason);
        }
    } else {
        g_step = g_totalSteps = 0;
        g_percent = -1;
        g_queuedBytes = 0;
        g_totalBytes = 0;
        g_speedMbSec = 0.0f;
    }
}

void SyncBlock_SetStep(int step, int totalSteps, const char* text, int percent)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_step = step;
    g_totalSteps = totalSteps;
    g_percent = percent;
    if (text && text[0]) {
        _snprintf_s(g_text, sizeof(g_text), _TRUNCATE, "%s", text);
    }
}

void SyncBlock_SetPercent(int percent)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_percent = percent;
    if (percent >= 100) {
        g_queuedBytes = 0;
    }
}

void SyncBlock_SetPercentEx(int percent, int queuedBytes, int totalBytes, float speedMbSec)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_percent = percent;
    g_queuedBytes = queuedBytes;
    g_totalBytes = totalBytes;
    g_speedMbSec = speedMbSec;
}

void SyncBlock_GetTransferStats(int* outPercent, int* outQueuedBytes, int* outTotalBytes, float* outSpeedMbSec)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (outPercent) *outPercent = g_percent;
    if (outQueuedBytes) *outQueuedBytes = g_queuedBytes;
    if (outTotalBytes) *outTotalBytes = g_totalBytes;
    if (outSpeedMbSec) *outSpeedMbSec = g_speedMbSec;
}

bool SyncBlock_IsActive()
{
    return g_active;
}

bool SyncBlock_IsMapInspectMode()
{
    return g_mapInspectMode;
}

void SyncBlock_SetMapInspectMode(bool inspect)
{
    if (g_active || SaveSync_GetState() != SESSION_HOST_LISTENING) {
        g_mapInspectMode = false;
        return;
    }
    g_mapInspectMode = inspect;
}

bool SyncBlock_UpdateMapInspectState(int sessionState, bool syncActive)
{
    if (syncActive || sessionState != (int)SESSION_HOST_LISTENING) {
        g_mapInspectMode = false;
    }
    return g_mapInspectMode;
}

bool SyncBlock_BlocksWorldInput()
{
    if (ModSync_IsDownloadActive()) return true;
    if (g_active) return true;
    if (SaveSync_IsWaitingLobbyActive() && !g_mapInspectMode) return true;
    return false;
}

static void CopyTextToClipboard(const char* text)
{
    if (!text || !text[0]) return;
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (hMem) {
        void* p = GlobalLock(hMem);
        if (p) {
            memcpy(p, text, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

static void PushSovietButtonStyle(bool primaryRed = false)
{
    if (primaryRed) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.54f, 0.14f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.19f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.44f, 0.10f, 0.09f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.24f, 0.19f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.28f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.14f, 0.10f, 1.0f));
    }
}

static void PopSovietButtonStyle()
{
    ImGui::PopStyleColor(3);
}

static void DrawMapInspectBanner(const ImGuiIO& io, bool isReconnect)
{
    float bannerW = 790.0f;
    if (bannerW > io.DisplaySize.x - 40.0f) bannerW = io.DisplaySize.x - 40.0f;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 44.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(bannerW, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.08f, 0.07f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.84f, 0.67f, 0.24f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##republicmp_inspect_banner", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.32f, 1.0f));
        ImGui::TextUnformatted(Tr("★ РЕЖИМ ОСМОТРА КАРТЫ — ИГРА НА ПАУЗЕ (СТРОИТЕЛЬСТВО И ТЕРРАФОРМИНГ ЗАБЛОКИРОВАНЫ) ★",
                                  "★ MAP INSPECTION MODE — GAME PAUSED (BUILDING & TERRAFORMING LOCKED) ★"));
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.84f, 0.78f, 1.0f));
        if (isReconnect) {
            const char* lastPeer = SaveSync_GetLastPartnerPeer();
            ImGui::Text(Tr("Партнёр отключился (%s). Ожидание переподключения... Камера: WASD / СКМ, Метка: Alt+ЛКМ.",
                           "Partner disconnected (%s). Waiting for reconnect... Camera: WASD / MMB, Ping: Alt+LMB."),
                        (lastPeer && lastPeer[0]) ? lastPeer : "peer");
        } else {
            ImGui::Text(Tr("Ожидание подключения второго игрока (Steam P2P / TCP :%d)... Камера: WASD / СКМ, Метка: Alt+ЛКМ.",
                           "Waiting for second player to connect (Steam P2P / TCP :%d)... Camera: WASD / MMB, Ping: Alt+LMB."),
                        SAVE_SYNC_PORT);
        }
        ImGui::PopStyleColor();

        ImGui::Spacing();
        PushSovietButtonStyle(false);
        if (ImGui::Button(Tr("Развернуть лобби [Tab]", "Expand Lobby [Tab]"), ImVec2(190.0f, 28.0f))) {
            SyncBlock_SetMapInspectMode(false);
        }
        ImGui::SameLine();
        bool b3d = TwoStates_GetShow3DBorder();
        char b3dLabel[64];
        _snprintf_s(b3dLabel, sizeof(b3dLabel), _TRUNCATE,
                    Tr("3D-Граница [F7]: %s", "3D Border [F7]: %s"),
                    b3d ? Tr("ВКЛ", "ON") : Tr("ВЫКЛ", "OFF"));
        if (ImGui::Button(b3dLabel, ImVec2(175.0f, 28.0f))) {
            TwoStates_SetShow3DBorder(!b3d);
        }
        if (isReconnect) {
            ImGui::SameLine();
            if (ImGui::Button(Tr("Сохранить кооп-игру", "Save Co-op Game"), ImVec2(180.0f, 28.0f))) {
                if (TwoStates_TriggerQuickCoopSave()) {
                    TwoStates_AddToast(Tr("Кооп-сохранение записано в банк коопа",
                                          "Co-op save written to Co-op bank"), 1);
                }
            }
        }
        PopSovietButtonStyle();

        ImGui::SameLine();
        PushSovietButtonStyle(true);
        if (ImGui::Button(Tr("Остановить сервер", "Stop Server"), ImVec2(165.0f, 28.0f))) {
            SaveSync_RequestDisconnect();
        }
        PopSovietButtonStyle();
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

static void DrawLobbyPlayersBox(SessionState st, bool isReconnect, float winW)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.11f, 0.09f, 0.95f));
    ImGui::BeginChild("##lobby_players_box", ImVec2(winW - 40.0f, 88.0f), true);
    {
        bool twoStates = (TwoStates_GetMode() == COOP_MODE_TWO_STATES);
        bool localIsHost = SaveSync_IsHostRole() || st == SESSION_HOST_LISTENING || st == SESSION_HOST_JOINING;

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.92f, 0.48f, 1.0f));
        ImGui::TextUnformatted(twoStates
            ? Tr("● Игрок 1 (Хост — Западная ССР):", "● Player 1 (Host — West SSR):")
            : Tr("● Игрок 1 (Хост сервера):", "● Player 1 (Server Host):"));
        ImGui::PopStyleColor();
        ImGui::SameLine(275.0f);

        unsigned long long localSid = (unsigned long long)SteamP2P_GetLocalSteamId();
        if (localIsHost && localSid != 0) {
            char sidStr[32];
            _snprintf_s(sidStr, sizeof(sidStr), _TRUNCATE, "%llu", localSid);
            ImGui::Text(Tr("В игре (Пауза) | SteamID: %s | TCP :%d",
                           "In game (Paused) | SteamID: %s | TCP :%d"),
                        sidStr, SAVE_SYNC_PORT);
            ImGui::SameLine();
            PushSovietButtonStyle(false);
            if (ImGui::SmallButton(Tr("Копировать ID", "Copy ID"))) {
                CopyTextToClipboard(sidStr);
                TwoStates_AddToast(Tr("SteamID скопирован в буфер обмена", "SteamID copied to clipboard"), 1);
            }
            PopSovietButtonStyle();
        } else {
            ImGui::Text(Tr("Готов (Синхронизация по порту :%d / Steam P2P)",
                           "Ready (Synchronizing on port :%d / Steam P2P)"),
                        SAVE_SYNC_PORT);
        }

        ImGui::Separator();

        float pulse = 0.65f + 0.35f * sinf((float)GetTickCount64() * 0.005f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.78f, 0.28f, pulse));
        ImGui::TextUnformatted(twoStates
            ? Tr("◌ Игрок 2 (Партнёр — Восточная ССР):", "◌ Player 2 (Partner — East SSR):")
            : Tr("◌ Игрок 2 (Партнёр по коопу):", "◌ Player 2 (Co-op Partner):"));
        ImGui::PopStyleColor();
        ImGui::SameLine(275.0f);

        const char* peer = SaveSync_GetPeerIp();
        const char* lastPeer = SaveSync_GetLastPartnerPeer();
        if (st == SESSION_HOST_LISTENING) {
            if (isReconnect) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.35f, 1.0f));
                ImGui::Text(Tr("Отключился (%s) — ожидание возвращения в игру...",
                               "Disconnected (%s) — waiting to rejoin..."),
                            (lastPeer && lastPeer[0]) ? lastPeer : "peer");
                ImGui::PopStyleColor();
            } else {
                ImGui::TextUnformatted(Tr("Ожидание входа (меню «Кооператив -> Подключиться»)...",
                                          "Waiting to join (menu 'Co-op -> Join Server')..."));
            }
        } else if (st == SESSION_HOST_JOINING) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.98f, 1.0f));
            ImGui::Text(Tr("Подключён (%s) — приём карты и вход в Lockstep...",
                           "Connected (%s) — receiving map & entering Lockstep..."),
                        (peer && peer[0]) ? peer : "peer");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.98f, 1.0f));
            ImGui::Text(Tr("Загрузка карты от Хоста (%s)...",
                           "Loading map from Host (%s)..."),
                        (peer && peer[0]) ? peer : "Host");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void DrawLobbySyncProgressBox(int step, int totalSteps, int percent,
                                     int queuedBytes, int totalBytes, float speedMbSec,
                                     const char* text, float winW)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.10f, 0.08f, 0.95f));
    ImGui::BeginChild("##lobby_sync_progress", ImVec2(winW - 40.0f, 138.0f), true);
    {
        const char* stagesRu[4] = {
            "1. Моды и DLC",
            "2. Подготовка сейва",
            "3. Передача по сети",
            "4. Загрузка и старт"
        };
        const char* stagesEn[4] = {
            "1. Mods & DLC",
            "2. Save Packaging",
            "3. Network Transfer",
            "4. World Load & Start"
        };
        int activeStage = (step <= 1) ? 1 : (step > 4 ? 4 : step);
        for (int i = 0; i < 4; ++i) {
            int idx = i + 1;
            ImVec4 col = (idx < activeStage)
                ? ImVec4(0.40f, 0.88f, 0.45f, 1.0f)
                : (idx == activeStage ? ImVec4(0.98f, 0.82f, 0.28f, 1.0f) : ImVec4(0.52f, 0.48f, 0.44f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(Tr(stagesRu[i], stagesEn[i]));
            ImGui::PopStyleColor();
            if (i < 3) {
                ImGui::SameLine();
                ImGui::TextDisabled(">>");
                ImGui::SameLine();
            }
        }

        ImGui::Spacing();
        const char* dispText = (strcmp(text, "Syncing...") == 0) ? Tr("Синхронизация...", "Syncing...") : text;
        char stepLine[220];
        if (totalSteps > 0) {
            _snprintf_s(stepLine, sizeof(stepLine), _TRUNCATE,
                        Tr("Этап %d/%d: %s", "Stage %d/%d: %s"),
                        step, totalSteps, dispText);
        } else {
            _snprintf_s(stepLine, sizeof(stepLine), _TRUNCATE, "%s", dispText);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.93f, 0.86f, 1.0f));
        ImGui::TextUnformatted(stepLine);
        ImGui::PopStyleColor();

        float barFraction = 0.0f;
        char barOverlay[96];
        if (percent >= 0) {
            barFraction = (float)percent / 100.0f;
            if (barFraction > 1.0f) barFraction = 1.0f;
            if (barFraction < 0.0f) barFraction = 0.0f;
            if (totalBytes > 0) {
                int sentBytes = totalBytes - queuedBytes;
                if (sentBytes < 0) sentBytes = 0;
                if (sentBytes > totalBytes) sentBytes = totalBytes;
                double mbSent = (double)sentBytes / (1024.0 * 1024.0);
                double mbTot = (double)totalBytes / (1024.0 * 1024.0);
                if (speedMbSec > 0.05f) {
                    _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE,
                                "%.1f / %.1f %s (%d%%) — %.1f %s",
                                mbSent, mbTot, Tr("МБ", "MB"), percent,
                                speedMbSec, Tr("МБ/с", "MB/s"));
                } else {
                    _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE,
                                "%.1f / %.1f %s (%d%%)",
                                mbSent, mbTot, Tr("МБ", "MB"), percent);
                }
            } else {
                _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE, "%d%%", percent);
            }
        } else {
            float base = (totalSteps > 0 && step > 0) ? ((float)(step - 1) / (float)totalSteps) : 0.15f;
            float wave = 0.12f * (0.5f + 0.5f * sinf((float)GetTickCount64() * 0.006f));
            barFraction = base + wave;
            if (barFraction > 0.95f) barFraction = 0.95f;
            unsigned long long elapsedSec = SaveSync_GetSessionAgeMs() / 1000ULL;
            _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE,
                        Tr("Выполняется (%llus)...", "In progress (%llus)..."), elapsedSec);
        }

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.76f, 0.56f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,       ImVec4(0.22f, 0.16f, 0.13f, 1.0f));
        ImGui::ProgressBar(barFraction, ImVec2(-1.0f, 26.0f), barOverlay);
        ImGui::PopStyleColor(2);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.72f, 0.65f, 1.0f));
        ImGui::TextUnformatted(Tr("По завершении загрузки (100%) лобби закроется автоматически, и вы сможете снять паузу.",
                                  "Once loading reaches 100%, the lobby will close automatically and you can unpause."));
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void DrawLobbyHostSettingsBox(bool isReconnect, float winW)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.10f, 0.08f, 0.95f));
    ImGui::BeginChild("##lobby_host_settings", ImVec2(winW - 40.0f, 152.0f), true);
    {
        bool twoStates = (TwoStates_GetMode() == COOP_MODE_TWO_STATES);
        bool fullCap = TwoStates_GetFullStartingCapital();
        float borderW = TwoStates_GetBorderHalfWidth();
        bool lockTreasuryRules = isReconnect || TwoStates_IsLoadedFromCoopSave();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.34f, 1.0f));
        ImGui::TextUnformatted(lockTreasuryRules
            ? Tr("ПАРАМЕТРЫ ЗАГРУЖЕННОЙ КООП-СЕССИИ:", "LOADED CO-OP SESSION PARAMETERS:")
            : Tr("НАСТРОЙКИ ПАРТИИ (МОЖНО ИЗМЕНИТЬ ДО ВХОДА ИГРОКА 2):",
                 "SESSION SETTINGS (EDITABLE BEFORE PLAYER 2 JOINS):"));
        ImGui::PopStyleColor();

        PushSovietButtonStyle(false);
        if (!lockTreasuryRules) {
            char modeBtn[128];
            _snprintf_s(modeBtn, sizeof(modeBtn), _TRUNCATE,
                        Tr("Режим: %s", "Mode: %s"),
                        twoStates ? Tr("Два государства (Запад и Восток)", "Two States (West & East)")
                                  : Tr("Одно государство (Общая ССР)", "One State (Shared SSR)"));
            if (ImGui::Button(modeBtn, ImVec2(315.0f, 26.0f))) {
                TwoStates_SetMode(twoStates ? COOP_MODE_ONE_STATE : COOP_MODE_TWO_STATES);
                TwoStates_ReapplyWaitingLobbySettings();
                TwoStates_SaveSettings();
            }

            if (twoStates) {
                ImGui::SameLine();
                char capBtn[128];
                _snprintf_s(capBtn, sizeof(capBtn), _TRUNCATE,
                            Tr("Капитал: %s", "Capital: %s"),
                            fullCap ? Tr("100% каждому", "100% Each") : Tr("50% / 50% пополам", "50% / 50% Split"));
                if (ImGui::Button(capBtn, ImVec2(190.0f, 26.0f))) {
                    TwoStates_SetFullStartingCapital(!fullCap);
                    TwoStates_ReapplyWaitingLobbySettings();
                    TwoStates_SaveSettings();
                }

                ImGui::SameLine();
                char brdBtn[96];
                _snprintf_s(brdBtn, sizeof(brdBtn), _TRUNCATE,
                            Tr("Зона: ±%.0fм", "Zone: ±%.0fm"), borderW);
                if (ImGui::Button(brdBtn, ImVec2(110.0f, 26.0f))) {
                    float nextW = (borderW < 200.0f) ? 250.0f
                                : (borderW < 400.0f) ? 500.0f
                                : (borderW < 800.0f) ? 1000.0f : 100.0f;
                    TwoStates_SetBorderHalfWidth(nextW);
                    TwoStates_SaveSettings();
                }
            }
        } else {
            ImGui::Text(Tr("Режим: %s | Пограничная зона X=0: ±%.0f м",
                           "Mode: %s | Border Zone X=0: ±%.0f m"),
                        twoStates ? Tr("Два государства (Запад и Восток)", "Two States (West & East)")
                                  : Tr("Одно государство (Общая ССР)", "One State (Shared SSR)"),
                        borderW);
        }
        PopSovietButtonStyle();

        ImGui::Spacing();
        if (twoStates) {
            double r1 = TwoStates_GetRubles(1), u1 = TwoStates_GetDollars(1);
            double r2 = TwoStates_GetRubles(2), u2 = TwoStates_GetDollars(2);
            ImGui::Text(Tr("Казна Запада (Вы): %.0f РУБ / %.0f $   |   Казна Востока (Партнёр): %.0f РУБ / %.0f $",
                           "West Treasury (You): %.0f RUB / %.0f $   |   East Treasury (Partner): %.0f RUB / %.0f $"),
                        r1, u1, r2, u2);
        } else {
            double r1 = TwoStates_GetRubles(1), u1 = TwoStates_GetDollars(1);
            ImGui::Text(Tr("Общая казна республики: %.0f РУБ / %.0f $",
                           "Shared Republic Treasury: %.0f RUB / %.0f $"),
                        r1, u1);
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.74f, 0.48f, 1.0f));
        ImGui::TextWrapped(Tr("Внимание: пока второй игрок не подключится и не загрузит карту, время принудительно остановлено, а строительство, прокладка дорог, снос и изменение рельефа заблокированы для сохранения 100%% синхронизации.",
                              "Note: until the second player connects and loads the map, time is held on pause and building, road laying, demolition, and terraforming are locked to guarantee 100%% synchronization."));
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void DrawLobbyActionButtons(SessionState st, bool syncActive, bool isReconnect, float winW)
{
    ImGui::Spacing();
    if (st == SESSION_HOST_LISTENING && !syncActive) {
        PushSovietButtonStyle(false);
        if (ImGui::Button(Tr("Осмотр карты [Tab]", "Inspect Map [Tab]"), ImVec2(isReconnect ? 155.0f : 185.0f, 32.0f))) {
            SyncBlock_SetMapInspectMode(true);
        }
        ImGui::SameLine();
        bool b3d = TwoStates_GetShow3DBorder();
        char b3dLabel[64];
        _snprintf_s(b3dLabel, sizeof(b3dLabel), _TRUNCATE,
                    Tr("3D-Граница [F7]: %s", "3D Border [F7]: %s"),
                    b3d ? Tr("ВКЛ", "ON") : Tr("ВЫКЛ", "OFF"));
        if (ImGui::Button(b3dLabel, ImVec2(isReconnect ? 160.0f : 175.0f, 32.0f))) {
            TwoStates_SetShow3DBorder(!b3d);
        }
        if (isReconnect) {
            ImGui::SameLine();
            if (ImGui::Button(Tr("Сохранить кооп-игру", "Save Co-op Game"), ImVec2(160.0f, 32.0f))) {
                if (TwoStates_TriggerQuickCoopSave()) {
                    TwoStates_AddToast(Tr("Кооп-сохранение записано в банк коопа",
                                          "Co-op save written to Co-op bank"), 1);
                }
            }
        }
        PopSovietButtonStyle();

        ImGui::SameLine();
        PushSovietButtonStyle(true);
        if (ImGui::Button(Tr("Остановить сервер", "Stop Server"), ImVec2(isReconnect ? 155.0f : 280.0f, 32.0f))) {
            SaveSync_RequestDisconnect();
        }
        PopSovietButtonStyle();
    } else if (SaveSync_IsBusy() && SaveSync_GetSessionAgeMs() >= SAVE_SYNC_STOP_BUTTON_DELAY_MS) {
        bool host = SaveSync_IsHostRole();
        float btnW = 240.0f;
        ImGui::SetCursorPosX((winW - btnW) * 0.5f);
        PushSovietButtonStyle(true);
        if (ImGui::Button(host ? Tr("Остановить сервер", "Stop Server")
                               : Tr("Отменить подключение", "Cancel Connection"),
                          ImVec2(btnW, 32.0f))) {
            SaveSync_RequestDisconnect();
        }
        PopSovietButtonStyle();
    }
}

static void DrawWorkshopDownloadModal(const ImGuiIO& io)
{
    ModDownloadProgress prog = {};
    if (!ModSync_GetDownloadProgress(&prog) || !prog.active) return;

    // Dimmer background
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.04f, 0.03f, 0.74f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGuiWindowFlags dimFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##republicmp_workshop_dimmer", nullptr, dimFlags)) {
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    float cardW = 700.0f;
    if (cardW > io.DisplaySize.x - 40.0f) cardW = io.DisplaySize.x - 40.0f;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(cardW, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.08f, 0.07f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.84f, 0.67f, 0.24f, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));

    ImGuiWindowFlags cardFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##republicmp_workshop_download_card", nullptr, cardFlags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 winPos = ImGui::GetWindowPos();
        float winW = ImGui::GetWindowWidth();

        dl->AddRectFilled(winPos, ImVec2(winPos.x + winW, winPos.y + 58.0f),
                          IM_COL32(112, 22, 20, 255), 8.0f, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(winPos.x, winPos.y + 58.0f), ImVec2(winPos.x + winW, winPos.y + 58.0f),
                    IM_COL32(214, 171, 61, 240), 2.0f);

        const char* headerTitle = Tr("★ ГОСПЛАН СССР — ЗАГРУЗКА МОДИФИКАЦИЙ ★",
                                     "★ GOSPLAN USSR — DOWNLOADING WORKSHOP MODS ★");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.85f, 0.38f, 1.0f));
        ImVec2 tSize = ImGui::CalcTextSize(headerTitle);
        ImGui::SetCursorPosX((winW - tSize.x) * 0.5f);
        ImGui::TextUnformatted(headerTitle);
        ImGui::PopStyleColor();

        const char* subTitle = Tr("СИНХРОНИЗАЦИЯ МОДОВ STEAM WORKSHOP ДЛЯ КООП-СЕССИИ",
                                  "STEAM WORKSHOP MOD SYNCHRONIZATION FOR CO-OP SESSION");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.89f, 0.82f, 1.0f));
        ImVec2 subSize = ImGui::CalcTextSize(subTitle);
        ImGui::SetCursorPosX((winW - subSize.x) * 0.5f);
        ImGui::TextUnformatted(subTitle);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.05f, 0.04f, 0.88f));
        if (ImGui::BeginChild("##ws_dl_inner", ImVec2(winW - 40.0f, 0.0f), true, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (prog.allCompleted) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.90f, 0.40f, 1.0f));
                ImGui::TextUnformatted(Tr("✔ ВСЕ МОДЫ УСПЕШНО ЗАГРУЖЕНЫ ИЗ STEAM WORKSHOP!",
                                          "✔ ALL MODS SUCCESSFULLY DOWNLOADED FROM STEAM WORKSHOP!"));
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Text(Tr("Загружено модов: %d из %d.", "Mods downloaded: %d of %d."),
                            prog.completedCount, prog.totalCount);

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.20f, 0.70f, 0.20f, 1.0f));
                ImGui::ProgressBar(1.0f, ImVec2(-1.0f, 24.0f), "100%");
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.60f, 1.0f));
                ImGui::TextWrapped(Tr("Для применения новых моделей, текстур и скриптов игра должна быть перезапущена.\n"
                                      "После перезапуска откройте «Кооператив» -> «Подключиться», чтобы войти в игру к хосту.",
                                      "To load newly downloaded models, textures and scripts, the game must be restarted.\n"
                                      "After restart, open 'Co-op' -> 'Join Server' to join the host."));
                ImGui::PopStyleColor();

                ImGui::Spacing();
                PushSovietButtonStyle(true);
                float restartW = 420.0f;
                if (restartW > winW - 60.0f) restartW = winW - 60.0f;
                ImGui::SetCursorPosX((winW - 40.0f - restartW) * 0.5f);
                if (ImGui::Button(Tr("★ ПЕРЕЗАПУСТИТЬ ИГРУ СЕЙЧАС ★", "★ RESTART GAME NOW ★"), ImVec2(restartW, 36.0f))) {
                    ModSync_RestartGameNow();
                }
                PopSovietButtonStyle();

                ImGui::Spacing();
                PushSovietButtonStyle(false);
                float closeW = 120.0f;
                ImGui::SetCursorPosX((winW - 40.0f - closeW) * 0.5f);
                if (ImGui::Button(Tr("Закрыть", "Close"), ImVec2(closeW, 28.0f))) {
                    ModSync_CancelDownloadModal();
                }
                PopSovietButtonStyle();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.85f, 0.38f, 1.0f));
                ImGui::Text(Tr("Загрузка недостающих модов из Steam Workshop (%d из %d)...",
                               "Downloading missing mods from Steam Workshop (%d of %d)..."),
                            prog.completedCount, prog.totalCount);
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Text(Tr("Текущий мод: %s", "Current mod: %s"),
                            prog.currentTitle[0] ? prog.currentTitle : Tr("Ожидание Steam...", "Waiting for Steam..."));

                if (prog.currentBytesTotal > 0) {
                    double mbCur = (double)prog.currentBytesDownloaded / (1024.0 * 1024.0);
                    double mbTot = (double)prog.currentBytesTotal / (1024.0 * 1024.0);
                    double pct = (mbTot > 0.0) ? (mbCur / mbTot * 100.0) : 0.0;
                    ImGui::Text(Tr("Размер: %.1f МБ / %.1f МБ (%.0f%%)", "Size: %.1f MB / %.1f MB (%.0f%%)"),
                                mbCur, mbTot, pct);
                }

                float frac = (prog.totalCount > 0) ? ((float)prog.completedCount / (float)prog.totalCount) : 0.0f;
                if (prog.currentBytesTotal > 0 && prog.totalCount > 0) {
                    float curFrac = (float)prog.currentBytesDownloaded / (float)prog.currentBytesTotal;
                    frac += (curFrac / (float)prog.totalCount);
                }
                if (frac > 0.99f) frac = 0.99f;
                if (frac < 0.0f) frac = 0.0f;

                char barOverlay[64];
                _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE, "%d / %d (%.0f%%)",
                            prog.completedCount, prog.totalCount, frac * 100.0f);

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.84f, 0.67f, 0.24f, 1.0f));
                ImGui::ProgressBar(frac, ImVec2(-1.0f, 24.0f), barOverlay);
                ImGui::PopStyleColor();

                ImGui::Spacing();
                PushSovietButtonStyle(false);
                float cancelW = 160.0f;
                ImGui::SetCursorPosX((winW - 40.0f - cancelW) * 0.5f);
                if (ImGui::Button(Tr("Отмена", "Cancel"), ImVec2(cancelW, 30.0f))) {
                    ModSync_CancelDownloadModal();
                }
                PopSovietButtonStyle();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

void SyncBlock_Draw()
{
    SessionState st = SaveSync_GetState();
    bool syncActive = SyncBlock_IsActive();
    bool waitingLobby = SaveSync_IsWaitingLobbyActive();
    SyncBlock_UpdateMapInspectState((int)st, syncActive);

    bool inActiveGameplay = (st == SESSION_CLIENT_IN_GAME || st == SESSION_HOST_IN_GAME ||
                             (!waitingLobby && !syncActive && SaveSync_IsWorldLoaded()));
    if (inActiveGameplay && ModSync_IsDownloadActive()) {
        ModSync_CancelDownloadModal();
    }

    if (waitingLobby) {
        SaveSync_EnforceWaitingLobbyHold();
    } else if (syncActive || ModSync_IsDownloadActive()) {
        EnsureGamePaused();
    }

    if (ModSync_IsDownloadActive()) {
        ModSync_TickDownloadProgress();
        ModDownloadProgress prog = {};
        if (ModSync_GetDownloadProgress(&prog) && prog.allCompleted &&
            (st == SESSION_HOST_JOINING || st == SESSION_CLIENT_JOINING || inActiveGameplay)) {
            ModSync_CancelDownloadModal();
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        DrawWorkshopDownloadModal(io);
        return;
    }

    if (!syncActive && !waitingLobby)
        return;

    ImGuiIO& io = ImGui::GetIO();
    bool isReconnect = SaveSync_IsWaitingForReconnect();

    if (waitingLobby && !syncActive && g_mapInspectMode) {
        DrawMapInspectBanner(io, isReconnect);
        return;
    }

    char text[160];
    int step, totalSteps, percent, queuedBytes, totalBytes;
    float speedMbSec;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%s", g_text);
        step = g_step;
        totalSteps = g_totalSteps;
        percent = g_percent;
        queuedBytes = g_queuedBytes;
        totalBytes = g_totalBytes;
        speedMbSec = g_speedMbSec;
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.04f, 0.03f, 0.74f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags dimFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##republicmp_lobby_dimmer", nullptr, dimFlags)) {
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    float cardW = 700.0f;
    if (cardW > io.DisplaySize.x - 40.0f) cardW = io.DisplaySize.x - 40.0f;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(cardW, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.08f, 0.07f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.84f, 0.67f, 0.24f, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));

    ImGuiWindowFlags cardFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##republicmp_coop_lobby_card", nullptr, cardFlags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 winPos = ImGui::GetWindowPos();
        float winW = ImGui::GetWindowWidth();

        dl->AddRectFilled(winPos, ImVec2(winPos.x + winW, winPos.y + 58.0f),
                          IM_COL32(112, 22, 20, 255), 8.0f, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(winPos.x, winPos.y + 58.0f), ImVec2(winPos.x + winW, winPos.y + 58.0f),
                    IM_COL32(214, 171, 61, 240), 2.0f);

        char headerTitle[160];
        _snprintf_s(headerTitle, sizeof(headerTitle), _TRUNCATE,
                    Tr("★ ГОСПЛАН СССР — КООПЕРАТИВНЫЙ ШТАБ (RepublicMP v%s) ★",
                       "★ GOSPLAN USSR — CO-OP COMMAND LOBBY (RepublicMP v%s) ★"),
                    REPUBLICMP_VERSION);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.85f, 0.38f, 1.0f));
        ImVec2 tSize = ImGui::CalcTextSize(headerTitle);
        ImGui::SetCursorPosX((winW - tSize.x) * 0.5f);
        ImGui::TextUnformatted(headerTitle);
        ImGui::PopStyleColor();

        const char* subTitle = "";
        if (st == SESSION_HOST_LISTENING && isReconnect) {
            subTitle = Tr("ПАРТНЁР ОТКЛЮЧИЛСЯ — ИГРА ПРИНУДИТЕЛЬНО НА ПАУЗЕ ДО ПЕРЕПОДКЛЮЧЕНИЯ",
                          "PARTNER DISCONNECTED — GAME HELD ON PAUSE UNTIL RECONNECT");
        } else if (st == SESSION_HOST_LISTENING) {
            subTitle = Tr("СЕРВЕР ОТКРЫТ — ОЖИДАНИЕ ВТОРОГО ИГРОКА (ИГРА НА ПАУЗЕ)",
                          "SERVER OPEN — WAITING FOR SECOND PLAYER (GAME PAUSED)");
        } else if (st == SESSION_HOST_JOINING) {
            subTitle = Tr("ВТОРОЙ ИГРОК ПОДКЛЮЧАЕТСЯ — ПЕРЕДАЧА И СИНХРОНИЗАЦИЯ МИРА",
                          "SECOND PLAYER JOINING — TRANSFERRING & SYNCHRONIZING WORLD");
        } else {
            subTitle = Tr("ПОДКЛЮЧЕНИЕ К КОМАНДНОМУ СЕРВЕРУ — СИНХРОНИЗАЦИЯ МИРА",
                          "CONNECTING TO HOST SERVER — SYNCHRONIZING WORLD");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.89f, 0.82f, 1.0f));
        ImVec2 subSize = ImGui::CalcTextSize(subTitle);
        ImGui::SetCursorPosX((winW - subSize.x) * 0.5f);
        ImGui::TextUnformatted(subTitle);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        DrawLobbyPlayersBox(st, isReconnect, winW);

        if (syncActive || st == SESSION_HOST_JOINING || st == SESSION_CLIENT_JOINING) {
            DrawLobbySyncProgressBox(step, totalSteps, percent, queuedBytes, totalBytes, speedMbSec, text, winW);
        } else {
            DrawLobbyHostSettingsBox(isReconnect, winW);
        }

        DrawLobbyActionButtons(st, syncActive, isReconnect, winW);
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}
