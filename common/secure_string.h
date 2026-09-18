#pragma once

#include <windows.h>
#include <string>

namespace facelogin {

inline void SecureErase(std::wstring& value) noexcept {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
    }
}

class ScopedWStringWipe {
public:
    explicit ScopedWStringWipe(std::wstring& value) noexcept : value_(value) {}
    ~ScopedWStringWipe() { SecureErase(value_); }

    ScopedWStringWipe(const ScopedWStringWipe&) = delete;
    ScopedWStringWipe& operator=(const ScopedWStringWipe&) = delete;

private:
    std::wstring& value_;
};

} // namespace facelogin
