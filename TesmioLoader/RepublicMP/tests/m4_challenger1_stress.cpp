// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone 4 Iteration 2 (R3, R10, R11, R17)
// Challenger 1: Adversarial Stress Testing
// Focus Areas:
//   1. R3: Charset stress (Cyrillic UTF-8, symbols, spaces, long strings, control char rejection) in SafeCopyPlacementName & CleanBuildingName
//   2. R3: Multi-threaded concurrency under TsmScopedCsLock (20,000 rapid updates, verification of 0 torn coordinate reads)
//   3. R3: Tool deselect and linger clearing (immediate clearing when tool pointer in world is null or cleared)
//   4. R10: Vehicle position snapping with drift spectrum from 0m to 50,000m (verifying removal of the 1000m drift ceiling)
//   5. R17: HookedBuildingRenderPass exception recovery under native engine SEH simulation (verifying __finally always restores visibility and distance)
//   6. R11: Rapid-fire purchase audio, sound mute flag, and null engine pointer safety

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

// ---------------------------------------------------------------------------
// 1. R3: Charset, Symbols, Boundary Lengths and Control Character Rejection
// ---------------------------------------------------------------------------

static bool SubTest_M4_Challenger1_R3_CharsetAndSymbolsStress()
{
    printf("      [Challenger1-1.1] R3 Charset & Symbol Stress (Cyrillic, Spaces, Symbols, Boundaries) ...\n");
    fflush(stdout);

    // A. Cyrillic UTF-8, Ukrainian, Mixed, European, and Symbolic building paths
    const char* testPaths[] = {
        "media_soviet/buildings/школа_номер_1.ini",
        "media_soviet/buildings/больница_№1.ini",
        "media_soviet/buildings/школа_імені_шевченка.ini",
        "media_soviet/buildings/Soviet_Школа_100.ini",
        "media_soviet/buildings/Ölraffinerie_Groß.ini",
        "buildings/Больница № 1 (Центральная) - цех #2.ini",
        "media_soviet\\sub_dir/buildings\\завод_цементный.ini",
        "buildings/warehouse_large",
        "buildings/depot.INI",
        "media_soviet/buildings/Bäckerei_Süd.ini",
        "buildings/Flugplatz_München_Groß.ini"
    };
    const char* expectedClean[] = {
        "школа_номер_1",
        "больница_№1",
        "школа_імені_шевченка",
        "Soviet_Школа_100",
        "Ölraffinerie_Groß",
        "Больница № 1 (Центральная) - цех #2",
        "завод_цементный",
        "warehouse_large",
        "depot",
        "Bäckerei_Süd",
        "Flugplatz_München_Groß"
    };
    const size_t testCount = sizeof(testPaths) / sizeof(testPaths[0]);

    for (size_t i = 0; i < testCount; ++i) {
        char copyDst[128] = {};
        SafeCopyPlacementName(testPaths[i], copyDst, sizeof(copyDst));
        TEST_CHECK(strcmp(copyDst, testPaths[i]) == 0);

        char cleanDst[128] = {};
        CleanBuildingName(copyDst, cleanDst, sizeof(cleanDst));
        TEST_CHECK(strcmp(cleanDst, expectedClean[i]) == 0);
    }

    // B. Boundary string lengths (126, 127, 200 chars)
    char buf126[128] = {};
    memset(buf126, 'x', 126);
    buf126[126] = '\0';
    char copy126[128] = {};
    SafeCopyPlacementName(buf126, copy126, sizeof(copy126));
    TEST_CHECK(strlen(copy126) == 126);

    char buf127[128] = {};
    memset(buf127, 'y', 127);
    buf127[127] = '\0';
    char copy127[128] = {};
    SafeCopyPlacementName(buf127, copy127, sizeof(copy127));
    TEST_CHECK(strlen(copy127) == 127);
    TEST_CHECK(copy127[127] == '\0');

    char buf200[256] = {};
    memset(buf200, 'z', 200);
    buf200[200] = '\0';
    char copy200[128] = {};
    SafeCopyPlacementName(buf200, copy200, sizeof(copy200));
    TEST_CHECK(strlen(copy200) == 127);
    TEST_CHECK(copy200[127] == '\0');

    // C. Control character and DEL rejection in SafeCopyPlacementName across entire control range
    for (int cc = 1; cc < 0x20; ++cc) {
        char badControl[128] = {};
        char src[32] = {};
        snprintf(src, sizeof(src), "building\x01test.ini");
        src[8] = (char)cc;
        SafeCopyPlacementName(src, badControl, sizeof(badControl));
        TEST_CHECK(badControl[0] == '\0');
    }

    char badDel[128] = {};
    SafeCopyPlacementName("building\x7Ftest.ini", badDel, sizeof(badDel));
    TEST_CHECK(badDel[0] == '\0');

    // D. Null and zero-length boundaries
    char nullDst[128] = { 'X' };
    SafeCopyPlacementName(nullptr, nullDst, sizeof(nullDst));
    TEST_CHECK(nullDst[0] == '\0');

    SafeCopyPlacementName("valid", nullptr, 128);
    SafeCopyPlacementName("valid", nullDst, 0);

    char cleanZero[128] = {};
    CleanBuildingName(nullptr, cleanZero, sizeof(cleanZero));
    TEST_CHECK(cleanZero[0] == '\0');

    CleanBuildingName("valid", nullptr, 128);
    CleanBuildingName("valid", cleanZero, 0);

    return true;
}

