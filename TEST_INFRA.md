# RepublicMP Automated Test Infrastructure & E2E Test Suite Specification

## 1. Overview & Architecture

RepublicMP features a comprehensive, standalone C++20 automated test harness (`republicmp_tests.exe`) designed to rigorously verify the multiplayer mod across network, synchronization, determinism, simulation, economic, UI, and save/load subsystems without requiring the live `SOVIET64.exe` game client or Steam connection.

### 1.1 MockEngineHarness (16 MB Simulated Engine Image)
- **Memory Footprint**: A 16 MB contiguous virtual page buffer (`MOCK_EXE_SIZE = 0x1000000`, allocated via `VirtualAlloc(PAGE_EXECUTE_READWRITE)`), covering all RVA addresses and offsets in `SOVIET64.exe` (PE timestamp `6A3EB6AD`).
- **Engine Subsystems Emulated**:
  - `WORLD` object at `0x9D4F10` (economy scalars, building vectors at `+0x9E6A18`, vehicle vectors at `+0xDE0`, loan structures at `+0x10BD0`, city lists at `+0x10348`, notifications at `+0xD208`, brush tools at `+0xD488`).
  - MSVC `std::vector` layout `{ begin, end, cap }` through safe POD helper `WriteMockVectorHeader`.
  - Tool guard state (`SafeIsToolGuardBusy`), building placement tool callbacks, and RVA function stubs (`MANAGER_RESET_RVA = 0x542A30`, `CANCEL_SEG_RVA = 0x5434C0`, etc.).
  - File system isolation: Isolated temporary directory per test process run (`RepublicMP_TestRun_<PID>`).

### 1.2 Verification Pipeline (`run_checks.bat` / `run_checks.ps1`)
The automated verification pipeline enforces 5 strict quality gates before any code or test change can be merged:
1. **[1/5] Full Plugin & Launcher Build (`build.bat`)**: Compiles `tesmioloader.dll`, `dinput8.dll`, `tesmiolauncher.exe`, all utility plugins, and `plugins\RepublicMP.dll` via MSBuild x64 Release.
2. **[2/5] Test Runner Compilation & Execution (`republicmp_tests.exe`)**: Builds `tests\test_main.cpp` linked against `obj\Release\x64\*.obj`, running all 23 suites with 0 assertion failures.
3. **[3/5] MSVC Static Code Analysis (`/analyze`)**: Enforces zero static analysis defects across core, determinism, lockstep, session, and test sources (`/wd6320 /wd6255 /wd6258 /wd28159`).
4. **[4/5] LLVM `clang-tidy` Static Analyzer**: Strict AST linting with `WarningsAsErrors: '*'` adhering to `.clang-tidy`.
5. **[5/5] Uncle Bob Clean Code Gate (`ub_check.py`)**:
   - `Dryer`: Structural duplication threshold `< 0.80` (`--dry-threshold 0.80`, max duplicates `0`).
   - `Crapper`: Cyclomatic complexity threshold `< 60` (`--max-cc 60`).
   - `Dead Code`: Zero unused `static` functions or unreferenced symbols.

---

## 2. 4-Tier Test Methodology & Coverage Matrix

The E2E test suite adheres to the 4-tier testing hierarchy specified in the project engineering plan:

