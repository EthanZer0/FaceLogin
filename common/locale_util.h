#pragma once

#include <string>

namespace facelogin {

// Resolves "auto" and normalizes supported BCP 47 locale tags. Unsupported
// Windows display languages intentionally fall back to the source catalog.
std::string ResolveLocale(const std::string& preference);

class LocaleCatalog {
public:
    bool Load(const std::wstring& installDir, const std::string& preference);

    const std::string& locale() const { return m_locale; }
    const std::string& json() const { return m_json; }

    std::string Get(const std::string& key, const std::string& fallback = "") const;
    std::wstring GetWide(const std::string& key, const wchar_t* fallback = L"") const;

private:
    std::string m_locale = "zh-CN";
    std::string m_json;
};

} // namespace facelogin
