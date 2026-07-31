#include "logger.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace facelogin {

Logger& Logger::Instance() {
    static Logger s_instance;
    if (!s_instance.m_csInitialized) {
        InitializeCriticalSection(&s_instance.m_cs);
        s_instance.m_csInitialized = true;
    }
    if (!s_instance.m_ringCsInitialized) {
        InitializeCriticalSection(&s_instance.m_ringCs);
        s_instance.m_ringBuffer.resize(RING_SIZE);
        s_instance.m_ringCsInitialized = true;
    }
    return s_instance;
}

void Logger::SetLogFile(const std::wstring& path) {
    EnterCriticalSection(&m_cs);
    // Ensure parent directory exists
    {
        std::wstring dir = path;
        size_t pos = dir.rfind(L'\\');
        if (pos != std::wstring::npos) {
            dir = dir.substr(0, pos);
            CreateDirectoryW(dir.c_str(), nullptr);
        }
    }
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
    }
    m_hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LeaveCriticalSection(&m_cs);
}

void Logger::SetMinLevel(LogLevel level) {
    m_minLevel = level;
}

void Logger::Log(LogLevel level, const wchar_t* format, ...) {
    if (level < m_minLevel) return;

    va_list args;
    va_start(args, format);

    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);

    // Timestamp prefix
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t finalMsg[2560];
    const wchar_t* levelStr = L"";
    switch (level) {
        case LogLevel::Debug:   levelStr = L"DEBUG"; break;
        case LogLevel::Info:    levelStr = L"INFO"; break;
        case LogLevel::Warning: levelStr = L"WARN"; break;
        case LogLevel::Error:   levelStr = L"ERROR"; break;
    }

    _snwprintf_s(finalMsg, _TRUNCATE,
                  L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] [%lu] %s\r\n",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  levelStr, GetCurrentThreadId(), buffer);

    // Output to debugger
    if (m_debugOutput) {
        OutputDebugStringW(finalMsg);
    }

    // Write to file
    WriteToFile(finalMsg);

    // Ring buffer for UI — store WITHOUT trailing \r\n for cleaner display
    {
        std::wstring clean = finalMsg;
        while (!clean.empty() && (clean.back() == L'\r' || clean.back() == L'\n'))
            clean.pop_back();
        AppendToRingBuffer(clean);
    }
}

void Logger::Debug(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Debug, L"%s", buffer);
}

void Logger::Info(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Info, L"%s", buffer);
}

void Logger::Warning(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Warning, L"%s", buffer);
}

void Logger::Error(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Error, L"%s", buffer);
}

void Logger::WriteToFile(const std::wstring& line) {
    EnterCriticalSection(&m_cs);
    if (m_hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(m_hFile, line.c_str(),
                  static_cast<DWORD>(line.size() * sizeof(wchar_t)),
                  &written, nullptr);
        FlushFileBuffers(m_hFile);
    }
    LeaveCriticalSection(&m_cs);
}

void Logger::AppendToRingBuffer(const std::wstring& line) {
    EnterCriticalSection(&m_ringCs);
    if (m_ringBuffer.empty()) {
        m_ringBuffer.resize(RING_SIZE);
    }
    m_ringBuffer[m_ringPos] = line;
    m_ringPos = (m_ringPos + 1) % RING_SIZE;
    m_ringCount++;
    LeaveCriticalSection(&m_ringCs);
}

std::vector<std::wstring> Logger::GetRecentLogs(size_t maxLines) {
    std::vector<std::wstring> result;
    EnterCriticalSection(&m_ringCs);
    if (m_ringCount == 0) {
        LeaveCriticalSection(&m_ringCs);
        return result;
    }
    size_t count = m_ringCount;
    if (count > RING_SIZE) count = RING_SIZE;
    if (count > maxLines) count = maxLines;
    result.reserve(count);
    // Read from the oldest entry. After ring wraps, pos points to oldest.
    size_t start = (m_ringCount <= RING_SIZE) ? 0 : m_ringPos;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % RING_SIZE;
        result.push_back(m_ringBuffer[idx]);
    }
    LeaveCriticalSection(&m_ringCs);
    return result;
}

void Logger::ClearLogs() {
    EnterCriticalSection(&m_ringCs);
    m_ringBuffer.clear();
    m_ringBuffer.resize(RING_SIZE);
    m_ringPos = 0;
    m_ringCount = 0;
    LeaveCriticalSection(&m_ringCs);
}

} // namespace facelogin
