@echo off
chcp 65001 >nul
title RepublicMP — Тестовый ХОСТ (Левое окно)

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "BUILD_DIR=%ROOT_DIR%\TesmioLoader\build"
set "HOST_DIR=%ROOT_DIR%\local\test-host"

if exist "%BUILD_DIR%\plugins\RepublicMP.dll" (
    copy /Y "%BUILD_DIR%\plugins\RepublicMP.dll" "%HOST_DIR%\plugins\RepublicMP.dll" >nul
    copy /Y "%BUILD_DIR%\tesmioloader.dll" "%HOST_DIR%\tesmioloader.dll" >nul
    copy /Y "%BUILD_DIR%\dinput8.dll" "%HOST_DIR%\dinput8.dll" >nul
    copy /Y "%BUILD_DIR%\tesmiolauncher.exe" "%HOST_DIR%\tesmiolauncher.exe" >nul
)

pushd "%HOST_DIR%"
set "REPUBLICMP_ROLE=HOST"
set "REPUBLICMP_AUTO_HOST=0"
set "REPUBLICMP_AUTO_JOIN=0"
if /I "%~1"=="--auto" set "REPUBLICMP_AUTO_HOST=1"
start "[HOST] Soviet Republic MP" "%HOST_DIR%\SOVIET64.exe"
popd
