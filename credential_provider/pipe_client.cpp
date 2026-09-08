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
    m_hReadStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeClient::~PipeClient() {
    Disconnect();

    if (m_hReadStop) {
        CloseHandle(m_hReadStop);
        m_hReadStop = nullptr;
    }
    if (m_csInitialized) {
        DeleteCriticalSection(&m_cs);
        m_csInitialized = false;
    }
}

bool PipeClient::IsStopping() const {
    return m_hReadStop && WaitForSingleObject(m_hReadStop, 0) == WAIT_OBJECT_0;
}

bool PipeClient::IsConnected() const {
    auto* cs = const_cast<CRITICAL_SECTION*>(&m_cs);
    EnterCriticalSection(cs);
    const bool connected = m_connected && m_hPipe != INVALID_HANDLE_VALUE;
    LeaveCriticalSection(cs);
    return connected;
}

void PipeClient::MarkDisconnected() {
    EnterCriticalSection(&m_cs);
    m_connected = false;
    LeaveCriticalSection(&m_cs);
}

void PipeClient::CleanupReadThread() {
    HANDLE thread = nullptr;
    EnterCriticalSection(&m_cs);
    thread = m_hReadThread;
    LeaveCriticalSection(&m_cs);

    if (!thread) return;
    if (thread == GetCurrentThread()) {
        FACELOGIN_ERROR(L"PipeClient: read thread attempted to join itself");
        return;
    }

    // Keep the pipe handle open until the reader has exited. The reader polls
    // in short intervals, and CancelSynchronousIo covers the Peek/Read window.
    CancelSynchronousIo(thread);
    const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        FACELOGIN_ERROR(L"PipeClient: failed to join read thread (wait=%lu)", waitResult);
        return;
    }

    EnterCriticalSection(&m_cs);
    if (m_hReadThread == thread) {
        CloseHandle(m_hReadThread);
        m_hReadThread = nullptr;
    }
    LeaveCriticalSection(&m_cs);
}

bool PipeClient::Connect(DWORD timeoutMs) {
    Disconnect();
    if (!m_hReadStop) return false;
    ResetEvent(m_hReadStop);

    const auto startTime = std::chrono::steady_clock::now();
    while (true) {
        if (IsStopping()) return false;

        HANDLE pipe = CreateFileW(
            ipc::PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                FACELOGIN_WARN(L"SetNamedPipeHandleState failed: %lu", GetLastError());
            }

            if (IsStopping()) {
                CloseHandle(pipe);
                return false;
            }

            EnterCriticalSection(&m_cs);
            m_hPipe = pipe;
            m_connected = true;
            LeaveCriticalSection(&m_cs);
            FACELOGIN_INFO(L"Pipe client connected to service");
            return true;
        }

        const DWORD err = GetLastError();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            FACELOGIN_WARN(L"Pipe connection timed out (error=%lu)", err);
            return false;
        }

        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(ipc::PIPE_NAME, 200);
            Sleep(50);
            continue;
        }
        if (err == ERROR_FILE_NOT_FOUND) {
            Sleep(100);
            continue;
        }

        FACELOGIN_ERROR(L"CreateFile on pipe failed: %lu", err);
        return false;
    }
}

bool PipeClient::ProbeServiceAvailable() {
    HANDLE h = CreateFileW(
        ipc::PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return true;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY) return true;
    if (err == ERROR_FILE_NOT_FOUND) {
        FACELOGIN_INFO(L"ProbeServiceAvailable: service pipe not found — service down");
        return false;
    }
    FACELOGIN_WARN(L"ProbeServiceAvailable: CreateFile err=%lu (treating as available)", err);
    return true;
}

bool PipeClient::SendMessage(const std::wstring& message) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    EnterCriticalSection(&m_cs);
    if (m_connected) pipe = m_hPipe;
    LeaveCriticalSection(&m_cs);
    if (pipe == INVALID_HANDLE_VALUE || IsStopping()) return false;

    const DWORD byteSize = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));
    DWORD bytesWritten = 0;
    const BOOL result = WriteFile(pipe, message.c_str(), byteSize, &bytesWritten, nullptr);
    if (!result) {
        const DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
            err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_OPERATION_ABORTED) {
            FACELOGIN_WARN(L"Pipe broken during send: %lu", err);
        } else {
            FACELOGIN_ERROR(L"WriteFile on pipe failed: %lu", err);
        }
        MarkDisconnected();
        return false;
    }
    return bytesWritten == byteSize;
}

bool PipeClient::IsTerminalMessage(const std::wstring& msg) {
    return msg.starts_with(ipc::MSG_AUTH_SUCCESS_PREFIX) ||
           msg.starts_with(ipc::MSG_AUTH_ERROR_PREFIX) ||
           msg == ipc::MSG_AUTH_TIMEOUT ||
           msg == ipc::MSG_AUTH_NO_FACE ||
           msg == ipc::MSG_AUTH_NO_MATCH ||
           msg == ipc::MSG_AUTH_CANCELLED;
}

