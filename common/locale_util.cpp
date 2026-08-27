// WTSUserSID (WTSQuerySessionInformation info class) requires Win8+ SDK.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WINVER
#define WINVER 0x0602
#endif

#include "locale_util.h"
#include "config_util.h"
#include "logger.h"

#include <windows.h>
#include <wtsapi32.h>
#include <sddl.h>       // ConvertSidToStringSidW
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace facelogin {
namespace {

std::string ReadUtf8File(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()),
                                          nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::string NormalizeTag(std::string locale) {
    std::replace(locale.begin(), locale.end(), '_', '-');
    std::transform(locale.begin(), locale.end(), locale.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (locale == "ko" || locale.rfind("ko-", 0) == 0) return "ko-KR";
    if (locale == "zh" || locale.rfind("zh-", 0) == 0) return "zh-CN";
    return "zh-CN";
}

// The service (Session 0) and LogonUI run as SYSTEM, where
// GetUserDefaultLocaleName/GetUserDefaultUILanguage return the SYSTEM
// profile's values — not the signed-in user's display language. Resolve the
// interactive session's user instead and read their UI language from
// HKEY_USERS\<SID>\Control Panel\Desktop\PreferredUILanguages — the registry
// source behind GetUserDefaultUILanguage, written when the user switches the
// Windows display language.
//
// NOTE: WTSQueryUserToken is NOT usable here — it requires SE_TCB_NAME
// *enabled*, which LogonUI's token does not have at the lock screen
// (ERROR_PRIVILEGE_NOT_HELD, observed 08:34). The WTSUserSID info class is
// not exposed by the current SDK either, so resolve the session user via the
// classic WTSUserName + WTSDomainName pair and LookupAccountNameW (needs no
// special privilege). On the sign-in screen there is no user yet, and the
// value read is empty — both cases fall back to the caller's default.
std::string ReadInteractiveSessionUiLanguage() {
    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) return {};

    LPWSTR userName = nullptr;
    DWORD nameBytes = 0;
    LPWSTR domainName = nullptr;
    DWORD domainBytes = 0;
    const bool gotUser = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                                     WTSUserName, &userName, &nameBytes) &&
                         userName && *userName;
    const bool gotDomain = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                                       WTSDomainName, &domainName, &domainBytes) &&
                           domainName && *domainName;
    if (!gotUser || !gotDomain) {
        if (userName) WTSFreeMemory(userName);
        if (domainName) WTSFreeMemory(domainName);
        return {};
    }

    std::string result;
    DWORD sidSize = 0, domSize = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(domainName, userName, nullptr, &sidSize, nullptr, &domSize, &use);
    if (sidSize > 0) {
        std::vector<BYTE> sidBuf(sidSize);
        std::vector<wchar_t> domBuf(domSize > 0 ? domSize : 1);
        if (LookupAccountNameW(domainName, userName,
                               reinterpret_cast<PSID>(sidBuf.data()), &sidSize,
                               domBuf.data(), &domSize, &use)) {
            LPWSTR sidStr = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf.data()), &sidStr)) {
                // PreferredUILanguages is a VALUE under Control Panel\Desktop,
                // not a subkey.
                const std::wstring subKey = std::wstring(sidStr) +
                    L"\\Control Panel\\Desktop";
                LocalFree(sidStr);

                HKEY hKey = nullptr;
                if (RegOpenKeyExW(HKEY_USERS, subKey.c_str(), 0, KEY_READ,
                                  &hKey) == ERROR_SUCCESS) {
                    // REG_MULTI_SZ; the first entry is the active UI language
                    // (e.g. "ko-KR"), exactly what GetUserDefaultUILanguage
                    // returns for the owning user.
                    wchar_t langs[512] = {};
                    DWORD size = sizeof(langs), type = 0;
                    if (RegQueryValueExW(hKey, L"PreferredUILanguages", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(langs), &size) ==
                            ERROR_SUCCESS && type == REG_MULTI_SZ && *langs) {
                        const int len = WideCharToMultiByte(CP_UTF8, 0, langs, -1,
                                                            nullptr, 0, nullptr, nullptr);
                        if (len > 1) {
                            std::string locale(static_cast<size_t>(len), '\0');
                            WideCharToMultiByte(CP_UTF8, 0, langs, -1,
                                                locale.data(), len, nullptr, nullptr);
                            locale.pop_back();
                            result = locale;
                        }
                    }
                    RegCloseKey(hKey);
                }
            }
        }
    }
    if (userName) WTSFreeMemory(userName);
    if (domainName) WTSFreeMemory(domainName);
    return result;
}

} // namespace

std::string ResolveLocale(const std::string& preference) {
    if (!preference.empty() && preference != "auto") {
        const std::string tag = NormalizeTag(preference);
        FACELOGIN_INFO(L"[l10n] ResolveLocale: explicit '%hs' -> '%hs'",
                       preference.c_str(), tag.c_str());
        return tag;
    }

    // Prefer the interactive-session user's UI language — the auto mode must
    // follow the person in front of the lock screen, not the SYSTEM profile.
    // Falls back to the process default when no user is signed in yet or the
    // caller lacks the privilege to query the session token.
    const std::string uiLang = ReadInteractiveSessionUiLanguage();
    if (!uiLang.empty()) {
        const std::string tag = NormalizeTag(uiLang);
        FACELOGIN_INFO(L"[l10n] ResolveLocale: auto -> session user '%hs' -> '%hs'",
                       uiLang.c_str(), tag.c_str());
        return tag;
    }

    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, localeName, -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::string locale(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, localeName, -1,
                                locale.data(), size, nullptr, nullptr);
            locale.pop_back();
            const std::string tag = NormalizeTag(locale);
            FACELOGIN_INFO(L"[l10n] ResolveLocale: auto -> process default '%hs' -> '%hs'",
                           locale.c_str(), tag.c_str());
            return tag;
        }
    }
    FACELOGIN_INFO(L"[l10n] ResolveLocale: auto -> zh-CN (last resort)");
    return "zh-CN";
}

bool LocaleCatalog::Load(const std::wstring& installDir, const std::string& preference) {
    m_locale = ResolveLocale(preference);
    const std::wstring path = installDir + L"\\locales\\" + Utf8ToWide(m_locale) + L".json";
    m_json = ReadUtf8File(path);
    if (m_json.empty() && m_locale != "zh-CN") {
        m_locale = "zh-CN";
        m_json = ReadUtf8File(installDir + L"\\locales\\zh-CN.json");
        FACELOGIN_INFO(L"[l10n] LocaleCatalog::Load: '%hs' missing — fell back to zh-CN",
                       Utf8ToWide(preference).c_str());
    }
    FACELOGIN_INFO(L"[l10n] LocaleCatalog::Load: locale='%hs' file='%ls' bytes=%zu ok=%d",
                   m_locale.c_str(), path.c_str(), m_json.size(), !m_json.empty());
    return !m_json.empty();
}

std::string LocaleCatalog::Get(const std::string& key, const std::string& fallback) const {
    const std::string value = JsonGetString(m_json, key);
    return value.empty() ? fallback : value;
}

std::wstring LocaleCatalog::GetWide(const std::string& key, const wchar_t* fallback) const {
    const std::wstring value = Utf8ToWide(Get(key));
    return value.empty() ? std::wstring(fallback ? fallback : L"") : value;
}

} // namespace facelogin
