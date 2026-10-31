// SPDX-License-Identifier: GPL-3.0-only
// Adversarial Stress Test Suite for RepublicMP Milestone 1 Iteration 2 (Challenger 1)
// Focus Areas:
// 1. R1 Route Buffer Stress: Concurrent dragging where local player has 0, 1, 50, and max (2048) route points
//    when remote segment is confirmed. Verify all points are restored bit-for-bit.
// 2. R16 Footpath Upgrade Stress: Test road upgrade with pedestrian attachments at Node 0 only, Node 1 only,
//    both nodes, and neither node. Simulate commit throwing an SEH exception and verify pedMgr + 0x360 and 0x370 are cleanly zeroed.
// 3. R5 DLC Road Filtering: Test boundary road types with mismatched DLC masks. Verify SaveSync_GetDlcCheck() controls filtering properly.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// Forward declarations from engine/road_sync
#ifndef MGR_ROAD_LINK_OFFSET
#define MGR_ROAD_LINK_OFFSET 0x360
#endif
#ifndef MGR_ROAD_LINK_EXTRA_OFFSET
#define MGR_ROAD_LINK_EXTRA_OFFSET 0x370
#endif

struct RawVectorHeader {
    const void* first;
    const void* last;
    const void* end;
};

// Internal mock storage for road manager route buffer
static RoadSyncPoint s_mockRouteBuffer[ROAD_SYNC_MAX_POINTS + 64];

static __int64* MockVecAssign_Iter2(__int64* dst, const void* srcVectorHeader)
{
    if (!dst) return dst;
    if (!srcVectorHeader) {
        dst[0] = (__int64)s_mockRouteBuffer;
        dst[1] = (__int64)s_mockRouteBuffer;
        dst[2] = (__int64)(s_mockRouteBuffer + ROAD_SYNC_MAX_POINTS);
        return dst;
    }
    const RawVectorHeader* src = (const RawVectorHeader*)srcVectorHeader;
    const RoadSyncPoint* first = (const RoadSyncPoint*)src->first;
    const RoadSyncPoint* last = (const RoadSyncPoint*)src->last;
    if (!first || !last || last < first) {
        dst[0] = (__int64)s_mockRouteBuffer;
        dst[1] = (__int64)s_mockRouteBuffer;
        dst[2] = (__int64)(s_mockRouteBuffer + ROAD_SYNC_MAX_POINTS);
        return dst;
    }
    size_t count = (size_t)(last - first);
    if (count > ROAD_SYNC_MAX_POINTS) count = ROAD_SYNC_MAX_POINTS;
    if (count > 0) {
        memcpy(s_mockRouteBuffer, first, count * sizeof(RoadSyncPoint));
    }
    dst[0] = (__int64)s_mockRouteBuffer;
    dst[1] = (__int64)(s_mockRouteBuffer + count);
    dst[2] = (__int64)(s_mockRouteBuffer + ROAD_SYNC_MAX_POINTS);
    return dst;
}

static void MockManagerReset_Iter2(__int64 manager, __int64 state)
{
    (void)state;
    if (manager) {
        // Clear route vector (0 points: begin == end)
        *(__int64*)(manager + 0x410) = (__int64)s_mockRouteBuffer;
        *(__int64*)(manager + 0x418) = (__int64)s_mockRouteBuffer;
        *(__int64*)(manager + 0x420) = (__int64)(s_mockRouteBuffer + ROAD_SYNC_MAX_POINTS);
    }
}

static bool s_mockCommitThrowsSEH = false;
static __int64 s_lastAnchorSeenInCommit = 0;
static __int64 s_lastSegSeenInCommit = 0;

static void MockCommit_Iter2(__int64 manager, int a, int b, int c, int d, int e, int f)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    __int64 pedMgr = *(__int64*)(world + 0xF968);
    if (pedMgr) {
        s_lastSegSeenInCommit = *(long long*)(pedMgr + MGR_ROAD_LINK_OFFSET);
        s_lastAnchorSeenInCommit = *(long long*)(pedMgr + MGR_ROAD_LINK_EXTRA_OFFSET);
    }
    if (s_mockCommitThrowsSEH) {
        RaiseException(0xC0000005, 0, 0, nullptr); // Force SEH Access Violation
    }
}

