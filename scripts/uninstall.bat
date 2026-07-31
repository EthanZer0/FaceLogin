@echo off
setlocal

echo ============================================
echo   FaceLogin - Uninstallation Script
echo ============================================
echo.

:: Check administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click uninstall.bat and select "Run as administrator".
    pause
    exit /b 1
)

set "INSTALL_DIR=%ProgramFiles%\FaceLogin"
set "DATA_DIR=%ProgramData%\FaceLogin"

echo [1/5] Stopping FaceLogin service...
sc stop FaceLoginService >nul 2>&1
timeout /t 3 /nobreak >nul
echo   Done.

echo [2/5] Deleting FaceLogin service...
sc delete FaceLoginService >nul 2>&1
echo   Done.

echo [3/5] Unregistering credential provider...
regsvr32 /s /u "%SystemRoot%\System32\FaceLoginCredentialProvider.dll" 2>nul
echo   Done.

echo [4/5] Removing installed files...
del /F "%SystemRoot%\System32\FaceLoginCredentialProvider.dll" 2>nul
echo   Removed DLL from System32.

if exist "%INSTALL_DIR%" (
    del /F /Q "%INSTALL_DIR%\*" 2>nul
    rmdir /S /Q "%INSTALL_DIR%" 2>nul
    echo   Removed install directory.
)

echo [5/5] Cleaning up...
echo   Data directory (%DATA_DIR%) has been preserved.
echo   To remove it manually: rmdir /S /Q "%DATA_DIR%"

echo.
echo ============================================
echo   Uninstallation Complete
echo ============================================
echo.
echo Your login will now use the default Windows password provider.
echo The data directory with encrypted credentials is still at:
echo   %DATA_DIR%
echo.
echo Delete it manually if you no longer need it.
echo.

pause
