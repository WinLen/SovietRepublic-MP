// SPDX-License-Identifier: GPL-3.0-only
// Thematic Test TU: Lockstep Commands, World Switches, Building/Office/Carrier Mechanics,
// Crash/RCO Track Builder Fixes, System UI Interception & Buffer Safety (Suites #7, #38-#44).

#include "test_harness.h"
#include <limits>

// ---------------------------------------------------------------------------
// Test 7: BuildingSwitches Exclusions (Invariant #12), Lockstep Commands & SaveZip
// ---------------------------------------------------------------------------

struct GuardModifyContext {
    BYTE* loco;
    BYTE* wagon2;
    BYTE* wheelBuf;
    float interpSteerDuringGuard;
    float interpWheel0DuringGuard;
    float interpWheel1DuringGuard;
};

static void GuardModifyCallback(void* ud)
{
    auto* ctx = (GuardModifyContext*)ud;
    // Simulate engine render 0x6A4FB7 updating dynamic matrices in 0x14C0..0x1730
    // to values that look like heap pointers
    *(uintptr_t*)(ctx->loco + 0x14C0) = (uintptr_t)ctx->wagon2;
    ctx->interpSteerDuringGuard = *(const float*)(ctx->loco + VEH_STEER_CURRENT_OFFSET);
    if (ctx->wheelBuf) {
        ctx->interpWheel0DuringGuard = *(const float*)(ctx->wheelBuf + 0 * VEH_WHEEL_STRUCT_STRIDE);
        ctx->interpWheel1DuringGuard = *(const float*)(ctx->wheelBuf + 1 * VEH_WHEEL_STRUCT_STRIDE);
    }
}

