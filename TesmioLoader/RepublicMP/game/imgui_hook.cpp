// SPDX-License-Identifier: GPL-3.0-only
#include "imgui_hook.h"
#include "window_focus.h"
#include "../ui/native_menu.h"
#include "../ui/sync_block.h"
#include "../ui/lang.h"
#include "../session/two_states.h"
#include "../session/save_sync.h"
#include "../core/version.h"
#include "../core/diag_log.h"
#include "present_hook.h"

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

static int g_testbedRole = 0; // 0 = normal, 1 = test-host (left), 2 = test-client (right)
static bool g_testbedWindowArranged = false;
static bool g_windowBarHovered = false;
static ULONGLONG g_lastTitleUpdateMs = 0;
static wchar_t g_lastWindowTitle[256] = {};
static wchar_t g_origWindowTitle[256] = {};
static bool g_hasOrigWindowTitle = false;

struct FindOtherWindowCtx {
    HWND selfHwnd;
    HWND foundHwnd;
};

static BOOL CALLBACK EnumOtherSovietWndProc(HWND hwnd, LPARAM lParam)
{
    FindOtherWindowCtx* ctx = (FindOtherWindowCtx*)lParam;
    if (!ctx || hwnd == ctx->selfHwnd || !IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        char path[MAX_PATH] = {};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameA(hProc, 0, path, &sz)) {
            if (strstr(path, "SOVIET64.exe") != nullptr || strstr(path, "soviet64.exe") != nullptr) {
                ctx->foundHwnd = hwnd;
                CloseHandle(hProc);
                return FALSE;
            }
        }
        CloseHandle(hProc);
    }
    return TRUE;
}

static HWND FindOtherSovietWindow(HWND selfHwnd)
{
    static HWND s_cachedOtherHwnd = nullptr;
    if (s_cachedOtherHwnd && s_cachedOtherHwnd != selfHwnd && IsWindow(s_cachedOtherHwnd)) {
        return s_cachedOtherHwnd;
    }
    FindOtherWindowCtx ctx = { selfHwnd, nullptr };
    EnumWindows(EnumOtherSovietWndProc, (LPARAM)&ctx);
    s_cachedOtherHwnd = ctx.foundHwnd;
    return s_cachedOtherHwnd;
}

static bool IsDualTestbedActive()
{
    if (g_testbedRole != 0) return true;
    return IsDualTestbed();
}

static bool IsInCoopOrOverlay()
{
    return (SaveSync_GetState() != SESSION_IDLE) ||
           NativeMenu_WantsInput() ||
           SaveSync_IsWaitingLobbyActive() ||
           SyncBlock_IsActive() ||
           IsDualTestbedActive();
}

static bool HandleTabInspectToggle(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (wParam != VK_TAB || (msg != WM_KEYDOWN && msg != WM_KEYUP)) return false;
    if (!IsGameWindowFocused() || !SaveSync_IsWorldLoaded() || SyncBlock_IsActive()) return false;
    if (SaveSync_GetState() != SESSION_HOST_LISTENING) return false;

    if (msg == WM_KEYDOWN && (lParam & 0x40000000) == 0)
        SyncBlock_SetMapInspectMode(!SyncBlock_IsMapInspectMode());
    return true;
}

static bool ShouldSwallowInputMessage(UINT msg, bool overlayActive, bool imguiWantsInput,
                                      bool syncBlocking, bool lobbyOrSyncVisible, const ImGuiIO& io)
{
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
                return false; // let keyboard hotkeys reach the game when only hovering the top bar
            return true;
        default:
            break;
        }
    }
    else if (lobbyOrSyncVisible)
    {
        switch (msg)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            return true;
        default:
            break;
        }
    }
    return false;
}

#define TESTBED_INPUT_OBJ_RVA        0xA54B90
#define TESTBED_IMPORT_MOUSE_POS_RVA 0x86C868
#define TESTBED_INPUT_LHELD_RVA      0xA54E90
#define TESTBED_INPUT_LCLICK_RVA     0xA54E91
#define DEVICE_RESTORED_BYTE_RVA     0x99208C
#define WM_TESTBED_WINDOW_CMD        (WM_APP + 0x52)
#define TESTBED_CMD_MINIMIZE         1
#define TESTBED_CMD_SWITCH           2
#define TESTBED_CMD_RESTORE          3

