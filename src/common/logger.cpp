#include "imagine/common/logger.hpp"
#include <chrono>
#include <iomanip>
#include <filesystem>

namespace imagine {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::log(LogLevel level, std::string_view file, int line, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level < level_ || level_ == LogLevel::None) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&time_t_now, &tm_buf);

    const char* level_str = "INFO";
    const char* color_code = "\033[0m";
    switch (level) {
        case LogLevel::Debug:
            level_str = "DEBUG";
            color_code = "\033[36m"; // Cyan
            break;
        case LogLevel::Info:
            level_str = "INFO ";
            color_code = "\033[32m"; // Green
            break;
        case LogLevel::Warn:
            level_str = "WARN ";
            color_code = "\033[33m"; // Yellow
            break;
        case LogLevel::Error:
            level_str = "ERROR";
            color_code = "\033[31m"; // Red
            break;
        default:
            break;
    }

    std::string filename = std::filesystem::path(file).filename().string();

    std::cout << color_code << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << std::setfill(' ') << "] ["
              << level_str << "] [" << filename << ":" << line << "] "
              << message << "\033[0m\n";
}

} // namespace imagine
