// SPDX-License-Identifier: GPL-3.0-only
// Hooks IDXGISwapChain::Present by patching its vtable slot (shared across
// all swap chains in the DirectX runtime), so it fires every frame
// regardless of what the local player is doing.
#include "present_hook.h"
#include "../world_sync/local_builds.h"
#include "imgui_hook.h"
#include "../world_sync/terrain/terrain_hook.h"
#include "../world_sync/buildings/demolish_hook.h"
#include "session_patches.h"
#include "../determinism/determinism.h"
#include "../determinism/sim_patches.h"
#include "input_block.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../session/sync_check.h"
#include "../session/settings_sync.h"
#include "../session/game_speed.h"
#include "../session/weather.h"
#include "../session/money.h"
#include "../core/engine_rva.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>

extern bool SimPatches_GetActivePlacementInfo(char* outName, size_t maxLen, float* outX, float* outY, float* outZ);


#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h> // IDXGISwapChain1::Present1 (re-entry fallback)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

void PresentHook_Bind(const TsmHost* host)
{
    TsmBind(host);
}

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

static PresentFn g_origPresent = nullptr;
volatile long long g_presentCallCount = 0;

// Depth of HookedPresent on this thread. An overlay that hooks the same
// Present after us (the Steam overlay does) may keep HookedPresent as its own
// "original": g_origPresent then runs the overlay, which calls HookedPresent
// again, which calls g_origPresent again - without a guard the stack runs out
// within a frame and the game crashes.
static thread_local int t_presentDepth = 0;
static volatile bool g_reentryLogged = false;

// Presents the frame without g_origPresent (which leads back into the
// overlay): through IDXGISwapChain1::Present1, which this module never hooks.
static HRESULT PresentBypassingChain(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) || !sc1)
        return S_OK; // nothing better possible - the frame is dropped instead of the game
    DXGI_PRESENT_PARAMETERS params = {};
    HRESULT hr = sc1->Present1(SyncInterval, Flags, &params);
    sc1->Release();
    return hr;
}

struct CamVec4 {
    float x;
    float y;
    float w;
};

CamVec4 ProjectPlacementPoint(float wx, float wy, float wz)
{
    CamVec4 pt = { 0.0f, 0.0f, 0.0f };
    if (!g_exeBase) return pt;
    __try {
        const float* mat = (const float*)(g_exeBase + CAMERA_RVA + CAMERA_VIEWPROJ_OFFSET);
        pt.x = wx * mat[0] + wy * mat[4] + wz * mat[8]  + mat[12];
        pt.y = wx * mat[1] + wy * mat[5] + wz * mat[9]  + mat[13];
        pt.w = wx * mat[3] + wy * mat[7] + wz * mat[11] + mat[15];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pt.w = 0.0f;
    }
    return pt;
}

void CleanBuildingName(const char* raw, char* out, size_t maxLen)
{
    if (!raw || !out || maxLen == 0) return;
    const char* p = raw;
    for (const char* s = raw; *s; s++) {
        if (*s == '/' || *s == '\\') {
            p = s + 1;
        }
    }
    strncpy_s(out, maxLen, p, _TRUNCATE);
    size_t len = strlen(out);
    if (len > 4 && _stricmp(out + len - 4, ".ini") == 0) {
        out[len - 4] = '\0';
    }
}

static void DrawPlacementTextBadge(ImDrawList* dl, const ImVec2& pos, const char* text)
{
    if (!dl || !text || text[0] == '\0') return;
    char label[160] = {};
    snprintf(label, sizeof(label), "★ %s", text);
    ImVec2 sz = ImGui::CalcTextSize(label);
    ImVec2 pMin(pos.x - sz.x * 0.5f - 8.0f, pos.y - sz.y - 14.0f);
    ImVec2 pMax(pos.x + sz.x * 0.5f + 8.0f, pos.y - 2.0f);
    dl->AddRectFilled(pMin, pMax, IM_COL32(20, 24, 30, 225), 4.0f);
    dl->AddRect(pMin, pMax, IM_COL32(235, 185, 45, 255), 4.0f, 0, 1.5f);
    dl->AddText(ImVec2(pos.x - sz.x * 0.5f, pMin.y + 4.0f), IM_COL32(255, 235, 175, 255), label);
}

void PresentHook_DrawPlacementBadgeForeground()
{
    char rawName[128] = {};
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!SimPatches_GetActivePlacementInfo(rawName, sizeof(rawName), &x, &y, &z) || rawName[0] == '\0') {
        return;
    }

    if (!ImGui::GetCurrentContext()) return;
    ImVec2 dispSize = ImGui::GetIO().DisplaySize;
    if (dispSize.x <= 0.0f || dispSize.y <= 0.0f) return;

    CamVec4 proj = ProjectPlacementPoint(x, y + 2.5f, z);
    if (proj.w > 0.5f && isfinite(proj.w)) {
        float rcpW = 1.0f / proj.w;
        float nx = proj.x * rcpW;
        float ny = proj.y * rcpW;
        if (nx >= -1.5f && nx <= 1.5f && ny >= -1.5f && ny <= 1.5f) {
            ImVec2 screenPos((nx * 0.5f + 0.5f) * dispSize.x, (0.5f - ny * 0.5f) * dispSize.y);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            if (dl) {
                char cleanName[128] = {};
                CleanBuildingName(rawName, cleanName, sizeof(cleanName));
                DrawPlacementTextBadge(dl, screenPos, cleanName);
            }
        }
    }
}