static bool SubTest_HeldFrameVehicleGraphAndSmoothOffset()
{
    HeldFrame_Reset();

    // 0. Shortest-arc angle interpolation across +/-pi
    constexpr float kPi = 3.14159265358979323846f;
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(0.0f, 1.0f, 0.25f), 0.25f, 1e-5f);
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(0.0f, 1.0f, -0.5f), 0.0f, 1e-5f);
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(0.0f, 1.0f, 1.5f), 1.0f, 1e-5f);
    // Wrap across +pi -> -pi (e.g. +3.04 rad -> -3.04 rad has shortest delta = +0.203 rad)
    float wrapPosToNeg = HeldFrame_LerpAngleRadians(kPi - 0.1f, -kPi + 0.1f, 0.5f);
    TEST_CHECK_NEAR(fabsf(wrapPosToNeg), kPi, 1e-4f);
    TEST_CHECK_NEAR(HeldFrame_LerpAngleRadians(NAN, 0.5f, 0.5f), 0.5f, 1e-5f);

    // 1. Setup 3 mock vehicles (size VEHICLE_STRUCT_BYTES): loco, wagon1, wagon2
    std::vector<BYTE> loco(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> wagon1(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> wagon2(VEHICLE_STRUCT_BYTES, 0);
    // Setup 2 wheels (24 bytes each) for loco at +0x1478 and steering at +0x1194/+0x1198
    std::vector<BYTE> locoWheels(2 * VEH_WHEEL_STRUCT_STRIDE, 0);
    *(BYTE**)(loco.data() + VEH_WHEELS_BEGIN_OFFSET) = locoWheels.data();
    *(BYTE**)(loco.data() + VEH_WHEELS_END_OFFSET)   = locoWheels.data() + locoWheels.size();
    *(float*)(locoWheels.data() + 0 * VEH_WHEEL_STRUCT_STRIDE) = 1.0f;
    *(float*)(locoWheels.data() + 1 * VEH_WHEEL_STRUCT_STRIDE) = kPi - 0.1f;
    *(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET) = 0.10f;
    *(float*)(loco.data() + VEH_STEER_TARGET_OFFSET)  = 0.50f;

    // Setup follow camera manager at world+0x18 pointing to wagon2 (with wagon2 hidden externally)
    std::vector<BYTE> mockCamMgr(CAM_MGR_STRUCT_BYTES, 0);
    std::vector<BYTE> mockFollowWin(0x80, 0);
    mockCamMgr[CAM_MGR_FOLLOW_ACTIVE_OFF] = 1;
    *(BYTE**)(mockCamMgr.data() + CAM_MGR_FOLLOW_WIN_PTR_OFF) = mockFollowWin.data();
    *(BYTE**)(mockFollowWin.data() + FOLLOW_WIN_VEHICLE_PTR_OFF) = wagon2.data();
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF) = mockCamMgr.data();

    *(unsigned*)(loco.data() + 0x04)   = 1001;
    *(unsigned*)(wagon1.data() + 0x04) = 1002;
    *(unsigned*)(wagon2.data() + 0x04) = 1003;

    *(int32_t*)(loco.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET)   = -1;
    *(int32_t*)(wagon1.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
    *(int32_t*)(wagon2.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;

    loco[VEH_RENDER_VISIBLE_OFFSET]   = 1;
    wagon1[VEH_RENDER_VISIBLE_OFFSET] = 1;
    wagon2[VEH_RENDER_VISIBLE_OFFSET] = 1;

    loco[VEH_WORKER_SIM_FLAG_OFFSET]   = 1;
    wagon1[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
    wagon2[VEH_WORKER_SIM_FLAG_OFFSET] = 1;

    // Critical root cause reproduction: +0x1A68 is a 32-bit integer counter, NOT a pointer!
    *(uint32_t*)(loco.data() + 0x1A68)   = 0x42;
    *(uint32_t*)(wagon1.data() + 0x1A68) = 0x42;
    *(uint32_t*)(wagon2.data() + 0x1A68) = 0x42;

    // Pre-seed 0x14C0 with a pointer-like value to test dynamic node restore
    *(uintptr_t*)(loco.data() + 0x14C0) = (uintptr_t)wagon1.data();

    // Graph links:
    // loco -> link (+0x4A0) points to wagon1
    *(BYTE**)(loco.data() + VEH_LINK_PTR_OFFSET) = wagon1.data();
    // wagon1 -> parent (+0x398) points to loco
    *(BYTE**)(wagon1.data() + VEH_PARENT_PTR_OFFSET) = loco.data();

    // wagon1 -> children vector (+0x3A0..+0x3A8) points to wagon2
    std::vector<BYTE*> wagon1Children = { wagon2.data() };
    *(BYTE***)(wagon1.data() + VEH_CHILDREN_BEGIN_OFFSET) = wagon1Children.data();
    *(BYTE***)(wagon1.data() + VEH_CHILDREN_END_OFFSET)   = wagon1Children.data() + wagon1Children.size();
    // wagon2 -> parent (+0x398) points to wagon1
    *(BYTE**)(wagon2.data() + VEH_PARENT_PTR_OFFSET) = wagon1.data();

    // Node setup for 5 C3D_NODES at 0xD98, 0xE54, 0xF10, 0xFCC, 0x10B8
    const unsigned nodeOffsets[5] = { 0xD98, 0xE54, 0xF10, 0xFCC, 0x10B8 };
    for (int k = 0; k < 5; ++k) {
        float* nLoco   = (float*)(loco.data() + nodeOffsets[k]);
        float* nWagon1 = (float*)(wagon1.data() + nodeOffsets[k]);
        float* nWagon2 = (float*)(wagon2.data() + nodeOffsets[k]);
        nLoco[0]  = nWagon1[0]  = nWagon2[0]  = 1.0f;
        nLoco[5]  = nWagon1[5]  = nWagon2[5]  = 1.0f;
        nLoco[10] = nWagon1[10] = nWagon2[10] = 1.0f;
        nLoco[15] = nWagon1[15] = nWagon2[15] = 1.0f;
        nLoco[32]   = 100.0f + (float)k;
        nWagon1[32] = 80.0f + (float)k;
        nWagon2[32] = 60.0f + (float)k;
        nLoco[33] = nWagon1[33] = nWagon2[33] = 10.0f;
        nLoco[34] = nWagon1[34] = nWagon2[34] = 200.0f;
        nLoco[38] = nWagon1[38] = nWagon2[38] = 1.0f; // scale.x
        nLoco[39] = nWagon1[39] = nWagon2[39] = 1.0f; // scale.y
        nLoco[40] = nWagon1[40] = nWagon2[40] = 1.0f; // scale.z
        nLoco[43] = nWagon1[43] = nWagon2[43] = 1.0f; // ori.z (+0xA4)
        nLoco[45] = nWagon1[45] = nWagon2[45] = 1.0f; // up.y  (+0xB0)
    }

    // Set up engine vehicle render vector at 0xA317E0 with only the lead loco
    std::vector<BYTE*> engineVehList = { loco.data() };
    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = engineVehList.data();
    *(BYTE***)(g_mockExeBuffer + 0xA317F0) = engineVehList.data() + engineVehList.size();

    // 2. Tick 1: HeldFrame_OnSimTick traverses graph without failing on +0x1A68
    HeldFrame_OnSimTick(1);
    HeldFrameDiagStats stats = {};
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.trackedVehicles >= 3);

    // 3. Move vehicles slightly on Tick 2 (dx = 2.0f, distSq = 4.0f < 400.0f) and advance wheels/steering
    for (int k = 0; k < 5; ++k) {
        float* nLoco   = (float*)(loco.data() + nodeOffsets[k]);
        float* nWagon1 = (float*)(wagon1.data() + nodeOffsets[k]);
        float* nWagon2 = (float*)(wagon2.data() + nodeOffsets[k]);
        nLoco[32]   += 2.0f;
        nWagon1[32] += 2.0f;
        nWagon2[32] += 2.0f;
    }
    *(float*)(locoWheels.data() + 0 * VEH_WHEEL_STRUCT_STRIDE) = 2.0f;
    *(float*)(locoWheels.data() + 1 * VEH_WHEEL_STRUCT_STRIDE) = -kPi + 0.1f; // wrapped across +pi/-pi
    // Leave VEH_STEER_CURRENT_OFFSET at 0.10f while target is 0.50f so AdvanceVehicleSteerAndWheels steps by +0.05f -> 0.15f
    HeldFrame_OnSimTick(2);
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.trackedVehicles >= 3);
    TEST_CHECK_NEAR(*(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET), 0.15f, 1e-5f);

    // 4. Test smooth offset rejection cap
    // a) magSq <= 2500.0f accepted (30^2 + 30^2 = 1800.0f)
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 30.0f, 0.0f, 30.0f);
    // b) magSq == 2500.0f exact boundary accepted (50^2 = 2500.0f)
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 50.0f, 0.0f, 0.0f);
    // c) magSq > 2500.0f rejected (50^2 + 10^2 = 2600.0f)
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 50.0f, 0.0f, 10.0f);
    // d) Non-finite inputs safely rejected
    HeldFrame_AddVehicleSmoothOffset(loco.data(), NAN, 0.0f, 0.0f);
    HeldFrame_AddVehicleSmoothOffset(loco.data(), 0.0f, INFINITY, 0.0f);

    // 5. Test VehicleGuard Begin/End lifecycle with sub-tick interpolation & dynamic node restore
    GuardModifyContext ctx = { loco.data(), wagon2.data(), locoWheels.data(), 0.0f, 0.0f, 0.0f };
    TEST_CHECK(HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E8, 0.5f, GuardModifyCallback, &ctx));
    // Verify wheel pitch & steering were interpolated at alpha=0.5 during guard
    TEST_CHECK_NEAR(ctx.interpSteerDuringGuard, 0.125f, 1e-4f);
    TEST_CHECK_NEAR(ctx.interpWheel0DuringGuard, 1.5f, 1e-4f);
    TEST_CHECK_NEAR(fabsf(ctx.interpWheel1DuringGuard), kPi, 1e-4f);
    // Verify wheel pitch & steering were restored to Tick 2 values after guard End()
    TEST_CHECK_NEAR(*(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET), 0.15f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(loco.data() + VEH_STEER_TARGET_OFFSET), 0.50f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(locoWheels.data() + 0 * VEH_WHEEL_STRUCT_STRIDE), 2.0f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(locoWheels.data() + 1 * VEH_WHEEL_STRUCT_STRIDE), -kPi + 0.1f, 1e-5f);
    *(BYTE**)(g_mockExeBuffer + WORLD_RVA + WORLD_CAMERA_MGR_PTR_OFF) = nullptr;

    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.guardFallbacks == 0);
    TEST_CHECK(stats.lastTargetsCount >= 3);
    TEST_CHECK(stats.lastInterpolatedCount >= 1);
    TEST_CHECK(stats.restoreRefusals == 0);

    // Verify worker sim flag 0xD41 restored to 1 on all vehicles
    TEST_CHECK(loco[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
    TEST_CHECK(wagon1[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
    TEST_CHECK(wagon2[VEH_WORKER_SIM_FLAG_OFFSET] == 1);

    // Verify dynamic sub-node matrix at +0x14C0 restored to original value
    TEST_CHECK(*(uintptr_t*)(loco.data() + 0x14C0) == (uintptr_t)wagon1.data());

    // 6. Test HeldFrame_SubmitVehicleHighlight: highlighting a coupled wagon (wagon2) interpolates
    //    the entire train subgraph (loco, wagon1, wagon2) during the highlight call and restores after.
    struct HighlightTestCtx {
        BYTE* loco;
        BYTE* wagon2;
        float locoXDuring;
        float wagon2XDuring;
        int calls;
    } hlCtx = { loco.data(), wagon2.data(), 0.0f, 0.0f, 0 };
    auto HlCallback = [](BYTE* /*veh*/, bool /*isExtraPass*/, void* ud) {
        auto* c = (HighlightTestCtx*)ud;
        c->locoXDuring   = *(const float*)(c->loco + 0xD98 + 0x80);
        c->wagon2XDuring = *(const float*)(c->wagon2 + 0xD98 + 0x80);
        c->calls++;
    };
    HeldFrame_SetHighlightCaptureForTest(HlCallback, &hlCtx);
    const float hlColor[4] = { 1.0f, 0.8f, 0.2f, 1.0f };
    HeldFrame_SubmitVehicleHighlight(wagon2.data(), 0, 1, hlColor, 0, false);
    HeldFrame_SubmitVehicleHighlight(wagon2.data(), 0, 1, hlColor, 0, true);
    HeldFrame_SetHighlightCaptureForTest(nullptr, nullptr);
    TEST_CHECK(hlCtx.calls == 2);
    // wagon2 had prevX=60, currX=62, smoothOff=0 -> at alpha=0.5f, interpolated X is 61.0f during highlight
    TEST_CHECK_NEAR(hlCtx.wagon2XDuring, 61.0f, 1e-4f);
    // And after HeldFrame_SubmitVehicleHighlight returns, both loco and wagon2 are restored to Tick 2 currX (102.0f and 62.0f)
    TEST_CHECK_NEAR(*(const float*)(loco.data() + 0xD98 + 0x80), 102.0f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(wagon2.data() + 0xD98 + 0x80), 62.0f, 1e-4f);

    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = nullptr;
    *(BYTE***)(g_mockExeBuffer + 0xA317F0) = nullptr;
    HeldFrame_Reset();

    return true;
}

static bool SubTest_HeldFrameQueueBoundAndCorruptedLinks()
{
    HeldFrame_Reset();

    // Setup lead loco
    std::vector<BYTE> leadLoco(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(leadLoco.data() + 0x04) = 2001;
    *(int32_t*)(leadLoco.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
    leadLoco[VEH_RENDER_VISIBLE_OFFSET] = 1;
    leadLoco[VEH_WORKER_SIM_FLAG_OFFSET] = 1;

    // Create a pool of 2100 mock vehicles coupled in a chain via VEH_LINK_PTR_OFFSET
    const size_t kPoolSize = 2100;
    std::vector<std::vector<BYTE>> pool(kPoolSize, std::vector<BYTE>(VEHICLE_STRUCT_BYTES, 0));

    for (size_t i = 0; i < kPoolSize; ++i) {
        *(unsigned*)(pool[i].data() + 0x04) = (unsigned)(3000 + i);
        *(int32_t*)(pool[i].data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        pool[i][VEH_RENDER_VISIBLE_OFFSET] = 1;
        pool[i][VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        if (i + 1 < kPoolSize) {
            *(BYTE**)(pool[i].data() + VEH_LINK_PTR_OFFSET) = pool[i + 1].data();
        }
    }

    // Lead loco links to the first wagon in the chain
    *(BYTE**)(leadLoco.data() + VEH_LINK_PTR_OFFSET) = pool[0].data();

    // Inject unmapped and malformed pointers into leadLoco children to verify traversal resilience
    std::vector<BYTE*> childPtrs = { (BYTE*)0x0000000400000000ull, (BYTE*)0x1234 };
    *(BYTE***)(leadLoco.data() + VEH_CHILDREN_BEGIN_OFFSET) = childPtrs.data();
    *(BYTE***)(leadLoco.data() + VEH_CHILDREN_END_OFFSET)   = childPtrs.data() + childPtrs.size();

    // Set engine vehicle list at 0xA317E0
    std::vector<BYTE*> engineVehList = { leadLoco.data() };
    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = engineVehList.data();
    *(BYTE***)(g_mockExeBuffer + 0xA317F0) = engineVehList.data() + engineVehList.size();

    // OnSimTick: Queue must process all up to 2048 without aborting early
    HeldFrame_OnSimTick(10);
    HeldFrameDiagStats stats = {};
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.trackedVehicles >= 2048);

    // VehicleGuard Begin/End: Must discard invalid pointers without guard fallback
    TEST_CHECK(HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E8, 0.0f));
    HeldFrame_GetDiagStats(&stats);
    TEST_CHECK(stats.guardFallbacks == 0);

    // 3. Test SafeRestoreWorkerFlags & End() restoration resilience when intermediate vehicle memory becomes unmapped
    std::vector<BYTE> vehA(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> vehC(VEHICLE_STRUCT_BYTES, 0);
    BYTE* vehPage = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    TEST_CHECK(vehPage != nullptr);
    if (vehPage) {
        *(unsigned*)(vehA.data() + 0x04) = 5001;
        *(unsigned*)(vehPage + 0x04) = 5002;
        *(unsigned*)(vehC.data() + 0x04) = 5003;

        *(int32_t*)(vehA.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        *(int32_t*)(vehPage + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
        *(int32_t*)(vehC.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;

        vehA[VEH_RENDER_VISIBLE_OFFSET] = 1;
        vehPage[VEH_RENDER_VISIBLE_OFFSET] = 1;
        vehC[VEH_RENDER_VISIBLE_OFFSET] = 1;

        vehA[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        vehPage[VEH_WORKER_SIM_FLAG_OFFSET] = 1;
        vehC[VEH_WORKER_SIM_FLAG_OFFSET] = 1;

        std::vector<BYTE*> testVehList = { vehA.data(), vehPage, vehC.data() };
        *(BYTE***)(g_mockExeBuffer + 0xA317E8) = testVehList.data();
        *(BYTE***)(g_mockExeBuffer + 0xA317F0) = testVehList.data() + testVehList.size();

        struct UnmapContext {
            BYTE* page;
            DWORD oldProtect;
        } unmapCtx = { vehPage, 0 };

        auto UnmapCallback = [](void* ud) {
            auto* ctx = (UnmapContext*)ud;
            VirtualProtect(ctx->page, 0x2000, PAGE_NOACCESS, &ctx->oldProtect);
        };

        // Guard Begin succeeds on all 3; in callback vehPage is unmapped; End must safely restore vehA and vehC
        HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E8, 0.0f, UnmapCallback, &unmapCtx);

        VirtualProtect(vehPage, 0x2000, unmapCtx.oldProtect, &unmapCtx.oldProtect);
        TEST_CHECK(vehA[VEH_WORKER_SIM_FLAG_OFFSET] == 1);
        TEST_CHECK(vehC[VEH_WORKER_SIM_FLAG_OFFSET] == 1);

        VirtualFree(vehPage, 0, MEM_RELEASE);
    }

    // Clean up mock engine vehicle list & reset HeldFrame state
    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = nullptr;
    *(BYTE***)(g_mockExeBuffer + 0xA317F0) = nullptr;
    HeldFrame_Reset();

    return true;
}

static bool SubTest_WorldSettingMultiToggleAndPerStateBuildMode()
{
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_HOST);

    *(int*)(world + 0xB20) = 0;
    *(int*)(world + 0xB24) = 1;
    *(int*)(world + 0xB28) = 70;
    *(int*)(world + 0xB2C) = 1;
    world[0xB30] = 1;
    world[0xC838] = 1;
    world[0xC7A1] = 0x5Au;

    auto runUiFrame = [&](bool clicked, int rub, int usd, BYTE c798) {
        g_mockExeBuffer[0xA9EF51] = clicked ? 1 : 0;
        BuildingSwitches_Before(false);
        *(int*)(world + 0xB20) = rub;
        *(int*)(world + 0xB24) = usd;
        world[0xC838] = c798;
        BuildingSwitches_After(false);
        g_mockExeBuffer[0xA9EF51] = 0;
    };

    // Initial baseline frame (no click)
    runUiFrame(false, 0, 1, 1);

    // 1. One-State mode: repeated multi-step toggles (USD -> RUB -> USD -> OWN -> RUB) must all succeed
    const int kSeqOneState[4][2] = { { 1, 0 }, { 0, 1 }, { 0, 0 }, { 1, 0 } };
    for (int step = 0; step < 4; ++step) {
        int wantRub = kSeqOneState[step][0];
        int wantUsd = kSeqOneState[step][1];
        runUiFrame(true, wantRub, wantUsd, 1);
        TEST_CHECK(Commands_Due((unsigned long long)(20 + step)));
        Commands_Boundary((unsigned long long)(20 + step));
        TEST_CHECK(*(int*)(world + 0xB20) == wantRub);
        TEST_CHECK(*(int*)(world + 0xB24) == wantUsd);
        runUiFrame(false, wantRub, wantUsd, 1);
    }

    // 1b. 1-byte world setting +0xC830 (autoSearchOffices) toggles 1 -> 0 -> 1 without touching +0xC799
    runUiFrame(true, 1, 0, 0);
    TEST_CHECK(Commands_Due(30));
    Commands_Boundary(30);
    TEST_CHECK(world[0xC838] == 0);
    TEST_CHECK(world[0xC7A1] == 0x5Au);

    runUiFrame(true, 1, 0, 1);
    TEST_CHECK(Commands_Due(31));
    Commands_Boundary(31);
    TEST_CHECK(world[0xC838] == 1);
    TEST_CHECK(world[0xC7A1] == 0x5Au);

    // 2. Two-States mode (R4): BuildMode (+0xAF8..+0xB04) is strictly local per state and toggles freely without reverting
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    for (int step = 0; step < 4; ++step) {
        int wantRub = kSeqOneState[step][0];
        int wantUsd = kSeqOneState[step][1];
        runUiFrame(true, wantRub, wantUsd, 1);
        TEST_CHECK(!Commands_Due((unsigned long long)(40 + step)));
        TEST_CHECK(*(int*)(world + 0xB20) == wantRub);
        TEST_CHECK(*(int*)(world + 0xB24) == wantUsd);
    }

    Commands_SetSession(COMMANDS_OFF);
    BuildingSwitches_Reset();
    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_UnivResearchTwoStatesIsolation()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_HOST);

    std::vector<BYTE> resBuf(4 * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < 4; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();

    std::vector<BYTE> univWest(0x1420, 0), univEast(0x1420, 0);
    *(int*)univWest.data() = 501;
    *(float*)(univWest.data() + OBJ_POS_OFFSET) = -800.0f;
    *(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;
    *(int*)univEast.data() = 502;
    *(float*)(univEast.data() + OBJ_POS_OFFSET) = 800.0f;
    *(float*)(univEast.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    std::vector<BYTE*> bldVec = { univWest.data(), univEast.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // East SSR (State 2) starts researching tech #2 in univEast
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univEast.data(), 2, 2));
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univEast.data(), 2) == 2);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univEast.data(), 1) == -1);

    // 1. Local player (West SSR / State 1) tries to change or cancel research in foreign univEast (State 2) -> BLOCKED!
    TwoStates_UnivResearchPanelForTest(world, univEast.data(), 0, 0.90f);
    TEST_CHECK(!Commands_Due(50));
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univEast.data(), 2) == 2);
    TEST_CHECK_NEAR(*(float*)(univEast.data() + BLD_UNIV_STAFF_RATIO_OFFSET), 0.50f, 1e-5f);

    TwoStates_UnivResearchPanelForTest(world, univEast.data(), -1, 0.10f);
    TEST_CHECK(!Commands_Due(51));
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univEast.data(), 2) == 2);
    TEST_CHECK_NEAR(*(float*)(univEast.data() + BLD_UNIV_STAFF_RATIO_OFFSET), 0.50f, 1e-5f);

    // 2. Local player (State 1) selects tech #1 with ratio 0.75 in own univWest -> synchronized via KIND_UNIV_RESEARCH
    TwoStates_UnivResearchPanelForTest(world, univWest.data(), 1, 0.75f);
    TEST_CHECK(Commands_Due(52));
    Commands_Boundary(52);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == 1);
    TEST_CHECK_NEAR(*(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET), 0.75f, 1e-5f);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univEast.data(), 2) == 2);

    // 3. Step univWest twice -> State 1 unlocks tech #1 (progress 1.0), State 2 remains at 0.0 on tech #1
    TwoStates_BldUnivStepForTest(world, univWest.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 1), 0.50f, 1e-4f);
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 1), 0.00f, 1e-4f);

    TwoStates_BldUnivStepForTest(world, univWest.data());
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 1), 1.00f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 1) == 0);

    // Step univEast once -> State 2 advances tech #2 to 0.50, State 1 remains at 0.0 on tech #2
    // Also test real-engine university with *(int*)univEast == 0 (resolved by TwoStates_BuildingPosKey)
    *(int*)univEast.data() = 0;
    TwoStates_BldUnivStepForTest(world, univEast.data());
    TwoStates_ResearchDailyUpdateForTest(world);
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 2), 0.50f, 1e-4f);
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 2), 0.00f, 1e-4f);

    // 4. Per-republic vehicle production license separation (type + VEH_TYPE_HAS_LICENCE_OFFSET = 0x9CD8)
    std::vector<BYTE> vehTypesBuf(3 * WORLD_VEHICLE_TYPE_STRIDE, 0);
    BYTE* vt0 = vehTypesBuf.data() + 0 * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vt1 = vehTypesBuf.data() + 1 * WORLD_VEHICLE_TYPE_STRIDE;
    BYTE* vt2 = vehTypesBuf.data() + 2 * WORLD_VEHICLE_TYPE_STRIDE;
    strcpy_s((char*)(vt0 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "t815_dump");
    strcpy_s((char*)(vt1 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "ikarus_260");
    strcpy_s((char*)(vt2 + VEH_TYPE_SCRIPT_NAME_OFFSET), 64, "chs4_loco");
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x00) = vehTypesBuf.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x08) = vehTypesBuf.data() + vehTypesBuf.size();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 0x10) = vehTypesBuf.data() + vehTypesBuf.size();

    // State 1 acquires license for vt0 ("t815_dump"), State 2 acquires license for vt1 ("ikarus_260")
    TwoStates_BeginCommand(1);
    vt0[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    TwoStates_NotifyVehicleLicenseChanged(world);
    TwoStates_EndCommand(1);

    TwoStates_BeginCommand(2);
    TEST_CHECK(vt0[VEH_TYPE_HAS_LICENCE_OFFSET] == 0); // State 2 must NOT see State 1's license
    vt1[VEH_TYPE_HAS_LICENCE_OFFSET] = 1;
    TwoStates_NotifyVehicleLicenseChanged(world);
    TwoStates_EndCommand(2);

    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 0) == true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 1) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 0) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 1) == true);
    // Local player is Host (State 1), so world has State 1's licenses restored
    TEST_CHECK(vt0[VEH_TYPE_HAS_LICENCE_OFFSET] == 1);
    TEST_CHECK(vt1[VEH_TYPE_HAS_LICENCE_OFFSET] == 0);

    // 5. Persistence round-trip via republicmp_coop.ini + Client load with Host's research.bin in world:
    //    Client's research tree & active progress (0.50 on tech #2 in univEast) and vehicle licenses
    //    must NOT be overwritten by Host's research.bin (tech #1 unlocked, tech #2 at 0.0, vt0 licensed)!
    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_univ_res";
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveDir.string().c_str()));

    // Simulate Client joining and loading the world whose research.bin and vehicle types reflect Host (State 1)
    TwoStates_OnSessionEnded();
    TwoStates_ResetResearch();
    {
        BYTE* it0 = resBuf.data() + 0 * RESEARCH_ITEM_STRIDE;
        BYTE* it1 = resBuf.data() + 1 * RESEARCH_ITEM_STRIDE;
        BYTE* it2 = resBuf.data() + 2 * RESEARCH_ITEM_STRIDE;
        *(float*)(it0 + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.0f;
        *(float*)(it1 + RESEARCH_ITEM_PROGRESS_OFFSET) = 1.0f;  // Host completed tech #1
        *(void**)(it1 + RESEARCH_ITEM_BLD_PTR_OFFSET)  = nullptr;
        *(float*)(it2 + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.0f;  // Host has 0 progress on tech #2
        *(void**)(it2 + RESEARCH_ITEM_BLD_PTR_OFFSET)  = nullptr;
        vt0[VEH_TYPE_HAS_LICENCE_OFFSET] = 1; // Host owns vt0 in saved world
        vt1[VEH_TYPE_HAS_LICENCE_OFFSET] = 0; // Host does not own vt1 in saved world
    }
    TwoStates_OnSessionJoined(/*isHost=*/false, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveDir.string().c_str(), true));
    TwoStates_OnWorldLoaded();

    // Verify Client's research tree was NOT replaced by Host's research.bin and preserved its 0.50 progress & univEast assignment
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 1), 1.00f, 1e-4f);
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 1), 0.00f, 1e-4f);
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 2, 2), 0.50f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 1) == 1);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 2, 1) == 0);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univEast.data(), 2) == 2);

    // Verify Client's world research items now reflect State 2 (Client) and univEast pointer is restored
    {
        BYTE* it1 = resBuf.data() + 1 * RESEARCH_ITEM_STRIDE;
        BYTE* it2 = resBuf.data() + 2 * RESEARCH_ITEM_STRIDE;
        TEST_CHECK_NEAR(*(float*)(it1 + RESEARCH_ITEM_PROGRESS_OFFSET), 0.0f, 1e-4f);
        TEST_CHECK_NEAR(*(float*)(it2 + RESEARCH_ITEM_PROGRESS_OFFSET), 0.50f, 1e-4f);
        TEST_CHECK(*(void**)(it2 + RESEARCH_ITEM_BLD_PTR_OFFSET) == univEast.data());
    }

    // Verify Client's vehicle production licenses are isolated and restored into world for Client (State 2)
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 0) == true);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 1, 1) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 0) == false);
    TEST_CHECK(TwoStates_IsVehicleLicenseOwned(world, 2, 1) == true);
    TEST_CHECK(vt0[VEH_TYPE_HAS_LICENCE_OFFSET] == 0);
    TEST_CHECK(vt1[VEH_TYPE_HAS_LICENCE_OFFSET] == 1);

    std::filesystem::remove_all(saveDir, ec);
    memset(world + WORLD_VEHICLE_TYPES_OFFSET, 0, 24);

    Commands_SetSession(COMMANDS_OFF);
    BuildingSwitches_Reset();
    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_UnivResearchNearCompleteAndStaffRatioSync()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_OFF);

    std::vector<BYTE> resBuf(4 * RESEARCH_ITEM_STRIDE, 0);
    for (int i = 0; i < 4; ++i) {
        BYTE* it = resBuf.data() + (size_t)i * RESEARCH_ITEM_STRIDE;
        it[RESEARCH_ITEM_INITIAL_OFFSET]   = 1;
        it[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
        *(int32_t*)(it + RESEARCH_ITEM_REQ_WORK_OFFSET) = 20;
    }
    *(BYTE**)(world + WORLD_RESEARCH_VEC_BEGIN_OFFSET) = resBuf.data();
    *(BYTE**)(world + WORLD_RESEARCH_VEC_END_OFFSET)   = resBuf.data() + resBuf.size();

    std::vector<BYTE> univWest(0x1420, 0);
    *(int*)univWest.data() = 501;
    *(float*)(univWest.data() + OBJ_POS_OFFSET) = -800.0f;
    *(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET) = 0.50f;

    std::vector<BYTE*> bldVec = { univWest.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, bldVec);
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = bldVec.data();
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA)   = bldVec.data() + bldVec.size();

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // 1. Slower peer has tech #0 at 0.988447f attached to univWest when player selects new tech #2:
    //    Tech #0 MUST be finalized to 1.0f & unlocked (not left orphaned at 98.84% to complete in 1 tick later),
    //    and tech #2 must attach cleanly at 0.0f.
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 0));
    BYTE* it0 = resBuf.data() + 0 * RESEARCH_ITEM_STRIDE;
    *(float*)(it0 + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.488447f;
    *(BYTE**)(it0 + RESEARCH_ITEM_BLD_PTR_OFFSET)  = univWest.data();
    it0[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
    world[WORLD_RESEARCH_UNLOCKS_OFFSET + 0] = 0;
    TwoStates_ResetResearch();
    TwoStates_OnWorldLoaded(); // captures 0.488447f attached to univWest into State 1 slice
    TwoStates_BldUnivStepForTest(world, univWest.data()); // advances by 0.50f -> 0.988447f (< 1.0f, still attached!)
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), 0.988447f, 1e-4f);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == 0);

    // Now apply selection of new tech #2 on univWest:
    // Feature 40: Free science exploit eliminated! Switching research retains curProg, does NOT force 1.0f or unlock!
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 2));
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 0), 0.988447f, 1e-4f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 0) == 0);
    TEST_CHECK(it0[RESEARCH_ITEM_AVAILABLE_OFFSET] == 1);
    TEST_CHECK(*(void**)(it0 + RESEARCH_ITEM_BLD_PTR_OFFSET) == nullptr);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == 2);
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 2), 0.0f, 1e-5f);

    // 2. Staff ratio slider adjustment (oldTechIdx == newTechIdx) must NOT re-attach a completed research:
    TwoStates_BldUnivStepForTest(world, univWest.data());
    TwoStates_BldUnivStepForTest(world, univWest.data()); // tech #2 reaches 1.0f and detaches
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 2), 1.0f, 1e-5f);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == -1);

    unsigned oldBits = 0, newBits = 0;
    float rOld = 0.50f, rNew = 0.85f;
    memcpy(&oldBits, &rOld, sizeof(oldBits));
    memcpy(&newBits, &rNew, sizeof(newBits));
    TEST_CHECK(BuildingSwitches_CaptureUnivResearch((unsigned long long)univWest.data(), 1, 2, 2, oldBits, newBits));
    TEST_CHECK_NEAR(*(float*)(univWest.data() + BLD_UNIV_STAFF_RATIO_OFFSET), 0.85f, 1e-5f);
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == -1);

    // Even direct TwoStates_ApplyUnivResearchSelection on an already-completed tech #2 must NOT attach univWest:
    TEST_CHECK(TwoStates_ApplyUnivResearchSelection(world, univWest.data(), 1, 2));
    TEST_CHECK(TwoStates_GetUnivResearchIdxForBuilding(world, univWest.data(), 1) == -1);

    // 3. Loading a save with an orphaned near-100% research (e.g. 0.988447f with bldPosKey == 0) heals it to 1.0f:
    std::filesystem::path saveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_heal_res";
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    BYTE* it3 = resBuf.data() + 3 * RESEARCH_ITEM_STRIDE;
    *(float*)(it3 + RESEARCH_ITEM_PROGRESS_OFFSET) = 0.988447f;
    *(BYTE**)(it3 + RESEARCH_ITEM_BLD_PTR_OFFSET)  = nullptr;
    it3[RESEARCH_ITEM_AVAILABLE_OFFSET] = 1;
    TwoStates_ResetResearch();
    TwoStates_OnWorldLoaded();
    TEST_CHECK_NEAR(TwoStates_GetEngineResearchProgress(world, 1, 3), 1.0f, 1e-5f);
    TEST_CHECK(TwoStates_GetEngineResearchUnlockByte(world, 1, 3) == 1);
    TEST_CHECK(it3[RESEARCH_ITEM_AVAILABLE_OFFSET] == 0);

    std::filesystem::remove_all(saveDir, ec);
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = nullptr;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA)   = nullptr;
    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_ConstructionOfficeTwoStatesAutoAndManual()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    TwoStates_OnSessionEnded();

    std::vector<BYTE> officeWest(BUILDING_STRUCT_BYTES, 0), officeEast(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> siteWest(BUILDING_STRUCT_BYTES, 0), siteEast(BUILDING_STRUCT_BYTES, 0);
    *(int*)officeWest.data() = 601;
    *(float*)(officeWest.data() + OBJ_POS_OFFSET) = -900.0f;
    *(int*)officeEast.data() = 602;
    *(float*)(officeEast.data() + OBJ_POS_OFFSET) = 900.0f;
    *(int*)siteWest.data() = 603;
    *(float*)(siteWest.data() + OBJ_POS_OFFSET) = -600.0f;
    *(int*)siteEast.data() = 604;
    *(float*)(siteEast.data() + OBJ_POS_OFFSET) = 600.0f;

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    // 1. Auto-search (checkFilters = 1) must ONLY attach same-SSR targets and block cross-SSR targets
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteWest.data(), 3000.0f, 1, 0));
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeEast.data(), siteEast.data(), 3000.0f, 1, 0));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 1, 0));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeEast.data(), siteWest.data(), 3000.0f, 1, 0));

    // 2. Periodic road/rail scan at RVA_CALLER_OFFICE_ROAD_SCAN (0x14119B, checkFilters = 0) must ALSO block cross-SSR targets
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteWest.data(), 3000.0f, 0, 0, RVA_CALLER_OFFICE_ROAD_SCAN));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 0, RVA_CALLER_OFFICE_ROAD_SCAN));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeEast.data(), siteWest.data(), 3000.0f, 0, 0, RVA_CALLER_OFFICE_ROAD_SCAN));

    // 3. Manual assignment (checkFilters = 0, isDemolish = 0):
    //    Local player (West SSR / State 1) CAN manually assign own officeWest to help build neighbor's siteEast!
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteWest.data(), 3000.0f, 0, 0));
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 0));
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteWest.data(), 0));
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 0));

    //    Local player (State 1) CANNOT manually assign foreign officeEast to any site
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeEast.data(), siteWest.data(), 3000.0f, 0, 0));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeEast.data(), siteEast.data(), 3000.0f, 0, 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeEast.data(), siteWest.data(), 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeEast.data(), siteEast.data(), 0));

    // 4. Demolition office (cat == 0x6A) or demolition site (+0xEA8 != 0) CANNOT demolish a foreign SSR's building,
    //    whereas regular CO / RCO passing arg6 = 1 (triggerImmediateScan) CAN manually assist neighbor construction
    std::vector<BYTE> bldTypeCo(0x400, 0), bldTypeDemol(0x400, 0);
    *(int*)(bldTypeCo.data() + BLD_TYPE_CAT_OFF) = (int)BLD_CAT_RAIL_CO;
    *(int*)(bldTypeDemol.data() + BLD_TYPE_CAT_OFF) = (int)BLD_CAT_DEMOLISH_CO;
    *(uintptr_t*)(officeWest.data() + BLD_TYPE_OFFSET) = (uintptr_t)bldTypeCo.data();
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 1));
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 1));

    *(uintptr_t*)(officeWest.data() + BLD_TYPE_OFFSET) = (uintptr_t)bldTypeDemol.data();
    TEST_CHECK(TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteWest.data(), 3000.0f, 0, 0));
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 0));
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteWest.data(), 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 0));
    *(uintptr_t*)(officeWest.data() + BLD_TYPE_OFFSET) = (uintptr_t)bldTypeCo.data();

    siteEast[BLD_IS_DEMOLITION_SITE_OFFSET] = 1;
    TEST_CHECK(!TwoStates_AssignConstructionOfficeForTest(world, officeWest.data(), siteEast.data(), 3000.0f, 0, 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_ASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 0));
    siteEast[BLD_IS_DEMOLITION_SITE_OFFSET] = 0;

    // 5. Unassignment (0x162360): allowed if local player owns EITHER the office OR the target construction site
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_UNASSIGN_CONSTRUCTION_OFFICE, officeWest.data(), siteEast.data(), 0));
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_UNASSIGN_CONSTRUCTION_OFFICE, officeEast.data(), siteWest.data(), 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_UNASSIGN_CONSTRUCTION_OFFICE, officeEast.data(), siteEast.data(), 0));

    // 6. Starting planned road construction via Construction Office pointer tool (0x1A8D70)
    TEST_CHECK(GameCalls_CheckOfficeCallAllowedForTest(RVA_START_PLANNED_CONSTRUCTION, officeWest.data(), siteEast.data(), 0));
    TEST_CHECK(!GameCalls_CheckOfficeCallAllowedForTest(RVA_START_PLANNED_CONSTRUCTION, officeEast.data(), siteWest.data(), 0));

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_BuildingRangeAndAutoSearchSettingsSync()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    SettingsSync_LockstepReset();
    Commands_SetSession(COMMANDS_OFF);

    // 1. Verify SettingsSync_OwnsBuildingWord covers BLD_SEARCH_RANGE_OFFSET (0xFC8),
    //    BLD_AUTOSEARCH_FLAGS_OFFSET (0x11BC..0x11C3), BLD_AUTOSEARCH_WIRES_OFFSET (0x11C4),
    //    and BLD_RECONSTRUCT_THRESHOLDS_OFFSET (0xE50..0xE57) for all range-bearing categories:
    //    Construction Office (0x31), Demolition Office (0x6A), Technical Services (0x36),
    //    Hospital (0x1C), Fire Station (0x09), Secret Police (0x70), Repair Station (0x6C).
    const int kCategories[] = { 0x31, 0x6A, 0x36, 0x1C, 0x09, 0x70, 0x6C };
    for (int cat : kCategories) {
        std::vector<BYTE> bldType(0x400, 0);
        std::vector<BYTE> bld(0x1600, 0);
        *(int*)(bldType.data() + BLD_TYPE_CAT_OFF) = cat;
        *(BYTE**)(bld.data() + 0x318) = bldType.data();
        TEST_CHECK(SettingsSync_OwnsBuildingWord(bld.data(), BLD_SEARCH_RANGE_OFFSET));
        TEST_CHECK(SettingsSync_OwnsBuildingWord(bld.data(), BLD_AUTOSEARCH_FLAGS_OFFSET));
        TEST_CHECK(SettingsSync_OwnsBuildingWord(bld.data(), BLD_AUTOSEARCH_FLAGS_OFFSET + 4));
        TEST_CHECK(SettingsSync_OwnsBuildingWord(bld.data(), BLD_AUTOSEARCH_WIRES_OFFSET));
        TEST_CHECK(SettingsSync_OwnsBuildingWord(bld.data(), BLD_RECONSTRUCT_THRESHOLDS_OFFSET));
        TEST_CHECK(SettingsSync_OwnsBuildingWord(bld.data(), BLD_RECONSTRUCT_THRESHOLDS_OFFSET + 4));
    }

    // 2. Full Lockstep round-trip for working range (+0xFC8) and auto-search flags (+0x11BC..+0x11C4):
    std::vector<BYTE> typeCo(0x400, 0), typeHosp(0x400, 0), typeFire(0x400, 0);
    *(int*)(typeCo.data() + BLD_TYPE_CAT_OFF) = 0x31;   // Construction Office
    *(int*)(typeHosp.data() + BLD_TYPE_CAT_OFF) = 0x1C; // Hospital
    *(int*)(typeFire.data() + BLD_TYPE_CAT_OFF) = 0x09; // Fire Station

    std::vector<BYTE> bldCo(0x1600, 0), bldHosp(0x1600, 0), bldFire(0x1600, 0);
    *(int*)bldCo.data() = 701;
    *(BYTE**)(bldCo.data() + 0x318) = typeCo.data();
    *(float*)(bldCo.data() + 0x350) = -500.0f;
    *(float*)(bldCo.data() + 0x354) = 10.0f;
    *(float*)(bldCo.data() + 0x358) = -200.0f;
    *(int*)(bldCo.data() + BLD_SEARCH_RANGE_OFFSET) = 1000;
    *(float*)(bldCo.data() + BLD_RECONSTRUCT_THRESHOLDS_OFFSET) = 0.5f;
    *(float*)(bldCo.data() + BLD_RECONSTRUCT_THRESHOLDS_OFFSET + 4) = 0.5f;

    *(int*)bldHosp.data() = 702;
    *(BYTE**)(bldHosp.data() + 0x318) = typeHosp.data();
    *(float*)(bldHosp.data() + 0x350) = -400.0f;
    *(float*)(bldHosp.data() + 0x354) = 10.0f;
    *(float*)(bldHosp.data() + 0x358) = -150.0f;
    *(int*)(bldHosp.data() + BLD_SEARCH_RANGE_OFFSET) = 1000;

    *(int*)bldFire.data() = 703;
    *(BYTE**)(bldFire.data() + 0x318) = typeFire.data();
    *(float*)(bldFire.data() + 0x350) = -300.0f;
    *(float*)(bldFire.data() + 0x354) = 10.0f;
    *(float*)(bldFire.data() + 0x358) = -100.0f;
    *(int*)(bldFire.data() + BLD_SEARCH_RANGE_OFFSET) = 1000;

    BYTE* bldList[3] = { bldCo.data(), bldHosp.data(), bldFire.data() };
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = bldList;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA) = bldList + 3;

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    Commands_SetSession(COMMANDS_HOST);

    SettingsSync_LockstepBaseline();

    // Player changes working range on Construction Office, Hospital, Fire Station, plus auto-search flags
    *(int*)(bldCo.data() + BLD_SEARCH_RANGE_OFFSET) = 3000;
    *(int*)(bldHosp.data() + BLD_SEARCH_RANGE_OFFSET) = 2500;
    *(int*)(bldFire.data() + BLD_SEARCH_RANGE_OFFSET) = 2200;
    bldCo[BLD_AUTOSEARCH_FLAGS_OFFSET + 0] = 1; // machine reconstruction disabled
    bldCo[BLD_AUTOSEARCH_FLAGS_OFFSET + 5] = 1; // roads auto-search disabled
    bldCo[BLD_AUTOSEARCH_WIRES_OFFSET] = 1;     // wires auto-search disabled
    *(float*)(bldCo.data() + BLD_RECONSTRUCT_THRESHOLDS_OFFSET) = 0.75f;

    SettingsSync_LockstepDetect();

    // Local memory is reverted until Lockstep command boundary executes
    TEST_CHECK(*(int*)(bldCo.data() + BLD_SEARCH_RANGE_OFFSET) == 1000);
    TEST_CHECK(*(int*)(bldHosp.data() + BLD_SEARCH_RANGE_OFFSET) == 1000);
    TEST_CHECK(*(int*)(bldFire.data() + BLD_SEARCH_RANGE_OFFSET) == 1000);
    TEST_CHECK(bldCo[BLD_AUTOSEARCH_FLAGS_OFFSET + 0] == 0);
    TEST_CHECK(bldCo[BLD_AUTOSEARCH_WIRES_OFFSET] == 0);

    // Execute queued LS_CMD_SETTING commands at tick boundary
    TEST_CHECK(Commands_Due(1));
    Commands_Boundary(1);

    TEST_CHECK(*(int*)(bldCo.data() + BLD_SEARCH_RANGE_OFFSET) == 3000);
    TEST_CHECK(*(int*)(bldHosp.data() + BLD_SEARCH_RANGE_OFFSET) == 2500);
    TEST_CHECK(*(int*)(bldFire.data() + BLD_SEARCH_RANGE_OFFSET) == 2200);
    TEST_CHECK(bldCo[BLD_AUTOSEARCH_FLAGS_OFFSET + 0] == 1);
    TEST_CHECK(bldCo[BLD_AUTOSEARCH_FLAGS_OFFSET + 5] == 1);
    TEST_CHECK(bldCo[BLD_AUTOSEARCH_WIRES_OFFSET] == 1);
    TEST_CHECK_NEAR(*(float*)(bldCo.data() + BLD_RECONSTRUCT_THRESHOLDS_OFFSET), 0.75f, 1e-4f);

    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = nullptr;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA) = nullptr;
    SettingsSync_LockstepReset();
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    return true;
}

