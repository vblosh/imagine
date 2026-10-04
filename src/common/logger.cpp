#include "imagine/common/logger.hpp"
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace imagine {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    if (logFile_.is_open()) {
        logFile_.close();
    }
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

bool Logger::setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.close();
    }
    logFile_.clear();
    logFile_.open(path, std::ios::app);
    return logFile_.is_open();
}

void Logger::setFileFormat(LogFormat fmt) {
    std::lock_guard<std::mutex> lock(mutex_);
    fileFormat_ = fmt;
}

void Logger::closeLogFile() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.close();
    }
}

void Logger::setConsoleEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleEnabled_ = enabled;
}

void Logger::setConsoleColors(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleColors_ = enabled;
}

const char* Logger::levelStr(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

std::string Logger::escapeJson(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 8);
    for (size_t i = 0; i < sv.size(); ++i) {
        const char c = sv[i];
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x80) {
            // Accept only Unicode scalar values encoded with the shortest UTF-8 sequence.
            size_t length = byte >= 0xc2 && byte <= 0xdf ? 2
                          : byte >= 0xe0 && byte <= 0xef ? 3
                          : byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
            bool valid = length != 0 && length <= sv.size() - i;
            for (size_t j = 1; valid && j < length; ++j) {
                const auto next = static_cast<unsigned char>(sv[i + j]);
                valid = next >= 0x80 && next <= 0xbf;
                if (j == 1) {
                    valid = valid && !(byte == 0xe0 && next < 0xa0)
                                  && !(byte == 0xed && next >= 0xa0)
                                  && !(byte == 0xf0 && next < 0x90)
                                  && !(byte == 0xf4 && next >= 0x90);
                }
            }
            if (valid) {
                out.append(sv.substr(i, length));
                i += length - 1;
            } else {
                out += "\\ufffd";
            }
            continue;
        }
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

LogLevel Logger::parseLevel(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "info")  return LogLevel::Info;
    if (lower == "warn" || lower == "warning") return LogLevel::Warn;
    if (lower == "error") return LogLevel::Error;
    if (lower == "none" || lower == "off") return LogLevel::None;
    return LogLevel::Info; // default fallback
}

std::string Logger::formatText(LogLevel level, std::string_view file, int line,
                                std::string_view message, bool withColor) const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    if (const std::tm* tm_ptr = std::localtime(&time_t_now)) {
        tm_buf = *tm_ptr;
    }

    const char* color_code = "\033[0m";
    if (withColor) {
        switch (level) {
            case LogLevel::Debug: color_code = "\033[36m"; break; // Cyan
            case LogLevel::Info:  color_code = "\033[32m"; break; // Green
            case LogLevel::Warn:  color_code = "\033[33m"; break; // Yellow
            case LogLevel::Error: color_code = "\033[31m"; break; // Red
            default: break;
        }
    }

    std::string filename = std::filesystem::path(file).filename().string();

    std::ostringstream oss;
    if (withColor) {
        oss << color_code;
    }
    oss << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count() << std::setfill(' ')
        << "] [" << levelStr(level) << "] [" << filename << ":" << line << "] "
        << message;
    if (withColor) {
        oss << "\033[0m";
    }
    oss << "\n";
    return oss.str();
}

std::string Logger::formatJson(LogLevel level, std::string_view file, int line,
                                std::string_view message) const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    std::string filename = std::filesystem::path(file).filename().string();

    // Trim trailing space from level string for JSON
    std::string lvl = levelStr(level);
    while (!lvl.empty() && lvl.back() == ' ') {
        lvl.pop_back();
    }

    std::ostringstream oss;
    oss << "{\"ts\":\"" << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z\""
        << ",\"level\":\"" << lvl << "\""
        << ",\"file\":\"" << escapeJson(filename) << "\""
        << ",\"line\":" << line
        << ",\"msg\":\"" << escapeJson(message) << "\"}";
    return oss.str();
}

void Logger::log(LogLevel level, std::string_view file, int line, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level < level_ || level_ == LogLevel::None) {
        return;
    }

    if (consoleEnabled_) {
        std::cout << formatText(level, file, line, message, consoleColors_) << std::flush;
    }

    if (logFile_.is_open()) {
        if (fileFormat_ == LogFormat::Json) {
            logFile_ << formatJson(level, file, line, message) << '\n' << std::flush;
        } else {
            logFile_ << formatText(level, file, line, message, false) << std::flush;
        }
    }
}

} // namespace imagine
