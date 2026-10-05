#include "imagine/common/logger.hpp"
#include "imagine/common/types.hpp"
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace imagine {

#if defined(_WIN32)
static void writeConsoleUtf8(HANDLE hConsole, std::string_view text) {
    if (text.empty()) return;
    std::cout.flush();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::string sanitized;
    if (wlen <= 0) {
        sanitized = sanitizeUtf8(text);
        wlen = MultiByteToWideChar(CP_UTF8, 0, sanitized.data(), static_cast<int>(sanitized.size()), nullptr, 0);
        text = sanitized;
    }
    if (wlen > 0) {
        std::wstring wstr(wlen, L'\0');
        if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wstr.data(), wlen) > 0) {
            const size_t chunkSize = 16384;
            for (size_t offset = 0; offset < wstr.size(); offset += chunkSize) {
                DWORD toWrite = static_cast<DWORD>(std::min(chunkSize, wstr.size() - offset));
                DWORD written = 0;
                if (!WriteConsoleW(hConsole, wstr.data() + offset, toWrite, &written, nullptr)) {
                    std::cout.write(text.data(), text.size());
                    std::cout.flush();
                    return;
                }
            }
            return;
        }
    }
    std::cout.write(text.data(), text.size());
    std::cout.flush();
}
#endif

static std::string_view extractFilename(std::string_view file) {
    auto pos = file.find_last_of("/\\");
    return (pos == std::string_view::npos) ? file : file.substr(pos + 1);
}

Logger::Logger() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            if (!SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                consoleColors_ = false;
            }
        }
    }
#endif
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

static std::filesystem::path rotatedPath(const std::string& basePath, size_t index) {
    return pathFromUtf8(basePath + "." + std::to_string(index));
}

// Enumerate and delete all rotated backups whose index exceeds keepCount.
// The active log file (basePath itself) is never touched here.
// Must be called with mutex_ held.
static void pruneStaleBackups(const std::string& basePath, size_t keepCount) {
    std::error_code ec;
    auto fsBase = pathFromUtf8(basePath);
    auto parent = fsBase.parent_path();
    auto stem = fsBase.filename();
    if (parent.empty()) { parent = std::filesystem::current_path(ec); }
    for (auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        auto name = entry.path().filename();
        // Match: <stem>.<N> where N > keepCount
        auto nameStr = name.string();
        auto stemStr = stem.string();
        if (nameStr.size() <= stemStr.size() + 1) continue;
        if (nameStr.substr(0, stemStr.size() + 1) != stemStr + ".") continue;
        auto suffix = nameStr.substr(stemStr.size() + 1);
        bool allDigits = !suffix.empty() &&
            std::all_of(suffix.begin(), suffix.end(), [](unsigned char c){ return std::isdigit(c); });
        if (!allDigits) continue;
        size_t idx = 0;
        try { idx = std::stoul(suffix); } catch (...) { continue; }
        if (idx > keepCount) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

void Logger::rotateLogFiles() {
    // NOTE: must be called with mutex_ held; does not acquire it.
    if (logFilePath_.empty()) {
        return;
    }

    if (logFile_.is_open()) {
        logFile_.flush();
        logFile_.close();
    }
    logFile_.clear();

    std::error_code ec;
    auto fsPath = pathFromUtf8(logFilePath_);

    if (maxFiles_ > 0) {
        // Delete any stale rotated files whose index exceeds maxFiles_
        pruneStaleBackups(logFilePath_, maxFiles_);

        // Delete the oldest backup (e.g. .3) if it exists
        auto oldest = rotatedPath(logFilePath_, maxFiles_);
        if (std::filesystem::exists(oldest, ec)) {
            std::filesystem::remove(oldest, ec);
        }

        // Shift existing backups: (maxFiles_ - 1) -> maxFiles_, ..., 1 -> 2
        for (size_t i = maxFiles_ - 1; i >= 1; --i) {
            auto src = rotatedPath(logFilePath_, i);
            auto dst = rotatedPath(logFilePath_, i + 1);
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::remove(dst, ec);
                std::filesystem::rename(src, dst, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        std::filesystem::remove(src, ec);
                    }
                }
            }
        }

        // Move active log file to .1
        auto firstBackup = rotatedPath(logFilePath_, 1);
        if (std::filesystem::exists(fsPath, ec)) {
            std::filesystem::remove(firstBackup, ec);
            std::filesystem::rename(fsPath, firstBackup, ec);
            if (ec) {
                ec.clear();
                std::filesystem::copy_file(fsPath, firstBackup, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) {
                    std::filesystem::remove(fsPath, ec);
                }
            }
        }
    } else {
        // maxFiles_ == 0: unlimited backups (no shift, no delete) — just truncate active file.
        // Keeping the active file is intentional: callers who want "delete on rotate"
        // should set maxFiles_ == 1 and let the oldest-backup removal handle it.
        std::filesystem::remove(fsPath, ec);
    }

    auto parent = fsPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    logFile_.open(fsPath, std::ios::out | std::ios::trunc);
    currentFileSize_ = 0;

    if (!logFile_.is_open()) {
        // Rotation succeeded but reopen failed (permissions, disk full, …).
        // Emit a one-time diagnostic to stderr so the problem is not invisible.
        std::cerr << "[imagine logger] WARNING: failed to reopen log file after rotation: "
                  << logFilePath_ << "\n";
        // logFilePath_ is preserved so the next log() call can attempt reopening.
    }
}

