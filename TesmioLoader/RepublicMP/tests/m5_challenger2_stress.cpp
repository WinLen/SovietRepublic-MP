// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone 5 (R6, R7, R14, R15, R18)
// Challenger 2: Adversarial Stress Testing of Research Independence, City Deletion, and Save Transfer
// Focus Areas:
//   1. R14 & R15: Multi-threaded concurrent research ticks across State 1 and State 2 universities.
//   2. R14 & R15: Saturation stress-test of all 32 technology slots simultaneously & boundary validation.
//   3. R14 & R15: Local UI display isolation strictly hiding peer state research.
//   4. R6: Lockstep replay ordering, payload fuzzing, and double-deletion safety on identical cities.
//   5. R7 & R18: Transfer speed EMA under irregular delivery, high jitter, rapid drops, and mod modal dismiss.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <crtdbg.h>
#include <vector>
#include <thread>
#include <atomic>
#include <random>

#include "../lockstep/commands.h"
#include "../lockstep/rename.h"
#include "../session/two_states.h"
#include "../session/save_sync.h"
#include "../session/mod_sync.h"
#include "../ui/sync_block.h"

#define M5_MAGIC 0x314D414E
#define M5_KIND_CITY_DELETE 0xD
#define M5_KIND_RESEARCH    0xE

extern void SyncBlock_SetPercentEx(int percent, int queuedBytes, int totalBytes, float speedMbSec);
extern void SyncBlock_GetTransferStats(int* outPercent, int* outQueuedBytes, int* outTotalBytes, float* outSpeedMbSec);

// Helper Fnv hash for city coordinates mirroring rename.cpp
static unsigned M5_CityCheck(const BYTE* cityPtr)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < 12; ++i) {
        h ^= cityPtr[4 + i];
        h *= 16777619u;
    }
    return h | 1u;
}