### 2.1 Summary of Test Suites (23 Total Suites)
| Suite Index | Name / Category | Scope / Focus | Test Count |
|:---:|:---|:---|:---:|
| 1–15 | Foundational Engine Suites | Memory safety, RVA invariants, determinism, two-state economy, save ZIP, loan math | ~60 tests |
| 16 | `E2E_Tier1_FeatureCoverage_R1_to_R6` | Linear build continuity, customs trade revenue, building names on placement, independent financing, DLC road filtering, city names & deletion | 30 tests (5 per feature) |
| 17 | `E2E_Tier1_FeatureCoverage_R7_to_R12` | Host save transfer %, separate budgets, justice/foster care stats, vehicle smoothing & passengers, purchase sound, health/satisfaction breakdown | 30 tests (5 per feature) |
| 18 | `E2E_Tier1_FeatureCoverage_R13_to_R18` | Stability & crash barriers, concurrent same-tech research, bidirectional research sync, road upgrade with footpaths, oil pump animation, mod sync modal | 30 tests (5 per feature) |
| 19 | `E2E_Tier2_BoundaryCases_R1_to_R6` | 0/max limits, negative balance, empty names, unshared DLC road replays, full buffers | 30 tests (5 per feature) |
| 20 | `E2E_Tier2_BoundaryCases_R7_to_R12` | Massive save transfers, debt caps, passenger overflow, rapid buy sound spam, mute persistence | 30 tests (5 per feature) |
| 21 | `E2E_Tier2_BoundaryCases_R13_to_R18` | Mid-load disconnects, double teardown, extreme brush edits, orphaned tech states, 50-mod packs | 30 tests (5 per feature) |
| 22 | `E2E_Tier3_CrossFeatureInteractions` | 10 Pairwise combinations across distinct features (customs trade + independent financing, drag continuity + footpath upgrade, research + city deletion, etc.) | 10 tests |
| 23 | `E2E_Tier4_RealWorldScenarios` | 5 Multi-step co-op workloads (Industrial corridor, save reload & expansion, research race & cross-border, recovery & resync under latency, high-density metropolitan) | 5 scenarios |
| **Total** | **Full Automated Test Suite** | **Comprehensive Opaque-Box Coverage (R1–R18)** | **255 tests** |

---

## 3. Requirement Mapping (R1 through R18)

