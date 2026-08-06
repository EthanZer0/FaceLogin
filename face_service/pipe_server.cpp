#include "pipe_server.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include <sddl.h>
#include <vector>

namespace facelogin {

PipeServer::~PipeServer() {
    Close();
}

PSECURITY_DESCRIPTOR PipeServer::CreateSecurityDescriptor() {
    // Create a security descriptor that grants access to:
    // - SYSTEM (full control)
    // - BUILTIN\Administrators (full control)
    // - Current interactive user (full control)
    // Deny: Network, Anonymous, Everyone else

    PSECURITY_DESCRIPTOR pSD = nullptr;
    PACL pACL = nullptr;

    EXPLICIT_ACCESSW ea[3] = {};

    // SYSTEM: full access
    ea[0].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName = const_cast<LPWSTR>(L"SYSTEM");

    // Administrators: full access
    ea[1].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    ea[1].grfAccessMode = SET_ACCESS;
    ea[1].grfInheritance = NO_INHERITANCE;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea[1].Trustee.ptstrName = const_cast<LPWSTR>(L"Administrators");

    DWORD entryCount = 2;

    // Try to add the current user by name.
    // GetUserNameW works even when running elevated.
    wchar_t currentUserName[256] = {};
    DWORD nameLen = 256;
    if (GetUserNameW(currentUserName, &nameLen) && nameLen > 0) {
        // Build domain\user format for SetEntriesInAclW
        wchar_t qualifiedName[512] = {};

        DWORD sidBufSize = 0;
        DWORD domainLen = 256;
        SID_NAME_USE sidType;

        // First call to get buffer size
        LookupAccountNameW(nullptr, currentUserName,
                          nullptr, &sidBufSize,
                          nullptr, &domainLen, &sidType);

        if (sidBufSize > 0) {
            std::vector<BYTE> sidBuf(sidBufSize);
            wchar_t domain[256] = {};
            DWORD domainSz = 256;
            if (LookupAccountNameW(nullptr, currentUserName,
                                   sidBuf.data(), &sidBufSize,
                                   domain, &domainSz, &sidType)) {
                swprintf_s(qualifiedName, L"%s\\%s", domain, currentUserName);
            } else {
                wcscpy_s(qualifiedName, currentUserName);
            }
        } else {
            wcscpy_s(qualifiedName, currentUserName);
        }

        ea[2].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
        ea[2].grfAccessMode = SET_ACCESS;
        ea[2].grfInheritance = NO_INHERITANCE;
        ea[2].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
        ea[2].Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea[2].Trustee.ptstrName = qualifiedName;
        entryCount = 3;
        FACELOGIN_INFO(L"Pipe ACL: added current user %s", qualifiedName);
    }

    DWORD dwErr = SetEntriesInAclW(entryCount, ea, nullptr, &pACL);

    if (dwErr != ERROR_SUCCESS) {
        FACELOGIN_ERROR(L"SetEntriesInAcl failed: %lu", dwErr);
        return nullptr;
    }

    pSD = static_cast<PSECURITY_DESCRIPTOR>(
        LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
    if (!pSD) {
        LocalFree(pACL);
        return nullptr;
    }

    if (!InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION)) {
        LocalFree(pACL);
        LocalFree(pSD);
        return nullptr;
    }

    if (!SetSecurityDescriptorDacl(pSD, TRUE, pACL, FALSE)) {
        LocalFree(pACL);
        LocalFree(pSD);
        return nullptr;
    }

    // pACL is owned by pSD now - do NOT free pACL separately
    return pSD;
}

bool PipeServer::WaitForClient(DWORD timeoutMs) {
    Close(); // Ensure clean state

    PSECURITY_DESCRIPTOR pSD = CreateSecurityDescriptor();
    if (!pSD) return false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    // Use synchronous (blocking) I/O for reliability.
    // ReadFile/WriteFile must be called with a valid OVERLAPPED struct
    // on overlapped handles — avoiding that complexity entirely.
    m_hPipe = CreateNamedPipeW(
        ipc::PIPE_NAME,
        PIPE_ACCESS_DUPLEX,                         // synchronous
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,                                          // Max 1 instance
        ipc::PIPE_BUFFER_SIZE,
        ipc::PIPE_BUFFER_SIZE,
        timeoutMs,                                  // default timeout for pipe ops
        &sa);

    LocalFree(pSD);

    if (m_hPipe == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"CreateNamedPipe failed: %lu", GetLastError());
        return false;
    }

    FACELOGIN_INFO(L"Named pipe created, waiting for client...");

    // Blocking wait for client connection.
    // The handle is closed by Close() in the Stop() path, which unblocks this.
    BOOL connected = ConnectNamedPipe(m_hPipe, nullptr);
    DWORD err = GetLastError();

    if (connected) {
        m_connected = true;
        FACELOGIN_INFO(L"Client connected synchronously");
        return true;
    }

    if (err == ERROR_PIPE_CONNECTED) {
        m_connected = true;
        FACELOGIN_INFO(L"Client already connected");
        return true;
    }

    // Handle was likely closed by Stop()
    if (err == ERROR_NO_DATA || err == ERROR_PIPE_NOT_CONNECTED) {
        FACELOGIN_INFO(L"Pipe closed while waiting for client");
    } else {
        FACELOGIN_WARN(L"Client connection failed: %lu", err);
    }
    Close();
    return false;
}

