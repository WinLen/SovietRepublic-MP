// SPDX-License-Identifier: GPL-3.0-only
// Thematic Test Suites: Session, ModSync, Waiting Lobby, Sound/Warning Isolation,
// SettingsSync (Media/Prison/Orphanage/Alerts), and SaveSync / M9.3 Persistence.
// Suites: #6, #8, #15, #36, #37, #45, #46, #47.

#include "test_harness.h"
#include <fstream>
#include <limits>
#include <memory>

// ---------------------------------------------------------------------------
// Test 6: ModSync header.bin Binary Parser (Invariant #9) & Workshop Manager Filter (Invariant #6)
// ---------------------------------------------------------------------------

bool Test_ModSync_HeaderBinAndWorkshopFilter()
{
    ResetMockWorldMemory();

    // 1. Test ModSync_ExtractHeaderBinModIdsFromBuffer (Invariant #9: 0x260 header + 0x80 records)
    const size_t numRecs = 6;
    std::vector<BYTE> hdrBin(HEADER_BIN_DATA_OFFSET + numRecs * HEADER_BIN_RECORD_SIZE, 0);

    auto writeRec = [&](size_t idx, const char* str) {
        char* dst = (char*)(hdrBin.data() + HEADER_BIN_DATA_OFFSET + idx * HEADER_BIN_RECORD_SIZE);
        strncpy_s(dst, HEADER_BIN_RECORD_SIZE, str, _TRUNCATE);
    };

    writeRec(0, "2850123456/custom_heating_plant");
    writeRec(1, "MIRRORZ_2999888777/mirrored_bus_station");
    writeRec(2, "vanilla_building_without_slash");
    writeRec(3, "2850123456/duplicate_id_another_building");
    writeRec(4, "3823543010/republicmp_self_id_must_be_ignored");
    writeRec(5, "1911223344\\backslash_vehicle_item");

    uint64_t extracted[16] = {};
    int extractedCount = 0;
    ModSync_ExtractHeaderBinModIdsFromBuffer(hdrBin.data(), hdrBin.size(), extracted, 16, &extractedCount);

    TEST_CHECK(extractedCount == 3);
    TEST_CHECK(extracted[0] == 2850123456ULL);
    TEST_CHECK(extracted[1] == 2999888777ULL);
    TEST_CHECK(extracted[2] == 1911223344ULL);

    // Truncated buffer smaller than 0x260 + 0x80 must extract 0 records safely
    int shortCount = 0;
    ModSync_ExtractHeaderBinModIdsFromBuffer(hdrBin.data(), HEADER_BIN_DATA_OFFSET + 10, extracted, 16, &shortCount);
    TEST_CHECK(shortCount == 0);

    // 2. Test SafeFilterAndSortWorkshopMgr & SafeRestoreWorkshopMgr (Invariant #6)
    auto makeMockWorkshopItem = [](uint64_t id, int itemType, bool isSubscribed) -> std::vector<BYTE> {
        std::vector<BYTE> item(0x4900, 0);
        *(WORD*)(item.data() + ITEM_IS_SUBSCRIBED_OFF) = isSubscribed ? 1 : 0;
        *(int*)(item.data() + ITEM_TYPE_OFF)           = itemType;
        *(uint64_t*)(item.data() + ITEM_ID_QWORD_OFF)  = id;
        _snprintf_s((char*)(item.data() + ITEM_ID_STR_OFF), 0x400, _TRUNCATE, "%llu", (unsigned long long)id);
        _snprintf_s((char*)(item.data() + ITEM_NAME_OFF), 0x400, _TRUNCATE, "Mod_%llu", (unsigned long long)id);
        return item;
    };

    auto itemA = makeMockWorkshopItem(3000000003ULL, 1 /*Building*/, true);
    auto itemB = makeMockWorkshopItem(1000000001ULL, 0 /*Vehicle*/,  true);
    auto itemC = makeMockWorkshopItem(2000000002ULL, 1 /*Building*/, false);
    auto itemD = makeMockWorkshopItem(4000000004ULL, 4 /*Text localization - always kept!*/, true);
    auto itemE = makeMockWorkshopItem(1500000005ULL, 1 /*Building*/, true);

    std::vector<BYTE*> mgrItems = {
        itemA.data(), itemB.data(), itemC.data(), itemD.data(), itemE.data()
    };
    BYTE mgrHeader[24] = {};
    WriteMockVectorHeader(mgrHeader, mgrItems);

    // Allow only 3000000003 and 1000000001 (plus itemD 4000000004 which is itemType == 4 text localization)
    uint64_t allowed[2] = { 3000000003ULL, 1000000001ULL };
    int hiddenTail = 0;
    int kept = ModSync_FilterAndSortWorkshopMgrForTest(mgrHeader, allowed, 2, &hiddenTail);

    TEST_CHECK(kept == 3);
    TEST_CHECK(hiddenTail == 2); // 5 total - 3 kept = 2 preserved in tail

    BYTE** activeBegin = *(BYTE***)(mgrHeader + 0x00);
    BYTE** activeEnd   = *(BYTE***)(mgrHeader + 0x08);
    TEST_CHECK((activeEnd - activeBegin) == 3);

    // Verify ascending ID sort order in [0 .. kept - 1]: 1000000001, 3000000003, 4000000004
    TEST_CHECK(activeBegin[0] == itemB.data());
    TEST_CHECK(activeBegin[1] == itemA.data());
    TEST_CHECK(activeBegin[2] == itemD.data());

    // Verify excluded items are preserved in the tail [kept .. total) so nothing is leaked
    TEST_CHECK((activeBegin[3] == itemC.data() && activeBegin[4] == itemE.data()) ||
               (activeBegin[3] == itemE.data() && activeBegin[4] == itemC.data()));

    // Restore manager vector and verify all 5 items are back in the active vector
    ModSync_RestoreWorkshopMgrForTest(mgrHeader, &hiddenTail);
    activeEnd = *(BYTE***)(mgrHeader + 0x08);
    TEST_CHECK(hiddenTail == 0);
    TEST_CHECK((activeEnd - activeBegin) == 5);

    // 3. Test Layer 2 Mod Checks (ModSync_IsModIdAllowedInSession, ModSync_CheckBuildingTypeAllowed, ModSync_CheckVehicleTypeAllowed)
    // When filter is inactive, all are allowed
    ModSync_SetTestSessionFilter(false, nullptr, 0);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(9999999999ULL) == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("9999999999/some_building") == true);

    // Activate session filter with allowed IDs { 1000000001ULL, 3000000003ULL }
    ModSync_SetTestSessionFilter(true, allowed, 2);

    TEST_CHECK(ModSync_IsModIdAllowedInSession(1000000001ULL) == true);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(3000000003ULL) == true);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(2000000002ULL) == false);
    TEST_CHECK(ModSync_IsModIdAllowedInSession(3823543010ULL) == true); // RepublicMP mod itself is always allowed
    TEST_CHECK(ModSync_IsModIdAllowedInSession(0) == true);

    // Building type strings
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed(nullptr) == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("media_soviet/buildings/hospital") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("dlc1_school") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("3000000003/large_school") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("MIRRORZ_1000000001/bus_station") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("1000000001\\truck_depot") == true);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("9999999999/super_plant") == false);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("MIRRORZ_8888888888/factory") == false);

    // Vehicle type pointers with name at offset +0x200
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(nullptr) == true);

    std::vector<BYTE> vehVanilla(0x250, 0);
    strncpy_s((char*)(vehVanilla.data() + 0x200), 64, "media_soviet/vehicles/bus_vanilla", _TRUNCATE);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehVanilla.data()) == true);

    std::vector<BYTE> vehAllowed(0x250, 0);
    strncpy_s((char*)(vehAllowed.data() + 0x200), 64, "1000000001/trolley_ziu9", _TRUNCATE);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehAllowed.data()) == true);

    std::vector<BYTE> vehUnallowed(0x250, 0);
    strncpy_s((char*)(vehUnallowed.data() + 0x200), 64, "7777777777/electric_locomotive", _TRUNCATE);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehUnallowed.data()) == false);

    // Restore / deactivate filter
    ModSync_SetTestSessionFilter(false, nullptr, 0);
    TEST_CHECK(ModSync_CheckBuildingTypeAllowed("9999999999/super_plant") == true);
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehUnallowed.data()) == true);

    return true;
}

// ---------------------------------------------------------------------------
// Test 8: Waiting Lobby Hold, Action Lock, Map Inspect & Idempotent Settings
// ---------------------------------------------------------------------------

bool Test_WaitingLobbyAndActionLock()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Pure state matrix for SaveSync_ShouldBlockWorldEdits(sessionState, inGameplay, syncBlockActive)
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_IDLE, false, false));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_IDLE, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_IDLE, true, true));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_LISTENING, false, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_LISTENING, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_JOINING, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_CLIENT_JOINING, true, false));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_IN_GAME, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_HOST_IN_GAME, true, true));
    TEST_CHECK(!SaveSync_ShouldBlockWorldEdits((int)SESSION_CLIENT_IN_GAME, true, false));
    TEST_CHECK(SaveSync_ShouldBlockWorldEdits((int)SESSION_CLIENT_IN_GAME, true, true));

    // Mark mock world as loaded (WORLD_ROAD_NETWORK_RVA = 0xA2E758 != 0 and LOAD_TRIGGER_DAT_RVA == 0)
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 1;
    *(unsigned char*)(g_mockExeBuffer + LOAD_TRIGGER_DAT_RVA) = 0;
    TEST_CHECK(SaveSync_IsWorldLoaded());

    // 2. Waiting Lobby & Reconnect state transitions
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    TEST_CHECK(!SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(!SaveSync_IsWaitingForReconnect());

    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(!SaveSync_IsWaitingForReconnect());

    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, true, "127.0.0.1:5556");
    TEST_CHECK(SaveSync_IsWaitingLobbyActive());
    TEST_CHECK(SaveSync_IsWaitingForReconnect());
    TEST_CHECK(strcmp(SaveSync_GetLastPartnerPeer(), "127.0.0.1:5556") == 0);

    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    TEST_CHECK(!SaveSync_IsWaitingLobbyActive());

    // 3. SaveSync_EnforceWaitingLobbyHold enforces pause (speed=0, timer+12=1), preserves saved speed (+0x5A8), and clears active tool & drag
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    *(int*)(world + 0x5CC) = 2;                               // GAME_SPEED_OFFSET
    *(int*)(world + 0x5D0) = 0;                               // GAME_SPEED_SAVED_OFFSET
    *(int*)(world + 0x5D4) = 1;                               // GAME_SPEED_TURBO_OFFSET
    *(unsigned char*)(world + 0x5CA) = 1;                     // GAME_SPEED_HOLD_OFFSET
    *(unsigned char*)(g_mockExeBuffer + 0xA1EEF0 + 12) = 0;   // timer paused byte (0 = running)
    *(void**)(world + WORLD_SELECTED_TOOL_PTR_OFF) = (void*)0x12345678ULL;
    *(unsigned char*)(world + 0xD34C) = 1;                    // TOOL_DRAG_ACTIVE_OFFSET
    *(unsigned char*)(g_mockExeBuffer + 0xA9EF50) = 1;        // LMB_HELD_RVA
    *(unsigned char*)(g_mockExeBuffer + 0xA9EF51) = 1;        // LMB_JUST_PRESSED_RVA

    SaveSync_EnforceWaitingLobbyHold();

    TEST_CHECK(*(int*)(world + 0x5CC) == 0);
    TEST_CHECK(*(int*)(world + 0x5D0) == 2); // saved speed preserved
    TEST_CHECK(*(int*)(world + 0x5D4) == 0);
    TEST_CHECK(*(unsigned char*)(world + 0x5CA) == 0);
    TEST_CHECK(*(unsigned char*)(g_mockExeBuffer + 0xA1EEF0 + 12) == 1);
    TEST_CHECK(*(void**)(world + WORLD_SELECTED_TOOL_PTR_OFF) == nullptr);
    TEST_CHECK(*(unsigned char*)(world + 0xD34C) == 0);
    TEST_CHECK(*(unsigned char*)(g_mockExeBuffer + 0xA9EF50) == 0);
    TEST_CHECK(*(unsigned char*)(g_mockExeBuffer + 0xA9EF51) == 0);

    // 4. Map Inspection Mode ([Tab]) and automatic exit when partner begins joining
    SyncBlock_SetActive(false, nullptr);
    SyncBlock_SetMapInspectMode(false);
    TEST_CHECK(!SyncBlock_IsMapInspectMode());
    TEST_CHECK(SyncBlock_BlocksWorldInput());

    SyncBlock_SetMapInspectMode(true);
    TEST_CHECK(SyncBlock_IsMapInspectMode());
    TEST_CHECK(!SyncBlock_BlocksWorldInput());

    // Partner starts joining -> Map Inspection Mode must automatically exit and block input
    TEST_CHECK(!SyncBlock_UpdateMapInspectState((int)SESSION_HOST_JOINING, true));
    TEST_CHECK(!SyncBlock_IsMapInspectMode());

    // 5. Idempotent Waiting Lobby Settings re-application (never doubles money on repeated toggles)
    SaveSync_SetTestSessionState(SESSION_HOST_LISTENING, false, "");
    *(int*)(world + WORLD_UNLIMITED_MONEY_OFFSET) = 0;
    *(double*)(world + WORLD_RUB_OFFSET) = 1000000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 400000.0;
    *(unsigned char*)(g_mockExeBuffer + TERRAIN_FLAG_DAT_RVA) = 1; // new map / non-coop save
    TwoStates_OnAnyWorldLoaded(false);
    TEST_CHECK(!TwoStates_IsLoadedFromCoopSave());

    TwoStates_SetMode(COOP_MODE_TWO_STATES);
    TwoStates_SetFullStartingCapital(true);
    TwoStates_ReapplyWaitingLobbySettings();
    TwoStates_ReapplyWaitingLobbySettings();
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 2000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 800000.0, 1e-2);

    // Toggle to 50/50 split -> each gets 500k RUB / 200k USD, world total returns to 1M RUB / 400k USD
    TwoStates_SetFullStartingCapital(false);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 500000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 400000.0, 1e-2);

    // Toggle back to One State -> world total stays 1M RUB / 400k USD
    TwoStates_SetMode(COOP_MODE_ONE_STATE);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 400000.0, 1e-2);

    // Stopping the server in the Waiting Lobby before any partner joins must restore the
    // Singleplayer balance (never leaving it doubled from 100% starting capital):
    TwoStates_SetMode(COOP_MODE_TWO_STATES);
    TwoStates_SetFullStartingCapital(true);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 2000000.0, 1e-2);
    TEST_CHECK(!SaveSync_HadPartnerInSession());
    TwoStates_OnSessionEnded();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 1000000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 400000.0, 1e-2);

    // Mid-game Singleplayer balance change before opening server: TwoStates_CaptureLobbyBaseMoney()
    *(double*)(world + WORLD_RUB_OFFSET) = 650000.0;
    *(double*)(world + WORLD_USD_OFFSET) = 210000.0;
    TwoStates_CaptureLobbyBaseMoney();
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 650000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 650000.0, 1e-2);
    TwoStates_OnSessionEnded();
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 650000.0, 1e-2);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_USD_OFFSET), 210000.0, 1e-2);

    // 6. Existing Co-op save protection: TwoStates_OnSessionJoined(true, ...) must preserve
    //    TwoStates_IsLoadedFromCoopSave() and never re-split or double loaded Co-op save balances.
    std::filesystem::path coopSaveDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save" / "test_coop_lobby";
    std::error_code ec;
    std::filesystem::create_directories(coopSaveDir, ec);
    TwoStates_SetMode(COOP_MODE_TWO_STATES);
    double savedRub[2] = { 725000.0, 315000.0 };
    double savedUsd[2] = { 180000.0, 95000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, true, savedRub, savedUsd);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(coopSaveDir.string().c_str()));
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(coopSaveDir.string().c_str(), true));
    TEST_CHECK(TwoStates_IsLoadedFromCoopSave());

    double prepRub[2] = {}, prepUsd[2] = {};
    TwoStates_PrepareHostSave(false, prepRub, prepUsd);
    TwoStates_OnSessionJoined(true, TwoStates_GetMode(), TwoStates_GetBorderHalfWidth(), TwoStates_GetExpandBorders(),
                              prepRub, prepUsd);
    TEST_CHECK(TwoStates_IsLoadedFromCoopSave());
    TwoStates_SetFullStartingCapital(true);
    TwoStates_ReapplyWaitingLobbySettings();
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 725000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 315000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(1), 180000.0, 1e-2);
    TEST_CHECK_NEAR(TwoStates_GetDollars(2), 95000.0, 1e-2);

    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SyncBlock_SetActive(false, nullptr);
    TwoStates_OnSessionEnded();
    *(__int64*)(g_mockExeBuffer + 0xA2E760) = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Test 15: Remote Sound Isolation, Warning Banner Restore & Notification Filter
