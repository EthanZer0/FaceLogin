@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   FaceLogin - Start Service in Standalone Mode
echo ============================================
echo.

:: Kill all existing zombie instances
echo [1/3] Killing all zombie FaceLoginService instances...
taskkill /F /IM FaceLoginService.exe 2>nul
timeout /t 2 /nobreak >nul
echo   Done.

:: Copy credential provider DLL
echo [2/3] Copying credential provider DLL to System32...
copy /Y "%~dp0..\build\credential_provider\Release\FaceLoginCredentialProvider.dll" "%SystemRoot%\System32\" >nul 2>&1
echo   Done.

:: Register credential provider
echo [3/3] Registering credential provider DLL...
regsvr32 /s "%SystemRoot%\System32\FaceLoginCredentialProvider.dll"
if %errorlevel% equ 0 (
    echo   Registered successfully.
) else (
    echo   WARNING: regsvr32 returned error %errorlevel%.
)

echo.
echo ============================================
echo Now starting FaceLoginService in standalone mode.
echo Keep this window open while testing lock screen.
echo Press Ctrl+C to stop.
echo ============================================
echo.

"%~dp0..\build\face_service\Release\FaceLoginService.exe" -standalone

pause