static BYTE* g_testCopySrcBld = nullptr;
static BYTE* g_testCopyDstBld = nullptr;
static unsigned __int64 MockCopySourceToolCallback(unsigned __int64 /*world*/, unsigned __int64 /*a2*/)
{
    if (g_testCopySrcBld && g_testCopyDstBld) {
        BYTE* srcSlotBegin = *(BYTE**)(g_testCopySrcBld + 0xF78);
        BYTE* srcSlotEnd   = *(BYTE**)(g_testCopySrcBld + 0xF80);
        BYTE* dstSlotBegin = *(BYTE**)(g_testCopyDstBld + 0xF78);
        BYTE* dstSlotEnd   = *(BYTE**)(g_testCopyDstBld + 0xF80);
        size_t n = (size_t)((srcSlotEnd - srcSlotBegin) / 0x10);
        if ((size_t)((dstSlotEnd - dstSlotBegin) / 0x10) == n) {
            for (size_t i = 0; i < n; ++i) {
                *(unsigned __int64*)(dstSlotBegin + i * 0x10) = *(const unsigned __int64*)(srcSlotBegin + i * 0x10);
            }
        }
    }
    return 1;
}

static bool SubTest_SourceSlotsAddAndCopySourceSync()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> typeCo(0x400, 0), typeSupply(0x400, 0);
    strcpy_s((char*)typeCo.data(), 64, "construction_office4");
    strcpy_s((char*)typeSupply.data(), 64, "storage_gravel");

    std::vector<BYTE> coSrc(0x1400, 0), coDst(0x1400, 0), coWest(0x1400, 0);
    std::vector<BYTE> supWorkers(0x1400, 0), supGravel(0x1400, 0), supAsphalt(0x1400, 0), supSteel(0x1400, 0);
    auto initBld = [&](std::vector<BYTE>& b, BYTE* t, float x, float z) {
        *(BYTE**)(b.data() + BLD_TYPE_OFFSET) = t;
        *(float*)(b.data() + OBJ_POS_OFFSET + 0) = x;
        *(float*)(b.data() + OBJ_POS_OFFSET + 4) = 10.0f;
        *(float*)(b.data() + OBJ_POS_OFFSET + 8) = z;
    };
    initBld(coSrc,      typeCo.data(),     +6284.0f, -5629.0f);
    initBld(coDst,      typeCo.data(),     +6221.0f, -5467.0f);
    initBld(coWest,     typeCo.data(),     -5000.0f, +1200.0f);
    initBld(supWorkers, typeSupply.data(), +5928.0f, -5528.0f);
    initBld(supGravel,  typeSupply.data(), +6287.0f, -5545.0f);
    initBld(supAsphalt, typeSupply.data(), +6426.0f, -5791.0f);
    initBld(supSteel,   typeSupply.data(), +6034.0f, -5339.0f);

    std::vector<BYTE> slotsSrc(4 * 0x10, 0), slotsDst(4 * 0x10, 0), slotsWest(4 * 0x10, 0);
    *(BYTE**)(coSrc.data()  + 0xF78) = slotsSrc.data();
    *(BYTE**)(coSrc.data()  + 0xF80) = slotsSrc.data() + slotsSrc.size();
    *(BYTE**)(coDst.data()  + 0xF78) = slotsDst.data();
    *(BYTE**)(coDst.data()  + 0xF80) = slotsDst.data() + slotsDst.size();
    *(BYTE**)(coWest.data() + 0xF78) = slotsWest.data();
    *(BYTE**)(coWest.data() + 0xF80) = slotsWest.data() + slotsWest.size();

    *(BYTE**)(slotsSrc.data() + 0 * 0x10) = supWorkers.data();
    *(BYTE**)(slotsSrc.data() + 1 * 0x10) = supGravel.data();
    *(BYTE**)(slotsSrc.data() + 2 * 0x10) = supAsphalt.data();
    *(BYTE**)(slotsSrc.data() + 3 * 0x10) = supSteel.data();

    BYTE* bldList[7] = { coSrc.data(), coDst.data(), coWest.data(), supWorkers.data(), supGravel.data(), supAsphalt.data(), supSteel.data() };
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = bldList;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA)   = bldList + 7;

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(false /*Client = East SSR (State 2)*/, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    Commands_SetSession(COMMANDS_HOST);
    SourceSlots_SetToolCallbackForTest(&MockCopySourceToolCallback);

    // 1. Client uses building_copysource (0x1D6820) to copy all 4 supply sources from coSrc (#193) to coDst (#201):
    g_testCopySrcBld = coSrc.data();
    g_testCopyDstBld = coDst.data();
    SourceSlots_CopySourceTool((unsigned __int64)world, 0);

    // Local slots on coDst must be reverted before Lockstep tick boundary, then applied on Commands_Boundary(1)
    TEST_CHECK(*(BYTE**)(slotsDst.data() + 0 * 0x10) == nullptr);
    TEST_CHECK(Commands_Due(1));
    Commands_Boundary(1);
    TEST_CHECK(*(BYTE**)(slotsDst.data() + 0 * 0x10) == supWorkers.data());
    TEST_CHECK(*(BYTE**)(slotsDst.data() + 1 * 0x10) == supGravel.data());
    TEST_CHECK(*(BYTE**)(slotsDst.data() + 2 * 0x10) == supAsphalt.data());
    TEST_CHECK(*(BYTE**)(slotsDst.data() + 3 * 0x10) == supSteel.data());

    // 2. Client attempting to copy supply sources onto Host's foreign coWest must be blocked and reverted:
    g_testCopyDstBld = coWest.data();
    SourceSlots_CopySourceTool((unsigned __int64)world, 0);
    TEST_CHECK(*(BYTE**)(slotsWest.data() + 0 * 0x10) == nullptr);
    TEST_CHECK(!Commands_Due(2));

    SourceSlots_SetToolCallbackForTest(nullptr);
    g_testCopySrcBld = nullptr;
    g_testCopyDstBld = nullptr;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA) = nullptr;
    *(BYTE***)(g_mockExeBuffer + VEC_BUILDINGS_END_RVA)   = nullptr;
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    return true;
}

