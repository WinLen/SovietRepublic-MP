@echo off
setlocal
cd /d "%~dp0"

echo =================================================================
echo   Building ^& Updating Steam Workshop Item 3823543010 (ISteamUGC)
echo =================================================================

call "TesmioLoader\build.bat"
if errorlevel 1 ( pause & exit /b 1 )

powershell -NoProfile -ExecutionPolicy Bypass -File "steam_workshop\make_preview.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "pack_friend.ps1" -GithubRepo "WinLen/SovietRepublic-MP"

for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do (
    call "%%I\VC\Auxiliary\Build\vcvars64.bat" >nul
)
cl /nologo /O2 /MT /EHsc /utf-8 /Fosteam_workshop\ /Festeam_workshop\upload_ugc.exe steam_workshop\upload_ugc.cpp
if exist "steam_workshop\upload_ugc.obj" del /q "steam_workshop\upload_ugc.obj"

"steam_workshop\upload_ugc.exe" 3823543010 --meta
pause
