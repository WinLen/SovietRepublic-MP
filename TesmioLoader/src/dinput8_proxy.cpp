// SPDX-License-Identifier: GPL-3.0-only
// dinput8.dll proxy auto-loader for Workers & Resources: Soviet Republic.
//
// When placed alongside SOVIET64.exe, Windows loads this DLL automatically
// when the player clicks "Play" in Steam (via C3DDLL64.dll -> DINPUT8.dll).
// Before the engine initializes DirectX / DirectInput, it locates and loads
// tesmioloader.dll (preferring the subscribed Steam Workshop folder
// steamapps\workshop\content\784150\3822972914 if present, or local
// TesmioLoader\build\tesmioloader.dll), so no external .bat or launcher is
// ever needed to play Co-op!

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <cstdio>
#include <cstring>

typedef HRESULT (WINAPI *Fn_DirectInput8Create)(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter);
typedef HRESULT (WINAPI *Fn_CreateDXGIFactory1)(REFIID riid, void** ppFactory);

static HMODULE g_realDinput8 = nullptr;
static Fn_DirectInput8Create g_origDirectInput8Create = nullptr;
static Fn_CreateDXGIFactory1 g_origCreateDXGIFactory1 = nullptr;
static volatile LONG g_loaderInitialized = 0;

static bool FileExistsA(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void BootLogf(const char* gameDir, bool rotate, const char* fmt, ...)
{
    char logPath[MAX_PATH] = {};
    _snprintf_s(logPath, sizeof(logPath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.log", gameDir);
    if (rotate) {
        char prevPath[MAX_PATH] = {};
        _snprintf_s(prevPath, sizeof(prevPath), _TRUNCATE, "%s\\RepublicMP_Diagnostics.prev.log", gameDir);
        CopyFileA(logPath, prevPath, FALSE);
        DeleteFileA(logPath);
    }
    HANDLE h = CreateFileA(logPath, FILE_APPEND_DATA | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2300];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [TID:%05lu] [BOOT   ] %s\r\n",
                        st.wYear, st.wMonth, st.wDay,
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        (unsigned long)GetCurrentThreadId(), body);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)n, &written, nullptr);
        FlushFileBuffers(h);
    }
    CloseHandle(h);
}

static void EnsureLoaderIni(const char* buildDir, const char* exePath)
{
    char iniPath[MAX_PATH] = {};
    _snprintf_s(iniPath, sizeof(iniPath), _TRUNCATE, "%s\\tesmioloader.ini", buildDir);
    WritePrivateProfileStringA("tesmioloader", "version", "b0.3.6", iniPath);
    WritePrivateProfileStringA("tesmioloader", "game_exe", exePath, iniPath);
    WritePrivateProfileStringA("tesmioloader", "plugins", "1", iniPath);
    WritePrivateProfileStringA("tesmioloader", "vfs", "0", iniPath);
    WritePrivateProfileStringA("tesmioloader", "save_manifest", "0", iniPath);
    WritePrivateProfileStringA("tesmioloader", "version_check", "1", iniPath);
    WritePrivateProfileStringA("tesmioloader", "menu_patch", "1", iniPath);
    WritePrivateProfileStringA("tesmioloader", "menu_tag", "RepublicMP Co-op", iniPath);
    WritePrivateProfileStringA("plugins", "RepublicMP", "1", iniPath);
}

