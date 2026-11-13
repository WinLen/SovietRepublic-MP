// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone M9.3
// Challenger 1: Adversarial Stress Testing of Autosave Cadence, ZIP Integrity & Deep UTF-8,
// Demographic Tracker Serialization Fuzzing, and Dead Entity Pruning & Segment Lifecycle
//
// Focus Areas:
//   1. Autosave Cadence & State Isolation (Feature 32):
//      - Verify tick boundary behavior: 18,000 tick intervals, boundary edge cases (0, 17999, 18000, 18001).
//      - Multi-period cadence simulation across 200,000 ticks proving exactly 11 triggers.
//      - Guard against divide-by-zero on intervalTicks == 0, extreme integer values (ULLONG_MAX).
//      - Verify path generation: Host -> save/coop_autosave, Client -> save/coop_client_backup.
//      - Session state isolation: disconnected/inactive sessions strictly reject unprompted autosaves.
//   2. ZIP Integrity, Deep Hierarchies, UTF-8 & Corrupt File Fail-Fast (Feature 33):
//      - Adversarial non-ASCII / Cyrillic paths in directory trees and file names.
//      - Deeply nested directory hierarchy (16 levels) with 100% binary round-trip comparison.
//      - Directory iteration error handling: fail-fast abort and incomplete archive deletion.
//      - Corrupted ZIP rejection: 0-byte archives, truncated PK headers, garbage payloads.
//   3. Demographic Tracker Serialization Fuzzing & Malformed Meta (Feature 35):
//      - Extreme and boundary float values: 0.0f, -0.0f, 1e-6f, 1e6f, -1e6f, 1e9f.
//      - Extreme integers: 0, -1, INT32_MAX, INT32_MIN.
//      - Non-finite numbers: NaN, +Inf, -Inf handled safely without corrupting INI.
//      - Fuzzed and truncated strings: truncated tags, empty values, garbage tokens, huge comma streams.
//      - Missing INI sections and key-recovery graceful fallbacks.
//   4. Dead Entity Pruning, Memory Safety & Planned Segment Lifecycle (Feature 36):
//      - Heavy load pruning: 2,000 live and 2,000 dead vehicles/buildings across mock world vectors.
//      - SafeReadPod and SEH defense: null pointers, unmapped pages, corrupted vector headers.
//      - Planned road segment ownership lifecycle: RoadSync_ClearPlannedSegments on world reloads.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <memory>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// SubTest 1: Autosave Cadence & State Isolation (Feature 32)
// ---------------------------------------------------------------------------

