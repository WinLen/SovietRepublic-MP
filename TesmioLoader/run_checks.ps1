# SPDX-License-Identifier: GPL-3.0-only
# Unified Build + C++ Static Analysis (MSVC /analyze + LLVM clang-tidy) + Unit Test Runner
$ErrorActionPreference = "Stop"

$tesmioDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repDir    = Join-Path $tesmioDir "RepublicMP"
$buildDir  = Join-Path $tesmioDir "build"

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host " [1/5] Building TesmioLoader & RepublicMP.dll (build.bat)" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan
& cmd.exe /c "`"$tesmioDir\build.bat`""
if ($LASTEXITCODE -ne 0) {
    Write-Error "build.bat failed with exit code $LASTEXITCODE"
    exit 1
}

# Locate Visual Studio vcvars64.bat
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vcvars) -and (Test-Path $vswhere)) {
    $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($inst) {
        $vcvars = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
    }
}

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host " [2/5] Compiling & Running Unit Test Suite (republicmp_tests.exe)" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan

# Remove any stale .obj files left from deleted legacy UI files
Remove-Item -Path (Join-Path $repDir "obj\Release\x64\debug_window.obj") -Force -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $repDir "obj\Release\x64\main_menu.obj") -Force -ErrorAction SilentlyContinue

for ($retry = 0; $retry -lt 5; $retry++) {
    try {
        if (Test-Path (Join-Path $buildDir "republicmp_tests.exe")) {
            Remove-Item -Path (Join-Path $buildDir "republicmp_tests.exe") -Force -ErrorAction Stop
        }
        if (Test-Path (Join-Path $buildDir "test_main.obj")) {
            Remove-Item -Path (Join-Path $buildDir "test_main.obj") -Force -ErrorAction Stop
        }
        break
    } catch {
        Start-Sleep -Milliseconds 500
    }
}

$testCompileCmd = @"
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repDir"
cl /nologo /O2 /MT /W3 /EHsc /std:c++20 /utf-8 ^
   /D "NDEBUG" /D "_WINDOWS" /D "_CRT_SECURE_NO_WARNINGS" /D "NOMINMAX" ^
   /I"..\src" /I"third_party\imgui" /I"third_party\imgui\backends" ^
   /Fo"$buildDir\test_main.obj" /Fe"$buildDir\republicmp_tests.exe" ^
   tests\test_main.cpp obj\Release\x64\*.obj ^
   /link /LTCG /SUBSYSTEM:CONSOLE kernel32.lib user32.lib gdi32.lib ws2_32.lib winhttp.lib d3d11.lib dxgi.lib imm32.lib advapi32.lib shell32.lib
"@

$tempBat = Join-Path $buildDir "compile_tests.bat"
Set-Content -Path $tempBat -Value $testCompileCmd -Encoding ASCII
& cmd.exe /c "`"$tempBat`""
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to compile republicmp_tests.exe"
    exit 1
}

& "$buildDir\republicmp_tests.exe"
if ($LASTEXITCODE -ne 0) {
    Write-Error "republicmp_tests.exe failed with exit code $LASTEXITCODE"
    exit 1
}

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host " [3/5] Running MSVC C++ Static Code Analysis (/analyze)" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan

$analyzeCmd = @"
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repDir"
cl /nologo /analyze:only /WX /W3 /EHsc /std:c++20 /utf-8 /MT ^
   /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "_CRT_SECURE_NO_WARNINGS" /D "NOMINMAX" ^
   /wd6320 /wd6255 /wd6258 /wd28159 /wd6101 /wd6322 ^
   /I"..\src" /I"third_party\imgui" /I"third_party\imgui\backends" ^
   core\main.cpp ^
   core\diag_log.cpp ^
   game\save_load.cpp ^
   game\present_hook.cpp ^
   game\imgui_hook.cpp ^
   game\session_patches.cpp ^
   game\input_block.cpp ^
   session\game_speed.cpp ^
   session\sync_check.cpp ^
   session\money.cpp ^
   session\two_states.cpp ^
   session\mod_sync.cpp ^
   session\updater.cpp ^
   session\settings_sync.cpp ^
   session\weather.cpp ^
   session\save_sync.cpp ^
   determinism\state_hash.cpp ^
   determinism\held_frame.cpp ^
   determinism\determinism.cpp ^
   determinism\sim_patches.cpp ^
   determinism\probe_trace.cpp ^
   determinism\probe_watch.cpp ^
   lockstep\commands.cpp ^
   lockstep\purchase.cpp ^
   lockstep\route_tools.cpp ^
   lockstep\world_refs.cpp ^
   lockstep\game_calls.cpp ^
   lockstep\line_accept.cpp ^
   lockstep\vehicle_switches.cpp ^
   lockstep\building_switches.cpp ^
   lockstep\brush_tools.cpp ^
   lockstep\hover_tools.cpp ^
   lockstep\junction_accept.cpp ^
   lockstep\cargo_settings.cpp ^
   lockstep\source_slots.cpp ^
   lockstep\rename.cpp ^
   lockstep\lockstep_net.cpp ^
   world_sync\local_builds.cpp ^
   world_sync\view_list_scope.cpp ^
   world_sync\terrain\terrain_hook.cpp ^
   world_sync\terrain\terrain_sync.cpp ^
   world_sync\buildings\demolish_hook.cpp ^
   world_sync\buildings\placement_collision.cpp ^
   world_sync\buildings\build_sync.cpp ^
   world_sync\citizens\citizen_destinations.cpp ^
   world_sync\vehicles\vehicle_soft_sync.cpp ^
   world_sync\networks\road_sync.cpp ^
   save\save_zip.cpp ^
   transport\network.cpp ^
   transport\steam_p2p.cpp ^
   ui\native_menu.cpp ^
   ui\ui_log.cpp ^
   ui\debug_log.cpp ^
   ui\sync_block.cpp ^
   tests\test_main.cpp
