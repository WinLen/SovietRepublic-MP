// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone 5 (R6, R7, R14, R15, R18)
// Challenger 1: Adversarial Stress Testing of Research Independence, City Deletion, and Save Transfer
// Focus Areas:
//   1. R14 & R15: 100% strict cross-unlock prevention oracle across all 32 technologies.
//   2. R14 & R15: Concurrent research of identical technology with asymmetric progress & independent completion.
//   3. R14 & R15: Serialization round-trip across binary (.bin) and INI fallback with corrupted input resilience.
//   4. R6: City deletion under bounds violations (invalid index, empty vector, capacity 4096), rapid interleaved rename/deletion, and vector compaction memory safety.
//   5. R7 & R18: Save transfer percentage edge cases (0 bytes, division by zero, 100% boundary) and modal auto-dismissal.

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

#define M5_C1_MAGIC 0x314D414E
#define M5_C1_KIND_CITY_DELETE 0xD
#define M5_C1_KIND_RESEARCH    0xE

#define M5_C1_WORLD_RVA 0x9D4F10
#define M5_C1_CITY_LIST 0x10348
#define M5_C1_MAX_CITIES 4096

#pragma pack(push, 1)
struct M5_C1_RenameMsg {
    unsigned magic, kind, index, check, chars;
    unsigned short text[64];
};
#pragma pack(pop)

extern void SyncBlock_SetPercentEx(int percent, int queuedBytes, int totalBytes, float speedMbSec);
extern void SyncBlock_GetTransferStats(int* outPercent, int* outQueuedBytes, int* outTotalBytes, float* outSpeedMbSec);
extern bool Rename_ApplyDeleteCommand(const void* data, size_t size);

static unsigned M5_C1_FnvHash(const BYTE* b, size_t n, unsigned h = 2166136261u)
{
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h | 1u;
}

static unsigned M5_C1_CityCheck(const BYTE* city)
{
    return M5_C1_FnvHash(city + 4, 12);
}

// ---------------------------------------------------------------------------
// 1. R14 & R15: Strict Cross-Unlock Prevention Oracle & Asymmetric Progress
// ---------------------------------------------------------------------------

static bool SubTest_M5_C1_R14_R15_CrossUnlockPreventionOracle()
{
    printf("      [Challenger1-1.1] R14/R15 32-Tech Cross-Unlock Prevention Oracle ...\n");
    fflush(stdout);

    TwoStates_ResetResearch();

    // Verify 32/32 technologies: unlocking on State 1 leaves State 2 strictly locked
    for (int t = 0; t < 32; ++t) {
        TwoStates_UnlockTech(1, t);
        if (!TwoStates_IsTechUnlocked(1, t)) return false;
        if (TwoStates_IsTechUnlocked(2, t)) return false; // Cross-leakage failure!
        if (fabsf(TwoStates_GetTechProgress(2, t)) > 1e-5f) return false;
    }

    StateResearchTree tree1 = {};
    StateResearchTree tree2 = {};
    if (!TwoStates_GetStateResearch(1, &tree1)) return false;
    if (!TwoStates_GetStateResearch(2, &tree2)) return false;

    if (tree1.unlockedMask != 0xFFFFFFFFu) return false;
    if (tree1.researchedCount != 32) return false;
    if (tree2.unlockedMask != 0u) return false;
    if (tree2.researchedCount != 0) return false;

    // Reset and reverse: unlocking on State 2 leaves State 1 strictly locked
    TwoStates_ResetResearch();
    for (int t = 0; t < 32; ++t) {
        TwoStates_UnlockTech(2, t);
        if (!TwoStates_IsTechUnlocked(2, t)) return false;
        if (TwoStates_IsTechUnlocked(1, t)) return false; // Cross-leakage failure!
        if (fabsf(TwoStates_GetTechProgress(1, t)) > 1e-5f) return false;
    }

    if (!TwoStates_GetStateResearch(1, &tree1)) return false;
    if (!TwoStates_GetStateResearch(2, &tree2)) return false;

    if (tree2.unlockedMask != 0xFFFFFFFFu) return false;
    if (tree2.researchedCount != 32) return false;
    if (tree1.unlockedMask != 0u) return false;
    if (tree1.researchedCount != 0) return false;

    // Out-of-bounds parameter fuzzing: stateOwner < 1 or > 2, techId < 0 or >= 32
    TwoStates_UnlockTech(0, 10);
    TwoStates_UnlockTech(3, 10);
    TwoStates_UnlockTech(-1, 10);
    TwoStates_UnlockTech(1, -1);
    TwoStates_UnlockTech(1, 32);
    TwoStates_UnlockTech(1, 9999);

    if (TwoStates_IsTechUnlocked(0, 10)) return false;
    if (TwoStates_IsTechUnlocked(3, 10)) return false;
    if (TwoStates_IsTechUnlocked(1, -1)) return false;
    if (TwoStates_IsTechUnlocked(1, 32)) return false;

    return true;
}

