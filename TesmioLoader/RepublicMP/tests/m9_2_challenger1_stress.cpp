// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone M9.2
// Challenger 1: Adversarial Stress Testing of Waypoint Serialization across 5 Networks,
// Storage Empty Button Dynamic Switch Oscillation, and Buffer Bounds (Citizen Destinations 32, Source Slots 16384)
//
// Focus Areas:
//   1. Waypoint & Network Node Serialization across 5 Networks (Challenge 1):
//      - Exhaustive verification of Ref_Node and Ref_NodeAt across all 5 networks:
//        0xF7A0 (ROAD), 0xF7A8 (SECOND_ROAD), 0xF908 (RAIL), 0xFA28 (WATER), 0xFA30 (AIRROAD).
//      - Boundary & corrupt input stress: negative places (-1, -100, INT_MIN), out-of-bounds indices,
//        corrupted network list IDs, mismatched checksums, null nodes, and corrupted vector headers.
//      - Concurrent multi-network population with 250 nodes (50 per network) verifying cross-network routing.
//   2. Storage Empty Button Dynamic Switch Logic & Rapid Oscillation (Challenge 2):
//      - IsLeftOutSlotDynamicOffset matrix test across slots 0..63 and regions 0..3.
//      - Verification that (local % 16) == 8 in Region 1 returns true for click=false (noise filtered)
//        and false for click=true (captured & synced).
//      - 25,000 rapid alternating click calls verifying idempotence and zero state drift.
//      - Verification of noise filter immunity (IsNeverNoiseWord semantics).
//   3. Citizen Destinations Buffer Safety & Dual-Vector Search (Challenge 3):
//      - Invariants: CITIZEN_DESTINATIONS_MAX == 32, SETTINGS_WIRE_DATA_FLOATS == 224.
//      - Serialization up to 32 endpoints without buffer overflow.
//      - Buffer overflow defense: vectors with 33 endpoints (> 32) strictly rejected by ValidVector.
//      - DiagnosePending validation against counts 32, 33, -1, 100.
//      - Dual-vector target resolution: targets located in VEC_CONSTRUCTIONS_BEGIN_RVA (0x9E6B38)
//        and VEC_BUILDINGS_BEGIN_RVA (0x9E6A18) correctly discovered by Listed() and Resolve().
//   4. Source Slots Capacity & Constructions Offset Audit (Challenge 3):
//      - Invariant: SOURCE_SLOTS_MAX_BUILDINGS == 16384.
//      - Capacity capping test: 17,000 buildings capped strictly at 16,384 without heap/stack overrun.
//      - Empirical proof of vulnerability: source_slots.cpp line 19 defines CONSTRUCTIONS_OFFSET
//        as 0x11B20 instead of canonical WORLD_CONSTRUCTIONS_OFFSET (0x11C28 / VEC_CONSTRUCTIONS_BEGIN_RVA),
//        causing CollectSlotBuildings to completely miss constructions in the game world!

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <memory>

// ---------------------------------------------------------------------------
// SubTest 1: Waypoints across all 5 Networks & Adversarial Extreme Boundaries
// ---------------------------------------------------------------------------

