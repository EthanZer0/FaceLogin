#include "locale_util.h"
#include "config_util.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>

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

} // namespace

std::string ResolveLocale(const std::string& preference) {
    if (!preference.empty() && preference != "auto") return NormalizeTag(preference);

    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, localeName, -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::string locale(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, localeName, -1,
                                locale.data(), size, nullptr, nullptr);
            locale.pop_back();
            return NormalizeTag(locale);
        }
    }
    return "zh-CN";
}

bool LocaleCatalog::Load(const std::wstring& installDir, const std::string& preference) {
    m_locale = ResolveLocale(preference);
    m_json = ReadUtf8File(installDir + L"\\locales\\" + Utf8ToWide(m_locale) + L".json");
    if (m_json.empty() && m_locale != "zh-CN") {
        m_locale = "zh-CN";
        m_json = ReadUtf8File(installDir + L"\\locales\\zh-CN.json");
    }
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
