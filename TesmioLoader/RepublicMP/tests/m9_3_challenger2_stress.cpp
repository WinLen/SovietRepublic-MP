// SPDX-License-Identifier: GPL-3.0-only
// Empirical Stress Test Harness for RepublicMP Milestone M9.3
// Challenger 2: Adversarial Stress Testing of Features 32-36:
//   1. Client Settings Isolation & Camera Branching (Feature 34)
//   2. Factory Vehicle Registration, Replacement Ownership & Selective Pruning (Feature 36)
//   3. Full World Save Backup Integrity & 64-bit Transfer Progress (>2GB) (Features 32 & 33)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <filesystem>

#ifndef COOP_SAVE_META_NAME
#define COOP_SAVE_META_NAME "republicmp_coop.ini"
#endif

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 1: Client Settings Isolation & Camera Fixes (Feature 34)
// ---------------------------------------------------------------------------

static bool SubTest_M9_3_C2_ClientSettingsIsolationAndCameraFixes()
{
    std::filesystem::path stagingDir = "temp_m9_3_ch2_staging";
    std::error_code ec;
    std::filesystem::remove_all(stagingDir, ec);
    std::filesystem::create_directories(stagingDir, ec);

    // 1. Initial join (rejoin = false): host windows.bin and host toolco.bin must both be deleted
    FILE* fw = nullptr;
    if (fopen_s(&fw, (stagingDir / "windows.bin").string().c_str(), "wb") == 0 && fw) {
        fputc(0xAA, fw);
        fclose(fw);
    }
    FILE* ft = nullptr;
    if (fopen_s(&ft, (stagingDir / "toolco.bin").string().c_str(), "wb") == 0 && ft) {
        const char hostData[] = "HOST_TOOLCO_SECRET_HOTKEYS";
        fwrite(hostData, 1, sizeof(hostData), ft);
        fclose(ft);
    }
    TEST_CHECK(std::filesystem::exists(stagingDir / "windows.bin", ec));
    TEST_CHECK(std::filesystem::exists(stagingDir / "toolco.bin", ec));

    SaveSync_IsolateClientSettingsForTest(stagingDir.string().c_str(), false);
    TEST_CHECK(!std::filesystem::exists(stagingDir / "windows.bin", ec));
    TEST_CHECK(!std::filesystem::exists(stagingDir / "toolco.bin", ec));

    // 2. Resync (rejoin = true) with client local backup:
    // Client backup has client's own toolco shortcuts.
    char backupDirBuf[MAX_PATH] = {};
    TEST_CHECK(SaveSync_GetGameSaveDirForTest(backupDirBuf, sizeof(backupDirBuf), "coop_client_backup"));
    std::filesystem::path backupDir(backupDirBuf);
    std::filesystem::create_directories(backupDir, ec);

    FILE* fLocalTool = nullptr;
    if (fopen_s(&fLocalTool, (backupDir / "toolco.bin").string().c_str(), "wb") == 0 && fLocalTool) {
        const char localData[] = "CLIENT_PRESET_GROUP_ALPHA";
        fwrite(localData, 1, sizeof(localData), fLocalTool);
        fclose(fLocalTool);
    }

    // Host incoming save contains host windows.bin and host toolco.bin
    if (fopen_s(&fw, (stagingDir / "windows.bin").string().c_str(), "wb") == 0 && fw) {
        fputc(0xBB, fw);
        fclose(fw);
    }
    if (fopen_s(&ft, (stagingDir / "toolco.bin").string().c_str(), "wb") == 0 && ft) {
        const char hostData2[] = "HOST_OVERWRITE_ATTEMPT";
        fwrite(hostData2, 1, sizeof(hostData2), ft);
        fclose(ft);
    }

    SaveSync_IsolateClientSettingsForTest(stagingDir.string().c_str(), true);
    TEST_CHECK(!std::filesystem::exists(stagingDir / "windows.bin", ec));
    TEST_CHECK(std::filesystem::exists(stagingDir / "toolco.bin", ec));

    char checkToolBuf[64] = {};
    FILE* fReadTool = nullptr;
    if (fopen_s(&fReadTool, (stagingDir / "toolco.bin").string().c_str(), "rb") == 0 && fReadTool) {
        fread(checkToolBuf, 1, sizeof(checkToolBuf), fReadTool);
        fclose(fReadTool);
    }
    TEST_CHECK(strcmp(checkToolBuf, "CLIENT_PRESET_GROUP_ALPHA") == 0);

    // 3. Resync without client local toolco.bin in backup:
    std::filesystem::remove(backupDir / "toolco.bin", ec);
    if (fopen_s(&ft, (stagingDir / "toolco.bin").string().c_str(), "wb") == 0 && ft) {
        fputc(0xCC, ft);
        fclose(ft);
    }
    SaveSync_IsolateClientSettingsForTest(stagingDir.string().c_str(), true);
    TEST_CHECK(!std::filesystem::exists(stagingDir / "toolco.bin", ec));

    std::filesystem::remove_all(stagingDir, ec);
    std::filesystem::remove_all(backupDir, ec);

    // 4. Client local BuildMode and autoPopulate preservation across resync
    BuildMode clientBm = { 1, 0, 80, 50 };
    BuildMode_Write(clientBm);
    BYTE* w = g_mockExeBuffer ? (g_mockExeBuffer + WORLD_RVA) : nullptr;
    if (w) *(w + 0xB08) = 1;

    // Simulate pre-resync capture
    BuildMode preResyncBm = BuildMode_Read();
    BYTE preResyncPop = w ? *(w + 0xB08) : 0;
    TEST_CHECK(BuildMode_Equal(preResyncBm, clientBm));
    TEST_CHECK(preResyncPop == 1);

    // Incoming save load clobbers memory with host's mode & autoPopulate
    BuildMode hostBm = { 0, 1, -200, 10 };
    BuildMode_Write(hostBm);
    if (w) *(w + 0xB08) = 0;
    TEST_CHECK(BuildMode_Equal(BuildMode_Read(), hostBm));

    // Post-load restore on resync
    BuildMode_Write(preResyncBm);
    if (w) *(w + 0xB08) = preResyncPop;
    TEST_CHECK(BuildMode_Equal(BuildMode_Read(), clientBm));
    TEST_CHECK(w && *(w + 0xB08) == 1);

    // Initial join resets to realistic defaults {0,0,0,0}
    BuildMode defBm = { 0, 0, 0, 0 };
    BuildMode_Write(defBm);
    TEST_CHECK(BuildMode_Equal(BuildMode_Read(), defBm));

    // 5. RestoreOrFrameCoopCamera branching:
    // Host restore applies cam1; Client restore applies cam2;
    // When cam2 is missing, Client framing branches to East (+X) and does not take cam1.
    double dummyRub[2] = { 500000.0, 500000.0 }, dummyUsd[2] = { 100000.0, 100000.0 };
    TwoStates_OnSessionJoined(true, COOP_MODE_TWO_STATES, 250.0f, false, dummyRub, dummyUsd);

    const float hostCamPos[3]  = { -1800.0f, 215.0f, 350.0f };
    const float hostCamLook[3] = { -2200.0f,  40.0f, 400.0f };
    const float clientCamPos[3]  = { 2600.0f, 190.0f, -600.0f };
    const float clientCamLook[3] = { 3000.0f,  30.0f, -700.0f };

    TwoStates_SetSavedCameraForTest(1, hostCamPos, hostCamLook);
    TwoStates_SetSavedCameraForTest(2, clientCamPos, clientCamLook);

    float outP[3] = {}, outL[3] = {};
    TwoStates_RestoreOrFrameCameraForTest(true);
    TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(outP, outL, nullptr, nullptr));
    TEST_CHECK_NEAR(outP[0], hostCamPos[0], 0.1f);

    TwoStates_RestoreOrFrameCameraForTest(false);
    TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(outP, outL, nullptr, nullptr));
    TEST_CHECK_NEAR(outP[0], clientCamPos[0], 0.1f);

    // Adversarial: Clear saved camera 2 via a save directory without cam2 metadata
    std::filesystem::path saveMetaDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save_cam_adv";
    std::filesystem::remove_all(saveMetaDir, ec);
    std::filesystem::create_directories(saveMetaDir, ec);

    TwoStates_ApplyCameraView(hostCamPos, hostCamLook);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(saveMetaDir.string().c_str()));
    // Write republicmp_coop.ini with only cam1, omitting cam2
    char iniPath[MAX_PATH] = {};
    _snprintf_s(iniPath, sizeof(iniPath), _TRUNCATE, "%s\\%s", saveMetaDir.string().c_str(), COOP_SAVE_META_NAME);
    WritePrivateProfileStringA("republicmp_coop", "cam2_pos_cm", nullptr, iniPath);
    WritePrivateProfileStringA("republicmp_coop", "cam2_look_cm", nullptr, iniPath);

    TEST_CHECK(TwoStates_ReadSaveMetaFromDir(saveMetaDir.string().c_str(), true));
    TEST_CHECK(TwoStates_GetSavedCameraForTest(1, outP, outL));
    TEST_CHECK(!TwoStates_GetSavedCameraForTest(2, outP, outL)); // Cam2 is absent!

    // In this state: Client restore MUST NOT adopt host cam1 (-1800m)!
    TwoStates_RestoreOrFrameCameraForTest(false);
    TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(outP, outL, nullptr, nullptr));
    TEST_CHECK(outP[0] > 0.0f); // Framed in East territory (+X)
    TEST_CHECK(outL[0] > outP[0]); // Looking East

    // Host restore still recovers host cam1 (-1800m)
    TwoStates_RestoreOrFrameCameraForTest(true);
    TEST_CHECK(TwoStates_GetPendingCameraRestoreForTest(outP, outL, nullptr, nullptr));
    TEST_CHECK_NEAR(outP[0], hostCamPos[0], 0.1f);

    std::filesystem::remove_all(saveMetaDir, ec);
    TwoStates_OnSessionEnded();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 2: Factory Assembly, Replacement Ownership & Pruning (Feature 36)
