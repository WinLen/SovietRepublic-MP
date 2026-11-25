// SPDX-License-Identifier: GPL-3.0-only
#include "two_states_internal.h"

void TwoStates_BindPersistence(const TsmHost* host) { if (host) TsmBind(host); }

// ---------------------------------------------------------------------------
// Module-Private Persistence State & MasterSave Hook Contract
// ---------------------------------------------------------------------------

#define RVA_MASTER_SAVE             0x450F50
#define RVA_MASTER_SAVE_AUTOSAVE_JE 0x450FB9

static volatile bool g_sendClientLogs = true;
static volatile bool g_importingSpSave = false;
static char g_savedJoinAddress[64] = "";

static volatile LONG g_saveInProgress = 0;
static volatile LONGLONG g_completedSaveCount = 0;

static const BYTE kMasterSaveExpect[19] = { 0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x0A, 0x00, 0x00 };
typedef void (__fastcall* MasterSaveFn)(BYTE* world);
static MasterSaveFn o_masterSave = nullptr;

bool TwoStates_IsSaveInProgress()
{
    return InterlockedCompareExchange(&g_saveInProgress, 0, 0) > 0;
}

unsigned long long TwoStates_GetCompletedSaveCount()
{
    return (unsigned long long)InterlockedCompareExchange64(&g_completedSaveCount, 0, 0);
}

// ---------------------------------------------------------------------------
// Map Expansion: Border Unlocker (20x20 km) & 30x30 km Generator
// ---------------------------------------------------------------------------

void TwoStates_SetExpandBorders(bool on)
{
    g_expandBorders = on;
}

bool TwoStates_GetExpandBorders()
{
    return g_expandBorders;
}

// Invariant #4 (AGENTS.md): Base terrain maps (media_soviet\terrains, dlc*\terrains) are inviolable.
// TwoStates_PatchSaveScriptIni may ONLY modify script.ini inside save directories (save, save_coop, mp_syncsave_*).
bool TwoStates_IsSafeSaveScriptPath(const char* saveDir)
{
    if (!saveDir || !saveDir[0]) return false;
    std::string lower(saveDir);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '/') c = '\\';
    }
    if (lower.find("\\terrains\\") != std::string::npos ||
        lower.find("\\terrains") == (lower.size() >= 9 ? lower.size() - 9 : std::string::npos) ||
        lower.rfind("terrains\\", 0) == 0) {
        return false;
    }
    if (lower.find("\\save\\") != std::string::npos ||
        lower.find("\\save_coop\\") != std::string::npos ||
        lower.find("mp_syncsave_") != std::string::npos ||
        lower.rfind("save\\", 0) == 0) {
        return true;
    }
    return false;
}

bool TwoStates_PatchSaveScriptIni(const char* saveDir, float customWorldSizeMeters)
{
    if (!TwoStates_IsSafeSaveScriptPath(saveDir)) return false;
    if (!g_expandBorders && customWorldSizeMeters <= 0.0f) return false;
    char path[MAX_PATH] = {};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\script.ini", saveDir);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return false;
    }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    float worldSize = customWorldSizeMeters > 20000.0f ? customWorldSizeMeters : 20000.0f;
    float halfEdge = worldSize * 0.5f - EXPANDED_BOUNDARY_MARGIN; // 9850m for 20km, 14850m for 30km

    std::istringstream in(content);
    std::ostringstream out;
    std::string line;
    int polyVertsLeft = 0;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (polyVertsLeft > 0) {
            float vx = 0, vy = 0, vz = 0;
            int flag = 0;
            if (sscanf_s(line.c_str(), "%f %f %f %d", &vx, &vy, &vz, &flag) == 4) {
                vx = (vx < 0.0f) ? -halfEdge : halfEdge;
                vz = (vz < 0.0f) ? -halfEdge : halfEdge;
                char buf[128];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f %.1f %.1f %d", vx, vy, vz, flag);
                out << buf << "\r\n";
                polyVertsLeft--;
                continue;
            }
            polyVertsLeft = 0;
        }

        if (customWorldSizeMeters > 20000.0f && line.rfind("$WORLDSIZE", 0) == 0) {
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "$WORLDSIZE %.6f %.6f", customWorldSizeMeters, customWorldSizeMeters);
            out << buf << "\r\n";
            continue;
        }
        if (customWorldSizeMeters > 20000.0f && line.rfind("$DRAWDISTANCE", 0) == 0) {
            out << "$DRAWDISTANCE 75000.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_XMIN", 0) == 0) {
            out << "$BOUNDARY_XMIN 150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_ZMIN", 0) == 0) {
            out << "$BOUNDARY_ZMIN 150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_XMAX", 0) == 0) {
            out << "$BOUNDARY_XMAX -150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BOUNDARY_ZMAX", 0) == 0) {
            out << "$BOUNDARY_ZMAX -150.000000\r\n";
            continue;
        }
        if ((g_expandBorders || customWorldSizeMeters > 0.0f) && line.rfind("$BORDER_POLYGON", 0) == 0) {
            int count = 0;
            if (sscanf_s(line.c_str(), "$BORDER_POLYGON %d", &count) == 1 && count > 0 && count <= 64) {
                polyVertsLeft = count;
            }
            out << line << "\r\n";
            continue;
        }

        out << line << "\r\n";
    }

    std::string patched = out.str();
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(patched.data(), 1, patched.size(), f);
    fclose(f);

    Logf("republicmp  TWO_STATES: patched [%s] (worldSize=%.0fm, border=±%.0fm)", path, worldSize, halfEdge);
    return true;
}

// ---------------------------------------------------------------------------
// Persistent Global Co-op Settings (republicmp_settings.ini)
// ---------------------------------------------------------------------------

static void RescueStrandedWorkshopSaveMeta(const char* realExeDir)
{
    static volatile LONG s_rescuedOnce = 0;
    if (!realExeDir || !realExeDir[0] || !g_baseDir || !g_baseDir[0]) return;
    if (_stricmp(realExeDir, g_baseDir) == 0) return;
    if (InterlockedCompareExchange(&s_rescuedOnce, 1, 0) != 0) return;

    std::error_code ec;
    std::filesystem::path wsMedia = std::filesystem::path(g_baseDir) / "media_soviet";
    std::filesystem::path exeMedia = std::filesystem::path(realExeDir) / "media_soviet";
    const char* kSubDirs[2] = { "save", "save_coop" };
    const char* kMetaFiles[3] = { "republicmp_coop.ini", "research_state1.bin", "research_state2.bin" };
    for (int b = 0; b < 2; ++b) {
        std::filesystem::path srcBank = wsMedia / kSubDirs[b];
        if (!std::filesystem::exists(srcBank, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(srcBank, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string folderName = entry.path().filename().string();
            if (_strnicmp(folderName.c_str(), "mp_syncsave_", 12) == 0) continue;
            if (!std::filesystem::exists(entry.path() / kMetaFiles[0], ec)) continue;
            for (int d = 0; d < 2; ++d) {
                std::filesystem::path dstFolder = exeMedia / kSubDirs[d] / folderName;
                if (!std::filesystem::exists(dstFolder / "header.bin", ec)) continue;
                for (int m = 0; m < 3; ++m) {
                    std::filesystem::path sf = entry.path() / kMetaFiles[m];
                    std::filesystem::path df = dstFolder / kMetaFiles[m];
                    if (!std::filesystem::exists(sf, ec)) continue;
                    bool shouldCopy = !std::filesystem::exists(df, ec);
                    if (!shouldCopy) {
                        auto st = std::filesystem::last_write_time(sf, ec);
                        auto dt = std::filesystem::last_write_time(df, ec);
                        if (!ec && st > dt) shouldCopy = true;
                    }
                    if (shouldCopy) {
                        std::filesystem::copy_file(sf, df, std::filesystem::copy_options::overwrite_existing, ec);
                    }
                }
            }
        }
    }
}

static bool GetExeDir(char* outDir, size_t outSize)
{
    if (!outDir || outSize == 0) return false;
    outDir[0] = '\0';
    char exePath[MAX_PATH] = {};
    DWORD len = g_exe ? GetModuleFileNameA(g_exe, exePath, MAX_PATH) : 0;
    if (len > 0 && len < MAX_PATH) {
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
            strncpy_s(outDir, outSize, exePath, _TRUNCATE);
            RescueStrandedWorkshopSaveMeta(outDir);
            return true;
        }
    }
    if (g_baseDir && g_baseDir[0] != '\0') {
        strncpy_s(outDir, outSize, g_baseDir, _TRUNCATE);
        return true;
    }
    len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return false;
    *lastSlash = '\0';
    strncpy_s(outDir, outSize, exePath, _TRUNCATE);
    return true;
}

void TwoStates_LoadSettings()
{
    std::string ini = Lang_GetConfigPath();
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    int mode = GetPrivateProfileIntA("coop", "mode", (int)g_chosenMode, ini.c_str());
    g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    if (!g_inSession) g_sessionMode = g_chosenMode;

    int fullCap = GetPrivateProfileIntA("coop", "full_starting_capital", g_fullStartingCapital ? 1 : 0, ini.c_str());
    g_fullStartingCapital = (fullCap != 0);

    int bw = GetPrivateProfileIntA("coop", "border_half_width_m", (int)lroundf(g_borderHalfWidth), ini.c_str());
    if (bw >= 50 && bw <= 2000) TwoStates_SetBorderHalfWidth((float)bw);

    int expB = GetPrivateProfileIntA("coop", "expand_borders", g_expandBorders ? 1 : 0, ini.c_str());
    g_expandBorders = (expB != 0);

    int show3d = GetPrivateProfileIntA("coop", "show_3d_border", g_show3DBorder ? 1 : 0, ini.c_str());
    g_show3DBorder = (show3d != 0);

    int showMini = GetPrivateProfileIntA("coop", "show_minimap_borders", g_showMinimapBorders ? 1 : 0, ini.c_str());
    g_showMinimapBorders = (showMini != 0);

    int dlc = GetPrivateProfileIntA("coop", "dlc_check", SaveSync_GetDlcCheck() ? 1 : 0, ini.c_str());
    SaveSync_SetDlcCheck(dlc != 0);

    int sendLogs = GetPrivateProfileIntA("coop", "send_client_logs", g_sendClientLogs ? 1 : 0, ini.c_str());
    g_sendClientLogs = (sendLogs != 0);

    char addr[64] = {};
    GetPrivateProfileStringA("coop", "last_join_address", "", addr, sizeof(addr), ini.c_str());
    if (addr[0]) {
        strncpy_s(g_savedJoinAddress, addr, _TRUNCATE);
    }

    ModSync_LoadSettingsFromIni(ini.c_str());

    Logf("republicmp  SETTINGS: loaded from [%s] (mode=%d, fullCap=%d, border=%dm, expand=%d, show3d=%d, showMini=%d, dlc=%d, sendLogs=%d, addr='%s')",
         ini.c_str(), (int)g_chosenMode, g_fullStartingCapital ? 1 : 0, (int)g_borderHalfWidth,
         g_expandBorders ? 1 : 0, g_show3DBorder ? 1 : 0, g_showMinimapBorders ? 1 : 0,
         SaveSync_GetDlcCheck() ? 1 : 0, g_sendClientLogs ? 1 : 0, g_savedJoinAddress);
}

void TwoStates_SaveSettings()
{
    std::string ini = Lang_GetConfigPath();
    char buf[32] = {};

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", (int)g_chosenMode);
    WritePrivateProfileStringA("coop", "mode", buf, ini.c_str());

    WritePrivateProfileStringA("coop", "full_starting_capital", g_fullStartingCapital ? "1" : "0", ini.c_str());

    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", (int)lroundf(g_borderHalfWidth));
    WritePrivateProfileStringA("coop", "border_half_width_m", buf, ini.c_str());

    WritePrivateProfileStringA("coop", "expand_borders", g_expandBorders ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "show_3d_border", g_show3DBorder ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "show_minimap_borders", g_showMinimapBorders ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "dlc_check", SaveSync_GetDlcCheck() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("coop", "send_client_logs", g_sendClientLogs ? "1" : "0", ini.c_str());

    if (g_savedJoinAddress[0]) {
        WritePrivateProfileStringA("coop", "last_join_address", g_savedJoinAddress, ini.c_str());
    }
    ModSync_SaveSettingsToIni(ini.c_str());
}

const char* TwoStates_GetSavedJoinAddress()
{
    return g_savedJoinAddress;
}

void TwoStates_SetSavedJoinAddress(const char* addr)
{
    if (!addr || !addr[0]) return;
    strncpy_s(g_savedJoinAddress, addr, _TRUNCATE);
}

bool TwoStates_GetSendClientLogs()
{
    return g_sendClientLogs;
}

void TwoStates_SetSendClientLogs(bool send)
{
    g_sendClientLogs = send;
}

// ---------------------------------------------------------------------------
// Separate Singleplayer vs Co-op Save Banks (media_soviet\save <-> save_sp / save_coop)
// ---------------------------------------------------------------------------

static bool BuildFullSaveDir(const char* saveRel, char* outFullDir, size_t outSize)
{
    if (!saveRel || !saveRel[0] || !outFullDir || outSize == 0) return false;
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;
    char normRel[160] = {};
    strncpy_s(normRel, saveRel, _TRUNCATE);
    for (char* p = normRel; *p; ++p) {
        if (*p == '/') *p = '\\';
    }
    return _snprintf_s(outFullDir, outSize, _TRUNCATE, "%s\\media_soviet\\%s", exeDir, normRel) >= 0;
}

bool TwoStates_IsCoopBankMounted()
{
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;
    char markerPath[MAX_PATH] = {};
    _snprintf_s(markerPath, sizeof(markerPath), _TRUNCATE, "%s\\media_soviet\\save\\%s", exeDir, COOP_BANK_MARKER_NAME);
    return GetFileAttributesA(markerPath) != INVALID_FILE_ATTRIBUTES;
}

bool TwoStates_PromoteStagingSavesInDir(const char* saveBankDir)
{
    if (!saveBankDir || !saveBankDir[0]) return false;
    std::error_code ec;
    std::filesystem::path dirPath(saveBankDir);
    if (!std::filesystem::exists(dirPath, ec)) return false;

    std::filesystem::path bestStaging;
    std::filesystem::file_time_type bestTime{};
    bool foundStaging = false;

    for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (_strnicmp(name.c_str(), "mp_syncsave_", 12) != 0) continue;
        std::filesystem::path headerBin = entry.path() / "header.bin";
        std::filesystem::path statsIni  = entry.path() / "stats.ini";
        std::filesystem::path coopMeta  = entry.path() / COOP_SAVE_META_NAME;
        if (!std::filesystem::exists(headerBin, ec) ||
            !std::filesystem::exists(statsIni, ec) ||
            !std::filesystem::exists(coopMeta, ec)) {
            continue;
        }
        auto mtime = std::filesystem::last_write_time(headerBin, ec);
        if (!foundStaging || mtime > bestTime) {
            bestTime = mtime;
            bestStaging = entry.path();
            foundStaging = true;
        }
    }
    if (!foundStaging) return false;

    std::filesystem::path dstDir = dirPath / "coop_last_session";
    std::filesystem::path dstHeader = dstDir / "header.bin";
    if (std::filesystem::exists(dstHeader, ec)) {
        auto dstTime = std::filesystem::last_write_time(dstHeader, ec);
        if (!ec && bestTime <= dstTime) return false;
    }

    std::filesystem::create_directories(dstDir, ec);
    std::filesystem::copy(bestStaging, dstDir,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) return false;
    std::filesystem::last_write_time(dstHeader, bestTime, ec);
    Logf("republicmp  SAVE_BANK: promoted newer staging save [%s] -> [%s]",
         bestStaging.string().c_str(), dstDir.string().c_str());
    return true;
}

static void CleanStagingFilesInDir(const std::filesystem::path& dirPath)
{
    if (SaveSync_GetState() != SESSION_IDLE || SaveSync_IsWorldLoaded()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dirPath, ec)) return;
    TwoStates_PromoteStagingSavesInDir(dirPath.string().c_str());
    for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
        std::string name = entry.path().filename().string();
        if (_strnicmp(name.c_str(), "mp_syncsave_", 12) == 0 ||
            (name.size() > 15 && _stricmp(name.c_str() + name.size() - 15, ".republicmp.zip") == 0)) {
            std::error_code rmEc;
            std::filesystem::remove_all(entry.path(), rmEc);
        }
    }
}