// ---------------------------------------------------------------------------


static bool SubTest_RemoteReplayScopeAndSnapshots()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    while (Commands_IsApplyingRemote()) {
        Commands_EndRemoteReplayScope();
    }
    TEST_CHECK(!Commands_IsApplyingRemote());

    // Set initial local state before remote command replay
    *(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    strcpy_s((char*)(world + WORLD_WARNING_TEXT_OFFSET), WORLD_WARNING_TEXT_BYTES, "Local warning");
    *(float*)(world + WORLD_WARNING_TIMER_OFFSET) = 2.5f;
    *(int32_t*)(world + WORLD_PLACEMENT_ERROR_OFFSET) = 0;

    void* dummyWinList[2] = { (void*)0x11110000ULL, (void*)0x22220000ULL };
    *(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_BEGIN_RVA) = dummyWinList;
    *(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_END_RVA)   = dummyWinList + 2;

    std::vector<BYTE> brushSoundBuf(2 * BRUSH_SOUND_REC_SIZE, 0);
    *(float*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 0.0f;
    *(uint8_t*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 0;
    *(float*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 0.35f;
    *(uint8_t*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 1;
    *(uintptr_t*)(world + WORLD_BRUSH_SOUNDS_BEGIN_OFFSET) = (uintptr_t)brushSoundBuf.data();
    *(uintptr_t*)(world + WORLD_BRUSH_SOUNDS_END_OFFSET)   = (uintptr_t)(brushSoundBuf.data() + brushSoundBuf.size());

    // Enter outer and nested remote replay scopes
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_IsApplyingRemote());
    TEST_CHECK(*(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 1);
    TEST_CHECK(*(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_END_RVA) == dummyWinList);

    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_IsApplyingRemote());

    // Simulate remote command overwriting warning banner, error code, and terrain brush loop sounds
    strcpy_s((char*)(world + WORLD_WARNING_TEXT_OFFSET), WORLD_WARNING_TEXT_BYTES, "> Remote error");
    *(float*)(world + WORLD_WARNING_TIMER_OFFSET) = 5.0f;
    *(int32_t*)(world + WORLD_PLACEMENT_ERROR_OFFSET) = 1;
    *(float*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 1.0f;
    *(uint8_t*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 1;
    *(float*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET)     = 1.0f;
    *(uint8_t*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) = 1;

    // Exit nested scope: still applying remote until outer scope exits
    Commands_EndRemoteReplayScope();
    TEST_CHECK(Commands_IsApplyingRemote());
    TEST_CHECK(*(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 1);

    // Exit outer scope: all local state must be restored
    Commands_EndRemoteReplayScope();
    TEST_CHECK(!Commands_IsApplyingRemote());
    TEST_CHECK(*(uint8_t*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 0);
    TEST_CHECK(strcmp((const char*)(world + WORLD_WARNING_TEXT_OFFSET), "Local warning") == 0);
    TEST_CHECK_NEAR(*(float*)(world + WORLD_WARNING_TIMER_OFFSET), 2.5f, 1e-5f);
    TEST_CHECK(*(int32_t*)(world + WORLD_PLACEMENT_ERROR_OFFSET) == 0);
    TEST_CHECK(*(void***)(g_mockExeBuffer + WORLD_OPEN_WINDOWS_END_RVA) == dummyWinList + 2);
    TEST_CHECK_NEAR(*(float*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET), 0.0f, 1e-5f);
    TEST_CHECK(*(uint8_t*)(brushSoundBuf.data() + 0 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) == 0);
    TEST_CHECK_NEAR(*(float*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_VOL_OFFSET), 0.35f, 1e-5f);
    TEST_CHECK(*(uint8_t*)(brushSoundBuf.data() + 1 * BRUSH_SOUND_REC_SIZE + BRUSH_SOUND_ACTIVE_OFFSET) == 1);

    return true;
}

static bool SubTest_Sound2DAnd3DDistanceHooks()
{
    ResetMockWorldMemory();
    g_mockHost.patchIat          = &MockHost_PatchIatForCommands;
    g_mockHost.installInlineHook = &MockHost_InstallInlineHookForCommands;
    Commands_Bind(&g_mockHost);
    TEST_CHECK(Commands_InstallHooks());
    g_mockHost.patchIat          = &MockHost_PatchIat;
    g_mockHost.installInlineHook = &MockHost_InstallInlineHook;

    g_mockPlaySound2DCalls = 0;
    g_mockPlaySound3DCalls = 0;

    // Set local camera eye at {1000.0f, 50.0f, 1000.0f}
    float* camEye = (float*)(g_mockExeBuffer + CAMERA_EYE_RVA);
    camEye[0] = 1000.0f;
    camEye[1] = 50.0f;
    camEye[2] = 1000.0f;

    const float nearPos[3] = { 1100.0f, 50.0f, 1000.0f }; // dist = 100m <= 250m
    const float farPos[3]  = { 2000.0f, 50.0f, 2000.0f }; // dist = 1414m > 250m

    // 1. Outside remote replay scope: 2D and both 3D sounds are forwarded
    TEST_CHECK(Commands_OnPlaySound2D(nullptr, "click", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 1);
    TEST_CHECK(Commands_OnPlaySound3D(nullptr, "build_near", nearPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 1);
    TEST_CHECK(Commands_OnPlaySound3D(nullptr, "build_far", farPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 2);

    // 2. Inside remote replay scope: 2D sound is muted; 3D near (100m <= 250m) is played; 3D far (> 250m) is muted
    Commands_BeginRemoteReplayScope();
    TEST_CHECK(!Commands_OnPlaySound2D(nullptr, "remote_click", 0, 1.0f, 0));
    TEST_CHECK(g_mockPlaySound2DCalls == 1);

    TEST_CHECK(Commands_OnPlaySound3D(nullptr, "remote_near", nearPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 3);

    TEST_CHECK(!Commands_OnPlaySound3D(nullptr, "remote_far", farPos, 0, 1.0f));
    TEST_CHECK(g_mockPlaySound3DCalls == 3);
    Commands_EndRemoteReplayScope();

    return true;
}

static bool SubTest_PopupAndForeignTerritoryNotificationFilter()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };

    // Create mock notification messages (size 0x4220 to cover NOTIF_MSG_POS_X/Z_OFFSET at +0x4200/+0x4208)
    std::vector<BYTE> msgWest(0x4220, 0), msgEast(0x4220, 0), msgGlobal(0x4220, 0);
    *(float*)(msgWest.data() + NOTIF_MSG_POS_X_OFFSET)   = -500.0f;
    *(float*)(msgWest.data() + NOTIF_MSG_POS_Z_OFFSET)   =  100.0f;
    *(float*)(msgEast.data() + NOTIF_MSG_POS_X_OFFSET)   = +500.0f;
    *(float*)(msgEast.data() + NOTIF_MSG_POS_Z_OFFSET)   =  100.0f;
    *(float*)(msgGlobal.data() + NOTIF_MSG_POS_X_OFFSET) =    0.0f;
    *(float*)(msgGlobal.data() + NOTIF_MSG_POS_Z_OFFSET) =    0.0f;

    // 1. One-State mode: remote replay scope blocks popups, outside scope all notifications are allowed
    TwoStates_OnSessionJoined(true, COOP_MODE_ONE_STATE, 250.0f, false, initRub, initUsd);
    g_mockCreatePopupCalls = 0;

    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 1, 0, 0, 0, 0, 100, 0, 0) == 0);
    TEST_CHECK(g_mockCreatePopupCalls == 0);
    Commands_EndRemoteReplayScope();

    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 2, (long long)msgEast.data(), 5, 0, 0, 0, 0, 0) == 0x7777);
    TEST_CHECK(g_mockCreatePopupCalls == 1);

    uintptr_t popupsOneState[3] = { (uintptr_t)msgWest.data(), (uintptr_t)msgEast.data(), (uintptr_t)msgGlobal.data() };
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_BEGIN_OFFSET) = popupsOneState;
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET)   = popupsOneState + 3;
    g_mockRebuildNotifCalls = 0;
    Commands_OnRebuildNotificationPopups((long long)world);
    TEST_CHECK(g_mockRebuildNotifCalls == 1);
    TEST_CHECK(*(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET) == popupsOneState + 3);

    // 2. Two-States mode (Host = State 1, West X < 0):
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    g_mockCreatePopupCalls = 0;

    Commands_BeginRemoteReplayScope();
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 1, 0, 0, 0, 0, 100, 0, 0) == 0);
    TEST_CHECK(g_mockCreatePopupCalls == 0);
    Commands_EndRemoteReplayScope();

    // East notification (+500.0f) suppressed; West notification (-500.0f) allowed
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 2, (long long)msgEast.data(), 5, 0, 0, 0, 0, 0) == 0);
    TEST_CHECK(g_mockCreatePopupCalls == 0);
    TEST_CHECK(Commands_OnCreatePopupWindow((long long)world, 2, (long long)msgWest.data(), 5, 0, 0, 0, 0, 0) == 0x7777);
    TEST_CHECK(g_mockCreatePopupCalls == 1);

    // RebuildNotificationPopups in Two-States mode removes East (+500.0f) and keeps West (-500.0f) + Global (0.0f)
    uintptr_t popupsTwoStates[3] = { (uintptr_t)msgEast.data(), (uintptr_t)msgWest.data(), (uintptr_t)msgGlobal.data() };
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_BEGIN_OFFSET) = popupsTwoStates;
    *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET)   = popupsTwoStates + 3;
    Commands_OnRebuildNotificationPopups((long long)world);
    uintptr_t* newEnd = *(uintptr_t**)(world + WORLD_NOTIF_POPUPS_END_OFFSET);
    TEST_CHECK((newEnd - popupsTwoStates) == 2);
    TEST_CHECK(popupsTwoStates[0] == (uintptr_t)msgWest.data());
    TEST_CHECK(popupsTwoStates[1] == (uintptr_t)msgGlobal.data());

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_CommandOwnerTrackingBothModes()
{
    ResetMockWorldMemory();
    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };

    // 1. COOP_MODE_ONE_STATE
    TwoStates_OnSessionJoined(true, COOP_MODE_ONE_STATE, 250.0f, false, initRub, initUsd);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);
    TwoStates_BeginCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 1);
    TwoStates_EndCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    TwoStates_BeginCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 2);
    TwoStates_EndCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    // 2. COOP_MODE_TWO_STATES
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);
    TwoStates_BeginCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 1);
    TwoStates_EndCommand(1);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    TwoStates_BeginCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 2);
    TwoStates_EndCommand(2);
    TEST_CHECK(TwoStates_GetCommandOwner() == 0);

    TwoStates_OnSessionEnded();
    return true;
}

