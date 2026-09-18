#pragma once

#include <windows.h>
#include <string>

// Some common-library translation units intentionally target an older Windows
// SDK contract, so the SDK header may hide this Windows 7+ API even though the
// application itself requires a newer Windows version.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
extern "C" ULONGLONG WINAPI GetTickCount64(void);
#endif

// Registry key used for all FaceLogin configuration
const wchar_t FACELOGIN_REG_KEY[] = L"SOFTWARE\\FaceLogin";

// Value names
const wchar_t REGVAL_DATA_PATH[]   = L"DataPath";
const wchar_t REGVAL_INSTALL_PATH[] = L"InstallPath";
// Internal login-entry generation. The service advances it from a Kernel-Boot
// evidence record or a console logoff. The credential provider claims each
// generation at most once for automatic recognition.
const wchar_t REGVAL_LOGIN_ENTRY_GENERATION[] = L"LoginEntryGeneration";
const wchar_t REGVAL_AUTO_ATTEMPT_GENERATION[] = L"AutoAttemptGeneration";
const wchar_t REGVAL_LOGIN_ENTRY_ACTIVE[] = L"LoginEntryActive";
const wchar_t REGVAL_LOGIN_ENTRY_SESSION[] = L"LoginEntrySession";
const wchar_t REGVAL_LOGIN_ENTRY_BOOT_RECORD[] = L"LoginEntryBootRecord";
const wchar_t REGVAL_LAST_KERNEL_BOOT_RECORD[] = L"LastKernelBootRecord";
// Mirrored from config.cold_boot_key_trigger by the Console's SetConfig so
// the credential provider (running inside LogonUI, which cannot reach
// config.json reliably) knows whether cold-boot recognition needs a key press.
const wchar_t REGVAL_COLD_BOOT_KEY_TRIGGER[] = L"ColdBootKeyTrigger";

// Read a REG_SZ value from HKLM\SOFTWARE\FaceLogin.
// Returns defaultValue if the key/value is missing or not a string.
inline std::wstring ReadRegString(const wchar_t* valueName,
                                  const std::wstring& defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY,
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, valueName, nullptr, &type,
                              reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS
            && type == REG_SZ)
        {
            RegCloseKey(hKey);
            return buf;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

// Read a REG_DWORD value from HKLM\SOFTWARE\FaceLogin.
// Returns defaultVal if the key/value is missing or not a DWORD.
inline DWORD ReadRegDword(const wchar_t* valueName, DWORD defaultVal)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY,
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD val = 0, size = sizeof(val), type = 0;
        if (RegQueryValueExW(hKey, valueName, nullptr, &type,
                              reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS
            && type == REG_DWORD)
        {
            RegCloseKey(hKey);
            return val;
        }
        RegCloseKey(hKey);
    }
    return defaultVal;
}

// Write a REG_DWORD value to HKLM\SOFTWARE\FaceLogin.
inline bool WriteRegDword(const wchar_t* valueName, DWORD val)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY,
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
            &hKey, nullptr) == ERROR_SUCCESS)
    {
        bool ok = (RegSetValueExW(hKey, valueName, 0, REG_DWORD,
                      reinterpret_cast<const BYTE*>(&val),
                      sizeof(val)) == ERROR_SUCCESS);
        RegCloseKey(hKey);
        return ok;
    }
    return false;
}

// Read a REG_QWORD value from HKLM\SOFTWARE\FaceLogin.
// Returns defaultVal if the key/value is missing or not a QWORD.
inline ULONGLONG ReadRegQword(const wchar_t* valueName, ULONGLONG defaultVal)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY,
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        ULONGLONG val = 0;
        DWORD size = sizeof(val), type = 0;
        if (RegQueryValueExW(hKey, valueName, nullptr, &type,
                              reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS
            && type == REG_QWORD)
        {
            RegCloseKey(hKey);
            return val;
        }
        RegCloseKey(hKey);
    }
    return defaultVal;
}

// Write a REG_QWORD value to HKLM\SOFTWARE\FaceLogin.
inline bool WriteRegQword(const wchar_t* valueName, ULONGLONG val)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY,
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
            &hKey, nullptr) == ERROR_SUCCESS)
    {
        bool ok = (RegSetValueExW(hKey, valueName, 0, REG_QWORD,
                      reinterpret_cast<const BYTE*>(&val),
                      sizeof(val)) == ERROR_SUCCESS);
        RegCloseKey(hKey);
        return ok;
    }
    return false;
}
