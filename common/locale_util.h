#pragma once

#include <string>

namespace facelogin {

// UTF-8 <-> UTF-16 conversion helpers (all locale packs are UTF-8 JSON).
std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);

// Resolves "auto" and normalizes supported BCP 47 locale tags. Unsupported
// Windows display languages intentionally fall back to the source catalog.
std::string ResolveLocale(const std::string& preference);

class LocaleCatalog {
public:
    bool Load(const std::wstring& installDir, const std::string& preference);

    const std::string& locale() const { return m_locale; }
    const std::string& json() const { return m_json; }

    // Lookup order: the active pack, then the zh-CN pack (fallback layer —
    // same policy as the Console's t(): current pack || zh pack || fallback).
    // A key missing from both packs returns `fallback`.
    std::string Get(const std::string& key, const std::string& fallback = "") const;
    std::wstring GetWide(const std::string& key, const wchar_t* fallback = L"") const;

private:
    std::string m_locale = "zh-CN";
    std::string m_json;      // active locale pack
    std::string m_jsonZh;    // zh-CN pack (fallback layer)
};

} // namespace facelogin
