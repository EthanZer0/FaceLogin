#include "dpapi_util.h"
#include <stdexcept>

namespace facelogin {

std::vector<uint8_t> DpapiUtil::Protect(const std::vector<uint8_t>& plaintext) {
    return Protect(plaintext.data(), plaintext.size());
}

std::vector<uint8_t> DpapiUtil::Protect(const std::wstring& plaintext) {
    auto bytes = reinterpret_cast<const uint8_t*>(plaintext.data());
    size_t byteSize = plaintext.size() * sizeof(wchar_t);
    return Protect(bytes, byteSize);
}

std::vector<uint8_t> DpapiUtil::Protect(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return {};
    }

    DATA_BLOB blobIn = {};
    blobIn.pbData = const_cast<uint8_t*>(data);
    blobIn.cbData = static_cast<DWORD>(size);

    DATA_BLOB blobOut = {};

    if (!CryptProtectData(&blobIn, L"FaceLogin encrypted credential",
                          nullptr, nullptr, nullptr,
                          kFlags, &blobOut)) {
        return {};
    }

    std::vector<uint8_t> result(blobOut.pbData, blobOut.pbData + blobOut.cbData);
    LocalFree(blobOut.pbData);
    return result;
}

std::vector<uint8_t> DpapiUtil::Unprotect(const std::vector<uint8_t>& ciphertext) {
    return Unprotect(ciphertext.data(), ciphertext.size());
}

std::wstring DpapiUtil::UnprotectToString(const std::vector<uint8_t>& ciphertext) {
    auto plain = Unprotect(ciphertext);
    if (plain.empty()) return {};
    if (plain.size() % sizeof(wchar_t) != 0) return {};

    return std::wstring(reinterpret_cast<const wchar_t*>(plain.data()),
                         plain.size() / sizeof(wchar_t));
}

std::vector<uint8_t> DpapiUtil::Unprotect(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return {};
    }

    DATA_BLOB blobIn = {};
    blobIn.pbData = const_cast<uint8_t*>(data);
    blobIn.cbData = static_cast<DWORD>(size);

    DATA_BLOB blobOut = {};
    LPWSTR pDesc = nullptr;

    if (!CryptUnprotectData(&blobIn, &pDesc, nullptr, nullptr, nullptr,
                            kFlags, &blobOut)) {
        return {};
    }

    std::vector<uint8_t> result(blobOut.pbData, blobOut.pbData + blobOut.cbData);

    if (pDesc) LocalFree(pDesc);
    LocalFree(blobOut.pbData);
    return result;
}

} // namespace facelogin