static bool MoveOrMergeDir(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return true;
    if (!std::filesystem::exists(dst, ec)) {
        std::filesystem::rename(src, dst, ec);
        if (!ec) return true;
    }
    std::filesystem::create_directories(dst, ec);
    for (const auto& entry : std::filesystem::directory_iterator(src, ec)) {
        std::filesystem::path target = dst / entry.path().filename();
        std::error_code rmEc;
        if (std::filesystem::exists(target, rmEc)) {
            std::filesystem::remove_all(target, rmEc);
        }
        std::filesystem::rename(entry.path(), target, rmEc);
        if (rmEc) {
            std::filesystem::copy(entry.path(), target,
                                  std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                  rmEc);
            std::filesystem::remove_all(entry.path(), rmEc);
        }
    }
    std::filesystem::remove_all(src, ec);
    return true;
}

bool TwoStates_MountSaveBank(bool wantCoop)
{
    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;

    std::filesystem::path mediaDir = std::filesystem::path(exeDir) / "media_soviet";
    std::filesystem::path saveDir  = mediaDir / "save";
    std::filesystem::path spDir    = mediaDir / "save_sp";
    std::filesystem::path coopDir  = mediaDir / "save_coop";
    std::filesystem::path marker   = saveDir / COOP_BANK_MARKER_NAME;

    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);

    bool curCoop = std::filesystem::exists(marker, ec);
    if (curCoop == wantCoop) {
        CleanStagingFilesInDir(saveDir);
        return true;
    }

    if (wantCoop) {
        // Switching from Singleplayer bank (currently in `save`) -> Co-op bank (`save_coop` -> `save`)
        CleanStagingFilesInDir(saveDir);
        MoveOrMergeDir(saveDir, spDir);
        if (std::filesystem::exists(coopDir, ec)) {
            MoveOrMergeDir(coopDir, saveDir);
        } else {
            std::filesystem::create_directories(saveDir, ec);
        }
        FILE* f = nullptr;
        if (fopen_s(&f, marker.string().c_str(), "wb") == 0 && f) {
            const char* txt = "REPUBLICMP_COOP_SAVE_BANK\n";
            fwrite(txt, 1, strlen(txt), f);
            fclose(f);
        }
        // Automatically migrate any pre-existing Co-op saves (folders with republicmp_coop.ini or coop_ prefix) from save_sp -> save
        if (std::filesystem::exists(spDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(spDir, ec)) {
                if (!entry.is_directory(ec)) continue;
                std::string folderName = entry.path().filename().string();
                bool isCoopFolder = (_strnicmp(folderName.c_str(), "coop_", 5) == 0) ||
                                    std::filesystem::exists(entry.path() / COOP_SAVE_META_NAME, ec);
                if (isCoopFolder) {
                    std::filesystem::path dst = saveDir / folderName;
                    if (!std::filesystem::exists(dst, ec)) {
                        std::filesystem::rename(entry.path(), dst, ec);
                    }
                }
            }
        }
        CleanStagingFilesInDir(saveDir);
        Logf("republicmp  SAVE_BANK: mounted CO-OP save bank at [%s]", saveDir.string().c_str());
    } else {
        // Switching from Co-op bank (currently in `save`) -> Singleplayer bank (`save_sp` -> `save`)
        ModSync_EndSession();
        CleanStagingFilesInDir(saveDir);
        MoveOrMergeDir(saveDir, coopDir);
        if (std::filesystem::exists(spDir, ec)) {
            MoveOrMergeDir(spDir, saveDir);
        } else {
            std::filesystem::create_directories(saveDir, ec);
        }
        if (std::filesystem::exists(marker, ec)) {
            std::filesystem::remove(marker, ec);
        }
        CleanStagingFilesInDir(saveDir);
        Logf("republicmp  SAVE_BANK: mounted SINGLEPLAYER save bank at [%s]", saveDir.string().c_str());
    }
    return true;
}

bool TwoStates_GetLatestCoopSaveName(char* outName, size_t outSize)
{
    if (!outName || outSize == 0) return false;
    outName[0] = '\0';

    char exeDir[MAX_PATH] = {};
    if (!GetExeDir(exeDir, sizeof(exeDir))) return false;

    std::filesystem::path mediaDir = std::filesystem::path(exeDir) / "media_soviet";
    std::filesystem::path coopBank = TwoStates_IsCoopBankMounted() ? (mediaDir / "save") : (mediaDir / "save_coop");

    std::error_code ec;
    if (!std::filesystem::exists(coopBank, ec)) return false;
    TwoStates_PromoteStagingSavesInDir(coopBank.string().c_str());

    std::string bestName;
    std::filesystem::file_time_type bestTime{};
    bool found = false;

    for (const auto& entry : std::filesystem::directory_iterator(coopBank, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (_strnicmp(name.c_str(), "mp_syncsave_", 12) == 0) continue;
        std::filesystem::path headerBin = entry.path() / "header.bin";
        std::filesystem::path statsIni  = entry.path() / "stats.ini";
        if (!std::filesystem::exists(headerBin, ec) || !std::filesystem::exists(statsIni, ec)) continue;
        auto mtime = std::filesystem::last_write_time(headerBin, ec);
        if (!found || mtime > bestTime) {
            bestTime = mtime;
            bestName = name;
            found = true;
        }
    }

    if (!found) return false;
    strncpy_s(outName, outSize, bestName.c_str(), _TRUNCATE);
    return true;
}

void TwoStates_SetImportingSpSave(bool importing)
{
    g_importingSpSave = importing;
}

bool TwoStates_IsImportingSpSave()
{
    return g_importingSpSave;
}

// ---------------------------------------------------------------------------
// Per-Save Co-op Metadata (republicmp_coop.ini) & World Load/Save Helpers
// ---------------------------------------------------------------------------

static bool SafeReadWorldMoney(double* outRub, double* outUsd)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        *outRub = *WorldRubles();
        *outUsd = *WorldDollars();
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

void SafeWriteWorldMoney(double rub, double usd)
{
    DiagLog_BeginSafeSeh();
    __try {
        *WorldRubles()  = rub;
        *WorldDollars() = usd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static bool SafeReadPendingLoadInfo(BYTE* outTerrainFlag, char* outSaveRel, size_t outSaveRelSize)
{
    if (!g_exeBase) return false;
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        *outTerrainFlag = *(BYTE*)(g_exeBase + TERRAIN_FLAG_DAT_RVA);
        strncpy_s(outSaveRel, outSaveRelSize, (const char*)(g_exeBase + SAVE_NAME_DAT_RVA), _TRUNCATE);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static std::string ResourcePtrToToken(const BYTE* world, uintptr_t resPtr)
{
    if (!resPtr) return "I0";
    if (world) {
        uintptr_t resBegin = 0, resEnd = 0;
        if (SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET), &resBegin) &&
            SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET + 8), &resEnd) &&
            resBegin && resEnd > resBegin && resPtr >= resBegin && resPtr < resEnd &&
            ((resPtr - resBegin) % WORLD_RESOURCE_STRIDE) == 0) {
            char buf[32];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "R%llu",
                        (unsigned long long)((resPtr - resBegin) / WORLD_RESOURCE_STRIDE));
            return buf;
        }
        for (size_t off = WORLD_RES_PTR_SLOTS_BEGIN_OFF; off <= WORLD_RES_PTR_SLOTS_END_OFF; off += sizeof(uintptr_t)) {
            uintptr_t slotVal = 0;
            if (SafeReadPod((const uintptr_t*)(world + off), &slotVal) && slotVal == resPtr) {
                char buf[32];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "P%llu", (unsigned long long)off);
                return buf;
            }
        }
        uintptr_t wBase = (uintptr_t)world;
        if (resPtr >= wBase && resPtr < wBase + 0x20000ULL) {
            char buf[32];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "W%llu", (unsigned long long)(resPtr - wBase));
            return buf;
        }
    }
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "I%llu", (unsigned long long)resPtr);
    return buf;
}

static uintptr_t ResourceTokenToPtr(const BYTE* world, const std::string& tok)
{
    if (tok.empty()) return 0;
    char prefix = tok[0];
    unsigned long long num = _strtoui64(tok.c_str() + 1, nullptr, 10);
    if (prefix == 'R' && world) {
        uintptr_t resBegin = 0, resEnd = 0;
        if (SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET), &resBegin) &&
            SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET + 8), &resEnd) &&
            resBegin && resEnd > resBegin) {
            uintptr_t ptr = resBegin + (uintptr_t)(num * WORLD_RESOURCE_STRIDE);
            if (ptr >= resBegin && ptr < resEnd) return ptr;
        }
    } else if (prefix == 'W' && world) {
        if (num < 0x20000ULL) {
            return (uintptr_t)world + (uintptr_t)num;
        }
    } else if (prefix == 'P' && world) {
        if (num >= WORLD_RES_PTR_SLOTS_BEGIN_OFF && num <= WORLD_RES_PTR_SLOTS_END_OFF &&
            (num % sizeof(uintptr_t)) == 0) {
            uintptr_t slotVal = 0;
            if (SafeReadPod((const uintptr_t*)(world + (size_t)num), &slotVal)) {
                return slotVal;
            }
        }
    } else if (prefix == 'I' && world) {
        uintptr_t ptr = (uintptr_t)num;
        uintptr_t wBase = (uintptr_t)world;
        if (ptr >= wBase && ptr < wBase + 0x1000000ULL) return ptr;
        uintptr_t resBegin = 0, resEnd = 0;
        if (SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET), &resBegin) &&
            SafeReadPod((const uintptr_t*)(world + WORLD_RESOURCES_OFFSET + 8), &resEnd) &&
            resBegin && resEnd > resBegin && ptr >= resBegin && ptr < resEnd) {
            return ptr;
        }
    }
    return 0;
}