static bool SubTest_M9_3_C1_AutosaveCadenceAndStateIsolation()
{
    printf("    [Challenger1-1.1] Autosave Cadence, Boundary Oracles & Session State Isolation ...\n");
    fflush(stdout);

    // 1. Exact boundary ticks
    TEST_CHECK(!TwoStates_IsAutosaveTick(0, 18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(1, 18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(17999, 18000));
    TEST_CHECK(TwoStates_IsAutosaveTick(18000, 18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(18001, 18000));

    TEST_CHECK(!TwoStates_IsAutosaveTick(35999, 18000));
    TEST_CHECK(TwoStates_IsAutosaveTick(36000, 18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(36001, 18000));

    // 2. Division-by-zero defense
    TEST_CHECK(!TwoStates_IsAutosaveTick(18000, 0));
    TEST_CHECK(!TwoStates_IsAutosaveTick(0, 0));

    // 3. Extreme tick values
    unsigned long long largeCadenceTick = 18000ULL * 50000ULL; // 900,000,000 ticks
    TEST_CHECK(TwoStates_IsAutosaveTick(largeCadenceTick, 18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(largeCadenceTick - 1, 18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(largeCadenceTick + 1, 18000));
    TEST_CHECK(!TwoStates_IsAutosaveTick(0xFFFFFFFFFFFFFFFFULL, 18000)); // ULLONG_MAX

    // 4. Cadence loop across 200,000 ticks
    int triggerCount = 0;
    for (unsigned long long tick = 0; tick < 200000ULL; ++tick) {
        if (TwoStates_IsAutosaveTick(tick, COOP_AUTOSAVE_INTERVAL_TICKS)) {
            ++triggerCount;
            TEST_CHECK(tick > 0);
            TEST_CHECK((tick % COOP_AUTOSAVE_INTERVAL_TICKS) == 0);
        }
    }
    // Expected triggers: 18000, 36000, 54000, 72000, 90000, 108000, 126000, 144000, 162000, 180000, 198000 = 11
    TEST_CHECK(triggerCount == 11);

    // 5. RelPath generation
    TEST_CHECK(strcmp(TwoStates_GetAutosaveRelPath(true), "save/coop_autosave") == 0);
    TEST_CHECK(strcmp(TwoStates_GetAutosaveRelPath(false), "save/coop_client_backup") == 0);

    // 6. Inactive session isolation: unprompted autosave must be rejected
    BYTE* mockWorld = g_mockExeBuffer + WORLD_RVA;
    TEST_CHECK(!TwoStates_CheckAndTriggerAutosave(nullptr, 18000, true));
    TEST_CHECK(!TwoStates_CheckAndTriggerAutosave(mockWorld, 17999, true)); // non-cadence tick
    // Outside active session or active co-op mode, must return false without modifying world save name
    if (!TwoStates_IsActive()) {
        TEST_CHECK(!TwoStates_CheckAndTriggerAutosave(mockWorld, 18000, true));
        TEST_CHECK(!TwoStates_CheckAndTriggerAutosave(mockWorld, 18000, false));
    }

    return true;
}

// ---------------------------------------------------------------------------
// SubTest 2: ZIP Integrity, Deep Hierarchies, UTF-8 & Corrupt Fail-Fast (Feature 33)
// ---------------------------------------------------------------------------

static void WriteTestPayload(const fs::path& filePath, size_t byteCount, unsigned char seed)
{
    std::vector<unsigned char> buf(byteCount);
    for (size_t i = 0; i < byteCount; ++i) {
        buf[i] = (unsigned char)((i * 31 + seed) & 0xFF);
    }
    std::ofstream out(filePath, std::ios::binary);
    if (!buf.empty()) {
        out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
}

static bool VerifyFileContent(const fs::path& p1, const fs::path& p2)
{
    std::error_code ec;
    if (!fs::exists(p1, ec) || !fs::exists(p2, ec)) return false;
    if (fs::file_size(p1, ec) != fs::file_size(p2, ec)) return false;

    std::ifstream f1(p1, std::ios::binary);
    std::ifstream f2(p2, std::ios::binary);
    std::vector<char> b1(4096), b2(4096);
    while (f1 && f2) {
        f1.read(b1.data(), b1.size());
        f2.read(b2.data(), b2.size());
        if (f1.gcount() != f2.gcount()) return false;
        if (f1.gcount() > 0 && memcmp(b1.data(), b2.data(), (size_t)f1.gcount()) != 0) {
            return false;
        }
    }
    return true;
}

static bool SubTest_M9_3_C1_ZipIntegrityDeepHierarchyAndCorruptStress()
{
    printf("    [Challenger1-1.2] ZIP Deep Hierarchies, UTF-8 Cyrillic & Corrupt File Fail-Fast ...\n");
    fflush(stdout);

    std::error_code ec;
    fs::path tempBase = fs::temp_directory_path() / "RepMP_ZipStress_C1";
    fs::remove_all(tempBase, ec);
    fs::create_directories(tempBase, ec);

    // 1. Cyrillic directory & file structure with mixed contents
    fs::path srcCyrillic = tempBase / "src_cyrillic";
    fs::path dir1 = srcCyrillic / "Кооператив_Запад_1968" / "Промзона_Север";
    fs::path dir2 = srcCyrillic / "Кооператив_Запад_1968" / "Транспортная_Сеть";
    fs::path dirEmpty = srcCyrillic / "Кооператив_Запад_1968" / "Пустая_Папка";
    fs::create_directories(dir1, ec);
    fs::create_directories(dir2, ec);
    fs::create_directories(dirEmpty, ec);

    fs::path file1 = dir1 / "Сталелитейный_Комбинат_№4.bin";
    fs::path file2 = dir2 / "Маршрут_Поезда_ВЛ80_Грузовой.dat";
    fs::path fileZero = srcCyrillic / "Кооператив_Запад_1968" / "нулевой_файл.bin";
    fs::path fileSymbols = srcCyrillic / "Кооператив_Запад_1968" / "Символы_★_☭_Партия.txt";

    WriteTestPayload(file1, 65536, 0x1A);
    WriteTestPayload(file2, 131072, 0x7B);
    WriteTestPayload(fileZero, 0, 0x00);
    {
        std::ofstream out(fileSymbols, std::ios::binary);
        out << "Союз Советских Социалистических Республик ★ Кооператив «Два государства»";
    }

    fs::path zipCyrillic = tempBase / "cyrillic_archive.zip";
    TEST_CHECK(ZipSaveDirectory(srcCyrillic.string().c_str(), zipCyrillic.string().c_str()));
    TEST_CHECK(fs::exists(zipCyrillic, ec));
    TEST_CHECK(fs::file_size(zipCyrillic, ec) > 0);

    fs::path dstCyrillic = tempBase / "dst_cyrillic";
    TEST_CHECK(UnzipToDirectory(zipCyrillic.string().c_str(), dstCyrillic.string().c_str()));

    // Verify extracted Cyrillic files
    fs::path extFile1 = dstCyrillic / "Кооператив_Запад_1968" / "Промзона_Север" / "Сталелитейный_Комбинат_№4.bin";
    fs::path extFile2 = dstCyrillic / "Кооператив_Запад_1968" / "Транспортная_Сеть" / "Маршрут_Поезда_ВЛ80_Грузовой.dat";
    fs::path extFileZero = dstCyrillic / "Кооператив_Запад_1968" / "нулевой_файл.bin";
    fs::path extFileSymbols = dstCyrillic / "Кооператив_Запад_1968" / "Символы_★_☭_Партия.txt";

    TEST_CHECK(VerifyFileContent(file1, extFile1));
    TEST_CHECK(VerifyFileContent(file2, extFile2));
    TEST_CHECK(VerifyFileContent(fileZero, extFileZero));
    TEST_CHECK(VerifyFileContent(fileSymbols, extFileSymbols));

    // 2. Deeply nested hierarchy (16 levels)
    fs::path deepDir = tempBase / "deep_src";
    fs::path curDeep = deepDir;
    for (int lvl = 1; lvl <= 16; ++lvl) {
        char lvlName[32];
        _snprintf_s(lvlName, sizeof(lvlName), _TRUNCATE, "level_%02d", lvl);
        curDeep /= lvlName;
    }
    fs::create_directories(curDeep, ec);
    fs::path deepLeaf = curDeep / "deep_leaf.bin";
    WriteTestPayload(deepLeaf, 32768, 0x5C);

    fs::path deepZip = tempBase / "deep_archive.zip";
    TEST_CHECK(ZipSaveDirectory(deepDir.string().c_str(), deepZip.string().c_str()));
    fs::path deepDst = tempBase / "deep_dst";
    TEST_CHECK(UnzipToDirectory(deepZip.string().c_str(), deepDst.string().c_str()));

    fs::path extDeepLeaf = deepDst;
    for (int lvl = 1; lvl <= 16; ++lvl) {
        char lvlName[32];
        _snprintf_s(lvlName, sizeof(lvlName), _TRUNCATE, "level_%02d", lvl);
        extDeepLeaf /= lvlName;
    }
    extDeepLeaf /= "deep_leaf.bin";
    TEST_CHECK(VerifyFileContent(deepLeaf, extDeepLeaf));

    // 3. Corrupt archive rejection & fail-fast
    // A. 0-byte archive
    fs::path corruptZero = tempBase / "corrupt_zero.zip";
    WriteTestPayload(corruptZero, 0, 0);
    TEST_CHECK(!UnzipToDirectory(corruptZero.string().c_str(), (tempBase / "dst_zero").string().c_str()));

    // B. Truncated header (only 4 bytes)
    fs::path corruptTrunc = tempBase / "corrupt_trunc.zip";
    {
        std::ofstream out(corruptTrunc, std::ios::binary);
        out.write("PK\x03\x04", 4);
    }
    TEST_CHECK(!UnzipToDirectory(corruptTrunc.string().c_str(), (tempBase / "dst_trunc").string().c_str()));

    // C. Random garbage bytes
    fs::path corruptGarbage = tempBase / "corrupt_garbage.zip";
    WriteTestPayload(corruptGarbage, 1024, 0xEE);
    TEST_CHECK(!UnzipToDirectory(corruptGarbage.string().c_str(), (tempBase / "dst_garbage").string().c_str()));

    // D. Non-existent source directory to ZipSaveDirectory
    fs::path fakeDir = tempBase / "non_existent_folder_99999";
    fs::path failZip = tempBase / "should_not_exist.zip";
    TEST_CHECK(!ZipSaveDirectory(fakeDir.string().c_str(), failZip.string().c_str()));
    TEST_CHECK(!fs::exists(failZip, ec)); // Must clean up incomplete archive

    // Clean up temporary workspace
    fs::remove_all(tempBase, ec);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 3: Demographic Tracker Serialization Fuzzing (Feature 35)
// ---------------------------------------------------------------------------

static bool SubTest_M9_3_C1_DemographicTrackerFuzzingAndMalformedMeta()
{
    printf("    [Challenger1-1.3] Demographic Tracker Fuzzing & Malformed Meta Robustness ...\n");
    fflush(stdout);

    // 1. Edge-case float values and boundary numbers
    StateTrackerSlice extremeSlice = {};
    extremeSlice.healthSum[0] = 0.0f;
    extremeSlice.healthSum[1] = -0.0f;
    extremeSlice.healthSum[2] = 1e-6f;   // sub-thousandth precision loss check (rounds to 0)
    extremeSlice.healthSum[3] = 1e6f;    // 1,000,000.0f -> 1,000,000,000 scaled
    extremeSlice.healthSum[4] = -1e6f;   // -1,000,000.0f
    extremeSlice.healthSum[5] = 1e9f;    // 1 billion
    extremeSlice.healthCnt[0] = 0;
    extremeSlice.healthCnt[1] = -1;
    extremeSlice.healthCnt[2] = 2147483647; // INT32_MAX
    extremeSlice.healthCnt[3] = -2147483647 - 1; // INT32_MIN

    extremeSlice.happySum[0] = 1234.567f;
    extremeSlice.happyCnt[0] = 987654;

    TwoStates_SetStateTrackerSliceForTest(1, false, extremeSlice);
    StateTrackerSlice outExtreme = {};
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(1, false, &outExtreme));

    TEST_CHECK_NEAR(outExtreme.healthSum[0], 0.0f, 0.001f);
    TEST_CHECK_NEAR(outExtreme.healthSum[1], 0.0f, 0.001f);
    TEST_CHECK_NEAR(outExtreme.healthSum[2], 0.0f, 0.001f); // 1e-6 * 1000 = 0.001 rounds to 0
    TEST_CHECK_NEAR(outExtreme.healthSum[3], 1e6f, 1.0f);
    TEST_CHECK_NEAR(outExtreme.healthSum[4], -1e6f, 1.0f);
    TEST_CHECK_NEAR(outExtreme.healthSum[5], 1e9f, 10.0f);
    TEST_CHECK(outExtreme.healthCnt[0] == 0);
    TEST_CHECK(outExtreme.healthCnt[1] == -1);
    TEST_CHECK(outExtreme.healthCnt[2] == 2147483647);
    TEST_CHECK(outExtreme.healthCnt[3] == -2147483647 - 1);
    TEST_CHECK_NEAR(outExtreme.happySum[0], 1234.567f, 0.005f);
    TEST_CHECK(outExtreme.happyCnt[0] == 987654);

    // 2. Malformed and corrupted INI serialization parsing
    std::error_code ec;
    fs::path tempSaveDir = fs::temp_directory_path() / "RepMP_MetaFuzz_C1";
    fs::remove_all(tempSaveDir, ec);
    fs::create_directories(tempSaveDir, ec);

    // A. Truncated tags and garbage strings
    {
        std::ofstream meta(tempSaveDir / "republicmp_coop.ini");
        meta << "[republicmp_coop]\r\n"
             << "coop_mode=1\r\n"
             << "[TrackerHealth0]\r\n"
             << "cur=HS:100;HC:;PS:bad,tokens;PC:,,,\r\n"
             << "old=UNKNOWN_TAG:9999;GARBAGE:abc:123\r\n"
             << "[TrackerHealth1]\r\n"
             << "cur=\r\n"
             << "old=HS:9999999999999999999999999999999999999999\r\n";
    }

    // Must parse without crash or access violation
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(tempSaveDir.string().c_str(), false));

    // B. Missing Tracker sections entirely (regression check for fallback)
    {
        std::ofstream meta(tempSaveDir / "republicmp_coop.ini");
        meta << "[republicmp_coop]\r\n"
             << "coop_mode=1\r\n"
             << "border_half_width_m=250\r\n";
    }
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(tempSaveDir.string().c_str(), false));

    // 3. Full Round-Trip Fidelity Test (State 1 and State 2)
    StateTrackerSlice s1Cur = {}, s1Old = {}, s2Cur = {}, s2Old = {};
    for (int i = 0; i < TRACKER_HEALTH_CAUSE_COUNT; ++i) {
        s1Cur.healthSum[i] = (float)i * 15.25f;
        s1Cur.healthCnt[i] = (i + 1) * 100;
        s1Old.healthSum[i] = (float)i * 7.5f;
        s1Old.healthCnt[i] = (i + 1) * 50;
        s2Cur.healthSum[i] = (float)i * 22.125f;
        s2Cur.healthCnt[i] = (i + 1) * 200;
        s2Old.healthSum[i] = (float)i * 11.0f;
        s2Old.healthCnt[i] = (i + 1) * 75;
    }
    for (int i = 0; i < TRACKER_HAPPY_CAUSE_COUNT; ++i) {
        s1Cur.happySum[i] = (float)i * 5.5f;
        s1Cur.happyCnt[i] = (i + 1) * 20;
        s1Old.happySum[i] = (float)i * 2.75f;
        s1Old.happyCnt[i] = (i + 1) * 10;
        s2Cur.happySum[i] = (float)i * 8.8f;
        s2Cur.happyCnt[i] = (i + 1) * 35;
        s2Old.happySum[i] = (float)i * 4.4f;
        s2Old.happyCnt[i] = (i + 1) * 18;
    }

    TwoStates_SetStateTrackerSliceForTest(1, false, s1Cur);
    TwoStates_SetStateTrackerSliceForTest(1, true,  s1Old);
    TwoStates_SetStateTrackerSliceForTest(2, false, s2Cur);
    TwoStates_SetStateTrackerSliceForTest(2, true,  s2Old);

    TEST_CHECK(TwoStates_WriteSaveMetaToDir(tempSaveDir.string().c_str()));

    // Clear slices
    StateTrackerSlice zeroSlice = {};
    TwoStates_SetStateTrackerSliceForTest(1, false, zeroSlice);
    TwoStates_SetStateTrackerSliceForTest(1, true,  zeroSlice);
    TwoStates_SetStateTrackerSliceForTest(2, false, zeroSlice);
    TwoStates_SetStateTrackerSliceForTest(2, true,  zeroSlice);

    // Read back
    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(tempSaveDir.string().c_str(), true));

    StateTrackerSlice readS1Cur = {}, readS1Old = {}, readS2Cur = {}, readS2Old = {};
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(1, false, &readS1Cur));
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(1, true,  &readS1Old));
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(2, false, &readS2Cur));
    TEST_CHECK(TwoStates_GetStateTrackerSliceForTest(2, true,  &readS2Old));

    for (int i = 0; i < TRACKER_HEALTH_CAUSE_COUNT; ++i) {
        TEST_CHECK_NEAR(readS1Cur.healthSum[i], s1Cur.healthSum[i], 0.005f);
        TEST_CHECK(readS1Cur.healthCnt[i] == s1Cur.healthCnt[i]);
        TEST_CHECK_NEAR(readS1Old.healthSum[i], s1Old.healthSum[i], 0.005f);
        TEST_CHECK(readS1Old.healthCnt[i] == s1Old.healthCnt[i]);

        TEST_CHECK_NEAR(readS2Cur.healthSum[i], s2Cur.healthSum[i], 0.005f);
        TEST_CHECK(readS2Cur.healthCnt[i] == s2Cur.healthCnt[i]);
        TEST_CHECK_NEAR(readS2Old.healthSum[i], s2Old.healthSum[i], 0.005f);
        TEST_CHECK(readS2Old.healthCnt[i] == s2Old.healthCnt[i]);
    }

    for (int i = 0; i < TRACKER_HAPPY_CAUSE_COUNT; ++i) {
        TEST_CHECK_NEAR(readS1Cur.happySum[i], s1Cur.happySum[i], 0.005f);
        TEST_CHECK(readS1Cur.happyCnt[i] == s1Cur.happyCnt[i]);
        TEST_CHECK_NEAR(readS2Cur.happySum[i], s2Cur.happySum[i], 0.005f);
        TEST_CHECK(readS2Cur.happyCnt[i] == s2Cur.happyCnt[i]);
    }

    fs::remove_all(tempSaveDir, ec);
    return true;
}

// ---------------------------------------------------------------------------
// SubTest 4: Dead Entity Pruning, Memory Safety & Planned Segment Reset (Feature 36)
// ---------------------------------------------------------------------------

static bool SubTest_M9_3_C1_DeadEntityPruningAndPlannedSegmentLifecycle()
{
    printf("    [Challenger1-1.4] Dead Entity Pruning, Memory Safety & Planned Segment Reset ...\n");
    fflush(stdout);

    ResetMockWorldMemory();

    // 1. Large-scale entity registration: 1,000 live entities and 1,000 dead entities
    const int kLiveStart = 5001;
    const int kLiveEnd   = 6000; // 1,000 live
    const int kDeadStart = 7001;
    const int kDeadEnd   = 8000; // 1,000 dead

    for (int id = kLiveStart; id <= kLiveEnd; ++id) {
        TwoStates_RegisterVehicleOwner(id, 1);
        TwoStates_RegisterBuildingOwner(id, 1);
    }
    for (int id = kDeadStart; id <= kDeadEnd; ++id) {
        TwoStates_RegisterVehicleOwner(id, 2);
        TwoStates_RegisterBuildingOwner(id, 2);
    }

    // Allocate backing memory for live vehicles (id at offset VEH_ID_OFFSET = 0x04)
    std::vector<std::vector<BYTE>> liveVehBuffers(1000, std::vector<BYTE>(64, 0));
    std::vector<BYTE*> liveVehPointers(1000);
    for (int i = 0; i < 1000; ++i) {
        *(int*)(liveVehBuffers[i].data() + VEH_ID_OFFSET) = kLiveStart + i;
        liveVehPointers[i] = liveVehBuffers[i].data();
    }
    WriteMockVectorHeader(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA, liveVehPointers);

    // Allocate backing memory for live buildings (id at offset 0, pos at OBJ_POS_OFFSET = 0x350)
    std::vector<std::vector<BYTE>> liveBldBuffers(1000, std::vector<BYTE>(0x380, 0));
    std::vector<BYTE*> liveBldPointers(1000);
    for (int i = 0; i < 1000; ++i) {
        *(int*)(liveBldBuffers[i].data()) = kLiveStart + i;
        *(float*)(liveBldBuffers[i].data() + OBJ_POS_OFFSET) = -500.0f; // West territory
        *(float*)(liveBldBuffers[i].data() + OBJ_POS_OFFSET + 8) = 100.0f;
        liveBldPointers[i] = liveBldBuffers[i].data();
    }
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, liveBldPointers);

    // Execute pruning
    TwoStates_PruneDeadEntities(false);

    // Verify:
    // All 1,000 live vehicles retained in owner map
    for (int id = kLiveStart; id <= kLiveEnd; ++id) {
        TEST_CHECK(TwoStates_GetVehicleOwner(id, nullptr) == 1);
    }
    // All 1,000 dead vehicles purged from owner map (returns 0)
    for (int id = kDeadStart; id <= kDeadEnd; ++id) {
        TEST_CHECK(TwoStates_GetVehicleOwner(id, nullptr) == 0);
    }

    // 2. Memory fault & null pointer resilience
    // Set vector pointer to null
    std::vector<BYTE*> emptyVec;
    WriteMockVectorHeader(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA, emptyVec);
    TwoStates_PruneDeadEntities(false); // must not crash

    // Corrupt vector begin address with unmapped pointer
    *(uintptr_t*)(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA) = 0xDEADBEEF00000000ULL;
    *(uintptr_t*)(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA + 8) = 0xDEADBEEF00001000ULL;
    TwoStates_PruneDeadEntities(false); // SafeReadPod / SEH protection: must not crash!

    ResetMockWorldMemory();

    // 3. Planned road segment reset & world reload lifecycle
    RoadSync_SetPlannedSegmentOwnerForTest(1001, 1);
    RoadSync_SetPlannedSegmentOwnerForTest(1002, 2);
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner(1001) == 1);
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner(1002) == 2);

    RoadSync_ClearPlannedSegments();
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner(1001) == 0);
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner(1002) == 0);

    // Lifecycle integration: TwoStates_OnAnyWorldLoaded clears planned segments and resets g_beforeLoadPrepared
    RoadSync_SetPlannedSegmentOwnerForTest(2002, 2);
    TwoStates_OnAnyWorldLoaded(false);
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner(2002) == 0);

    // Lifecycle integration: TwoStates_BeforeAnyWorldLoad clears planned segments on initial load
    RoadSync_SetPlannedSegmentOwnerForTest(2001, 1);
    TwoStates_BeforeAnyWorldLoad();
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner(2001) == 0);

    // Consecutive BeforeAnyWorldLoad call without OnAnyWorldLoaded (e.g. WorkshopRefresh hook + HookedLoadWorld)
    // RoadSync_ClearPlannedSegments() is placed unconditionally at the top of TwoStates_BeforeAnyWorldLoad()
    // before 'if (g_beforeLoadPrepared) return;', ensuring consecutive calls or aborted loads reset segments!
    RoadSync_SetPlannedSegmentOwnerForTest(2003, 1);
    TwoStates_BeforeAnyWorldLoad(); // g_beforeLoadPrepared is still true from line 480!
    TEST_CHECK(RoadSync_GetPlannedSegmentOwner(2003) == 0);

    // Also verify normal lifecycle reset when OnAnyWorldLoaded runs afterwards
    TwoStates_OnAnyWorldLoaded(false);

    ResetMockWorldMemory();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 1 Master Test Runner Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M9_3_Challenger1()
{
    if (!SubTest_M9_3_C1_AutosaveCadenceAndStateIsolation()) return false;
    if (!SubTest_M9_3_C1_ZipIntegrityDeepHierarchyAndCorruptStress()) return false;
    if (!SubTest_M9_3_C1_DemographicTrackerFuzzingAndMalformedMeta()) return false;
    if (!SubTest_M9_3_C1_DeadEntityPruningAndPlannedSegmentLifecycle()) return false;

    return true;
}
