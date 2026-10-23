#include "imagine/server/api_router.hpp"
#include "imagine/server/mime_types.hpp"
#include "imagine/core/catalog.hpp"
#include "imagine/core/query.hpp"
#include "imagine/core/importer.hpp"
#include "imagine/core/event_suggestions.hpp"
#include "imagine/metadata/hasher.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/common/logger.hpp"
#include "imagine/core/deduplicator.hpp"
#include "imagine/clip/service.hpp"
#include "imagine/metadata/media_reader.hpp"
#include <atomic>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cmath>

namespace imagine::server {

namespace {

std::vector<uint8_t> base64Decode(const std::string& input) {
    size_t commaPos = input.find(',');
    std::string_view sv = (commaPos != std::string::npos) ? std::string_view(input).substr(commaPos + 1) : std::string_view(input);

    static const int b64Table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::vector<uint8_t> out;
    out.reserve((sv.size() * 3) / 4);
    uint32_t val = 0;
    int valb = -8;
    for (char c : sv) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
        int d = b64Table[static_cast<unsigned char>(c)];
        if (d < 0) continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

int64_t daysFromCivil(int y, int m, int d) noexcept {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    y += (m <= 2);
}

bool constantTimeEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

int64_t parseDateStringToTimestamp(const std::string& dateStr) {
    if (dateStr.empty()) return 0;
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    char sep1 = 0, sep2 = 0, sep3 = 0, sep4 = 0, sep5 = 0;
    if (std::sscanf(dateStr.c_str(), "%d%c%d%c%d%c%d%c%d%c%d",
                    &year, &sep1, &month, &sep2, &day, &sep3, &hour, &sep4, &min, &sep5, &sec) >= 9) {
        // Parsed at least year, month, day, hour, min
    } else if (std::sscanf(dateStr.c_str(), "%d%c%d%c%d", &year, &sep1, &month, &sep2, &day) == 5) {
        hour = 0; min = 0; sec = 0;
    } else {
        return 0;
    }

    if (year < 1800 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return 0;
    }

    const int64_t days = daysFromCivil(year, month, day);
    return days * 86400 + static_cast<int64_t>(hour) * 3600 +
           static_cast<int64_t>(min) * 60 + static_cast<int64_t>(sec);
}

std::string formatTimestampToDateString(int64_t timestamp) {
    if (timestamp == 0) return "";
    int64_t days = timestamp / 86400;
    int64_t rem = timestamp % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    int year = 0, month = 0, day = 0;
    civilFromDays(days, year, month, day);
    const int hour = static_cast<int>(rem / 3600);
    rem %= 3600;
    const int min = static_cast<int>(rem / 60);
    const int sec = static_cast<int>(rem % 60);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d",
                  year, month, day, hour, min, sec);
    return std::string(buf);
}

template <typename T>
bool inRange(long long val) {
    if constexpr (std::is_signed_v<T>) {
        return val >= static_cast<long long>(std::numeric_limits<T>::min()) &&
               val <= static_cast<long long>(std::numeric_limits<T>::max());
    } else {
        if (val < 0) return false;
        return static_cast<unsigned long long>(val) <= static_cast<unsigned long long>(std::numeric_limits<T>::max());
    }
}

template <typename T>
bool parseInt(const std::string& str, T& out) {
    try {
        size_t idx = 0;
        auto v = std::stoll(str, &idx);
        if (idx != str.size()) return false;
        if (!inRange<T>(v)) return false;
        out = static_cast<T>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseDouble(const std::string& str, double& out) {
    try {
        size_t idx = 0;
        double v = std::stod(str, &idx);
        if (idx != str.size() || !std::isfinite(v)) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<int> parseQueryInt(const httplib::Request& req, const char* key, int fallback,
                                 int minVal = std::numeric_limits<int>::min(),
                                 int maxVal = std::numeric_limits<int>::max()) {
    if (!req.has_param(key)) return fallback;
    try {
        std::string value = req.get_param_value(key);
        size_t used = 0;
        long long val = std::stoll(value, &used);
        if (used != value.size()) return std::nullopt;
        if (val < minVal || val > maxVal) return std::nullopt;
        return static_cast<int>(val);
    } catch (...) {
        return std::nullopt;
    }
}

void sendJson(httplib::Response& res, const nlohmann::json& j, int status = 200) {
    res.status = status;
    try {
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    } catch (const std::exception& ex) {
        IMAGINE_LOG_ERROR("JSON serialization failed: " + std::string(ex.what()));
        res.status = 500;
        res.set_content(R"({"error":"Internal JSON serialization error"})", "application/json");
    }
}

void sendError(httplib::Response& res, const std::string& message, int status = 400) {
    if (status >= 500) {
        IMAGINE_LOG_ERROR("HTTP " + std::to_string(status) + ": " + message);
    } else if (status >= 400) {
        IMAGINE_LOG_WARN("HTTP " + std::to_string(status) + ": " + message);
    }
    nlohmann::json err = {{"error", sanitizeUtf8(message)}};
    res.status = status;
    try {
        res.set_content(err.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    } catch (...) {
        res.set_content(R"({"error":"Error"})", "application/json");
    }
}

template <typename T = int64_t>
std::optional<T> parseMatchId(const httplib::Request& req, size_t matchIndex, httplib::Response& res, const std::string& fieldName = "ID") {
    try {
        size_t used = 0;
        const std::string& str = req.matches[matchIndex];
        long long val = std::stoll(str, &used);
        if (used != str.size() || !inRange<T>(val) || val <= 0) {
            sendError(res, "Invalid " + fieldName, 400);
            return std::nullopt;
        }
        return static_cast<T>(val);
    } catch (...) {
        sendError(res, "Invalid " + fieldName, 400);
        return std::nullopt;
    }
}

constexpr size_t kMaxBatchSize = 1000;
constexpr size_t kMaxSearchLength = 500;
constexpr size_t kMaxFolderLength = 1000;
constexpr size_t kMaxNameLength = 100;
constexpr size_t kMaxCategoryLength = 50;
constexpr size_t kMaxAlbumNameLength = 200;
constexpr size_t kMaxAlbumDescLength = 2000;
constexpr size_t kMaxAlbumOrderSize = 100000;

int statusToHttpCode(const Status& s) {
    switch (s.code()) {
        case StatusCode::Ok:
            return 200;
        case StatusCode::NotFound:
            return 404;
        case StatusCode::AlreadyExists:
            return 409;
        case StatusCode::InvalidArgument:
        case StatusCode::ParseError:
            return 400;
        case StatusCode::IoError:
        case StatusCode::DatabaseError:
        case StatusCode::InternalError:
        default:
            return 500;
    }
}

void sendStatusError(httplib::Response& res, const Status& s) {
    sendError(res, s.message(), statusToHttpCode(s));
}


bool isSensitiveSystemPath(const std::filesystem::path& canonicalPath) {
    std::string s = canonicalPath.lexically_normal().string();
    if (s.empty()) return true;
    if (s == "/") return true;

    static const std::vector<std::string> kSensitivePrefixes = {
        "/etc", "/proc", "/sys", "/dev", "/boot", "/root",
        "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32",
        "/usr/bin", "/usr/sbin", "/usr/lib", "/var/run", "/var/lock",
        "/var/log", "/var/spool"
    };

    for (const auto& prefix : kSensitivePrefixes) {
        if (s == prefix || (s.rfind(prefix + "/", 0) == 0)) {
            return true;
        }
    }

#if defined(_WIN32)
    if (canonicalPath.has_root_path() && canonicalPath == canonicalPath.root_path()) {
        return true;
    }
    std::string sLower = s;
    std::transform(sLower.begin(), sLower.end(), sLower.begin(), [](unsigned char c) {
        return c == '/' ? '\\' : static_cast<char>(std::tolower(c));
    });
    static const std::vector<std::string> kWinSensitivePrefixes = {
        "\\windows", "\\system32", "\\syswow64", "\\program files", "\\program files (x86)", "\\programdata"
    };
    std::string noDrive = sLower;
    if (noDrive.size() >= 2 && noDrive[1] == ':') {
        noDrive = noDrive.substr(2);
    }
    for (const auto& prefix : kWinSensitivePrefixes) {
        if (noDrive == prefix || (noDrive.rfind(prefix + "\\", 0) == 0)) {
            return true;
        }
    }
#endif

    return false;
}

#if defined(_WIN32)
constexpr char kImportRootsDelimiter = ';';
#else
constexpr char kImportRootsDelimiter = ':';
#endif

bool isDescendantOrEqual(const std::filesystem::path& target, const std::filesystem::path& root) {
    auto pNormal = target.lexically_normal();
    auto rNormal = root.lexically_normal();

    if (pNormal.filename() == ".") {
        pNormal = pNormal.parent_path();
    }
    if (rNormal.filename() == ".") {
        rNormal = rNormal.parent_path();
    }

    auto itP = pNormal.begin();
    auto endP = pNormal.end();
    auto itR = rNormal.begin();
    auto endR = rNormal.end();

    while (itR != endR) {
        if (itP == endP) {
            return false;
        }

#if defined(_WIN32)
        std::string sP = itP->string();
        std::string sR = itR->string();
        if (sP.size() != sR.size()) {
            return false;
        }
        for (size_t i = 0; i < sP.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(sP[i])) !=
                std::tolower(static_cast<unsigned char>(sR[i]))) {
                return false;
            }
        }
#else
        if (*itP != *itR) {
            return false;
        }
#endif
        ++itP;
        ++itR;
    }

    return true;
}

bool isWithinAllowedRoots(const std::filesystem::path& canonicalPath, std::string& outError) {
    if (isSensitiveSystemPath(canonicalPath)) {
        outError = "Importing sensitive system directory is forbidden: " + pathToUtf8(canonicalPath);
        return false;
    }

    const char* envRoots = std::getenv("IMAGINE_ALLOWED_IMPORT_ROOTS");
    if (envRoots && std::strlen(envRoots) > 0) {
        std::string envStr(envRoots);
        std::stringstream ss(envStr);
        std::string root;
        bool matched = false;
        while (std::getline(ss, root, kImportRootsDelimiter)) {
            while (!root.empty() && (root.front() == ' ' || root.front() == '\t' || root.front() == '\r' || root.front() == '\n')) {
                root.erase(0, 1);
            }
            while (!root.empty() && (root.back() == ' ' || root.back() == '\t' || root.back() == '\r' || root.back() == '\n')) {
                root.pop_back();
            }
            if (root.empty()) continue;
            std::error_code ec;
            auto rootPath = pathFromUtf8(root);
            auto cRoot = stripExtendedPrefix(std::filesystem::canonical(rootPath, ec));
            if (ec) {
                ec.clear();
                cRoot = stripExtendedPrefix(std::filesystem::weakly_canonical(rootPath, ec));
            }
            if (!ec) {
                if (isDescendantOrEqual(canonicalPath, cRoot)) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            outError = "Directory is not within allowed import roots: " + pathToUtf8(canonicalPath);
            return false;
        }
    }
    return true;
}

} // anonymous namespace

struct ApiRouter::FallbackImportState {
    std::mutex mutex;
    ImportProgress progress;
    std::atomic<bool> isImporting{false};
};

void ApiRouter::initFromEnvironment() {
    std::lock_guard<std::mutex> lock(configMutex_);
    const char* envToken = std::getenv("IMAGINE_API_TOKEN");
    if (envToken && *envToken) {
        apiToken_ = envToken;
    }
    const char* envOrigin = std::getenv("IMAGINE_ALLOWED_ORIGIN");
    if (envOrigin && *envOrigin) {
        allowedOrigin_ = envOrigin;
    }
}

ApiRouter::ApiRouter(core::Catalog& catalog)
    : catalog_(&catalog), fallbackImportState_(std::make_unique<FallbackImportState>()) {
    initFromEnvironment();
    faceService_ = std::make_unique<faces::Service>(this->db(), this->cache().cacheDir(),
        [this](const std::string& path) { return resolvePhotoPath(path); });
    clipService_ = std::make_unique<clip::Service>(this->db(), this->cache().cacheDir(),
        [this](const std::string& path) { return resolvePhotoPath(path); });
}

ApiRouter::ApiRouter(db::CatalogDb& db, thumbnail::Cache& cache)
    : db_(&db), cache_(&cache), fallbackImportState_(std::make_unique<FallbackImportState>()) {
    initFromEnvironment();
    faceService_ = std::make_unique<faces::Service>(this->db(), this->cache().cacheDir(),
        [this](const std::string& path) { return resolvePhotoPath(path); });
    clipService_ = std::make_unique<clip::Service>(this->db(), this->cache().cacheDir(),
        [this](const std::string& path) { return resolvePhotoPath(path); });
}

void ApiRouter::stop() {
    stopImport();
    if (clipService_) {
        clipService_->stop();
    }
    if (faceService_) {
        faceService_->stop();
    }
}

ApiRouter::~ApiRouter() {
    stop();
    clipService_.reset();
    faceService_.reset();
}

void ApiRouter::setApiToken(std::string token) {
    std::lock_guard<std::mutex> lock(configMutex_);
    apiToken_ = std::move(token);
}

std::string ApiRouter::apiToken() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return apiToken_;
}

void ApiRouter::setAllowedOrigin(std::string origin) {
    std::lock_guard<std::mutex> lock(configMutex_);
    allowedOrigin_ = std::move(origin);
}

std::string ApiRouter::allowedOrigin() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return allowedOrigin_;
}

void ApiRouter::stopImport() {
    cancelFallbackImport_.store(true);
    std::lock_guard<std::mutex> lock(importThreadMutex_);
    if (importThread_.joinable()) {
        importThread_.request_stop();
    }
    if (catalog_) {
        catalog_->cancelImport();
    }
    if (importThread_.joinable()) {
        importThread_.join();
    }
    isImporting_.store(false);
    if (fallbackImportState_) {
        fallbackImportState_->isImporting.store(false);
    }
}

bool ApiRouter::isImportRunning() const {
    if (isImporting_.load()) {
        return true;
    }
    if (catalog_) {
        return catalog_->importer().isRunning();
    }
    return fallbackImportState_ ? fallbackImportState_->isImporting.load() : false;
}

bool ApiRouter::checkAuth(const httplib::Request& req, httplib::Response& res) const {
    const std::string expectedToken = apiToken();
    if (expectedToken.empty()) {
        return true;
    }
    if (req.has_header("Authorization")) {
        auto auth = req.get_header_value("Authorization");
        constexpr std::string_view kBearerPrefix = "Bearer ";
        if (auth.size() > kBearerPrefix.size() &&
            std::string_view(auth).substr(0, kBearerPrefix.size()) == kBearerPrefix &&
            constantTimeEquals(std::string_view(auth).substr(kBearerPrefix.size()), expectedToken)) {
            return true;
        }
    }
    if (req.has_header("X-API-Key")) {
        auto key = req.get_header_value("X-API-Key");
        if (constantTimeEquals(key, expectedToken)) return true;
    }
    if (req.has_param("api_token")) {
        auto key = req.get_param_value("api_token");
        if (constantTimeEquals(key, expectedToken)) return true;
    }
    IMAGINE_LOG_WARN("Authentication failed for " + req.method + " " + req.path +
                     " from " + req.remote_addr);
    sendError(res, "Unauthorized: valid API token required", 401);
    return false;
}

db::CatalogDb& ApiRouter::db() const {
    if (db_) return *db_;
    if (catalog_) return catalog_->db();
    throw std::runtime_error("No CatalogDb available in ApiRouter");
}

thumbnail::Cache& ApiRouter::cache() const {
    if (cache_) return *cache_;
    if (catalog_) return catalog_->cache();
    throw std::runtime_error("No Thumbnail Cache available in ApiRouter");
}

std::string ApiRouter::resolvePhotoPath(const std::string& recordedPath) const {
    if (catalog_) {
        return catalog_->resolvePhotoPath(recordedPath);
    }
    return recordedPath;
}

Status ApiRouter::moveMedia(MediaId id, const std::string& destinationPath, std::string* outNewFilePath) {
    if (!catalog_) {
        return Status::internal("Catalog is required to move media");
    }
    return catalog_->moveMedia(id, destinationPath, outNewFilePath);
}

void ApiRouter::registerRoutes(httplib::Server& server) {
    registerCorsHandler(server);
    registerAccessLog(server);
    registerMediaRoutes(server);
    registerTagRoutes(server);
    registerAlbumRoutes(server);
    registerThumbnailRoutes(server);
    registerTimelineRoutes(server);
    registerStatsRoutes(server);
    registerFolderRoutes(server);
    registerImportRoutes(server);
    registerGeocodeRoutes(server);
    registerFaceRoutes(server);
    registerSemanticRoutes(server);
    registerDuplicateRoutes(server);
    registerSmartTagRoutes(server);
}

void ApiRouter::registerCorsHandler(httplib::Server& server) {
    server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        // Record request start time for access log duration measurement
        auto requestStart = std::chrono::steady_clock::now();
        res.set_header("X-Internal-Start-Ns",
            std::to_string(requestStart.time_since_epoch().count()));

        static std::atomic<uint64_t> s_reqCounter{0};
        std::string requestId;
        if (req.has_header("X-Request-ID")) {
            std::string candidate = req.get_header_value("X-Request-ID");
            if (!candidate.empty() && candidate.size() <= 128 &&
                std::all_of(candidate.begin(), candidate.end(), [](unsigned char c) {
                    return c >= 0x20 && c <= 0x7E;
                })) {
                requestId = std::move(candidate);
            }
        }
        if (requestId.empty()) {
            requestId = "req-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
                        "-" + std::to_string(++s_reqCounter);
        }
        res.set_header("X-Request-ID", requestId);

        std::string origin = allowedOrigin();
        if (origin != "*" && req.has_header("Origin")) {
            std::string reqOrigin = req.get_header_value("Origin");
            if (reqOrigin == origin) {
                res.set_header("Access-Control-Allow-Origin", reqOrigin);
            }
        } else {
            res.set_header("Access-Control-Allow-Origin", origin);
        }

        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Key, Accept, Origin, X-Requested-With, X-Request-ID, Range");
        res.set_header("Access-Control-Expose-Headers", "Content-Range, Accept-Ranges, ETag, X-Request-ID");

        if (req.method == "OPTIONS") {
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
}

void ApiRouter::registerAccessLog(httplib::Server& server) {
    server.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        // Compute request duration
        std::string durationStr = "-";
        if (res.has_header("X-Internal-Start-Ns")) {
            try {
                auto startNs = std::stoull(res.get_header_value("X-Internal-Start-Ns"));
                auto endNs = std::chrono::steady_clock::now().time_since_epoch().count();
                auto elapsedMs = (static_cast<unsigned long long>(endNs) - startNs) / 1000000ULL;
                durationStr = std::to_string(elapsedMs);
            } catch (const std::exception& ex) {
                IMAGINE_LOG_DEBUG("Failed to parse request start timestamp: " + std::string(ex.what()));
            }
            // Remove internal header so it's not sent to the client
            res.headers.erase("X-Internal-Start-Ns");
        }

        // Skip OPTIONS only after removing the internal timing header.
        if (req.method == "OPTIONS") return;

        // Log static file requests at Debug level to reduce noise
        auto level = LogLevel::Info;
        if (req.path.rfind("/api/", 0) != 0) {
            level = LogLevel::Debug;
        }

        // Override level for errors
        if (res.status >= 500) {
            level = LogLevel::Error;
        } else if (res.status >= 400) {
            level = LogLevel::Warn;
        }

        std::string logMsg = req.method + " " + req.path
            + " " + std::to_string(res.status)
            + " " + durationStr + "ms";

        Logger::instance().log(level, "access", 0, logMsg);
    });
}

void ApiRouter::registerMediaRoutes(httplib::Server& server) {
    // GET /api/media
    server.Get("/api/media", [this](const httplib::Request& req, httplib::Response& res) {
        core::QueryCriteria criteria;

        if (req.has_param("rating")) {
            int32_t val = 0;
            if (!parseInt(req.get_param_value("rating"), val) || val < 0 || val > 5) {
                sendError(res, "Query parameter 'rating' must be an integer between 0 and 5", 400);
                return;
            }
            criteria.min_rating = val;
        }
        if (req.has_param("max_rating")) {
            int32_t val = 0;
            if (!parseInt(req.get_param_value("max_rating"), val) || val < 0 || val > 5) {
                sendError(res, "Query parameter 'max_rating' must be an integer between 0 and 5", 400);
                return;
            }
            criteria.max_rating = val;
        }
        if (criteria.min_rating.has_value() && criteria.max_rating.has_value() &&
            *criteria.min_rating > *criteria.max_rating) {
            sendError(res, "'rating' cannot be greater than 'max_rating'", 400);
            return;
        }
        if (req.has_param("flag")) {
            int val = 0;
            if (!parseInt(req.get_param_value("flag"), val) || val < -1 || val > 1) {
                sendError(res, "Query parameter 'flag' must be -1, 0, or 1", 400);
                return;
            }
            criteria.flag = static_cast<FlagState>(val);
        }
        if (req.has_param("not_flag")) {
            int val = 0;
            if (!parseInt(req.get_param_value("not_flag"), val) || val < -1 || val > 1) {
                sendError(res, "Query parameter 'not_flag' must be -1, 0, or 1", 400);
                return;
            }
            criteria.not_flag = static_cast<FlagState>(val);
        }
        if (req.has_param("exclude_rejects")) {
            std::string ex = req.get_param_value("exclude_rejects");
            if (ex == "1" || ex == "true") {
                criteria.not_flag = FlagState::Reject;
            }
        }
        if (criteria.flag.has_value() && criteria.not_flag.has_value() &&
            criteria.flag == criteria.not_flag) {
            sendError(res, "'flag' and 'not_flag' cannot have the same value", 400);
            return;
        }
        if (req.has_param("media_type")) {
            std::string mt = req.get_param_value("media_type");
            if (mt != "photo" && mt != "video" && mt != "audio") {
                sendError(res, "Query parameter 'media_type' must be 'photo', 'video', or 'audio'", 400);
                return;
            }
            criteria.media_type = std::move(mt);
        }
        if (req.has_param("last_imported")) {
            std::string value = req.get_param_value("last_imported");
            if (value == "1" || value == "true") {
                criteria.last_imported = true;
            } else if (value == "0" || value == "false") {
                criteria.last_imported = false;
            } else {
                sendError(res, "Query parameter 'last_imported' must be 'true', 'false', '1', or '0'", 400);
                return;
            }
        }
        if (req.has_param("search")) {
            std::string search = req.get_param_value("search");
            if (search.size() > kMaxSearchLength) {
                sendError(res, "Search query exceeds maximum length", 400);
                return;
            }
            criteria.search_text = std::move(search);
        }
        if (req.has_param("folder")) {
            size_t count = req.get_param_value_count("folder");
            for (size_t i = 0; i < count; ++i) {
                std::string folder = req.get_param_value("folder", i);
                if (folder.size() > kMaxFolderLength) {
                    sendError(res, "Folder path exceeds maximum length", 400);
                    return;
                }
                criteria.folders.push_back(folder);
            }
            if (criteria.folder.empty() && !criteria.folders.empty()) {
                criteria.folder = criteria.folders.front();
            }
        }
        if (req.has_param("folders")) {
            std::string foldersStr = req.get_param_value("folders");
            if (!foldersStr.empty()) {
                if (foldersStr.front() == '[' && foldersStr.back() == ']') {
                    try {
                        auto arr = nlohmann::json::parse(foldersStr);
                        if (arr.is_array()) {
                            for (const auto& item : arr) {
                                if (item.is_string()) {
                                    std::string f = item.get<std::string>();
                                    if (f.size() > kMaxFolderLength) {
                                        sendError(res, "Folder path exceeds maximum length", 400);
                                        return;
                                    }
                                    criteria.folders.push_back(f);
                                }
                            }
                        }
                    } catch (const std::exception& ex) {
                        IMAGINE_LOG_WARN("Failed to parse folders JSON parameter: " + std::string(ex.what()));
                    }
                } else {
                    char delim = (foldersStr.find('|') != std::string::npos) ? '|' : ',';
                    std::stringstream ss(foldersStr);
                    std::string item;
                    while (std::getline(ss, item, delim)) {
                        if (!item.empty()) {
                            if (item.size() > kMaxFolderLength) {
                                sendError(res, "Folder path exceeds maximum length", 400);
                                return;
                            }
                            criteria.folders.push_back(item);
                        }
                    }
                }
            }
            if (criteria.folder.empty() && !criteria.folders.empty()) {
                criteria.folder = criteria.folders.front();
            }
        }
        if (req.has_param("tag_category")) {
            std::string tagCat = req.get_param_value("tag_category");
            if (tagCat.size() > kMaxCategoryLength) {
                sendError(res, "Tag category exceeds maximum length", 400);
                return;
            }
            criteria.tag_category = std::move(tagCat);
        } else if (req.has_param("category")) {
            std::string tagCat = req.get_param_value("category");
            if (tagCat.size() > kMaxCategoryLength) {
                sendError(res, "Category exceeds maximum length", 400);
                return;
            }
            criteria.tag_category = std::move(tagCat);
        }
        if (req.has_param("tag_id")) {
            size_t count = req.get_param_value_count("tag_id");
            for (size_t i = 0; i < count; ++i) {
                TagId tid = 0;
                if (!parseInt(req.get_param_value("tag_id", i), tid)) {
                    sendError(res, "Query parameter 'tag_id' must be an integer", 400);
                    return;
                }
                criteria.tag_ids.push_back(tid);
            }
        }
        if (req.has_param("tag_ids")) {
            std::string tidsStr = req.get_param_value("tag_ids");
            std::stringstream ss(tidsStr);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (!item.empty()) {
                    TagId tid = 0;
                    if (!parseInt(item, tid)) {
                        sendError(res, "Query parameter 'tag_ids' must contain comma-separated integers", 400);
                        return;
                    }
                    criteria.tag_ids.push_back(tid);
                }
            }
        }
        if (req.has_param("tag_folder_mode")) {
            std::string mode = req.get_param_value("tag_folder_mode");
            if (mode == "or" || mode == "1" || mode == "true") {
                criteria.tag_folder_or_mode = true;
            }
        } else if (req.has_param("filter_or")) {
            std::string mode = req.get_param_value("filter_or");
            if (mode == "or" || mode == "1" || mode == "true") {
                criteria.tag_folder_or_mode = true;
            }
        }
        if (req.has_param("album_id")) {
            AlbumId aid = 0;
            if (!parseInt(req.get_param_value("album_id"), aid)) {
                sendError(res, "Query parameter 'album_id' must be an integer", 400);
                return;
            }
            criteria.album_id = aid;
        }
        if (req.has_param("date_from")) {
            int64_t df = 0;
            if (!parseInt(req.get_param_value("date_from"), df)) {
                sendError(res, "Query parameter 'date_from' must be an integer", 400);
                return;
            }
            criteria.date_from = df;
        }
        if (req.has_param("date_to")) {
            int64_t dt = 0;
            if (!parseInt(req.get_param_value("date_to"), dt)) {
                sendError(res, "Query parameter 'date_to' must be an integer", 400);
                return;
            }
            criteria.date_to = dt;
        }
        if (criteria.date_from.has_value() && criteria.date_to.has_value() && *criteria.date_from > *criteria.date_to) {
            sendError(res, "'date_from' cannot be greater than 'date_to'", 400);
            return;
        }
        if (req.has_param("limit")) {
            int32_t lim = 0;
            if (!parseInt(req.get_param_value("limit"), lim) || lim < 0 || lim > 1000) {
                sendError(res, "Query parameter 'limit' must be an integer between 0 and 1000", 400);
                return;
            }
            criteria.limit = lim;
        }
        if (req.has_param("offset")) {
            int32_t off = 0;
            if (!parseInt(req.get_param_value("offset"), off) || off < 0) {
                sendError(res, "Query parameter 'offset' must be a non-negative integer", 400);
                return;
            }
            criteria.offset = off;
        }
        if (req.has_param("sort")) {
            std::string sort = req.get_param_value("sort");
            std::string col = sort;
            bool desc = true;
            auto dash = sort.find('-');
            if (dash != std::string::npos) {
                col = sort.substr(0, dash);
                std::string dir = sort.substr(dash + 1);
                if (dir != "asc" && dir != "desc") {
                    sendError(res, "Invalid sort direction: must be 'asc' or 'desc'", 400);
                    return;
                }
                desc = (dir != "asc");
            }
            static const std::unordered_set<std::string> allowedSortCols = {
                "date_taken", "rating", "file_name", "file_size", "duration", "album_order"
            };
            if (allowedSortCols.find(col) == allowedSortCols.end()) {
                sendError(res, "Invalid sort column: " + col, 400);
                return;
            }
            if (col == "album_order" && !criteria.album_id.has_value()) {
                sendError(res, "Sort column 'album_order' requires 'album_id'", 400);
                return;
            }
            criteria.sort_by = col;
            criteria.sort_descending = desc;
        }
        if (req.has_param("has_gps")) {
            std::string val = req.get_param_value("has_gps");
            criteria.has_gps = (val == "1" || val == "true");
        }
        if (req.has_param("min_lat")) {
            double v = 0.0;
            if (!parseDouble(req.get_param_value("min_lat"), v) || v < -90.0 || v > 90.0) {
                sendError(res, "Query parameter 'min_lat' must be a number between -90 and 90", 400);
                return;
            }
            criteria.min_lat = v;
        }
        if (req.has_param("max_lat")) {
            double v = 0.0;
            if (!parseDouble(req.get_param_value("max_lat"), v) || v < -90.0 || v > 90.0) {
                sendError(res, "Query parameter 'max_lat' must be a number between -90 and 90", 400);
                return;
            }
            criteria.max_lat = v;
        }
        if (criteria.min_lat.has_value() && criteria.max_lat.has_value() && *criteria.min_lat > *criteria.max_lat) {
            sendError(res, "'min_lat' cannot be greater than 'max_lat'", 400);
            return;
        }
        if (req.has_param("min_lon")) {
            double v = 0.0;
            if (!parseDouble(req.get_param_value("min_lon"), v) || v < -180.0 || v > 180.0) {
                sendError(res, "Query parameter 'min_lon' must be a number between -180 and 180", 400);
                return;
            }
            criteria.min_lon = v;
        }
        if (req.has_param("max_lon")) {
            double v = 0.0;
            if (!parseDouble(req.get_param_value("max_lon"), v) || v < -180.0 || v > 180.0) {
                sendError(res, "Query parameter 'max_lon' must be a number between -180 and 180", 400);
                return;
            }
            criteria.max_lon = v;
        }
        if (criteria.min_lon.has_value() && criteria.max_lon.has_value() && *criteria.min_lon > *criteria.max_lon) {
            sendError(res, "'min_lon' cannot be greater than 'max_lon'", 400);
            return;
        }
        if (req.has_param("bbox")) {
            std::string bbox = req.get_param_value("bbox");
            if (bbox.empty() || bbox.back() == ',') {
                sendError(res, "Invalid bbox format: expected min_lon,min_lat,max_lon,max_lat", 400);
                return;
            }
            std::stringstream ss(bbox);
            std::string sMinLon, sMinLat, sMaxLon, sMaxLat, extra;
            if (std::getline(ss, sMinLon, ',') && std::getline(ss, sMinLat, ',') &&
                std::getline(ss, sMaxLon, ',') && std::getline(ss, sMaxLat, ',') &&
                !std::getline(ss, extra, ',')) {
                double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
                if (!parseDouble(sMinLon, minLon) || !parseDouble(sMinLat, minLat) ||
                    !parseDouble(sMaxLon, maxLon) || !parseDouble(sMaxLat, maxLat) ||
                    sMinLon.empty() || sMinLat.empty() || sMaxLon.empty() || sMaxLat.empty() ||
                    minLat < -90.0 || minLat > 90.0 || maxLat < -90.0 || maxLat > 90.0 || minLat > maxLat ||
                    minLon < -180.0 || minLon > 180.0 || maxLon < -180.0 || maxLon > 180.0 || minLon > maxLon) {
                    sendError(res, "Invalid bbox format: must be min_lon,min_lat,max_lon,max_lat within bounds", 400);
                    return;
                }
                criteria.min_lon = minLon;
                criteria.min_lat = minLat;
                criteria.max_lon = maxLon;
                criteria.max_lat = maxLat;
            } else {
                sendError(res, "Invalid bbox format: expected min_lon,min_lat,max_lon,max_lat", 400);
                return;
            }
        }

        Result<core::QueryResult> queryRes = catalog_
            ? catalog_->query(criteria)
            : core::QueryBuilder::execute(db(), criteria);

        if (!queryRes.isOk()) {
            sendStatusError(res, queryRes.status());
            return;
        }

        auto& qResult = queryRes.value();
        // Populate tags for items using a single batched query
        if (!qResult.items.empty()) {
            std::vector<MediaId> ids;
            ids.reserve(qResult.items.size());
            for (const auto& item : qResult.items) {
                ids.push_back(item.id);
            }
            auto batchRes = catalog_
                ? catalog_->getTagsForMediaBatch(ids)
                : db().getTagsForMediaBatch(ids);
            if (batchRes.isOk()) {
                auto& tagsMap = batchRes.value();
                for (auto& item : qResult.items) {
                    auto it = tagsMap.find(item.id);
                    if (it != tagsMap.end()) {
                        item.tags = std::move(it->second);
                    }
                }
            }
        }

        nlohmann::json responseJson;
        responseJson["items"] = qResult.items;
        responseJson["total"] = qResult.total_count;

        sendJson(res, responseJson);
    });