static long long FloatToCents(float v)
{
    return isfinite(v) ? (long long)llround((double)v * 100.0) : 0LL;
}

static long long FloatToMillitons(float v)
{
    return isfinite(v) ? (long long)llround((double)v * 1000.0) : 0LL;
}

static void AppendScaledFloatSlice(std::ostringstream& ss, const char* prefix, const float* vals, int count, double mult)
{
    ss << prefix;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ss << ",";
        long long scaled = isfinite(vals[i]) ? (long long)llround((double)vals[i] * mult) : 0LL;
        ss << scaled;
    }
}

static void AppendIntSlice(std::ostringstream& ss, const char* prefix, const int32_t* vals, int count)
{
    ss << prefix;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ss << ",";
        ss << vals[i];
    }
}

static std::string SerializeStateEconPeriod(const BYTE* world, const StateEconPeriodStats& st)
{
    std::ostringstream ss;
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "D:%d,%d", st.day, st.year);
    ss << buf;
    AppendScaledFloatSlice(ss, ";V:", st.scalars.vehicles, 4, 100.0);
    AppendScaledFloatSlice(ss, ";M:", st.scalars.immigrantSpend, 2, 100.0);
    AppendIntSlice(ss, ";C:", st.scalars.citizenCounts, 5);
    AppendScaledFloatSlice(ss, ";S:", st.scalars.satisfaction, 10, 1000.0);
    AppendScaledFloatSlice(ss, ";A:", st.scalars.citizenAverages, 3, 1000.0);
    AppendIntSlice(ss, ";P:", st.scalars.demographics, 4);
    AppendIntSlice(ss, ";B:", st.scalars.citizenBrackets, 9);
    AppendScaledFloatSlice(ss, ";L:", st.scalars.loans, 4, 100.0);

    bool hasTourism = false;
    for (int i = 0; i < 44; ++i) {
        long long cents = FloatToCents(st.scalars.tourism[i]);
        if (cents != 0) {
            ss << (hasTourism ? "," : ";T:");
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d:%lld", i, cents);
            ss << buf;
            hasTourism = true;
        }
    }

    bool hasExtra = false;
    for (int i = 0; i < 21; ++i) {
        if (st.scalars.extraCounts[i] != 0) {
            ss << (hasExtra ? "," : ";X:");
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d:%d", i, st.scalars.extraCounts[i]);
            ss << buf;
            hasExtra = true;
        }
    }

    for (int v = 0; v < STAT_RES_VEC_COUNT; ++v) {
        const std::vector<SovietStatResEntry>& vec = st.resVecs[v];
        if (vec.empty()) continue;
        ss << ";R" << v << ":";
        for (size_t i = 0; i < vec.size(); ++i) {
            if (i > 0) ss << ",";
            std::string rTok = ResourcePtrToToken(world, vec[i].resourcePtr);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s:%lld:%lld",
                        rTok.c_str(), FloatToMillitons(vec[i].amountTons), FloatToCents(vec[i].moneyValue));
            ss << buf;
        }
    }
    return ss.str();
}

static void ParseScaledFloatSlice(const std::string& body, float* outFloats, int maxCount, double divisor)
{
    size_t pos = 0;
    for (int i = 0; i < maxCount && pos < body.size(); ++i) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        long long scaled = _strtoi64(body.substr(pos, comma - pos).c_str(), nullptr, 10);
        outFloats[i] = (float)((double)scaled / divisor);
        pos = comma + 1;
    }
}

static void ParseIntSlice(const std::string& body, int32_t* outInts, int maxCount)
{
    size_t pos = 0;
    for (int i = 0; i < maxCount && pos < body.size(); ++i) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        outInts[i] = (int32_t)atoi(body.substr(pos, comma - pos).c_str());
        pos = comma + 1;
    }
}

static void ParseTourismTokenBody(const std::string& body, float outTourism[44])
{
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        std::string item = body.substr(pos, comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            int idx = atoi(item.substr(0, colon).c_str());
            long long cents = _strtoi64(item.substr(colon + 1).c_str(), nullptr, 10);
            if (idx >= 0 && idx < 44) {
                outTourism[idx] = (float)((double)cents / 100.0);
            }
        }
        pos = comma + 1;
    }
}

static void ParseExtraCountsTokenBody(const std::string& body, int32_t outExtra[21])
{
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        std::string item = body.substr(pos, comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            int idx = atoi(item.substr(0, colon).c_str());
            int32_t val = (int32_t)atoi(item.substr(colon + 1).c_str());
            if (idx >= 0 && idx < 21) {
                outExtra[idx] = val;
            }
        }
        pos = comma + 1;
    }
}

static void ParseResVecTokenBody(const BYTE* world, const std::string& body, std::vector<SovietStatResEntry>& outVec)
{
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        if (comma == std::string::npos) comma = body.size();
        std::string item = body.substr(pos, comma - pos);
        size_t c1 = item.find(':');
        size_t c2 = (c1 != std::string::npos) ? item.find(':', c1 + 1) : std::string::npos;
        if (c1 != std::string::npos && c2 != std::string::npos) {
            uintptr_t resPtr = ResourceTokenToPtr(world, item.substr(0, c1));
            long long mtons = _strtoi64(item.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
            long long cents = _strtoi64(item.substr(c2 + 1).c_str(), nullptr, 10);
            if (resPtr != 0) {
                SovietStatResEntry e = { resPtr, (float)((double)mtons / 1000.0), (float)((double)cents / 100.0) };
                outVec.push_back(e);
            }
        }
        pos = comma + 1;
    }
}

static void ApplyEconPeriodSegment(const BYTE* world, const std::string& tag, const std::string& body, StateEconPeriodStats* out)
{
    if (tag == "D")      sscanf_s(body.c_str(), "%d,%d", &out->day, &out->year);
    else if (tag == "V") ParseScaledFloatSlice(body, out->scalars.vehicles, 4, 100.0);
    else if (tag == "M") ParseScaledFloatSlice(body, out->scalars.immigrantSpend, 2, 100.0);
    else if (tag == "C") ParseIntSlice(body, out->scalars.citizenCounts, 5);
    else if (tag == "S") ParseScaledFloatSlice(body, out->scalars.satisfaction, 10, 1000.0);
    else if (tag == "A") ParseScaledFloatSlice(body, out->scalars.citizenAverages, 3, 1000.0);
    else if (tag == "P") ParseIntSlice(body, out->scalars.demographics, 4);
    else if (tag == "B") ParseIntSlice(body, out->scalars.citizenBrackets, 9);
    else if (tag == "L") ParseScaledFloatSlice(body, out->scalars.loans, 4, 100.0);
    else if (tag == "T") ParseTourismTokenBody(body, out->scalars.tourism);
    else if (tag == "X") ParseExtraCountsTokenBody(body, out->scalars.extraCounts);
    else if (tag.size() >= 2 && tag[0] == 'R') {
        int vIdx = atoi(tag.c_str() + 1);
        if (vIdx >= 0 && vIdx < STAT_RES_VEC_COUNT) {
            ParseResVecTokenBody(world, body, out->resVecs[vIdx]);
        }
    }
}

static void DeserializeStateEconPeriod(const BYTE* world, const std::string& str, StateEconPeriodStats* out)
{
    if (!out) return;
    ClearStateEconPeriod(out, 0, 0);
    size_t pos = 0;
    while (pos < str.size()) {
        size_t semi = str.find(';', pos);
        if (semi == std::string::npos) semi = str.size();
        std::string seg = str.substr(pos, semi - pos);
        pos = semi + 1;
        size_t colon = seg.find(':');
        if (colon != std::string::npos) {
            ApplyEconPeriodSegment(world, seg.substr(0, colon), seg.substr(colon + 1), out);
        }
    }
}

static bool ParseCoopMetaEconKey(const BYTE* world, const std::string& key, const std::string& val, bool haveEconCur[2])
{
    if (key == "econ_cur1") {
        DeserializeStateEconPeriod(world, val, &g_stateCurEcon[0]);
        haveEconCur[0] = true;
        return true;
    }
    if (key == "econ_cur2") {
        DeserializeStateEconPeriod(world, val, &g_stateCurEcon[1]);
        haveEconCur[1] = true;
        return true;
    }
    if (key == "econ_hist_count") {
        int cnt = atoi(val.c_str());
        if (cnt > 0 && cnt <= 4096) {
            g_stateHistEcon[0].resize((size_t)cnt);
            g_stateHistEcon[1].resize((size_t)cnt);
            TwoStates_BumpHistSwapGen();
        }
        return true;
    }
    if (_strnicmp(key.c_str(), "econ_hist1_", 11) == 0) {
        int idx = atoi(key.c_str() + 11);
        if (idx >= 0 && (size_t)idx < g_stateHistEcon[0].size()) {
            DeserializeStateEconPeriod(world, val, &g_stateHistEcon[0][(size_t)idx]);
            TwoStates_BumpHistSwapGen();
        }
        return true;
    }
    if (_strnicmp(key.c_str(), "econ_hist2_", 11) == 0) {
        int idx = atoi(key.c_str() + 11);
        if (idx >= 0 && (size_t)idx < g_stateHistEcon[1].size()) {
            DeserializeStateEconPeriod(world, val, &g_stateHistEcon[1][(size_t)idx]);
            TwoStates_BumpHistSwapGen();
        }
        return true;
    }
    return false;
}

static void WriteCoopMetaVec3Cm(std::ostringstream& out, const char* key, const float v[3])
{
    out << key << "="
        << (long long)llround((double)v[0] * 100.0) << ","
        << (long long)llround((double)v[1] * 100.0) << ","
        << (long long)llround((double)v[2] * 100.0) << "\r\n";
}

static std::string SerializeEngineResearchUnlocks(const StateEngineResearch& src)
{
    std::ostringstream ss;
    for (size_t i = 0; i < WORLD_RESEARCH_UNLOCKS_BYTES; ++i) {
        if (i > 0) ss << ",";
        ss << (unsigned)src.unlocks[i];
    }
    return ss.str();
}

static std::string SerializeEngineResearchItems(const StateEngineResearch& src)
{
    std::ostringstream ss;
    for (size_t i = 0; i < src.items.size(); ++i) {
        if (i > 0) ss << ";";
        const EngineResearchItemSlice& it = src.items[i];
        long long progBp = (long long)llround((double)it.progress * 1000000.0);
        int posKey = it.researchingBld ? ComputeBuildingPosKeySafe(it.researchingBld) : it.bldPosKey;
        ss << (unsigned)it.available << ":" << progBp << ":" << posKey << ":" << it.timerE0;
    }
    return ss.str();
}

static void ParseEngineResearchUnlocks(const std::string& val, StateEngineResearch* dst)
{
    if (!dst) return;
    memset(dst->unlocks, 0, sizeof(dst->unlocks));
    size_t pos = 0;
    size_t idx = 0;
    while (pos < val.size() && idx < WORLD_RESEARCH_UNLOCKS_BYTES) {
        size_t comma = val.find(',', pos);
        if (comma == std::string::npos) comma = val.size();
        dst->unlocks[idx++] = (uint8_t)atoi(val.substr(pos, comma - pos).c_str());
        pos = comma + 1;
    }
    dst->gen++;
    dst->initialized = true;
    dst->loadedFromMeta = true;
}

static void ParseEngineResearchItems(const std::string& val, int stateOwner, bool keepLoadedSummary, StateEngineResearch* dst)
{
    if (!dst) return;
    dst->items.clear();
    bool anyUnresolved = false;
    size_t pos = 0;
    while (pos < val.size()) {
        size_t semi = val.find(';', pos);
        if (semi == std::string::npos) semi = val.size();
        std::string tok = val.substr(pos, semi - pos);
        if (!tok.empty()) {
            EngineResearchItemSlice s = {};
            size_t c1 = tok.find(':');
            size_t c2 = (c1 != std::string::npos) ? tok.find(':', c1 + 1) : std::string::npos;
            size_t c3 = (c2 != std::string::npos) ? tok.find(':', c2 + 1) : std::string::npos;
            if (c1 != std::string::npos && c2 != std::string::npos) {
                s.available = (uint8_t)atoi(tok.substr(0, c1).c_str());
                long long progBp = _strtoi64(tok.substr(c1 + 1, c2 - (c1 + 1)).c_str(), nullptr, 10);
                s.progress = (float)((double)progBp / 1000000.0);
                if (c3 != std::string::npos) {
                    s.bldPosKey = atoi(tok.substr(c2 + 1, c3 - (c2 + 1)).c_str());
                    s.timerE0   = (int32_t)atoi(tok.substr(c3 + 1).c_str());
                } else {
                    s.bldPosKey = atoi(tok.substr(c2 + 1).c_str());
                }
            }
            if (s.bldPosKey > 0 && !s.researchingBld) anyUnresolved = true;
            dst->items.push_back(s);
        }
        pos = semi + 1;
    }
    dst->hasUnresolvedBldPtr = anyUnresolved;
    dst->gen++;
    dst->initialized = true;
    dst->loadedFromMeta = true;
    if (!keepLoadedSummary) {
        SyncSummaryResearchFromSlice(stateOwner, *dst);
    }
}

