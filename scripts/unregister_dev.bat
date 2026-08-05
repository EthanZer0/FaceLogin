@echo off
setlocal enabledelayedexpansion

REM ============================================================================
REM FaceLogin - Unregister dev credential provider
REM
REM Undoes what scripts\start_standalone.bat did, so the "Face Login" tile
REM stops appearing on the lock screen:
REM   1. Kills any running FaceLoginService (standalone process / SCM service)
REM   2. Unregisters the credential provider DLL (regsvr32 /u)
REM   3. Removes leftover registry registrations (safety net -- regsvr32 /u
REM      can't run if the DLL is already gone, and reg delete covers that)
REM   4. Deletes the credential provider DLL from System32
REM   5. Cleans up runtime registry keys under HKLM\SOFTWARE\FaceLogin
REM
REM Every step is ALSO appended to %TEMP%\FaceLogin_unregister.log, so if the
REM window flashes closed again you can read the log to see exactly how far the
REM script got before dying.
REM
REM This is a dev-cleanup script only -- it does NOT touch the formal
REM installer (FaceLoginSetup.exe). Re-run scripts\start_standalone.bat to
REM re-register for testing.
REM
REM (NOTE: keep this file pure-ASCII -- cmd.exe on a GBK codepage chokes on
REM  multi-byte UTF-8 chars like em-dash and the whole script silently mis-parses.)
REM ============================================================================

set "LOG=%TEMP%\FaceLogin_unregister.log"
> "%LOG%" echo FaceLogin unregister started: %DATE% %TIME%

REM ---- Admin self-check ----
net session >nul 2>&1
if %errorlevel% neq 0 (
    call :note "[ERROR] This script must be run as Administrator."
    call :note "Right-click the .bat and choose 'Run as administrator'."
    echo.
    pause
    exit /b 1
)

echo.
echo ============================================
echo   FaceLogin - Unregister Dev Credential Provider
echo ============================================
echo.

set "DLL=%SystemRoot%\System32\FaceLoginCredentialProvider.dll"
set "CLSID={B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}"
set "CPKEY=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\%CLSID%"
set "CLSIDKEY=HKLM\SOFTWARE\Classes\CLSID\%CLSID%"

REM ---- [1/5] Kill all running FaceLoginService instances ----
call :note "[1/5] Killing FaceLoginService instances..."
taskkill /F /IM FaceLoginService.exe >nul 2>&1
if !errorlevel! equ 0 (
    call :note "  Killed running instance(s)."
) else (
    call :note "  No running instance found (ok)."
)
sc stop FaceLoginService >nul 2>&1
sc delete FaceLoginService >nul 2>&1
REM Sleep WITHOUT timeout.exe -- timeout can abort the whole batch when stdin
REM is redirected. ping never fails, so it can't kill the script.
ping -n 3 127.0.0.1 >nul

REM ---- [2/5] Unregister credential provider DLL ----
call :note "[2/5] Unregistering credential provider DLL..."
if exist "%DLL%" (
    regsvr32 /s /u "%DLL%"
    if !errorlevel! equ 0 (
        call :note "  Unregistered successfully."
    ) else (
        call :note "  [WARNING] regsvr32 /u returned error !errorlevel!."
        call :note "  Registry keys will be removed manually next."
    )
) else (
    call :note "  [WARNING] %DLL% not found -- keys removed manually next."
)

REM ---- [3/5] Remove leftover registry registrations (safety net) ----
call :note "[3/5] Removing leftover registry registrations..."
reg delete "%CLSIDKEY%" /f >nul 2>&1
if !errorlevel! equ 0 (
    call :note "  Removed CLSID key."
) else (
    call :note "  CLSID key already gone (ok)."
)
reg delete "%CPKEY%" /f >nul 2>&1
if !errorlevel! equ 0 (
    call :note "  Removed Credential Providers key."
) else (
    call :note "  Credential Providers key already gone (ok)."
)

REM ---- [4/5] Delete credential provider DLL from System32 ----
call :note "[4/5] Deleting credential provider DLL from System32..."
if exist "%DLL%" (
    del /F "%DLL%" >nul 2>&1
    if !errorlevel! equ 0 (
        call :note "  Deleted %DLL%"
    ) else (
        call :note "  [ERROR] Could not delete %DLL%."
        call :note "  LogonUI / the lock screen may still be holding it. Log off"
        call :note "  fully or reboot, then delete it manually: del %DLL%"
    )
) else (
    call :note "  Already absent (ok)."
)

REM ---- [5/5] Clean up runtime registry keys (optional) ----
call :note "[5/5] Cleaning up runtime registry key HKLM\SOFTWARE\FaceLogin..."
reg delete "HKLM\SOFTWARE\FaceLogin" /f >nul 2>&1
if !errorlevel! equ 0 (
    call :note "  Removed runtime registry key."
) else (
    call :note "  No runtime key found (ok)."
)

REM ---- Verify ----
reg query "%CLSIDKEY%" >nul 2>&1
if !errorlevel! equ 0 (
    call :note "[VERIFY] CLSID registration STILL PRESENT -- something went wrong."
) else (
    call :note "[VERIFY] CLSID registration removed. Tile should be gone from lock screen."
)

echo.
echo  Log written to: %LOG%
echo ============================================
echo  Done. The "Face Login" tile will no longer
echo  appear on the lock screen.
echo ============================================
echo.
pause
exit /b 0

:note
echo %~1
echo %~1>> "%LOG%"
exit /b 0