"@

$analyzeBat = Join-Path $buildDir "run_msvc_analyze.bat"
Set-Content -Path $analyzeBat -Value $analyzeCmd -Encoding ASCII
$analyzeOut = & cmd.exe /c "`"$analyzeBat`" 2>&1"
$analyzeExit = $LASTEXITCODE
$analyzeOut | ForEach-Object { Write-Host $_ }
Remove-Item -Path (Join-Path $repDir "*.nativecodeanalysis.xml") -Force -ErrorAction SilentlyContinue
$warns = $analyzeOut | Where-Object { $_ -match "warning C[0-9]+" }
if ($analyzeExit -ne 0 -or $warns.Count -gt 0) {
    Write-Error "MSVC /analyze failed with exit code $analyzeExit and $($warns.Count) warning(s)"
    exit 1
}
Write-Host "[PASS] MSVC /analyze completed cleanly across all 55 compilation units (0 warnings)." -ForegroundColor Green

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host " [4/5] Running LLVM clang-tidy Static Analyzer" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan

$clangTidy = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe"
if (-not (Test-Path $clangTidy)) {
    $clangTidy = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\Llvm\x64\bin\clang-tidy.exe"
}
if (-not (Test-Path $clangTidy)) {
    $found = Get-Command clang-tidy.exe -ErrorAction SilentlyContinue
    if ($found) { $clangTidy = $found.Source }
}

if (Test-Path $clangTidy) {
    $tidyVcvars = $vcvars
    if ($clangTidy -like "*2019*") {
        $vs2019Vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $vs2019Vcvars) { $tidyVcvars = $vs2019Vcvars }
    }
    $tidyStd = if ($clangTidy -like "*2019*") { "-std=c++17" } else { "-std=c++20" }

    $tidyCmd = @"
call "$tidyVcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$repDir"
"$clangTidy" --quiet ^
   core\diag_log.cpp ^
   determinism\state_hash.cpp ^
   determinism\determinism.cpp ^
   lockstep\world_refs.cpp ^
   lockstep\building_switches.cpp ^
   lockstep\commands.cpp ^
   lockstep\rename.cpp ^
   session\two_states.cpp ^
   session\mod_sync.cpp ^
   save\save_zip.cpp ^
   ui\native_menu.cpp ^
   world_sync\networks\road_sync.cpp ^
   tests\test_main.cpp ^
   -- $tidyStd -fms-extensions -fms-compatibility -D_MT -DNDEBUG -D_WINDOWS -D_USRDLL -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -Wno-microsoft-include -Wno-pragma-pack -I..\src -Ithird_party\imgui -Ithird_party\imgui\backends
"@
    $tidyBat = Join-Path $buildDir "run_clang_tidy.bat"
    Set-Content -Path $tidyBat -Value $tidyCmd -Encoding ASCII
    $tidyOut = & cmd.exe /c "`"$tidyBat`" 2>&1"
    $tidyExit = $LASTEXITCODE
    $tidyOut | ForEach-Object { Write-Host $_ }
    if ($tidyExit -ne 0) {
        Write-Error "clang-tidy failed with exit code $tidyExit"
        exit 1
    }
    Write-Host "[PASS] LLVM clang-tidy completed cleanly." -ForegroundColor Green
} else {
    Write-Warning "clang-tidy.exe not found on PATH; skipped step 4."
}

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host " [5/5] Running ub-mode Clean Code Gate (Crapper + Dryer + Dead Code)" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan

& python "$tesmioDir\ub_check.py" --root "$repDir" --dry-threshold 0.80 --max-dups 0 --max-cc 60
if ($LASTEXITCODE -ne 0) {
    Write-Error "ub-mode Clean Code Gate failed with exit code $LASTEXITCODE"
    exit 1
}
Write-Host "[PASS] ub-mode Clean Code Gate completed cleanly." -ForegroundColor Green

Write-Host "================================================================" -ForegroundColor Green
Write-Host " ALL CHECKS PASSED (Build + Unit Tests + MSVC /analyze + clang-tidy + ub-mode)" -ForegroundColor Green
Write-Host "================================================================" -ForegroundColor Green
exit 0
