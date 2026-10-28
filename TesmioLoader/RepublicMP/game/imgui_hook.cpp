// SPDX-License-Identifier: GPL-3.0-only
#include "imgui_hook.h"
#include "window_focus.h"
#include "../ui/native_menu.h"
#include "../ui/sync_block.h"
#include "../ui/lang.h"
#include "../session/two_states.h"
#include "../session/save_sync.h"
#include "../core/version.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

// Declared in imgui_impl_win32.h only under IMGUI_IMPL_API; re-declared here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_originalWndProc = nullptr;
static bool g_initialized = false;

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

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wParam == VK_TAB && IsGameWindowFocused() &&
        SaveSync_GetState() == SESSION_HOST_LISTENING && SaveSync_IsWorldLoaded() && !SyncBlock_IsActive())
    {
        if (msg == WM_KEYDOWN && (lParam & 0x40000000) == 0)
            SyncBlock_SetMapInspectMode(!SyncBlock_IsMapInspectMode());
        return TRUE;
    }

    bool lobbyOrSyncVisible = SyncBlock_IsActive() || SaveSync_IsWaitingLobbyActive() ||
                              (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded());
    bool syncBlocking = SyncBlock_BlocksWorldInput();
    bool overlayActive = NativeMenu_WantsInput() || lobbyOrSyncVisible;
    bool effectiveVisible = syncBlocking || overlayActive;

    ImGuiIO& io = ImGui::GetIO();

    bool showOurCursor = syncBlocking || (overlayActive && io.WantCaptureMouse);
    io.MouseDrawCursor = showOurCursor;

    // Always let ImGui track mouse position so top bar hover works seamlessly.
    if (effectiveVisible || msg == WM_MOUSEMOVE)
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

    bool imguiWantsInput = overlayActive && (io.WantCaptureMouse || io.WantCaptureKeyboard);

    // During a sync all input is swallowed regardless of ImGui's capture flags.
    if (imguiWantsInput || syncBlocking)
    {
        switch (msg)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (!io.WantCaptureKeyboard && overlayActive &&
                (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP))
                break; // let keyboard hotkeys reach the game when only hovering the top bar
            return TRUE;
        }
    }
    else if (lobbyOrSyncVisible)
    {
        // In [Tab] Map Inspection Mode: allow camera movement (WASD, MMB rotate, MouseWheel zoom),
        // but swallow left/right mouse clicks in the 3D world so no buildings/windows/tools can be clicked.
        switch (msg)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            return TRUE;
        }
    }

    if (showOurCursor && msg == WM_SETCURSOR)
        return TRUE;

    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
}

static int g_testbedRole = 0; // 0 = normal, 1 = test-host (left), 2 = test-client (right)
static bool g_testbedWindowArranged = false;
static ULONGLONG g_lastTitleUpdateMs = 0;
static wchar_t g_lastWindowTitle[256] = {};

static bool IsCurrentThreadWindowOwner(HWND hwnd)
{
    if (!hwnd) return false;
    DWORD wndTid = GetWindowThreadProcessId(hwnd, nullptr);
    return wndTid != 0 && wndTid == GetCurrentThreadId();
}