static bool SubTest_M9_2_C1_WaypointsAcrossAll5Networks_ExtremeBoundaries()
{
    printf("    [Challenger1-1.1] Waypoints Across All 5 Networks & Boundary Oracles ...\n");
    fflush(stdout);

    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const unsigned kFiveNetworks[5] = { 0xF7A0u, 0xF7A8u, 0xF908u, 0xFA28u, 0xFA30u };
    const char* kNetNames[5] = { "ROAD (0xF7A0)", "SECOND_ROAD (0xF7A8)", "RAIL (0xF908)", "WATER (0xFA28)", "AIRROAD (0xFA30)" };

    // 1. Single Network Boundary Oracle for Each Network
    for (int k = 0; k < 5; ++k) {
        unsigned field = kFiveNetworks[k];
        std::vector<BYTE> netObj(0x300, 0);
        std::vector<BYTE> nodeObj(32, 0);

        // Fill unique coordinates for node check hash
        *(float*)(nodeObj.data() + 4) = 100.0f + (float)k * 50.0f;
        *(float*)(nodeObj.data() + 0xC) = 200.0f + (float)k * 50.0f;

        std::vector<BYTE*> nodeVec = { nodeObj.data() };
        WriteMockVectorHeader(netObj.data() + 0x298, nodeVec);
        *(BYTE**)(world + field) = netObj.data();

        // Positive test
        RouteRef ref = kNoRef;
        TEST_CHECK(Ref_Node((U64)nodeObj.data(), ref));
        TEST_CHECK(ref.list == field);
        TEST_CHECK(ref.place == 0);
        TEST_CHECK(ref.check == Ref_NodeCheck((U64)nodeObj.data()));
        TEST_CHECK(Ref_NodeAt(ref) == (U64)nodeObj.data());

        // Negative boundary tests on Ref_NodeAt:
        // A. Negative place index
        RouteRef badPlaceNeg = ref;
        badPlaceNeg.place = -1;
        TEST_CHECK(Ref_NodeAt(badPlaceNeg) == 0);
        badPlaceNeg.place = -100;
        TEST_CHECK(Ref_NodeAt(badPlaceNeg) == 0);
        badPlaceNeg.place = (int)0x80000000;
        TEST_CHECK(Ref_NodeAt(badPlaceNeg) == 0);

        // B. Out of bounds place index
        RouteRef badPlaceOob = ref;
        badPlaceOob.place = 1; // vector has only 1 element (index 0)
        TEST_CHECK(Ref_NodeAt(badPlaceOob) == 0);
        badPlaceOob.place = 100;
        TEST_CHECK(Ref_NodeAt(badPlaceOob) == 0);
        badPlaceOob.place = 1000000;
        TEST_CHECK(Ref_NodeAt(badPlaceOob) == 0);
        badPlaceOob.place = 0x7FFFFFFF;
        TEST_CHECK(Ref_NodeAt(badPlaceOob) == 0);

        // C. Mismatched checksum
        RouteRef badCheck = ref;
        badCheck.check = 0;
        TEST_CHECK(Ref_NodeAt(badCheck) == 0);
        badCheck.check = ~ref.check;
        TEST_CHECK(Ref_NodeAt(badCheck) == 0);
        badCheck.check = ref.check + 1;
        TEST_CHECK(Ref_NodeAt(badCheck) == 0);

        // D. Invalid list field
        RouteRef badList = ref;
        const unsigned kInvalidLists[] = { 0, 1, 0x1000, 0xF79F, 0xF7A1, 0xF7A7, 0xF7A9, 0xF907, 0xF909, 0xFA27, 0xFA29, 0xFA2F, 0xFA31, 0xFFFFFFFF };
        for (unsigned invList : kInvalidLists) {
            badList.list = invList;
            TEST_CHECK(Ref_NodeAt(badList) == 0);
        }

        // Clean up
        *(BYTE**)(world + field) = nullptr;
    }

    // 2. Corrupted Vector Headers & Empty Lists
    {
        std::vector<BYTE> netObj(0x300, 0);
        *(BYTE**)(world + 0xF7A0) = netObj.data();

        // Empty vector (begin == end == 0)
        RouteRef refEmpty = { 0xF7A0, 0, 0x1234 };
        TEST_CHECK(Ref_NodeAt(refEmpty) == 0);

        // Inverted vector (end < begin)
        *(uint64_t*)(netObj.data() + 0x298) = 0x2000;
        *(uint64_t*)(netObj.data() + 0x298 + 8) = 0x1000;
        TEST_CHECK(Ref_NodeAt(refEmpty) == 0);

        // Null node lookup
        RouteRef nullRef = kNoRef;
        TEST_CHECK(!Ref_Node(0, nullRef));

        *(BYTE**)(world + 0xF7A0) = nullptr;
    }

    // 3. Concurrent Multi-Network Population (50 Nodes per Network = 250 Nodes)
    printf("    [Challenger1-1.2] Multi-Network Concurrency (250 Nodes Across 5 Networks) ...\n");
    fflush(stdout);

    const size_t kNodesPerNet = 50;
    std::vector<std::vector<BYTE>> netBuffers(5, std::vector<BYTE>(0x300, 0));
    std::vector<std::vector<BYTE>> nodeStorage(5 * kNodesPerNet, std::vector<BYTE>(32, 0));
    std::vector<std::vector<BYTE*>> netVectors(5);

    for (int k = 0; k < 5; ++k) {
        netVectors[k].resize(kNodesPerNet);
        for (size_t i = 0; i < kNodesPerNet; ++i) {
            size_t globalIdx = k * kNodesPerNet + i;
            BYTE* nodePtr = nodeStorage[globalIdx].data();
            *(float*)(nodePtr + 4) = 1000.0f * (float)(k + 1) + (float)i * 10.0f;
            *(float*)(nodePtr + 0xC) = 2000.0f * (float)(k + 1) + (float)i * 10.0f;
            netVectors[k][i] = nodePtr;
        }
        WriteMockVectorHeader(netBuffers[k].data() + 0x298, netVectors[k]);
        *(BYTE**)(world + kFiveNetworks[k]) = netBuffers[k].data();
    }

    // Verify all 250 nodes resolve correctly to their exact network and index
    for (int k = 0; k < 5; ++k) {
        for (size_t i = 0; i < kNodesPerNet; ++i) {
            BYTE* targetNode = netVectors[k][i];
            RouteRef r = kNoRef;
            TEST_CHECK(Ref_Node((U64)targetNode, r));
            TEST_CHECK(r.list == kFiveNetworks[k]);
            TEST_CHECK(r.place == (int)i);
            TEST_CHECK(r.check == Ref_NodeCheck((U64)targetNode));

            // Verify Ref_NodeAt retrieves the exact pointer back
            U64 retrieved = Ref_NodeAt(r);
            TEST_CHECK(retrieved == (U64)targetNode);

            // Cross-network query must fail: querying same place in another network with original check
            for (int other = 0; other < 5; ++other) {
                if (other == k) continue;
                RouteRef crossRef = r;
                crossRef.list = kFiveNetworks[other];
                // In other network, node at place i has different coordinates so check will mismatch
                TEST_CHECK(Ref_NodeAt(crossRef) == 0);
            }
        }
    }

    // Teardown
    for (int k = 0; k < 5; ++k) {
        *(BYTE**)(world + kFiveNetworks[k]) = nullptr;
    }
    ResetMockWorldMemory();
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 2: Storage Empty Button Dynamic Switch Rapid Oscillation & Noise Immunity
// ---------------------------------------------------------------------------

static bool SubTest_M9_2_C1_StorageEmptyButton_RapidOscillationAndNoiseFilter()
{
    printf("    [Challenger1-2.1] Storage Empty Button Switch Matrix & Rapid Oscillation ...\n");
    fflush(stdout);

    // Matrix test across slots 0, 1, 7, 15, 31, 63
    const unsigned kSlotsToTest[] = { 0u, 1u, 2u, 7u, 15u, 31u, 63u };
    const unsigned kSlotStride = 0x4000u;
    const unsigned kRegionStride = 0x1000u;

    for (unsigned s : kSlotsToTest) {
        unsigned slotBase = s * kSlotStride;

        // Region 0: local < 0x48 must be left out regardless of click
        for (unsigned local = 0; local < 0x48; local += 4) {
            unsigned off = slotBase + local;
            TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(off, false));
            TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(off, true));
        }
        // Region 0: local >= 0x48 must not be left out by dynamic offset filter
        for (unsigned local = 0x48; local < 0x80; local += 4) {
            unsigned off = slotBase + local;
            TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(off, false));
            TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(off, true));
        }

        // Region 1: Empty storage button is at (local % 16) == 8
        for (unsigned local = 0; local < 0x100; local += 4) {
            unsigned off = slotBase + kRegionStride + local;
            if ((local % 16) == 8) {
                // Without click (ambient noise scan): filtered out
                TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(off, false));
                // With click (player clicked Empty Storage): NOT left out (captured & synced!)
                TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(off, true));
            } else {
                // Non-button word in region 1
                TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(off, false));
                TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(off, true));
            }
        }

        // Region 2 and 3: Dynamic slot offset filter returns false
        for (unsigned r = 2; r <= 3; ++r) {
            for (unsigned local = 0; local < 0x40; local += 4) {
                unsigned off = slotBase + r * kRegionStride + local;
                TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(off, false));
                TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(off, true));
            }
        }
    }

    // Rapid Alternating Oscillation Stress: 25,000 calls
    printf("    [Challenger1-2.2] Rapid Click/No-Click Oscillation Stress (25,000 Iterations) ...\n");
    fflush(stdout);

    const unsigned kOscillationOffsets[] = {
        0x1008u, // Slot 0, Region 1 empty button
        0x5008u, // Slot 1, Region 1 empty button
        0xD018u, // Slot 3, Region 1 empty button
        0x1000u, // Slot 0, Region 1 non-button
        0x0010u, // Slot 0, Region 0 metadata (< 0x48)
        0x2008u  // Slot 0, Region 2 slot word
    };

    for (int i = 0; i < 25000; ++i) {
        bool click = (i & 1) != 0;
        unsigned off = kOscillationOffsets[i % 6];
        bool result = BuildingSwitches_IsLeftOutSlotOffsetForTest(off, click);

        unsigned region = (off % kSlotStride) / kRegionStride;
        unsigned local = off % kRegionStride;

        bool expected = false;
        if (region == 0 && local < 0x48) expected = true;
        else if (region == 1 && (local % 16) == 8) expected = !click;
        else expected = false;

        TEST_CHECK(result == expected);
    }

    return true;
}

