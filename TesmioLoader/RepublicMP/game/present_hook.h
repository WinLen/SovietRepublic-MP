// SPDX-License-Identifier: GPL-3.0-only
// IDXGISwapChain::Present hook - public interface.
#pragma once

#include "tesmio_plugin.h"

extern volatile long long g_presentCallCount;

void PresentHook_Bind(const TsmHost* host);
DWORD WINAPI PresentHookThread(LPVOID); // retries until the graphics subsystem is ready
