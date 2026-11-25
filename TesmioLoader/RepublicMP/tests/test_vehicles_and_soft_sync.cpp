// SPDX-License-Identifier: GPL-3.0-only
// Vehicle DLC filtering, soft-sync, schedules, M4 stress suites, and RCO refuel/held-frame fixes.

#include "test_harness.h"
#include <chrono>

// ---------------------------------------------------------------------------
// Test 12: Vehicle DLC Filtering, Canonical ScriptName Remapping & Diag Telemetry
// ---------------------------------------------------------------------------

static bool SubTest_DlcTokenExtractionAndMasks()
{
    char dlcTok[16] = {};

    // 1. Building DLC token extraction (dlc[0-9]+_)
    TEST_CHECK(SaveSync_ExtractDlcToken("dlc2_residential01", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc2") == 0);

    TEST_CHECK(SaveSync_ExtractDlcToken("DLC3_swimming_hall", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc3") == 0);

    TEST_CHECK(!SaveSync_ExtractDlcToken("building_vanilla_farm", dlcTok, sizeof(dlcTok)));

    // 2. Vehicle DLC token extraction (forward and back slashes, media_soviet/ prefix)
    TEST_CHECK(SaveSync_ExtractDlcToken("media_soviet/dlc2/vehicles/truck.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc2") == 0);

    TEST_CHECK(SaveSync_ExtractDlcToken("dlc3\\vehicles\\steam_p36.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc3") == 0);

    TEST_CHECK(SaveSync_ExtractDlcToken("media_soviet/dlc1/vehicles/bus.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(strcmp(dlcTok, "dlc1") == 0);

    TEST_CHECK(!SaveSync_ExtractDlcToken("media_soviet/vehicles/bus.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(!SaveSync_ExtractDlcToken("workshop/123456/truck.ini", dlcTok, sizeof(dlcTok)));
    TEST_CHECK(!SaveSync_ExtractDlcToken(nullptr, dlcTok, sizeof(dlcTok)));
    TEST_CHECK(!SaveSync_ExtractDlcToken("", dlcTok, sizeof(dlcTok)));

    // 3. Bitmask mapping
    TEST_CHECK(SaveSync_GetDlcMask("dlc1") == 0x01);
    TEST_CHECK(SaveSync_GetDlcMask("dlc2") == 0x02);
    TEST_CHECK(SaveSync_GetDlcMask("dlc3") == 0x04);
    TEST_CHECK(SaveSync_GetDlcMask("dlc2,dlc3") == 0x06);
    TEST_CHECK(SaveSync_GetDlcMask("dlc5") == 0x10);
    TEST_CHECK(SaveSync_GetDlcMask("dlc_unknown") == 0x00);
    TEST_CHECK(SaveSync_GetDlcMask(nullptr) == 0x00);

    // 4. Formatting DLC names
    char namesBuf[128] = {};
    SaveSync_FormatDlcNames(0x02, namesBuf, sizeof(namesBuf));
    TEST_CHECK(strstr(namesBuf, "Biomes") != nullptr);

    SaveSync_FormatDlcNames(0x06, namesBuf, sizeof(namesBuf));
    TEST_CHECK(strstr(namesBuf, "Biomes") != nullptr && strstr(namesBuf, "Early Start") != nullptr);

    SaveSync_FormatDlcNames(0x10, namesBuf, sizeof(namesBuf));
    TEST_CHECK(strstr(namesBuf, "Space Program") != nullptr && strstr(namesBuf, "dlc5") != nullptr);
    SaveSync_FormatDlcNames(0x20, namesBuf, sizeof(namesBuf));
    TEST_CHECK(strstr(namesBuf, "DLC 6") != nullptr && strstr(namesBuf, "dlc6") != nullptr);
    SaveSync_FormatDlcNames(0x00, namesBuf, sizeof(namesBuf));
    TEST_CHECK(_stricmp(namesBuf, "none") == 0);

    return true;
}

static bool SubTest_VehicleDlcSessionValidation()
{
    // 1. Singleplayer Isolation: All vehicles allowed regardless of DLC state
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_SetTestDlcLists("dlc2", ""); // host has dlc2, client has none

    std::vector<BYTE> vehType(WORLD_VEHICLE_TYPE_STRIDE, 0);
    char dlcPath[64] = "dlc3/vehicles/steam_p36.ini";
    strncpy_s((char*)vehType.data() + 0x200, 64, dlcPath, _TRUNCATE);

    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2. Co-op Session: Host has dlc2,dlc3 (0x06), Client has dlc2 (0x02) -> Common is dlc2 (0x02)
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    SaveSync_SetTestDlcLists("dlc2,dlc3", "dlc2");

    uint32_t hostM = 0, clientM = 0, commonM = 0;
    SaveSync_GetDlcMasks(&hostM, &clientM, &commonM);
    TEST_CHECK(hostM == 0x06 && clientM == 0x02 && commonM == 0x02);

    // 2a. Vehicle with shared DLC (dlc2) -> ALLOWED
    strncpy_s((char*)vehType.data() + 0x200, 64, "media_soviet/dlc2/vehicles/truck.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2b. Vehicle with unshared DLC (dlc3) -> BLOCKED
    strncpy_s((char*)vehType.data() + 0x200, 64, "dlc3\\vehicles\\steam_p36.ini", _TRUNCATE);
    TEST_CHECK(!SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(!ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2c. Vanilla vehicle -> ALLOWED
    strncpy_s((char*)vehType.data() + 0x200, 64, "media_soviet/vehicles/bus.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    TEST_CHECK(ModSync_CheckVehicleTypeAllowed(vehType.data()));

    // 2d. Space Program DLC (dlc5) -> blocked unless both sides own dlc5
    strncpy_s((char*)vehType.data() + 0x200, 64, "media_soviet/dlc5/vehicles/rocket_carrier.ini", _TRUNCATE);
    TEST_CHECK(!SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    SaveSync_SetTestDlcLists("dlc2,dlc3,dlc5", "dlc2,dlc5");
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));
    // 2e. Workshop mod vehicle -> ALLOWED by DLC check (falls through to workshop check)
    strncpy_s((char*)vehType.data() + 0x200, 64, "media_soviet/workshop/12345/truck.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(vehType.data()));

    // Cleanup session state back to singleplayer
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_SetTestDlcLists("", "");
    return true;
}

static bool SubTest_TrailerDlcValidation()
{
    SaveSync_SetTestSessionState(SESSION_HOST_IN_GAME, true, "127.0.0.1:5556");
    SaveSync_SetTestDlcLists("dlc2,dlc3", "dlc2"); // dlc3 is unshared

    // Lead vehicle is vanilla (no DLC requirement)
    std::vector<BYTE> leadType(WORLD_VEHICLE_TYPE_STRIDE, 0);
    strncpy_s((char*)leadType.data() + 0x200, 64, "media_soviet/vehicles/train_loco.ini", _TRUNCATE);

    // Coupled trailer array at leadType + 0x7C58
    // Vector of elements with stride 0x110. Pointer to trailer VehicleType is at offset +0x100 within each record.
    const unsigned kNumTrailers = 2;
    std::vector<BYTE> trailerRecords(0x110 * kNumTrailers, 0);

    std::vector<BYTE> tr0Type(0x1000, 0);
    strncpy_s((char*)tr0Type.data() + 0x200, 64, "media_soviet/vehicles/boxcar.ini", _TRUNCATE); // vanilla trailer

    std::vector<BYTE> tr1Type(0x1000, 0);
    strncpy_s((char*)tr1Type.data() + 0x200, 64, "dlc3/vehicles/wagon_2axle.ini", _TRUNCATE); // unowned DLC!

    *(BYTE**)(trailerRecords.data() + 0 * 0x110 + 0x100) = tr0Type.data();
    *(BYTE**)(trailerRecords.data() + 1 * 0x110 + 0x100) = tr1Type.data();

    *(BYTE**)(leadType.data() + 0x7C58) = trailerRecords.data();
    *(BYTE**)(leadType.data() + 0x7C60) = trailerRecords.data() + trailerRecords.size();

    // Must block because trailer 1 requires unshared dlc3
    TEST_CHECK(!SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Change trailer 1 to owned dlc2 -> now both trailers allowed -> returns true
    strncpy_s((char*)tr1Type.data() + 0x200, 64, "media_soviet/dlc2/vehicles/flatcar.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Consist length 20+: verify trailer inspection extends past legacy 16 cap
    const unsigned kNumTrailers20 = 21;
    std::vector<BYTE> trailerRecords20(0x110 * kNumTrailers20, 0);
    std::vector<std::vector<BYTE>> trailerTypes20(kNumTrailers20, std::vector<BYTE>(0x1000, 0));

    for (unsigned i = 0; i < kNumTrailers20; ++i) {
        strncpy_s((char*)trailerTypes20[i].data() + 0x200, 64, "media_soviet/vehicles/boxcar.ini", _TRUNCATE);
        *(BYTE**)(trailerRecords20.data() + i * 0x110 + 0x100) = trailerTypes20[i].data();
    }

    // Trailer 17 (index 16) requires unshared DLC (dlc3)
    strncpy_s((char*)trailerTypes20[16].data() + 0x200, 64, "dlc3/vehicles/wagon_special.ini", _TRUNCATE);
    *(BYTE**)(leadType.data() + 0x7C58) = trailerRecords20.data();
    *(BYTE**)(leadType.data() + 0x7C60) = trailerRecords20.data() + trailerRecords20.size();

    // Must block because trailer 17 (index 16) requires unshared dlc3 (verifies cap > 16)
    TEST_CHECK(!SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Revert trailer 17 to owned dlc2 -> now all trailers allowed -> returns true
    strncpy_s((char*)trailerTypes20[16].data() + 0x200, 64, "media_soviet/dlc2/vehicles/flatcar.ini", _TRUNCATE);
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(leadType.data()));

    // Cleanup session state
    SaveSync_SetTestSessionState(SESSION_IDLE, false, "");
    SaveSync_SetTestDlcLists("", "");
    return true;
}

static bool SubTest_ScriptNameResolutionWithShiftedIndices()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Create 4 vehicle types, each of size WORLD_VEHICLE_TYPE_STRIDE (0xA0C8)
    const size_t kTypeCount = 4;
    const size_t kStride = WORLD_VEHICLE_TYPE_STRIDE;
    std::vector<BYTE> typePool(kStride * kTypeCount, 0);

    BYTE* t0 = typePool.data() + 0 * kStride;
    BYTE* t1 = typePool.data() + 1 * kStride;
    BYTE* t2 = typePool.data() + 2 * kStride;
    BYTE* t3 = typePool.data() + 3 * kStride;

    strncpy_s((char*)t0 + 0x200, 64, "media_soviet/vehicles/bus_old.ini", _TRUNCATE);
    strncpy_s((char*)t1 + 0x200, 64, "media_soviet/dlc2/vehicles/bus_bi.ini", _TRUNCATE);
    strncpy_s((char*)t2 + 0x200, 64, "media_soviet/vehicles/truck_tatra.ini", _TRUNCATE);
    strncpy_s((char*)t3 + 0x200, 64, "dlc2\\vehicles\\loader_win.ini", _TRUNCATE);

    // Setup vehicle types vector at world + WORLD_VEHICLE_TYPES_OFFSET (0x128F0)
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET)     = typePool.data();
    *(BYTE**)(world + WORLD_VEHICLE_TYPES_OFFSET + 8) = typePool.data() + typePool.size();

    // Direct match by full path
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2/vehicles/bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "dlc2/vehicles/bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/vehicles/bus_old.ini") == t0);

    // Case-insensitive match without "media_soviet/" prefix
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "vehicles/truck_tatra.ini") == t2);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "MEDIA_SOVIET/VEHICLES/TRUCK_TATRA.INI") == t2);

    // Slash variations (forward slashes, backslashes, mixed slashes)
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet\\dlc2\\vehicles\\bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "dlc2\\vehicles\\bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2\\vehicles/bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet\\dlc2/vehicles\\bus_bi.ini") == t1);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "dlc2/vehicles/loader_win.ini") == t3);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2/vehicles/loader_win.ini") == t3);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet\\dlc2\\vehicles\\loader_win.ini") == t3);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/dlc2\\vehicles/loader_win.ini") == t3);

    // Inexistent vehicle
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(world, "media_soviet/vehicles/non_existent.ini") == nullptr);

    // Index drift test: simulated host has t2 at index 0, but on our client t2 is at index 2
    // Wire message has index 0 (which points to bus_old), but m.scriptName is "truck_tatra.ini"
    // Purchase_FindVehicleTypeByScriptName recovers the true type t2
    BYTE* recoveredType = Purchase_FindVehicleTypeByScriptName(world, "vehicles/truck_tatra.ini");
    TEST_CHECK(recoveredType == t2);
    TEST_CHECK(recoveredType != t0);

    return true;
}

static bool SubTest_CabinCategoryHandling()
{
    // Cable car cabin category is 7 (VEHICLETYPE_CABIN).
    // Categories 1, 4, 5 route to customs slots.
    // Categories 6, 8, 10 route to border points via PlaceRealisticVehicleAtBorder.
    // Category 7 MUST be excluded from border arrival logic to avoid navigation/aviation crashes.
    const unsigned catTrain = 1;
    const unsigned catRoad = 4;
    const unsigned catTram = 5;
    const unsigned catShip = 6;
    const unsigned catCabin = 7;
    const unsigned catPlane = 8;
    const unsigned catHeli = 10;

    auto IsCustomsSlotCategory = [](unsigned c) { return c == 1 || c - 4 <= 1; };
    auto IsBorderArrivalCategory = [](unsigned c) { return c == 6 || c == 8 || c == 10; };

    TEST_CHECK(IsCustomsSlotCategory(catTrain));
    TEST_CHECK(IsCustomsSlotCategory(catRoad));
    TEST_CHECK(IsCustomsSlotCategory(catTram));
    TEST_CHECK(!IsCustomsSlotCategory(catCabin));

    TEST_CHECK(IsBorderArrivalCategory(catShip));
    TEST_CHECK(IsBorderArrivalCategory(catPlane));
    TEST_CHECK(IsBorderArrivalCategory(catHeli));
    TEST_CHECK(!IsBorderArrivalCategory(catCabin));

    return true;
}

