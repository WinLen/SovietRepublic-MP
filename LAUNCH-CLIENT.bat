@echo off
chcp 65001 >nul
title RepublicMP — Тестовый КЛИЕНТ (Правое окно, 127.0.0.1)

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "BUILD_DIR=%ROOT_DIR%\TesmioLoader\build"
set "CLIENT_DIR=%ROOT_DIR%\local\test-client"

if exist "%BUILD_DIR%\plugins\RepublicMP.dll" (
    call :sync_dir "%CLIENT_DIR%"
    if errorlevel 1 exit /b 1
)

pushd "%CLIENT_DIR%"
set "REPUBLICMP_ROLE=CLIENT"
set "REPUBLICMP_AUTO_HOST=0"
set "REPUBLICMP_AUTO_JOIN=0"
if /I "%~1"=="--auto" set "REPUBLICMP_AUTO_JOIN=1"
start "[CLIENT] Soviet Republic MP" "%CLIENT_DIR%\SOVIET64.exe"
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