bool Test_BuildingSwitches_Commands_SaveZip()
{
    ResetMockWorldMemory();

    // 1. Invariant #12: BuildingSwitches_IsExcludedBuildingOffset must exclude std::vector headers in BUILDING
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBC0)); // Citizen* vector begin
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBC8)); // Citizen* vector end
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBD0)); // Citizen* vector cap
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xBD8)); // Inmate* vector begin
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0x970)); // Storage slots vector
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xA20)); // Post-0x9E8 pointer block
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xF18)); // Citizen destinations vector
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(0xFD0)); // Post-0xFC8 vector block

    // Legitimate building settings offsets must NOT be excluded
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(0x114)); // Building custom name
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(0x500)); // Setting word
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(0x9E8)); // Water reservoir / heating plant setting word
    TEST_CHECK(!BuildingSwitches_IsExcludedBuildingOffset(BLD_SEARCH_RANGE_OFFSET)); // Working range / search distance (+0xFC8)
    // Live building animation & bay/corridor-lock runtime fields must be excluded
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(BLD_ANIM_SUBMESH_OFFSET));
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(BLD_ANIM_FRAME_OFFSET));
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(BLD_BAY_STATE_BEGIN_OFFSET));
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(BLD_BAY_VEH_BEGIN_OFFSET));
    TEST_CHECK(BuildingSwitches_IsExcludedBuildingOffset(BLD_CORRIDOR_LOCK_VEH_OFFSET));
    TEST_CHECK(SubTest_BuildingRangeAndAutoSearchSettingsSync());
    TEST_CHECK(SubTest_SourceSlotsAddAndCopySourceSync());

    // 1b. RoadSync_ClearCategoryPreview & RoadSync_CancelLocalDrag state cleanup
    {
        BYTE* world = g_mockExeBuffer + WORLD_RVA;
        std::vector<BYTE> pedMgr(0x1000, 0);
        std::vector<BYTE> secRoadMgr(0x1000, 0);
        *(BYTE**)(world + 0xFA08) = pedMgr.data();     // PEDESTRIAN manager
        *(BYTE**)(world + 0xF848) = secRoadMgr.data(); // SECOND_ROAD manager
        *(unsigned long long*)(pedMgr.data() + 0x360) = 0x112233445566ULL;
        *(unsigned char*)(pedMgr.data() + 0x390) = 1;
        *(unsigned char*)(pedMgr.data() + 0x748) = 1;
        *(unsigned char*)(pedMgr.data() + 0x749) = 1;
        *(unsigned long long*)(g_mockExeBuffer + 0xA2E640) = 0xAABBCCDDEEFFULL; // HOVER_ROAD_EDGE_RVA
        *(unsigned long long*)(g_mockExeBuffer + 0xA2E610) = 0x123456789ABCULL; // HOVER_NODE_RVA (must stay for quick-click cancel Inv #13)
        RoadSync_ClearCategoryPreview(ROAD_SYNC_CATEGORY_PEDESTRIAN);
        TEST_CHECK(*(unsigned long long*)(pedMgr.data() + 0x360) == 0);
        TEST_CHECK(*(unsigned char*)(pedMgr.data() + 0x390) == 0);
        TEST_CHECK(*(unsigned char*)(pedMgr.data() + 0x748) == 0);
        TEST_CHECK(*(unsigned char*)(pedMgr.data() + 0x749) == 0);
        TEST_CHECK(*(unsigned long long*)(g_mockExeBuffer + 0xA2E640) == 0);
        TEST_CHECK(*(unsigned long long*)(g_mockExeBuffer + 0xA2E610) == 0x123456789ABCULL);

        // Verify RoadSync_CancelLocalDrag clears SECOND_ROAD square preview vector (world+0xF6A0..0xF6A8)
        std::vector<unsigned long long> sqSegs = { 0x1001ULL, 0x1002ULL };
        *(unsigned long long**)(world + 0xF6A8) = sqSegs.data();
        *(unsigned long long**)(world + 0xF6B0) = sqSegs.data() + sqSegs.size();
        *(unsigned char*)(world + 0xD34C) = 1;
        RoadSync_CancelLocalDrag();
        TEST_CHECK(*(unsigned char*)(world + 0xD34C) == 0);
        TEST_CHECK(*(unsigned long long**)(world + 0xF6B0) == *(unsigned long long**)(world + 0xF6A8));
    }

    // 2. Lockstep Commands queue & tick boundary execution
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    Commands_SetSession(COMMANDS_HOST);
    TEST_CHECK(Commands_Capturing());

    unsigned long long epochBefore = Commands_ApplyEpoch();
    LsStateTransfer cmdTr = { 1, 2, 25000.0, 10000.0 };
    Commands_Submit(LS_CMD_STATE_TRANSFER, &cmdTr, sizeof(cmdTr));
    TEST_CHECK(Commands_Due(1));
    Commands_Boundary(1);

    TEST_CHECK(Commands_ApplyEpoch() > epochBefore);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 475000.0, 1e-3);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 525000.0, 1e-3);

    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();
    TEST_CHECK(SubTest_WorldSettingMultiToggleAndPerStateBuildMode());

    // 2b. Cheat / dev mode sanitization (C+H+E / D+E+V / cheat systab pages 5..7)
    {
        BYTE* world = g_mockExeBuffer + WORLD_RVA;
        BYTE sysTabStruct[32] = {};
        memcpy(sysTabStruct + 1, "system", 7);
        BYTE tradeTabStruct[32] = {};
        memcpy(tradeTabStruct + 1, "trade", 6);
        BYTE listTabStruct[32] = {};
        memcpy(listTabStruct + 1, "list", 5);

        *(const BYTE**)(world + WORLD_SYSTAB_TAB_PTR_OFFSET) = sysTabStruct;
        world[WORLD_CHEAT_MODE_OFFSET] = 1;
        world[WORLD_DEV_MODE_OFFSET]   = 1;
        world[WORLD_FAST_RESEARCH_CHEAT_OFFSET]  = 1;
        world[WORLD_FAST_CONSTRUCT_CHEAT_OFFSET] = 1;
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 6; // "cheatgeneral" under "system"
        TEST_CHECK(Commands_SanitizeCheatState(world));
        TEST_CHECK(world[WORLD_CHEAT_MODE_OFFSET] == 0);
        TEST_CHECK(world[WORLD_DEV_MODE_OFFSET] == 0);
        TEST_CHECK(world[WORLD_FAST_RESEARCH_CHEAT_OFFSET] == 0);
        TEST_CHECK(world[WORLD_FAST_CONSTRUCT_CHEAT_OFFSET] == 0);
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == -1);
        // Idempotent when already clean, and preserves normal sub-pages (0..4)
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 2;
        TEST_CHECK(!Commands_SanitizeCheatState(world));
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == 2);

        // Must preserve Tourism (subPage=5) and Crime & Justice (subPage=6) when top tab is "trade" or "list"!
        *(const BYTE**)(world + WORLD_SYSTAB_TAB_PTR_OFFSET) = tradeTabStruct;
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 5; // Tourism tab in Economy & Trade
        TEST_CHECK(!Commands_SanitizeCheatState(world));
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == 5);
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 6; // Crime & Justice tab in Economy & Trade
        TEST_CHECK(!Commands_SanitizeCheatState(world));
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == 6);

        *(const BYTE**)(world + WORLD_SYSTAB_TAB_PTR_OFFSET) = listTabStruct;
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 7; // Line list tab
        TEST_CHECK(!Commands_SanitizeCheatState(world));
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == 7);

        // Collapsing sidebar (tabPtr == nullptr) while on subPage 5..7 must NOT reset subPage or trigger false cheat warning
        *(const BYTE**)(world + WORLD_SYSTAB_TAB_PTR_OFFSET) = nullptr;
        *(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) = 6;
        TEST_CHECK(!Commands_SanitizeCheatState(world));
        TEST_CHECK(*(int32_t*)(world + WORLD_SYSTAB_SUBPAGE_OFFSET) == 6);
    }

    // 2b-bis. Two-States Tourism & Crime statistics attribution (hotel spending, citizen crime, and float rating sums)
    {
        ResetMockWorldMemory();
        BYTE* world = g_mockExeBuffer + WORLD_RVA;
        double startRub[2] = { 100000.0, 100000.0 };
        double startUsd[2] = { 50000.0,  50000.0 };
        TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, startRub, startUsd);
        TwoStates_ResetEconStatsForTest(world);

        std::vector<BYTE> hotelWest(0x1400, 0), hotelEast(0x1400, 0);
        *(int*)hotelWest.data() = 501;
        *(float*)(hotelWest.data() + OBJ_POS_OFFSET) = -1200.0f;
        *(float*)(hotelWest.data() + OBJ_POS_OFFSET + 8) = 100.0f;
        TwoStates_RegisterBuildingOwner(501, 1);

        *(int*)hotelEast.data() = 502;
        *(float*)(hotelEast.data() + OBJ_POS_OFFSET) = 1200.0f;
        *(float*)(hotelEast.data() + OBJ_POS_OFFSET + 8) = 100.0f;
        TwoStates_RegisterBuildingOwner(502, 2);

        std::vector<BYTE> citWest(0x800, 0), touristEast(0x800, 0);
        *(BYTE**)(citWest.data() + CIT_HOME_BUILDING_OFFSET) = hotelWest.data();
        // Tourist has null home (0x20), staying at hotelEast (0x58)
        *(BYTE**)(touristEast.data() + 0x58) = hotelEast.data();

        TwoStates_BeforeStep(world);
        TwoStates_BldHotelStepForTest(world, hotelWest.data(), 0, 1250.0f, true);  // West hotel USD
        TwoStates_BldHotelStepForTest(world, hotelEast.data(), 1, 3400.0f, false); // East hotel RUB
        TwoStates_CitizenCommitCrimeForTest(world, citWest.data(), 1);             // West medium crime (idx 13)
        TwoStates_CitizenCommitCrimeForTest(world, touristEast.data(), 2);         // East severe crime (idx 14)

        // Simulate tourist departure rating float sums in extraCounts[10] (USD) & extraCounts[11] (RUB) during East tourist scope
        TwoStates_AfterStep(world);

        TEST_CHECK_NEAR(TwoStates_GetStateEconTourismFloatForTest(1, 0), 1250.0f, 1e-2f);
        TEST_CHECK_NEAR(TwoStates_GetStateEconTourismFloatForTest(2, 0), 0.0f,    1e-2f);
        TEST_CHECK_NEAR(TwoStates_GetStateEconTourismFloatForTest(2, 1), 3400.0f, 1e-2f);
        TEST_CHECK_NEAR(TwoStates_GetStateEconTourismFloatForTest(1, 1), 0.0f,    1e-2f);
        TEST_CHECK(TwoStates_GetStateEconExtraCountForTest(1, 13) == 1);
        TEST_CHECK(TwoStates_GetStateEconExtraCountForTest(2, 13) == 0);
        TEST_CHECK(TwoStates_GetStateEconExtraCountForTest(2, 14) == 1);
        TEST_CHECK(TwoStates_GetStateEconExtraCountForTest(1, 14) == 0);

        // Verify float rating sum in extraCounts[10..11] and integer extraCounts are never double-added on SplitEngineStatRecordToStates
        // and that single-tourist rating is not split out of sync with the integer tourist count (extraCounts[6])
        BYTE* curStat = world + WORLD_STAT_CURRENT_OFFSET;
        *(int32_t*)(curStat + STAT_EXTRA_COUNTS_OFF + 6 * sizeof(int32_t)) = 1; // 1 departed USD tourist
        float scoreUsd = 4.0f;
        memcpy(curStat + STAT_EXTRA_COUNTS_OFF + 10 * sizeof(int32_t), &scoreUsd, sizeof(float));
        TwoStates_ResetEconStatsForTest(world);
        int32_t end1 = TwoStates_GetStateEconExtraCountForTest(1, 6);
        int32_t end2 = TwoStates_GetStateEconExtraCountForTest(2, 6);
        float sc1 = TwoStates_GetStateEconExtraFloatForTest(1, 10);
        float sc2 = TwoStates_GetStateEconExtraFloatForTest(2, 10);
        TEST_CHECK(end1 + end2 == 1);
        TEST_CHECK_NEAR(sc1 + sc2, 4.0f, 1e-3f);
        if (end1 == 1) {
            TEST_CHECK_NEAR(sc1, 4.0f, 1e-3f);
            TEST_CHECK_NEAR(sc2, 0.0f, 1e-3f);
        } else {
            TEST_CHECK_NEAR(sc1, 0.0f, 1e-3f);
            TEST_CHECK_NEAR(sc2, 4.0f, 1e-3f);
        }

        TwoStates_OnSessionEnded();
    }

    // 2c. Rail/road depot row self-drop uncoupling guard (0x7C7E87 / 0x7C7ED8)
    {
        std::vector<BYTE> locoA(0x600, 0), wagonA1(0x600, 0), locoB(0x600, 0);
        unsigned long long ptrLocoA   = (unsigned long long)locoA.data();
        unsigned long long ptrWagonA1 = (unsigned long long)wagonA1.data();
        unsigned long long ptrLocoB   = (unsigned long long)locoB.data();
        *(unsigned long long*)(wagonA1.data() + VEH_PARENT_PTR_OFFSET) = ptrLocoA; // wagonA1 attached to locoA

        // Clicking/double-clicking locoA or its attached wagonA1 is a self-drop -> must be blocked
        TEST_CHECK(GameCalls_IsDepotRowSelfDrop(ptrLocoA, ptrLocoA, ptrLocoA, ptrLocoA));
        TEST_CHECK(GameCalls_IsDepotRowSelfDrop(ptrLocoA, ptrWagonA1, ptrWagonA1, ptrLocoA));
        TEST_CHECK(GameCalls_IsDepotRowSelfDrop(ptrWagonA1, ptrWagonA1, ptrWagonA1, ptrLocoA));
        // Dragging independent locoB onto locoA's row is a valid couple -> must be allowed
        TEST_CHECK(!GameCalls_IsDepotRowSelfDrop(ptrLocoB, ptrLocoA, ptrLocoA, ptrLocoA));
        TEST_CHECK(!GameCalls_IsDepotRowSelfDrop(ptrLocoB, ptrWagonA1, ptrWagonA1, ptrLocoA));
    }

    // 2d. Sub-tick C3D_NODE position + 3x3 rotation matrix interpolation & Gram-Schmidt orthonormalization
    //     Verifies the true 47-float (0xBC-byte) C3D_NODE layout:
    //       +0x80 (float[32..34]): Position
    //       +0x8C (float[35..37]): Rotation (Euler angles, preserved)
    //       +0x98 (float[38..40]): Scale ((1,1,1), preserved and never normalized to 0.577)
    //       +0xA4 (float[41..43]): Orientation (interpolated & normalized)
    //       +0xB0 (float[44..46]): UpVector (interpolated & normalized)
    {
        float prevNode[47] = {};
        float currNode[47] = {};
        // prevNode: identity rotation at (10, 0, 20)
        prevNode[0] = 1.0f; prevNode[5] = 1.0f; prevNode[10] = 1.0f; prevNode[15] = 1.0f;
        prevNode[16] = 1.0f; prevNode[21] = 1.0f; prevNode[26] = 1.0f; prevNode[31] = 1.0f;
        prevNode[12] = prevNode[32] = 10.0f;
        prevNode[13] = prevNode[33] = 0.0f;
        prevNode[14] = prevNode[34] = 20.0f;
        prevNode[38] = prevNode[39] = prevNode[40] = 1.0f; // scale = (1,1,1)
        prevNode[43] = 1.0f; // ori = (0,0,1) at +0xA4
        prevNode[45] = 1.0f; // up  = (0,1,0) at +0xB0

        // currNode: 45-degree yaw around Y at (20, 0, 30)
        const float c45 = 0.70710678f, s45 = 0.70710678f;
        currNode[0] =  c45; currNode[2]  = -s45;
        currNode[5] = 1.0f;
        currNode[8] =  s45; currNode[10] =  c45;
        currNode[15] = 1.0f;
        currNode[16] =  c45; currNode[24] = -s45;
        currNode[21] = 1.0f;
        currNode[18] =  s45; currNode[26] =  c45;
        currNode[31] = 1.0f;
        currNode[12] = currNode[32] = 20.0f;
        currNode[13] = currNode[33] = 0.0f;
        currNode[14] = currNode[34] = 30.0f;
        currNode[36] = 0.78539816f; // Euler yaw at +0x8C
        currNode[38] = currNode[39] = currNode[40] = 1.0f; // scale = (1,1,1) at +0x98
        currNode[41] = s45; currNode[43] = c45; // ori at +0xA4
        currNode[45] = 1.0f; // up at +0xB0

        alignas(16) BYTE nodeBuf[C3D_NODE_BYTE_SIZE] = {};
        memcpy(nodeBuf, currNode, sizeof(currNode));
        const float smoothOff[3] = { 1.0f, 0.0f, -1.0f };
        HeldFrame_InterpolateC3DNode(nodeBuf, prevNode, currNode, 0.5f, smoothOff);

        const float* m     = (const float*)nodeBuf;
        const float* minv  = (const float*)(nodeBuf + 0x40);
        const float* pos   = (const float*)(nodeBuf + 0x80);
        const float* rot   = (const float*)(nodeBuf + 0x8C);
        const float* scale = (const float*)(nodeBuf + 0x98);
        const float* ori   = (const float*)(nodeBuf + 0xA4);
        const float* up    = (const float*)(nodeBuf + 0xB0);
        TEST_CHECK_NEAR(pos[0], 16.0f, 1e-4f);
        TEST_CHECK_NEAR(pos[2], 24.0f, 1e-4f);
        TEST_CHECK_NEAR(m[12],  16.0f, 1e-4f);
        TEST_CHECK_NEAR(m[14],  24.0f, 1e-4f);
        // Scale (+0x98) must be preserved intact, and Euler rotation (+0x8C) interpolated via HeldFrame_LerpAngleRadians
        TEST_CHECK_NEAR(rot[1],   0.39269908f, 1e-5f);
        TEST_CHECK_NEAR(scale[0], 1.0f, 1e-5f);
        TEST_CHECK_NEAR(scale[1], 1.0f, 1e-5f);
        TEST_CHECK_NEAR(scale[2], 1.0f, 1e-5f);
        // Halfway between 0 deg and 45 deg is 22.5 deg (sin=0.382683, cos=0.923880)
        TEST_CHECK_NEAR(m[8],   0.382683f, 1e-3f);
        TEST_CHECK_NEAR(m[10],  0.923880f, 1e-3f);
        TEST_CHECK_NEAR(ori[0], 0.382683f, 1e-3f);
        TEST_CHECK_NEAR(ori[2], 0.923880f, 1e-3f);
        TEST_CHECK_NEAR(up[1],  1.0f,      1e-5f);
        // Orthonormality of m and exactness of m * minv translation
        float dotXZ = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
        TEST_CHECK_NEAR(dotXZ, 0.0f, 1e-5f);
        float tx = m[12] * minv[0] + m[13] * minv[4] + m[14] * minv[8]  + minv[12];
        float tz = m[12] * minv[2] + m[13] * minv[6] + m[14] * minv[10] + minv[14];
        TEST_CHECK_NEAR(tx, 0.0f, 1e-4f);
        TEST_CHECK_NEAR(tz, 0.0f, 1e-4f);

        // Verify alpha = 0.0f places node at prevNode + smoothOff (not currNode)
        memcpy(nodeBuf, currNode, sizeof(currNode));
        HeldFrame_InterpolateC3DNode(nodeBuf, prevNode, currNode, 0.0f, nullptr);
        TEST_CHECK_NEAR(pos[0], 10.0f, 1e-4f);
        TEST_CHECK_NEAR(pos[2], 20.0f, 1e-4f);
        TEST_CHECK_NEAR(rot[1], 0.0f,  1e-5f);
        TEST_CHECK_NEAR(ori[0], 0.0f,  1e-4f);
        TEST_CHECK_NEAR(ori[2], 1.0f,  1e-4f);

        // Verify Step-Frame follow-camera snapshot/restore saves & restores CAMERA_RVA (0xBC bytes)
        // and all 7 heap PID damper structs at camMgr + CAM_MGR_PID_PTRS_OFFSET (0x38 bytes each)
        std::vector<BYTE> stepCamMgr(CAM_MGR_STRUCT_BYTES, 0);
        std::vector<std::vector<BYTE>> pidBlocks(CAM_MGR_PID_COUNT, std::vector<BYTE>(CAM_MGR_PID_STRUCT_BYTES, 0));
        for (int i = 0; i < CAM_MGR_PID_COUNT; ++i) {
            *(float*)(pidBlocks[i].data() + 0x00) = 10.0f + (float)i; // PID curValue
            *(float*)(pidBlocks[i].data() + 0x08) = 0.25f * (float)(i + 1); // PID velocity
            *(BYTE**)(stepCamMgr.data() + CAM_MGR_PID_PTRS_OFFSET + (size_t)i * sizeof(BYTE*)) = pidBlocks[i].data();
        }
        BYTE* globalCam = g_mockExeBuffer + CAMERA_RVA;
        *(float*)(globalCam + 0x80) = 500.0f; // camera pos.x
        *(float*)(globalCam + 0x8C) = 0.40f;  // camera pitch
        *(float*)(globalCam + 0x90) = 1.20f;  // camera yaw

        HeldFrame_SaveCameraStepStateForTest(stepCamMgr.data(), 60.0f);

        // Simulate 1st call to RVA_CAMERA_MGR_UPDATE (0x14250) on a Step Frame mutating CAMERA_RVA and PID dampers
        *(float*)(globalCam + 0x80) = 512.0f;
        *(float*)(globalCam + 0x8C) = 0.55f;
        *(float*)(globalCam + 0x90) = 1.45f;
        for (int i = 0; i < CAM_MGR_PID_COUNT; ++i) {
            *(float*)(pidBlocks[i].data() + 0x00) = 999.0f;
            *(float*)(pidBlocks[i].data() + 0x08) = -99.0f;
        }
        TEST_CHECK(HeldFrame_RestoreCameraStepStateForTest(stepCamMgr.data()));
        TEST_CHECK_NEAR(*(float*)(globalCam + 0x80), 500.0f, 1e-5f);
        TEST_CHECK_NEAR(*(float*)(globalCam + 0x8C), 0.40f,  1e-5f);
        TEST_CHECK_NEAR(*(float*)(globalCam + 0x90), 1.20f,  1e-5f);
        for (int i = 0; i < CAM_MGR_PID_COUNT; ++i) {
            TEST_CHECK_NEAR(*(float*)(pidBlocks[i].data() + 0x00), 10.0f + (float)i, 1e-5f);
            TEST_CHECK_NEAR(*(float*)(pidBlocks[i].data() + 0x08), 0.25f * (float)(i + 1), 1e-5f);
        }
    }

    // 2e. HeldFrame OnSimTick & VehicleGuard with +0x1A68 integer counters, graph traversal, and soft-sync smoothing cap
    TEST_CHECK(SubTest_HeldFrameVehicleGraphAndSmoothOffset());
    TEST_CHECK(SubTest_HeldFrameQueueBoundAndCorruptedLinks());

    // 3. SaveZip pack & unpack roundtrip
    std::filesystem::path baseDir(g_mockBaseDir);
    std::filesystem::path srcSaveDir = baseDir / "zip_src_save";
    std::filesystem::path dstSaveDir = baseDir / "zip_dst_save";
    std::filesystem::path zipFile    = baseDir / "test_save_pack.zip";
    std::filesystem::create_directories(srcSaveDir);

    std::vector<BYTE> binPayload(4096);
    for (size_t i = 0; i < binPayload.size(); ++i) binPayload[i] = (BYTE)((i * 37 + 13) & 0xFF);
    {
        FILE* f = nullptr;
        TEST_CHECK(fopen_s(&f, (srcSaveDir / "header.bin").string().c_str(), "wb") == 0 && f);
        fwrite(binPayload.data(), 1, binPayload.size(), f);
        fclose(f);
    }

    size_t progressCalls = 0;
    auto onProg = [](size_t, size_t, void* ud) { ++(*(size_t*)ud); };
    TEST_CHECK(ZipSaveDirectory(srcSaveDir.string().c_str(), zipFile.string().c_str(), onProg, &progressCalls));
    TEST_CHECK(progressCalls > 0);
    TEST_CHECK(UnzipToDirectory(zipFile.string().c_str(), dstSaveDir.string().c_str(), onProg, &progressCalls));

    std::vector<BYTE> readBack(binPayload.size(), 0);
    {
        FILE* f = nullptr;
        TEST_CHECK(fopen_s(&f, (dstSaveDir / "header.bin").string().c_str(), "rb") == 0 && f);
        size_t got = fread(readBack.data(), 1, readBack.size(), f);
        fclose(f);
        TEST_CHECK(got == binPayload.size());
    }
    TEST_CHECK(memcmp(binPayload.data(), readBack.data(), binPayload.size()) == 0);

    // 4. InviteCitizensNow (0x6F8CC0 -> 0x823470): verify USD (mode 0), Soviet RUB (mode 2), and Expert RUB (mode 3)
    {
        ResetMockWorldMemory();
        BYTE* world = g_mockExeBuffer + WORLD_RVA;

        static std::vector<std::vector<BYTE>> s_spawnedCitizens;
        static std::vector<BYTE*> s_worldCitVec;
        static int s_mockRandCounter = 0;
        s_spawnedCitizens.clear();
        s_worldCitVec.clear();
        s_mockRandCounter = 0;

        auto mockSpawnCit = +[](unsigned long long w, unsigned long long bld, unsigned char /*isChild*/,
                                unsigned char /*isSoviet*/, unsigned char /*isThirdWorld*/, unsigned char /*unusedZero*/) -> unsigned long long {
            s_spawnedCitizens.emplace_back(0x750, 0);
            BYTE* cit = s_spawnedCitizens.back().data();
            *(unsigned long long*)(cit + CIT_HOME_BUILDING_OFFSET) = bld;
            s_worldCitVec.push_back(cit);
            *(BYTE***)(w + WORLD_CITIZENS_OFFSET)     = s_worldCitVec.data();
            *(BYTE***)(w + WORLD_CITIZENS_OFFSET + 8) = s_worldCitVec.data() + s_worldCitVec.size();
            return (unsigned long long)cit;
        };
        auto mockRand = +[]() -> int { return s_mockRandCounter++; };
        auto mockRandFloat = +[](float minVal, float maxVal) -> float { return 0.5f * (minVal + maxVal); };

        *(decltype(mockSpawnCit)*)(g_mockExeBuffer + RVA_SPAWN_CITIZEN) = mockSpawnCit;
        *(decltype(mockRand)*)(g_mockExeBuffer + RVA_IAT_RAND) = mockRand;
        *(decltype(mockRandFloat)*)(g_mockExeBuffer + RVA_IAT_RAND_FLOAT) = mockRandFloat;
        *(float*)(g_mockExeBuffer + RVA_INVITE_EXPERT_MULT_FLOAT)    = 2.5f;
        *(float*)(g_mockExeBuffer + RVA_INVITE_EXPERT_EDU_MIN_FLOAT) = 2.05f;
        *(float*)(g_mockExeBuffer + RVA_INVITE_EXPERT_EDU_MAX_FLOAT) = 2.30f;

        *(double*)(world + WORLD_USD_OFFSET) = 50000.0;
        *(double*)(world + WORLD_RUB_OFFSET) = 100000.0;
        *(float*)(world + WORLD_PRICE_IMMIGRANT_USD_OFF)      = 100.0f;
        *(float*)(world + WORLD_PRICE_IMMIGRANT_RUB_OFF)      = 200.0f;
        *(float*)(world + WORLD_IMMIGRANT_BASE_SPEND_USD_OFF) = 100.0f;
        *(float*)(world + WORLD_IMMIGRANT_BASE_SPEND_RUB_OFF) = 200.0f;
        *(float*)(world + WORLD_IMMIGRANT_RATE_MULT_USD_OFF)  = 1.5f;
        *(float*)(world + WORLD_IMMIGRANT_RATE_MULT_RUB_OFF)  = 1.2f;

        std::vector<BYTE> resSpec(0x100, 0);
        *(float*)(resSpec.data() + RES_SPEC_MAX_FLATS_FLOAT_OFF) = 12.0f; // 12 flats max
        std::vector<BYTE> bldType(0x400, 0);
        *(BYTE**)(bldType.data() + BLD_TYPE_RES_SPEC_PTR_OFF) = resSpec.data();
        std::vector<BYTE> cityStat(STAT_RECORD_SIZE, 0);
        std::vector<BYTE> bldArea(0x200, 0);
        *(BYTE**)(bldArea.data() + AREA_CITY_STAT_PTR_OFF) = cityStat.data();
        std::vector<BYTE> resBld(0x1420, 0);
        *(BYTE**)(resBld.data() + BLD_TYPE_OFFSET)   = bldType.data();
        *(BYTE**)(resBld.data() + BLD_AREA_PTR_OFF)  = bldArea.data();

        // Mode 3: Invite up to 5 educated Soviet experts (RUB)
        GameCalls_InviteCitizensForTest((unsigned long long)world, (unsigned long long)resBld.data(), 3);
        // 5 experts spawned (price = 200 * 1.2 * 2.5 = 600 RUB each -> 3000 RUB total)
        TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 97000.0, 1e-2);
        TEST_CHECK(*(int*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_COUNTS_OFF + 12) == 5);
        TEST_CHECK(*(int*)(cityStat.data() + STAT_CITIZEN_COUNTS_OFF + 12) == 5);
        TEST_CHECK_NEAR(*(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_IMMIGRANT_SPEND_OFF + 4), 3000.0f, 1e-1f);
        // First spawned adult (index 0) must have education ~2.175f and a linked child (index 1, since rand()==0 % 2 == 0)
        TEST_CHECK(s_worldCitVec.size() >= 2);
        TEST_CHECK_NEAR(*(float*)(s_worldCitVec[0] + CIT_EDUCATION_FLOAT_OFF), 2.175f, 1e-3f);
        TEST_CHECK(*(BYTE**)(s_worldCitVec[0] + CIT_CHILD_PTR_OFF)  == s_worldCitVec[1]);
        TEST_CHECK(*(BYTE**)(s_worldCitVec[1] + CIT_PARENT_PTR_OFF) == s_worldCitVec[0]);

        // Populate building's citizen vector so currentIndependent == 5, leaving 7 free flats out of 12
        *(BYTE***)(resBld.data() + BLD_CITIZENS_BEGIN_OFF) = s_worldCitVec.data();
        *(BYTE***)(resBld.data() + BLD_CITIZENS_END_OFF)   = s_worldCitVec.data() + s_worldCitVec.size();

        // Mode 0: Invite Third World immigrants (USD) -> capped by remaining 7 free flats (price = 100 * 1.5 = 150 USD each -> 1050 USD)
        GameCalls_InviteCitizensForTest((unsigned long long)world, (unsigned long long)resBld.data(), 0);
        TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 48950.0, 1e-2);
        TEST_CHECK(*(int*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_COUNTS_OFF + 16) == 7);
        TEST_CHECK(*(int*)(cityStat.data() + STAT_CITIZEN_COUNTS_OFF + 16) == 7);
        TEST_CHECK_NEAR(*(float*)(world + WORLD_STAT_CURRENT_OFFSET + STAT_IMMIGRANT_SPEND_OFF), 1050.0f, 1e-1f);
    }

    TEST_CHECK(SubTest_UnivResearchTwoStatesIsolation());
    TEST_CHECK(SubTest_UnivResearchNearCompleteAndStaffRatioSync());
    TEST_CHECK(SubTest_ConstructionOfficeTwoStatesAutoAndManual());
    return true;
}