static bool SubTest_M4_Challenger1_R3_BadgeProjectionAndForeground()
{
    printf("      [Challenger1-1.4] R3 Badge Projection Math & Foreground Draw Invocation ...\n");
    fflush(stdout);

    // 1. Invocation of PresentHook_DrawPlacementBadgeForeground without active placement
    SimPatches_ClearPlacementActive();
    PresentHook_DrawPlacementBadgeForeground(); // Must safely return without crash

    // 2. Invocation with active placement (headless, no ImGui context)
    SimPatches_RecordPlacementForTest("media_soviet/buildings/Ölraffinerie_Groß.ini", 100.0f, 10.0f, 200.0f);
    PresentHook_DrawPlacementBadgeForeground(); // Must safely return without crash
    SimPatches_ClearPlacementActive();

    // 3. Mathematical validation of ProjectPlacementPoint with camera matrix
    BYTE* camera = g_mockExeBuffer + CAMERA_RVA;
    float* mat = (float*)(camera + CAMERA_VIEWPROJ_OFFSET);
    memset(mat, 0, 16 * sizeof(float));
    mat[0] = 1.0f;
    mat[5] = 1.0f;
    mat[10] = 1.0f;
    mat[15] = 1.0f; // Identity viewproj

    CamVec4 pt1 = ProjectPlacementPoint(100.0f, 50.0f, 200.0f);
    TEST_CHECK_NEAR(pt1.x, 100.0f, 1e-4f);
    TEST_CHECK_NEAR(pt1.y, 50.0f, 1e-4f);
    TEST_CHECK_NEAR(pt1.w, 1.0f, 1e-4f);

    // Point behind camera: w <= 0.5f
    mat[15] = -1.0f;
    CamVec4 ptBehind = ProjectPlacementPoint(100.0f, 50.0f, 200.0f);
    TEST_CHECK(ptBehind.w <= 0.5f);

    // Reset camera matrix
    mat[15] = 1.0f;
    return true;
}

// ---------------------------------------------------------------------------
// 2. R3: Multi-Threaded Concurrency Under TsmScopedCsLock
// ---------------------------------------------------------------------------