bool Test_RemoteSoundAndWarningIsolation()
{
    return SubTest_RemoteReplayScopeAndSnapshots() &&
            SubTest_Sound2DAnd3DDistanceHooks() &&
            SubTest_PopupAndForeignTerritoryNotificationFilter() &&
            SubTest_CommandOwnerTrackingBothModes();
}

// ---------------------------------------------------------------------------
// Test 36: SettingsSync for TV/Radio Broadcast, Prison/Orphanage Upbringing, and Building Alerts
// ---------------------------------------------------------------------------

static bool SubTest_SettingsSync_OwnsBuildingWord()
{
    std::vector<BYTE> typeOrph(0x400, 0), typeSite(0x400, 0), typePoll(0x400, 0);
    *(int*)(typeOrph.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    *(int*)(typeSite.data() + BLD_TYPE_CAT_OFF) = 0x01;
    *(int*)(typePoll.data() + BLD_TYPE_CAT_OFF) = 0x24;

    std::vector<BYTE> bldOrph(BUILDING_STRUCT_BYTES, 0), bldSite(BUILDING_STRUCT_BYTES, 0), bldPoll(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldOrph.data() + BLD_TYPE_OFFSET) = typeOrph.data();
    *(BYTE**)(bldSite.data() + BLD_TYPE_OFFSET) = typeSite.data();
    *(BYTE**)(bldPoll.data() + BLD_TYPE_OFFSET) = typePoll.data();

    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldOrph.data(), BLD_UPBRINGING_SLIDERS_OFFSET));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldOrph.data(), BLD_UPBRINGING_SLIDERS_OFFSET + 20));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldOrph.data(), BLD_MIN_LOYALTY_EMPLOYEES_OFFSET));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldOrph.data(), BLD_MIN_LOYALTY_CITIZENS_OFFSET));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldOrph.data(), BLD_ALERT_GOODS_MISSING_OFFSET));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldOrph.data(), BLD_ALERT_MAIN_TOGGLE_OFFSET));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldSite.data(), BLD_ALERT_FINISHED_OFFSET));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldPoll.data(), BLD_POLLUTION_ALERT_TOGGLE_OFFSET));
    TEST_CHECK(SettingsSync_OwnsBuildingWord(bldPoll.data(), BLD_POLLUTION_ALERT_THRESH_OFFSET));
    TEST_CHECK(!SettingsSync_OwnsBuildingWord(bldOrph.data(), BLD_POLLUTION_ALERT_TOGGLE_OFFSET));
    TEST_CHECK(!SettingsSync_OwnsBuildingWord(bldOrph.data(), 0x500));
    return true;
}

