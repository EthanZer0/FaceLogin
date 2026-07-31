#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace facelogin {

// Simple file/console logger for debugging.
// In production (credential provider), messages go to a log file.
// In test/CLI programs, messages go to stdout or DebugOutput.

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger& Instance();

    void SetLogFile(const std::wstring& path);
    void SetMinLevel(LogLevel level);
    void SetEnableDebugOutput(bool enable) { m_debugOutput = enable; }

    void Log(LogLevel level, const wchar_t* format, ...);
    void Debug(const wchar_t* format, ...);
    void Info(const wchar_t* format, ...);
    void Warning(const wchar_t* format, ...);
    void Error(const wchar_t* format, ...);

    // In-memory ring buffer for UI log viewer
    std::vector<std::wstring> GetRecentLogs(size_t maxLines = 500);
    void ClearLogs();

private:
    Logger() = default;
    void WriteToFile(const std::wstring& line);
    void AppendToRingBuffer(const std::wstring& line);

    HANDLE m_hFile = INVALID_HANDLE_VALUE;
    LogLevel m_minLevel = LogLevel::Info;
    bool m_debugOutput = false;
    CRITICAL_SECTION m_cs{};
    bool m_csInitialized = false;

    // Ring buffer for UI log viewer (cursor wraps when full)
    static constexpr size_t RING_SIZE = 2000;
    std::vector<std::wstring> m_ringBuffer;
    size_t m_ringPos = 0;          // next write position
    size_t m_ringCount = 0;        // total entries written
    CRITICAL_SECTION m_ringCs{};
    bool m_ringCsInitialized = false;
};

// Convenience macros for file/line info
#define FACELOGIN_LOG(level, fmt, ...) \
    facelogin::Logger::Instance().Log(level, L"[%s:%d] " fmt, __FUNCTIONW__, __LINE__, ##__VA_ARGS__)

#define FACELOGIN_DEBUG(fmt, ...) FACELOGIN_LOG(facelogin::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define FACELOGIN_INFO(fmt, ...)  FACELOGIN_LOG(facelogin::LogLevel::Info, fmt, ##__VA_ARGS__)
#define FACELOGIN_WARN(fmt, ...)  FACELOGIN_LOG(facelogin::LogLevel::Warning, fmt, ##__VA_ARGS__)
#define FACELOGIN_ERROR(fmt, ...) FACELOGIN_LOG(facelogin::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace facelogin