static std::string SerializeEngineVehLicenses(const StateEngineVehLicenses& src)
{
    std::ostringstream ss;
    for (size_t i = 0; i < src.ownedKeys.size(); ++i) {
        if (i > 0) ss << ",";
        ss << src.ownedKeys[i];
    }
    return ss.str();
}

static void ParseEngineVehLicenses(const std::string& val, StateEngineVehLicenses* dst)
{
    if (!dst) return;
    dst->ownedKeys.clear();
    dst->cachedFlags.clear();
    dst->cachedFlagsGen = 0;
    dst->cachedFlagsWorldGen = 0;
    size_t pos = 0;
    while (pos < val.size()) {
        size_t comma = val.find(',', pos);
        if (comma == std::string::npos) comma = val.size();
        std::string tok = val.substr(pos, comma - pos);
        if (!tok.empty()) {
            AddLicenseKeyToSlice(dst, tok);
        }
        pos = comma + 1;
    }
    dst->gen++;
    dst->initialized = true;
    dst->loadedFromMeta = true;
}

// ---------------------------------------------------------------------------
// Demographic Trackers Serialization (Feature 35)
// ---------------------------------------------------------------------------

static std::string SerializeStateTrackerSlice(const StateTrackerSlice& slice)
{
    std::ostringstream ss;
    AppendScaledFloatSlice(ss, "HS:", slice.healthSum, TRACKER_HEALTH_CAUSE_COUNT, 1000.0);
    AppendIntSlice(ss, ";HC:", slice.healthCnt, TRACKER_HEALTH_CAUSE_COUNT);
    AppendScaledFloatSlice(ss, ";PS:", slice.happySum, TRACKER_HAPPY_CAUSE_COUNT, 1000.0);
    AppendIntSlice(ss, ";PC:", slice.happyCnt, TRACKER_HAPPY_CAUSE_COUNT);
    return ss.str();
}

static void DeserializeStateTrackerSlice(const std::string& str, StateTrackerSlice* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    while (pos < str.size()) {
        size_t semi = str.find(';', pos);
        if (semi == std::string::npos) semi = str.size();
        std::string seg = str.substr(pos, semi - pos);
        pos = semi + 1;
        size_t colon = seg.find(':');
        if (colon != std::string::npos) {
            std::string tag = seg.substr(0, colon);
            std::string vals = seg.substr(colon + 1);
            if (tag == "HS") {
                ParseScaledFloatSlice(vals, out->healthSum, TRACKER_HEALTH_CAUSE_COUNT, 1000.0);
            } else if (tag == "HC") {
                ParseIntSlice(vals, out->healthCnt, TRACKER_HEALTH_CAUSE_COUNT);
            } else if (tag == "PS") {
                ParseScaledFloatSlice(vals, out->happySum, TRACKER_HAPPY_CAUSE_COUNT, 1000.0);
            } else if (tag == "PC") {
                ParseIntSlice(vals, out->happyCnt, TRACKER_HAPPY_CAUSE_COUNT);
            }
        }
    }
}

static bool ParseCoopMetaTrackerKey(const std::string& curSection, const std::string& key, const std::string& val,
                                    bool haveTrackerCur[2], bool haveTrackerOld[2])
{
    if (curSection == "TrackerHealth0") {
        if (key == "cur") { DeserializeStateTrackerSlice(val, &g_stateTrackerCur[0]); haveTrackerCur[0] = true; return true; }
        if (key == "old") { DeserializeStateTrackerSlice(val, &g_stateTrackerOld[0]); haveTrackerOld[0] = true; return true; }
    } else if (curSection == "TrackerHealth1") {
        if (key == "cur") { DeserializeStateTrackerSlice(val, &g_stateTrackerCur[1]); haveTrackerCur[1] = true; return true; }
        if (key == "old") { DeserializeStateTrackerSlice(val, &g_stateTrackerOld[1]); haveTrackerOld[1] = true; return true; }
    } else {
        if (key == "tracker_cur1") { DeserializeStateTrackerSlice(val, &g_stateTrackerCur[0]); haveTrackerCur[0] = true; return true; }
        if (key == "tracker_cur2") { DeserializeStateTrackerSlice(val, &g_stateTrackerCur[1]); haveTrackerCur[1] = true; return true; }
        if (key == "tracker_old1") { DeserializeStateTrackerSlice(val, &g_stateTrackerOld[0]); haveTrackerOld[0] = true; return true; }
        if (key == "tracker_old2") { DeserializeStateTrackerSlice(val, &g_stateTrackerOld[1]); haveTrackerOld[1] = true; return true; }
    }
    return false;
}

bool TwoStates_GetStateTrackerSliceForTest(int state, bool isOld, StateTrackerSlice* out)
{
    if (!out || (state != 1 && state != 2)) return false;
    *out = isOld ? g_stateTrackerOld[state - 1] : g_stateTrackerCur[state - 1];
    return true;
}

void TwoStates_SetStateTrackerSliceForTest(int state, bool isOld, const StateTrackerSlice& in)
{
    if (state != 1 && state != 2) return;
    if (isOld) g_stateTrackerOld[state - 1] = in;
    else g_stateTrackerCur[state - 1] = in;
    g_stateTrackerInitialized = true;
}

static void WriteCoopSaveMetaInternal(const char* fullSaveDir)
{
    if (!fullSaveDir || !fullSaveDir[0]) return;
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);

    int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
    double rub0 = g_stateRub[0];
    double rub1 = g_stateRub[1];
    double usd0 = g_stateUsd[0];
    double usd1 = g_stateUsd[1];

    if (!g_treasuryInitialized) {
        double wRub = 0.0, wUsd = 0.0;
        if (SafeReadWorldMoney(&wRub, &wUsd)) {
            if (mode == COOP_MODE_TWO_STATES) {
                if (g_fullStartingCapital) {
                    rub0 = wRub; rub1 = wRub;
                    usd0 = wUsd; usd1 = wUsd;
                } else {
                    rub0 = wRub * 0.5; rub1 = wRub - rub0;
                    usd0 = wUsd * 0.5; usd1 = wUsd - usd0;
                }
            } else {
                rub0 = wRub; rub1 = 0.0;
                usd0 = wUsd; usd1 = 0.0;
            }
        }
    }

    std::ostringstream out;
    out << "[republicmp_coop]\r\n";
    out << "version=1\r\n";
    out << "coop_mode=" << mode << "\r\n";
    out << "border_half_width_m=" << (int)lroundf(g_borderHalfWidth) << "\r\n";
    out << "expand_borders=" << (g_expandBorders ? 1 : 0) << "\r\n";
    out << "state1_rub_cents=" << (long long)llround(rub0 * 100.0) << "\r\n";
    out << "state2_rub_cents=" << (long long)llround(rub1 * 100.0) << "\r\n";
    out << "state1_usd_cents=" << (long long)llround(usd0 * 100.0) << "\r\n";
    out << "state2_usd_cents=" << (long long)llround(usd1 * 100.0) << "\r\n";
    out << "net_trade1_rub_cents=" << (long long)llround(g_netTradeRub[0] * 100.0) << "\r\n";
    out << "net_trade2_rub_cents=" << (long long)llround(g_netTradeRub[1] * 100.0) << "\r\n";
    out << "net_trade1_usd_cents=" << (long long)llround(g_netTradeUsd[0] * 100.0) << "\r\n";
    out << "net_trade2_usd_cents=" << (long long)llround(g_netTradeUsd[1] * 100.0) << "\r\n";

    float localPos[3] = {}, localLook[3] = {};
    bool haveLocal = TwoStates_GetCameraView(localPos, localLook) && IsValidCameraPair(localPos, localLook);
    PartnerCursorInfo pc = LockstepNet_GetPartnerCursor();
    const bool havePartnerLive = pc.valid && IsValidCameraPair(pc.camPos, pc.camLookAt);
    const bool localIsState2 = (TwoStates_LocalState() == 2);

    float c1_pos[3] = {}, c1_look[3] = {};
    bool haveC1 = false;
    if (!localIsState2 && haveLocal) {
        memcpy(c1_pos, localPos, sizeof(c1_pos));
        memcpy(c1_look, localLook, sizeof(c1_look));
        haveC1 = true;
    } else if (localIsState2 && havePartnerLive) {
        memcpy(c1_pos, pc.camPos, sizeof(c1_pos));
        memcpy(c1_look, pc.camLookAt, sizeof(c1_look));
        haveC1 = true;
    } else if (g_hasSavedCam1) {
        memcpy(c1_pos, g_savedCam1Pos, sizeof(c1_pos));
        memcpy(c1_look, g_savedCam1Look, sizeof(c1_look));
        haveC1 = true;
    }
    if (haveC1) {
        WriteCoopMetaVec3Cm(out, "cam1_pos_cm", c1_pos);
        WriteCoopMetaVec3Cm(out, "cam1_look_cm", c1_look);
    }

    float c2_pos[3] = {}, c2_look[3] = {};
    bool haveC2 = false;
    if (localIsState2 && haveLocal) {
        memcpy(c2_pos, localPos, sizeof(c2_pos));
        memcpy(c2_look, localLook, sizeof(c2_look));
        haveC2 = true;
    } else if (!localIsState2 && havePartnerLive) {
        memcpy(c2_pos, pc.camPos, sizeof(c2_pos));
        memcpy(c2_look, pc.camLookAt, sizeof(c2_look));
        haveC2 = true;
    } else if (g_hasSavedCam2) {
        memcpy(c2_pos, g_savedCam2Pos, sizeof(c2_pos));
        memcpy(c2_look, g_savedCam2Look, sizeof(c2_look));
        haveC2 = true;
    }
    if (haveC2) {
        WriteCoopMetaVec3Cm(out, "cam2_pos_cm", c2_pos);
        WriteCoopMetaVec3Cm(out, "cam2_look_cm", c2_look);
    }

    TwoStates_PruneDeadEntities(true);

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    out << "building_owners=";
    bool first = true;
    for (const auto& kv : g_buildingOwners) {
        if (!first) out << ",";
        out << kv.first << ":" << kv.second;
        first = false;
    }
    out << "\r\n";

    out << "vehicle_owners=";
    first = true;
    for (const auto& kv : g_vehicleOwners) {
        if (!first) out << ",";
        out << kv.first << ":" << kv.second;
        first = false;
    }
    out << "\r\n";
    LeaveCriticalSection(&g_ownerLock);

    const BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;
    if (g_stateEconInitialized && world) {
        out << "econ_cur1=" << SerializeStateEconPeriod(world, g_stateCurEcon[0]) << "\r\n";
        out << "econ_cur2=" << SerializeStateEconPeriod(world, g_stateCurEcon[1]) << "\r\n";
        size_t histCount = std::min(g_stateHistEcon[0].size(), g_stateHistEcon[1].size());
        out << "econ_hist_count=" << histCount << "\r\n";
        for (size_t i = 0; i < histCount; ++i) {
            out << "econ_hist1_" << i << "=" << SerializeStateEconPeriod(world, g_stateHistEcon[0][i]) << "\r\n";
            out << "econ_hist2_" << i << "=" << SerializeStateEconPeriod(world, g_stateHistEcon[1][i]) << "\r\n";
        }
    }

    out << "state1_research_unlocked=" << g_stateResearch[0].unlockedMask << "\r\n";
    out << "state2_research_unlocked=" << g_stateResearch[1].unlockedMask << "\r\n";
    out << "state1_research_active=" << g_stateResearch[0].activeTechId << "\r\n";
    out << "state2_research_active=" << g_stateResearch[1].activeTechId << "\r\n";
    out << "state1_research_progress=" << (long long)llround((double)g_stateResearch[0].activeTechProgress * 10000.0) << "\r\n";
    out << "state2_research_progress=" << (long long)llround((double)g_stateResearch[1].activeTechProgress * 10000.0) << "\r\n";

    if (mode == COOP_MODE_TWO_STATES && world) {
        EnsureEngineResearchInitialized(const_cast<BYTE*>(world));
        EnsureEngineVehLicensesInitialized(const_cast<BYTE*>(world));
    }
    if (g_engineResearch[0].initialized) {
        out << "engine_research_unlocks1=" << SerializeEngineResearchUnlocks(g_engineResearch[0]) << "\r\n";
        out << "engine_research_items1=" << SerializeEngineResearchItems(g_engineResearch[0]) << "\r\n";
    }
    if (g_engineResearch[1].initialized) {
        out << "engine_research_unlocks2=" << SerializeEngineResearchUnlocks(g_engineResearch[1]) << "\r\n";
        out << "engine_research_items2=" << SerializeEngineResearchItems(g_engineResearch[1]) << "\r\n";
    }
    if (g_engineVehLicenses[0].initialized) {
        out << "veh_licenses1=" << SerializeEngineVehLicenses(g_engineVehLicenses[0]) << "\r\n";
    }
    if (g_engineVehLicenses[1].initialized) {
        out << "veh_licenses2=" << SerializeEngineVehLicenses(g_engineVehLicenses[1]) << "\r\n";
    }

    // Feature 35: Demographic Trackers Persistence
    out << "tracker_cur1=" << SerializeStateTrackerSlice(g_stateTrackerCur[0]) << "\r\n";
    out << "tracker_cur2=" << SerializeStateTrackerSlice(g_stateTrackerCur[1]) << "\r\n";
    out << "tracker_old1=" << SerializeStateTrackerSlice(g_stateTrackerOld[0]) << "\r\n";
    out << "tracker_old2=" << SerializeStateTrackerSlice(g_stateTrackerOld[1]) << "\r\n";

    out << "\r\n[TrackerHealth0]\r\n";
    out << "cur=" << SerializeStateTrackerSlice(g_stateTrackerCur[0]) << "\r\n";
    out << "old=" << SerializeStateTrackerSlice(g_stateTrackerOld[0]) << "\r\n";

    out << "\r\n[TrackerHealth1]\r\n";
    out << "cur=" << SerializeStateTrackerSlice(g_stateTrackerCur[1]) << "\r\n";
    out << "old=" << SerializeStateTrackerSlice(g_stateTrackerOld[1]) << "\r\n";

    std::string data = out.str();
    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "wb") == 0 && f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        Logf("republicmp  COOP_META: saved [%s] (mode=%d, rub=[%.0f, %.0f], usd=[%.0f, %.0f], blds=%zu, vehs=%zu)",
             metaPath, mode, rub0, rub1, usd0, usd1, g_buildingOwners.size(), g_vehicleOwners.size());
    }

    char r1Path[MAX_PATH] = {};
    char r2Path[MAX_PATH] = {};
    _snprintf_s(r1Path, sizeof(r1Path), _TRUNCATE, "%s\\research_state1.bin", fullSaveDir);
    _snprintf_s(r2Path, sizeof(r2Path), _TRUNCATE, "%s\\research_state2.bin", fullSaveDir);
    FILE* fr1 = nullptr;
    if (fopen_s(&fr1, r1Path, "wb") == 0 && fr1) {
        fwrite(&g_stateResearch[0], 1, sizeof(StateResearchTree), fr1);
        fclose(fr1);
    }
    FILE* fr2 = nullptr;
    if (fopen_s(&fr2, r2Path, "wb") == 0 && fr2) {
        fwrite(&g_stateResearch[1], 1, sizeof(StateResearchTree), fr2);
        fclose(fr2);
    }
}