static void DetectAndArrangeTestbedWindow(HWND hwnd)
{
    if (g_testbedWindowArranged || !IsCurrentThreadWindowOwner(hwnd)) return;
    g_testbedWindowArranged = true;

    char roleEnv[32] = {};
    const char* cmdLine = GetCommandLineA();
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    if ((GetEnvironmentVariableA("REPUBLICMP_ROLE", roleEnv, sizeof(roleEnv)) > 0 && _stricmp(roleEnv, "HOST") == 0) ||
        (cmdLine && (strstr(cmdLine, "--mp-host") || strstr(cmdLine, "--mp-left"))) ||
        strstr(exePath, "test-host") != nullptr) {
        g_testbedRole = 1;
    } else if ((GetEnvironmentVariableA("REPUBLICMP_ROLE", roleEnv, sizeof(roleEnv)) > 0 && _stricmp(roleEnv, "CLIENT") == 0) ||
               (cmdLine && (strstr(cmdLine, "--mp-client") || strstr(cmdLine, "--mp-right"))) ||
               strstr(exePath, "test-client") != nullptr) {
        g_testbedRole = 2;
    }

    if (g_testbedRole != 0) {
        RECT workArea = { 0, 0, 2560, 1400 };
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
        int workW = workArea.right - workArea.left;
        int targetX = (g_testbedRole == 1)
            ? (workArea.left - 7)
            : (workArea.left + (workW / 2) - 7);
        int targetY = workArea.top + 8;
        SetWindowPos(hwnd, nullptr, targetX, targetY, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        Logf("republicmp  IMGUI: arranged Dual-Window Testbed [%s] at (%d, %d)",
             g_testbedRole == 1 ? "HOST/LEFT" : "CLIENT/RIGHT", targetX, targetY);
    }
}

static void UpdateWindowTitlePeriodic(HWND hwnd)
{
    // Critical: never call SetWindowTextW from a background worker/loading thread
    // (e.g. during WaitForLoadToFinish), as SetWindowTextW sends a synchronous cross-thread
    // WM_SETTEXT message to the main thread which is blocked waiting for the loader thread.
    if (!IsCurrentThreadWindowOwner(hwnd)) return;

    DetectAndArrangeTestbedWindow(hwnd);

    ULONGLONG now = GetTickCount64();
    if (g_lastTitleUpdateMs != 0 && (now - g_lastTitleUpdateMs) < 500) return;
    g_lastTitleUpdateMs = now;

    SessionState st = SaveSync_GetState();
    if (g_testbedRole == 0 && st == SESSION_IDLE) return;

    bool isHost = (st == SESSION_HOST_LISTENING || st == SESSION_HOST_JOINING || st == SESSION_HOST_IN_GAME) ||
                  (st == SESSION_IDLE && g_testbedRole == 1);
    bool twoStates = (TwoStates_GetMode() == COOP_MODE_TWO_STATES);

    const wchar_t* roleTag = isHost
        ? (twoStates ? TrW(L"[ХОСТ — Западная ССР]", L"[HOST — Western SSR]") : TrW(L"[ХОСТ]", L"[HOST]"))
        : (twoStates ? TrW(L"[КЛИЕНТ — Восточная ССР]", L"[CLIENT — Eastern SSR]") : TrW(L"[КЛИЕНТ]", L"[CLIENT]"));

    const wchar_t* stateTag = TrW(L"Главное меню / Одиночная", L"Main Menu / Singleplayer");
    if (st == SESSION_HOST_LISTENING)      stateTag = TrW(L"Ожидание второго игрока (:5556)", L"Waiting for Player 2 (:5556)");
    else if (st == SESSION_HOST_JOINING)   stateTag = TrW(L"Передача сохранения...", L"Sending save...");
    else if (st == SESSION_HOST_IN_GAME)   stateTag = TrW(L"В ИГРЕ (Lockstep активен)", L"IN GAME (Lockstep active)");
    else if (st == SESSION_CLIENT_JOINING) stateTag = TrW(L"Подключение и загрузка мира...", L"Connecting & loading world...");
    else if (st == SESSION_CLIENT_IN_GAME) stateTag = TrW(L"В ИГРЕ (Lockstep активен)", L"IN GAME (Lockstep active)");
    else if (SaveSync_IsWorldLoaded())     stateTag = TrW(L"Мир загружен", L"World loaded");

    wchar_t title[256] = {};
    _snwprintf_s(title, _TRUNCATE, L"%ls %ls — Workers & Resources (RepublicMP v" REPUBLICMP_VERSION_W L")",
                 roleTag, stateTag);
    if (wcscmp(title, g_lastWindowTitle) != 0) {
        wcsncpy_s(g_lastWindowTitle, title, _TRUNCATE);
        SetWindowTextW(hwnd, title);
    }
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
        ImFont* mainFont = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
        if (mainFont && GetFileAttributesA("C:\\Windows\\Fonts\\seguisym.ttf") != INVALID_FILE_ATTRIBUTES) {
            static const ImWchar kSymbolRanges[] = { 0x2000, 0x27BF, 0 };
            ImFontConfig symCfg;
            symCfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 16.0f, &symCfg, kSymbolRanges);
        }
    }

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    DetectAndArrangeTestbedWindow(g_hwnd);
    UpdateWindowTitlePeriodic(g_hwnd);

    g_initialized = true;
    Logf("republicmp  IMGUI: initialized, hwnd=%p", (void*)g_hwnd);
}

void ImGuiHook_NewFrameAndRender()
{
    if (!g_initialized)
        return;

    UpdateWindowTitlePeriodic(g_hwnd);

    bool lobbyOrSyncVisible = SyncBlock_IsActive() || SaveSync_IsWaitingLobbyActive() ||
                              (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded());
    bool syncBlocking = SyncBlock_BlocksWorldInput();
    bool overlayActive = NativeMenu_WantsInput() || lobbyOrSyncVisible;

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = syncBlocking || (overlayActive && io.WantCaptureMouse);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Draw 3D border lines, 3D partner cursor/pings, Top-Center Mini-HUD, Toast notifications,
    // and the Native Load/New Game Co-op launch bar
    TwoStates_DrawOverlay();
    NativeMenu_DrawOverlay();
    SyncBlock_Draw();

    ImGui::Render();
    g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
