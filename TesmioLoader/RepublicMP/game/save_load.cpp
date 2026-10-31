// SPDX-License-Identifier: GPL-3.0-only
// See save_load.h.
#include "save_load.h"
#include "../determinism/determinism.h"
#include "../session/two_states.h"
#include <string.h>

void SaveLoad_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static char* GetSaveNamePtr()
{
    return (char*)((uintptr_t)g_exe + SAVE_NAME_DAT_RVA);
}

static volatile char* GetLoadTriggerPtr()
{
    return (volatile char*)((uintptr_t)g_exe + LOAD_TRIGGER_DAT_RVA);
}

static volatile char* GetTerrainFlagPtr()
{
    return (volatile char*)((uintptr_t)g_exe + TERRAIN_FLAG_DAT_RVA);
}

static volatile char* GetReturnMenuPtr()
{
    return (volatile char*)((uintptr_t)g_exe + RETURN_MENU_DAT_RVA);
}

void TriggerLoadSave(const char* saveName)
{
    char fullName[SAVE_NAME_MAX_LEN];
    _snprintf_s(fullName, sizeof(fullName), _TRUNCATE, "%s%s", SAVE_FOLDER_PREFIX, saveName);

    char* dst = GetSaveNamePtr();
    strncpy_s(dst, SAVE_NAME_MAX_LEN, fullName, _TRUNCATE);

    *GetTerrainFlagPtr() = 0;  // must be 0, otherwise the name above gets overwritten with a hardcoded terrain
    *GetReturnMenuPtr() = 0;

    Logf("republicmp  SAVE_LOAD: triggering load of [%s]", fullName);
    *GetLoadTriggerPtr() = 1;
}

bool WaitForLoadToFinish(DWORD timeoutMs, volatile bool* cancelFlag, bool (*abortCheck)(void*), void* abortCtx)
{
    volatile char* trigger = GetLoadTriggerPtr();
    const DWORD stepMs = 50;
    bool aborted = false;

    if (timeoutMs == 0) {
        // No cap - wait until load completes or abort check triggered and engine load finishes.
        for (;;) {
            if (*trigger == 0 && !Determinism_WorldLoading()) return !aborted;
            if (!aborted && ((cancelFlag && *cancelFlag) || (abortCheck && abortCheck(abortCtx)))) {
                Logf("republicmp  SAVE_LOAD: WaitForLoadToFinish aborted/cancelled; waiting for engine load cycle to safely complete...");
                aborted = true;
            }
            Sleep(stepMs);
        }
    }

    for (DWORD waited = 0; waited < timeoutMs; waited += stepMs) {
        if (*trigger == 0 && !Determinism_WorldLoading()) return !aborted;
        if (!aborted && ((cancelFlag && *cancelFlag) || (abortCheck && abortCheck(abortCtx)))) {
            Logf("republicmp  SAVE_LOAD: WaitForLoadToFinish aborted/cancelled; waiting for engine load cycle to safely complete...");
            aborted = true;
        }
        Sleep(stepMs);
    }
    return false;
}

static char* GetWorldStatePtr()
{
    return (char*)((uintptr_t)g_exe + WORLD_THIS_RVA);
}

bool TriggerSaveGame(const char* saveName)
{
    char* base = GetWorldStatePtr();

    char fullName[SAVE_NAME_FIELD_MAX_LEN];
    _snprintf_s(fullName, sizeof(fullName), _TRUNCATE, "%s%s", SAVE_NAME_FIELD_PREFIX, saveName);

    char* nameDst = base + SAVE_NAME_FIELD_OFFSET;
    strncpy_s(nameDst, SAVE_NAME_FIELD_MAX_LEN, fullName, _TRUNCATE);

    // Skip the engine's own 7z step (we zip ourselves) and fire next frame.
    *(volatile BYTE*)(base + SAVE_ZIP_AFTER_FLAG_OFFSET) = 0;
    *(volatile float*)(base + SAVE_DELAY_COUNTER_OFFSET) = 0.0f;

    Logf("republicmp  SAVE_LOAD: triggering save of [%s]", fullName);
    *(volatile BYTE*)(base + SAVE_TRIGGER_FLAG_OFFSET) = 1;
    return true;
}

bool WaitForSaveToFinish(DWORD timeoutMs, volatile bool* cancelFlag)
{
    volatile BYTE* saveTrigger = (volatile BYTE*)(GetWorldStatePtr() + SAVE_TRIGGER_FLAG_OFFSET);
    const DWORD stepMs = 100;

    if (timeoutMs == 0) {
        // No timeout: the save writes dozens of files.
        for (;;) {
            if (*saveTrigger == 0 && !TwoStates_IsSaveInProgress()) return true; // dispatcher (FUN_14030d1a0) cleared it - FUN_14042cc70 is done
            if (cancelFlag && *cancelFlag) {
                Logf("republicmp  SAVE_LOAD: WaitForSaveToFinish cancelled");
                return false;
            }
            Sleep(stepMs);
        }
    }

    for (DWORD waited = 0; waited < timeoutMs; waited += stepMs) {
        if (*saveTrigger == 0 && !TwoStates_IsSaveInProgress()) return true;
        if (cancelFlag && *cancelFlag) {
            Logf("republicmp  SAVE_LOAD: WaitForSaveToFinish cancelled");
            return false;
        }
        Sleep(stepMs);
    }
    return false;
}
