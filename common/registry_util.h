#pragma once

#include <windows.h>
#include <string>

// Registry key used for all FaceLogin configuration
const wchar_t FACELOGIN_REG_KEY[] = L"SOFTWARE\\FaceLogin";

// Value names
const wchar_t REGVAL_DATA_PATH[]   = L"DataPath";
const wchar_t REGVAL_INSTALL_PATH[] = L"InstallPath";
const wchar_t REGVAL_USER_LOGGED_IN[] = L"UserLoggedIn";
const wchar_t REGVAL_SERVICE_START_UPTIME[] = L"ServiceStartUptime";
// Mirrored from config.cold_boot_key_trigger by the Console's SetConfig so
// the credential provider (running inside LogonUI, which cannot reach
// config.json reliably) knows whether cold-boot recognition needs a key press.
const wchar_t REGVAL_COLD_BOOT_KEY_TRIGGER[] = L"ColdBootKeyTrigger";
// Set once by the service/console when a camera was caught in a broken manual
// exposure state (severe overexposure that Set(Auto) cannot undo — a driver
// bug). While set, the exposure controller skips the camera channel entirely
// and runs digital gain only, so the camera is never re-poisoned. The user
// must restart the machine (or the camera driver) to recover the camera, and
// may clear this value manually after that.
const wchar_t REGVAL_EXPOSURE_HW_BROKEN[] = L"ExposureHardwareBroken";

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