Logger::~Logger() {
    if (logFile_.is_open()) {
        logFile_.flush();
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

bool Logger::setLogFile(const std::string& path, size_t maxFileSize, size_t maxFiles) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.flush();
        logFile_.close();
    }
    logFile_.clear();
    logFilePath_.clear();
    currentFileSize_ = 0;
    maxFileSize_ = maxFileSize;
    maxFiles_ = maxFiles;

    if (path.empty()) {
        return false;
    }
    auto fsPath = pathFromUtf8(path);
    std::error_code ec;
    if (std::filesystem::is_directory(fsPath, ec)) {
        return false;
    }
    auto parent = fsPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    logFilePath_ = path;

    // Delete any stale old files beyond maxFiles_
    if (maxFiles_ > 0) {
        pruneStaleBackups(logFilePath_, maxFiles_);
    }

    // Check existing file size
    if (std::filesystem::exists(fsPath, ec)) {
        auto sz = std::filesystem::file_size(fsPath, ec);
        currentFileSize_ = ec ? 0 : static_cast<size_t>(sz);
    }

    if (maxFileSize_ > 0 && currentFileSize_ >= maxFileSize_) {
        rotateLogFiles();
    } else {
        logFile_.open(fsPath, std::ios::app);
    }

    return logFile_.is_open();
}

void Logger::setFileFormat(LogFormat fmt) {
    std::lock_guard<std::mutex> lock(mutex_);
    fileFormat_ = fmt;
}

LogFormat Logger::fileFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fileFormat_;
}

void Logger::closeLogFile() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.flush();
        logFile_.close();
    }
    logFile_.clear();
    logFilePath_.clear();
    currentFileSize_ = 0;
}

void Logger::setRotation(size_t maxFileSize, size_t maxFiles) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxFileSize_ = maxFileSize;
    maxFiles_ = maxFiles;
}

size_t Logger::maxFileSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxFileSize_;
}

size_t Logger::maxFiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxFiles_;
}

std::string Logger::logFilePath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return logFilePath_;
}

bool Logger::isLogFileOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return logFile_.is_open();
}

void Logger::setConsoleEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleEnabled_ = enabled;
}

void Logger::setConsoleColors(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleColors_ = enabled;
}

bool Logger::isConsoleEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consoleEnabled_;
}

bool Logger::consoleColors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consoleColors_;
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
#if defined(_WIN32)
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif

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

    std::string_view filename = extractFilename(file);

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

    std::string_view filename = extractFilename(file);

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
#if defined(_WIN32)
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        bool isConsole = (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode));
        std::string formatted = formatText(level, file, line, message, consoleColors_ && isConsole);
        if (isConsole) {
            writeConsoleUtf8(hOut, formatted);
        } else {
            std::cout.write(formatted.data(), formatted.size());
            std::cout.flush();
        }
#else
        std::string formatted = formatText(level, file, line, message, consoleColors_);
        std::cout.write(formatted.data(), formatted.size());
        std::cout.flush();
#endif
    }

    if (!logFilePath_.empty()) {
        if (!logFile_.is_open()) {
            auto fsPath = pathFromUtf8(logFilePath_);
            logFile_.open(fsPath, std::ios::app);
            if (logFile_.is_open()) {
                std::error_code ec;
                auto sz = std::filesystem::file_size(fsPath, ec);
                currentFileSize_ = ec ? 0 : static_cast<size_t>(sz);
            }
        }

        if (logFile_.is_open()) {
            std::string formatted;
            if (fileFormat_ == LogFormat::Json) {
                formatted = formatJson(level, file, line, message);
                formatted += '\n';
            } else {
                formatted = formatText(level, file, line, message, false);
            }

            if (maxFileSize_ > 0 && (currentFileSize_ + formatted.size() > maxFileSize_)) {
                if (currentFileSize_ > 0) {
                    rotateLogFiles();
                }
            }

            if (logFile_.is_open()) {
                logFile_.write(formatted.data(), formatted.size());
                logFile_.flush();
                currentFileSize_ += formatted.size();
            }
        }
    }
}

} // namespace imagine