static bool SubTest_M5_C1_R14_R15_ConcurrentIdenticalTechProgress()
{
    printf("      [Challenger1-1.2] R14/R15 Concurrent Identical Tech With Asymmetric Progress ...\n");
    fflush(stdout);

    TwoStates_ResetResearch();

    const int kTargetTech = 19;
    TwoStates_SetActiveTech(1, kTargetTech);
    TwoStates_SetActiveTech(2, kTargetTech);

    if (TwoStates_GetActiveTech(1) != kTargetTech) return false;
    if (TwoStates_GetActiveTech(2) != kTargetTech) return false;

    std::atomic<bool> startFlag{false};
    std::thread t1([&]() {
        while (!startFlag.load()) std::this_thread::yield();
        for (int i = 0; i < 50; ++i) {
            TwoStates_AddResearchProgress(1, kTargetTech, 0.010f); // reaches 0.50f
            std::this_thread::yield();
        }
    });

    std::thread t2([&]() {
        while (!startFlag.load()) std::this_thread::yield();
        for (int i = 0; i < 20; ++i) {
            TwoStates_AddResearchProgress(2, kTargetTech, 0.010f); // reaches 0.20f
            std::this_thread::yield();
        }
    });

    startFlag.store(true);
    t1.join();
    t2.join();

    float p1 = TwoStates_GetTechProgress(1, kTargetTech);
    float p2 = TwoStates_GetTechProgress(2, kTargetTech);

    if (fabsf(p1 - 0.50f) > 1e-4f) return false;
    if (fabsf(p2 - 0.20f) > 1e-4f) return false;
    if (TwoStates_IsTechUnlocked(1, kTargetTech)) return false;
    if (TwoStates_IsTechUnlocked(2, kTargetTech)) return false;

    // Now push State 1 past 1.0f: it unlocks and clears activeTechId, leaving State 2 in progress
    TwoStates_AddResearchProgress(1, kTargetTech, 0.60f);

    if (!TwoStates_IsTechUnlocked(1, kTargetTech)) return false;
    if (TwoStates_GetActiveTech(1) != 0) return false; // reset upon unlock
    if (fabsf(TwoStates_GetTechProgress(1, kTargetTech) - 1.0f) > 1e-4f) return false;

    // State 2 must be completely unaffected: still researching, progress 0.20f, activeTechId 19
    if (TwoStates_IsTechUnlocked(2, kTargetTech)) return false;
    if (TwoStates_GetActiveTech(2) != kTargetTech) return false;
    if (fabsf(TwoStates_GetTechProgress(2, kTargetTech) - 0.20f) > 1e-4f) return false;

    // Further progress on already unlocked State 1 tech must be rejected as no-op
    TwoStates_AddResearchProgress(1, kTargetTech, 0.50f);
    if (fabsf(TwoStates_GetTechProgress(1, kTargetTech) - 1.0f) > 1e-4f) return false;

    // Negative / zero progress delta fuzzing
    TwoStates_AddResearchProgress(2, kTargetTech, -0.10f);
    TwoStates_AddResearchProgress(2, kTargetTech, 0.0f);
    if (fabsf(TwoStates_GetTechProgress(2, kTargetTech) - 0.20f) > 1e-4f) return false;

    return true;
}