    // GET /api/media/:id
    server.Get(R"(/api/media/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        auto mediaRes = catalog_ ? catalog_->getMedia(id) : db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendStatusError(res, mediaRes.status());
            return;
        }

        sendJson(res, mediaRes.value());
    });

    // POST /api/media/:id/rating
    server.Post(R"(/api/media/(\d+)/rating)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("rating") || !body["rating"].is_number_integer()) {
                sendError(res, "Missing or invalid rating field: must be an integer");
                return;
            }
            int32_t rating = body["rating"].get<int32_t>();
            if (rating < 0 || rating > 5) {
                sendError(res, "Rating must be between 0 and 5");
                return;
            }

            Status s = catalog_ ? catalog_->setRating(id, rating) : db().updateRating(id, rating);
            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"id", id}, {"rating", rating}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/:id/flag
    server.Post(R"(/api/media/(\d+)/flag)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("flag") || !body["flag"].is_number_integer()) {
                sendError(res, "Missing or invalid flag field: must be an integer");
                return;
            }
            int flagInt = body["flag"].get<int>();
            if (flagInt < -1 || flagInt > 1) {
                sendError(res, "Flag must be -1, 0, or 1");
                return;
            }

            int8_t flagVal = static_cast<int8_t>(flagInt);
            FlagState flag = static_cast<FlagState>(flagVal);
            Status s = catalog_ ? catalog_->setFlag(id, flag) : db().updateFlag(id, flag);
            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"id", id}, {"flag", flagVal}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/:id/caption
    server.Post(R"(/api/media/(\d+)/caption)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("caption") || !body["caption"].is_string()) {
                sendError(res, "Missing or invalid caption field: must be a string");
                return;
            }
            std::string caption = body["caption"].get<std::string>();
            Status s = catalog_ ? catalog_->setCaption(id, caption) : db().updateCaption(id, caption);
            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"id", id}, {"caption", caption}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/:id/rename
    server.Post(R"(/api/media/(\d+)/rename)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string newName;
            if (body.contains("file_name") && body["file_name"].is_string()) {
                newName = body["file_name"].get<std::string>();
            } else if (body.contains("name") && body["name"].is_string()) {
                newName = body["name"].get<std::string>();
            } else {
                sendError(res, "Missing or invalid 'name' or 'file_name' field: must be a non-empty string");
                return;
            }

            newName = trimWhitespace(newName);

            if (newName.empty() || newName.size() > kMaxNameLength) {
                sendError(res, "File name cannot be empty and must be at most " + std::to_string(kMaxNameLength) + " characters", 400);
                return;
            }
            if (newName.find('/') != std::string::npos ||
                newName.find('\\') != std::string::npos ||
                newName.find(':') != std::string::npos ||
                newName == "." || newName == "..") {
                sendError(res, "Invalid file name: cannot contain path separators or illegal characters", 400);
                return;
            }

            std::string newPath;
            std::string finalName = newName;
            Status s;
            if (catalog_) {
                s = catalog_->renameMedia(id, newName, &newPath, &finalName);
            } else {
                s = Status::internal("Catalog is required to rename media");
            }

            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {
                {"status", "ok"},
                {"id", id},
                {"file_name", finalName},
                {"file_path", newPath}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/:id/move
    server.Post(R"(/api/media/(\d+)/move)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string destPath;
            if (body.contains("destination_path") && body["destination_path"].is_string()) {
                destPath = body["destination_path"].get<std::string>();
            } else if (body.contains("destination") && body["destination"].is_string()) {
                destPath = body["destination"].get<std::string>();
            } else if (body.contains("path") && body["path"].is_string()) {
                destPath = body["path"].get<std::string>();
            } else {
                sendError(res, "Missing or invalid 'destination_path' field: must be a non-empty string", 400);
                return;
            }

            destPath = trimWhitespace(destPath);
            if (destPath.empty()) {
                sendError(res, "Destination path cannot be empty", 400);
                return;
            }

            std::error_code ec;
            auto destFsPath = pathFromUtf8(destPath);
            auto canonicalDest = stripExtendedPrefix(std::filesystem::weakly_canonical(destFsPath, ec));
            if (ec) {
                sendError(res, "Invalid destination path: " + ec.message(), 400);
                return;
            }
            std::string rootErr;
            if (!isWithinAllowedRoots(canonicalDest, rootErr)) {
                sendError(res, rootErr, 403);
                return;
            }

            std::string newPath;
            Status s = moveMedia(id, destPath, &newPath);
            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {
                {"status", "ok"},
                {"id", id},
                {"file_path", newPath}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
        }
    });

    // POST /api/media/:id/date
    server.Post(R"(/api/media/(\d+)/date)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            int64_t dateTaken = 0;
            std::string dateTakenStr;

            if (body.contains("date_taken")) {
                if (body["date_taken"].is_number_integer()) {
                    dateTaken = body["date_taken"].get<int64_t>();
                } else if (body["date_taken"].is_string()) {
                    dateTakenStr = body["date_taken"].get<std::string>();
                    dateTaken = parseDateStringToTimestamp(dateTakenStr);
                }
            }
            if (body.contains("date_taken_str") && body["date_taken_str"].is_string()) {
                std::string customStr = body["date_taken_str"].get<std::string>();
                if (!customStr.empty()) {
                    dateTakenStr = customStr;
                    if (dateTaken == 0) {
                        dateTaken = parseDateStringToTimestamp(dateTakenStr);
                    }
                }
            }

            if (dateTakenStr.empty() && dateTaken != 0) {
                dateTakenStr = formatTimestampToDateString(dateTaken);
            }

            Status s = catalog_
                ? catalog_->setDateTaken(id, dateTaken, dateTakenStr)
                : db().updateDateTaken(id, dateTaken, dateTakenStr);

            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {
                {"status", "ok"},
                {"id", id},
                {"date_taken", dateTaken},
                {"date_taken_str", dateTakenStr}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });


    // POST /api/media/:id/gps
    server.Post(R"(/api/media/(\d+)/gps)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            bool hasGps = true;
            if (body.contains("has_gps") && body["has_gps"].is_boolean()) {
                hasGps = body["has_gps"].get<bool>();
            }

            double latitude = 0.0;
            double longitude = 0.0;
            double altitude = 0.0;

            if (hasGps) {
                if (!body.contains("latitude") || !body["latitude"].is_number() ||
                    !body.contains("longitude") || !body["longitude"].is_number()) {
                    sendError(res, "Missing or invalid latitude or longitude");
                    return;
                }
                latitude = body["latitude"].get<double>();
                longitude = body["longitude"].get<double>();
                if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
                    sendError(res, "Latitude must be between -90 and 90");
                    return;
                }
                if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
                    sendError(res, "Longitude must be between -180 and 180");
                    return;
                }
                if (body.contains("altitude") && body["altitude"].is_number()) {
                    altitude = body["altitude"].get<double>();
                    if (!std::isfinite(altitude)) {
                        sendError(res, "Altitude must be a finite number");
                        return;
                    }
                }
            }

            Status s = catalog_ ? catalog_->setGps(id, hasGps, latitude, longitude, altitude)
                                : db().updateGps(id, hasGps, latitude, longitude, altitude);
            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {
                {"status", "ok"},
                {"id", id},
                {"has_gps", hasGps},
                {"latitude", latitude},
                {"longitude", longitude},
                {"altitude", altitude}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/:id/tags
    server.Post(R"(/api/media/(\d+)/tags)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("name") || !body["name"].is_string()) {
                sendError(res, "Missing or invalid name field");
                return;
            }
            std::string name = body["name"].get<std::string>();
            if (name.empty() || name.size() > kMaxNameLength) {
                sendError(res, "Tag name must not be empty and must be at most " + std::to_string(kMaxNameLength) + " characters", 400);
                return;
            }
            std::string category = body.value("category", "keyword");
            if (category.size() > kMaxCategoryLength) {
                sendError(res, "Category must be at most " + std::to_string(kMaxCategoryLength) + " characters", 400);
                return;
            }

            Status s = catalog_
                ? catalog_->addTag(id, name, category)
                : [&]() {
                    auto tagRes = db().createOrGetTag(name, category);
                    if (!tagRes.isOk()) return tagRes.status();
                    return db().addTagToMedia(id, tagRes.value());
                }();

            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"media_id", id}, {"name", name}, {"category", category}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/media/:id/tags/:tag_id
    server.Delete(R"(/api/media/(\d+)/tags/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        auto tagIdOpt = parseMatchId<TagId>(req, 2, res, "tag ID");
        if (!tagIdOpt) return;
        TagId tagId = *tagIdOpt;

        Status s = catalog_ ? catalog_->removeTag(id, tagId) : db().removeTagFromMedia(id, tagId);
        if (!s.isOk()) {
            sendStatusError(res, s);
            return;
        }

        sendJson(res, {{"status", "ok"}});
    });

    // DELETE /api/media/:id
    server.Delete(R"(/api/media/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;

        bool deleteFromDisk = false;
        if (req.has_param("delete_from_disk")) {
            std::string val = req.get_param_value("delete_from_disk");
            deleteFromDisk = (val == "true" || val == "1");
        }

        Status s;
        if (catalog_) {
            s = catalog_->deleteMedia(id, deleteFromDisk);
        } else {
            if (deleteFromDisk) {
                sendStatusError(res, Status::internal("Catalog is required to delete files from disk"));
                return;
            }
            s = db().deleteMedia(id);
        }

        if (!s.isOk()) {
            sendStatusError(res, s);
            return;
        }

        if (clipService_) {
            clipService_->removeMedia(id);
        }

        sendJson(res, {{"status", "ok"}, {"id", id}, {"deleted_from_disk", deleteFromDisk}});
    });

    // POST /api/media/batch-delete
    server.Post("/api/media/batch-delete", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("ids") || !body["ids"].is_array()) {
                sendError(res, "Missing or invalid 'ids' array");
                return;
            }
            if (body["ids"].size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            bool deleteFromDisk = false;
            if (body.contains("delete_from_disk") && body["delete_from_disk"].is_boolean()) {
                deleteFromDisk = body["delete_from_disk"].get<bool>();
            } else if (req.has_param("delete_from_disk")) {
                std::string val = req.get_param_value("delete_from_disk");
                deleteFromDisk = (val == "true" || val == "1");
            }
            std::vector<MediaId> ids = body["ids"].get<std::vector<MediaId>>();
            int deletedCount = 0;
            std::vector<MediaId> failedIds;
            for (MediaId id : ids) {
                Status s;
                if (catalog_) {
                    s = catalog_->deleteMedia(id, deleteFromDisk);
                } else {
                    if (deleteFromDisk) {
                        s = Status::internal("Catalog is required to delete files from disk");
                    } else {
                        s = db().deleteMedia(id);
                    }
                }
                if (s.isOk()) {
                    if (clipService_) {
                        clipService_->removeMedia(id);
                    }
                    deletedCount++;
                } else {
                    failedIds.push_back(id);
                }
            }
            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : "partial"},
                {"deleted_count", deletedCount},
                {"failed_ids", failedIds},
                {"deleted_from_disk", deleteFromDisk}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/batch-rating
    server.Post("/api/media/batch-rating", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("ids") || !body["ids"].is_array()) {
                sendError(res, "Missing or invalid 'ids' array");
                return;
            }
            if (body["ids"].size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            if (!body.contains("rating") || !body["rating"].is_number_integer()) {
                sendError(res, "Missing or invalid rating field: must be an integer");
                return;
            }
            int32_t rating = body["rating"].get<int32_t>();
            if (rating < 0 || rating > 5) {
                sendError(res, "Rating must be between 0 and 5");
                return;
            }

            std::vector<MediaId> ids = body["ids"].get<std::vector<MediaId>>();
            int updatedCount = 0;
            std::vector<MediaId> failedIds;
            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = catalog_->setRating(id, rating);
                        if (s.isOk()) {
                            updatedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = db().updateRating(id, rating);
                        if (s.isOk()) {
                            updatedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }
            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : "partial"},
                {"updated_count", updatedCount},
                {"rating", rating},
                {"failed_ids", failedIds}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/batch-flag
    server.Post("/api/media/batch-flag", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("ids") || !body["ids"].is_array()) {
                sendError(res, "Missing or invalid 'ids' array");
                return;
            }
            if (body["ids"].size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            if (!body.contains("flag") || !body["flag"].is_number_integer()) {
                sendError(res, "Missing or invalid flag field: must be an integer");
                return;
            }
            int flagInt = body["flag"].get<int>();
            if (flagInt < -1 || flagInt > 1) {
                sendError(res, "Flag must be -1, 0, or 1");
                return;
            }

            FlagState flag = static_cast<FlagState>(flagInt);
            std::vector<MediaId> ids = body["ids"].get<std::vector<MediaId>>();
            int updatedCount = 0;
            std::vector<MediaId> failedIds;
            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = catalog_->setFlag(id, flag);
                        if (s.isOk()) {
                            updatedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = db().updateFlag(id, flag);
                        if (s.isOk()) {
                            updatedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }
            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : "partial"},
                {"updated_count", updatedCount},
                {"flag", flagInt},
                {"failed_ids", failedIds}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/batch-tags
    server.Post("/api/media/batch-tags", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<MediaId> ids;
            const nlohmann::json* idsArray = nullptr;
            if (body.contains("ids") && body["ids"].is_array()) {
                idsArray = &body["ids"];
            } else if (body.contains("media_ids") && body["media_ids"].is_array()) {
                idsArray = &body["media_ids"];
            }
            if (!idsArray) {
                sendError(res, "Missing or invalid 'ids' or 'media_ids' array");
                return;
            }
            if (idsArray->size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            ids = idsArray->get<std::vector<MediaId>>();

            if (!body.contains("name") || !body["name"].is_string()) {
                sendError(res, "Missing or invalid name field");
                return;
            }
            std::string name = body["name"].get<std::string>();
            if (name.empty() || name.size() > kMaxNameLength) {
                sendError(res, "Tag name must not be empty and must be at most " + std::to_string(kMaxNameLength) + " characters", 400);
                return;
            }
            std::string category = body.value("category", "keyword");
            if (category.size() > kMaxCategoryLength) {
                sendError(res, "Category must be at most " + std::to_string(kMaxCategoryLength) + " characters", 400);
                return;
            }

            auto tagRes = catalog_
                ? catalog_->createOrGetTag(name, category)
                : db().createOrGetTag(name, category);

            if (!tagRes.isOk()) {
                sendStatusError(res, tagRes.status());
                return;
            }

            TagId tagId = tagRes.value();
            int taggedCount = 0;
            std::vector<MediaId> failedIds;
            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = catalog_->addTag(id, tagId);
                        if (s.isOk()) {
                            taggedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = db().addTagToMedia(id, tagId);
                        if (s.isOk()) {
                            taggedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }
            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : "partial"},
                {"tag_id", tagId},
                {"name", name},
                {"category", category},
                {"tagged_count", taggedCount},
                {"failed_ids", failedIds}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/media/batch-tags
    server.Delete("/api/media/batch-tags", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.contains("ids") || !body["ids"].is_array() || body["ids"].empty()) {
                sendError(res, "Missing or invalid non-empty 'ids' array", 400);
                return;
            }
            if (body["ids"].size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            if (!body.contains("tag_id") || !body["tag_id"].is_number_integer()) {
                sendError(res, "Missing or invalid tag_id field", 400);
                return;
            }

            const TagId tagId = body["tag_id"].get<TagId>();
            if (tagId <= 0) {
                sendError(res, "tag_id must be a positive integer", 400);
                return;
            }

            std::vector<MediaId> ids;
            ids.reserve(body["ids"].size());
            std::unordered_set<MediaId> seenIds;
            for (const auto& value : body["ids"]) {
                if (!value.is_number_integer()) {
                    sendError(res, "ids must contain only integer media IDs", 400);
                    return;
                }
                const MediaId id = value.get<MediaId>();
                if (seenIds.insert(id).second) {
                    ids.push_back(id);
                }
            }

            auto tagsRes = catalog_ ? catalog_->getTags() : db().getAllTags();
            if (!tagsRes.isOk()) {
                sendStatusError(res, tagsRes.status());
                return;
            }
            const auto tagIt = std::find_if(tagsRes.value().begin(), tagsRes.value().end(),
                [tagId](const Tag& tag) { return tag.id == tagId; });
            if (tagIt == tagsRes.value().end()) {
                sendStatusError(res, Status::notFound("Tag not found: " + std::to_string(tagId)));
                return;
            }

            auto batchRes = catalog_
                ? catalog_->getTagsForMediaBatch(ids)
                : db().getTagsForMediaBatch(ids);
            if (!batchRes.isOk()) {
                sendStatusError(res, batchRes.status());
                return;
            }
            const auto& tagsMap = batchRes.value();

            int removedCount = 0;
            int unchangedCount = 0;
            std::vector<MediaId> failedIds;
            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (const MediaId id : ids) {
                        auto it = tagsMap.find(id);
                        if (it == tagsMap.end()) {
                            auto mediaRes = catalog_->getMedia(id);
                            if (!mediaRes.isOk()) {
                                failedIds.push_back(id);
                                continue;
                            }
                            unchangedCount++;
                            continue;
                        }

                        const auto& mediaTags = it->second;
                        const bool hasTag = std::any_of(mediaTags.begin(), mediaTags.end(),
                            [tagId](const Tag& tag) { return tag.id == tagId; });
                        if (!hasTag) {
                            unchangedCount++;
                            continue;
                        }

                        Status status = catalog_->removeTag(id, tagId);
                        if (status.isOk()) {
                            removedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (const MediaId id : ids) {
                        auto it = tagsMap.find(id);
                        if (it == tagsMap.end()) {
                            auto mediaRes = db().getMediaById(id);
                            if (!mediaRes.isOk()) {
                                failedIds.push_back(id);
                                continue;
                            }
                            unchangedCount++;
                            continue;
                        }

                        const auto& mediaTags = it->second;
                        const bool hasTag = std::any_of(mediaTags.begin(), mediaTags.end(),
                            [tagId](const Tag& tag) { return tag.id == tagId; });
                        if (!hasTag) {
                            unchangedCount++;
                            continue;
                        }

                        Status status = db().removeTagFromMedia(id, tagId);
                        if (status.isOk()) {
                            removedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }

            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : (removedCount > 0 ? "partial" : "error")},
                {"tag_id", tagId},
                {"removed_count", removedCount},
                {"unchanged_count", unchangedCount},
                {"failed_ids", failedIds}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
        }
    });

    // POST /api/media/batch-gps
    server.Post("/api/media/batch-gps", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<MediaId> ids;
            if (body.contains("ids") && body["ids"].is_array()) {
                if (body["ids"].size() > kMaxBatchSize) {
                    sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                    return;
                }
                ids = body["ids"].get<std::vector<MediaId>>();
            } else {
                sendError(res, "Missing or invalid 'ids' array");
                return;
            }

            bool hasGps = body.value("has_gps", true);
            double latitude = body.value("latitude", 0.0);
            double longitude = body.value("longitude", 0.0);
            double altitude = body.value("altitude", 0.0);

            if (hasGps) {
                if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
                    sendError(res, "Latitude must be between -90 and 90");
                    return;
                }
                if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
                    sendError(res, "Longitude must be between -180 and 180");
                    return;
                }
                if (!std::isfinite(altitude)) {
                    sendError(res, "Altitude must be a finite number");
                    return;
                }
            }

            int updatedCount = 0;
            std::vector<MediaId> failedIds;
            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = catalog_->setGps(id, hasGps, latitude, longitude, altitude);
                        if (s.isOk()) {
                            updatedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (MediaId id : ids) {
                        Status s = db().updateGps(id, hasGps, latitude, longitude, altitude);
                        if (s.isOk()) {
                            updatedCount++;
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }
            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : "partial"},
                {"updated_count", updatedCount},
                {"has_gps", hasGps},
                {"latitude", latitude},
                {"longitude", longitude},
                {"failed_ids", failedIds}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/batch-move
    server.Post("/api/media/batch-move", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("ids") || !body["ids"].is_array()) {
                sendError(res, "Missing or invalid 'ids' array", 400);
                return;
            }
            if (body["ids"].size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            std::string destPath;
            if (body.contains("destination_path") && body["destination_path"].is_string()) {
                destPath = body["destination_path"].get<std::string>();
            } else if (body.contains("destination") && body["destination"].is_string()) {
                destPath = body["destination"].get<std::string>();
            } else if (body.contains("path") && body["path"].is_string()) {
                destPath = body["path"].get<std::string>();
            } else {
                sendError(res, "Missing or invalid 'destination_path' field: must be a non-empty string", 400);
                return;
            }

            destPath = trimWhitespace(destPath);
            if (destPath.empty()) {
                sendError(res, "Destination path cannot be empty", 400);
                return;
            }

            std::error_code ec;
            auto destFsPath = pathFromUtf8(destPath);
            auto canonicalDest = stripExtendedPrefix(std::filesystem::weakly_canonical(destFsPath, ec));
            if (ec) {
                sendError(res, "Invalid destination path: " + ec.message(), 400);
                return;
            }
            std::string rootErr;
            if (!isWithinAllowedRoots(canonicalDest, rootErr)) {
                sendError(res, rootErr, 403);
                return;
            }

            std::vector<MediaId> ids = body["ids"].get<std::vector<MediaId>>();

            int movedCount = 0;
            std::vector<MediaId> failedIds;
            std::vector<std::string> errors;
            nlohmann::json items = nlohmann::json::array();

            for (MediaId id : ids) {
                std::string newPath;
                Status s = moveMedia(id, destPath, &newPath);
                if (s.isOk()) {
                    movedCount++;
                    items.push_back({{"id", id}, {"file_path", newPath}});
                } else {
                    IMAGINE_LOG_WARN("batch-move failed for id " + std::to_string(id) + ": " + s.message());
                    failedIds.push_back(id);
                    errors.push_back(s.message());
                }
            }

            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : (movedCount > 0 ? "partial" : "error")},
                {"moved_count", movedCount},
                {"failed_ids", failedIds},
                {"errors", errors},
                {"destination_path", destPath},
                {"items", items}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
        }
    });

    // POST /api/media/batch-date
    server.Post("/api/media/batch-date", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<std::pair<MediaId, int64_t>> updates;
            std::unordered_map<MediaId, std::string> customStrs;

            if (body.contains("items") && body["items"].is_array()) {
                auto itemsJson = body["items"];
                if (itemsJson.size() > kMaxBatchSize) {
                    sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                    return;
                }
                for (const auto& itm : itemsJson) {
                    if (!itm.contains("id")) continue;
                    MediaId id = itm["id"].get<MediaId>();
                    int64_t ts = 0;
                    std::string strVal;
                    if (itm.contains("date_taken")) {
                        if (itm["date_taken"].is_number_integer()) {
                            ts = itm["date_taken"].get<int64_t>();
                        } else if (itm["date_taken"].is_string()) {
                            strVal = itm["date_taken"].get<std::string>();
                            ts = parseDateStringToTimestamp(strVal);
                        }
                    }
                    if (itm.contains("date_taken_str") && itm["date_taken_str"].is_string()) {
                        std::string custom = itm["date_taken_str"].get<std::string>();
                        if (!custom.empty()) {
                            strVal = custom;
                            if (ts == 0) ts = parseDateStringToTimestamp(strVal);
                        }
                    }
                    updates.emplace_back(id, ts);
                    if (!strVal.empty()) {
                        customStrs[id] = strVal;
                    }
                }
            } else if (body.contains("ids") && body["ids"].is_array()) {
                if (body["ids"].size() > kMaxBatchSize) {
                    sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                    return;
                }
                std::vector<MediaId> ids = body["ids"].get<std::vector<MediaId>>();

                if (body.contains("shift_seconds")) {
                    int64_t shiftSec = body["shift_seconds"].get<int64_t>();
                    for (MediaId id : ids) {
                        auto mRes = catalog_ ? catalog_->getMedia(id) : db().getMediaById(id);
                        if (mRes.isOk()) {
                            int64_t origTs = mRes.value().date_taken;
                            int64_t newTs = (origTs != 0) ? (origTs + shiftSec) : 0;
                            updates.emplace_back(id, newTs);
                        }
                    }
                } else if (body.contains("date_taken")) {
                    int64_t ts = 0;
                    std::string strVal;
                    if (body["date_taken"].is_number_integer()) {
                        ts = body["date_taken"].get<int64_t>();
                    } else if (body["date_taken"].is_string()) {
                        strVal = body["date_taken"].get<std::string>();
                        ts = parseDateStringToTimestamp(strVal);
                    }
                    if (body.contains("date_taken_str") && body["date_taken_str"].is_string()) {
                        std::string custom = body["date_taken_str"].get<std::string>();
                        if (!custom.empty()) {
                            strVal = custom;
                            if (ts == 0) ts = parseDateStringToTimestamp(strVal);
                        }
                    }
                    for (MediaId id : ids) {
                        updates.emplace_back(id, ts);
                        if (!strVal.empty()) customStrs[id] = strVal;
                    }
                } else {
                    sendError(res, "Missing date_taken, shift_seconds, or items array");
                    return;
                }
            } else {
                sendError(res, "Missing or invalid 'ids' or 'items' array");
                return;
            }

            int updatedCount = 0;
            std::vector<MediaId> failedIds;
            nlohmann::json updatedItems = nlohmann::json::array();
            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (const auto& [id, dateTaken] : updates) {
                        std::string dateTakenStr = customStrs.count(id) ? customStrs[id] : "";
                        if (dateTakenStr.empty() && dateTaken != 0) {
                            dateTakenStr = formatTimestampToDateString(dateTaken);
                        }

                        Status s = catalog_->setDateTaken(id, dateTaken, dateTakenStr);
                        if (s.isOk()) {
                            updatedCount++;
                            updatedItems.push_back({
                                {"id", id},
                                {"date_taken", dateTaken},
                                {"date_taken_str", dateTakenStr}
                            });
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (const auto& [id, dateTaken] : updates) {
                        std::string dateTakenStr = customStrs.count(id) ? customStrs[id] : "";
                        if (dateTakenStr.empty() && dateTaken != 0) {
                            dateTakenStr = formatTimestampToDateString(dateTaken);
                        }

                        Status s = db().updateDateTaken(id, dateTaken, dateTakenStr);
                        if (s.isOk()) {
                            updatedCount++;
                            updatedItems.push_back({
                                {"id", id},
                                {"date_taken", dateTaken},
                                {"date_taken_str", dateTakenStr}
                            });
                        } else {
                            failedIds.push_back(id);
                        }
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }

            sendJson(res, {
                {"status", failedIds.empty() ? "ok" : "partial"},
                {"updated_count", updatedCount},
                {"items", updatedItems},
                {"failed_ids", failedIds}
            });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/event-suggestions
    // Build a read-only, deterministic preview from the selected catalog items.
    server.Post("/api/media/event-suggestions", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.contains("ids") || !body["ids"].is_array()) {
                sendError(res, "Missing or invalid 'ids' array", 400);
                return;
            }
            if (body["ids"].empty()) {
                sendError(res, "At least one media ID is required", 400);
                return;
            }
            if (body["ids"].size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }

            std::vector<MediaId> ids;
            ids.reserve(body["ids"].size());
            std::unordered_set<MediaId> seen;
            for (const auto& value : body["ids"]) {
                if (!value.is_number_integer()) {
                    sendError(res, "Each media ID must be a positive integer", 400);
                    return;
                }
                const MediaId id = value.get<MediaId>();
                if (id <= 0) {
                    sendError(res, "Each media ID must be a positive integer", 400);
                    return;
                }
                if (seen.insert(id).second) ids.push_back(id);
            }
            if (ids.empty()) {
                sendError(res, "At least one media ID is required", 400);
                return;
            }

            std::vector<MediaItem> media;
            std::vector<MediaId> missingIds;
            std::vector<MediaId> foundIds;
            media.reserve(ids.size());
            foundIds.reserve(ids.size());
            for (const MediaId id : ids) {
                auto itemRes = catalog_ ? catalog_->getMedia(id) : db().getMediaById(id);
                if (!itemRes.isOk()) {
                    missingIds.push_back(id);
                    continue;
                }
                foundIds.push_back(id);
                media.push_back(std::move(itemRes.value()));
            }
            if (!foundIds.empty()) {
                auto batchTagsRes = catalog_
                    ? catalog_->getTagsForMediaBatch(foundIds)
                    : db().getTagsForMediaBatch(foundIds);
                if (batchTagsRes.isOk()) {
                    auto& tagsMap = batchTagsRes.value();
                    for (auto& item : media) {
                        auto it = tagsMap.find(item.id);
                        if (it != tagsMap.end()) {
                            item.tags = std::move(it->second);
                        }
                    }
                }
            }

            auto suggestions = core::EventSuggestionEngine::suggest(media);
            nlohmann::json groups = nlohmann::json::array();
            for (const auto& group : suggestions.groups) {
                nlohmann::json photoJson = nlohmann::json::array();
                for (const auto& photo : group.photos) {
                    photoJson.push_back({
                        {"id", photo.id},
                        {"file_name", photo.file_name},
                        {"thumb_small", thumbnail::Cache::getRelativeThumbnailPath(photo.content_hash, thumbnail::Cache::SmallSize)},
                        {"thumb_large", thumbnail::Cache::getRelativeThumbnailPath(photo.content_hash, thumbnail::Cache::LargeSize)},
                        {"content_hash", photo.content_hash},
                        {"date_taken", photo.date_taken}
                    });
                }
                groups.push_back({
                    {"media_ids", [&]() {
                        nlohmann::json result = nlohmann::json::array();
                        for (const auto& photo : group.photos) result.push_back(photo.id);
                        return result;
                    }()},
                    {"start_date", group.start_date},
                    {"end_date", group.end_date},
                    {"name_source", core::eventSuggestionNameSourceString(group.name_source)},
                    {"name_value", group.name_value},
                    {"existing_event_tag_id", group.existing_event_tag_id.has_value() ? nlohmann::json(*group.existing_event_tag_id) : nlohmann::json(nullptr)},
                    {"people", group.people},
                    {"places", group.places},
                    {"keywords", group.keywords},
                    {"photos", photoJson}
                });
            }

            nlohmann::json skipped = nlohmann::json::array();
            for (const auto& id : missingIds) {
                skipped.push_back({{"id", id}, {"reason", "missing"}});
            }
            for (const auto& item : suggestions.skipped) {
                skipped.push_back({{"id", item.id}, {"reason", item.reason}});
            }
            sendJson(res, {{"groups", groups}, {"skipped", skipped}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
        }
    });
}

void ApiRouter::registerThumbnailRoutes(httplib::Server& server) {
    // GET /api/thumbnails/:hash/:size
    server.Get(R"(/api/thumbnails/([a-zA-Z0-9_-]+)/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string hash = req.matches[1];
        auto sizeOpt = parseMatchId<int>(req, 2, res, "thumbnail size");
        if (!sizeOpt) return;
        int size = *sizeOpt;

        if (size <= 0 || size > 2048) {
            sendError(res, "Thumbnail size must be between 1 and 2048", 400);
            return;
        }

        std::string thumbPath = cache().getThumbnailPath(hash, size);
        std::error_code ec;
        // Match Cache::hasThumbnail: zero-byte files count as missing.
        bool thumbExists = std::filesystem::is_regular_file(pathFromUtf8(thumbPath), ec) && !ec &&
                           std::filesystem::file_size(pathFromUtf8(thumbPath), ec) > 0 && !ec;
        ec.clear();

        // A procedural video placeholder is retried once FFmpeg is usable again.
        // The marker age acts as a back-off so a failing extraction is not retried
        // on every request. The marker is the cheap hot-path check; the expensive
        // pixel-signature fallback runs only for video items with no marker
        // (legacy placeholders), so photo/audio hits never pay for a JPEG decode.
        constexpr int kPlaceholderRetrySeconds = 300;
        bool isPlaceholder = false;
        bool placeholderServed = false;
        if (thumbExists && metadata::FfmpegHelper::isAvailable() &&
            cache().hasPlaceholderMarker(hash)) {
            isPlaceholder = cache().shouldRetryPlaceholder(hash, kPlaceholderRetrySeconds);
            placeholderServed = !isPlaceholder;
        }

        if (isPlaceholder) {
            cache().clearThumbnail(hash);
            thumbExists = false;
        } else if (thumbExists && metadata::FfmpegHelper::isAvailable() &&
                   !cache().hasPlaceholderMarker(hash)) {
            // Legacy placeholder without a marker: resolve the media type first so
            // only video items pay for the signature decode. A match is cleared so
            // the miss path below regenerates (and marks) it now.
            if (auto legacyMedia = db().getMediaByHash(hash);
                legacyMedia.isOk() && legacyMedia.value().media_type == "video" &&
                thumbnail::Generator::isProceduralThumbnail(thumbPath, "video")) {
                cache().clearThumbnail(hash);
                thumbExists = false;
            }
        }

        if (!thumbExists) {
            // Attempt on-demand generation
            auto mediaRes = db().getMediaByHash(hash);
            if (mediaRes.isOk()) {
                const auto& item = mediaRes.value();
                if (item.media_type == "video") {
                    static std::atomic<uint64_t> tmpCounter{0};
                    const std::string videoPath = resolvePhotoPath(item.file_path);
                    bool generated = false;

                    // Prefer deriving the requested size from an already cached frame
                    // instead of paying for another FFmpeg extraction.
                    bool largeIsPlaceholder = cache().hasPlaceholderMarker(hash) ||
                        thumbnail::Generator::isProceduralThumbnail(
                            cache().getThumbnailPath(hash, thumbnail::Cache::LargeSize), "video");
                    if (!largeIsPlaceholder && cache().hasThumbnail(hash, thumbnail::Cache::LargeSize)) {
                        auto deriveRes = cache().ensureThumbnail(
                            cache().getThumbnailPath(hash, thumbnail::Cache::LargeSize), hash, size);
                        if (deriveRes.isOk()) {
                            thumbPath = deriveRes.value();
                            generated = true;
                        }
                    }

                    if (!generated && !cache().cacheDir().empty() && metadata::FfmpegHelper::isAvailable()) {
                        auto cacheDirFs = pathFromUtf8(cache().cacheDir());
                        std::string tmpFileName = "tmp_ondemand_" + hash + "_" +
                                                  std::to_string(++tmpCounter) + ".jpg";
                        std::string tmpThumb = pathToUtf8(cacheDirFs / pathFromUtf8(tmpFileName));
                        double captureTime = std::min(1.0, item.duration > 0 ? item.duration / 2.0 : 0.0);
                        auto ffRes = metadata::FfmpegHelper::extractVideoFrame(videoPath, captureTime, tmpThumb);
                        if (ffRes.isOk()) {
                            auto dualRes = cache().ensureDualThumbnails(tmpThumb, hash);
                            if (dualRes.isOk()) {
                                const std::string& largePath = dualRes.value().second;
                                const std::string& sizeSource =
                                    size > thumbnail::Cache::LargeSize ? tmpThumb : largePath;
                                auto genRes = cache().ensureThumbnail(sizeSource, hash, size);
                                if (genRes.isOk()) {
                                    thumbPath = genRes.value();
                                    thumbExists = std::filesystem::is_regular_file(pathFromUtf8(thumbPath), ec) && !ec;
                                    generated = thumbExists;
                                }
                                if (generated) {
                                    auto updateRes = db().updateThumbnails(
                                        item.id,
                                        thumbnail::Cache::getRelativeThumbnailPath(hash, thumbnail::Cache::SmallSize),
                                        thumbnail::Cache::getRelativeThumbnailPath(hash, thumbnail::Cache::LargeSize));
                                    if (!updateRes.isOk()) {
                                        IMAGINE_LOG_WARN("Failed to persist video thumbnails for " + hash + ": " +
                                                         updateRes.message());
                                    }
                                    cache().clearPlaceholderMarker(hash);
                                }
                            } else {
                                IMAGINE_LOG_WARN("Failed to generate video thumbnails for " + hash + ": " +
                                                 dualRes.status().message());
                            }
                        } else {
                            IMAGINE_LOG_WARN("Video frame extraction failed for " + hash + " (" + videoPath + "): " +
                                             ffRes.status().message());
                        }
                        ec.clear();
                        std::filesystem::remove(pathFromUtf8(tmpThumb), ec);
                    }

                    if (!generated) {
                        auto procRes = cache().ensureProceduralThumbnails(hash, item.media_type);
                        if (procRes.isOk()) {
                            auto sizeRes = cache().ensureThumbnail(procRes.value().second, hash, size);
                            thumbPath = sizeRes.isOk() ? sizeRes.value()
                                                       : cache().getThumbnailPath(hash, size);
                            auto markerRes = cache().setPlaceholderMarker(hash);
                            if (!markerRes.isOk()) {
                                IMAGINE_LOG_WARN("Failed to mark video thumbnail as placeholder for " + hash + ": " +
                                                 markerRes.message());
                            }
                        }
                    }
                } else if (item.media_type == "audio") {
                    std::string label;
                    if (!item.audio_artist.empty()) {
                        label = item.audio_artist + (!item.audio_title.empty() ? " - " + item.audio_title : "");
                    }
                    auto procRes = cache().ensureProceduralThumbnails(hash, item.media_type, label);
                    if (procRes.isOk()) {
                        auto sizeRes = cache().ensureThumbnail(procRes.value().second, hash, size);
                        thumbPath = sizeRes.isOk() ? sizeRes.value()
                                                   : cache().getThumbnailPath(hash, size);
                    }
                } else {
                    std::string photoPath = resolvePhotoPath(item.file_path);
                    auto genRes = cache().ensureThumbnail(photoPath, hash, size, item.exif.orientation);
                    if (genRes.isOk()) {
                        thumbPath = genRes.value();
                    }
                }
            }
        }

        const auto thumbFsPath = pathFromUtf8(thumbPath);
        ec.clear();
        if (std::filesystem::is_regular_file(thumbFsPath, ec) && !ec) {
            auto fsize = std::filesystem::file_size(thumbFsPath, ec);
            if (ec) { fsize = 0; ec.clear(); }
            auto mtime = std::filesystem::last_write_time(thumbFsPath, ec);
            int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
            std::string etag = "\"thumb-v2-" + hash + "_" + std::to_string(size) + "-" + std::to_string(fsize) + "-" + std::to_string(mtimeSec) + "\"";

            // No signature decode here: placeholder state is already tracked above and
            // the fallback path (re)sets the marker when it serves a placeholder.
            placeholderServed = placeholderServed || cache().hasPlaceholderMarker(hash);
            if (placeholderServed) {
                res.set_header("Cache-Control", "no-cache, must-revalidate");
            } else {
                res.set_header("Cache-Control", "public, max-age=31536000, immutable");
            }
            res.set_header("ETag", etag);

            if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag) {
                res.status = 304;
                return;
            }

            // Read via the wide path (thumbFsPath) to guarantee correct file reading on Windows
            // even if cacheDir or file path contains non-ASCII characters.
            std::ifstream ifs(thumbFsPath, std::ios::binary);
            if (ifs) {
                std::string body((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                res.set_content(std::move(body), "image/jpeg");
            } else {
                res.set_file_content(thumbPath, "image/jpeg");
            }
        } else {
            sendError(res, "Thumbnail not found", 404);
        }
    });

    // GET /api/photos/:id/original (and /api/media/:id/file, /api/media/:id/original)
    auto handleServeFile = [this](const httplib::Request& req, httplib::Response& res) {
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        auto mediaRes = db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendError(res, "Media not found", 404);
            return;
        }

        const auto& item = mediaRes.value();
        std::string photoPath = resolvePhotoPath(item.file_path);
        std::error_code ec;
        if (!std::filesystem::exists(photoPath, ec) || !std::filesystem::is_regular_file(photoPath, ec)) {
            IMAGINE_LOG_WARN("File not found on disk: " + photoPath);
            sendError(res, "File not found on disk", 404);
            return;
        }

        auto fsize = std::filesystem::file_size(photoPath, ec);
        auto mtime = std::filesystem::last_write_time(photoPath, ec);
        int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
        std::string etag = "\"" + std::to_string(id) + "-" + std::to_string(fsize) + "-" + std::to_string(mtimeSec) + "\"";

        res.set_header("Accept-Ranges", "bytes");
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");

        if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag) {
            res.status = 304;
            return;
        }

        std::string mime = getMimeType(photoPath);

        if (req.has_header("Range") && !req.ranges.empty()) {
            bool satisfiable = true;
            for (const auto& r : req.ranges) {
                if (r.first >= static_cast<ssize_t>(fsize)) {
                    satisfiable = false;
                    break;
                }
            }
            if (!satisfiable) {
                res.status = 416;
                res.set_header("Content-Range", "bytes */" + std::to_string(fsize));
                sendError(res, "Range Not Satisfiable", 416);
                return;
            }
        }

        res.set_file_content(photoPath, mime);
    };

    server.Get(R"(/api/photos/(\d+)/original)", handleServeFile);
    server.Get(R"(/api/media/(\d+)/file)", handleServeFile);
    server.Get(R"(/api/media/(\d+)/original)", handleServeFile);

    // POST /api/media/:id/thumbnail
    server.Post(R"(/api/media/(\d+)/thumbnail)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;

        auto mediaRes = db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendError(res, "Media not found", 404);
            return;
        }

        const auto& item = mediaRes.value();
        if (req.body.empty()) {
            sendError(res, "Request body cannot be empty", 400);
            return;
        }

        const uint8_t* data = reinterpret_cast<const uint8_t*>(req.body.data());
        size_t size = req.body.size();

        std::vector<uint8_t> decoded;
        if (req.body.rfind("data:image/", 0) == 0) {
            auto comma = req.body.find(',');
            if (comma != std::string::npos) {
                std::string b64 = req.body.substr(comma + 1);
                decoded = base64Decode(b64);
                data = decoded.data();
                size = decoded.size();
            }
        }

        if (size == 0) {
            sendError(res, "Invalid thumbnail data", 400);
            return;
        }

        std::error_code ec;
        std::filesystem::remove(cache().getThumbnailPath(item.content_hash, thumbnail::Cache::SmallSize), ec);
        std::filesystem::remove(cache().getThumbnailPath(item.content_hash, thumbnail::Cache::LargeSize), ec);

        int outW = 0, outH = 0;
        auto thumbRes = cache().ensureDualThumbnailsFromMemory(
            data, size, item.content_hash, 1, &outW, &outH
        );
        if (!thumbRes.isOk()) {
            sendError(res, "Failed to create thumbnail: " + thumbRes.status().message(), 500);
            return;
        }

        sendJson(res, {
            {"status", "ok"},
            {"thumb_small", thumbnail::Cache::getRelativeThumbnailPath(item.content_hash, thumbnail::Cache::SmallSize)},
            {"thumb_large", thumbnail::Cache::getRelativeThumbnailPath(item.content_hash, thumbnail::Cache::LargeSize)}
        });
    });

    // POST /api/photos/:id/edit and POST /api/media/:id/edit
    auto handleEditPhoto = [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto idOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!idOpt) return;
        MediaId id = *idOpt;
        auto mediaRes = db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendError(res, "Media not found", 404);
            return;
        }

        auto item = mediaRes.value();
        if (item.media_type != "photo") {
            sendError(res, "Quick edit is not supported for " + item.media_type + " files", 400);
            return;
        }

        std::string photoPath = resolvePhotoPath(item.file_path);
        std::error_code ec;
        if (!std::filesystem::exists(photoPath, ec) || !std::filesystem::is_regular_file(photoPath, ec)) {
            IMAGINE_LOG_WARN("Source photo not found on disk: " + photoPath);
            sendError(res, "Source photo not found on disk", 404);
            return;
        }

        std::string mode = "overwrite";
        std::vector<uint8_t> newImageBytes;

        bool isJson = false;
        std::string contentType = req.get_header_value("Content-Type");
        if (contentType.find("application/json") != std::string::npos || (!req.body.empty() && req.body.front() == '{')) {
            isJson = true;
        }

        if (isJson) {
            try {
                auto body = nlohmann::json::parse(req.body);
                if (body.contains("mode") && body["mode"].is_string()) {
                    mode = body["mode"].get<std::string>();
                } else if (req.has_param("mode")) {
                    mode = req.get_param_value("mode");
                } else if (req.has_header("X-Edit-Mode")) {
                    mode = req.get_header_value("X-Edit-Mode");
                }
                if (body.contains("image_data") && body["image_data"].is_string()) {
                    newImageBytes = base64Decode(body["image_data"].get<std::string>());
                } else if (body.contains("operations") && body["operations"].is_object()) {
                    auto loadRes = thumbnail::Generator::loadImage(photoPath);
                    if (!loadRes.isOk()) {
                        sendStatusError(res, loadRes.status());
                        return;
                    }
                    auto img = loadRes.value();
                    if (item.exif.orientation > 1 && item.exif.orientation <= 8) {
                        img = thumbnail::Generator::rotate(img, item.exif.orientation);
                    }
                    const auto& ops = body["operations"];
                    if (ops.contains("crop") && ops["crop"].is_object()) {
                        int x = ops["crop"].value("x", 0);
                        int y = ops["crop"].value("y", 0);
                        int w = ops["crop"].value("width", img.width);
                        int h = ops["crop"].value("height", img.height);
                        auto cropRes = thumbnail::Generator::crop(img, x, y, w, h);
                        if (!cropRes.isOk()) {
                            sendStatusError(res, cropRes.status());
                            return;
                        }
                        img = cropRes.value();
                    }
                    if (ops.contains("rotation") && ops["rotation"].is_number_integer()) {
                        int deg = ops["rotation"].get<int>();
                        img = thumbnail::Generator::rotateAngle(img, deg);
                    }
                    static std::atomic<uint64_t> s_tmpCounter{0};
                    std::string tmpOut = photoPath + "." + std::to_string(++s_tmpCounter) + ".proc.tmp";
                    auto saveStatus = thumbnail::Generator::saveJpeg(img, tmpOut, 95);
                    if (!saveStatus.isOk()) {
                        sendStatusError(res, saveStatus);
                        return;
                    }
                    std::ifstream ifs(tmpOut, std::ios::binary);
                    newImageBytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
                    ifs.close();
                    std::filesystem::remove(tmpOut, ec);
                } else {
                    sendError(res, "Missing image_data or operations in edit request");
                    return;
                }
            } catch (const std::exception& ex) {
                sendError(res, std::string("Invalid JSON: ") + ex.what());
                return;
            }
        } else {
            if (req.has_param("mode")) {
                mode = req.get_param_value("mode");
            } else if (req.has_header("X-Edit-Mode")) {
                mode = req.get_header_value("X-Edit-Mode");
            }
            newImageBytes.assign(reinterpret_cast<const uint8_t*>(req.body.data()),
                                 reinterpret_cast<const uint8_t*>(req.body.data()) + req.body.size());
        }

        if (newImageBytes.empty()) {
            sendError(res, "No image data provided");
            return;
        }

        auto dimRes = thumbnail::Generator::getImageDimensionsFromMemory(newImageBytes.data(), newImageBytes.size());
        if (!dimRes.isOk()) {
            sendError(res, "Invalid image data: unable to parse dimensions");
            return;
        }
        int newW = dimRes.value().first;
        int newH = dimRes.value().second;

        if (mode == "copy") {
            std::filesystem::path origPath(photoPath);
            std::string stem = origPath.stem().string();
            std::string ext = origPath.extension().string();
            auto parent = origPath.parent_path();

            std::filesystem::path destPath;
            int counter = 1;
            while (true) {
                std::string suffix = (counter == 1) ? "_edited" : ("_edited_" + std::to_string(counter));
                destPath = parent / (stem + suffix + ext);
                if (!std::filesystem::exists(destPath, ec)) {
                    break;
                }
                counter++;
            }

            {
                std::ofstream ofs(destPath, std::ios::binary);
                if (!ofs) {
                    sendError(res, "Failed to write new file: " + destPath.string(), 500);
                    return;
                }
                ofs.write(reinterpret_cast<const char*>(newImageBytes.data()), newImageBytes.size());
                ofs.flush();
                if (!ofs) {
                    ofs.close();
                    std::filesystem::remove(destPath, ec);
                    sendError(res, "Failed to write new file: " + destPath.string(), 500);
                    return;
                }
                ofs.close();
                if (ofs.fail()) {
                    std::filesystem::remove(destPath, ec);
                    sendError(res, "Failed to close new file: " + destPath.string(), 500);
                    return;
                }
            }

            if (catalog_) {
                auto importRes = catalog_->importFile(destPath.string());
                if (!importRes.isOk()) {
                    sendStatusError(res, importRes.status());
                    return;
                }
                auto newItem = importRes.value();
                if (item.rating > 0) catalog_->setRating(newItem.id, item.rating);
                if (item.flag != FlagState::Unflagged) catalog_->setFlag(newItem.id, item.flag);
                auto tagsRes = catalog_->getTagsForMedia(item.id);
                if (tagsRes.isOk()) {
                    for (const auto& tag : tagsRes.value()) {
                        catalog_->addTag(newItem.id, tag.id);
                    }
                }
                auto reloaded = catalog_->getMedia(newItem.id);
                sendJson(res, reloaded.isOk() ? reloaded.value() : newItem, 201);
            } else {
                sendJson(res, {{"status", "created"}, {"path", destPath.string()}}, 201);
            }
        } else {
            std::error_code dirEc;
            if (std::filesystem::is_directory(photoPath + ".edit.tmp", dirEc)) {
                sendError(res, "Failed to create temporary file for saving", 500);
                return;
            }
            static std::atomic<uint64_t> s_editTmpCounter{0};
            std::string tmpPath = photoPath + "." + std::to_string(++s_editTmpCounter) + ".edit.tmp";
            {
                std::ofstream ofs(tmpPath, std::ios::binary);
                if (!ofs) {
                    sendError(res, "Failed to create temporary file for saving", 500);
                    return;
                }
                ofs.write(reinterpret_cast<const char*>(newImageBytes.data()), newImageBytes.size());
                ofs.flush();
                if (!ofs) {
                    ofs.close();
                    std::filesystem::remove(tmpPath, ec);
                    sendError(res, "Failed to write temporary file for saving", 500);
                    return;
                }
                ofs.close();
                if (ofs.fail()) {
                    std::filesystem::remove(tmpPath, ec);
                    sendError(res, "Failed to close temporary file for saving", 500);
                    return;
                }
            }

            std::filesystem::rename(tmpPath, photoPath, ec);
            if (ec) {
                std::filesystem::remove(tmpPath, ec);
                sendError(res, "Failed to overwrite original file: " + ec.message(), 500);
                return;
            }

            auto hashRes = metadata::Hasher::computeFileSha256(photoPath);
            if (!hashRes.isOk()) {
                sendStatusError(res, hashRes.status());
                return;
            }
            std::string oldHash = item.content_hash;
            std::string newHash = hashRes.value();
            auto fsize = std::filesystem::file_size(photoPath, ec);
            auto mtime = std::filesystem::last_write_time(photoPath, ec);
            int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();

            if (!oldHash.empty() && oldHash != newHash) {
                std::filesystem::remove(cache().getThumbnailPath(oldHash, thumbnail::Cache::SmallSize), ec);
                std::filesystem::remove(cache().getThumbnailPath(oldHash, thumbnail::Cache::LargeSize), ec);
            }

            item.exif.orientation = 1;
            cache().ensureDualThumbnails(photoPath, newHash, item.exif.orientation);

            item.content_hash = newHash;
            item.width = newW;
            item.height = newH;
            item.file_size = static_cast<int64_t>(fsize);
            item.file_modified_time = mtimeSec;
            item.thumb_small = thumbnail::Cache::getRelativeThumbnailPath(newHash, thumbnail::Cache::SmallSize);
            item.thumb_large = thumbnail::Cache::getRelativeThumbnailPath(newHash, thumbnail::Cache::LargeSize);
            item.updated_at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

            auto updateStatus = db().updateMedia(item);
            if (!updateStatus.isOk()) {
                sendStatusError(res, updateStatus);
                return;
            }

            if (clipService_) {
                clipService_->removeMedia(item.id);
            }

            sendJson(res, item, 200);
        }
    };

    server.Post(R"(/api/photos/(\d+)/edit)", handleEditPhoto);
    server.Post(R"(/api/media/(\d+)/edit)", handleEditPhoto);

}