static bool SubTest_TelemetryCountersAndMemorySafety()
{
    Purchase_ResetDiagStatsForTest();
    VehicleSyncDiagStats stats = {};
    Purchase_GetDiagStats(&stats);
    TEST_CHECK(stats.purchasesAttempted == 0);
    TEST_CHECK(stats.purchasesMade == 0);
    TEST_CHECK(stats.vehiclesSold == 0);

    // Record sales across diverse vehicle categories
    Purchase_RecordDiagSale(1); // road (cat 1: ROAD)
    Purchase_RecordDiagSale(4); // rail (cat 4: RAIL_LOCOMOTIVE)
    Purchase_RecordDiagSale(6); // water (cat 6: SHIP)
    Purchase_RecordDiagSale(8); // air (cat 8: AIRPLANE)
    Purchase_RecordDiagSale(7); // special / cabin (cat 7: CABIN)

    Purchase_GetDiagStats(&stats);
    TEST_CHECK(stats.vehiclesSold == 5);
    TEST_CHECK(stats.countRoad == 1);
    TEST_CHECK(stats.countRail == 1);
    TEST_CHECK(stats.countWater == 1);
    TEST_CHECK(stats.countAir == 1);
    TEST_CHECK(stats.countSpecial == 1);

    // Memory Safety Probes: nullptr and unmapped pointers must not crash
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed(nullptr));
    TEST_CHECK(SaveSync_CheckVehicleDlcAllowed((const void*)0x1234));
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(nullptr, "test.ini") == nullptr);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(g_mockExeBuffer + WORLD_RVA, nullptr) == nullptr);
    TEST_CHECK(Purchase_FindVehicleTypeByScriptName(g_mockExeBuffer + WORLD_RVA, (const char*)0x1234) == nullptr);

    // Direct test of NormalizeVehicleScriptName
    {
        char out[64] = {};
        NormalizeVehicleScriptName("media_soviet\\dlc2\\vehicles\\bus.ini", out, sizeof(out));
        TEST_CHECK(strcmp(out, "dlc2/vehicles/bus.ini") == 0);
        NormalizeVehicleScriptName("MEDIA_SOVIET/VEHICLES/CAR.INI", out, sizeof(out));
        TEST_CHECK(strcmp(out, "vehicles/car.ini") == 0);
    }

    // Defense-in-depth: Malformed network packet with 64 non-null bytes in scriptName
    {
        std::vector<BYTE> rawPacket(208, 0xAA);
        memset(rawPacket.data() + rawPacket.size() - 64, 'X', 64);
        TEST_CHECK(!Purchase_ApplyCommand(rawPacket.data(), rawPacket.size()));
    }

    return true;
}

bool Test_VehicleDlcFilter_ScriptName_DiagLog()
{
    return SubTest_DlcTokenExtractionAndMasks() &&
           SubTest_VehicleDlcSessionValidation() &&
           SubTest_TrailerDlcValidation() &&
           SubTest_ScriptNameResolutionWithShiftedIndices() &&
           SubTest_CabinCategoryHandling() &&
           SubTest_TelemetryCountersAndMemorySafety();
}

// ---------------------------------------------------------------------------
// Test 13: Train/Wagon Cargo Soft-Sync, Building Storage Healing & Vehicle Window Actions
// ---------------------------------------------------------------------------

static bool SubTest_StoppedTrainAndWagonEffectiveBld()
{
    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld.data() = 64;

    std::vector<BYTE> truck(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(truck.data() + VEH_CUR_BLD_OFFSET) = bld.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(truck.data()) == bld.data());

    std::vector<BYTE> loco(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(loco.data() + VEH_STOPPED_BLD_OFFSET) = bld.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(loco.data()) == bld.data());

    std::vector<BYTE> wagon(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(wagon.data() + VEH_LINK_PTR_OFFSET) = loco.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(wagon.data()) == bld.data());

    *(BYTE**)(wagon.data() + VEH_LINK_PTR_OFFSET) = nullptr;
    *(BYTE**)(wagon.data() + VEH_PARENT_PTR_OFFSET) = loco.data();
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(wagon.data()) == bld.data());

    *(BYTE**)(loco.data() + VEH_STOPPED_BLD_OFFSET) = nullptr;
    TEST_CHECK(VehicleSoftSync_ResolveEffectiveBld(wagon.data()) == nullptr);

    // Verify coupled wagons/mechanisms (+0x398) and moving rail locomotives (+0x6A0..+0x6A8, +0x798)
    // are recognized as having an active engine path so VehicleSoftSync never snaps individual wagons mid-motion.
    TEST_CHECK(VehicleSoftSync_HasActiveEnginePathForTest(wagon.data())); // +0x398 == loco
    TEST_CHECK(!VehicleSoftSync_HasActiveEnginePathForTest(loco.data()));
    *(uintptr_t*)(loco.data() + 0x6A0) = 0x1000;
    *(uintptr_t*)(loco.data() + 0x6A8) = 0x1020;
    TEST_CHECK(VehicleSoftSync_HasActiveEnginePathForTest(loco.data()));
    *(uintptr_t*)(loco.data() + 0x6A0) = 0;
    *(uintptr_t*)(loco.data() + 0x6A8) = 0;
    *(void**)(loco.data() + 0x798) = (void*)0x2000;
    TEST_CHECK(VehicleSoftSync_HasActiveEnginePathForTest(loco.data()));

    // Verify VehicleSwitches excludes +0x4C8..+0x4E8 (+0x4D0 runtime state) and live motion/link/fuel fields
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, 0x4D0));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, VEH_LINK_PTR_OFFSET));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, VEH_FUEL_OFFSET));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, VEH_STEER_CURRENT_OFFSET));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, VEH_TRAIL_BUFFER_OFFSET));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, VEH_SIXTH_NODE_OFFSET));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, VEH_RENDER_VISIBLE_OFFSET));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(0 /*KIND_VEHICLE*/, VEH_WHEELS_BEGIN_OFFSET));

    // Verify VehicleSwitches KIND_CARGO (2) excludes Resource* (+0x00/+0x04) and dynamic amountTons (+0x08, +0x18),
    // while allowing UI cargo filterFlag (+0x0C, +0x1C)
    TEST_CHECK(VehicleSwitches_IsListWordForTest(2 /*KIND_CARGO*/, 0x00));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(2 /*KIND_CARGO*/, 0x04));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(2 /*KIND_CARGO*/, 0x08));
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(2 /*KIND_CARGO*/, 0x0C));
    TEST_CHECK(VehicleSwitches_IsListWordForTest(2 /*KIND_CARGO*/, 0x18)); // entry[1].amountTons (waste_aluminum)
    TEST_CHECK(!VehicleSwitches_IsListWordForTest(2 /*KIND_CARGO*/, 0x1C)); // entry[1].filterFlag
    return true;
}

static bool SubTest_BuildingStorageAndWagonLoadingSoftSync()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Setup 3 canonical resources: iron, coal, steel
    std::vector<BYTE> resTable(WORLD_RESOURCE_STRIDE * 3, 0);
    const char* resIron  = (const char*)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    const char* resCoal  = (const char*)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);
    const char* resSteel = (const char*)(resTable.data() + 2 * WORLD_RESOURCE_STRIDE);
    strcpy_s((char*)resIron, 32, "iron");
    strcpy_s((char*)resCoal, 32, "coal");
    strcpy_s((char*)resSteel, 32, "steel");
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET)     = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 8) = resTable.data() + resTable.size();

    // 2. Setup Bld [64] steel_mill_v2 with 3 storage slots (0: empty on Host, 1: 120t coal on Host, 2: 10t steel on Host)
    std::vector<BYTE> bld64(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld64.data() = 64;
    *(float*)(bld64.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(float*)(bld64.data() + BLD_PRODUCED_OFFSET) = 10.0f;

    std::vector<BYTE> slots(BLD_STORAGE_SLOT_SIZE * 3, 0);
    std::vector<BYTE> slotEntries(0x10 * 3, 0);
    // Slot 0: empty on Host (sBeg == sEnd, sCap = sBeg + 16)
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x00) = slotEntries.data() + 0x00;
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x00;
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x10) = slotEntries.data() + 0x10;
    // Slot 1: 120t coal on Host
    *(const char**)(slotEntries.data() + 0x10) = resCoal;
    *(float*)(slotEntries.data() + 0x18) = 120.0f;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x00) = slotEntries.data() + 0x10;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x20;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x10) = slotEntries.data() + 0x20;
    // Slot 2: 0t steel on Host (empty)
    *(BYTE**)(slots.data() + 2 * BLD_STORAGE_SLOT_SIZE + 0x00) = slotEntries.data() + 0x20;
    *(BYTE**)(slots.data() + 2 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x20;
    *(BYTE**)(slots.data() + 2 * BLD_STORAGE_SLOT_SIZE + 0x10) = slotEntries.data() + 0x30;

    *(BYTE**)(bld64.data() + BLD_STORAGE_OFFSET)     = slots.data();
    *(BYTE**)(bld64.data() + BLD_STORAGE_OFFSET + 8) = slots.data() + slots.size();
    BYTE* bldList[1] = { bld64.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 1;

    // 3. Setup stopped locomotive #32 + coupled wagon #34 at Bld [64], plus moving truck #38
    std::vector<BYTE> loco(VEHICLE_STRUCT_BYTES, 0), wagon(VEHICLE_STRUCT_BYTES, 0), truck(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(loco.data() + VEH_ID_OFFSET) = 32;
    *(BYTE**)(loco.data() + VEH_STOPPED_BLD_OFFSET) = bld64.data();
    *(unsigned*)(wagon.data() + VEH_ID_OFFSET) = 34;
    *(BYTE**)(wagon.data() + VEH_LINK_PTR_OFFSET) = loco.data();
    *(unsigned*)(truck.data() + VEH_ID_OFFSET) = 38;
    *(float*)(truck.data() + 0xD44) = 42.0f; // moving

    std::vector<BYTE> wagonCargo(0x10, 0);
    *(const char**)wagonCargo.data() = resSteel;
    *(float*)(wagonCargo.data() + 0x08) = 10.0f; // Host wagon has 10.0t steel
    *(BYTE**)(wagon.data() + VEH_CARGO_OFFSET)     = wagonCargo.data();
    *(BYTE**)(wagon.data() + VEH_CARGO_OFFSET + 8) = wagonCargo.data() + 0x10;

    BYTE* vehList[3] = { loco.data(), wagon.data(), truck.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET)     = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 3;

    // 4. Build periodic soft-sync batch (immediate = false): stopped loco & wagon MUST be skipped!
    std::vector<BYTE> wire(8192, 0);
    size_t wireBytes = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, false));
    TEST_CHECK(*(const unsigned*)wire.data() == 1u); // only moving truck #38!

    // 5. Build immediate soft-sync batch (immediate = true): includes all 3 vehicles + Bld [64] storage
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, true));
    TEST_CHECK(*(const unsigned*)wire.data() == 3u);

    // 6. Simulate Client drift:
    //    - Slot 0 on Client has phantom 150.94t iron (sEnd = sBeg + 16)
    //    - Slot 1 on Client is empty (sEnd = sBeg)
    //    - Wagon #34 on Client is actively loading at 10.5t steel (only 0.5t ahead of Host's 10.0t)
    *(const char**)(slotEntries.data() + 0x00) = resIron;
    *(float*)(slotEntries.data() + 0x08) = 150.94f;
    *(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x10;
    *(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x08) = slotEntries.data() + 0x10; // empty on Client
    *(float*)(wagonCargo.data() + 0x08) = 10.5f;

    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireBytes));

    // Verify Client Slot 0 had its storedTons zeroed while preserving sEnd (slot + 8), and Slot 1 was populated with 120t coal!
    TEST_CHECK(*(BYTE**)(slots.data() + 0 * BLD_STORAGE_SLOT_SIZE + 0x08) == slotEntries.data() + 0x10);
    TEST_CHECK_NEAR(*(float*)(slotEntries.data() + 0x08), 0.0f, 1e-4f);
    TEST_CHECK(*(BYTE**)(slots.data() + 1 * BLD_STORAGE_SLOT_SIZE + 0x08) == slotEntries.data() + 0x20);
    TEST_CHECK(*(const char**)(slotEntries.data() + 0x10) == resCoal);
    TEST_CHECK_NEAR(*(float*)(slotEntries.data() + 0x18), 120.0f, 1e-4f);

    // Verify actively loading wagon #34 was NOT jittered back from 10.5t to 10.0t while stopped at Bld [64]
    TEST_CHECK_NEAR(*(float*)(wagonCargo.data() + 0x08), 10.5f, 1e-4f);

    // 7. Now test phantom cargo on stopped wagon when Host has 0.0t and Client has 0.24t:
    *(float*)(wagonCargo.data() + 0x08) = 0.0f;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, true));
    *(float*)(wagonCargo.data() + 0x08) = 0.24f; // Client has phantom 0.24t
    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireBytes));
    TEST_CHECK_NEAR(*(float*)(wagonCargo.data() + 0x08), 0.0f, 1e-4f);

    VehicleSoftSyncDiagStats vss = {};
    VehicleSoftSync_GetDiagStats(&vss);
    TEST_CHECK(vss.bldStorageUpdated >= 2);
    return true;
}