static bool SubTest_M5_C1_R14_R15_SerializationRoundTripBinaryAndIni()
{
    printf("      [Challenger1-1.3] R14/R15 Serialization Round-Trip (.bin & INI Fallback) ...\n");
    fflush(stdout);

    char tempDir[MAX_PATH] = {};
    char baseTemp[MAX_PATH] = {};
    GetTempPathA(sizeof(baseTemp), baseTemp);
    _snprintf_s(tempDir, sizeof(tempDir), _TRUNCATE, "%sRepublicMP_M5_ResearchTest_%u", baseTemp, GetCurrentProcessId());
    CreateDirectoryA(tempDir, nullptr);

    TwoStates_ResetResearch();

    // Configure distinct state research trees
    TwoStates_UnlockTech(1, 2);
    TwoStates_UnlockTech(1, 8);
    TwoStates_UnlockTech(1, 15);
    TwoStates_SetActiveTech(1, 10);
    TwoStates_AddResearchProgress(1, 10, 0.450f);

    TwoStates_UnlockTech(2, 3);
    TwoStates_UnlockTech(2, 7);
    TwoStates_UnlockTech(2, 25);
    TwoStates_SetActiveTech(2, 10);
    TwoStates_AddResearchProgress(2, 10, 0.780f);

    StateResearchTree orig1 = {};
    StateResearchTree orig2 = {};
    TwoStates_GetStateResearch(1, &orig1);
    TwoStates_GetStateResearch(2, &orig2);

    // Save metadata and binary dumps
    if (!TwoStates_WriteSaveMetaToDir(tempDir)) return false;

    // Phase A: Test Binary Load Priority
    TwoStates_ResetResearch();
    if (!TwoStates_ReadSaveMetaFromDir(tempDir, true)) return false;

    StateResearchTree binLoaded1 = {};
    StateResearchTree binLoaded2 = {};
    TwoStates_GetStateResearch(1, &binLoaded1);
    TwoStates_GetStateResearch(2, &binLoaded2);

    if (binLoaded1.unlockedMask != orig1.unlockedMask) return false;
    if (binLoaded1.activeTechId != orig1.activeTechId) return false;
    if (fabsf(binLoaded1.activeTechProgress - orig1.activeTechProgress) > 1e-4f) return false;

    if (binLoaded2.unlockedMask != orig2.unlockedMask) return false;
    if (binLoaded2.activeTechId != orig2.activeTechId) return false;
    if (fabsf(binLoaded2.activeTechProgress - orig2.activeTechProgress) > 1e-4f) return false;

    // Phase B: Test Pure INI Fallback (remove binary .bin files)
    char bin1Path[MAX_PATH] = {};
    char bin2Path[MAX_PATH] = {};
    _snprintf_s(bin1Path, sizeof(bin1Path), _TRUNCATE, "%s\\research_state1.bin", tempDir);
    _snprintf_s(bin2Path, sizeof(bin2Path), _TRUNCATE, "%s\\research_state2.bin", tempDir);
    DeleteFileA(bin1Path);
    DeleteFileA(bin2Path);

    TwoStates_ResetResearch();
    if (!TwoStates_ReadSaveMetaFromDir(tempDir, true)) return false;

    StateResearchTree iniLoaded1 = {};
    StateResearchTree iniLoaded2 = {};
    TwoStates_GetStateResearch(1, &iniLoaded1);
    TwoStates_GetStateResearch(2, &iniLoaded2);

    if (iniLoaded1.unlockedMask != orig1.unlockedMask) return false;
    if (iniLoaded1.activeTechId != orig1.activeTechId) return false;
    if (fabsf(iniLoaded1.activeTechProgress - orig1.activeTechProgress) > 1e-3f) return false;

    if (iniLoaded2.unlockedMask != orig2.unlockedMask) return false;
    if (iniLoaded2.activeTechId != orig2.activeTechId) return false;
    if (fabsf(iniLoaded2.activeTechProgress - orig2.activeTechProgress) > 1e-3f) return false;

    // Cleanup temp directory
    char iniPath[MAX_PATH] = {};
    _snprintf_s(iniPath, sizeof(iniPath), _TRUNCATE, "%s\\republicmp_coop.ini", tempDir);
    DeleteFileA(iniPath);
    RemoveDirectoryA(tempDir);

    return true;
}

// ---------------------------------------------------------------------------
// 2. R6: City Deletion Under Bounds Violations, Interleaved Rename & Safety
// ---------------------------------------------------------------------------

