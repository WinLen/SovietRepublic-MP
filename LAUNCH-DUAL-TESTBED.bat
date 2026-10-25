@echo off
chcp 65001 >nul
title RepublicMP — Dual-Window Single-PC Co-op Testbed (Host + Client)

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "BUILD_DIR=%ROOT_DIR%\TesmioLoader\build"
set "HOST_DIR=%ROOT_DIR%\local\test-host"
set "CLIENT_DIR=%ROOT_DIR%\local\test-client"

echo ============================================================================
echo   RepublicMP Dual-Window Single-PC Testbed (Host Left + Client Right)
echo ============================================================================

if exist "%BUILD_DIR%\plugins\RepublicMP.dll" (
    echo [1/3] Синхронизация свежих DLL из TesmioLoader\build...
    copy /Y "%BUILD_DIR%\plugins\RepublicMP.dll" "%HOST_DIR%\plugins\RepublicMP.dll" >nul
    copy /Y "%BUILD_DIR%\tesmioloader.dll" "%HOST_DIR%\tesmioloader.dll" >nul
    copy /Y "%BUILD_DIR%\dinput8.dll" "%HOST_DIR%\dinput8.dll" >nul
    copy /Y "%BUILD_DIR%\tesmiolauncher.exe" "%HOST_DIR%\tesmiolauncher.exe" >nul

    copy /Y "%BUILD_DIR%\plugins\RepublicMP.dll" "%CLIENT_DIR%\plugins\RepublicMP.dll" >nul
    copy /Y "%BUILD_DIR%\tesmioloader.dll" "%CLIENT_DIR%\tesmioloader.dll" >nul
    copy /Y "%BUILD_DIR%\dinput8.dll" "%CLIENT_DIR%\dinput8.dll" >nul
    copy /Y "%BUILD_DIR%\tesmiolauncher.exe" "%CLIENT_DIR%\tesmiolauncher.exe" >nul
)

set "AUTO_FLAG=1"
if /I "%~1"=="--menu" set "AUTO_FLAG=0"

echo [2/3] Запуск окна ХОСТА (слева, Западная ССР)...
pushd "%HOST_DIR%"
set "REPUBLICMP_ROLE=HOST"
set "REPUBLICMP_AUTO_HOST=%AUTO_FLAG%"
set "REPUBLICMP_AUTO_JOIN=0"
start "[HOST] Soviet Republic MP" "%HOST_DIR%\SOVIET64.exe"
popd

timeout /t 2 /nobreak >nul

echo [3/3] Запуск окна КЛИЕНТА (справа, Восточная ССР -> 127.0.0.1)...
pushd "%CLIENT_DIR%"
set "REPUBLICMP_ROLE=CLIENT"
set "REPUBLICMP_AUTO_HOST=0"
set "REPUBLICMP_AUTO_JOIN=%AUTO_FLAG%"
start "[CLIENT] Soviet Republic MP" "%CLIENT_DIR%\SOVIET64.exe"
popd

echo.
echo Готово! Оба окна запущены бок о бок (1280x720 каждое).
if "%AUTO_FLAG%"=="1" (
    echo Хост автоматически загрузит последнее кооп-сохранение и откроет сервер :5556,
    echo а Клиент автоматически подключится к 127.0.0.1:5556!
    echo Для запуска без авто-входа используйте: LAUNCH-DUAL-TESTBED.bat --menu
)