static bool SubTest_VehicleWindowScheduleAndLaunchCommands()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. AddBorderStopForTest on Airplane (cat == 8):
    std::vector<BYTE> planeType(0x400, 0), plane(VEHICLE_STRUCT_BYTES, 0), borderBld(0x400, 0);
    *(int*)(planeType.data() + VEH_TYPE_CATEGORY_OFFSET) = 8; // AIRPLANE
    *(BYTE**)(plane.data() + VEH_TYPE_PTR_OFFSET) = planeType.data();
    *(int*)(plane.data() + 0x182C) = 1;

    std::vector<BYTE> stopRecs(VEH_STOP_RECORD_STRIDE * 2, 0xFF);
    // First stop (nStops == 1): aircraft first stop must set load=0, unload=0
    *(BYTE**)(plane.data() + VEH_STOPS_LIST_OFFSET)     = stopRecs.data();
    *(BYTE**)(plane.data() + VEH_STOPS_LIST_OFFSET + 8) = stopRecs.data() + VEH_STOP_RECORD_STRIDE;
    GameCalls_AddBorderStopForTest((unsigned long long)world, (unsigned long long)plane.data(),
                                   (unsigned long long)borderBld.data(), -1);
    TEST_CHECK(stopRecs[0] == 0 && stopRecs[VEH_STOP_HALF_STRIDE] == 0);

    // Second stop (nStops == 2) with "passengers" in carried summary (+0x458): sets load=1, unload=1
    const char* passName = "passengers";
    BYTE carriedEntry[16] = {};
    *(const char**)carriedEntry = passName;
    *(BYTE**)(plane.data() + 0x458) = carriedEntry;
    *(BYTE**)(plane.data() + 0x460) = carriedEntry + 16;
    *(BYTE**)(plane.data() + VEH_STOPS_LIST_OFFSET + 8) = stopRecs.data() + 2 * VEH_STOP_RECORD_STRIDE;
    GameCalls_AddBorderStopForTest((unsigned long long)world, (unsigned long long)plane.data(),
                                   (unsigned long long)borderBld.data(), -1);
    TEST_CHECK(stopRecs[VEH_STOP_RECORD_STRIDE + 0] == 1);
    TEST_CHECK(stopRecs[VEH_STOP_RECORD_STRIDE + VEH_STOP_HALF_STRIDE] == 1);

    // 2. DeleteStopForTest + ChangeCurrentStopForTest on Train (cat == 4):
    std::vector<BYTE> trainType(0x400, 0), train(VEHICLE_STRUCT_BYTES, 0), stationA(0x100, 0), stationB(0x100, 0);
    *(int*)(trainType.data() + VEH_TYPE_CATEGORY_OFFSET) = 4; // RAIL_LOCOMOTIVE
    *(BYTE**)(train.data() + VEH_TYPE_PTR_OFFSET) = trainType.data();
    BYTE* stopBlds[2] = { stationA.data(), stationB.data() };
    *(BYTE***)(train.data() + VEH_STOP_BLD_PTR_VEC_OFF)     = stopBlds;
    *(BYTE***)(train.data() + VEH_STOP_BLD_PTR_VEC_OFF + 8) = stopBlds + 2;

    // In depot (+0x528 == 0 && +0x530 == 0): ChangeCurrentStop updates +0x698, +0xD78, +0xD79
    GameCalls_ChangeCurrentStopForTest((unsigned long long)train.data(), 1);
    TEST_CHECK(*(int*)(train.data() + VEH_SCHED_IDX_OFFSET) == 1);
    TEST_CHECK(train[0xD78] == 1 && train[0xD79] == 1);

    // Out of depot (+0x530 == stationA): ChangeCurrentStop updates +0x610 to stationB
    *(BYTE**)(train.data() + VEH_STOPPED_BLD_OFFSET) = stationA.data();
    GameCalls_ChangeCurrentStopForTest((unsigned long long)train.data(), 1);
    TEST_CHECK(*(BYTE**)(train.data() + VEH_TARGET_STOP_BLD_OFF) == stationB.data());

    // DeleteStopForTest on stop 0 (stationA): clears +0x530 since it matched stationA
    train[0xD78] = 0;
    GameCalls_DeleteStopForTest((unsigned long long)world, (unsigned long long)train.data(), 0);
    TEST_CHECK(train[0xD78] == 1);
    TEST_CHECK(*(BYTE**)(train.data() + VEH_STOPPED_BLD_OFFSET) == nullptr);

    // 3. ReleaseVehicleForTest on Ship (cat == 7) and Cableway (cat == 6):
    std::vector<BYTE> shipType(0x400, 0), ship(VEHICLE_STRUCT_BYTES, 0), dock(0x100, 0);
    *(int*)(shipType.data() + VEH_TYPE_CATEGORY_OFFSET) = 7; // SHIP in 0x7DA236
    *(BYTE**)(ship.data() + VEH_TYPE_PTR_OFFSET) = shipType.data();
    *(BYTE**)(ship.data() + VEH_CUR_BLD_OFFSET) = dock.data();
    GameCalls_ReleaseVehicleForTest((unsigned long long)ship.data());
    TEST_CHECK(*(BYTE**)(ship.data() + VEH_CUR_BLD_OFFSET) == nullptr);

    std::vector<BYTE> cableType(0x400, 0), cableVeh(VEHICLE_STRUCT_BYTES, 0);
    *(int*)(cableType.data() + VEH_TYPE_CATEGORY_OFFSET) = 6; // CABLEWAY in 0x7DA236
    *(BYTE**)(cableVeh.data() + VEH_TYPE_PTR_OFFSET) = cableType.data();
    GameCalls_ReleaseVehicleForTest((unsigned long long)cableVeh.data());
    TEST_CHECK(cableVeh[VEH_CABLEWAY_START_FLAG_OFF] == 1);

    return true;
}

static bool SubTest_MultiResourceStorageSoftSyncAndManualBuy()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Setup 6 construction/warehouse resources with USD (+0x58) and RUB (+0x5C) prices
    const char* const kResNames[6] = { "gravel", "steel", "bricks", "boards", "prefabpanels", "cement" };
    const float kRubPrices[6]      = { 20.0f, 300.0f, 100.0f, 80.0f, 150.0f, 120.0f };
    const float kHostTons[6]       = { 45.0f,  30.0f,  60.0f, 25.0f,  80.0f,  50.0f };
    std::vector<BYTE> resTable(WORLD_RESOURCE_STRIDE * 6, 0);
    const char* resPtrs[6] = {};
    for (int i = 0; i < 6; ++i) {
        BYTE* r = resTable.data() + (size_t)i * WORLD_RESOURCE_STRIDE;
        strcpy_s((char*)r, 32, kResNames[i]);
        *(float*)(r + RES_PRICE_USD_OFFSET) = kRubPrices[i] * 0.5f;
        *(float*)(r + RES_PRICE_RUB_OFFSET) = kRubPrices[i];
        resPtrs[i] = (const char*)r;
    }
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET)     = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 8) = resTable.data() + resTable.size();

    // 2. Setup multi-resource warehouse building #88 with 1 storage slot containing 6 resource entries
    std::vector<BYTE> bldType(0x400, 0), bld88(BUILDING_STRUCT_BYTES, 0);
    strcpy_s((char*)bldType.data(), 64, "storage_general");
    *(int*)bld88.data() = 88;
    *(BYTE**)(bld88.data() + BLD_TYPE_OFFSET) = bldType.data();
    *(float*)(bld88.data() + OBJ_POS_OFFSET) = -300.0f;
    *(float*)(bld88.data() + BLD_PROGRESS_OFFSET) = 1.0f;

    std::vector<BYTE> slotBuf(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> entriesBuf(0x10 * 6, 0);
    for (int i = 0; i < 6; ++i) {
        BYTE* e = entriesBuf.data() + (size_t)i * 0x10;
        *(const char**)e    = resPtrs[i];
        *(float*)(e + 0x08) = kHostTons[i];
        *(float*)(e + 0x0C) = 25.0f; // autoPurchaseTons slider target
    }
    *(BYTE**)(slotBuf.data() + 0x00) = entriesBuf.data();
    *(BYTE**)(slotBuf.data() + 0x08) = entriesBuf.data() + entriesBuf.size();
    *(BYTE**)(slotBuf.data() + 0x10) = entriesBuf.data() + entriesBuf.size();
    *(BYTE**)(bld88.data() + BLD_STORAGE_OFFSET)     = slotBuf.data();
    *(BYTE**)(bld88.data() + BLD_STORAGE_OFFSET + 8) = slotBuf.data() + slotBuf.size();

    BYTE* bldList[1] = { bld88.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 1;

    // 3. Build VehicleSoftSync packet on Host, then simulate Client having 0.0f storedTons and 25.0f autoPurchaseTons
    std::vector<BYTE> wire(8192, 0);
    size_t wireBytes = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, true));

    for (int i = 0; i < 6; ++i) {
        BYTE* e = entriesBuf.data() + (size_t)i * 0x10;
        *(float*)(e + 0x08) = 0.0f;
        *(float*)(e + 0x0C) = 25.0f;
    }
    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireBytes));

    // Verify all 6 entries are preserved (no truncation to 2), storedTons match Host, and autoPurchaseTons stay 25.0f!
    size_t finalEntryCount = (size_t)((*(BYTE**)(slotBuf.data() + 0x08) - *(BYTE**)(slotBuf.data() + 0x00)) / 0x10);
    TEST_CHECK(finalEntryCount == 6);
    for (int i = 0; i < 6; ++i) {
        const BYTE* e = entriesBuf.data() + (size_t)i * 0x10;
        TEST_CHECK_NEAR(*(const float*)(e + 0x08), kHostTons[i], 1e-4f);
        TEST_CHECK_NEAR(*(const float*)(e + 0x0C), 25.0f, 1e-4f);
    }

    // 4. Verify BuildingSwitches slot offset filtering:
    //    - Region 0 dynamic header (< 0x48, e.g. 0x38 + 8) -> true (excluded)
    //    - Region 1 (+0x1000) storedTons (+0x08) -> true (excluded so UI never reverts soft-synced stock)
    //    - Region 1 (+0x1000) autoPurchaseTons (+0x0C) -> false (carried as slider setting)
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(0x38 + 8));
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 0 * 16 + 8));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 0 * 16 + 12));
    TEST_CHECK(BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 3 * 16 + 8));
    TEST_CHECK(!BuildingSwitches_IsLeftOutSlotOffsetForTest(0x1000 + 3 * 16 + 12));

    // 5. Verify BuildingSwitches_CaptureManualBuy + Lockstep replay (ApplyManualBuyFallback):
    for (int i = 0; i < 6; ++i) {
        *(float*)(entriesBuf.data() + (size_t)i * 0x10 + 0x08) = 0.0f;
    }
    *(int*)(bld88.data() + BLD_EXPORT_MODE_OFFSET) = 1;
    *(double*)(world + WORLD_RUB_OFFSET) = 100000.0;

    std::vector<BYTE> uiSliderSlot(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<BYTE> uiSliderEntries(0x10 * 3, 0);
    *(float*)(uiSliderEntries.data() + 0 * 0x10 + 0x08) = 10.0f; // 10t gravel @ 20 RUB = 200 RUB
    *(float*)(uiSliderEntries.data() + 1 * 0x10 + 0x08) =  0.0f; // 0t steel
    *(float*)(uiSliderEntries.data() + 2 * 0x10 + 0x08) =  5.0f; // 5t bricks @ 100 RUB = 500 RUB
    *(BYTE**)(uiSliderSlot.data() + 0x00) = uiSliderEntries.data();
    *(BYTE**)(uiSliderSlot.data() + 0x08) = uiSliderEntries.data() + uiSliderEntries.size();
    *(BYTE**)(world + WORLD_UI_RES_SLIDERS_OFFSET + 0x00) = uiSliderSlot.data();
    *(BYTE**)(world + WORLD_UI_RES_SLIDERS_OFFSET + 0x08) = uiSliderSlot.data() + uiSliderSlot.size();

    Commands_SetSession(COMMANDS_HOST);
    TEST_CHECK(BuildingSwitches_CaptureManualBuy((unsigned long long)world, (unsigned long long)bld88.data(), 4 /*RUB*/));
    Commands_Boundary(1);
    Commands_SetSession(COMMANDS_OFF);

    TEST_CHECK_NEAR(*(float*)(entriesBuf.data() + 0 * 0x10 + 0x08), 10.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(entriesBuf.data() + 1 * 0x10 + 0x08),  0.0f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(entriesBuf.data() + 2 * 0x10 + 0x08),  5.0f, 1e-4f);
    TEST_CHECK(*(int*)(bld88.data() + BLD_EXPORT_MODE_OFFSET) == 0);
    TEST_CHECK_NEAR(*(double*)(world + WORLD_RUB_OFFSET), 99300.0, 1e-2);

    // 6. Verify 9-component waste container truck (SKD 706R) cargo soft-sync & heap waste Resource* lookup:
    //    Ensures entry[7] (waste_toxic at world + 0xC400) is synced alongside entry[0..1] (waste_metal / waste_aluminum)
    //    so 0x14019CD70 never misclassifies hospital toxic waste as aluminum/metal scrap on the Client!
    const char* const kWasteNames[9] = {
        "waste_metal", "waste_aluminum", "waste_plastic", "waste_bio", "waste_burnable",
        "waste_construction", "waste_ash", "waste_toxic", "waste_other"
    };
    const float kWasteTons[9] = { 0.0f, 0.25f, 0.0f, 0.0f, 0.40f, 0.0f, 0.10f, 1.75f, 0.0f };
    std::vector<BYTE> wasteResPool(WORLD_RESOURCE_STRIDE * 9, 0);
    for (int i = 0; i < 9; ++i) {
        BYTE* wr = wasteResPool.data() + (size_t)i * WORLD_RESOURCE_STRIDE;
        strcpy_s((char*)wr, 32, kWasteNames[i]);
        *(BYTE**)(world + 0xC3D0 + (size_t)i * 8u) = wr;
    }
    std::vector<BYTE> skdTruck(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(skdTruck.data() + VEH_ID_OFFSET) = 121;
    *(float*)(skdTruck.data() + 0xD44) = 25.0f; // moving
    std::vector<BYTE> skdCargo(0x10 * 9, 0);
    for (int i = 0; i < 9; ++i) {
        BYTE* ce = skdCargo.data() + (size_t)i * 0x10;
        *(const char**)ce = (const char*)(wasteResPool.data() + (size_t)i * WORLD_RESOURCE_STRIDE);
        *(float*)(ce + 0x08) = kWasteTons[i];
    }
    *(BYTE**)(skdTruck.data() + VEH_CARGO_OFFSET)     = skdCargo.data();
    *(BYTE**)(skdTruck.data() + VEH_CARGO_OFFSET + 8) = skdCargo.data() + skdCargo.size();
    BYTE* skdList[1] = { skdTruck.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET)     = skdList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = skdList + 1;

    // Also populate slot 0 on bld88 with heap-allocated waste_toxic (entry 0) to verify FindResourceByHash via 0xC360..0xC4C8
    *(const char**)(entriesBuf.data() + 0 * 0x10) = (const char*)(wasteResPool.data() + 7 * WORLD_RESOURCE_STRIDE);
    *(float*)(entriesBuf.data() + 0 * 0x10 + 0x08) = 3.25f;
    *(BYTE**)(slotBuf.data() + 0x08) = entriesBuf.data() + 0x10;

    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireBytes, true));

    // Simulate Client having zeroed entry[7] (waste_toxic) and empty slot 0 (sEnd == sBeg)
    *(float*)(skdCargo.data() + 7 * 0x10 + 0x08) = 0.0f;
    *(BYTE**)(slotBuf.data() + 0x08) = entriesBuf.data();
    memset(entriesBuf.data(), 0, 0x10);
    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireBytes));

    TEST_CHECK_NEAR(*(float*)(skdCargo.data() + 1 * 0x10 + 0x08), 0.25f, 1e-4f);
    TEST_CHECK_NEAR(*(float*)(skdCargo.data() + 7 * 0x10 + 0x08), 1.75f, 1e-4f);
    TEST_CHECK(*(const char**)(entriesBuf.data() + 0 * 0x10) == (const char*)(wasteResPool.data() + 7 * WORLD_RESOURCE_STRIDE));
    TEST_CHECK_NEAR(*(float*)(entriesBuf.data() + 0 * 0x10 + 0x08), 3.25f, 1e-4f);

    return true;
}

