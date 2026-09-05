#include "imagine/server/api_router.hpp"
#include "imagine/core/catalog.hpp"
#include "imagine/core/query.hpp"
#include "imagine/core/importer.hpp"
#include "imagine/common/logger.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>

namespace imagine::server {

namespace {

void sendJson(httplib::Response& res, const nlohmann::json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, const std::string& message, int status = 400) {
    nlohmann::json err = {{"error", message}};
    res.status = status;
    res.set_content(err.dump(), "application/json");
}

std::string getMimeTypeForImage(const std::string& path) {
    std::string ext;
    auto dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
    }
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".webp") return "image/webp";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}


// Fallback progress state if catalog is not used
static std::mutex g_importMutex;
static ImportProgress g_fallbackProgress;
static std::atomic<bool> g_isImporting{false};

} // anonymous namespace

ApiRouter::ApiRouter(core::Catalog& catalog)
    : catalog_(&catalog) {}

ApiRouter::ApiRouter(db::CatalogDb& db, thumbnail::Cache& cache)
    : db_(&db), cache_(&cache) {}

ApiRouter::~ApiRouter() = default;

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

void ApiRouter::registerRoutes(httplib::Server& server) {
    registerCorsHandler(server);
    registerMediaRoutes(server);
    registerTagRoutes(server);
    registerAlbumRoutes(server);
    registerThumbnailRoutes(server);
    registerTimelineRoutes(server);
    registerStatsRoutes(server);
    registerImportRoutes(server);
}