static void ParseOwnersList(const std::string& val, std::unordered_map<int, int>& outMap, int minId, bool* outHadPoisonedZero)
{
    outMap.clear();
    if (outHadPoisonedZero) *outHadPoisonedZero = false;
    size_t pos = 0;
    while (pos < val.size()) {
        size_t comma = val.find(',', pos);
        if (comma == std::string::npos) comma = val.size();
        std::string token = val.substr(pos, comma - pos);
        size_t colon = token.find(':');
        if (colon != std::string::npos) {
            int id = atoi(token.substr(0, colon).c_str());
            int owner = atoi(token.substr(colon + 1).c_str());
            if (owner == 1 || owner == 2) {
                if (id >= minId) {
                    outMap[id] = owner;
                } else if (outHadPoisonedZero && id == 0) {
                    *outHadPoisonedZero = true;
                }
            }
        }
        pos = comma + 1;
    }
}

static void MergePeriodTradeResVecsIntoState(StateEconPeriodStats* dst, StateEconPeriodStats* src)
{
    if (!dst || !src) return;
    for (int v = 0; v < 8; ++v) {
        const std::vector<SovietStatResEntry>& sv = src->resVecs[v];
        for (size_t i = 0; i < sv.size(); ++i) {
            bool isNeg = (sv[i].amountTons < 0.0f);
            AddEntryDeltaToStateResVec(dst->resVecs[v], sv[i].resourcePtr, isNeg,
                                       sv[i].amountTons, sv[i].moneyValue);
        }
        src->resVecs[v].clear();
    }
}

static void ClearPeriodTradeResVecs(StateEconPeriodStats* st)
{
    if (!st) return;
    for (int v = 0; v < 8; ++v) {
        st->resVecs[v].clear();
    }
}

static void SanitizeEconPeriodScalars(StateEconPeriodStats* st)
{
    if (!st) return;
    for (int i = 0; i < 4; ++i) {
        if (!isfinite(st->scalars.vehicles[i]) || fabsf(st->scalars.vehicles[i]) > 1.0e11f)
            st->scalars.vehicles[i] = 0.0f;
    }
    for (int i = 0; i < 10; ++i) {
        if (!isfinite(st->scalars.satisfaction[i]) || fabsf(st->scalars.satisfaction[i]) > 1.0e5f)
            st->scalars.satisfaction[i] = 0.0f;
    }
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(st->scalars.citizenAverages[i]) || fabsf(st->scalars.citizenAverages[i]) > 1.0e5f)
            st->scalars.citizenAverages[i] = 0.0f;
    }
    for (int i = 10; i <= 11; ++i) {
        float f = 0.0f;
        memcpy(&f, &st->scalars.extraCounts[i], sizeof(float));
        if (!isfinite(f) || f < 0.0f || f > 1.0e7f)
            st->scalars.extraCounts[i] = 0;
    }
}

static void HealCorruptedSaveBalances(long long rubCents[2], long long usdCents[2],
                                      long long tradeRubCents[2], long long tradeUsdCents[2])
{
    const long long kMaxSaneCents = 10000000000000LL; // 100 billion RUB/USD in cents
    for (int s = 0; s < 2; ++s) {
        long long garbageVehCents = FloatToCents(g_stateCurEcon[s].scalars.vehicles[3]);
        if (llabs(garbageVehCents) > kMaxSaneCents) {
            if (llabs(rubCents[s]) > kMaxSaneCents) rubCents[s] -= garbageVehCents;
            if (llabs(tradeRubCents[s]) > kMaxSaneCents) tradeRubCents[s] -= garbageVehCents;
            g_stateCurEcon[s].scalars.vehicles[3] = 0.0f;
        }
        for (size_t i = 0; i < g_stateHistEcon[s].size(); ++i) {
            long long histGarbage = FloatToCents(g_stateHistEcon[s][i].scalars.vehicles[3]);
            if (llabs(histGarbage) > kMaxSaneCents) {
                if (llabs(rubCents[s]) > kMaxSaneCents) rubCents[s] -= histGarbage;
                if (llabs(tradeRubCents[s]) > kMaxSaneCents) tradeRubCents[s] -= histGarbage;
                g_stateHistEcon[s][i].scalars.vehicles[3] = 0.0f;
            }
            SanitizeEconPeriodScalars(&g_stateHistEcon[s][i]);
        }
        SanitizeEconPeriodScalars(&g_stateCurEcon[s]);
        if (llabs(rubCents[s]) > kMaxSaneCents) {
            int other = 1 - s;
            rubCents[s] = (rubCents[other] > 0 && rubCents[other] <= kMaxSaneCents) ? rubCents[other] : 1000000000LL;
        }
        if (llabs(usdCents[s]) > kMaxSaneCents) {
            int other = 1 - s;
            usdCents[s] = (usdCents[other] > 0 && usdCents[other] <= kMaxSaneCents) ? usdCents[other] : 200000000LL;
        }
        if (llabs(tradeRubCents[s]) > kMaxSaneCents) tradeRubCents[s] = 0;
        if (llabs(tradeUsdCents[s]) > kMaxSaneCents) tradeUsdCents[s] = 0;
        if (!isfinite(g_stateRub[s]) || fabs(g_stateRub[s]) > 1.0e11) g_stateRub[s] = (double)rubCents[s] / 100.0;
        if (!isfinite(g_stateUsd[s]) || fabs(g_stateUsd[s]) > 1.0e11) g_stateUsd[s] = (double)usdCents[s] / 100.0;
    }
}

static void HealLoadedCoopMetaTradeStats(bool hadPoisonedZero, long long rubCents[2], long long usdCents[2],
                                         long long tradeRubCents[2], long long tradeUsdCents[2])
{
    HealCorruptedSaveBalances(rubCents, usdCents, tradeRubCents, tradeUsdCents);
    if (!g_stateEconInitialized) return;
    if (hadPoisonedZero) {
        for (int s = 0; s < 2; ++s) {
            int other = 1 - s;
            bool sZeroTrade = (tradeRubCents[s] == 0 && tradeUsdCents[s] == 0);
            bool otherHasTrade = (tradeRubCents[other] != 0 || tradeUsdCents[other] != 0);
            if (sZeroTrade && otherHasTrade) {
                MergePeriodTradeResVecsIntoState(&g_stateCurEcon[other], &g_stateCurEcon[s]);
                size_t n = std::min(g_stateHistEcon[s].size(), g_stateHistEcon[other].size());
                for (size_t i = 0; i < n; ++i) {
                    MergePeriodTradeResVecsIntoState(&g_stateHistEcon[other][i], &g_stateHistEcon[s][i]);
                }
                return;
            }
        }
    }
    if (tradeRubCents[0] == 0 && tradeRubCents[1] == 0 && tradeUsdCents[0] == 0 && tradeUsdCents[1] == 0 &&
        (hadPoisonedZero || (rubCents[0] == 1000000000LL && rubCents[1] == 1000000000LL))) {
        for (int s = 0; s < 2; ++s) {
            ClearPeriodTradeResVecs(&g_stateCurEcon[s]);
            for (size_t i = 0; i < g_stateHistEcon[s].size(); ++i) {
                ClearPeriodTradeResVecs(&g_stateHistEcon[s][i]);
            }
        }
    }
    TwoStates_BumpHistSwapGen();
}

static bool ParseCmVector3(const std::string& val, float out[3])
{
    if (val.empty() || !out) return false;
    const char* p = val.c_str();
    char* endp = nullptr;

    while (*p == ' ' || *p == '\t') ++p;
    long long cx = _strtoi64(p, &endp, 10);
    if (!endp || endp == p || *endp != ',') return false;

    p = endp + 1;
    while (*p == ' ' || *p == '\t') ++p;
    long long cy = _strtoi64(p, &endp, 10);
    if (!endp || endp == p || *endp != ',') return false;

    p = endp + 1;
    while (*p == ' ' || *p == '\t') ++p;
    long long cz = _strtoi64(p, &endp, 10);
    if (!endp || endp == p) return false;

    while (*endp == ' ' || *endp == '\t' || *endp == '\r' || *endp == '\n') {
        ++endp;
    }
    if (*endp != '\0') return false;

    out[0] = (float)((double)cx / 100.0);
    out[1] = (float)((double)cy / 100.0);
    out[2] = (float)((double)cz / 100.0);
    return true;
}

struct CoopCamParseState {
    float pos1[3]  = { 0.0f, 0.0f, 0.0f };
    float look1[3] = { 0.0f, 0.0f, 0.0f };
    float pos2[3]  = { 0.0f, 0.0f, 0.0f };
    float look2[3] = { 0.0f, 0.0f, 0.0f };
    bool havePos1  = false;
    bool haveLook1 = false;
    bool havePos2  = false;
    bool haveLook2 = false;
};

static bool ParseCoopMetaCameraKey(const std::string& key, const std::string& val, CoopCamParseState& st)
{
    if (key == "cam1_pos_cm") {
        st.havePos1 = ParseCmVector3(val, st.pos1);
        return true;
    }
    if (key == "cam1_look_cm") {
        st.haveLook1 = ParseCmVector3(val, st.look1);
        return true;
    }
    if (key == "cam2_pos_cm") {
        st.havePos2 = ParseCmVector3(val, st.pos2);
        return true;
    }
    if (key == "cam2_look_cm") {
        st.haveLook2 = ParseCmVector3(val, st.look2);
        return true;
    }
    return false;
}