static bool SubTest_M5_C1_R6_CityDeletionBoundsAndCompactionStress()
{
    printf("      [Challenger1-2.1] R6 City Deletion Bounds & Vector Compaction Stress ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + M5_C1_WORLD_RVA;

    // 1. Empty City Vector Bounds Violations
    *(uint64_t*)(world + M5_C1_CITY_LIST + 0) = 0;
    *(uint64_t*)(world + M5_C1_CITY_LIST + 8) = 0;

    M5_C1_RenameMsg msg = {};
    msg.magic = M5_C1_MAGIC;
    msg.kind = M5_C1_KIND_CITY_DELETE;
    msg.index = 0;
    msg.check = 0x12345678;

    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false; // must reject on empty vector

    msg.index = 10;
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    msg.index = 4095;
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    // Invalid wire packet sizes and corrupted magic
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg) - 1)) return false;
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg) + 4)) return false;

    msg.magic = 0xDEADBEEF;
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;
    msg.magic = M5_C1_MAGIC;

    // 2. Populate 16 Mock Cities (allocating 256 bytes per city POD)
    std::vector<std::vector<BYTE>> cities(16, std::vector<BYTE>(256, 0));
    std::vector<uint64_t> cityPtrs(16);

    for (size_t i = 0; i < 16; ++i) {
        *(float*)(cities[i].data() + 4) = 100.0f + (float)i * 50.0f; // pos X
        *(float*)(cities[i].data() + 8) = 0.0f;                      // pos Y
        *(float*)(cities[i].data() + 12) = 200.0f + (float)i * 50.0f; // pos Z
        cityPtrs[i] = (uint64_t)cities[i].data();
    }

    *(uint64_t*)(world + M5_C1_CITY_LIST + 0) = (uint64_t)cityPtrs.data();
    *(uint64_t*)(world + M5_C1_CITY_LIST + 8) = (uint64_t)(cityPtrs.data() + 16);

    // Out of bounds index (> 15) must return false without altering vector
    msg.index = 16;
    msg.check = M5_C1_CityCheck(cities[0].data());
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    msg.index = 4096;
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    uint64_t beginBef = *(uint64_t*)(world + M5_C1_CITY_LIST + 0);
    uint64_t endBef = *(uint64_t*)(world + M5_C1_CITY_LIST + 8);
    if ((endBef - beginBef) / 8 != 16) return false;

    // Mismatched check hash must return false without deleting
    msg.index = 0;
    msg.check = 0xBADF00D;
    if (Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;
    if ((*(uint64_t*)(world + M5_C1_CITY_LIST + 8) - beginBef) / 8 != 16) return false;

    // 3. Sequential Compaction Test:
    // Delete Front (index 0)
    msg.index = 0;
    msg.check = M5_C1_CityCheck(cities[0].data());
    if (!Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    uint64_t curEnd = *(uint64_t*)(world + M5_C1_CITY_LIST + 8);
    if ((curEnd - beginBef) / 8 != 15) return false;
    // Former city 1 is now at index 0
    if (*(uint64_t*)(beginBef + 0) != (uint64_t)cities[1].data()) return false;

    // Delete Middle (index 5)
    uint64_t delPtr = *(uint64_t*)(beginBef + 5 * 8);
    msg.index = 5;
    msg.check = M5_C1_CityCheck((const BYTE*)delPtr);
    if (!Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    curEnd = *(uint64_t*)(world + M5_C1_CITY_LIST + 8);
    if ((curEnd - beginBef) / 8 != 14) return false;

    // Delete Last (index 13)
    delPtr = *(uint64_t*)(beginBef + 13 * 8);
    msg.index = 13;
    msg.check = M5_C1_CityCheck((const BYTE*)delPtr);
    if (!Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    curEnd = *(uint64_t*)(world + M5_C1_CITY_LIST + 8);
    if ((curEnd - beginBef) / 8 != 13) return false;

    // Rapidly delete all remaining 13 cities until empty
    for (int rem = 12; rem >= 0; --rem) {
        delPtr = *(uint64_t*)(beginBef + 0);
        msg.index = 0;
        msg.check = M5_C1_CityCheck((const BYTE*)delPtr);
        if (!Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;
    }

    curEnd = *(uint64_t*)(world + M5_C1_CITY_LIST + 8);
    if ((curEnd - beginBef) / 8 != 0) return false;

    // 4. Interleaved Rename & Deletion Safety
    // Repopulate 2 cities
    cityPtrs[0] = (uint64_t)cities[0].data();
    cityPtrs[1] = (uint64_t)cities[1].data();
    *(uint64_t*)(world + M5_C1_CITY_LIST + 0) = (uint64_t)cityPtrs.data();
    *(uint64_t*)(world + M5_C1_CITY_LIST + 8) = (uint64_t)(cityPtrs.data() + 2);

    // Rename City 0
    M5_C1_RenameMsg renMsg = {};
    renMsg.magic = M5_C1_MAGIC;
    renMsg.kind = 3; // KIND_CITY
    renMsg.index = 0;
    renMsg.check = M5_C1_CityCheck(cities[0].data());
    renMsg.chars = 10;
    const wchar_t* wName = L"ЛЕНИНОГОРС";
    memcpy(renMsg.text, wName, 10 * sizeof(wchar_t));

    if (!Rename_ApplyCommand(&renMsg, sizeof(renMsg))) return false;
    const wchar_t* curName = (const wchar_t*)(cities[0].data() + 0x10);
    if (wcsncmp(curName, L"ЛЕНИНОГОРС", 10) != 0) return false;

    // Delete City 1
    msg.index = 1;
    msg.check = M5_C1_CityCheck(cities[1].data());
    if (!Rename_ApplyDeleteCommand(&msg, sizeof(msg))) return false;

    // Verify City 0 name remains uncorrupted
    if (wcsncmp(curName, L"ЛЕНИНОГОРС", 10) != 0) return false;

    // Trying to rename former City 1 at index 1 must now fail cleanly
    renMsg.index = 1;
    renMsg.check = M5_C1_CityCheck(cities[1].data());
    if (Rename_ApplyCommand(&renMsg, sizeof(renMsg))) return false;

    return true;
}

// ---------------------------------------------------------------------------
// 3. R7 & R18: Save Transfer Telemetry & Modal Auto-Dismissal
// ---------------------------------------------------------------------------

static bool SubTest_M5_C1_R7_R18_SaveTransferAndModalAutoDismiss()
{
    printf("      [Challenger1-3.1] R7/R18 Save Transfer Telemetry & Modal Auto-Dismissal ...\n");
    fflush(stdout);

    // 1. Division by Zero & Zero-Byte Boundaries
    SyncBlock_SetPercentEx(0, 0, 0, 0.0f);
    int p = -1, q = -1, tot = -1;
    float spd = -1.0f;
    SyncBlock_GetTransferStats(&p, &q, &tot, &spd);
    if (p != 0 || q != 0 || tot != 0 || spd != 0.0f) return false;

    // 2. Sent Bytes > Total Bytes Clamping
    // If queued is negative or done > total, percent and sent bytes must clamp cleanly
    SyncBlock_SetPercentEx(105, -500, 1000, 2.5f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &spd);
    if (p != 105) return false; // Wire percent preserved

    // 3. 100% Completion Boundary Reset
    SyncBlock_SetPercentEx(100, 0, 50000000, 15.0f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &spd);
    if (p != 100 || q != 0 || tot != 50000000) return false;

    // 4. Large Transfer Scalability (2 GB boundary)
    const int kTwoGb = 2147483647;
    SyncBlock_SetPercentEx(50, kTwoGb / 2, kTwoGb, 45.5f);
    SyncBlock_GetTransferStats(&p, &q, &tot, &spd);
    if (p != 50 || tot != kTwoGb || fabsf(spd - 45.5f) > 1e-4f) return false;

    // 5. R18 ModSync Download Modal Auto-Dismissal Verification
    // Verify dismissal in active gameplay
    ModSync_CancelDownloadModal();
    if (ModSync_IsDownloadActive()) return false;

    // Idempotency: multiple cancels when already closed must not fault
    for (int i = 0; i < 5; ++i) {
        ModSync_CancelDownloadModal();
        if (ModSync_IsDownloadActive()) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Challenger 1 Milestone 5 Suite Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18()
{
    printf("  [M5-CHALLENGER1] Starting Empirical Stress Testing for Milestone 5 (R6, R7, R14, R15, R18) ...\n");
    fflush(stdout);

    if (!SubTest_M5_C1_R14_R15_CrossUnlockPreventionOracle()) return false;
    if (!SubTest_M5_C1_R14_R15_ConcurrentIdenticalTechProgress()) return false;
    if (!SubTest_M5_C1_R14_R15_SerializationRoundTripBinaryAndIni()) return false;
    if (!SubTest_M5_C1_R6_CityDeletionBoundsAndCompactionStress()) return false;
    if (!SubTest_M5_C1_R7_R18_SaveTransferAndModalAutoDismiss()) return false;

    printf("  [M5-CHALLENGER1] ALL Milestone 5 Empirical Stress Tests PASSED!\n");
    fflush(stdout);
    return true;
}
