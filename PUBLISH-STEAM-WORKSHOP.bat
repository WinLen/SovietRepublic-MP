@echo off
setlocal
cd /d "%~dp0"

echo =================================================================
echo   Building ^& Updating Steam Workshop Item 3823543010 (ISteamUGC)
echo =================================================================

call "TesmioLoader\build.bat"
if errorlevel 1 ( pause & exit /b 1 )

powershell -NoProfile -ExecutionPolicy Bypass -File "steam_workshop\make_preview.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "pack_friend.ps1" -Version "1.0.0" -GithubRepo "WinLen/SovietRepublic-MP"

if not exist "local\upload_ugc.exe" (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
        call "%%I\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
    cl /nologo /O2 /MT /EHsc /utf-8 /Folocal\ /Felocal\upload_ugc.exe steam_workshop\upload_ugc.cpp
)

"local\upload_ugc.exe" 3823543010
pause