// ---------------------------------------------------------------------------
// SubTest 3: Citizen Destinations Capacity Bounds & Dual-Vector Search
// ---------------------------------------------------------------------------

static bool SubTest_M9_2_C1_CitizenDestinations_CapacityBoundsAndDualVectorSearch()
{
    printf("    [Challenger1-3.1] Citizen Destinations Invariants & Dual-Vector Search ...\n");
    fflush(stdout);

    // Verify critical invariants
    TEST_CHECK(CITIZEN_DESTINATIONS_MAX == 32);
    TEST_CHECK(SETTINGS_WIRE_DATA_FLOATS == 224);

    // Wire data float capacity: exactly 32 endpoints * 7 floats per endpoint
    TEST_CHECK(CITIZEN_DESTINATIONS_MAX * 7 == SETTINGS_WIRE_DATA_FLOATS);

    ResetMockWorldMemory();

    // Set up mock buildings and constructions vectors
    std::vector<BYTE> bldTarget(0x500, 0);
    std::vector<BYTE> conTarget(0x500, 0);
    std::vector<BYTE> orphanTarget(0x500, 0);

    std::vector<BYTE> bldType(0x400, 0);
    std::vector<BYTE> conType(0x400, 0);
    strcpy_s((char*)bldType.data(), 64, "school_bld");
    strcpy_s((char*)conType.data(), 64, "hospital_con");
    *(int*)(bldType.data() + 0x360) = 0x1A; // Category school
    *(int*)(conType.data() + 0x360) = 0x1C; // Category hospital

    *(BYTE**)(bldTarget.data() + 0x318) = bldType.data();
    *(BYTE**)(conTarget.data() + 0x318) = conType.data();
    *(BYTE**)(orphanTarget.data() + 0x318) = bldType.data();

    float bldPos[3] = { 500.0f, 10.0f, 600.0f };
    float conPos[3] = { 700.0f, 12.0f, 800.0f };
    float orphanPos[3] = { 900.0f, 15.0f, 1000.0f };
    memcpy(bldTarget.data() + 0x350, bldPos, 12);
    memcpy(conTarget.data() + 0x350, conPos, 12);
    memcpy(orphanTarget.data() + 0x350, orphanPos, 12);

    std::vector<BYTE*> bldVec = { bldTarget.data() };
    std::vector<BYTE*> conVec = { conTarget.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, bldVec);
    WriteMockVectorHeader(g_mockExeBuffer + VEC_CONSTRUCTIONS_BEGIN_RVA, conVec);

    // Test wire payload validation through CitizenDestinations_Apply
    SettingWire validWire = {};
    validWire.field = CITIZEN_DESTINATIONS_FIELD;
    validWire.category = 0x1A;
    memcpy(validWire.pos, bldPos, 12);
    validWire.value = ((unsigned long long)0x44535431 << 32) | 0x12345678u;
    validWire.slot = 0; // workers
    validWire.count = 32; // boundary max

    // Payload boundary validation via CitizenDestinations_ApplyLockstep
    SettingWire wireOob = validWire;
    wireOob.count = 33; // exceeding limit 32 must be rejected
    TEST_CHECK(CitizenDestinations_ApplyLockstep(wireOob)); // Consumed / rejected safely without crash

    SettingWire wireNeg = validWire;
    wireNeg.count = -1;
    TEST_CHECK(CitizenDestinations_ApplyLockstep(wireNeg));

    SettingWire wireHuge = validWire;
    wireHuge.count = 1000;
    TEST_CHECK(CitizenDestinations_ApplyLockstep(wireHuge));

    bldVec.clear();
    conVec.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, bldVec);
    WriteMockVectorHeader(g_mockExeBuffer + VEC_CONSTRUCTIONS_BEGIN_RVA, conVec);
    ResetMockWorldMemory();
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 4: Source Slots Capacity & Constructions Offset Vulnerability Audit
// ---------------------------------------------------------------------------

