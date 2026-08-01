#pragma once

#include <windows.h>
#include <string>
#include <functional>

namespace facelogin {

// Named pipe client for the credential provider DLL.
// Connects to the FaceLogin service to send/receive authentication messages.
//
// Runs in LogonUI.exe (SYSTEM context on Secure Desktop).
// Uses a background thread with blocking synchronous ReadFile so the
// message is captured immediately when the server sends it — no race
// with GetSerialization() polling or server-side DisconnectNamedPipe.
//
// OnResponseCallback: called from the background read thread when a
// response arrives (or the pipe breaks).  The credential uses this to
// transition state and signal LogonUI to re-serialize immediately.

using OnResponseCallback = std::function<void(bool success, const std::wstring& message)>;
using OnStatusCallback = std::function<void(const std::wstring& message)>;

class PipeClient {
public:
    PipeClient();
    ~PipeClient();

    // Non-copyable
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Connect to the FaceLogin named pipe server.
    // Retries for up to ~5 seconds (pipe server may not be ready yet).
    bool Connect(DWORD timeoutMs = 5000);

    // Send a message to the server. Returns true on success.
    bool SendMessage(const std::wstring& message);

    // Spawn a background thread that loops reading messages from the pipe.
    // STATUS: messages trigger onStatus (if set).
    // Terminal messages (AUTH_SUCCESS/AUTH_TIMEOUT/AUTH_ERROR/etc.) trigger
    // onResponse and the thread exits.
    void StartBackgroundRead(OnResponseCallback onResponse = nullptr,
                             OnStatusCallback onStatus = nullptr);

    // Check whether the background read has completed.  Non-blocking.
    // Returns true and sets outMessage when the server response was received.
    bool CheckResponse(std::wstring& outMessage);

    // Check if connected
    bool IsConnected() const { return m_connected; }

    // Close the connection (closes the pipe handle, which unblocks the
    // background read thread, then joins the thread).
    void Disconnect();

private:
    static DWORD WINAPI ReadThreadProc(LPVOID param);
    void CleanupReadThread();

    // Returns true if msg is a terminal (non-status) message
    static bool IsTerminalMessage(const std::wstring& msg);

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    bool m_connected = false;

    // Background blocking read
    HANDLE m_hReadThread = nullptr;
    HANDLE m_hDataReady = nullptr;       // manual-reset: set when response arrives
    HANDLE m_hReadStop = nullptr;        // manual-reset: signaled to stop the read thread
    wchar_t m_readBuffer[4096] = {};
    DWORD  m_bytesRead = 0;
    bool   m_readSuccess = false;

    // Callbacks
    OnResponseCallback m_onResponse;
    OnStatusCallback   m_onStatus;

    CRITICAL_SECTION m_cs;
    bool m_csInitialized = false;
};

} // namespace facelogin