void ApiRouter::registerTagRoutes(httplib::Server& server) {
    // GET /api/tags
    server.Get("/api/tags", [this](const httplib::Request& req, httplib::Response& res) {
        auto tagsRes = catalog_ ? catalog_->getTags() : db().getAllTags();
        if (!tagsRes.isOk()) {
            sendStatusError(res, tagsRes.status());
            return;
        }
        sendJson(res, tagsRes.value());
    });

    // POST /api/tags
    server.Post("/api/tags", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("name") || !body["name"].is_string()) {
                sendError(res, "Missing or invalid name");
                return;
            }
            std::string name = body["name"].get<std::string>();
            if (name.empty() || name.size() > kMaxNameLength) {
                sendError(res, "Tag name must not be empty and must be at most " + std::to_string(kMaxNameLength) + " characters", 400);
                return;
            }
            std::string category = body.value("category", "keyword");
            if (category.size() > kMaxCategoryLength) {
                sendError(res, "Category must be at most " + std::to_string(kMaxCategoryLength) + " characters", 400);
                return;
            }

            auto tagRes = catalog_
                ? catalog_->createOrGetTag(name, category)
                : db().createOrGetTag(name, category);

            if (!tagRes.isOk()) {
                sendStatusError(res, tagRes.status());
                return;
            }

            sendJson(res, {{"id", tagRes.value()}, {"name", name}, {"category", category}}, 201);
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/tags/:id
    server.Delete(R"(/api/tags/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto tagIdOpt = parseMatchId<TagId>(req, 1, res, "tag ID");
        if (!tagIdOpt) return;
        TagId tagId = *tagIdOpt;
        Status s = catalog_ ? catalog_->deleteTag(tagId) : db().deleteTag(tagId);
        if (!s.isOk()) {
            sendStatusError(res, s);
            return;
        }
        sendJson(res, {{"status", "ok"}});
    });
}