static bool SubTest_SettingsSync_UpbringingComposite()
{
    ResetMockWorldMemory();
    std::vector<BYTE> typeOrph(0x400, 0);
    *(int*)(typeOrph.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    std::vector<BYTE> bldOrph(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldOrph.data() + BLD_TYPE_OFFSET) = typeOrph.data();
    float posOrph[3] = { 100.0f, 20.0f, 200.0f };
    memcpy(bldOrph.data() + OBJ_POS_OFFSET, posOrph, sizeof(posOrph));

    std::vector<BYTE*> mockBlds = { bldOrph.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    SettingWire w = {};
    w.field = SETTINGS_FIELD_UPBRINGING;
    w.category = 0x4A;
    memcpy(w.pos, posOrph, sizeof(posOrph));
    w.count = 6;
    float target[6] = { 0.15f, 0.20f, 0.10f, 0.25f, 0.20f, 0.10f };
    memcpy(w.data, target, sizeof(target));

    TEST_CHECK(SettingsSync_ApplyCommand(w));
    const float* applied = (const float*)(bldOrph.data() + BLD_UPBRINGING_SLIDERS_OFFSET);
    for (int i = 0; i < 6; i++) {
        TEST_CHECK_NEAR(applied[i], target[i], 1e-5f);
    }

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_SettingsSync_BroadcastComposite()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> typeMedia(0x400, 0);
    *(int*)(typeMedia.data() + BLD_TYPE_CAT_OFF) = 0x25;
    std::vector<BYTE> bldMedia(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldMedia.data() + BLD_TYPE_OFFSET) = typeMedia.data();
    float posMedia[3] = { 300.0f, 10.0f, 400.0f };
    memcpy(bldMedia.data() + OBJ_POS_OFFSET, posMedia, sizeof(posMedia));

    std::vector<BYTE*> mockBlds = { bldMedia.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    std::vector<BYTE> studioObj(0x180, 0);
    *(BYTE**)(studioObj.data() + STUDIO_BUILDING_OFFSET) = bldMedia.data();
    std::vector<BYTE*> mockStudios = { studioObj.data() };
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, mockStudios);

    SettingWire w = {};
    w.field = SETTINGS_FIELD_BROADCAST;
    w.category = 0x25;
    memcpy(w.pos, posMedia, sizeof(posMedia));
    w.count = 6;
    float target[6] = { 0.05f, 0.15f, 0.30f, 0.20f, 0.20f, 0.10f };
    memcpy(w.data, target, sizeof(target));

    TEST_CHECK(SettingsSync_ApplyCommand(w));
    const float* applied = (const float*)(studioObj.data() + STUDIO_BROADCAST_SLIDERS_OFFSET);
    for (int i = 0; i < 6; i++) {
        TEST_CHECK_NEAR(applied[i], target[i], 1e-5f);
    }

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    mockStudios.clear();
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, mockStudios);
    return true;
}

static bool SubTest_SettingsSync_CompositeSanitizationAndClamping()
{
    ResetMockWorldMemory();
    std::vector<BYTE> typeOrph(0x400, 0);
    *(int*)(typeOrph.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    std::vector<BYTE> bldOrph(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldOrph.data() + BLD_TYPE_OFFSET) = typeOrph.data();
    float posOrph[3] = { 100.0f, 20.0f, 200.0f };
    memcpy(bldOrph.data() + OBJ_POS_OFFSET, posOrph, sizeof(posOrph));

    std::vector<BYTE*> mockBlds = { bldOrph.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    // Out of range clamped
    SettingWire w = {};
    w.field = SETTINGS_FIELD_UPBRINGING;
    w.category = 0x4A;
    memcpy(w.pos, posOrph, sizeof(posOrph));
    w.count = 6;
    float clampedFloats[6] = { -0.5f, 1.5f, 0.4f, -10.0f, 10.0f, 0.2f };
    memcpy(w.data, clampedFloats, sizeof(clampedFloats));

    TEST_CHECK(SettingsSync_ApplyCommand(w));
    const float* applied = (const float*)(bldOrph.data() + BLD_UPBRINGING_SLIDERS_OFFSET);
    TEST_CHECK_NEAR(applied[0], 0.0f, 1e-5f);
    TEST_CHECK_NEAR(applied[1], 1.0f, 1e-5f);
    TEST_CHECK_NEAR(applied[2], 0.4f, 1e-5f);
    TEST_CHECK_NEAR(applied[3], 0.0f, 1e-5f);
    TEST_CHECK_NEAR(applied[4], 1.0f, 1e-5f);
    TEST_CHECK_NEAR(applied[5], 0.2f, 1e-5f);

    // NaN rejected
    float beforeNan[6];
    memcpy(beforeNan, applied, sizeof(beforeNan));
    float nanFloats[6] = { 0.1f, NAN, 0.2f, 0.3f, 0.2f, 0.2f };
    memcpy(w.data, nanFloats, sizeof(nanFloats));
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    for (int i = 0; i < 6; i++) {
        TEST_CHECK_NEAR(applied[i], beforeNan[i], 1e-5f);
    }

    // Inf rejected
    float infFloats[6] = { 0.1f, INFINITY, 0.2f, 0.3f, 0.2f, 0.2f };
    memcpy(w.data, infFloats, sizeof(infFloats));
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    for (int i = 0; i < 6; i++) {
        TEST_CHECK_NEAR(applied[i], beforeNan[i], 1e-5f);
    }

    // Wrong count rejected
    w.count = 3;
    float wrongCount[6] = { 0.2f, 0.2f, 0.2f, 0.2f, 0.1f, 0.1f };
    memcpy(w.data, wrongCount, sizeof(wrongCount));
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    for (int i = 0; i < 6; i++) {
        TEST_CHECK_NEAR(applied[i], beforeNan[i], 1e-5f);
    }

    // Negative zero & subnormal normalized to clean +0.0f (sign bit 0)
    w.count = 6;
    float negZeroFloats[6] = { -0.0f, 1e-40f, 0.5f, 0.2f, 0.1f, 0.2f };
    memcpy(w.data, negZeroFloats, sizeof(negZeroFloats));
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    TEST_CHECK_NEAR(applied[0], 0.0f, 1e-5f);
    TEST_CHECK_NEAR(applied[1], 0.0f, 1e-5f);
    uint32_t bit0 = 0, bit1 = 0;
    memcpy(&bit0, &applied[0], sizeof(uint32_t));
    memcpy(&bit1, &applied[1], sizeof(uint32_t));
    TEST_CHECK(bit0 == 0);
    TEST_CHECK(bit1 == 0);

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_SettingsSync_ScalarAlertsAndLoyalty()
{
    ResetMockWorldMemory();
    std::vector<BYTE> typeOrph(0x400, 0), typeSite(0x400, 0), typePoll(0x400, 0);
    *(int*)(typeOrph.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    *(int*)(typeSite.data() + BLD_TYPE_CAT_OFF) = 0x01;
    *(int*)(typePoll.data() + BLD_TYPE_CAT_OFF) = 0x24;

    std::vector<BYTE> bldOrph(BUILDING_STRUCT_BYTES, 0), bldSite(BUILDING_STRUCT_BYTES, 0), bldPoll(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldOrph.data() + BLD_TYPE_OFFSET) = typeOrph.data();
    *(BYTE**)(bldSite.data() + BLD_TYPE_OFFSET) = typeSite.data();
    *(BYTE**)(bldPoll.data() + BLD_TYPE_OFFSET) = typePoll.data();

    float posOrph[3] = { 100.0f, 20.0f, 200.0f };
    float posSite[3] = { 700.0f, 5.0f, 800.0f };
    float posPoll[3] = { 500.0f, 15.0f, 600.0f };
    memcpy(bldOrph.data() + OBJ_POS_OFFSET, posOrph, sizeof(posOrph));
    memcpy(bldSite.data() + OBJ_POS_OFFSET, posSite, sizeof(posSite));
    memcpy(bldPoll.data() + OBJ_POS_OFFSET, posPoll, sizeof(posPoll));

    std::vector<BYTE*> mockBlds = { bldOrph.data(), bldSite.data(), bldPoll.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    int fMain = -1, fGoods = -1, fFin = -1, fPolTog = -1, fPolThr = -1, fLoyal = -1;
    for (int f = 0; f < 32; f++) {
        SettingWire tw = {};
        tw.category = 0x4A;
        memcpy(tw.pos, posOrph, sizeof(posOrph));
        tw.field = f;
        tw.value = 1;
        bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] = 0;
        if (SettingsSync_ApplyCommand(tw) && bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] == 1) {
            fMain = f;
            bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] = 0;
        }
        bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] = 0;
        if (SettingsSync_ApplyCommand(tw) && bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] == 1) {
            fGoods = f;
            bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] = 0;
        }
    }
    TEST_CHECK(fMain >= 0 && fGoods >= 0);

    for (int f = 0; f < 32; f++) {
        SettingWire tw = {};
        tw.category = 0x01;
        memcpy(tw.pos, posSite, sizeof(posSite));
        tw.field = f;
        tw.value = 1;
        bldSite[BLD_ALERT_FINISHED_OFFSET] = 0;
        if (SettingsSync_ApplyCommand(tw) && bldSite[BLD_ALERT_FINISHED_OFFSET] == 1) {
            fFin = f;
            bldSite[BLD_ALERT_FINISHED_OFFSET] = 0;
            break;
        }
    }
    TEST_CHECK(fFin >= 0);

    for (int f = 0; f < 32; f++) {
        SettingWire tw = {};
        tw.category = 0x24;
        memcpy(tw.pos, posPoll, sizeof(posPoll));
        tw.field = f;
        tw.value = 1;
        *(int32_t*)(bldPoll.data() + BLD_POLLUTION_ALERT_TOGGLE_OFFSET) = 0;
        if (SettingsSync_ApplyCommand(tw) && *(int32_t*)(bldPoll.data() + BLD_POLLUTION_ALERT_TOGGLE_OFFSET) == 1) {
            fPolTog = f;
            break;
        }
    }
    TEST_CHECK(fPolTog >= 0);

    float testThresh = 45.5f;
    unsigned long long rawThresh = 0;
    memcpy(&rawThresh, &testThresh, sizeof(float));
    for (int f = 0; f < 32; f++) {
        SettingWire tw = {};
        tw.category = 0x24;
        memcpy(tw.pos, posPoll, sizeof(posPoll));
        tw.field = f;
        tw.value = rawThresh;
        *(float*)(bldPoll.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET) = 0.0f;
        if (SettingsSync_ApplyCommand(tw) && *(float*)(bldPoll.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET) == 45.5f) {
            fPolThr = f;
            break;
        }
    }
    TEST_CHECK(fPolThr >= 0);

    float testLoyal = 0.65f;
    unsigned long long rawLoyal = 0;
    memcpy(&rawLoyal, &testLoyal, sizeof(float));
    for (int f = 0; f < 32; f++) {
        SettingWire tw = {};
        tw.category = 0x4A;
        memcpy(tw.pos, posOrph, sizeof(posOrph));
        tw.field = f;
        tw.value = rawLoyal;
        *(float*)(bldOrph.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.0f;
        if (SettingsSync_ApplyCommand(tw) && *(float*)(bldOrph.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) == 0.65f) {
            fLoyal = f;
            break;
        }
    }
    TEST_CHECK(fLoyal >= 0);

    SettingWire w = {};
    w.category = 0x4A;
    memcpy(w.pos, posOrph, sizeof(posOrph));
    w.field = fMain;
    w.value = 2;
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    TEST_CHECK(bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] == 2);

    w.field = fGoods;
    w.value = 1;
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    TEST_CHECK(bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] == 1);

    w.category = 0x01;
    memcpy(w.pos, posSite, sizeof(posSite));
    w.field = fFin;
    w.value = 1;
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    TEST_CHECK(bldSite[BLD_ALERT_FINISHED_OFFSET] == 1);

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_SettingsSync_EdgeCases()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> typeMedia(0x400, 0);
    *(int*)(typeMedia.data() + BLD_TYPE_CAT_OFF) = 0x25;
    std::vector<BYTE> bldMedia(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldMedia.data() + BLD_TYPE_OFFSET) = typeMedia.data();
    float posMedia[3] = { 300.0f, 10.0f, 400.0f };
    memcpy(bldMedia.data() + OBJ_POS_OFFSET, posMedia, sizeof(posMedia));

    std::vector<BYTE*> mockBlds = { bldMedia.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    SettingWire w = {};
    w.field = SETTINGS_FIELD_UPBRINGING;
    w.category = 0x4A;
    w.pos[0] = 9999.0f; w.pos[1] = 0.0f; w.pos[2] = 9999.0f;
    w.count = 6;
    float data[6] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.5f };
    memcpy(w.data, data, sizeof(data));
    TEST_CHECK(!SettingsSync_ApplyCommand(w));

    w.pos[0] = posMedia[0]; w.pos[1] = posMedia[1]; w.pos[2] = posMedia[2];
    w.category = 0x4A;
    TEST_CHECK(!SettingsSync_ApplyCommand(w));

    w.field = SETTINGS_FIELD_BROADCAST;
    w.category = 0x25;
    std::vector<BYTE*> emptyStudios;
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, emptyStudios);
    TEST_CHECK(SettingsSync_ApplyCommand(w));

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_SettingsSync_LoyaltySerializationAndBoundaries()
{
    ResetMockWorldMemory();
    std::vector<BYTE> typeBld(0x400, 0);
    *(int*)(typeBld.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bld.data() + BLD_TYPE_OFFSET) = typeBld.data();
    float pos[3] = { 250.0f, 15.0f, 350.0f };
    memcpy(bld.data() + OBJ_POS_OFFSET, pos, sizeof(pos));

    std::vector<BYTE*> mockBlds = { bld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    // Locate field indices for employee loyalty and citizen loyalty
    int fEmp = -1;
    int fCit = -1;
    for (int f = 0; f < 32; ++f) {
        float probe = 0.77f;
        unsigned long long rawProbe = 0;
        memcpy(&rawProbe, &probe, sizeof(float));

        SettingWire tw = {};
        tw.category = 0x4A;
        memcpy(tw.pos, pos, sizeof(pos));
        tw.field = f;
        tw.value = rawProbe;

        *(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.0f;
        *(float*)(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET) = 0.0f;

        if (SettingsSync_ApplyCommand(tw)) {
            if (fabsf(*(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) - 0.77f) < 1e-4f) {
                fEmp = f;
            }
            if (fabsf(*(float*)(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET) - 0.77f) < 1e-4f) {
                fCit = f;
            }
        }
    }
    TEST_CHECK(fEmp >= 0);
    TEST_CHECK(fCit >= 0);
    TEST_CHECK(fEmp != fCit);

    // 1. Test normal values serialization & deserialization round-trip
    const float kValidValues[] = { 0.0f, 0.15f, 0.33f, 0.50f, 0.85f, 1.0f };
    for (float val : kValidValues) {
        unsigned long long rawVal = 0;
        memcpy(&rawVal, &val, sizeof(float));

        SettingWire w = {};
        w.category = 0x4A;
        memcpy(w.pos, pos, sizeof(pos));
        w.field = fEmp;
        w.value = rawVal;
        TEST_CHECK(SettingsSync_ApplyCommand(w));
        float appliedEmp = *(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET);
        TEST_CHECK_NEAR(appliedEmp, val, 1e-5f);

        w.field = fCit;
        TEST_CHECK(SettingsSync_ApplyCommand(w));
        float appliedCit = *(float*)(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET);
        TEST_CHECK_NEAR(appliedCit, val, 1e-5f);
    }

    // 2. Test boundary handling & clamping: negative, > 1.0f, NaN, +Inf, -Inf, -0.0f
    struct BoundaryCase {
        float input;
        float expected;
    };
    BoundaryCase cases[] = {
        { -0.0f, 0.0f },
        { -0.5f, 0.0f },
        { -100.0f, 0.0f },
        { 1.05f, 1.0f },
        { 2.5f, 1.0f },
        { 100.0f, 1.0f },
        { NAN, 0.0f },
        { (float)INFINITY, 1.0f },
        { (float)(-INFINITY), 0.0f }
    };
    for (const auto& bc : cases) {
        unsigned long long rawVal = 0;
        memcpy(&rawVal, &bc.input, sizeof(float));

        SettingWire w = {};
        w.category = 0x4A;
        memcpy(w.pos, pos, sizeof(pos));
        w.field = fEmp;
        w.value = rawVal;
        TEST_CHECK(SettingsSync_ApplyCommand(w));
        float appliedEmp = *(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET);
        TEST_CHECK_NEAR(appliedEmp, bc.expected, 1e-5f);
        if (bc.expected == 0.0f) {
            uint32_t empBits = 0;
            memcpy(&empBits, &appliedEmp, sizeof(uint32_t));
            TEST_CHECK(empBits == 0); // Must be clean +0.0f with sign bit cleared
        }

        w.field = fCit;
        TEST_CHECK(SettingsSync_ApplyCommand(w));
        float appliedCit = *(float*)(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET);
        TEST_CHECK_NEAR(appliedCit, bc.expected, 1e-5f);
        if (bc.expected == 0.0f) {
            uint32_t citBits = 0;
            memcpy(&citBits, &appliedCit, sizeof(uint32_t));
            TEST_CHECK(citBits == 0); // Must be clean +0.0f with sign bit cleared
        }
    }

    // 3. Engine buildings_game.bin binary serialization/deserialization simulation
    // Building::Save (RVA 0x1EDC40) packs BLD_MIN_LOYALTY_EMPLOYEES_OFFSET into chunk 2 offset +0x68
    // and BLD_MIN_LOYALTY_CITIZENS_OFFSET into chunk 2 offset +0xA0.
    // Building::Load (RVA 0x1F0BE0) reads 0x190 bytes for version >= 109 and unpacks back.
    const size_t kChunk2Size = 0x190;
    const size_t kSaveEmpLoyaltyOffset = 0x68;
    const size_t kSaveCitLoyaltyOffset = 0xA0;
    std::vector<BYTE> saveChunk(kChunk2Size, 0);

    float sourceEmpLoyalty = 0.72f;
    float sourceCitLoyalty = 0.48f;
    *(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = sourceEmpLoyalty;
    *(float*)(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET) = sourceCitLoyalty;

    memcpy(saveChunk.data() + kSaveEmpLoyaltyOffset, bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET, sizeof(float));
    memcpy(saveChunk.data() + kSaveCitLoyaltyOffset, bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET, sizeof(float));

    *(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.0f;
    *(float*)(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET) = 0.0f;

    memcpy(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET, saveChunk.data() + kSaveEmpLoyaltyOffset, sizeof(float));
    memcpy(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET, saveChunk.data() + kSaveCitLoyaltyOffset, sizeof(float));

    TEST_CHECK_NEAR(*(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET), sourceEmpLoyalty, 1e-6f);
    TEST_CHECK_NEAR(*(float*)(bld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET), sourceCitLoyalty, 1e-6f);

    // 4. Demolished / Corrupted building pointers resilience
    BYTE* badBld = (BYTE*)0xBAADF00DDEADBEEFull;
    std::vector<BYTE*> corruptedBlds = { badBld, nullptr, bld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, corruptedBlds);

    SettingWire wBad = {};
    wBad.category = 0x4A;
    memcpy(wBad.pos, pos, sizeof(pos));
    wBad.field = fEmp;
    float testVal = 0.62f;
    memcpy(&wBad.value, &testVal, sizeof(float));

    // Applying must safely skip invalid pointers without throwing SEH or crashing,
    // find the valid building, and apply the setting cleanly.
    TEST_CHECK(SettingsSync_ApplyCommand(wBad));
    TEST_CHECK_NEAR(*(float*)(bld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET), 0.62f, 1e-5f);

    // Querying a non-existent building coordinates with corrupted vector entries must return false cleanly
    float nonExistentPos[3] = { 9999.0f, 0.0f, 9999.0f };
    memcpy(wBad.pos, nonExistentPos, sizeof(nonExistentPos));
    TEST_CHECK(!SettingsSync_ApplyCommand(wBad));

    // 5. Legacy save version (< 109) compatibility simulation
    // In SOVIET64.exe (0x1F0D28), save version is checked against version 109 (0x6D).
    // Pre-109 legacy saves did not contain Chunk 2; the engine skipped reading Chunk 2
    // and left fields at their constructor defaults (0.0f, i.e. unrestricted).
    std::vector<BYTE> legacyBld(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(legacyBld.data() + BLD_TYPE_OFFSET) = typeBld.data();
    *(float*)(legacyBld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.0f;
    *(float*)(legacyBld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET) = 0.0f;

    TEST_CHECK(*(float*)(legacyBld.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) == 0.0f);
    TEST_CHECK(*(float*)(legacyBld.data() + BLD_MIN_LOYALTY_CITIZENS_OFFSET) == 0.0f);

    // 6. Pollution alert threshold & water quality float clamping & negative-zero checks
    std::vector<BYTE> pollBld(BUILDING_STRUCT_BYTES, 0), waterBld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> typePoll(0x400, 0), typeWater(0x400, 0);
    *(int*)(typePoll.data() + BLD_TYPE_CAT_OFF) = 0x24;
    *(int*)(typeWater.data() + BLD_TYPE_CAT_OFF) = 0x61;
    *(BYTE**)(pollBld.data() + BLD_TYPE_OFFSET) = typePoll.data();
    *(BYTE**)(waterBld.data() + BLD_TYPE_OFFSET) = typeWater.data();
    float pollPos[3] = { 400.0f, 10.0f, 400.0f };
    float waterPos[3] = { 500.0f, 10.0f, 500.0f };
    memcpy(pollBld.data() + OBJ_POS_OFFSET, pollPos, sizeof(pollPos));
    memcpy(waterBld.data() + OBJ_POS_OFFSET, waterPos, sizeof(waterPos));
    std::vector<BYTE*> extraBlds = { pollBld.data(), waterBld.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, extraBlds);

    int fPoll = -1, fWater = -1;
    for (int f = 0; f < 32; f++) {
        SettingWire tw = {};
        tw.category = 0x24;
        memcpy(tw.pos, pollPos, sizeof(pollPos));
        tw.field = f;
        float probeVal = 55.0f;
        memcpy(&tw.value, &probeVal, sizeof(float));
        *(float*)(pollBld.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET) = 0.0f;
        if (SettingsSync_ApplyCommand(tw) && *(float*)(pollBld.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET) == 55.0f) {
            fPoll = f;
            break;
        }
    }
    for (int f = 0; f < 32; f++) {
        SettingWire tw = {};
        tw.category = 0x61;
        memcpy(tw.pos, waterPos, sizeof(waterPos));
        tw.field = f;
        float probeVal = 0.75f;
        memcpy(&tw.value, &probeVal, sizeof(float));
        *(float*)(waterBld.data() + 0x1340) = 0.0f;
        if (SettingsSync_ApplyCommand(tw) && *(float*)(waterBld.data() + 0x1340) == 0.75f) {
            fWater = f;
            break;
        }
    }
    TEST_CHECK(fPoll >= 0);
    TEST_CHECK(fWater >= 0);

    // Negative zero & boundary clamping on pollution & water quality
    SettingWire wPoll = {};
    wPoll.category = 0x24;
    memcpy(wPoll.pos, pollPos, sizeof(pollPos));
    wPoll.field = fPoll;
    float badPoll = -0.0f;
    memcpy(&wPoll.value, &badPoll, sizeof(float));
    TEST_CHECK(SettingsSync_ApplyCommand(wPoll));
    float polRes = *(float*)(pollBld.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET);
    TEST_CHECK_NEAR(polRes, 0.0f, 1e-5f);
    uint32_t polBits = 0;
    memcpy(&polBits, &polRes, sizeof(uint32_t));
    TEST_CHECK(polBits == 0); // Sign bit cleared

    badPoll = 250.0f; // Exceeds 100.0f cap
    memcpy(&wPoll.value, &badPoll, sizeof(float));
    TEST_CHECK(SettingsSync_ApplyCommand(wPoll));
    TEST_CHECK_NEAR(*(float*)(pollBld.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET), 100.0f, 1e-5f);

    SettingWire wWater = {};
    wWater.category = 0x61;
    memcpy(wWater.pos, waterPos, sizeof(waterPos));
    wWater.field = fWater;
    float badWater = -0.0f;
    memcpy(&wWater.value, &badWater, sizeof(float));
    TEST_CHECK(SettingsSync_ApplyCommand(wWater));
    float watRes = *(float*)(waterBld.data() + 0x1340);
    TEST_CHECK_NEAR(watRes, 0.0f, 1e-5f);
    uint32_t watBits = 0;
    memcpy(&watBits, &watRes, sizeof(uint32_t));
    TEST_CHECK(watBits == 0); // Sign bit cleared

    badWater = 1.5f; // Exceeds 1.0f cap
    memcpy(&wWater.value, &badWater, sizeof(float));
    TEST_CHECK(SettingsSync_ApplyCommand(wWater));
    TEST_CHECK_NEAR(*(float*)(waterBld.data() + 0x1340), 1.0f, 1e-5f);

    // 7. CopyBuildings resilience during Lockstep Baseline & Detect:
    // Place a corrupted pointer between valid bld1 and bld2.
    // Verify that SettingsSync_LockstepBaseline skips the bad pointer,
    // continues scanning, tracks bld2, and detects loyalty modifications on bld2.
    std::vector<BYTE> bld1(BUILDING_STRUCT_BYTES, 0), bld2(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bld1.data() + BLD_TYPE_OFFSET) = typeBld.data();
    *(BYTE**)(bld2.data() + BLD_TYPE_OFFSET) = typeBld.data();
    float pos1[3] = { -100.0f, 10.0f, -100.0f };
    float pos2[3] = { -200.0f, 10.0f, -200.0f };
    memcpy(bld1.data() + OBJ_POS_OFFSET, pos1, sizeof(pos1));
    memcpy(bld2.data() + OBJ_POS_OFFSET, pos2, sizeof(pos2));
    *(float*)(bld1.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.20f;
    *(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.30f;

    std::vector<BYTE*> mixedBlds = { bld1.data(), badBld, nullptr, bld2.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mixedBlds);

    double testRub[2] = { 500000.0, 500000.0 };
    double testUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, testRub, testUsd);
    Commands_SetSession(COMMANDS_HOST);
    SettingsSync_LockstepBaseline();

    // Modify loyalty on bld2 (located after bad pointer)
    *(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET) = 0.85f;
    SettingsSync_LockstepDetect();

    // Local memory should be reverted to baseline (0.30f) until tick boundary
    TEST_CHECK_NEAR(*(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET), 0.30f, 1e-5f);

    // Execute boundary
    TEST_CHECK(Commands_Due(1));
    Commands_Boundary(1);

    // After command boundary, bld2 must have 0.85f applied
    TEST_CHECK_NEAR(*(float*)(bld2.data() + BLD_MIN_LOYALTY_EMPLOYEES_OFFSET), 0.85f, 1e-5f);

    SettingsSync_LockstepReset();
    Commands_SetSession(COMMANDS_OFF);
    TwoStates_OnSessionEnded();

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

bool Test_SettingsSync_MediaPrisonOrphanageAlerts()
{
    printf("    [SubTest 1: SettingsSync_OwnsBuildingWord boundaries] ...\n"); fflush(stdout);
    if (!SubTest_SettingsSync_OwnsBuildingWord()) return false;
    printf("    [SubTest 2: Upbringing Composite 6-Float Sync] ...\n"); fflush(stdout);
    if (!SubTest_SettingsSync_UpbringingComposite()) return false;
    printf("    [SubTest 3: Broadcast Composite 6-Float Sync] ...\n"); fflush(stdout);
    if (!SubTest_SettingsSync_BroadcastComposite()) return false;
    printf("    [SubTest 4: Composite Sanitization & Clamping] ...\n"); fflush(stdout);
    if (!SubTest_SettingsSync_CompositeSanitizationAndClamping()) return false;
    printf("    [SubTest 5: Scalar Alerts & Employee Loyalty Sync] ...\n"); fflush(stdout);
    if (!SubTest_SettingsSync_ScalarAlertsAndLoyalty()) return false;
    printf("    [SubTest 6: Loyalty Serialization & Boundary Clamping] ...\n"); fflush(stdout);
    if (!SubTest_SettingsSync_LoyaltySerializationAndBoundaries()) return false;
    printf("    [SubTest 7: SettingsSync Edge Cases & Rejections] ...\n"); fflush(stdout);
    if (!SubTest_SettingsSync_EdgeCases()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Challenger Gen6: Empirical Stress Testing & Adversarial Verification
// ---------------------------------------------------------------------------

static bool SubTest_Gen6_FloatFuzzingUpbringingAndBroadcast()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> typeOrph(0x400, 0), typePrison(0x400, 0), typeMedia(0x400, 0);
    *(int*)(typeOrph.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    *(int*)(typePrison.data() + BLD_TYPE_CAT_OFF) = 0x4C;
    *(int*)(typeMedia.data() + BLD_TYPE_CAT_OFF) = 0x25;

    std::vector<BYTE> bldOrph(BUILDING_STRUCT_BYTES, 0), bldPrison(BUILDING_STRUCT_BYTES, 0), bldMedia(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldOrph.data() + BLD_TYPE_OFFSET) = typeOrph.data();
    *(BYTE**)(bldPrison.data() + BLD_TYPE_OFFSET) = typePrison.data();
    *(BYTE**)(bldMedia.data() + BLD_TYPE_OFFSET) = typeMedia.data();

    float posOrph[3] = { 100.0f, 20.0f, 200.0f };
    float posPrison[3] = { 200.0f, 20.0f, 300.0f };
    float posMedia[3] = { 300.0f, 20.0f, 400.0f };
    memcpy(bldOrph.data() + OBJ_POS_OFFSET, posOrph, sizeof(posOrph));
    memcpy(bldPrison.data() + OBJ_POS_OFFSET, posPrison, sizeof(posPrison));
    memcpy(bldMedia.data() + OBJ_POS_OFFSET, posMedia, sizeof(posMedia));

    std::vector<BYTE*> mockBlds = { bldOrph.data(), bldPrison.data(), bldMedia.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    std::vector<BYTE> studioObj(0x180, 0);
    *(BYTE**)(studioObj.data() + STUDIO_BUILDING_OFFSET) = bldMedia.data();
    std::vector<BYTE*> mockStudios = { studioObj.data() };
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, mockStudios);

    float baseUpbringing[6] = { 0.2f, 0.2f, 0.2f, 0.2f, 0.1f, 0.1f };
    memcpy(bldOrph.data() + BLD_UPBRINGING_SLIDERS_OFFSET, baseUpbringing, sizeof(baseUpbringing));
    memcpy(bldPrison.data() + BLD_UPBRINGING_SLIDERS_OFFSET, baseUpbringing, sizeof(baseUpbringing));
    memcpy(studioObj.data() + STUDIO_BROADCAST_SLIDERS_OFFSET, baseUpbringing, sizeof(baseUpbringing));

    // 1. Fuzzing with quiet NaN, signaling NaN, -NaN across all 6 positions
    float qnan = std::numeric_limits<float>::quiet_NaN();
    float snan = std::numeric_limits<float>::signaling_NaN();
    float negNan = -qnan;
    const float nanVariants[] = { qnan, snan, negNan };

    for (float badNan : nanVariants) {
        for (int posIdx = 0; posIdx < 6; ++posIdx) {
            SettingWire w = {};
            w.field = SETTINGS_FIELD_UPBRINGING;
            w.category = 0x4A;
            memcpy(w.pos, posOrph, sizeof(posOrph));
            w.count = 6;
            for (int i = 0; i < 6; ++i) w.data[i] = (i == posIdx) ? badNan : 0.3f;

            TEST_CHECK(SettingsSync_ApplyCommand(w));
            const float* mem = (const float*)(bldOrph.data() + BLD_UPBRINGING_SLIDERS_OFFSET);
            for (int i = 0; i < 6; ++i) {
                TEST_CHECK_NEAR(mem[i], baseUpbringing[i], 1e-5f);
            }

            w.field = SETTINGS_FIELD_BROADCAST;
            w.category = 0x25;
            memcpy(w.pos, posMedia, sizeof(posMedia));
            TEST_CHECK(SettingsSync_ApplyCommand(w));
            const float* smem = (const float*)(studioObj.data() + STUDIO_BROADCAST_SLIDERS_OFFSET);
            for (int i = 0; i < 6; ++i) {
                TEST_CHECK_NEAR(smem[i], baseUpbringing[i], 1e-5f);
            }
        }
    }

    // 2. Fuzzing with +Infinity and -Infinity across all positions
    const float infVariants[] = { (float)INFINITY, (float)(-INFINITY) };
    for (float badInf : infVariants) {
        for (int posIdx = 0; posIdx < 6; ++posIdx) {
            SettingWire w = {};
            w.field = SETTINGS_FIELD_UPBRINGING;
            w.category = 0x4C; // Prison
            memcpy(w.pos, posPrison, sizeof(posPrison));
            w.count = 6;
            for (int i = 0; i < 6; ++i) w.data[i] = (i == posIdx) ? badInf : 0.25f;

            TEST_CHECK(SettingsSync_ApplyCommand(w));
            const float* mem = (const float*)(bldPrison.data() + BLD_UPBRINGING_SLIDERS_OFFSET);
            for (int i = 0; i < 6; ++i) {
                TEST_CHECK_NEAR(mem[i], baseUpbringing[i], 1e-5f);
            }
        }
    }

    // 3. Fuzzing with denormals / subnormals
    float denormMin = std::numeric_limits<float>::denorm_min();
    float negDenorm = -denormMin;
    SettingWire wDenorm = {};
    wDenorm.field = SETTINGS_FIELD_UPBRINGING;
    wDenorm.category = 0x4A;
    memcpy(wDenorm.pos, posOrph, sizeof(posOrph));
    wDenorm.count = 6;
    wDenorm.data[0] = denormMin;
    wDenorm.data[1] = negDenorm;
    wDenorm.data[2] = 0.5f;
    wDenorm.data[3] = 0.0f;
    wDenorm.data[4] = 1.0f;
    wDenorm.data[5] = denormMin * 2.0f;

    TEST_CHECK(SettingsSync_ApplyCommand(wDenorm));
    const float* appliedDenorm = (const float*)(bldOrph.data() + BLD_UPBRINGING_SLIDERS_OFFSET);
    TEST_CHECK_NEAR(appliedDenorm[0], denormMin, 1e-20f);
    TEST_CHECK_NEAR(appliedDenorm[1], 0.0f, 1e-5f); // Negative denorm clamped to 0.0f
    TEST_CHECK_NEAR(appliedDenorm[2], 0.5f, 1e-5f);
    TEST_CHECK_NEAR(appliedDenorm[3], 0.0f, 1e-5f);
    TEST_CHECK_NEAR(appliedDenorm[4], 1.0f, 1e-5f);

    // 4. Extreme out-of-range clamping (-1000.0f, +1000.0f, etc.)
    SettingWire wClamp = {};
    wClamp.field = SETTINGS_FIELD_BROADCAST;
    wClamp.category = 0x25;
    memcpy(wClamp.pos, posMedia, sizeof(posMedia));
    wClamp.count = 6;
    float extremeFloats[6] = { -1000.0f, 1000.0f, -1e20f, 1e20f, -0.00001f, 1.00001f };
    memcpy(wClamp.data, extremeFloats, sizeof(extremeFloats));

    TEST_CHECK(SettingsSync_ApplyCommand(wClamp));
    const float* appliedClamp = (const float*)(studioObj.data() + STUDIO_BROADCAST_SLIDERS_OFFSET);
    TEST_CHECK_NEAR(appliedClamp[0], 0.0f, 1e-5f);
    TEST_CHECK_NEAR(appliedClamp[1], 1.0f, 1e-5f);
    TEST_CHECK_NEAR(appliedClamp[2], 0.0f, 1e-5f);
    TEST_CHECK_NEAR(appliedClamp[3], 1.0f, 1e-5f);
    TEST_CHECK_NEAR(appliedClamp[4], 0.0f, 1e-5f);
    TEST_CHECK_NEAR(appliedClamp[5], 1.0f, 1e-5f);

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    mockStudios.clear();
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, mockStudios);
    return true;
}

static bool SubTest_Gen6_MalformedPacketPayloads()
{
    ResetMockWorldMemory();
    std::vector<BYTE> typeOrph(0x400, 0);
    *(int*)(typeOrph.data() + BLD_TYPE_CAT_OFF) = 0x4A;
    std::vector<BYTE> bldOrph(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldOrph.data() + BLD_TYPE_OFFSET) = typeOrph.data();
    float posOrph[3] = { 100.0f, 20.0f, 200.0f };
    memcpy(bldOrph.data() + OBJ_POS_OFFSET, posOrph, sizeof(posOrph));

    std::vector<BYTE*> mockBlds = { bldOrph.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    float baseUpbringing[6] = { 0.3f, 0.3f, 0.1f, 0.1f, 0.1f, 0.1f };
    memcpy(bldOrph.data() + BLD_UPBRINGING_SLIDERS_OFFSET, baseUpbringing, sizeof(baseUpbringing));

    // 1. Invalid counts: count != 6
    const int badCounts[] = { -9999, -1, 0, 1, 2, 3, 4, 5, 7, 8, 16, 64, 1000, 0x7FFFFFFF };
    for (int count : badCounts) {
        SettingWire w = {};
        w.field = SETTINGS_FIELD_UPBRINGING;
        w.category = 0x4A;
        memcpy(w.pos, posOrph, sizeof(posOrph));
        w.count = count;
        for (int i = 0; i < 6; ++i) w.data[i] = 0.5f;

        TEST_CHECK(SettingsSync_ApplyCommand(w));
        const float* mem = (const float*)(bldOrph.data() + BLD_UPBRINGING_SLIDERS_OFFSET);
        for (int i = 0; i < 6; ++i) {
            TEST_CHECK_NEAR(mem[i], baseUpbringing[i], 1e-5f);
        }
    }

    // 2. Out of bounds field IDs
    const int badFields[] = { -100, -1, 999, 1004, 1005, 50000, 0x7FFFFFFF };
    for (int field : badFields) {
        SettingWire w = {};
        w.field = field;
        w.category = 0x4A;
        memcpy(w.pos, posOrph, sizeof(posOrph));
        w.value = 1;
        TEST_CHECK(SettingsSync_ApplyCommand(w));
    }

    // 3. Corrupted position coordinates: NaN, Inf, extreme
    const float badCoords[4][3] = {
        { std::numeric_limits<float>::quiet_NaN(), 20.0f, 200.0f },
        { 100.0f, (float)INFINITY, 200.0f },
        { 100.0f, 20.0f, (float)(-INFINITY) },
        { 1e30f, 1e30f, 1e30f }
    };
    for (const auto& coords : badCoords) {
        SettingWire w = {};
        w.field = SETTINGS_FIELD_UPBRINGING;
        w.category = 0x4A;
        memcpy(w.pos, coords, sizeof(coords));
        w.count = 6;
        for (int i = 0; i < 6; ++i) w.data[i] = 0.5f;

        TEST_CHECK(!SettingsSync_ApplyCommand(w));
    }

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_Gen6_CheckboxBoundariesAndAdjacentIntegrity()
{
    ResetMockWorldMemory();
    std::vector<BYTE> typeSite(0x400, 0), typePoll(0x400, 0), typeOrph(0x400, 0);
    *(int*)(typeSite.data() + BLD_TYPE_CAT_OFF) = 0x01; // Construction site
    *(int*)(typePoll.data() + BLD_TYPE_CAT_OFF) = 0x24; // Water treatment
    *(int*)(typeOrph.data() + BLD_TYPE_CAT_OFF) = 0x4A; // Orphanage

    std::vector<BYTE> bldSite(BUILDING_STRUCT_BYTES, 0xCC);
    std::vector<BYTE> bldPoll(BUILDING_STRUCT_BYTES, 0xCC);
    std::vector<BYTE> bldOrph(BUILDING_STRUCT_BYTES, 0xCC);

    *(BYTE**)(bldSite.data() + BLD_TYPE_OFFSET) = typeSite.data();
    *(BYTE**)(bldPoll.data() + BLD_TYPE_OFFSET) = typePoll.data();
    *(BYTE**)(bldOrph.data() + BLD_TYPE_OFFSET) = typeOrph.data();

    float posSite[3] = { 100.0f, 10.0f, 100.0f };
    float posPoll[3] = { 200.0f, 10.0f, 200.0f };
    float posOrph[3] = { 300.0f, 10.0f, 300.0f };
    memcpy(bldSite.data() + OBJ_POS_OFFSET, posSite, sizeof(posSite));
    memcpy(bldPoll.data() + OBJ_POS_OFFSET, posPoll, sizeof(posPoll));
    memcpy(bldOrph.data() + OBJ_POS_OFFSET, posOrph, sizeof(posOrph));

    std::vector<BYTE*> mockBlds = { bldSite.data(), bldPoll.data(), bldOrph.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    int fMain = -1, fGoods = -1, fFin = -1, fPolTog = -1, fPolThr = -1;
    for (int f = 0; f < 32; ++f) {
        SettingWire tw = {};
        tw.category = 0x4A;
        memcpy(tw.pos, posOrph, sizeof(posOrph));
        tw.field = f;
        tw.value = 0x55;
        bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] = 0;
        if (SettingsSync_ApplyCommand(tw) && bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] == 0x55) fMain = f;

        bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] = 0;
        if (SettingsSync_ApplyCommand(tw) && bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] == 0x55) fGoods = f;
    }
    for (int f = 0; f < 32; ++f) {
        SettingWire tw = {};
        tw.category = 0x01;
        memcpy(tw.pos, posSite, sizeof(posSite));
        tw.field = f;
        tw.value = 0x55;
        bldSite[BLD_ALERT_FINISHED_OFFSET] = 0;
        if (SettingsSync_ApplyCommand(tw) && bldSite[BLD_ALERT_FINISHED_OFFSET] == 0x55) fFin = f;
    }
    for (int f = 0; f < 32; ++f) {
        SettingWire tw = {};
        tw.category = 0x24;
        memcpy(tw.pos, posPoll, sizeof(posPoll));
        tw.field = f;
        tw.value = 0x55;
        *(int32_t*)(bldPoll.data() + BLD_POLLUTION_ALERT_TOGGLE_OFFSET) = 0;
        if (SettingsSync_ApplyCommand(tw) && *(int32_t*)(bldPoll.data() + BLD_POLLUTION_ALERT_TOGGLE_OFFSET) == 0x55) fPolTog = f;

        float pval = 77.0f;
        unsigned long long rval = 0;
        memcpy(&rval, &pval, sizeof(float));
        tw.value = rval;
        *(float*)(bldPoll.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET) = 0.0f;
        if (SettingsSync_ApplyCommand(tw) && fabsf(*(float*)(bldPoll.data() + BLD_POLLUTION_ALERT_THRESH_OFFSET) - 77.0f) < 1e-4f) fPolThr = f;
    }

    TEST_CHECK(fMain >= 0 && fGoods >= 0 && fFin >= 0 && fPolTog >= 0 && fPolThr >= 0);

    // 1. Verify 1-byte main alert toggle: only offset +0x1270 modified, +0x126F and +0x1271 untouched
    bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET - 1] = 0xAA;
    bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] = 0x00;
    bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET + 1] = 0xBB;

    SettingWire wMain = {};
    wMain.category = 0x4A;
    memcpy(wMain.pos, posOrph, sizeof(posOrph));
    wMain.field = fMain;
    wMain.value = 0x12345678; // Upper bytes must be truncated for 1-byte field
    TEST_CHECK(SettingsSync_ApplyCommand(wMain));
    TEST_CHECK(bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET] == 0x78);
    TEST_CHECK(bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET - 1] == 0xAA);
    TEST_CHECK(bldOrph[BLD_ALERT_MAIN_TOGGLE_OFFSET + 1] == 0xBB);

    // 2. Verify 1-byte goods missing: only offset +0x126D modified
    bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET - 1] = 0x11;
    bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] = 0x00;
    bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET + 1] = 0x22;

    SettingWire wGoods = {};
    wGoods.category = 0x4A;
    memcpy(wGoods.pos, posOrph, sizeof(posOrph));
    wGoods.field = fGoods;
    wGoods.value = 0x99;
    TEST_CHECK(SettingsSync_ApplyCommand(wGoods));
    TEST_CHECK(bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET] == 0x99);
    TEST_CHECK(bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET - 1] == 0x11);
    TEST_CHECK(bldOrph[BLD_ALERT_GOODS_MISSING_OFFSET + 1] == 0x22);

    // 3. Verify 1-byte construction finished alert: only offset +0x0E10 modified
    bldSite[BLD_ALERT_FINISHED_OFFSET - 1] = 0x33;
    bldSite[BLD_ALERT_FINISHED_OFFSET] = 0x00;
    bldSite[BLD_ALERT_FINISHED_OFFSET + 1] = 0x44;

    SettingWire wFin = {};
    wFin.category = 0x01;
    memcpy(wFin.pos, posSite, sizeof(posSite));
    wFin.field = fFin;
    wFin.value = 1;
    TEST_CHECK(SettingsSync_ApplyCommand(wFin));
    TEST_CHECK(bldSite[BLD_ALERT_FINISHED_OFFSET] == 1);
    TEST_CHECK(bldSite[BLD_ALERT_FINISHED_OFFSET - 1] == 0x33);
    TEST_CHECK(bldSite[BLD_ALERT_FINISHED_OFFSET + 1] == 0x44);

    // 4. Verify 4-byte pollution alert toggle: offsets +0x0DFC..+0x0DFF modified, adjacent preserved
    bldPoll[BLD_POLLUTION_ALERT_TOGGLE_OFFSET - 1] = 0x55;
    bldPoll[BLD_POLLUTION_ALERT_TOGGLE_OFFSET + 4] = 0x66;
    SettingWire wPolTog = {};
    wPolTog.category = 0x24;
    memcpy(wPolTog.pos, posPoll, sizeof(posPoll));
    wPolTog.field = fPolTog;
    wPolTog.value = 1;
    TEST_CHECK(SettingsSync_ApplyCommand(wPolTog));
    TEST_CHECK(*(int32_t*)(bldPoll.data() + BLD_POLLUTION_ALERT_TOGGLE_OFFSET) == 1);
    TEST_CHECK(bldPoll[BLD_POLLUTION_ALERT_TOGGLE_OFFSET - 1] == 0x55);
    TEST_CHECK(bldPoll[BLD_POLLUTION_ALERT_TOGGLE_OFFSET + 4] == 0x66);

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_Gen6_StudioLookupResilienceAndCorruptedPointers()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> typeMedia(0x400, 0);
    *(int*)(typeMedia.data() + BLD_TYPE_CAT_OFF) = 0x25;
    std::vector<BYTE> bldMedia(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldMedia.data() + BLD_TYPE_OFFSET) = typeMedia.data();
    float posMedia[3] = { 500.0f, 10.0f, 600.0f };
    memcpy(bldMedia.data() + OBJ_POS_OFFSET, posMedia, sizeof(posMedia));

    std::vector<BYTE*> mockBlds = { bldMedia.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    SettingWire w = {};
    w.field = SETTINGS_FIELD_BROADCAST;
    w.category = 0x25;
    memcpy(w.pos, posMedia, sizeof(posMedia));
    w.count = 6;
    for (int i = 0; i < 6; ++i) w.data[i] = 0.4f;

    // 1. Studio vector pointers are null
    *(BYTE***)(world + WORLD_STUDIOS_BEGIN_OFFSET) = nullptr;
    *(BYTE***)(world + WORLD_STUDIOS_END_OFFSET) = nullptr;
    TEST_CHECK(SettingsSync_ApplyCommand(w));

    // 2. Studio vector corrupted: end < begin
    BYTE* dummy = (BYTE*)0x1000;
    *(BYTE***)(world + WORLD_STUDIOS_BEGIN_OFFSET) = (BYTE**)&dummy;
    *(BYTE***)(world + WORLD_STUDIOS_END_OFFSET) = (BYTE**)&dummy - 10;
    TEST_CHECK(SettingsSync_ApplyCommand(w));

    // 3. Studio vector excessively huge (> 50,000 entries)
    *(BYTE***)(world + WORLD_STUDIOS_END_OFFSET) = (BYTE**)&dummy + 100000;
    TEST_CHECK(SettingsSync_ApplyCommand(w));

    // 4. Studio vector contains null entries and corrupted pointer
    BYTE* badStudio = (BYTE*)0xBAADF00DDEADBEEFull;
    std::vector<BYTE*> badStudioList = { nullptr, badStudio, nullptr };
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, badStudioList);
    TEST_CHECK(SettingsSync_ApplyCommand(w));

    // 5. Studio exists but points to different building
    std::vector<BYTE> otherBld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> studioObj(0x180, 0);
    *(BYTE**)(studioObj.data() + STUDIO_BUILDING_OFFSET) = otherBld.data();
    std::vector<BYTE*> studioList = { studioObj.data() };
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, studioList);
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    const float* smem = (const float*)(studioObj.data() + STUDIO_BROADCAST_SLIDERS_OFFSET);
    for (int i = 0; i < 6; ++i) {
        TEST_CHECK_NEAR(smem[i], 0.0f, 1e-5f);
    }

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    badStudioList.clear();
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, badStudioList);
    return true;
}

