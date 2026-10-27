@echo off
setlocal
rem The hard-coded path is one machine's install; when it misses, ask the
rem Visual Studio installer where a toolset actually lives before giving up.
rem %ProgramFiles(x86)% can be unset in a shell that cannot pass a name with
rem parentheses through its environment (Git Bash drops it), so fall back to
rem the location the installer has used since forever.
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
set PF86=%ProgramFiles(x86)%
if not defined PF86 set PF86=C:\Program Files (x86)
set VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VCVARS%" if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat
    )
)
if not exist "%VCVARS%" (
    echo [build] vcvars64.bat not found - is Visual Studio with the C++ toolset installed?
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 ( echo [build] vcvars failed & exit /b 1 )

cd /d "%~dp0"
if not exist build mkdir build

echo [build] tesmioloader.dll
cl /nologo /O2 /MT /W3 /EHsc /LD ^
   /Fo"build\\" /Fd"build\\" /Fe"build\tesmioloader.dll" ^
   src\tesmioloader.cpp /link kernel32.lib
if errorlevel 1 ( echo [build] tesmioloader.dll FAILED & exit /b 1 )

if exist src\dinput8_proxy.cpp (
    echo [build] dinput8.dll ^(Steam direct launch proxy^)
    cl /nologo /O2 /MT /W3 /EHsc /LD ^
       /Fo"build\\" /Fd"build\\" /Fe"build\dinput8.dll" ^
       src\dinput8_proxy.cpp /link kernel32.lib
    if errorlevel 1 ( echo [build] dinput8.dll FAILED & exit /b 1 )
)

rem A windowed program: wWinMain, and /SUBSYSTEM:WINDOWS so double-clicking it
rem does not open a console behind the window. It attaches to the parent console
rem when there is one, so --nogui from a terminal still prints.
rem
rem /MANIFEST:EMBED because the source declares a MANIFESTDEPENDENCY on
rem Microsoft.Windows.Common-Controls v6 for themed checkboxes, and link.exe
rem otherwise leaves it in a tesmiolauncher.exe.manifest beside the exe - which
rem works only as long as nobody copies the exe on its own.
rem The application icon, compiled separately and handed to the linker like an
rem object file. rc.exe ships with the Windows SDK and vcvars64 puts it on PATH;
rem if it is missing the launcher is still built, just without an icon, because
rem an icon is not worth failing a build over.
echo [build] tesmiolauncher.exe
if exist build\tesmiolauncher.res del build\tesmiolauncher.res
rc /nologo /fo "build\tesmiolauncher.res" src\tesmiolauncher.rc >nul 2>&1
if exist build\tesmiolauncher.res (
    set LAUNCHER_RES=build\tesmiolauncher.res
) else (
    set LAUNCHER_RES=
    echo [build] rc.exe unavailable - launcher built without its icon
)
cl /nologo /O2 /MT /W3 /EHsc ^
   /Fo"build\\" /Fd"build\\" /Fe"build\tesmiolauncher.exe" ^
   src\tesmiolauncher.cpp %LAUNCHER_RES% /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED kernel32.lib
if errorlevel 1 ( echo [build] tesmiolauncher.exe FAILED & exit /b 1 )
if exist build\tesmiolauncher.exe.manifest del build\tesmiolauncher.exe.manifest

rem The signing tool. It only ever runs here, offline: the launcher embeds the
rem public half of the key and verifies, this is the half that touches the
rem private one. bcrypt.lib comes in through a pragma in tesmio_sign.h.
rem The whole block is optional - a source drop without the signing machinery
rem must still build, so a missing tsmsign.cpp skips the tool rather than
rem failing the build.
if exist tools\signing\tsmsign.cpp if exist src\tesmio_sign.h (
    echo [build] tsmsign.exe
    cl /nologo /O2 /MT /W3 /EHsc ^
       /Fo"build\\" /Fd"build\\" /Fe"build\tsmsign.exe" ^
       tools\signing\tsmsign.cpp /link kernel32.lib
    if errorlevel 1 ( echo [build] tsmsign.exe FAILED & exit /b 1 )
) else (
    echo [build] no tsmsign.cpp or no src\tesmio_sign.h - tsmsign.exe skipped
)

rem Plugins. One folder per plugin under plugins\, each holding <name>.cpp and
rem optionally <name>.ini; both land in build\plugins\, which is what the loader
rem scans. Adding a plugin is adding a folder - nothing here lists them, except
rem the two below held back on purpose.
if not exist build\plugins mkdir build\plugins

rem aging and buildings are parked, not gone: aging is a probe with no feature
rem behind it yet (docs/02-findings.md, "Where the age is not"), and buildings
rem writes into the game's own workshop_wip folder, which wants a deliberate
rem look after a game update before it runs again. Skipping them here is the
rem whole of it - the folders, source and .ini are untouched, and deleting this
rem block is the whole of turning either back on.
for /d %%P in (plugins\*) do (
    if /i "%%~nxP"=="aging" (
        echo [build] plugins\aging: skipped, parked - see build.bat
    ) else if /i "%%~nxP"=="buildings" (
        echo [build] plugins\buildings: skipped, parked - see build.bat
    ) else if exist "%%P\%%~nxP.cpp" (
        echo [build] plugins\%%~nxP.dll
        cl /nologo /O2 /MT /W3 /EHsc /LD ^
           /Fo"build\plugins\\" /Fd"build\plugins\\" /Fe"build\plugins\%%~nxP.dll" ^
           "%%P\%%~nxP.cpp" /link kernel32.lib
        if errorlevel 1 ( echo [build] plugins\%%~nxP.dll FAILED & exit /b 1 )
        if exist "%%P\%%~nxP.ini" copy /y "%%P\%%~nxP.ini" "build\plugins\%%~nxP.ini" >nul
    ) else (
        echo [build] plugins\%%~nxP: no %%~nxP.cpp, skipped
    )
)

if exist "RepublicMP\RepublicMP.vcxproj" (
    echo [build] plugins\RepublicMP.dll ^(MSBuild^)
    msbuild "RepublicMP\RepublicMP.vcxproj" /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if errorlevel 1 ( echo [build] plugins\RepublicMP.dll FAILED & exit /b 1 )
)

rem Signing. A rebuilt DLL invalidates its old .dll.sig, so every plugin is
rem re-signed on every build the private key is present for. Without the key
rem there is nothing to do - the launcher then marks the plugins "not from
rem Tesmio", which for a third-party build from source is simply the truth.
rem The key file is the one thing a release must never ship.
rem Requires both the tool and the key; a build without either stays unsigned.
if exist build\tsmsign.exe if exist tools\signing\tsm_private.bin (
    for %%D in (build\plugins\*.dll) do (
        build\tsmsign.exe sign tools\signing\tsm_private.bin "%%D" >nul
        if errorlevel 1 ( echo [build] signing %%~nxD FAILED & exit /b 1 )
    )
    echo [build] plugins signed with the Tesmio key
) else (
    echo [build] plugins left unsigned - no signing tool or no private key
)

rem build\tesmioloader.ini is the live config: the launcher writes the plugin
rem checkboxes and the game path into it, so overwriting it on every build would
rem throw away what the user chose. Copied only when it is not there yet - to
rem pick up new defaults from the repo copy, delete it and build again.
if not exist build\tesmioloader.ini (
    if exist tesmioloader.ini (
        copy /y tesmioloader.ini build\tesmioloader.ini >nul
    ) else (
        (
            echo [tesmioloader]
            echo version = b0.3.6
            echo plugins = 1
            echo vfs = 0
            echo save_manifest = 0
            echo version_check = 1
            echo menu_patch = 1
            echo menu_tag = RepublicMP Co-op
            echo.
            echo [plugins]
            echo RepublicMP = 1
        ) > build\tesmioloader.ini
    )
) else (
    echo [build] build\tesmioloader.ini kept - delete it to take the repo defaults
)
rem Auto-deploy fresh DLLs into Steam Workshop staging content and local test instances
if exist "..\steam_workshop\RepublicMP.dll" del /q "..\steam_workshop\RepublicMP.dll" >nul 2>&1
if exist "..\steam_workshop\content\TesmioLoader\build\plugins" (
    copy /y "build\plugins\RepublicMP.dll" "..\steam_workshop\content\TesmioLoader\build\plugins\RepublicMP.dll" >nul 2>&1
    copy /y "build\tesmioloader.dll" "..\steam_workshop\content\TesmioLoader\build\tesmioloader.dll" >nul 2>&1
    copy /y "build\tesmiolauncher.exe" "..\steam_workshop\content\TesmioLoader\build\tesmiolauncher.exe" >nul 2>&1
    copy /y "build\dinput8.dll" "..\steam_workshop\content\dinput8.dll" >nul 2>&1
    echo [build] synced DLLs -^> ..\steam_workshop\content
)
if exist "..\local\test-host\plugins" (
    copy /y "build\plugins\RepublicMP.dll" "..\local\test-host\plugins\RepublicMP.dll" >nul 2>&1
    copy /y "build\tesmioloader.dll" "..\local\test-host\tesmioloader.dll" >nul 2>&1
    copy /y "build\dinput8.dll" "..\local\test-host\dinput8.dll" >nul 2>&1
)
if exist "..\local\test-client\plugins" (
    copy /y "build\plugins\RepublicMP.dll" "..\local\test-client\plugins\RepublicMP.dll" >nul 2>&1
    copy /y "build\tesmioloader.dll" "..\local\test-client\tesmioloader.dll" >nul 2>&1
    copy /y "build\dinput8.dll" "..\local\test-client\dinput8.dll" >nul 2>&1
)
echo [build] ok -^> build\tesmiolauncher.exe
endlocal
exit /b 0