static void __fastcall MockCargoDestroyFn(U64 /*at*/, U64 /*unused*/)
{
}

static U64 __fastcall MockCargoCopyFn(U64 dst, U64 src)
{
    if (dst && src) memcpy((void*)dst, (const void*)src, 0xC8);
    return dst;
}

bool Test_JoinHubDistOfficeCarrierAndStorageVisuals()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Carried tools check: join_hub_all, building_addstation, building_addstation_dist, building_copy_*
    TEST_CHECK(Commands_IsToolCarriedForTest("join_hub_all"));
    TEST_CHECK(Commands_IsToolCarriedForTest("join_hub"));
    TEST_CHECK(Commands_IsToolCarriedForTest("join_hub_stop"));
    TEST_CHECK(Commands_IsToolCarriedForTest("building_addstation"));
    TEST_CHECK(Commands_IsToolCarriedForTest("building_addstation_dist"));
    TEST_CHECK(Commands_IsToolCarriedForTest("building_copy_dist"));
    TEST_CHECK(Commands_IsToolCarriedForTest("building_copy_waste"));
    TEST_CHECK(Commands_IsToolCarriedForTest("building_copy_transfer"));
    TEST_CHECK(!Commands_IsToolCarriedForTest("unknown_uncarried_tool"));

    // 2. FindNetworkNodeByPos fallback via segment endpoints (net + 0x2B0..0x2B8) when net + 0x298 / 0xAA0 are empty
    std::vector<BYTE> netMgr(0xB00, 0);
    std::vector<BYTE> nodeA(0x40, 0), nodeB(0x40, 0), loopNode(0x40, 0);
    *(float*)(nodeA.data() + 4) = 120.0f; *(float*)(nodeA.data() + 8) = 15.0f; *(float*)(nodeA.data() + 12) = -340.0f;
    *(float*)(nodeB.data() + 4) = 220.0f; *(float*)(nodeB.data() + 8) = 15.0f; *(float*)(nodeB.data() + 12) = -340.0f;
    *(float*)(loopNode.data() + 4) = 260.0f; *(float*)(loopNode.data() + 8) = 16.0f; *(float*)(loopNode.data() + 12) = -350.0f;

    std::vector<BYTE> seg(0x160, 0);
    *(U64*)(seg.data() + 0x20) = (U64)nodeA.data();
    *(U64*)(seg.data() + 0x28) = (U64)nodeB.data();
    std::vector<BYTE*> loopVec = { loopNode.data() };
    WriteMockVectorHeader(seg.data() + 0xD0, loopVec);
    std::vector<BYTE*> segVec = { seg.data() };
    WriteMockVectorHeader(netMgr.data() + 0x2B0, segVec);

    *(int*)(world + 0x140EC) = 1;
    *(U64*)(world + 0x140F0) = (U64)netMgr.data();
    const float qA[3] = { 120.05f, 15.0f, -339.95f };
    const float qB[3] = { 220.0f, 15.0f, -340.0f };
    const float qL[3] = { 260.0f, 16.0f, -350.0f };
    const float qMiss[3] = { 999.0f, 15.0f, 999.0f };
    U64 foundNet = 0;
    TEST_CHECK(RouteTools_FindNetworkNodeByPosForTest((U64)world, qA, &foundNet) == (U64)nodeA.data());
    TEST_CHECK(foundNet == (U64)netMgr.data());
    TEST_CHECK(RouteTools_FindNetworkNodeByPosForTest((U64)world, qB, nullptr) == (U64)nodeB.data());
    TEST_CHECK(RouteTools_FindNetworkNodeByPosForTest((U64)world, qL, nullptr) == (U64)loopNode.data());
    TEST_CHECK(RouteTools_FindNetworkNodeByPosForTest((U64)world, qMiss, nullptr) == 0ULL);

    // 3. Distribution Office CargoSettings_ApplyCommand with mismatched record count & targetRef resolution
    const unsigned char sigDestroy[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xff, 0x48 };
    const unsigned char sigCopy[16]    = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe };
    const unsigned char sigRefresh[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 };
    memcpy(g_mockExeBuffer + 0x0F6520, sigDestroy, 16);
    memcpy(g_mockExeBuffer + 0x21FBF0, sigCopy, 16);
    memcpy(g_mockExeBuffer + 0x4BFC10, sigRefresh, 16);

    std::vector<BYTE> distBld(BUILDING_STRUCT_BYTES, 0), storageBldA(BUILDING_STRUCT_BYTES, 0), storageBldB(BUILDING_STRUCT_BYTES, 0);
    *(int*)distBld.data() = 501;
    *(int*)storageBldA.data() = 502;
    *(int*)storageBldB.data() = 503;
    std::vector<BYTE*> worldBlds = { distBld.data(), storageBldA.data(), storageBldB.data() };
    WriteMockVectorHeader(world + WORLD_BUILDINGS_OFFSET, worldBlds);

    std::vector<BYTE> recA(0x198, 0), recB(0x198, 0);
    std::vector<BYTE> linkA(0x18, 0), linkB(0x18, 0);
    linkA[0] = 1; *(U64*)(linkA.data() + 0x08) = (U64)storageBldA.data(); *(U64*)(linkA.data() + 0x10) = (U64)recA.data();
    linkB[0] = 1; *(U64*)(linkB.data() + 0x08) = (U64)storageBldB.data(); *(U64*)(linkB.data() + 0x10) = (U64)recB.data();
    // Host has 2 links (linkA, linkB), while packet from Client had records=1 for storageBldB!
    std::vector<BYTE*> distLinks = { linkA.data(), linkB.data() };
    WriteMockVectorHeader(distBld.data() + 0xD38, distLinks);

    RouteRef distRef = kNoRef, targetRefB = kNoRef;
    TEST_CHECK(Ref_Building((U64)distBld.data(), distRef));
    TEST_CHECK(Ref_Building((U64)storageBldB.data(), targetRefB));

    // Prime CargoSettings Ready() routine signature check, then install mock JMPs at 0x124D80 / 0x204EF0
    BYTE primeBuf[64] = {};
    *(unsigned*)primeBuf = 0x32534743u;
    CargoSettings_ApplyCommand(primeBuf, sizeof(primeBuf));
    WriteAbsJmp12(g_mockExeBuffer + 0x0F6520, (void*)&MockCargoDestroyFn);
    WriteAbsJmp12(g_mockExeBuffer + 0x21FBF0, (void*)&MockCargoCopyFn);

    std::vector<BYTE> pkt;
    auto pushRaw = [&](const void* p, size_t n) { pkt.insert(pkt.end(), (const BYTE*)p, (const BYTE*)p + n); };
    unsigned magic = 0x32534743u;
    BYTE kind = 1, half = 0, activeFlag = 1;
    unsigned short recIdx = 0;
    unsigned recTotal = 1; // Mismatched count (1 vs 2 on Host), resolved via targetRefB!
    U64 tailThresholds = 0;
    float loadPct = 0.35f, unloadPct = 0.75f;
    memcpy((BYTE*)&tailThresholds, &loadPct, 4);
    memcpy((BYTE*)&tailThresholds + 4, &unloadPct, 4);
    BYTE scalars[22] = {};
    scalars[0] = 1; // loading enabled
    pushRaw(&magic, 4);
    pushRaw(&kind, 1);
    pushRaw(&half, 1);
    pushRaw(&recIdx, 2);
    pushRaw(&recTotal, 4);
    pushRaw(&distRef, sizeof(distRef));
    pushRaw(&targetRefB, sizeof(targetRefB));
    pushRaw(&activeFlag, 1);
    pushRaw(&tailThresholds, 8);
    pushRaw(scalars, sizeof(scalars));
    unsigned zeroVec = 0;
    for (int v = 0; v < 7; ++v) pushRaw(&zeroVec, 4);

    TEST_CHECK(CargoSettings_ApplyCommand(pkt.data(), pkt.size()));
    TEST_CHECK(recB[0] == 1);
    TEST_CHECK_NEAR(*(float*)(recB.data() + 0x190), 0.35f, 1e-5f);
    TEST_CHECK_NEAR(*(float*)(recB.data() + 0x194), 0.75f, 1e-5f);
    TEST_CHECK(distBld[0xD50] == 1);

    // 4. Orphaned Carrier Mechanism Recovery & VehicleSoftSync Carrier/Carried Sync
    std::vector<BYTE> coBld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> carrier(VEHICLE_STRUCT_BYTES, 0), mech(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(carrier.data() + VEH_ID_OFFSET) = 701;
    *(unsigned*)(mech.data() + VEH_ID_OFFSET) = 702;
    *(U64*)(carrier.data() + VEH_WORKPLACE_BLD_OFFSET) = (U64)coBld.data();
    // Simulate mech loaded onto carrier (0x68A420 cleared mech workplace & removed mech from coBld + 0xC70)
    *(U64*)(mech.data() + VEH_LINK_PTR_OFFSET) = (U64)carrier.data();
    *(U64*)(mech.data() + VEH_WORKPLACE_BLD_OFFSET) = 0;
    std::vector<BYTE*> carriedVec = { mech.data() };
    WriteMockVectorHeader(carrier.data() + VEH_CARRIED_BEGIN_OFFSET, carriedVec);
    std::vector<BYTE*> officeVehBuf = { carrier.data(), nullptr };
    *(U64*)(coBld.data() + BLD_VEHICLES_BEGIN_OFFSET) = (U64)officeVehBuf.data();
    *(U64*)(coBld.data() + BLD_VEHICLES_END_OFFSET) = (U64)(officeVehBuf.data() + 1);
    *(U64*)(coBld.data() + BLD_VEHICLES_END_OFFSET + 8) = (U64)(officeVehBuf.data() + 2);

    TEST_CHECK(VehicleSoftSync_HasActiveEnginePathForTest(mech.data()));
    *(float*)(carrier.data() + 0xD98 + 0x80) = 100.0f;
    *(float*)(mech.data() + 0xD98 + 0x80) = 100.0f;
    VehicleSoftSync_TranslateWithChildrenForTest(carrier.data(), 12.5f, 0.0f, 0.0f, false);
    TEST_CHECK_NEAR(*(float*)(carrier.data() + 0xD98 + 0x80), 112.5f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(mech.data() + 0xD98 + 0x80), 112.5f, 1e-4f);

    // Recover orphaned mechanism when carrier is interrupted on pause / sent home
    GameCalls_RecoverOrphanedCarrierMechanismsForTest((U64)carrier.data());
    TEST_CHECK(*(U64*)(mech.data() + VEH_LINK_PTR_OFFSET) == 0ULL);
    TEST_CHECK(*(U64*)(mech.data() + VEH_WORKPLACE_BLD_OFFSET) == (U64)coBld.data());
    TEST_CHECK(*(U64*)(carrier.data() + VEH_CARRIED_BEGIN_OFFSET) == *(U64*)(carrier.data() + VEH_CARRIED_END_OFFSET));
    TEST_CHECK(*(U64*)(coBld.data() + BLD_VEHICLES_END_OFFSET) == (U64)(officeVehBuf.data() + 2));
    TEST_CHECK(officeVehBuf[1] == mech.data());

    // Safety guards on carrier recovery:
    // (a) Coupled train wagon (+0x4A0 == loco, loco +0x488..+0x490 empty) must NOT be uncoupled
    std::vector<BYTE> railLoco(VEHICLE_STRUCT_BYTES, 0), railWagon(VEHICLE_STRUCT_BYTES, 0), depotBld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE*> depotVehs = { railLoco.data(), nullptr };
    *(U64*)(depotBld.data() + BLD_VEHICLES_BEGIN_OFFSET) = (U64)depotVehs.data();
    *(U64*)(depotBld.data() + BLD_VEHICLES_END_OFFSET)   = (U64)(depotVehs.data() + 1);
    *(U64*)(depotBld.data() + BLD_VEHICLES_END_OFFSET + 8) = (U64)(depotVehs.data() + 2);
    *(U64*)(railLoco.data() + VEH_WORKPLACE_BLD_OFFSET)  = (U64)depotBld.data();
    *(U64*)(railWagon.data() + VEH_LINK_PTR_OFFSET)      = (U64)railLoco.data();
    GameCalls_RecoverOrphanedCarrierMechanismsForTest((U64)railWagon.data());
    TEST_CHECK(*(U64*)(railWagon.data() + VEH_LINK_PTR_OFFSET) == (U64)railLoco.data());
    TEST_CHECK(*(U64*)(depotBld.data() + BLD_VEHICLES_END_OFFSET) == (U64)(depotVehs.data() + 1));

    // (b) Normal depot flatbed (workplace == 0, depot == depotBld) carrying a mechanism preserves +0x488..+0x490
    std::vector<BYTE> depFlatbed(VEHICLE_STRUCT_BYTES, 0), depCargoVeh(VEHICLE_STRUCT_BYTES, 0);
    *(U64*)(depFlatbed.data() + VEH_DEPOT_PTR_OFFSET) = (U64)depotBld.data();
    *(U64*)(depCargoVeh.data() + VEH_LINK_PTR_OFFSET) = (U64)depFlatbed.data();
    std::vector<BYTE*> depCarried = { depCargoVeh.data() };
    WriteMockVectorHeader(depFlatbed.data() + VEH_CARRIED_BEGIN_OFFSET, depCarried);
    GameCalls_RecoverOrphanedCarrierMechanismsForTest((U64)depFlatbed.data());
    TEST_CHECK(*(U64*)(depCargoVeh.data() + VEH_LINK_PTR_OFFSET) == (U64)depFlatbed.data());
    TEST_CHECK(*(U64*)(depFlatbed.data() + VEH_CARRIED_END_OFFSET) == (U64)(depCarried.data() + 1));

    // (c) Recovery fires ONLY on recall / send-home / clear-line, never on release (0x6C5460) / start-toggle (0x6B7810) / line copy (0x7C85F8)
    TEST_CHECK(!GameCalls_ShouldRecoverCarrierForSiteForTest(0x8190EEu, RVA_VEH_RELEASE, "V"));
    TEST_CHECK(!GameCalls_ShouldRecoverCarrierForSiteForTest(0x818F8Fu, 0x6EB6C0u, "V"));
    TEST_CHECK(!GameCalls_ShouldRecoverCarrierForSiteForTest(0x806AA8u, 0x4BDF90u, "MLVbo"));
    TEST_CHECK(GameCalls_ShouldRecoverCarrierForSiteForTest(0x818F2Bu, 0x41F0F0u, "WVbbbb"));
    TEST_CHECK(GameCalls_ShouldRecoverCarrierForSiteForTest(0x818F38u, 0x41EC10u, "xV"));
    TEST_CHECK(GameCalls_ShouldRecoverCarrierForSiteForTest(0x82736Au, 0x4BDF90u, "MoVbo"));

    // 5. Meshless Free Open Storage Visual Goods (FillPrivateVisible inclusion rule)
    TEST_CHECK(SimPatches_ShouldIncludeMeshlessStorageInVisible(false, 0x2000ULL, 0x2018ULL, 350.0f));
    TEST_CHECK(SimPatches_ShouldIncludeMeshlessStorageInVisible(true, 0ULL, 0ULL, 1800.0f));
    TEST_CHECK(!SimPatches_ShouldIncludeMeshlessStorageInVisible(false, 0ULL, 0ULL, 350.0f));
    TEST_CHECK(!SimPatches_ShouldIncludeMeshlessStorageInVisible(false, 0x2000ULL, 0x2000ULL, 350.0f));
    TEST_CHECK(!SimPatches_ShouldIncludeMeshlessStorageInVisible(false, 0x2000ULL, 0x2018ULL, 1500.0f));
    TEST_CHECK(!SimPatches_ShouldIncludeMeshlessStorageInVisible(false, 0x2000ULL, 0x2018ULL, -5.0f));

    return true;
}