static bool SubTest_Gen6_CategoryMismatchAndInjection()
{
    ResetMockWorldMemory();
    std::vector<BYTE> typeHosp(0x400, 0), typeMedia(0x400, 0);
    *(int*)(typeHosp.data() + BLD_TYPE_CAT_OFF) = 0x1C; // Hospital
    *(int*)(typeMedia.data() + BLD_TYPE_CAT_OFF) = 0x25; // TV/Radio
    std::vector<BYTE> bldHosp(BUILDING_STRUCT_BYTES, 0), bldMedia(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldHosp.data() + BLD_TYPE_OFFSET) = typeHosp.data();
    *(BYTE**)(bldMedia.data() + BLD_TYPE_OFFSET) = typeMedia.data();
    float posHosp[3] = { 400.0f, 10.0f, 400.0f };
    float posMedia[3] = { 600.0f, 10.0f, 600.0f };
    memcpy(bldHosp.data() + OBJ_POS_OFFSET, posHosp, sizeof(posHosp));
    memcpy(bldMedia.data() + OBJ_POS_OFFSET, posMedia, sizeof(posMedia));

    std::vector<BYTE*> mockBlds = { bldHosp.data(), bldMedia.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    // 1. Send upbringing to TV station with mismatched category (0x4A in packet vs 0x25 building)
    SettingWire w = {};
    w.field = SETTINGS_FIELD_UPBRINGING;
    w.category = 0x4A;
    memcpy(w.pos, posMedia, sizeof(posMedia));
    w.count = 6;
    for (int i = 0; i < 6; ++i) w.data[i] = 0.5f;

    // Must be rejected because FindBuilding fails to find building with category 0x4A at posMedia
    TEST_CHECK(!SettingsSync_ApplyCommand(w));

    // 2. Send broadcast to Hospital with mismatched category (0x25 in packet vs 0x1C building)
    w.field = SETTINGS_FIELD_BROADCAST;
    w.category = 0x25;
    memcpy(w.pos, posHosp, sizeof(posHosp));
    TEST_CHECK(!SettingsSync_ApplyCommand(w));

    // 3. Send pollution alert toggle to Hospital with category 0x1C (FieldApplies must reject)
    int fPolTog = -1;
    for (int f = 0; f < 32; ++f) {
        if (f == 20) { fPolTog = f; break; }
    }
    w.field = fPolTog;
    w.category = 0x1C;
    w.value = 1;
    TEST_CHECK(SettingsSync_ApplyCommand(w));
    TEST_CHECK(*(int32_t*)(bldHosp.data() + BLD_POLLUTION_ALERT_TOGGLE_OFFSET) == 0);

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    return true;
}

static bool SubTest_Gen6_RapidPacketBurst()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> typeMedia(0x400, 0);
    *(int*)(typeMedia.data() + BLD_TYPE_CAT_OFF) = 0x25;
    std::vector<BYTE> bldMedia(BUILDING_STRUCT_BYTES, 0);
    *(BYTE**)(bldMedia.data() + BLD_TYPE_OFFSET) = typeMedia.data();
    float posMedia[3] = { 600.0f, 10.0f, 700.0f };
    memcpy(bldMedia.data() + OBJ_POS_OFFSET, posMedia, sizeof(posMedia));

    std::vector<BYTE*> mockBlds = { bldMedia.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);

    std::vector<BYTE> studioObj(0x180, 0);
    *(BYTE**)(studioObj.data() + STUDIO_BUILDING_OFFSET) = bldMedia.data();
    std::vector<BYTE*> mockStudios = { studioObj.data() };
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, mockStudios);

    // Rapid burst of 100 alternating valid and invalid broadcast packets
    for (int burst = 0; burst < 100; ++burst) {
        SettingWire w = {};
        w.field = SETTINGS_FIELD_BROADCAST;
        w.category = 0x25;
        memcpy(w.pos, posMedia, sizeof(posMedia));
        if (burst % 2 == 0) {
            w.count = 6;
            float val = (float)(burst % 10) / 10.0f;
            for (int i = 0; i < 6; ++i) w.data[i] = val;
        } else {
            w.count = (burst % 5);
            for (int i = 0; i < 6; ++i) w.data[i] = std::numeric_limits<float>::quiet_NaN();
        }
        SettingsSync_ApplyCommand(w);
    }

    const float* smem = (const float*)(studioObj.data() + STUDIO_BROADCAST_SLIDERS_OFFSET);
    for (int i = 0; i < 6; ++i) {
        TEST_CHECK_NEAR(smem[i], 0.8f, 1e-5f);
    }

    mockBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, mockBlds);
    mockStudios.clear();
    WriteMockVectorHeader(world + WORLD_STUDIOS_BEGIN_OFFSET, mockStudios);
    return true;
}

