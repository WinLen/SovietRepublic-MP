@echo off
chcp 65001 >nul
title RepublicMP — Тестовый КЛИЕНТ (Правое окно, 127.0.0.1)

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "BUILD_DIR=%ROOT_DIR%\TesmioLoader\build"
set "CLIENT_DIR=%ROOT_DIR%\local\test-client"

if exist "%BUILD_DIR%\plugins\RepublicMP.dll" (
    copy /Y "%BUILD_DIR%\plugins\RepublicMP.dll" "%CLIENT_DIR%\plugins\RepublicMP.dll" >nul
    copy /Y "%BUILD_DIR%\tesmioloader.dll" "%CLIENT_DIR%\tesmioloader.dll" >nul
    copy /Y "%BUILD_DIR%\dinput8.dll" "%CLIENT_DIR%\dinput8.dll" >nul
    copy /Y "%BUILD_DIR%\tesmiolauncher.exe" "%CLIENT_DIR%\tesmiolauncher.exe" >nul
)

pushd "%CLIENT_DIR%"
set "REPUBLICMP_ROLE=CLIENT"
set "REPUBLICMP_AUTO_HOST=0"
set "REPUBLICMP_AUTO_JOIN=0"
if /I "%~1"=="--auto" set "REPUBLICMP_AUTO_JOIN=1"
start "[CLIENT] Soviet Republic MP" "%CLIENT_DIR%\SOVIET64.exe"
popd