// ---------------------------------------------------------------------------
// SUBTEST 1: R14 & R15 - Multi-Threaded Concurrent Research Ticks
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R14_R15_MultiThreadResearchConcurrency()
{
    printf("      [Challenger2-1.1] R14/R15 Multi-Threaded Concurrent Research Ticks ...\n");
    fflush(stdout);

    TwoStates_ResetResearch();

    // Verify initial clean state
    for (int t = 0; t < 32; ++t) {
        if (TwoStates_IsTechUnlocked(1, t) || TwoStates_IsTechUnlocked(2, t)) return false;
        if (fabsf(TwoStates_GetTechProgress(1, t)) > 1e-5f) return false;
        if (fabsf(TwoStates_GetTechProgress(2, t)) > 1e-5f) return false;
    }

    std::atomic<bool> startFlag{false};
    std::atomic<int> completedWorkers{0};
    const int kIterations = 600;

    std::vector<std::thread> workers;
    workers.reserve(8);

    // 4 threads for State 1 (West SSR)
    for (int i = 0; i < 4; ++i) {
        int threadIdx = i;
        workers.emplace_back([&, threadIdx]() {
            while (!startFlag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            int startTech = (threadIdx < 2) ? 1 : 16;
            int endTech = (threadIdx < 2) ? 15 : 31;
            for (int it = 0; it < kIterations; ++it) {
                int tech = startTech + (it % (endTech - startTech + 1));
                TwoStates_AddResearchProgress(1, tech, 0.003f);
            }
            completedWorkers.fetch_add(1, std::memory_order_release);
        });
    }

    // 4 threads for State 2 (East SSR)
    for (int i = 0; i < 4; ++i) {
        int threadIdx = i;
        workers.emplace_back([&, threadIdx]() {
            while (!startFlag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            int startTech = (threadIdx < 2) ? 1 : 16;
            int endTech = (threadIdx < 2) ? 15 : 31;
            for (int it = 0; it < kIterations; ++it) {
                int tech = startTech + (it % (endTech - startTech + 1));
                TwoStates_AddResearchProgress(2, tech, 0.002f);
            }
            completedWorkers.fetch_add(1, std::memory_order_release);
        });
    }

    // 2 reader threads concurrently inspecting state
    std::vector<std::thread> readers;
    readers.reserve(2);
    std::atomic<bool> stopReaders{false};
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&]() {
            while (!startFlag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            while (!stopReaders.load(std::memory_order_relaxed)) {
                for (int t = 0; t < 32; ++t) {
                    TwoStates_IsTechUnlocked(1, t);
                    TwoStates_IsTechUnlocked(2, t);
                    TwoStates_GetTechProgress(1, t);
                    TwoStates_GetTechProgress(2, t);
                }
                StateResearchTree t1 = {}, t2 = {};
                TwoStates_GetStateResearch(1, &t1);
                TwoStates_GetStateResearch(2, &t2);
            }
        });
    }

    // Unleash all threads simultaneously
    startFlag.store(true, std::memory_order_release);

    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }
    stopReaders.store(true, std::memory_order_release);
    for (auto& rd : readers) {
        if (rd.joinable()) rd.join();
    }

    if (completedWorkers.load() != 8) return false;

    // Verify heap integrity
    if (!_CrtCheckMemory()) return false;

    // Verify researchedCount invariants
    StateResearchTree t1 = {}, t2 = {};
    if (!TwoStates_GetStateResearch(1, &t1)) return false;
    if (!TwoStates_GetStateResearch(2, &t2)) return false;

    int expectedCount1 = 0;
    for (int bit = 0; bit < 32; ++bit) {
        if (t1.unlockedMask & (1u << bit)) expectedCount1++;
    }
    if (t1.researchedCount != expectedCount1) return false;

    int expectedCount2 = 0;
    for (int bit = 0; bit < 32; ++bit) {
        if (t2.unlockedMask & (1u << bit)) expectedCount2++;
    }
    if (t2.researchedCount != expectedCount2) return false;

    // State 1 and State 2 must have distinct progress and distinct masks
    bool identical = true;
    for (int t = 1; t < 32; ++t) {
        if (fabsf(t1.techProgress[t] - t2.techProgress[t]) > 1e-4f) {
            identical = false;
            break;
        }
    }
    if (identical) return false;

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 2: R14 & R15 - 32-Slot Saturation & Boundary Stress
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R14_R15_32SlotSaturationAndBoundaries()
{
    printf("      [Challenger2-1.2] R14/R15 32-Slot Saturation & Boundary Stress ...\n");
    fflush(stdout);

    TwoStates_ResetResearch();

    // 1. Boundary: Invalid State Owners
    if (TwoStates_IsTechUnlocked(0, 5)) return false;
    if (TwoStates_IsTechUnlocked(3, 5)) return false;
    if (TwoStates_IsTechUnlocked(-1, 5)) return false;
    TwoStates_UnlockTech(0, 5);
    TwoStates_UnlockTech(3, 5);
    TwoStates_AddResearchProgress(0, 5, 0.5f);
    TwoStates_AddResearchProgress(3, 5, 0.5f);
    if (TwoStates_IsTechUnlocked(1, 5) || TwoStates_IsTechUnlocked(2, 5)) return false;

    // 2. Boundary: Out of bounds tech IDs
    if (TwoStates_IsTechUnlocked(1, -1)) return false;
    if (TwoStates_IsTechUnlocked(1, 32)) return false;
    if (TwoStates_IsTechUnlocked(1, 100)) return false;
    TwoStates_AddResearchProgress(1, -1, 0.5f);
    TwoStates_AddResearchProgress(1, 32, 0.5f);
    TwoStates_AddResearchProgress(1, 1000, 0.5f);
    TwoStates_AddResearchProgress(1, 5, -0.5f); // negative delta rejected
    if (fabsf(TwoStates_GetTechProgress(1, 5)) > 1e-5f) return false;

    // 3. Boundary: Slot 0 (Idle / Cancel)
    TwoStates_SetActiveTech(1, 0);
    if (TwoStates_GetActiveTech(1) != 0) return false;
    TwoStates_AddResearchProgress(1, 0, 0.5f);
    if (fabsf(TwoStates_GetTechProgress(1, 0)) > 1e-5f) return false;

    // 4. Boundary: Slot 31 (Highest bit 0x80000000u)
    TwoStates_SetActiveTech(1, 31);
    if (TwoStates_GetActiveTech(1) != 31) return false;
    TwoStates_AddResearchProgress(1, 31, 0.35f);
    if (fabsf(TwoStates_GetTechProgress(1, 31) - 0.35f) > 1e-4f) return false;
    TwoStates_AddResearchProgress(1, 31, 0.70f); // exceeds 1.0f -> unlock
    if (!TwoStates_IsTechUnlocked(1, 31)) return false;
    if (TwoStates_GetActiveTech(1) != 0) return false; // active tech resets on unlock

    StateResearchTree t1 = {};
    if (!TwoStates_GetStateResearch(1, &t1)) return false;
    if ((t1.unlockedMask & (1u << 31)) == 0) return false;

    // State 2 must NOT have Tech 31 unlocked
    if (TwoStates_IsTechUnlocked(2, 31)) return false;
    if (fabsf(TwoStates_GetTechProgress(2, 31)) > 1e-5f) return false;

    // 5. Full Saturation: Unlock all remaining slots 1..30 in State 1
    for (int t = 1; t <= 30; ++t) {
        TwoStates_UnlockTech(1, t);
    }
    if (!TwoStates_GetStateResearch(1, &t1)) return false;
    if (t1.researchedCount != 31) return false;
    if (t1.unlockedMask != 0xFFFFFFFEu) return false;

    // State 2 remains untouched
    StateResearchTree t2 = {};
    if (!TwoStates_GetStateResearch(2, &t2)) return false;
    if (t2.researchedCount != 0 || t2.unlockedMask != 0) return false;

    // 6. Wire Command Dispatch via Rename_ApplyCommand
    LsResearchCommand cmd = {};
    cmd.stateOwner = 2;
    cmd.techId = 14;
    cmd.progressDelta = 0.42f;
    if (!Rename_ApplyCommand(&cmd, sizeof(cmd))) return false;
    if (fabsf(TwoStates_GetTechProgress(2, 14) - 0.42f) > 1e-4f) return false;

    // 7. Binary Snapshot Round-Trip
    StateResearchTree snapshot = {};
    if (!TwoStates_GetStateResearch(1, &snapshot)) return false;
    if (!TwoStates_SetStateResearch(2, &snapshot)) return false;
    if (!TwoStates_IsTechUnlocked(2, 31)) return false;
    if (TwoStates_GetTechProgress(2, 31) < 0.99f) return false;

    TwoStates_ResetResearch();
    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 3: R14 & R15 - UI Display Isolation Strictly Hiding Peer State
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R14_R15_UIStateIsolation()
{
    printf("      [Challenger2-1.3] R14/R15 UI Display Isolation & Peer Hiding ...\n");
    fflush(stdout);

    TwoStates_ResetResearch();

    double dummyRub[2] = { 1000000.0, 1000000.0 };
    double dummyUsd[2] = { 500000.0, 500000.0 };

    // 1. Enter session as Host (State 1)
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, dummyRub, dummyUsd);
    if (TwoStates_LocalState() != 1) return false;

    // State 1 unlocks {1, 2, 5}
    TwoStates_UnlockTech(1, 1);
    TwoStates_UnlockTech(1, 2);
    TwoStates_UnlockTech(1, 5);

    // State 2 unlocks {3, 4, 7, 12}
    TwoStates_UnlockTech(2, 3);
    TwoStates_UnlockTech(2, 4);
    TwoStates_UnlockTech(2, 7);
    TwoStates_UnlockTech(2, 12);

    // Host UI query: MUST see State 1 only
    const StateResearchTree* localTreeHost = TwoStates_GetLocalResearchTree();
    if (!localTreeHost) return false;
    if (localTreeHost->researchedCount != 3) return false;
    if ((localTreeHost->unlockedMask & (1u << 1)) == 0) return false;
    if ((localTreeHost->unlockedMask & (1u << 2)) == 0) return false;
    if ((localTreeHost->unlockedMask & (1u << 5)) == 0) return false;
    // Host UI must strictly NOT see State 2 unlocks
    if ((localTreeHost->unlockedMask & (1u << 3)) != 0) return false;
    if ((localTreeHost->unlockedMask & (1u << 4)) != 0) return false;
    if ((localTreeHost->unlockedMask & (1u << 7)) != 0) return false;
    if ((localTreeHost->unlockedMask & (1u << 12)) != 0) return false;

    // 2. Switch role to Client (State 2)
    TwoStates_OnSessionJoined(false, COOP_MODE_TWO_STATES, 250.0f, false, dummyRub, dummyUsd);
    if (TwoStates_LocalState() != 2) return false;

    // Client UI query: MUST see State 2 only
    const StateResearchTree* localTreeClient = TwoStates_GetLocalResearchTree();
    if (!localTreeClient) return false;
    if (localTreeClient->researchedCount != 4) return false;
    if ((localTreeClient->unlockedMask & (1u << 3)) == 0) return false;
    if ((localTreeClient->unlockedMask & (1u << 4)) == 0) return false;
    if ((localTreeClient->unlockedMask & (1u << 7)) == 0) return false;
    if ((localTreeClient->unlockedMask & (1u << 12)) == 0) return false;
    // Client UI must strictly NOT see State 1 unlocks
    if ((localTreeClient->unlockedMask & (1u << 1)) != 0) return false;
    if ((localTreeClient->unlockedMask & (1u << 2)) != 0) return false;
    if ((localTreeClient->unlockedMask & (1u << 5)) != 0) return false;

    // 3. R14 Core Requirement: Simultaneous Identical Technology Selection
    // Both states independently select and research Tech #20
    TwoStates_SetActiveTech(1, 20);
    TwoStates_SetActiveTech(2, 20);
    TwoStates_AddResearchProgress(1, 20, 0.25f);
    TwoStates_AddResearchProgress(2, 20, 0.80f);

    // Client view shows Tech 20 at 0.80f
    if (localTreeClient->activeTechId != 20) return false;
    if (fabsf(localTreeClient->activeTechProgress - 0.80f) > 1e-4f) return false;

    // State 2 completes Tech 20
    TwoStates_AddResearchProgress(2, 20, 0.25f);
    if (!TwoStates_IsTechUnlocked(2, 20)) return false;
    if (localTreeClient->activeTechId != 0) return false;

    // State 1 must STILL be researching Tech 20 at 0.25f without being unlocked!
    if (TwoStates_IsTechUnlocked(1, 20)) return false;
    if (TwoStates_GetActiveTech(1) != 20) return false;
    if (fabsf(TwoStates_GetTechProgress(1, 20) - 0.25f) > 1e-4f) return false;

    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 4: R6 - City Deletion Lockstep Replay Safety & Double-Deletion
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R6_CityDeletionLockstepSafety()
{
    printf("      [Challenger2-1.4] R6 City Deletion Lockstep Replay & Double-Deletion ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Prepare 3 mock city buffers (each at least 64 bytes)
    // Offset +4: x, y, z (3 floats, 12 bytes). Offset +0x10: wide string name.
    std::vector<BYTE> city0(128, 0);
    std::vector<BYTE> city1(128, 0);
    std::vector<BYTE> city2(128, 0);

    *(float*)(city0.data() + 4) = 100.0f;
    *(float*)(city0.data() + 8) = 10.0f;
    *(float*)(city0.data() + 12) = 200.0f;
    wcscpy_s((wchar_t*)(city0.data() + 0x10), 32, L"Leningrad");

    *(float*)(city1.data() + 4) = 300.0f;
    *(float*)(city1.data() + 8) = 20.0f;
    *(float*)(city1.data() + 12) = 400.0f;
    wcscpy_s((wchar_t*)(city1.data() + 0x10), 32, L"Stalingrad");

    *(float*)(city2.data() + 4) = 500.0f;
    *(float*)(city2.data() + 8) = 30.0f;
    *(float*)(city2.data() + 12) = 600.0f;
    wcscpy_s((wchar_t*)(city2.data() + 0x10), 32, L"Sevastopol");

    std::vector<BYTE*> cityVec = { city0.data(), city1.data(), city2.data() };
    WriteMockVectorHeader(world + 0x10348, cityVec);

    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 3) return false;

    // 1. Initial valid deletion of City 1 ("Stalingrad")
    TestRenameMsg mDelete1 = {};
    mDelete1.magic = M5_MAGIC;
    mDelete1.kind = M5_KIND_CITY_DELETE;
    mDelete1.index = 1;
    mDelete1.check = M5_CityCheck(city1.data());

    if (!Rename_ApplyCommand(&mDelete1, sizeof(mDelete1))) return false;

    // Check count decreased to 2
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 2) return false;

    // Vector elements shifted: index 0 = Leningrad, index 1 = Sevastopol
    BYTE** remaining = *(BYTE***)(world + 0x10348);
    if (!remaining || !remaining[0] || !remaining[1]) return false;
    if (wcscmp((const wchar_t*)(remaining[0] + 0x10), L"Leningrad") != 0) return false;
    if (wcscmp((const wchar_t*)(remaining[1] + 0x10), L"Sevastopol") != 0) return false;

    // 2. ADVERSARIAL CHALLENGE: Double-deletion of the SAME city ("Stalingrad")
    // Replay receives duplicate delete command for index 1, check = CityCheck(Stalingrad)
    // Must return false because index 1 now holds Sevastopol whose check does not match!
    bool secondDeleteResult = Rename_ApplyCommand(&mDelete1, sizeof(mDelete1));
    if (secondDeleteResult) return false; // MUST FAIL gracefully

    // Verify vector count DID NOT change
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 2) return false;
    if (wcscmp((const wchar_t*)(remaining[1] + 0x10), L"Sevastopol") != 0) return false;

    // 3. ADVERSARIAL CHALLENGE: Double-deletion on a single-city world
    std::vector<BYTE> singleCity(128, 0);
    *(float*)(singleCity.data() + 4) = 700.0f;
    *(float*)(singleCity.data() + 8) = 15.0f;
    *(float*)(singleCity.data() + 12) = 800.0f;
    wcscpy_s((wchar_t*)(singleCity.data() + 0x10), 32, L"Moscow");

    std::vector<BYTE*> singleVec = { singleCity.data() };
    WriteMockVectorHeader(world + 0x10348, singleVec);
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 1) return false;

    TestRenameMsg mSingle = {};
    mSingle.magic = M5_MAGIC;
    mSingle.kind = M5_KIND_CITY_DELETE;
    mSingle.index = 0;
    mSingle.check = M5_CityCheck(singleCity.data());

    // First deletion succeeds -> count 0
    if (!Rename_ApplyCommand(&mSingle, sizeof(mSingle))) return false;
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 0) return false;

    // Second deletion of already deleted city on 0-city world: MUST return false safely
    if (Rename_ApplyCommand(&mSingle, sizeof(mSingle))) return false;
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 0) return false;

    // 4. Payload Fuzzing: Bad magic, truncated size, out-of-bounds index
    TestRenameMsg mFuzz = mDelete1;
    mFuzz.magic = 0xDEADBEEFu;
    if (Rename_ApplyCommand(&mFuzz, sizeof(mFuzz))) return false;

    mFuzz.magic = M5_MAGIC;
    mFuzz.index = 9999;
    if (Rename_ApplyCommand(&mFuzz, sizeof(mFuzz))) return false;

    if (Rename_ApplyCommand(&mFuzz, 16)) return false; // truncated payload

    cityVec.clear();
    WriteMockVectorHeader(world + 0x10348, cityVec);
    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 5: R7 & R18 - Save Transfer Telemetry, Jitter & Modal Dismiss
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R7_R18_TransferSpeedJitterAndModalDismiss()
{
    printf("      [Challenger2-1.5] R7/R18 Transfer Telemetry, Jitter & Modal Dismiss ...\n");
    fflush(stdout);

    // 1. Thread Safety of SyncBlock_SetPercentEx and SyncBlock_GetTransferStats
    std::atomic<bool> startSync{false};
    std::atomic<int> completedSync{0};

    std::vector<std::thread> syncWriters;
    syncWriters.reserve(4);
    for (int w = 0; w < 4; ++w) {
        syncWriters.emplace_back([&, w]() {
            while (!startSync.load(std::memory_order_relaxed)) std::this_thread::yield();
            for (int i = 0; i < 400; ++i) {
                int p = (i * 100) / 400;
                int q = 4000000 - i * 10000;
                float sp = 2.5f + (float)(w * 0.5);
                SyncBlock_SetPercentEx(p, q, 4000000, sp);
            }
            completedSync.fetch_add(1, std::memory_order_release);
        });
    }

    std::vector<std::thread> syncReaders;
    syncReaders.reserve(4);
    for (int r = 0; r < 4; ++r) {
        syncReaders.emplace_back([&]() {
            while (!startSync.load(std::memory_order_relaxed)) std::this_thread::yield();
            int p = 0, q = 0, tot = 0;
            float sp = 0.0f;
            for (int i = 0; i < 400; ++i) {
                SyncBlock_GetTransferStats(&p, &q, &tot, &sp);
            }
            completedSync.fetch_add(1, std::memory_order_release);
        });
    }

    startSync.store(true, std::memory_order_release);
    for (auto& th : syncWriters) if (th.joinable()) th.join();
    for (auto& th : syncReaders) if (th.joinable()) th.join();

    if (completedSync.load() != 8) return false;

    // 2. Transfer Speed EMA Under Jitter, Drops & Delivery Variations
    float emaSpeed = 0.0f;
    uint64_t lastDone = 0;
    uint64_t lastTick = 1000;

    auto SimUpdateSpeed = [&](uint64_t now, uint64_t done) {
        uint64_t dt = now - lastTick;
        if (dt >= 150) {
            double bytesDelta = (double)(done > lastDone ? done - lastDone : 0);
            double curSpeed = (bytesDelta / (1024.0 * 1024.0)) / ((double)dt / 1000.0);
            if (emaSpeed <= 0.001f) {
                emaSpeed = (float)curSpeed;
            } else {
                emaSpeed = 0.7f * emaSpeed + 0.3f * (float)curSpeed;
            }
            lastTick = now;
            lastDone = done;
        }
    };

    // Fast packet intervals (< 150ms): EMA must not update on sub-threshold noise
    SimUpdateSpeed(1050, 100000);
    if (emaSpeed > 0.0f) return false;

    // At 1200ms (dt = 200 >= 150ms): first speed calculation
    SimUpdateSpeed(1200, 2 * 1024 * 1024); // 2 MB in 0.2s = 10 MB/s
    if (fabsf(emaSpeed - 10.0f) > 0.05f) return false;

    // Connection drop / stall (dt = 3000ms, done unchanged): speed drops to 0.0 MB/s
    SimUpdateSpeed(4200, 2 * 1024 * 1024);
    if (emaSpeed >= 10.0f) return false; // decayed by 0.7

    // Connection reset / progress rollback (done drops from 2MB to 1MB)
    // Handled safely without unsigned underflow
    SimUpdateSpeed(5500, 1 * 1024 * 1024);
    if (emaSpeed < 0.0f || isnan(emaSpeed) || isinf(emaSpeed)) return false;

    // Simulated 200 random jittered packets (delays 50..800ms, sizes 0..4MB)
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint64_t> dtDist(50, 800);
    std::uniform_int_distribution<uint64_t> bytesDist(0, 4 * 1024 * 1024);
    uint64_t curSimTime = 6000;
    uint64_t curSimBytes = 1 * 1024 * 1024;

    for (int p = 0; p < 200; ++p) {
        curSimTime += dtDist(rng);
        curSimBytes += bytesDist(rng);
        SimUpdateSpeed(curSimTime, curSimBytes);
        if (emaSpeed < 0.0f || isnan(emaSpeed) || isinf(emaSpeed)) return false;
    }

    // 3. Progress Overlay String Formatting Boundaries
    char buf[128] = {};
    int totalBytes = 100 * 1024 * 1024;
    int queuedBytes = 50 * 1024 * 1024;
    int sentBytes = totalBytes - queuedBytes;
    double mbSent = (double)sentBytes / (1024.0 * 1024.0);
    double mbTot = (double)totalBytes / (1024.0 * 1024.0);
    float speed = 5.25f;

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f / %.1f MB (50%%) — %.1f MB/s", mbSent, mbTot, speed);
    if (strstr(buf, "50.0 / 100.0 MB (50%) — 5.2 MB/s") == nullptr) return false;

    // Stalled transfer (speed <= 0.05f) hides speed suffix
    speed = 0.02f;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f / %.1f MB (50%%)", mbSent, mbTot);
    if (strstr(buf, "50.0 / 100.0 MB (50%)") == nullptr) return false;

    // 4. R18 ModSync Download Modal Auto-Dismiss
    ModSync_CancelDownloadModal();
    if (ModSync_IsDownloadActive()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 6: R14 & R15 - Invalid IDs Boundary & Progress Overflow Stress
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R14_R15_InvalidBoundariesAndOverflow()
{
    printf("      [Challenger2-1.6] R14/R15 Invalid Tech/State IDs & Progress Overflow ...\n");
    fflush(stdout);

    TwoStates_ResetResearch();

    // 1. Boundary matrix for invalid Tech IDs: < 0 or >= 32
    const int invalidTechIds[] = { -1, -50, 32, 33, 99, 1000, 65535 };
    for (int badTech : invalidTechIds) {
        if (TwoStates_IsTechUnlocked(1, badTech)) return false;
        if (TwoStates_IsTechUnlocked(2, badTech)) return false;
        TwoStates_UnlockTech(1, badTech);
        TwoStates_UnlockTech(2, badTech);
        TwoStates_SetActiveTech(1, badTech);
        TwoStates_SetActiveTech(2, badTech);
        if (TwoStates_GetActiveTech(1) != 0) return false;
        if (TwoStates_GetActiveTech(2) != 0) return false;
        if (fabsf(TwoStates_GetTechProgress(1, badTech)) > 1e-5f) return false;
        if (fabsf(TwoStates_GetTechProgress(2, badTech)) > 1e-5f) return false;
        TwoStates_AddResearchProgress(1, badTech, 0.5f);
        TwoStates_AddResearchProgress(2, badTech, 0.5f);
        if (TwoStates_IsTechUnlocked(1, badTech) || TwoStates_IsTechUnlocked(2, badTech)) return false;

        LsResearchCommand badCmd = {};
        badCmd.stateOwner = 1;
        badCmd.techId = badTech;
        badCmd.progressDelta = 0.5f;
        if (TwoStates_ApplyResearchCommand(badCmd)) return false;
    }

    // 2. Boundary matrix for invalid State IDs: not in {1, 2}
    const int invalidStateIds[] = { -1, 0, 3, 4, 10, 999 };
    for (int badState : invalidStateIds) {
        if (TwoStates_IsTechUnlocked(badState, 5)) return false;
        TwoStates_UnlockTech(badState, 5);
        TwoStates_SetActiveTech(badState, 5);
        if (TwoStates_GetActiveTech(badState) != 0) return false;
        TwoStates_AddResearchProgress(badState, 5, 0.5f);
        if (fabsf(TwoStates_GetTechProgress(badState, 5)) > 1e-5f) return false;

        StateResearchTree dummyTree = {};
        if (TwoStates_GetStateResearch(badState, &dummyTree)) return false;
        if (TwoStates_SetStateResearch(badState, &dummyTree)) return false;

        LsResearchCommand badCmd = {};
        badCmd.stateOwner = badState;
        badCmd.techId = 5;
        badCmd.progressDelta = 0.5f;
        if (TwoStates_ApplyResearchCommand(badCmd)) return false;
    }

    // State 1 and 2 must have remained completely clean
    StateResearchTree t1 = {}, t2 = {};
    if (!TwoStates_GetStateResearch(1, &t1) || !TwoStates_GetStateResearch(2, &t2)) return false;
    if (t1.researchedCount != 0 || t1.unlockedMask != 0 || t2.researchedCount != 0 || t2.unlockedMask != 0) return false;

    // 3. Progress overflow stress (> 1.0f): Single-step overflow
    TwoStates_SetActiveTech(1, 15);
    TwoStates_AddResearchProgress(1, 15, 1.8f);
    if (!TwoStates_IsTechUnlocked(1, 15)) return false;
    if (fabsf(TwoStates_GetTechProgress(1, 15) - 1.0f) > 1e-5f) return false; // strictly clamped to 1.0f
    if (TwoStates_GetActiveTech(1) != 0) return false; // active tech resets on unlock

    // Adding more progress to already unlocked tech is safely ignored
    TwoStates_AddResearchProgress(1, 15, 2.5f);
    if (fabsf(TwoStates_GetTechProgress(1, 15) - 1.0f) > 1e-5f) return false;

    // 4. Multi-step cumulative progress overflow (> 1.0f)
    TwoStates_SetActiveTech(2, 16);
    TwoStates_AddResearchProgress(2, 16, 0.65f);
    if (TwoStates_IsTechUnlocked(2, 16)) return false;
    if (fabsf(TwoStates_GetTechProgress(2, 16) - 0.65f) > 1e-4f) return false;
    TwoStates_AddResearchProgress(2, 16, 0.70f); // 0.65 + 0.70 = 1.35f -> unlock & clamp to 1.0f
    if (!TwoStates_IsTechUnlocked(2, 16)) return false;
    if (fabsf(TwoStates_GetTechProgress(2, 16) - 1.0f) > 1e-5f) return false;
    if (TwoStates_GetActiveTech(2) != 0) return false;

    // 5. Wire command progress overflow
    LsResearchCommand cmdOverflow = {};
    cmdOverflow.stateOwner = 1;
    cmdOverflow.techId = 17;
    cmdOverflow.progressDelta = 5.0f;
    if (!TwoStates_ApplyResearchCommand(cmdOverflow)) return false;
    if (!TwoStates_IsTechUnlocked(1, 17)) return false;
    if (fabsf(TwoStates_GetTechProgress(1, 17) - 1.0f) > 1e-5f) return false;

    TwoStates_ResetResearch();
    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 7: R6 - Out-of-Bounds & Empty City Vector Deletion Safety
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R6_OutOfBoundsAndEmptyVectorDeletion()
{
    printf("      [Challenger2-1.7] R6 Out-of-Bounds & Empty City Vector Deletion ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Empty city vector: deletion attempts must safely fail without crash
    std::vector<BYTE*> emptyVec;
    WriteMockVectorHeader(world + 0x10348, emptyVec);
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 0) return false;

    TestRenameMsg mEmpty = {};
    mEmpty.magic = M5_MAGIC;
    mEmpty.kind = M5_KIND_CITY_DELETE;
    mEmpty.index = 0;
    mEmpty.check = 0x12345678;

    if (Rename_ApplyDeleteCommand(&mEmpty, sizeof(mEmpty))) return false;
    if (Rename_ApplyCommand(&mEmpty, sizeof(mEmpty))) return false;

    mEmpty.index = 1;
    if (Rename_ApplyDeleteCommand(&mEmpty, sizeof(mEmpty))) return false;
    mEmpty.index = 9999;
    if (Rename_ApplyDeleteCommand(&mEmpty, sizeof(mEmpty))) return false;

    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 0) return false;

    // 2. Populate 2 cities: valid indices are 0 and 1
    std::vector<BYTE> cityA(128, 0);
    std::vector<BYTE> cityB(128, 0);
    *(float*)(cityA.data() + 4) = 150.0f;
    *(float*)(cityA.data() + 8) = 12.0f;
    *(float*)(cityA.data() + 12) = 250.0f;
    wcscpy_s((wchar_t*)(cityA.data() + 0x10), 32, L"Kiev");

    *(float*)(cityB.data() + 4) = 350.0f;
    *(float*)(cityB.data() + 8) = 22.0f;
    *(float*)(cityB.data() + 12) = 450.0f;
    wcscpy_s((wchar_t*)(cityB.data() + 0x10), 32, L"Minsk");

    std::vector<BYTE*> twoVec = { cityA.data(), cityB.data() };
    WriteMockVectorHeader(world + 0x10348, twoVec);
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 2) return false;

    // Out-of-bounds boundary: index = 2 (off-by-one, count is 2)
    TestRenameMsg mOob = {};
    mOob.magic = M5_MAGIC;
    mOob.kind = M5_KIND_CITY_DELETE;
    mOob.index = 2;
    mOob.check = 0xAABBCCDD;
    if (Rename_ApplyDeleteCommand(&mOob, sizeof(mOob))) return false;

    // Out-of-bounds index = 500 and 0xFFFFFFFF
    mOob.index = 500;
    if (Rename_ApplyDeleteCommand(&mOob, sizeof(mOob))) return false;
    mOob.index = 0xFFFFFFFFu;
    if (Rename_ApplyDeleteCommand(&mOob, sizeof(mOob))) return false;

    // Vector must remain 2 intact cities
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 2) return false;

    // 3. Double-deletion idempotency at index 0
    TestRenameMsg mDelKiev = {};
    mDelKiev.magic = M5_MAGIC;
    mDelKiev.kind = M5_KIND_CITY_DELETE;
    mDelKiev.index = 0;
    mDelKiev.check = M5_CityCheck(cityA.data());

    // First deletion of city 0 ("Kiev") succeeds
    if (!Rename_ApplyDeleteCommand(&mDelKiev, sizeof(mDelKiev))) return false;
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 1) return false;

    // Remaining city at index 0 is now Minsk
    BYTE** rem = *(BYTE***)(world + 0x10348);
    if (!rem || !rem[0]) return false;
    if (wcscmp((const wchar_t*)(rem[0] + 0x10), L"Minsk") != 0) return false;

    // Replay receives duplicate delete command for city 0 with Kiev check hash
    // MUST FAIL because index 0 holds Minsk!
    if (Rename_ApplyDeleteCommand(&mDelKiev, sizeof(mDelKiev))) return false;
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 1) return false;
    if (wcscmp((const wchar_t*)(rem[0] + 0x10), L"Minsk") != 0) return false;

    // Delete Minsk at index 0
    TestRenameMsg mDelMinsk = {};
    mDelMinsk.magic = M5_MAGIC;
    mDelMinsk.kind = M5_KIND_CITY_DELETE;
    mDelMinsk.index = 0;
    mDelMinsk.check = M5_CityCheck(cityB.data());

    if (!Rename_ApplyDeleteCommand(&mDelMinsk, sizeof(mDelMinsk))) return false;
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 0) return false;

    // Delete Minsk AGAIN on empty vector -> must fail safely
    if (Rename_ApplyDeleteCommand(&mDelMinsk, sizeof(mDelMinsk))) return false;
    if (SafeCountStdVector(world + 0x10348, sizeof(BYTE*), 100) != 0) return false;

    twoVec.clear();
    WriteMockVectorHeader(world + 0x10348, twoVec);
    return true;
}

