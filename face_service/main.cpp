#include "FaceService.h"
#include "../common/logger.h"
#include <cstdio>

// Named mutex to prevent multiple instances
static constexpr wchar_t SINGLE_INSTANCE_MUTEX[] =
    L"Global\\FaceLoginService_SingleInstance";

// Entry point for the Windows service executable.
// Usage:
//   FaceLoginService.exe                    — Run as service (SCM entry)
//   FaceLoginService.exe -install           — Install the service
//   FaceLoginService.exe -uninstall         — Uninstall the service
//   FaceLoginService.exe -standalone        — Run in foreground (for testing)

int wmain(int argc, wchar_t* argv[]) {
    // Prevent multiple instances
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, SINGLE_INSTANCE_MUTEX);
    if (hMutex == nullptr) {
        wprintf(L"ERROR: Failed to create singleton mutex (error %lu).\n",
                GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        wprintf(L"FaceLoginService is already running.\n");
        return 0;
    }
    // Mutex held for the lifetime of this process — released on exit.

    // Parse command line
    bool installMode = false;
    bool uninstallMode = false;
    bool standaloneMode = false;

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"-install") == 0 || _wcsicmp(argv[i], L"/install") == 0) {
            installMode = true;
        }
        else if (_wcsicmp(argv[i], L"-uninstall") == 0 || _wcsicmp(argv[i], L"/uninstall") == 0) {
            uninstallMode = true;
        }
        else if (_wcsicmp(argv[i], L"-standalone") == 0 || _wcsicmp(argv[i], L"/standalone") == 0) {
            standaloneMode = true;
        }
    }

    // Handle install/uninstall
    if (installMode) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (facelogin::FaceService::Install(exePath)) {
            wprintf(L"FaceLoginService installed successfully.\n");
            wprintf(L"Start with: sc start FaceLoginService\n");
            return 0;
        }
        wprintf(L"ERROR: Failed to install FaceLoginService.\n");
        return 1;
    }

    if (uninstallMode) {
        if (facelogin::FaceService::Uninstall()) {
            wprintf(L"FaceLoginService uninstalled successfully.\n");
            return 0;
        }
        wprintf(L"ERROR: Failed to uninstall FaceLoginService.\n");
        return 1;
    }

    // Standalone mode (foreground, for testing)
    if (standaloneMode) {
        wprintf(L"Running FaceLoginService in standalone (foreground) mode...\n");
        wprintf(L"Press Ctrl+C to stop.\n\n");

        facelogin::Logger::Instance().SetEnableDebugOutput(true);
        facelogin::Logger::Instance().SetMinLevel(facelogin::LogLevel::Debug);

        // The service entry point starts its own loop
        facelogin::FaceService::RunStandalone();
        return 0;
    }

    // Default: run as a Windows service via SCM
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(L"FaceLoginService"),
          facelogin::FaceService::ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Not started by SCM — print usage
            wprintf(L"FaceLoginService — Custom Face Recognition Login for Windows\n\n");
            wprintf(L"Usage:\n");
            wprintf(L"  FaceLoginService.exe                 Run as Windows service\n");
            wprintf(L"  FaceLoginService.exe -install        Install the service\n");
            wprintf(L"  FaceLoginService.exe -uninstall      Uninstall the service\n");
            wprintf(L"  FaceLoginService.exe -standalone     Run in foreground (testing)\n");
            return 0;
        }
        return static_cast<int>(err);
    }

    return 0;
}