static bool ParseCoopMetaResearchKey(const std::string& key, const std::string& val, bool loadedR1, bool loadedR2)
{
    if (key == "state1_research_unlocked") {
        if (!loadedR1) {
            g_stateResearch[0].unlockedMask = (uint32_t)_strtoui64(val.c_str(), nullptr, 10);
            g_stateResearch[0].researchedCount = 0;
            for (int t = 0; t < 32; ++t) {
                if (g_stateResearch[0].unlockedMask & (1u << t)) {
                    g_stateResearch[0].techProgress[t] = 1.0f;
                    g_stateResearch[0].researchedCount++;
                }
            }
        }
        return true;
    }
    if (key == "state2_research_unlocked") {
        if (!loadedR2) {
            g_stateResearch[1].unlockedMask = (uint32_t)_strtoui64(val.c_str(), nullptr, 10);
            g_stateResearch[1].researchedCount = 0;
            for (int t = 0; t < 32; ++t) {
                if (g_stateResearch[1].unlockedMask & (1u << t)) {
                    g_stateResearch[1].techProgress[t] = 1.0f;
                    g_stateResearch[1].researchedCount++;
                }
            }
        }
        return true;
    }
    if (key == "state1_research_active") {
        if (!loadedR1) g_stateResearch[0].activeTechId = atoi(val.c_str());
        return true;
    }
    if (key == "state2_research_active") {
        if (!loadedR2) g_stateResearch[1].activeTechId = atoi(val.c_str());
        return true;
    }
    if (key == "state1_research_progress") {
        if (!loadedR1) {
            long long bp = _strtoi64(val.c_str(), nullptr, 10);
            float p = (float)((double)bp / 10000.0);
            g_stateResearch[0].activeTechProgress = p;
            if (g_stateResearch[0].activeTechId > 0 && g_stateResearch[0].activeTechId < 32) {
                g_stateResearch[0].techProgress[g_stateResearch[0].activeTechId] = p;
            }
        }
        return true;
    }
    if (key == "state2_research_progress") {
        if (!loadedR2) {
            long long bp = _strtoi64(val.c_str(), nullptr, 10);
            float p = (float)((double)bp / 10000.0);
            g_stateResearch[1].activeTechProgress = p;
            if (g_stateResearch[1].activeTechId > 0 && g_stateResearch[1].activeTechId < 32) {
                g_stateResearch[1].techProgress[g_stateResearch[1].activeTechId] = p;
            }
        }
        return true;
    }
    if (key == "engine_research_unlocks1") {
        ParseEngineResearchUnlocks(val, &g_engineResearch[0]);
        return true;
    }
    if (key == "engine_research_unlocks2") {
        ParseEngineResearchUnlocks(val, &g_engineResearch[1]);
        return true;
    }
    if (key == "engine_research_items1") {
        ParseEngineResearchItems(val, 1, loadedR1, &g_engineResearch[0]);
        return true;
    }
    if (key == "engine_research_items2") {
        ParseEngineResearchItems(val, 2, loadedR2, &g_engineResearch[1]);
        return true;
    }
    if (key == "veh_licenses1") {
        ParseEngineVehLicenses(val, &g_engineVehLicenses[0]);
        return true;
    }
    if (key == "veh_licenses2") {
        ParseEngineVehLicenses(val, &g_engineVehLicenses[1]);
        return true;
    }
    return false;
}

static void FinalizeSavedCameraMeta(const CoopCamParseState& st)
{
    if (st.havePos1 && st.haveLook1 && IsValidCameraPair(st.pos1, st.look1)) {
        g_hasSavedCam1 = true;
        memcpy(g_savedCam1Pos, st.pos1, sizeof(g_savedCam1Pos));
        memcpy(g_savedCam1Look, st.look1, sizeof(g_savedCam1Look));
    } else {
        g_hasSavedCam1 = false;
        memset(g_savedCam1Pos, 0, sizeof(g_savedCam1Pos));
        memset(g_savedCam1Look, 0, sizeof(g_savedCam1Look));
    }

    if (st.havePos2 && st.haveLook2 && IsValidCameraPair(st.pos2, st.look2)) {
        g_hasSavedCam2 = true;
        memcpy(g_savedCam2Pos, st.pos2, sizeof(g_savedCam2Pos));
        memcpy(g_savedCam2Look, st.look2, sizeof(g_savedCam2Look));
    } else {
        g_hasSavedCam2 = false;
        memset(g_savedCam2Pos, 0, sizeof(g_savedCam2Pos));
        memset(g_savedCam2Look, 0, sizeof(g_savedCam2Look));
    }
}

bool TwoStates_GetSavedCameraForTest(int state, float outPos[3], float outLook[3])
{
    if (state == 1 && g_hasSavedCam1) {
        if (outPos) memcpy(outPos, g_savedCam1Pos, sizeof(g_savedCam1Pos));
        if (outLook) memcpy(outLook, g_savedCam1Look, sizeof(g_savedCam1Look));
        return true;
    }
    if (state == 2 && g_hasSavedCam2) {
        if (outPos) memcpy(outPos, g_savedCam2Pos, sizeof(g_savedCam2Pos));
        if (outLook) memcpy(outLook, g_savedCam2Look, sizeof(g_savedCam2Look));
        return true;
    }
    return false;
}

void TwoStates_SetSavedCameraForTest(int state, const float pos[3], const float look[3])
{
    if (!IsValidCameraPair(pos, look)) return;
    if (state == 1) {
        g_hasSavedCam1 = true;
        memcpy(g_savedCam1Pos, pos, sizeof(g_savedCam1Pos));
        memcpy(g_savedCam1Look, look, sizeof(g_savedCam1Look));
    } else if (state == 2) {
        g_hasSavedCam2 = true;
        memcpy(g_savedCam2Pos, pos, sizeof(g_savedCam2Pos));
        memcpy(g_savedCam2Look, look, sizeof(g_savedCam2Look));
    }
}

static void LoadOptionalBinaryResearchTrees(const char* fullSaveDir, bool* outLoadedR1, bool* outLoadedR2)
{
    *outLoadedR1 = false;
    *outLoadedR2 = false;
    const char* kNames[2] = { "research_state1.bin", "research_state2.bin" };
    bool* outs[2] = { outLoadedR1, outLoadedR2 };
    for (int i = 0; i < 2; ++i) {
        char path[MAX_PATH] = {};
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", fullSaveDir, kNames[i]);
        FILE* fp = nullptr;
        if (fopen_s(&fp, path, "rb") == 0 && fp) {
            if (fread(&g_stateResearch[i], 1, sizeof(StateResearchTree), fp) == sizeof(StateResearchTree)) {
                *outs[i] = true;
            }
            fclose(fp);
        }
    }
}

static void ApplyLoadedCoopBudgetsAndMode(int mode, int bw, int expB, bool haveRub, bool haveUsd,
                                          const long long rubCents[2], const long long usdCents[2])
{
    g_chosenMode = (mode == COOP_MODE_TWO_STATES) ? COOP_MODE_TWO_STATES : COOP_MODE_ONE_STATE;
    g_sessionMode = g_chosenMode;
    if (bw >= 50 && bw <= 2000) TwoStates_SetBorderHalfWidth((float)bw);
    g_expandBorders = (expB != 0);
    if (haveRub) {
        g_stateRub[0] = (double)rubCents[0] / 100.0;
        g_stateRub[1] = (double)rubCents[1] / 100.0;
    }
    if (haveUsd) {
        g_stateUsd[0] = (double)usdCents[0] / 100.0;
        g_stateUsd[1] = (double)usdCents[1] / 100.0;
    }
    g_treasuryInitialized = (g_chosenMode == COOP_MODE_TWO_STATES && (haveRub || haveUsd));
    g_loadedFromCoopSave = true;

    if (g_chosenMode == COOP_MODE_TWO_STATES && (haveRub || haveUsd)) {
        if (IsUnlimitedMoney()) {
            g_stateRub[0] = g_stateRub[1] = 100000000.0;
            g_stateUsd[0] = g_stateUsd[1] = 100000000.0;
            SafeWriteWorldMoney(100000000.0, 100000000.0);
        } else {
            SafeWriteWorldMoney(g_stateRub[0] + g_stateRub[1], g_stateUsd[0] + g_stateUsd[1]);
        }
    }
}

static bool ReadCoopSaveMetaInternal(const char* fullSaveDir, bool updateBudgetsAndMode)
{
    ResetSavedCameraState();
    TwoStates_ResetResearch();
    if (!fullSaveDir || !fullSaveDir[0]) return false;

    bool loadedR1 = false;
    bool loadedR2 = false;
    LoadOptionalBinaryResearchTrees(fullSaveDir, &loadedR1, &loadedR2);

    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);

    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    int mode = (int)g_chosenMode;
    int bw = (int)lroundf(g_borderHalfWidth);
    int expB = g_expandBorders ? 1 : 0;
    long long rubCents[2] = { 0, 0 };
    long long usdCents[2] = { 0, 0 };
    long long tradeRubCents[2] = { 0, 0 };
    long long tradeUsdCents[2] = { 0, 0 };
    bool haveRub = false, haveUsd = false;
    bool haveEconCur[2] = { false, false };
    bool haveTrackerCur[2] = { false, false };
    bool haveTrackerOld[2] = { false, false };
    bool hadPoisonedZero = false;
    std::string curSection = "republicmp_coop";
    const BYTE* world = g_exeBase ? (g_exeBase + WORLD_RVA) : nullptr;

    ClearStateEconPeriod(&g_stateCurEcon[0], 0, 0);
    ClearStateEconPeriod(&g_stateCurEcon[1], 0, 0);
    g_stateHistEcon[0].clear();
    g_stateHistEcon[1].clear();
    TwoStates_BumpHistSwapGen();
    TwoStates_InvalidateHistBackfillCache();
    TwoStates_InvalidateUiCitizenCache();
    TwoStates_InvalidateUiHistBackupCache();

    CoopCamParseState camState;

    EnsureOwnerLock();
    EnterCriticalSection(&g_ownerLock);
    g_buildingOwners.clear();
    g_vehicleOwners.clear();

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            curSection = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (ParseCoopMetaTrackerKey(curSection, key, val, haveTrackerCur, haveTrackerOld)) { /* parsed tracker key */ }
        else if (key == "coop_mode") mode = atoi(val.c_str());
        else if (key == "border_half_width_m") bw = atoi(val.c_str());
        else if (key == "expand_borders") expB = atoi(val.c_str());
        else if (key == "state1_rub_cents") { rubCents[0] = _strtoi64(val.c_str(), nullptr, 10); haveRub = true; }
        else if (key == "state2_rub_cents") { rubCents[1] = _strtoi64(val.c_str(), nullptr, 10); haveRub = true; }
        else if (key == "state1_usd_cents") { usdCents[0] = _strtoi64(val.c_str(), nullptr, 10); haveUsd = true; }
        else if (key == "state2_usd_cents") { usdCents[1] = _strtoi64(val.c_str(), nullptr, 10); haveUsd = true; }
        else if (key == "net_trade1_rub_cents") tradeRubCents[0] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade2_rub_cents") tradeRubCents[1] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade1_usd_cents") tradeUsdCents[0] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "net_trade2_usd_cents") tradeUsdCents[1] = _strtoi64(val.c_str(), nullptr, 10);
        else if (key == "building_owners") ParseOwnersList(val, g_buildingOwners, 1, &hadPoisonedZero);
        else if (key == "vehicle_owners")  ParseOwnersList(val, g_vehicleOwners, 0, nullptr);
        else if (ParseCoopMetaCameraKey(key, val, camState)) { /* parsed camera key */ }
        else if (ParseCoopMetaResearchKey(key, val, loadedR1, loadedR2)) { /* parsed research key */ }
        else ParseCoopMetaEconKey(world, key, val, haveEconCur);
    }
    size_t bldCount = g_buildingOwners.size();
    size_t vehCount = g_vehicleOwners.size();
    TwoStates_BumpOwnerMapGen();
    TwoStates_BumpVehOwnerMapGen();
    LeaveCriticalSection(&g_ownerLock);

    FinalizeSavedCameraMeta(camState);

    g_stateEconInitialized = (haveEconCur[0] && haveEconCur[1]);
    if (haveTrackerCur[0] && haveTrackerCur[1]) {
        g_stateTrackerInitialized = true;
        if (world) {
            SafeReadTrackerSlice(world, false, &g_lastWorldTrackerCur);
            memset(&g_unattributedTrackerCur, 0, sizeof(g_unattributedTrackerCur));
        }
    } else {
        ResetStateTrackerSlices();
    }
    HealLoadedCoopMetaTradeStats(hadPoisonedZero, rubCents, usdCents, tradeRubCents, tradeUsdCents);
    if (g_stateEconInitialized && world) {
        SafeReadStatScalars(world + WORLD_STAT_CURRENT_OFFSET, &g_lastWorldScalars);
        memset(&g_unattributedScalars, 0, sizeof(g_unattributedScalars));
        EnsureStateEconInitialized(const_cast<BYTE*>(world));
    }
    if ((g_engineResearch[0].initialized || g_engineResearch[1].initialized) && world) {
        EnsureEngineResearchInitialized(const_cast<BYTE*>(world));
    }
    if ((g_engineVehLicenses[0].initialized || g_engineVehLicenses[1].initialized) && world) {
        EnsureEngineVehLicensesInitialized(const_cast<BYTE*>(world));
    }

    g_netTradeRub[0] = (double)tradeRubCents[0] / 100.0;
    g_netTradeRub[1] = (double)tradeRubCents[1] / 100.0;
    g_netTradeUsd[0] = (double)tradeUsdCents[0] / 100.0;
    g_netTradeUsd[1] = (double)tradeUsdCents[1] / 100.0;

    if (updateBudgetsAndMode) {
        ApplyLoadedCoopBudgetsAndMode(mode, bw, expB, haveRub, haveUsd, rubCents, usdCents);
    }

    Logf("republicmp  COOP_META: loaded [%s] (updateBudgets=%d, mode=%d, rub=[%.0f, %.0f], usd=[%.0f, %.0f], blds=%zu, vehs=%zu)",
         metaPath, updateBudgetsAndMode ? 1 : 0, g_inSession ? (int)g_sessionMode : (int)g_chosenMode,
         g_stateRub[0], g_stateRub[1], g_stateUsd[0], g_stateUsd[1], bldCount, vehCount);
    return true;
}

