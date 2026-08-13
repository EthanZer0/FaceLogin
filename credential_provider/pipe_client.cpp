#include "pipe_client.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include <chrono>
#include <thread>
#include <cwchar>

namespace facelogin {

PipeClient::PipeClient() {
    InitializeCriticalSection(&m_cs);
    m_csInitialized = true;
    m_hDataReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_hReadStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeClient::~PipeClient() {
    Disconnect();
    if (m_hDataReady) {
        CloseHandle(m_hDataReady);
        m_hDataReady = nullptr;
    }
    if (m_hReadStop) {
        CloseHandle(m_hReadStop);
        m_hReadStop = nullptr;
    }
    if (m_csInitialized) {
        DeleteCriticalSection(&m_cs);
        m_csInitialized = false;
    }
}

void PipeClient::CleanupReadThread() {
    if (m_hReadThread) {
        // Wait up to 2 seconds for the thread to exit.
        DWORD waitResult = WaitForSingleObject(m_hReadThread, 2000);
        if (waitResult != WAIT_OBJECT_0) {
            FACELOGIN_WARN(L"Read thread did not exit in time");
        }
        CloseHandle(m_hReadThread);
        m_hReadThread = nullptr;
    }
}

bool PipeClient::Connect(DWORD timeoutMs) {
    Disconnect();
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        m_hPipe = CreateFileW(
            ipc::PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,                          // No sharing
            nullptr,                    // Default security
            OPEN_EXISTING,
            0,                          // Synchronous I/O
            nullptr);

        if (m_hPipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(m_hPipe, &mode, nullptr, nullptr)) {
                FACELOGIN_WARN(L"SetNamedPipeHandleState failed: %lu", GetLastError());
            }

            m_connected = true;
            FACELOGIN_INFO(L"Pipe client connected to service");
            return true;
        }

        DWORD err = GetLastError();

        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(ipc::PIPE_NAME, 200)) {
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeoutMs) {
                    FACELOGIN_WARN(L"Pipe connection timed out (pipe busy)");
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        if (err == ERROR_FILE_NOT_FOUND) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeoutMs) {
                FACELOGIN_WARN(L"Pipe connection timed out (pipe not found)");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        FACELOGIN_ERROR(L"CreateFile on pipe failed: %lu", err);
        return false;
    }
}

bool PipeClient::ProbeServiceAvailable() {
    // One-shot CreateFileW with no retry and no wait. If the pipe exists we
    // can open it (even if busy, ERROR_PIPE_BUSY means the service is up); if
    // it's ERROR_FILE_NOT_FOUND the service is not running.
    HANDLE h = CreateFileW(
        ipc::PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY) {
        // Pipe exists but a client is already connected — service is up.
        return true;
    }
    if (err == ERROR_FILE_NOT_FOUND) {
        FACELOGIN_INFO(L"ProbeServiceAvailable: service pipe not found — service down");
        return false;
    }
    // Other errors (e.g. access denied) — treat as available so we don't
    // mislabel a permission issue as "service down".
    FACELOGIN_WARN(L"ProbeServiceAvailable: CreateFile err=%lu (treating as available)", err);
    return true;
}

bool PipeClient::SendMessage(const std::wstring& message) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    DWORD byteSize = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));

    BOOL result = WriteFile(m_hPipe, message.c_str(), byteSize,
                            &bytesWritten, nullptr);

    if (!result) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            FACELOGIN_WARN(L"Pipe broken during send");
            m_connected = false;
        } else {
            FACELOGIN_ERROR(L"WriteFile on pipe failed: %lu", err);
        }
        return false;
    }

    return bytesWritten == byteSize;
}

// ============================================================================
// Background read thread — loops, reading messages until a terminal one
// arrives (AUTH_SUCCESS, AUTH_TIMEOUT, etc.) or the pipe breaks.
// STATUS: messages are dispatched immediately via m_onStatus so the CP
// can update the LogonUI status text in real time.
// ============================================================================

bool PipeClient::IsTerminalMessage(const std::wstring& msg) {
    return msg.starts_with(ipc::MSG_AUTH_SUCCESS_PREFIX) ||
           msg.starts_with(ipc::MSG_AUTH_ERROR_PREFIX) ||
           msg == ipc::MSG_AUTH_TIMEOUT ||
           msg == ipc::MSG_AUTH_NO_FACE ||
           msg == ipc::MSG_AUTH_NO_MATCH ||
           msg == ipc::MSG_AUTH_CANCELLED;
}

