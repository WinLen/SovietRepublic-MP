// SPDX-License-Identifier: GPL-3.0-only
// Dear ImGui overlay rendered from the IDXGISwapChain::Present hook.
// Used exclusively for 3D state border lines, 3D partner cursor/pings,
// the top-center Mini-HUD, the Load/New Game Co-op launch bar, and the
// world-transfer sync screen (no INSERT floating window).
#pragma once

#include "tesmio_plugin.h"
#include <dxgi.h>

void ImGuiHook_Bind(const TsmHost* host);

// Called every frame from HookedPresent; creates the ImGui context and the
// DX11/Win32 backends on first call.
void ImGuiHook_EnsureInit(IDXGISwapChain* swapChain);

// Called every frame right before the original Present.
void ImGuiHook_NewFrameAndRender();

// Test helpers for unit tests in tests/test_main.cpp.
void ImGuiHook_UpdateWindowTitleForTest(HWND hwnd);
void ImGuiHook_ResetTitleTrackingForTest();