bool TwoStates_WriteSaveMetaToDir(const char* fullSaveDir)
{
    if (!fullSaveDir || !fullSaveDir[0]) return false;
    WriteCoopSaveMetaInternal(fullSaveDir);
    char metaPath[MAX_PATH] = {};
    _snprintf_s(metaPath, sizeof(metaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);
    return GetFileAttributesA(metaPath) != INVALID_FILE_ATTRIBUTES;
}

bool TwoStates_ReadSaveMetaFromDir(const char* fullSaveDir, bool updateBudgetsAndMode)
{
    return ReadCoopSaveMetaInternal(fullSaveDir, updateBudgetsAndMode);
}

void TwoStates_BeforeAnyWorldLoad()
{
    g_pendingCamRestore.active = false;
    MemoryBarrier();
    g_pendingCamRestore = {};
    RoadSync_ClearPlannedSegments();
    if (!g_exeBase) return;
    if (g_beforeLoadPrepared) return;
    g_beforeLoadPrepared = true;
    TwoStates_RestoreUiMoneySwap();
    ResetStateHudPopCache();
    TwoStates_InvalidateUiCitizenCache();
    TwoStates_InvalidateMountCaches();
    ResetStateTrackerSlices();
    ResetEngineResearchSlices();
    ResetEngineVehLicenseSlices();
    g_stateEconInitialized = false;
    ClearStateEconPeriod(&g_stateCurEcon[0], 0, 0);
    ClearStateEconPeriod(&g_stateCurEcon[1], 0, 0);
    g_stateHistEcon[0].clear();
    g_stateHistEcon[1].clear();
    TwoStates_BumpHistSwapGen();
    TwoStates_InvalidateHistBackfillCache();
    TwoStates_InvalidateUiCitizenCache();
    TwoStates_InvalidateUiHistBackupCache();

    bool isSessionLoad = Determinism_WorldLoading() || SaveSync_GetState() != SESSION_IDLE;
    bool wantCoopHost = (NativeMenu_IsHostOnNextLoad() || g_importingSpSave) && !isSessionLoad;

    BYTE terrainFlag = 0;
    char saveRel[128] = {};
    if (!SafeReadPendingLoadInfo(&terrainFlag, saveRel, sizeof(saveRel))) {
        return;
    }

    char fullDir[MAX_PATH] = {};
    bool isSaveRel = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
    bool hasFullDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullDir, sizeof(fullDir)));

    if (!wantCoopHost) {
        if (isSessionLoad && hasFullDir && g_expandBorders) {
            TwoStates_PatchSaveScriptIni(fullDir, 0.0f);
        }
        ModSync_BeforeWorldLoad(false, nullptr);
        return;
    }

    if (!TwoStates_IsCoopBankMounted()) {
        if (g_importingSpSave) {
            // User explicitly selected a Singleplayer save to import into Co-op!
            bool isSaveFolder = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
            if (terrainFlag == 0 && isSaveFolder) {
                char spFullDir[MAX_PATH] = {};
                char exeDir[MAX_PATH] = {};
                if (BuildFullSaveDir(saveRel, spFullDir, sizeof(spFullDir)) && GetExeDir(exeDir, sizeof(exeDir))) {
                    std::error_code ec;
                    std::filesystem::path srcPath(spFullDir);
                    std::string folderName = srcPath.filename().string();
                    std::filesystem::path coopCopy = std::filesystem::path(exeDir) / "media_soviet" / "save_coop" / folderName;
                    if (std::filesystem::exists(srcPath, ec) && std::filesystem::is_directory(srcPath, ec)) {
                        std::filesystem::create_directories(coopCopy.parent_path(), ec);
                        if (std::filesystem::exists(coopCopy, ec)) {
                            std::filesystem::remove_all(coopCopy, ec);
                        }
                        std::filesystem::copy(srcPath, coopCopy,
                                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                              ec);
                        // Remove any old Co-op metadata on a fresh SP import so menu capital settings apply cleanly
                        std::filesystem::remove(coopCopy / COOP_SAVE_META_NAME, ec);
                        std::filesystem::remove(coopCopy / "research_state1.bin", ec);
                        std::filesystem::remove(coopCopy / "research_state2.bin", ec);
                        Logf("republicmp  SAVE_BANK: imported SP save [%s] -> [%s]",
                             srcPath.string().c_str(), coopCopy.string().c_str());
                    } else {
                        // Check if it was a .zip save in Singleplayer
                        std::filesystem::path srcZip( std::string(spFullDir) + ".zip" );
                        std::filesystem::path coopZip = std::filesystem::path(exeDir) / "media_soviet" / "save_coop" / (folderName + ".zip");
                        if (std::filesystem::exists(srcZip, ec)) {
                            std::filesystem::create_directories(coopZip.parent_path(), ec);
                            std::filesystem::copy_file(srcZip, coopZip, std::filesystem::copy_options::overwrite_existing, ec);
                        }
                    }
                }
            }
        }
        TwoStates_MountSaveBank(true);
    }

    g_importingSpSave = false;

    hasFullDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullDir, sizeof(fullDir)));
    if (hasFullDir && g_expandBorders) {
        TwoStates_PatchSaveScriptIni(fullDir, 0.0f);
    }

    ModSync_BeforeWorldLoad(true, hasFullDir ? fullDir : nullptr);
}

static void RestoreOrFrameCoopCamera(bool isHost)
{
    bool twoStates = (g_inSession ? g_sessionMode : g_chosenMode) == COOP_MODE_TWO_STATES;
    float targetPos[3] = {};
    float targetLook[3] = {};
    bool hasTarget = false;

    if (g_hasResyncCam) {
        memcpy(targetPos, g_resyncCamPos, sizeof(targetPos));
        memcpy(targetLook, g_resyncCamLook, sizeof(targetLook));
        hasTarget = true;
        g_hasResyncCam = false;
        TwoStates_ApplyCameraView(targetPos, targetLook);
        Logf("republicmp  CAMERA: restored pre-resync view (pos=[%.1f, %.1f, %.1f], look=[%.1f, %.1f, %.1f])",
             targetPos[0], targetPos[1], targetPos[2],
             targetLook[0], targetLook[1], targetLook[2]);
    } else if (isHost && g_hasSavedCam1) {
        memcpy(targetPos, g_savedCam1Pos, sizeof(targetPos));
        memcpy(targetLook, g_savedCam1Look, sizeof(targetLook));
        hasTarget = true;
        TwoStates_ApplyCameraView(targetPos, targetLook);
        Logf("republicmp  CAMERA: restored saved Host view (pos=[%.1f, %.1f, %.1f], look=[%.1f, %.1f, %.1f])",
             targetPos[0], targetPos[1], targetPos[2],
             targetLook[0], targetLook[1], targetLook[2]);
    } else if (!isHost && g_hasSavedCam2) {
        memcpy(targetPos, g_savedCam2Pos, sizeof(targetPos));
        memcpy(targetLook, g_savedCam2Look, sizeof(targetLook));
        hasTarget = true;
        TwoStates_ApplyCameraView(targetPos, targetLook);
        Logf("republicmp  CAMERA: restored saved Client view (pos=[%.1f, %.1f, %.1f], look=[%.1f, %.1f, %.1f])",
             targetPos[0], targetPos[1], targetPos[2],
             targetLook[0], targetLook[1], targetLook[2]);
    } else {
        TwoStates_ApplyInitialFraming(isHost, twoStates, targetPos, targetLook);
        hasTarget = true;
        Logf("republicmp  CAMERA: applied initial %s framing (mode=%s)",
             isHost ? "Host (West)" : "Client (East)", twoStates ? "TwoStates" : "OneState");
    }

    if (hasTarget) {
        memcpy(g_pendingCamRestore.pos, targetPos, sizeof(targetPos));
        memcpy(g_pendingCamRestore.look, targetLook, sizeof(targetLook));
        g_pendingCamRestore.framesRemaining = 10;
        g_pendingCamRestore.checkFrameCounter = 0;
        MemoryBarrier();
        g_pendingCamRestore.active = true;
    }
}

void TwoStates_RestoreOrFrameCameraForTest(bool isHost)
{
    RestoreOrFrameCoopCamera(isHost);
}

void TwoStates_OnAnyWorldLoaded(bool isLockstepSessionLoad)
{
    g_beforeLoadPrepared = false;
    TwoStates_InvalidateUiCitizenCache();
    TwoStates_InvalidateMountCaches();
    if (!g_exeBase) return;
    SanitizeAllWorldBuildingsEffectiveCap(g_exeBase + WORLD_RVA);
    BYTE terrainFlag = 0;
    char saveRel[128] = {};
    if (!SafeReadPendingLoadInfo(&terrainFlag, saveRel, sizeof(saveRel))) {
        return;
    }

    char fullSaveDir[MAX_PATH] = {};
    bool isSaveRel = (_strnicmp(saveRel, "save\\", 5) == 0 || _strnicmp(saveRel, "save/", 5) == 0);
    bool hasSaveDir = (terrainFlag == 0 && isSaveRel && BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir)));

    if (isLockstepSessionLoad) {
        if (hasSaveDir) {
            ReadCoopSaveMetaInternal(fullSaveDir, false);
        }
        TwoStates_OnWorldLoaded();
        RestoreOrFrameCoopCamera(g_isHost);
        return;
    }

    char coopMetaPath[MAX_PATH] = {};
    if (hasSaveDir) {
        _snprintf_s(coopMetaPath, sizeof(coopMetaPath), _TRUNCATE, "%s\\%s", fullSaveDir, COOP_SAVE_META_NAME);
    }
    const bool coopMetaExists = hasSaveDir && (GetFileAttributesA(coopMetaPath) != INVALID_FILE_ATTRIBUTES);

    const bool inCoop = g_inSession || NativeMenu_IsHostOnNextLoad() || SaveSync_IsBusy();
    if ((inCoop || coopMetaExists) && hasSaveDir && ReadCoopSaveMetaInternal(fullSaveDir, true)) {
        g_haveLobbyBaseMoney = false;
        g_loadedFromCoopSave = true;
        if (g_chosenMode == COOP_MODE_TWO_STATES) {
            g_treasuryInitialized = true;
            EnsureStateEconInitialized(g_exeBase + WORLD_RVA);
            EnsureEngineResearchInitialized(g_exeBase + WORLD_RVA);
            EnsureEngineVehLicensesInitialized(g_exeBase + WORLD_RVA);
            char msg[240];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        Tr("Загружен кооп-сейв «2 государства» (Запад: %.0f РУБ / %.0f $, Восток: %.0f РУБ / %.0f $)",
                           "Loaded 'Two States' Co-op save (West: %.0f RUB / %.0f $, East: %.0f RUB / %.0f $)"),
                        g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
            TwoStates_AddToast(msg, 1);
        } else {
            TwoStates_AddToast(Tr("Загружен кооп-сейв «Одно государство»",
                                  "Loaded 'One State' Co-op save"), 1);
        }
        RestoreOrFrameCoopCamera(g_isHost);
    } else {
        TwoStates_ResetResearch();
        g_loadedFromCoopSave = false;
        g_treasuryInitialized = false;
        g_haveLobbyBaseMoney = false;
        DiagLog_BeginSafeSeh();
        __try {
            g_lobbyBaseWorldRub = *WorldRubles();
            g_lobbyBaseWorldUsd = *WorldDollars();
            g_haveLobbyBaseMoney = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_haveLobbyBaseMoney = false;
        }
        DiagLog_EndSafeSeh();
        TwoStates_ClearEntityOwners();
        g_netTradeRub[0] = g_netTradeRub[1] = 0.0;
        g_netTradeUsd[0] = g_netTradeUsd[1] = 0.0;
        ResetStateHudPopCache();
        RoadSync_ClearPlannedSegments();
        ResetStateTrackerSlices();
        g_stateEconInitialized = false;
        ClearStateEconPeriod(&g_stateCurEcon[0], 0, 0);
        ClearStateEconPeriod(&g_stateCurEcon[1], 0, 0);
        g_stateHistEcon[0].clear();
        g_stateHistEcon[1].clear();
        TwoStates_BumpHistSwapGen();
        TwoStates_InvalidateUiHistBackupCache();
        if (inCoop) {
            RestoreOrFrameCoopCamera(g_isHost);
        }
    }
}