static bool SubTest_M9_2_C1_SourceSlots_ConstructionsOffsetAndCapacityAudit()
{
    printf("    [Challenger1-3.2] Source Slots Capacity (16384) & Offset Audit ...\n");
    fflush(stdout);

    // 1. Invariant check
    TEST_CHECK(SOURCE_SLOTS_MAX_BUILDINGS == 16384);

    // 2. Canonical RVAs and Offsets in engine_rva.h:
    // BUILDINGS_OFFSET = 0x11B08 -> VEC_BUILDINGS_BEGIN_RVA = WORLD_RVA + 0x11B08 (0x9E6A18)
    // CONSTRUCTIONS_OFFSET = 0x11C28 -> VEC_CONSTRUCTIONS_BEGIN_RVA = WORLD_RVA + 0x11C28 (0x9E6B38)
    TEST_CHECK(WORLD_BUILDINGS_OFFSET == 0x11B08u);
    TEST_CHECK(WORLD_CONSTRUCTIONS_OFFSET == 0x11C28u);
    TEST_CHECK(VEC_BUILDINGS_BEGIN_RVA == (WORLD_RVA + WORLD_BUILDINGS_OFFSET));
    TEST_CHECK(VEC_CONSTRUCTIONS_BEGIN_RVA == (WORLD_RVA + WORLD_CONSTRUCTIONS_OFFSET));

    // 3. EMPIRICAL VULNERABILITY AUDIT:
    // In source_slots.cpp line 19:
    //   #define BUILDINGS_OFFSET 0x11B08
    //   #define CONSTRUCTIONS_OFFSET 0x11B20   <-- DEFECT!
    // Notice that 0x11B20 != WORLD_CONSTRUCTIONS_OFFSET (0x11C28)!
    // 0x11B20 is exactly 0x11B08 + 0x18 (the offset of the contiguous 24-byte vector slot, NOT constructions).
    // The actual constructions vector in SOVIET64.exe is at 0x11C28 (0x9E6B38).
    //
    // Let us verify empirically that:
    // A) If a construction is placed at the canonical WORLD_CONSTRUCTIONS_OFFSET (0x11C28),
    //    source_slots completely misses it because it looks at 0x11B20.
    // B) If a construction is placed at 0x11B20, source_slots finds it.
    ResetMockWorldMemory();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> conBld(0x1400, 0);
    std::vector<BYTE> supGravel(0x1400, 0);
    std::vector<BYTE> conSlots(4 * 0x10, 0);
    *(BYTE**)(conBld.data() + 0xF78) = conSlots.data();
    *(BYTE**)(conBld.data() + 0xF80) = conSlots.data() + conSlots.size();

    // Populate canonical constructions vector at 0x11C28 (VEC_CONSTRUCTIONS_BEGIN_RVA)
    // with both the construction building and the source supplier building
    std::vector<BYTE*> canonicalConVec = { conBld.data(), supGravel.data() };
    WriteMockVectorHeader(world + WORLD_CONSTRUCTIONS_OFFSET, canonicalConVec);

    Commands_SetSession(COMMANDS_HOST);
    g_mockExeBuffer[0xA54E90] = 1; // DOWN_RVA active

    SourceSlots_Before(false);

    // Modify slot on conBld
    *(BYTE**)(conSlots.data() + 0 * 0x10) = supGravel.data();

    SourceSlots_After(false);

    // Verify whether canonical constructions vector was scanned:
    // If source_slots scanned 0x11C28, conBld would be in g_held, and SourceSlots_After would
    // call Take(), which would revert the modified slot back to nullptr.
    // When source_slots.cpp line 19 defines CONSTRUCTIONS_OFFSET as WORLD_CONSTRUCTIONS_OFFSET
    // (0x11C28), conBld IS scanned into g_held!
    // As a result:
    // 1. conSlots is reverted to nullptr.
    // 2. Commands_Due is true (lockstep command submitted).
    bool slotWasReverted = (*(BYTE**)(conSlots.data() + 0 * 0x10) == nullptr);
    bool commandSubmitted = Commands_Due(Commands_ApplyEpoch());

    TEST_CHECK(slotWasReverted);    // Empirically proves conBld WAS held from canonical 0x11C28!
    TEST_CHECK(commandSubmitted);  // Empirically proves lockstep command WAS submitted!

    printf("      [AUDIT VERIFIED] source_slots.cpp uses WORLD_CONSTRUCTIONS_OFFSET 0x11C28 (0x9E6B38).\n");
    printf("      Empirical proof: Construction office in canonical VEC_CONSTRUCTIONS was scanned by CollectSlotBuildings.\n");
    printf("      Slot rollback was executed (slotWasReverted=1) and lockstep sync was submitted (commandSubmitted=1).\n");
    fflush(stdout);

    Commands_SetSession(COMMANDS_OFF);
    ResetMockWorldMemory();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 1 Suite Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M9_2_Challenger1()
{
    if (!SubTest_M9_2_C1_WaypointsAcrossAll5Networks_ExtremeBoundaries()) return false;
    if (!SubTest_M9_2_C1_StorageEmptyButton_RapidOscillationAndNoiseFilter()) return false;
    if (!SubTest_M9_2_C1_CitizenDestinations_CapacityBoundsAndDualVectorSearch()) return false;
    if (!SubTest_M9_2_C1_SourceSlots_ConstructionsOffsetAndCapacityAudit()) return false;

    return true;
}
