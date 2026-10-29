// SPDX-License-Identifier: GPL-3.0-only
// Two-States mode ("Два государства") & Map Border / World Size expansion.
//
// Gameplay modes:
//   0 = COOP_MODE_ONE_STATE  ("Одно государство" - shared republic, shared treasury, full map)
//   1 = COOP_MODE_TWO_STATES ("Два государства"  - West Republic [Host] & East Republic [Client],
//                             separate Ruble/Dollar treasuries, X=0 border with shared border zone,
//                             building/network ownership protection, inter-state money transfers)
//
// Map expansion:
//   - Runtime C3D_TERRAIN::GetTerrainBoundary* IAT hooks + script.ini patcher to unlock the 2 km
//     restricted buffer around standard 20x20 km maps (256 km^2 -> ~388 km^2 playable area).
//   - 30x30 km (900 km^2) expanded map generator from any loaded map/save.
#pragma once

#include "tesmio_plugin.h"
#include "../core/engine_rva.h"

enum CoopStateMode {
    COOP_MODE_ONE_STATE = 0,
    COOP_MODE_TWO_STATES = 1,
};

enum LsLoanOp {
    LOAN_OP_BORROW        = 1,
    LOAN_OP_REPAY_PARTIAL = 2,
    LOAN_OP_REPAY_FULL    = 3,
};

#pragma pack(push, 1)
struct LsStateTransfer {
    int fromState; // 1 = Host (West), 2 = Client (East)
    int toState;   // 1 = Host (West), 2 = Client (East)
    double rubles;
    double dollars;
};

struct LsLoanCommand {
    int32_t    op;           // LsLoanOp: 1 = borrow, 2 = partial repay, 3 = full repay
    int32_t    ownerSubType; // 0 = State 1 (West SSR), 1 = State 2 (East SSR)
    int32_t    ownerLoanOrd; // 0..M-1 index among ownerSubType's loans (for repay)
    SovietLoan loan;         // 40-byte loan record (new loan on borrow, updated/repaid loan on repay)
    double     dRub;         // treasury delta in RUB (+amount on borrow, -repay on repay)
    double     dUsd;         // treasury delta in USD (+amount on borrow, -repay on repay)
    float      dStats[4];    // delta of world + 0x11718..0x11724 ($Loan_Ballance_USD, $Loan_Interest_USD, $Loan_Ballance_RUB, $Loan_Interest_RUB)
};
#pragma pack(pop)
static_assert(sizeof(LsLoanCommand) == 84, "LsLoanCommand wire size must be 84 bytes");

void TwoStates_Bind(const TsmHost* host);
bool TwoStates_Install();

// Host choice before starting a session (propagated to the client in SessionHello).
void TwoStates_SetMode(int mode);
int TwoStates_GetMode();
bool TwoStates_IsActive(); // true while a session (or local test) has COOP_MODE_TWO_STATES active

// Half-width of the shared border strip around X = 0 (default 250.0f meters -> 500m strip [-250..+250]).
void TwoStates_SetBorderHalfWidth(float meters);
float TwoStates_GetBorderHalfWidth();

// Whether each state receives 100% of the save's starting money (true, default) or splits 50/50 (false).
void TwoStates_SetFullStartingCapital(bool full);
bool TwoStates_GetFullStartingCapital();

// Local player's state index: 1 = West Republic (Host), 2 = East Republic (Client).
int TwoStates_LocalState();
const char* TwoStates_StateName(int state);

// Session lifecycle (called by save_sync and determinism load hook).
void TwoStates_CaptureLobbyBaseMoney();
void TwoStates_PrepareHostSave(bool isResync, double* outRub, double* outUsd);
void TwoStates_ReapplyWaitingLobbySettings();
bool TwoStates_IsLoadedFromCoopSave();
void TwoStates_OnSessionJoined(bool isHost, int mode, float borderHalfWidth, bool expandBorders,
                               const double stateRub[2], const double stateUsd[2]);