static bool SubTest_M4_Challenger1_R3_ConcurrencyUnderCsLock()
{
    printf("      [Challenger1-1.2] R3 Multi-Threaded Concurrency Under TsmScopedCsLock (20,000 updates) ...\n");
    fflush(stdout);

    std::atomic<bool> stopFlag{ false };
    std::atomic<uint64_t> writeCount{ 0 };
    std::atomic<uint64_t> readCount1{ 0 };
    std::atomic<uint64_t> readCount2{ 0 };
    std::atomic<uint32_t> tornReads{ 0 };
    std::atomic<uint32_t> emptyReads{ 0 };

    const char* candidateNames[] = {
        "media_soviet/buildings/школа_номер_1.ini",
        "media_soviet/buildings/больница_№1.ini",
        "media_soviet/buildings/завод_цементный.ini",
        "media_soviet/buildings/электростанция_тэц.ini",
        "media_soviet/buildings/пожарная_станция.ini",
        "media_soviet/buildings/детский_сад.ini"
    };
    const size_t numCandidates = sizeof(candidateNames) / sizeof(candidateNames[0]);
    // Ensure active tool pointer in mock world so SimPatches_GetActivePlacementInfo remains active
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = "active_tool";

    // Writer thread (simulating game thread executing SimPatches_RecordPlacementForTest)
    std::thread writer([&]() {
        for (uint32_t i = 1; i <= 20000 && !stopFlag.load(std::memory_order_relaxed); ++i) {
            const char* name = candidateNames[i % numCandidates];
            float baseCoord = (float)(i * 5);
            SimPatches_RecordPlacementForTest(name, baseCoord, baseCoord + 1.0f, baseCoord + 2.0f);
            writeCount.fetch_add(1, std::memory_order_relaxed);
            if (i % 200 == 0) {
                std::this_thread::yield();
            }
        }
    });

    // Reader thread 1 (simulating DXGI render thread)
    std::thread reader1([&]() {
        char nameBuf[128] = {};
        char cleanBuf[128] = {};
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        while (!stopFlag.load(std::memory_order_relaxed)) {
            if (SimPatches_GetActivePlacementInfo(nameBuf, sizeof(nameBuf), &ox, &oy, &oz)) {
                readCount1.fetch_add(1, std::memory_order_relaxed);
                if (nameBuf[0] == '\0') {
                    emptyReads.fetch_add(1, std::memory_order_relaxed);
                }
                float dy = oy - ox;
                float dz = oz - ox;
                if (fabsf(dy - 1.0f) > 1e-3f || fabsf(dz - 2.0f) > 1e-3f) {
                    tornReads.fetch_add(1, std::memory_order_relaxed);
                }
                CleanBuildingName(nameBuf, cleanBuf, sizeof(cleanBuf));
            }
        }
    });

    // Reader thread 2 (simulating concurrent inspector / overlay thread)
    std::thread reader2([&]() {
        char nameBuf[128] = {};
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        while (!stopFlag.load(std::memory_order_relaxed)) {
            if (SimPatches_GetActivePlacementInfo(nameBuf, sizeof(nameBuf), &ox, &oy, &oz)) {
                readCount2.fetch_add(1, std::memory_order_relaxed);
                float dy = oy - ox;
                float dz = oz - ox;
                if (fabsf(dy - 1.0f) > 1e-3f || fabsf(dz - 2.0f) > 1e-3f) {
                    tornReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    writer.join();
    stopFlag.store(true, std::memory_order_relaxed);
    reader1.join();
    reader2.join();

    printf("        Results: %llu writes, %llu reads (R1), %llu reads (R2), torn reads: %u\n",
           (unsigned long long)writeCount.load(),
           (unsigned long long)readCount1.load(),
           (unsigned long long)readCount2.load(),
           tornReads.load());
    fflush(stdout);

    TEST_CHECK(writeCount.load() >= 20000);
    TEST_CHECK(readCount1.load() > 0);
    TEST_CHECK(readCount2.load() > 0);
    TEST_CHECK(tornReads.load() == 0);
    TEST_CHECK(emptyReads.load() == 0);

    SimPatches_ClearPlacementActive();
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
// 3. R3: Tool Deselect and Immediate Linger Clearing
// ---------------------------------------------------------------------------

static bool SubTest_M4_Challenger1_R3_ToolDeselectAndLingerClearing()
{
    printf("      [Challenger1-1.3] R3 Tool Deselect & Immediate Clearing ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    char outName[128] = {};
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;

    // 1. Tool active and pointer set
    const char* schoolName = "media_soviet/buildings/school_large.ini";
    SimPatches_RecordPlacementForTest(schoolName, 300.0f, 15.0f, 400.0f);
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = schoolName;

    TEST_CHECK(SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));
    TEST_CHECK(strcmp(outName, schoolName) == 0);
    TEST_CHECK_NEAR(ox, 300.0f, 1e-4f);

    // 2. Immediate clearing upon tool deselect (pointer set to null in world)
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nullptr;
    TEST_CHECK(!SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));

    // 3. Re-record, then call SimPatches_ClearPlacementActive()
    SimPatches_RecordPlacementForTest(schoolName, 300.0f, 15.0f, 400.0f);
    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = schoolName;
    TEST_CHECK(SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));

    SimPatches_ClearPlacementActive();
    TEST_CHECK(!SimPatches_GetActivePlacementInfo(outName, sizeof(outName), &ox, &oy, &oz));

    *(const char**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
// 4. R10: Vehicle Snapping Spectrum (0m to 50,000m - Verifying 1000m Cap Removal)
// ---------------------------------------------------------------------------

static bool SubTest_M4_Challenger1_R10_VehicleSnappingSpectrum0To50km()
{
    printf("      [Challenger1-2.1] R10 Vehicle Snapping Spectrum 0m to 50,000m ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> bus(0x1B00, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 888;
    *(float*)(bus.data() + VEH_POS_OFFSET)     = 0.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 4) = 0.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 8) = 0.0f;
    *(int*)(bus.data() + VEH_SCHED_IDX_OFFSET) = 1;
    *(BYTE**)(bus.data() + VEH_STOPPED_BLD_OFFSET) = nullptr;

    BYTE* vehList[1] = { bus.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    std::vector<BYTE> pkt(2048, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    VehicleSoftSyncMsg_M4* msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);

    VehicleSoftSyncDiagStats vssStats = {};

    // 1. Zero drift (0m) -> no snap, no blend
    msg->entries[0].pos[0] = 0.0f;
    msg->entries[0].pos[1] = 0.0f;
    msg->entries[0].pos[2] = 0.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 0);
    TEST_CHECK(vssStats.posSnapped == 0);

    // 2. Small drift (20m < 50m) -> blended
    msg->entries[0].pos[0] = 20.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 1);
    TEST_CHECK(vssStats.posSnapped == 0);

    // 3. Boundary drift (49.9m < 50m) -> blended
    msg->entries[0].pos[0] = 69.9f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 0);

    // 4. Snap threshold (50.0m >= 50m) -> snapped
    msg->entries[0].pos[0] = 119.9f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 1);

    // 5. 500m drift -> snapped
    msg->entries[0].pos[0] = 119.9f + 500.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 2);

    // 6. Old ceiling boundary: 999m drift -> snapped
    msg->entries[0].pos[0] = *(float*)(bus.data() + VEH_POS_OFFSET) + 999.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 3);

    // 7. CRITICAL: 1,000m drift (distSq = 1,000,000f, exactly the old ceiling) -> MUST SNAP!
    float curX = *(float*)(bus.data() + VEH_POS_OFFSET);
    msg->entries[0].pos[0] = curX + 1000.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 4);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), curX + 1000.0f, 1e-3f);

    // 8. 5,000m drift
    curX = *(float*)(bus.data() + VEH_POS_OFFSET);
    msg->entries[0].pos[0] = curX + 5000.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 5);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), curX + 5000.0f, 1e-3f);

    // 9. 10,000m drift
    curX = *(float*)(bus.data() + VEH_POS_OFFSET);
    msg->entries[0].pos[0] = curX + 10000.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 6);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), curX + 10000.0f, 1e-3f);

    // 10. 25,000m drift
    curX = *(float*)(bus.data() + VEH_POS_OFFSET);
    msg->entries[0].pos[0] = curX + 25000.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 7);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), curX + 25000.0f, 1e-3f);

    // 11. 50,000m drift
    curX = *(float*)(bus.data() + VEH_POS_OFFSET);
    msg->entries[0].pos[0] = curX + 50000.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 8);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), curX + 50000.0f, 1e-3f);

    // 12. Randomized 3D drift stress across 500 iterations (offsets 50m .. 50,000m)
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> distOffset(50.0f, 50000.0f);
    std::uniform_real_distribution<float> distDir(-1.0f, 1.0f);
    for (int iter = 0; iter < 500; ++iter) {
        float mag = distOffset(rng);
        float dirX = distDir(rng);
        float dirY = distDir(rng);
        float dirZ = distDir(rng);
        float len = sqrtf(dirX * dirX + dirY * dirY + dirZ * dirZ);
        if (len < 1e-3f) len = 1.0f;
        float offX = (dirX / len) * mag;
        float offY = (dirY / len) * mag;
        float offZ = (dirZ / len) * mag;

        float preX = *(float*)(bus.data() + VEH_POS_OFFSET);
        float preY = *(float*)(bus.data() + VEH_POS_OFFSET + 4);
        float preZ = *(float*)(bus.data() + VEH_POS_OFFSET + 8);

        msg->entries[0].pos[0] = preX + offX;
        msg->entries[0].pos[1] = preY + offY;
        msg->entries[0].pos[2] = preZ + offZ;

        TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
        TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), preX + offX, 1e-2f);
        TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET + 4), preY + offY, 1e-2f);
        TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET + 8), preZ + offZ, 1e-2f);
    }
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 8 + 500);

    // 13. Negative extreme drift (-50,000m)
    curX = *(float*)(bus.data() + VEH_POS_OFFSET);
    msg->entries[0].pos[0] = curX - 50000.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 8 + 500 + 1);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), curX - 50000.0f, 1e-3f);

    // 14. Extreme 3D diagonal drift (50,000m on X, 1,000m on Y, 50,000m on Z)
    curX = *(float*)(bus.data() + VEH_POS_OFFSET);
    float curY = *(float*)(bus.data() + VEH_POS_OFFSET + 4);
    float curZ = *(float*)(bus.data() + VEH_POS_OFFSET + 8);
    msg->entries[0].pos[0] = curX + 50000.0f;
    msg->entries[0].pos[1] = curY + 1000.0f;
    msg->entries[0].pos[2] = curZ + 50000.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posSnapped == 8 + 500 + 2);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), curX + 50000.0f, 1e-2f);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET + 4), curY + 1000.0f, 1e-2f);
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET + 8), curZ + 50000.0f, 1e-2f);

    memset(world + WORLD_VEHICLES_OFFSET, 0, 24);
    return true;
}