bool Test_Stress_Gen6_Challenger1()
{
    printf("    [SubTest 1: Float Fuzzing Upbringing & Broadcast] ...\n"); fflush(stdout);
    if (!SubTest_Gen6_FloatFuzzingUpbringingAndBroadcast()) return false;
    printf("    [SubTest 2: Malformed Packet Payloads] ...\n"); fflush(stdout);
    if (!SubTest_Gen6_MalformedPacketPayloads()) return false;
    printf("    [SubTest 3: Checkbox Boundaries & Adjacent Integrity] ...\n"); fflush(stdout);
    if (!SubTest_Gen6_CheckboxBoundariesAndAdjacentIntegrity()) return false;
    printf("    [SubTest 4: Studio & Building Lookup Resilience] ...\n"); fflush(stdout);
    if (!SubTest_Gen6_StudioLookupResilienceAndCorruptedPointers()) return false;
    printf("    [SubTest 5: Category Mismatch & Cross-Category Injection] ...\n"); fflush(stdout);
    if (!SubTest_Gen6_CategoryMismatchAndInjection()) return false;
    printf("    [SubTest 6: Rapid Packet Burst & Concurrency Integrity] ...\n"); fflush(stdout);
    if (!SubTest_Gen6_RapidPacketBurst()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Milestone M9.3 Features 32–36 Test Suite
// ---------------------------------------------------------------------------

static bool SubTest_M9_3_DeterministicAutosaveAndBackup()
{
    // Feature 32: Autosave tick interval & deterministic paths
    TEST_CHECK(!TwoStates_IsAutosaveTick(0));
    TEST_CHECK(!TwoStates_IsAutosaveTick(1));
    TEST_CHECK(!TwoStates_IsAutosaveTick(17999));
    TEST_CHECK(TwoStates_IsAutosaveTick(18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(18001));
    TEST_CHECK(TwoStates_IsAutosaveTick(36000));
    TEST_CHECK(TwoStates_IsAutosaveTick(54000));

    TEST_CHECK(strcmp(TwoStates_GetAutosaveRelPath(true), "save/coop_autosave") == 0);
    TEST_CHECK(strcmp(TwoStates_GetAutosaveRelPath(false), "save/coop_client_backup") == 0);

    // SaveSync_PreserveClientBackup: client preserves incoming save into coop_client_backup
    std::filesystem::path testIncoming = "temp_m9_3_incoming_test";
    std::error_code ec;
    std::filesystem::remove_all(testIncoming, ec);
    std::filesystem::create_directories(testIncoming, ec);

    FILE* fTest = nullptr;
    if (fopen_s(&fTest, (testIncoming / "header.bin").string().c_str(), "wb") == 0 && fTest) {
        const char dummy[] = "SOVIET_HEADER_DATA";
        fwrite(dummy, 1, sizeof(dummy), fTest);
        fclose(fTest);
    }
    TEST_CHECK(SaveSync_PreserveClientBackupForTest(testIncoming.string().c_str()));
    char backupDirBuf[MAX_PATH] = {};
    TEST_CHECK(SaveSync_GetGameSaveDirForTest(backupDirBuf, sizeof(backupDirBuf), "coop_client_backup"));
    std::filesystem::path backupHeader = std::filesystem::path(backupDirBuf) / "header.bin";
    TEST_CHECK(std::filesystem::exists(backupHeader, ec));

    std::filesystem::remove_all(testIncoming, ec);
    std::filesystem::remove_all(backupDirBuf, ec);

    return true;
}

static bool SubTest_M9_3_ZipUtf8AndZip64Integrity()
{
    // Feature 33: UTF-8 encoding for archive entries and zip integrity
    std::filesystem::path utf8Src = "temp_m9_3_zip_utf8";
    std::error_code ec;
    std::filesystem::remove_all(utf8Src, ec);
    std::filesystem::create_directories(utf8Src / u8"город" / u8"завод", ec);

    std::filesystem::path testFile = utf8Src / u8"город" / u8"завод" / u8"план_пятилетки.txt";
    FILE* fUtf = nullptr;
    if (fopen_s(&fUtf, testFile.string().c_str(), "wb") == 0 && fUtf) {
        const char content[] = "План пятилетки выполнен досрочно!";
        fwrite(content, 1, sizeof(content), fUtf);
        fclose(fUtf);
    }

    std::filesystem::path zipDst = "temp_m9_3_utf8.zip";
    std::filesystem::remove(zipDst, ec);
    TEST_CHECK(ZipSaveDirectory(utf8Src.string().c_str(), zipDst.string().c_str()));
    TEST_CHECK(std::filesystem::exists(zipDst, ec));

    std::filesystem::path unzipDst = "temp_m9_3_unzip_out";
    std::filesystem::remove_all(unzipDst, ec);
    TEST_CHECK(UnzipToDirectory(zipDst.string().c_str(), unzipDst.string().c_str()));
    std::filesystem::path extractedFile = unzipDst / u8"город" / u8"завод" / u8"план_пятилетки.txt";
    TEST_CHECK(std::filesystem::exists(extractedFile, ec));

    std::filesystem::remove_all(utf8Src, ec);
    std::filesystem::remove(zipDst, ec);
    std::filesystem::remove_all(unzipDst, ec);

    // Fail-fast on non-existent directory
    std::filesystem::path nonExistent = "temp_m9_3_does_not_exist_xyz";
    std::filesystem::path badZip = "temp_m9_3_bad.zip";
    TEST_CHECK(!ZipSaveDirectory(nonExistent.string().c_str(), badZip.string().c_str()));
    TEST_CHECK(!std::filesystem::exists(badZip, ec));

    // 64-bit progress transfer stats in SyncBlock
    int64_t largeQueued = 3456789012LL;
    int64_t largeTotal = 8901234567LL;
    SyncBlock_SetPercentEx(1, largeQueued, largeTotal, 38.8f);
    int pct = 0;
    int64_t q = 0, t = 0;
    float spd = 0.0f;
    SyncBlock_GetTransferStats(&pct, &q, &t, &spd);
    TEST_CHECK(q == largeQueued);
    TEST_CHECK(t == largeTotal);
    SyncBlock_SetPercent(-1);

    return true;
}

static bool SubTest_M9_3_ClientSettingsIsolationAndCameraFix()
{
    // Feature 34: Client settings isolation (windows.bin, toolco.bin)
    std::filesystem::path destDir = "temp_m9_3_settings_dest";
    std::error_code ec;
    std::filesystem::remove_all(destDir, ec);
    std::filesystem::create_directories(destDir, ec);

    // Create windows.bin and toolco.bin in incoming save
    FILE* fw = nullptr;
    if (fopen_s(&fw, (destDir / "windows.bin").string().c_str(), "wb") == 0 && fw) {
        fputc(1, fw);
        fclose(fw);
    }
    FILE* ft = nullptr;
    if (fopen_s(&ft, (destDir / "toolco.bin").string().c_str(), "wb") == 0 && ft) {
        fputc(2, ft);
        fclose(ft);
    }
    TEST_CHECK(std::filesystem::exists(destDir / "windows.bin", ec));
    TEST_CHECK(std::filesystem::exists(destDir / "toolco.bin", ec));

    // Initial join (rejoin = false): windows.bin and toolco.bin deleted
    SaveSync_IsolateClientSettingsForTest(destDir.string().c_str(), false);
    TEST_CHECK(!std::filesystem::exists(destDir / "windows.bin", ec));
    TEST_CHECK(!std::filesystem::exists(destDir / "toolco.bin", ec));

    // Resync (rejoin = true): host's windows.bin deleted, client's local toolco.bin restored
    char backupDirBuf2[MAX_PATH] = {};
    TEST_CHECK(SaveSync_GetGameSaveDirForTest(backupDirBuf2, sizeof(backupDirBuf2), "coop_client_backup"));
    std::filesystem::path backupDir(backupDirBuf2);
    std::filesystem::create_directories(backupDir, ec);
    FILE* fb = nullptr;
    if (fopen_s(&fb, (backupDir / "toolco.bin").string().c_str(), "wb") == 0 && fb) {
        const char toolData[] = "CLIENT_LOCAL_TOOLCO";
        fwrite(toolData, 1, sizeof(toolData), fb);
        fclose(fb);
    }
    if (fopen_s(&fw, (destDir / "windows.bin").string().c_str(), "wb") == 0 && fw) {
        fputc(5, fw);
        fclose(fw);
    }
    if (fopen_s(&ft, (destDir / "toolco.bin").string().c_str(), "wb") == 0 && ft) {
        fputc(6, ft);
        fclose(ft);
    }
    SaveSync_IsolateClientSettingsForTest(destDir.string().c_str(), true);
    TEST_CHECK(!std::filesystem::exists(destDir / "windows.bin", ec));
    TEST_CHECK(std::filesystem::exists(destDir / "toolco.bin", ec));

    FILE* fRestored = nullptr;
    char checkBuf[64] = {};
    if (fopen_s(&fRestored, (destDir / "toolco.bin").string().c_str(), "rb") == 0 && fRestored) {
        fread(checkBuf, 1, sizeof(checkBuf), fRestored);
        fclose(fRestored);
    }
    TEST_CHECK(strcmp(checkBuf, "CLIENT_LOCAL_TOOLCO") == 0);

    std::filesystem::remove_all(destDir, ec);
    std::filesystem::remove_all(backupDir, ec);

    return true;
}

static bool SubTest_M9_3_DemographicTrackersPersistence()
{
    // Feature 35: Demographic trackers persistence
    StateTrackerSlice cur1 = {};
    StateTrackerSlice cur2 = {};
    StateTrackerSlice old1 = {};
    StateTrackerSlice old2 = {};

    cur1.healthSum[0] = 12.345f;
    cur1.healthCnt[0] = 77;
    cur1.happySum[1] = 88.5f;
    cur1.happyCnt[1] = 101;

    cur2.healthSum[3] = 45.678f;
    cur2.healthCnt[3] = 23;
    cur2.happySum[2] = 55.25f;
    cur2.happyCnt[2] = 64;

    old1.healthSum[1] = 3.1415f;
    old1.healthCnt[1] = 9;
    old2.happySum[0] = 99.9f;
    old2.happyCnt[0] = 15;

    TwoStates_SetStateTrackerSliceForTest(1, false, cur1);
    TwoStates_SetStateTrackerSliceForTest(1, true, old1);
    TwoStates_SetStateTrackerSliceForTest(2, false, cur2);
    TwoStates_SetStateTrackerSliceForTest(2, true, old2);

    StateTrackerSlice outCur1 = {}, outOld1 = {}, outCur2 = {}, outOld2 = {};
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(1, false, &outCur1));
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(1, true, &outOld1));
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(2, false, &outCur2));
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(2, true, &outOld2));

    TEST_CHECK_NEAR(outCur1.healthSum[0], 12.345f, 0.002f);
    TEST_CHECK(outCur1.healthCnt[0] == 77);
    TEST_CHECK_NEAR(outCur1.happySum[1], 88.5f, 0.002f);
    TEST_CHECK(outCur1.happyCnt[1] == 101);

    TEST_CHECK_NEAR(outCur2.healthSum[3], 45.678f, 0.002f);
    TEST_CHECK(outCur2.healthCnt[3] == 23);
    TEST_CHECK_NEAR(outCur2.happySum[2], 55.25f, 0.002f);
    TEST_CHECK(outCur2.happyCnt[2] == 64);

    TEST_CHECK_NEAR(outOld1.healthSum[1], 3.1415f, 0.002f);
    TEST_CHECK(outOld1.healthCnt[1] == 9);
    TEST_CHECK_NEAR(outOld2.happySum[0], 99.9f, 0.002f);
    TEST_CHECK(outOld2.happyCnt[0] == 15);

    return true;
}