void TwoStates_OnWorldLoaded();
void TwoStates_OnSessionEnded();

// Per-state treasury access (state: 1 or 2).
double TwoStates_GetRubles(int state);
double TwoStates_GetDollars(int state);
double TwoStates_LocalRubles();
double TwoStates_LocalDollars();
void TwoStates_SyncBalancesFromHost(double r1, double r2, double u1, double u2);

// Deterministic Lockstep hooks:
//   - Around Apply(const Command& c) in commands.cpp
void TwoStates_BeginCommand(int ownerState);
void TwoStates_EndCommand(int ownerState);
int  TwoStates_GetCommandOwner();
bool TwoStates_ApplyTransferCommand(const LsStateTransfer& t);
void TwoStates_RequestTransfer(double rubles, double dollars);
bool TwoStates_ApplyLoanCommand(const LsLoanCommand& cmd);
void TwoStates_GetLoanDebt(int state, double* outRubDebt, double* outUsdDebt, int* outLoanCount = nullptr);

//   - Around simulation step (HookedSimStep .. Determinism_AfterLate)
void TwoStates_BeforeStep(BYTE* world);
void TwoStates_AfterStep(BYTE* world);

//   - Around local UI / tool frames so the game HUD and windows show the local state's balance
void TwoStates_UiBegin();
void TwoStates_UiEnd();
void TwoStates_RestoreUiMoneySwap();
void TwoStates_ToolMoneyBegin(double* outSavedSimRub, double* outSavedSimUsd);
void TwoStates_ToolMoneyEnd(double savedSimRub, double savedSimUsd);

// Territory, building & vehicle ownership checks:
int  TwoStates_BuildingPosKey(float x, float z);
void TwoStates_RegisterBuildingOwner(int buildingId, int ownerState);
int  TwoStates_GetBuildingOwner(int buildingId, float x, float z);
void TwoStates_RegisterVehicleOwner(int vehicleId, int ownerState);
int  TwoStates_GetVehicleOwner(int vehicleId, const BYTE* vehiclePtr);

// Zone at world X: 1 = West territory, 0 = Shared Border Zone, 2 = East territory.
int TwoStates_GetZoneAt(float x, float z);
int TwoStates_GetStateAtWorldPos(float x, float z);
bool TwoStates_CanBuildAt(float x, float z, int state = 0);
bool TwoStates_CanDemolishAt(float x, float z, int state = 0);
bool TwoStates_CanControlBuilding(int buildingId, float x, float z, int state = 0);
bool TwoStates_CanControlVehicle(const BYTE* vehiclePtr, int state = 0);

// Checks with rate-limited Russian UI log + on-screen Toast notification when refused.
bool TwoStates_CheckBuildAllowed(float x, float z, const char* actionDesc);
bool TwoStates_CheckDemolishAllowed(float x, float z, const char* actionDesc);
bool TwoStates_CheckBuildingControlAllowed(int buildingId, float x, float z, const char* actionDesc);
bool TwoStates_CheckBuildingControlAllowed(const BYTE* buildingPtr, const char* actionDesc = nullptr);
bool TwoStates_CheckVehicleControlAllowed(const BYTE* vehiclePtr, const char* actionDesc);

// Current cursor & camera position in the world (for HUD & partner sync).
bool TwoStates_GetCursorWorldPos(float* outX, float* outY, float* outZ);
bool TwoStates_GetCameraPos(float* outX, float* outY, float* outZ);
bool TwoStates_GetCameraLookAt(float* outX, float* outY, float* outZ);
bool TwoStates_GetCameraView(float pos[3], float lookAt[3]);
bool TwoStates_ApplyCameraView(const float pos[3], const float lookAt[3]);
void TwoStates_ApplyInitialFraming(bool isHost, bool isTwoStates);
void TwoStates_CapturePreResyncCamera();
bool TwoStates_HasCapturedResyncCamera();
bool TwoStates_JumpCameraTo(float wx, float wz);
bool TwoStates_JumpCameraToPartner();