void ApiRouter::registerAlbumRoutes(httplib::Server& server) {
    // GET /api/albums
    server.Get("/api/albums", [this](const httplib::Request& req, httplib::Response& res) {
        auto albumsRes = catalog_ ? catalog_->getAlbums() : db().getAllAlbums();
        if (!albumsRes.isOk()) {
            sendStatusError(res, albumsRes.status());
            return;
        }
        sendJson(res, albumsRes.value());
    });

    // POST /api/albums
    server.Post("/api/albums", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("name") || !body["name"].is_string()) {
                sendError(res, "Missing or invalid name");
                return;
            }
            std::string name = body["name"].get<std::string>();
            if (name.empty() || name.size() > kMaxAlbumNameLength) {
                sendError(res, "Album name must not be empty and must be at most " + std::to_string(kMaxAlbumNameLength) + " characters", 400);
                return;
            }
            std::string desc = body.value("description", "");
            if (desc.size() > kMaxAlbumDescLength) {
                sendError(res, "Album description must be at most " + std::to_string(kMaxAlbumDescLength) + " characters", 400);
                return;
            }
            bool isSmart = body.value("is_smart", false);
            std::string queryJson = body.value("query_json", "");

            auto albumRes = catalog_
                ? catalog_->createAlbum(name, desc, isSmart, queryJson)
                : db().createAlbum(name, desc, isSmart, queryJson);

            if (!albumRes.isOk()) {
                sendStatusError(res, albumRes.status());
                return;
            }

            sendJson(res, {{"id", albumRes.value()}, {"name", name}}, 201);
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/albums/:id/media
    server.Post(R"(/api/albums/(\d+)/media)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto albumIdOpt = parseMatchId<AlbumId>(req, 1, res, "album ID");
        if (!albumIdOpt) return;
        AlbumId albumId = *albumIdOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<MediaId> mediaIds;
            if (body.contains("media_id") && body["media_id"].is_number_integer()) {
                mediaIds.push_back(body["media_id"].get<MediaId>());
            } else if (body.contains("media_ids") && body["media_ids"].is_array()) {
                if (body["media_ids"].size() > kMaxBatchSize) {
                    sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                    return;
                }
                mediaIds.reserve(body["media_ids"].size());
                for (const auto& mid : body["media_ids"]) {
                    if (!mid.is_number_integer()) {
                        sendError(res, "media_ids must contain only integers", 400);
                        return;
                    }
                    mediaIds.push_back(mid.get<MediaId>());
                }
            } else {
                sendError(res, "Missing media_id or media_ids array");
                return;
            }

            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (MediaId mid : mediaIds) {
                        Status s = catalog_->addMediaToAlbum(albumId, mid);
                        if (!s.isOk()) return s;
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (MediaId mid : mediaIds) {
                        Status s = db().addMediaToAlbum(albumId, mid);
                        if (!s.isOk()) return s;
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"album_id", albumId}, {"added_count", mediaIds.size()}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/albums/:id/order
    server.Post(R"(/api/albums/(\d+)/order)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto albumIdOpt = parseMatchId<AlbumId>(req, 1, res, "album ID");
        if (!albumIdOpt) return;
        AlbumId albumId = *albumIdOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("media_ids") || !body["media_ids"].is_array()) {
                sendError(res, "Missing or invalid media_ids array", 400);
                return;
            }
            if (body["media_ids"].size() > kMaxAlbumOrderSize) {
                sendError(res, "Album order exceeds maximum limit of " +
                               std::to_string(kMaxAlbumOrderSize) + " items", 400);
                return;
            }

            std::vector<MediaId> mediaIds;
            mediaIds.reserve(body["media_ids"].size());
            for (const auto& value : body["media_ids"]) {
                if (!value.is_number_integer()) {
                    sendError(res, "media_ids must contain only integers", 400);
                    return;
                }
                mediaIds.push_back(value.get<MediaId>());
            }

            Status s = catalog_
                ? catalog_->reorderAlbumMedia(albumId, mediaIds)
                : db().reorderAlbumMedia(albumId, mediaIds);
            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }
            sendJson(res, {{"status", "ok"}, {"album_id", albumId}, {"ordered_count", mediaIds.size()}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/albums/:id/cover
    server.Post(R"(/api/albums/(\d+)/cover)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto albumIdOpt = parseMatchId<AlbumId>(req, 1, res, "album ID");
        if (!albumIdOpt) return;
        AlbumId albumId = *albumIdOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("media_id") || !body["media_id"].is_number_integer()) {
                sendError(res, "Missing or invalid media_id", 400);
                return;
            }
            MediaId mediaId = body["media_id"].get<MediaId>();
            Status s = catalog_ ? catalog_->setAlbumCover(albumId, mediaId) : db().setAlbumCover(albumId, mediaId);
            if (!s.isOk()) {
                sendStatusError(res, s);
                return;
            }
            sendJson(res, {{"status", "ok"}, {"album_id", albumId}, {"media_id", mediaId}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/albums/:id/media
    server.Delete(R"(/api/albums/(\d+)/media)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto albumIdOpt = parseMatchId<AlbumId>(req, 1, res, "album ID");
        if (!albumIdOpt) return;
        AlbumId albumId = *albumIdOpt;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<MediaId> mediaIds;
            if (body.contains("media_id") && body["media_id"].is_number_integer()) {
                mediaIds.push_back(body["media_id"].get<MediaId>());
            } else if (body.contains("media_ids") && body["media_ids"].is_array()) {
                if (body["media_ids"].size() > kMaxBatchSize) {
                    sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                    return;
                }
                mediaIds.reserve(body["media_ids"].size());
                for (const auto& mid : body["media_ids"]) {
                    if (!mid.is_number_integer()) {
                        sendError(res, "media_ids must contain only integers", 400);
                        return;
                    }
                    mediaIds.push_back(mid.get<MediaId>());
                }
            } else {
                sendError(res, "Missing media_id or media_ids array");
                return;
            }

            if (mediaIds.empty()) {
                sendError(res, "At least one media ID is required");
                return;
            }

            auto albumRes = catalog_ ? catalog_->getAlbum(albumId) : db().getAlbumById(albumId);
            if (!albumRes.isOk()) {
                sendStatusError(res, albumRes.status());
                return;
            }

            Status batchStatus = catalog_
                ? catalog_->runInTransaction([&]() -> Status {
                    for (MediaId mediaId : mediaIds) {
                        Status s = catalog_->removeMediaFromAlbum(albumId, mediaId);
                        if (!s.isOk()) return s;
                    }
                    return Status::ok();
                })
                : db().runInTransaction([&]() -> Status {
                    for (MediaId mediaId : mediaIds) {
                        Status s = db().removeMediaFromAlbum(albumId, mediaId);
                        if (!s.isOk()) return s;
                    }
                    return Status::ok();
                });
            if (!batchStatus.isOk()) {
                sendStatusError(res, batchStatus);
                return;
            }

            sendJson(res, { {"status", "ok"}, {"album_id", albumId}, {"removed_count", mediaIds.size()} });
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/albums/:id
    server.Delete(R"(/api/albums/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto albumIdOpt = parseMatchId<AlbumId>(req, 1, res, "album ID");
        if (!albumIdOpt) return;
        AlbumId albumId = *albumIdOpt;
        Status s = catalog_ ? catalog_->deleteAlbum(albumId) : db().deleteAlbum(albumId);
        if (!s.isOk()) {
            sendStatusError(res, s);
            return;
        }
        sendJson(res, {{"status", "ok"}});
    });
}

void ApiRouter::registerTimelineRoutes(httplib::Server& server) {
    // GET /api/timeline
    server.Get("/api/timeline", [this](const httplib::Request& req, httplib::Response& res) {
        auto timelineRes = catalog_ ? catalog_->getTimeline() : db().getTimeline();
        if (!timelineRes.isOk()) {
            sendStatusError(res, timelineRes.status());
            return;
        }
        sendJson(res, timelineRes.value());
    });
}

void ApiRouter::registerStatsRoutes(httplib::Server& server) {
    // GET /api/stats
    server.Get("/api/stats", [this](const httplib::Request& req, httplib::Response& res) {
        auto statsRes = catalog_ ? catalog_->getStats() : db().getStats();
        if (!statsRes.isOk()) {
            sendStatusError(res, statsRes.status());
            return;
        }
        sendJson(res, statsRes.value());
    });
}

void ApiRouter::registerFolderRoutes(httplib::Server& server) {
    // GET /api/folders
    server.Get("/api/folders", [this](const httplib::Request& req, httplib::Response& res) {
        auto foldersRes = catalog_ ? catalog_->getFolders() : db().getAllFolders();
        if (!foldersRes.isOk()) {
            sendStatusError(res, foldersRes.status());
            return;
        }
        sendJson(res, foldersRes.value());
    });
}

void ApiRouter::registerImportRoutes(httplib::Server& server) {
    // POST /api/import
    server.Post("/api/import", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("path") || !body["path"].is_string()) {
                sendError(res, "Missing or invalid path");
                return;
            }
            std::string rawPath = body["path"].get<std::string>();
            bool recursive = body.value("recursive", true);

            auto p = pathFromUtf8(rawPath);
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                sendError(res, "Directory does not exist: " + rawPath, 404);
                return;
            }

            // Path canonicalization (resolves '..' traversal and relative paths)
            auto canonicalPath = stripExtendedPrefix(std::filesystem::canonical(p, ec));
            if (ec) {
                sendError(res, "Failed to resolve directory path: " + ec.message(), 400);
                return;
            }

            if (!std::filesystem::is_directory(canonicalPath, ec)) {
                sendError(res, "Path is not a directory: " + rawPath, 400);
                return;
            }

            // Enforce allowed root directories & protect against sensitive system paths
            std::string reason;
            if (!isWithinAllowedRoots(canonicalPath, reason)) {
                sendError(res, reason, 403);
                return;
            }

            // Symlink restrictions: disallow importing directory if it points to sensitive paths
            std::error_code symEc;
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(p, symEc))) {
                if (isSensitiveSystemPath(canonicalPath)) {
                    sendError(res, "Importing symlink pointing to sensitive system directory is forbidden", 403);
                    return;
                }
            }

            // Permission checks: verify directory is readable
            std::error_code itEc;
            std::filesystem::directory_iterator it(canonicalPath, std::filesystem::directory_options::skip_permission_denied, itEc);
            if (itEc) {
                sendError(res, "Permission denied or directory cannot be read: " + itEc.message(), 403);
                return;
            }

            std::string path = pathToUtf8(canonicalPath);

            std::lock_guard<std::mutex> lock(importThreadMutex_);
            if (isImportRunning()) {
                sendError(res, "An import is already running", 409);
                return;
            }

            if (importThread_.joinable()) {
                importThread_.join();
            }

            isImporting_.store(true);
            if (catalog_) {
                importThread_ = std::jthread([this, path, recursive](std::stop_token stopToken) {
                    IMAGINE_LOG_INFO("Starting background import of: " + path);
                    auto res = catalog_->importDirectory(path, recursive, [stopToken, this](const ImportProgress&) {
                        if (stopToken.stop_requested()) {
                            if (catalog_) catalog_->cancelImport();
                        }
                    });
                    if (res.isOk()) {
                        IMAGINE_LOG_INFO("Background import complete: " + std::to_string(res.value().imported_files.load()) + " imported");
                    } else {
                        IMAGINE_LOG_ERROR("Background import error: " + res.status().message());
                    }
                    isImporting_.store(false);
                });
            } else {
                if (fallbackImportState_) {
                    fallbackImportState_->isImporting.store(true);
                }
                cancelFallbackImport_.store(false);

                importThread_ = std::jthread([this, path, recursive](std::stop_token stopToken) {
                    IMAGINE_LOG_INFO("Starting fallback background import: " + path);
                    core::Importer imp(db(), cache());
                    auto res = imp.importDirectory(path, recursive, [this, &imp, stopToken](const ImportProgress& p) {
                        if (stopToken.stop_requested() || cancelFallbackImport_.load()) {
                            imp.cancel();
                        }
                        if (fallbackImportState_) {
                            std::lock_guard<std::mutex> lock(fallbackImportState_->mutex);
                            fallbackImportState_->progress = p;
                        }
                    });
                    if (fallbackImportState_) {
                        std::lock_guard<std::mutex> lock(fallbackImportState_->mutex);
                        if (res.isOk()) {
                            fallbackImportState_->progress = res.value();
                        }
                        fallbackImportState_->isImporting.store(false);
                    }
                    isImporting_.store(false);
                });
            }

            sendJson(res, {{"status", "started"}, {"path", path}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/import/cancel
    server.Post("/api/import/cancel", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            stopImport();
            sendJson(res, {{"status", "cancelled"}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Error cancelling import: ") + ex.what(), 500);
        }
    });

    // GET /api/import/progress
    server.Get("/api/import/progress", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            if (catalog_) {
                auto progress = catalog_->importer().currentProgress();
                sendJson(res, progress);
            } else {
                if (fallbackImportState_) {
                    std::lock_guard<std::mutex> lock(fallbackImportState_->mutex);
                    sendJson(res, fallbackImportState_->progress);
                } else {
                    sendJson(res, ImportProgress{});
                }
            }
        } catch (const std::exception& ex) {
            IMAGINE_LOG_ERROR("Error getting import progress: " + std::string(ex.what()));
            sendError(res, "Failed to get import progress", 500);
        }
    });
}

namespace {
struct GeocodeCacheEntry {
    std::chrono::steady_clock::time_point timestamp;
    std::string responseBody;
};
static std::mutex g_geocodeCacheMutex;
static std::unordered_map<std::string, GeocodeCacheEntry> g_geocodeCache;
static std::mutex g_geocodeRateMutex;
static std::chrono::steady_clock::time_point g_nextAllowedGeocodeTime{};
} // anonymous namespace

void ApiRouter::registerGeocodeRoutes(httplib::Server& server) {
    // GET /api/geocode?q=...&limit=...
    server.Get("/api/geocode", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("q")) {
            sendError(res, "Missing required query parameter 'q'", 400);
            return;
        }
        std::string query = req.get_param_value("q");
        if (query.empty()) {
            sendJson(res, nlohmann::json::array());
            return;
        }

        int limitVal = 5;
        if (req.has_param("limit")) {
            try {
                size_t idx = 0;
                std::string lStr = req.get_param_value("limit");
                int parsed = std::stoi(lStr, &idx);
                if (idx != lStr.size() || parsed < 1 || parsed > 10) {
                    sendError(res, "Query parameter 'limit' must be an integer between 1 and 10", 400);
                    return;
                }
                limitVal = parsed;
            } catch (...) {
                sendError(res, "Query parameter 'limit' must be an integer between 1 and 10", 400);
                return;
            }
        }

        std::string cacheKey = query + "|" + std::to_string(limitVal);
        {
            std::lock_guard<std::mutex> lock(g_geocodeCacheMutex);
            auto it = g_geocodeCache.find(cacheKey);
            if (it != g_geocodeCache.end()) {
                auto age = std::chrono::duration_cast<std::chrono::hours>(std::chrono::steady_clock::now() - it->second.timestamp);
                if (age < std::chrono::hours(24)) {
                    res.status = 200;
                    res.set_content(it->second.responseBody, "application/json");
                    return;
                }
            }
        }

        try {
            // Respect Nominatim rate limit: max 1 request per second without holding lock while sleeping
            // Bound backlog to avoid starving worker threads
            constexpr auto kMaxWait = std::chrono::seconds(5);
            std::chrono::milliseconds waitMs{0};
            {
                std::lock_guard<std::mutex> lock(g_geocodeRateMutex);
                auto now = std::chrono::steady_clock::now();
                if (g_nextAllowedGeocodeTime > now) {
                    waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(g_nextAllowedGeocodeTime - now);
                    if (waitMs > kMaxWait) {
                        sendError(res, "Geocoding service rate limit backlog exceeded, try again later", 429);
                        return;
                    }
                    g_nextAllowedGeocodeTime = g_nextAllowedGeocodeTime + std::chrono::milliseconds(1000);
                } else {
                    g_nextAllowedGeocodeTime = now + std::chrono::milliseconds(1000);
                }
            }
            if (waitMs.count() > 0) {
                std::this_thread::sleep_for(waitMs);
            }

            // Re-check cache after wait in case a concurrent request with the same query finished
            {
                std::lock_guard<std::mutex> lock(g_geocodeCacheMutex);
                auto it = g_geocodeCache.find(cacheKey);
                if (it != g_geocodeCache.end()) {
                    auto age = std::chrono::duration_cast<std::chrono::hours>(std::chrono::steady_clock::now() - it->second.timestamp);
                    if (age < std::chrono::hours(24)) {
                        res.status = 200;
                        res.set_content(it->second.responseBody, "application/json");
                        return;
                    }
                }
            }

            httplib::SSLClient cli("nominatim.openstreetmap.org");
            cli.set_connection_timeout(5, 0);
            cli.set_read_timeout(5, 0);
            httplib::Headers headers = {
                {"User-Agent", "ImaginePhotoCatalog/1.0 (https://github.com/vblosh/imagine; contact: imagine-app@github.com)"},
                {"Accept", "application/json"}
            };

            // URL-encode query
            std::string encodedQuery;
            for (char c : query) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
                    encodedQuery += c;
                } else if (c == ' ') {
                    encodedQuery += '+';
                } else {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                    encodedQuery += buf;
                }
            }

            std::string path = "/search?format=json&q=" + encodedQuery + "&limit=" + std::to_string(limitVal);
            auto osmRes = cli.Get(path.c_str(), headers);
            if (osmRes && osmRes->status == 200) {
                {
                    std::lock_guard<std::mutex> lock(g_geocodeCacheMutex);
                    constexpr size_t kMaxGeocodeCacheEntries = 500;
                    if (g_geocodeCache.size() >= kMaxGeocodeCacheEntries) {
                        auto now = std::chrono::steady_clock::now();
                        for (auto it = g_geocodeCache.begin(); it != g_geocodeCache.end(); ) {
                            if (now - it->second.timestamp >= std::chrono::hours(24)) {
                                it = g_geocodeCache.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        if (g_geocodeCache.size() >= kMaxGeocodeCacheEntries) {
                            auto oldestIt = std::min_element(g_geocodeCache.begin(), g_geocodeCache.end(),
                                [](const auto& a, const auto& b) { return a.second.timestamp < b.second.timestamp; });
                            if (oldestIt != g_geocodeCache.end()) g_geocodeCache.erase(oldestIt);
                        }
                    }
                    g_geocodeCache[cacheKey] = GeocodeCacheEntry{std::chrono::steady_clock::now(), osmRes->body};
                }
                res.status = 200;
                res.set_content(osmRes->body, "application/json");
            } else {
                sendError(res, "Geocoding upstream service unavailable", 502);
            }
        } catch (const std::exception& ex) {
            sendError(res, std::string("Geocoding failed: ") + ex.what(), 502);
        }
    });
}

