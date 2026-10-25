@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_checks.ps1"
exit /b %ERRORLEVEL%