bool TwoStates_TriggerQuickCoopSave()
{
    if (!SaveSync_IsWorldLoaded()) return false;
    TwoStates_MountSaveBank(true);

    SYSTEMTIME st;
    GetLocalTime(&st);
    int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
    char saveName[96] = {};
    _snprintf_s(saveName, sizeof(saveName), _TRUNCATE,
                "coop_%s_%04d-%02d-%02d_%02d-%02d-%02d",
                (mode == COOP_MODE_TWO_STATES) ? "2states" : "1state",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char fullSaveDir[MAX_PATH] = {};
    char saveRel[128] = {};
    _snprintf_s(saveRel, sizeof(saveRel), _TRUNCATE, "save\\%s", saveName);
    if (BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir))) {
        CreateDirectoryA(fullSaveDir, nullptr);
    }

    return TriggerSaveGame(saveName);
}

// ---------------------------------------------------------------------------
// Feature 32: Deterministic Co-op Autosaves & Client Backup
// ---------------------------------------------------------------------------

bool TwoStates_IsAutosaveTick(unsigned long long stepCount, unsigned long long intervalTicks)
{
    if (stepCount == 0 || intervalTicks == 0) return false;
    return (stepCount % intervalTicks) == 0;
}

const char* TwoStates_GetAutosaveRelPath(bool isHost)
{
    return isHost ? "save/coop_autosave" : "save/coop_client_backup";
}

// ---------------------------------------------------------------------------
// Vehicle Save Snapshot & MasterSave Hook
// ---------------------------------------------------------------------------

struct VehSaveSnapshot {
    BYTE* veh;
    unsigned long long f17E8;
    unsigned long long f1A54;
    unsigned long long f1A7C;
};
static std::vector<VehSaveSnapshot> g_vehSaveSnaps;

static void SnapshotOneVehicleBeforeSave(BYTE* v)
{
    if (!v) return;
    VehSaveSnapshot s;
    s.veh   = v;
    s.f17E8 = *(unsigned long long*)(v + 0x1880);
    s.f1A54 = *(unsigned long long*)(v + 0x1BAC);
    s.f1A7C = *(unsigned long long*)(v + 0x1BD4);
    g_vehSaveSnaps.push_back(s);
}

static void SnapshotVehiclesBeforeSave()
{
    g_vehSaveSnaps.clear();
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** vb = *(BYTE***)(g_exeBase + VEC_VEHICLES_BEGIN_RVA);
        BYTE** ve = *(BYTE***)(g_exeBase + VEC_VEHICLES_END_RVA);
        if (vb && ve > vb && (ve - vb) <= 65536) {
            g_vehSaveSnaps.reserve((size_t)(ve - vb));
            for (BYTE** it = vb; it < ve; ++it) {
                BYTE* v = *it;
                if (!v) continue;
                SnapshotOneVehicleBeforeSave(v);
                BYTE** cBegin = *(BYTE***)(v + 0x3A0);
                BYTE** cEnd = *(BYTE***)(v + 0x3A8);
                if (cBegin && cEnd > cBegin && (cEnd - cBegin) <= 1024) {
                    for (BYTE** cit = cBegin; cit < cEnd; ++cit) {
                        if (*cit) SnapshotOneVehicleBeforeSave(*cit);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
}

static void RestoreVehiclesAfterSave()
{
    DiagLog_BeginSafeSeh();
    __try {
        for (size_t i = 0; i < g_vehSaveSnaps.size(); ++i) {
            const VehSaveSnapshot& s = g_vehSaveSnaps[i];
            *(unsigned long long*)(s.veh + 0x1880) = s.f17E8;
            *(unsigned long long*)(s.veh + 0x1BAC) = s.f1A54;
            *(unsigned long long*)(s.veh + 0x1BD4) = s.f1A7C;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();
    g_vehSaveSnaps.clear();
}

static void __fastcall H_MasterSave(BYTE* world)
{
    InterlockedIncrement(&g_saveInProgress);
    TwoStates_RestoreUiMoneySwap();

    bool isCoopSave = g_inSession || NativeMenu_IsHostOnNextLoad() || TwoStates_IsCoopBankMounted();
    bool twoStates  = TwoStates_IsActive() ||
                      (isCoopSave && g_treasuryInitialized &&
                       (g_inSession ? g_sessionMode : g_chosenMode) == COOP_MODE_TWO_STATES);

    char saveRel[128] = {};
    char fullSaveDir[MAX_PATH] = {};
    double prevRub = 0.0, prevUsd = 0.0;
    bool moneySwapped = false;
    BYTE prevFc90 = 0;
    bool fc90Patched = false;
    BYTE* autosaveJeAddr = g_exeBase ? (g_exeBase + RVA_MASTER_SAVE_AUTOSAVE_JE) : nullptr;
    BYTE prevAutosaveJe = 0x74;
    DWORD oldProt = 0;
    bool autosaveJePatched = false;

    BYTE searchPoolBackup[72] = {};
    bool searchPoolSaved = false;

    SnapshotVehiclesBeforeSave();

    DiagLog_BeginSafeSeh();
    __try {
        if (world) {
            memcpy(searchPoolBackup, world + 0x14530, sizeof(searchPoolBackup));
            searchPoolSaved = true;
            strncpy_s(saveRel, (const char*)(world + SAVE_NAME_FIELD_OFFSET), _TRUNCATE);
            bool isManualIntoStaging = (strstr(saveRel, "mp_syncsave_") != nullptr) &&
                                       SaveSync_GetState() != SESSION_HOST_JOINING &&
                                       SaveSync_GetState() != SESSION_CLIENT_JOINING;
            if (isCoopSave && (_stricmp(saveRel, "saved_last") == 0 || isManualIntoStaging)) {
                // Redirect engine's automatic save-on-exit ("saved_last") or manual save overwriting
                // a staging "mp_syncsave_*" name into "save/coop_last_session" so user saves are never
                // lost to staging cleanup and always appear in "Continue Co-op" / auto-host.
                strncpy_s((char*)(world + SAVE_NAME_FIELD_OFFSET), SAVE_NAME_FIELD_MAX_LEN, "save/coop_last_session", _TRUNCATE);
                strncpy_s(saveRel, "save/coop_last_session", _TRUNCATE);
                prevFc90 = *(BYTE*)(world + 0xFD30);
                *(BYTE*)(world + 0xFD30) = 1;
                fc90Patched = true;
            }
            if (saveRel[0] && BuildFullSaveDir(saveRel, fullSaveDir, sizeof(fullSaveDir))) {
                CreateDirectoryA(fullSaveDir, nullptr);
                char scriptPath[MAX_PATH] = {};
                _snprintf_s(scriptPath, sizeof(scriptPath), _TRUNCATE, "%s\\script.ini", fullSaveDir);
                if (!fc90Patched && GetFileAttributesA(scriptPath) == INVALID_FILE_ATTRIBUTES) {
                    prevFc90 = *(BYTE*)(world + 0xFD30);
                    *(BYTE*)(world + 0xFD30) = 1; // force C3D_TERRAIN::Save to write script.ini & terrain files
                    fc90Patched = true;
                }
            }
            if (isCoopSave) {
                *(BYTE*)(world + SAVE_ZIP_AFTER_FLAG_OFFSET) = 0;
                if (autosaveJeAddr && *autosaveJeAddr == 0x74) {
                    if (VirtualProtect(autosaveJeAddr, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                        prevAutosaveJe = *autosaveJeAddr;
                        *autosaveJeAddr = 0xEB; // always skip AUTOSAVE_TO_ZIP so Co-op saves remain folders
                        autosaveJePatched = true;
                    }
                }
            }
            if (twoStates && !IsUnlimitedMoney()) {
                prevRub = *WorldRubles();
                prevUsd = *WorldDollars();
                *WorldRubles()  = g_stateRub[0] + g_stateRub[1];
                *WorldDollars() = g_stateUsd[0] + g_stateUsd[1];
                moneySwapped = true;
            }
            if (twoStates) {
                TwoStates_InvalidateMountCaches();
                EnsureEngineResearchInitialized(world);
                EnsureEngineVehLicensesInitialized(world);
                int localSt = TwoStates_LocalState();
                if (localSt != 1 && localSt != 2) localSt = 1;
                CaptureWorldResearchToSlice(world, &g_engineResearch[localSt - 1], localSt);
                CaptureWorldVehLicensesToSlice(world, &g_engineVehLicenses[localSt - 1]);
                WriteSliceToWorldResearch(world, &g_engineResearch[0]);
                WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[0]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();

    if (o_masterSave) {
        Determinism_ToolScopeBegin();
        o_masterSave(world);
        Determinism_ToolScopeEnd();
    }

    RestoreVehiclesAfterSave();

    DiagLog_BeginSafeSeh();
    __try {
        if (searchPoolSaved && world) {
            memcpy(world + 0x14530, searchPoolBackup, sizeof(searchPoolBackup));
        }
        if (autosaveJePatched && autosaveJeAddr) {
            *autosaveJeAddr = prevAutosaveJe;
            VirtualProtect(autosaveJeAddr, 2, oldProt, &oldProt);
        }
        if (fc90Patched && world) {
            *(BYTE*)(world + 0xFD30) = prevFc90;
        }
        if (moneySwapped) {
            *WorldRubles()  = prevRub;
            *WorldDollars() = prevUsd;
        }
        if (twoStates && world) {
            TwoStates_InvalidateMountCaches();
            int localSt = TwoStates_LocalState();
            if (localSt != 1 && localSt != 2) localSt = 1;
            WriteSliceToWorldResearch(world, &g_engineResearch[localSt - 1]);
            WriteSliceToWorldVehLicenses(world, &g_engineVehLicenses[localSt - 1]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DiagLog_EndSafeSeh();

    if (isCoopSave && fullSaveDir[0]) {
        if (twoStates && world) {
            EnsureStateEconInitialized(world);
            FlushUnattributedScalarsToStates(world);
            ComputeBothStatesCitizenDemographics(world, &g_stateCurEcon[0].scalars, &g_stateCurEcon[1].scalars);
        }
        if (g_expandBorders) {
            TwoStates_PatchSaveScriptIni(fullSaveDir, 0.0f);
        }
        WriteCoopSaveMetaInternal(fullSaveDir);
        if (strstr(saveRel, "mp_syncsave_") == nullptr) {
            int mode = g_inSession ? (int)g_sessionMode : (int)g_chosenMode;
            if (mode == COOP_MODE_TWO_STATES) {
                char msg[240];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                            Tr("Кооп-сохранение записано (Запад: %.0f РУБ / %.0f $, Восток: %.0f РУБ / %.0f $)",
                               "Co-op save written (West: %.0f RUB / %.0f $, East: %.0f RUB / %.0f $)"),
                            g_stateRub[0], g_stateUsd[0], g_stateRub[1], g_stateUsd[1]);
                TwoStates_AddToast(msg, 1);
            } else {
                TwoStates_AddToast(Tr("Кооп-сохранение успешно записано", "Co-op save written"), 1);
            }
        }
    }

    InterlockedIncrement64(&g_completedSaveCount);
    InterlockedDecrement(&g_saveInProgress);
}

bool TwoStates_CheckAndTriggerAutosave(BYTE* world, unsigned long long stepCount, bool isHost)
{
    if (!world || !TwoStates_IsAutosaveTick(stepCount)) return false;
    if (!Determinism_SessionRunning() && !TwoStates_IsActive()) return false;
    if (!o_masterSave) return false;

    const char* targetRel = TwoStates_GetAutosaveRelPath(isHost);
    char fullSaveDir[MAX_PATH] = {};
    if (BuildFullSaveDir(targetRel, fullSaveDir, sizeof(fullSaveDir))) {
        CreateDirectoryA(fullSaveDir, nullptr);
    }

    strncpy_s((char*)(world + SAVE_NAME_FIELD_OFFSET), SAVE_NAME_FIELD_MAX_LEN, targetRel, _TRUNCATE);
    H_MasterSave(world);

    Logf("republicmp  TWO_STATES: deterministic %s autosave completed at tick %llu to [%s]",
         isHost ? "host" : "client", stepCount, targetRel);
    return true;
}

void TwoStates_MasterSaveForTest(BYTE* world)
{
    H_MasterSave(world);
}

int TwoStates_InstallPersistenceHooks()
{
    if (!g_exeBase) return 0;
    return InstallInlineHook(g_exeBase + RVA_MASTER_SAVE, (void*)&H_MasterSave, (void**)&o_masterSave,
                             kMasterSaveExpect, sizeof(kMasterSaveExpect), "republicmp two_states master_save");
}
