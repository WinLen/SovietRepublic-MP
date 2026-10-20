// SPDX-License-Identifier: GPL-3.0-only
// See sync_block.h.
#include "sync_block.h"
#include "lang.h"
#include "../session/game_speed.h" // EnsureGamePaused() re-assert, see SyncBlock_Draw
#include "../session/save_sync.h"    // the Cancel button
#include "imgui.h"
#include <windows.h>
#include <mutex>
#include <cstdio>

static std::mutex g_mutex;
static volatile bool g_active = false;
static char g_text[160] = "Syncing...";
static int g_step = 0;
static int g_totalSteps = 0;
static int g_percent = -1;

void SyncBlock_SetActive(bool active, const char* reason)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = active;
    if (active && reason && reason[0]) {
        _snprintf_s(g_text, sizeof(g_text), _TRUNCATE, "%s", reason);
    }
    if (!active) {
        g_step = g_totalSteps = 0;
        g_percent = -1;
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
}

bool SyncBlock_IsActive()
{
    return g_active;
}

static void CenteredText(const char* s, float y)
{
    ImVec2 size = ImGui::CalcTextSize(s);
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - size.x) * 0.5f, y));
    ImGui::TextUnformatted(s);
}

void SyncBlock_Draw()
{
    if (!g_active)
        return;

    // The game also reads input outside the window message queue, so re-assert
    // the pause every frame; building placement is refused separately in
    // HookedUniversal.
    EnsureGamePaused();

    char text[160];
    int step, totalSteps, percent;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%s", g_text);
        step = g_step;
        totalSteps = g_totalSteps;
        percent = g_percent;
    }

    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.12f, 0.92f)); // grey, nearly opaque
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##syncblock", nullptr, flags))
    {
        float cy = io.DisplaySize.y * 0.5f;
        float lineH = ImGui::GetTextLineHeight();

        ImGui::SetWindowFontScale(1.4f);
        CenteredText(Tr("Мультиплеер — синхронизация", "Multiplayer - joining"), cy - lineH * 5.0f);

        ImGui::SetWindowFontScale(2.0f);
        char stepLine[200];
        if (totalSteps > 0)
            _snprintf_s(stepLine, sizeof(stepLine), _TRUNCATE, Tr("Шаг %d/%d — %s", "Step %d/%d - %s"), step, totalSteps, text);
        else
            _snprintf_s(stepLine, sizeof(stepLine), _TRUNCATE, "%s", text);
        CenteredText(stepLine, cy - lineH * 1.5f);
        ImGui::SetWindowFontScale(1.0f);

        if (percent >= 0) {
            float w = io.DisplaySize.x * 0.4f;
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - w) * 0.5f, cy + lineH * 1.5f));
            char overlay[16];
            _snprintf_s(overlay, sizeof(overlay), _TRUNCATE, "%d%%", percent);
            ImGui::ProgressBar(percent / 100.0f, ImVec2(w, lineH * 1.4f), overlay);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        CenteredText(Tr("Игра на паузе и управление заблокировано до готовности обоих игроков.",
                        "The game is paused and input is locked until both players are ready."),
                     cy + lineH * 4.5f);
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // A way out that does not need the other side: a join stuck on a dead
    // connection would otherwise only end by killing the game. Its own small
    // window, drawn after the dimmer, so it takes clicks. Shown a moment
    // after the start and low on the screen, away from where Connect was
    // clicked.
    if (SaveSync_IsBusy() && SaveSync_GetSessionAgeMs() >= SAVE_SYNC_STOP_BUTTON_DELAY_MS) {
        float lineH = ImGui::GetTextLineHeight();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.8f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGuiWindowFlags btnFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##syncblockcancel", nullptr, btnFlags)) {
            bool host = SaveSync_IsHostRole();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
            if (ImGui::Button(host ? Tr("Остановить сервер", "Stop hosting") : Tr("Отменить вход", "Cancel joining"),
                              ImVec2(lineH * 12.0f, lineH * 2.0f)))
                SaveSync_RequestDisconnect();
            ImGui::PopStyleColor(2);
        }
        ImGui::End();
    }

    // NoInputs is intentional: this window only dims the screen; input is
    // blocked in HookedWndProc and game/input_block.
}