void ApiRouter::registerFaceRoutes(httplib::Server& server) {
    server.Get("/api/faces/status", [this](const httplib::Request&, httplib::Response& res) {
        if (faceService_) {
            sendJson(res, faceService_->status());
        } else {
            nlohmann::json j;
            j["built"] = false;
            j["ready"] = false;
            sendJson(res, j);
        }
    });

    server.Post("/api/faces/jobs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.is_object() || !body.contains("scope") || !body["scope"].is_string()) {
                sendError(res, "scope must be selected or catalog", 400);
                return;
            }
            std::string scope = body["scope"].get<std::string>();
            if (scope == "catalog" && body.contains("media_ids")) {
                sendError(res, "media_ids may only be supplied with selected scope", 400);
                return;
            }
            if (scope == "selected") {
                if (!body.contains("media_ids") || !body["media_ids"].is_array() || body["media_ids"].empty()) {
                    sendError(res, "media_ids must be non-empty when scope is selected", 400);
                    return;
                }
            }
            bool force = false;
            if (body.contains("force")) {
                if (!body["force"].is_boolean()) { sendError(res, "force must be a boolean", 400); return; }
                force = body["force"].get<bool>();
            }
            std::vector<MediaId> ids;
            if (body.contains("media_ids")) {
                if (!body["media_ids"].is_array()) { sendError(res, "media_ids must be an array", 400); return; }
                for (const auto& value : body["media_ids"]) {
                    if (!value.is_number_integer()) { sendError(res, "media_ids must contain integer IDs", 400); return; }
                    ids.push_back(value.get<MediaId>());
                }
            }
            int64_t jobId = 0;
            Status status = faceService_->startJob(scope, ids, force, jobId);
            if (!status.isOk()) {
                if (status.code() == StatusCode::IoError) sendError(res, status.message(), 503);
                else sendStatusError(res, status);
                return;
            }
            auto job = faceService_->getJob(jobId);
            if (!job.isOk()) { sendStatusError(res, job.status()); return; }
            sendJson(res, job.value(), 202);
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
        }
    });

    server.Get(R"(/api/faces/jobs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto jobIdOpt = parseMatchId<int64_t>(req, 1, res, "job ID");
        if (!jobIdOpt) return;
        auto result = faceService_->getJob(*jobIdOpt);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        sendJson(res, result.value());
    });

    server.Post(R"(/api/faces/jobs/(\d+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto jobIdOpt = parseMatchId<int64_t>(req, 1, res, "job ID");
        if (!jobIdOpt) return;
        Status status = faceService_->cancelJob(*jobIdOpt);
        if (!status.isOk()) { sendStatusError(res, status); return; }
        auto job = faceService_->getJob(*jobIdOpt);
        if (!job.isOk()) { sendStatusError(res, job.status()); return; }
        sendJson(res, job.value());
    });

    server.Get(R"(/api/faces/media/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto mediaIdOpt = parseMatchId<MediaId>(req, 1, res, "media ID");
        if (!mediaIdOpt) return;
        auto result = faceService_->getMedia(*mediaIdOpt);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        sendJson(res, result.value());
    });

    server.Get("/api/faces/review", [this](const httplib::Request& req, httplib::Response& res) {
        auto offset = parseQueryInt(req, "offset", 0);
        auto limit = parseQueryInt(req, "limit", 50);
        if (!offset || !limit) { sendError(res, "offset and limit must be integers", 400); return; }
        if (*offset < 0 || *limit < 1 || *limit > 200) {
            sendError(res, "offset must be nonnegative and limit must be between 1 and 200", 400);
            return;
        }
        auto result = faceService_->getReview(*offset, *limit);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        sendJson(res, result.value());
    });

    server.Get("/api/faces/grid", [this](const httplib::Request& req, httplib::Response& res) {
        auto offset = parseQueryInt(req, "offset", 0);
        auto limit = parseQueryInt(req, "limit", 200);
        if (!offset || !limit) { sendError(res, "offset and limit must be integers", 400); return; }
        if (*offset < 0 || *limit < 1 || *limit > 1000) {
            sendError(res, "job_id must be positive, offset nonnegative, and limit between 1 and 1000", 400);
            return;
        }

        std::optional<int64_t> jobId;
        if (req.has_param("job_id")) {
            try {
                std::string value = req.get_param_value("job_id");
                size_t used = 0;
                int64_t parsed = std::stoll(value, &used);
                if (used != value.size() || parsed <= 0) { sendError(res, "job_id must be a positive integer", 400); return; }
                jobId = parsed;
            } catch (...) { sendError(res, "job_id must be a positive integer", 400); return; }
        }

        bool includeDismissed = false;
        if (req.has_param("include_dismissed")) {
            const std::string value = req.get_param_value("include_dismissed");
            if (value == "true") includeDismissed = true;
            else if (value != "false") { sendError(res, "include_dismissed must be true or false", 400); return; }
        }
        auto result = faceService_->getGrid(jobId, *offset, *limit, includeDismissed);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        sendJson(res, result.value());
    });

    server.Get(R"(/api/faces/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto faceIdOpt = parseMatchId<int64_t>(req, 1, res, "face ID");
        if (!faceIdOpt) return;
        auto result = faceService_->getFace(*faceIdOpt);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        sendJson(res, result.value());
    });

    server.Get("/api/faces/lookup", [this](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("ids")) { sendError(res, "ids is required", 400); return; }
        const std::string encodedIds = req.get_param_value("ids");
        std::vector<int64_t> ids;
        std::unordered_set<int64_t> uniqueIds;
        size_t start = 0;
        while (start <= encodedIds.size()) {
            const size_t end = encodedIds.find(',', start);
            const size_t length = (end == std::string::npos ? encodedIds.size() : end) - start;
            const std::string token = encodedIds.substr(start, length);
            if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char c) {
                    return c >= '0' && c <= '9';
                })) {
                sendError(res, "ids must be a comma-separated list of unique positive integers", 400); return;
            }
            int64_t id = 0;
            try {
                size_t used = 0;
                id = std::stoll(token, &used);
                if (used != token.size() || id <= 0) {
                    sendError(res, "ids must be a comma-separated list of unique positive integers", 400); return;
                }
            } catch (...) {
                sendError(res, "ids must be a comma-separated list of unique positive integers", 400); return;
            }
            if (!uniqueIds.insert(id).second) {
                sendError(res, "ids must not contain duplicates", 400); return;
            }
            ids.push_back(id);
            if (ids.size() > 200) { sendError(res, "at most 200 face IDs are allowed", 400); return; }
            if (end == std::string::npos) break;
            start = end + 1;
        }

        auto result = faceService_->lookupFaces(ids);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        nlohmann::json items = nlohmann::json::array();
        nlohmann::json failed = nlohmann::json::array();
        for (const auto& outcome : result.value()) {
            if (outcome.status.isOk()) items.push_back(outcome.face);
            else failed.push_back({{"id", outcome.id}, {"status", statusToHttpCode(outcome.status)},
                                   {"error", sanitizeUtf8(outcome.status.message())}});
        }
        sendJson(res, {{"items", std::move(items)}, {"failed", std::move(failed)}});
    });

    server.Get("/api/faces/groups", [this](const httplib::Request& req, httplib::Response& res) {
        auto parseId = [&](const char* key) -> std::optional<int64_t> {
            if (!req.has_param(key)) return std::nullopt;
            try {
                const std::string value = req.get_param_value(key);
                size_t used = 0;
                const int64_t parsed = std::stoll(value, &used);
                if (used != value.size() || parsed <= 0) return std::nullopt;
                return parsed;
            } catch (...) { return std::nullopt; }
        };
        auto parseBoolean = [&](const char* key, bool fallback) -> std::optional<bool> {
            if (!req.has_param(key)) return fallback;
            const std::string value = req.get_param_value(key);
            if (value == "true") return true;
            if (value == "false") return false;
            return std::nullopt;
        };
        std::optional<int64_t> jobId;
        if (req.has_param("job_id")) {
            jobId = parseId("job_id");
            if (!jobId) { sendError(res, "job_id must be a positive integer", 400); return; }
        }
        std::optional<TagId> personTagId;
        if (req.has_param("person_tag_id")) {
            auto parsed = parseId("person_tag_id");
            if (!parsed) { sendError(res, "person_tag_id must be a positive integer", 400); return; }
            personTagId = *parsed;
        }
        auto showNamed = parseBoolean("show_named", true);
        auto showUnnamed = parseBoolean("show_unnamed", true);
        auto includeDismissed = parseBoolean("include_dismissed", false);
        if (!showNamed || !showUnnamed || !includeDismissed) {
            sendError(res, "show_named, show_unnamed, and include_dismissed must be true or false", 400);
            return;
        }
        auto result = faceService_->getGroups(jobId, *showNamed, *showUnnamed,
                                              *includeDismissed, personTagId);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        sendJson(res, result.value());
    });

    server.Get("/api/faces/group", [this](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("snapshot") || !req.has_param("key")) {
            sendError(res, "snapshot and key are required", 400); return;
        }
        auto offset = parseQueryInt(req, "offset", 0);
        auto limit = parseQueryInt(req, "limit", 100);
        if (!offset || !limit) { sendError(res, "offset and limit must be integers", 400); return; }
        if (*offset < 0 || *limit < 1 || *limit > 200) {
            sendError(res, "snapshot and key are required; offset must be nonnegative and limit between 1 and 200", 400);
            return;
        }
        auto result = faceService_->getGroupPage(req.get_param_value("snapshot"),
            req.get_param_value("key"), *offset, *limit);
        if (!result.isOk()) { sendStatusError(res, result.status()); return; }
        sendJson(res, result.value());
    });

    server.Post("/api/faces/groups/accept", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.is_object() || !body.contains("snapshot") || !body["snapshot"].is_string() ||
                !body.contains("key") || !body["key"].is_string()) {
                sendError(res, "snapshot and key strings are required", 400); return;
            }
            auto accepted = faceService_->acceptGroup(body["snapshot"].get<std::string>(),
                                                       body["key"].get<std::string>());
            if (!accepted.isOk()) { sendStatusError(res, accepted.status()); return; }
            nlohmann::json failed = nlohmann::json::array();
            int64_t updatedCount = 0;
            for (const auto& outcome : accepted.value()) {
                if (outcome.status.isOk()) {
                    ++updatedCount;
                } else {
                    failed.push_back({{"id", outcome.id}, {"status", statusToHttpCode(outcome.status)},
                                      {"error", sanitizeUtf8(outcome.status.message())}});
                }
            }
            sendJson(res, {{"updated_count", updatedCount}, {"failed_count", failed.size()},
                           {"failed", std::move(failed)}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid request: ") + ex.what(), 400);
        }
    });

    server.Post("/api/faces/batch", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.is_object() || !body.contains("action") || !body["action"].is_string() ||
                !body.contains("faces") || !body["faces"].is_array()) {
                sendError(res, "action and faces are required", 400); return;
            }
            const std::string action = body["action"].get<std::string>();
            if (action != "identity" && action != "accept" && action != "dismiss" && action != "reject") {
                sendError(res, "action must be identity, accept, dismiss, or reject", 400); return;
            }
            const auto& faces = body["faces"];
            if (faces.empty() || faces.size() > 1000) {
                sendError(res, "faces must contain between 1 and 1000 entries", 400); return;
            }

            struct FaceRevision { int64_t id; int64_t revision; std::optional<TagId> tagId; };
            std::vector<FaceRevision> edits;
            edits.reserve(faces.size());
            std::unordered_set<int64_t> uniqueIds;
            for (const auto& face : faces) {
                if (!face.is_object() || !face.contains("id") || !face["id"].is_number_integer() ||
                    !face.contains("revision") || !face["revision"].is_number_integer()) {
                    sendError(res, "each face must contain integer id and revision fields", 400); return;
                }
                const int64_t id = face["id"].get<int64_t>();
                const int64_t revision = face["revision"].get<int64_t>();
                if (id <= 0 || revision <= 0 || !uniqueIds.insert(id).second) {
                    sendError(res, "face IDs and revisions must be positive and IDs unique", 400); return;
                }
                std::optional<TagId> faceTagId;
                if (face.contains("tag_id") && !face["tag_id"].is_null()) {
                    if (!face["tag_id"].is_number_integer() || face["tag_id"].get<int64_t>() <= 0) {
                        sendError(res, "tag_id must be a positive integer", 400); return;
                    }
                    faceTagId = face["tag_id"].get<TagId>();
                }
                edits.push_back({id, revision, faceTagId});
            }

            std::optional<TagId> tagId;
            std::optional<std::string> name;
            bool dismissed = false;
            if (action == "identity") {
                const bool hasTag = body.contains("tag_id");
                const bool hasName = body.contains("name");
                if (hasTag == hasName) { sendError(res, "Provide exactly one of tag_id or name", 400); return; }
                if (hasTag && !body["tag_id"].is_null()) {
                    if (!body["tag_id"].is_number_integer() || body["tag_id"].get<int64_t>() <= 0) {
                        sendError(res, "tag_id must be a positive integer or null", 400); return;
                    }
                    tagId = body["tag_id"].get<TagId>();
                }
                if (hasName) {
                    if (!body["name"].is_string()) { sendError(res, "name must be a string", 400); return; }
                    name = body["name"].get<std::string>();
                }
            } else if (action == "accept") {
                if (!body.contains("tag_id") || !body["tag_id"].is_number_integer() ||
                    body["tag_id"].get<int64_t>() <= 0) {
                    sendError(res, "accept requires a positive tag_id", 400); return;
                }
                tagId = body["tag_id"].get<TagId>();
            } else if (action == "reject") {
                if (body.contains("tag_id") && !body["tag_id"].is_null()) {
                    if (!body["tag_id"].is_number_integer() || body["tag_id"].get<int64_t>() <= 0) {
                        sendError(res, "tag_id must be a positive integer", 400); return;
                    }
                    tagId = body["tag_id"].get<TagId>();
                }
            } else {
                if (!body.contains("dismissed") || !body["dismissed"].is_boolean()) {
                    sendError(res, "dismiss requires a boolean dismissed value", 400); return;
                }
                dismissed = body["dismissed"].get<bool>();
            }

            if (action == "accept") {
                std::vector<faces::SuggestionAcceptance> acceptances;
                acceptances.reserve(edits.size());
                for (const auto& edit : edits) acceptances.push_back({edit.id, edit.revision, *tagId});
                auto outcomes = faceService_->acceptSuggestionsBatch(acceptances);
                if (!outcomes.isOk()) { sendStatusError(res, outcomes.status()); return; }
                nlohmann::json updated = nlohmann::json::array();
                nlohmann::json failed = nlohmann::json::array();
                for (auto& outcome : outcomes.value()) {
                    if (outcome.status.isOk()) {
                        updated.push_back(std::move(outcome.face));
                    } else {
                        failed.push_back({{"id", outcome.id}, {"status", statusToHttpCode(outcome.status)},
                                          {"error", sanitizeUtf8(outcome.status.message())}});
                    }
                }
                sendJson(res, {{"updated", std::move(updated)}, {"failed", std::move(failed)}});
                return;
            }

            if (action == "reject") {
                std::vector<faces::SuggestionRejection> rejections;
                rejections.reserve(edits.size());
                for (const auto& edit : edits) {
                    const std::optional<TagId> tId = edit.tagId.has_value() ? edit.tagId : tagId;
                    if (!tId.has_value()) {
                        sendError(res, "reject requires tag_id either per face or in top-level body", 400);
                        return;
                    }
                    rejections.push_back({edit.id, edit.revision, *tId});
                }
                auto outcomes = faceService_->rejectSuggestionsBatch(rejections);
                if (!outcomes.isOk()) { sendStatusError(res, outcomes.status()); return; }
                nlohmann::json updated = nlohmann::json::array();
                nlohmann::json failed = nlohmann::json::array();
                for (auto& outcome : outcomes.value()) {
                    if (outcome.status.isOk()) {
                        updated.push_back(std::move(outcome.face));
                    } else {
                        failed.push_back({{"id", outcome.id}, {"status", statusToHttpCode(outcome.status)},
                                          {"error", sanitizeUtf8(outcome.status.message())}});
                    }
                }
                sendJson(res, {{"updated", std::move(updated)}, {"failed", std::move(failed)}});
                return;
            }

            if (action == "dismiss") {
                std::vector<faces::FaceDismissal> dismissals;
                dismissals.reserve(edits.size());
                for (const auto& edit : edits) dismissals.push_back({edit.id, edit.revision});
                auto outcomes = faceService_->setDismissedBatch(dismissals, dismissed);
                if (!outcomes.isOk()) { sendStatusError(res, outcomes.status()); return; }
                nlohmann::json updated = nlohmann::json::array();
                nlohmann::json failed = nlohmann::json::array();
                for (auto& outcome : outcomes.value()) {
                    if (outcome.status.isOk()) {
                        updated.push_back(std::move(outcome.face));
                    } else {
                        failed.push_back({{"id", outcome.id}, {"status", statusToHttpCode(outcome.status)},
                                          {"error", sanitizeUtf8(outcome.status.message())}});
                    }
                }
                sendJson(res, {{"updated", std::move(updated)}, {"failed", std::move(failed)}});
                return;
            }

            nlohmann::json updated = nlohmann::json::array();
            nlohmann::json failed = nlohmann::json::array();
            for (const auto& edit : edits) {
                const bool includeSuggestions = !tagId.has_value() && !name.has_value();
                Result<nlohmann::json> result = faceService_->setIdentity(
                    edit.id, edit.revision, tagId, name, includeSuggestions);
                if (result.isOk()) {
                    updated.push_back(std::move(result.value()));
                } else {
                    const Status status = result.status();
                    failed.push_back({{"id", edit.id}, {"status", statusToHttpCode(status)},
                                      {"error", sanitizeUtf8(status.message())}});
                }
            }
            sendJson(res, {{"updated", std::move(updated)}, {"failed", std::move(failed)}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid request: ") + ex.what(), 400);
        }
    });

    server.Post(R"(/api/faces/(\d+)/identity)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto faceIdOpt = parseMatchId<int64_t>(req, 1, res, "face ID");
        if (!faceIdOpt) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.is_object() || !body.contains("revision") || !body["revision"].is_number_integer()) {
                sendError(res, "revision must be a positive integer", 400); return;
            }
            const bool hasTag = body.contains("tag_id");
            const bool hasName = body.contains("name");
            if (hasTag == hasName) { sendError(res, "Provide exactly one of tag_id or name", 400); return; }
            std::optional<TagId> tagId;
            std::optional<std::string> name;
            if (hasTag && !body["tag_id"].is_null()) {
                if (!body["tag_id"].is_number_integer()) { sendError(res, "tag_id must be an integer or null", 400); return; }
                tagId = body["tag_id"].get<TagId>();
            }
            if (hasName) {
                if (!body["name"].is_string()) { sendError(res, "name must be a string", 400); return; }
                name = body["name"].get<std::string>();
            }
            int64_t revision = body["revision"].get<int64_t>();
            if (revision <= 0) { sendError(res, "revision must be positive", 400); return; }
            auto result = faceService_->setIdentity(*faceIdOpt, revision, tagId, name);
            if (!result.isOk()) { sendStatusError(res, result.status()); return; }
            sendJson(res, result.value());
        } catch (const std::exception& ex) { sendError(res, std::string("Invalid request: ") + ex.what(), 400); }
    });

    auto suggestionRoute = [this](const httplib::Request& req, httplib::Response& res, bool accept) {
        if (!checkAuth(req, res)) return;
        auto faceIdOpt = parseMatchId<int64_t>(req, 1, res, "face ID");
        if (!faceIdOpt) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.is_object() || !body.contains("revision") || !body["revision"].is_number_integer() ||
                !body.contains("tag_id") || !body["tag_id"].is_number_integer()) {
                sendError(res, "revision and tag_id must be integers", 400); return;
            }
            int64_t revision = body["revision"].get<int64_t>();
            TagId tagId = body["tag_id"].get<TagId>();
            if (revision <= 0 || tagId <= 0) { sendError(res, "revision and tag_id must be positive", 400); return; }
            int64_t faceId = *faceIdOpt;
            auto result = accept ? faceService_->acceptSuggestion(faceId, revision, tagId)
                                 : faceService_->rejectSuggestion(faceId, revision, tagId);
            if (!result.isOk()) { sendStatusError(res, result.status()); return; }
            sendJson(res, result.value());
        } catch (const std::exception& ex) { sendError(res, std::string("Invalid request: ") + ex.what(), 400); }
    };
    server.Post(R"(/api/faces/(\d+)/accept)", [suggestionRoute](const httplib::Request& req, httplib::Response& res) { suggestionRoute(req, res, true); });
    server.Post(R"(/api/faces/(\d+)/reject)", [suggestionRoute](const httplib::Request& req, httplib::Response& res) { suggestionRoute(req, res, false); });

    server.Post(R"(/api/faces/(\d+)/dismiss)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto faceIdOpt = parseMatchId<int64_t>(req, 1, res, "face ID");
        if (!faceIdOpt) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.is_object() || !body.contains("revision") || !body["revision"].is_number_integer() ||
                !body.contains("dismissed") || !body["dismissed"].is_boolean()) {
                sendError(res, "revision and dismissed are required", 400); return;
            }
            int64_t revision = body["revision"].get<int64_t>();
            if (revision <= 0) { sendError(res, "revision must be positive", 400); return; }
            auto result = faceService_->setDismissed(*faceIdOpt, revision, body["dismissed"].get<bool>());
            if (!result.isOk()) { sendStatusError(res, result.status()); return; }
            sendJson(res, result.value());
        } catch (const std::exception& ex) { sendError(res, std::string("Invalid request: ") + ex.what(), 400); }
    });

    server.Get(R"(/api/faces/(\d+)/crop)", [this](const httplib::Request& req, httplib::Response& res) {
        auto faceIdOpt = parseMatchId<int64_t>(req, 1, res, "face ID");
        if (!faceIdOpt) return;
        if (!req.has_param("revision")) { sendError(res, "revision query parameter is required", 400); return; }
        try {
            size_t used = 0;
            std::string value = req.get_param_value("revision");
            int64_t revision = std::stoll(value, &used);
            if (used != value.size() || revision <= 0) { sendError(res, "revision must be a positive integer", 400); return; }
            auto path = faceService_->cropPath(*faceIdOpt, revision);
            if (!path.isOk()) { sendStatusError(res, path.status()); return; }
            std::error_code ec;
            auto fsPath = pathFromUtf8(path.value());
            if (!std::filesystem::exists(fsPath, ec) || !std::filesystem::is_regular_file(fsPath, ec)) {
                sendError(res, "Face crop is unavailable", 404);
                return;
            }
            res.set_header("Cache-Control", "private, max-age=3600");
            res.set_file_content(path.value(), "image/jpeg");
        } catch (...) { sendError(res, "Invalid crop request", 400); }
    });
}