static bool SubTest_CoopGameSpeedAndTimerScaling()
{
    ResetMockWorldMemory();
    *(float*)(g_mockExeBuffer + 0x9512F4) = 0.35f; // FAST_FACTOR_RVA (2x)
    *(float*)(g_mockExeBuffer + 0x951188) = 0.05f; // HYPER_FACTOR_RVA (LCtrl+Numpad1)
    *(float*)(g_mockExeBuffer + 0x951100) = 0.01f; // PLAID_FACTOR_RVA (LCtrl+Numpad2 / [>>>] held)
    *(uint8_t*)(g_mockExeBuffer + 0xA1EEF0 + 13) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA1EF20 + 0x5CA) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x1D) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x4F) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x50) = 0;

    // 1. Activate determinism and run initial Prepare: buttons armed, speed 1 (30.0f)
    Determinism_SetActiveForTest(true);
    Determinism_BeforePrepare();
    bool paused = true;
    int speed = 0;
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 1);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 30.0f, 1e-4f);

    // 2. Click UI Speed 2 ([>>]) and verify a second UI frame before Prepare does not clobber it
    Determinism_UiSpeedBegin();
    *(int*)(g_mockExeBuffer + 0xA1F4EC) = 2;
    *(int*)(g_mockExeBuffer + 0xA1F4F4) = 0;
    Determinism_UiSpeedEnd();

    Determinism_UiSpeedBegin();
    Determinism_UiSpeedEnd();

    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 2);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 10.5f, 1e-4f);

    // 3. Hold 4th UI speed button ([>>>]) across two UI frames without touching 0xA1F4F4:
    //    must never spam-toggle pause, and must cap at Hyper (speed 3 -> 30.0f * 0.05f = 1.5f), NOT Plaid (4)
    for (int f = 0; f < 2; ++f) {
        Determinism_UiSpeedBegin();
        *(int*)(g_mockExeBuffer + 0xA1F4EC) = 2;
        *(uint8_t*)(g_mockExeBuffer + 0xA1EF20 + 0x5CA) = 1;
        Determinism_UiSpeedEnd();
    }
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 3);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 1.5f, 1e-4f);

    // 4. Release [>>>], switch to Speed 1 ([>]), and hold LCtrl + Numpad2:
    //    LCtrl + Numpad2 must be completely blocked (stays Speed 1 = 30.0f)
    *(uint8_t*)(g_mockExeBuffer + 0xA1EF20 + 0x5CA) = 0;
    Determinism_UiSpeedBegin();
    *(int*)(g_mockExeBuffer + 0xA1F4EC) = 1;
    *(int*)(g_mockExeBuffer + 0xA1F4F4) = 0;
    Determinism_UiSpeedEnd();

    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x1D) = 1; // LCtrl
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x50) = 1; // Numpad2
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 1);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 30.0f, 1e-4f);

    // 4b. With Speed 2 ([>>]) active, LCtrl + Numpad2 still stays Speed 2 (10.5f)
    TEST_CHECK(Determinism_RequestSpeed(2));
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 2);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 10.5f, 1e-4f);

    // 4c. Holding LCtrl + Numpad1 + Numpad2 simultaneously caps at Hyper (speed 3 = 1.5f)
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x4F) = 1; // Numpad1
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 3);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 1.5f, 1e-4f);

    // 4d. Switch to Speed 1 ([>]) and hold only LCtrl + Numpad1 -> still Speed 3 (1.5f)
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x50) = 0;
    Determinism_UiSpeedBegin();
    *(int*)(g_mockExeBuffer + 0xA1F4EC) = 1;
    *(int*)(g_mockExeBuffer + 0xA1F4F4) = 0;
    Determinism_UiSpeedEnd();

    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 3);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 1.5f, 1e-4f);

    // 5. GameSpeed_OnRemote while Determinism_IsActive() must not overwrite 0xA1F4F4 (kSpeedSentinel)
    int sentinelBefore = *(int*)(g_mockExeBuffer + 0xA1F4F4);
    GameSpeed_OnRemote(0, 1, 1, 0);
    TEST_CHECK(*(int*)(g_mockExeBuffer + 0xA1F4F4) == sentinelBefore);

    // 6. Verify Determinism_RequestSpeed (0/1/2) after clearing boost keys
    *(uint8_t*)(g_mockExeBuffer + 0xA1EF20 + 0x5CA) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x1D) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x4F) = 0;
    *(uint8_t*)(g_mockExeBuffer + 0xA9EC50 + 0x50) = 0;
    TEST_CHECK(!Determinism_RequestSpeed(-1));
    TEST_CHECK(!Determinism_RequestSpeed(3));

    TEST_CHECK(Determinism_RequestSpeed(0));
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(paused);

    TEST_CHECK(Determinism_RequestSpeed(2));
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 2);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 10.5f, 1e-4f);

    TEST_CHECK(Determinism_RequestSpeed(1));
    Determinism_BeforePrepare();
    Determinism_SessionSpeed(&paused, &speed);
    TEST_CHECK(!paused && speed == 1);
    TEST_CHECK_NEAR(*(float*)(g_mockExeBuffer + 0xA1EEF0), 30.0f, 1e-4f);

    // 7. Clean up
    Determinism_SetActiveForTest(false);
    TEST_CHECK(!Determinism_RequestSpeed(1));
    return true;
}

struct CountLimitCmdThreadSync {
    HANDLE readyEvt;
    HANDLE releaseEvt;
};

static DWORD WINAPI CountLimitCmdThreadWorker(LPVOID param)
{
    CountLimitCmdThreadSync* sync = (CountLimitCmdThreadSync*)param;
    TwoStates_BeginCommand(2);
    SetEvent(sync->readyEvt);
    WaitForSingleObject(sync->releaseEvt, 5000);
    TwoStates_EndCommand(2);
    return 0;
}