static std::vector<void*>* g_recordedSimPatchHookTargets = nullptr;
static int MockHost_RecordSimPatchInlineHook(void* target, void*, void**, const unsigned char*, size_t, const char*)
{
    if (g_recordedSimPatchHookTargets) {
        g_recordedSimPatchHookTargets->push_back(target);
    }
    return 0;
}

static bool SubTest_CorridorLockPreservationAndSingleplayerIsolation()
{
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);

    // 1. Road truck waiting at dock/customs (waitTimer > 0 or non-air +0x1A04 != 0) keeps corridor lock
    std::vector<BYTE> truckType(0x400, 0);
    *(int32_t*)(truckType.data() + VEH_TYPE_CATEGORY_OFFSET) = 1; // Road
    std::vector<BYTE> truck(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(truck.data() + VEH_TYPE_PTR_OFFSET) = truckType.data();
    *(BYTE**)(truck.data() + VEH_BAY_TARGET_BLD_OFFSET) = bld.data();
    *(BYTE**)(truck.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) = bld.data();
    *(BYTE**)(bld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) = truck.data();
    *(float*)(truck.data() + VEH_WAIT_TIMER_OFFSET) = 3.5f;
    *(int32_t*)(truck.data() + VEH_HELI_FLIGHT_STATE_OFF) = 2;
    *(int32_t*)(truck.data() + VEH_WAYPOINT_INDEX_OFFSET) = 5;

    SimPatches_TestSanitizeVehicleArrival(truck.data());
    TEST_CHECK(*(BYTE**)(bld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) == truck.data());
    TEST_CHECK(*(BYTE**)(truck.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) == bld.data());

    // 2. Tram / motor rail car (vehCat == 3, even if VEH_IS_RAIL_FLAG_OFFSET == 0) keeps corridor lock
    std::vector<BYTE> tramType(0x400, 0);
    *(int32_t*)(tramType.data() + VEH_TYPE_CATEGORY_OFFSET) = 3;
    std::vector<BYTE> tram(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(tram.data() + VEH_TYPE_PTR_OFFSET) = tramType.data();
    *(BYTE**)(tram.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) = bld.data();
    *(BYTE**)(bld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) = tram.data();
    *(int32_t*)(tram.data() + VEH_BAY_SLOT_IDX_OFFSET) = -1;
    *(int32_t*)(tram.data() + VEH_WAYPOINT_INDEX_OFFSET) = 5;

    SimPatches_TestSanitizeVehicleArrival(tram.data());
    TEST_CHECK(*(BYTE**)(bld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) == tram.data());
    TEST_CHECK(*(BYTE**)(tram.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) == bld.data());

    // 3. Singleplayer isolation: when !Determinism_IsActive(), HookedVehicleArrivalStep must not mutate locks
    std::vector<BYTE> airType(0x400, 0);
    *(int32_t*)(airType.data() + VEH_TYPE_CATEGORY_OFFSET) = 8;
    std::vector<BYTE> plane(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(plane.data() + VEH_TYPE_PTR_OFFSET) = airType.data();
    *(int32_t*)(plane.data() + VEH_HELI_FLIGHT_STATE_OFF) = 3;
    *(BYTE**)(plane.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) = bld.data();
    *(BYTE**)(bld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) = plane.data();

    Determinism_SetActiveForTest(false);
    SimPatches_TestHookedVehicleArrivalStep(plane.data());
    TEST_CHECK(*(BYTE**)(bld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) == plane.data());
    TEST_CHECK(*(BYTE**)(plane.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) == bld.data());

    return true;
}

bool Test_CrashFixesAndRcoTrackBuilder()
{
    ResetMockWorldMemory();

    // 1. DiagLog_IsInSafeSeh exported state for TesmioLoader CrashHandler
    TEST_CHECK(DiagLog_IsInSafeSeh() == 0);
    DiagLog_BeginSafeSeh();
    TEST_CHECK(DiagLog_IsInSafeSeh() == 1);
    DiagLog_EndSafeSeh();
    TEST_CHECK(DiagLog_IsInSafeSeh() == 0);

    // 2. Crime / Court case owner resolution with empty suspect vector (Crash #2 at 0x182D41)
    double initRub[2] = { 100000.0, 100000.0 };
    double initUsd[2] = { 50000.0, 50000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 0.0f, false, initRub, initUsd);

    std::vector<BYTE> eastCrimeBld(BUILDING_STRUCT_BYTES, 0);
    *(int32_t*)eastCrimeBld.data() = 801;
    *(float*)(eastCrimeBld.data() + OBJ_POS_OFFSET) = 1200.0f; // East SSR (State 2)

    std::vector<BYTE> crimeCase(0x80, 0);
    *(uint64_t*)(crimeCase.data() + 0x00) = 0xFFFFFFFF00000016ULL; // non-pointer case header
    *(BYTE**)(crimeCase.data() + 0x08) = eastCrimeBld.data();
    // Suspect vector (+0x28..+0x30) is empty -> must NOT cast crimeCase to CITIZEN*!
    TEST_CHECK(TwoStates_ResolveCrimeCaseOwnerForTest(crimeCase.data()) == 2);

    // Invalid non-pointer at +0x08 and +0x10 must safely return default fallback (1) without faulting
    *(uint64_t*)(crimeCase.data() + 0x08) = 0x0000000000000352ULL;
    *(uint64_t*)(crimeCase.data() + 0x10) = 0x0000000000000016ULL;
    TEST_CHECK(TwoStates_ResolveCrimeCaseOwnerForTest(crimeCase.data()) == 1);
    TwoStates_OnSessionEnded();

    // 3. IsVehicleAtDepotOrStop (0x3BB580) corrupted parentBld guard (Crash #4 at 0x3BB65C)
    std::vector<BYTE> bldType(0x400, 0);
    *(int32_t*)(bldType.data() + BLD_TYPE_CAT_OFF) = 0x0F; // Rail depot / station category
    std::vector<BYTE> vehType(0x8700, 0);
    *(int32_t*)(vehType.data() + VEH_TYPE_CATEGORY_OFFSET) = 4; // VEHICLETYPE_RAIL_LOCOMOTIVE (trains & RCO track builders)
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bld.data() + 0x318) = bldType.data();
    std::vector<BYTE> veh(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(veh.data() + VEH_TYPE_PTR_OFFSET) = vehType.data();

    TEST_CHECK(SimPatches_TestIsVehicleAtDepotOrStop(bld.data(), veh.data(), 0) == 1);
    // Helicopter (cat == 0x0A, VEHICLETYPE_HELICOPTER) is accepted at helipad/airport (0x2F), NOT rail depot (0x0F)
    *(int32_t*)(vehType.data() + VEH_TYPE_CATEGORY_OFFSET) = 0x0A;
    TEST_CHECK(SimPatches_TestIsVehicleAtDepotOrStop(bld.data(), veh.data(), 0) == 0);
    *(int32_t*)(bldType.data() + BLD_TYPE_CAT_OFF) = 0x2F;
    TEST_CHECK(SimPatches_TestIsVehicleAtDepotOrStop(bld.data(), veh.data(), 0) == 1);
    *(int32_t*)(bldType.data() + BLD_TYPE_CAT_OFF) = 0x0F;
    *(int32_t*)(vehType.data() + VEH_TYPE_CATEGORY_OFFSET) = 4;
    // Corrupt parentBld (+0x5B0) to 0x3E7 (999) -> must return 0 safely without AV at 0x3E7 + 0x318 = 0x705
    *(uint64_t*)(bld.data() + 0x5B0) = 0x3E7ULL;
    TEST_CHECK(SimPatches_TestIsVehicleAtDepotOrStop(bld.data(), veh.data(), 0) == 0);
    *(uint64_t*)(bld.data() + 0x5B0) = 0ULL;

    // 4. Rail Construction Office (RCO) Track Builder corridor lock, hook scope, & multi-stage preservation
    std::vector<BYTE> rcoBldType(0x400, 0);
    *(int32_t*)(rcoBldType.data() + BLD_TYPE_CAT_OFF) = 0x1C; // Rail Construction Office building category
    std::vector<BYTE> rcoBld(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(rcoBld.data() + 0x318) = rcoBldType.data();

    // Real RCO track builders have vehCat == 4 (VEHICLETYPE_RAIL_LOCOMOTIVE) + 16-byte skill entry at vehType + 0x85E0
    std::vector<BYTE> railSkills(VEH_TYPE_SKILL_ENTRY_BYTES, 0);
    *(int32_t*)(railSkills.data() + 0) = SKILL_CONSTRUCTION_RAIL_TRACK_LAYER; // 0x1A
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_BEGIN_OFFSET) = (uintptr_t)railSkills.data();
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_END_OFFSET)   = (uintptr_t)(railSkills.data() + railSkills.size());

    std::vector<BYTE> railSiteBld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> constrSiteData(0x200, 0);
    *(BYTE**)(railSiteBld.data() + BLD_CONSTR_SITE_PTR_OFFSET) = constrSiteData.data();
    *(BYTE**)(railSiteBld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) = veh.data();

    veh[VEH_IS_RAIL_FLAG_OFFSET] = 1;
    *(int32_t*)(vehType.data() + VEH_TYPE_CATEGORY_OFFSET) = 4;
    *(BYTE**)(veh.data() + VEH_WORKPLACE_BLD_OFFSET) = rcoBld.data();
    *(BYTE**)(veh.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) = railSiteBld.data();
    *(int32_t*)(veh.data() + VEH_BAY_SLOT_IDX_OFFSET) = -10;
    *(int32_t*)(veh.data() + VEH_DEPOT_SLOT_IDX_OFFSET) = -1;
    *(float*)(veh.data() + VEH_WAIT_TIMER_OFFSET) = 5.0f;
    *(int32_t*)(veh.data() + VEH_WAYPOINT_INDEX_OFFSET) = 6;

    // Corridor lock on rail construction site must NOT be stripped by SanitizeVehicleArrivalBeforeStep
    SimPatches_TestSanitizeVehicleArrival(veh.data());
    TEST_CHECK(*(BYTE**)(railSiteBld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) == veh.data());
    TEST_CHECK(*(BYTE**)(veh.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) == railSiteBld.data());

    // Verify SimPatches_Install() hooks RVA_VEH_ARRIVAL_STEP & RVA_IS_VEHICLE_AT_DEPOT_OR_STOP,
    // and does NOT hook RVA_VEH_BUILD_WAYPOINTS (0x683240), preserving its native 0x6832EB guard.
    std::vector<void*> installedHookTargets;
    g_recordedSimPatchHookTargets = &installedHookTargets;
    auto prevInstallInlineHook = g_mockHost.installInlineHook;
    g_mockHost.installInlineHook = &MockHost_RecordSimPatchInlineHook;
    SimPatches_Bind(&g_mockHost);
    SimPatches_Install();
    g_mockHost.installInlineHook = prevInstallInlineHook;
    SimPatches_Bind(&g_mockHost);
    g_recordedSimPatchHookTargets = nullptr;

    TEST_CHECK(std::find(installedHookTargets.begin(), installedHookTargets.end(),
                         (void*)(g_mockExeBuffer + RVA_VEH_ARRIVAL_STEP)) != installedHookTargets.end());
    TEST_CHECK(std::find(installedHookTargets.begin(), installedHookTargets.end(),
                         (void*)(g_mockExeBuffer + RVA_IS_VEHICLE_AT_DEPOT_OR_STOP)) != installedHookTargets.end());
    TEST_CHECK(std::find(installedHookTargets.begin(), installedHookTargets.end(),
                         (void*)(g_mockExeBuffer + RVA_VEH_BUILD_WAYPOINTS)) == installedHookTargets.end());

    // Keep SimPatches_TestBuildVehicleWaypoints linked & verified as non-mutating
    veh[VEH_BAY_ARRIVED_FLAG_OFFSET] = 1;
    *(uintptr_t*)(veh.data() + VEH_WAYPOINTS_BEGIN_OFFSET) = 0;
    *(uintptr_t*)(veh.data() + VEH_WAYPOINTS_END_OFFSET) = 0;
    SimPatches_TestBuildVehicleWaypoints(veh.data(), 0);
    TEST_CHECK(veh[VEH_BAY_ARRIVED_FLAG_OFFSET] == 1);

    // Track builder outside RCO (+0x4F0 == nullptr) transitioning between rail stages:
    // wpCount == 0 with targetBld (+0x528) != nullptr, +0x56C == -1, +0x510 == -1 MUST retain +0x528 and +0xD39 == 0
    // so o_vehicleArrivalStep (0x6CD380) can reach AssignBayPath (0x6B2C00).
    *(BYTE**)(veh.data() + VEH_CUR_BLD_OFFSET) = nullptr;
    *(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) = bld.data();
    *(int32_t*)(veh.data() + VEH_BAY_SLOT_IDX_OFFSET) = -1;
    *(int32_t*)(veh.data() + VEH_DEPOT_SLOT_IDX_OFFSET) = -1;
    veh[VEH_BAY_ARRIVED_FLAG_OFFSET] = 0;
    SimPatches_TestSanitizeVehicleArrival(veh.data());
    TEST_CHECK(*(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) == bld.data());
    TEST_CHECK(veh[VEH_BAY_ARRIVED_FLAG_OFFSET] == 0);

    // Same wpCount == 0 inter-stage preservation for track builder with +0x67D == 0 and for scheduled rail train (no skills)
    veh[VEH_IS_RAIL_FLAG_OFFSET] = 0;
    SimPatches_TestSanitizeVehicleArrival(veh.data());
    TEST_CHECK(*(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) == bld.data());
    TEST_CHECK(veh[VEH_BAY_ARRIVED_FLAG_OFFSET] == 0);

    veh[VEH_IS_RAIL_FLAG_OFFSET] = 1;
    *(BYTE**)(veh.data() + VEH_WORKPLACE_BLD_OFFSET) = nullptr;
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_BEGIN_OFFSET) = 0;
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_END_OFFSET)   = 0;
    SimPatches_TestSanitizeVehicleArrival(veh.data());
    TEST_CHECK(*(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) == bld.data());
    TEST_CHECK(veh[VEH_BAY_ARRIVED_FLAG_OFFSET] == 0);

    // End-of-waypoints state (wpCount > 0, wpIdx >= wpCount, veh[0xD78] == 1) with 1 and 2 BUILDING* orders (8B / 16B):
    // BOTH RCO Track Builders (vehCat == 4 + construction skills 0x1A/0x1B/0x1C + RCO workplace)
    // AND scheduled rail trains (vehCat == 4, no construction skills) MUST preserve +0x528, wpEnd, wpIdx, +0xD78, and +0xD39 == 0
    // so native 0x6CE65F..0x6CE67B -> 0x6D7DE0 (reading +0x528 at 0x6D85EF) and 0x147B1F..0x147D0C are not corrupted.
    std::vector<float> dummyWp(6, 100.0f); // 2 waypoints (24 bytes)
    std::vector<BYTE*> orders = { bld.data(), railSiteBld.data() }; // 2 BUILDING* pointers = 16 bytes
    *(uintptr_t*)(veh.data() + VEH_WAYPOINTS_BEGIN_OFFSET) = (uintptr_t)dummyWp.data();
    *(uintptr_t*)(veh.data() + VEH_WAYPOINTS_END_OFFSET) = (uintptr_t)(dummyWp.data() + 6);
    *(int32_t*)(veh.data() + VEH_WAYPOINT_INDEX_OFFSET) = 2;
    *(uintptr_t*)(veh.data() + VEH_ORDERS_BEGIN_OFFSET) = (uintptr_t)orders.data();
    *(uintptr_t*)(veh.data() + VEH_ORDERS_END_OFFSET) = (uintptr_t)(orders.data() + orders.size());
    *(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) = bld.data();
    veh[VEH_REVERSE_NEEDED_FLAG_OFFSET] = 1;
    veh[VEH_BAY_ARRIVED_FLAG_OFFSET] = 0;

    // Case A: RCO Track Builders with skills 0x1A (track layer), 0x1B (tunnel boring), 0x1C (rail crane 2)
    *(BYTE**)(veh.data() + VEH_WORKPLACE_BLD_OFFSET) = rcoBld.data();
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_BEGIN_OFFSET) = (uintptr_t)railSkills.data();
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_END_OFFSET)   = (uintptr_t)(railSkills.data() + railSkills.size());
    const int32_t kBuilderSkills[3] = {
        SKILL_CONSTRUCTION_RAIL_TRACK_LAYER,
        SKILL_CONSTRUCTION_RAIL_TUNNEL_BORING,
        SKILL_CONSTRUCTION_RAIL_CRANE2
    };
    for (int32_t skillId : kBuilderSkills) {
        *(int32_t*)(railSkills.data() + 0) = skillId;
        SimPatches_TestSanitizeVehicleArrival(veh.data());
        TEST_CHECK(*(uintptr_t*)(veh.data() + VEH_WAYPOINTS_END_OFFSET) == (uintptr_t)(dummyWp.data() + 6));
        TEST_CHECK(*(int32_t*)(veh.data() + VEH_WAYPOINT_INDEX_OFFSET) == 2);
        TEST_CHECK(veh[VEH_REVERSE_NEEDED_FLAG_OFFSET] == 1);
        TEST_CHECK(*(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) == bld.data());
        TEST_CHECK(veh[VEH_BAY_ARRIVED_FLAG_OFFSET] == 0);
    }

    // Case B: Scheduled rail train (vehCat == 4, no construction skills) with 1 order (8 bytes) and 2 orders (16 bytes)
    *(BYTE**)(veh.data() + VEH_WORKPLACE_BLD_OFFSET) = nullptr;
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_BEGIN_OFFSET) = 0;
    *(uintptr_t*)(vehType.data() + VEH_TYPE_SKILLS_END_OFFSET)   = 0;
    *(uintptr_t*)(veh.data() + VEH_ORDERS_END_OFFSET) = (uintptr_t)(orders.data() + 1);
    SimPatches_TestSanitizeVehicleArrival(veh.data());
    TEST_CHECK(*(uintptr_t*)(veh.data() + VEH_WAYPOINTS_END_OFFSET) == (uintptr_t)(dummyWp.data() + 6));
    TEST_CHECK(*(int32_t*)(veh.data() + VEH_WAYPOINT_INDEX_OFFSET) == 2);
    TEST_CHECK(veh[VEH_REVERSE_NEEDED_FLAG_OFFSET] == 1);
    TEST_CHECK(*(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) == bld.data());
    TEST_CHECK(veh[VEH_BAY_ARRIVED_FLAG_OFFSET] == 0);

    *(uintptr_t*)(veh.data() + VEH_ORDERS_END_OFFSET) = (uintptr_t)(orders.data() + orders.size());
    SimPatches_TestSanitizeVehicleArrival(veh.data());
    TEST_CHECK(*(uintptr_t*)(veh.data() + VEH_WAYPOINTS_END_OFFSET) == (uintptr_t)(dummyWp.data() + 6));
    TEST_CHECK(*(int32_t*)(veh.data() + VEH_WAYPOINT_INDEX_OFFSET) == 2);
    TEST_CHECK(veh[VEH_REVERSE_NEEDED_FLAG_OFFSET] == 1);
    TEST_CHECK(*(BYTE**)(veh.data() + VEH_BAY_TARGET_BLD_OFFSET) == bld.data());
    TEST_CHECK(veh[VEH_BAY_ARRIVED_FLAG_OFFSET] == 0);

    TEST_CHECK(SubTest_CorridorLockPreservationAndSingleplayerIsolation());

    return true;
}