void ApiRouter::registerSemanticRoutes(httplib::Server& server) {
    server.Get("/api/semantic/status", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (clipService_) {
            sendJson(res, clipService_->status());
        } else {
            nlohmann::json j;
            j["built"] = false;
            j["ready"] = false;
            sendJson(res, j);
        }
    });

    server.Post("/api/semantic/search", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Semantic search not built", 400);
            return;
        }
        try {
            auto body = nlohmann::json::parse(req.body);
            clip::SearchRequest sreq;
            if (body.contains("query") && body["query"].is_string()) {
                sreq.query = body["query"].get<std::string>();
            }
            if (sreq.query.empty() || sreq.query.size() > 500) {
                sendError(res, "Query required, non-empty, max 500 chars", 400);
                return;
            }
            if (body.contains("limit") && body["limit"].is_number_integer()) {
                sreq.limit = body["limit"].get<int>();
            }
            if (sreq.limit < 1 || sreq.limit > 1000) {
                sendError(res, "Limit must be between 1 and 1000", 400);
                return;
            }
            if (body.contains("filters") && body["filters"].is_object()) {
                auto& f = body["filters"];
                if (f.contains("dateFrom")) {
                    if (f["dateFrom"].is_number()) {
                        sreq.dateFrom = f["dateFrom"].get<int64_t>();
                    } else if (f["dateFrom"].is_string()) {
                        try { sreq.dateFrom = std::stoll(f["dateFrom"].get<std::string>()); } catch (...) {}
                    }
                }
                if (f.contains("dateTo")) {
                    if (f["dateTo"].is_number()) {
                        sreq.dateTo = f["dateTo"].get<int64_t>();
                    } else if (f["dateTo"].is_string()) {
                        try { sreq.dateTo = std::stoll(f["dateTo"].get<std::string>()); } catch (...) {}
                    }
                }
                if (f.contains("ratingMin") && f["ratingMin"].is_number()) sreq.ratingMin = f["ratingMin"].get<int32_t>();
                if (f.contains("mediaType") && f["mediaType"].is_string()) sreq.mediaType = f["mediaType"].get<std::string>();
            }
            
            auto result = clipService_->search(sreq);
            if (result.isOk()) {
                nlohmann::json j = result.value();
                std::vector<MediaItem> mediaItems;
                std::vector<MediaId> foundIds;
                mediaItems.reserve(result.value().items.size());
                foundIds.reserve(result.value().items.size());
                for (const auto& hit : result.value().items) {
                    auto mRes = catalog_ ? catalog_->getMedia(hit.mediaId) : db().getMediaById(hit.mediaId);
                    if (mRes.isOk()) {
                        if (!catalog_) foundIds.push_back(hit.mediaId);
                        mediaItems.push_back(std::move(mRes.value()));
                    }
                }
                if (!catalog_ && !foundIds.empty()) {
                    auto batchRes = db().getTagsForMediaBatch(foundIds);
                    if (batchRes.isOk()) {
                        auto& tagsMap = batchRes.value();
                        for (auto& item : mediaItems) {
                            auto it = tagsMap.find(item.id);
                            if (it != tagsMap.end()) item.tags = std::move(it->second);
                        }
                    }
                }
                j["media_items"] = mediaItems;
                sendJson(res, j);
            } else {
                sendStatusError(res, result.status());
            }
        } catch (...) {
            sendError(res, "Invalid JSON", 400);
        }
    });

    server.Post(R"(/api/semantic/similar/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Semantic search not built", 400);
            return;
        }
        auto mediaIdOpt = parseMatchId<MediaId>(req, 1, res, "Media ID");
        if (!mediaIdOpt) return;
        
        int limit = 20;
        try {
            if (!req.body.empty()) {
                auto body = nlohmann::json::parse(req.body);
                if (body.contains("limit") && body["limit"].is_number_integer()) {
                    limit = body["limit"].get<int>();
                }
            }
        } catch (...) {
            sendError(res, "Invalid JSON", 400);
            return;
        }
        if (limit < 1 || limit > 1000) {
            sendError(res, "Limit must be between 1 and 1000", 400);
            return;
        }
        
        auto result = clipService_->findSimilar(*mediaIdOpt, limit);
        if (result.isOk()) {
            nlohmann::json j = result.value();
            std::vector<MediaItem> mediaItems;
            std::vector<MediaId> foundIds;
            mediaItems.reserve(result.value().items.size());
            foundIds.reserve(result.value().items.size());
            for (const auto& hit : result.value().items) {
                auto mRes = catalog_ ? catalog_->getMedia(hit.mediaId) : db().getMediaById(hit.mediaId);
                if (mRes.isOk()) {
                    if (!catalog_) foundIds.push_back(hit.mediaId);
                    mediaItems.push_back(std::move(mRes.value()));
                }
            }
            if (!catalog_ && !foundIds.empty()) {
                auto batchRes = db().getTagsForMediaBatch(foundIds);
                if (batchRes.isOk()) {
                    auto& tagsMap = batchRes.value();
                    for (auto& item : mediaItems) {
                        auto it = tagsMap.find(item.id);
                        if (it != tagsMap.end()) item.tags = std::move(it->second);
                    }
                }
            }
            j["media_items"] = mediaItems;
            sendJson(res, j);
        } else {
            sendStatusError(res, result.status());
        }
    });

    server.Post("/api/semantic/jobs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Semantic search not built", 400);
            return;
        }
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string scope = body.value("scope", "catalog");
            if (scope != "catalog" && scope != "selected") {
                sendError(res, "scope must be selected or catalog", 400);
                return;
            }
            if (scope == "catalog" && body.contains("media_ids")) {
                sendError(res, "media_ids may only be supplied with selected scope", 400);
                return;
            }
            bool force = body.value("force", false);
            std::vector<MediaId> mediaIds;
            if (body.contains("media_ids") && body["media_ids"].is_array()) {
                for (const auto& el : body["media_ids"]) {
                    if (!el.is_number_integer()) {
                        sendError(res, "media_ids must contain integer IDs", 400);
                        return;
                    }
                    mediaIds.push_back(el.get<MediaId>());
                }
            }
            int64_t jobId = 0;
            auto status = clipService_->startJob(scope, mediaIds, force, jobId);
            if (status.isOk()) {
                nlohmann::json j;
                j["job_id"] = jobId;
                j["status"] = "ok";
                sendJson(res, j);
            } else {
                sendStatusError(res, status);
            }
        } catch (...) {
            sendError(res, "Invalid JSON", 400);
        }
    });

    server.Get(R"(/api/semantic/jobs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Semantic search not built", 400);
            return;
        }
        auto jobIdOpt = parseMatchId<int64_t>(req, 1, res, "Job ID");
        if (!jobIdOpt) return;
        
        auto result = clipService_->getJob(*jobIdOpt, "index");
        if (result.isOk()) {
            sendJson(res, result.value());
        } else {
            sendStatusError(res, result.status());
        }
    });

    server.Post(R"(/api/semantic/jobs/(\d+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Semantic search not built", 400);
            return;
        }
        auto jobIdOpt = parseMatchId<int64_t>(req, 1, res, "Job ID");
        if (!jobIdOpt) return;
        
        auto status = clipService_->cancelJob(*jobIdOpt, "index");
        if (status.isOk()) {
            nlohmann::json j;
            j["status"] = "ok";
            sendJson(res, j);
        } else {
            sendStatusError(res, status);
        }
    });
}