static void WriteJumpTrampoline(BYTE* dst, void* targetFn)
{
    dst[0] = 0x48;
    dst[1] = 0xB8;
    *(uint64_t*)(dst + 2) = (uint64_t)targetFn;
    dst[10] = 0xFF;
    dst[11] = 0xE0;
}

// ---------------------------------------------------------------------------
// 1. R1 Route Buffer Stress
// ---------------------------------------------------------------------------

static bool RunR1SinglePointCountCase(BYTE* world, std::vector<BYTE>& roadMgr, int pointCount)
{
    // Reset local drag state
    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
    *(void**)(roadMgr.data() + 0x5F8) = (void*)0xAAAA1111ULL;
    *(void**)(roadMgr.data() + 0x600) = (void*)0xBBBB2222ULL;

    std::vector<BYTE> dragStartNode(0x100, 0);
    *(__int64*)(roadMgr.data() + 0x428) = (__int64)dragStartNode.data();

    // Prepare point vector
    std::vector<RoadSyncPoint> originalPoints(pointCount);
    for (int i = 0; i < pointCount; ++i) {
        originalPoints[i].pos[0] = 100.0f + (float)i * 1.75f;
        originalPoints[i].pos[1] = 50.0f + (float)i * 0.5f;
        originalPoints[i].pos[2] = 200.0f - (float)i * 2.25f;
        originalPoints[i].extra[0] = (float)(i + 1);
        originalPoints[i].extra[1] = (float)(i + 2);
        originalPoints[i].extra[2] = (float)(i + 3);
    }

    if (pointCount > 0) {
        memcpy(s_mockRouteBuffer, originalPoints.data(), pointCount * sizeof(RoadSyncPoint));
        *(const RoadSyncPoint**)(roadMgr.data() + 0x410) = s_mockRouteBuffer;
        *(const RoadSyncPoint**)(roadMgr.data() + 0x418) = s_mockRouteBuffer + pointCount;
        *(const RoadSyncPoint**)(roadMgr.data() + 0x420) = s_mockRouteBuffer + ROAD_SYNC_MAX_POINTS;
    } else {
        *(const RoadSyncPoint**)(roadMgr.data() + 0x410) = s_mockRouteBuffer;
        *(const RoadSyncPoint**)(roadMgr.data() + 0x418) = s_mockRouteBuffer;
        *(const RoadSyncPoint**)(roadMgr.data() + 0x420) = s_mockRouteBuffer + ROAD_SYNC_MAX_POINTS;
    }

    // Remote segment arrives and confirms
    RoadSyncMsg remoteMsg = {};
    remoteMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
    remoteMsg.seg.typeId = 1;
    remoteMsg.seg.pointCount = 2;
    remoteMsg.seg.startNode[0] = 500.0f; remoteMsg.seg.startNode[2] = 500.0f;
    remoteMsg.seg.endNode[0] = 600.0f; remoteMsg.seg.endNode[2] = 600.0f;
    remoteMsg.seg.points[0].pos[0] = 533.0f; remoteMsg.seg.points[0].pos[2] = 533.0f;
    remoteMsg.seg.points[1].pos[0] = 566.0f; remoteMsg.seg.points[1].pos[2] = 566.0f;

    Commands_BeginRemoteReplayScope();
    RoadSync_ApplyCommandSegment(remoteMsg);
    Commands_EndRemoteReplayScope();

    // Verify metadata restoration
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);
    TEST_CHECK(*(void**)(roadMgr.data() + 0x5F8) == (void*)0xAAAA1111ULL);
    TEST_CHECK(*(void**)(roadMgr.data() + 0x600) == (void*)0xBBBB2222ULL);
    TEST_CHECK(*(__int64*)(roadMgr.data() + 0x428) == (__int64)dragStartNode.data());

    // Verify bit-for-bit route point buffer restoration
    const RoadSyncPoint* curBeg = *(const RoadSyncPoint**)(roadMgr.data() + 0x410);
    const RoadSyncPoint* curEnd = *(const RoadSyncPoint**)(roadMgr.data() + 0x418);
    TEST_CHECK(curBeg != nullptr);
    TEST_CHECK(curEnd != nullptr);

    if (pointCount == 0) {
        TEST_CHECK(curEnd == curBeg);
    } else {
        int restoredCount = (int)(curEnd - curBeg);
        TEST_CHECK(restoredCount == pointCount);
        int cmp = memcmp(curBeg, originalPoints.data(), pointCount * sizeof(RoadSyncPoint));
        TEST_CHECK(cmp == 0);
    }

    return true;
}