static bool SubTest_BuildingCountLimitPerStateAndTrailerSoftSync()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // 1. Setup free construction office primary + mirrored types with $COUNT_LIMIT = 7 at +0x208
    std::vector<BYTE> typeMuddyCo(0x400, 0), typeMuddyCoMirror(0x400, 0), typeRegular(0x400, 0);
    *(int32_t*)(typeMuddyCo.data() + BLD_TYPE_COUNT_LIMIT_OFFSET) = 7;
    *(int32_t*)(typeMuddyCoMirror.data() + BLD_TYPE_COUNT_LIMIT_OFFSET) = 7;
    *(int32_t*)(typeRegular.data() + BLD_TYPE_COUNT_LIMIT_OFFSET) = -1;

    struct RawLimitEntry {
        const BYTE* primaryType;
        const BYTE* mirroredType;
        int32_t     currentCount;
        int32_t     customLimitOverride;
    };
    RawLimitEntry rawEntry = { typeMuddyCo.data(), typeMuddyCoMirror.data(), 7, -1 };
    BYTE limitVecHdr[24] = {};
    *(RawLimitEntry**)(limitVecHdr + 0)  = &rawEntry;
    *(RawLimitEntry**)(limitVecHdr + 8)  = &rawEntry + 1;
    *(RawLimitEntry**)(limitVecHdr + 16) = &rawEntry + 1;
    *(BYTE**)(world + WORLD_BLD_COUNT_LIMIT_VEC_PTR_OFFSET) = limitVecHdr;

    // Create 7 free offices on State 1 territory (West, X < 0): 5 primary + 2 mirrored
    std::vector<std::vector<BYTE>> blds(10, std::vector<BYTE>(BUILDING_STRUCT_BYTES, 0));
    std::vector<BYTE*> bldPtrs;
    for (int i = 0; i < 7; ++i) {
        *(BYTE**)(blds[(size_t)i].data() + BLD_TYPE_OFFSET) =
            (i < 5) ? typeMuddyCo.data() : typeMuddyCoMirror.data();
        *(float*)(blds[(size_t)i].data() + OBJ_POS_OFFSET)     = -500.0f - (float)i * 20.0f;
        *(float*)(blds[(size_t)i].data() + OBJ_POS_OFFSET + 8) = 100.0f;
        bldPtrs.push_back(blds[(size_t)i].data());
    }
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldPtrs.data();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldPtrs.data() + bldPtrs.size();

    double initRub[2] = { 100000.0, 100000.0 }, initUsd[2] = { 50000.0, 50000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);

    // State 1 has 7/7 -> blocked; State 2 has 0/7 -> allowed!
    int32_t cur1 = -1, max1 = -1, cur2 = -1, max2 = -1;
    TEST_CHECK(TwoStates_GetBuildingCountLimitEntryForTest(world, limitVecHdr, typeMuddyCo.data(), 1, &cur1, &max1));
    TEST_CHECK(cur1 == 7 && max1 == 7);
    TEST_CHECK(!TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 1));
    TEST_CHECK(!TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCoMirror.data(), 1));
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 1, &cur1, &max1) == typeMuddyCo.data());
    TEST_CHECK(cur1 == 7 && max1 == 7);
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCoMirror.data(), 1, &cur1, &max1) == typeMuddyCo.data());
    TEST_CHECK(cur1 == 7 && max1 == 7);

    TEST_CHECK(TwoStates_GetBuildingCountLimitEntryForTest(world, limitVecHdr, typeMuddyCo.data(), 2, &cur2, &max2));
    TEST_CHECK(cur2 == 0 && max2 == 7);
    TEST_CHECK(TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 2));
    TEST_CHECK(TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCoMirror.data(), 2));
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 2, &cur2, &max2) == typeMuddyCo.data());
    TEST_CHECK(cur2 == 0 && max2 == 7);
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCoMirror.data(), 2, &cur2, &max2) == typeMuddyCo.data());
    TEST_CHECK(cur2 == 0 && max2 == 7);

    // Add 3 free offices on State 2 territory (East, X > 0) -> State 2 now has 3/7 (still allowed)
    for (int i = 7; i < 10; ++i) {
        *(BYTE**)(blds[(size_t)i].data() + BLD_TYPE_OFFSET) =
            (i == 9) ? typeMuddyCoMirror.data() : typeMuddyCo.data();
        *(float*)(blds[(size_t)i].data() + OBJ_POS_OFFSET)     = 500.0f + (float)i * 20.0f;
        *(float*)(blds[(size_t)i].data() + OBJ_POS_OFFSET + 8) = 100.0f;
        bldPtrs.push_back(blds[(size_t)i].data());
    }
    rawEntry.currentCount = 10;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldPtrs.data();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldPtrs.data() + bldPtrs.size();

    TEST_CHECK(TwoStates_GetBuildingCountLimitEntryForTest(world, limitVecHdr, typeMuddyCoMirror.data(), 2, &cur2, &max2));
    TEST_CHECK(cur2 == 3 && max2 == 7);
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 2, &cur2, &max2) == typeMuddyCo.data());
    TEST_CHECK(cur2 == 3 && max2 == 7);
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCoMirror.data(), 2, &cur2, &max2) == typeMuddyCo.data());
    TEST_CHECK(cur2 == 3 && max2 == 7);
    TEST_CHECK(TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 2));
    TEST_CHECK(!TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 1));
    TEST_CHECK(TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeRegular.data(), 1));
    TEST_CHECK(!TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, nullptr, 1));
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeRegular.data(), 1, &cur1, &max1) == nullptr);
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, nullptr, 1, &cur1, &max1) == nullptr);

    // Negative customLimitOverride (< 0) resolves from +0x208; positive override takes precedence
    rawEntry.customLimitOverride = -2;
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 2, &cur2, &max2) == typeMuddyCo.data());
    TEST_CHECK(cur2 == 3 && max2 == 7);
    rawEntry.customLimitOverride = 3;
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 2, &cur2, &max2) == typeMuddyCo.data());
    TEST_CHECK(cur2 == 3 && max2 == 3);
    TEST_CHECK(!TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 2));
    rawEntry.customLimitOverride = -1;

    // Cross-thread isolation: Sim-thread TwoStates_BeginCommand(2) must NOT leak State 2 into UI-thread checks/tooltips
    CountLimitCmdThreadSync threadSync = {
        CreateEventW(nullptr, TRUE, FALSE, nullptr),
        CreateEventW(nullptr, TRUE, FALSE, nullptr)
    };
    TEST_CHECK(threadSync.readyEvt != nullptr && threadSync.releaseEvt != nullptr);
    HANDLE cmdThread = CreateThread(nullptr, 0, &CountLimitCmdThreadWorker, &threadSync, 0, nullptr);
    bool crossThreadOk = false;
    if (cmdThread != nullptr && WaitForSingleObject(threadSync.readyEvt, 5000) == WAIT_OBJECT_0) {
        int32_t uiCur = -1, uiMax = -1;
        const BYTE* uiPrimary = TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 0, &uiCur, &uiMax);
        bool uiCanPlace = TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 0);
        crossThreadOk = (uiPrimary == typeMuddyCo.data() && uiCur == 7 && uiMax == 7 && !uiCanPlace);
    }
    SetEvent(threadSync.releaseEvt);
    if (cmdThread != nullptr) {
        WaitForSingleObject(cmdThread, 5000);
        CloseHandle(cmdThread);
    }
    CloseHandle(threadSync.readyEvt);
    CloseHandle(threadSync.releaseEvt);
    TEST_CHECK(crossThreadOk);

    // Singleplayer / One State fallback uses global currentCount from limitTableVec
    TwoStates_OnSessionEnded();
    rawEntry.currentCount = 6;
    TEST_CHECK(TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 0));
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 0, &cur1, &max1) == typeMuddyCo.data());
    TEST_CHECK(cur1 == 6 && max1 == 7);
    rawEntry.currentCount = 7;
    TEST_CHECK(!TwoStates_CheckBuildingCountLimitForTest(world, limitVecHdr, typeMuddyCo.data(), 0));
    TEST_CHECK(TwoStates_ResolveTooltipCountLimitForTest(world, typeMuddyCo.data(), 0, &cur1, &max1) == typeMuddyCo.data());
    TEST_CHECK(cur1 == 7 && max1 == 7);

    // 2. Verify towed child trailer (+0x3A0..+0x3A8) is translated together with parent vehicle during SoftSync
    HeldFrame_Reset();
    std::vector<BYTE> parentTruck(VEHICLE_STRUCT_BYTES, 0), childTrailer(VEHICLE_STRUCT_BYTES, 0);
    *(int32_t*)(parentTruck.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
    *(int32_t*)(childTrailer.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
    const unsigned nodeOffsets[5] = { 0xD98, 0xE54, 0xF10, 0xFCC, 0x10B8 };
    for (int k = 0; k < 5; ++k) {
        float* np = (float*)(parentTruck.data() + nodeOffsets[k]);
        float* nc = (float*)(childTrailer.data() + nodeOffsets[k]);
        np[0] = np[5] = np[10] = np[15] = 1.0f;
        nc[0] = nc[5] = nc[10] = nc[15] = 1.0f;
        np[32] = 100.0f; np[33] = 10.0f; np[34] = 200.0f;
        nc[32] =  90.0f; nc[33] = 10.0f; nc[34] = 200.0f;
    }
    BYTE* childList[1] = { childTrailer.data() };
    *(BYTE***)(parentTruck.data() + VEH_CHILDREN_BEGIN_OFFSET) = childList;
    *(BYTE***)(parentTruck.data() + VEH_CHILDREN_END_OFFSET)   = childList + 1;
    *(BYTE**)(childTrailer.data() + VEH_PARENT_PTR_OFFSET)     = parentTruck.data();

    VehicleSoftSync_TranslateWithChildrenForTest(parentTruck.data(), 5.0f, 1.5f, -3.0f, true);
    TEST_CHECK_NEAR(*(const float*)(parentTruck.data() + 0xD98 + 0x80), 105.0f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(parentTruck.data() + 0xF10 + 0x84),  11.5f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(childTrailer.data() + 0xD98 + 0x80), 95.0f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(childTrailer.data() + 0xD98 + 0x84), 11.5f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(childTrailer.data() + 0xD98 + 0x88), 197.0f, 1e-4f);
    return true;
}

bool Test_VehicleCargoAndScheduleSync()
{
    return SubTest_StoppedTrainAndWagonEffectiveBld() &&
           SubTest_BuildingStorageAndWagonLoadingSoftSync() &&
           SubTest_VehicleWindowScheduleAndLaunchCommands() &&
           SubTest_MultiResourceStorageSoftSyncAndManualBuy() &&
           SubTest_CoopGameSpeedAndTimerScaling() &&
           SubTest_BuildingCountLimitPerStateAndTrailerSoftSync();
}

// ---------------------------------------------------------------------------
// Challenger M4 Stress Test Suite: Adversarial Verification of R3, R10, R11, R17
// ---------------------------------------------------------------------------

#ifndef IMPORT_HAS_SOUND_RVA
#define IMPORT_HAS_SOUND_RVA    0x86CD10
#endif
#ifndef IMPORT_GET_SOUND_RVA
#define IMPORT_GET_SOUND_RVA    0x86CD18
#endif
#ifndef SOUND_ENGINE_PTR_RVA
#define SOUND_ENGINE_PTR_RVA    0xA2ECC8
#endif
#ifndef SOUND_MGR_RVA
#define SOUND_MGR_RVA           0xA2ECD0
#endif
#ifndef M4_BUILDINGS_BEGIN_RVA
#define M4_BUILDINGS_BEGIN_RVA  0xA30AD8
#define M4_BUILDINGS_END_RVA    0xA30AE0
#endif

// 1. R10: Vehicle SoftSync, Smoothing, EMA Decay & Passengers
static bool SubTest_Stress_M4_R10_VehicleSmoothingAndPassengers()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    // Bus setup with negative base coordinates
    std::vector<BYTE> bus(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(bus.data() + VEH_ID_OFFSET) = 777;
    *(float*)(bus.data() + VEH_POS_OFFSET)     = -1000.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 4) = -500.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 8) = -200.0f;
    *(int*)(bus.data() + VEH_SCHED_IDX_OFFSET) = 1;
    *(BYTE**)(bus.data() + VEH_STOPPED_BLD_OFFSET) = nullptr;

    // 16-byte passenger summary struct at +0x458 .. +0x460
    static const char kPassLabel[] = "passengers";
    struct MockPersonEntry {
        const char* name;
        float amount;
        unsigned int pad;
    };
    std::vector<MockPersonEntry> passVector;
    passVector.push_back({ kPassLabel, 35.0f, 0 });
    *(BYTE**)(bus.data() + 0x458) = (BYTE*)passVector.data();
    *(BYTE**)(bus.data() + 0x460) = (BYTE*)(passVector.data() + passVector.size());

    BYTE* vehList[1] = { bus.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET) = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    // Build soft-sync packet from local vehicle state
    std::vector<BYTE> pkt(2048, 0);
    size_t sz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(pkt.data(), pkt.size(), &sz, true));
    VehicleSoftSyncMsg_M4* msg = (VehicleSoftSyncMsg_M4*)pkt.data();
    TEST_CHECK(msg->count == 1);
    TEST_CHECK_NEAR(msg->entries[0].passengers, 35.0f, 1e-4f);

    VehicleSoftSyncDiagStats vssStats = {};

    // --- Adversarial Coordinate Jumps ---
    // A. Small drift in negative coordinates: 30m drift (< 50m) -> EMA offset blended
    msg->entries[0].pos[0] = -970.0f;
    msg->entries[0].pos[1] = -500.0f;
    msg->entries[0].pos[2] = -200.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 1);
    TEST_CHECK(vssStats.posSnapped == 0);

    // B. Boundary drift 49.9m (< 50m) -> EMA offset blended (distSq = 2490.01f < 2500.0f)
    // Vehicle node pos was translated to -970.0f. Target: -970.0f + 49.9f = -920.1f
    msg->entries[0].pos[0] = -920.1f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 0);

    // C. Boundary drift 50.1m (>= 50m) -> Hard Snap directly (distSq = 2510.01f >= 2500.0f)
    // Vehicle node pos was translated to -920.1f. Target: -920.1f + 50.1f = -870.0f
    msg->entries[0].pos[0] = -870.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 1);

    // D. Boundary drift exactly 50.0m (>= 50m) -> Hard Snap directly (distSq = 2500.0f >= 2500.0f)
    // Vehicle node pos was translated to -870.0f. Target: -870.0f + 50.0f = -820.0f
    msg->entries[0].pos[0] = -820.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 2);

    // E. Pathological non-finite coordinates (NaN and Inf) -> Ignored, no crash
    msg->entries[0].pos[0] = NAN;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    msg->entries[0].pos[0] = INFINITY;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 2);

    // F. Extreme outlier drift >= 1000m (distSq >= 1e6f) -> Snaps cleanly (1000m cap removed per R10)
    msg->entries[0].pos[0] = 5000.0f; // drift > 5800m
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    VehicleSoftSync_GetDiagStats(&vssStats);
    TEST_CHECK(vssStats.posBlended == 2);
    TEST_CHECK(vssStats.posSnapped == 3);

    // --- Passenger Count Synchronization Boundary Cases ---
    msg->entries[0].pos[0] = *(float*)(bus.data() + VEH_POS_OFFSET); // zero drift
    // 1. 0 passengers
    msg->entries[0].passengers = 0.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVector[0].amount, 0.0f, 1e-4f);

    // 2. Negative passengers (-10.0f) -> must be rejected, preserving 0.0f
    msg->entries[0].passengers = -10.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVector[0].amount, 0.0f, 1e-4f);

    // 3. Maximum float passengers (FLT_MAX)
    msg->entries[0].passengers = 3.402823466e+38f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(passVector[0].amount, 3.402823466e+38f, 1e20f);

    // 4. Non-finite passengers (NaN / Inf) -> must be rejected
    msg->entries[0].passengers = NAN;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    msg->entries[0].passengers = INFINITY;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK(isfinite(passVector[0].amount));

    // 5. Vehicle without passenger vector header (nullptr at 0x458) -> safe, no crash
    *(BYTE**)(bus.data() + 0x458) = nullptr;
    *(BYTE**)(bus.data() + 0x460) = nullptr;
    msg->entries[0].passengers = 25.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));

    // 6. Vehicle with active road segment (+0x528) or waypoint queue (+0x738..+0x740) ->
    //    must NOT yank C3D_NODE (+0xD98) off the road spline into fields, while still syncing passengers
    *(BYTE**)(bus.data() + 0x458) = (BYTE*)passVector.data();
    *(BYTE**)(bus.data() + 0x460) = (BYTE*)(passVector.data() + passVector.size());
    *(void**)(bus.data() + 0x528) = (void*)0x140001000ULL;
    float splinePosX = *(float*)(bus.data() + VEH_POS_OFFSET);
    msg->entries[0].pos[0] = splinePosX + 45.0f;
    msg->entries[0].passengers = 42.0f;
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_POS_OFFSET), splinePosX, 1e-4f);
    TEST_CHECK_NEAR(passVector[0].amount, 42.0f, 1e-4f);
    *(void**)(bus.data() + 0x528) = nullptr;

    // 7. Trail buffer (+0x1168..+0x12E7, 24 entries) + cached front/rear axle positions (+0x12F8, +0x1304)
    //    co-translation and VEH_CORNER_TURN_ANGLE_OFFSET (+0x770) preservation:
    //    Ensures airplanes parking on stands (0x6CE180) and buses entering terminals (0x699420 / 0x6AF0C0)
    //    never twist crookedly from unshifted rear-axle trail history or overwritten corner turn angle.
    *(float*)(bus.data() + VEH_POS_OFFSET + 0) = 100.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 4) = 10.0f;
    *(float*)(bus.data() + VEH_POS_OFFSET + 8) = 200.0f;
    *(float*)(bus.data() + VEH_CORNER_TURN_ANGLE_OFFSET) = 0.5235988f; // 30 deg turn angle
    float* trail0 = (float*)(bus.data() + VEH_TRAIL_BUFFER_OFFSET + 0 * VEH_TRAIL_ENTRY_STRIDE);
    float* trail1 = (float*)(bus.data() + VEH_TRAIL_BUFFER_OFFSET + 1 * VEH_TRAIL_ENTRY_STRIDE);
    float* trail2 = (float*)(bus.data() + VEH_TRAIL_BUFFER_OFFSET + 2 * VEH_TRAIL_ENTRY_STRIDE);
    trail0[0] = 98.0f; trail0[1] = 10.0f; trail0[2] = 198.0f; trail0[3] = 1.25f;
    trail1[0] = 96.0f; trail1[1] = 10.0f; trail1[2] = 196.0f; trail1[3] = 1.25f;
    trail2[0] = 0.0f;  trail2[1] = 0.0f;  trail2[2] = 0.0f;   trail2[3] = 0.0f; // uninitialized slot
    float* frontAxle = (float*)(bus.data() + VEH_AXLE_FRONT_POS_OFFSET);
    float* rearAxle  = (float*)(bus.data() + VEH_AXLE_REAR_POS_OFFSET);
    frontAxle[0] = 103.0f; frontAxle[1] = 10.0f; frontAxle[2] = 203.0f;
    rearAxle[0]  = 95.0f;  rearAxle[1]  = 10.0f; rearAxle[2]  = 195.0f;

    msg->entries[0].pos[0] = 120.0f; // +20m X
    msg->entries[0].pos[1] = 12.0f;  // +2m Y
    msg->entries[0].pos[2] = 215.0f; // +15m Z
    msg->entries[0].fuel   = 99.0f;  // must NOT overwrite VEH_CORNER_TURN_ANGLE_OFFSET (+0x770)
    TEST_CHECK(VehicleSoftSync_ApplyCommand(pkt.data(), sz));
    TEST_CHECK_NEAR(*(float*)(bus.data() + VEH_CORNER_TURN_ANGLE_OFFSET), 0.5235988f, 1e-5f);
    TEST_CHECK_NEAR(trail0[0], 118.0f, 1e-4f);
    TEST_CHECK_NEAR(trail0[1], 12.0f,  1e-4f);
    TEST_CHECK_NEAR(trail0[2], 213.0f, 1e-4f);
    TEST_CHECK_NEAR(trail0[3], 1.25f,  1e-4f); // heading in trail[3] preserved
    TEST_CHECK_NEAR(trail1[0], 116.0f, 1e-4f);
    TEST_CHECK_NEAR(trail1[2], 211.0f, 1e-4f);
    TEST_CHECK_NEAR(trail2[0], 0.0f,   1e-6f); // uninitialized zero entry untouched
    TEST_CHECK_NEAR(frontAxle[0], 123.0f, 1e-4f);
    TEST_CHECK_NEAR(frontAxle[1], 12.0f,  1e-4f);
    TEST_CHECK_NEAR(frontAxle[2], 218.0f, 1e-4f);
    TEST_CHECK_NEAR(rearAxle[0],  115.0f, 1e-4f);
    TEST_CHECK_NEAR(rearAxle[1],  12.0f,  1e-4f);
    TEST_CHECK_NEAR(rearAxle[2],  210.0f, 1e-4f);

    // 8. Terminal & attached aircraft stand arrival/corridor lock sanitization (0x6CD1E0):
    //    Verifies that an airborne departed plane releases TERM->0xBB0, duplicate stand reservations
    //    across attached stands are deduplicated to the closest stand entry point p2, and an unassigned
    //    arriving plane gets its stand index (+0x56C) and 2 pull-in waypoints (p2 -> p1).
    std::vector<BYTE> termBld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> att0Bld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> att1Bld(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> termBayPts(3 * BLD_BAY_POINT_STRIDE, 0);
    std::vector<BYTE> att0BayPts(1 * BLD_BAY_POINT_STRIDE, 0);
    std::vector<BYTE> att1BayPts(3 * BLD_BAY_POINT_STRIDE, 0);
    *(BYTE**)(termBld.data() + BLD_BAY_POINTS_BEGIN_OFFSET) = termBayPts.data();
    *(BYTE**)(termBld.data() + BLD_BAY_POINTS_END_OFFSET)   = termBayPts.data() + termBayPts.size();
    *(BYTE**)(att0Bld.data() + BLD_BAY_POINTS_BEGIN_OFFSET) = att0BayPts.data();
    *(BYTE**)(att0Bld.data() + BLD_BAY_POINTS_END_OFFSET)   = att0BayPts.data() + att0BayPts.size();
    *(BYTE**)(att1Bld.data() + BLD_BAY_POINTS_BEGIN_OFFSET) = att1BayPts.data();
    *(BYTE**)(att1Bld.data() + BLD_BAY_POINTS_END_OFFSET)   = att1BayPts.data() + att1BayPts.size();

    std::vector<BYTE*> attList = { att0Bld.data(), att1Bld.data() };
    *(BYTE***)(termBld.data() + BLD_ATTACHED_BEGIN_OFFSET) = attList.data();
    *(BYTE***)(termBld.data() + BLD_ATTACHED_END_OFFSET)   = attList.data() + attList.size();

    std::vector<BYTE> airType(0x400, 0);
    *(int32_t*)(airType.data() + VEH_TYPE_CATEGORY_OFFSET) = 8; // aircraft
    std::vector<BYTE> departedPlane(VEHICLE_STRUCT_BYTES, 0);
    std::vector<BYTE> arrivingPlane(VEHICLE_STRUCT_BYTES, 0);
    *(BYTE**)(departedPlane.data() + VEH_TYPE_PTR_OFFSET) = airType.data();
    *(BYTE**)(arrivingPlane.data() + VEH_TYPE_PTR_OFFSET) = airType.data();
    *(BYTE**)(departedPlane.data() + 0x390) = departedPlane.data();
    *(BYTE**)(arrivingPlane.data() + 0x390) = arrivingPlane.data();

    // Departed plane is airborne (1A04 = 3) but left TERM->0xBB0 locked
    *(int32_t*)(departedPlane.data() + VEH_HELI_FLIGHT_STATE_OFF) = 3;
    *(BYTE**)(departedPlane.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) = termBld.data();
    *(BYTE**)(termBld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) = departedPlane.data();

    // Arriving plane is on ground at (4926, 199.5, -4916) with duplicate reservations in ATT[0].slot[0] (far) and ATT[1].slot[0] (near)
    *(float*)(arrivingPlane.data() + VEH_POS_OFFSET + 0) = 4926.0f;
    *(float*)(arrivingPlane.data() + VEH_POS_OFFSET + 4) = 199.5f;
    *(float*)(arrivingPlane.data() + VEH_POS_OFFSET + 8) = -4916.0f;
    *(BYTE**)(arrivingPlane.data() + VEH_BAY_TARGET_BLD_OFFSET) = termBld.data();
    *(int32_t*)(arrivingPlane.data() + VEH_BAY_SLOT_IDX_OFFSET) = -1;
    *(int32_t*)(arrivingPlane.data() + VEH_LINE_SLOT_IDX_OFFSET) = -1;
    *(uint8_t*)(arrivingPlane.data() + VEH_BAY_ARRIVED_FLAG_OFFSET) = 1; // corrupted D39=1 while 56C=-1

    float* att0s0p2 = (float*)(att0BayPts.data() + 0x0C);
    att0s0p2[0] = 4934.6f; att0s0p2[1] = 199.5f; att0s0p2[2] = -4999.9f; // 84m away
    att0Bld[BLD_BAY_STATE_BEGIN_OFFSET + 0] = 1;
    *(BYTE**)(att0Bld.data() + BLD_BAY_VEH_BEGIN_OFFSET + 0) = arrivingPlane.data();

    float* att1s0p1 = (float*)(att1BayPts.data() + 0x00);
    float* att1s0p2 = (float*)(att1BayPts.data() + 0x0C);
    att1s0p1[0] = 4917.5f; att1s0p1[1] = 199.5f; att1s0p1[2] = -4951.7f;
    att1s0p2[0] = 4919.8f; att1s0p2[1] = 199.5f; att1s0p2[2] = -4905.8f; // 12m away
    att1Bld[BLD_BAY_STATE_BEGIN_OFFSET + 0] = 1;
    *(BYTE**)(att1Bld.data() + BLD_BAY_VEH_BEGIN_OFFSET + 0) = arrivingPlane.data();

    std::vector<float> wpBuf(12, 0.0f); // capacity for 4 waypoints (12 floats)
    *(uintptr_t*)(arrivingPlane.data() + VEH_WAYPOINTS_BEGIN_OFFSET) = (uintptr_t)wpBuf.data();
    *(uintptr_t*)(arrivingPlane.data() + VEH_WAYPOINTS_END_OFFSET)   = (uintptr_t)(wpBuf.data() + 9); // 3 bogus waypoints
    *(uintptr_t*)(arrivingPlane.data() + VEH_WAYPOINTS_CAP_OFFSET)   = (uintptr_t)(wpBuf.data() + wpBuf.size());

    SimPatches_TestSanitizeVehicleArrival(arrivingPlane.data());

    // TERM->0xBB0 must be freed from the airborne plane
    TEST_CHECK(*(BYTE**)(termBld.data() + BLD_CORRIDOR_LOCK_VEH_OFFSET) == nullptr);
    TEST_CHECK(*(BYTE**)(departedPlane.data() + VEH_CORRIDOR_LOCK_BLD_OFFSET) == nullptr);
    // Duplicate reservation on ATT[0].slot[0] must be cleared, and ATT[1].slot[0] (global slot 3 + 1 + 0 = 4) assigned
    TEST_CHECK(att0Bld[BLD_BAY_STATE_BEGIN_OFFSET + 0] == 0);
    TEST_CHECK(*(BYTE**)(att0Bld.data() + BLD_BAY_VEH_BEGIN_OFFSET + 0) == nullptr);
    TEST_CHECK(att1Bld[BLD_BAY_STATE_BEGIN_OFFSET + 0] == 1);
    TEST_CHECK(*(BYTE**)(att1Bld.data() + BLD_BAY_VEH_BEGIN_OFFSET + 0) == arrivingPlane.data());
    TEST_CHECK(*(int32_t*)(arrivingPlane.data() + VEH_BAY_SLOT_IDX_OFFSET) == 4);
    TEST_CHECK(arrivingPlane[VEH_BAY_ARRIVED_FLAG_OFFSET] == 0);
    // Waypoints must now be exactly 2 points: p2 then p1 of ATT[1].slot[0]
    uintptr_t newWpBegin = *(uintptr_t*)(arrivingPlane.data() + VEH_WAYPOINTS_BEGIN_OFFSET);
    uintptr_t newWpEnd   = *(uintptr_t*)(arrivingPlane.data() + VEH_WAYPOINTS_END_OFFSET);
    TEST_CHECK(newWpEnd - newWpBegin == 24);
    TEST_CHECK_NEAR(wpBuf[0], 4919.8f, 1e-4f);
    TEST_CHECK_NEAR(wpBuf[2], -4905.8f, 1e-4f);
    TEST_CHECK_NEAR(wpBuf[3], 4917.5f, 1e-4f);
    TEST_CHECK_NEAR(wpBuf[5], -4951.7f, 1e-4f);

    memset(world + WORLD_VEHICLES_OFFSET, 0, 24);
    return true;
}