void ApiRouter::registerCorsHandler(httplib::Server& server) {
    server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Origin, X-Requested-With");
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

        if (req.has_param("rating")) {
            try {
                criteria.min_rating = std::stoi(req.get_param_value("rating"));
            } catch (...) {}
        }
        if (req.has_param("max_rating")) {
            try {
                criteria.max_rating = std::stoi(req.get_param_value("max_rating"));
            } catch (...) {}
        }
        if (req.has_param("flag")) {
            try {
                criteria.flag = static_cast<FlagState>(std::stoi(req.get_param_value("flag")));
            } catch (...) {}
        }
        if (req.has_param("search")) {
            criteria.search_text = req.get_param_value("search");
        }
        if (req.has_param("folder")) {
            criteria.folder = req.get_param_value("folder");
        }
        if (req.has_param("tag_category")) {
            criteria.tag_category = req.get_param_value("tag_category");
        } else if (req.has_param("category")) {
            criteria.tag_category = req.get_param_value("category");
        }
        if (req.has_param("tag_id")) {
            try {
                criteria.tag_ids.push_back(std::stoll(req.get_param_value("tag_id")));
            } catch (...) {}
        }
        if (req.has_param("album_id")) {
            try {
                criteria.album_id = std::stoll(req.get_param_value("album_id"));
            } catch (...) {}
        }
        if (req.has_param("date_from")) {
            try {
                criteria.date_from = std::stoll(req.get_param_value("date_from"));
            } catch (...) {}
        }
        if (req.has_param("date_to")) {
            try {
                criteria.date_to = std::stoll(req.get_param_value("date_to"));
            } catch (...) {}
        }
        if (req.has_param("limit")) {
            try {
                criteria.limit = std::stoi(req.get_param_value("limit"));
            } catch (...) {}
        }
        if (req.has_param("offset")) {
            try {
                criteria.offset = std::stoi(req.get_param_value("offset"));
            } catch (...) {}
        }
        if (req.has_param("sort")) {
            std::string sort = req.get_param_value("sort");
            auto dash = sort.find('-');
            if (dash != std::string::npos) {
                criteria.sort_by = sort.substr(0, dash);
                criteria.sort_descending = (sort.substr(dash + 1) != "asc");
            } else {
                criteria.sort_by = sort;
                criteria.sort_descending = true;
            }
        }
        if (req.has_param("has_gps")) {
            std::string val = req.get_param_value("has_gps");
            criteria.has_gps = (val == "1" || val == "true");
        }
        if (req.has_param("min_lat")) {
            try { criteria.min_lat = std::stod(req.get_param_value("min_lat")); } catch (...) {}
        }
        if (req.has_param("max_lat")) {
            try { criteria.max_lat = std::stod(req.get_param_value("max_lat")); } catch (...) {}
        }
        if (req.has_param("min_lon")) {
            try { criteria.min_lon = std::stod(req.get_param_value("min_lon")); } catch (...) {}
        }
        if (req.has_param("max_lon")) {
            try { criteria.max_lon = std::stod(req.get_param_value("max_lon")); } catch (...) {}
        }
        if (req.has_param("bbox")) {
            std::string bbox = req.get_param_value("bbox");
            std::stringstream ss(bbox);
            std::string sMinLon, sMinLat, sMaxLon, sMaxLat;
            if (std::getline(ss, sMinLon, ',') && std::getline(ss, sMinLat, ',') &&
                std::getline(ss, sMaxLon, ',') && std::getline(ss, sMaxLat, ',')) {
                try {
                    criteria.min_lon = std::stod(sMinLon);
                    criteria.min_lat = std::stod(sMinLat);
                    criteria.max_lon = std::stod(sMaxLon);
                    criteria.max_lat = std::stod(sMaxLat);
                } catch (...) {}
            }
        }

        Result<core::QueryResult> queryRes = catalog_
            ? catalog_->query(criteria)
            : core::QueryBuilder::execute(db(), criteria);

        if (!queryRes.isOk()) {
            sendError(res, queryRes.status().message(), 500);
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
            sendError(res, "Media item not found", 404);
            return;
        }

        sendJson(res, mediaRes.value());
    });

    // POST /api/media/:id/rating
    server.Post(R"(/api/media/(\d+)/rating)", [this](const httplib::Request& req, httplib::Response& res) {
        MediaId id = std::stoll(req.matches[1]);
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("rating") || !body["rating"].is_number()) {
                sendError(res, "Missing or invalid rating field");
                return;
            }
            int32_t rating = body["rating"].get<int32_t>();
            if (rating < 0 || rating > 5) {
                sendError(res, "Rating must be between 0 and 5");
                return;
            }

            Status s = catalog_ ? catalog_->setRating(id, rating) : db().updateRating(id, rating);
            if (!s.isOk()) {
                sendError(res, s.message(), 500);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"id", id}, {"rating", rating}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/:id/flag
    server.Post(R"(/api/media/(\d+)/flag)", [this](const httplib::Request& req, httplib::Response& res) {
        MediaId id = std::stoll(req.matches[1]);
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("flag") || !body["flag"].is_number()) {
                sendError(res, "Missing or invalid flag field");
                return;
            }
            int8_t flagVal = body["flag"].get<int8_t>();
            if (flagVal < -1 || flagVal > 1) {
                sendError(res, "Flag must be -1, 0, or 1");
                return;
            }

            FlagState flag = static_cast<FlagState>(flagVal);
            Status s = catalog_ ? catalog_->setFlag(id, flag) : db().updateFlag(id, flag);
            if (!s.isOk()) {
                sendError(res, s.message(), 500);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"id", id}, {"flag", flagVal}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/media/:id/gps
    server.Post(R"(/api/media/(\d+)/gps)", [this](const httplib::Request& req, httplib::Response& res) {
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
                sendError(res, s.message(), 500);
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
        MediaId id = std::stoll(req.matches[1]);
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("name") || !body["name"].is_string()) {
                sendError(res, "Missing or invalid name field");
                return;
            }
            std::string name = body["name"].get<std::string>();
            std::string category = body.value("category", "keyword");

            Status s = catalog_
                ? catalog_->addTag(id, name, category)
                : [&]() {
                    auto tagRes = db().createOrGetTag(name, category);
                    if (!tagRes.isOk()) return tagRes.status();
                    return db().addTagToMedia(id, tagRes.value());
                }();

            if (!s.isOk()) {
                sendError(res, s.message(), 500);
                return;
            }

            sendJson(res, {{"status", "ok"}, {"media_id", id}, {"name", name}, {"category", category}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/media/:id/tags/:tag_id
    server.Delete(R"(/api/media/(\d+)/tags/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        MediaId id = std::stoll(req.matches[1]);
        TagId tagId = std::stoll(req.matches[2]);

        Status s = catalog_ ? catalog_->removeTag(id, tagId) : db().removeTagFromMedia(id, tagId);
        if (!s.isOk()) {
            sendError(res, s.message(), 500);
            return;
        }

        sendJson(res, {{"status", "ok"}});
    });

    // DELETE /api/media/:id
    server.Delete(R"(/api/media/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        MediaId id = std::stoll(req.matches[1]);
        auto mediaRes = catalog_ ? catalog_->getMedia(id) : db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendError(res, "Media item not found", 404);
            return;
        }

        Status s = catalog_ ? catalog_->deleteMedia(id) : db().deleteMedia(id);
        if (!s.isOk()) {
            sendError(res, s.message(), 500);
            return;
        }

        sendJson(res, {{"status", "ok"}, {"id", id}});
    });

    // POST /api/media/batch-delete
    server.Post("/api/media/batch-delete", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("ids") || !body["ids"].is_array()) {
                sendError(res, "Missing or invalid 'ids' array");
                return;
            }
            std::vector<MediaId> ids = body["ids"].get<std::vector<MediaId>>();
            int deletedCount = 0;
            for (MediaId id : ids) {
                Status s = catalog_ ? catalog_->deleteMedia(id) : db().deleteMedia(id);
                if (s.isOk()) {
                    deletedCount++;
                }
            }
            sendJson(res, {{"status", "ok"}, {"deleted_count", deletedCount}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });
}

void ApiRouter::registerThumbnailRoutes(httplib::Server& server) {
    // GET /api/thumbnails/:hash/:size
    server.Get(R"(/api/thumbnails/([a-zA-Z0-9_-]+)/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string hash = req.matches[1];
        int size = std::stoi(req.matches[2]);

        std::string etag = "\"" + hash + "_" + std::to_string(size) + "\"";
        res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        res.set_header("ETag", etag);

        if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag) {
            res.status = 304;
            return;
        }

        std::string thumbPath = cache().getThumbnailPath(hash, size);
        std::error_code ec;
        if (!std::filesystem::exists(thumbPath, ec)) {
            // Attempt on-demand generation
            auto mediaRes = db().getMediaByHash(hash);
            if (mediaRes.isOk()) {
                const auto& item = mediaRes.value();
                auto genRes = cache().ensureThumbnail(item.file_path, hash, size, item.exif.orientation);
                if (genRes.isOk()) {
                    thumbPath = genRes.value();
                }
            }
        }

        if (std::filesystem::exists(thumbPath, ec) && std::filesystem::is_regular_file(thumbPath, ec)) {
            res.set_file_content(thumbPath, "image/jpeg");
        } else {
            sendError(res, "Thumbnail not found", 404);
        }
    });

    // GET /api/photos/:id/original
    server.Get(R"(/api/photos/(\d+)/original)", [this](const httplib::Request& req, httplib::Response& res) {
        MediaId id = std::stoll(req.matches[1]);
        auto mediaRes = db().getMediaById(id);
        if (!mediaRes.isOk()) {
            sendError(res, "Media not found", 404);
            return;
        }

        const auto& item = mediaRes.value();
        std::error_code ec;
        if (!std::filesystem::exists(item.file_path, ec) || !std::filesystem::is_regular_file(item.file_path, ec)) {
            sendError(res, "File not found on disk: " + item.file_path, 404);
            return;
        }

        auto fsize = std::filesystem::file_size(item.file_path, ec);
        auto mtime = std::filesystem::last_write_time(item.file_path, ec);
        int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
        std::string etag = "\"" + std::to_string(id) + "-" + std::to_string(fsize) + "-" + std::to_string(mtimeSec) + "\"";

        res.set_header("Accept-Ranges", "bytes");
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "public, max-age=86400");

        if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == etag) {
            res.status = 304;
            return;
        }

        std::string mime = getMimeTypeForImage(item.file_path);
        res.set_file_content(item.file_path, mime);
    });
}

