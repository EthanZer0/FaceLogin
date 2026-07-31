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

// Parsed authentication result
struct AuthResult {
    enum class Status {
        Success,
        Timeout,
        NoFace,
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