void ApiRouter::registerSmartTagRoutes(httplib::Server& server) {
    server.Get("/api/smart-tags/status", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (clipService_) {
            sendJson(res, clipService_->smartTagStatus());
        } else {
            nlohmann::json j;
            j["built"] = false;
            j["ready"] = false;
            j["taxonomyReady"] = false;
            j["taxonomyError"] = "Smart tagging engine not built";
            j["taxonomyVersion"] = "";
            j["labelCount"] = 0;
            j["indexedPhotos"] = 0;
            j["labeledPhotos"] = 0;
            j["tagAssignments"] = 0;
            j["modelId"] = "";
            sendJson(res, j);
        }
    });

    server.Post("/api/smart-tags/jobs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Smart tagging engine not built", 400);
            return;
        }

        try {
            auto body = nlohmann::json::parse(req.body);
            std::string scope = body.value("scope", "catalog");
            if (scope != "catalog" && scope != "selected") {
                sendError(res, "scope must be selected or catalog", 400);
                return;
            }
            if (scope == "catalog" && body.contains("media_ids")) {
                sendError(res, "media_ids may only be supplied with selected scope", 400);
                return;
            }
            if (body.contains("force") && !body["force"].is_boolean()) {
                sendError(res, "force must be a boolean", 400);
                return;
            }
            bool force = body.value("force", false);
            std::vector<MediaId> mediaIds;
            if (body.contains("media_ids")) {
                if (!body["media_ids"].is_array()) {
                    sendError(res, "media_ids must be an array", 400);
                    return;
                }
                std::unordered_set<MediaId> seen;
                for (const auto& el : body["media_ids"]) {
                    if (!el.is_number_integer()) {
                        sendError(res, "media_ids must contain integer IDs", 400);
                        return;
                    }
                    MediaId id = el.get<MediaId>();
                    if (id <= 0) {
                        sendError(res, "media_ids must contain positive integers", 400);
                        return;
                    }
                    if (!seen.insert(id).second) {
                        sendError(res, "media_ids cannot contain duplicates", 400);
                        return;
                    }
                    mediaIds.push_back(id);
                }
            }

            auto st = clipService_->smartTagStatus();
            if (!st.value("built", false)) {
                sendError(res, "Smart tagging engine not built in this binary", 400);
                return;
            }
            if (!st.value("ready", false)) {
                sendError(res, "CLIP model unavailable", 400);
                return;
            }
            if (!st.value("taxonomyReady", false)) {
                sendError(res, "Taxonomy error: " + st.value("taxonomyError", ""), 400);
                return;
            }
            if (st.value("indexedPhotos", 0) <= 0) {
                sendError(res, "Build the search index first (0 photos indexed)", 400);
                return;
            }

            int64_t jobId = 0;
            auto status = clipService_->startSmartTagJob(scope, mediaIds, force, jobId);
            if (status.isOk()) {
                nlohmann::json j;
                j["job_id"] = jobId;
                j["status"] = "ok";
                sendJson(res, j);
            } else {
                sendStatusError(res, status);
            }
        } catch (...) {
            sendError(res, "Invalid JSON", 400);
        }
    });

    server.Get(R"(/api/smart-tags/jobs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Smart tagging engine not built", 400);
            return;
        }
        auto jobIdOpt = parseMatchId<int64_t>(req, 1, res, "Job ID");
        if (!jobIdOpt) return;

        auto result = clipService_->getJob(*jobIdOpt, "tags");
        if (result.isOk()) {
            sendJson(res, result.value());
        } else {
            sendStatusError(res, result.status());
        }
    });

    server.Post(R"(/api/smart-tags/jobs/(\d+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Smart tagging engine not built", 400);
            return;
        }
        auto jobIdOpt = parseMatchId<int64_t>(req, 1, res, "Job ID");
        if (!jobIdOpt) return;

        auto status = clipService_->cancelJob(*jobIdOpt, "tags");
        if (status.isOk()) {
            nlohmann::json j;
            j["status"] = "ok";
            sendJson(res, j);
        } else {
            sendStatusError(res, status);
        }
    });

    server.Post("/api/smart-tags/clear", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        if (!clipService_) {
            sendError(res, "Smart tagging engine not built", 400);
            return;
        }

        auto status = clipService_->clearSmartTags();
        if (status.isOk()) {
            nlohmann::json j;
            j["status"] = "ok";
            sendJson(res, j);
        } else {
            sendStatusError(res, status);
        }
    });
}

