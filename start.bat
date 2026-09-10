@echo off
setlocal
cd /d "%~dp0"
if not exist "bin\lamacpp-local.exe" (
  echo Missing bin\lamacpp-local.exe. Build from src\lamacpp_local.cpp with MSVC first.
  pause
  exit /b 1
)
set "CMD=%~1"
if /I "%CMD%"=="run" goto direct
if /I "%CMD%"=="start" goto direct
if /I "%CMD%"=="stop" goto direct
if /I "%CMD%"=="status" goto direct
if /I "%CMD%"=="switch" goto direct
if /I "%CMD%"=="bench" goto direct
if /I "%CMD%"=="compare" goto direct
if /I "%CMD%"=="longtest" goto direct
if /I "%CMD%"=="proxy" goto direct
if /I "%CMD%"=="downloads" goto direct
if /I "%CMD%"=="firewall" goto direct

"%~dp0bin\lamacpp-local.exe" run %*
goto done

:direct
"%~dp0bin\lamacpp-local.exe" %*

:done
if errorlevel 1 pause