void ApiRouter::registerTagRoutes(httplib::Server& server) {
    // GET /api/tags
    server.Get("/api/tags", [this](const httplib::Request& req, httplib::Response& res) {
        auto tagsRes = catalog_ ? catalog_->getTags() : db().getAllTags();
        if (!tagsRes.isOk()) {
            sendError(res, tagsRes.status().message(), 500);
            return;
        }
        sendJson(res, tagsRes.value());
    });

    // POST /api/tags
    server.Post("/api/tags", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("name") || !body["name"].is_string()) {
                sendError(res, "Missing or invalid name");
                return;
            }
            std::string name = body["name"].get<std::string>();
            std::string category = body.value("category", "keyword");

            auto tagRes = catalog_
                ? catalog_->createOrGetTag(name, category)
                : db().createOrGetTag(name, category);

            if (!tagRes.isOk()) {
                sendError(res, tagRes.status().message(), 500);
                return;
            }

            sendJson(res, {{"id", tagRes.value()}, {"name", name}, {"category", category}}, 201);
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // DELETE /api/tags/:id
    server.Delete(R"(/api/tags/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        TagId tagId = std::stoll(req.matches[1]);
        Status s = catalog_ ? catalog_->deleteTag(tagId) : db().deleteTag(tagId);
        if (!s.isOk()) {
            sendError(res, s.message(), 500);
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
            sendError(res, albumsRes.status().message(), 500);
            return;
        }
        sendJson(res, albumsRes.value());
    });

    // POST /api/albums
    server.Post("/api/albums", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("name") || !body["name"].is_string()) {
                sendError(res, "Missing or invalid name");
                return;
            }
            std::string name = body["name"].get<std::string>();
            std::string desc = body.value("description", "");
            bool isSmart = body.value("is_smart", false);
            std::string queryJson = body.value("query_json", "");

            auto albumRes = catalog_
                ? catalog_->createAlbum(name, desc, isSmart, queryJson)
                : db().createAlbum(name, desc, isSmart, queryJson);

            if (!albumRes.isOk()) {
                sendError(res, albumRes.status().message(), 500);
                return;
            }

            sendJson(res, {{"id", albumRes.value()}, {"name", name}}, 201);
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // POST /api/albums/:id/media
    server.Post(R"(/api/albums/(\d+)/media)", [this](const httplib::Request& req, httplib::Response& res) {
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

            for (MediaId mid : mediaIds) {
                Status s = catalog_ ? catalog_->addMediaToAlbum(albumId, mid) : db().addMediaToAlbum(albumId, mid);
                if (!s.isOk()) {
                    sendError(res, s.message(), 500);
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
        AlbumId albumId = std::stoll(req.matches[1]);
        Status s = catalog_ ? catalog_->deleteAlbum(albumId) : db().deleteAlbum(albumId);
        if (!s.isOk()) {
            sendError(res, s.message(), 500);
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
            sendError(res, timelineRes.status().message(), 500);
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
            sendError(res, statsRes.status().message(), 500);
            return;
        }
        sendJson(res, statsRes.value());
    });
}

void ApiRouter::registerImportRoutes(httplib::Server& server) {
    // POST /api/import
    server.Post("/api/import", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body);
            if (!body.contains("path") || !body["path"].is_string()) {
                sendError(res, "Missing or invalid path");
                return;
            }
            std::string path = body["path"].get<std::string>();
            bool recursive = body.value("recursive", true);

            if (!std::filesystem::exists(path)) {
                sendError(res, "Directory does not exist: " + path, 404);
                return;
            }

            if (catalog_) {
                if (catalog_->importer().isRunning()) {
                    sendError(res, "An import is already running", 409);
                    return;
                }

                // Run import asynchronously in detached thread
                std::thread([this, path, recursive]() {
                    IMAGINE_LOG_INFO("Starting background import of: " + path);
                    auto res = catalog_->importDirectory(path, recursive);
                    if (res.isOk()) {
                        IMAGINE_LOG_INFO("Background import complete: " + std::to_string(res.value().imported_files.load()) + " imported");
                    } else {
                        IMAGINE_LOG_ERROR("Background import error: " + res.status().message());
                    }
                }).detach();
            } else {
                if (g_isImporting.load()) {
                    sendError(res, "An import is already running", 409);
                    return;
                }
                g_isImporting.store(true);

                std::thread([this, path, recursive]() {
                    IMAGINE_LOG_INFO("Starting fallback background import: " + path);
                    core::Importer imp(db(), cache());
                    auto res = imp.importDirectory(path, recursive, [](const ImportProgress& p) {
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
                }).detach();
            }

            sendJson(res, {{"status", "started"}, {"path", path}});
        } catch (const std::exception& ex) {
            sendError(res, std::string("Invalid JSON: ") + ex.what());
        }
    });

    // GET /api/import/progress
    server.Get("/api/import/progress", [this](const httplib::Request& req, httplib::Response& res) {
        if (catalog_) {
            auto progress = catalog_->importer().currentProgress();
            sendJson(res, progress);
        } else {
            std::lock_guard<std::mutex> lock(g_importMutex);
            sendJson(res, g_fallbackProgress);
        }
    });
}

} // namespace imagine::server
