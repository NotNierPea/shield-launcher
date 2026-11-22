@echo off
rem one command for an update: tools\ship.bat [version] [--dry-run], see tools\ship.py
cd /d "%~dp0.."
python tools\ship.py %*
echo.
pause
