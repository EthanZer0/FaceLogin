@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   FaceLogin - Installation Script
echo   Custom Face Recognition Login for Windows
echo ============================================
echo.

:: Check administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click install.bat and select "Run as administrator".
    pause
    exit /b 1
)

:: Determine paths
set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%..\build"
set "MODELS_DIR=%SCRIPT_DIR%..\models"
set "ASSETS_DIR=%SCRIPT_DIR%..\assets"

set "INSTALL_DIR=%ProgramFiles%\FaceLogin"
set "DATA_DIR=%ProgramData%\FaceLogin"
set "MODELS_DEST=%DATA_DIR%\models"

:: Get the current interactive user (not SYSTEM)
for /f "tokens=2 delims=\" %%a in ('whoami /user /fo csv ^| findstr /v "S-1-5-18"') do (
    :: Just get the username
)
set "CURRENT_USER=%USERNAME%"

echo Installation Paths:
echo   Script:      %SCRIPT_DIR%
echo   Install:     %INSTALL_DIR%
echo   Data:        %DATA_DIR%
echo   Models:      %MODELS_DEST%
echo   User:        %CURRENT_USER%
echo.

:: Create directories
echo [1/8] Creating directories...
mkdir "%INSTALL_DIR%" 2>nul
mkdir "%DATA_DIR%" 2>nul
mkdir "%MODELS_DEST%" 2>nul
echo   Done.

:: Copy service executable
echo [2/8] Installing FaceLogin Service...
if exist "%BUILD_DIR%\face_service\Release\FaceLoginService.exe" (
    copy /Y "%BUILD_DIR%\face_service\Release\FaceLoginService.exe" "%INSTALL_DIR%\" >nul
    echo   Service binary copied.
) else if exist "%BUILD_DIR%\face_service\FaceLoginService.exe" (
    copy /Y "%BUILD_DIR%\face_service\FaceLoginService.exe" "%INSTALL_DIR%\" >nul
    echo   Service binary copied.
) else (
    echo   WARNING: FaceLoginService.exe not found in build directory.
    echo   Please build the project first (cmake --build build --config Release).
)

:: Copy credential provider DLL
echo [3/8] Installing Credential Provider...
if exist "%BUILD_DIR%\credential_provider\Release\FaceLoginCredentialProvider.dll" (
    copy /Y "%BUILD_DIR%\credential_provider\Release\FaceLoginCredentialProvider.dll" "%SystemRoot%\System32\" >nul
    echo   DLL copied to System32.
) else if exist "%BUILD_DIR%\credential_provider\FaceLoginCredentialProvider.dll" (
    copy /Y "%BUILD_DIR%\credential_provider\FaceLoginCredentialProvider.dll" "%SystemRoot%\System32\" >nul
    echo   DLL copied to System32.
) else (
    echo   WARNING: FaceLoginCredentialProvider.dll not found in build directory.
    echo   Please build the project first.
)

:: Copy enrollment application
echo [4/8] Installing Enrollment Application...
if exist "%BUILD_DIR%\enrollment_app\Release\FaceLoginEnrollment.exe" (
    copy /Y "%BUILD_DIR%\enrollment_app\Release\FaceLoginEnrollment.exe" "%INSTALL_DIR%\" >nul
    echo   Enrollment app copied.
) else if exist "%BUILD_DIR%\enrollment_app\FaceLoginEnrollment.exe" (
    copy /Y "%BUILD_DIR%\enrollment_app\FaceLoginEnrollment.exe" "%INSTALL_DIR%\" >nul
    echo   Enrollment app copied.
)

:: Copy model files
echo [5/8] Copying model files...
if exist "%MODELS_DIR%\shape_predictor_68_face_landmarks.dat" (
    copy /Y "%MODELS_DIR%\shape_predictor_68_face_landmarks.dat" "%MODELS_DEST%\" >nul
    echo   shape_predictor_68_face_landmarks.dat copied.
) else (
    echo   WARNING: shape_predictor_68_face_landmarks.dat not found.
    echo   Run download_models.ps1 to download it.
)

if exist "%MODELS_DIR%\dlib_face_recognition_resnet_model_v1.dat" (
    copy /Y "%MODELS_DIR%\dlib_face_recognition_resnet_model_v1.dat" "%MODELS_DEST%\" >nul
    echo   dlib_face_recognition_resnet_model_v1.dat copied.
) else (
    echo   WARNING: dlib_face_recognition_resnet_model_v1.dat not found.
    echo   Run download_models.ps1 to download it.
)

:: Set secure ACLs on data directory (allow SYSTEM + Administrators + current user)
echo [6/8] Setting security permissions...
icacls "%DATA_DIR%" /inheritance:r /grant "SYSTEM:(OI)(CI)F" /grant "BUILTIN\Administrators:(OI)(CI)F" >nul 2>&1
icacls "%DATA_DIR%" /grant "%USERDOMAIN%\%CURRENT_USER%:(OI)(CI)F" >nul 2>&1
echo   ACLs set: SYSTEM + Administrators + %CURRENT_USER%.

:: Register credential provider
echo [7/8] Registering credential provider...
regsvr32 /s "%SystemRoot%\System32\FaceLoginCredentialProvider.dll"
if %errorlevel% equ 0 (
    echo   Credential provider registered successfully.
) else (
    echo   WARNING: regsvr32 returned error code %errorlevel%.
    echo   You may need to run this step manually.
)

:: Remove any existing service (clean up SYSTEM-installed version)
echo [8/8] Setting up Windows service...
sc stop FaceLoginService >nul 2>&1
timeout /t 2 /nobreak >nul
sc delete FaceLoginService >nul 2>&1
timeout /t 1 /nobreak >nul

:: Install service as LocalSystem (Session 0).
:: DirectShow camera capture enables webcam access from system services.
sc create FaceLoginService binPath= "\"%INSTALL_DIR%\FaceLoginService.exe\"" start= auto obj= LocalSystem >nul 2>&1

if %errorlevel% equ 0 (
    echo   Service created.
) else (
    echo   Service may already exist (that's OK).
)

sc description FaceLoginService "FaceLogin - Custom face recognition authentication for Windows login" >nul 2>&1
sc failure FaceLoginService reset= 86400 actions= restart/60000/restart/60000/restart/60000 >nul 2>&1

sc start FaceLoginService >nul 2>&1
if %errorlevel% equ 0 (
    echo   Service started.
) else (
    echo   NOTE: Service could not start. Check Event Viewer for details.
    echo   To run in standalone mode instead: FaceLoginService.exe -standalone
)

echo.
echo ============================================
echo   Installation Complete!
echo ============================================
echo.
echo Next steps:
echo   1. Run FaceLoginEnrollment.exe to register your face
echo      (requires Administrator privileges)
echo   2. Press Win+L to lock and test face login
echo.
echo If the service won't start (camera access issue):
echo   - Run FaceLoginService.exe -standalone instead
echo   - The password login provider is always available as fallback
echo   - Click "Switch to password login" on the lock screen
echo   - To uninstall, run uninstall.bat as Administrator
echo.
echo Log files: %DATA_DIR%\*.log
echo.

pause