// ---------------------------------------------------------------------------

static bool SubTest_M9_3_C2_FactoryAssemblyAndReplacementOwnership()
{
    ResetMockWorldMemory();

    // 1. Vehicle 1-for-1 replacement ownership preservation
    // Old vehicle 61001 owned by State 2 (East)
    TwoStates_RegisterVehicleOwner(61001, 2);
    TEST_CHECK(TwoStates_GetVehicleOwner(61001, nullptr) == 2);

    // Replacement vehicle 61002 starts unowned
    TEST_CHECK(TwoStates_GetVehicleOwner(61002, nullptr) == 0);

    // 1-for-1 replacement rule: dst vehicle inherits oldOwner (2)
    int oldOwner = TwoStates_GetVehicleOwner(61001, nullptr);
    if (oldOwner == 1 || oldOwner == 2) {
        TwoStates_RegisterVehicleOwner(61002, oldOwner);
    }
    TEST_CHECK(TwoStates_GetVehicleOwner(61002, nullptr) == 2);

    // Old vehicle 61003 owned by State 1 (West)
    TwoStates_RegisterVehicleOwner(61003, 1);
    TEST_CHECK(TwoStates_GetVehicleOwner(61003, nullptr) == 1);
    int oldOwner1 = TwoStates_GetVehicleOwner(61003, nullptr);
    if (oldOwner1 == 1 || oldOwner1 == 2) {
        TwoStates_RegisterVehicleOwner(61004, oldOwner1);
    }
    TEST_CHECK(TwoStates_GetVehicleOwner(61004, nullptr) == 1);

    // 2. Factory vehicle assembly registration under owner 1 and owner 2
    // West factory (X = -2500m, Z = 500m)
    int westKey = TwoStates_BuildingPosKey(-2500.0f, 500.0f);
    TwoStates_RegisterBuildingOwner(westKey, 1);
    int westFactoryOwner = TwoStates_GetBuildingOwner(westKey, -2500.0f, 500.0f);
    TEST_CHECK(westFactoryOwner == 1);

    // Produced vehicles at West factory get owner 1
    for (int vId = 72001; vId <= 72005; ++vId) {
        TwoStates_RegisterVehicleOwner(vId, westFactoryOwner);
        TEST_CHECK(TwoStates_GetVehicleOwner(vId, nullptr) == 1);
    }

    // East factory (X = +2500m, Z = 500m)
    int eastKey = TwoStates_BuildingPosKey(2500.0f, 500.0f);
    TwoStates_RegisterBuildingOwner(eastKey, 2);
    int eastFactoryOwner = TwoStates_GetBuildingOwner(eastKey, 2500.0f, 500.0f);
    TEST_CHECK(eastFactoryOwner == 2);

    // Produced vehicles at East factory get owner 2
    for (int vId = 73001; vId <= 73005; ++vId) {
        TwoStates_RegisterVehicleOwner(vId, eastFactoryOwner);
        TEST_CHECK(TwoStates_GetVehicleOwner(vId, nullptr) == 2);
    }

    // 3. Dead entity pruning with active vehicles and buildings in mock world
    std::vector<BYTE> vehA(0x100, 0), vehB(0x100, 0), vehC(0x100, 0);
    *(int*)(vehA.data() + VEH_ID_OFFSET) = 80001;
    *(int*)(vehB.data() + VEH_ID_OFFSET) = 80002;
    *(int*)(vehC.data() + VEH_ID_OFFSET) = 80003;
    std::vector<BYTE*> activeVehs = { vehA.data(), vehB.data(), vehC.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA, activeVehs);

    std::vector<BYTE> bldA(0x400, 0), bldB(0x400, 0);
    *(int*)(bldA.data()) = 90001;
    *(float*)(bldA.data() + OBJ_POS_OFFSET) = -500.0f;
    *(float*)(bldA.data() + OBJ_POS_OFFSET + 8) = 100.0f;

    *(int*)(bldB.data()) = 90002;
    *(float*)(bldB.data() + OBJ_POS_OFFSET) = 1500.0f;
    *(float*)(bldB.data() + OBJ_POS_OFFSET + 8) = 200.0f;

    std::vector<BYTE*> activeBlds = { bldA.data(), bldB.data() };
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, activeBlds);

    // Register active entities
    TwoStates_RegisterVehicleOwner(80001, 1);
    TwoStates_RegisterVehicleOwner(80002, 2);
    TwoStates_RegisterVehicleOwner(80003, 1);
    TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(-500.0f, 100.0f), 1);
    TwoStates_RegisterBuildingOwner(TwoStates_BuildingPosKey(1500.0f, 200.0f), 2);

    // Register dead entities (cross-border registered buildings, not present in world memory vectors)
    TwoStates_RegisterVehicleOwner(81004, 2); // Dead vehicle D
    TwoStates_RegisterVehicleOwner(81005, 1); // Dead vehicle E
    TwoStates_RegisterBuildingOwner(91003, 1); // Dead building C (registered in East territory as State 1)
    TwoStates_RegisterBuildingOwner(91004, 2); // Dead building D (registered in West territory as State 2)

    TEST_CHECK(TwoStates_GetBuildingOwner(91003, 2000.0f, 0.0f) == 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(91004, -2000.0f, 0.0f) == 2);

    // Run pruning
    TwoStates_PruneDeadEntities(false);

    // Active entities MUST be retained
    TEST_CHECK(TwoStates_GetVehicleOwner(80001, nullptr) == 1);
    TEST_CHECK(TwoStates_GetVehicleOwner(80002, nullptr) == 2);
    TEST_CHECK(TwoStates_GetVehicleOwner(80003, nullptr) == 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(90001, -500.0f, 100.0f) == 1);
    TEST_CHECK(TwoStates_GetBuildingOwner(90002, 1500.0f, 200.0f) == 2);

    // Dead entities MUST be pruned:
    // Vehicles return 0
    TEST_CHECK(TwoStates_GetVehicleOwner(81004, nullptr) == 0);
    TEST_CHECK(TwoStates_GetVehicleOwner(81005, nullptr) == 0);
    // Buildings purged from map fall back to native territory owner
    TEST_CHECK(TwoStates_GetBuildingOwner(91003, 2000.0f, 0.0f) == 2);
    TEST_CHECK(TwoStates_GetBuildingOwner(91004, -2000.0f, 0.0f) == 1);

    // Verify pruning reflection in serialized save metadata
    std::error_code ec;
    std::filesystem::path pruneMetaDir = std::filesystem::path(g_mockBaseDir) / "media_soviet" / "save_prune_test";
    std::filesystem::remove_all(pruneMetaDir, ec);
    std::filesystem::create_directories(pruneMetaDir, ec);
    TEST_CHECK(TwoStates_WriteSaveMetaToDir(pruneMetaDir.string().c_str()));

    char pruneIni[MAX_PATH] = {};
    _snprintf_s(pruneIni, sizeof(pruneIni), _TRUNCATE, "%s\\%s", pruneMetaDir.string().c_str(), COOP_SAVE_META_NAME);
    char ownersBuf[2048] = {};
    GetPrivateProfileStringA("republicmp_coop", "vehicle_owners", "", ownersBuf, sizeof(ownersBuf), pruneIni);
    std::string vehOwnersStr(ownersBuf);
    TEST_CHECK(vehOwnersStr.find("80001") != std::string::npos);
    TEST_CHECK(vehOwnersStr.find("80002") != std::string::npos);
    TEST_CHECK(vehOwnersStr.find("81004") == std::string::npos); // Dead vehicle purged!
    TEST_CHECK(vehOwnersStr.find("81005") == std::string::npos); // Dead vehicle purged!

    std::filesystem::remove_all(pruneMetaDir, ec);

    // Cleanup mock memory
    activeVehs.clear();
    activeBlds.clear();
    WriteMockVectorHeader(g_mockExeBuffer + VEC_VEHICLES_BEGIN_RVA, activeVehs);
    WriteMockVectorHeader(g_mockExeBuffer + VEC_BUILDINGS_BEGIN_RVA, activeBlds);
    ResetMockWorldMemory();
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Subtest 3: Save Backup Integrity & 64-bit Progress Limits (Features 32 & 33)
// ---------------------------------------------------------------------------