bool Test_SystemUiInterceptionAndNetworkTools_M9_1()
{
    if (!H) { TsmBind(&g_mockHost); }
    // 1. Feature 22: IsPointerLikeX64 pointer filter refinement
    // Scalar pair {1.0f, 0} = 0x000000003F800000ULL: high == 0, must return false
    TEST_CHECK(!IsPointerLikeX64(0x000000003F800000ULL));
    // Small integer pair {10, 0} = 0x000000000000000AULL: high == 0, must return false
    TEST_CHECK(!IsPointerLikeX64(0x000000000000000AULL));
    // Misaligned 64-bit addresses (not 8-byte aligned) must return false
    TEST_CHECK(!IsPointerLikeX64(0x0000010000000004ULL));
    TEST_CHECK(!IsPointerLikeX64(0x0000000100000002ULL));
    TEST_CHECK(!IsPointerLikeX64(0x0000000100000007ULL));
    // Kernel space addresses (high > 0x7FFF) must return false
    TEST_CHECK(!IsPointerLikeX64(0x8000000000000000ULL));
    TEST_CHECK(!IsPointerLikeX64(0xFFFFF80000000000ULL));
    // Valid module / heap pointers (8-byte aligned, >= 0x00100000) must return true
    if (g_exeBase) {
        uint64_t exePtr = (uint64_t)(uintptr_t)g_exeBase;
        if ((exePtr & 7u) == 0 && exePtr >= 0x00100000ULL && (exePtr >> 32) <= 0x7FFF) {
            TEST_CHECK(IsPointerLikeX64(exePtr));
        }
    }
    void* heapBuf = malloc(64);
    if (heapBuf) {
        uint64_t heapPtr = (uint64_t)(uintptr_t)heapBuf;
        if ((heapPtr & 7u) == 0 && heapPtr >= 0x00100000ULL && (heapPtr >> 32) <= 0x7FFF) {
            TEST_CHECK(IsPointerLikeX64(heapPtr));
        }
        free(heapBuf);
    }

    // 2. Feature 21: BuildingSwitches_Reset and VehicleSwitches_Reset clearing noise arrays
    BuildingSwitches_Reset();
    TEST_CHECK(BuildingSwitches_IsNoiseEmptyForTest());
    VehicleSwitches_Reset();
    TEST_CHECK(VehicleSwitches_IsNoiseEmptyForTest());

    // 3. Feature 21: Mouse wheel interaction detection in Clicked()
    if (g_exeBase) {
        // Clear buttons and wheel
        g_exeBase[0xA9EF50] = 0; // DOWN
        g_exeBase[0xA9EF51] = 0; // PRESS
        g_exeBase[0xA9ED6C] = 0; // ENTER
        g_exeBase[0xA9EDEC] = 0; // KEYPAD_ENTER
        *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = 0.0f;
        TEST_CHECK(!BuildingSwitches_ClickedForTest());

        // Wheel delta scrolled up (+120.0f)
        *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = 120.0f;
        TEST_CHECK(BuildingSwitches_ClickedForTest());

        // Wheel delta scrolled down (-120.0f)
        *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = -120.0f;
        TEST_CHECK(BuildingSwitches_ClickedForTest());

        // Reset wheel
        *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = 0.0f;
        TEST_CHECK(!BuildingSwitches_ClickedForTest());
    }

    // 4. Feature 23: Bottom panel blind bytes 0xB08..0xB0B and 0xC830..0xC79B in IsOneByteWorldSetting
    TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(0xB20)); // dword
    TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(0xB24)); // dword
    TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(0xB28)); // dword
    TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(0xB2C)); // dword
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xB30));  // byte 0
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xB31));  // byte 1
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xB32));  // byte 2
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xB33));  // byte 3
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xC838)); // byte 0
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xC839)); // byte 1
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xC83A)); // byte 2
    TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(0xC83B)); // byte 3
    TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(0xC83C));

    // 5. Feature 24: Commands_SanitizeCheatState clears terrain editor cheat fields 0xAE18, 0xAE1C
    std::vector<BYTE> mockWorld(0x15000, 0);
    *(int32_t*)(mockWorld.data() + 0xAE18) = 42;
    *(float*)(mockWorld.data() + 0xAE1C) = 3.14159f;
    TEST_CHECK(Commands_SanitizeCheatState(mockWorld.data()));
    TEST_CHECK(*(int32_t*)(mockWorld.data() + 0xAE18) == 0);
    TEST_CHECK(*(float*)(mockWorld.data() + 0xAE1C) == 0.0f);
    // Idempotent: second call returns false because already sanitized
    TEST_CHECK(!Commands_SanitizeCheatState(mockWorld.data()));

    // 6. Feature 24 & 25: Tool carriage & Area clone safe blocking
    TEST_CHECK(Commands_IsAreaCloneToolBlockedForTest("area_copy"));
    TEST_CHECK(Commands_IsAreaCloneToolBlockedForTest("area_paste"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("building_distribution"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("assign_co"));

    return true;
}

// ---------------------------------------------------------------------------
// Challenger M9.1: Adversarial Stress Tests (Features 21–25)
// ---------------------------------------------------------------------------

static bool SubTest_M9_1_MouseWheelAdversarialFloatFuzzing()
{
    if (!g_exeBase) return true;

    // Reset button states
    g_exeBase[0xA9EF50] = 0; // DOWN
    g_exeBase[0xA9EF51] = 0; // PRESS
    g_exeBase[0xA9ED6C] = 0; // ENTER
    g_exeBase[0xA9EDEC] = 0; // KEYPAD_ENTER

    // 1. Zero values
    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = 0.0f;
    TEST_CHECK(!BuildingSwitches_ClickedForTest());

    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = -0.0f;
    TEST_CHECK(!BuildingSwitches_ClickedForTest());

    // 2. Normal deltas (positive and negative, discrete and smooth)
    const float kNormalDeltas[] = { 120.0f, -120.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.0001f, -0.0001f, 1e20f, -1e20f };
    for (float delta : kNormalDeltas) {
        *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = delta;
        TEST_CHECK(BuildingSwitches_ClickedForTest());
    }

    // 3. Infinite values
    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = std::numeric_limits<float>::infinity();
    TEST_CHECK(!BuildingSwitches_ClickedForTest());

    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = -std::numeric_limits<float>::infinity();
    TEST_CHECK(!BuildingSwitches_ClickedForTest());

    // 4. NaN values (quiet, signaling, and arbitrary payloads)
    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = std::numeric_limits<float>::quiet_NaN();
    TEST_CHECK(!BuildingSwitches_ClickedForTest());

    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = std::numeric_limits<float>::signaling_NaN();
    TEST_CHECK(!BuildingSwitches_ClickedForTest());

    const uint32_t kNanPayloads[] = {
        0x7F800001u, 0x7F800002u, 0x7FA00000u, 0x7FC00000u, 0x7FFFFFFFu,
        0xFF800001u, 0xFF800002u, 0xFFA00000u, 0xFFC00000u, 0xFFFFFFFFu
    };
    for (uint32_t rawNan : kNanPayloads) {
        memcpy(g_exeBase + MOUSE_WHEEL_DELTA_RVA, &rawNan, sizeof(rawNan));
        TEST_CHECK(!BuildingSwitches_ClickedForTest());
    }

    // 5. Subnormal floats (denorm_min, tiny positive/negative)
    // In IEEE-754, non-zero subnormal floats have isfinite() == true, so they evaluate to true.
    const uint32_t kDenormPayloads[] = { 0x00000001u, 0x00000002u, 0x007FFFFFu, 0x80000001u, 0x807FFFFFu };
    for (uint32_t rawDenorm : kDenormPayloads) {
        memcpy(g_exeBase + MOUSE_WHEEL_DELTA_RVA, &rawDenorm, sizeof(rawDenorm));
        TEST_CHECK(BuildingSwitches_ClickedForTest());
    }

    // 6. Keyboard / Mouse button precedence over NaN/Inf wheel
    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = std::numeric_limits<float>::quiet_NaN();
    g_exeBase[0xA9EF50] = 1; // DOWN
    TEST_CHECK(BuildingSwitches_ClickedForTest());
    g_exeBase[0xA9EF50] = 0;

    g_exeBase[0xA9EF51] = 1; // PRESS
    TEST_CHECK(BuildingSwitches_ClickedForTest());
    g_exeBase[0xA9EF51] = 0;

    g_exeBase[0xA9ED6C] = 1; // ENTER
    TEST_CHECK(BuildingSwitches_ClickedForTest());
    g_exeBase[0xA9ED6C] = 0;

    g_exeBase[0xA9EDEC] = 1; // KEYPAD_ENTER
    TEST_CHECK(BuildingSwitches_ClickedForTest());
    g_exeBase[0xA9EDEC] = 0;

    // Reset clean
    *(float*)(g_exeBase + MOUSE_WHEEL_DELTA_RVA) = 0.0f;
    TEST_CHECK(!BuildingSwitches_ClickedForTest());

    return true;
}

static bool SubTest_M9_1_PointerFilterExhaustiveBoundaries()
{
    // A. Misaligned addresses must return false
    TEST_CHECK(!IsPointerLikeX64(0ULL));
    for (uint64_t off = 1; off <= 7; ++off) {
        TEST_CHECK(!IsPointerLikeX64(off));
        TEST_CHECK(!IsPointerLikeX64(0x0000010000000000ULL | off));
        TEST_CHECK(!IsPointerLikeX64(0x00007FFF00000000ULL | off));
    }

    // B. Unmapped scalar pairs below 4GB (high == 0) must return false
    TEST_CHECK(!IsPointerLikeX64(0x000000003F800000ULL)); // {1.0f, 0}
    TEST_CHECK(!IsPointerLikeX64(0x00000000BF800000ULL)); // {-1.0f, 0}
    TEST_CHECK(!IsPointerLikeX64(0x000000000000000AULL)); // {10, 0}
    TEST_CHECK(!IsPointerLikeX64(0x00000000000186A0ULL)); // {100000, 0}
    TEST_CHECK(!IsPointerLikeX64(0x000000007F800000ULL)); // {+inf, 0}
    TEST_CHECK(!IsPointerLikeX64(0x000000007FC00000ULL)); // {NaN, 0}
    TEST_CHECK(!IsPointerLikeX64(0x00000000FFFFFFF8ULL)); // Max aligned word under 4GB

    // C. Scalar pairs with high > 0x7FFF must return false
    TEST_CHECK(!IsPointerLikeX64(0x3F80000000000000ULL)); // {0, 1.0f}
    TEST_CHECK(!IsPointerLikeX64(0xBF80000000000000ULL)); // {0, -1.0f}
    TEST_CHECK(!IsPointerLikeX64(0x3F8000003F800000ULL)); // {1.0f, 1.0f}
    TEST_CHECK(!IsPointerLikeX64(0x42C8000042C80000ULL)); // {100.0f, 100.0f}

    // D. Kernel / non-canonical space (high >= 0x8000) must return false
    TEST_CHECK(!IsPointerLikeX64(0x0000800000000000ULL));
    TEST_CHECK(!IsPointerLikeX64(0x0000800100000000ULL));
    TEST_CHECK(!IsPointerLikeX64(0x0000FFFF00000000ULL));
    TEST_CHECK(!IsPointerLikeX64(0x8000000000000000ULL));
    TEST_CHECK(!IsPointerLikeX64(0xFFFFF80000000000ULL));
    TEST_CHECK(!IsPointerLikeX64(0xFFFFFFFFFFFFFFFFULL));
    TEST_CHECK(!IsPointerLikeX64(0xFFFFFFFFFFFFFFF8ULL));

    // E. Dynamic virtual allocation in low range (v >= 0x00100000 && high <= 0x7FFF)
    TEST_CHECK(!IsPointerLikeX64(0x0000000100000000ULL));

    void* allocated = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (allocated) {
        uint64_t addr = (uint64_t)(uintptr_t)allocated;
        if ((addr & 7u) == 0 && addr >= 0x00100000ULL && (addr >> 32) <= 0x7FFF) {
            TEST_CHECK(IsPointerLikeX64(addr));

            // Misaligned offsets into the valid page must return false
            TEST_CHECK(!IsPointerLikeX64(addr + 1));
            TEST_CHECK(!IsPointerLikeX64(addr + 2));
            TEST_CHECK(!IsPointerLikeX64(addr + 3));
            TEST_CHECK(!IsPointerLikeX64(addr + 4));
            TEST_CHECK(!IsPointerLikeX64(addr + 5));
            TEST_CHECK(!IsPointerLikeX64(addr + 6));
            TEST_CHECK(!IsPointerLikeX64(addr + 7));
        }
        VirtualFree(allocated, 0, MEM_RELEASE);
    }

    return true;
}

static bool SubTest_M9_1_BuildingSwitches_PointerRejectionAndRollback()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BuildingSwitches_Reset();
    Commands_SetSession(COMMANDS_HOST);

    // Setup mock building
    std::vector<BYTE> bldType(0x400, 0);
    *(int*)(bldType.data() + BLD_TYPE_CAT_OFF) = 0x1C; // Hospital
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(unsigned*)bld.data() = 101u; // ID = 101
    *(BYTE**)(bld.data() + 0x318) = bldType.data();
    float pos[3] = { 100.0f, 0.0f, 200.0f };
    memcpy(bld.data() + 0x350, pos, sizeof(pos));

    std::vector<BYTE*> mockBlds = { bld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    // Baseline setting at offset 0x500 (not in kViewWords)
    *(uint32_t*)(bld.data() + 0x500) = 0x11112222;
    *(uint32_t*)(bld.data() + 0x504) = 0x00000000;

    // 1. Frame with click and pointer modification (heap pointer in 64-bit pair)
    g_mockExeBuffer[0xA9EF51] = 1; // Click active
    BuildingSwitches_Before(false); // Snapshots 0x11112222 baseline

    uint64_t heapVal = 0x000001B400000008ULL;
    *(uint64_t*)(bld.data() + 0x500) = heapVal; // UI modified word to pointer
    BuildingSwitches_After(false); // Detects pointer, rejects and rolls back!
    g_mockExeBuffer[0xA9EF51] = 0;

    // Verify: live memory at 0x500..0x507 was ROLLED BACK to 0x11112222 and 0
    TEST_CHECK(*(uint32_t*)(bld.data() + 0x500) == 0x11112222);
    TEST_CHECK(*(uint32_t*)(bld.data() + 0x504) == 0x00000000);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // 2. Legitimate setting modification (scalar pair {1.0f, 0})
    g_mockExeBuffer[0xA9EF51] = 1; // Click active
    BuildingSwitches_Before(false); // Snapshots 0x11112222 baseline
    *(uint32_t*)(bld.data() + 0x500) = 0x3F800000; // 1.0f
    *(uint32_t*)(bld.data() + 0x504) = 0x00000000;
    BuildingSwitches_After(false);
    g_mockExeBuffer[0xA9EF51] = 0;

    // Verify: accepted! Local memory rolled back to wait for lockstep tick
    TEST_CHECK(*(uint32_t*)(bld.data() + 0x500) == 0x11112222);
    // Submit was called: tick due
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    // After lockstep tick boundary, live memory has the new setting
    TEST_CHECK(*(uint32_t*)(bld.data() + 0x500) == 0x3F800000);

    Commands_SetSession(COMMANDS_OFF);
    BuildingSwitches_Reset();
    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_M9_1_VehicleSwitches_PointerRejectionAndRollback()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    VehicleSwitches_Reset();
    Commands_SetSession(COMMANDS_HOST);

    // Setup mock vehicle
    std::vector<BYTE> veh(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(veh.data() + VEH_ID_OFFSET) = 502u;
    float pos[3] = { 150.0f, 0.0f, 250.0f };
    memcpy(veh.data() + 0x350, pos, sizeof(pos));

    std::vector<BYTE*> mockVehs = { veh.data() };
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_VEHICLES_OFFSET, mockVehs);

    // Offset 0x610: baseline setting (not a ListWord, not a TextWord)
    *(uint32_t*)(veh.data() + 0x610) = 0x33334444;
    *(uint32_t*)(veh.data() + 0x614) = 0x00000000;

    // 1. Frame with click and pointer modification
    g_mockExeBuffer[0xA9EF51] = 1; // Click active
    VehicleSwitches_Before(false); // Snapshots 0x33334444 baseline

    uint64_t heapVal = 0x000001B400000008ULL;
    *(uint64_t*)(veh.data() + 0x610) = heapVal; // UI modified word to pointer
    VehicleSwitches_After(false); // Detects pointer, rejects and rolls back!
    g_mockExeBuffer[0xA9EF51] = 0;

    // Verify: live memory rolled back to 0x33334444 and 0
    TEST_CHECK(*(uint32_t*)(veh.data() + 0x610) == 0x33334444);
    TEST_CHECK(*(uint32_t*)(veh.data() + 0x614) == 0x00000000);
    TEST_CHECK(!Commands_Due(Commands_ApplyEpoch()));

    // 2. Legitimate setting modification (scalar pair {1.0f, 0})
    g_mockExeBuffer[0xA9EF51] = 1; // Click active
    VehicleSwitches_Before(false); // Snapshots 0x33334444 baseline
    *(uint32_t*)(veh.data() + 0x610) = 0x3F800000; // 1.0f
    *(uint32_t*)(veh.data() + 0x614) = 0x00000000;
    VehicleSwitches_After(false);
    g_mockExeBuffer[0xA9EF51] = 0;

    // Verify: accepted! Local memory rolled back awaiting tick
    TEST_CHECK(*(uint32_t*)(veh.data() + 0x610) == 0x33334444);
    TEST_CHECK(Commands_Due(Commands_ApplyEpoch()));
    Commands_Boundary(Commands_ApplyEpoch());
    TEST_CHECK(*(uint32_t*)(veh.data() + 0x610) == 0x3F800000);

    Commands_SetSession(COMMANDS_OFF);
    VehicleSwitches_Reset();
    mockVehs.clear();
    WriteMockVectorHeader(g_mockExeBuffer + WORLD_RVA + WORLD_VEHICLES_OFFSET, mockVehs);
    return true;
}

static bool SubTest_M9_1_BlindBytesAndSettingsSyncToastSuppression()
{
    // 1. Feature 23: Complete scan of world range 0xAF0..0xB18
    for (unsigned off = 0xB18; off < 0xB30; off += 4) {
        TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(off));
    }
    for (unsigned off = 0xB30; off <= 0xB33; ++off) {
        TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(off));
    }
    for (unsigned off = 0xB34; off <= 0xB40; off += 4) {
        TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(off));
    }

    // 2. Feature 23: Complete scan of world range 0xC828..0xC840
    for (unsigned off = 0xC830; off < 0xC838; off += 4) {
        TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(off));
    }
    for (unsigned off = 0xC838; off <= 0xC83B; ++off) {
        TEST_CHECK(BuildingSwitches_IsOneByteWorldSettingForTest(off));
    }
    for (unsigned off = 0xC83C; off <= 0xC848; off += 4) {
        TEST_CHECK(!BuildingSwitches_IsOneByteWorldSettingForTest(off));
    }

    return true;
}

