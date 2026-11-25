// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP Automated Unit & Integration Test Suite with Mock Engine Harness.
// Verifies refactored modules and all critical invariants from AGENTS.md without launching SOVIET64.exe.

#include "test_harness.h"
#include "../core/version.h"
#include "../transport/lan_discovery.h"
#include "../transport/network.h"

#include <stdio.h>
#include <filesystem>

// ---------------------------------------------------------------------------
// Test Runner Entry Point
// ---------------------------------------------------------------------------

struct TestCase {
    const char* name;
    bool (*fn)();
};

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    printf("================================================================\n");
    printf(" RepublicMP Automated Unit & Integration Test Suite (v%s)\n", REPUBLICMP_VERSION);
    printf("================================================================\n");

    InitMockHarness();
    LanDiscovery_SetLoopbackOnlyForTest(true);
    Network_SetLoopbackOnlyForTest(true);

    const TestCase kTests[] = {
        { "RvaAndSafeMem (Invariants #1, #12, #16, InitOnce)",   &Test_RvaAndSafeMem },
        { "WorldRefs (Buildings, Constructions, Vehicles, Net)", &Test_WorldRefs },
        { "StateHash (Determinism, Mutation, Corrupted Bounds)", &Test_StateHash },
        { "TwoStates_ZonesAndEconomy (Zones, Split, Inv #10)",   &Test_TwoStates_ZonesAndEconomy },
        { "TwoStates_SaveMetaAndScriptIni (Inv #4 & #5)",        &Test_TwoStates_SaveMetaAndScriptIniSafety },
        { "ModSync_HeaderBinAndWorkshopFilter (Inv #6 & #9)",    &Test_ModSync_HeaderBinAndWorkshopFilter },
        { "BuildingSwitches_Commands_SaveZip (Inv #12, Zip)",    &Test_BuildingSwitches_Commands_SaveZip },
        { "WaitingLobbyAndActionLock (Pause, Lock, Inspect)",    &Test_WaitingLobbyAndActionLock },
        { "TwoStates_Loans (Borrow Ceiling, UI Swap, Daily)",    &Test_TwoStates_Loans },
        { "TwoStates_EconomyStats (Ledgers, UI Swap, Rollover)", &Test_TwoStates_EconomyStats },
        { "SingleplayerIsolationAndReset (Dormancy, Reset, SP)", &Test_SingleplayerIsolationAndReset },
        { "VehicleDlcFilter_ScriptName_DiagLog (DLC, Remap, Diag)", &Test_VehicleDlcFilter_ScriptName_DiagLog },
        { "VehicleCargoAndScheduleSync (Train/BldSoftSync, Sched)", &Test_VehicleCargoAndScheduleSync },
        { "CameraPersistenceAndFraming (SaveMeta, Resync, Init)", &Test_CameraPersistenceAndFraming },
        { "RemoteSoundAndWarningIsolation (Sound2D/3D, Popups)", &Test_RemoteSoundAndWarningIsolation },
        { "E2E_Tier1_FeatureCoverage_R1_to_R6 (LinearBuild, Customs, BldNames, Finance, DlcRoads, Cities)", &Test_E2E_Tier1_FeatureCoverage_R1_to_R6 },
        { "E2E_Tier1_FeatureCoverage_R7_to_R12 (Transfer%, Budgets, Justice, Vehicles, Sound, Health)", &Test_E2E_Tier1_FeatureCoverage_R7_to_R12 },
        { "E2E_Tier1_FeatureCoverage_R13_to_R18 (Stability, ResearchConcurrent, ResearchSync, Footpaths, OilPump, ModModal)", &Test_E2E_Tier1_FeatureCoverage_R13_to_R18 },
        { "E2E_Tier2_BoundaryCases_R1_to_R6 (Limits, 0-Values, Unshared DLCs, Empty Strings, Rapid Fire)", &Test_E2E_Tier2_BoundaryCases_R1_to_R6 },
        { "E2E_Tier2_BoundaryCases_R7_to_R12 (Large Saves, Debt, Capacity Caps, Passenger Sync, Mute Safety)", &Test_E2E_Tier2_BoundaryCases_R7_to_R12 },
        { "E2E_Tier2_BoundaryCases_R13_to_R18 (Disconnects, Double Teardown, Extreme Brush, Orphan Techs)", &Test_E2E_Tier2_BoundaryCases_R13_to_R18 },
        { "E2E_Tier3_CrossFeatureInteractions (Pairwise Combinations Across R1..R18)", &Test_E2E_Tier3_CrossFeatureInteractions },
        { "E2E_Tier4_RealWorldScenarios (Multi-Step Workloads, Industrial, Urban, Research, Recovery)", &Test_E2E_Tier4_RealWorldScenarios },
        { "Stress_M1_Challenger_R1_R4_R5_R16 (DragContinuity, ComplexFootpaths, DlcFiltering, Finance)", &Test_Stress_M1_Challenger_R1_R4_R5_R16 },
        { "Stress_M4_Challenger_R3_R10_R11_R17 (SmoothingEMA, AudioMute, OilPumpRestoration, BadgeCharset)", &Test_Stress_M4_Challenger_R3_R10_R11_R17 },
        { "Stress_M4_Challenger2 (PlacementConcurrency, RenderPassDeterminism, SoftSyncWireFuzz)", &Test_Stress_M4_Challenger2 },
        { "Stress_M4_Challenger1_Iter2 (Charset, Concurrency, 50kmSnapping, SEHRecovery)", &Test_Stress_M4_Challenger1_Iter2 },
        { "Stress_M1_Challenger2 (InterleavedDragContinuity, SnapshotRouteMemorySafety, MultiThreadConcurrency)", &Test_Stress_M1_Challenger2 },
        { "Stress_M1_Challenger1_Iter2 (RouteBufferBitExact, SEHFootpathUpgrade, DlcBoundaryCheck)", &Test_Stress_M1_Challenger1_Iter2 },
        { "Stress_M3_Challenger2 (CivicSegregation, CrimeTourism, DemographicWeights, SingleplayerIsolation)", &Test_Stress_M3_Challenger2 },
        { "Stress_M3_Challenger1_R9_R12 (CivicSegregation, ZeroFacilities, wCit0Demographics, HealthHooks)", &Test_Stress_M3_Challenger1_R9_R12 },
        { "Stress_M5_Challenger2 (ResearchConcurrency, 32SlotSat, CityDoubleDelete, TransferJitter)", &Test_Stress_M5_Challenger2 },
        { "Stress_M5_Challenger1_R6_R7_R14_R15_R18 (TechIsolation, AsymmetricProgress, CityBounds, Telemetry)", &Test_Stress_M5_Challenger1_R6_R7_R14_R15_R18 },
        { "Stress_M6_Challenger1_R13 (TeardownSafety, MaterialValidation, BrushThrottling)", &Test_Stress_M6_Challenger1_R13 },
        { "Stress_M6_Challenger2 (TeardownConcurrency, MaterialBoundaryFuzz, BrushRecovery)", &Test_Stress_M6_Challenger2 },
        { "SettingsSync_MediaPrisonOrphanageAlerts (R2/R3 Broadcast, Upbringing, Alerts, Clamping)", &Test_SettingsSync_MediaPrisonOrphanageAlerts },
        { "Stress_Gen6_Challenger1 (FloatFuzzing, MalformedPackets, CheckboxBoundaries, StudioResilience)", &Test_Stress_Gen6_Challenger1 },
        { "JoinHubDistOfficeCarrierAndStorageVisuals (JoinHubAll, DistLinks, CarrierRecovery, StorageVis)", &Test_JoinHubDistOfficeCarrierAndStorageVisuals },
        { "CrashFixesAndRcoTrackBuilder (SafeSeh, CourtCase, DepotStopGuard, RcoTrackBuilder)", &Test_CrashFixesAndRcoTrackBuilder },
        { "SystemUiInterceptionAndNetworkTools_M9_1 (WheelScroll, PtrFilter, BlindBytes, SanitizeCheat, AreaCloneBlock)", &Test_SystemUiInterceptionAndNetworkTools_M9_1 },
        { "Stress_M9_1_Challenger1 (WheelScrollFuzz, PtrFilterBoundaries, SwitchRollback, BlindBytes, CheatSanitize)", &Test_Stress_M9_1_Challenger1 },
        { "MissingUiElementsAndBufferSafety_M9_2 (Features 26-31)", &Test_MissingUiElementsAndBufferSafety_M9_2 },
        { "Stress_M9_2_Challenger2 (GameCallsSites, WasteSlotBoundaries, CityReassignReplay, LsMaxPayload16MB)", &Test_Stress_M9_2_Challenger2 },
        { "Stress_M9_2_Challenger1 (Waypoints5Nets, StorageEmptyOscillation, BufferBoundsDestinationsSlots)", &Test_Stress_M9_2_Challenger1 },
        { "SaveSyncAndEconStatePersistence_M9_3 (Features 32-36)", &Test_SaveSyncAndEconStatePersistence_M9_3 },
        { "Stress_M9_3_Challenger1 (AutosaveCadence, TrackerFuzz, PruningStress)", &Test_Stress_M9_3_Challenger1 },
        { "Stress_M9_3_Challenger2 (SettingsIsolation, CamBranching, OwnershipPreservation, BackupIntegrity, Int64Limits)", &Test_Stress_M9_3_Challenger2 },
        { "StateHashAndTwoStates_M9_4 (Features 37-40)", &Test_StateHashAndTwoStates_M9_4 },
        { "Stress_M9_4_Challenger1 (14NetsSensitivity, FalseNegatives, FuelSpeedFuzz, 4Passengers, MachinerySync)", &Test_Stress_M9_4_Challenger1 },
        { "Stress_M9_4_Challenger2 (ResearchSwitchAntiExploit, BorderDemolition, TouristEdgeCases, StrictResearchIsolation, LegacyModeElimination)", &Test_Stress_M9_4_Challenger2 },
        { "R2Bugs_DenomReplaceAutoReplaceLineSet (M1 Bugs #1-#4)", &Test_R2Bugs_DenomReplaceAutoReplaceLineSet },
        { "R3Bugs_PersonalCarAutoBuyFactoryQueue (M2 Bugs #5-#7)", &Test_R3Bugs_PersonalCarAutoBuyFactoryQueue },
        { "R1SecondPass_WasteSetForBioCrossroadEditor (M3 R1-1A..2B)", &Test_R1SecondPass_WasteSetForBioCrossroadEditor },
        { "HookFixesAndRunAudit_M1 (Removed9, FireDispatch, AssignCo, RunAudit)", &Test_HookFixesAndRunAudit_M1 },
        { "Audit7Fixes_M1 (Bugs #1-#7 Sync, Skip, CopyWeights, Pre-GameRules)", &Test_Audit7Fixes_M1 },
        { "LanDiscoveryAndAddressNormalization_M1 (F1-F5)", &Test_LanDiscoveryDirectSmoke_M1 },
        { "SteamFreeSessionAndModSync_M2 (F6-F8)", &Test_SteamFreeSessionAndModSyncDirectSmoke_M2 },
        { "LanUiBrowserHostIpAndToasts_M3 (F9-F11)", &Test_LanUiBrowserHostIpAndToastsDirectSmoke_M3 },
        { "LanE2eSuite_M4 (145 Cases: F1-F11, Tiers 1-5)", &Test_LanE2eSuite_M4 },
        { "HotfixV171_CrashesHostLostSteamFriendsAckSpeed (Issues #1-#4)", &Test_HotfixV171_All },
        { "RcoRailConstructionFixes (Bugs #1-#6: SiteStopFilter, DemolArg6, RailChain, QueueReorder, 0x7D8BE8)", &Test_RcoRailConstructionFixes_All },
        { "TwoStatesResearchDesyncFixes (HashResearch, MidStep DailyUpdate/Customs, BeforeStep Idempotency)", &Test_TwoStatesResearchDesyncFixes },
        { "RcoRefuelHeldFrameAndSoftSyncFixes (VehicleRefuel float ABI, HeldFrame SameTick Refresh, SoftSync Exact Cargo, DesyncBreakdown)", &Test_RcoRefuelHeldFrameAndSoftSyncFixes },
        { "Perf_BldPtrOwnerCacheParityAndInvalidation (1A: 512+ Overflow, Gen Bumps, SessionRunning Gate)", &TestPerf_BldPtrOwnerCacheParityAndInvalidation },
        { "Perf_SinglePassDemographicsAndUiCacheParity (1B+1D: Early/Late SubStep, 3 Predicates, Held UI Cache, Backfill, 365 Hist SEH)", &TestPerf_SinglePassDemographicsAndUiCacheParity },
        { "Perf_InPlaceEconAttributionAndNestedVehicleOverride (1E: Zero-Delta, 5-memcmp, Nested Vehicle Scope, Illness Direct, Scratch)", &TestPerf_InPlaceEconAttributionAndNestedVehicleOverride },
        { "Perf_ResearchAndVehLicenseMountCache (1C: VehKey Cache, Mask Equivalence Skip, hasUnresolvedBldPtr, 7+ Invalidations)", &TestPerf_ResearchAndVehLicenseMountCache },
        { "Perf_SiteRandFlatTableAndSwitchInPlaceCompare (2A-2C: SiteRand 2048 Flat, KeyDown/SetEvent Cache, Switch In-Place & Backoff, Coalesced SEH)", &TestPerf_SiteRandFlatTableAndSwitchInPlaceCompare },
        { "Perf_TcpNoDelayAndSteamP2PRingBuffer (3A-3B: TCP_NODELAY, 16MB Power-of-2 Ring Buffer, Scratch Reuse, s_dataCv)", &TestPerf_TcpNoDelayAndSteamP2PRingBuffer },
        { "Perf_M6TsUiAndHeldFrameOptimizations (M6: CitLifespan Dirty Hook, AfterSubStep UI Cache, Hist Dirty Mask, Loan Fast-Path, HeldFrame Coalesced SEH)", &TestPerf_M6TsUiAndHeldFrameOptimizations },
    };

    for (const auto& tc : kTests) {
        ++g_totalTests;
        printf("[RUN ] %s ...\n", tc.name);
        bool ok = tc.fn();
        if (ok) {
            ++g_passedTests;
            printf("[PASS] %s\n", tc.name);
        } else {
            printf("[FAIL] %s\n", tc.name);
        }
        ResetMockWorldMemory();
    }

    ResetMockWorldMemory();

    // Cleanup temporary test directory
    if (g_mockBaseDir[0]) {
        std::error_code ec;
        std::filesystem::remove_all(g_mockBaseDir, ec);
    }
    if (g_mockExeBuffer) {
        VirtualFree(g_mockExeBuffer, 0, MEM_RELEASE);
        g_mockExeBuffer = nullptr;
    }

    printf("================================================================\n");
    printf(" Result: %d / %d test suites passed (%d failed assertions)\n",
           g_passedTests, g_totalTests, g_failedAssertions);
    printf("================================================================\n");

    int code = (g_passedTests == g_totalTests && g_failedAssertions == 0) ? 0 : 1;
    ExitProcess(code);
    return code;
}
