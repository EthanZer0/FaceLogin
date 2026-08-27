#pragma once

#include <windows.h>
#include <string>

// FaceLogin IPC Protocol Definitions
//
// Transport: Named pipe \\.\pipe\FaceLoginPipe
// Framing: UTF-16LE text, null-terminated
// Security: DACL restricted to SYSTEM + Administrators, PIPE_REJECT_REMOTE_CLIENTS

namespace facelogin {
namespace ipc {

// Pipe name
constexpr wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\FaceLoginPipe";
constexpr DWORD PIPE_BUFFER_SIZE = 4096;
constexpr DWORD PIPE_TIMEOUT_MS = 30000;
constexpr DWORD AUTH_TIMEOUT_SECONDS = 15;

// Message types
constexpr wchar_t MSG_AUTH_REQUEST[] = L"AUTH_REQUEST";
constexpr wchar_t MSG_AUTH_SUCCESS_PREFIX[] = L"AUTH_SUCCESS:";
constexpr wchar_t MSG_AUTH_TIMEOUT[] = L"AUTH_TIMEOUT";
constexpr wchar_t MSG_AUTH_NO_FACE[] = L"AUTH_NO_FACE";
// A face WAS detected (embedding computed) but no enrolled face matched it —
// distinct from AUTH_TIMEOUT (nothing useful seen for the whole window) and
// AUTH_NO_FACE (no face at all). The CP shows "人脸匹配失败" for this and
// reserves "未识别到人脸" for the timeout case.
constexpr wchar_t MSG_AUTH_NO_MATCH[] = L"AUTH_NO_MATCH";
constexpr wchar_t MSG_AUTH_ERROR_PREFIX[] = L"AUTH_ERROR:";
constexpr wchar_t MSG_AUTH_CANCELLED[] = L"AUTH_CANCELLED";
constexpr wchar_t MSG_STATUS_PREFIX[] = L"STATUS:";
constexpr wchar_t MSG_RELOAD_DB[] = L"RELOAD_DB";
constexpr wchar_t MSG_RELOAD_OK[] = L"RELOAD_OK";
constexpr wchar_t MSG_CONFIG_RELOAD[] = L"CONFIG_RELOAD";
constexpr wchar_t MSG_CONFIG_RELOAD_OK[] = L"CONFIG_RELOAD_OK";
constexpr wchar_t MSG_GET_LOGS[] = L"GET_LOGS";
constexpr wchar_t MSG_GET_LOGS_OK_PREFIX[] = L"GET_LOGS_OK:";
constexpr wchar_t MSG_PING[] = L"PING";
constexpr wchar_t MSG_PONG[] = L"PONG";

// Locale keys carried by STATUS:/AUTH_ERROR: payloads. The values MUST match
// keys in locales/*.json — the JSON packs are the single source of truth.
// The credential provider translates any payload through its LocaleCatalog
// (current pack → zh-CN pack → its own fallback), so the service never
// embeds display text; an unknown key simply degrades to Chinese. These
// constants keep service-side spelling in sync with the packs.
constexpr wchar_t L10N_LOADING_MODELS[] = L"service.loadingModels";
constexpr wchar_t L10N_RECOGNIZING[] = L"credential.recognizing";
constexpr wchar_t L10N_NO_REGISTERED_USERS[] = L"service.noRegisteredUsers";
constexpr wchar_t L10N_MODEL_LOAD_FAILED[] = L"service.modelLoadFailed";
constexpr wchar_t L10N_CAMERA_UNAVAILABLE[] = L"service.cameraUnavailable";
constexpr wchar_t L10N_NO_MATCH[] = L"credential.noMatch";
constexpr wchar_t L10N_LIVENESS_CHECKING[] = L"service.livenessChecking";
constexpr wchar_t L10N_BLINK_PROMPT[] = L"service.blinkPrompt";
constexpr wchar_t L10N_ANTI_SPOOF_FAILED[] = L"service.antiSpoofFailed";
constexpr wchar_t L10N_BLINK_FAILED[] = L"service.blinkFailed";
constexpr wchar_t L10N_FINAL_MATCH_FAILED[] = L"service.finalMatchFailed";

// Legacy AUTH_ERROR payload for a passwordless account (MSA with no password
// — PIN/Hello only). No longer sent by the service (passwordless accounts now
// unlock via blank-password submission), but kept so the credential provider
// can still recognize the notice if an older service version sends it. The
// value is a locale key, not display text.
constexpr wchar_t MSG_PASSWORDLESS_NOTICE[] = L"credential.passwordless";

// Parsed authentication result
struct AuthResult {
    enum class Status {
        Success,
        Timeout,
        NoFace,
        NoMatch,
        Error,
        Cancelled
    };

    Status status = Status::Error;
    std::wstring sid;         // User's SID (e.g. "S-1-5-21-...")
    std::wstring upn;         // UserPrincipalName (e.g. "john@outlook.com")
    std::wstring domain;
    std::wstring username;
    std::wstring password;    // NOTE: zero this out ASAP
    std::wstring errorMessage;
};

// Parse a received message into an AuthResult
// Message formats:
//   "AUTH_SUCCESS:SID:UPN:USERNAME:PASSWORD"
//   "AUTH_SUCCESS:SID::DOMAIN\\USER:PASSWORD"  (no UPN, e.g. local account)
//   "AUTH_TIMEOUT"
//   "AUTH_NO_FACE"
//   "AUTH_ERROR:some error message"
//   "AUTH_CANCELLED"
AuthResult ParseAuthMessage(const std::wstring& message);

// Build a success message to send through the pipe.
// Format: AUTH_SUCCESS:SID:UPN:USERNAME:PASSWORD
// For local accounts without UPN, upn can be empty.
std::wstring BuildAuthSuccessMessage(const std::wstring& sid,
                                      const std::wstring& upn,
                                      const std::wstring& domain,
                                      const std::wstring& username,
                                      const std::wstring& password);

// Build an error message
std::wstring BuildAuthErrorMessage(const std::wstring& error);

} // namespace ipc
} // namespace facelogin