// 2. R11: Vehicle Purchase Sound Muting, Null Pointers & Rapid Fire Purchases
static int s_mockM4SoundCount = 0;
static const char* s_lastM4Sound = nullptr;

static bool MockM4HasSound(void*, const char*) { return true; }
static const char* MockM4GetSound(void*, const char* name) { return name; }
static void MockM4PlaySound2D(void*, const char* name, bool, float, void*) {
    s_mockM4SoundCount++;
    s_lastM4Sound = name;
}

static void TestTriggerPurchaseSoundSafe()
{
    if (!g_mockExeBuffer) return;
    __try {
        if (*(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) == 0 &&
            *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) != nullptr) {
            uintptr_t hasSndAddr  = *(uintptr_t*)(g_mockExeBuffer + IMPORT_HAS_SOUND_RVA);
            uintptr_t getSndAddr  = *(uintptr_t*)(g_mockExeBuffer + IMPORT_GET_SOUND_RVA);
            uintptr_t playSndAddr = *(uintptr_t*)(g_mockExeBuffer + IMPORT_PLAY_SOUND_RVA);
            void* soundEngine = *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA);
            void* soundCfg = (void*)(g_mockExeBuffer + SOUND_MGR_RVA);
            if (soundEngine && playSndAddr) {
                typedef bool (*FnHasSound)(void*, const char*);
                typedef const char* (*FnGetSound)(void*, const char*);
                typedef void (*FnPlaySound2D)(void*, const char*, bool, float, void*);
                const char* sndPath = nullptr;
                if (hasSndAddr && getSndAddr && ((FnHasSound)hasSndAddr)(soundCfg, "gui_click")) {
                    sndPath = ((FnGetSound)getSndAddr)(soundCfg, "gui_click");
                }
                if (sndPath && sndPath[0]) {
                    ((FnPlaySound2D)playSndAddr)(soundEngine, sndPath, true, 1.0f, nullptr);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool SubTest_Stress_M4_R11_VehiclePurchaseSoundStress()
{
    ResetMockWorldMemory();
    s_mockM4SoundCount = 0;
    s_lastM4Sound = nullptr;

    static int s_engineToken = 0xBEEF;
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = &s_engineToken;
    *(uintptr_t*)(g_mockExeBuffer + IMPORT_HAS_SOUND_RVA)  = (uintptr_t)&MockM4HasSound;
    *(uintptr_t*)(g_mockExeBuffer + IMPORT_GET_SOUND_RVA)  = (uintptr_t)&MockM4GetSound;
    *(uintptr_t*)(g_mockExeBuffer + IMPORT_PLAY_SOUND_RVA) = (uintptr_t)&MockM4PlaySound2D;

    // 1. Sound plays when mute byte is 0
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    TestTriggerPurchaseSoundSafe();
    TEST_CHECK(s_mockM4SoundCount == 1);
    TEST_CHECK(s_lastM4Sound != nullptr && strcmp(s_lastM4Sound, "gui_click") == 0);

    // 2. Sound is completely silenced when mute byte is 1
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 1;
    TestTriggerPurchaseSoundSafe();
    TEST_CHECK(s_mockM4SoundCount == 1); // count unchanged!

    // 3. Sound engine null pointer safety
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = nullptr;
    TestTriggerPurchaseSoundSafe();
    TEST_CHECK(s_mockM4SoundCount == 1); // no crash, no sound

    // 4. Multiple rapid consecutive vehicle purchases (500 iterations)
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = &s_engineToken;
    for (int i = 0; i < 500; ++i) {
        TestTriggerPurchaseSoundSafe();
    }
    TEST_CHECK(s_mockM4SoundCount == 501);

    // Reset
    *(BYTE*)(g_mockExeBuffer + SOUND_MUTE_BYTE_RVA) = 0;
    *(void**)(g_mockExeBuffer + SOUND_ENGINE_PTR_RVA) = nullptr;
    return true;
}

// 3. R17: Oil Pump Rendering Hook (0 buildings, 10,000 buildings, null vector, exception safety)
static void* MockM4_BuildingRenderPass_Normal(void*, void*, void*, void*)
{
    return (void*)0x1234;
}

static void* MockM4_BuildingRenderPass_Exception(void*, void*, void*, void*)
{
    RaiseException(0xC0000005, 0, 0, nullptr);
    return nullptr;
}

typedef void* (*M4RenderPassFn)(void*, void*, void*, void*);

static void* RunSimulatedBuildingRenderPass(M4RenderPassFn passFn, bool simulateAllocFail, bool& exceptionThrown)
{
    exceptionThrown = false;
    VisibleKept* kept = nullptr;
    size_t keptCount = 0;

    BYTE** it = *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA);
    BYTE** end = *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA);

    if (it && end > it) {
        size_t total = (size_t)(end - it);
        if (total > 0 && total < 100000) {
            kept = simulateAllocFail ? nullptr : (VisibleKept*)malloc(total * sizeof(VisibleKept));
            if (kept) {
                for (; it < end; ++it) {
                    BYTE* b = *it;
                    if (!b) continue;
                    float dist = *(float*)(b + 0x115C);
                    if (dist < 4000.0f && isfinite(dist) && keptCount < total) {
                        kept[keptCount].building = b;
                        kept[keptCount].visible = b[0x115A];
                        kept[keptCount].distance = *(float*)(b + 0x115C);
                        keptCount++;
                        b[0x115A] = 1;
                        *(float*)(b + 0x115C) = dist;
                    }
                }
            }
        }
    }

    void* res = nullptr;
    __try {
        __try {
            res = passFn(nullptr, nullptr, nullptr, nullptr);
        }
        __finally {
            RestoreVisibleKeptSafe(kept, keptCount);
            if (kept) {
                free(kept);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionThrown = true;
    }

    return res;
}

static bool SubTest_Stress_M4_R17_OilPumpRenderingHookStress()
{
    ResetMockWorldMemory();

    // 1. Test with 0 buildings
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA) = nullptr;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA)   = nullptr;
    bool exc = false;
    void* res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Normal, false, exc);
    TEST_CHECK(res == (void*)0x1234 && !exc);

    // 2. Test with 10,000 buildings
    const size_t kBldCount = 10000;
    std::vector<std::vector<BYTE>> bldStorage(kBldCount, std::vector<BYTE>(BUILDING_STRUCT_BYTES, 0));
    std::vector<BYTE*> bldPtrs(kBldCount);

    for (size_t i = 0; i < kBldCount; i++) {
        bldPtrs[i] = bldStorage[i].data();
        bldStorage[i][0x115A] = 0;
        float dist = (i < 5000) ? 500.0f : 6000.0f;
        *(float*)(bldStorage[i].data() + 0x115C) = dist;
    }

    BYTE** itBegin = bldPtrs.data();
    BYTE** itEnd   = bldPtrs.data() + kBldCount;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA) = itBegin;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA)   = itEnd;

    res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Normal, false, exc);
    TEST_CHECK(res == (void*)0x1234 && !exc);

    // Verify all 10,000 buildings strictly restored to original values!
    for (size_t i = 0; i < kBldCount; i++) {
        TEST_CHECK(bldStorage[i][0x115A] == 0);
        float expectedDist = (i < 5000) ? 500.0f : 6000.0f;
        TEST_CHECK_NEAR(*(float*)(bldStorage[i].data() + 0x115C), expectedDist, 1e-4f);
    }

    // 3. Test simulated allocation failure (malloc returns null)
    res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Normal, true, exc);
    TEST_CHECK(res == (void*)0x1234 && !exc);

    // 4. Exception during building render pass
    // When an exception occurs during passFn, __finally guarantees restoration via RestoreVisibleKeptSafe!
    res = RunSimulatedBuildingRenderPass(MockM4_BuildingRenderPass_Exception, false, exc);
    TEST_CHECK(exc == true);
    // The building visibility byte 0x115A is strictly restored to 0!
    bool stuckAtOne = (bldStorage[0][0x115A] == 1);
    TEST_CHECK(stuckAtOne == false);
    TEST_CHECK(bldStorage[0][0x115A] == 0);

    // Clean up
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_BEGIN_RVA) = nullptr;
    *(BYTE***)(g_mockExeBuffer + M4_BUILDINGS_END_RVA)   = nullptr;
    return true;
}

