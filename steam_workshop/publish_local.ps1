$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$wsDir = Join-Path $root 'steam_workshop'
$steamcmdDir = Join-Path $root 'local\steamcmd'

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  Publication to Steam Workshop (AppID 784150 - Soviet Republic) " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

# 1. Ensure build & package are fresh
& (Join-Path $wsDir 'make_preview.ps1')
& (Join-Path $root 'pack_friend.ps1') -Version "1.0.0"

# 2. Download SteamCMD if not present
New-Item -ItemType Directory -Path $steamcmdDir -Force | Out-Null
$steamcmdExe = Join-Path $steamcmdDir 'steamcmd.exe'
if (-not (Test-Path $steamcmdExe)) {
    Write-Host "[SteamCMD] Downloading steamcmd.zip..." -ForegroundColor Yellow
    $zipPath = Join-Path $steamcmdDir 'steamcmd.zip'
    Invoke-WebRequest -Uri "https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip" -OutFile $zipPath -UseBasicParsing
    Expand-Archive -Path $zipPath -DestinationPath $steamcmdDir -Force
}

# 3. Read or ask for Workshop ID (always default to existing RepublicMP item 3823543010)
$idFile = Join-Path $wsDir 'published_file_id.txt'
$defaultId = "3823543010"
if (Test-Path $idFile) {
    $idVal = (Get-Content $idFile -Raw).Trim()
    if ($idVal -and $idVal -ne "0") { $defaultId = $idVal }
}
$steamUser = Read-Host "Enter your Steam Username (login)"
$pubId = Read-Host "Enter existing Workshop ID or press Enter for [$defaultId] (0 = create new)"
if (-not $pubId) { $pubId = $defaultId }

$contentDir = Join-Path $wsDir 'content'
$previewFile = Join-Path $wsDir 'preview.png'
$vdfPath = Join-Path $wsDir 'workshop_local.vdf'

$vdf = @"
"workshopitem"
{
	"appid"		"784150"
	"publishedfileid"		"$pubId"
	"contentfolder"		"$contentDir"
	"previewfile"		"$previewFile"
	"visibility"		"0"
	"title"		"RepublicMP - Co-op Multiplayer (One State / Two States)"
	"description"		"Co-op multiplayer mod for Workers & Resources: Soviet Republic (Lockstep sync, Steam P2P, One State & Two States modes, 30x30 km maps, GitHub Releases auto-updater)."
	"changenote"		"Release update"
}
"@
[System.IO.File]::WriteAllText($vdfPath, $vdf, [System.Text.Encoding]::ASCII)

Write-Host ""
Write-Host "[SteamCMD] Launching SteamCMD (enter your password and Steam Guard code if prompted)..." -ForegroundColor Yellow
& $steamcmdExe +login $steamUser +workshop_build_item $vdfPath +quit

# Check if SteamCMD updated publishedfileid inside workshop_local.vdf
if (Test-Path $vdfPath) {
    $afterVdf = Get-Content $vdfPath -Raw
    if ($afterVdf -match '"publishedfileid"\s+"(\d+)"') {
        $newId = $Matches[1]
        if ($newId -ne "0") {
            Set-Content -Path $idFile -Value $newId -Encoding ASCII
            Write-Host ""
            Write-Host "[SUCCESS] Steam Workshop Item ID (STEAM_WORKSHOP_ID): $newId" -ForegroundColor Green
            Write-Host "Workshop URL: https://steamcommunity.com/sharedfiles/filedetails/?id=$newId" -ForegroundColor Green
        }
    }
}

# Export Base64 config.vdf for GitHub Actions secret STEAM_CONFIG_VDF
$cfgFile = Join-Path $steamcmdDir 'config\config.vdf'
if (Test-Path $cfgFile) {
    $b64 = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($cfgFile))
    $b64Out = Join-Path $wsDir 'STEAM_CONFIG_VDF.txt'
    Set-Content -Path $b64Out -Value $b64 -Encoding ASCII
    Write-Host "[SUCCESS] Saved Base64 Steam session token to: $b64Out" -ForegroundColor Green
    Write-Host "Copy its contents into GitHub Secret 'STEAM_CONFIG_VDF' so GitHub Actions can upload without Steam Guard prompts!" -ForegroundColor Yellow
}
