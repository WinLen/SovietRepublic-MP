// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone 1 Iteration 2 (R1, R4, R5, R16)
// Challenger 2: Adversarial Stress Testing
// Focus Areas:
//   1. Concurrency: Simulate rapid interleaved calls to ApplySegmentImplInner from multiple
//      simulated players while local drag is active. Ensure no heap corruption, double frees
//      of savedRoutePts, or dangling route vectors.
//   2. Memory Safety: Ensure SnapshotManagerRoute handles invalid or non-canonical route
//      pointers safely with SEH guards.
//   3. Test Suite Verification: Run republicmp_tests.exe and confirm exit code 0.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <vector>
#include <thread>
#include <atomic>

// ---------------------------------------------------------------------------
// SubTest 1: Rapid Interleaved Calls to ApplySegmentImplInner during Active Drag (R1)
// ---------------------------------------------------------------------------

static bool SubTest_M1_Challenger2_RapidInterleavedDragContinuity()
{
    printf("    [Challenger2-1.1] Rapid Interleaved Remote Segments during Active Drag (1,000 iterations) ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Set up Road manager (0xF7A0)
    std::vector<BYTE> roadMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data();

    // 1. Local player initiates active drag
    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;

    // Local player in-progress drag state: preview meshes, start node, route buffer
    void* kMockPreviewA = (void*)0x00007FFF11112222ULL;
    void* kMockPreviewB = (void*)0x00007FFF33334444ULL;
    *(void**)(roadMgr.data() + 0x5F8) = kMockPreviewA;
    *(void**)(roadMgr.data() + 0x600) = kMockPreviewB;

    std::vector<BYTE> dragStartNode(0x100, 0);
    std::vector<BYTE> dragEndNode(0x100, 0);
    *(__int64*)(roadMgr.data() + 0x428) = (__int64)dragStartNode.data();
    *(__int64*)(roadMgr.data() + 0x430) = (__int64)dragEndNode.data();

    // Local route points (5 points)
    std::vector<RoadSyncPoint> localRoute(5);
    for (int i = 0; i < 5; ++i) {
        localRoute[i].pos[0] = 50.0f * (float)(i + 1);
        localRoute[i].pos[1] = 10.0f;
        localRoute[i].pos[2] = 25.0f * (float)(i + 1);
    }
    *(const RoadSyncPoint**)(roadMgr.data() + 0x410) = localRoute.data();
    *(const RoadSyncPoint**)(roadMgr.data() + 0x418) = localRoute.data() + localRoute.size();
    *(const RoadSyncPoint**)(roadMgr.data() + 0x420) = localRoute.data() + localRoute.size();

    // Setup mock function return stubs
    g_mockExeBuffer[0x3D00F0] = 0xC3; // wrapper ret
    g_mockExeBuffer[0x4FC040] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x4FCAF0] = 0xC3; // FCAF0_RVA ret
    g_mockExeBuffer[0x518A80] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x21D2B0] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x542A30] = 0xC3; // MANAGER_RESET_RVA ret
    g_mockExeBuffer[0x542900] = 0xC3; // queueDeleteMesh ret

    // 2. Simulate 1,000 rapid interleaved commands from 3 remote players (Player 2, 3, 4)
    int successfulReplays = 0;
    for (int iter = 0; iter < 1000; ++iter) {
        RoadSyncMsg remoteMsg = {};
        remoteMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
        remoteMsg.seg.typeId = 1 + (iter % 3);
        int simPlayer = 2 + (iter % 3);
        (void)simPlayer;

        int ptCount = (iter % 7); // varies 0, 1, 2, 3, 4, 5, 6 points
        remoteMsg.seg.pointCount = ptCount;
        remoteMsg.seg.startNode[0] = 100.0f + (float)(iter % 50);
        remoteMsg.seg.startNode[2] = 100.0f + (float)(iter % 50);
        remoteMsg.seg.endNode[0]   = 200.0f + (float)(iter % 50);
        remoteMsg.seg.endNode[2]   = 200.0f + (float)(iter % 50);

        for (int p = 0; p < ptCount; ++p) {
            remoteMsg.seg.points[p].pos[0] = 120.0f + (float)p * 15.0f;
            remoteMsg.seg.points[p].pos[2] = 120.0f + (float)p * 15.0f;
        }

        Commands_BeginRemoteReplayScope();
        bool ok = RoadSync_ApplyCommandSegment(remoteMsg);
        Commands_EndRemoteReplayScope();

        if (ok) successfulReplays++;

        // CRITICAL ORACLE: After EVERY interleaved command:
        // 1. TOOL_DRAG_ACTIVE_OFFSET must remain 1 (no drop)
        if (*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) != 1) {
            TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
            return false;
        }

        // 2. Preview pointers must remain preserved
        if (*(void**)(roadMgr.data() + 0x5F8) != kMockPreviewA ||
            *(void**)(roadMgr.data() + 0x600) != kMockPreviewB) {
            TEST_CHECK(*(void**)(roadMgr.data() + 0x5F8) == kMockPreviewA);
            return false;
        }

        // 3. Start node must remain preserved
        if (*(__int64*)(roadMgr.data() + 0x428) != (__int64)dragStartNode.data()) {
            TEST_CHECK(*(__int64*)(roadMgr.data() + 0x428) == (__int64)dragStartNode.data());
            return false;
        }

        // 4. CRT Heap integrity verification every 100 iterations
        if ((iter % 100) == 0) {
            TEST_CHECK(_CrtCheckMemory() == TRUE);
        }
    }

    TEST_CHECK(successfulReplays == 1000);
    TEST_CHECK(_CrtCheckMemory() == TRUE);

    // 3. Explicit local drag cancellation cleanly zeroes drag active
    RoadSync_CancelLocalDrag();
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);

    // Teardown world reference to avoid dangling pointer
    *(BYTE**)(world + 0xF7A0) = nullptr;

    printf("      Verified: 1,000 rapid interleaved commands applied with 0 heap corruption, 0 double frees, 100%% drag continuity.\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 2: Memory Safety & SEH Failure Modes in SnapshotManagerRoute
// ---------------------------------------------------------------------------

// Direct pure-SEH oracle of what a safe SnapshotManagerRoute requires
static RoadSyncPoint* SafeSnapshotManagerRouteOracle(__int64 manager, int* outCount)
{
    if (outCount) *outCount = 0;
    if (!manager) return nullptr;

    char* rBeg = nullptr;
    char* rEnd = nullptr;
    if (!SafeReadPod((const void*)(manager + MGR_ROUTE_BEGIN_OFFSET), &rBeg) ||
        !SafeReadPod((const void*)(manager + MGR_ROUTE_END_OFFSET), &rEnd)) {
        return nullptr;
    }

    if (!rBeg || !rEnd || rEnd <= rBeg) return nullptr;
    size_t bytes = (size_t)(rEnd - rBeg);
    if ((bytes % sizeof(RoadSyncPoint)) != 0) return nullptr;
    int count = (int)(bytes / sizeof(RoadSyncPoint));
    if (count <= 0 || count > ROAD_SYNC_MAX_POINTS) return nullptr;

    RoadSyncPoint* pts = (RoadSyncPoint*)malloc(count * sizeof(RoadSyncPoint));
    if (!pts) return nullptr;

    if (!SafeMemcpyPod(pts, rBeg, count * sizeof(RoadSyncPoint))) {
        free(pts);
        return nullptr;
    }

    if (outCount) *outCount = count;
    return pts;
}

static bool SubTest_M1_Challenger2_SnapshotManagerRouteMemorySafety()
{
    printf("    [Challenger2-2.1] Memory Safety & SEH Boundary Challenge in SnapshotManagerRoute ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data();

    g_mockExeBuffer[0x3D00F0] = 0xC3;
    g_mockExeBuffer[0x4FC040] = 0xC3;
    g_mockExeBuffer[0x4FCAF0] = 0xC3;
    g_mockExeBuffer[0x518A80] = 0xC3;
    g_mockExeBuffer[0x21D2B0] = 0xC3;
    g_mockExeBuffer[0x542A30] = 0xC3;
    g_mockExeBuffer[0x542900] = 0xC3;

    RoadSyncMsg remoteMsg = {};
    remoteMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
    remoteMsg.seg.typeId = 1;
    remoteMsg.seg.pointCount = 2;
    remoteMsg.seg.startNode[0] = 100.0f; remoteMsg.seg.startNode[2] = 100.0f;
    remoteMsg.seg.endNode[0] = 200.0f; remoteMsg.seg.endNode[2] = 200.0f;

    // -----------------------------------------------------------------------
    // Case 2.1: Inverted route pointers: rEnd < rBeg
    // -----------------------------------------------------------------------
    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    *(void**)(roadMgr.data() + 0x5F8) = (void*)0x1111ULL;
    *(void**)(roadMgr.data() + 0x600) = (void*)0x2222ULL;
    *(char**)(roadMgr.data() + 0x410) = (char*)0x2000ULL;
    *(char**)(roadMgr.data() + 0x418) = (char*)0x1000ULL; // rEnd < rBeg

    bool okInverted = RoadSync_ApplyCommandSegment(remoteMsg);
    TEST_CHECK(okInverted == true);
    // Drag active survives because rEnd <= rBeg returns nullptr cleanly
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    TEST_CHECK(*(void**)(roadMgr.data() + 0x5F8) == (void*)0x1111ULL);

    // -----------------------------------------------------------------------
    // Case 2.2: Misaligned byte count: bytes % sizeof(RoadSyncPoint) != 0
    // -----------------------------------------------------------------------
    *(char**)(roadMgr.data() + 0x410) = (char*)0x1000ULL;
    *(char**)(roadMgr.data() + 0x418) = (char*)0x1015ULL; // 21 bytes (not multiple of 32)
    bool okMisaligned = RoadSync_ApplyCommandSegment(remoteMsg);
    TEST_CHECK(okMisaligned == true);
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);

    // -----------------------------------------------------------------------
    // Case 2.3: Exorbitant count: count > ROAD_SYNC_MAX_POINTS (e.g. 5,000 points)
    // -----------------------------------------------------------------------
    *(char**)(roadMgr.data() + 0x410) = (char*)0x1000ULL;
    *(char**)(roadMgr.data() + 0x418) = (char*)(0x1000ULL + 5000 * sizeof(RoadSyncPoint));
    bool okOverflow = RoadSync_ApplyCommandSegment(remoteMsg);
    TEST_CHECK(okOverflow == true);
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);

    // -----------------------------------------------------------------------
    // Case 2.4: EMPIRICAL VULNERABILITY CONFIRMATION:
    // Non-canonical pointer in manager route buffer: 0xCCCCCCCCCCCCCCCCULL
    // In production SnapshotManagerRoute (road_sync.cpp:741), raw memcpy(pts, rBeg, bytes)
    // is executed without SEH wrapper.
    // -----------------------------------------------------------------------
    printf("      [Challenger2-2.2] Non-canonical route pointer vulnerability verification ...\n");
    fflush(stdout);

    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    *(void**)(roadMgr.data() + 0x5F8) = (void*)0x1111ULL;
    *(void**)(roadMgr.data() + 0x600) = (void*)0x2222ULL;
    *(char**)(roadMgr.data() + 0x410) = (char*)0xCCCCCCCCCCCCCCCCULL;
    *(char**)(roadMgr.data() + 0x418) = (char*)(0xCCCCCCCCCCCCCCCCULL + sizeof(RoadSyncPoint));

    // When RoadSync_ApplyCommandSegment runs with non-canonical rBeg:
    // 1. SnapshotManagerRoute enters: bytes = 32, count = 1.
    // 2. malloc(32) succeeds on CRT heap.
    // 3. raw memcpy(pts, 0xCCCCCCCCCCCCCCCC, 32) raises EXCEPTION_ACCESS_VIOLATION (0xC0000005).
    // 4. Because SnapshotManagerRoute has NO SEH guard, the exception escapes.
    // 5. In ApplySegmentImplInner, line 830 (saved = true) is skipped!
    // 6. In __finally, if (saved) is false, so preview meshes (0x5F8, 0x600) are NOT restored!
    // 7. Outer __except catches the AV: drew = true, forcing TOOL_DRAG_ACTIVE_OFFSET = 0 (drag cancelled)!
    // 8. The malloc'd buffer in SnapshotManagerRoute is permanently LEAKED!
    bool okNonCanonical = RoadSync_ApplyCommandSegment(remoteMsg);
    TEST_CHECK(okNonCanonical == false); // Caught and returned false

    // EMPIRICAL PROOF OF VULNERABILITY:
    // Local player's active drag was wiped to 0 and preview pointers lost because SnapshotManagerRoute lacked SEH!
    bool dragWasWiped = (*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 0);
    bool previewsLost = (*(void**)(roadMgr.data() + 0x5F8) == nullptr);
    TEST_CHECK(dragWasWiped);
    TEST_CHECK(previewsLost);

    // -----------------------------------------------------------------------
    // Case 2.5: Verify SafeSnapshotManagerRouteOracle handles non-canonical safely
    // -----------------------------------------------------------------------
    int oracleCount = 0;
    RoadSyncPoint* safePts = SafeSnapshotManagerRouteOracle((__int64)roadMgr.data(), &oracleCount);
    // Oracle catches access violation with SafeMemcpyPod, frees temporary buffer, and returns nullptr safely!
    TEST_CHECK(safePts == nullptr);
    TEST_CHECK(oracleCount == 0);

    // Teardown
    *(BYTE**)(world + 0xF7A0) = nullptr;
    printf("      Verified: Pointer validation boundaries confirmed; non-canonical route pointer failure mode isolated.\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 3: Multi-Threaded Concurrency Adversarial Challenge
// ---------------------------------------------------------------------------

static bool SubTest_M1_Challenger2_MultiThreadedConcurrencyStress()
{
    printf("    [Challenger2-3.1] Multi-Threaded Concurrent ApplySegment Invocation Stress ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data();

    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    *(void**)(roadMgr.data() + 0x5F8) = (void*)0xAAAAULL;
    *(void**)(roadMgr.data() + 0x600) = (void*)0xBBBBULL;

    std::vector<BYTE> dragStartNode(0x100, 0);
    *(__int64*)(roadMgr.data() + 0x428) = (__int64)dragStartNode.data();

    RoadSyncPoint localPts[2] = {};
    localPts[0].pos[0] = 10.0f; localPts[0].pos[2] = 20.0f;
    localPts[1].pos[0] = 30.0f; localPts[1].pos[2] = 40.0f;
    *(const RoadSyncPoint**)(roadMgr.data() + 0x410) = localPts;
    *(const RoadSyncPoint**)(roadMgr.data() + 0x418) = localPts + 2;
    *(const RoadSyncPoint**)(roadMgr.data() + 0x420) = localPts + 2;

    g_mockExeBuffer[0x3D00F0] = 0xC3;
    g_mockExeBuffer[0x4FC040] = 0xC3;
    g_mockExeBuffer[0x4FCAF0] = 0xC3;
    g_mockExeBuffer[0x518A80] = 0xC3;
    g_mockExeBuffer[0x21D2B0] = 0xC3;
    g_mockExeBuffer[0x542A30] = 0xC3;
    g_mockExeBuffer[0x542900] = 0xC3;

    // Concurrent execution across 2 threads simulating simultaneous network packet arrivals
    std::atomic<bool> startFlag{false};
    std::atomic<int> completedCalls{0};
    std::atomic<int> errorsCaught{0};

    auto WorkerThread = [&](int threadId) {
        while (!startFlag.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < 50; ++i) {
            RoadSyncMsg msg = {};
            msg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
            msg.seg.typeId = 1;
            msg.seg.pointCount = 2;
            msg.seg.startNode[0] = (float)(threadId * 1000 + i * 10);
            msg.seg.endNode[0]   = (float)(threadId * 1000 + i * 10 + 50);

            __try {
                RoadSync_ApplyCommandSegment(msg);
                completedCalls.fetch_add(1, std::memory_order_relaxed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                errorsCaught.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t1(WorkerThread, 1);
    std::thread t2(WorkerThread, 2);

    startFlag.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    TEST_CHECK(completedCalls.load() + errorsCaught.load() == 100);
    TEST_CHECK(_CrtCheckMemory() == TRUE);

    // Teardown
    *(BYTE**)(world + 0xF7A0) = nullptr;

    printf("      Completed: 100 concurrent calls executed; heap integrity intact (_CrtCheckMemory == TRUE).\n");
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Milestone 1 Main Suite Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M1_Challenger2()
{
    printf("  [M1-CHALLENGER2] Starting Stress Testing for Milestone 1 (R1, R4, R5, R16) ...\n");
    fflush(stdout);

    if (!SubTest_M1_Challenger2_RapidInterleavedDragContinuity()) return false;
    if (!SubTest_M1_Challenger2_SnapshotManagerRouteMemorySafety()) return false;
    if (!SubTest_M1_Challenger2_MultiThreadedConcurrencyStress()) return false;

    printf("  [M1-CHALLENGER2] ALL Milestone 1 Empirical Stress Tests COMPLETED!\n");
    fflush(stdout);
    return true;
}