static HRESULT PresentFrame(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    if (t_presentDepth > 0) {
        if (!g_reentryLogged) {
            g_reentryLogged = true;
            Logf("republicmp  PRESENT: re-entered through another overlay's Present hook (Steam overlay?) - "
                 "presenting through Present1 to break the loop");
        }
        return PresentBypassingChain(pSwapChain, SyncInterval, Flags);
    }
    t_presentDepth++;
    HRESULT hr = PresentFrame(pSwapChain, SyncInterval, Flags);
    t_presentDepth--;
    return hr;
}

static HRESULT PresentFrame(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    g_presentCallCount++;
    // While MainThread is inside o_loadWorld() (0x28CFD0 -> 0x005920), the engine's dedicated
    // Loading Screen Render Thread (0x4AC020) calls Present after releasing the engine D3D11 lock
    // while still holding CRITICAL_SECTION 0x140A564A8. Never touch the shared D3D11 Immediate Context
    // via ImGui or run gameplay *_Tick() callbacks on half-loaded world structures during loading.
    if (Determinism_WorldLoading()) {
        return g_origPresent(pSwapChain, SyncInterval, Flags);
    }
    SessionPatches_Tick(); // camera/frame-rate patches while connected (see session_patches.h)
    SimPatches_Tick();     // determinism patches back once the probe ends (see sim_patches.h)
    InputBlock_Tick();  // keep the game's mouse-button bytes clear while the join screen is up
    TerrainHook_Tick(); // flush last frame's terrain-edit log, F8 verbose toggle
    TerrainSync_Tick(); // send this frame's terrain edits, apply received ones (before roads/buildings)
    DemolishHook_Tick(); // send demolitions captured this frame (after the terrain batch, for its fence)
    BuildSync_Tick(); // apply any building placements received over the network
    GameSpeed_SyncTick(); // send local speed changes (host decides, see game_speed.h)
    Weather_SyncTick(); // host: send the weather (see weather.h)
    SaveSync_Tick(); // host: send a requested re-sync announcement
    SyncCheck_Tick(); // player: send the hourly state summary
    Money_SyncTick(); // host: send the money balances (see money.h)
    SettingsSync_Tick(); // building window settings: send local changes, apply received ones
    RoadSync_Tick(); // apply infrastructure received over the network
    LocalBuilds_Tick(); // drag state, captured commits, F9 debug spawn

    ImGuiHook_EnsureInit(pSwapChain); // one-time: grabs device/hwnd, sets up ImGui + WndProc hook
    ImGuiHook_NewFrameAndRender();    // draws the overlay on top of the game's own frame

    return g_origPresent(pSwapChain, SyncInterval, Flags);
}

// Creates a temporary swap chain to read the real Present address from its
// vtable (index 8, per the COM/DXGI standard), patches that one pointer,
// then destroys the temporary objects - the shared vtable stays.
static bool InstallPresentHook()
{
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "republicmp_dummy_wnd";
    RegisterClassExA(&wc);

    HWND dummyWnd = CreateWindowExA(0, wc.lpszClassName, "republicmp_dummy",
                                     WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                     nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummyWnd) {
        Logf("republicmp  PRESENT: failed to create hidden window");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 100;
    scd.BufferDesc.Height = 100;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyWnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11Device* dummyDevice = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &dummySwapChain, &dummyDevice, nullptr, &dummyContext);

    if (FAILED(hr) || !dummySwapChain) {
        Logf("republicmp  PRESENT: HARDWARE failed, hr=0x%08lX - trying WARP", (unsigned long)hr);
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &scd, &dummySwapChain, &dummyDevice, nullptr, &dummyContext);
    }

    if (FAILED(hr) || !dummySwapChain) {
        Logf("republicmp  PRESENT: WARP also failed, hr=0x%08lX", (unsigned long)hr);
        DestroyWindow(dummyWnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(dummySwapChain);
    g_origPresent = (PresentFn)vtable[8];

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Logf("republicmp  PRESENT: VirtualProtect on vtable[8] failed");
        dummyContext->Release();
        dummySwapChain->Release();
        dummyDevice->Release();
        DestroyWindow(dummyWnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }
    vtable[8] = (void*)&HookedPresent;
    VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);

    dummyContext->Release();
    dummySwapChain->Release();
    dummyDevice->Release();
    DestroyWindow(dummyWnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    Logf("republicmp  PRESENT: hook installed, g_origPresent=%p", (void*)g_origPresent);
    return true;
}

// Retries until the graphics subsystem is ready (fails if called too early).
DWORD WINAPI PresentHookThread(LPVOID)
{
    const int maxAttempts = 30;
    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        if (InstallPresentHook()) {
            return 0;
        }
        Logf("republicmp  PRESENT: attempt %d/%d failed, retrying in 1s",
             attempt, maxAttempts);
        Sleep(1000);
    }
    Logf("republicmp  PRESENT: giving up after %d attempts", maxAttempts);
    return 0;
}
