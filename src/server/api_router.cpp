#include "imagine/server/api_router.hpp"
#include "imagine/server/mime_types.hpp"
#include "imagine/core/catalog.hpp"
#include "imagine/core/query.hpp"
#include "imagine/core/importer.hpp"
#include "imagine/metadata/hasher.hpp"
#include "imagine/thumbnail/generator.hpp"
#include "imagine/common/logger.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <cstdlib>
#include <cctype>

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

constexpr size_t kMaxBatchSize = 1000;
constexpr size_t kMaxSearchLength = 500;
constexpr size_t kMaxFolderLength = 1000;
constexpr size_t kMaxNameLength = 100;
constexpr size_t kMaxCategoryLength = 50;
constexpr size_t kMaxAlbumNameLength = 200;
constexpr size_t kMaxAlbumDescLength = 2000;

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
    nlohmann::json err = {{"error", sanitizeUtf8(message)}};
    res.status = status;
    try {
        res.set_content(err.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    } catch (...) {
        res.set_content(R"({"error":"Error"})", "application/json");
    }
}

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
    return false;
}

bool isWithinAllowedRoots(const std::filesystem::path& canonicalPath, std::string& outError) {
    if (isSensitiveSystemPath(canonicalPath)) {
        outError = "Importing sensitive system directory is forbidden: " + canonicalPath.string();
        return false;
    }

    const char* envRoots = std::getenv("IMAGINE_ALLOWED_IMPORT_ROOTS");
    if (envRoots && std::strlen(envRoots) > 0) {
        std::string envStr(envRoots);
        std::stringstream ss(envStr);
        std::string root;
        bool matched = false;
        while (std::getline(ss, root, ':')) {
            if (root.empty()) continue;
            std::error_code ec;
            auto cRoot = std::filesystem::canonical(root, ec);
            if (!ec) {
                std::string cRootStr = cRoot.lexically_normal().string();
                std::string cPathStr = canonicalPath.lexically_normal().string();
                if (cPathStr == cRootStr || (cPathStr.rfind(cRootStr + "/", 0) == 0)) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            outError = "Directory is not within allowed import roots: " + canonicalPath.string();
            return false;
        }
    }
    return true;
}

// Fallback progress state if catalog is not used
static std::mutex g_importMutex;
static ImportProgress g_fallbackProgress;
static std::atomic<bool> g_isImporting{false};

} // anonymous namespace

ApiRouter::ApiRouter(core::Catalog& catalog)
    : catalog_(&catalog) {
    const char* envToken = std::getenv("IMAGINE_API_TOKEN");
    if (envToken && *envToken) {
        apiToken_ = envToken;
    }
    const char* envOrigin = std::getenv("IMAGINE_ALLOWED_ORIGIN");
    if (envOrigin && *envOrigin) {
        allowedOrigin_ = envOrigin;
    }
}

ApiRouter::ApiRouter(db::CatalogDb& db, thumbnail::Cache& cache)
    : db_(&db), cache_(&cache) {
    const char* envToken = std::getenv("IMAGINE_API_TOKEN");
    if (envToken && *envToken) {
        apiToken_ = envToken;
    }
    const char* envOrigin = std::getenv("IMAGINE_ALLOWED_ORIGIN");
    if (envOrigin && *envOrigin) {
        allowedOrigin_ = envOrigin;
    }
}

ApiRouter::~ApiRouter() {
    stopImport();
}

void ApiRouter::setApiToken(std::string token) {
    apiToken_ = std::move(token);
}

void ApiRouter::setAllowedOrigin(std::string origin) {
    allowedOrigin_ = std::move(origin);
}

void ApiRouter::stopImport() {
    if (catalog_) {
        catalog_->cancelImport();
    }
    cancelFallbackImport_.store(true);
    std::lock_guard<std::mutex> lock(importThreadMutex_);
    if (importThread_.joinable()) {
        importThread_.request_stop();
        importThread_.join();
    }
}

bool ApiRouter::isImportRunning() const {
    if (catalog_) {
        return catalog_->importer().isRunning();
    }
    return g_isImporting.load();
}