DWORD WINAPI PipeClient::ReadThreadProc(LPVOID param) {
    PipeClient* self = static_cast<PipeClient*>(param);

    // Poll the pipe for data instead of blocking forever on ReadFile.
    // A permanently-blocked synchronous ReadFile is NOT canceled by
    // CloseHandle (cancel-on-close only works for OVERLAPPED I/O), so a
    // thread stuck there would keep the pipe's client file object alive
    // and the server could never observe the disconnect. We instead check
    // PeekNamedPipe + the stop event in a short loop and exit promptly.
    while (true) {
        // Stop signal (set by Disconnect) — exit without touching the pipe.
        if (WaitForSingleObject(self->m_hReadStop, 0) == WAIT_OBJECT_0) {
            FACELOGIN_INFO(L"Background read: stop signaled — exiting");
            break;
        }

        DWORD bytesAvail = 0, totalBytes = 0;
        if (PeekNamedPipe(self->m_hPipe, nullptr, 0, nullptr, &bytesAvail, &totalBytes)) {
            if (bytesAvail == 0) {
                // Connected but idle — wait briefly, keep polling.
                // 10ms (was 50ms): the auth result arrives from the service via
                // this loop, so a shorter poll cuts the perceived unlock delay
                // by ~40ms while staying trivially cheap (a few wakeups/frame
                // of CPU on a 60Hz display).
                Sleep(10);
                continue;
            }
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
                err == ERROR_PIPE_NOT_CONNECTED) {
                FACELOGIN_INFO(L"Background read: pipe broken by server");
            } else {
                FACELOGIN_WARN(L"Background read failed: %lu", err);
            }
            // Signal the main thread that the pipe is dead
            EnterCriticalSection(&self->m_cs);
            self->m_readSuccess = false;
            self->m_bytesRead = 0;
            self->m_connected = false;
            LeaveCriticalSection(&self->m_cs);
            SetEvent(self->m_hDataReady);
            // Notify — connection lost unexpectedly
            if (self->m_onResponse) {
                self->m_onResponse(false, L"");
            }
            break;
        }

        // Data is available — ReadFile returns immediately.
        ZeroMemory(self->m_readBuffer, sizeof(self->m_readBuffer));
        DWORD bytesRead = 0;
        BOOL result = ReadFile(self->m_hPipe,
                               self->m_readBuffer,
                               static_cast<DWORD>(sizeof(self->m_readBuffer) - sizeof(wchar_t)),
                               &bytesRead,
                               nullptr);

        bool success = (result && bytesRead > 0);
        if (!success) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                FACELOGIN_INFO(L"Background read: pipe broken by server");
            } else {
                FACELOGIN_WARN(L"Background read failed: %lu", err);
            }
            EnterCriticalSection(&self->m_cs);
            self->m_readSuccess = false;
            self->m_bytesRead = 0;
            self->m_connected = false;
            LeaveCriticalSection(&self->m_cs);
            SetEvent(self->m_hDataReady);
            if (self->m_onResponse) {
                self->m_onResponse(false, L"");
            }
            break;
        }

        // Parse message from buffer
        size_t len = bytesRead / sizeof(wchar_t);
        while (len > 0 && self->m_readBuffer[len - 1] == L'\0') {
            len--;
        }
        std::wstring msg(self->m_readBuffer, len);
        FACELOGIN_INFO(L"Background read received: %s (len=%zu)",
                       msg.substr(0, 80).c_str(), len);

        // STATUS: prefix → dispatch immediately, keep reading
        if (msg.starts_with(ipc::MSG_STATUS_PREFIX)) {
            std::wstring statusText = msg.substr(wcslen(ipc::MSG_STATUS_PREFIX));
            FACELOGIN_INFO(L"Status update: %s", statusText.c_str());
            if (self->m_onStatus) {
                self->m_onStatus(statusText);
            }
            continue;  // keep looping for more messages
        }

        // Terminal message — store and signal
        EnterCriticalSection(&self->m_cs);
        self->m_bytesRead = bytesRead;
        self->m_readSuccess = true;
        LeaveCriticalSection(&self->m_cs);

        SetEvent(self->m_hDataReady);

        if (self->m_onResponse) {
            self->m_onResponse(success, msg);
        }
        break;
    }

    return 0;
}

void PipeClient::StartBackgroundRead(OnResponseCallback onResponse,
                                      OnStatusCallback onStatus) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return;

    // Join any previous thread
    CleanupReadThread();

    // Reset state
    ResetEvent(m_hDataReady);
    ResetEvent(m_hReadStop);
    m_bytesRead = 0;
    m_readSuccess = false;
    ZeroMemory(m_readBuffer, sizeof(m_readBuffer));
    m_onResponse = std::move(onResponse);
    m_onStatus   = std::move(onStatus);

    m_hReadThread = CreateThread(
        nullptr, 0,
        ReadThreadProc, this,
        0, nullptr);

    if (!m_hReadThread) {
        FACELOGIN_ERROR(L"Failed to create read thread: %lu", GetLastError());
    }
}

bool PipeClient::CheckResponse(std::wstring& outMessage) {
    if (!m_hDataReady) return false;

    // Non-blocking: has the background thread finished?
    DWORD waitResult = WaitForSingleObject(m_hDataReady, 0);
    if (waitResult != WAIT_OBJECT_0) {
        return false; // still waiting
    }

    // Thread is done — grab the result under the CS
    EnterCriticalSection(&m_cs);
    if (m_readSuccess && m_bytesRead > 0) {
        size_t len = m_bytesRead / sizeof(wchar_t);
        while (len > 0 && m_readBuffer[len - 1] == L'\0') {
            len--;
        }
        outMessage.assign(m_readBuffer, len);
    }
    LeaveCriticalSection(&m_cs);

    // Clean up the thread handle
    if (m_hReadThread) {
        CloseHandle(m_hReadThread);
        m_hReadThread = nullptr;
    }

    return m_readSuccess && m_bytesRead > 0;
}

void PipeClient::Disconnect() {
    // Signal the read thread to stop FIRST. It polls the stop event every
    // ~50ms, so it exits promptly. (A permanently-blocked synchronous ReadFile
    // would NOT be canceled by CloseHandle — cancel-on-close only works for
    // OVERLAPPED I/O — leaving a "zombie" thread that keeps the pipe's client
    // file object alive, so the server never sees the disconnect.)
    if (m_hReadStop) {
        SetEvent(m_hReadStop);
    }

    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
    m_connected = false;

    CleanupReadThread();

    m_bytesRead = 0;
    m_readSuccess = false;
}

} // namespace facelogin
