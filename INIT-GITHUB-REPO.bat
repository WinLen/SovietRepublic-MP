@echo off
setlocal
cd /d "%~dp0"

echo =====================================================================
echo   Preparing clean Git repository for GitHub + GitHub Actions CI/CD
echo =====================================================================

rem Remove nested .git folders inside TesmioLoader / RepublicMP so the root repo tracks everything as one repository
if exist "TesmioLoader\.git" (
    echo Removing nested TesmioLoader\.git metadata...
    rmdir /s /q "TesmioLoader\.git"
)
if exist "TesmioLoader\RepublicMP\.git" (
    echo Removing nested TesmioLoader\RepublicMP\.git metadata...
    rmdir /s /q "TesmioLoader\RepublicMP\.git"
)

if not exist ".git" (
    git init -b main
)

git add .
git status -s

echo.
echo [OK] Repository staged! Next commands to publish and trigger v1.0.0 release:
echo   git commit -m "Release RepublicMP v1.0.0 (Lockstep + Two States + Steam P2P + Auto-Updater)"
echo   git remote add origin https://github.com/YOUR_USERNAME/YOUR_REPO.git
echo   git push -u origin main
echo   git tag v1.0.0
echo   git push origin v1.0.0
echo.
pause