bool ApiRouter::checkAuth(const httplib::Request& req, httplib::Response& res) const {
    if (apiToken_.empty()) {
        return true;
    }
    if (req.has_header("Authorization")) {
        auto auth = req.get_header_value("Authorization");
        if (auth == ("Bearer " + apiToken_)) return true;
    }
    if (req.has_header("X-API-Key")) {
        auto key = req.get_header_value("X-API-Key");
        if (key == apiToken_) return true;
    }
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

void ApiRouter::registerRoutes(httplib::Server& server) {
    registerCorsHandler(server);
    registerMediaRoutes(server);
    registerTagRoutes(server);
    registerAlbumRoutes(server);
    registerThumbnailRoutes(server);
    registerTimelineRoutes(server);
    registerStatsRoutes(server);
    registerFolderRoutes(server);
    registerImportRoutes(server);
    registerGeocodeRoutes(server);
}

void ApiRouter::registerCorsHandler(httplib::Server& server) {
    server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        std::string requestId;
        if (req.has_header("X-Request-ID")) {
            requestId = req.get_header_value("X-Request-ID");
        } else {
            requestId = "req-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        }
        res.set_header("X-Request-ID", requestId);

        std::string origin = allowedOrigin_;
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

void ApiRouter::registerMediaRoutes(httplib::Server& server) {
    // GET /api/media
    server.Get("/api/media", [this](const httplib::Request& req, httplib::Response& res) {
        core::QueryCriteria criteria;

        auto parseInt = [](const std::string& str, auto& out) -> bool {
            try {
                size_t idx = 0;
                auto v = std::stoll(str, &idx);
                if (idx != str.size()) return false;
                out = static_cast<std::decay_t<decltype(out)>>(v);
                return true;
            } catch (...) {
                return false;
            }
        };

        auto parseDouble = [](const std::string& str, double& out) -> bool {
            try {
                size_t idx = 0;
                double v = std::stod(str, &idx);
                if (idx != str.size()) return false;
                out = v;
                return true;
            } catch (...) {
                return false;
            }
        };

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
        if (req.has_param("media_type")) {
            std::string mt = req.get_param_value("media_type");
            if (mt != "photo" && mt != "video" && mt != "audio") {
                sendError(res, "Query parameter 'media_type' must be 'photo', 'video', or 'audio'", 400);
                return;
            }
            criteria.media_type = std::move(mt);
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
            std::string folder = req.get_param_value("folder");
            if (folder.size() > kMaxFolderLength) {
                sendError(res, "Folder path exceeds maximum length", 400);
                return;
            }
            criteria.folder = std::move(folder);
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
            TagId tid = 0;
            if (!parseInt(req.get_param_value("tag_id"), tid)) {
                sendError(res, "Query parameter 'tag_id' must be an integer", 400);
                return;
            }
            criteria.tag_ids.push_back(tid);
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
        if (criteria.date_from > 0 && criteria.date_to > 0 && criteria.date_from > criteria.date_to) {
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
                "date_taken", "rating", "file_name", "file_size", "duration"
            };
            if (allowedSortCols.find(col) == allowedSortCols.end()) {
                sendError(res, "Invalid sort column: " + col, 400);
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
            std::stringstream ss(bbox);
            std::string sMinLon, sMinLat, sMaxLon, sMaxLat;
            if (std::getline(ss, sMinLon, ',') && std::getline(ss, sMinLat, ',') &&
                std::getline(ss, sMaxLon, ',') && std::getline(ss, sMaxLat, ',')) {
                double minLon = 0.0, minLat = 0.0, maxLon = 0.0, maxLat = 0.0;
                if (!parseDouble(sMinLon, minLon) || !parseDouble(sMinLat, minLat) ||
                    !parseDouble(sMaxLon, maxLon) || !parseDouble(sMaxLat, maxLat) ||
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
        MediaId id = std::stoll(req.matches[1]);
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
        MediaId id = std::stoll(req.matches[1]);
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
        MediaId id = std::stoll(req.matches[1]);
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
        MediaId id = std::stoll(req.matches[1]);
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

    // POST /api/media/:id/gps
    server.Post(R"(/api/media/(\d+)/gps)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        MediaId id = std::stoll(req.matches[1]);
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
                if (latitude < -90.0 || latitude > 90.0) {
                    sendError(res, "Latitude must be between -90 and 90");
                    return;
                }
                if (longitude < -180.0 || longitude > 180.0) {
                    sendError(res, "Longitude must be between -180 and 180");
                    return;
                }
                if (body.contains("altitude") && body["altitude"].is_number()) {
                    altitude = body["altitude"].get<double>();
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
        MediaId id = std::stoll(req.matches[1]);
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
        MediaId id = std::stoll(req.matches[1]);
        TagId tagId = std::stoll(req.matches[2]);

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
        MediaId id = std::stoll(req.matches[1]);
        auto mediaRes = catalog_ ? catalog_->getMedia(id) : db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendStatusError(res, mediaRes.status());
            return;
        }

        bool deleteFromDisk = false;
        if (req.has_param("delete_from_disk")) {
            std::string val = req.get_param_value("delete_from_disk");
            deleteFromDisk = (val == "true" || val == "1");
        }

        std::string filePath;
        if (!catalog_ && deleteFromDisk) {
            filePath = resolvePhotoPath(mediaRes.value().file_path);
        }

        Status s = catalog_ ? catalog_->deleteMedia(id, deleteFromDisk) : db().deleteMedia(id);
        if (!s.isOk()) {
            sendStatusError(res, s);
            return;
        }

        if (!catalog_ && deleteFromDisk && !filePath.empty()) {
            std::error_code ec;
            if (!std::filesystem::remove(pathFromUtf8(filePath), ec)) {
                if (ec) {
                    IMAGINE_LOG_WARN("Failed to delete file from disk: " + filePath + " (" + ec.message() + ")");
                }
            }
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
            bool deleteFromDisk = false;
            if (body.contains("delete_from_disk") && body["delete_from_disk"].is_boolean()) {
                deleteFromDisk = body["delete_from_disk"].get<bool>();
            } else if (req.has_param("delete_from_disk")) {
                std::string val = req.get_param_value("delete_from_disk");
                deleteFromDisk = (val == "true" || val == "1");
            }
            std::vector<MediaId> ids = body["ids"].get<std::vector<MediaId>>();
            if (ids.size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            int deletedCount = 0;
            std::vector<MediaId> failedIds;
            for (MediaId id : ids) {
                std::string filePath;
                if (!catalog_ && deleteFromDisk) {
                    auto itemRes = db().getMediaById(id);
                    if (itemRes.isOk()) {
                        filePath = resolvePhotoPath(itemRes.value().file_path);
                    }
                }
                Status s = catalog_ ? catalog_->deleteMedia(id, deleteFromDisk) : db().deleteMedia(id);
                if (s.isOk()) {
                    if (!catalog_ && deleteFromDisk && !filePath.empty()) {
                        std::error_code ec;
                        if (!std::filesystem::remove(pathFromUtf8(filePath), ec)) {
                            if (ec) {
                                IMAGINE_LOG_WARN("Failed to delete file from disk: " + filePath + " (" + ec.message() + ")");
                            }
                        }
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
            if (ids.size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            int updatedCount = 0;
            std::vector<MediaId> failedIds;
            for (MediaId id : ids) {
                Status s = catalog_ ? catalog_->setRating(id, rating) : db().updateRating(id, rating);
                if (s.isOk()) {
                    updatedCount++;
                } else {
                    failedIds.push_back(id);
                }
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
            if (ids.size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }
            int updatedCount = 0;
            std::vector<MediaId> failedIds;
            for (MediaId id : ids) {
                Status s = catalog_ ? catalog_->setFlag(id, flag) : db().updateFlag(id, flag);
                if (s.isOk()) {
                    updatedCount++;
                } else {
                    failedIds.push_back(id);
                }
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
            if (body.contains("ids") && body["ids"].is_array()) {
                ids = body["ids"].get<std::vector<MediaId>>();
            } else if (body.contains("media_ids") && body["media_ids"].is_array()) {
                ids = body["media_ids"].get<std::vector<MediaId>>();
            } else {
                sendError(res, "Missing or invalid 'ids' or 'media_ids' array");
                return;
            }

            if (ids.size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }

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
            for (MediaId id : ids) {
                Status s = catalog_ ? catalog_->addTag(id, tagId) : db().addTagToMedia(id, tagId);
                if (s.isOk()) {
                    taggedCount++;
                } else {
                    failedIds.push_back(id);
                }
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

    // POST /api/media/batch-gps
    server.Post("/api/media/batch-gps", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<MediaId> ids;
            if (body.contains("ids") && body["ids"].is_array()) {
                ids = body["ids"].get<std::vector<MediaId>>();
            } else {
                sendError(res, "Missing or invalid 'ids' array");
                return;
            }

            if (ids.size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }

            bool hasGps = body.value("has_gps", true);
            double latitude = body.value("latitude", 0.0);
            double longitude = body.value("longitude", 0.0);
            double altitude = body.value("altitude", 0.0);

            if (hasGps) {
                if (latitude < -90.0 || latitude > 90.0) {
                    sendError(res, "Latitude must be between -90 and 90");
                    return;
                }
                if (longitude < -180.0 || longitude > 180.0) {
                    sendError(res, "Longitude must be between -180 and 180");
                    return;
                }
            }

            int updatedCount = 0;
            std::vector<MediaId> failedIds;
            for (MediaId id : ids) {
                Status s = catalog_
                    ? catalog_->setGps(id, hasGps, latitude, longitude, altitude)
                    : db().updateGps(id, hasGps, latitude, longitude, altitude);
                if (s.isOk()) {
                    updatedCount++;
                } else {
                    failedIds.push_back(id);
                }
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
}

void ApiRouter::registerThumbnailRoutes(httplib::Server& server) {
    // GET /api/thumbnails/:hash/:size
    server.Get(R"(/api/thumbnails/([a-zA-Z0-9_-]+)/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string hash = req.matches[1];
        int size = 0;
        try {
            size = std::stoi(req.matches[2]);
        } catch (...) {
            sendError(res, "Invalid thumbnail size", 400);
            return;
        }

        if (size <= 0 || size > 2048) {
            sendError(res, "Thumbnail size must be between 1 and 2048", 400);
            return;
        }

        std::string thumbPath = cache().getThumbnailPath(hash, size);
        std::error_code ec;
        if (!std::filesystem::exists(thumbPath, ec)) {
            // Attempt on-demand generation
            auto mediaRes = db().getMediaByHash(hash);
            if (mediaRes.isOk()) {
                const auto& item = mediaRes.value();
                if (item.media_type == "video" || item.media_type == "audio") {
                    std::string label;
                    if (item.media_type == "audio" && !item.audio_artist.empty()) {
                        label = item.audio_artist + (!item.audio_title.empty() ? " - " + item.audio_title : "");
                    }
                    auto procRes = cache().ensureProceduralThumbnails(hash, item.media_type, label);
                    if (procRes.isOk()) {
                        thumbPath = cache().getThumbnailPath(hash, size);
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

        if (std::filesystem::exists(thumbPath, ec) && std::filesystem::is_regular_file(thumbPath, ec)) {
            auto fsize = std::filesystem::file_size(thumbPath, ec);
            auto mtime = std::filesystem::last_write_time(thumbPath, ec);
            int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
            std::string etag = "\"thumb-v1-" + hash + "_" + std::to_string(size) + "-" + std::to_string(fsize) + "-" + std::to_string(mtimeSec) + "\"";

            res.set_header("Cache-Control", "public, max-age=31536000, immutable");
            res.set_header("ETag", etag);

            if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag) {
                res.status = 304;
                return;
            }

            res.set_file_content(thumbPath, "image/jpeg");
        } else {
            sendError(res, "Thumbnail not found", 404);
        }
    });

    // GET /api/photos/:id/original (and /api/media/:id/file, /api/media/:id/original)
    auto handleServeFile = [this](const httplib::Request& req, httplib::Response& res) {
        MediaId id = 0;
        try {
            id = std::stoll(req.matches[1]);
        } catch (...) {
            sendError(res, "Invalid media ID", 400);
            return;
        }
        auto mediaRes = db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendError(res, "Media not found", 404);
            return;
        }

        const auto& item = mediaRes.value();
        std::string photoPath = resolvePhotoPath(item.file_path);
        std::error_code ec;
        if (!std::filesystem::exists(photoPath, ec) || !std::filesystem::is_regular_file(photoPath, ec)) {
            sendError(res, "File not found on disk: " + photoPath, 404);
            return;
        }

        auto fsize = std::filesystem::file_size(photoPath, ec);
        auto mtime = std::filesystem::last_write_time(photoPath, ec);
        int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
        std::string etag = "\"" + std::to_string(id) + "-" + std::to_string(fsize) + "-" + std::to_string(mtimeSec) + "\"";

        res.set_header("Accept-Ranges", "bytes");
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "public, max-age=86400");

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
        MediaId id = 0;
        try {
            id = std::stoll(req.matches[1]);
        } catch (...) {
            sendError(res, "Invalid media ID", 400);
            return;
        }

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
        MediaId id = std::stoll(req.matches[1]);
        auto mediaRes = db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendError(res, "Media not found", 404);
            return;
        }

        auto item = mediaRes.value();
        if (item.media_type == "video" || item.media_type == "audio" || item.media_type != "photo") {
            sendError(res, "Quick edit is not supported for " + item.media_type + " files", 400);
            return;
        }

        std::string photoPath = resolvePhotoPath(item.file_path);
        std::error_code ec;
        if (!std::filesystem::exists(photoPath, ec) || !std::filesystem::is_regular_file(photoPath, ec)) {
            sendError(res, "Source photo not found on disk: " + photoPath, 404);
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
                    std::string tmpOut = photoPath + ".proc.tmp";
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
            std::string tmpPath = photoPath + ".edit.tmp";
            {
                std::ofstream ofs(tmpPath, std::ios::binary);
                if (!ofs) {
                    sendError(res, "Failed to create temporary file for saving", 500);
                    return;
                }
                ofs.write(reinterpret_cast<const char*>(newImageBytes.data()), newImageBytes.size());
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
            std::string newHash = hashRes.value();
            auto fsize = std::filesystem::file_size(photoPath, ec);
            auto mtime = std::filesystem::last_write_time(photoPath, ec);
            int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();

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
        TagId tagId = std::stoll(req.matches[1]);
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
        AlbumId albumId = std::stoll(req.matches[1]);
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<MediaId> mediaIds;
            if (body.contains("media_id") && body["media_id"].is_number()) {
                mediaIds.push_back(body["media_id"].get<MediaId>());
            } else if (body.contains("media_ids") && body["media_ids"].is_array()) {
                for (const auto& mid : body["media_ids"]) {
                    if (mid.is_number()) {
                        mediaIds.push_back(mid.get<MediaId>());
                    }
                }
            } else {
                sendError(res, "Missing media_id or media_ids array");
                return;
            }

            if (mediaIds.size() > kMaxBatchSize) {
                sendError(res, "Batch size exceeds maximum limit of " + std::to_string(kMaxBatchSize) + " items", 400);
                return;
            }

            for (MediaId mid : mediaIds) {
                Status s = catalog_ ? catalog_->addMediaToAlbum(albumId, mid) : db().addMediaToAlbum(albumId, mid);
                if (!s.isOk()) {
                    sendStatusError(res, s);
                    return;
                }
            }

            sendJson(res, {{"status", "ok"}, {"album_id", albumId}, {"added_count", mediaIds.size()}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/albums/:id
    server.Delete(R"(/api/albums/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        AlbumId albumId = std::stoll(req.matches[1]);
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

            if (isImportRunning()) {
                sendError(res, "An import is already running", 409);
                return;
            }

            std::lock_guard<std::mutex> lock(importThreadMutex_);
            if (importThread_.joinable()) {
                importThread_.join();
            }

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
                });
            } else {
                g_isImporting.store(true);
                cancelFallbackImport_.store(false);

                importThread_ = std::jthread([this, path, recursive](std::stop_token stopToken) {
                    IMAGINE_LOG_INFO("Starting fallback background import: " + path);
                    core::Importer imp(db(), cache());
                    auto res = imp.importDirectory(path, recursive, [this, &imp, stopToken](const ImportProgress& p) {
                        if (stopToken.stop_requested() || cancelFallbackImport_.load()) {
                            imp.cancel();
                        }
                        std::lock_guard<std::mutex> lock(g_importMutex);
                        g_fallbackProgress = p;
                    });
                    {
                        std::lock_guard<std::mutex> lock(g_importMutex);
                        if (res.isOk()) {
                            g_fallbackProgress = res.value();
                        }
                        g_isImporting.store(false);
                    }
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
                std::lock_guard<std::mutex> lock(g_importMutex);
                sendJson(res, g_fallbackProgress);
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
static std::mutex g_geocodeMutex;
static std::unordered_map<std::string, GeocodeCacheEntry> g_geocodeCache;
static std::chrono::steady_clock::time_point g_lastGeocodeRequestTime{};
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
            std::lock_guard<std::mutex> lock(g_geocodeMutex);
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
            // Respect Nominatim rate limit: max 1 request per second
            {
                std::unique_lock<std::mutex> lock(g_geocodeMutex);
                auto now = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastGeocodeRequestTime).count();
                if (elapsedMs < 1000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsedMs));
                }
                g_lastGeocodeRequestTime = std::chrono::steady_clock::now();
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
                    std::lock_guard<std::mutex> lock(g_geocodeMutex);
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

} // namespace imagine::server