static bool SubTest_M9_3_VehicleOwnershipAndPruning()
{
    // Feature 36: Ownership registration, pruning dead entities, and road segment cleanup
    TwoStates_RegisterVehicleOwner(8801, 1);
    TwoStates_RegisterVehicleOwner(8802, 2);
    TEST_CHECK(TwoStates_GetVehicleOwner(8801, nullptr) == 1);
    TEST_CHECK(TwoStates_GetVehicleOwner(8802, nullptr) == 2);

    TwoStates_RegisterBuildingOwner(7701, 1);
    TwoStates_RegisterBuildingOwner(7702, 2);
    TEST_CHECK(TwoStates_GetBuildingOwner(7701, 0.0f, 0.0f) == 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(7702, 0.0f, 0.0f) == 2);

    // Prune dead entities (in mock test harness, no entities exist in world memory)
    TwoStates_PruneDeadEntities(false);

    // Dead entities must be purged from owner maps
    TEST_CHECK(TwoStates_GetVehicleOwner(8801, nullptr) == 0);
    TEST_CHECK(TwoStates_GetVehicleOwner(8802, nullptr) == 0);

    // Planned road segment reset
    RoadSync_ClearPlannedSegments();

    return true;
}

static bool SubTest_M9_3_GameSaveDirPrefersExeOverWorkshopBaseDir()
{
    HMODULE selfMod = GetModuleHandleA(nullptr);
    TEST_CHECK(selfMod != nullptr);

    char realExeDir[MAX_PATH] = {};
    DWORD exeLen = GetModuleFileNameA(selfMod, realExeDir, MAX_PATH);
    TEST_CHECK(exeLen > 0 && exeLen < MAX_PATH);
    char* slash = strrchr(realExeDir, '\\');
    TEST_CHECK(slash != nullptr);
    *slash = '\0';

    char buf[MAX_PATH] = {};
    char expected[MAX_PATH] = {};

    // 1. Workshop launch regression: real exe module dir must override loader baseDir.
    TEST_CHECK(SaveSync_ResolveGameSaveDir(
        selfMod, "C:\\FakeWorkshop\\784150\\3823543010\\TesmioLoader\\build",
        buf, sizeof(buf), "mp_syncsave_deadbeef"));
    _snprintf_s(expected, sizeof(expected), _TRUNCATE, "%s\\media_soviet\\save\\mp_syncsave_deadbeef", realExeDir);
    TEST_CHECK(strcmp(buf, expected) == 0);
    TEST_CHECK(strstr(buf, "FakeWorkshop") == nullptr);

    // 2. Leading "save/" and "save\\" prefix stripping.
    _snprintf_s(expected, sizeof(expected), _TRUNCATE, "%s\\media_soviet\\save\\coop_last_session", realExeDir);
    TEST_CHECK(SaveSync_ResolveGameSaveDir(selfMod, g_mockBaseDir, buf, sizeof(buf), "save/coop_last_session"));
    TEST_CHECK(strcmp(buf, expected) == 0);
    TEST_CHECK(SaveSync_ResolveGameSaveDir(selfMod, g_mockBaseDir, buf, sizeof(buf), "save\\coop_last_session"));
    TEST_CHECK(strcmp(buf, expected) == 0);

    // 3. Mock harness fallback when exeMod is non-module VirtualAlloc memory.
    TEST_CHECK(SaveSync_ResolveGameSaveDir(
        (HMODULE)g_mockExeBuffer, g_mockBaseDir, buf, sizeof(buf), "coop_client_backup"));
    _snprintf_s(expected, sizeof(expected), _TRUNCATE, "%s\\media_soviet\\save\\coop_client_backup", g_mockBaseDir);
    TEST_CHECK(strcmp(buf, expected) == 0);

    // 4. Null / empty buffer guards.
    TEST_CHECK(!SaveSync_ResolveGameSaveDir(nullptr, nullptr, nullptr, 0, "x"));
    TEST_CHECK(!SaveSync_ResolveGameSaveDir(selfMod, g_mockBaseDir, buf, sizeof(buf), nullptr));

    return true;
}