static void EnsureTesmioLoaderLoaded()
{
    if (InterlockedCompareExchange(&g_loaderInitialized, 1, 0) != 0)
        return;

    // If launched via tesmiolauncher.exe, tesmioloader.dll is already in memory.
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char gameDir[MAX_PATH] = {};
    strncpy_s(gameDir, exePath, _TRUNCATE);
    if (char* slash = strrchr(gameDir, '\\')) *slash = '\0';

    if (GetModuleHandleA("tesmioloader.dll") || GetModuleHandleA("RepublicMP.dll")) {
        BootLogf(gameDir, false, "dinput8.dll: tesmioloader.dll/RepublicMP.dll already in memory (launched via tesmiolauncher)");
        return;
    }

    BootLogf(gameDir, true, "dinput8.dll: Direct Steam launch detected for %s", exePath);

    char candidateBuildDirs[4][MAX_PATH] = {};
    int numCandidates = 0;

    // 1. Subscribed Steam Workshop item 3822972914 (steamapps\workshop\content\784150\3822972914\TesmioLoader\build)
    _snprintf_s(candidateBuildDirs[numCandidates++], MAX_PATH, _TRUNCATE,
                "%s\\..\\..\\workshop\\content\\784150\\3822972914\\TesmioLoader\\build", gameDir);

    // 2. Local TesmioLoader\build inside game directory
    _snprintf_s(candidateBuildDirs[numCandidates++], MAX_PATH, _TRUNCATE,
                "%s\\TesmioLoader\\build", gameDir);

    // 3. Game root directory itself (if tesmioloader.dll + plugins\RepublicMP.dll were placed flat in game root)
    _snprintf_s(candidateBuildDirs[numCandidates++], MAX_PATH, _TRUNCATE,
                "%s", gameDir);

    // Also scan any other folder inside workshop\content\784150\*
    char wsWildcard[MAX_PATH] = {};
    _snprintf_s(wsWildcard, sizeof(wsWildcard), _TRUNCATE, "%s\\..\\..\\workshop\\content\\784150\\*", gameDir);
    WIN32_FIND_DATAA fd = {};
    HANDLE hFind = FindFirstFileA(wsWildcard, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                char extraWs[MAX_PATH] = {};
                _snprintf_s(extraWs, sizeof(extraWs), _TRUNCATE,
                            "%s\\..\\..\\workshop\\content\\784150\\%s\\TesmioLoader\\build\\tesmioloader.dll",
                            gameDir, fd.cFileName);
                if (FileExistsA(extraWs) && numCandidates < 4) {
                    _snprintf_s(candidateBuildDirs[numCandidates++], MAX_PATH, _TRUNCATE,
                                "%s\\..\\..\\workshop\\content\\784150\\%s\\TesmioLoader\\build",
                                gameDir, fd.cFileName);
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    for (int i = 0; i < numCandidates; ++i) {
        char dllPath[MAX_PATH] = {};
        _snprintf_s(dllPath, sizeof(dllPath), _TRUNCATE, "%s\\tesmioloader.dll", candidateBuildDirs[i]);
        BootLogf(gameDir, false, "dinput8.dll: Checking candidate [%d]: %s (exists=%d)",
                 i, dllPath, (int)FileExistsA(dllPath));
        if (FileExistsA(dllPath)) {
            EnsureLoaderIni(candidateBuildDirs[i], exePath);
            HMODULE hLoaded = LoadLibraryA(dllPath);
            BootLogf(gameDir, false, "dinput8.dll: LoadLibraryA(%s) -> %p (err=%lu)",
                     dllPath, (void*)hLoaded, (unsigned long)GetLastError());
            break;
        }
    }
}

static HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    EnsureTesmioLoaderLoaded();
    return g_origCreateDXGIFactory1(riid, ppFactory);
}

static void PatchIatCreateDXGI(HMODULE mod)
{
    if (!mod) return;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), "dxgi.dll") != 0) continue;
        DWORD nameRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + nameRva);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, "CreateDXGIFactory1") == 0) {
                DWORD prot = 0;
                if (VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &prot)) {
                    if (!g_origCreateDXGIFactory1)
                        g_origCreateDXGIFactory1 = (Fn_CreateDXGIFactory1)ft->u1.Function;
                    ft->u1.Function = (ULONG_PTR)HookedCreateDXGIFactory1;
                    VirtualProtect(&ft->u1.Function, sizeof(void*), prot, &prot);
                }
                return;
            }
        }
    }
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    EnsureTesmioLoaderLoaded();

    if (!g_realDinput8) {
        char sysDir[MAX_PATH] = {};
        GetSystemDirectoryA(sysDir, MAX_PATH);
        char realPath[MAX_PATH] = {};
        _snprintf_s(realPath, sizeof(realPath), _TRUNCATE, "%s\\dinput8.dll", sysDir);
        g_realDinput8 = LoadLibraryA(realPath);
        if (g_realDinput8) {
            g_origDirectInput8Create = (Fn_DirectInput8Create)GetProcAddress(g_realDinput8, "DirectInput8Create");
        }
    }
    if (!g_origDirectInput8Create) return E_FAIL;
    return g_origDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        PatchIatCreateDXGI(GetModuleHandleA(nullptr));
        PatchIatCreateDXGI(GetModuleHandleA("C3DDLL64.dll"));
    }
    return TRUE;
}