namespace {

using db::StepResult;

inline void rankKeeperCandidateIds(std::vector<MediaId>& members, db::CatalogDb& db) {
    if (members.size() <= 1) return;
    std::vector<MediaItem> items;
    items.reserve(members.size());
    for (MediaId id : members) {
        auto r = db.getMediaById(id);
        if (r.isOk()) {
            items.push_back(r.value());
        } else {
            MediaItem dummy;
            dummy.id = id;
            items.push_back(dummy);
        }
    }

    std::stable_sort(items.begin(), items.end(), core::isBetterKeeper);

    for (size_t i = 0; i < items.size(); ++i) {
        members[i] = items[i].id;
    }
}

} // namespace

void ApiRouter::registerDuplicateRoutes(httplib::Server& server) {
    // POST /api/media/duplicates/scan
    server.Post("/api/media/duplicates/scan", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;

        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            try {
                body = nlohmann::json::parse(req.body);
            } catch (const std::exception& ex) {
                sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
                return;
            }
        }

        std::vector<MediaId> scopeIds;
        std::unordered_set<MediaId> scopeSet;
        if (body.contains("media_ids")) {
            if (!body["media_ids"].is_array()) {
                sendError(res, "media_ids must be an array of integers", 400);
                return;
            }
            if (body["media_ids"].size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            for (const auto& el : body["media_ids"]) {
                if (!el.is_number_integer() || el.get<int64_t>() <= 0) {
                    sendError(res, "media_ids must contain positive integers", 400);
                    return;
                }
                MediaId id = el.get<MediaId>();
                scopeIds.push_back(id);
                scopeSet.insert(id);
            }
        }

        double visualThreshold = 0.97;
        if (body.contains("similarity_threshold")) {
            if (!body["similarity_threshold"].is_number()) {
                sendError(res, "similarity_threshold must be a number", 400);
                return;
            }
            visualThreshold = body["similarity_threshold"].get<double>();
        } else if (body.contains("visual_threshold")) {
            if (!body["visual_threshold"].is_number()) {
                sendError(res, "visual_threshold must be a number", 400);
                return;
            }
            visualThreshold = body["visual_threshold"].get<double>();
        }
        if (!std::isfinite(visualThreshold) || visualThreshold <= 0.0 || visualThreshold > 1.0) {
            sendError(res, "similarity_threshold must be a finite number between 0.0 and 1.0", 400);
            return;
        }

        int64_t dateWindowSec = 86400;
        if (body.contains("date_window_sec")) {
            if (!body["date_window_sec"].is_number_integer() || body["date_window_sec"].get<int64_t>() < 0) {
                sendError(res, "date_window_sec must be a non-negative integer", 400);
                return;
            }
            dateWindowSec = body["date_window_sec"].get<int64_t>();
        }

        std::unordered_set<uint64_t> dismissedPairs;
        auto disRes = db().getDismissedDuplicatePairs();
        if (disRes.isOk()) {
            for (const auto& [a, b] : disRes.value()) {
                MediaId low = std::min(a, b);
                MediaId high = std::max(a, b);
                dismissedPairs.insert((static_cast<uint64_t>(low) << 32) | (static_cast<uint64_t>(high) & 0xFFFFFFFFULL));
            }
        }

        // 1. Exact duplicate pass
        std::vector<std::vector<MediaId>> exactDuplicateGroups;
        std::unordered_set<MediaId> exactMemberIds;
        auto exactRes = db().findExactDuplicateGroups(scopeIds);
        if (exactRes.isOk()) {
            for (auto& members : exactRes.value()) {
                if (members.size() < 2) continue;

                // Exclude dismissed pairs within this exact group
                std::unordered_map<MediaId, MediaId> parent;
                auto findRoot = [&](auto& self, MediaId x) -> MediaId {
                    if (!parent.count(x)) parent[x] = x;
                    if (parent[x] == x) return x;
                    return parent[x] = self(self, parent[x]);
                };
                auto unite = [&](MediaId x, MediaId y) {
                    MediaId rx = findRoot(findRoot, x);
                    MediaId ry = findRoot(findRoot, y);
                    if (rx != ry) parent[rx] = ry;
                };

                for (size_t i = 0; i < members.size(); ++i) {
                    for (size_t j = i + 1; j < members.size(); ++j) {
                        MediaId a = members[i];
                        MediaId b = members[j];
                        if (a > b) std::swap(a, b);
                        uint64_t packed = (static_cast<uint64_t>(a) << 32) | (static_cast<uint64_t>(b) & 0xFFFFFFFFULL);
                        if (!dismissedPairs.count(packed)) {
                            unite(a, b);
                        }
                    }
                }

                std::unordered_map<MediaId, std::vector<MediaId>> clusters;
                for (MediaId m : members) {
                    if (parent.count(m)) {
                        clusters[findRoot(findRoot, m)].push_back(m);
                    }
                }

                for (auto& [root, clusterMembers] : clusters) {
                    if (clusterMembers.size() >= 2) {
                        rankKeeperCandidateIds(clusterMembers, db());
                        for (MediaId mid : clusterMembers) {
                            exactMemberIds.insert(mid);
                        }
                        exactDuplicateGroups.push_back(std::move(clusterMembers));
                    }
                }
            }
        }

        // Sort exact groups largest-first
        std::sort(exactDuplicateGroups.begin(), exactDuplicateGroups.end(),
                  [](const std::vector<MediaId>& a, const std::vector<MediaId>& b) {
                      return a.size() > b.size();
                  });

        // 2. Visual duplicate pass
        std::vector<clip::VisualDuplicateGroup> visualGroups;
        if (clipService_) {
            visualGroups = clipService_->scanVisualDuplicates(
                static_cast<float>(visualThreshold),
                dateWindowSec,
                scopeSet,
                exactMemberIds
            );
        }

        // Compute media scanned
        int mediaScanned = 0;
        if (!scopeSet.empty()) {
            mediaScanned = static_cast<int>(scopeSet.size());
        } else {
            auto countRes = db().countMedia("", {});
            if (countRes.isOk()) {
                mediaScanned = static_cast<int>(countRes.value());
            }
        }

        // Hydration: batch load metadata and tags
        std::vector<MediaId> allMemberIds;
        for (const auto& eg : exactDuplicateGroups) {
            allMemberIds.insert(allMemberIds.end(), eg.begin(), eg.end());
        }
        for (const auto& vg : visualGroups) {
            allMemberIds.insert(allMemberIds.end(), vg.memberIds.begin(), vg.memberIds.end());
        }

        std::unordered_map<MediaId, std::vector<Tag>> batchTags;
        if (!allMemberIds.empty()) {
            auto batchRes = db().getTagsForMediaBatch(allMemberIds);
            if (batchRes.isOk()) {
                batchTags = std::move(batchRes.value());
            }
        }

        std::unordered_map<MediaId, MediaItem> mediaMap;
        if (!allMemberIds.empty()) {
            std::unordered_set<MediaId> uniqueIds(allMemberIds.begin(), allMemberIds.end());
            std::vector<MediaId> deduplicatedIds(uniqueIds.begin(), uniqueIds.end());
            constexpr size_t kBatchChunkSize = 500;
            for (size_t chunkStart = 0; chunkStart < deduplicatedIds.size(); chunkStart += kBatchChunkSize) {
                size_t chunkEnd = std::min(chunkStart + kBatchChunkSize, deduplicatedIds.size());
                size_t count = chunkEnd - chunkStart;
                std::string whereClause = "id IN (";
                std::vector<std::string> params;
                params.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    if (i > 0) whereClause += ",";
                    whereClause += "?";
                    params.push_back(std::to_string(deduplicatedIds[chunkStart + i]));
                }
                whereClause += ")";
                auto itemsRes = db().queryMedia(whereClause, params, "", 0, 0);
                if (itemsRes.isOk()) {
                    for (auto& item : itemsRes.value()) {
                        auto mid = item.id;
                        auto it = batchTags.find(mid);
                        if (it != batchTags.end()) {
                            item.tags = it->second;
                        }
                        mediaMap[mid] = std::move(item);
                    }
                }
            }
        }

        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& eg : exactDuplicateGroups) {
            nlohmann::json g;
            g["type"] = "exact";
            g["keeper"] = eg.empty() ? 0 : eg[0];
            g["members"] = eg;
            nlohmann::json itemsJson = nlohmann::json::array();
            for (MediaId mid : eg) {
                if (mediaMap.count(mid)) itemsJson.push_back(mediaMap[mid]);
            }
            g["items"] = itemsJson;
            g["photos"] = std::move(itemsJson);
            g["min_similarity"] = 1.0;
            groupsJson.push_back(std::move(g));
        }

        for (const auto& vg : visualGroups) {
            nlohmann::json g;
            g["type"] = "visual";
            g["keeper"] = vg.memberIds.empty() ? 0 : vg.memberIds[0];
            g["members"] = vg.memberIds;
            nlohmann::json itemsJson = nlohmann::json::array();
            for (MediaId mid : vg.memberIds) {
                if (mediaMap.count(mid)) itemsJson.push_back(mediaMap[mid]);
            }
            g["items"] = itemsJson;
            g["photos"] = std::move(itemsJson);
            g["min_similarity"] = std::round(vg.minSimilarity * 1000.0) / 1000.0;
            groupsJson.push_back(std::move(g));
        }

        nlohmann::json responseJson = {
            {"groups", std::move(groupsJson)},
            {"media_scanned", mediaScanned}
        };
        sendJson(res, responseJson);
    });

    // POST /api/media/duplicates/resolve
    server.Post("/api/media/duplicates/resolve", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
            return;
        }

        if (!body.contains("groups") || !body["groups"].is_array()) {
            sendError(res, "Missing or invalid 'groups' array", 400);
            return;
        }

        if (body["groups"].size() > kMaxBatchSize) {
            sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
            return;
        }

        size_t totalRemoveItems = 0;
        for (const auto& g : body["groups"]) {
            if (g.contains("remove") && g["remove"].is_array()) {
                totalRemoveItems += g["remove"].size();
            }
        }
        if (totalRemoveItems > kMaxBatchSize) {
            sendError(res, "Total remove items exceed maximum limit of " + std::to_string(kMaxBatchSize), 400);
            return;
        }

        bool globalDeleteFromDisk = false;
        if (body.contains("delete_from_disk") && body["delete_from_disk"].is_boolean()) {
            globalDeleteFromDisk = body["delete_from_disk"].get<bool>();
        } else if (req.has_param("delete_from_disk")) {
            std::string val = req.get_param_value("delete_from_disk");
            globalDeleteFromDisk = (val == "true" || val == "1");
        }

        bool globalMergeMetadata = true;
        if (body.contains("merge_metadata") && body["merge_metadata"].is_boolean()) {
            globalMergeMetadata = body["merge_metadata"].get<bool>();
        }

        int totalMergedCount = 0;
        int totalDeletedCount = 0;
        int64_t totalFreedBytes = 0;
        nlohmann::json groupResults = nlohmann::json::array();

        for (const auto& groupEl : body["groups"]) {
            if (!groupEl.is_object() || !groupEl.contains("keeper") || !groupEl.contains("remove")) {
                groupResults.push_back({{"status", 400}, {"error", "Invalid group structure"}});
                continue;
            }
            if (!groupEl["keeper"].is_number_integer() || groupEl["keeper"].get<int64_t>() <= 0) {
                groupResults.push_back({{"status", 400}, {"error", "Invalid keeper media ID"}});
                continue;
            }
            if (!groupEl["remove"].is_array()) {
                groupResults.push_back({{"status", 400}, {"error", "Invalid remove list"}});
                continue;
            }

            bool groupDeleteFromDisk = globalDeleteFromDisk;
            if (groupEl.contains("delete_from_disk") && groupEl["delete_from_disk"].is_boolean()) {
                groupDeleteFromDisk = groupEl["delete_from_disk"].get<bool>();
            }

            bool groupMergeMetadata = globalMergeMetadata;
            if (groupEl.contains("merge_metadata") && groupEl["merge_metadata"].is_boolean()) {
                groupMergeMetadata = groupEl["merge_metadata"].get<bool>();
            }

            MediaId keeperId = groupEl["keeper"].get<MediaId>();
            const auto& removeList = groupEl["remove"];

            auto keeperRes = catalog_ ? catalog_->getMedia(keeperId) : db().getMediaById(keeperId);
            if (!keeperRes.isOk()) {
                groupResults.push_back({{"keeper", keeperId}, {"status", 404}, {"error", "Keeper media not found"}});
                continue;
            }

            size_t keeperTagsBefore = 0;
            if (auto tr = db().getTagsForMedia(keeperId); tr.isOk()) {
                keeperTagsBefore = tr.value().size();
            }

            int64_t groupFreedBytes = 0;
            std::vector<MediaId> groupRemoved;
            std::vector<nlohmann::json> groupItemErrors;

            Status txStatus = db().runInTransaction([&]() -> Status {
                for (const auto& removeEl : removeList) {
                    if (!removeEl.is_number_integer() || removeEl.get<int64_t>() <= 0) {
                        groupItemErrors.push_back({{"id", 0}, {"status", 400}, {"error", "Invalid remove media ID"}});
                        continue;
                    }
                    MediaId loserId = removeEl.get<MediaId>();
                    if (loserId == keeperId) {
                        groupItemErrors.push_back({{"id", loserId}, {"status", 400}, {"error", "Cannot remove keeper"}});
                        continue;
                    }
                    auto loserRes = catalog_ ? catalog_->getMedia(loserId) : db().getMediaById(loserId);
                    if (!loserRes.isOk()) {
                        groupItemErrors.push_back({{"id", loserId}, {"status", 404}, {"error", "Remove media not found"}});
                        continue;
                    }

                    MediaItem loser = loserRes.value();
                    if (groupMergeMetadata) {
                        Status mergeStatus = db().mergeMediaMetadata(keeperId, loserId);
                        if (!mergeStatus.isOk()) {
                            return mergeStatus;
                        }
                    }

                    Status delStatus = catalog_ ? catalog_->deleteMedia(loserId, groupDeleteFromDisk)
                                                : (groupDeleteFromDisk ? Status::internal("Catalog is required to delete files from disk") : db().deleteMedia(loserId));
                    if (!delStatus.isOk()) {
                        return delStatus;
                    }

                    groupFreedBytes += loser.file_size;
                    groupRemoved.push_back(loserId);
                }
                return Status::ok();
            });

            if (txStatus.isOk()) {
                if (clipService_) {
                    for (MediaId loserId : groupRemoved) {
                        clipService_->removeMedia(loserId);
                    }
                }
                totalMergedCount += 1;
                totalDeletedCount += static_cast<int>(groupRemoved.size());
                totalFreedBytes += groupFreedBytes;

                size_t keeperTagsAfter = 0;
                if (auto tr = db().getTagsForMedia(keeperId); tr.isOk()) {
                    keeperTagsAfter = tr.value().size();
                }
                int groupMergedTags = static_cast<int>(keeperTagsAfter > keeperTagsBefore ? keeperTagsAfter - keeperTagsBefore : 0);

                nlohmann::json gr = {
                    {"keeper", keeperId},
                    {"removed", groupRemoved},
                    {"merged_tags", groupMergedTags},
                    {"freed_bytes", groupFreedBytes},
                    {"status", 200}
                };
                if (!groupItemErrors.empty()) {
                    gr["errors"] = groupItemErrors;
                }
                groupResults.push_back(std::move(gr));
            } else {
                groupResults.push_back({
                    {"keeper", keeperId},
                    {"status", 500},
                    {"error", "Failed to resolve group: " + txStatus.message()}
                });
            }
        }

        nlohmann::json resJson = {
            {"status", "ok"},
            {"merged_count", totalMergedCount},
            {"deleted_count", totalDeletedCount},
            {"freed_bytes", totalFreedBytes},
            {"merged", totalMergedCount},
            {"deleted", totalDeletedCount},
            {"groups", std::move(groupResults)}
        };
        sendJson(res, resJson);
    });

    // POST /api/media/duplicates/dismiss
    server.Post("/api/media/duplicates/dismiss", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what(), 400);
            return;
        }

        if (!body.contains("pairs") || !body["pairs"].is_array()) {
            sendError(res, "Missing or invalid 'pairs' array", 400);
            return;
        }

        if (body["pairs"].size() > kMaxBatchSize) {
            sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
            return;
        }

        int dismissedCount = 0;
        for (const auto& pairEl : body["pairs"]) {
            if (!pairEl.is_array() || pairEl.size() != 2) continue;
            if (!pairEl[0].is_number_integer() || !pairEl[1].is_number_integer()) continue;
            MediaId a = pairEl[0].get<MediaId>();
            MediaId b = pairEl[1].get<MediaId>();
            if (a <= 0 || b <= 0 || a == b) continue;
            if (db().dismissDuplicatePair(a, b).isOk()) {
                dismissedCount++;
            }
        }

        sendJson(res, {{"status", "ok"}, {"dismissed", dismissedCount}});
    });

    // POST /api/media/duplicates/dismissals/reset
    server.Post("/api/media/duplicates/dismissals/reset", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        auto resetRes = db().clearDismissedDuplicatePairs();
        if (!resetRes.isOk()) {
            sendError(res, "Failed to reset duplicate dismissals: " + resetRes.status().message(), 500);
            return;
        }
        sendJson(res, {{"status", "ok"}, {"cleared", resetRes.value()}});
    });
}

} // namespace imagine::server
