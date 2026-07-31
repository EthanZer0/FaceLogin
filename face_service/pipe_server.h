#pragma once

#include <windows.h>
#include <string>
#include <accctrl.h>
#include <aclapi.h>

namespace facelogin {

// Named pipe server for communication with the credential provider DLL.
// Uses synchronous I/O (no FILE_FLAG_OVERLAPPED) for reliability.
// Security: SYSTEM + Administrators + current interactive user can connect.

class PipeServer {
public:
    PipeServer() = default;
    ~PipeServer();

    // Non-copyable
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Create the named pipe and wait for a client connection.
    // Blocks until a client connects or the handle is closed (via Close()).
    // Returns true when a client has connected.
    bool WaitForClient(DWORD timeoutMs = 30000);

    // Read a null-terminated UTF-16LE message from the pipe (synchronous).
    // Returns true and sets outMessage on success.
    bool ReadMessage(std::wstring& outMessage, DWORD timeoutMs = 30000);

    // Write a null-terminated UTF-16LE message to the pipe (synchronous).
    bool WriteMessage(const std::wstring& message);

    // Disconnect current client (allows a new client to connect).
    void Disconnect();

    // Close the pipe entirely. Unblocks any pending I/O.
    void Close();

    // Get the raw pipe handle (for FlushFileBuffers, etc.)
    HANDLE GetHandle() const { return m_hPipe; }

    bool IsConnected() const { return m_connected; }

private:
    PSECURITY_DESCRIPTOR CreateSecurityDescriptor();

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    bool m_connected = false;
};

} // namespace facelogin