static bool SubTest_M9_3_C2_ClientSaveBackupIntegrityAnd64BitCounters()
{
    // Feature 32 & 33: Full world save backup integrity & 64-bit progress counters > 2 GB
    std::filesystem::path fullSaveSrc = "temp_m9_3_ch2_full_save";
    std::error_code ec;
    std::filesystem::remove_all(fullSaveSrc, ec);
    std::filesystem::create_directories(fullSaveSrc / "nested_cfg", ec);

    const char* saveFiles[] = {
        "header.bin", "buildings.bin", "vehicles.bin", "roads.bin", "terrain.bin",
        "script.ini", "republicmp_coop.ini", "nested_cfg/settings.bin"
    };
    const size_t fileSizes[] = {
        1024, 8192, 4096, 4096, 16384, 512, 256, 128
    };

    for (size_t i = 0; i < sizeof(saveFiles) / sizeof(saveFiles[0]); ++i) {
        std::filesystem::path p = fullSaveSrc / saveFiles[i];
        FILE* fp = nullptr;
        if (fopen_s(&fp, p.string().c_str(), "wb") == 0 && fp) {
            std::vector<BYTE> buf(fileSizes[i]);
            for (size_t b = 0; b < fileSizes[i]; ++b) {
                buf[b] = (BYTE)((b + i * 37) & 0xFF);
            }
            fwrite(buf.data(), 1, buf.size(), fp);
            fclose(fp);
        }
    }

    // Preserve client backup
    TEST_CHECK(SaveSync_PreserveClientBackupForTest(fullSaveSrc.string().c_str()));

    char backupDirBuf[MAX_PATH] = {};
    TEST_CHECK(SaveSync_GetGameSaveDirForTest(backupDirBuf, sizeof(backupDirBuf), "coop_client_backup"));
    std::filesystem::path backupDir(backupDirBuf);

    // Verify all 8 files exist with bit-exact identical bytes
    for (size_t i = 0; i < sizeof(saveFiles) / sizeof(saveFiles[0]); ++i) {
        std::filesystem::path srcP = fullSaveSrc / saveFiles[i];
        std::filesystem::path dstP = backupDir / saveFiles[i];
        TEST_CHECK(std::filesystem::exists(dstP, ec));
        TEST_CHECK(std::filesystem::file_size(srcP, ec) == std::filesystem::file_size(dstP, ec));

        FILE* fSrc = nullptr;
        FILE* fDst = nullptr;
        if (fopen_s(&fSrc, srcP.string().c_str(), "rb") == 0 && fSrc &&
            fopen_s(&fDst, dstP.string().c_str(), "rb") == 0 && fDst) {
            std::vector<BYTE> b1(fileSizes[i]), b2(fileSizes[i]);
            fread(b1.data(), 1, b1.size(), fSrc);
            fread(b2.data(), 1, b2.size(), fDst);
            fclose(fSrc);
            fclose(fDst);
            TEST_CHECK(memcmp(b1.data(), b2.data(), fileSizes[i]) == 0);
        } else {
            if (fSrc) fclose(fSrc);
            if (fDst) fclose(fDst);
            TEST_CHECK(false);
        }
    }

    // Simulate autosave cycle updating republicmp_coop.ini
    FILE* fMetaUpdate = nullptr;
    if (fopen_s(&fMetaUpdate, (fullSaveSrc / "republicmp_coop.ini").string().c_str(), "wb") == 0 && fMetaUpdate) {
        const char updatedMeta[] = "[CoopSave]\nmode=1\nrub_west=9999999\n";
        fwrite(updatedMeta, 1, sizeof(updatedMeta), fMetaUpdate);
        fclose(fMetaUpdate);
    }
    TEST_CHECK(SaveSync_PreserveClientBackupForTest(fullSaveSrc.string().c_str()));
    TEST_CHECK(std::filesystem::file_size(backupDir / "republicmp_coop.ini", ec) ==
               std::filesystem::file_size(fullSaveSrc / "republicmp_coop.ini", ec));

    std::filesystem::remove_all(fullSaveSrc, ec);
    std::filesystem::remove_all(backupDir, ec);

    // 2. 64-bit progress counters > 2 GB (3 GB, 5 GB, 10 GB)
    int64_t q3GB = 3435973836LL;   // 3.2 GB (> 2^31 - 1, bit 31 set)
    int64_t t5GB = 5368709120LL;   // 5.0 GB (> 2^32, requires > 32 bits)
    int64_t t10GB = 10737418240LL; // 10.0 GB (> 2^33)

    SyncBlock_SetPercentEx(64, q3GB, t5GB, 52.4f);
    int pct = 0;
    int64_t outQ = 0, outT = 0;
    float spd = 0.0f;
    SyncBlock_GetTransferStats(&pct, &outQ, &outT, &spd);

    TEST_CHECK(pct == 64);
    TEST_CHECK(outQ == q3GB);
    TEST_CHECK(outT == t5GB);
    TEST_CHECK(outQ > 0); // No negative signed-32 wrap
    TEST_CHECK(outT > 0);
    TEST_CHECK(outT > 0x100000000LL); // Exceeds 4 GB
    TEST_CHECK_NEAR(spd, 52.4f, 0.01f);

    SyncBlock_SetPercentEx(32, q3GB, t10GB, 110.5f);
    SyncBlock_GetTransferStats(&pct, &outQ, &outT, &spd);
    TEST_CHECK(pct == 32);
    TEST_CHECK(outT == t10GB);
    TEST_CHECK(outT > 0x200000000LL); // Exceeds 8 GB
    TEST_CHECK_NEAR(spd, 110.5f, 0.01f);

    SyncBlock_SetPercent(-1);
    return true;
}

// ---------------------------------------------------------------------------
// Challenger 2 Master Test Runner Entry Point
// ---------------------------------------------------------------------------

static bool Test_Stress_M9_3_Challenger2()
{
    printf("    [Challenger2-1: Client Settings Isolation & Camera Fixes (Feature 34)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_C2_ClientSettingsIsolationAndCameraFixes()) return false;

    printf("    [Challenger2-2: Factory Assembly, Replacement Ownership & Pruning (Feature 36)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_C2_FactoryAssemblyAndReplacementOwnership()) return false;

    printf("    [Challenger2-3: Save Backup Integrity & 64-bit Progress Limits (Features 32 & 33)] ...\n"); fflush(stdout);
    if (!SubTest_M9_3_C2_ClientSaveBackupIntegrityAnd64BitCounters()) return false;

    return true;
}