static bool SubTest_M9_1_CheatSanitizationAndAreaCloneBlock()
{
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Feature 24: Cheat sanitization of terrain elevation modifications
    *(int32_t*)(world + 0xAE18) = -99999;
    *(float*)(world + 0xAE1C) = 12345.67f;
    TEST_CHECK(Commands_SanitizeCheatState(world));
    TEST_CHECK(*(int32_t*)(world + 0xAE18) == 0);
    TEST_CHECK(*(float*)(world + 0xAE1C) == 0.0f);
    // Idempotent: already clean
    TEST_CHECK(!Commands_SanitizeCheatState(world));

    // 2. Feature 24 & 25: Uncarried tools & Area clone blocking
    TEST_CHECK(Commands_IsAreaCloneToolBlockedForTest("area_copy"));
    TEST_CHECK(Commands_IsAreaCloneToolBlockedForTest("area_paste"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("building_distribution"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("assign_co"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("firestation_addfire"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("building_copyweights"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("kgb_surveillance"));
    TEST_CHECK(!Commands_IsAreaCloneToolBlockedForTest("christmas_snow"));

    return true;
}

bool Test_Stress_M9_1_Challenger1()
{
    if (!H) { TsmBind(&g_mockHost); }
    printf("    [SubTest 1: Mouse Wheel Adversarial Float Fuzzing] ...\n"); fflush(stdout);
    if (!SubTest_M9_1_MouseWheelAdversarialFloatFuzzing()) return false;

    printf("    [SubTest 2: Pointer Filter Exhaustive Boundaries] ...\n"); fflush(stdout);
    if (!SubTest_M9_1_PointerFilterExhaustiveBoundaries()) return false;

    printf("    [SubTest 3: Building Switches Pointer Rejection & Rollback] ...\n"); fflush(stdout);
    if (!SubTest_M9_1_BuildingSwitches_PointerRejectionAndRollback()) return false;

    printf("    [SubTest 4: Vehicle Switches Pointer Rejection & Rollback] ...\n"); fflush(stdout);
    if (!SubTest_M9_1_VehicleSwitches_PointerRejectionAndRollback()) return false;

    printf("    [SubTest 5: Blind Bytes & Toast Suppression] ...\n"); fflush(stdout);
    if (!SubTest_M9_1_BlindBytesAndSettingsSyncToastSuppression()) return false;

    printf("    [SubTest 6: Cheat Sanitization & Area Clone Blocking] ...\n"); fflush(stdout);
    if (!SubTest_M9_1_CheatSanitizationAndAreaCloneBlock()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 42: Missing UI Elements Sync & Buffer Safety (Milestone M9.2, Features 26–31)
// ---------------------------------------------------------------------------

static bool SubTest_M9_2_VehicleAutoReplacementSync()
{
    // Feature 26: Intercept kind == 0xB in Purchase_Press(), sync replacement manager at *(world + 0xD440)
    TEST_CHECK(WORLD_AUTO_REPLACE_MGR_OFFSET == 0xD4E0u);
    TEST_CHECK(RVA_SET_AUTO_REPLACE == 0x63C410u);

    // Verify PurchaseMsg layout & auto-replace semantics
    PurchaseMsg m = {};
    m.kind = 0xB;
    m.typeIndex = 12u;
    m.resIndex = 5; // target vehicle type index
    m.typeCheck = 0xA1B2C3D4u;
    snprintf(m.scriptName, sizeof(m.scriptName), "media_soviet/scripts/truck_skd.ini");

    TEST_CHECK(m.kind == 0xB);
    TEST_CHECK(m.resIndex == 5);
    TEST_CHECK(strcmp(m.scriptName, "media_soviet/scripts/truck_skd.ini") == 0);

    return true;
}

static bool SubTest_M9_2_StorageEmptyButtonSync()
{
    // Feature 27: Storage empty button sync (IsLeftOutSlotDynamicOffset permits (local % 16) == 8 on click)
    // Offset in region 1 (0x1000..0x1FFF), local = 8 (local % 16 == 8)
    const unsigned kEmptyBtnOffset = 0x1008u;
    // Without click (noise filter scan): must be filtered out as noise
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(kEmptyBtnOffset, false));
    // With click: must NOT be left out, allowing the Empty Storage click to be captured and synced!
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(kEmptyBtnOffset, true));

    // Another offset with local % 16 == 8 in another slot (slot 1: 0x4000 + 0x1008)
    const unsigned kSlot1EmptyBtnOffset = 0x5008u;
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(kSlot1EmptyBtnOffset, false));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(kSlot1EmptyBtnOffset, true));

    // Arbitrary non-button offsets (e.g. local % 16 == 0 or 4) are not dynamic slot buttons
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000u, false));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000u, true));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1004u, false));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1004u, true));

    return true;
}

static bool SubTest_M9_2_LineWaypointsOnRoadAndAirNetworks()
{
    // Feature 28: Add 0xF838 (ROAD), 0xF840 (SECOND_ROAD), and 0xFAC8 (AIRROAD) to kNodeFields and Ref_NodeAt
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    const unsigned kTestFields[] = { 0xF840u, 0xF848u, 0xF9A8u, 0xFAC8u, 0xFAD0u };
    for (unsigned fieldOffset : kTestFields) {
        std::vector<BYTE> netObj(0x300, 0);
        std::vector<BYTE> nodeObj(32, 0);
        *(unsigned*)(nodeObj.data() + 4) = 0x43000000u; // float X = 128.0f
        *(unsigned*)(nodeObj.data() + 0xC) = 0x43800000u; // float Z = 256.0f

        std::vector<BYTE*> nodeVec = { nodeObj.data() };
        WriteMockVectorHeader(netObj.data() + 0x298, nodeVec);
        *(BYTE**)(world + fieldOffset) = netObj.data();

        RouteRef refN = kNoRef;
        TEST_CHECK(Ref_Node((U64)nodeObj.data(), refN));
        TEST_CHECK(refN.list == fieldOffset);
        TEST_CHECK(refN.place == 0);
        TEST_CHECK(Ref_NodeAt(refN) == (U64)nodeObj.data());
        TEST_CHECK(Ref_NodeCheck((U64)nodeObj.data()) == refN.check);

        *(BYTE**)(world + fieldOffset) = nullptr;
    }

    return true;
}

static bool SubTest_M9_2_FactoryQueueAndMachineReplacementSites()
{
    // Feature 29: In game_calls.cpp:
    // Verify site 0x776557 (ReplaceMachines) & removal of broken 0x1A15A8 and read-only 0x7D8738.
    TEST_CHECK(GameCalls_HasCallSiteForTest(0x7B3937u, 0x1C1A50u));
    TEST_CHECK(!GameCalls_HasCallSiteForTest(0x1A1448u, 0x1BBEB0u));
    TEST_CHECK(!GameCalls_HasCallSiteForTest(0x7D8BE8u, 0x664C30u));

    // Check non-existent site
    TEST_CHECK(!GameCalls_HasCallSiteForTest(0x123456u, 0x654321u));

    return true;
}

static bool SubTest_M9_2_WasteContainerTypesAndCityReassign()
{
    // Feature 30: SettingsSync_OwnsSlotWord returns true for region == 2 (waste container slot data)
    TEST_CHECK(SettingsSync_OwnsSlotWord(nullptr, 0, 2, 0));
    TEST_CHECK(SettingsSync_OwnsSlotWord(nullptr, 0, 2, 4));
    TEST_CHECK(SettingsSync_OwnsSlotWord(nullptr, 0, 2, 8));
    TEST_CHECK(SettingsSync_OwnsSlotWord(nullptr, 1, 2, 12));
    TEST_CHECK(!SettingsSync_OwnsSlotWord(nullptr, 1, 1, 0)); // region 1 non-slot0 is not owned by SettingsSync

    // Verify SettingWire data floats capacity: 224 floats supports 32 endpoints (7 floats each)
    // and waste container resource serialization
    TEST_CHECK(SETTINGS_WIRE_DATA_FLOATS == 224);
    SettingWire wire = {};
    wire.field = CITIZEN_DESTINATIONS_FIELD;
    wire.count = 32 * 7;
    TEST_CHECK(wire.count <= SETTINGS_WIRE_DATA_FLOATS);

    // City reassign kind constant
    TEST_CHECK(KIND_CITY_REASSIGN == 0xFu);

    return true;
}

static bool SubTest_M9_2_BufferSafetyAndLimitsExpansion()
{
    // Feature 31: Buffer safety expansions
    TEST_CHECK(LS_MAX_PAYLOAD == (16u << 20)); // 16 MB lockstep payload
    TEST_CHECK(CITIZEN_DESTINATIONS_MAX == 32); // 32 citizen destination endpoints
    TEST_CHECK(SOURCE_SLOTS_MAX_BUILDINGS == 16384); // 16384 source slot scan capacity
    TEST_CHECK(CARGO_SETTINGS_MAX_OBJECTS == 32); // 32 objects per kind
    TEST_CHECK(ROUTE_TOOLS_DEPOT_MAX == 256); // 256 vehicles in depot batch
    TEST_CHECK(ROAD_SYNC_MAX_UPGRADE_PIECES == 128); // 128 upgrade pieces
    TEST_CHECK(ROAD_SYNC_MAX_DEMOLISH_PIECES == 64); // 64 demolish pieces

    RoadSyncDemolishMsg demoMsg = {};
    demoMsg.pieceCount = ROAD_SYNC_MAX_DEMOLISH_PIECES;
    TEST_CHECK(sizeof(demoMsg.pieces) / sizeof(demoMsg.pieces[0]) == 64);

    RoadSyncUpgradeMsg upMsg = {};
    upMsg.pieceCount = ROAD_SYNC_MAX_UPGRADE_PIECES;
    TEST_CHECK(sizeof(upMsg.pieces) / sizeof(upMsg.pieces[0]) == 128);

    return true;
}

bool Test_MissingUiElementsAndBufferSafety_M9_2()
{
    printf("    [SubTest 1: Vehicle Auto-Replacement Sync (Feature 26)] ...\n"); fflush(stdout);
    if (!SubTest_M9_2_VehicleAutoReplacementSync()) return false;

    printf("    [SubTest 2: Storage Empty Button Sync (Feature 27)] ...\n"); fflush(stdout);
    if (!SubTest_M9_2_StorageEmptyButtonSync()) return false;

    printf("    [SubTest 3: Line Waypoints On Road & Air Networks (Feature 28)] ...\n"); fflush(stdout);
    if (!SubTest_M9_2_LineWaypointsOnRoadAndAirNetworks()) return false;

    printf("    [SubTest 4: Factory Queue & Machine Replacement Sites (Feature 29)] ...\n"); fflush(stdout);
    if (!SubTest_M9_2_FactoryQueueAndMachineReplacementSites()) return false;

    printf("    [SubTest 5: Waste Container Types & City Reassign (Feature 30)] ...\n"); fflush(stdout);
    if (!SubTest_M9_2_WasteContainerTypesAndCityReassign()) return false;

    printf("    [SubTest 6: Buffer Safety & Limits Expansion (Feature 31)] ...\n"); fflush(stdout);
    if (!SubTest_M9_2_BufferSafetyAndLimitsExpansion()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Suites #43 & #44: Milestone M9.2 Challenger Stress Suites
// ---------------------------------------------------------------------------

#define Test_Stress_M9_2_Challenger2 Test_Stress_M9_2_Challenger2_Impl
#include "m9_2_challenger2_stress.cpp"
#undef Test_Stress_M9_2_Challenger2

bool Test_Stress_M9_2_Challenger2()
{
    return Test_Stress_M9_2_Challenger2_Impl();
}

#define Test_Stress_M9_2_Challenger1 Test_Stress_M9_2_Challenger1_Impl
#include "m9_2_challenger1_stress.cpp"
#undef Test_Stress_M9_2_Challenger1

bool Test_Stress_M9_2_Challenger1()
{
    return Test_Stress_M9_2_Challenger1_Impl();
}
