// SPDX-License-Identifier: GPL-3.0-only
#include "imgui_hook.h"
#include "window_focus.h"
#include "../ui/main_menu.h"
#include "../ui/debug_window.h"
#include "../ui/sync_block.h"
#include "../ui/lang.h"
#include "../session/two_states.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>

// Declared in imgui_impl_win32.h only under IMGUI_IMPL_API; re-declared here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_originalWndProc = nullptr;
static bool g_initialized = false;
static bool g_insertWasDown = false;

bool g_imguiVisible = true;

void ImGuiHook_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static void CreateRenderTarget(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (backBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
        backBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool g_topBtnHovered = false;

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // The sync-block screen and Mini-HUD / Top Co-op button keep receiving mouse input even while
    // the main menu is hidden with INSERT.
    bool syncBlocking = SyncBlock_IsActive();
    bool miniHudMouse = TwoStates_MiniHudWantsMouse() || g_topBtnHovered;
    bool effectiveVisible = g_imguiVisible || syncBlocking || miniHudMouse;

    ImGuiIO& io = ImGui::GetIO();

    // Draw ImGui's own cursor only while the mouse is over an ImGui window, or
    // while the sync-block screen is up.
    bool showOurCursor = syncBlocking || ((g_imguiVisible || miniHudMouse) && io.WantCaptureMouse);
    io.MouseDrawCursor = showOurCursor;

    // Always let ImGui track mouse position so Mini-HUD hover works when hidden.
    if (effectiveVisible || msg == WM_MOUSEMOVE)
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

    bool imguiWantsInput = (g_imguiVisible && (io.WantCaptureMouse || io.WantCaptureKeyboard)) ||
                           (miniHudMouse && io.WantCaptureMouse);

    // During a sync all input is swallowed regardless of ImGui's capture flags.
    if (imguiWantsInput || syncBlocking)
    {
        // Consumed by ImGui, or dropped during a sync - do not forward to the game.
        switch (msg)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (!g_imguiVisible && miniHudMouse &&
                (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP))
                break; // let keyboard hotkeys reach the game when only hovering Mini-HUD
            return TRUE;
        }
    }

    // Swallow WM_SETCURSOR only while we draw the cursor, so the game cannot
    // override our sprite; otherwise let the game handle it.
    if (showOurCursor && msg == WM_SETCURSOR)
        return TRUE;

    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
}

void ImGuiHook_EnsureInit(IDXGISwapChain* swapChain)
{
    if (g_initialized || !swapChain)
        return;

    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice)) || !g_pd3dDevice)
        return;
    g_pd3dDevice->GetImmediateContext(&g_pd3dContext);

    DXGI_SWAP_CHAIN_DESC desc = {};
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    if (!g_hwnd)
        return;

    CreateRenderTarget(swapChain);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    }

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

    // Subclass the game window now that its HWND is known.
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

    g_initialized = true;
    Logf("republicmp  IMGUI: initialized, hwnd=%p", (void*)g_hwnd);
}

void ImGuiHook_NewFrameAndRender()
{
    if (!g_initialized)
        return;

    // INSERT toggles the overlay (only while the game window has focus).
    bool insertDown = IsGameWindowFocused() && (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertDown && !g_insertWasDown)
        g_imguiVisible = !g_imguiVisible;
    g_insertWasDown = insertDown;

    bool syncBlocking = SyncBlock_IsActive();
    bool miniHudMouse = TwoStates_MiniHudWantsMouse() || g_topBtnHovered;

    // Refresh the cursor flag for frames where hover changed without a message.
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = syncBlocking || ((g_imguiVisible || miniHudMouse) && io.WantCaptureMouse);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Always draw 3D border lines, 3D partner cursor/pings, Mini-HUD and Toast notifications
    TwoStates_DrawOverlay();

    if (g_imguiVisible) {
        g_topBtnHovered = false;
        g_mainMenu.Draw();
        g_debug_window.Draw();
    } else if (!syncBlocking) {
        // Floating top-right button so the player can always open the Co-op menu with the mouse
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 16.0f, vp->Pos.y + 42.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.82f);
        ImGuiWindowFlags btnFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##RepublicMPTopToggle", nullptr, btnFlags)) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.16f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.22f, 0.22f, 1.0f));
            if (ImGui::Button(Tr("Кооператив / Сервер (INSERT)", "Co-op / Server (INSERT)"))) {
                g_imguiVisible = true;
            }
            ImGui::PopStyleColor(2);
            g_topBtnHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsItemHovered();
        }
        ImGui::End();
    } else {
        g_topBtnHovered = false;
    }
    SyncBlock_Draw(); // drawn last (on top), independent of g_imguiVisible

    ImGui::Render();
    g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
