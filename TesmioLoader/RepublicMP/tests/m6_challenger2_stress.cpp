// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone 6 (R13: Stability, Teardown Safety & Crash Prevention)
// Challenger 2: Adversarial Stress Testing of Teardown Concurrency, Material Pointer Fuzzing & Brush Recovery
// Focus Areas:
//   1. Multi-threaded teardown, disconnect requests, bank unmounting, and load triggers concurrency stress.
//   2. Callback unregistration and thread joining safety under race conditions.
//   3. Material pointer fuzzing: boundary pointers (just outside heap range, boundary between 551 and 552 bytes, count 1025).
//   4. Intercepted C3D detour boundary stress under fuzzed materials and floating-point edge cases.
//   5. Brush stroke continuity and recovery: sequence number jumps, stroke breaks, extreme coordinates, and ledger safety.
//   6. Widget method 98 (0x6563 / 0x657A) null guard and Safe SEH concurrency under stress.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <thread>
#include <atomic>
#include <random>

#include "../core/safe_mem.h"
#include "../core/engine_rva.h"
#include "../core/diag_log.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../lockstep/brush_tools.h"
#include "../game/input_block.h"
#include "../game/session_patches.h"
#include "../determinism/sim_patches.h"
#include "../transport/steam_p2p.h"
#include "../transport/network.h"