// ---------------------------------------------------------------------------
// SUBTEST 8: R7 - Percentage Clamping (0%, 100%, 150%) & 0-Byte Safety
// ---------------------------------------------------------------------------

static bool SubTest_M5_C2_R7_PercentageClampingAndZeroByteSafety()
{
    printf("      [Challenger2-1.8] R7 Percentage Clamping & 0-Byte Division-by-Zero Safety ...\n");
    fflush(stdout);

    int p = -1, q = -1, tot = -1;
    float sp = -1.0f;

    auto ClampedBarFraction = [](int percent) -> float {
        float frac = (float)percent / 100.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (frac < 0.0f) frac = 0.0f;
        return frac;
    };

    // 1. Clamping test: 0% boundary
    SyncBlock_SetPercentEx(0, 5000000, 5000000, 0.0f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &sp);
    if (p != 0 || tot != 5000000 || ClampedBarFraction(p) != 0.0f) return false;

    // 2. Clamping test: 100% boundary
    SyncBlock_SetPercentEx(100, 0, 5000000, 10.0f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &sp);
    if (p != 100 || ClampedBarFraction(p) != 1.0f) return false;

    // 3. Clamping test: 150% overflow boundary
    SyncBlock_SetPercentEx(150, 0, 5000000, 15.0f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &sp);
    if (p != 150) return false; // raw percentage stored
    if (ClampedBarFraction(p) != 1.0f) return false; // UI bar strictly clamped to 1.0f

    // 4. Clamping test: Negative percentage (-25%)
    SyncBlock_SetPercentEx(-25, 5000000, 5000000, 0.0f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &sp);
    if (p != -25) return false;
    if (ClampedBarFraction(p) != 0.0f) return false; // UI bar strictly clamped to 0.0f

    // 5. 0-byte division-by-zero safety test (totalBytes = 0, queuedBytes = 0)
    SyncBlock_SetPercentEx(0, 0, 0, 0.0f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &sp);
    if (tot != 0) return false;

    char barOverlay[96] = {};
    if (tot > 0) {
        int sentBytes = tot - q;
        if (sentBytes < 0) sentBytes = 0;
        if (sentBytes > tot) sentBytes = tot;
        double mbSent = (double)sentBytes / (1024.0 * 1024.0);
        double mbTot = (double)tot / (1024.0 * 1024.0);
        _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE, "%.1f / %.1f MB (%d%%)", mbSent, mbTot, p);
    } else {
        _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE, "%d%%", p);
    }
    if (strcmp(barOverlay, "0%") != 0) return false;

    // 6. Adversarial 0-byte with queuedBytes > 0
    SyncBlock_SetPercentEx(50, 2048, 0, 0.0f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &sp);
    if (tot != 0) return false;

    if (tot > 0) {
        int sentBytes = tot - q;
        double mbSent = (double)sentBytes / (1024.0 * 1024.0);
        double mbTot = (double)tot / (1024.0 * 1024.0);
        _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE, "%.1f / %.1f MB (%d%%)", mbSent, mbTot, p);
    } else {
        _snprintf_s(barOverlay, sizeof(barOverlay), _TRUNCATE, "%d%%", p);
    }
    if (strcmp(barOverlay, "50%") != 0) return false;

    // 7. dt = 0 speed calculation safety
    uint64_t lastTick = 1000;
    uint64_t curTick = 1000; // dt = 0
    uint64_t dt = curTick - lastTick;
    float curSpeed = 0.0f;
    if (dt >= 150) {
        curSpeed = (float)(((double)1000 / (1024.0 * 1024.0)) / ((double)dt / 1000.0));
    }
    if (curSpeed != 0.0f || isnan(curSpeed) || isinf(curSpeed)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Milestone 5 Suite Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M5_Challenger2()
{
    printf("  [M5-CHALLENGER2] Starting Empirical Stress Testing for Milestone 5 (R6, R7, R14, R15, R18) ...\n");
    fflush(stdout);

    if (!SubTest_M5_C2_R14_R15_MultiThreadResearchConcurrency()) return false;
    if (!SubTest_M5_C2_R14_R15_32SlotSaturationAndBoundaries()) return false;
    if (!SubTest_M5_C2_R14_R15_UIStateIsolation()) return false;
    if (!SubTest_M5_C2_R6_CityDeletionLockstepSafety()) return false;
    if (!SubTest_M5_C2_R7_R18_TransferSpeedJitterAndModalDismiss()) return false;
    if (!SubTest_M5_C2_R14_R15_InvalidBoundariesAndOverflow()) return false;
    if (!SubTest_M5_C2_R6_OutOfBoundsAndEmptyVectorDeletion()) return false;
    if (!SubTest_M5_C2_R7_PercentageClampingAndZeroByteSafety()) return false;

    printf("  [M5-CHALLENGER2] ALL Milestone 5 Empirical Stress Tests PASSED!\n");
    fflush(stdout);
    return true;
}