// 4. R3: Placement Tool Badge (Screen projection, camera behind object, extreme FOVs, Cyrillic / Charset)
static bool SubTest_Stress_M4_R3_PlacementBadgeProjectionAndCharset()
{
    // A. Projection behind camera and threshold boundary
    auto Project = [](float wx, float wy, float wz, float& outNx, float& outNy, float& outW) -> bool {
        outW = wz;
        if (outW <= 0.5f || !isfinite(outW)) return false;
        float rcpW = 1.0f / outW;
        outNx = wx * rcpW;
        outNy = wy * rcpW;
        return (outNx >= -1.5f && outNx <= 1.5f && outNy >= -1.5f && outNy <= 1.5f);
    };

    float nx = 0.0f, ny = 0.0f, w = 0.0f;

    // 1. Object directly in front of camera at 20m -> accepted
    TEST_CHECK(Project(0.0f, 0.0f, 20.0f, nx, ny, w) == true);
    TEST_CHECK(w == 20.0f);

    // 2. Object behind camera (z = -10m) -> rejected
    TEST_CHECK(Project(0.0f, 0.0f, -10.0f, nx, ny, w) == false);

    // 3. Object at camera plane (z = 0m) -> rejected
    TEST_CHECK(Project(0.0f, 0.0f, 0.0f, nx, ny, w) == false);

    // 4. Boundary threshold: 0.49m (< 0.5m) -> rejected
    TEST_CHECK(Project(0.0f, 0.0f, 0.49f, nx, ny, w) == false);

    // 5. Boundary threshold: 0.51m (> 0.5m) -> accepted
    TEST_CHECK(Project(0.0f, 0.0f, 0.51f, nx, ny, w) == true);

    // 6. Extreme FOV / off-screen culling (nx > 1.5) -> rejected
    TEST_CHECK(Project(100.0f, 0.0f, 10.0f, nx, ny, w) == false);

    // 7. Non-finite coordinates -> rejected
    TEST_CHECK(Project(NAN, 0.0f, 10.0f, nx, ny, w) == false);
    TEST_CHECK(Project(0.0f, 0.0f, INFINITY, nx, ny, w) == false);

    // B. Building name cleaning
    char cleanBuf[128] = {};
    CleanBuildingName(nullptr, cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(cleanBuf[0] == '\0');

    CleanBuildingName("", cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(cleanBuf[0] == '\0');

    CleanBuildingName("media_soviet\\buildings\\oil_pump.ini", cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(strcmp(cleanBuf, "oil_pump") == 0);

    CleanBuildingName("buildings/hospital.INI", cleanBuf, sizeof(cleanBuf));
    TEST_CHECK(strcmp(cleanBuf, "hospital") == 0);

    // C. Cyrillic and non-ASCII character preservation in genuine SafeCopyPlacementName
    char copyBuf[128] = {};
    // Pure ASCII works
    SafeCopyPlacementName("media_soviet/buildings/factory.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "media_soviet/buildings/factory.ini") == 0);

    // Cyrillic Russian building name preserved
    SafeCopyPlacementName("media_soviet/buildings/больница.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "media_soviet/buildings/больница.ini") == 0);

    SafeCopyPlacementName("Шахта_Угольная", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "Шахта_Угольная") == 0);

    // German umlauts preserved
    SafeCopyPlacementName("Ölraffinerie", copyBuf, sizeof(copyBuf));
    TEST_CHECK(strcmp(copyBuf, "Ölraffinerie") == 0);

    // Control characters and DEL actively rejected
    SafeCopyPlacementName("building\x01test.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(copyBuf[0] == '\0');

    SafeCopyPlacementName("building\x1Ftest.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(copyBuf[0] == '\0');

    SafeCopyPlacementName("building\x7Ftest.ini", copyBuf, sizeof(copyBuf));
    TEST_CHECK(copyBuf[0] == '\0');

    return true;
}

bool Test_Stress_M4_Challenger_R3_R10_R11_R17()
{
    printf("    [SubTest 1: R10 Vehicle SoftSync & Smoothing Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R10_VehicleSmoothingAndPassengers()) return false;
    printf("    [SubTest 2: R11 Vehicle Purchase Sound Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R11_VehiclePurchaseSoundStress()) return false;
    printf("    [SubTest 3: R17 Oil Pump Rendering Hook Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R17_OilPumpRenderingHookStress()) return false;
    printf("    [SubTest 4: R3 Placement Tool Badge Stress] ...\n"); fflush(stdout);
    if (!SubTest_Stress_M4_R3_PlacementBadgeProjectionAndCharset()) return false;
    return true;
}

// Peer Challenger includes (preserve Suite #25 macros & MockM4* helpers above):
#define Test_Stress_M4_Challenger2 Test_Stress_M4_Challenger2_Impl
#include "m4_challenger2_stress.cpp"
#undef Test_Stress_M4_Challenger2

bool Test_Stress_M4_Challenger2()
{
    return Test_Stress_M4_Challenger2_Impl();
}

#define Test_Stress_M4_Challenger1_Iter2 Test_Stress_M4_Challenger1_Iter2_Impl
#include "m4_challenger1_stress.cpp"
#undef Test_Stress_M4_Challenger1_Iter2

bool Test_Stress_M4_Challenger1_Iter2()
{
    return Test_Stress_M4_Challenger1_Iter2_Impl();
}

// ---------------------------------------------------------------------------
// Test 63: RCO Refuel float ABI, 0x66EAB6 Patch Note, HeldFrame Refresh & SoftSync Exact Cargo
// ---------------------------------------------------------------------------

static float g_t63RefuelRet = 1.0f;
static double g_t63RefuelDebitRub = 0.0;
static float __fastcall MockVehicleRefuelOriginal(BYTE* /*vehicle*/, float /*a2*/, unsigned char /*a3*/,
                                                  uintptr_t /*a4*/, uintptr_t /*a5*/, uintptr_t /*a6*/)
{
    if (g_mockExeBuffer && g_t63RefuelDebitRub != 0.0) {
        *(double*)(g_mockExeBuffer + WORLD_RVA + WORLD_RUB_OFFSET) -= g_t63RefuelDebitRub;
    }
    return g_t63RefuelRet;
}

static bool SubTest_T63_VehicleRefuelFloatAbiAndEcon()
{
    ResetMockWorldMemory();
    TwoStates_OnSessionEnded();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> train(VEHICLE_STRUCT_BYTES, 0);
    *(int*)(train.data() + VEH_ID_OFFSET) = 999;
    *(float*)(train.data() + VEH_POS_OFFSET) = 800.0f; // State 2 (East of 250m border)

    double initRub[2] = { 500000.0, 500000.0 };
    double initUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, initRub, initUsd);
    TwoStates_OnWorldLoaded();

    TwoStates_BeforeStep(world);

    g_t63RefuelRet = 1.0f;
    g_t63RefuelDebitRub = 125.0;
    float r1 = TwoStates_InvokeVehicleRefuelHookForTest(train.data(), MockVehicleRefuelOriginal, 1.0f, 1);
    TEST_CHECK_NEAR(r1, 1.0f, 1e-6f);

    g_t63RefuelRet = 0.73f;
    g_t63RefuelDebitRub = 0.0;
    float r2 = TwoStates_InvokeVehicleRefuelHookForTest(train.data(), MockVehicleRefuelOriginal, 0.5f, 1);
    TEST_CHECK_NEAR(r2, 0.73f, 1e-6f);

    g_t63RefuelRet = 0.0f;
    float r3 = TwoStates_InvokeVehicleRefuelHookForTest(train.data(), MockVehicleRefuelOriginal, 0.0f, 1);
    TEST_CHECK_NEAR(r3, 0.0f, 1e-6f);

    TwoStates_AfterStep(world);
    TEST_CHECK_NEAR(TwoStates_GetRubles(2), 500000.0 - 125.0, 1e-4);
    TEST_CHECK_NEAR(TwoStates_GetRubles(1), 500000.0, 1e-4);

    TwoStates_OnSessionEnded();
    return true;
}

static bool SubTest_T63_HeldFrameSameTickRefreshAndLiveRestore()
{
    HeldFrame_Reset();
    std::vector<BYTE> loco(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(loco.data() + 0x04) = 401;
    *(int32_t*)(loco.data() + VEH_SIXTH_NODE_ACTIVE_OFFSET) = -1;
    loco[VEH_RENDER_VISIBLE_OFFSET] = 1;
    loco[VEH_WORKER_SIM_FLAG_OFFSET] = 1;

    const unsigned kNodeOffsets[5] = { 0xD98, 0xE54, 0xF10, 0xFCC, 0x10B8 };
    auto setAllNodesX = [&](float x) {
        for (unsigned off : kNodeOffsets) {
            float* n = (float*)(loco.data() + off);
            n[0] = n[5] = n[10] = n[15] = 1.0f;
            n[32] = x; n[33] = 10.0f; n[34] = 200.0f;
            n[38] = n[39] = n[40] = 1.0f;
            n[43] = 1.0f; n[45] = 1.0f;
        }
    };

    setAllNodesX(100.0f);
    *(float*)(loco.data() + VEH_STEER_CURRENT_OFFSET) = 0.10f;
    *(float*)(loco.data() + VEH_STEER_TARGET_OFFSET) = 0.50f;
    std::vector<BYTE*> vehList = { loco.data() };
    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = vehList.data();
    *(BYTE***)(g_mockExeBuffer + 0xA317F0) = vehList.data() + 1;

    HeldFrame_OnSimTick(10);

    // First OnSimTick(11) after Early (x=105)
    setAllNodesX(105.0f);
    HeldFrame_OnSimTick(11);
    TEST_CHECK_NEAR(*(const float*)(loco.data() + VEH_STEER_CURRENT_OFFSET), 0.15f, 1e-5f);

    // Late moves vehicle to x=108; second OnSimTick(11) must refresh currNode to 108 without advancing steer again
    setAllNodesX(108.0f);
    HeldFrame_OnSimTick(11);
    TEST_CHECK_NEAR(*(const float*)(loco.data() + VEH_STEER_CURRENT_OFFSET), 0.15f, 1e-5f);

    struct GuardCtx {
        BYTE* veh;
        float xDuring;
    } gctx = { loco.data(), 0.0f };
    auto guardCb = [](void* ud) {
        auto* c = (GuardCtx*)ud;
        c->xDuring = *(const float*)(c->veh + 0xD98 + 0x80);
    };
    TEST_CHECK(HeldFrame_RunVehicleGuardForTest(g_mockExeBuffer + 0xA317E8, 0.5f, guardCb, &gctx));
    TEST_CHECK_NEAR(gctx.xDuring, 104.0f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(loco.data() + 0xD98 + 0x80), 108.0f, 1e-4f);

    struct HlCtx {
        BYTE* veh;
        float xDuring;
        int calls;
    } hlCtx = { loco.data(), 0.0f, 0 };
    auto hlCb = [](BYTE* /*v*/, bool /*extra*/, void* ud) {
        auto* c = (HlCtx*)ud;
        c->xDuring = *(const float*)(c->veh + 0xD98 + 0x80);
        c->calls++;
    };
    const float hlColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    HeldFrame_SetHighlightCaptureForTest(hlCb, &hlCtx);
    HeldFrame_SubmitVehicleHighlight(loco.data(), 0, 1, hlColor, 0, false);
    TEST_CHECK(hlCtx.calls == 1);
    TEST_CHECK_NEAR(hlCtx.xDuring, 104.0f, 1e-4f);
    TEST_CHECK_NEAR(*(const float*)(loco.data() + 0xD98 + 0x80), 108.0f, 1e-4f);

    // Live nudge to 109.5 without another OnSimTick must restore exact live 109.5, not stale rec.currNode
    setAllNodesX(109.5f);
    HeldFrame_SubmitVehicleHighlight(loco.data(), 0, 1, hlColor, 0, false);
    HeldFrame_SetHighlightCaptureForTest(nullptr, nullptr);
    TEST_CHECK(hlCtx.calls == 2);
    TEST_CHECK_NEAR(*(const float*)(loco.data() + 0xD98 + 0x80), 109.5f, 1e-4f);

    *(BYTE***)(g_mockExeBuffer + 0xA317E8) = nullptr;
    *(BYTE***)(g_mockExeBuffer + 0xA317F0) = nullptr;
    HeldFrame_Reset();
    return true;
}

static bool SubTest_T63_SoftSyncExactCargoAndDesyncDump()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> resTable(WORLD_RESOURCE_STRIDE * 2, 0);
    const char* resGravel = (const char*)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    const char* resSteel  = (const char*)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);
    strcpy_s((char*)resGravel, 32, "gravel");
    strcpy_s((char*)resSteel, 32, "steel");
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET)     = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 8) = resTable.data() + resTable.size();

    std::vector<BYTE> conBld(BUILDING_STRUCT_BYTES, 0);
    *(int*)conBld.data() = 401;
    *(float*)(conBld.data() + BLD_PROGRESS_OFFSET) = 0.45f;

    std::vector<BYTE> bld(BUILDING_STRUCT_BYTES, 0);
    *(int*)bld.data() = 12;
    *(float*)(bld.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(float*)(bld.data() + BLD_PRODUCED_OFFSET) = 3.5f;

    std::vector<BYTE> loco(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(loco.data() + VEH_ID_OFFSET) = 88;
    *(BYTE**)(loco.data() + 0x528) = conBld.data();

    struct MockCargo {
        const char* res;
        float amt;
        float pad;
    };
    std::vector<MockCargo> cargo = {
        { resSteel, 12.40f, 0.0f },
        { resGravel, 8.10f, 0.0f }
    };
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET)      = (BYTE*)cargo.data();
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET + 8)  = (BYTE*)(cargo.data() + cargo.size());
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET + 16) = (BYTE*)(cargo.data() + cargo.size());

    BYTE* vehList[2] = { loco.data(), nullptr };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET)     = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 2;
    BYTE* bldList[2] = { bld.data(), nullptr };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = bldList;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = bldList + 2;
    BYTE* conList[1] = { conBld.data() };
    *(BYTE***)(world + WORLD_CONSTRUCTIONS_OFFSET)     = conList;
    *(BYTE***)(world + WORLD_CONSTRUCTIONS_OFFSET + 8) = conList + 1;

    std::vector<BYTE> wire(8192, 0);
    size_t wireSz = 0;
    TEST_CHECK(VehicleSoftSync_BuildExactPacketForTest(wire.data(), wire.size(), &wireSz));

    // Reordered local slots with < 2.0f small drift on stopped vehicle
    cargo[0] = { resGravel, 8.15f, 0.0f };
    cargo[1] = { resSteel, 12.25f, 0.0f };
    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireSz));
    TEST_CHECK_NEAR(cargo[0].amt, 8.10f, 1e-4f);
    TEST_CHECK_NEAR(cargo[1].amt, 12.40f, 1e-4f);

    // Host cargoCount == 0 must zero leftover local cargo entries
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET)      = nullptr;
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET + 8)  = nullptr;
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET + 16) = nullptr;
    TEST_CHECK(VehicleSoftSync_BuildExactPacketForTest(wire.data(), wire.size(), &wireSz));

    cargo[0] = { resSteel, 0.35f, 0.0f };
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET)      = (BYTE*)cargo.data();
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET + 8)  = (BYTE*)(cargo.data() + 1);
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET + 16) = (BYTE*)(cargo.data() + cargo.size());
    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireSz));
    TEST_CHECK_NEAR(cargo[0].amt, 0.0f, 1e-4f);

    // StateHash_LogDesyncBreakdown safely handles valid, zero, and bad resource pointers in cargo=[...]
    cargo[0] = { resSteel, 2.5f, 0.0f };
    cargo[1] = { reinterpret_cast<const char*>(0x123ULL), 0.0f, 0.0f };
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET)      = (BYTE*)cargo.data();
    *(BYTE**)(loco.data() + VEH_CARGO_OFFSET + 8)  = (BYTE*)(cargo.data() + 2);
    StateHash_LogDesyncBreakdown("veh-logical bld-storage constructions ", 8880);
    StateHash_LogDesyncBreakdown(nullptr, 8881);
    return true;
}

