#pragma once

#include <windows.h>
#include <dpapi.h>
#include <vector>
#include <string>
#include <cstdint>

namespace facelogin {

// DPAPI utility functions for encrypting/decrypting sensitive data.
// Uses CRYPTPROTECT_LOCAL_MACHINE so that the credential provider
// (running as SYSTEM) can decrypt data even when no user is logged in.
class DpapiUtil {
public:
    // Encrypt plaintext data with DPAPI (machine scope).
    // Returns empty vector on failure.
    static std::vector<uint8_t> Protect(const std::vector<uint8_t>& plaintext);
    static std::vector<uint8_t> Protect(const std::wstring& plaintext);
    static std::vector<uint8_t> Protect(const uint8_t* data, size_t size);

    // Decrypt data previously encrypted with Protect().
    // Returns empty vector on failure.
    static std::vector<uint8_t> Unprotect(const std::vector<uint8_t>& ciphertext);
    static std::wstring UnprotectToString(const std::vector<uint8_t>& ciphertext);
    static std::vector<uint8_t> Unprotect(const uint8_t* data, size_t size);

private:
    static constexpr DWORD kFlags = CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN;
};

} // namespace facelogin