// 3D World Overlays & Toast notifications:
void TwoStates_SetShow3DBorder(bool on);
bool TwoStates_GetShow3DBorder();
void TwoStates_PingAtCursor();
void TwoStates_AddToast(const char* text, int colorKind = 0);
void TwoStates_DrawOverlay();

// Map expansion: unlock 2km border buffer on all maps (20x20 km full area)
void TwoStates_SetExpandBorders(bool on);
bool TwoStates_GetExpandBorders();
bool TwoStates_PatchSaveScriptIni(const char* saveDir, float customWorldSizeMeters = 0.0f);

// Persistent global Co-op settings (republicmp_settings.ini)
void TwoStates_LoadSettings();
void TwoStates_SaveSettings();
const char* TwoStates_GetSavedJoinAddress();
void TwoStates_SetSavedJoinAddress(const char* addr);
bool TwoStates_GetSendClientLogs();
void TwoStates_SetSendClientLogs(bool send);

// Separate Singleplayer vs Co-op Save Banks (media_soviet\save <-> save_sp / save_coop)
bool TwoStates_MountSaveBank(bool wantCoop);
bool TwoStates_IsCoopBankMounted();
bool TwoStates_GetLatestCoopSaveName(char* outName, size_t outSize);
bool TwoStates_PromoteStagingSavesInDir(const char* saveBankDir);
void TwoStates_SetImportingSpSave(bool importing);
bool TwoStates_IsImportingSpSave();

// Per-save Co-op metadata (republicmp_coop.ini) & save/load hooks
void TwoStates_BeforeAnyWorldLoad();
void TwoStates_OnAnyWorldLoaded(bool isLockstepSessionLoad);
bool TwoStates_TriggerQuickCoopSave();
bool TwoStates_IsSaveInProgress();
unsigned long long TwoStates_GetCompletedSaveCount();

// Testable domain helpers (used by runtime and unit test suite)
bool   TwoStates_IsSafeSaveScriptPath(const char* saveDir);
double TwoStates_ComputeStepWeightState1(int countState1, int countState2);
bool   TwoStates_WriteSaveMetaToDir(const char* fullSaveDir);
bool   TwoStates_ReadSaveMetaFromDir(const char* fullSaveDir, bool updateBudgetsAndMode);
float  TwoStates_CalcMaxAvailableBorrow(BYTE* world, int loanType, int state);
void   TwoStates_StepOneLoanDayForTest(BYTE* world, SovietLoan* loan);
void   TwoStates_ResetEconStatsForTest(BYTE* world);
void   TwoStates_LedgerAddForTest(BYTE* world, const SovietStatResEntry* row, BYTE* ledgerVec, float mult = 1.0f);
void   TwoStates_ConstructStepBatchForTest(BYTE* world, BYTE* building, const SovietStatResEntry* entries,
                                           const int* vecIdxs, int count);
void   TwoStates_StatRecordClearForTest(BYTE* world, BYTE* rec);
bool   TwoStates_GetStateEconResEntry(int state, int vecIdx, uintptr_t resourcePtr, float* outTons, float* outValue);
void   TwoStates_GetStateEconFloat4ForTest(int state, bool loansSlice, float outVals[4]);
int    TwoStates_GetStateEconHistoryCount(int state);
void   TwoStates_CalcStoredResourcesForTest(BYTE* world, int* outBuildingCount, int* outVehicleCount);
bool   TwoStates_GetSavedCameraForTest(int state, float outPos[3], float outLook[3]);
void   TwoStates_SetSavedCameraForTest(int state, const float pos[3], const float look[3]);
void   TwoStates_RestoreOrFrameCameraForTest(bool isHost);
void*  TwoStates_BuildRegPreservingRelayStub(void* targetCppFn, bool keepXmm0Result, bool zeroRcxOnExit);