DWORD WINAPI PipeClient::ReadThreadProc(LPVOID param) {
    auto* self = static_cast<PipeClient*>(param);

    while (!self->IsStopping()) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        EnterCriticalSection(&self->m_cs);
        pipe = self->m_hPipe;
        LeaveCriticalSection(&self->m_cs);
        if (pipe == INVALID_HANDLE_VALUE) break;

        DWORD bytesAvail = 0;
        DWORD totalBytes = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytesAvail, &totalBytes)) {
            if (bytesAvail == 0) {
                Sleep(10);
                continue;
            }
        } else {
            const DWORD err = GetLastError();
            if (!self->IsStopping()) {
                FACELOGIN_WARN(L"Background read failed: %lu", err);
                self->MarkDisconnected();
                OnResponseCallback callback;
                EnterCriticalSection(&self->m_cs);
                callback = self->m_onResponse;
                LeaveCriticalSection(&self->m_cs);
                if (callback) callback(false, L"");
            }
            break;
        }

        wchar_t buffer[4096] = {};
        DWORD bytesRead = 0;
        const BOOL result = ReadFile(
            pipe, buffer, static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
            &bytesRead, nullptr);
        if (!result || bytesRead == 0) {
            const DWORD err = GetLastError();
            if (!self->IsStopping()) {
                FACELOGIN_WARN(L"Background read failed: %lu", err);
                self->MarkDisconnected();
                OnResponseCallback callback;
                EnterCriticalSection(&self->m_cs);
                callback = self->m_onResponse;
                LeaveCriticalSection(&self->m_cs);
                if (callback) callback(false, L"");
            }
            break;
        }

        size_t len = bytesRead / sizeof(wchar_t);
        while (len > 0 && buffer[len - 1] == L'\0') --len;
        const std::wstring msg(buffer, len);
        FACELOGIN_INFO(L"Background read received: %s (len=%zu)",
                       msg.substr(0, 80).c_str(), len);

        if (msg.starts_with(ipc::MSG_STATUS_PREFIX)) {
            OnStatusCallback callback;
            EnterCriticalSection(&self->m_cs);
            callback = self->m_onStatus;
            LeaveCriticalSection(&self->m_cs);
            if (callback && !self->IsStopping()) {
                callback(msg.substr(wcslen(ipc::MSG_STATUS_PREFIX)));
            }
            continue;
        }

        if (!IsTerminalMessage(msg)) continue;

        OnResponseCallback callback;
        bool deliver = false;
        EnterCriticalSection(&self->m_cs);
        if (!self->m_terminalDelivered) {
            self->m_terminalDelivered = true;
            self->m_connected = false;
            callback = self->m_onResponse;
            deliver = true;
        }
        LeaveCriticalSection(&self->m_cs);

        if (deliver && callback && !self->IsStopping()) {
            callback(true, msg);
        }
        break;
    }

    return 0;
}

bool PipeClient::StartBackgroundRead(OnResponseCallback onResponse,
                                     OnStatusCallback onStatus) {
    if (m_hReadStop) SetEvent(m_hReadStop);
    CleanupReadThread();

    EnterCriticalSection(&m_cs);
    const bool canStart = m_connected && m_hPipe != INVALID_HANDLE_VALUE;
    if (canStart) {
        m_onResponse = std::move(onResponse);
        m_onStatus = std::move(onStatus);
        m_terminalDelivered = false;
        ResetEvent(m_hReadStop);
    }
    LeaveCriticalSection(&m_cs);
    if (!canStart) return false;

    EnterCriticalSection(&m_cs);
    m_hReadThread = CreateThread(nullptr, 0, ReadThreadProc, this, 0, nullptr);
    const bool created = m_hReadThread != nullptr;
    LeaveCriticalSection(&m_cs);
    if (!created) {
        FACELOGIN_ERROR(L"Failed to create pipe read thread: %lu", GetLastError());
        MarkDisconnected();
    }
    return created;
}

void PipeClient::Disconnect() {
    if (m_hReadStop) SetEvent(m_hReadStop);

    // Join before closing the pipe handle. This removes the old window where
    // the reader could still access PipeClient after its handle was closed.
    CleanupReadThread();

    HANDLE pipe = INVALID_HANDLE_VALUE;
    EnterCriticalSection(&m_cs);
    pipe = m_hPipe;
    m_hPipe = INVALID_HANDLE_VALUE;
    m_connected = false;
    m_terminalDelivered = true;
    m_onResponse = nullptr;
    m_onStatus = nullptr;
    LeaveCriticalSection(&m_cs);

    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}

} // namespace facelogin
