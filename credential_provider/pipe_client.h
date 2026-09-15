#pragma once

#include <windows.h>
#include <string>
#include <functional>
#include <memory>

namespace facelogin {

// Named pipe client for the credential provider DLL.
// Connects to the FaceLogin service to send/receive authentication messages.
//
// Runs in LogonUI.exe (SYSTEM context on Secure Desktop).
// Uses a background polling thread so terminal messages are delivered once
// and the owning credential can deterministically join the thread before the
// PipeClient object is destroyed.
//
// OnResponseCallback: called from the background read thread when a
// response arrives (or the pipe breaks).  The credential uses this to
// transition state and signal LogonUI to re-serialize immediately.

enum class PipeTerminalTransport { Message, Failed };
using OnResponseCallback =
    std::function<void(PipeTerminalTransport transport,
                       const std::wstring& message)>;
using OnStatusCallback = std::function<void(const std::wstring& message)>;

class PipeClient : public std::enable_shared_from_this<PipeClient> {
public:
    PipeClient();
    ~PipeClient();

    // Non-copyable
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Connect to the FaceLogin named pipe server.
    // Retries for up to ~5 seconds (pipe server may not be ready yet).
    bool Connect(DWORD timeoutMs = 5000, HANDLE cancelEvent = nullptr);

    // Lightweight liveness probe: does the service's named pipe exist right
    // now? Attempts one CreateFileW with no wait/retry — used by the
    // credential provider at lock-screen time to show "service not running"
    // immediately instead of waiting for the user to press a key (which would
    // then time out after ~5s). Returns true if the pipe is openable, false
    // if it doesn't exist (service down).
    static bool ProbeServiceAvailable();

    // Send a message to the server. Returns true on success.
    bool SendMessage(const std::wstring& message);

    // Spawn a background thread that loops reading messages from the pipe.
    // STATUS: messages trigger onStatus (if set).
    // Terminal messages (AUTH_SUCCESS/AUTH_TIMEOUT/AUTH_ERROR/etc.) trigger
    // onResponse and the thread exits.
    bool StartBackgroundRead(OnResponseCallback onResponse = nullptr,
                             OnStatusCallback onStatus = nullptr,
                             DWORD timeoutMs = 0);

    // Check if connected
    bool IsConnected() const;

    // Close the connection (closes the pipe handle, which unblocks the
    // background read thread, then joins the thread).
    void Disconnect();

private:
    static DWORD WINAPI ReadThreadProc(LPVOID param);
    void CleanupReadThread();
    bool IsStopping() const;
    void MarkDisconnected();

    // Returns true if msg is a terminal (non-status) message
    static bool IsTerminalMessage(const std::wstring& msg);

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    bool m_connected = false;

    // Background blocking read
    HANDLE m_hReadThread = nullptr;
    DWORD m_readThreadId = 0;
    HANDLE m_hReadStop = nullptr;        // manual-reset: signaled to stop the read thread

    // Callbacks
    OnResponseCallback m_onResponse;
    OnStatusCallback   m_onStatus;
    bool m_terminalDelivered = false;
    ULONGLONG m_readDeadlineTick = 0;

    CRITICAL_SECTION m_cs;
    bool m_csInitialized = false;
};

} // namespace facelogin
