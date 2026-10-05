#pragma once

#include <string>
#include <string_view>
#include <mutex>
#include <iostream>
#include <sstream>
#include <fstream>

namespace imagine {

enum class LogLevel {
    Debug = 0,
    Info,
    Warn,
    Error,
    None
};

enum class LogFormat {
    Text,   // [2026-10-04 13:00:00.123] [INFO ] [file.cpp:42] message
    Json    // {"ts":"2026-10-04T13:00:00.123Z","level":"INFO","file":"file.cpp","line":42,"msg":"..."}
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const;

    static constexpr size_t kDefaultMaxLogFileSize = 10 * 1024 * 1024; // 10MB
    static constexpr size_t kDefaultMaxLogFiles = 3;                  // 3 rotated files

    // File output with rotation. Returns false if the file could not be opened.
    bool setLogFile(const std::string& path,
                    size_t maxFileSize = kDefaultMaxLogFileSize,
                    size_t maxFiles = kDefaultMaxLogFiles);
    void setFileFormat(LogFormat fmt);
    void closeLogFile();

    // Rotation control
    void setRotation(size_t maxFileSize, size_t maxFiles);
    size_t maxFileSize() const;
    size_t maxFiles() const;
    std::string logFilePath() const;
    bool isLogFileOpen() const;

    // Console control
    void setConsoleEnabled(bool enabled);
    void setConsoleColors(bool enabled);

    void log(LogLevel level, std::string_view file, int line, std::string_view message);

    // Utility: parse a level name string (debug/info/warn/error/none, case-insensitive)
    static LogLevel parseLevel(std::string_view name);

private:
    Logger();
    ~Logger();

    std::string formatText(LogLevel level, std::string_view file, int line,
                           std::string_view message, bool withColor) const;
    std::string formatJson(LogLevel level, std::string_view file, int line,
                           std::string_view message) const;

    static const char* levelStr(LogLevel level);
    static std::string escapeJson(std::string_view sv);
    void rotateLogFiles();

    LogLevel level_{LogLevel::Info};
    LogFormat fileFormat_{LogFormat::Json};
    bool consoleEnabled_{true};
    bool consoleColors_{true};
    std::string logFilePath_;
    std::ofstream logFile_;
    size_t currentFileSize_{0};
    size_t maxFileSize_{kDefaultMaxLogFileSize};
    size_t maxFiles_{kDefaultMaxLogFiles};
    mutable std::mutex mutex_;
};

#define IMAGINE_LOG_DEBUG(msg) ::imagine::Logger::instance().log(::imagine::LogLevel::Debug, __FILE__, __LINE__, (msg))
#define IMAGINE_LOG_INFO(msg)  ::imagine::Logger::instance().log(::imagine::LogLevel::Info,  __FILE__, __LINE__, (msg))
#define IMAGINE_LOG_WARN(msg)  ::imagine::Logger::instance().log(::imagine::LogLevel::Warn,  __FILE__, __LINE__, (msg))
#define IMAGINE_LOG_ERROR(msg) ::imagine::Logger::instance().log(::imagine::LogLevel::Error, __FILE__, __LINE__, (msg))

} // namespace imagine
