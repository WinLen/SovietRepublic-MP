// SPDX-License-Identifier: GPL-3.0-only
// Shared Mock Engine Harness implementation for RepublicMP unit & integration tests.

#include "test_harness.h"
#include <stdarg.h>

int g_totalTests = 0;
int g_passedTests = 0;
int g_failedAssertions = 0;

BYTE* g_mockExeBuffer = nullptr;
char  g_mockBaseDir[MAX_PATH] = {};
TsmHost g_mockHost = {};

void MockHost_Log(const char* fmt, ...)
{
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void** MockHost_FindIatSlot(void*, const char*, const char*)
{
    return nullptr;
}

int MockHost_PatchIat(void*, const char*, const char*, void*, void**, const char*)
{
    return 0;
}

int MockHost_InstallInlineHook(void*, void*, void**, const unsigned char*, size_t, const char*)
{
    return 0;
}

unsigned char* MockHost_AllocNear(unsigned char*, size_t)
{
    return nullptr;
}

int MockHost_ReadablePtr(const void* p, size_t n)
{
    if (!p || n == 0) return 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    uintptr_t endRegion = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return ((uintptr_t)p + n <= endRegion) ? 1 : 0;
}

long MockHost_FaultFilter(const char*, void*)
{
    return EXCEPTION_EXECUTE_HANDLER;
}

const void* MockHost_Consume(const char*, unsigned)
{
    return nullptr;
}

void InitMockHarness()
{
    SetEnvironmentVariableA("REPUBLICMP_DISABLE_CRASH_UPLOAD", "1");
    if (!g_mockExeBuffer) {
        g_mockExeBuffer = (BYTE*)VirtualAlloc(nullptr, MOCK_EXE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!g_mockExeBuffer) { abort(); }
    memset(g_mockExeBuffer, 0, MOCK_EXE_SIZE);
    g_mockExeBuffer[0x568DC0] = 0xC3; // ret for QUEUE_DELETE_MESH_RVA
    g_mockExeBuffer[0x568EF0] = 0xC3; // ret for MANAGER_RESET_RVA
    g_mockExeBuffer[0x569980] = 0xC3; // ret for CANCEL_SEG_RVA

    char tmpPath[MAX_PATH] = {};
    GetTempPathA(sizeof(tmpPath), tmpPath);
    _snprintf_s(g_mockBaseDir, sizeof(g_mockBaseDir), _TRUNCATE, "%sRepublicMP_TestRun_%lu",
                tmpPath, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_mockBaseDir, nullptr);

    g_mockHost.structSize        = sizeof(TsmHost);
    g_mockHost.apiVersion        = TSM_API_VERSION;
    g_mockHost.exeModule         = (void*)g_mockExeBuffer;
    g_mockHost.exeBase           = g_mockExeBuffer;
    g_mockHost.exeSize           = MOCK_EXE_SIZE;
    g_mockHost.engineModule      = nullptr;
    g_mockHost.baseDir           = g_mockBaseDir;
    g_mockHost.log               = &MockHost_Log;
    g_mockHost.findIatSlot       = &MockHost_FindIatSlot;
    g_mockHost.patchIat          = &MockHost_PatchIat;
    g_mockHost.installInlineHook = &MockHost_InstallInlineHook;
    g_mockHost.allocNear         = &MockHost_AllocNear;
    g_mockHost.readablePtr       = &MockHost_ReadablePtr;
    g_mockHost.faultFilter       = &MockHost_FaultFilter;
    g_mockHost.consume           = &MockHost_Consume;
    g_mockHost.vfsRoot           = g_mockBaseDir;

    TsmPluginInfo pluginInfo = {};
    TsmPluginInit(&g_mockHost, &pluginInfo);
    TsmBind(&g_mockHost);
}

void ResetMockWorldMemory()
{
    TwoStates_ResetEconStatsForTest(nullptr);
    TwoStates_ClearToastsForTest();
    if (!g_mockExeBuffer) return;
    memset(g_mockExeBuffer, 0, MOCK_EXE_SIZE);
    g_mockExeBuffer[0x568DC0] = 0xC3; // ret for QUEUE_DELETE_MESH_RVA
    g_mockExeBuffer[0x568EF0] = 0xC3; // ret for MANAGER_RESET_RVA
    g_mockExeBuffer[0x569980] = 0xC3; // ret for CANCEL_SEG_RVA
    g_mockExeBuffer[0x18E180] = 0xC3; // ret for AUTO_PURCHASE_RVA
    g_mockExeBuffer[0x1D0C40] = 0xC3; // ret for PIPELINE_TO_EXPORT_RVA
    g_mockExeBuffer[0x1D0D50] = 0xC3; // ret for PIPELINE_TO_IMPORT_RVA
    g_mockExeBuffer[RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT] = 0xC3; // ret for RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT
}

void WriteAbsJmp12(BYTE* dst, void* fn)
{
    if (!dst) return;
    dst[0] = 0x48;
    dst[1] = 0xB8;
    memcpy(dst + 2, &fn, 8);
    dst[10] = 0xFF;
    dst[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), dst, 12);
}

int g_mockPlaySound2DCalls = 0;
int g_mockPlaySound3DCalls = 0;
int g_mockCreatePopupCalls = 0;
int g_mockRebuildNotifCalls = 0;

bool __fastcall MockOrig_PlaySound2D(void* mgr, const char* name, unsigned char loop, float vol, unsigned char p5)
{
    (void)mgr; (void)name; (void)loop; (void)vol; (void)p5;
    ++g_mockPlaySound2DCalls;
    return true;
}

bool __fastcall MockOrig_PlaySound3D(void* mgr, const char* name, const float* pos, unsigned char loop, float param5)
{
    (void)mgr; (void)name; (void)pos; (void)loop; (void)param5;
    ++g_mockPlaySound3DCalls;
    return true;
}

long long __fastcall MockOrig_CreatePopupWindow(long long world, int kind, long long ptr, int subkind, long long p5,
                                               int popupType, int stringId, long long p8, long long p9)
{
    (void)world; (void)kind; (void)ptr; (void)subkind; (void)p5;
    (void)popupType; (void)stringId; (void)p8; (void)p9;
    ++g_mockCreatePopupCalls;
    return 0x7777;
}

void __fastcall MockOrig_RebuildNotificationPopups(long long world)
{
    (void)world;
    ++g_mockRebuildNotifCalls;
}

int MockHost_PatchIatForCommands(void*, const char*, const char* fn, void*, void** outOrig, const char*)
{
    if (!fn || !outOrig) return 0;
    if (strstr(fn, "PlaySound2D") != nullptr) {
        *outOrig = (void*)&MockOrig_PlaySound2D;
        return 1;
    }
    if (strstr(fn, "PlaySound3D") != nullptr) {
        *outOrig = (void*)&MockOrig_PlaySound3D;
        return 1;
    }
    return 0;
}

int MockHost_InstallInlineHookForCommands(void* target, void*, void** outTramp, const unsigned char*, size_t, const char*)
{
    if (!target || !outTramp) return 0;
    if (target == (void*)(g_mockExeBuffer + WARNING_POPUP_CREATE_RVA)) {
        *outTramp = (void*)&MockOrig_CreatePopupWindow;
        return 1;
    }
    if (target == (void*)(g_mockExeBuffer + NOTIFICATION_REBUILD_POPUPS_RVA)) {
        *outTramp = (void*)&MockOrig_RebuildNotificationPopups;
        return 1;
    }
    return 0;
}
