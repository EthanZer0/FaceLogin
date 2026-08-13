#include "ipc_protocol.h"
#include <algorithm>

namespace facelogin {
namespace ipc {

AuthResult ParseAuthMessage(const std::wstring& message) {
    AuthResult result;

    if (message.empty()) {
        result.status = AuthResult::Status::Error;
        result.errorMessage = L"Empty message received";
        return result;
    }

    // Check for success: "AUTH_SUCCESS:SID:UPN:USERNAME:PASSWORD"
    // Older format: "AUTH_SUCCESS:DOMAIN\\USER:PASSWORD" (no SID/UPN prefix)
    if (message.starts_with(MSG_AUTH_SUCCESS_PREFIX)) {
        std::wstring payload = message.substr(wcslen(MSG_AUTH_SUCCESS_PREFIX));

        // Split by colons. New format has 4 parts: SID:UPN:USERNAME:PASSWORD
        // Old format has 1 colon separating USER and PASSWORD.
        // Count colons to detect format.
        size_t colonCount = 0;
        for (wchar_t ch : payload) {
            if (ch == L':') colonCount++;
        }

        if (colonCount >= 3) {
            // New format: SID:UPN:USERNAME:PASSWORD
            size_t pos1 = payload.find(L':');
            if (pos1 == std::wstring::npos) { result.status = AuthResult::Status::Error; result.errorMessage = L"Malformed AUTH_SUCCESS"; return result; }
            result.sid = payload.substr(0, pos1);

            size_t pos2 = payload.find(L':', pos1 + 1);
            if (pos2 == std::wstring::npos) { result.status = AuthResult::Status::Error; result.errorMessage = L"Malformed AUTH_SUCCESS"; return result; }
            result.upn = payload.substr(pos1 + 1, pos2 - pos1 - 1);

            size_t pos3 = payload.find(L':', pos2 + 1);
            if (pos3 == std::wstring::npos) { result.status = AuthResult::Status::Error; result.errorMessage = L"Malformed AUTH_SUCCESS"; return result; }

            std::wstring userPart = payload.substr(pos2 + 1, pos3 - pos2 - 1);
            std::wstring passwordPart = payload.substr(pos3 + 1);

            // Split domain\user
            size_t slashPos = userPart.find(L'\\');
            if (slashPos != std::wstring::npos) {
                result.domain = userPart.substr(0, slashPos);
                result.username = userPart.substr(slashPos + 1);
            } else {
                result.domain = L".";
                result.username = userPart;
            }

            result.password = passwordPart;
            result.status = AuthResult::Status::Success;
            return result;
        } else {
            // Old format: "DOMAIN\\USER:PASSWORD" (backward compat)
            size_t colonPos = payload.find(L':');
            if (colonPos == std::wstring::npos) {
                result.status = AuthResult::Status::Error;
                result.errorMessage = L"Malformed AUTH_SUCCESS: unexpected format";
                return result;
            }

            std::wstring userPart = payload.substr(0, colonPos);
            std::wstring passwordPart = payload.substr(colonPos + 1);

            size_t slashPos = userPart.find(L'\\');
            if (slashPos != std::wstring::npos) {
                result.domain = userPart.substr(0, slashPos);
                result.username = userPart.substr(slashPos + 1);
            } else {
                result.domain = L".";
                result.username = userPart;
            }

            result.password = passwordPart;
            result.status = AuthResult::Status::Success;
            return result;
        }
    }

    if (message == MSG_AUTH_TIMEOUT) {
        result.status = AuthResult::Status::Timeout;
        return result;
    }

    if (message == MSG_AUTH_NO_FACE) {
        result.status = AuthResult::Status::NoFace;
        return result;
    }

    if (message == MSG_AUTH_NO_MATCH) {
        result.status = AuthResult::Status::NoMatch;
        return result;
    }

    if (message == MSG_AUTH_CANCELLED) {
        result.status = AuthResult::Status::Cancelled;
        return result;
    }

    if (message.starts_with(MSG_AUTH_ERROR_PREFIX)) {
        result.status = AuthResult::Status::Error;
        result.errorMessage = message.substr(wcslen(MSG_AUTH_ERROR_PREFIX));
        return result;
    }

    // Unknown message
    result.status = AuthResult::Status::Error;
    result.errorMessage = L"Unknown message: " + message;
    return result;
}

std::wstring BuildAuthSuccessMessage(const std::wstring& sid,
                                      const std::wstring& upn,
                                      const std::wstring& domain,
                                      const std::wstring& username,
                                      const std::wstring& password) {
    std::wstring msg = MSG_AUTH_SUCCESS_PREFIX;
    // Format: SID:UPN:DOMAIN\USERNAME:PASSWORD
    msg += sid + L":" + upn + L":" + domain + L"\\" + username + L":" + password;
    return msg;
}

std::wstring BuildAuthErrorMessage(const std::wstring& error) {
    std::wstring msg(MSG_AUTH_ERROR_PREFIX);
    msg += error;
    return msg;
}

} // namespace ipc
} // namespace facelogin