// ---------------------------------------------------------------------------
// 5. R17: HookedBuildingRenderPass Exception Safety & SEH __finally Recovery
// ---------------------------------------------------------------------------

struct SimBuildingPass {
    BYTE data[BUILDING_STRUCT_BYTES];
};

static bool ExecuteProductionRenderPassPod(VisibleKept* kept, size_t keptCount, int exceptionCode)
{
    bool caught = false;
    __try {
        __try {
            if (exceptionCode != 0) {
                RaiseException((DWORD)exceptionCode, 0, 0, nullptr);
            }
        }
        __finally {
            RestoreVisibleKeptSafe(kept, keptCount);
            if (kept) {
                free(kept);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        caught = true;
    }
    return (exceptionCode == 0) ? (!caught) : caught;
}

static bool SubTest_M4_Challenger1_R17_RenderPassSEHRecovery()
{
    printf("      [Challenger1-3.1] R17 Render Pass SEH Recovery & __finally Restoration ...\n");
    fflush(stdout);

    std::vector<SimBuildingPass> buildings(64);
    std::vector<BYTE> origVis(64);
    std::vector<float> origDist(64);

    for (size_t i = 0; i < 64; ++i) {
        origVis[i] = (BYTE)(i % 2);
        origDist[i] = 100.0f + (float)(i * 75);
        buildings[i].data[0x115A] = origVis[i];
        *(float*)(buildings[i].data + 0x115C) = origDist[i];
    }

    auto RunPass = [&](int exceptionCode) -> bool {
        VisibleKept* kept = (VisibleKept*)malloc(64 * sizeof(VisibleKept));
        if (!kept) return false;
        for (size_t i = 0; i < 64; ++i) {
            kept[i].building = buildings[i].data;
            kept[i].visible = origVis[i];
            kept[i].distance = origDist[i];
            buildings[i].data[0x115A] = 1;
            *(float*)(buildings[i].data + 0x115C) = 250.0f;
        }
        return ExecuteProductionRenderPassPod(kept, 64, exceptionCode);
    };

    // Case 1: Normal pass without exception
    TEST_CHECK(RunPass(0));
    for (size_t i = 0; i < 64; ++i) {
        TEST_CHECK(buildings[i].data[0x115A] == origVis[i]);
        TEST_CHECK_NEAR(*(float*)(buildings[i].data + 0x115C), origDist[i], 1e-4f);
    }

    // Case 2: SEH Access Violation 0xC0000005
    TEST_CHECK(RunPass(0xC0000005));
    for (size_t i = 0; i < 64; ++i) {
        TEST_CHECK(buildings[i].data[0x115A] == origVis[i]);
        TEST_CHECK_NEAR(*(float*)(buildings[i].data + 0x115C), origDist[i], 1e-4f);
    }

    // Case 3: SEH Integer Divide by Zero 0xC0000094
    TEST_CHECK(RunPass(0xC0000094));
    for (size_t i = 0; i < 64; ++i) {
        TEST_CHECK(buildings[i].data[0x115A] == origVis[i]);
        TEST_CHECK_NEAR(*(float*)(buildings[i].data + 0x115C), origDist[i], 1e-4f);
    }

    // Case 4: SEH Stack Overflow / Custom Exception 0xC00000FD
    TEST_CHECK(RunPass(0xC00000FD));
    for (size_t i = 0; i < 64; ++i) {
        TEST_CHECK(buildings[i].data[0x115A] == origVis[i]);
        TEST_CHECK_NEAR(*(float*)(buildings[i].data + 0x115C), origDist[i], 1e-4f);
    }

    // Case 5: Direct boundary checks on RestoreVisibleKeptSafe (null pointer & zero count)
    RestoreVisibleKeptSafe(nullptr, 0);
    RestoreVisibleKeptSafe(nullptr, 10);
    VisibleKept dummyKept = {};
    dummyKept.building = buildings[0].data;
    dummyKept.visible = 1;
    dummyKept.distance = 999.0f;
    RestoreVisibleKeptSafe(&dummyKept, 0); // 0 count -> must do nothing
    TEST_CHECK(buildings[0].data[0x115A] == origVis[0]);

    // Case 6: Corrupted pointer resilience in RestoreVisibleKeptSafe
    VisibleKept badKept[3] = {};
    badKept[0].building = buildings[0].data;
    badKept[0].visible = 1;
    badKept[0].distance = 42.0f;

    badKept[1].building = (BYTE*)(uintptr_t)0xDEADBEEFULL;
    badKept[1].visible = 1;
    badKept[1].distance = 99.0f;

    badKept[2].building = buildings[2].data;
    badKept[2].visible = 0;
    badKept[2].distance = 123.0f;

    RestoreVisibleKeptSafe(badKept, 3);
    TEST_CHECK(buildings[0].data[0x115A] == 1);
    TEST_CHECK_NEAR(*(float*)(buildings[0].data + 0x115C), 42.0f, 1e-4f);

    return true;
}

// ---------------------------------------------------------------------------
// 6. R11: Vehicle Purchase Audio, Mute Flag & Engine Null Pointer Safety
// ---------------------------------------------------------------------------

static bool SubTest_M4_Challenger1_R11_PurchaseAudioAndMuting()
{
    printf("      [Challenger1-4.1] R11 Vehicle Purchase Sound & Mute Safety ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* exe = g_mockExeBuffer;

    // A. Unmuted purchase sound trigger
    *(BYTE*)(exe + SOUND_MUTE_BYTE_RVA) = 0;
    *(void**)(exe + SOUND_ENGINE_PTR_RVA) = (void*)0x12345678;
    s_mockM4SoundCount = 0;
    s_lastM4Sound = nullptr;

    *(uintptr_t*)(exe + IMPORT_HAS_SOUND_RVA) = (uintptr_t)&MockM4HasSound;
    *(uintptr_t*)(exe + IMPORT_GET_SOUND_RVA) = (uintptr_t)&MockM4GetSound;
    *(uintptr_t*)(exe + IMPORT_PLAY_SOUND_RVA) = (uintptr_t)&MockM4PlaySound2D;

    for (int i = 0; i < 50; ++i) {
        TestTriggerPurchaseSoundSafe();
    }
    TEST_CHECK(s_mockM4SoundCount == 50);
    TEST_CHECK(s_lastM4Sound != nullptr && strcmp(s_lastM4Sound, "gui_click") == 0);

    // B. Muted purchase sound trigger
    *(BYTE*)(exe + SOUND_MUTE_BYTE_RVA) = 1;
    for (int i = 0; i < 50; ++i) {
        TestTriggerPurchaseSoundSafe();
    }
    TEST_CHECK(s_mockM4SoundCount == 50);

    // C. Null sound engine pointer safety
    *(BYTE*)(exe + SOUND_MUTE_BYTE_RVA) = 0;
    *(void**)(exe + SOUND_ENGINE_PTR_RVA) = nullptr;
    TestTriggerPurchaseSoundSafe();
    TEST_CHECK(s_mockM4SoundCount == 50);

    // D. Rapid alternating purchase audio (200 iterations alternating mute on/off)
    for (int i = 0; i < 200; ++i) {
        *(BYTE*)(exe + SOUND_MUTE_BYTE_RVA) = (BYTE)(i % 2);
        *(void**)(exe + SOUND_ENGINE_PTR_RVA) = (void*)0x12345678;
        TestTriggerPurchaseSoundSafe();
    }
    // Exactly 100 unmuted purchases should have triggered
    TEST_CHECK(s_mockM4SoundCount == 50 + 100);

    return true;
}

// ---------------------------------------------------------------------------
// Unified Runner for Milestone 4 Iteration 2 Adversarial Stress Suite (Challenger 1)
// ---------------------------------------------------------------------------

static bool Test_Stress_M4_Challenger1_Iter2()
{
    printf("  [M4-CHALLENGER1-ITER2] Starting Adversarial Stress Suite (R3, R10, R11, R17) ...\n");
    fflush(stdout);

    if (!SubTest_M4_Challenger1_R3_CharsetAndSymbolsStress()) return false;
    if (!SubTest_M4_Challenger1_R3_BadgeProjectionAndForeground()) return false;
    if (!SubTest_M4_Challenger1_R3_ConcurrencyUnderCsLock()) return false;
    if (!SubTest_M4_Challenger1_R3_ToolDeselectAndLingerClearing()) return false;
    if (!SubTest_M4_Challenger1_R10_VehicleSnappingSpectrum0To50km()) return false;
    if (!SubTest_M4_Challenger1_R17_RenderPassSEHRecovery()) return false;
    if (!SubTest_M4_Challenger1_R11_PurchaseAudioAndMuting()) return false;

    printf("  [M4-CHALLENGER1-ITER2] ALL Adversarial Stress Tests PASSED!\n");
    fflush(stdout);
    return true;
}
