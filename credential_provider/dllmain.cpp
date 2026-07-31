#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <initguid.h>
#include <credentialprovider.h>
#include <string>

#include "FaceLoginProvider.h"
#include "resource.h"
#include "../common/logger.h"
#include "../common/registry_util.h"

// ============================================================================
// Module state
// ============================================================================

HMODULE g_hModule = nullptr;
static LONG g_cRefCount = 0;
static LONG g_cLockCount = 0;

// ============================================================================
// GUIDs
// ============================================================================

// CLSID_FaceLoginProvider defined in resource.h/initguid

// ============================================================================
// Class Factory
// ============================================================================

class FaceLoginClassFactory : public IClassFactory {
public:
    FaceLoginClassFactory() = default;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() {
        LONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    // IClassFactory
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) {
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        FaceLoginProvider* pProvider = new (std::nothrow) FaceLoginProvider();
        if (!pProvider) return E_OUTOFMEMORY;

        HRESULT hr = pProvider->QueryInterface(riid, ppv);
        if (FAILED(hr)) delete pProvider;

        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) {
        if (fLock) {
            InterlockedIncrement(&g_cLockCount);
        } else {
            InterlockedDecrement(&g_cLockCount);
        }
        return S_OK;
    }

private:
    LONG m_refCount = 1;
};

// ============================================================================
// DLL Entry Point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    UNREFERENCED_PARAMETER(lpReserved);

    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);

        // Initialize logger — in credential provider context,
        // we log to a file since we can't use stdout
        {
            std::wstring logDir = ReadRegString(REGVAL_DATA_PATH, L"");
            if (logDir.empty()) {
                wchar_t programData[MAX_PATH];
                if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                              nullptr, 0, programData))) {
                    logDir = std::wstring(programData) + L"\\FaceLogin";
                } else {
                    logDir = L"C:\\ProgramData\\FaceLogin";
                }
            }
            std::wstring logPath = logDir + L"\\log\\credential_provider.log";
            facelogin::Logger::Instance().SetLogFile(logPath);
            facelogin::Logger::Instance().SetMinLevel(facelogin::LogLevel::Info);
            FACELOGIN_INFO(L"=== FaceLogin Credential Provider DLL loaded ===");
        }
        break;

    case DLL_PROCESS_DETACH:
        FACELOGIN_INFO(L"=== FaceLogin Credential Provider DLL unloaded ===");
        break;
    }

    return TRUE;
}

// ============================================================================
// COM Exports
// ============================================================================

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    *ppv = nullptr;

    if (rclsid != CLSID_FaceLoginProvider) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    FaceLoginClassFactory* pFactory = new (std::nothrow) FaceLoginClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;

    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release(); // QI already added ref for caller
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_cRefCount == 0 && g_cLockCount == 0) ? S_OK : S_FALSE;
}

// ============================================================================
// Registry Registration / Unregistration
// ============================================================================

static const wchar_t* CLSID_STRING = L"{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}";
static const wchar_t* PROVIDER_NAME = L"FaceLogin Credential Provider";

STDAPI DllRegisterServer() {
    // Register under HKEY_CLASSES_ROOT\CLSID\{GUID}
    wchar_t clsidKey[MAX_PATH];
    swprintf_s(clsidKey, L"CLSID\\%s", CLSID_STRING);

    HKEY hKey = nullptr;
    DWORD disp;

    // CLSID
    LONG result = RegCreateKeyExW(HKEY_CLASSES_ROOT, clsidKey,
                                  0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE, nullptr, &hKey, &disp);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);

    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(PROVIDER_NAME),
                   static_cast<DWORD>((wcslen(PROVIDER_NAME) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);

    // CLSID\{GUID}\InprocServer32
    wchar_t inprocKey[MAX_PATH];
    swprintf_s(inprocKey, L"CLSID\\%s\\InprocServer32", CLSID_STRING);

    result = RegCreateKeyExW(HKEY_CLASSES_ROOT, inprocKey,
                             0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_WRITE, nullptr, &hKey, &disp);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);

    // Get the full path to our DLL
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);

    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(dllPath),
                   static_cast<DWORD>((wcslen(dllPath) + 1) * sizeof(wchar_t)));

    // ThreadingModel
    static const wchar_t threadingModel[] = L"Apartment";
    RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(threadingModel),
                   sizeof(threadingModel));

    RegCloseKey(hKey);

    // Register under HKLM\...\Credential Providers\{GUID}
    wchar_t cpKey[MAX_PATH];
    swprintf_s(cpKey,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\%s",
        CLSID_STRING);

    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, cpKey,
                             0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_WRITE, nullptr, &hKey, &disp);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);

    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(PROVIDER_NAME),
                   static_cast<DWORD>((wcslen(PROVIDER_NAME) + 1) * sizeof(wchar_t)));

    // Don't set Disabled=0 by default — absence of key = enabled

    RegCloseKey(hKey);

    FACELOGIN_INFO(L"Credential provider registered successfully");
    FACELOGIN_INFO(L"  CLSID: %s", CLSID_STRING);
    FACELOGIN_INFO(L"  DLL: %s", dllPath);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    // Remove CLSID registration
    wchar_t clsidKey[MAX_PATH];
    swprintf_s(clsidKey, L"CLSID\\%s", CLSID_STRING);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey);

    // Remove credential provider registration
    wchar_t cpKey[MAX_PATH];
    swprintf_s(cpKey,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\%s",
        CLSID_STRING);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, cpKey);

    FACELOGIN_INFO(L"Credential provider unregistered");
    return S_OK;
}