static bool SubTest_T63_OrderIndependentHashAndFastBldSoftSync()
{
    ResetMockWorldMemory();
    VehicleSoftSync_Reset();
    BYTE* world = g_mockExeBuffer + WORLD_RVA;

    std::vector<BYTE> resTable(WORLD_RESOURCE_STRIDE * 4, 0);
    const char* resGravel = (const char*)(resTable.data() + 0 * WORLD_RESOURCE_STRIDE);
    const char* resSteel  = (const char*)(resTable.data() + 1 * WORLD_RESOURCE_STRIDE);
    const char* resBoards = (const char*)(resTable.data() + 2 * WORLD_RESOURCE_STRIDE);
    const char* resBricks = (const char*)(resTable.data() + 3 * WORLD_RESOURCE_STRIDE);
    strcpy_s((char*)resGravel, 32, "gravel");
    strcpy_s((char*)resSteel,  32, "steel");
    strcpy_s((char*)resBoards, 32, "boards");
    strcpy_s((char*)resBricks, 32, "bricks");
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET)     = resTable.data();
    *(BYTE**)(world + WORLD_RESOURCES_OFFSET + 8) = resTable.data() + resTable.size();

    // 1. Order-independent, zero-slot-invariant, ULP-normalized MixEntries on vehicles & buildings
    struct CargoSlot { const char* res; float amt; float pad; };
    std::vector<CargoSlot> vCargoHost = {
        { resGravel, 2.000f, 0.0f },
        { resSteel,  1.525f, 0.0f },
        { resBoards, 1.500f, 0.0f },
    };
    std::vector<CargoSlot> vCargoClient = {
        { resBoards, 1.500f, 0.0f },
        { resBricks, -0.0f,  0.0f },
        { resGravel, 2.000f + 1e-6f, 0.0f },
        { resSteel,  1.525f, 0.0f },
        { resBricks, +0.0f,  0.0f },
    };
    std::vector<BYTE> b0(BUILDING_STRUCT_BYTES, 0);
    std::vector<BYTE> b0Slot(BLD_STORAGE_SLOT_SIZE, 0);
    std::vector<CargoSlot> b0Host = {
        { resGravel, 10.0f, 0.0f },
        { resSteel,   5.0f, 0.0f },
    };
    std::vector<CargoSlot> b0Client = {
        { resSteel,   5.0f, 0.0f },
        { resBricks, -0.0f, 0.0f },
        { resGravel, 10.0f, 0.0f },
    };
    *(int*)b0.data() = 1;
    *(float*)(b0.data() + BLD_PROGRESS_OFFSET) = 1.0f;
    *(BYTE**)(b0Slot.data() + 0x00) = (BYTE*)b0Host.data();
    *(BYTE**)(b0Slot.data() + 0x08) = (BYTE*)(b0Host.data() + b0Host.size());
    *(BYTE**)(b0.data() + BLD_STORAGE_OFFSET)     = b0Slot.data();
    *(BYTE**)(b0.data() + BLD_STORAGE_OFFSET + 8) = b0Slot.data() + BLD_STORAGE_SLOT_SIZE;
    BYTE* b0List[1] = { b0.data() };
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)     = b0List;
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8) = b0List + 1;

    std::vector<BYTE> veh(VEHICLE_STRUCT_BYTES, 0);
    *(unsigned*)(veh.data() + VEH_ID_OFFSET) = 123;
    *(BYTE**)(veh.data() + VEH_CARGO_OFFSET)     = (BYTE*)vCargoHost.data();
    *(BYTE**)(veh.data() + VEH_CARGO_OFFSET + 8) = (BYTE*)(vCargoHost.data() + vCargoHost.size());
    BYTE* vehList[1] = { veh.data() };
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET)     = vehList;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = vehList + 1;

    StateSummary sHost = {}, sClient = {};
    TEST_CHECK(StateHash_Compute(&sHost));

    *(BYTE**)(veh.data() + VEH_CARGO_OFFSET)     = (BYTE*)vCargoClient.data();
    *(BYTE**)(veh.data() + VEH_CARGO_OFFSET + 8) = (BYTE*)(vCargoClient.data() + vCargoClient.size());
    *(BYTE**)(b0Slot.data() + 0x00) = (BYTE*)b0Client.data();
    *(BYTE**)(b0Slot.data() + 0x08) = (BYTE*)(b0Client.data() + b0Client.size());
    TEST_CHECK(StateHash_Compute(&sClient));
    TEST_CHECK(sHost.hashVehicles == sClient.hashVehicles);
    TEST_CHECK(sHost.hashBuildings == sClient.hashBuildings);

    // 0.001t difference still detected
    vCargoClient[2].amt = 2.001f;
    TEST_CHECK(StateHash_Compute(&sClient));
    TEST_CHECK(sHost.hashVehicles != sClient.hashVehicles);

    // Swapped resource names with same quantities detected
    std::vector<CargoSlot> vCargoSwap = {
        { resBoards, 2.000f, 0.0f },
        { resSteel,  1.525f, 0.0f },
        { resGravel, 1.500f, 0.0f },
    };
    *(BYTE**)(veh.data() + VEH_CARGO_OFFSET)     = (BYTE*)vCargoSwap.data();
    *(BYTE**)(veh.data() + VEH_CARGO_OFFSET + 8) = (BYTE*)(vCargoSwap.data() + vCargoSwap.size());
    TEST_CHECK(StateHash_Compute(&sClient));
    TEST_CHECK(sHost.hashVehicles != sClient.hashVehicles);

    // 2. Fast bld-storage sweep: 15 inert finished houses + 12 active storage blds + 2 constructions
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET)     = nullptr;
    *(BYTE***)(world + WORLD_VEHICLES_OFFSET + 8) = nullptr;

    std::vector<std::vector<BYTE>> inertBlds(15, std::vector<BYTE>(BUILDING_STRUCT_BYTES, 0));
    std::vector<std::vector<BYTE>> activeBlds(12, std::vector<BYTE>(BUILDING_STRUCT_BYTES, 0));
    std::vector<std::vector<BYTE>> activeSlots(12, std::vector<BYTE>(BLD_STORAGE_SLOT_SIZE, 0));
    std::vector<std::vector<CargoSlot>> activeEntries(12);
    std::vector<BYTE*> allBlds;

    for (size_t i = 0; i < inertBlds.size(); ++i) {
        *(int*)inertBlds[i].data() = (int)(10 + i);
        *(float*)(inertBlds[i].data() + BLD_PROGRESS_OFFSET) = 1.0f;
        *(float*)(inertBlds[i].data() + BLD_PRODUCED_OFFSET) = 0.0f;
        allBlds.push_back(inertBlds[i].data());
    }
    for (size_t i = 0; i < activeBlds.size(); ++i) {
        *(int*)activeBlds[i].data() = (int)(100 + i);
        *(float*)(activeBlds[i].data() + BLD_PROGRESS_OFFSET) = 1.0f;
        activeEntries[i] = { { resGravel, 4.5f + (float)i * 0.25f, 0.0f }, { nullptr, 0.0f, 0.0f } };
        activeEntries[i].reserve(4);
        *(BYTE**)(activeSlots[i].data() + 0x00) = (BYTE*)activeEntries[i].data();
        *(BYTE**)(activeSlots[i].data() + 0x08) = (BYTE*)(activeEntries[i].data() + 1);
        *(BYTE**)(activeSlots[i].data() + 0x10) = (BYTE*)(activeEntries[i].data() + 4);
        *(BYTE**)(activeBlds[i].data() + BLD_STORAGE_OFFSET)     = activeSlots[i].data();
        *(BYTE**)(activeBlds[i].data() + BLD_STORAGE_OFFSET + 8) = activeSlots[i].data() + BLD_STORAGE_SLOT_SIZE;
        allBlds.push_back(activeBlds[i].data());
    }

    std::vector<BYTE> con0(BUILDING_STRUCT_BYTES, 0), con1(BUILDING_STRUCT_BYTES, 0);
    *(int*)con0.data() = 200;
    *(float*)(con0.data() + BLD_PROGRESS_OFFSET) = 0.25f;
    *(int*)con1.data() = 201;
    *(float*)(con1.data() + BLD_PROGRESS_OFFSET) = 0.50f;
    BYTE* conList[2] = { con0.data(), con1.data() };

    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET)         = allBlds.data();
    *(BYTE***)(world + WORLD_BUILDINGS_OFFSET + 8)     = allBlds.data() + allBlds.size();
    *(BYTE***)(world + WORLD_CONSTRUCTIONS_OFFSET)     = conList;
    *(BYTE***)(world + WORLD_CONSTRUCTIONS_OFFSET + 8) = conList + 2;

    TEST_CHECK(StateHash_Compute(&sHost));

    std::vector<BYTE> wire(32768, 0);
    size_t wireSz = 0;
    TEST_CHECK(VehicleSoftSync_BuildPacketForTest(wire.data(), wire.size(), &wireSz, true));
    TEST_CHECK(wireSz > sizeof(unsigned) * 3);
    const unsigned vehCount = *(const unsigned*)wire.data();
    TEST_CHECK(vehCount == 0);
    // Offset after VehicleSoftSyncTreasury (56 bytes when unlimitedMoney == 0)
    const size_t bldHdrOff = sizeof(unsigned) + 56u;
    TEST_CHECK(wireSz > bldHdrOff + 8);
    TEST_CHECK(*(const unsigned*)(wire.data() + bldHdrOff) == 0x42535447u);
    const unsigned bldCount = *(const unsigned*)(wire.data() + bldHdrOff + 4);
    TEST_CHECK(bldCount == 14u); // all 12 active storage blds + 2 constructions in 1 immediate packet!

    // Corrupt Client buildings & constructions: add phantom steel=9.0f to bld 100 and drift all 12 + constructions
    activeEntries[0] = { { resSteel, 9.0f, 0.0f }, { resGravel, 1.0f, 0.0f } };
    *(BYTE**)(activeSlots[0].data() + 0x00) = (BYTE*)activeEntries[0].data();
    *(BYTE**)(activeSlots[0].data() + 0x08) = (BYTE*)(activeEntries[0].data() + 2);
    *(BYTE**)(activeSlots[0].data() + 0x10) = (BYTE*)(activeEntries[0].data() + 2);
    for (size_t i = 1; i < activeBlds.size(); ++i) activeEntries[i][0].amt = 0.0f;
    *(float*)(con0.data() + BLD_PROGRESS_OFFSET) = 0.10f;
    *(float*)(con1.data() + BLD_PROGRESS_OFFSET) = 0.20f;

    TEST_CHECK(VehicleSoftSync_ApplyCommand(wire.data(), wireSz));
    TEST_CHECK(StateHash_Compute(&sClient));
    TEST_CHECK(sClient.hashBuildings == sHost.hashBuildings);
    TEST_CHECK(sClient.hashConstructions == sHost.hashConstructions);
    return true;
}

bool Test_RcoRefuelHeldFrameAndSoftSyncFixes()
{
    printf("    [T63-1: H_VehicleRefuel float ABI preservation & TwoStates econ] ...\n"); fflush(stdout);
    if (!SubTest_T63_VehicleRefuelFloatAbiAndEcon()) return false;

    printf("    [T63-2: HeldFrame same-tick Late refresh & live visual restore] ...\n"); fflush(stdout);
    if (!SubTest_T63_HeldFrameSameTickRefreshAndLiveRestore()) return false;

    printf("    [T63-3: VehicleSoftSync exact cargo healing & StateHash_LogDesyncBreakdown] ...\n"); fflush(stdout);
    if (!SubTest_T63_SoftSyncExactCargoAndDesyncDump()) return false;

    printf("    [T63-4: Order-independent MixEntries hash & fast 16-bld soft-sync sweep] ...\n"); fflush(stdout);
    if (!SubTest_T63_OrderIndependentHashAndFastBldSoftSync()) return false;

    ResetMockWorldMemory();
    return true;
}