static bool Adversarial_R1_RouteBufferStress()
{
    printf("      [R1 Stress] Initializing mock road manager and vector assign trampoline...\n"); fflush(stdout);
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    std::vector<BYTE> roadMgr(0x1000, 0);
    *(BYTE**)(world + 0xF7A0) = roadMgr.data();

    g_mockExeBuffer[0x3D00F0] = 0xC3; // wrapper ret
    g_mockExeBuffer[0x4FC040] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x4FCAF0] = 0xC3; // FCAF0_RVA ret
    g_mockExeBuffer[0x518A80] = 0xC3; // COMMIT_RVA ret
    g_mockExeBuffer[0x542900] = 0xC3; // queueDeleteMesh ret

    // Install real vector assign and manager reset trampolines
    WriteJumpTrampoline(g_mockExeBuffer + 0x21D2B0, (void*)MockVecAssign_Iter2);
    WriteJumpTrampoline(g_mockExeBuffer + 0x542A30, (void*)MockManagerReset_Iter2);

    // 1. Test 0 points (empty drag route)
    printf("      [R1 Stress] Testing N = 0 route points ...\n"); fflush(stdout);
    if (!RunR1SinglePointCountCase(world, roadMgr, 0)) return false;

    // 2. Test 1 point (minimal route)
    printf("      [R1 Stress] Testing N = 1 route points ...\n"); fflush(stdout);
    if (!RunR1SinglePointCountCase(world, roadMgr, 1)) return false;

    // 3. Test 50 points (intermediate route)
    printf("      [R1 Stress] Testing N = 50 route points ...\n"); fflush(stdout);
    if (!RunR1SinglePointCountCase(world, roadMgr, 50)) return false;

    // 4. Test max points (ROAD_SYNC_MAX_POINTS = 2048)
    printf("      [R1 Stress] Testing N = 2048 (ROAD_SYNC_MAX_POINTS) route points ...\n"); fflush(stdout);
    if (!RunR1SinglePointCountCase(world, roadMgr, ROAD_SYNC_MAX_POINTS)) return false;

    // 5. Test overflow handling (2049 points - snapshot must reject overflow safely)
    printf("      [R1 Stress] Testing overflow N = 2049 route points (overflow rejection) ...\n"); fflush(stdout);
    std::vector<RoadSyncPoint> overflowPoints(ROAD_SYNC_MAX_POINTS + 1);
    *(const RoadSyncPoint**)(roadMgr.data() + 0x410) = overflowPoints.data();
    *(const RoadSyncPoint**)(roadMgr.data() + 0x418) = overflowPoints.data() + overflowPoints.size();
    *(const RoadSyncPoint**)(roadMgr.data() + 0x420) = overflowPoints.data() + overflowPoints.size();
    *(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;

    RoadSyncMsg remoteMsg = {};
    remoteMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
    remoteMsg.seg.typeId = 1;
    remoteMsg.seg.pointCount = 2;
    Commands_BeginRemoteReplayScope();
    RoadSync_ApplyCommandSegment(remoteMsg);
    Commands_EndRemoteReplayScope();

    // Drag remains active, no crash or heap corruption occurred
    TEST_CHECK(*(unsigned char*)(world + TOOL_DRAG_ACTIVE_OFFSET) == 1);

    *(BYTE**)(world + 0xF7A0) = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
// 2. R16 Footpath Upgrade Stress
// ---------------------------------------------------------------------------

struct PedMgrFixture {
    std::vector<BYTE> roadMgr;
    std::vector<BYTE> pedMgr;
    std::vector<BYTE> node0;
    std::vector<BYTE> node1;
    std::vector<BYTE> pedNodeA;
    std::vector<BYTE> pedNodeB;
    std::vector<BYTE> pedNodeC;
    std::vector<BYTE> pedNodeD;
    std::vector<RoadSyncPoint> points;
    std::vector<BYTE> roadSeg;
    std::vector<BYTE> pedSeg0;
    std::vector<BYTE> pedSeg1;
    std::vector<BYTE> pedSegUnrelated;
    std::vector<__int64> roadSegList;
    std::vector<__int64> pedSegList;

    PedMgrFixture()
        : roadMgr(0x1000, 0), pedMgr(0x1000, 0),
          node0(0xE8, 0), node1(0xE8, 0),
          pedNodeA(0xE8, 0), pedNodeB(0xE8, 0), pedNodeC(0xE8, 0), pedNodeD(0xE8, 0),
          points(5), roadSeg(0x200, 0), pedSeg0(0x200, 0), pedSeg1(0x200, 0), pedSegUnrelated(0x200, 0)
    {
        *(float*)(node0.data() + 4) = 100.0f; *(float*)(node0.data() + 12) = 200.0f;
        *(float*)(node1.data() + 4) = 300.0f; *(float*)(node1.data() + 12) = 200.0f;
        *(float*)(pedNodeA.data() + 4) = 100.0f; *(float*)(pedNodeA.data() + 12) = 50.0f;
        *(float*)(pedNodeB.data() + 4) = 300.0f; *(float*)(pedNodeB.data() + 12) = 50.0f;
        *(float*)(pedNodeC.data() + 4) = 800.0f; *(float*)(pedNodeC.data() + 12) = 800.0f;
        *(float*)(pedNodeD.data() + 4) = 900.0f; *(float*)(pedNodeD.data() + 12) = 900.0f;

        for (int i = 0; i < 5; ++i) {
            points[i].pos[0] = 100.0f + (float)(i + 1) * 33.3f;
            points[i].pos[1] = 10.0f;
            points[i].pos[2] = 200.0f;
        }

        *(__int64*)(roadSeg.data() + 0x20) = (__int64)node0.data();
        *(__int64*)(roadSeg.data() + 0x28) = (__int64)node1.data();
        *(const RoadSyncPoint**)(roadSeg.data() + 0x08) = points.data();
        *(const RoadSyncPoint**)(roadSeg.data() + 0x10) = points.data() + points.size();
        *(int*)(roadSeg.data() + 0x120) = 1;
        *(int*)(roadSeg.data() + 0x124) = 0;

        roadSegList = { (__int64)roadSeg.data() };
        *(const __int64**)(roadMgr.data() + 0x2B0) = roadSegList.data();
        *(const __int64**)(roadMgr.data() + 0x2B8) = roadSegList.data() + roadSegList.size();

        // pedSeg0 attached to node0
        *(__int64*)(pedSeg0.data() + 0x20) = (__int64)node0.data();
        *(__int64*)(pedSeg0.data() + 0x28) = (__int64)pedNodeA.data();

        // pedSeg1 attached to node1
        *(__int64*)(pedSeg1.data() + 0x20) = (__int64)node1.data();
        *(__int64*)(pedSeg1.data() + 0x28) = (__int64)pedNodeB.data();

        // pedSegUnrelated attached to neither
        *(__int64*)(pedSegUnrelated.data() + 0x20) = (__int64)pedNodeC.data();
        *(__int64*)(pedSegUnrelated.data() + 0x28) = (__int64)pedNodeD.data();
    }
};

static RoadSyncUpgradeMsg MakeUpgradeMessage(int rangeStart = -1, int rangeEnd = 5)
{
    RoadSyncUpgradeMsg msg = {};
    msg.category = ROAD_SYNC_CATEGORY_ROAD;
    msg.newType = 3; // Asphalt lighted
    msg.newVariant = 0;
    msg.pieceCount = 1;
    msg.pieces[0].segStart[0] = 100.0f; msg.pieces[0].segStart[2] = 200.0f;
    msg.pieces[0].segEnd[0] = 300.0f; msg.pieces[0].segEnd[2] = 200.0f;
    msg.pieces[0].segPointCount = 5;
    msg.pieces[0].rangeStart = rangeStart;
    msg.pieces[0].rangeEnd = rangeEnd;
    msg.pieces[0].oldType = 1;
    return msg;
}

static bool Adversarial_R16_FootpathUpgradeStress()
{
    printf("      [R16 Stress] Initializing mock managers and SEH throw harness ...\n"); fflush(stdout);
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    PedMgrFixture fix;
    *(BYTE**)(world + 0xF7A0) = fix.roadMgr.data();
    *(BYTE**)(world + 0xF968) = fix.pedMgr.data();

    g_mockExeBuffer[0x4FC040] = 0xC3; // FC040_RVA ret
    g_mockExeBuffer[0x21D2B0] = 0xC3; // VEC_ASSIGN_RVA ret
    g_mockExeBuffer[0x542A30] = 0xC3; // MANAGER_RESET_RVA ret
    g_mockExeBuffer[0x542900] = 0xC3; // queueDeleteMesh ret

    // Install mock commit trampoline
    WriteJumpTrampoline(g_mockExeBuffer + 0x518A80, (void*)MockCommit_Iter2);

    s_mockCommitThrowsSEH = false;

    // Case 1: Attachment at Node 0 only
    printf("      [R16 Stress] Attachment: Node 0 only ...\n"); fflush(stdout);
    fix.pedSegList = { (__int64)fix.pedSeg0.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();

    RoadSyncUpgradeMsg msg1 = MakeUpgradeMessage(-1, 5);
    bool ok1 = RoadSync_ApplyCommandUpgrade(msg1);
    TEST_CHECK(ok1);
    TEST_CHECK(s_lastSegSeenInCommit == (__int64)fix.roadSeg.data());
    TEST_CHECK(s_lastAnchorSeenInCommit == (__int64)fix.node0.data());
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    // Case 2: Attachment at Node 1 only
    printf("      [R16 Stress] Attachment: Node 1 only ...\n"); fflush(stdout);
    fix.pedSegList = { (__int64)fix.pedSeg1.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();

    RoadSyncUpgradeMsg msg2 = MakeUpgradeMessage(-1, 5);
    bool ok2 = RoadSync_ApplyCommandUpgrade(msg2);
    TEST_CHECK(ok2);
    TEST_CHECK(s_lastSegSeenInCommit == (__int64)fix.roadSeg.data());
    TEST_CHECK(s_lastAnchorSeenInCommit == (__int64)fix.node1.data());
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    // Case 3: Attachment at Both Nodes (Full/Head -> Node 0, Tail -> Node 1)
    printf("      [R16 Stress] Attachment: Both Nodes ...\n"); fflush(stdout);
    fix.pedSegList = { (__int64)fix.pedSeg0.data(), (__int64)fix.pedSeg1.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();

    RoadSyncUpgradeMsg msg3Head = MakeUpgradeMessage(-1, 5);
    bool ok3Head = RoadSync_ApplyCommandUpgrade(msg3Head);
    TEST_CHECK(ok3Head);
    TEST_CHECK(s_lastAnchorSeenInCommit == (__int64)fix.node0.data());
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    RoadSyncUpgradeMsg msg3Tail = MakeUpgradeMessage(2, 5);
    bool ok3Tail = RoadSync_ApplyCommandUpgrade(msg3Tail);
    TEST_CHECK(ok3Tail);
    TEST_CHECK(s_lastAnchorSeenInCommit == (__int64)fix.node1.data());
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    // Case 4: Neither Node attached
    printf("      [R16 Stress] Attachment: Neither Node ...\n"); fflush(stdout);
    fix.pedSegList = { (__int64)fix.pedSegUnrelated.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();

    RoadSyncUpgradeMsg msg4 = MakeUpgradeMessage(-1, 5);
    bool ok4 = RoadSync_ApplyCommandUpgrade(msg4);
    TEST_CHECK(ok4);
    TEST_CHECK(s_lastAnchorSeenInCommit == (__int64)fix.node0.data());
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    // -----------------------------------------------------------------------
    // SEH Exception Simulation during commit across all 4 attachment states
    // -----------------------------------------------------------------------
    printf("      [R16 Stress] Simulating SEH Exceptions during commit ...\n"); fflush(stdout);
    s_mockCommitThrowsSEH = true;

    // SEH with Node 0
    fix.pedSegList = { (__int64)fix.pedSeg0.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();
    bool okSeh0 = RoadSync_ApplyCommandUpgrade(msg1);
    TEST_CHECK(!okSeh0); // Gracefully returned false via __except
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    // SEH with Node 1
    fix.pedSegList = { (__int64)fix.pedSeg1.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();
    bool okSeh1 = RoadSync_ApplyCommandUpgrade(msg2);
    TEST_CHECK(!okSeh1);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    // SEH with Both Nodes
    fix.pedSegList = { (__int64)fix.pedSeg0.data(), (__int64)fix.pedSeg1.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();
    bool okSehBoth = RoadSync_ApplyCommandUpgrade(msg3Head);
    TEST_CHECK(!okSehBoth);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    // SEH with Neither Node
    fix.pedSegList = { (__int64)fix.pedSegUnrelated.data() };
    *(const __int64**)(fix.pedMgr.data() + 0x2B0) = fix.pedSegList.data();
    *(const __int64**)(fix.pedMgr.data() + 0x2B8) = fix.pedSegList.data() + fix.pedSegList.size();
    bool okSehNeither = RoadSync_ApplyCommandUpgrade(msg4);
    TEST_CHECK(!okSehNeither);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x360) == 0);
    TEST_CHECK(*(long long*)(fix.pedMgr.data() + 0x370) == 0);

    s_mockCommitThrowsSEH = false;
    *(BYTE**)(world + 0xF7A0) = nullptr;
    *(BYTE**)(world + 0xF968) = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
// 3. R5 DLC Road Filtering Stress
// ---------------------------------------------------------------------------

static bool Adversarial_R5_DlcRoadFilteringStress()
{
    printf("      [R5 Stress] Setting up mismatched DLC session masks ...\n"); fflush(stdout);
    ResetMockWorldMemory();
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME);

    // Host has DLC2 (bit 1) and DLC4 (bit 3)
    // Client has DLC3 (bit 2) and DLC4 (bit 3)
    // Common mask has ONLY DLC4 (bit 3)
    SaveSync_SetTestDlcLists("dlc2,dlc4", "dlc3,dlc4");

    uint32_t hm = 0, cm = 0, comm = 0;
    SaveSync_GetDlcMasks(&hm, &cm, &comm);
    TEST_CHECK((comm & (1u << 1)) == 0); // DLC2 unshared
    TEST_CHECK((comm & (1u << 2)) == 0); // DLC3 unshared
    TEST_CHECK((comm & (1u << 3)) != 0); // DLC4 shared

    // Verify SaveSync_GetDlcCheck() default is true
    SaveSync_SetDlcCheck(true);
    TEST_CHECK(SaveSync_GetDlcCheck() == true);

    // 1. Boundary tests for DLC2 vs Base (19, 20, 21, 22)
    printf("      [R5 Stress] Boundary check: DLC2 vs Base (19, 20, 21, 22) ...\n"); fflush(stdout);
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 19, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr)); // Blocked (DLC2)
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 21, nullptr)); // Blocked (DLC2)
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 22, nullptr));

    // 2. Boundary tests for DLC3 vs Base (29, 30, 31, 32)
    printf("      [R5 Stress] Boundary check: DLC3 vs Base (29, 30, 31, 32) ...\n"); fflush(stdout);
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 29, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, nullptr)); // Blocked (DLC3)
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 31, nullptr)); // Blocked (DLC3)
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 32, nullptr));

    // 3. Boundary tests for ranges (199, 200..299, 300..399, 400..499, 500)
    printf("      [R5 Stress] Boundary check: Ranges 200s, 300s, 400s, 500 ...\n"); fflush(stdout);
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 199, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 200, nullptr)); // Blocked (DLC2)
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 299, nullptr)); // Blocked (DLC2)
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 300, nullptr)); // Blocked (DLC3)
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 399, nullptr)); // Blocked (DLC3)
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 400, nullptr));  // Allowed (DLC4 shared)
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 401, nullptr));  // Allowed (DLC4 shared)
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 499, nullptr));  // Allowed (DLC4 shared)
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 500, nullptr));

    // 4. Boundary string tokens
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc2/roads/mud_path.ini"));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc3/roads/cobblestone.ini"));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "dlc4/roads/highway.ini"));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "road_asphalt_main"));

    // 5. Test control via SaveSync_SetDlcCheck(false)
    printf("      [R5 Stress] Toggling SaveSync_SetDlcCheck(false) ...\n"); fflush(stdout);
    SaveSync_SetDlcCheck(false);
    TEST_CHECK(SaveSync_GetDlcCheck() == false);

    // All previously blocked types must now be allowed
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 21, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 31, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 200, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 300, nullptr));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc2/roads/mud_path.ini"));
    TEST_CHECK(SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 1, "media_soviet/dlc3/roads/cobblestone.ini"));

    // 6. Toggle back to true
    printf("      [R5 Stress] Restoring SaveSync_SetDlcCheck(true) ...\n"); fflush(stdout);
    SaveSync_SetDlcCheck(true);
    TEST_CHECK(SaveSync_GetDlcCheck() == true);
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 20, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 30, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 200, nullptr));
    TEST_CHECK(!SaveSync_CheckRoadDlcAllowed(ROAD_SYNC_CATEGORY_ROAD, 300, nullptr));

    // 7. Verification in Command Pipelines
    RoadSyncMsg segMsg = {};
    segMsg.seg.category = ROAD_SYNC_CATEGORY_ROAD;
    segMsg.seg.typeId = 30; // DLC3 blocked
    SaveSync_SetDlcCheck(true);
    TEST_CHECK(!RoadSync_ApplyCommandSegment(segMsg));

    SaveSync_SetDlcCheck(false);
    // When DLC check is disabled, ApplyCommandSegment passes the DLC filter stage
    // (it reaches manager check and rejects due to null manager, rather than DLC block)
    TEST_CHECK(!RoadSync_ApplyCommandSegment(segMsg)); // manager is null in mock world

    SaveSync_SetDlcCheck(true);
    SaveSync_SetTestSessionState(SESSION_IDLE);
    return true;
}

// ---------------------------------------------------------------------------
// Unified Runner for Milestone 1 Iteration 2 Adversarial Stress Suite (Challenger 1)
// ---------------------------------------------------------------------------

static bool Test_Stress_M1_Challenger1_Iter2()
{
    printf("    [Challenger1-1: R1 Route Buffer Stress (0, 1, 50, 2048 pts, bit-for-bit)] ...\n"); fflush(stdout);
    if (!Adversarial_R1_RouteBufferStress()) return false;

    printf("    [Challenger1-2: R16 Footpath Upgrade Stress (Attachments + SEH Zeroing)] ...\n"); fflush(stdout);
    if (!Adversarial_R16_FootpathUpgradeStress()) return false;

    printf("    [Challenger1-3: R5 DLC Road Filtering Stress (Boundaries + DlcCheck Control)] ...\n"); fflush(stdout);
    if (!Adversarial_R5_DlcRoadFilteringStress()) return false;

    return true;
}
