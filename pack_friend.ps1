param(
    [string]$Version = "",
    [string]$GithubRepo = $(if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { "WinLen/SovietRepublic-MP" })
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Version) {
    $verHeader = Join-Path $root 'TesmioLoader\RepublicMP\core\version.h'
    if (Test-Path $verHeader) {
        $vh = Get-Content $verHeader -Raw
        if ($vh -match '#define\s+REPUBLICMP_VERSION\s+"([^"]+)"') {
            $Version = $Matches[1]
        }
    }
    if (-not $Version) { $Version = "1.0.9" }
}
$stage = Join-Path $root 'local\friend_pack_stage'
$wsContent = Join-Path $root 'steam_workshop\content'
$zip = Join-Path $root 'RepublicMP_Friend_Pack.zip'

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'TesmioLoader\build\plugins') -Force | Out-Null

Copy-Item (Join-Path $root 'TesmioLoader\build\tesmiolauncher.exe') (Join-Path $stage 'TesmioLoader\build\') -Force
Copy-Item (Join-Path $root 'TesmioLoader\build\tesmioloader.dll')   (Join-Path $stage 'TesmioLoader\build\') -Force
Copy-Item (Join-Path $root 'TesmioLoader\build\plugins\RepublicMP.dll') (Join-Path $stage 'TesmioLoader\build\plugins\') -Force
if (Test-Path (Join-Path $root 'TesmioLoader\build\dinput8.dll')) {
    Copy-Item (Join-Path $root 'TesmioLoader\build\dinput8.dll') (Join-Path $stage 'dinput8.dll') -Force
}
if (Test-Path (Join-Path $root 'README-RU.txt')) {
    Copy-Item (Join-Path $root 'README-RU.txt') (Join-Path $stage 'README-RU.txt') -Force
}
if (Test-Path (Join-Path $root 'TesmioLoader\RepublicMP\assets\editor')) {
    New-Item -ItemType Directory -Path (Join-Path $stage 'media_soviet\editor') -Force | Out-Null
    Copy-Item (Join-Path $root 'TesmioLoader\RepublicMP\assets\editor\mp_icon_*.png') (Join-Path $stage 'media_soviet\editor\') -Force
}

# Write republicmp_updater.ini for in-game version metadata
$updaterIni = @(
    '[updater]',
    "github_repo=$GithubRepo",
    "current_version=$Version"
)
[System.IO.File]::WriteAllText((Join-Path $stage 'TesmioLoader\build\plugins\republicmp_updater.ini'), ($updaterIni -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)

$batLines = @(
    '@echo off',
    'setlocal EnableDelayedExpansion',
    'set "HERE=%~dp0"',
    'if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"',
    '',
    'rem Locate SOVIET64.exe (current dir, relative from Steam Workshop 784150, or standard Steam libraries)',
    'set "GAME_EXE="',
    'if exist "%HERE%\SOVIET64.exe" set "GAME_EXE=%HERE%\SOVIET64.exe"',
    'if not defined GAME_EXE if exist "%HERE%\..\..\..\..\common\SovietRepublic\SOVIET64.exe" (',
    '  for %%F in ("%HERE%\..\..\..\..\common\SovietRepublic\SOVIET64.exe") do set "GAME_EXE=%%~fF"',
    ')',
    'if not defined GAME_EXE if exist "C:\Program Files (x86)\Steam\steamapps\common\SovietRepublic\SOVIET64.exe" set "GAME_EXE=C:\Program Files (x86)\Steam\steamapps\common\SovietRepublic\SOVIET64.exe"',
    'if not defined GAME_EXE if exist "C:\Program Files\Steam\steamapps\common\SovietRepublic\SOVIET64.exe" set "GAME_EXE=C:\Program Files\Steam\steamapps\common\SovietRepublic\SOVIET64.exe"',
    'if not defined GAME_EXE (',
    '  for %%D in (C D E F G H) do (',
    '    if not defined GAME_EXE if exist "%%D:\SteamLibrary\steamapps\common\SovietRepublic\SOVIET64.exe" set "GAME_EXE=%%D:\SteamLibrary\steamapps\common\SovietRepublic\SOVIET64.exe"',
    '    if not defined GAME_EXE if exist "%%D:\Steam\steamapps\common\SovietRepublic\SOVIET64.exe" set "GAME_EXE=%%D:\Steam\steamapps\common\SovietRepublic\SOVIET64.exe"',
    '    if not defined GAME_EXE if exist "%%D:\Games\Steam\steamapps\common\SovietRepublic\SOVIET64.exe" set "GAME_EXE=%%D:\Games\Steam\steamapps\common\SovietRepublic\SOVIET64.exe"',
    '  )',
    ')',
    'if not defined GAME_EXE (',
    '  echo [!] SOVIET64.exe not found automatically. Unpack this archive into your SovietRepublic game folder and run START-COOP.bat again.',
    '  pause',
    '  exit /b 1',
    ')',
    'echo Found game: !GAME_EXE!',
    'for %%G in ("!GAME_EXE!") do set "GAME_DIR=%%~dpG"',
    'if exist "%HERE%\dinput8.dll" copy /y "%HERE%\dinput8.dll" "!GAME_DIR!dinput8.dll" >nul 2>&1',
    'if exist "%HERE%\media_soviet\editor\mp_icon_coop.png" copy /y "%HERE%\media_soviet\editor\mp_icon_*.png" "!GAME_DIR!media_soviet\editor\" >nul 2>&1',
    'echo [OK] dinput8.dll installed into !GAME_DIR! - future launches work directly via Play in Steam!',
    'start "" "!GAME_EXE!"'
)
[System.IO.File]::WriteAllText((Join-Path $stage 'START-COOP.bat'), ($batLines -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)

# Mirror staged files to steam_workshop/content for Steam Workshop upload
if (Test-Path $wsContent) { Remove-Item $wsContent -Recurse -Force }
New-Item -ItemType Directory -Path $wsContent -Force | Out-Null
Copy-Item (Join-Path $stage '*') $wsContent -Recurse -Force

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Write-Host ("[OK] Created " + $zip + " (" + (Get-Item $zip).Length + " bytes) and staged steam_workshop\content")
