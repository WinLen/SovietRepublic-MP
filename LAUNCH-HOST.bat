@echo off
chcp 65001 >nul
title RepublicMP — Тестовый ХОСТ (Левое окно)

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "BUILD_DIR=%ROOT_DIR%\TesmioLoader\build"
set "HOST_DIR=%ROOT_DIR%\local\test-host"

if exist "%BUILD_DIR%\plugins\RepublicMP.dll" (
    call :sync_dir "%HOST_DIR%"
    if errorlevel 1 exit /b 1
)

pushd "%HOST_DIR%"
set "REPUBLICMP_ROLE=HOST"
set "REPUBLICMP_AUTO_HOST=0"
set "REPUBLICMP_AUTO_JOIN=0"
if /I "%~1"=="--auto" set "REPUBLICMP_AUTO_HOST=1"
start "[HOST] Soviet Republic MP" "%HOST_DIR%\SOVIET64.exe"
popd
goto :eof

:sync_dir
if not exist "%~1\plugins" mkdir "%~1\plugins"
call :sync_file "%BUILD_DIR%\plugins\RepublicMP.dll" "%~1\plugins\RepublicMP.dll" || exit /b 1
call :sync_file "%BUILD_DIR%\tesmioloader.dll" "%~1\tesmioloader.dll" || exit /b 1
call :sync_file "%BUILD_DIR%\dinput8.dll" "%~1\dinput8.dll" || exit /b 1
call :sync_file "%BUILD_DIR%\tesmiolauncher.exe" "%~1\tesmiolauncher.exe" || exit /b 1
goto :eof

:sync_file
copy /Y "%~1" "%~2" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy "%~1" to "%~2". Close any running SOVIET64.exe and retry.
    exit /b 1
)
goto :eof