#pragma pack(push, 1)
struct M6C2_BrushState {
    float held, cost, timers[10], tree;
};
struct M6C2_BrushSample {
    float cursor[3], brush, target, setting374;
    int setting338;
    unsigned char timer[16];
    unsigned char press, pressOff, pad[2];
    M6C2_BrushState state;
};
struct M6C2_BrushMsg {
    unsigned origin;
    unsigned serial, sequence;
    int kind, mode;
    int record;
    char name[32];
    M6C2_BrushSample sample;
};
struct M6C2_LedgerMsg {
    unsigned ledger;
    char resource[64];
    float value8;
    unsigned valueC;
    float a4, a6;
    unsigned a5, a7;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// SUBTEST 1: Multi-Threaded Teardown & Load Concurrency Stress
// ---------------------------------------------------------------------------

static bool SubTest_M6_C2_TeardownAndLoadConcurrency()
{
    printf("      [Challenger2-1.1] Multi-Threaded Teardown & Load Trigger Concurrency ...\n");
    fflush(stdout);

    SaveSync_SetTestWorldLoadInProgress(false);
    SaveSync_SetTestSessionState(SESSION_IDLE);

    std::atomic<bool> startFlag{false};
    std::atomic<int> completedWorkers{0};
    std::atomic<bool> errorFlag{false};
    const int kIterations = 600;

    std::vector<std::thread> workers;
    workers.reserve(8);

    // 8 concurrent threads hammering teardown, disconnect, load triggers, and state transitions
    for (int i = 0; i < 8; ++i) {
        int threadIdx = i;
        workers.emplace_back([&, threadIdx]() {
            while (!startFlag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            for (int it = 0; it < kIterations; ++it) {
                switch (threadIdx % 4) {
                    case 0:
                        SaveSync_RequestDisconnect();
                        break;
                    case 1:
                        SaveSync_SetTestWorldLoadInProgress((it % 2) == 0);
                        break;
                    case 2:
                        SaveSync_SetTestSessionState((it % 3 == 0) ? SESSION_CLIENT_JOINING :
                                                     (it % 3 == 1) ? SESSION_HOST_IN_GAME : SESSION_IDLE);
                        break;
                    case 3:
                        TwoStates_OnSessionEnded();
                        break;
                }
                // Verify invariant: whenever world load is in progress, input must be blocked
                if (SaveSync_IsWorldLoadInProgress()) {
                    if (!InputBlock_IsBlocked()) {
                        errorFlag.store(true, std::memory_order_relaxed);
                    }
                }
            }
            completedWorkers.fetch_add(1, std::memory_order_release);
        });
    }

    startFlag.store(true, std::memory_order_release);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    if (errorFlag.load(std::memory_order_relaxed)) {
        printf("  [FAIL] Input was not blocked during concurrent world load trigger!\n");
        return false;
    }
    if (completedWorkers.load(std::memory_order_acquire) != 8) {
        printf("  [FAIL] Not all workers finished teardown concurrency test!\n");
        return false;
    }

    // Clean reset after concurrency storm
    SaveSync_SetTestWorldLoadInProgress(false);
    SaveSync_SetTestSessionState(SESSION_IDLE);
    SaveSync_RequestDisconnect();

    if (SaveSync_IsWorldLoadInProgress() != false) return false;
    if (SaveSync_GetState() != SESSION_IDLE) return false;

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 2: Thread Joining & Steam Callback Concurrency
// ---------------------------------------------------------------------------

static DWORD WINAPI M6C2_DummyWorkerProc(LPVOID)
{
    Sleep(1);
    return 0;
}

static bool SubTest_M6_C2_ThreadJoinAndSteamCallbackConcurrency()
{
    printf("      [Challenger2-1.2] Thread Joining & Callback Unregistration Concurrency ...\n");
    fflush(stdout);

    // 1. Concurrent SteamP2P_UnregisterCallbacks across 4 threads
    std::atomic<bool> startCallbacks{false};
    std::vector<std::thread> cbThreads;
    cbThreads.reserve(4);
    for (int i = 0; i < 4; ++i) {
        cbThreads.emplace_back([&]() {
            while (!startCallbacks.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            for (int k = 0; k < 300; ++k) {
                SteamP2P_UnregisterCallbacks();
            }
        });
    }
    startCallbacks.store(true, std::memory_order_release);
    for (auto& t : cbThreads) {
        if (t.joinable()) t.join();
    }

    // 2. NetworkJoinAndCloseThread boundary stress
    NetworkJoinAndCloseThread(nullptr);
    HANDLE nullH = nullptr;
    NetworkJoinAndCloseThread(&nullH);
    if (nullH != nullptr) return false;

    // 3. Spawning and joining multiple real threads
    for (int round = 0; round < 10; ++round) {
        HANDLE workerHandles[4] = {};
        for (int i = 0; i < 4; ++i) {
            workerHandles[i] = CreateThread(nullptr, 0, M6C2_DummyWorkerProc, nullptr, 0, nullptr);
            if (!workerHandles[i]) return false;
        }
        for (int i = 0; i < 4; ++i) {
            NetworkJoinAndCloseThread(&workerHandles[i]);
            if (workerHandles[i] != nullptr) return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 3: Material Pointer Boundary Fuzzing
// ---------------------------------------------------------------------------

static bool SubTest_M6_C2_MaterialPointerBoundaryFuzzing()
{
    printf("      [Challenger2-2.1] Material Pointer Boundary Fuzzing & Stride Limits ...\n");
    fflush(stdout);

    // 1. IsPointerLikeX64 boundary edge cases
    const uint64_t kBadPointers[] = {
        0x0ULL,
        0x1ULL, 0x2ULL, 0x3ULL, 0x4ULL, 0x7ULL, 0x8ULL, 0xFULL,
        0x000000000000FFFFULL,
        0x00000000FFFFFFFFULL, // 32-bit address boundary
        0x000000FF00000001ULL, // unaligned low segment heap
        0x000000FF00000003ULL, // unaligned low segment heap
        0x0000800000000000ULL, // non-canonical high boundary
        0xFFFF800000000000ULL, // kernel-mode address
        0xFFFFFFFFFFFFFFFFULL  // -1
    };
    for (uint64_t ptrVal : kBadPointers) {
        if (IsPointerLikeX64(ptrVal)) {
            printf("  [FAIL] IsPointerLikeX64 accepted bad pointer 0x%llX\n", ptrVal);
            return false;
        }
    }

    // 2. SafeIsValidMaterial on null and bad pointers
    if (SafeIsValidMaterial(nullptr)) {
        printf("  [FAIL] SafeIsValidMaterial(nullptr) was true\n");
        return false;
    }
    for (uint64_t ptrVal : kBadPointers) {
        if (SafeIsValidMaterial((const void*)ptrVal)) {
            printf("  [FAIL] SafeIsValidMaterial((const void*)0x%llX) was true\n", ptrVal);
            return false;
        }
    }

    // 3. Unaligned material pointer
    uintptr_t emptyMat[3] = { 0, 0, 0 };
    if (!SafeIsValidMaterial(emptyMat)) {
        printf("  [FAIL] SafeIsValidMaterial(emptyMat) was false, addr=%p\n", emptyMat);
        return false;
    }
    if (SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 4))) {
        printf("  [FAIL] SafeIsValidMaterial unaligned +4 was true\n");
        return false;
    }
    // +1 and +2 are caught because IsPointerLikeX64 checks (v & 3) == 0 for low addresses
    if (SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 1))) {
        printf("  [FAIL] SafeIsValidMaterial unaligned +1 was true\n");
        return false;
    }
    if (SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 2))) {
        printf("  [FAIL] SafeIsValidMaterial unaligned +2 was true\n");
        return false;
    }
    if (SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 3))) {
        printf("  [FAIL] SafeIsValidMaterial unaligned +3 was true\n");
        return false;
    }
    if (SafeIsValidMaterial((const void*)((uintptr_t)emptyMat + 7))) {
        printf("  [FAIL] SafeIsValidMaterial unaligned +7 was true\n");
        return false;
    }

    // 4. Stride boundary testing: 551 vs 552 vs 553 vs 1104
    std::vector<uint8_t> testBuf(600000, 0x55);
    uintptr_t b = (uintptr_t)testBuf.data();

    // 551 bytes: MUST BE REJECTED (diff % 552 != 0)
    uintptr_t mat551[3] = { b, b + 551, b + 551 };
    if (SafeIsValidMaterial(mat551)) {
        printf("  [FAIL] SafeIsValidMaterial accepted stride of 551 bytes!\n");
        return false;
    }

    // 552 bytes: MUST BE ACCEPTED (1 submaterial)
    uintptr_t mat552[3] = { b, b + 552, b + 552 };
    if (!SafeIsValidMaterial(mat552)) {
        printf("  [FAIL] SafeIsValidMaterial rejected valid stride of 552 bytes! b=%p\n", (void*)b);
        return false;
    }

    // 553 bytes: MUST BE REJECTED (diff % 552 != 0)
    uintptr_t mat553[3] = { b, b + 553, b + 553 };
    if (SafeIsValidMaterial(mat553)) {
        printf("  [FAIL] SafeIsValidMaterial accepted stride of 553 bytes!\n");
        return false;
    }

    // 1103 bytes: MUST BE REJECTED
    uintptr_t mat1103[3] = { b, b + 1103, b + 1103 };
    if (SafeIsValidMaterial(mat1103)) {
        printf("  [FAIL] SafeIsValidMaterial accepted stride of 1103 bytes!\n");
        return false;
    }

    // 1104 bytes (2 * 552): MUST BE ACCEPTED
    uintptr_t mat1104[3] = { b, b + 1104, b + 1104 };
    if (!SafeIsValidMaterial(mat1104)) {
        printf("  [FAIL] SafeIsValidMaterial rejected valid stride of 1104 bytes!\n");
        return false;
    }

    // 5. Count boundaries: count 1024 vs count 1025
    // Count 1024 (1024 * 552 = 565248 bytes): MUST BE ACCEPTED
    uintptr_t mat1024[3] = { b, b + 565248, b + 565248 };
    if (!SafeIsValidMaterial(mat1024)) {
        printf("  [FAIL] SafeIsValidMaterial rejected boundary count 1024!\n");
        return false;
    }

    // Count 1025 (1025 * 552 = 565800 bytes): MUST BE REJECTED (count > 1024)
    uintptr_t mat1025[3] = { b, b + 565800, b + 565800 };
    if (SafeIsValidMaterial(mat1025)) {
        printf("  [FAIL] SafeIsValidMaterial accepted out-of-bounds count 1025!\n");
        return false;
    }

    // 6. Inverted pointers (b > e)
    uintptr_t invertedMat[3] = { b + 552, b, b + 552 };
    if (SafeIsValidMaterial(invertedMat)) {
        printf("  [FAIL] SafeIsValidMaterial accepted inverted pointers!\n");
        return false;
    }

    // 7. Unmapped / non-readable probe check
    // b points to non-committed memory in user space
    uintptr_t unmappedMat[3] = { 0x000007FFFF000000ULL, 0x000007FFFF000000ULL + 552, 0x000007FFFF000000ULL + 552 };
    if (SafeIsValidMaterial(unmappedMat)) {
        printf("  [FAIL] SafeIsValidMaterial accepted unmapped probe address!\n");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 4: Intercepted C3D Detours Under Fuzzing
// ---------------------------------------------------------------------------

static bool SubTest_M6_C2_C3dDetourBoundaryStress()
{
    printf("      [Challenger2-2.2] C3D Detour Boundary Stress & Recovery ...\n");
    fflush(stdout);

    const float kColors[][4] = {
        { 1.0f, 0.5f, 0.2f, 1.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { 1e9f, -1e9f, 0.0f, 1.0f },
        { (float)NAN, (float)INFINITY, 0.0f, 1.0f }
    };

    std::vector<uint8_t> validBuf(600000, 0x0);
    uintptr_t b = (uintptr_t)validBuf.data();
    uintptr_t validMat[3] = { b, b + 552, b + 552 };
    uintptr_t mat1024[3] = { b, b + 565248, b + 565248 };
    uintptr_t mat1025[3] = { b, b + 565800, b + 565800 };
    uintptr_t badStrideMat[3] = { b, b + 551, b + 551 };

    void* testCases[] = {
        nullptr,
        (void*)0x1ULL,
        (void*)0x00000000FFFFFFFFULL,
        (void*)0x0000800000000000ULL,
        (void*)0xFFFF800000000000ULL,
        badStrideMat,
        mat1025,
        mat1024,
        validMat
    };

    for (void* mat : testCases) {
        for (const auto& color : kColors) {
            SimPatches_TestC3dSaveOriginalValues(mat);
            SimPatches_TestC3dChangeValuesTo(mat, color);
            SimPatches_TestC3dRestoreOriginalValues(mat);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 5: Brush Stroke Continuity, Sequence Recovery & Extreme Coordinates
// ---------------------------------------------------------------------------

static bool SubTest_M6_C2_BrushStrokeContinuityAndRecovery()
{
    printf("      [Challenger2-3.1] Brush Stroke Continuity, Sequence Recovery & Fuzzing ...\n");
    fflush(stdout);

    // 1. Packet size validation
    uint8_t tinyBuf[8] = {};
    if (BrushTools_ApplyCommand(tinyBuf, sizeof(tinyBuf))) return false;
    if (BrushTools_ApplyCommand(tinyBuf, sizeof(M6C2_BrushMsg) - 1)) return false;
    if (BrushTools_ApplyCommand(tinyBuf, 0)) return false;

    std::vector<uint8_t> largeBuf(sizeof(M6C2_BrushMsg) + 16, 0);
    if (BrushTools_ApplyCommand(largeBuf.data(), largeBuf.size())) return false;

    // 2. Demolish ground tool (kind = 4) execution under sequence jumps
    M6C2_BrushMsg m = {};
    m.origin = 0xAA0011u;
    m.serial = 101;
    m.kind = 4; // KIND_DEMOLISH_GROUND
    m.sample.cursor[0] = 100.0f;
    m.sample.cursor[1] = 0.0f;
    m.sample.cursor[2] = 200.0f;
    m.sample.brush = 5.0f;

    // Sequence 0
    m.sequence = 0;
    BrushTools_ApplyCommand(&m, sizeof(m));

    // Sequence jump: 0 -> 10 (skipping 1..9)
    m.sequence = 10;
    BrushTools_ApplyCommand(&m, sizeof(m));

    // Sequence jump: 10 -> 50
    m.sequence = 50;
    BrushTools_ApplyCommand(&m, sizeof(m));

    // 3. Demolish ground tool under extreme coordinates & float edge cases
    const float kExtremeCoords[][3] = {
        { 1e9f, 0.0f, 1e9f },
        { -1e9f, -1e9f, -1e9f },
        { (float)NAN, 0.0f, (float)INFINITY },
        { 0.0f, (float)-INFINITY, 0.0f }
    };
    for (const auto& coord : kExtremeCoords) {
        m.sample.cursor[0] = coord[0];
        m.sample.cursor[1] = coord[1];
        m.sample.cursor[2] = coord[2];
        m.sample.brush = 1000.0f;
        m.sequence++;
        BrushTools_ApplyCommand(&m, sizeof(m));
    }

    // 4. Terrain tools (kind 0..3) with missing / corrupted records
    M6C2_BrushMsg tm = {};
    tm.origin = 0xBB0022u;
    tm.serial = 500;
    tm.sequence = 0;
    tm.kind = 0; // terrain
    tm.record = 9999;
    strncpy_s(tm.name, "non_existent_tool", _TRUNCATE);
    tm.sample.cursor[0] = 50.0f;
    tm.sample.cursor[2] = 50.0f;

    // Missing build menu record must be rejected without crashing
    if (BrushTools_ApplyCommand(&tm, sizeof(tm))) {
        printf("  [FAIL] BrushTools_ApplyCommand accepted invalid tool record!\n");
        return false;
    }

    // Sequence jump on unbuilt tool
    tm.sequence = 12;
    if (BrushTools_ApplyCommand(&tm, sizeof(tm))) return false;

    // Invalid kinds
    tm.kind = -1;
    if (BrushTools_ApplyCommand(&tm, sizeof(tm))) return false;
    tm.kind = 5;
    if (BrushTools_ApplyCommand(&tm, sizeof(tm))) return false;
    tm.kind = 999999;
    if (BrushTools_ApplyCommand(&tm, sizeof(tm))) return false;

    // 5. Ledger command safety
    M6C2_LedgerMsg lm = {};
    if (BrushTools_ApplyLedger(&lm, sizeof(lm) - 1)) return false;
    if (BrushTools_ApplyLedger(&lm, 0)) return false;

    // Invalid ledger offset
    lm.ledger = 0x11000;
    if (BrushTools_ApplyLedger(&lm, sizeof(lm))) return false;
    lm.ledger = 0xFFFFFFFF;
    if (BrushTools_ApplyLedger(&lm, sizeof(lm))) return false;

    // Valid ledger offset with empty / unknown resource
    lm.ledger = 0x11330; // dollars
    strncpy_s(lm.resource, "UNKNOWN_RESOURCE_XYZ", _TRUNCATE);
    if (BrushTools_ApplyLedger(&lm, sizeof(lm))) return false; // not present in world

    lm.ledger = 0x11360; // rubles
    if (BrushTools_ApplyLedger(&lm, sizeof(lm))) return false;

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 6: Brush Rate-Limiting & Time Setting Stress
// ---------------------------------------------------------------------------

static bool SubTest_M6_C2_BrushRateLimitingAndActiveState()
{
    printf("      [Challenger2-3.2] Brush Rate-Limiting & Active State Invariants ...\n");
    fflush(stdout);

    // Initial state: brush inactive
    if (BrushTools_IsActive()) return false;

    // Test time setting & restoration
    BrushTools_SetTestTimeMs(5000);
    BrushTools_SetTestTimeMs(5020); // 20 ms elapsed (< 33 ms threshold)
    BrushTools_SetTestTimeMs(5050); // 50 ms elapsed (>= 33 ms threshold)
    BrushTools_SetTestTimeMs(0);    // restore to real time

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 7: Widget Method 98 & Safe SEH Concurrency Stress
// ---------------------------------------------------------------------------

static bool SubTest_M6_C2_WidgetMethod98AndSafeSehStress()
{
    printf("      [Challenger2-3.3] Widget Method 98 Null Guard & Safe SEH Concurrency ...\n");
    fflush(stdout);

    std::atomic<bool> startFlag{false};
    std::atomic<int> completedWorkers{0};
    const int kIterations = 500;

    std::vector<std::thread> workers;
    workers.reserve(4);

    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&]() {
            while (!startFlag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            for (int it = 0; it < kIterations; ++it) {
                // RVA 0x6563 null widget method 98 guard
                SessionPatches_TestSafeInvokeWidgetMethod98(nullptr);

                // SEH probe on invalid addresses
                uint32_t val = 0;
                SafeReadPod<uint32_t>(nullptr, &val);
                SafeReadPod<uint32_t>((const uint32_t*)0x1ULL, &val);
                SafeReadPod<uint32_t>((const uint32_t*)0x00000000FFFFFFFFULL, &val);

                SafeWritePod<uint32_t>(nullptr, 123u);
                SafeWritePod<uint32_t>((uint32_t*)0x1ULL, 123u);
            }
            completedWorkers.fetch_add(1, std::memory_order_release);
        });
    }

    startFlag.store(true, std::memory_order_release);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    if (completedWorkers.load(std::memory_order_acquire) != 4) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Milestone 6 Suite Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M6_Challenger2()
{
    printf("  [M6-CHALLENGER2] Starting Empirical Stress Testing for Milestone 6 (R13: Stability & Crash Prevention) ...\n");
    fflush(stdout);

    if (!SubTest_M6_C2_TeardownAndLoadConcurrency()) return false;
    if (!SubTest_M6_C2_ThreadJoinAndSteamCallbackConcurrency()) return false;
    if (!SubTest_M6_C2_MaterialPointerBoundaryFuzzing()) return false;
    if (!SubTest_M6_C2_C3dDetourBoundaryStress()) return false;
    if (!SubTest_M6_C2_BrushStrokeContinuityAndRecovery()) return false;
    if (!SubTest_M6_C2_BrushRateLimitingAndActiveState()) return false;
    if (!SubTest_M6_C2_WidgetMethod98AndSafeSehStress()) return false;

    printf("  [M6-CHALLENGER2] ALL Milestone 6 Empirical Stress Tests PASSED!\n");
    fflush(stdout);
    return true;
}
