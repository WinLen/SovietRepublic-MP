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

# Write republicmp_updater.ini for both pre-launch PowerShell updater and in-game WinHTTP updater
$updaterIni = @(
    '[updater]',
    "github_repo=$GithubRepo",
    "current_version=$Version"
)
[System.IO.File]::WriteAllText((Join-Path $stage 'TesmioLoader\build\plugins\republicmp_updater.ini'), ($updaterIni -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)

# Pre-launch auto-update script (checks Steam Workshop folder + GitHub Releases before starting SOVIET64.exe)
$updatePs1Lines = @(
    '$ErrorActionPreference = "SilentlyContinue"',
    '$here = Split-Path -Parent $MyInvocation.MyCommand.Path',
    '$dllPath = Join-Path $here "TesmioLoader\build\plugins\RepublicMP.dll"',
    '$iniPath = Join-Path $here "TesmioLoader\build\plugins\republicmp_updater.ini"',
    '$repo = ""',
    '$curVer = ""',
    'if (Test-Path $iniPath) {',
    '    foreach ($line in Get-Content $iniPath) {',
    '        if ($line -match "^github_repo=(.+)$") { $repo = $Matches[1].Trim() }',
    '        if ($line -match "^current_version=(.+)$") { $curVer = $Matches[1].Trim().TrimStart("v","V") }',
    '    }',
    '}',
    'if ($repo -and $repo.Contains("/")) {',
    '    Write-Host "[Updater] Checking GitHub Releases ($repo)..."',
    '    $apiUrl = "https://api.github.com/repos/$repo/releases/latest"',
    '    $rel = Invoke-RestMethod -Uri $apiUrl -TimeoutSec 4 -Headers @{ "User-Agent" = "RepublicMP-Updater" }',
    '    if ($rel -and $rel.tag_name) {',
    '        $remoteVer = $rel.tag_name.Trim().TrimStart("v","V")',
    '        $isNewer = $false',
    '        try { if ($curVer) { $isNewer = ([version]$remoteVer -gt [version]$curVer) } } catch {}',
    '        if ($isNewer) {',
    '            $asset = $rel.assets | Where-Object { $_.name -eq "RepublicMP.dll" } | Select-Object -First 1',
    '            if ($asset -and $asset.browser_download_url) {',
    '                Write-Host "[Updater] Downloading update $($rel.tag_name) from GitHub Releases..."',
    '                $tmpDll = "$dllPath.new"',
    '                Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $tmpDll -TimeoutSec 15 -UseBasicParsing',
    '                if ((Test-Path $tmpDll) -and ((Get-Item $tmpDll).Length -gt 65536)) {',
    '                    Move-Item -Path $tmpDll -Destination $dllPath -Force',
    '                    Set-Content -Path $iniPath -Value "[updater]`r`ngithub_repo=$repo`r`ncurrent_version=$remoteVer`r`n" -Encoding ASCII',
    '                    Write-Host "[Updater] Updated RepublicMP.dll to $($rel.tag_name)!"',
    '                }',
    '            }',
    '        }',
    '    }',
    '}'
)
[System.IO.File]::WriteAllText((Join-Path $stage 'update_mod.ps1'), ($updatePs1Lines -join "`r`n") + "`r`n", [System.Text.Encoding]::UTF8)

$batLines = @(
    '@echo off',
    'setlocal EnableDelayedExpansion',
    'set "HERE=%~dp0"',
    'if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"',
    '',
    'rem 1. Auto-update from GitHub Releases before launching',
    'if exist "%HERE%\update_mod.ps1" (',
    '  powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%\update_mod.ps1"',
    ')',
    '',
    'rem 2. Locate SOVIET64.exe (current dir, relative from Steam Workshop 784150, or standard Steam libraries)',
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
    '(',
    'echo [tesmioloader]',
    'echo version = b0.3.6',
    'echo game_exe =!GAME_EXE!',
    'echo plugins =1',
    'echo vfs = 0',
    'echo save_manifest = 0',
    'echo version_check = 1',
    'echo menu_patch = 1',
    'echo menu_tag =tesmioloader v. b0.3.6',
    'echo.',
    'echo [plugins]',
    'echo RepublicMP =1',
    ') > "%HERE%\TesmioLoader\build\tesmioloader.ini"',
    'cd /d "%HERE%\TesmioLoader\build"',
    'start "" "tesmiolauncher.exe" --nogui'
)
[System.IO.File]::WriteAllText((Join-Path $stage 'START-COOP.bat'), ($batLines -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)

# Helper script to create Desktop shortcut (useful when subscribed via Steam Workshop)
$shortcutBat = @(
    '@echo off',
    'set "HERE=%~dp0"',
    'if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"',
    'powershell -NoProfile -ExecutionPolicy Bypass -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([Environment]::GetFolderPath(''Desktop'') + ''\Soviet Republic Co-op (RepublicMP).lnk''); $s.TargetPath = ''%HERE%\START-COOP.bat''; $s.WorkingDirectory = ''%HERE%''; $s.Save()"',
    'echo [OK] Desktop shortcut "Soviet Republic Co-op (RepublicMP)" created!',
    'pause'
)
[System.IO.File]::WriteAllText((Join-Path $stage 'CREATE-DESKTOP-SHORTCUT.bat'), ($shortcutBat -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)

# Mirror staged files to steam_workshop/content for Steam Workshop upload
if (Test-Path $wsContent) { Remove-Item $wsContent -Recurse -Force }
New-Item -ItemType Directory -Path $wsContent -Force | Out-Null
Copy-Item (Join-Path $stage '*') $wsContent -Recurse -Force

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Write-Host ("[OK] Created " + $zip + " (" + (Get-Item $zip).Length + " bytes) and staged steam_workshop\content")