| Req ID | Requirement Description | Tier 1 Function | Tier 2 Function | Tier 3 & Tier 4 Coverage |
|:---:|:---|:---|:---|:---|
| **R1** | Linear Building Continuity & Drag Protection | `SubTest_T1_R1_LinearBuildingContinuity` | `SubTest_T2_R1_LinearBuildingContinuity_Boundaries` | `SubTest_T3_Cross_2`, `SubTest_T4_Scenario_1` |
| **R2** | Foreign Customs Border Trade Revenue Attribution | `SubTest_T1_R2_ForeignCustomsTradeRevenue` | `SubTest_T2_R2_ForeignCustomsTrade_Boundaries` | `SubTest_T3_Cross_1`, `SubTest_T4_Scenario_1` |
| **R3** | Building Names Display During Placement Tool | `SubTest_T1_R3_BuildingNamesOnPlacement` | `SubTest_T2_R3_BuildingNamesOnPlacement_Boundaries` | `SubTest_T3_Cross_5`, `SubTest_T4_Scenario_2` |
| **R4** | Independent Currency & Financing Choice (Rub/USD) | `SubTest_T1_R4_IndependentCurrencyChoice` | `SubTest_T2_R4_IndependentCurrencyChoice_Boundaries` | `SubTest_T3_Cross_1`, `SubTest_T4_Scenario_1` |
| **R5** | Prohibiting / Filtering Roads from Missing DLCs | `SubTest_T1_R5_DlcRoadFiltering` | `SubTest_T2_R5_DlcRoadFiltering_Boundaries` | `SubTest_T3_Cross_4`, `SubTest_T4_Scenario_2` |
| **R6** | City Name Synchronization & City Deletion | `SubTest_T1_R6_CityNameSyncAndDeletion` | `SubTest_T2_R6_CityNameSyncAndDeletion_Boundaries` | `SubTest_T3_Cross_3`, `SubTest_T4_Scenario_2` |
| **R7** | Host Save Transfer Percentage Display in Lobby | `SubTest_T1_R7_HostSaveTransferPercentage` | `SubTest_T2_R7_HostSaveTransferPercentage_Boundaries` | `SubTest_T3_Cross_7`, `SubTest_T4_Scenario_4` |
| **R8** | Separate Player Budgets on Save Load | `SubTest_T1_R8_SeparateBudgetPersistence` | `SubTest_T2_R8_SeparateBudgetPersistence_Boundaries` | `SubTest_T3_Cross_4`, `SubTest_T4_Scenario_2` |
| **R9** | Courts, Prisons & Orphanages UI Stats Split | `SubTest_T1_R9_JusticeFosterCareAndUiStats` | `SubTest_T2_R9_JusticeFosterCareAndUiStats_Boundaries` | `SubTest_T3_Cross_6`, `SubTest_T4_Scenario_5` |
| **R10** | Vehicle Movement Smoothing & Passenger Sync | `SubTest_T1_R10_VehicleSmoothingAndPassengers` | `SubTest_T2_R10_VehicleSmoothingAndPassengers_Boundaries` | `SubTest_T3_Cross_8`, `SubTest_T4_Scenario_5` |
| **R11** | Vehicle Purchase Sound Replay | `SubTest_T1_R11_VehiclePurchaseSound` | `SubTest_T2_R11_VehiclePurchaseSound_Boundaries` | `SubTest_T3_Cross_5`, `SubTest_T4_Scenario_1` |
| **R12** | Health & Citizen Satisfaction Breakdown by State | `SubTest_T1_R12_HealthSatisfactionBreakdown` | `SubTest_T2_R12_HealthSatisfactionBreakdown_Boundaries` | `SubTest_T3_Cross_6`, `SubTest_T4_Scenario_3` |
| **R13** | Stability, Crash Prevention & SEH Protection | `SubTest_T1_R13_StabilityAndCrashPrevention` | `SubTest_T2_R13_StabilityAndCrashPrevention_Boundaries` | `SubTest_T3_Cross_9`, `SubTest_T4_Scenario_4` |
| **R14** | Concurrent Same-Tech Research by Both Republics | `SubTest_T1_R14_ConcurrentSameTechResearch` | `SubTest_T2_R14_ConcurrentSameTechResearch_Boundaries` | `SubTest_T3_Cross_3`, `SubTest_T4_Scenario_3` |
| **R15** | Fully Independent Research Trees per State (Zero Cross-Unlock, Independent UI & Separate Persistence) | `SubTest_T1_R15_IndependentResearchTrees` | `SubTest_T2_R15_IndependentResearchTrees_Boundaries` | `SubTest_T3_Cross_10`, `SubTest_T4_Scenario_3` |
| **R16** | Road Upgrade with Footpaths Preservation | `SubTest_T1_R16_RoadUpgradeWithFootpaths` | `SubTest_T2_R16_RoadUpgradeWithFootpaths_Boundaries` | `SubTest_T3_Cross_2`, `SubTest_T4_Scenario_1` |
| **R17** | Oil Pump Model Visibility & Nodding Animation | `SubTest_T1_R17_OilPumpModelAndAnimation` | `SubTest_T2_R17_OilPumpModelAndAnimation_Boundaries` | `SubTest_T3_Cross_8`, `SubTest_T4_Scenario_4` |
| **R18** | Mod Sync Download Modal Automatic Dismissal | `SubTest_T1_R18_ModSyncPopupAutoDismiss` | `SubTest_T2_R18_ModSyncPopupAutoDismiss_Boundaries` | `SubTest_T3_Cross_10`, `SubTest_T4_Scenario_5` |

---

## 4. Execution Commands

- **Run unit tests only**:
  ```powershell
  cmd.exe /c "d:\SovietRepublic MP\TesmioLoader\build\compile_tests.bat && d:\SovietRepublic MP\TesmioLoader\build\republicmp_tests.exe"
  ```
- **Run full 5-stage verification gate**:
  ```powershell
  cmd.exe /c "d:\SovietRepublic MP\TesmioLoader\run_checks.bat"
  ```
