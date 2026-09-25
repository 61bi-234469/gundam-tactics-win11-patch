@echo off
rem Gundam Tactics Win11 patch v1.2.0 - double-click to revert the patch.
rem Drag and drop the game folder onto this file to choose it directly.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0patch_files\install.ps1" -Mode uninstall %*
echo.
pause