typedef float* (__fastcall *FnTestbedGetMousePos)(void* inputObj, float* outVec3);

static bool g_prevPillLmbDown = false;
static bool g_prevF10Down = false;
static bool g_prevF11Down = false;

static bool GetTestbedSovietCursorPos(float* outX, float* outY)
{
    if (!g_exeBase || !outX || !outY) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        uintptr_t fnAddr = *(uintptr_t*)(g_exeBase + TESTBED_IMPORT_MOUSE_POS_RVA);
        if (fnAddr) {
            float pt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ((FnTestbedGetMousePos)fnAddr)((void*)(g_exeBase + TESTBED_INPUT_OBJ_RVA), pt);
            *outX = pt[0];
            *outY = pt[1];
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool GetTestbedEngineMouseDown()
{
    if (!g_exeBase) return false;
    bool down = false;
    DiagLog_BeginSafeSeh();
    __try {
        down = (*(BYTE*)(g_exeBase + TESTBED_INPUT_LHELD_RVA) != 0) ||
               (*(BYTE*)(g_exeBase + TESTBED_INPUT_LCLICK_RVA) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        down = false;
    }
    DiagLog_EndSafeSeh();
    return down;
}

static void ClearTestbedEngineMouseClick()
{
    if (!g_exeBase) return;
    DiagLog_BeginSafeSeh();
    __try {
        *(BYTE*)(g_exeBase + TESTBED_INPUT_LCLICK_RVA) = 0;
        *(BYTE*)(g_exeBase + TESTBED_INPUT_LHELD_RVA) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void ExecuteTestbedMinimize(HWND hwnd)
{
    ClipCursor(nullptr);
    if (g_exeBase) {
        *(BYTE*)(g_exeBase + WINDOW_FOCUS_BYTE_RVA) = 0;
        *(BYTE*)(g_exeBase + DEVICE_RESTORED_BYTE_RVA) = 1;
    }
    HWND otherHwnd = FindOtherSovietWindow(hwnd);
    if (otherHwnd && !IsIconic(otherHwnd)) {
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(otherHwnd);
    }
    ShowWindow(hwnd, SW_MINIMIZE);
}

static void ExecuteTestbedRestore(HWND hwnd)
{
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    ClipCursor(nullptr);
    if (g_exeBase) {
        *(BYTE*)(g_exeBase + WINDOW_FOCUS_BYTE_RVA) = 1;
        *(BYTE*)(g_exeBase + DEVICE_RESTORED_BYTE_RVA) = 1;
    }
}

static void ExecuteTestbedSwitchToOther(HWND hwnd)
{
    HWND otherHwnd = FindOtherSovietWindow(hwnd);
    if (!otherHwnd) return;
    ClipCursor(nullptr);
    if (g_exeBase) {
        *(BYTE*)(g_exeBase + WINDOW_FOCUS_BYTE_RVA) = 0;
        *(BYTE*)(g_exeBase + DEVICE_RESTORED_BYTE_RVA) = 1;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    if (!IsIconic(otherHwnd)) {
        SetForegroundWindow(otherHwnd);
    }
    PostMessageW(otherHwnd, WM_TESTBED_WINDOW_CMD, TESTBED_CMD_RESTORE, 0);
}

static bool HandleTestbedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/, LRESULT* outRes)
{
    if (msg == WM_TESTBED_WINDOW_CMD) {
        if (wParam == TESTBED_CMD_MINIMIZE) {
            ExecuteTestbedMinimize(hwnd);
        } else if (wParam == TESTBED_CMD_SWITCH) {
            ExecuteTestbedSwitchToOther(hwnd);
        } else if (wParam == TESTBED_CMD_RESTORE) {
            ExecuteTestbedRestore(hwnd);
        }
        *outRes = 0;
        return true;
    }
    if (msg == WM_SYSCOMMAND) {
        WPARAM cmd = (wParam & 0xFFF0);
        if (cmd == SC_MINIMIZE) {
            ExecuteTestbedMinimize(hwnd);
            *outRes = 0;
            return true;
        } else if (cmd == SC_RESTORE) {
            ExecuteTestbedRestore(hwnd);
            *outRes = 0;
            return true;
        }
    }
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)) {
        ClipCursor(nullptr);
        if (g_exeBase) {
            *(BYTE*)(g_exeBase + WINDOW_FOCUS_BYTE_RVA) = 0;
            *(BYTE*)(g_exeBase + DEVICE_RESTORED_BYTE_RVA) = 1;
        }
    } else if (msg == WM_SETFOCUS || (msg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE)) {
        ClipCursor(nullptr);
        if (g_exeBase) {
            *(BYTE*)(g_exeBase + WINDOW_FOCUS_BYTE_RVA) = 1;
            *(BYTE*)(g_exeBase + DEVICE_RESTORED_BYTE_RVA) = 1;
        }
    }
    return false;
}

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (IsDualTestbedActive()) {
        LRESULT testbedRes = 0;
        if (HandleTestbedWndProc(hwnd, msg, wParam, lParam, &testbedRes)) {
            return testbedRes;
        }
    }

    if (!IsInCoopOrOverlay())
    {
        return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
    }

    if (HandleTabInspectToggle(msg, wParam, lParam))
    {
        return TRUE;
    }

    bool lobbyOrSyncVisible = SyncBlock_IsActive() || SaveSync_IsWaitingLobbyActive() ||
                              (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded());
    bool syncBlocking = SyncBlock_BlocksWorldInput();
    bool overlayActive = NativeMenu_WantsInput() || lobbyOrSyncVisible || g_windowBarHovered;
    bool effectiveVisible = syncBlocking || overlayActive;

    ImGuiIO& io = ImGui::GetIO();
    bool showOurCursor = syncBlocking || (overlayActive && io.WantCaptureMouse);
    io.MouseDrawCursor = showOurCursor;

    // Always let ImGui track mouse position so top bar hover works seamlessly.
    if (effectiveVisible || msg == WM_MOUSEMOVE)
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

    bool imguiWantsInput = overlayActive && (io.WantCaptureMouse || io.WantCaptureKeyboard);

    if (ShouldSwallowInputMessage(msg, overlayActive, imguiWantsInput, syncBlocking, lobbyOrSyncVisible, io))
    {
        return TRUE;
    }

    if (showOurCursor && msg == WM_SETCURSOR)
        return TRUE;

    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
}

static bool IsCurrentThreadWindowOwner(HWND hwnd)
{
    if (!hwnd) return false;
    DWORD wndTid = GetWindowThreadProcessId(hwnd, nullptr);
    return wndTid != 0 && wndTid == GetCurrentThreadId();
}

struct TestbedCodePatch {
    unsigned int rva;
    unsigned char size;
    unsigned char expected[9];
    unsigned char replacement[9];
};

static const TestbedCodePatch kTestbedPatches[] = {
    // 0x105B81: cmove eax, r14d -> nop4 (never reset 0x99208C to 0 when unfocused, avoiding DXGI mode re-enum on focus)
    { 0x105B81, 4, { 0x41, 0x0F, 0x44, 0xC6 }, { 0x0F, 0x1F, 0x40, 0x00 } },
    // 0x10370A: shr rax, 16; test ax, ax; jne +0x28 -> nop4; test ax, ax; je +0x28 (check LOWORD(wParam) == WA_INACTIVE)
    { 0x10370A, 9, { 0x48, 0xC1, 0xE8, 0x10, 0x66, 0x85, 0xC0, 0x75, 0x28 },
                   { 0x0F, 0x1F, 0x40, 0x00, 0x66, 0x85, 0xC0, 0x74, 0x28 } },
    // 0x1036EF & 0x103726: call ShowWindow(g_hwnd, SW_SHOWDEFAULT) in WndProc -> nop6
    { 0x1036EF, 6, { 0xFF, 0x15, 0xEB, 0xA4, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
    { 0x103726, 6, { 0xFF, 0x15, 0xB4, 0xA4, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
    // 0x103668, 0x1036DD, 0x103733, 0x103742, 0x105FC4, 0x106341: call ClipCursor -> nop6
    { 0x103668, 6, { 0xFF, 0x15, 0xAA, 0xA5, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
    { 0x1036DD, 6, { 0xFF, 0x15, 0x35, 0xA5, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
    { 0x103733, 6, { 0xFF, 0x15, 0xDF, 0xA4, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
    { 0x103742, 6, { 0xFF, 0x15, 0xD0, 0xA4, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
    { 0x105FC4, 6, { 0xFF, 0x15, 0x4E, 0x7C, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
    { 0x106341, 6, { 0xFF, 0x15, 0xD1, 0x78, 0x76, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 } },
};

static void ApplyTestbedWindowPatches()
{
    if (!g_exeBase) return;
    static bool s_testbedPatched = false;
    if (s_testbedPatched) return;
    s_testbedPatched = true;

    *(BYTE*)(g_exeBase + DEVICE_RESTORED_BYTE_RVA) = 1;
    for (size_t i = 0; i < sizeof(kTestbedPatches) / sizeof(kTestbedPatches[0]); ++i) {
        const TestbedCodePatch& p = kTestbedPatches[i];
        BYTE* at = g_exeBase + p.rva;
        if (memcmp(at, p.expected, p.size) == 0) {
            DWORD oldProt = 0;
            if (VirtualProtect(at, p.size, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(at, p.replacement, p.size);
                VirtualProtect(at, p.size, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), at, p.size);
            }
        }
    }
}

static void ConfigureTestbedWindowAndDxgi(HWND hwnd)
{
    if (!IsCurrentThreadWindowOwner(hwnd)) return;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wantBits = (WS_MINIMIZEBOX | WS_SYSMENU);
    if ((style & wantBits) != wantBits || (style & WS_CAPTION) != 0) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style | wantBits) & ~((LONG_PTR)WS_CAPTION));
    }

    HMODULE hDwm = GetModuleHandleA("dwmapi.dll");
    if (!hDwm) hDwm = LoadLibraryA("dwmapi.dll");
    if (hDwm) {
        typedef HRESULT (WINAPI *FnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        auto pSetAttr = (FnDwmSetWindowAttribute)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (pSetAttr) {
            BOOL disableTransitions = TRUE;
            pSetAttr(hwnd, 3 /*DWMWA_TRANSITIONS_FORCEDISABLED*/, &disableTransitions, sizeof(disableTransitions));
        }
    }

    if (g_pd3dDevice) {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) && dxgiDevice) {
            IDXGIAdapter* dxgiAdapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) && dxgiAdapter) {
                IDXGIFactory* dxgiFactory = nullptr;
                if (SUCCEEDED(dxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&dxgiFactory)) && dxgiFactory) {
                    dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                    dxgiFactory->Release();
                }
                dxgiAdapter->Release();
            }
            dxgiDevice->Release();
        }
    }
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
        ApplyTestbedWindowPatches();
        ConfigureTestbedWindowAndDxgi(hwnd);
        RECT workArea = { 0, 0, 2560, 1400 };
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
        int workW = workArea.right - workArea.left;
        RECT wndRect = {};
        GetWindowRect(hwnd, &wndRect);
        int wndW = wndRect.right - wndRect.left;

        int targetX = 0;
        int targetY = 0;
        if (wndW <= (workW / 2) + 100) {
            targetX = (g_testbedRole == 1)
                ? (workArea.left - 7)
                : (workArea.left + (workW / 2) - 7);
            targetY = workArea.top + 8;
        }
        SetWindowPos(hwnd, nullptr, targetX, targetY, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        Logf("republicmp  IMGUI: arranged Dual-Window Testbed [%s] (%dx%d) at (%d, %d)",
             g_testbedRole == 1 ? "HOST" : "CLIENT",
             wndW, (int)(wndRect.bottom - wndRect.top), targetX, targetY);
    }
}

static void UpdateWindowTitlePeriodic(HWND hwnd)
{
    // Critical: never call SetWindowTextW from a background worker/loading thread
    // (e.g. during WaitForLoadToFinish), as SetWindowTextW sends a synchronous cross-thread
    // WM_SETTEXT message to the main thread which is blocked waiting for the loader thread.
    if (!IsCurrentThreadWindowOwner(hwnd)) return;

    DetectAndArrangeTestbedWindow(hwnd);

    if (!g_hasOrigWindowTitle) {
        if (GetWindowTextW(hwnd, g_origWindowTitle, 256) > 0) {
            g_hasOrigWindowTitle = true;
        } else {
            wcscpy_s(g_origWindowTitle, L"Workers & Resources: Soviet Republic");
            g_hasOrigWindowTitle = true;
        }
    }

    ULONGLONG now = GetTickCount64();
    if (g_lastTitleUpdateMs != 0 && (now - g_lastTitleUpdateMs) < 500) return;
    g_lastTitleUpdateMs = now;

    SessionState st = SaveSync_GetState();
    if (g_testbedRole == 0 && st == SESSION_IDLE) {
        if (g_lastWindowTitle[0] != L'\0') {
            const wchar_t* defaultTitle = (g_origWindowTitle[0] != L'\0')
                ? g_origWindowTitle
                : L"Workers & Resources: Soviet Republic";
            SetWindowTextW(hwnd, defaultTitle);
            g_lastWindowTitle[0] = L'\0';
        }
        return;
    }

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

void ImGuiHook_UpdateWindowTitleForTest(HWND hwnd)
{
    g_lastTitleUpdateMs = 0;
    UpdateWindowTitlePeriodic(hwnd);
}

void ImGuiHook_ResetTitleTrackingForTest()
{
    g_lastTitleUpdateMs = 0;
    g_lastWindowTitle[0] = L'\0';
    g_origWindowTitle[0] = L'\0';
    g_hasOrigWindowTitle = false;
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

static bool IsTestbedRectHovered(const ImVec2& rMin, const ImVec2& rMax,
                                 bool hasSovietPos, float sovietX, float sovietY,
                                 const ImVec2& winPos)
{
    if (hasSovietPos && sovietX >= rMin.x && sovietX <= rMax.x && sovietY >= rMin.y && sovietY <= rMax.y) {
        return true;
    }
    if (winPos.x >= rMin.x && winPos.x <= rMax.x && winPos.y >= rMin.y && winPos.y <= rMax.y) {
        return true;
    }
    return false;
}

static void DrawWindowControlPill()
{
    g_windowBarHovered = false;
    if (!g_hwnd || !IsDualTestbedActive()) return;

    bool isFg = (GetForegroundWindow() == g_hwnd);
    bool lmbNow = isFg && (((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) || GetTestbedEngineMouseDown());
    bool lmbJustClicked = (lmbNow && !g_prevPillLmbDown);
    g_prevPillLmbDown = lmbNow;

    bool f10Now = isFg && ((GetAsyncKeyState(VK_F10) & 0x8000) != 0);
    bool f10JustPressed = (f10Now && !g_prevF10Down);
    g_prevF10Down = f10Now;

    bool f11Now = isFg && ((GetAsyncKeyState(VK_F11) & 0x8000) != 0);
    bool f11JustPressed = (f11Now && !g_prevF11Down);
    g_prevF11Down = f11Now;

    if (f10JustPressed) {
        PostMessageW(g_hwnd, WM_TESTBED_WINDOW_CMD, TESTBED_CMD_SWITCH, 0);
    }
    if (f11JustPressed) {
        PostMessageW(g_hwnd, WM_TESTBED_WINDOW_CMD, TESTBED_CMD_MINIMIZE, 0);
    }

    float sovietX = -1000.0f, sovietY = -1000.0f;
    bool hasSovietPos = GetTestbedSovietCursorPos(&sovietX, &sovietY);
    ImVec2 winMousePos = ImGui::GetIO().MousePos;

    SessionState st = SaveSync_GetState();
    bool isHost = (st == SESSION_HOST_LISTENING || st == SESSION_HOST_JOINING || st == SESSION_HOST_IN_GAME) ||
                  (st == SESSION_IDLE && g_testbedRole == 1);
    bool twoStates = (TwoStates_GetMode() == COOP_MODE_TWO_STATES);

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, 3.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.08f, 0.06f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Border, isHost ? ImVec4(0.95f, 0.42f, 0.30f, 0.85f)
                                                  : ImVec4(0.30f, 0.72f, 0.95f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.20f, 0.14f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.32f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.56f, 0.42f, 0.24f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##RepublicMP_WindowControlPill", nullptr, flags)) {
        ImVec4 roleCol = isHost ? ImVec4(1.0f, 0.55f, 0.42f, 1.0f) : ImVec4(0.45f, 0.82f, 1.0f, 1.0f);
        const char* roleText = isHost
            ? (twoStates ? Tr("★ Западная ССР (Хост)", "★ West SSR (Host)") : Tr("★ Хост", "★ Host"))
            : (twoStates ? Tr("★ Восточная ССР (Клиент)", "★ East SSR (Client)") : Tr("★ Клиент", "★ Client"));
        ImGui::TextColored(roleCol, "%s", roleText);

        ImGui::SameLine();
        const char* switchLabel = isHost
            ? Tr("⇄ Окно Клиента [F10]", "⇄ Client Window [F10]")
            : Tr("⇄ Окно Хоста [F10]", "⇄ Host Window [F10]");
        bool imguiSwitchClick = ImGui::SmallButton(switchLabel);
        ImVec2 swMin = ImGui::GetItemRectMin();
        ImVec2 swMax = ImGui::GetItemRectMax();
        bool swHovered = IsTestbedRectHovered(swMin, swMax, hasSovietPos, sovietX, sovietY, winMousePos);
        if (swHovered) {
            ImGui::GetWindowDrawList()->AddRect(swMin, swMax, IM_COL32(255, 215, 80, 255), 3.0f, 0, 2.0f);
        }
        if (imguiSwitchClick || (swHovered && lmbJustClicked)) {
            ClearTestbedEngineMouseClick();
            PostMessageW(g_hwnd, WM_TESTBED_WINDOW_CMD, TESTBED_CMD_SWITCH, 0);
        }

        ImGui::SameLine();
        bool imguiMinClick = ImGui::SmallButton(Tr("— Свернуть [F11]", "— Minimize [F11]"));
        ImVec2 minMin = ImGui::GetItemRectMin();
        ImVec2 minMax = ImGui::GetItemRectMax();
        bool minHovered = IsTestbedRectHovered(minMin, minMax, hasSovietPos, sovietX, sovietY, winMousePos);
        if (minHovered) {
            ImGui::GetWindowDrawList()->AddRect(minMin, minMax, IM_COL32(255, 215, 80, 255), 3.0f, 0, 2.0f);
        }
        if (imguiMinClick || (minHovered && lmbJustClicked)) {
            ClearTestbedEngineMouseClick();
            PostMessageW(g_hwnd, WM_TESTBED_WINDOW_CMD, TESTBED_CMD_MINIMIZE, 0);
        }

        ImVec2 wMin = ImGui::GetWindowPos();
        ImVec2 wSize = ImGui::GetWindowSize();
        ImVec2 wMax = ImVec2(wMin.x + wSize.x, wMin.y + wSize.y);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
            IsTestbedRectHovered(wMin, wMax, hasSovietPos, sovietX, sovietY, winMousePos)) {
            g_windowBarHovered = true;
            if (lmbNow) {
                ClearTestbedEngineMouseClick();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

void ImGuiHook_NewFrameAndRender()
{
    if (!g_initialized)
        return;

    if (IsDualTestbedActive()) {
        if (g_exeBase) {
            *(BYTE*)(g_exeBase + DEVICE_RESTORED_BYTE_RVA) = 1;
        }
        if (g_hwnd && IsIconic(g_hwnd)) {
            g_windowBarHovered = false;
            Sleep(10);
            return;
        }
    }

    UpdateWindowTitlePeriodic(g_hwnd);

    bool lobbyOrSyncVisible = SyncBlock_IsActive() || SaveSync_IsWaitingLobbyActive() ||
                              (NativeMenu_IsHostOnNextLoad() && SaveSync_IsWorldLoaded());
    bool syncBlocking = SyncBlock_BlocksWorldInput();
    bool overlayActive = NativeMenu_WantsInput() || lobbyOrSyncVisible || g_windowBarHovered;

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
    DrawWindowControlPill();

    ImGui::Render();
    g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
