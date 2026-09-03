#pragma once

#include <string>
#include <string_view>
#include <mutex>
#include <iostream>
#include <sstream>

namespace imagine {

enum class LogLevel {
    Debug = 0,
    Info,
    Warn,
    Error,
    None
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const;

    void log(LogLevel level, std::string_view file, int line, std::string_view message);

private:
    Logger() = default;
    LogLevel level_{LogLevel::Info};
    mutable std::mutex mutex_;
};

#define IMAGINE_LOG_DEBUG(msg) ::imagine::Logger::instance().log(::imagine::LogLevel::Debug, __FILE__, __LINE__, (msg))
#define IMAGINE_LOG_INFO(msg)  ::imagine::Logger::instance().log(::imagine::LogLevel::Info,  __FILE__, __LINE__, (msg))
#define IMAGINE_LOG_WARN(msg)  ::imagine::Logger::instance().log(::imagine::LogLevel::Warn,  __FILE__, __LINE__, (msg))
#define IMAGINE_LOG_ERROR(msg) ::imagine::Logger::instance().log(::imagine::LogLevel::Error, __FILE__, __LINE__, (msg))

} // namespace imagine
