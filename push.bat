@echo off
setlocal
cd /d "%~dp0"

node src\distribute.cjs
if errorlevel 1 goto failed
echo.
echo Finished.
pause
exit /b 0

:failed
echo.
echo Failed. Read the error above before retrying.
pause
exit /b 1