bool Test_SaveSyncAndEconStatePersistence_M9_3()
{
    printf("    [SubTest 1: Deterministic Co-op Autosaves & Client Backup (Feature 32)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_DeterministicAutosaveAndBackup()) return false;

    printf("    [SubTest 2: ZIP UTF-8 Encoding & ZIP64 Limits (Feature 33)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_ZipUtf8AndZip64Integrity()) return false;

    printf("    [SubTest 3: Client Settings Isolation & Camera Fixes (Feature 34)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_ClientSettingsIsolationAndCameraFix()) return false;

    printf("    [SubTest 4: Demographic Trackers Persistence (Feature 35)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_DemographicTrackersPersistence()) return false;

    printf("    [SubTest 5: Vehicle Ownership & Dead Entity Pruning (Feature 36)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_VehicleOwnershipAndPruning()) return false;

    printf("    [SubTest 6: Save Directory Prefers Exe Over Workshop BaseDir] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_GameSaveDirPrefersExeOverWorkshopBaseDir()) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Test 46 & 47: M9.3 Challenger 1 & 2 Stress Harnesses
// ---------------------------------------------------------------------------
// Note: m9_3_challenger1_stress.cpp and m9_3_challenger2_stress.cpp define
// their entry points with internal linkage (static bool Test_Stress_M9_3_Challenger*).
// Wrapping each include in a dedicated namespace avoids linkage conflicts with
// the external-linkage declarations in test_harness.h while forwarding cleanly.

namespace m9_3_c1 {
#include "m9_3_challenger1_stress.cpp"
} // namespace m9_3_c1

bool Test_Stress_M9_3_Challenger1()
{
    return m9_3_c1::Test_Stress_M9_3_Challenger1();
}

namespace m9_3_c2 {
#include "m9_3_challenger2_stress.cpp"
} // namespace m9_3_c2

bool Test_Stress_M9_3_Challenger2()
{
    if (!H) { TsmBind(&g_mockHost); }
    return m9_3_c2::Test_Stress_M9_3_Challenger2();
}