bool PipeServer::ReadMessage(std::wstring& outMessage, DWORD timeoutMs) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return false;

    // Bounded synchronous read on a message-mode pipe. ReadFile on a sync
    // pipe would block forever if the client never sends/never disconnects,
    // so we first poll PeekNamedPipe for either available data or a broken
    // connection, up to timeoutMs.
    DWORD bytesAvail = 0, totalBytes = 0;
    for (DWORD waited = 0; waited < timeoutMs; ) {
        if (PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvail, &totalBytes)) {
            if (bytesAvail > 0) {
                // Data ready — the ReadFile below returns immediately.
                break;
            }
            // Connected but idle — wait briefly, keep polling.
            DWORD sleepMs = (timeoutMs - waited < 50) ? (timeoutMs - waited) : 50;
            Sleep(sleepMs);
            waited += sleepMs;
            continue;
        }
        // PeekNamedPipe failed — client closed its end or pipe is broken.
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
            err == ERROR_PIPE_NOT_CONNECTED) {
            FACELOGIN_INFO(L"Pipe broken by client while waiting for message");
            m_connected = false;
            return false;
        }
        FACELOGIN_ERROR(L"PeekNamedPipe failed: %lu", err);
        m_connected = false;
        return false;
    }

    // If we timed out with no data, treat as disconnect (caller will clean up).
    DWORD bytesAvailAfter = 0, totalBytesAfter = 0;
    if (!PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvailAfter, &totalBytesAfter)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
            err == ERROR_PIPE_NOT_CONNECTED) {
            m_connected = false;
        }
        return false;
    }

    wchar_t buffer[ipc::PIPE_BUFFER_SIZE / sizeof(wchar_t)] = {};
    DWORD bytesRead = 0;
    BOOL result = ReadFile(m_hPipe, buffer,
                           static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                           &bytesRead, nullptr);

    if (!result || bytesRead == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            FACELOGIN_INFO(L"Pipe broken by client");
        } else if (!result) {
            FACELOGIN_ERROR(L"ReadFile failed: %lu", err);
        }
        m_connected = false;
        return false;
    }

    size_t len = bytesRead / sizeof(wchar_t);
    // Strip trailing null terminator(s) added by pipe WriteFile
    while (len > 0 && buffer[len - 1] == L'\0') {
        len--;
    }
    outMessage.assign(buffer, len);
    return true;
}

// Bounded "wait for the client to drain what we wrote". Previously the code
// did an unbounded ReadFile(dummy) after every WriteMessage to handshake the
// disconnect; if the client never read or never closed, the service blocked
// forever (SCM killed it → 7034, no crash event). We poll PeekNamedPipe:
// when no bytes remain to be read, the client has consumed everything (or
// closed its end) and it is safe to Disconnect().
bool PipeServer::DrainOutput(DWORD timeoutMs) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return true;

    DWORD bytesAvail = 0, totalBytes = 0;
    for (DWORD waited = 0; waited < timeoutMs; ) {
        if (PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvail, &totalBytes)) {
            if (bytesAvail == 0) {
                // All output consumed (or client already closed). Done.
                return true;
            }
            DWORD sleepMs = (timeoutMs - waited < 50) ? (timeoutMs - waited) : 50;
            Sleep(sleepMs);
            waited += sleepMs;
            continue;
        }
        // PeekNamedPipe failed → client closed its end. Fine, nothing to drain.
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
            err == ERROR_PIPE_NOT_CONNECTED) {
            m_connected = false;
            return true;
        }
        FACELOGIN_ERROR(L"DrainOutput: PeekNamedPipe failed: %lu", err);
        return false;
    }
    // Timed out with unread bytes still pending — do NOT block further.
    FACELOGIN_WARN(L"DrainOutput: timed out with unread bytes pending");
    return false;
}

bool PipeServer::WriteMessage(const std::wstring& message) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    DWORD byteSize = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));

    BOOL result = WriteFile(m_hPipe, message.c_str(), byteSize,
                            &bytesWritten, nullptr);

    if (!result || bytesWritten == 0) {
        DWORD err = GetLastError();
        // 109 (ERROR_BROKEN_PIPE) and 232 (ERROR_NO_DATA) are both normal
        // "the client closed its end" signals — a benign race when the client
        // disconnects between our disconnect check and this write. Not an
        // error; log at INFO so the service log isn't polluted.
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
            FACELOGIN_INFO(L"Pipe closed during write (err=%lu)", err);
        } else if (!result) {
            FACELOGIN_ERROR(L"WriteFile failed: %lu", err);
        }
        m_connected = false;
        return false;
    }

    return true;
}

bool PipeServer::IsClientDisconnected() const {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return true;

    // PeekNamedPipe with null buffers never blocks and reports the pipe state.
    // ERROR_BROKEN_PIPE / ERROR_NO_DATA => client closed its end.
    DWORD bytesAvail = 0, bytesLeft = 0;
    if (PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvail, &bytesLeft)) {
        return false; // still connected
    }
    DWORD err = GetLastError();
    return err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
           err == ERROR_PIPE_NOT_CONNECTED;
}

void PipeServer::Disconnect() {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_hPipe);
        DisconnectNamedPipe(m_hPipe);
        m_connected = false;
    }
}

void PipeServer::Close() {
    Disconnect();

    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }

    m_connected = false;
}

} // namespace facelogin
